#pragma once
#include <stddef.h>
#include <stdint.h>
#include "filed/flags.h"
#include "filed/ipc_protocol.h"
enum {
    FILED_EXEC_BOOTSTRAP_FD = 1u << 0,
    FILED_EXEC_INHERIT_FDS = 1u << 1,
    FILED_EXEC_PATCH_BOOTSTRAP_FDS = 1u << 2,
    FILED_EXEC_INHERIT_HANDLES = 1u << 3,
    FILED_EXEC_LINUX_LPR = 1u << 4,
    FILED_EXEC_SELF = 1u << 5,
    FILED_EXEC_TRANSFER_PROCESS_FD = 1u << 6,
    FILED_EXEC_DEFER_START = 1u << 7,
    /* Explicit launch-manager grants, never an automatic Linux default. */
    FILED_EXEC_SERVICE_NETD = 1u << 8,
    FILED_EXEC_SERVICE_TERMD = 1u << 9,
    FILED_EXEC_SERVICE_GPUD_DRM = 1u << 10,
    FILED_EXEC_SERVICE_INPUTD = 1u << 11,
    FILED_EXEC_PATCH_INHERIT_FD = 1,
    FILED_EXEC_PATCH_BOOTSTRAP_FD = 2,
    FILED_EXEC_PATCH_INHERIT_HANDLE = 3,
};
typedef struct filed_exec_fd_patch {
    uint64_t kind, index, offset, reserved0;
} filed_exec_fd_patch_t;
typedef struct filed_exec_string_ref {
    uint64_t offset, length;
} filed_exec_string_ref_t;
typedef struct filed_exec_fd_grant {
    uint64_t target, rights, flags;
} filed_exec_fd_grant_t;
/* Decoded process-local values. Only the little-endian encoder below may
 * serialize these values; pointers never cross the ABI. */
typedef struct filed_exec_path {
    uint64_t dir_handle, flags, inherit_fd_count, fd_patch_count;
    uint64_t inherit_handle_count, string_bytes, argc, envc;
    uint64_t *inherit_handles;
    filed_exec_fd_grant_t *fd_grants;
    filed_exec_fd_patch_t *fd_patches;
    char *path;
    filed_exec_string_ref_t *argv, *envp;
    char *strings;
    void *allocation;
} filed_exec_path_t;
static inline int filed_exec_string_ref_empty(filed_exec_string_ref_t ref)
{
    return ref.offset == 0 && ref.length == 0;
}
static inline int filed_exec_string_ref_valid(const filed_exec_path_t *r,
                                             filed_exec_string_ref_t ref)
{
    if (!r || !r->strings || !ref.length || ref.offset > r->string_bytes ||
        ref.length > r->string_bytes - ref.offset) return 0;
    const char *s = r->strings + ref.offset;
    if (s[ref.length - 1]) return 0;
    for (uint64_t i = 0; i + 1 < ref.length; ++i) if (!s[i]) return 0;
    return 1;
}
static inline const char *filed_exec_string(const filed_exec_path_t *r,
                                          filed_exec_string_ref_t ref)
{
    return filed_exec_string_ref_valid(r, ref) ? r->strings + ref.offset : NULL;
}
int filed_exec_payload_encode(const filed_exec_path_t *, uint8_t **, size_t *);
int filed_exec_payload_decode(const uint8_t *, size_t, const char *, uint64_t,
                              uint64_t, filed_exec_path_t *);
void filed_exec_payload_destroy(filed_exec_path_t *);
