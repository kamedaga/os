#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifndef PAGE_TIMING_MINIMAL
#define PAGE_TIMING_MINIMAL 0
#endif
#ifdef PAGE_IDB_TIMING
#include "page_idb_timing.h"
#define SNAPSHOT_EXTRA ",idb:globalThis.__pachaIdbTiming"
#define SNAPSHOT_WORLD NULL
#else
#define SNAPSHOT_EXTRA ""
#define SNAPSHOT_WORLD "pacha-page-timing"
#endif

/* Diagnostic preload for actual Epiphany navigations, not a replacement
 * browser. Uses public WebKit/GLib APIs; no upstream patch or navigation
 * setting changes. UI signal times include IPC/event-loop delay. The JS
 * result separately reports the page's Navigation/Resource Timing data. */
static uint64_t ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static uint64_t previous_tick;
static int main_tick(void *data)
{
    (void)data;
    uint64_t current = ms();
    if (previous_tick && current - previous_tick > 250)
        dprintf(2, "PAGE_MAIN_GAP ms=%llu pid=%ld gap_ms=%llu\n",
            (unsigned long long)current, (long)getpid(),
            (unsigned long long)(current - previous_tick));
    previous_tick = current;
    return 1;
}
void *g_subprocess_launcher_spawnv(void *launcher, const char *const *argv, void **error)
{
    void *(*next)(void *, const char *const *, void **) =
        dlsym(RTLD_NEXT, "g_subprocess_launcher_spawnv");
    if (PAGE_TIMING_MINIMAL) return next(launcher, argv, error);
    uint64_t start = ms();
    void *result = next(launcher, argv, error);
    int saved = errno;
    dprintf(2, "PAGE_SPAWN ms=%llu pid=%ld elapsed_ms=%llu ok=%d path=%.180s\n",
        (unsigned long long)ms(), (long)getpid(),
        (unsigned long long)(ms() - start), result != NULL,
        argv && argv[0] ? argv[0] : "-");
    errno = saved;
    return result;
}
static const char *uri(void *view)
{
    const char *(*f)(void *) = dlsym(RTLD_DEFAULT, "webkit_web_view_get_uri");
    const char *s = f ? f(view) : NULL;
    return s ? s : "-";
}
/* Two bounded post-load observations also cover SPAs whose fetch/render work
 * continues after load. No DOM traversal, layout query or application hook. */
