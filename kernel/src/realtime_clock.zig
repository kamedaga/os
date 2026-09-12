const std = @import("std");
const rtc = @import("rtc.zig");
const hpet = @import("hpet.zig");

// The fallback clock is published by the BSP. The high-resolution HPET
// counter is shared by all CPUs, so initialization proves one cache policy
// across the complete online set before enabling it.
pub const Clock = struct {
    frequency_hz: u64 = 0,
    epoch_seconds: u64 = 0,
    epoch_counter: u64 = 0,
    published_seconds: u64 = 0,
    ready: bool = false,

    pub fn start(self: *Clock, seconds: u64, counter: u64) void {
        if (self.frequency_hz == 0 or seconds == 0) return;
        self.epoch_seconds = seconds;
        self.epoch_counter = counter;
        @atomicStore(u64, &self.published_seconds, seconds, .release);
        @atomicStore(bool, &self.ready, true, .release);
    }

    pub fn update(self: *Clock, counter: u64) void {
        if (!@atomicLoad(bool, &self.ready, .acquire)) return;
        if (counter < self.epoch_counter) return;
        const seconds = self.epoch_seconds +| ((counter - self.epoch_counter) / self.frequency_hz);
        // Never regress if a counter sample is older than the last update.
        if (seconds > @atomicLoad(u64, &self.published_seconds, .monotonic))
            @atomicStore(u64, &self.published_seconds, seconds, .release);
    }

    pub fn read(self: *const Clock) ?u64 {
        if (!@atomicLoad(bool, &self.ready, .acquire)) return null;
        return @atomicLoad(u64, &self.published_seconds, .acquire);
    }
};

var clock: Clock = .{};
const PvTime = extern struct {
    version: u32 = 0,
    pad: u32 = 0,
    tsc_timestamp: u64 = 0,
    system_time: u64 = 0,
    multiplier: u32 = 0,
    shift: i8 = 0,
    flags: u8 = 0,
    padding: [2]u8 = .{ 0, 0 },
};
var pv_time: PvTime align(64) = .{};
var pv_enabled: bool = false;
var highres_realtime_offset_ns: u64 = 0;

pub fn kernelStaticStorageEndAddr() usize {
    return @max(hpet.staticEnd(), @max(@intFromPtr(&highres_realtime_offset_ns) + @sizeOf(u64), @max(@intFromPtr(&clock) + @sizeOf(Clock), @max(@intFromPtr(&pv_time) + @sizeOf(PvTime), @intFromPtr(&pv_enabled) + @sizeOf(bool)))));
}

// The existing TSC/pvclock conversion is BSP-only. The HPET main counter
// supplies one cross-CPU domain; LAPIC remains the deadline interrupt device.
pub fn initializeHighResolution(
    rsdp: u64,
    mmio_allowed: *const fn (u64, u64) bool,
) bool {
    if (!@import("lapic.zig").highResolutionReady() or
        !hpet.initialize(rsdp, mmio_allowed)) return false;
    const now = hpet.monotonicNs() orelse return false;
    highres_realtime_offset_ns = (unixTimeSeconds() *| 1_000_000_000) -| now;
    return true;
}

pub fn monotonicNs() ?u64 {
    return hpet.monotonicNs();
}

pub fn resolutionNs() ?u64 {
    return hpet.resolutionNs();
}

pub fn realtimeNs() ?u64 {
    return highres_realtime_offset_ns +| (hpet.monotonicNs() orelse return null);
}

fn cpuid(leaf: u32) struct { eax: u32, ebx: u32, ecx: u32, edx: u32 } {
    var eax: u32 = undefined;
    var ebx: u32 = undefined;
    var ecx: u32 = undefined;
    var edx: u32 = undefined;
    asm volatile ("cpuid"
        : [eax] "={eax}" (eax),
          [ebx] "={ebx}" (ebx),
          [ecx] "={ecx}" (ecx),
          [edx] "={edx}" (edx),
        : [leaf] "{eax}" (leaf),
          [subleaf] "{ecx}" (@as(u32, 0)),
    );
    return .{ .eax = eax, .ebx = ebx, .ecx = ecx, .edx = edx };
}

