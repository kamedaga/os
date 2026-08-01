#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif
#ifndef TIOCGPTN
#define TIOCGPTN 0x80045430
#endif

static int fail_errno(const char *operation)
{
    const int saved_errno = errno;
    fprintf(
        stderr,
        "lpr_pty_nonblock_probe: %s failed errno=%d (%s)\n",
        operation,
        saved_errno,
        strerror(saved_errno));
    return 1;
}

static int fail_message(const char *message)
{
    fprintf(stderr, "lpr_pty_nonblock_probe: %s\n", message);
    return 1;
}

int main(void)
{
    int master = -1;
    int master_dup = -1;
    int slave = -1;
    int result = 1;

    master = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        return fail_errno("open /dev/ptmx");
    }

    int unlock = 0;
    if (ioctl(master, TIOCSPTLCK, &unlock) != 0) {
        result = fail_errno("unlock ptmx");
        goto out;
    }

    unsigned int pts_index = UINT_MAX;
    if (ioctl(master, TIOCGPTN, &pts_index) != 0) {
        result = fail_errno("query pts index");
        goto out;
    }

    char slave_path[32];
    const int path_length = snprintf(
        slave_path, sizeof(slave_path), "/dev/pts/%u", pts_index);
    if (path_length < 0 || path_length >= (int)sizeof(slave_path)) {
        result = fail_message("slave path overflow");
        goto out;
    }

    slave = open(slave_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        result = fail_errno("open pts slave");
        goto out;
    }

    struct termios termios_state;
    if (tcgetattr(slave, &termios_state) != 0) {
        result = fail_errno("tcgetattr slave");
        goto out;
    }
    cfmakeraw(&termios_state);
    termios_state.c_cflag |= CLOCAL | CREAD;
    termios_state.c_cc[VMIN] = 1;
    termios_state.c_cc[VTIME] = 0;
    if (tcsetattr(slave, TCSANOW, &termios_state) != 0) {
        result = fail_errno("tcsetattr raw-ish slave");
        goto out;
    }

    master_dup = dup(master);
    if (master_dup < 0) {
        result = fail_errno("dup master");
        goto out;
    }
    const int duplicate_flags = fcntl(master_dup, F_GETFL);
    if (duplicate_flags < 0) {
        result = fail_errno("F_GETFL duplicate master");
        goto out;
    }
    if (fcntl(master_dup, F_SETFL, duplicate_flags | O_NONBLOCK) != 0) {
        result = fail_errno("F_SETFL O_NONBLOCK duplicate master");
        goto out;
    }

    const int original_flags = fcntl(master, F_GETFL);
    if (original_flags < 0) {
        result = fail_errno("F_GETFL original master");
        goto out;
    }
    if ((original_flags & O_NONBLOCK) == 0) {
        result = fail_message(
            "O_NONBLOCK set through duplicate is not visible on original master");
        goto out;
    }

    static const char payload[] = "pty-nonblock-payload";
    const size_t payload_bytes = sizeof(payload) - 1u;
    const ssize_t written = write(slave, payload, payload_bytes);
    if (written < 0) {
        result = fail_errno("write slave payload");
        goto out;
    }
    if ((size_t)written != payload_bytes) {
        result = fail_message("short slave payload write");
        goto out;
    }

    struct pollfd pollfd = {
        .fd = master,
        .events = POLLIN,
        .revents = 0,
    };
    const int poll_result = poll(&pollfd, 1, 2000);
    if (poll_result < 0) {
        result = fail_errno("poll master payload");
        goto out;
    }
    if (poll_result != 1 || (pollfd.revents & POLLIN) == 0) {
        fprintf(
            stderr,
            "lpr_pty_nonblock_probe: payload poll result=%d revents=0x%x\n",
            poll_result,
            pollfd.revents);
        goto out;
    }

    char received[sizeof(payload) - 1u];
    size_t received_bytes = 0;
    while (received_bytes < sizeof(received)) {
        const ssize_t count = read(
            master,
            received + received_bytes,
            sizeof(received) - received_bytes);
        if (count < 0) {
            result = fail_errno("read master payload");
            goto out;
        }
        if (count == 0) {
            result = fail_message("unexpected EOF while reading master payload");
            goto out;
        }
        received_bytes += (size_t)count;
    }
    if (memcmp(received, payload, payload_bytes) != 0) {
        result = fail_message("master payload mismatch");
        goto out;
    }

    printf("lpr_pty_nonblock_probe: stage=empty-read-enter\n");
    fflush(stdout);
    char empty_byte = 0;
    errno = 0;
    const ssize_t empty_result = read(master, &empty_byte, sizeof(empty_byte));
    const int empty_errno = errno;
    printf(
        "lpr_pty_nonblock_probe: stage=empty-read-return result=%zd errno=%d\n",
        empty_result,
        empty_errno);
    fflush(stdout);
    if (empty_result != -1 ||
        (empty_errno != EAGAIN && empty_errno != EWOULDBLOCK))
    {
        fprintf(
            stderr,
            "lpr_pty_nonblock_probe: empty read result=%zd errno=%d\n",
            empty_result,
            empty_errno);
        goto out;
    }

    result = 0;

out:
    if (slave >= 0 && close(slave) != 0 && result == 0) {
        result = fail_errno("close slave");
    }
    if (master_dup >= 0 && close(master_dup) != 0 && result == 0) {
        result = fail_errno("close duplicate master");
    }
    if (master >= 0 && close(master) != 0 && result == 0) {
        result = fail_errno("close master");
    }
    if (result == 0) {
        printf("lpr_pty_nonblock_probe: ok\n");
    }
    return result;
}
