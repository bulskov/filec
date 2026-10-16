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

fs_err_t fs_dir_list(string_t path, uint32_t want, allocator_t a, vec_t *out)
{
    if (!out || vec_elem_size(out) != sizeof(fs_entry_t))
    {
        return (fs_err_t){FS_ERR_INVALID, 0};
    }

    filec_dir_t *dir;
    fs_err_t err = filec_dir_open(path, want, a, &dir);
    if (!fs_ok(err))
    {
        return err;
    }

    bool done = false;
    size_t mark = vec_len(out);
    for (;;)
    {
        fs_entry_t e;
        err = filec_dir_next(dir, &e, &done);
        if (!fs_ok(err) || done)
        {
            break;
        }
        char *name = mem_alloc(a, e.name.len ? e.name.len : 1, 1);
        if (!name)
        {
            err = (fs_err_t){FS_ERR_OOM, 0};
            break;
        }
        memcpy(name, e.name.ptr, e.name.len);
        e.name = (string_t){name, e.name.len};
        if (vec_push(out, &e) != SEQC_OK)
        {
            err = (fs_err_t){FS_ERR_OOM, 0};
            break;
        }
    }
    filec_dir_close(dir);
    if (!fs_ok(err))
    {
        vec_truncate(out, mark);
    }
    return err;
}

struct fs_dir_iter_state
{
    filec_dir_t *dir;
    fs_err_t *err;
    bool done;
};

static bool fs_dir_iter_next(iter_t *iter, void *out)
{
    struct fs_dir_iter_state *state = iter->state;
    if (state->done)
    {
        return false;
    }

    fs_entry_t *entry = out;

    fs_err_t next_err = filec_dir_next(state->dir, entry, &state->done);
    if (!fs_ok(next_err) || state->done)
    {
        if (state->err)
        {
            *state->err = next_err;
        }
        state->done = true;
        return false;
    }
    return true;
}

static void fs_dir_iter_destroy(iter_t *it)
{
    struct fs_dir_iter_state *state = it->state;
    filec_dir_close(state->dir);
    mem_free(it->allocator, state, sizeof(struct fs_dir_iter_state));
}

iter_t fs_dir_iter(string_t path, uint32_t want, allocator_t a, fs_err_t *err)
{
    filec_dir_t *dir;
    fs_err_t open_err = filec_dir_open(path, want, a, &dir);
    if (!fs_ok(open_err))
    {
        if (err)
        {
            *err = open_err;
        }

        return (iter_t){0};
    }

    struct fs_dir_iter_state *state = mem_alloc(
        a,
        sizeof(struct fs_dir_iter_state),
        _Alignof(struct fs_dir_iter_state));
    if (!state)
    {
        if (err)
        {
            *err = (fs_err_t){FS_ERR_OOM, 0};
        }
        filec_dir_close(dir);
        return (iter_t){0};
    }
    if (err)
    {
        *err = (fs_err_t){FS_OK, 0};
    }
    state->dir = dir;
    state->done = false;
    state->err = err;
    return (iter_t){
        .next = fs_dir_iter_next,
        .destroy = fs_dir_iter_destroy,
        .state = state,
        .elem_size = sizeof(fs_entry_t),
        .allocator = a};
}
