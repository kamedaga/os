#include "unixd/client.h"
#include "pacha/status.h"
#include <errno.h>
static int create_page(struct unix_control **out)
{
    const uint64_t page_rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int page_fd = pacha_vmo_create(UNIX_CONTROL_BYTES, page_rights, PACHA_FD_FLAG_PRIVATE);
    if (page_fd < 16) return page_fd < 0 ? (int)pacha_kernel_status_to_errno(page_fd) : -EIO;
    *out = pacha_mmap(page_fd, UNIX_CONTROL_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!*out) { (void)pacha_fd_close(page_fd); return -ENOMEM; }
    return page_fd;
}

static void destroy_page(int page_fd, struct unix_control *page)
{
    (void)pacha_munmap(page, UNIX_CONTROL_BYTES);
    (void)pacha_fd_close(page_fd);
}

static int call(int endpoint, const struct pacha_ipc_msg *request)
{
    const int fd = pacha_ipc_call(endpoint, request);
    return fd >= 16 ? fd : fd < 0 ? (int)pacha_kernel_status_to_errno(fd) : -EIO;
}

static int receive(int fd, struct pacha_ipc_msg *reply)
{
    return (int)pacha_kernel_status_to_errno(pacha_ipc_recv_wait(fd, reply, PACHA_FD_WAIT_FOREVER));
}

static void close_fd(int fd) { (void)pacha_fd_close(fd); }

int unix_client_call(int endpoint, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *caps, unsigned capacity, unsigned *received)
{
    const struct unix_client_io io = { create_page, destroy_page, call, receive, close_fd };
    return unix_client_exchange(&io, endpoint, request, send, send_count, caps, capacity, received);
}
