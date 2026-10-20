/* SPDX-License-Identifier: MIT */
#include "../lpr_filed_internal.h"
#include "session.h"
#include "diagnostic.h"
#include "../lpr_common/boot_diagnostic.h"
#include <kobox2/virtqueue_x86_64.h>

struct lpr_fs_request {
    struct ph_fs_pending pending;
    unsigned occupied;
};

struct lpr_fs_session {
    struct lpr_fs_session *next;
    struct ph_fs_channel channel;
    uint64_t metadata_bytes, mapping_bytes;
    void *mapping;
    pacha_fd_t vmo, doorbell, wake_read, wake_write, remote;
    int attached, remote_retired, retirement_failed;
    int temporary;
    uint64_t client;
    struct lpr_fs_request *requests;
    volatile uint32_t queue_lock, epoch;
    uint32_t references, receiver;
};

static struct lpr_fs_session *session;
static struct lpr_fs_session *retired;
static volatile uint32_t session_lock;
/* Establishment is control-plane work, separate from every live queue. */
static volatile uint32_t management_lock, inventory_epoch;
static uint32_t terminal;
/* HELLO can reserve authority before even local metadata can be allocated.
 * Filed retains that reservation; a lost cancellation ACK closes admission. */
static uint32_t reservation_fault;

int lpr_fs_session_terminal(void)
{
    return __atomic_load_n(&terminal, __ATOMIC_ACQUIRE) != 0;
}

static uint64_t get64(const unsigned char *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static int native_error(int64_t status)
{
    int error = (int)lpr_pacha_status_to_errno(status);
    return error < 0 ? error : -LPR_LINUX_EIO;
}

static int free_session(struct lpr_fs_session *s, int close_fds)
{
    if (!s) return 0;
    if (s->mapping) {
        int64_t status = lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)s->mapping, s->mapping_bytes);
        if (status) return native_error(status);
        s->mapping = 0;
    }
    if (close_fds) {
        pacha_fd_t *fds[] = {&s->vmo, &s->doorbell, &s->wake_read, &s->wake_write, &s->remote};
        for (unsigned i = 0; i < sizeof(fds) / sizeof(*fds); ++i) {
            if (*fds[i] < 16) continue;
            int64_t status = lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, *fds[i]);
            if (status) return native_error(status);
            *fds[i] = -1;
        }
    }
    /* Metadata goes last. Any failed step retains the exact remaining local
     * inventory, without repeating a successful remote retirement or close. */
    int64_t status = lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
        (uint64_t)(uintptr_t)s, s->metadata_bytes);
    return status ? native_error(status) : 0;
}

static int retire_config(const struct ph_fs_channel_config *config, uint64_t client)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_SESSION_DETACH,
        .generation = config->generation, .second_handle = config->channel_id};
    kb2_fs_response_t response = {0};
    int64_t status = lpr_filed_client_fd >= 16 && lpr_filed_endpoint_checked > 0 &&
        lpr_filed_client_id == client ?
        lpr_fs_call(&request, 0, 0, 0, 0, 0, 0, &response, -1, 0) : -LPR_LINUX_EIO;
    if (!status && response.handle != config->channel_id) status = -LPR_LINUX_EIO;
    return (int)status;
}

static int retire_remote(struct lpr_fs_session *s)
{
    if (s->remote_retired) return 0;
    if (s->retirement_failed) return -LPR_LINUX_EIO;
    int status = retire_config(&s->channel.config, s->client);
    if (status) s->retirement_failed = 1;
    else s->remote_retired = 1;
    return status;
}

