#include "arena/fixed_arena.h"
#include "filec/fs.h"
#include "filec/path.h"
#include "fs_internal.h"
#include "seqc/string.h"

enum
{
    READ_CHUNK = 64 * 1024
};

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

fs_err_t fs_read_all(string_t path, strbuf_t *out)
{
    if (!out)
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    fs_file_t f;
    fs_err_t err = fs_open(path, FS_READ, &f);
    if (!fs_ok(err))
    {
        return err;
    }

    uint64_t offset = 0;
    char buf[READ_CHUNK];

    size_t out_pos = strbuf_len(out);

    while (true)
    {
        size_t got;
        err = fs_read_at(f, offset, buf, READ_CHUNK, &got);
        if (!fs_ok(err))
        {
            fs_close(f);
            strbuf_truncate(out, out_pos);
            return err;
        }
        if (got == 0)
        {
            break;
        }
        if (strbuf_append(out, (string_t){buf, got}) != SEQC_OK)
        {
            fs_close(f);
            strbuf_truncate(out, out_pos);
            return (fs_err_t){FS_ERR_OOM, 0};
        }
        if (got < READ_CHUNK)
        {
            break;
        }
        offset += got;
    }
    fs_close(f);
    return (fs_err_t){FS_OK, 0};
}