/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_BLOCK_PROTOCOL_H
#define PACHA_INSTALLER_BLOCK_PROTOCOL_H

#include <kobox2/block_device.h>
#include <stddef.h>
#include <stdint.h>

#define INSTALLER_BLOCK_MAGIC UINT64_C(0x314b4c424e495041)
#define INSTALLER_BLOCK_PAGE_BYTES (4096u + KB2_BLOCK_DEVICE_MAX_TRANSFER)

enum installer_block_operation {
    INSTALLER_BLOCK_ATTACH = 1,
    INSTALLER_BLOCK_LIST,
    INSTALLER_BLOCK_READ,
    INSTALLER_BLOCK_WRITE,
    INSTALLER_BLOCK_FLUSH,
    INSTALLER_BLOCK_ARM,
    INSTALLER_BLOCK_DISARM,
};

/* Private native IPC between one Kobox2 storage owner and the installer.
 * One shared page and one synchronous channel serialize operations. A disk
 * key is meaningful only with its source capability and generation. */
struct installer_block_page {
    uint64_t magic;
    uint64_t sequence;
    uint64_t generation;
    uint64_t resource_id;
    uint64_t offset;
    uint64_t next;
    int64_t status;
    uint32_t operation;
    uint32_t count;
    uint32_t length;
    uint32_t reserved;
    kb2_block_disk_t key;
    kb2_block_disk_t disks[KB2_BLOCK_DEVICE_MAX_DISKS];
    uint8_t data[KB2_BLOCK_DEVICE_MAX_TRANSFER];
};

_Static_assert(sizeof(struct installer_block_page) <= INSTALLER_BLOCK_PAGE_BYTES,
    "installer block page exceeds the shared VMO");

#endif
