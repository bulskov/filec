/* filec/fs step 7: watching directories — fs_watch_create, _add, _remove,
 * _wait, _wake, _destroy.
 *
 * Events arrive asynchronously, so the tests make a change and then
 * collect events for a while (collect).  On macOS (kqueue) an event says
 * only that a directory changed, with the name "": expect() accepts that
 * there in place of the exact kind and name. */

#include "fs_fixture.h" /* first: it sets _XOPEN_SOURCE on POSIX */

#include "ctt.h"
#include "filec/fs.h"

#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#endif

static fs_watch_t *w;

/* The events collected so far, names copied. */
typedef struct
{
    fs_watch_kind_t kind;
    uint32_t id;
    char name[256];
} seen_t;

static seen_t seen[512];
static int n_seen;

void ctt_before_each(void)
{
    fx_setup();
    w = NULL;
    n_seen = 0;
}

void ctt_after_each(void)
{
    fs_watch_destroy(w);
    fx_teardown();
}

#define P(lit) STRING_LIT(lit)
#define KIND(call) ((call).kind)

/* --- Time and threads ---------------------------------------------------- */

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
#endif
}

static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
#endif
}

/* --- Helpers ------------------------------------------------------------- */

static void create_watcher(void)
{
    CTT_ASSERT_EQ(FS_OK, KIND(fs_watch_create(heap(), &w)));
    CTT_ASSERT_NOT_NULL(w);
}

static uint32_t watch(const char *rel)
{
    uint32_t id = 0;
    CTT_ASSERT_EQ(FS_OK, KIND(fs_watch_add(w, FX(rel), &id)));
    CTT_ASSERT_TRUE(id != 0);
    return id;
}

/* Waits for events for up to ms, copying them into seen; stops early once
 * events have come and then 200 ms pass without more. */
static void collect(unsigned ms)
{
    uint64_t end = now_ms() + ms;
    bool got_some = false;
    while (now_ms() < end)
    {
        fs_watch_event_t ev[16];
        size_t count = 0;
        CTT_ASSERT_EQ(FS_OK, KIND(fs_watch_wait(w, ev, 16, 200, &count)));
        for (size_t i = 0; i < count && n_seen < 512; i++)
        {
            seen_t *s = &seen[n_seen++];
            s->kind = ev[i].kind;
            s->id = ev[i].watch_id;
            size_t n = ev[i].name.len < 255 ? ev[i].name.len : 255;
            if (n)
            {
                memcpy(s->name, ev[i].name.ptr, n);
            }
            s->name[n] = '\0';
        }
        if (count > 0)
        {
            got_some = true;
        }
        else if (got_some)
        {
            break;
        }
    }
}

/* An event (id, kind, name) was seen — or, on macOS, any event for id
 * with the name "" (kqueue does not say what changed). */
static bool expect(uint32_t id, fs_watch_kind_t kind, const char *name)
{
    for (int i = 0; i < n_seen; i++)
    {
        if (seen[i].id != id)
        {
            continue;
        }
#ifdef __APPLE__
        if (seen[i].name[0] == '\0')
        {
            return true;
        }
#endif
        if (seen[i].kind == kind && strcmp(seen[i].name, name) == 0)
        {
            return true;
        }
    }
    return false;
}

static int events_for(uint32_t id)
{
    int n = 0;
    for (int i = 0; i < n_seen; i++)
    {
        n += seen[i].id == id;
    }
    return n;
}

/* =========================================================================
 * Creating, adding, removing
 * ========================================================================= */

TEST(watch_create_and_destroy)
{
    create_watcher();
    fs_watch_destroy(w);
    w = NULL;
    fs_watch_destroy(NULL); /* no-op */
}

TEST(watch_create_errors)
{
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_watch_create(heap(), NULL)));
    size_t budget = 0;
    fs_watch_t *x = (fs_watch_t *)&budget;
    ASSERT_EQ(FS_ERR_OOM, KIND(fs_watch_create(heap_with_budget(&budget), &x)));
    ASSERT_NULL(x);
}

