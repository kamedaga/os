#include <assert.h>
#include <stdio.h>
#include "../userland/gpud/drm_service.c"

static int revoked, released, closed, fail_pair, hungup;
int pacha_fd_get_info(int fd, struct pacha_fd_info *out)
{
    assert(fd == 80);
    *out = (struct pacha_fd_info){.kind = PACHA_FD_KIND_VMO,
        .size = 4096, .rights = mapping_root_rights(3),
        .flags = PACHA_FD_FLAG_CLOEXEC};
    return 0;
}
int gpud_gpu_rpc_take_dma_buf(struct gpud_gpu_rpc *rpc,
    uint64_t correlation, uint64_t token, struct pacha_ipc_fd *fd)
{
    (void)rpc; (void)correlation; assert(token);
    *fd = (struct pacha_ipc_fd){.fd = 80,
        .rights = mapping_root_rights(3), .flags = PACHA_FD_FLAG_CLOEXEC};
    return 0;
}
int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *pair,
    uint64_t rights, uint32_t flags)
{
    assert(rights & PACHA_FD_RIGHT_TRANSFER);
    assert(flags == PACHA_FD_FLAG_CLOEXEC);
    if (fail_pair) return -EMFILE;
    *pair = (struct pacha_ipc_channel_pair){.a = 81, .b = 82};
    return 0;
}
int pacha_fd_close(int fd) { assert(fd >= 16); ++closed; return 0; }
int pacha_vmo_revoke(int fd) { assert(fd == 80); ++revoked; return 0; }
int gpud_gpu_rpc_release_mapping(struct gpud_gpu_rpc *rpc,
    uint64_t correlation, uint64_t token)
{ (void)rpc; assert(correlation && token); ++released; return 0; }
long pacha_fd_poll(struct pacha_pollfd *fds, uint64_t count)
{
    long ready = 0;
    for (uint64_t i = 0; i < count; ++i) {
        fds[i].revents = fds[i].fd == hungup ? PACHA_FD_EVENT_HANGUP : 0;
        ready += !!fds[i].revents;
    }
    return ready;
}
long pacha_fd_wait_many_batched(struct pacha_pollfd *fds, uint64_t count, uint64_t ticks)
{ assert(!ticks); return pacha_fd_poll(fds, count); }
long pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b)
{ (void)nr; (void)a; (void)b; assert(0); return -1; }
/* These paths must not run: the test has no DRM file watches. */
int gpud_drm_close_prepare(struct gpud_drm_files *f, uint64_t g, uint64_t c,
    struct gpud_drm_control *p, unsigned char *b, size_t n, size_t *s)
{ (void)f;(void)g;(void)c;(void)p;(void)b;(void)n;(void)s; assert(0); return -EIO; }
int gpud_drm_control_complete(struct gpud_drm_files *f,
    struct gpud_drm_control *p, const unsigned char *b, size_t n, uint64_t *h)
{ (void)f;(void)p;(void)b;(void)n;(void)h; assert(0); return -EIO; }
int gpud_gpu_rpc_call(struct gpud_gpu_rpc *r, unsigned int q,
    const unsigned char *b, size_t n, unsigned char *o, size_t cap, size_t *s)
{ (void)r;(void)q;(void)b;(void)n;(void)o;(void)cap;(void)s; assert(0); return -EIO; }

int main(void)
{
    static struct gpud_drm_service service;
    service.files.generation = 1;
    struct gpud_drm_reply_transfer transfer = {.owner_fd = -1, .client_fd = -1};
    uint64_t result = 0;
    /* More than the 64 PRIME slots must be reusable after client death. */
    for (unsigned i = 1; i <= 192; ++i) {
        assert(!adopt_prime(&service, i, &transfer, &result));
        assert(result == i && transfer.count == 2 && transfer.prime_owner);
        assert(transfer.fds[1].rights == PACHA_FD_RIGHT_CLOSE);
        assert(transfer.fds[1].transfer_flags & PACHA_IPC_TRANSFER_INHERIT);
        finish_reply_transfer(&transfer);
        /* A second process/mapping lease must survive original owner death. */
        service.object_leases[1] = (struct gpud_drm_object_lease){
            .object_id = i, .fd = 83};
        hungup = 81;
        assert(!gpud_drm_service_reap_hangups(&service));
        assert(find_prime(&service, i)->owner_closed);
        assert(released == (int)i - 1);
        hungup = 83;
        assert(!gpud_drm_service_reap_hangups(&service));
        assert(!find_prime(&service, i));
        assert(released == (int)i && revoked == (int)i);
    }
    assert(!adopt_prime(&service, 200, &transfer, &result));
    assert(!cancel_reply_transfer(&service, &transfer));
    assert(!find_prime(&service, 200) && released == 193);
    fail_pair = 1;
    assert(adopt_prime(&service, 201, &transfer, &result) == -EMFILE);
    assert(!find_prime(&service, 201) && released == 194);
    fail_pair = 0;
    assert(!adopt_prime(&service, 202, &transfer, &result));
    finish_reply_transfer(&transfer);
    assert(!prime_release_request(&service, 202));
    assert(find_prime(&service, 202) && released == 194);
    hungup = 81;
    assert(!gpud_drm_service_reap_hangups(&service));
    assert(!find_prime(&service, 202) && released == 195);
    for (size_t i = 0; i < GPUD_DRM_OBJECT_LEASES_MAX; ++i)
        service.object_leases[i].object_id = 999;
    assert(adopt_prime(&service, 203, &transfer, &result) == -EMFILE);
    assert(!find_prime(&service, 203) && released == 195);
    free(service.pollfds);
    puts("PRIME owner death, surviving leases, 192 reuses and reply rollback: PASS");
}
