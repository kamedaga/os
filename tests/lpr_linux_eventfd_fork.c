#define _GNU_SOURCE
#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

static int consume(int fd, int semaphore)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, 5000) != 1 || !(p.revents & POLLIN)) return 1;
    uint64_t value = 0;
    if (eventfd_read(fd, &value) != 0 || value != (semaphore ? 1u : 42u)) return 2;
    if (semaphore) {
        for (int i = 0; i < 2; ++i)
            if (eventfd_read(fd, &value) != 0 || value != 1) return 3;
    }
    if (poll(&p, 1, 0) != 0) return 4;
    close(fd);
    return 0;
}

static void run(const char *executable, int do_exec, int semaphore)
{
    int fd = eventfd(0, (do_exec ? 0 : EFD_CLOEXEC) |
                         (semaphore ? EFD_SEMAPHORE : 0));
    int ready[2];
    assert(fd >= 0 && pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready[0]);
        int alias = dup(fd);
        assert(alias >= 0);
        close(fd);
        assert(write(ready[1], "R", 1) == 1);
        close(ready[1]);
        if (do_exec) {
            char number[32];
            snprintf(number, sizeof(number), "%d", alias);
            execl(executable, executable, "--consume", number, NULL);
            _exit(100);
        }
        _exit(consume(alias, semaphore));
    }
    close(ready[1]);
    char marker;
    assert(read(ready[0], &marker, 1) == 1);
    close(ready[0]);
    assert(eventfd_write(fd, semaphore ? 3 : 42) == 0);
    /* The child's open description must keep the counter and notification
     * capabilities alive after the parent closes its final descriptor. */
    close(fd);
    int status;
    assert(waitpid(child, &status, 0) == child);
    printf("EVENTFD phase=%s status=%d\n", do_exec ? "exec" :
           (semaphore ? "semaphore" : "fork"), status);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--consume") == 0)
        return consume(atoi(argv[2]), 0);
    run(argv[0], 0, 0);
    run(argv[0], 1, 0);
    run(argv[0], 0, 1);
    puts("EVENTFD_FORK=PASS shared-counter fork exec dup parent-close semaphore");
    return 0;
}
