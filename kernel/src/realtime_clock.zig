const lock_diagnostics = @import("sync/lock_diagnostics.zig");
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

// This fallback samples the TSC on the BSP only. Invariant TSC does not by
// itself prove cross-CPU synchronization, so AP clock reads still consume
// published ticks; IRQ latency must not be mistaken for elapsed time.
const BootstrapCounter = struct {
    frequency_hz: u64 = 0,
    origin: u64 = 0,
    epoch_tick: u64 = 0,
    started: bool = false,

    fn start(self: *BootstrapCounter, value: u64, starting_tick: u64) void {
        self.origin = value;
        self.epoch_tick = starting_tick;
        self.started = self.frequency_hz != 0;
    }

    fn tick(self: *const BootstrapCounter, value: u64, previous: u64) ?u64 {
        if (!self.started or self.frequency_hz == 0 or value < self.origin) return null;
        const elapsed = @as(u128, value - self.origin) * 1000 / self.frequency_hz;
        const delta: u64 = @intCast(@min(elapsed, std.math.maxInt(u64)));
        return @max(previous, self.epoch_tick +| delta);
    }
};
var bootstrap_counter: BootstrapCounter = .{};
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
// RTC/pvclock starts before MADT allocation. Only the BSP exists then;
// transfer its registration into the runtime rows before starting any AP.
var bsp_pv_slot: [1]PvSlot = @splat(.{});
var pv_slots: []PvSlot = &bsp_pv_slot;
pub fn initCpuStorage(count: usize) !void {
    const rows = try @import("arch/x86_64/cpu_runtime.zig").alloc(PvSlot, count);
    rows[0] = pv_slots[0];
    pv_slots = rows;
}
var pv_enabled: bool = false;
var highres_realtime_offset_ns: u64 = 0;

const runtime_abi = @import("kernel_abi_root").runtime_abi;

/// Kernel-written part of the read-only clock page (runtime_abi clock_page_*).
/// floor_ns is the kernel's monotonic floor itself, not a copy, so a fast
/// reader that returns max(ns, floor_ns) never undercuts a kernel result.
pub const ClockPageHeader = extern struct {
    fast: u32 = 0,
    reserved0: u32 = 0,
    pv_origin: u64 = 0,
    ns_origin: u64 = 0,
    floor_ns: u64 = 0,
    ceiling_ns: u64 = 0,
    reserved: [3]u64 = .{ 0, 0, 0 },
};

/// One whole page: user processes map it read-only, so nothing else may share
/// it. Only vCPU 0's pvclock lives here; a stable-bit reader may use it from any
/// CPU (as Linux's vDSO does), and keeping the other vCPUs' records in pv_slots
/// avoids exposing them and keeps each record on its own cache line.
const ClockPage = extern struct {
    header: ClockPageHeader align(64) = .{},
    pv0: PvTime align(64) = .{},
    unused: [4096 - 128]u8 = @splat(0),
};
var clock_page: ClockPage align(4096) = .{};

comptime {
    std.debug.assert(@sizeOf(ClockPage) == runtime_abi.clock_page_size);
    std.debug.assert(@offsetOf(ClockPageHeader, "fast") == runtime_abi.clock_page_fast_offset);
    std.debug.assert(@offsetOf(ClockPageHeader, "pv_origin") == runtime_abi.clock_page_pv_origin_offset);
    std.debug.assert(@offsetOf(ClockPageHeader, "ns_origin") == runtime_abi.clock_page_ns_origin_offset);
    std.debug.assert(@offsetOf(ClockPageHeader, "floor_ns") == runtime_abi.clock_page_floor_ns_offset);
    std.debug.assert(@offsetOf(ClockPageHeader, "ceiling_ns") == runtime_abi.clock_page_ceiling_ns_offset);
    std.debug.assert(@offsetOf(ClockPage, "pv0") == runtime_abi.clock_page_pv0_offset);
    std.debug.assert(@offsetOf(PvTime, "version") == runtime_abi.clock_page_pv_version_offset);
    std.debug.assert(@offsetOf(PvTime, "tsc_timestamp") == runtime_abi.clock_page_pv_tsc_timestamp_offset);
    std.debug.assert(@offsetOf(PvTime, "system_time") == runtime_abi.clock_page_pv_system_time_offset);
    std.debug.assert(@offsetOf(PvTime, "multiplier") == runtime_abi.clock_page_pv_multiplier_offset);
    std.debug.assert(@offsetOf(PvTime, "shift") == runtime_abi.clock_page_pv_shift_offset);
    std.debug.assert(@offsetOf(PvTime, "flags") == runtime_abi.clock_page_pv_flags_offset);
}

