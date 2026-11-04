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
static int cpu_testing, cpu_status, cpu_calls, cpu_bad_fd, cpu_bad_reply;
static uint64_t cpu_flags, cpu_rights = UINT64_MAX;
int64_t lpr_sync_file_create_signaled(void) { assert(0); return -1; }
int lpr_linux_sync_file_fd_active(uint64_t fd) { (void)fd; assert(0); return 0; }
pacha_fd_t lpr_sync_file_duplicate_wait(uint64_t fd) { (void)fd; assert(0); return -1; }

static atomic_uint unmap_race_stage;
static int unmap_failure;
static int64_t map_failure, remap_failure;
static unsigned hint_testing, hint_calls;
static int hint_occupied;
static uint64_t hint_seen_address[10], hint_seen_flags[10];
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
    assert(cpu_testing ? msg->fd_count == 1 : (msg->fd_count == 2 && msg->fds[1].fd == 81));
    request_id = msg->word3;
    if (cpu_testing) {
        const pacha_service_envelope_t *h = (void *)wire_page;
        const gpud_drm_prime_cpu_sync_request_t *r = lpr_gpud_drm_payload(wire_page);
        assert(h->op == GPUD_DRM_OP_PRIME_CPU_SYNC && h->payload_size == sizeof(*r));
        assert(r->token == 123 && r->flags == cpu_flags); ++cpu_calls;
    }
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
    if (cpu_testing) {
        if (cpu_bad_fd) return 0;
        *info = (struct pacha_fd_info){.kind=fd==90?PACHA_FD_KIND_VMO:PACHA_FD_KIND_CHANNEL,
            .size=fd==90?4096:0,.rights=fd==90?cpu_rights:GPUD_DRM_PRIME_OWNER_RIGHTS};
        return 1;
    }
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
        ((pacha_service_envelope_t *)wire_page)->status = cpu_testing ? cpu_status : 0;
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
    if (hint_testing) {
        assert(hint_calls < 10 && length == 8192 && prot == 3 && offset == 4096);
        hint_seen_address[hint_calls] = address;
        hint_seen_flags[hint_calls++] = flags;
        if (flags & PACHAOS_MMAP_FIXED_NOREPLACE)
            return hint_occupied == 1 || (hint_occupied == 2 && hint_calls < 3) ? -17 : (int64_t)address;
        return 0x90000;
    }
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