struct delayed_snapshot { void *view; uintptr_t generation; unsigned delay; uint64_t requested; };
static uintptr_t generation(void *view)
{
    void *(*get)(void *, const char *) = dlsym(RTLD_DEFAULT, "g_object_get_data");
    return (uintptr_t)get(view, "pacha-page-generation");
}
static void release_snapshot(struct delayed_snapshot *snapshot)
{
    void (*unref)(void *) = dlsym(RTLD_DEFAULT, "g_object_unref");
    unref(snapshot->view);
    free(snapshot);
}
static void snapshot_ready(void *view, void *result, void *data)
{
    struct delayed_snapshot *snapshot = data;
    void *(*finish)(void *, void *, void **) = dlsym(RTLD_DEFAULT, "webkit_web_view_evaluate_javascript_finish");
    char *(*string)(void *) = dlsym(RTLD_DEFAULT, "jsc_value_to_string");
    void (*unref)(void *) = dlsym(RTLD_DEFAULT, "g_object_unref");
    void (*release)(void *) = dlsym(RTLD_DEFAULT, "g_free");
    void *value = finish(view, result, NULL);
    if (value) {
        char *json = string(value);
        if (json && generation(view) == snapshot->generation)
            dprintf(2, "PAGE_SNAPSHOT ms=%llu view=%p delay_ms=%u requested_ms=%llu data=%s\n",
                (unsigned long long)ms(), view, snapshot->delay,
                (unsigned long long)snapshot->requested, json);
        if (json) release(json);
        unref(value);
    }
    release_snapshot(snapshot);
}
static int take_snapshot(void *data)
{
    struct delayed_snapshot *snapshot = data;
    void (*evaluate)(void *, const char *, long, const char *, const char *, void *,
        void (*)(void *, void *, void *), void *) =
        dlsym(RTLD_DEFAULT, "webkit_web_view_evaluate_javascript");
    if (!evaluate || generation(snapshot->view) != snapshot->generation) {
        release_snapshot(snapshot);
        return 0;
    }
    snapshot->requested = ms();
    evaluate(snapshot->view,
        "JSON.stringify({url:location.href,now:performance.now(),visibility:document.visibilityState,ready:document.readyState,paint:performance.getEntriesByType('paint').map(p=>({name:p.name,start:p.startTime})),r:performance.getEntriesByType('resource').filter(r=>r.initiatorType==='fetch'||r.initiatorType==='xmlhttprequest'||r.initiatorType==='script').slice(-24).map(r=>({name:r.name,type:r.initiatorType,start:r.startTime,end:r.responseEnd,duration:r.duration,size:r.transferSize}))"
        SNAPSHOT_EXTRA "})",
        -1, SNAPSHOT_WORLD, NULL, NULL, snapshot_ready, snapshot);
    return 0;
}
static void schedule_snapshots(void *view)
{
    unsigned (*timer)(unsigned, int (*)(void *), void *) = dlsym(RTLD_DEFAULT, "g_timeout_add");
    void *(*ref)(void *) = dlsym(RTLD_DEFAULT, "g_object_ref");
    if (!timer || !ref) return;
    const unsigned delays[] = {5000, 15000};
    for (unsigned i = 0; i < 2; ++i) {
        struct delayed_snapshot *snapshot = malloc(sizeof(*snapshot));
        if (!snapshot) return;
        *snapshot = (struct delayed_snapshot){ ref(view), generation(view), delays[i], 0 };
        if (!timer(delays[i], take_snapshot, snapshot)) release_snapshot(snapshot);
    }
}
static void timing_ready(void *view, void *result, void *data)
{
    (void)data;
    void *(*finish)(void *, void *, void **) = dlsym(RTLD_DEFAULT, "webkit_web_view_evaluate_javascript_finish");
    char *(*string)(void *) = dlsym(RTLD_DEFAULT, "jsc_value_to_string");
    void (*unref)(void *) = dlsym(RTLD_DEFAULT, "g_object_unref");
    void (*release)(void *) = dlsym(RTLD_DEFAULT, "g_free");
    void *value = finish(view, result, NULL);
    if (value) {
        char *json = string(value);
        if (json) { dprintf(2, "PAGE_TIMING view=%p data=%s\n", view, json); release(json); }
        unref(value);
    } else dprintf(2, "PAGE_TIMING view=%p evaluation_failed=1\n", view);
}
static void changed(void *view, unsigned event, void *data)
{
    (void)data;
    if (event == 0) {
        void (*set)(void *, const char *, void *) = dlsym(RTLD_DEFAULT, "g_object_set_data");
        set(view, "pacha-page-generation", (void *)(generation(view) + 1));
    }
    dprintf(2, "PAGE_EVENT ms=%llu view=%p event=%u uri=%.240s\n",
        (unsigned long long)ms(), view, event, uri(view));
    if (event != 3) return; /* WEBKIT_LOAD_FINISHED, not proof of success */
    if (!PAGE_TIMING_MINIMAL) schedule_snapshots(view);
    void (*evaluate)(void *, const char *, long, const char *, const char *, void *,
        void (*)(void *, void *, void *), void *) =
        dlsym(RTLD_DEFAULT, "webkit_web_view_evaluate_javascript");
    /* Console writes themselves use DMA on the guest. The minimal build
     * keeps navigation/error records but excludes resource dumps and timers
     * so measurement does not introduce their per-resource I/O and wakeups. */
    if (evaluate) evaluate(view, PAGE_TIMING_MINIMAL ?
        "JSON.stringify({url:location.href,paint:performance.getEntriesByType('paint').map(p=>({name:p.name,start:p.startTime})),n:performance.getEntriesByType('navigation').map(n=>({start:n.startTime,responseEnd:n.responseEnd,interactive:n.domInteractive,dcl:n.domContentLoadedEventEnd,load:n.loadEventEnd}))})" :
        "JSON.stringify({url:location.href,probe:globalThis.pachaTiming,paint:performance.getEntriesByType('paint').map(p=>({name:p.name,start:p.startTime})),n:performance.getEntriesByType('navigation').map(n=>({start:n.startTime,dns:n.domainLookupEnd-n.domainLookupStart,connect:n.connectEnd-n.connectStart,responseStart:n.responseStart,responseEnd:n.responseEnd,interactive:n.domInteractive,dcl:n.domContentLoadedEventEnd,load:n.loadEventEnd})),r:performance.getEntriesByType('resource').slice(0,60).map(r=>({name:r.name,start:r.startTime,request:r.requestStart,first:r.responseStart,end:r.responseEnd,duration:r.duration,size:r.transferSize}))})",
        -1, "pacha-page-timing", NULL, NULL, timing_ready, NULL);
}
static int failed(void *view, unsigned event, const char *url, void *error, void *data)
{
    (void)error; (void)data;
    dprintf(2, "PAGE_FAILED ms=%llu view=%p event=%u uri=%.240s\n",
        (unsigned long long)ms(), view, event, url ? url : "-");
    return 0; /* preserve the application's normal error handling */
}
static void terminated(void *view, unsigned reason, void *data)
{
    (void)data;
    /* WebKit enum: 0 crash, 1 memory limit, 2 application request. */
    dprintf(2, "PAGE_TERMINATED ms=%llu view=%p reason=%u uri=%.240s\n",
        (unsigned long long)ms(), view, reason, uri(view));
}
struct resource_clock { uint64_t start; void *view; };
static void release_clock(void *data, void *closure)
{
    (void)closure; free(data);
}
static void resource_finished(void *resource, void *data)
{
    struct resource_clock *clock = data;
    const char *(*get_uri)(void *) = dlsym(RTLD_DEFAULT, "webkit_web_resource_get_uri");
    const char *url = get_uri(resource);
    dprintf(2, "PAGE_RESOURCE ms=%llu view=%p elapsed_ms=%llu uri=%.240s\n",
        (unsigned long long)ms(), clock->view,
        (unsigned long long)(ms() - clock->start), url ? url : "-");
}
static void resource_started(void *view, void *resource, void *request, void *data)
{
    (void)request; (void)data;
    static unsigned count;
    if (count++ >= 256) return;
    struct resource_clock *clock = malloc(sizeof(*clock));
    if (!clock) return;
    *clock = (struct resource_clock){ ms(), view };
    unsigned long (*connect)(void *, const char *, void (*)(void), void *,
        void (*)(void *, void *), int) = dlsym(RTLD_DEFAULT, "g_signal_connect_data");
    connect(resource, "finished", (void (*)(void))resource_finished, clock, release_clock, 0);
}
static void attach(void *view)
{
    void *(*get)(void *, const char *) = dlsym(RTLD_DEFAULT, "g_object_get_data");
    void (*set)(void *, const char *, void *) = dlsym(RTLD_DEFAULT, "g_object_set_data");
    unsigned long (*connect)(void *, const char *, void (*)(void), void *, void *, int) =
        dlsym(RTLD_DEFAULT, "g_signal_connect_data");
    if (!get || !set || !connect || get(view, "pacha-page-timing")) return;
    if (!PAGE_TIMING_MINIMAL && !previous_tick) {
        unsigned (*timer)(unsigned, int (*)(void *), void *) = dlsym(RTLD_DEFAULT, "g_timeout_add");
        previous_tick = ms();
        if (timer) timer(100, main_tick, NULL);
    }
    set(view, "pacha-page-timing", (void *)1);
    /* Isolated world: do not change page globals or its timer settings.
     * Hidden-page timer clamping is expected, so gaps alone are not proof
     * of a stalled WebProcess. Stop collecting once this navigation loads. */
    void *(*manager_for)(void *) = dlsym(RTLD_DEFAULT, "webkit_web_view_get_user_content_manager");
    void *(*script_new)(const char *, int, int, const char *, const char *const *, const char *const *) =
        dlsym(RTLD_DEFAULT, "webkit_user_script_new_for_world");
    void (*add_script)(void *, void *) = dlsym(RTLD_DEFAULT, "webkit_user_content_manager_add_script");
    void (*script_unref)(void *) = dlsym(RTLD_DEFAULT, "webkit_user_script_unref");
    void *manager = manager_for ? manager_for(view) : NULL;
#ifdef PAGE_IDB_TIMING
    void *(*page_script_new)(const char *, int, int, const char *const *, const char *const *) =
        dlsym(RTLD_DEFAULT, "webkit_user_script_new");
    if (!PAGE_TIMING_MINIMAL && manager && page_script_new && add_script && script_unref && !get(manager, "pacha-idb-timing")) {
        void *script = page_script_new(page_idb_script, 1, 0, NULL, NULL);
        if (script) { add_script(manager, script); script_unref(script); set(manager, "pacha-idb-timing", (void *)1); }
    }
#endif
    if (!PAGE_TIMING_MINIMAL && manager && script_new && add_script && script_unref && !get(manager, "pacha-page-timing")) {
        void *script = script_new(
            "(()=>{const p=globalThis.pachaTiming={start:performance.now(),visibility:document.visibilityState,events:[],gaps:[],longtasks:[],supported:PerformanceObserver.supportedEntryTypes};let last=p.start;"
            "const tick=setInterval(()=>{const n=performance.now();if(n-last>250&&p.gaps.length<64)p.gaps.push([last,n,document.visibilityState]);last=n},100);"
            "for(const e of ['readystatechange','visibilitychange','DOMContentLoaded'])document.addEventListener(e,()=>{if(p.events.length<32)p.events.push([e,performance.now(),document.readyState,document.visibilityState])});"
            "let observer;try{if(p.supported.includes('longtask')){observer=new PerformanceObserver(l=>{for(const e of l.getEntries())if(p.longtasks.length<64)p.longtasks.push([e.startTime,e.duration])});observer.observe({type:'longtask',buffered:true})}}catch(e){}"
            "addEventListener('load',()=>{clearInterval(tick);if(observer)observer.disconnect();p.end=performance.now()},{once:true})})()",
            1, 0, "pacha-page-timing", NULL, NULL);
        if (script) { add_script(manager, script); script_unref(script); set(manager, "pacha-page-timing", (void *)1); }
    }
    connect(view, "load-changed", (void (*)(void))changed, NULL, NULL, 0);
    connect(view, "load-failed", (void (*)(void))failed, NULL, NULL, 0);
    connect(view, "web-process-terminated", (void (*)(void))terminated, NULL, NULL, 0);
    if (!PAGE_TIMING_MINIMAL)
        connect(view, "resource-load-started", (void (*)(void))resource_started, NULL, NULL, 0);
    dprintf(2, "PAGE_ATTACH ms=%llu view=%p\n", (unsigned long long)ms(), view);
}
void webkit_web_view_load_uri(void *view, const char *url)
{
    void (*next)(void *, const char *) = dlsym(RTLD_NEXT, "webkit_web_view_load_uri");
    int saved = errno; attach(view); errno = saved; next(view, url);
}
void webkit_web_view_restore_session_state(void *view, void *state)
{
    void (*next)(void *, void *) = dlsym(RTLD_NEXT, "webkit_web_view_restore_session_state");
    int saved = errno; attach(view); errno = saved; next(view, state);
}
void webkit_web_view_go_to_back_forward_list_item(void *view, void *item)
{
    void (*next)(void *, void *) = dlsym(RTLD_NEXT, "webkit_web_view_go_to_back_forward_list_item");
    int saved = errno; attach(view); errno = saved; next(view, item);
}
void webkit_web_view_load_request(void *view, void *request)
{
    void (*next)(void *, void *) = dlsym(RTLD_NEXT, "webkit_web_view_load_request");
    int saved = errno; attach(view); errno = saved; next(view, request);
}
