#include "lpr_ipc_mock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include "../userland/personality/linux/runtime/lpr_drm/client.c"

static unsigned closes;
static uint64_t pair_flags;
static uint64_t pair_base = 80;
static uint64_t high_closed;
lpr_state_t lpr_state;
static unsigned char wire_page[4096];
static uint64_t request_id;
static atomic_uint unmap_race_stage;
static int unmap_failure;
static int64_t map_failure, remap_failure;
#if defined(LPR_UNMAP_PROFILE) && LPR_UNMAP_PROFILE
static unsigned profile_logs;
#endif
pacha_fd_t lpr_create_tty_wire_page(void **out) { *out = wire_page; return 91; }
void lpr_destroy_tty_wire_page(pacha_fd_t fd, void *page)
{ assert(fd == 91 && page == wire_page); }
uint64_t lpr_next_request_id(volatile uint64_t *counter) { return ++*counter; }
static int64_t fixture_prepare_request(uint64_t nr, uint64_t fd, uint64_t message)
{
#if defined(LPR_UNMAP_PROFILE) && LPR_UNMAP_PROFILE
    if (nr == PACHAOS_SYSCALL_LOG) {
        assert(lpr_drm_mapping_lock == 0);
        assert(message < 208 && !memcmp((void *)(uintptr_t)fd, "LPR_UNMAP_STAGE", 15));
        ++profile_logs;
        return -5;
    }
#endif
    if (nr == PACHAOS_SYSCALL_MUNMAP) {
        assert(lpr_drm_mapping_lock == 1);
        assert(fd == 0x50000 && message == 4096);
        if (atomic_load(&unmap_race_stage)) {
            atomic_store(&unmap_race_stage, 2);
            while (atomic_load(&unmap_race_stage) != 3) sched_yield();
        }
        return unmap_failure;
    }
    assert(nr == PACHAOS_SYSCALL_IPC_CALL && fd == LPR_GPUD_DRM_ENDPOINT_FD);
    struct pacha_ipc_msg *msg = (void *)(uintptr_t)message;
    assert(msg->fd_count == 2 && msg->fds[1].fd == 81);
    request_id = msg->word3;
    return 92;
}
static lpr_dmabuf_backend_t dmabuf = {
    .native.raw = 90, .size = 4096, .writable = 1,
};
lpr_dmabuf_backend_t *lpr_dmabuf_backend(uint64_t fd)
{ assert(fd == 4); return &dmabuf; }
int lpr_native_fd_info(uint64_t fd, struct pacha_fd_info *info)
{
    assert(fd == 90 || fd == 81);
    *info = (struct pacha_fd_info){
        .kind = fd == 90 ? PACHA_FD_KIND_VMO : PACHA_FD_KIND_CHANNEL,
        .rights = PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS };
    return 1;
}
static int64_t fixture_finish_request(uint64_t nr, uint64_t fd, uint64_t min,
    uint64_t rights, uint64_t flags)
{
    if (nr == PACHAOS_SYSCALL_IPC_RECV_WAIT) {
        assert(fd == 92);
        struct pacha_ipc_msg *msg = (void *)(uintptr_t)min;
        msg->word0 = PACHA_SERVICE_REPLY_MAGIC;
        msg->word3 = request_id;
        ((pacha_service_envelope_t *)wire_page)->magic = PACHA_SERVICE_REPLY_MAGIC;
        ((pacha_service_envelope_t *)wire_page)->status = 0;
        return 0;
    }
    assert(nr == PACHA_FD_SYSCALL_DUP && fd == 90 && min == 16);
    assert(rights == PACHA_FD_RIGHT_CLOSE);
    assert(flags == (PACHA_FD_FLAG_INHERIT | PACHA_FD_FLAG_CLOEXEC));
    return 80;
}
int64_t lpr_close_native_fd_if_open(uint64_t fd)
{ return lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd); }
void *lpr_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void lpr_state_lock(volatile uint32_t *lock)
{ while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) sched_yield(); }
void lpr_state_unlock(volatile uint32_t *lock)
{ assert(*lock == 1); __atomic_store_n(lock, 0, __ATOMIC_RELEASE); }
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    if (nr == PACHAOS_SYSCALL_FD_CLOSE && fd >= 0x80000000ull) { high_closed = fd; return 0; }
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && (fd == 80 || fd == 81 || fd == 92));
    if (fd != 92) ++closes;
    return 0;
}
int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t address, uint64_t rights, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_IPC_CHANNEL_CREATE);
    assert(rights & PACHA_FD_RIGHT_CLOSE);
    pair_flags = flags;
    uint64_t *pair = (void *)(uintptr_t)address;
    pair[0] = pair_base; pair[1] = pair_base + 1;
    return 0;
}
int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t address,
    uint64_t length, uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(nr == PACHAOS_SYSCALL_MMAP && fd == 90);
    assert(lpr_drm_mapping_lock == 1);
    (void)length; (void)prot; (void)flags; (void)offset;
    return map_failure ? map_failure : (int64_t)address;
}
int64_t lpr_pacha_syscall5(uint64_t nr, uint64_t address, uint64_t length,
    uint64_t new_length, uint64_t flags, uint64_t target)
{
    assert(nr == PACHA_VM_SYSCALL_MREMAP && lpr_drm_mapping_lock == 1);
    assert(address == 0x50000 && length == 4096 && new_length == 4096);
    assert(flags == 3 && target == 0x60000);
    return remap_failure ? remap_failure : (int64_t)target;
}
int64_t pacha_kernel_status_to_errno(int64_t status) { return -status; }
int64_t lpr_pacha_status_to_errno(int64_t status) { return -status; }

