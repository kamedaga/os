#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lpr_image_abi.h"

enum {
    LPR_MANIFEST_FLAG_DEFAULT_STDIO = 1ull << 0,
    LPR_MANIFEST_FLAG_SUPERVISOR = 1ull << 1,

    LPR_MANIFEST_ENTRY_OPEN = 1u,

    LPR_MANIFEST_CAPABILITY_CWD_LEASE = 1u << 0,

    LPR_MANIFEST_CTTY_BYTES = 64u,
    LPR_MANIFEST_CWD_BYTES = 480u,
};

#define LPR_MANIFEST_MAGIC 0x3154464e4d52504cull

typedef struct lpr_manifest_entry {
    uint32_t fd;
    uint32_t ofd_index;
    uint32_t ofd_generation;
    uint16_t fd_flags;
    uint16_t state;
    uint64_t effective_rights;
    uint64_t reserved0;
} lpr_manifest_entry_t;

typedef struct lpr_manifest_ofd {
    uint64_t generation;
    uint32_t backend_id;
    uint16_t access_mode;
    uint16_t capability_count;
    uint32_t status_flags;
    uint32_t rights_ceiling;
    uint64_t offset;
    uint64_t record_offset;
    uint32_t record_bytes;
    uint32_t capability_first;
    uint64_t reserved0;
    uint64_t reserved1;
} lpr_manifest_ofd_t;

typedef struct lpr_manifest_capability {
    uint32_t ordinal;
    uint32_t flags;
    uint64_t native_fd;
    uint64_t rights;
} lpr_manifest_capability_t;

typedef struct lpr_manifest {
    uint64_t magic;
    uint64_t byte_size;
    uint64_t transaction_id;
    uint64_t generation;
    uint64_t checksum;
    uint64_t flags;
    uint64_t entry_offset;
    uint64_t entry_count;
    uint64_t ofd_offset;
    uint64_t ofd_count;
    uint64_t capability_offset;
    uint64_t capability_count;
    uint64_t record_offset;
    uint64_t record_bytes;
    uint64_t linux_pid;
    uint64_t linux_ppid;
    uint64_t linux_sid;
    uint64_t linux_pgrp;
    uint64_t linux_next_pid;
    uint64_t signal_mask;
    uint64_t signal_ignored_mask;
    uint64_t cwd_handle;
    uint64_t cwd_capability_index;
    uint64_t supervisor_token;
    uint64_t supervisor_endpoint_fd;
    uint64_t owner_generation;
    char ctty[LPR_MANIFEST_CTTY_BYTES];
    char cwd[LPR_MANIFEST_CWD_BYTES];
} lpr_manifest_t;

typedef struct lpr_manifest_layout {
    uint64_t byte_size;
    uint64_t entry_offset;
    uint64_t ofd_offset;
    uint64_t capability_offset;
    uint64_t record_offset;
} lpr_manifest_layout_t;

int lpr_manifest_layout(
    uint64_t entry_count,
    uint64_t ofd_count,
    uint64_t capability_count,
    uint64_t record_bytes,
    lpr_manifest_layout_t *out);
int lpr_manifest_begin(
    void *memory,
    uint64_t capacity,
    const lpr_manifest_layout_t *layout,
    uint64_t entry_count,
    uint64_t ofd_count,
    uint64_t capability_count,
    uint64_t record_bytes);
uint64_t lpr_manifest_checksum(const void *memory, uint64_t byte_size);
int lpr_manifest_seal(lpr_manifest_t *manifest, uint64_t capacity);
int lpr_manifest_validate(const void *memory, uint64_t capacity);

static inline lpr_manifest_entry_t *lpr_manifest_entries(lpr_manifest_t *manifest)
{
    return (lpr_manifest_entry_t *)((uint8_t *)manifest + manifest->entry_offset);
}

static inline lpr_manifest_ofd_t *lpr_manifest_ofds(lpr_manifest_t *manifest)
{
    return (lpr_manifest_ofd_t *)((uint8_t *)manifest + manifest->ofd_offset);
}

static inline lpr_manifest_capability_t *lpr_manifest_capabilities(lpr_manifest_t *manifest)
{
    return (lpr_manifest_capability_t *)((uint8_t *)manifest + manifest->capability_offset);
}

_Static_assert(sizeof(lpr_manifest_entry_t) == 32, "lpr manifest entry size");
_Static_assert(sizeof(lpr_manifest_ofd_t) == 64, "lpr manifest ofd size");
_Static_assert(sizeof(lpr_manifest_capability_t) == 24, "lpr manifest capability size");
_Static_assert(sizeof(lpr_manifest_t) == 752, "lpr manifest header size");
