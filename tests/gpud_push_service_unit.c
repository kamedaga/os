#include <assert.h>
#include <stdio.h>
#include "../userland/gpud/drm_service.c"
#include <poll.h>
#include <unistd.h>

static int revoked, released, closed, fail_pair, hungup;
int pacha_fd_get_info(pacha_fd_t fd, struct pacha_fd_info *out) {
    assert(fd == 80);
    *out = (struct pacha_fd_info){.kind = PACHA_FD_KIND_VMO,
                                  .size = 4096,
                                  .rights = mapping_root_rights(3),
                                  .flags = PACHA_FD_FLAG_CLOEXEC};
    return 0;
}
int gpud_gpu_rpc_take_dma_buf(struct gpud_gpu_rpc *rpc, uint64_t correlation, uint64_t token,
                              struct pacha_ipc_fd *fd) {
    (void)rpc;
    (void)correlation;
    assert(token);
    *fd = (struct pacha_ipc_fd){
        .fd = 80, .rights = mapping_root_rights(3), .flags = PACHA_FD_FLAG_CLOEXEC};
    return 0;
}
int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *pair, uint64_t rights, uint32_t flags) {
    assert(rights & PACHA_FD_RIGHT_TRANSFER);
    assert(flags == PACHA_FD_FLAG_CLOEXEC);
    if (fail_pair)
        return -EMFILE;
    *pair = (struct pacha_ipc_channel_pair){.a = 81, .b = 82};
    return 0;
}
int pacha_fd_close(pacha_fd_t fd) {
    assert(fd >= 16);
    ++closed;
    return fd == 80 ? 0 : close(fd);
}
int pacha_vmo_revoke(pacha_fd_t fd) {
    assert(fd == 80);
    ++revoked;
    return 0;
}
int gpud_gpu_rpc_release_mapping(struct gpud_gpu_rpc *rpc, uint64_t correlation, uint64_t token) {
    (void)rpc;
    assert(correlation && token);
    ++released;
    return 0;
}
long pacha_fd_poll(struct pacha_pollfd *fds, uint64_t n) {
    assert(n <= 256);
    long count = 0;
    for (uint64_t i = 0; i < n; i++) {
        struct pollfd p = {fds[i].fd, POLLIN | POLLHUP, 0};
        assert(poll(&p, 1, 0) >= 0);
        fds[i].revents = (p.revents & POLLHUP) ? PACHA_FD_EVENT_HANGUP : 0;
        if (p.revents & POLLIN)
            fds[i].revents |= PACHA_FD_EVENT_READABLE;
        count += fds[i].revents != 0;
    }
    return count;
}
long pacha_fd_wait_many_batched(struct pacha_pollfd *fds, uint64_t count, uint64_t ticks) {
    assert(!ticks);
    return pacha_fd_poll(fds, count);
}
long pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b) {
    (void)nr;
    (void)a;
    (void)b;
    assert(0);
    return -1;
}
/* These paths must not run: the test has no DRM file watches. */
int gpud_drm_close_prepare(struct gpud_drm_files *f, uint64_t g, uint64_t c,
                           struct gpud_drm_control *p, unsigned char *b, size_t n, size_t *s) {
    (void)f;
    (void)g;
    (void)c;
    (void)p;
    (void)b;
    (void)n;
    (void)s;
    return 0;
}
int gpud_drm_control_complete(struct gpud_drm_files *f, struct gpud_drm_control *p,
                              const unsigned char *b, size_t n, uint64_t *h) {
    (void)f;
    (void)p;
    (void)b;
    (void)n;
    (void)h;
    assert(0);
    return -EIO;
}
int gpud_gpu_rpc_call(struct gpud_gpu_rpc *r, unsigned int q, const unsigned char *b, size_t n,
                      unsigned char *o, size_t cap, size_t *s) {
    (void)r;
    (void)q;
    (void)b;
    (void)n;
    (void)o;
    (void)cap;
    (void)s;
    assert(0);
    return -EIO;
}

