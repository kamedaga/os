/* SPDX-License-Identifier: MIT */
#include "bootstrap.h"
#include <pacha/syscall.h>
#include <errno.h>

#define PH_BOOTSTRAP_METADATA_LIMIT (16u * 1024u * 1024u)
#define PH_BOOTSTRAP_ARTIFACT_LIMIT (128u * 1024u * 1024u)

static int native_status(long status) {
    switch (status) {
    case 0: return 0;
    case PACHA_SYSCALL_ERR_INVALID: return -EINVAL;
    case PACHA_SYSCALL_ERR_ALLOC: return -ENOMEM;
    case PACHA_SYSCALL_ERR_MAP: return -EIO;
    default: return -EPROTO;
    }
}

static int counts_valid(size_t artifacts, size_t resources) {
    return artifacts && artifacts <= PH_BOOTSTRAP_MAX_ARTIFACTS &&
        resources <= PH_BOOTSTRAP_MAX_RESOURCES;
}

static int item_size_valid(size_t index, size_t artifacts, uint64_t size) {
    if (index >= 2 + artifacts) return size == 0;
    const uint64_t limit = index < 2 ? PH_BOOTSTRAP_METADATA_LIMIT : PH_BOOTSTRAP_ARTIFACT_LIMIT;
    return size && size <= limit;
}

static int endpoint_ready(const struct ph_ipc *ipc, uint64_t generation) {
    if (!ipc || !generation) return -EINVAL;
    if (ipc->generation != generation) return -ESTALE;
    return ipc->admitted ? 0 : -ESHUTDOWN;
}

int ph_bootstrap_sender_init(struct ph_bootstrap_sender *sender, uint64_t generation,
    const struct ph_bootstrap_item *items, size_t artifact_count, size_t resource_count) {
    if (!sender || sender->generation || !generation || !items ||
        !counts_valid(artifact_count, resource_count)) return -EINVAL;
    for (size_t i = 0; i < 2 + artifact_count + resource_count; ++i) {
        const struct pacha_ipc_fd *cap = &items[i].capability;
        if (cap->fd < 16 || cap->fd >= PACHA_FD_TABLE_LIMIT ||
            !item_size_valid(i, artifact_count, items[i].size)) return -EINVAL;
        /* Descriptors are borrowed and cannot be moved by this transaction. */
        if (cap->transfer_flags || cap->flags != PACHA_FD_FLAG_CLOEXEC) return -EINVAL;
        if (i < 2 + artifact_count && cap->rights != PH_BOOTSTRAP_BLOB_RIGHTS) return -EACCES;
    }
    *sender = (struct ph_bootstrap_sender){.generation = generation, .items = items,
        .artifact_count = artifact_count, .resource_count = resource_count};
    return 0;
}

int ph_bootstrap_send_next(struct ph_bootstrap_sender *sender, struct ph_ipc *ipc) {
    if (!sender) return -EINVAL;
    int result = endpoint_ready(ipc, sender->generation);
    if (result) return result;
    if (sender->finished) return -EALREADY;
    const size_t count = 2 + sender->artifact_count + sender->resource_count;
    struct ph_ipc_packet packet = {.generation = sender->generation,
        .correlation = sender->next, .operation = PH_BOOTSTRAP_FINISH};
    if (sender->next < count) {
        const struct ph_bootstrap_item *item = &sender->items[sender->next];
        packet.operation = sender->next < 2 + sender->artifact_count ?
            PH_BOOTSTRAP_BLOB : PH_BOOTSTRAP_RESOURCE;
        packet.value = item->size;
        packet.fd_count = 1;
        packet.fds[0] = item->capability;
    }
    result = ph_ipc_send(ipc, &packet);
    if (!result) {
        if (sender->next == count) sender->finished = 1;
        else ++sender->next;
    }
    return result;
}

int ph_bootstrap_receiver_init(struct ph_bootstrap_receiver *receiver, uint64_t generation,
    size_t artifact_count, size_t resource_count) {
    if (!receiver || receiver->generation || !generation ||
        !counts_valid(artifact_count, resource_count)) return -EINVAL;
    *receiver = (struct ph_bootstrap_receiver){.generation = generation,
        .artifact_count = artifact_count, .resource_count = resource_count};
    return 0;
}

