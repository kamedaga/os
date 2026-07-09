#include "pacha/ipc.h"
#include "pacha/syscall.h"

int pacha_ipc_endpoint_create(uint64_t rights, uint32_t flags) {
    return pacha_fd_result_to_int(pacha_syscall2(PACHA_IPC_SYSCALL_ENDPOINT_CREATE, rights, flags));
}

int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *out, uint64_t rights, uint32_t flags) {
    if (!out) return -1;
    uint64_t pair[2] = {0, 0};
    const long status = pacha_syscall3(PACHA_IPC_SYSCALL_CHANNEL_CREATE, (uint64_t)(uintptr_t)pair, rights, flags);
    if (status != 0) return -(int)status;
    out->a = (int)pair[0];
    out->b = (int)pair[1];
    return 0;
}

int pacha_ipc_send(int fd, const struct pacha_ipc_msg *msg) {
    if (!msg) return -1;
    return pacha_status_to_int(pacha_syscall2(PACHA_IPC_SYSCALL_SEND, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)msg));
}

int pacha_ipc_recv(int fd, struct pacha_ipc_msg *msg) {
    if (!msg) return -1;
    return pacha_status_to_int(pacha_syscall2(PACHA_IPC_SYSCALL_RECV, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)msg));
}

int pacha_ipc_recv_wait(int fd, struct pacha_ipc_msg *msg, uint64_t timeout_ticks) {
    if (!msg) return -1;
    return pacha_status_to_int(pacha_syscall4(
        PACHA_IPC_SYSCALL_RECV_WAIT,
        (uint64_t)(uint32_t)fd,
        (uint64_t)(uintptr_t)msg,
        timeout_ticks,
        0));
}

int pacha_ipc_call(int fd, const struct pacha_ipc_msg *msg) {
    if (!msg) return -1;
    return pacha_fd_result_to_int(pacha_syscall2(PACHA_IPC_SYSCALL_CALL, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)msg));
}

int pacha_ipc_reply(int reply_fd, const struct pacha_ipc_msg *msg) {
    if (!msg) return -1;
    return pacha_status_to_int(pacha_syscall2(PACHA_IPC_SYSCALL_REPLY, (uint64_t)(uint32_t)reply_fd, (uint64_t)(uintptr_t)msg));
}

int pacha_process_create(uint64_t rights, uint32_t flags) {
    return pacha_fd_result_to_int(pacha_syscall3(PACHA_PROCESS_SYSCALL_CREATE, 0, rights, flags));
}

int pacha_process_clone(uint64_t rights, uint32_t flags) {
    return pacha_fd_result_to_int(pacha_syscall2(PACHA_PROCESS_SYSCALL_CLONE, rights, flags));
}

int pacha_thread_create(int process_fd, uint64_t entry_rip, uint64_t stack_rsp, uint64_t flags, uint64_t fs_base, uint64_t rights) {
    return pacha_fd_result_to_int(pacha_syscall6(
        PACHA_THREAD_SYSCALL_CREATE,
        (uint64_t)(uint32_t)process_fd,
        entry_rip,
        stack_rsp,
        flags,
        fs_base,
        rights
    ));
}

int pacha_thread_start(int thread_fd) {
    return pacha_status_to_int(pacha_syscall1(PACHA_THREAD_SYSCALL_START, (uint64_t)(uint32_t)thread_fd));
}

int pacha_thread_set_gs_base(uint64_t gs_base) {
    return pacha_status_to_int(pacha_syscall1(PACHA_THREAD_SYSCALL_SET_GS_BASE, gs_base));
}

long pacha_process_map(int process_fd, int vmo_fd, uint64_t target_va, uint64_t size, uint64_t prot, uint64_t vmo_offset) {
    return pacha_process_map_flags(process_fd, vmo_fd, target_va, size, prot, vmo_offset, PACHA_PROCESS_MAP_SHARED);
}

