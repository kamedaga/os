#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pacha/ipc.h"

static unsigned kinds[256], incoming_kinds[19];
static struct pacha_ipc_msg incoming, last_reply;
static int queued;
static unsigned bad_envelope;
static unsigned replies;
static void *mapped_page;

static unsigned free_fds(void)
{
    unsigned n = 0;
    for (int fd = 16; fd < 256; ++fd) n += kinds[fd] == 0;
    return n;
}
static int install(unsigned kind)
{
    for (int fd = 16; fd < 256; ++fd)
        if (!kinds[fd]) { kinds[fd] = kind; return fd; }
    abort();
}
int pacha_fd_close(int fd)
{
    assert(fd >= 16 && fd < 256 && kinds[fd]);
    kinds[fd] = 0;
    return 0;
}
int pacha_fd_get_info(int fd, struct pacha_fd_info *info)
{
    if (fd < 16 || fd >= 256 || !kinds[fd]) return -9;
    memset(info, 0, sizeof(*info));
    info->kind = kinds[fd];
    info->rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_TRANSFER;
    return 0;
}
void *pacha_mmap(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{
    (void)prot; (void)flags; (void)offset;
    assert(kinds[fd] == PACHA_FD_KIND_VMO);
    mapped_page = calloc(1, size);
    assert(mapped_page);
    return mapped_page;
}
int pacha_munmap(void *page, uint64_t size)
{ (void)size; free(page); return 0; }
int pacha_ipc_recv(int fd, struct pacha_ipc_msg *msg)
{
    (void)fd;
    if (!queued) return PACHA_ERR_NOT_READY;
    if (free_fds() < incoming.fd_count) return PACHA_ERR_ALLOC;
    assert(msg->fd_capacity >= incoming.fd_count);
    struct pacha_ipc_fd *fds = msg->fds;
    *msg = incoming;
    msg->fds = fds;
    for (unsigned i = 0; i < incoming.fd_count; ++i)
        fds[i].fd = install(incoming_kinds[i]);
    queued = 0;
    return 0;
}
int pacha_ipc_reply(int fd, const struct pacha_ipc_msg *msg)
{
    assert(kinds[fd] == PACHA_FD_KIND_REPLY);
    last_reply = *msg;
    ++replies;
    return 0;
}
int pacha_ipc_send(int fd, const struct pacha_ipc_msg *msg)
{ (void)fd; (void)msg; return 0; }
int pacha_service_wait_add(struct pacha_service_wait_set *set, int fd, uint32_t events)
{
    set->fds[set->count++] = (struct pacha_pollfd){.fd=fd, .events=events};
    return 0;
}
uint64_t pacha_service_wait_revents(const struct pacha_service_wait_set *set, int fd)
{
    for (uint64_t i = 0; i < set->count; ++i)
        if (set->fds[i].fd == fd) return set->fds[i].revents;
    return 0;
}

#include "../userland/netd/src/socket_service.c"

void pacha_trace_emit(uint32_t c, uint32_t e, uint32_t cl, uint32_t n,
    uint64_t a, uint64_t b, uint64_t d, uint64_t f, uint64_t g, uint64_t h)
{ (void)c; (void)e; (void)cl; (void)n; (void)a; (void)b; (void)d; (void)f; (void)g; (void)h; }

/* INET is outside this Unix/IPC regression; reaching it is a test error. */
int netd_libuinet_socket_open(uint64_t d, uint64_t t, uint64_t p, int f, uint64_t *h)
{ (void)d; (void)t; (void)p; (void)f; (void)h; abort(); }
int netd_libuinet_socket_dup(uint64_t h) { (void)h; abort(); }
int netd_libuinet_socket_close(uint64_t h) { (void)h; abort(); }
int netd_libuinet_socket_connect(uint64_t h, uint32_t a, uint16_t p, uint64_t f)
{ (void)h; (void)a; (void)p; (void)f; abort(); }
int netd_libuinet_socket_send(uint64_t h, const void *d, size_t n, uint64_t f, uint32_t a, uint16_t p, size_t *s)
{ (void)h; (void)d; (void)n; (void)f; (void)a; (void)p; (void)s; abort(); }
int netd_libuinet_socket_recv(uint64_t h, void *d, size_t n, uint64_t f, size_t *s)
{ (void)h; (void)d; (void)n; (void)f; (void)s; abort(); }
int netd_libuinet_socket_poll(uint64_t h, uint32_t e, uint32_t *r, int32_t *s)
{ (void)h; (void)e; (void)r; (void)s; abort(); }

static int64_t rpc(uint64_t op, uint64_t arg, unsigned caps)
{
    assert(caps <= 19 && !queued);
    incoming = (struct pacha_ipc_msg){.word0=PACHA_SERVICE_REQUEST_MAGIC,
        .word1=op, .word2=arg, .word3=replies+1, .fd_count=caps};
    if (bad_envelope == 1) incoming.word0 = 0;
    if (bad_envelope == 2) incoming.word3 = 0;
    for (unsigned i = 0; i < caps; ++i) incoming_kinds[i] = PACHA_FD_KIND_CHANNEL;
    if (caps) incoming_kinds[caps-1] = PACHA_FD_KIND_REPLY;
    if (op == NETD_OP_PAGE_ATTACH) incoming_kinds[0] = PACHA_FD_KIND_VMO;
    unsigned before = replies;
    queued = 1;
    (void)netd_socket_service_poll();
    assert(!queued && replies == before+1);
    return (int64_t)last_reply.word1;
}

int main(void)
{
    g_netd_socket_endpoint_fd = install(PACHA_FD_KIND_ENDPOINT);
    assert(rpc(NETD_OP_PAGE_ATTACH, 0, 3) == 0);
    uint64_t page = last_reply.word2;
    netd_socket_t *req = mapped_page;
    req->domain = NETD_AF_UNIX;
    req->type = NETD_SOCK_STREAM;
    assert(rpc(NETD_OP_SOCKET, page, 2) == 0);
    uint64_t handle = last_reply.word2;
    int padding[240];
    unsigned pad = 0;
    while (free_fds() > 20) padding[pad++] = install(PACHA_FD_KIND_VMO);
    assert(rpc(NETD_OP_DUP, handle, 2) == 0 && free_fds() == 19);
    for (unsigned i = 0; i < 100; ++i) {
        assert(rpc(NETD_OP_DUP, handle, 2) == -24 && free_fds() == 19);
        assert(rpc(NETD_OP_HELLO, 0, 1) == 0 && free_fds() == 19);
    }
    assert(rpc(NETD_OP_PAGE_ATTACH, 0, 3) == -24 && free_fds() == 19);
    assert(rpc(NETD_OP_SOCKET, page, 2) == -24 && free_fds() == 19);
    assert(rpc(NETD_OP_SOCKETPAIR, page, 3) == -24 && free_fds() == 19);
    assert(rpc(NETD_OP_ATTACH_WAIT, page, 2) == -24 && free_fds() == 19);
    netd_io_t *io = mapped_page;
    memset(io, 0, sizeof(*io));
    io->handle = handle;
    io->transfer_count = 1;
    io->capability_count = 16;
    assert(rpc(NETD_OP_SEND, page, 17) == -24 && free_fds() == 19);
    /* Invalid maximum-sized requests must release all received capabilities. */
    assert(rpc(NETD_OP_HELLO, 0, 19) == -22 && free_fds() == 19);
    for (bad_envelope = 1; bad_envelope <= 2; ++bad_envelope)
        assert(rpc(NETD_OP_HELLO, 0, 19) == -22 && free_fds() == 19);
    bad_envelope = 0;
    assert(rpc(NETD_OP_CLOSE, handle, 1) == 0);
    struct pacha_service_wait_set wait_set = {0};
    assert(netd_socket_service_collect_wait_sources(&wait_set) == 0);
    for (uint64_t i = 0; i < wait_set.count; ++i)
        wait_set.fds[i].revents = PACHA_FD_EVENT_HANGUP;
    netd_socket_service_reap_hangups(&wait_set);
    assert(g_netd_socket_transfer_leases == NULL && g_netd_page_attachment_count == 0);
    assert(free_fds() == 22);
    while (pad) assert(pacha_fd_close(padding[--pad]) == 0);
    assert(free_fds() == 239);
    assert(rpc(NETD_OP_PAGE_ATTACH, 0, 3) == 0);
    page = last_reply.word2;
    req = mapped_page;
    req->domain = NETD_AF_UNIX;
    req->type = NETD_SOCK_STREAM;
    assert(rpc(NETD_OP_SOCKET, page, 2) == 0);
    assert(rpc(NETD_OP_CLOSE, last_reply.word2, 1) == 0);
    memset(&wait_set, 0, sizeof(wait_set));
    assert(netd_socket_service_collect_wait_sources(&wait_set) == 0);
    for (uint64_t i = 0; i < wait_set.count; ++i)
        wait_set.fds[i].revents = PACHA_FD_EVENT_HANGUP;
    netd_socket_service_reap_hangups(&wait_set);
    assert(free_fds() == 239);
    puts("netd FD budget: PASS (pressure replies, max transfer, CLOSE, reclamation, recovery)");
}
