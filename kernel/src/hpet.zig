const std = @import("std");
const acpi = @import("acpi_tables.zig");
const physical_layout = @import("arch/x86_64/physical_layout.zig");
const mtrr = @import("arch/x86_64/mtrr.zig");

// IA-PC HPET 1.0a, sections 2.3.4-2.3.7 and 3.2.4. The shared main
// counter is the clocksource; interrupt comparators remain disabled.
// https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/software-developers-hpet-spec-1-0a.pdf
const capabilities_offset = 0x00;
const configuration_offset = 0x10;
const counter_offset = 0xf0;
const fs_per_ns: u64 = 1_000_000;
const register_bytes: u64 = 0x500;
const page_size: u64 = 4096;

pub const InitReason = enum {
    not_attempted,
    acpi_missing_or_invalid,
    invalid_address,
    outside_identity_range,
    bsp_cache_evidence_missing,
    bsp_cache_unsupported,
    boot_mapping_failed,
    boot_mapped,
    mmio_not_uc,
    invalid_capabilities,
    counter_32bit,
    counter_not_advancing,
    ready,
};
pub const Diagnostic = struct {
    reason: InitReason = .not_attempted,
    address: u64 = 0,
    capabilities: u64 = 0,
    period_fs: u32 = 0,
};
var init_diagnostic: Diagnostic = .{};

pub fn diagnostic() Diagnostic {
    return init_diagnostic;
}

const RegisterRange = struct { base: u64, bytes: u64 };
fn registerRange(address: u64) ?RegisterRange {
    if (address < 0x100000 or address >= physical_layout.identity_limit or
        register_bytes > physical_layout.identity_limit - address) return null;
    const base = address & ~(page_size - 1);
    const end = std.mem.alignForward(u64, address + register_bytes, page_size);
    return .{ .base = base, .bytes = end - base };
}

fn discover(rsdp: u64) ?u64 {
    init_diagnostic = .{};
    const table = acpi.find(rsdp, "HPET", 56) orelse {
        init_diagnostic.reason = .acpi_missing_or_invalid;
        return null;
    };
    const address = registerAddress(table) orelse {
        init_diagnostic.reason = .invalid_address;
        return null;
    };
    init_diagnostic.address = address;
    return address;
}

/// Before AP startup, change the firmware-declared identity aperture itself.
/// UC_MINUS over a WB MTRR is not sufficient, and a late remap would require
/// a global cache/TLB transition. Do not weaken the existing MMIO proof.
pub fn prepareBootMmio(rsdp: u64) bool {
    const address = discover(rsdp) orelse return false;
    const range = registerRange(address) orelse {
        init_diagnostic.reason = .outside_identity_range;
        return false;
    };
    var snapshot: mtrr.CacheSnapshot = .{};
    if (!mtrr.captureCacheSnapshot(&snapshot)) {
        init_diagnostic.reason = .bsp_cache_evidence_missing;
        return false;
    }
    if (!snapshot.rangeSupportsUncachedMapping(range.base, range.bytes)) {
        init_diagnostic.reason = .bsp_cache_unsupported;
        return false;
    }
    if (!@import("arch/x86_64/platform.zig").mapBootMmioIdentityRange(range.base, range.bytes)) {
        init_diagnostic.reason = .boot_mapping_failed;
        return false;
    }
    init_diagnostic.reason = .boot_mapped;
    return true;
}

pub fn registerAddress(table: []const u8) ?u64 {
    if (table.len < 56 or !std.mem.eql(u8, table[0..4], "HPET")) return null;
    const length = std.mem.readInt(u32, table[4..8], .little);
    if (length < 56 or length > table.len) return null;
    var checksum: u8 = 0;
    for (table[0..length]) |byte| checksum +%= byte;
    if (checksum != 0) return null;
    // GAS must describe system memory at bit offset zero. The register
    // block is 1 KiB; firmware may leave the GAS bit width unspecified.
    if (table[40] != 0 or table[42] != 0) return null;
    const address = std.mem.readInt(u64, table[44..52], .little);
    if (address < 0x100000 or (address & 1023) != 0) return null;
    return address;
}

pub fn period(capabilities: u64) ?u32 {
    const value: u32 = @truncate(capabilities >> 32);
    // A 32-bit counter needs wrap accounting; do not advertise it as this
    // unbounded, cross-CPU 64-bit clocksource.
    if ((capabilities & (1 << 13)) == 0 or (capabilities & 0xff) == 0 or
        value == 0 or value > 100_000_000) return null;
    return value;
}

pub fn elapsedNs(count: u64, period_fs: u32) u64 {
    const ns = @as(u128, count) * period_fs / fs_per_ns;
    return @intCast(@min(ns, std.math.maxInt(u64)));
}

const Counter = struct {
    address: u64 = 0,
    period_fs: u32 = 0,
    origin: u64 = 0,
    epoch_ns: u64 = 0,

    fn read(self: *const Counter, offset: u64) u64 {
        const reg: *volatile u64 = @ptrFromInt(self.address + offset);
        return reg.*;
    }

    fn write(self: *const Counter, offset: u64, value: u64) void {
        const reg: *volatile u64 = @ptrFromInt(self.address + offset);
        reg.* = value;
    }
};

var counter: Counter = .{};

