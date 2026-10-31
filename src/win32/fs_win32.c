/* filec/fs — Win32 backend (FindFirstFileExW, ReadDirectoryChangesW,
 * CopyFileExW, MoveFileExW, IFileOperation for the Recycle Bin).
 *
 * The principle is the same as the POSIX backend: validate the canonical
 * path, convert it to what the OS wants, make the call, map the OS error.
 * What differs is the conversion and the error codes:
 *
 *   - Paths: canonical "/C:/x" -> native "C:\x" (path_to_native) ->
 *     "\\?\C:\x" in UTF-16.  The "\\?\" prefix switches off Win32's own
 *     path parsing: no MAX_PATH (260) limit, no "." / ".." handling, no
 *     trailing-dot stripping — we hand over a normalised path exactly.
 *     Shares become "\\?\UNC\server\share\x".
 *   - Errors: GetLastError() codes, mapped in win_err.  A few calls need
 *     help to give the POSIX answer (see fs_mkdir and fs_remove).
 *   - Metadata: one handle opened for attributes only, then
 *     GetFileInformationByHandle — that gives kind, size, times, attributes
 *     and the file's identity in one go, and the same call follows links
 *     or not depending on FILE_FLAG_OPEN_REPARSE_POINT. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "filec/fs.h"
#include "filec/native.h"
#include "filec/path.h"
#include <stdatomic.h>

#include "../fs_internal.h"
#include "arena/fixed_arena.h"

#include <string.h>
#include <wchar.h>
#include <wctype.h>

/* --- Errors -------------------------------------------------------------- */

static fs_err_t win_err(DWORD code)
{
    fs_err_kind_t kind;
    switch (code)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_INVALID_DRIVE:
        kind = FS_ERR_NOT_FOUND;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        kind = FS_ERR_ACCESS;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        kind = FS_ERR_EXISTS;
        break;
    case ERROR_DIR_NOT_EMPTY:
        kind = FS_ERR_NOT_EMPTY;
        break;
    case ERROR_DIRECTORY: /* "The directory name is invalid" */
        kind = FS_ERR_NOT_DIR;
        break;
    case ERROR_NOT_SAME_DEVICE:
        kind = FS_ERR_CROSS_DEVICE;
        break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        kind = FS_ERR_NO_SPACE;
        break;
    case ERROR_FILENAME_EXCED_RANGE:
        kind = FS_ERR_NAME_TOO_LONG;
        break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_BUSY:
        kind = FS_ERR_BUSY;
        break;
    case ERROR_WRITE_PROTECT:
        kind = FS_ERR_READ_ONLY;
        break;
    case ERROR_CANT_RESOLVE_FILENAME: /* too many links */
        kind = FS_ERR_LOOP;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        kind = FS_ERR_OOM;
        break;
    case ERROR_NOT_SUPPORTED:
        kind = FS_ERR_UNSUPPORTED;
        break;
    default:
        kind = FS_ERR_IO;
        break;
    }
    return (fs_err_t){kind, (int32_t)code};
}

static fs_err_t last_err(void)
{
    return win_err(GetLastError());
}

static const fs_err_t ok = {FS_OK, 0};

/* --- Paths: canonical -> "\\?\" + native, in UTF-16 ---------------------- */

/* A canonical path of FS_PATH_MAX UTF-8 bytes is at most FS_PATH_MAX UTF-16
 * units (never more units than bytes), plus "\\?\UNC" and the NUL. */
#define WPATH_MAX (FS_PATH_MAX + 16)

/* Room for path_to_native's strbuf in a fixed arena: the strbuf and its
 * vec, and the buffer doubling up to twice the longest native path.  The
 * arena grows the last allocation in place, so nothing is wasted. */
#define NATIVE_ARENA (2 * FS_PATH_MAX + 256)

