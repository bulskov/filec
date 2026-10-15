/* filec/fs step 2: files — fs_open, fs_read_at, fs_write_at, fs_file_size,
 * fs_sync, fs_close, fs_read_all, fs_write_atomic.
 *
 * Every test gets a fresh temporary directory (tests/fs_fixture.h); files
 * are prepared and checked with stdio, not with the functions under
 * test.  FX("a") is the canonical path of a inside it. */

#include "fs_fixture.h" /* first: it sets _XOPEN_SOURCE on POSIX */

#include "ctt.h"
#include "filec/fs.h"

#include "arena/fixed_arena.h"
#include "arena/growing_arena.h"

static growing_arena_t arena;
static strbuf_t *sb; /* the out of fs_read_all in a test */

void ctt_before_each(void)
{
    fx_setup();
    growing_arena_init(&arena, 4096);
    sb = strbuf_create(growing_arena_allocator(&arena));
}

void ctt_after_each(void)
{
    growing_arena_destroy(&arena);
    fx_teardown();
}

#define P(lit) STRING_LIT(lit)
#define KIND(call) ((call).kind)

/* fs_open that must succeed. */
static fs_file_t open_ok(const char *rel, uint32_t mode)
{
    fs_file_t f = FS_FILE_NONE;
    fs_err_t err = fs_open(FX(rel), mode, &f);
    CTT_ASSERT_EQ(FS_OK, err.kind);
    CTT_ASSERT_TRUE(fs_file_is_open(f));
    return f;
}

/* The contents of rel (read with stdio) as a C string. */
static const char *contents(const char *rel)
{
    const char *s = fx_read_file(rel, NULL);
    return s ? s : "(cannot read)";
}

static size_t size_of(const char *rel)
{
    size_t n = 0;
    return fx_read_file(rel, &n) ? n : (size_t)-1;
}

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

/* --- fs_open ------------------------------------------------------------- */

TEST(open_an_existing_file_for_reading)
{
    ASSERT_TRUE(fx_write_file("f", "hello"));
    fs_file_t f = open_ok("f", FS_READ);
    fs_close(f);
}

TEST(open_missing_is_not_found)
{
    fs_file_t f = {123};
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_open(FX("missing"), FS_READ, &f)));
    ASSERT_FALSE(fs_file_is_open(f)); /* FS_FILE_NONE on failure */
    ASSERT_EQ(
        FS_ERR_NOT_FOUND, KIND(fs_open(FX("missing"), FS_READ | FS_WRITE, &f)));
}

TEST(open_create_makes_an_empty_file)
{
    fs_file_t f = open_ok("new", FS_WRITE | FS_CREATE);
    fs_close(f);
    ASSERT_EQ(0u, size_of("new"));
}

/* FS_CREATE alone opens an existing file as it is. */
TEST(open_create_keeps_an_existing_file)
{
    ASSERT_TRUE(fx_write_file("f", "hello"));
    fs_file_t f = open_ok("f", FS_WRITE | FS_CREATE);
    fs_close(f);
    ASSERT_STR_EQ("hello", contents("f"));
}

TEST(open_create_excl_refuses_an_existing_file)
{
    ASSERT_TRUE(fx_write_file("f", "hello"));
    fs_file_t f;
    ASSERT_EQ(
        FS_ERR_EXISTS,
        KIND(fs_open(FX("f"), FS_WRITE | FS_CREATE | FS_EXCL, &f)));
    ASSERT_STR_EQ("hello", contents("f"));

    fs_file_t g = open_ok("g", FS_WRITE | FS_CREATE | FS_EXCL);
    fs_close(g);
    ASSERT_EQ(0u, size_of("g"));
}

TEST(open_trunc_empties_the_file)
{
    ASSERT_TRUE(fx_write_file("f", "hello"));
    fs_file_t f = open_ok("f", FS_WRITE | FS_TRUNC);
    fs_close(f);
    ASSERT_EQ(0u, size_of("f"));
}

