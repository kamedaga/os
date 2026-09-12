/* SPDX-License-Identifier: MIT */
#include "ipc.h"
#include <pacha/syscall.h>
#include <errno.h>

#define PH_IPC_FD_FLAGS (PACHA_FD_FLAG_CLOEXEC | PACHA_FD_FLAG_NONBLOCK | \
    PACHA_FD_FLAG_INHERIT | PACHA_FD_FLAG_PRIVATE)
#define PH_IPC_TRANSFER_FLAGS (PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_CLOEXEC | \
    PACHA_IPC_TRANSFER_NONBLOCK | PACHA_IPC_TRANSFER_INHERIT | PACHA_IPC_TRANSFER_PRIVATE)

static int native_status(long result) {
    switch (result) {
    case 0: return 0;
    case PACHA_SYSCALL_ERR_INVALID: return -EINVAL;
    case PACHA_SYSCALL_ERR_NOT_READY:
    case PACHA_SYSCALL_ERR_EMPTY: return -EAGAIN;
    case PACHA_SYSCALL_ERR_ALLOC: return -ENOMEM;
    case PACHA_SYSCALL_ERR_MAP: return -EIO;
    case PACHA_SYSCALL_ERR_CLOSED: return -EPIPE;
    default: return -EPROTO;
    }
}

static int valid_fd(uint64_t fd) {
    return fd >= 16 && fd < PACHA_FD_TABLE_LIMIT;
}

static int check_generation(const struct ph_ipc *ipc, uint64_t generation) {
    if (!ipc || !generation) return -EINVAL;
    return ipc->generation == generation ? 0 : -ESTALE;
}

static int check_admission(const struct ph_ipc *ipc, uint64_t generation) {
    int result = check_generation(ipc, generation);
    if (result) return result;
    return ipc->admitted ? 0 : -ESHUTDOWN;
}

int ph_ipc_init(struct ph_ipc *ipc, int fd, uint64_t generation) {
    struct pacha_fd_info info = {0};

    if (!ipc || !valid_fd((uint64_t)fd) || !generation) return -EINVAL;
    if (ipc->fd || ipc->admitted || ipc->rejected.fd_count) return -EBUSY;
    if (generation <= ipc->generation) return -ESTALE;
    int result = native_status(pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,
        (uint64_t)fd, (uintptr_t)&info));
    if (result) return result;
    if (info.kind != PACHA_FD_KIND_CHANNEL) return -ENOTSOCK;
    if ((info.rights & PH_IPC_CHANNEL_RIGHTS) != PH_IPC_CHANNEL_RIGHTS) return -EACCES;
    ipc->generation = generation;
    ipc->fd = fd;
    ipc->admitted = 1;
    return 0;
}

static int validate_send(const struct ph_ipc *ipc, const struct ph_ipc_packet *packet) {
    if (!packet->operation || packet->fd_count > PACHA_IPC_MAX_TRANSFER_FDS) return -EINVAL;
    for (size_t i = 0; i < packet->fd_count; ++i) {
        const struct pacha_ipc_fd *entry = &packet->fds[i];
        if (!valid_fd(entry->fd) || entry->flags & ~(uint64_t)PH_IPC_FD_FLAGS ||
            entry->transfer_flags & ~(uint64_t)PH_IPC_TRANSFER_FLAGS) return -EINVAL;
        /* Moving our own endpoint would invalidate the post-enqueue native
         * wake lookup and violate this object's exclusive FD ownership. */
        if (entry->fd == (uint64_t)ipc->fd &&
            entry->transfer_flags & PACHA_IPC_TRANSFER_MOVE) return -EINVAL;
        for (size_t j = 0; j < i; ++j) {
            if (entry->fd == packet->fds[j].fd &&
                (entry->transfer_flags | packet->fds[j].transfer_flags) &
                    PACHA_IPC_TRANSFER_MOVE) return -EINVAL;
        }
    }
    /* Native IPC verifies source TRANSFER, rights attenuation and object
     * transferability atomically with enqueue. Do not add a racy GET_INFO
     * check or demand INSPECT on capabilities that need not grant it. */
    return 0;
}

