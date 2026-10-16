/* filec/fs step 3: reading directories — fs_dir_list (the whole listing,
 * names in the caller's arena) and fs_dir_iter (a stream, names valid
 * until the next step).
 *
 * Every test gets a fresh temporary directory (tests/fs_fixture.h); the
 * entries are made with OS calls, and what a listing says is checked
 * against fs_stat (step 1) and against the fixture's own count.  FX("a")
 * is the canonical path of a inside it.
 *
 * Order is unspecified, so the tests look names up (find, occurrences). */

#include "fs_fixture.h" /* first: it sets _XOPEN_SOURCE on POSIX */

#include "ctt.h"
#include "filec/fs.h"

#include "arena/fixed_arena.h"
#include "arena/growing_arena.h"
#include "seqc/vec.h"

/* Every test: entries (a vec of fs_entry_t) and names (what fs_dir_list
 * allocates them from) in two growing arenas. */
static growing_arena_t entries_arena, names_arena;
static vec_t *entries;
static allocator_t names;

/* An allocator that counts what is still allocated and fails once its
 * budget is spent: for the iterator's state, which must go back. */
typedef struct
{
    size_t budget;
    int live;
} counting_t;

static counting_t cnt;

static void *count_alloc(void *ctx, size_t size, size_t align)
{
    counting_t *c = ctx;
    if (c->budget == 0)
    {
        return NULL;
    }
    c->budget--;
    void *p = heap_alloc(NULL, size, align);
    c->live += p != NULL;
    return p;
}

static void *count_realloc(
    void *ctx, void *ptr, size_t old_size, size_t new_size, size_t align)
{
    counting_t *c = ctx;
    if (c->budget == 0)
    {
        return NULL;
    }
    c->budget--;
    void *p = heap_realloc(NULL, ptr, old_size, new_size, align);
    c->live += p != NULL && ptr == NULL;
    return p;
}

static void count_free(void *ctx, void *ptr, size_t size)
{
    counting_t *c = ctx;
    c->live -= ptr != NULL;
    heap_free(NULL, ptr, size);
}

static const allocator_vtable_t count_vtable = {
    count_alloc, count_realloc, count_free};

static allocator_t counted(void)
{
    return (allocator_t){&count_vtable, &cnt};
}

void ctt_before_each(void)
{
    fx_setup();
    growing_arena_init(&entries_arena, 16 * 1024);
    growing_arena_init(&names_arena, 16 * 1024);
    entries =
        vec_create(sizeof(fs_entry_t), growing_arena_allocator(&entries_arena));
    names = growing_arena_allocator(&names_arena);
    cnt = (counting_t){(size_t)-1, 0};
}

void ctt_after_each(void)
{
    growing_arena_destroy(&names_arena);
    growing_arena_destroy(&entries_arena);
    fx_teardown();
}

#define P(lit) STRING_LIT(lit)
#define KIND(call) ((call).kind)

/* string_t -> C string, in rotating static buffers. */
static const char *c(string_t s)
{
    static char bufs[4][1024];
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

static bool name_is(string_t name, const char *s)
{
    return name.len == strlen(s) && memcmp(name.ptr, s, name.len) == 0;
}

/* fs_dir_list of rel into entries. */
static fs_err_t list(const char *rel, uint32_t want)
{
    return fs_dir_list(FX(rel), want, names, entries);
}

static size_t n_entries(void)
{
    return vec_len(entries);
}

static const fs_entry_t *entry(size_t i)
{
    return vec_get_ptr(entries, i);
}

static int occurrences(const char *name)
{
    int n = 0;
    for (size_t i = 0; i < n_entries(); i++)
    {
        n += name_is(entry(i)->name, name);
    }
    return n;
}

/* The entry called name; fails the test unless it is there exactly once. */
static const fs_entry_t *find(const char *name)
{
    CTT_ASSERT_EQ(1, occurrences(name));
    for (size_t i = 0; i < n_entries(); i++)
    {
        if (name_is(entry(i)->name, name))
        {
            return entry(i);
        }
    }
    return NULL;
}

static void make_files(int n)
{
    char name[32];
    for (int i = 0; i < n; i++)
    {
        snprintf(name, sizeof name, "f%03d", i);
        CTT_ASSERT_TRUE(fx_write_file(name, "x"));
    }
}

/* Every f000..f(n-1) is in entries exactly once. */
static void expect_files(int n)
{
    char name[32];
    for (int i = 0; i < n; i++)
    {
        snprintf(name, sizeof name, "f%03d", i);
        CTT_ASSERT_EQ(1, occurrences(name));
    }
}

/* --- What a listing contains --------------------------------------------- */

TEST(list_of_an_empty_directory_is_empty)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_EQ(FS_OK, KIND(list("d", 0)));
    ASSERT_EQ(0u, n_entries()); /* no "." and no ".." */
}

