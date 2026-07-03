#include "bootstrap.h"

#include "pacha/abi.h"
#include "pacha/ipc.h"

#include <stdio.h>
#include <string.h>

static const char *const koboxd_expected_modules[] = {
    "nvme-auth.ko",
    "nvme-core.ko",
    "nvme.ko",
    "crc16.ko",
    "mbcache.ko",
    "jbd2.ko",
    "ext4.ko",
};

int koboxd_align_image_size(uint64_t size, uint64_t *out_size)
{
    if (out_size == NULL || size == 0 || size > UINT64_MAX - (KOBOXD_PAGE_SIZE - 1)) {
        return -1;
    }
    *out_size = (size + (KOBOXD_PAGE_SIZE - 1)) & ~(uint64_t)(KOBOXD_PAGE_SIZE - 1);
    return 0;
}

int koboxd_find_bootstrap_fd(char **argv, int *out_bootstrap_fd)
{
    if (argv == NULL || out_bootstrap_fd == NULL) {
        return -1;
    }
    *out_bootstrap_fd = -1;

    char **p = argv;
    while (*p != NULL) {
        p++;
    }
    p++;
    while (*p != NULL) {
        p++;
    }
    p++;

    uint64_t bootstrap_fd = 0;
    const uint64_t *auxv = (const uint64_t *)(const void *)p;
    for (unsigned i = 0; i < 64; i++) {
        const uint64_t type = auxv[i * 2u];
        const uint64_t value = auxv[i * 2u + 1u];
        if (type == 0) {
            break;
        }
        if (type == PACHA_AT_BOOTSTRAP_FD) {
            bootstrap_fd = value;
        }
    }
    if (bootstrap_fd < 16) {
        return -2;
    }
    *out_bootstrap_fd = (int)bootstrap_fd;
    return 0;
}

int koboxd_read_bootstrap_fd(int fd, koboxd_bootstrap_t *out_bootstrap)
{
    if (fd < 16 || out_bootstrap == NULL) {
        return -1;
    }
    const long got = pacha_fd_read(fd, out_bootstrap, sizeof(*out_bootstrap));
    if (got != (long)sizeof(*out_bootstrap)) {
        fprintf(stderr,
            "[filed-storage] bootstrap fd read failed fd=%d got=%ld size=%llu\n",
            fd,
            got,
            (unsigned long long)sizeof(*out_bootstrap));
        return -2;
    }
    return 0;
}

int koboxd_validate_bootstrap_package(
    const koboxd_bootstrap_t *bootstrap,
    uint64_t bootstrap_size)
{
    if (bootstrap == NULL || bootstrap_size < sizeof(*bootstrap)) {
        return -1;
    }
    const uint64_t expected_count =
        sizeof(koboxd_expected_modules) / sizeof(koboxd_expected_modules[0]);
    if (bootstrap->magic != KOBOXD_BOOTSTRAP_MAGIC ||
        bootstrap->device_fd < 16 ||
        bootstrap->control_fd < 16 ||
        bootstrap->module_count != expected_count ||
        bootstrap->module_count > KOBOXD_BOOTSTRAP_MAX_MODULES)
    {
        fprintf(stderr,
            "[filed-storage] bootstrap invalid magic=0x%llx device_fd=%llu control_fd=%llu modules=%llu size=%llu\n",
            (unsigned long long)bootstrap->magic,
            (unsigned long long)bootstrap->device_fd,
            (unsigned long long)bootstrap->control_fd,
            (unsigned long long)bootstrap->module_count,
            (unsigned long long)bootstrap_size);
        return -1;
    }

    for (uint64_t i = 0; i < bootstrap->module_count; i++) {
        const koboxd_bootstrap_module_t *module = &bootstrap->modules[i];
        if (strncmp(module->name, koboxd_expected_modules[i], KOBOXD_BOOTSTRAP_NAME_BYTES) != 0 ||
            module->image_fd < 16 ||
            module->image_size < 4)
        {
            fprintf(stderr,
                "[filed-storage] bootstrap module invalid index=%llu name=%s fd=%llu size=%llu\n",
                (unsigned long long)i,
                module->name,
                (unsigned long long)module->image_fd,
                (unsigned long long)module->image_size);
            return -2;
        }

        uint64_t map_size = 0;
        if (koboxd_align_image_size(module->image_size, &map_size) != 0) {
            return -3;
        }
        const unsigned char *image = pacha_mmap(
            (int)module->image_fd,
            map_size,
            PACHA_PROT_READ,
            PACHA_MMAP_SHARED,
            0);
        if (image == NULL) {
            fprintf(stderr, "[filed-storage] bootstrap module mmap failed name=%s fd=%llu\n",
                module->name,
                (unsigned long long)module->image_fd);
            return -3;
        }
        if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
            fprintf(stderr, "[filed-storage] bootstrap module is not ELF name=%s\n", module->name);
            (void)pacha_munmap((void *)image, map_size);
            return -4;
        }
        (void)pacha_munmap((void *)image, map_size);
    }

    return 0;
}

const koboxd_bootstrap_module_t *koboxd_bootstrap_find_module(
    const koboxd_bootstrap_t *bootstrap,
    const char *name)
{
    if (bootstrap == NULL || name == NULL) {
        return NULL;
    }
    for (uint64_t i = 0; i < bootstrap->module_count; i++) {
        if (strncmp(bootstrap->modules[i].name, name, KOBOXD_BOOTSTRAP_NAME_BYTES) == 0) {
            return &bootstrap->modules[i];
        }
    }
    return NULL;
}