TEST(watch_add_gives_distinct_ids)
{
    ASSERT_TRUE(fx_mkdir("a"));
    ASSERT_TRUE(fx_mkdir("b"));
    create_watcher();
    uint32_t a = watch("a");
    uint32_t b = watch("b");
    ASSERT_TRUE(a != b);
}

TEST(watch_add_errors)
{
    ASSERT_TRUE(fx_write_file("f", "x"));
    create_watcher();
    uint32_t id = 7;
    ASSERT_EQ(FS_ERR_NOT_FOUND, KIND(fs_watch_add(w, FX("missing"), &id)));
    ASSERT_EQ(FS_ERR_NOT_DIR, KIND(fs_watch_add(w, FX("f"), &id)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_watch_add(w, P("relative"), &id)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_watch_add(w, FX(""), NULL)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_watch_add(NULL, FX(""), &id)));
}

/* =========================================================================
 * Events
 * ========================================================================= */

TEST(watch_sees_a_file_created)
{
    ASSERT_TRUE(fx_mkdir("d"));
    create_watcher();
    uint32_t id = watch("d");
    ASSERT_TRUE(fx_write_file("d/new", "x"));
    collect(3000);
    ASSERT_TRUE(expect(id, FS_W_CREATED, "new"));
}

TEST(watch_sees_a_file_deleted)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_write_file("d/old", "x"));
    create_watcher();
    uint32_t id = watch("d");
    ASSERT_EQ(0, remove(fx_native(FX("d/old"))));
    collect(3000);
    ASSERT_TRUE(expect(id, FS_W_DELETED, "old"));
}

TEST(watch_sees_a_file_modified)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_write_file("d/f", "x"));
    create_watcher();
    uint32_t id = watch("d");
    ASSERT_TRUE(fx_write_file("d/f", "a new, longer content"));
    collect(3000);
    ASSERT_TRUE(expect(id, FS_W_MODIFIED, "f"));
}

/* Both names, where the system tells them. */
TEST(watch_sees_a_rename)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_write_file("d/a", "x"));
    create_watcher();
    uint32_t id = watch("d");
    ASSERT_EQ(FS_OK, KIND(fs_rename(FX("d/a"), FX("d/b"), 0)));
    collect(3000);
    ASSERT_TRUE(expect(id, FS_W_RENAMED, "a"));
    ASSERT_TRUE(expect(id, FS_W_RENAMED, "b"));
}

/* Events carry the id of the directory they happened in. */
TEST(watch_events_carry_their_id)
{
    ASSERT_TRUE(fx_mkdir("a"));
    ASSERT_TRUE(fx_mkdir("b"));
    create_watcher();
    uint32_t a = watch("a");
    uint32_t b = watch("b");
    ASSERT_TRUE(fx_write_file("b/x", "1"));
    collect(3000);
    ASSERT_TRUE(expect(b, FS_W_CREATED, "x"));
    ASSERT_EQ(0, events_for(a));
}

/* Not recursive: a change two levels down is not seen. */
TEST(watch_is_not_recursive)
{
    ASSERT_TRUE(fx_mkdir("d"));
    ASSERT_TRUE(fx_mkdir("d/sub"));
    create_watcher();
    uint32_t id = watch("d");
    ASSERT_TRUE(fx_write_file("d/sub/deep", "x"));
    collect(500);
    for (int i = 0; i < n_seen; i++)
    {
        ASSERT_TRUE(strcmp(seen[i].name, "deep") != 0);
    }
    (void)id;
}

