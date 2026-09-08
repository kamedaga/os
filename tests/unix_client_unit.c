#include "unixd/client.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static _Alignas(struct unix_control) unsigned char memory[UNIX_CONTROL_BYTES];
static unsigned live[256], maps;
enum { NORMAL, BAD_REPLY, BAD_PAGE, SERVICE_ERROR, CALL_ERROR, RECEIVE_ERROR, MAP_ERROR, ALLOC_ERROR };
static int mode;

int pacha_vmo_create(uint64_t bytes, uint64_t rights, uint32_t flags)
{
    assert(bytes == UNIX_CONTROL_BYTES && !live[20]);
    assert((rights & PACHA_FD_RIGHT_TRANSFER) && flags == PACHA_FD_FLAG_PRIVATE);
    if (mode == ALLOC_ERROR) return PACHA_ERR_ALLOC;
    live[20] = 1;
    return 20;
}

void *pacha_mmap(int fd, uint64_t bytes, uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(fd == 20 && live[20] && bytes == sizeof(memory) && !offset);
    assert(prot == (PACHA_PROT_READ | PACHA_PROT_WRITE) && flags == PACHA_MMAP_SHARED);
    if (mode == MAP_ERROR) return NULL;
    maps++;
    return memory;
}

int pacha_munmap(void *address, uint64_t bytes)
{
    assert(address == memory && bytes == sizeof(memory) && maps == 1);
    maps--;
    return 0;
}

int pacha_fd_close(int fd)
{
    assert(fd >= 16 && fd < 256 && live[fd]);
    live[fd]--;
    return 0;
}

int pacha_ipc_call(int endpoint, const struct pacha_ipc_msg *request)
{
    assert(endpoint == 200 && request->fd_count == 1 && request->fds[0].fd == 20);
    assert(!(request->fds[0].rights & PACHA_FD_RIGHT_TRANSFER));
    assert(request->word0 == UNIX_SERVICE_MAGIC && request->word1 == UNIX_OP_HELLO && request->word3 == 9);
    if (mode == CALL_ERROR) return PACHA_ERR_INVALID;
    struct unix_control *page = (struct unix_control *)memory;
    assert(page->magic == UNIX_SERVICE_MAGIC && page->version == UNIX_SERVICE_VERSION);
    page->magic = UNIX_REPLY_MAGIC;
    page->status = mode == SERVICE_ERROR ? -EPERM : 0;
    page->result = 41;
    if (mode == BAD_PAGE) page->request++;
    assert(!live[21]);
    live[21] = 1;
    return 21;
}

int pacha_ipc_recv_wait(int fd, struct pacha_ipc_msg *reply, uint64_t timeout)
{
    assert(fd == 21 && live[21] && timeout == PACHA_FD_WAIT_FOREVER);
    if (mode == RECEIVE_ERROR) return PACHA_ERR_INVALID;
    assert(reply->fd_capacity == PACHA_IPC_MAX_TRANSFER_FDS);
    reply->word0 = mode == BAD_REPLY ? 0 : UNIX_REPLY_MAGIC;
    reply->word1 = mode == SERVICE_ERROR ? (uint64_t)(int64_t)-EPERM : 0;
    reply->word2 = 41;
    reply->word3 = 9;
    reply->fd_count = 1;
    reply->fds[0] = (struct pacha_ipc_fd){ .fd = 22, .rights = PACHA_FD_RIGHT_CLOSE };
    assert(!live[22]); live[22] = 1;
    return 0;
}

int main(void)
{
    struct unix_control request = { .operation = UNIX_OP_HELLO, .request = 9 };
    struct pacha_ipc_fd received_cap = {0};
    unsigned received;
    assert(unix_client_call(200, &request, NULL, 0, &received_cap, 1, &received) == 0);
    assert(received == 1 && received_cap.fd == 22 && request.result == 41 && live[22]);
    pacha_fd_close(22);
    for (mode = NORMAL; mode <= ALLOC_ERROR; mode++) {
        request = (struct unix_control){ .operation = UNIX_OP_HELLO, .request = 9, .result = 123 };
        received = 999;
        const int status = unix_client_call(200, &request, NULL, 0, NULL, 0, &received);
        assert(status < 0 && !received && request.result == 123);
        if (mode == NORMAL) assert(status == -ENOBUFS);
        if (mode == BAD_REPLY || mode == BAD_PAGE) assert(status == -EPROTO);
        if (mode == SERVICE_ERROR) assert(status == -EPERM);
        assert(!maps);
        for (int fd = 16; fd < 256; fd++) assert(!live[fd]);
    }
    assert(unix_client_call(200, &request, NULL, PACHA_IPC_MAX_TRANSFER_FDS, NULL, 0, &received) == -EINVAL);
    puts("unix client: reply correlation, capability bounds, errors and cleanup passed");
}