static void *reuse_address(void *unused)
{
    (void)unused;
    while (atomic_load(&unmap_race_stage) != 2) sched_yield();
    atomic_store(&unmap_race_stage, 3);
    assert(lpr_drm_map_received(90, 81, 0x50000, 4096, 3, 1, 0) == 0x50000);
    return NULL;
}

static void check_unmap_reuse(void)
{
    unsigned before = closes;
    assert(lpr_drm_map_received(90, 80, 0x50000, 4096, 3, 1, 0) == 0x50000);
    unmap_failure = 3;
    assert(lpr_drm_native_munmap(0x50000, 4096) == 3);
    assert(closes == before);
    unmap_failure = 0;
    atomic_store(&unmap_race_stage, 1);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, reuse_address, NULL));
    assert(!lpr_drm_native_munmap(0x50000, 4096));
    assert(!pthread_join(thread, NULL));
    atomic_store(&unmap_race_stage, 0);
    assert(closes == before + 1); /* Only the old lease is gone. */
    assert(!lpr_drm_native_munmap(0x50000, 4096));
    assert(closes == before + 2);
}

static void check_native_replacements(void)
{
    unsigned before = closes;
    assert(lpr_drm_map_received(90, 80, 0x50000, 4096, 3, 1, 0) == 0x50000);
    map_failure = 3;
    assert(lpr_drm_native_mmap(90, 0x50000, 4096, 3, PACHAOS_MMAP_FIXED, 0) == 3);
    assert(closes == before);
    map_failure = 0;
    assert(lpr_drm_native_mmap(90, 0x50000, 4096, 3, PACHAOS_MMAP_FIXED, 0) == 0x50000);
    assert(closes == before + 1);

    assert(lpr_drm_map_received(90, 80, 0x50000, 4096, 3, 1, 0) == 0x50000);
    assert(lpr_drm_map_received(90, 81, 0x60000, 4096, 3, 1, 0) == 0x60000);
    remap_failure = 3;
    assert(lpr_drm_native_mremap(0x50000, 4096, 4096, 3, 0x60000) == 3);
    assert(closes == before + 1);
    remap_failure = 0;
    assert(lpr_drm_native_mremap(0x50000, 4096, 4096, 3, 0x60000) == 0x60000);
    assert(closes == before + 2); /* Destination replaced, source retained. */
    lpr_drm_mapping_unmapped(0x50000, 4096);
    assert(closes == before + 2);
    lpr_drm_mapping_unmapped(0x60000, 4096);
    assert(closes == before + 3);
}

