//! Boot-only, cross-CPU TSC qualification and value-only error bounds.
//! This owns no clock epoch and never adjusts a CPU's TSC or TSC_ADJUST.
const std = @import("std");
const hpet = @import("hpet.zig");

// Linux normally observes a sibling pair for 2ms (and may skip warp testing
// with matching TSC_ADJUST). Keep the test even for known-frequency CPUs:
// at least 250us AND 100 cross-CPU transitions, allowing up to 2ms to get
// those transitions. Zero backwards steps are tolerated. The shorter minimum
// bounds serial boot overhead without accepting absence of interleaving.
const sync_min_ns = 250_000;
const sync_max_ns = 2_000_000;
const sync_iterations = 1_000_000; // Broken/stalled-counter safety exit only.
const spin_limit = 2_000_000;
pub const Cpu = struct {
    eligible: bool = false,
    arat: bool = false,
    adjust_present: bool = false,
    adjust: u64 = 0,
    verified: bool = false,
    known_hz: u64 = 0,
    sync_cycles: u64 = 0,
    observation_cycles: u64 = 0,
    observation_complete: bool = false,
    switches: u32 = 0,
    warp: bool = false,
};
var bootstrap: [1]Cpu = .{.{}};
var cpus: []Cpu = &bootstrap;
// AP startup is serial; only that AP and the IRQ-disabled BSP touch this
// rendezvous. Release/acquire phases also publish the non-atomic payloads.
// Exactly two participants: BSP (0) and the one AP being started (1).
// Peterson admission prevents an unlocking CPU from barging ahead of an
// already waiting peer. Unlike an abandoned ticket, withdrawing intent at
// the observation deadline cannot strand the other CPU behind a missing owner.
// Sequential consistency supplies the store->load ordering needed to prevent
// both CPUs from missing the other's intent and entering simultaneously.
const WarpLock = struct {
    intent: [2]u32 = .{ 0, 0 },
    turn: u32 = 0,

    fn tryAcquire(self: *WarpLock, side: u1, pending: *bool) bool {
        const peer = side ^ 1;
        if (!pending.*) {
            @atomicStore(u32, &self.intent[side], 1, .seq_cst);
            @atomicStore(u32, &self.turn, peer, .seq_cst);
            pending.* = true;
        }
        // A pending request must not repeatedly offer the turn: only a NEW
        // contender yields priority. Retrying keeps its existing place.
        if (@atomicLoad(u32, &self.intent[peer], .seq_cst) != 0 and
            @atomicLoad(u32, &self.turn, .seq_cst) == peer) return false;
        pending.* = false;
        return true;
    }

    fn withdraw(self: *WarpLock, side: u1) void {
        @atomicStore(u32, &self.intent[side], 0, .seq_cst);
    }
};

const WarpHistory = struct {
    last: u64 = 0,
    last_cpu: usize = 0,
    have_sample: bool = false,
    switches: u32 = 0,
    warp: bool = false,

    // Caller owns WarpLock. Sample ordering comes from mutual exclusion,
    // never from assuming which CPU's clock ought to be ahead.
    fn record(self: *WarpHistory, cpu: usize, next: u64) bool {
        if (self.have_sample) {
            if (warped(self.last, next)) self.warp = true;
            if (self.last_cpu != cpu) self.switches += 1;
        }
        self.have_sample = true;
        self.last_cpu = cpu;
        self.last = next;
        return self.switches >= 100;
    }
};

const Rendezvous = struct {
    phase: u32 = 0,
    lock: WarpLock = .{},
    history: WarpHistory = .{},
    response: u64 = 0,
    max_skew: u64 = 0,
    failed: bool = false,
};
var sync: Rendezvous = .{};
var boot_counter: u64 = 0;
var boot_hz: u64 = 0;
var reference_sample: ?Sample = null;

pub fn staticEnd() usize {
    return @max(@intFromPtr(&bootstrap) + @sizeOf(@TypeOf(bootstrap)), @max(@intFromPtr(&cpus) + @sizeOf(@TypeOf(cpus)), @max(@intFromPtr(&sync) + @sizeOf(Rendezvous), @max(@intFromPtr(&boot_counter) + 8, @max(@intFromPtr(&boot_hz) + 8, @intFromPtr(&reference_sample) + @sizeOf(@TypeOf(reference_sample)))))));
}

