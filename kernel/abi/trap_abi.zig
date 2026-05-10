const std = @import("std");

pub const magic: u64 = 0x3149_4241_5041_5254; // TRAPABI1
pub const version: u32 = 1;

pub const TrapKind = enum(u32) {
    abi_syscall = 1,
    page_fault = 2,
    illegal_instruction = 3,
    breakpoint = 4,
    protection_fault = 5,
};

pub const AbiFlavor = enum(u32) {
    native = 0,
    linux_x86_64 = 1,
    linux_i386 = 2,
    wasm_hostcall = 3,
    debug = 4,
};

pub const TrapAction = enum(u32) {
    resume_thread = 0,
    fail = 1,
    kill = 2,
    block = 3,
    restart = 4,
};

pub const response_flag_exit: u64 = 1 << 0;
pub const response_flag_block: u64 = 1 << 1;
pub const response_flag_restart: u64 = 1 << 2;

pub const max_args: usize = 6;
pub const syscall_set_abi_trap_delegate: u64 = 0x49;
pub const syscall_clear_abi_trap_delegate: u64 = 0x4A;
pub const syscall_map_abi_trap_reply_target_pages: u64 = 0x4C;
pub const syscall_copy_from_abi_trap_reply_target: u64 = 0x4D;
pub const syscall_copy_to_abi_trap_reply_target: u64 = 0x4E;
pub const syscall_set_abi_trap_reply_target_fs_base: u64 = 0x4F;
pub const syscall_protect_abi_trap_reply_target_pages: u64 = 0x50;
pub const syscall_unmap_abi_trap_reply_target_pages: u64 = 0x51;
pub const syscall_reclaim_abi_trap_reply_target_private_pages: u64 = 0x52;
pub const syscall_reply_abi_trap_target: u64 = 0x54;
pub const syscall_copy_to_abi_trap_target: u64 = 0x55;
pub const syscall_start_abi_trap_target: u64 = 0x56;
pub const syscall_set_abi_trap_target_request_page: u64 = 0x57;
pub const syscall_detach_abi_trap_reply_token: u64 = 0x59;
pub const syscall_share_abi_trap_reply_target_pages_to_target: u64 = 0x5A;
pub const syscall_unmap_abi_trap_target_pages: u64 = 0x5B;
pub const abi_trap_copy_max_bytes: usize = 4096;
pub const delegate_target_token_tag: u64 = 0x3 << 60;
pub const delegate_target_token_tag_mask: u64 = 0xF << 60;
pub const delegate_target_slot_bits: u6 = 32;
pub const delegate_target_generation_bits: u6 = 28;
pub const delegate_target_slot_mask: u64 = (@as(u64, 1) << delegate_target_slot_bits) - 1;
pub const delegate_target_generation_mask: u64 = (@as(u64, 1) << delegate_target_generation_bits) - 1;
pub const delegate_target_generation_shift: u6 = delegate_target_slot_bits;

pub const DelegateTargetToken = struct {
    process_slot: u64,
    generation: u64,
};

pub fn encodeDelegateTargetToken(process_slot: u64, generation: u64) u64 {
    std.debug.assert(process_slot != 0);
    std.debug.assert(generation != 0);
    std.debug.assert((process_slot & ~delegate_target_slot_mask) == 0);
    std.debug.assert((generation & ~delegate_target_generation_mask) == 0);
    return delegate_target_token_tag |
        process_slot |
        (generation << delegate_target_generation_shift);
}

pub fn decodeDelegateTargetToken(token: u64) ?DelegateTargetToken {
    if ((token & delegate_target_token_tag_mask) != delegate_target_token_tag) return null;
    const slot = token & delegate_target_slot_mask;
    const generation = (token >> delegate_target_generation_shift) & delegate_target_generation_mask;
    if (slot == 0 or generation == 0) return null;
    return .{
        .process_slot = slot,
        .generation = generation,
    };
}

pub const TrapRequest = extern struct {
    magic: u64 = magic,
    version: u32 = version,
    kind: u32,
    flavor: u32,
    reserved0: u32 = 0,
    caller_principal: u64,
    thread_id: u64,
    rip: u64,
    rsp: u64,
    fault_addr: u64,
    error_code: u64,
    nr: u64,
    args: [max_args]u64,
};

pub const TrapResponse = extern struct {
    magic: u64 = magic,
    version: u32 = version,
    action: u32,
    flags: u64,
    result: u64,
    new_rip: u64,
    new_rsp: u64,
};

test "delegate target token round-trips" {
    const encoded = encodeDelegateTargetToken(12, 7);
    const decoded = decodeDelegateTargetToken(encoded).?;
    try std.testing.expectEqual(@as(u64, 12), decoded.process_slot);
    try std.testing.expectEqual(@as(u64, 7), decoded.generation);
    try std.testing.expectEqual(@as(?DelegateTargetToken, null), decodeDelegateTargetToken(12));
    try std.testing.expectEqual(@as(?DelegateTargetToken, null), decodeDelegateTargetToken(delegate_target_token_tag | 12));
    try std.testing.expectEqual(
        @as(?DelegateTargetToken, null),
        decodeDelegateTargetToken(delegate_target_token_tag | (@as(u64, 7) << delegate_target_generation_shift)),
    );
}
