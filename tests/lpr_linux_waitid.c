#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    siginfo_t info;
    assert(waitid(P_PIDFD, INT_MAX, NULL, WEXITED | WNOHANG) == -1);
    assert(errno == EINVAL || errno == EBADF);
    int ready[2];
    uid_t expected_uid = getuid();
    assert(pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(ready[1]);
        char c;
        assert(read(ready[0], &c, 1) == 1);
        _exit(42);
    }
    close(ready[0]);
    memset(&info, 0xa5, sizeof(info));
    assert(waitid(P_PID, child, &info, WEXITED | WNOHANG) == 0);
    assert(info.si_pid == 0 && info.si_signo == 0);
    assert(write(ready[1], "R", 1) == 1);
    close(ready[1]);
    assert(waitid(P_PID, child, &info, WEXITED | WNOWAIT) == 0);
    assert(info.si_pid == child && info.si_signo == SIGCHLD);
    assert(info.si_code == CLD_EXITED && info.si_status == 42);
    assert(info.si_uid == expected_uid);
    assert(waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT) == 0);
    assert(info.si_pid == child && info.si_status == 42);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 42);
    assert(waitid(P_PID, child, &info, WEXITED | WNOHANG) == -1 && errno == ECHILD);
    puts("WAITID=PASS nohang repeated-peek WNOWAIT wait4-reap child-status actual-uid pidfd-probe");
    return 0;
}
