const mmio = @import("memory/mmio.zig");
const std = @import("std");
const realtime_clock = @import("realtime_clock.zig");

const ia32_apic_base_msr: u32 = 0x1B;
const apic_enable_bit: u64 = 1 << 11;
const x2apic_enable_bit: u64 = 1 << 10;
const apic_base_mask: u64 = 0xFFFF_F000;

const lapic_reg_eoi: u32 = 0x0B0;
const lapic_reg_id: u32 = 0x020;
const lapic_reg_isr_base: u32 = 0x100;
const lapic_reg_irr_base: u32 = 0x200;
const lapic_reg_svr: u32 = 0x0F0;
const lapic_reg_icr_low: u32 = 0x300;
const lapic_reg_icr_high: u32 = 0x310;
const lapic_reg_lvt_timer: u32 = 0x320;
const lapic_reg_initial_count: u32 = 0x380;
const lapic_reg_current_count: u32 = 0x390;
const lapic_reg_divide_config: u32 = 0x3E0;

pub const pit_frequency_hz: u64 = 1_193_182;
pub const calibration_pit_ticks: u16 = 59_659;
pub const timer_target_period_ns: u64 = 1_000_000;
// CPUs without a CPUID crystal frequency use PIT channel 2 as their
// independent boot-time reference; qualified TSC CPUs retain the short path.
// PIT mode 0 nominally completes in 50 ms. HPET is independent of both
// clocks being calibrated. A second work bound handles a stopped HPET; it
// is not a claim about wall time of a trapped port I/O instruction.
const pit_timeout_ns: u64 = 100_000_000;
const pit_clock_stall_limit: usize = 100_000;
pub const CalibrationReason = enum { ready, unavailable, no_deadline_clock, pit_not_low, timeout, clock_stalled, invalid_measurement, tsc_unqualified, arat_missing };

const PitWait = struct {
    start: u64,
    previous: u64,
    stagnant: usize = 0,
    fn check(self: *PitWait, now: u64, done: bool) ?CalibrationReason {
        if (now < self.previous or now - self.start >= pit_timeout_ns) return .timeout;
        if (now == self.previous) self.stagnant += 1 else self.stagnant = 0;
        self.previous = now;
        if (self.stagnant >= pit_clock_stall_limit) return .clock_stalled;
        return if (done) .ready else null;
    }
};

// With no HPET, mode 0's decreasing count is the deadline clock itself.
// The PIT's known frequency bounds the progressing window to ~50 ms; stalled
// reads have a separate work bound, not a wall-time claim about port I/O.
const PitCounterWait = struct {
    previous: u16 = calibration_pit_ticks,
    observed_count: bool = false,
    stagnant: usize = 0,

    fn check(self: *PitCounterWait, count: u16, done: bool) ?CalibrationReason {
        // Read OUT after the latch: reaching zero between those reads is a
        // successful terminal count, not a timeout. OUT stays high in mode 0.
        if (done) return .ready;
        // Before the first PIT clock loads the programmed value, a latch can
        // still return zero. Bound that initialization wait like a stall.
        if (count == 0 and !self.observed_count) {
            self.stagnant += 1;
        } else {
            if (count == 0 or count > self.previous) return .timeout;
            self.observed_count = true;
            if (count == self.previous) self.stagnant += 1 else self.stagnant = 0;
            self.previous = count;
        }
        return if (self.stagnant >= pit_clock_stall_limit) .clock_stalled else null;
    }
};

fn latchPitCounter2() u16 {
    outb(0x43, 0x80); // channel 2 counter latch; preserve mode and gate
    const low = inb(0x42);
    const high = inb(0x42);
    return @as(u16, low) | (@as(u16, high) << 8);
}

fn calibrationPitLatch(comptime stopped: bool) u16 {
    // Freeze the PIT interface in diagnostic images. Merely holding gate low
    // is not a portable fault injection: some QEMU PIT models still advance
    // their latched count. Production always reads the actual channel.
    return if (stopped) calibration_pit_ticks else latchPitCounter2();
}

const icr_delivery_status: u32 = 1 << 12;
const icr_level_assert: u32 = 1 << 14;
const icr_trigger_level: u32 = 1 << 15;
const icr_delivery_init: u32 = 0x5 << 8;
const icr_delivery_startup: u32 = 0x6 << 8;

var lapic_base_pa: u64 = 0;
var registers: mmio.Borrow = undefined;
// Chosen by the BSP before AP startup, then read-only. Each AP must enable
// and verify this mode before it is made online; a mismatch fails closed.
var x2apic_mode: bool = false;
var bsp_timer_initial_count: u32 = 0;
var timer_frequency_hz: u64 = 0;

pub fn kernelStaticStorageEndAddr() usize {
    return @max(@max(@intFromPtr(&timer_frequency_hz) + @sizeOf(u64), @intFromPtr(&bsp_timer_initial_count) + @sizeOf(u32)), @max(@max(@intFromPtr(&lapic_base_pa) + @sizeOf(u64), @intFromPtr(&registers) + @sizeOf(mmio.Borrow)), @intFromPtr(&x2apic_mode) + @sizeOf(bool)));
}

pub fn usesX2Apic() bool {
    return x2apic_mode;
}

fn cpuid(leaf: u32) struct { ebx: u32, ecx: u32, edx: u32 } {
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
    return .{ .ebx = ebx, .ecx = ecx, .edx = edx };
}

fn kvmX2ApicSupported() bool {
    const features = cpuid(1).ecx;
    if (features & ((1 << 31) | (1 << 21)) != ((1 << 31) | (1 << 21))) return false;
    const signature = cpuid(0x40000000);
    // Physical x2APIC without interrupt remapping is supported by KVM.
    // Do not make that assumption for bare metal or another hypervisor.
    return signature.ebx == 0x4b4d564b and signature.ecx == 0x564b4d56 and signature.edx == 0x4d;
}

