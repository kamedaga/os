const std = @import("std");

const ia32_apic_base_msr: u32 = 0x1B;
const apic_enable_bit: u64 = 1 << 11;
const x2apic_enable_bit: u64 = 1 << 10;
const apic_base_mask: u64 = 0xFFFF_F000;

const lapic_reg_eoi: u32 = 0x0B0;
const lapic_reg_id: u32 = 0x020;
const lapic_reg_isr_base: u32 = 0x100;
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
const pit_poll_limit: u64 = 50_000_000;

const icr_delivery_status: u32 = 1 << 12;
const icr_level_assert: u32 = 1 << 14;
const icr_trigger_level: u32 = 1 << 15;
const icr_delivery_init: u32 = 0x5 << 8;
const icr_delivery_startup: u32 = 0x6 << 8;

var lapic_base_pa: u64 = 0;
var bsp_timer_initial_count: u32 = 0;

pub const TimerCalibration = struct {
    calibrated: bool,
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

fn mmioRead(offset: u32) u32 {
    const reg: *volatile u32 = @ptrFromInt(lapic_base_pa + offset);
    return reg.*;
}

fn mmioWrite(offset: u32, value: u32) void {
    const reg: *volatile u32 = @ptrFromInt(lapic_base_pa + offset);
    reg.* = value;
}

fn waitIcrIdle() bool {
    var spins: u32 = 0;
    while ((mmioRead(lapic_reg_icr_low) & icr_delivery_status) != 0 and spins < 1000000) : (spins += 1) {
        asm volatile ("pause");
    }
    return (mmioRead(lapic_reg_icr_low) & icr_delivery_status) == 0;
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

    var apic_base = rdmsr(ia32_apic_base_msr);
    if ((apic_base & x2apic_enable_bit) != 0) return false;
    if ((apic_base & apic_enable_bit) == 0) {
        apic_base |= apic_enable_bit;
        wrmsr(ia32_apic_base_msr, apic_base);
        apic_base = rdmsr(ia32_apic_base_msr);
    }

    lapic_base_pa = apic_base & apic_base_mask;
    if (lapic_base_pa == 0) return false;

    mmioWrite(lapic_reg_svr, 0x100 | 0xFF); // software enable LAPIC
    mmioWrite(lapic_reg_divide_config, 0x3); // divide by 16
    // Keep the LAPIC timer in one-shot mode.  The BSP rearms its 1 ms
    // timekeeping deadline in the interrupt handler, while APs arm only when
    // they actually enter a user thread.
    mmioWrite(lapic_reg_lvt_timer, @as(u32, timer_vector) & 0xFF);
    mmioWrite(lapic_reg_initial_count, initial_count);
    mmioWrite(lapic_reg_eoi, 0);
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

fn fallbackCalibration(fallback_initial_count: u32, rearm_overhead_ns: u64) TimerCalibration {
    bsp_timer_initial_count = fallback_initial_count;
    _ = armTimer(fallback_initial_count);
    return .{
        .calibrated = false,
        .frequency_hz = 0,
        .initial_count = fallback_initial_count,
        .rearm_overhead_ns = rearm_overhead_ns,
        .pit_ticks = 0,
    };
}

pub fn calibrateTimer(fallback_initial_count: u32, rearm_overhead_ns: u64) TimerCalibration {
    if (lapic_base_pa == 0 or fallback_initial_count == 0) {
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns);
    }

    const original_port_b = inb(0x61);
    defer outb(0x61, original_port_b);

    // PIT channel 2 is polled, so calibration does not depend on the legacy
    // PIC or on interrupts being enabled. Mode 0 gives one 49.999916 ms window.
    outb(0x61, original_port_b & ~@as(u8, 0x03));
    outb(0x43, 0xB0); // channel 2, low/high bytes, mode 0, binary
    outb(0x42, @truncate(calibration_pit_ticks));
    outb(0x42, @truncate(calibration_pit_ticks >> 8));
    if ((inb(0x61) & 0x20) != 0) {
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns);
    }

    mmioWrite(lapic_reg_initial_count, std.math.maxInt(u32));
    outb(0x61, (original_port_b & ~@as(u8, 0x02)) | 0x01);

    var polls: u64 = 0;
    while ((inb(0x61) & 0x20) == 0 and polls < pit_poll_limit) : (polls += 1) {
        asm volatile ("pause");
    }
    if ((inb(0x61) & 0x20) == 0) {
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns);
    }

    const current_count = mmioRead(lapic_reg_current_count);
    const elapsed_counts = std.math.maxInt(u32) - current_count;
    const frequency_hz = frequencyFromPitMeasurement(elapsed_counts, calibration_pit_ticks) orelse {
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns);
    };
    const initial_count = initialCountForPeriod(
        frequency_hz,
        timer_target_period_ns,
        rearm_overhead_ns,
    ) orelse {
        return fallbackCalibration(fallback_initial_count, rearm_overhead_ns);
    };

    bsp_timer_initial_count = initial_count;
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
    mmioWrite(lapic_reg_initial_count, initial_count);
    return true;
}

