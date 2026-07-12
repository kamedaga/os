#pragma once

#include <stdint.h>

#include "filed/flags.h"
#include "pacha/service_abi.h"

/* Replies use pacha_service_envelope.status: 0 or a negative Linux errno. */

enum {
    FILED_SERVICE_ID = PACHA_SERVICE_ID_FILED,

    FILED_OP_HELLO = 0u,

    FILED_OP_SESSION_OPEN = 1u,
    FILED_OP_SESSION_CLOSE = 2u,
    FILED_OP_SESSION_DOORBELL = 3u,

    FILED_OP_VFS_ROOT_STAT = 4u,
    FILED_OP_VFS_ROOT_GETDENTS = 5u,
    FILED_OP_VFS_OPENAT = 6u,
    FILED_OP_VFS_VALIDATE_OPEN_CACHE = 7u,
    FILED_OP_VFS_STAT = 8u,
    FILED_OP_VFS_READ = 9u,
    FILED_OP_VFS_PREAD = 10u,
    FILED_OP_VFS_WRITE = 11u,
    FILED_OP_VFS_PWRITE = 12u,
    FILED_OP_VFS_WRITE_BATCH = 13u,
    FILED_OP_VFS_PWRITE_BATCH = 14u,
    FILED_OP_VFS_GETDENTS = 15u,
    FILED_OP_VFS_SEEK = 16u,
    FILED_OP_VFS_CLOSE = 17u,
    FILED_OP_VFS_DUP = 18u,
    FILED_OP_VFS_GET_FLAGS = 19u,
    FILED_OP_VFS_SET_FLAGS = 20u,
    FILED_OP_VFS_FSYNC = 21u,
    FILED_OP_VFS_TRUNCATE = 22u,
    FILED_OP_VFS_UNLINK = 23u,
    FILED_OP_VFS_RENAME = 24u,
    FILED_OP_VFS_MKDIR = 25u,
    FILED_OP_VFS_RMDIR = 26u,
    FILED_OP_VFS_SYMLINK = 27u,
    FILED_OP_VFS_READLINK = 28u,
    FILED_OP_VFS_LINK = 29u,
    FILED_OP_VFS_FILE_VMO = 30u,
    FILED_OP_VFS_PREAD_TO_VMO = 31u,
    FILED_OP_VFS_SYNC_ALL = 32u,
    FILED_OP_VFS_UTIMENS = 33u,
    FILED_OP_VFS_CHMOD = 34u,
    FILED_OP_VFS_SHARED_FILE_VMO = 35u,
    FILED_OP_VFS_MEMFD_CREATE = 36u,

    FILED_OP_EXEC_PATH = 37u,
    FILED_OP_EXEC_SELF = 38u,

    FILED_OP_SERVICE_SET_NETD_SOCKET = 39u,
    FILED_OP_SERVICE_SET_TERMD_TTY = 40u,
    FILED_OP_SERVICE_SET_DRMD_DRM = 41u,
    FILED_OP_SERVICE_SET_INPUTD_INPUT = 42u,
    FILED_OP_SERVICE_REGISTER_TERMD_SIGNAL_SUPERVISOR = 43u,

    FILED_OP_DIAG_PING = 44u,
    FILED_OP_DIAG_DUMP = 45u,
    FILED_OP_DIAG_ERROR_GET = 46u,
    FILED_OP_DIAG_DUMP_METRICS = 47u,
    FILED_OP_DIAG_SET_CACHE_SLOTS = 48u,

    FILED_NAME_BYTES = 96u,
    FILED_PATH_BYTES = 480u,
    FILED_IO_BYTES = 7680u,
    FILED_DIRENT_NAME_BYTES = 96u,
    FILED_DIRENT_CAPACITY = 16u,

    FILED_FILE_VMO_WRITE = 1u << 0,
    FILED_FILE_VMO_EXEC = 1u << 1,

    FILED_MEMFD_CLOEXEC = 1u << 0,
    FILED_MEMFD_ALLOW_SEALING = 1u << 1,
    FILED_MEMFD_NAME_BYTES = 250u,
};

typedef struct filed_service_endpoint_request {
    uint64_t endpoint_kind;
    uint64_t flags;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_service_endpoint_request_t;

typedef struct filed_path_request {
    uint64_t dir_handle;
    uint64_t rights;
    uint64_t flags;
    uint64_t mode;
    char path[FILED_PATH_BYTES];
} filed_path_request_t;

typedef struct filed_handle_request {
    uint64_t handle;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
} filed_handle_request_t;

typedef struct filed_io_request {
    uint64_t handle;
    uint64_t offset;
    uint64_t length;
    uint64_t flags;
    uint8_t data[FILED_IO_BYTES];
} filed_io_request_t;

typedef struct filed_file_vmo_request {
    uint64_t handle;
    uint64_t file_offset;
    uint64_t length;
    uint64_t flags;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_file_vmo_request_t;

typedef struct filed_memfd_create_request {
    uint64_t flags;
    uint64_t reserved0;
    char name[FILED_MEMFD_NAME_BYTES];
} filed_memfd_create_request_t;

typedef struct filed_exec_request {
    uint64_t dir_handle;
    uint64_t flags;
    uint64_t argc;
    uint64_t envc;
    uint64_t inherit_fd_count;
    uint64_t inherit_handle_count;
    char path[FILED_PATH_BYTES];
} filed_exec_request_t;

typedef struct filed_diag_request {
    uint64_t selector;
    uint64_t subject;
    uint64_t flags;
    uint64_t reserved0;
} filed_diag_request_t;

_Static_assert(sizeof(filed_service_endpoint_request_t) == 32,
    "filed_service_endpoint_request size");
_Static_assert(sizeof(filed_path_request_t) == 512, "filed_path_request size");
_Static_assert(sizeof(filed_handle_request_t) == 32, "filed_handle_request size");
_Static_assert(sizeof(filed_io_request_t) == 7712, "filed_io_request size");
_Static_assert(sizeof(filed_file_vmo_request_t) == 48, "filed_file_vmo_request size");
_Static_assert(sizeof(filed_exec_request_t) == 528, "filed_exec_request size");
_Static_assert(sizeof(filed_diag_request_t) == 32, "filed_diag_request size");