static fs_err_t to_wide(string_t path, wchar_t out[WPATH_MAX])
{
    /* The same checks as the POSIX backend: absolute, no NUL, length. */
    char check[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(path, check, sizeof check);
    if (!fs_ok(err))
    {
        return err;
    }

    /* canonical -> native ("C:\x", "\\srv\share\x"), no heap: the strbuf
     * lives in a fixed arena on the stack. */
    char mem[NATIVE_ARENA];
    fixed_arena_t arena;
    fixed_arena_init(&arena, mem, sizeof mem);
    strbuf_t *native = strbuf_create(fixed_arena_allocator(&arena));
    if (!native)
    {
        return (fs_err_t){FS_ERR_NAME_TOO_LONG, 0};
    }
    switch (path_to_native(path, PATH_STYLE_WINDOWS, native))
    {
    case PATH_NATIVE_OK:
        break;
    case PATH_NATIVE_NO_NATIVE_FORM: /* "/", "/home": not on Windows */
        return (fs_err_t){FS_ERR_NOT_FOUND, 0};
    case PATH_NATIVE_OOM: /* the arena is sized for FS_PATH_MAX */
        return (fs_err_t){FS_ERR_NAME_TOO_LONG, 0};
    default:
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    /* The long-path prefix: "\\?\C:\x", or "\\?\UNC\srv\share\x" for a
     * share — "\\srv" loses one of its backslashes to "\\?\UNC". */
    string_t n = strbuf_view(native);
    const wchar_t *prefix = L"\\\\?\\";
    size_t plen = 4;
    if (n.len >= 2 && n.ptr[0] == '\\' && n.ptr[1] == '\\')
    {
        prefix = L"\\\\?\\UNC";
        plen = 7;
        n = string_slice(n, 1, n.len);
    }
    wmemcpy(out, prefix, plen);

    int units = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        n.ptr,
        (int)n.len,
        out + plen,
        (int)(WPATH_MAX - plen - 1));
    if (units == 0)
    {
        DWORD code = GetLastError();
        if (code == ERROR_NO_UNICODE_TRANSLATION) /* not valid UTF-8 */
        {
            return (fs_err_t){FS_ERR_INVALID, (int32_t)code};
        }
        return win_err(code);
    }
    out[plen + (size_t)units] = L'\0';
    return ok;
}

/* --- Metadata ------------------------------------------------------------ */

/* FILETIME: 100 ns ticks since 1601-01-01.  Unix: ns since 1970-01-01. */
static int64_t unix_ns(FILETIME ft)
{
    const int64_t epoch_diff = 116444736000000000LL; /* 1601 -> 1970 */
    int64_t ticks =
        (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    return (ticks - epoch_diff) * 100;
}

/* A handle that can only read attributes — no read or write access, so it
 * opens even files others have open.  BACKUP_SEMANTICS is what lets
 * CreateFileW open a directory; OPEN_REPARSE_POINT opens a link itself
 * instead of its target. */
static HANDLE open_for_metadata(const wchar_t *w, bool follow)
{
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (!follow)
    {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    return CreateFileW(
        w,
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        flags,
        NULL);
}

/* A reparse point is a link only for these tags; others (OneDrive
 * placeholders, dedup, ...) are ordinary files and directories. */
static bool is_link_tag(DWORD tag)
{
    return tag == IO_REPARSE_TAG_SYMLINK
           || tag == IO_REPARSE_TAG_MOUNT_POINT; /* junction */
}

/* Whether the open handle h (with attributes attrs) is a link. */
static bool is_link(HANDLE h, DWORD attrs)
{
    if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag;
    if (!GetFileInformationByHandleEx(
            h, FileAttributeTagInfo, &tag, sizeof tag))
    {
        return false;
    }
    return is_link_tag(tag.ReparseTag);
}

/* Everything but the name and the identity, from what both
 * GetFileInformationByHandle (fs_stat) and FindFirstFileW (listings)
 * give.  One place, so fs_stat and a listing cannot disagree. */
static void entry_from_attrs(
    DWORD attrs,
    DWORD size_high,
    DWORD size_low,
    FILETIME mtime,
    FILETIME btime,
    bool link,
    fs_entry_t *out)
{
    bool dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->kind = link ? FS_KIND_LINK : dir ? FS_KIND_DIR : FS_KIND_FILE;
    out->size = dir ? 0 : ((uint64_t)size_high << 32) | size_low;
    out->mtime_ns = unix_ns(mtime);
    out->btime_ns = unix_ns(btime);

    /* Windows has no permission bits: synthesise them from the read-only
     * attribute, as the C runtime's stat() does. */
    bool readonly = (attrs & FILE_ATTRIBUTE_READONLY) != 0;
    out->mode = 0444u | (readonly ? 0u : 0222u) | (dir ? 0111u : 0u);

    if (attrs & FILE_ATTRIBUTE_HIDDEN)
    {
        out->flags |= FS_FLAG_HIDDEN;
    }
    if (readonly)
    {
        out->flags |= FS_FLAG_READONLY;
    }
    if (attrs & FILE_ATTRIBUTE_SYSTEM)
    {
        out->flags |= FS_FLAG_SYSTEM;
    }

    /* NTFS records the birth time too */
    out->valid = FS_F_ALL & ~(uint32_t)FS_F_ID;
}

fs_err_t fs_stat(string_t path, uint32_t want, uint32_t flags, fs_entry_t *out)
{
    (void)want; /* one call delivers every field, as on POSIX */
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    wchar_t w[WPATH_MAX];
    fs_err_t err = to_wide(path, w);
    if (!fs_ok(err))
    {
        return err;
    }

    bool follow = !(flags & FS_NOFOLLOW);
    HANDLE h = open_for_metadata(w, follow);
    if (h == INVALID_HANDLE_VALUE)
    {
        return last_err();
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info))
    {
        err = last_err();
        CloseHandle(h);
        return err;
    }
    DWORD attrs = info.dwFileAttributes;
    bool link = !follow && is_link(h, attrs);
    CloseHandle(h);

    memset(out, 0, sizeof *out);
    entry_from_attrs(
        attrs,
        info.nFileSizeHigh,
        info.nFileSizeLow,
        info.ftLastWriteTime,
        info.ftCreationTime,
        link,
        out);
    out->name = path_file_name(path);

    /* (volume serial, file index) identify a file, like (dev, ino). */
    out->dev = info.dwVolumeSerialNumber;
    out->ino = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    out->valid |= FS_F_ID;
    return ok;
}

bool fs_exists(string_t path)
{
    /* Following links — GetFileAttributesW would not. */
    fs_entry_t e;
    return fs_ok(fs_stat(path, FS_F_KIND, 0, &e));
}

/* --- Changing the filesystem --------------------------------------------- */

/* The attributes of a path without following links, or
 * INVALID_FILE_ATTRIBUTES. */
static DWORD attributes_of(string_t path)
{
    wchar_t w[WPATH_MAX];
    if (!fs_ok(to_wide(path, w)))
    {
        return INVALID_FILE_ATTRIBUTES;
    }
    return GetFileAttributesW(w);
}

fs_err_t fs_mkdir(string_t path)
{
    wchar_t w[WPATH_MAX];
    fs_err_t err = to_wide(path, w);
    if (!fs_ok(err))
    {
        return err;
    }
    if (CreateDirectoryW(w, NULL))
    {
        return ok;
    }
    err = last_err();

    /* POSIX says ENOTDIR when the parent is a file; Windows only says
     * "path not found".  Look at the parent to give the same answer. */
    if (err.os_code == ERROR_PATH_NOT_FOUND)
    {
        DWORD parent = attributes_of(path_parent(path));
        if (parent != INVALID_FILE_ATTRIBUTES
            && !(parent & FILE_ATTRIBUTE_DIRECTORY))
        {
            err.kind = FS_ERR_NOT_DIR;
        }
    }
    return err;
}

fs_err_t fs_remove(string_t path)
{
    wchar_t w[WPATH_MAX];
    fs_err_t err = to_wide(path, w);
    if (!fs_ok(err))
    {
        return err;
    }

    /* Windows has one call per kind.  GetFileAttributesW does not follow
     * links, and a link to a directory is itself a directory entry, so
     * RemoveDirectoryW removes the link — never what it points to. */
    DWORD attrs = GetFileAttributesW(w);
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        return last_err();
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
    {
        return RemoveDirectoryW(w) ? ok : last_err();
    }
    if (DeleteFileW(w))
    {
        return ok;
    }

    /* POSIX unlinks a read-only file (the directory decides); Windows
     * refuses.  Clear the attribute and try once more. */
    err = last_err();
    if (err.os_code == ERROR_ACCESS_DENIED && (attrs & FILE_ATTRIBUTE_READONLY)
        && SetFileAttributesW(w, attrs & ~(DWORD)FILE_ATTRIBUTE_READONLY))
    {
        if (DeleteFileW(w))
        {
            return ok;
        }
        err = last_err();
        SetFileAttributesW(w, attrs); /* put it back */
    }
    return err;
}

/* --- Files --------------------------------------------------------------- */

/* The POSIX backend's pieces, one for one:
 *
 *   open(flags, 0666) + O_CLOEXEC   CreateFileW(access, share, disposition);
 *                                   handles are not inherited unless the
 *                                   SECURITY_ATTRIBUTES say so (NULL here)
 *   fstat + S_ISDIR                 a directory refuses to open without
 *                                   FILE_FLAG_BACKUP_SEMANTICS, with
 *                                   "access denied" — look at it to say IS_DIR
 *   pread / pwrite                  ReadFile / WriteFile, the offset in an
 *                                   OVERLAPPED (no seek, no shared position)
 *   fstat st_size / fsync           GetFileSizeEx / FlushFileBuffers
 *   rename                          MoveFileExW(REPLACE_EXISTING)
 *
 * Windows locks files by default (share mode 0); sharing everything gives
 * the POSIX behaviour: others may read, write, rename and delete. */

static HANDLE win_handle(fs_file_t f)
{
    return (HANDLE)f.h;
}

#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)

/* The most ReadFile / WriteFile take at once (a DWORD count); the loops
 * cut larger requests into pieces of this size. */
#define IO_CHUNK ((DWORD)1 << 30)

static OVERLAPPED at_offset(uint64_t offset)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = (DWORD)(offset >> 32);
    return ov;
}

/* Empties an open file: the end of file moves to 0. */
static bool truncate_handle(HANDLE h)
{
    FILE_END_OF_FILE_INFO eof;
    eof.EndOfFile.QuadPart = 0;
    return SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof eof);
}

