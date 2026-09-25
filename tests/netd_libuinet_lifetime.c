/* Exercise the real netd libuinet archive without a VM or external traffic. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include "uinet_api.h"
#include "uinet_pachaos_api.h"

/* This fixture sends only unsupported Ethernet frames (synchronously dropped).
 * It does not test TCP timers; no timer is allowed to fire during the fixture. */
static void *instance_created(void *ctx, uinet_instance_t instance)
{ (void)ctx; return instance; }
static void *if_created(void *ctx, uinet_if_t interface)
{ (void)ctx; return interface; }
static void destroyed(void *ctx) { (void)ctx; }
static void notify(void *ctx) { uinet_instance_sts_events_process(ctx); }
static void timer_init(void *ctx, void *storage)
{ (void)ctx; memset(storage, 0, sizeof(int)); }
static int timer_schedule(void *ctx, void *storage, int ticks)
{ (void)ctx; (void)ticks; int old = *(int *)storage; *(int *)storage = 1; return old; }
static int timer_reset(void *ctx, void *storage, int ticks, void (*fn)(void *), void *arg)
{ (void)fn; (void)arg; return timer_schedule(ctx, storage, ticks); }
static int timer_pending(void *ctx, void *storage)
{ (void)ctx; return *(int *)storage; }
static void timer_deactivate(void *ctx, void *storage)
{ (void)ctx; *(int *)storage = 0; }
static int timer_stop(void *ctx, void *storage)
{ int old = timer_pending(ctx, storage); timer_deactivate(ctx, storage); return old; }

static void fault(int signal_number)
{
    void *frames[32];
    int count = backtrace(frames, 32);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    _exit(128 + signal_number);
}

static int fail_init(void *mem, int size, int flags)
{ (void)mem; (void)size; (void)flags; return 1; }

static int check_allocation_failure(void)
{
    /* Empty-bucket cleanup must also work for the bootstrap UMA zones. */
    uinet_pool_t failure = uinet_pool_create("fail-init", 64, NULL, NULL,
        fail_init, NULL, UINET_POOL_ALIGN_PTR, 0);
    if (failure == NULL) return 4;
    for (unsigned i = 0; i < 64; ++i)
        if (uinet_pool_alloc(failure, UINET_POOL_ALLOC_NOWAIT) != NULL) return 5;
    uinet_pool_destroy(failure);

    uinet_pool_t limited = uinet_pool_create("limited", 64, NULL, NULL,
        NULL, NULL, UINET_POOL_ALIGN_PTR, 0);
    if (limited == NULL) return 6;
    (void)uinet_pool_set_max(limited, 64);
    for (unsigned pass = 0; pass < 4; ++pass) {
        void *items[1024];
        unsigned count = 0;
        while (count < 1024) {
            void *item = uinet_pool_alloc(limited, UINET_POOL_ALLOC_NOWAIT);
            if (item == NULL) break;
            items[count++] = item;
        }
        if (count == 0 || count == 1024) return 7;
        for (unsigned i = 0; i < count; ++i) uinet_pool_free(limited, items[i]);
    }
    uinet_pool_destroy(limited);
    puts("netd libuinet lifetime: allocation failure and reuse passed");
    return 0;
}

int main(void)
{
    signal(SIGSEGV, fault);
    struct uinet_global_cfg cfg;
    struct uinet_instance_cfg instance;
    uinet_default_cfg(&cfg, UINET_GLOBAL_CFG_SMALL);
    uinet_instance_default_cfg(&instance);
    instance.sts = (struct uinet_sts_cfg) {
        .sts_enabled = 1,
        .sts_instance_created_cb = instance_created,
        .sts_instance_destroyed_cb = destroyed,
        .sts_instance_event_notify_cb = notify,
        .sts_if_created_cb = if_created,
        .sts_if_destroyed_cb = destroyed,
        .sts_callout_init = timer_init,
        .sts_callout_reset = timer_reset,
        .sts_callout_schedule = timer_schedule,
        .sts_callout_pending = timer_pending,
        .sts_callout_active = timer_pending,
        .sts_callout_deactivate = timer_deactivate,
        .sts_callout_msecs_remaining = timer_pending,
        .sts_callout_stop = timer_stop,
    };
    if (uinet_init(&cfg, &instance) || uinet_initialize_thread("lifetime-test")) return 1;
    int failure_status = check_allocation_failure();
    if (failure_status) return failure_status;
    struct uinet_if_cfg ifcfg;
    uinet_if_default_config(UINET_IFTYPE_PACHAOS, &ifcfg);
    ifcfg.configstr = "lifetime-test";
    uinet_if_t interface;
    if (uinet_ifcreate(uinet_instance_default(), &ifcfg, &interface)) return 2;
    /* Local unicast, experimental EtherType: must be freed by the real stack. */
    unsigned char frame[60] = {
        0x52, 0x54, 0, 0x12, 0x34, 0x56,
        0x52, 0x54, 0, 0x12, 0x34, 0x57, 0x88, 0xb5,
    };
    for (unsigned i = 0; i < 100000; ++i) {
        int status = uinet_pachaos_if_deliver(interface, frame, sizeof(frame));
        if (status) {
            fprintf(stderr, "RX lifetime failed at frame %u: %d\n", i, status);
            return 3;
        }
    }
    puts("netd libuinet lifetime: 100000 frames passed (4096 cluster limit)");
    return 0;
}
