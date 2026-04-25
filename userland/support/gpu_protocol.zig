const service_registry_abi = @import("service_registry_abi.zig");

pub const page_bytes: usize = 4096;
pub const request_magic: u32 = 0x5147_5047; // "GPGQ"
pub const response_magic: u32 = 0x5247_5047; // "GPGR"
pub const version: u16 = 1;
pub const endpoint_id: u64 = service_registry_abi.dynamic_endpoint_id_base + 0x20;

pub const feature_virgl: u64 = 1 << 0;
pub const feature_submit_3d: u64 = 1 << 1;
pub const feature_present_2d: u64 = 1 << 2;
pub const feature_present_3d: u64 = 1 << 3;
pub const feature_texture_2d: u64 = 1 << 4;
pub const feature_app_surface: u64 = 1 << 5;
pub const feature_cursor: u64 = 1 << 6;

pub const default_virgl_resource_id: u32 = 2;
pub const default_virgl_vertex_buffer_id: u32 = 3;
pub const first_virgl_texture_resource_id: u32 = 4;

pub const Opcode = enum(u16) {
    query_caps = 1,
    submit_nop = 2,
    submit_3d = 3,
    present_test_pattern = 4,
    prepare_3d = 5,
    present_3d = 6,
    upload_texture_2d = 7,
    update_texture_2d = 8,
    delete_texture_2d = 9,
    create_app_surface = 10,
    set_cursor_position = 11,
};

pub const Status = enum(i32) {
    ok = 0,
    invalid = 1,
    unavailable = 2,
    io_error = 3,
};

pub const RequestHeader = extern struct {
    magic: u32 = request_magic,
    version: u16 = version,
    op: u16 = 0,
    request_seq: u64 = 0,
    response_paddr: u64 = 0,
    arg0: u64 = 0,
    arg1: u64 = 0,
    inline_bytes: u32 = 0,
    reserved0: u32 = 0,
};

pub const ResponseHeader = extern struct {
    magic: u32 = response_magic,
    version: u16 = version,
    op: u16 = 0,
    response_seq: u64 = 0,
    status: i32 = @intFromEnum(Status.ok),
    result_flags: u32 = 0,
    arg0: u64 = 0,
    arg1: u64 = 0,
    arg2: u64 = 0,
    inline_bytes: u32 = 0,
    reserved0: u32 = 0,
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
