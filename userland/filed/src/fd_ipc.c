#include "filed/fd_ipc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pacha/abi.h"

int filed_ipc_recv_wait(int fd, struct pacha_ipc_msg *msg)
{
    if (fd < 16 || msg == NULL) {
        return -1;
    }

    return pacha_ipc_recv_wait(fd, msg, PACHA_FD_WAIT_FOREVER);
}

int filed_ipc_send_wait(int fd, const struct pacha_ipc_msg *msg)
{
    if (fd < 16 || msg == NULL) {
        return -1;
    }

    for (;;) {
        const int status = pacha_ipc_send(fd, msg);
        if (status == 0) {
            return 0;
        }
        if (status != PACHA_ERR_NOT_READY) {
            return status;
        }

        struct pacha_pollfd pollfd = {
            .fd = fd,
            .events = PACHA_FD_EVENT_WRITABLE,
            .revents = 0,
        };
        const long wait_status =
            pacha_fd_wait_many(&pollfd, 1, PACHA_FD_WAIT_FOREVER);
        if (wait_status < 0) {
            return (int)wait_status;
        }
    }
}

int filed_ipc_create_wire_page(uint64_t size, filed_page_t *out_page)
{
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;

    if (size == 0 || out_page == NULL) {
        return -1;
    }

    memset(out_page, 0, sizeof(*out_page));
    out_page->fd = -1;
    out_page->size = size;

    const int fd = pacha_vmo_create(size, rights, 0);
    if (fd < 16) {
        return fd;
    }

    void *addr = pacha_mmap(
        fd,
        size,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (addr == NULL) {
        (void)pacha_fd_close(fd);
        return -2;
    }

    memset(addr, 0, (size_t)size);
    out_page->fd = fd;
    out_page->addr = addr;
    return 0;
}

void filed_ipc_destroy_wire_page(filed_page_t *page)
{
    if (page == NULL) {
        return;
    }
    if (page->addr != NULL && page->size != 0) {
        (void)pacha_munmap(page->addr, page->size);
    }
    if (page->fd >= 16) {
        (void)pacha_fd_close(page->fd);
    }
    memset(page, 0, sizeof(*page));
    page->fd = -1;
}

int filed_ipc_create_client_endpoint(void)
{
    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_SEND |
        PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_CALL |
        PACHA_FD_RIGHT_TRANSFER;

    return pacha_ipc_endpoint_create(rights, PACHA_FD_FLAG_CLOEXEC);
}