fn registerMsr(offset: u32) u32 {
    return 0x800 + (offset >> 4);
}

fn x2Icr(destination: u32, command: u32) u64 {
    return (@as(u64, destination) << 32) | command;
}

pub const TimerCalibration = struct {
    calibrated: bool,
    source: enum { none, tsc, pit } = .none,
    lower_hz: u64 = 0,
    first_width_cycles: u64 = 0,
    last_width_cycles: u64 = 0,
    elapsed_ns: u64 = 0,
    known_hz: u64 = 0,
    error_bound_ppm: u64 = 0,
    window_ns: u64 = 0,
    reason: CalibrationReason = .ready,
    frequency_hz: u64,
    initial_count: u32,
    rearm_overhead_ns: u64,
    pit_ticks: u16,
};

fn rdmsr(msr: u32) u64 {
    var low: u32 = 0;
    var high: u32 = 0;
    asm volatile ("rdmsr"
        : [low] "={eax}" (low),
          [high] "={edx}" (high),
        : [msr] "{ecx}" (msr),
    );
    return (@as(u64, high) << 32) | low;
}

fn wrmsr(msr: u32, value: u64) void {
    const low: u32 = @truncate(value & 0xFFFF_FFFF);
    const high: u32 = @truncate(value >> 32);
    asm volatile ("wrmsr"
        :
        : [msr] "{ecx}" (msr),
          [low] "{eax}" (low),
          [high] "{edx}" (high),
        : .{ .memory = true });
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

fn readRegister(offset: u32) u32 {
    if (x2apic_mode) return @truncate(rdmsr(registerMsr(offset)));
    return registers.read(u32, offset);
}

fn writeRegister(offset: u32, value: u32) void {
    if (x2apic_mode) {
        wrmsr(registerMsr(offset), value);
        return;
    }
    registers.write(u32, offset, value);
}

fn waitIcrIdle() bool {
    var spins: u32 = 0;
    while (spins < 1000000) : (spins += 1) {
        if ((readRegister(lapic_reg_icr_low) & icr_delivery_status) == 0) return true;
        asm volatile ("pause");
    }
    return (readRegister(lapic_reg_icr_low) & icr_delivery_status) == 0;
}

fn sendIcr(destination: u32, command: u32) bool {
    // The all-ones destination is architecturally reserved for broadcast,
    // never a physical per-CPU destination in either APIC mode.
    if (command & (@as(u32, 3) << 18) == 0 and destination == 0xffffffff) return false;
    if (x2apic_mode) {
        // x2APIC WRMSR is not serializing. Publish prior memory updates
        // before notifying the target CPU (Intel SDM, x2APIC ordering).
        asm volatile ("mfence; lfence" ::: .{ .memory = true });
        wrmsr(registerMsr(lapic_reg_icr_low), x2Icr(destination, command));
        return true;
    }
    // xAPIC 255 is broadcast, never a directed CPU destination.
    if (command & (@as(u32, 3) << 18) == 0 and destination > 254) return false;
    if (!waitIcrIdle()) return false;
    // Shorthand delivery ignores the high destination register.
    if (command & (@as(u32, 3) << 18) == 0)
        writeRegister(lapic_reg_icr_high, @as(u32, destination) << 24);
    writeRegister(lapic_reg_icr_low, command);
    return waitIcrIdle();
}

fn delayIpi() void {
    var spins: u32 = 0;
    while (spins < 200000) : (spins += 1) {
        asm volatile ("pause");
    }
}

pub fn maskLegacyPic() void {
    // Mask all legacy PIC IRQ lines to avoid stray IRQ while using LAPIC timer.
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

pub fn initTimer(timer_vector: u8, initial_count: u32) bool {
    maskLegacyPic();
    if (!enableLocalApic()) return false;
    writeRegister(lapic_reg_divide_config, 0x3); // divide by 16
    // Keep the LAPIC timer in one-shot mode.  The BSP rearms its 1 ms
    // timekeeping deadline in the interrupt handler, while APs arm only when
    // they actually enter a user thread.
    writeRegister(lapic_reg_lvt_timer, @as(u32, timer_vector) & 0xFF);
    writeRegister(lapic_reg_initial_count, initial_count);
    writeRegister(lapic_reg_eoi, 0);
    return true;
}

pub fn frequencyFromPitMeasurement(lapic_counts: u32, pit_ticks: u16) ?u64 {
    if (lapic_counts == 0 or pit_ticks == 0) return null;
    const numerator = @as(u128, lapic_counts) * pit_frequency_hz + pit_ticks / 2;
    const frequency = numerator / pit_ticks;
    if (frequency == 0 or frequency > std.math.maxInt(u64)) return null;
    return @intCast(frequency);
}

pub fn initialCountForPeriod(frequency_hz: u64, period_ns: u64, rearm_overhead_ns: u64) ?u32 {
    if (frequency_hz == 0 or period_ns == 0 or rearm_overhead_ns >= period_ns) return null;
    const countdown_ns = period_ns - rearm_overhead_ns;
    const numerator = @as(u128, frequency_hz) * countdown_ns + 500_000_000;
    const initial_count = numerator / 1_000_000_000;
    if (initial_count == 0 or initial_count > std.math.maxInt(u32)) return null;
    return @intCast(initial_count);
}

fn fallbackCalibration(fallback_initial_count: u32, rearm_overhead_ns: u64, reason: CalibrationReason) TimerCalibration {
    timer_frequency_hz = 0;
    bsp_timer_initial_count = fallback_initial_count;
    _ = armTimer(fallback_initial_count);
    return .{
        .calibrated = false,
        .reason = reason,
        .frequency_hz = 0,
        .initial_count = fallback_initial_count,
        .rearm_overhead_ns = rearm_overhead_ns,
        .pit_ticks = 0,
    };
}

const tsc = @import("tsc_clock.zig");
const calibration_options = if (@import("builtin").os.tag == .freestanding) @import("clock_test_options") else struct {
    const lapic_stop_pit = false;
    const lapic_reject_tsc = false;
};
const minimum_window_ns = 500_000;
const maximum_window_ns = 2_000_000;
const maximum_error_ppm = 10_000;
pub const CountSample = struct { before: u64, count: u32, after: u64 };
pub const RateBounds = struct { lower_hz: u64, upper_hz: u64, error_ppm: u64, window_ns: u64 };

fn rawRateBounds(a: CountSample, b: CountSample, known_hz: u64) ?RateBounds {
    if (known_hz == 0 or a.before > a.after or a.after >= b.before or b.before > b.after or
        a.count == 0 or b.count == 0 or b.count >= a.count) return null;
    const shortest = b.before - a.after;
    const longest = b.after - a.before;
    if (@as(u128, shortest) * 1_000_000_000 < @as(u128, known_hz) * minimum_window_ns or
        @as(u128, longest) * 1_000_000_000 > @as(u128, known_hz) * maximum_window_ns) return null;
    const delta: u64 = a.count - b.count;
    if (delta <= 1) return null;
    // Both MMIO sample instants lie inside ordered TSC brackets. Include one
    // count of endpoint phase uncertainty as well: D/minDelta is not an upper
    // bound. Programming the CEIL of the upper rate makes conversion late,
    // never early, in the same known-TSC time domain as absolute deadlines.
    const lower = @as(u128, delta - 1) * known_hz / longest;
    const upper = (@as(u128, delta + 1) * known_hz + shortest - 1) / shortest;
    if (lower == 0 or upper > std.math.maxInt(u64)) return null;
    const error_ppm = ((upper - lower) * 1_000_000 + lower - 1) / lower;
    return .{ .lower_hz = @intCast(lower), .upper_hz = @intCast(upper), .error_ppm = @intCast(error_ppm), .window_ns = tsc.cyclesNs(longest, known_hz) };
}

pub fn rateFromTsc(a: CountSample, b: CountSample, known_hz: u64) ?RateBounds {
    const rate = rawRateBounds(a, b, known_hz) orelse return null;
    return if (rate.error_ppm <= maximum_error_ppm) rate else null;
}
const MeasurementDecision = enum { accept, extend, reject };
fn measurementDecision(a: CountSample, b: CountSample, hz: u64, extended: bool) MeasurementDecision {
    const rate = rawRateBounds(a, b, hz) orelse return .reject;
    if (rate.error_ppm <= maximum_error_ppm) return .accept;
    return if (extended) .reject else .extend;
}

const TscSource = struct {
    fn read() u64 {
        return tsc.readCounter();
    }
};
fn waitCounter(comptime Source: type, start: u64, target: u64) ?u64 {
    var previous = start;
    // A stopped TSC has no independent wall clock here. Bound work instead;
    // exhaustion is terminal, even if a later sample would look plausible.
    for (0..100_000) |_| {
        const now = Source.read();
        if (now < previous) return null;
        if (now >= target) return now;
        previous = now;
        std.atomic.spinLoopHint();
    }
    return null;
}

fn countSample() CountSample {
    // Both fences and compiler memory clobbers matter for UC MMIO as well as
    // MSR-backed x2APIC reads. A one-sided RDTSC can understate the bracket.
    const before = tsc.readCounter();
    const count = readRegister(lapic_reg_current_count);
    return .{ .before = before, .count = count, .after = tsc.readCounter() };
}

pub fn calibrateTimer(fallback_initial_count: u32, rearm_overhead_ns: u64) TimerCalibration {
    const start = tsc.readCounter();
    const q = tsc.lapicTimerQualification();
    // A rejected known-frequency TSC must not bypass qualification through
    // the legacy path. Missing CPUID frequency is different: PIT measures
    // the BSP counter without claiming cross-CPU TSC synchronization.
    if (usePitCalibration(tsc.knownFrequency(), calibration_options.lapic_reject_tsc))
        return calibrateWithPit(fallback_initial_count, rearm_overhead_ns);
    var result = calibrateWithTsc(fallback_initial_count, rearm_overhead_ns, q);
    result.known_hz = q.hz;
    result.elapsed_ns = tsc.cyclesNs(tsc.readCounter() -| start, q.hz);
    return result;
}
fn usePitCalibration(known_hz: ?u64, reject_tsc: bool) bool {
    return known_hz == null and !reject_tsc;
}

const PitMeasurement = struct { frequency_hz: u64, tsc_cycles: u64 };
fn pitMeasurement(current_count: u32, counter_start: u64, counter_end: u64) ?PitMeasurement {
    if (current_count == 0 or current_count == std.math.maxInt(u32) or counter_end <= counter_start) return null;
    const hz = frequencyFromPitMeasurement(std.math.maxInt(u32) - current_count, calibration_pit_ticks) orelse return null;
    return .{ .frequency_hz = hz, .tsc_cycles = counter_end - counter_start };
}

fn calibrateWithPit(fallback_initial_count: u32, rearm_overhead_ns: u64) TimerCalibration {
    if (!tsc.onlineTimersAlwaysRunning())
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .arat_missing);
    if (lapic_base_pa == 0 or fallback_initial_count == 0)
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    const original_lvt = readRegister(lapic_reg_lvt_timer);
    if (original_lvt & (3 << 17) != 0)
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    writeRegister(lapic_reg_lvt_timer, original_lvt | (1 << 16));
    defer writeRegister(lapic_reg_lvt_timer, original_lvt);
    writeRegister(lapic_reg_divide_config, 0x3);
    const original_port = inb(0x61);
    defer outb(0x61, original_port);
    outb(0x61, original_port & ~@as(u8, 3));
    outb(0x43, 0xb0);
    outb(0x42, @truncate(calibration_pit_ticks));
    outb(0x42, @truncate(calibration_pit_ticks >> 8));
    if (inb(0x61) & 0x20 != 0)
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .pit_not_low);
    writeRegister(lapic_reg_initial_count, std.math.maxInt(u32));
    const counter_start = tsc.readCounter();
    outb(0x61, (original_port & ~@as(u8, 3)) | @as(u8, if (calibration_options.lapic_stop_pit) 0 else 1));
    var wait: PitCounterWait = .{};
    var completed = false;
    // A progressing PIT gives a 50ms reference window. Total work is also
    // bounded independently: a broken/trapped port cannot extend each small
    // decrement into another unbounded wait. No HPET health is assumed here.
    for (0..1_000_000) |_| {
        const count = calibrationPitLatch(calibration_options.lapic_stop_pit);
        const done = !calibration_options.lapic_stop_pit and inb(0x61) & 0x20 != 0;
        if (wait.check(count, done)) |reason| {
            if (reason != .ready)
                return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, reason);
            completed = true;
            break;
        }
        std.atomic.spinLoopHint();
    }
    if (!completed) return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .timeout);
    const counter_end = tsc.readCounter();
    const current_count = readRegister(lapic_reg_current_count);
    // The one-shot counter must still be running; saturation at zero loses
    // the elapsed count and must not be accepted as a low-frequency sample.
    const measured = pitMeasurement(current_count, counter_start, counter_end) orelse
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    const frequency_hz = measured.frequency_hz;
    const initial = deadlineCount(frequency_hz, 0, timer_target_period_ns) orelse
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    // Publish only a BSP elapsed-time fallback. configureCounter checks
    // invariant TSC and rebases its epoch; it does not expose an AP fast page.
    realtime_clock.configureCounter(measured.tsc_cycles, calibration_pit_ticks, pit_frequency_hz);
    timer_frequency_hz = frequency_hz;
    bsp_timer_initial_count = initial;
    _ = armTimer(initial);
    return .{ .calibrated = true, .source = .pit, .frequency_hz = frequency_hz, .initial_count = initial, .rearm_overhead_ns = rearm_overhead_ns, .pit_ticks = calibration_pit_ticks, .window_ns = @as(u64, calibration_pit_ticks) * 1_000_000_000 / pit_frequency_hz };
}

