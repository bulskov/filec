#pragma once
/* filec/fs — filesystem access.  POSIX (Linux, macOS) and Win32 backends.
 *
 * STATUS: being implemented step by step — see README.md.  Step 1 (errors,
 * mkdir, remove, stat, exists) is specified by tests/fs_basic_test.c.
 *
 * Paths:     every path in and out is CANONICAL, normalised and ABSOLUTE
 *            (see filec/path.h): "/C:/Users/me", not "C:\Users\me".  A
 *            relative path is FS_ERR_INVALID — the current directory is
 *            process-wide state, and a file manager runs many threads.
 *            Paths typed by a user go through fs_from_native first.
 *            Input need not be NUL-terminated.
 * Path size: each call converts its paths to the native form (NUL on POSIX,
 *            UTF-16 and "\\?\" on Windows) in a stack buffer.  A canonical
 *            path longer than FS_PATH_MAX bytes is FS_ERR_NAME_TOO_LONG —
 *            Linux refuses longer paths itself.
 * Errors:    every function that can fail returns fs_err_t; results come
 *            back through out-parameters.  FS_NODISCARD makes ignoring the
 *            error a compiler warning.
 * Strings:   results that are strings APPEND to a strbuf_t you own, as in
 *            filec/path.h; on failure out is unchanged.  Names in directory
 *            listings and watch events are views, valid until the next
 *            read — copy what you keep.
 * Names out: raw bytes.  On Linux they may be invalid UTF-8; nothing is
 *            "fixed" here — sanitising for display is the caller's job.
 * Blocking:  every function may block on I/O.  Call them from worker threads
 *            if you have a UI.
 * Threads:   functions are safe to call concurrently on different objects.
 *            An fs_dir_t is single-owner.  Positional file I/O (fs_read_at /
 *            fs_write_at) may run concurrently on the same file. */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena/allocator.h"
#include "seqc/iter.h"
#include "seqc/string.h"
#include "seqc/vec.h"

/* Longest canonical path, in bytes, that fs accepts. */
#define FS_PATH_MAX 4096

#if defined(__GNUC__) || defined(__clang__)
#define FS_NODISCARD __attribute__((warn_unused_result))
#else
#define FS_NODISCARD
#endif

/* --- Errors -------------------------------------------------------------- */

typedef enum
{
    FS_OK = 0,
    FS_ERR_NOT_FOUND,
    FS_ERR_ACCESS, /* permission denied                              */
    FS_ERR_EXISTS,
    FS_ERR_NOT_EMPTY, /* directory not empty                         */
    FS_ERR_NOT_DIR,   /* a path component (or the target) is not a dir */
    FS_ERR_IS_DIR,
    FS_ERR_CROSS_DEVICE, /* rename across volumes: copy + delete instead */
    FS_ERR_NO_SPACE,
    FS_ERR_NAME_TOO_LONG, /* also: longer than FS_PATH_MAX              */
    FS_ERR_BUSY,          /* in use / sharing violation (Windows)      */
    FS_ERR_READ_ONLY,     /* read-only filesystem                      */
    FS_ERR_LOOP,          /* too many symlinks                         */
    FS_ERR_CANCELLED,     /* fs_op_t.cancel was set                    */
    FS_ERR_UNSUPPORTED,
    FS_ERR_OOM,     /* an allocator or strbuf could not grow            */
    FS_ERR_INVALID, /* relative path, NUL byte, NULL out, ...           */
    FS_ERR_IO,      /* anything else; see os_code                       */
} fs_err_kind_t;

typedef struct
{
    fs_err_kind_t kind;
    int32_t os_code; /* errno / GetLastError(), 0 if not from the OS */
} fs_err_t;

static inline bool fs_ok(fs_err_t e)
{
    return e.kind == FS_OK;
}

/* A short, static, human-readable description of e.kind ("not found").
 * Never empty, also for a kind out of range. */
string_t fs_err_str(fs_err_t e);

/* --- Entries ------------------------------------------------------------- */

typedef enum
{
    FS_KIND_UNKNOWN, /* not known yet: ask for FS_F_KIND             */
    FS_KIND_FILE,
    FS_KIND_DIR,
    FS_KIND_LINK,  /* symlink, or Windows junction / reparse point  */
    FS_KIND_OTHER, /* fifo, socket, device                          */
} fs_kind_t;

/* Requested (want) and delivered (valid) fields.  The backend fills what it
 * gets for free plus what you asked for; check valid, never assume.  Every
 * field you ask for is delivered, except FS_F_BTIME where the filesystem
 * does not record it. */
