/* filec/path: canonical paths.
 *
 * The canonical form (see include/filec/path.h): '/' is the only separator,
 * absolute means "starts with /", and Windows drives are first components
 * ("/C:/Users/me").  No platform anywhere, so every test here runs the same
 * way on Linux, macOS and Windows.
 *
 * Inputs are normalised unless a test is about normalising.  Building
 * functions append to sb, a strbuf from a fresh arena per test; every
 * failure test checks that out is left as it was. */

#include "ctt.h"
#include "filec/path.h"

#include "arena/growing_arena.h"

#include "heap_allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- helpers ------------------------------------------------------------ */

static growing_arena_t arena;
static allocator_t A;
static strbuf_t *sb; /* the out of every building call in a test */

void ctt_before_each(void)
{
    growing_arena_init(&arena, 4096);
    A = growing_arena_allocator(&arena);
    sb = strbuf_create(A);
}

void ctt_after_each(void)
{
    growing_arena_destroy(&arena);
}

#define P(lit) STRING_LIT(lit)

/* string_t -> C string for ASSERT_STR_EQ.  Rotates through a few buffers so
 * one assertion can show several.  {NULL, 0} prints as "(null)". */
static const char *c(string_t s)
{
    static char bufs[4][1024];
    static int next;
    if (!s.ptr)
    {
        return "(null)";
    }
    char *b = bufs[next++ % 4];
    size_t n = s.len < sizeof bufs[0] - 1 ? s.len : sizeof bufs[0] - 1;
    memcpy(b, s.ptr, n);
    b[n] = '\0';
    return b;
}

/* The components of p, each in brackets so an empty one shows:
 * "/a/b" -> "[/][a][b]".  No components at all gives "". */
static const char *components_of(string_t p)
{
    static char out[1024];
    size_t len = 0;
    out[0] = '\0';
    iter_t it = path_components(p, A);
    string_t part;
    while (it.next(&it, &part))
    {
        len += (size_t)snprintf(
            out + len,
            sizeof out - len,
            "[%.*s]",
            (int)part.len,
            part.ptr ? part.ptr : "");
    }
    iter_destroy(&it);
    return out;
}

/* The result of a building call into the cleared sb, as a C string;
 * "(error N)" if it failed.  RESULT(path_join(a, b, sb)). */
static const char *result_of(path_err_t err)
{
    if (err != PATH_OK)
    {
        static char msg[64];
        snprintf(msg, sizeof msg, "(error %d)", (int)err);
        return msg;
    }
    return c(strbuf_view(sb));
}
#define RESULT(call) (strbuf_clear(sb), result_of(call))

#define CS(lit) string_view_cstr(lit)

/* A building call that must fail with `expected` and leave sb as it was. */
#define ASSERT_FAILS(expected, call)                                           \
    do                                                                         \
    {                                                                          \
        strbuf_clear(sb);                                                      \
        strbuf_append(sb, P("keep"));                                          \
        ASSERT_EQ((expected), (call));                                         \
        ASSERT_STR_EQ("keep", c(strbuf_view(sb)));                             \
    } while (0)

/* path_join_iter over a list of C strings, into sb. */
static const char *join_all(const char *const *parts, size_t n)
{
    string_t items[16];
    for (size_t i = 0; i < n; ++i)
    {
        items[i] = string_view_cstr(parts[i]);
    }
    slice_t s = {items, n, sizeof(string_t)};
    return RESULT(path_join_iter(iter_from_slice(s, A), sb));
}

#define S PATH_CASE_SENSITIVE
#define I PATH_CASE_INSENSITIVE

/* --- path_is_absolute --------------------------------------------------- */

TEST(is_absolute)
{
    ASSERT_TRUE(path_is_absolute(P("/")));
    ASSERT_TRUE(path_is_absolute(P("/a/b")));
    ASSERT_TRUE(path_is_absolute(P("/C:/Users")));
    ASSERT_FALSE(path_is_absolute(P("a/b")));
    ASSERT_FALSE(path_is_absolute(P(".")));
    ASSERT_FALSE(path_is_absolute(P("..")));
    ASSERT_FALSE(path_is_absolute(P("")));
}

