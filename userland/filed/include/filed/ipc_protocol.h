#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    FILED_WIRE_VERSION = 1,
    FILED_WIRE_REQUEST_MAGIC = 0x31465152444c4946ull,
    FILED_WIRE_REPLY_MAGIC = 0x31595052444c4946ull,

    FILED_WIRE_OP_HELLO = 1,
    FILED_WIRE_OP_ROOT_STAT = 2,
    FILED_WIRE_OP_ROOT_GETDENTS = 3,
    FILED_WIRE_OP_OPENAT = 4,
    FILED_WIRE_OP_STAT = 5,
    FILED_WIRE_OP_UTIMENS = 6,
    FILED_WIRE_OP_CHMOD = 7,
    FILED_WIRE_OP_PREAD = 8,
    FILED_WIRE_OP_GETDENTS = 9,
    FILED_WIRE_OP_CLOSE = 10,
    FILED_WIRE_OP_EXEC_PATH = 11,
    FILED_WIRE_OP_READ = 12,
    FILED_WIRE_OP_DUP = 13,
    FILED_WIRE_OP_GET_FLAGS = 14,
    FILED_WIRE_OP_SET_FLAGS = 15,
    FILED_WIRE_OP_PWRITE = 16,
    FILED_WIRE_OP_WRITE = 17,
    FILED_WIRE_OP_FSYNC = 18,
    FILED_WIRE_OP_TRUNCATE = 19,
    FILED_WIRE_OP_UNLINK = 20,
    FILED_WIRE_OP_RENAME = 21,
    FILED_WIRE_OP_MKDIR = 22,
    FILED_WIRE_OP_RMDIR = 23,
    FILED_WIRE_OP_SYMLINK = 24,
    FILED_WIRE_OP_READLINK = 25,
    FILED_WIRE_OP_LINK = 26,
    FILED_WIRE_OP_SEEK = 27,
    FILED_WIRE_OP_DUMP_METRICS = 28,
    FILED_WIRE_OP_SET_CACHE_SLOTS = 29,
    FILED_WIRE_OP_CONNECT = 30,
    FILED_WIRE_OP_SET_NETD_SOCKET_ENDPOINT = 31,
    FILED_WIRE_OP_SET_TERMD_TTY_ENDPOINT = 32,
    FILED_WIRE_OP_PING = 33,
    FILED_WIRE_OP_FAST_DOORBELL = 34,
    FILED_WIRE_OP_VALIDATE_OPEN_CACHE = 35,
    FILED_WIRE_OP_PWRITE_BATCH = 36,
    FILED_WIRE_OP_WRITE_BATCH = 37,
    FILED_WIRE_OP_PREAD_TO_VMO = 38,
    FILED_WIRE_OP_FILE_VMO = 39,
    FILED_WIRE_OP_SYNC_ALL = 40,

    FILED_WIRE_NAME_BYTES = 96,
    FILED_WIRE_PATH_BYTES = 480,
    FILED_WIRE_IO_BYTES = 7680,
    FILED_WIRE_SYMLINK_TARGET_BYTES = FILED_WIRE_IO_BYTES,
    FILED_WIRE_DIRENT_NAME_BYTES = 96,
    FILED_WIRE_DIRENT_CAPACITY = 16,
    FILED_WIRE_PAGE_BYTES = 8192,
    FILED_WIRE_SESSION_PAGE_BYTES = 40960,
    FILED_WIRE_FAST_MAGIC = 0x31545341464c4446ull,
    FILED_WIRE_FAST_VERSION = 1,
    FILED_WIRE_FAST_REQUEST_CAPACITY = 8,
    FILED_WIRE_FAST_COMPLETION_CAPACITY = 8,
    FILED_WIRE_FAST_PAYLOAD_SLOT_COUNT = 4,
    FILED_WIRE_FAST_PAYLOAD_OFFSET = 4096,
    FILED_WIRE_FAST_GENERATION_OFFSET =
        FILED_WIRE_FAST_PAYLOAD_OFFSET +
        FILED_WIRE_FAST_PAYLOAD_SLOT_COUNT * FILED_WIRE_PAGE_BYTES,
    FILED_WIRE_FAST_GENERATION_CAPACITY = 64,

    FILED_WIRE_RIGHT_LOOKUP = 1u << 0,
    FILED_WIRE_RIGHT_READ = 1u << 1,
    FILED_WIRE_RIGHT_WRITE = 1u << 2,
    FILED_WIRE_RIGHT_EXEC = 1u << 3,
    FILED_WIRE_RIGHT_STAT = 1u << 4,
    FILED_WIRE_RIGHT_GETDENTS = 1u << 5,
    FILED_WIRE_RIGHT_CREATE = 1u << 6,
    FILED_WIRE_RIGHT_REMOVE = 1u << 7,
    FILED_WIRE_RIGHT_RENAME = 1u << 8,

    FILED_WIRE_OPEN_CREATE = 1u << 0,
    FILED_WIRE_OPEN_EXCLUSIVE = 1u << 1,
    FILED_WIRE_OPEN_TRUNCATE = 1u << 2,
    FILED_WIRE_OPEN_DIRECTORY = 1u << 3,
    FILED_WIRE_OPEN_NOFOLLOW = 1u << 4,
    FILED_WIRE_OPEN_CLOEXEC = 1u << 5,
    FILED_WIRE_OPEN_APPEND = 1u << 6,
    FILED_WIRE_OPEN_NONBLOCK = 1u << 7,
    FILED_WIRE_OPEN_SYNC = 1u << 8,

    FILED_WIRE_FD_CLOEXEC = 1u << 0,

    FILED_WIRE_FILE_APPEND = 1u << 0,
    FILED_WIRE_FILE_NONBLOCK = 1u << 1,
    FILED_WIRE_FILE_SYNC = 1u << 2,

    FILED_WIRE_UTIMENS_ATIME = 1u << 0,
    FILED_WIRE_UTIMENS_MTIME = 1u << 1,

    FILED_WIRE_EXEC_BOOTSTRAP_FD = 1u << 0,
    FILED_WIRE_EXEC_INHERIT_FDS = 1u << 1,
    FILED_WIRE_EXEC_PATCH_BOOTSTRAP_FDS = 1u << 2,
    FILED_WIRE_EXEC_INHERIT_HANDLES = 1u << 3,
    FILED_WIRE_EXEC_LINUX_LPR = 1u << 4,
    FILED_WIRE_EXEC_MAX_INHERIT_FDS = 4,
    FILED_WIRE_EXEC_MAX_INHERIT_HANDLES = 4,
    FILED_WIRE_EXEC_MAX_FD_PATCHES = 4,
    FILED_WIRE_EXEC_MAX_ARGS = 128,
    FILED_WIRE_EXEC_MAX_ENVS = 64,
    FILED_WIRE_EXEC_STRING_BYTES = 6144,

    FILED_WIRE_EXEC_PATCH_INHERIT_FD = 1,
    FILED_WIRE_EXEC_PATCH_BOOTSTRAP_FD = 2,
    FILED_WIRE_EXEC_PATCH_INHERIT_HANDLE = 3,
};

