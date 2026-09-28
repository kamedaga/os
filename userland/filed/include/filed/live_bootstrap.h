#pragma once

#include <stdint.h>

/* A separate private launch contract: a bootfs archive is not an ext4 backend.
 * Keeping its magic distinct prevents a storage bootstrap from being mistaken
 * for a writable RAM root (or the reverse). */
enum {
    FILED_LIVE_BOOTSTRAP_MAGIC = 0x324d4152444c4946ull,
    FILED_LIVE_READY_MAGIC = 0x315944524d415246ull,
    /* Bootfs now uses bounded allocated boot pages, separate from the
     * seed0boot ELF scratch. This also bounds untrusted bootstrap input. */
    FILED_LIVE_BOOTFS_MAX_BYTES = 192u * 1024u * 1024u,
};

typedef struct filed_live_bootstrap {
    uint64_t magic;
    uint64_t bootfs_fd;
    uint64_t bootfs_size;
    uint64_t public_endpoint_fd;
    uint64_t ready_channel_fd;
    uint64_t unix_path_fd;
} filed_live_bootstrap_t;