fn calibrateWithTsc(fallback_initial_count: u32, rearm_overhead_ns: u64, q: tsc.TimerQualification) TimerCalibration {
    if (q.hz == 0 or calibration_options.lapic_reject_tsc)
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .tsc_unqualified);
    if (!q.arat) return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .arat_missing);
    if (lapic_base_pa == 0 or fallback_initial_count == 0)
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    const original_lvt = readRegister(lapic_reg_lvt_timer);
    // The existing timer is one-shot/div16 on BSP and AP. One-shot stops at
    // zero, so it cannot hide a complete modulo wrap between our samples.
    if (original_lvt & (3 << 17) != 0)
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    writeRegister(lapic_reg_lvt_timer, original_lvt | (1 << 16));
    defer writeRegister(lapic_reg_lvt_timer, original_lvt);
    writeRegister(lapic_reg_divide_config, 0x3);
    const original_port = inb(0x61);
    defer outb(0x61, original_port);
    outb(0x61, original_port & ~@as(u8, 3));
    outb(0x43, 0xb0);
    outb(0x42, @truncate(calibration_pit_ticks));
    outb(0x42, @truncate(calibration_pit_ticks >> 8));
    const programmed_port = inb(0x61);
    outb(0x61, (original_port & ~@as(u8, 3)) | @as(u8, if (calibration_options.lapic_stop_pit) 0 else 1));
    const pit_first = calibrationPitLatch(calibration_options.lapic_stop_pit);
    const port_first = inb(0x61);
    // Record PIT progress once, without waiting 50ms for OUT or treating HPET
    // as a healthy reference on PC10-damaged hardware. PIT cannot veto TSC.
    defer @import("kernel_log.zig").writeBootFmt("lapic-pit: original=0x{x} programmed=0x{x} first-port=0x{x} last-port=0x{x} first={} last={} test-stop={}\n", .{ original_port, programmed_port, port_first, inb(0x61), pit_first, calibrationPitLatch(calibration_options.lapic_stop_pit), @intFromBool(calibration_options.lapic_stop_pit) });
    writeRegister(lapic_reg_initial_count, std.math.maxInt(u32));
    const first = countSample();
    const target = std.math.add(u64, first.after, @max(q.hz / 1000, 1)) catch
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    const reached = waitCounter(TscSource, first.after, target) orelse
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .clock_stalled);
    var last = countSample();
    if (last.before < reached) return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    var measured = rateFromTsc(first, last, q.hz);
    if (measurementDecision(first, last, q.hz, false) == .extend) {
        // Trapped MMIO may have a wider bracket than bare metal. One bounded
        // extension can reduce that relative uncertainty; NEVER relax the
        // error bound or the 2ms maximum admission window to make it pass.
        const extended = std.math.add(u64, first.after, @intCast(@as(u128, q.hz) * 7 / 4000)) catch 0;
        if (extended != 0) {
            const extended_reached = waitCounter(TscSource, last.after, extended) orelse
                return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .clock_stalled);
            const previous_count = last.count;
            last = countSample();
            if (last.count == 0 or last.count >= previous_count) return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
            if (last.before < extended_reached) return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
            measured = rateFromTsc(first, last, q.hz);
        }
    }
    @import("kernel_log.zig").writeBootFmt("lapic-samples: first={},{},{} last={},{},{} hz={}\n", .{ first.before, first.count, first.after, last.before, last.count, last.after, q.hz });
    const rate = measured orelse return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    const initial = deadlineCount(rate.upper_hz, 0, timer_target_period_ns).?;
    // SMM may have changed ADJUST during the window; never publish that
    // interval as a permanent rate. Later ADJUST-only changes do not alter
    // the calibrated ARAT hardware rate itself.
    if (tsc.runtimeFailure(0) != .none) return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .tsc_unqualified);
    // HPET may be absent even though CPUID provides a qualified TSC Hz.
    // Install BSP elapsed-time catch-up without touching the realtime units
    // or the already-selected cross-CPU clock conversion.
    realtime_clock.configureBootstrapCounter(q.hz);
    timer_frequency_hz = rate.upper_hz;
    bsp_timer_initial_count = initial;
    _ = armTimer(initial);
    // CPUID known Hz belongs to the clocksource. Do not retune realtime from
    // this timer or from PIT, and do not subtract a guessed rearm overhead.
    return .{ .calibrated = true, .source = .tsc, .frequency_hz = rate.upper_hz, .initial_count = initial, .rearm_overhead_ns = rearm_overhead_ns, .pit_ticks = 0, .lower_hz = rate.lower_hz, .first_width_cycles = first.after - first.before, .last_width_cycles = last.after - last.before, .error_bound_ppm = rate.error_ppm, .window_ns = rate.window_ns };
}