long pacha_process_map_flags(
    int process_fd,
    int vmo_fd,
    uint64_t target_va,
    uint64_t size,
    uint64_t prot,
    uint64_t vmo_offset,
    uint64_t flags)
{
    return pacha_syscall6(
        PACHA_PROCESS_SYSCALL_MAP,
        (uint64_t)(uint32_t)process_fd,
        (uint64_t)(uint32_t)vmo_fd,
        target_va,
        size,
        prot,
        vmo_offset | flags
    );
}

int pacha_process_map_batch(
    int process_fd,
    const struct pacha_process_map_batch_entry *entries,
    uint64_t entry_count)
{
    return pacha_status_to_int(pacha_syscall3(
        PACHA_PROCESS_SYSCALL_MAP_BATCH,
        (uint64_t)(uint32_t)process_fd,
        (uint64_t)(uintptr_t)entries,
        entry_count
    ));
}

long pacha_getrandom(void *buf, uint64_t len, uint64_t flags) {
    return pacha_syscall3(PACHA_RUNTIME_SYSCALL_GETRANDOM, (uint64_t)(uintptr_t)buf, len, flags);
}

int pacha_fd_get_info(int fd, struct pacha_fd_info *out) {
    if (!out) return -1;
    return pacha_status_to_int(pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)out));
}

int pacha_fd_close(int fd) {
    return pacha_status_to_int(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, (uint64_t)(uint32_t)fd));
}

long pacha_fd_read(int fd, void *buf, uint64_t len) {
    return pacha_syscall3(PACHA_FD_SYSCALL_READ, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)buf, len);
}

long pacha_fd_write(int fd, const void *buf, uint64_t len) {
    return pacha_syscall3(PACHA_FD_SYSCALL_WRITE, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)buf, len);
}

long pacha_fd_readv(int fd, const struct pacha_iovec *iov, uint64_t iov_count) {
    return pacha_syscall3(PACHA_FD_SYSCALL_READV, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)iov, iov_count);
}

long pacha_fd_writev(int fd, const struct pacha_iovec *iov, uint64_t iov_count) {
    return pacha_syscall3(PACHA_FD_SYSCALL_WRITEV, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)iov, iov_count);
}

long pacha_fd_fcntl(int fd, uint64_t cmd, uint64_t arg0, uint64_t arg1) {
    return pacha_syscall4(PACHA_FD_SYSCALL_FCNTL, (uint64_t)(uint32_t)fd, cmd, arg0, arg1);
}

long pacha_fd_poll(struct pacha_pollfd *fds, uint64_t count) {
    return pacha_syscall2(PACHA_FD_SYSCALL_POLL, (uint64_t)(uintptr_t)fds, count);
}

static long pacha_fd_ready_count(const struct pacha_pollfd *fds, uint64_t count) {
    long ready = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (fds[i].revents != 0) {
            ready += 1;
        }
    }
    return ready;
}

long pacha_fd_wait_many(struct pacha_pollfd *fds, uint64_t count, uint64_t timeout_ticks) {
    for (;;) {
        const long ret = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY, (uint64_t)(uintptr_t)fds, count, timeout_ticks, 0);
        if (ret > 0 && fds != NULL && (uint64_t)ret <= count) {
            const long ready = pacha_fd_ready_count(fds, count);
            if (ready != 0) {
                return ready;
            }
        }
        if (ret == PACHA_SYSCALL_ERR_NOT_READY || ret == PACHA_ERR_NOT_READY) {
            if (timeout_ticks == 0) return PACHA_ERR_NOT_READY;
            if (timeout_ticks != UINT64_MAX) return PACHA_ERR_NOT_READY;
            continue;
        }
        if (ret > 0 && ret <= PACHA_SYSCALL_ERR_CLOSED) {
            return -ret;
        }
        return ret;
    }
}

