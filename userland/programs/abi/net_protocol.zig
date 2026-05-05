pub const page_bytes: usize = 4096;
pub const request_magic: u32 = 0x514E_4554; // "QNET"
pub const response_magic: u32 = 0x524E_4554; // "RNET"
pub const version: u16 = 1;

pub const Opcode = enum(u16) {
    connect = 1,
    get_status = 2,
};

pub const Status = enum(i32) {
    ok = 0,
    invalid = 2,
    not_connected = 4,
};

pub const flag_link_up: u32 = 1 << 0;
pub const flag_dhcp_bound: u32 = 1 << 1;
pub const flag_gateway_arp: u32 = 1 << 2;

pub const RequestHeader = extern struct {
    magic: u32 = request_magic,
    version: u16 = version,
    op: u16 = 0,
    request_seq: u64 = 0,
    session_nonce: u64 = 0,
    arg0: u64 = 0,
    arg1: u64 = 0,
    arg2: u64 = 0,
    reserved0: u64 = 0,
};

pub const ResponseHeader = extern struct {
    magic: u32 = response_magic,
    version: u16 = version,
    op: u16 = 0,
    response_seq: u64 = 0,
    status: i32 = @intFromEnum(Status.ok),
    inline_bytes: u32 = 0,
    arg0: u64 = 0,
    arg1: u64 = 0,
    arg2: u64 = 0,
    reserved0: u64 = 0,
};

pub const StatusPayload = extern struct {
    mac: [6]u8,
    link_up: u8,
    dhcp_bound: u8,
    ipv4_addr: u32,
    gateway_addr: u32,
    dns_addr: u32,
    dhcp_server_addr: u32,
    flags: u32,
    rx_packets: u64,
    tx_completions: u64,
};

pub const request_header_bytes = @sizeOf(RequestHeader);
pub const response_header_bytes = @sizeOf(ResponseHeader);
pub const request_payload_bytes = page_bytes - request_header_bytes;
pub const response_payload_bytes = page_bytes - response_header_bytes;

pub fn opcodeRaw(op: Opcode) u16 {
    return @intFromEnum(op);
}

pub fn statusRaw(status: Status) i32 {
    return @intFromEnum(status);
}
