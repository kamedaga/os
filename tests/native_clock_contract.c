/* Real native clock/timer FD exercise; no host or LPR time emulation. */
#include <stdint.h>
#include <stddef.h>
#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include "pacha/abi.h"
#include "../userland/kobox2_adapter/clock_callbacks.h"

static uint64_t clock_now(void)
{
    uint64_t value;
    if (pacha_clock_read(1, &value)) return UINT64_MAX;
    return value;
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

#define CLOCK_STRING_1(x) #x
#define CLOCK_STRING(x) CLOCK_STRING_1(x)
static volatile unsigned clock_signal_seen;
static uint64_t clock_signal_status, clock_signal_value, clock_signal_rip;
extern unsigned char clock_signal_begin[], clock_signal_resume[];
extern unsigned char __start_clock_handlers[], __stop_clock_handlers[];
extern unsigned char __start_native_handlers[], __stop_native_handlers[];
extern void notification_entry(void);
extern void clock_signal_entry(void);
extern long clock_pending_value(int self, uint64_t *value);

__attribute__((used, noinline, section("clock_handlers")))
static void clock_signal_body(struct pacha_native_signal_frame *frame)
{
    clock_signal_status = frame->rax;
    clock_signal_value = frame->rdx;
    clock_signal_rip = frame->rip;
    STORE(clock_signal_seen, frame->signo);
    __asm__ volatile("pxor %%xmm15, %%xmm15" ::: "xmm15");
}

__asm__(".pushsection clock_handlers,\"ax\",@progbits\n"
    ".global clock_signal_entry\n"
    "clock_signal_entry: mov %rdi,%r12; call clock_signal_body; mov %r12,%rsi\n"
    "mov $3,%edi; mov $" CLOCK_STRING(PACHA_PROCESS_SYSCALL_SIGNAL_CTL) ",%eax; syscall; ud2\n"
    ".popsection\n.text\n.global clock_pending_value\n"
    "clock_pending_value: push %rbx; mov %rsi,%rbx\n"
    ".global clock_signal_begin\nclock_signal_begin:\n"
    "mov $12,%esi; mov $" CLOCK_STRING(PACHA_THREAD_SYSCALL_SIGNAL) ",%eax; syscall\n"
    "test %rax,%rax; jnz 1f\n"
    "mov $1,%edi; mov $" CLOCK_STRING(PACHA_RUNTIME_SYSCALL_CLOCK_READ) ",%eax; syscall\n"
    ".global clock_signal_resume\nclock_signal_resume:\n"
    "mov %rdx,(%rbx)\n1: pop %rbx; ret\n");

static int clock_register_contract(void)
{
    uint64_t value = 123;
    CHECK(pacha_clock_read(99, &value) == PACHA_SYSCALL_ERR_INVALID && value == 0);
    for (unsigned id = 0; id <= 1; ++id) {
        for (unsigned i = 0; i < 128; ++i) {
            uint64_t before[2], after[2];
            CHECK(pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, id, (uintptr_t)before) == 0);
            CHECK(pacha_clock_read(id, &value) == 0);
            CHECK(pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, id, (uintptr_t)after) == 0);
            CHECK(value >= before[0] * 1000000000ull + before[1]);
            CHECK(value <= after[0] * 1000000000ull + after[1]);
        }
    }
    const uint64_t rights = PACHA_FD_RIGHT_PROCESS_SIGNAL | PACHA_FD_RIGHT_CLOSE;
    long self = pacha_syscall4(PACHA_FD_SYSCALL_DUP, PACHA_THREAD_SELF_FD, 16, rights, 0);
    CHECK(self >= 16);
    CHECK(pacha_syscall6(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER,
        (uintptr_t)&(struct pacha_signal_registration){
            .entry = (uintptr_t)clock_signal_entry,
            .inhibit_start = (uintptr_t)__start_clock_handlers,
            .inhibit_end = (uintptr_t)__stop_clock_handlers,
            .inhibit_secondary_start = (uintptr_t)clock_signal_begin,
            .inhibit_secondary_end = (uintptr_t)clock_signal_resume,
            .scratch_base = 0,
            .scratch_size = 0,
            .flags = PACHA_SIGNAL_REGISTRATION_PROCESS_OWNER,
        }, 0, 0, 0, 0) == 0);
    CHECK(pacha_syscall2(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_SET_MASK, 0) == 0);
    const uint64_t before = clock_now();
    CHECK(clock_pending_value((int)self, &value) == 0);
    CHECK(LOAD(clock_signal_seen) == 12 && clock_signal_status == 0);
    CHECK(clock_signal_rip == (uintptr_t)clock_signal_resume);
    CHECK(value == clock_signal_value && value >= before && value <= clock_now());
    // REGISTER requires a valid entry; restore the existing fixture's blocked
    // handler rather than inventing an unsupported unregister operation.
    CHECK(pacha_syscall6(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER,
        (uintptr_t)&(struct pacha_signal_registration){
            .entry = (uintptr_t)notification_entry,
            .inhibit_start = (uintptr_t)__start_native_handlers,
            .inhibit_end = (uintptr_t)__stop_native_handlers,
            .inhibit_secondary_start = 0,
            .inhibit_secondary_end = 0,
            .scratch_base = 0,
            .scratch_size = 0,
            .flags = PACHA_SIGNAL_REGISTRATION_PROCESS_OWNER,
        }, 0, 0, 0, 0) == 0);
    CHECK(pacha_syscall2(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_SET_MASK, 1ull << 11) == 0);
    CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, self) == 0);
    static const char passed[] = "NATIVE_CLOCK_VALUE=PASS\n";
    pacha_syscall2(1, (uintptr_t)passed, sizeof(passed) - 1);
    return 0;
}

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