static int initialize(uint64_t minimum_arena, struct lpr_fs_session **out)
{
    unsigned char config_wire[32];
    kb2_fs_request_t hello = {.opcode = KB2_FILESYSTEM_OP_HELLO, .flags = 1,
        .length = minimum_arena};
    kb2_fs_response_t response;
    lpr_boot_diagnostic_fs_emit("fs.hello.enter", hello.opcode, 0, 0, 0, 0);
    int64_t status = lpr_fs_call(&hello, 0, 0, 0, 0, config_wire,
        sizeof(config_wire), &response, -1, 0);
    lpr_boot_diagnostic_fs_emit("fs.hello.return", hello.opcode, 0, 0, status, 0);
    if (status) { lpr_fs_diag(1, status, 0); return (int)status; }
    if (response.data_length != sizeof(config_wire) || get64(config_wire + 24) > UINT32_MAX) {
        __atomic_store_n(&reservation_fault, 1, __ATOMIC_RELEASE);
        return -LPR_LINUX_EIO;
    }
    const struct ph_fs_channel_config config = {
        .generation = get64(config_wire), .channel_id = get64(config_wire + 8),
        .arena_bytes = get64(config_wire + 16), .queue_size = (uint32_t)get64(config_wire + 24),
    };
    if (config.arena_bytes < minimum_arena || !config.channel_id ||
        response.handle != config.channel_id) {
        __atomic_store_n(&reservation_fault, 1, __ATOMIC_RELEASE);
        return -LPR_LINUX_EIO;
    }
    size_t mapping_bytes, arena_offset;
    if (ph_fs_channel_layout(&config, &mapping_bytes, &arena_offset)) {
        __atomic_store_n(&reservation_fault, 1, __ATOMIC_RELEASE);
        return -LPR_LINUX_EIO;
    }
    (void)arena_offset;
    const uint64_t metadata_bytes = lpr_page_align_up(sizeof(struct lpr_fs_session) +
        config.queue_size * (sizeof(kb2_vq_chain_t *) + sizeof(uint16_t)) +
        7 + (config.queue_size / 2) * sizeof(struct lpr_fs_request));
    const int64_t meta = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP, 0, 0,
        metadata_bytes, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS, 0);
    if (meta < 4096) {
        int error = native_error(meta);
        if (retire_config(&config, lpr_filed_client_id))
            __atomic_store_n(&reservation_fault, 1, __ATOMIC_RELEASE);
        return error;
    }
    struct lpr_fs_session *s = (void *)(uintptr_t)meta;
    lpr_memset(s, 0, metadata_bytes);
    s->metadata_bytes = metadata_bytes;
    s->mapping_bytes = mapping_bytes;
    s->vmo = s->doorbell = s->wake_read = s->wake_write = s->remote = -1;
    s->client = lpr_filed_client_id;
    s->channel.config = config;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t vmo = lpr_pacha_syscall3(PACHAOS_SYSCALL_VMO_CREATE,
        mapping_bytes, rights, PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC);
    if (vmo < 16) { status = native_error(vmo); goto fail; }
    s->vmo = (pacha_fd_t)vmo;
    const int64_t mapping = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP, vmo, 0,
        mapping_bytes, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED, 0);
    if (mapping < 4096) { status = native_error(mapping); goto fail; }
    s->mapping = (void *)(uintptr_t)mapping;
    kb2_vq_chain_t **slots = (void *)(s + 1);
    uint16_t *owners = (void *)(slots + config.queue_size);
    s->requests = (void *)(((uintptr_t)(owners + config.queue_size) + 7) & ~(uintptr_t)7);
    status = ph_fs_channel_bind(&s->channel, s->mapping, mapping_bytes,
        &config, KB2_VQ_DRIVER, &kb2_vq_x86_64_atomics, slots, owners);
    lpr_boot_diagnostic_fs_emit("fs.bind", config.channel_id, config.queue_size,
        config.arena_bytes, status, 0);
    if (status) { lpr_fs_diag(2, status, config.channel_id); goto fail; }
    /* A local fault must wake the elected native receiver even when the peer
     * is still executing. This private latch never reaches filed or sandbox. */
    status = lpr_native_wait_pair_flags(&s->wake_read, &s->wake_write,
        PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC);
    if (status) goto fail;
    status = lpr_native_wait_pair_flags(&s->doorbell, &s->remote,
        PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC);
    lpr_boot_diagnostic_fs_emit("fs.wait-pair", s->doorbell, s->remote, config.channel_id, status, 0);
    if (status) goto fail;
    kb2_fs_request_t attach = {.opcode = KB2_FILESYSTEM_OP_SESSION_ATTACH,
        .generation = config.generation, .second_handle = config.channel_id,
        .mask = config.queue_size, .length = config.arena_bytes};
    /* Once capabilities have been sent, a lost or malformed reply cannot
     * prove the peer rejected them. Keep inventory until client retirement. */
    s->attached = 1;
    lpr_boot_diagnostic_fs_emit("fs.attach.enter", config.channel_id, s->vmo,
        s->remote, 0, 0);
    status = lpr_fs_call_attach(&attach, s->vmo, s->remote, &response);
    lpr_boot_diagnostic_fs_emit("fs.attach.return", config.channel_id, 0, 0, status, 0);
    int64_t closed = lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, s->remote);
    if (!closed) s->remote = -1;
    else if (!status) status = native_error(closed);
    if (status) { lpr_fs_diag(3, status, config.channel_id); goto fail; }
    if (response.handle != config.channel_id) { status = -LPR_LINUX_EIO; goto fail; }
    s->attached = 1;
    *out = s;
    return 0;
