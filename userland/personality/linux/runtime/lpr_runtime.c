#include <stdint.h>
#include <personality/linux_lpr.h>

extern void lpr_syscall_entry(void);
extern void lpr_trap_entry(void);
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
    if ((request->flags & LPR_PATCH_FLAG_EXECUTABLE) == 0) {
        return LPR_PATCH_STATUS_NOT_EXECUTABLE;
    }
    if (request->start_va == 0 || request->size_bytes == 0) {
        return PERSONALITY_STATUS_OK;
    }
    uint8_t *bytes = (uint8_t *)(uintptr_t)request->start_va;
    result->scanned_bytes = request->size_bytes;
    if (request->size_bytes < 2) {
        return PERSONALITY_STATUS_OK;
    }
    for (uint64_t i = 0; i + 1 < request->size_bytes; i++) {
        if (bytes[i] == LPR_ZPOLINE_PATCH_FROM0 && bytes[i + 1] == LPR_ZPOLINE_PATCH_FROM1) {
            bytes[i] = LPR_ZPOLINE_PATCH_TO0;
            bytes[i + 1] = LPR_ZPOLINE_PATCH_TO1;
            result->patched_sites++;
            i++;
        }
    }
    return PERSONALITY_STATUS_OK;
}

int64_t lpr_init_zpoline_page(uint8_t *page) {
    return lpr_build_zpoline_page(page, (uint64_t)(uintptr_t)lpr_syscall_entry);
}
