/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "pacha/native_syscall_retry.h"
#include "pachaos/abi.h"

static long call(uint64_t nr, uint64_t a, uint64_t b, uint64_t c)
{
    return pacha_native_syscall_retry6(nr, a, b, c, 0, 0, 0).status;
}

static int pause_poll(void)
{
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 200000000};
    return call(PACHAOS_SYSCALL_NANOSLEEP,
        (uintptr_t)&delay, 0, 0) == PACHA_SYSCALL_OK ? 0 : -1;
}

static int write_all(unsigned fd, const void *data, size_t length)
{
    const char *bytes = data;
    while (length) {
        long n = call(PACHAOS_SYSCALL_FD_WRITE, fd, (uintptr_t)bytes, length);
        /* Pipe errors use negative native statuses, not Linux errno. */
        if (n == -PACHA_SYSCALL_ERR_NOT_READY) {
            if (pause_poll()) return -1;
            continue;
        }
        if (n <= 0 || (uint64_t)n > length) return -1;
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}

#define MESSAGE(fd, text) write_all(fd, text, sizeof(text) - 1)

int main(int argc, char **argv)
{
    uint64_t cursor = 0, dropped = 0;
    int from_end = 0;
    if (argc == 2 && !strcmp(argv[1], "--from-end")) from_end = 1;
    else if (argc != 1 && !(argc == 2 && !strcmp(argv[1], "--from-start"))) {
        MESSAGE(2, "usage: klogtail [--from-start|--from-end]\n");
        return 2;
    }
    /* A cursor beyond the current end is clamped by the existing ring ABI.
     * This obtains one fixed startup boundary without chasing producers. */
    if (from_end) cursor = UINT64_MAX;
    for (;;) {
        struct pacha_kernel_log_read request = {
            .cursor = cursor, .limit = PACHA_KERNEL_LOG_READ_MAX_BYTES,
        };
        long status = call(PACHAOS_SYSCALL_KERNEL_LOG_READ,
            (uintptr_t)&request, sizeof(request), 0);
        if (status == PACHA_SYSCALL_ERR_EMPTY) {
            if (pause_poll()) return 4;
            continue;
        }
        if (status != PACHA_SYSCALL_OK) {
            MESSAGE(2, "klogtail: kernel log read unavailable or failed\n");
            return 3;
        }
        if (request.written > request.limit) return 3;
        if (!from_end && (request.overrun || request.dropped != dropped)) {
            if (MESSAGE(1, "\n[klogtail: kernel ring overrun or dropped fragments]\n"))
                return 4;
        }
        dropped = request.dropped;
        cursor = request.next_cursor;
        from_end = 0;
        if (write_all(1, request.bytes, (size_t)request.written)) return 4;
        if (!request.written && pause_poll()) return 4;
    }
}
