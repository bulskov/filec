/* filec/fs step 6: fs_volumes, fs_space.
 *
 * What volumes a machine has differs from machine to machine, so these
 * tests check what must hold everywhere: the root and the volume holding
 * the test directory are listed, every path is canonical, the list is
 * appended to the caller's vec with the strings from the caller's
 * allocator, and nothing changes on failure. */

#include "fs_fixture.h" /* first: it sets _XOPEN_SOURCE on POSIX */

#include "ctt.h"
#include "filec/fs.h"
#include "filec/path.h"

#include "arena/growing_arena.h"
#include "seqc/vec.h"

static growing_arena_t arena;
static allocator_t a;
static vec_t *vols;

void ctt_before_each(void)
{
    fx_setup();
    growing_arena_init(&arena, 16 * 1024);
    a = growing_arena_allocator(&arena);
    vols = vec_create(sizeof(fs_volume_t), a);
}

void ctt_after_each(void)
{
    growing_arena_destroy(&arena);
    fx_teardown();
}

#define P(lit) STRING_LIT(lit)
#define KIND(call) ((call).kind)

static const char *c(string_t s)
{
    static char bufs[4][8192];
    static int next;
    char *b = bufs[next++ % 4];
    size_t n = s.len < sizeof bufs[0] - 1 ? s.len : sizeof bufs[0] - 1;
    if (n > 0)
    {
        memcpy(b, s.ptr, n);
    }
    b[n] = '\0';
    return b;
}

static const fs_volume_t *vol(size_t i)
{
    return vec_get_ptr(vols, i);
}

static bool is_canonical(string_t p)
{
    if (p.len == 0 || p.ptr[0] != '/')
    {
        return false;
    }
    growing_arena_t t;
    growing_arena_init(&t, 4096);
    strbuf_t *n = strbuf_create(growing_arena_allocator(&t));
    bool same =
        path_normalize(p, n) == PATH_OK && string_equals(strbuf_view(n), p);
    growing_arena_destroy(&t);
    return same;
}

/* =========================================================================
 * fs_volumes
 * ========================================================================= */

TEST(volumes_lists_something_canonical)
{
    ASSERT_EQ(FS_OK, KIND(fs_volumes(a, vols)));
    ASSERT_TRUE(vec_len(vols) >= 1);
    for (size_t i = 0; i < vec_len(vols); i++)
    {
        ASSERT_TRUE(is_canonical(vol(i)->path));
        ASSERT_TRUE(vol(i)->label.len == 0 || vol(i)->label.ptr != NULL);
    }
}

/* The volume the test directory is on is listed: some volume's path is a
 * prefix of the test directory's real path. */
TEST(volumes_include_the_one_we_are_on)
{
    growing_arena_t t;
    growing_arena_init(&t, 4096);
    strbuf_t *here = strbuf_create(growing_arena_allocator(&t));
    ASSERT_EQ(FS_OK, KIND(fs_real_path(FX(""), here)));
    ASSERT_EQ(FS_OK, KIND(fs_volumes(a, vols)));
    bool found = false;
    for (size_t i = 0; i < vec_len(vols); i++)
    {
        found = found
                || path_starts_with(
                    strbuf_view(here), vol(i)->path, PATH_CASE_NATIVE);
    }
    ASSERT_TRUE(found);
    growing_arena_destroy(&t);
}

#ifndef _WIN32
TEST(volumes_include_the_root)
{
    ASSERT_EQ(FS_OK, KIND(fs_volumes(a, vols)));
    int roots = 0;
    for (size_t i = 0; i < vec_len(vols); i++)
    {
        roots += string_equals(vol(i)->path, P("/"));
    }
    ASSERT_EQ(1, roots);
}
#endif

#if defined(__linux__)
/* Pseudo-filesystems are not volumes. */
TEST(volumes_leave_out_pseudo_filesystems)
{
    ASSERT_EQ(FS_OK, KIND(fs_volumes(a, vols)));
    const char *pseudo[] = {"/proc", "/sys", "/dev", "/run", "/sys/fs/cgroup"};
    for (size_t i = 0; i < vec_len(vols); i++)
    {
        for (size_t j = 0; j < sizeof pseudo / sizeof pseudo[0]; j++)
        {
            ASSERT_FALSE(
                string_equals(vol(i)->path, string_view_cstr(pseudo[j])));
        }
    }
}
#endif

