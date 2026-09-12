/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_IPC_H
#define PACHA_KOBOX_IPC_H

#include <pacha/ipc.h>

#define PH_IPC_NO_FD UINT64_MAX
#define PH_IPC_CHANNEL_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_SEND | \
    PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL)

/* Native bootstrap/capability exchange, not the GPU command wire format.
 * operation/correlation/value are interpreted by the service above this port.
 * Native FD numbers occur only in ancillary data, never in shared GPU memory.
 * A packet owns its received FDs. To take one, replace its fd with NO_FD.
 * Outgoing FDs remain caller-owned except for successful MOVE entries. */
struct ph_ipc_packet {
    uint64_t operation, generation, correlation, value;
    size_t fd_count;
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
};

struct ph_ipc {
    uint64_t generation;
    int fd;
    unsigned int admitted;
    /* Rejected received capabilities whose close has not succeeded yet. */
    struct ph_ipc_packet rejected;
};

/* Single owner, no concurrent calls, closes, packet mutation or unmapping of
 * syscall buffers. Zero-initialize the endpoint. Successful init takes the FD;
 * failed init leaves it with the caller. No hidden allocation/thread/TLS use.
 * After destroy, generation remains a watermark: reuse needs a greater value
 * and a fresh channel. The controller must never recycle generation numbers
 * or old channel objects, even when native FD numbers are recycled. */
int ph_ipc_init(struct ph_ipc *ipc, int fd, uint64_t generation);
int ph_ipc_send(struct ph_ipc *ipc, struct ph_ipc_packet *packet);
/* out must have fd_count == 0; failures leave it untouched. Native receive
 * errors do not confer FD ownership, including partial copyout failures. */
int ph_ipc_receive(struct ph_ipc *ipc, uint64_t generation, struct ph_ipc_packet *out);
int ph_ipc_revoke(struct ph_ipc *ipc, uint64_t generation);
/* Destroy also retries rejected-FD cleanup. Errors retain remaining ownership.
 * Successful destroy is idempotent for its generation, including after FD reuse. */
int ph_ipc_destroy(struct ph_ipc *ipc, uint64_t generation);
/* Only use for FDs owned by this packet, not borrowed outgoing SHARE entries.
 * Closes every owned entry, preserves failed ones, never retries a closed FD. */
int ph_ipc_packet_release(struct ph_ipc_packet *packet);

#endif