static long arm(long fd, uint64_t deadline, uint64_t interval)
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

static int mixed_timer_ipc_poll(void)
{
    // Timer readiness needs POLL, not READ. Put IPC first to exercise the
    // lazy clock sample, then wait through registration with both present.
    long timer = pacha_syscall6(PACHA_FD_SYSCALL_TIMERFD_CREATE,
        PACHA_TIMERFD_CLOCK_MONOTONIC, 0, 0, 0,
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE, 0);
    CHECK(timer >= 16);
    uint64_t pair[2];
    CHECK(pacha_syscall3(PACHA_IPC_SYSCALL_CHANNEL_CREATE, (uintptr_t)pair,
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_CLOSE, 0) == 0);
    struct pacha_pollfd items[2] = {
        { .fd = (int)pair[0], .events = PACHA_FD_EVENT_READABLE },
        { .fd = (int)timer, .events = PACHA_FD_EVENT_READABLE }
    };
    CHECK(pacha_syscall2(PACHA_FD_SYSCALL_POLL, (uintptr_t)items, 2) == 0);
    const uint64_t deadline = clock_now() + 10000000;
    CHECK(arm(timer, deadline, 0) == 0);
    CHECK(pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY, (uintptr_t)items, 2, 1000, 0) == 1);
    CHECK(clock_now() >= deadline && items[0].revents == 0 &&
        items[1].revents == PACHA_FD_EVENT_READABLE);
    CHECK(pacha_syscall2(PACHA_FD_SYSCALL_POLL, (uintptr_t)items, 2) == 1);
    CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, timer) == 0);
    CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, pair[0]) == 0);
    CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, pair[1]) == 0);
    return 0;
}

