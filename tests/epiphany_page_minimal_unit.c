#define PAGE_TIMING_MINIMAL 1
#define dlsym test_dlsym
#include "epiphany_page_timing.c"
#undef dlsym
#include <assert.h>
#include <string.h>

static int view_object, attached, signals, evaluations, spawns;
static uintptr_t current_generation;
static void *get_data(void *view, const char *key)
{
    assert(view == &view_object);
    if (!strcmp(key, "pacha-page-timing")) return (void *)(uintptr_t)attached;
    assert(!strcmp(key, "pacha-page-generation"));
    return (void *)current_generation;
}
static void set_data(void *view, const char *key, void *value)
{
    assert(view == &view_object);
    if (!strcmp(key, "pacha-page-timing")) attached = (int)(uintptr_t)value;
    else {
        assert(!strcmp(key, "pacha-page-generation"));
        current_generation = (uintptr_t)value;
    }
}
static unsigned long connect_signal(void *view, const char *name,
    void (*callback)(void), void *data, void *destroy, int flags)
{
    assert(view == &view_object && !data && !destroy && !flags && callback);
    assert(!strcmp(name, "load-changed") || !strcmp(name, "load-failed") ||
        !strcmp(name, "web-process-terminated"));
    return ++signals;
}
static const char *get_uri(void *view)
{
    assert(view == &view_object);
    return "https://example.test/";
}
static void evaluate(void *view, const char *script, long length, const char *world,
    const char *source, void *cancel, void (*done)(void *, void *, void *), void *data)
{
    assert(view == &view_object && length == -1 && !source && !cancel && !data);
    assert(!strcmp(world, "pacha-page-timing") && done == timing_ready);
    assert(strstr(script, "responseEnd") && strstr(script, "domInteractive"));
    assert(!strstr(script, "pachaTiming") && !strstr(script, "'resource'"));
    assert(!strstr(script, "setInterval") && !strstr(script, "innerText"));
    ++evaluations;
}
static void *spawn(void *launcher, const char *const *argv, void **error)
{
    assert(launcher == &view_object && !argv && !error);
    ++spawns;
    errno = EAGAIN;
    return launcher;
}
void *test_dlsym(void *handle, const char *name)
{
    if (handle == RTLD_NEXT) {
        assert(!strcmp(name, "g_subprocess_launcher_spawnv"));
        return spawn;
    }
    assert(handle == RTLD_DEFAULT);
    if (!strcmp(name, "g_object_get_data")) return get_data;
    if (!strcmp(name, "g_object_set_data")) return set_data;
    if (!strcmp(name, "g_signal_connect_data")) return connect_signal;
    if (!strcmp(name, "webkit_web_view_get_uri")) return get_uri;
    if (!strcmp(name, "webkit_web_view_evaluate_javascript")) return evaluate;
    assert(strcmp(name, "g_timeout_add") && strcmp(name, "g_object_ref"));
    return NULL;
}
int main(void)
{
    attach(&view_object);
    attach(&view_object);
    assert(attached && signals == 3 && !previous_tick);
    changed(&view_object, 0, NULL);
    assert(current_generation == 1 && !evaluations);
    changed(&view_object, 2, NULL);
    changed(&view_object, 3, NULL);
    assert(evaluations == 1);
    assert(failed(&view_object, 0, NULL, NULL, NULL) == 0);
    terminated(&view_object, 0, NULL);
    assert(g_subprocess_launcher_spawnv(&view_object, NULL, NULL) == &view_object);
    assert(spawns == 1 && errno == EAGAIN);
    puts("minimal page timing: no timers/resources/scripts, errors and spawn semantics PASS");
}
