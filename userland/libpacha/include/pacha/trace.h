#ifndef PACHA_TRACE_H
#define PACHA_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PACHA_TRACE_MAX_ARGS 6u
#define PACHA_TRACE_RING_CAPACITY 128u

typedef enum pacha_trace_component {
    PACHA_TRACE_COMPONENT_LPR = 0,
    PACHA_TRACE_COMPONENT_FILED = 1,
    PACHA_TRACE_COMPONENT_TERMD = 2,
    PACHA_TRACE_COMPONENT_NETD = 3,
    PACHA_TRACE_COMPONENT_KOBOXD = 4,
    PACHA_TRACE_COMPONENT_LPR_SUPERVISOR = 5,
    PACHA_TRACE_COMPONENT_COUNT = 6,
} pacha_trace_component_t;

#define PACHA_TRACE_COMPONENT_BIT(component) (1ull << (uint64_t)(component))
#define PACHA_TRACE_COMPONENT_MASK_ALL ((1ull << PACHA_TRACE_COMPONENT_COUNT) - 1ull)

typedef enum pacha_trace_class {
    PACHA_TRACE_CLASS_ERROR = 1u << 0,
    PACHA_TRACE_CLASS_STATE = 1u << 1,
    PACHA_TRACE_CLASS_IO = 1u << 2,
    PACHA_TRACE_CLASS_METRIC = 1u << 3,
    PACHA_TRACE_CLASS_DEBUG = 1u << 4,
    PACHA_TRACE_CLASS_SYSCALL = 1u << 5,
} pacha_trace_class_t;

#define PACHA_TRACE_CLASS_MASK_DEFAULT PACHA_TRACE_CLASS_ERROR
#define PACHA_TRACE_CLASS_MASK_ALL \
    (PACHA_TRACE_CLASS_ERROR | PACHA_TRACE_CLASS_STATE | PACHA_TRACE_CLASS_IO | \
     PACHA_TRACE_CLASS_METRIC | PACHA_TRACE_CLASS_DEBUG | PACHA_TRACE_CLASS_SYSCALL)

typedef enum pacha_trace_event_id {
    PACHA_TRACE_EVENT_GENERIC_ERROR = 1,
    PACHA_TRACE_EVENT_GENERIC_STATE = 2,
    PACHA_TRACE_EVENT_METRIC_COUNTER = 3,
    PACHA_TRACE_EVENT_METRIC_TIMING = 4,
    PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA = 5,

    PACHA_TRACE_EVENT_LPR_ENOSYS = 100,
    PACHA_TRACE_EVENT_LPR_BAD_RETURN = 101,
    PACHA_TRACE_EVENT_LPR_MMAP_CALL = 102,
    PACHA_TRACE_EVENT_LPR_MMAP_ERROR = 103,
    PACHA_TRACE_EVENT_LPR_MMAP_LOAD = 104,
    PACHA_TRACE_EVENT_LPR_FILE_MAP_CACHE = 105,
    PACHA_TRACE_EVENT_LPR_PATCH_MAPPING = 106,
    PACHA_TRACE_EVENT_LPR_SYSCALL_ENTER = 107,
    PACHA_TRACE_EVENT_LPR_SYSCALL_EXIT = 108,
    PACHA_TRACE_EVENT_LPR_SLOW_SYSCALL = 109,
    PACHA_TRACE_EVENT_LPR_SYSCALL_METRIC = 110,
    PACHA_TRACE_EVENT_LPR_SYSCALL_SUMMARY = 111,
    PACHA_TRACE_EVENT_LPR_CLONE_ARGS = 112,
    PACHA_TRACE_EVENT_LPR_CLONE_FRAME = 113,
    PACHA_TRACE_EVENT_LPR_PROCESS = 114,
    PACHA_TRACE_EVENT_LPR_READV_SIZE = 115,
    PACHA_TRACE_EVENT_LPR_READV_TO_VMO_STATUS = 116,
    PACHA_TRACE_EVENT_LPR_READV_CACHE_METRIC = 117,
    PACHA_TRACE_EVENT_LPR_SOCKET_CREATE = 118,
    PACHA_TRACE_EVENT_LPR_SOCKET_CONNECT = 119,
    PACHA_TRACE_EVENT_LPR_NETD_CALL = 120,
    PACHA_TRACE_EVENT_LPR_IMAGE_ABI_MISMATCH = 121,

    PACHA_TRACE_EVENT_FILED_METRIC_DISPATCH = 200,
    PACHA_TRACE_EVENT_FILED_METRIC_FAST = 201,
    PACHA_TRACE_EVENT_FILED_METRIC_FAST_OP = 202,
    PACHA_TRACE_EVENT_FILED_METRIC_CACHE = 203,
    PACHA_TRACE_EVENT_FILED_METRIC_LOOKUP = 204,
    PACHA_TRACE_EVENT_FILED_METRIC_FILE_VMO = 205,
    PACHA_TRACE_EVENT_FILED_EXEC_METRIC = 206,

    PACHA_TRACE_EVENT_TERMD_BOOT_CONFIG_INVALID = 300,
    PACHA_TRACE_EVENT_TERMD_ISLAND_INIT = 301,
    PACHA_TRACE_EVENT_TERMD_BOOT_READY_SEND = 302,
    PACHA_TRACE_EVENT_TERMD_RECV = 303,
    PACHA_TRACE_EVENT_TERMD_TTY_STATE = 304,

    PACHA_TRACE_EVENT_NETD_METRIC = 400,
    PACHA_TRACE_EVENT_NETD_SOCKET = 401,
    PACHA_TRACE_EVENT_NETD_DEVICE = 402,
    PACHA_TRACE_EVENT_NETD_LIBUINET = 403,

    PACHA_TRACE_EVENT_KOBOXD_CONTROL = 500,
    PACHA_TRACE_EVENT_KOBOXD_STORAGE = 501,
    PACHA_TRACE_EVENT_KOBOXD_FS_METRIC = 502,

    PACHA_TRACE_EVENT_LPRS_BOOTSTRAP = 600,
    PACHA_TRACE_EVENT_LPRS_RECV = 601,
} pacha_trace_event_id_t;

