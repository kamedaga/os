#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_drm/client.c"

lpr_state_t lpr_state;
static unsigned char page[4096];
static unsigned char auxiliary[GPUD_DRM_AUX_REUSE_BYTES], temporary[2 * GPUD_DRM_AUX_REUSE_BYTES];
static int vmo_creates, maps, unmaps, allocation_failure, mapping_failure;
static uint64_t allocation_size, allocation_flags, request_marker;
static int creates, registrations, requests, closed[128], fail_recv, fail_call, bad_reply, bind_error;
static uint64_t last_magic, last_id;
void *lpr_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void *lpr_memcpy(void *p, const void *s, size_t n) { return memcpy(p, s, n); }
uint64_t lpr_next_request_id(volatile uint64_t *counter) { return ++*counter; }
int64_t lpr_pacha_status_to_errno(int64_t status) { return status > 0 ? -status : status; }
int64_t pacha_kernel_status_to_errno(int64_t status) { return lpr_pacha_status_to_errno(status); }
int lpr_native_fd_info(uint64_t fd, struct pacha_fd_info *info)
{
    assert(fd == 91 || fd == 92);
    *info = (struct pacha_fd_info){.kind = PACHA_FD_KIND_VMO, .rights = UINT64_MAX};
    return 1;
}
int64_t lpr_pacha_syscall1(uint64_t op, uint64_t fd)
{ assert(op == PACHAOS_SYSCALL_FD_CLOSE && fd < 128 && fd >= 16); ++closed[fd]; return 0; }
int64_t lpr_pacha_syscall3(uint64_t op, uint64_t address, uint64_t rights, uint64_t flags)
{
    if (op == PACHAOS_SYSCALL_VMO_CREATE) {
        ++vmo_creates; allocation_size = address; allocation_flags = flags;
        if (allocation_failure) return PACHA_SYSCALL_ERR_ALLOC;
        return flags ? 91 : 92;
    }
    assert(op == PACHAOS_SYSCALL_IPC_CHANNEL_CREATE);
    assert((rights & (PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_RECV)) ==
        (PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_RECV));
    assert(flags == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
    uint64_t *pair = (void *)(uintptr_t)address;
    pair[0] = 80; pair[1] = 81; ++creates; return 0;
}
int64_t lpr_pacha_syscall2(uint64_t op, uint64_t fd, uint64_t address)
{
    if (op == PACHAOS_SYSCALL_MUNMAP) {
        assert((fd == (uintptr_t)auxiliary && address == sizeof(auxiliary)) ||
            (fd == (uintptr_t)temporary && address == allocation_size));
        ++unmaps; return 0;
    }
    assert(op == PACHAOS_SYSCALL_IPC_CALL);
    const struct pacha_ipc_msg *message = (void *)(uintptr_t)address;
    last_magic = message->word0; last_id = message->word3;
    if (last_magic == GPUD_DRM_BIND_PAGE_REQUEST_MAGIC) {
        ++registrations;
        assert(fd == LPR_GPUD_DRM_ENDPOINT_FD && message->fd_count == (lpr_drm_aux_cache ? 3u : 2u));
        assert(message->fds[0].fd == 90 && message->fds[1].fd == 81);
        if (lpr_drm_aux_cache) {
            assert(message->word1 == sizeof(auxiliary) && message->fds[2].fd == 91);
            assert(message->fds[2].rights == (PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE));
        }
    } else {
        ++requests;
        assert(fd == 80 && last_magic == PACHA_SERVICE_REQUEST_MAGIC && !message->fd_count);
        assert(message->word1 == request_marker);
        if (fail_call) return PACHA_SYSCALL_ERR_CLOSED;
    }
    return 82;
}
int64_t lpr_pacha_syscall6(uint64_t op, uint64_t fd, uint64_t address,
    uint64_t bytes, uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(op == PACHAOS_SYSCALL_MMAP && !address && bytes == allocation_size && !offset);
    assert(prot == (PACHAOS_PROT_READ | PACHAOS_PROT_WRITE) && flags == PACHAOS_MMAP_SHARED);
    assert(fd == 91 || fd == 92);
    ++maps;
    return mapping_failure ? PACHA_SYSCALL_ERR_ALLOC :
        (int64_t)(uintptr_t)(fd == 91 ? auxiliary : temporary);
}
int64_t lpr_pacha_syscall4(uint64_t op, uint64_t fd, uint64_t address, uint64_t ticks, uint64_t flags)
{
    assert(op == PACHAOS_SYSCALL_IPC_RECV_WAIT && fd == 82 && ticks == UINT64_MAX && !flags);
    if (fail_recv && last_magic != GPUD_DRM_BIND_PAGE_REQUEST_MAGIC) return PACHA_SYSCALL_ERR_CLOSED;
    struct pacha_ipc_msg *reply = (void *)(uintptr_t)address;
    if (last_magic == GPUD_DRM_BIND_PAGE_REQUEST_MAGIC) {
        reply->word0 = GPUD_DRM_BIND_PAGE_REPLY_MAGIC;
        reply->word1 = (uint64_t)bind_error;
    } else {
        reply->word0 = bad_reply ? 0 : PACHA_SERVICE_REPLY_MAGIC;
        reply->word3 = last_id;
        pacha_service_envelope_t *header = (void *)page;
        header->magic = PACHA_SERVICE_REPLY_MAGIC;
        header->status = 0; header->result = 1;
    }
    return 0;
}
static int64_t hello(void)
{
    uint64_t result = 0;
    int64_t error = lpr_gpud_drm_call(GPUD_DRM_OP_HELLO, 90, page, 0, &result, NULL);
    if (!error) assert(result == 1);
    return error;
}
int main(void)
{
    lpr_tty_wire_page_fd = 90; lpr_tty_wire_page = page;
    assert(!hello() && creates == 1 && registrations == 1 && requests == 1);
    assert(!closed[80] && closed[81] == 1);
    for (int i = 0; i < 7; ++i) assert(!hello());
    assert(creates == 1 && requests == 8);
    fail_recv = 1;
    assert(hello() < 0 && requests == 9 && creates == 1 && closed[80] == 1);
    assert(!lpr_drm_request_channel_fd); /* No automatic request replay. */
    fail_recv = 0;
    assert(!hello() && creates == 2 && requests == 10);
    int before = closed[80];
    lpr_drm_after_fork_child();
    assert(!lpr_drm_request_channel_fd && closed[80] == before);
    assert(!hello() && creates == 3 && requests == 11);
    bad_reply = 1;
    assert(hello() < 0 && requests == 12 && !lpr_drm_request_channel_fd);
    bad_reply = 0; bind_error = -12;
    assert(hello() == -12 && requests == 12 && !lpr_drm_request_channel_fd);
    bind_error = 0; fail_call = 1;
    assert(hello() < 0 && requests == 13 && !lpr_drm_request_channel_fd);
    assert(lpr_gpud_drm_bind_page(91, page) == LPR_GPUD_DRM_ENDPOINT_FD);
    fail_call = 0;
    assert(!hello());
    int aux_fd; void *mapping; uint64_t bytes;
    allocation_failure = 1;
    assert(lpr_drm_aux_create(5284, 1, &aux_fd, &mapping, &bytes) < 0);
    assert(!lpr_drm_aux_cache);
    allocation_failure = 0; mapping_failure = 1;
    assert(lpr_drm_aux_create(5284, 1, &aux_fd, &mapping, &bytes) < 0);
    assert(!lpr_drm_aux_cache && closed[91] == 1);
    mapping_failure = 0;
    assert(!lpr_drm_aux_create(5284, 1, &aux_fd, &mapping, &bytes));
    assert(aux_fd == 91 && mapping == auxiliary && bytes == sizeof(auxiliary));
    assert(allocation_flags == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
    assert(!lpr_drm_request_channel_fd && lpr_drm_aux_cache_busy);
    request_marker = GPUD_DRM_REQUEST_BOUND_AUX;
    gpud_drm_ioctl_request_t *ioctl = lpr_gpud_drm_payload(page);
    memset(ioctl, 0, sizeof(*ioctl)); ioctl->aux_size = 5284;
    assert(!lpr_gpud_drm_call_transfer(GPUD_DRM_OP_HANDLE_IOCTL, 90, page,
        sizeof(*ioctl), NULL, NULL, aux_fd, 1));
    int saved_maps = maps, saved_unmaps = unmaps, saved_creates = vmo_creates;
    memset(mapping, 0xa5, 5284);
    lpr_drm_aux_destroy(aux_fd, mapping, bytes);
    assert(!lpr_drm_aux_cache_busy && unmaps == saved_unmaps && closed[91] == 1);
    assert(!lpr_drm_aux_create(7276, 1, &aux_fd, &mapping, &bytes));
    for (int i = 0; i < 7276; ++i) assert(auxiliary[i] == 0);
    assert(maps == saved_maps && vmo_creates == saved_creates);
    /* A concurrent owner, a temporary control page, and an oversized payload
     * all keep the exact-size temporary path; the cache budget is not a limit. */
    int tmp_fd; void *tmp; uint64_t tmp_bytes;
    assert(!lpr_drm_aux_create(5284, 1, &tmp_fd, &tmp, &tmp_bytes));
    assert(tmp_fd == 92 && tmp_bytes == 8192 && !allocation_flags);
    lpr_drm_aux_destroy(tmp_fd, tmp, tmp_bytes);
    lpr_drm_aux_destroy(aux_fd, mapping, bytes);
    assert(!lpr_drm_aux_create(5284, 0, &tmp_fd, &tmp, &tmp_bytes));
    assert(tmp_fd == 92 && tmp_bytes == 8192);
    lpr_drm_aux_destroy(tmp_fd, tmp, tmp_bytes);
    assert(!lpr_drm_aux_create(sizeof(auxiliary) + 1, 1, &tmp_fd, &tmp, &tmp_bytes));
    assert(tmp_fd == 92 && tmp_bytes == sizeof(auxiliary) + 4096);
    lpr_drm_aux_destroy(tmp_fd, tmp, tmp_bytes);
    saved_unmaps = unmaps; before = closed[91];
    lpr_drm_after_fork_child();
    assert(!lpr_drm_aux_cache && !lpr_drm_aux_cache_fd && !lpr_drm_aux_cache_busy);
    assert(unmaps == saved_unmaps + 1 && closed[91] == before);
    puts("LPR page connection: one bind, no VMO per call, loss/no replay, fork, errors PASS");
    puts("LPR auxiliary reuse: zeroing, bounded retention, temporary fallback, fork and errors PASS");
}
