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
const PvSlot = struct {
    time: PvTime align(64) = .{},
    registered: bool = false,
    stable: bool = false,
};
var pv_slots: [@import("smp.zig").max_cpus]PvSlot = @splat(.{});
var pv_enabled: bool = false;
var highres_realtime_offset_ns: u64 = 0;
const PvMonotonic = struct {
    enabled: bool = false,
    pv_origin: u64 = 0,
    ns_origin: u64 = 0,
    last_ns: u64 = 0,
    fallback: HpetFallback = .{},
    publication_lock: u8 = 0,

    fn last(self: *const PvMonotonic) u64 {
        return @atomicLoad(u64, &self.last_ns, .monotonic);
    }

    fn tryPublishPv(self: *PvMonotonic, ns: u64) u64 {
        if (@cmpxchgStrong(u8, &self.publication_lock, 0, 1, .acquire, .monotonic) != null)
            return self.last();
        defer @atomicStore(u8, &self.publication_lock, 0, .release);
        if (self.fallback.state() != 0) return self.last();
        return publishMonotonic(&self.last_ns, ns);
    }

    fn beginFallback(self: *PvMonotonic) bool {
        if (@cmpxchgStrong(u8, &self.publication_lock, 0, 1, .acquire, .monotonic) != null)
            return false;
        defer @atomicStore(u8, &self.publication_lock, 0, .release);
        return self.fallback.begin();
    }
};
var pv_monotonic: PvMonotonic = .{};

pub fn kernelStaticStorageEndAddr() usize {
    var end = hpet.staticEnd();
    end = @max(end, @intFromPtr(&clock) + @sizeOf(Clock));
    end = @max(end, @intFromPtr(&pv_slots) + @sizeOf(@TypeOf(pv_slots)));
    end = @max(end, @intFromPtr(&pv_monotonic) + @sizeOf(PvMonotonic));
    end = @max(end, @intFromPtr(&pv_enabled) + @sizeOf(bool));
    return @max(end, @intFromPtr(&highres_realtime_offset_ns) + @sizeOf(u64));
}

// Select only after all online CPUs have registered their own conversion.
// HPET remains available for unsupported CPUs or a failed runtime snapshot.
pub fn initializeHighResolution(
    rsdp: u64,
    mmio_allowed: *const fn (u64, u64) bool,
) bool {
    if (!@import("lapic.zig").highResolutionReady() or
        !hpet.initialize(rsdp, mmio_allowed)) return false;
    const now = hpet.monotonicNs() orelse return false;
    highres_realtime_offset_ns = (unixTimeSeconds() *| 1_000_000_000) -| now;
    const online = @import("smp.zig").onlineCpuMask();
    if (online != 0 and (stableCpuMask() & online) == online) {
        if (readPvCounter(&pv_slots[0].time, true)) |pv_now| {
            // Pair the KVM clock with the existing monotonic epoch. KVM's
            // system_time is not the HPET epoch and must never be returned raw.
            pv_monotonic.pv_origin = pv_now;
            pv_monotonic.ns_origin = hpet.monotonicNs() orelse return false;
            pv_monotonic.last_ns = pv_monotonic.ns_origin;
            @atomicStore(bool, &pv_monotonic.enabled, true, .release);
        }
    }
    return true;
}

fn pvEpochNs(value: u64, pv_origin: u64, ns_origin: u64) ?u64 {
    if (value < pv_origin) return null;
    return ns_origin +| (value - pv_origin);
}

fn publishMonotonic(last: *u64, value: u64) u64 {
    return @max(value, @atomicRmw(u64, last, .Max, value, .monotonic));
}

// Switch once, publishing the two anchors together. Interrupt readers must
// not spin if they interrupt the CPU that is installing the fallback.
const HpetFallback = struct {
    phase: u8 = 0, // 0: PV, 1: installing anchors, 2: HPET
    raw_origin: u64 = 0,
    ns_origin: u64 = 0,

    fn state(self: *const HpetFallback) u8 {
        return @atomicLoad(u8, &self.phase, .acquire);
    }

    fn begin(self: *HpetFallback) bool {
        return @cmpxchgStrong(u8, &self.phase, 0, 1, .acq_rel, .acquire) == null;
    }

    fn finish(self: *HpetFallback, raw: u64, ns: u64) void {
        self.raw_origin = raw;
        self.ns_origin = ns;
        @atomicStore(u8, &self.phase, 2, .release);
    }

    fn sample(self: *const HpetFallback, raw: u64, last: u64) u64 {
        if (self.state() != 2) return last;
        return self.ns_origin +| (raw -| self.raw_origin);
    }
};