TEST(list_has_files_and_directories)
{
    ASSERT_TRUE(fx_write_file("a.txt", "hello"));
    ASSERT_TRUE(fx_write_file("b", ""));
    ASSERT_TRUE(fx_mkdir("sub"));
    ASSERT_TRUE(fx_write_file("sub/inner", "x")); /* not listed: deeper */

    ASSERT_EQ(FS_OK, KIND(list("", FS_F_KIND)));
    ASSERT_EQ(3u, n_entries());
    ASSERT_EQ(fx_count_entries(""), (int)n_entries());
    ASSERT_EQ(FS_KIND_FILE, find("a.txt")->kind);
    ASSERT_EQ(FS_KIND_FILE, find("b")->kind);
    ASSERT_EQ(FS_KIND_DIR, find("sub")->kind);
    ASSERT_EQ(0, occurrences("."));
    ASSERT_EQ(0, occurrences(".."));
    ASSERT_EQ(0, occurrences("inner"));
}

/* Hidden entries are listed — hiding them is the caller's choice — and
 * flagged as fs_stat flags them. */
TEST(list_has_hidden_entries)
{
#ifdef _WIN32
    const char *hidden = "secret";
    ASSERT_TRUE(fx_write_file(hidden, "x"));
    ASSERT_TRUE(fx_set_hidden(hidden));
#else
    const char *hidden = ".secret";
    ASSERT_TRUE(fx_write_file(hidden, "x"));
#endif
    ASSERT_TRUE(fx_write_file("plain", "x"));

    ASSERT_EQ(FS_OK, KIND(list("", FS_F_FLAGS)));
    ASSERT_EQ(2u, n_entries());
    const fs_entry_t *h = find(hidden);
    ASSERT_TRUE(h->valid & FS_F_FLAGS);
    ASSERT_EQ((uint32_t)FS_FLAG_HIDDEN, h->flags & FS_FLAG_HIDDEN);
    ASSERT_EQ(0u, find("plain")->flags & FS_FLAG_HIDDEN);
}

/* Names come back as they are: UTF-8, spaces, long. */
TEST(list_names_come_back_exactly)
{
    char long_name[151];
    memset(long_name, 'n', 150); /* 150: the fixture's own Windows calls
                                    stop at MAX_PATH (260) in all */
    long_name[150] = '\0';
    const char *odd[] = {
        "\xc3\xa6\xc3\xb8\xc3\xa5.txt", /* æøå.txt */
        "\xe6\x97\xa5\xe6\x9c\xac",     /* 日本 */
        "with space",
        long_name,
#ifndef _WIN32
        "trailing.dot.", /* Win32 strips the dot: the fixture can't make it */
#endif
    };
    size_t n = sizeof odd / sizeof odd[0];
    for (size_t i = 0; i < n; i++)
    {
        ASSERT_TRUE(fx_write_file(odd[i], "x"));
    }
    ASSERT_EQ(FS_OK, KIND(list("", 0)));
    ASSERT_EQ(n, n_entries());
    for (size_t i = 0; i < n; i++)
    {
        ASSERT_EQ(1, occurrences(odd[i]));
    }
}

