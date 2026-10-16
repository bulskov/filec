/* filec/fs step 5: fs_cwd, fs_location, fs_from_native.
 *
 * These read process-wide state — the current directory and the
 * environment — so every test that changes either puts it back after
 * itself (ctt_after_each).  The test directory may be reached through a
 * link (/tmp on macOS) or an 8.3 name (Windows CI): where a path is
 * compared with it, the comparison is by identity (fs_stat's dev, ino),
 * not by string. */

#include "fs_fixture.h" /* first: it sets _XOPEN_SOURCE on POSIX */

#include "ctt.h"
#include "filec/fs.h"
#include "filec/path.h"

#include "arena/growing_arena.h"

static growing_arena_t arena;
static strbuf_t *sb;

/* --- The current directory and the environment, saved and restored ----- */

#ifdef _WIN32
static wchar_t saved_cwd[8192];

static void save_cwd(void)
{
    GetCurrentDirectoryW(8192, saved_cwd);
}

static void restore_cwd(void)
{
    SetCurrentDirectoryW(saved_cwd);
}

static bool chdir_to(const char *rel)
{
    return SetCurrentDirectoryW(fx_wide(fx_native(FX(rel))));
}
#else
static char saved_cwd[8192];

static void save_cwd(void)
{
    if (!getcwd(saved_cwd, sizeof saved_cwd))
    {
        abort();
    }
}

static void restore_cwd(void)
{
    if (chdir(saved_cwd) != 0)
    {
        abort();
    }
}

static bool chdir_to(const char *rel)
{
    return chdir(fx_native(FX(rel))) == 0;
}
#endif

/* Environment variables a test changes: name and the old value. */
typedef struct
{
    const char *name;
    char old[8192];
    bool had;
} env_save_t;

static env_save_t envs[8];
static int n_envs;

static void env_set(const char *name, const char *value)
{
    env_save_t *e = &envs[n_envs++];
    e->name = name;
    const char *old = getenv(name);
    e->had = old != NULL;
    snprintf(e->old, sizeof e->old, "%s", old ? old : "");
#ifdef _WIN32
    /* GetTempPathW and friends read the process environment block */
    SetEnvironmentVariableW(fx_wide(name), value ? fx_wide(value) : NULL);
    char buf[8300];
    snprintf(buf, sizeof buf, "%s=%s", name, value ? value : "");
    _putenv(buf);
#else
    if (value)
    {
        setenv(name, value, 1);
    }
    else
    {
        unsetenv(name);
    }
#endif
}

static void env_restore(void)
{
    while (n_envs > 0)
    {
        env_save_t *e = &envs[--n_envs];
#ifdef _WIN32
        SetEnvironmentVariableW(
            fx_wide(e->name), e->had ? fx_wide(e->old) : NULL);
        char buf[8300];
        snprintf(buf, sizeof buf, "%s=%s", e->name, e->had ? e->old : "");
        _putenv(buf);
#else
        if (e->had)
        {
            setenv(e->name, e->old, 1);
        }
        else
        {
            unsetenv(e->name);
        }
#endif
    }
}

void ctt_before_each(void)
{
    fx_setup();
    save_cwd();
    growing_arena_init(&arena, 16 * 1024);
    sb = strbuf_create(growing_arena_allocator(&arena));
}