// Every AP uses the BSP's divide-by-16 rate. Both calibration paths check
// ARAT on every online CPU. Saturation also protects synthetic large slices.
pub fn sliceCount(initial: u32, ticks: u32) u32 {
    // A zero slice disables AP preemption; preserve that existing contract.
    // Only a nonzero count product saturates at the hardware register limit.
    return @intCast(@min(@as(u64, initial) * ticks, std.math.maxInt(u32)));
}
pub fn timerSliceCount(fallback: u32, ticks: u32) u32 {
    return sliceCount(timerInitialCount(fallback), ticks);
}

pub fn timerInitialCount(fallback_initial_count: u32) u32 {
    return if (bsp_timer_initial_count == 0) fallback_initial_count else bsp_timer_initial_count;
}

pub fn armTimer(initial_count: u32) bool {
    if (lapic_base_pa == 0 or initial_count == 0) return false;
    writeRegister(lapic_reg_initial_count, initial_count);
    return true;
}

pub fn disarmTimer() void {
    if (lapic_base_pa == 0) return;
    writeRegister(lapic_reg_initial_count, 0);
}

pub fn highResolutionReady() bool {
    return timer_frequency_hz != 0;
}

pub fn deadlineCount(frequency: u64, now: u64, deadline: u64) ?u32 {
    if (frequency == 0) return null;
    if (deadline <= now) return 1;
    const delay = deadline - now;
    // Round up: a conversion must never make a deadline fire early.
    const count = (@as(u128, delay) * frequency + 999_999_999) / 1_000_000_000;
    return @intCast(@min(@max(count, 1), std.math.maxInt(u32)));
}

