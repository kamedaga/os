const std = @import("std");

pub const max_cpus = @bitSizeOf(u64);
// Separate supervisor-only high-half PDP entry, outside the kernel image.
pub const base: u64 = 0xffff_ffff_c000_0000;
pub const stride: usize = 4 * 1024 * 1024;
pub const kernel_bytes: usize = 2 * 1024 * 1024;
pub const ist_bytes: usize = 256 * 1024;
pub const backing_bytes: usize = kernel_bytes + 2 * ist_bytes;
pub const offsets = [_]usize{ 4096, kernel_bytes + 8192, kernel_bytes + ist_bytes + 12288 };
pub const sizes = [_]usize{ kernel_bytes, ist_bytes, ist_bytes };

pub fn stackBase(cpu: usize, stack: usize) ?u64 {
    if (cpu == 0 or cpu >= max_cpus or stack >= sizes.len) return null;
    return base + (cpu - 1) * stride + offsets[stack];
}

test "all 63 AP stack slots are disjoint, guarded and inside one PDP entry" {
    try std.testing.expectEqual(@as(usize, 64), max_cpus);
    try std.testing.expectEqual(@as(?u64, null), stackBase(0, 0));
    try std.testing.expectEqual(@as(?u64, null), stackBase(64, 0));
    try std.testing.expectEqual(@as(?u64, null), stackBase(1, 3));
    var previous_end = base;
    for (1..max_cpus) |cpu| {
        for (sizes, 0..) |bytes, stack| {
            const start = stackBase(cpu, stack).?;
            try std.testing.expect(start >= previous_end + 4096);
            try std.testing.expectEqual(@as(u64, 0), start & 4095);
            try std.testing.expect((start >> 30) == (base >> 30));
            previous_end = start + bytes;
            try std.testing.expect(previous_end + 4096 <= base + cpu * stride);
        }
    }
}