fail:
    if (!s->attached) {
        /* No capabilities were transferred, but HELLO still charged a remote
         * reservation. Cancel it through the same authenticated ACK path. */
        int canceled = retire_remote(s);
        if (canceled) s->attached = 1;
        else {
            int cleaned = free_session(s, 1);
            if (!cleaned) return (int)status;
        }
    }
    atomic_store_explicit(&s->channel.channel.faulted, 1, memory_order_release);
    *out = s;
    return (int)status;
}

/* Threads inside wait_epoch, over every epoch word of this process. A call
 * publishes its epochs several times (completion, receiver handoff, lease
 * release, inventory); an unconditional FUTEX_WAKE made each of those a
 * native syscall even in the usual single-threaded caller with no sleeper. */
static volatile uint32_t epoch_waiters;

static void wake_epoch(volatile uint32_t *epoch)
{
    /* Both orders are seq_cst and pair with wait_epoch: a waiter whose count
     * this load misses registered after the increment, so its compare-and-
     * sleep observes the new epoch and does not sleep. */
    __atomic_add_fetch(epoch, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&epoch_waiters, __ATOMIC_SEQ_CST))
        (void)lpr_pacha_syscall2(PACHA_RUNTIME_SYSCALL_FUTEX_WAKE,
            (uint64_t)(uintptr_t)epoch, UINT32_MAX);
}

static void wait_epoch(volatile uint32_t *epoch, uint32_t expected)
{
    /* Compare-and-sleep closes the unlock/wakeup gap. An interrupted internal
     * wait only rechecks inventory: a published VFS operation is never replayed. */
    __atomic_add_fetch(&epoch_waiters, 1, __ATOMIC_SEQ_CST);
    (void)lpr_pacha_syscall4(PACHA_RUNTIME_SYSCALL_FUTEX_WAIT,
        (uint64_t)(uintptr_t)epoch, expected, 0, PACHA_WAIT_UNINTERRUPTIBLE);
    __atomic_sub_fetch(&epoch_waiters, 1, __ATOMIC_SEQ_CST);
}

static void fault_session(struct lpr_fs_session *s)
{
    kb2_vq_channel_fault(&s->channel.channel);
    if (s->wake_write >= 16) {
        int64_t status = lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, s->wake_write);
        if (!status) s->wake_write = -1;
        else {
            /* Retain a failed-close FD. A queued local hint provides the same
             * persistent readable latch without freeing uncertain inventory. */
            const struct pacha_ipc_msg hint = {.word0 = 1};
            (void)lpr_ipc_message(PACHAOS_SYSCALL_IPC_SEND, s->wake_write, (uint64_t)(uintptr_t)((uint64_t)(uintptr_t)&hint), 0, 0, 0, 0);
        }
    }
    wake_epoch(&s->epoch);
}

