const std = @import("std");
const init_bootstrap_abi = @import("kernel_abi_root").init_bootstrap_abi;

pub const Location = struct {
    bus: u8,
    device: u8,
    function: u8,
};

pub const pci_resource_id_prefix: u64 = 0x50434900_00000000;
pub const pci_resource_id_mask: u64 = 0xffffffff_00000000;

pub const interrupt_vector_base: u8 = 0x50;
pub const interrupt_vectors_per_device: u8 = 16;
pub const interrupt_device_count: usize = init_bootstrap_abi.max_device_descriptors;
pub const interrupt_vector_count: u8 =
    @intCast(interrupt_device_count * @as(usize, interrupt_vectors_per_device));

pub const InterruptRoute = struct {
    device: u64,
    entry: u8,
};

var interrupt_route_devices: [interrupt_device_count]u64 =
    [_]u64{0} ** interrupt_device_count;

pub const bar_flag_io: u64 = 1 << 0;
pub const bar_flag_mem: u64 = 1 << 1;
pub const bar_flag_prefetchable: u64 = 1 << 2;
pub const bar_flag_64bit: u64 = 1 << 3;

pub const BarInfo = struct {
    start: u64,
    size: u64,
    flags: u64,

    pub fn end(self: BarInfo) ?u64 {
        if (self.size == 0) return null;
        const value, const overflow = @addWithOverflow(self.start, self.size - 1);
        if (overflow != 0) return null;
        return value;
    }
};

const config_address_port: u16 = 0xCF8;
const config_data_port: u16 = 0xCFC;

fn outl(port: u16, value: u32) void {
    asm volatile ("outl %[value], %[port]"
        :
        : [value] "{eax}" (value),
          [port] "{dx}" (port),
    );
}

fn inl(port: u16) u32 {
    var value: u32 = 0;
    asm volatile ("inl %[port], %[value]"
        : [value] "={eax}" (value),
        : [port] "{dx}" (port),
    );
    return value;
}

fn configAddress(loc: Location, offset: u8) u32 {
    return 0x8000_0000 |
        (@as(u32, loc.bus) << 16) |
        (@as(u32, loc.device) << 11) |
        (@as(u32, loc.function) << 8) |
        (@as(u32, offset) & 0xFC);
}

pub fn readConfigU32(loc: Location, offset: u8) u32 {
    outl(config_address_port, configAddress(loc, offset));
    return inl(config_data_port);
}

pub fn writeConfigU32(loc: Location, offset: u8, value: u32) void {
    outl(config_address_port, configAddress(loc, offset));
    outl(config_data_port, value);
}

pub fn readConfigU16(loc: Location, offset: u8) u16 {
    const value = readConfigU32(loc, offset);
    const shift: u5 = @intCast((offset & 0x2) * 8);
    return @intCast((value >> shift) & 0xFFFF);
}

pub fn writeConfigU16(loc: Location, offset: u8, value: u16) void {
    const aligned = offset & 0xFC;
    const shift: u5 = @intCast((offset & 0x2) * 8);
    const mask: u32 = ~(@as(u32, 0xFFFF) << shift);
    const cur = readConfigU32(loc, aligned);
    const next = (cur & mask) | (@as(u32, value) << shift);
    writeConfigU32(loc, aligned, next);
}

pub fn readConfigU8(loc: Location, offset: u8) u8 {
    const value = readConfigU32(loc, offset);
    const shift: u5 = @intCast((offset & 0x3) * 8);
    return @intCast((value >> shift) & 0xFF);
}

pub fn writeConfigU8(loc: Location, offset: u8, value: u8) void {
    const aligned = offset & 0xFC;
    const shift: u5 = @intCast((offset & 0x3) * 8);
    const mask: u32 = ~(@as(u32, 0xFF) << shift);
    const cur = readConfigU32(loc, aligned);
    const next = (cur & mask) | (@as(u32, value) << shift);
    writeConfigU32(loc, aligned, next);
}

pub fn readVendorId(loc: Location) u16 {
    return readConfigU16(loc, 0x00);
}

