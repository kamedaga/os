const std = @import("std");
const interrupts = @import("../../interrupts.zig");
const image_range = @import("../../boot/image_range.zig");
const xstate_format = @import("xstate.zig");
const physical_layout = @import("physical_layout.zig");
const addresses = @import("../../memory/addresses.zig");
const direct_map = @import("direct_map.zig");
const boot_direct_map = @import("boot_direct_map.zig");
const cpu_stacks = @import("cpu_stack_layout.zig");

const cpu_runtime = @import("cpu_runtime.zig");
pub const page_entries: usize = 512;
pub const two_mib: u64 = 2 * 1024 * 1024;
pub const four_gib: u64 = 4 * 1024 * 1024 * 1024;
pub const one_tib: u64 = 1024 * 1024 * 1024 * 1024;
pub const high_mmio_pml4_index: usize = 510;
pub const high_mmio_pdp_table_count: usize = 1;
pub const guard_page_bytes: usize = 4096;
pub const stack_region_align: u64 = 2 * 1024 * 1024;
// Preserve the linked stack regions and ABI-independent stack identities.
// Reserve the smallest whole
// regions containing each usable stack and both guards, plus alignment slack.
pub const ring0_stack_bytes: usize = 1024 * 1024;
pub const pf_ist_stack_bytes: usize = 256 * 1024;
pub const df_ist_stack_bytes: usize = 2 * 1024 * 1024;
const bsp_stack_bytes = [_]usize{ ring0_stack_bytes, pf_ist_stack_bytes, df_ist_stack_bytes };
const small_stack_region_bytes: usize = 2 * 1024 * 1024;
const df_stack_region_bytes: usize = 4 * 1024 * 1024;
const small_stack_raw_bytes = small_stack_region_bytes + stack_region_align;
const df_stack_raw_bytes = df_stack_region_bytes + stack_region_align;

pub const phys_copy_window_va: u64 = @import("../../memory/mmio.zig").window_base;
/// Runtime copy-window ways user_copy keeps per CPU (slot = cpu * ways
/// + way); the static diagnostic PT is only the serial BSP fallback. One slot per CPU made syscalls whose copies alternate between a few
/// pages (IPC header, receiver words, wait capacity) pay a PTE store, INVLPG
/// and a TLB refill on almost every copy. Four covers the measured working set
/// while keeping one CPU's ways in a single cache line.
pub const phys_copy_window_ways: usize = 4;

pub const gdt_kernel_code_selector: u16 = 0x08;
pub const gdt_kernel_data_selector: u16 = 0x10;
// SYSRET uses STAR.user+8 for SS and +16 for CS. One user pair is
// shared by boot, IRET, signals and SYSRET; old selector aliases are invalid.
pub const gdt_user_data_selector: u16 = 0x18;
pub const gdt_user_code_selector: u16 = 0x20;
pub const gdt_tss_selector: u16 = 0x28;
pub fn validUserSelectors(cs: u64, ss: u64) bool {
    return cs == (gdt_user_code_selector | 3) and ss == (gdt_user_data_selector | 3);
}

pub const page_present: u64 = 1 << 0;
pub const page_rw: u64 = 1 << 1;
pub const page_user: u64 = 1 << 2;
pub const page_ps: u64 = 1 << 7;
pub const page_nx: u64 = @as(u64, 1) << 63;

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
    divide_error_stub: usize,
    breakpoint_stub: usize,
    page_fault_stub: usize,
    general_protection_stub: usize,
    double_fault_stub: usize,
    invalid_opcode_stub: usize,
    invalid_tss_stub: usize,
    segment_not_present_stub: usize,
    stack_segment_fault_stub: usize,
    timer_interrupt_stub: usize,
    scheduler_wake_ipi_stub: usize,
    device_interrupt_stub: usize,
    lapic_timer_vector: u8,
    scheduler_wake_ipi_vector: u8,
    unrouted_device_interrupt_vector: u8,
    device_interrupt_vector: u8,
    device_interrupt_vector_count: u8,
};

var pml4_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var kernel_mmio_window: ?MmioWindow = null;

var high_mmio_pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var high_mmio_pd_tables: [high_mmio_pdp_table_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** high_mmio_pdp_table_count;
var high_kernel_pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var high_kernel_pd_tables: []direct_map.Table = &.{};
var high_kernel_pt_tables: []direct_map.Table = &.{};
pub var phys_copy_window_pt: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var idt: [256]interrupts.IdtEntry align(16) = [_]interrupts.IdtEntry{interrupts.zeroIdtEntry()} ** 256;
const gdt_template: [7]u64 = .{
    0x0000000000000000,
    0x00AF9A000000FFFF,
    0x00AF92000000FFFF,
    0x00CFF2000000FFFF,
    0x00AFFA000000FFFF,
    0x0000000000000000,
    0x0000000000000000,
};
var bootstrap_gdt: [1][7]u64 align(16) = .{gdt_template};
var gdt_tables: [][7]u64 = &bootstrap_gdt;
var ring0_stack_region_raw: [small_stack_raw_bytes]u8 align(4096) = [_]u8{0} ** small_stack_raw_bytes;
var pf_ist_stack_region_raw: [small_stack_raw_bytes]u8 align(4096) = [_]u8{0} ** small_stack_raw_bytes;
var df_ist_stack_region_raw: [df_stack_raw_bytes]u8 align(4096) = [_]u8{0} ** df_stack_raw_bytes;
var ap_stack_backing: []u64 = &.{};

var bootstrap_tss: [1]Tss = .{std.mem.zeroes(Tss)};
var tss_tables: []Tss = &bootstrap_tss;
var de_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var bp_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var pf_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var gp_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var df_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ud_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ts_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var np_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ss_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var timer_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var de_trampoline_entry: usize = 0;
var bp_trampoline_entry: usize = 0;
var pf_trampoline_entry: usize = 0;
var gp_trampoline_entry: usize = 0;
var df_trampoline_entry: usize = 0;
var ud_trampoline_entry: usize = 0;
var ts_trampoline_entry: usize = 0;
var np_trampoline_entry: usize = 0;
var ss_trampoline_entry: usize = 0;
var timer_trampoline_entry: usize = 0;
pub export var kernel_cr3_value: u64 = 0;
/// The operand every trap/syscall entry loads. With PCIDs the kernel lives in
/// PCID 0 and its mappings are not global, so a flushing load threw away the
/// whole kernel TLB working set on every entry. No-flush is safe because every
/// kernel mapping removal or attribute change is invalidated explicitly: the
/// all-CPU shootdowns write kernel_cr3_value itself (without bit 63), and
/// local changes reload it or INVLPG. Like user CR3 values, bit 63 never
/// enters stored CR3 values; it exists only in this load operand.
pub export var kernel_cr3_entry_value: u64 = 0;
pub export var kernel_syscall_stack_top: u64 = 0;
var bootstrap_syscall_stack_tops: [1]u64 = .{0};
pub export var kernel_syscall_stack_tops: [*]u64 = &bootstrap_syscall_stack_tops;
pub export var pcid_enabled: u64 = 0;
pub export var pku_enabled: u64 = 0;
/// Set once by the BSP before any AP starts. readFsBase/writeFsBase and the
/// GS pair then use RDFSBASE/WRFSBASE (7 and 14 cycles here, against 68 and
/// 150 for RDMSR/WRMSR), on every CPU: each AP must enable CR4.FSGSBASE before
/// it can execute them, or stay out (see smp AP start).
pub var fsgsbase_enabled: u64 = 0;
pub export var x86_rdtscp_supported: u64 = 0;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn staticStorageStart(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

fn minStaticStart(a: usize, b: usize) usize {
    return if (a < b) a else b;
}

pub fn kernelStaticStorageStartAddr() usize {
    var start = staticStorageStart(@TypeOf(pml4_table), &pml4_table);
    start = minStaticStart(start, staticStorageStart(@TypeOf(high_mmio_pdp_table), &high_mmio_pdp_table));
    start = minStaticStart(start, staticStorageStart(@TypeOf(high_mmio_pd_tables), &high_mmio_pd_tables));
    start = minStaticStart(start, staticStorageStart(@TypeOf(boot_scratch_mapping), &boot_scratch_mapping));
    start = minStaticStart(start, staticStorageStart(@TypeOf(high_kernel_pdp_table), &high_kernel_pdp_table));
    start = minStaticStart(start, staticStorageStart(@TypeOf(high_kernel_pd_tables), &high_kernel_pd_tables));
    start = minStaticStart(start, staticStorageStart(@TypeOf(high_kernel_pt_tables), &high_kernel_pt_tables));
    start = minStaticStart(start, staticStorageStart(@TypeOf(phys_copy_window_pt), &phys_copy_window_pt));
    start = minStaticStart(start, staticStorageStart(@TypeOf(idt), &idt));
    start = minStaticStart(start, staticStorageStart(@TypeOf(gdt_tables), &gdt_tables));
    start = minStaticStart(start, staticStorageStart(@TypeOf(ring0_stack_region_raw), &ring0_stack_region_raw));
    start = minStaticStart(start, staticStorageStart(@TypeOf(pf_ist_stack_region_raw), &pf_ist_stack_region_raw));
    start = minStaticStart(start, staticStorageStart(@TypeOf(df_ist_stack_region_raw), &df_ist_stack_region_raw));
    start = minStaticStart(start, staticStorageStart(@TypeOf(ap_stack_backing), &ap_stack_backing));
    start = minStaticStart(start, staticStorageStart(@TypeOf(tss_tables), &tss_tables));
    start = minStaticStart(start, staticStorageStart(@TypeOf(de_trampoline_page), &de_trampoline_page));
    start = minStaticStart(start, staticStorageStart(@TypeOf(bp_trampoline_page), &bp_trampoline_page));
    start = minStaticStart(start, staticStorageStart(@TypeOf(pf_trampoline_page), &pf_trampoline_page));
    start = minStaticStart(start, staticStorageStart(@TypeOf(gp_trampoline_page), &gp_trampoline_page));
    start = minStaticStart(start, staticStorageStart(@TypeOf(df_trampoline_page), &df_trampoline_page));
    start = minStaticStart(start, staticStorageStart(@TypeOf(ud_trampoline_page), &ud_trampoline_page));
    start = minStaticStart(start, staticStorageStart(@TypeOf(ts_trampoline_page), &ts_trampoline_page));
    start = minStaticStart(start, staticStorageStart(@TypeOf(np_trampoline_page), &np_trampoline_page));
    start = minStaticStart(start, staticStorageStart(@TypeOf(ss_trampoline_page), &ss_trampoline_page));
    start = minStaticStart(start, staticStorageStart(@TypeOf(timer_trampoline_page), &timer_trampoline_page));
    start = minStaticStart(start, staticStorageStart(@TypeOf(de_trampoline_entry), &de_trampoline_entry));
    start = minStaticStart(start, staticStorageStart(@TypeOf(bp_trampoline_entry), &bp_trampoline_entry));
    start = minStaticStart(start, staticStorageStart(@TypeOf(pf_trampoline_entry), &pf_trampoline_entry));
    start = minStaticStart(start, staticStorageStart(@TypeOf(gp_trampoline_entry), &gp_trampoline_entry));
    start = minStaticStart(start, staticStorageStart(@TypeOf(df_trampoline_entry), &df_trampoline_entry));
    start = minStaticStart(start, staticStorageStart(@TypeOf(ud_trampoline_entry), &ud_trampoline_entry));
    start = minStaticStart(start, staticStorageStart(@TypeOf(ts_trampoline_entry), &ts_trampoline_entry));
    start = minStaticStart(start, staticStorageStart(@TypeOf(np_trampoline_entry), &np_trampoline_entry));
    start = minStaticStart(start, staticStorageStart(@TypeOf(ss_trampoline_entry), &ss_trampoline_entry));
    start = minStaticStart(start, staticStorageStart(@TypeOf(timer_trampoline_entry), &timer_trampoline_entry));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_cr3_value), &kernel_cr3_value));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_cr3_entry_value), &kernel_cr3_entry_value));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_syscall_stack_top), &kernel_syscall_stack_top));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_syscall_stack_tops), &kernel_syscall_stack_tops));
    start = minStaticStart(start, staticStorageStart(@TypeOf(pcid_enabled), &pcid_enabled));
    start = minStaticStart(start, staticStorageStart(@TypeOf(invpcid_support), &invpcid_support));
    start = minStaticStart(start, staticStorageStart(@TypeOf(pku_enabled), &pku_enabled));
    start = minStaticStart(start, staticStorageStart(@TypeOf(x86_rdtscp_supported), &x86_rdtscp_supported));
    return start;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(pml4_table), &pml4_table));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(high_mmio_pdp_table), &high_mmio_pdp_table));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(high_mmio_pd_tables), &high_mmio_pd_tables));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(boot_scratch_mapping), &boot_scratch_mapping));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(high_kernel_pdp_table), &high_kernel_pdp_table));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(high_kernel_pd_tables), &high_kernel_pd_tables));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(high_kernel_pt_tables), &high_kernel_pt_tables));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(phys_copy_window_pt), &phys_copy_window_pt));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(idt), &idt));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(gdt_tables), &gdt_tables));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ring0_stack_region_raw), &ring0_stack_region_raw));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(pf_ist_stack_region_raw), &pf_ist_stack_region_raw));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(df_ist_stack_region_raw), &df_ist_stack_region_raw));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ap_stack_backing), &ap_stack_backing));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(tss_tables), &tss_tables));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(de_trampoline_page), &de_trampoline_page));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(bp_trampoline_page), &bp_trampoline_page));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(pf_trampoline_page), &pf_trampoline_page));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(gp_trampoline_page), &gp_trampoline_page));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(df_trampoline_page), &df_trampoline_page));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ud_trampoline_page), &ud_trampoline_page));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ts_trampoline_page), &ts_trampoline_page));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(np_trampoline_page), &np_trampoline_page));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ss_trampoline_page), &ss_trampoline_page));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(timer_trampoline_page), &timer_trampoline_page));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(de_trampoline_entry), &de_trampoline_entry));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(bp_trampoline_entry), &bp_trampoline_entry));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(pf_trampoline_entry), &pf_trampoline_entry));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(gp_trampoline_entry), &gp_trampoline_entry));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(df_trampoline_entry), &df_trampoline_entry));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ud_trampoline_entry), &ud_trampoline_entry));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ts_trampoline_entry), &ts_trampoline_entry));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(np_trampoline_entry), &np_trampoline_entry));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ss_trampoline_entry), &ss_trampoline_entry));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(timer_trampoline_entry), &timer_trampoline_entry));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_cr3_value), &kernel_cr3_value));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_cr3_entry_value), &kernel_cr3_entry_value));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_syscall_stack_top), &kernel_syscall_stack_top));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_syscall_stack_tops), &kernel_syscall_stack_tops));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(pcid_enabled), &pcid_enabled));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(invpcid_support), &invpcid_support));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(pku_enabled), &pku_enabled));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(x86_rdtscp_supported), &x86_rdtscp_supported));
    return end;
}