fs_err_t fs_open(string_t path, uint32_t mode, fs_file_t *out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    *out = FS_FILE_NONE;

    wchar_t w[WPATH_MAX];
    fs_err_t err = to_wide(path, w);
    if (!fs_ok(err))
    {
        return err;
    }

    bool wr = mode & FS_WRITE;
    if (!(mode & (FS_READ | FS_WRITE))
        || ((mode & (FS_CREATE | FS_TRUNC)) && !wr)
        || ((mode & FS_EXCL) && !(mode & FS_CREATE)))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    DWORD access = 0;
    if (mode & FS_READ)
    {
        access |= GENERIC_READ;
    }
    if (wr)
    {
        access |= GENERIC_WRITE;
    }

    /* O_CREAT / O_EXCL map onto a "disposition".  O_TRUNC is done by hand
     * after the open: CREATE_ALWAYS and TRUNCATE_EXISTING refuse a hidden
     * or system file ("access denied"), and CREATE_ALWAYS also resets its
     * attributes — POSIX's O_TRUNC only empties the file. */
    DWORD disposition = OPEN_EXISTING;
    if ((mode & FS_CREATE) && (mode & FS_EXCL))
    {
        disposition = CREATE_NEW;
    }
    else if (mode & FS_CREATE)
    {
        disposition = OPEN_ALWAYS;
    }

    HANDLE h = CreateFileW(
        w, access, SHARE_ALL, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        err = last_err();
        if (err.os_code == ERROR_ACCESS_DENIED)
        {
            DWORD attrs = GetFileAttributesW(w);
            if (attrs != INVALID_FILE_ATTRIBUTES
                && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            {
                err.kind = FS_ERR_IS_DIR;
            }
        }
        return err;
    }

    if ((mode & FS_TRUNC) && !truncate_handle(h))
    {
        err = last_err();
        CloseHandle(h);
        return err;
    }

    out->h = (intptr_t)h;
    return ok;
}

fs_err_t fs_read_at(
    fs_file_t f, uint64_t offset, void *dst, size_t n, size_t *got)
{
    if (!got)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    *got = 0;
    if (!fs_file_is_open(f) || (!dst && n > 0) || offset > INT64_MAX)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    char *p = dst;
    while (*got < n)
    {
        size_t left = n - *got;
        DWORD want = left > IO_CHUNK ? IO_CHUNK : (DWORD)left;
        OVERLAPPED ov = at_offset(offset + *got);
        DWORD r = 0;
        if (!ReadFile(win_handle(f), p + *got, want, &r, &ov))
        {
            /* With an OVERLAPPED on a synchronous handle, reading at the
             * end of the file is this "error" rather than 0 bytes. */
            if (GetLastError() == ERROR_HANDLE_EOF)
            {
                break;
            }
            return last_err();
        }
        if (r == 0)
        {
            break; /* end of file */
        }
        *got += r;
    }
    return ok;
}

fs_err_t fs_write_at(fs_file_t f, uint64_t offset, const void *src, size_t n)
{
    if (!fs_file_is_open(f) || (!src && n > 0) || offset > INT64_MAX)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    const char *p = src;
    size_t written = 0;
    while (written < n)
    {
        size_t left = n - written;
        DWORD want = left > IO_CHUNK ? IO_CHUNK : (DWORD)left;
        OVERLAPPED ov = at_offset(offset + written);
        DWORD r = 0;
        if (!WriteFile(win_handle(f), p + written, want, &r, &ov))
        {
            return last_err();
        }
        if (r == 0)
        {
            return (fs_err_t){FS_ERR_IO, 0};
        }
        written += r;
    }
    return ok;
}

fs_err_t fs_file_size(fs_file_t f, uint64_t *size)
{
    if (!fs_file_is_open(f) || !size)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    LARGE_INTEGER s;
    if (!GetFileSizeEx(win_handle(f), &s))
    {
        return last_err();
    }
    *size = (uint64_t)s.QuadPart;
    return ok;
}

fs_err_t fs_sync(fs_file_t f)
{
    if (!fs_file_is_open(f))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    return FlushFileBuffers(win_handle(f)) ? ok : last_err();
}

void fs_close(fs_file_t f)
{
    if (fs_file_is_open(f))
    {
        CloseHandle(win_handle(f));
    }
}

/* The temporary file's name: path + ".tmp<pid>.<n>", as on POSIX. */
#define WTMP_MAX (WPATH_MAX + 32)

fs_err_t fs_write_atomic(string_t path, string_t data)
{
    wchar_t w[WPATH_MAX];
    fs_err_t err = to_wide(path, w);
    if (!fs_ok(err))
    {
        return err;
    }

    /* POSIX's rename says EISDIR for a directory; MoveFileExW only says
     * "access denied".  Look first.  The hidden and system attributes are
     * Windows' part of what fchmod keeps on POSIX: the new file gets them
     * too, or replacing a hidden file would make it visible.  (ReplaceFileW
     * keeps even more — ACLs, creation time — but it needs an existing
     * target and has its own odd failure modes.) */
    DWORD keep = 0;
    DWORD attrs = GetFileAttributesW(w);
    if (attrs != INVALID_FILE_ATTRIBUTES)
    {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        {
            return (fs_err_t){FS_ERR_IS_DIR, 0};
        }
        keep = attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    }

    wchar_t tmp[WTMP_MAX];
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 1;; i++)
    {
        if (i == 1000)
        {
            return (fs_err_t){FS_ERR_IO, 0};
        }
        swprintf(tmp, WTMP_MAX, L"%ls.tmp%lu.%03d", w, pid, i);
        /* CREATE_NEW is O_CREAT | O_EXCL: the check and the create in one
         * step. */
        h = CreateFileW(
            tmp,
            GENERIC_WRITE,
            SHARE_ALL,
            NULL,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | keep,
            NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            break;
        }
        DWORD code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS)
        {
            return win_err(code);
        }
    }

    fs_file_t f = {(intptr_t)h};
    err = fs_write_at(f, 0, data.ptr, data.len);
    if (fs_ok(err))
    {
        err = fs_sync(f);
    }
    fs_close(f);

    /* WRITE_THROUGH: return only when the rename is on disk — POSIX's
     * fsync of the directory, which the POSIX backend leaves out. */
    if (fs_ok(err)
        && !MoveFileExW(
            tmp, w, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        err = last_err();
    }
    if (!fs_ok(err))
    {
        DeleteFileW(tmp);
    }
    return err;
}

/* --- Reading directories ------------------------------------------------ */

/* The POSIX backend's pieces, one for one:
 *
 *   opendir / readdir / closedir   FindFirstFileExW / FindNextFileW /
 *                                  FindClose; the first entry comes with
 *                                  the open, so it is kept for the first
 *                                  filec_dir_next
 *   d_type, fstatat                WIN32_FIND_DATAW has kind (attributes,
 *                                  and the reparse tag in dwReserved0),
 *                                  size, times and attributes for free —
 *                                  only FS_F_ID needs the file opened
 *   d_name (bytes)                 cFileName (UTF-16), converted to UTF-8
 *                                  into the name buffer
 *
 * FindExInfoBasic leaves out the 8.3 short name nobody asked for, and
 * FIND_FIRST_EX_LARGE_FETCH fetches many entries per call, as getdents
 * does under readdir. */

/* A name is at most 255 UTF-16 units; each becomes at most 3 UTF-8 bytes
 * (a surrogate pair: 2 units, 4 bytes). */
#define NAME_UTF8_MAX (3 * 255 + 1)

struct filec_dir_t
{
    HANDLE find;
    allocator_t a;
    uint32_t want;
    bool pending; /* fd holds an entry not yet handed out (the first) */
    bool done;
    WIN32_FIND_DATAW fd;
    size_t dir_len;         /* units of dir, ending in a backslash */
    wchar_t dir[WPATH_MAX]; /* "\\?\C:\x\": for opening an entry (FS_F_ID) */
    char name[NAME_UTF8_MAX];
};

/* POSIX says ENOTDIR when the path, or a component on the way, is a file;
 * Windows says "not found" (or "the directory name is invalid").  Find the
 * nearest part of path that exists: a file there means NOT_DIR. */
static fs_err_t not_dir_on_the_way(string_t path, fs_err_t err)
{
    if (err.kind != FS_ERR_NOT_FOUND && err.kind != FS_ERR_NOT_DIR)
    {
        return err;
    }
    for (string_t p = path;;)
    {
        DWORD attrs = attributes_of(p);
        if (attrs != INVALID_FILE_ATTRIBUTES)
        {
            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                return err; /* a link (dangling?): not a file in the way */
            }
            if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
            {
                err.kind = FS_ERR_NOT_DIR;
            }
            else if (err.kind == FS_ERR_NOT_DIR && p.len != path.len)
            {
                err.kind = FS_ERR_NOT_FOUND; /* a directory, then nothing */
            }
            return err;
        }
        string_t parent = path_parent(p);
        if (parent.len == 0 || parent.len >= p.len)
        {
            return err;
        }
        p = parent;
    }
}

