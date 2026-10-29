/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/personality/linux/runtime/lpr_vfs/session.h"
#include "../userland/personality/linux/runtime/lpr_common/boot_diagnostic.h"
#include <kobox2/virtqueue_x86_64.h>

lpr_state_t lpr_state;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
lpr_boot_diagnostic_state_t lpr_boot_diagnostic_state;
#endif
struct peer_request {
    kb2_vq_chain_t chain;
    kb2_vq_segment_t segments[4];
    kb2_fs_request_t decoded;
    int active;
};
struct peer {
    struct ph_fs_channel_config config;
    struct ph_fs_channel device;
    kb2_vq_chain_t *slots[4];
    uint16_t owners[4];
    struct peer_request requests[2];
    void *metadata, *mapping;
    size_t metadata_bytes, mapping_bytes;
    int vmo, local, remote, wake_read, wake_write, wake_closed, wake_hint, pairs, live, reserved, messages;
};
static struct peer peers[64];
static unsigned peer_count, closes, unmaps, attaches, detaches;
static unsigned seen[128], finished[128];
static unsigned writes[128];
static uint64_t released = UINT64_MAX;
static unsigned hold_first, stop_server, malformed, attach_error, detach_error, bad_ack;
static unsigned third_waiting;
static unsigned lease_limit = UINT_MAX, budget_rejections;
static unsigned initialize_failure, cleanup_failure;
static unsigned wait_interruptions, interrupted_waits, kicks;
static unsigned wake_close_failure, control_close_failure, malformed_config;
static _Thread_local unsigned caller_handle;
static int fork_cleanup;
static pthread_mutex_t peer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t peer_event = PTHREAD_COND_INITIALIZER;

