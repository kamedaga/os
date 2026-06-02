const std = @import("std");

pub const abi_process_native_syscall_tag: u64 = 0x50414348_CA000000; // "PACH" + Linux-low32 escape class.
pub const abi_process_native_syscall_mask: u64 = 0xFFFF_FFFF_FF00_0000;
pub const native_syscall_number_mask: u64 = 0x0000_0000_00FF_FFFF;

pub fn encodeAbiProcessNativeSyscall(native_nr: u64) u64 {
    std.debug.assert((native_nr & ~native_syscall_number_mask) == 0);
    return abi_process_native_syscall_tag | native_nr;
}

pub fn decodeAbiProcessNativeSyscall(nr: u64) ?u64 {
    if ((nr & abi_process_native_syscall_mask) != abi_process_native_syscall_tag) return null;
    return nr & native_syscall_number_mask;
}

test "abi process native syscall escape is tagged" {
    const encoded = encodeAbiProcessNativeSyscall(0x70);
    try std.testing.expectEqual(@as(u64, 0x50414348_CA000070), encoded);
    try std.testing.expectEqual(@as(?u64, 0x70), decodeAbiProcessNativeSyscall(encoded));
    try std.testing.expectEqual(@as(?u64, null), decodeAbiProcessNativeSyscall(0x70));
}
