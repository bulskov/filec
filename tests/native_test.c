/* filec/native: native <-> canonical paths.
 *
 *   path_from_native(native, style, out)   native -> canonical
 *   path_to_native(canonical, style, out)  canonical -> native
 *
 * Both append to the strbuf out and return a path_native_err_t; on failure
 * out is unchanged.  Every failure test checks that.
 *
 * Pure string code with a style parameter, so the Windows rules run on
 * Linux and the POSIX rules on Windows.  The canonical form is the one in
 * filec/path.h: '/' only, absolute = starts with '/', Windows drives and
 * shares as first components (/C:/Users/me, /UNC/server/share/x).
 *
 * In C string literals every '\' is written "\\": "C:\\Users" is C:\Users,
 * and "\\\\srv\\share" is \\srv\share. */

#include "ctt.h"

#include "filec/native.h"

#include "arena/growing_arena.h"

#include "heap_allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- helpers ------------------------------------------------------------ */

static growing_arena_t arena;
static strbuf_t *sb; /* one builder, reused by every conversion in a test */

void ctt_before_each(void)
{
    growing_arena_init(&arena, 4096);
    sb = strbuf_create(growing_arena_allocator(&arena));
}

void ctt_after_each(void)
{
    growing_arena_destroy(&arena);
}

#define P(lit) STRING_LIT(lit)
#define POSIX PATH_STYLE_POSIX
#define WIN PATH_STYLE_WINDOWS

/* string_t -> C string for ASSERT_STR_EQ. */
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

/* A conversion that must succeed, into the cleared sb: the result as a C
 * string, or "(error N)". */
static const char *convert(
    path_native_err_t (*fn)(string_t, path_style_t, strbuf_t *),
    const char *in,
    path_style_t style)
{
    strbuf_clear(sb);
    path_native_err_t err = fn(string_view_cstr(in), style, sb);
    if (err != PATH_NATIVE_OK)
    {
        static char msg[64];
        snprintf(msg, sizeof msg, "(error %d)", (int)err);
        return msg;
    }
    return c(strbuf_view(sb));
}

static const char *from(const char *native, path_style_t style)
{
    return convert(path_from_native, native, style);
}

static const char *to(const char *canonical, path_style_t style)
{
    return convert(path_to_native, canonical, style);
}

/* A conversion that must fail with `expected` and leave sb as it was. */
#define ASSERT_FAILS(fn, expected, in, style)                                  \
    do                                                                         \
    {                                                                          \
        strbuf_clear(sb);                                                      \
        strbuf_append(sb, P("keep"));                                          \
        ASSERT_EQ((expected), fn(P(in), (style), sb));                         \
        ASSERT_STR_EQ("keep", c(strbuf_view(sb)));                             \
    } while (0)

#define ASSERT_FROM_FAILS(expected, native, style)                             \
    ASSERT_FAILS(path_from_native, expected, native, style)
#define ASSERT_TO_FAILS(expected, canonical, style)                            \
    ASSERT_FAILS(path_to_native, expected, canonical, style)

/* --- from_native: POSIX ------------------------------------------------- */

TEST(posix_paths_pass_through)
{
    ASSERT_STR_EQ("/home/me/x", from("/home/me/x", POSIX));
    ASSERT_STR_EQ("/", from("/", POSIX));
    ASSERT_STR_EQ("a/b", from("a/b", POSIX));
    ASSERT_STR_EQ("../x", from("../x", POSIX));
}

TEST(posix_result_is_normalised)
{
    ASSERT_STR_EQ("/home/me", from("/home//me/./x/../", POSIX));
    ASSERT_STR_EQ("/", from("/..", POSIX));
    ASSERT_STR_EQ("a", from("./a/", POSIX));
}

/* On POSIX '\' and ':' are ordinary characters in a name. */
TEST(posix_backslash_and_colon_are_ordinary)
{
    ASSERT_STR_EQ("a\\b", from("a\\b", POSIX));
    ASSERT_STR_EQ("C:\\x", from("C:\\x", POSIX));
    ASSERT_STR_EQ("/srv/\\\\share", from("/srv/\\\\share", POSIX));
}

/* --- from_native: Windows drives ---------------------------------------- */

TEST(windows_drive_paths)
{
    ASSERT_STR_EQ("/C:/Users/me", from("C:\\Users\\me", WIN));
    ASSERT_STR_EQ("/C:/Users/me", from("C:/Users/me", WIN));
    ASSERT_STR_EQ("/C:/Users/me", from("C:\\Users/me", WIN)); /* mixed */
    ASSERT_STR_EQ("/D:/a b/c.txt", from("D:\\a b\\c.txt", WIN));
}

