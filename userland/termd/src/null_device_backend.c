#include "null_device_backend.h"

#include "device/device_backend_internal.h"

#include <stdlib.h>

typedef struct termd_null_device_backend {
    kb_device_backend_t base;
} termd_null_device_backend_t;

static void null_destroy(kb_device_backend_t *backend)
{
    free(backend);
}

static kb_status_t null_device_count(kb_device_backend_t *backend, size_t *out_count)
{
    (void)backend;
    if (out_count == 0) {
        return KB_ERR_INVALID;
    }
    *out_count = 0;
    return KB_OK;
}

static kb_status_t null_device_at(kb_device_backend_t *backend, size_t index, kb_device_t **out_device)
{
    (void)backend;
    (void)index;
    if (out_device == 0) {
        return KB_ERR_INVALID;
    }
    *out_device = 0;
    return KB_ERR_NOT_FOUND;
}

static uint64_t null_monotonic_ns(kb_device_backend_t *backend)
{
    (void)backend;
    return 0;
}

static void null_log(kb_device_backend_t *backend, int level, const char *message)
{
    (void)backend;
    (void)level;
    (void)message;
}

static const kb_device_backend_ops_t null_ops = {
    .destroy = null_destroy,
    .device_count = null_device_count,
    .device_at = null_device_at,
    .monotonic_ns = null_monotonic_ns,
    .log = null_log,
};

kb_status_t termd_null_device_backend_create(kb_device_backend_t **out_backend)
{
    if (out_backend == 0) {
        return KB_ERR_INVALID;
    }
    termd_null_device_backend_t *backend = calloc(1, sizeof(*backend));
    if (backend == 0) {
        return KB_ERR_NOMEM;
    }
    backend->base.ops = &null_ops;
    *out_backend = &backend->base;
    return KB_OK;
}
