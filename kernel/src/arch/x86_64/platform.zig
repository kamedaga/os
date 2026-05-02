const std = @import("std");
const interrupts = @import("../../interrupts.zig");

pub const max_cpus: usize = 4;
pub const page_entries: usize = 512;
pub const two_mib: u64 = 2 * 1024 * 1024;
pub const four_gib: u64 = 4 * 1024 * 1024 * 1024;
pub const one_tib: u64 = 1024 * 1024 * 1024 * 1024;
pub const pd_table_count: usize = 16;
pub const high_mmio_pml4_index: usize = 1;
pub const high_mmio_pdp_table_count: usize = page_entries;
pub const guard_page_bytes: usize = 4096;
pub const stack_region_bytes: usize = 8 * 1024 * 1024;
pub const stack_region_align: u64 = 2 * 1024 * 1024;
pub const stack_region_raw_bytes: usize = stack_region_bytes + @as(usize, @intCast(stack_region_align));
pub const stack_region_chunk_count: usize = stack_region_bytes / @as(usize, @intCast(two_mib));
pub const ring0_stack_bytes: usize = stack_region_bytes - (2 * guard_page_bytes);
pub const ist_stack_bytes: usize = 2 * 1024 * 1024;
const ap_ring0_stack_bytes: usize = 64 * 1024;
const ap_ist_stack_bytes: usize = 64 * 1024;
pub const phys_copy_window_va: u64 = (@as(u64, @intCast(high_mmio_pml4_index)) << 39);

pub const gdt_kernel_code_selector: u16 = 0x08;
pub const gdt_kernel_data_selector: u16 = 0x10;
pub const gdt_user_code_selector: u16 = 0x18;
pub const gdt_user_data_selector: u16 = 0x20;
pub const gdt_tss_selector: u16 = 0x28;
pub const gdt_sysret_user_base_selector: u16 = 0x30;
pub const gdt_sysret_user_data_selector: u16 = 0x38;
pub const gdt_sysret_user_code_selector: u16 = 0x40;

pub const page_present: u64 = 1 << 0;
pub const page_rw: u64 = 1 << 1;
pub const page_user: u64 = 1 << 2;
pub const page_ps: u64 = 1 << 7;

const GdtPtr = packed struct {
    limit: u16,
    base: u64,
};

const Tss = packed struct {
    _rsv0: u32 = 0,
    rsp0: u64 = 0,
    rsp1: u64 = 0,
    rsp2: u64 = 0,
    _rsv1: u64 = 0,
    ist1: u64 = 0,
    ist2: u64 = 0,
    ist3: u64 = 0,
    ist4: u64 = 0,
    ist5: u64 = 0,
    ist6: u64 = 0,
    ist7: u64 = 0,
    _rsv2: u64 = 0,
    _rsv3: u16 = 0,
    iomap_base: u16 = 0,
};

pub const TrapTargets = struct {
    syscall_stub: usize,
    page_fault_stub: usize,
    general_protection_stub: usize,
    double_fault_stub: usize,
    invalid_opcode_stub: usize,
    invalid_tss_stub: usize,
    segment_not_present_stub: usize,
    stack_segment_fault_stub: usize,
    timer_interrupt_stub: usize,
    lapic_timer_vector: u8,
};

