/* filec/fs step 1: errors, fs_mkdir, fs_remove, fs_stat, fs_exists.
 *
 * Every test gets a fresh temporary directory (tests/fs_fixture.h) that is
 * created and deleted with OS calls, never with the functions under test.
 * FX("a/b") is the canonical path of a/b inside it.
 *
 * The same tests run against the POSIX backend (Linux, macOS) and the
 * Win32 backend: where the platforms differ, the API picks one meaning
 * and these tests pin it down. */

#include "fs_fixture.h" /* first: it sets _XOPEN_SOURCE on POSIX */

#include "ctt.h"
#include "filec/fs.h"

#include <time.h>

void ctt_before_each(void)
{
    fx_setup();
}

void ctt_after_each(void)
{
    fx_teardown();
}

#define P(lit) STRING_LIT(lit)

/* string_t -> C string for ASSERT_STR_EQ, in rotating static buffers. */
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

/* The kind of an fs_err_t, for ASSERT_EQ against FS_OK, FS_ERR_*. */
#define KIND(call) ((call).kind)

/* fs_stat that must succeed. */
static fs_entry_t stat_ok(string_t path, uint32_t want, uint32_t flags)
{
    fs_entry_t e;
    memset(&e, 0xAB, sizeof e); /* garbage: the backend must fill it */
    fs_err_t err = fs_stat(path, want, flags, &e);
    if (!fs_ok(err))
    {
        fprintf(
            stderr,
            "fs_stat '%.*s' failed: %d (os %d)\n",
            (int)path.len,
            path.ptr,
            (int)err.kind,
            (int)err.os_code);
    }
    CTT_ASSERT_EQ(FS_OK, err.kind);
    return e;
}

static fs_kind_t kind_of(string_t path)
{
    return stat_ok(path, FS_F_KIND, 0).kind;
}

/* --- errors -------------------------------------------------------------- */

TEST(err_str_describes_every_kind)
{
    for (int k = FS_OK; k <= FS_ERR_IO; ++k)
    {
        string_t s = fs_err_str((fs_err_t){(fs_err_kind_t)k, 0});
        ASSERT_NOT_NULL(s.ptr);
        ASSERT_GT(s.len, 0u);
    }
    /* out of range: still something to print */
    ASSERT_GT(fs_err_str((fs_err_t){(fs_err_kind_t)999, 0}).len, 0u);
    ASSERT_STR_EQ("not found", c(fs_err_str((fs_err_t){FS_ERR_NOT_FOUND, 2})));
}

TEST(fs_ok_tests_the_kind)
{
    ASSERT_TRUE(fs_ok((fs_err_t){FS_OK, 0}));
    ASSERT_FALSE(fs_ok((fs_err_t){FS_ERR_NOT_FOUND, 2}));
}

/* An error from the OS carries the OS's own code; success carries 0. */
TEST(os_errors_carry_the_os_code)
{
    ASSERT_TRUE(fx_mkdir("d"));
    fs_err_t err = fs_mkdir(FX("d"));
    ASSERT_EQ(FS_ERR_EXISTS, err.kind);
    ASSERT_NE(0, err.os_code);

    err = fs_mkdir(FX("e"));
    ASSERT_EQ(FS_OK, err.kind);
    ASSERT_EQ(0, err.os_code);
}

/* --- fs_mkdir ------------------------------------------------------------ */

TEST(mkdir_creates_a_directory)
{
    ASSERT_EQ(FS_OK, KIND(fs_mkdir(FX("new"))));
    ASSERT_EQ(FS_KIND_DIR, kind_of(FX("new")));
    ASSERT_EQ(FS_OK, KIND(fs_mkdir(FX("new/inner"))));
    ASSERT_EQ(FS_KIND_DIR, kind_of(FX("new/inner")));
}

/* Whatever is there already — a directory or a file. */
TEST(mkdir_reports_exists)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_EQ(FS_ERR_EXISTS, KIND(fs_mkdir(FX("d"))));
    ASSERT_EQ(FS_ERR_EXISTS, KIND(fs_mkdir(FX("f"))));
    ASSERT_EQ(FS_KIND_FILE, kind_of(FX("f"))); /* untouched */
}