static int acquire_session(uint64_t minimum, struct lpr_fs_session **out)
{
    int establishing = 0;
    for (;;) {
        lpr_state_lock(&session_lock);
        if (lpr_fs_session_terminal()) {
            lpr_state_unlock(&session_lock);
            if (establishing) lpr_state_unlock(&management_lock);
            return -LPR_LINUX_EIO;
        }
        if (retired || __atomic_load_n(&reservation_fault, __ATOMIC_ACQUIRE)) {
            lpr_state_unlock(&session_lock);
            if (establishing) lpr_state_unlock(&management_lock);
            return -LPR_LINUX_EIO;
        }
        /* A size change cannot erase an uncertain generation. Fail closed
         * rather than silently reconnect and possibly replay a side effect. */
        for (struct lpr_fs_session *s = session; s; s = s->next) {
            if (atomic_load_explicit(&s->channel.channel.faulted, memory_order_acquire)) {
                lpr_state_unlock(&session_lock);
                if (establishing) lpr_state_unlock(&management_lock);
                return -LPR_LINUX_EIO;
            }
        }
        struct lpr_fs_session *best = 0;
        for (struct lpr_fs_session *s = session; s; s = s->next) {
            if (!s->temporary && minimum <= s->channel.config.arena_bytes &&
                (!best || s->channel.config.arena_bytes < best->channel.config.arena_bytes)) best = s;
        }
        if (best) {
            ++best->references;
            *out = best;
            lpr_state_unlock(&session_lock);
            if (establishing) lpr_state_unlock(&management_lock);
            return 0;
        }
        int additional = session != 0;
        lpr_state_unlock(&session_lock);
        if (!establishing) {
            lpr_state_lock(&management_lock);
            establishing = 1;
            continue;
        }
        struct lpr_fs_session *s = 0;
        /* Establish the negotiated base independently of the first syscall's
         * size. Oversized calls get distinct authenticated temporary leases,
         * not an ever-growing cache of transport mappings. */
        int status = initialize(additional ? minimum : 0, &s);
        lpr_state_lock(&session_lock);
        if (s) {
            s->temporary = additional;
            s->next = session;
            session = s;
        }
        if (!status && additional && !lpr_fs_session_terminal()) {
            s->references = 1;
            *out = s;
            lpr_state_unlock(&session_lock);
            lpr_state_unlock(&management_lock);
            return 0;
        }
        if (status == -LPR_LINUX_EAGAIN) {
            unsigned releasable = 0;
            for (struct lpr_fs_session *q = session; q; q = q->next)
                if (q->temporary && q->references) releasable = 1;
            uint32_t expected = __atomic_load_n(&inventory_epoch, __ATOMIC_ACQUIRE);
            lpr_state_unlock(&session_lock);
            lpr_state_unlock(&management_lock);
            establishing = 0;
            /* Wait only for our own live temporary leases, whose retirement
             * advances this epoch. Foreign pressure has no local wake source:
             * report allocation failure, never spin or wait indefinitely. */
            if (!releasable) return -LPR_LINUX_ENOMEM;
            wait_epoch(&inventory_epoch, expected);
            continue;
        }
        lpr_state_unlock(&session_lock);
        if (status) { lpr_state_unlock(&management_lock); return status; }
        /* Recheck terminal after construction. Drop waits on management_lock
         * and will retire this binding even when exit raced with ATTACH. */
    }
}

static int wait_endpoint(struct lpr_fs_session *s, uint64_t events)
{
    for (;;) {
        if (atomic_load_explicit(&s->channel.channel.faulted, memory_order_acquire))
            return -LPR_LINUX_EIO;
        struct pacha_pollfd fds[2] = {
            {.fd = s->doorbell, .events = events | PACHA_FD_EVENT_HANGUP},
            {.fd = s->wake_read, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP}};
        int64_t status = lpr_pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uint64_t)(uintptr_t)fds, 2, PACHA_FD_WAIT_FOREVER, PACHA_WAIT_UNINTERRUPTIBLE);
        lpr_fs_diag(30, status, (uint64_t)fds[0].revents | ((uint64_t)fds[1].revents << 32));
        if (fds[1].revents || (fds[0].revents & PACHA_FD_EVENT_HANGUP)) return -LPR_LINUX_EIO;
        if (status > 0 && (fds[0].revents & events)) return 0;
        if (status == PACHA_SYSCALL_ERR_NOT_READY || status == -PACHA_SYSCALL_ERR_NOT_READY)
            continue;
        return status ? (int)lpr_pacha_status_to_errno(status) : -LPR_LINUX_EIO;
    }
}

