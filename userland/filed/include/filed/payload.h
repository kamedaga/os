#pragma once

/*
 * Filed payload and fast-session layouts used by the filed ABI.
 *
 * Do not add request/reply envelope fields or compatibility shims here. The
 * common service envelope and op numbering live in ipc_protocol.h.
 */

#include <stddef.h>
#include <stdint.h>

#include "filed/flags.h"
#include "filed/ipc_protocol.h"

enum {
    FILED_PAGE_BYTES = 8192,
    FILED_SYMLINK_TARGET_BYTES =
        FILED_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES -
        FILED_PATH_BYTES - 2u * sizeof(uint64_t),
    FILED_SESSION_PAGE_BYTES = 40960,
    FILED_FAST_MAGIC = 0x31545341464c4446ull,
    FILED_FAST_VERSION = PACHA_SERVICE_ABI_VERSION,
    FILED_FAST_REQUEST_CAPACITY = 8,
    FILED_FAST_COMPLETION_CAPACITY = 8,
    FILED_FAST_PAYLOAD_SLOT_COUNT = 4,
    FILED_FAST_PAYLOAD_OFFSET = 4096,
    FILED_FAST_GENERATION_OFFSET =
        FILED_FAST_PAYLOAD_OFFSET +
        FILED_FAST_PAYLOAD_SLOT_COUNT * FILED_PAGE_BYTES,
    FILED_FAST_GENERATION_CAPACITY = 64,

    FILED_UTIMENS_ATIME = 1u << 0,
    FILED_UTIMENS_MTIME = 1u << 1,

    FILED_EXEC_BOOTSTRAP_FD = 1u << 0,
    FILED_EXEC_INHERIT_FDS = 1u << 1,
    FILED_EXEC_PATCH_BOOTSTRAP_FDS = 1u << 2,
    FILED_EXEC_INHERIT_HANDLES = 1u << 3,
    FILED_EXEC_LINUX_LPR = 1u << 4,
    FILED_EXEC_LINUX_BOOTSTRAP = 1u << 5,
    FILED_EXEC_SELF = 1u << 6,
    FILED_EXEC_LINUX_DEFAULT_STDIO = 1u << 7,
    FILED_EXEC_LPR_FD_TABLE = 1u << 8,
    FILED_EXEC_TRANSFER_PROCESS_FD = 1u << 9,
    FILED_EXEC_MAX_INHERIT_FDS = 16,
    FILED_EXEC_MAX_INHERIT_HANDLES = 4,
    FILED_EXEC_MAX_FD_PATCHES = 4,
    FILED_EXEC_MAX_ARGS = 128,
    FILED_EXEC_MAX_ENVS = 64,
    FILED_EXEC_STRING_BYTES = 6144,

    FILED_EXEC_PATCH_INHERIT_FD = 1,
    FILED_EXEC_PATCH_BOOTSTRAP_FD = 2,
    FILED_EXEC_PATCH_INHERIT_HANDLE = 3,

    FILED_EXEC_LPR_FD_FILED = 1,
    FILED_EXEC_LPR_FD_TTY = 2,
    FILED_EXEC_LPR_FD_DRM = 3,
    FILED_EXEC_LPR_FD_INPUT = 4,
    FILED_EXEC_LPR_FD_PIPE = 5,
    FILED_EXEC_LPR_FD_EVENT = 6,
    FILED_EXEC_LPR_FD_SOCKET = 7,
    FILED_EXEC_LPR_FD_NATIVE = 8,
    FILED_EXEC_LPR_FD_DMABUF = 9,
};

#define FILED_EXEC_LPR_FD_TABLE_MAGIC 0x3144424652504c46ull
#define FILED_EXEC_LPR_FD_TABLE_VERSION 5ull

typedef struct filed_openat {
    uint64_t dir_handle;
    uint64_t rights;
    uint64_t open_flags;
    uint64_t object_generation;
    uint64_t dir_generation;
    uint64_t reserved0;
    char name[FILED_PATH_BYTES];
} filed_openat_t;

typedef struct filed_validate_open_cache {
    uint64_t cached_handle;
    uint64_t dir_handle;
    uint64_t rights;
    uint64_t open_flags;
    uint64_t object_generation;
    uint64_t dir_generation;
    uint64_t reserved0;
    uint64_t reserved1;
    char name[FILED_NAME_BYTES];
} filed_validate_open_cache_t;

