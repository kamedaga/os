const std = @import("std");

const service_registry_abi = @import("service_registry_abi.zig");

pub const magic: u64 = 0x4C54_5043; // "CPTL"
pub const version: u64 = 1;
pub const endpoint_id: u64 = service_registry_abi.dynamic_endpoint_id_base + 0x10;

pub const status_flag_block_present: u64 = 1 << 0;
pub const status_flag_iommu_active: u64 = 1 << 1;
pub const status_flag_virtqueue_active: u64 = 1 << 2;
pub const status_flag_command_active: u64 = 1 << 3;
pub const status_flag_gpu_present: u64 = 1 << 4;

pub const Opcode = enum(u64) {
    status = 1,
    revoke_iommu = 2,
    revoke_virtqueue = 3,
    revoke_command = 4,
    profile_full = 5,
    profile_read_only = 6,
    profile_no_iommu = 7,
    profile_no_virtqueue = 8,
    launch_gpu = 9,
};

pub const BlockProfile = enum(u64) {
    full = 1,
    read_only = 2,
    no_iommu = 3,
    no_virtqueue = 4,
};

pub const ResponseStatus = enum(u64) {
    ok = 0,
    invalid = 1,
    unsupported = 2,
    unavailable = 3,
    already = 4,
    kernel_error = 5,
};

pub const Request = extern struct {
    magic: u64,
    version: u64,
    opcode: u64,
    request_seq: u64,
    response_paddr: u64,
    arg0: u64,
    arg1: u64,
    reserved0: u64,
};

pub const Response = extern struct {
    magic: u64,
    version: u64,
    opcode: u64,
    status: u64,
    response_seq: u64,
    detail: u64,
    block_process_slot: u64,
    block_endpoint_id: u64,
    status_flags: u64,
    block_profile: u64,
    gpu_process_slot: u64,
    gpu_endpoint_id: u64,
};

pub fn opcodeRaw(opcode: Opcode) u64 {
    return @intFromEnum(opcode);
}

pub fn decodeOpcode(raw: u64) ?Opcode {
    return std.meta.intToEnum(Opcode, raw) catch null;
}

pub fn responseStatusRaw(status: ResponseStatus) u64 {
    return @intFromEnum(status);
}

pub fn decodeResponseStatus(raw: u64) ?ResponseStatus {
    return std.meta.intToEnum(ResponseStatus, raw) catch null;
}

pub fn blockProfileRaw(profile: BlockProfile) u64 {
    return @intFromEnum(profile);
}

pub fn decodeBlockProfile(raw: u64) ?BlockProfile {
    return std.meta.intToEnum(BlockProfile, raw) catch null;
}
