#include "../userland/personality/linux/runtime/lpr_unix/context.c"
#include "../userland/personality/linux/runtime/lpr_process/syscalls.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

lpr_state_t lpr_state;
static int64_t tid = 1;
static unsigned opens, registrations, closes, calls, lock_depth, tid_calls;
static unsigned live[256];
static int fail_open, fail_register;
static int fail_signal_fd;

void *lpr_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void lpr_state_lock(volatile uint32_t *word) { (void)word; assert(!lock_depth++); }
void lpr_state_unlock(volatile uint32_t *word) { (void)word; assert(lock_depth-- == 1); }
void lpr_linux_signal_hint_reset_for_fork_child(void) {}
int64_t lpr_pacha_syscall0(uint64_t nr) { assert(nr == PACHAOS_SYSCALL_GETTID); tid_calls++; return tid; }
int64_t lpr_pacha_status_to_errno(int64_t status) { (void)status; return -ENOMEM; }
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t fd, uint64_t minimum, uint64_t rights, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_FD_DUP && fd == PACHAOS_THREAD_SELF_FD && minimum == 16);
    assert(rights == (PACHA_FD_RIGHT_PROCESS_SIGNAL | PACHA_FD_RIGHT_CLOSE));
    assert(flags == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
    if (fail_signal_fd) return -1;
    for (int i = 255; i >= 16; i--) if (!live[i]) { live[i] = 1; return i; }
    assert(!"thread capability table exhausted");
    return -1;
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    (void)nr; (void)a0; (void)a1;
    assert(!"no notification resolution or send during context lifecycle");
    return -1;
}
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd < 256 && live[fd]);
    live[fd] = 0; closes++; return 0;
}

int lpr_unix_client_open(struct lpr_unix_client *out)
{
    assert(!lock_depth);
    *out = (struct lpr_unix_client){ .fd = -1 };
    opens++;
    if (fail_open) return -ENOMEM;
    int fd;
    for (fd = 16; fd < 256 && live[fd]; fd++) {}
    assert(fd < 256); live[fd] = 1;
    *out = (struct lpr_unix_client){ .fd = fd, .session = 700, .process_token = lpr_supervisor_token };
    return 0;
}

void lpr_unix_client_close(struct lpr_unix_client *client)
{
    if (client->process_token && client->process_token == lpr_supervisor_token && client->fd >= 16)
        lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)client->fd);
    *client = (struct lpr_unix_client){ .fd = -1 };
}

int lpr_unix_client_register_thread(const struct lpr_unix_client *client,
    uint64_t request_id, uint64_t *out_owner)
{
    assert(!lock_depth && live[client->fd] && request_id == 1);
    registrations++;
    if (fail_register) return -EMFILE;
    *out_owner = 100 + registrations;
    return 0;
}

int lpr_unix_client_call(const struct lpr_unix_client *client, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received)
{
    assert(!lock_depth && live[client->fd] && request->request >= 2);
    assert(!send && !send_count && !receive && !capacity);
    calls++; *received = 0;
    return 0;
}

static void empty(void)
{
    for (unsigned fd = 16; fd < 256; fd++)
        if (fd != (unsigned)lpr_state.threads.main_thread.signal_fd) assert(!live[fd]);
}

int main(void)
{
    lpr_supervisor_token = 101;
    struct lpr_unix_context *first = NULL;
    assert(lpr_unix_context_current(NULL) == -EINVAL);
    fail_signal_fd = 1;
    assert(lpr_unix_context_current(&first) == -ENOMEM && !first);
    assert(!lpr_state.threads.head); empty();
    fail_signal_fd = 0;
    fail_open = 1;
    assert(lpr_unix_context_current(&first) == -ENOMEM && !first); empty();
    fail_open = 0; fail_register = 1;
    assert(lpr_unix_context_current(&first) == -EMFILE && !first); empty();
    fail_register = 0;
    assert(lpr_unix_context_current(&first) == 0);
    assert(first == &lpr_state.threads.main_thread.unix_context);
    assert(first->notifier.client == &first->client && first->waiter.client == &first->client);
    assert(first->waiter.owner && first->waiter.fd == -1 && !first->waiter.id);
    const unsigned old_opens = opens, old_registrations = registrations;
    const unsigned old_tid_calls = tid_calls;
    for (unsigned i = 0; i < 1000; i++) {
        struct lpr_unix_context *again = NULL;
        assert(lpr_unix_context_current(&again) == 0 && again == first);
    }
    assert(opens == old_opens && registrations == old_registrations && !calls);
    assert(tid_calls == old_tid_calls + 1000); /* One lookup per UNIX I/O, not two. */
    /* clone creates a separate record before its thread enters user code. */
    lpr_thread_record_t second_record = { .tid = 2, .started = 1, .parent_ready = 1 };
    lpr_thread_record_add(&second_record);
    tid = 2;
    struct lpr_unix_context *second = NULL;
    assert(lpr_unix_context_current(&second) == 0 && second == &second_record.unix_context);
    assert(first != second && first->client.fd != second->client.fd);
    assert(first->client.session == second->client.session && first->waiter.owner != second->waiter.owner);
    struct unix_control request = { .operation = UNIX_OP_SOCKETPAIR };
    unsigned received = 99;
    assert(lpr_unix_context_call(second, &request, NULL, 0, NULL, 0, &received) == 0);
    assert(request.request == 2 && !received && calls == 1 && first->request == 1);
    second->waiter.fd = 70; second->waiter.id = 8; live[70] = 1;
    second->notifier.cache.entries[0].fd = 71;
    second->notifier.cache.entries[0].notification = 9; live[71] = 1;
    unsigned before_close = closes;
    lpr_unix_context_destroy(second);
    lpr_unix_context_destroy(second);
    assert(closes == before_close + 3 && !live[70] && !live[71]);
    assert(live[first->client.fd]);
    /* The actual fork reset must never close copied private FD numbers. */
    before_close = closes;
    lpr_supervisor_token = 102;
    lpr_thread_after_fork_child();
    assert(closes == before_close && !lpr_state.threads.head && !lpr_state.threads.main_thread.unix_context.client.process_token);
    memset(live, 0, sizeof(live)); /* Native fork excluded these private caps. */
    tid = 3;
    assert(lpr_unix_context_current(&first) == 0 && first->client.process_token == 102);
    assert(first->request == 1 && first->notifier.client == &first->client);
    first->request = UINT64_MAX;
    assert(lpr_unix_context_call(first, &request, NULL, 0, NULL, 0, &received) == -EOVERFLOW && !received);
    lpr_unix_context_destroy(first);
    lpr_state_lock(&lpr_state.threads.lock_word);
    assert(lpr_thread_record_remove(&lpr_state.threads.main_thread));
    lpr_state_unlock(&lpr_state.threads.lock_word);
    assert(!lpr_state.threads.main_thread.signal_fd); empty();
    tid = -1;
    assert(lpr_unix_context_current(&first) == -LPR_LINUX_EINVAL && !first);
    assert(!lock_depth);
    puts("lpr unix context: actual thread lookup/fork reset, lazy session/owner reuse, per-thread isolation, failure and private-cap cleanup passed");
}