var pml4_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var pd_tables: [pd_table_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** pd_table_count;
var high_mmio_pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var high_mmio_pd_tables: [high_mmio_pdp_table_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** high_mmio_pdp_table_count;
pub var phys_copy_window_pt: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var idt: [256]interrupts.IdtEntry align(16) = [_]interrupts.IdtEntry{interrupts.zeroIdtEntry()} ** 256;
const gdt_template: [9]u64 = .{
    0x0000000000000000,
    0x00AF9A000000FFFF,
    0x00AF92000000FFFF,
    0x00AFFA000000FFFF,
    0x00CFF2000000FFFF,
    0x0000000000000000,
    0x0000000000000000,
    0x00CFF2000000FFFF,
    0x00AFFA000000FFFF,
};
var gdt_tables: [max_cpus][9]u64 align(16) = [_][9]u64{gdt_template} ** max_cpus;
var ring0_stack_region_raw: [stack_region_raw_bytes]u8 align(4096) = [_]u8{0} ** stack_region_raw_bytes;
var pf_ist_stack_region_raw: [stack_region_raw_bytes]u8 align(4096) = [_]u8{0} ** stack_region_raw_bytes;
var df_ist_stack_region_raw: [stack_region_raw_bytes]u8 align(4096) = [_]u8{0} ** stack_region_raw_bytes;
var ap_ring0_stacks: [max_cpus - 1][ap_ring0_stack_bytes]u8 align(16) = [_][ap_ring0_stack_bytes]u8{[_]u8{0} ** ap_ring0_stack_bytes} ** (max_cpus - 1);
var ap_pf_ist_stacks: [max_cpus - 1][ap_ist_stack_bytes]u8 align(16) = [_][ap_ist_stack_bytes]u8{[_]u8{0} ** ap_ist_stack_bytes} ** (max_cpus - 1);
var ap_df_ist_stacks: [max_cpus - 1][ap_ist_stack_bytes]u8 align(16) = [_][ap_ist_stack_bytes]u8{[_]u8{0} ** ap_ist_stack_bytes} ** (max_cpus - 1);
var ring0_stack_guard_pt: [stack_region_chunk_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** stack_region_chunk_count;
var pf_ist_stack_guard_pt: [stack_region_chunk_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** stack_region_chunk_count;
var df_ist_stack_guard_pt: [stack_region_chunk_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** stack_region_chunk_count;
var tss_tables: [max_cpus]Tss = [_]Tss{std.mem.zeroes(Tss)} ** max_cpus;
var int80_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var pf_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var gp_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var df_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ud_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ts_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var np_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ss_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var timer_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var int80_trampoline_entry: usize = 0;
var pf_trampoline_entry: usize = 0;
var gp_trampoline_entry: usize = 0;
var df_trampoline_entry: usize = 0;
var ud_trampoline_entry: usize = 0;
var ts_trampoline_entry: usize = 0;
var np_trampoline_entry: usize = 0;
var ss_trampoline_entry: usize = 0;
var timer_trampoline_entry: usize = 0;
pub export var kernel_cr3_value: u64 = 0;
pub export var kernel_syscall_stack_top: u64 = 0;
pub export var pcid_enabled: u64 = 0;

const msr_efer: u32 = 0xC000_0080;
const msr_star: u32 = 0xC000_0081;
const msr_lstar: u32 = 0xC000_0082;
const msr_fmask: u32 = 0xC000_0084;
const msr_ia32_fs_base: u32 = 0xC000_0100;
const efer_sce: u64 = 1 << 0;
const cr4_pge: u64 = 1 << 7;
const cr4_pcide: u64 = 1 << 17;
const cpuid_leaf1_ecx_pcid: u32 = 1 << 17;
const page_addr_mask: u64 = 0x000f_ffff_ffff_f000;
const cr3_addr_mask: u64 = 0x000f_ffff_ffff_f000;
const cr3_no_flush: u64 = 1 << 63;

fn cpuid(leaf: u32) struct { eax: u32, ebx: u32, ecx: u32, edx: u32 } {
    var eax: u32 = 0;
    var ebx: u32 = 0;
    var ecx: u32 = 0;
    var edx: u32 = 0;
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

fn readCr4() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr4, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

fn writeCr4(value: u64) void {
    asm volatile ("mov %[value], %%cr4"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

pub fn enablePcidIfSupported() bool {
    const features = cpuid(1);
    if ((features.ecx & cpuid_leaf1_ecx_pcid) == 0) return false;
    if ((readCr3() & 0xfff) != 0) return false;

    var cr4 = readCr4();
    cr4 |= cr4_pge;
    writeCr4(cr4);
    cr4 |= cr4_pcide;
    writeCr4(cr4);
    pcid_enabled = 1;
    return true;
}

pub fn cr3AddressPart(value: u64) u64 {
    return value & cr3_addr_mask;
}

pub fn cr3WithUserPcid(raw_cr3: u64, pcid: u16) u64 {
    const raw = cr3AddressPart(raw_cr3);
    if (pcid_enabled == 0 or pcid == 0) return raw;
    return raw | (@as(u64, pcid) & 0xfff) | cr3_no_flush;
}

fn readMsr(msr: u32) u64 {
    var lo: u32 = 0;
    var hi: u32 = 0;
    asm volatile ("rdmsr"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
        : [msr] "{ecx}" (msr),
    );
    return (@as(u64, hi) << 32) | @as(u64, lo);
}

fn writeMsr(msr: u32, value: u64) void {
    asm volatile ("wrmsr"
        :
        : [msr] "{ecx}" (msr),
          [lo] "{eax}" (@as(u32, @truncate(value))),
          [hi] "{edx}" (@as(u32, @truncate(value >> 32))),
        : .{ .memory = true });
}

pub fn writeFsBase(value: u64) void {
    writeMsr(msr_ia32_fs_base, value);
}

fn writeU64LEBytes(ptr: [*]u8, offset: usize, value: u64) void {
    var i: usize = 0;
    while (i < 8) : (i += 1) {
        ptr[offset + i] = @intCast((value >> @intCast(i * 8)) & 0xFF);
    }
}

fn writeI32LEBytes(ptr: [*]u8, offset: usize, value: i32) void {
    const bits: u32 = @bitCast(value);
    var i: usize = 0;
    while (i < 4) : (i += 1) {
        ptr[offset + i] = @intCast((bits >> @intCast(i * 8)) & 0xFF);
    }
}

fn buildCr3SwitchTrampoline(page: *[4096]u8, target: usize) usize {
    @memset(page[0..], 0x90);
    const out: [*]u8 = @ptrCast(page);
    const scratch_offset: usize = 0x100;
    var off: usize = 0;

    const store_disp: i32 = @intCast(@as(i64, @intCast(scratch_offset)) - @as(i64, @intCast(off + 7)));
    out[off] = 0x48;
    out[off + 1] = 0x89;
    out[off + 2] = 0x05;
    writeI32LEBytes(out, off + 3, store_disp);
    off += 7;
    out[off] = 0x48;
    out[off + 1] = 0xB8;
    writeU64LEBytes(out, off + 2, kernel_cr3_value);
    off += 10;
    out[off] = 0x0F;
    out[off + 1] = 0x22;
    out[off + 2] = 0xD8;
    off += 3;
    const load_disp: i32 = @intCast(@as(i64, @intCast(scratch_offset)) - @as(i64, @intCast(off + 7)));
    out[off] = 0x48;
    out[off + 1] = 0x8B;
    out[off + 2] = 0x05;
    writeI32LEBytes(out, off + 3, load_disp);
    off += 7;
    out[off] = 0xFF;
    out[off + 1] = 0x25;
    out[off + 2] = 0x00;
    out[off + 3] = 0x00;
    out[off + 4] = 0x00;
    out[off + 5] = 0x00;
    off += 6;
    writeU64LEBytes(out, off, target);
    return @intFromPtr(page);
}

pub fn installInterruptTrampolines(targets: TrapTargets) void {
    int80_trampoline_entry = buildCr3SwitchTrampoline(&int80_trampoline_page, targets.syscall_stub);
    pf_trampoline_entry = buildCr3SwitchTrampoline(&pf_trampoline_page, targets.page_fault_stub);
    gp_trampoline_entry = buildCr3SwitchTrampoline(&gp_trampoline_page, targets.general_protection_stub);
    df_trampoline_entry = buildCr3SwitchTrampoline(&df_trampoline_page, targets.double_fault_stub);
    ud_trampoline_entry = buildCr3SwitchTrampoline(&ud_trampoline_page, targets.invalid_opcode_stub);
    ts_trampoline_entry = buildCr3SwitchTrampoline(&ts_trampoline_page, targets.invalid_tss_stub);
    np_trampoline_entry = buildCr3SwitchTrampoline(&np_trampoline_page, targets.segment_not_present_stub);
    ss_trampoline_entry = buildCr3SwitchTrampoline(&ss_trampoline_page, targets.stack_segment_fault_stub);
    timer_trampoline_entry = buildCr3SwitchTrampoline(&timer_trampoline_page, targets.timer_interrupt_stub);
    interrupts.clearIdt(&idt);
    interrupts.setIdtEntry(&idt, 6, gdt_kernel_code_selector, ud_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, 10, gdt_kernel_code_selector, ts_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, 11, gdt_kernel_code_selector, np_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, 12, gdt_kernel_code_selector, ss_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, 13, gdt_kernel_code_selector, gp_trampoline_entry, 0x8E);
    interrupts.setIdtEntryWithIst(&idt, 14, gdt_kernel_code_selector, pf_trampoline_entry, 1, 0x8E);
    interrupts.setIdtEntryWithIst(&idt, 8, gdt_kernel_code_selector, df_trampoline_entry, 2, 0x8E);
    interrupts.setIdtEntry(&idt, 0x20, gdt_kernel_code_selector, timer_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, targets.lapic_timer_vector, gdt_kernel_code_selector, timer_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, 0x80, gdt_kernel_code_selector, int80_trampoline_entry, 0xEE);
    interrupts.loadIdt(&idt);
}

pub fn installSyscallEntry(target: usize) void {
    const star = (@as(u64, gdt_sysret_user_base_selector | 0x3) << 48) |
        (@as(u64, gdt_kernel_code_selector) << 32);
    const fmask: u64 = (1 << 8) | (1 << 9) | (1 << 10) | (1 << 14) | (1 << 18);
    writeMsr(msr_star, star);
    writeMsr(msr_lstar, @as(u64, @intCast(target)));
    writeMsr(msr_fmask, fmask);
    writeMsr(msr_efer, readMsr(msr_efer) | efer_sce);
}

pub fn readCr2() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr2, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

pub fn writeCr3(value: u64) void {
    asm volatile ("mov %[value], %%cr3"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

pub fn readCr3() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr3, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

pub fn invlpg(addr: u64) void {
    asm volatile ("invlpg (%[addr])"
        :
        : [addr] "r" (addr),
        : .{ .memory = true });
}

pub fn stackTop(region: []u8, usable_bytes: usize) u64 {
    _ = usable_bytes;
    return @intFromPtr(region.ptr) + region.len - guard_page_bytes;
}

fn stackBottom(region: []u8, usable_bytes: usize) u64 {
    return stackTop(region, usable_bytes) - usable_bytes;
}

fn alignedStackRegion(raw: []u8) []u8 {
    const base = @intFromPtr(raw.ptr);
    const aligned_base = (base + (stack_region_align - 1)) & ~(stack_region_align - 1);
    const start: usize = @intCast(aligned_base - base);
    return raw[start .. start + stack_region_bytes];
}

pub fn cpuKernelStackTop(cpu_slot: usize) ?u64 {
    if (cpu_slot == 0) return stackTop(alignedStackRegion(ring0_stack_region_raw[0..]), ring0_stack_bytes);
    if (cpu_slot >= max_cpus) return null;
    const stack = &ap_ring0_stacks[cpu_slot - 1];
    return (@intFromPtr(stack) + ap_ring0_stack_bytes) & ~@as(u64, 0xF);
}

pub fn cpuSlotForStackPointer(rsp: u64) ?usize {
    const bsp_region = alignedStackRegion(ring0_stack_region_raw[0..]);
    const bsp_base = @intFromPtr(bsp_region.ptr) + guard_page_bytes;
    const bsp_end = bsp_base + ring0_stack_bytes;
    if (rsp >= bsp_base and rsp <= bsp_end) return 0;

    var cpu_slot: usize = 1;
    while (cpu_slot < max_cpus) : (cpu_slot += 1) {
        const stack = &ap_ring0_stacks[cpu_slot - 1];
        const base = @intFromPtr(stack);
        const end = base + ap_ring0_stack_bytes;
        if (rsp >= base and rsp <= end) return cpu_slot;
    }
    return null;
}

fn cpuPageFaultIstTop(cpu_slot: usize) ?u64 {
    if (cpu_slot == 0) return stackTop(alignedStackRegion(pf_ist_stack_region_raw[0..]), ist_stack_bytes);
    if (cpu_slot >= max_cpus) return null;
    const stack = &ap_pf_ist_stacks[cpu_slot - 1];
    return (@intFromPtr(stack) + ap_ist_stack_bytes) & ~@as(u64, 0xF);
}

fn cpuDoubleFaultIstTop(cpu_slot: usize) ?u64 {
    if (cpu_slot == 0) return stackTop(alignedStackRegion(df_ist_stack_region_raw[0..]), ist_stack_bytes);
    if (cpu_slot >= max_cpus) return null;
    const stack = &ap_df_ist_stacks[cpu_slot - 1];
    return (@intFromPtr(stack) + ap_ist_stack_bytes) & ~@as(u64, 0xF);
}

fn installGuardedIdentityStackRegion(region: []u8, usable_bytes: usize, pt_tables: *[stack_region_chunk_count][page_entries]u64) bool {
    if (region.len != stack_region_bytes) return false;
    if (usable_bytes == 0) return false;
    if (usable_bytes + (2 * guard_page_bytes) > region.len) return false;

    const region_base = @intFromPtr(region.ptr);
    if ((region_base & (stack_region_align - 1)) != 0) return false;

    const usable_start = stackBottom(region, usable_bytes);
    const usable_end = stackTop(region, usable_bytes);
    const pml4_index: usize = @intCast((region_base >> 39) & 0x1FF);
    const pdp_index: usize = @intCast((region_base >> 30) & 0x1FF);
    const pd_index: usize = @intCast((region_base >> 21) & 0x1FF);
    if (pml4_index != 0 or pdp_index >= pd_table_count or (pd_index + stack_region_chunk_count) > page_entries) return false;

    var chunk_index: usize = 0;
    while (chunk_index < stack_region_chunk_count) : (chunk_index += 1) {
        const chunk_base = region_base + (@as(u64, @intCast(chunk_index)) * two_mib);
        const pt = &pt_tables[chunk_index];
        @memset(pt[0..], 0);

        var page_index: usize = 0;
        while (page_index < page_entries) : (page_index += 1) {
            const page_base = chunk_base + (@as(u64, @intCast(page_index)) * 4096);
            if (page_base < usable_start or page_base >= usable_end) continue;
            pt[page_index] = page_base | page_present | page_rw;
        }

        const pt_pa = @intFromPtr(pt);
        if (pt_pa >= four_gib) return false;
        pd_tables[pdp_index][pd_index + chunk_index] = pt_pa | page_present | page_rw;
    }
    return true;
}

fn mapKernelIdentityPage(page_base: u64) bool {
    if ((page_base & 0xFFF) != 0) return false;
    const pml4_index: usize = @intCast((page_base >> 39) & 0x1FF);
    const pdp_index: usize = @intCast((page_base >> 30) & 0x1FF);
    const pd_index: usize = @intCast((page_base >> 21) & 0x1FF);
    const pt_index: usize = @intCast((page_base >> 12) & 0x1FF);
    if (pml4_index != 0 or pdp_index >= pd_table_count) return false;

    const pd_entry = &pd_tables[pdp_index][pd_index];
    if ((pd_entry.* & page_present) == 0) return false;
    if ((pd_entry.* & page_ps) != 0) return true;

    const pt: *[page_entries]u64 = @ptrFromInt(pd_entry.* & page_addr_mask);
    pt[pt_index] = page_base | page_present | page_rw;
    return true;
}

fn mapKernelIdentityRange(base: u64, bytes: usize) bool {
    if (bytes == 0) return true;
    var page = base & ~@as(u64, 0xFFF);
    const end = (base + @as(u64, @intCast(bytes)) + 0xFFF) & ~@as(u64, 0xFFF);
    while (page < end) : (page += 4096) {
        if (!mapKernelIdentityPage(page)) return false;
    }
    return true;
}

fn mapPerCpuKernelStorage() bool {
    if (!mapKernelIdentityRange(@intFromPtr(&gdt_tables), @sizeOf(@TypeOf(gdt_tables)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&tss_tables), @sizeOf(@TypeOf(tss_tables)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&ap_ring0_stacks), @sizeOf(@TypeOf(ap_ring0_stacks)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&ap_pf_ist_stacks), @sizeOf(@TypeOf(ap_pf_ist_stacks)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&ap_df_ist_stacks), @sizeOf(@TypeOf(ap_df_ist_stacks)))) return false;
    return true;
}

pub fn installIdentityPageTables0To1GiB() bool {
    @memset(pml4_table[0..], 0);
    @memset(pdp_table[0..], 0);
    var pd_idx: usize = 0;
    while (pd_idx < pd_table_count) : (pd_idx += 1) {
        @memset(pd_tables[pd_idx][0..], 0);
    }
    @memset(high_mmio_pdp_table[0..], 0);
    pd_idx = 0;
    while (pd_idx < high_mmio_pdp_table_count) : (pd_idx += 1) {
        @memset(high_mmio_pd_tables[pd_idx][0..], 0);
    }

    const pml4_pa: u64 = @intFromPtr(&pml4_table);
    const pdp_pa: u64 = @intFromPtr(&pdp_table);
    const pd0_pa: u64 = @intFromPtr(&pd_tables[0]);
    const high_pdp_pa: u64 = @intFromPtr(&high_mmio_pdp_table);
    const high_pd0_pa: u64 = @intFromPtr(&high_mmio_pd_tables[0]);
    const phys_copy_window_pt_pa: u64 = @intFromPtr(&phys_copy_window_pt);
    if (pml4_pa >= four_gib or pdp_pa >= four_gib or pd0_pa >= four_gib or high_pdp_pa >= four_gib or high_pd0_pa >= four_gib or phys_copy_window_pt_pa >= four_gib) return false;

    const kernel_table_flags = page_present | page_rw;
    const kernel_large_page_flags = page_present | page_rw | page_ps;

    pml4_table[0] = pdp_pa | kernel_table_flags;
    pd_idx = 0;
    while (pd_idx < pd_table_count) : (pd_idx += 1) {
        const pd_pa: u64 = @intFromPtr(&pd_tables[pd_idx]);
        pdp_table[pd_idx] = pd_pa | kernel_table_flags;

        var i: usize = 0;
        while (i < page_entries) : (i += 1) {
            const absolute_entry = (pd_idx * page_entries) + i;
            const base = @as(u64, absolute_entry) * two_mib;
            pd_tables[pd_idx][i] = base | kernel_large_page_flags;
        }
    }
    pml4_table[high_mmio_pml4_index] = high_pdp_pa | kernel_table_flags;
    @memset(phys_copy_window_pt[0..], 0);
    var high_pdp_idx: usize = 0;
    while (high_pdp_idx < high_mmio_pdp_table_count) : (high_pdp_idx += 1) {
        const high_pd_pa: u64 = @intFromPtr(&high_mmio_pd_tables[high_pdp_idx]);
        high_mmio_pdp_table[high_pdp_idx] = high_pd_pa | kernel_table_flags;

        const region_base = (@as(u64, @intCast(high_mmio_pml4_index)) << 39) + (@as(u64, @intCast(high_pdp_idx)) << 30);
        var i: usize = 0;
        while (i < page_entries) : (i += 1) {
            const base = region_base + (@as(u64, @intCast(i)) * two_mib);
            high_mmio_pd_tables[high_pdp_idx][i] = base | kernel_large_page_flags;
        }
    }
    high_mmio_pd_tables[0][0] = phys_copy_window_pt_pa | kernel_table_flags;

    if (!installGuardedIdentityStackRegion(alignedStackRegion(ring0_stack_region_raw[0..]), ring0_stack_bytes, &ring0_stack_guard_pt)) return false;
    if (!installGuardedIdentityStackRegion(alignedStackRegion(pf_ist_stack_region_raw[0..]), ist_stack_bytes, &pf_ist_stack_guard_pt)) return false;
    if (!installGuardedIdentityStackRegion(alignedStackRegion(df_ist_stack_region_raw[0..]), ist_stack_bytes, &df_ist_stack_guard_pt)) return false;
    if (!mapPerCpuKernelStorage()) return false;

    writeCr3(pml4_pa);
    kernel_cr3_value = pml4_pa;
    return true;
}

pub fn hardenKernelMappingsSupervisorOnly() void {
    pml4_table[0] &= ~page_user;
    pml4_table[high_mmio_pml4_index] &= ~page_user;

    var pdp_idx: usize = 0;
    while (pdp_idx < pd_table_count) : (pdp_idx += 1) {
        pdp_table[pdp_idx] &= ~page_user;
        var pd_idx: usize = 0;
        while (pd_idx < page_entries) : (pd_idx += 1) {
            pd_tables[pdp_idx][pd_idx] &= ~page_user;
        }
    }
    var high_pdp_idx: usize = 0;
    while (high_pdp_idx < high_mmio_pdp_table_count) : (high_pdp_idx += 1) {
        high_mmio_pdp_table[high_pdp_idx] &= ~page_user;
        var pd_idx2: usize = 0;
        while (pd_idx2 < page_entries) : (pd_idx2 += 1) {
            high_mmio_pd_tables[high_pdp_idx][pd_idx2] &= ~page_user;
        }
    }
}

pub fn seedUserPdWithKernelIdentity(pd: []u64) void {
    var i: usize = 0;
    while (i < page_entries and i < pd.len) : (i += 1) {
        pd[i] = pd_tables[0][i] & ~page_user;
    }
}

fn installTssDescriptor(cpu_slot: usize) bool {
    if (cpu_slot >= max_cpus) return false;
    const rsp0 = cpuKernelStackTop(cpu_slot) orelse return false;
    const ist1 = cpuPageFaultIstTop(cpu_slot) orelse return false;
    const ist2 = cpuDoubleFaultIstTop(cpu_slot) orelse return false;
    const tss = &tss_tables[cpu_slot];
    tss.* = std.mem.zeroes(Tss);
    tss.rsp0 = rsp0;
    tss.ist1 = ist1;
    tss.ist2 = ist2;
    tss.iomap_base = @sizeOf(Tss);

    const tss_base = @intFromPtr(tss);
    const tss_limit: u64 = @sizeOf(Tss) - 1;
    gdt_tables[cpu_slot][5] =
        (tss_limit & 0xFFFF) |
        ((tss_base & 0x00FF_FFFF) << 16) |
        (@as(u64, 0x89) << 40) |
        (((tss_limit >> 16) & 0xF) << 48) |
        (((tss_base >> 24) & 0xFF) << 56);
    gdt_tables[cpu_slot][6] = (tss_base >> 32) & 0xFFFF_FFFF;
    return true;
}

fn loadGdtAndReloadSegmentsForCpuInternal(cpu_slot: usize) bool {
    if (!installTssDescriptor(cpu_slot)) return false;

    const gdt_ptr = GdtPtr{
        .limit = @as(u16, @intCast(@sizeOf(@TypeOf(gdt_tables[0])) - 1)),
        .base = @intFromPtr(&gdt_tables[cpu_slot]),
    };
    asm volatile ("lgdt (%[ptr])"
        :
        : [ptr] "r" (&gdt_ptr),
        : .{ .memory = true });

    asm volatile (
        \\pushq %[kcs]
        \\pushq $1f
        \\lretq
        \\1:
        \\mov %[kds], %%ax
        \\mov %%ax, %%ds
        \\mov %%ax, %%es
        \\mov %%ax, %%ss
        :
        : [kcs] "i" (@as(u64, gdt_kernel_code_selector)),
          [kds] "i" (gdt_kernel_data_selector),
        : .{ .memory = true });
    asm volatile (
        \\mov %[tss_sel], %%ax
        \\ltr %%ax
        :
        : [tss_sel] "i" (gdt_tss_selector),
        : .{ .memory = true });
    return true;
}

pub fn loadGdtAndReloadSegmentsForCpu(cpu_slot: usize) bool {
    return loadGdtAndReloadSegmentsForCpuInternal(cpu_slot);
}

pub fn loadGdtAndReloadSegments() void {
    if (!loadGdtAndReloadSegmentsForCpu(0)) unreachable;
    kernel_syscall_stack_top = tss_tables[0].rsp0;
}

pub fn loadInterruptTableForCurrentCpu() void {
    interrupts.loadIdt(&idt);
}

pub fn ring0StackTop() u64 {
    return cpuKernelStackTop(0) orelse unreachable;
}

pub fn userCodeDescriptor() u64 {
    return gdt_tables[0][3];
}

pub fn userDataDescriptor() u64 {
    return gdt_tables[0][4];
}

pub fn gdtBase() u64 {
    return @intFromPtr(&gdt_tables[0]);
}
