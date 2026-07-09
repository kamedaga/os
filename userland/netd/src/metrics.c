#include "netd_internal.h"

#include <pacha/trace.h>
#include <stdio.h>

struct netd_metric {
    const char *stage;
    const char *name;
    uint64_t cycles;
    uint64_t size;
};

enum {
    NETD_MAX_METRICS = 32,
};

static struct netd_metric g_netd_metrics[NETD_MAX_METRICS];
static unsigned g_netd_metric_count;
static int g_netd_metrics_enabled;

uint64_t netd_metrics_read_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

void netd_metrics_set_enabled(int enabled)
{
    g_netd_metrics_enabled = enabled ? 1 : 0;
}

void netd_metrics_record_ex(
    const char *stage,
    const char *name,
    uint64_t start_cycles,
    uint64_t end_cycles,
    uint64_t size)
{
    if (!g_netd_metrics_enabled || stage == NULL || start_cycles == 0 || end_cycles < start_cycles ||
        g_netd_metric_count >= NETD_MAX_METRICS) {
        return;
    }
    struct netd_metric *metric = &g_netd_metrics[g_netd_metric_count++];
    metric->stage = stage;
    metric->name = name;
    metric->cycles = end_cycles - start_cycles;
    metric->size = size;
}

void netd_metrics_record(const char *stage, uint64_t start_cycles, uint64_t end_cycles)
{
    netd_metrics_record_ex(stage, NULL, start_cycles, end_cycles, 0);
}

void netd_metrics_print(void)
{
    for (unsigned i = 0; i < g_netd_metric_count; i++) {
        const struct netd_metric *metric = &g_netd_metrics[i];
        if (metric->name != NULL) {
            pacha_trace4(
                PACHA_TRACE_COMPONENT_NETD,
                PACHA_TRACE_EVENT_NETD_METRIC,
                PACHA_TRACE_CLASS_METRIC,
                pacha_trace_name_id(metric->stage),
                pacha_trace_name_id(metric->name),
                metric->cycles,
                metric->size);
        } else {
            pacha_trace2(
                PACHA_TRACE_COMPONENT_NETD,
                PACHA_TRACE_EVENT_NETD_METRIC,
                PACHA_TRACE_CLASS_METRIC,
                pacha_trace_name_id(metric->stage),
                metric->cycles);
        }
    }
}