TEST(mkdir_needs_the_parent)
{
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_mkdir(FX("missing/new"))));
    ASSERT_FALSE(fs_exists(FX("missing")));
}

/* POSIX says ENOTDIR; Windows says ERROR_PATH_NOT_FOUND, and the backend
 * has to look closer to give the same answer. */
TEST(mkdir_under_a_file_is_not_dir)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_EQ(FS_ERR_NOT_DIR, KIND(fs_mkdir(FX("f/new"))));
}

/* --- fs_remove ----------------------------------------------------------- */

TEST(remove_a_file)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_EQ(FS_OK, KIND(fs_remove(FX("f"))));
    ASSERT_FALSE(fs_exists(FX("f")));
}

TEST(remove_an_empty_directory)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_EQ(FS_OK, KIND(fs_remove(FX("d"))));
    ASSERT_FALSE(fs_exists(FX("d")));
}

/* Never recursive: a directory with anything in it stays. */
TEST(remove_refuses_a_non_empty_directory)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_write_file("d/f", "x"));
    ASSERT_EQ(FS_ERR_NOT_EMPTY, KIND(fs_remove(FX("d"))));
    ASSERT_TRUE(fs_exists(FX("d/f")));
}

TEST(remove_missing_is_not_found)
{
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_remove(FX("missing"))));
}

/* Removing a link removes the link, never what it points to. */
TEST(remove_a_link_keeps_its_target)
{
    ASSERT_TRUE(fx_write_file("target", "x"));
    if (!fx_symlink("target", "link"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_EQ(FS_OK, KIND(fs_remove(FX("link"))));
    ASSERT_TRUE(fs_exists(FX("target")));
    fs_entry_t e;
    ASSERT_EQ(
        FS_ERR_NOT_FOUND,
        KIND(fs_stat(FX("link"), FS_F_KIND, FS_NOFOLLOW, &e)));
}

/* --- fs_stat ------------------------------------------------------------- */

TEST(stat_a_file)
{
    ASSERT_TRUE(fx_write_file("a.txt", "hello"));
    time_t now = time(NULL);
    fs_entry_t e = stat_ok(FX("a.txt"), FS_F_ALL, 0);

    ASSERT_EQ(FS_KIND_FILE, e.kind);
    ASSERT_EQ(5u, e.size);
    ASSERT_STR_EQ("a.txt", c(e.name));
    /* written a moment ago, by this machine's clock */
    ASSERT_LE(e.mtime_ns / 1000000000 - (int64_t)now, 60);
    ASSERT_LE((int64_t)now - e.mtime_ns / 1000000000, 60);
    ASSERT_TRUE(e.mode & 0400); /* the owner can read it */
    ASSERT_EQ(0u, e.flags & FS_FLAG_HIDDEN);
}

TEST(stat_a_directory)
{
    ASSERT_TRUE(fx_mkdir("d"));
    fs_entry_t e = stat_ok(FX("d"), FS_F_ALL, 0);
    ASSERT_EQ(FS_KIND_DIR, e.kind);
    ASSERT_TRUE(e.mode & 0400);
}

/* Every field asked for is delivered — on its own and all together.  Only
 * the birth time may be missing (not every filesystem records it). */
TEST(stat_delivers_what_is_wanted)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    const uint32_t fields[] = {
        FS_F_KIND, FS_F_SIZE, FS_F_MTIME, FS_F_MODE, FS_F_FLAGS, FS_F_ID};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i)
    {
        fs_entry_t e = stat_ok(FX("f"), fields[i], 0);
        ASSERT_EQ(fields[i], e.valid & fields[i]);
    }
    fs_entry_t all = stat_ok(FX("f"), FS_F_ALL, 0);
    ASSERT_EQ(
        FS_F_ALL & ~(uint32_t)FS_F_BTIME, all.valid & ~(uint32_t)FS_F_BTIME);
    if (all.valid & FS_F_BTIME)
    {
        ASSERT_LE(all.btime_ns, all.mtime_ns + 1000000000); /* born first */
    }
}

