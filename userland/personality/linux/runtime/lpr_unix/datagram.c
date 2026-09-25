#include "socket.h"
#include "cache.h"
#include <unixd/profile.h>
#include "../lpr_filed_internal.h"
#include <errno.h>

enum { UX_NONBLOCK = 0x800, UX_PEEK = 2, UX_TRUNC = 0x20,
    UX_DONTWAIT = 0x40, UX_NOSIGNAL = 0x4000 };

static int call(struct lpr_unix_context *context, struct unix_control *request)
{
    unsigned count;
    return lpr_unix_context_call(context, request, NULL, 0, NULL, 0, &count);
}

static int unlocked(void *opaque)
{
    return __atomic_load_n((const uint64_t *)opaque, __ATOMIC_SEQ_CST) == 0;
}

/* The daemon serializes delivery metadata/FIFO, never payload. A route's TX
 * is writable only by its source; RX only by its destination. In particular,
 * receiving does not parse any length/ticket supplied by a writable header. */
int64_t lpr_unix_dgram_io(const lpr_fd_pin_t *pin, const lpr_linux_iovec_t *vectors,
    unsigned count, uint64_t length, int writing, uint64_t flags,
    uint32_t *message_flags, struct lpr_unix_ancillary *ancillary)
{
    struct lpr_unix_socket *socket = pin->state;
    UP_BEGIN(total, UP_IO, socket->type * 2 + writing, UP_TOTAL);
    UP_BEGIN(setup, UP_IO, socket->type * 2 + writing, UP_SETUP);
    int64_t result = lpr_unix_socket_adopt(socket);
    if (result) return result;
    if (writing && length > UNIX_TRANSPORT_BYTES - sizeof(struct unix_record)) return -EMSGSIZE;
    struct lpr_unix_context *context;
    result = lpr_unix_context_current(&context);
    if (result) return result;
    struct lpr_unix_ancillary empty = {0};
    if (!ancillary) ancillary = &empty;
    struct lpr_unix_cache_lease lease;
    uint64_t generation = 0;
    int watched = 0;
    lpr_wait_deadline_t deadline;
    uint64_t timeout = __atomic_load_n(writing ? &socket->send_timeout_ns : &socket->receive_timeout_ns, __ATOMIC_ACQUIRE);
    result = timeout ? lpr_wait_deadline_init_ns(&deadline, timeout) : lpr_wait_deadline_init(&deadline, -1);
    if (result) return result;
    result = lpr_unix_cache_begin(&lease, &context->client, socket->socket, writing, NULL);
    if (result) return result;
    struct lpr_unix_route_mapping *mapping = &lease.mapping;
    UP_END(setup);
    if (writing) {
        UP_BEGIN(route_time, UP_IO, socket->type * 2 + writing, UP_ROUTE);
        struct unix_control route = { .operation = UNIX_OP_DGRAM_ROUTE,
            .socket = socket->socket, .cached_generation = lease.generation };
        lpr_memcpy(&route.address, &ancillary->address, sizeof(route.address));
        if (route.address.kind == UNIX_ADDRESS_PATH) {
            lpr_cwd_init();
            route.argument = route.address.bytes[0] == '/' ? 0 : lpr_cwd_handle;
        }
        struct pacha_ipc_fd caps[2];
        unsigned received;
        result = lpr_unix_context_call(context, &route, NULL, 0, caps, 2, &received);
        UP_END(route_time);
        if (result) goto done;
        generation = route.argument;
        UP_BEGIN(map_time, UP_IO, socket->type * 2 + writing, UP_MAP);
        result = lpr_unix_cache_import(&lease, caps, received, generation);
        UP_END(map_time);
        if (result) goto done;
        ancillary->route = route.result;
        result = lpr_unix_rights_prepare(context, socket->socket, ancillary);
        if (result) goto done;
    }
    for (;;) {
        if (watched) {
            result = lpr_unix_waiter_drain(&context->waiter);
            if (result && result != -EAGAIN) break;
        }
        int released = 0;
        if (writing) {
            struct unix_write write;
            UP_BEGIN(reserve, UP_IO, socket->type * 2 + writing, UP_RESERVE);
            result = unix_transport_write_begin(mapping->tx, mapping->rx, generation,
                context->waiter.owner, length, ancillary->ticket, ancillary->operation, &write);
            UP_END(reserve);
            if (!result) {
                const struct unix_const_span spans[2] = {
                    {write.spans[0].base, write.spans[0].length},
                    {write.spans[1].base, write.spans[1].length} };
                UP_BEGIN(copy, UP_IO, socket->type * 2 + writing, UP_COPY);
                lpr_unix_copy_iov(vectors, count, spans, 1);
                UP_END(copy);
                UP_BEGIN(commit_time, UP_IO, socket->type * 2 + writing, UP_COMMIT);
                struct unix_control commit = { .operation = UNIX_OP_DGRAM_COMMIT,
                    .socket = socket->socket, .argument = ancillary->route,
                    .io = { .owner = write.owner, .before = write.before,
                        .after = write.after, .length = write.length } };
                result = ancillary->ticket ? lpr_unix_rights_commit(context, socket->socket, ancillary, &write) :
                    call(context, &commit);
                if (result) unix_transport_write_cancel(mapping->tx, &write);
                else result = write.length;
                released = 1;
            }
        } else {
            UP_BEGIN(route_time, UP_IO, socket->type * 2 + writing, UP_ROUTE);
            struct unix_control head = { .operation = UNIX_OP_DGRAM_HEAD,
                .socket = socket->socket, .cached_generation = lease.generation };
            struct pacha_ipc_fd caps[2];
            unsigned received;
            result = lpr_unix_context_call(context, &head, NULL, 0, caps, 2, &received);
            UP_END(route_time);
            if (!result && !head.result && !received) {
                uint32_t socket_flags;
                result = lpr_unix_socket_flags(pin, &socket_flags, 0);
                if (!result && ((flags & UX_DONTWAIT) || (socket_flags & UX_NONBLOCK))) result = -EAGAIN;
                break;
            }
            if (!result) {
                generation = head.delivery.generation;
                ancillary->passcred = (head.transaction & UNIX_SOCKET_PASSCRED) != 0;
                UP_BEGIN(map_time, UP_IO, socket->type * 2 + writing, UP_MAP);
                result = lpr_unix_cache_import(&lease, caps, received, generation);
                UP_END(map_time);
                if (result) break;
                struct unix_read read;
                UP_BEGIN(reserve, UP_IO, socket->type * 2 + writing, UP_RESERVE);
                result = unix_transport_packet_begin(mapping->tx, mapping->rx, generation,
                    context->waiter.owner, head.delivery.position, head.delivery.length,
                    head.delivery.ticket, head.delivery.operation, length, &read);
                UP_END(reserve);
                if (!result) {
                    UP_BEGIN(copy, UP_IO, socket->type * 2 + writing, UP_COPY);
                    lpr_unix_copy_iov(vectors, count, read.spans, 0);
                    UP_END(copy);
                    UP_BEGIN(commit_time, UP_IO, socket->type * 2 + writing, UP_COMMIT);
                    struct unix_control consume = { .operation = UNIX_OP_DGRAM_CONSUME,
                        .socket = socket->socket, .argument = head.delivery.id,
                        .transaction = flags & UX_PEEK,
                        .io = { .owner = read.owner, .before = read.before, .after = read.after,
                            .length = read.length, .message_length = read.message_length } };
                    result = read.ticket ? lpr_unix_rights_receive(context, socket->socket,
                        &read, ancillary, flags, message_flags) : call(context, &consume);
                    if (result) unix_transport_read_cancel(mapping->rx, &read);
                    else {
                        lpr_memcpy(&ancillary->address, &head.delivery.source, sizeof(ancillary->address));
                        if (!read.ticket)
                            lpr_unix_credentials_output(ancillary, &head.delivery.credentials, message_flags);
                        if (message_flags && read.truncated) *message_flags |= UX_TRUNC;
                        result = (flags & UX_TRUNC) ? read.message_length : read.length;
                    }
                    released = 1;
                }
            }
        }
        if (released) {
            UP_BEGIN(notify_time, UP_IO, socket->type * 2 + writing, UP_NOTIFY);
            int status = lpr_unix_notifier_signal(&context->notifier, socket->socket, mapping->tx, mapping->rx);
            if (status) __atomic_store_n(&socket->error, -status, __ATOMIC_RELEASE);
        }
        /* Another reader or connect(AF_UNSPEC)/reconnect can invalidate HEAD
         * both before RX acquisition and after copying. Failed rights imports
         * have already been rolled back; never publish the discarded packet. */
        if (!writing && result == -ESTALE) continue;
        if (result != -EAGAIN && result != -EBUSY) break;
        const int busy = result == -EBUSY;
        uint32_t socket_flags;
        int status = lpr_unix_socket_flags(pin, &socket_flags, 0);
        if (status) { result = status; break; }
        if ((flags & UX_DONTWAIT) || (socket_flags & UX_NONBLOCK)) { result = -EAGAIN; break; }
        int expired;
        result = lpr_wait_deadline_expired(&deadline, &expired);
        if (result || expired) { if (expired) result = -EAGAIN; break; }
        if (!watched) {
            UP_BEGIN(watch, UP_IO, socket->type * 2 + writing, UP_WATCH);
            result = lpr_unix_waiter_watch(&context->waiter, socket->socket, &socket->wait_identity);
            if (result) break;
            watched = 1;
            continue;
        }
        UP_BEGIN(wait_time, UP_IO, socket->type * 2 + writing, UP_WAIT);
        if (busy) {
            /* Payload lock release can happen without a broker commit (PEEK
             * or cancel), so use the same arm/recheck protocol as STREAM. */
            result = lpr_unix_waiter_wait(&context->waiter,
                writing ? &mapping->tx->waiters : &mapping->rx->waiters,
                &mapping->tx->changes, &mapping->rx->changes, unlocked,
                writing ? (void *)&mapping->tx->owner : (void *)&mapping->rx->owner, &deadline);
        } else {
            /* The broker notifies every source when destination budget opens.
             * Nothing drains the notification queue after this attempt. */
            lpr_wait_graph_t graph;
            lpr_wait_graph_init(&graph);
            result = lpr_unix_waiter_add_graph(&context->waiter, &graph);
            if (!result) result = lpr_wait_graph_block(&graph, &deadline);
        }
        if (result) break;
    }
done:
    ;
    UP_BEGIN(cleanup, UP_IO, socket->type * 2 + writing, UP_CLEANUP);
    if (watched) (void)lpr_unix_waiter_unwatch(&context->waiter, socket->socket);
    if (writing && result < 0) lpr_unix_rights_cancel(context, ancillary);
    lpr_unix_cache_end(&lease, result >= 0 || result == -EAGAIN);
    /* Linux AF_UNIX DGRAM reports EPIPE without generating SIGPIPE. */
    return result;
}