/* A file manager must never get a handle to a directory by accident. */
TEST(open_a_directory_is_is_dir)
{
    ASSERT_TRUE(fx_mkdir("d"));
    fs_file_t f;
    ASSERT_EQ(FS_ERR_IS_DIR, KIND(fs_open(FX("d"), FS_READ, &f)));
    ASSERT_FALSE(fs_file_is_open(f));
    ASSERT_EQ(FS_ERR_IS_DIR, KIND(fs_open(FX("d"), FS_READ | FS_WRITE, &f)));
    ASSERT_EQ(FS_ERR_IS_DIR, KIND(fs_open(FX("d"), FS_WRITE | FS_CREATE, &f)));
}

/* Combinations that make no sense are refused before touching the disk. */
TEST(open_checks_the_mode)
{
    fs_file_t f;
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_open(FX("a"), 0, &f)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_open(FX("a"), FS_READ | FS_CREATE, &f)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_open(FX("a"), FS_READ | FS_TRUNC, &f)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_open(FX("a"), FS_WRITE | FS_EXCL, &f)));
    ASSERT_FALSE(fs_exists(FX("a"))); /* nothing was created */
}

TEST(open_checks_path_and_out)
{
    fs_file_t f;
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_open(P("relative"), FS_READ, &f)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_open(FX("a"), FS_READ, NULL)));
    ASSERT_EQ(
        FS_ERR_NOT_FOUND,
        KIND(fs_open(FX("missing/new"), FS_WRITE | FS_CREATE, &f)));
}

TEST(close_none_is_a_no_op)
{
    fs_close(FS_FILE_NONE);
    ASSERT_FALSE(fs_file_is_open(FS_FILE_NONE));
}

/* An open file does not lock anybody out — on Windows too. */
TEST(open_files_are_shared)
{
    ASSERT_TRUE(fx_write_file("f", "hello"));
    fs_file_t a = open_ok("f", FS_READ | FS_WRITE);
    fs_file_t b = open_ok("f", FS_READ | FS_WRITE); /* a second writer */
    ASSERT_EQ(FS_OK, KIND(fs_write_at(b, 0, "J", 1)));
    char buf[8];
    size_t got = 0;
    ASSERT_EQ(FS_OK, KIND(fs_read_at(a, 0, buf, 5, &got)));
    ASSERT_EQ(5u, got);
    ASSERT_EQ(0, memcmp(buf, "Jello", 5));

    /* and it can be removed while open.  The name may linger until the
     * last handle is closed (Windows before 10 1809, FAT, Wine: "delete
     * pending"), so look only after closing. */
    ASSERT_EQ(FS_OK, KIND(fs_remove(FX("f"))));
    fs_close(a);
    fs_close(b);
    ASSERT_FALSE(fs_exists(FX("f")));
}

/* --- fs_read_at ---------------------------------------------------------- */

TEST(read_at_reads_from_any_offset)
{
    ASSERT_TRUE(fx_write_file("f", "0123456789"));
    fs_file_t f = open_ok("f", FS_READ);
    char buf[16];
    size_t got = 0;

    ASSERT_EQ(FS_OK, KIND(fs_read_at(f, 3, buf, 4, &got)));
    ASSERT_EQ(4u, got);
    ASSERT_EQ(0, memcmp(buf, "3456", 4));

    /* no hidden position: the same call again reads the same bytes */
    ASSERT_EQ(FS_OK, KIND(fs_read_at(f, 3, buf, 4, &got)));
    ASSERT_EQ(0, memcmp(buf, "3456", 4));

    ASSERT_EQ(FS_OK, KIND(fs_read_at(f, 0, buf, 2, &got)));
    ASSERT_EQ(0, memcmp(buf, "01", 2));
    fs_close(f);
}

/* Fewer bytes than asked for means end of file; at or past the end is
 * 0 bytes, not an error. */
TEST(read_at_stops_at_end_of_file)
{
    ASSERT_TRUE(fx_write_file("f", "0123456789"));
    fs_file_t f = open_ok("f", FS_READ);
    char buf[16];
    size_t got = 99;

    ASSERT_EQ(FS_OK, KIND(fs_read_at(f, 7, buf, 10, &got)));
    ASSERT_EQ(3u, got);
    ASSERT_EQ(0, memcmp(buf, "789", 3));

    ASSERT_EQ(FS_OK, KIND(fs_read_at(f, 10, buf, 10, &got)));
    ASSERT_EQ(0u, got);
    ASSERT_EQ(FS_OK, KIND(fs_read_at(f, 1000000, buf, 10, &got)));
    ASSERT_EQ(0u, got);
    fs_close(f);
}

