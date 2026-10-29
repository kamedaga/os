/* SPDX-License-Identifier: MIT */
#include "../userland/seed0root/src/live_console.c"
#include <assert.h>

static unsigned native_calls, closed, ticks;
static long native(uint64_t nr) {
    ++native_calls;
    if (nr==PACHA_FD_SYSCALL_CLOSE) { ++closed; return 0; }
    if (nr==PACHA_FD_SYSCALL_TIMERFD_CREATE) return 42;
    return PACHA_SYSCALL_ERR_INVALID;
}
long pacha_syscall0(uint64_t n) { return native(n); }
long pacha_syscall1(uint64_t n,uint64_t a) { (void)a;return native(n); }
long pacha_syscall2(uint64_t n,uint64_t a,uint64_t b) { (void)a;(void)b;return native(n); }
long pacha_syscall3(uint64_t n,uint64_t a,uint64_t b,uint64_t c) { (void)a;(void)b;(void)c;return native(n); }
long pacha_syscall4(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d) { (void)a;(void)b;(void)c;(void)d;return native(n); }
long pacha_syscall5(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e) { (void)a;(void)b;(void)c;(void)d;(void)e;return native(n); }
long pacha_syscall6(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return native(n); }
long pacha_ipc_registers(uint64_t n,uint64_t fd,uint64_t flags,uint64_t timeout,uint64_t words[4],struct pacha_ipc_extension *ext,int *committed) { (void)fd;(void)flags;(void)timeout;(void)words;(void)ext;(void)committed;return native(n); }
int pacha_status_to_int(long n) { return (int)n; }
pacha_fd_t pacha_fd_result(long n) { return (pacha_fd_t)n; }
static void tick(void *context) { assert(context==&ticks);++ticks; }
int main(void) {
    struct live_console console={.mmio_fd=17};
    assert(!live_console_release_framebuffer(&console));
    assert(closed==1 && !console.pixels && console.display_handed_off);
    const unsigned after_release=native_calls;
    assert(!live_console_release_framebuffer(&console));
    assert(!live_console_step(&console));
    assert(native_calls==after_release); /* No keyboard or TTY RPC after handoff. */
    assert(live_console_run(&console,tick,&ticks)==-5); /* Inject timer-wait failure after one tick. */
    assert(ticks==1 && closed==2); /* Service supervision still executes. */
    console.display_handed_off=0;
    const unsigned before_boot_step=native_calls;
    assert(live_console_step(&console)<0); /* Boot console still reports real input failures. */
    assert(native_calls>before_boot_step);
    free(console.slots);
    puts("SEED0ROOT_DISPLAY_HANDOFF_UNIT_PASS");
}