pub fn armDeadline(now: u64, deadline: u64) bool {
    return armTimer(deadlineCount(timer_frequency_hz, now, deadline) orelse return false);
}

test "absolute LAPIC deadline rounds up and bounds register count" {
    try std.testing.expectEqual(@as(?u32, 2), deadlineCount(10_000_000, 100, 201));
    try std.testing.expectEqual(@as(?u32, 1), deadlineCount(10_000_000, 100, 99));
    try std.testing.expectEqual(@as(?u32, null), deadlineCount(0, 0, 1));
    try std.testing.expectEqual(@as(?u32, std.math.maxInt(u32)), deadlineCount(1_000_000_000, 0, std.math.maxInt(u64)));
}

pub fn enableLocalApic() bool {
    var apic_base = rdmsr(ia32_apic_base_msr);
    const base = apic_base & apic_base_mask;
    if (base == 0) return false;
    const supported = kvmX2ApicSupported();
    const selected = if (lapic_base_pa == 0) supported else x2apic_mode;
    if (selected and !supported) return false;
    // Never fall back by directly clearing x2APIC mode once enabled.
    if (!selected and apic_base & x2apic_enable_bit != 0) return false;
    if (lapic_base_pa != 0 and lapic_base_pa != base) return false;
    if ((apic_base & apic_enable_bit) == 0) {
        apic_base |= apic_enable_bit;
        wrmsr(ia32_apic_base_msr, apic_base);
        apic_base = rdmsr(ia32_apic_base_msr);
    }
    if (apic_base & apic_enable_bit == 0) return false;
    if (selected and apic_base & x2apic_enable_bit == 0) {
        // Disabled -> xAPIC -> x2APIC: the direct transition is illegal.
        wrmsr(ia32_apic_base_msr, apic_base | x2apic_enable_bit);
        apic_base = rdmsr(ia32_apic_base_msr);
    }
    if (apic_base & apic_enable_bit == 0 or
        (apic_base & x2apic_enable_bit != 0) != selected) return false;
    if (selected and rdmsr(registerMsr(lapic_reg_id)) >= 0xff) return false;
    if (lapic_base_pa == 0) {
        x2apic_mode = selected;
        lapic_base_pa = base;
        // The architectural APIC aperture is one aligned page selected by
        // IA32_APIC_BASE; x2APIC never dereferences this mapping.
        if (!selected) {
            if (!@import("arch/x86_64/platform.zig").mapBootMmioRange(base, 4096)) return false;
            registers = (@import("arch/x86_64/platform.zig").kernelMmioBorrow(base, 4096) orelse unreachable);
        }
    }
    writeRegister(lapic_reg_svr, 0x100 | 0xFF);
    return true;
}