typedef struct filed_io {
    uint64_t handle;
    uint64_t offset;
    uint64_t length;
    uint8_t data[FILED_IO_BYTES];
} filed_io_t;

typedef struct filed_pread_vmo {
    uint64_t handle;
    uint64_t file_offset;
    uint64_t vmo_offset;
    uint64_t length;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_pread_vmo_t;

typedef struct filed_file_vmo {
    uint64_t handle;
    uint64_t file_offset;
    uint64_t length;
    uint64_t flags;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_file_vmo_t;

typedef struct filed_memfd_create {
    uint64_t flags;
    uint64_t reserved0;
    char name[FILED_MEMFD_NAME_BYTES];
} filed_memfd_create_t;

typedef struct filed_fast_header {
    uint64_t magic;
    uint64_t version;
    uint64_t flags;
    uint64_t request_capacity;
    uint64_t completion_capacity;
    uint64_t payload_slot_count;
    uint64_t payload_slot_size;
    uint64_t payload_offset;
    uint64_t request_head;
    uint64_t request_tail;
    uint64_t completion_head;
    uint64_t completion_tail;
    uint64_t doorbell_seq;
    uint64_t completion_seq;
    uint64_t generation_offset;
    uint64_t generation_capacity;
} filed_fast_header_t;

typedef struct filed_fast_request {
    uint64_t request_id;
    uint64_t opcode;
    uint64_t flags;
    uint64_t handle;
    uint64_t word2;
    uint64_t offset;
    uint64_t length;
    uint64_t payload_slot;
    uint64_t payload_length;
    uint64_t timeout_ns;
} filed_fast_request_t;

typedef struct filed_fast_completion {
    uint64_t request_id;
    int64_t status;
    uint64_t result;
    uint64_t bytes;
    uint64_t flags;
} filed_fast_completion_t;

typedef struct filed_generation_entry {
    uint64_t seq;
    uint64_t handle;
    uint64_t object_generation;
    uint64_t dir_generation;
} filed_generation_entry_t;

typedef struct filed_statx {
    uint64_t handle;
    uint64_t mode;
    uint64_t size;
    uint64_t blocks;
    uint64_t nlink;
    uint64_t kind;
    int64_t atime_sec;
    int64_t atime_nsec;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t ctime_sec;
    int64_t ctime_nsec;
    uint64_t object_generation;
    uint64_t dir_generation;
} filed_statx_t;

typedef struct filed_utimens {
    uint64_t handle;
    uint64_t mask;
    int64_t atime_sec;
    int64_t atime_nsec;
    int64_t mtime_sec;
    int64_t mtime_nsec;
} filed_utimens_t;

typedef struct filed_chmod {
    uint64_t handle;
    uint64_t mode;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_chmod_t;

typedef struct filed_truncate {
    uint64_t handle;
    uint64_t size;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_truncate_t;

typedef struct filed_seek {
    uint64_t handle;
    int64_t offset;
    uint64_t whence;
    uint64_t reserved0;
} filed_seek_t;

typedef struct filed_unlink {
    uint64_t dir_handle;
    uint64_t reserved0;
    char name[FILED_PATH_BYTES];
} filed_unlink_t;

typedef struct filed_mkdir {
    uint64_t dir_handle;
    uint64_t mode;
    char name[FILED_PATH_BYTES];
} filed_mkdir_t;

typedef struct filed_rmdir {
    uint64_t dir_handle;
    uint64_t reserved0;
    char name[FILED_PATH_BYTES];
} filed_rmdir_t;

typedef struct filed_symlink {
    uint64_t dir_handle;
    uint64_t target_length;
    char name[FILED_PATH_BYTES];
    char target[FILED_SYMLINK_TARGET_BYTES];
} filed_symlink_t;

typedef struct filed_readlink {
    uint64_t dir_handle;
    uint64_t target_length;
    char name[FILED_PATH_BYTES];
    char target[FILED_SYMLINK_TARGET_BYTES];
} filed_readlink_t;

typedef char filed_symlink_fits_wire_page[
    sizeof(filed_symlink_t) <=
        FILED_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES ? 1 : -1];
typedef char filed_readlink_fits_wire_page[
    sizeof(filed_readlink_t) <=
        FILED_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES ? 1 : -1];

typedef struct filed_link {
    uint64_t old_dir_handle;
    uint64_t new_dir_handle;
    uint64_t flags;
    uint64_t reserved0;
    char old_name[FILED_PATH_BYTES];
    char new_name[FILED_PATH_BYTES];
} filed_link_t;

typedef struct filed_rename {
    uint64_t old_dir_handle;
    uint64_t new_dir_handle;
    char old_name[FILED_PATH_BYTES];
    char new_name[FILED_PATH_BYTES];
} filed_rename_t;

typedef struct filed_handle_flags {
    uint64_t handle;
    uint64_t fd_flags;
    uint64_t status_flags;
    uint64_t reserved0;
} filed_handle_flags_t;

typedef struct filed_dirent {
    uint64_t handle;
    uint64_t kind;
    uint64_t name_len;
    char name[FILED_DIRENT_NAME_BYTES];
} filed_dirent_t;

typedef struct filed_getdents {
    uint64_t dir_handle;
    uint64_t offset;
    uint64_t capacity;
    uint64_t count;
    uint64_t dir_generation;
    uint64_t reserved0;
    filed_dirent_t entries[FILED_DIRENT_CAPACITY];
} filed_getdents_t;

typedef struct filed_exec_fd_patch {
    uint64_t kind;
    uint64_t index;
    uint64_t offset;
    uint64_t reserved0;
} filed_exec_fd_patch_t;

typedef struct filed_exec_string_ref {
    uint16_t offset;
    uint16_t length;
} filed_exec_string_ref_t;

typedef struct filed_exec_lpr_fd {
    uint64_t fd;
    uint64_t kind;
    uint64_t flags;
    uint64_t handle;
    uint64_t offset_or_counter;
} filed_exec_lpr_fd_t;

typedef struct filed_exec_lpr_fd_table {
    uint64_t magic;
    uint64_t version;
    uint64_t byte_size;
    uint64_t fd_count;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_exec_lpr_fd_table_t;

typedef struct filed_exec_path {
    uint64_t dir_handle;
    uint64_t flags;
    uint64_t inherit_fd_count;
    uint64_t fd_patch_count;
    uint64_t inherit_handle_count;
    uint64_t string_bytes;
    uint64_t argc;
    uint64_t envc;
    uint64_t linux_pid;
    uint64_t linux_ppid;
    uint64_t linux_sid;
    uint64_t linux_pgrp;
    uint64_t linux_next_pid;
    uint64_t cwd_handle;
    uint64_t lpr_fd_table_bytes;
    uint64_t lpr_supervisor_token;
    uint64_t lpr_fd_table_token;
    uint64_t inherit_handles[FILED_EXEC_MAX_INHERIT_HANDLES];
    uint64_t inherit_fd_targets[FILED_EXEC_MAX_INHERIT_FDS];
    filed_exec_fd_patch_t fd_patches[FILED_EXEC_MAX_FD_PATCHES];
    char path[FILED_PATH_BYTES];
    filed_exec_string_ref_t cwd;
    filed_exec_string_ref_t ctty;
    filed_exec_string_ref_t argv[FILED_EXEC_MAX_ARGS];
    filed_exec_string_ref_t envp[FILED_EXEC_MAX_ENVS];
    char strings[FILED_EXEC_STRING_BYTES];
} filed_exec_path_t;

typedef char filed_exec_path_fits_wire_page[
    sizeof(filed_exec_path_t) <=
        FILED_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES ? 1 : -1];

static inline int filed_exec_string_ref_empty(filed_exec_string_ref_t ref)
{
    return ref.offset == 0 && ref.length == 0;
}

static inline int filed_exec_string_ref_valid(
    const filed_exec_path_t *request,
    filed_exec_string_ref_t ref)
{
    if (request == NULL ||
        request->string_bytes > FILED_EXEC_STRING_BYTES ||
        ref.length == 0 ||
        ref.offset >= request->string_bytes ||
        (uint64_t)ref.offset + (uint64_t)ref.length > request->string_bytes)
    {
        return 0;
    }
    return request->strings[(uint64_t)ref.offset + (uint64_t)ref.length - 1u] == '\0';
}

static inline const char *filed_exec_string(
    const filed_exec_path_t *request,
    filed_exec_string_ref_t ref)
{
    return &request->strings[ref.offset];
}
