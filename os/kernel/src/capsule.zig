const std = @import("std");
const abi_root = @import("kernel_abi_root");
const capsule_abi = abi_root.capsule_abi;

pub const known_rights_mask: u64 = (1 << 13) - 1;

pub const CapsuleKind = enum(u8) {
    none = 0,
    session = 1,
    device = 2,
    mmio = 3,
    dma_buffer = 4,
    dma_mapping = 5,
    irq = 6,
    event_queue = 7,
};

pub const CapsuleState = enum(u8) {
    empty = 0,
    active = 1,
    revoked = 2,
};

comptime {
    std.debug.assert(known_rights_mask == capsule_abi.known_rights_mask);
    std.debug.assert(@intFromEnum(CapsuleKind.session) == @intFromEnum(capsule_abi.CapsuleKind.session));
    std.debug.assert(@intFromEnum(CapsuleKind.device) == @intFromEnum(capsule_abi.CapsuleKind.device));
    std.debug.assert(@intFromEnum(CapsuleKind.mmio) == @intFromEnum(capsule_abi.CapsuleKind.mmio));
    std.debug.assert(@intFromEnum(CapsuleKind.dma_buffer) == @intFromEnum(capsule_abi.CapsuleKind.dma_buffer));
    std.debug.assert(@intFromEnum(CapsuleKind.dma_mapping) == @intFromEnum(capsule_abi.CapsuleKind.dma_mapping));
    std.debug.assert(@intFromEnum(CapsuleKind.irq) == @intFromEnum(capsule_abi.CapsuleKind.irq));
    std.debug.assert(@intFromEnum(CapsuleKind.event_queue) == @intFromEnum(capsule_abi.CapsuleKind.event_queue));
    std.debug.assert(@intFromEnum(CapsuleState.active) == @intFromEnum(capsule_abi.CapsuleState.active));
    std.debug.assert(@intFromEnum(CapsuleState.revoked) == @intFromEnum(capsule_abi.CapsuleState.revoked));
}

pub const Rights = packed struct(u64) {
    query: bool = false,
    config_read: bool = false,
    config_write: bool = false,
    bar_info: bool = false,
    bar_map: bool = false,
    dma_alloc: bool = false,
    dma_map_user: bool = false,
    irq_bind: bool = false,
    bus_master: bool = false,
    reset: bool = false,
    power: bool = false,
    hotplug_observe: bool = false,
    grant: bool = false,
    _reserved: u51 = 0,
};

pub const Metadata = struct {
    device: u64 = 0,
    object_id: u64 = 0,
    user_va: u64 = 0,
    iova: u64 = 0,
    size: u64 = 0,
    index: u32 = 0,
    flags: u32 = 0,
};

pub const Snapshot = struct {
    fd: u64 = 0,
    owner_principal_raw: u32 = 0,
    kind: CapsuleKind = .none,
    state: CapsuleState = .empty,
    rights: Rights = .{},
    metadata: Metadata = .{},
};

pub const DmaDirection = enum(u2) {
    to_device = 1,
    from_device = 2,
    bidirectional = 3,
};

pub const IrqKind = enum(u2) {
    auto = 0,
    intx = 1,
    msi = 2,
    msix = 3,
};

pub fn rightsFromBits(bits: u64) Rights {
    return @bitCast(bits & known_rights_mask);
}

pub fn rightsToBits(rights: Rights) u64 {
    return @as(u64, @bitCast(rights)) & known_rights_mask;
}

pub fn isRightsSubset(child: Rights, parent: Rights) bool {
    const child_bits = rightsToBits(child);
    const parent_bits = rightsToBits(parent);
    return (child_bits & ~parent_bits) == 0;
}

test "capsule rights remain an ABI bitmask only" {
    const parent = rightsFromBits(std.math.maxInt(u64));
    const child = Rights{ .query = true, .config_read = true };

    try std.testing.expectEqual(known_rights_mask, rightsToBits(parent));
    try std.testing.expect(isRightsSubset(child, parent));
}
