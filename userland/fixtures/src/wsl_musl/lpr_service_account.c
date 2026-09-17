#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "LPR_SERVICE_ACCOUNT=FAIL line=%d errno=%d\n", __LINE__, errno); exit(1); } } while (0)
static atomic_int stop_readers, reader_failed;
static void *reader(void *unused)
{
    (void)unused;
    while (!atomic_load(&stop_readers)) {
        uid_t r,e,s;
        if (getresuid(&r,&e,&s) || r != e || e != s || (r != 81 && r != 1000))
            atomic_store(&reader_failed, 1);
    }
    return NULL;
}
static void check_peer(int fd, unsigned uid, unsigned gid)
{
    struct ucred c;
    socklen_t size = sizeof(c);
    CHECK(!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &c, &size));
    CHECK(size == sizeof(c) && c.uid == uid && c.gid == gid && c.pid == getpid());
}
static void check_rights(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd < 0); /* No netd client in this service's launch specification. */
}
static void check_groups(unsigned count, gid_t first)
{
    gid_t groups[4];
    CHECK(getgroups(4, groups) == (int)count);
    CHECK(!count || groups[0] == first);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    check_rights();
    if (!strcmp(argv[1], "no-grants")) {
        CHECK(getuid() == 0 && getgid() == 0);
        CHECK(open("/etc/passwd", O_RDONLY) < 0);
        CHECK(setuid(81) == -1 && errno == EPERM);
        CHECK(setgid(81) == -1 && errno == EPERM);
        CHECK(setgroups(0, NULL) == -1 && errno == EPERM);
        puts("LPR_SERVICE_NO_GRANTS=OK");
        return 0;
    }
    if (!strcmp(argv[1], "restricted")) {
        CHECK(getuid() == 1000 && getgid() == 1000);
        check_groups(1, 100);
        int fd = open("/etc/passwd", O_RDONLY);
        CHECK(fd >= 0); close(fd);
        CHECK(open("/tmp/lpr-service-forbidden", O_CREAT|O_WRONLY, 0600) < 0);
        CHECK(setuid(0) == -1 && errno == EPERM);
        CHECK(setgid(0) == -1 && errno == EPERM);
        CHECK(!setreuid(-1,1000) && !setregid(-1,1000));
        puts("LPR_SERVICE_RESTRICTED=OK");
        return 0;
    }
    if (!strcmp(argv[1], "exec")) {
        CHECK(getuid() == 1000 && getgid() == 1000);
        check_groups(1, 100);
        CHECK(!setuid(81) && !setuid(1000)); /* Explicit authority survives exec. */
        puts("LPR_SERVICE_EXEC=OK");
        return 0;
    }
    CHECK(!strcmp(argv[1], "transitions"));
    struct passwd *account = getpwnam("messagebus");
    CHECK(account && getuid() == account->pw_uid && getgid() == account->pw_gid);
    CHECK(getuid() == 81 && geteuid() == 81 && getegid() == 81);
    check_groups(0, 0);
    int old[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, old));
    check_peer(old[0],81,81);
    int enabled = 1;
    CHECK(!setsockopt(old[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)));
    CHECK(!initgroups("user", 1000));
    CHECK(!setgid(1000) && !setuid(1000));
    /* initgroups includes the requested base GID as well as supplementary IDs. */
    CHECK(getuid() == 1000 && getgid() == 1000);
    check_peer(old[0],81,81); /* Existing connection snapshot is immutable. */
    CHECK(write(old[0], "x", 1) == 1);
    char byte, control[CMSG_SPACE(sizeof(struct ucred))];
    struct iovec iov = {&byte,1};
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    CHECK(recvmsg(old[1], &msg, 0) == 1);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    CHECK(cmsg && cmsg->cmsg_type == SCM_CREDENTIALS);
    struct ucred sent; memcpy(&sent, CMSG_DATA(cmsg), sizeof(sent));
    CHECK(sent.uid == 1000 && sent.gid == 1000 && sent.pid == getpid());
    close(old[0]); close(old[1]);
    int fresh[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fresh));
    check_peer(fresh[0],1000,1000);
    close(fresh[0]); close(fresh[1]);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/lpr-service-%d.sock", getpid());
    int listener = socket(AF_UNIX, SOCK_STREAM, 0), client = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listener >= 0 && client >= 0);
    CHECK(!bind(listener, (void *)&address, sizeof(address)) && !listen(listener,1));
    CHECK(!connect(client,(void *)&address,sizeof(address)));
    int accepted = accept(listener,NULL,NULL); CHECK(accepted >= 0);
    check_peer(accepted,1000,1000);
    close(accepted); close(listener); close(client); CHECK(!unlink(address.sun_path));
    /* Existing filed/unixd sessions have already been issued before changes. */
    CHECK(!setresuid(81,1000,0));
    uid_t r,e,s; CHECK(!getresuid(&r,&e,&s) && r == 81 && e == 1000 && s == 0);
    CHECK(!setreuid(1000,81));
    CHECK(!getresuid(&r,&e,&s) && r == 1000 && e == 81 && s == 81);
    CHECK(!setuid(1000));
    pthread_t readers[4];
    for (unsigned i = 0; i < 4; i++) CHECK(!pthread_create(&readers[i],NULL,reader,NULL));
    /* Exercise musl's all-thread synchronization as well as manager readers. */
    for (unsigned i = 0; i < 8; i++) CHECK(!setuid(i & 1 ? 1000 : 81));
    atomic_store(&stop_readers, 1);
    for (unsigned i = 0; i < 4; i++) CHECK(!pthread_join(readers[i],NULL));
    CHECK(!atomic_load(&reader_failed));
    gid_t groups[] = {100};
    CHECK(!setgroups(1,groups));
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(getuid() == 1000 && getgid() == 1000); check_groups(1,100);
        execl(argv[0], argv[0], "exec", NULL); _exit(2);
    }
    int status; CHECK(waitpid(child,&status,0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(getuid() == 1000 && getgid() == 1000); check_groups(1,100);
    puts("LPR_SERVICE_TRANSITIONS=OK");
    return 0;
}