pub fn initCpuStorage(count: usize) !void {
    const rows = try @import("arch/x86_64/cpu_runtime.zig").alloc(Cpu, count);
    rows[0] = cpus[0];
    cpus = rows;
}

fn cpuid(leaf: u32) struct { eax: u32, ebx: u32, ecx: u32, edx: u32 } {
    var a: u32 = undefined;
    var b: u32 = undefined;
    var c: u32 = undefined;
    var d: u32 = undefined;
    asm volatile ("cpuid"
        : [a] "={eax}" (a),
          [b] "={ebx}" (b),
          [c] "={ecx}" (c),
          [d] "={edx}" (d),
        : [leaf] "{eax}" (leaf),
          [sub] "{ecx}" (@as(u32, 0)),
    );
    return .{ .eax = a, .ebx = b, .ecx = c, .edx = d };
}

pub fn readCounter() u64 {
    var lo: u32 = undefined;
    var hi: u32 = undefined;
    asm volatile ("lfence; rdtsc; lfence"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
        :
        : .{ .memory = true });
    return (@as(u64, hi) << 32) | lo;
}

fn readAdjust() u64 {
    var lo: u32 = undefined;
    var hi: u32 = undefined;
    asm volatile ("rdmsr"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
        : [msr] "{ecx}" (@as(u32, 0x3b)),
        : .{ .memory = true });
    return (@as(u64, hi) << 32) | lo;
}

pub fn assertIrqsOff() void {
    const flags = asm volatile ("pushfq; popq %[flags]"
        : [flags] "=r" (-> u64),
    );
    if (flags & (1 << 9) != 0) @panic("TSC qualification requires IF=0");
}

pub fn captureCpu(slot: usize) void {
    assertIrqsOff();
    const max = cpuid(0).eax;
    const eligible = cpuid(1).ecx & (1 << 31) == 0 and
        cpuid(0x80000000).eax >= 0x80000007 and cpuid(0x80000007).edx & (1 << 8) != 0;
    const adjust_present = max >= 7 and cpuid(7).ebx & (1 << 1) != 0;
    cpus[slot] = .{ .arat = max >= 6 and cpuid(6).eax & (1 << 2) != 0, .eligible = eligible, .adjust_present = adjust_present, .adjust = if (eligible and adjust_present) readAdjust() else 0, .verified = slot == 0 and eligible, .known_hz = knownFrequency() orelse 0 };
}

pub const RuntimeFailure = enum { none, adjust_changed, invariance_lost };
pub fn classifyRuntime(invariant_ok: bool, boot_adjust: u64, now_adjust: u64) RuntimeFailure {
    if (!invariant_ok) return .invariance_lost;
    return if (boot_adjust == now_adjust) .none else .adjust_changed;
}
pub fn runtimeFailure(slot: usize) RuntimeFailure {
    if (slot >= cpus.len or !cpus[slot].verified) return .invariance_lost;
    const invariant_ok = cpuid(1).ecx & (1 << 31) == 0 and
        cpuid(0x80000000).eax >= 0x80000007 and cpuid(0x80000007).edx & (1 << 8) != 0 and
        (knownFrequency() orelse 0) == cpus[slot].known_hz and
        (cpuid(0).eax >= 7 and cpuid(7).ebx & (1 << 1) != 0) == cpus[slot].adjust_present;
    if (!invariant_ok) return .invariance_lost;
    const now_adjust = if (cpus[slot].adjust_present) readAdjust() else cpus[slot].adjust;
    return classifyRuntime(invariant_ok, cpus[slot].adjust, now_adjust);
}

test "runtime clock rejection depends on architectural facts, not HPET" {
    try std.testing.expectEqual(RuntimeFailure.none, classifyRuntime(true, 42, 42));
    try std.testing.expectEqual(RuntimeFailure.adjust_changed, classifyRuntime(true, 42, 43));
    try std.testing.expectEqual(RuntimeFailure.invariance_lost, classifyRuntime(false, 42, 42));
}

