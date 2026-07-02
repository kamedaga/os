#include <stdint.h>
#include <hde64.h>
#include <personality/linux_lpr.h>

extern void lpr_syscall_entry(void);
extern void lpr_trap_entry(void);

static uint64_t lpr_hde_opcode_offset(const uint8_t *bytes, uint64_t len)
{
    uint64_t off = 0;
    while (off < len) {
        const uint8_t p = bytes[off];
        if (p != 0xf0 && p != 0xf2 && p != 0xf3 && p != 0x2e && p != 0x36 &&
            p != 0x3e && p != 0x26 && p != 0x64 && p != 0x65 && p != 0x66 && p != 0x67)
        {
            break;
        }
        off++;
    }
    if (off < len && bytes[off] >= 0x40 && bytes[off] <= 0x4f) {
        off++;
    }
    return off;
}

static int lpr_hde_decode(const uint8_t *bytes, uint64_t size, uint64_t pos, hde64s *out)
{
    if (bytes == 0 || out == 0 || pos >= size) {
        return 0;
    }
    const unsigned int len = hde64_disasm(bytes + pos, out);
    if (len == 0 || (out->flags & F_ERROR) != 0 || len > size - pos) {
        return 0;
    }
    return (int)len;
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
    if ((request->flags & LPR_PATCH_FLAG_EXECUTABLE) == 0) {
        return LPR_PATCH_STATUS_NOT_EXECUTABLE;
    }
    if (request->start_va == 0 || request->size_bytes == 0) {
        return PERSONALITY_STATUS_OK;
    }
    uint8_t *bytes = (uint8_t *)(uintptr_t)request->start_va;
    result->scanned_bytes = request->size_bytes;
    for (uint64_t i = 0; i < request->size_bytes;) {
        hde64s insn;
        const int insn_len = lpr_hde_decode(bytes, request->size_bytes, i, &insn);
        if (insn_len <= 0) {
            break;
        }
        if (insn.opcode == 0x0f && (insn.opcode2 == 0x05 || insn.opcode2 == 0x34)) {
            hde64s next_insn;
            const uint64_t next = i + (uint64_t)insn_len;
            const int next_len = next < request->size_bytes
                ? lpr_hde_decode(bytes, request->size_bytes, next, &next_insn)
                : 0;
            const uint64_t opcode = i + lpr_hde_opcode_offset(bytes + i, (uint64_t)insn_len);
            if (next_len > 0 && opcode + 1u < request->size_bytes &&
                bytes[opcode] == LPR_ZPOLINE_PATCH_FROM0 &&
                bytes[opcode + 1u] == LPR_ZPOLINE_PATCH_FROM1)
            {
                bytes[opcode] = LPR_ZPOLINE_PATCH_TO0;
                bytes[opcode + 1u] = LPR_ZPOLINE_PATCH_TO1;
                result->patched_sites++;
            } else {
                result->skipped_sites++;
            }
        }
        i += (uint64_t)insn_len;
    }
    return PERSONALITY_STATUS_OK;
}

int64_t lpr_init_zpoline_page(uint8_t *page) {
    return lpr_build_zpoline_page(page, (uint64_t)(uintptr_t)lpr_syscall_entry);
}
