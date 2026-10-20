//! Fixed logarithmic distribution shared by every profile series. Integer
//! buckets are exact through 15 cycles, then have eight subdivisions/octave.
const std = @import("std");
pub const bucket_count = 496;
pub fn bucket(value: u64) usize {
    if (value < 16) return @intCast(value);
    const exponent: u6 = @intCast(63 - @clz(value));
    return 16 + (@as(usize, exponent) - 4) * 8 + @as(usize, @intCast((value >> (exponent - 3)) & 7));
}
pub const Histogram = extern struct {
    buckets: [bucket_count]u32 = @splat(0),
    count: u64 = 0,
    cycles: u64 = 0,
    maximum: u64 = 0,
    exclusive_cycles: u64 = 0,
    excluded: u64 = 0,
    overflow: u64 = 0,
    pub fn record(self: *Histogram, elapsed: u64, exclusive: u64) void {
        const old = @atomicRmw(u32, &self.buckets[bucket(elapsed)], .Add, 1, .monotonic);
        if (old == std.math.maxInt(u32)) @atomicStore(u64, &self.overflow, 1, .monotonic);
        _ = @atomicRmw(u64, &self.count, .Add, 1, .monotonic);
        _ = @atomicRmw(u64, &self.cycles, .Add, elapsed, .monotonic);
        _ = @atomicRmw(u64, &self.maximum, .Max, elapsed, .monotonic);
        _ = @atomicRmw(u64, &self.exclusive_cycles, .Add, exclusive, .monotonic);
    }
};
test "histogram all octave boundaries and maximum fit" {
    try std.testing.expectEqual(@as(usize, 0), bucket(0));
    for (0..16) |i| try std.testing.expectEqual(i, bucket(i));
    for (4..64) |e| {
        const shift: u6 = @intCast(e);
        const base = @as(u64, 1) << shift;
        for (0..8) |sub| {
            const lo = base + (@as(u64, @intCast(sub)) << (shift - 3));
            try std.testing.expectEqual(16 + (e - 4) * 8 + sub, bucket(lo));
            const hi = lo + ((@as(u64, 1) << (shift - 3)) - 1);
            try std.testing.expectEqual(bucket(lo), bucket(hi));
        }
    }
    try std.testing.expectEqual(bucket_count - 1, bucket(std.math.maxInt(u64)));
}
test "histogram accumulates exact max exclusive and detects overflow" {
    var h: Histogram = .{};
    h.record(27, 12);
    h.record(19, 10);
    try std.testing.expectEqual(@as(u64, 2), h.count);
    try std.testing.expectEqual(@as(u64, 46), h.cycles);
    try std.testing.expectEqual(@as(u64, 27), h.maximum);
    try std.testing.expectEqual(@as(u64, 22), h.exclusive_cycles);
    h.buckets[bucket(27)] = std.math.maxInt(u32);
    h.record(27, 27);
    try std.testing.expectEqual(@as(u64, 1), h.overflow);
}
