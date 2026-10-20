#include <pacha/abi.h>
/* SPDX-License-Identifier: MIT */
#ifndef PACHA_USBD_INSTALL_PORT_H
#define PACHA_USBD_INSTALL_PORT_H

#include "../kobox2_adapter/block_client.h"
#include "../installer/block_protocol.h"

struct usbd_install_port {
    pacha_fd_t fd;
    pacha_fd_t shared_fd;
    uint64_t sequence;
    struct installer_block_page *shared;
};

int usbd_install_port_pump(struct usbd_install_port *port,
    struct ph_block_client *block, struct ph_ipc *ipc, pacha_fd_t process_fd,
    uint64_t resource_id, kb2_block_disk_t *write_disk,
    int *restart_requested);
void usbd_install_port_close(struct usbd_install_port *port);

#endif