fs_err_t filec_dir_open(
    string_t path, uint32_t want, allocator_t a, filec_dir_t **out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    *out = NULL;

    wchar_t w[WPATH_MAX];
    fs_err_t err = to_wide(path, w);
    if (!fs_ok(err))
    {
        return err;
    }

    filec_dir_t *d = mem_alloc(a, sizeof *d, _Alignof(filec_dir_t));
    if (!d)
    {
        return (fs_err_t){FS_ERR_OOM, 0};
    }
    memset(d, 0, offsetof(filec_dir_t, fd)); /* the small fields */
    d->a = a;
    d->want = want;

    /* dir = w with a trailing backslash ("\\?\C:\" has one already), and
     * the search pattern dir + "*" after it. */
    size_t n = wcslen(w);
    if (n + 2 >= WPATH_MAX)
    {
        mem_free(a, d, sizeof *d);
        return (fs_err_t){FS_ERR_NAME_TOO_LONG, 0};
    }
    wmemcpy(d->dir, w, n);
    if (d->dir[n - 1] != L'\\')
    {
        d->dir[n++] = L'\\';
    }
    d->dir_len = n;
    d->dir[n] = L'*';
    d->dir[n + 1] = L'\0';

    d->find = FindFirstFileExW(
        d->dir,
        FindExInfoBasic,
        &d->fd,
        FindExSearchNameMatch,
        NULL,
        FIND_FIRST_EX_LARGE_FETCH);
    d->dir[n] = L'\0';
    if (d->find == INVALID_HANDLE_VALUE)
    {
        DWORD code = GetLastError();
        DWORD attrs = GetFileAttributesW(w);
        if (code == ERROR_FILE_NOT_FOUND && attrs != INVALID_FILE_ATTRIBUTES
            && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            d->done = true; /* a directory without even "." (a drive root) */
            *out = d;
            return ok;
        }
        mem_free(a, d, sizeof *d);
        return not_dir_on_the_way(path, win_err(code));
    }
    d->pending = true;
    *out = d;
    return ok;
}

/* The identity of dir\name, which the find data does not have: open it
 * (attributes only, not following a link) and ask. */
static fs_err_t entry_id(filec_dir_t *d, const wchar_t *name, fs_entry_t *e)
{
    size_t len = wcslen(name);
    if (d->dir_len + len + 1 > WPATH_MAX)
    {
        return (fs_err_t){FS_ERR_NAME_TOO_LONG, 0};
    }
    wchar_t full[WPATH_MAX];
    wmemcpy(full, d->dir, d->dir_len);
    wmemcpy(full + d->dir_len, name, len + 1);

    HANDLE h = open_for_metadata(full, false);
    if (h == INVALID_HANDLE_VALUE)
    {
        return last_err();
    }
    BY_HANDLE_FILE_INFORMATION info;
    fs_err_t err = ok;
    if (GetFileInformationByHandle(h, &info))
    {
        e->dev = info.dwVolumeSerialNumber;
        e->ino = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
        e->valid |= FS_F_ID;
    }
    else
    {
        err = last_err();
    }
    CloseHandle(h);
    return err;
}

