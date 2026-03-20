const std = @import("std");

pub const syscall_spawn_exec: u64 = 0x1D;
pub const syscall_arm_deferred_compositor: u64 = 0x22;
pub const spawn_result_tag: u64 = 1 << 63;
pub const spawn_flag_bootstrap_page_writable: u64 = 1 << 0;
pub const spawn_flag_bootstrap_descriptor_table: u64 = 1 << 1;
pub const spawn_flag_bootstrap_extended_descriptor_table: u64 = 1 << 2;
pub const max_bootstrap_page_descriptors: usize = 136;
pub const max_bootstrap_cap_descriptors: usize = 8;
pub const aux_base_va: u64 = 0x3C00_0000;
pub const aux_page_bytes: u64 = 0x1000;
pub const standard_config_target_va: u64 = auxPageVa(2);

pub const BootstrapPageDescriptor = extern struct {
    source_va: u64,
    target_va: u64,
    flags: u64,
};

pub const BootstrapCapKind = enum(u8) {
    fs = 1,
    vm_object = 2,
};

pub const BootstrapCapDescriptor = extern struct {
    source_token: u64,
    target_token_va: u64,
    rights_bits: u64,
    kind: BootstrapCapKind,
    _reserved: [7]u8 = [_]u8{0} ** 7,
};

pub const BootstrapDescriptorTable = extern struct {
    page_count: u16 = 0,
    cap_count: u16 = 0,
    _reserved0: u32 = 0,
    page_descriptors: [max_bootstrap_page_descriptors]BootstrapPageDescriptor = [_]BootstrapPageDescriptor{.{
        .source_va = 0,
        .target_va = 0,
        .flags = 0,
    }} ** max_bootstrap_page_descriptors,
    cap_descriptors: [max_bootstrap_cap_descriptors]BootstrapCapDescriptor = [_]BootstrapCapDescriptor{.{
        .source_token = 0,
        .target_token_va = 0,
        .rights_bits = 0,
        .kind = .fs,
    }} ** max_bootstrap_cap_descriptors,
};

pub fn auxPageVa(page_index: u64) u64 {
    return aux_base_va + page_index * aux_page_bytes;
}

pub fn encodeSpawnedProcessSlot(process_slot: u64) u64 {
    std.debug.assert(process_slot != 0);
    std.debug.assert((process_slot & spawn_result_tag) == 0);
    return spawn_result_tag | process_slot;
}

pub fn decodeSpawnedProcessSlot(value: u64) ?u64 {
    if ((value & spawn_result_tag) == 0) return null;
    const process_slot = value & ~spawn_result_tag;
    if (process_slot == 0) return null;
    return process_slot;
}

test "spawned process slot round-trips" {
    const encoded = encodeSpawnedProcessSlot(10);
    try std.testing.expectEqual(@as(?u64, 10), decodeSpawnedProcessSlot(encoded));
    try std.testing.expectEqual(@as(?u64, null), decodeSpawnedProcessSlot(10));
}