TEST(list_of_a_large_directory)
{
    make_files(300);
    ASSERT_EQ(FS_OK, KIND(list("", 0)));
    ASSERT_EQ(300u, n_entries());
    expect_files(300);
}

/* --- fs_dir_list: where the results go ----------------------------------- */

/* It appends: what out held stays, in front. */
TEST(list_appends_to_out)
{
    fs_entry_t mine = {.name = P("mine"), .kind = FS_KIND_OTHER};
    ASSERT_EQ(SEQC_OK, vec_push(entries, &mine));
    ASSERT_TRUE(fx_write_file("a", "x"));
    ASSERT_TRUE(fx_write_file("b", "x"));
    ASSERT_EQ(FS_OK, KIND(list("", 0)));
    ASSERT_EQ(3u, n_entries());
    ASSERT_STR_EQ("mine", c(entry(0)->name));
    ASSERT_EQ(1, occurrences("a"));
    ASSERT_EQ(1, occurrences("b"));
}

/* The names live as long as the arena they came from — not until the next
 * call, and not as long as the directory. */
TEST(list_names_live_as_long_as_the_arena)
{
    make_files(3);
    ASSERT_EQ(FS_OK, KIND(list("", 0)));
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_write_file("d/other", "x"));
    ASSERT_EQ(FS_OK, KIND(list("d", 0)));        /* a second listing */
    ASSERT_EQ(0, remove(fx_native(FX("f000")))); /* and a change */
    ASSERT_EQ(4u, n_entries());
    expect_files(3); /* the first listing's names are untouched */
    ASSERT_EQ(1, occurrences("other"));
}

/* The entries and the names may come from one arena. */
TEST(list_into_a_single_arena)
{
    make_files(50);
    vec_t *v = vec_create(sizeof(fs_entry_t), names);
    ASSERT_EQ(FS_OK, KIND(fs_dir_list(FX(""), FS_F_SIZE, names, v)));
    ASSERT_EQ(50u, vec_len(v));
    vec_destroy(v);
}

/* --- Fields -------------------------------------------------------------- */

/* Every field asked for is there, with the value fs_stat(FS_NOFOLLOW)
 * gives for the same entry. */
TEST(list_fields_match_fs_stat)
{
    ASSERT_TRUE(fx_write_file("small", "x"));
    ASSERT_TRUE(fx_write_file("bigger", "some more bytes in this one"));
    ASSERT_TRUE(fx_write_file("empty", ""));
    ASSERT_TRUE(fx_mkdir("dir"));

    ASSERT_EQ(FS_OK, KIND(list("", FS_F_ALL)));
    ASSERT_EQ(4u, n_entries());
    for (size_t i = 0; i < n_entries(); i++)
    {
        const fs_entry_t *e = entry(i);
        uint32_t must = FS_F_ALL & ~(uint32_t)FS_F_BTIME;
        ASSERT_EQ(must, e->valid & must);

        fs_entry_t s;
        ASSERT_EQ(
            FS_OK, KIND(fs_stat(FX(c(e->name)), FS_F_ALL, FS_NOFOLLOW, &s)));
        ASSERT_EQ(s.kind, e->kind);
        if (s.kind == FS_KIND_FILE)
        {
            ASSERT_EQ(s.size, e->size);
        }
        ASSERT_EQ(s.mtime_ns, e->mtime_ns);
        ASSERT_EQ(s.mode, e->mode);
        ASSERT_EQ(s.flags, e->flags);
        ASSERT_EQ(s.dev, e->dev);
        ASSERT_EQ(s.ino, e->ino);
        if ((s.valid & e->valid) & FS_F_BTIME)
        {
            ASSERT_EQ(s.btime_ns, e->btime_ns);
        }
    }
    ASSERT_EQ(1u, find("small")->size);
    ASSERT_EQ(27u, find("bigger")->size);
    ASSERT_EQ(0u, find("empty")->size);
}

