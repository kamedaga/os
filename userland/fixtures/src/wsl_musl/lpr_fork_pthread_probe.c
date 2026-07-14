/* Minimal probe: does a forked child still support pthread_create?
 *
 * Case A: parent creates a thread (baseline, known good).
 * Case B: a plain fork child creates a thread.
 * Case C: a fork child creates a thread after a sibling process was killed
 *         and reaped (the sequence used by the signal-owner red fixture).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void *noop_thread(void *argument)
{
    (void)argument;
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

static void report(const char *label, int status)
{
    char buffer[96];
    const int length = snprintf(buffer, sizeof(buffer),
        "FORK_PTHREAD_%s=%s rc=%d\n", label, status == 0 ? "OK" : "FAIL", status);
    if (length > 0) (void)write(STDOUT_FILENO, buffer, (size_t)length);
}

int main(void)
{
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

    (void)write(STDOUT_FILENO, "FORK_PTHREAD_DONE\n", 18);
    return 0;
}