pub fn localApicId() u32 {
    if (lapic_base_pa == 0 and !enableLocalApic()) return 0;
    const raw = readRegister(lapic_reg_id);
    const id = if (x2apic_mode) raw else raw >> 24;
    return id;
}

pub fn sendInitSipi(apic_id: u32, startup_vector: u8) bool {
    if (lapic_base_pa == 0 and !enableLocalApic()) return false;
    if (!sendIcr(apic_id, icr_delivery_init | icr_level_assert | icr_trigger_level)) return false;
    delayIpi();
    if (!sendIcr(apic_id, icr_delivery_startup | @as(u32, startup_vector))) return false;
    delayIpi();
    return sendIcr(apic_id, icr_delivery_startup | @as(u32, startup_vector));
}

pub fn sendFixedIpi(apic_id: u32, vector: u8) bool {
    if (lapic_base_pa == 0 and !enableLocalApic()) return false;
    return sendIcr(apic_id, vector);
}

/// Caller keeps interrupts disabled across the entire ICR transaction.
/// Use only after SMP has established that every physical CPU is online.
pub fn sendFixedIpiAllExcludingSelf(vector: u8) bool {
    if (lapic_base_pa == 0) return false;
    // Fixed, edge-triggered, all excluding self. Destination high is ignored.
    return sendIcr(0, (@as(u32, 3) << 18) | @as(u32, vector));
}

pub fn eoi() void {
    if (lapic_base_pa == 0) return;
    writeRegister(lapic_reg_eoi, 0);
}

pub fn activeInterruptVectorInRange(first: u8, count: u8) ?u8 {
    if (lapic_base_pa == 0 or count == 0) return null;
    // Device interrupt-gate entry keeps IF=0 and has not issued EOI yet.
    // The local ISR is stable here; read each word once, not once per bit.
    // This does not apply to the asynchronous IRR/drain check below.
    return activeVectorFromIsr(first, count, IsrReader{});
}

const IsrReader = struct {
    fn read(_: @This(), index: u32) u32 {
        return readRegister(lapic_reg_isr_base + index * 0x10);
    }
};

fn activeVectorFromIsr(first: u8, count: u8, reader: anytype) ?u8 {
    const end: u16 = @min(@as(u16, first) + count, 256);
    var vector: u16 = first;
    while (vector < end) {
        const word_base = vector & ~@as(u16, 31);
        const word_end = @min(word_base + 32, end);
        const low: u5 = @intCast(vector - word_base);
        const high_trim: u5 = @intCast(word_base + 32 - word_end);
        const all_bits: u32 = std.math.maxInt(u32);
        const mask = (all_bits << low) & (all_bits >> high_trim);
        const bits = reader.read(@as(u32, word_base / 32)) & mask;
        if (bits != 0) return @intCast(word_base + @ctz(bits));
        vector = word_end;
    }
    return null;
}

test "LAPIC ISR scan preserves range and reads each word once" {
    const Reader = struct {
        words: [8]u32 = @splat(0),
        reads: [8]u8 = @splat(0),
        fn read(self: *@This(), index: u32) u32 {
            self.reads[index] += 1;
            return self.words[index];
        }
    };
    const ranges = [_][2]u8{
        .{ 0, 0 },     .{ 0, 255 }, .{ 1, 255 },   .{ 31, 1 },
        .{ 31, 2 },    .{ 32, 32 }, .{ 63, 2 },    .{ 80, 128 },
        .{ 254, 255 }, .{ 255, 1 }, .{ 255, 255 },
    };
    for (ranges) |range| {
        for (0..256) |bit| {
            var reader: Reader = .{};
            reader.words[bit / 32] = @as(u32, 1) << @as(u5, @intCast(bit % 32));
            const end = @min(@as(u16, range[0]) + range[1], 256);
            const expected: ?u8 = if (bit >= range[0] and bit < end) @intCast(bit) else null;
            try std.testing.expectEqual(expected, activeVectorFromIsr(range[0], range[1], &reader));
            for (reader.reads, 0..) |reads, word| {
                const intersects = range[1] != 0 and word * 32 < end and (word + 1) * 32 > range[0];
                const visited = intersects and (expected == null or word <= bit / 32);
                try std.testing.expectEqual(@as(u8, if (visited) 1 else 0), reads);
            }
        }
    }
    var reader: Reader = .{};
    reader.words[0] = 1; // Below the range; must not shadow vector 35.
    reader.words[1] = (@as(u32, 1) << 3) | (@as(u32, 1) << 31);
    reader.words[2] = 1;
    try std.testing.expectEqual(@as(?u8, 35), activeVectorFromIsr(31, 34, &reader));
    try std.testing.expectEqual([8]u8{ 1, 1, 0, 0, 0, 0, 0, 0 }, reader.reads);
}

/// A source must already be masked and its posted mask write read back.
/// An empty ISR alone is insufficient: a pending IRR edge can be delivered
/// after a new owner has installed its handler.
pub fn interruptRangePending(first: u8, count: u8) bool {
    if (lapic_base_pa == 0 or count == 0) return false;
    const end = @as(u16, first) + count;
    var vector: u16 = first;
    while (vector < end and vector <= 255) : (vector += 1) {
        const index: u32 = @intCast(vector / 32);
        const bit = @as(u32, 1) << @as(u5, @intCast(vector & 31));
        if (((readRegister(lapic_reg_irr_base + index * 0x10) |
            readRegister(lapic_reg_isr_base + index * 0x10)) & bit) != 0) return true;
    }
    return false;
}