const msr_ia32_fs_base: u32 = 0xC000_0100;
const msr_ia32_gs_base: u32 = 0xC000_0101;
const msr_ia32_tsc_aux: u32 = 0xC000_0103;
const msr_ia32_efer: u32 = 0xC000_0080;
const msr_ia32_star: u32 = 0xC000_0081;
const msr_ia32_lstar: u32 = 0xC000_0082;
const msr_ia32_fmask: u32 = 0xC000_0084;
const efer_sce: u64 = 1 << 0;
const efer_nxe: u64 = 1 << 11;
const rflags_if: u64 = 1 << 9;
const rflags_df: u64 = 1 << 10;
const cr4_pge: u64 = 1 << 7;
const cr4_osfxsr: u64 = 1 << 9;
const cr4_osxmmexcpt: u64 = 1 << 10;
const cr4_pcide: u64 = 1 << 17;
const cr4_osxsave: u64 = 1 << 18;
const cr4_fsgsbase: u64 = 1 << 16;
const cr4_pke: u64 = 1 << 22;
const cpuid_leaf1_ecx_pcid: u32 = 1 << 17;
const cpuid_leaf1_ecx_xsave: u32 = 1 << 26;
const cpuid_leaf1_ecx_osxsave: u32 = 1 << 27;
const cpuid_leaf1_ecx_avx: u32 = 1 << 28;
const cpuid_leaf7_ecx_pku: u32 = 1 << 3;
const cpuid_leaf7_ebx_fsgsbase: u32 = 1 << 0;
const cpuid_leaf80000001_edx_rdtscp: u32 = 1 << 27;
const cpuid_leaf80000001_edx_nx: u32 = 1 << 20;
const page_addr_mask: u64 = 0x000f_ffff_ffff_f000;
const cr3_addr_mask: u64 = 0x000f_ffff_ffff_f000;
const cr3_no_flush: u64 = 1 << 63;

pub const xstate_bytes: usize = xstate_format.image_bytes;
pub const xstate_alignment: usize = xstate_format.image_alignment;
pub const xstate_feature_mask: u64 = xstate_format.feature_mask;

const CpuidResult = struct { eax: u32, ebx: u32, ecx: u32, edx: u32 };

fn cpuidSubleaf(leaf: u32, subleaf: u32) CpuidResult {
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
          [subleaf] "{ecx}" (subleaf),
    );
    return .{ .eax = eax, .ebx = ebx, .ecx = ecx, .edx = edx };
}

fn cpuid(leaf: u32) CpuidResult {
    return cpuidSubleaf(leaf, 0);
}

pub fn directMapCpuFeatures() struct { bits: u8, huge: bool } {
    const maximum = cpuidMaxExtendedLeaf();
    return .{ .bits = if (maximum >= 0x8000_0008) @truncate(cpuid(0x8000_0008).eax) else 36, .huge = maximum >= 0x8000_0001 and cpuid(0x8000_0001).edx & (1 << 26) != 0 };
}

fn cpuidMaxExtendedLeaf() u32 {
    return cpuid(0x8000_0000).eax;
}

fn cpuHasRdtscp() bool {
    if (cpuidMaxExtendedLeaf() < 0x8000_0001) return false;
    return (cpuid(0x8000_0001).edx & cpuid_leaf80000001_edx_rdtscp) != 0;
}