TEST(volumes_append_to_out)
{
    fs_volume_t mine = {.path = P("/mine")};
    ASSERT_EQ(SEQC_OK, vec_push(vols, &mine));
    ASSERT_EQ(FS_OK, KIND(fs_volumes(a, vols)));
    ASSERT_TRUE(vec_len(vols) >= 2);
    ASSERT_STR_EQ("/mine", c(vol(0)->path));
}

/* The strings live as long as a — not as long as some internal buffer:
 * a second call does not change the first one's. */
TEST(volumes_strings_live_as_long_as_a)
{
    ASSERT_EQ(FS_OK, KIND(fs_volumes(a, vols)));
    size_t n = vec_len(vols);
    char first[8192];
    snprintf(first, sizeof first, "%s", c(vol(0)->path));
    ASSERT_EQ(FS_OK, KIND(fs_volumes(a, vols)));
    ASSERT_EQ(2 * n, vec_len(vols));
    ASSERT_STR_EQ(first, c(vol(0)->path));
    ASSERT_STR_EQ(first, c(vol(n)->path));
}

TEST(volumes_errors)
{
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_volumes(a, NULL)));
    vec_t *ints = vec_create(sizeof(int), a);
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_volumes(a, ints)));
    ASSERT_EQ(0u, vec_len(ints));
}

/* a with a budget: n allocations succeed (from the arena, so nothing
 * leaks), then every one fails. */
typedef struct
{
    size_t budget;
    allocator_t inner;
} limited_t;

static void *limited_alloc(void *ctx, size_t size, size_t align)
{
    limited_t *l = ctx;
    if (l->budget == 0)
    {
        return NULL;
    }
    l->budget--;
    return mem_alloc(l->inner, size, align);
}

static void *limited_realloc(
    void *ctx, void *ptr, size_t old_size, size_t new_size, size_t align)
{
    limited_t *l = ctx;
    if (l->budget == 0)
    {
        return NULL;
    }
    l->budget--;
    return mem_realloc(l->inner, ptr, old_size, new_size, align);
}

static void limited_free(void *ctx, void *ptr, size_t size)
{
    limited_t *l = ctx;
    mem_free(l->inner, ptr, size);
}

static const allocator_vtable_t limited_vtable = {
    limited_alloc, limited_realloc, limited_free};

/* a running out at any point: FS_ERR_OOM, out as it was. */
TEST(volumes_out_of_memory_leaves_out_unchanged)
{
    fs_volume_t mine = {.path = P("/mine")};
    ASSERT_EQ(SEQC_OK, vec_push(vols, &mine));
    for (size_t n = 0;; n++)
    {
        limited_t l = {n, a};
        fs_err_t err = fs_volumes((allocator_t){&limited_vtable, &l}, vols);
        if (fs_ok(err))
        {
            ASSERT_TRUE(vec_len(vols) >= 2);
            break;
        }
        ASSERT_EQ(FS_ERR_OOM, err.kind);
        ASSERT_EQ(1u, vec_len(vols));
        ASSERT_TRUE(n < 10000);
    }
}

/* =========================================================================
 * fs_space
 * ========================================================================= */

TEST(space_of_the_test_directory)
{
    uint64_t total = 0, avail = UINT64_MAX;
    ASSERT_EQ(FS_OK, KIND(fs_space(FX(""), &total, &avail)));
    ASSERT_TRUE(total > 0);
    ASSERT_TRUE(avail <= total);
}

/* A file is on a volume too; either result may be left out. */
TEST(space_of_a_file_and_one_result)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    uint64_t total = 0, avail = 0;
    ASSERT_EQ(FS_OK, KIND(fs_space(FX("f"), &total, NULL)));
    ASSERT_TRUE(total > 0);
    ASSERT_EQ(FS_OK, KIND(fs_space(FX("f"), NULL, &avail)));
    ASSERT_TRUE(avail <= total);
}

TEST(space_errors)
{
    uint64_t total, avail;
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_space(FX("missing"), &total, &avail)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_space(P("relative"), &total, &avail)));
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "filec fs volumes");
}