typedef struct filed_wire_openat {
    uint64_t dir_handle;
    uint64_t rights;
    uint64_t open_flags;
    uint64_t object_generation;
    uint64_t dir_generation;
    uint64_t reserved0;
    char name[FILED_WIRE_PATH_BYTES];
} filed_wire_openat_t;

typedef struct filed_wire_validate_open_cache {
    uint64_t cached_handle;
    uint64_t dir_handle;
    uint64_t rights;
    uint64_t open_flags;
    uint64_t object_generation;
    uint64_t dir_generation;
    uint64_t reserved0;
    uint64_t reserved1;
    char name[FILED_WIRE_NAME_BYTES];
} filed_wire_validate_open_cache_t;

typedef struct filed_wire_io {
    uint64_t handle;
    uint64_t offset;
    uint64_t length;
    uint8_t data[FILED_WIRE_IO_BYTES];
} filed_wire_io_t;

typedef struct filed_wire_pread_vmo {
    uint64_t handle;
    uint64_t file_offset;
    uint64_t vmo_offset;
    uint64_t length;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_wire_pread_vmo_t;

typedef struct filed_wire_file_vmo {
    uint64_t handle;
    uint64_t file_offset;
    uint64_t length;
    uint64_t flags;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_wire_file_vmo_t;

typedef struct filed_wire_fast_header {
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
} filed_wire_fast_header_t;

typedef struct filed_wire_fast_request {
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
} filed_wire_fast_request_t;

typedef struct filed_wire_fast_completion {
    uint64_t request_id;
    int64_t status;
    uint64_t result;
    uint64_t bytes;
    uint64_t flags;
} filed_wire_fast_completion_t;

typedef struct filed_wire_generation_entry {
    uint64_t seq;
    uint64_t handle;
    uint64_t object_generation;
    uint64_t dir_generation;
} filed_wire_generation_entry_t;

typedef struct filed_wire_statx {
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
} filed_wire_statx_t;

typedef struct filed_wire_utimens {
    uint64_t handle;
    uint64_t mask;
    int64_t atime_sec;
    int64_t atime_nsec;
    int64_t mtime_sec;
    int64_t mtime_nsec;
} filed_wire_utimens_t;

typedef struct filed_wire_chmod {
    uint64_t handle;
    uint64_t mode;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_wire_chmod_t;

typedef struct filed_wire_truncate {
    uint64_t handle;
    uint64_t size;
    uint64_t reserved0;
    uint64_t reserved1;
} filed_wire_truncate_t;

typedef struct filed_wire_seek {
    uint64_t handle;
    int64_t offset;
    uint64_t whence;
    uint64_t reserved0;
} filed_wire_seek_t;

typedef struct filed_wire_unlink {
    uint64_t dir_handle;
    uint64_t reserved0;
    char name[FILED_WIRE_PATH_BYTES];
} filed_wire_unlink_t;

typedef struct filed_wire_mkdir {
    uint64_t dir_handle;
    uint64_t mode;
    char name[FILED_WIRE_PATH_BYTES];
} filed_wire_mkdir_t;

typedef struct filed_wire_rmdir {
    uint64_t dir_handle;
    uint64_t reserved0;
    char name[FILED_WIRE_PATH_BYTES];
} filed_wire_rmdir_t;

typedef struct filed_wire_symlink {
    uint64_t dir_handle;
    uint64_t target_length;
    char name[FILED_WIRE_PATH_BYTES];
    char target[FILED_WIRE_SYMLINK_TARGET_BYTES];
} filed_wire_symlink_t;

typedef struct filed_wire_readlink {
    uint64_t dir_handle;
    uint64_t target_length;
    char name[FILED_WIRE_PATH_BYTES];
    char target[FILED_WIRE_SYMLINK_TARGET_BYTES];
} filed_wire_readlink_t;

typedef struct filed_wire_link {
    uint64_t old_dir_handle;
    uint64_t new_dir_handle;
    uint64_t flags;
    uint64_t reserved0;
    char old_name[FILED_WIRE_PATH_BYTES];
    char new_name[FILED_WIRE_PATH_BYTES];
} filed_wire_link_t;

typedef struct filed_wire_rename {
    uint64_t old_dir_handle;
    uint64_t new_dir_handle;
    char old_name[FILED_WIRE_PATH_BYTES];
    char new_name[FILED_WIRE_PATH_BYTES];
} filed_wire_rename_t;

typedef struct filed_wire_handle_flags {
    uint64_t handle;
    uint64_t fd_flags;
    uint64_t status_flags;
    uint64_t reserved0;
} filed_wire_handle_flags_t;

typedef struct filed_wire_dirent {
    uint64_t handle;
    uint64_t kind;
    uint64_t name_len;
    char name[FILED_WIRE_DIRENT_NAME_BYTES];
} filed_wire_dirent_t;

typedef struct filed_wire_getdents {
    uint64_t dir_handle;
    uint64_t offset;
    uint64_t capacity;
    uint64_t count;
    uint64_t dir_generation;
    uint64_t reserved0;
    filed_wire_dirent_t entries[FILED_WIRE_DIRENT_CAPACITY];
} filed_wire_getdents_t;

typedef struct filed_wire_exec_fd_patch {
    uint64_t kind;
    uint64_t index;
    uint64_t offset;
    uint64_t reserved0;
} filed_wire_exec_fd_patch_t;

typedef struct filed_wire_exec_string_ref {
    uint16_t offset;
    uint16_t length;
} filed_wire_exec_string_ref_t;

typedef struct filed_wire_exec_path {
    uint64_t dir_handle;
    uint64_t flags;
    uint64_t inherit_fd_count;
    uint64_t fd_patch_count;
    uint64_t inherit_handle_count;
    uint64_t string_bytes;
    uint64_t argc;
    uint64_t envc;
    uint64_t inherit_handles[FILED_WIRE_EXEC_MAX_INHERIT_HANDLES];
    filed_wire_exec_fd_patch_t fd_patches[FILED_WIRE_EXEC_MAX_FD_PATCHES];
    char path[FILED_WIRE_PATH_BYTES];
    filed_wire_exec_string_ref_t argv[FILED_WIRE_EXEC_MAX_ARGS];
    filed_wire_exec_string_ref_t envp[FILED_WIRE_EXEC_MAX_ENVS];
    char strings[FILED_WIRE_EXEC_STRING_BYTES];
} filed_wire_exec_path_t;

static inline int filed_wire_exec_string_ref_valid(
    const filed_wire_exec_path_t *request,
    filed_wire_exec_string_ref_t ref)
{
    if (request == NULL ||
        request->string_bytes > FILED_WIRE_EXEC_STRING_BYTES ||
        ref.length == 0 ||
        ref.offset >= request->string_bytes ||
        (uint64_t)ref.offset + (uint64_t)ref.length > request->string_bytes)
    {
        return 0;
    }
    return request->strings[(uint64_t)ref.offset + (uint64_t)ref.length - 1u] == '\0';
}

static inline const char *filed_wire_exec_string(
    const filed_wire_exec_path_t *request,
    filed_wire_exec_string_ref_t ref)
{
    return &request->strings[ref.offset];
}
