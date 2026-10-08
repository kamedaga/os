/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "../userland/installer/catalog.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int media_fd;
static uint64_t media_bytes;

int installer_block_list(struct installer_block_source *source,
    uint64_t start, kb2_block_disk_t *disks, size_t capacity,
    size_t *count, uint64_t *next) {
    if (start || !capacity) { *count = 0; *next = 0; return 0; }
    kb2_block_disk_t disk = {.diskseq = 1, .major = 8,
        .minor = source->resource_id == 100 ? 0 : 16,
        .bytes = source->resource_id == 100 ? media_bytes : UINT64_C(2) << 30,
        .logical_block_size = 512,
        .read_only = source->resource_id == 100};
    snprintf(disk.name, sizeof(disk.name), "%s",
        source->resource_id == 100 ? "sda" : "nvme0n1");
    disks[0] = disk;
    *count = 1;
    *next = 0;
    return 0;
}

int installer_block_read(struct installer_block_target *target,
    uint64_t offset, void *buffer, size_t bytes) {
    if (target->source->resource_id != 100) {
        memset(buffer, 0, bytes);
        return 0;
    }
    if (offset > media_bytes || bytes > media_bytes - offset) return -EIO;
    return pread(media_fd, buffer, bytes, (off_t)offset) == (ssize_t)bytes ?
        0 : -EIO;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    media_fd = open(argv[1], O_RDONLY);
    assert(media_fd >= 0);
    struct stat st;
    assert(fstat(media_fd, &st) == 0 && st.st_size > 0);
    media_bytes = (uint64_t)st.st_size;
    struct installer_block_source sources[2] = {
        {.resource_id = 100, .generation = 7},
        {.resource_id = 200, .generation = 9}};
    struct installer_catalog_entry entries[2];
    size_t count = 0, media = 99;
    assert(installer_catalog_scan(sources, 2, entries, 2,
        &count, &media) == 0);
    assert(count == 2 && media == 0);
    assert(entries[0].target.disk.diskseq == 1 &&
        entries[1].target.disk.diskseq == 1);
    assert(entries[0].display.resource_id == 100 &&
        entries[1].display.resource_id == 200);
    assert(entries[0].display.gpt && entries[0].display.gpt->install_media);
    assert(entries[1].display.blank_verified && !entries[1].display.gpt);
    assert(close(media_fd) == 0);
    puts("installer merged catalog and boot-media exclusion passed");
    return 0;
}
