#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"

enum {
    FILED_V2_SERVICE_ID = PACHA_SERVICE_ID_FILED,

    FILED_V2_OP_HELLO = 0x0001u,

    FILED_V2_OP_SESSION_OPEN = 0x0101u,
    FILED_V2_OP_SESSION_CLOSE = 0x0102u,
    FILED_V2_OP_SESSION_DOORBELL = 0x0103u,

    FILED_V2_OP_VFS_ROOT_STAT = 0x0201u,
    FILED_V2_OP_VFS_ROOT_GETDENTS = 0x0202u,
    FILED_V2_OP_VFS_OPENAT = 0x0203u,
    FILED_V2_OP_VFS_VALIDATE_OPEN_CACHE = 0x0204u,
    FILED_V2_OP_VFS_STAT = 0x0205u,
    FILED_V2_OP_VFS_READ = 0x0206u,
    FILED_V2_OP_VFS_PREAD = 0x0207u,
    FILED_V2_OP_VFS_WRITE = 0x0208u,
    FILED_V2_OP_VFS_PWRITE = 0x0209u,
    FILED_V2_OP_VFS_WRITE_BATCH = 0x020au,
    FILED_V2_OP_VFS_PWRITE_BATCH = 0x020bu,
    FILED_V2_OP_VFS_GETDENTS = 0x020cu,
    FILED_V2_OP_VFS_SEEK = 0x020du,
    FILED_V2_OP_VFS_CLOSE = 0x020eu,
    FILED_V2_OP_VFS_DUP = 0x020fu,
    FILED_V2_OP_VFS_GET_FLAGS = 0x0210u,
    FILED_V2_OP_VFS_SET_FLAGS = 0x0211u,
    FILED_V2_OP_VFS_FSYNC = 0x0212u,
    FILED_V2_OP_VFS_TRUNCATE = 0x0213u,
    FILED_V2_OP_VFS_UNLINK = 0x0214u,
    FILED_V2_OP_VFS_RENAME = 0x0215u,
    FILED_V2_OP_VFS_MKDIR = 0x0216u,
    FILED_V2_OP_VFS_RMDIR = 0x0217u,
    FILED_V2_OP_VFS_SYMLINK = 0x0218u,
    FILED_V2_OP_VFS_READLINK = 0x0219u,
    FILED_V2_OP_VFS_LINK = 0x021au,
    FILED_V2_OP_VFS_FILE_VMO = 0x021bu,
    FILED_V2_OP_VFS_PREAD_TO_VMO = 0x021cu,
    FILED_V2_OP_VFS_SYNC_ALL = 0x021du,
    FILED_V2_OP_VFS_UTIMENS = 0x021eu,
    FILED_V2_OP_VFS_CHMOD = 0x021fu,

    FILED_V2_OP_EXEC_PATH = 0x0301u,
    FILED_V2_OP_EXEC_SELF = 0x0302u,

    FILED_V2_OP_SERVICE_SET_NETD_SOCKET = 0x0401u,
    FILED_V2_OP_SERVICE_SET_TERMD_TTY = 0x0402u,
    FILED_V2_OP_SERVICE_REGISTER_TERMD_SIGNAL_SUPERVISOR = 0x0403u,

    FILED_V2_OP_DIAG_PING = 0x7f01u,
    FILED_V2_OP_DIAG_DUMP = 0x7f02u,
    FILED_V2_OP_DIAG_ERROR_GET = 0x7f03u,
    FILED_V2_OP_DIAG_DUMP_METRICS = 0x7f04u,
    FILED_V2_OP_DIAG_SET_CACHE_SLOTS = 0x7f05u,

    FILED_V2_NAME_BYTES = 96u,
    FILED_V2_PATH_BYTES = 480u,
    FILED_V2_IO_BYTES = 7680u,
    FILED_V2_DIRENT_NAME_BYTES = 96u,
    FILED_V2_DIRENT_CAPACITY = 16u,

    FILED_V2_RIGHT_LOOKUP = 1u << 0,
    FILED_V2_RIGHT_READ = 1u << 1,
    FILED_V2_RIGHT_WRITE = 1u << 2,
    FILED_V2_RIGHT_EXEC = 1u << 3,
    FILED_V2_RIGHT_STAT = 1u << 4,
    FILED_V2_RIGHT_GETDENTS = 1u << 5,
    FILED_V2_RIGHT_CREATE = 1u << 6,
    FILED_V2_RIGHT_REMOVE = 1u << 7,
    FILED_V2_RIGHT_RENAME = 1u << 8,

    FILED_V2_OPEN_CREATE = 1u << 0,
    FILED_V2_OPEN_EXCLUSIVE = 1u << 1,
    FILED_V2_OPEN_TRUNCATE = 1u << 2,
    FILED_V2_OPEN_DIRECTORY = 1u << 3,
    FILED_V2_OPEN_NOFOLLOW = 1u << 4,
    FILED_V2_OPEN_CLOEXEC = 1u << 5,
    FILED_V2_OPEN_APPEND = 1u << 6,
    FILED_V2_OPEN_NONBLOCK = 1u << 7,
    FILED_V2_OPEN_SYNC = 1u << 8,
};

typedef struct filed_v2_service_endpoint_request {
    uint64_t endpoint_kind;
    uint64_t flags;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_v2_service_endpoint_request_t;

typedef struct filed_v2_path_request {
    uint64_t dir_handle;
    uint64_t rights;
    uint64_t flags;
    uint64_t mode;
    char path[FILED_V2_PATH_BYTES];
} filed_v2_path_request_t;

typedef struct filed_v2_handle_request {
    uint64_t handle;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
} filed_v2_handle_request_t;

typedef struct filed_v2_io_request {
    uint64_t handle;
    uint64_t offset;
    uint64_t length;
    uint64_t flags;
    uint8_t data[FILED_V2_IO_BYTES];
} filed_v2_io_request_t;

typedef struct filed_v2_file_vmo_request {
    uint64_t handle;
    uint64_t file_offset;
    uint64_t length;
    uint64_t flags;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_v2_file_vmo_request_t;

typedef struct filed_v2_exec_request {
    uint64_t dir_handle;
    uint64_t flags;
    uint64_t argc;
    uint64_t envc;
    uint64_t inherit_fd_count;
    uint64_t inherit_handle_count;
    char path[FILED_V2_PATH_BYTES];
} filed_v2_exec_request_t;

typedef struct filed_v2_diag_request {
    uint64_t selector;
    uint64_t subject;
    uint64_t flags;
    uint64_t reserved0;
} filed_v2_diag_request_t;

_Static_assert(sizeof(filed_v2_service_endpoint_request_t) == 32,
    "filed_v2_service_endpoint_request size");
_Static_assert(sizeof(filed_v2_path_request_t) == 512, "filed_v2_path_request size");
_Static_assert(sizeof(filed_v2_handle_request_t) == 32, "filed_v2_handle_request size");
_Static_assert(sizeof(filed_v2_io_request_t) == 7712, "filed_v2_io_request size");
_Static_assert(sizeof(filed_v2_file_vmo_request_t) == 48, "filed_v2_file_vmo_request size");
_Static_assert(sizeof(filed_v2_exec_request_t) == 528, "filed_v2_exec_request size");
_Static_assert(sizeof(filed_v2_diag_request_t) == 32, "filed_v2_diag_request size");