typedef struct pacha_trace_record {
    uint64_t tsc;
    uint32_t component;
    uint32_t event;
    uint32_t event_class;
    uint32_t arg_count;
    uint64_t args[PACHA_TRACE_MAX_ARGS];
} pacha_trace_record_t;

typedef void (*pacha_trace_write_fn)(const char *data, uint64_t len);

uint64_t pacha_trace_read_tsc(void);
uint64_t pacha_trace_name_id(const char *name);
const char *pacha_trace_component_name(uint32_t component);
const char *pacha_trace_event_name(uint32_t event);

void pacha_trace_set_writer(pacha_trace_write_fn writer);
void pacha_trace_set_masks(uint64_t component_mask, uint32_t class_mask);
void pacha_trace_get_masks(uint64_t *component_mask, uint32_t *class_mask);
void pacha_trace_enable_all(void);
int pacha_trace_enabled(uint32_t component, uint32_t event_class);

void pacha_trace_emit(uint32_t component,
                      uint32_t event,
                      uint32_t event_class,
                      uint32_t arg_count,
                      uint64_t a0,
                      uint64_t a1,
                      uint64_t a2,
                      uint64_t a3,
                      uint64_t a4,
                      uint64_t a5);

void pacha_trace_dump_ring(void);

static inline void pacha_trace0(uint32_t component, uint32_t event, uint32_t event_class)
{
    pacha_trace_emit(component, event, event_class, 0, 0, 0, 0, 0, 0, 0);
}

static inline void pacha_trace1(uint32_t component, uint32_t event, uint32_t event_class, uint64_t a0)
{
    pacha_trace_emit(component, event, event_class, 1, a0, 0, 0, 0, 0, 0);
}

static inline void pacha_trace2(uint32_t component, uint32_t event, uint32_t event_class, uint64_t a0, uint64_t a1)
{
    pacha_trace_emit(component, event, event_class, 2, a0, a1, 0, 0, 0, 0);
}

static inline void pacha_trace3(uint32_t component, uint32_t event, uint32_t event_class, uint64_t a0, uint64_t a1, uint64_t a2)
{
    pacha_trace_emit(component, event, event_class, 3, a0, a1, a2, 0, 0, 0);
}

static inline void pacha_trace4(uint32_t component, uint32_t event, uint32_t event_class, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    pacha_trace_emit(component, event, event_class, 4, a0, a1, a2, a3, 0, 0);
}

static inline void pacha_trace5(uint32_t component, uint32_t event, uint32_t event_class, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    pacha_trace_emit(component, event, event_class, 5, a0, a1, a2, a3, a4, 0);
}

static inline void pacha_trace6(uint32_t component, uint32_t event, uint32_t event_class, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    pacha_trace_emit(component, event, event_class, 6, a0, a1, a2, a3, a4, a5);
}

#ifdef __cplusplus
}
#endif

#endif
