#define _GNU_SOURCE

#include "input_metadata.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum {
    INPUT_EVENT_LIMIT = 192,
    INPUT_PATH_BYTES = 128,
    INPUT_UEVENT_BYTES = 1024,
    INPUT_EV_SYN = 0,
    INPUT_EV_KEY = 1,
    INPUT_EV_REL = 2,
    INPUT_EV_ABS = 3,
    INPUT_KEY_A = 30,
    INPUT_REL_X = 0,
    INPUT_REL_Y = 1,
    INPUT_ABS_X = 0,
    INPUT_ABS_Y = 1,
    INPUT_BTN_TOOL_PEN = 0x140,
    INPUT_BTN_STYLUS = 0x14b,
};

typedef struct input_roles {
    int keyboard;
    int relative;
    int absolute;
    int tablet_tool;
} input_roles_t;

static int bit_is_set(const uint8_t *bits, size_t bytes, unsigned bit)
{
    return bit / 8u < bytes && (bits[bit / 8u] & (uint8_t)(1u << (bit % 8u))) != 0;
}

static unsigned long eviocgbit(unsigned event_type, size_t bytes)
{
    return 0x80000000ul | ((unsigned long)bytes << 16) |
        ((unsigned long)'E' << 8) | (0x20u + event_type);
}

static int read_bits(int fd, unsigned event_type, uint8_t *bits, size_t bytes)
{
    memset(bits, 0, bytes);
    return ioctl(fd, eviocgbit(event_type, bytes), bits) < 0 ? -1 : 0;
}

static int classify_event_device(int fd, input_roles_t *roles)
{
    uint8_t events[8], keys[96], relative[8], absolute[8];
    int have_keys = 0;
    memset(roles, 0, sizeof(*roles));
    if (read_bits(fd, INPUT_EV_SYN, events, sizeof(events)) != 0 ||
        !bit_is_set(events, sizeof(events), INPUT_EV_SYN))
        return -1;
    if (bit_is_set(events, sizeof(events), INPUT_EV_KEY)) {
        if (read_bits(fd, INPUT_EV_KEY, keys, sizeof(keys)) != 0) return -1;
        have_keys = 1;
        roles->keyboard = bit_is_set(keys, sizeof(keys), INPUT_KEY_A);
    }
    if (bit_is_set(events, sizeof(events), INPUT_EV_REL)) {
        if (read_bits(fd, INPUT_EV_REL, relative, sizeof(relative)) != 0) return -1;
        roles->relative = bit_is_set(relative, sizeof(relative), INPUT_REL_X) &&
            bit_is_set(relative, sizeof(relative), INPUT_REL_Y);
    }
    if (bit_is_set(events, sizeof(events), INPUT_EV_ABS)) {
        if (read_bits(fd, INPUT_EV_ABS, absolute, sizeof(absolute)) != 0) return -1;
        roles->absolute = bit_is_set(absolute, sizeof(absolute), INPUT_ABS_X) &&
            bit_is_set(absolute, sizeof(absolute), INPUT_ABS_Y);
    }
    if (roles->absolute && have_keys &&
        bit_is_set(keys, sizeof(keys), INPUT_BTN_TOOL_PEN) &&
        bit_is_set(keys, sizeof(keys), INPUT_BTN_STYLUS)) {
        int x[6] = {0}, y[6] = {0};
        if (ioctl(fd, 0x80184540ul + INPUT_ABS_X, x) == 0 &&
            ioctl(fd, 0x80184540ul + INPUT_ABS_Y, y) == 0 &&
            x[5] > 0 && y[5] > 0)
            roles->tablet_tool = 1;
    }
    return 0;
}

static int append_bytes(char *out, size_t capacity, size_t *used,
    const char *bytes, size_t length)
{
    if (*used > capacity || length > capacity - *used) return -1;
    memcpy(out + *used, bytes, length);
    *used += length;
    return 0;
}

static int append_line(char *out, size_t capacity, size_t *used, const char *line)
{
    const size_t length = strlen(line);
    return append_bytes(out, capacity, used, line, length) == 0 &&
        append_bytes(out, capacity, used, "\n", 1) == 0 ? 0 : -1;
}

