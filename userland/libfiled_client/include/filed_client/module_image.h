#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    FILED_CLIENT_ENDPOINT_FD = 240,
    FILED_CLIENT_MODULE_MAX_BYTES = 16u * 1024u * 1024u,
};

struct filed_client_module_image {
    unsigned char *data;
    size_t size;
    const char *name;
};

int filed_client_load_module_image(
    int endpoint_fd,
    const char *path,
    const char *name,
    struct filed_client_module_image *out);

void filed_client_release_module_image(struct filed_client_module_image *image);
