#include <stdint.h>
#include "../decoder/patch_scan.h"
#include <personality/linux_lpr.h>
#include <personality/zpoline.h>

extern void lpr_syscall_entry(void);
extern void lpr_trap_entry(void);

static uint64_t lpr_read_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

__attribute__((section(".lpr_note"), used))
const struct personality_lpr_note lpr_personality_note = {
    .magic = PERSONALITY_LPR_NOTE_MAGIC,
    .version = PERSONALITY_ABI_VERSION,
    .personality_id = PERSONALITY_ID_LINUX,
    .flags = 0,
    .lpr_start_offset = 0,
    .syscall_entry_offset = 0,
    .trap_entry_offset = 0,
    .patch_mapping_offset = 0,
    .runtime_state_size = sizeof(struct lpr_runtime_page),
    .zpoline_page_size = LPR_ZPOLINE_PAGE_SIZE,
    .reserved0 = 0,
};

int64_t lpr_start(struct lpr_runtime_page *runtime) {
    if (runtime == 0) {
        return -PERSONALITY_STATUS_INVALID;
    }
    runtime->magic = LPR_RUNTIME_MAGIC;
    runtime->version = LPR_RUNTIME_VERSION;
    runtime->personality_id = PERSONALITY_ID_LINUX;
    runtime->flags = 0;
    runtime->fd_generation = 1;
    runtime->vma_generation = 1;
    runtime->site_generation = 1;
    return PERSONALITY_STATUS_OK;
}

int64_t lpr_patch_mapping(const struct lpr_patch_mapping_request *request,
                          struct lpr_patch_mapping_result *result) {
    if (request == 0 || result == 0) {
        return -PERSONALITY_STATUS_INVALID;
    }
    result->scanned_bytes = 0;
    result->patched_sites = 0;
    result->skipped_sites = 0;
    result->failed_sites = 0;
    result->cycles = 0;
    if ((request->flags & LPR_PATCH_FLAG_EXECUTABLE) == 0) {
        return LPR_PATCH_STATUS_NOT_EXECUTABLE;
    }
    if ((request->flags & LPR_PATCH_FLAG_PRIVATE) == 0) {
        return LPR_PATCH_STATUS_NOT_PRIVATE;
    }
    if (request->start_va == 0 || request->size_bytes == 0) {
        return PERSONALITY_STATUS_OK;
    }
    const uint64_t patch_start_cycles = lpr_read_tsc();
    uint8_t *bytes = (uint8_t *)(uintptr_t)request->start_va;
    result->scanned_bytes = request->size_bytes;
    lpr_patch_scan_result_t scan_result;
    const uint64_t scan_flags =
        (request->flags & LPR_PATCH_FLAG_VALIDATED_INSN) != 0
            ? LPR_PATCH_SCAN_START_BOUNDARY
            : 0;
    const int scan_status = lpr_patch_scan_syscalls(
        bytes, request->size_bytes, scan_flags, &scan_result);
    result->patched_sites = scan_result.patched_sites;
    result->skipped_sites = scan_result.skipped_sites;
    result->failed_sites = scan_result.failed_sites;
    const uint64_t patch_end_cycles = lpr_read_tsc();
    if (patch_end_cycles >= patch_start_cycles) {
        result->cycles = patch_end_cycles - patch_start_cycles;
    }
    return scan_status == 0 ? PERSONALITY_STATUS_OK : -PERSONALITY_STATUS_INVALID;
}

int64_t lpr_init_zpoline_page(uint8_t *page) {
    return lpr_build_zpoline_page(page, (uint64_t)(uintptr_t)lpr_syscall_entry);
}