pub fn prepareAp() void {
    assertIrqsOff();
    // Do not clear the aggregate error or skew bound from earlier APs.
    sync.phase = 0;
    sync.lock = .{};
    sync.history = .{};
}

fn phase() u32 {
    return @atomicLoad(u32, &sync.phase, .acquire);
}
fn setPhase(value: u32) void {
    @atomicStore(u32, &sync.phase, value, .release);
}
fn waitPhase(value: u32) bool {
    for (0..spin_limit) |_| {
        const current = phase();
        if (current == value) return true;
        if (current > value) return false;
        std.atomic.spinLoopHint();
    }
    return false;
}

pub fn warped(previous: u64, next: u64) bool {
    return next < previous;
}

test "warp lock bars immediate reacquisition ahead of a waiting peer" {
    // Deterministic adversarial schedule: the loser runs while the winner
    // owns the lock; the winner releases and immediately tries again. The
    // old try-lock admits this schedule forever, regardless of CPU speed.
    var unfair: u32 = 1;
    var old: WarpHistory = .{};
    _ = old.record(0, 1);
    for (0..100) |i| {
        try std.testing.expect(@cmpxchgStrong(u32, &unfair, 0, 1, .acquire, .monotonic) != null);
        @atomicStore(u32, &unfair, 0, .release);
        try std.testing.expect(@cmpxchgStrong(u32, &unfair, 0, 1, .acquire, .monotonic) == null);
        try std.testing.expect(!old.record(0, i + 2));
    }
    try std.testing.expectEqual(@as(u32, 0), old.switches);

    // The SAME barging schedule against the production lock: a queued peer
    // must be admitted before the releasing CPU can enter again. Both keep
    // their request pending between unsuccessful attempts, as observeWarp does.
    var fair: WarpLock = .{};
    var history: WarpHistory = .{};
    var a = false;
    var b = false;
    try std.testing.expect(fair.tryAcquire(0, &a));
    _ = history.record(0, 1);
    try std.testing.expect(!fair.tryAcquire(1, &b));
    fair.withdraw(0);
    for (0..100) |i| {
        try std.testing.expect(!fair.tryAcquire(0, &a));
        try std.testing.expect(fair.tryAcquire(1, &b));
        _ = history.record(1, i * 2 + 2);
        fair.withdraw(1);
        try std.testing.expect(!fair.tryAcquire(1, &b));
        try std.testing.expect(fair.tryAcquire(0, &a));
        _ = history.record(0, i * 2 + 3);
        fair.withdraw(0);
    }
    fair.withdraw(1);
    try std.testing.expectEqual(@as(u32, 200), history.switches);
    try std.testing.expect(!history.warp);
    std.debug.print("WARP_LOCK_MODEL unfair-switches={} fair-switches={}\n", .{ old.switches, history.switches });
}

test "warp lock permits deadline withdrawal without a stranded waiter" {
    var lock: WarpLock = .{};
    var a = false;
    var b = false;
    try std.testing.expect(lock.tryAcquire(0, &a));
    try std.testing.expect(!lock.tryAcquire(1, &b));
    lock.withdraw(0);
    try std.testing.expect(!lock.tryAcquire(0, &a));
    // CPU1 reaches its observation deadline without entering its critical
    // section. CPU0 can now finish alone; no uncancellable ticket remains.
    lock.withdraw(1);
    try std.testing.expect(lock.tryAcquire(0, &a));
    lock.withdraw(0);
    b = false; // A fresh observation owns fresh local request state.
    try std.testing.expect(lock.tryAcquire(1, &b));
    lock.withdraw(1);
}

