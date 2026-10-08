/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_DISK_PORT_H
#define PACHA_INSTALLER_DISK_PORT_H

#include "block_transport.h"
#include "progress_protocol.h"

#define INSTALLER_DISK_PORT_MAGIC UINT64_C(0x314f49504b534450)
#define INSTALLER_DISK_PORT_BYTES (4096u + KB2_BLOCK_DEVICE_MAX_TRANSFER)

enum installer_disk_port_op {
    INSTALLER_DISK_PORT_ATTACH = 1,
    INSTALLER_DISK_PORT_INFO,
    INSTALLER_DISK_PORT_READ,
    INSTALLER_DISK_PORT_WRITE,
    INSTALLER_DISK_PORT_FLUSH,
    INSTALLER_DISK_PORT_PROGRESS,
};

struct installer_disk_port_page {
    uint64_t magic;
    uint64_t sequence;
    uint64_t offset;
    uint64_t disk_bytes;
    uint64_t writable_first;
    uint64_t writable_end; /* exclusive */
    int64_t status;
    uint32_t operation;
    uint32_t length;
    uint32_t transferred;
    uint32_t logical_block;
    uint8_t data[KB2_BLOCK_DEVICE_MAX_TRANSFER];
};

struct installer_disk_port {
    struct installer_block_target *target;
    /* Optional client-visible subrange. Zero/zero means the complete disk.
     * Offsets returned to the Linux tool remain relative to this view. */
    uint64_t visible_first;
    uint64_t visible_end;
    uint64_t writable_first;
    uint64_t writable_end;
    int allow_progress;
    int fd;
    int page_fd;
    struct installer_disk_port_page *page;
    uint64_t sequence;
    /* The transport page is shared with the Linux client.  Once a reply is
     * sent, that client may immediately reuse the page for close-time flush
     * or its next I/O.  Keep the completed request here before replying so
     * UI progress never observes a later request by accident. */
    int completed_status;
    uint32_t completed_operation;
    uint32_t completed_transferred;
    struct installer_progress_wire completed_progress;
};

_Static_assert(sizeof(struct installer_disk_port_page) <=
    INSTALLER_DISK_PORT_BYTES, "installer disk port page size");

/* The client receives a data-only port for one already confirmed disk. It
 * cannot enumerate devices, select another key, ARM, or widen this range. */
int installer_disk_port_dispatch(struct installer_disk_port *port,
    struct installer_disk_port_page *page);
int installer_disk_port_pump(struct installer_disk_port *port);
void installer_disk_port_close(struct installer_disk_port *port);

#endif