fn fallbackMonotonicNs() u64 {
    const last = @atomicLoad(u64, &pv_monotonic.last_ns, .monotonic);
    if (pv_monotonic.fallback.state() != 2) return last;
    const raw = hpet.monotonicNs() orelse return last;
    return publishMonotonic(&pv_monotonic.last_ns,
        pv_monotonic.fallback.sample(raw, last));
}

fn stablePvNs(slot: usize) ?u64 {
    if (!@atomicLoad(bool, &pv_slots[slot].registered, .acquire)) return null;
    const value = readPvCounter(&pv_slots[slot].time, true) orelse return null;
    return pvEpochNs(value, pv_monotonic.pv_origin, pv_monotonic.ns_origin);
}

pub fn monotonicNs() ?u64 {
    if (!@atomicLoad(bool, &pv_monotonic.enabled, .acquire)) return hpet.monotonicNs();
    if (pv_monotonic.fallback.state() != 0) return fallbackMonotonicNs();
    const slot = @import("smp.zig").currentCpuSlotIfKnown();
    const local = if (slot) |s| stablePvNs(s) else null;
    // PVCLOCK_TSC_STABLE_BIT guarantees matching conversion data and synced
    // TSCs across vCPUs (also used by Linux's vCPU-0 pvclock vDSO reader).
    // Initialization requires that guarantee on every online CPU.
    if (local orelse stablePvNs(0)) |ns| {
        // Serialize publication with fallback entry, not with the clock read.
        // An IRQ that interrupts publication returns the last value, never spins.
        return pv_monotonic.tryPublishPv(ns);
    }
    if (pv_monotonic.beginFallback()) {
        // HPET and PV need not advance at identical rates. Never publish raw
        // HPET into the PV floor and then wait for PV to catch up.
        const raw = hpet.monotonicNs() orelse 0;
        pv_monotonic.fallback.finish(raw,
            @atomicLoad(u64, &pv_monotonic.last_ns, .monotonic));
    }
    return fallbackMonotonicNs();
}

pub fn resolutionNs() ?u64 {
    return hpet.resolutionNs();
}