static int notify_session(struct lpr_fs_session *s)
{
    /* The doorbell is a kernel notification latch, not a message: the channel
     * capability already proves the sender and the ring carries the request.
     * A latch has no queue slot to wait for, so any failure is peer death or
     * lost authority, never backpressure. */
    int64_t status = lpr_pacha_syscall1(PACHAOS_SYSCALL_IPC_NOTIFY, s->doorbell);
    if (status) lpr_fs_diag(31, status, s->doorbell);
    return status ? (int)lpr_pacha_status_to_errno(status) : 0;
}

static int receive_session(struct lpr_fs_session *s)
{
    /* A lone thread parks in NOTIFY_WAIT, the kernel's single receiver slot:
     * the owner's NOTIFY completes it and hands the CPU straight over. The
     * local wake FD only lets another thread fault or drop the session while
     * this one waits; none exists, and this blocked thread cannot create one.
     * Signals remain pending until the RPC releases its resources. */
    while (__atomic_load_n(&lpr_state.thread_count, __ATOMIC_ACQUIRE) <= 1u) {
        if (atomic_load_explicit(&s->channel.channel.faulted, memory_order_acquire))
            return -LPR_LINUX_EIO;
        int64_t status = lpr_pacha_syscall3(PACHAOS_SYSCALL_IPC_NOTIFY_WAIT, s->doorbell,
            PACHA_FD_WAIT_FOREVER, PACHA_WAIT_UNINTERRUPTIBLE);
        if (!status) return 0;
        if (status == PACHA_SYSCALL_ERR_NOT_READY || status == -PACHA_SYSCALL_ERR_NOT_READY)
            continue;
        lpr_fs_diag(32, status, s->doorbell);
        /* Peer death, as the WAIT_MANY path reports for HANGUP. */
        if (status == PACHA_SYSCALL_ERR_CLOSED || status == -PACHA_SYSCALL_ERR_CLOSED)
            return -LPR_LINUX_EIO;
        return (int)lpr_pacha_status_to_errno(status);
    }
    for (;;) {
        /* Wait first: the caller just armed and found no completion, so the
         * latch is rarely set yet and a speculative consume would only cost
         * a native syscall. Readiness is level-triggered, so a latched
         * notification returns at once. */
        int error = wait_endpoint(s, PACHA_FD_EVENT_READABLE);
        if (error) return error;
        int64_t status = lpr_pacha_syscall3(PACHAOS_SYSCALL_IPC_NOTIFY_WAIT, s->doorbell, 0, PACHA_WAIT_UNINTERRUPTIBLE);
        /* A consume racing another reader reports EMPTY, unlike an interrupted
         * native wait. Neither retires the published request or permits replay. */
        if (!status) return 0;
        if (status == PACHA_SYSCALL_ERR_EMPTY || status == -PACHA_SYSCALL_ERR_EMPTY) {
            /* Readable with no latch can only be a queued message, which a
             * doorbell never carries. Left unread it would keep WAIT_MANY
             * ready and spin this loop, so receive it (no capability room)
             * and fail the session; EMPTY again was a benign race. */
            struct pacha_ipc_msg message = {0};
            status = lpr_ipc_message(PACHAOS_SYSCALL_IPC_RECV, s->doorbell, 0, (uint64_t)(uintptr_t)((uint64_t)(uintptr_t)&message), 0, 0, 0);
            if (status != PACHA_SYSCALL_ERR_EMPTY && status != -PACHA_SYSCALL_ERR_EMPTY) {
                lpr_fs_diag(32, status ? status : -LPR_LINUX_EIO, s->doorbell);
                return -LPR_LINUX_EIO;
            }
            continue;
        }
        if (status != PACHA_SYSCALL_ERR_EMPTY && status != -PACHA_SYSCALL_ERR_EMPTY &&
            status != PACHA_SYSCALL_ERR_NOT_READY && status != -PACHA_SYSCALL_ERR_NOT_READY) {
            lpr_fs_diag(32, status, s->doorbell);
            return (int)lpr_pacha_status_to_errno(status);
        }
    }
}

/* queue_lock protects only private ring inventory. Taking a used entry never
 * discards another caller's completion: that caller's persistent pending stays
 * marked until its own collect. The elected receiver does not own the queue. */
