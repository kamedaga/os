const std = @import("std");

pub const syscall_grant_cap: u64 = 0x23;
pub const syscall_iommu_authorize: u64 = 0x35;
pub const syscall_command_authorize: u64 = 0x36;
pub const syscall_dma_map_create: u64 = 0x37;
pub const syscall_dma_map_set_state: u64 = 0x38;
pub const syscall_dma_map_release: u64 = 0x39;
pub const syscall_revoke_cap: u64 = 0x3A;
pub const syscall_derive_command_cap: u64 = 0x3B;

pub const DeviceId = u64;
pub const invalid_device_id: DeviceId = 0;

pub const DmaDirection = enum(u8) {
    read = 0,
    write = 1,
    bidirectional = 2,
};

pub const DmaMappingState = enum(u8) {
    mapped = 0,
    in_flight = 1,
    completed = 2,
};

pub const IommuOperation = enum(u8) {
    map_read = 0,
    map_write = 1,
    map_status = 2,
};

pub const CommandOpcodeClass = enum(u8) {
    blk_read = 0,
    blk_write = 1,
    blk_flush = 2,
    blk_identify = 3,
    gpu_admin = 4,
    gpu_resource_2d = 5,
    gpu_scanout = 6,
    gpu_cursor = 7,
    gpu_virgl_context = 8,
    gpu_virgl_resource = 9,
    gpu_virgl_submit = 10,
    gpu_fence = 11,
};

pub fn commandOpcodeBit(opcode: CommandOpcodeClass) u64 {
    return @as(u64, 1) << @as(u6, @intCast(@intFromEnum(opcode)));
}

pub fn commandOpcodeMask(opcodes: []const CommandOpcodeClass) u64 {
    var mask: u64 = 0;
    for (opcodes) |opcode| {
        mask |= commandOpcodeBit(opcode);
    }
    return mask;
}

pub const CapabilityKind = enum(u8) {
    iommu = 1,
    virtqueue = 2,
    command = 3,
};

const cap_token_tag_base: u64 = (1 << 62) | (1 << 60);
const cap_token_kind_shift: u6 = 56;
const cap_token_kind_mask: u64 = 0xF << cap_token_kind_shift;
const cap_token_payload_mask: u64 = ~(cap_token_tag_base | cap_token_kind_mask);
const dma_mapping_token_tag: u64 = (1 << 63) | (1 << 61);
const dma_mapping_token_payload_mask: u64 = ~dma_mapping_token_tag;

fn encodeCapToken(kind: CapabilityKind, token: u64) u64 {
    std.debug.assert(token != 0);
    std.debug.assert((token & ~cap_token_payload_mask) == 0);
    return cap_token_tag_base | (@as(u64, @intFromEnum(kind)) << cap_token_kind_shift) | token;
}

pub const DecodedCapabilityToken = struct {
    kind: CapabilityKind,
    token: u64,
};

pub fn decodeCapToken(value: u64) ?DecodedCapabilityToken {
    if ((value & cap_token_tag_base) != cap_token_tag_base) return null;
    const kind_raw = (value & cap_token_kind_mask) >> cap_token_kind_shift;
    const kind = std.meta.intToEnum(CapabilityKind, @as(u8, @intCast(kind_raw))) catch return null;
    const token = value & cap_token_payload_mask;
    if (token == 0) return null;
    return .{
        .kind = kind,
        .token = token,
    };
}

pub fn encodeIommuCapToken(token: u64) u64 {
    return encodeCapToken(.iommu, token);
}

pub fn decodeIommuCapToken(value: u64) ?u64 {
    const decoded = decodeCapToken(value) orelse return null;
    if (decoded.kind != .iommu) return null;
    return decoded.token;
}

pub fn encodeVirtqueueCapToken(token: u64) u64 {
    return encodeCapToken(.virtqueue, token);
}

pub fn decodeVirtqueueCapToken(value: u64) ?u64 {
    const decoded = decodeCapToken(value) orelse return null;
    if (decoded.kind != .virtqueue) return null;
    return decoded.token;
}

pub fn encodeCommandCapToken(token: u64) u64 {
    return encodeCapToken(.command, token);
}

pub fn decodeCommandCapToken(value: u64) ?u64 {
    const decoded = decodeCapToken(value) orelse return null;
    if (decoded.kind != .command) return null;
    return decoded.token;
}

pub fn encodeQueueCapToken(token: u64) u64 {
    return encodeVirtqueueCapToken(token);
}

pub fn decodeQueueCapToken(value: u64) ?u64 {
    return decodeVirtqueueCapToken(value);
}

pub fn encodeDmaMappingToken(token: u64) u64 {
    std.debug.assert(token != 0);
    std.debug.assert((token & ~dma_mapping_token_payload_mask) == 0);
    return dma_mapping_token_tag | token;
}

pub fn decodeDmaMappingToken(value: u64) ?u64 {
    if ((value & dma_mapping_token_tag) != dma_mapping_token_tag) return null;
    const token = value & dma_mapping_token_payload_mask;
    if (token == 0) return null;
    return token;
}