pub fn realtimeNs() ?u64 {
    return highres_realtime_offset_ns +| (monotonicNs() orelse return null);
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

fn readPvCounter(time: *const PvTime, require_stable: bool) ?u64 {
    const p: *const volatile PvTime = time;
    // KVM may replace the conversion parameters on vCPU migration. Take a
    // versioned snapshot of this CPU's slot, using this CPU's TSC.
    for (0..8) |_| {
        const version = @atomicLoad(u32, &time.version, .acquire);
        if ((version & 1) != 0) continue;
        const stamp = p.tsc_timestamp;
        const system_time = p.system_time;
        const multiplier = p.multiplier;
        const shift = p.shift;
        const flags = p.flags;
        const counter = readCounter();
        asm volatile ("lfence" ::: .{ .memory = true });
        if (version != @atomicLoad(u32, &time.version, .acquire)) continue;
        if (counter < stamp or (require_stable and (flags & 1) == 0)) return null;
        return pvNanoseconds(counter - stamp, multiplier, shift, system_time);
    }
    return null;
}

// Called on the CPU being registered, before publishing that CPU as online.
pub fn registerCpu(slot: usize, kernel_pointer_paddr: *const fn (usize) ?u64) void {
    if (slot >= pv_slots.len or @atomicLoad(bool, &pv_slots[slot].registered, .acquire)) return;
    const vendor = cpuid(0x40000000);
    if (vendor.eax >= 0x40000001 and vendor.ebx == 0x4b4d564b and
        vendor.ecx == 0x564b4d56 and vendor.edx == 0x4d and
        (cpuid(0x40000001).eax & (1 << 3)) != 0)
    {
        const local = &pv_slots[slot];
        if (kernel_pointer_paddr(@intFromPtr(&local.time))) |paddr| {
            writePvMsr(paddr | 1);
            if (readPvCounter(&local.time, false) != null) {
                local.stable = (cpuid(0x40000001).eax & (1 << 24)) != 0 and
                    readPvCounter(&local.time, true) != null;
                @atomicStore(bool, &local.registered, true, .release);
                return;
            }
            writePvMsr(0);
        }
    }
}

pub fn stableCpuMask() u64 {
    var mask: u64 = 0;
    for (&pv_slots, 0..) |*slot, index| {
        if (@atomicLoad(bool, &slot.registered, .acquire) and slot.stable)
            mask |= @as(u64, 1) << @intCast(index);
    }
    return mask;
}

pub fn usesStableKvmClock() bool {
    return @atomicLoad(bool, &pv_monotonic.enabled, .acquire) and pv_monotonic.fallback.state() == 0;
}

pub fn initialize(kernel_pointer_paddr: *const fn (usize) ?u64) void {
    registerCpu(0, kernel_pointer_paddr);
    if (@atomicLoad(bool, &pv_slots[0].registered, .acquire)) {
        const seconds = rtc.unixTimeSeconds();
        if (readPvCounter(&pv_slots[0].time, false)) |counter| {
            if (seconds != 0) {
                pv_enabled = true;
                clock.frequency_hz = 1_000_000_000;
                clock.start(seconds, counter);
                return;
            }
        }
    }
    if (clock.frequency_hz == 0) return;
    const seconds = rtc.unixTimeSeconds();
    clock.start(seconds, readCounter());
}

pub fn updateFromBootstrapTimer() void {
    if (!@atomicLoad(bool, &clock.ready, .acquire)) return;
    clock.update(if (pv_enabled) readPvCounter(&pv_slots[0].time, false) orelse return else readCounter());
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
    var time: PvTime = .{ .version = 1 };
    try std.testing.expect(readPvCounter(&time, false) == null);
    time = .{};
    try std.testing.expect(readPvCounter(&time, false) == null);
    time = .{ .version = 2, .multiplier = 1 };
    try std.testing.expect(readPvCounter(&time, true) == null);
    time.flags = 1;
    try std.testing.expect(readPvCounter(&time, true) != null);
}

test "pvclock epoch and fallback share a non-regressing monotonic domain" {
    try std.testing.expectEqual(@as(?u64, 250), pvEpochNs(1050, 1000, 200));
    try std.testing.expect(pvEpochNs(999, 1000, 200) == null);
    try std.testing.expectEqual(@as(?u64, std.math.maxInt(u64)), pvEpochNs(1050, 1000, std.math.maxInt(u64) - 1));
    var last: u64 = 200;
    try std.testing.expectEqual(@as(u64, 250), publishMonotonic(&last, 250));
    try std.testing.expectEqual(@as(u64, 250), publishMonotonic(&last, 240));
    try std.testing.expectEqual(@as(u64, 260), publishMonotonic(&last, 260));
}

test "HPET fallback rebases once without exposing a faster clock epoch" {
    var fallback: HpetFallback = .{};
    try std.testing.expect(fallback.begin());
    try std.testing.expect(!fallback.begin()); // competing CPU cannot replace anchors
    try std.testing.expectEqual(@as(u64, 1000), fallback.sample(9_000_000_000, 1000));
    fallback.finish(9_000_000_000, 1000);
    try std.testing.expectEqual(@as(u64, 1000), fallback.sample(9_000_000_000, 1000));
    try std.testing.expectEqual(@as(u64, 2000), fallback.sample(9_000_001_000, 1000));
    try std.testing.expect(!fallback.begin()); // no return to PV, no second rebase
    try std.testing.expectEqual(@as(u64, 2), fallback.state());
    try std.testing.expectEqual(@as(u64, 1000), fallback.sample(1, 1000));
    fallback.ns_origin = std.math.maxInt(u64) - 1;
    try std.testing.expectEqual(std.math.maxInt(u64), fallback.sample(9_000_000_002, 1000));
}

test "PV sample cannot publish after HPET rebase or spin in an interrupt" {
    var time: PvMonotonic = .{ .last_ns = 1000 };
    const in_flight_pv = 2000;
    try std.testing.expect(time.beginFallback());
    time.fallback.finish(9_000_000_000, time.last());
    try std.testing.expectEqual(@as(u64, 1000), time.tryPublishPv(in_flight_pv));
    try std.testing.expectEqual(@as(u64, 1000), time.last());

    time = .{ .last_ns = 1000, .publication_lock = 1 };
    try std.testing.expectEqual(@as(u64, 1000), time.tryPublishPv(in_flight_pv));
    try std.testing.expect(!time.beginFallback());
    time.publication_lock = 0;
    try std.testing.expectEqual(@as(u64, 2000), time.tryPublishPv(in_flight_pv));
    try std.testing.expect(time.beginFallback());
    time.fallback.finish(9_000_000_000, time.last());
    try std.testing.expectEqual(@as(u64, 2000), time.fallback.sample(9_000_000_000, time.last()));
}