int pacha_eventfd_create(uint64_t initial_value, uint64_t rights, uint32_t fd_flags) {
    return pacha_fd_result_to_int(pacha_syscall3(PACHA_FD_SYSCALL_EVENTFD_CREATE, initial_value, rights, fd_flags));
}

int pacha_timerfd_create(uint64_t initial_ns, uint64_t interval_ns, uint64_t rights, uint32_t fd_flags) {
    return pacha_fd_result_to_int(pacha_syscall6(
        PACHA_FD_SYSCALL_TIMERFD_CREATE,
        PACHA_TIMERFD_CLOCK_MONOTONIC,
        0,
        initial_ns,
        interval_ns,
        rights,
        fd_flags
    ));
}

int pacha_timerfd_settime(int fd, uint64_t initial_ns, uint64_t interval_ns, uint64_t flags) {
    struct pacha_timer_spec {
        uint64_t interval_sec;
        uint64_t interval_nsec;
        uint64_t value_sec;
        uint64_t value_nsec;
    };
    const struct pacha_timer_spec spec = {
        .interval_sec = interval_ns / 1000000000ull,
        .interval_nsec = interval_ns % 1000000000ull,
        .value_sec = initial_ns / 1000000000ull,
        .value_nsec = initial_ns % 1000000000ull,
    };
    return pacha_status_to_int(pacha_syscall4(
        PACHA_FD_SYSCALL_TIMERFD_SETTIME,
        (uint64_t)(uint32_t)fd,
        flags,
        (uint64_t)(uintptr_t)&spec,
        0
    ));
}

int pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags) {
    return pacha_fd_result_to_int(pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, size, rights, flags));
}

int pacha_vmo_revoke(int fd) {
    return pacha_status_to_int(pacha_syscall1(PACHA_FD_SYSCALL_VMO_REVOKE, (uint64_t)(uint32_t)fd));
}

void *pacha_mmap(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset) {
    const long result = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, (uint64_t)(uint32_t)fd, 0, size, prot, flags, offset);
    if (result < 4096) return (void *)0;
    return (void *)(uintptr_t)result;
}

void *pacha_mmap_anonymous(uint64_t size, uint64_t prot, uint64_t flags) {
    const long result = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, 0, size, prot, flags | PACHA_MMAP_ANONYMOUS, 0);
    if (result < 4096) return (void *)0;
    return (void *)(uintptr_t)result;
}

int pacha_munmap(void *addr, uint64_t size) {
    return pacha_status_to_int(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)addr, size));
}

static long pacha_mmap_raw(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset) {
    return pacha_syscall6(PACHA_VM_SYSCALL_MMAP, (uint64_t)(uint32_t)fd, 0, size, prot, flags, offset);
}

uint64_t pacha_mmap_pkey_flags(uint64_t flags, uint32_t pkey) {
    flags &= ~PACHA_MMAP_PKEY_MASK;
    return flags | (((uint64_t)pkey & 0xfu) << PACHA_MMAP_PKEY_SHIFT);
}

enum {
    PACHA_CPUID7_ECX_PKU = 1u << 3,
    PACHA_CPUID7_ECX_OSPKE = 1u << 4,
    PACHA_IPC_FAST_SETUP_MAGIC = 0x50494643u,
};

static void pacha_cpuid7(uint32_t *ecx_out) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7u), "c"(0u));
    (void)eax;
    (void)ebx;
    (void)edx;
    *ecx_out = ecx;
}

int pacha_ipc_pkey_supported(void) {
    uint32_t ecx = 0;
    pacha_cpuid7(&ecx);
    return (ecx & PACHA_CPUID7_ECX_PKU) != 0;
}

int pacha_ipc_pkey_enabled(void) {
    uint32_t ecx = 0;
    pacha_cpuid7(&ecx);
    return (ecx & (PACHA_CPUID7_ECX_PKU | PACHA_CPUID7_ECX_OSPKE)) == (PACHA_CPUID7_ECX_PKU | PACHA_CPUID7_ECX_OSPKE);
}