/* Each field on its own: asked for, delivered. */
TEST(list_delivers_each_field_asked_for)
{
    ASSERT_TRUE(fx_write_file("f", "hello"));
    ASSERT_TRUE(fx_mkdir("d"));
    uint32_t fields[] = {
        FS_F_KIND, FS_F_SIZE, FS_F_MTIME, FS_F_MODE, FS_F_FLAGS, FS_F_ID};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++)
    {
        vec_clear(entries);
        ASSERT_EQ(FS_OK, KIND(list("", fields[i])));
        ASSERT_EQ(2u, n_entries());
        ASSERT_TRUE(find("f")->valid & fields[i]);
        ASSERT_TRUE(find("d")->valid & fields[i]);
    }
    vec_clear(entries);
    ASSERT_EQ(FS_OK, KIND(list("", FS_F_SIZE)));
    ASSERT_EQ(5u, find("f")->size);
}

/* --- Links --------------------------------------------------------------- */

/* Entries are not followed: a link is a link, wherever it points. */
TEST(list_entries_are_not_followed)
{
    ASSERT_TRUE(fx_write_file("target", "hello"));
    ASSERT_TRUE(fx_mkdir("dir"));
    if (!fx_symlink("target", "to_file"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_TRUE(fx_symlink_dir("dir", "to_dir"));
    ASSERT_TRUE(fx_symlink("nowhere", "dangling"));

    ASSERT_EQ(FS_OK, KIND(list("", FS_F_KIND | FS_F_SIZE)));
    ASSERT_EQ(5u, n_entries());
    ASSERT_EQ(FS_KIND_LINK, find("to_file")->kind);
    ASSERT_EQ(FS_KIND_LINK, find("to_dir")->kind);
    ASSERT_EQ(FS_KIND_LINK, find("dangling")->kind); /* listed, not skipped */
    ASSERT_EQ(FS_KIND_FILE, find("target")->kind);
    ASSERT_EQ(FS_KIND_DIR, find("dir")->kind);
}

/* The directory path itself is followed. */
TEST(list_follows_a_link_to_a_directory)
{
    ASSERT_TRUE(fx_mkdir("real"));
    ASSERT_TRUE(fx_write_file("real/x", "1"));
    ASSERT_TRUE(fx_write_file("real/y", "2"));
    if (!fx_symlink_dir("real", "via"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_EQ(FS_OK, KIND(list("via", FS_F_KIND)));
    ASSERT_EQ(2u, n_entries());
    ASSERT_EQ(FS_KIND_FILE, find("x")->kind);
    ASSERT_EQ(FS_KIND_FILE, find("y")->kind);
}

#ifndef _WIN32
/* Neither file, directory nor link: a fifo. */
TEST(list_other_kinds)
{
    ASSERT_EQ(0, mkfifo(fx_native(FX("pipe")), 0644));
    ASSERT_EQ(FS_OK, KIND(list("", FS_F_KIND)));
    ASSERT_EQ(FS_KIND_OTHER, find("pipe")->kind);
}
#endif

/* --- fs_dir_list: errors leave out unchanged ----------------------------- */

TEST(list_errors)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    fs_entry_t mine = {.name = P("mine")};
    ASSERT_EQ(SEQC_OK, vec_push(entries, &mine));

    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(list("missing", 0)));
    ASSERT_EQ(FS_ERR_NOT_DIR, KIND(list("f", 0)));
    ASSERT_EQ(FS_ERR_NOT_DIR, KIND(list("f/x", 0)));
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(list("missing/x", 0)));
    ASSERT_EQ(
        FS_ERR_INVALID, KIND(fs_dir_list(P("relative"), 0, names, entries)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_dir_list(FX(""), 0, names, NULL)));
    ASSERT_EQ(1u, n_entries()); /* unchanged by all of it */

    /* a vec of something else */
    vec_t *ints = vec_create(sizeof(int), names);
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_dir_list(FX(""), 0, names, ints)));
    ASSERT_EQ(0u, vec_len(ints));
}

