#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_int stopping;
static atomic_int worker_error;
static void *cycle_open(void *unused)
{
    (void)unused;
    while (!atomic_load(&stopping)) {
        int fd = open("/usr/share/fonts", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0 || close(fd)) { atomic_store(&worker_error, errno); break; }
    }
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_t worker;
    if (pthread_create(&worker, NULL, cycle_open, NULL)) return 2;
    int failed = 0;
    for (unsigned i = 0; i < 32; ++i) {
        pid_t child = fork();
        if (!child) _exit(0);
        if (child < 0) {
            printf("FORK_OPEN_RACE iteration=%u errno=%d\n", i, errno);
            failed = 1;
            break;
        }
        int status;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
            failed = 1;
            break;
        }
    }
    atomic_store(&stopping, 1);
    pthread_join(worker, NULL);
    printf("FORK_OPEN_RACE=%s worker_error=%d\n",
        failed || atomic_load(&worker_error) ? "FAIL" : "OK", atomic_load(&worker_error));
    return failed || atomic_load(&worker_error);
}
