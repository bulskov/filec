/* filec/fs — POSIX backend (Linux first: getdents64, statx, inotify,
 * copy_file_range, renameat2; macOS later with fallbacks).
 */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE /* macOS: st_mtimespec, st_birthtimespec */
#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include "filec/fs.h"
#include "filec/native.h"
#include "filec/path.h"

#include "../fs_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct filec_dir_t
{
    DIR *dir;
    allocator_t a;
    uint32_t want;
};

static fs_err_t filec_from_errno(int e)
{
    switch (e)
    {
    case ENOENT:
        return (fs_err_t){.kind = FS_ERR_NOT_FOUND, ENOENT};
    case EACCES:
        return (fs_err_t){.kind = FS_ERR_ACCESS, EACCES};
    case EPERM:
        return (fs_err_t){.kind = FS_ERR_ACCESS, EPERM};
    case EEXIST:
        return (fs_err_t){.kind = FS_ERR_EXISTS, EEXIST};
    case ENOTEMPTY:
        return (fs_err_t){.kind = FS_ERR_NOT_EMPTY, ENOTEMPTY};
    case ENOTDIR:
        return (fs_err_t){.kind = FS_ERR_NOT_DIR, ENOTDIR};
    case EISDIR:
        return (fs_err_t){.kind = FS_ERR_IS_DIR, EISDIR};
    case EXDEV:
        return (fs_err_t){.kind = FS_ERR_CROSS_DEVICE, EXDEV};
    case ENOSPC:
        return (fs_err_t){.kind = FS_ERR_NO_SPACE, ENOSPC};
    case ENAMETOOLONG:
        return (fs_err_t){.kind = FS_ERR_NAME_TOO_LONG, ENAMETOOLONG};
    case EBUSY:
        return (fs_err_t){.kind = FS_ERR_BUSY, EBUSY};
    case EROFS:
        return (fs_err_t){.kind = FS_ERR_READ_ONLY, EROFS};
    case ELOOP:
        return (fs_err_t){.kind = FS_ERR_LOOP, ELOOP};
    case ENOMEM:
        return (fs_err_t){.kind = FS_ERR_OOM, ENOMEM};
    default:
        return (fs_err_t){.kind = FS_ERR_IO, e};
    }
}

static void entry_from_stat(const struct stat *st, fs_entry_t *e)
{
    if (S_ISREG(st->st_mode))
    {
        e->kind = FS_KIND_FILE;
    }
    else if (S_ISDIR(st->st_mode))
    {
        e->kind = FS_KIND_DIR;
    }
    else if (S_ISLNK(st->st_mode))
    {
        e->kind = FS_KIND_LINK;
    }
    else
    {
        e->kind = FS_KIND_OTHER;
    }
    e->size = (uint64_t)st->st_size;
    e->mode = (uint32_t)(st->st_mode & 07777);
    e->dev = st->st_dev;
    e->ino = st->st_ino;
#ifdef __APPLE__
    struct timespec mt = st->st_mtimespec;
#else
    struct timespec mt = st->st_mtim;
#endif
    e->mtime_ns = (int64_t)mt.tv_sec * 1000000000 + mt.tv_nsec;
    e->valid = FS_F_ALL & ~(uint32_t)FS_F_BTIME;
}

fs_err_t fs_mkdir(string_t path)
{
    char buf[FILEC_PATH_BUF_MAX];

    fs_err_t err = filec_path_cstr(path, buf, FILEC_PATH_BUF_MAX);
    if (err.kind != FS_OK)
    {
        return err;
    }

    int e = mkdir(buf, 0777);
    if (e == 0)
    {
        return (fs_err_t){.kind = FS_OK, 0};
    }

    return filec_from_errno(errno);
}

fs_err_t fs_remove(string_t path)
{
    char buf[FILEC_PATH_BUF_MAX];

    fs_err_t err = filec_path_cstr(path, buf, FILEC_PATH_BUF_MAX);
    if (err.kind != FS_OK)
    {
        return err;
    }

    int e = unlink(buf);
    if (e == 0)
    {
        return (fs_err_t){.kind = FS_OK, 0};
    }

    int err_save = errno;

    if (errno != EISDIR && errno != EPERM)
    {

        return filec_from_errno(errno);
    }

    if (rmdir(buf) == 0)
    {
        return (fs_err_t){.kind = FS_OK, 0};
    }

    if (errno == ENOTDIR && err_save == EPERM)
    {
        return filec_from_errno(err_save);
    }

    return filec_from_errno(errno == EEXIST ? ENOTEMPTY : errno);
}

