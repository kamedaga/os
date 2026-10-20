#include "../userland/personality/linux/runtime/lpr_fd/control.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
lpr_state_t lpr_state;
static uint64_t expected;
static unsigned inspected, closed;
void *lpr_memset(void *p, int v, size_t n) { return memset(p, v, n); }
int64_t lpr_pacha_syscall2(uint64_t n, uint64_t fd, uint64_t address)
{ assert(n == PACHAOS_SYSCALL_FD_GET_INFO && fd == expected && address); ++inspected; return 0; }
int64_t lpr_pacha_syscall1(uint64_t n, uint64_t fd)
{ assert(n == PACHAOS_SYSCALL_FD_CLOSE && fd == expected); ++closed; return 0; }
int64_t lpr_pacha_status_to_errno(int64_t s) { return s; }
int main(void)
{
    const uint64_t good[] = {16, INT32_MAX, UINT64_C(0x80000000), UINT64_C(0xfffffffd)};
    struct pacha_fd_info info;
    for (unsigned i = 0; i < sizeof(good)/sizeof(good[0]); ++i) {
        expected = good[i]; inspected = closed = 0;
        assert(lpr_native_fd_info(expected, &info));
        assert(!lpr_close_native_fd_if_open(expected));
        assert(inspected == 2 && closed == 1);
    }
    const uint64_t bad[] = {0, 15, PACHAOS_THREAD_SELF_FD, UINT32_MAX, UINT64_MAX};
    inspected = closed = 0;
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        assert(!lpr_native_fd_info(bad[i], &info));
        assert(!lpr_close_native_fd_if_open(bad[i]));
    }
    assert(!inspected && !closed);
    puts("native capability inspect/close: full u32 range excluding reserved values PASS");
}

#include "lpr_ipc_mock.h"
int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send, uint64_t receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    return test_lpr_ipc_noncall(nr, fd, send, receive, timeout, flags, committed);
}

int64_t lpr_pacha_syscall4(uint64_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    (void)nr;(void)a;(void)b;(void)c;(void)d;abort();
}
