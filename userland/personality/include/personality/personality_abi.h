#ifndef PERSONALITY_PERSONALITY_ABI_H
#define PERSONALITY_PERSONALITY_ABI_H

#include <stdint.h>

#include "pacha/service_abi.h"

#define PERSONALITY_ABI_VERSION PACHA_SERVICE_ABI_VERSION
#define PERSONALITY_PAGE_SIZE 4096ull

#define PERSONALITY_LPR_NOTE_MAGIC 0x3250524c50414348ull
#define PERSONALITY_TRAP_FRAME_MAGIC 0x325241505052534full

enum personality_id {
    PERSONALITY_ID_NONE = 0,
    PERSONALITY_ID_LINUX = 1,
    PERSONALITY_ID_FREEBSD = 2,
};

enum personality_status {
    PERSONALITY_STATUS_OK = 0,
    PERSONALITY_STATUS_INVALID = 1,
    PERSONALITY_STATUS_UNSUPPORTED = 2,
    PERSONALITY_STATUS_NO_MEMORY = 3,
    PERSONALITY_STATUS_NOT_READY = 4,
};

struct personality_lpr_note {
    uint64_t magic;
    uint32_t version;
    uint32_t personality_id;
    uint64_t flags;
    uint64_t lpr_start_offset;
    uint64_t syscall_entry_offset;
    uint64_t trap_entry_offset;
    uint64_t patch_mapping_offset;
    uint64_t runtime_state_size;
    uint64_t zpoline_page_size;
    uint64_t reserved0;
};

struct personality_trap_frame {
    uint64_t magic;
    uint32_t version;
    uint32_t personality_id;
    uint64_t syscall_nr;
    uint64_t args[6];
    uint64_t saved_rip;
    uint64_t saved_rsp;
    uint64_t saved_rflags;
    uint64_t syscall_site_va;
    uint64_t result_rax;
    uint64_t flags;
    uint64_t reserved0;
};

_Static_assert(sizeof(struct personality_lpr_note) == 80, "personality_lpr_note size");
_Static_assert(sizeof(struct personality_trap_frame) == 128, "personality_trap_frame size");

#endif
