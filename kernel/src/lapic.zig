const ia32_apic_base_msr: u32 = 0x1B;
const apic_enable_bit: u64 = 1 << 11;
const x2apic_enable_bit: u64 = 1 << 10;
const apic_base_mask: u64 = 0xFFFF_F000;

const lapic_reg_eoi: u32 = 0x0B0;
const lapic_reg_svr: u32 = 0x0F0;
const lapic_reg_lvt_timer: u32 = 0x320;
const lapic_reg_initial_count: u32 = 0x380;
const lapic_reg_divide_config: u32 = 0x3E0;

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

pub fn eoi() void {
    if (lapic_base_pa == 0) return;
    mmioWrite(lapic_reg_eoi, 0);
}

pub fn eoiLegacyPicMaster() void {
    outb(0x20, 0x20);
}
