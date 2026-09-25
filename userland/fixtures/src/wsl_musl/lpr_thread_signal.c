#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "LPR_THREAD_SIGNAL=FAIL line=%d errno=%d\n", __LINE__, errno); \
    _Exit(1); \
} } while (0)

static _Thread_local int slot;
static atomic_int tids[3], seen[3], ready, command, done;
static sem_t masked_gate, sleep_gate, reply;

static void handler(int sig)
{
    int saved = errno;
    if (sig != SIGUSR1) _Exit(2);
    atomic_fetch_add(&seen[slot], 1);
    if (slot == 1 && syscall(SYS_tgkill, getpid(), atomic_load(&tids[0]), sig))
        _Exit(3);
    errno = saved;
}

static void wait_sem(sem_t *sem)
{
    int result;
    do result = sem_wait(sem); while (result && errno == EINTR);
    CHECK(!result);
}

static void sync_credentials(void)
{
    /* Invoke unmodified musl wrappers, not raw credential syscalls. Even
     * no-op IDs require its signal-34 / semaphore handshake on every thread. */
    CHECK(!setresuid(-1, -1, -1));
    CHECK(!setresgid(-1, -1, -1));
}

static void *worker(void *arg)
{
    slot = (int)(long)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (slot == 2) CHECK(!pthread_sigmask(SIG_BLOCK, &set, NULL));
    atomic_store(&tids[slot], syscall(SYS_gettid));
    atomic_fetch_add(&ready, 1);
    if (slot == 2) {
        wait_sem(&masked_gate);
        CHECK(!pthread_sigmask(SIG_UNBLOCK, &set, NULL));
        wait_sem(&sleep_gate);
    } else {
        /* No syscall is needed for delivery to this CPU-bound target. */
        while (!atomic_load(&done)) {
            if (atomic_exchange(&command, 0)) {
                sync_credentials();
                CHECK(!sem_post(&reply));
            }
        }
    }
    return NULL;
}

static void wait_seen(int index)
{
    while (!atomic_load(&seen[index])) sched_yield();
}

static void *kill_target(void *unused)
{
    (void)unused;
    for (;;) sched_yield();
    return NULL;
}

int main(void)
{
    alarm(20);
    struct sigaction action = {.sa_handler = handler, .sa_flags = SA_RESTART};
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGUSR1, &action, NULL));
    CHECK(!sem_init(&masked_gate, 0, 0));
    CHECK(!sem_init(&sleep_gate, 0, 0));
    CHECK(!sem_init(&reply, 0, 0));
    atomic_store(&tids[0], syscall(SYS_gettid));
    pthread_t threads[2];
    CHECK(!pthread_create(&threads[0], NULL, worker, (void *)1L));
    CHECK(!pthread_create(&threads[1], NULL, worker, (void *)2L));
    while (atomic_load(&ready) != 2) sched_yield();

    CHECK(!syscall(SYS_tkill, atomic_load(&tids[1]), 0));
    CHECK(!syscall(SYS_tkill, atomic_load(&tids[1]), SIGUSR1));
    wait_seen(0);
    CHECK(atomic_load(&seen[1]) == 1 && !atomic_load(&seen[2]));
    puts("LPR_THREAD_SIGNAL_BUSY_AND_MAIN=OK");

    CHECK(!syscall(SYS_tgkill, getpid(), atomic_load(&tids[2]), SIGUSR1));
    usleep(10000);
    CHECK(!atomic_load(&seen[2]));
    CHECK(!sem_post(&masked_gate));
    wait_seen(2);
    CHECK(atomic_load(&seen[0]) == 1 && atomic_load(&seen[1]) == 1);
    puts("LPR_THREAD_SIGNAL_MASKED=OK");

    CHECK(syscall(SYS_tgkill, getpid() + 1, atomic_load(&tids[1]), SIGUSR1) == -1 && errno == ESRCH);
    CHECK(syscall(SYS_tkill, INT_MAX, SIGUSR1) == -1 && errno == ESRCH);
    CHECK(syscall(SYS_tkill, atomic_load(&tids[1]), 65) == -1 && errno == EINVAL);
    for (int i = 0; i < 8; i++) {
        sync_credentials();
        atomic_store(&command, 1);
        wait_sem(&reply);
    }
    puts("LPR_THREAD_SIGNAL_MUSL_SYNCCALL=OK");

    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        /* PRIVATE capabilities must not target the parent's old threads. */
        CHECK(syscall(SYS_tgkill, getpid(), atomic_load(&tids[1]), 0) == -1 && errno == ESRCH);
        sync_credentials();
        _Exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    atomic_store(&done, 1);
    CHECK(!sem_post(&sleep_gate));
    CHECK(!pthread_join(threads[0], NULL));
    CHECK(!pthread_join(threads[1], NULL));
    CHECK(syscall(SYS_tkill, atomic_load(&tids[1]), 0) == -1 && errno == ESRCH);
    CHECK(!sem_destroy(&masked_gate) && !sem_destroy(&sleep_gate) && !sem_destroy(&reply));
    puts("LPR_THREAD_SIGNAL_FORK_EXIT=OK");
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        pthread_t victim;
        CHECK(!pthread_create(&victim, NULL, kill_target, NULL));
        CHECK(!pthread_kill(victim, SIGKILL));
        _Exit(4);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    puts("LPR_THREAD_SIGNAL_GROUP_KILL=OK");
    puts("LPR_THREAD_SIGNAL=OK");
    return 0;
}