/* --- anatomy ------------------------------------------------------------ */

typedef struct
{
    const char *path, *parent, *name, *stem, *ext;
} anatomy_t;

static const anatomy_t anatomy[] = {
    /* path            parent   name          stem       ext  */
    {"/a/b.tar.gz", "/a", "b.tar.gz", "b.tar", "gz"},
    {"/x/.bashrc", "/x", ".bashrc", ".bashrc", ""},
    {"/x/a.", "/x", "a.", "a.", ""},
    {"/x/noext", "/x", "noext", "noext", ""},
    {"/a.b/c", "/a.b", "c", "c", ""}, /* a dot in a directory is not ext */
    {"/a", "/", "a", "a", ""},
    {"/", "/", "", "", ""},
    {"/C:/Users", "/C:", "Users", "Users", ""},
    {"/C:", "/", "C:", "C:", ""},
    {"/UNC/srv/share", "/UNC/srv", "share", "share", ""},
    {"a/b.txt", "a", "b.txt", "b", "txt"},
    {"a", ".", "a", "a", ""},
    {".", ".", "", "", ""},
    {"..", ".", "..", "..", ""},
    {"../x", "..", "x", "x", ""},
    {"/a/b c/d e.txt", "/a/b c", "d e.txt", "d e", "txt"},
    {"/\xc3\xa6\xc3\xb8/\xc3\xa5.txt",
     "/\xc3\xa6\xc3\xb8",
     "\xc3\xa5.txt",
     "\xc3\xa5",
     "txt"}, /* UTF-8 is just bytes */
};

TEST(parent)
{
    for (size_t i = 0; i < sizeof anatomy / sizeof anatomy[0]; ++i)
    {
        ASSERT_STR_EQ(
            anatomy[i].parent,
            c(path_parent(string_view_cstr(anatomy[i].path))));
    }
}

TEST(file_name)
{
    for (size_t i = 0; i < sizeof anatomy / sizeof anatomy[0]; ++i)
    {
        ASSERT_STR_EQ(
            anatomy[i].name,
            c(path_file_name(string_view_cstr(anatomy[i].path))));
    }
}

TEST(stem)
{
    for (size_t i = 0; i < sizeof anatomy / sizeof anatomy[0]; ++i)
    {
        ASSERT_STR_EQ(
            anatomy[i].stem, c(path_stem(string_view_cstr(anatomy[i].path))));
    }
}

TEST(extension)
{
    for (size_t i = 0; i < sizeof anatomy / sizeof anatomy[0]; ++i)
    {
        ASSERT_STR_EQ(
            anatomy[i].ext,
            c(path_extension(string_view_cstr(anatomy[i].path))));
    }
}

/* Anatomy results are views into the input — no copy, no allocation. */
TEST(anatomy_returns_views_into_the_input)
{
    string_t p = P("/dir/file.txt");
    const char *lo = p.ptr, *hi = p.ptr + p.len;
    string_t parts[] = {
        path_parent(p), path_file_name(p), path_stem(p), path_extension(p)};
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i)
    {
        ASSERT_TRUE(parts[i].ptr >= lo);
        ASSERT_TRUE(parts[i].ptr + parts[i].len <= hi);
    }
}

/* Paths are string_t, not C strings: nothing may read past len. */
TEST(input_need_not_be_nul_terminated)
{
    const char buf[] = "/a/b/c.txtGARBAGE";
    string_t p = {buf, 10}; /* "/a/b/c.txt" */
    ASSERT_STR_EQ("/a/b", c(path_parent(p)));
    ASSERT_STR_EQ("c.txt", c(path_file_name(p)));
    ASSERT_STR_EQ("txt", c(path_extension(p)));
    ASSERT_STR_EQ("/a/b/c.txt", RESULT(path_normalize(p, sb)));
}

/* --- path_components ---------------------------------------------------- */

TEST(components)
{
    ASSERT_STR_EQ("[/][a][b]", components_of(P("/a/b")));
    ASSERT_STR_EQ("[/]", components_of(P("/")));
    ASSERT_STR_EQ("[/][C:][Users]", components_of(P("/C:/Users")));
    ASSERT_STR_EQ("[a][b]", components_of(P("a/b")));
    ASSERT_STR_EQ("[..][..][x]", components_of(P("../../x")));
    ASSERT_STR_EQ("", components_of(P("."))); /* nothing — not one "" */
}