fn slotTime(slot: usize) *PvTime {
    return if (slot == 0) &clock_page.pv0 else &pv_slots[slot].time;
}

pub fn clockPagePaddr(kernel_pointer_paddr: *const fn (usize) ?u64) ?u64 {
    return kernel_pointer_paddr(@intFromPtr(&clock_page));
}

/// How far above the last kernel-published value a fast read may go. A fallback
/// starts at least at the ceiling, so this also bounds its one forward step.
/// One 250 Hz tick keeps CLOCK_READ refreshes rare for a busy reader.
const fast_read_window_ns: u64 = 4_000_000;

/// Fast reads rely on one invariant: ceiling_ns is written only under
/// publication_lock while the fallback phase is 0. beginFallback clears fast,
/// then reads the ceiling under that lock, so the value it sees bounds every
/// ceiling any accepted fast read observed, and it lifts the floor to it before
/// the HPET anchors are installed from the floor.
const PvMonotonic = struct {
    enabled: bool = false,
    page: *ClockPageHeader = &clock_page.header,
    fallback: HpetFallback = .{},
    publication_lock: u8 = 0,
    diagnostic_record: lock_diagnostics.Record = .{},

    fn last(self: *const PvMonotonic) u64 {
        return @atomicLoad(u64, &self.page.floor_ns, .monotonic);
    }

    fn ceiling(self: *const PvMonotonic) u64 {
        return @atomicLoad(u64, &self.page.ceiling_ns, .acquire);
    }

    fn tryPublishPv(self: *PvMonotonic, ns: u64) u64 {
        const diagnostic_if = lock_diagnostics.maskInterrupts();
        defer lock_diagnostics.restoreInterrupts(diagnostic_if);
        if (@cmpxchgStrong(u8, &self.publication_lock, 0, 1, .acquire, .monotonic) != null) {
            // Returning only the old floor could undercut a fast read already
            // returned from this same PV clock. The ceiling clamp keeps the
            // value at or below where any later fallback starts; the atomic
            // floor update needs no lock because fallback starts above it.
            return publishMonotonic(&self.page.floor_ns, @min(ns, self.ceiling()));
        }
        lock_diagnostics.acquiredTry(&self.diagnostic_record, .{ .class = .clock_publication });
        defer {
            lock_diagnostics.releasing(&self.diagnostic_record);
            @atomicStore(u8, &self.publication_lock, 0, .release);
        }
        if (self.fallback.state() != 0) return self.last();
        const published = publishMonotonic(&self.page.floor_ns, ns);
        self.raiseCeilingLocked(published);
        return published;
    }

    /// Caller holds publication_lock and has seen fallback phase 0.
    fn raiseCeilingLocked(self: *PvMonotonic, published: u64) void {
        if (published +| fast_read_window_ns / 2 <= self.page.ceiling_ns) return;
        @atomicStore(u64, &self.page.ceiling_ns, published +| fast_read_window_ns, .release);
    }

    /// Publish the epoch and open fast reads, unless a fallback already began.
    fn enable(self: *PvMonotonic, pv_origin: u64, ns_origin: u64) void {
        var diagnostic_if = lock_diagnostics.maskInterrupts();
        defer lock_diagnostics.restoreInterrupts(diagnostic_if);
        lock_diagnostics.beforeAcquire(&self.diagnostic_record, .{ .class = .clock_publication });
        while (@cmpxchgWeak(u8, &self.publication_lock, 0, 1, .acquire, .monotonic) != null) {
            lock_diagnostics.restoreInterrupts(diagnostic_if);
            std.atomic.spinLoopHint();
            diagnostic_if = lock_diagnostics.maskInterrupts();
        }
        lock_diagnostics.acquired(&self.diagnostic_record, .{ .class = .clock_publication });
        defer {
            lock_diagnostics.releasing(&self.diagnostic_record);
            @atomicStore(u8, &self.publication_lock, 0, .release);
        }
        if (self.fallback.state() != 0) return;
        self.page.pv_origin = pv_origin;
        self.page.ns_origin = ns_origin;
        _ = publishMonotonic(&self.page.floor_ns, ns_origin);
        self.raiseCeilingLocked(self.last());
        @atomicStore(bool, &self.enabled, true, .release);
        @atomicStore(u32, &self.page.fast, 1, .release);
    }

    fn beginFallback(self: *PvMonotonic) bool {
        const diagnostic_if = lock_diagnostics.maskInterrupts();
        defer lock_diagnostics.restoreInterrupts(diagnostic_if);
        if (@cmpxchgStrong(u8, &self.publication_lock, 0, 1, .acquire, .monotonic) != null)
            return false;
        lock_diagnostics.acquiredTry(&self.diagnostic_record, .{ .class = .clock_publication });
        defer {
            lock_diagnostics.releasing(&self.diagnostic_record);
            @atomicStore(u8, &self.publication_lock, 0, .release);
        }
        if (self.fallback.state() != 0) return false;
        // The sequentially consistent store is a full barrier on x86: a reader
        // that still sees fast == 1 afterwards loaded its ceiling before this
        // point, so the ceiling read below is at least that value.
        @atomicStore(u32, &self.page.fast, 0, .seq_cst);
        const ceiling_ns = @atomicLoad(u64, &self.page.ceiling_ns, .seq_cst);
        _ = publishMonotonic(&self.page.floor_ns, ceiling_ns);
        return self.fallback.begin();
    }
};
var pv_monotonic: PvMonotonic = .{};

