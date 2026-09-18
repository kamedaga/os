#include "socket.h"
#include "cache.h"
#include <unixd/profile.h>
#include "diagnostic.h"
#include "../lpr_filed_internal.h"
#include <errno.h>

enum { UX_NONBLOCK = 0x800, UX_CLOEXEC = 0x80000,
    UX_PEEK = 2, UX_TRUNC = 0x20, UX_DONTWAIT = 0x40, UX_NOSIGNAL = 0x4000,
    UX_CMSG_CLOEXEC = 0x40000000 };
_Static_assert(sizeof(struct lpr_unix_socket) <= 256, "unix socket backend slab");

static int close_socket(struct lpr_unix_context *context, uint64_t socket)
{
    struct unix_control request = { .operation = UNIX_OP_CLOSE, .socket = socket };
    unsigned count = 0;
    return lpr_unix_context_call(context, &request, NULL, 0, NULL, 0, &count);
}

int lpr_unix_socket_active(uint64_t fd)
{
    lpr_fd_arrays_init();
    if (fd > LPR_LINUX_FD_MAX) return 0;
    lpr_fd_pin_t pin;
    if (lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0) return 0;
    const int active = pin.ops_id == LPR_FD_OPS_UNIX;
    lpr_fd_unpin(&pin);
    return active;
}

int64_t lpr_unix_socket_close(void *state)
{
    UP_BEGIN(total, UP_CONTROL, UNIX_OP_CLOSE, UP_TOTAL);
    struct lpr_unix_socket *socket = state;
    lpr_unix_cache_forget_socket(socket->socket);
    lpr_unix_mapping_destroy(&socket->mapping);
    if (socket->handoff_fd >= 16) {
        (void)lpr_close_native_fd_if_open((uint32_t)socket->handoff_fd);
        socket->handoff_fd = -1;
    }
    if (!socket->socket || socket->process_token != lpr_supervisor_token) return 0;
    struct lpr_unix_context *context;
    int status = lpr_unix_context_current(&context);
    if (status == 0) status = close_socket(context, socket->socket);
    socket->socket = 0;
    return status;
}