typedef enum
{
    FS_F_KIND = 1u << 0,
    FS_F_SIZE = 1u << 1,
    FS_F_MTIME = 1u << 2, /* last content modification                   */
    FS_F_BTIME = 1u << 3, /* birth/creation; not every Linux fs has it   */
    FS_F_MODE = 1u << 4,  /* POSIX permission bits; synthesised on Win32 */
    FS_F_FLAGS = 1u << 5, /* FS_FLAG_*                                   */
    FS_F_ID = 1u << 6,    /* (dev, ino) / (volume serial, file index)    */
    FS_F_ALL = 0x7f,
} fs_field_t;

enum
{
    FS_FLAG_HIDDEN = 1u << 0, /* dotfile on POSIX, attribute on Windows */
    FS_FLAG_READONLY = 1u << 1,
    FS_FLAG_SYSTEM = 1u << 2,
};

typedef struct
{
    string_t name;  /* file name only, no directory — a view          */
    uint32_t valid; /* fs_field_t bits that are filled in            */
    fs_kind_t kind;
    uint32_t flags;
    uint32_t mode;
    uint64_t size;
    int64_t mtime_ns; /* Unix epoch                                   */
    int64_t btime_ns;
    uint64_t dev, ino; /* together: the identity of the file          */
} fs_entry_t;

/* --- Reading directories ------------------------------------------------- */
/* "." and ".." are never returned.  Order is unspecified. */

typedef struct fs_dir_t fs_dir_t;

/* One allocation from a, for the directory and its buffer. */
FS_NODISCARD fs_err_t
fs_dir_open(string_t path, uint32_t want, allocator_t a, fs_dir_t **out);

/* Fills up to cap entries and sets *count; 0 means end of directory.  The
 * names are views into d, valid until the next fs_dir_read or
 * fs_dir_close.  An entry that vanished between listing and stat is
 * skipped, not an error. */
FS_NODISCARD fs_err_t
fs_dir_read(fs_dir_t *d, fs_entry_t *out, size_t cap, size_t *count);

void fs_dir_close(fs_dir_t *d); /* NULL is a no-op */

/* Convenience: iter_t yielding fs_entry_t (names valid until the next
 * step).  iter_t has no error channel, so errors — opening or mid-way —
 * stop the iteration and are written to *err: check it after the loop. */
iter_t fs_dir_iter(string_t path, uint32_t want, allocator_t a, fs_err_t *err);

/* --- Metadata ------------------------------------------------------------ */

enum
{
    FS_NOFOLLOW = 1u << 0, /* stat the link itself */
};

/* out->name is a view of path's file name. */
FS_NODISCARD fs_err_t
fs_stat(string_t path, uint32_t want, uint32_t flags, fs_entry_t *out);

/* true if path names something, following links: a dangling link does
 * not exist.  Any error counts as false. */
bool fs_exists(string_t path);

/* The target of a link, as stored (may be relative), appended to out. */
FS_NODISCARD fs_err_t fs_read_link(string_t path, strbuf_t *out);
/* path with every link resolved, appended to out. */
FS_NODISCARD fs_err_t fs_real_path(string_t path, strbuf_t *out);

/* --- Files --------------------------------------------------------------- */

typedef struct
{
    intptr_t h; /* fd or HANDLE — no allocation */
} fs_file_t;

enum
{
    FS_READ = 1u << 0,
    FS_WRITE = 1u << 1,
    FS_CREATE = 1u << 2,
    FS_TRUNC = 1u << 3,
    FS_EXCL = 1u << 4, /* with FS_CREATE: fail if it exists */
};

FS_NODISCARD fs_err_t fs_open(string_t path, uint32_t mode, fs_file_t *out);
/* Positional only — there is no seek and no hidden file position.
 * fs_read_at sets *got; fewer than n bytes means end of file.
 * fs_write_at writes all n bytes or fails. */
FS_NODISCARD fs_err_t
fs_read_at(fs_file_t f, uint64_t offset, void *dst, size_t n, size_t *got);
FS_NODISCARD fs_err_t
fs_write_at(fs_file_t f, uint64_t offset, const void *src, size_t n);
FS_NODISCARD fs_err_t fs_file_size(fs_file_t f, uint64_t *size);
FS_NODISCARD fs_err_t fs_sync(fs_file_t f);
void fs_close(fs_file_t f);

/* Whole-file helpers.  fs_read_all appends the contents to out. */
FS_NODISCARD fs_err_t fs_read_all(string_t path, strbuf_t *out);
/* Write to a temp file in the same directory, fsync, rename over path.
 * Readers see the old or the new content, never a mix. */
FS_NODISCARD fs_err_t fs_write_atomic(string_t path, string_t data);

/* --- Changing the filesystem --------------------------------------------- */
/* Single-object operations only.  Recursive copy / delete / walk are
 * deliberately left out for now — see docs/interface.md. */

typedef struct
{
    atomic_bool *cancel; /* NULL = not cancellable */
    void (*progress)(void *user, uint64_t done, uint64_t total);
    void *user;
} fs_op_t;

