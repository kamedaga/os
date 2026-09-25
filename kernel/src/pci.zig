const std = @import("std");
const acpi_tables = @import("acpi_tables.zig");
const user_copy = @import("user_copy.zig");
const boot_scratch = @import("boot/boot_scratch.zig");
const identity_limit = @import("arch/x86_64/physical_layout.zig").identity_limit;

pub const Location = struct {
    bus: u8,
    device: u8,
    function: u8,
};

pub const pci_resource_id_prefix: u64 = 0x50434900_00000000;
pub const pci_resource_id_mask: u64 = 0xffffffff_00000000;

pub const interrupt_vector_base: u8 = 0x50;
pub const interrupt_vectors_per_device: u8 = 16;
// 0xf0..0xff stays outside device routing; each routed device needs a
// contiguous MSI block. This finite APIC budget is independent of PCI count.
pub const interrupt_vector_end_exclusive: u8 = 0xf0;
pub const interrupt_device_count: usize =
    (interrupt_vector_end_exclusive - interrupt_vector_base) / interrupt_vectors_per_device;
pub const interrupt_vector_count: u8 =
    @intCast(interrupt_device_count * @as(usize, interrupt_vectors_per_device));

pub const InterruptRoute = struct {
    device: u64,
    entry: u8,
};

var interrupt_route_devices: [interrupt_device_count]u64 =
    [_]u64{0} ** interrupt_device_count;
var interrupt_route_leases: [interrupt_device_count]u16 =
    [_]u16{0} ** interrupt_device_count;
var interrupt_route_retired: [interrupt_device_count][4]u16 =
    [_][4]u16{.{ 0, 0, 0, 0 }} ** interrupt_device_count;

pub fn kernelStaticStorageEndAddr() usize {
    var end = @intFromPtr(&interrupt_route_devices) + @sizeOf(@TypeOf(interrupt_route_devices));
    end = @max(end, @intFromPtr(&interrupt_route_leases) + @sizeOf(@TypeOf(interrupt_route_leases)));
    end = @max(end, @intFromPtr(&interrupt_route_retired) + @sizeOf(@TypeOf(interrupt_route_retired)));
    end = @max(end, @intFromPtr(&device_apertures) + @sizeOf(@TypeOf(device_apertures)));
    end = @max(end, @intFromPtr(&ecam_windows) + @sizeOf(@TypeOf(ecam_windows)));
    return @max(end, @intFromPtr(&config_lock) + @sizeOf(@TypeOf(config_lock)));
}

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

/// A BAR capability includes its containing pages. Subpage BAR users must
/// retain the original in-page register offset; no neighboring page is granted.
pub fn barMappingPaddr(info: BarInfo, page_offset: u64, map_size: u64) ?u64 {
    if ((info.flags & bar_flag_mem) == 0 or info.size == 0 or map_size == 0 or
        ((page_offset | map_size) & 4095) != 0) return null;
    _ = info.end() orelse return null;
    const first_page = info.start & ~@as(u64, 4095);
    if (first_page == 0) return null;
    const span = std.math.add(u64, info.start - first_page, info.size) catch return null;
    const rounded = std.math.add(u64, span, 4095) catch return null;
    const aperture = rounded & ~@as(u64, 4095);
    if (page_offset >= aperture or map_size > aperture - page_offset) return null;
    const paddr = std.math.add(u64, first_page, page_offset) catch return null;
    _ = std.math.add(u64, paddr, map_size - 1) catch return null;
    return paddr;
}

const BarAperture = struct {
    info: BarInfo,
    low: u32,
    high: u32,
};
const DeviceApertures = struct {
    resource_id: u64 = 0,
    vendor_device: u32 = 0,
    subsystem_id: u16 = 0,
    class_code: u32 = 0,
    claimed: bool = false,
    bars: [6]?BarAperture = [_]?BarAperture{null} ** 6,
};
var device_apertures: []DeviceApertures = &.{};

pub const BootFunction = struct {
    resource_id: u64,
    vendor_id: u16,
    device_id: u16,
    subsystem_id: u16,
    class_code: u32,
    bus: u8,
    device: u8,
    function: u8,
};

pub fn bootFunctionCount() usize {
    return device_apertures.len;
}

pub fn bootFunctionClaimed(index: usize) bool {
    return index >= device_apertures.len or device_apertures[index].claimed;
}

pub fn markBootFunctionClaimed(index: usize) void {
    std.debug.assert(index < device_apertures.len and !device_apertures[index].claimed);
    device_apertures[index].claimed = true;
}

pub fn bootFunctionAt(index: usize) ?BootFunction {
    if (index >= device_apertures.len) return null;
    const entry = device_apertures[index];
    const loc = locationFromResourceId(entry.resource_id) orelse return null;
    return .{
        .resource_id = entry.resource_id,
        .vendor_id = @truncate(entry.vendor_device),
        .device_id = @truncate(entry.vendor_device >> 16),
        .subsystem_id = entry.subsystem_id,
        .class_code = entry.class_code,
        .bus = loc.bus,
        .device = loc.device,
        .function = loc.function,
    };
}

/// Capture firmware-assigned BARs before user drivers can enable bus mastering.
/// The table is sized by discovery, rather than truncating at the IRQ budget.
pub fn captureBootFunctions() bool {
    var count: usize = 0;
    for (0..256) |bus| for (0..32) |device| {
        const first = Location{ .bus = @intCast(bus), .device = @intCast(device), .function = 0 };
        if (readVendorId(first) == 0xffff) continue;
        const functions: usize = if ((readHeaderType(first) & 0x80) != 0) 8 else 1;
        for (0..functions) |function| {
            const loc = Location{ .bus = @intCast(bus), .device = @intCast(device), .function = @intCast(function) };
            if (readVendorId(loc) != 0xffff and readClassCode(loc) != 0x06) count += 1;
        }
    };
    const size = std.math.mul(usize, count, @sizeOf(DeviceApertures)) catch return false;
    const storage = boot_scratch.allocate(size) orelse return false;
    const entries: [*]DeviceApertures = @ptrCast(@alignCast(storage.ptr));
    device_apertures = entries[0..count];
    @memset(device_apertures, .{});
    var index: usize = 0;
    for (0..256) |bus| for (0..32) |device| {
        const first = Location{ .bus = @intCast(bus), .device = @intCast(device), .function = 0 };
        if (readVendorId(first) == 0xffff) continue;
        const functions: usize = if ((readHeaderType(first) & 0x80) != 0) 8 else 1;
        for (0..functions) |function| {
            const loc = Location{ .bus = @intCast(bus), .device = @intCast(device), .function = @intCast(function) };
            if (readVendorId(loc) == 0xffff or readClassCode(loc) == 0x06) continue;
            if (index >= count or !captureBarApertures(resourceIdFromLocation(loc), index)) return false;
            index += 1;
        }
    };
    return index == count;
}

test "PCI catalog is not bounded by interrupt-route slots" {
    var entries: [9]DeviceApertures = [_]DeviceApertures{.{}} ** 9;
    const previous = device_apertures;
    device_apertures = entries[0..];
    defer device_apertures = previous;
    clearInterruptRoutes();
    defer clearInterruptRoutes();
    for (&entries, 0..) |*entry, index| {
        entry.resource_id = resourceIdFromLocation(.{
            .bus = 0, .device = @intCast(index), .function = 0,
        });
        entry.vendor_device = 0x0010_1b36;
    }
    try std.testing.expectEqual(@as(usize, 9), bootFunctionCount());
    try std.testing.expectEqual(entries[8].resource_id, bootFunctionAt(8).?.resource_id);
    try std.testing.expect(!bootFunctionClaimed(8));
    markBootFunctionClaimed(8);
    try std.testing.expect(bootFunctionClaimed(8));
    try std.testing.expect(ensureInterruptRoute(entries[8].resource_id));
    try std.testing.expect(interruptVectorBaseForResourceId(entries[8].resource_id) != null);
}

