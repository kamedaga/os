#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif
#ifndef TIOCGPTN
#define TIOCGPTN 0x80045430
#endif
#ifndef TIOCGPTPEER
#define TIOCGPTPEER 0x5441
#endif
#ifndef TIOCPKT
#define TIOCPKT 0x5420
#endif
#ifndef TIOCPKT_DATA
#define TIOCPKT_DATA 0
#endif

static int fail_errno(const char *label)
{
    fprintf(stderr, "lpr_pty_probe: %s failed errno=%d (%s)\n", label, errno, strerror(errno));
    return 1;
}

static int drain_echo(int fd)
{
    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
        return fail_errno("read echo");
    }
    if (n == 0) {
        fprintf(stderr, "lpr_pty_probe: missing echo\n");
        return 6;
    }
    return 0;
}

static int expect_text(const char *label, int fd, const char *expected)
{
    char buf[64];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        return fail_errno(label);
    }
    if ((size_t)n != strlen(expected) || memcmp(buf, expected, (size_t)n) != 0) {
        fprintf(stderr, "lpr_pty_probe: %s got %zd bytes '%s'\n", label, n, buf);
        return 2;
    }
    return 0;
}

int main(void)
{
    int master = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        return fail_errno("open /dev/ptmx");
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if (ioctl(master, TCGETS, &tio) != 0) {
        return fail_errno("TCGETS master");
    }

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(master, TIOCGWINSZ, &ws) != 0) {
        return fail_errno("TIOCGWINSZ master");
    }
    if (ws.ws_row != 24 || ws.ws_col != 80) {
        fprintf(stderr, "lpr_pty_probe: unexpected winsize %u x %u\n", ws.ws_row, ws.ws_col);
        return 3;
    }

    unsigned int pts_index = UINT_MAX;
    if (ioctl(master, TIOCGPTN, &pts_index) != 0) {
        return fail_errno("TIOCGPTN master");
    }
    printf("lpr_pty_probe: TIOCGPTN index=%u\n", pts_index);

    int second_master = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (second_master < 0) {
        return fail_errno("open second /dev/ptmx");
    }
    unsigned int second_pts_index = UINT_MAX;
    if (ioctl(second_master, TIOCGPTN, &second_pts_index) != 0) {
        int saved_errno = errno;
        close(second_master);
        errno = saved_errno;
        return fail_errno("TIOCGPTN second master");
    }
    if (pts_index == UINT_MAX || second_pts_index != pts_index + 1u) {
        fprintf(stderr,
            "lpr_pty_probe: non-consecutive pts indexes %u then %u\n",
            pts_index,
            second_pts_index);
        close(second_master);
        return 18;
    }
    printf("lpr_pty_probe: TIOCGPTN secondary index=%u\n", second_pts_index);
    close(second_master);

    int unlock = 0;
    if (ioctl(master, TIOCSPTLCK, &unlock) != 0) {
        return fail_errno("TIOCSPTLCK master");
    }

    pid_t child = fork();
    if (child < 0) {
        return fail_errno("fork controlling tty probe");
    }
    if (child == 0) {
        if (setsid() < 0) {
            fprintf(stderr, "lpr_pty_probe: child setsid failed errno=%d\n", errno);
            _exit(21);
        }
        int child_peer = ioctl(
            master,
            TIOCGPTPEER,
            O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (child_peer < 0) {
            fprintf(stderr, "lpr_pty_probe: child TIOCGPTPEER failed errno=%d\n", errno);
            _exit(22);
        }
        if (ioctl(child_peer, TIOCSCTTY, 0) != 0) {
            fprintf(stderr, "lpr_pty_probe: child TIOCSCTTY failed errno=%d\n", errno);
            _exit(23);
        }
        pid_t foreground = tcgetpgrp(child_peer);
        if (foreground != getpgrp()) {
            fprintf(stderr,
                "lpr_pty_probe: child foreground pgrp=%d expected=%d errno=%d\n",
                (int)foreground,
                (int)getpgrp(),
                errno);
            _exit(24);
        }
        close(child_peer);
        _exit(0);
    }
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child) {
        return fail_errno("waitpid controlling tty probe");
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr,
            "lpr_pty_probe: controlling tty child status=0x%x\n",
            child_status);
        return 21;
    }

    int peer = ioctl(master, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (peer < 0) {
        return fail_errno("TIOCGPTPEER master");
    }
    if ((fcntl(peer, F_GETFD) & FD_CLOEXEC) == 0) {
        fprintf(stderr, "lpr_pty_probe: TIOCGPTPEER missing FD_CLOEXEC\n");
        close(peer);
        return 20;
    }
    if (close(peer) != 0) {
        return fail_errno("close TIOCGPTPEER slave");
    }

    char slave_path[32];
    if (snprintf(slave_path, sizeof(slave_path), "/dev/pts/%u", pts_index) >=
        (int)sizeof(slave_path)) {
        fprintf(stderr, "lpr_pty_probe: pts path overflow\n");
        return 19;
    }
    int slave = open(slave_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        return fail_errno("open pts slave");
    }

    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = slave;
    pfd.events = POLLIN | POLLOUT;
    int poll_status = poll(&pfd, 1, 0);
    if (poll_status < 0) {
        return fail_errno("poll slave");
    }
    if (poll_status != 1 || (pfd.revents & POLLOUT) == 0) {
        fprintf(stderr, "lpr_pty_probe: unexpected initial poll revents=0x%x\n", pfd.revents);
        return 4;
    }

    if ((tio.c_lflag & ICANON) == 0 || (tio.c_lflag & ECHO) == 0) {
        fprintf(stderr, "lpr_pty_probe: default termios missing ICANON/ECHO lflag=0x%lx\n", (unsigned long)tio.c_lflag);
        return 5;
    }

    const char canonical_payload[] = "ab\177c\n";
    const char canonical_expected[] = "ac\n";
    if (write(master, canonical_payload, sizeof(canonical_payload) - 1) != (ssize_t)(sizeof(canonical_payload) - 1)) {
        return fail_errno("write master");
    }
    int status = drain_echo(master);
    if (status != 0) {
        return status;
    }

    int available = 0;
    if (ioctl(slave, FIONREAD, &available) != 0) {
        return fail_errno("FIONREAD slave");
    }
    if (available != (int)(sizeof(canonical_expected) - 1)) {
        fprintf(stderr, "lpr_pty_probe: slave FIONREAD=%d\n", available);
        return 7;
    }
    status = expect_text("read slave canonical", slave, canonical_expected);
    if (status != 0) {
        return status;
    }

    const char intr_payload[] = "\003";
    if (write(master, intr_payload, sizeof(intr_payload) - 1) != (ssize_t)(sizeof(intr_payload) - 1)) {
        return fail_errno("write master intr");
    }
    status = drain_echo(master);
    if (status != 0) {
        return status;
    }
    available = -1;
    if (ioctl(slave, FIONREAD, &available) != 0) {
        return fail_errno("FIONREAD slave after intr");
    }
    if (available != 0) {
        fprintf(stderr, "lpr_pty_probe: slave FIONREAD after intr=%d\n", available);
        return 8;
    }
    char intr_buf[8];
    errno = 0;
    ssize_t intr_read = read(slave, intr_buf, sizeof(intr_buf));
    if (intr_read != -1 || errno != EINTR) {
        fprintf(stderr, "lpr_pty_probe: intr read=%zd errno=%d\n", intr_read, errno);
        return 9;
    }

    tio.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    if (ioctl(slave, TCSETS, &tio) != 0) {
        return fail_errno("TCSETS raw-ish slave");
    }

    const char master_payload[] = "master-to-slave";
    if (write(master, master_payload, sizeof(master_payload) - 1) != (ssize_t)(sizeof(master_payload) - 1)) {
        return fail_errno("write master raw");
    }
    status = expect_text("read slave raw", slave, master_payload);
    if (status != 0) {
        return status;
    }

    const char slave_payload[] = "slave-to-master";
    if (write(slave, slave_payload, sizeof(slave_payload) - 1) != (ssize_t)(sizeof(slave_payload) - 1)) {
        return fail_errno("write slave");
    }
    status = expect_text("read master raw", master, slave_payload);
    if (status != 0) {
        return status;
    }

    int packet_mode = 1;
    if (ioctl(master, TIOCPKT, &packet_mode) != 0) {
        return fail_errno("TIOCPKT enable master");
    }
    const char packet_payload[] = "packet-mode";
    if (write(slave, packet_payload, sizeof(packet_payload) - 1) !=
        (ssize_t)(sizeof(packet_payload) - 1))
    {
        return fail_errno("write slave packet mode");
    }
    char packet_buf[sizeof(packet_payload)];
    memset(packet_buf, 0xff, sizeof(packet_buf));
    const ssize_t packet_read = read(master, packet_buf, sizeof(packet_buf));
    if (packet_read != (ssize_t)sizeof(packet_buf) ||
        (unsigned char)packet_buf[0] != TIOCPKT_DATA ||
        memcmp(packet_buf + 1, packet_payload, sizeof(packet_payload) - 1) != 0)
    {
        fprintf(stderr,
            "lpr_pty_probe: TIOCPKT data read=%zd header=%u\n",
            packet_read,
            (unsigned)(unsigned char)packet_buf[0]);
        return 25;
    }
    packet_mode = 0;
    if (ioctl(master, TIOCPKT, &packet_mode) != 0) {
        return fail_errno("TIOCPKT disable master");
    }
    printf("lpr_pty_probe: TIOCPKT data ok\n");

    if (close(slave) != 0) {
        return fail_errno("close slave");
    }
    if (close(master) != 0) {
        return fail_errno("close master");
    }

    printf("lpr_pty_probe: ok\n");
    return 0;
}
