/* SPDX-License-Identifier: MIT */
/* Empty native thread roundtrip, without Linux or per-transfer probes. */
#include <stdint.h>
#include <stddef.h>
#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
static unsigned permit[2], stop_peer;
static unsigned char stacks[2][32768] __attribute__((aligned(4096)));
static struct { uint64_t rounds, ticks, elapsed; } result[3];

static void line(const char *s) {
    size_t n = 0;
    while (s[n]) ++n;
    (void)pacha_syscall2(1, (uintptr_t)s, n);
}
static void number(uint64_t n) {
    char buffer[24]; unsigned used = 0;
    do { buffer[used++] = '0' + n % 10; n /= 10; } while (n);
    for (unsigned i = 0; i < used / 2; ++i) {
        char c = buffer[i]; buffer[i] = buffer[used - 1 - i]; buffer[used - 1 - i] = c;
    }
    buffer[used] = 0; line(buffer);
}
__attribute__((noreturn)) static void finish(unsigned status) {
    if (status) { line("NATIVE_HANDOFF=FAIL code="); number(status); line("\n"); }
    (void)pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, status, 0, 0);
    __builtin_trap();
}
static uint64_t now(void) {
    uint64_t ns;
    if (pacha_clock_read(1, &ns)) finish(1);
    return ns;
}
static uint64_t ticks(void) {
    unsigned lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
static void await(unsigned self) {
    while (!LOAD(permit[self])) {
        long rc = pacha_syscall3(PACHA_RUNTIME_SYSCALL_FUTEX_WAIT,
            (uintptr_t)&permit[self], 0, 1000);
        if (rc && rc != PACHA_SYSCALL_ERR_NOT_READY) finish(2);
    }
}
static void transfer(unsigned self) {
    STORE(permit[self], 0);
    STORE(permit[1 - self], 1);
    uint64_t count;
    long rc = pacha_futex_wake_wait((uintptr_t)&permit[1 - self],
        (uintptr_t)&permit[self], 0, 1000, &count);
    /* A rejected inbound wait may already have published the outbound wake.
     * Only wait for our permit again; never replay the compound operation. */
    if (count > 1 || (rc && rc != PACHA_SYSCALL_ERR_NOT_READY)) finish(3);
    await(self);
}
__attribute__((used, noreturn)) static void peer(void) {
    await(1);
    while (!LOAD(stop_peer)) transfer(1);
    finish(0);
}
__attribute__((used, noreturn)) static void source(void) {
    for (unsigned trial = 0; trial < 3; ++trial) {
        for (unsigned i = 0; i < 32; ++i) transfer(0);
        uint64_t start = now(), begin = ticks(), elapsed, rounds = 0;
        do {
            for (unsigned i = 0; i < 128; ++i) transfer(0);
            rounds += 128;
            elapsed = now() - start;
        } while (elapsed < 1000000000ull);
        result[trial].ticks = ticks() - begin;
        result[trial].elapsed = elapsed;
        result[trial].rounds = rounds;
    }
    /* Retire only after the final return to source. The peer is parked or
     * about to check its permit, so release publication plus wake cannot lose
     * this stop, and no final inbound permit is needed by source. */
    STORE(stop_peer, 1);
    STORE(permit[1], 1);
    if ((unsigned long)pacha_syscall2(PACHA_RUNTIME_SYSCALL_FUTEX_WAKE,
        (uintptr_t)&permit[1], 1) > 1)
        finish(4);
    finish(0);
}
extern void native_handoff_source(void), native_handoff_peer(void);
__asm__(".text\n.global native_handoff_source,native_handoff_peer\n"
    "native_handoff_source: call source; ud2\n"
    "native_handoff_peer: call peer; ud2\n");

static int join(uint64_t fd) {
    struct pacha_pollfd p = { .fd = fd, .events = PACHA_FD_EVENT_READABLE };
    for (;;) {
        p.revents = 0;
        long rc = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)&p, 1, PACHA_FD_WAIT_FOREVER, 0);
        if (rc == 1 && p.revents == PACHA_FD_EVENT_READABLE) break;
        if (rc != PACHA_SYSCALL_ERR_NOT_READY || p.revents) return 1;
    }
    uint64_t status[4];
    if (pacha_syscall2(PACHA_THREAD_SYSCALL_WAIT, fd, (uintptr_t)status) ||
        status[0] != 2 || status[1]) return 1;
    return pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, fd) != 0;
}
int main(void) {
    uint64_t fd[2];
    long rc = 0;
    unsigned step = 0;
    STORE(permit[0], 1);
    for (unsigned i = 0; i < 2; ++i) {
        long created = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE,
            PACHA_PROCESS_SELF_FD,
            (uintptr_t)(i ? native_handoff_peer : native_handoff_source),
            (uintptr_t)(stacks[i] + sizeof(stacks[i])), 0, 0,
            PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_POLL);
        step = 1 + i; rc = created;
        if (created < 16) goto fail;
        fd[i] = created;
    }
    step = 3; rc = pacha_syscall1(PACHA_THREAD_SYSCALL_START, fd[1]);
    if (rc) goto fail;
    step = 4; rc = pacha_syscall1(PACHA_THREAD_SYSCALL_START, fd[0]);
    if (rc) goto fail;
    /* The coordinator blocks, rather than polling and competing with the
     * timed pair. Stacks and result storage stay live through both joins. */
    step = 5; rc = join(fd[0]); if (rc) goto fail;
    step = 6; rc = join(fd[1]); if (rc) goto fail;
    for (unsigned i = 0; i < 3; ++i) {
        line("NATIVE_HANDOFF trial="); number(i + 1);
        line(" rounds="); number(result[i].rounds);
        line(" ticks="); number(result[i].ticks);
        line(" elapsed_ns="); number(result[i].elapsed); line("\n");
    }
    line("NATIVE_HANDOFF=PASS\n");
    return 0;
fail:
    line("NATIVE_HANDOFF=FAIL coordinator step="); number(step);
    line(" rc="); number(rc); line("\n");
    return 1;
}