TEST(read_at_zero_bytes)
{
    ASSERT_TRUE(fx_write_file("f", "abc"));
    fs_file_t f = open_ok("f", FS_READ);
    size_t got = 99;
    char buf[1];
    ASSERT_EQ(FS_OK, KIND(fs_read_at(f, 0, buf, 0, &got)));
    ASSERT_EQ(0u, got);
    fs_close(f);
}

/* Bigger than any single OS read may deliver: read_at loops until it has
 * n bytes or reaches the end. */
TEST(read_at_reads_large_blocks_whole)
{
    enum
    {
        N = 3 * 1024 * 1024
    };
    char *data = malloc(N);
    char *back = malloc(N);
    ASSERT_NOT_NULL(data);
    ASSERT_NOT_NULL(back);
    for (size_t i = 0; i < N; ++i)
    {
        data[i] = (char)(i * 7 + i / 251);
    }
    fs_file_t f = open_ok("big", FS_READ | FS_WRITE | FS_CREATE);
    ASSERT_EQ(FS_OK, KIND(fs_write_at(f, 0, data, N)));
    size_t got = 0;
    ASSERT_EQ(FS_OK, KIND(fs_read_at(f, 0, back, N, &got)));
    ASSERT_EQ((size_t)N, got);
    ASSERT_EQ(0, memcmp(data, back, N));
    fs_close(f);
    free(data);
    free(back);
}

TEST(read_at_checks_its_arguments)
{
    ASSERT_TRUE(fx_write_file("f", "abc"));
    fs_file_t f = open_ok("f", FS_READ);
    char buf[4];
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_read_at(f, 0, buf, 3, NULL)));
    size_t got;
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_read_at(FS_FILE_NONE, 0, buf, 3, &got)));
    fs_close(f);
}

/* --- fs_write_at --------------------------------------------------------- */

TEST(write_at_writes_at_the_offset)
{
    ASSERT_TRUE(fx_write_file("f", "hello world"));
    fs_file_t f = open_ok("f", FS_WRITE);
    ASSERT_EQ(FS_OK, KIND(fs_write_at(f, 6, "WORLD", 5)));
    ASSERT_EQ(FS_OK, KIND(fs_write_at(f, 0, "J", 1)));
    fs_close(f);
    ASSERT_STR_EQ("Jello WORLD", contents("f")); /* the rest is kept */
}

/* Writes in any order; past the end extends the file, a gap is zeros. */
TEST(write_at_past_the_end_extends_with_zeros)
{
    fs_file_t f = open_ok("f", FS_READ | FS_WRITE | FS_CREATE);
    ASSERT_EQ(FS_OK, KIND(fs_write_at(f, 8, "end", 3)));
    ASSERT_EQ(FS_OK, KIND(fs_write_at(f, 0, "ab", 2)));
    uint64_t size = 0;
    ASSERT_EQ(FS_OK, KIND(fs_file_size(f, &size)));
    ASSERT_EQ(11u, size);
    fs_close(f);

    size_t n = 0;
    const char *s = fx_read_file("f", &n);
    ASSERT_EQ(11u, n);
    ASSERT_EQ(0, memcmp(s, "ab\0\0\0\0\0\0end", 11));
}

TEST(write_at_zero_bytes_changes_nothing)
{
    ASSERT_TRUE(fx_write_file("f", "abc"));
    fs_file_t f = open_ok("f", FS_WRITE);
    ASSERT_EQ(FS_OK, KIND(fs_write_at(f, 100, "", 0)));
    fs_close(f);
    ASSERT_STR_EQ("abc", contents("f"));
}

