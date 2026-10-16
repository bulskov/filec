/* filec/fs step 4: fs_mkdir_all, fs_rename, fs_copy_file, fs_read_link,
 * fs_real_path.
 *
 * Every test gets a fresh temporary directory (tests/fs_fixture.h); files
 * are made and checked with OS calls, and with the fs functions of the
 * steps before this one (fs_stat, fs_open, fs_read_all, fs_dir_list).
 * FX("a") is the canonical path of a inside it.  Tests that need symlinks
 * return early where the OS does not allow them (Windows without
 * developer mode). */

#include "fs_fixture.h" /* first: it sets _XOPEN_SOURCE on POSIX */

#include "ctt.h"
#include "filec/fs.h"

#include "arena/growing_arena.h"
#include "seqc/vec.h"

#include <stdatomic.h>

static growing_arena_t arena;
static strbuf_t *sb; /* the out of read_link / real_path / read_all */

void ctt_before_each(void)
{
    fx_setup();
    growing_arena_init(&arena, 64 * 1024);
    sb = strbuf_create(growing_arena_allocator(&arena));
}

void ctt_after_each(void)
{
    growing_arena_destroy(&arena);
    fx_teardown();
}

#define P(lit) STRING_LIT(lit)
#define KIND(call) ((call).kind)

/* string_t -> C string, in rotating static buffers. */
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

/* The contents of rel (read with stdio) as a C string. */
static const char *contents(const char *rel)
{
    const char *s = fx_read_file(rel, NULL);
    return s ? s : "(cannot read)";
}

static fs_kind_t kind_of(const char *rel)
{
    fs_entry_t e;
    if (!fs_ok(fs_stat(FX(rel), FS_F_KIND, FS_NOFOLLOW, &e)))
    {
        return FS_KIND_UNKNOWN;
    }
    return e.kind;
}

static int64_t mtime_of(const char *rel)
{
    fs_entry_t e;
    CTT_ASSERT_EQ(FS_OK, KIND(fs_stat(FX(rel), FS_F_MTIME, 0, &e)));
    return e.mtime_ns;
}

/* n bytes of a pattern that is different at every offset of a MiB. */
static unsigned char *pattern(size_t n)
{
    unsigned char *p = malloc(n ? n : 1);
    CTT_ASSERT_NOT_NULL(p);
    for (size_t i = 0; i < n; i++)
    {
        p[i] = (unsigned char)((i * 131u) ^ (i >> 10));
    }
    return p;
}

/* The whole file rel (fs_read_all, step 2) equals p[0..n). */
static bool file_is(const char *rel, const unsigned char *p, size_t n)
{
    strbuf_clear(sb);
    if (!fs_ok(fs_read_all(FX(rel), sb)))
    {
        return false;
    }
    string_t v = strbuf_view(sb);
    return v.len == n && (n == 0 || memcmp(v.ptr, p, n) == 0);
}

/* =========================================================================
 * fs_mkdir_all
 * ========================================================================= */

TEST(mkdir_all_makes_every_level)
{
    ASSERT_EQ(FS_OK, KIND(fs_mkdir_all(FX("a/b/c/d"))));
    ASSERT_TRUE(fx_is_dir("a"));
    ASSERT_TRUE(fx_is_dir("a/b"));
    ASSERT_TRUE(fx_is_dir("a/b/c"));
    ASSERT_TRUE(fx_is_dir("a/b/c/d"));
}

/* What is already there is fine: all of it, part of it, again. */
TEST(mkdir_all_accepts_what_exists)
{
    ASSERT_EQ(FS_OK, KIND(fs_mkdir_all(FX(""))));
    ASSERT_TRUE(fx_mkdir("a"));
    ASSERT_EQ(FS_OK, KIND(fs_mkdir_all(FX("a"))));
    ASSERT_EQ(FS_OK, KIND(fs_mkdir_all(FX("a/b/c"))));
    ASSERT_EQ(FS_OK, KIND(fs_mkdir_all(FX("a/b/c"))));
    ASSERT_TRUE(fx_is_dir("a/b/c"));
}

TEST(mkdir_all_errors)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_EQ(FS_ERR_EXISTS, KIND(fs_mkdir_all(FX("f"))));
    ASSERT_EQ(FS_ERR_NOT_DIR, KIND(fs_mkdir_all(FX("f/x/y"))));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_mkdir_all(P("relative/x"))));
    ASSERT_STR_EQ("x", contents("f"));
}

