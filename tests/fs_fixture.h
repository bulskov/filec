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
 *   fx_symlink_dir(target, rel)  the same, for a target that is a directory
 *   fx_set_hidden(rel)           Windows hidden attribute; false on POSIX
 *   fx_read_file(rel, &len)      the contents of a file (static buffer,
 *                                up to 64 KiB), NULL if it cannot be read
 *   fx_count_entries(rel)        entries in a directory, without . and ..;
 *                                -1 if it cannot be read
 *   fx_write_bytes(rel, p, n)    create a file with n bytes of binary data
 *   fx_exists(rel), fx_is_dir    without following a link
 *   fx_set_mtime(rel, secs)      the modification time, seconds since 1970
 *   fx_mode(rel)                 POSIX permission bits; -1 on Windows */

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
#include <share.h> /* _SH_DENYNO */
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h> /* AT_FDCWD */
#include <ftw.h>
#include <sys/stat.h>
#include <time.h>
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
    /* Process ids are reused (under Wine almost always), so a directory a
     * crashed run left behind can have this name: take the next one. */
    static unsigned counter;
    wchar_t dir[8192];
    for (;;)
    {
        swprintf(
            dir,
            8192,
            L"%lsfilec_fs_test_%lu_%u",
            tmp,
            (unsigned long)GetCurrentProcessId(),
            counter++);
        if (CreateDirectoryW(dir, NULL))
        {
            break;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS || counter > 100000)
        {
            abort();
        }
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

static inline bool fx_symlink_flags(
    const char *target, const char *rel, DWORD flags)
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
    flags |= 0x2; /* SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE */
    return CreateSymbolicLinkW(fx_wide(fx_native(FX(rel))), fx_wide(t), flags);
}

static inline bool fx_symlink(const char *target, const char *rel)
{
    return fx_symlink_flags(target, rel, 0);
}

/* Windows fixes a link's kind when it is made: only a directory link can
 * be listed through. */
static inline bool fx_symlink_dir(const char *target, const char *rel)
{
    return fx_symlink_flags(
        target, rel, 0x1); /* SYMBOLIC_LINK_FLAG_DIRECTORY */
}

static inline bool fx_set_hidden(const char *rel)
{
    return SetFileAttributesW(
        fx_wide(fx_native(FX(rel))), FILE_ATTRIBUTE_HIDDEN);
}

static inline int fx_count_entries(const char *rel)
{
    wchar_t pattern[8192];
    swprintf(pattern, 8192, L"%ls\\*", fx_wide(fx_native(FX(rel))));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    int n = 0;
    do
    {
        if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
        {
            n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* _wfsopen with _SH_DENYNO: shared like fs_open's handles.  _wfopen_s
 * opens unshared and cannot read a file a test still has open for
 * writing (Wine does not enforce that; Windows does). */
static inline FILE *fx_fopen(const char *native, const char *mode)
{
    return _wfsopen(fx_wide(native), fx_wide(mode), _SH_DENYNO);
}

static inline DWORD fx_attrs(const char *rel)
{
    return GetFileAttributesW(fx_wide(fx_native(FX(rel))));
}

static inline bool fx_exists(const char *rel)
{
    return fx_attrs(rel) != INVALID_FILE_ATTRIBUTES;
}

static inline bool fx_is_dir(const char *rel)
{
    DWORD a = fx_attrs(rel);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static inline bool fx_set_mtime(const char *rel, long long secs)
{
    HANDLE h = CreateFileW(
        fx_wide(fx_native(FX(rel))),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    /* 100 ns ticks since 1601 */
    unsigned long long t =
        (unsigned long long)secs * 10000000ULL + 116444736000000000ULL;
    FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)};
    bool ok = SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    return ok;
}

static inline int fx_mode(const char *rel)
{
    (void)rel;
    return -1;
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

static inline bool fx_symlink_dir(const char *target, const char *rel)
{
    return fx_symlink(target, rel);
}

static inline bool fx_set_hidden(const char *rel)
{
    (void)rel;
    return false;
}

static inline int fx_count_entries(const char *rel)
{
    DIR *d = opendir(fx_native(FX(rel)));
    if (!d)
    {
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
        {
            n++;
        }
    }
    closedir(d);
    return n;
}

static inline FILE *fx_fopen(const char *native, const char *mode)
{
    return fopen(native, mode);
}

static inline bool fx_exists(const char *rel)
{
    struct stat st;
    return lstat(fx_native(FX(rel)), &st) == 0;
}

static inline bool fx_is_dir(const char *rel)
{
    struct stat st;
    return lstat(fx_native(FX(rel)), &st) == 0 && S_ISDIR(st.st_mode);
}

static inline bool fx_set_mtime(const char *rel, long long secs)
{
    struct timespec ts[2] = {{0, UTIME_OMIT}, {(time_t)secs, 0}};
    return utimensat(AT_FDCWD, fx_native(FX(rel)), ts, 0) == 0;
}

static inline int fx_mode(const char *rel)
{
    struct stat st;
    return stat(fx_native(FX(rel)), &st) == 0 ? (int)(st.st_mode & 07777) : -1;
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

static inline bool fx_write_bytes(const char *rel, const void *p, size_t n)
{
    FILE *f = fx_fopen(fx_native(FX(rel)), "wb");
    if (!f)
    {
        return false;
    }
    bool ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* The contents of a file, read with stdio; *len gets the length.  The
 * buffer is static (64 KiB) and NUL-terminated for convenience. */
static inline const char *fx_read_file(const char *rel, size_t *len)
{
    static char buf[64 * 1024 + 1];
    FILE *f = fx_fopen(fx_native(FX(rel)), "rb");
    if (!f)
    {
        return NULL;
    }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (len)
    {
        *len = n;
    }
    return buf;
}