pub fn kernelStaticStorageEndAddr() usize {
    var end = hpet.staticEnd();
    end = @max(end, @intFromPtr(&clock) + @sizeOf(Clock));
    end = @max(end, @intFromPtr(&bootstrap_counter) + @sizeOf(BootstrapCounter));
    end = @max(end, @intFromPtr(&pv_slots) + @sizeOf(@TypeOf(pv_slots)));
    end = @max(end, @intFromPtr(&bsp_pv_slot) + @sizeOf(@TypeOf(bsp_pv_slot)));
    end = @max(end, @intFromPtr(&pv_monotonic) + @sizeOf(PvMonotonic));
    end = @max(end, @intFromPtr(&clock_page) + @sizeOf(ClockPage));
    end = @max(end, @intFromPtr(&pv_enabled) + @sizeOf(bool));
    return @max(end, @intFromPtr(&highres_realtime_offset_ns) + @sizeOf(u64));
}

// Select only after all online CPUs have registered their own conversion.
// HPET remains available for unsupported CPUs or a failed runtime snapshot.
pub fn initializeHighResolution(
    rsdp: u64,
    mmio_allowed: *const fn (u64, u64) bool,
) bool {
    bootstrap_counter.start(readCounter(), @import("scheduler_connection.zig").lapic_tick_count);
    // A clocksource does not require a calibrated LAPIC clockevent. If PIT
    // calibration fails, HPET still supplies elapsed time and tick catch-up.
    if (!hpet.initialize(rsdp, mmio_allowed)) return false;
    const now = hpet.monotonicNs() orelse return false;
    highres_realtime_offset_ns = (unixTimeSeconds() *| 1_000_000_000) -| now;
    const online = @import("smp.zig").onlineCpuSet();
    if (online.count() != 0 and allOnlineClocksStable(online)) {
        if (readPvCounter(slotTime(0), true)) |pv_now| {
            // Pair the KVM clock with the existing monotonic epoch. KVM's
            // system_time is not the HPET epoch and must never be returned raw.
            pv_monotonic.enable(pv_now, hpet.monotonicNs() orelse return false);
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
    const last = pv_monotonic.last();
    if (pv_monotonic.fallback.state() != 2) return last;
    const raw = hpet.monotonicNs() orelse return last;
    return publishMonotonic(&pv_monotonic.page.floor_ns, pv_monotonic.fallback.sample(raw, last));
}

fn stablePvNs(slot: usize) ?u64 {
    if (!@atomicLoad(bool, &pv_slots[slot].registered, .acquire)) return null;
    const value = readPvCounter(slotTime(slot), true) orelse return null;
    return pvEpochNs(value, pv_monotonic.page.pv_origin, pv_monotonic.page.ns_origin);
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
        // beginFallback already lifted the floor to the fast-read ceiling.
        pv_monotonic.fallback.finish(raw, pv_monotonic.last());
    }
    return fallbackMonotonicNs();
}

pub fn resolutionNs() ?u64 {
    return hpet.resolutionNs();
}

// Both sources are published during boot and remain available thereafter.
// A caller may use this admission result until its clock_gettime completes:
// a later PV fallback retains the monotonic floor, while HPET stays mapped.
pub fn monotonicSourceReady() bool {
    return @atomicLoad(bool, &pv_monotonic.enabled, .acquire) or hpet.resolutionNs() != null;
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
    const hz = frequencyFromMeasurement(cycles, pit_ticks, pit_hz) orelse 0;
    clock.frequency_hz = hz;
    // Independent of RTC readiness and later KVM realtime setup.
    bootstrap_counter.frequency_hz = hz;
}

pub fn bootstrapFallbackTick(previous: u64) u64 {
    std.debug.assert(@import("scheduler_connection.zig").isBootstrapSchedulerCpu());
    return bootstrap_counter.tick(readCounter(), previous) orelse previous +| 1;
}

pub fn bootstrapCounterFrequencyHz() u64 {
    return bootstrap_counter.frequency_hz;
}

test "BSP fallback clock catches up missed IRQs without AP TSC assumptions" {
    var source: BootstrapCounter = .{ .frequency_hz = 3_500_000_000 };
    try std.testing.expect(source.tick(42, 0) == null);
    source.start(9000, 7);
    try std.testing.expectEqual(@as(?u64, 7), source.tick(9000, 7));
    try std.testing.expectEqual(@as(?u64, 8), source.tick(9000 + 3_500_000, 7));
    try std.testing.expectEqual(@as(?u64, 1007), source.tick(9000 + 3_500_000_000, 8));
    try std.testing.expectEqual(@as(?u64, 1007), source.tick(9000 + 3_500_000, 1007));
    try std.testing.expect(source.tick(8999, 1007) == null);
    source.start(9000, std.math.maxInt(u64) - 1);
    try std.testing.expectEqual(@as(?u64, std.math.maxInt(u64)), source.tick(9000 + 7_000_000, 0));
    source.frequency_hz = 0;
    try std.testing.expect(source.tick(9000, 0) == null);
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
        const time = slotTime(slot);
        if (kernel_pointer_paddr(@intFromPtr(time))) |paddr| {
            writePvMsr(paddr | 1);
            if (readPvCounter(time, false) != null) {
                local.stable = (cpuid(0x40000001).eax & (1 << 24)) != 0 and
                    readPvCounter(time, true) != null;
                @atomicStore(bool, &local.registered, true, .release);
                return;
            }
            writePvMsr(0);
        }
    }
}

