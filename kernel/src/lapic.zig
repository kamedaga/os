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
// PIT mode 0 nominally completes in 50 ms. HPET is independent of both
// clocks being calibrated. A second work bound handles a stopped HPET; it
// is not a claim about wall time of a trapped port I/O instruction.
const pit_timeout_ns: u64 = 100_000_000;
const pit_clock_stall_limit: usize = 100_000;
pub const CalibrationReason = enum { ready, unavailable, no_deadline_clock, pit_not_low, timeout, clock_stalled, invalid_measurement };

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

pub fn calibrateTimer(fallback_initial_count: u32, rearm_overhead_ns: u64) TimerCalibration {
    if (lapic_base_pa == 0 or fallback_initial_count == 0) {
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    }

    // Prefer the post-SMP UC-verified HPET. When absent, use the PIT's own
    // known-frequency counter; TSC remains only a measurement target.
    const hpet = @import("hpet.zig");
    const start = hpet.monotonicNs();
    var wait = PitWait{ .start = start orelse 0, .previous = start orelse 0 };
    var pit_wait: PitCounterWait = .{};
    const original_port_b = inb(0x61);
    defer outb(0x61, original_port_b);

    // PIT channel 2 is polled, so calibration does not depend on the legacy
    // PIC or on interrupts being enabled. Mode 0 gives one 49.999916 ms window.
    outb(0x61, original_port_b & ~@as(u8, 0x03));
    outb(0x43, 0xB0); // channel 2, low/high bytes, mode 0, binary
    outb(0x42, @truncate(calibration_pit_ticks));
    outb(0x42, @truncate(calibration_pit_ticks >> 8));
    if ((inb(0x61) & 0x20) != 0) {
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    }

    writeRegister(lapic_reg_initial_count, std.math.maxInt(u32));
    const counter_start = realtime_clock.readCounter();
    outb(0x61, (original_port_b & ~@as(u8, 0x02)) | 0x01);

    while (true) {
        const outcome = if (start != null) hpet_wait: {
            const done = (inb(0x61) & 0x20) != 0;
            const now = hpet.monotonicNs() orelse
                return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .no_deadline_clock);
            break :hpet_wait wait.check(now, done);
        } else pit_counter_wait: {
            const count = latchPitCounter2();
            const done = (inb(0x61) & 0x20) != 0;
            break :pit_counter_wait pit_wait.check(count, done);
        };
        if (outcome) |reason| {
            if (reason != .ready) return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, reason);
            break;
        }
        asm volatile ("pause");
    }
    const counter_end = realtime_clock.readCounter();
    const current_count = readRegister(lapic_reg_current_count);
    const elapsed_counts = std.math.maxInt(u32) - current_count;
    const frequency_hz = frequencyFromPitMeasurement(elapsed_counts, calibration_pit_ticks) orelse {
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    };
    const initial_count = initialCountForPeriod(
        frequency_hz,
        timer_target_period_ns,
        rearm_overhead_ns,
    ) orelse {
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns, .invalid_measurement);
    };

    bsp_timer_initial_count = initial_count;
    timer_frequency_hz = frequency_hz;
    if (counter_end > counter_start)
        realtime_clock.configureCounter(counter_end - counter_start, calibration_pit_ticks, pit_frequency_hz);
    _ = armTimer(initial_count);
    return .{
        .calibrated = true,
        .frequency_hz = frequency_hz,
        .initial_count = initial_count,
        .rearm_overhead_ns = rearm_overhead_ns,
        .pit_ticks = calibration_pit_ticks,
    };
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
    const delay = if (deadline > now) deadline - now else 1;
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
