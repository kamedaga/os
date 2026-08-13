#pragma once

#include "storage/bootstrap.h"

#include <stdint.h>

enum {
    KOBOXD_PAGE_SIZE = 4096,
};

#define KOBOXD_BOOTSTRAP_MAGIC STORAGE_FILED_BOOTSTRAP_MAGIC
#define KOBOXD_BOOTSTRAP_MAX_MODULES STORAGE_STACK_MODULE_CAPACITY
#define KOBOXD_BOOTSTRAP_NAME_BYTES STORAGE_STACK_MODULE_NAME_BYTES

typedef storage_module_image_desc_t koboxd_bootstrap_module_t;
typedef storage_filed_bootstrap_t koboxd_bootstrap_t;

int koboxd_align_image_size(uint64_t size, uint64_t *out_size);
int koboxd_find_bootstrap_fd(char **argv, int *out_bootstrap_fd);
int koboxd_read_bootstrap_fd(int fd, koboxd_bootstrap_t *out_bootstrap);
int koboxd_validate_bootstrap_package(
    const koboxd_bootstrap_t *bootstrap,
    uint64_t bootstrap_size);
const koboxd_bootstrap_module_t *koboxd_bootstrap_find_module(
    const koboxd_bootstrap_t *bootstrap,
    const char *name);
