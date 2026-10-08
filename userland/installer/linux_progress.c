/* SPDX-License-Identifier: MIT */
#include "progress_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int parse_u64(const char *text, uint64_t *value) {
    if (!text || !*text || !value || *text == '-') return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || !end || *end) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

int main(int argc, char **argv) {
    uint64_t phase = 0, completed = 0, total = 0;
    if (argc != 5 || parse_u64(argv[2], &phase) ||
        parse_u64(argv[3], &completed) || parse_u64(argv[4], &total) ||
        phase < INSTALLER_PROGRESS_PREFLIGHT_FILES ||
        phase > INSTALLER_PROGRESS_COMPLETE || phase > UINT32_MAX ||
        (total && completed > total)) {
        fputs("usage: pacha-install-progress DEVICE PHASE DONE TOTAL\n",
            stderr);
        return 64;
    }
    int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open installer progress device"); return 1; }
    const struct installer_progress_wire wire = {
        .magic = INSTALLER_PROGRESS_MAGIC,
        .completed_bytes = completed,
        .total_bytes = total,
        .phase = (uint32_t)phase,
    };
    int status = ioctl(fd, (unsigned long)INSTALLER_PROGRESS_IOCTL, &wire);
    if (status) perror("report installer progress");
    if (close(fd) && !status) { perror("close installer progress device"); status = -1; }
    return status ? 1 : 0;
}
