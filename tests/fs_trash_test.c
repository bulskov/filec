/* filec/fs step 8: fs_trash.
 *
 * On POSIX the trash is pointed into the test directory — $XDG_DATA_HOME
 * on Linux, $HOME on macOS — so the tests can look inside it; the test
 * directory is on one volume, so that trash is the one used.  On Windows
 * the Recycle Bin cannot be looked into from here: the tests check that
 * the file is gone and nothing else broke. */

#include "fs_fixture.h" /* first: it sets _XOPEN_SOURCE on POSIX */

#include "ctt.h"
#include "filec/fs.h"

#if defined(__APPLE__)
#define TRASH_ENV "HOME"
#define TRASH_FILES ".Trash"
#elif !defined(_WIN32)
#define TRASH_ENV "XDG_DATA_HOME"
#define TRASH_FILES "data/Trash/files"
#define TRASH_INFO "data/Trash/info"
#endif

#ifdef TRASH_ENV
static char old_env[8192];
static bool had_env;
#endif

/* Wine's Recycle Bin is the real trash of the Linux user running it:
 * tests that trash something do not run there (real Windows CI does). */
#define SKIP_UNDER_WINE()                                                      \
    do                                                                         \
    {                                                                          \
        if (fx_is_wine())                                                      \
        {                                                                      \
            return;                                                            \
        }                                                                      \
    } while (0)

void ctt_before_each(void)
{
    fx_setup();
#ifdef TRASH_ENV
    const char *old = getenv(TRASH_ENV);
    had_env = old != NULL;
    snprintf(old_env, sizeof old_env, "%s", old ? old : "");
#ifdef __APPLE__
    setenv(TRASH_ENV, fx_native(FX("")), 1);
#else
    setenv(TRASH_ENV, fx_native(FX("data")), 1);
#endif
#endif
}

void ctt_after_each(void)
{
#ifdef TRASH_ENV
    if (had_env)
    {
        setenv(TRASH_ENV, old_env, 1);
    }
    else
    {
        unsetenv(TRASH_ENV);
    }
#endif
    fx_teardown();
}

#define P(lit) STRING_LIT(lit)
#define KIND(call) ((call).kind)

#ifdef TRASH_FILES
static char *in_trash(const char *name)
{
    static char buf[512];
    snprintf(buf, sizeof buf, "%s/%s", TRASH_FILES, name);
    return buf;
}
#endif

TEST(trash_a_file)
{
    SKIP_UNDER_WINE();
    ASSERT_TRUE(fx_write_file("f", "hello"));
    ASSERT_EQ(FS_OK, KIND(fs_trash(FX("f"))));
    ASSERT_FALSE(fx_exists("f"));
#ifdef TRASH_FILES
    const char *s = fx_read_file(in_trash("f"), NULL);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ("hello", s);
#endif
}

#ifdef TRASH_INFO
/* The .trashinfo that lets a file manager put it back: where it was
 * (percent-encoded) and when it was trashed. */
TEST(trash_writes_trashinfo)
{
    ASSERT_TRUE(fx_write_file("a b%.txt", "x"));
    ASSERT_EQ(FS_OK, KIND(fs_trash(FX("a b%.txt"))));
    const char *info = fx_read_file(TRASH_INFO "/a b%.txt.trashinfo", NULL);
    ASSERT_NOT_NULL(info);
    char want[8192];
    snprintf(want, sizeof want, "Path=%s/a%%20b%%25.txt\n", fx_native(FX("")));
    ASSERT_STR_CONTAINS(info, "[Trash Info]\n");
    ASSERT_STR_CONTAINS(info, want);
    ASSERT_STR_CONTAINS(info, "DeletionDate=20");
}
#endif

/* The same name twice: both are kept. */
TEST(trash_keeps_both_of_one_name)
{
    SKIP_UNDER_WINE();
    ASSERT_TRUE(fx_write_file("f", "first"));
    ASSERT_EQ(FS_OK, KIND(fs_trash(FX("f"))));
    ASSERT_TRUE(fx_write_file("f", "second"));
    ASSERT_EQ(FS_OK, KIND(fs_trash(FX("f"))));
    ASSERT_FALSE(fx_exists("f"));
#ifdef TRASH_FILES
    ASSERT_EQ(2, fx_count_entries(TRASH_FILES));
    ASSERT_STR_EQ("first", fx_read_file(in_trash("f"), NULL));
#endif
}

TEST(trash_a_directory_with_its_contents)
{
    SKIP_UNDER_WINE();
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_mkdir("d/sub"));
    ASSERT_TRUE(fx_write_file("d/sub/x", "1"));
    ASSERT_EQ(FS_OK, KIND(fs_trash(FX("d"))));
    ASSERT_FALSE(fx_exists("d"));
#ifdef TRASH_FILES
    ASSERT_STR_EQ("1", fx_read_file(in_trash("d/sub/x"), NULL));
#endif
}

/* A link goes to the trash itself; what it points to stays. */
TEST(trash_a_link)
{
    SKIP_UNDER_WINE();
    ASSERT_TRUE(fx_write_file("target", "keep me"));
    if (!fx_symlink("target", "l"))
    {
        return; /* no symlinks here (Windows without developer mode) */
    }
    ASSERT_EQ(FS_OK, KIND(fs_trash(FX("l"))));
    ASSERT_FALSE(fx_exists("l"));
    ASSERT_STR_EQ("keep me", fx_read_file("target", NULL));
}

TEST(trash_errors)
{
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_trash(FX("missing"))));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_trash(P("relative"))));
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "filec fs trash");
}