test "fair handoff still rejects a single backwards tick under the lock" {
    var lock: WarpLock = .{};
    var history: WarpHistory = .{};
    var a = false;
    var b = false;
    try std.testing.expect(lock.tryAcquire(0, &a));
    _ = history.record(0, 100);
    try std.testing.expect(!lock.tryAcquire(1, &b));
    lock.withdraw(0);
    try std.testing.expect(lock.tryAcquire(1, &b));
    _ = history.record(1, 99);
    lock.withdraw(1);
    try std.testing.expect(history.warp);
    try std.testing.expectEqual(@as(u32, 1), history.switches);
    try std.testing.expect(lock.tryAcquire(0, &a));
    _ = history.record(0, 101);
    lock.withdraw(0);
    try std.testing.expect(history.warp); // Later forward motion cannot erase it.
}

fn warpSample(cpu: usize, side: u1, pending: *bool) bool {
    if (!sync.lock.tryAcquire(side, pending)) return false;
    defer sync.lock.withdraw(side);
    // Like check_tsc_sync: a shared lock orders samples from the two CPUs.
    // Any backwards step fails, with zero tolerance; invariant alone is not
    // evidence of synchronization. LFENCE on both sides orders RDTSC too.
    // Fair admission changes who gets to sample, not the warp predicate:
    // RDTSC, comparison with the shared last value, and its replacement all
    // remain INSIDE the lock, as in check_tsc_warp. A backward tick still fails.
    return sync.history.record(cpu, readCounter());
}

const Observation = struct { cycles: u64, complete: bool };
fn observeWarp(slot: usize) Observation {
    const hz = cpus[slot].known_hz;
    const start = readCounter();
    var enough = false;
    const side: u1 = @intFromBool(slot != 0);
    var pending = false;
    defer sync.lock.withdraw(side);
    for (0..sync_iterations) |i| {
        enough = warpSample(slot, side, &pending) or enough;
        // Especially on SMT siblings, give the waiting sampler execution
        // resources on EVERY retry, not only every eighth time check.
        std.atomic.spinLoopHint();
        if (i & 7 != 0) continue;
        const now = readCounter();
        if (now < start) return .{ .cycles = 0, .complete = false };
        const elapsed = cyclesNs(now - start, hz);
        if (elapsed >= sync_max_ns) return .{ .cycles = now - start, .complete = enough };
        if (elapsed >= sync_min_ns and enough) return .{ .cycles = now - start, .complete = true };
        std.atomic.spinLoopHint();
    }
    return .{ .cycles = readCounter() -| start, .complete = false };
}

pub fn cyclesNs(cycles: u64, hz: u64) u64 {
    if (hz == 0) return 0;
    return @intCast(@min(@as(u128, cycles) * 1_000_000_000 / hz, std.math.maxInt(u64)));
}

pub fn apHandshake(slot: usize) void {
    assertIrqsOff();
    if (!pairCompatible(cpus[0], cpus[slot])) return;
    setPhase(1);
    if (!waitPhase(2)) {
        setPhase(7);
        return;
    }
    sync.response = readCounter();
    setPhase(3);
    if (!waitPhase(4)) {
        setPhase(7);
        return;
    }
    const observation = observeWarp(slot);
    cpus[slot].observation_cycles = observation.cycles;
    cpus[slot].observation_complete = observation.complete;
    setPhase(5);
    _ = waitPhase(6);
}

pub fn progressAp(slot: usize) void {
    if (phase() != 1) return;
    assertIrqsOff();
    const before = readCounter();
    setPhase(2);
    if (!waitPhase(3)) {
        sync.failed = true;
        setPhase(6);
        return;
    }
    const after = readCounter();
    const bracket_ok = after >= before and sync.response >= before and sync.response <= after;
    if (bracket_ok) sync.max_skew = @max(sync.max_skew, after - before);
    setPhase(4);
    const observation = observeWarp(0);
    const completed = waitPhase(5);
    const ok = completed and bracket_ok and observation.complete and cpus[slot].observation_complete and !sync.history.warp and sync.history.switches >= 100 and
        pairCompatible(cpus[0], cpus[slot]);
    cpus[slot].verified = ok;
    cpus[slot].sync_cycles = readCounter() -| before;
    cpus[slot].switches = sync.history.switches;
    cpus[slot].warp = sync.history.warp;
    sync.failed = sync.failed or !ok;
    setPhase(6);
}

