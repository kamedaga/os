#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pacha/ipc.h"
#include "pacha/trace.h"
typedef struct lpr_exec_map_metric_record {
    const char *name;
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint64_t total_bytes;
} lpr_exec_map_metric_record_t;

enum {
    LPR_EXEC_MAP_METRIC_SLOTS = 64,
};

static lpr_exec_map_metric_record_t lpr_exec_map_metrics[LPR_EXEC_MAP_METRIC_SLOTS];

static uint64_t lpr_exec_map_now_ns(void)
{
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t lpr_exec_map_now_cycles(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static lpr_exec_map_metric_record_t *lpr_exec_map_metric_slot(const char *label)
{
    if (label == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < LPR_EXEC_MAP_METRIC_SLOTS; ++i) {
        if (lpr_exec_map_metrics[i].name == label ||
            (lpr_exec_map_metrics[i].name != NULL && strcmp(lpr_exec_map_metrics[i].name, label) == 0))
        {
            return &lpr_exec_map_metrics[i];
        }
    }
    for (size_t i = 0; i < LPR_EXEC_MAP_METRIC_SLOTS; ++i) {
        if (lpr_exec_map_metrics[i].name == NULL) {
            lpr_exec_map_metrics[i].name = label;
            return &lpr_exec_map_metrics[i];
        }
    }
    return NULL;
}

static void lpr_exec_map_metric_count(const char *label, uint64_t bytes)
{
    lpr_exec_map_metric_record_t *metric = lpr_exec_map_metric_slot(label);
    if (metric == NULL) {
        return;
    }
    metric->count++;
    metric->total_bytes += bytes;
}

static void lpr_exec_map_metric_time(const char *label, uint64_t start_ns, uint64_t end_ns, uint64_t bytes)
{
    if (start_ns == 0 || end_ns < start_ns) {
        return;
    }
    lpr_exec_map_metric_record_t *metric = lpr_exec_map_metric_slot(label);
    if (metric == NULL) {
        return;
    }
    const uint64_t elapsed_ns = end_ns - start_ns;
    metric->count++;
    metric->total_ns += elapsed_ns;
    if (elapsed_ns > metric->max_ns) {
        metric->max_ns = elapsed_ns;
    }
    metric->total_bytes += bytes;
}

static void lpr_exec_map_metric_time_cycles(
    const char *label,
    uint64_t start_cycles,
    uint64_t end_cycles,
    uint64_t bytes)
{
    if (start_cycles == 0 || end_cycles < start_cycles) {
        return;
    }
    lpr_exec_map_metric_record_t *metric = lpr_exec_map_metric_slot(label);
    if (metric == NULL) {
        return;
    }
    const uint64_t elapsed_cycles = end_cycles - start_cycles;
    metric->count++;
    metric->total_cycles += elapsed_cycles;
    if (elapsed_cycles > metric->max_cycles) {
        metric->max_cycles = elapsed_cycles;
    }
    metric->total_bytes += bytes;
}

void lpr_exec_map_dump_metrics(void)
{
    for (size_t i = 0; i < LPR_EXEC_MAP_METRIC_SLOTS; ++i) {
        const lpr_exec_map_metric_record_t *metric = &lpr_exec_map_metrics[i];
        if (metric->name == NULL || metric->count == 0) {
            continue;
        }
        pacha_trace6(
            PACHA_TRACE_COMPONENT_FILED,
            PACHA_TRACE_EVENT_FILED_EXEC_METRIC,
            PACHA_TRACE_CLASS_METRIC,
            pacha_trace_name_id(metric->name),
            metric->count,
            metric->total_ns / metric->count,
            metric->max_ns,
            metric->total_cycles / metric->count,
            metric->max_cycles);
        pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_EXEC_METRIC, PACHA_TRACE_CLASS_METRIC, pacha_trace_name_id(metric->name), metric->total_bytes);
    }
}

static int fill_vmo(int vmo_fd, const void *initial_bytes, uint64_t initial_size, uint64_t map_size)
{
    if (initial_size == 0) {
        return 0;
    }
    if (initial_bytes == NULL || initial_size > map_size) {
        return -22;
    }
    unsigned char *mapped = pacha_mmap(vmo_fd, map_size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped == NULL) {
        return -12;
    }
    memcpy(mapped, initial_bytes, (size_t)initial_size);
    (void)pacha_munmap(mapped, map_size);
    return 0;
}

static int create_segment_vmo(uint64_t map_size, int *out_vmo_fd, unsigned char **out_mapped)
{
    if (out_vmo_fd == NULL || out_mapped == NULL || map_size == 0 || map_size > SIZE_MAX) {
        return -22;
    }
    *out_vmo_fd = -1;
    *out_mapped = NULL;

    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC;
    uint64_t metric_start = lpr_exec_map_now_cycles();
    const int vmo_fd = pacha_vmo_create(map_size, rights, 0);
    lpr_exec_map_metric_time_cycles(
        "segment_vmo_create_syscall",
        metric_start,
        lpr_exec_map_now_cycles(),
        map_size);
    if (vmo_fd < 16) {
        return -12;
    }
    metric_start = lpr_exec_map_now_cycles();
    unsigned char *mapped = pacha_mmap(
        vmo_fd,
        map_size,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    lpr_exec_map_metric_time_cycles(
        "segment_vmo_mmap_syscall",
        metric_start,
        lpr_exec_map_now_cycles(),
        map_size);
    if (mapped == NULL) {
        (void)pacha_fd_close(vmo_fd);
        return -12;
    }

    *out_vmo_fd = vmo_fd;
    *out_mapped = mapped;
    return 0;
}

static int build_zpoline_page(unsigned char *page, uint64_t handler_va)
{
    if (page == NULL || handler_va == 0) {
        return -22;
    }
    memset(page, LPR_ZPOLINE_NOP_BYTE, LPR_ZPOLINE_PAGE_SIZE);
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_POP_RCX_OFFSET] =
        LPR_ZPOLINE_SHIM_POP_RCX_BYTE;
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_MOVABS_R11_OFFSET] =
        LPR_ZPOLINE_SHIM_MOVABS_R11_BYTE0;
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_MOVABS_R11_OFFSET + 1u] =
        LPR_ZPOLINE_SHIM_MOVABS_R11_BYTE1;
    lpr_exec_wr64(
        page + LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_HANDLER_VA_OFFSET,
        handler_va);
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_JMP_R11_OFFSET] =
        LPR_ZPOLINE_SHIM_JMP_R11_BYTE0;
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_JMP_R11_OFFSET + 1u] =
        LPR_ZPOLINE_SHIM_JMP_R11_BYTE1;
    page[LPR_ZPOLINE_SHIM_OFFSET + LPR_ZPOLINE_SHIM_JMP_R11_OFFSET + 2u] =
        LPR_ZPOLINE_SHIM_JMP_R11_BYTE2;
    return 0;
}

static int zpoline_page_vmo(uint64_t handler_va)
{
    static int cached_vmo_fd = -1;
    static uint64_t cached_handler_va = 0;

    if (handler_va == 0) {
        return -22;
    }
    if (cached_vmo_fd >= 16 && cached_handler_va == handler_va) {
        return cached_vmo_fd;
    }
    if (cached_vmo_fd >= 16) {
        (void)pacha_fd_close(cached_vmo_fd);
        cached_vmo_fd = -1;
        cached_handler_va = 0;
    }

    unsigned char zpoline[LPR_ZPOLINE_PAGE_SIZE];
    int status = build_zpoline_page(zpoline, handler_va);
    if (status != 0) {
        return status;
    }

    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC;
    const int vmo_fd = pacha_vmo_create(LPR_ZPOLINE_PAGE_SIZE, rights, 0);
    if (vmo_fd < 16) {
        return -12;
    }
    status = fill_vmo(vmo_fd, zpoline, sizeof(zpoline), LPR_ZPOLINE_PAGE_SIZE);
    if (status != 0) {
        (void)pacha_fd_close(vmo_fd);
        return status;
    }
    cached_vmo_fd = vmo_fd;
    cached_handler_va = handler_va;
    return cached_vmo_fd;
}

int lpr_exec_install_low_layout(int process_fd, uint64_t syscall_entry_va)
{
    const int vmo_fd = zpoline_page_vmo(syscall_entry_va);
    if (vmo_fd < 16) {
        return vmo_fd;
    }
    const long map_result = pacha_process_map(
        process_fd,
        vmo_fd,
        LPR_ZPOLINE_PAGE_VA,
        LPR_ZPOLINE_PAGE_SIZE,
        PACHA_PROT_READ | PACHA_PROT_EXEC,
        0);
    return (uint64_t)map_result == LPR_ZPOLINE_PAGE_VA ? 0 : -12;
}

static int range_overlaps(uint64_t a, uint64_t asz, uint64_t b, uint64_t bsz)
{
    if (asz == 0 || bsz == 0 || a > UINT64_MAX - asz || b > UINT64_MAX - bsz) {
        return 1;
    }
    return a < b + bsz && b < a + asz;
}

static int load_segment_bounds(
    const unsigned char *ph,
    uint64_t load_bias,
    uint64_t *out_target_va,
    uint64_t *out_page_offset,
    uint64_t *out_map_size)
{
    const uint64_t p_vaddr = lpr_exec_rd64(ph + 16);
    const uint64_t p_memsz = lpr_exec_rd64(ph + 40);
    if (out_target_va == NULL || out_page_offset == NULL || out_map_size == NULL ||
        p_vaddr > UINT64_MAX - load_bias)
    {
        return -8;
    }
    if (p_memsz == 0) {
        *out_target_va = 0;
        *out_page_offset = 0;
        *out_map_size = 0;
        return 0;
    }
    const uint64_t target_va = lpr_exec_align_down(p_vaddr + load_bias);
    const uint64_t page_offset = (p_vaddr + load_bias) - target_va;
    if (p_memsz > UINT64_MAX - page_offset) {
        return -75;
    }
    uint64_t map_size = 0;
    int status = lpr_exec_align_up(page_offset + p_memsz, &map_size);
    if (status != 0) {
        return status;
    }
    if (map_size > SIZE_MAX ||
        range_overlaps(target_va, map_size, LPR_ZPOLINE_PAGE_VA, LPR_LOW_GUARD_END_VA))
    {
        return -8;
    }
    *out_target_va = target_va;
    *out_page_offset = page_offset;
    *out_map_size = map_size;
    return 0;
}

