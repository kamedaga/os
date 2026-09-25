#include "mapping.h"
#include "../support/syscall.h"
#include <pachaos/abi.h>
#include <errno.h>

static void close_caps(const struct pacha_ipc_fd *caps, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        if (caps[i].fd < 16 || caps[i].fd >= PACHAOS_FD_TABLE_LIMIT) continue;
        unsigned j;
        for (j = 0; j < i && caps[j].fd != caps[i].fd; j++) {}
        if (j == i) (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, caps[i].fd);
    }
}

void lpr_unix_mapping_destroy(struct lpr_unix_mapping *mapping)
{
    if (!mapping) return;
    const void *pages[2] = {
        mapping->outgoing_tx, mapping->incoming_tx,
    };
    for (unsigned i = 0; i < 2; i++) {
        if (pages[i]) (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)pages[i], sizeof(struct unix_endpoint));
        if (mapping->fds[i] >= 16) (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE,
            (uint64_t)(uint32_t)mapping->fds[i]);
    }
    *mapping = (struct lpr_unix_mapping){ .fds = { -1, -1 } };
}

static int check_headers(const struct lpr_unix_mapping *mapping)
{
    const struct unix_tx *tx[2] = { mapping->outgoing_tx, mapping->incoming_tx };
    const struct unix_rx *rx[2] = { mapping->outgoing_rx, mapping->incoming_rx };
    for (unsigned i = 0; i < 2; i++) {
        if (__atomic_load_n(&tx[i]->magic, __ATOMIC_RELAXED) != UNIX_TRANSPORT_MAGIC ||
            __atomic_load_n(&rx[i]->magic, __ATOMIC_RELAXED) != UNIX_TRANSPORT_MAGIC ||
            __atomic_load_n(&tx[i]->capacity, __ATOMIC_RELAXED) != UNIX_TRANSPORT_BYTES ||
            __atomic_load_n(&tx[i]->type, __ATOMIC_RELAXED) != mapping->type) return -EPROTO;
        if (__atomic_load_n(&tx[i]->generation, __ATOMIC_RELAXED) != mapping->generation ||
            __atomic_load_n(&rx[i]->generation, __ATOMIC_RELAXED) != mapping->generation) return -ESTALE;
    }
    return 0;
}

void lpr_unix_route_destroy(struct lpr_unix_route_mapping *mapping)
{
    if (mapping->tx) (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
        (uint64_t)(uintptr_t)mapping->tx, sizeof(struct unix_tx));
    if (mapping->rx) (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
        (uint64_t)(uintptr_t)mapping->rx, sizeof(struct unix_rx));
    for (unsigned i = 0; i < 2; i++)
        if (mapping->fds[i] >= 16) (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, mapping->fds[i]);
    *mapping = (struct lpr_unix_route_mapping){ .fds = {-1, -1} };
}

int lpr_unix_route_import(const struct pacha_ipc_fd *caps, unsigned count,
    uint64_t generation, int writing, struct lpr_unix_route_mapping *out)
{
    *out = (struct lpr_unix_route_mapping){ .fds = {-1, -1} };
    const uint64_t common = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ;
    if (count != 2 || !generation) { close_caps(caps, count); return -EPROTO; }
    for (unsigned i = 0; i < 2; i++) {
        struct pacha_fd_info info;
        uint64_t rights = common | ((i == 0) == !!writing ? PACHA_FD_RIGHT_MAP_WRITE : 0);
        uint64_t size = i ? sizeof(struct unix_rx) : sizeof(struct unix_tx);
        if (caps[i].fd < 16 || caps[i].fd >= PACHAOS_FD_TABLE_LIMIT || caps[0].fd == caps[1].fd ||
            lpr_pacha_syscall2(PACHAOS_SYSCALL_FD_GET_INFO, caps[i].fd, (uint64_t)(uintptr_t)&info) != 0 ||
            info.kind != PACHA_FD_KIND_VMO || info.rights != rights || info.size != size ||
            info.flags != (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC)) {
            close_caps(caps, count); return -EBADF;
        }
    }
    out->fds[0] = caps[0].fd; out->fds[1] = caps[1].fd;
    for (unsigned i = 0; i < 2; i++) {
        uint64_t size = i ? sizeof(struct unix_rx) : sizeof(struct unix_tx);
        uint64_t prot = PACHA_PROT_READ | ((i == 0) == !!writing ? PACHA_PROT_WRITE : 0);
        int64_t mapped = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP, caps[i].fd, 0, size, prot, PACHA_MMAP_SHARED, 0);
        if (mapped < 4096) { lpr_unix_route_destroy(out); return -ENOMEM; }
        if (i) out->rx = (void *)(uintptr_t)mapped;
        else out->tx = (void *)(uintptr_t)mapped;
    }
    /* Only RX belongs to the receiver. The sender-controlled record header
     * is deliberately not trusted by packet_begin; HEAD supplies metadata. */
    if (out->rx->magic != UNIX_TRANSPORT_MAGIC || out->rx->generation != generation ||
        (writing && (out->tx->magic != UNIX_TRANSPORT_MAGIC || out->tx->generation != generation ||
            out->tx->type != UNIX_TRANSPORT_DGRAM || out->tx->capacity != UNIX_TRANSPORT_BYTES))) {
        lpr_unix_route_destroy(out); return -ESTALE;
    }
    return 0;
}