fn pairCompatible(bsp: Cpu, ap: Cpu) bool {
    return bsp.eligible and ap.eligible and bsp.known_hz != 0 and bsp.known_hz == ap.known_hz and bsp.adjust_present == ap.adjust_present and bsp.adjust == ap.adjust;
}

pub fn logQualification() void {
    var total: u64 = 0;
    for (cpus, 0..) |cpu, slot| {
        const elapsed = cyclesNs(cpu.sync_cycles, cpu.known_hz);
        total +|= elapsed;
        @import("kernel_log.zig").writeBootFmt("clock: tsc cpu={} eligible={} verified={} adjust-present={} adjust=0x{x} known-hz={} sync-ns={} observation-ns={} switches={} warp={}\n", .{ slot, @intFromBool(cpu.eligible), @intFromBool(cpu.verified), @intFromBool(cpu.adjust_present), cpu.adjust, cpu.known_hz, elapsed, cyclesNs(cpu.observation_cycles, cpu.known_hz), cpu.switches, @intFromBool(cpu.warp) });
    }
    @import("kernel_log.zig").writeBootFmt("clock: tsc sync failed={} last-pair-warp={} last-pair-switches={} skew-bound-cycles={} total-sync-ns={} min-window-ns={} max-window-ns={}\n", .{ @intFromBool(sync.failed), @intFromBool(sync.history.warp), sync.history.switches, sync.max_skew, total, sync_min_ns, sync_max_ns });
}

test "TSC admission requires invariant nonvirtual CPUs and unchanged matching ADJUST" {
    const good: Cpu = .{ .eligible = true, .adjust_present = true, .adjust = 42, .known_hz = 2_918_400_000 };
    try std.testing.expect(pairCompatible(good, good));
    var bad = good;
    bad.eligible = false;
    try std.testing.expect(!pairCompatible(good, bad));
    bad = good;
    bad.adjust += 1;
    try std.testing.expect(!pairCompatible(good, bad));
    bad = good;
    bad.adjust_present = false;
    try std.testing.expect(!pairCompatible(good, bad));
}

pub fn allOnlineVerified(online: @import("cpu_set.zig").CpuSet) bool {
    return rowsVerified(cpus, online, sync.failed);
}

pub const TimerQualification = struct { hz: u64 = 0, arat: bool = false };
// ARAT is a per-CPU architectural promise. A BSP-only check would silently
// apply its calibrated slice to an AP timer that can stop in an idle state.
// PIT needs no cross-CPU TSC conversion, but its rate is still used to
// program every online LAPIC. Keep the ARAT evidence independent of missing
// crystal frequency or failed TSC synchronization; never infer it from BSP.
pub fn onlineTimersAlwaysRunning() bool {
    return timerRowsAlwaysRunning(cpus, @import("smp.zig").onlineCpuSet());
}

fn timerRowsAlwaysRunning(rows: []const Cpu, online: @import("cpu_set.zig").CpuSet) bool {
    if (online.count() == 0 or online.capacity != rows.len) return false;
    for (rows, 0..) |row, slot| {
        if (online.contains(slot) and !row.arat) return false;
    }
    return true;
}

pub fn lapicTimerQualification() TimerQualification {
    if (runtimeFailure(0) != .none) return .{};
    return timerRowsQualification(cpus, @import("smp.zig").onlineCpuSet(), sync.failed);
}
fn timerRowsQualification(rows: []const Cpu, online: @import("cpu_set.zig").CpuSet, failed: bool) TimerQualification {
    if (!rowsVerified(rows, online, failed)) return .{};
    var hz: u64 = 0;
    var arat = true;
    for (rows, 0..) |row, slot| {
        if (!online.contains(slot)) continue;
        if (row.known_hz == 0 or (hz != 0 and hz != row.known_hz)) return .{};
        hz = row.known_hz;
        arat = arat and row.arat;
    }
    return .{ .hz = hz, .arat = arat };
}

