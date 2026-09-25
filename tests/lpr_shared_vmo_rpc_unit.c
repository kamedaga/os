#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"

lpr_state_t lpr_state;
static _Thread_local unsigned slot;
static unsigned char pages[2][FILED_PAGE_BYTES] __attribute__((aligned(16)));
static lpr_filed_backend_t files[2];
static unsigned live[2], allocated[2], released[2], closed[2], cached;
static int overlap, failure;
static pthread_barrier_t barrier;

void *lpr_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void lpr_zero_bytes(void *p, uint64_t n) { memset(p, 0, n); }
int lpr_fd_is_filed(uint64_t fd) { return fd == 10 + slot; }
lpr_filed_backend_t *lpr_filed_backend(uint64_t fd)
{
    assert(fd == 10 + slot);
    return &files[slot];
}
uint64_t lpr_next_request_id(volatile uint64_t *counter)
{
    return __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
}
int64_t lpr_pacha_status_to_errno(int64_t status)
{
    assert(status == 3);
    return -5;
}
int64_t lpr_filed_endpoint_ready(void) { return failure == 1 ? -38 : 0; }
int lpr_create_standalone_wire_page(void **out)
{
    if (failure == 2) return -12;
    assert(!live[slot]);
    live[slot] = 1;
    allocated[slot]++;
    *out = pages[slot];
    return 40 + slot;
}
void lpr_destroy_standalone_wire_page(int fd, void *page)
{
    assert(fd == (int)(40 + slot) && page == pages[slot] && live[slot]);
    live[slot] = 0;
    released[slot]++;
}
int lpr_create_pread_vmo_wire_page(void **out)
{
    /* Shared requests must never acquire the general FileD RPC wire lock. */
    assert(!overlap);
    cached++;
    return lpr_create_standalone_wire_page(out);
}
void lpr_destroy_pread_vmo_wire_page(int fd, void *page)
{
    assert(cached && !overlap);
    lpr_destroy_standalone_wire_page(fd, page);
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t raw)
{
    assert(nr == PACHAOS_SYSCALL_IPC_CALL && fd == 21);
    struct pacha_ipc_msg *request = (void *)(uintptr_t)raw;
    assert(request->fd_count == 1 && request->fds[0].fd == 40 + slot);
    pacha_service_envelope_t *header = (void *)pages[slot];
    filed_file_vmo_request_t *payload = (void *)(pages[slot] + PACHA_SERVICE_HEADER_BYTES);
    assert(header->service_id == FILED_SERVICE_ID && header->request_id == request->word3);
    assert(payload->handle == files[slot].handle && payload->length == 8192);
    assert(payload->file_offset == slot * 4096);
    if (header->op == FILED_OP_VFS_SHARED_FILE_VMO)
        assert(payload->flags == (FILED_FILE_VMO_EXEC | (slot ? FILED_FILE_VMO_WRITE : 0)));
    else assert(header->op == FILED_OP_VFS_FILE_VMO && payload->flags == 0);
    return failure == 3 ? 3 : 80 + slot;
}
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t fd, uint64_t raw, uint64_t timeout, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_IPC_RECV_WAIT && fd == 80 + slot);
    assert(timeout == UINT64_MAX && flags == 0);
    if (overlap) {
        int result = pthread_barrier_wait(&barrier);
        assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    }
    if (failure == 4) return 3;
    struct pacha_ipc_msg *reply = (void *)(uintptr_t)raw;
    pacha_service_envelope_t *header = (void *)pages[slot];
    header->magic = PACHA_SERVICE_REPLY_MAGIC;
    header->status = failure == 6 ? -13 : 0;
    header->result = 4096 + slot;
    reply->word0 = failure == 5 ? 0 : PACHA_SERVICE_REPLY_MAGIC;
    reply->word3 = header->request_id;
    assert(reply->fd_capacity == 1);
    reply->fd_count = failure ? 0 : 1;
    if (reply->fd_count) reply->fds[0].fd = 200 + slot;
    return 0;
}
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd == 80 + slot);
    closed[slot]++;
    return 0;
}
static void *run(void *index)
{
    slot = (unsigned)(uintptr_t)index;
    uint64_t loaded = 0;
    assert(lpr_linux_shared_file_vmo(10 + slot, slot * 4096, 8192, slot != 0, 1, &loaded)
           == (int64_t)(200 + slot));
    assert(loaded == 4096 + slot && !live[slot]);
    return NULL;
}
int main(void)
{
    files[0].handle = 100;
    files[1].handle = 101;
    lpr_filed_client_fd = 21;
    assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
    overlap = 1;
    pthread_t a, b;
    assert(pthread_create(&a, NULL, run, (void *)0) == 0);
    assert(pthread_create(&b, NULL, run, (void *)1) == 0);
    assert(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
    overlap = 0;
    assert(!cached && allocated[0] == 1 && allocated[1] == 1);
    assert(released[0] == 1 && released[1] == 1 && closed[0] == 1 && closed[1] == 1);
    const int64_t expected[] = {0, -38, -12, -5, -5, -5, -13, -5};
    for (failure = 1; failure <= 7; ++failure) {
        uint64_t loaded = 99;
        assert(lpr_linux_shared_file_vmo(10, 0, 8192, 0, 1, &loaded) == expected[failure]);
        assert(loaded == 0 && !live[0] && allocated[0] == released[0]);
    }
    failure = 0;
    uint64_t loaded = 0;
    assert(lpr_linux_file_vmo(10, 0, 8192, &loaded) == 200 && loaded == 4096);
    assert(cached == 1 && allocated[0] == released[0]);
    assert(lpr_linux_shared_file_vmo(99, 0, 8192, 0, 1, &loaded) == -LPR_LINUX_EBADF);
    assert(lpr_linux_shared_file_vmo(10, 0, 0, 0, 1, &loaded) == -LPR_LINUX_EINVAL);
    assert(pthread_barrier_destroy(&barrier) == 0);
    puts("shared VMO independent RPC, cleanup and private wire policy: PASS");
}
