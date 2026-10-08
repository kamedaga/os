/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_FIRMWARE_BUNDLE_H
#define PACHA_KOBOX_FIRMWARE_BUNDLE_H

#include <stddef.h>
#include <stdint.h>
#include "boot/firmware_files.h"

#define PH_FIRMWARE_BUNDLE_MAGIC UINT64_C(0x314c444e424d5746)
#define PH_FIRMWARE_BUNDLE_MAX_BYTES (64u * 1024u * 1024u)

struct ph_firmware_bundle_entry {
    char name[128];
    uint64_t offset, size;
    uint8_t sha256[32];
};

struct ph_firmware_bundle {
    uint64_t magic, header_bytes, count, bytes;
    struct ph_firmware_bundle_entry entries[KOBOX_FIRMWARE_MAX_FILES];
    unsigned char payload[];
};

_Static_assert(sizeof(struct ph_firmware_bundle_entry) == 176,
    "firmware bundle entry layout");
_Static_assert(sizeof(struct ph_firmware_bundle) == 11296,
    "firmware bundle header layout");

/* Validate the complete immutable mapping before publishing any Linux file
 * pointers. The parent-provided whole-bundle hash binds names, sizes, and
 * file hashes to the launch policy; Linux rechecks each file before staging. */
int ph_firmware_bundle_open(const void *mapping, size_t size,
    const uint8_t expected_sha256[32],
    struct kobox_linux_firmware_file files[KOBOX_FIRMWARE_MAX_FILES],
    size_t *count);

#endif