static int load_span_from_phdrs(
    const unsigned char *phdrs,
    uint16_t phnum,
    uint16_t phent,
    uint64_t load_bias,
    uint64_t *out_base,
    uint64_t *out_size,
    uint16_t *out_load_segments)
{
    if (phdrs == NULL || out_base == NULL || out_size == NULL || out_load_segments == NULL) {
        return -22;
    }
    uint64_t span_base = UINT64_MAX;
    uint64_t span_end = 0;
    uint16_t load_segments = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        const unsigned char *ph = phdrs + (uint64_t)i * phent;
        if (lpr_exec_rd32(ph) != LPR_EXEC_PT_LOAD) {
            continue;
        }
        const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
        const uint64_t p_memsz = lpr_exec_rd64(ph + 40);
        if (p_filesz > p_memsz) {
            return -8;
        }
        uint64_t target_va = 0;
        uint64_t page_offset = 0;
        uint64_t map_size = 0;
        int status = load_segment_bounds(ph, load_bias, &target_va, &page_offset, &map_size);
        if (status != 0) {
            return status;
        }
        if (map_size == 0) {
            continue;
        }
        if (target_va < span_base) {
            span_base = target_va;
        }
        if (target_va > UINT64_MAX - map_size) {
            return -75;
        }
        const uint64_t segment_end = target_va + map_size;
        if (segment_end > span_end) {
            span_end = segment_end;
        }
        load_segments++;
    }
    if (load_segments == 0 || span_base == UINT64_MAX || span_end <= span_base) {
        return -8;
    }
    const uint64_t span_size = span_end - span_base;
    if (span_size > SIZE_MAX) {
        return -8;
    }
    *out_base = span_base;
    *out_size = span_size;
    *out_load_segments = load_segments;
    return 0;
}

static int load_file_span_from_phdrs(
    const unsigned char *phdrs,
    uint16_t phnum,
    uint16_t phent,
    uint64_t file_size,
    uint64_t *out_offset,
    uint64_t *out_size)
{
    if (phdrs == NULL || out_offset == NULL || out_size == NULL) {
        return -22;
    }
    uint64_t span_offset = UINT64_MAX;
    uint64_t span_end = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        const unsigned char *ph = phdrs + (uint64_t)i * phent;
        if (lpr_exec_rd32(ph) != LPR_EXEC_PT_LOAD) {
            continue;
        }
        const uint64_t p_offset = lpr_exec_rd64(ph + 8);
        const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
        const uint64_t p_memsz = lpr_exec_rd64(ph + 40);
        if (p_filesz > p_memsz || p_offset > file_size || p_filesz > file_size - p_offset) {
            return -8;
        }
        if (p_filesz == 0) {
            continue;
        }
        if (p_offset < span_offset) {
            span_offset = p_offset;
        }
        if (p_offset > UINT64_MAX - p_filesz) {
            return -75;
        }
        const uint64_t segment_end = p_offset + p_filesz;
        if (segment_end > span_end) {
            span_end = segment_end;
        }
    }
    if (span_offset == UINT64_MAX || span_end <= span_offset) {
        *out_offset = 0;
        *out_size = 0;
        return 0;
    }
    const uint64_t span_size = span_end - span_offset;
    if (span_size > SIZE_MAX) {
        return -8;
    }
    *out_offset = span_offset;
    *out_size = span_size;
    return 0;
}

enum {
    LPR_EXEC_SEGMENT_VMO_CACHE_SLOTS = 24,
    LPR_EXEC_SEGMENT_VMO_CACHE_MAX_BYTES = 8u * 1024u * 1024u,
    LPR_EXEC_MEMORY_SPAN_VMO_CACHE_SLOTS = 4,
    LPR_EXEC_MEMORY_SPAN_VMO_CACHE_MAX_BYTES = 4u * 1024u * 1024u,
    LPR_EXEC_FILE_MAP_PLAN_CACHE_SLOTS = 16,
    LPR_EXEC_FILE_MAP_PLAN_MAX_SEGMENTS = 16,
};

typedef struct lpr_exec_file_map_segment_plan {
    uint64_t file_offset;
    uint64_t file_size;
    uint64_t mem_size;
    uint64_t target_va;
    uint64_t page_offset;
    uint64_t map_size;
    uint64_t patch_file_offset;
    uint64_t patch_file_size;
    uint64_t prot;
    int patch_text;
    int cached_vmo_fd;
} lpr_exec_file_map_segment_plan_t;

typedef struct lpr_exec_file_map_plan_cache_slot {
    bool valid;
    uint64_t backend_object;
    uint64_t object_generation;
    uint64_t file_size;
    uint64_t load_bias;
    int patch_text;
    uint64_t span_base;
    uint64_t span_size;
    uint64_t file_span_offset;
    uint64_t file_span_size;
    uint16_t load_segments;
    uint16_t segment_count;
    int has_page_overlap;
    uint64_t last_used;
    lpr_exec_file_map_segment_plan_t segments[LPR_EXEC_FILE_MAP_PLAN_MAX_SEGMENTS];
} lpr_exec_file_map_plan_cache_slot_t;

typedef struct lpr_exec_segment_vmo_cache_slot {
    bool valid;
    uint64_t backend_object;
    uint64_t object_generation;
    uint64_t file_offset;
    uint64_t file_size;
    uint64_t map_size;
    uint64_t page_offset;
    int patch_text;
    uint64_t last_used;
    int vmo_fd;
} lpr_exec_segment_vmo_cache_slot_t;

static lpr_exec_segment_vmo_cache_slot_t lpr_exec_segment_vmo_cache[LPR_EXEC_SEGMENT_VMO_CACHE_SLOTS];
static uint64_t lpr_exec_segment_vmo_cache_clock;
static uint64_t lpr_exec_segment_vmo_cache_bytes;
static lpr_exec_file_map_plan_cache_slot_t lpr_exec_file_map_plan_cache[LPR_EXEC_FILE_MAP_PLAN_CACHE_SLOTS];
static uint64_t lpr_exec_file_map_plan_cache_clock;

static void lpr_exec_segment_vmo_cache_clear_slot(lpr_exec_segment_vmo_cache_slot_t *slot);

typedef struct lpr_exec_memory_span_vmo_cache_slot {
    bool valid;
    uint64_t backend_object;
    uint64_t object_generation;
    uint64_t image_size;
    uint64_t load_bias;
    uint64_t span_base;
    uint64_t span_size;
    int patch_text;
    uint64_t last_used;
    int vmo_fd;
} lpr_exec_memory_span_vmo_cache_slot_t;

static lpr_exec_memory_span_vmo_cache_slot_t lpr_exec_memory_span_vmo_cache[LPR_EXEC_MEMORY_SPAN_VMO_CACHE_SLOTS];
static uint64_t lpr_exec_memory_span_vmo_cache_clock;
static uint64_t lpr_exec_memory_span_vmo_cache_bytes;

static int lpr_exec_text_file_intersection(
    uint64_t segment_file_offset,
    uint64_t segment_file_size,
    uint64_t text_offset,
    uint64_t text_size,
    uint64_t *out_offset,
    uint64_t *out_size)
{
    if (out_offset == NULL || out_size == NULL) {
        return 0;
    }
    *out_offset = 0;
    *out_size = 0;
    if (segment_file_size == 0 || text_size == 0 ||
        segment_file_offset > UINT64_MAX - segment_file_size ||
        text_offset > UINT64_MAX - text_size)
    {
        return 0;
    }
    const uint64_t segment_end = segment_file_offset + segment_file_size;
    const uint64_t text_end = text_offset + text_size;
    const uint64_t start = segment_file_offset > text_offset ? segment_file_offset : text_offset;
    const uint64_t end = segment_end < text_end ? segment_end : text_end;
    if (start >= end) {
        return 0;
    }
    *out_offset = start;
    *out_size = end - start;
    return 1;
}

static void lpr_exec_patch_segment_text_range(
    unsigned char *mapped,
    uint64_t map_size,
    uint64_t segment_file_offset,
    uint64_t page_offset,
    uint64_t patch_file_offset,
    uint64_t patch_file_size)
{
    if (mapped == NULL || patch_file_size == 0 || patch_file_offset < segment_file_offset) {
        return;
    }
    const uint64_t file_delta = patch_file_offset - segment_file_offset;
    if (page_offset > map_size || file_delta > map_size - page_offset) {
        return;
    }
    const uint64_t mapped_offset = page_offset + file_delta;
    if (patch_file_size > map_size - mapped_offset) {
        return;
    }
    const uint64_t patch_start_cycles = lpr_exec_map_now_cycles();
    const uint64_t patched = lpr_exec_patch_syscalls(mapped + mapped_offset, patch_file_size);
    lpr_exec_map_metric_time_cycles(
        "patch_syscalls_scan",
        patch_start_cycles,
        lpr_exec_map_now_cycles(),
        patch_file_size);
    if (patched != 0) {
        lpr_exec_map_metric_count("patch_syscalls", patched);
    }
}

static void lpr_exec_memory_span_vmo_cache_clear_slot(lpr_exec_memory_span_vmo_cache_slot_t *slot);
static int phdr_va_from_phdrs(
    const unsigned char *phdrs,
    uint16_t phnum,
    uint16_t phent,
    uint64_t fallback,
    uint64_t *out_phdr_va);
static int load_bias_for_type(uint16_t type, uint64_t dyn_base, uint64_t *out_load_bias);
static int set_loaded_result(
    lpr_exec_loaded_t *loaded,
    uint64_t entry,
    uint64_t load_bias,
    uint64_t phdr_va,
    uint16_t phent,
    uint16_t phnum,
    uint16_t load_segments);
static int validate_loaded_meta(uint16_t type, uint16_t phent, uint16_t phnum);

