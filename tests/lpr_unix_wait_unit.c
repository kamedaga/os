#include "../userland/personality/linux/runtime/lpr_unix/wait.c"
/* Exercise the actual graph construction; only the blocking syscall layer
 * is replaced below. Guest wait scheduling is a separate acceptance gate. */
#define lpr_wait_graph_block unused_production_block
#include "../userland/personality/linux/runtime/lpr_wait.c"
#undef lpr_wait_graph_block
#include <assert.h>
#include <stdio.h>
#include <string.h>

lpr_state_t lpr_state;
static unsigned live, closes, calls, queued, drains, blocks, ready_calls;
static uint64_t identities[1024];
static unsigned registered[1024], removals;
static int bad_cap, fail_rpc, bad_result, peer_hup, broker_hup, bad_poll;
static int lose_remove_reply;
static int mode;
static int64_t block_status;
static struct unix_wait_bank bank;
static uint64_t changes[2];
static lpr_wait_deadline_t deadline = { .finite = 1, .expires_ns = 123456 };

void *lpr_memset(void *p, int value, size_t bytes) { return memset(p, value, bytes); }

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd == 40 && live);
    live = 0; closes++; return 0;
}

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    if (nr == PACHAOS_SYSCALL_LOG) {
        const char *line = (void *)(uintptr_t)a0;
        assert(a1 == 109 && !memcmp(line, "[unix] ", 7) && line[a1 - 1] == '\n');
        return 0;
    }
    if (nr == PACHAOS_SYSCALL_FD_GET_INFO) {
        assert(a0 == 40 && live);
        *(struct pacha_fd_info *)(uintptr_t)a1 = (struct pacha_fd_info){
            .kind = bad_cap == 3 ? PACHA_FD_KIND_VMO : PACHA_FD_KIND_CHANNEL,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_RECV |
                PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | (bad_cap == 1 ? PACHA_FD_RIGHT_DUP : 0),
            .flags = PACHA_FD_FLAG_PRIVATE | (bad_cap == 2 ? 0 : PACHA_FD_FLAG_CLOEXEC) };
        return 0;
    }
    if (nr == PACHAOS_SYSCALL_FD_POLL) {
        assert(a1 == 2 && live);
        struct pacha_pollfd *leaves = (void *)(uintptr_t)a0;
        assert(leaves[0].fd == 80 && leaves[1].fd == 40);
        assert(leaves[0].events == PACHA_FD_EVENT_HANGUP && leaves[1].events == PACHA_FD_EVENT_HANGUP);
        if (bad_poll) return PACHA_SYSCALL_ERR_INVALID;
        leaves[0].revents = broker_hup ? PACHA_FD_EVENT_HANGUP : 0;
        leaves[1].revents = peer_hup ? PACHA_FD_EVENT_HANGUP : 0;
        return (broker_hup != 0) + (peer_hup != 0);
    }
    assert(nr == PACHAOS_SYSCALL_IPC_RECV && a0 == 40 && live);
    struct pacha_ipc_msg *message = (void *)(uintptr_t)a1;
    assert(!message->fd_capacity);
    drains++;
    if (!queued) return PACHA_SYSCALL_ERR_EMPTY;
    queued--;
    message->word0 = 999; /* Even forged/stale hints cannot skip state recheck. */
    return 0;
}

int lpr_unix_client_call(const struct lpr_unix_client *client, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received)
{
    assert(client->process_token == 101 && request->request && !send && !send_count);
    calls++; *received = 0;
    if (fail_rpc) return -ENOMEM;
    if (request->operation == UNIX_OP_WAIT_REMOVE) {
        assert(request->argument == 7 && request->socket && !receive && !capacity);
        registered[request->socket] = 0;
        removals++;
        if (lose_remove_reply) return -EIO;
        return 0;
    }
    assert(request->operation == UNIX_OP_WAIT_REGISTER && request->socket && request->transaction == 77);
    if (!request->argument) {
        assert(!live && capacity == 1 && receive);
        *receive = (struct pacha_ipc_fd){ .fd = 40 };
        *received = 1; live = 1;
    } else assert(live && request->argument == 7 && !capacity);
    request->result = bad_result ? UINT64_MAX : 7;
    registered[request->socket] = 1;
    return 0;
}

int64_t lpr_wait_graph_block(lpr_wait_graph_t *graph, const lpr_wait_deadline_t *until)
{
    assert(until == &deadline && until->expires_ns == 123456);
    assert(graph->leaf_count == 2 && graph->leaves[0].fd == 40 && graph->leaves[1].fd == 80);
    assert(graph->leaves[0].events == (PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP));
    assert(graph->leaves[1].events == PACHA_FD_EVENT_HANGUP);
    assert(!graph->drain_modes[0] && !graph->drain_modes[1]);
    assert((uint32_t)bank.armed[0] == 7 && (uint32_t)(bank.armed[0] >> 32));
    blocks++;
    if (mode == 4) broker_hup = 1; /* SEND caps may keep notification peer alive. */
    return block_status;
}