int ph_ipc_send(struct ph_ipc *ipc, struct ph_ipc_packet *packet) {
    if (!packet) return -EINVAL;
    int result = check_admission(ipc, packet->generation);
    if (result) return result;
    result = validate_send(ipc, packet);
    if (result) return result;
    struct pacha_ipc_msg message = {
        .word0 = packet->operation, .word1 = packet->generation,
        .word2 = packet->correlation, .word3 = packet->value,
        .fds = packet->fds, .fd_count = packet->fd_count,
    };
    result = native_status(pacha_syscall2(PACHA_IPC_SYSCALL_SEND,
        (uint64_t)ipc->fd, (uintptr_t)&message));
    if (result) return result;
    for (size_t i = 0; i < packet->fd_count; ++i) {
        if (packet->fds[i].transfer_flags & PACHA_IPC_TRANSFER_MOVE)
            packet->fds[i].fd = PH_IPC_NO_FD;
    }
    return 0;
}

int ph_ipc_packet_release(struct ph_ipc_packet *packet) {
    if (!packet || packet->fd_count > PACHA_IPC_MAX_TRANSFER_FDS) return -EINVAL;
    int first_error = 0;
    for (size_t i = 0; i < packet->fd_count; ++i) {
        uint64_t fd = packet->fds[i].fd;
        if (fd == PH_IPC_NO_FD) continue;
        int result = valid_fd(fd) ?
            native_status(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, fd)) : -EINVAL;
        if (result) {
            if (!first_error) first_error = result;
            continue;
        }
        /* Also handles repeated outgoing SHARE references owned by a caller. */
        for (size_t j = i; j < packet->fd_count; ++j) {
            if (packet->fds[j].fd == fd) packet->fds[j].fd = PH_IPC_NO_FD;
        }
    }
    if (!first_error) *packet = (struct ph_ipc_packet){0};
    return first_error;
}

int ph_ipc_receive(struct ph_ipc *ipc, uint64_t generation, struct ph_ipc_packet *out) {
    int result = check_admission(ipc, generation);
    if (result) return result;
    if (!out || out == &ipc->rejected) return -EINVAL;
    if (out->fd_count) return -EBUSY;
    /* Staging belongs to the endpoint until validation succeeds. It is always
     * large enough for the native maximum, even for a rejected peer message. */
    struct ph_ipc_packet *packet = &ipc->rejected;
    struct pacha_ipc_msg message = {
        .fds = packet->fds, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS,
    };
    result = native_status(pacha_syscall2(PACHA_IPC_SYSCALL_RECV,
        (uint64_t)ipc->fd, (uintptr_t)&message));
    if (result) {
        /* On failure native IPC rolls back installed FDs. Do not close any
         * numbers left by a partial copyout: they may have been recycled. */
        *packet = (struct ph_ipc_packet){0};
        return result;
    }
    /* fd_count and ancillary metadata are kernel-generated, not peer bytes.
     * The native ABI guarantees count <= capacity and unique installed FDs. */
    packet->fd_count = message.fd_count;
    packet->operation = message.word0;
    packet->generation = message.word1;
    packet->correlation = message.word2;
    packet->value = message.word3;
    if (packet->generation != generation || !packet->operation) {
        ipc->admitted = 0;
        result = packet->generation != generation ? -ESTALE : -EPROTO;
        int cleanup = ph_ipc_packet_release(packet);
        return cleanup ? cleanup : result;
    }
    *out = *packet;
    *packet = (struct ph_ipc_packet){0};
    return 0;
}

int ph_ipc_revoke(struct ph_ipc *ipc, uint64_t generation) {
    int result = check_generation(ipc, generation);
    if (result) return result;
    ipc->admitted = 0;
    return 0;
}

int ph_ipc_destroy(struct ph_ipc *ipc, uint64_t generation) {
    int result = ph_ipc_revoke(ipc, generation);
    if (result) return result;
    result = ph_ipc_packet_release(&ipc->rejected);
    /* Closing the endpoint drops queued capability references, even when a
     * rejected installed FD needs another cleanup attempt. */
    if (ipc->fd) {
        int close_result = native_status(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE,
            (uint64_t)ipc->fd));
        if (!close_result) ipc->fd = 0;
        if (!result) result = close_result;
    }
    return result;
}
