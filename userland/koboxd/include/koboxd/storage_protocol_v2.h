#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"

enum {
    STORAGE_V2_SERVICE_ID = PACHA_SERVICE_ID_STORAGE,

    STORAGE_V2_OP_HELLO = 0x0001u,
    STORAGE_V2_OP_MOUNT_ROOT = 0x0101u,
    STORAGE_V2_OP_LOOKUP = 0x0201u,
    STORAGE_V2_OP_STATX = 0x0202u,
    STORAGE_V2_OP_GETDENTS = 0x0203u,
    STORAGE_V2_OP_PREAD = 0x0301u,
    STORAGE_V2_OP_PWRITE = 0x0302u,
    STORAGE_V2_OP_FSYNC = 0x0303u,
    STORAGE_V2_OP_CREATE = 0x0401u,
    STORAGE_V2_OP_TRUNCATE = 0x0402u,
    STORAGE_V2_OP_UTIMENS = 0x0403u,
    STORAGE_V2_OP_CHMOD = 0x0404u,
    STORAGE_V2_OP_UNLINK = 0x0405u,
    STORAGE_V2_OP_RENAME = 0x0406u,
    STORAGE_V2_OP_MKDIR = 0x0407u,
    STORAGE_V2_OP_RMDIR = 0x0408u,
    STORAGE_V2_OP_RELEASE_OBJECT = 0x0501u,
    STORAGE_V2_OP_SYNC_ALL = 0x0502u,
    STORAGE_V2_OP_DIAG_DUMP = 0x7f01u,

    STORAGE_V2_ROOT_OBJECT_ID = 1u,
    STORAGE_V2_NAME_BYTES = 96u,
    STORAGE_V2_IO_BYTES = 7680u,
    STORAGE_V2_DIRENT_NAME_BYTES = 96u,
    STORAGE_V2_DIRENT_CAPACITY = 16u,
    STORAGE_V2_PAGE_BYTES = 8192u,
};

typedef struct storage_v2_object_request {
    uint64_t object_id;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t flags;
} storage_v2_object_request_t;

typedef struct storage_v2_lookup_request {
    uint64_t parent_object_id;
    uint64_t flags;
    char name[STORAGE_V2_NAME_BYTES];
} storage_v2_lookup_request_t;

typedef struct storage_v2_create_request {
    uint64_t parent_object_id;
    uint64_t mode;
    char name[STORAGE_V2_NAME_BYTES];
} storage_v2_create_request_t;

typedef struct storage_v2_truncate_request {
    uint64_t object_id;
    uint64_t size;
} storage_v2_truncate_request_t;

typedef struct storage_v2_utimens_request {
    uint64_t object_id;
    uint64_t mask;
    int64_t atime_sec;
    int64_t atime_nsec;
    int64_t mtime_sec;
    int64_t mtime_nsec;
} storage_v2_utimens_request_t;

typedef struct storage_v2_chmod_request {
    uint64_t object_id;
    uint64_t mode;
} storage_v2_chmod_request_t;

typedef struct storage_v2_unlink_request {
    uint64_t parent_object_id;
    char name[STORAGE_V2_NAME_BYTES];
} storage_v2_unlink_request_t;

typedef struct storage_v2_mkdir_request {
    uint64_t parent_object_id;
    uint64_t mode;
    char name[STORAGE_V2_NAME_BYTES];
} storage_v2_mkdir_request_t;

typedef struct storage_v2_rmdir_request {
    uint64_t parent_object_id;
    char name[STORAGE_V2_NAME_BYTES];
} storage_v2_rmdir_request_t;

typedef struct storage_v2_rename_request {
    uint64_t old_parent_object_id;
    uint64_t new_parent_object_id;
    char old_name[STORAGE_V2_NAME_BYTES];
    char new_name[STORAGE_V2_NAME_BYTES];
} storage_v2_rename_request_t;

typedef struct storage_v2_io_request {
    uint64_t object_id;
    uint64_t offset;
    uint64_t length;
    uint64_t flags;
    uint8_t data[STORAGE_V2_IO_BYTES];
} storage_v2_io_request_t;

typedef struct storage_v2_statx_reply {
    uint64_t object_id;
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
} storage_v2_statx_reply_t;

typedef struct storage_v2_dirent {
    uint64_t object_id;
    uint64_t kind;
    uint64_t name_len;
    char name[STORAGE_V2_DIRENT_NAME_BYTES];
} storage_v2_dirent_t;

typedef struct storage_v2_getdents_request {
    uint64_t dir_object_id;
    uint64_t offset;
    uint64_t capacity;
    uint64_t count;
    storage_v2_dirent_t entries[STORAGE_V2_DIRENT_CAPACITY];
} storage_v2_getdents_request_t;

_Static_assert(sizeof(storage_v2_object_request_t) == 32, "storage_v2_object_request size");
_Static_assert(sizeof(storage_v2_lookup_request_t) == 112, "storage_v2_lookup_request size");
_Static_assert(sizeof(storage_v2_create_request_t) == 112, "storage_v2_create_request size");
_Static_assert(sizeof(storage_v2_truncate_request_t) == 16, "storage_v2_truncate_request size");
_Static_assert(sizeof(storage_v2_utimens_request_t) == 48, "storage_v2_utimens_request size");
_Static_assert(sizeof(storage_v2_chmod_request_t) == 16, "storage_v2_chmod_request size");
_Static_assert(sizeof(storage_v2_unlink_request_t) == 104, "storage_v2_unlink_request size");
_Static_assert(sizeof(storage_v2_mkdir_request_t) == 112, "storage_v2_mkdir_request size");
_Static_assert(sizeof(storage_v2_rmdir_request_t) == 104, "storage_v2_rmdir_request size");
_Static_assert(sizeof(storage_v2_rename_request_t) == 208, "storage_v2_rename_request size");
_Static_assert(sizeof(storage_v2_io_request_t) == 7712, "storage_v2_io_request size");
_Static_assert(sizeof(storage_v2_statx_reply_t) == 96, "storage_v2_statx_reply size");
_Static_assert(sizeof(storage_v2_dirent_t) == 120, "storage_v2_dirent size");
_Static_assert(sizeof(storage_v2_getdents_request_t) == 1952, "storage_v2_getdents_request size");