fs_err_t filec_dir_next(filec_dir_t *d, fs_entry_t *e, bool *done)
{
    if (!d || !e || !done)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    for (;;)
    {
        if (d->done)
        {
            *done = true;
            return ok;
        }
        if (d->pending)
        {
            d->pending = false;
        }
        else if (!FindNextFileW(d->find, &d->fd))
        {
            DWORD code = GetLastError();
            if (code == ERROR_NO_MORE_FILES)
            {
                d->done = true;
                continue;
            }
            return win_err(code);
        }

        const wchar_t *wn = d->fd.cFileName;
        if (wn[0] == L'.'
            && (wn[1] == L'\0' || (wn[1] == L'.' && wn[2] == L'\0')))
        {
            continue; /* skip "." and ".." */
        }

        /* UTF-16 -> UTF-8.  A lone surrogate (legal in NTFS names, not in
         * UTF-16) becomes U+FFFD: such a name can be shown, not reopened. */
        int bytes = WideCharToMultiByte(
            CP_UTF8, 0, wn, -1, d->name, (int)sizeof d->name, NULL, NULL);
        if (bytes <= 0)
        {
            return last_err();
        }

        DWORD attrs = d->fd.dwFileAttributes;
        bool link = (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
                    && is_link_tag(d->fd.dwReserved0);
        memset(e, 0, sizeof *e);
        entry_from_attrs(
            attrs,
            d->fd.nFileSizeHigh,
            d->fd.nFileSizeLow,
            d->fd.ftLastWriteTime,
            d->fd.ftCreationTime,
            link,
            e);
        e->name = (string_t){d->name, (size_t)bytes - 1};

        if (d->want & FS_F_ID)
        {
            fs_err_t err = entry_id(d, wn, e);
            if (err.kind == FS_ERR_NOT_FOUND)
            {
                continue; /* removed since it was listed: skip it */
            }
            if (!fs_ok(err))
            {
                return err;
            }
        }
        *done = false;
        return ok;
    }
}

void filec_dir_close(filec_dir_t *d)
{
    if (d)
    {
        if (d->find && d->find != INVALID_HANDLE_VALUE)
        {
            FindClose(d->find);
        }
        mem_free(d->a, d, sizeof *d);
    }
}

/* --- Renaming, copying, links (step 4, tests/fs_ops_test.c) ------------
 *
 * The POSIX backend's pieces, one for one:
 *
 *   lstat (dev, ino)                 volume serial + file index of a handle
 *                                    opened for attributes (same_file)
 *   rename / renameat2(NOREPLACE)    MoveFileExW with / without
 *                                    MOVEFILE_REPLACE_EXISTING — the
 *                                    "fail if it exists" is atomic already
 *   copy_file_range + progress loop  CopyFileExW: the copy, its chunks and
 *                                    its progress callback are the OS's; the
 *                                    callback returns PROGRESS_CANCEL
 *   futimens / fchmod                CopyFileExW keeps attributes and times
 *                                    itself; without FS_KEEP_METADATA the
 *                                    copy's mtime is set to now
 *   readlink                         the reparse point's data
 *                                    (FSCTL_GET_REPARSE_POINT)
 *   realpath                         GetFinalPathNameByHandleW, which also
 *                                    turns 8.3 names into long ones */

#include <winioctl.h>

/* The identity of what w names (following links or not): volume serial
 * and file index, like (dev, ino). */
static bool file_id(const wchar_t *w, bool follow, uint64_t id[2])
{
    HANDLE h = open_for_metadata(w, follow);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info;
    bool got = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (got)
    {
        id[0] = info.dwVolumeSerialNumber;
        id[1] = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    }
    return got;
}

static bool same_file(const wchar_t *a, const wchar_t *b, bool follow)
{
    uint64_t ia[2], ib[2];
    return file_id(a, follow, ia) && file_id(b, follow, ib) && ia[0] == ib[0]
           && ia[1] == ib[1];
}

/* to is inside from: "d" -> "d/sub/d".  Canonical paths make it a prefix
 * test. */
static bool is_inside(string_t to, string_t from)
{
    return to.len > from.len && to.ptr[from.len] == '/'
           && path_starts_with(to, from, PATH_CASE_NATIVE);
}

fs_err_t fs_rename(string_t from, string_t to, uint32_t flags)
{
    wchar_t wf[WPATH_MAX], wt[WPATH_MAX];
    fs_err_t err = to_wide(from, wf);
    if (!fs_ok(err))
    {
        return err;
    }
    err = to_wide(to, wt);
    if (!fs_ok(err))
    {
        return err;
    }
    if (is_inside(to, from))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    if (GetFileAttributesW(wf) == INVALID_FILE_ATTRIBUTES)
    {
        return not_dir_on_the_way(from, last_err());
    }

    DWORD to_attrs = GetFileAttributesW(wt);
    if (to_attrs != INVALID_FILE_ATTRIBUTES)
    {
        if (same_file(wf, wt, false))
        {
            if (wcscmp(wf, wt) == 0)
            {
                return ok; /* to itself: nothing to do */
            }
            /* "abc" -> "ABC": the same entry, its case changes.  (Otherwise
             * two hard links: to is in the way like any file.) */
            if (path_equals(from, to, PATH_CASE_INSENSITIVE))
            {
                return MoveFileExW(wf, wt, 0) ? ok : last_err();
            }
        }
        if (to_attrs & FILE_ATTRIBUTE_DIRECTORY)
        {
            return (fs_err_t){
                (flags & FS_REPLACE) ? FS_ERR_IS_DIR : FS_ERR_EXISTS, 0};
        }
        if (!(flags & FS_REPLACE))
        {
            return (fs_err_t){FS_ERR_EXISTS, 0};
        }
    }
    else
    {
        /* A file on the way to to: Windows answers with codes that do not
         * say so (and differ between versions); look instead. */
        fs_err_t way = not_dir_on_the_way(to, (fs_err_t){FS_ERR_NOT_FOUND, 0});
        if (way.kind == FS_ERR_NOT_DIR)
        {
            return way;
        }
    }

    /* No MOVEFILE_COPY_ALLOWED: across volumes is FS_ERR_CROSS_DEVICE. */
    DWORD mf = (flags & FS_REPLACE) ? MOVEFILE_REPLACE_EXISTING : 0;
    if (!MoveFileExW(wf, wt, mf))
    {
        return not_dir_on_the_way(to, last_err());
    }
    return ok;
}

static bool cancelled(const fs_op_t *op)
{
    return op && op->cancel && atomic_load(op->cancel);
}

/* CopyFileExW's progress callback: report, and stop when cancel is set. */
static DWORD CALLBACK copy_progress(
    LARGE_INTEGER total,
    LARGE_INTEGER done,
    LARGE_INTEGER stream_size,
    LARGE_INTEGER stream_done,
    DWORD stream,
    DWORD reason,
    HANDLE src,
    HANDLE dst,
    LPVOID user)
{
    (void)stream_size;
    (void)stream_done;
    (void)stream;
    (void)reason;
    (void)src;
    (void)dst;
    const fs_op_t *op = user;
    if (op && op->progress)
    {
        op->progress(
            op->user, (uint64_t)done.QuadPart, (uint64_t)total.QuadPart);
    }
    return cancelled(op) ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

/* The modification time of the file at w: now. */
static fs_err_t touch_mtime(const wchar_t *w)
{
    HANDLE h = CreateFileW(
        w,
        FILE_WRITE_ATTRIBUTES,
        SHARE_ALL,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        return last_err();
    }
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    fs_err_t err = SetFileTime(h, NULL, NULL, &now) ? ok : last_err();
    CloseHandle(h);
    return err;
}

/* Deletes w, read-only or not (CopyFileExW copies the attribute). */
static void delete_file(const wchar_t *w)
{
    SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(w);
}

fs_err_t fs_copy_file(
    string_t from, string_t to, uint32_t flags, const fs_op_t *op)
{
    wchar_t wf[WPATH_MAX], wt[WPATH_MAX];
    fs_err_t err = to_wide(from, wf);
    if (!fs_ok(err))
    {
        return err;
    }
    err = to_wide(to, wt);
    if (!fs_ok(err))
    {
        return err;
    }

    /* from, followed: a directory is IS_DIR, a dangling link NOT_FOUND. */
    fs_entry_t src;
    err = fs_stat(from, FS_F_KIND, 0, &src);
    if (!fs_ok(err))
    {
        return err;
    }
    if (src.kind == FS_KIND_DIR)
    {
        return (fs_err_t){FS_ERR_IS_DIR, 0};
    }

    /* What is at to: the same file (copying would empty it), a directory,
     * or anything at all without FS_REPLACE. */
    DWORD to_attrs = GetFileAttributesW(wt);
    if (to_attrs != INVALID_FILE_ATTRIBUTES)
    {
        if (same_file(wf, wt, true))
        {
            return (fs_err_t){FS_ERR_INVALID, 0};
        }
        if (to_attrs & FILE_ATTRIBUTE_DIRECTORY)
        {
            return (fs_err_t){FS_ERR_IS_DIR, 0};
        }
        if (!(flags & FS_REPLACE))
        {
            return (fs_err_t){FS_ERR_EXISTS, 0};
        }
    }
    if (cancelled(op))
    {
        return (fs_err_t){FS_ERR_CANCELLED, 0};
    }

    /* The copy is written beside to and moved into place at the end, so
     * an old file at to survives a cancel or an error. */
    wchar_t tmp[WTMP_MAX];
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    for (int i = 1;; i++)
    {
        if (i == 1000)
        {
            return (fs_err_t){FS_ERR_IO, 0};
        }
        swprintf(tmp, WTMP_MAX, L"%ls.tmp%lu.%03d", wt, pid, i);
        if (CopyFileExW(
                wf,
                tmp,
                copy_progress,
                (LPVOID)op,
                NULL,
                COPY_FILE_FAIL_IF_EXISTS))
        {
            break;
        }
        DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS)
        {
            continue;
        }
        DeleteFileW(tmp); /* CopyFileExW removes it; be sure */
        if (code == ERROR_REQUEST_ABORTED)
        {
            return (fs_err_t){FS_ERR_CANCELLED, (int32_t)code};
        }
        return not_dir_on_the_way(to, win_err(code));
    }

    if (!(flags & FS_KEEP_METADATA))
    {
        err = touch_mtime(tmp); /* CopyFileExW kept the source's */
    }
    if (fs_ok(err))
    {
        DWORD mf = MOVEFILE_WRITE_THROUGH;
        if (flags & FS_REPLACE)
        {
            mf |= MOVEFILE_REPLACE_EXISTING;
        }
        if (!MoveFileExW(tmp, wt, mf))
        {
            err = last_err();
        }
    }
    if (!fs_ok(err))
    {
        delete_file(tmp);
    }
    return err;
}

/* The reparse data of a symlink or junction (ntifs.h's
 * REPARSE_DATA_BUFFER, which the SDK headers do not all have). */
typedef struct
{
    ULONG tag;
    USHORT data_len;
    USHORT reserved;
    USHORT subst_off, subst_len, print_off, print_len;
    union {
        struct
        {
            ULONG flags; /* 1: relative */
            WCHAR path[1];
        } symlink;
        struct
        {
            WCHAR path[1];
        } junction;
    } u;
} filec_reparse_t;

#define REPARSE_MAX (16 * 1024) /* MAXIMUM_REPARSE_DATA_BUFFER_SIZE */
#define SYMLINK_RELATIVE 1u

/* UTF-16 units -> UTF-8, appended to out ('\' becomes '/' when slashes). */
static fs_err_t append_utf8(
    const wchar_t *w, size_t n, bool slashes, strbuf_t *out)
{
    char buf[3 * WPATH_MAX];
    int bytes = n ? WideCharToMultiByte(
                        CP_UTF8, 0, w, (int)n, buf, (int)sizeof buf, NULL, NULL)
                  : 0;
    if (n && bytes <= 0)
    {
        return last_err();
    }
    for (int i = 0; slashes && i < bytes; i++)
    {
        if (buf[i] == '\\')
        {
            buf[i] = '/';
        }
    }
    return strbuf_append(out, (string_t){buf, (size_t)bytes}) == SEQC_OK
               ? ok
               : (fs_err_t){FS_ERR_OOM, 0};
}

/* An NT path ("\??\C:\x", "\??\UNC\srv\share\x") or a "\\?\" one -> a
 * Win32 one ("C:\x", "\\srv\share\x"), as UTF-8 -> canonical, appended. */
static fs_err_t append_canonical(const wchar_t *w, size_t n, strbuf_t *out)
{
    char buf[3 * WPATH_MAX + 2];
    size_t at = 0;
    if (n >= 8
        && (wcsncmp(w, L"\\??\\UNC\\", 8) == 0
            || wcsncmp(w, L"\\\\?\\UNC\\", 8) == 0))
    {
        buf[at++] = '\\';
        buf[at++] = '\\';
        w += 8;
        n -= 8;
    }
    else if (
        n >= 4
        && (wcsncmp(w, L"\\??\\", 4) == 0 || wcsncmp(w, L"\\\\?\\", 4) == 0))
    {
        w += 4;
        n -= 4;
    }
    int bytes = n ? WideCharToMultiByte(
                        CP_UTF8,
                        0,
                        w,
                        (int)n,
                        buf + at,
                        (int)(sizeof buf - at),
                        NULL,
                        NULL)
                  : 0;
    if (n && bytes <= 0)
    {
        return last_err();
    }
    string_t native = {buf, at + (size_t)bytes};
    switch (path_from_native(native, PATH_STYLE_WINDOWS, out))
    {
    case PATH_NATIVE_OK:
        return ok;
    case PATH_NATIVE_OOM:
        return (fs_err_t){FS_ERR_OOM, 0};
    default:
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
}

fs_err_t fs_read_link(string_t path, strbuf_t *out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    wchar_t w[WPATH_MAX];
    fs_err_t err = to_wide(path, w);
    if (!fs_ok(err))
    {
        return err;
    }
    HANDLE h = open_for_metadata(w, false);
    if (h == INVALID_HANDLE_VALUE)
    {
        return not_dir_on_the_way(path, last_err());
    }
    _Alignas(ULONG) char data[REPARSE_MAX];
    DWORD got = 0;
    BOOL read = DeviceIoControl(
        h, FSCTL_GET_REPARSE_POINT, NULL, 0, data, sizeof data, &got, NULL);
    DWORD code = GetLastError();
    CloseHandle(h);
    if (!read)
    {
        return code == ERROR_NOT_A_REPARSE_POINT
                   ? (fs_err_t){FS_ERR_INVALID, (int32_t)code} /* no link */
                   : win_err(code);
    }

    const filec_reparse_t *r = (const filec_reparse_t *)data;
    const WCHAR *names;
    bool relative = false;
    if (r->tag == IO_REPARSE_TAG_SYMLINK)
    {
        names = r->u.symlink.path;
        relative = (r->u.symlink.flags & SYMLINK_RELATIVE) != 0;
    }
    else if (r->tag == IO_REPARSE_TAG_MOUNT_POINT)
    {
        names = r->u.junction.path;
    }
    else
    {
        return (fs_err_t){FS_ERR_INVALID, 0}; /* a reparse point, no link */
    }
    const wchar_t *subst = names + r->subst_off / sizeof(WCHAR);
    size_t subst_len = r->subst_len / sizeof(WCHAR);

    /* As stored: a relative target with '/', an absolute one canonical. */
    return relative ? append_utf8(subst, subst_len, true, out)
                    : append_canonical(subst, subst_len, out);
}

fs_err_t fs_real_path(string_t path, strbuf_t *out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    wchar_t w[WPATH_MAX];
    fs_err_t err = to_wide(path, w);
    if (!fs_ok(err))
    {
        return err;
    }
    HANDLE h = open_for_metadata(w, true);
    if (h == INVALID_HANDLE_VALUE)
    {
        return not_dir_on_the_way(path, last_err());
    }
    wchar_t final[WPATH_MAX];
    DWORD n = GetFinalPathNameByHandleW(
        h, final, WPATH_MAX, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    err = n == 0           ? last_err()
          : n >= WPATH_MAX ? (fs_err_t){FS_ERR_NAME_TOO_LONG, 0}
                           : ok;
    CloseHandle(h);
    return fs_ok(err) ? append_canonical(final, n, out) : err;
}

/* --- Places (step 5, tests/fs_places_test.c) -----------------------------
 *
 *   getcwd                 GetCurrentDirectoryW
 *   $HOME, XDG             Known Folders (SHGetKnownFolderPath): Profile,
 *                          RoamingAppData, LocalAppData
 *   $TMPDIR                GetTempPathW (TMP, TEMP, ...), long form
 *   cwd + "/" + path       GetFullPathNameW, which knows all of Windows'
 *                          relative forms ("C:x", "\x", "x\..\y") and its
 *                          rules (trailing dots and spaces) */

#include <shlobj.h>

/* The known folders, defined here: their symbols live in different
 * import libraries for MSVC and MinGW. */
static const GUID folder_profile = {
    0x5E6C858F,
    0x0E22,
    0x4760,
    {0x9A, 0xFE, 0xEA, 0x33, 0x17, 0xB6, 0x71, 0x73}};
static const GUID folder_roaming = {
    0x3EB685DB,
    0x65F9,
    0x4CF6,
    {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}};
static const GUID folder_local = {
    0xF1B32785,
    0x6FBA,
    0x4FCF,
    {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};

fs_err_t fs_cwd(strbuf_t *out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    wchar_t w[WPATH_MAX];
    DWORD n = GetCurrentDirectoryW(WPATH_MAX, w);
    if (n == 0)
    {
        return last_err();
    }
    if (n >= WPATH_MAX)
    {
        return (fs_err_t){FS_ERR_NAME_TOO_LONG, 0};
    }
    return append_canonical(w, n, out);
}

fs_err_t fs_location(fs_loc_t loc, strbuf_t *out)
{
    if (!out || (unsigned)loc > (unsigned)FS_LOC_STATE)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    if (loc == FS_LOC_TEMP)
    {
        wchar_t w[WPATH_MAX];
        DWORD n = GetTempPathW(WPATH_MAX, w);
        if (n == 0 || n >= WPATH_MAX)
        {
            return n ? (fs_err_t){FS_ERR_NAME_TOO_LONG, 0} : last_err();
        }
        /* GetTempPathW may give an 8.3 name ("RUNNER~1"): the long one. */
        DWORD m = GetLongPathNameW(w, w, WPATH_MAX);
        if (m > 0 && m < WPATH_MAX)
        {
            n = m;
        }
        return append_canonical(w, n, out);
    }

    /* config roams; cache, data and state stay on this machine */
    const GUID *id = loc == FS_LOC_HOME     ? &folder_profile
                     : loc == FS_LOC_CONFIG ? &folder_roaming
                                            : &folder_local;
    PWSTR p = NULL;
    HRESULT hr = SHGetKnownFolderPath(id, 0, NULL, &p);
    if (FAILED(hr))
    {
        CoTaskMemFree(p);
        return (fs_err_t){FS_ERR_NOT_FOUND, (int32_t)hr};
    }
    fs_err_t err = append_canonical(p, wcslen(p), out);
    CoTaskMemFree(p);
    return err;
}

fs_err_t fs_from_native(string_t native, strbuf_t *out)
{
    if (!out || native.len == 0 || memchr(native.ptr, '\0', native.len)
        || native.len >= WPATH_MAX)
    {
        return native.len >= WPATH_MAX ? (fs_err_t){FS_ERR_NAME_TOO_LONG, 0}
                                       : (fs_err_t){FS_ERR_INVALID, 0};
    }
    wchar_t w[WPATH_MAX];
    int units = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        native.ptr,
        (int)native.len,
        w,
        WPATH_MAX - 1);
    if (units <= 0)
    {
        return (fs_err_t){FS_ERR_INVALID, (int32_t)GetLastError()};
    }
    w[units] = L'\0';
    wchar_t full[WPATH_MAX];
    DWORD n = GetFullPathNameW(w, WPATH_MAX, full, NULL);
    if (n == 0)
    {
        return last_err();
    }
    if (n >= WPATH_MAX)
    {
        return (fs_err_t){FS_ERR_NAME_TOO_LONG, 0};
    }
    return append_canonical(full, n, out);
}

/* --- Volumes (step 6, tests/fs_volume_test.c) -----------------------------
 *
 *   mountinfo / getmntinfo   the drive letters (GetLogicalDriveStringsW)
 *   removable, network       GetDriveTypeW
 *   label, read-only         GetVolumeInformationW — which fails for a
 *                            drive without a medium (an empty card
 *                            reader): listed anyway, without a label
 *   statvfs                  GetDiskFreeSpaceExW */

/* UTF-16 -> UTF-8 copied into a. */
static bool dup_wide(allocator_t a, const wchar_t *w, string_t *out)
{
    char buf[3 * (MAX_PATH + 1)];
    int n = WideCharToMultiByte(
        CP_UTF8, 0, w, -1, buf, (int)sizeof buf, NULL, NULL);
    return n > 0 && filec_dup_string(a, buf, (size_t)n - 1, out);
}

static fs_err_t list_volumes(allocator_t a, vec_t *out)
{
    wchar_t drives[26 * 4 + 1];
    DWORD n = GetLogicalDriveStringsW(sizeof drives / sizeof drives[0], drives);
    if (n == 0 || n > sizeof drives / sizeof drives[0])
    {
        return n ? (fs_err_t){FS_ERR_IO, 0} : last_err();
    }
    /* no "insert a disk" dialog for an empty drive */
    DWORD old_mode;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    fs_err_t err = ok;
    for (const wchar_t *root = drives; *root && fs_ok(err);
         root += wcslen(root) + 1)
    {
        fs_volume_t v = {0};
        UINT type = GetDriveTypeW(root);
        v.removable = type == DRIVE_REMOVABLE || type == DRIVE_CDROM;
        v.network = type == DRIVE_REMOTE;

        wchar_t label[MAX_PATH + 1] = L"";
        DWORD flags = 0;
        if (!GetVolumeInformationW(
                root, label, MAX_PATH + 1, NULL, NULL, &flags, NULL, 0))
        {
            label[0] = L'\0';
        }
        v.read_only = (flags & FILE_READ_ONLY_VOLUME) != 0;

        char path[4] = {'/', (char)towupper(root[0]), ':', '\0'};
        if (!filec_dup_string(a, path, 3, &v.path)
            || !dup_wide(a, label, &v.label) || vec_push(out, &v) != SEQC_OK)
        {
            err = (fs_err_t){FS_ERR_OOM, 0};
        }
    }
    SetThreadErrorMode(old_mode, NULL);
    return err;
}

fs_err_t fs_volumes(allocator_t a, vec_t *out)
{
    if (!out || vec_elem_size(out) != sizeof(fs_volume_t))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    size_t mark = vec_len(out);
    fs_err_t err = list_volumes(a, out);
    if (!fs_ok(err))
    {
        vec_truncate(out, mark);
    }
    return err;
}

fs_err_t fs_space(string_t path, uint64_t *total, uint64_t *avail)
{
    wchar_t w[WPATH_MAX];
    fs_err_t err = to_wide(path, w);
    if (!fs_ok(err))
    {
        return err;
    }
    DWORD attrs = GetFileAttributesW(w);
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        return not_dir_on_the_way(path, last_err());
    }
    /* GetDiskFreeSpaceExW wants a directory, with a trailing backslash */
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        err = to_wide(path_parent(path), w);
        if (!fs_ok(err))
        {
            return err;
        }
    }
    size_t n = wcslen(w);
    if (w[n - 1] != L'\\' && n + 1 < WPATH_MAX)
    {
        w[n] = L'\\';
        w[n + 1] = L'\0';
    }
    ULARGE_INTEGER caller_avail, size;
    if (!GetDiskFreeSpaceExW(w, &caller_avail, &size, NULL))
    {
        return last_err();
    }
    if (total)
    {
        *total = size.QuadPart;
    }
    if (avail)
    {
        *avail = caller_avail.QuadPart;
    }
    return ok;
}

/* --- Watching (step 7, tests/fs_watch_test.c) -----------------------------
 *
 *   inotify watch            a directory handle with an overlapped
 *                            ReadDirectoryChangesW always pending into
 *                            the watch's buffer
 *   poll on the inotify fd   GetQueuedCompletionStatus on one completion
 *                            port that every directory handle is tied to
 *                            (no 64-handle limit, unlike
 *                            WaitForMultipleObjects)
 *   eventfd (wake)           PostQueuedCompletionStatus with key 0
 *
 * A watch's buffer belongs to the kernel while its read is pending, so a
 * removed watch is only freed once its cancelled read has come back
 * through the port — in fs_watch_wait, or in fs_watch_destroy. */

typedef struct dir_watch
{
    struct dir_watch *next;
    HANDLE dir;
    uint32_t id;
    bool removed; /* fs_watch_remove: hand out nothing more, free on return */
    bool pending; /* a read is with the kernel */
    OVERLAPPED ov;
    DWORD buf[16 * 1024]; /* 64 KiB, DWORD-aligned as the API wants */
} dir_watch_t;

struct fs_watch_t
{
    allocator_t a;
    HANDLE port;
    SRWLOCK lock; /* watches, next_id, removed, pending */
    dir_watch_t *watches;
    uint32_t next_id;
    /* the buffer being handed out, and how far */
    dir_watch_t *cur;
    DWORD cur_off, cur_len;
    char names[64 * 1024]; /* this call's names, as UTF-8 */
};

#define WATCH_FILTER                                                           \
    (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME                \
     | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE                 \
     | FILE_NOTIFY_CHANGE_ATTRIBUTES)

/* Gives the buffer to the kernel for the next changes. */
static bool issue_read(dir_watch_t *d)
{
    memset(&d->ov, 0, sizeof d->ov);
    d->pending = ReadDirectoryChangesW(
        d->dir, d->buf, sizeof d->buf, FALSE, WATCH_FILTER, NULL, &d->ov, NULL);
    return d->pending;
}

fs_err_t fs_watch_create(allocator_t a, fs_watch_t **out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    *out = NULL;
    fs_watch_t *w = mem_alloc(a, sizeof *w, _Alignof(fs_watch_t));
    if (!w)
    {
        return (fs_err_t){FS_ERR_OOM, 0};
    }
    memset(w, 0, offsetof(fs_watch_t, names));
    w->a = a;
    InitializeSRWLock(&w->lock);
    w->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!w->port)
    {
        fs_err_t err = last_err();
        mem_free(a, w, sizeof *w);
        return err;
    }
    *out = w;
    return ok;
}