/// Boot-only read-only bound for a continuous DMA window. Do not size-probe
/// devices here: firmware may have left bus mastering enabled. Conservatively
/// exclude everything from each assigned memory BAR base upward. A BAR below
/// start makes the window empty (its extent is not proven disjoint).
/// Like firmware-assigned BAR publication, this assumes unassigned zero-base
/// BARs do not decode memory; it is not an arbitrary-firmware resource solver.
pub fn bootDmaWindowEnd(start: u64, ceiling: u64) u64 {
    const Reader = struct {
        fn read(_: @This(), loc: Location, offset: u8) u32 {
            return readConfigU32(loc, offset);
        }
    };
    return bootDmaWindowEndWithReader(start, ceiling, ecam_windows.entries[0..ecam_windows.count], Reader{});
}

fn bootDmaWindowEndWithReader(start: u64, ceiling: u64, windows: []const EcamWindow, reader: anytype) u64 {
    const exclude = @import("iova.zig").exclude;
    var end = ceiling;
    for (windows) |window| {
        const base = window.base + (@as(u64, window.bus_start) << 20);
        const last = window.base + ((@as(u64, window.bus_end) + 1) << 20) - 1;
        end = exclude(start, end, base, last);
    }
    for (0..256) |bus| {
        for (0..32) |device| {
            var loc = Location{ .bus = @intCast(bus), .device = @intCast(device), .function = 0 };
            if (@as(u16, @truncate(reader.read(loc, 0))) == 0xffff) continue;
            const functions: usize = if (@as(u8, @truncate(reader.read(loc, 0x0c) >> 16)) & 0x80 != 0) 8 else 1;
            for (0..functions) |function| {
                loc.function = @intCast(function);
                if (@as(u16, @truncate(reader.read(loc, 0))) == 0xffff) continue;
                const header = @as(u8, @truncate(reader.read(loc, 0x0c) >> 16)) & 0x7f;
                const bar_count: u8 = switch (header) {
                    0 => 6,
                    1 => 2,
                    else => return start, // CardBus/unknown windows unsupported.
                };
                var index: u8 = 0;
                while (index < bar_count) : (index += 1) {
                    const low = reader.read(loc, 0x10 + index * 4);
                    if (low == 0 or low & 1 != 0) continue;
                    const kind = (low >> 1) & 3;
                    var base: u64 = low & 0xfffffff0;
                    switch (kind) {
                        0 => {},
                        2 => {
                            if (index + 1 >= bar_count) return start;
                            index += 1;
                            base |= @as(u64, reader.read(loc, 0x10 + index * 4)) << 32;
                        },
                        else => return start,
                    }
                    if (base != 0) end = exclude(start, end, base, std.math.maxInt(u64));
                }
                const rom = reader.read(loc, if (header == 0) 0x30 else 0x38);
                const rom_base: u64 = rom & 0xfffff800;
                if (rom_base != 0) end = exclude(start, end, rom_base, std.math.maxInt(u64));
                if (header == 1) {
                    end = dmaBridgeWindowEnd(start, end, reader.read(loc, 0x20), 0, 0, false);
                    end = dmaBridgeWindowEnd(start, end, reader.read(loc, 0x24), reader.read(loc, 0x28), reader.read(loc, 0x2c), true);
                }
            }
        }
    }
    return end;
}

fn dmaBridgeWindowEnd(start: u64, end: u64, low: u32, base_high: u32, last_high: u32, prefetch: bool) u64 {
    const kind = low & 15;
    if (kind != ((low >> 16) & 15) or kind > @intFromBool(prefetch)) return start;
    var base = @as(u64, low & 0xfff0) << 16;
    var last = @as(u64, low & 0xfff00000) | 0xfffff;
    if (prefetch and kind == 1) {
        base |= @as(u64, base_high) << 32;
        last |= @as(u64, last_high) << 32;
    }
    // Base > limit is the architected disabled window encoding.
    return @import("iova.zig").exclude(start, end, base, last);
}

/// Called once while boot owns the device, before exposing any device FD.
/// The capability grants these firmware-assigned apertures, not later BAR
/// addresses a client writes. There is no allocator or resource policy here.
pub fn captureBarApertures(resource_id: u64, device_index: usize) bool {
    const loc = locationFromResourceId(resource_id) orelse return false;
    if (device_index >= device_apertures.len or device_apertures[device_index].resource_id != 0) return false;
    var captured = DeviceApertures{
        .resource_id = resource_id,
        .vendor_device = readConfigU32(loc, 0),
        .subsystem_id = readSubsystemId(loc),
        .class_code = readConfigU32(loc, 0x08) >> 8,
    };
    if (@as(u16, @truncate(captured.vendor_device)) == 0xffff) return false;
    for (&captured.bars, 0..) |*entry, index| {
        const bar_index: u8 = @intCast(index);
        const info = probeBarInfo(loc, bar_index) orelse continue;
        entry.* = .{
            .info = info,
            .low = readConfigU32(loc, 0x10 + bar_index * 4),
            .high = if ((info.flags & bar_flag_64bit) != 0) readConfigU32(loc, 0x14 + bar_index * 4) else 0,
        };
    }
    device_apertures[device_index] = captured;
    return true;
}

fn apertureMatches(aperture: BarAperture, low: u32, high: u32) bool {
    return aperture.low == low and ((aperture.info.flags & bar_flag_64bit) == 0 or aperture.high == high);
}

/// Live queries do not size-probe or temporarily disable a running device.
/// Relocation or replacement needs a new authorized device publication.
pub fn authorizedBarInfo(resource_id: u64, bar_index: u8) ?BarInfo {
    if (bar_index >= 6) return null;
    const loc = locationFromResourceId(resource_id) orelse return null;
    for (device_apertures) |*device| {
        if (device.resource_id != resource_id) continue;
        const aperture = device.bars[bar_index] orelse return null;
        if (readConfigU32(loc, 0) != device.vendor_device) return null;
        const low = readConfigU32(loc, 0x10 + bar_index * 4);
        const high = if ((aperture.info.flags & bar_flag_64bit) != 0) readConfigU32(loc, 0x14 + bar_index * 4) else 0;
        if (!apertureMatches(aperture, low, high)) return null;
        return aperture.info;
    }
    return null;
}

const config_address_port: u16 = 0xCF8;
const config_data_port: u16 = 0xCFC;

const max_ecam_windows = 32;
pub const EcamWindow = struct {
    base: u64,
    segment: u16,
    bus_start: u8,
    bus_end: u8,

    pub fn address(self: EcamWindow, loc: Location, offset: u16) ?u64 {
        if (self.segment != 0 or loc.bus < self.bus_start or loc.bus > self.bus_end or
            loc.device >= 32 or loc.function >= 8 or offset >= 4096) return null;
        // MCFG bases correspond to bus zero even for nonzero starting buses.
        return self.base + (@as(u64, loc.bus) << 20) +
            (@as(u64, loc.device) << 15) + (@as(u64, loc.function) << 12) + offset;
    }
};
pub const EcamWindows = struct {
    entries: [max_ecam_windows]EcamWindow = undefined,
    count: usize = 0,
};
var ecam_windows: EcamWindows = .{};
var config_lock: u8 = 0;

pub fn parseMcfg(bytes: []const u8) ?EcamWindows {
    if (bytes.len < 44 or !std.mem.eql(u8, bytes[0..4], "MCFG")) return null;
    const length = std.mem.readInt(u32, bytes[4..8], .little);
    if (length < 44 or length > bytes.len or (length - 44) % 16 != 0) return null;
    var sum: u8 = 0;
    for (bytes[0..length]) |byte| sum +%= byte;
    if (sum != 0 or !std.mem.allEqual(u8, bytes[36..44], 0)) return null;
    var result = EcamWindows{};
    var offset: usize = 44;
    while (offset < length) : (offset += 16) {
        if (result.count == result.entries.len) return null;
        const entry = bytes[offset..][0..16];
        const window = EcamWindow{
            .base = std.mem.readInt(u64, entry[0..8], .little),
            .segment = std.mem.readInt(u16, entry[8..10], .little),
            .bus_start = entry[10],
            .bus_end = entry[11],
        };
        if (window.base == 0 or (window.base & 0xfffff) != 0 or
            window.bus_start > window.bus_end or !std.mem.allEqual(u8, entry[12..16], 0)) return null;
        const span = (@as(u64, window.bus_end) + 1) << 20;
        const end, const overflow = @addWithOverflow(window.base, span);
        if (overflow != 0 or end > 0x0010_0000_0000_0000) return null;
        for (result.entries[0..result.count]) |prior| {
            if (prior.segment == window.segment and prior.bus_start <= window.bus_end and
                window.bus_start <= prior.bus_end) return null;
            const start = window.base + (@as(u64, window.bus_start) << 20);
            const prior_start = prior.base + (@as(u64, prior.bus_start) << 20);
            const prior_end = prior.base + ((@as(u64, prior.bus_end) + 1) << 20);
            if (start < prior_end and prior_start < end) return null;
        }
        result.entries[result.count] = window;
        result.count += 1;
    }
    return result;
}

