#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define KEY_A 30
#define BTN_LEFT 272
#define REL_X 0
#define REL_Y 1
#define EV_VERSION 0x010001
#define EVIOCGVERSION _IOR('E', 0x01, int)
#define EVIOCGID _IOR('E', 0x02, struct input_id)
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGBIT(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)

struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

struct input_event {
    int64_t sec;
    int64_t usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

static int bit_is_set(const unsigned long *bits, unsigned int bit)
{
    const unsigned int per_word = 8u * sizeof(*bits);
    return (bits[bit / per_word] & (1ul << (bit % per_word))) != 0;
}

static int inspect_device(int fd, const char *expected_name, int keyboard)
{
    int version = 0;
    struct input_id id = {0};
    char name[128] = {0};
    unsigned long event_bits[2] = {0};
    unsigned long key_bits[8] = {0};
    unsigned long rel_bits[2] = {0};
    struct stat st;
    if (ioctl(fd, EVIOCGVERSION, &version) != 0 || version != EV_VERSION ||
        ioctl(fd, EVIOCGID, &id) != 0 ||
        ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 ||
        ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0 ||
        fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
        perror("evdev inspect");
        return -1;
    }
    if (strstr(name, expected_name) == NULL || id.bustype != 0x0006 ||
        !bit_is_set(event_bits, EV_SYN) || !bit_is_set(event_bits, keyboard ? EV_KEY : EV_REL)) {
        fprintf(stderr, "EVDEV_METADATA_FAIL name=%s bus=%04x events=%lx\n",
            name, id.bustype, event_bits[0]);
        return -1;
    }
    if (keyboard) {
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0 ||
            !bit_is_set(key_bits, KEY_A)) return -1;
    } else {
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) < 0 ||
            !bit_is_set(rel_bits, REL_X) || !bit_is_set(rel_bits, REL_Y) ||
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0 ||
            !bit_is_set(key_bits, BTN_LEFT)) return -1;
    }
    printf("EVDEV_METADATA_OK name=%s bus=%04x vendor=%04x product=%04x version=%04x\n",
        name, id.bustype, id.vendor, id.product, id.version);
    return 0;
}

int main(void)
{
    int fds[2] = {
        open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC),
        open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC),
    };
    if (fds[0] < 0 || fds[1] < 0) {
        perror("open evdev");
        return 1;
    }
    if (inspect_device(fds[0], "Keyboard", 1) != 0 ||
        inspect_device(fds[1], "Mouse", 0) != 0) return 1;

    printf("EVDEV_READY keyboard=event0 mouse=event1\n");
    fflush(stdout);
    int key_down = 0, key_up = 0, rel_x = 0, rel_y = 0, btn_down = 0, btn_up = 0;
    struct pollfd pollfds[2] = {
        {.fd = fds[0], .events = POLLIN},
        {.fd = fds[1], .events = POLLIN},
    };
    for (unsigned int iteration = 0; iteration < 200 &&
        !(key_down && key_up && rel_x && rel_y && btn_down && btn_up); iteration++) {
        int status = poll(pollfds, 2, 50);
        if (status < 0) {
            perror("poll evdev");
            return 1;
        }
        for (int device = 0; device < 2; device++) {
            if ((pollfds[device].revents & POLLIN) == 0) continue;
            struct input_event events[32];
            ssize_t bytes = read(fds[device], events, sizeof(events));
            if (bytes < 0 && errno == EAGAIN) continue;
            if (bytes < 0 || bytes % (ssize_t)sizeof(events[0]) != 0) {
                perror("read evdev");
                return 1;
            }
            size_t count = (size_t)bytes / sizeof(events[0]);
            for (size_t i = 0; i < count; i++) {
                const struct input_event *event = &events[i];
                printf("EVDEV_EVENT device=event%d type=%u code=%u value=%d\n",
                    device, event->type, event->code, event->value);
                if (device == 0 && event->type == EV_KEY && event->code == KEY_A) {
                    if (event->value == 1) key_down = 1;
                    if (event->value == 0) key_up = 1;
                }
                if (device == 1 && event->type == EV_REL && event->code == REL_X && event->value == 7) rel_x = 1;
                if (device == 1 && event->type == EV_REL && event->code == REL_Y && event->value == -4) rel_y = 1;
                if (device == 1 && event->type == EV_KEY && event->code == BTN_LEFT) {
                    if (event->value == 1) btn_down = 1;
                    if (event->value == 0) btn_up = 1;
                }
            }
        }
    }
    close(fds[0]);
    close(fds[1]);
    if (!(key_down && key_up && rel_x && rel_y && btn_down && btn_up)) {
        fprintf(stderr, "EVDEV_EVENT_FAIL key=%d/%d rel=%d/%d btn=%d/%d\n",
            key_down, key_up, rel_x, rel_y, btn_down, btn_up);
        return 1;
    }
    printf("EVDEV_EVENT_PASS key=30:1,0 rel=0:7,1:-4 button=272:1,0\n");
    return 0;
}
