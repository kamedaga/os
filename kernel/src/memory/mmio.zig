const std = @import("std");
pub const addresses = @import("addresses.zig");

pub const Cache = enum { uc, wc };

/// A borrowed, already validated device mapping. The owner proves range,
/// effective memory type and lifetime before construction; this is not a RAM
/// pointer conversion or authority to access an arbitrary physical page.
/// Temporary copy-window borrows require the pinned, IRQ-disabled guard.
/// Persistent device borrows last only as long as their kernel mapping owner.
pub const Borrow = struct {
    base: [*]volatile u8,
    length: usize,
    cache: Cache,

    pub inline fn pointer(self: *const Borrow, comptime T: type, offset: usize) *volatile T {
        std.debug.assert(offset <= self.length and @sizeOf(T) <= self.length - offset);
        return @ptrCast(@alignCast(self.base + offset));
    }

    pub inline fn read(self: *const Borrow, comptime T: type, offset: usize) T {
        return self.pointer(T, offset).*;
    }

    pub inline fn write(self: *const Borrow, comptime T: type, offset: usize, value: T) void {
        self.pointer(T, offset).* = value;
    }
};

/// Only mapping owners may call these after their existing MMIO proof. The
/// closed boundary checker audits those call sites; nominal types alone do not
/// prevent forging a borrow. Neither function establishes cache attributes.
pub inline fn mappedAfterValidation(base: [*]u8, length: usize, cache: Cache) Borrow {
    return .{ .base = base, .length = length, .cache = cache };
}

test "MMIO borrowed host mapping keeps scalar widths and offsets" {
    var storage: [32]u8 align(8) = @splat(0);
    const mapped = mappedAfterValidation(&storage, storage.len, .uc);
    mapped.write(u8, 1, 0x12);
    mapped.write(u16, 2, 0x3456);
    mapped.write(u32, 4, 0x789abcde);
    mapped.write(u64, 8, 0xfedcba9876543210);
    try std.testing.expectEqual(@as(u8, 0x12), mapped.read(u8, 1));
    try std.testing.expectEqual(@as(u16, 0x3456), mapped.read(u16, 2));
    try std.testing.expectEqual(@as(u32, 0x789abcde), mapped.read(u32, 4));
    try std.testing.expectEqual(@as(u64, 0xfedcba9876543210), mapped.read(u64, 8));
    try std.testing.expectEqual(Cache.uc, mapped.cache);
}

/// Kernel-only slot 510. Copy windows own the first 2 MiB; device/firmware
/// mappings use allocated VA extents, independently of the physical address.
pub const window_base: u64 = 0xffff_ff00_0000_0000;
pub const window_bytes: u64 = @as(u64, 1) << 39;
