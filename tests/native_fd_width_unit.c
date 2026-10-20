#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <pacha/ipc.h>
#include <pacha/capsule.h>
#include <pacha/syscall.h>
#include <pacha/bootstrap.h>

static long result;
static uint64_t seen_fd, seen_nr;
static int committed_value = 1;
long __wrap_pacha_syscall0(uint64_t nr) { seen_nr = nr; return result; }
long __wrap_pacha_syscall1(uint64_t nr, uint64_t a)
{ (void)nr; seen_fd = a; return result; }
long __wrap_pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b)
{ (void)nr; (void)b; seen_fd = a; return result; }
long __wrap_pacha_syscall3(uint64_t nr, uint64_t a, uint64_t b, uint64_t c)
{
    (void)b; (void)c; seen_fd = a;
    if (nr == PACHA_IPC_SYSCALL_CHANNEL_CREATE) {
        uint64_t *pair = (void *)(uintptr_t)a;
        pair[0] = 0x80000000u; pair[1] = 0xfffffffdu;
        return 0;
    }
    return result;
}
long __wrap_pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{ (void)nr; (void)b; (void)c; (void)d; seen_fd = a; return result; }
long __wrap_pacha_syscall5(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{ (void)nr; (void)b; (void)c; (void)d; (void)e; seen_fd = a; return result; }
long __wrap_pacha_syscall6(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{ (void)nr; (void)b; (void)c; (void)d; (void)e; (void)f; seen_fd = a; return result; }

int main(void)
{
    _Static_assert(sizeof(struct pacha_pollfd) == 32, "kernel poll item width");
    _Static_assert(offsetof(struct pacha_pollfd, events) == 8, "kernel events offset");
    const uint64_t values[] = {4097, 0x7fffffff, 0x80000000, 0xfffffffd};
    for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        char *argv_aux[] = {0, 0, (char *)(uintptr_t)PACHA_AT_BOOTSTRAP_FD,
            (char *)(uintptr_t)values[i], 0, 0};
        assert(pacha_bootstrap_fd_from_argv(argv_aux) == (pacha_fd_t)values[i]);
        result = (long)values[i];
        assert(pacha_fd_result(result) == result);
        assert(pacha_capsule_is_fd(result));
        assert(pacha_ipc_endpoint_create(0, 0) == result);
        assert(pacha_vmo_create(4096, 0, 0) == result);
        assert(pacha_capsule_pci_claim(0) == result);
        struct pacha_ipc_msg msg = {0};
        result = 0;
        int sent = 0;
        assert(!pacha_ipc_call((pacha_fd_t)values[i], &msg, &msg, UINT64_MAX, &sent) && sent);
        assert(seen_fd == values[i]);
        result = 0;
        assert(!pacha_fd_close((pacha_fd_t)values[i]) && seen_fd == values[i]);
        assert(!pacha_capsule_close((pacha_fd_t)values[i]) && seen_fd == values[i]);
        struct pacha_fd_info info;
        assert(!pacha_fd_get_info((pacha_fd_t)values[i], &info) && seen_fd == values[i]);
        struct pacha_pollfd poll = {.fd = (pacha_fd_t)values[i]};
        assert((uint64_t)poll.fd == values[i]);
    }
    struct pacha_ipc_msg request = {0}, reply = {0};
    int committed = 0;
    result = PACHA_SYSCALL_ERR_NOT_READY;
    assert(pacha_ipc_call(42, &request, &reply, 1, &committed) == PACHA_ERR_NOT_READY);
    assert(committed && seen_nr == PACHA_IPC_SYSCALL_CALL);
    committed_value = 0;
    assert(pacha_ipc_call(42, &request, &reply, 1, &committed) == PACHA_ERR_NOT_READY);
    assert(!committed);
    committed_value = 1;
    assert(pacha_ipc_reply_recv(42, &request, &reply, 1, &committed) == PACHA_ERR_NOT_READY);
    assert(committed && seen_nr == PACHA_IPC_SYSCALL_REPLY_RECV);
    result = INT64_C(0xfffffffd);
    assert(pacha_ipc_reply_extract() == result && seen_nr == PACHA_IPC_SYSCALL_REPLY_EXTRACT);
    result = 0;
    assert(!pacha_ipc_reply(PACHA_IPC_REPLY_IMPLICIT, &reply) && seen_fd == UINT64_MAX);
    struct pacha_ipc_channel_pair pair;
    assert(!pacha_ipc_channel_create(&pair, 0, 0));
    assert(pair.a == INT64_C(0x80000000) && pair.b == INT64_C(0xfffffffd));
    const int64_t invalid[] = {-1, 0, 7, 15, 0xfffffffeLL, 0xffffffffLL, 0x100000000LL};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        char *argv_aux[] = {0, 0, (char *)(uintptr_t)PACHA_AT_BOOTSTRAP_FD,
            (char *)(uintptr_t)invalid[i], 0, 0};
        assert(pacha_bootstrap_fd_from_argv(argv_aux) == -1);
        assert(pacha_fd_result(invalid[i]) < 0);
        assert(!pacha_capsule_is_fd(invalid[i]));
    }
    for (long status = 1; status <= 6; status++) assert(pacha_fd_result(status) == -status);
    result = 0;
    /* A wide invalid input must reach the kernel intact, never alias FD 16. */
    (void)pacha_fd_close(INT64_C(0x100000010));
    assert(seen_fd == UINT64_C(0x100000010));
    puts("native FD carriers: high u32, errno separation, reserved values, poll wire layout, no input truncation PASS");
}



long __wrap_pacha_ipc_registers(uint64_t nr, uint64_t fd, uint64_t flags,
    uint64_t timeout, uint64_t words[4], struct pacha_ipc_extension *extension, int *committed) {
    (void)flags; (void)timeout; (void)extension;
    seen_nr=nr; seen_fd=fd;
    words[0]=words[1]=words[2]=words[3]=0;
    if(committed) *committed=committed_value;
    return result;
}
