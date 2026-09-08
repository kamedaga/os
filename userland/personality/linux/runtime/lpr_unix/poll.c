#include "poll.h"
#include "diagnostic.h"
#include "context.h"
#include <unixd/profile.h>
#include "../lpr_filed_internal.h"
#include <errno.h>

enum { UX_IN = 1, UX_OUT = 4, UX_ERR = 8, UX_HUP = 16, UX_RDHUP = 0x2000 };

static int64_t poll_socket(struct lpr_unix_socket *socket, uint32_t events,
    struct unix_poll_sequence *sequence)
{
    UP_BEGIN(total, UP_CONTROL, UNIX_OP_POLL, UP_TOTAL);
    int adoption = lpr_unix_socket_adopt(socket);
    if (adoption) return adoption;
    if (!__atomic_load_n(&socket->mapped, __ATOMIC_ACQUIRE)) {
        struct lpr_unix_context *context;
        int status = lpr_unix_context_current(&context);
        if (status != 0) return status;
        struct unix_control request = { .operation = UNIX_OP_POLL,
            .socket = socket->socket, .argument = events };
        unsigned count;
        status = lpr_unix_context_call(context, &request, NULL, 0, NULL, 0, &count);
        if (status == -EISCONN) status = lpr_unix_socket_map(socket);
        /* A concurrent connect may have published its mapping during RPC. */
        if (!__atomic_load_n(&socket->mapped, __ATOMIC_ACQUIRE)) {
            if (sequence && !status) *sequence = request.poll_sequence;
            return status ? status : (int64_t)request.result;
        }
    }
    const struct lpr_unix_mapping *map = &socket->mapping;
    /* Sample progress before readiness. Publication racing this sample may
     * produce another wake, but must not be acknowledged without observation. */
    if (sequence) *sequence = (struct unix_poll_sequence){
        .source = map->generation,
        .read = __atomic_load_n(&map->incoming_tx->progress, __ATOMIC_SEQ_CST) +
            !!(__atomic_load_n(&map->incoming_rx->consumed, __ATOMIC_SEQ_CST) & UNIX_TRANSPORT_CLOSED),
        .write = __atomic_load_n(&map->outgoing_rx->progress, __ATOMIC_SEQ_CST) +
            !!(__atomic_load_n(&map->outgoing_tx->published, __ATOMIC_SEQ_CST) & UNIX_TRANSPORT_CLOSED),
    };
    uint32_t result = __atomic_load_n(&socket->error, __ATOMIC_ACQUIRE) ? UX_ERR : 0;
    const int read_closed = ((__atomic_load_n(&map->incoming_tx->published, __ATOMIC_SEQ_CST) |
        __atomic_load_n(&map->incoming_rx->consumed, __ATOMIC_SEQ_CST)) & UNIX_TRANSPORT_CLOSED) != 0;
    const int write_closed = ((__atomic_load_n(&map->outgoing_tx->published, __ATOMIC_SEQ_CST) |
        __atomic_load_n(&map->outgoing_rx->consumed, __ATOMIC_SEQ_CST)) & UNIX_TRANSPORT_CLOSED) != 0;
    if (read_closed) result |= events & UX_RDHUP;
    if (read_closed && write_closed) result |= UX_HUP;
    int available = 0, eof = 0;
    if (events & UX_IN) {
        int status = unix_transport_readable(map->incoming_tx, map->incoming_rx,
            map->generation, &available, &eof);
        if (status != 0 && status != -EBUSY) return status;
        if (available) result |= UX_IN;
    }
    if (events & UX_OUT) {
        int status = unix_transport_writable(map->outgoing_tx, map->outgoing_rx,
            map->generation, &available);
        if (status != 0 && status != -EBUSY) return status;
        if (available) result |= UX_OUT;
    }
    return result;
}

int64_t lpr_unix_socket_poll(uint64_t fd, uint32_t events)
{
    return lpr_unix_socket_poll_sequence(fd, events, NULL);
}

int64_t lpr_unix_socket_poll_sequence(uint64_t fd, uint32_t events,
    struct unix_poll_sequence *sequence)
{
    lpr_fd_pin_t pin;
    if (fd > LPR_LINUX_FD_MAX || lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0)
        return -EBADF;
    int64_t result = pin.ops_id == LPR_FD_OPS_UNIX ? poll_socket(pin.state, events, sequence) : -ENOTSOCK;
    lpr_fd_unpin(&pin);
    return result;
}

struct poll_watch {
    struct poll_watch *next;
    lpr_fd_pin_t pin;
    struct unix_wait_registration registrations[2];
    uint32_t events, ignore_ready;
    struct unix_poll_sequence sequence;
    unsigned watched, mapped, armed;
};
_Static_assert(sizeof(struct poll_watch) <= 256, "poll watch fits backend slab");

static struct unix_wait_bank *bank(struct lpr_unix_mapping *map, unsigned index)
{
    return index ? &map->outgoing_tx->waiters : &map->incoming_rx->waiters;
}
static const uint64_t *changes(struct lpr_unix_mapping *map, unsigned index, unsigned second)
{
    if (index) return second ? &map->outgoing_rx->changes : &map->outgoing_tx->changes;
    return second ? &map->incoming_rx->changes : &map->incoming_tx->changes;
}

