#pragma once

#include <stdint.h>
#include "filed/tmpfs_internal.h"

/* Populate only entries of the dedicated bootfs. The archive is immutable;
 * the resulting root is an independent, writable tmpfs. */
int filed_live_bootfs_populate(filed_tmpfs_backend_t *tmpfs,
    const unsigned char *image, uint64_t image_size,
    uint32_t *out_files, uint64_t *out_bytes);