fn rowsVerified(rows: []const Cpu, online: @import("cpu_set.zig").CpuSet, failed: bool) bool {
    if (failed or online.count() == 0 or online.capacity != rows.len) return false;
    for (0..online.capacity) |slot| {
        if (online.contains(slot) and (!rows[slot].verified or !rows[slot].eligible)) return false;
        // An extra qualified CPU is also an unreviewed topology change.
        if (!online.contains(slot) and rows[slot].verified) return false;
    }
    return true;
}

test "TSC qualification rejects unverified added CPUs and lost membership" {
    const CpuSet = @import("cpu_set.zig").CpuSet;
    var words: [1]usize = .{0};
    const online = CpuSet.init(&words, 2);
    var rows = [_]Cpu{ .{ .eligible = true, .verified = true, .known_hz = 2_918_400_000 }, .{} };
    online.insert(0);
    try std.testing.expect(rowsVerified(&rows, online, false));
    online.insert(1);
    try std.testing.expect(!rowsVerified(&rows, online, false));
    rows[1] = rows[0];
    try std.testing.expect(rowsVerified(&rows, online, false));
    try std.testing.expect(!rowsVerified(&rows, online, true));
    online.remove(1);
    try std.testing.expect(!rowsVerified(&rows, online, false));
    rows[1] = .{};
    rows[0].eligible = false;
    try std.testing.expect(!rowsVerified(&rows, online, false));
}

pub const Sample = struct {
    before: u64,
    after: u64,
    ns: u64,
    pub fn width(self: Sample) u64 {
        return self.after - self.before;
    }
};

fn sample() ?Sample {
    const before = readCounter();
    const ns = hpet.monotonicNs() orelse return null;
    const after = readCounter();
    if (after < before) return null;
    return .{ .before = before, .after = after, .ns = ns };
}

pub const Bounds = struct { low: u64, high: u64 };
pub fn bounds(a: Sample, b: Sample, resolution: u64) ?Bounds {
    if (a.after < a.before or b.after < b.before or b.before <= a.after or b.ns <= a.ns) return null;
    const ns = b.ns - a.ns;
    const quant = @mulWithOverflow(resolution, 2);
    if (quant[1] != 0 or ns <= quant[0]) return null;
    const upper_ns = @addWithOverflow(ns, quant[0]);
    if (upper_ns[1] != 0) return null;
    const low = @as(u128, b.before - a.after) * 1_000_000_000 / upper_ns[0];
    const high = (@as(u128, b.after - a.before) * 1_000_000_000 + ns - quant[0] - 1) / (ns - quant[0]);
    if (low == 0 or high > std.math.maxInt(u64)) return null;
    return .{ .low = @intCast(low), .high = @intCast(high) };
}

pub fn frequencyFromCpuid15(denominator: u32, numerator: u32, crystal_hz: u32) ?u64 {
    if (denominator == 0 or numerator == 0 or crystal_hz == 0) return null;
    const hz = @as(u64, crystal_hz) * numerator / denominator;
    return if (hz != 0) hz else null;
}

pub fn knownFrequency() ?u64 {
    if (cpuid(0).eax < 0x15) return null;
    const leaf = cpuid(0x15);
    return frequencyFromCpuid15(leaf.eax, leaf.ebx, leaf.ecx);
}

pub fn beginBoot() void {
    boot_counter = readCounter();
    boot_hz = knownFrequency() orelse 0;
}

pub fn logBoot() void {
    @import("kernel_log.zig").writeFmt("clock: boot counter={} known-hz={}\n", .{ boot_counter, boot_hz });
}

pub fn captureReference() void {
    // HPET has completed its all-online-CPU UC proof. Use boot work that
    // already has to run as the interval; never wait or poll for calibration.
    reference_sample = sample();
}

pub const Sanity = enum { consistent, reference_mismatch, inconclusive };
pub fn bootSanity(hz: u64, a: Sample, b: Sample, resolution: u64) Sanity {
    if (hz == 0) return .inconclusive;
    const measured = bounds(a, b, resolution) orelse return .inconclusive;
    const low = @as(u128, hz) * 999_000 / 1_000_000;
    const high = (@as(u128, hz) * 1_001_000 + 999_999) / 1_000_000;
    return if (measured.low <= high and measured.high >= low) .consistent else .reference_mismatch;
}

