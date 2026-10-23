#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Keep both backing pages live while reusing one virtual address. Reusing
 * freshly freed anonymous memory alone can mask stale TLB entries when the
 * allocator immediately returns the same physical page. Readers are quiescent
 * during unmap/remap; any stale value after publication is therefore a bug,
 * not an intentional access to an unmapped address. */
enum { READERS = 3, ROUNDS = 128, PAGE = 4096 };
static unsigned char *view;
static atomic_uint epoch, done[READERS], failed;

static unsigned cpu_masks[READERS], migration_counts[READERS];
static unsigned cpu_now(void);
static void blocked_permission_test(void);
static int syscall_bench(void);

static void *reader(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    unsigned previous = cpu_now();
    for (unsigned round = 1; round <= ROUNDS; ++round) {
        unsigned cpu = cpu_now();
        cpu_masks[id] |= 1U << (cpu & 31);
        migration_counts[id] += cpu != previous; previous = cpu;
        while (atomic_load_explicit(&epoch, memory_order_acquire) < round)
            __asm__ volatile("pause" ::: "memory");
        unsigned char expected = (round & 1) ? 0x35 : 0xa7;
        volatile unsigned char *sample = view;
        if (sample[0] != expected || sample[PAGE - 1] != expected)
            atomic_store_explicit(&failed, 1, memory_order_relaxed);
        atomic_store_explicit(&done[id], round, memory_order_release);
        if (id == 2) for (unsigned i=0; i<4; ++i) sched_yield();
    }
    return NULL;
}