uint32_t pacha_ipc_pkru_read(void) {
    uint32_t eax;
    uint32_t edx;
    __asm__ volatile(
        "rdpkru"
        : "=a"(eax), "=d"(edx)
        : "c"(0u));
    (void)edx;
    return eax;
}

void pacha_ipc_pkru_write(uint32_t pkru) {
    __asm__ volatile(
        "wrpkru"
        :
        : "a"(pkru), "c"(0u), "d"(0u)
        : "memory");
}

uint32_t pacha_ipc_pkey_disable_mask(uint32_t pkey) {
    if (pkey == 0 || pkey > 15) return 0;
    return 0x3u << (pkey * 2u);
}

void pacha_ipc_pkey_open(uint32_t pkey, uint32_t *saved_pkru) {
    if (pkey == 0 || pkey > 15) {
        if (saved_pkru) *saved_pkru = 0;
        return;
    }
    const uint32_t old_pkru = pacha_ipc_pkru_read();
    if (saved_pkru) *saved_pkru = old_pkru;
    pacha_ipc_pkru_write(old_pkru & ~pacha_ipc_pkey_disable_mask(pkey));
}

void pacha_ipc_pkey_close(uint32_t pkey, uint32_t saved_pkru) {
    if (pkey == 0 || pkey > 15) return;
    pacha_ipc_pkru_write(saved_pkru);
}

static void pacha_ipc_ring_zero(struct pacha_ipc_fast_ring *ring) {
    volatile uint64_t *words = (volatile uint64_t *)(void *)ring;
    const uint64_t count = sizeof(*ring) / sizeof(uint64_t);
    for (uint64_t i = 0; i < count; i++) words[i] = 0;
}

static void pacha_ipc_ring_init(struct pacha_ipc_fast_ring *ring, uint64_t generation) {
    pacha_ipc_ring_zero(ring);
    ring->magic = 0x5041434841495043ull;
    ring->generation = generation;
    ring->capacity = PACHA_IPC_FAST_RING_ENTRIES;
}

static int pacha_ipc_ring_push(struct pacha_ipc_fast_ring *ring, const struct pacha_ipc_fast_entry *entry) {
    if (!ring || !entry) return -1;
    if (ring->magic != 0x5041434841495043ull) return -2;
    if (ring->capacity == 0 || ring->capacity > PACHA_IPC_FAST_RING_ENTRIES) return -3;
    const uint64_t producer = ring->producer;
    const uint64_t consumer = ring->consumer;
    if (producer - consumer >= ring->capacity) return -4;
    const uint64_t slot = producer % ring->capacity;
    ring->entries[slot] = *entry;
    ring->entries[slot].seq = producer + 1;
    ring->entries[slot].generation = ring->generation;
    __asm__ volatile("" ::: "memory");
    ring->producer = producer + 1;
    return 0;
}

static int pacha_ipc_ring_pop(struct pacha_ipc_fast_ring *ring, struct pacha_ipc_fast_entry *out) {
    if (!ring || !out) return -1;
    if (ring->magic != 0x5041434841495043ull) return -2;
    if (ring->capacity == 0 || ring->capacity > PACHA_IPC_FAST_RING_ENTRIES) return -3;
    const uint64_t consumer = ring->consumer;
    const uint64_t producer = ring->producer;
    if (consumer == producer) return -4;
    const uint64_t slot = consumer % ring->capacity;
    *out = ring->entries[slot];
    if (out->seq != consumer + 1 || out->generation != ring->generation) return -5;
    __asm__ volatile("" ::: "memory");
    ring->consumer = consumer + 1;
    return 0;
}