pub fn staticEnd() usize {
    return @max(@intFromPtr(&counter) + @sizeOf(Counter), @intFromPtr(&init_diagnostic) + @sizeOf(Diagnostic));
}

// Called after all CPUs have published immutable cache snapshots and before
// user execution. Reject the complete register mapping before MMIO access.
pub fn initialize(rsdp: u64, mmio_allowed: *const fn (u64, u64) bool) bool {
    const address = discover(rsdp) orelse return false;
    const range = registerRange(address) orelse {
        init_diagnostic.reason = .outside_identity_range;
        return false;
    };
    if (!mmio_allowed(range.base, range.bytes)) {
        init_diagnostic.reason = .mmio_not_uc;
        return false;
    }
    var candidate: Counter = .{ .address = address };
    const caps = candidate.read(capabilities_offset);
    init_diagnostic.capabilities = caps;
    init_diagnostic.period_fs = @truncate(caps >> 32);
    candidate.period_fs = period(caps) orelse {
        init_diagnostic.reason = if ((caps & (1 << 13)) == 0) .counter_32bit else .invalid_capabilities;
        return false;
    };
    const config = candidate.read(configuration_offset);
    candidate.write(configuration_offset, config & ~@as(u64, 3));
    const comparators = ((caps >> 8) & 31) + 1;
    for (0..comparators) |index| {
        const offset = 0x100 + @as(u64, @intCast(index)) * 0x20;
        const timer_config = candidate.read(offset);
        // Disable comparator interrupts and FSB delivery, preserve RO and
        // reserved fields. No legacy replacement IRQ routing is enabled.
        candidate.write(offset, timer_config & ~@as(u64, (1 << 2) | (1 << 14)));
    }
    candidate.write(configuration_offset, (config & ~@as(u64, 2)) | 1);
    candidate.origin = candidate.read(counter_offset);
    candidate.epoch_ns = @import("scheduler_connection.zig").lapic_tick_count *| 1_000_000;
    for (0..10000) |_| {
        if (candidate.read(counter_offset) > candidate.origin) {
            counter = candidate;
            init_diagnostic.reason = .ready;
            return true;
        }
        asm volatile ("pause");
    }
    candidate.write(configuration_offset, config & ~@as(u64, 3));
    init_diagnostic.reason = .counter_not_advancing;
    return false;
}

test "HPET boot mapping bounds only the firmware-declared register pages" {
    const aligned = registerRange(0xfed00000).?;
    try std.testing.expectEqual(@as(u64, 0xfed00000), aligned.base);
    try std.testing.expectEqual(@as(u64, 4096), aligned.bytes);
    const crossing = registerRange(0xfed00c00).?;
    try std.testing.expectEqual(@as(u64, 8192), crossing.bytes);
    try std.testing.expect(registerRange(0) == null);
    try std.testing.expect(registerRange(physical_layout.identity_limit - 1024) == null);
    try std.testing.expect(registerRange(std.math.maxInt(u64)) == null);
}

pub fn monotonicNs() ?u64 {
    if (counter.address == 0) return null;
    const value = counter.read(counter_offset);
    return counter.epoch_ns +| elapsedNs(value -% counter.origin, counter.period_fs);
}

pub fn resolutionNs() ?u64 {
    if (counter.address == 0) return null;
    return (@as(u64, counter.period_fs) + fs_per_ns - 1) / fs_per_ns;
}

test "HPET counter conversion retains sub-millisecond precision and saturates" {
    try std.testing.expectEqual(@as(u64, 100), elapsedNs(1, 100_000_000));
    try std.testing.expectEqual(@as(u64, 125), elapsedNs(3, 41_666_667));
    try std.testing.expectEqual(@as(u64, 100_000), elapsedNs(1000, 100_000_000));
    try std.testing.expectEqual(std.math.maxInt(u64), elapsedNs(std.math.maxInt(u64), 100_000_000));
    try std.testing.expectEqual(@as(?u32, 100_000_000), period((@as(u64, 100_000_000) << 32) | 0x2001));
    try std.testing.expect(period(0x2001) == null);
    try std.testing.expect(period((@as(u64, 100_000_001) << 32) | 0x2001) == null);
    try std.testing.expect(period((@as(u64, 100_000_000) << 32) | 1) == null);
}

fn checksumTable(table: []u8) void {
    table[9] = 0;
    var sum: u8 = 0;
    for (table) |byte| sum +%= byte;
    table[9] = 0 -% sum;
}

test "HPET ACPI validates checksum, length, GAS and register alignment" {
    var table = [_]u8{0} ** 56;
    @memcpy(table[0..4], "HPET");
    std.mem.writeInt(u32, table[4..8], table.len, .little);
    std.mem.writeInt(u64, table[44..52], 0xfed00000, .little);
    checksumTable(&table);
    try std.testing.expectEqual(@as(?u64, 0xfed00000), registerAddress(&table));
    table[9] +%= 1;
    try std.testing.expect(registerAddress(&table) == null);
    table[40] = 1;
    checksumTable(&table);
    try std.testing.expect(registerAddress(&table) == null);
    table[40] = 0;
    table[44] = 1;
    checksumTable(&table);
    try std.testing.expect(registerAddress(&table) == null);
    try std.testing.expect(registerAddress(table[0..55]) == null);
}