pub const Calibration = struct { hz: u64, skew: u64 };
pub fn calibrate() ?Calibration {
    const hz = knownFrequency() orelse {
        @import("kernel_log.zig").write("clock: tsc no CPUID15 crystal frequency; keep HPET\n");
        return null;
    };
    // CPUID 15's complete architectural ratio is the authoritative nominal
    // rate (Linux TSC_KNOWN_FREQ). Its absolute error is the reference crystal's
    // tolerance, NOT HPET sampling error. CPUID does not specify that tolerance:
    // no unsupported 20ppm claim is made. A board-specified 20ppm crystal would
    // imply about 1.728 seconds/day of realtime rate error before correction.
    // Fixed-point conversion adds <2ns per 2s. PIT never overrides this rate.
    if (reference_sample) |a| {
        if (sample()) |b| {
            const check = bootSanity(hz, a, b, hpet.resolutionNs() orelse 0);
            @import("kernel_log.zig").writeBootFmt("clock: tsc boot-reference={s} nominal-hz={} interval-ns={} first-cycles={} first-width={} last-cycles={} last-width={} tolerance-ppm=1000\n", .{ @tagName(check), hz, b.ns -| a.ns, a.before, a.width(), b.before, b.width() });
            // A PC10-damaged/stopped HPET cannot veto or retune a hardware-
            // reported TSC frequency. A mismatch marks the reference suspect;
            // it is a boot diagnostic, never a runtime clocksource watchdog.
        }
    }
    return .{ .hz = hz, .skew = sync.max_skew };
}

pub fn logSelection() void {
    const now = readCounter();
    @import("kernel_log.zig").writeFmt("clock: selection counter={} boot-elapsed-ns={} known-hz={}\n", .{ now, cyclesNs(now -| boot_counter, boot_hz), boot_hz });
}

pub const Conversion = struct { multiplier: u32, shift: i8 };
pub fn conversion(hz: u64) ?Conversion {
    if (hz == 0) return null;
    // Normalize the multiplier to >= 2^31, bounding rounding to < 1/2^31
    // relative error. Rounding plus shifted-bit loss costs <2ns per 2s, below
    // a crystal ppm budget. A fixed record is never corrected live.
    for (0..64) |s| {
        const divisor = @as(u128, hz) << @as(u7, @intCast(s));
        const multiplier = (@as(u128, 1_000_000_000) << 32) / divisor;
        if (multiplier >= 0x80000000 and multiplier <= std.math.maxInt(u32))
            return .{ .multiplier = @intCast(multiplier), .shift = @intCast(s) };
        if (multiplier < 0x80000000) break;
    }
    // GHz-class TSCs require negative shifts (the common 2-5 GHz case).
    for (1..64) |s| {
        const numerator = @as(u128, 1_000_000_000) << @as(u7, @intCast(32 + s));
        const multiplier = numerator / hz;
        if (multiplier >= 0x80000000 and multiplier <= std.math.maxInt(u32))
            return .{ .multiplier = @intCast(multiplier), .shift = -@as(i8, @intCast(s)) };
        if (multiplier > std.math.maxInt(u32)) break;
    }
    return null;
}

test "CPUID15 nominal frequency is exact and missing crystal stays unsupported" {
    try std.testing.expectEqual(@as(?u64, 2_918_400_000), frequencyFromCpuid15(2, 152, 38_400_000));
    try std.testing.expect(frequencyFromCpuid15(0, 152, 38_400_000) == null);
    try std.testing.expect(frequencyFromCpuid15(2, 0, 38_400_000) == null);
    try std.testing.expect(frequencyFromCpuid15(2, 152, 0) == null);
    try std.testing.expect(frequencyFromCpuid15(std.math.maxInt(u32), 1, 1) == null);
    try std.testing.expectEqual(@as(?u64, @as(u64, std.math.maxInt(u32)) * std.math.maxInt(u32)), frequencyFromCpuid15(1, std.math.maxInt(u32), std.math.maxInt(u32)));
    try std.testing.expectEqual(@as(u64, 1_000_000_000), cyclesNs(2_918_400_000, 2_918_400_000));
    try std.testing.expectEqual(std.math.maxInt(u64), cyclesNs(std.math.maxInt(u64), 1));
    try std.testing.expectEqual(@as(u64, 0), cyclesNs(123, 0));
}

