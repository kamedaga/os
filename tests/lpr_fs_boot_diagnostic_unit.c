#include "lpr_ipc_mock.h"
/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/personality/linux/runtime/lpr_vfs/session.h"
#include "../userland/personality/linux/runtime/lpr_common/boot_diagnostic.h"
#include <kobox2/virtqueue_x86_64.h>

lpr_state_t lpr_state;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
lpr_boot_diagnostic_state_t lpr_boot_diagnostic_state;
#endif
struct allocation { void *memory; uint64_t bytes; int fd; };
static struct allocation allocations[32];
static unsigned allocation_count, next_fd, logs, publishes, reads, controls, sends;
static int endpoint_ready;
static int64_t log_result;
static struct ph_fs_channel device;
static kb2_vq_chain_t *device_slots[8];
static uint16_t device_owners[8];
static struct pacha_ipc_msg completion;

void lpr_state_lock(volatile uint32_t *word) { assert(!*word); *word = 1; }
void lpr_state_unlock(volatile uint32_t *word) { assert(*word); *word = 0; }
void *lpr_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void *lpr_memcpy(void *p, const void *q, size_t n) { return memcpy(p, q, n); }
size_t lpr_strnlen(const char *p, size_t n) { size_t i = 0; while (i < n && p[i]) ++i; return i; }
uint64_t lpr_page_align_up(uint64_t n) { return (n + 4095) & ~4095ull; }
uint64_t lpr_next_request_id(volatile uint64_t *counter) { return ++*counter; }
int64_t lpr_pacha_status_to_errno(int64_t status) { return status; }
int64_t pacha_kernel_status_to_errno(int64_t status) { return status; }

int64_t lpr_filed_endpoint_ready(void)
{
    lpr_boot_diagnostic_fs_begin();
    if (!endpoint_ready) {
        /* Endpoint/supervisor and stdio are external to this fixture. Reserve
         * their actual successful-path record counts before exercising the
         * real RPC, lazy session setup and mapped virtqueue together. */
        for (unsigned i = 0; i < 10; ++i)
            lpr_boot_diagnostic_fs_emit("endpoint-fixture", i, 0, 0, 0, 0);
        endpoint_ready = 1;
    }
    lpr_filed_client_fd = 50;
    lpr_filed_client_id = 2;
    lpr_filed_endpoint_checked = 1;
    return 0;
}

static struct allocation *find_fd(uint64_t fd)
{
    for (unsigned i = 0; i < allocation_count; ++i)
        if (allocations[i].fd == (int)fd) return &allocations[i];
    assert(0); return NULL;
}

static void put64(unsigned char *p, uint64_t value)
{ for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(value >> (8 * i)); }

static int64_t serve_doorbell(void)
{
    /* This synchronous mock services even unnotified requests so both
     * diagnostic boundaries complete. Lost-wakeup semantics have their own
     * real relay regression; this test measures observer noninterference. */
    kb2_vq_segment_t segments[8];
    kb2_vq_chain_t chain = {.segments = segments, .capacity = 8};
    assert(!kb2_vq_take_available(&device.lane, 7, &chain));
    unsigned char wire[KB2_FILESYSTEM_REQUEST_SIZE];
    assert(!kb2_vq_copy_request(&device.lane, 7, &chain, 0, wire, sizeof(wire)));
    kb2_fs_request_t request;
    assert(!kb2_fs_request_decode(wire, sizeof(wire), chain.readable, &request));
    kb2_fs_response_t response = {.opcode = request.opcode, .generation = 7,
        .sequence = request.sequence};
    unsigned char output[KB2_FILESYSTEM_RESPONSE_SIZE];
    assert(!kb2_fs_response_encode(output, sizeof(output), &response));
    assert(!kb2_vq_write_response(&device.lane, 7, &chain, output, sizeof(output)));
    int notify;
    assert(!kb2_vq_complete(&device.lane, 7, &chain, &notify));
    ++reads; return 0;
}

