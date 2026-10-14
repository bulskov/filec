#pragma once
/* Test fixture for the fs tests: a fresh temporary directory per test.
 *
 * Everything here uses the OS directly (mkdtemp, nftw, fopen, symlink /
 * CreateDirectoryW, FindFirstFileW, ...) and never the fs functions under
 * test — a broken fs_mkdir must not be able to break its own test setup.
 *
 * Include this header FIRST in a test file: on POSIX it asks for the
 * POSIX 2008 declarations (the build is strict C11, no extensions), plus
 * the Darwin ones on macOS.
 *
 *   fx_setup() / fx_teardown()   create / delete the directory (with all
 *                                its contents), from ctt_before_each and
 *                                ctt_after_each
 *   FX("a/b.txt")                canonical path of a/b.txt inside it
 *   fx_write_file(rel, text)     create a file
 *   fx_mkdir(rel)                create a directory
 *   fx_symlink(target, rel)      create a symlink; false where the OS does
 *                                not allow it (Windows without developer
 *                                mode) — tests then skip that part
 *   fx_set_hidden(rel)           Windows hidden attribute; false on POSIX */

#ifndef _WIN32
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE /* macOS hides mkdtemp under plain XOPEN */
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filec/native.h"
#include "seqc/string.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "heap_allocator.h"

static strbuf_t *fx_root; /* canonical path of this test's directory */

/* --- canonical <-> native, for the OS calls ------------------------------ */

/* The native, NUL-terminated form of the canonical path c, in a rotating
 * static buffer. */
static inline const char *fx_native(string_t c)
{
    static char bufs[4][8192];
    static int next;
    char *b = bufs[next++ % 4];
    strbuf_t *sb = strbuf_create(heap());
    if (!sb || path_to_native(c, PATH_STYLE_NATIVE, sb) != PATH_NATIVE_OK)
    {
        fprintf(
            stderr,
            "fx_native: cannot convert '%.*s'\n",
            (int)c.len,
            c.ptr ? c.ptr : "");
        abort();
    }
    string_t v = strbuf_view(sb);
    if (v.len >= sizeof bufs[0])
    {
        abort();
    }
    memcpy(b, v.ptr, v.len);
    b[v.len] = '\0';
    strbuf_destroy(sb);
    return b;
}

/* FX("a/b"): the canonical path root/a/b, as a string_t into a rotating
 * static buffer.  FX("") is the root itself. */
static inline string_t fx_path(const char *rel)
{
    static char bufs[8][8192];
    static int next;
    char *b = bufs[next++ % 8];
    string_t root = strbuf_view(fx_root);
    size_t n = (size_t)snprintf(
        b,
        sizeof bufs[0],
        "%.*s%s%s",
        (int)root.len,
        root.ptr,
        rel[0] ? "/" : "",
        rel);
    return (string_t){b, n};
}
#define FX(rel) fx_path(rel)

#ifdef _WIN32

/* UTF-8 (NUL-terminated) -> UTF-16 in a rotating static buffer. */
static inline const wchar_t *fx_wide(const char *utf8)
{
    static wchar_t bufs[4][8192];
    static int next;
    wchar_t *w = bufs[next++ % 4];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 8192))
    {
        abort();
    }
    return w;
}

static inline void fx_remove_tree(const wchar_t *dir)
{
    wchar_t pattern[8192];
    swprintf(pattern, 8192, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(fd.cFileName, L".") == 0
                || wcscmp(fd.cFileName, L"..") == 0)
            {
                continue;
            }
            wchar_t child[8192];
            swprintf(child, 8192, L"%ls\\%ls", dir, fd.cFileName);
            SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            bool is_dir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            bool is_link = fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
            if (is_dir && !is_link)
            {
                fx_remove_tree(child);
            }
            else if (is_dir)
            {
                RemoveDirectoryW(child); /* a directory link: not into it */
            }
            else
            {
                DeleteFileW(child);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static inline void fx_setup(void)
{
    wchar_t tmp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    if (n == 0)
    {
        abort();
    }
    static unsigned counter;
    wchar_t dir[8192];
    swprintf(
        dir,
        8192,
        L"%lsfilec_fs_test_%lu_%u",
        tmp,
        (unsigned long)GetCurrentProcessId(),
        counter++);
    if (!CreateDirectoryW(dir, NULL))
    {
        abort();
    }
    char utf8[8192];
    if (!WideCharToMultiByte(
            CP_UTF8, 0, dir, -1, utf8, sizeof utf8, NULL, NULL))
    {
        abort();
    }
    fx_root = strbuf_create(heap());
    if (path_from_native(string_view_cstr(utf8), PATH_STYLE_WINDOWS, fx_root)
        != PATH_NATIVE_OK)
    {
        abort();
    }
}

static inline void fx_teardown(void)
{
    fx_remove_tree(fx_wide(fx_native(strbuf_view(fx_root))));
    strbuf_destroy(fx_root);
    fx_root = NULL;
}

static inline bool fx_mkdir(const char *rel)
{
    return CreateDirectoryW(fx_wide(fx_native(FX(rel))), NULL);
}

static inline bool fx_symlink(const char *target, const char *rel)
{
    /* target relative to the link's directory, as POSIX symlink() takes */
    char t[8192];
    snprintf(t, sizeof t, "%s", target);
    for (char *p = t; *p; ++p)
    {
        if (*p == '/')
        {
            *p = '\\';
        }
    }
    DWORD flags = 0x2; /* SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE */
    return CreateSymbolicLinkW(fx_wide(fx_native(FX(rel))), fx_wide(t), flags);
}

static inline bool fx_set_hidden(const char *rel)
{
    return SetFileAttributesW(
        fx_wide(fx_native(FX(rel))), FILE_ATTRIBUTE_HIDDEN);
}

static inline FILE *fx_fopen(const char *native, const char *mode)
{
    FILE *f = NULL;
    return _wfopen_s(&f, fx_wide(native), fx_wide(mode)) == 0 ? f : NULL;
}

#else /* POSIX */

static inline int fx_remove_one(
    const char *p, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st;
    (void)type;
    (void)ftw;
    remove(p);
    return 0;
}

static inline void fx_setup(void)
{
    const char *tmp = getenv("TMPDIR");
    char dir[4096];
    snprintf(
        dir,
        sizeof dir,
        "%s/filec_fs_test_XXXXXX",
        tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(dir))
    {
        abort();
    }
    fx_root = strbuf_create(heap());
    if (path_from_native(string_view_cstr(dir), PATH_STYLE_POSIX, fx_root)
        != PATH_NATIVE_OK)
    {
        abort();
    }
}

static inline void fx_teardown(void)
{
    nftw(
        fx_native(strbuf_view(fx_root)),
        fx_remove_one,
        16,
        FTW_DEPTH | FTW_PHYS);
    strbuf_destroy(fx_root);
    fx_root = NULL;
}

static inline bool fx_mkdir(const char *rel)
{
    return mkdir(fx_native(FX(rel)), 0755) == 0;
}

static inline bool fx_symlink(const char *target, const char *rel)
{
    return symlink(target, fx_native(FX(rel))) == 0;
}

static inline bool fx_set_hidden(const char *rel)
{
    (void)rel;
    return false;
}

static inline FILE *fx_fopen(const char *native, const char *mode)
{
    return fopen(native, mode);
}

#endif

static inline bool fx_write_file(const char *rel, const char *text)
{
    FILE *f = fx_fopen(fx_native(FX(rel)), "wb");
    if (!f)
    {
        return false;
    }
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
