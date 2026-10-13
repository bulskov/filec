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

#include "../fs_internal.h"
#include "arena/fixed_arena.h"

#include <string.h>
#include <wchar.h>

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
    return tag.ReparseTag == IO_REPARSE_TAG_SYMLINK
           || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT; /* junction */
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
    out->name = path_file_name(path);

    bool dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->kind = link ? FS_KIND_LINK : dir ? FS_KIND_DIR : FS_KIND_FILE;
    out->size =
        dir ? 0 : ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    out->mtime_ns = unix_ns(info.ftLastWriteTime);
    out->btime_ns = unix_ns(info.ftCreationTime);

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

    /* (volume serial, file index) identify a file, like (dev, ino). */
    out->dev = info.dwVolumeSerialNumber;
    out->ino = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;

    out->valid = FS_F_ALL; /* NTFS records the birth time too */
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
