#include "../userland/personality/linux/runtime/lpr_unix/notify.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lpr_state_t lpr_state;
static struct { unsigned live; uint32_t notification; } caps[256];
static unsigned resolves, sends, relays, closes, logs;
static int resolve_status, relay_status, bad_cap;
static int64_t send_status;

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd < 256 && caps[fd].live);
    caps[fd].live = 0; closes++; return 0;
}

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t address)
{
    if (nr == PACHAOS_SYSCALL_LOG) {
        assert(address == 109 && memcmp((void *)(uintptr_t)fd, "[unix] ", 7) == 0);
        ++logs;
        return 0;
    }
    assert(fd < 256 && caps[fd].live);
    if (nr == PACHAOS_SYSCALL_FD_GET_INFO) {
        struct pacha_fd_info *info = (struct pacha_fd_info *)(uintptr_t)address;
        *info = (struct pacha_fd_info){ .kind = bad_cap == 3 ? PACHA_FD_KIND_VMO : PACHA_FD_KIND_CHANNEL,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND |
                (bad_cap == 1 ? PACHA_FD_RIGHT_DUP : 0),
            .flags = PACHA_FD_FLAG_PRIVATE | (bad_cap == 2 ? 0 : PACHA_FD_FLAG_CLOEXEC) };
        return 0;
    }
    assert(nr == PACHAOS_SYSCALL_IPC_SEND);
    const struct pacha_ipc_msg *message = (const struct pacha_ipc_msg *)(uintptr_t)address;
    assert(message->word0 == UNIX_NOTIFY_MAGIC && !message->fd_count);
    assert(message->word1 == caps[fd].notification && message->word2 >= 8 && message->word2 <= 11);
    sends++; return send_status;
}

int lpr_unix_client_call(const struct lpr_unix_client *client, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received)
{
    assert(client->process_token == 101 && request->socket == 10 && request->request);
    assert(!send && !send_count);
    *received = 0;
    if (request->operation == UNIX_OP_WAIT_NOTIFY) {
        assert(!receive && !capacity);
        relays++; return relay_status;
    }
    assert(request->operation == UNIX_OP_WAIT_SYNC && receive && capacity == 1);
    resolves++;
    if (request->argument == 99) return -EPERM;
    if (resolve_status) return resolve_status;
    unsigned fd;
    for (fd = 16; fd < 256 && caps[fd].live; fd++) {}
    assert(fd < 256);
    caps[fd].live = 1; caps[fd].notification = (uint32_t)request->argument;
    *receive = (struct pacha_ipc_fd){ .fd = fd };
    *received = 1; request->result = request->argument;
    return 0;
}

static void empty_caps(void)
{
    for (unsigned fd = 16; fd < 256; fd++) assert(!caps[fd].live);
}

int main(void)
{
    struct unix_tx *tx = calloc(1, sizeof(*tx));
    struct unix_rx *rx = calloc(1, sizeof(*rx));
    assert(tx && rx && unix_transport_init(tx, rx, 7, UNIX_TRANSPORT_STREAM) == 0);
    struct unix_wait_registration first, second, forged;
    assert(unix_wait_arm(&tx->waiters, &tx->changes, &rx->changes, 1, 8, &first) == 0);
    assert(unix_wait_arm(&rx->waiters, &tx->changes, &rx->changes, 2, 9, &second) == 0);
    lpr_supervisor_token = 101;
    const struct lpr_unix_client client = { .fd = 200, .session = 1, .process_token = 101 };
    struct lpr_unix_notifier notifier;
    lpr_unix_notifier_init(&notifier, &client);
    assert(lpr_unix_notifier_signal(&notifier, 10, tx, rx) == 0);
    assert(resolves == 2 && sends == 2 && !relays);
    assert(lpr_unix_notifier_signal(&notifier, 10, tx, rx) == 0);
    assert(resolves == 2 && sends == 2 && !relays); /* same arm: no native SEND */
    unix_wait_disarm(&tx->waiters, &first);
    unix_wait_disarm(&rx->waiters, &second);
    assert(unix_wait_arm(&tx->waiters, &tx->changes, &rx->changes, 1, 10, &first) == 0);
    assert(unix_wait_arm(&rx->waiters, &tx->changes, &rx->changes, 2, 11, &second) == 0);
    send_status = PACHA_SYSCALL_ERR_NOT_READY;
    assert(lpr_unix_notifier_signal(&notifier, 10, tx, rx) == 0);
    assert(resolves == 2 && !relays && !closes); /* full queue already has a wake */
    send_status = -PACHA_SYSCALL_ERR_NOT_READY;
    assert(lpr_unix_notifier_signal(&notifier, 10, tx, rx) == 0);
    assert(resolves == 2 && !relays && !closes && !logs);
    send_status = PACHA_SYSCALL_ERR_ALLOC;
    assert(lpr_unix_notifier_signal(&notifier, 10, tx, rx) == 0);
    assert(relays == 2 && closes == 2); /* allocation failure is not a queued wake */
    empty_caps();
    send_status = 0; resolve_status = -EMFILE;
    assert(lpr_unix_notifier_signal(&notifier, 10, tx, rx) == 0);
    assert(relays == 4); empty_caps();
    relay_status = -EPIPE;
    assert(lpr_unix_notifier_signal(&notifier, 10, tx, rx) == -EPIPE);
    assert(relays == 6); empty_caps();
    resolve_status = relay_status = 0;
    for (bad_cap = 1; bad_cap <= 3; bad_cap++) {
        assert(lpr_unix_notifier_signal(&notifier, 10, tx, rx) == 0);
        empty_caps(); /* unsafe cap rejected and closed, notification relayed */
    }
    assert(relays == 12 && closes == 8);
    bad_cap = 0;
    assert(lpr_unix_notifier_signal(&notifier, 10, tx, rx) == 0);
    const unsigned known_relays = relays;
    assert(unix_wait_arm(&tx->waiters, &tx->changes, &rx->changes, 99, 10, &forged) == 0);
    assert(lpr_unix_notifier_signal(&notifier, 10, tx, rx) == 0);
    assert(relays == known_relays); /* forged ID is not forwarded through relay */
    unix_wait_disarm(&tx->waiters, &forged);
    struct lpr_unix_notifier copied = notifier;
    lpr_supervisor_token = 102;
    const unsigned old_closes = closes, old_sends = sends;
    assert(lpr_unix_notifier_signal(&copied, 10, tx, rx) == -ENOTCONN);
    lpr_unix_notifier_destroy(&copied);
    assert(closes == old_closes && sends == old_sends);
    lpr_supervisor_token = 101;
    lpr_unix_notifier_destroy(&notifier); empty_caps();
    lpr_unix_notifier_init(&notifier, &client);
    notifier.request = UINT64_MAX;
    assert(lpr_unix_notifier_signal(&notifier, 10, tx, rx) == -EOVERFLOW);
    lpr_unix_notifier_destroy(&notifier); empty_caps();
    free(tx); free(rx);
    puts("lpr unix notify: warm SEND, NOT_READY queue-full vs ALLOC, FD-pressure relay, cap validation, forged ID, fork cleanup passed");
}
