const fs_abi = @import("fs_abi.zig");

pub const page_bytes: usize = 4096;
pub const max_path_bytes: usize = 512;
pub const request_magic: u32 = 0x5153_4653; // "QSFS"
pub const response_magic: u32 = 0x5253_4653; // "RSFS"
pub const version: u16 = 1;
pub const create_flag_directory: u32 = 1 << 0;

pub const Opcode = enum(u16) {
    connect = 1,
    lookup = 16,
    open = 17,
    read = 18,
    readdir = 19,
    stat = 20,
    close = 21,
    create = 22,
    write = 23,
    unlink = 24,
    rename = 25,
    statfs = 26,
    // Reserved legacy opcodes. PersistentFs replies not_supported; the
    // supported FS contract is blocking inline read/write.
    read_bulk = 27,
    readdir_bulk = 28,
    open_exec = 32,
};

pub const Status = enum(i32) {
    ok = 0,
    invalid = 1,
    not_found = 2,
    not_dir = 3,
    is_dir = 4,
    no_right = 5,
    too_big = 6,
    not_supported = 7,
    io_error = 8,
    busy = 9,
    end_of_dir = 10,
};

pub const ObjectKind = fs_abi.ObjectKind;

pub const FsRequestHeader = extern struct {
    magic: u32 = request_magic,
    version: u16 = version,
    op: u16 = 0,
    request_seq: u64 = 0,
    object_token: u64 = 0,
    offset: u64 = 0,
    length: u32 = 0,
    flags: u32 = 0,
    path_bytes: u16 = 0,
    inline_bytes: u16 = 0,
    reserved0: u32 = 0,
    arg0: u64 = 0,
    arg1: u64 = 0,
    session_nonce: u64 = 0,
};

pub const FsResponseHeader = extern struct {
    magic: u32 = response_magic,
    version: u16 = version,
    op: u16 = 0,
    response_seq: u64 = 0,
    status: i32 = @intFromEnum(Status.ok),
    result_flags: u32 = 0,
    result_token: u64 = 0,
    file_bytes: u64 = 0,
    cursor_next: u64 = 0,
    inline_bytes: u16 = 0,
    object_kind: u8 = 0,
    reserved0: u8 = 0,
    reserved1: u32 = 0,
    arg0: u64 = 0,
    arg1: u64 = 0,
};

pub const FsStatRecord = extern struct {
    object_kind: u8 = 0,
    reserved0: [7]u8 = [_]u8{0} ** 7,
    size_bytes: u64 = 0,
    mode_bits: u32 = 0,
    reserved1: u32 = 0,
    mtime_unix_sec: u64 = 0,
    reserved2: [2]u64 = [_]u64{0} ** 2,
};

pub const FsDirentRecord = extern struct {
    next_cursor: u64 = 0,
    object_kind: u8 = 0,
    reserved0: [7]u8 = [_]u8{0} ** 7,
    name_bytes: u16 = 0,
    reserved1: u16 = 0,
    reserved2: u32 = 0,
};

pub const request_header_bytes = @sizeOf(FsRequestHeader);
pub const response_header_bytes = @sizeOf(FsResponseHeader);
pub const request_payload_bytes = page_bytes - request_header_bytes;
pub const response_payload_bytes = page_bytes - response_header_bytes;
pub const stat_record_bytes = @sizeOf(FsStatRecord);
pub const dirent_record_bytes = @sizeOf(FsDirentRecord);

pub fn opcodeRaw(op: Opcode) u16 {
    return @intFromEnum(op);
}

pub fn statusRaw(status: Status) i32 {
    return @intFromEnum(status);
}

pub fn objectKindRaw(kind: ObjectKind) u8 {
    return @intFromEnum(kind);
}
