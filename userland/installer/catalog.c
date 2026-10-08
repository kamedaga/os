/* SPDX-License-Identifier: MIT */
#include "catalog.h"

#include <errno.h>
#include <string.h>

static int catalog_read_at(void *context, uint64_t offset,
    void *buffer, size_t bytes) {
    struct installer_block_target *target = context;
    if (!target || !buffer || !target->disk.logical_block_size ||
        offset % target->disk.logical_block_size ||
        bytes % target->disk.logical_block_size ||
        offset > target->disk.bytes || bytes > target->disk.bytes - offset)
        return -EINVAL;
    uint8_t *cursor = buffer;
    while (bytes) {
        const size_t chunk = bytes > KB2_BLOCK_DEVICE_MAX_TRANSFER ?
            KB2_BLOCK_DEVICE_MAX_TRANSFER : bytes;
        int status = installer_block_read(target, offset, cursor, chunk);
        if (status) return status;
        offset += chunk;
        cursor += chunk;
        bytes -= chunk;
    }
    return 0;
}

int installer_catalog_scan(struct installer_block_source *sources,
    size_t source_count, struct installer_catalog_entry *entries,
    size_t capacity, size_t *count, size_t *media_index) {
    if (!sources || !source_count || !entries || !capacity || !count ||
        !media_index) return -EINVAL;
    size_t used = 0, media = 0, markers = 0;
    for (size_t source_index = 0; source_index < source_count; ++source_index) {
        struct installer_block_source *source = &sources[source_index];
        uint64_t cursor = 0;
        do {
            kb2_block_disk_t disks[KB2_BLOCK_DEVICE_MAX_DISKS];
            size_t found = 0;
            uint64_t next = 0;
            int status = installer_block_list(source, cursor, disks,
                KB2_BLOCK_DEVICE_MAX_DISKS, &found, &next);
            if (status) return status;
            if (found > capacity - used || (next && next <= cursor))
                return -EOVERFLOW;
            for (size_t i = 0; i < found; ++i) {
                struct installer_catalog_entry *entry = &entries[used];
                memset(entry, 0, sizeof(*entry));
                entry->target = (struct installer_block_target){
                    .source = source, .generation = source->generation,
                    .disk = disks[i]};
                memcpy(entry->name, disks[i].name, sizeof(entry->name));
                entry->name[sizeof(entry->name) - 1] = 0;
                entry->display = (struct installer_disk){
                    .name = entry->name,
                    .model = "model unavailable",
                    .resource_id = source->resource_id,
                    .generation = source->generation,
                    .bytes = disks[i].bytes,
                    .diskseq = disks[i].diskseq,
                    .major = disks[i].major,
                    .minor = disks[i].minor,
                    .logical_block = disks[i].logical_block_size,
                    .read_at = catalog_read_at,
                    .read_context = &entry->target};
                entry->gpt_status = installer_read_gpt(catalog_read_at,
                    &entry->target, disks[i].bytes,
                    disks[i].logical_block_size, &entry->gpt);
                if (!entry->gpt_status) {
                    entry->display.gpt = &entry->gpt;
                    if (entry->gpt.install_media) {
                        media = used;
                        ++markers;
                    }
                } else if (entry->gpt_status == -EINVAL &&
                    !installer_verify_blank(catalog_read_at,
                        &entry->target, disks[i].bytes,
                        disks[i].logical_block_size)) {
                    entry->display.blank_verified = 1;
                }
                ++used;
            }
            cursor = next;
        } while (cursor);
    }
    *count = used;
    if (markers != 1) return markers ? -EEXIST : -ENODEV;
    *media_index = media;
    return 0;
}
