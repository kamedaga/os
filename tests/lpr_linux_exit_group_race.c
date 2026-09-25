#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_int ready;
static int watched;
static void *reader(void *unused)
{
    (void)unused;
    char data[4096];
    atomic_store(&ready, 1);
    for (;;) {
        ssize_t n = read(watched, data, sizeof(data));
        /* exit_group must stop us before disposing the process FD table. */
        if (n < 0 && errno != EAGAIN && errno != EINTR)
            __asm__ volatile("int3");
    }
    return NULL;
}

int main(void)
{
    setbuf(stdout, NULL);
    for (unsigned round = 0; round < 32; ++round) {
        pid_t child = fork();
        if (child < 0) { perror("fork"); return 1; }
        if (!child) {
            watched = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (watched < 0) _exit(80);
            /* Keep cleanup busy after the earliest descriptor is closed. */
            for (unsigned i = 0; i < 48; ++i)
                if (open("/etc/os-release", O_RDONLY | O_CLOEXEC) < 0) _exit(81);
            pthread_t thread;
            if (pthread_create(&thread, NULL, reader, NULL)) _exit(82);
            while (!atomic_load(&ready)) sched_yield();
            syscall(SYS_exit_group, 37);
            _exit(83);
        }
        int status = 0;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 37) {
            printf("EXIT_GROUP_RACE FAIL round=%u status=%d child=%ld waited=%ld errno=%d\n",
                round, status, (long)child, (long)waited, errno);
            return 2;
        }
    }
    puts("EXIT_GROUP_RACE PASS 32 exits with active inotify reader");
    return 0;
}