pub fn readCounter() u64 {
    var lo: u32 = undefined;
    var hi: u32 = undefined;
    asm volatile ("lfence; rdtsc"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
        :
        : .{ .memory = true });
    return (@as(u64, hi) << 32) | lo;
}

pub fn frequencyFromMeasurement(cycles: u64, reference_ticks: u64, reference_hz: u64) ?u64 {
    if (cycles == 0 or reference_ticks == 0 or reference_hz == 0) return null;
    const hz = (@as(u128, cycles) * reference_hz + reference_ticks / 2) / reference_ticks;
    if (hz == 0 or hz > std.math.maxInt(u64)) return null;
    return @intCast(hz);
}

pub fn configureCounter(cycles: u64, pit_ticks: u64, pit_hz: u64) void {
    // A variable-rate/stopping TSC cannot be used as an elapsed-time source.
    if (cpuid(0x80000000).eax < 0x80000007 or
        (cpuid(0x80000007).edx & (1 << 8)) == 0) return;
    clock.frequency_hz = frequencyFromMeasurement(cycles, pit_ticks, pit_hz) orelse 0;
}

fn writePvMsr(value: u64) void {
    asm volatile ("wrmsr"
        :
        : [msr] "{ecx}" (@as(u32, 0x4b564d01)),
          [lo] "{eax}" (@as(u32, @truncate(value))),
          [hi] "{edx}" (@as(u32, @truncate(value >> 32))),
        : .{ .memory = true });
}

pub fn pvNanoseconds(delta: u64, multiplier: u32, shift: i8, system_time: u64) ?u64 {
    if (multiplier == 0 or shift < -63 or shift > 63) return null;
    const scaled: u128 = if (shift < 0)
        delta >> @as(u6, @intCast(-@as(i16, shift)))
    else
        @as(u128, delta) << @as(u7, @intCast(shift));
    const product = @mulWithOverflow(scaled, @as(u128, multiplier));
    if (product[1] != 0) return null;
    const ns = (product[0] >> 32) + system_time;
    return if (ns <= std.math.maxInt(u64)) @intCast(ns) else null;
}

fn readPvCounter() ?u64 {
    const p: *const volatile PvTime = &pv_time;
    // KVM may replace the conversion parameters on vCPU migration. Take a
    // versioned snapshot on the registered BSP, not on an arbitrary AP.
    for (0..8) |_| {
        const version = @atomicLoad(u32, &pv_time.version, .acquire);
        if ((version & 1) != 0) continue;
        const stamp = p.tsc_timestamp;
        const system_time = p.system_time;
        const multiplier = p.multiplier;
        const shift = p.shift;
        const counter = readCounter();
        asm volatile ("lfence" ::: .{ .memory = true });
        if (version != @atomicLoad(u32, &pv_time.version, .acquire)) continue;
        if (counter < stamp) return null;
        return pvNanoseconds(counter - stamp, multiplier, shift, system_time);
    }
    return null;
}

pub fn initialize(kernel_pointer_paddr: *const fn (usize) ?u64) void {
    const vendor = cpuid(0x40000000);
    if (vendor.eax >= 0x40000001 and vendor.ebx == 0x4b4d564b and
        vendor.ecx == 0x564b4d56 and vendor.edx == 0x4d and
        (cpuid(0x40000001).eax & (1 << 3)) != 0)
    {
        if (kernel_pointer_paddr(@intFromPtr(&pv_time))) |paddr| {
            writePvMsr(paddr | 1);
            if (readPvCounter()) |_| {
                const seconds = rtc.unixTimeSeconds();
                if (readPvCounter()) |counter| {
                    if (seconds != 0) {
                        pv_enabled = true;
                        clock.frequency_hz = 1_000_000_000;
                        clock.start(seconds, counter);
                        return;
                    }
                }
            }
            writePvMsr(0);
        }
    }
    if (clock.frequency_hz == 0) return;
    const seconds = rtc.unixTimeSeconds();
    clock.start(seconds, readCounter());
}