static int64_t create(uint64_t type_flags, uint64_t protocol, int pair, uint64_t output)
{
    UP_BEGIN(total, UP_CONTROL, pair ? UNIX_OP_SOCKETPAIR : UNIX_OP_SOCKET, UP_TOTAL);
    const uint32_t type = (uint32_t)(type_flags & ~(uint64_t)(UX_NONBLOCK | UX_CLOEXEC));
    if (type_flags > UINT32_MAX || (type != UNIX_TRANSPORT_STREAM &&
        type != UNIX_TRANSPORT_SEQPACKET && type != UNIX_TRANSPORT_DGRAM)) return -ESOCKTNOSUPPORT;
    if (protocol) return -EPROTONOSUPPORT;
    if (pair && !lpr_user_range_plausible(output, 2 * sizeof(int32_t))) return -EFAULT;
    struct lpr_unix_context *context;
    int status = lpr_unix_context_current(&context);
    if (status != 0) return status;
    struct unix_control request = { .operation = pair ? UNIX_OP_SOCKETPAIR : UNIX_OP_SOCKET,
        .argument = type, .transaction = type_flags & UX_NONBLOCK };
    unsigned received = 0;
    status = lpr_unix_context_call(context, &request, NULL, 0, NULL, 0, &received);
    if (status != 0) return status;
    uint64_t ids[2] = { request.result, pair ? request.argument : 0 };
    const unsigned count = pair ? 2 : 1;
    struct lpr_unix_socket *states[2] = {0};
    lpr_fd_install_t installs[2] = {{0}};
    lpr_linux_fd_t fds[2];
    if (!ids[0] || (pair && (!ids[1] || ids[0] == ids[1]))) { status = -EPROTO; goto fail; }
    for (unsigned i = 0; i < count; i++) {
        states[i] = lpr_backend_state_alloc(sizeof(*states[i]));
        if (!states[i]) { status = -ENOMEM; goto fail; }
        *states[i] = (struct lpr_unix_socket){ .socket = ids[i], .type = type,
            .process_token = context->client.process_token,
            .flags = LPR_LINUX_O_RDWR | (uint32_t)(type_flags & UX_NONBLOCK),
            .mapping = { .fds = {-1, -1} } };
        if (pair && type != UNIX_TRANSPORT_DGRAM) {
            uint64_t request_id;
            status = lpr_unix_context_next_request(context, &request_id);
            if (status == 0) status = lpr_unix_mapping_attach(&context->client, ids[i], type,
                request_id, &states[i]->mapping);
            if (status != 0) goto fail;
            states[i]->mapped = 1;
        }
        installs[i] = (lpr_fd_install_t){ .ops_id = LPR_FD_OPS_UNIX,
            .fd_flags = type_flags & UX_CLOEXEC ? LPR_FD_ENTRY_CLOEXEC : 0,
            .access_mode = LPR_LINUX_O_RDWR,
            .status_flags = type_flags & UX_NONBLOCK ? LPR_OFD_NONBLOCK : 0,
            .rights = LPR_FD_RIGHT_READ | LPR_FD_RIGHT_WRITE | LPR_FD_RIGHT_DUP |
                LPR_FD_RIGHT_STAT | LPR_FD_RIGHT_IOCTL,
            .backend_state = states[i], .backend_state_bytes = sizeof(*states[i]) };
    }
    lpr_fd_arrays_init();
    while (lpr_fd_table_alloc_batch(&lpr_control_fd_table, 0, installs, count, NULL, 0, fds) != 0) {
        const uint64_t capacity = lpr_fd_table_capacity;
        if (capacity >= LPR_FD_TABLE_MAX_SIZE || lpr_fd_table_ensure_capacity(capacity + count) != 0) {
            status = -EMFILE; goto fail;
        }
    }
    if (!pair) return fds[0];
    ((int32_t *)(uintptr_t)output)[0] = (int32_t)fds[0];
    ((int32_t *)(uintptr_t)output)[1] = (int32_t)fds[1];
    return 0;
fail:
    for (unsigned i = 0; i < count; i++) {
        if (states[i]) {
            lpr_unix_mapping_destroy(&states[i]->mapping);
            (void)lpr_backend_state_free(states[i], sizeof(*states[i]));
        }
        if (ids[i] && (i == 0 || ids[i] != ids[0])) (void)close_socket(context, ids[i]);
    }
    return status;
}

int64_t lpr_unix_socket_create(uint64_t type, uint64_t protocol) { return create(type, protocol, 0, 0); }
int64_t lpr_unix_socket_pair(uint64_t type, uint64_t protocol, uint64_t output) { return create(type, protocol, 1, output); }

struct readiness { struct lpr_unix_socket *socket; int writing; size_t length; };
static int ready(void *opaque)
{
    struct readiness *state = opaque;
    struct lpr_unix_mapping *map = &state->socket->mapping;
    int result = 0, eof = 0;
    if (__atomic_load_n(state->writing ? &map->outgoing_tx->owner : &map->incoming_rx->owner,
        __ATOMIC_SEQ_CST)) return 0;
    const int status = state->writing ? unix_transport_writable_for(map->outgoing_tx,
        map->outgoing_rx, map->generation, state->length, &result) :
        unix_transport_readable(map->incoming_tx, map->incoming_rx, map->generation, &result, &eof);
    return status == -EBUSY ? 0 : status != 0 ? status : result;
}

static void copy_iov_at(const lpr_linux_iovec_t *vectors, unsigned count,
    const struct unix_const_span spans[2], int writing, uint64_t offset)
{
    unsigned vector = 0;
    while (vector < count && offset >= vectors[vector].len) {
        offset -= vectors[vector].len;
        vector++;
    }
    for (unsigned i = 0; i < 2; i++) {
        unsigned char *shared = (void *)spans[i].base;
        size_t left = spans[i].length;
        while (left) {
            while (vector < count && offset == vectors[vector].len) { vector++; offset = 0; }
            if (vector == count) return; /* validated total bounds */
            size_t bytes = vectors[vector].len - offset;
            if (bytes > left) bytes = left;
            void *user = (void *)(uintptr_t)(vectors[vector].base + offset);
            if (writing) lpr_memcpy(shared, user, bytes);
            else lpr_memcpy(user, shared, bytes);
            shared += bytes; left -= bytes; offset += bytes;
        }
    }
}