/* Binary data: NUL bytes and every byte value go through unchanged. */
TEST(write_at_is_binary_safe)
{
    unsigned char all[256];
    for (int i = 0; i < 256; ++i)
    {
        all[i] = (unsigned char)i;
    }
    fs_file_t f = open_ok("bin", FS_WRITE | FS_CREATE);
    ASSERT_EQ(FS_OK, KIND(fs_write_at(f, 0, all, sizeof all)));
    fs_close(f);
    size_t n = 0;
    const char *s = fx_read_file("bin", &n);
    ASSERT_EQ(256u, n);
    ASSERT_EQ(0, memcmp(s, all, 256));
}

/* Writing needs FS_WRITE; reading needs FS_READ. */
TEST(handles_respect_their_mode)
{
    ASSERT_TRUE(fx_write_file("f", "abc"));
    fs_file_t r = open_ok("f", FS_READ);
    ASSERT_FALSE(fs_ok(fs_write_at(r, 0, "x", 1)));
    fs_close(r);
    ASSERT_STR_EQ("abc", contents("f"));

    fs_file_t w = open_ok("f", FS_WRITE);
    char buf[4];
    size_t got;
    ASSERT_FALSE(fs_ok(fs_read_at(w, 0, buf, 3, &got)));
    fs_close(w);
}

TEST(write_at_checks_its_arguments)
{
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_write_at(FS_FILE_NONE, 0, "x", 1)));
}

/* --- fs_file_size, fs_sync ----------------------------------------------- */

TEST(file_size_follows_the_writes)
{
    fs_file_t f = open_ok("f", FS_WRITE | FS_CREATE);
    uint64_t size = 99;
    ASSERT_EQ(FS_OK, KIND(fs_file_size(f, &size)));
    ASSERT_EQ(0u, size);
    ASSERT_EQ(FS_OK, KIND(fs_write_at(f, 0, "hello", 5)));
    ASSERT_EQ(FS_OK, KIND(fs_file_size(f, &size)));
    ASSERT_EQ(5u, size);
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_file_size(f, NULL)));
    fs_close(f);
}

TEST(sync_writes_through)
{
    fs_file_t f = open_ok("f", FS_WRITE | FS_CREATE);
    ASSERT_EQ(FS_OK, KIND(fs_write_at(f, 0, "durable", 7)));
    ASSERT_EQ(FS_OK, KIND(fs_sync(f)));
    ASSERT_STR_EQ("durable", contents("f")); /* visible before the close */
    fs_close(f);
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_sync(FS_FILE_NONE)));
}

/* --- fs_read_all --------------------------------------------------------- */

TEST(read_all_appends_the_file)
{
    ASSERT_TRUE(fx_write_file("f", "hello"));
    strbuf_append(sb, P("> "));
    ASSERT_EQ(FS_OK, KIND(fs_read_all(FX("f"), sb)));
    ASSERT_STR_EQ("> hello", c(strbuf_view(sb)));
}

TEST(read_all_of_an_empty_file)
{
    ASSERT_TRUE(fx_write_file("empty", ""));
    ASSERT_EQ(FS_OK, KIND(fs_read_all(FX("empty"), sb)));
    ASSERT_EQ(0u, strbuf_len(sb));
}

TEST(read_all_reads_a_large_binary_file)
{
    enum
    {
        N = 1024 * 1024 + 17
    };
    char *data = malloc(N);
    ASSERT_NOT_NULL(data);
    for (size_t i = 0; i < N; ++i)
    {
        data[i] = (char)(i % 251); /* NUL bytes included */
    }
    fs_file_t f = open_ok("big", FS_WRITE | FS_CREATE);
    ASSERT_EQ(FS_OK, KIND(fs_write_at(f, 0, data, N)));
    fs_close(f);

    ASSERT_EQ(FS_OK, KIND(fs_read_all(FX("big"), sb)));
    ASSERT_EQ((size_t)N, strbuf_len(sb));
    ASSERT_EQ(0, memcmp(strbuf_view(sb).ptr, data, N));
    free(data);
}

