//! Boot-only DMA permission to firmware-reserved memory, never a PMM grant.
const std = @import("std");
const map = @import("arch/x86_64/direct_map.zig");
pub const Error = error{ InvalidRange, NotReserved };
pub const ReservedGrant = struct { start: u64, end: u64 };

pub fn platformLimit(host_bits: u16, cpu_limit: u64) Error!u64 {
    if (host_bits == 0 or host_bits > 64) return error.InvalidRange;
    return if (host_bits == 64) cpu_limit else @min(cpu_limit, @as(u64, 1) << @as(u6, @intCast(host_bits)));
}

/// Immutable boot-map provenance excludes RAM, modules, holes and the kernel.
/// This grants DMA translation only; no CPU cache policy or MMIO alias follows.
pub fn validate(regions: []const map.Region, image: map.Extent, start: u64, last: u64, limit: u64) Error!ReservedGrant {
    const end = std.math.add(u64, last, 1) catch return error.InvalidRange;
    if (start >= end or (start | end) & 4095 != 0 or end > limit or end > (@as(u64, 1) << 48)) return error.InvalidRange;
    if (start < image.end and image.start < end) return error.NotReserved;
    for (regions) |region| {
        const stop = std.math.add(u64, region.base, region.bytes) catch return error.InvalidRange;
        if (start < stop and region.base < end and
            (!region.firmware_reserved or region.usable or region.prepare_only)) return error.NotReserved;
    }
    var cursor = start;
    while (cursor < end) {
        var next = cursor;
        for (regions) |region| {
            const stop = region.base + region.bytes; // overflow checked above
            if (region.base <= cursor and stop > next) next = @min(end, stop);
        }
        if (next == cursor) return error.NotReserved;
        cursor = next;
    }
    return .{ .start = start, .end = end };
}

test "VT-d firmware grant requires complete reserved provenance and excludes owned pages" {
    const good = [_]map.Region{
        .{ .base = 0x1000, .bytes = 0x2000, .usable = false, .firmware_reserved = true },
        .{ .base = 0x3000, .bytes = 0x1000, .usable = false, .firmware_reserved = true },
    };
    const image = map.Extent{ .start = 0x8000, .end = 0xa000 };
    const grant = try validate(&good, image, 0x1000, 0x3fff, 0x10000);
    try std.testing.expectEqual(@as(u64, 0x4000), grant.end);
    try std.testing.expectError(error.NotReserved, validate(&good, image, 0x1000, 0x4fff, 0x10000));
    try std.testing.expectError(error.NotReserved, validate(&good, .{ .start = 0x2000, .end = 0x3000 }, 0x1000, 0x3fff, 0x10000));
    for (0..3) |kind| {
        var bad = good;
        if (kind == 0) bad[1].firmware_reserved = false;
        if (kind == 1) bad[1].usable = true;
        if (kind == 2) bad[1].prepare_only = true;
        try std.testing.expectError(error.NotReserved, validate(&bad, image, 0x1000, 0x3fff, 0x10000));
    }
    try std.testing.expectError(error.InvalidRange, validate(&good, image, 0x1001, 0x3fff, 0x10000));
    try std.testing.expectError(error.InvalidRange, validate(&good, image, 0x1000, std.math.maxInt(u64), 0x10000));
    try std.testing.expectError(error.InvalidRange, validate(&good, image, 0x1000, 0x3fff, 0x2000));
}

test "VT-d firmware grant includes DMAR host width independently of CPU and MGAW" {
    const limit = try platformLimit(39, @as(u64, 1) << 48);
    const regions = [_]map.Region{.{ .base = limit - 4096, .bytes = 8192, .usable = false, .firmware_reserved = true }};
    const image = map.Extent{ .start = 0, .end = 0 };
    _ = try validate(&regions, image, limit - 4096, limit - 1, limit);
    try std.testing.expectError(error.InvalidRange, validate(&regions, image, limit, limit + 4095, limit));
    try std.testing.expectError(error.InvalidRange, platformLimit(65, limit));
}
