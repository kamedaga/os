#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int check(int nonzero)
{
    const uid_t uid = nonzero ? 1001 : 0, euid = nonzero ? 1002 : 0, suid = nonzero ? 1003 : 0;
    const gid_t gid = nonzero ? 2001 : 0, egid = nonzero ? 2002 : 0, sgid = nonzero ? 2003 : 0;
    uid_t r, e, s;
    gid_t gr, ge, gs;
    if (getuid() != uid || geteuid() != euid || getgid() != gid || getegid() != egid ||
        getresuid(&r, &e, &s) || getresgid(&gr, &ge, &gs) ||
        r != uid || e != euid || s != suid || gr != gid || ge != egid || gs != sgid) return 1;
    if (setresuid(-1, -1, -1) || setresgid(-1, -1, -1) ||
        setresuid(uid, euid, suid) || setresgid(gid, egid, sgid)) return 2;
    errno = 0;
    if (setuid(nonzero ? 0 : 1) != -1 || errno != EPERM) return 3;
    errno = 0;
    if (setgid(nonzero ? 0 : 1) != -1 || errno != EPERM) return 4;
    errno = 0;
    if (setresuid(-1, nonzero ? 0 : 1, -1) != -1 || errno != EPERM) return 5;
    errno = 0;
    if (setresgid(-1, nonzero ? 0 : 1, -1) != -1 || errno != EPERM) return 6;
    if (getuid() != uid || geteuid() != euid || getgid() != gid || getegid() != egid) return 7;
    errno = 0;
    if (syscall(SYS_getresuid, 0, &e, &s) != -1 || errno != EFAULT) return 8;
    errno = 0;
    if (syscall(SYS_getresgid, &gr, &ge, UINT64_MAX) != -1 || errno != EFAULT) return 9;
    struct { uint32_t version; int32_t pid; } header = {0x20080522u, 0};
    struct { uint32_t effective, permitted, inheritable; } caps[2] = {{0}, {0}};
    if (syscall(SYS_capget, &header, caps) || caps[0].effective || caps[0].permitted ||
        caps[0].inheritable || caps[1].effective || caps[1].permitted || caps[1].inheritable) return 10;
    caps[0].effective = caps[0].permitted = 1;
    errno = 0;
    if (syscall(SYS_capset, &header, caps) != -1 || errno != EPERM) return 11;
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 12;
    struct ucred peer;
    socklen_t size = sizeof(peer);
    int status = getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, &peer, &size);
    if (!status && (size != sizeof(peer) || peer.pid != getpid() || peer.uid != euid || peer.gid != egid))
        status = -1;
    close(pair[0]); close(pair[1]);
    if (status) return 13;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/lpr-credentials-%d.sock", getpid());
    (void)unlink(address.sun_path);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0 || client < 0 || bind(server, (void *)&address, sizeof(address)) ||
        listen(server, 1) || connect(client, (void *)&address, sizeof(address))) return 14;
    int accepted = accept(server, NULL, NULL);
    if (accepted < 0) return 15;
    close(accepted); close(client); close(server);
    if (unlink(address.sun_path)) return 16;
    return 0;
}

int main(int argc, char **argv)
{
    const int nonzero = argc > 1 && !strcmp(argv[1], "--nonzero");
    const int child_exec = argc > 2 && !strcmp(argv[2], "--exec-child");
    int status = check(nonzero);
    if (status) {
        fprintf(stderr, "LPR_CREDENTIALS=FAIL stage=%s check=%d errno=%d\n",
            child_exec ? "exec" : "parent", status, errno);
        return status;
    }
    if (child_exec) {
        char byte;
        if (argc < 4 || pread(atoi(argv[3]), &byte, 1, 0) != 1) return 24;
        close(atoi(argv[3]));
        puts("LPR_CREDENTIALS_EXEC=OK");
        return 0;
    }
    int inherited = open("/etc/os-release", O_RDONLY);
    if (inherited < 0) return 25;
    const pid_t pid = fork();
    if (pid < 0) return 20;
    if (!pid) {
        char byte;
        if (pread(inherited, &byte, 1, 0) != 1) _exit(26);
        status = check(nonzero);
        if (status) {
            fprintf(stderr, "LPR_CREDENTIALS=FAIL stage=fork check=%d\n", status);
            _exit(status);
        }
        char inherited_arg[24];
        snprintf(inherited_arg, sizeof(inherited_arg), "%d", inherited);
        char *args[] = {argv[0], nonzero ? "--nonzero" : "--root", "--exec-child", inherited_arg, NULL};
        execv(argv[0], args);
        _exit(21);
    }
    int wait_status;
    if (waitpid(pid, &wait_status, 0) != pid || !WIFEXITED(wait_status) || WEXITSTATUS(wait_status)) return 22;
    if (check(nonzero)) return 23;
    close(inherited);
    printf("LPR_CREDENTIALS=OK mode=%s getters real-effective-saved fork exec peercred pathname inherited-file no-escalation\n",
        nonzero ? "nonzero" : "root");
    return 0;
}