/* Active references, armed slots and pins belong to this invocation. Even an
 * interrupted or failed arm releases them. Idle control-plane registrations
 * belong to the thread's bounded waiter cache. Graph construction owns nothing. */
int64_t lpr_unix_poll_block(lpr_wait_graph_t *graph,
    const lpr_wait_deadline_t *deadline,
    int64_t (*block_native)(lpr_wait_graph_t *, const lpr_wait_deadline_t *))
{
    lpr_unix_diag('G', 0, graph->unix_count, graph->leaf_count, 0);
    struct lpr_unix_context *context;
    int64_t status = lpr_unix_context_current(&context);
    if (status != 0) return status;
    struct poll_watch *watches = NULL;
    const uint32_t leaves = graph->leaf_count;
    for (unsigned i = 0; i < graph->unix_count; i++) {
        struct poll_watch *watch = lpr_backend_state_alloc(sizeof(*watch));
        if (!watch) { status = -ENOMEM; goto done; }
        lpr_memset(watch, 0, sizeof(*watch));
        if (lpr_fd_table_pin(&lpr_control_fd_table, graph->unix_interests[i].fd, &watch->pin) != 0) {
            (void)lpr_backend_state_free(watch, sizeof(*watch));
            status = 0; goto done; /* rescan reports POLLNVAL */
        }
        watch->next = watches;
        watches = watch;
        if (watch->pin.ops_id != LPR_FD_OPS_UNIX) { status = 0; goto done; }
        struct lpr_unix_socket *socket = watch->pin.state;
        status = lpr_unix_socket_adopt(socket);
        if (status) goto done;
        watch->events = graph->unix_interests[i].events;
        watch->ignore_ready = graph->unix_interests[i].ignore_ready;
        watch->sequence = graph->unix_interests[i].sequence;
        status = lpr_unix_waiter_watch(&context->waiter, socket->socket, &socket->wait_identity);
        lpr_unix_diag('w', socket->socket, status, context->waiter.id, 0);
        if (status != 0) goto done;
        watch->watched = 1;
    }
    status = lpr_unix_waiter_drain(&context->waiter);
    lpr_unix_diag('D', 0, status, 0, 0);
    if (status != 0) { if (status == -EAGAIN) status = 0; goto done; }
    for (struct poll_watch *watch = watches; watch; watch = watch->next) {
        struct lpr_unix_socket *socket = watch->pin.state;
        watch->mapped = __atomic_load_n(&socket->mapped, __ATOMIC_ACQUIRE);
        if (!watch->mapped) continue;
        struct lpr_unix_mapping *map = &socket->mapping;
        /* Both directions carry close/shutdown notifications, even if the
         * requested mask is zero (POLLHUP is unconditional). */
        for (unsigned i = 0; i < 2; i++) {
            if (context->waiter.attempt == UINT32_MAX) { status = -EOVERFLOW; goto done; }
            status = unix_wait_arm(bank(map, i), changes(map, i, 0), changes(map, i, 1),
                context->waiter.id, ++context->waiter.attempt, &watch->registrations[i]);
            if (status != 0) goto done;
            watch->armed++;
        }
    }
    for (struct poll_watch *watch = watches; watch; watch = watch->next) {
        struct lpr_unix_socket *socket = watch->pin.state;
        struct unix_poll_sequence sequence = {0};
        status = poll_socket(socket, watch->events, &sequence);
        lpr_unix_diag('P', socket->socket, status, watch->events, watch->ignore_ready);
        if (watch->mapped) lpr_unix_diag('C', socket->socket,
            socket->mapping.incoming_tx->published, socket->mapping.incoming_rx->consumed,
            socket->mapping.incoming_rx->owner);
        if (status > 0) status &= ~(watch->ignore_ready &
            ~unix_poll_sequence_changed(&watch->sequence, &sequence));
        if (status != 0) { if (status > 0) status = 0; goto done; }
        if (watch->mapped != __atomic_load_n(&socket->mapped, __ATOMIC_ACQUIRE)) goto done;
        for (unsigned i = 0; i < watch->armed; i++) {
            if (unix_wait_changed(changes(&socket->mapping, i, 0), changes(&socket->mapping, i, 1),
                &watch->registrations[i])) goto done;
        }
    }
    status = lpr_unix_waiter_add_graph(&context->waiter, graph);
    if (status == 0) status = block_native(graph, deadline);
done:
    lpr_unix_diag('Z', 0, status, 0, 0);
    while (watches) {
        struct poll_watch *watch = watches;
        watches = watch->next;
        if (watch->watched) {
            struct lpr_unix_socket *socket = watch->pin.state;
            for (unsigned i = 0; i < watch->armed; i++)
                unix_wait_disarm(bank(&socket->mapping, i), &watch->registrations[i]);
            (void)lpr_unix_waiter_unwatch(&context->waiter, socket->socket);
        }
        lpr_fd_unpin(&watch->pin);
        (void)lpr_backend_state_free(watch, sizeof(*watch));
    }
    graph->leaf_count = leaves;
    return status;
}