void ctt_after_each(void)
{
    env_restore();
    restore_cwd(); /* before teardown: Windows cannot remove a cwd */
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

/* A canonical, absolute, normalised path: what path_normalize leaves as
 * it is, starting with '/'. */
static bool is_canonical(string_t p)
{
    if (p.len == 0 || p.ptr[0] != '/')
    {
        return false;
    }
    growing_arena_t a;
    growing_arena_init(&a, 4096);
    strbuf_t *n = strbuf_create(growing_arena_allocator(&a));
    bool same =
        path_normalize(p, n) == PATH_OK && string_equals(strbuf_view(n), p);
    growing_arena_destroy(&a);
    return same;
}

/* a and b name the same directory or file (dev, ino). */
static bool same_place(string_t a, string_t b)
{
    fs_entry_t ea, eb;
    return fs_ok(fs_stat(a, FS_F_ID, 0, &ea))
           && fs_ok(fs_stat(b, FS_F_ID, 0, &eb)) && ea.dev == eb.dev
           && ea.ino == eb.ino;
}

/* The result of fs_cwd, in a buffer of its own. */
static const char *cwd(void)
{
    static char buf[8192];
    strbuf_t *s = strbuf_create(growing_arena_allocator(&arena));
    CTT_ASSERT_EQ(FS_OK, KIND(fs_cwd(s)));
    snprintf(buf, sizeof buf, "%s", c(strbuf_view(s)));
    return buf;
}

static const char *joined(const char *a, const char *b)
{
    static char buf[8192];
    snprintf(buf, sizeof buf, "%s%s%s", a, strcmp(a, "/") ? "/" : "", b);
    return buf;
}

/* fs_from_native into a fresh sb; "(error)" on failure. */
static const char *from_native(const char *native)
{
    strbuf_clear(sb);
    fs_err_t err = fs_from_native(string_view_cstr(native), sb);
    return fs_ok(err) ? c(strbuf_view(sb)) : "(error)";
}

/* =========================================================================
 * fs_cwd
 * ========================================================================= */

TEST(cwd_is_the_current_directory)
{
    ASSERT_TRUE(fx_mkdir("a"));
    ASSERT_TRUE(fx_mkdir("a/b"));
    ASSERT_TRUE(chdir_to(""));
    ASSERT_TRUE(is_canonical(string_view_cstr(cwd())));
    ASSERT_TRUE(same_place(string_view_cstr(cwd()), FX("")));

    ASSERT_TRUE(chdir_to("a/b"));
    ASSERT_TRUE(same_place(string_view_cstr(cwd()), FX("a/b")));
    string_t name = path_file_name(string_view_cstr(cwd()));
    ASSERT_STR_EQ("b", c(name));
}

TEST(cwd_appends)
{
    ASSERT_TRUE(chdir_to(""));
    ASSERT_EQ(SEQC_OK, strbuf_append(sb, P("keep:")));
    ASSERT_EQ(FS_OK, KIND(fs_cwd(sb)));
    char want[8192];
    snprintf(want, sizeof want, "keep:%s", cwd());
    ASSERT_STR_EQ(want, c(strbuf_view(sb)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_cwd(NULL)));
}

/* =========================================================================
 * fs_location
 * ========================================================================= */

static const fs_loc_t all_locs[] = {
    FS_LOC_HOME, FS_LOC_TEMP, FS_LOC_CONFIG, FS_LOC_CACHE, FS_LOC_DATA};

/* Whatever the platform: an answer, canonical and absolute. */
TEST(location_gives_canonical_paths)
{
    for (size_t i = 0; i < sizeof all_locs / sizeof all_locs[0]; i++)
    {
        strbuf_clear(sb);
        ASSERT_EQ(FS_OK, KIND(fs_location(all_locs[i], sb)));
        ASSERT_TRUE(is_canonical(strbuf_view(sb)));
    }
}

/* Home and temp exist on every system that runs tests. */
TEST(location_home_and_temp_exist)
{
    fs_entry_t e;
    ASSERT_EQ(FS_OK, KIND(fs_location(FS_LOC_HOME, sb)));
    ASSERT_EQ(FS_OK, KIND(fs_stat(strbuf_view(sb), FS_F_KIND, 0, &e)));
    ASSERT_EQ(FS_KIND_DIR, e.kind);
    strbuf_clear(sb);
    ASSERT_EQ(FS_OK, KIND(fs_location(FS_LOC_TEMP, sb)));
    ASSERT_EQ(FS_OK, KIND(fs_stat(strbuf_view(sb), FS_F_KIND, 0, &e)));
    ASSERT_EQ(FS_KIND_DIR, e.kind);
}

/* The temporary directory follows the environment ($TMPDIR; Windows:
 * TMP, which GetTempPathW reads). */
TEST(location_temp_follows_the_environment)
{
    ASSERT_TRUE(fx_mkdir("t"));
#ifdef _WIN32
    env_set("TMP", fx_native(FX("t")));
    env_set("TEMP", fx_native(FX("t")));
#else
    env_set("TMPDIR", fx_native(FX("t")));
#endif
    ASSERT_EQ(FS_OK, KIND(fs_location(FS_LOC_TEMP, sb)));
    ASSERT_TRUE(is_canonical(strbuf_view(sb)));
    ASSERT_TRUE(same_place(strbuf_view(sb), FX("t")));
}

#ifndef _WIN32
/* Everything under home follows $HOME. */
TEST(location_follows_home)
{
    env_set("HOME", fx_native(FX("")));
    env_set("XDG_CONFIG_HOME", NULL);
    env_set("XDG_CACHE_HOME", NULL);
    env_set("XDG_DATA_HOME", NULL);
    char home[8192];
    snprintf(home, sizeof home, "%s", c(FX("")));
    ASSERT_EQ(FS_OK, KIND(fs_location(FS_LOC_HOME, sb)));
    ASSERT_STR_EQ(home, c(strbuf_view(sb)));
    struct
    {
        fs_loc_t loc;
        const char *rel;
    } under[] = {
#ifdef __APPLE__
        {FS_LOC_CONFIG, "Library/Application Support"},
        {FS_LOC_CACHE, "Library/Caches"},
        {FS_LOC_DATA, "Library/Application Support"},
#else
        {FS_LOC_CONFIG, ".config"},
        {FS_LOC_CACHE, ".cache"},
        {FS_LOC_DATA, ".local/share"},
#endif
    };
    for (size_t i = 0; i < sizeof under / sizeof under[0]; i++)
    {
        strbuf_clear(sb);
        ASSERT_EQ(FS_OK, KIND(fs_location(under[i].loc, sb)));
        ASSERT_STR_EQ(joined(home, under[i].rel), c(strbuf_view(sb)));
    }
}

/* A relative $HOME does not count: the password database is asked. */
TEST(location_ignores_a_relative_home)
{
    env_set("HOME", "relative/home");
    ASSERT_EQ(FS_OK, KIND(fs_location(FS_LOC_HOME, sb)));
    ASSERT_TRUE(is_canonical(strbuf_view(sb)));
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
/* XDG: an absolute $XDG_*_HOME wins, a relative one is ignored. */
TEST(location_follows_xdg)
{
    env_set("HOME", fx_native(FX("")));
    env_set("XDG_CONFIG_HOME", fx_native(FX("cfg")));
    env_set("XDG_CACHE_HOME", "relative/cache");
    env_set("XDG_DATA_HOME", fx_native(FX("data/dir/")));
    ASSERT_EQ(FS_OK, KIND(fs_location(FS_LOC_CONFIG, sb)));
    ASSERT_STR_EQ(c(FX("cfg")), c(strbuf_view(sb)));
    strbuf_clear(sb);
    ASSERT_EQ(FS_OK, KIND(fs_location(FS_LOC_CACHE, sb)));
    ASSERT_STR_EQ(joined(c(FX("")), ".cache"), c(strbuf_view(sb)));
    strbuf_clear(sb);
    ASSERT_EQ(FS_OK, KIND(fs_location(FS_LOC_DATA, sb)));
    ASSERT_STR_EQ(c(FX("data/dir")), c(strbuf_view(sb))); /* normalised */
}
#endif

TEST(location_errors)
{
    ASSERT_EQ(SEQC_OK, strbuf_append(sb, P("keep")));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_location((fs_loc_t)99, sb)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_location((fs_loc_t)-1, sb)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_location(FS_LOC_HOME, NULL)));
    ASSERT_STR_EQ("keep", c(strbuf_view(sb)));
}