void lpr_unix_copy_iov(const lpr_linux_iovec_t *vectors, unsigned count,
    const struct unix_const_span spans[2], int writing)
{
    copy_iov_at(vectors, count, spans, writing, 0);
}

static int64_t io(const lpr_fd_pin_t *pin, const lpr_linux_iovec_t *vectors,
    unsigned count, uint64_t length, int writing, uint64_t flags, uint32_t *message_flags,
    struct lpr_unix_ancillary *ancillary)
{
    if (!pin || pin->ops_id != LPR_FD_OPS_UNIX || !pin->state) return -EBADF;
    if (!(pin->effective_rights & (writing ? LPR_FD_RIGHT_WRITE : LPR_FD_RIGHT_READ))) return -EBADF;
    if (flags & ~(uint64_t)(UX_DONTWAIT | UX_NOSIGNAL | (writing ? 0 : UX_PEEK | UX_TRUNC | UX_CMSG_CLOEXEC)))
        return -EOPNOTSUPP;
    struct lpr_unix_socket *socket = pin->state;
    if (socket->type == UNIX_TRANSPORT_DGRAM)
        return lpr_unix_dgram_io(pin, vectors, count, length, writing, flags, message_flags, ancillary);
    UP_BEGIN(total, UP_IO, socket->type * 2 + writing, UP_TOTAL);
    UP_BEGIN(setup, UP_IO, socket->type * 2 + writing, UP_SETUP);
    int mapped_status = lpr_unix_socket_map(socket);
    if (mapped_status) { lpr_unix_diag('E', socket->socket, mapped_status, 1, writing); return mapped_status; }
    if (!length && socket->type == UNIX_TRANSPORT_STREAM) return 0;
    struct lpr_unix_context *context;
    int status = lpr_unix_context_current(&context);
    if (status != 0) { lpr_unix_diag('E', socket->socket, status, 2, writing); return status; }
    struct lpr_unix_mapping *map = &socket->mapping;
    struct lpr_unix_ancillary empty = {0};
    if (!ancillary) ancillary = &empty;
    if (writing) ancillary->automatic_credentials =
        ((__atomic_load_n(&map->incoming_rx->options, __ATOMIC_ACQUIRE) |
          __atomic_load_n(&map->outgoing_rx->options, __ATOMIC_ACQUIRE)) & UNIX_SOCKET_PASSCRED) != 0;
    else ancillary->passcred =
        (__atomic_load_n(&map->incoming_rx->options, __ATOMIC_ACQUIRE) & UNIX_SOCKET_PASSCRED) != 0;
    lpr_wait_deadline_t deadline;
    const uint64_t timeout = __atomic_load_n(writing ? &socket->send_timeout_ns : &socket->receive_timeout_ns, __ATOMIC_ACQUIRE);
    status = (int)(timeout ? lpr_wait_deadline_init_ns(&deadline, timeout) : lpr_wait_deadline_init(&deadline, -1));
    if (status != 0) return status;
    int watched = 0;
    if (writing) {
        status = lpr_unix_rights_prepare(context, socket->socket, ancillary);
        if (status) return status;
    }
    UP_END(setup);
    int64_t result;
    for (;;) {
        if (writing) {
            struct unix_write write;
            UP_BEGIN(reserve, UP_IO, socket->type * 2 + writing, UP_RESERVE);
            status = unix_transport_write_begin(map->outgoing_tx, map->outgoing_rx, map->generation,
                context->waiter.owner, length, ancillary ? ancillary->ticket : 0,
                ancillary ? ancillary->operation : 0, &write);
            UP_END(reserve);
            if (status == 0) {
                const struct unix_const_span spans[2] = {
                    {write.spans[0].base, write.spans[0].length}, {write.spans[1].base, write.spans[1].length} };
                UP_BEGIN(copy, UP_IO, socket->type * 2 + writing, UP_COPY);
                lpr_unix_copy_iov(vectors, count, spans, 1);
                UP_END(copy);
                UP_BEGIN(commit, UP_IO, socket->type * 2 + writing, UP_COMMIT);
                status = ancillary && ancillary->ticket ?
                    lpr_unix_rights_commit(context, socket->socket, ancillary, &write) :
                    unix_transport_write_commit(map->outgoing_tx, &write);
                if (status) unix_transport_write_cancel(map->outgoing_tx, &write);
                result = status ? status : (int64_t)write.length;
            } else result = status;
        } else {
            struct unix_read read;
            UP_BEGIN(reserve, UP_IO, socket->type * 2 + writing, UP_RESERVE);
            status = unix_transport_read_begin(map->incoming_tx, map->incoming_rx, map->generation,
                context->waiter.owner, length, &read);
            UP_END(reserve);
            if (status == 0) {
                {
                    UP_BEGIN(copy, UP_IO, socket->type * 2 + writing, UP_COPY);
                    uint64_t received = 0;
                    do {
                        copy_iov_at(vectors, count, read.spans, 0, received);
                        received += read.length;
                    } while (socket->type == UNIX_TRANSPORT_STREAM && received < length &&
                        unix_transport_read_next(map->incoming_tx, map->incoming_rx,
                            map->generation, length - received, &read) == 1);
                    UP_END(copy);
                    UP_BEGIN(commit, UP_IO, socket->type * 2 + writing, UP_COMMIT);
                    if (read.ticket) {
                        status = lpr_unix_rights_receive(context, socket->socket, &read,
                            ancillary, flags, message_flags);
                        if (status) unix_transport_read_cancel(map->incoming_rx, &read);
                    } else if (flags & UX_PEEK) unix_transport_read_cancel(map->incoming_rx, &read);
                    else status = unix_transport_read_commit(map->incoming_rx, &read);
                    if (!status && !read.ticket && !read.eof)
                        lpr_unix_credentials_output(ancillary, NULL, message_flags);
                    if (status == 0 && message_flags && read.truncated) *message_flags |= UX_TRUNC;
                    result = status ? status : (int64_t)((flags & UX_TRUNC) &&
                        socket->type != UNIX_TRANSPORT_STREAM ? read.message_length : received);
                }
            } else result = status;
        }
        if (status != -EBUSY) {
            UP_BEGIN(notify_time, UP_IO, socket->type * 2 + writing, UP_NOTIFY);
            const int notify = lpr_unix_notifier_signal(&context->notifier, socket->socket,
                writing ? map->outgoing_tx : map->incoming_tx,
                writing ? map->outgoing_rx : map->incoming_rx);
            if (notify != 0) {
                lpr_unix_diag('N', socket->socket, notify, writing, 0);
                __atomic_store_n(&socket->error, -notify, __ATOMIC_RELEASE);
                if (result < 0) result = notify;
            }
        }
        if (result != -EAGAIN && result != -EBUSY) break;
        if ((__atomic_load_n(&map->outgoing_tx->flags, __ATOMIC_ACQUIRE) & UX_NONBLOCK) ||
            (flags & UX_DONTWAIT)) { result = -EAGAIN; break; }
        int expired = 0;
        result = lpr_wait_deadline_expired(&deadline, &expired);
        if (result != 0 || expired) { if (expired) result = -EAGAIN; break; }
        if (!watched) {
            UP_BEGIN(watch, UP_IO, socket->type * 2 + writing, UP_WATCH);
            result = lpr_unix_waiter_watch(&context->waiter, socket->socket, &socket->wait_identity);
            if (result != 0) break;
            watched = 1;
        }
        struct readiness state = { socket, writing, length };
        UP_BEGIN(wait_time, UP_IO, socket->type * 2 + writing, UP_WAIT);
        result = lpr_unix_waiter_wait(&context->waiter,
            writing ? &map->outgoing_tx->waiters : &map->incoming_rx->waiters,
            writing ? &map->outgoing_tx->changes : &map->incoming_tx->changes,
            writing ? &map->outgoing_rx->changes : &map->incoming_rx->changes,
            ready, &state, &deadline);
        if (result != 0) break;
    }
    UP_BEGIN(cleanup, UP_IO, socket->type * 2 + writing, UP_CLEANUP);
    if (watched) (void)lpr_unix_waiter_unwatch(&context->waiter, socket->socket);
    if (writing && result < 0) lpr_unix_rights_cancel(context, ancillary);
    if (writing && result == -EPIPE && !(flags & UX_NOSIGNAL)) lpr_linux_raise_sigpipe();
    lpr_unix_diag(writing ? 'W' : 'R', socket->socket, result,
        map->incoming_tx->published, map->incoming_rx->consumed);
    return result;
}

