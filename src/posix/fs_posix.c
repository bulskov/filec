/* filec/fs — POSIX backend (Linux first: getdents64, statx, inotify,
 * copy_file_range, renameat2; macOS later with fallbacks).
 */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE /* macOS: st_mtimespec, st_birthtimespec */
#define _DEFAULT_SOURCE

#include "filec/fs.h"
#include "filec/path.h"
#include <stdint.h>

#include "../fs_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
