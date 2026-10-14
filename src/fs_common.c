#include "arena/fixed_arena.h"
#include "filec/path.h"
#include "fs_internal.h"
#include "seqc/string.h"
#include <assert.h>

/* --- Step 1 stubs: replace with the real thing ---------------------------
 * (tests/fs_basic_test.c).  They exist so the tests build and fail. */

string_t fs_err_str(fs_err_t e)
{
    switch (e.kind)
    {
    case FS_OK:
        return STRING_LIT("ok");
    case FS_ERR_NOT_FOUND:
        return STRING_LIT("not found");
    case FS_ERR_ACCESS:
        return STRING_LIT("access denied");
    case FS_ERR_EXISTS:
        return STRING_LIT("already exists");
    case FS_ERR_NOT_EMPTY:
        return STRING_LIT("not empty");
    case FS_ERR_NOT_DIR:
        return STRING_LIT("not a directory");
    case FS_ERR_IS_DIR:
        return STRING_LIT("is a directory");
    case FS_ERR_CROSS_DEVICE:
        return STRING_LIT("cross device");
    case FS_ERR_NO_SPACE:
        return STRING_LIT("no space");
    case FS_ERR_NAME_TOO_LONG:
        return STRING_LIT("name too long");
    case FS_ERR_BUSY:
        return STRING_LIT("busy");
    case FS_ERR_READ_ONLY:
        return STRING_LIT("read only");
    case FS_ERR_LOOP:
        return STRING_LIT("loop");
    case FS_ERR_CANCELLED:
        return STRING_LIT("cancelled");
    case FS_ERR_UNSUPPORTED:
        return STRING_LIT("unsupported");
    case FS_ERR_OOM:
        return STRING_LIT("out of memory");
    case FS_ERR_INVALID:
        return STRING_LIT("invalid");
    case FS_ERR_IO:
        return STRING_LIT("I/O error");
    default:
        return STRING_LIT("?");
    }
    return STRING_LIT("?");
}

fs_err_t filec_path_cstr(string_t path, char *buf, size_t buf_size)
{

    if (!path_is_absolute(path))
    {
        return (fs_err_t){.kind = FS_ERR_INVALID, 0};
    }
    if (string_find_char(path, '\0') != STRING_NOT_FOUND)
    {
        return (fs_err_t){.kind = FS_ERR_INVALID, 0};
    }
    if (path.len > FS_PATH_MAX)
    {
        return (fs_err_t){.kind = FS_ERR_NAME_TOO_LONG, 0};
    }

    fixed_arena_t arena;
    fixed_arena_init(&arena, buf, buf_size);
    const char *cstr = string_to_cstr(path, fixed_arena_allocator(&arena));
    // this should never happen
    if (cstr == NULL)
    {
        return (fs_err_t){.kind = FS_ERR_OOM, 0};
    }
    return (fs_err_t){.kind = FS_OK, 0};
}