int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t a, uint64_t b, uint64_t flags)
{
    if (nr == PACHAOS_SYSCALL_IPC_NOTIFY_WAIT) {
        assert(a == 80 && b == UINT64_MAX && flags == PACHA_WAIT_UNINTERRUPTIBLE);
        return serve_doorbell();
    }
    assert(flags == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
    if (nr == PACHAOS_SYSCALL_IPC_CHANNEL_CREATE) {
        uint64_t *pair = (void *)(uintptr_t)a; pair[0] = 80; pair[1] = 81; return 0;
    }
    assert(nr == PACHAOS_SYSCALL_VMO_CREATE && (b & PACHA_FD_RIGHT_INSPECT));
    assert(allocation_count < 32);
    struct allocation *slot = &allocations[allocation_count++];
    *slot = (struct allocation){.bytes = a, .fd = (int)next_fd++};
    return slot->fd;
}

int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t address,
    uint64_t bytes, uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(nr == PACHAOS_SYSCALL_MMAP && !address && !offset);
    assert(prot == (PACHAOS_PROT_READ | PACHAOS_PROT_WRITE));
    struct allocation *slot;
    if (!fd) {
        assert(flags == (PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS));
        assert(allocation_count < 32);
        slot = &allocations[allocation_count++];
        *slot = (struct allocation){.bytes = bytes, .fd = -1};
    } else {
        assert(flags == PACHAOS_MMAP_SHARED);
        slot = find_fd(fd); assert(slot->bytes == bytes);
    }
    assert(!slot->memory);
    slot->memory = calloc(1, bytes); assert(slot->memory);
    return (int64_t)(uintptr_t)slot->memory;
}

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    if (nr == PACHAOS_SYSCALL_IPC_NOTIFY) { assert(fd == 80); ++sends; return 0; }
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd >= 16); return 0;
}

static int64_t fixture_prepare_request(uint64_t nr, uint64_t a, uint64_t b)
{
    if (nr == PACHAOS_SYSCALL_LOG) {
        const char *line = (void *)(uintptr_t)a;
        assert(b > 0 && b <= 256 && line[b - 1] == '\n');
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
        char copy[257]; memcpy(copy, line, b); copy[b] = 0;
        if (strstr(copy, "fs.queue.publish")) {
            /* The fixture deliberately does not arm its device between
             * calls: the real producer must report notify=1 then notify=0. */
            assert(strstr(copy, publishes ? "status=0 raw=0" : "status=1 raw=0"));
            ++publishes;
        }
#endif
        ++logs; return log_result;
    }
    if (nr == PACHAOS_SYSCALL_MUNMAP) {
        for (unsigned i = 0; i < allocation_count; ++i)
            if (allocations[i].memory == (void *)(uintptr_t)a) {
                assert(allocations[i].bytes == b);
                free(allocations[i].memory); allocations[i].memory = NULL; return 0;
            }
        assert(0);
    }
    assert(nr == PACHAOS_SYSCALL_IPC_CALL && a == 50);
    const struct pacha_ipc_msg *message = (void *)(uintptr_t)b;
    unsigned char *wire = find_fd(message->fds[0].fd)->memory;
    kb2_fs_request_t request;
    assert(!kb2_fs_request_decode(wire, message->word1, message->word1, &request));
    kb2_fs_response_t response = {.opcode = request.opcode,
        .generation = request.generation, .sequence = request.sequence};
    if (request.opcode == KB2_FILESYSTEM_OP_HELLO) {
        assert(request.flags == 1);
        response.data_length = 32;
        response.handle = 99;
        put64(wire + KB2_FILESYSTEM_RESPONSE_SIZE, 7);
        put64(wire + KB2_FILESYSTEM_RESPONSE_SIZE + 8, 99);
        put64(wire + KB2_FILESYSTEM_RESPONSE_SIZE + 16, 65536);
        put64(wire + KB2_FILESYSTEM_RESPONSE_SIZE + 24, 8);
    } else if (request.opcode == KB2_FILESYSTEM_OP_SESSION_DETACH) {
        assert(message->fd_count == 1 && message->word1 == KB2_FILESYSTEM_REQUEST_SIZE);
        assert(request.generation == 7 && request.second_handle == 99 && !request.handle &&
            !request.flags && !request.mask && !request.length && !request.offset);
        response.handle = 99;
    } else {
        assert(request.opcode == KB2_FILESYSTEM_OP_SESSION_ATTACH && message->fd_count == 3);
        struct allocation *queue = find_fd(message->fds[1].fd);
        const struct ph_fs_channel_config config = {7, 99, 65536, 8};
        assert(!ph_fs_channel_bind(&device, queue->memory, queue->bytes,
            &config, KB2_VQ_DEVICE, &kb2_vq_x86_64_atomics, device_slots, device_owners));
        response.handle = 99;
    }
    assert(!kb2_fs_response_encode(wire, message->word2, &response));
    completion = (struct pacha_ipc_msg){.word0 = request.opcode,
        .word2 = KB2_FILESYSTEM_RESPONSE_SIZE + response.data_length, .word3 = request.sequence};
    ++controls; return 90;
}