pub fn eoiLegacyPicMaster() void {
    outb(0x20, 0x20);
}

test "LAPIC frequency calculation reproduces the PIT measurement" {
    try std.testing.expectEqual(
        @as(?u64, 62_531_245),
        frequencyFromPitMeasurement(3_126_557, calibration_pit_ticks),
    );
}

test "LAPIC initial count subtracts measured one-shot rearm overhead" {
    try std.testing.expectEqual(
        @as(?u32, 60_208),
        initialCountForPeriod(62_531_245, timer_target_period_ns, 37_160),
    );
}

test "LAPIC calibration calculation rejects unusable inputs" {
    try std.testing.expect(frequencyFromPitMeasurement(0, calibration_pit_ticks) == null);
    try std.testing.expect(frequencyFromPitMeasurement(1, 0) == null);
    try std.testing.expect(initialCountForPeriod(0, timer_target_period_ns, 37_160) == null);
    try std.testing.expect(initialCountForPeriod(62_531_245, timer_target_period_ns, timer_target_period_ns) == null);
}

test "x2APIC register mapping and physical ICR encoding" {
    try std.testing.expectEqual(@as(u32, 0x802), registerMsr(lapic_reg_id));
    try std.testing.expectEqual(@as(u32, 0x80b), registerMsr(lapic_reg_eoi));
    try std.testing.expectEqual(@as(u32, 0x830), registerMsr(lapic_reg_icr_low));
    try std.testing.expectEqual(@as(u64, 0x7e00000040), x2Icr(0x7e, 0x40));
    try std.testing.expectEqual(@as(u64, 0xc0040), x2Icr(0, (3 << 18) | 0x40));
}

test "PIT deadline bounds missing slow and stopped responses" {
    var wait = PitWait{ .start = 0, .previous = 0 };
    var now: u64 = 0;
    while (wait.check(now, false) == null) now += 1_000_000;
    try std.testing.expectEqual(pit_timeout_ns, now);
    try std.testing.expectEqual(CalibrationReason.timeout, wait.check(now, true).?);
    wait = .{ .start = 0, .previous = 0 };
    try std.testing.expect(wait.check(49_000_000, false) == null);
    try std.testing.expectEqual(CalibrationReason.ready, wait.check(50_000_000, true).?);
    wait = .{ .start = 0, .previous = 0 };
    for (1..pit_clock_stall_limit) |_| try std.testing.expect(wait.check(0, false) == null);
    try std.testing.expectEqual(CalibrationReason.clock_stalled, wait.check(0, false).?);
}

test "PIT deadline without HPET bounds missing output stopped counter and slow response" {
    var wait: PitCounterWait = .{};
    // A not-yet-loaded counter must not reject an otherwise normal device.
    try std.testing.expect(wait.check(0, false) == null);
    try std.testing.expect(wait.check(calibration_pit_ticks, false) == null);
    var count: u16 = calibration_pit_ticks - 1;
    while (count != 0) : (count -= 1) {
        // Slow polling/progress is valid, and each decrement resets stalls.
        try std.testing.expect(wait.check(count, false) == null);
        try std.testing.expect(wait.check(count, false) == null);
    }
    try std.testing.expectEqual(CalibrationReason.ready, wait.check(0, true).?);
    wait = .{};
    try std.testing.expect(wait.check(100, false) == null);
    try std.testing.expectEqual(CalibrationReason.timeout, wait.check(0, false).?);
    wait = .{};
    try std.testing.expect(wait.check(1, false) == null);
    try std.testing.expectEqual(CalibrationReason.timeout, wait.check(65535, false).?);
    for ([_]u16{ 0, calibration_pit_ticks, 12345 }) |stopped| {
        wait = .{};
        if (stopped == 12345) try std.testing.expect(wait.check(stopped, false) == null);
        for (1..pit_clock_stall_limit) |_| try std.testing.expect(wait.check(stopped, false) == null);
        try std.testing.expectEqual(CalibrationReason.clock_stalled, wait.check(stopped, false).?);
    }
    // Terminal count can wrap before a slow reader gets scheduled again;
    // OUT is sticky high and still proves completion of the programmed count.
    wait = .{};
    try std.testing.expect(wait.check(1, false) == null);
    try std.testing.expectEqual(CalibrationReason.ready, wait.check(65535, true).?);
}

test "TSC LAPIC brackets include endpoint phase and round safely" {
    const a: CountSample = .{ .before = 0, .count = 100000, .after = 100 };
    const b: CountSample = .{ .before = 1000100, .count = 90000, .after = 1000200 };
    const r = rateFromTsc(a, b, 1_000_000_000).?;
    try std.testing.expect(r.lower_hz <= 10_000_000 and r.upper_hz >= 10_000_000);
    try std.testing.expect(r.error_ppm > 0 and r.error_ppm <= maximum_error_ppm);
    try std.testing.expect(@as(u128, deadlineCount(r.upper_hz, 0, 16_000_000).?) * 1_000_000_000 >= @as(u128, r.upper_hz) * 16_000_000);
    try std.testing.expectEqual(@as(u32, std.math.maxInt(u32)), sliceCount(std.math.maxInt(u32), 4));
    try std.testing.expectEqual(@as(u32, 0), sliceCount(0, 0));
    try std.testing.expectEqual(@as(u32, 0), sliceCount(50000, 0));
}
test "TSC LAPIC rejects wraps stops bad windows and excessive uncertainty" {
    const a: CountSample = .{ .before = 0, .count = 100000, .after = 100 };
    const good: CountSample = .{ .before = 1000100, .count = 90000, .after = 1000200 };
    for (0..9) |bad| {
        var b = good;
        switch (bad) {
            0 => b.count = 100001,
            1 => b.count = 100000,
            2 => b.count = 0,
            3 => b.before = 99,
            4 => b.after = b.before - 1,
            5 => {
                b.before = 200;
                b.after = 300;
            },
            6 => {
                b.before = 3000000;
                b.after = 3000100;
            },
            7 => {
                b.after = 1800000;
            },
            else => b.count = 99999,
        }
        try std.testing.expect(rateFromTsc(a, b, 1_000_000_000) == null);
    }
    try std.testing.expect(rateFromTsc(a, good, 0) == null);
    try std.testing.expect(rateFromTsc(.{ .before = 101, .after = 100, .count = 100000 }, good, 1_000_000_000) == null);
    try std.testing.expectEqual(@as(?u32, 1), deadlineCount(1, 123, 0));
    try std.testing.expectEqual(@as(?u32, 1), deadlineCount(std.math.maxInt(u64), 123, 0));
    try std.testing.expectEqual(@as(?u32, std.math.maxInt(u32)), deadlineCount(std.math.maxInt(u64), 0, std.math.maxInt(u64)));
}