int lpr_unix_mapping_attach(const struct lpr_unix_client *client, uint64_t socket,
    uint32_t expected_type, uint64_t request_id, struct lpr_unix_mapping *out)
{
    if (!out) return -EINVAL;
    *out = (struct lpr_unix_mapping){ .fds = { -1, -1 } };
    if (!socket || !request_id) return -EINVAL;
    if (expected_type != UNIX_TRANSPORT_STREAM && expected_type != UNIX_TRANSPORT_SEQPACKET)
        return -EOPNOTSUPP;
    struct unix_control request = { .operation = UNIX_OP_ATTACH, .socket = socket, .request = request_id };
    struct pacha_ipc_fd caps[2] = {{0}};
    unsigned count = 0;
    int status = lpr_unix_client_call(client, &request, NULL, 0, caps, 2, &count);
    if (status != 0) return status;
    const struct unix_attachment attachment = { .socket = request.socket,
        .generation = request.result, .peer = request.argument,
        .type = (uint32_t)request.transaction, .flags = (uint32_t)(request.transaction >> 32),
        .peer_credentials = request.credentials };
    if (request.socket != socket || attachment.type != expected_type) {
        close_caps(caps, count); return -EPROTO;
    }
    return lpr_unix_mapping_import(&attachment, caps, count, out);
}

int lpr_unix_mapping_import(const struct unix_attachment *attachment,
    const struct pacha_ipc_fd *caps, unsigned count, struct lpr_unix_mapping *out)
{
    *out = (struct lpr_unix_mapping){ .fds = {-1, -1} };
    int status = 0;
    if (count != 2 || !attachment->socket || !attachment->generation ||
        (attachment->type != UNIX_TRANSPORT_STREAM && attachment->type != UNIX_TRANSPORT_SEQPACKET)) {
        close_caps(caps, count); return -EPROTO;
    }
    const uint64_t common = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ;
    for (unsigned i = 0; i < 2; i++) {
        struct pacha_fd_info info;
        unsigned j;
        for (j = 0; j < i && caps[j].fd != caps[i].fd; j++) {}
        const uint64_t expected = common | (i == 0 ? PACHA_FD_RIGHT_MAP_WRITE : 0);
        if (j != i || caps[i].fd < 16 || caps[i].fd >= PACHAOS_FD_TABLE_LIMIT ||
            lpr_pacha_syscall2(PACHAOS_SYSCALL_FD_GET_INFO, caps[i].fd, (uint64_t)(uintptr_t)&info) != 0 ||
            info.kind != PACHA_FD_KIND_VMO || info.rights != expected || info.size != sizeof(struct unix_endpoint) || info.flags != 0) {
            close_caps(caps, count);
            return -EBADF;
        }
    }
    struct lpr_unix_mapping local = { .socket = attachment->socket, .generation = attachment->generation,
        .peer = attachment->peer, .type = attachment->type, .flags = attachment->flags,
        .peer_credentials = attachment->peer_credentials };
    for (unsigned i = 0; i < 2; i++) local.fds[i] = (int)caps[i].fd;
    struct unix_endpoint *pages[2] = {0};
    for (unsigned i = 0; i < 2; i++) {
        const uint64_t prot = PACHA_PROT_READ | (i == 0 ? PACHA_PROT_WRITE : 0);
        const int64_t mapped = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP, caps[i].fd, 0, sizeof(struct unix_endpoint), prot, PACHA_MMAP_SHARED, 0);
        if (mapped < 4096) { status = -ENOMEM; break; }
        pages[i] = (void *)(uintptr_t)mapped;
    }
    local.outgoing_tx = pages[0] ? &pages[0]->tx : NULL;
    local.incoming_rx = pages[0] ? &pages[0]->rx : NULL;
    local.incoming_tx = pages[1] ? &pages[1]->tx : NULL;
    local.outgoing_rx = pages[1] ? &pages[1]->rx : NULL;
    if (status == 0) status = check_headers(&local);
    if (status != 0) { lpr_unix_mapping_destroy(&local); return status; }
    *out = local;
    return 0;
}