int ph_bootstrap_release(struct ph_bootstrap_receiver *receiver) {
    if (!receiver) return -EINVAL;
    if (receiver->generation && !receiver->complete && !receiver->failed) return -EBUSY;
    if (receiver->generation) {
        receiver->complete = 0;
        receiver->failed = 1;
    }
    int result = ph_ipc_packet_release(&receiver->pending);
    for (size_t i = 0; i < receiver->received; ++i) {
        struct ph_bootstrap_item *item = &receiver->items[i];
        if (item->capability.fd == PH_IPC_NO_FD) continue;
        int closed = native_status(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, item->capability.fd));
        if (!closed) item->capability.fd = PH_IPC_NO_FD;
        else if (!result) result = closed;
    }
    if (!result) *receiver = (struct ph_bootstrap_receiver){0};
    return result;
}

int ph_bootstrap_abort(struct ph_bootstrap_receiver *receiver, struct ph_ipc *ipc) {
    if (!receiver || !receiver->generation) return -EINVAL;
    int result = ph_ipc_revoke(ipc, receiver->generation);
    if (result) return result;
    receiver->failed = 1;
    receiver->complete = 0;
    return ph_bootstrap_release(receiver);
}

static int reject_message(struct ph_bootstrap_receiver *receiver, struct ph_ipc *ipc,
    int error) {
    int cleanup = ph_bootstrap_abort(receiver, ipc);
    return cleanup ? cleanup : error;
}

static int validate_blob(const struct ph_ipc_packet *packet) {
    const struct pacha_ipc_fd *cap = &packet->fds[0];
    struct pacha_fd_info info = {0};
    if (cap->rights != PH_BOOTSTRAP_BLOB_RIGHTS) return -EACCES;
    int result = native_status(pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,
        cap->fd, (uintptr_t)&info));
    if (result) return result;
    if (info.kind != PACHA_FD_KIND_VMO) return -ENOTSUP;
    if (packet->value > info.size) return -EMSGSIZE;
    return 0;
}

int ph_bootstrap_receive_next(struct ph_bootstrap_receiver *receiver, struct ph_ipc *ipc) {
    if (!receiver) return -EINVAL;
    int result = endpoint_ready(ipc, receiver->generation);
    if (result) return result;
    if (receiver->complete) return -EALREADY;
    if (receiver->failed) return -ESHUTDOWN;
    struct ph_ipc_packet *packet = &receiver->pending;
    result = ph_ipc_receive(ipc, receiver->generation, packet);
    if ((result == -EAGAIN || result == -ENOMEM) && ipc->admitted) return result;
    if (result) return reject_message(receiver, ipc, result);
    const size_t index = receiver->received;
    const size_t count = 2 + receiver->artifact_count + receiver->resource_count;
    if (packet->correlation != index) return reject_message(receiver, ipc, -EPROTO);
    if (index == count) {
        if (packet->operation != PH_BOOTSTRAP_FINISH || packet->fd_count || packet->value)
            return reject_message(receiver, ipc, -EPROTO);
        receiver->complete = 1;
        *packet = (struct ph_ipc_packet){0};
        return 0;
    }
    const unsigned int operation = index < 2 + receiver->artifact_count ?
        PH_BOOTSTRAP_BLOB : PH_BOOTSTRAP_RESOURCE;
    if (packet->operation != operation || packet->fd_count != 1 ||
        !item_size_valid(index, receiver->artifact_count, packet->value) ||
        packet->fds[0].flags != PACHA_FD_FLAG_CLOEXEC || packet->fds[0].transfer_flags)
        return reject_message(receiver, ipc, -EPROTO);
    if (operation == PH_BOOTSTRAP_BLOB) {
        result = validate_blob(packet);
        if (result) return reject_message(receiver, ipc, result);
    }
    receiver->items[index] = (struct ph_bootstrap_item){
        .capability = packet->fds[0], .size = packet->value};
    ++receiver->received;
    /* Ownership is now in items, not pending. No close at this transition. */
    *packet = (struct ph_ipc_packet){0};
    return 0;
}