TEST(windows_drive_letter_is_upper_cased)
{
    ASSERT_STR_EQ("/C:/Users", from("c:\\Users", WIN));
    ASSERT_STR_EQ("/Z:", from("z:\\", WIN));
    /* ...only the drive letter: names keep their case. */
    ASSERT_STR_EQ("/C:/users/Me", from("c:\\users\\Me", WIN));
}

TEST(windows_drive_root)
{
    ASSERT_STR_EQ("/C:", from("C:\\", WIN));
    ASSERT_STR_EQ("/C:", from("C:/", WIN));
}

TEST(windows_result_is_normalised)
{
    ASSERT_STR_EQ("/C:/a/c", from("C:\\a\\\\b\\..\\.\\c\\", WIN));
}

/* On Windows C:\.. is C:\ — ".." never leaves the drive.  (Plain
 * path_normalize would climb to the virtual root "/".) */
TEST(windows_dotdot_stops_at_the_drive_root)
{
    ASSERT_STR_EQ("/C:", from("C:\\..", WIN));
    ASSERT_STR_EQ("/C:/x", from("C:\\..\\..\\x", WIN));
}

/* --- from_native: Windows shares (UNC) ---------------------------------- */

TEST(windows_unc_paths)
{
    ASSERT_STR_EQ("/UNC/srv/share/x", from("\\\\srv\\share\\x", WIN));
    ASSERT_STR_EQ("/UNC/srv/share/x", from("//srv/share/x", WIN));
    ASSERT_STR_EQ("/UNC/srv/share", from("\\\\srv\\share", WIN));
    ASSERT_STR_EQ("/UNC/srv/share", from("\\\\srv\\share\\", WIN));
    /* server and share names keep their case */
    ASSERT_STR_EQ("/UNC/Srv/Share", from("\\\\Srv\\Share", WIN));
}

/* A share is the root of a UNC path: ".." stops there, like at a drive. */
TEST(windows_dotdot_stops_at_the_share_root)
{
    ASSERT_STR_EQ("/UNC/srv/share/x", from("\\\\srv\\share\\..\\x", WIN));
}

TEST(windows_unc_needs_server_and_share)
{
    ASSERT_FROM_FAILS(PATH_NATIVE_INVALID, "\\\\srv", WIN);
    ASSERT_FROM_FAILS(PATH_NATIVE_INVALID, "\\\\srv\\", WIN);
    ASSERT_FROM_FAILS(PATH_NATIVE_INVALID, "\\\\", WIN);
}

/* --- from_native: the \\?\ long-path prefix ----------------------------- */

TEST(windows_long_path_prefix_is_stripped)
{
    ASSERT_STR_EQ("/C:/very/long", from("\\\\?\\C:\\very\\long", WIN));
    ASSERT_STR_EQ("/C:/x", from("\\\\?\\c:\\x", WIN));
    ASSERT_STR_EQ("/UNC/srv/share/x", from("\\\\?\\UNC\\srv\\share\\x", WIN));
}

/* \\.\ is the device namespace (COM1, PhysicalDrive0): not files. */
TEST(windows_device_paths_are_invalid)
{
    ASSERT_FROM_FAILS(PATH_NATIVE_INVALID, "\\\\.\\COM1", WIN);
    ASSERT_FROM_FAILS(PATH_NATIVE_INVALID, "\\\\.\\PhysicalDrive0", WIN);
}

/* --- from_native: forms that need the current directory ----------------- */

/* "C:x" and "C:" mean "on drive C, relative to C's current directory". */
TEST(windows_drive_relative_is_refused)
{
    ASSERT_FROM_FAILS(PATH_NATIVE_DRIVE_RELATIVE, "C:x", WIN);
    ASSERT_FROM_FAILS(PATH_NATIVE_DRIVE_RELATIVE, "C:", WIN);
    ASSERT_FROM_FAILS(PATH_NATIVE_DRIVE_RELATIVE, "c:..\\x", WIN);
}

/* "\x" (and "/x") mean "x at the root of the current drive". */
TEST(windows_root_relative_is_refused)
{
    ASSERT_FROM_FAILS(PATH_NATIVE_ROOT_RELATIVE, "\\x", WIN);
    ASSERT_FROM_FAILS(PATH_NATIVE_ROOT_RELATIVE, "/x", WIN);
    ASSERT_FROM_FAILS(PATH_NATIVE_ROOT_RELATIVE, "\\", WIN);
}

TEST(windows_relative_paths)
{
    ASSERT_STR_EQ("a/b", from("a\\b", WIN));
    ASSERT_STR_EQ("../x", from("..\\x", WIN));
    ASSERT_STR_EQ("a/b", from("a/b", WIN));
}

/* --- from_native: both styles ------------------------------------------- */