/* a with a budget: n allocations succeed (from the names arena, so
 * nothing leaks), then every one fails. */
typedef struct
{
    size_t budget;
    allocator_t inner;
} limited_t;

static void *limited_alloc(void *ctx, size_t size, size_t align)
{
    limited_t *b = ctx;
    if (b->budget == 0)
    {
        return NULL;
    }
    b->budget--;
    return mem_alloc(b->inner, size, align);
}

static void *limited_realloc(
    void *ctx, void *ptr, size_t old_size, size_t new_size, size_t align)
{
    limited_t *b = ctx;
    if (b->budget == 0)
    {
        return NULL;
    }
    b->budget--;
    return mem_realloc(b->inner, ptr, old_size, new_size, align);
}

static void limited_free(void *ctx, void *ptr, size_t size)
{
    limited_t *b = ctx;
    mem_free(b->inner, ptr, size);
}

static const allocator_vtable_t limited_vtable = {
    limited_alloc, limited_realloc, limited_free};

/* a runs out — at the start, after a few names, anywhere: OOM, and out as
 * it was.  Walks every allocation fs_dir_list makes from a. */
TEST(list_out_of_memory_in_a_leaves_out_unchanged)
{
    make_files(20);
    fs_entry_t mine = {.name = P("mine")};
    ASSERT_EQ(SEQC_OK, vec_push(entries, &mine));
    for (size_t n = 0;; n++)
    {
        limited_t b = {n, names};
        fs_err_t err =
            fs_dir_list(FX(""), 0, (allocator_t){&limited_vtable, &b}, entries);
        if (fs_ok(err))
        {
            ASSERT_EQ(21u, n_entries());
            break;
        }
        ASSERT_EQ(FS_ERR_OOM, err.kind);
        ASSERT_EQ(1u, n_entries());
        ASSERT_STR_EQ("mine", c(entry(0)->name));
        ASSERT_TRUE(n < 1000);
    }
}

/* out cannot grow: OOM, and out as it was. */
TEST(list_out_out_of_memory)
{
    make_files(100);
    static char mem[2048]; /* the vec and a few entries, not 100 */
    fixed_arena_t fixed;
    fixed_arena_init(&fixed, mem, sizeof mem);
    vec_t *v = vec_create(sizeof(fs_entry_t), fixed_arena_allocator(&fixed));
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(FS_ERR_OOM, KIND(fs_dir_list(FX(""), 0, names, v)));
    ASSERT_EQ(0u, vec_len(v));
}

/* --- fs_dir_iter --------------------------------------------------------- */

/* Copies the iterator's entries into entries, names into names: they are
 * only views until the next step. */
static size_t drain(iter_t *it)
{
    fs_entry_t e;
    size_t n = 0;
    while (iter_next(it, &e))
    {
        char *copy = mem_alloc(names, e.name.len + 1, 1);
        CTT_ASSERT_NOT_NULL(copy);
        memcpy(copy, e.name.ptr, e.name.len);
        e.name = (string_t){copy, e.name.len};
        CTT_ASSERT_EQ(SEQC_OK, vec_push(entries, &e));
        n++;
    }
    return n;
}

TEST(iter_yields_every_entry)
{
    make_files(300);
    ASSERT_TRUE(fx_mkdir("d"));
    fs_err_t err = {FS_ERR_IO, 12345}; /* overwritten */
    iter_t it = fs_dir_iter(FX(""), FS_F_KIND, counted(), &err);
    ASSERT_TRUE(iter_is_valid(&it));
    ASSERT_EQ(sizeof(fs_entry_t), it.elem_size);
    ASSERT_EQ(301u, drain(&it));
    iter_destroy(&it);
    ASSERT_EQ(FS_OK, err.kind);
    expect_files(300);
    ASSERT_EQ(FS_KIND_DIR, find("d")->kind);
    ASSERT_EQ(0, cnt.live); /* the state went back */
}

