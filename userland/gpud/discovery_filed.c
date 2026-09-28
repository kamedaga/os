/* SPDX-License-Identifier: MIT */
#include "discovery.h"
#include <filed/flags.h>
#include <filed/ipc_protocol.h>
#include <filed/payload.h>
#include <pacha/ipc.h>
#include <errno.h>
#include <string.h>

struct transport { int endpoint, fd; void *page; };
static int open_transport(struct transport *t, int endpoint)
{
    t->endpoint = endpoint;
    t->fd = pacha_vmo_create(FILED_PAGE_BYTES, PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE, 0);
    if (t->fd < 16) return t->fd < 0 ? t->fd : -EIO;
    t->page = pacha_mmap(t->fd, FILED_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (t->page) return 0;
    pacha_fd_close(t->fd);
    return -ENOMEM;
}
static void close_transport(struct transport *t)
{
    pacha_munmap(t->page, FILED_PAGE_BYTES);
    pacha_fd_close(t->fd);
}
static void *payload(struct transport *t)
{
    memset(t->page, 0, FILED_PAGE_BYTES);
    return (unsigned char *)t->page + PACHA_SERVICE_HEADER_BYTES;
}
static int call(struct transport *t, uint32_t op, size_t size, uint64_t *result)
{
    static uint64_t sequence;
    const uint64_t id = ++sequence;
    pacha_service_envelope_t *h = t->page;
    *h = (pacha_service_envelope_t){.magic = PACHA_SERVICE_REQUEST_MAGIC,
        .abi_version = PACHA_SERVICE_ABI_VERSION, .service_id = FILED_SERVICE_ID,
        .op = op, .flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD,
        .request_id = id, .trace_id = id, .payload_size = size};
    struct pacha_ipc_fd fd = {.fd = t->fd, .rights = PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE};
    struct pacha_ipc_msg req = {.word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word3 = id, .fds = &fd, .fd_count = 1}, reply = {0};
    int ep = pacha_ipc_call(t->endpoint, &req);
    if (ep < 16) return ep < 0 ? ep : -EIO;
    int err = pacha_ipc_recv_wait(ep, &reply, PACHA_FD_WAIT_FOREVER);
    pacha_fd_close(ep);
    if (err) return err;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC || h->magic != PACHA_SERVICE_REPLY_MAGIC ||
        h->abi_version != PACHA_SERVICE_ABI_VERSION || h->service_id != FILED_SERVICE_ID ||
        h->request_id != id || h->op != op) return -EPROTO;
    if (h->status) return (int)h->status;
    if (result) *result = h->result;
    return 0;
}
static int create(struct transport *t, const struct gpud_discovery_entry *e, int *owned)
{
    if (e->kind == GPUD_DISC_DIR) {
        filed_mkdir_t *p = payload(t);
        p->mode = 0755; strcpy(p->name, e->path);
        int err = call(t, FILED_OP_VFS_MKDIR, sizeof(*p), NULL);
        *owned = err == 0;
        return err;
    }
    if (e->kind == GPUD_DISC_LINK) {
        filed_symlink_t *p = payload(t);
        strcpy(p->name, e->path); strcpy(p->target, e->value);
        p->target_length = strlen(e->value);
        int err = call(t, FILED_OP_VFS_SYMLINK, sizeof(*p), NULL);
        *owned = err == 0; return err;
    }
    if (e->kind == GPUD_DISC_NODE) {
        filed_mknod_t *p = payload(t);
        strcpy(p->name, e->path); p->mode = 0020660; p->dev = e->dev;
        int err = call(t, FILED_OP_VFS_MKNOD, sizeof(*p), NULL);
        *owned = err == 0; return err;
    }
    filed_path_request_t *p = payload(t);
    strcpy(p->path, e->path);
    p->rights = FILED_RIGHT_WRITE | FILED_RIGHT_STAT;
    p->flags = FILED_OPEN_CREATE | FILED_OPEN_EXCLUSIVE | FILED_OPEN_NOFOLLOW;
    uint64_t handle;
    int err = call(t, FILED_OP_VFS_OPENAT, sizeof(*p), &handle);
    if (err) return err;
    *owned = 1;
    filed_io_t *io = payload(t);
    io->handle = handle; io->length = strlen(e->value);
    memcpy(io->data, e->value, io->length);
    uint64_t written = 0;
    err = call(t, FILED_OP_VFS_PWRITE, sizeof(*io), &written);
    if (!err && written != strlen(e->value)) err = -EIO;
    filed_handle_request_t *close = payload(t);
    close->handle = handle;
    int closed = call(t, FILED_OP_VFS_CLOSE, sizeof(*close), NULL);
    return err ? err : closed;
}
int gpud_discovery_remove(struct gpud_discovery *d, int filed_fd)
{
    struct transport t;
    int err = open_transport(&t, filed_fd), first = 0;
    if (err) return err;
    for (size_t n = d->published; n; --n) {
        size_t i = n - 1;
        if (!(d->owned & (UINT32_C(1) << i))) continue;
        filed_unlink_t *p = payload(&t);
        strcpy(p->name, d->entries[i].path);
        err = call(&t, d->entries[i].kind == GPUD_DISC_DIR ?
            FILED_OP_VFS_RMDIR : FILED_OP_VFS_UNLINK, sizeof(*p), NULL);
        if (!err || err == -ENOENT) d->owned &= ~(UINT32_C(1) << i);
        else if (!first) first = err;
    }
    close_transport(&t);
    if (!d->owned) d->published = 0;
    return first;
}
int gpud_discovery_publish(struct gpud_discovery *d, int filed_fd)
{
    if (!d || !d->count || d->count > 32 || d->published) return -EINVAL;
    struct transport t;
    int err = open_transport(&t, filed_fd);
    if (err) return err;
    for (size_t i = 0; i < d->count; ++i) {
        int owned = 0;
        err = create(&t, &d->entries[i], &owned);
        d->published = i + 1;
        if (owned) d->owned |= UINT32_C(1) << i;
        /* Only shared ancestor directories may preexist. Never replace
         * another publisher's device metadata or remove its entries. */
        if (err == -EEXIST && i < 8) err = 0;
        if (err) break;
    }
    close_transport(&t);
    if (err) (void)gpud_discovery_remove(d, filed_fd);
    return err;
}