TEST(empty_is_the_current_directory)
{
    ASSERT_STR_EQ(".", from("", POSIX));
    ASSERT_STR_EQ(".", from("", WIN));
}

/* A NUL byte cannot be passed to any OS call: no path contains one. */
TEST(nul_byte_is_invalid)
{
    const char posix[] = {'/', 'a', '\0', 'b'};
    const char win[] = {'C', ':', '\\', 'a', '\0', 'b'};
    strbuf_clear(sb);
    ASSERT_EQ(
        PATH_NATIVE_INVALID, path_from_native((string_t){posix, 4}, POSIX, sb));
    ASSERT_EQ(
        PATH_NATIVE_INVALID, path_from_native((string_t){win, 6}, WIN, sb));
    ASSERT_EQ(0u, strbuf_len(sb));
}

TEST(input_need_not_be_nul_terminated)
{
    const char buf[] = "C:\\a\\bGARBAGE";
    strbuf_clear(sb);
    ASSERT_EQ(PATH_NATIVE_OK, path_from_native((string_t){buf, 6}, WIN, sb));
    ASSERT_STR_EQ("/C:/a/b", c(strbuf_view(sb)));
}

TEST(null_out_is_invalid)
{
    ASSERT_EQ(PATH_NATIVE_INVALID, path_from_native(P("a"), POSIX, NULL));
    ASSERT_EQ(PATH_NATIVE_INVALID, path_to_native(P("a"), POSIX, NULL));
}

/* --- appending ---------------------------------------------------------- */

/* The result goes after what out already holds... */
TEST(results_are_appended)
{
    strbuf_clear(sb);
    strbuf_append(sb, P("from: "));
    ASSERT_EQ(PATH_NATIVE_OK, path_from_native(P("c:\\a"), WIN, sb));
    strbuf_append(sb, P(", to: "));
    ASSERT_EQ(PATH_NATIVE_OK, path_to_native(P("/C:/a"), WIN, sb));
    ASSERT_STR_EQ("from: /C:/a, to: C:\\a", c(strbuf_view(sb)));
}

/* ...and normalising never reaches back into it: ".." works only on the
 * names this call appended. */
TEST(dotdot_never_reaches_into_earlier_content)
{
    strbuf_clear(sb);
    strbuf_append(sb, P("/base/"));
    ASSERT_EQ(PATH_NATIVE_OK, path_from_native(P("x/../../y"), POSIX, sb));
    ASSERT_STR_EQ("/base/../y", c(strbuf_view(sb)));

    strbuf_clear(sb);
    strbuf_append(sb, P("/base"));
    ASSERT_EQ(PATH_NATIVE_OK, path_from_native(P("/.."), POSIX, sb));
    ASSERT_STR_EQ("/base/", c(strbuf_view(sb)));
}

/* --- to_native ---------------------------------------------------------- */

TEST(to_native_posix_is_the_identity)
{
    ASSERT_STR_EQ("/home/me/x", to("/home/me/x", POSIX));
    ASSERT_STR_EQ("/", to("/", POSIX));
    ASSERT_STR_EQ("a/b", to("a/b", POSIX));
    ASSERT_STR_EQ(".", to(".", POSIX));
}

TEST(to_native_windows_drives)
{
    ASSERT_STR_EQ("C:\\Users\\me", to("/C:/Users/me", WIN));
    /* the drive root keeps its backslash: "C:" alone is drive-relative */
    ASSERT_STR_EQ("C:\\", to("/C:", WIN));
}

TEST(to_native_windows_shares)
{
    ASSERT_STR_EQ("\\\\srv\\share\\x", to("/UNC/srv/share/x", WIN));
    ASSERT_STR_EQ("\\\\srv\\share", to("/UNC/srv/share", WIN));
}

TEST(to_native_windows_relative)
{
    ASSERT_STR_EQ("a\\b", to("a/b", WIN));
    ASSERT_STR_EQ("..\\x", to("../x", WIN));
    ASSERT_STR_EQ(".", to(".", WIN));
}

/* The virtual root, and anything that is not under a drive or a share,
 * has no Windows form. */
TEST(to_native_windows_refuses_virtual_paths)
{
    ASSERT_TO_FAILS(PATH_NATIVE_NO_NATIVE_FORM, "/", WIN);
    ASSERT_TO_FAILS(PATH_NATIVE_NO_NATIVE_FORM, "/home/me", WIN);
    ASSERT_TO_FAILS(PATH_NATIVE_NO_NATIVE_FORM, "/UNC", WIN);
    ASSERT_TO_FAILS(PATH_NATIVE_NO_NATIVE_FORM, "/UNC/srv", WIN);
}

/* --- round trips -------------------------------------------------------- */