/* =========================================================================
 * fs_from_native
 * ========================================================================= */

/* Absolute forms: normalised, nothing looked up. */
TEST(from_native_absolute)
{
#ifdef _WIN32
    ASSERT_STR_EQ("/C:/a/c", from_native("C:\\a\\.\\b\\..\\c"));
    ASSERT_STR_EQ("/C:/x/y", from_native("c:/x/y"));
    ASSERT_STR_EQ("/C:", from_native("C:\\"));
    ASSERT_STR_EQ("/UNC/srv/share/x", from_native("\\\\srv\\share\\x"));
    ASSERT_STR_EQ("/C:/x", from_native("\\\\?\\C:\\x"));
#else
    ASSERT_STR_EQ("/a/c", from_native("/a/./b/../c"));
    ASSERT_STR_EQ("/a/b", from_native("//a///b/"));
    ASSERT_STR_EQ("/", from_native("/"));
#endif
}

/* Relative forms: against the current directory. */
TEST(from_native_relative)
{
    ASSERT_TRUE(fx_mkdir("a"));
    ASSERT_TRUE(chdir_to("a"));
    const char *here = cwd();
    ASSERT_STR_EQ(joined(here, "x/y"), from_native("x/y"));
    ASSERT_STR_EQ(here, from_native("."));
    ASSERT_STR_EQ(c(path_parent(string_view_cstr(here))), from_native(".."));
    ASSERT_STR_EQ(joined(here, "z"), from_native("./x/../z"));
    ASSERT_STR_EQ(joined(here, "~/x"), from_native("~/x")); /* no ~ */
#ifdef _WIN32
    ASSERT_STR_EQ(joined(here, "x/y"), from_native("x\\y"));
#endif
}

