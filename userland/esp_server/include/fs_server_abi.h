#ifndef CAPABILITYOS_FS_SERVER_ABI_H
#define CAPABILITYOS_FS_SERVER_ABI_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int i32;

enum {
    FS_PAGE_BYTES = 4096,
    FS_MAX_PATH_BYTES = 512,
    FS_REQUEST_MAGIC = 0x51534653u,
    FS_RESPONSE_MAGIC = 0x52534653u,
    FS_PROTOCOL_VERSION = 1,
};

enum fs_opcode {
    FS_OP_CONNECT = 1,
    FS_OP_LOOKUP = 16,
    FS_OP_OPEN = 17,
    FS_OP_READ = 18,
    FS_OP_READDIR = 19,
    FS_OP_STAT = 20,
    FS_OP_CLOSE = 21,
    FS_OP_CREATE = 22,
    FS_OP_WRITE = 23,
    FS_OP_UNLINK = 24,
    FS_OP_RENAME = 25,
    FS_OP_STATFS = 26,
    FS_OP_READ_BULK = 27,
    FS_OP_READDIR_BULK = 28,
    FS_OP_OPEN_EXEC = 32,
};

enum fs_status {
    FS_STATUS_OK = 0,
    FS_STATUS_INVALID = 1,
    FS_STATUS_NOT_FOUND = 2,
    FS_STATUS_NOT_DIR = 3,
    FS_STATUS_IS_DIR = 4,
    FS_STATUS_NO_RIGHT = 5,
    FS_STATUS_TOO_BIG = 6,
    FS_STATUS_NOT_SUPPORTED = 7,
    FS_STATUS_IO_ERROR = 8,
    FS_STATUS_BUSY = 9,
    FS_STATUS_END_OF_DIR = 10,
};

enum fs_object_kind {
    FS_OBJECT_NONE = 0,
    FS_OBJECT_MOUNT = 1,
    FS_OBJECT_DIRECTORY = 2,
    FS_OBJECT_FILE = 3,
    FS_OBJECT_OPEN_FILE = 4,
};

struct fs_request_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 request_seq;
    u64 object_token;
    u64 offset;
    u32 length;
    u32 flags;
    u16 path_bytes;
    u16 inline_bytes;
    u32 reserved0;
    u64 arg0;
    u64 arg1;
    u64 session_nonce;
};

struct fs_response_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 response_seq;
    i32 status;
    u32 result_flags;
    u64 result_token;
    u64 file_bytes;
    u64 cursor_next;
    u16 inline_bytes;
    u8 object_kind;
    u8 reserved0;
    u32 reserved1;
    u64 arg0;
    u64 arg1;
};

struct fs_stat_record {
    u8 object_kind;
    u8 reserved0[7];
    u64 size_bytes;
    u32 mode_bits;
    u32 reserved1;
    u64 mtime_unix_sec;
    u64 reserved2[2];
};

struct fs_dirent_record {
    u64 next_cursor;
    u8 object_kind;
    u8 reserved0[7];
    u16 name_bytes;
    u16 reserved1;
    u32 reserved2;
};

enum {
    FS_REQUEST_HEADER_BYTES = sizeof(struct fs_request_header),
    FS_RESPONSE_HEADER_BYTES = sizeof(struct fs_response_header),
    FS_REQUEST_PAYLOAD_BYTES = FS_PAGE_BYTES - sizeof(struct fs_request_header),
    FS_RESPONSE_PAYLOAD_BYTES = FS_PAGE_BYTES - sizeof(struct fs_response_header),
};

_Static_assert(sizeof(struct fs_request_header) == 72, "fs request ABI size changed");
_Static_assert(sizeof(struct fs_response_header) == 72, "fs response ABI size changed");

#endif
