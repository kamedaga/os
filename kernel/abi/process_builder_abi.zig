const std = @import("std");

pub const syscall_create_suspended_process: u64 = 0x41;
pub const syscall_map_vm_object_to_process: u64 = 0x42;
pub const syscall_alloc_map_pages_to_process: u64 = 0x43;
pub const syscall_set_process_initial_context: u64 = 0x44;
pub const syscall_start_process: u64 = 0x45;
pub const syscall_abort_process: u64 = 0x46;
pub const syscall_copy_to_process: u64 = 0x47;
pub const syscall_mprotect_self: u64 = 0x48;
pub const syscall_set_process_abi_trap_delegate: u64 = 0x4B;
pub const syscall_fork_abi_trap_reply_target: u64 = 0x53;
pub const syscall_clone_abi_trap_reply_target: u64 = 0x58;

pub const abi_trap_request_page_bytes: u64 = 4096;

pub const process_builder_token_tag: u64 = 1 << 60;
pub const process_builder_process_mask: u64 = (@as(u64, 1) << 32) - 1;
pub const copy_to_process_max_bytes: u64 = 64 * 1024 * 1024;

pub const MapProt = packed struct(u64) {
    read: bool = false,
    write: bool = false,
    exec: bool = false,
    _reserved: u61 = 0,
};

pub fn mapProtToBits(prot: MapProt) u64 {
    return @bitCast(prot);
}

pub fn mapProtFromBits(bits: u64) MapProt {
    return @bitCast(bits);
}

pub fn encodeProcessBuilderToken(process_slot: u64) u64 {
    std.debug.assert(process_slot != 0);
    std.debug.assert((process_slot & ~process_builder_process_mask) == 0);
    return process_builder_token_tag | process_slot;
}

pub fn decodeProcessBuilderToken(token: u64) ?u64 {
    if ((token & process_builder_token_tag) == 0) return null;
    const slot = token & process_builder_process_mask;
    if (slot == 0) return null;
    return slot;
}

test "process builder token round-trips" {
    const encoded = encodeProcessBuilderToken(42);
    try std.testing.expectEqual(@as(?u64, 42), decodeProcessBuilderToken(encoded));
    try std.testing.expectEqual(@as(?u64, null), decodeProcessBuilderToken(42));
}
