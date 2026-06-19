#pragma once

#include <stdint.h>

enum {
    KOBOXD_WIRE_VERSION = 1,
    KOBOXD_WIRE_CONTROL_MAGIC = 0x4b42584354524c30ull,
    KOBOXD_WIRE_REPLY_MAGIC = 0x4b42585245504c59ull,
    KOBOXD_WIRE_ENDPOINT_MAGIC = 0x4b42584550544d30ull,

    KOBOXD_WIRE_ENDPOINT_CONTROL = 1,
    KOBOXD_WIRE_ENDPOINT_BLOCK = 2,
    KOBOXD_WIRE_ENDPOINT_FS_BACKEND = 3,
    KOBOXD_WIRE_ENDPOINT_EVENT = 4,

    KOBOXD_WIRE_CONTROL_GET_ENDPOINT = 2,

    KOBOXD_WIRE_BLOCK_IDENTIFY = 1,
    KOBOXD_WIRE_FS_MOUNT_ROOT = 1,
    KOBOXD_WIRE_FS_LOOKUP = 2,
    KOBOXD_WIRE_FS_OPEN = 3,
    KOBOXD_WIRE_FS_PREAD = 4,
    KOBOXD_WIRE_FS_PWRITE = 5,
    KOBOXD_WIRE_FS_STATX = 6,
    KOBOXD_WIRE_FS_GETDENTS = 7,
    KOBOXD_WIRE_FS_FSYNC = 8,

    KOBOXD_WIRE_FS_ROOT_OBJECT_ID = 1,
    KOBOXD_WIRE_FS_NAME_BYTES = 96,
    KOBOXD_WIRE_FS_IO_BYTES = 7680,
    KOBOXD_WIRE_FS_DIRENT_NAME_BYTES = 96,
    KOBOXD_WIRE_FS_DIRENT_CAPACITY = 16,
    KOBOXD_WIRE_FS_PAGE_BYTES = 8192,
};

typedef struct koboxd_wire_fs_lookup {
    uint64_t parent_object_id;
    char name[KOBOXD_WIRE_FS_NAME_BYTES];
} koboxd_wire_fs_lookup_t;

typedef struct koboxd_wire_fs_io {
    uint64_t object_id;
    uint64_t offset;
    uint64_t length;
    uint8_t data[KOBOXD_WIRE_FS_IO_BYTES];
} koboxd_wire_fs_io_t;

typedef struct koboxd_wire_fs_statx {
    uint64_t object_id;
    uint64_t mode;
    uint64_t size;
    uint64_t blocks;
    uint64_t nlink;
    uint64_t kind;
} koboxd_wire_fs_statx_t;

typedef struct koboxd_wire_fs_dirent {
    uint64_t object_id;
    uint64_t kind;
    uint64_t name_len;
    char name[KOBOXD_WIRE_FS_DIRENT_NAME_BYTES];
} koboxd_wire_fs_dirent_t;

typedef struct koboxd_wire_fs_getdents {
    uint64_t dir_object_id;
    uint64_t offset;
    uint64_t capacity;
    uint64_t count;
    koboxd_wire_fs_dirent_t entries[KOBOXD_WIRE_FS_DIRENT_CAPACITY];
} koboxd_wire_fs_getdents_t;