void lpr_exec_invalidate_segment_vmo_cache(uint64_t backend_object)
{
    if (backend_object == 0) {
        return;
    }
    for (uint64_t i = 0; i < LPR_EXEC_SEGMENT_VMO_CACHE_SLOTS; ++i) {
        lpr_exec_segment_vmo_cache_slot_t *slot = &lpr_exec_segment_vmo_cache[i];
        if (slot->valid && slot->backend_object == backend_object) {
            lpr_exec_segment_vmo_cache_clear_slot(slot);
        }
    }
    for (uint64_t i = 0; i < LPR_EXEC_MEMORY_SPAN_VMO_CACHE_SLOTS; ++i) {
        lpr_exec_memory_span_vmo_cache_slot_t *slot = &lpr_exec_memory_span_vmo_cache[i];
        if (slot->valid && slot->backend_object == backend_object) {
            lpr_exec_memory_span_vmo_cache_clear_slot(slot);
        }
    }
    for (uint64_t i = 0; i < LPR_EXEC_FILE_MAP_PLAN_CACHE_SLOTS; ++i) {
        lpr_exec_file_map_plan_cache_slot_t *slot = &lpr_exec_file_map_plan_cache[i];
        if (slot->valid && slot->backend_object == backend_object) {
            memset(slot, 0, sizeof(*slot));
        }
    }
}

static uint64_t lpr_exec_segment_vmo_cache_next_clock(void)
{
    if (lpr_exec_segment_vmo_cache_clock == UINT64_MAX) {
        lpr_exec_segment_vmo_cache_clock = 0;
    }
    ++lpr_exec_segment_vmo_cache_clock;
    if (lpr_exec_segment_vmo_cache_clock == 0) {
        lpr_exec_segment_vmo_cache_clock = 1;
    }
    return lpr_exec_segment_vmo_cache_clock;
}

static uint64_t lpr_exec_file_map_plan_cache_next_clock(void)
{
    if (lpr_exec_file_map_plan_cache_clock == UINT64_MAX) {
        lpr_exec_file_map_plan_cache_clock = 0;
    }
    ++lpr_exec_file_map_plan_cache_clock;
    if (lpr_exec_file_map_plan_cache_clock == 0) {
        lpr_exec_file_map_plan_cache_clock = 1;
    }
    return lpr_exec_file_map_plan_cache_clock;
}

static void lpr_exec_segment_vmo_cache_clear_slot(lpr_exec_segment_vmo_cache_slot_t *slot)
{
    if (slot == NULL || !slot->valid) {
        return;
    }
    for (uint64_t i = 0; i < LPR_EXEC_FILE_MAP_PLAN_CACHE_SLOTS; ++i) {
        lpr_exec_file_map_plan_cache_slot_t *plan = &lpr_exec_file_map_plan_cache[i];
        if (!plan->valid ||
            plan->backend_object != slot->backend_object ||
            plan->object_generation != slot->object_generation)
        {
            continue;
        }
        for (uint16_t j = 0; j < plan->segment_count; ++j) {
            lpr_exec_file_map_segment_plan_t *segment = &plan->segments[j];
            if (segment->file_offset == slot->file_offset &&
                segment->file_size == slot->file_size &&
                segment->map_size == slot->map_size &&
                segment->page_offset == slot->page_offset &&
                segment->patch_text == slot->patch_text &&
                segment->cached_vmo_fd == slot->vmo_fd)
            {
                segment->cached_vmo_fd = -1;
            }
        }
    }
    if (slot->vmo_fd >= 16) {
        (void)pacha_fd_close(slot->vmo_fd);
    }
    if (lpr_exec_segment_vmo_cache_bytes >= slot->map_size) {
        lpr_exec_segment_vmo_cache_bytes -= slot->map_size;
    } else {
        lpr_exec_segment_vmo_cache_bytes = 0;
    }
    memset(slot, 0, sizeof(*slot));
    slot->vmo_fd = -1;
}

static uint64_t lpr_exec_memory_span_vmo_cache_next_clock(void)
{
    if (lpr_exec_memory_span_vmo_cache_clock == UINT64_MAX) {
        lpr_exec_memory_span_vmo_cache_clock = 0;
    }
    ++lpr_exec_memory_span_vmo_cache_clock;
    if (lpr_exec_memory_span_vmo_cache_clock == 0) {
        lpr_exec_memory_span_vmo_cache_clock = 1;
    }
    return lpr_exec_memory_span_vmo_cache_clock;
}

static void lpr_exec_memory_span_vmo_cache_clear_slot(lpr_exec_memory_span_vmo_cache_slot_t *slot)
{
    if (slot == NULL || !slot->valid) {
        return;
    }
    if (slot->vmo_fd >= 16) {
        (void)pacha_fd_close(slot->vmo_fd);
    }
    if (lpr_exec_memory_span_vmo_cache_bytes >= slot->span_size) {
        lpr_exec_memory_span_vmo_cache_bytes -= slot->span_size;
    } else {
        lpr_exec_memory_span_vmo_cache_bytes = 0;
    }
    memset(slot, 0, sizeof(*slot));
    slot->vmo_fd = -1;
}

static lpr_exec_memory_span_vmo_cache_slot_t *lpr_exec_memory_span_vmo_cache_find(
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t image_size,
    uint64_t load_bias,
    uint64_t span_base,
    uint64_t span_size,
    int patch_text)
{
    if (backend_object == 0 || span_size == 0) {
        return NULL;
    }
    for (uint64_t i = 0; i < LPR_EXEC_MEMORY_SPAN_VMO_CACHE_SLOTS; ++i) {
        lpr_exec_memory_span_vmo_cache_slot_t *slot = &lpr_exec_memory_span_vmo_cache[i];
        if (slot->valid &&
            slot->backend_object == backend_object &&
            slot->object_generation == object_generation &&
            slot->image_size == image_size &&
            slot->load_bias == load_bias &&
            slot->span_base == span_base &&
            slot->span_size == span_size &&
            slot->patch_text == patch_text)
        {
            return slot;
        }
    }
    return NULL;
}