fs_err_t fs_stat(string_t path, uint32_t want, uint32_t flags, fs_entry_t *out)
{
    (void)want;
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    char buf[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(path, buf, FILEC_PATH_BUF_MAX);
    if (!fs_ok(err))
    {
        return err;
    }

    struct stat st;
    int r = (flags & FS_NOFOLLOW) ? lstat(buf, &st) : stat(buf, &st);
    if (r != 0)
    {
        return filec_from_errno(errno); /* not always NOT_FOUND */
    }

    memset(out, 0, sizeof *out);

    entry_from_stat(&st, out);

    out->name = path_file_name(path);

    if (out->name.len > 0 && out->name.ptr[0] == '.')
    {
        out->flags |= FS_FLAG_HIDDEN;
    }

    return (fs_err_t){FS_OK, 0};
}

bool fs_exists(string_t path)
{
    char buf[FILEC_PATH_BUF_MAX];
    if (filec_path_cstr(path, buf, FILEC_PATH_BUF_MAX).kind != FS_OK)
    {
        return false;
    }
    return access(buf, F_OK) == 0;
}

/* --- Step 2 stubs: replace with the real thing ---------------------------
 * (tests/fs_file_test.c).  They exist so the tests build and fail. */

fs_err_t fs_open(string_t path, uint32_t mode, fs_file_t *out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    *out = FS_FILE_NONE;

    char buf[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(path, buf, FILEC_PATH_BUF_MAX);
    if (err.kind != FS_OK)
    {
        return err;
    }

    bool w = mode & FS_WRITE;
    if (!(mode & (FS_READ | FS_WRITE))
        || ((mode & (FS_CREATE | FS_TRUNC)) && !w)
        || ((mode & FS_EXCL) && !(mode & FS_CREATE)))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    int mode_flags = 0;

    if (mode & FS_READ && mode & FS_WRITE)
    {
        mode_flags |= O_RDWR;
    }
    else if (mode & FS_READ)
    {
        mode_flags |= O_RDONLY;
    }
    else if (mode & FS_WRITE)
    {
        mode_flags |= O_WRONLY;
    }

    if (mode & FS_CREATE)
    {
        mode_flags |= O_CREAT;
    }
    if (mode & FS_EXCL)
    {
        mode_flags |= O_EXCL;
    }
    if (mode & FS_TRUNC)
    {
        mode_flags |= O_TRUNC;
    }

    mode_flags |= O_CLOEXEC;

    int f = open(buf, mode_flags, 0666);

    if (f == -1)
    {
        return filec_from_errno(errno);
    }

    struct stat st;
    if (fstat(f, &st) == -1)
    {
        int e = errno;
        close(f);
        return filec_from_errno(e);
    }

    bool is_dir = S_ISDIR(st.st_mode);

    if (is_dir)
    {
        close(f);
        return (fs_err_t){FS_ERR_IS_DIR, 0};
    }

    out->h = f;
    return (fs_err_t){FS_OK, 0};
}

fs_err_t fs_read_at(
    fs_file_t f, uint64_t offset, void *dst, size_t n, size_t *got)
{
    if (!got)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    *got = 0;

    if (!fs_file_is_open(f) || (!dst && n > 0))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    if (offset > INT64_MAX)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    if (n == 0)
    {
        return (fs_err_t){FS_OK, 0};
    }

    char *p = dst;
    while (*got < n)
    {
        ssize_t r = pread((int)f.h, p + *got, n - *got, (off_t)(offset + *got));
        if (r < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return filec_from_errno(errno);
        }
        if (r == 0)
        {
            break; /* end of file */
        }
        *got += (size_t)r;
    }
    return (fs_err_t){FS_OK, 0};
}

fs_err_t fs_write_at(fs_file_t f, uint64_t offset, const void *src, size_t n)
{

    if (!fs_file_is_open(f) || (!src && n > 0))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    if (offset > INT64_MAX)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    if (n == 0)
    {
        return (fs_err_t){FS_OK, 0};
    }

    size_t written = 0;
    while (written < n)
    {
        ssize_t r = pwrite(
            (int)f.h,
            (const char *)src + written,
            n - written,
            (off_t)(offset + written));
        if (r < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return filec_from_errno(errno);
        }
        if (r == 0)
        {
            return (fs_err_t){FS_ERR_IO, 0};
        }
        written += (size_t)r;
    }
    return (fs_err_t){FS_OK, 0};
}

fs_err_t fs_file_size(fs_file_t f, uint64_t *size)
{
    if (!fs_file_is_open(f) || !size)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    struct stat st;
    if (fstat((int)f.h, &st) == -1)
    {
        return filec_from_errno(errno);
    }

    *size = (uint64_t)(st.st_size);
    return (fs_err_t){FS_OK, 0};
}

fs_err_t fs_sync(fs_file_t f)
{
    if (!fs_file_is_open(f))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    if (fsync((int)f.h) == -1)
    {
        return filec_from_errno(errno);
    }
    return (fs_err_t){FS_OK, 0};
}

void fs_close(fs_file_t f)
{
    if (fs_file_is_open(f))
    {
        close((int)f.h);
    }
}

fs_err_t fs_write_atomic(string_t path, string_t data)
{
    char path_buf[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(path, path_buf, FILEC_PATH_BUF_MAX);
    if (err.kind != FS_OK)
    {
        return err;
    }

    char tmp_buf[FILEC_PATH_BUF_MAX + 32];
    fs_file_t f_tmp;
    long pid = (long)getpid();
    for (int i = 1;; i++)
    {
        if (i == 1000)
        {
            return (fs_err_t){FS_ERR_IO, 0};
        }
        snprintf(tmp_buf, sizeof tmp_buf, "%s.tmp%ld.%03d", path_buf, pid, i);
        f_tmp.h = open(tmp_buf, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0666);
        if (f_tmp.h >= 0)
        {
            break;
        }
        if (errno != EEXIST)
        {
            return filec_from_errno(errno);
        }
    }

    struct stat st;
    if (stat(path_buf, &st) == 0 && S_ISREG(st.st_mode))
    {
        fchmod((int)f_tmp.h, st.st_mode & 07777);
    }

    err = fs_write_at(f_tmp, 0, data.ptr, data.len);
    if (!fs_ok(err))
    {
        goto err_close_unlink;
    }

    err = fs_sync(f_tmp);
    if (!fs_ok(err))
    {
        goto err_close_unlink;
    }

    fs_close(f_tmp);
    f_tmp = FS_FILE_NONE;

    if (rename(tmp_buf, path_buf) == -1)
    {
        err = filec_from_errno(errno);
        goto err_close_unlink;
    }

    return (fs_err_t){FS_OK, 0};

err_close_unlink:
    fs_close(f_tmp);
    unlink(tmp_buf);
    return err;
}

fs_err_t filec_dir_open(
    string_t path, uint32_t want, allocator_t a, filec_dir_t **out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    *out = NULL;

    char path_buf[FILEC_PATH_BUF_MAX];

    fs_err_t err = filec_path_cstr(path, path_buf, FILEC_PATH_BUF_MAX);
    if (err.kind != FS_OK)
    {
        return err;
    }

    filec_dir_t *d = mem_alloc(a, sizeof *d, _Alignof(filec_dir_t));
    if (!d)
    {
        return (fs_err_t){FS_ERR_OOM, 0};
    }
    d->dir = opendir(path_buf);
    if (!d->dir)
    {
        int e = errno; /* mem_free may change errno */
        mem_free(a, d, sizeof *d);
        return filec_from_errno(e);
    }
    d->a = a;
    d->want = want;
    *out = d;

    return (fs_err_t){FS_OK, 0};
}

static fs_kind_t kind_from_d_type(unsigned char d_type)
{
    switch (d_type)
    {
    case DT_REG:
        return FS_KIND_FILE;
    case DT_DIR:
        return FS_KIND_DIR;
    case DT_LNK:
        return FS_KIND_LINK;
    default:
        return FS_KIND_OTHER;
    }
}

fs_err_t filec_dir_next(filec_dir_t *d, fs_entry_t *e, bool *done)
{
    if (!d || !e || !done)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    while (true)
    {
        errno = 0;
        struct dirent *entry = readdir(d->dir);
        if (!entry)
        {
            if (errno)
            {
                return filec_from_errno(errno);
            }
            *done = true;
            return (fs_err_t){FS_OK, 0};
        }

        if (entry->d_name[0] == '.'
            && (entry->d_name[1] == '\0'
                || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
        {
            continue; /* skip "." and ".." */
        }

        memset(e, 0, sizeof *e);

        bool need_stat =
            (d->want & (FS_F_SIZE | FS_F_MTIME | FS_F_MODE | FS_F_ID))
            || entry->d_type == DT_UNKNOWN;

        if (need_stat)
        {
            struct stat st;
            if (fstatat(dirfd(d->dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW)
                == -1)
            {
                if (errno == ENOENT)
                {
                    continue;
                }
                return filec_from_errno(errno);
            }
            entry_from_stat(&st, e);
        }
        else
        {
            e->kind = kind_from_d_type(entry->d_type);
            e->valid = FS_F_KIND;
        }

        if (entry->d_name[0] == '.')
        {
            e->flags |= FS_FLAG_HIDDEN;
        }

        e->valid |= FS_F_FLAGS;
        e->name = (string_t){entry->d_name, strlen(entry->d_name)};
        *done = false;

        return (fs_err_t){FS_OK, 0};
    }
}

void filec_dir_close(filec_dir_t *d)
{
    if (d)
    {
        closedir(d->dir);
        mem_free(d->a, d, sizeof *d);
    }
}

/* --- Renaming, copying, links (step 4, tests/fs_ops_test.c) ------------ */

static const fs_err_t posix_ok = {FS_OK, 0};

/* rename that fails with EEXIST if to exists, atomically where the OS can
 * (Linux renameat2, macOS renameatx_np).  Where it cannot — a kernel or a
 * filesystem without the flag (EINVAL / ENOSYS / ENOTSUP: older NFS, some
 * FUSE) — it falls back to check-then-rename, which only a racing creator
 * can beat. */
static int rename_noreplace(const char *from, const char *to)
{
#if defined(__APPLE__)
    if (renameatx_np(AT_FDCWD, from, AT_FDCWD, to, RENAME_EXCL) == 0)
    {
        return 0;
    }
#elif defined(__linux__)
    if (renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) == 0)
    {
        return 0;
    }
#else
    errno = ENOSYS;
#endif
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP)
    {
        return -1;
    }
    struct stat st;
    if (lstat(to, &st) == 0)
    {
        errno = EEXIST;
        return -1;
    }
    return rename(from, to);
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
    char from_buf[FILEC_PATH_BUF_MAX];
    char to_buf[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(from, from_buf, sizeof from_buf);
    if (!fs_ok(err))
    {
        return err;
    }
    err = filec_path_cstr(to, to_buf, sizeof to_buf);
    if (!fs_ok(err))
    {
        return err;
    }
    if (is_inside(to, from))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    struct stat from_st;
    if (lstat(from_buf, &from_st) != 0)
    {
        return filec_from_errno(errno);
    }
    struct stat to_st;
    bool to_exists = lstat(to_buf, &to_st) == 0;
    if (!to_exists && errno != ENOENT)
    {
        return filec_from_errno(errno); /* e.g. a file on the way */
    }

    if (to_exists && from_st.st_dev == to_st.st_dev
        && from_st.st_ino == to_st.st_ino)
    {
        if (strcmp(from_buf, to_buf) == 0)
        {
            return posix_ok; /* to itself: nothing to do */
        }
        /* The same entry under another case (a case-insensitive volume):
         * rename changes the case.  Otherwise these are two hard links to
         * one file, and to is in the way like any other file. */
        if (path_equals(from, to, PATH_CASE_INSENSITIVE))
        {
            return rename(from_buf, to_buf) == 0 ? posix_ok
                                                 : filec_from_errno(errno);
        }
    }

    if (to_exists)
    {
        if (S_ISDIR(to_st.st_mode))
        {
            return (fs_err_t){
                (flags & FS_REPLACE) ? FS_ERR_IS_DIR : FS_ERR_EXISTS, 0};
        }
        if (!(flags & FS_REPLACE))
        {
            return (fs_err_t){FS_ERR_EXISTS, 0};
        }
    }

    int r = (flags & FS_REPLACE) ? rename(from_buf, to_buf)
                                 : rename_noreplace(from_buf, to_buf);
    if (r != 0)
    {
        return errno == EINVAL ? (fs_err_t){FS_ERR_INVALID, EINVAL}
                               : filec_from_errno(errno);
    }
    return posix_ok;
}

/* At most this much between two looks at cancel and two progress calls. */
#define COPY_CHUNK ((size_t)1 << 20)

/* The read/write fallback's buffer, on the stack. */
#define COPY_BUF (64 * 1024)

/* Up to COPY_CHUNK bytes from in to out at their current positions:
 * copy_file_range on Linux (in the kernel, reflinks where the filesystem
 * can), read/write of up to COPY_BUF otherwise or where it is refused
 * (another filesystem, an old kernel).  *got: what was copied, 0 at the
 * end of in. */
static fs_err_t copy_chunk(int in, int out, char buf[COPY_BUF], size_t *got)
{
#if defined(__linux__)
    static bool no_cfr; /* once refused, never asked again */
    if (!no_cfr)
    {
        for (;;)
        {
            ssize_t r = copy_file_range(in, NULL, out, NULL, COPY_CHUNK, 0);
            if (r >= 0)
            {
                *got = (size_t)r;
                return posix_ok;
            }
            if (errno == EINTR)
            {
                continue;
            }
            if (errno != EXDEV && errno != ENOSYS && errno != EINVAL
                && errno != EOPNOTSUPP)
            {
                return filec_from_errno(errno);
            }
            no_cfr = true; /* nothing was copied: read/write instead */
            break;
        }
    }
#endif
    ssize_t r;
    do
    {
        r = read(in, buf, COPY_BUF);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
    {
        return filec_from_errno(errno);
    }
    for (size_t done = 0; done < (size_t)r;)
    {
        ssize_t w = write(out, buf + done, (size_t)r - done);
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return filec_from_errno(errno);
        }
        done += (size_t)w;
    }
    *got = (size_t)r;
    return posix_ok;
}

static bool cancelled(const fs_op_t *op)
{
    return op && op->cancel && atomic_load(op->cancel);
}

fs_err_t fs_copy_file(
    string_t from, string_t to, uint32_t flags, const fs_op_t *op)
{
    char from_buf[FILEC_PATH_BUF_MAX];
    char to_buf[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(from, from_buf, sizeof from_buf);
    if (!fs_ok(err))
    {
        return err;
    }
    err = filec_path_cstr(to, to_buf, sizeof to_buf);
    if (!fs_ok(err))
    {
        return err;
    }

    int in = open(from_buf, O_RDONLY | O_CLOEXEC); /* follows a link */
    if (in < 0)
    {
        return filec_from_errno(errno);
    }
    struct stat from_st;
    if (fstat(in, &from_st) != 0)
    {
        err = filec_from_errno(errno);
        close(in);
        return err;
    }
    if (S_ISDIR(from_st.st_mode))
    {
        close(in);
        return (fs_err_t){FS_ERR_IS_DIR, 0};
    }

    /* What is at to: the same file (copying would empty it), a directory
     * (also through a link), or anything at all without FS_REPLACE. */
    struct stat to_st;
    if (lstat(to_buf, &to_st) == 0)
    {
        struct stat target; /* to followed, if it is a link */
        bool followed = stat(to_buf, &target) == 0;
        if (followed && target.st_dev == from_st.st_dev
            && target.st_ino == from_st.st_ino)
        {
            err = (fs_err_t){FS_ERR_INVALID, 0};
        }
        else if (
            S_ISDIR(to_st.st_mode) || (followed && S_ISDIR(target.st_mode)))
        {
            err = (fs_err_t){FS_ERR_IS_DIR, 0};
        }
        else if (!(flags & FS_REPLACE))
        {
            err = (fs_err_t){FS_ERR_EXISTS, 0};
        }
    }
    else if (errno != ENOENT)
    {
        err = filec_from_errno(errno);
    }
    if (!fs_ok(err))
    {
        close(in);
        return err;
    }

    /* The copy is written beside to and renamed into place at the end, so
     * an old file at to survives a cancel or an error. */
    char tmp_buf[FILEC_PATH_BUF_MAX + 32];
    long pid = (long)getpid();
    int out = -1;
    for (int i = 1; out < 0; i++)
    {
        if (i == 1000)
        {
            close(in);
            return (fs_err_t){FS_ERR_IO, 0};
        }
        snprintf(tmp_buf, sizeof tmp_buf, "%s.tmp%ld.%03d", to_buf, pid, i);
        out = open(tmp_buf, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (out < 0 && errno != EEXIST)
        {
            err = filec_from_errno(errno);
            close(in);
            return err;
        }
    }

    char buf[COPY_BUF];
    uint64_t total = (uint64_t)from_st.st_size;
    uint64_t done = 0;
    while (fs_ok(err))
    {
        if (cancelled(op))
        {
            err = (fs_err_t){FS_ERR_CANCELLED, 0};
            break;
        }
        size_t got = 0;
        err = copy_chunk(in, out, buf, &got);
        if (!fs_ok(err) || got == 0)
        {
            break;
        }
        done += got;
        if (op && op->progress)
        {
            op->progress(op->user, done, total > done ? total : done);
        }
    }
    if (fs_ok(err) && done == 0 && op && op->progress)
    {
        op->progress(op->user, 0, 0); /* an empty file: done == total */
    }

    if (fs_ok(err) && (flags & FS_KEEP_METADATA))
    {
#ifdef __APPLE__
        struct timespec times[2] = {from_st.st_atimespec, from_st.st_mtimespec};
#else
        struct timespec times[2] = {from_st.st_atim, from_st.st_mtim};
#endif
        if (futimens(out, times) != 0
            || fchmod(out, from_st.st_mode & 07777) != 0)
        {
            err = filec_from_errno(errno);
        }
    }
    /* Replacing a file: its content must be on disk before the rename
     * puts it in place (as in fs_write_atomic). */
    if (fs_ok(err) && (flags & FS_REPLACE) && fsync(out) != 0)
    {
        err = filec_from_errno(errno);
    }
    if (close(out) != 0 && fs_ok(err))
    {
        err = filec_from_errno(errno);
    }
    close(in);

    if (fs_ok(err))
    {
        int r = (flags & FS_REPLACE) ? rename(tmp_buf, to_buf)
                                     : rename_noreplace(tmp_buf, to_buf);
        if (r != 0)
        {
            err = filec_from_errno(errno);
        }
    }
    if (!fs_ok(err))
    {
        unlink(tmp_buf);
    }
    return err;
}

fs_err_t fs_read_link(string_t path, strbuf_t *out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    char buf[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(path, buf, sizeof buf);
    if (!fs_ok(err))
    {
        return err;
    }
    char target[FILEC_PATH_BUF_MAX];
    ssize_t n = readlink(buf, target, sizeof target);
    if (n < 0)
    {
        return errno == EINVAL ? (fs_err_t){FS_ERR_INVALID, EINVAL}
                               /* no link */
                               : filec_from_errno(errno);
    }
    if ((size_t)n == sizeof target)
    {
        return (fs_err_t){FS_ERR_NAME_TOO_LONG, 0};
    }
    string_t t = {target, (size_t)n};

    /* As stored; an absolute target is normalised to canonical form. */
    if (n > 0 && target[0] == '/')
    {
        switch (path_from_native(t, PATH_STYLE_POSIX, out))
        {
        case PATH_NATIVE_OK:
            return posix_ok;
        case PATH_NATIVE_OOM:
            return (fs_err_t){FS_ERR_OOM, 0};
        default:
            return (fs_err_t){FS_ERR_INVALID, 0};
        }
    }
    return strbuf_append(out, t) == SEQC_OK ? posix_ok
                                            : (fs_err_t){FS_ERR_OOM, 0};
}

fs_err_t fs_real_path(string_t path, strbuf_t *out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    char buf[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(path, buf, sizeof buf);
    if (!fs_ok(err))
    {
        return err;
    }
    char resolved[PATH_MAX];
    if (!realpath(buf, resolved))
    {
        return filec_from_errno(errno);
    }
    switch (path_from_native(string_view_cstr(resolved), PATH_STYLE_POSIX, out))
    {
    case PATH_NATIVE_OK:
        return posix_ok;
    case PATH_NATIVE_OOM:
        return (fs_err_t){FS_ERR_OOM, 0};
    default:
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
}

/* --- Places (step 5, tests/fs_places_test.c) ----------------------------- */

#include <pwd.h>

/* A native POSIX path (absolute) -> canonical, appended to out. */
static fs_err_t append_from_native(string_t native, strbuf_t *out)
{
    switch (path_from_native(native, PATH_STYLE_POSIX, out))
    {
    case PATH_NATIVE_OK:
        return posix_ok;
    case PATH_NATIVE_OOM:
        return (fs_err_t){FS_ERR_OOM, 0};
    default:
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
}

fs_err_t fs_cwd(strbuf_t *out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    char buf[FILEC_PATH_BUF_MAX];
    if (!getcwd(buf, sizeof buf))
    {
        return errno == ERANGE ? (fs_err_t){FS_ERR_NAME_TOO_LONG, ERANGE}
                               : filec_from_errno(errno);
    }
    return append_from_native(string_view_cstr(buf), out);
}

/* An environment variable that holds an absolute path, or NULL. */
static const char *absolute_env(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] == '/' ? v : NULL;
}

/* The home directory into buf: $HOME, or the password database. */
static fs_err_t home_dir(char *buf, size_t size)
{
    const char *home = absolute_env("HOME");
    char pwbuf[16 * 1024];
    if (!home)
    {
        struct passwd pw, *found = NULL;
        int e = getpwuid_r(getuid(), &pw, pwbuf, sizeof pwbuf, &found);
        if (e != 0 || !found || !pw.pw_dir || pw.pw_dir[0] != '/')
        {
            return e != 0 ? filec_from_errno(e)
                          : (fs_err_t){FS_ERR_NOT_FOUND, 0};
        }
        home = pw.pw_dir;
    }
    if ((size_t)snprintf(buf, size, "%s", home) >= size)
    {
        return (fs_err_t){FS_ERR_NAME_TOO_LONG, 0};
    }
    return posix_ok;
}

fs_err_t fs_location(fs_loc_t loc, strbuf_t *out)
{
    if (!out || (unsigned)loc > (unsigned)FS_LOC_STATE)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    if (loc == FS_LOC_TEMP)
    {
        const char *tmp = absolute_env("TMPDIR");
        return append_from_native(string_view_cstr(tmp ? tmp : "/tmp"), out);
    }

#ifndef __APPLE__
    /* XDG base directories: an absolute $XDG_*_HOME wins */
    const char *xdg = loc == FS_LOC_CONFIG  ? absolute_env("XDG_CONFIG_HOME")
                      : loc == FS_LOC_CACHE ? absolute_env("XDG_CACHE_HOME")
                      : loc == FS_LOC_DATA  ? absolute_env("XDG_DATA_HOME")
                      : loc == FS_LOC_STATE ? absolute_env("XDG_STATE_HOME")
                                            : NULL;
    if (xdg)
    {
        return append_from_native(string_view_cstr(xdg), out);
    }
#endif

    char home[FILEC_PATH_BUF_MAX];
    fs_err_t err = home_dir(home, sizeof home);
    if (!fs_ok(err))
    {
        return err;
    }
#ifdef __APPLE__
    const char *under = loc == FS_LOC_CACHE  ? "Library/Caches"
                        : loc == FS_LOC_HOME ? ""
                                             : "Library/Application Support";
#else
    const char *under = loc == FS_LOC_CONFIG  ? ".config"
                        : loc == FS_LOC_CACHE ? ".cache"
                        : loc == FS_LOC_DATA  ? ".local/share"
                        : loc == FS_LOC_STATE ? ".local/state"
                                              : "";
#endif
    char buf[2 * FILEC_PATH_BUF_MAX];
    snprintf(buf, sizeof buf, "%s/%s", home, under);
    return append_from_native(string_view_cstr(buf), out);
}

fs_err_t fs_from_native(string_t native, strbuf_t *out)
{
    if (!out || native.len == 0 || memchr(native.ptr, '\0', native.len))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    if (native.ptr[0] == '/')
    {
        return append_from_native(native, out);
    }
    /* relative: against the current directory, then normalised */
    char buf[2 * FILEC_PATH_BUF_MAX + 2];
    if (!getcwd(buf, FILEC_PATH_BUF_MAX))
    {
        return errno == ERANGE ? (fs_err_t){FS_ERR_NAME_TOO_LONG, ERANGE}
                               : filec_from_errno(errno);
    }
    size_t n = strlen(buf);
    if (n + 1 + native.len >= sizeof buf)
    {
        return (fs_err_t){FS_ERR_NAME_TOO_LONG, 0};
    }
    buf[n++] = '/';
    memcpy(buf + n, native.ptr, native.len);
    return append_from_native((string_t){buf, n + native.len}, out);
}

/* --- Volumes (step 6, tests/fs_volume_test.c) ----------------------------- */

#include <sys/statvfs.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#endif

/* Appends v (its strings copied into a) to out; false: out of memory. */
static bool push_volume(
    allocator_t a,
    vec_t *out,
    const char *path,
    size_t path_len,
    const char *label,
    size_t label_len,
    fs_volume_t v)
{
    return filec_dup_string(a, path, path_len, &v.path)
           && filec_dup_string(a, label, label_len, &v.label)
           && vec_push(out, &v) == SEQC_OK;
}

#if defined(__linux__)

/* Undoes mountinfo's octal escapes (\040 for a space ...) in place. */
static size_t unescape_octal(char *s)
{
    size_t r = 0, w = 0;
    while (s[r])
    {
        if (s[r] == '\\' && s[r + 1] >= '0' && s[r + 1] <= '3'
            && s[r + 2] >= '0' && s[r + 2] <= '7' && s[r + 3] >= '0'
            && s[r + 3] <= '7')
        {
            s[w++] = (char)((s[r + 1] - '0') * 64 + (s[r + 2] - '0') * 8
                            + (s[r + 3] - '0'));
            r += 4;
        }
        else
        {
            s[w++] = s[r++];
        }
    }
    s[w] = '\0';
    return w;
}

static bool in_list(const char *s, const char *const *list)
{
    for (; *list; list++)
    {
        if (strcmp(s, *list) == 0)
        {
            return true;
        }
    }
    return false;
}

static const char *const network_fs[] = {
    "nfs",
    "nfs4",
    "cifs",
    "smb3",
    "smbfs",
    "ceph",
    "glusterfs",
    "afs",
    "9p",
    "fuse.sshfs",
    "fuse.rclone",
    "davfs",
    NULL};

/* Block devices that are not where a user keeps files. */
static const char *const hidden_fs[] = {"squashfs", "swap", NULL};

/* A small file's first line (a /sys attribute) into buf. */
static bool read_small(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n <= 0)
    {
        return false;
    }
    buf[n] = '\0';
    return true;
}

/* Removable: /sys says so for the disk (a partition's parent), or the
 * device hangs off USB. */
static bool is_removable(const char *majmin)
{
    char p[128], v[16];
    snprintf(p, sizeof p, "/sys/dev/block/%s/removable", majmin);
    if (read_small(p, v, sizeof v) && v[0] == '1')
    {
        return true;
    }
    snprintf(p, sizeof p, "/sys/dev/block/%s/../removable", majmin);
    if (read_small(p, v, sizeof v) && v[0] == '1')
    {
        return true;
    }
    snprintf(p, sizeof p, "/sys/dev/block/%s", majmin);
    char real[PATH_MAX];
    return realpath(p, real) && strstr(real, "/usb") != NULL;
}

/* The label of the block device source, from /dev/disk/by-label (udev
 * escapes a space as \x20 there), into buf; "" if it has none. */
static size_t device_label(const char *source, char *buf, size_t size)
{
    buf[0] = '\0';
    char dev[PATH_MAX];
    if (!realpath(source, dev))
    {
        return 0;
    }
    DIR *d = opendir("/dev/disk/by-label");
    if (!d)
    {
        return 0;
    }
    size_t len = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        char link[PATH_MAX], target[PATH_MAX];
        snprintf(link, sizeof link, "/dev/disk/by-label/%s", e->d_name);
        if (e->d_name[0] == '.' || !realpath(link, target)
            || strcmp(target, dev) != 0)
        {
            continue;
        }
        for (const char *s = e->d_name; *s && len + 1 < size;)
        {
            unsigned hex;
            if (s[0] == '\\' && s[1] == 'x' && sscanf(s + 2, "%2x", &hex) == 1)
            {
                buf[len++] = (char)hex;
                s += 4;
            }
            else
            {
                buf[len++] = *s++;
            }
        }
        buf[len] = '\0';
        break;
    }
    closedir(d);
    return len;
}

/* One line of /proc/self/mountinfo:
 *   id parent maj:min root mountpoint options [optional...] - type source
 * superoptions.  Appends it to out if it is a volume.  false: out of
 * memory. */
static bool mountinfo_line(char *line, allocator_t a, vec_t *out)
{
    char *field[6];
    char *save = NULL;
    for (int i = 0; i < 6; i++)
    {
        field[i] = strtok_r(i ? NULL : line, " ", &save);
        if (!field[i])
        {
            return true;
        }
    }
    char *t;
    while ((t = strtok_r(NULL, " ", &save)) != NULL && strcmp(t, "-") != 0)
    {
    }
    char *type = t ? strtok_r(NULL, " ", &save) : NULL;
    char *source = type ? strtok_r(NULL, " ", &save) : NULL;
    if (!source)
    {
        return true;
    }
    char *mount = field[4];
    size_t mount_len = unescape_octal(mount);
    unescape_octal(source);

    bool root = strcmp(mount, "/") == 0;
    bool network = in_list(type, network_fs);
    bool block = strncmp(source, "/dev/", 5) == 0 && !in_list(type, hidden_fs);
    if (!root && !network && !block)
    {
        return true; /* proc, sysfs, cgroup, tmpfs, overlay, ... */
    }

    const char *opts = field[5];
    fs_volume_t v = {0};
    v.network = network;
    v.read_only = strcmp(opts, "ro") == 0 || strncmp(opts, "ro,", 3) == 0;
    v.removable = block && is_removable(field[2]);
    char label[256];
    size_t label_len = block ? device_label(source, label, sizeof label) : 0;
    return push_volume(a, out, mount, mount_len, label, label_len, v);
}

static fs_err_t list_volumes(allocator_t a, vec_t *out)
{
    int fd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return filec_from_errno(errno);
    }
    char buf[64 * 1024];
    size_t have = 0;
    fs_err_t err = posix_ok;
    for (;;)
    {
        ssize_t n = read(fd, buf + have, sizeof buf - 1 - have);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n < 0)
        {
            err = filec_from_errno(errno);
            break;
        }
        have += (size_t)n;
        buf[have] = '\0';
        char *start = buf;
        char *nl;
        while ((nl = memchr(start, '\n', have - (size_t)(start - buf))))
        {
            *nl = '\0';
            if (!mountinfo_line(start, a, out))
            {
                err = (fs_err_t){FS_ERR_OOM, 0};
                break;
            }
            start = nl + 1;
        }
        if (!fs_ok(err) || n == 0)
        {
            break;
        }
        have -= (size_t)(start - buf);
        memmove(buf, start, have);
        if (have == sizeof buf - 1)
        {
            err = (fs_err_t){FS_ERR_IO, 0}; /* a line longer than buf */
            break;
        }
    }
    close(fd);
    return err;
}

#elif defined(__APPLE__)

static fs_err_t list_volumes(allocator_t a, vec_t *out)
{
    struct statfs *m = NULL;
    int n = getmntinfo(&m, MNT_NOWAIT);
    if (n <= 0)
    {
        return filec_from_errno(errno);
    }
    for (int i = 0; i < n; i++)
    {
        bool root = strcmp(m[i].f_mntonname, "/") == 0;
        if (!root
            && ((m[i].f_flags & MNT_DONTBROWSE)
                || strcmp(m[i].f_fstypename, "devfs") == 0
                || strcmp(m[i].f_fstypename, "autofs") == 0))
        {
            continue;
        }
        fs_volume_t v = {0};
        v.network = !(m[i].f_flags & MNT_LOCAL);
        v.read_only = (m[i].f_flags & MNT_RDONLY) != 0;
#ifdef MNT_REMOVABLE
        v.removable = (m[i].f_flags & MNT_REMOVABLE) != 0;
#endif
        const char *path = m[i].f_mntonname;
        /* /Volumes/Name: Finder shows the name */
        const char *label = strncmp(path, "/Volumes/", 9) == 0 ? path + 9 : "";
        if (!push_volume(a, out, path, strlen(path), label, strlen(label), v))
        {
            return (fs_err_t){FS_ERR_OOM, 0};
        }
    }
    return posix_ok;
}

#else

static fs_err_t list_volumes(allocator_t a, vec_t *out)
{
    fs_volume_t v = {0};
    return push_volume(a, out, "/", 1, "", 0, v) ? posix_ok
                                                 : (fs_err_t){FS_ERR_OOM, 0};
}

#endif

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
    char buf[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(path, buf, sizeof buf);
    if (!fs_ok(err))
    {
        return err;
    }
    struct statvfs st;
    if (statvfs(buf, &st) != 0)
    {
        return filec_from_errno(errno);
    }
    if (total)
    {
        *total = (uint64_t)st.f_blocks * st.f_frsize;
    }
    if (avail)
    {
        *avail = (uint64_t)st.f_bavail * st.f_frsize;
    }
    return posix_ok;
}

/* --- Watching (step 7, tests/fs_watch_test.c) ----------------------------- */

static bool wait_args_ok(
    fs_watch_t *w, fs_watch_event_t *out, size_t cap, size_t *count)
{
    if (count)
    {
        *count = 0;
    }
    return w && out && cap > 0 && count;
}

#if defined(__linux__)

/* inotify for the events, an eventfd for fs_watch_wake; one poll waits
 * for both.  The kernel hands over many events per read: what does not
 * fit into the caller's array stays in buf for the next fs_watch_wait,
 * and the names point into buf. */

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>

#define WATCH_MASK                                                             \
    (IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_MOVED_FROM             \
     | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR)

struct fs_watch_t
{
    allocator_t a;
    int ino;         /* inotify */
    int wake;        /* eventfd */
    size_t pos, len; /* unread events in buf */
    _Alignas(struct inotify_event) char buf[64 * 1024];
};

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
    w->a = a;
    w->pos = w->len = 0;
    w->ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    w->wake = w->ino < 0 ? -1 : eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (w->ino < 0 || w->wake < 0)
    {
        fs_err_t err = filec_from_errno(errno);
        if (w->ino >= 0)
        {
            close(w->ino);
        }
        mem_free(a, w, sizeof *w);
        return err;
    }
    *out = w;
    return posix_ok;
}

fs_err_t fs_watch_add(fs_watch_t *w, string_t dir, uint32_t *id)
{
    if (!w || !id)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    char buf[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(dir, buf, sizeof buf);
    if (!fs_ok(err))
    {
        return err;
    }
    int wd = inotify_add_watch(w->ino, buf, WATCH_MASK);
    if (wd < 0)
    {
        return filec_from_errno(errno); /* ENOTDIR from IN_ONLYDIR */
    }
    *id = (uint32_t)wd; /* watch descriptors start at 1 */
    return posix_ok;
}

void fs_watch_remove(fs_watch_t *w, uint32_t watch_id)
{
    if (w && watch_id != 0)
    {
        inotify_rm_watch(w->ino, (int)watch_id);
    }
}

/* The next unread event in buf as an fs_watch_event_t; false if it is
 * one to skip (IN_IGNORED, which follows a removal). */
static bool next_event(fs_watch_t *w, fs_watch_event_t *e)
{
    const struct inotify_event *ev =
        (const struct inotify_event *)(w->buf + w->pos);
    w->pos += sizeof *ev + ev->len;

    e->watch_id = (uint32_t)ev->wd;
    e->name = ev->len ? string_view_cstr(ev->name) : (string_t){"", 0};
    uint32_t m = ev->mask;
    if (m & IN_Q_OVERFLOW)
    {
        e->kind = FS_W_OVERFLOW;
        e->watch_id = 0;
    }
    else if (m & (IN_DELETE_SELF | IN_MOVE_SELF))
    {
        e->kind = FS_W_DELETED;
        e->name = (string_t){"", 0};
    }
    else if (m & IN_CREATE)
    {
        e->kind = FS_W_CREATED;
    }
    else if (m & IN_DELETE)
    {
        e->kind = FS_W_DELETED;
    }
    else if (m & (IN_MOVED_FROM | IN_MOVED_TO))
    {
        e->kind = FS_W_RENAMED;
    }
    else if (m & (IN_MODIFY | IN_ATTRIB))
    {
        e->kind = FS_W_MODIFIED;
    }
    else
    {
        return false; /* IN_IGNORED */
    }
    return true;
}

fs_err_t fs_watch_wait(
    fs_watch_t *w,
    fs_watch_event_t *out,
    size_t cap,
    uint32_t timeout_ms,
    size_t *count)
{
    if (!wait_args_ok(w, out, cap, count))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;)
    {
        while (w->pos < w->len && *count < cap)
        {
            if (next_event(w, &out[*count]))
            {
                (*count)++;
            }
        }
        if (*count > 0)
        {
            return posix_ok;
        }

        /* nothing unread: wait for the kernel, a wake, or the time */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t spent = (int64_t)(now.tv_sec - start.tv_sec) * 1000
                        + (now.tv_nsec - start.tv_nsec) / 1000000;
        int left = spent >= (int64_t)timeout_ms ? 0 : (int)(timeout_ms - spent);
        struct pollfd p[2] = {{w->ino, POLLIN, 0}, {w->wake, POLLIN, 0}};
        int r = poll(p, 2, left);
        if (r < 0 && errno == EINTR)
        {
            continue;
        }
        if (r < 0)
        {
            return filec_from_errno(errno);
        }
        if (p[1].revents & POLLIN)
        {
            uint64_t n;
            ssize_t got = read(w->wake, &n, sizeof n); /* reset to 0 */
            (void)got;
            return posix_ok;
        }
        if (r == 0)
        {
            return posix_ok; /* the time is up */
        }
        ssize_t n = read(w->ino, w->buf, sizeof w->buf);
        if (n < 0 && errno != EAGAIN && errno != EINTR)
        {
            return filec_from_errno(errno);
        }
        w->pos = 0;
        w->len = n > 0 ? (size_t)n : 0;
    }
}

void fs_watch_wake(fs_watch_t *w)
{
    if (w)
    {
        uint64_t one = 1;
        ssize_t n = write(w->wake, &one, sizeof one);
        (void)n;
    }
}

void fs_watch_destroy(fs_watch_t *w)
{
    if (w)
    {
        close(w->ino);
        close(w->wake);
        mem_free(w->a, w, sizeof *w);
    }
}

#elif defined(__APPLE__)

/* kqueue: one EVFILT_VNODE per watched directory (opened with
 * O_EVTONLY, so it does not keep a volume from being ejected), and an
 * EVFILT_USER event for fs_watch_wake.  kqueue says that a directory
 * changed, not what: FS_W_MODIFIED with the name "". */

#include <pthread.h>
#include <sys/event.h>

typedef struct
{
    int fd;
    uint32_t id;
} kq_watch_t;

struct fs_watch_t
{
    allocator_t a;
    int kq;
    pthread_mutex_t lock; /* watches, n, cap, next_id */
    kq_watch_t *watches;
    size_t n, cap;
    uint32_t next_id;
};

#define WAKE_IDENT 1

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
    memset(w, 0, sizeof *w);
    w->a = a;
    w->kq = kqueue();
    if (w->kq < 0)
    {
        fs_err_t err = filec_from_errno(errno);
        mem_free(a, w, sizeof *w);
        return err;
    }
    struct kevent ev;
    EV_SET(&ev, WAKE_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(w->kq, &ev, 1, NULL, 0, NULL) < 0)
    {
        fs_err_t err = filec_from_errno(errno);
        close(w->kq);
        mem_free(a, w, sizeof *w);
        return err;
    }
    pthread_mutex_init(&w->lock, NULL);
    *out = w;
    return posix_ok;
}

fs_err_t fs_watch_add(fs_watch_t *w, string_t dir, uint32_t *id)
{
    if (!w || !id)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    char buf[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(dir, buf, sizeof buf);
    if (!fs_ok(err))
    {
        return err;
    }
    int fd = open(buf, O_EVTONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return filec_from_errno(errno);
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        close(fd);
        return (fs_err_t){FS_ERR_NOT_DIR, ENOTDIR};
    }

    pthread_mutex_lock(&w->lock);
    if (w->n == w->cap)
    {
        size_t cap = w->cap ? 2 * w->cap : 8;
        kq_watch_t *grown = mem_realloc(
            w->a,
            w->watches,
            w->cap * sizeof *grown,
            cap * sizeof *grown,
            _Alignof(kq_watch_t));
        if (!grown)
        {
            pthread_mutex_unlock(&w->lock);
            close(fd);
            return (fs_err_t){FS_ERR_OOM, 0};
        }
        w->watches = grown;
        w->cap = cap;
    }
    uint32_t new_id = ++w->next_id;
    struct kevent ev;
    EV_SET(
        &ev,
        (uintptr_t)fd,
        EVFILT_VNODE,
        EV_ADD | EV_CLEAR,
        NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME
            | NOTE_REVOKE,
        0,
        (void *)(uintptr_t)new_id);
    if (kevent(w->kq, &ev, 1, NULL, 0, NULL) < 0)
    {
        err = filec_from_errno(errno);
        pthread_mutex_unlock(&w->lock);
        close(fd);
        return err;
    }
    w->watches[w->n++] = (kq_watch_t){fd, new_id};
    pthread_mutex_unlock(&w->lock);
    *id = new_id;
    return posix_ok;
}

void fs_watch_remove(fs_watch_t *w, uint32_t watch_id)
{
    if (!w)
    {
        return;
    }
    pthread_mutex_lock(&w->lock);
    for (size_t i = 0; i < w->n; i++)
    {
        if (w->watches[i].id == watch_id)
        {
            close(w->watches[i].fd); /* removes its kevent too */
            w->watches[i] = w->watches[--w->n];
            break;
        }
    }
    pthread_mutex_unlock(&w->lock);
}

fs_err_t fs_watch_wait(
    fs_watch_t *w,
    fs_watch_event_t *out,
    size_t cap,
    uint32_t timeout_ms,
    size_t *count)
{
    if (!wait_args_ok(w, out, cap, count))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }
    struct kevent evs[64];
    int max = cap < 64 ? (int)cap : 64;
    struct timespec ts = {
        (time_t)(timeout_ms / 1000), (long)(timeout_ms % 1000) * 1000000};
    int n;
    do
    {
        n = kevent(w->kq, NULL, 0, evs, max, &ts);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
    {
        return filec_from_errno(errno);
    }
    for (int i = 0; i < n; i++)
    {
        if (evs[i].filter == EVFILT_USER)
        {
            continue; /* a wake: count stays as it is */
        }
        fs_watch_event_t *e = &out[(*count)++];
        e->watch_id = (uint32_t)(uintptr_t)evs[i].udata;
        e->name = (string_t){"", 0};
        e->kind = (evs[i].fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE))
                      ? FS_W_DELETED
                      : FS_W_MODIFIED;
    }
    return posix_ok;
}

void fs_watch_wake(fs_watch_t *w)
{
    if (w)
    {
        struct kevent ev;
        EV_SET(&ev, WAKE_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
        kevent(w->kq, &ev, 1, NULL, 0, NULL);
    }
}

void fs_watch_destroy(fs_watch_t *w)
{
    if (w)
    {
        for (size_t i = 0; i < w->n; i++)
        {
            close(w->watches[i].fd);
        }
        mem_free(w->a, w->watches, w->cap * sizeof *w->watches);
        close(w->kq);
        pthread_mutex_destroy(&w->lock);
        mem_free(w->a, w, sizeof *w);
    }
}

#else

fs_err_t fs_watch_create(allocator_t a, fs_watch_t **out)
{
    (void)a;
    if (out)
    {
        *out = NULL;
    }
    return (fs_err_t){FS_ERR_UNSUPPORTED, 0};
}

fs_err_t fs_watch_add(fs_watch_t *w, string_t dir, uint32_t *id)
{
    (void)w;
    (void)dir;
    (void)id;
    return (fs_err_t){FS_ERR_UNSUPPORTED, 0};
}

void fs_watch_remove(fs_watch_t *w, uint32_t watch_id)
{
    (void)w;
    (void)watch_id;
}

fs_err_t fs_watch_wait(
    fs_watch_t *w,
    fs_watch_event_t *out,
    size_t cap,
    uint32_t timeout_ms,
    size_t *count)
{
    (void)w;
    (void)out;
    (void)cap;
    (void)timeout_ms;
    if (count)
    {
        *count = 0;
    }
    return (fs_err_t){FS_ERR_UNSUPPORTED, 0};
}

void fs_watch_wake(fs_watch_t *w)
{
    (void)w;
}

void fs_watch_destroy(fs_watch_t *w)
{
    (void)w;
}

#endif

/* --- Trash (step 8, tests/fs_trash_test.c) ------------------------------- */

/* snprintf's result r into buf fitted (no truncation). */
#define FITS_N(r, n) ((r) >= 0 && (size_t)(r) < (n))
#define FITS(r, buf) FITS_N(r, sizeof(buf))

/* mkdir -p for a native path, the last level with mode. */
static bool make_dirs(const char *path, mode_t mode)
{
    char buf[FILEC_PATH_BUF_MAX * 2];
    size_t n = (size_t)snprintf(buf, sizeof buf, "%s", path);
    if (n >= sizeof buf)
    {
        return false;
    }
    for (size_t i = 1; i <= n; i++)
    {
        if (buf[i] == '/' || buf[i] == '\0')
        {
            char c = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, i == n ? mode : 0777) != 0 && errno != EEXIST)
            {
                return false;
            }
            buf[i] = c;
        }
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The directory where the volume of dev starts, walking up from dir. */
static bool volume_top(const char *dir, dev_t dev, char *top, size_t size)
{
    char cur[PATH_MAX];
    if (!realpath(dir, cur))
    {
        return false;
    }
    for (;;)
    {
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur)
        {
            struct stat st;
            if (stat("/", &st) == 0 && st.st_dev == dev)
            {
                snprintf(top, size, "/");
                return true;
            }
            snprintf(top, size, "%s", cur);
            return true;
        }
        *slash = '\0';
        struct stat st;
        if (stat(cur, &st) != 0 || st.st_dev != dev)
        {
            *slash = '/';
            snprintf(top, size, "%s", cur);
            return true;
        }
    }
}

/* The trash for something on device dev in directory parent: the home
 * trash if it is on that device, else one at the top of its volume.
 * files and info are set (info is "" on macOS, which keeps none);
 * *topdir is set for a top-of-volume trash (its .trashinfo paths are
 * relative to it), "" for the home trash. */
static fs_err_t find_trash(
    const char *parent,
    dev_t dev,
    char files[FILEC_PATH_BUF_MAX],
    char info[FILEC_PATH_BUF_MAX],
    char topdir[FILEC_PATH_BUF_MAX])
{
    char home[FILEC_PATH_BUF_MAX];
    fs_err_t err = home_dir(home, sizeof home);
    char base[FILEC_PATH_BUF_MAX];
    topdir[0] = '\0';
    info[0] = '\0';
    struct stat st;
#ifdef __APPLE__
    if (fs_ok(err))
    {
        snprintf(files, FILEC_PATH_BUF_MAX, "%s/.Trash", home);
        if (make_dirs(files, 0700) && stat(files, &st) == 0 && st.st_dev == dev)
        {
            return posix_ok;
        }
    }
    if (!volume_top(parent, dev, topdir, FILEC_PATH_BUF_MAX))
    {
        return (fs_err_t){FS_ERR_UNSUPPORTED, 0};
    }
    snprintf(
        files,
        FILEC_PATH_BUF_MAX,
        "%s/.Trashes/%ld",
        strcmp(topdir, "/") ? topdir : "",
        (long)getuid());
    topdir[0] = '\0';
    return make_dirs(files, 0700) ? posix_ok
                                  : (fs_err_t){FS_ERR_UNSUPPORTED, 0};
#else
    /* the home trash: $XDG_DATA_HOME/Trash */
    const char *xdg = absolute_env("XDG_DATA_HOME");
    if (xdg || fs_ok(err))
    {
        bool fits =
            (xdg ? FITS(snprintf(base, sizeof base, "%s/Trash", xdg), base)
                 : FITS(
                       snprintf(
                           base, sizeof base, "%s/.local/share/Trash", home),
                       base))
            && FITS_N(
                snprintf(files, FILEC_PATH_BUF_MAX, "%s/files", base),
                FILEC_PATH_BUF_MAX)
            && FITS_N(
                snprintf(info, FILEC_PATH_BUF_MAX, "%s/info", base),
                FILEC_PATH_BUF_MAX);
        if (fits && make_dirs(base, 0700) && make_dirs(files, 0700)
            && make_dirs(info, 0700) && stat(base, &st) == 0
            && st.st_dev == dev)
        {
            return posix_ok;
        }
    }

    /* a trash at the top of the volume: $topdir/.Trash/$uid (made by an
     * administrator: a real directory with the sticky bit), else
     * $topdir/.Trash-$uid */
    if (!volume_top(parent, dev, topdir, FILEC_PATH_BUF_MAX))
    {
        return (fs_err_t){FS_ERR_UNSUPPORTED, 0};
    }
    const char *t = strcmp(topdir, "/") ? topdir : "";
    long uid = (long)getuid();
    char shared[FILEC_PATH_BUF_MAX];
    snprintf(shared, sizeof shared, "%s/.Trash", t);
    if (lstat(shared, &st) == 0 && S_ISDIR(st.st_mode)
        && (st.st_mode & S_ISVTX))
    {
        snprintf(base, sizeof base, "%s/.Trash/%ld", t, uid);
    }
    else
    {
        snprintf(base, sizeof base, "%s/.Trash-%ld", t, uid);
    }
    bool fits = FITS_N(
                    snprintf(files, FILEC_PATH_BUF_MAX, "%s/files", base),
                    FILEC_PATH_BUF_MAX)
                && FITS_N(
                    snprintf(info, FILEC_PATH_BUF_MAX, "%s/info", base),
                    FILEC_PATH_BUF_MAX);
    if (fits && make_dirs(base, 0700) && make_dirs(files, 0700)
        && make_dirs(info, 0700) && stat(base, &st) == 0 && st.st_dev == dev)
    {
        return posix_ok;
    }
    return (fs_err_t){FS_ERR_UNSUPPORTED, 0}; /* never copy across */
#endif
}

/* path percent-encoded for a .trashinfo (RFC 2396: '/' and the
 * unreserved characters stay). */
static bool url_encode(const char *path, char *out, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
    {
        bool keep = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
                    || (*p >= '0' && *p <= '9') || strchr("/-_.~", *p);
        if (n + 4 > size)
        {
            return false;
        }
        if (keep)
        {
            out[n++] = (char)*p;
        }
        else
        {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    out[n] = '\0';
    return true;
}

fs_err_t fs_trash(string_t path)
{
    char buf[FILEC_PATH_BUF_MAX];
    fs_err_t err = filec_path_cstr(path, buf, sizeof buf);
    if (!fs_ok(err))
    {
        return err;
    }
    struct stat st;
    if (lstat(buf, &st) != 0)
    {
        return filec_from_errno(errno);
    }
    string_t name = path_file_name(path);
    if (name.len == 0)
    {
        return (fs_err_t){FS_ERR_INVALID, 0}; /* "/" */
    }
    char parent[FILEC_PATH_BUF_MAX];
    snprintf(parent, sizeof parent, "%.*s", (int)(path.len - name.len), buf);

    char files[FILEC_PATH_BUF_MAX], info[FILEC_PATH_BUF_MAX];
    char topdir[FILEC_PATH_BUF_MAX];
    err = find_trash(parent, st.st_dev, files, info, topdir);
    if (!fs_ok(err))
    {
        return err;
    }

    /* A free name: name, name.2, name.3, ... — claimed by creating its
     * .trashinfo with O_EXCL (on macOS: by the rename itself). */
    char dest[2 * FILEC_PATH_BUF_MAX + 32];
    char info_path[2 * FILEC_PATH_BUF_MAX + 64];
    for (int i = 1;; i++)
    {
        if (i == 10000)
        {
            return (fs_err_t){FS_ERR_EXISTS, 0};
        }
        char suffix[16] = "";
        if (i > 1)
        {
#ifdef __APPLE__
            snprintf(suffix, sizeof suffix, " %d", i);
#else
            snprintf(suffix, sizeof suffix, ".%d", i);
#endif
        }
        snprintf(
            dest,
            sizeof dest,
            "%s/%.*s%s",
            files,
            (int)name.len,
            name.ptr,
            suffix);
        struct stat there;
        if (lstat(dest, &there) == 0)
        {
            continue;
        }
        if (info[0] == '\0')
        {
            break;
        }
        snprintf(
            info_path,
            sizeof info_path,
            "%s/%.*s%s.trashinfo",
            info,
            (int)name.len,
            name.ptr,
            suffix);
        int fd = open(info_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno == EEXIST)
        {
            continue;
        }
        if (fd < 0)
        {
            return filec_from_errno(errno);
        }
        /* where it was (relative to the top of its volume for a trash
         * there), and when */
        const char *where = buf;
        size_t tl = strlen(topdir);
        if (tl > 0)
        {
            where = buf + tl + (strcmp(topdir, "/") ? 1 : 0);
        }
        char encoded[3 * FILEC_PATH_BUF_MAX + 1];
        char date[32];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
        char text[4 * FILEC_PATH_BUF_MAX];
        int len = url_encode(where, encoded, sizeof encoded)
                      ? snprintf(
                            text,
                            sizeof text,
                            "[Trash Info]\nPath=%s\nDeletionDate=%s\n",
                            encoded,
                            date)
                      : -1;
        bool written = len > 0 && write(fd, text, (size_t)len) == len;
        close(fd);
        if (!written)
        {
            unlink(info_path);
            return (fs_err_t){FS_ERR_IO, 0};
        }
        break;
    }

    if (rename(buf, dest) != 0)
    {
        err = filec_from_errno(errno);
        if (info[0])
        {
            unlink(info_path);
        }
        return err;
    }
    return posix_ok;
}
