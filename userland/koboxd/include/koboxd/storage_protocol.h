#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"

/* Replies use pacha_service_envelope.status: 0 or a negative Linux errno. */

enum {
    STORAGE_SERVICE_ID = PACHA_SERVICE_ID_STORAGE,

    STORAGE_OP_HELLO = 0u,
    STORAGE_OP_MOUNT_ROOT = 1u,
    STORAGE_OP_LOOKUP = 2u,
    STORAGE_OP_STATX = 3u,
    STORAGE_OP_GETDENTS = 4u,
    STORAGE_OP_PREAD = 5u,
    STORAGE_OP_PWRITE = 6u,
    STORAGE_OP_FSYNC = 7u,
    STORAGE_OP_CREATE = 8u,
    STORAGE_OP_TRUNCATE = 9u,
    STORAGE_OP_UTIMENS = 10u,
    STORAGE_OP_CHMOD = 11u,
    STORAGE_OP_UNLINK = 12u,
    STORAGE_OP_RENAME = 13u,
    STORAGE_OP_MKDIR = 14u,
    STORAGE_OP_MKNOD = 15u,
    STORAGE_OP_RMDIR = 16u,
    STORAGE_OP_RELEASE_OBJECT = 17u,
    STORAGE_OP_SYNC_ALL = 18u,
    STORAGE_OP_DIAG_DUMP = 19u,

    STORAGE_ROOT_OBJECT_ID = 1u,
    STORAGE_NAME_BYTES = 96u,
    STORAGE_IO_BYTES = 7680u,
    STORAGE_DIRENT_NAME_BYTES = 96u,
    STORAGE_DIRENT_CAPACITY = 16u,
    STORAGE_PAGE_BYTES = 8192u,
};

typedef struct storage_object_request {
    uint64_t object_id;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t flags;
} storage_object_request_t;

typedef struct storage_lookup_request {
    uint64_t parent_object_id;
    uint64_t flags;
    char name[STORAGE_NAME_BYTES];
} storage_lookup_request_t;

typedef struct storage_create_request {
    uint64_t parent_object_id;
    uint64_t mode;
    char name[STORAGE_NAME_BYTES];
} storage_create_request_t;

typedef struct storage_truncate_request {
    uint64_t object_id;
    uint64_t size;
} storage_truncate_request_t;

typedef struct storage_utimens_request {
    uint64_t object_id;
    uint64_t mask;
    int64_t atime_sec;
    int64_t atime_nsec;
    int64_t mtime_sec;
    int64_t mtime_nsec;
} storage_utimens_request_t;

typedef struct storage_chmod_request {
    uint64_t object_id;
    uint64_t mode;
} storage_chmod_request_t;

typedef struct storage_unlink_request {
    uint64_t parent_object_id;
    char name[STORAGE_NAME_BYTES];
} storage_unlink_request_t;

typedef struct storage_mkdir_request {
    uint64_t parent_object_id;
    uint64_t mode;
    char name[STORAGE_NAME_BYTES];
} storage_mkdir_request_t;

typedef struct storage_mknod_request {
    uint64_t parent_object_id;
    uint64_t mode;
    uint64_t dev;
    char name[STORAGE_NAME_BYTES];
} storage_mknod_request_t;

typedef struct storage_rmdir_request {
    uint64_t parent_object_id;
    char name[STORAGE_NAME_BYTES];
} storage_rmdir_request_t;

typedef struct storage_rename_request {
    uint64_t old_parent_object_id;
    uint64_t new_parent_object_id;
    char old_name[STORAGE_NAME_BYTES];
    char new_name[STORAGE_NAME_BYTES];
} storage_rename_request_t;

typedef struct storage_io_request {
    uint64_t object_id;
    uint64_t offset;
    uint64_t length;
    uint64_t flags;
    uint8_t data[STORAGE_IO_BYTES];
} storage_io_request_t;

typedef struct storage_statx_reply {
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
    uint64_t rdev;
} storage_statx_reply_t;

typedef struct storage_dirent {
    uint64_t object_id;
    uint64_t kind;
    uint64_t name_len;
    char name[STORAGE_DIRENT_NAME_BYTES];
} storage_dirent_t;

typedef struct storage_getdents_request {
    uint64_t dir_object_id;
    uint64_t offset;
    uint64_t capacity;
    uint64_t count;
    storage_dirent_t entries[STORAGE_DIRENT_CAPACITY];
} storage_getdents_request_t;

_Static_assert(sizeof(storage_object_request_t) == 32, "storage_object_request size");
_Static_assert(sizeof(storage_lookup_request_t) == 112, "storage_lookup_request size");
_Static_assert(sizeof(storage_create_request_t) == 112, "storage_create_request size");
_Static_assert(sizeof(storage_truncate_request_t) == 16, "storage_truncate_request size");
_Static_assert(sizeof(storage_utimens_request_t) == 48, "storage_utimens_request size");
_Static_assert(sizeof(storage_chmod_request_t) == 16, "storage_chmod_request size");
_Static_assert(sizeof(storage_unlink_request_t) == 104, "storage_unlink_request size");
_Static_assert(sizeof(storage_mkdir_request_t) == 112, "storage_mkdir_request size");
_Static_assert(sizeof(storage_mknod_request_t) == 120, "storage_mknod_request size");
_Static_assert(sizeof(storage_rmdir_request_t) == 104, "storage_rmdir_request size");
_Static_assert(sizeof(storage_rename_request_t) == 208, "storage_rename_request size");
_Static_assert(sizeof(storage_io_request_t) == 7712, "storage_io_request size");
_Static_assert(sizeof(storage_statx_reply_t) == 104, "storage_statx_reply size");
_Static_assert(sizeof(storage_dirent_t) == 120, "storage_dirent size");
_Static_assert(sizeof(storage_getdents_request_t) == 1952, "storage_getdents_request size");