static int poll_session(struct lpr_fs_session *s)
{
    for (;;) {
        struct ph_fs_pending *completed;
        int status = ph_fs_channel_poll(&s->channel, &completed);
        if (status == -LPR_LINUX_EAGAIN) return 0;
        if (status) {
            lpr_fs_diag(33, status, s->channel.lane.owned);
            fault_session(s);
            return status;
        }
        wake_epoch(&s->epoch);
    }
}

/* Enter holding queue_lock, return without it. One caller drains native
 * doorbells; others sleep on a local epoch. Receiver handoff wakes all so a
 * receiver returning its own syscall cannot strand remaining callers. */
static void wait_progress(struct lpr_fs_session *s)
{
    int ready = 0;
    if (ph_fs_channel_arm(&s->channel, &ready)) {
        fault_session(s);
        lpr_state_unlock(&s->queue_lock);
        return;
    }
    if (ready) { lpr_state_unlock(&s->queue_lock); return; }
    if (!s->receiver) {
        s->receiver = 1;
        lpr_state_unlock(&s->queue_lock);
        int status = receive_session(s);
        lpr_state_lock(&s->queue_lock);
        s->receiver = 0;
        if (status) fault_session(s);
        else (void)poll_session(s);
        wake_epoch(&s->epoch);
        lpr_state_unlock(&s->queue_lock);
    } else {
        uint32_t expected = __atomic_load_n(&s->epoch, __ATOMIC_ACQUIRE);
        lpr_state_unlock(&s->queue_lock);
        wait_epoch(&s->epoch, expected);
    }
}

