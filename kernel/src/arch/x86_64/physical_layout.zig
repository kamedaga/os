const std = @import("std");
const direct_map = @import("direct_map.zig");
const addresses = @import("../../memory/addresses.zig");
pub const PhysicalAddress = addresses.PhysicalAddress;
pub const KernelVirtualAddress = addresses.KernelVirtualAddress;

// Written once during serial CR3 activation, before APs or user dispatch.
// Ownership-aware callers prove the RAM extent; this boundary checks address
// arithmetic without a memmap walk on every hot-path access.
var direct_active = false;
var direct_limit: u64 = 0;
var active_image: KernelImage = .{ .virtual_base = .fromInt(0), .physical_base = .fromInt(0), .size_bytes = 0 };
var returned_image: direct_map.Extent = .{ .start = 0, .end = 0 };
// Shared DMA32 / low-RAM boundary, also used by the PMM cache interval table.
pub const low_memory_limit: u64 = 1 << 32;
pub const allocation_limit = direct_map.aperture_bytes;

pub fn activateDirect(limit: u64, image: KernelImage) void {
    std.debug.assert(!direct_active);
    direct_limit = limit;
    active_image = image;
    direct_active = true;
}

pub fn returnImageExtent(start: u64, end: u64) void {
    returned_image = .{ .start = start, .end = end };
}

inline fn tableVirtual(physical: u64, bytes: usize) ?u64 {
    if (@import("builtin").is_test and !direct_active)
        return if (physical < (@as(u64, 1) << 52) and bytes <= (@as(u64, 1) << 52) - physical) physical else null;
    if (physical >= direct_limit or bytes > direct_limit - physical) return null;
    const image_pa = active_image.physical_base.toInt();
    if (physical >= image_pa and physical - image_pa < active_image.size_bytes) {
        if (physical >= returned_image.start and bytes <= returned_image.end -| physical)
            return direct_map.base + physical;
        const offset = physical - image_pa;
        if (bytes > active_image.size_bytes - offset or
            (physical < returned_image.end and returned_image.start < physical + bytes)) return null;
        return active_image.virtual_base.toInt() + offset;
    }
    // An owned range cannot straddle image RAM and ordinary RAM.
    if (physical < image_pa and bytes > image_pa - physical) return null;
    return direct_map.base + physical;
}

/// Only the architecture boundary may turn a physical extent into a kernel
/// pointer. This checks the active address window and alignment, NOT RAM kind,
/// cache attributes, ownership, permissions, or lifetime. The caller must own
/// accessible RAM (or a validated page-table borrow) throughout the use.
pub fn identitySlice(comptime T: type, base: PhysicalAddress, count: usize) ?[]T {
    if (@sizeOf(T) == 0) @compileError("physical extents require a sized element");
    const bytes = std.math.mul(usize, @sizeOf(T), count) catch return null;
    if (count == 0 or base.toInt() == 0 or base.toInt() % @alignOf(T) != 0) return null;
    const physical = base.toInt();
    const limit = if (@import("builtin").is_test and !direct_active) (@as(u64, 1) << 52) else direct_limit;
    if (physical >= limit or bytes > limit - physical) return null;
    const virtual = if (@import("builtin").is_test and !direct_active) physical else direct_map.base + physical;
    const pointer: [*]T = @ptrFromInt(@as(usize, @intCast(virtual)));
    return pointer[0..count];
}

/// Static image tables have only their linked high alias. Returned scratch
/// tables instead belong to ordinary direct-mapped RAM; ownership lives with
/// the table owner and is never inferred merely from this address conversion.
pub inline fn tablePointer(base: PhysicalAddress) ?*[512]u64 {
    if (base.toInt() == 0 or base.toInt() & 4095 != 0) return null;
    const virtual = tableVirtual(base.toInt(), 4096) orelse return null;
    return @ptrFromInt(virtual);
}

/// Limine's physical-zero HHDM base must be valid for the active paging mode,
/// and its mapping must STILL be installed (boot, before our CR3 switch).
/// Checks arithmetic/alignment only, not mapping presence, RAM kind, cache
/// attributes, ownership or lifetime; those remain the caller's obligations.
pub fn hhdmSlice(comptime T: type, base: KernelVirtualAddress, physical: PhysicalAddress, count: usize) ?[]T {
    if (@sizeOf(T) == 0) @compileError("physical extents require a sized element");
    const bytes = std.math.mul(usize, @sizeOf(T), count) catch return null;
    if (bytes == 0 or base.toInt() == 0 or physical.toInt() == 0) return null;
    _ = physical.offset(bytes - 1) orelse return null;
    const first = base.offset(physical.toInt()) orelse return null;
    _ = first.offset(bytes - 1) orelse return null;
    if (first.toInt() % @alignOf(T) != 0) return null;
    const pointer: [*]T = @ptrFromInt(@as(usize, @intCast(first.toInt())));
    return pointer[0..count];
}

test "HHDM boundary checks complete physical and virtual extents without claiming mappings" {
    const base = KernelVirtualAddress.fromInt(0xffff_8000_0000_0000);
    try std.testing.expectEqual(@as(u64, 0xffff_8000_0000_1000), @intFromPtr(hhdmSlice(u8, base, .fromInt(4096), 4096).?.ptr));
    try std.testing.expect(hhdmSlice(u8, .fromInt(0), .fromInt(4096), 4096) == null);
    try std.testing.expect(hhdmSlice(u8, base, .fromInt(0), 4096) == null);
    try std.testing.expect(hhdmSlice(u8, base, .fromInt(4096), 0) == null);
    try std.testing.expect(hhdmSlice(u64, base, .fromInt(4097), 1) == null);
    try std.testing.expect(hhdmSlice(u64, base, .fromInt(4096), std.math.maxInt(usize)) == null);
    try std.testing.expect(hhdmSlice(u8, .fromInt(1), .fromInt(std.math.maxInt(u64)), 2) == null);
    try std.testing.expect(hhdmSlice(u8, .fromInt(std.math.maxInt(u64)), .fromInt(4096), 1) == null);
    try std.testing.expect(hhdmSlice(u8, .fromInt(std.math.maxInt(u64) - 4096), .fromInt(4096), 2) == null);
}