static int write_all(int fd, const char *bytes, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(fd, bytes + offset, length - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        offset += (size_t)written;
    }
    return 0;
}

static int publish_roles(unsigned event, const input_roles_t *roles)
{
    char path[INPUT_PATH_BYTES];
    if (snprintf(path, sizeof(path), "/sys/class/input/event%u/uevent", event) <= 0)
        return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char current[INPUT_UEVENT_BYTES];
    ssize_t length;
    do {
        length = read(fd, current, sizeof(current));
    } while (length < 0 && errno == EINTR);
    const int read_errno = errno;
    (void)close(fd);
    errno = read_errno;
    if (length < 0 || (size_t)length == sizeof(current)) return -1;

    char updated[INPUT_UEVENT_BYTES];
    size_t used = 0;
    size_t offset = 0;
    while (offset < (size_t)length) {
        const size_t start = offset;
        while (offset < (size_t)length && current[offset] != '\n') offset++;
        const size_t line_length = offset - start;
        if (offset < (size_t)length) offset++;
        if (line_length >= 8 && memcmp(current + start, "ID_INPUT", 8) == 0)
            continue;
        if (line_length != 0 &&
            (append_bytes(updated, sizeof(updated), &used,
                 current + start, line_length) != 0 ||
             append_bytes(updated, sizeof(updated), &used, "\n", 1) != 0))
            return -1;
    }
    if (append_line(updated, sizeof(updated), &used, "ID_INPUT=1") != 0 ||
        (roles->keyboard && append_line(updated, sizeof(updated), &used,
            "ID_INPUT_KEYBOARD=1") != 0) ||
        ((roles->relative || (roles->absolute && !roles->tablet_tool)) &&
            append_line(updated, sizeof(updated), &used,
            "ID_INPUT_MOUSE=1") != 0) ||
        (roles->tablet_tool && append_line(updated, sizeof(updated), &used,
            "ID_INPUT_TABLET=1") != 0))
        return -1;

    fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;
    const int status = write_all(fd, updated, used);
    const int write_errno = errno;
    const int close_status = close(fd);
    errno = write_errno;
    return status == 0 && close_status == 0 ? 0 : -1;
}

static int parse_event_name(const char *name, unsigned *event)
{
    if (strncmp(name, "event", 5) != 0 || name[5] == '\0') return 0;
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(name + 5, &end, 10);
    if (errno != 0 || end == name + 5 || *end != '\0' || value >= INPUT_EVENT_LIMIT)
        return 0;
    *event = (unsigned)value;
    return 1;
}

int pacha_prepare_input_metadata(void)
{
    DIR *directory = opendir("/dev/input");
    if (directory == NULL) return -1;
    unsigned devices = 0, keyboards = 0, relative = 0, absolute = 0;
    int status = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) status = -1;
            break;
        }
        unsigned event = 0;
        if (!parse_event_name(entry->d_name, &event)) continue;
        char path[INPUT_PATH_BYTES];
        if (snprintf(path, sizeof(path), "/dev/input/event%u", event) <= 0) {
            status = -1;
            break;
        }
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            status = -1;
            break;
        }
        input_roles_t roles;
        const int classify_status = classify_event_device(fd, &roles);
        const int classify_errno = errno;
        const int close_status = close(fd);
        errno = classify_errno;
        if (classify_status != 0 || close_status != 0 ||
            publish_roles(event, &roles) != 0) {
            status = -1;
            break;
        }
        devices++;
        keyboards += (unsigned)roles.keyboard;
        relative += (unsigned)roles.relative;
        absolute += (unsigned)roles.absolute;
    }
    const int directory_status = closedir(directory);
    if (status == 0 && directory_status != 0) status = -1;
    if (status == 0 && devices == 0) status = -1;
    if (status == 0) {
        fprintf(stderr,
            "pacha-user-session: input metadata devices=%u keyboard=%u relative=%u absolute=%u\n",
            devices, keyboards, relative, absolute);
    }
    return status;
}