enum
{
    FS_REPLACE = 1u << 0,       /* overwrite an existing target  */
    FS_KEEP_METADATA = 1u << 1, /* copy: preserve mtime and mode */
};

/* One directory; its parent must exist.  FS_ERR_EXISTS if path exists
 * (whatever it is), FS_ERR_NOT_FOUND if the parent is missing,
 * FS_ERR_NOT_DIR if the parent is a file. */
FS_NODISCARD fs_err_t fs_mkdir(string_t path);
FS_NODISCARD fs_err_t fs_mkdir_all(string_t path); /* like mkdir -p */
/* A file, a link (not its target) or an EMPTY directory.
 * FS_ERR_NOT_EMPTY for a directory with anything in it. */
FS_NODISCARD fs_err_t fs_remove(string_t path);

/* Atomic, same volume only (FS_ERR_CROSS_DEVICE otherwise).  Without
 * FS_REPLACE the existence check is atomic too: renameat2(RENAME_NOREPLACE)
 * / MoveFileExW without MOVEFILE_REPLACE_EXISTING. */
FS_NODISCARD fs_err_t fs_rename(string_t from, string_t to, uint32_t flags);

/* One file.  copy_file_range / CopyFileExW; op may be NULL.
 * On cancel or error the partial target is removed. */
FS_NODISCARD fs_err_t
fs_copy_file(string_t from, string_t to, uint32_t flags, const fs_op_t *op);

/* XDG trash spec on Linux (home trash, or $topdir/.Trash-$uid on other
 * volumes), Recycle Bin on Windows. */
FS_NODISCARD fs_err_t fs_trash(string_t path);

/* --- Locations and volumes ----------------------------------------------- */

typedef enum
{
    FS_LOC_HOME,
    FS_LOC_TEMP,
    FS_LOC_CONFIG,
    FS_LOC_CACHE,
    FS_LOC_DATA,
} fs_loc_t;

/* XDG base directories / Windows Known Folders, appended to out. */
FS_NODISCARD fs_err_t fs_location(fs_loc_t loc, strbuf_t *out);
FS_NODISCARD fs_err_t fs_cwd(strbuf_t *out);

/* Any native path the OS would accept -> canonical, absolute, normalised,
 * appended to out.  Unlike path_from_native (filec/native.h), relative
 * forms are resolved against the real current directory, including
 * Windows' "C:x" (drive C's current directory) and "\x" (root of the
 * current drive).  For paths typed by a user or given on a command line. */
FS_NODISCARD fs_err_t fs_from_native(string_t native, strbuf_t *out);

typedef struct
{
    string_t path;  /* "/", "/media/usb", "/C:", "/UNC/srv/share" */
    string_t label; /* volume label, may be ""                  */
    bool removable;
    bool network;
    bool read_only;
} fs_volume_t;

/* *out: a vec_t of fs_volume_t; it and its strings come from a.
 * Linux: /proc/self/mountinfo minus pseudo-fs. */
FS_NODISCARD fs_err_t fs_volumes(allocator_t a, vec_t **out);
FS_NODISCARD fs_err_t fs_space(string_t path, uint64_t *total, uint64_t *avail);

/* --- Watching ------------------------------------------------------------ */
/* Events are hints, not truth: on FS_W_OVERFLOW (or whenever in doubt)
 * re-read the directory.  Watches are not recursive (inotify is not);
 * Windows' recursive mode is not exposed, to keep semantics identical. */

typedef enum
{
    FS_W_CREATED,
    FS_W_DELETED,
    FS_W_MODIFIED,
    FS_W_RENAMED,
    FS_W_OVERFLOW,
} fs_watch_kind_t;

typedef struct
{
    fs_watch_kind_t kind;
    uint32_t watch_id; /* from fs_watch_add; 0 for a global overflow */
    string_t name;     /* entry inside the watched dir, may be ""    */
} fs_watch_event_t;

typedef struct fs_watch_t fs_watch_t;

FS_NODISCARD fs_err_t fs_watch_create(allocator_t a, fs_watch_t **out);
/* add/remove may be called from any thread, also while another waits.
 * *id is never 0. */
FS_NODISCARD fs_err_t fs_watch_add(fs_watch_t *w, string_t dir, uint32_t *id);
void fs_watch_remove(fs_watch_t *w, uint32_t watch_id);
/* Blocks until events, timeout or fs_watch_wake; sets *count.  Names are
 * views into w, valid until the next fs_watch_wait. */
FS_NODISCARD fs_err_t fs_watch_wait(
    fs_watch_t *w,
    fs_watch_event_t *out,
    size_t cap,
    uint32_t timeout_ms,
    size_t *count);
/* Any thread: unblock a waiting fs_watch_wait (e.g. for shutdown). */
void fs_watch_wake(fs_watch_t *w);
void fs_watch_destroy(fs_watch_t *w);