/* A link to a directory on the way is a directory. */
TEST(mkdir_all_through_a_link)
{
    ASSERT_TRUE(fx_mkdir("real"));
    if (!fx_symlink_dir("real", "via"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_EQ(FS_OK, KIND(fs_mkdir_all(FX("via/x/y"))));
    ASSERT_TRUE(fx_is_dir("real/x/y"));
}

/* =========================================================================
 * fs_rename
 * ========================================================================= */

TEST(rename_a_file)
{
    ASSERT_TRUE(fx_write_file("a", "hello"));
    ASSERT_EQ(FS_OK, KIND(fs_rename(FX("a"), FX("b"), 0)));
    ASSERT_FALSE(fx_exists("a"));
    ASSERT_STR_EQ("hello", contents("b"));
}

TEST(rename_a_directory_with_its_contents)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_mkdir("d/s"));
    ASSERT_TRUE(fx_write_file("d/x", "1"));
    ASSERT_TRUE(fx_write_file("d/s/y", "2"));
    ASSERT_EQ(FS_OK, KIND(fs_rename(FX("d"), FX("e"), 0)));
    ASSERT_FALSE(fx_exists("d"));
    ASSERT_STR_EQ("1", contents("e/x"));
    ASSERT_STR_EQ("2", contents("e/s/y"));
}

/* Without FS_REPLACE nothing at to is touched — file or directory. */
TEST(rename_refuses_an_existing_target)
{
    ASSERT_TRUE(fx_write_file("a", "new"));
    ASSERT_TRUE(fx_write_file("b", "old"));
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_EQ(FS_ERR_EXISTS, KIND(fs_rename(FX("a"), FX("b"), 0)));
    ASSERT_EQ(FS_ERR_EXISTS, KIND(fs_rename(FX("a"), FX("d"), 0)));
    ASSERT_STR_EQ("new", contents("a"));
    ASSERT_STR_EQ("old", contents("b"));
    ASSERT_TRUE(fx_is_dir("d"));
}

TEST(rename_replace_replaces_a_file)
{
    ASSERT_TRUE(fx_write_file("a", "new"));
    ASSERT_TRUE(fx_write_file("b", "the old, longer content"));
    ASSERT_EQ(FS_OK, KIND(fs_rename(FX("a"), FX("b"), FS_REPLACE)));
    ASSERT_FALSE(fx_exists("a"));
    ASSERT_STR_EQ("new", contents("b"));
}

/* A directory at to is never replaced, not even an empty one. */
TEST(rename_replace_never_replaces_a_directory)
{
    ASSERT_TRUE(fx_write_file("a", "x"));
    ASSERT_TRUE(fx_mkdir("src"));
    ASSERT_TRUE(fx_mkdir("empty"));
    ASSERT_EQ(FS_ERR_IS_DIR, KIND(fs_rename(FX("a"), FX("empty"), FS_REPLACE)));
    ASSERT_EQ(
        FS_ERR_IS_DIR, KIND(fs_rename(FX("src"), FX("empty"), FS_REPLACE)));
    ASSERT_STR_EQ("x", contents("a"));
    ASSERT_TRUE(fx_is_dir("src"));
    ASSERT_TRUE(fx_is_dir("empty"));
}

TEST(rename_to_itself_does_nothing)
{
    ASSERT_TRUE(fx_write_file("a", "hello"));
    ASSERT_EQ(FS_OK, KIND(fs_rename(FX("a"), FX("a"), 0)));
    ASSERT_EQ(FS_OK, KIND(fs_rename(FX("a"), FX("a"), FS_REPLACE)));
    ASSERT_STR_EQ("hello", contents("a"));
}

/* "abc" -> "ABC": the name changes, on case-insensitive volumes too, and
 * without FS_REPLACE — it is the same file, not one in the way. */
TEST(rename_changes_only_the_case)
{
    if (fx_is_wine())
    {
        return; /* Wine's MoveFileExW keeps the old case; Windows does not */
    }
    ASSERT_TRUE(fx_write_file("abc", "hello"));
    ASSERT_EQ(FS_OK, KIND(fs_rename(FX("abc"), FX("ABC"), 0)));

    growing_arena_t la;
    growing_arena_init(&la, 4096);
    vec_t *v = vec_create(sizeof(fs_entry_t), growing_arena_allocator(&la));
    ASSERT_EQ(
        FS_OK, KIND(fs_dir_list(FX(""), 0, growing_arena_allocator(&la), v)));
    ASSERT_EQ(1u, vec_len(v));
    ASSERT_STR_EQ("ABC", c(((fs_entry_t *)vec_get_ptr(v, 0))->name));
    growing_arena_destroy(&la);
    ASSERT_STR_EQ("hello", contents("ABC"));
}

