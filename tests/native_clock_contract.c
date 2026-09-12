/* Real native clock/timer FD exercise; no host or LPR time emulation. */
#include <stdint.h>
#include <stddef.h>
#include "pacha/ipc.h"
#include "pacha/syscall.h"

static uint64_t clock_now(void)
{
    uint64_t ts[2];
    if (pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1,
        (uintptr_t)ts)) return UINT64_MAX;
    return ts[0] * 1000000000ull + ts[1];
}

static int clock_failure(unsigned line)
{
    static const char prefix[] = "NATIVE_CLOCK_CONTRACT=FAIL line=";
    pacha_syscall2(1, (uintptr_t)prefix, sizeof(prefix) - 1);
    char digits[16];
    unsigned n = 0;
    do { digits[n++] = '0' + line % 10; line /= 10; } while (line);
    for (unsigned i = 0; i < n / 2; ++i) {
        char c = digits[i]; digits[i] = digits[n - i - 1]; digits[n - i - 1] = c;
    }
    digits[n++] = '\n';
    pacha_syscall2(1, (uintptr_t)digits, n);
    return 1;
}
#define CHECK(x) do { if (!(x)) return clock_failure(__LINE__); } while (0)
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

static unsigned char clock_worker_stack[32768] __attribute__((aligned(4096)));
static volatile unsigned command, entered, done, stopping;
static uint64_t worker_started_ns, worker_returned_ns, worker_revents;
static long worker_result;
static int shared_timer_fd;

extern void clock_worker_entry(void);
__asm__(".text\n.global clock_worker_entry\n"
    "clock_worker_entry: call clock_worker_body; ud2\n");

__attribute__((used, noreturn)) static void clock_worker_body(void)
{
    unsigned previous = 0;
    while (!LOAD(stopping)) {
        const unsigned next = LOAD(command);
        if (next == previous) { __asm__ volatile("pause"); continue; }
        previous = next;
        struct pacha_pollfd poll = {
            .fd = shared_timer_fd, .events = PACHA_FD_EVENT_READABLE
        };
        worker_started_ns = clock_now();
        STORE(entered, next);
        worker_result = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)&poll, 1, 2000, 0);
        worker_returned_ns = clock_now();
        worker_revents = poll.revents;
        STORE(done, next);
    }
    pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, 0, 0, 0);
    __builtin_trap();
}

static int wait_sequence(volatile unsigned *word, unsigned sequence)
{
    const uint64_t start = clock_now();
    CHECK(start != UINT64_MAX);
    while (LOAD(*word) != sequence) {
        CHECK(clock_now() - start < 3000000000ull);
        __asm__ volatile("pause");
    }
    return 0;
}

static int yield_to_waiter(void)
{
    // Entry publication precedes WAIT_MANY. Let its native thread enter the
    // kernel and park before the owner cancels or changes the armed deadline.
    const uint64_t delay[2] = { 0, 3000000 };
    CHECK(pacha_syscall2(PACHA_RUNTIME_SYSCALL_NANOSLEEP,
        (uintptr_t)delay, 0) == 0);
    return 0;
}

static long arm(int fd, uint64_t deadline, uint64_t interval)
{
    uint64_t spec[4] = { interval / 1000000000ull, interval % 1000000000ull,
        deadline / 1000000000ull, deadline % 1000000000ull };
    return pacha_syscall4(PACHA_FD_SYSCALL_TIMERFD_SETTIME, fd,
        PACHA_TIMERFD_ABSTIME, (uintptr_t)spec, 0);
}

static int wait_once(int fd, uint64_t deadline)
{
    struct pacha_pollfd poll = { .fd = fd, .events = PACHA_FD_EVENT_READABLE };
    long result = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
        (uintptr_t)&poll, 1, 1000, 0);
    CHECK(result == 1 && poll.revents == PACHA_FD_EVENT_READABLE);
    CHECK(clock_now() >= deadline);
    uint64_t count = 0;
    CHECK(pacha_syscall3(PACHA_FD_SYSCALL_READ, fd, (uintptr_t)&count, 8) == 8);
    CHECK(count == 1);
    poll.revents = 0;
    CHECK(pacha_syscall2(PACHA_FD_SYSCALL_POLL, (uintptr_t)&poll, 1) == 0);
    return 0;
}

static int finish_wait(int fd, unsigned sequence, uint64_t deadline,
    unsigned *fine_deliveries)
{
    CHECK(wait_sequence(&done, sequence) == 0);
    CHECK(worker_result == 1 && worker_revents == PACHA_FD_EVENT_READABLE);
    CHECK(worker_returned_ns >= deadline && worker_returned_ns >= worker_started_ns);
    CHECK(clock_now() >= worker_returned_ns);
    if (worker_returned_ns < (deadline / 1000000 + 1) * 1000000)
        ++*fine_deliveries;
    uint64_t expirations = 0;
    CHECK(pacha_syscall3(PACHA_FD_SYSCALL_READ, fd,
        (uintptr_t)&expirations, sizeof(expirations)) == sizeof(expirations));
    CHECK(expirations == 1);
    struct pacha_pollfd poll = { .fd = fd, .events = PACHA_FD_EVENT_READABLE };
    CHECK(pacha_syscall2(PACHA_FD_SYSCALL_POLL, (uintptr_t)&poll, 1) == 0);
    return 0;
}

