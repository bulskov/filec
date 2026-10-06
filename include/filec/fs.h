#pragma once
/* filec/fs — filesystem access.  POSIX (Linux first) and Win32 backends.
 *
 * STATUS: DRAFT INTERFACE — not implemented yet.  See docs/interface.md.
 *
 * Paths in:  UTF-8 string_t in canonical '/' form (see filec/path.h).  Need
 *            not be NUL-terminated.  Each call converts the path (NUL
 *            termination on POSIX, UTF-16 + "\\?\" prefix on Windows) into a
 *            conversion buffer — see "Path buffers" below.
 * Names out: raw bytes.  On Linux they may be invalid UTF-8; nothing is
 *            "fixed" here — sanitising for display is the caller's job.
 * Blocking:  every function may block on I/O.  Call them from worker threads
 *            if you have a UI.
 * Threads:   functions are safe to call concurrently on different objects.
 *            An fs_dir_t is single-owner.  Positional file I/O (fs_read_at /
 *            fs_write_at) may run concurrently on the same file.
 * Memory:    nothing is allocated except through an allocator_t you pass. */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena/allocator.h"
#include "seqc/iter.h"
#include "seqc/string.h"
#include "seqc/vec.h"

/* --- Path buffers -------------------------------------------------------- */
/* By default every call converts its path arguments in a stack buffer of
 * FS_PATH_MAX bytes; longer paths fail with FS_ERR_NAME_TOO_LONG.
 *
 * For longer paths the caller hands in the buffer and decides where it is
 * allocated (stack, arena, static).  The *_buf variants are the primitive;
 * the plain functions are thin wrappers that pass a stack buffer.
 *
 * OPEN: the _buf variants are only sketched for fs_stat and fs_dir_open
 * below.  Decide whether every path-taking function gets one, or whether a
 * single fs_pathbuf_t argument goes on all functions.  See
 * docs/interface.md. */

#define FS_PATH_MAX 4096

typedef struct
{
    void *ptr;   /* caller-owned scratch for path conversion           */
    size_t size; /* bytes; Windows needs about 2 * path.len + 16       */
} fs_pathbuf_t;

/* --- Errors -------------------------------------------------------------- */

typedef enum
{
    FS_OK = 0,
    FS_ERR_NOT_FOUND,
    FS_ERR_ACCESS, /* permission denied                              */
    FS_ERR_EXISTS,
    FS_ERR_NOT_EMPTY, /* directory not empty                         */
    FS_ERR_NOT_DIR,
    FS_ERR_IS_DIR,
    FS_ERR_CROSS_DEVICE, /* rename across volumes: copy + delete instead */
    FS_ERR_NO_SPACE,
    FS_ERR_NAME_TOO_LONG, /* also: path does not fit the path buffer   */
    FS_ERR_BUSY,          /* in use / sharing violation (Windows)      */
    FS_ERR_READ_ONLY,     /* read-only filesystem                      */
    FS_ERR_LOOP,          /* too many symlinks                         */
    FS_ERR_CANCELLED,     /* fs_op_t.cancel was set                    */
    FS_ERR_UNSUPPORTED,
    FS_ERR_OOM, /* the allocator you passed returned NULL               */
    FS_ERR_INVALID,
    FS_ERR_IO, /* anything else; see os_code                            */
} fs_err_kind_t;

typedef struct
{
    fs_err_kind_t kind;
    int32_t os_code; /* errno / GetLastError(), 0 if not from the OS */
} fs_err_t;

string_t fs_err_str(fs_err_t e); /* static, human-readable */

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
 * gets for free plus what you asked for; check valid, never assume. */
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
    string_t name;  /* file name only, no directory                  */
    uint32_t valid; /* fs_field_t bits that are filled in            */
    fs_kind_t kind;
    uint32_t flags;
    uint32_t mode;
    uint64_t size;
    int64_t mtime_ns; /* Unix epoch                                   */
    int64_t btime_ns;
    uint64_t dev, ino;
} fs_entry_t;

/* --- Reading directories ------------------------------------------------- */
/* "." and ".." are never returned.  Order is unspecified. */

typedef struct fs_dir_t fs_dir_t;

fs_dir_t *fs_dir_open(
    string_t path, uint32_t want, allocator_t a, fs_err_t *err);
fs_dir_t *fs_dir_open_buf(
    string_t path,
    uint32_t want,
    fs_pathbuf_t buf,
    allocator_t a,
    fs_err_t *err);

/* Fills up to cap entries, names allocated from `names`.  Returns the count;
 * 0 with err->kind == FS_OK means end of directory.  An entry that vanished
 * between listing and stat is skipped, not an error. */
size_t fs_dir_read(
    fs_dir_t *d,
    fs_entry_t *out,
    size_t cap,
    allocator_t names,
    fs_err_t *err);

void fs_dir_close(fs_dir_t *d);