int64_t lpr_unix_socket_io(const lpr_fd_pin_t *pin, uint64_t buffer, uint64_t length,
    int writing, uint64_t flags)
{
    if (length > INT64_MAX || (length && !lpr_user_range_plausible(buffer, length))) return -EFAULT;
    const lpr_linux_iovec_t vector = { .base = buffer, .len = length };
    return io(pin, &vector, 1, length, writing, flags, NULL, NULL);
}

int64_t lpr_unix_socket_iov(const lpr_fd_pin_t *pin, uint64_t raw, uint64_t count,
    int writing, uint64_t flags, uint32_t *message_flags)
{
    return lpr_unix_socket_message_iov(pin, raw, count, writing, flags, message_flags, NULL);
}

int64_t lpr_unix_socket_message_iov(const lpr_fd_pin_t *pin, uint64_t raw, uint64_t count,
    int writing, uint64_t flags, uint32_t *message_flags, struct lpr_unix_ancillary *ancillary)
{
    if (count > 1024) return -EMSGSIZE;
    if (count && !lpr_user_range_plausible(raw, count * sizeof(lpr_linux_iovec_t))) return -EFAULT;
    lpr_linux_iovec_t vectors[1024];
    if (count) lpr_memcpy(vectors, (const void *)(uintptr_t)raw, count * sizeof(*vectors));
    uint64_t length = 0;
    for (unsigned i = 0; i < count; i++) {
        if (vectors[i].len > INT64_MAX - length) return -EINVAL;
        if (vectors[i].len && !lpr_user_range_plausible(vectors[i].base, vectors[i].len)) return -EFAULT;
        length += vectors[i].len;
    }
    return io(pin, vectors, (unsigned)count, length, writing, flags, message_flags, ancillary);
}

