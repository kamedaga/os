/* Minimal probe: does a forked child still support pthread_create?
 *
 * Case A: parent creates a thread (baseline, known good).
 * Case B: a plain fork child creates a thread.
 * Case C: a fork child creates a thread after a sibling process was killed
 *         and reaped (the sequence used by the signal-owner red fixture).
 * Case D: a fork child creates a thread while another thread remains live in
 *         the parent; only the calling thread may exist in the child.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static __thread uintptr_t tls_cookie;

static uintptr_t fs_self_pointer(void)
{
    uintptr_t value;
    __asm__ volatile ("movq %%fs:0, %0" : "=r"(value));
    return value;
}

typedef struct tls_check {
    uintptr_t cookie;
    int status;
} tls_check_t;

static void *tls_check_thread(void *argument)
{
    tls_check_t *check = argument;
    if (fs_self_pointer() != (uintptr_t)pthread_self()) {
        check->status = 1;
        return 0;
    }
    tls_cookie = check->cookie;
    if (tls_cookie != check->cookie) {
        check->status = 2;
    }
    return 0;
}

static void *noop_thread(void *argument)
{
    (void)argument;
    return 0;
}

static pthread_mutex_t live_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t live_worker_cond = PTHREAD_COND_INITIALIZER;
static int live_worker_ready;
static int live_worker_stop;

static void *live_worker(void *argument)
{
    (void)argument;
    (void)pthread_mutex_lock(&live_worker_mutex);
    live_worker_ready = 1;
    (void)pthread_cond_broadcast(&live_worker_cond);
    while (!live_worker_stop) {
        (void)pthread_cond_wait(&live_worker_cond, &live_worker_mutex);
    }
    (void)pthread_mutex_unlock(&live_worker_mutex);
    return 0;
}

static int create_and_join(void)
{
    pthread_t thread;
    const int status = pthread_create(&thread, 0, noop_thread, 0);
    if (status != 0) return status;
    (void)pthread_join(thread, 0);
    return 0;
}

static int check_tls_and_create_thread(uintptr_t cookie)
{
    if (fs_self_pointer() != (uintptr_t)pthread_self()) return 10;
    tls_cookie = cookie;
    tls_check_t check = {
        .cookie = cookie ^ UINT64_C(0x5a5aa5a55a5aa5a5),
        .status = 0,
    };
    pthread_t thread;
    const int create_status = pthread_create(&thread, 0, tls_check_thread, &check);
    if (create_status != 0) return 20 + create_status;
    const int join_status = pthread_join(thread, 0);
    if (join_status != 0) return 40 + join_status;
    if (check.status != 0) return 60 + check.status;
    if (tls_cookie != cookie) return 70;
    return 0;
}

static int run_stress_iteration(unsigned iteration)
{
    pid_t child = -1;
    for (unsigned attempt = 0; attempt < 1000u; ++attempt) {
        child = fork();
        if (child >= 0) break;
        if (errno != EAGAIN && errno != ENOMEM) break;
        (void)usleep(1000);
        (void)sched_yield();
    }
    if (child < 0) return 80;
    if (child == 0) {
        if ((iteration & 1u) != 0) {
            char number[24];
            (void)snprintf(number, sizeof(number), "%u", iteration);
            for (unsigned attempt = 0; attempt < 1000u; ++attempt) {
                execl("/cmd/lpr_fork_pthread_probe.elf",
                    "lpr_fork_pthread_probe", "--exec-child", number, (char *)0);
                if (errno != EAGAIN && errno != ENOMEM) break;
                (void)usleep(1000);
                (void)sched_yield();
            }
            _exit(81);
        }
        const int status = check_tls_and_create_thread(
            UINT64_C(0x1357000000000000) | iteration);
        _exit(status == 0 ? 0 : status);
    }
    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    if (waited != child) {
        char line[128];
        const int saved_errno = errno;
        const int length = snprintf(line, sizeof(line),
            "FORK_EXEC_TLS_WAIT_FAIL iteration=%u child=%ld waited=%ld errno=%d\n",
            iteration, (long)child, (long)waited, saved_errno);
        if (length > 0) (void)write(STDOUT_FILENO, line, (size_t)length);
        return 82;
    }
    if (!WIFEXITED(status)) return 83;
    return WEXITSTATUS(status);
}

static int run_stress(unsigned iterations)
{
    char line[96];
    int length = snprintf(line, sizeof(line),
        "FORK_EXEC_TLS_STRESS_START iterations=%u\n", iterations);
    if (length > 0) (void)write(STDOUT_FILENO, line, (size_t)length);

    pthread_t worker;
    const int worker_status = pthread_create(&worker, 0, live_worker, 0);
    if (worker_status != 0) return worker_status;
    (void)pthread_mutex_lock(&live_worker_mutex);
    while (!live_worker_ready) {
        (void)pthread_cond_wait(&live_worker_cond, &live_worker_mutex);
    }
    (void)pthread_mutex_unlock(&live_worker_mutex);

    int result = 0;
    for (unsigned iteration = 0; iteration < iterations; ++iteration) {
        result = run_stress_iteration(iteration);
        if (result != 0) {
            length = snprintf(line, sizeof(line),
                "FORK_EXEC_TLS_STRESS_FAIL iteration=%u rc=%d\n",
                iteration, result);
            if (length > 0) (void)write(STDOUT_FILENO, line, (size_t)length);
            break;
        }
        if ((iteration + 1u) % 32u == 0u) {
            length = snprintf(line, sizeof(line),
                "FORK_EXEC_TLS_STRESS_PROGRESS=%u\n", iteration + 1u);
            if (length > 0) (void)write(STDOUT_FILENO, line, (size_t)length);
        }
    }

    (void)pthread_mutex_lock(&live_worker_mutex);
    live_worker_stop = 1;
    (void)pthread_cond_broadcast(&live_worker_cond);
    (void)pthread_mutex_unlock(&live_worker_mutex);
    (void)pthread_join(worker, 0);
    if (result == 0) {
        length = snprintf(line, sizeof(line),
            "FORK_EXEC_TLS_STRESS=OK iterations=%u\n", iterations);
        if (length > 0) (void)write(STDOUT_FILENO, line, (size_t)length);
    }
    return result;
}

static int run_parallel_stress(unsigned workers, unsigned iterations)
{
    pid_t children[16];
    if (workers == 0 || workers > 16u) return 90;
    memset(children, 0, sizeof(children));
    for (unsigned worker = 0; worker < workers; ++worker) {
        const pid_t child = fork();
        if (child < 0) return 91;
        if (child == 0) {
            const int status = run_stress(iterations);
            _exit(status == 0 ? 0 : status);
        }
        children[worker] = child;
    }
    int result = 0;
    for (unsigned worker = 0; worker < workers; ++worker) {
        int status = 0;
        if (waitpid(children[worker], &status, 0) != children[worker] ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            result = 92;
        }
    }
    char line[112];
    const int length = snprintf(line, sizeof(line),
        "FORK_EXEC_TLS_PARALLEL=%s workers=%u iterations=%u\n",
        result == 0 ? "OK" : "FAIL", workers, iterations);
    if (length > 0) (void)write(STDOUT_FILENO, line, (size_t)length);
    return result;
}

static int wait_for_clean_exit(pid_t child)
{
    int status = 0;
    if (waitpid(child, &status, 0) != child) return 1;
    if (!WIFEXITED(status)) return 2;
    return WEXITSTATUS(status);
}

static int run_cow_mprotect_probe(void)
{
    const uint64_t child_marker = UINT64_C(0x434f575f4348494c);
    const uint64_t initial_marker = UINT64_C(0x434f575f42415345);
    const uint64_t parent_marker = UINT64_C(0x434f575f50415245);

    volatile uint64_t *arena = mmap(0, 8192, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) return 101;
    const pid_t none_child = fork();
    if (none_child < 0) return 102;
    if (none_child == 0) {
        if (mprotect((void *)arena, 4096, PROT_READ | PROT_WRITE) != 0) _exit(103);
        arena[0] = child_marker;
        if (arena[0] != child_marker) _exit(104);
        _exit(0);
    }
    int status = wait_for_clean_exit(none_child);
    if (status != 0) return status;
    if (mprotect((void *)arena, 4096, PROT_READ) != 0) return 105;
    if (arena[0] != 0) return 106;
    if (munmap((void *)arena, 8192) != 0) return 107;

    volatile uint64_t *present = mmap(0, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (present == MAP_FAILED) return 108;
    present[0] = initial_marker;
    int ready_pipe[2];
    if (pipe(ready_pipe) != 0) return 109;
    const pid_t present_child = fork();
    if (present_child < 0) return 110;
    if (present_child == 0) {
        char ready = 0;
        (void)close(ready_pipe[1]);
        if (read(ready_pipe[0], &ready, 1) != 1 || ready != 'R') _exit(111);
        if (present[0] != initial_marker) _exit(112);
        _exit(0);
    }
    (void)close(ready_pipe[0]);
    if (mprotect((void *)present, 4096, PROT_READ) != 0) return 113;
    if (mprotect((void *)present, 4096, PROT_READ | PROT_WRITE) != 0) return 114;
    present[0] = parent_marker;
    if (write(ready_pipe[1], "R", 1) != 1) return 115;
    (void)close(ready_pipe[1]);
    status = wait_for_clean_exit(present_child);
    if (status != 0) return status;
    if (present[0] != parent_marker) return 116;
    if (munmap((void *)present, 4096) != 0) return 117;
    return 0;
}

static void report(const char *label, int status)
{
    char buffer[96];
    const int length = snprintf(buffer, sizeof(buffer),
        "FORK_PTHREAD_%s=%s rc=%d\n", label, status == 0 ? "OK" : "FAIL", status);
    if (length > 0) (void)write(STDOUT_FILENO, buffer, (size_t)length);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--exec-child") == 0) {
        const unsigned iteration = argc >= 3 ? (unsigned)strtoul(argv[2], 0, 10) : 0;
        return check_tls_and_create_thread(
            UINT64_C(0x2468000000000000) | iteration);
    }
    if (argc >= 2 && strcmp(argv[1], "--stress") == 0) {
        unsigned iterations = argc >= 3 ? (unsigned)strtoul(argv[2], 0, 10) : 128u;
        if (iterations == 0 || iterations > 4096u) iterations = 128u;
        return run_stress(iterations);
    }
    if (argc >= 2 && strcmp(argv[1], "--parallel") == 0) {
        unsigned workers = argc >= 3 ? (unsigned)strtoul(argv[2], 0, 10) : 8u;
        unsigned iterations = argc >= 4 ? (unsigned)strtoul(argv[3], 0, 10) : 128u;
        if (workers == 0 || workers > 16u) workers = 8u;
        if (iterations == 0 || iterations > 4096u) iterations = 128u;
        return run_parallel_stress(workers, iterations);
    }
    if (argc >= 2 && strcmp(argv[1], "--cow-mprotect") == 0) {
        const int status = run_cow_mprotect_probe();
        char line[96];
        const int length = snprintf(line, sizeof(line),
            "FORK_COW_MPROTECT=%s rc=%d\n",
            status == 0 ? "OK" : "FAIL", status);
        if (length > 0) (void)write(STDOUT_FILENO, line, (size_t)length);
        return status;
    }
    (void)write(STDOUT_FILENO, "FORK_PTHREAD_START\n", 19);

    report("PARENT", create_and_join());

    const pid_t plain = fork();
    if (plain == 0) {
        report("CHILD_PLAIN", create_and_join());
        _exit(0);
    }
    int status = 0;
    (void)waitpid(plain, &status, 0);

    const pid_t holder = fork();
    if (holder == 0) {
        for (;;) pause();
    }
    const pid_t child = fork();
    if (child == 0) {
        report("CHILD_AFTER_KILL", create_and_join());
        _exit(0);
    }
    (void)kill(holder, SIGKILL);
    (void)waitpid(holder, &status, 0);
    (void)waitpid(child, &status, 0);

    pthread_t worker;
    const int worker_status = pthread_create(&worker, 0, live_worker, 0);
    if (worker_status != 0) {
        report("CHILD_LIVE_WORKER", worker_status);
    } else {
        (void)pthread_mutex_lock(&live_worker_mutex);
        while (!live_worker_ready) {
            (void)pthread_cond_wait(&live_worker_cond, &live_worker_mutex);
        }
        (void)pthread_mutex_unlock(&live_worker_mutex);

        const pid_t multithreaded = fork();
        if (multithreaded == 0) {
            report("CHILD_LIVE_WORKER", create_and_join());
            _exit(0);
        }
        (void)waitpid(multithreaded, &status, 0);

        (void)pthread_mutex_lock(&live_worker_mutex);
        live_worker_stop = 1;
        (void)pthread_cond_broadcast(&live_worker_cond);
        (void)pthread_mutex_unlock(&live_worker_mutex);
        (void)pthread_join(worker, 0);
    }

    (void)write(STDOUT_FILENO, "FORK_PTHREAD_DONE\n", 18);
    return 0;
}