pub fn readDeviceId(loc: Location) u16 {
    return readConfigU16(loc, 0x02);
}

pub fn readSubsystemId(loc: Location) u16 {
    return readConfigU16(loc, 0x2E);
}

pub fn readRevisionId(loc: Location) u8 {
    return readConfigU8(loc, 0x08);
}

pub fn readProgIf(loc: Location) u8 {
    return readConfigU8(loc, 0x09);
}

pub fn readSubclass(loc: Location) u8 {
    return readConfigU8(loc, 0x0A);
}

pub fn readClassCode(loc: Location) u8 {
    return readConfigU8(loc, 0x0B);
}

pub fn readHeaderType(loc: Location) u8 {
    return readConfigU8(loc, 0x0E);
}

pub fn resourceIdFromLocation(loc: Location) u64 {
    return pci_resource_id_prefix |
        (@as(u64, loc.bus) << 16) |
        (@as(u64, loc.device) << 8) |
        @as(u64, loc.function);
}

pub fn locationFromResourceId(resource_id: u64) ?Location {
    if ((resource_id & pci_resource_id_mask) != pci_resource_id_prefix) return null;
    return .{
        .bus = @intCast((resource_id >> 16) & 0xff),
        .device = @intCast((resource_id >> 8) & 0xff),
        .function = @intCast(resource_id & 0xff),
    };
}

pub fn clearInterruptRoutes() void {
    @memset(interrupt_route_devices[0..], 0);
}

pub fn registerInterruptRoute(resource_id: u64, device_index: usize) bool {
    if (locationFromResourceId(resource_id) == null or
        device_index >= interrupt_route_devices.len)
    {
        return false;
    }
    for (interrupt_route_devices, 0..) |device, index| {
        if (index != device_index and device == resource_id) return false;
    }
    const existing = interrupt_route_devices[device_index];
    if (existing != 0 and existing != resource_id) return false;
    interrupt_route_devices[device_index] = resource_id;
    return true;
}

pub fn interruptVectorBaseForResourceId(resource_id: u64) ?u8 {
    for (interrupt_route_devices, 0..) |device, index| {
        if (device != resource_id) continue;
        return interrupt_vector_base +
            @as(u8, @intCast(index)) * interrupt_vectors_per_device;
    }
    return null;
}

pub fn interruptRouteForVector(vector: u8) ?InterruptRoute {
    if (vector < interrupt_vector_base) return null;
    const offset: u16 = @as(u16, vector) - interrupt_vector_base;
    if (offset >= interrupt_vector_count) return null;
    const device_index: usize = offset / interrupt_vectors_per_device;
    const device = interrupt_route_devices[device_index];
    if (device == 0) return null;
    return .{
        .device = device,
        .entry = @intCast(offset % interrupt_vectors_per_device),
    };
}

fn barSizeFromMask32(mask: u32) ?u64 {
    if (mask == 0) return null;
    const size = (~mask) +% 1;
    if (size == 0) return null;
    return @as(u64, size);
}

fn barSizeFromMask64(mask: u64) ?u64 {
    if (mask == 0) return null;
    const size = (~mask) +% 1;
    if (size == 0) return null;
    return size;
}