fs_err_t fs_watch_add(fs_watch_t *w, string_t dir, uint32_t *id)
{
    if (!w || !id)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    wchar_t wd[WPATH_MAX];
    fs_err_t err = to_wide(dir, wd);
    if (!fs_ok(err))
    {
        return err;
    }
    HANDLE h = CreateFileW(
        wd,
        FILE_LIST_DIRECTORY,
        SHARE_ALL,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        return not_dir_on_the_way(dir, last_err());
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)
        || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        CloseHandle(h);
        return (fs_err_t){FS_ERR_NOT_DIR, 0};
    }

    dir_watch_t *d = mem_alloc(w->a, sizeof *d, _Alignof(dir_watch_t));
    if (!d)
    {
        CloseHandle(h);
        return (fs_err_t){FS_ERR_OOM, 0};
    }
    memset(d, 0, offsetof(dir_watch_t, buf));
    d->dir = h;
    if (!CreateIoCompletionPort(h, w->port, (ULONG_PTR)d, 0))
    {
        err = last_err();
        CloseHandle(h);
        mem_free(w->a, d, sizeof *d);
        return err;
    }
    AcquireSRWLockExclusive(&w->lock);
    d->id = ++w->next_id;
    if (!issue_read(d))
    {
        err = last_err();
        ReleaseSRWLockExclusive(&w->lock);
        CloseHandle(h);
        mem_free(w->a, d, sizeof *d);
        return err;
    }
    d->next = w->watches;
    w->watches = d;
    *id = d->id;
    ReleaseSRWLockExclusive(&w->lock);
    return ok;
}