/* name is a view of the path's file name — no allocation. */
TEST(stat_name_is_a_view_of_the_path)
{
    ASSERT_TRUE(fx_write_file("file.txt", "x"));
    string_t p = FX("file.txt");
    fs_entry_t e = stat_ok(p, FS_F_KIND, 0);
    ASSERT_EQ(8u, e.name.len);
    ASSERT_TRUE(e.name.ptr == p.ptr + p.len - 8);
}

/* (dev, ino) identify a file: the same file twice is equal, two files
 * differ. */
TEST(stat_id_identifies_the_file)
{
    ASSERT_TRUE(fx_write_file("a", "x"));
    ASSERT_TRUE(fx_write_file("b", "x"));
    fs_entry_t a1 = stat_ok(FX("a"), FS_F_ID, 0);
    fs_entry_t a2 = stat_ok(FX("a"), FS_F_ID, 0);
    fs_entry_t b = stat_ok(FX("b"), FS_F_ID, 0);
    ASSERT_TRUE(a1.dev == a2.dev && a1.ino == a2.ino);
    ASSERT_FALSE(a1.dev == b.dev && a1.ino == b.ino);
}

TEST(stat_missing_is_not_found)
{
    fs_entry_t e;
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_stat(FX("missing"), FS_F_KIND, 0, &e)));
    ASSERT_EQ(
        FS_ERR_NOT_FOUND, KIND(fs_stat(FX("missing/x"), FS_F_KIND, 0, &e)));
}

TEST(stat_follows_links_unless_nofollow)
{
    ASSERT_TRUE(fx_write_file("target", "hello"));
    if (!fx_symlink("target", "link"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    fs_entry_t followed = stat_ok(FX("link"), FS_F_KIND | FS_F_SIZE, 0);
    ASSERT_EQ(FS_KIND_FILE, followed.kind);
    ASSERT_EQ(5u, followed.size);
    ASSERT_STR_EQ("link", c(followed.name));

    fs_entry_t link = stat_ok(FX("link"), FS_F_KIND, FS_NOFOLLOW);
    ASSERT_EQ(FS_KIND_LINK, link.kind);

    /* a dangling link: following fails, the link itself is there */
    ASSERT_TRUE(fx_symlink("nowhere", "dangling"));
    fs_entry_t e;
    ASSERT_EQ(
        FS_ERR_NOT_FOUND, KIND(fs_stat(FX("dangling"), FS_F_KIND, 0, &e)));
    ASSERT_EQ(
        FS_KIND_LINK, stat_ok(FX("dangling"), FS_F_KIND, FS_NOFOLLOW).kind);
}

/* Hidden means a dotfile on POSIX and the hidden attribute on Windows —
 * whatever the platform's own file browser hides. */
TEST(stat_reports_hidden)
{
#ifdef _WIN32
    ASSERT_TRUE(fx_write_file("secret", "x"));
    ASSERT_TRUE(fx_set_hidden("secret"));
    ASSERT_EQ(
        (uint32_t)FS_FLAG_HIDDEN,
        stat_ok(FX("secret"), FS_F_FLAGS, 0).flags & FS_FLAG_HIDDEN);
#else
    ASSERT_FALSE(fx_set_hidden("unused")); /* no attribute on POSIX */
    ASSERT_TRUE(fx_write_file(".secret", "x"));
    ASSERT_EQ(
        (uint32_t)FS_FLAG_HIDDEN,
        stat_ok(FX(".secret"), FS_F_FLAGS, 0).flags & FS_FLAG_HIDDEN);
#endif
    ASSERT_TRUE(fx_write_file("plain", "x"));
    ASSERT_EQ(0u, stat_ok(FX("plain"), FS_F_FLAGS, 0).flags & FS_FLAG_HIDDEN);
}

/* --- fs_exists ----------------------------------------------------------- */

TEST(exists)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fs_exists(FX("f")));
    ASSERT_TRUE(fs_exists(FX("d")));
    ASSERT_TRUE(fs_exists(FX("")));
    ASSERT_FALSE(fs_exists(FX("missing")));
    ASSERT_FALSE(fs_exists(FX("f/below_a_file")));
}

