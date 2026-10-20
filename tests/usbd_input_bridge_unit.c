/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "../userland/usbd/input_bridge.c"

static struct ph_ipc_packet incoming;
static struct pacha_fd_info event_info;
static unsigned released, closed;

int ph_ipc_receive(struct ph_ipc *ipc, uint64_t generation, struct ph_ipc_packet *reply) {
    assert(ipc->generation == generation); *reply = incoming; return 0;
}
int ph_ipc_packet_release(struct ph_ipc_packet *packet) {
    for (size_t i = 0; i < packet->fd_count; ++i)
        if (packet->fds[i].fd != PH_IPC_NO_FD) released++;
    *packet = (struct ph_ipc_packet){0}; return 0;
}
long pacha_fd_wait_many(struct pacha_pollfd *events, size_t count, uint64_t ticks) {
    (void)events; (void)count; (void)ticks; assert(0); return -1;
}
long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t address) {
    assert(nr == PACHA_FD_SYSCALL_GET_INFO && fd == 40);
    *(struct pacha_fd_info *)(uintptr_t)address = event_info; return 0;
}
int pacha_fd_close(pacha_fd_t fd) { assert(fd == 40); closed++; return 0; }
int pacha_munmap(void *address, size_t size) {
    (void)address; (void)size; assert(0); return 0;
}

int main(void) {
    struct usbd_input_bridge bridge = {0};
    struct ph_ipc ipc = {.generation = 7};
    incoming = (struct ph_ipc_packet){.operation = PH_USB_INPUT_ATTACH,
        .generation = 7, .correlation = 1, .fd_count = 1, .fds = {{.fd = 40}}};
    event_info = (struct pacha_fd_info){.kind = PACHA_FD_KIND_EVENT,
        .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL};
    event_info.kind = PACHA_FD_KIND_VMO;
    assert(wait_reply(&bridge, &ipc, 30, PH_USB_INPUT_ATTACH, 1) == -EPROTO);
    assert(released == 1 && !bridge.event_fd);
    event_info.kind = PACHA_FD_KIND_EVENT;
    event_info.rights |= PACHA_FD_RIGHT_WRITE;
    assert(wait_reply(&bridge, &ipc, 30, PH_USB_INPUT_ATTACH, 1) == -EPROTO);
    assert(released == 2 && !bridge.event_fd);
    event_info.rights &= ~PACHA_FD_RIGHT_WRITE;
    incoming.correlation = 2;
    assert(wait_reply(&bridge, &ipc, 30, PH_USB_INPUT_ATTACH, 1) == -EPROTO);
    assert(released == 3 && !bridge.event_fd);
    incoming.correlation = 1;
    assert(wait_reply(&bridge, &ipc, 30, PH_USB_INPUT_ATTACH, 1) == -EPROTO);
    assert(released == 4 && !bridge.event_fd);
    event_info.flags = PACHA_FD_FLAG_NONBLOCK;
    assert(!wait_reply(&bridge, &ipc, 30, PH_USB_INPUT_ATTACH, 1));
    assert(bridge.event_fd == 40 && released == 4);
    incoming.operation = PH_USB_INPUT_READ;
    assert(wait_reply(&bridge, &ipc, 30, PH_USB_INPUT_READ, 1) == -EPROTO);
    assert(released == 5);
    incoming.fd_count = 0;
    assert(!wait_reply(&bridge, &ipc, 30, PH_USB_INPUT_READ, 1));
    assert(!usbd_input_bridge_close(&bridge) && closed == 1);
    assert(!usbd_input_bridge_close(&bridge) && closed == 1);
    puts("input bridge: capability kind/rights, correlation, ancillary ownership and close passed");
    return 0;
}
