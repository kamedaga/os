/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_LAUNCH_IMAGE_H
#define PACHA_GPUD_LAUNCH_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define GPUD_LAUNCH_MAX_SEGMENTS 64u
#define GPUD_LAUNCH_PAGE 4096u
#define GPUD_LAUNCH_IMAGE_LIMIT (128u * 1024u * 1024u)

struct gpud_launch_segment {
    uint64_t address, mapping_size, file_offset, file_size, data_offset, protection;
};

struct gpud_launch_image {
    uint64_t entry;
    size_t segment_count;
    struct gpud_launch_segment segments[GPUD_LAUNCH_MAX_SEGMENTS];
};

/* Build-selected CPU profile for the freestanding native sandbox executable,
 * NOT a Linux module loader or general application runtime. Reads immutable
 * caller-owned bytes, validates all segments before publishing any plan.
 * ET_DYN must provide its own entry-time relocation; no interpreter, TLS
 * loader, argv/envp or libc startup is supplied here. The sandbox build must
 * isolate the rounded PT_LOAD ranges on native page boundaries and never
 * require W+X. A segment's first byte need not itself be page-aligned. */
int gpud_launch_image_plan(const void *bytes, size_t size, uint64_t load_bias,
    struct gpud_launch_image *out);
/* CPU profile's usable user address range, excluding the null page. */
int gpud_launch_address_valid(uint64_t address, uint64_t size);

#endif