static void fail(const char *where)
{
    fprintf(stderr, "TLB_REUSE FAIL %s errno=%d\n", where, errno);
    /* Do not let readers access a partially restored mapping on failure. */
    _exit(1);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--micro")) return syscall_bench();
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(120);
    char path[] = "/tmp/tlb-reuse-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) fail("mkstemp");
    if (unlink(path) || ftruncate(fd, PAGE * 2)) fail("prepare");
    unsigned char *alias = mmap(NULL, PAGE * 2, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
    if (alias == MAP_FAILED) fail("alias");
    for (unsigned i = 0; i < PAGE; ++i) {
        alias[i] = 0x35;
        alias[PAGE + i] = 0xa7;
    }
    view = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) fail("initial view");
    if (syscall(SYS_membarrier, 1 << 4, 0, 0)) fail("register membarrier");
    pthread_t threads[READERS];
    for (unsigned i = 0; i < READERS; ++i) {
        int result = pthread_create(&threads[i], NULL, reader, (void *)(uintptr_t)i);
        if (result) { errno = result; fail("pthread_create"); }
    }
    void *permissions = mmap(NULL, PAGE * 2, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (permissions == MAP_FAILED) fail("permission probe");
    memset(permissions, 1, PAGE * 2);
    for (unsigned round = 1; round <= ROUNDS; ++round) {
        /* Separate leaf downgrades while readers spin without syscalls. */
        int prot = (round & 1) ? PROT_READ : PROT_READ | PROT_WRITE;
        if (mprotect(permissions, PAGE, prot) ||
            mprotect((char *)permissions + PAGE, PAGE, prot)) fail("consecutive mprotect");
        if (munmap(view, PAGE)) fail("munmap");
        void *mapped = mmap(view, PAGE, PROT_READ, MAP_SHARED | MAP_FIXED,
                            fd, (round & 1) ? 0 : PAGE);
        if (mapped != view) fail("same-address mmap");
        /* Do not issue membarrier here: its own CR3 flush could hide a
         * missing munmap shootdown. Readers also avoid syscall transitions. */
        atomic_store_explicit(&epoch, round, memory_order_release);
        for (unsigned i = 0; i < READERS; ++i)
            while (atomic_load_explicit(&done[i], memory_order_acquire) != round)
                __asm__ volatile("pause" ::: "memory");
    }
    for (unsigned i = 0; i < READERS; ++i) pthread_join(threads[i], NULL);
    unsigned migrations = 0, cpus = 0;
    for (unsigned i=0; i<READERS; ++i) {
        migrations += migration_counts[i]; cpus |= cpu_masks[i];
        printf("TLB_READER id=%u cpumask=%x migrations=%u\n", i, cpu_masks[i], migration_counts[i]);
    }
    printf("TLB_MIGRATION observed=%u cpumask=%x\n", migrations, cpus);
    if (munmap(permissions, PAGE * 2)) fail("permission cleanup");
    blocked_permission_test();
    /* Separate API smoke only; this is not a memory-ordering litmus test. */
    for (unsigned i = 0; i < 16; ++i)
        if (syscall(SYS_membarrier, 1 << 3, 0, 0)) fail("membarrier");
    if (alias[0] != 0x35 || alias[PAGE] != 0xa7)
        atomic_store(&failed, 1);
    if (munmap(view, PAGE) || munmap(alias, PAGE * 2) || close(fd)) fail("cleanup");
    printf("TLB_REUSE=%s readers=%u rounds=%u\n",
           atomic_load(&failed) ? "FAIL" : "OK", READERS, ROUNDS);
    return atomic_load(&failed) ? 1 : 0;
}

/* Additional dormant/active coverage. RDTSCP samples CPU identity without a
 * syscall boundary, so two pure-user readers can refill between consecutive
 * requests. The third reader yields between rounds to exercise redispatch. */
static unsigned cpu_now(void)
{
    unsigned lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
    (void)lo; (void)hi;
    return aux;
}

static _Thread_local sigjmp_buf fault_return;
static _Thread_local volatile sig_atomic_t wanted_fault, got_fault;
static void *protected_pages;
static int wake_pipe[2];
static atomic_uint blocked_ready;
static void permission_fault(int sig, siginfo_t *info, void *context)
{
    (void)context;
    uintptr_t address = (uintptr_t)info->si_addr;
    if (!wanted_fault || sig != SIGSEGV || address < (uintptr_t)protected_pages ||
        address >= (uintptr_t)protected_pages + PAGE * 2) _exit(202);
    got_fault = 1;
    siglongjmp(fault_return, 1);
}
static void *blocked_writer(void *unused)
{
    (void)unused;
    volatile unsigned char *p = protected_pages;
    p[0] = 17; p[PAGE] = 23; /* Prime writable translations before blocking. */
    atomic_store_explicit(&blocked_ready, 1, memory_order_release);
    char byte;
    if (read(wake_pipe[0], &byte, 1) != 1) fail("blocked read");
    for (unsigned i=0; i<2; ++i) {
        got_fault = 0; wanted_fault = 1;
        if (!sigsetjmp(fault_return, 1)) {
            p[i * PAGE] = 99;
            fail("write after unblock did not fault");
        }
        wanted_fault = 0;
        if (!got_fault) fail("wrong permission fault");
    }
    return NULL;
}
static void blocked_permission_test(void)
{
    struct sigaction action = { .sa_sigaction = permission_fault, .sa_flags = SA_SIGINFO };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL)) fail("sigaction");
    for (unsigned i=0; i<32; ++i) {
        protected_pages = mmap(NULL, PAGE * 2, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (protected_pages == MAP_FAILED || pipe(wake_pipe)) fail("blocked setup");
        atomic_store(&blocked_ready, 0);
        pthread_t thread;
        if (pthread_create(&thread, NULL, blocked_writer, NULL)) fail("blocked create");
        while (!atomic_load_explicit(&blocked_ready, memory_order_acquire))
            __asm__ volatile("pause" ::: "memory");
        /* Readiness precedes read; allow it to reach the blocking syscall. */
        usleep(10000);
        if (mprotect(protected_pages, PAGE, PROT_READ) ||
            mprotect((char *)protected_pages + PAGE, PAGE, PROT_READ)) fail("downgrade");
        if (write(wake_pipe[1], "x", 1) != 1 || pthread_join(thread, NULL)) fail("unblock");
        if (munmap(protected_pages, PAGE * 2) || close(wake_pipe[0]) || close(wake_pipe[1])) fail("blocked cleanup");
    }
    puts("TLB_BLOCKED_WRITE_FAULT=OK rounds=32 faults=64");
}
static uint64_t monotonic_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fail("clock");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int syscall_bench(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned futex_word = 0;
    for (unsigned mode=0; mode<3; ++mode)
    for (unsigned trial=0; trial<7; ++trial) {
        const unsigned count = 20000;
        uint64_t start = monotonic_ns();
        for (unsigned i=0; i<count; ++i) {
            /* Linux getpid is cached by LPR. sched_yield always invokes the
             * native YIELD syscall, so mode 1 measures the kernel return fence
             * as well as the scheduler's ordinary yield work. */
            long result = mode == 2 ? syscall(SYS_futex, &futex_word, 1, 1, 0, 0, 0) :
                          mode == 1 ? syscall(SYS_sched_yield) : syscall(SYS_getpid);
            if ((mode && result != 0) || (!mode && result <= 0)) fail("micro syscall");
        }
        uint64_t elapsed = monotonic_ns() - start;
        printf("TLB_MICRO phase=%s trial=%u ops=%u ns=%llu ns_per_op=%.3f\n",
               mode == 2 ? "futex_wake_empty" : mode == 1 ? "yield_kernel" : "getpid_cached", trial, count,
               (unsigned long long)elapsed, (double)elapsed / count);
    }
    puts("TLB_MICRO=OK");
    return 0;
}