static int pacha_ipc_normal_send_entry(int fd, const struct pacha_ipc_fast_entry *entry) {
    if (fd < 16 || !entry) return -1;
    const struct pacha_ipc_msg msg = {
        .word0 = entry->op,
        .word1 = entry->offset,
        .word2 = entry->len,
        .word3 = (entry->flags & 0xffffffffull) | (entry->status << 32),
    };
    return pacha_ipc_send(fd, &msg);
}

static int pacha_ipc_normal_recv_entry(int fd, struct pacha_ipc_fast_entry *out) {
    if (fd < 16 || !out) return -1;
    struct pacha_ipc_msg msg = {0};
    const int status = pacha_ipc_recv(fd, &msg);
    if (status != 0) return status;
    *out = (struct pacha_ipc_fast_entry){
        .op = msg.word0,
        .offset = msg.word1,
        .len = msg.word2,
        .flags = msg.word3 & 0xffffffffull,
        .status = msg.word3 >> 32,
    };
    return 0;
}

static uint64_t pacha_page_align_up(uint64_t value) {
    return (value + 4095u) & ~4095ull;
}

static int pacha_ipc_fast_pkey_available(uint32_t flags, uint32_t pkey, struct pacha_ipc_fast_channel *fast) {
    const int wants_pkey = (flags & (PACHA_IPC_FAST_F_PREFER_PKEY | PACHA_IPC_FAST_F_REQUIRE_PKEY)) != 0;
    const int requires_pkey = (flags & PACHA_IPC_FAST_F_REQUIRE_PKEY) != 0;
    if (!wants_pkey) {
        fast->fallback_reason = PACHA_IPC_FAST_FALLBACK_PKEY_NOT_REQUESTED;
        return 0;
    }
    if (pkey == 0 || pkey > 15) {
        fast->fallback_reason = PACHA_IPC_FAST_FALLBACK_PKEY_INVALID;
        return requires_pkey ? -1 : 0;
    }
    if (!pacha_ipc_pkey_enabled()) {
        fast->fallback_reason = PACHA_IPC_FAST_FALLBACK_PKEY_UNAVAILABLE;
        return requires_pkey ? -1 : 0;
    }
    return 1;
}

static int pacha_ipc_fast_map_pkey_rings(
    struct pacha_ipc_fast_channel *fast,
    int req_fd,
    int comp_fd,
    uint32_t pkey,
    int offer_side
) {
    const uint64_t ring_size = pacha_page_align_up(sizeof(struct pacha_ipc_fast_ring));
    const uint64_t mmap_flags = pacha_mmap_pkey_flags(PACHA_MMAP_SHARED, pkey);
    long request_raw = pacha_mmap_raw(req_fd, ring_size, PACHA_PROT_READ | PACHA_PROT_WRITE, mmap_flags, 0);
    long completion_raw = pacha_mmap_raw(comp_fd, ring_size, PACHA_PROT_READ | PACHA_PROT_WRITE, mmap_flags, 0);
    struct pacha_ipc_fast_ring *request = request_raw >= 4096 ? (struct pacha_ipc_fast_ring *)(uintptr_t)request_raw : 0;
    struct pacha_ipc_fast_ring *completion = completion_raw >= 4096 ? (struct pacha_ipc_fast_ring *)(uintptr_t)completion_raw : 0;
    if (!request || !completion) {
        fast->last_error = !request ? (int)request_raw : (int)completion_raw;
        fast->fallback_reason = PACHA_IPC_FAST_FALLBACK_PKEY_MMAP_FAILED;
        return -4;
    }

    fast->request_vmo_fd = req_fd;
    fast->completion_vmo_fd = comp_fd;
    fast->request = request;
    fast->completion = completion;
    fast->tx = offer_side ? request : completion;
    fast->rx = offer_side ? completion : request;
    fast->backend = PACHA_IPC_BACKEND_PKEY_RING;
    fast->fallback_reason = PACHA_IPC_FAST_FALLBACK_NONE;
    fast->pkey = pkey;
    const uint32_t old_pkru = pacha_ipc_pkru_read();
    pacha_ipc_pkru_write(old_pkru | pacha_ipc_pkey_disable_mask(pkey));
    return 0;
}

