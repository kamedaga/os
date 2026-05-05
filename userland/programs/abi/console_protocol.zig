pub const page_bytes: usize = 4096;
pub const request_magic: u32 = 0x514E_4F43; // "CONQ"
pub const response_magic: u32 = 0x524E_4F43; // "CONR"
pub const version: u16 = 1;

pub const Opcode = enum(u16) {
    connect = 1,
    read = 2,
    write = 3,
    get_attr = 4,
    set_attr = 5,
};

pub const Status = enum(i32) {
    ok = 0,
    again = 1,
    invalid = 2,
    io_error = 3,
    not_connected = 4,
};

pub const ConsoleRequestHeader = extern struct {
    magic: u32 = request_magic,
    version: u16 = version,
    op: u16 = 0,
    request_seq: u64 = 0,
    session_nonce: u64 = 0,
    length: u32 = 0,
    flags: u32 = 0,
    arg0: u64 = 0,
    arg1: u64 = 0,
    arg2: u64 = 0,
    reserved0: u64 = 0,
};

pub const ConsoleResponseHeader = extern struct {
    magic: u32 = response_magic,
    version: u16 = version,
    op: u16 = 0,
    response_seq: u64 = 0,
    status: i32 = @intFromEnum(Status.ok),
    result_flags: u32 = 0,
    inline_bytes: u32 = 0,
    reserved0: u32 = 0,
    arg0: u64 = 0,
    arg1: u64 = 0,
    reserved1: u64 = 0,
    reserved2: u64 = 0,
};

pub const request_header_bytes = @sizeOf(ConsoleRequestHeader);
pub const response_header_bytes = @sizeOf(ConsoleResponseHeader);
pub const request_payload_bytes = page_bytes - request_header_bytes;
pub const response_payload_bytes = page_bytes - response_header_bytes;

pub fn opcodeRaw(op: Opcode) u16 {
    return @intFromEnum(op);
}

pub fn statusRaw(status: Status) i32 {
    return @intFromEnum(status);
}