pub const KernelImage = struct {
    virtual_base: KernelVirtualAddress,
    physical_base: PhysicalAddress,
    size_bytes: u64,
};

pub fn kernelAddressToPhysical(addr: KernelVirtualAddress, image: KernelImage) ?PhysicalAddress {
    const value = addr.toInt();
    const virtual_base = image.virtual_base.toInt();
    if (virtual_base >= 0xffff_8000_0000_0000 and image.physical_base.toInt() != 0 and value >= virtual_base) {
        const offset = value - virtual_base;
        if (image.size_bytes == 0 or offset < image.size_bytes)
            return image.physical_base.offset(offset);
    }
    if (direct_active and value >= direct_map.base and value - direct_map.base < direct_limit)
        return .fromInt(value - direct_map.base);
    if (@import("builtin").is_test and !direct_active and value < (@as(u64, 1) << 52)) return .fromInt(value);
    return null;
}

pub fn kernelPointerPaddr(addr: u64, virtual_base: u64, physical_base: u64, image_size: u64) ?u64 {
    const physical = kernelAddressToPhysical(.fromInt(addr), .{
        .virtual_base = .fromInt(virtual_base),
        .physical_base = .fromInt(physical_base),
        .size_bytes = image_size,
    }) orelse return null;
    return physical.toInt();
}

test "identity pointer boundary checks the whole extent and alignment" {
    const base = PhysicalAddress.fromInt((@as(u64, 1) << 52) - 4096);
    try std.testing.expectEqual((@as(u64, 1) << 52) - 4096, @intFromPtr(identitySlice(u8, base, 4096).?.ptr));
    try std.testing.expect(identitySlice(u8, base, 4097) == null);
    try std.testing.expect(identitySlice(u8, .fromInt((@as(u64, 1) << 52)), 1) == null);
    try std.testing.expect(identitySlice(u64, .fromInt(4097), 1) == null);
    try std.testing.expect(identitySlice(u8, .fromInt(0), 1) == null);
    try std.testing.expect(identitySlice(u8, .fromInt(4096), 0) == null);
    try std.testing.expect(identitySlice(u64, .fromInt(4096), std.math.maxInt(usize)) == null);
    try std.testing.expect(identitySlice(u8, .fromInt(std.math.maxInt(u64)), 2) == null);
}

test "identity translation spans 4GiB but stops at the mapped limit" {
    for ([_]u64{ 0, 0xffff_ffff, 0x100000000, 0x1233b4000, (@as(u64, 1) << 52) - 1 }) |addr| {
        try std.testing.expectEqual(@as(?u64, addr), kernelPointerPaddr(addr, 0, 0, 0));
    }
    try std.testing.expectEqual(@as(?u64, null), kernelPointerPaddr((@as(u64, 1) << 52), 0, 0, 0));
    try std.testing.expectEqual(@as(?u64, 0x10000000000), kernelPointerPaddr(0x10000000000, 0, 0, 0));
}

test "linked kernel image translation remains distinct from identity mapping" {
    const base = 0xffff_ffff_8000_0000;
    try std.testing.expectEqual(@as(?u64, 0x201000), kernelPointerPaddr(base + 0x1000, base, 0x200000, 0x3000));
    try std.testing.expectEqual(@as(?u64, null), kernelPointerPaddr(base + 0x3000, base, 0x200000, 0x3000));
    try std.testing.expectEqual(@as(?u64, null), kernelPointerPaddr(base - 1, base, 0x200000, 0x3000));
    try std.testing.expectEqual(@as(?u64, 0x1233b4000), kernelPointerPaddr(0x1233b4000, base, 0x200000, 0x3000));
    try std.testing.expectEqual(@as(?u64, null), kernelPointerPaddr(base + 0x1000, base, std.math.maxInt(u64), 0x3000));
}

test "active RAM conversion uses direct addresses above 16 GiB and image table exception" {
    defer {
        direct_active = false;
        returned_image = .{ .start = 0, .end = 0 };
    }
    const image: KernelImage = .{ .virtual_base = .fromInt(0xffff_ffff_8000_0000), .physical_base = .fromInt(0x200000), .size_bytes = 0x10000 };
    activateDirect(direct_map.aperture_bytes, image);
    try std.testing.expectEqual(direct_map.base + (20 << 30), @intFromPtr(identitySlice(u8, .fromInt(20 << 30), 4096).?.ptr));
    try std.testing.expectEqual(image.virtual_base.toInt(), @intFromPtr(tablePointer(image.physical_base).?));
    try std.testing.expectEqual(@as(u64, 20) << 30, kernelAddressToPhysical(.fromInt(direct_map.base + (20 << 30)), image).?.toInt());
    try std.testing.expect(kernelAddressToPhysical(.fromInt(4096), image) == null);
    returnImageExtent(0x20f000, 0x210000);
    try std.testing.expectEqual(direct_map.base + 0x20f000, @intFromPtr(identitySlice(u8, .fromInt(0x20f000), 4096).?.ptr));
    try std.testing.expect(tablePointer(.fromInt(0x20f000)) != null);
    try std.testing.expect(identitySlice(u8, .fromInt(direct_map.aperture_bytes - 4096), 4097) == null);
}