pub fn probeBarInfo(loc: Location, bar_index: u8) ?BarInfo {
    if (bar_index >= 6) return null;
    if (readVendorId(loc) == 0xFFFF) return null;

    if (bar_index > 0) {
        const previous_off: u8 = @intCast(0x10 + (bar_index - 1) * 4);
        const previous = readConfigU32(loc, previous_off);
        const previous_is_64bit_mem = (previous & 0x1) == 0 and ((previous >> 1) & 0x3) == 0x2;
        if (previous_is_64bit_mem) return null;
    }

    const bar_off: u8 = @intCast(0x10 + bar_index * 4);
    const original = readConfigU32(loc, bar_off);
    if (original == 0 or original == 0xFFFF_FFFF) return null;

    const command = readConfigU16(loc, 0x04);
    writeConfigU16(loc, 0x04, command & ~@as(u16, 0x3));
    defer writeConfigU16(loc, 0x04, command);

    if ((original & 0x1) != 0) {
        writeConfigU32(loc, bar_off, 0xFFFF_FFFC);
        const mask = readConfigU32(loc, bar_off) & 0xFFFF_FFFC;
        writeConfigU32(loc, bar_off, original);
        const size = barSizeFromMask32(mask) orelse return null;
        const start = @as(u64, original & 0xFFFF_FFFC);
        const info: BarInfo = .{ .start = start, .size = size, .flags = bar_flag_io };
        _ = info.end() orelse return null;
        return info;
    }

    const mem_type = (original >> 1) & 0x3;
    const prefetchable = (original & 0x8) != 0;
    if (mem_type == 0x2) {
        if (bar_index >= 5) return null;
        const original_high = readConfigU32(loc, bar_off + 4);
        writeConfigU32(loc, bar_off, 0xFFFF_FFFF);
        writeConfigU32(loc, bar_off + 4, 0xFFFF_FFFF);
        const mask_low = readConfigU32(loc, bar_off) & 0xFFFF_FFF0;
        const mask_high = readConfigU32(loc, bar_off + 4);
        writeConfigU32(loc, bar_off + 4, original_high);
        writeConfigU32(loc, bar_off, original);
        const mask = (@as(u64, mask_high) << 32) | @as(u64, mask_low);
        const size = barSizeFromMask64(mask) orelse return null;
        const start = (@as(u64, original_high) << 32) | @as(u64, original & 0xFFFF_FFF0);
        var flags = bar_flag_mem | bar_flag_64bit;
        if (prefetchable) flags |= bar_flag_prefetchable;
        const info: BarInfo = .{ .start = start, .size = size, .flags = flags };
        _ = info.end() orelse return null;
        return info;
    }

    writeConfigU32(loc, bar_off, 0xFFFF_FFFF);
    const mask = readConfigU32(loc, bar_off) & 0xFFFF_FFF0;
    writeConfigU32(loc, bar_off, original);
    const size = barSizeFromMask32(mask) orelse return null;
    const start = @as(u64, original & 0xFFFF_FFF0);
    var flags = bar_flag_mem;
    if (prefetchable) flags |= bar_flag_prefetchable;
    const info: BarInfo = .{ .start = start, .size = size, .flags = flags };
    _ = info.end() orelse return null;
    return info;
}

test "config address encoding" {
    const loc = Location{
        .bus = 0x12,
        .device = 0x03,
        .function = 0x04,
    };
    try std.testing.expectEqual(@as(u32, 0x8012_1C20), configAddress(loc, 0x20));
}

test "PCI resource id encodes location" {
    const loc = Location{ .bus = 1, .device = 2, .function = 3 };
    const decoded = locationFromResourceId(resourceIdFromLocation(loc)).?;
    try std.testing.expectEqual(loc.bus, decoded.bus);
    try std.testing.expectEqual(loc.device, decoded.device);
    try std.testing.expectEqual(loc.function, decoded.function);
    try std.testing.expectEqual(@as(?Location, null), locationFromResourceId(0x1001));
}

test "routed interrupt vectors preserve device and MSI-X entry" {
    clearInterruptRoutes();
    defer clearInterruptRoutes();

    const first = resourceIdFromLocation(.{ .bus = 0, .device = 3, .function = 0 });
    const second = resourceIdFromLocation(.{ .bus = 0, .device = 4, .function = 0 });
    try std.testing.expect(registerInterruptRoute(first, 0));
    try std.testing.expect(registerInterruptRoute(second, 1));
    try std.testing.expectEqual(@as(?u8, 0x50), interruptVectorBaseForResourceId(first));
    try std.testing.expectEqual(@as(?u8, 0x60), interruptVectorBaseForResourceId(second));
    try std.testing.expectEqual(
        InterruptRoute{ .device = second, .entry = 7 },
        interruptRouteForVector(0x67).?,
    );
    try std.testing.expectEqual(@as(?InterruptRoute, null), interruptRouteForVector(0x41));
    try std.testing.expectEqual(@as(?InterruptRoute, null), interruptRouteForVector(0xD0));
}
