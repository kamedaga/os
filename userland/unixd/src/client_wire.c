#include "unixd/client.h"
#include <errno.h>

/* No libc dependency: this wire implementation also links into LPR. */
static void copy_bytes(void *destination, const void *source, size_t count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < count; i++) out[i] = in[i];
}

int unix_client_exchange_page(const struct unix_client_io *io, int endpoint,
    struct unix_control *request, const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received,
    const struct unix_client_buffer *buffer)
{
    if (!io || !io->page_create || !io->page_destroy || !io->call || !io->receive || !io->close ||
        !request || !request->request || !received || endpoint < 16 || endpoint >= 256 ||
        (send_count && !send) || (capacity && !receive) ||
        send_count > PACHA_IPC_MAX_TRANSFER_FDS - 2 || capacity > PACHA_IPC_MAX_TRANSFER_FDS)
        return -EINVAL;
    *received = 0;
    if (!buffer || buffer->fd < 16 || !buffer->page) return -EINVAL;
    struct unix_control *page = buffer->page;
    const int page_fd = buffer->fd;
    copy_bytes(page, request, sizeof(*request));
    page->magic = UNIX_SERVICE_MAGIC;
    page->version = UNIX_SERVICE_VERSION;
    page->status = 0;
    page->result = 0;
    struct pacha_ipc_fd outgoing[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
    outgoing[0] = (struct pacha_ipc_fd){ .fd = (uint64_t)page_fd,
        .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE };
    if (send_count) copy_bytes(outgoing + 1, send, send_count * sizeof(*send));
    struct pacha_ipc_msg message = { .word0 = UNIX_SERVICE_MAGIC, .word1 = request->operation,
        .word3 = request->request, .fds = outgoing, .fd_count = send_count + 1 };
    int status;
    const int reply_fd = io->call(endpoint, &message);
    if (reply_fd < 16) status = reply_fd < 0 ? reply_fd : -EIO;
    else {
        struct pacha_ipc_fd incoming[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
        struct pacha_ipc_msg reply = { .fds = incoming, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS };
        status = io->receive(reply_fd, &reply);
        if (status == 0) {
            struct unix_control result;
            copy_bytes(&result, page, sizeof(result));
            if (reply.word0 != UNIX_REPLY_MAGIC || reply.word3 != request->request ||
                result.magic != UNIX_REPLY_MAGIC || result.version != UNIX_SERVICE_VERSION ||
                result.request != request->request || result.operation != request->operation ||
                result.status != (int64_t)reply.word1 || result.result != reply.word2 ||
                result.status > 0 || result.status < -4095 || reply.fd_count > PACHA_IPC_MAX_TRANSFER_FDS)
                status = -EPROTO;
            else if (result.status) status = (int)result.status;
            else if (reply.fd_count > capacity) status = -ENOBUFS;
            else {
                copy_bytes(request, &result, sizeof(result));
                *received = (unsigned)reply.fd_count;
                if (*received) copy_bytes(receive, incoming, *received * sizeof(*receive));
            }
        }
        if (status != 0)
            for (unsigned i = 0; i < reply.fd_count && i < PACHA_IPC_MAX_TRANSFER_FDS; i++)
                if (incoming[i].fd >= 16) io->close((int)incoming[i].fd);
        io->close(reply_fd);
    }
    return status;
}

int unix_client_exchange(const struct unix_client_io *io, int endpoint,
    struct unix_control *request, const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received)
{
    if (!io || !io->page_create || !io->page_destroy || !request || !received)
        return -EINVAL;
    *received = 0;
    struct unix_client_buffer buffer = { .fd = -1 };
    buffer.fd = io->page_create(&buffer.page);
    if (buffer.fd < 16) return buffer.fd < 0 ? buffer.fd : -EIO;
    int status = unix_client_exchange_page(io, endpoint, request, send, send_count,
        receive, capacity, received, &buffer);
    io->page_destroy(buffer.fd, buffer.page);
    return status;
}