int lpr_fs_session_call(kb2_fs_request_t *request,
    const struct lpr_fs_buffer *input, unsigned input_count, kb2_fs_response_t *response,
    const struct lpr_fs_buffer *output, unsigned output_count, size_t *used)
{
    if (!request || !response || !used || (input_count && !input) || (output_count && !output))
        return -LPR_LINUX_EINVAL;
    size_t bytes = KB2_FILESYSTEM_REQUEST_SIZE, capacity = 0;
    for (unsigned i = 0; i < input_count; ++i) {
        if (!input[i].bytes && input[i].length) return -LPR_LINUX_EFAULT;
        if (input[i].length > UINT32_MAX - bytes) return -LPR_LINUX_EOVERFLOW;
        bytes += input[i].length;
    }
    for (unsigned i = 0; i < output_count; ++i) {
        if (!output[i].bytes && output[i].length) return -LPR_LINUX_EFAULT;
        if (output[i].length > UINT32_MAX - KB2_FILESYSTEM_RESPONSE_SIZE - capacity)
            return -LPR_LINUX_EOVERFLOW;
        capacity += output[i].length;
    }
    *used = 0;
    struct lpr_fs_session *s;
    size_t minimum = (bytes + capacity + KB2_FILESYSTEM_RESPONSE_SIZE + 7) & ~(size_t)7;
    int status = acquire_session(minimum, &s);
    if (status) return status;
    request->generation = s->channel.config.generation;
    struct lpr_fs_request *owned = 0;
    unsigned char header[KB2_FILESYSTEM_REQUEST_SIZE];
    if (kb2_fs_request_encode(header, sizeof(header), bytes, request)) {
        status = -LPR_LINUX_EINVAL;
        goto finish;
    }
    for (;;) {
        lpr_state_lock(&s->queue_lock);
        (void)poll_session(s);
        if (atomic_load_explicit(&s->channel.channel.faulted, memory_order_acquire) ||
            lpr_fs_session_terminal()) {
            status = -LPR_LINUX_EIO;
            lpr_state_unlock(&s->queue_lock);
            goto finish;
        }
        owned = 0;
        for (unsigned i = 0; i < s->channel.config.queue_size / 2; ++i)
            if (!s->requests[i].occupied) { owned = &s->requests[i]; break; }
        int notify = 0;
        void *destination = 0;
        status = owned ? ph_fs_channel_reserve(&s->channel, &owned->pending, header, bytes,
            capacity + KB2_FILESYSTEM_RESPONSE_SIZE, &destination) : -LPR_LINUX_EAGAIN;
        if (status == -LPR_LINUX_EAGAIN) {
            /* Space includes completed-but-uncollected leases. The receiver
             * may already have consumed all used entries, so wait for the
             * caller's collect rather than require another remote message. */
            uint32_t expected = __atomic_load_n(&s->epoch, __ATOMIC_ACQUIRE);
            lpr_state_unlock(&s->queue_lock);
            wait_epoch(&s->epoch, expected);
            continue;
        }
        if (owned && owned->pending.channel) owned->occupied = 1;
        if (status) {
            lpr_fs_diag(34, status, bytes);
            if (owned && owned->occupied) fault_session(s);
            wake_epoch(&s->epoch);
        }
        lpr_state_unlock(&s->queue_lock);
        if (status) goto finish;
        lpr_memcpy(destination, header, sizeof(header));
        size_t copied = sizeof(header);
        for (unsigned i = 0; i < input_count; ++i) {
            if (input[i].length) lpr_memcpy((unsigned char *)destination + copied,
                input[i].bytes, input[i].length);
            copied += input[i].length;
        }
        lpr_state_lock(&s->queue_lock);
        status = ph_fs_channel_publish(&owned->pending, &notify);
        if (status) lpr_fs_diag(35, status, owned->pending.chain.head);
        if (status && !owned->pending.channel) owned->occupied = 0;
        if (status) {
            if (atomic_load_explicit(&s->channel.channel.faulted, memory_order_acquire)) fault_session(s);
            else wake_epoch(&s->epoch);
        }
        lpr_state_unlock(&s->queue_lock);
        if (status == -LPR_LINUX_EAGAIN) continue; /* Not published: retry admission, not VFS. */
        if (status) goto finish;
        if (notify && (status = notify_session(s))) {
            lpr_state_lock(&s->queue_lock);
            fault_session(s);
            lpr_state_unlock(&s->queue_lock);
            goto finish;
        }
        break;
    }
    for (;;) {
        lpr_state_lock(&s->queue_lock);
        (void)poll_session(s);
        if (atomic_load_explicit(&s->channel.channel.faulted, memory_order_acquire)) {
            status = -LPR_LINUX_EIO;
            lpr_state_unlock(&s->queue_lock);
            break;
        }
        if (owned->pending.completed) {
            lpr_state_unlock(&s->queue_lock);
            status = ph_fs_channel_result_header(&owned->pending, response, used);
            if (!status && response->data_length > capacity) status = -LPR_LINUX_EIO;
            uint64_t copied = 0;
            for (unsigned i = 0; !status && i < output_count && copied < response->data_length; ++i) {
                uint64_t part = output[i].length;
                if (part > response->data_length - copied) part = response->data_length - copied;
                status = ph_fs_channel_result_data(&owned->pending, copied, (void *)output[i].bytes, part);
                copied += part;
            }
            lpr_state_lock(&s->queue_lock);
            if (!status) status = ph_fs_channel_release(&owned->pending);
            if (!status) owned->occupied = 0;
            else fault_session(s);
            wake_epoch(&s->epoch);
            lpr_state_unlock(&s->queue_lock);
            break;
        }
        wait_progress(s);
    }
finish:
    if (s->temporary && !atomic_load_explicit(&s->channel.channel.faulted, memory_order_acquire)) {
        /* Keep the caller reference while obtaining control-plane exclusion.
         * Drop never holds that exclusion while waiting for these references.
         * A matching RELEASED ACK, not a used entry, permits VMO destruction. */
        lpr_state_lock(&management_lock);
        int retired_status = retire_remote(s);
        lpr_state_lock(&session_lock);
        struct lpr_fs_session **cursor = &session;
        while (*cursor && *cursor != s) cursor = &(*cursor)->next;
        if (*cursor) *cursor = s->next;
        --s->references;
        if (retired_status) {
            s->next = retired;
            retired = s;
            if (!status) status = retired_status;
        }
        wake_epoch(&inventory_epoch);
        lpr_state_unlock(&session_lock);
        if (!retired_status) {
            int cleaned = free_session(s, 1);
            if (cleaned) {
                lpr_state_lock(&session_lock);
                s->next = retired;
                retired = s;
                lpr_state_unlock(&session_lock);
                if (!status) status = cleaned;
            }
        }
        lpr_state_unlock(&management_lock);
        return status;
    }
    lpr_state_lock(&session_lock);
    --s->references;
    wake_epoch(&inventory_epoch);
    lpr_state_unlock(&session_lock);
    return status;
}

