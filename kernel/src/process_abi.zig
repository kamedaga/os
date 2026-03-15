const std = @import("std");

pub const syscall_spawn_exec: u64 = 0x1D;
pub const spawn_result_tag: u64 = 1 << 63;
pub const spawn_flag_bootstrap_page_writable: u64 = 1 << 0;
pub const spawn_flag_bootstrap_descriptor_table: u64 = 1 << 1;
pub const max_bootstrap_page_descriptors: usize = 8;

pub const BootstrapPageDescriptor = extern struct {
    source_va: u64,
    target_va: u64,
    flags: u64,
};

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
