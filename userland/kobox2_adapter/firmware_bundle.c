/* SPDX-License-Identifier: MIT */
#include "firmware_bundle.h"
#include <kobox2/sha256.h>
#include <errno.h>
#include <string.h>

static int valid_name(const char *name, size_t capacity)
{
    size_t start = 0;
    size_t length = 0;

    while (length < capacity && name[length])
        length++;
    if (!length || length == capacity)
        return 0;
    for (size_t i = 0; i <= length; ++i) {
        char c = name[i];

        if (c == '/' || c == '\0') {
            size_t part = i - start;

            if (!part || (part == 1 && name[start] == '.') ||
                (part == 2 && name[start] == '.' && name[start + 1] == '.'))
                return 0;
            start = i + 1;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

int ph_firmware_bundle_open(const void *mapping, size_t size,
    const uint8_t expected_sha256[32],
    struct kobox_linux_firmware_file files[KOBOX_FIRMWARE_MAX_FILES],
    size_t *count)
{
    uint8_t digest[32];
    const struct ph_firmware_bundle *bundle = mapping;
    size_t total = 0;

    if (!mapping || !expected_sha256 || !files || !count || *count ||
        size < sizeof(*bundle) ||
        size > sizeof(*bundle) + PH_FIRMWARE_BUNDLE_MAX_BYTES)
        return -EINVAL;
    kb2_sha256(mapping, size, digest);
    if (memcmp(digest, expected_sha256, sizeof(digest)))
        return -EBADMSG;
    if (bundle->magic != PH_FIRMWARE_BUNDLE_MAGIC ||
        bundle->header_bytes != sizeof(*bundle) ||
        bundle->bytes != size || !bundle->count ||
        bundle->count > KOBOX_FIRMWARE_MAX_FILES)
        return -EINVAL;
    for (size_t i = 0; i < bundle->count; ++i) {
        const struct ph_firmware_bundle_entry *entry = &bundle->entries[i];

        if (!valid_name(entry->name, sizeof(entry->name)) ||
            !entry->size || entry->size > (8u << 20) ||
            entry->offset < sizeof(*bundle) ||
            entry->offset > size || entry->size > size - entry->offset ||
            entry->size > PH_FIRMWARE_BUNDLE_MAX_BYTES - total)
            return -EINVAL;
        for (size_t j = 0; j < i; ++j) {
            const struct ph_firmware_bundle_entry *previous = &bundle->entries[j];

            if (!strcmp(entry->name, previous->name) ||
                (entry->offset < previous->offset + previous->size &&
                 previous->offset < entry->offset + entry->size))
                return -EINVAL;
        }
        kb2_sha256((const unsigned char *)mapping + entry->offset,
            entry->size, digest);
        if (memcmp(digest, entry->sha256, sizeof(digest)))
            return -EBADMSG;
        files[i] = (struct kobox_linux_firmware_file){
            .name = entry->name,
            .data = (const unsigned char *)mapping + entry->offset,
            .size = entry->size,
        };
        memcpy(files[i].sha256, entry->sha256, sizeof(entry->sha256));
        total += entry->size;
    }
    *count = bundle->count;
    return 0;
}