static int ready(void *context)
{
    assert(context == &bank);
    ready_calls++;
    if (mode == 1) return 1;
    if (mode == 2 && ready_calls == 2) return 1;
    if (mode == 3 && ready_calls == 2) __atomic_add_fetch(&changes[1], 1, __ATOMIC_SEQ_CST);
    if (mode == 5 && ready_calls == 2) return -ECONNRESET;
    return 0;
}

static int64_t attempt(struct lpr_unix_waiter *waiter)
{
    ready_calls = 0;
    return lpr_unix_waiter_wait(waiter, &bank, &changes[0], &changes[1], ready, &bank, &deadline);
}

static void disarmed(void)
{
    for (unsigned i = 0; i < UNIX_WAIT_SLOTS; i++) assert(!bank.armed[i]);
}

int main(void)
{
    uint64_t fresh_identity = 0, identity_value = 0;
    next_identity = UINT64_MAX;
    assert(socket_identity(&fresh_identity, &identity_value) == -EOVERFLOW && !fresh_identity);
    next_identity = 0;
    lpr_supervisor_token = 101;
    struct lpr_unix_client client = { .fd = 80, .session = 1, .process_token = 101 };
    struct lpr_unix_waiter waiter;
    lpr_unix_waiter_init(&waiter, &client, 77);
    fail_rpc = 1;
    assert(lpr_unix_waiter_watch(&waiter, 10, &identities[10]) == -ENOMEM && !live);
    fail_rpc = 0;
    for (bad_cap = 1; bad_cap <= 3; bad_cap++)
        assert(lpr_unix_waiter_watch(&waiter, 10, &identities[10]) == -EPROTO && !live && !waiter.id);
    bad_cap = 0; bad_result = 1;
    assert(lpr_unix_waiter_watch(&waiter, 10, &identities[10]) == -EPROTO && !live);
    bad_result = 0;
    assert(lpr_unix_waiter_watch(&waiter, 10, &identities[10]) == 0 && live && waiter.id == 7);
    assert(lpr_unix_waiter_watch(&waiter, 11, &identities[11]) == 0 && waiter.fd == 40);
    assert(lpr_unix_waiter_unwatch(&waiter, 11) == 0 && live);
    unsigned warm_calls = calls;
    for (unsigned i = 0; i < 1000; i++) {
        assert(lpr_unix_waiter_watch(&waiter, 11, &identities[11]) == 0);
        assert(lpr_unix_waiter_unwatch(&waiter, 11) == 0);
    }
    assert(calls == warm_calls && !removals);
    /* Close/prune followed by SCM_RIGHTS reimport of the SAME broker ID. */
    uint64_t old_identity = identities[11];
    registered[11] = 0;
    identities[11] = 0;
    assert(lpr_unix_waiter_watch(&waiter, 11, &identities[11]) == 0);
    assert(registered[11] && identities[11] != old_identity && calls == warm_calls + 1);
    assert(lpr_unix_waiter_unwatch(&waiter, 11) == 0);
    /* A repeated descriptor in poll holds another reference to one watch. */
    assert(lpr_unix_waiter_watch(&waiter, 10, &identities[10]) == 0);
    assert(lpr_unix_waiter_unwatch(&waiter, 10) == 0);
    /* Evict idle entries without removing an active socket's registration. */
    for (unsigned i = 20; i < 20 + LPR_WAIT_GRAPH_MAX_LEAVES * 2; i++) {
        assert(lpr_unix_waiter_watch(&waiter, i, &identities[i]) == 0);
        assert(lpr_unix_waiter_unwatch(&waiter, i) == 0);
        assert(registered[10]);
    }
    assert(removals);
    unsigned victim_slot = waiter.next;
    while (waiter.watches[victim_slot].references)
        victim_slot = (victim_slot + 1) % LPR_WAIT_GRAPH_MAX_LEAVES;
    unsigned victim = (unsigned)waiter.watches[victim_slot].socket;
    waiter.next = victim_slot;
    lose_remove_reply = 1;
    assert(lpr_unix_waiter_watch(&waiter, 800, &identities[800]) == -EIO);
    assert(!registered[victim]);
    lose_remove_reply = 0;
    warm_calls = calls;
    assert(lpr_unix_waiter_watch(&waiter, victim, &identities[victim]) == 0);
    assert(registered[victim] && calls == warm_calls + 1);
    assert(lpr_unix_waiter_unwatch(&waiter, victim) == 0);
    for (unsigned i = 600; i < 600 + LPR_WAIT_GRAPH_MAX_LEAVES - 1; i++)
        assert(lpr_unix_waiter_watch(&waiter, i, &identities[i]) == 0);
    warm_calls = calls;
    assert(lpr_unix_waiter_watch(&waiter, 900, &identities[900]) == -EAGAIN);
    assert(calls == warm_calls && registered[10]);
    assert(lpr_unix_waiter_watch(&waiter, 600, &identities[600]) == 0);
    assert(lpr_unix_waiter_unwatch(&waiter, 600) == 0);
    assert(lpr_unix_waiter_watch(&waiter, 900, &identities[900]) == -EAGAIN);
    assert(lpr_unix_waiter_unwatch(&waiter, 600) == 0);
    assert(lpr_unix_waiter_watch(&waiter, 900, &identities[900]) == 0);
    assert(!registered[600]);
    for (unsigned i = 601; i < 600 + LPR_WAIT_GRAPH_MAX_LEAVES - 1; i++)
        assert(lpr_unix_waiter_unwatch(&waiter, i) == 0);
    assert(lpr_unix_waiter_unwatch(&waiter, 900) == 0);
    queued = 2;
    assert(lpr_unix_waiter_drain(&waiter) == 0 && !queued && drains == 3);
    bad_poll = 1;
    assert(lpr_unix_waiter_drain(&waiter) == -EIO);
    bad_poll = 0; peer_hup = 1;
    assert(lpr_unix_waiter_drain(&waiter) == -EPIPE);
    peer_hup = 0; broker_hup = 1;
    assert(lpr_unix_waiter_drain(&waiter) == -EPIPE);
    broker_hup = 0;
    queued = 300;
    assert(attempt(&waiter) == 0 && queued == 44 && !blocks); disarmed();
    queued = 0;
    mode = 1;
    unsigned before_drains = drains;
    assert(attempt(&waiter) == 0 && drains == before_drains && !blocks);
    for (mode = 1; mode <= 3; mode++) {
        assert(attempt(&waiter) == 0 && !blocks); disarmed();
    }
    mode = 5;
    assert(attempt(&waiter) == -ECONNRESET && !blocks); disarmed();
    mode = 0;
    assert(attempt(&waiter) == 0 && blocks == 1); disarmed();
    block_status = LPR_WAIT_RESTART_SYSCALL;
    assert(attempt(&waiter) == LPR_WAIT_RESTART_SYSCALL && blocks == 2); disarmed();
    block_status = -EINTR;
    assert(attempt(&waiter) == -EINTR && blocks == 3); disarmed();
    block_status = 0; mode = 4;
    assert(attempt(&waiter) == -EPIPE && blocks == 4); disarmed();
    broker_hup = 0; mode = 0;
    for (unsigned i = 0; i < UNIX_WAIT_SLOTS; i++) bank.armed[i] = UINT64_MAX;
    assert(attempt(&waiter) == -EAGAIN && blocks == 4);
    for (unsigned i = 0; i < UNIX_WAIT_SLOTS; i++) assert(bank.armed[i] == UINT64_MAX);
    memset(&bank, 0, sizeof(bank));
    waiter.attempt = UINT32_MAX;
    assert(attempt(&waiter) == -EOVERFLOW && blocks == 4); disarmed();
    lpr_wait_graph_t graph;
    lpr_wait_graph_init(&graph);
    assert(lpr_unix_waiter_add_graph(&waiter, &graph) == 0);
    assert(lpr_unix_waiter_add_graph(&waiter, &graph) == 0 && graph.leaf_count == 2);
    struct lpr_unix_waiter copied = waiter;
    unsigned old_calls = calls, old_closes = closes;
    lpr_supervisor_token = 102;
    assert(lpr_unix_waiter_watch(&copied, 10, &identities[10]) == -ENOTCONN);
    assert(lpr_unix_waiter_drain(&copied) == -ENOTCONN);
    lpr_unix_waiter_destroy(&copied);
    assert(live && calls == old_calls && closes == old_closes);
    lpr_supervisor_token = 101;
    waiter.request = UINT64_MAX;
    assert(lpr_unix_waiter_watch(&waiter, 901, &identities[901]) == -EOVERFLOW);
    client.process_token = 0; /* Cleanup does not require a live control client. */
    lpr_unix_waiter_destroy(&waiter);
    lpr_unix_waiter_destroy(&waiter);
    assert(!live && closes == old_closes + 1);
    puts("lpr unix wait: cap validation, own drain, bounded flood, arm/recheck, real graph leaves, signal cleanup, broker HUP, fork cleanup passed");
}