TEST(canonical_survives_a_round_trip)
{
    strbuf_t *native = strbuf_create(growing_arena_allocator(&arena));
    static const char *win[] = {
        "/C:",
        "/C:/Users/me",
        "/UNC/srv/share",
        "/UNC/srv/share/x",
        "a/b",
        "../x",
        "."};
    for (size_t i = 0; i < sizeof win / sizeof win[0]; ++i)
    {
        strbuf_clear(native);
        ASSERT_EQ(
            PATH_NATIVE_OK,
            path_to_native(string_view_cstr(win[i]), WIN, native));
        strbuf_clear(sb);
        ASSERT_EQ(
            PATH_NATIVE_OK, path_from_native(strbuf_view(native), WIN, sb));
        ASSERT_STR_EQ(win[i], c(strbuf_view(sb)));
    }
    static const char *posix[] = {"/", "/home/me", "a/b", "../x", "."};
    for (size_t i = 0; i < sizeof posix / sizeof posix[0]; ++i)
    {
        strbuf_clear(native);
        ASSERT_EQ(
            PATH_NATIVE_OK,
            path_to_native(string_view_cstr(posix[i]), POSIX, native));
        strbuf_clear(sb);
        ASSERT_EQ(
            PATH_NATIVE_OK, path_from_native(strbuf_view(native), POSIX, sb));
        ASSERT_STR_EQ(posix[i], c(strbuf_view(sb)));
    }
}

/* --- memory ------------------------------------------------------------- */

/* Run fn with every possible number of allocations allowed before the
 * first failure.  Each run either fails with OOM and leaves out exactly as
 * it was, or succeeds with the full result. */
static void check_oom_points(
    path_native_err_t (*fn)(string_t, path_style_t, strbuf_t *),
    const char *in,
    path_style_t style,
    const char *expected)
{
    size_t budget = SIZE_MAX;
    strbuf_t *out = strbuf_create(heap_with_budget(&budget));
    ASSERT_NOT_NULL(out);
    bool saw_oom = false;
    for (size_t n = 0;; ++n)
    {
        strbuf_clear(out);
        strbuf_append(out, P("keep"));
        budget = n;
        path_native_err_t err = fn(string_view_cstr(in), style, out);
        budget = SIZE_MAX;
        if (err == PATH_NATIVE_OK)
        {
            char want[256];
            snprintf(want, sizeof want, "keep%s", expected);
            ASSERT_STR_EQ(want, c(strbuf_view(out)));
            break;
        }
        ASSERT_EQ(PATH_NATIVE_OOM, err);
        ASSERT_STR_EQ("keep", c(strbuf_view(out)));
        saw_oom = true;
    }
    ASSERT_TRUE(saw_oom);
    strbuf_destroy(out);
}

TEST(oom_leaves_out_unchanged)
{
    check_oom_points(
        path_from_native,
        "c:\\Users\\me\\projects\\fskim\\src\\..\\docs",
        WIN,
        "/C:/Users/me/projects/fskim/docs");
    check_oom_points(
        path_from_native,
        "\\\\?\\UNC\\server\\share\\a\\long\\enough\\path",
        WIN,
        "/UNC/server/share/a/long/enough/path");
    check_oom_points(
        path_from_native,
        "/home/me/projects/fskim/src/../docs",
        POSIX,
        "/home/me/projects/fskim/docs");
    check_oom_points(
        path_to_native,
        "/C:/Users/me/projects/fskim/docs",
        WIN,
        "C:\\Users\\me\\projects\\fskim\\docs");
    check_oom_points(
        path_to_native,
        "/UNC/server/share/a/long/enough/path",
        WIN,
        "\\\\server\\share\\a\\long\\enough\\path");
    check_oom_points(
        path_to_native,
        "/home/me/projects/fskim/docs",
        POSIX,
        "/home/me/projects/fskim/docs");
}

/* With a malloc-backed builder nothing is left behind — the caller's
 * strbuf_destroy (and mem_free of a kept copy) give everything back.
 * Checked under ./test.sh asan. */
TEST(nothing_leaks)
{
    allocator_t h = heap();
    strbuf_t *out = strbuf_create(h);
    ASSERT_EQ(PATH_NATIVE_OK, path_from_native(P("c:\\a\\..\\b"), WIN, out));
    ASSERT_STR_EQ("/C:/b", c(strbuf_view(out)));
    string_t kept = strbuf_to_string(out, h);

    strbuf_clear(out);
    ASSERT_EQ(PATH_NATIVE_OK, path_to_native(kept, WIN, out));
    ASSERT_STR_EQ("C:\\b", c(strbuf_view(out)));
    ASSERT_EQ(PATH_NATIVE_INVALID, path_from_native(P("\\\\srv"), WIN, out));

    mem_free(h, (void *)kept.ptr, kept.len);
    strbuf_destroy(out);
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "filec native");
}