int64_t lpr_fs_session_drop(void)
{
    lpr_state_lock(&session_lock);
    /* No sibling may publish a new queue after the final retirement ACK.
     * This is private admission state, reset only in a new image/fork child. */
    __atomic_store_n(&terminal, 1, __ATOMIC_RELEASE);
    for (struct lpr_fs_session *q = session; q; q = q->next) wake_epoch(&q->epoch);
    lpr_state_unlock(&session_lock);
    for (;;) {
        lpr_state_lock(&session_lock);
        unsigned active = 0;
        for (struct lpr_fs_session *q = session; q; q = q->next) active |= q->references;
        uint32_t expected = __atomic_load_n(&inventory_epoch, __ATOMIC_ACQUIRE);
        lpr_state_unlock(&session_lock);
        if (!active) {
            /* Join an establishment/lease retirement already in progress,
             * then inspect its final inventory. Terminal admission ensures
             * no new caller reference can appear during this handoff. */
            lpr_state_lock(&management_lock);
            lpr_state_lock(&session_lock);
            active = 0;
            for (struct lpr_fs_session *q = session; q; q = q->next) active |= q->references;
            if (!active) break;
            lpr_state_unlock(&session_lock);
            lpr_state_unlock(&management_lock);
            continue;
        }
        wait_epoch(&inventory_epoch, expected);
    }
    struct lpr_fs_session *s = session;
    session = 0;
    lpr_state_unlock(&session_lock);
    /* The supervisor retains a duplicate of the client endpoint across exec.
     * Local CLOSE cannot retire its queue: waiting for HANGUP here would block
     * the native exit that releases that duplicate. Retire only this queue and
     * validate its ACK while the authenticated control endpoint is still live. */
    int64_t result = __atomic_load_n(&reservation_fault, __ATOMIC_ACQUIRE) ? -LPR_LINUX_EIO : 0;
    struct lpr_fs_session **unresolved = &retired;
    while (*unresolved) {
        struct lpr_fs_session *held = *unresolved, *next = held->next;
        /* A second drop is not evidence of a lost remote ACK. Retry only
         * local cleanup whose remote quiescence was already authenticated. */
        int status = held->remote_retired && !held->retirement_failed ?
            free_session(held, 1) : -LPR_LINUX_EIO;
        if (!status) { *unresolved = next; continue; }
        if (!result) result = status;
        unresolved = &held->next;
    }
    while (s) {
        struct lpr_fs_session *next = s->next;
        int64_t status = 0;
        if (s->attached) {
            status = retire_remote(s);
            if (status) {
                /* A lost attach/detach reply or failed teardown is not an ACK.
                 * No retry can establish which generation a late peer used. */
                atomic_store_explicit(&s->channel.channel.faulted, 1, memory_order_release);
                if (!result) result = status;
            }
        }
        if (!status) status = free_session(s, 1);
        if (status) {
            s->next = retired; retired = s;
            if (!result) result = status;
        }
        s = next;
    }
    lpr_state_unlock(&management_lock);
    return result;
}

void lpr_fs_session_after_fork_child(void)
{
    session_lock = 0;
    management_lock = inventory_epoch = 0;
    epoch_waiters = 0; /* Only this thread survives fork. */
    __atomic_store_n(&terminal, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&reservation_fault, 0, __ATOMIC_RELEASE);
    /* PRIVATE queue capabilities are not inherited. Numeric FD slots may
     * already hold child resources, so detach child mappings without CLOSE. */
    struct lpr_fs_session *lists[] = {session, retired};
    session = retired = 0;
    for (unsigned i = 0; i < sizeof(lists) / sizeof(*lists); ++i) {
        struct lpr_fs_session *s = lists[i];
        while (s) {
            struct lpr_fs_session *next = s->next;
            if (free_session(s, 0)) {
                s->next = retired;
                retired = s;
            }
            s = next;
        }
    }
}