pub fn initEcam(rsdp_paddr: u64) void {
    const log = @import("kernel_log.zig");
    ecam_windows.count = 0;
    const bytes = acpi_tables.find(rsdp_paddr, "MCFG", 44) orelse {
        log.write("pci: ECAM unavailable reason=no-valid-MCFG\n");
        return;
    };
    const parsed = parseMcfg(bytes) orelse {
        log.write("pci: ECAM unavailable reason=invalid-MCFG-window\n");
        return;
    };
    for (parsed.entries[0..parsed.count]) |window| {
        if (window.segment != 0) continue;
        const address = window.base + (@as(u64, window.bus_start) << 20);
        const size = (@as(u64, window.bus_end) - window.bus_start + 1) << 20;
        const mapped = @import("arch/x86_64/platform.zig").mapBootMmioIdentityRange(address, size);
        log.writeFmt("pci: ECAM base=0x{x} segment={} buses={}-{} mapped_uc={}\n", .{
            window.base, window.segment, window.bus_start, window.bus_end,
            mapped,
        });
        if (!mapped) continue;
        ecam_windows.entries[ecam_windows.count] = window;
        ecam_windows.count += 1;
    }
}

fn ecamAddress(loc: Location, offset: u16) ?u64 {
    for (ecam_windows.entries[0..ecam_windows.count]) |window| {
        const address = window.address(loc, offset) orelse continue;
        // Only windows mapped UC before SMP startup are published here.
        if (address >= identity_limit) return null;
        return address;
    }
    return null;
}

pub fn configAccessValid(loc: Location, offset: u32, width: u32) bool {
    if (loc.device >= 32 or loc.function >= 8 or (width != 1 and width != 2 and width != 4)) return false;
    if (offset >= 4096 or offset % width != 0 or width > 4096 - offset) return false;
    return offset < 256 or ecamAddress(loc, @intCast(offset)) != null;
}

fn lockConfig() u64 {
    const flags = asm volatile ("pushfq; popq %[flags]; cli"
        : [flags] "=r" (-> u64),
        :
        : .{ .memory = true });
    while (@cmpxchgWeak(u8, &config_lock, 0, 1, .acquire, .monotonic) != null)
        std.atomic.spinLoopHint();
    return flags;
}

fn unlockConfig(flags: u64) void {
    @atomicStore(u8, &config_lock, 0, .release);
    if ((flags & (1 << 9)) != 0) asm volatile ("sti" ::: .{ .memory = true });
}

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

fn outw(port: u16, value: u16) void {
    asm volatile ("outw %[value], %[port]"
        :
        : [value] "{ax}" (value),
          [port] "{dx}" (port),
    );
}

fn inw(port: u16) u16 {
    return asm volatile ("inw %[port], %[value]"
        : [value] "={ax}" (-> u16),
        : [port] "{dx}" (port),
    );
}

fn outb(port: u16, value: u8) void {
    asm volatile ("outb %[value], %[port]"
        :
        : [value] "{al}" (value),
          [port] "{dx}" (port),
    );
}

fn inb(port: u16) u8 {
    return asm volatile ("inb %[port], %[value]"
        : [value] "={al}" (-> u8),
        : [port] "{dx}" (port),
    );
}

fn configAddress(loc: Location, offset: u8) u32 {
    return 0x8000_0000 |
        (@as(u32, loc.bus) << 16) |
        (@as(u32, loc.device) << 11) |
        (@as(u32, loc.function) << 8) |
        (@as(u32, offset) & 0xFC);
}

const NativeConfigIo = struct {
    fn read(_: @This(), port: u16, width: u32) u32 {
        return switch (width) {
            1 => inb(port),
            2 => inw(port),
            4 => inl(port),
            else => unreachable,
        };
    }
    fn write(_: @This(), port: u16, width: u32, value: u32) void {
        switch (width) {
            1 => outb(port, @truncate(value)),
            2 => outw(port, @truncate(value)),
            4 => outl(port, value),
            else => unreachable,
        }
    }
};

fn readLegacyConfig(io: anytype, loc: Location, offset: u8, width: u32) u32 {
    io.write(config_address_port, 4, configAddress(loc, offset));
    return io.read(config_data_port + (offset & 3), width);
}

fn writeLegacyConfig(io: anytype, loc: Location, offset: u8, width: u32, value: u32) void {
    io.write(config_address_port, 4, configAddress(loc, offset));
    // Command and Status share a DWORD, but Status contains W1C bits.
    // There is exactly one data write of the requested width, with no RMW.
    io.write(config_data_port + (offset & 3), width, value);
}

pub fn readConfig(loc: Location, offset: u32, width: u32) ?u32 {
    if (!configAccessValid(loc, offset, width)) return null;
    const flags = lockConfig();
    defer unlockConfig(flags);
    if (offset < 256) return readLegacyConfig(NativeConfigIo{}, loc, @intCast(offset), width);
    const address = ecamAddress(loc, @intCast(offset)) orelse return null;
    return readEcam(address, width);
}

fn readEcam(address: u64, width: u32) u32 {
    return switch (width) {
        1 => @as(*volatile u8, @ptrFromInt(address)).*,
        2 => @as(*volatile u16, @ptrFromInt(address)).*,
        4 => @as(*volatile u32, @ptrFromInt(address)).*,
        else => unreachable,
    };
}

pub fn writeConfig(loc: Location, offset: u32, width: u32, value: u32) bool {
    if (!configAccessValid(loc, offset, width)) return false;
    const flags = lockConfig();
    defer unlockConfig(flags);
    if (offset < 256) {
        writeLegacyConfig(NativeConfigIo{}, loc, @intCast(offset), width, value);
    } else {
        const address = ecamAddress(loc, @intCast(offset)) orelse return false;
        writeEcam(address, width, value);
    }
    return true;
}

fn writeEcam(address: u64, width: u32, value: u32) void {
    switch (width) {
        1 => @as(*volatile u8, @ptrFromInt(address)).* = @truncate(value),
        2 => @as(*volatile u16, @ptrFromInt(address)).* = @truncate(value),
        4 => @as(*volatile u32, @ptrFromInt(address)).* = value,
        else => unreachable,
    }
}

pub fn readConfigU32(loc: Location, offset: u8) u32 {
    return readConfig(loc, offset, 4) orelse std.math.maxInt(u32);
}

pub fn writeConfigU32(loc: Location, offset: u8, value: u32) void {
    std.debug.assert(writeConfig(loc, offset, 4, value));
}

pub fn readConfigU16(loc: Location, offset: u8) u16 {
    return @truncate(readConfig(loc, offset, 2) orelse std.math.maxInt(u16));
}

pub fn writeConfigU16(loc: Location, offset: u8, value: u16) void {
    std.debug.assert(writeConfig(loc, offset, 2, value));
}

pub fn readConfigU8(loc: Location, offset: u8) u8 {
    return @truncate(readConfig(loc, offset, 1) orelse std.math.maxInt(u8));
}

pub fn writeConfigU8(loc: Location, offset: u8, value: u8) void {
    std.debug.assert(writeConfig(loc, offset, 1, value));
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
    if ((resource_id & 0xff00_e0f8) != 0) return null;
    return .{
        .bus = @intCast((resource_id >> 16) & 0xff),
        .device = @intCast((resource_id >> 8) & 0xff),
        .function = @intCast(resource_id & 0xff),
    };
}

