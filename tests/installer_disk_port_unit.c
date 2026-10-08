/* SPDX-License-Identifier: MIT */
#include "../userland/installer/disk_port.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t disk[128 * 1024];
static unsigned writes, flushes;

int installer_block_read(struct installer_block_target *target,
    uint64_t offset, void *bytes, size_t length) {
    const size_t block = target->disk.logical_block_size;
    if (offset % block || length % block ||
        offset > sizeof(disk) || length > sizeof(disk) - offset)
        return -EINVAL;
    memcpy(bytes, disk + offset, length);
    return 0;
}

int installer_block_write(struct installer_block_target *target,
    uint64_t offset, const void *bytes, size_t length) {
    const size_t block = target->disk.logical_block_size;
    if (offset % block || length % block ||
        offset > sizeof(disk) || length > sizeof(disk) - offset)
        return -EINVAL;
    memcpy(disk + offset, bytes, length);
    writes++;
    return 0;
}

int installer_block_flush(struct installer_block_target *target) {
    (void)target;
    flushes++;
    return 0;
}

int main(void) {
    struct installer_block_target target = {.disk = {
        .bytes = sizeof(disk), .logical_block_size = 4096}};
    struct installer_disk_port port = {.target = &target,
        .writable_first = 16 * 4096, .writable_end = 32 * 4096};
    struct installer_disk_port_page page = {
        .magic = INSTALLER_DISK_PORT_MAGIC,
        .operation = INSTALLER_DISK_PORT_INFO};
    memset(disk, 0xa5, sizeof(disk));
    assert(installer_disk_port_dispatch(&port, &page) == 0);
    assert(page.disk_bytes == sizeof(disk) && page.logical_block == 4096);
    assert(page.writable_first == 16 * 4096 &&
        page.writable_end == 32 * 4096);
    page.operation = INSTALLER_DISK_PORT_WRITE;
    page.offset = port.writable_first - 1;
    page.length = 2;
    memset(page.data, 0x5a, page.length);
    assert(installer_disk_port_dispatch(&port, &page) == -EACCES);
    assert(writes == 0 && disk[page.offset] == 0xa5);
    page.offset = port.writable_first + 7;
    page.length = 9000;
    memset(page.data, 0x5a, page.length);
    assert(installer_disk_port_dispatch(&port, &page) == 0);
    assert(page.transferred == 9000 && writes >= 2);
    assert(disk[port.writable_first] == 0xa5);
    assert(disk[page.offset] == 0x5a);
    assert(disk[page.offset + 8999] == 0x5a);
    assert(disk[page.offset + 9000] == 0xa5);
    page.operation = INSTALLER_DISK_PORT_READ;
    page.offset = port.writable_first + 7;
    page.length = 9000;
    memset(page.data, 0, page.length);
    assert(installer_disk_port_dispatch(&port, &page) == 0);
    for (unsigned i = 0; i < 9000; ++i) assert(page.data[i] == 0x5a);
    page.operation = INSTALLER_DISK_PORT_WRITE;
    page.offset = port.writable_end - 1;
    page.length = 2;
    assert(installer_disk_port_dispatch(&port, &page) == -EACCES);
    page.operation = INSTALLER_DISK_PORT_FLUSH;
    page.offset = 0;
    page.length = 0;
    assert(installer_disk_port_dispatch(&port, &page) == 0 && flushes == 1);
    port.writable_first = port.writable_end = 0;
    page.operation = INSTALLER_DISK_PORT_WRITE;
    page.offset = 16 * 4096;
    page.length = 4096;
    assert(installer_disk_port_dispatch(&port, &page) == -EACCES);

    page = (struct installer_disk_port_page){
        .magic = INSTALLER_DISK_PORT_MAGIC,
        .operation = INSTALLER_DISK_PORT_PROGRESS,
        .length = sizeof(struct installer_progress_wire)};
    struct installer_progress_wire progress = {
        .magic = INSTALLER_PROGRESS_MAGIC,
        .completed_bytes = 1024,
        .total_bytes = 4096,
        .phase = INSTALLER_PROGRESS_PREFLIGHT_ROOTFS,
    };
    memcpy(page.data, &progress, sizeof(progress));
    assert(installer_disk_port_dispatch(&port, &page) == -EACCES);
    port.allow_progress = 1;
    assert(installer_disk_port_dispatch(&port, &page) == 0);
    progress.completed_bytes = 4097;
    memcpy(page.data, &progress, sizeof(progress));
    assert(installer_disk_port_dispatch(&port, &page) == -EINVAL);
    progress.completed_bytes = 1024;
    progress.reserved = 1;
    memcpy(page.data, &progress, sizeof(progress));
    assert(installer_disk_port_dispatch(&port, &page) == -EINVAL);

    /* A partition view exposes offset zero to an unmodified formatter while
     * translating every transfer into the selected extent of the same disk. */
    port.visible_first = 8 * 4096;
    port.visible_end = 24 * 4096;
    port.writable_first = 0;
    port.writable_end = 16 * 4096;
    page = (struct installer_disk_port_page){
        .magic = INSTALLER_DISK_PORT_MAGIC,
        .operation = INSTALLER_DISK_PORT_INFO};
    assert(installer_disk_port_dispatch(&port, &page) == 0);
    assert(page.disk_bytes == 16 * 4096 && page.writable_first == 0 &&
        page.writable_end == 16 * 4096);
    page.operation = INSTALLER_DISK_PORT_WRITE;
    page.offset = 3;
    page.length = 5;
    memset(page.data, 0x3c, 5);
    assert(installer_disk_port_dispatch(&port, &page) == 0);
    assert(!memcmp(disk + 8 * 4096 + 3, page.data, 5));
    assert(disk[3] != 0x3c);
    puts("installer selected-disk data port passed");
    return 0;
}