/* Convenience: iter_t yielding fs_entry_t, names allocated from a.
 * Errors (open or mid-way) stop the iteration and are written to *err —
 * check it after the loop. */
iter_t fs_dir_iter(string_t path, uint32_t want, allocator_t a, fs_err_t *err);

/* --- Metadata ------------------------------------------------------------ */

enum
{
    FS_NOFOLLOW = 1u << 0, /* stat the link itself */
};

/* out->name is a view of path. */
bool fs_stat(
    string_t path,
    uint32_t want,
    uint32_t flags,
    fs_entry_t *out,
    fs_err_t *err);
bool fs_stat_buf(
    string_t path,
    uint32_t want,
    uint32_t flags,
    fs_pathbuf_t buf,
    fs_entry_t *out,
    fs_err_t *err);

bool fs_exists(string_t path); /* follows links */
string_t fs_read_link(string_t path, allocator_t a, fs_err_t *err);
string_t fs_real_path(string_t path, allocator_t a, fs_err_t *err);

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

bool fs_open(string_t path, uint32_t mode, fs_file_t *out, fs_err_t *err);
/* Positional only — there is no seek and no hidden file position. */
size_t fs_read_at(
    fs_file_t f, uint64_t offset, void *dst, size_t n, fs_err_t *err);
size_t fs_write_at(
    fs_file_t f, uint64_t offset, const void *src, size_t n, fs_err_t *err);
uint64_t fs_file_size(fs_file_t f, fs_err_t *err);
bool fs_sync(fs_file_t f, fs_err_t *err);
void fs_close(fs_file_t f);

/* Whole-file helpers. */
string_t fs_read_all(string_t path, allocator_t a, fs_err_t *err);
/* Write to a temp file in the same directory, fsync, rename over path.
 * Readers see the old or the new content, never a mix. */
bool fs_write_atomic(string_t path, string_t data, fs_err_t *err);

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

bool fs_mkdir(string_t path, fs_err_t *err);
bool fs_mkdir_all(string_t path, fs_err_t *err); /* like mkdir -p */
/* File, link or EMPTY directory. */
bool fs_remove(string_t path, fs_err_t *err);

/* Atomic, same volume only (FS_ERR_CROSS_DEVICE otherwise).  Without
 * FS_REPLACE the existence check is atomic too: renameat2(RENAME_NOREPLACE)
 * / MoveFileExW without MOVEFILE_REPLACE_EXISTING. */
bool fs_rename(string_t from, string_t to, uint32_t flags, fs_err_t *err);

/* One file.  copy_file_range / CopyFileExW; op may be NULL.
 * On cancel or error the partial target is removed. */
bool fs_copy_file(
    string_t from,
    string_t to,
    uint32_t flags,
    const fs_op_t *op,
    fs_err_t *err);

/* XDG trash spec on Linux (home trash, or $topdir/.Trash-$uid on other
 * volumes), Recycle Bin on Windows. */
bool fs_trash(string_t path, fs_err_t *err);

/* --- Locations and volumes ----------------------------------------------- */

typedef enum
{
    FS_LOC_HOME,
    FS_LOC_TEMP,
    FS_LOC_CONFIG,
    FS_LOC_CACHE,
    FS_LOC_DATA,
} fs_loc_t;

/* XDG base directories / Windows Known Folders. */
string_t fs_location(fs_loc_t loc, allocator_t a, fs_err_t *err);
string_t fs_cwd(allocator_t a, fs_err_t *err);

typedef struct
{
    string_t path;  /* "/", "/media/usb", "C:/", "//srv/share/" */
    string_t label; /* volume label, may be ""                  */
    bool removable;
    bool network;
    bool read_only;
} fs_volume_t;

/* vec_t of fs_volume_t.  Linux: /proc/self/mountinfo minus pseudo-fs. */
vec_t *fs_volumes(allocator_t a, fs_err_t *err);
bool fs_space(string_t path, uint64_t *total, uint64_t *free, fs_err_t *err);

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

fs_watch_t *fs_watch_create(allocator_t a, fs_err_t *err);
/* add/remove may be called from any thread, also while another waits.
 * Returns a watch id, 0 on failure. */
uint32_t fs_watch_add(fs_watch_t *w, string_t dir, fs_err_t *err);
void fs_watch_remove(fs_watch_t *w, uint32_t watch_id);
/* Blocks until events, timeout or fs_watch_wake.  Names allocated from a. */
size_t fs_watch_wait(
    fs_watch_t *w,
    fs_watch_event_t *out,
    size_t cap,
    uint32_t timeout_ms,
    allocator_t a,
    fs_err_t *err);
/* Any thread: unblock a waiting fs_watch_wait (e.g. for shutdown). */
void fs_watch_wake(fs_watch_t *w);
void fs_watch_destroy(fs_watch_t *w);