test "TSC rate bounds constrain endpoint phase exact window and ppm boundaries" {
    const a: CountSample = .{ .before = 0, .after = 0, .count = 100000 };
    var b: CountSample = .{ .before = 1000000, .after = 1000000, .count = 90000 };
    const r = rateFromTsc(a, b, 1_000_000_000).?;
    try std.testing.expectEqual(@as(u64, 10001000), r.upper_hz);
    try std.testing.expectEqual(@as(u64, 9999000), r.lower_hz);
    for ([_]u64{ 500000, 2000000 }) |window| {
        b.before = window;
        b.after = window;
        try std.testing.expect(rateFromTsc(a, b, 1_000_000_000) != null);
    }
    for ([_]u64{ 499999, 2000001 }) |window| {
        b.before = window;
        b.after = window;
        try std.testing.expect(rateFromTsc(a, b, 1_000_000_000) == null);
    }
    b = .{ .before = 1000000, .after = 1000000, .count = 99799 };
    try std.testing.expectEqual(@as(u64, 10000), rateFromTsc(a, b, 1_000_000_000).?.error_ppm);
    b.count = 99800;
    try std.testing.expect(rateFromTsc(a, b, 1_000_000_000) == null);
}

test "only uncertainty can extend once and a later sample cannot hide a backwards TSC" {
    const a: CountSample = .{ .before = 0, .after = 100, .count = 100000 };
    var b: CountSample = .{ .before = 1000100, .after = 1020000, .count = 90000 };
    try std.testing.expectEqual(MeasurementDecision.extend, measurementDecision(a, b, 1_000_000_000, false));
    try std.testing.expectEqual(MeasurementDecision.reject, measurementDecision(a, b, 1_000_000_000, true));
    b = .{ .before = 1750100, .after = 1750200, .count = 82500 };
    try std.testing.expectEqual(MeasurementDecision.accept, measurementDecision(a, b, 1_000_000_000, true));
    for (0..4) |bad| {
        var broken = b;
        switch (bad) {
            0 => broken.after = broken.before - 1,
            1 => broken.before = 99,
            2 => broken.count = 100000,
            else => broken.count = 0,
        }
        try std.testing.expectEqual(MeasurementDecision.reject, measurementDecision(a, broken, 1_000_000_000, false));
    }
}

test "counter wait cannot accept a reverse then recovery or exhaustion before extended target" {
    const Fake = struct {
        var calls: usize = 0;
        var reverse: bool = false;
        fn read() u64 {
            calls += 1;
            return if (reverse) (if (calls == 1) 150 else if (calls == 2) 149 else 300) else 150;
        }
    };
    Fake.calls = 0;
    Fake.reverse = true;
    try std.testing.expect(waitCounter(Fake, 100, 200) == null);
    try std.testing.expectEqual(@as(usize, 2), Fake.calls);
    Fake.calls = 0;
    Fake.reverse = false;
    try std.testing.expect(waitCounter(Fake, 100, 200) == null);
    try std.testing.expectEqual(@as(usize, 100_000), Fake.calls);
    try std.testing.expectEqual(@as(?u64, 150), waitCounter(Fake, 100, 150));
}

test "PIT stopped interface injection does not execute privileged port IO" {
    try std.testing.expectEqual(calibration_pit_ticks, calibrationPitLatch(true));
    try std.testing.expectEqual(calibration_pit_ticks, calibrationPitLatch(true));
}

test "PIT fallback is only selected for absent CPUID frequency" {
    try std.testing.expect(usePitCalibration(null, false));
    try std.testing.expect(!usePitCalibration(3_000_000_000, false));
    try std.testing.expect(!usePitCalibration(null, true));
    try std.testing.expect(!usePitCalibration(3_000_000_000, true));
}

test "PIT calibration rejects stopped saturated and regressing samples" {
    const top = std.math.maxInt(u32);
    try std.testing.expect(pitMeasurement(top, 10, 20) == null);
    try std.testing.expect(pitMeasurement(0, 10, 20) == null);
    try std.testing.expect(pitMeasurement(top - 100, 10, 10) == null);
    try std.testing.expect(pitMeasurement(top - 100, 20, 10) == null);
    const measured = pitMeasurement(top - 3_126_557, 100, 175_000_100).?;
    try std.testing.expectEqual(@as(u64, 175_000_000), measured.tsc_cycles);
    try std.testing.expectEqual(frequencyFromPitMeasurement(3_126_557, calibration_pit_ticks).?, measured.frequency_hz);
}