void fs_watch_remove(fs_watch_t *w, uint32_t watch_id)
{
    if (!w)
    {
        return;
    }
    AcquireSRWLockExclusive(&w->lock);
    for (dir_watch_t *d = w->watches; d; d = d->next)
    {
        if (d->id == watch_id && !d->removed)
        {
            d->removed = true;
            /* closing the handle cancels the read; its completion comes
             * back through the port, and then d is freed */
            CloseHandle(d->dir);
            d->dir = NULL;
            break;
        }
    }
    ReleaseSRWLockExclusive(&w->lock);
}

/* Unlinks d and gives it back (the lock is held). */
static void free_watch(fs_watch_t *w, dir_watch_t *d)
{
    for (dir_watch_t **p = &w->watches; *p; p = &(*p)->next)
    {
        if (*p == d)
        {
            *p = d->next;
            break;
        }
    }
    if (w->cur == d)
    {
        w->cur = NULL;
    }
    mem_free(w->a, d, sizeof *d);
}

/* Hands out events from the current buffer; when it is used up, gives it
 * back to the kernel.  *used: bytes of names taken so far. */
static void hand_out(
    fs_watch_t *w,
    fs_watch_event_t *out,
    size_t cap,
    size_t *count,
    size_t *used)
{
    dir_watch_t *d = w->cur;
    while (d && *count < cap && w->cur_off < w->cur_len)
    {
        const FILE_NOTIFY_INFORMATION *fi =
            (const FILE_NOTIFY_INFORMATION *)((const char *)d->buf
                                              + w->cur_off);
        size_t units = fi->FileNameLength / sizeof(WCHAR);
        if (*used + 3 * units + 1 > sizeof w->names)
        {
            break; /* no room for the name: the next call */
        }
        int bytes = units ? WideCharToMultiByte(
                                CP_UTF8,
                                0,
                                fi->FileName,
                                (int)units,
                                w->names + *used,
                                (int)(sizeof w->names - *used),
                                NULL,
                                NULL)
                          : 0;
        fs_watch_event_t *e = &out[*count];
        e->watch_id = d->id;
        e->name = (string_t){w->names + *used, bytes > 0 ? (size_t)bytes : 0};
        *used += bytes > 0 ? (size_t)bytes : 0;
        switch (fi->Action)
        {
        case FILE_ACTION_ADDED:
            e->kind = FS_W_CREATED;
            break;
        case FILE_ACTION_REMOVED:
            e->kind = FS_W_DELETED;
            break;
        case FILE_ACTION_RENAMED_OLD_NAME:
        case FILE_ACTION_RENAMED_NEW_NAME:
            e->kind = FS_W_RENAMED;
            break;
        default:
            e->kind = FS_W_MODIFIED;
            break;
        }
        if (!d->removed)
        {
            (*count)++;
        }
        w->cur_off =
            fi->NextEntryOffset ? w->cur_off + fi->NextEntryOffset : w->cur_len;
    }
    if (d && w->cur_off >= w->cur_len)
    {
        w->cur = NULL;
        AcquireSRWLockExclusive(&w->lock);
        if (d->removed || !issue_read(d))
        {
            if (!d->removed && d->dir)
            {
                CloseHandle(d->dir); /* the directory is gone */
                d->dir = NULL;
            }
            free_watch(w, d); /* no read pending: nothing comes back */
        }
        ReleaseSRWLockExclusive(&w->lock);
    }
}

