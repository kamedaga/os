#include "filed/backend_router.h"

#include "filed/kobox_backend.h"
#include "filed/tmpfs_backend.h"

int filed_runtime_backend_lookup(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name,
    uint64_t *out_object_id)
{
    if (runtime == NULL) {
        return -22;
    }
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_lookup(&runtime->tmpfs, parent_object_id, name, out_object_id);
    }
    return filed_kobox_backend_lookup(&runtime->backend, parent_object_id, name, out_object_id);
}

int filed_runtime_backend_statx(
    filed_runtime_t *runtime,
    uint64_t object_id,
    storage_v2_statx_reply_t *out_stat)
{
    if (runtime == NULL) {
        return -22;
    }
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_statx(&runtime->tmpfs, object_id, out_stat);
    }
    return filed_kobox_backend_statx(&runtime->backend, object_id, out_stat);
}

int filed_runtime_backend_pread(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    if (runtime == NULL) {
        return -22;
    }
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_pread(&runtime->tmpfs, object_id, offset, buffer, length, out_bytes);
    }
    return filed_kobox_backend_pread(&runtime->backend, object_id, offset, buffer, length, out_bytes);
}