int main(void)
{
    pacha_fd_t high_local, high_remote;
    pair_base = 0x80000000ull;
    assert(!lpr_native_wait_pair(&high_local, &high_remote));
    assert(high_local == 0x80000000ll && high_remote == 0x80000001ll);
    lpr_event_backend_t event = { .wait_fd.raw = high_local, .notify_fd.raw = high_remote };
    assert(event.wait_fd.raw == high_local && event.notify_fd.raw == high_remote);
    assert(lpr_drm_map_received(90, 0xfffffffdll, 0x50000, 4096, 3, 1, 0) == 0x50000);
    assert(!lpr_drm_native_munmap(0x50000, 4096));
    assert(high_closed == 0xfffffffdull);
    pair_base = 80;
    check_unmap_reuse();
    check_native_replacements();
#if defined(LPR_UNMAP_PROFILE) && LPR_UNMAP_PROFILE
    lpr_unmap_profile_reset();
    for (unsigned i = 0; i < 65536; ++i)
        assert(!lpr_drm_native_munmap(0x50000, 4096));
    assert(profile_logs == 1 && lpr_unmap_profile_totals.value[0] == 65536);
    lpr_drm_mapping_fork_child();
    assert(lpr_unmap_profile_totals.value[0] == 0);
#endif
    closes = 0;
    pacha_fd_t local, remote;
    assert(!lpr_native_wait_pair(&local, &remote));
    assert(pair_flags == PACHA_FD_FLAG_INHERIT);
    assert(!lpr_native_wait_pair_flags(&local, &remote,
        PACHA_FD_FLAG_INHERIT | PACHA_FD_FLAG_CLOEXEC));
    assert(pair_flags == (PACHA_FD_FLAG_INHERIT | PACHA_FD_FLAG_CLOEXEC));
    assert(lpr_drm_map_received(90, 80, 0x10000, 12288, 3, 1, 0) == 0x10000);
    lpr_drm_mapping_unmapped(0x11000, 4096);
    assert(closes == 0);
    lpr_drm_mapping_unmapped(0x10000, 4096);
    assert(closes == 0);
    lpr_drm_mapping_unmapped(0x12000, 4096);
    assert(closes == 1);
    /* Fixed replacement closes the old lease, retaining the new lease. */
    assert(lpr_drm_map_received(90, 80, 0x10000, 4096, 3, 1, 0) == 0x10000);
    assert(lpr_drm_map_received(90, 81, 0x10000, 4096, 3, 0x11, 0) == 0x10000);
    assert(closes == 2);
    lpr_drm_mapping_unmapped(0x10000, 4096);
    assert(closes == 3);
    assert(lpr_dmabuf_mmap(4, 0x10000, 4096, 3, 1, 0) == 0x10000);
    lpr_drm_mapping_unmapped(0x10000, 4096);
    assert(closes == 4);
    dmabuf.token = 123;
    const int64_t mapped = lpr_dmabuf_mmap(4, 0x10000, 4096, 3, 1, 0);
    if (mapped != 0x10000) fprintf(stderr, "prime map result=%lld\n", (long long)mapped);
    assert(mapped == 0x10000);
    assert(pair_flags == (PACHA_FD_FLAG_INHERIT | PACHA_FD_FLAG_CLOEXEC));
    assert(closes == 5); /* remote endpoint transferred to GPUD */
    lpr_drm_mapping_unmapped(0x10000, 4096);
    assert(closes == 6);
    /* A multi-process desktop exhausted the old 64-entry global pool.
     * A single client must also be able to track the larger shared budget. */
    for (unsigned i = 0; i < LPR_DRM_MAPPING_LEASE_CAPACITY; ++i)
        assert(lpr_drm_map_received(90, 80, 0x100000 + i * 4096,
            4096, 3, 1, 0) == 0x100000 + (int64_t)i * 4096);
    assert(LPR_DRM_MAPPING_LEASE_CAPACITY > 64);
    assert(lpr_drm_map_received(90, 80, 0x900000, 4096, 3, 1, 0) == -LPR_LINUX_EMFILE);
    lpr_drm_mapping_unmapped(0x100000 + 64 * 4096, 4096);
    assert(lpr_drm_map_received(90, 80, 0x900000, 4096, 3, 1, 0) == 0x900000);
    lpr_drm_mapping_unmapped(0x100000, LPR_DRM_MAPPING_LEASE_CAPACITY * 4096);
    lpr_drm_mapping_unmapped(0x900000, 4096);
    assert(closes == 6 + LPR_DRM_MAPPING_LEASE_CAPACITY + 1);
    puts("DRM mapping lease flags, split-unmap and fixed-replacement PASS");
}

/* Request preparation and reply rendering are fixture phases within one
 * synchronous syscall. Their transaction tags never become caller FDs. */
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b)
{
    assert(nr != PACHAOS_SYSCALL_IPC_CALL);
    return fixture_prepare_request(nr, a, b);
}
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return fixture_finish_request(nr, a, b, c, d);
}
int64_t lpr_ipc_message(uint64_t nr, uint64_t endpoint,
    uint64_t request, uint64_t reply, uint64_t timeout, uint64_t flags, int *sent)
{
    if (nr != PACHA_IPC_SYSCALL_CALL) return test_lpr_ipc_noncall(nr, endpoint, request, reply, timeout, flags, sent);

    assert(nr == PACHAOS_SYSCALL_IPC_CALL && request && reply);
    assert(timeout == UINT64_MAX && flags == PACHA_WAIT_UNINTERRUPTIBLE);
    *sent = 0;
    int64_t transaction = fixture_prepare_request(nr, endpoint, request);
    if (transaction < 16) return transaction;
    *sent = 1;
    int64_t status = fixture_finish_request(PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)transaction, reply, timeout, flags);

    return status;
}