fs_err_t fs_watch_wait(
    fs_watch_t *w,
    fs_watch_event_t *out,
    size_t cap,
    uint32_t timeout_ms,
    size_t *count)
{
    if (count)
    {
        *count = 0;
    }
    if (!w || !out || cap == 0 || !count)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    size_t used = 0;
    ULONGLONG start = GetTickCount64();
    for (;;)
    {
        hand_out(w, out, cap, count, &used);
        if (*count > 0)
        {
            return ok;
        }
        ULONGLONG spent = GetTickCount64() - start;
        DWORD left = spent >= timeout_ms ? 0 : (DWORD)(timeout_ms - spent);
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED *ov = NULL;
        BOOL got = GetQueuedCompletionStatus(w->port, &bytes, &key, &ov, left);
        if (!ov)
        {
            /* a wake (key 0), or the time is up */
            return got || GetLastError() == WAIT_TIMEOUT ? ok : last_err();
        }
        dir_watch_t *d = (dir_watch_t *)key;
        AcquireSRWLockExclusive(&w->lock);
        d->pending = false;
        if (d->removed)
        {
            free_watch(w, d); /* its cancelled read is back */
            ReleaseSRWLockExclusive(&w->lock);
            continue;
        }
        ReleaseSRWLockExclusive(&w->lock);
        if (!got)
        {
            /* the directory itself went away */
            out[(*count)++] = (fs_watch_event_t){FS_W_DELETED, d->id, {"", 0}};
            AcquireSRWLockExclusive(&w->lock);
            CloseHandle(d->dir);
            d->dir = NULL;
            free_watch(w, d);
            ReleaseSRWLockExclusive(&w->lock);
            return ok;
        }
        if (bytes == 0)
        {
            /* more changes than the buffer held */
            out[(*count)++] = (fs_watch_event_t){FS_W_OVERFLOW, d->id, {"", 0}};
            AcquireSRWLockExclusive(&w->lock);
            issue_read(d);
            ReleaseSRWLockExclusive(&w->lock);
            return ok;
        }
        w->cur = d;
        w->cur_off = 0;
        w->cur_len = bytes;
    }
}

void fs_watch_wake(fs_watch_t *w)
{
    if (w)
    {
        PostQueuedCompletionStatus(w->port, 0, 0, NULL);
    }
}

void fs_watch_destroy(fs_watch_t *w)
{
    if (!w)
    {
        return;
    }
    /* cancel every read, and wait for each to come back before its
     * buffer is freed */
    size_t pending = 0;
    for (dir_watch_t *d = w->watches; d; d = d->next)
    {
        if (d->dir)
        {
            CloseHandle(d->dir);
            d->dir = NULL;
        }
        pending += d->pending;
    }
    while (pending > 0)
    {
        DWORD bytes;
        ULONG_PTR key;
        OVERLAPPED *ov = NULL;
        GetQueuedCompletionStatus(w->port, &bytes, &key, &ov, 5000);
        if (!ov)
        {
            if (GetLastError() == WAIT_TIMEOUT)
            {
                break; /* should not happen; leak rather than free early */
            }
            continue; /* a wake */
        }
        pending--;
    }
    while (w->watches)
    {
        dir_watch_t *d = w->watches;
        w->watches = d->next;
        mem_free(w->a, d, sizeof *d);
    }
    CloseHandle(w->port);
    mem_free(w->a, w, sizeof *w);
}

/* --- Trash (step 8, tests/fs_trash_test.c) ---------------------------------
 *
 * SHFileOperationW with FOF_ALLOWUNDO puts it in the Recycle Bin.  But
 * where a drive has no Recycle Bin (network, removable) the same call
 * DELETES for good, without asking — so only fixed drives are let
 * through, and everything else is FS_ERR_UNSUPPORTED.  SHFileOperationW
 * knows no "\\?\" paths: MAX_PATH is its limit. */

#include <shellapi.h>

fs_err_t fs_trash(string_t path)
{
    wchar_t w[WPATH_MAX];
    fs_err_t err = to_wide(path, w);
    if (!fs_ok(err))
    {
        return err;
    }
    if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES)
    {
        return not_dir_on_the_way(path, last_err());
    }

    /* the plain Win32 path: "\\?\C:\x" -> "C:\x", "\\?\UNC\s\h" ->
     * "\\s\h" (a share: no Recycle Bin, refused below anyway) */
    const wchar_t *plain = w + 4;
    if (wcsncmp(w, L"\\\\?\\UNC\\", 8) == 0)
    {
        return (fs_err_t){FS_ERR_UNSUPPORTED, 0};
    }
    wchar_t root[4] = {plain[0], L':', L'\\', L'\0'};
    if (GetDriveTypeW(root) != DRIVE_FIXED)
    {
        return (fs_err_t){FS_ERR_UNSUPPORTED, 0}; /* would delete for good */
    }
    size_t n = wcslen(plain);
    if (n >= MAX_PATH)
    {
        return (fs_err_t){FS_ERR_NAME_TOO_LONG, 0};
    }
    wchar_t from[MAX_PATH + 1]; /* double-NUL-terminated list of one */
    wmemcpy(from, plain, n);
    from[n] = L'\0';
    from[n + 1] = L'\0';

    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI
                | FOF_NOCONFIRMMKDIR;
    int r = SHFileOperationW(&op);
    if (op.fAnyOperationsAborted)
    {
        return (fs_err_t){FS_ERR_CANCELLED, 0};
    }
    if (r != 0)
    {
        /* SHFileOperationW's own codes; most are Win32 codes too */
        return r == 0x7C /* DE_INVALIDFILES */
                   ? (fs_err_t){FS_ERR_NOT_FOUND, r}
                   : win_err((DWORD)r);
    }
    return ok;
}
