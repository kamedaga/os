#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_int stop, failure;
static void *reader(void *unused)
{
    (void)unused;
    char bytes[4096];
    while (!atomic_load(&stop)) {
        int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
        if (fd < 0) { atomic_store(&failure, errno); break; }
        if (read(fd, bytes, sizeof(bytes)) < 0) atomic_store(&failure, errno);
        if (close(fd)) atomic_store(&failure, errno);
    }
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Carry a FileD lease into the multi-threaded parent as WebKit does. */
    int inherited = open("/proc/meminfo", O_RDONLY);
    if (inherited < 0) { perror("FORK_FILED initial open"); return 2; }
    pid_t outer = fork();
    if (outer < 0) { perror("FORK_FILED outer fork"); return 2; }
    if (outer) {
        int status;
        if (waitpid(outer, &status, 0) != outer) { perror("FORK_FILED outer wait"); return 2; }
        close(inherited);
        return !WIFEXITED(status) || WEXITSTATUS(status);
    }
    pthread_t workers[4];
    for (unsigned i = 0; i < 4; ++i) {
        int error = pthread_create(&workers[i], NULL, reader, NULL);
        if (error) { dprintf(2, "FORK_FILED pthread index=%u error=%d\n", i, error); _exit(2); }
    }
    int failed = 0;
    for (unsigned round = 0; round < 64; ++round) {
        pid_t child = fork();
        if (!child) {
            char byte;
            int fd = open("/proc/meminfo", O_RDONLY);
            if (fd < 0 || read(inherited, &byte, 1) != 1) {
                dprintf(2, "FORK_FILED child round=%u errno=%d\n", round, errno);
                _exit(3);
            }
            close(fd);
            execl("/bin/true", "true", (char *)NULL);
            dprintf(2, "FORK_FILED exec round=%u errno=%d\n", round, errno);
            _exit(4);
        }
        int status;
        if (child < 0 || waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status)) {
            printf("FORK_FILED failed round=%u child=%ld errno=%d\n", round, (long)child, errno);
            failed = 1;
            break;
        }
    }
    atomic_store(&stop, 1);
    for (unsigned i = 0; i < 4; ++i) pthread_join(workers[i], NULL);
    printf("FORK_FILED=%s worker_errno=%d\n", failed ? "FAIL" : "OK", atomic_load(&failure));
    _exit(failed || atomic_load(&failure));
}
