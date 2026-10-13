/* filec/fs — Win32 backend (FindFirstFileExW, ReadDirectoryChangesW,
 * CopyFileExW, MoveFileExW, IFileOperation for the Recycle Bin).
 *
 * Empty scaffold: the implementation of include/filec/fs.h goes here. */

#include "filec/fs.h"

/* --- Step 1 stubs: replace with the real thing ---------------------------
 * (tests/fs_basic_test.c).  They exist so the tests build and fail. */

fs_err_t fs_mkdir(string_t path)
{
    (void)path;
    return (fs_err_t){FS_ERR_UNSUPPORTED, 0};
}

fs_err_t fs_remove(string_t path)
{
    (void)path;
    return (fs_err_t){FS_ERR_UNSUPPORTED, 0};
}

fs_err_t fs_stat(string_t path, uint32_t want, uint32_t flags, fs_entry_t *out)
{
    (void)path;
    (void)want;
    (void)flags;
    (void)out;
    return (fs_err_t){FS_ERR_UNSUPPORTED, 0};
}

bool fs_exists(string_t path)
{
    (void)path;
    return false;
}