/* A stream: the memory it holds does not grow with the directory. */
TEST(iter_runs_in_constant_memory)
{
    make_files(300);
    fs_err_t err;
    iter_t it = fs_dir_iter(FX(""), FS_F_ALL, counted(), &err);
    fs_entry_t e;
    ASSERT_TRUE(iter_next(&it, &e));
    int after_one = cnt.live;
    while (iter_next(&it, &e))
    {
        ASSERT_TRUE(cnt.live <= after_one); /* never more than at the start */
    }
    iter_destroy(&it);
    ASSERT_EQ(FS_OK, err.kind);
    ASSERT_EQ(0, cnt.live);
}

TEST(iter_with_a_terminal)
{
    make_files(20);
    fs_err_t err;
    ASSERT_EQ(20u, iter_count(fs_dir_iter(FX(""), 0, counted(), &err)));
    ASSERT_EQ(FS_OK, err.kind);
    ASSERT_EQ(0, cnt.live); /* the terminal gave the state back */
}

static bool is_dir(const void *elem, void *ctx)
{
    (void)ctx;
    return ((const fs_entry_t *)elem)->kind == FS_KIND_DIR;
}

TEST(iter_with_adaptors)
{
    make_files(5);
    ASSERT_TRUE(fx_mkdir("d1"));
    ASSERT_TRUE(fx_mkdir("d2"));
    ASSERT_TRUE(fx_mkdir("d3"));
    fs_err_t err;
    iter_t dirs = iter_filter(
        fs_dir_iter(FX(""), FS_F_KIND, counted(), &err), is_dir, NULL);
    ASSERT_EQ(3u, iter_count(dirs));
    ASSERT_EQ(FS_OK, err.kind);
    ASSERT_EQ(0, cnt.live);
}

/* Stopping early: iter_destroy closes the directory and gives the state
 * back. */
TEST(iter_stopped_early)
{
    make_files(10);
    fs_err_t err;
    iter_t it = fs_dir_iter(FX(""), 0, counted(), &err);
    fs_entry_t e;
    ASSERT_TRUE(iter_next(&it, &e));
    ASSERT_TRUE(cnt.live > 0);
    iter_destroy(&it);
    ASSERT_EQ(0, cnt.live);
}

/* A directory that cannot be opened: the error in err, and a failed
 * iterator that is safe anywhere. */
TEST(iter_errors)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    fs_err_t err;

    iter_t it = fs_dir_iter(FX("missing"), 0, counted(), &err);
    ASSERT_EQ(FS_ERR_NOT_FOUND, err.kind);
    ASSERT_FALSE(iter_is_valid(&it));
    ASSERT_EQ(0u, iter_count(it));

    ASSERT_EQ(0u, iter_count(fs_dir_iter(FX("f"), 0, counted(), &err)));
    ASSERT_EQ(FS_ERR_NOT_DIR, err.kind);

    ASSERT_EQ(0u, iter_count(fs_dir_iter(P("relative"), 0, counted(), &err)));
    ASSERT_EQ(FS_ERR_INVALID, err.kind);

    cnt.budget = 0;
    ASSERT_EQ(0u, iter_count(fs_dir_iter(FX(""), 0, counted(), &err)));
    ASSERT_EQ(FS_ERR_OOM, err.kind);
    cnt.budget = (size_t)-1;

    /* a failed iterator through an adaptor stays failed */
    iter_t f = iter_filter(
        fs_dir_iter(FX("missing"), 0, counted(), &err), is_dir, NULL);
    ASSERT_FALSE(iter_is_valid(&f));

    /* without err: nothing breaks */
    ASSERT_EQ(0u, iter_count(fs_dir_iter(FX("missing"), 0, counted(), NULL)));
    ASSERT_EQ(0, cnt.live);
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "filec fs directories");
}
