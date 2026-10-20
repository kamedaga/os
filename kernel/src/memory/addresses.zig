const std = @import("std");

pub const Kind = enum { physical, kernel_virtual, user_virtual, iova };

/// These types prevent implicit address-domain substitution, not forged raw
/// values or stale references. ABI/PTE/boot inputs still need their own checks.
fn Address(comptime domain: Kind) type {
    // A normal one-field struct is passed indirectly by Zig's internal call
    // lowering here. Keep nominal domains without adding pointer loads at
    // every VM/copy call; packed-u64 lowers like the original scalar. This is
    // an internal representation, not a shared/user ABI or lifetime proof.
    return packed struct(u64) {
        const Self = @This();
        pub const kind = domain;
        value: u64,

        pub inline fn fromInt(value: u64) Self {
            return .{ .value = value };
        }

        pub inline fn toInt(self: Self) u64 {
            return self.value;
        }

        pub fn offset(self: Self, bytes: u64) ?Self {
            return .fromInt(std.math.add(u64, self.value, bytes) catch return null);
        }

        /// Half-open extent; a zero-sized range may end exactly at the limit.
        pub fn rangeFits(self: Self, bytes: u64, limit: u64) bool {
            return self.value <= limit and bytes <= limit - self.value;
        }

        pub inline fn fromPointer(pointer: anytype) Self {
            if (domain != .kernel_virtual)
                @compileError("only kernel virtual addresses can be made from kernel pointers");
            return .fromInt(@intFromPtr(pointer));
        }
    };
}

pub const PhysicalAddress = Address(.physical);
pub const KernelVirtualAddress = Address(.kernel_virtual);
pub const UserVirtualAddress = Address(.user_virtual);
pub const IovaAddress = Address(.iova);

test "address domains remain distinct without adding storage" {
    try std.testing.expect(PhysicalAddress != KernelVirtualAddress);
    try std.testing.expect(PhysicalAddress != UserVirtualAddress);
    try std.testing.expect(KernelVirtualAddress != UserVirtualAddress);
    try std.testing.expect(IovaAddress != PhysicalAddress);
    inline for (.{ PhysicalAddress, KernelVirtualAddress, UserVirtualAddress, IovaAddress }) |T| {
        try std.testing.expectEqual(@sizeOf(u64), @sizeOf(T));
        try std.testing.expectEqual(@alignOf(u64), @alignOf(T));
        try std.testing.expectEqual(@as(u64, 4096), T.fromInt(4096).toInt());
    }
}

test "address extent and offset reject overflow and crossing the limit" {
    const address = PhysicalAddress.fromInt(4096);
    try std.testing.expect(address.rangeFits(4096, 8192));
    try std.testing.expect(!address.rangeFits(4097, 8192));
    try std.testing.expect(!address.rangeFits(0, 4095));
    try std.testing.expect(address.rangeFits(0, 4096));
    const last = UserVirtualAddress.fromInt(std.math.maxInt(u64));
    try std.testing.expect(last.offset(1) == null);
    try std.testing.expect(!last.rangeFits(1, std.math.maxInt(u64)));
    try std.testing.expectEqual(@as(u64, 8192), address.offset(4096).?.toInt());
}

test "kernel pointer identity stays in the virtual domain" {
    var value: u64 = 1;
    try std.testing.expectEqual(@as(u64, @intFromPtr(&value)), KernelVirtualAddress.fromPointer(&value).toInt());
}
