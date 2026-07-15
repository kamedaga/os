#include "personality/lpr_manifest.h"

#include <limits.h>

static int lpr_manifest_add(uint64_t left, uint64_t right, uint64_t *out)
{
    if (out == NULL || right > UINT64_MAX - left) {
        return -1;
    }
    *out = left + right;
    return 0;
}

static int lpr_manifest_array_bytes(uint64_t count, uint64_t width, uint64_t *out)
{
    if (out == NULL || (width != 0 && count > UINT64_MAX / width)) {
        return -1;
    }
    *out = count * width;
    return 0;
}

int lpr_manifest_layout(
    uint64_t entry_count,
    uint64_t ofd_count,
    uint64_t capability_count,
    uint64_t record_bytes,
    lpr_manifest_layout_t *out)
{
    uint64_t entries = 0;
    uint64_t ofds = 0;
    uint64_t capabilities = 0;
    if (out == NULL ||
        lpr_manifest_array_bytes(entry_count, sizeof(lpr_manifest_entry_t), &entries) != 0 ||
        lpr_manifest_array_bytes(ofd_count, sizeof(lpr_manifest_ofd_t), &ofds) != 0 ||
        lpr_manifest_array_bytes(capability_count, sizeof(lpr_manifest_capability_t), &capabilities) != 0)
    {
        return -1;
    }
    out->entry_offset = sizeof(lpr_manifest_t);
    if (lpr_manifest_add(out->entry_offset, entries, &out->ofd_offset) != 0 ||
        lpr_manifest_add(out->ofd_offset, ofds, &out->capability_offset) != 0 ||
        lpr_manifest_add(out->capability_offset, capabilities, &out->record_offset) != 0 ||
        lpr_manifest_add(out->record_offset, record_bytes, &out->byte_size) != 0)
    {
        return -1;
    }
    return 0;
}

int lpr_manifest_begin(
    void *memory,
    uint64_t capacity,
    const lpr_manifest_layout_t *layout,
    uint64_t entry_count,
    uint64_t ofd_count,
    uint64_t capability_count,
    uint64_t record_bytes)
{
    if (memory == NULL || layout == NULL || layout->byte_size > capacity) {
        return -1;
    }
    uint8_t *bytes = memory;
    for (uint64_t i = 0; i < layout->byte_size; ++i) {
        bytes[i] = 0;
    }
    lpr_manifest_t *manifest = memory;
    manifest->magic = LPR_MANIFEST_MAGIC;
    manifest->image_abi_version = LPR_IMAGE_ABI_VERSION;
    manifest->byte_size = layout->byte_size;
    manifest->entry_offset = layout->entry_offset;
    manifest->entry_count = entry_count;
    manifest->ofd_offset = layout->ofd_offset;
    manifest->ofd_count = ofd_count;
    manifest->capability_offset = layout->capability_offset;
    manifest->capability_count = capability_count;
    manifest->record_offset = layout->record_offset;
    manifest->record_bytes = record_bytes;
    manifest->cwd_capability_index = UINT64_MAX;
    return 0;
}

uint64_t lpr_manifest_checksum(const void *memory, uint64_t byte_size)
{
    const uint8_t *bytes = memory;
    uint64_t hash = 1469598103934665603ull;
    if (bytes == NULL) {
        return 0;
    }
    const uint64_t checksum_begin = offsetof(lpr_manifest_t, checksum);
    const uint64_t checksum_end = checksum_begin + sizeof(((lpr_manifest_t *)0)->checksum);
    for (uint64_t i = 0; i < byte_size; ++i) {
        const uint8_t value = i >= checksum_begin && i < checksum_end ? 0 : bytes[i];
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

static int lpr_manifest_range(
    uint64_t offset,
    uint64_t count,
    uint64_t width,
    uint64_t byte_size)
{
    uint64_t bytes = 0;
    return lpr_manifest_array_bytes(count, width, &bytes) == 0 &&
        offset <= byte_size && bytes <= byte_size - offset;
}

int lpr_manifest_seal(lpr_manifest_t *manifest, uint64_t capacity)
{
    if (manifest == NULL || manifest->byte_size > capacity ||
        !lpr_manifest_validate(manifest, capacity)) {
        return -1;
    }
    manifest->checksum = lpr_manifest_checksum(manifest, manifest->byte_size);
    return 0;
}

int lpr_manifest_validate(const void *memory, uint64_t capacity)
{
    const lpr_manifest_t *manifest = memory;
    if (manifest == NULL || capacity < sizeof(*manifest) ||
        manifest->magic != LPR_MANIFEST_MAGIC ||
        manifest->image_abi_version != LPR_IMAGE_ABI_VERSION ||
        manifest->byte_size < sizeof(*manifest) || manifest->byte_size > capacity ||
        !lpr_manifest_range(manifest->entry_offset, manifest->entry_count,
            sizeof(lpr_manifest_entry_t), manifest->byte_size) ||
        !lpr_manifest_range(manifest->ofd_offset, manifest->ofd_count,
            sizeof(lpr_manifest_ofd_t), manifest->byte_size) ||
        !lpr_manifest_range(manifest->capability_offset, manifest->capability_count,
            sizeof(lpr_manifest_capability_t), manifest->byte_size) ||
        manifest->record_offset > manifest->byte_size ||
        manifest->record_bytes > manifest->byte_size - manifest->record_offset)
    {
        return 0;
    }
    if (manifest->checksum != 0 &&
        manifest->checksum != lpr_manifest_checksum(manifest, manifest->byte_size)) {
        return 0;
    }
    const lpr_manifest_entry_t *entries =
        (const lpr_manifest_entry_t *)((const uint8_t *)manifest + manifest->entry_offset);
    const lpr_manifest_ofd_t *ofds =
        (const lpr_manifest_ofd_t *)((const uint8_t *)manifest + manifest->ofd_offset);
    const lpr_manifest_capability_t *capabilities =
        (const lpr_manifest_capability_t *)((const uint8_t *)manifest +
            manifest->capability_offset);
    if ((manifest->cwd_handle == 0 &&
            manifest->cwd_capability_index != UINT64_MAX) ||
        (manifest->cwd_handle != 0 &&
            manifest->cwd_capability_index >= manifest->capability_count))
        return 0;
    for (uint64_t i = 0; i < manifest->capability_count; ++i) {
        if (capabilities[i].ordinal != i ||
            (capabilities[i].flags & ~LPR_MANIFEST_CAPABILITY_CWD_LEASE) != 0 ||
            ((capabilities[i].flags & LPR_MANIFEST_CAPABILITY_CWD_LEASE) != 0) !=
                (i == manifest->cwd_capability_index))
            return 0;
    }
    for (uint64_t i = 0; i < manifest->entry_count; ++i) {
        if (entries[i].state != LPR_MANIFEST_ENTRY_OPEN ||
            entries[i].ofd_index >= manifest->ofd_count ||
            ofds[entries[i].ofd_index].generation != entries[i].ofd_generation ||
            (i != 0 && entries[i].fd <= entries[i - 1u].fd)) {
            return 0;
        }
    }
    for (uint64_t i = 0; i < manifest->ofd_count; ++i) {
        const lpr_manifest_ofd_t *ofd = &ofds[i];
        if (ofd->backend_id == 0 || ofd->generation == 0 ||
            ofd->record_offset > manifest->record_bytes ||
            ofd->record_bytes > manifest->record_bytes - ofd->record_offset ||
            ofd->capability_first > manifest->capability_count ||
            ofd->capability_count > manifest->capability_count - ofd->capability_first) {
            return 0;
        }
    }
    return 1;
}