#define GPUD_READY_HOST_NO_MAIN
#include "gpud_ready_host.c"
static void push_lifetimes(void) {
    static struct gpud_drm_service s;
    static unsigned char gpu[GPUD_GPU_CHANNEL_SIZE];
    memset(&s, 0, sizeof(s));
    s.gpu.mapping = gpu;
    s.ready = gpud_ready_create();
    assert(s.ready);
    s.files.generation = 1;
    s.files.limit = GPUD_DRM_FILES_MAX;
    s.correlation = 1;
    int p[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
    int a = lift(p[0]), b = lift(p[1]);
    struct gpud_drm_mapping *m = s.mappings;
    *m = (struct gpud_drm_mapping){
        .id = 4096, .handle = 7, .gem_handle = 9, .view_fd = 80, .lease_count = 1};
    assert(!gpud_index_put(s.mapping_index, 2 * GPUD_DRM_MAPPINGS_MAX, 4096, 0, 1));
    assert(!gpud_index_put(s.gem_index, 2 * GPUD_DRM_MAPPINGS_MAX, 7, 9, 1));
    struct gpud_drm_object_lease *l = s.object_leases;
    *l = (struct gpud_drm_object_lease){.object_id = 4096, .fd = a};
    assert(!gpud_ready_add(s.ready, a, PACHA_FD_EVENT_HANGUP, READY_LEASE, l, &l->source));
    close_mapping_owner(&s, 7, 9);
    assert(m->owner_closed);
    assert(!release_closed_objects(&s));
    assert(m->id && !released && !revoked);
    close(b);
    for (int i = 0; i < 5 && m->id; i++) {
        assert(!gpud_drm_service_reap_hangups(&s));
        if (m->id) {
            struct pollfd e = {gpud_ready_fd(s.ready), POLLIN, 0};
            assert(poll(&e, 1, 3000) > 0);
        }
    }
    assert(!m->id && !l->object_id && released == 1 && revoked == 1);
    assert(!gpud_index_get(s.mapping_index, 2 * GPUD_DRM_MAPPINGS_MAX, 4096, 0));
    assert(!gpud_index_get(s.gem_index, 2 * GPUD_DRM_MAPPINGS_MAX, 7, 9));
    assert(empty_mapping(&s) == m && empty_object_lease(&s) == l);
    /* Reserve before reply, cancel before delivery, and commit exactly once. */
    *m = (struct gpud_drm_mapping){.id = 8192, .handle = 7, .gem_handle = 9, .view_fd = 80};
    s.mapping_free_head = 0;
    assert(!gpud_index_put(s.mapping_index, 2 * GPUD_DRM_MAPPINGS_MAX, 8192, 0, 1));
    assert(!gpud_index_put(s.gem_index, 2 * GPUD_DRM_MAPPINGS_MAX, 7, 9, 1));
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
    a = lift(p[0]);
    b = lift(p[1]);
    struct gpud_drm_reply_transfer transfer = {
        .lease = l, .object_id = 8192, .owner_fd = a, .client_fd = b, .count = 2};
    assert(!prepare_reply_transfer(&s, &transfer));
    assert(!s.lease_free_head && s.pending_transfer_client_fd == b);
    assert(!cancel_reply_transfer(&s, &transfer));
    assert(empty_object_lease(&s) == l && !s.pending_transfer_client_fd && !m->lease_count);
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
    a = lift(p[0]);
    b = lift(p[1]);
    transfer = (struct gpud_drm_reply_transfer){
        .lease = l, .object_id = 8192, .owner_fd = a, .client_fd = b, .count = 2};
    assert(!prepare_reply_transfer(&s, &transfer));
    finish_reply_transfer(&transfer);
    assert(m->lease_count == 1 && !s.pending_transfer_client_fd);
    close(b);
    for (int i = 0; i < 5 && m->lease_count; i++) {
        assert(!gpud_drm_service_reap_hangups(&s));
        if (m->lease_count) {
            struct pollfd e = {gpud_ready_fd(s.ready), POLLIN, 0};
            assert(poll(&e, 1, 3000) > 0);
        }
    }
    assert(!m->lease_count && m->id == 8192 && empty_object_lease(&s) == l);
    close_mapping_owner(&s, 7, 9);
    assert(!release_closed_objects(&s));
    assert(!m->id && empty_mapping(&s) == m && released == 2 && revoked == 2);
    /* Collector scheduling is not the authority boundary: target-handle refresh
     * observes HUP synchronously even before its queued notification is drained. */
    uint64_t handle;
    assert(!gpud_drm_file_open_begin(&s.files, 1, &handle));
    assert(!gpud_drm_file_open_finish(&s.files, 1, handle, 99, 0));
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
    a = lift(p[0]);
    b = lift(p[1]);
    struct gpud_drm_watch *w = s.watches;
    *w = (struct gpud_drm_watch){.handle = handle, .fd = a};
    assert(!gpud_ready_add(s.ready, a, PACHA_FD_EVENT_HANGUP, READY_WATCH, w, &w->source));
    close(b);
    assert(!refresh_handle(&s, handle));
    assert(!w->handle);
    struct gpud_drm_binding binding;
    assert(gpud_drm_file_acquire(&s.files, 1, handle, &binding) < 0);
    assert(!gpud_drm_service_reap_hangups(&s));
    assert(!gpud_drm_service_stop_ready(&s));
    puts("push service deferred GEM close/last lease/count/hash removal and delayed HUP authority "
         "PASS");
}
static void ready_burst_budget(void) {
    enum { N = 512 };
    static struct gpud_drm_service s;
    s.ready = gpud_ready_create();
    assert(s.ready);
    s.files.generation = 1;
    s.correlation = 1;
    revoked = released = closed = 0;
    int peers[N];
    for (unsigned i = 0; i < N; i++) {
        int p[2];
        assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
        int fd = fcntl(p[0], F_DUPFD_CLOEXEC, 256);
        assert(fd >= 256);
        close(p[0]);
        peers[i] = p[1];
        struct gpud_drm_mapping *m = s.mappings + i;
        *m = (struct gpud_drm_mapping){.id = (i + 1) * 4096ULL,
                                       .handle = 7,
                                       .gem_handle = i + 1,
                                       .view_fd = 80,
                                       .lease_count = 1,
                                       .owner_closed = 1};
        assert(!gpud_index_put(s.mapping_index, 2 * GPUD_DRM_MAPPINGS_MAX, m->id, 0, i + 1));
        assert(!gpud_index_put(s.gem_index, 2 * GPUD_DRM_MAPPINGS_MAX, 7, i + 1, i + 1));
        struct gpud_drm_object_lease *l = s.object_leases + i;
        *l = (struct gpud_drm_object_lease){.object_id = m->id, .fd = fd};
        assert(!gpud_ready_add(s.ready, fd, PACHA_FD_EVENT_HANGUP, READY_LEASE, l, &l->source));
    }
    for (unsigned i = 0; i < N; i++)
        close(peers[i]);
    for (unsigned turn = 0; released < N && turn < 1000; turn++) {
        int before = released;
        assert(!gpud_drm_service_reap_hangups(&s));
        assert(released - before <= 64);
        if (released == before) {
            struct pollfd p = {gpud_ready_fd(s.ready), POLLIN, 0};
            assert(poll(&p, 1, 3000) > 0);
        }
    }
    assert(released == N && revoked == N && closed == N);
    unsigned maps = 0, leases = 0;
    for (unsigned i = s.mapping_free_head; i; i = s.mappings[i - 1].free_next) {
        assert(i <= N && !s.mappings[i - 1].id);
        assert(++maps <= N);
    }
    for (unsigned i = s.lease_free_head; i; i = s.object_leases[i - 1].free_next) {
        assert(i <= N && !s.object_leases[i - 1].object_id);
        assert(++leases <= N);
    }
    assert(maps == N && leases == N);
    assert(!gpud_drm_service_stop_ready(&s));
    puts("512 HUP burst/bounded64 owner work/no lost notification/free-pool exact recovery PASS");
}

static void direct_request_readiness(void) {
    static struct gpud_drm_service s;
    memset(&s, 0, sizeof(s)); s.ready = gpud_ready_create(); assert(s.ready);
    enum { C = 300 };
    struct gpud_drm_connection c[C]; int peers[C];
    memset(c, 0, sizeof(c));
    for (unsigned i = 0; i < C; i++) {
        int p[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
        c[i].fd = lift(p[0]); peers[i] = lift(p[1]);
        c[i].next = i + 1 < C ? c + i + 1 : NULL;
    }
    s.connections = c;
    assert(!gpud_ready_add(s.ready, c[0].fd, PACHA_FD_EVENT_HANGUP, READY_CONNECTION, c, &c[0].source));
    int p[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
    struct gpud_drm_object_lease *l = s.object_leases;
    *l = (struct gpud_drm_object_lease){.object_id=99,.prime_owner=1,.fd=lift(p[0])};
    int peer = lift(p[1]);
    struct gpud_drm_reply_transfer transfer = {.lease=l,.object_id=99,.prime_owner=1,.owner_fd=l->fd,.service=&s};
    s.primes[0].token = 99; finish_reply_transfer(&transfer);
    assert(s.primes[0].owner_lease == l && s.primes[0].lease_count == 1);
    assert(!gpud_ready_add(s.ready, l->fd, PACHA_FD_EVENT_HANGUP, READY_LEASE, l, &l->source));
    assert(gpud_drm_service_pollfds(&s,NULL,0) == C + 2);
    assert(write(peers[0],"a",1)==1 && write(peers[C-1],"b",1)==1 && write(peer,"p",1)==1);
    assert(!refresh_requests(&s));
    assert(c[0].ready && c[C-1].ready && !c[1].ready && l->readable);
    /* No readable registration or collector wake is required to admit requests. */
    struct gpud_ready_event e; assert(!gpud_ready_next(s.ready,&e));
    char value; assert(read(c[0].fd,&value,1)==1 && read(l->fd,&value,1)==1);
    assert(!refresh_requests(&s)); assert(!c[0].ready && !l->readable && c[C-1].ready);
    close(peers[0]); peers[0] = -1;
    struct pollfd notify={gpud_ready_fd(s.ready),POLLIN,0}; assert(poll(&notify,1,3000)>0);
    assert(gpud_ready_next(s.ready,&e)==1 && e.kind==READY_CONNECTION && e.context==c && (e.events&PACHA_FD_EVENT_HANGUP));
    assert(!gpud_ready_remove(&c[0].source));
    s.correlation = 1; s.primes[0].view_fd = 80;
    close(peer);
    for (unsigned i=0;i<5 && s.primes[0].token;i++) {
        assert(!gpud_drm_service_reap_hangups(&s));
        if (s.primes[0].token) assert(poll(&notify,1,3000)>0);
    }
    assert(!s.primes[0].token && !s.primes[0].owner_lease && !l->object_id);
    for (unsigned i=0;i<C;i++){close(c[i].fd);if(peers[i]>=0)close(peers[i]);}
    assert(!gpud_ready_destroy(s.ready));free(s.pollfds);
    puts("direct requests/PRIME owner index/batched300 channels/no readable hop/HUP PASS");
}

int main(void) {
    push_lifetimes();
    ready_burst_budget();
    direct_request_readiness();
    return 0;
}