int64_t lpr_unix_socket_sendrecv(uint64_t fd, uint64_t buffer, uint64_t length,
    int writing, uint64_t flags)
{
    if (fd > LPR_LINUX_FD_MAX) return -EBADF;
    lpr_fd_pin_t pin;
    if (lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0) return -EBADF;
    int64_t result = lpr_unix_socket_io(&pin, buffer, length, writing, flags);
    lpr_fd_unpin(&pin);
    return result;
}

int64_t lpr_unix_socket_stat(const lpr_fd_pin_t *pin, uint64_t output)
{
    if (!lpr_user_range_plausible(output, sizeof(lpr_linux_stat_t))) return -EFAULT;
    lpr_linux_stat_t *st = (void *)(uintptr_t)output;
    lpr_memset(st, 0, sizeof(*st));
    st->st_ino = ((const struct lpr_unix_socket *)pin->state)->socket;
    st->st_mode = LPR_LINUX_S_IFSOCK | 0777u; st->st_nlink = 1; st->st_blksize = 4096;
    return 0;
}

int64_t lpr_unix_socket_shutdown(uint64_t fd, uint64_t how)
{
    if (how > 2 || fd > LPR_LINUX_FD_MAX) return -EINVAL;
    lpr_fd_pin_t pin;
    if (lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0) return -EBADF;
    struct lpr_unix_context *context;
    int status = pin.ops_id != LPR_FD_OPS_UNIX ? -ENOTSOCK :
        lpr_unix_socket_adopt(pin.state);
    if (!status) status = lpr_unix_context_current(&context);
    if (status == 0) {
        struct unix_control request = { .operation = UNIX_OP_SHUTDOWN,
            .socket = ((struct lpr_unix_socket *)pin.state)->socket, .argument = how };
        unsigned count;
        status = lpr_unix_context_call(context, &request, NULL, 0, NULL, 0, &count);
    }
    lpr_fd_unpin(&pin);
    return status;
}