/* More events than fit: the rest come with the next calls. */
TEST(watch_hands_out_what_does_not_fit_later)
{
    ASSERT_TRUE(fx_mkdir("d"));
    create_watcher();
    uint32_t id = watch("d");
    char name[32];
    for (int i = 0; i < 10; i++)
    {
        snprintf(name, sizeof name, "f%d", i);
        char rel[64];
        snprintf(rel, sizeof rel, "d/%s", name);
        ASSERT_TRUE(fx_write_file(rel, "x"));
    }
    uint64_t end = now_ms() + 3000;
    while (now_ms() < end)
    {
        fs_watch_event_t ev;
        size_t count = 0;
        ASSERT_EQ(FS_OK, KIND(fs_watch_wait(w, &ev, 1, 200, &count)));
        ASSERT_TRUE(count <= 1);
        if (count == 1 && n_seen < 512)
        {
            seen_t *s = &seen[n_seen++];
            s->kind = ev.kind;
            s->id = ev.watch_id;
            size_t n = ev.name.len < 255 ? ev.name.len : 255;
            if (n)
            {
                memcpy(s->name, ev.name.ptr, n);
            }
            s->name[n] = '\0';
        }
        if (count == 0 && n_seen > 0)
        {
            break;
        }
    }
    for (int i = 0; i < 10; i++)
    {
        snprintf(name, sizeof name, "f%d", i);
        ASSERT_TRUE(expect(id, FS_W_CREATED, name));
    }
}

/* After remove, the directory is not watched any more. */
TEST(watch_remove_stops_events)
{
    ASSERT_TRUE(fx_mkdir("a"));
    ASSERT_TRUE(fx_mkdir("b"));
    create_watcher();
    uint32_t a = watch("a");
    uint32_t b = watch("b");
    fs_watch_remove(w, a);
    fs_watch_remove(w, 12345); /* unknown: no-op */
    ASSERT_TRUE(fx_write_file("a/x", "1"));
    ASSERT_TRUE(fx_write_file("b/y", "1"));
    collect(3000);
    ASSERT_EQ(0, events_for(a));
    ASSERT_TRUE(expect(b, FS_W_CREATED, "y"));
}

/* =========================================================================
 * Waiting
 * ========================================================================= */

/* Nothing happens: the wait returns after about timeout_ms, count 0. */
TEST(watch_wait_times_out)
{
    ASSERT_TRUE(fx_mkdir("d"));
    create_watcher();
    watch("d");
    fs_watch_event_t ev[4];
    size_t count = 99;
    uint64_t start = now_ms();
    ASSERT_EQ(FS_OK, KIND(fs_watch_wait(w, ev, 4, 150, &count)));
    uint64_t took = now_ms() - start;
    ASSERT_EQ(0u, count);
    ASSERT_TRUE(took >= 100);
    ASSERT_TRUE(took < 2000);

    count = 99;
    ASSERT_EQ(FS_OK, KIND(fs_watch_wait(w, ev, 4, 0, &count))); /* look */
    ASSERT_EQ(0u, count);
}

/* A wake before the wait: the next wait returns at once. */
TEST(watch_wake_before_wait)
{
    create_watcher();
    fs_watch_wake(w);
    fs_watch_event_t ev[4];
    size_t count = 99;
    uint64_t start = now_ms();
    ASSERT_EQ(FS_OK, KIND(fs_watch_wait(w, ev, 4, 10000, &count)));
    ASSERT_EQ(0u, count);
    ASSERT_TRUE(now_ms() - start < 2000);
}

#ifdef _WIN32
static unsigned __stdcall waker(void *arg)
#else
static void *waker(void *arg)
#endif
{
    sleep_ms(200);
    fs_watch_wake((fs_watch_t *)arg);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Another thread wakes the waiting one. */
TEST(watch_wake_from_another_thread)
{
    create_watcher();
#ifdef _WIN32
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, waker, w, 0, NULL);
    ASSERT_TRUE(t != NULL);
#else
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, waker, w));
#endif
    fs_watch_event_t ev[4];
    size_t count = 99;
    uint64_t start = now_ms();
    ASSERT_EQ(FS_OK, KIND(fs_watch_wait(w, ev, 4, 10000, &count)));
    uint64_t took = now_ms() - start;
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
    ASSERT_EQ(0u, count);
    ASSERT_TRUE(took < 5000);
}

TEST(watch_wait_checks_its_arguments)
{
    create_watcher();
    fs_watch_event_t ev[4];
    size_t count;
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_watch_wait(NULL, ev, 4, 0, &count)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_watch_wait(w, NULL, 4, 0, &count)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_watch_wait(w, ev, 0, 0, &count)));
    ASSERT_EQ(FS_ERR_INVALID, KIND(fs_watch_wait(w, ev, 4, 0, NULL)));
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "filec fs watching");
}
