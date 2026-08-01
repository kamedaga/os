#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EV_SYN 0
#define EV_ABS 3
#define SYN_REPORT 0
#define SYN_DROPPED 3
#define ABS_X 0
#define ABS_Y 1
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGABS(axis) _IOR('E', 0x40 + (axis), struct input_absinfo)

struct input_absinfo {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
};

struct input_event {
    int64_t sec;
    int64_t usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

static int read_frame(int fd, int expected_x, int expected_y,
    int expect_x_event, int expect_y_event)
{
    int seen_x = 0;
    int seen_y = 0;
    int value_x = 0;
    int value_y = 0;
    struct pollfd ready = {.fd = fd, .events = POLLIN};

    for (unsigned int attempt = 0; attempt < 200; attempt++) {
        ready.revents = 0;
        const int status = poll(&ready, 1, 50);
        if (status < 0) {
            perror("poll tablet");
            return -1;
        }
        if (status == 0 || (ready.revents & POLLIN) == 0) continue;

        struct input_event events[32];
        const ssize_t bytes = read(fd, events, sizeof(events));
        if (bytes < 0 && errno == EAGAIN) continue;
        if (bytes < 0 || bytes % (ssize_t)sizeof(events[0]) != 0) {
            perror("read tablet");
            return -1;
        }
        const size_t count = (size_t)bytes / sizeof(events[0]);
        for (size_t i = 0; i < count; i++) {
            const struct input_event *event = &events[i];
            if (event->type == EV_SYN && event->code == SYN_DROPPED) {
                fprintf(stderr, "TABLET_FRAME_FAIL unexpected_syn_dropped=1\n");
                return -1;
            }
            if (event->type == EV_ABS && event->code == ABS_X) {
                seen_x++;
                value_x = event->value;
            }
            if (event->type == EV_ABS && event->code == ABS_Y) {
                seen_y++;
                value_y = event->value;
            }
            if (event->type == EV_SYN && event->code == SYN_REPORT) {
                if (seen_x != expect_x_event || seen_y != expect_y_event ||
                    (seen_x != 0 && value_x != expected_x) ||
                    (seen_y != 0 && value_y != expected_y)) {
                    fprintf(stderr,
                        "TABLET_FRAME_FAIL seen=%d/%d value=%d/%d expected=%d/%d\n",
                        seen_x, seen_y, value_x, value_y, expected_x, expected_y);
                    return -1;
                }
                return 0;
            }
        }
    }
    fprintf(stderr, "TABLET_FRAME_FAIL timeout=1\n");
    return -1;
}

static int check_state(int fd, int expected_x, int expected_y)
{
    struct input_absinfo x = {0};
    struct input_absinfo y = {0};
    if (ioctl(fd, EVIOCGABS(ABS_X), &x) != 0 ||
        ioctl(fd, EVIOCGABS(ABS_Y), &y) != 0) {
        perror("EVIOCGABS tablet");
        return -1;
    }
    if (x.value != expected_x || y.value != expected_y ||
        x.minimum != 0 || y.minimum != 0 ||
        x.maximum != 32767 || y.maximum != 32767) {
        fprintf(stderr,
            "TABLET_STATE_FAIL value=%d/%d range=%d..%d/%d..%d expected=%d/%d\n",
            x.value, y.value, x.minimum, x.maximum, y.minimum, y.maximum,
            expected_x, expected_y);
        return -1;
    }
    return 0;
}

int main(void)
{
    const int fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror("open tablet");
        return 1;
    }
    char name[128] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 ||
        strstr(name, "Tablet") == NULL) {
        fprintf(stderr, "TABLET_METADATA_FAIL name=%s\n", name);
        return 1;
    }
    printf("TABLET_STATE_READY name=%s\n", name);
    fflush(stdout);

    if (read_frame(fd, 4096, 8192, 1, 1) != 0 ||
        check_state(fd, 4096, 8192) != 0) return 1;
    printf("TABLET_FIRST_PASS state=4096,8192\n");
    printf("TABLET_SECOND_READY unchanged_x=4096\n");
    fflush(stdout);

    if (read_frame(fd, 4096, 16384, 0, 1) != 0 ||
        check_state(fd, 4096, 16384) != 0) return 1;
    printf("TABLET_STATE_PASS state=4096,16384 duplicate_x_filtered=1\n");
    close(fd);
    return 0;
}
