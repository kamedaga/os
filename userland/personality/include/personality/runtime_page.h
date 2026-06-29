#ifndef PERSONALITY_RUNTIME_PAGE_H
#define PERSONALITY_RUNTIME_PAGE_H

#include <stdint.h>
#include "personality_abi.h"

#define LPR_RUNTIME_MAGIC 0x52504c5841484350ull
#define LPR_RUNTIME_VERSION 1u
#define LPR_INLINE_FD_COUNT 32u
#define LPR_INLINE_VMA_COUNT 24u
#define LPR_INLINE_SITE_COUNT 16u

enum lpr_runtime_flags {
    LPR_RUNTIME_FLAG_ZPOLINE_READY = 1u << 0,
    LPR_RUNTIME_FLAG_COORDINATOR_READY = 1u << 1,
    LPR_RUNTIME_FLAG_TRAP_FALLBACK_READY = 1u << 2,
};

enum lpr_fd_flags {
    LPR_FD_FLAG_USED = 1u << 0,
    LPR_FD_FLAG_CLOEXEC = 1u << 1,
    LPR_FD_FLAG_NONBLOCK = 1u << 2,
    LPR_FD_FLAG_SHARED_TABLE = 1u << 3,
};

struct lpr_fd_entry {
    int32_t linux_fd;
    int32_t pacha_fd;
    uint32_t flags;
    uint32_t generation;
    uint64_t rights;
};

struct lpr_vma_entry {
    uint64_t start_va;
    uint64_t size_bytes;
    uint64_t prot;
    uint64_t flags;
    uint64_t file_id;
    uint64_t file_offset;
};

struct lpr_site_entry {
    uint64_t site_va;
    uint32_t syscall_nr;
    uint32_t flags;
    uint64_t hit_count;
};

struct lpr_runtime_page {
    uint64_t magic;
    uint32_t version;
    uint32_t personality_id;
    uint32_t flags;
    uint32_t reserved0;
    int32_t pid;
    int32_t tid;
    int32_t ppid;
    int32_t reserved1;
    uint64_t coordinator_fd;
    uint64_t filed_endpoint_fd;
    uint64_t brk_base;
    uint64_t brk_current;
    uint64_t brk_limit;
    uint32_t fd_generation;
    uint32_t fd_count;
    uint32_t vma_generation;
    uint32_t vma_count;
    uint32_t site_generation;
    uint32_t site_count;
    uint64_t overflow_fd_table_va;
    uint64_t overflow_vma_table_va;
    uint64_t coordinator_queue_va;
    struct lpr_fd_entry fds[LPR_INLINE_FD_COUNT];
    struct lpr_vma_entry vmas[LPR_INLINE_VMA_COUNT];
    struct lpr_site_entry sites[LPR_INLINE_SITE_COUNT];
};

_Static_assert(sizeof(struct lpr_fd_entry) == 24, "lpr_fd_entry size");
_Static_assert(sizeof(struct lpr_vma_entry) == 48, "lpr_vma_entry size");
_Static_assert(sizeof(struct lpr_site_entry) == 24, "lpr_site_entry size");
_Static_assert(sizeof(struct lpr_runtime_page) <= PERSONALITY_PAGE_SIZE, "lpr runtime must fit one page");

#endif