void *lpr_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void *lpr_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
int64_t pacha_kernel_status_to_errno(int64_t s)
{
    if (!s || s < 0) return s;
    return s == PACHA_SYSCALL_ERR_ALLOC ? -LPR_LINUX_ENOMEM : -LPR_LINUX_EIO;
}
uint64_t lpr_page_align_up(uint64_t n) { return (n + 4095) & ~4095ull; }
static void put64(unsigned char *p, uint64_t v)
{ for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static struct peer *fd_peer(int fd)
{
    for (unsigned i = 0; i < peer_count; ++i)
        if (peers[i].vmo == fd || peers[i].local == fd || peers[i].remote == fd ||
            peers[i].wake_read == fd || peers[i].wake_write == fd) return &peers[i];
    assert(0); return NULL;
}
int64_t lpr_fs_call(kb2_fs_request_t *r, const char *path, const char *second,
    const void *input, uint64_t input_bytes, void *output, uint64_t output_bytes,
    kb2_fs_response_t *reply, pacha_fd_t transfer, pacha_fd_t *returned)
{
    assert(!path && !second && !input && !input_bytes && transfer == -1 && !returned);
    assert(!lpr_state.filed_rpc.connection_lock);
    pthread_mutex_lock(&peer_lock);
    if (r->opcode == KB2_FILESYSTEM_OP_HELLO) {
        if (r->length > 65536) {
            unsigned active = 0;
            for (unsigned i = 0; i < peer_count; ++i)
                if (peers[i].live && peers[i].config.arena_bytes > 65536) ++active;
            if (active >= lease_limit) {
                ++budget_rejections; pthread_cond_broadcast(&peer_event);
                pthread_mutex_unlock(&peer_lock); return -LPR_LINUX_EAGAIN;
            }
        }
        assert(r->flags == 1 && output_bytes == 32 && peer_count < 64);
        struct peer *p = &peers[peer_count++];
        p->reserved = 1;
        p->config = (struct ph_fs_channel_config){7, 99 + peer_count,
            r->length > 65536 ? r->length : 65536, 4};
        p->vmo = 100 + peer_count * 5; p->local = p->vmo + 1; p->remote = p->vmo + 2;
        p->wake_read = p->vmo + 3; p->wake_write = p->vmo + 4;
        size_t offset;
        assert(!ph_fs_channel_layout(&p->config, &p->mapping_bytes, &offset));
        unsigned char *wire = output;
        put64(wire, 7); put64(wire + 8, p->config.channel_id);
        put64(wire + 16, p->config.arena_bytes); put64(wire + 24, 4);
        *reply = (kb2_fs_response_t){.data_length = 32, .handle = p->config.channel_id};
        if (malformed_config) reply->handle++;
        lpr_filed_client_fd = 50; lpr_filed_client_id = 2; lpr_filed_endpoint_checked = 1;
    } else {
        assert(r->opcode == KB2_FILESYSTEM_OP_SESSION_DETACH && !output && !output_bytes);
        assert(r->generation == 7 && !r->handle && !r->length && !r->flags);
        struct peer *p = NULL;
        for (unsigned i = 0; i < peer_count; ++i)
            if (peers[i].config.channel_id == r->second_handle) p = &peers[i];
        assert(p && p->reserved);
        ++detaches;
        if (!detach_error && !bad_ack) p->live = p->reserved = 0;
        *reply = (kb2_fs_response_t){.handle = bad_ack ? 3 : r->second_handle};
        if (detach_error) { pthread_mutex_unlock(&peer_lock); return -LPR_LINUX_EIO; }
    }
    pthread_mutex_unlock(&peer_lock); return 0;
}
int64_t lpr_fs_call_attach(kb2_fs_request_t *r, pacha_fd_t vmo, pacha_fd_t remote, kb2_fs_response_t *reply)
{
    pthread_mutex_lock(&peer_lock);
    struct peer *p = fd_peer(vmo);
    assert(remote == p->remote && r->opcode == KB2_FILESYSTEM_OP_SESSION_ATTACH &&
        r->second_handle == p->config.channel_id && r->length == p->config.arena_bytes);
    assert(!ph_fs_channel_bind(&p->device, p->mapping, p->mapping_bytes, &p->config,
        KB2_VQ_DEVICE, &kb2_vq_x86_64_atomics, p->slots, p->owners));
    p->live = 1; ++attaches;
    *reply = (kb2_fs_response_t){.handle = p->config.channel_id};
    pthread_cond_broadcast(&peer_event);
    pthread_mutex_unlock(&peer_lock); return attach_error ? -LPR_LINUX_EIO : 0;
}
int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t a, uint64_t b, uint64_t c)
{
    if (nr == PACHAOS_SYSCALL_IPC_NOTIFY_WAIT) {
        /* Eight caller threads take the WAIT_MANY path, which only consumes. */
        assert(!b && c == PACHA_WAIT_UNINTERRUPTIBLE);
        pthread_mutex_lock(&peer_lock);
        struct peer *p = fd_peer(a); assert(a == (uint64_t)p->local);
        int latched = p->messages != 0; p->messages = 0;
        pthread_mutex_unlock(&peer_lock);
        return latched ? 0 : PACHA_SYSCALL_ERR_EMPTY;
    }
    assert(c == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
    struct peer *p = &peers[peer_count - 1];
    if (nr == PACHAOS_SYSCALL_VMO_CREATE) {
        if (initialize_failure == 8) return 0;
        if (initialize_failure == 2) return PACHA_SYSCALL_ERR_ALLOC;
        assert(a == p->mapping_bytes && (b & PACHA_FD_RIGHT_INSPECT)); return p->vmo;
    }
    assert(nr == PACHAOS_SYSCALL_IPC_CHANNEL_CREATE);
    if (initialize_failure == 4 + (unsigned)p->pairs) return PACHA_SYSCALL_ERR_ALLOC;
    uint64_t *pair = (void *)(uintptr_t)a;
    if (!p->pairs++) { pair[0] = p->wake_read; pair[1] = p->wake_write; }
    else { pair[0] = p->local; pair[1] = p->remote; }
    return 0;
}
int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t at, uint64_t bytes,
    uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(nr == PACHAOS_SYSCALL_MMAP && !at && !offset &&
        prot == (PACHAOS_PROT_READ | PACHAOS_PROT_WRITE));
    struct peer *p = &peers[peer_count - 1];
    if (initialize_failure == 7 && !fd) return 0;
    if (initialize_failure == (fd ? 3u : 1u)) return PACHA_SYSCALL_ERR_ALLOC;
    void *memory = calloc(1, bytes); assert(memory);
    if (!fd) {
        assert(flags == (PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS));
        p->metadata = memory; p->metadata_bytes = bytes;
    } else {
        assert(fd == (uint64_t)p->vmo && bytes == p->mapping_bytes && flags == PACHAOS_MMAP_SHARED);
        p->mapping = memory;
    }
    return (int64_t)(uintptr_t)memory;
}
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    if (nr == PACHAOS_SYSCALL_IPC_NOTIFY) {
        pthread_mutex_lock(&peer_lock);
        struct peer *p = fd_peer(fd); assert(fd == (uint64_t)p->local);
        ++kicks; pthread_cond_broadcast(&peer_event);
        pthread_mutex_unlock(&peer_lock); return 0;
    }
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd >= 16);
    pthread_mutex_lock(&peer_lock);
    if (fd == 50 && control_close_failure) {
        pthread_mutex_unlock(&peer_lock); return PACHA_SYSCALL_ERR_MAP;
    }
    if (fd != 50) {
        struct peer *p = fd_peer(fd);
        if ((initialize_failure == 6 && fd == (uint64_t)p->remote) ||
            (cleanup_failure == 2 && fd == (uint64_t)p->vmo) ||
            (wake_close_failure && fd == (uint64_t)p->wake_write)) {
            pthread_mutex_unlock(&peer_lock); return PACHA_SYSCALL_ERR_MAP;
        }
        if (fd == (uint64_t)p->wake_write) p->wake_closed = 1;
    }
    ++closes; pthread_cond_broadcast(&peer_event);
    pthread_mutex_unlock(&peer_lock); return 0;
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b)
{
    if (nr == PACHA_RUNTIME_SYSCALL_FUTEX_WAKE) {
        (void)syscall(SYS_futex, (void *)(uintptr_t)a, FUTEX_WAKE_PRIVATE,
            b > INT_MAX ? INT_MAX : (int)b); return 0;
    }
    if (nr == PACHAOS_SYSCALL_LOG) { assert(b && b <= 256); return 0; }
    pthread_mutex_lock(&peer_lock);
    if (nr == PACHAOS_SYSCALL_IPC_SEND) {
        struct peer *p = fd_peer(a);
        const struct pacha_ipc_msg *m = (void *)(uintptr_t)b;
        if (a == (uint64_t)p->wake_write) {
            assert(m->word0 == 1); p->wake_hint = 1;
            pthread_cond_broadcast(&peer_event); pthread_mutex_unlock(&peer_lock); return 0;
        }
        (void)m; assert(!"doorbells are notify latches, never packets");
    } else if (nr == PACHAOS_SYSCALL_IPC_RECV) {
        /* Only reached when WAIT_MANY saw a doorbell readable with no latch,
         * i.e. a forged packet; this peer never queues one. */
        struct peer *p = fd_peer(a); assert(a == (uint64_t)p->local);
        pthread_mutex_unlock(&peer_lock); return PACHA_SYSCALL_ERR_EMPTY;
    } else {
        assert(nr == PACHAOS_SYSCALL_MUNMAP);
        void *memory = (void *)(uintptr_t)a; int found = 0;
        for (unsigned i = 0; i < peer_count; ++i) {
            struct peer *p = &peers[i];
            if (p->mapping != memory && p->metadata != memory) continue;
            assert(!p->live || fork_cleanup); found = 1;
            if ((cleanup_failure == 1 && p->mapping == memory) ||
                (cleanup_failure == 3 && p->metadata == memory)) {
                pthread_mutex_unlock(&peer_lock); return PACHA_SYSCALL_ERR_MAP;
            }
            if (p->mapping == memory) { assert(b == p->mapping_bytes); p->mapping = NULL; }
            else { assert(b == p->metadata_bytes); p->metadata = NULL; }
            free(memory); ++unmaps; break;
        }
        assert(found);
    }
    pthread_mutex_unlock(&peer_lock); return 0;
}
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t fd, uint64_t raw, uint64_t timeout, uint64_t flags)
{
    if (nr == PACHA_RUNTIME_SYSCALL_FUTEX_WAIT) {
        assert(!timeout && flags == PACHA_WAIT_UNINTERRUPTIBLE);
        if (caller_handle == 6) {
            pthread_mutex_lock(&peer_lock);
            third_waiting = 1; pthread_cond_broadcast(&peer_event);
            pthread_mutex_unlock(&peer_lock);
        }
        (void)syscall(SYS_futex, (void *)(uintptr_t)fd, FUTEX_WAIT_PRIVATE, (uint32_t)raw, NULL);
        return 0;
    }

    assert(nr == PACHA_FD_SYSCALL_WAIT_MANY && raw == 2 && timeout == UINT64_MAX && flags == PACHA_WAIT_UNINTERRUPTIBLE);
    pthread_mutex_lock(&peer_lock);
    struct pacha_pollfd *fds = (void *)(uintptr_t)fd;
    struct peer *p = fd_peer(fds[0].fd);
    assert(fds[1].fd == (uint64_t)p->wake_read);
    if (wait_interruptions) {
        --wait_interruptions; ++interrupted_waits;
        pthread_mutex_unlock(&peer_lock); return PACHA_SYSCALL_ERR_NOT_READY;
    }
    if (fds[0].events & PACHA_FD_EVENT_WRITABLE) {
        fds[0].revents = PACHA_FD_EVENT_WRITABLE;
        pthread_mutex_unlock(&peer_lock); return 1;
    }
    while (!p->messages && !p->wake_closed && !p->wake_hint) pthread_cond_wait(&peer_event, &peer_lock);
    if (p->messages) fds[0].revents = PACHA_FD_EVENT_READABLE;
    if (p->wake_closed) fds[1].revents = PACHA_FD_EVENT_HANGUP;
    if (p->wake_hint) fds[1].revents |= PACHA_FD_EVENT_READABLE;
    pthread_mutex_unlock(&peer_lock); return 1;
}
int64_t lpr_wait_native_retained_fds(struct pacha_pollfd *leaves,
    uint32_t count, uint64_t timeout, uint64_t native_flags)
{
    return lpr_pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
        (uint64_t)(uintptr_t)leaves, count, timeout, native_flags);
}
static void *server(void *unused)
{
    (void)unused; pthread_mutex_lock(&peer_lock);
    while (!stop_server) {
        int progress = 0;
        for (unsigned i = 0; i < peer_count; ++i) {
            struct peer *p = &peers[i];
            if (!p->live || !p->mapping) continue;
            for (unsigned j = 0; j < 2; ++j) {
                struct peer_request *r = &p->requests[j];
                if (r->active) continue;
                r->chain = (kb2_vq_chain_t){.segments = r->segments, .capacity = 4};
                kb2_vq_status_t status = kb2_vq_take_available(&p->device.lane, 7, &r->chain);
                if (status == KB2_VQ_EMPTY) {
                    int ready; assert(!kb2_vq_arm(&p->device.lane, 7, &ready));
                    if (ready) progress = 1;
                    break;
                }
                assert(!status);
                unsigned char header[KB2_FILESYSTEM_REQUEST_SIZE];
                assert(!kb2_vq_copy_request(&p->device.lane, 7, &r->chain, 0, header, sizeof(header)));
                assert(!kb2_fs_request_decode(header, sizeof(header), r->chain.readable, &r->decoded));
                assert(r->decoded.handle < 128 && !r->decoded.client && !r->decoded.credential_generation);
                ++seen[r->decoded.handle]; r->active = 1; progress = 1;
            }
            for (unsigned j = 0; j < 2; ++j) {
                struct peer_request *r = &p->requests[j];
                if (!r->active || (hold_first && r->decoded.handle == hold_first) ||
                    (r->decoded.handle < 64 && !(released & (1ull << r->decoded.handle)))) continue;
                kb2_fs_response_t response = {.opcode = r->decoded.opcode, .generation = 7,
                    .sequence = r->decoded.sequence, .result = r->decoded.handle};
                if (malformed) response.sequence ^= 0x1000;
                if (r->decoded.opcode == KB2_FILESYSTEM_OP_PREAD) {
                    response.result = r->decoded.length; response.data_length = r->decoded.length;
                }
                if (r->decoded.opcode == KB2_FILESYSTEM_OP_PWRITE) {
                    unsigned char bytes[64];
                    assert(r->decoded.length == r->decoded.data.length && r->decoded.length <= sizeof(bytes));
                    assert(!kb2_vq_copy_request(&p->device.lane, 7, &r->chain,
                        r->decoded.data.offset, bytes, r->decoded.length));
                    for (size_t k = 0; k < r->decoded.length; ++k)
                        assert(bytes[k] == r->decoded.handle);
                    writes[r->decoded.handle] += r->decoded.length;
                    response.result = r->decoded.length;
                }
                unsigned char *out = (unsigned char *)p->mapping + r->segments[1].address;
                assert(!kb2_fs_response_encode(out, r->chain.writable, &response));
                memset(out + KB2_FILESYSTEM_RESPONSE_SIZE, (unsigned char)r->decoded.handle, response.data_length);
                int notify;
                r->chain.written = KB2_FILESYSTEM_RESPONSE_SIZE + response.data_length;
                assert(!kb2_vq_complete(&p->device.lane, 7, &r->chain, &notify));
                if (notify) p->messages = 1; /* binary kernel latch */
                ++finished[r->decoded.handle];
                r->active = 0; progress = 1;
            }
        }
        pthread_cond_broadcast(&peer_event);
        if (!progress) pthread_cond_wait(&peer_event, &peer_lock);
    }
    pthread_mutex_unlock(&peer_lock); return NULL;
}
struct caller { unsigned handle; size_t bytes; int status; unsigned done; int writing; };
static void *call(void *raw)
{
    struct caller *c = raw;
    caller_handle = c->handle;
    kb2_fs_request_t request = {.opcode = c->writing ? KB2_FILESYSTEM_OP_PWRITE :
        c->bytes ? KB2_FILESYSTEM_OP_PREAD : KB2_FILESYSTEM_OP_STATX,
        .generation = 1, .sequence = c->handle, .handle = c->handle, .length = c->bytes};
    void *data = c->bytes ? malloc(c->bytes) : NULL;
    const struct lpr_fs_buffer output = {c->writing ? NULL : data, c->writing ? 0 : c->bytes};
    const struct lpr_fs_buffer input = {data, c->bytes};
    if (c->writing) {
        memset(data, (unsigned char)c->handle, c->bytes);
        request.data = (kb2_fs_span_t){KB2_FILESYSTEM_REQUEST_SIZE, c->bytes};
    }
    kb2_fs_response_t response; size_t used;
    c->status = lpr_fs_session_call(&request, c->writing ? &input : NULL,
        c->writing ? 1 : 0, &response, &output, 1, &used);
    if (!c->status) {
        assert(response.sequence == c->handle && response.generation == 7 &&
            response.result == (int64_t)(c->bytes ? c->bytes : c->handle));
        if (!c->writing)
            for (size_t i = 0; i < c->bytes; ++i) assert(((unsigned char *)data)[i] == c->handle);
    }
    free(data); __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE); return NULL;
}
static void await_seen(unsigned handle)
{
    pthread_mutex_lock(&peer_lock);
    while (!seen[handle]) pthread_cond_wait(&peer_event, &peer_lock);
    pthread_mutex_unlock(&peer_lock);
}
static void fork_reset(void)
{
    unsigned before = closes;
    fork_cleanup = 1; lpr_fs_session_after_fork_child(); fork_cleanup = 0;
    assert(closes == before);
    pthread_mutex_lock(&peer_lock);
    for (unsigned i = 0; i < peer_count; ++i) peers[i].live = peers[i].reserved = 0;
    pthread_mutex_unlock(&peer_lock);
}
static void *drop(void *unused)
{ (void)unused; assert(!lpr_filed_session_drop()); return NULL; }
int main(void)
{
    alarm(30); lpr_state.thread_count = 8;
    pthread_t host; assert(!pthread_create(&host, NULL, server, NULL));
    pthread_t threads[3];
    struct caller first = {.handle = 1}, second = {.handle = 2}, large = {.handle = 3, .bytes = 1024 * 1024};
    hold_first = 1;
    assert(!pthread_create(&threads[0], NULL, call, &first)); await_seen(1);
    assert(!pthread_create(&threads[1], NULL, call, &second));
    assert(!pthread_join(threads[1], NULL) && !second.status && !__atomic_load_n(&first.done, __ATOMIC_ACQUIRE));
    assert(!pthread_create(&threads[2], NULL, call, &large));
    assert(!pthread_join(threads[2], NULL) && !large.status && !__atomic_load_n(&first.done, __ATOMIC_ACQUIRE));
    assert(peer_count == 2 && detaches == 1 && unmaps == 2);
    struct caller pressure[2] = {{.handle = 12, .bytes = 1024 * 1024},
        {.handle = 13, .bytes = 1024 * 1024}};
    pthread_mutex_lock(&peer_lock); lease_limit = 1; released &= ~(1ull << 12); pthread_mutex_unlock(&peer_lock);
    assert(!pthread_create(&threads[1], NULL, call, &pressure[0])); await_seen(12);
    assert(!pthread_create(&threads[2], NULL, call, &pressure[1]));
    pthread_mutex_lock(&peer_lock);
    while (!budget_rejections) pthread_cond_wait(&peer_event, &peer_lock);
    assert(!seen[13] && peer_count == 3 && !pressure[0].done);
    released |= 1ull << 12; pthread_cond_broadcast(&peer_event); pthread_mutex_unlock(&peer_lock);
    assert(!pthread_join(threads[1], NULL) && !pressure[0].status);
    assert(!pthread_join(threads[2], NULL) && !pressure[1].status);
    assert(seen[12] == 1 && seen[13] == 1 && peer_count == 4 && detaches == 3 && unmaps == 6);
    puts("LPR_FS_AGGREGATE_LEASE_BACKPRESSURE_RELEASE_ACK_AND_READMISSION_PASS");
    pthread_mutex_lock(&peer_lock); hold_first = 0; pthread_cond_broadcast(&peer_event); pthread_mutex_unlock(&peer_lock);
    assert(!pthread_join(threads[0], NULL) && !first.status);
    puts("LPR_FS_MULTIFLIGHT_REVERSED_COMPLETION_AND_LARGE_LEASE_PASS");
    struct caller full[3] = {{.handle = 4}, {.handle = 5}, {.handle = 6}};
    pthread_mutex_lock(&peer_lock); released &= ~((1ull << 4) | (1ull << 5)); pthread_mutex_unlock(&peer_lock);
    assert(!pthread_create(&threads[0], NULL, call, &full[0])); await_seen(4);
    assert(!pthread_create(&threads[1], NULL, call, &full[1])); await_seen(5);
    assert(!pthread_create(&threads[2], NULL, call, &full[2]));
    pthread_mutex_lock(&peer_lock);
    while (!third_waiting) pthread_cond_wait(&peer_event, &peer_lock);
    assert(!seen[6]); released |= 1ull << 5;
    pthread_cond_broadcast(&peer_event); pthread_mutex_unlock(&peer_lock);
    assert(!pthread_join(threads[1], NULL) && !full[1].status);
    assert(!pthread_join(threads[2], NULL) && !full[2].status && !__atomic_load_n(&full[0].done, __ATOMIC_ACQUIRE));
    pthread_t teardown;
    assert(!pthread_create(&teardown, NULL, drop, NULL));
    while (!lpr_fs_session_terminal()) sched_yield();
    assert(detaches == 3 && unmaps == 6); /* Base is pinned; completed large leases were freed. */
    struct caller late = {.handle = 7}; call(&late); assert(late.status == -LPR_LINUX_EIO);
    pthread_mutex_lock(&peer_lock); released |= 1ull << 4; pthread_cond_broadcast(&peer_event); pthread_mutex_unlock(&peer_lock);
    assert(!pthread_join(threads[0], NULL));
    assert(!pthread_join(teardown, NULL));
    assert(seen[4] == 1 && seen[5] == 1 && seen[6] == 1);
    assert(detaches == 4 && unmaps == 8);
    assert(lpr_fs_session_terminal());
    puts("LPR_FS_QUEUE_BACKPRESSURE_AND_TERMINAL_RETIREMENT_PASS");
    fork_reset();
    struct caller blocked_receiver = {.handle = 11};
    pthread_mutex_lock(&peer_lock); hold_first = 11; pthread_mutex_unlock(&peer_lock);
    assert(!pthread_create(&threads[0], NULL, call, &blocked_receiver)); await_seen(11);
    pthread_mutex_lock(&peer_lock); malformed = wake_close_failure = 1; pthread_mutex_unlock(&peer_lock);
    struct caller bad = {.handle = 8}; call(&bad); assert(bad.status < 0);
    assert(!pthread_join(threads[0], NULL) && blocked_receiver.status == -LPR_LINUX_EIO);
    wake_close_failure = 0;
    /* Neither a native receiver nor a local waiter needs another peer reply
     * after a sibling faults the queue. Its remote request is still pinned. */
    pthread_mutex_lock(&peer_lock);
    assert(!finished[11]); hold_first = 0; pthread_cond_broadcast(&peer_event);
    while (!finished[11]) pthread_cond_wait(&peer_event, &peer_lock);
    pthread_mutex_unlock(&peer_lock);
    puts("LPR_FS_LOCAL_FAULT_WAKES_NATIVE_RECEIVER_WITHOUT_REMOTE_REPLY_PASS");
    unsigned before = peer_count;
    struct caller cannot_reset = {.handle = 9, .bytes = 2 * 1024 * 1024}; call(&cannot_reset);
    assert(cannot_reset.status == -LPR_LINUX_EIO && peer_count == before && seen[8] == 1 && !seen[9]);
    bad_ack = 1; unsigned before_unmap = unmaps;
    assert(lpr_filed_session_drop() == -LPR_LINUX_EIO && unmaps == before_unmap);
    unsigned before_detach = detaches;
    assert(lpr_filed_session_drop() == -LPR_LINUX_EIO && detaches == before_detach && unmaps == before_unmap);
    fork_reset(); bad_ack = 0;
    pthread_mutex_lock(&peer_lock); malformed = 0; pthread_mutex_unlock(&peer_lock);
    attach_error = 1;
    struct caller lost = {.handle = 10}; call(&lost); assert(lost.status == -LPR_LINUX_EIO);
    before_unmap = unmaps; detach_error = 1;
    assert(lpr_filed_session_drop() == -LPR_LINUX_EIO && unmaps == before_unmap);
    fork_reset(); attach_error = detach_error = 0;
    puts("LPR_FS_UNCERTAIN_INVENTORY_AND_PRIVATE_FORK_PASS");
    for (unsigned i = 1; i <= 5; ++i) {
        unsigned before_detach = detaches, before_attach = attaches;
        initialize_failure = i;
        struct caller allocation = {.handle = 14 + i}; call(&allocation);
        assert(allocation.status == -LPR_LINUX_ENOMEM && detaches == before_detach + 1 &&
            attaches == before_attach && !peers[peer_count - 1].reserved);
        assert(!peers[peer_count - 1].metadata && !peers[peer_count - 1].mapping);
        initialize_failure = 0;
    }
    for (unsigned i = 7; i <= 8; ++i) {
        unsigned before_detach = detaches;
        initialize_failure = i;
        struct caller invalid = {.handle = 29 + i}; call(&invalid);
        assert(invalid.status == -LPR_LINUX_EIO && detaches == before_detach + 1 &&
            !peers[peer_count - 1].reserved && !peers[peer_count - 1].metadata);
        initialize_failure = 0;
    }
    before_detach = detaches;
    initialize_failure = 1; detach_error = 1;
    struct caller lost_cancel = {.handle = 20}; call(&lost_cancel);
    assert(lost_cancel.status == -LPR_LINUX_ENOMEM && detaches == before_detach + 1);
    unsigned before_peer = peer_count;
    struct caller no_reset = {.handle = 21}; call(&no_reset);
    assert(no_reset.status == -LPR_LINUX_EIO && peer_count == before_peer);
    assert(lpr_filed_session_drop() == -LPR_LINUX_EIO && detaches == before_detach + 1);
    initialize_failure = detach_error = 0; fork_reset();
    initialize_failure = 6;
    struct caller remote_close = {.handle = 22}; call(&remote_close);
    assert(remote_close.status == -LPR_LINUX_EIO && peers[peer_count - 1].metadata);
    initialize_failure = 0;
    assert(!lpr_filed_session_drop()); fork_reset();
    for (unsigned i = 1; i <= 3; ++i) {
        struct caller local = {.handle = 23 + i}; call(&local); assert(!local.status);
        cleanup_failure = i;
        unsigned before_detach = detaches;
        assert(lpr_filed_session_drop() == -LPR_LINUX_EIO && detaches == before_detach + 1);
        assert(peers[peer_count - 1].metadata);
        assert(lpr_filed_session_drop() == -LPR_LINUX_EIO && detaches == before_detach + 1);
        cleanup_failure = 0;
        assert(!lpr_filed_session_drop() && detaches == before_detach + 1 && !peers[peer_count - 1].metadata);
        fork_reset();
    }
    puts("LPR_FS_PREATTACH_RESERVATION_CANCELLATION_AND_LOCAL_CLEANUP_INVENTORY_PASS");
    pthread_mutex_lock(&peer_lock);
    wait_interruptions = 2;
    pthread_mutex_unlock(&peer_lock);
    struct caller interrupted = {.handle = 28, .bytes = 16, .writing = 1}; call(&interrupted);
    assert(!interrupted.status && seen[28] == 1 && interrupted_waits == 2 && writes[28] == 16 && finished[28] == 1);
    assert(!lpr_filed_session_drop()); fork_reset();
    puts("LPR_FS_INTERRUPTED_WAIT_NO_REPLAY_PASS");
    puts("LPR_FS_INTERRUPTED_PWRITE_SINGLE_EFFECT_PASS");
    pthread_mutex_lock(&peer_lock); malformed = 1; pthread_mutex_unlock(&peer_lock);
    struct caller ambiguous_write = {.handle = 29, .bytes = 16, .writing = 1};
    call(&ambiguous_write);
    if (ambiguous_write.status != -EPROTO || seen[29] != 1 || writes[29] != 16)
        fprintf(stderr, "ambiguous write status=%d seen=%u writes=%u finished=%u\n",
            ambiguous_write.status, seen[29], writes[29], finished[29]);
    assert(ambiguous_write.status == -EPROTO && seen[29] == 1 && writes[29] == 16);
    unsigned before_ambiguous = peer_count;
    call(&ambiguous_write);
    assert(ambiguous_write.status == -LPR_LINUX_EIO && peer_count == before_ambiguous &&
        seen[29] == 1 && writes[29] == 16);
    bad_ack = 1; before_unmap = unmaps;
    assert(lpr_filed_session_drop() == -LPR_LINUX_EIO && unmaps == before_unmap);
    before_detach = detaches;
    assert(lpr_filed_session_drop() == -LPR_LINUX_EIO && detaches == before_detach && unmaps == before_unmap);
    fork_reset(); bad_ack = 0;
    pthread_mutex_lock(&peer_lock); malformed = 0; pthread_mutex_unlock(&peer_lock);
    puts("LPR_FS_AMBIGUOUS_WRITE_LOST_ACK_STAYS_FAILED_NO_REPLAY_PASS");
    struct caller retained_control = {.handle = 38}; call(&retained_control);
    assert(!retained_control.status);
    control_close_failure = 1;
    assert(lpr_filed_session_drop() == -LPR_LINUX_EIO && lpr_filed_client_fd == 50 &&
        !lpr_filed_client_id && lpr_filed_endpoint_checked == -1);
    control_close_failure = 0;
    assert(!lpr_filed_session_drop() && lpr_filed_client_fd == -1); fork_reset();
    malformed_config = 1;
    struct caller bad_config = {.handle = 39}; call(&bad_config);
    assert(bad_config.status == -LPR_LINUX_EIO);
    unsigned before_config = peer_count;
    struct caller cannot_hello_reset = {.handle = 40}; call(&cannot_hello_reset);
    assert(cannot_hello_reset.status == -LPR_LINUX_EIO && peer_count == before_config);
    assert(lpr_filed_session_drop() == -LPR_LINUX_EIO);
    malformed_config = 0; fork_reset();
    puts("LPR_FS_CONTROL_CLOSE_AND_MALFORMED_RESERVATION_INVENTORY_PASS");
    for (unsigned i = 1; i <= 3; ++i) {
        cleanup_failure = i;
        unsigned before_detach = detaches;
        struct caller oversized_cleanup = {.handle = 40 + i, .bytes = 1024 * 1024};
        call(&oversized_cleanup);
        assert(oversized_cleanup.status == -LPR_LINUX_EIO && seen[40 + i] == 1 &&
            detaches == before_detach + 1 && peers[peer_count - 1].metadata &&
            !peers[peer_count - 1].reserved);
        unsigned before_peer = peer_count;
        struct caller retained_large = {.handle = 44}; call(&retained_large);
        assert(retained_large.status == -LPR_LINUX_EIO && peer_count == before_peer);
        cleanup_failure = 0;
        assert(!lpr_filed_session_drop() && detaches == before_detach + 2);
        fork_reset();
    }
    puts("LPR_FS_TEMPORARY_LEASE_LOCAL_CLEANUP_FAILURE_PINNED_NO_REPLAY_PASS");
    pthread_mutex_lock(&peer_lock); stop_server = 1; pthread_cond_broadcast(&peer_event); pthread_mutex_unlock(&peer_lock);
    assert(!pthread_join(host, NULL));
    puts("LPR_FS_SESSION_LIFETIME_UNIT_PASS");
    return 0;
}

#include "lpr_ipc_mock.h"
int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send, uint64_t receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    return test_lpr_ipc_noncall(nr, fd, send, receive, timeout, flags, committed);
}
