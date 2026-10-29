/* SPDX-License-Identifier: MIT */
#include "progress_protocol.h"
#include "../kobox2_owner/bootfs.h"
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>

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

/* Read the root identity from the hash-verified ordinary boot archive.
 * Formatting must create the UUID storage_boot will search for. */
static int print_root_uuid(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return 1;
    if (fstat(fd, &st) || st.st_size < 104 || st.st_size > 1024 * 1024 * 1024) {
        close(fd); return 1;
    }
    void *image = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (image == MAP_FAILED) return 1;
    struct ph_bootfs archive;
    const unsigned char *uuid = NULL;
    size_t size = 0;
    int status = ph_bootfs_view(&archive, image, (size_t)st.st_size);
    if (!status) status = ph_bootfs_find(&archive, "/etc/pacha/root-uuid", &uuid, &size);
    unsigned nonzero = 0;
    if (!status && size == 16) for (unsigned i = 0; i < 16; ++i) nonzero |= uuid[i];
    if (!status && size == 16 && nonzero) {
        for (unsigned i = 0; i < 16; ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) putchar('-');
            printf("%02x", uuid[i]);
        }
        putchar('\n');
    } else status = 1;
    munmap(image, (size_t)st.st_size);
    return status ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--root-uuid")) return print_root_uuid(argv[2]);
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
