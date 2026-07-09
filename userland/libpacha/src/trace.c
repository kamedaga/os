#include "pacha/trace.h"

#include <stddef.h>

#define PACHA_TRACE_SYSCALL_LOG 1ull

static uint64_t pacha_trace_component_mask = PACHA_TRACE_COMPONENT_MASK_ALL;
static uint32_t pacha_trace_class_mask = PACHA_TRACE_CLASS_MASK_DEFAULT;
static pacha_trace_write_fn pacha_trace_writer;
static pacha_trace_record_t pacha_trace_ring[PACHA_TRACE_RING_CAPACITY];
static uint64_t pacha_trace_next_record;

static long pacha_trace_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return (long)ret;
}

uint64_t pacha_trace_read_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

uint64_t pacha_trace_name_id(const char *name)
{
    uint64_t hash = 1469598103934665603ull;
    if (name == NULL) {
        return 0;
    }
    while (*name != 0) {
        hash ^= (uint64_t)(unsigned char)*name++;
        hash *= 1099511628211ull;
    }
    return hash;
}

const char *pacha_trace_component_name(uint32_t component)
{
    switch (component) {
    case PACHA_TRACE_COMPONENT_LPR: return "lpr";
    case PACHA_TRACE_COMPONENT_FILED: return "filed";
    case PACHA_TRACE_COMPONENT_TERMD: return "termd";
    case PACHA_TRACE_COMPONENT_NETD: return "netd";
    case PACHA_TRACE_COMPONENT_KOBOXD: return "koboxd";
    case PACHA_TRACE_COMPONENT_LPR_SUPERVISOR: return "lpr_supervisor";
    default: return "unknown";
    }
}

