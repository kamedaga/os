#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void write_all(int fd, const char *data, size_t len)
{
    while (len != 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return;
        }
        data += (size_t)n;
        len -= (size_t)n;
    }
}

static void write_text(int fd, const char *text)
{
    write_all(fd, text, strlen(text));
}

static int open_hvc_with_retry(void)
{
    enum {
        max_attempts = 200,
        retry_delay_ms = 25,
    };
    int last_hvc_errno = 0;
    int last_console_errno = 0;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int fd = open("/dev/hvc0", O_RDWR);
        if (fd >= 0) {
            return fd;
        }
        int saved_errno = errno;
        last_hvc_errno = saved_errno;

        fd = open("/dev/console", O_RDWR);
        if (fd >= 0) {
            return fd;
        }
        last_console_errno = errno;

        if (saved_errno != ENOENT &&
            saved_errno != ENODEV &&
            saved_errno != EINVAL &&
            saved_errno != EAGAIN)
        {
            errno = saved_errno;
            return -1;
        }

        (void)poll(NULL, 0, retry_delay_ms);
    }

    dprintf(
        2,
        "lpr_hvc_console: hvc retry timeout hvc_errno=%d console_errno=%d\n",
        last_hvc_errno,
        last_console_errno);
    errno = ETIMEDOUT;
    return -1;
}

int main(void)
{
    int fd = open_hvc_with_retry();
    if (fd < 0) {
        dprintf(2, "lpr_hvc_console: open hvc failed errno=%d\n", errno);
        return 1;
    }

    write_text(fd, "PachaOS hvc0 ready\n> ");

    char line[192];
    size_t len = 0;
    for (;;) {
        char ch = 0;
        ssize_t n = read(fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                struct pollfd pfd = {
                    .fd = fd,
                    .events = POLLIN,
                };
                (void)poll(&pfd, 1, 100);
                continue;
            }
            return 2;
        }
        if (n == 0) {
            continue;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            line[len] = 0;
            write_text(fd, "\n");
            if (strcmp(line, "exit") == 0) {
                write_text(fd, "PachaOS hvc0 bye\n");
                return 0;
            }
            write_text(fd, "pacha-hvc received: ");
            write_all(fd, line, len);
            write_text(fd, "\n> ");
            len = 0;
            continue;
        }
        if ((unsigned char)ch == 0x7f || ch == '\b') {
            if (len != 0) {
                len--;
            }
            continue;
        }
        if (len + 1 < sizeof(line)) {
            line[len++] = ch;
        }
    }
}