pub fn stableCpuCount() usize {
    var count: usize = 0;
    for (pv_slots) |*slot| {
        if (@atomicLoad(bool, &slot.registered, .acquire) and slot.stable) count += 1;
    }
    return count;
}

fn allOnlineClocksStable(online: @import("cpu_set.zig").CpuSet) bool {
    for (0..online.capacity) |cpu| {
        if (!online.contains(cpu)) continue;
        if (cpu >= pv_slots.len or !@atomicLoad(bool, &pv_slots[cpu].registered, .acquire) or !pv_slots[cpu].stable) return false;
    }
    return true;
}

pub fn usesStableKvmClock() bool {
    return @atomicLoad(bool, &pv_monotonic.enabled, .acquire) and pv_monotonic.fallback.state() == 0;
}

pub fn initialize(kernel_pointer_paddr: *const fn (usize) ?u64) void {
    registerCpu(0, kernel_pointer_paddr);
    if (@atomicLoad(bool, &pv_slots[0].registered, .acquire)) {
        const seconds = rtc.unixTimeSeconds();
        if (readPvCounter(slotTime(0), false)) |counter| {
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
    clock.update(if (pv_enabled) readPvCounter(slotTime(0), false) orelse return else readCounter());
}

pub fn unixTimeSeconds() u64 {
    // Preserve the existing RTC behavior when calibration/RTC initialization
    // fails, or neither KVM pvclock nor an invariant TSC is available.
    return clock.read() orelse rtc.unixTimeSeconds();
}

// Value-only syscall readers may use published seconds without entering the
// shared CMOS index/data transaction. An absent value requires the old lock.
pub fn publishedUnixTimeSeconds() ?u64 {
    return clock.read();
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
    var header: ClockPageHeader = .{ .floor_ns = 1000, .ceiling_ns = 1000 };
    var time: PvMonotonic = .{ .page = &header };
    const in_flight_pv = 2000;
    try std.testing.expect(time.beginFallback());
    time.fallback.finish(9_000_000_000, time.last());
    try std.testing.expectEqual(@as(u64, 1000), time.tryPublishPv(in_flight_pv));
    try std.testing.expectEqual(@as(u64, 1000), time.last());

    header = .{ .floor_ns = 1000, .ceiling_ns = 1000 + fast_read_window_ns };
    time = .{ .page = &header, .publication_lock = 1 };
    try std.testing.expect(!time.beginFallback());
    time.publication_lock = 0;
    try std.testing.expectEqual(@as(u64, 2000), time.tryPublishPv(in_flight_pv));
    try std.testing.expect(time.beginFallback());
    time.fallback.finish(9_000_000_000, time.last());
    // The fallback starts at the fast-read ceiling, not at the last PV value.
    try std.testing.expectEqual(@as(u64, 1000 + fast_read_window_ns), time.fallback.sample(9_000_000_000, time.last()));
}

/// Model of the user reader (userland/kobox2_adapter/clock_page.h) for one
/// already-validated pv0 counter value: fast, ceiling and floor are loaded
/// before the value is accepted, and fast is checked again afterwards.
fn modelFastRead(page: *const ClockPageHeader, pv: u64) ?u64 {
    if (@atomicLoad(u32, &page.fast, .acquire) != 1) return null;
    const ceiling = @atomicLoad(u64, &page.ceiling_ns, .acquire);
    const floor = @atomicLoad(u64, &page.floor_ns, .acquire);
    const ns = pvEpochNs(pv, page.pv_origin, page.ns_origin) orelse return null;
    if (ns > ceiling) return null;
    const result = @max(ns, floor);
    if (@atomicLoad(u32, &page.fast, .acquire) != 1) return null;
    return result;
}

test "clock page opens fast reads only with a ceiling above the floor" {
    var header: ClockPageHeader = .{};
    var time: PvMonotonic = .{ .page = &header };
    time.enable(50_000, 1_000_000);
    try std.testing.expectEqual(@as(u32, 1), header.fast);
    try std.testing.expectEqual(@as(u64, 1_000_000), header.floor_ns);
    try std.testing.expectEqual(@as(u64, 1_000_000 + fast_read_window_ns), header.ceiling_ns);
    // pv 50_000 + 2_000 maps to ns_origin + 2_000.
    try std.testing.expectEqual(@as(?u64, 1_002_000), modelFastRead(&header, 52_000));
    // Beyond the ceiling the reader must take CLOCK_READ instead.
    try std.testing.expect(modelFastRead(&header, 50_000 + fast_read_window_ns + 1) == null);
    // Kernel publication keeps floor <= ceiling and moves the ceiling ahead.
    const published = time.tryPublishPv(1_000_000 + 3 * fast_read_window_ns / 4);
    try std.testing.expect(header.floor_ns == published and header.floor_ns < header.ceiling_ns);
    try std.testing.expectEqual(published + fast_read_window_ns, header.ceiling_ns);
}

test "contended PV publication never undercuts an earlier fast read (counterexample A)" {
    var header: ClockPageHeader = .{};
    var time: PvMonotonic = .{ .page = &header };
    time.enable(0, 1_000);
    const fast = modelFastRead(&header, 2_000).?; // floor 1_000 < fast 3_000
    try std.testing.expectEqual(@as(u64, 3_000), fast);
    time.publication_lock = 1; // another CPU publishes or enters fallback
    const later = time.tryPublishPv(3_500);
    try std.testing.expect(later >= fast);
    try std.testing.expect(header.floor_ns >= later);
    // A contended value far in the future is clamped to the ceiling, so it
    // cannot exceed where a fallback would start.
    const clamped = time.tryPublishPv(header.ceiling_ns + 99);
    try std.testing.expectEqual(header.ceiling_ns, clamped);
}

test "fallback entry never returns below an accepted fast read (counterexample B)" {
    var header: ClockPageHeader = .{};
    var time: PvMonotonic = .{ .page = &header };
    time.enable(0, 1_000);
    const fast = modelFastRead(&header, fast_read_window_ns - 1).?; // ns = ceiling - 1
    try std.testing.expect(time.beginFallback());
    // Phase 1 and HPET-unavailable readers return the floor.
    try std.testing.expectEqual(@as(u64, 1), time.fallback.state());
    try std.testing.expect(time.last() >= fast);
    try std.testing.expectEqual(@as(u32, 0), header.fast);
    try std.testing.expect(modelFastRead(&header, 1_000) == null);
    time.fallback.finish(77, time.last());
    try std.testing.expect(time.fallback.sample(77, time.last()) >= fast);
    try std.testing.expect(time.fallback.sample(78, time.last()) > fast);
}

test "fast reads cannot reopen after a fallback began (counterexample C)" {
    var header: ClockPageHeader = .{};
    var time: PvMonotonic = .{ .page = &header };
    try std.testing.expect(time.beginFallback());
    time.enable(0, 1_000);
    try std.testing.expectEqual(@as(u32, 0), header.fast);
    try std.testing.expect(!time.enabled);
    try std.testing.expect(modelFastRead(&header, 2_000) == null);
}

test "ceiling seen by a completed fast read bounds the fallback anchor" {
    var header: ClockPageHeader = .{};
    var time: PvMonotonic = .{ .page = &header };
    time.enable(0, 1_000);
    // Reader samples its ceiling, then the kernel raises it and falls back.
    const seen_ceiling = header.ceiling_ns;
    const fast = modelFastRead(&header, seen_ceiling - 1_000).?;
    _ = time.tryPublishPv(1_000 + fast_read_window_ns);
    try std.testing.expect(header.ceiling_ns >= seen_ceiling);
    try std.testing.expect(time.beginFallback());
    time.fallback.finish(5, time.last());
    try std.testing.expect(time.last() >= header.ceiling_ns);
    try std.testing.expect(time.fallback.sample(5, time.last()) >= fast);
}

test "clock page is one exclusive page holding only vCPU 0's pvclock" {
    // slotTime(0) is &clock_page.pv0 by construction; other slots stay in
    // pv_slots (not referenced here, it would pull smp.zig into this test).
    try std.testing.expectEqual(@as(usize, 0), @intFromPtr(&clock_page) % 4096);
    try std.testing.expectEqual(@as(usize, 4096), @sizeOf(@TypeOf(clock_page)));
    try std.testing.expectEqual(@as(usize, 64), @intFromPtr(&clock_page.pv0) - @intFromPtr(&clock_page));
}

test "lock admission clock publication enable blocking and runtime try" {
    if (!lock_diagnostics.enabled) return;
    var header: ClockPageHeader = .{};
    var time: PvMonotonic = .{ .page = &header };
    time.enable(1, 1);
    try std.testing.expectEqual(lock_diagnostics.order.Mode.blocking, time.diagnostic_record.mode);
    _ = time.tryPublishPv(2);
    try std.testing.expectEqual(lock_diagnostics.order.Mode.try_once, time.diagnostic_record.mode);
    try std.testing.expect(time.beginFallback());
    try std.testing.expect(!time.diagnostic_record.linked);
    lock_diagnostics.assertRootEmpty();
}