test "boot-only HPET sanity handles MMIO uncertainty and stopped reference" {
    const a: Sample = .{ .before = 0, .after = 60_000, .ns = 0 };
    var b: Sample = .{ .before = 29_184_000, .after = 29_244_000, .ns = 10_000_000 };
    try std.testing.expectEqual(Sanity.consistent, bootSanity(2_918_400_000, a, b, 53));
    b.ns = 20_000_000;
    try std.testing.expectEqual(Sanity.reference_mismatch, bootSanity(2_918_400_000, a, b, 53));
    b.ns = 0;
    try std.testing.expectEqual(Sanity.inconclusive, bootSanity(2_918_400_000, a, b, 53));
    try std.testing.expect(bounds(a, b, std.math.maxInt(u64)) == null);
}

test "TSC normalized conversion handles GHz and overflow boundaries" {
    for ([_]u64{ 1, 1_000_000, 1_000_000_000, 2_918_000_000, 5_000_000_000, std.math.maxInt(u64) }) |hz| {
        const c = conversion(hz).?;
        try std.testing.expect(c.multiplier >= 0x80000000);
        try std.testing.expect(c.shift >= -63 and c.shift <= 63);
        const delta: u64 = hz;
        const scaled: u128 = if (c.shift < 0) delta >> @as(u6, @intCast(-c.shift)) else @as(u128, delta) << @as(u7, @intCast(c.shift));
        const actual = scaled * c.multiplier >> 32;
        try std.testing.expect(actual <= 1_000_000_000 and actual + 2 >= 1_000_000_000);
    }
    try std.testing.expect(conversion(0) == null);
}

test "TSC warp has zero tolerance" {
    try std.testing.expect(warped(100, 99));
    try std.testing.expect(!warped(100, 100));
}

test "LAPIC qualification requires every online verified known-frequency ARAT CPU" {
    var words: [1]usize = .{0};
    const online = @import("cpu_set.zig").CpuSet.init(&words, 2);
    online.insert(0);
    online.insert(1);
    const good: Cpu = .{ .eligible = true, .verified = true, .arat = true, .known_hz = 2_918_400_000 };
    var rows = [_]Cpu{ good, good };
    try std.testing.expectEqual(TimerQualification{ .hz = good.known_hz, .arat = true }, timerRowsQualification(&rows, online, false));
    rows[1].arat = false;
    try std.testing.expect(!timerRowsQualification(&rows, online, false).arat);
    for (0..3) |bad| {
        rows[1] = good;
        switch (bad) {
            0 => rows[1].known_hz = 0,
            1 => rows[1].known_hz += 1,
            else => rows[1].verified = false,
        }
        try std.testing.expectEqual(@as(u64, 0), timerRowsQualification(&rows, online, false).hz);
    }
    rows[1] = good;
    try std.testing.expectEqual(@as(u64, 0), timerRowsQualification(&rows, online, true).hz);
}

test "PIT timer qualification requires online ARAT independently of TSC frequency" {
    var words: [1]usize = .{0};
    const online = @import("cpu_set.zig").CpuSet.init(&words, 2);
    var rows = [_]Cpu{ .{ .arat = true }, .{ .arat = true } };
    try std.testing.expect(!timerRowsAlwaysRunning(&rows, online));
    online.insert(0);
    online.insert(1);
    try std.testing.expect(timerRowsAlwaysRunning(&rows, online));
    try std.testing.expectEqual(@as(u64, 0), rows[0].known_hz);
    try std.testing.expect(!rows[1].verified);
    rows[1].arat = false;
    try std.testing.expect(!timerRowsAlwaysRunning(&rows, online));
    try std.testing.expect(!timerRowsAlwaysRunning(rows[0..1], online));
    online.remove(1);
    try std.testing.expect(timerRowsAlwaysRunning(&rows, online));
}