pub fn detectRdtscpSupport() bool {
    const supported = cpuHasRdtscp();
    x86_rdtscp_supported = if (supported) 1 else 0;
    return supported;
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

fn readCr0() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr0, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

fn writeCr0(value: u64) void {
    asm volatile ("mov %[value], %%cr0"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

fn readXcr0() u64 {
    var eax: u32 = 0;
    var edx: u32 = 0;
    asm volatile ("xgetbv"
        : [eax] "={eax}" (eax),
          [edx] "={edx}" (edx),
        : [ecx] "{ecx}" (@as(u32, 0)),
    );
    return (@as(u64, edx) << 32) | eax;
}

fn writeXcr0(value: u64) void {
    asm volatile ("xsetbv"
        :
        : [eax] "{eax}" (@as(u32, @truncate(value))),
          [edx] "{edx}" (@as(u32, @truncate(value >> 32))),
          [ecx] "{ecx}" (@as(u32, 0)),
        : .{ .memory = true });
}

/// Enable the one kernel-wide user xstate format on the current CPU.  XCR0 is
/// CPU-local, so the BSP and every AP must call this before entering user mode.
pub fn enableAvxXStateForCurrentCpu() bool {
    if (cpuid(0).eax < 0xD) return false;
    const base = cpuid(1);
    if ((base.ecx & (cpuid_leaf1_ecx_xsave | cpuid_leaf1_ecx_avx)) !=
        (cpuid_leaf1_ecx_xsave | cpuid_leaf1_ecx_avx))
    {
        return false;
    }
    const supported = cpuidSubleaf(0xD, 0);
    if ((@as(u64, supported.edx) << 32 | supported.eax) & xstate_feature_mask != xstate_feature_mask) {
        return false;
    }

    var cr0 = readCr0();
    cr0 &= ~@as(u64, 1 << 2); // EM=0
    cr0 |= @as(u64, 1 << 1); // MP=1
    writeCr0(cr0);
    var cr4 = readCr4();
    cr4 |= cr4_osfxsr | cr4_osxmmexcpt | cr4_osxsave;
    writeCr4(cr4);
    if ((cpuid(1).ecx & cpuid_leaf1_ecx_osxsave) == 0) return false;
    writeXcr0(xstate_feature_mask);
    if ((readXcr0() & xstate_feature_mask) != xstate_feature_mask) return false;
    const active = cpuidSubleaf(0xD, 0);
    if (active.ebx != xstate_bytes) return false;
    asm volatile ("clts");
    asm volatile ("fninit");
    return true;
}

var bootstrap_xsaveopt: [1]bool = .{false};
var xsaveopt_by_cpu: []bool = &bootstrap_xsaveopt;

pub fn configureXStateOptimizationForCpu(cpu: usize) void {
    xsaveopt_by_cpu[cpu] = cpuidSubleaf(0xD, 1).eax & 1 != 0;
}

pub fn saveThreadXState(state: *align(xstate_alignment) [xstate_bytes]u8) void {
    const cpu = @import("../../smp.zig").currentCpuSlot();
    if (!xsaveopt_by_cpu[cpu]) return saveXState(state);
    // Eager save to the same stable per-thread standard-format image used by
    // XRSTOR. Hardware controls init/modified elision; never skip the restore
    // when changing threads (no lazy-FPU transient exposure). Boot snapshots
    // still use plain XSAVE, and unsupported CPUs retain that implementation.
    asm volatile ("xsaveopt64 (%[ptr])"
        :
        : [ptr] "r" (state),
          [eax] "{eax}" (@as(u32, @truncate(xstate_feature_mask))),
          [edx] "{edx}" (@as(u32, @truncate(xstate_feature_mask >> 32))),
        : .{ .memory = true });
}

pub fn saveXState(state: *align(xstate_alignment) [xstate_bytes]u8) void {
    asm volatile ("xsave64 (%[ptr])"
        :
        : [ptr] "r" (state),
          [eax] "{eax}" (@as(u32, @truncate(xstate_feature_mask))),
          [edx] "{edx}" (@as(u32, @truncate(xstate_feature_mask >> 32))),
        : .{ .memory = true });
}

pub fn restoreXState(state: *align(xstate_alignment) const [xstate_bytes]u8) void {
    asm volatile ("xrstor64 (%[ptr])"
        :
        : [ptr] "r" (state),
          [eax] "{eax}" (@as(u32, @truncate(xstate_feature_mask))),
          [edx] "{edx}" (@as(u32, @truncate(xstate_feature_mask >> 32))),
        : .{ .memory = true });
}

fn currentMxcsrValidMask() u32 {
    var fx_state: [512]u8 align(16) = [_]u8{0} ** 512;
    asm volatile ("fxsave64 (%[ptr])"
        :
        : [ptr] "r" (&fx_state),
        : .{ .memory = true });
    const reported = xstate_format.mxcsrMaskFromFxsave(&fx_state);
    // Intel specifies this conservative mask for older implementations that
    // leave MXCSR_MASK zero in an FXSAVE image.
    return if (reported == 0) 0x0000_ffbf else reported;
}

pub fn validateUserXState(
    state: *const [xstate_bytes]u8,
) bool {
    return xstate_format.validateStandardUserImage(
        state,
        currentMxcsrValidMask(),
    );
}

fn refreshKernelCr3EntryValue() void {
    kernel_cr3_entry_value = if (pcid_enabled != 0) kernel_cr3_value | (@as(u64, 1) << 63) else kernel_cr3_value;
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
    refreshKernelCr3EntryValue();
    return true;
}

pub fn readPkru() u32 {
    if (pku_enabled == 0) return 0;
    var eax: u32 = 0;
    var edx: u32 = 0;
    asm volatile ("rdpkru"
        : [eax] "={eax}" (eax),
          [edx] "={edx}" (edx),
        : [ecx] "{ecx}" (@as(u32, 0)),
    );
    return eax;
}

pub fn writePkru(value: u32) void {
    if (pku_enabled == 0) return;
    asm volatile ("wrpkru"
        :
        : [eax] "{eax}" (value),
          [ecx] "{ecx}" (@as(u32, 0)),
          [edx] "{edx}" (@as(u32, 0)),
        : .{ .memory = true });
}

pub fn enablePkuIfSupported() bool {
    const features = cpuid(7);
    if ((features.ecx & cpuid_leaf7_ecx_pku) == 0) return false;
    var cr4 = readCr4();
    cr4 |= cr4_pke;
    writeCr4(cr4);
    pku_enabled = 1;
    writePkru(0);
    return true;
}

pub fn cr3AddressPart(value: u64) u64 {
    return value & cr3_addr_mask;
}

pub fn cr3WithUserPcid(raw_cr3: u64, pcid: u16) u64 {
    const raw = cr3AddressPart(raw_cr3);
    if (raw == 0) return 0;
    if (pcid_enabled == 0 or pcid == 0) return raw;
    return raw | (@as(u64, pcid) & 0xfff);
}

/// All user-return CR3 operands publish execution, even PCID-0/forced loads.
/// Leaf invalidations clear remote retention and flush only active users;
/// dormant CPUs must flush before reuse. Retirement, cache maintenance, PCID
/// reuse and PCID-0 still rendezvous with every online CPU before reclamation.
/// Stored CR3s never contain NOFLUSH; add it only at this IF-clear boundary.
pub fn userReturnCr3(cr3: u64, force_flush: bool) u64 {
    if (userReturnChecksEnabled()) assertUserReturnInterruptsOff();
    const retained = @import("../../tlb_residency.zig").enterUser(
        @import("../../smp.zig").currentCpuSlot(),
        @import("../../tlb_residency.zig").UserContext.fromCr3(cr3),
    );
    if (force_flush or pcid_enabled == 0 or (cr3 & 0xfff) == 0 or !retained) return cr3;
    return cr3 | (@as(u64, 1) << 63);
}

pub fn userReturnChecksEnabled() bool {
    if (@import("builtin").mode == .Debug) return true;
    return @import("kernel_lock_check_options").user_return_checks_enabled;
}

fn assertUserReturnInterruptsOff() void {
    var flags: u64 = undefined;
    asm volatile ("pushfq; popq %[flags]"
        : [flags] "=r" (flags),
        :
        : .{ .memory = true });
    if (flags & (1 << 9) != 0) asm volatile ("ud2");
}

/// Called immediately after the actual user CR3 load in checked builds.
pub export fn assertUserCr3LoadedFromAsm() callconv(.c) void {
    if (!userReturnChecksEnabled()) return;
    assertUserReturnInterruptsOff();
    const residency = @import("../../tlb_residency.zig");
    if (!residency.isActive(@import("../../smp.zig").currentCpuSlot(), residency.UserContext.fromCr3(readCr3())))
        asm volatile ("ud2");
}

pub fn userPcidForProcessIndex(process_index: usize) u16 {
    // PCID 0 is the safe flushing fallback.  Do not alias live process slots
    // into the 12-bit user PCID namespace.
    if (process_index >= 0xfff) return 0;
    return @intCast(process_index + 1);
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

/// Enable page-table execute-disable enforcement on the current CPU.
/// EFER is CPU-local, so this is required on the BSP and every AP.
pub fn enableNxForCurrentCpu() bool {
    if (cpuid(0x8000_0000).eax < 0x8000_0001) return false;
    if ((cpuid(0x8000_0001).edx & cpuid_leaf80000001_edx_nx) == 0) return false;
    writeMsr(msr_ia32_efer, readMsr(msr_ia32_efer) | efer_nxe);
    return (readMsr(msr_ia32_efer) & efer_nxe) != 0;
}

pub fn enableWriteProtectForCurrentCpu() bool {
    writeCr0(readCr0() | (@as(u64, 1) << 16));
    return (readCr0() & (@as(u64, 1) << 16)) != 0 and (readMsr(msr_ia32_efer) & efer_nxe) != 0;
}

// These are the user's FS/GS bases: the kernel itself never addresses
// through FS or GS (no swapgs, no %fs:/%gs: operands; CPU identity comes from
// the stack), so user code may hold any canonical value in them.
pub fn readFsBase() u64 {
    if (fsgsbase_enabled == 0) return readMsr(msr_ia32_fs_base);
    return asm volatile ("rdfsbase %[value]"
        : [value] "=r" (-> u64),
    );
}

pub fn writeFsBase(value: u64) void {
    if (fsgsbase_enabled == 0) return writeMsr(msr_ia32_fs_base, value);
    asm volatile ("wrfsbase %[value]"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

pub fn readGsBase() u64 {
    if (fsgsbase_enabled == 0) return readMsr(msr_ia32_gs_base);
    return asm volatile ("rdgsbase %[value]"
        : [value] "=r" (-> u64),
    );
}

pub fn writeGsBase(value: u64) void {
    if (fsgsbase_enabled == 0) return writeMsr(msr_ia32_gs_base, value);
    asm volatile ("wrgsbase %[value]"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

/// Whether WRMSR/WRFSBASE accept `va` as an FS/GS base: canonical under the
/// 4-level paging this kernel enables (LA57 stays off). Either half is fine:
/// the kernel never addresses through these bases.
pub fn isCanonicalBase(va: u64) bool {
    const upper = va >> 47;
    return upper == 0 or upper == 0x1ffff;
}

test "FS/GS bases must be 48-bit canonical, in either half" {
    try std.testing.expect(isCanonicalBase(0));
    try std.testing.expect(isCanonicalBase(0x0000_7fff_ffff_ffff));
    try std.testing.expect(isCanonicalBase(0xffff_8000_0000_0000));
    try std.testing.expect(isCanonicalBase(0xffff_ffff_ffff_ffff));
    try std.testing.expect(!isCanonicalBase(0x0000_8000_0000_0000));
    try std.testing.expect(!isCanonicalBase(0xffff_7fff_ffff_ffff));
}

/// BSP, before any AP starts: enable CR4.FSGSBASE where supported and switch
/// the base accessors to the instructions. User code can then also use
/// RDFSBASE/WRFSBASE; HWCAP2 does not advertise it.
pub fn enableFsgsbaseIfSupported() bool {
    if (!enableFsgsbaseOnCurrentCpu()) return false;
    fsgsbase_enabled = 1;
    return true;
}

/// Set CR4.FSGSBASE on this CPU only. An AP calls this when the BSP enabled
/// the instructions; it never turns the shared accessor choice on itself, so a
/// CPU lacking the feature cannot make others execute them.
pub fn enableFsgsbaseOnCurrentCpu() bool {
    if ((cpuidSubleaf(7, 0).ebx & cpuid_leaf7_ebx_fsgsbase) == 0) return false;
    writeCr4(readCr4() | cr4_fsgsbase);
    return true;
}

pub fn enableSyscallEntry(entry: usize) void {
    const star = (@as(u64, gdt_kernel_code_selector) << 32) | (@as(u64, gdt_user_data_selector - 8) << 48);
    writeMsr(msr_ia32_star, star);
    writeMsr(msr_ia32_lstar, @intCast(entry));
    // Clear IF and DF on syscall entry: kernel code (compiler-emitted string
    // copies included) assumes DF=0, and a user may enter with DF=1.
    writeMsr(msr_ia32_fmask, rflags_if | rflags_df);
    writeMsr(msr_ia32_efer, readMsr(msr_ia32_efer) | efer_sce);
}

fn writeU64LEBytes(ptr: [*]u8, offset: usize, value: u64) void {
    var i: usize = 0;
    while (i < 8) : (i += 1) {
        ptr[offset + i] = @intCast((value >> @intCast(i * 8)) & 0xFF);
    }
}

fn buildCr3SwitchTrampoline(page: *[4096]u8, target: usize) usize {
    @memset(page[0..], 0x90);
    const out: [*]u8 = @ptrCast(page);
    var off: usize = 0;

    // The interrupted register state must be CPU-local.  A scratch slot in
    // this shared trampoline page lets simultaneous interrupts overwrite one
    // another, so preserve RAX on the current CPU's ring-0/IST stack instead.
    out[off] = 0x50; // push %rax
    off += 1;
    out[off] = 0x48;
    out[off + 1] = 0xB8;
    writeU64LEBytes(out, off + 2, kernel_cr3_value);
    off += 10;
    out[off] = 0x0F;
    out[off + 1] = 0x22;
    out[off + 2] = 0xD8;
    off += 3;
    out[off] = 0x58; // pop %rax
    off += 1;
    out[off] = 0xFF;
    out[off + 1] = 0x25;
    out[off + 2] = 0x00;
    out[off + 3] = 0x00;
    out[off + 4] = 0x00;
    out[off + 5] = 0x00;
    off += 6;
    writeU64LEBytes(out, off, target);
    // Before APs/users exist: generated bytes become executable only after
    // every writable image alias has been withdrawn. No later rewrite occurs.
    const va = @intFromPtr(page);
    const pa = kernelVirtToPhys(va) orelse unreachable;
    const high = bootScratchHighPte(va) orelse unreachable;
    high.* = pa | page_present;
    invlpg(va);
    return va;
}

// Four levels for exactly one low trampoline page. Only this temporary
// root needs DMA32 placement: the protected-mode CR3 load has 32 bits.
var ap_boot_tables: ?[]u64 = null;
var ap_boot_physical: u64 = 0;
var ap_boot_trampoline: u64 = 0;
var ap_boot_retired: bool = false;

pub fn prepareApBootRoot(pa: u64, list: anytype) ?u64 {
    if (ap_boot_retired or ap_boot_tables != null or pa == 0 or pa >= 0x100000 or pa & 4095 != 0) return null;
    const physical = list.popContiguousInRange(4, 4096, four_gib) catch return null;
    const words = physical_layout.identitySlice(u64, .fromInt(physical), 4 * 512) orelse unreachable;
    @memset(words, 0);
    words[0] = (physical + 4096) | 3;
    words[511] = pml4_table[511];
    words[512] = (physical + 8192) | 3;
    words[1024] = (physical + 12288) | 3;
    ap_boot_tables = words;
    ap_boot_physical = physical;
    ap_boot_trampoline = pa;
    return physical;
}

/// No AP may still execute the page being rewritten. Serial startup waits
/// for the preceding AP's ACK after loading its formal CR3 and permanent GDT.
pub fn setApTrampolinePermissions(pa: u64, writable: bool, executable: bool) bool {
    if (ap_boot_retired or pa != ap_boot_trampoline or (writable and executable)) return false;
    const tables = ap_boot_tables orelse return false;
    const low = &tables[1536 + (pa >> 12)];
    const direct = boot_direct_map.draft.?.preparedLeaf(pa) catch return false;
    if ((direct.* & page_addr_mask) != pa or direct.* & page_present == 0) return false;
    low.* = 0;
    direct.* = pa | page_present | page_nx;
    const copy = @import("../../user_copy.zig");
    copy.flushTlbForCr3Range(ap_boot_physical, pa, 4096);
    copy.flushTlbForCr3Range(kernel_cr3_value, direct_map.base + pa, 4096);
    // Real/protected mode writes no page contents: descriptor accessed bits
    // are pre-set. No RWX alias exists while the low entry is executable.
    if (executable) low.* = pa | page_present;
    direct.* = pa | page_present | page_nx | (if (writable) page_rw else @as(u64, 0));
    copy.flushTlbForCr3Range(kernel_cr3_value, direct_map.base + pa, 4096);
    return true;
}

/// Called only on complete startup success. A timed-out AP may still fetch
/// its SIPI page, so every failure path retains these allocations until halt.
pub fn retireApBootRoot(list: anytype) bool {
    const tables = ap_boot_tables orelse return false;
    if (ap_boot_retired or !setApTrampolinePermissions(ap_boot_trampoline, false, false)) return false;
    tables[0] = 0;
    @import("../../user_copy.zig").flushTlbForCr3Range(ap_boot_physical, 0, 0x100000);
    const direct = boot_direct_map.draft.?.preparedLeaf(ap_boot_trampoline) catch return false;
    direct.* = ap_boot_trampoline | page_present | page_rw | page_nx;
    @import("../../user_copy.zig").flushTlbForCr3Range(kernel_cr3_value, direct_map.base + ap_boot_trampoline, 4096);
    // All CPUs have left the boot root and acknowledged its withdrawal.
    // PMM return failure keeps owned extents out of circulation and halts boot.
    ap_boot_retired = true;
    list.appendContiguousRange(ap_boot_physical, 4) catch return false;
    ap_boot_tables = null;
    list.appendPage(ap_boot_trampoline) catch return false;
    return true;
}

var permission_probe_data: [4096]u8 align(4096) = [_]u8{0xc3} ** 4096;

/// One BSP diagnostic after every AP has left its boot root. The temporary
/// user root shares exactly slot 511, just like a real AS. Interrupts remain
/// masked inside the fault boundary; no scheduler can dispatch this root.
pub fn checkRetiredLowMappings(list: anytype, trampoline: u64) bool {
    for (pml4_table[0..256]) |entry| if (entry != 0) return false;
    const probe = @import("boot_hhdm.zig");
    if (!probe.unmappedReadFault(trampoline)) return false;
    // Four paging levels plus a user code page and stack page.
    const pa = list.popContiguousInRange(6, 4096, physical_layout.allocation_limit) catch return false;
    const bytes = physical_layout.identitySlice(u8, .fromInt(pa), 6 * 4096) orelse unreachable;
    @memset(bytes, 0);
    const words: [*]u64 = @ptrCast(@alignCast(bytes.ptr));
    words[0] = (pa + 4096) | 7;
    words[511] = pml4_table[511];
    words[512] = (pa + 8192) | 7;
    words[1024] = (pa + 12288) | 7;
    words[1536 + 16] = (pa + 16384) | 5;
    words[1536 + 17] = (pa + 20480) | 7 | page_nx;
    const code = bytes[16384..][0..15];
    code[0] = 0x48;
    code[1] = 0xb8;
    std.mem.writeInt(u64, code[2..10], @intFromPtr(&permission_probe_data), .little);
    @memcpy(code[10..15], &[_]u8{ 0x48, 0x8b, 0x00, 0xcd, 0x80 });
    const denied = probe.userReadFault(pa);
    // Only the BSP ever loaded this root. Flush its PCID0 translations before
    // returning any paging or user-code storage to PMM.
    writeCr3(readCr3() & ~(@as(u64, 1) << 63));
    list.appendContiguousRange(pa, 6) catch return false;
    if (!denied) return false;
    @import("../../kernel_log.zig").write("isolation: kernel-low=empty low-PA=PF user-kernel=PF AP-root=retired\n");
    return true;
}

/// One BSP-only check before AP startup. Expected faults resume in the shared
/// boot fault boundary; normal trap accounting never sees these probes.
pub fn checkBootPermissions(list: anytype) bool {
    const probe = @import("boot_hhdm.zig");
    const sections = direct_map.linkedImageSections();
    const page = list.popContiguousBelow(1, physical_layout.allocation_limit) catch return false;
    defer list.appendPage(page) catch unreachable;
    const bytes = physical_layout.identitySlice(u8, .fromInt(page), 4096) orelse return false;
    bytes[0] = 0xc3; // A broken NX mapping returns safely and fails the check.
    if (!probe.permissionFault(sections.text, false) or
        !probe.permissionFault(sections.rodata, false) or
        !probe.unmappedReadFault(image_range.base_paddr) or
        !probe.unmappedReadFault(image_range.base_paddr + sections.rodata - sections.text) or
        !probe.permissionFault(@intFromPtr(&permission_probe_data), true) or
        !probe.permissionFault(@intFromPtr(bytes.ptr), true)) return false;
    @import("../../kernel_log.zig").write("permissions: cpu=0 WP=1 NXE=1 text-W=PF rodata-W=PF data-X=PF direct-X=PF low-image-R=PF\n");
    return true;
}

pub fn installInterruptTrampolines(targets: TrapTargets) void {
    de_trampoline_entry = buildCr3SwitchTrampoline(&de_trampoline_page, targets.divide_error_stub);
    bp_trampoline_entry = buildCr3SwitchTrampoline(&bp_trampoline_page, targets.breakpoint_stub);
    pf_trampoline_entry = buildCr3SwitchTrampoline(&pf_trampoline_page, targets.page_fault_stub);
    gp_trampoline_entry = buildCr3SwitchTrampoline(&gp_trampoline_page, targets.general_protection_stub);
    df_trampoline_entry = buildCr3SwitchTrampoline(&df_trampoline_page, targets.double_fault_stub);
    ud_trampoline_entry = buildCr3SwitchTrampoline(&ud_trampoline_page, targets.invalid_opcode_stub);
    ts_trampoline_entry = buildCr3SwitchTrampoline(&ts_trampoline_page, targets.invalid_tss_stub);
    np_trampoline_entry = buildCr3SwitchTrampoline(&np_trampoline_page, targets.segment_not_present_stub);
    ss_trampoline_entry = buildCr3SwitchTrampoline(&ss_trampoline_page, targets.stack_segment_fault_stub);
    timer_trampoline_entry = buildCr3SwitchTrampoline(&timer_trampoline_page, targets.timer_interrupt_stub);
    interrupts.clearIdt(&idt);
    interrupts.setIdtEntry(&idt, 0, gdt_kernel_code_selector, de_trampoline_entry, 0x8E);
    // INT3 is a user-callable trap; hardware saves the following instruction.
    interrupts.setIdtEntry(&idt, 3, gdt_kernel_code_selector, bp_trampoline_entry, 0xEE);
    interrupts.setIdtEntry(&idt, 6, gdt_kernel_code_selector, ud_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, 10, gdt_kernel_code_selector, ts_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, 11, gdt_kernel_code_selector, np_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, 12, gdt_kernel_code_selector, ss_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, 13, gdt_kernel_code_selector, gp_trampoline_entry, 0x8E);
    interrupts.setIdtEntryWithIst(&idt, 14, gdt_kernel_code_selector, pf_trampoline_entry, 1, 0x8E);
    interrupts.setIdtEntryWithIst(&idt, 8, gdt_kernel_code_selector, df_trampoline_entry, 2, 0x8E);
    interrupts.setIdtEntry(&idt, 0x20, gdt_kernel_code_selector, timer_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, targets.lapic_timer_vector, gdt_kernel_code_selector, timer_trampoline_entry, 0x8E);
    interrupts.setIdtEntry(&idt, targets.scheduler_wake_ipi_vector, gdt_kernel_code_selector, targets.scheduler_wake_ipi_stub, 0x8E);
    interrupts.setIdtEntry(
        &idt,
        targets.unrouted_device_interrupt_vector,
        gdt_kernel_code_selector,
        targets.device_interrupt_stub,
        0x8E,
    );
    var device_vector_index: u16 = 0;
    while (device_vector_index < targets.device_interrupt_vector_count) : (device_vector_index += 1) {
        const vector: usize = @as(usize, targets.device_interrupt_vector) + @as(usize, device_vector_index);
        interrupts.setIdtEntry(&idt, vector, gdt_kernel_code_selector, targets.device_interrupt_stub, 0x8E);
    }
    interrupts.loadIdt(&idt);
}

pub fn readCr2() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr2, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

// Query on the receiving CPU: a BSP feature bit alone cannot authorize an
// instruction on an AP. Cache only after this CPU has queried its own CPUID.
var bootstrap_invpcid_support: [1]u8 = .{0};
var invpcid_support: []u8 = &bootstrap_invpcid_support;
pub fn hasInvpcid() bool {
    const cpu = @import("../../smp.zig").currentCpuSlot();
    const cached = @atomicLoad(u8, &invpcid_support[cpu], .monotonic);
    if (cached != 0) return cached == 2;
    const supported = cpuid(0).eax >= 7 and (cpuidSubleaf(7, 0).ebx & (1 << 10)) != 0;
    @atomicStore(u8, &invpcid_support[cpu], if (supported) 2 else 1, .monotonic);
    return supported;
}

pub fn invalidatePcidAddress(pcid: u12, va: u64) void {
    // Type 0 preserves unrelated PCIDs; caller guarantees non-global user
    // leaf mappings, canonical addresses, and CPUID.07H:EBX.INVPCID.
    const descriptor = [2]u64{ pcid, va };
    asm volatile ("invpcid (%[descriptor]), %[kind]"
        :
        : [descriptor] "r" (&descriptor),
          [kind] "r" (@as(u64, 0)),
        : .{ .memory = true });
}

pub fn writeCr3(value: u64) void {
    var operand = value;
    if (pcid_enabled != 0 and value & 4095 != 0) {
        // Registration and its load are one CPU-local transaction. Otherwise
        // an IPI could ACK between them, then this load retain stale entries.
        var flags: u64 = undefined;
        asm volatile ("pushfq; popq %[flags]; cli"
            : [flags] "=r" (flags),
            :
            : .{ .memory = true });
        if (!@import("../../tlb_residency.zig").enterRetained(@import("../../smp.zig").currentCpuSlot(), value)) operand &= ~(@as(u64, 1) << 63);
        writeCr3ForMaintenance(operand);
        if (flags & (1 << 9) != 0) asm volatile ("sti" ::: .{ .memory = true });
        return;
    }
    writeCr3ForMaintenance(operand);
}

// Invalidation-only loads do not walk user mappings. Keeping them outside
// residency/active avoids publishing an ACK receiver as a user entrant.
pub fn writeCr3ForMaintenance(value: u64) void {
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

pub fn readTimestampCounter() u64 {
    var lo: u32 = 0;
    var hi: u32 = 0;
    asm volatile ("rdtsc"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
    );
    return (@as(u64, hi) << 32) | @as(u64, lo);
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
    return raw[start .. start + raw.len - stack_region_align];
}

const stack_high_water_enabled = if (@hasDecl(@import("root"), "stack_high_water_enabled")) @import("root").stack_high_water_enabled else false;

fn stackPaintWord(cpu_slot: usize, stack: usize, word: usize) u64 {
    return 0xd6e8_feb8_6659_fd93 ^ (@as(u64, cpu_slot) << 48) ^
        (@as(u64, stack) << 40) ^ (@as(u64, word) *% 0x9e37_79b9_7f4a_7c15);
}

fn paintUnusedStack(bytes: []align(8) u8, cpu_slot: usize, stack: usize) void {
    // Volatile scalar stores keep painting observable to the host and avoid
    // touching XState before the early boot CPU setup. No running stack may
    // reach this function as a destination; guards are excluded by callers.
    const words: [*]volatile u64 = @ptrCast(bytes.ptr);
    for (0..bytes.len / 8) |word| words[word] = stackPaintWord(cpu_slot, stack, word);
}

test "BSP stack painting excludes guards and distinguishes CPU and IST" {
    try std.testing.expectEqual(@as(usize, 32), @bitOffsetOf(Tss, "rsp0"));
    try std.testing.expectEqual(@as(usize, 288), @bitOffsetOf(Tss, "ist1"));
    try std.testing.expectEqual(@as(usize, 352), @bitOffsetOf(Tss, "ist2"));
    var bytes: [4096 * 3]u8 align(8) = @splat(0x6b);
    paintUnusedStack(bytes[4096..8192], 3, 1);
    for (bytes[0..4096]) |b| try std.testing.expectEqual(@as(u8, 0x6b), b);
    for (bytes[8192..]) |b| try std.testing.expectEqual(@as(u8, 0x6b), b);
    for (0..512) |word| {
        const actual = std.mem.readInt(u64, bytes[4096 + word * 8 ..][0..8], .little);
        try std.testing.expectEqual(stackPaintWord(3, 1, word), actual);
        try std.testing.expect(actual != stackPaintWord(2, 1, word));
        try std.testing.expect(actual != stackPaintWord(3, 2, word));
    }
    try std.testing.expectEqual(@as(u64, 0xd6eb_ffb8_6659_fd93), stackPaintWord(3, 1, 0));
    try std.testing.expectEqual(@as(u64, 0x48dc_8601_1913_8186), stackPaintWord(3, 1, 1));
}

pub fn paintUnusedBspStacks(initial_rsp: u64) bool {
    if (comptime !stack_high_water_enabled) return true;
    const raws = [_][]u8{ ring0_stack_region_raw[0..], pf_ist_stack_region_raw[0..], df_ist_stack_region_raw[0..] };
    // Validate every destination before the first store. The caller is still
    // on Limine's stack, before any BSP runtime stack or IST becomes active.
    var current_rsp: u64 = 0;
    asm volatile ("mov %%rsp, %[out]"
        : [out] "=r" (current_rsp),
    );
    for (raws, 0..) |raw, stack| {
        const region = alignedStackRegion(raw);
        const bytes = bsp_stack_bytes[stack];
        const bottom = stackBottom(region, bytes);
        const top = stackTop(region, bytes);
        if (current_rsp <= top and initial_rsp >= bottom) return false;
    }
    for (raws, 0..) |raw, stack| {
        const region = alignedStackRegion(raw);
        const bytes = bsp_stack_bytes[stack];
        const start: usize = @intCast(stackBottom(region, bytes) - @intFromPtr(region.ptr));
        paintUnusedStack(@alignCast(region[start .. start + bytes]), 0, stack);
    }
    return true;
}

pub fn cpuKernelStackTop(cpu_slot: usize) ?u64 {
    if (cpu_slot == 0) return stackTop(alignedStackRegion(ring0_stack_region_raw[0..]), ring0_stack_bytes);
    if (cpu_slot >= tss_tables.len) return null;
    if (ap_stack_backing[cpu_slot - 1] == 0) return null;
    return cpu_stacks.stackBase(cpu_slot, 0).? + cpu_stacks.kernel_bytes;
}

pub fn cpuSlotForStackPointer(rsp: u64) cpu_stacks.CpuIdentity {
    if (stackPointerInRegion(ring0_stack_region_raw[0..], ring0_stack_bytes, rsp)) return .bootstrap;
    if (stackPointerInRegion(pf_ist_stack_region_raw[0..], pf_ist_stack_bytes, rsp)) return .bootstrap;
    if (stackPointerInRegion(df_ist_stack_region_raw[0..], df_ist_stack_bytes, rsp)) return .bootstrap;

    return cpu_stacks.apSlotForStackPointer(rsp, ap_stack_backing);
}

inline fn stackPointerInRegion(raw: []u8, usable_bytes: usize, rsp: u64) bool {
    const region = alignedStackRegion(raw);
    // All BSP stacks grow down from region's upper guard. In particular the
    // usable bytes occupy the upper part of each region. Match the
    // actual mapping/TSS bounds; an empty stack's top is a valid RSP too.
    return rsp >= stackBottom(region, usable_bytes) and rsp <= stackTop(region, usable_bytes);
}

test "BSP stack CPU identity uses the actual kernel and IST bounds" {
    const raws = [_][]u8{ ring0_stack_region_raw[0..], pf_ist_stack_region_raw[0..], df_ist_stack_region_raw[0..] };
    try std.testing.expectEqual(@as(usize, 14 * 1024 * 1024), ring0_stack_region_raw.len + pf_ist_stack_region_raw.len + df_ist_stack_region_raw.len);
    for (raws, 0..) |raw, index| {
        const region = alignedStackRegion(raw);
        const bytes = bsp_stack_bytes[index];
        const bottom = stackBottom(region, bytes);
        const top = stackTop(region, bytes);
        for ([_]u64{ bottom, bottom + bytes / 2, top }) |rsp|
            try std.testing.expectEqual(cpu_stacks.CpuIdentity.bootstrap, cpuSlotForStackPointer(rsp));
        for ([_]u64{ bottom - 1, top + 1 }) |rsp|
            try std.testing.expectEqual(cpu_stacks.CpuIdentity.unknown, cpuSlotForStackPointer(rsp));
        try std.testing.expect(bottom >= @intFromPtr(region.ptr) + guard_page_bytes);
        try std.testing.expectEqual(@intFromPtr(region.ptr) + region.len - guard_page_bytes, top);
        try std.testing.expectEqual(cpu_stacks.CpuIdentity.unknown, cpuSlotForStackPointer(bottom - guard_page_bytes));
        const tss_top = switch (index) {
            0 => cpuKernelStackTop(0),
            1 => cpuPageFaultIstTop(0),
            2 => cpuDoubleFaultIstTop(0),
            else => unreachable,
        };
        try std.testing.expectEqual(@as(?u64, top), tss_top);
    }
}

fn cpuPageFaultIstTop(cpu_slot: usize) ?u64 {
    if (cpu_slot == 0) return stackTop(alignedStackRegion(pf_ist_stack_region_raw[0..]), pf_ist_stack_bytes);
    if (cpu_slot >= tss_tables.len) return null;
    if (ap_stack_backing[cpu_slot - 1] == 0) return null;
    return cpu_stacks.stackBase(cpu_slot, 1).? + cpu_stacks.pf_ist_bytes;
}

fn cpuDoubleFaultIstTop(cpu_slot: usize) ?u64 {
    if (cpu_slot == 0) return stackTop(alignedStackRegion(df_ist_stack_region_raw[0..]), df_ist_stack_bytes);
    if (cpu_slot >= tss_tables.len) return null;
    if (ap_stack_backing[cpu_slot - 1] == 0) return null;
    return cpu_stacks.stackBase(cpu_slot, 2).? + cpu_stacks.df_ist_bytes;
}

fn kernelHighMappingActive() bool {
    return image_range.virtual_base >= 0xffff_8000_0000_0000 and image_range.base_paddr != 0;
}

fn kernelVirtToPhys(addr: u64) ?u64 {
    const physical = kernelAddressToPhysical(.fromInt(addr)) orelse return null;
    return physical.toInt();
}

fn kernelPtrPaddr(ptr: anytype) ?u64 {
    return kernelVirtToPhys(@intFromPtr(ptr));
}

/// Device and firmware owners borrow only this sparse high window. RAM and
/// image extents are excluded: otherwise cache policy could miss a WB alias.
pub fn kernelMmioRangeFits(base: u64, bytes: u64) bool {
    const window = if (kernel_mmio_window) |*value| value else return false;
    return window.rangeFits(base, bytes);
}

fn nonRamWindowRange(base: u64, bytes: u64) bool {
    if (!kernelMmioRangeFits(base, bytes)) return false;
    const start = base & ~@as(u64, 4095);
    const end = (base + bytes + 4095) & ~@as(u64, 4095);
    const layout = boot_direct_map.layout orelse return false;
    for (layout.regions) |region| {
        if ((region.usable or region.prepare_only) and start < region.base + region.bytes and region.base < end) return false;
    }
    return !(start < image_range.base_paddr + image_range.size_bytes and image_range.base_paddr < end);
}

pub fn kernelMmioBorrow(base: u64, bytes: usize) ?@import("../../memory/mmio.zig").Borrow {
    const window = if (kernel_mmio_window) |*value| value else return null;
    const va = window.lookup(base, bytes, page_nx | 0x1b) orelse return null;
    return @import("../../memory/mmio.zig").mappedAfterValidation(@ptrFromInt(va), bytes, .uc);
}

pub fn mapKernelFirmwareBytes(base: u64, bytes: usize) ?[]const u8 {
    if (!nonRamWindowRange(base, bytes)) return null;
    const window = if (kernel_mmio_window) |*value| value else return null;
    const va = window.map(base, bytes, page_nx | page_present) catch return null;
    return @as([*]const u8, @ptrFromInt(va))[0..bytes];
}

pub fn kernelMmioOverlaps(base: u64, bytes: u64) bool {
    const window = if (kernel_mmio_window) |*value| value else return true;
    return window.overlaps(base, bytes);
}

pub fn kernelDeviceCacheAllowed(base: u64, bytes: u64, wc: bool) bool {
    // A BAR token is not proof that firmware placed it outside usable RAM.
    // Device transitions must never leave the RAM direct alias behind.
    if (!nonRamWindowRange(base, bytes)) return false;
    return kernel_mmio_window.?.deviceCacheAllowed(base, bytes, wc);
}

pub fn sealKernelMmioWindow() void {
    if (kernel_mmio_window) |*window| window.sealed = true;
}

/// Serial boot adds absent high VA extents before publishing a device Borrow.
/// PAT3 must actually be UC; firmware/device ranges cannot overlap RAM/image.
pub fn mapBootMmioRange(base: u64, bytes: u64) bool {
    if (((base | bytes) & 4095) != 0 or !nonRamWindowRange(base, bytes)) return false;
    if (((readMsr(0x277) >> 24) & 0xff) != 0) return false;
    const window = if (kernel_mmio_window) |*value| value else return false;
    _ = window.map(base, bytes, page_present | page_rw | page_nx | (1 << 3) | (1 << 4)) catch return false;
    return true;
}

/// Validate the live ownership chain as well as the UC reservation record.
pub fn bootMmioPageIsExplicitUc(page_base: u64) bool {
    if (page_base & 4095 != 0) return false;
    const window = if (kernel_mmio_window) |*value| value else return false;
    window.validate(kernelPtrPaddr(&phys_copy_window_pt) orelse return false) catch return false;
    return window.lookup(page_base, 4096, page_present | page_rw | page_nx | 0x18) != null;
}

/// Enumerate the native runtime's persistent direct aliases. The temporary
/// physical-copy windows are retired by the writer's all-CPU rendezvous, not
/// treated as permanent direct leaves. Canonical VMO ownership is required
/// separately; in particular it excludes kernel stacks and page-table pages.
pub fn visitRamCacheDirectAliases(pages: []const u64, context: anytype, comptime visit: anytype) !void {
    const draft = if (boot_direct_map.draft) |*value| value else return;
    const SplitView = struct {
        pub fn borrow(_: @This(), va: u64, size: direct_map.LeafSize, pa: u64) ?*[512]u64 {
            if (!cpu_runtime.directSplitOwns(va, size, pa) and !@import("../../memory/user_vm.zig").ramCacheKernelAliasSplitOwns(va, size, pa)) return null;
            return borrowIdentityTableMut(.fromInt(pa));
        }
    };
    try draft.validateRoot(&pml4_table);
    try draft.visitAliases(boot_direct_map.layout.?, pages, SplitView{}, context, visit);
}

pub fn validateRamCachePages(pages: []const u64) !void {
    try @import("../../memory/ram_cache_paging.zig").validatePages(pages, physical_layout.allocation_limit);
    try validateRamCacheKernelRoots();
    for (pages) |pa| {
        if (kernelMmioOverlaps(pa, 4096)) return error.CacheAliasConflict;
        if (!ramCacheImagePageAllowed(pa)) return error.KernelImageAlias;
    }
}

fn ramCacheImagePageAllowed(pa: u64) bool {
    if (pa < image_range.base_paddr or pa - image_range.base_paddr >= image_range.size_bytes) return true;
    const returned = @atomicLoad(u64, &boot_scratch_mapping.returned_start, .acquire);
    if (returned == 0 or pa < returned or pa >= boot_scratch_mapping.returned_end) return false;
    const Mapper = if (@import("builtin").is_test) ScratchMappingFixture else NativeScratchMapping;
    const high = Mapper.high(boot_scratch_mapping.retired_start + (pa - returned)) orelse return false;
    return @atomicLoad(u64, high, .acquire) == 0;
}

/// Preflight is side-effect free. Caller holds all VM transactions and has
/// proved the range is PCI MMIO with effective UC old aliases. A successful
/// acquire cannot fail after this check under the global state lock.
fn highKernelPdSlot(first_pdp_index: usize, pdp_index: usize) ?usize {
    if (pdp_index < first_pdp_index) return null;
    const slot = pdp_index - first_pdp_index;
    if (slot >= high_kernel_pd_tables.len) return null;
    return slot;
}

fn installHighKernelMapping() bool {
    if (boot_scratch_mapping.start != 0) return false;
    if (!kernelHighMappingActive() or image_range.size_bytes == 0) return false;
    const sections = direct_map.linkedImageSections();
    sections.validate() catch return false;
    if (sections.text != image_range.virtual_base or sections.end - sections.text > image_range.size_bytes) return false;
    high_kernel_pd_tables = boot_direct_map.image_pds;
    high_kernel_pt_tables = boot_direct_map.image_pts;
    const pbase = image_range.base_paddr;
    sections.install(&high_kernel_pdp_table, high_kernel_pd_tables, high_kernel_pt_tables, pbase) catch return false;
    const high_pdp_pa = kernelPtrPaddr(&high_kernel_pdp_table) orelse return false;
    boot_direct_map.installScratchRoot(&high_kernel_pdp_table, high_pdp_pa) catch return false;
    pml4_table[511] = high_pdp_pa | page_present | page_rw;
    return true;
}

// Boot-only ownership fence. No user root or AP may exist while preparing;
// after retirement the image mapping must never be rebuilt over this hole.
const BootScratchMapping = struct {
    start: u64 = 0,
    bytes: usize = 0,
    retired: bool = false,
    retired_start: u64 = 0,
    retired_bytes: usize = 0,
    acknowledged: bool = false,
    // One BSP writer before user dispatch; readers acquire returned_start
    // before reading the immutable end/retired extent. Zero means no exception.
    returned_start: u64 = 0,
    returned_end: u64 = 0,

    fn acknowledge(self: *@This(), va: u64, bytes: usize) bool {
        if (!self.retired or self.acknowledged or va != self.retired_start or bytes != self.retired_bytes) return false;
        self.acknowledged = true;
        return true;
    }
};
var boot_scratch_mapping: BootScratchMapping = .{};

fn bootScratchHighPte(va: u64) ?*u64 {
    if (!kernelHighMappingActive() or va < image_range.virtual_base or
        va - image_range.virtual_base >= image_range.size_bytes) return null;
    const vbase = image_range.virtual_base & ~@as(u64, 4095);
    const slot: usize = @intCast((va - vbase) >> 21);
    if (slot >= high_kernel_pt_tables.len) return null;
    const pd_slot = highKernelPdSlot(@intCast((vbase >> 30) & 511), @intCast((va >> 30) & 511)) orelse return null;
    const expected_pd = high_kernel_pd_tables[pd_slot].physical;
    const expected_pt = high_kernel_pt_tables[slot].physical;
    if ((high_kernel_pdp_table[(va >> 30) & 511] & ~@as(u64, 1 << 5)) != (expected_pd | page_present | page_rw) or
        (high_kernel_pd_tables[pd_slot].words[(va >> 21) & 511] & ~@as(u64, 1 << 5)) != (expected_pt | page_present | page_rw)) return null;
    return &high_kernel_pt_tables[slot].words[(va >> 12) & 511];
}

const NativeScratchMapping = struct {
    fn high(va: u64) ?*u64 {
        return bootScratchHighPte(va);
    }
};

// Before publishing CR3, remove image guard leaves. No low or direct image
// alias exists that could bypass the guard through the same physical page.
fn protectBspStackGuardWith(comptime Mapper: type, va: u64, pa: u64) bool {
    if (((va | pa) & 4095) != 0 or pa == 0) return false;
    const high = Mapper.high(va) orelse return false;
    if (!scratchRamLeaf(high.*, pa)) return false;
    high.* = 0;
    return true;
}

fn protectBspStackGuards() bool {
    const raws = [_][]u8{ ring0_stack_region_raw[0..], pf_ist_stack_region_raw[0..], df_ist_stack_region_raw[0..] };
    for (raws, bsp_stack_bytes) |raw, bytes| {
        const region = alignedStackRegion(raw);
        for ([_]u64{ stackBottom(region, bytes) - guard_page_bytes, stackTop(region, bytes) }) |va| {
            const pa = kernelVirtToPhys(va) orelse return false;
            if (!protectBspStackGuardWith(NativeScratchMapping, va, pa)) return false;
        }
    }
    return true;
}

fn scratchRamLeaf(entry: u64, pa: u64) bool {
    // Keep WB, supervisor, writable RAM; do not silently repurpose MMIO,
    // global or user aliases. Hardware accessed/dirty bits are immaterial.
    return (entry & ~(page_nx | @as(u64, (1 << 5) | (1 << 6)))) == (pa | page_present | page_rw);
}

// Compile-time selection lets host tests execute the same complete transaction
// without physical pointer dereferences or privileged CR3 instructions.
fn prepareBootScratchWith(comptime Mapper: type, va: u64, pa: u64, bytes: usize) bool {
    if (bytes == 0 or ((va | pa | bytes) & 4095) != 0 or pa == 0 or
        bytes > std.math.maxInt(u64) - va or bytes > physical_layout.allocation_limit -| pa) return false;
    var offset: usize = 0;
    while (offset < bytes) : (offset += 4096) {
        const high = Mapper.high(va + offset) orelse return false;
        if (!scratchRamLeaf(high.*, pa + offset) or high.* & page_nx == 0) return false;
    }
    return true;
}

pub fn prepareBootScratchMapping(va: u64, bytes: usize) bool {
    if (boot_scratch_mapping.start != 0 or kernel_cr3_value == 0) return false;
    const pa = kernelVirtToPhys(va) orelse return false;
    if (!prepareBootScratchWith(NativeScratchMapping, va, pa, bytes)) return false;
    // Only the BSP is online and no user PCID has run. Retire its old executable
    // translations before exposing the prepared tables to APs or user roots.
    writeCr3(readCr3());
    boot_scratch_mapping = .{ .start = va, .bytes = bytes };
    return true;
}

fn withdrawBootScratchWith(comptime Mapper: type, va: u64, pa: u64, bytes: usize) bool {
    if (!prepareBootScratchWith(Mapper, va, pa, bytes)) return false;
    var offset: usize = 0;
    while (offset < bytes) : (offset += 4096) (Mapper.high(va + offset) orelse unreachable).* = 0;
    return true;
}

// The Arena is the only production caller (through entry's static adapter).
// It supplies its computed suffix; this boundary independently rejects any
// extent outside the prepared scratch mapping. No PMM publication happens here.
pub fn withdrawBootScratchMapping(va: u64, bytes: usize) bool {
    const prepared = &boot_scratch_mapping;
    if (prepared.start == 0 or prepared.retired or va < prepared.start or
        bytes > prepared.bytes or va - prepared.start != prepared.bytes - bytes) return false;
    const pa = kernelVirtToPhys(va) orelse return false;
    if (!withdrawBootScratchWith(NativeScratchMapping, va, pa, bytes)) return false;
    prepared.retired_start = va;
    prepared.retired_bytes = bytes;
    prepared.retired = true;
    return true;
}

pub fn acknowledgeBootScratchMapping(va: u64, bytes: usize) bool {
    return boot_scratch_mapping.acknowledge(va, bytes);
}

fn returnBootScratchWith(comptime Mapper: type, mapping: *BootScratchMapping, list: anytype, pa: u64, pages: usize) !void {
    const bytes = std.math.mul(usize, pages, 4096) catch return error.InvalidState;
    const end = std.math.add(u64, pa, bytes) catch return error.InvalidState;
    if (!mapping.retired or !mapping.acknowledged or bytes == 0 or bytes != mapping.retired_bytes or pa == 0 or (pa & 4095) != 0 or end > physical_layout.allocation_limit or
        @atomicLoad(u64, &mapping.returned_start, .acquire) != 0) return error.InvalidState;
    // Validate all protection before the fallible PMM return. Nothing after
    // that return may fail and pretend the pages are still owned by scratch.
    var offset: usize = 0;
    while (offset < bytes) : (offset += 4096) {
        const high = Mapper.high(mapping.retired_start + offset) orelse return error.InvalidState;
        if (high.* != 0) return error.InvalidState;
    }
    if (comptime Mapper == NativeScratchMapping) {
        try boot_direct_map.prepareReturn(pa, bytes);
        boot_direct_map.publishReturn(pa, bytes);
        physical_layout.returnImageExtent(pa, end);
    }
    if (comptime Mapper == ScratchMappingFixture) {
        var offset2: usize = 0;
        boot_direct_map.layout.?.returned = .{ .start = pa, .end = end };
        while (offset2 < bytes) : (offset2 += 4096)
            ScratchMappingFixture.leaves[((pa + offset2) >> 12) & 511] = (pa + offset2) | page_present | page_rw | page_nx;
    }
    try list.appendContiguousRange(pa, pages);
    // ACK precedes PMM ownership transfer. Only a successful transfer grants
    // the runtime image exception; failure leaves its sentinel empty. The BSP
    // publishes once, before dispatch, and never changes this extent again.
    mapping.returned_end = end;
    @atomicStore(u64, &mapping.returned_start, pa, .release);
}

pub fn returnBootScratchPages(list: anytype, pa: u64, pages: usize) !void {
    const expected = kernelVirtToPhys(boot_scratch_mapping.retired_start) orelse return error.InvalidState;
    if (expected != pa) return error.InvalidState;
    try returnBootScratchWith(NativeScratchMapping, &boot_scratch_mapping, list, pa, pages);
}

pub fn validateKernelRuntimeRange(base: u64, bytes: usize) bool {
    if (kernelHighMappingActive()) {
        if (bytes == 0) return true;
        if (base < image_range.virtual_base) return false;
        const offset = base - image_range.virtual_base;
        return image_range.size_bytes == 0 or offset + @as(u64, @intCast(bytes)) <= image_range.size_bytes;
    }
    return false;
}

// Called on the BSP before AP startup and before user address spaces exist.
// Backing is lifetime-pinned, but only CPUs actually present consume it.
pub fn allocateApRuntimeStacks(cpu_slot: usize, free_list: anytype) bool {
    if (cpu_slot == 0 or cpu_slot >= tss_tables.len or !kernelHighMappingActive()) return false;
    if (ap_stack_backing[cpu_slot - 1] != 0) return true;
    const backing = free_list.popContiguousBelow(cpu_stacks.backing_bytes / 4096, physical_layout.allocation_limit) catch return false;
    const raw = physical_layout.identitySlice(u8, .fromInt(backing), cpu_stacks.backing_bytes) orelse unreachable;
    if (comptime stack_high_water_enabled) {
        // Backing is private and no INIT/SIPI has been sent. Physical backing
        // packs only usable ranges; virtual guard holes have no backing here.
        var start: usize = 0;
        for (cpu_stacks.sizes, 0..) |bytes, stack| {
            paintUnusedStack(@alignCast(raw[start .. start + bytes]), cpu_slot, stack);
            start += bytes;
        }
    } else {
        @memset(raw[0..cpu_stacks.backing_bytes], 0);
    }
    var phys = backing;
    for (cpu_stacks.sizes, 0..) |bytes, stack| {
        const va = cpu_stacks.stackBase(cpu_slot, stack).?;
        var off: usize = 0;
        while (off < bytes) : (off += 4096) {
            cpu_runtime.map(va + off, phys) catch return false;
            phys += 4096;
        }
    }
    ap_stack_backing[cpu_slot - 1] = backing;
    return true;
}

pub fn mapCpuRuntimeStacks(cpu_slot: usize) bool {
    if (cpu_slot >= tss_tables.len) return false;
    return if (cpu_slot == 0) kernelHighMappingActive() else ap_stack_backing[cpu_slot - 1] != 0;
}

pub fn installKernelPageTables() bool {
    // Never clear live tables or recreate a retired scratch alias.
    if (boot_scratch_mapping.start != 0) return false;
    @memset(pml4_table[0..], 0);
    @memset(high_mmio_pdp_table[0..], 0);
    @memset(high_mmio_pd_tables[0][0..], 0);

    const pml4_pa: u64 = kernelPtrPaddr(&pml4_table) orelse return false;
    const high_pdp_pa: u64 = kernelPtrPaddr(&high_mmio_pdp_table) orelse return false;
    const high_pd0_pa: u64 = kernelPtrPaddr(&high_mmio_pd_tables[0]) orelse return false;
    const phys_copy_window_pt_pa: u64 = kernelPtrPaddr(&phys_copy_window_pt) orelse return false;
    const kernel_table_flags = page_present | page_rw;
    pml4_table[high_mmio_pml4_index] = high_pdp_pa | kernel_table_flags;
    @memset(phys_copy_window_pt[0..], 0);
    // Slot 510 has no blanket PA=VA mapping. The first PDE is exclusively
    // the copy window; later device extents are allocated by the MMIO owner.
    high_mmio_pdp_table[0] = high_pd0_pa | kernel_table_flags | page_nx;
    high_mmio_pd_tables[0][0] = phys_copy_window_pt_pa | kernel_table_flags;

    if (!installHighKernelMapping()) return false;
    // A failure leaves these unpublished tables unusable; the caller halts boot.
    if (kernelHighMappingActive() and !protectBspStackGuards()) return false;

    const draft = if (boot_direct_map.draft) |*value| value else return false;
    draft.publish(&pml4_table) catch return false;
    writeCr3(pml4_pa);
    boot_direct_map.rebase();
    physical_layout.activateDirect(boot_direct_map.layout.?.limit, .{
        .virtual_base = .fromInt(image_range.virtual_base),
        .physical_base = .fromInt(image_range.base_paddr),
        .size_bytes = image_range.size_bytes,
    });
    @import("../../memory/pmm.zig").finishBootAccess() catch return false;
    boot_direct_map.rebaseScratch();
    kernel_cr3_value = pml4_pa;
    const features = directMapCpuFeatures();
    kernel_mmio_window = .{
        .root = &high_mmio_pdp_table,
        .first_pd = .{ .physical = high_pd0_pa, .words = &high_mmio_pd_tables[0] },
        .allocator = @import("../../memory/identity_allocator.zig").allocator(@import("../../kernel_runtime.zig").global_free_list),
        .physical_limit = @as(u64, 1) << @as(u6, @intCast(features.bits)),
    };
    refreshKernelCr3EntryValue();
    return true;
}

pub fn hardenKernelMappingsSupervisorOnly() void {
    // All roots are constructed supervisor-only. Changing bits here would
    // conceal a publication bug; reject it before any user root can inherit.
    for (pml4_table[0..256]) |entry| if (entry != 0) @panic("low kernel mapping");
    for (pml4_table[256..]) |entry| if (entry & page_user != 0) @panic("user kernel branch");
}

pub fn kernelPointerPaddr(addr: usize) ?u64 {
    return kernelVirtToPhys(@intCast(addr));
}

pub fn kernelAddressToPhysical(address: addresses.KernelVirtualAddress) ?addresses.PhysicalAddress {
    if (boot_direct_map.scratchPhysical(address.toInt())) |physical| return physical;
    if (cpu_runtime.physicalAddress(address)) |backing| return backing;
    return physical_layout.kernelAddressToPhysical(address, .{
        .virtual_base = .fromInt(image_range.virtual_base),
        .physical_base = .fromInt(image_range.base_paddr),
        .size_bytes = image_range.size_bytes,
    });
}

/// The caller's VM exclusion and table ownership keep this borrow alive.
/// Page reachability alone is not proof of a valid or non-user table.
pub fn borrowIdentityTable(address: addresses.PhysicalAddress) ?*const [512]u64 {
    return borrowIdentityTableMut(address);
}

/// Only the table owner or an excluded page-table writer may mutate this borrow.
/// Both borrow forms share alignment and complete-page reachability checks.
pub inline fn borrowIdentityTableMut(address: addresses.PhysicalAddress) ?*[512]u64 {
    return physical_layout.tablePointer(address);
}

pub fn seedUserPml4WithKernel(pml4: []u64) void {
    if (!kernelHighMappingActive()) return;
    const pml4_index: usize = @intCast((image_range.virtual_base >> 39) & 0x1FF);
    if (pml4_index >= pml4.len) return;
    const high_pdp_pa = kernelPtrPaddr(&high_kernel_pdp_table) orelse return;
    pml4[pml4_index] = high_pdp_pa | page_present | page_rw;
}

// The cache writer validates shared parent pointers without following an
// unrecognized table supplied by corrupted/stale address-space metadata.
pub fn ramCacheSharedPml4Entry(index: usize) ?u64 {
    if (!kernelHighMappingActive() or index != ((image_range.virtual_base >> 39) & 511)) return null;
    const pa = kernelPtrPaddr(&high_kernel_pdp_table) orelse return null;
    return pa | page_present | page_rw;
}

fn ramCacheTableLinkMatches(entry: u64, table: anytype) bool {
    const pa = kernelPtrPaddr(table) orelse return false;
    return (entry & ~@as(u64, 1 << 5)) == (pa | page_present | page_rw);
}

/// Prove the persistent roots before inspecting their known table arrays.
/// Metadata arrays alone do not prove that hardware uses those same parents.
/// This proves root links; lower split tables still need their ownership and
/// target-leaf checks before cache policy can be published.
/// The cache writer's VM exclusion keeps these roots stable; borrow the
/// arrays so validation does not copy aligned tables onto the caller's stack.
pub fn validateRamCacheKernelRoots() !void {
    const root = kernelPtrPaddr(&pml4_table) orelse return error.UnknownKernelRoot;
    if ((kernel_cr3_value & page_addr_mask) != root or
        (kernel_cr3_value & ~(page_addr_mask | @as(u64, 4095))) != 0)
        return error.UnknownKernelRoot;
    if (boot_direct_map.draft) |*draft| try draft.validateRoot(&pml4_table);
    for (&pml4_table, 0..) |entry, index| {
        if (boot_direct_map.draft != null and index >= direct_map.first_slot and index < direct_map.end_slot) continue;
        if (index == high_mmio_pml4_index) {
            if (!ramCacheTableLinkMatches(entry, &high_mmio_pdp_table)) return error.UnknownKernelRoot;
        } else if (kernelHighMappingActive() and index == ((image_range.virtual_base >> 39) & 511)) {
            if (!ramCacheTableLinkMatches(entry, &high_kernel_pdp_table)) return error.UnknownKernelRoot;
        } else if ((entry & page_present) != 0) return error.UnknownKernelRoot;
    }
    if (kernel_mmio_window) |*window| {
        try window.validate(kernelPtrPaddr(&phys_copy_window_pt) orelse return error.UnknownKernelRoot);
    } else return error.UnknownKernelRoot;
    if (kernelHighMappingActive()) {
        const first: usize = @intCast((image_range.virtual_base >> 30) & 511);
        for (&high_kernel_pdp_table, 0..) |entry, index| {
            if (boot_direct_map.scratchRootMatches(index, entry) or cpu_runtime.rootMatches(index, entry)) continue;
            if ((entry & page_present) == 0) continue;
            const slot = highKernelPdSlot(first, index) orelse return error.UnknownKernelRoot;
            if ((entry & ~@as(u64, 1 << 5)) != (high_kernel_pd_tables[slot].physical | page_present | page_rw)) return error.UnknownKernelRoot;
        }
    }
}

fn installTssDescriptor(cpu_slot: usize) bool {
    if (cpu_slot >= tss_tables.len) return false;
    const rsp0 = cpuKernelStackTop(cpu_slot) orelse return false;
    const ist1 = cpuPageFaultIstTop(cpu_slot) orelse return false;
    const ist2 = cpuDoubleFaultIstTop(cpu_slot) orelse return false;
    const tss = &tss_tables[cpu_slot];
    tss.* = std.mem.zeroes(Tss);
    tss.rsp0 = rsp0;
    tss.ist1 = ist1;
    tss.ist2 = ist2;
    tss.iomap_base = @sizeOf(Tss);
    kernel_syscall_stack_tops[cpu_slot] = rsp0;

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
    if (cpuHasRdtscp()) {
        writeMsr(msr_ia32_tsc_aux, @intCast(cpu_slot));
    }

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
        : .{ .memory = true, .rax = true });
    asm volatile (
        \\mov %[tss_sel], %%ax
        \\ltr %%ax
        :
        : [tss_sel] "i" (gdt_tss_selector),
        : .{ .memory = true, .rax = true });
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
    return gdt_tables[0][gdt_user_code_selector / 8];
}

pub fn userDataDescriptor() u64 {
    return gdt_tables[0][gdt_user_data_selector / 8];
}

pub fn gdtBase() u64 {
    return @intFromPtr(&gdt_tables[0]);
}

test "WC identity lease physical access table borrows share full-page validation" {
    for ([_]u64{ 0, 1, 4097, (@as(u64, 1) << 52), std.math.maxInt(u64) - 4095 }) |address| {
        try std.testing.expect(borrowIdentityTable(.fromInt(address)) == null);
        try std.testing.expect(borrowIdentityTableMut(.fromInt(address)) == null);
    }
    const last = (@as(u64, 1) << 52) - 4096;
    try std.testing.expectEqual(last, @intFromPtr(borrowIdentityTable(.fromInt(last)).?));
    try std.testing.expectEqual(last, @intFromPtr(borrowIdentityTableMut(.fromInt(last)).?));
}

const ScratchMappingFixture = struct {
    const va: u64 = 0xffff_ffff_9000_0000;
    const pa: u64 = two_mib + 4096;
    const unpublishedIdentity = identity;
    var virtual: u64 = va;
    var pde: u64 = 0;
    var highs: [3]u64 = undefined;
    var leaves: [512]u64 align(4096) = undefined;
    var capacity: usize = 0;
    var splits: usize = 0;
    fn reset() void {
        virtual = va;
        pde = two_mib | page_present | page_rw | page_ps;
        for (&highs, 0..) |*leaf, i| leaf.* = pa + i * 4096 | page_present | page_rw | page_nx;
        @memset(&leaves, 0);
        capacity = 1;
        splits = 0;
    }
    fn high(address: u64) ?*u64 {
        if (address < virtual or address - virtual >= highs.len * 4096) return null;
        return &highs[(address - virtual) / 4096];
    }
    fn directory(_: u64) *u64 {
        return &pde;
    }
    fn identity(address: u64) ?*u64 {
        if ((pde & page_ps) != 0) return null;
        return &leaves[(address >> 12) & 511];
    }
    fn split(_: u64) bool {
        const prepared = @import("../../memory/ram_cache_paging.zig").IdentitySplit.prepare(&pde, two_mib, &leaves, 4096) catch return false;
        prepared.publishEquivalent();
        splits += 1;
        return true;
    }
    fn remaining() usize {
        return capacity;
    }
};

test "WC identity lease boot scratch requires preexisting WB supervisor RW NX image leaves" {
    const F = ScratchMappingFixture;
    F.reset();
    const before = F.highs;
    try std.testing.expect(prepareBootScratchWith(F, F.va, F.pa, 3 * 4096));
    try std.testing.expectEqualSlices(u64, &before, &F.highs);
    for ([_]u64{ page_user, 8, 16, 256, page_nx }) |bad| {
        F.reset();
        F.highs[1] ^= bad;
        try std.testing.expect(!prepareBootScratchWith(F, F.va, F.pa, 3 * 4096));
    }
    F.reset();
    try std.testing.expect(!prepareBootScratchWith(F, F.va + 1, F.pa, 4096));
    try std.testing.expect(!prepareBootScratchWith(F, F.va, F.pa, 0));
    try std.testing.expect(!prepareBootScratchWith(F, F.va, F.pa, std.math.maxInt(usize)));
}

test "WC identity lease boot scratch withdrawal validates whole range before removing only the tail" {
    const F = ScratchMappingFixture;
    F.reset();
    F.highs[2] &= ~page_nx;
    try std.testing.expect(!withdrawBootScratchWith(F, F.va + 4096, F.pa + 4096, 8192));
    try std.testing.expect(F.highs[1] != 0);
    F.highs[2] |= page_nx;
    try std.testing.expect(withdrawBootScratchWith(F, F.va + 4096, F.pa + 4096, 8192));
    try std.testing.expectEqual(F.pa | page_present | page_rw | page_nx, F.highs[0]);
    try std.testing.expectEqual(@as(u64, 0), F.highs[1] | F.highs[2]);
    try std.testing.expect(!withdrawBootScratchWith(F, F.va + 4096, F.pa + 4096, 8192));
}

// Host-only physical-table adapter: admission and publication use the same
// production algorithms, while privileged CR3/TLB operations stay outside it.
pub const ScratchReturnTest = if (@import("builtin").is_test) struct {
    pub const pa = ScratchMappingFixture.pa;
    pub const tail = pa + 4096;
    var saved_mapping: BootScratchMapping = undefined;
    var saved_va: u64 = 0;
    var saved_pa: u64 = 0;
    var saved_size: usize = 0;
    var saved_root: [512]u64 = undefined;
    var saved_high: [512]u64 = undefined;
    var saved_io: [512]u64 = undefined;
    var saved_io_pd: [512]u64 = undefined;
    var saved_cr3: u64 = 0;
    var saved_window: @TypeOf(kernel_mmio_window) = undefined;
    var saved_draft: @TypeOf(boot_direct_map.draft) = undefined;
    var saved_layout: @TypeOf(boot_direct_map.layout) = undefined;
    var direct_pdp: [512]u64 align(4096) = @splat(0);
    var direct_pd: [512]u64 align(4096) = @splat(0);
    var records: [3]direct_map.Table = undefined;
    pub fn begin() !void {
        saved_mapping = boot_scratch_mapping;
        saved_va = image_range.virtual_base;
        saved_pa = image_range.base_paddr;
        saved_size = image_range.size_bytes;
        image_range.virtual_base = ScratchMappingFixture.va;
        image_range.base_paddr = pa;
        image_range.size_bytes = 4 * 4096;
        ScratchMappingFixture.reset();
        saved_root = pml4_table;
        saved_high = high_kernel_pdp_table;
        saved_io = high_mmio_pdp_table;
        saved_io_pd = high_mmio_pd_tables[0];
        saved_cr3 = kernel_cr3_value;
        saved_window = kernel_mmio_window;
        saved_draft = boot_direct_map.draft;
        saved_layout = boot_direct_map.layout;
        @memset(&pml4_table, 0);
        @memset(&high_kernel_pdp_table, 0);
        @memset(&high_mmio_pdp_table, 0);
        @memset(&high_mmio_pd_tables[0], 0);
        @memset(&direct_pdp, 0);
        @memset(&direct_pd, 0);
        const flags = page_nx | 3;
        high_mmio_pdp_table[0] = @intFromPtr(&high_mmio_pd_tables[0]) | flags;
        high_mmio_pd_tables[0][0] = @intFromPtr(&phys_copy_window_pt) | 3;
        pml4_table[510] = @intFromPtr(&high_mmio_pdp_table) | 3;
        pml4_table[511] = @intFromPtr(&high_kernel_pdp_table) | 3;
        kernel_cr3_value = @intFromPtr(&pml4_table);
        kernel_mmio_window = .{ .root = &high_mmio_pdp_table, .first_pd = .{ .physical = @intFromPtr(&high_mmio_pd_tables[0]), .words = &high_mmio_pd_tables[0] }, .allocator = undefined, .physical_limit = @as(u64, 1) << 52 };
        records = .{
            .{ .physical = @intFromPtr(&direct_pdp), .words = &direct_pdp },
            .{ .physical = @intFromPtr(&direct_pd), .words = &direct_pd },
            .{ .physical = @intFromPtr(&ScratchMappingFixture.leaves), .words = &ScratchMappingFixture.leaves },
        };
        direct_pdp[0] = records[1].physical | flags;
        direct_pd[1] = records[2].physical | flags;
        boot_direct_map.draft = .{ .tables = &records, .published = true };
        boot_direct_map.draft.?.roots[0] = records[0].physical | flags;
        pml4_table[256] = records[0].physical | flags;
        boot_direct_map.layout = try direct_map.Layout.init(&.{}, &.{}, 48, true);
        if (!prepareBootScratchWith(ScratchMappingFixture, ScratchMappingFixture.va, pa, 3 * 4096)) return error.InvalidState;
        boot_scratch_mapping = .{ .start = ScratchMappingFixture.va, .bytes = 3 * 4096 };
    }
    pub fn end() void {
        boot_scratch_mapping = saved_mapping;
        pml4_table = saved_root;
        high_kernel_pdp_table = saved_high;
        high_mmio_pdp_table = saved_io;
        high_mmio_pd_tables[0] = saved_io_pd;
        kernel_cr3_value = saved_cr3;
        kernel_mmio_window = saved_window;
        boot_direct_map.draft = saved_draft;
        boot_direct_map.layout = saved_layout;
        image_range.virtual_base = saved_va;
        image_range.base_paddr = saved_pa;
        image_range.size_bytes = saved_size;
    }
    pub fn withdraw() !void {
        const va = ScratchMappingFixture.va + 4096;
        if (!withdrawBootScratchWith(ScratchMappingFixture, va, tail, 8192)) return error.InvalidState;
        boot_scratch_mapping.retired = true;
        boot_scratch_mapping.retired_start = va;
        boot_scratch_mapping.retired_bytes = 8192;
    }
    pub fn ack() !void {
        if (!boot_scratch_mapping.acknowledge(ScratchMappingFixture.va + 4096, 8192)) return error.InvalidState;
    }
    pub fn giveBack(list: anytype) !void {
        try returnBootScratchWith(ScratchMappingFixture, &boot_scratch_mapping, list, tail, 2);
    }
    pub fn leaf(address: u64) *u64 {
        return &ScratchMappingFixture.leaves[(address >> 12) & 511];
    }
} else struct {};

test "WC identity lease scratch RAM exception requires ACK and successful PMM return and rejects unsplit or high aliases" {
    const T = ScratchReturnTest;
    const Source = @import("../../memory/ram_cache_paging.zig").AliasSource;
    const Probe = struct {
        calls: usize = 0,
        fail: bool = true,
        pub fn appendContiguousRange(self: *@This(), address: u64, pages: usize) !void {
            try std.testing.expectEqual(T.tail, address);
            try std.testing.expectEqual(@as(usize, 2), pages);
            try std.testing.expectEqual(@as(u64, 0), @atomicLoad(u64, &boot_scratch_mapping.returned_start, .acquire));
            self.calls += 1;
            if (self.fail) return error.NoSpace;
        }
        fn visit(_: *@This(), source: Source) !void {
            try std.testing.expect(source == .leaf);
        }
    };
    try T.begin();
    defer T.end();
    var probe: Probe = .{};
    for ([_]u64{T.tail}) |pa| try std.testing.expect(!ramCacheImagePageAllowed(pa));
    try std.testing.expectError(error.InvalidState, T.giveBack(&probe));
    try T.withdraw();
    try std.testing.expectError(error.InvalidState, T.giveBack(&probe));
    try std.testing.expectEqual(@as(usize, 0), probe.calls);
    try T.ack();
    try std.testing.expectError(error.NoSpace, T.giveBack(&probe));
    for ([_]u64{T.tail}) |pa| try std.testing.expect(!ramCacheImagePageAllowed(pa));
    probe.fail = false;
    try T.giveBack(&probe);
    try std.testing.expect(ramCacheImagePageAllowed(T.tail) and ramCacheImagePageAllowed(T.tail + 4096));
    try std.testing.expectError(error.InvalidState, T.giveBack(&probe));
    try std.testing.expectEqual(@as(usize, 2), probe.calls);
    for ([_]u64{ T.pa, T.pa + 3 * 4096 }) |neighbor|
        for ([_]u64{neighbor}) |pa| try std.testing.expect(!ramCacheImagePageAllowed(pa));
    try std.testing.expect(!ramCacheImagePageAllowed(T.pa));
    ScratchMappingFixture.highs[1] = T.tail | page_present;
    for ([_]u64{T.tail}) |pa| try std.testing.expect(!ramCacheImagePageAllowed(pa));
}

test "BSP stack CPU identity guards withdraw only selected high image leaves" {
    const F = ScratchMappingFixture;
    // Both ends of the real BSP/kernel and IST regions must preserve the
    // adjacent image/body leaves, including their distinct permissions.
    const raws = [_][]u8{ ring0_stack_region_raw[0..], pf_ist_stack_region_raw[0..], df_ist_stack_region_raw[0..] };
    for (raws, bsp_stack_bytes) |raw, bytes| {
        const region = alignedStackRegion(raw);
        const bottom = stackBottom(region, bytes);
        const top = stackTop(region, bytes);
        try std.testing.expectEqual(@as(u64, bytes), top - bottom);
        for ([_]u64{ bottom - guard_page_bytes, top }) |guard| {
            F.reset();
            F.virtual = guard - guard_page_bytes;
            F.highs[0] &= ~page_rw;
            const before = F.highs;
            try std.testing.expect(protectBspStackGuardWith(F, guard, F.pa + 4096));
            try std.testing.expectEqual(before[0], F.highs[0]);
            try std.testing.expectEqual(@as(u64, 0), F.highs[1]);
            try std.testing.expectEqual(before[2], F.highs[2]);
            F.highs = before;
            F.highs[1] |= page_user;
            const rejected = F.highs;
            try std.testing.expect(!protectBspStackGuardWith(F, guard, F.pa + 4096));
            try std.testing.expectEqualSlices(u64, &rejected, &F.highs);
        }
    }
}

test "BSP stack CPU identity TSS descriptors use exact new BSP and AP tops" {
    const saved_backing = ap_stack_backing;
    const saved_tss = tss_tables;
    const saved_gdt = gdt_tables;
    const saved_tops = kernel_syscall_stack_tops;
    defer {
        ap_stack_backing = saved_backing;
        tss_tables = saved_tss;
        gdt_tables = saved_gdt;
        kernel_syscall_stack_tops = saved_tops;
    }
    var test_backing: [512]u64 = @splat(4096);
    var test_tss: [513]Tss = @splat(std.mem.zeroes(Tss));
    var test_gdt: [513][7]u64 = @splat(gdt_template);
    var test_tops: [513]u64 = @splat(0);
    ap_stack_backing = &test_backing;
    tss_tables = &test_tss;
    gdt_tables = &test_gdt;
    kernel_syscall_stack_tops = &test_tops;
    for (0..tss_tables.len) |cpu| {
        try std.testing.expect(installTssDescriptor(cpu));
        try std.testing.expectEqual(cpuKernelStackTop(cpu).?, tss_tables[cpu].rsp0);
        try std.testing.expectEqual(cpuPageFaultIstTop(cpu).?, tss_tables[cpu].ist1);
        try std.testing.expectEqual(cpuDoubleFaultIstTop(cpu).?, tss_tables[cpu].ist2);
        try std.testing.expectEqual(tss_tables[cpu].rsp0, kernel_syscall_stack_tops[cpu]);
        if (cpu != 0) {
            try std.testing.expectEqual(cpu_stacks.stackBase(cpu, 0).? + 512 * 1024, tss_tables[cpu].rsp0);
            try std.testing.expectEqual(cpu_stacks.stackBase(cpu, 1).? + 128 * 1024, tss_tables[cpu].ist1);
            try std.testing.expectEqual(cpu_stacks.stackBase(cpu, 2).? + 256 * 1024, tss_tables[cpu].ist2);
        }
    }
}

pub const MmioWindow = struct {
    const Window = @This();
    const nx = page_nx;
    const mask = page_addr_mask;
    const pt = @import("../../memory/pt_storage.zig");
    const dm = direct_map;
    const window_base = @import("../../memory/mmio.zig").window_base;
    const window_bytes = @import("../../memory/mmio.zig").window_bytes;
    root: *[512]u64,
    first_pd: dm.Table,
    allocator: pt.Allocator,
    physical_limit: u64,
    storage: pt.Store = .{},
    records: ?*Record = null,
    next_va: u64 = window_base + (1 << 21),
    sealed: bool = false,

    const Record = struct {
        next: ?*Record,
        physical: u64,
        bytes: u64,
        virtual: u64,
        flags: u64,
        ready: bool = false,
    };

    fn page(self: *Window, table_page: bool) !dm.Table {
        const slot = self.storage.firstFreeSlot() orelse return error.OutOfMemory;
        const words = self.storage.ensurePage(slot, self.allocator) orelse return error.OutOfMemory;
        const descriptor = self.storage.descriptor(slot).?;
        descriptor.pml4 = if (table_page) 510 else 511;
        return .{ .physical = descriptor.allocation.physical.toInt(), .words = words };
    }

    fn borrow(self: *Window, pa: u64) !*[512]u64 {
        if (pa == self.first_pd.physical) return self.first_pd.words;
        var it = self.storage.iterator();
        while (it.next()) |slot| {
            const descriptor = self.storage.descriptor(slot).?;
            if (descriptor.pml4 == 510 and descriptor.allocation.physical.toInt() == pa) return descriptor.page.?;
        }
        return error.UnknownTable;
    }

    pub fn rangeFits(self: *const Window, physical: u64, bytes: u64) bool {
        return physical != 0 and bytes != 0 and physical < self.physical_limit and bytes <= self.physical_limit - physical;
    }

    pub fn lookup(self: *const Window, physical: u64, bytes: u64, flags: u64) ?u64 {
        if (!self.rangeFits(physical, bytes)) return null;
        var current = self.records;
        while (current) |record| : (current = record.next) {
            if (record.ready and record.flags == flags and physical >= record.physical and
                bytes <= record.bytes -| (physical - record.physical))
                return record.virtual + (physical - record.physical);
        }
        return null;
    }

    /// Serial boot writer only. Published pointers stay valid until shutdown.
    /// Seal before user dispatch: runtime leases cannot race new cache aliases.
    /// On OOM, retain the owned, unreachable VA reservation and its tables;
    /// retry completes that same record without exposing a partial Borrow.
    pub fn map(self: *Window, physical: u64, bytes: u64, flags: u64) !u64 {
        if (!self.rangeFits(physical, bytes) or flags & (1 | nx) != (1 | nx) or flags & ~(nx | 0x1b) != 0)
            return error.InvalidRange;
        if (self.lookup(physical, bytes, flags)) |va| return va;
        if (self.sealed) return error.Sealed;
        const start = physical & ~@as(u64, 4095);
        const end = (try std.math.add(u64, physical + bytes, 4095)) & ~@as(u64, 4095);
        var current = self.records;
        var pending: ?*Record = null;
        while (current) |record| : (current = record.next) {
            if (start < record.physical + record.bytes and record.physical < end and record.flags != flags)
                return error.CacheConflict;
            if (!record.ready and start == record.physical and end - start == record.bytes and flags == record.flags) pending = record;
        }
        const record = pending orelse blk: {
            if (end - start > window_base + window_bytes - self.next_va) return error.NoVirtualSpace;
            const backing = try self.page(false);
            const result: *Record = @ptrCast(backing.words);
            result.* = .{ .next = self.records, .physical = start, .bytes = end - start, .virtual = self.next_va, .flags = flags };
            self.next_va += end - start;
            self.records = result;
            break :blk result;
        };
        var offset: u64 = 0;
        while (offset < record.bytes) : (offset += 4096) {
            const va = record.virtual + offset;
            var table = self.root;
            for ([_]u6{ 30, 21 }) |shift| {
                const link = &table[(va >> shift) & 511];
                if (link.* == 0) {
                    const child = try self.page(true);
                    @atomicStore(u64, link, child.physical | 3 | nx, .release);
                }
                if (link.* & ~(mask | 0x60) != (3 | nx)) return error.UnknownTable;
                table = try self.borrow(link.* & mask);
            }
            const leaf = &table[(va >> 12) & 511];
            const value = (start + offset) | flags;
            if (leaf.* != 0 and leaf.* & ~@as(u64, 0x60) != value) return error.UnknownTable;
            @atomicStore(u64, leaf, value, .release);
        }
        record.ready = true;
        return record.virtual + (physical - start);
    }

    /// Root ownership alone is insufficient: reject grafted branches and
    /// leaves outside recorded extents, including during an OOM retry.
    pub fn validate(self: *Window, copy_pt: u64) !void {
        for (self.root, 0..) |pdp, pi| {
            if (pdp == 0) continue;
            if (pdp & ~(mask | 0x60) != (3 | nx)) return error.UnknownTable;
            const pd = try self.borrow(pdp & mask);
            for (pd, 0..) |directory, di| {
                if (pi == 0 and di == 0) {
                    if (directory & ~@as(u64, 0x60) != (copy_pt | 3)) return error.UnknownTable;
                    continue;
                }
                if (directory == 0) continue;
                if (directory & ~(mask | 0x60) != (3 | nx)) return error.UnknownTable;
                const leaves = try self.borrow(directory & mask);
                for (leaves, 0..) |leaf, ti| {
                    if (leaf == 0) continue;
                    const va = window_base + (pi << 30) + (di << 21) + (ti << 12);
                    var record = self.records;
                    var found = false;
                    while (record) |r| : (record = r.next) {
                        if (va < r.virtual or va - r.virtual >= r.bytes) continue;
                        if (leaf & ~@as(u64, 0x60) != ((r.physical + (va - r.virtual)) | r.flags)) return error.UnknownTable;
                        found = true;
                        break;
                    }
                    if (!found) return error.UnknownTable;
                }
            }
        }
        if (self.root[0] & ~@as(u64, 0x60) != (self.first_pd.physical | 3 | nx)) return error.UnknownTable;
    }

    /// Live kernel register/firmware borrowers cannot be withdrawn behind
    /// their owners. A conflicting WC grant must fail before publication.
    pub fn overlaps(self: *const Window, physical: u64, bytes: u64) bool {
        if (!self.rangeFits(physical, bytes)) return true;
        var current = self.records;
        while (current) |record| : (current = record.next) {
            if (physical < record.physical + record.bytes and record.physical < physical + bytes) return true;
        }
        return false;
    }

    pub fn deviceCacheAllowed(self: *const Window, physical: u64, bytes: u64, wc: bool) bool {
        if (!self.rangeFits(physical, bytes)) return false;
        var current = self.records;
        while (current) |record| : (current = record.next) {
            if (physical < record.physical + record.bytes and record.physical < physical + bytes and
                (wc or record.flags & 0x18 != 0x18)) return false;
        }
        return true;
    }
};

test "WC identity lease root validation rejects low branches and user propagation" {
    try ScratchReturnTest.begin();
    defer ScratchReturnTest.end();
    try validateRamCacheKernelRoots();
    pml4_table[0] = pml4_table[511];
    try std.testing.expectError(error.UnknownKernelRoot, validateRamCacheKernelRoots());
    pml4_table[0] = 0;
    for ([_]usize{ 256, 510, 511 }) |index| {
        pml4_table[index] |= page_user;
        try std.testing.expectError(if (index == 256) error.InvalidTable else error.UnknownKernelRoot, validateRamCacheKernelRoots());
        pml4_table[index] &= ~page_user;
    }
    // Poison PA is noncanonical as a host pointer: following it would fault.
    // Exact root ownership must reject it before any table dereference.
    const direct_before = ScratchReturnTest.direct_pd;
    const leaves_before = ScratchMappingFixture.leaves;
    const high_before = ScratchMappingFixture.highs;
    for ([_]usize{ 256, 510, 511 }) |index| {
        const saved = pml4_table[index];
        pml4_table[index] = 0x000f_ffff_ffff_f000 | (saved & ~page_addr_mask);
        const root_before = pml4_table;
        try std.testing.expectError(if (index == 256) error.InvalidTable else error.UnknownKernelRoot, validateRamCacheKernelRoots());
        try std.testing.expectEqualSlices(u64, &root_before, &pml4_table);
        try std.testing.expectEqualSlices(u64, &direct_before, &ScratchReturnTest.direct_pd);
        try std.testing.expectEqualSlices(u64, &leaves_before, &ScratchMappingFixture.leaves);
        try std.testing.expectEqualSlices(u64, &high_before, &ScratchMappingFixture.highs);
        pml4_table[index] = saved;
    }
    try validateRamCacheKernelRoots();
}

test "WC identity lease AP startup root owns only one low path and shared image" {
    const F = struct {
        var backing: [2048]u64 align(4096) = @splat(0);
        fail: bool = false,
        calls: usize = 0,
        pub fn popContiguousInRange(self: *@This(), count: usize, first: u64, end: u64) error{NoSpace}!u64 {
            std.debug.assert(count == 4 and first == 4096 and end == four_gib);
            self.calls += 1;
            if (self.fail) return error.NoSpace;
            std.debug.assert(@intFromPtr(&backing) + @sizeOf(@TypeOf(backing)) <= four_gib);
            return @intFromPtr(&backing);
        }
    };
    const saved_tables = ap_boot_tables;
    const saved_pa = ap_boot_physical;
    const saved_trampoline = ap_boot_trampoline;
    const saved_high = pml4_table[511];
    defer {
        ap_boot_tables = saved_tables;
        ap_boot_physical = saved_pa;
        ap_boot_trampoline = saved_trampoline;
        pml4_table[511] = saved_high;
    }
    ap_boot_tables = null;
    pml4_table[511] = 0x200003;
    var allocator = F{ .fail = true };
    try std.testing.expect(prepareApBootRoot(0x8000, &allocator) == null);
    try std.testing.expect(ap_boot_tables == null);
    allocator.fail = false;
    const root = prepareApBootRoot(0x8000, &allocator).?;
    try std.testing.expect(root < four_gib);
    try std.testing.expectEqual(root + 4096 | 3, F.backing[0]);
    try std.testing.expectEqual(@as(u64, 0x200003), F.backing[511]);
    for (F.backing[1..511]) |entry| try std.testing.expectEqual(@as(u64, 0), entry);
    for (F.backing[1536..]) |entry| try std.testing.expectEqual(@as(u64, 0), entry);
    try std.testing.expect(prepareApBootRoot(0x8000, &allocator) == null);
    try std.testing.expectEqual(@as(usize, 2), allocator.calls);
}

// The runtime island is shared through the existing supervisor image root.
// Only its BSP owner creates mappings, before any AP or user can observe them.
pub fn runtimePdp() *[512]u64 {
    return &high_kernel_pdp_table;
}
pub fn runtimeRoot() *[512]u64 {
    return &pml4_table;
}
var cpu_window_leaves: []*u64 = &.{};
var cpu_window_base: u64 = 0;
pub fn perCpuWindowEntry(cpu: usize, way: usize) *u64 {
    if (cpu_window_leaves.len == 0) {
        std.debug.assert(cpu == 0);
        return &phys_copy_window_pt[way];
    }
    return cpu_window_leaves[cpu * phys_copy_window_ways + way];
}
pub fn perCpuWindowVa(cpu: usize, way: usize) u64 {
    if (cpu_window_leaves.len == 0) {
        std.debug.assert(cpu == 0);
        return phys_copy_window_va + way * 4096;
    }
    return cpu_window_base + (cpu * phys_copy_window_ways + way) * 4096;
}
pub fn initCpuStorage(count: usize) !void {
    const optimized = try cpu_runtime.alloc(bool, count);
    @memset(optimized, false);
    optimized[0] = xsaveopt_by_cpu[0];
    xsaveopt_by_cpu = optimized;
    // Bootstrap copies used the original diagnostic PT. Withdraw its ways
    // before switching owners; slot 511 remains the separate diagnostic lease.
    for (0..phys_copy_window_ways) |way| {
        phys_copy_window_pt[way] = 0;
        invlpg(phys_copy_window_va + way * 4096);
    }
    const stack_bytes = try std.math.mul(usize, count - 1, cpu_stacks.stride);
    cpu_stacks.base = try cpu_runtime.reserve(stack_bytes, cpu_stacks.stride);
    ap_stack_backing = try cpu_runtime.alloc(u64, count - 1);
    const gdts = try cpu_runtime.alloc([7]u64, count);
    @memset(gdts, gdt_template);
    gdts[0] = gdt_tables[0];
    gdt_tables = gdts;
    const tss = try cpu_runtime.alloc(Tss, count);
    tss[0] = tss_tables[0];
    tss_tables = tss;
    const tops = try cpu_runtime.alloc(u64, count);
    tops[0] = kernel_syscall_stack_tops[0];
    kernel_syscall_stack_tops = tops.ptr;
    const support = try cpu_runtime.alloc(u8, count);
    support[0] = invpcid_support[0];
    invpcid_support = support;
    const slots = try std.math.mul(usize, count, phys_copy_window_ways);
    cpu_window_leaves = try cpu_runtime.alloc(*u64, slots);
    cpu_window_base = try cpu_runtime.reserve(try std.math.mul(usize, slots, 4096), 4096);
    for (cpu_window_leaves, 0..) |*entry, i| entry.* = try cpu_runtime.leaf(cpu_window_base + i * 4096);
    // Reload TR/GDTR after migration; the descriptor must name the new TSS,
    // even though the bootstrap stack itself keeps its original image pages.
    if (!loadGdtAndReloadSegmentsForCpu(0)) return error.InvalidBootstrapTss;
}

test "SYSRET has one user descriptor pair and rejects obsolete selectors" {
    try std.testing.expectEqual(@as(usize, 7), gdt_template.len);
    try std.testing.expectEqual(@as(u64, 0x00CFF2000000FFFF), gdt_template[3]);
    try std.testing.expectEqual(@as(u64, 0x00AFFA000000FFFF), gdt_template[4]);
    try std.testing.expectEqual(gdt_user_data_selector + 8, gdt_user_code_selector);
    try std.testing.expectEqual(gdt_template[4], userCodeDescriptor());
    try std.testing.expectEqual(gdt_template[3], userDataDescriptor());
    try std.testing.expectEqual(@as(u16, 0x28), gdt_tss_selector);
    try std.testing.expect(validUserSelectors(0x23, 0x1b));
    try std.testing.expect(!validUserSelectors(0x43, 0x3b));
    try std.testing.expect(!validUserSelectors(0x1b, 0x23));
    try std.testing.expect(!validUserSelectors(0x43, 0x23));
    try std.testing.expect(!validUserSelectors(0x08, 0x10));
}