/* Follows links: a link to nowhere does not exist. */
TEST(exists_follows_links)
{
    if (!fx_symlink("nowhere", "dangling"))
    {
        return;
    }
    ASSERT_FALSE(fs_exists(FX("dangling")));
    ASSERT_TRUE(fx_write_file("nowhere", "x"));
    ASSERT_TRUE(fs_exists(FX("dangling")));
}

/* --- paths in ------------------------------------------------------------ */

/* Only absolute canonical paths: the current directory is process-wide
 * state, and nothing in fs depends on it. */
TEST(relative_paths_are_invalid)
{
    fs_entry_t e;
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_mkdir(P("rel"))));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_remove(P("rel"))));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_stat(P("a/b"), FS_F_KIND, 0, &e)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_stat(P("."), FS_F_KIND, 0, &e)));
    ASSERT_FALSE(fs_exists(P(".")));
}

TEST(empty_path_is_invalid)
{
    fs_entry_t e;
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_mkdir(P(""))));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_stat(P(""), FS_F_KIND, 0, &e)));
    ASSERT_FALSE(fs_exists(P("")));
}

/* A NUL inside the path would silently cut it short in the OS call. */
TEST(nul_byte_is_invalid)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    string_t f = FX("f");
    char buf[8192];
    memcpy(buf, f.ptr, f.len);
    memcpy(buf + f.len, "\0tail", 5);
    string_t with_nul = {buf, f.len + 5};
    fs_entry_t e;
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_stat(with_nul, FS_F_KIND, 0, &e)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_remove(with_nul)));
    ASSERT_TRUE(fs_exists(f)); /* not removed through the cut-short path */
}

TEST(null_out_is_invalid)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_stat(FX("f"), FS_F_KIND, 0, NULL)));
}

TEST(too_long_is_name_too_long)
{
    static char buf[FS_PATH_MAX + 64];
    string_t root = FX("");
    memcpy(buf, root.ptr, root.len);
    size_t n = root.len;
    buf[n++] = '/';
    while (n < FS_PATH_MAX + 10)
    {
        buf[n++] = 'a';
    }
    string_t lng = {buf, n};
    fs_entry_t e;
    ASSERT_EQ(FS_ERR_NAME_TOO_LONG, KIND(fs_mkdir(lng)));
    ASSERT_EQ(FS_ERR_NAME_TOO_LONG, KIND(fs_stat(lng, FS_F_KIND, 0, &e)));
    ASSERT_FALSE(fs_exists(lng));
}

/* Paths are string_t: nothing past len is read. */
TEST(input_need_not_be_nul_terminated)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    string_t f = FX("f");
    char buf[8192];
    memcpy(buf, f.ptr, f.len);
    memcpy(buf + f.len, "GARBAGE", 8);
    string_t view = {buf, f.len};
    ASSERT_EQ(FS_KIND_FILE, kind_of(view));
    ASSERT_EQ(FS_OK, KIND(fs_remove(view)));
    ASSERT_FALSE(fs_exists(f));
}

/* UTF-8 in, UTF-8 out — on Windows through UTF-16. */
TEST(utf8_names)
{
    const char *name = "bl\xc3\xa5"
                       "b\xc3\xa6r \xc3\xb8 \xe6\x97\xa5\xe6\x9c\xac";
    char rel[256];
    snprintf(rel, sizeof rel, "%s", name);
    ASSERT_EQ(FS_OK, KIND(fs_mkdir(FX(rel))));
    fs_entry_t e = stat_ok(FX(rel), FS_F_KIND, 0);
    ASSERT_EQ(FS_KIND_DIR, e.kind);
    ASSERT_STR_EQ(name, c(e.name));
    ASSERT_TRUE(fs_exists(FX(rel)));
    ASSERT_EQ(FS_OK, KIND(fs_remove(FX(rel))));
    ASSERT_FALSE(fs_exists(FX(rel)));
}

TEST(names_with_spaces_and_dots)
{
    ASSERT_EQ(FS_OK, KIND(fs_mkdir(FX("a b"))));
    ASSERT_TRUE(fx_write_file("a b/x.y.z", "x"));
    ASSERT_EQ(FS_KIND_FILE, kind_of(FX("a b/x.y.z")));
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "filec fs basics");
}