static int64_t fixture_finish_request(uint64_t nr, uint64_t fd, uint64_t raw,
    uint64_t timeout, uint64_t flags)
{
    assert(timeout == UINT64_MAX && flags ==
        (nr == PACHAOS_SYSCALL_IPC_RECV_WAIT ? PACHA_WAIT_UNINTERRUPTIBLE : 0));
    if (nr == PACHA_FD_SYSCALL_WAIT_MANY) {
        struct pacha_pollfd *item = (void *)(uintptr_t)fd;
        assert(raw == 1 && item->fd == 80 && item->events == PACHA_FD_EVENT_HANGUP);
        item->revents = PACHA_FD_EVENT_HANGUP; return 1;
    }
    assert(nr == PACHAOS_SYSCALL_IPC_RECV_WAIT);
    struct pacha_ipc_msg *reply = (void *)(uintptr_t)raw;
    if (fd == 90) { *reply = completion; return 0; }
    assert(0);
    return PACHA_SYSCALL_ERR_INVALID;
}

static void exercise(int64_t rejected_log)
{
    lpr_fs_session_after_fork_child();
    memset(&lpr_state, 0, sizeof(lpr_state));
    memset(&device, 0, sizeof(device));
    memset(allocations, 0, sizeof(allocations));
    allocation_count = 0; next_fd = 100; endpoint_ready = 0;
    logs = publishes = reads = controls = sends = 0; log_result = rejected_log;
    lpr_linux_current_pid = 1;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    memset(&lpr_boot_diagnostic_state, 0, sizeof(lpr_boot_diagnostic_state));
#endif
    for (unsigned i = 0; i < 15; ++i)
        lpr_boot_diagnostic_emit("stdio-bootstrap-fixture", i, 0, 0, 0, 0);
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_READ, .handle = 42};
    kb2_fs_response_t response;
    for (unsigned i = 0; i < 2; ++i)
        assert(!lpr_fs_call(&request, NULL, NULL, NULL, 0, NULL, 0, &response, -1, NULL));
    assert(controls == 2 && reads == 2 && sends == 1);
    for (unsigned i = 0; i < 10; ++i)
        lpr_boot_diagnostic_read_enter(0, 3, LPR_FD_OPS_PIPE, UINT64_MAX);
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    if (!rejected_log) {
        /* The asynchronous session uses ph_fs_channel_publish/take rather
         * than ph_fs_channel_call: FS_DIAG(26) belongs to the latter.
         * Actual EVENT_IDX coalescing is checked above via sends == 1. */
        assert(publishes == 0 && logs < LPR_BOOT_DIAGNOSTIC_RECORDS);
        assert(lpr_boot_diagnostic_state.fs_completed == 2 && !lpr_boot_diagnostic_fs_active());
        assert(lpr_boot_diagnostic_state.fd3_reads == 3 && logs == 48);
        printf("LPR FS boot observer: stdio15 + endpoint10 + real session/two VQ calls + fd3read3 = %u/64 records\n", logs);
    } else {
        assert(logs == 1 && lpr_boot_diagnostic_state.last_log_status == rejected_log);
    }
#else
    assert(!logs);
#endif
    const unsigned before = logs;
    assert(!lpr_fs_call(&request, NULL, NULL, NULL, 0, NULL, 0, &response, -1, NULL));
    assert(logs == before && reads == 3 && sends == 1);
    lpr_fs_session_drop();
    for (unsigned i = 0; i < allocation_count; ++i) assert(!allocations[i].memory);
}

int main(void)
{
    exercise(0); exercise(19); exercise(-19);
    puts("LPR_FS_BOOT_DIAGNOSTIC_UNIT_PASS");
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
