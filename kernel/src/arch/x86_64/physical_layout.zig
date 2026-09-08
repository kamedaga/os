const std = @import("std");

// The low supervisor identity map has this many 1GiB PDP entries. All
// allocations dereferenced as raw physical pointers must fit within it.
pub const identity_pdp_entries: usize = 16;
pub const identity_limit: u64 = identity_pdp_entries * 1024 * 1024 * 1024;

pub fn kernelPointerPaddr(addr: u64, virtual_base: u64, physical_base: u64, image_size: u64) ?u64 {
    if (virtual_base >= 0xffff_8000_0000_0000 and physical_base != 0 and addr >= virtual_base) {
        const offset = addr - virtual_base;
        if (image_size == 0 or offset < image_size)
            return std.math.add(u64, physical_base, offset) catch null;
    }
    if (addr < identity_limit) return addr;
    return null;
}

test "identity translation spans 4GiB but stops at the mapped limit" {
    for ([_]u64{ 0, 0xffff_ffff, 0x100000000, 0x1233b4000, identity_limit - 1 }) |addr| {
        try std.testing.expectEqual(@as(?u64, addr), kernelPointerPaddr(addr, 0, 0, 0));
    }
    try std.testing.expectEqual(@as(?u64, null), kernelPointerPaddr(identity_limit, 0, 0, 0));
    try std.testing.expectEqual(@as(?u64, null), kernelPointerPaddr(0x10000000000, 0, 0, 0));
}

test "linked kernel image translation remains distinct from identity mapping" {
    const base = 0xffff_ffff_8000_0000;
    try std.testing.expectEqual(@as(?u64, 0x201000), kernelPointerPaddr(base + 0x1000, base, 0x200000, 0x3000));
    try std.testing.expectEqual(@as(?u64, null), kernelPointerPaddr(base + 0x3000, base, 0x200000, 0x3000));
    try std.testing.expectEqual(@as(?u64, null), kernelPointerPaddr(base - 1, base, 0x200000, 0x3000));
    try std.testing.expectEqual(@as(?u64, 0x1233b4000), kernelPointerPaddr(0x1233b4000, base, 0x200000, 0x3000));
    try std.testing.expectEqual(@as(?u64, null), kernelPointerPaddr(base + 0x1000, base, std.math.maxInt(u64), 0x3000));
}
