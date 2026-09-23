#define dlsym test_dlsym
#include "epiphany_page_timing.c"
#undef dlsym
#include <assert.h>
#include <string.h>

static int view_object, value_object, references, evaluations, finishes, released_values;
static uintptr_t current_generation = 1;
static struct { int (*callback)(void *); void *data; unsigned delay; } timers[8];
static unsigned timer_count;
static void (*completion)(void *, void *, void *);
static void *completion_data;
static void *get_data(void *view, const char *key)
{
    assert(view == &view_object && !strcmp(key, "pacha-page-generation"));
    return (void *)current_generation;
}
static void *reference(void *view) { assert(view == &view_object); ++references; return view; }
static void unreference(void *object)
{
    if (object == &view_object) { assert(references > 0); --references; }
    else { assert(object == &value_object); ++released_values; }
}
static unsigned add_timer(unsigned delay, int (*callback)(void *), void *data)
{
    assert(timer_count < 8);
    timers[timer_count].callback = callback;
    timers[timer_count].data = data;
    timers[timer_count++].delay = delay;
    return timer_count;
}
static void evaluate(void *view, const char *script, long length, const char *world,
    const char *source, void *cancel, void (*done)(void *, void *, void *), void *data)
{
    assert(view == &view_object && length == -1 && !source && !cancel);
#ifdef PAGE_IDB_TIMING
    assert(!world && strstr(script, "idb:globalThis.__pachaIdbTiming"));
#else
    assert(!strcmp(world, "pacha-page-timing"));
#endif
    assert(strstr(script, "slice(-24)") && !strstr(script, "innerText"));
    assert(strstr(script, "r.initiatorType==='script'") && strstr(script, "type:r.initiatorType"));
    ++evaluations; completion = done; completion_data = data;
}
static void *finish(void *view, void *result, void **error)
{
    assert(view == &view_object && result == &value_object && !error);
    ++finishes; return &value_object;
}
static char *to_string(void *value) { assert(value == &value_object); return strdup("{}"); }
void *test_dlsym(void *handle, const char *name)
{
    assert(handle == RTLD_DEFAULT);
    if (!strcmp(name, "g_object_get_data")) return get_data;
    if (!strcmp(name, "g_object_ref")) return reference;
    if (!strcmp(name, "g_object_unref")) return unreference;
    if (!strcmp(name, "g_timeout_add")) return add_timer;
    if (!strcmp(name, "webkit_web_view_evaluate_javascript")) return evaluate;
    if (!strcmp(name, "webkit_web_view_evaluate_javascript_finish")) return finish;
    if (!strcmp(name, "jsc_value_to_string")) return to_string;
    if (!strcmp(name, "g_free")) return free;
    assert(!"unexpected symbol"); return NULL;
}
int main(void)
{
    schedule_snapshots(&view_object);
    assert(timer_count == 2 && references == 2);
    assert(timers[0].delay == 5000 && timers[1].delay == 15000);
    assert(!timers[0].callback(timers[0].data) && evaluations == 1);
    completion(&view_object, &value_object, completion_data);
    assert(references == 1 && finishes == 1 && released_values == 1);
    ++current_generation;
    assert(!timers[1].callback(timers[1].data) && evaluations == 1 && references == 0);
    schedule_snapshots(&view_object);
    assert(!timers[2].callback(timers[2].data) && evaluations == 2);
    ++current_generation; /* navigation changed while JavaScript was pending */
    completion(&view_object, &value_object, completion_data);
    assert(finishes == 2 && released_values == 2 && references == 1);
    assert(!timers[3].callback(timers[3].data) && references == 0);
    puts("page snapshots: bounded timers, references, stale navigation/response PASS");
    return 0;
}
