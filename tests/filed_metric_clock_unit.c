#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static unsigned clock_calls;
static int clock_failed;
static int metric_test_clock_gettime(clockid_t id, struct timespec *ts)
{
    assert(id == CLOCK_MONOTONIC);
    ++clock_calls;
    ts->tv_sec = 2;
    ts->tv_nsec = 3;
    return clock_failed ? -1 : 0;
}

#define clock_gettime metric_test_clock_gettime
#include "../userland/filed/src/metrics.c"
#include "../userland/filed/src/kobox_backend.c"
#undef clock_gettime

int main(void)
{
    /* Include the production collectors, not a second implementation. */
    static filed_dispatch_state_t state;
    filed_runtime_t runtime_value = { .dispatch_state = &state };
    filed_runtime_t *runtime = &runtime_value;
    filed_kobox_backend_t backend = {0};
    (void)metric_test_clock_gettime;
    for (unsigned i = 0; i < 100; ++i) {
        const uint64_t a = filed_now_ns();
        const uint64_t b = filed_kobox_backend_now_ns();
        assert(a == (FILED_WALL_TIME_METRICS ? 2000000003ull : 0));
        assert(a == b);
    }
    assert(clock_calls == (FILED_WALL_TIME_METRICS ? 200u : 0u));
    clock_failed = 1;
    assert(filed_now_ns() == 0 && filed_kobox_backend_now_ns() == 0);

    /* No ns sample, failed/backwards sample, then a valid sample. Counts,
     * errors and cycles must be independent of wall-clock availability. */
    const uint64_t starts[] = {0, 50, 20};
    const uint64_t ends[] = {0, 40, 80};
    for (unsigned i = 0; i < 3; ++i) {
        filed_record_dispatch_metric(runtime, FILED_OP_VFS_PREAD,
            starts[i], ends[i], 100, 150, i == 1 ? -5 : 0);
        filed_kobox_backend_record_metric(&backend, STORAGE_OP_PREAD,
            starts[i], ends[i], 100, 150, i == 1 ? -5 : 0);
    }
    const filed_dispatch_metric_t *d = &filed_dispatch_metrics[FILED_OP_VFS_PREAD];
    const filed_kobox_backend_metric_t *b = &backend.metrics[filed_kobox_backend_metric_slot(STORAGE_OP_PREAD)];
    assert(d->count == 3 && b->count == 3);
    assert(d->reply_errors == 1 && b->errors == 1);
    assert(d->total_cycles == 150 && b->total_cycles == 150);
    assert(d->max_cycles == 50 && b->max_cycles == 50);
    assert(d->total_ns == 60 && b->total_ns == 60);
    assert(d->max_ns == 60 && b->max_ns == 60);
    filed_record_dispatch_metric(runtime, FILED_METRIC_OP_MAX, 1, 2, 1, 2, 0);
    filed_kobox_backend_record_metric(&backend, UINT64_MAX, 1, 2, 1, 2, 0);
    filed_kobox_backend_record_metric(NULL, STORAGE_OP_PREAD, 1, 2, 1, 2, 0);
    puts("filed metric clock: PASS");
    return 0;
}