static lpr_exec_memory_span_vmo_cache_slot_t *lpr_exec_memory_span_vmo_cache_choose_slot(void)
{
    lpr_exec_memory_span_vmo_cache_slot_t *oldest = NULL;
    for (uint64_t i = 0; i < LPR_EXEC_MEMORY_SPAN_VMO_CACHE_SLOTS; ++i) {
        lpr_exec_memory_span_vmo_cache_slot_t *slot = &lpr_exec_memory_span_vmo_cache[i];
        if (!slot->valid) {
            return slot;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    return oldest;
}

static int map_memory_image_span_vmo(
    int process_fd,
    int vmo_fd,
    uint64_t span_base,
    const unsigned char *phdrs,
    uint16_t phnum,
    uint16_t phent,
    uint64_t load_bias,
    const struct pacha_process_map_batch_entry *extra_entries,
    uint64_t extra_count)
{
    struct pacha_process_map_batch_entry entries[PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES];
    uint64_t entry_count = 0;
    if (extra_count > PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES ||
        (extra_count != 0 && extra_entries == NULL))
    {
        return -22;
    }
    for (uint64_t i = 0; i < extra_count; ++i) {
        entries[entry_count++] = extra_entries[i];
    }
    for (uint16_t i = 0; i < phnum; ++i) {
        const unsigned char *ph = phdrs + (uint64_t)i * phent;
        if (lpr_exec_rd32(ph) != LPR_EXEC_PT_LOAD) {
            continue;
        }
        uint64_t target_va = 0;
        uint64_t page_offset = 0;
        uint64_t map_size = 0;
        int status = load_segment_bounds(ph, load_bias, &target_va, &page_offset, &map_size);
        (void)page_offset;
        if (status != 0) {
            return status;
        }
        if (map_size == 0) {
            continue;
        }
        if (target_va < span_base || entry_count >= PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES) {
            return -8;
        }
        entries[entry_count++] = (struct pacha_process_map_batch_entry){
            .vmo_fd = (uint64_t)(uint32_t)vmo_fd,
            .target_va = target_va,
            .size = map_size,
            .prot = lpr_exec_prot_from_elf_flags(lpr_exec_rd32(ph + 4)),
            .vmo_offset = target_va - span_base,
            .flags = PACHA_PROCESS_MAP_PRIVATE,
        };
    }
    if (entry_count == 0) {
        return 0;
    }
    const uint64_t map_start_ns = lpr_exec_map_now_ns();
    const int status = pacha_process_map_batch(process_fd, entries, entry_count);
    lpr_exec_map_metric_time("map_batch", map_start_ns, lpr_exec_map_now_ns(), entry_count);
    return status;
}

static int memory_image_span_vmo_cacheable(
    const lpr_exec_image_t *image,
    const unsigned char *phdrs,
    uint16_t phnum,
    uint16_t phent,
    uint64_t span_size)
{
    (void)phdrs;
    (void)phnum;
    (void)phent;
    if (image == NULL || image->backend_object == 0 || span_size == 0 ||
        span_size > LPR_EXEC_MEMORY_SPAN_VMO_CACHE_MAX_BYTES)
    {
        return 0;
    }
    return 1;
}

static int load_segments_have_page_overlap(
    const unsigned char *phdrs,
    uint16_t phnum,
    uint16_t phent,
    uint64_t load_bias,
    int *out_overlap)
{
    if (phdrs == NULL || out_overlap == NULL) {
        return -22;
    }
    *out_overlap = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        const unsigned char *a = phdrs + (uint64_t)i * phent;
        if (lpr_exec_rd32(a) != LPR_EXEC_PT_LOAD) {
            continue;
        }
        uint64_t a_va = 0;
        uint64_t a_page_offset = 0;
        uint64_t a_size = 0;
        int status = load_segment_bounds(a, load_bias, &a_va, &a_page_offset, &a_size);
        (void)a_page_offset;
        if (status != 0) {
            return status;
        }
        if (a_size == 0) {
            continue;
        }
        for (uint16_t j = (uint16_t)(i + 1u); j < phnum; ++j) {
            const unsigned char *b = phdrs + (uint64_t)j * phent;
            if (lpr_exec_rd32(b) != LPR_EXEC_PT_LOAD) {
                continue;
            }
            uint64_t b_va = 0;
            uint64_t b_page_offset = 0;
            uint64_t b_size = 0;
            status = load_segment_bounds(b, load_bias, &b_va, &b_page_offset, &b_size);
            (void)b_page_offset;
            if (status != 0) {
                return status;
            }
            if (b_size != 0 && range_overlaps(a_va, a_size, b_va, b_size)) {
                *out_overlap = 1;
                return 0;
            }
        }
    }
    return 0;
}

static int file_span_vmo_cacheable(const lpr_exec_file_t *file, uint64_t span_size)
{
    if (file == NULL || file->backend_object == 0 || span_size == 0 ||
        span_size > LPR_EXEC_MEMORY_SPAN_VMO_CACHE_MAX_BYTES)
    {
        return 0;
    }
    return 1;
}

static lpr_exec_segment_vmo_cache_slot_t *lpr_exec_segment_vmo_cache_find(
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t file_offset,
    uint64_t file_size,
    uint64_t map_size,
    uint64_t page_offset,
    int patch_text)
{
    if (backend_object == 0 || map_size == 0) {
        return NULL;
    }
    for (uint64_t i = 0; i < LPR_EXEC_SEGMENT_VMO_CACHE_SLOTS; ++i) {
        lpr_exec_segment_vmo_cache_slot_t *slot = &lpr_exec_segment_vmo_cache[i];
        if (slot->valid &&
            slot->backend_object == backend_object &&
            slot->object_generation == object_generation &&
            slot->file_offset == file_offset &&
            slot->file_size == file_size &&
            slot->map_size == map_size &&
            slot->page_offset == page_offset &&
            slot->patch_text == patch_text)
        {
            return slot;
        }
    }
    return NULL;
}

static lpr_exec_segment_vmo_cache_slot_t *lpr_exec_segment_vmo_cache_choose_slot(void)
{
    lpr_exec_segment_vmo_cache_slot_t *oldest = NULL;
    for (uint64_t i = 0; i < LPR_EXEC_SEGMENT_VMO_CACHE_SLOTS; ++i) {
        lpr_exec_segment_vmo_cache_slot_t *slot = &lpr_exec_segment_vmo_cache[i];
        if (!slot->valid) {
            return slot;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    return oldest;
}

static lpr_exec_file_map_plan_cache_slot_t *lpr_exec_file_map_plan_cache_find(
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t file_size,
    uint64_t load_bias,
    int patch_text)
{
    if (backend_object == 0) {
        return NULL;
    }
    for (uint64_t i = 0; i < LPR_EXEC_FILE_MAP_PLAN_CACHE_SLOTS; ++i) {
        lpr_exec_file_map_plan_cache_slot_t *slot = &lpr_exec_file_map_plan_cache[i];
        if (slot->valid &&
            slot->backend_object == backend_object &&
            slot->object_generation == object_generation &&
            slot->file_size == file_size &&
            slot->load_bias == load_bias &&
            slot->patch_text == patch_text)
        {
            return slot;
        }
    }
    return NULL;
}

static lpr_exec_file_map_plan_cache_slot_t *lpr_exec_file_map_plan_cache_choose_slot(void)
{
    lpr_exec_file_map_plan_cache_slot_t *oldest = NULL;
    for (uint64_t i = 0; i < LPR_EXEC_FILE_MAP_PLAN_CACHE_SLOTS; ++i) {
        lpr_exec_file_map_plan_cache_slot_t *slot = &lpr_exec_file_map_plan_cache[i];
        if (!slot->valid) {
            return slot;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    return oldest;
}

static int build_file_map_plan(
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta,
    uint64_t load_bias,
    int patch_text,
    lpr_exec_file_map_plan_cache_slot_t *plan)
{
    if (file == NULL || meta == NULL || meta->phdrs == NULL || plan == NULL) {
        return -22;
    }
    memset(plan, 0, sizeof(*plan));
    plan->backend_object = file->backend_object;
    plan->object_generation = file->object_generation;
    plan->file_size = file->size;
    plan->load_bias = load_bias;
    plan->patch_text = patch_text;
    plan->file_span_offset = UINT64_MAX;

    uint64_t span_end = 0;
    uint64_t file_span_end = 0;
    for (uint16_t i = 0; i < meta->phnum; ++i) {
        const unsigned char *ph = meta->phdrs + (uint64_t)i * meta->phent;
        if (lpr_exec_rd32(ph) != LPR_EXEC_PT_LOAD) {
            continue;
        }
        const uint32_t p_flags = lpr_exec_rd32(ph + 4);
        const uint64_t p_offset = lpr_exec_rd64(ph + 8);
        const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
        const uint64_t p_memsz = lpr_exec_rd64(ph + 40);
        if (p_filesz > p_memsz || p_offset > file->size || p_filesz > file->size - p_offset) {
            return -8;
        }

        uint64_t target_va = 0;
        uint64_t page_offset = 0;
        uint64_t map_size = 0;
        int status = load_segment_bounds(ph, load_bias, &target_va, &page_offset, &map_size);
        if (status != 0) {
            return status;
        }
        if (map_size == 0) {
            continue;
        }
        if (plan->segment_count >= LPR_EXEC_FILE_MAP_PLAN_MAX_SEGMENTS) {
            return -95;
        }
        if (plan->load_segments == 0 || target_va < plan->span_base) {
            plan->span_base = target_va;
        }
        if (target_va > UINT64_MAX - map_size) {
            return -75;
        }
        const uint64_t segment_end = target_va + map_size;
        if (segment_end > span_end) {
            span_end = segment_end;
        }
        if (p_filesz != 0) {
            if (p_offset < plan->file_span_offset) {
                plan->file_span_offset = p_offset;
            }
            if (p_offset > UINT64_MAX - p_filesz) {
                return -75;
            }
            const uint64_t file_segment_end = p_offset + p_filesz;
            if (file_segment_end > file_span_end) {
                file_span_end = file_segment_end;
            }
        }

        uint64_t patch_file_offset = 0;
        uint64_t patch_file_size = 0;
        const int patch_segment_text =
            patch_text &&
            (p_flags & LPR_EXEC_PF_X) != 0 &&
            lpr_exec_text_file_intersection(
                p_offset,
                p_filesz,
                meta->text_offset,
                meta->text_size,
                &patch_file_offset,
                &patch_file_size);

        plan->segments[plan->segment_count++] = (lpr_exec_file_map_segment_plan_t){
            .file_offset = p_offset,
            .file_size = p_filesz,
            .mem_size = p_memsz,
            .target_va = target_va,
            .page_offset = page_offset,
            .map_size = map_size,
            .patch_file_offset = patch_file_offset,
            .patch_file_size = patch_file_size,
            .prot = lpr_exec_prot_from_elf_flags(p_flags),
            .patch_text = patch_segment_text,
            .cached_vmo_fd = -1,
        };
        plan->load_segments++;
    }
    if (plan->load_segments == 0 || span_end <= plan->span_base) {
        return -8;
    }
    plan->span_size = span_end - plan->span_base;
    if (plan->span_size > SIZE_MAX) {
        return -8;
    }
    if (plan->file_span_offset == UINT64_MAX || file_span_end <= plan->file_span_offset) {
        plan->file_span_offset = 0;
        plan->file_span_size = 0;
    } else {
        plan->file_span_size = file_span_end - plan->file_span_offset;
        if (plan->file_span_size > SIZE_MAX) {
            return -8;
        }
    }

    for (uint16_t i = 0; i < plan->segment_count; ++i) {
        const lpr_exec_file_map_segment_plan_t *a = &plan->segments[i];
        for (uint16_t j = (uint16_t)(i + 1u); j < plan->segment_count; ++j) {
            const lpr_exec_file_map_segment_plan_t *b = &plan->segments[j];
            if (range_overlaps(a->target_va, a->map_size, b->target_va, b->map_size)) {
                plan->has_page_overlap = 1;
                return 0;
            }
        }
    }
    return 0;
}

static int get_file_map_plan(
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta,
    uint64_t load_bias,
    int patch_text,
    lpr_exec_file_map_plan_cache_slot_t **out_plan)
{
    if (file == NULL || meta == NULL || out_plan == NULL) {
        return -22;
    }
    *out_plan = NULL;
    lpr_exec_file_map_plan_cache_slot_t *slot = lpr_exec_file_map_plan_cache_find(
        file->backend_object,
        file->object_generation,
        file->size,
        load_bias,
        patch_text);
    if (slot != NULL) {
        slot->last_used = lpr_exec_file_map_plan_cache_next_clock();
        lpr_exec_map_metric_count("file_map_plan_hit", slot->segment_count);
        *out_plan = slot;
        return 0;
    }

    lpr_exec_map_metric_count("file_map_plan_miss", 0);
    static lpr_exec_file_map_plan_cache_slot_t uncached_plan;
    slot = lpr_exec_file_map_plan_cache_choose_slot();
    lpr_exec_file_map_plan_cache_slot_t *target = slot == NULL ? &uncached_plan : slot;
    int status = build_file_map_plan(file, meta, load_bias, patch_text, target);
    if (status != 0) {
        return status;
    }
    if (slot != NULL && file->backend_object != 0) {
        target->valid = true;
        target->last_used = lpr_exec_file_map_plan_cache_next_clock();
        *out_plan = target;
    } else {
        uncached_plan.valid = false;
        *out_plan = &uncached_plan;
    }
    return 0;
}

static int map_file_segment_batch(
    int process_fd,
    struct pacha_process_map_batch_entry *entries,
    int *close_fds,
    uint64_t *entry_count)
{
    if (entries == NULL || close_fds == NULL || entry_count == NULL) {
        return -22;
    }
    if (*entry_count == 0) {
        return 0;
    }
    const uint64_t map_start_ns = lpr_exec_map_now_ns();
    const int status = pacha_process_map_batch(process_fd, entries, *entry_count);
    lpr_exec_map_metric_time("segment_map_batch", map_start_ns, lpr_exec_map_now_ns(), *entry_count);
    for (uint64_t i = 0; i < *entry_count; ++i) {
        if (close_fds[i] >= 16) {
            (void)pacha_fd_close(close_fds[i]);
            close_fds[i] = -1;
        }
    }
    *entry_count = 0;
    return status;
}

void lpr_exec_pending_map_batch_init(lpr_exec_pending_map_batch_t *batch)
{
    if (batch == NULL) {
        return;
    }
    memset(batch, 0, sizeof(*batch));
    for (uint64_t i = 0; i < PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES; ++i) {
        batch->close_fds[i] = -1;
    }
}

void lpr_exec_pending_map_batch_discard(lpr_exec_pending_map_batch_t *batch)
{
    if (batch == NULL) {
        return;
    }
    for (uint64_t i = 0; i < batch->count && i < PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES; ++i) {
        if (batch->close_fds[i] >= 16) {
            (void)pacha_fd_close(batch->close_fds[i]);
            batch->close_fds[i] = -1;
        }
    }
    batch->count = 0;
}

int lpr_exec_pending_map_batch_commit(int process_fd, lpr_exec_pending_map_batch_t *batch)
{
    if (batch == NULL || process_fd < 16) {
        return -22;
    }
    return map_file_segment_batch(process_fd, batch->entries, batch->close_fds, &batch->count);
}

static int create_segment_vmo_from_bytes(
    const unsigned char *bytes,
    uint64_t bytes_size,
    uint64_t file_offset,
    uint64_t page_offset,
    uint64_t map_size,
    int patch_text,
    uint64_t patch_file_offset,
    uint64_t patch_file_size,
    int *out_vmo_fd)
{
    if ((bytes == NULL && bytes_size != 0) ||
        out_vmo_fd == NULL ||
        bytes_size > SIZE_MAX ||
        map_size == 0 ||
        map_size > SIZE_MAX ||
        page_offset > map_size || bytes_size > map_size - page_offset)
    {
        return -22;
    }
    *out_vmo_fd = -1;
    int vmo_fd = -1;
    unsigned char *mapped = NULL;
    uint64_t metric_start = lpr_exec_map_now_cycles();
    int status = create_segment_vmo(map_size, &vmo_fd, &mapped);
    lpr_exec_map_metric_time_cycles(
        "segment_vmo_create_map",
        metric_start,
        lpr_exec_map_now_cycles(),
        map_size);
    if (status != 0) {
        return status;
    }
    if (page_offset != 0) {
        metric_start = lpr_exec_map_now_cycles();
        memset(mapped, 0, (size_t)page_offset);
        lpr_exec_map_metric_time_cycles(
            "segment_vmo_zero_prefix",
            metric_start,
            lpr_exec_map_now_cycles(),
            page_offset);
    }
    if (bytes_size != 0) {
        metric_start = lpr_exec_map_now_cycles();
        memcpy(mapped + page_offset, bytes, (size_t)bytes_size);
        lpr_exec_map_metric_time_cycles(
            "segment_vmo_copy_file",
            metric_start,
            lpr_exec_map_now_cycles(),
            bytes_size);
    }
    const uint64_t data_end = page_offset + bytes_size;
    if (data_end < map_size) {
        metric_start = lpr_exec_map_now_cycles();
        memset(mapped + data_end, 0, (size_t)(map_size - data_end));
        lpr_exec_map_metric_time_cycles(
            "segment_vmo_zero_suffix",
            metric_start,
            lpr_exec_map_now_cycles(),
            map_size - data_end);
    }
    if (patch_text) {
        lpr_exec_patch_segment_text_range(
            mapped,
            map_size,
            file_offset,
            page_offset,
            patch_file_offset,
            patch_file_size);
    }
    metric_start = lpr_exec_map_now_cycles();
    (void)pacha_munmap(mapped, map_size);
    lpr_exec_map_metric_time_cycles(
        "segment_vmo_munmap_syscall",
        metric_start,
        lpr_exec_map_now_cycles(),
        map_size);
    *out_vmo_fd = vmo_fd;
    return 0;
}

static int readonly_segment_vmo_cacheable_from_plan(
    const lpr_exec_file_t *file,
    const lpr_exec_file_map_segment_plan_t *segment)
{
    if (file == NULL || segment == NULL) {
        return 0;
    }
    if ((segment->prot & PACHA_PROT_WRITE) != 0 ||
        segment->file_size != segment->mem_size ||
        segment->file_size == 0 ||
        file->backend_object == 0)
    {
        return 0;
    }
    return 1;
}

static int cached_readonly_segment_vmo_lookup_from_plan(
    const lpr_exec_file_t *file,
    lpr_exec_file_map_segment_plan_t *segment,
    int record_metric)
{
    if (file == NULL || segment == NULL) {
        return -22;
    }
    if (!readonly_segment_vmo_cacheable_from_plan(file, segment)) {
        return -1;
    }
    if (segment->cached_vmo_fd >= 16) {
        if (record_metric) {
            lpr_exec_map_metric_count("segment_vmo_plan_hit", segment->map_size);
        }
        return segment->cached_vmo_fd;
    }
    lpr_exec_segment_vmo_cache_slot_t *slot = lpr_exec_segment_vmo_cache_find(
        file->backend_object,
        file->object_generation,
        segment->file_offset,
        segment->file_size,
        segment->map_size,
        segment->page_offset,
        segment->patch_text);
    if (slot != NULL && slot->vmo_fd >= 16) {
        slot->last_used = lpr_exec_segment_vmo_cache_next_clock();
        segment->cached_vmo_fd = slot->vmo_fd;
        if (record_metric) {
            lpr_exec_map_metric_count("segment_vmo_hit", segment->map_size);
        }
        return slot->vmo_fd;
    }
    return -1;
}

static int cached_readonly_segment_vmo_from_plan(
    const lpr_exec_file_t *file,
    lpr_exec_file_map_segment_plan_t *segment,
    const unsigned char *source)
{
    if (file == NULL || segment == NULL) {
        return -22;
    }
    if (!readonly_segment_vmo_cacheable_from_plan(file, segment) || source == NULL) {
        return -1;
    }
    int cached_vmo_fd = cached_readonly_segment_vmo_lookup_from_plan(file, segment, 1);
    if (cached_vmo_fd >= 16) {
        return cached_vmo_fd;
    }
    lpr_exec_map_metric_count("segment_vmo_miss", segment->map_size);

    while (lpr_exec_segment_vmo_cache_bytes + segment->map_size > LPR_EXEC_SEGMENT_VMO_CACHE_MAX_BYTES) {
        lpr_exec_segment_vmo_cache_slot_t *victim = lpr_exec_segment_vmo_cache_choose_slot();
        if (victim == NULL || !victim->valid) {
            break;
        }
        lpr_exec_segment_vmo_cache_clear_slot(victim);
    }
    lpr_exec_segment_vmo_cache_slot_t *slot = lpr_exec_segment_vmo_cache_choose_slot();
    if (slot == NULL) {
        return -1;
    }
    if (slot->valid) {
        lpr_exec_segment_vmo_cache_clear_slot(slot);
    }

    int vmo_fd = -1;
    int status = create_segment_vmo_from_bytes(
        source,
        segment->file_size,
        segment->file_offset,
        segment->page_offset,
        segment->map_size,
        segment->patch_text,
        segment->patch_file_offset,
        segment->patch_file_size,
        &vmo_fd);
    if (status != 0) {
        return status;
    }
    slot->valid = true;
    slot->backend_object = file->backend_object;
    slot->object_generation = file->object_generation;
    slot->file_offset = segment->file_offset;
    slot->file_size = segment->file_size;
    slot->map_size = segment->map_size;
    slot->page_offset = segment->page_offset;
    slot->patch_text = segment->patch_text;
    slot->last_used = lpr_exec_segment_vmo_cache_next_clock();
    slot->vmo_fd = vmo_fd;
    segment->cached_vmo_fd = vmo_fd;
    lpr_exec_segment_vmo_cache_bytes += segment->map_size;
    lpr_exec_map_metric_count("segment_vmo_store", segment->map_size);
    return vmo_fd;
}

static int file_map_plan_needs_file_span(
    const lpr_exec_file_t *file,
    lpr_exec_file_map_plan_cache_slot_t *plan)
{
    if (file == NULL || plan == NULL) {
        return 1;
    }
    for (uint16_t i = 0; i < plan->segment_count; ++i) {
        lpr_exec_file_map_segment_plan_t *segment = &plan->segments[i];
        if (segment->file_size == 0) {
            continue;
        }
        if (cached_readonly_segment_vmo_lookup_from_plan(file, segment, 0) >= 16) {
            continue;
        }
        return 1;
    }
    return 0;
}

static int file_map_plan_needs_readonly_warm_span(
    const lpr_exec_file_t *file,
    lpr_exec_file_map_plan_cache_slot_t *plan)
{
    if (file == NULL || plan == NULL) {
        return 0;
    }
    for (uint16_t i = 0; i < plan->segment_count; ++i) {
        lpr_exec_file_map_segment_plan_t *segment = &plan->segments[i];
        if (!readonly_segment_vmo_cacheable_from_plan(file, segment)) {
            continue;
        }
        if (cached_readonly_segment_vmo_lookup_from_plan(file, segment, 0) < 16) {
            return 1;
        }
    }
    return 0;
}

static int lpr_exec_load_memory_image_into_process(
    int process_fd,
    const lpr_exec_image_t *image,
    const unsigned char *phdrs,
    uint16_t phnum,
    uint16_t phent,
    uint64_t load_bias,
    int patch_text,
    const struct pacha_process_map_batch_entry *extra_entries,
    uint64_t extra_count,
    uint16_t *out_load_segments)
{
    if (image == NULL || image->bytes == NULL) {
        return -22;
    }
    uint64_t span_base = 0;
    uint64_t span_size = 0;
    uint16_t load_segments = 0;
    int status = load_span_from_phdrs(phdrs, phnum, phent, load_bias, &span_base, &span_size, &load_segments);
    if (status != 0) {
        return status;
    }
    const int cacheable = memory_image_span_vmo_cacheable(image, phdrs, phnum, phent, span_size);
    lpr_exec_memory_span_vmo_cache_slot_t *slot = NULL;
    if (cacheable) {
        slot = lpr_exec_memory_span_vmo_cache_find(
            image->backend_object,
            image->object_generation,
            image->size,
            load_bias,
            span_base,
            span_size,
            patch_text);
        if (slot != NULL && slot->vmo_fd >= 16) {
            slot->last_used = lpr_exec_memory_span_vmo_cache_next_clock();
            lpr_exec_map_metric_count("memory_span_hit", span_size);
            status = map_memory_image_span_vmo(
                process_fd,
                slot->vmo_fd,
                span_base,
                phdrs,
                phnum,
                phent,
                load_bias,
                extra_entries,
                extra_count);
            if (status != 0) {
                return status;
            }
            if (out_load_segments != NULL) {
                *out_load_segments = load_segments;
            }
            return 0;
        }
        lpr_exec_map_metric_count("memory_span_miss", span_size);
    }

    int vmo_fd = -1;
    unsigned char *mapped = NULL;
    status = create_segment_vmo(span_size, &vmo_fd, &mapped);
    if (status != 0) {
        return status;
    }
    uint64_t text_offset = 0;
    uint64_t text_size = 0;
    if (patch_text) {
        status = lpr_exec_image_find_text_section(image, &text_offset, &text_size);
        if (status != 0) {
            (void)pacha_munmap(mapped, span_size);
            (void)pacha_fd_close(vmo_fd);
            return status;
        }
    }
    memset(mapped, 0, (size_t)span_size);
    for (uint16_t i = 0; i < phnum; ++i) {
        const unsigned char *ph = phdrs + (uint64_t)i * phent;
        if (lpr_exec_rd32(ph) != LPR_EXEC_PT_LOAD) {
            continue;
        }
        const uint64_t p_offset = lpr_exec_rd64(ph + 8);
        const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
        const uint64_t p_memsz = lpr_exec_rd64(ph + 40);
        if (p_filesz > p_memsz || p_offset > image->size || p_filesz > image->size - p_offset) {
            status = -8;
            break;
        }
        uint64_t target_va = 0;
        uint64_t page_offset = 0;
        uint64_t map_size = 0;
        status = load_segment_bounds(ph, load_bias, &target_va, &page_offset, &map_size);
        if (status != 0) {
            break;
        }
        if (map_size == 0) {
            continue;
        }
        memcpy(mapped + (target_va - span_base) + page_offset, image->bytes + p_offset, (size_t)p_filesz);
        if (patch_text && (lpr_exec_rd32(ph + 4) & LPR_EXEC_PF_X) != 0) {
            uint64_t patch_file_offset = 0;
            uint64_t patch_file_size = 0;
            if (lpr_exec_text_file_intersection(
                    p_offset,
                    p_filesz,
                    text_offset,
                    text_size,
                    &patch_file_offset,
                    &patch_file_size))
            {
                lpr_exec_patch_segment_text_range(
                    mapped + (target_va - span_base),
                    map_size,
                    p_offset,
                    page_offset,
                    patch_file_offset,
                    patch_file_size);
            }
        }
    }
    (void)pacha_munmap(mapped, span_size);

    int keep_vmo_cached = 0;
    if (status == 0) {
        status = map_memory_image_span_vmo(
            process_fd,
            vmo_fd,
            span_base,
            phdrs,
            phnum,
            phent,
            load_bias,
            extra_entries,
            extra_count);
    }
    if (status == 0 && cacheable) {
        while (lpr_exec_memory_span_vmo_cache_bytes + span_size > LPR_EXEC_MEMORY_SPAN_VMO_CACHE_MAX_BYTES) {
            lpr_exec_memory_span_vmo_cache_slot_t *victim = lpr_exec_memory_span_vmo_cache_choose_slot();
            if (victim == NULL || !victim->valid) {
                break;
            }
            lpr_exec_memory_span_vmo_cache_clear_slot(victim);
        }
        slot = lpr_exec_memory_span_vmo_cache_choose_slot();
        if (slot != NULL) {
            if (slot->valid) {
                lpr_exec_memory_span_vmo_cache_clear_slot(slot);
            }
            slot->valid = true;
            slot->backend_object = image->backend_object;
            slot->object_generation = image->object_generation;
            slot->image_size = image->size;
            slot->load_bias = load_bias;
            slot->span_base = span_base;
            slot->span_size = span_size;
            slot->patch_text = patch_text;
            slot->last_used = lpr_exec_memory_span_vmo_cache_next_clock();
            slot->vmo_fd = vmo_fd;
            lpr_exec_memory_span_vmo_cache_bytes += span_size;
            lpr_exec_map_metric_count("memory_span_store", span_size);
            keep_vmo_cached = 1;
        }
    }
    if (!keep_vmo_cached) {
        (void)pacha_fd_close(vmo_fd);
    }
    if (status != 0) {
        return status;
    }
    if (out_load_segments != NULL) {
        *out_load_segments = load_segments;
    }
    return 0;
}

static int lpr_exec_load_file_span_vmo_into_process(
    filed_runtime_t *runtime,
    int process_fd,
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta,
    uint64_t load_bias,
    int patch_text,
    uint64_t span_base,
    uint64_t span_size,
    uint64_t file_span_offset,
    uint64_t file_span_size,
    unsigned char *file_span,
    uint16_t load_segments,
    uint16_t *out_load_segments)
{
    if (runtime == NULL || process_fd < 16 || file == NULL || meta == NULL) {
        return -22;
    }
    (void)runtime;

    const int cacheable = file_span_vmo_cacheable(file, span_size);
    lpr_exec_memory_span_vmo_cache_slot_t *slot = NULL;
    if (cacheable) {
        slot = lpr_exec_memory_span_vmo_cache_find(
            file->backend_object,
            file->object_generation,
            file->size,
            load_bias,
            span_base,
            span_size,
            patch_text);
        if (slot != NULL && slot->vmo_fd >= 16) {
            slot->last_used = lpr_exec_memory_span_vmo_cache_next_clock();
            lpr_exec_map_metric_count("file_span_hit", span_size);
            const int status = map_memory_image_span_vmo(
                process_fd,
                slot->vmo_fd,
                span_base,
                meta->phdrs,
                meta->phnum,
                meta->phent,
                load_bias,
                NULL,
                0);
            if (status != 0) {
                return status;
            }
            if (out_load_segments != NULL) {
                *out_load_segments = load_segments;
            }
            return 0;
        }
        lpr_exec_map_metric_count("file_span_miss", span_size);
    }

    int vmo_fd = -1;
    unsigned char *mapped = NULL;
    int status = create_segment_vmo(span_size, &vmo_fd, &mapped);
    if (status != 0) {
        return status;
    }
    memset(mapped, 0, (size_t)span_size);
    for (uint16_t i = 0; i < meta->phnum; ++i) {
        const unsigned char *ph = meta->phdrs + (uint64_t)i * meta->phent;
        if (lpr_exec_rd32(ph) != LPR_EXEC_PT_LOAD) {
            continue;
        }
        const uint64_t p_offset = lpr_exec_rd64(ph + 8);
        const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
        const uint64_t p_memsz = lpr_exec_rd64(ph + 40);
        if (p_filesz > p_memsz || p_offset > file->size || p_filesz > file->size - p_offset) {
            status = -8;
            break;
        }
        uint64_t target_va = 0;
        uint64_t page_offset = 0;
        uint64_t map_size = 0;
        status = load_segment_bounds(ph, load_bias, &target_va, &page_offset, &map_size);
        if (status != 0) {
            break;
        }
        if (map_size == 0) {
            continue;
        }
        if (target_va < span_base || target_va - span_base > span_size || map_size > span_size - (target_va - span_base)) {
            status = -8;
            break;
        }
        if (p_filesz != 0 &&
            (file_span == NULL ||
             p_offset < file_span_offset ||
             p_filesz > file_span_size ||
             p_offset - file_span_offset > file_span_size - p_filesz))
        {
            status = -8;
            break;
        }

        if (p_filesz != 0) {
            memcpy(
                mapped + (target_va - span_base) + page_offset,
                file_span + (p_offset - file_span_offset),
                (size_t)p_filesz);
        }
        if (patch_text && (lpr_exec_rd32(ph + 4) & LPR_EXEC_PF_X) != 0) {
            uint64_t patch_file_offset = 0;
            uint64_t patch_file_size = 0;
            if (lpr_exec_text_file_intersection(
                    p_offset,
                    p_filesz,
                    meta->text_offset,
                    meta->text_size,
                    &patch_file_offset,
                    &patch_file_size))
            {
                lpr_exec_patch_segment_text_range(
                    mapped + (target_va - span_base),
                    map_size,
                    p_offset,
                    page_offset,
                    patch_file_offset,
                    patch_file_size);
            }
        }
    }
    (void)pacha_munmap(mapped, span_size);

    int keep_vmo_cached = 0;
    if (status == 0) {
        status = map_memory_image_span_vmo(
            process_fd,
            vmo_fd,
            span_base,
            meta->phdrs,
            meta->phnum,
            meta->phent,
            load_bias,
            NULL,
            0);
    }
    if (status == 0 && cacheable) {
        while (lpr_exec_memory_span_vmo_cache_bytes + span_size > LPR_EXEC_MEMORY_SPAN_VMO_CACHE_MAX_BYTES) {
            lpr_exec_memory_span_vmo_cache_slot_t *victim = lpr_exec_memory_span_vmo_cache_choose_slot();
            if (victim == NULL || !victim->valid) {
                break;
            }
            lpr_exec_memory_span_vmo_cache_clear_slot(victim);
        }
        slot = lpr_exec_memory_span_vmo_cache_choose_slot();
        if (slot != NULL) {
            if (slot->valid) {
                lpr_exec_memory_span_vmo_cache_clear_slot(slot);
            }
            slot->valid = true;
            slot->backend_object = file->backend_object;
            slot->object_generation = file->object_generation;
            slot->image_size = file->size;
            slot->load_bias = load_bias;
            slot->span_base = span_base;
            slot->span_size = span_size;
            slot->patch_text = patch_text;
            slot->last_used = lpr_exec_memory_span_vmo_cache_next_clock();
            slot->vmo_fd = vmo_fd;
            lpr_exec_memory_span_vmo_cache_bytes += span_size;
            lpr_exec_map_metric_count("file_span_store", span_size);
            keep_vmo_cached = 1;
        }
    }
    if (!keep_vmo_cached) {
        (void)pacha_fd_close(vmo_fd);
    }
    if (status != 0) {
        return status;
    }
    if (out_load_segments != NULL) {
        *out_load_segments = load_segments;
    }
    return 0;
}

static int lpr_exec_try_map_file_span_vmo_cache(
    int process_fd,
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta,
    uint64_t load_bias,
    int patch_text,
    uint64_t span_base,
    uint64_t span_size,
    uint16_t load_segments,
    uint16_t *out_load_segments,
    int *out_hit)
{
    if (out_hit != NULL) {
        *out_hit = 0;
    }
    if (process_fd < 16 || file == NULL || meta == NULL || out_hit == NULL) {
        return -22;
    }
    if (!file_span_vmo_cacheable(file, span_size)) {
        return 0;
    }
    lpr_exec_memory_span_vmo_cache_slot_t *slot = lpr_exec_memory_span_vmo_cache_find(
        file->backend_object,
        file->object_generation,
        file->size,
        load_bias,
        span_base,
        span_size,
        patch_text);
    if (slot == NULL || slot->vmo_fd < 16) {
        lpr_exec_map_metric_count("file_span_miss", span_size);
        return 0;
    }
    slot->last_used = lpr_exec_memory_span_vmo_cache_next_clock();
    lpr_exec_map_metric_count("file_span_hit", span_size);
    const int status = map_memory_image_span_vmo(
        process_fd,
        slot->vmo_fd,
        span_base,
        meta->phdrs,
        meta->phnum,
        meta->phent,
        load_bias,
        NULL,
        0);
    if (status != 0) {
        return status;
    }
    if (out_load_segments != NULL) {
        *out_load_segments = load_segments;
    }
    *out_hit = 1;
    return 0;
}

static int lpr_exec_load_file_image_into_process(
    filed_runtime_t *runtime,
    int process_fd,
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta,
    uint64_t load_bias,
    int patch_text,
    uint16_t *out_load_segments)
{
    if (runtime == NULL || file == NULL || meta == NULL) {
        return -22;
    }
    lpr_exec_file_map_plan_cache_slot_t *plan = NULL;
    int status = get_file_map_plan(file, meta, load_bias, patch_text, &plan);
    if (status != 0) {
        return status;
    }
    if (plan == NULL || plan->load_segments == 0) {
        return -8;
    }

    const int use_full_span_vmo =
        !plan->has_page_overlap &&
        file_span_vmo_cacheable(file, plan->span_size);
    if (use_full_span_vmo) {
        int span_cache_hit = 0;
        status = lpr_exec_try_map_file_span_vmo_cache(
            process_fd,
            file,
            meta,
            load_bias,
            patch_text,
            plan->span_base,
            plan->span_size,
            plan->load_segments,
            out_load_segments,
            &span_cache_hit);
        if (status != 0) {
            return status;
        }
        if (span_cache_hit) {
            return 0;
        }
    }

    unsigned char *file_span = NULL;
    bool file_span_owned = true;
    const int needs_file_span = use_full_span_vmo ?
        plan->file_span_size != 0 :
        file_map_plan_needs_file_span(file, plan);
    if (needs_file_span && plan->file_span_size != 0) {
        status = lpr_exec_read_file_range_for_load(
            runtime,
            file,
            plan->file_span_offset,
            plan->file_span_size,
            &file_span,
            &file_span_owned);
        if (status != 0) {
            return status;
        }
    }
    if (!plan->has_page_overlap && use_full_span_vmo) {
        status = lpr_exec_load_file_span_vmo_into_process(
            runtime,
            process_fd,
            file,
            meta,
            load_bias,
            patch_text,
            plan->span_base,
            plan->span_size,
            plan->file_span_offset,
            plan->file_span_size,
            file_span,
            plan->load_segments,
            out_load_segments);
    } else {
        struct pacha_process_map_batch_entry map_entries[PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES];
        int close_fds[PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES];
        uint64_t map_entry_count = 0;
        for (uint64_t i = 0; i < PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES; ++i) {
            close_fds[i] = -1;
        }
        for (uint16_t i = 0; i < plan->segment_count; ++i) {
            lpr_exec_file_map_segment_plan_t *segment = &plan->segments[i];
            const unsigned char *source = NULL;
            int segment_vmo_fd = cached_readonly_segment_vmo_lookup_from_plan(file, segment, 1);
            int close_segment_vmo = 0;
            if (segment_vmo_fd < 16) {
                if (segment->file_size != 0 &&
                    (file_span == NULL ||
                     segment->file_offset < plan->file_span_offset ||
                     segment->file_size > plan->file_span_size ||
                     segment->file_offset - plan->file_span_offset > plan->file_span_size - segment->file_size))
                {
                    status = -8;
                    break;
                }
                source = segment->file_size == 0 ?
                    NULL :
                    file_span + (segment->file_offset - plan->file_span_offset);
                segment_vmo_fd = cached_readonly_segment_vmo_from_plan(file, segment, source);
            }
            if (segment_vmo_fd < 16) {
                status = create_segment_vmo_from_bytes(
                    source,
                    segment->file_size,
                    segment->file_offset,
                    segment->page_offset,
                    segment->map_size,
                    segment->patch_text,
                    segment->patch_file_offset,
                    segment->patch_file_size,
                    &segment_vmo_fd);
                if (status != 0) {
                    break;
                }
                close_segment_vmo = 1;
            }

            if (map_entry_count >= PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES) {
                status = map_file_segment_batch(process_fd, map_entries, close_fds, &map_entry_count);
                if (status != 0) {
                    if (close_segment_vmo) {
                        (void)pacha_fd_close(segment_vmo_fd);
                    }
                    break;
                }
            }

            map_entries[map_entry_count] = (struct pacha_process_map_batch_entry){
                .vmo_fd = (uint64_t)(uint32_t)segment_vmo_fd,
                .target_va = segment->target_va,
                .size = segment->map_size,
                .prot = segment->prot,
                .vmo_offset = 0,
                .flags = PACHA_PROCESS_MAP_PRIVATE,
            };
            close_fds[map_entry_count] = close_segment_vmo ? segment_vmo_fd : -1;
            map_entry_count++;
        }
        if (status == 0) {
            status = map_file_segment_batch(process_fd, map_entries, close_fds, &map_entry_count);
        } else {
            for (uint64_t i = 0; i < map_entry_count; ++i) {
                if (close_fds[i] >= 16) {
                    (void)pacha_fd_close(close_fds[i]);
                    close_fds[i] = -1;
                }
            }
        }
        if (status != 0) {
            map_entry_count = 0;
        }
        if (status == 0 && out_load_segments != NULL) {
            *out_load_segments = plan->load_segments;
        }
    }
    if (file_span_owned) {
        free(file_span);
    }
    return status;
}

int lpr_exec_prepare_file_into_map_batch(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta,
    uint64_t dyn_base,
    int patch_text,
    lpr_exec_pending_map_batch_t *batch,
    lpr_exec_loaded_t *loaded)
{
    if (runtime == NULL || file == NULL || meta == NULL || batch == NULL || loaded == NULL) {
        return -22;
    }
    if (meta->phdrs == NULL || meta->phnum == 0 || meta->phent < LPR_EXEC_PHDR_BYTES) {
        return -8;
    }
    memset(loaded, 0, sizeof(*loaded));

    uint64_t load_bias = 0;
    uint64_t phdr_va = 0;
    int status = validate_loaded_meta(meta->type, meta->phent, meta->phnum);
    if (status != 0) {
        return status;
    }
    status = load_bias_for_type(meta->type, dyn_base, &load_bias);
    if (status != 0) {
        return status;
    }
    status = phdr_va_from_phdrs(meta->phdrs, meta->phnum, meta->phent, meta->phoff, &phdr_va);
    if (status != 0) {
        return status;
    }

    lpr_exec_file_map_plan_cache_slot_t *plan = NULL;
    status = get_file_map_plan(file, meta, load_bias, patch_text, &plan);
    if (status != 0) {
        return status;
    }
    if (plan == NULL || plan->load_segments == 0) {
        return -8;
    }

    unsigned char *file_span = NULL;
    bool file_span_owned = true;
    const int needs_file_span = file_map_plan_needs_readonly_warm_span(file, plan);
    if (needs_file_span && plan->file_span_size != 0) {
        status = lpr_exec_read_file_range_for_load(
            runtime,
            file,
            plan->file_span_offset,
            plan->file_span_size,
            &file_span,
            &file_span_owned);
        if (status != 0) {
            return status;
        }
    }

    for (uint16_t i = 0; i < plan->segment_count; ++i) {
        lpr_exec_file_map_segment_plan_t *segment = &plan->segments[i];
        const unsigned char *source = NULL;
        unsigned char *source_span = NULL;
        bool source_span_owned = true;
        int segment_vmo_fd = cached_readonly_segment_vmo_lookup_from_plan(file, segment, 1);
        int close_segment_vmo = 0;
        if (segment_vmo_fd < 16) {
            if (segment->file_size != 0) {
                if (file_span != NULL &&
                    segment->file_offset >= plan->file_span_offset &&
                    segment->file_size <= plan->file_span_size &&
                    segment->file_offset - plan->file_span_offset <= plan->file_span_size - segment->file_size)
                {
                    source = file_span + (segment->file_offset - plan->file_span_offset);
                } else {
                    status = lpr_exec_read_file_range_for_load(
                        runtime,
                        file,
                        segment->file_offset,
                        segment->file_size,
                        &source_span,
                        &source_span_owned);
                    if (status != 0) {
                        break;
                    }
                    source = source_span;
                }
            }
            segment_vmo_fd = cached_readonly_segment_vmo_from_plan(file, segment, source);
        }
        if (segment_vmo_fd < 16) {
            status = create_segment_vmo_from_bytes(
                source,
                segment->file_size,
                segment->file_offset,
                segment->page_offset,
                segment->map_size,
                segment->patch_text,
                segment->patch_file_offset,
                segment->patch_file_size,
                &segment_vmo_fd);
            if (status != 0) {
                if (source_span_owned) {
                    free(source_span);
                }
                break;
            }
            close_segment_vmo = 1;
        }
        if (source_span_owned) {
            free(source_span);
        }

        if (batch->count >= PACHA_PROCESS_MAP_BATCH_MAX_ENTRIES) {
            if (close_segment_vmo) {
                (void)pacha_fd_close(segment_vmo_fd);
            }
            status = -7;
            break;
        }

        batch->entries[batch->count] = (struct pacha_process_map_batch_entry){
            .vmo_fd = (uint64_t)(uint32_t)segment_vmo_fd,
            .target_va = segment->target_va,
            .size = segment->map_size,
            .prot = segment->prot,
            .vmo_offset = 0,
            .flags = PACHA_PROCESS_MAP_PRIVATE,
        };
        batch->close_fds[batch->count] = close_segment_vmo ? segment_vmo_fd : -1;
        batch->count++;
    }

    if (file_span_owned) {
        free(file_span);
    }
    if (status != 0) {
        return status;
    }
    return set_loaded_result(loaded, meta->entry, load_bias, phdr_va, meta->phent, meta->phnum, plan->load_segments);
}

static int phdr_va_from_phdrs(
    const unsigned char *phdrs,
    uint16_t phnum,
    uint16_t phent,
    uint64_t phoff,
    uint64_t *out_phdr_va)
{
    if (out_phdr_va == NULL) {
        return -22;
    }
    uint64_t phdr_va = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        const unsigned char *ph = phdrs + (uint64_t)i * phent;
        if (lpr_exec_rd32(ph) == LPR_EXEC_PT_PHDR) {
            phdr_va = lpr_exec_rd64(ph + 16);
            break;
        }
    }
    if (phdr_va == 0) {
        const uint64_t phdr_bytes = (uint64_t)phnum * (uint64_t)phent;
        for (uint16_t i = 0; i < phnum; ++i) {
            const unsigned char *ph = phdrs + (uint64_t)i * phent;
            if (lpr_exec_rd32(ph) != LPR_EXEC_PT_LOAD) {
                continue;
            }
            const uint64_t p_offset = lpr_exec_rd64(ph + 8);
            const uint64_t p_vaddr = lpr_exec_rd64(ph + 16);
            const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
            if (phoff >= p_offset &&
                phoff - p_offset <= p_filesz &&
                phdr_bytes <= p_filesz - (phoff - p_offset))
            {
                const uint64_t phdr_delta = phoff - p_offset;
                if (p_vaddr > UINT64_MAX - phdr_delta) {
                    return -75;
                }
                phdr_va = p_vaddr + phdr_delta;
                break;
            }
        }
    }
    if (phdr_va == 0) {
        return -8;
    }
    *out_phdr_va = phdr_va;
    return 0;
}

static int load_bias_for_type(uint16_t type, uint64_t dyn_base, uint64_t *out_load_bias)
{
    if (out_load_bias == NULL) {
        return -22;
    }
    *out_load_bias = type == LPR_EXEC_ELF_TYPE_DYN ? dyn_base : 0;
    return 0;
}

static int set_loaded_result(
    lpr_exec_loaded_t *loaded,
    uint64_t entry,
    uint64_t load_bias,
    uint64_t phdr_va,
    uint16_t phent,
    uint16_t phnum,
    uint16_t load_segments)
{
    if (loaded == NULL || load_segments == 0) {
        return -8;
    }
    loaded->entry = entry + load_bias;
    loaded->base = load_bias;
    loaded->phdr_va = phdr_va + load_bias;
    loaded->phent = phent;
    loaded->phnum = phnum;
    loaded->load_segments = load_segments;
    return 0;
}

static int validate_loaded_meta(uint16_t type, uint16_t phent, uint16_t phnum)
{
    if ((type != LPR_EXEC_ELF_TYPE_DYN && type != LPR_EXEC_ELF_TYPE_EXEC) ||
        phnum == 0 ||
        phent < LPR_EXEC_PHDR_BYTES)
    {
        return -8;
    }
    return 0;
}

int lpr_exec_load_image_into_process(
    int process_fd,
    const lpr_exec_image_t *image,
    uint64_t dyn_base,
    int patch_text,
    lpr_exec_loaded_t *loaded)
{
    if (process_fd < 16 || image == NULL || loaded == NULL) {
        return -22;
    }
    memset(loaded, 0, sizeof(*loaded));
    int status = lpr_exec_validate_elf(image);
    if (status != 0) {
        return status;
    }
    const unsigned char *bytes = image->bytes;
    const uint64_t e_entry = lpr_exec_rd64(bytes + 24);
    const uint16_t e_type = lpr_exec_rd16(bytes + 16);
    const uint64_t e_phoff = lpr_exec_rd64(bytes + 32);
    const uint16_t e_phentsize = lpr_exec_rd16(bytes + 54);
    const uint16_t e_phnum = lpr_exec_rd16(bytes + 56);
    const unsigned char *phdrs = bytes + e_phoff;
    uint64_t load_bias = 0;
    uint64_t phdr_va = 0;
    uint16_t load_segments = 0;

    status = validate_loaded_meta(e_type, e_phentsize, e_phnum);
    if (status != 0) {
        return status;
    }
    status = load_bias_for_type(e_type, dyn_base, &load_bias);
    if (status != 0) {
        return status;
    }
    status = phdr_va_from_phdrs(phdrs, e_phnum, e_phentsize, e_phoff, &phdr_va);
    if (status != 0) {
        return status;
    }
    status = lpr_exec_load_memory_image_into_process(
        process_fd,
        image,
        phdrs,
        e_phnum,
        e_phentsize,
        load_bias,
        patch_text,
        NULL,
        0,
        &load_segments);
    if (status != 0) {
        return status;
    }
    return set_loaded_result(loaded, e_entry, load_bias, phdr_va, e_phentsize, e_phnum, load_segments);
}

int lpr_exec_load_image_with_low_layout_into_process(
    int process_fd,
    const lpr_exec_image_t *image,
    uint64_t dyn_base,
    int patch_text,
    uint64_t syscall_entry_offset,
    lpr_exec_loaded_t *loaded)
{
    if (process_fd < 16 || image == NULL || loaded == NULL) {
        return -22;
    }
    memset(loaded, 0, sizeof(*loaded));
    int status = lpr_exec_validate_elf(image);
    if (status != 0) {
        return status;
    }
    const unsigned char *bytes = image->bytes;
    const uint64_t e_entry = lpr_exec_rd64(bytes + 24);
    const uint16_t e_type = lpr_exec_rd16(bytes + 16);
    const uint64_t e_phoff = lpr_exec_rd64(bytes + 32);
    const uint16_t e_phentsize = lpr_exec_rd16(bytes + 54);
    const uint16_t e_phnum = lpr_exec_rd16(bytes + 56);
    const unsigned char *phdrs = bytes + e_phoff;
    uint64_t load_bias = 0;
    uint64_t phdr_va = 0;
    uint16_t load_segments = 0;

    status = validate_loaded_meta(e_type, e_phentsize, e_phnum);
    if (status != 0) {
        return status;
    }
    status = load_bias_for_type(e_type, dyn_base, &load_bias);
    if (status != 0) {
        return status;
    }
    status = phdr_va_from_phdrs(phdrs, e_phnum, e_phentsize, e_phoff, &phdr_va);
    if (status != 0) {
        return status;
    }
    if (syscall_entry_offset > UINT64_MAX - load_bias) {
        return -75;
    }
    const int zpoline_vmo_fd = zpoline_page_vmo(load_bias + syscall_entry_offset);
    if (zpoline_vmo_fd < 16) {
        return zpoline_vmo_fd;
    }
    const struct pacha_process_map_batch_entry low_layout_entry = {
        .vmo_fd = (uint64_t)(uint32_t)zpoline_vmo_fd,
        .target_va = LPR_ZPOLINE_PAGE_VA,
        .size = LPR_ZPOLINE_PAGE_SIZE,
        .prot = PACHA_PROT_READ | PACHA_PROT_EXEC,
        .vmo_offset = 0,
        .flags = 0,
    };
    status = lpr_exec_load_memory_image_into_process(
        process_fd,
        image,
        phdrs,
        e_phnum,
        e_phentsize,
        load_bias,
        patch_text,
        &low_layout_entry,
        1,
        &load_segments);
    if (status != 0) {
        return status;
    }
    return set_loaded_result(loaded, e_entry, load_bias, phdr_va, e_phentsize, e_phnum, load_segments);
}

int lpr_exec_load_file_into_process(
    filed_runtime_t *runtime,
    int process_fd,
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta,
    uint64_t dyn_base,
    int patch_text,
    lpr_exec_loaded_t *loaded)
{
    if (runtime == NULL || process_fd < 16 || file == NULL || meta == NULL || loaded == NULL) {
        return -22;
    }
    if (meta->phdrs == NULL || meta->phnum == 0 || meta->phent < LPR_EXEC_PHDR_BYTES) {
        return -8;
    }
    memset(loaded, 0, sizeof(*loaded));
    uint64_t load_bias = 0;
    uint64_t phdr_va = 0;
    uint16_t load_segments = 0;

    int status = validate_loaded_meta(meta->type, meta->phent, meta->phnum);
    if (status != 0) {
        return status;
    }
    status = load_bias_for_type(meta->type, dyn_base, &load_bias);
    if (status != 0) {
        return status;
    }
    status = phdr_va_from_phdrs(meta->phdrs, meta->phnum, meta->phent, meta->phoff, &phdr_va);
    if (status != 0) {
        return status;
    }
    status = lpr_exec_load_file_image_into_process(
        runtime,
        process_fd,
        file,
        meta,
        load_bias,
        patch_text,
        &load_segments);
    if (status != 0) {
        return status;
    }
    return set_loaded_result(loaded, meta->entry, load_bias, phdr_va, meta->phent, meta->phnum, load_segments);
}
