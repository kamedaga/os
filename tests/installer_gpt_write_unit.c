/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "../userland/installer/gpt_write.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct test_disk {
    int fd;
    uint64_t bytes;
    int writes;
    int fail_write;
    int corrupt_readback;
};

struct progress_state { unsigned calls, last_percent; };

static void progress(void *context, unsigned percent, const char *phase) {
    struct progress_state *state = context;
    assert(state && phase && *phase && percent >= state->last_percent);
    state->last_percent = percent;
    ++state->calls;
}

static int read_at(void *context, uint64_t offset, void *buffer, size_t bytes) {
    struct test_disk *disk = context;
    if (offset > disk->bytes || bytes > disk->bytes - offset) return -EIO;
    if (pread(disk->fd, buffer, bytes, (off_t)offset) != (ssize_t)bytes)
        return -EIO;
    if (disk->corrupt_readback && disk->writes && offset == 512)
        ((uint8_t *)buffer)[0] ^= 1;
    return 0;
}

static int write_at(void *context, uint64_t offset,
    const void *buffer, size_t bytes) {
    struct test_disk *disk = context;
    if (disk->fail_write) return -EIO;
    if (offset > disk->bytes || bytes > disk->bytes - offset) return -EIO;
    if (pwrite(disk->fd, buffer, bytes, (off_t)offset) != (ssize_t)bytes)
        return -EIO;
    disk->writes++;
    return 0;
}

static int flush(void *context) {
    return fsync(((struct test_disk *)context)->fd) ? -EIO : 0;
}

static void run(uint32_t block) {
    FILE *file = tmpfile();
    assert(file);
    struct test_disk disk = {.fd = fileno(file), .bytes = UINT64_C(2) << 30};
    assert(ftruncate(disk.fd, disk.bytes) == 0);
    struct installer_plan plan = {0};
    uint8_t blank[16] = {0};
    assert(installer_guided_plan(9, disk.bytes, block, blank, &plan) == 0);
    struct installer_new_guids ids = {.disk = {1}, .esp = {2}, .root = {3}};
    plan.root_filesystem = INSTALLER_FS_FAT32;
    assert(installer_write_guided_gpt(&plan, 9, disk.bytes, block, 1,
        &ids, read_at, write_at, flush, &disk, NULL, NULL) == -EINVAL);
    assert(disk.writes == 0);
    plan.root_filesystem = INSTALLER_FS_EXT4;
    assert(installer_write_guided_gpt(&plan, 9, disk.bytes, block, 0,
        &ids, read_at, write_at, flush, &disk, NULL, NULL) == -EINVAL);
    assert(disk.writes == 0);
    assert(installer_write_guided_gpt(&plan, 10, disk.bytes, block, 1,
        &ids, read_at, write_at, flush, &disk, NULL, NULL) == -ESTALE);
    assert(disk.writes == 0);
    struct progress_state progress_state = {0};
    assert(installer_write_guided_gpt(&plan, 9, disk.bytes, block, 1,
        &ids, read_at, write_at, flush, &disk,
        progress, &progress_state) == 0);
    assert(progress_state.calls >= 8 && progress_state.last_percent == 29);
    assert(disk.writes == 5);
    struct installer_gpt gpt = {0};
    assert(installer_read_gpt(read_at, &disk, disk.bytes, block, &gpt) == 0);
    assert(gpt.partition_count == 2 && !gpt.install_media);
    assert(installer_guid_equal(gpt.disk_guid, ids.disk));
    assert(gpt.partitions[0].first_lba == plan.esp.first_lba);
    assert(gpt.partitions[1].first_lba == plan.root.first_lba);
    int prior_writes = disk.writes;
    assert(installer_write_guided_gpt(&plan, 9, disk.bytes, block, 1,
        &ids, read_at, write_at, flush, &disk, NULL, NULL) == -ESTALE);
    assert(disk.writes == prior_writes);
    fclose(file);
}

int main(void) {
    run(512);
    run(4096);
    puts("guided GPT writes and readback verified on sparse test disks");
    return 0;
}