static void check_mmap_hint(void)
{
    hint_testing = 1;
    const uint64_t ordinary = PACHAOS_MMAP_PRIVATE;
    assert(lpr_drm_native_mmap(90, 0x5007b, 8192, 3, ordinary, 4096) == 0x50000);
    assert(hint_calls == 1 && hint_seen_address[0] == 0x50000);
    assert(hint_seen_flags[0] == (ordinary | PACHAOS_MMAP_FIXED_NOREPLACE));
    hint_calls = 0; hint_occupied = 1;
    assert(lpr_drm_native_mmap(90, 0x5007b, 8192, 3, ordinary, 4096) == 0x90000);
    assert(hint_calls == 9 && hint_seen_address[8] == 0x5007b);
    for (unsigned i = 0; i < 8; ++i)
        assert(hint_seen_address[i] == 0x50000 + i * 8192);
    assert(hint_seen_flags[8] == ordinary); /* Never replace an occupied hint. */
    hint_calls = 0; hint_occupied = 2;
    assert(lpr_drm_native_mmap(90, 0x5007b, 8192, 3, ordinary, 4096) == 0x54000);
    assert(hint_calls == 3); /* Skip occupied spans without touching them. */
    hint_occupied = 1;
    hint_calls = 0;
    assert(lpr_drm_native_mmap(90, 0, 8192, 3, ordinary, 4096) == 0x90000);
    assert(hint_calls == 1 && hint_seen_flags[0] == ordinary);
    hint_calls = 0;
    assert(lpr_drm_native_mmap(90, UINT64_C(0x7ffffffff000), 8192, 3,
        ordinary, 4096) == 0x90000);
    assert(hint_calls == 1 && hint_seen_flags[0] == ordinary);
    hint_calls = 0;
    assert(lpr_drm_native_mmap(90, 0x50000, 8192, 3,
        ordinary | PACHAOS_MMAP_FIXED_NOREPLACE, 4096) == -17);
    assert(hint_calls == 1); /* Explicit NOREPLACE must not fall back. */
    hint_calls = 0;
    assert(lpr_drm_native_mmap(90, 0x50000, 8192, 3,
        ordinary | PACHAOS_MMAP_FIXED, 4096) == 0x90000);
    assert(hint_calls == 1 && hint_seen_flags[0] == (ordinary | PACHAOS_MMAP_FIXED));
    hint_testing = hint_calls = hint_occupied = 0;
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

static void cpu_sync_case(void) {
    cpu_testing=1; dmabuf.token=123; dmabuf.lease_fd.raw=81;
    uint64_t flags=1;
    assert(lpr_dmabuf_ioctl(4,0x40086200,0)==-LPR_LINUX_EFAULT);
    const uint64_t valid[]={1,2,3,5,6,7};
    for(unsigned i=0;i<6;++i) {
        flags=cpu_flags=valid[i];
        assert(!lpr_dmabuf_ioctl(4,0x40086200,(uintptr_t)&flags));
    }
    assert(cpu_calls==6);
    const uint64_t invalid[]={0,4,8,UINT64_C(1)<<32|1,UINT64_MAX};
    for(unsigned i=0;i<5;++i) {
        flags=invalid[i];
        assert(lpr_dmabuf_ioctl(4,0x40086200,(uintptr_t)&flags)==-LPR_LINUX_EINVAL);
    }
    flags=1; dmabuf.token=0;
    assert(lpr_dmabuf_ioctl(4,0x40086200,(uintptr_t)&flags)==-LPR_LINUX_EOPNOTSUPP);
    dmabuf.token=123; dmabuf.writable=0; flags=2;
    assert(lpr_dmabuf_ioctl(4,0x40086200,(uintptr_t)&flags)==-LPR_LINUX_EACCES);
    dmabuf.writable=1; cpu_bad_fd=1;
    assert(lpr_dmabuf_ioctl(4,0x40086200,(uintptr_t)&flags)==-LPR_LINUX_EBADF);
    cpu_bad_fd=0; cpu_rights=PACHA_FD_RIGHT_MAP_READ;
    assert(lpr_dmabuf_ioctl(4,0x40086200,(uintptr_t)&flags)==-LPR_LINUX_EACCES);
    cpu_rights=UINT64_MAX; assert(cpu_calls==6);
    const int errors[]={-LPR_LINUX_EINTR,-LPR_LINUX_EAGAIN,-LPR_LINUX_EIO};
    for(unsigned i=0;i<3;++i) {
        cpu_status=errors[i]; flags=cpu_flags=7;
        assert(lpr_dmabuf_ioctl(4,0x40086200,(uintptr_t)&flags)==errors[i]);
    }
    cpu_bad_reply=1; cpu_status=0;
    assert(lpr_dmabuf_ioctl(4,0x40086200,(uintptr_t)&flags)==-LPR_DRM_ERR_PROTOCOL);
    cpu_bad_reply=0;
    lpr_dma_buf_sync_file_t exported={.flags=3,.fd=-1};
    assert(lpr_dmabuf_ioctl(4,LPR_LINUX_DMA_BUF_IOCTL_EXPORT_SYNC_FILE,(uintptr_t)&exported)==-LPR_LINUX_EOPNOTSUPP);
    assert(exported.fd==-1); /* No completed fence may be fabricated. */
    cpu_testing=cpu_status=0; dmabuf.token=0; dmabuf.lease_fd.raw=-1;
    puts("LPR DMA_BUF_IOCTL_SYNC: flags64, rights, token0 refusal, IPC and errno PASS");
}

int main(void)
{
    check_mmap_hint();
    struct pacha_fd_info mapping_lease = {.kind = PACHA_FD_KIND_CHANNEL,
        .rights = PACHA_FD_RIGHT_CLOSE,
        .flags = PACHA_FD_FLAG_CLOEXEC | PACHA_FD_FLAG_INHERIT};
    assert(lpr_drm_mapping_lease_info_valid(&mapping_lease));
    mapping_lease.rights = GPUD_DRM_PRIME_OWNER_RIGHTS;
    assert(!lpr_drm_mapping_lease_info_valid(&mapping_lease));
    mapping_lease.rights = PACHA_FD_RIGHT_CLOSE;
    mapping_lease.flags &= ~PACHA_FD_FLAG_INHERIT;
    assert(!lpr_drm_mapping_lease_info_valid(&mapping_lease));
    assert(!lpr_drm_mapping_lease_info_valid(NULL));
    cpu_sync_case();
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

    if (cpu_testing) {
        assert(endpoint==81 && timeout==UINT64_MAX && flags==PACHA_WAIT_UNINTERRUPTIBLE);
        const struct pacha_ipc_msg *call=(void *)(uintptr_t)request;
        struct pacha_ipc_msg *response=(void *)(uintptr_t)reply;
        assert(call->word0==GPUD_DRM_PRIME_CPU_SYNC_REQUEST_MAGIC && call->word1==123 &&
            call->word2==cpu_flags && !call->word3 && !call->fd_count);
        ++cpu_calls; *sent=1;
        response->word0=GPUD_DRM_PRIME_CPU_SYNC_REPLY_MAGIC;
        response->word1=(uint64_t)(int64_t)cpu_status;
        response->word2=cpu_bad_reply?124:123;
        response->word3=cpu_flags;
        return 0;
    }
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
