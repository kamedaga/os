#include "unixd/client.h"
#include "unixd/profile.h"
#include <errno.h>

/* No libc dependency: this wire implementation also links into LPR. */
static void copy_bytes(void *destination, const void *source, size_t count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < count; i++) out[i] = in[i];
}

static int exchange_page_once(const struct unix_client_io *io, int endpoint,
    struct unix_control *request, const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received,
    struct unix_client_buffer *buffer, int *miss)
{
    *miss = 0;
    /* 256 is the initial table capacity, not the native descriptor limit.
     * LPR can grow its table before creating per-thread UNIX sessions. */
    if (!io || !io->page_create || !io->page_destroy || !io->call || !io->receive || !io->close ||
        !request || !request->request || !received || endpoint < 16 || endpoint >= PACHA_FD_TABLE_LIMIT ||
        (send_count && !send) || (capacity && !receive) ||
        send_count > PACHA_IPC_MAX_TRANSFER_FDS - 2 || capacity > PACHA_IPC_MAX_TRANSFER_FDS)
        return -EINVAL;
    *received = 0;
    if (!buffer || buffer->fd < 16 || !buffer->page) return -EINVAL;
    struct unix_control *page = buffer->page;
    const int page_fd = buffer->fd;
    UP_BEGIN(serialize, UP_RPC, request->operation, UP_SERIALIZE);
    copy_bytes(page, request, sizeof(*request));
    page->magic = UNIX_SERVICE_MAGIC;
    page->version = UNIX_SERVICE_VERSION;
    page->status = 0;
    page->result = 0;
    page->buffer_token = 0;
    uint64_t token = buffer->session ? (buffer->token ? buffer->token : 1) : 0;
    /* A miss consumes and closes received copies. MOVE donors would no
     * longer exist for retry, so such calls always attach the page. */
    for (unsigned i = 0; i < send_count; i++)
        if ((send[i].transfer_flags & PACHA_IPC_TRANSFER_MOVE) && token >= 2) token = 1;
    const unsigned page_caps = token < 2;
    struct pacha_ipc_fd outgoing[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
    outgoing[0] = (struct pacha_ipc_fd){ .fd = (uint64_t)page_fd,
        .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE };
    if (send_count) copy_bytes(outgoing + page_caps, send, send_count * sizeof(*send));
    struct pacha_ipc_msg message = { .word0 = UNIX_SERVICE_MAGIC, .word1 = request->operation,
        .word2 = token, .word3 = request->request, .fds = outgoing, .fd_count = send_count + page_caps };
    int status;
    UP_END(serialize);
    UP_BEGIN(call, UP_RPC, request->operation, UP_CALL);
    const int reply_fd = io->call(endpoint, &message);
    UP_END(call);
    if (reply_fd < 16) status = reply_fd < 0 ? reply_fd : -EIO;
    else {
        struct pacha_ipc_fd incoming[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
        struct pacha_ipc_msg reply = { .fds = incoming, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS };
        UP_BEGIN(receive_time, UP_RPC, request->operation, UP_RECEIVE);
        status = io->receive(reply_fd, &reply);
        UP_END(receive_time);
        UP_BEGIN(validate, UP_RPC, request->operation, UP_VALIDATE);
        if (status == 0 && reply.word0 == UNIX_BUFFER_MISS_MAGIC) {
            if (token >= 2 && reply.word1 == UNIX_SERVICE_VERSION && reply.word2 == token &&
                reply.word3 == request->request && reply.fd_count == 0) *miss = 1;
            status = -EPROTO;
        }
        if (status == 0) {
            struct unix_control result;
            copy_bytes(&result, page, sizeof(result));
            if (reply.word0 != UNIX_REPLY_MAGIC || reply.word3 != request->request ||
                result.magic != UNIX_REPLY_MAGIC || result.version != UNIX_SERVICE_VERSION ||
                result.request != request->request || result.operation != request->operation ||
                result.status != (int64_t)reply.word1 || result.result != reply.word2 ||
                result.buffer_token == 1 || (token >= 2 && result.buffer_token != token) ||
                (!token && result.buffer_token) ||
                result.status > 0 || result.status < -4095 || reply.fd_count > PACHA_IPC_MAX_TRANSFER_FDS)
                status = -EPROTO;
            else if (result.status) status = (int)result.status;
            else if (reply.fd_count > capacity) status = -ENOBUFS;
            else {
                buffer->token = result.buffer_token;
                copy_bytes(request, &result, sizeof(result));
                *received = (unsigned)reply.fd_count;
                if (*received) copy_bytes(receive, incoming, *received * sizeof(*receive));
            }
        }
        if (status != 0)
            for (unsigned i = 0; i < reply.fd_count && i < PACHA_IPC_MAX_TRANSFER_FDS; i++)
                if (incoming[i].fd >= 16) io->close((int)incoming[i].fd);
        UP_END(validate);
        UP_BEGIN(close_time, UP_RPC, request->operation, UP_CLOSE);
        io->close(reply_fd);
    }
    return status;
}

int unix_client_exchange_page(const struct unix_client_io *io, int endpoint,
    struct unix_control *request, const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received,
    struct unix_client_buffer *buffer)
{
    int miss;
    int status = exchange_page_once(io, endpoint, request, send, send_count,
        receive, capacity, received, buffer, &miss);
    if (!miss) return status;
    /* Only the special, correlated no-dispatch reply permits this retry.
     * EAGAIN/ESTALE from an operation or a transport error never does. */
    buffer->token = 0;
    return exchange_page_once(io, endpoint, request, send, send_count,
        receive, capacity, received, buffer, &miss);
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