static void pacha_ipc_fast_init_ring_pair(struct pacha_ipc_fast_channel *fast) {
    uint32_t saved = 0;
    if (fast->backend == PACHA_IPC_BACKEND_PKEY_RING) pacha_ipc_pkey_open(fast->pkey, &saved);
    pacha_ipc_ring_init(fast->request, 1);
    pacha_ipc_ring_init(fast->completion, 1);
    if (fast->backend == PACHA_IPC_BACKEND_PKEY_RING) pacha_ipc_pkey_close(fast->pkey, saved);
}

int pacha_ipc_fast_channel_init_local(struct pacha_ipc_fast_channel *fast, int channel_fd, uint32_t flags, uint32_t pkey) {
    if (!fast) return -1;
    *fast = (struct pacha_ipc_fast_channel){
        .channel_fd = channel_fd,
        .request_vmo_fd = -1,
        .completion_vmo_fd = -1,
        .backend = PACHA_IPC_BACKEND_NORMAL,
        .fallback_reason = PACHA_IPC_FAST_FALLBACK_NONE,
        .pkey = 0,
        .flags = flags,
        .last_error = 0,
    };

    const int requires_pkey = (flags & PACHA_IPC_FAST_F_REQUIRE_PKEY) != 0;
    const int can_pkey = pacha_ipc_fast_pkey_available(flags, pkey, fast);
    if (can_pkey <= 0) {
        if (can_pkey < 0) return -2;
        if (channel_fd >= 16 && !requires_pkey) return 0;
        return -2;
    }

    const uint64_t rights = PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const uint64_t ring_size = pacha_page_align_up(sizeof(struct pacha_ipc_fast_ring));
    const int req_fd = pacha_vmo_create(ring_size, rights, 0);
    const int comp_fd = pacha_vmo_create(ring_size, rights, 0);
    if (req_fd < 16 || comp_fd < 16) {
        fast->fallback_reason = PACHA_IPC_FAST_FALLBACK_VMO_CREATE_FAILED;
        if (channel_fd >= 16 && !requires_pkey) return 0;
        return -3;
    }

    if (pacha_ipc_fast_map_pkey_rings(fast, req_fd, comp_fd, pkey, 1) != 0) {
        if (channel_fd >= 16 && !requires_pkey) return 0;
        return -4;
    }

    pacha_ipc_fast_init_ring_pair(fast);
    return 0;
}

int pacha_ipc_fast_channel_init_normal(struct pacha_ipc_fast_channel *fast, int channel_fd) {
    if (!fast || channel_fd < 16) return -1;
    *fast = (struct pacha_ipc_fast_channel){
        .channel_fd = channel_fd,
        .request_vmo_fd = -1,
        .completion_vmo_fd = -1,
        .backend = PACHA_IPC_BACKEND_NORMAL,
        .fallback_reason = PACHA_IPC_FAST_FALLBACK_NONE,
        .pkey = 0,
        .flags = 0,
        .last_error = 0,
    };
    return 0;
}