/* A link is renamed itself; what it points to stays. */
TEST(rename_a_link_moves_the_link)
{
    ASSERT_TRUE(fx_write_file("target", "hello"));
    if (!fx_symlink("target", "l"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_EQ(FS_OK, KIND(fs_rename(FX("l"), FX("m"), 0)));
    ASSERT_EQ(FS_KIND_LINK, kind_of("m"));
    ASSERT_FALSE(fx_exists("l"));
    ASSERT_EQ(FS_KIND_FILE, kind_of("target"));
    ASSERT_STR_EQ("hello", contents("m")); /* still points there */
}

TEST(rename_a_directory_into_itself_is_invalid)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_mkdir("d/sub"));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_rename(FX("d"), FX("d/sub/d"), 0)));
    ASSERT_TRUE(fx_is_dir("d/sub"));
}

/* A file someone has open can be renamed, and they keep reading it. */
TEST(rename_an_open_file)
{
    ASSERT_TRUE(fx_write_file("a", "hello"));
    fs_file_t f;
    ASSERT_EQ(FS_OK, KIND(fs_open(FX("a"), FS_READ, &f)));
    ASSERT_EQ(FS_OK, KIND(fs_rename(FX("a"), FX("b"), 0)));
    char buf[8];
    size_t got = 0;
    ASSERT_EQ(FS_OK, KIND(fs_read_at(f, 0, buf, 5, &got)));
    ASSERT_EQ(5u, got);
    ASSERT_EQ(0, memcmp(buf, "hello", 5));
    fs_close(f);
    ASSERT_STR_EQ("hello", contents("b"));
}

TEST(rename_errors)
{
    ASSERT_TRUE(fx_write_file("a", "x"));
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_rename(FX("missing"), FX("b"), 0)));
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_rename(FX("a"), FX("no/b"), 0)));
    ASSERT_EQ(FS_ERR_NOT_DIR, KIND(fs_rename(FX("a"), FX("f/b"), 0)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_rename(P("relative"), FX("b"), 0)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_rename(FX("a"), P("relative"), 0)));
    ASSERT_STR_EQ("x", contents("a"));
}

/* =========================================================================
 * fs_copy_file
 * ========================================================================= */

TEST(copy_a_file)
{
    const unsigned char bin[] = {'a', 0, 'b', '\n', '\r', 0xff};
    ASSERT_TRUE(fx_write_bytes("a", bin, sizeof bin));
    ASSERT_EQ(FS_OK, KIND(fs_copy_file(FX("a"), FX("b"), 0, NULL)));
    ASSERT_TRUE(file_is("b", bin, sizeof bin));
    ASSERT_TRUE(file_is("a", bin, sizeof bin)); /* the source stays */
}

TEST(copy_empty_and_large_files)
{
    ASSERT_TRUE(fx_write_file("empty", ""));
    ASSERT_EQ(FS_OK, KIND(fs_copy_file(FX("empty"), FX("e2"), 0, NULL)));
    ASSERT_TRUE(file_is("e2", NULL, 0));

    size_t n = 3 * 1024 * 1024 + 17;
    unsigned char *p = pattern(n);
    ASSERT_TRUE(fx_write_bytes("big", p, n));
    ASSERT_EQ(FS_OK, KIND(fs_copy_file(FX("big"), FX("big2"), 0, NULL)));
    ASSERT_TRUE(file_is("big2", p, n));
    free(p);
}

TEST(copy_refuses_an_existing_target)
{
    ASSERT_TRUE(fx_write_file("a", "new"));
    ASSERT_TRUE(fx_write_file("b", "old"));
    ASSERT_EQ(FS_ERR_EXISTS, KIND(fs_copy_file(FX("a"), FX("b"), 0, NULL)));
    ASSERT_STR_EQ("old", contents("b"));
}

TEST(copy_replace_replaces_a_file)
{
    ASSERT_TRUE(fx_write_file("a", "new"));
    ASSERT_TRUE(fx_write_file("b", "the old, longer content"));
    ASSERT_EQ(FS_OK, KIND(fs_copy_file(FX("a"), FX("b"), FS_REPLACE, NULL)));
    ASSERT_STR_EQ("new", contents("b"));
    ASSERT_EQ(2, fx_count_entries("")); /* no temporary left */
}

