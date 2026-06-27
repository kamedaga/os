#pragma once

#include <stdint.h>

enum {
    KOBOXD_BOOTSTRAP_MAGIC = 0x3150474b42584f4bull,
    KOBOXD_BOOTSTRAP_MAX_MODULES = 8,
    KOBOXD_BOOTSTRAP_NAME_BYTES = 64,
    KOBOXD_PAGE_SIZE = 4096,
};

typedef struct koboxd_bootstrap_module {
    char name[KOBOXD_BOOTSTRAP_NAME_BYTES];
    uint64_t image_fd;
    uint64_t image_size;
} koboxd_bootstrap_module_t;

typedef struct koboxd_bootstrap {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t control_fd;
    uint64_t module_count;
    koboxd_bootstrap_module_t modules[KOBOXD_BOOTSTRAP_MAX_MODULES];
} koboxd_bootstrap_t;

int koboxd_align_image_size(uint64_t size, uint64_t *out_size);
int koboxd_find_bootstrap_fd(char **argv, int *out_bootstrap_fd);
int koboxd_read_bootstrap_fd(int fd, koboxd_bootstrap_t *out_bootstrap);
int koboxd_validate_bootstrap_package(
    const koboxd_bootstrap_t *bootstrap,
    uint64_t bootstrap_size);
const koboxd_bootstrap_module_t *koboxd_bootstrap_find_module(
    const koboxd_bootstrap_t *bootstrap,
    const char *name);
