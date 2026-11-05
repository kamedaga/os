/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#define gpud_ready_add lease_test_ready_add
#define gpud_ready_remove lease_test_ready_remove
#include "../drm_service.c"
#undef gpud_ready_add
#undef gpud_ready_remove
int gpud_ready_add(struct gpud_ready *, pacha_fd_t, uint64_t, unsigned, void *,
    struct gpud_ready_source **);
int gpud_ready_remove(struct gpud_ready_source **);
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

static int revoked, released, closed, fail_pair, fail_add, fail_remove;
int ph_ipc_packet_release(struct ph_ipc_packet *packet) {
    assert(!packet->fd_count);
    return 0;
}
int pacha_ipc_send(pacha_fd_t fd, const struct pacha_ipc_msg *message) {
    (void)fd;
    (void)message;
    assert(0);
    return -EIO;
}
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
    int p[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
    pair->a = fcntl(p[0], F_DUPFD_CLOEXEC, 16);
    pair->b = fcntl(p[1], F_DUPFD_CLOEXEC, 16);
    close(p[0]);
    close(p[1]);
    assert(pair->a >= 16 && pair->b >= 16);
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
    assert(nr == PACHA_FD_SYSCALL_GET_INFO && a >= 16);
    *(struct pacha_fd_info *)(uintptr_t)b = (struct pacha_fd_info){
        .kind = PACHA_FD_KIND_CHANNEL,
        .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL};
    return 0;
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
#include "../../../tests/gpud_ready_host.c"

/* Fail both before allocation and after the collector owns a source. */
int lease_test_ready_add(struct gpud_ready *r, pacha_fd_t fd, uint64_t events,
    unsigned kind, void *context, struct gpud_ready_source **source) {
    if (fail_add == 1) return -ENOMEM;
    int error = gpud_ready_add(r, fd, events, kind, context, source);
    return error ? error : fail_add == 2 ? -ENOMEM : 0;
}
int lease_test_ready_remove(struct gpud_ready_source **source) {
    return fail_remove ? -EIO : gpud_ready_remove(source);
}

static void check_pool(struct gpud_drm_service *s, unsigned live) {
    unsigned free_count = 0, used = 0;
    unsigned char seen[GPUD_DRM_OBJECT_LEASES_MAX] = {0};
    for (unsigned n = s->lease_free_head; n; n = s->object_leases[n - 1].free_next) {
        assert(n <= GPUD_DRM_OBJECT_LEASES_MAX && !seen[n - 1]);
        seen[n - 1] = 1;
        assert(!s->object_leases[n - 1].object_id && !s->object_leases[n - 1].source);
        ++free_count;
    }
    for (unsigned i = 0; i < GPUD_DRM_OBJECT_LEASES_MAX; i++) {
        if (!s->object_leases[i].object_id) { assert(seen[i]); continue; }
        assert(!seen[i] && s->object_leases[i].source);
        ++used;
    }
    assert(used == live && free_count + used == GPUD_DRM_OBJECT_LEASES_MAX);
}

static void reap_to(struct gpud_drm_service *s, unsigned prime_count, unsigned map_count) {
    for (unsigned turn = 0; turn < 10; turn++) {
        assert(!gpud_drm_service_reap_hangups(s));
        if (s->primes[0].lease_count == prime_count && s->mappings[0].lease_count == map_count)
            return;
        struct pollfd notify = {gpud_ready_fd(s->ready), POLLIN, 0};
        assert(poll(&notify, 1, 3000) > 0);
    }
    assert(0);
}

int main(void) {
    static struct gpud_drm_service s;
    s.ready = gpud_ready_create(); assert(s.ready);
    s.files.generation = 1; s.files.limit = GPUD_DRM_FILES_MAX; s.correlation = 1;
    s.lease_free_head = 1;
    for (unsigned i = 0; i < GPUD_DRM_OBJECT_LEASES_MAX; i++)
        s.object_leases[i].free_next = i + 1 < GPUD_DRM_OBJECT_LEASES_MAX ? i + 2 : 0;
    uint64_t handle, result;
    assert(!gpud_drm_file_open_begin(&s.files, 1, &handle));
    assert(!gpud_drm_file_open_finish(&s.files, 1, handle, 99, 0));
    s.mappings[0] = (struct gpud_drm_mapping){.id = 4096, .handle = handle,
        .gem_handle = 9, .view_fd = 80, .length = 4096, .rights = 3};
    assert(!gpud_index_put(s.mapping_index, 2 * GPUD_DRM_MAPPINGS_MAX, 4096, 0, 1));
    assert(!gpud_index_put(s.gem_index, 2 * GPUD_DRM_MAPPINGS_MAX, handle, 9, 1));
    struct gpud_drm_reply_transfer t = {.owner_fd = -1, .client_fd = -1};
    assert(!adopt_prime(&s, 99, 3, &t, &result));
    int owner_peer = t.client_fd;
    assert(!prepare_reply_transfer(&s, &t)); finish_reply_transfer(&t);
    assert(s.primes[0].owner_lease && s.primes[0].lease_count == 1);
    check_pool(&s, 1);
    gpud_drm_mmap_request_t request = {.handle = handle, .offset = 4096,
        .length = 4096, .prot = PACHA_PROT_READ, .flags = PACHA_MMAP_SHARED};
    struct pacha_ipc_fd *received = s.received.fds;
    for (unsigned turn = 0; turn < GPUD_DRM_OBJECT_LEASES_MAX + 1; turn++) {
        int p[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
        int fd = lift(p[0]), peer = lift(p[1]); received[1].fd = fd;
        if (turn < 2) {
            unsigned head = s.lease_free_head;
            fail_add = turn + 1;
            assert(prime_acquire_request(&s, 99) == -ENOMEM);
            assert(received[1].fd == (uint64_t)fd && s.lease_free_head == head);
            assert(s.primes[0].lease_count == 1); check_pool(&s, 1);
            fail_add = 0;
        }
        assert(!prime_acquire_request(&s, 99));
        assert(received[1].fd == PH_IPC_NO_FD && s.primes[0].lease_count == 2);
        check_pool(&s, 2);
        assert(!mmap_request(&s, &request, &t, &result));
        /* Reply cancellation must restore the free slot without changing counts. */
        if (turn == 0) {
            assert(!prepare_reply_transfer(&s, &t));
            assert(!cancel_reply_transfer(&s, &t));
            assert(!s.mappings[0].lease_count); check_pool(&s, 2);
            assert(!mmap_request(&s, &request, &t, &result));
        }
        int map_peer = t.client_fd;
        assert(!prepare_reply_transfer(&s, &t)); finish_reply_transfer(&t);
        assert(s.mappings[0].lease_count == 1); check_pool(&s, 3);
        if (turn == GPUD_DRM_OBJECT_LEASES_MAX) {
            close_mapping_owner(&s, handle, 9);
            assert(!close(owner_peer)); reap_to(&s, 1, 1);
            assert(s.primes[0].owner_closed && s.primes[0].token);
            assert(s.mappings[0].owner_closed && s.mappings[0].id);
        }
        assert(!close(peer) && !close(map_peer));
        unsigned retained = turn == GPUD_DRM_OBJECT_LEASES_MAX ? 0 : 1;
        reap_to(&s, retained, 0); check_pool(&s, retained);
    }
    assert(!s.primes[0].token && !s.mappings[0].id && released == 2 && revoked == 2);
    /* A collector that cannot detach must keep FD ownership until teardown. */
    s.primes[0] = (struct gpud_drm_prime){.token = 99, .view_fd = 80};
    int p[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
    int fd = lift(p[0]), peer = lift(p[1]); received[1].fd = fd;
    fail_add = 2; fail_remove = 1;
    assert(prime_acquire_request(&s, 99) == -EIO);
    assert(s.files.terminal_error == -EIO && received[1].fd == PH_IPC_NO_FD);
    assert(s.primes[0].lease_count == 1 && fcntl(fd, F_GETFD) >= 0);
    check_pool(&s, 1);
    fail_add = fail_remove = 0;
    assert(!gpud_drm_service_retire_mappings(&s)); assert(!close(peer));
    errno = 0; assert(fcntl(fd, F_GETFD) < 0 && errno == EBADF);
    puts("ready PRIME acquire/mmap/HUP repeated beyond pool capacity; rollback/owner exit PASS");
    return 0;
}