#ifdef _WIN32
/* "\x" is the root of the current drive; "C:x" is relative to drive C's
 * current directory — for the current drive, the current directory. */
TEST(from_native_windows_drive_forms)
{
    ASSERT_TRUE(chdir_to(""));
    const char *here = cwd(); /* "/C:/..." */
    char drive[8];
    snprintf(drive, sizeof drive, "%.3s", here); /* "/C:" */
    ASSERT_STR_EQ(joined(drive, "x"), from_native("\\x"));
    char rel[8];
    snprintf(rel, sizeof rel, "%c:x", here[1]); /* "C:x" */
    ASSERT_STR_EQ(joined(here, "x"), from_native(rel));
}

/* Windows' own rules: trailing dots and spaces of a name go. */
TEST(from_native_windows_trailing_dots)
{
    ASSERT_STR_EQ("/C:/a/b", from_native("C:\\a.\\b. ."));
}
#endif

TEST(from_native_appends)
{
    ASSERT_EQ(SEQC_OK, strbuf_append(sb, P("keep:")));
#ifdef _WIN32
    ASSERT_EQ(FS_OK, KIND(fs_from_native(P("C:\\x"), sb)));
    ASSERT_STR_EQ("keep:/C:/x", c(strbuf_view(sb)));
#else
    ASSERT_EQ(FS_OK, KIND(fs_from_native(P("/x"), sb)));
    ASSERT_STR_EQ("keep:/x", c(strbuf_view(sb)));
#endif
}

TEST(from_native_errors)
{
    ASSERT_EQ(SEQC_OK, strbuf_append(sb, P("keep")));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_from_native(P(""), sb)));
    ASSERT_EQ(
        FS_ERR_INVALID,
        KIND(fs_from_native((string_t){"/a\0b", 4}, sb))); /* a NUL */
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_from_native(P("/x"), NULL)));
    ASSERT_STR_EQ("keep", c(strbuf_view(sb)));
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "filec fs places");
}