int pacha_ipc_fast_channel_offer(struct pacha_ipc_fast_channel *fast, int control_fd, uint32_t flags, uint32_t pkey) {
    if (!fast) return -1;
    const int status = pacha_ipc_fast_channel_init_local(fast, control_fd, flags, pkey);
    if (status != 0) return status;
    if (fast->backend == PACHA_IPC_BACKEND_NORMAL) {
        const struct pacha_ipc_msg msg = {
            .word0 = PACHA_IPC_FAST_SETUP_MAGIC,
            .word1 = 0,
            .word2 = flags,
            .word3 = fast->fallback_reason,
            .fd_count = 0,
        };
        return pacha_ipc_send(control_fd, &msg);
    }

    struct pacha_ipc_fd fds[2] = {
        {
            .fd = (uint64_t)(uint32_t)fast->request_vmo_fd,
            .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,
            .flags = 0,
            .transfer_flags = 0,
        },
        {
            .fd = (uint64_t)(uint32_t)fast->completion_vmo_fd,
            .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,
            .flags = 0,
            .transfer_flags = 0,
        },
    };
    struct pacha_ipc_msg msg = {
        .word0 = PACHA_IPC_FAST_SETUP_MAGIC,
        .word1 = sizeof(struct pacha_ipc_fast_ring),
        .word2 = flags,
        .word3 = pkey,
        .fds = fds,
        .fd_count = 2,
    };
    return pacha_ipc_send(control_fd, &msg);
}

int pacha_ipc_fast_channel_accept(struct pacha_ipc_fast_channel *fast, int control_fd, uint32_t flags, uint32_t pkey) {
    if (!fast) return -1;
    *fast = (struct pacha_ipc_fast_channel){
        .channel_fd = control_fd,
        .request_vmo_fd = -1,
        .completion_vmo_fd = -1,
        .backend = PACHA_IPC_BACKEND_NORMAL,
        .fallback_reason = PACHA_IPC_FAST_FALLBACK_NONE,
        .flags = flags,
        .last_error = 0,
    };

    struct pacha_ipc_fd fds[2] = {0};
    struct pacha_ipc_msg msg = {
        .fds = fds,
        .fd_capacity = 2,
    };
    const int recv_status = pacha_ipc_recv(control_fd, &msg);
    if (recv_status != 0) return recv_status;
    if (msg.word0 != PACHA_IPC_FAST_SETUP_MAGIC) return -2;
    if (msg.fd_count == 0) {
        fast->fallback_reason = (enum pacha_ipc_fast_fallback_reason)msg.word3;
        return 0;
    }
    if (msg.fd_count != 2) return -2;
    if (msg.word1 > pacha_page_align_up(sizeof(struct pacha_ipc_fast_ring))) return -3;
    const uint32_t remote_flags = (uint32_t)msg.word2;
    const uint32_t remote_pkey = (uint32_t)msg.word3;
    const uint32_t effective_flags = flags | (remote_flags & (PACHA_IPC_FAST_F_PREFER_PKEY | PACHA_IPC_FAST_F_REQUIRE_PKEY));
    const uint32_t effective_pkey = pkey != 0 ? pkey : remote_pkey;
    const int can_pkey = pacha_ipc_fast_pkey_available(effective_flags, effective_pkey, fast);
    if (can_pkey <= 0) return can_pkey < 0 ? -2 : 0;
    return pacha_ipc_fast_map_pkey_rings(fast, (int)fds[0].fd, (int)fds[1].fd, effective_pkey, 0);
}

int pacha_ipc_fast_channel_ready(const struct pacha_ipc_fast_channel *fast) {
    if (!fast) return 0;
    if (fast->backend == PACHA_IPC_BACKEND_NORMAL) return fast->channel_fd >= 16;
    return fast->tx != 0 && fast->rx != 0;
}

int pacha_ipc_fast_channel_uses_ring(const struct pacha_ipc_fast_channel *fast) {
    if (!pacha_ipc_fast_channel_ready(fast)) return 0;
    return fast->backend == PACHA_IPC_BACKEND_PKEY_RING;
}

void pacha_ipc_fast_entry_init(struct pacha_ipc_fast_entry *entry, uint64_t op, uint64_t offset, uint64_t len, uint64_t flags) {
    if (!entry) return;
    *entry = (struct pacha_ipc_fast_entry){
        .op = op,
        .offset = offset,
        .len = len,
        .flags = flags,
    };
}