static int clock_copyout_contract(void)
{
    const uint64_t page = 4096, size = 8192;
    const uint64_t rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_CLOSE;
    long fd = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, size, rights, 0);
    CHECK(fd >= 16);
    const uint64_t rw = PACHA_PROT_READ | PACHA_PROT_WRITE;
    long shared = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, 0, size, rw,
        PACHA_MMAP_SHARED, 0);
    CHECK(shared >= (long)page);
    unsigned char *source = (void *)(uintptr_t)shared;
    for (unsigned i = 0; i < size; ++i) source[i] = 0xa5;
    long private = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, 0, size, rw,
        PACHA_MMAP_PRIVATE, 0);
    CHECK(private >= (long)page);
    unsigned char *bytes = (void *)(uintptr_t)private;
    const uintptr_t output = (uintptr_t)bytes + page - 8;
    // Both pages begin as private borrowed backing. Clock copyout must COW
    // each page, not write through its physical alias into the shared VMO.
    CHECK(pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, output) == 0);
    for (unsigned i = 0; i < size; ++i) CHECK(source[i] == 0xa5);
    uint64_t nsec = 0;
    for (unsigned i = 0; i < 8; ++i)
        nsec |= (uint64_t)bytes[page + i] << (8 * i);
    CHECK(nsec < 1000000000ull);
    unsigned char before[16];
    for (unsigned i = 0; i < sizeof(before); ++i) before[i] = bytes[page - 8 + i];
    CHECK(pacha_syscall3(PACHA_VM_SYSCALL_MPROTECT, private, size,
        PACHA_PROT_READ) == 0);
    CHECK(pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, output) ==
        PACHA_SYSCALL_ERR_INVALID);
    for (unsigned i = 0; i < sizeof(before); ++i)
        CHECK(bytes[page - 8 + i] == before[i]);
    CHECK(pacha_syscall3(PACHA_VM_SYSCALL_MPROTECT, private, page, rw) == 0);
    // Cross-page failure may update the writable prefix, but must leave
    // the second read-only page unchanged rather than bypassing its PTE.
    CHECK(pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, output) ==
        PACHA_SYSCALL_ERR_INVALID);
    for (unsigned i = 0; i < 8; ++i) CHECK(bytes[page + i] == before[8 + i]);
    CHECK(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, private + page, page) == 0);
    CHECK(pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, output) ==
        PACHA_SYSCALL_ERR_INVALID);
    for (unsigned i = 0; i < size; ++i) CHECK(source[i] == 0xa5);
    CHECK(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, private, page) == 0);
    CHECK(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, shared, size) == 0);
    CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, fd) == 0);
    static const char passed[] = "NATIVE_CLOCK_COPYOUT=PASS\n";
    pacha_syscall2(1, (uintptr_t)passed, sizeof(passed) - 1);
    return 0;
}

/* Clock page: read-only capability, fast reads in the kernel's monotonic
 * domain. Workers on several CPUs alternate fast and CLOCK_READ reads; every
 * read must be at least the largest value any thread published before it. */
#define CLOCK_PAGE_WORKERS 3
static unsigned char clock_page_stacks[CLOCK_PAGE_WORKERS][16384] __attribute__((aligned(4096)));
static uint64_t clock_page_published, clock_page_fast_hits;
static volatile unsigned clock_page_go, clock_page_failed, clock_page_done;

static uint64_t clock_page_mixed(unsigned i)
{
    uint64_t value;
    if (i & 1) {
        if (!clock_page_monotonic(&value)) {
            __atomic_fetch_add(&clock_page_fast_hits, 1, __ATOMIC_RELAXED);
            return value;
        }
    }
    return clock_now();
}