const char *pacha_trace_event_name(uint32_t event)
{
    switch (event) {
    case PACHA_TRACE_EVENT_GENERIC_ERROR: return "error";
    case PACHA_TRACE_EVENT_GENERIC_STATE: return "state";
    case PACHA_TRACE_EVENT_METRIC_COUNTER: return "metric.counter";
    case PACHA_TRACE_EVENT_METRIC_TIMING: return "metric.timing";
    case PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA: return "metric.timing_extra";
    case PACHA_TRACE_EVENT_LPR_ENOSYS: return "enosys";
    case PACHA_TRACE_EVENT_LPR_BAD_RETURN: return "bad_return";
    case PACHA_TRACE_EVENT_LPR_MMAP_CALL: return "mmap.call";
    case PACHA_TRACE_EVENT_LPR_MMAP_ERROR: return "mmap.error";
    case PACHA_TRACE_EVENT_LPR_MMAP_LOAD: return "mmap.load";
    case PACHA_TRACE_EVENT_LPR_FILE_MAP_CACHE: return "file_map_cache";
    case PACHA_TRACE_EVENT_LPR_PATCH_MAPPING: return "patch_mapping";
    case PACHA_TRACE_EVENT_LPR_SYSCALL_ENTER: return "syscall.enter";
    case PACHA_TRACE_EVENT_LPR_SYSCALL_EXIT: return "syscall.exit";
    case PACHA_TRACE_EVENT_LPR_SLOW_SYSCALL: return "syscall.slow";
    case PACHA_TRACE_EVENT_LPR_SYSCALL_METRIC: return "syscall.metric";
    case PACHA_TRACE_EVENT_LPR_SYSCALL_SUMMARY: return "syscall.summary";
    case PACHA_TRACE_EVENT_LPR_CLONE_ARGS: return "clone.args";
    case PACHA_TRACE_EVENT_LPR_CLONE_FRAME: return "clone.frame";
    case PACHA_TRACE_EVENT_LPR_PROCESS: return "process";
    case PACHA_TRACE_EVENT_LPR_READV_SIZE: return "readv.size";
    case PACHA_TRACE_EVENT_LPR_READV_TO_VMO_STATUS: return "readv.to_vmo_status";
    case PACHA_TRACE_EVENT_LPR_READV_CACHE_METRIC: return "readv.cache_metric";
    case PACHA_TRACE_EVENT_LPR_SOCKET_CREATE: return "socket.create";
    case PACHA_TRACE_EVENT_LPR_SOCKET_CONNECT: return "socket.connect";
    case PACHA_TRACE_EVENT_LPR_NETD_CALL: return "netd.call";
    case PACHA_TRACE_EVENT_LPR_IMAGE_ABI_MISMATCH: return "image_abi.mismatch";
    case PACHA_TRACE_EVENT_FILED_METRIC_DISPATCH: return "filed.metric.dispatch";
    case PACHA_TRACE_EVENT_FILED_METRIC_FAST: return "filed.metric.fast";
    case PACHA_TRACE_EVENT_FILED_METRIC_FAST_OP: return "filed.metric.fast_op";
    case PACHA_TRACE_EVENT_FILED_METRIC_CACHE: return "filed.metric.cache";
    case PACHA_TRACE_EVENT_FILED_METRIC_LOOKUP: return "filed.metric.lookup";
    case PACHA_TRACE_EVENT_FILED_METRIC_FILE_VMO: return "filed.metric.file_vmo";
    case PACHA_TRACE_EVENT_FILED_EXEC_METRIC: return "filed.metric.exec";
    case PACHA_TRACE_EVENT_TERMD_BOOT_CONFIG_INVALID: return "boot_config.invalid";
    case PACHA_TRACE_EVENT_TERMD_ISLAND_INIT: return "island.init";
    case PACHA_TRACE_EVENT_TERMD_BOOT_READY_SEND: return "boot_ready.send";
    case PACHA_TRACE_EVENT_TERMD_RECV: return "recv";
    case PACHA_TRACE_EVENT_TERMD_TTY_STATE: return "tty.state";
    case PACHA_TRACE_EVENT_NETD_METRIC: return "netd.metric";
    case PACHA_TRACE_EVENT_NETD_SOCKET: return "netd.socket";
    case PACHA_TRACE_EVENT_NETD_DEVICE: return "netd.device";
    case PACHA_TRACE_EVENT_NETD_LIBUINET: return "netd.libuinet";
    case PACHA_TRACE_EVENT_KOBOXD_CONTROL: return "control";
    case PACHA_TRACE_EVENT_KOBOXD_STORAGE: return "storage";
    case PACHA_TRACE_EVENT_KOBOXD_FS_METRIC: return "fs.metric";
    case PACHA_TRACE_EVENT_LPRS_BOOTSTRAP: return "bootstrap";
    case PACHA_TRACE_EVENT_LPRS_RECV: return "recv";
    default: return "unknown";
    }
}

void pacha_trace_set_writer(pacha_trace_write_fn writer)
{
    pacha_trace_writer = writer;
}

void pacha_trace_set_masks(uint64_t component_mask, uint32_t class_mask)
{
    pacha_trace_component_mask = component_mask;
    pacha_trace_class_mask = class_mask;
}

void pacha_trace_get_masks(uint64_t *component_mask, uint32_t *class_mask)
{
    if (component_mask != NULL) {
        *component_mask = pacha_trace_component_mask;
    }
    if (class_mask != NULL) {
        *class_mask = pacha_trace_class_mask;
    }
}

void pacha_trace_enable_all(void)
{
    pacha_trace_set_masks(PACHA_TRACE_COMPONENT_MASK_ALL, PACHA_TRACE_CLASS_MASK_ALL);
}

int pacha_trace_enabled(uint32_t component, uint32_t event_class)
{
    if (component >= 64u) {
        return 0;
    }
    return ((pacha_trace_component_mask & (1ull << component)) != 0) &&
           ((pacha_trace_class_mask & event_class) != 0);
}

static char *pacha_trace_append_literal(char *out, const char *end, const char *text)
{
    while (out < end && *text != 0) {
        *out++ = *text++;
    }
    return out;
}

