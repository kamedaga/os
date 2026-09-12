#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BUS "unix:abstract=pacha-account-test"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "REAL_SERVICE=FAIL mode=%s line=%d errno=%d\n", mode, __LINE__, errno); exit(1); } } while (0)
static const char *mode;
static unsigned expected_uid;
static pid_t listener_pid;
static const char *bus = BUS;
static int system_bus;

/* One TTY write per record; printf's multiple writev fragments can interleave
 * when the independently launched account probes finish simultaneously. */
static void record(const char *format, ...)
{
    char line[4608];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    CHECK(length > 0 && (size_t)length < sizeof(line));
    CHECK(write(STDOUT_FILENO, line, (size_t)length) == length);
}

static int connect_bus(void)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    const char name[] = "pacha-account-test";
    socklen_t address_length;
    if (system_bus) {
        strcpy(address.sun_path, "/run/dbus/system_bus_socket");
        address_length = offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1;
    } else {
        memcpy(address.sun_path + 1, name, sizeof(name) - 1);
        address_length = offsetof(struct sockaddr_un, sun_path) + sizeof(name);
    }
    for (unsigned retry = 0; retry < 200; retry++) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(fd >= 0);
        if (!connect(fd, (void *)&address, address_length)) return fd;
        int error = errno;
        close(fd);
        CHECK(error == ECONNREFUSED || error == ENOENT);
        struct timespec delay = { .tv_nsec = 50000000 };
        nanosleep(&delay, NULL);
    }
    CHECK(0);
    return -1;
}

static void check_peer(void)
{
    int fd = connect_bus();
    struct ucred peer;
    socklen_t length = sizeof(peer);
    CHECK(!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length));
    CHECK(length == sizeof(peer) && peer.uid == 81 && peer.gid == 81 && peer.pid > 1);
    /* dbus-daemon listens before --fork. The kernel-compatible peer PID is
     * the listener's creation-time PID, not necessarily its surviving child. */
    listener_pid = peer.pid;
    record("REAL_SERVICE_PEER mode=%s uid=%u gid=%u pid=%d\n", mode, peer.uid, peer.gid, peer.pid);
    close(fd);
}

static void check_forgery(void)
{
    int fd = connect_bus();
    /* 3831 is EXTERNAL's hex encoding of "81". The real caller is user/root.
     * Neither that claim nor UID zero may impersonate the service account. */
    const char auth[] = "\0AUTH EXTERNAL 3831\r\n";
    CHECK(write(fd, auth, sizeof(auth) - 1) == sizeof(auth) - 1);
    struct pollfd watched = { .fd = fd, .events = POLLIN };
    CHECK(poll(&watched, 1, 6000) > 0);
    char response[256] = {0};
    ssize_t n = read(fd, response, sizeof(response) - 1);
    CHECK(n == 0 || (n > 0 && !strncmp(response, "REJECTED", 8)) || (n < 0 && errno == ECONNRESET));
    record("REAL_SERVICE_FORGERY mode=%s claimed=81 rejected=1\n", mode);
    close(fd);
}

static void call_real_client(int allowed, int query_pid)
{
    int output[2]; CHECK(!pipe2(output, O_CLOEXEC));
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(getuid() == expected_uid && geteuid() == expected_uid && getgid() == expected_uid);
        gid_t groups[4];
        CHECK(getgroups(4, groups) == (expected_uid == 1000 ? 1 : 0));
        if (expected_uid == 1000) CHECK(groups[0] == 100);
        CHECK(dup2(output[1], 1) == 1 && dup2(output[1], 2) == 2);
        close(output[0]); close(output[1]);
        execl("/usr/bin/gdbus", "gdbus", "call", "--address", bus,
            "--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus",
            "--method", query_pid ? "org.freedesktop.DBus.GetConnectionUnixProcessID" :
                "org.freedesktop.DBus.GetConnectionUnixUser", "org.freedesktop.DBus", NULL);
        _exit(127);
    }
    close(output[1]);
    char response[4096] = {0};
    size_t used = 0;
    for (;;) {
        struct pollfd watched = { .fd = output[0], .events = POLLIN };
        int ready = poll(&watched, 1, 10000);
        if (ready < 0 && errno == EINTR) continue;
        CHECK(ready > 0);
        ssize_t n = read(output[0], response + used, sizeof(response) - 1 - used);
        if (!n) break;
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0);
        used += (size_t)n;
        CHECK(used < sizeof(response) - 1);
    }
    close(output[0]);
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
    record("REAL_SERVICE_CLIENT mode=%s query=%s exit=%d output=%s\n",
        mode, query_pid ? "pid" : "uid", WEXITSTATUS(status), response);
    if (allowed && query_pid) {
        const char *number = strstr(response, "uint32 ");
        CHECK(!WEXITSTATUS(status) && number);
        long daemon_pid = strtol(number + 7, NULL, 10);
        CHECK(daemon_pid > listener_pid && daemon_pid != child && daemon_pid != getpid());
        record("REAL_SERVICE_DAEMON_FORK=OK listener_pid=%d daemon_pid=%ld\n", listener_pid, daemon_pid);
    } else if (allowed) CHECK(!WEXITSTATUS(status) && strstr(response, "uint32 81"));
    else CHECK(WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 127 &&
        (strstr(response, "closed") || strstr(response, "AccessDenied") || strstr(response, "rejected") ||
         strstr(response, "Exhausted all available authentication mechanisms")));
    CHECK(getuid() == expected_uid && geteuid() == expected_uid);
}

int main(int argc, char **argv)
{
    mode = argc == 2 ? argv[1] : "missing";
    if (!strcmp(mode, "system")) {
        system_bus = 1;
        bus = "unix:path=/run/dbus/system_bus_socket";
        expected_uid = 0;
        CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0);
        check_peer();
        call_real_client(1, 0);
        record("SYSTEM_BUS=OK peer_uid=81 peer_gid=81 authenticated_root_client=1\n");
        return 0;
    }
    const int allowed = !strcmp(mode, "allowed");
    CHECK(allowed || !strcmp(mode, "denied-user") || !strcmp(mode, "denied-root"));
    expected_uid = allowed ? 81 : !strcmp(mode, "denied-user") ? 1000 : 0;
    CHECK(getuid() == expected_uid && geteuid() == expected_uid && getgid() == expected_uid);
    CHECK(open("/tmp/real-service-forbidden", O_CREAT | O_WRONLY, 0600) < 0);
    CHECK(setuid(allowed ? 0 : 81) == -1 && errno == EPERM);
    check_peer();
    if (!allowed) check_forgery();
    call_real_client(allowed, 0);
    /* Verify the daemon still accepts its authorized account after forking
     * and executing a real GLib client, not just an artificial socket probe. */
    if (allowed) call_real_client(1, 1);
    record("REAL_SERVICE_%s=OK\n", allowed ? "ALLOWED" : expected_uid ? "DENIED_USER" : "DENIED_ROOT");
    return 0;
}