int pacha_ipc_fast_send(struct pacha_ipc_fast_channel *fast, const struct pacha_ipc_fast_entry *entry) {
    if (!fast || !entry) return -1;
    if (!pacha_ipc_fast_channel_ready(fast)) return -2;
    if (fast->backend == PACHA_IPC_BACKEND_NORMAL) return pacha_ipc_normal_send_entry(fast->channel_fd, entry);
    uint32_t saved = 0;
    if (fast->backend == PACHA_IPC_BACKEND_PKEY_RING) pacha_ipc_pkey_open(fast->pkey, &saved);
    const int status = pacha_ipc_ring_push(fast->tx, entry);
    if (fast->backend == PACHA_IPC_BACKEND_PKEY_RING) pacha_ipc_pkey_close(fast->pkey, saved);
    return status;
}

int pacha_ipc_fast_recv(struct pacha_ipc_fast_channel *fast, struct pacha_ipc_fast_entry *out) {
    if (!fast || !out) return -1;
    if (!pacha_ipc_fast_channel_ready(fast)) return -2;
    if (fast->backend == PACHA_IPC_BACKEND_NORMAL) return pacha_ipc_normal_recv_entry(fast->channel_fd, out);
    uint32_t saved = 0;
    if (fast->backend == PACHA_IPC_BACKEND_PKEY_RING) pacha_ipc_pkey_open(fast->pkey, &saved);
    const int status = pacha_ipc_ring_pop(fast->rx, out);
    if (fast->backend == PACHA_IPC_BACKEND_PKEY_RING) pacha_ipc_pkey_close(fast->pkey, saved);
    return status;
}

int pacha_ipc_fast_call(struct pacha_ipc_fast_channel *fast, const struct pacha_ipc_fast_entry *request, struct pacha_ipc_fast_entry *response) {
    if (!fast || !request || !response) return -1;
    const int send_status = pacha_ipc_fast_send(fast, request);
    if (send_status != 0) return send_status;
    return pacha_ipc_fast_recv(fast, response);
}

int pacha_ipc_fast_serve_once(struct pacha_ipc_fast_channel *fast, pacha_ipc_fast_handler_fn handler, void *ctx) {
    if (!fast || !handler) return -1;
    struct pacha_ipc_fast_entry request = {0};
    struct pacha_ipc_fast_entry response = {0};
    const int recv_status = pacha_ipc_fast_recv(fast, &request);
    if (recv_status != 0) return recv_status;
    const int handler_status = handler(ctx, &request, &response);
    if (handler_status != 0) {
        response.op = request.op;
        response.offset = request.offset;
        response.len = 0;
        response.flags = request.flags;
        response.status = (uint64_t)(int64_t)handler_status;
    }
    const int send_status = pacha_ipc_fast_send(fast, &response);
    return send_status != 0 ? send_status : handler_status;
}

const char *pacha_ipc_fast_backend_name(enum pacha_ipc_fast_backend backend) {
    switch (backend) {
        case PACHA_IPC_BACKEND_NORMAL: return "normal";
        case PACHA_IPC_BACKEND_PKEY_RING: return "pkey_ring";
        default: return "unknown";
    }
}

const char *pacha_ipc_fast_fallback_reason_name(enum pacha_ipc_fast_fallback_reason reason) {
    switch (reason) {
        case PACHA_IPC_FAST_FALLBACK_NONE: return "none";
        case PACHA_IPC_FAST_FALLBACK_PKEY_NOT_REQUESTED: return "pkey_not_requested";
        case PACHA_IPC_FAST_FALLBACK_PKEY_INVALID: return "pkey_invalid";
        case PACHA_IPC_FAST_FALLBACK_PKEY_UNAVAILABLE: return "pkey_unavailable";
        case PACHA_IPC_FAST_FALLBACK_VMO_CREATE_FAILED: return "vmo_create_failed";
        case PACHA_IPC_FAST_FALLBACK_PKEY_MMAP_FAILED: return "pkey_mmap_failed";
        default: return "unknown";
    }
}
