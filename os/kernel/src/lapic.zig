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
const lapic_reg_divide_config: u32 = 0x3E0;

const icr_delivery_status: u32 = 1 << 12;
const icr_level_assert: u32 = 1 << 14;
const icr_trigger_level: u32 = 1 << 15;
const icr_delivery_init: u32 = 0x5 << 8;
const icr_delivery_startup: u32 = 0x6 << 8;

var lapic_base_pa: u64 = 0;

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

fn mmioRead(offset: u32) u32 {
    const reg: *volatile u32 = @ptrFromInt(lapic_base_pa + offset);
    return reg.*;
}

fn mmioWrite(offset: u32, value: u32) void {
    const reg: *volatile u32 = @ptrFromInt(lapic_base_pa + offset);
    reg.* = value;
}

fn waitIcrIdle() void {
    var spins: u32 = 0;
    while ((mmioRead(lapic_reg_icr_low) & icr_delivery_status) != 0 and spins < 1000000) : (spins += 1) {
        asm volatile ("pause");
    }
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
    mmioWrite(lapic_reg_lvt_timer, (@as(u32, timer_vector) & 0xFF) | (1 << 17)); // periodic mode
    mmioWrite(lapic_reg_initial_count, initial_count);
    mmioWrite(lapic_reg_eoi, 0);
    return true;
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
    waitIcrIdle();
    mmioWrite(lapic_reg_icr_high, @as(u32, apic_id) << 24);
    mmioWrite(lapic_reg_icr_low, icr_delivery_init | icr_level_assert | icr_trigger_level);
    waitIcrIdle();
    delayIpi();
    mmioWrite(lapic_reg_icr_high, @as(u32, apic_id) << 24);
    mmioWrite(lapic_reg_icr_low, icr_delivery_startup | @as(u32, startup_vector));
    waitIcrIdle();
    delayIpi();
    mmioWrite(lapic_reg_icr_high, @as(u32, apic_id) << 24);
    mmioWrite(lapic_reg_icr_low, icr_delivery_startup | @as(u32, startup_vector));
    waitIcrIdle();
    return true;
}

pub fn sendFixedIpi(apic_id: u8, vector: u8) bool {
    if (lapic_base_pa == 0 and !enableLocalApic()) return false;
    waitIcrIdle();
    mmioWrite(lapic_reg_icr_high, @as(u32, apic_id) << 24);
    mmioWrite(lapic_reg_icr_low, @as(u32, vector));
    waitIcrIdle();
    return true;
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