static int concurrent_deadlines(int fd)
{
    shared_timer_fd = fd;
    const uint64_t rights = PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_CLOSE;
    const long thread = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE,
        PACHA_PROCESS_SELF_FD, (uintptr_t)clock_worker_entry,
        (uintptr_t)(clock_worker_stack + sizeof(clock_worker_stack)), 0, 0, rights);
    CHECK(thread >= 16);
    CHECK(pacha_syscall1(PACHA_THREAD_SYSCALL_START, thread) == 0);
    unsigned sequence = 0, fine_deliveries = 0;

    // Unlike a poll-only cancellation test, the same WAIT_MANY remains pending
    // across cancellation, the old deadline, and rearming from another thread.
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const uint64_t old_deadline = clock_now() + 50000000;
        CHECK(arm(fd, old_deadline, 0) == 0);
        STORE(command, ++sequence);
        CHECK(wait_sequence(&entered, sequence) == 0);
        CHECK(clock_now() >= worker_started_ns);
        CHECK(yield_to_waiter() == 0);
        CHECK(clock_now() < old_deadline && LOAD(done) != sequence);
        CHECK(arm(fd, 0, 0) == 0);
        while (clock_now() <= old_deadline + 2000000)
            CHECK(LOAD(done) != sequence);
        CHECK(LOAD(done) != sequence);
        const uint64_t new_deadline = (clock_now() / 1000000 + 2) * 1000000 + 100000;
        CHECK(arm(fd, new_deadline, 0) == 0);
        CHECK(finish_wait(fd, sequence, new_deadline, &fine_deliveries) == 0);
    }

    // One real return before the next millisecond boundary excludes the old
    // rounded-tick implementation. Keep retries bounded: late scheduling is
    // tolerated, but precise timestamps alone cannot satisfy this assertion.
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        CHECK(arm(fd, clock_now() + 1000000000, 0) == 0);
        STORE(command, ++sequence);
        CHECK(wait_sequence(&entered, sequence) == 0);
        CHECK(yield_to_waiter() == 0);
        CHECK(LOAD(done) != sequence);
        const uint64_t deadline = (clock_now() / 1000000 + 2) * 1000000 + 100000;
        CHECK(arm(fd, deadline, 0) == 0);
        CHECK(finish_wait(fd, sequence, deadline, &fine_deliveries) == 0);
    }
    CHECK(fine_deliveries != 0);
    STORE(stopping, 1);
    uint64_t status[4];
    const uint64_t end = clock_now() + 2000000000;
    long result;
    do {
        result = pacha_syscall2(PACHA_THREAD_SYSCALL_WAIT, thread, (uintptr_t)status);
        CHECK(clock_now() < end);
    } while (result == PACHA_SYSCALL_ERR_NOT_READY);
    CHECK(result == 0 && status[0] == 2 && status[1] == 0);
    CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, thread) == 0);
    return 0;
}

int native_clock_contract(void)
{
    uint64_t resolution[2];
    CHECK(pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETRES, 1,
        (uintptr_t)resolution) == 0);
    CHECK(resolution[0] == 0 && resolution[1] != 0 && resolution[1] < 1000000);
    uint64_t start = clock_now(), previous = start;
    int fractional = 0;
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t next = clock_now();
        CHECK(next != UINT64_MAX && next >= previous);
        fractional |= next % 1000000 != 0;
        previous = next;
    }
    CHECK(previous > start && fractional);
    const uint64_t rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_CLOSE;
    long fd = pacha_syscall6(PACHA_FD_SYSCALL_TIMERFD_CREATE,
        PACHA_TIMERFD_CLOCK_MONOTONIC, 0, 0, 0, rights, 0);
    CHECK(fd >= 16);
    CHECK(concurrent_deadlines((int)fd) == 0);
    for (unsigned i = 0; i < 8; ++i) {
        // Force a non-tick-aligned deadline; cancel and rearm the same FD.
        uint64_t deadline = clock_now() + 1234567;
        CHECK(arm(fd, deadline, 0) == 0);
        CHECK(arm(fd, 0, 0) == 0);
        struct pacha_pollfd poll = { .fd = (int)fd, .events = PACHA_FD_EVENT_READABLE };
        CHECK(pacha_syscall2(PACHA_FD_SYSCALL_POLL, (uintptr_t)&poll, 1) == 0);
        deadline = clock_now() + 2345678;
        CHECK(arm(fd, deadline, 0) == 0);
        CHECK(wait_once(fd, deadline) == 0);
    }
    // GETTIME keeps the exact interval instead of silently rounding to ms.
    CHECK(arm(fd, clock_now() + 100000000, 123457) == 0);
    uint64_t remaining[4];
    CHECK(pacha_syscall2(PACHA_FD_SYSCALL_TIMERFD_GETTIME, fd, (uintptr_t)remaining) == 0);
    CHECK(remaining[0] == 0 && remaining[1] == 123457);
    CHECK(remaining[2] == 0 && remaining[3] > 0 && remaining[3] <= 100000000);
    CHECK(arm(fd, 0, 0) == 0);
    CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, fd) == 0);
    return 0;
}