TEST(copy_directories_are_is_dir)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_EQ(FS_ERR_IS_DIR, KIND(fs_copy_file(FX("d"), FX("e"), 0, NULL)));
    ASSERT_FALSE(fx_exists("e"));
    ASSERT_EQ(FS_ERR_IS_DIR, KIND(fs_copy_file(FX("f"), FX("d"), 0, NULL)));
    ASSERT_EQ(
        FS_ERR_IS_DIR, KIND(fs_copy_file(FX("f"), FX("d"), FS_REPLACE, NULL)));
    ASSERT_TRUE(fx_is_dir("d"));
    ASSERT_EQ(0, fx_count_entries("d"));
}

/* A link at from is followed: the copy is a file with the content. */
TEST(copy_follows_a_link)
{
    ASSERT_TRUE(fx_write_file("target", "hello"));
    if (!fx_symlink("target", "l"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_EQ(FS_OK, KIND(fs_copy_file(FX("l"), FX("copy"), 0, NULL)));
    ASSERT_EQ(FS_KIND_FILE, kind_of("copy"));
    ASSERT_STR_EQ("hello", contents("copy"));

    ASSERT_TRUE(fx_symlink("nowhere", "dangling"));
    ASSERT_EQ(
        FS_ERR_NOT_FOUND,
        KIND(fs_copy_file(FX("dangling"), FX("c2"), 0, NULL)));
    ASSERT_FALSE(fx_exists("c2"));
}

/* Copying a file onto itself would empty it: refused, directly or
 * through a link. */
TEST(copy_onto_itself_is_invalid)
{
    ASSERT_TRUE(fx_write_file("f", "precious"));
    ASSERT_EQ(
        FS_ERR_INVALID, KIND(fs_copy_file(FX("f"), FX("f"), FS_REPLACE, NULL)));
    ASSERT_STR_EQ("precious", contents("f"));
    if (!fx_symlink("f", "l"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_EQ(
        FS_ERR_INVALID, KIND(fs_copy_file(FX("f"), FX("l"), FS_REPLACE, NULL)));
    ASSERT_EQ(
        FS_ERR_INVALID, KIND(fs_copy_file(FX("l"), FX("f"), FS_REPLACE, NULL)));
    ASSERT_STR_EQ("precious", contents("f"));
}

TEST(copy_keep_metadata)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_TRUE(fx_set_mtime("f", 1000000000)); /* 2001-09-09 */
#ifndef _WIN32
    ASSERT_EQ(0, chmod(fx_native(FX("f")), 0640));
#endif
    ASSERT_EQ(
        FS_OK, KIND(fs_copy_file(FX("f"), FX("kept"), FS_KEEP_METADATA, NULL)));
    ASSERT_EQ(mtime_of("f"), mtime_of("kept"));
#ifndef _WIN32
    ASSERT_EQ(0640, fx_mode("kept"));
#endif

    /* without the flag the copy is new */
    ASSERT_EQ(FS_OK, KIND(fs_copy_file(FX("f"), FX("new"), 0, NULL)));
    ASSERT_TRUE(mtime_of("new") > mtime_of("f"));
}

/* What progress saw, and when to cancel. */
typedef struct
{
    int calls;
    bool monotonic;
    uint64_t last_done, total;
    bool total_steady;
    atomic_bool *cancel_at_first; /* set it on the first call */
} progress_t;

static void on_progress(void *user, uint64_t done, uint64_t total)
{
    progress_t *p = user;
    if (p->calls > 0)
    {
        p->monotonic = p->monotonic && done >= p->last_done;
        p->total_steady = p->total_steady && total == p->total;
    }
    p->calls++;
    p->last_done = done;
    p->total = total;
    if (p->cancel_at_first)
    {
        atomic_store(p->cancel_at_first, true);
    }
}

TEST(copy_reports_progress)
{
    size_t n = 3 * 1024 * 1024 + 5;
    unsigned char *p = pattern(n);
    ASSERT_TRUE(fx_write_bytes("big", p, n));
    progress_t pr = {0, true, 0, 0, true, NULL};
    fs_op_t op = {NULL, on_progress, &pr};
    ASSERT_EQ(FS_OK, KIND(fs_copy_file(FX("big"), FX("copy"), 0, &op)));
    ASSERT_TRUE(pr.calls >= 1);
    ASSERT_TRUE(pr.monotonic);
    ASSERT_TRUE(pr.total_steady);
    ASSERT_EQ((uint64_t)n, pr.total);
    ASSERT_EQ((uint64_t)n, pr.last_done); /* the last call: all of it */
    ASSERT_TRUE(file_is("copy", p, n));
    free(p);
}

TEST(copy_cancelled_before_it_starts)
{
    ASSERT_TRUE(fx_write_file("a", "hello"));
    atomic_bool cancel = true;
    fs_op_t op = {&cancel, NULL, NULL};
    ASSERT_EQ(FS_ERR_CANCELLED, KIND(fs_copy_file(FX("a"), FX("b"), 0, &op)));
    ASSERT_FALSE(fx_exists("b"));
    ASSERT_EQ(1, fx_count_entries("")); /* no temporary either */
}

/* Cancelled half-way (cancel is checked at least every MiB): nothing new
 * is left. */
TEST(copy_cancelled_half_way)
{
    size_t n = 8 * 1024 * 1024;
    unsigned char *p = pattern(n);
    ASSERT_TRUE(fx_write_bytes("big", p, n));
    atomic_bool cancel = false;
    progress_t pr = {0, true, 0, 0, true, &cancel};
    fs_op_t op = {&cancel, on_progress, &pr};
    ASSERT_EQ(
        FS_ERR_CANCELLED, KIND(fs_copy_file(FX("big"), FX("copy"), 0, &op)));
    ASSERT_FALSE(fx_exists("copy"));
    ASSERT_EQ(1, fx_count_entries(""));
    ASSERT_TRUE(file_is("big", p, n)); /* the source is untouched */
    free(p);
}

/* With FS_REPLACE a cancelled copy does not cost the old file. */
TEST(copy_cancelled_keeps_the_old_target)
{
    size_t n = 8 * 1024 * 1024;
    unsigned char *p = pattern(n);
    ASSERT_TRUE(fx_write_bytes("big", p, n));
    ASSERT_TRUE(fx_write_file("copy", "the old one"));
    atomic_bool cancel = false;
    progress_t pr = {0, true, 0, 0, true, &cancel};
    fs_op_t op = {&cancel, on_progress, &pr};
    ASSERT_EQ(
        FS_ERR_CANCELLED,
        KIND(fs_copy_file(FX("big"), FX("copy"), FS_REPLACE, &op)));
    ASSERT_STR_EQ("the old one", contents("copy"));
    ASSERT_EQ(2, fx_count_entries(""));
    free(p);
}

TEST(copy_errors)
{
    ASSERT_TRUE(fx_write_file("a", "x"));
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_EQ(
        FS_ERR_NOT_FOUND, KIND(fs_copy_file(FX("missing"), FX("b"), 0, NULL)));
    ASSERT_EQ(
        FS_ERR_NOT_FOUND, KIND(fs_copy_file(FX("a"), FX("no/b"), 0, NULL)));
    ASSERT_EQ(FS_ERR_NOT_DIR, KIND(fs_copy_file(FX("a"), FX("f/b"), 0, NULL)));
    ASSERT_EQ(
        FS_ERR_INVALID, KIND(fs_copy_file(P("relative"), FX("b"), 0, NULL)));
    ASSERT_EQ(
        FS_ERR_INVALID, KIND(fs_copy_file(FX("a"), P("relative"), 0, NULL)));
    ASSERT_EQ(2, fx_count_entries(""));
}

/* =========================================================================
 * fs_read_link
 * ========================================================================= */

/* A relative target comes back as stored, with '/', appended to out. */
TEST(read_link_relative_targets)
{
    ASSERT_TRUE(fx_mkdir("d"));
    if (!fx_symlink("target", "l1"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_TRUE(fx_symlink("sub/file", "l2"));
    ASSERT_TRUE(fx_symlink("../x", "d/l3"));

    ASSERT_EQ(SEQC_OK, strbuf_append(sb, P("keep:")));
    ASSERT_EQ(FS_OK, KIND(fs_read_link(FX("l1"), sb)));
    ASSERT_STR_EQ("keep:target", c(strbuf_view(sb)));

    strbuf_clear(sb);
    ASSERT_EQ(FS_OK, KIND(fs_read_link(FX("l2"), sb)));
    ASSERT_STR_EQ("sub/file", c(strbuf_view(sb)));

    strbuf_clear(sb);
    ASSERT_EQ(FS_OK, KIND(fs_read_link(FX("d/l3"), sb)));
    ASSERT_STR_EQ("../x", c(strbuf_view(sb))); /* dangling: fine */
}

/* An absolute target comes back canonical. */
TEST(read_link_absolute_target)
{
    ASSERT_TRUE(fx_write_file("target", "x"));
    if (!fx_symlink(fx_native(FX("target")), "l"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_EQ(FS_OK, KIND(fs_read_link(FX("l"), sb)));
    ASSERT_STR_EQ(c(FX("target")), c(strbuf_view(sb)));
}

TEST(read_link_errors)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_EQ(SEQC_OK, strbuf_append(sb, P("keep")));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_read_link(FX("f"), sb))); /* no link */
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_read_link(FX("missing"), sb)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_read_link(P("relative"), sb)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_read_link(FX("f"), NULL)));
    ASSERT_STR_EQ("keep", c(strbuf_view(sb))); /* unchanged */
}

/* =========================================================================
 * fs_real_path
 * ========================================================================= */

/* real_path of the test directory itself: the base the others are
 * compared with.  (The temporary directory may itself be reached through
 * a link — /tmp on macOS — or an 8.3 name on Windows.) */
static const char *real_root(void)
{
    static char buf[8192];
    strbuf_clear(sb);
    CTT_ASSERT_EQ(FS_OK, KIND(fs_real_path(FX(""), sb)));
    snprintf(buf, sizeof buf, "%s", c(strbuf_view(sb)));
    strbuf_clear(sb);
    return buf;
}

static const char *real(const char *rel)
{
    strbuf_clear(sb);
    fs_err_t err = fs_real_path(FX(rel), sb);
    return fs_ok(err) ? c(strbuf_view(sb)) : "(error)";
}

static const char *under(const char *root, const char *rel)
{
    static char buf[8192];
    snprintf(buf, sizeof buf, "%s/%s", root, rel);
    return buf;
}

TEST(real_path_of_plain_paths)
{
    ASSERT_TRUE(fx_mkdir("a"));
    ASSERT_TRUE(fx_write_file("a/f", "x"));
    const char *root = real_root();
    ASSERT_EQ('/', root[0]); /* canonical and absolute */
    ASSERT_STR_EQ(under(root, "a"), real("a"));
    ASSERT_STR_EQ(under(root, "a/f"), real("a/f"));
}

TEST(real_path_resolves_every_link)
{
    ASSERT_TRUE(fx_mkdir("real"));
    ASSERT_TRUE(fx_write_file("real/x", "1"));
    ASSERT_TRUE(fx_write_file("target", "2"));
    if (!fx_symlink("target", "l1"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_TRUE(fx_symlink("l1", "l2")); /* a chain */
    ASSERT_TRUE(fx_symlink_dir("real", "via"));
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_symlink_dir("..", "d/up"));

    const char *root = real_root();
    ASSERT_STR_EQ(under(root, "target"), real("l1"));
    ASSERT_STR_EQ(under(root, "target"), real("l2"));
    ASSERT_STR_EQ(under(root, "real/x"), real("via/x"));
    if (!fx_is_wine()) /* Wine cannot follow a directory link to ".." */
    {
        ASSERT_STR_EQ(under(root, "target"), real("d/up/target"));
    }
}

TEST(real_path_appends)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    const char *root = real_root();
    ASSERT_EQ(SEQC_OK, strbuf_append(sb, P("keep:")));
    ASSERT_EQ(FS_OK, KIND(fs_real_path(FX("f"), sb)));
    char want[8192];
    snprintf(want, sizeof want, "keep:%s/f", root);
    ASSERT_STR_EQ(want, c(strbuf_view(sb)));
}

TEST(real_path_errors)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_EQ(SEQC_OK, strbuf_append(sb, P("keep")));
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_real_path(FX("missing"), sb)));
    ASSERT_EQ(FS_ERR_NOT_DIR, KIND(fs_real_path(FX("f/x"), sb)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_real_path(P("relative"), sb)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_real_path(FX("f"), NULL)));
    ASSERT_STR_EQ("keep", c(strbuf_view(sb)));

    if (!fx_symlink("nowhere", "dangling"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_real_path(FX("dangling"), sb)));
    ASSERT_TRUE(fx_symlink("loop_b", "loop_a"));
    ASSERT_TRUE(fx_symlink("loop_a", "loop_b"));
    ASSERT_EQ(FS_ERR_LOOP, KIND(fs_real_path(FX("loop_a"), sb)));
    ASSERT_STR_EQ("keep", c(strbuf_view(sb)));
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "filec fs operations");
}
