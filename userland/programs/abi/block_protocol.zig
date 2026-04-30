const fs_abi = @import("fs_abi.zig");

pub const page_bytes: usize = 4096;
pub const request_magic: u32 = 0x514B_4C42; // "BLKQ"
pub const response_magic: u32 = 0x524B_4C42; // "BLKR"
pub const version: u16 = 1;

pub const Opcode = enum(u16) {
    connect = 1,
    identify = 2,
    read_blocks = 3,
    write_blocks = 4,
    flush = 5,
    read_blocks_bulk = 6,
};

pub const Status = enum(i32) {
    ok = 0,
    invalid = 1,
    not_found = 2,
    no_right = 3,
    too_big = 4,
    io_error = 5,
    busy = 6,
};

pub const BlockRequestHeader = extern struct {
    magic: u32 = request_magic,
    version: u16 = version,
    op: u16 = 0,
    request_seq: u64 = 0,
    object_token: u64 = 0,
    block_index: u64 = 0,
    block_count: u32 = 0,
    flags: u32 = 0,
    inline_bytes: u16 = 0,
    reserved0: u16 = 0,
    reserved1: u32 = 0,
    arg0: u64 = 0,
    arg1: u64 = 0,
    session_nonce: u64 = 0,
};

pub const BlockResponseHeader = extern struct {
    magic: u32 = response_magic,
    version: u16 = version,
    op: u16 = 0,
    response_seq: u64 = 0,
    status: i32 = @intFromEnum(Status.ok),
    result_flags: u32 = 0,
    result_token: u64 = 0,
    inline_bytes: u16 = 0,
    object_kind: u8 = 0,
    reserved0: u8 = 0,
    reserved1: u32 = 0,
    arg0: u64 = 0,
    arg1: u64 = 0,
};

pub const request_header_bytes = @sizeOf(BlockRequestHeader);
pub const response_header_bytes = @sizeOf(BlockResponseHeader);
pub const request_payload_bytes = page_bytes - request_header_bytes;
pub const response_payload_bytes = page_bytes - response_header_bytes;

pub fn opcodeRaw(op: Opcode) u16 {
    return @intFromEnum(op);
}

pub fn statusRaw(status: Status) i32 {
    return @intFromEnum(status);
}

pub fn objectKindRaw(kind: fs_abi.ObjectKind) u8 {
    return @intFromEnum(kind);
}