static int clock_page_check_order(unsigned rounds)
{
    for (unsigned i = 0; i < rounds; ++i) {
        const uint64_t before = __atomic_load_n(&clock_page_published, __ATOMIC_ACQUIRE);
        const uint64_t value = clock_page_mixed(i);
        if (value == UINT64_MAX || value < before) return 1;
        uint64_t seen = before;
        while (seen < value && !__atomic_compare_exchange_n(&clock_page_published, &seen, value,
            1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
        if ((i & 255) == 0) pacha_syscall0(PACHA_RUNTIME_SYSCALL_YIELD);
    }
    return 0;
}

extern void clock_page_worker_entry(void);
__asm__(".text\n.global clock_page_worker_entry\n"
    "clock_page_worker_entry: call clock_page_worker_body; ud2\n");

__attribute__((used, noreturn)) static void clock_page_worker_body(void)
{
    while (!LOAD(clock_page_go)) __asm__ volatile("pause");
    if (clock_page_check_order(20000)) STORE(clock_page_failed, 1);
    __atomic_fetch_add(&clock_page_done, 1, __ATOMIC_ACQ_REL);
    pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, 0, 0, 0);
    __builtin_trap();
}

static int clock_page_contract(void)
{
    (void)realtime;
    const long fd = pacha_syscall0(PACHA_RUNTIME_SYSCALL_CLOCK_PAGE_OPEN);
    CHECK(fd >= 16);
    struct pacha_fd_info info;
    CHECK(pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, fd, (uintptr_t)&info) == 0);
    CHECK(info.kind == PACHA_FD_KIND_VMO && info.size == PACHA_CLOCK_PAGE_SIZE);
    CHECK(info.rights == (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER));
    /* Only a shared read-only view exists. */
    CHECK(pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, 0, PACHA_CLOCK_PAGE_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0) < 4096);
    CHECK(pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, 0, PACHA_CLOCK_PAGE_SIZE,
        PACHA_PROT_READ, PACHA_MMAP_PRIVATE, 0) < 4096);
    CHECK(pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, 0, PACHA_CLOCK_PAGE_SIZE,
        PACHA_PROT_READ | PACHA_PROT_EXEC, PACHA_MMAP_SHARED, 0) < 4096);
    const long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, 0, PACHA_CLOCK_PAGE_SIZE,
        PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
    /* Native status codes are small positive values, never mappable addresses. */
    CHECK(address >= 4096);
    CHECK(pacha_syscall3(PACHA_VM_SYSCALL_MPROTECT, address, PACHA_CLOCK_PAGE_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE) != 0);
    CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, fd) == 0);
    clock_page = (const volatile unsigned char *)(uintptr_t)address;
    /* This QEMU/KVM configuration has a stable kvmclock, so the fast path
     * must actually be exercised; otherwise the ordering test is vacuous. */
    CHECK(CLOCK_PAGE_U32(PACHA_CLOCK_PAGE_FAST_OFFSET) == 1);
    CHECK(CLOCK_PAGE_U64(PACHA_CLOCK_PAGE_FLOOR_NS_OFFSET) <
        CLOCK_PAGE_U64(PACHA_CLOCK_PAGE_CEILING_NS_OFFSET));
    for (unsigned i = 0; i < 1000; ++i) {
        uint64_t before = clock_now(), fast, after;
        CHECK(!monotonic(&fast)); /* the adapter's callback: fast or CLOCK_READ */
        after = clock_now();
        CHECK(before <= fast && fast <= after);
    }
    const uint64_t rights = PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE;
    long threads[CLOCK_PAGE_WORKERS];
    for (unsigned i = 0; i < CLOCK_PAGE_WORKERS; ++i) {
        threads[i] = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE, PACHA_PROCESS_SELF_FD,
            (uintptr_t)clock_page_worker_entry,
            (uintptr_t)(clock_page_stacks[i] + sizeof(clock_page_stacks[i])), 0, 0, rights);
        CHECK(threads[i] >= 16);
        CHECK(pacha_syscall1(PACHA_THREAD_SYSCALL_START, threads[i]) == 0);
    }
    STORE(clock_page_go, 1);
    CHECK(clock_page_check_order(20000) == 0);
    for (unsigned i = 0; i < CLOCK_PAGE_WORKERS; ++i) {
        uint64_t status[4];
        long result;
        const uint64_t end = clock_now() + 10000000000ull;
        do {
            result = pacha_syscall2(PACHA_THREAD_SYSCALL_WAIT, threads[i], (uintptr_t)status);
            CHECK(clock_now() < end);
        } while (result == PACHA_SYSCALL_ERR_NOT_READY);
        CHECK(result == 0 && status[0] == 2 && status[1] == 0);
        CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, threads[i]) == 0);
    }
    CHECK(!LOAD(clock_page_failed) && LOAD(clock_page_done) == CLOCK_PAGE_WORKERS);
    /* Most odd-indexed reads should be fast; a ceiling refresh needs one
     * CLOCK_READ per window, so require a clear majority, not all. */
    CHECK(__atomic_load_n(&clock_page_fast_hits, __ATOMIC_RELAXED) > 4 * 20000 / 2 / 2);
    CHECK(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, address, PACHA_CLOCK_PAGE_SIZE) == 0);
    clock_page = NULL;
    static const char passed[] = "NATIVE_CLOCK_PAGE=PASS\n";
    pacha_syscall2(1, (uintptr_t)passed, sizeof(passed) - 1);
    return 0;
}

int native_clock_contract(void)
{
    CHECK(clock_page_contract() == 0);
    extern int native_clock_lifecycle_contract(void);
    CHECK(clock_register_contract() == 0);
    CHECK(native_clock_lifecycle_contract() == 0);
    CHECK(clock_copyout_contract() == 0);
    CHECK(mixed_timer_ipc_poll() == 0);
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