pub fn clearInterruptRoutes() void {
    @memset(interrupt_route_devices[0..], 0);
    @memset(interrupt_route_leases[0..], 0);
    @memset(interrupt_route_retired[0..], .{ 0, 0, 0, 0 });
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

/// Assign a vector block only when a device actually derives an IRQ. PCI
/// discovery and Device FD creation must not consume this finite APIC budget.
pub fn ensureInterruptRoute(resource_id: u64) bool {
    if (locationFromResourceId(resource_id) == null) return false;
    if (interruptVectorBaseForResourceId(resource_id) != null) return true;
    for (interrupt_route_devices, 0..) |device, index| {
        if (device == 0) return registerInterruptRoute(resource_id, index);
    }
    return false;
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

const InterruptKind = @import("kernel_abi_root").capsule_abi.IrqKind;
pub const InterruptLeaseError = error{ Invalid, Unsupported, Busy, NotReady };

fn interruptEntryMask(kind: InterruptKind, entry: u32) ?u16 {
    return switch (kind) {
        .auto => if (entry == 0) std.math.maxInt(u16) else null,
        .msi, .msix => if (entry < interrupt_vectors_per_device)
            @as(u16, 1) << @as(u4, @intCast(entry))
        else
            null,
        .intx => null,
    };
}

const NativeInterruptIo = struct {
    fn config(_: @This(), loc: Location, offset: u8, width: u32) ?u32 {
        return readConfig(loc, offset, width);
    }
    fn setConfig(_: @This(), loc: Location, offset: u8, width: u32, value: u32) bool {
        return writeConfig(loc, offset, width, value);
    }
    fn authorizedBar(_: @This(), loc: Location, bar: u8) ?BarInfo {
        return authorizedBarInfo(resourceIdFromLocation(loc), bar);
    }
    fn readWord(_: @This(), address: u64) ?u32 {
        return user_copy.readUncachedMmioU32(address);
    }
    fn writeWord(_: @This(), address: u64, value: u32) bool {
        return user_copy.writeUncachedMmioU32(address, value);
    }
    fn drain(_: @This(), first: u8, count: u8) bool {
        return @import("smp.zig").drainInterruptRange(first, count);
    }
};

fn interruptBarAddress(io: anytype, loc: Location, descriptor: u32, span: u64) ?u64 {
    const bar: u8 = @intCast(descriptor & 7);
    if (bar >= 6 or span == 0) return null;
    // Use the immutable capability aperture, including its live BAR identity
    // check. An uncached mapping alone grants no authority to this device.
    const aperture = io.authorizedBar(loc, bar) orelse return null;
    if ((aperture.flags & bar_flag_mem) == 0) return null;
    _ = aperture.end() orelse return null;
    const offset: u64 = descriptor & 0xfffffff8;
    // These are byte-exact PCI structures, not a page-rounded mapping grant.
    if (offset >= aperture.size or span > aperture.size - offset) return null;
    const address = std.math.add(u64, aperture.start, offset) catch return null;
    _ = std.math.add(u64, address, span - 1) catch return null;
    return address;
}

/// Mask and read back the physical source. Pending MSI/MSI-X bits are device
/// owned and read-only: they are checked, never cleared by a software counter.
/// GPU reset/source quiescence remains with the upstream driver/device owner.
const InterruptSourceState = struct { pending: bool, was_unmasked: bool };

fn changeInterruptSourceMask(io: anytype, loc: Location, kind: InterruptKind, entry: u32, masked: bool) InterruptLeaseError!InterruptSourceState {
    if ((io.config(loc, 0x06, 2) orelse return error.Unsupported) & 0x10 == 0)
        return error.Unsupported;
    var cap: u8 = @truncate((io.config(loc, 0x34, 1) orelse return error.Unsupported) & 0xfc);
    var visited: u64 = 0;
    var found = false;
    var pending = false;
    var was_unmasked = false;
    while (cap != 0) {
        if (cap < 0x40 or cap > 0xf8) return error.Unsupported;
        const visit = @as(u64, 1) << @as(u6, @intCast(cap / 4));
        if ((visited & visit) != 0) return error.Unsupported;
        visited |= visit;
        const header = io.config(loc, cap, 4) orelse return error.Unsupported;
        const id: u8 = @truncate(header);
        const control: u16 = @truncate(header >> 16);
        if (id == 0x05 and (kind == .msi or kind == .auto)) {
            found = true;
            // Without per-vector masking/pending bits there is no generic
            // way to prove that an old source is safe to hand to a new owner.
            if ((control & 0x100) == 0) {
                _ = io.setConfig(loc, cap + 2, 2, control & ~@as(u16, 1));
                _ = io.config(loc, cap + 2, 2);
                return error.Unsupported;
            }
            const messages = @as(u32, 1) << @as(u5, @intCast((control >> 1) & 7));
            if (messages > 32 or (kind != .auto and entry >= messages)) return error.Invalid;
            const mask: u32 = if (kind == .auto)
                if (messages == 32) std.math.maxInt(u32) else (@as(u32, 1) << @as(u5, @intCast(messages))) - 1
            else
                @as(u32, 1) << @as(u5, @intCast(entry));
            const offset: u8 = if ((control & 0x80) != 0) 16 else 12;
            if (@as(u16, cap) + offset + 8 > 256) return error.Unsupported;
            const old_mask = io.config(loc, cap + offset, 4) orelse return error.Unsupported;
            was_unmasked = was_unmasked or (old_mask & mask) == 0;
            const next_mask = if (masked) old_mask | mask else old_mask & ~mask;
            if (!io.setConfig(loc, cap + offset, 4, next_mask)) return error.Unsupported;
            if (((io.config(loc, cap + offset, 4) orelse return error.Unsupported) & mask) != (next_mask & mask))
                return error.NotReady;
            pending = pending or ((io.config(loc, cap + offset + 4, 4) orelse return error.Unsupported) & mask) != 0;
        } else if (id == 0x11 and (kind == .msix or kind == .auto)) {
            found = true;
            if (cap > 0xf4 or ((io.config(loc, 0x04, 2) orelse return error.Unsupported) & 2) == 0)
                return error.Unsupported;
            const entries: u32 = (control & 0x7ff) + 1;
            if (kind != .auto and entry >= entries) return error.Invalid;
            // Validate both complete structures before the first MMIO access.
            // A valid selected entry does not excuse an out-of-aperture table
            // or PBA advertised by malformed capability metadata.
            const table_bytes = @as(u64, entries) * 16;
            const pba_bytes = ((@as(u64, entries) + 63) / 64) * 8;
            const table = interruptBarAddress(io, loc, io.config(loc, cap + 4, 4) orelse return error.Unsupported, table_bytes) orelse return error.Unsupported;
            const pba = interruptBarAddress(io, loc, io.config(loc, cap + 8, 4) orelse return error.Unsupported, pba_bytes) orelse return error.Unsupported;
            const first = if (kind == .auto) 0 else entry;
            const end = if (kind == .auto) entries else entry + 1;
            var index = first;
            while (index < end) : (index += 1) {
                const address = std.math.add(u64, table, @as(u64, index) * 16 + 12) catch return error.Unsupported;
                const flags = io.readWord(address) orelse return error.Unsupported;
                was_unmasked = was_unmasked or (flags & 1) == 0;
                const next_flags = if (masked) flags | 1 else flags & ~@as(u32, 1);
                if (!io.writeWord(address, next_flags)) return error.Unsupported;
                if (((io.readWord(address) orelse return error.Unsupported) & 1) != (next_flags & 1)) return error.NotReady;
                const pending_address = std.math.add(u64, pba, @as(u64, index / 32) * 4) catch return error.Unsupported;
                const word = io.readWord(pending_address) orelse return error.Unsupported;
                pending = pending or (word & (@as(u32, 1) << @as(u5, @intCast(index & 31)))) != 0;
            }
        }
        cap = @truncate((header >> 8) & 0xfc);
    }
    if (!found) return error.Unsupported;
    return .{ .pending = pending, .was_unmasked = was_unmasked };
}

fn maskInterruptSource(io: anytype, loc: Location, kind: InterruptKind, entry: u32) InterruptLeaseError!bool {
    return (try changeInterruptSourceMask(io, loc, kind, entry, true)).pending;
}

fn acquireInterruptLease(io: anytype, resource_id: u64, kind: InterruptKind, entry: u32) InterruptLeaseError!void {
    const mask = interruptEntryMask(kind, entry) orelse return error.Invalid;
    const first = interruptVectorBaseForResourceId(resource_id) orelse return error.Invalid;
    const index = (first - interrupt_vector_base) / interrupt_vectors_per_device;
    // AUTO is a wildcard event observer used by native daemon wait loops.
    // The explicit MSI/MSI-X objects own hardware; an observer must neither
    // mask their live vectors nor reopen a retired generation on its own.
    if (kind == .auto) {
        if (interrupt_route_leases[index] == 0) return error.NotReady;
        return;
    }
    if ((interrupt_route_leases[index] & mask) != 0) return error.Busy;
    const loc = locationFromResourceId(resource_id) orelse return error.Invalid;
    // A failed close cannot be bypassed by deriving a different IRQ kind.
    // Retry the original physical source's quiescence before reusing its
    // vector; pending bits require actual driver reset/source completion.
    for (&interrupt_route_retired[index], 0..) |*retired, raw_kind| {
        const old_kind: InterruptKind = @enumFromInt(raw_kind);
        var retry = retired.* & mask;
        if (retry == 0) continue;
        if (old_kind == .auto) {
            if (try maskInterruptSource(io, loc, old_kind, 0)) return error.NotReady;
            if (!io.drain(first, interrupt_vectors_per_device)) return error.NotReady;
            if (try maskInterruptSource(io, loc, old_kind, 0)) return error.NotReady;
            retired.* = 0;
        } else {
            while (retry != 0) {
                const old_entry: u4 = @intCast(@ctz(retry));
                const bit = @as(u16, 1) << old_entry;
                if (try maskInterruptSource(io, loc, old_kind, old_entry)) return error.NotReady;
                if (!io.drain(first + old_entry, 1)) return error.NotReady;
                if (try maskInterruptSource(io, loc, old_kind, old_entry)) return error.NotReady;
                retired.* &= ~bit;
                retry &= ~bit;
            }
        }
    }
    if (try maskInterruptSource(io, loc, kind, entry)) return error.NotReady;
    const vector = first + @as(u8, @intCast(if (kind == .auto) 0 else entry));
    const count: u8 = if (kind == .auto) interrupt_vectors_per_device else 1;
    if (!io.drain(vector, count)) return error.NotReady;
    // Source remains masked throughout the CPU ACK barrier and publication.
    if (try maskInterruptSource(io, loc, kind, entry)) return error.NotReady;
    interrupt_route_leases[index] |= mask;
}

fn prepareInterruptLease(io: anytype, resource_id: u64, kind: InterruptKind, entry: u32) InterruptLeaseError!bool {
    if (kind == .auto) {
        try acquireInterruptLease(io, resource_id, kind, entry);
        return false;
    }
    const mask = interruptEntryMask(kind, entry) orelse return error.Invalid;
    const first = interruptVectorBaseForResourceId(resource_id) orelse return error.Invalid;
    const index = (first - interrupt_vector_base) / interrupt_vectors_per_device;
    if ((interrupt_route_leases[index] & mask) != 0) return error.Busy;
    const loc = locationFromResourceId(resource_id) orelse return error.Invalid;
    interrupt_route_retired[index][@intFromEnum(kind)] |= mask;
    const prior = try changeInterruptSourceMask(io, loc, kind, entry, true);
    if (prior.pending) return error.NotReady;
    try acquireInterruptLease(io, resource_id, kind, entry);
    return prior.was_unmasked;
}

/// Returns whether the source was unmasked before registration. The caller
/// restores that state only after publishing the actual IRQ object.
pub fn acquireInterruptRoute(resource_id: u64, kind: u2, entry: u32) InterruptLeaseError!bool {
    return prepareInterruptLease(NativeInterruptIo{}, resource_id, @enumFromInt(kind), entry);
}

fn restoreInterruptLeaseMask(io: anytype, resource_id: u64, irq_kind: InterruptKind, entry: u32, was_unmasked: bool) bool {
    if (!was_unmasked) return true;
    if (irq_kind == .auto) return false;
    const mask = interruptEntryMask(irq_kind, entry) orelse return false;
    const first = interruptVectorBaseForResourceId(resource_id) orelse return false;
    const index = (first - interrupt_vector_base) / interrupt_vectors_per_device;
    if ((interrupt_route_leases[index] & mask) != mask) return false;
    const loc = locationFromResourceId(resource_id) orelse return false;
    _ = changeInterruptSourceMask(io, loc, irq_kind, entry, false) catch return false;
    return true;
}

pub fn restoreInterruptRouteMask(resource_id: u64, kind: u2, entry: u32, was_unmasked: bool) bool {
    return restoreInterruptLeaseMask(NativeInterruptIo{}, resource_id, @enumFromInt(kind), entry, was_unmasked);
}

pub fn interruptRouteLeased(resource_id: u64, kind: u2, entry: u32) bool {
    if (kind != @intFromEnum(InterruptKind.msi) and kind != @intFromEnum(InterruptKind.msix)) return false;
    const mask = interruptEntryMask(@enumFromInt(kind), entry) orelse return false;
    const first = interruptVectorBaseForResourceId(resource_id) orelse return false;
    const index = (first - interrupt_vector_base) / interrupt_vectors_per_device;
    return (interrupt_route_leases[index] & mask) == mask;
}

fn quiesceInterruptLease(io: anytype, resource_id: u64, kind: InterruptKind, entry: u32) InterruptLeaseError!void {
    if (!interruptRouteLeased(resource_id, @intFromEnum(kind), entry)) return error.Invalid;
    const first = interruptVectorBaseForResourceId(resource_id).?;
    const loc = locationFromResourceId(resource_id) orelse return error.Invalid;
    // Failure may leave the source masked, but must not surrender its lease.
    // Pending device bits are never cleared in software on the owner's behalf.
    if (try maskInterruptSource(io, loc, kind, entry)) return error.NotReady;
    if (!io.drain(first + @as(u8, @intCast(entry)), 1)) return error.NotReady;
    if (try maskInterruptSource(io, loc, kind, entry)) return error.NotReady;
}

pub fn quiesceInterruptRoute(resource_id: u64, kind: u2, entry: u32) InterruptLeaseError!void {
    return quiesceInterruptLease(NativeInterruptIo{}, resource_id, @enumFromInt(kind), entry);
}

/// No-fail commit after quiesce, with the global kernel-state lock retained.
/// The caller unpublishes and marks the shared object retired before this call.
pub fn commitInterruptRouteRetirement(resource_id: u64, kind: u2, entry: u32) void {
    std.debug.assert(interruptRouteLeased(resource_id, kind, entry));
    const mask = interruptEntryMask(@enumFromInt(kind), entry).?;
    const first = interruptVectorBaseForResourceId(resource_id).?;
    const index = (first - interrupt_vector_base) / interrupt_vectors_per_device;
    interrupt_route_retired[index][kind] |= mask;
    interrupt_route_leases[index] &= ~mask;
}

fn releaseInterruptLease(io: anytype, resource_id: u64, kind: InterruptKind, entry: u32) bool {
    const mask = interruptEntryMask(kind, entry) orelse return false;
    const first = interruptVectorBaseForResourceId(resource_id) orelse return false;
    const index = (first - interrupt_vector_base) / interrupt_vectors_per_device;
    if (kind == .auto) return true;
    if ((interrupt_route_leases[index] & mask) != mask) return false;
    interrupt_route_leases[index] &= ~mask;
    interrupt_route_retired[index][@intFromEnum(kind)] |= mask;
    const loc = locationFromResourceId(resource_id) orelse return false;
    _ = maskInterruptSource(io, loc, kind, entry) catch return false;
    const vector = first + @as(u8, @intCast(if (kind == .auto) 0 else entry));
    return io.drain(vector, if (kind == .auto) interrupt_vectors_per_device else 1);
}

pub fn releaseInterruptRoute(resource_id: u64, kind: u2, entry: u32) bool {
    return releaseInterruptLease(NativeInterruptIo{}, resource_id, @enumFromInt(kind), entry);
}

test "IRQ release masks source and pending MSI-X blocks same-vector reuse until reset" {
    const Fixture = struct {
        registers: [64]u32 = [_]u32{0} ** 64,
        table: [64]u32 = [_]u32{0} ** 64,
        pending: u32 = 0,
        drains: usize = 0,
        drain_ok: bool = true,
        mmio_available: bool = true,
        ignore_mask_write: bool = false,
        pending_after_drain: u32 = 0,
        fail_mmio_after_drain: bool = false,
        mmio_reads: usize = 0,
        mmio_writes: usize = 0,
        bar_start: u64 = 0x1000,
        bar_size: u64 = 0x2000,
        fn config(self: *@This(), _: Location, offset: u8, width: u32) ?u32 {
            const word = self.registers[offset / 4] >> @as(u5, @intCast((offset & 3) * 8));
            return word & (if (width == 4) std.math.maxInt(u32) else (@as(u32, 1) << @as(u5, @intCast(width * 8))) - 1);
        }
        fn setConfig(self: *@This(), _: Location, offset: u8, width: u32, value: u32) bool {
            const shift: u5 = @intCast((offset & 3) * 8);
            const mask: u32 = if (width == 4) std.math.maxInt(u32) else ((@as(u32, 1) << @as(u5, @intCast(width * 8))) - 1) << shift;
            self.registers[offset / 4] = (self.registers[offset / 4] & ~mask) | ((value << shift) & mask);
            return true;
        }
        fn authorizedBar(self: *@This(), _: Location, bar: u8) ?BarInfo {
            const live = @as(u64, self.registers[0x10 / 4]) |
                (@as(u64, self.registers[0x14 / 4]) << 32);
            if (bar != 0 or live != self.bar_start) return null;
            return .{ .start = self.bar_start, .size = self.bar_size, .flags = bar_flag_mem };
        }
        fn readWord(self: *@This(), address: u64) ?u32 {
            self.mmio_reads += 1;
            if (!self.mmio_available) return null;
            if (address == self.bar_start + 0x1000) return self.pending;
            if (address < self.bar_start or address >= self.bar_start + 0x100 or (address & 3) != 0) return null;
            return self.table[@intCast((address - self.bar_start) / 4)];
        }
        fn writeWord(self: *@This(), address: u64, value: u32) bool {
            self.mmio_writes += 1;
            if (self.ignore_mask_write) return true;
            // PBA is intentionally read-only, as on real MSI-X hardware.
            if (address < self.bar_start or address >= self.bar_start + 0x100 or (address & 3) != 0) return false;
            self.table[@intCast((address - self.bar_start) / 4)] = value;
            return true;
        }
        fn drain(self: *@This(), first: u8, count: u8) bool {
            if (first != 0x53 or count != 1) return false;
            self.drains += 1;
            self.pending |= self.pending_after_drain;
            if (self.fail_mmio_after_drain) self.mmio_available = false;
            return self.drain_ok;
        }
    };
    clearInterruptRoutes();
    defer clearInterruptRoutes();
    const device = resourceIdFromLocation(.{ .bus = 0, .device = 3, .function = 0 });
    try std.testing.expect(registerInterruptRoute(device, 0));
    var fixture = Fixture{};
    fixture.registers[0x04 / 4] = 0x00100002; // capability list + Memory Enable
    fixture.registers[0x10 / 4] = 0x1000;
    fixture.registers[0x34 / 4] = 0x40;
    fixture.registers[0x40 / 4] = 0x800f0011; // enabled MSI-X, sixteen entries
    fixture.registers[0x44 / 4] = 0;
    fixture.registers[0x48 / 4] = 0x1000;
    // Malformed capability offsets/extent or a relocated BAR must fail before
    // even a read (and especially a mask write) reaches another MMIO region.
    const loc = locationFromResourceId(device).?;
    // Table/PBA offsets retain all 64 BAR bits; relocating the upper DWORD
    // must still be rejected before touching either MMIO structure.
    var high = fixture;
    high.bar_start = 0x380000000000;
    high.registers[0x10 / 4] = 0;
    high.registers[0x14 / 4] = 0x3800;
    try std.testing.expect(!try maskInterruptSource(&high, loc, .msix, 3));
    try std.testing.expectEqual(@as(u32, 1), high.table[15]);
    high.pending = 1 << 3;
    try std.testing.expect(try maskInterruptSource(&high, loc, .msix, 3));
    high.registers[0x14 / 4] = 0x3801;
    const reads_before_relocation = high.mmio_reads;
    const writes_before_relocation = high.mmio_writes;
    try std.testing.expectError(error.Unsupported, maskInterruptSource(&high, loc, .msix, 3));
    try std.testing.expectEqual(reads_before_relocation, high.mmio_reads);
    try std.testing.expectEqual(writes_before_relocation, high.mmio_writes);
    fixture.registers[0x10 / 4] = 0x3000;
    try std.testing.expectError(error.Unsupported, maskInterruptSource(&fixture, loc, .msix, 3));
    fixture.registers[0x10 / 4] = 0x1000;
    fixture.registers[0x44 / 4] = 0x1ff8; // first entry/full table extends past BAR
    try std.testing.expectError(error.Unsupported, maskInterruptSource(&fixture, loc, .msix, 3));
    fixture.registers[0x44 / 4] = 6; // invalid BIR
    try std.testing.expectError(error.Unsupported, maskInterruptSource(&fixture, loc, .msix, 3));
    fixture.registers[0x44 / 4] = 0;
    fixture.registers[0x48 / 4] = 0x2000; // PBA starts at aperture end
    try std.testing.expectError(error.Unsupported, maskInterruptSource(&fixture, loc, .msix, 3));
    fixture.registers[0x48 / 4] = 0x1000;
    fixture.bar_size = 0x1004; // byte tail is too short even inside its last page
    try std.testing.expectError(error.Unsupported, maskInterruptSource(&fixture, loc, .msix, 3));
    fixture.bar_size = 0x2000;
    fixture.registers[0x40 / 4] = 0x87ff0011; // full 2048-entry table will not fit
    try std.testing.expectError(error.Unsupported, maskInterruptSource(&fixture, loc, .msix, 3));
    fixture.registers[0x40 / 4] = 0x800f0011;
    try std.testing.expectEqual(@as(usize, 0), fixture.mmio_reads);
    try std.testing.expectEqual(@as(usize, 0), fixture.mmio_writes);
    try std.testing.expectError(error.NotReady, acquireInterruptLease(&fixture, device, .auto, 0));
    const was_unmasked = try prepareInterruptLease(&fixture, device, .msix, 3);
    try std.testing.expect(was_unmasked);
    try std.testing.expectEqual(@as(u32, 1), fixture.table[15] & 1);
    try std.testing.expect(restoreInterruptLeaseMask(&fixture, device, .msix, 3, was_unmasked));
    try std.testing.expectEqual(@as(u32, 0), fixture.table[15] & 1);
    const drains_before_observer = fixture.drains;
    try acquireInterruptLease(&fixture, device, .auto, 0);
    try std.testing.expect(releaseInterruptLease(&fixture, device, .auto, 0));
    try std.testing.expectEqual(drains_before_observer, fixture.drains);
    try std.testing.expectEqual(@as(u32, 0), fixture.table[15] & 1);
    try std.testing.expectError(error.Busy, acquireInterruptLease(&fixture, device, .msix, 3));
    // Explicit retirement's preparation preserves lease ownership on every
    // failure. Another derive cannot take it from the still-live IRQ object.
    fixture.mmio_available = false;
    try std.testing.expectError(error.Unsupported, quiesceInterruptLease(&fixture, device, .msix, 3));
    fixture.mmio_available = true;
    fixture.ignore_mask_write = true;
    try std.testing.expectError(error.NotReady, quiesceInterruptLease(&fixture, device, .msix, 3));
    fixture.ignore_mask_write = false;
    fixture.pending = 1 << 3;
    try std.testing.expectError(error.NotReady, quiesceInterruptLease(&fixture, device, .msix, 3));
    fixture.pending = 0;
    fixture.drain_ok = false;
    try std.testing.expectError(error.NotReady, quiesceInterruptLease(&fixture, device, .msix, 3));
    try std.testing.expect(interruptRouteLeased(device, @intFromEnum(InterruptKind.msix), 3));
    try std.testing.expectError(error.Busy, acquireInterruptLease(&fixture, device, .msix, 3));
    fixture.drain_ok = true;
    fixture.pending_after_drain = 1 << 3;
    try std.testing.expectError(error.NotReady, quiesceInterruptLease(&fixture, device, .msix, 3));
    try std.testing.expect(interruptRouteLeased(device, @intFromEnum(InterruptKind.msix), 3));
    fixture.pending_after_drain = 0;
    fixture.pending = 0;
    fixture.fail_mmio_after_drain = true;
    try std.testing.expectError(error.Unsupported, quiesceInterruptLease(&fixture, device, .msix, 3));
    try std.testing.expect(interruptRouteLeased(device, @intFromEnum(InterruptKind.msix), 3));
    fixture.fail_mmio_after_drain = false;
    fixture.mmio_available = true;
    try quiesceInterruptLease(&fixture, device, .msix, 3);
    commitInterruptRouteRetirement(device, @intFromEnum(InterruptKind.msix), 3);
    try std.testing.expect(!interruptRouteLeased(device, @intFromEnum(InterruptKind.msix), 3));
    try std.testing.expectError(error.Invalid, quiesceInterruptLease(&fixture, device, .msix, 3));
    try acquireInterruptLease(&fixture, device, .msix, 3);
    fixture.table[15] = 0; // Device owner enabled its installed handler.
    fixture.pending = 1 << 3;
    try std.testing.expect(releaseInterruptLease(&fixture, device, .msix, 3));
    try std.testing.expectError(error.NotReady, acquireInterruptLease(&fixture, device, .auto, 0));
    try std.testing.expectEqual(@as(u32, 1), fixture.table[15] & 1);
    try std.testing.expectError(error.NotReady, acquireInterruptLease(&fixture, device, .msix, 3));
    try std.testing.expectEqual(@as(u32, 1 << 3), fixture.pending);
    fixture.pending = 0; // Device reset/source quiescence, not a kernel PBA write.
    fixture.drain_ok = false;
    try std.testing.expectError(error.NotReady, acquireInterruptLease(&fixture, device, .msix, 3));
    fixture.drain_ok = true;
    try acquireInterruptLease(&fixture, device, .msix, 3);
    try std.testing.expect(releaseInterruptLease(&fixture, device, .msix, 3));
    try std.testing.expect(fixture.drains >= 4);

    // The same production lifecycle handles MSI's per-vector mask/pending
    // registers, and refuses a device which cannot provide that evidence.
    clearInterruptRoutes();
    try std.testing.expect(registerInterruptRoute(device, 0));
    fixture.registers[0x40 / 4] = 0x01050005; // MSI, four messages, per-vector mask
    fixture.registers[0x4c / 4] = 0;
    fixture.registers[0x50 / 4] = 0;
    const msi_unmasked = try prepareInterruptLease(&fixture, device, .msi, 3);
    try std.testing.expect(msi_unmasked);
    try std.testing.expectEqual(@as(u32, 1 << 3), fixture.registers[0x4c / 4]);
    try std.testing.expect(restoreInterruptLeaseMask(&fixture, device, .msi, 3, msi_unmasked));
    fixture.registers[0x50 / 4] = 1 << 3;
    try std.testing.expect(releaseInterruptLease(&fixture, device, .msi, 3));
    try std.testing.expectError(error.NotReady, prepareInterruptLease(&fixture, device, .msi, 3));
    fixture.registers[0x50 / 4] = 0;
    _ = try prepareInterruptLease(&fixture, device, .msi, 3);
    try std.testing.expect(releaseInterruptLease(&fixture, device, .msi, 3));
    fixture.registers[0x40 / 4] = 0x00050005; // No per-vector mask support.
    try std.testing.expectError(error.Unsupported, prepareInterruptLease(&fixture, device, .msi, 3));
    try std.testing.expectEqual(@as(u32, 0), fixture.registers[0x40 / 4] & 0x00010000);
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

fn probeBarInfo(loc: Location, bar_index: u8) ?BarInfo {
    if (readVendorId(loc) == 0xFFFF) return null;
    const bar_count: u8 = switch (readHeaderType(loc) & 0x7f) {
        0 => 6,
        1 => 2,
        else => return null,
    };
    if (bar_index >= bar_count) return null;
    // Walk real low slots: a 64-bit BAR's high address DWORD can itself have
    // bits that look like a low BAR type, so examining only the previous slot
    // would reject an unrelated BAR following it.
    var preceding: u8 = 0;
    while (preceding < bar_index) {
        const low = readConfigU32(loc, 0x10 + preceding * 4);
        preceding += if ((low & 7) == 4) @as(u8, 2) else 1;
    }
    if (preceding != bar_index) return null;

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
        if (bar_index + 1 >= bar_count) return null;
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

    if (mem_type != 0) return null;

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

test "PCI DMA boot scan skips upper BAR slots and clips ROM ECAM and unknown headers" {
    const Reader = struct {
        config: [64]u32 = @splat(0),
        fn read(self: @This(), loc: Location, offset: u8) u32 {
            if (loc.bus != 0 or loc.device != 0 or loc.function != 0) return 0xffffffff;
            return self.config[offset / 4];
        }
    };
    var reader = Reader{};
    reader.config[0] = 0x12348086;
    const start = 0x80000000;
    const end = 0xfee00000;
    // Unimplemented/zero-base slots do not constrain the platform window.
    try std.testing.expectEqual(@as(u64, end), bootDmaWindowEndWithReader(start, end, &.{}, reader));
    reader.config[4] = 0x0000000c;
    reader.config[5] = 0xa0000000; // High DWORD must not be parsed as a low BAR.
    try std.testing.expectEqual(@as(u64, end), bootDmaWindowEndWithReader(start, end, &.{}, reader));
    reader.config[6] = 0xc0000000;
    try std.testing.expectEqual(@as(u64, 0xc0000000), bootDmaWindowEndWithReader(start, end, &.{}, reader));
    reader.config[0x30 / 4] = 0xa0000001;
    try std.testing.expectEqual(@as(u64, 0xa0000000), bootDmaWindowEndWithReader(start, end, &.{}, reader));
    reader.config[0x30 / 4] = 0;
    const windows = [_]EcamWindow{.{ .base = 0xb0000000, .segment = 0, .bus_start = 0, .bus_end = 255 }};
    try std.testing.expectEqual(@as(u64, 0xb0000000), bootDmaWindowEndWithReader(start, end, &windows, reader));
    reader.config[6] = 0x40000000; // Unknown low BAR extent: fail closed.
    try std.testing.expectEqual(@as(u64, start), bootDmaWindowEndWithReader(start, end, &.{}, reader));
    reader.config[6] = 0xc0000002; // Unsupported memory type.
    try std.testing.expectEqual(@as(u64, start), bootDmaWindowEndWithReader(start, end, &.{}, reader));
    reader.config[6] = 0;
    reader.config[3] = 2 << 16; // CardBus windows are not implemented.
    try std.testing.expectEqual(@as(u64, start), bootDmaWindowEndWithReader(start, end, &.{}, reader));
}

test "PCI DMA boot scan leaves translated IOVAs below a UEFI GOP BAR" {
    const Reader = struct {
        fn read(_: @This(), loc: Location, offset: u8) u32 {
            if (loc.bus != 0 or loc.device != 1 or loc.function != 0)
                return 0xffffffff;
            return switch (offset) {
                0 => 0x11111234,
                0x10 => 0x80000000,
                else => 0,
            };
        }
    };
    const start = @import("iova.zig").window_start;
    try std.testing.expectEqual(@as(u64, 0x01000000), start);
    try std.testing.expectEqual(@as(u64, 0x80000000),
        bootDmaWindowEndWithReader(start, @import("iova.zig").window_ceiling,
            &.{}, Reader{}));
}

test "PCI DMA bridge windows clip 32-bit and 64-bit ranges and reject unknown types" {
    const start = 0x80000000;
    const end = 0xfee00000;
    try std.testing.expectEqual(@as(u64, 0xa0000000), dmaBridgeWindowEnd(start, end, 0xa0f0a000, 0, 0, false));
    try std.testing.expectEqual(@as(u64, end), dmaBridgeWindowEnd(start, end, 0x0000fff0, 0, 0, false));
    try std.testing.expectEqual(@as(u64, end), dmaBridgeWindowEnd(start, end, 0x00010001, 0x3800, 0x3800, true));
    try std.testing.expectEqual(@as(u64, start), dmaBridgeWindowEnd(start, end, 0xa0007000, 0, 0, false));
    try std.testing.expectEqual(@as(u64, start), dmaBridgeWindowEnd(start, end, 0xa000a001, 0, 0, true));
}

test "config address encoding" {
    const loc = Location{
        .bus = 0x12,
        .device = 0x03,
        .function = 0x04,
    };
    try std.testing.expectEqual(@as(u32, 0x8012_1C20), configAddress(loc, 0x20));
}

test "PCI subword command writes preserve neighboring W1C status and transaction width" {
    const Fixture = struct {
        latch: u32 = 0,
        command: u16 = 0,
        status: u16 = 0xf900,
        writes: u32 = 0,
        reads: u32 = 0,
        width: u32 = 0,

        fn read(self: *@This(), port: u16, width: u32) u32 {
            self.reads += 1;
            self.width = width;
            const value = @as(u32, self.command) | (@as(u32, self.status) << 16);
            return (value >> @as(u5, @intCast((port - config_data_port) * 8))) &
                (@as(u32, std.math.maxInt(u32)) >> @as(u5, @intCast(32 - width * 8)));
        }
        fn write(self: *@This(), port: u16, width: u32, value: u32) void {
            if (port == config_address_port) {
                self.latch = value;
                return;
            }
            self.writes += 1;
            self.width = width;
            const lane = port - config_data_port;
            var i: u32 = 0;
            while (i < width) : (i += 1) {
                const byte: u8 = @truncate(value >> @as(u5, @intCast(i * 8)));
                const target = lane + i;
                if (target < 2) {
                    const shift: u4 = @intCast(target * 8);
                    self.command = (self.command & ~(@as(u16, 255) << shift)) | (@as(u16, byte) << shift);
                } else {
                    self.status &= ~(@as(u16, byte) << @as(u4, @intCast((target - 2) * 8)));
                }
            }
        }
    };
    var fixture = Fixture{};
    const loc = Location{ .bus = 0, .device = 3, .function = 0 };
    writeLegacyConfig(&fixture, loc, 4, 2, 0x407);
    try std.testing.expectEqual(@as(u16, 0x407), fixture.command);
    try std.testing.expectEqual(@as(u16, 0xf900), fixture.status);
    try std.testing.expectEqual(@as(u32, 2), fixture.width);
    try std.testing.expectEqual(@as(u32, 0), fixture.reads);
    writeLegacyConfig(&fixture, loc, 4, 1, 3);
    try std.testing.expectEqual(@as(u16, 0x403), fixture.command);
    try std.testing.expectEqual(@as(u16, 0xf900), fixture.status);
    try std.testing.expectEqual(@as(u32, 1), fixture.width);
    try std.testing.expectEqual(@as(u32, 2), fixture.writes);
    try std.testing.expectEqual(@as(u32, 0xf900), readLegacyConfig(&fixture, loc, 6, 2));
    try std.testing.expectEqual(@as(u32, 2), fixture.width);
}

test "PCI config widths boundaries and resource identities are validated" {
    const loc = Location{ .bus = 255, .device = 31, .function = 7 };
    try std.testing.expect(configAccessValid(loc, 255, 1));
    try std.testing.expect(configAccessValid(loc, 254, 2));
    try std.testing.expect(configAccessValid(loc, 252, 4));
    for ([_][2]u32{ .{ 255, 2 }, .{ 254, 4 }, .{ 0, 3 }, .{ 0, 0 }, .{ 4096, 1 }, .{ 256, 4 } }) |request|
        try std.testing.expect(!configAccessValid(loc, request[0], request[1]));
    try std.testing.expect(locationFromResourceId(pci_resource_id_prefix | 0x2000) == null);
    try std.testing.expect(locationFromResourceId(pci_resource_id_prefix | 8) == null);
    try std.testing.expect(locationFromResourceId(pci_resource_id_prefix | 0x0100_0000) == null);
}

fn mcfgChecksum(bytes: []u8) void {
    bytes[9] = 0;
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    bytes[9] = 0 -% sum;
}

test "MCFG validates checksum ranges overlap and bus-zero-relative 4KiB ECAM addresses" {
    var bytes = [_]u8{0} ** 76;
    @memcpy(bytes[0..4], "MCFG");
    std.mem.writeInt(u32, bytes[4..8], 60, .little);
    std.mem.writeInt(u64, bytes[44..52], 0xe000_0000, .little);
    bytes[54] = 32;
    bytes[55] = 63;
    mcfgChecksum(bytes[0..60]);
    const window = parseMcfg(bytes[0..60]).?.entries[0];
    try std.testing.expectEqual(@as(?u64, 0xe20f_fffc), window.address(.{ .bus = 32, .device = 31, .function = 7 }, 4092));
    try std.testing.expect(window.address(.{ .bus = 31, .device = 0, .function = 0 }, 0) == null);
    try std.testing.expect(window.address(.{ .bus = 32, .device = 0, .function = 0 }, 4096) == null);
    bytes[9] +%= 1;
    try std.testing.expect(parseMcfg(bytes[0..60]) == null);
    mcfgChecksum(bytes[0..60]);
    bytes[55] = 31;
    mcfgChecksum(bytes[0..60]);
    try std.testing.expect(parseMcfg(bytes[0..60]) == null);
    bytes[55] = 63;
    std.mem.writeInt(u32, bytes[4..8], 76, .little);
    @memcpy(bytes[60..76], bytes[44..60]);
    mcfgChecksum(&bytes);
    try std.testing.expect(parseMcfg(&bytes) == null);
    bytes[68] = 1; // A distinct segment is not an overlapping bus identity.
    mcfgChecksum(&bytes);
    try std.testing.expect(parseMcfg(&bytes) == null); // But physical ECAM aliases still overlap.
    std.mem.writeInt(u64, bytes[60..68], 0xf000_0000, .little);
    mcfgChecksum(&bytes);
    try std.testing.expectEqual(@as(usize, 2), parseMcfg(&bytes).?.count);
}

test "ECAM accesses preserve byte word and DWORD widths at the extended boundary" {
    var bytes: [4096]u8 align(4096) = [_]u8{0xa5} ** 4096;
    const base: u64 = @intFromPtr(&bytes);
    writeEcam(base + 4092, 4, 0x1122_3344);
    try std.testing.expectEqual(@as(u32, 0x1122_3344), readEcam(base + 4092, 4));
    writeEcam(base + 4092, 2, 0x5566);
    try std.testing.expectEqual(@as(u32, 0x1122_5566), readEcam(base + 4092, 4));
    writeEcam(base + 4095, 1, 0x77);
    try std.testing.expectEqual(@as(u32, 0x77), readEcam(base + 4095, 1));
    try std.testing.expectEqual(@as(u32, 0x7722), readEcam(base + 4094, 2));
    try std.testing.expectEqual(@as(u8, 0xa5), bytes[4091]);
}

test "partial 64-bit BAR mappings stay in the authorized page aperture" {
    const info = BarInfo{ .start = 0x1_0000_0000, .size = 0x10000, .flags = bar_flag_mem | bar_flag_64bit };
    try std.testing.expectEqual(@as(?u64, 0x1_0000_3000), barMappingPaddr(info, 0x3000, 0x2000));
    try std.testing.expectEqual(@as(?u64, 0x1_0000_f000), barMappingPaddr(info, 0xf000, 0x1000));
    try std.testing.expect(barMappingPaddr(info, 0xf000, 0x2000) == null);
    try std.testing.expect(barMappingPaddr(info, 0x10000, 0x1000) == null);
    try std.testing.expect(barMappingPaddr(info, 1, 0x1000) == null);
    try std.testing.expect(barMappingPaddr(info, 0, 0) == null);
    try std.testing.expect(barMappingPaddr(.{ .start = 0x1000, .size = 4096, .flags = bar_flag_io }, 0, 4096) == null);
    try std.testing.expect(barMappingPaddr(.{ .start = std.math.maxInt(u64) - 4095, .size = 8192, .flags = bar_flag_mem }, 0, 4096) == null);
}

test "authorized BAR identity rejects low and high relocation without changing its aperture" {
    const aperture = BarAperture{
        .info = .{ .start = 0x1_8000_0000, .size = 0x10000, .flags = bar_flag_mem | bar_flag_64bit },
        .low = 0x8000_0004,
        .high = 1,
    };
    try std.testing.expect(apertureMatches(aperture, 0x8000_0004, 1));
    try std.testing.expect(!apertureMatches(aperture, 0x9000_0004, 1));
    try std.testing.expect(!apertureMatches(aperture, 0x8000_0004, 2));
    try std.testing.expect(!apertureMatches(aperture, 0x8000_000c, 1));
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