/* Failures leave out exactly as it was. */
TEST(read_all_failures_leave_out_unchanged)
{
    ASSERT_TRUE(fx_mkdir("d"));
    strbuf_append(sb, P("keep"));
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_read_all(FX("missing"), sb)));
    ASSERT_EQ(FS_ERR_IS_DIR, KIND(fs_read_all(FX("d"), sb)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_read_all(P("relative"), sb)));
    ASSERT_STR_EQ("keep", c(strbuf_view(sb)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_read_all(FX("missing"), NULL)));
}

/* Out of memory: out cannot hold the file.  A strbuf from a small fixed
 * arena runs out half-way — and must be left as it was. */
TEST(read_all_reports_oom)
{
    ASSERT_TRUE(fx_write_file(
        "f",
        "this file does not fit in the arena"
        " that backs the strbuf below, not at"
        " all, it is far too long for it"));
    char mem[160];
    fixed_arena_t small;
    fixed_arena_init(&small, mem, sizeof mem);
    strbuf_t *tiny = strbuf_create(fixed_arena_allocator(&small));
    ASSERT_NOT_NULL(tiny);
    strbuf_append(tiny, P("keep"));
    ASSERT_EQ(FS_ERR_OOM, KIND(fs_read_all(FX("f"), tiny)));
    ASSERT_STR_EQ("keep", c(strbuf_view(tiny)));
}

/* --- fs_write_atomic ----------------------------------------------------- */

TEST(write_atomic_creates_a_file)
{
    ASSERT_EQ(FS_OK, KIND(fs_write_atomic(FX("new"), P("fresh"))));
    ASSERT_STR_EQ("fresh", contents("new"));
}

/* All of the old content goes, also when the new content is shorter. */
TEST(write_atomic_replaces_a_file)
{
    ASSERT_TRUE(fx_write_file("f", "the old, long content"));
    ASSERT_EQ(FS_OK, KIND(fs_write_atomic(FX("f"), P("new"))));
    ASSERT_STR_EQ("new", contents("f"));
}

TEST(write_atomic_empty_data)
{
    ASSERT_TRUE(fx_write_file("f", "old"));
    ASSERT_EQ(FS_OK, KIND(fs_write_atomic(FX("f"), P(""))));
    ASSERT_EQ(0u, size_of("f"));
}

/* The temp file is gone afterwards: the directory holds just the
 * target — after success and after failure. */
TEST(write_atomic_leaves_no_temp_file)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_EQ(FS_OK, KIND(fs_write_atomic(FX("d/f"), P("one"))));
    ASSERT_EQ(FS_OK, KIND(fs_write_atomic(FX("d/f"), P("two"))));
    ASSERT_EQ(1, fx_count_entries("d"));

    ASSERT_TRUE(fx_mkdir("d/sub"));
    ASSERT_EQ(FS_ERR_IS_DIR, KIND(fs_write_atomic(FX("d/sub"), P("x"))));
    ASSERT_EQ(2, fx_count_entries("d")); /* f and sub, nothing else */
    ASSERT_EQ(0, fx_count_entries("d/sub"));
}

TEST(write_atomic_failures)
{
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_write_atomic(FX("missing/f"), P("x"))));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_write_atomic(P("relative"), P("x"))));
}

TEST(write_atomic_round_trips_through_read_all)
{
    const char bin[] = {'a', '\0', 'b', '\n', '\r', (char)0xff};
    string_t data = {bin, sizeof bin};
    ASSERT_EQ(FS_OK, KIND(fs_write_atomic(FX("f"), data)));
    ASSERT_EQ(FS_OK, KIND(fs_read_all(FX("f"), sb)));
    ASSERT_EQ(sizeof bin, strbuf_len(sb));
    ASSERT_EQ(0, memcmp(strbuf_view(sb).ptr, bin, sizeof bin));
}

#ifndef _WIN32
TEST(write_atomic_keeps_the_permissions)
{
    ASSERT_TRUE(fx_write_file("f", "secret"));
    ASSERT_EQ(0, chmod(fx_native(FX("f")), 0600));
    ASSERT_EQ(FS_OK, KIND(fs_write_atomic(FX("f"), P("new"))));
    struct stat st;
    ASSERT_EQ(0, stat(fx_native(FX("f")), &st));
    ASSERT_EQ(0600, (int)(st.st_mode & 0777));
}
#endif

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "filec fs files");
}