static char *pacha_trace_append_u64(char *out, const char *end, uint64_t value)
{
    char tmp[20];
    uint64_t n = 0;
    do {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && n < sizeof(tmp));
    while (out < end && n != 0) {
        *out++ = tmp[--n];
    }
    return out;
}

static void pacha_trace_write(const char *data, uint64_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    if (pacha_trace_writer != NULL) {
        pacha_trace_writer(data, len);
        return;
    }
    (void)pacha_trace_syscall2(PACHA_TRACE_SYSCALL_LOG, (uint64_t)(uintptr_t)data, len);
}

static void pacha_trace_write_record(const pacha_trace_record_t *record)
{
    char line[384];
    char *out = line;
    const char *end = line + sizeof(line);
    out = pacha_trace_append_literal(out, end, "[trace] c=");
    out = pacha_trace_append_literal(out, end, pacha_trace_component_name(record->component));
    out = pacha_trace_append_literal(out, end, " e=");
    out = pacha_trace_append_literal(out, end, pacha_trace_event_name(record->event));
    out = pacha_trace_append_literal(out, end, " cls=");
    out = pacha_trace_append_u64(out, end, record->event_class);
    out = pacha_trace_append_literal(out, end, " tsc=");
    out = pacha_trace_append_u64(out, end, record->tsc);
    const uint32_t arg_count = record->arg_count <= PACHA_TRACE_MAX_ARGS ?
        record->arg_count :
        PACHA_TRACE_MAX_ARGS;
    for (uint32_t i = 0; i < arg_count; ++i) {
        out = pacha_trace_append_literal(out, end, " a");
        out = pacha_trace_append_u64(out, end, i);
        out = pacha_trace_append_literal(out, end, "=");
        out = pacha_trace_append_u64(out, end, record->args[i]);
    }
    if (out < end) {
        *out++ = '\n';
    }
    pacha_trace_write(line, (uint64_t)(out - line));
}

void pacha_trace_emit(uint32_t component,
                      uint32_t event,
                      uint32_t event_class,
                      uint32_t arg_count,
                      uint64_t a0,
                      uint64_t a1,
                      uint64_t a2,
                      uint64_t a3,
                      uint64_t a4,
                      uint64_t a5)
{
    if (!pacha_trace_enabled(component, event_class)) {
        return;
    }
    pacha_trace_record_t record;
    record.tsc = pacha_trace_read_tsc();
    record.component = component;
    record.event = event;
    record.event_class = event_class;
    record.arg_count = arg_count <= PACHA_TRACE_MAX_ARGS ? arg_count : PACHA_TRACE_MAX_ARGS;
    record.args[0] = a0;
    record.args[1] = a1;
    record.args[2] = a2;
    record.args[3] = a3;
    record.args[4] = a4;
    record.args[5] = a5;
    pacha_trace_record_t *slot = &pacha_trace_ring[pacha_trace_next_record % PACHA_TRACE_RING_CAPACITY];
    slot->tsc = record.tsc;
    slot->component = record.component;
    slot->event = record.event;
    slot->event_class = record.event_class;
    slot->arg_count = record.arg_count;
    for (uint32_t i = 0; i < PACHA_TRACE_MAX_ARGS; ++i) {
        slot->args[i] = record.args[i];
    }
    pacha_trace_next_record++;
    pacha_trace_write_record(&record);
}

void pacha_trace_dump_ring(void)
{
    const uint64_t count = pacha_trace_next_record < PACHA_TRACE_RING_CAPACITY ?
        pacha_trace_next_record :
        PACHA_TRACE_RING_CAPACITY;
    const uint64_t start = pacha_trace_next_record >= count ? pacha_trace_next_record - count : 0;
    for (uint64_t i = 0; i < count; ++i) {
        const pacha_trace_record_t *record =
            &pacha_trace_ring[(start + i) % PACHA_TRACE_RING_CAPACITY];
        pacha_trace_write_record(record);
    }
}