pub fn disarmTimer() void {
    if (lapic_base_pa == 0) return;
    mmioWrite(lapic_reg_initial_count, 0);
}

pub fn enableLocalApic() bool {
    var apic_base = rdmsr(ia32_apic_base_msr);
    if ((apic_base & x2apic_enable_bit) != 0) return false;
    if ((apic_base & apic_enable_bit) == 0) {
        apic_base |= apic_enable_bit;
        wrmsr(ia32_apic_base_msr, apic_base);
        apic_base = rdmsr(ia32_apic_base_msr);
    }
    lapic_base_pa = apic_base & apic_base_mask;
    if (lapic_base_pa == 0) return false;
    mmioWrite(lapic_reg_svr, 0x100 | 0xFF);
    return true;
}

pub fn localApicId() u8 {
    if (lapic_base_pa == 0 and !enableLocalApic()) return 0;
    return @truncate(mmioRead(lapic_reg_id) >> 24);
}

pub fn sendInitSipi(apic_id: u8, startup_vector: u8) bool {
    if (lapic_base_pa == 0 and !enableLocalApic()) return false;
    if (!waitIcrIdle()) return false;
    mmioWrite(lapic_reg_icr_high, @as(u32, apic_id) << 24);
    mmioWrite(lapic_reg_icr_low, icr_delivery_init | icr_level_assert | icr_trigger_level);
    if (!waitIcrIdle()) return false;
    delayIpi();
    mmioWrite(lapic_reg_icr_high, @as(u32, apic_id) << 24);
    mmioWrite(lapic_reg_icr_low, icr_delivery_startup | @as(u32, startup_vector));
    if (!waitIcrIdle()) return false;
    delayIpi();
    mmioWrite(lapic_reg_icr_high, @as(u32, apic_id) << 24);
    mmioWrite(lapic_reg_icr_low, icr_delivery_startup | @as(u32, startup_vector));
    return waitIcrIdle();
}

pub fn sendFixedIpi(apic_id: u8, vector: u8) bool {
    if (lapic_base_pa == 0 and !enableLocalApic()) return false;
    if (!waitIcrIdle()) return false;
    mmioWrite(lapic_reg_icr_high, @as(u32, apic_id) << 24);
    mmioWrite(lapic_reg_icr_low, @as(u32, vector));
    return waitIcrIdle();
}

pub fn eoi() void {
    if (lapic_base_pa == 0) return;
    mmioWrite(lapic_reg_eoi, 0);
}

pub fn activeInterruptVectorInRange(first: u8, count: u8) ?u8 {
    if (lapic_base_pa == 0 or count == 0) return null;
    const first_vec: u16 = first;
    const end_vec: u16 = first_vec + @as(u16, count);
    var vec = first_vec;
    while (vec < end_vec and vec <= std.math.maxInt(u8)) : (vec += 1) {
        const reg_index: u32 = @intCast(vec / 32);
        const bit_index: u5 = @intCast(vec & 31);
        const bits = mmioRead(lapic_reg_isr_base + reg_index * 0x10);
        if ((bits & (@as(u32, 1) << bit_index)) != 0) return @intCast(vec);
    }
    return null;
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