/* seqc adaptors (iter_map, iter_collect, ...) rely on elem_size. */
TEST(components_iterator_has_string_elements)
{
    iter_t it = path_components(P("/a"), A);
    ASSERT_EQ(sizeof(string_t), it.elem_size);
    iter_destroy(&it);
}

/* components and join_iter are inverses. */
TEST(components_round_trip_through_join_iter)
{
    const char *paths[] = {"/a/b/c", "/", "/C:/x", "a/b", "../x", "."};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i)
    {
        string_t p = string_view_cstr(paths[i]);
        ASSERT_STR_EQ(
            paths[i], RESULT(path_join_iter(path_components(p, A), sb)));
    }
}

/* --- path_normalize ----------------------------------------------------- */

TEST(normalize)
{
    static const char *cases[][2] = {
        {"", "."},
        {".", "."},
        {"./", "."},
        {"/", "/"},
        {"//", "/"},
        {"/a//b/", "/a/b"},
        {"/a/./b", "/a/b"},
        {"/a/b/..", "/a"},
        {"/a/b/../..", "/"},
        {"/..", "/"}, /* never above the root */
        {"/../a", "/a"},
        {"/a/../../b", "/b"},
        {"a/..", "."},
        {"a/../..", ".."}, /* a relative path keeps leading .. */
        {"../a/../b", "../b"},
        {"../..", "../.."},
        {"./a", "a"},
        {"a/./b/./", "a/b"},
        {"/a//./b/../c/", "/a/c"},
        {"/C:/x/../../D:/y", "/D:/y"},
        {"/C:/..", "/"}, /* up from a drive is the virtual root */
        {"...", "..."},  /* not special: an ordinary name */
        {"/a/.../b", "/a/.../b"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
    {
        ASSERT_STR_EQ(cases[i][1], RESULT(path_normalize(CS(cases[i][0]), sb)));
    }
}

/* The second pass reads from the first strbuf and writes to another: the
 * input must never point into out. */
TEST(normalize_is_idempotent)
{
    strbuf_t *once = strbuf_create(A);
    const char *paths[] = {"/a//b/../c/./", "a/../../x", "", "/../.."};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i)
    {
        strbuf_clear(once);
        ASSERT_EQ(PATH_OK, path_normalize(CS(paths[i]), once));
        ASSERT_STR_EQ(
            c(strbuf_view(once)),
            RESULT(path_normalize(strbuf_view(once), sb)));
    }
}

/* No fixed-size buffers: 1000 levels down and 1000 back up. */
TEST(normalize_handles_deep_paths)
{
    static char buf[1 + 1000 * 2 + 1000 * 3];
    size_t n = 0;
    buf[n++] = '/';
    for (int i = 0; i < 1000; ++i)
    {
        buf[n++] = 'x';
        buf[n++] = '/';
    }
    for (int i = 0; i < 1000; ++i)
    {
        memcpy(buf + n, "../", 3);
        n += 3;
    }
    string_t deep = {buf, n};
    ASSERT_STR_EQ("/", RESULT(path_normalize(deep, sb)));

    string_t down = {buf, 1 + 1000 * 2};
    strbuf_clear(sb);
    ASSERT_EQ(PATH_OK, path_normalize(down, sb));
    ASSERT_EQ(1 + 1000 * 2 - 1, strbuf_len(sb)); /* only the trailing '/' */
}

/* --- path_join ---------------------------------------------------------- */

TEST(join)
{
    static const char *cases[][3] = {
        {"/a", "b", "/a/b"},
        {"/a", "b/c", "/a/b/c"},
        {"/a", "/b", "/b"}, /* an absolute right side replaces the left */
        {"a", "b", "a/b"},
        {"/a/b", "../c", "/a/c"},
        {"/a", "", "/a"},
        {"", "b", "b"},
        {"", "", "."},
        {"/", "..", "/"},
        {"/", "C:", "/C:"},
        {"/a/", "./b//c/", "/a/b/c"}, /* unnormalised input is fine */
        {"..", "..", "../.."},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
    {
        ASSERT_STR_EQ(
            cases[i][2],
            RESULT(path_join(CS(cases[i][0]), CS(cases[i][1]), sb)));
    }
}

TEST(join_iter)
{
    const char *usr[] = {"/", "usr", "lib"};
    ASSERT_STR_EQ("/usr/lib", join_all(usr, 3));

    const char *rel[] = {"a", "..", ".."};
    ASSERT_STR_EQ("..", join_all(rel, 3));

    const char *abs_wins[] = {"/a", "b", "/c", "d"};
    ASSERT_STR_EQ("/c/d", join_all(abs_wins, 4));

    ASSERT_STR_EQ(".", join_all(NULL, 0));
}

/* --- path_with_extension ------------------------------------------------ */

TEST(with_extension)
{
    static const char *cases[][3] = {
        {"/a/b.txt", "md", "/a/b.md"},
        {"/a/b", "md", "/a/b.md"},
        {"/a/b.txt", "", "/a/b"},
        {"/a/b", "", "/a/b"},
        {"a.tar.gz", "zip", "a.tar.zip"},
        {"/x/.bashrc", "bak", "/x/.bashrc.bak"}, /* .bashrc has no ext */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
    {
        ASSERT_STR_EQ(
            cases[i][2],
            RESULT(path_with_extension(CS(cases[i][0]), CS(cases[i][1]), sb)));
    }
}

/* "/" and "." have no file name, so nothing to put an extension on. */
TEST(with_extension_needs_a_file_name)
{
    ASSERT_FAILS(PATH_INVALID, path_with_extension(P("/"), P("md"), sb));
    ASSERT_FAILS(PATH_INVALID, path_with_extension(P("."), P("md"), sb));
}

/* --- path_relative ------------------------------------------------------ */

TEST(relative)
{
    static const char *cases[][3] = {
        {"/a/b", "/a/c/d", "../c/d"},
        {"/a", "/a", "."},
        {"/a", "/a/b", "b"},
        {"/a/b/c", "/a", "../.."},
        {"/", "/x/y", "x/y"},
        {"/x/y", "/", "../.."},
        {"/ab", "/a/b", "../a/b"},        /* component-wise, not byte-wise */
        {"/C:/x", "/D:/y", "../../D:/y"}, /* through the virtual root */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
    {
        ASSERT_STR_EQ(
            cases[i][2],
            RESULT(path_relative(CS(cases[i][0]), CS(cases[i][1]), S, sb)));
    }
}

TEST(relative_honours_case_mode)
{
    ASSERT_STR_EQ(
        "x",
        RESULT(path_relative(P("/C:/Users/Me"), P("/C:/users/me/x"), I, sb)));
    ASSERT_STR_EQ(
        "../../users/me/x",
        RESULT(path_relative(P("/C:/Users/Me"), P("/C:/users/me/x"), S, sb)));
}

TEST(relative_needs_two_absolute_paths)
{
    ASSERT_FAILS(PATH_INVALID, path_relative(P("a/b"), P("/a"), S, sb));
    ASSERT_FAILS(PATH_INVALID, path_relative(P("/a"), P("b"), S, sb));
}

/* --- comparing ---------------------------------------------------------- */

TEST(equals)
{
    ASSERT_TRUE(path_equals(P("/a/b"), P("/a/b"), S));
    ASSERT_FALSE(path_equals(P("/a/b"), P("/a/c"), S));
    ASSERT_FALSE(path_equals(P("/a/b"), P("/a/b/c"), S));
    ASSERT_FALSE(path_equals(P("/a"), P("a"), S));

    ASSERT_FALSE(path_equals(P("/C:/Users"), P("/c:/users"), S));
    ASSERT_TRUE(path_equals(P("/C:/Users"), P("/c:/users"), I));
}

/* Component by component: a directory sorts right before its contents. */
TEST(compare_orders_component_by_component)
{
    ASSERT_EQ(0, path_compare(P("/a/b"), P("/a/b"), S));
    ASSERT_LT(path_compare(P("/a/b"), P("/a/c"), S), 0);
    ASSERT_GT(path_compare(P("/a/c"), P("/a/b"), S), 0);
    ASSERT_LT(path_compare(P("/a"), P("/a/b"), S), 0);
    ASSERT_LT(path_compare(P("/a/b"), P("/a-b"), S), 0); /* not byte order */
    ASSERT_LT(path_compare(P("/a/z"), P("/aa"), S), 0);
}

TEST(compare_honours_case_mode)
{
    ASSERT_EQ(0, path_compare(P("/a/B"), P("/A/b"), I));
    ASSERT_NE(0, path_compare(P("/a/B"), P("/A/b"), S));
    ASSERT_LT(path_compare(P("/a"), P("/B"), I), 0); /* folded: a < b */
}

TEST(starts_with_matches_whole_components)
{
    ASSERT_TRUE(path_starts_with(P("/a/b"), P("/a"), S));
    ASSERT_TRUE(path_starts_with(P("/a"), P("/a"), S));
    ASSERT_TRUE(path_starts_with(P("/a/b"), P("/"), S));
    ASSERT_TRUE(path_starts_with(P("a/b"), P("a"), S));
    ASSERT_FALSE(path_starts_with(P("/ab"), P("/a"), S));
    ASSERT_FALSE(path_starts_with(P("/a"), P("/a/b"), S));
    ASSERT_FALSE(path_starts_with(P("a/b"), P("/a"), S));

    ASSERT_FALSE(path_starts_with(P("/C:/Users/me"), P("/c:/users"), S));
    ASSERT_TRUE(path_starts_with(P("/C:/Users/me"), P("/c:/users"), I));
}

/* An empty prefix is not a normalised path, but it must not crash (the
 * boundary check looks at prefix's last byte).  Like string_starts_with,
 * every path starts with "". */
TEST(starts_with_empty_prefix)
{
    ASSERT_TRUE(path_starts_with(P("/a"), P(""), S));
    ASSERT_TRUE(path_starts_with(P("a/b"), P(""), I));
}

/* Equal paths hash equal — in each case mode, with the matching equals. */
TEST(hash_agrees_with_equals)
{
    ASSERT_EQ(path_hash(P("/a/b"), S), path_hash(P("/a/b"), S));
    ASSERT_EQ(path_hash(P("/C:/Users"), I), path_hash(P("/c:/USERS"), I));
    /* Not required, but a hash that ignores everything would be useless: */
    ASSERT_NE(path_hash(P("/a/b"), S), path_hash(P("/a/c"), S));
}

/* --- appending -------------------------------------------------------- */

/* Results go after what out holds, and ".." never reaches back into it. */
TEST(results_are_appended)
{
    strbuf_clear(sb);
    strbuf_append(sb, P("cd "));
    ASSERT_EQ(PATH_OK, path_join(P("/a/b"), P("../c"), sb));
    strbuf_append(sb, P(" && ls "));
    ASSERT_EQ(PATH_OK, path_relative(P("/a/c"), P("/a/d"), S, sb));
    ASSERT_STR_EQ("cd /a/c && ls ../d", c(strbuf_view(sb)));

    strbuf_clear(sb);
    strbuf_append(sb, P("/base/"));
    ASSERT_EQ(PATH_OK, path_normalize(P("x/../../y"), sb));
    ASSERT_STR_EQ("/base/../y", c(strbuf_view(sb)));
}

TEST(null_out_is_invalid)
{
    ASSERT_EQ(PATH_INVALID, path_normalize(P("/a"), NULL));
    ASSERT_EQ(PATH_INVALID, path_join(P("/a"), P("b"), NULL));
    ASSERT_EQ(PATH_INVALID, path_with_extension(P("/a"), P("b"), NULL));
    ASSERT_EQ(PATH_INVALID, path_relative(P("/a"), P("/b"), S, NULL));

    /* the iterator is consumed even so — LeakSanitizer checks with heap() */
    string_t items[] = {P("/"), P("a")};
    slice_t s = {items, 2, sizeof(string_t)};
    ASSERT_EQ(PATH_INVALID, path_join_iter(iter_from_slice(s, heap()), NULL));
}

/* --- out of memory ------------------------------------------------------ */

/* Runs `call` (which appends to a strbuf named out) once for every number
 * of allocations allowed before the first failure.  Each run either fails
 * with PATH_OOM and leaves out exactly as it was, or succeeds with the full
 * result.  The inputs are long enough that out must grow at least once. */
#define CHECK_OOM_POINTS(expected, call)                                       \
    do                                                                         \
    {                                                                          \
        bool saw_oom_ = false;                                                 \
        for (size_t n_ = 0;; ++n_)                                             \
        {                                                                      \
            size_t budget_ = SIZE_MAX;                                         \
            strbuf_t *out = strbuf_create(heap_with_budget(&budget_));         \
            ASSERT_NOT_NULL(out);                                              \
            strbuf_append(out, P("keep"));                                     \
            budget_ = n_;                                                      \
            path_err_t err_ = (call);                                          \
            budget_ = SIZE_MAX;                                                \
            const char *got_ = c(strbuf_view(out));                            \
            strbuf_destroy(out);                                               \
            if (err_ == PATH_OK)                                               \
            {                                                                  \
                ASSERT_STR_EQ("keep" expected, got_);                          \
                break;                                                         \
            }                                                                  \
            ASSERT_EQ(PATH_OOM, err_);                                         \
            ASSERT_STR_EQ("keep", got_);                                       \
            saw_oom_ = true;                                                   \
        }                                                                      \
        ASSERT_TRUE(saw_oom_);                                                 \
    } while (0)

#define LONG_DIR "/home/me/projects/fskim/src/very/deep/directory/tree"

TEST(oom_leaves_out_unchanged)
{
    CHECK_OOM_POINTS(LONG_DIR, path_normalize(P(LONG_DIR "/./x/../"), out));
    CHECK_OOM_POINTS(
        LONG_DIR "/file.txt", path_join(P(LONG_DIR), P("x/../file.txt"), out));
    CHECK_OOM_POINTS(
        LONG_DIR "/file.md",
        path_with_extension(P(LONG_DIR "/file.txt"), P("md"), out));
    CHECK_OOM_POINTS(
        "../../../../../../../other/place/entirely/x",
        path_relative(
            P(LONG_DIR), P("/home/me/other/place/entirely/x"), S, out));

    string_t items[] = {P(LONG_DIR), P(".."), P("tree"), P("leaf")};
    slice_t s = {items, 4, sizeof(string_t)};
    CHECK_OOM_POINTS(
        LONG_DIR "/leaf", path_join_iter(iter_from_slice(s, A), out));
}

/* --- ownership with a real allocator ------------------------------------ */
/* The arena used everywhere else never frees anything one by one, so it
 * hides memory the functions forget to free.  With a malloc-based out and
 * iterator, LeakSanitizer (./test.sh asan) sees every allocation: the
 * caller's strbuf_destroy, mem_free of a kept copy, and the iterator that
 * path_join_iter consumes must give everything back. */
TEST(nothing_leaks)
{
    allocator_t h = heap();
    strbuf_t *out = strbuf_create(h);

    ASSERT_EQ(PATH_OK, path_normalize(P("/a//b/../c/"), out));
    string_t kept = strbuf_to_string(out, h);
    ASSERT_STR_EQ("/a/c", c(kept));

    strbuf_clear(out);
    ASSERT_EQ(PATH_OK, path_join(kept, P("../d"), out));
    ASSERT_STR_EQ("/a/d", c(strbuf_view(out)));

    string_t items[] = {P("/"), P("usr"), P("lib")};
    slice_t s = {items, 3, sizeof(string_t)};
    strbuf_clear(out);
    ASSERT_EQ(PATH_OK, path_join_iter(iter_from_slice(s, h), out));
    ASSERT_STR_EQ("/usr/lib", c(strbuf_view(out)));

    strbuf_clear(out);
    ASSERT_EQ(PATH_OK, path_with_extension(P("/a/b.txt"), P("md"), out));
    ASSERT_EQ(PATH_OK, path_relative(P("/a/b"), P("/a/c/d"), S, out));
    ASSERT_STR_EQ("/a/b.md../c/d", c(strbuf_view(out)));

    mem_free(h, (void *)kept.ptr, kept.len);
    strbuf_destroy(out);
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "filec path");
}