pub fn updateFromBootstrapTimer() void {
    if (!@atomicLoad(bool, &clock.ready, .acquire)) return;
    clock.update(if (pv_enabled) readPvCounter() orelse return else readCounter());
}

pub fn unixTimeSeconds() u64 {
    // Preserve the existing RTC behavior when calibration/RTC initialization
    // fails, or neither KVM pvclock nor an invariant TSC is available.
    return clock.read() orelse rtc.unixTimeSeconds();
}

pub fn counterFrequencyHz() u64 {
    return if (clock.read() != null) clock.frequency_hz else 0;
}

pub fn usesKvmClock() bool {
    return pv_enabled;
}

test "RTC epoch advances across missed timer ticks without per-call hardware reads" {
    var c: Clock = .{ .frequency_hz = 3500000000 };
    try std.testing.expect(c.read() == null);
    c.start(1788760000, 9000);
    c.update(9000 + 3499999999);
    try std.testing.expectEqual(@as(?u64, 1788760000), c.read());
    c.update(9000 + 12 * 3500000000);
    try std.testing.expectEqual(@as(?u64, 1788760012), c.read());
    c.update(8999);
    c.update(9000 + 3500000000);
    try std.testing.expectEqual(@as(?u64, 1788760012), c.read());
    for (0..10000) |_| try std.testing.expectEqual(@as(?u64, 1788760012), c.read());
}

test "invalid initialization falls back and epoch arithmetic saturates" {
    var c: Clock = .{};
    c.start(42, 0);
    try std.testing.expect(c.read() == null);
    c.frequency_hz = 1;
    c.start(0, 0);
    try std.testing.expect(c.read() == null);
    c.start(std.math.maxInt(u64) - 1, 0);
    c.update(10);
    try std.testing.expectEqual(@as(?u64, std.math.maxInt(u64)), c.read());
}

test "counter calibration validates and uses wide arithmetic" {
    try std.testing.expectEqual(@as(?u64, 3500000000), frequencyFromMeasurement(175000000, 50, 1000));
    try std.testing.expect(frequencyFromMeasurement(1, 0, 1000) == null);
    try std.testing.expect(frequencyFromMeasurement(0, 50, 1000) == null);
    try std.testing.expect(frequencyFromMeasurement(1, 50, 0) == null);
    try std.testing.expect(frequencyFromMeasurement(std.math.maxInt(u64), 1, 1000) == null);
}

test "KVM pvclock conversion handles signed shifts and invalid snapshots" {
    try std.testing.expectEqual(@as(?u64, 123 + 500), pvNanoseconds(1000, 0x80000000, 0, 123));
    try std.testing.expectEqual(@as(?u64, 123 + 1000), pvNanoseconds(1000, 0x80000000, 1, 123));
    try std.testing.expectEqual(@as(?u64, 123 + 250), pvNanoseconds(1000, 0x80000000, -1, 123));
    try std.testing.expect(pvNanoseconds(1000, 0, 0, 0) == null);
    try std.testing.expect(pvNanoseconds(1000, 1, -128, 0) == null);
    try std.testing.expect(pvNanoseconds(std.math.maxInt(u64), std.math.maxInt(u32), 63, 0) == null);
    try std.testing.expect(pvNanoseconds(1000, 0x80000000, 0, std.math.maxInt(u64)) == null);
    try std.testing.expectEqual(@as(usize, 32), @sizeOf(PvTime));
}

test "KVM pvclock incomplete snapshots fail without unbounded retry" {
    defer pv_time = .{};
    pv_time = .{ .version = 1 };
    try std.testing.expect(readPvCounter() == null);
    pv_time = .{};
    try std.testing.expect(readPvCounter() == null);
}
