const std = @import("std");
const interrupts = @import("../../interrupts.zig");
const image_range = @import("../../boot/image_range.zig");
const xstate_format = @import("xstate.zig");
const physical_layout = @import("physical_layout.zig");
const cpu_stacks = @import("cpu_stack_layout.zig");

// CPU affinity, online membership and TLB shootdown targets use u64 masks.
// Support their full capacity; exceeding this requires a bitmap ABI redesign.
pub const max_cpus: usize = cpu_stacks.max_cpus;
pub const page_entries: usize = 512;
pub const two_mib: u64 = 2 * 1024 * 1024;
pub const four_gib: u64 = 4 * 1024 * 1024 * 1024;
pub const one_tib: u64 = 1024 * 1024 * 1024 * 1024;
pub const pd_table_count: usize = physical_layout.identity_pdp_entries;
pub const high_mmio_pml4_index: usize = 1;
pub const high_mmio_pdp_table_count: usize = page_entries;
pub const guard_page_bytes: usize = 4096;
pub const stack_region_bytes: usize = 8 * 1024 * 1024;
pub const stack_region_align: u64 = 2 * 1024 * 1024;
pub const stack_region_raw_bytes: usize = stack_region_bytes + @as(usize, @intCast(stack_region_align));
pub const stack_region_chunk_count: usize = stack_region_bytes / @as(usize, @intCast(two_mib));
pub const ring0_stack_bytes: usize = stack_region_bytes - (2 * guard_page_bytes);
pub const ist_stack_bytes: usize = 2 * 1024 * 1024;
const runtime_identity_split_pt_count: usize = 256;
const high_kernel_pd_table_count: usize = 4;
const high_kernel_pt_table_count: usize = 128;
pub const phys_copy_window_va: u64 = (@as(u64, @intCast(high_mmio_pml4_index)) << 39);

pub const gdt_kernel_code_selector: u16 = 0x08;
pub const gdt_kernel_data_selector: u16 = 0x10;
pub const gdt_user_code_selector: u16 = 0x18;
pub const gdt_user_data_selector: u16 = 0x20;
pub const gdt_tss_selector: u16 = 0x28;

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
var pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var pd_tables: [pd_table_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** pd_table_count;
var high_mmio_pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var high_mmio_pd_tables: [high_mmio_pdp_table_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** high_mmio_pdp_table_count;
var high_kernel_pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var high_kernel_pd_tables: [high_kernel_pd_table_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** high_kernel_pd_table_count;
var high_kernel_pt_tables: [high_kernel_pt_table_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** high_kernel_pt_table_count;
pub var phys_copy_window_pt: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var idt: [256]interrupts.IdtEntry align(16) = [_]interrupts.IdtEntry{interrupts.zeroIdtEntry()} ** 256;
const gdt_template: [7]u64 = .{
    0x0000000000000000,
    0x00AF9A000000FFFF,
    0x00AF92000000FFFF,
    0x00AFFA000000FFFF,
    0x00CFF2000000FFFF,
    0x0000000000000000,
    0x0000000000000000,
};
var gdt_tables: [max_cpus][7]u64 align(16) = [_][7]u64{gdt_template} ** max_cpus;
var ring0_stack_region_raw: [stack_region_raw_bytes]u8 align(4096) = [_]u8{0} ** stack_region_raw_bytes;
var pf_ist_stack_region_raw: [stack_region_raw_bytes]u8 align(4096) = [_]u8{0} ** stack_region_raw_bytes;
var df_ist_stack_region_raw: [stack_region_raw_bytes]u8 align(4096) = [_]u8{0} ** stack_region_raw_bytes;
var ap_stack_backing: [max_cpus - 1]u64 = [_]u64{0} ** (max_cpus - 1);
var ap_stack_pts: [max_cpus - 1][2][page_entries]u64 align(4096) = std.mem.zeroes([max_cpus - 1][2][page_entries]u64);
var ring0_stack_guard_pt: [stack_region_chunk_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** stack_region_chunk_count;
var pf_ist_stack_guard_pt: [stack_region_chunk_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** stack_region_chunk_count;
var df_ist_stack_guard_pt: [stack_region_chunk_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** stack_region_chunk_count;
var runtime_identity_split_pts: [runtime_identity_split_pt_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** runtime_identity_split_pt_count;
var runtime_identity_split_pt_used: usize = 0;
var tss_tables: [max_cpus]Tss = [_]Tss{std.mem.zeroes(Tss)} ** max_cpus;
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
pub export var kernel_syscall_stack_top: u64 = 0;
pub export var kernel_syscall_stack_tops: [max_cpus]u64 = [_]u64{0} ** max_cpus;
pub export var pcid_enabled: u64 = 0;
pub export var pku_enabled: u64 = 0;
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
    start = minStaticStart(start, staticStorageStart(@TypeOf(pdp_table), &pdp_table));
    start = minStaticStart(start, staticStorageStart(@TypeOf(pd_tables), &pd_tables));
    start = minStaticStart(start, staticStorageStart(@TypeOf(high_mmio_pdp_table), &high_mmio_pdp_table));
    start = minStaticStart(start, staticStorageStart(@TypeOf(high_mmio_pd_tables), &high_mmio_pd_tables));
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
    start = minStaticStart(start, staticStorageStart(@TypeOf(ap_stack_pts), &ap_stack_pts));
    start = minStaticStart(start, staticStorageStart(@TypeOf(ring0_stack_guard_pt), &ring0_stack_guard_pt));
    start = minStaticStart(start, staticStorageStart(@TypeOf(pf_ist_stack_guard_pt), &pf_ist_stack_guard_pt));
    start = minStaticStart(start, staticStorageStart(@TypeOf(df_ist_stack_guard_pt), &df_ist_stack_guard_pt));
    start = minStaticStart(start, staticStorageStart(@TypeOf(runtime_identity_split_pts), &runtime_identity_split_pts));
    start = minStaticStart(start, staticStorageStart(@TypeOf(runtime_identity_split_pt_used), &runtime_identity_split_pt_used));
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
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_syscall_stack_top), &kernel_syscall_stack_top));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_syscall_stack_tops), &kernel_syscall_stack_tops));
    start = minStaticStart(start, staticStorageStart(@TypeOf(pcid_enabled), &pcid_enabled));
    start = minStaticStart(start, staticStorageStart(@TypeOf(pku_enabled), &pku_enabled));
    start = minStaticStart(start, staticStorageStart(@TypeOf(x86_rdtscp_supported), &x86_rdtscp_supported));
    return start;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(pml4_table), &pml4_table));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(pdp_table), &pdp_table));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(pd_tables), &pd_tables));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(high_mmio_pdp_table), &high_mmio_pdp_table));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(high_mmio_pd_tables), &high_mmio_pd_tables));
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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ap_stack_pts), &ap_stack_pts));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ring0_stack_guard_pt), &ring0_stack_guard_pt));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(pf_ist_stack_guard_pt), &pf_ist_stack_guard_pt));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(df_ist_stack_guard_pt), &df_ist_stack_guard_pt));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(runtime_identity_split_pts), &runtime_identity_split_pts));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(runtime_identity_split_pt_used), &runtime_identity_split_pt_used));
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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_syscall_stack_top), &kernel_syscall_stack_top));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_syscall_stack_tops), &kernel_syscall_stack_tops));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(pcid_enabled), &pcid_enabled));
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
const cr4_pge: u64 = 1 << 7;
const cr4_osfxsr: u64 = 1 << 9;
const cr4_osxmmexcpt: u64 = 1 << 10;
const cr4_pcide: u64 = 1 << 17;
const cr4_osxsave: u64 = 1 << 18;
const cr4_pke: u64 = 1 << 22;
const cpuid_leaf1_ecx_pcid: u32 = 1 << 17;
const cpuid_leaf1_ecx_xsave: u32 = 1 << 26;
const cpuid_leaf1_ecx_osxsave: u32 = 1 << 27;
const cpuid_leaf1_ecx_avx: u32 = 1 << 28;
const cpuid_leaf7_ecx_pku: u32 = 1 << 3;
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

pub fn readFsBase() u64 {
    return readMsr(msr_ia32_fs_base);
}

pub fn writeFsBase(value: u64) void {
    writeMsr(msr_ia32_fs_base, value);
}

pub fn readGsBase() u64 {
    return readMsr(msr_ia32_gs_base);
}

pub fn writeGsBase(value: u64) void {
    writeMsr(msr_ia32_gs_base, value);
}

pub fn enableSyscallEntry(entry: usize) void {
    const star = (@as(u64, gdt_kernel_code_selector) << 32);
    writeMsr(msr_ia32_star, star);
    writeMsr(msr_ia32_lstar, @intCast(entry));
    writeMsr(msr_ia32_fmask, rflags_if);
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
    return @intFromPtr(page);
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
    return raw[start .. start + stack_region_bytes];
}

pub fn cpuKernelStackTop(cpu_slot: usize) ?u64 {
    if (cpu_slot == 0) return stackTop(alignedStackRegion(ring0_stack_region_raw[0..]), ring0_stack_bytes);
    if (cpu_slot >= max_cpus) return null;
    if (ap_stack_backing[cpu_slot - 1] == 0) return null;
    return cpu_stacks.stackBase(cpu_slot, 0).? + cpu_stacks.kernel_bytes;
}

pub fn cpuSlotForStackPointer(rsp: u64) ?usize {
    const bsp_region = alignedStackRegion(ring0_stack_region_raw[0..]);
    const bsp_base = @intFromPtr(bsp_region.ptr) + guard_page_bytes;
    const bsp_end = bsp_base + ring0_stack_bytes;
    if (rsp >= bsp_base and rsp <= bsp_end) return 0;

    const bsp_pf_region = alignedStackRegion(pf_ist_stack_region_raw[0..]);
    const bsp_pf_base = @intFromPtr(bsp_pf_region.ptr) + guard_page_bytes;
    const bsp_pf_end = bsp_pf_base + ist_stack_bytes;
    if (rsp >= bsp_pf_base and rsp <= bsp_pf_end) return 0;

    const bsp_df_region = alignedStackRegion(df_ist_stack_region_raw[0..]);
    const bsp_df_base = @intFromPtr(bsp_df_region.ptr) + guard_page_bytes;
    const bsp_df_end = bsp_df_base + ist_stack_bytes;
    if (rsp >= bsp_df_base and rsp <= bsp_df_end) return 0;

    var cpu_slot: usize = 1;
    while (cpu_slot < max_cpus) : (cpu_slot += 1) {
        if (ap_stack_backing[cpu_slot - 1] == 0) continue;
        for (cpu_stacks.sizes, 0..) |bytes, stack| {
            const base = cpu_stacks.stackBase(cpu_slot, stack).?;
            if (rsp >= base and rsp <= base + bytes) return cpu_slot;
        }
    }
    return null;
}

fn cpuPageFaultIstTop(cpu_slot: usize) ?u64 {
    if (cpu_slot == 0) return stackTop(alignedStackRegion(pf_ist_stack_region_raw[0..]), ist_stack_bytes);
    if (cpu_slot >= max_cpus) return null;
    if (ap_stack_backing[cpu_slot - 1] == 0) return null;
    return cpu_stacks.stackBase(cpu_slot, 1).? + cpu_stacks.ist_bytes;
}

fn cpuDoubleFaultIstTop(cpu_slot: usize) ?u64 {
    if (cpu_slot == 0) return stackTop(alignedStackRegion(df_ist_stack_region_raw[0..]), ist_stack_bytes);
    if (cpu_slot >= max_cpus) return null;
    if (ap_stack_backing[cpu_slot - 1] == 0) return null;
    return cpu_stacks.stackBase(cpu_slot, 2).? + cpu_stacks.ist_bytes;
}

fn kernelHighMappingActive() bool {
    return image_range.virtual_base >= 0xffff_8000_0000_0000 and image_range.base_paddr != 0;
}

fn kernelVirtToPhys(addr: u64) ?u64 {
    return physical_layout.kernelPointerPaddr(addr, image_range.virtual_base, image_range.base_paddr, image_range.size_bytes);
}

fn kernelPtrPaddr(ptr: anytype) ?u64 {
    return kernelVirtToPhys(@intFromPtr(ptr));
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

        var page_index: usize = 0;
        while (page_index < page_entries) : (page_index += 1) {
            const page_base = chunk_base + (@as(u64, @intCast(page_index)) * 4096);
            pt[page_index] = page_base | page_present | page_rw;
            if (page_base >= region_base and page_base < region_base + region.len) {
                if (page_base < usable_start or page_base >= usable_end) {
                    pt[page_index] = 0;
                }
            }
        }

        const pt_pa = kernelPtrPaddr(pt) orelse return false;
        if (pt_pa >= four_gib) return false;
        pd_tables[pdp_index][pd_index + chunk_index] = pt_pa | page_present | page_rw;
    }
    return true;
}

fn splitLargeKernelIdentityPage(pd_entry: *u64) bool {
    if ((pd_entry.* & page_present) == 0) return false;
    if ((pd_entry.* & page_ps) == 0) return true;
    if (runtime_identity_split_pt_used >= runtime_identity_split_pts.len) return false;
    const pt = &runtime_identity_split_pts[runtime_identity_split_pt_used];
    runtime_identity_split_pt_used += 1;

    const large_base = pd_entry.* & ~@as(u64, 0x1F_FFFF);
    const pte_flags = (pd_entry.* & 0xFFF) & ~page_ps;
    var page_index: usize = 0;
    while (page_index < page_entries) : (page_index += 1) {
        const page_base = large_base + (@as(u64, @intCast(page_index)) * 4096);
        pt[page_index] = page_base | pte_flags;
    }
    const pt_pa = kernelPtrPaddr(pt) orelse return false;
    pd_entry.* = pt_pa | pte_flags;
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
    if ((pd_entry.* & page_ps) != 0 and !splitLargeKernelIdentityPage(pd_entry)) return false;

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

/// Boot-only: called before APs or user address spaces exist, and before any
/// access to the firmware-declared MMIO aperture. Change the identity mapping
/// itself so there is no competing WB alias. Do not use this for live remaps.
pub fn mapBootMmioIdentityRange(base: u64, bytes: u64) bool {
    if (bytes == 0 or ((base | bytes) & 4095) != 0 or
        base < 0x100000 or base >= physical_layout.identity_limit or
        bytes > physical_layout.identity_limit - base) return false;
    // PWT=PCD=1, PAT=0 selects PAT entry 3. Do not silently assume firmware
    // left that entry UC, and do not change the global PAT configuration.
    if (((readMsr(0x277) >> 24) & 0xff) != 0) return false;
    const uc_flags = page_present | page_rw | page_nx | (1 << 3) | (1 << 4);
    const end = base + bytes;
    var address = base;
    while (address < end) {
        const pdp_index: usize = @intCast(address >> 30);
        const pd_index: usize = @intCast((address >> 21) & 511);
        const entry = &pd_tables[pdp_index][pd_index];
        if ((entry.* & page_present) == 0) return false;
        if ((address & (two_mib - 1)) == 0 and end - address >= two_mib and
            (entry.* & page_ps) != 0)
        {
            if ((entry.* & page_addr_mask & ~(two_mib - 1)) != address) return false;
            entry.* = address | uc_flags | page_ps;
            address += two_mib;
        } else {
            if (!splitLargeKernelIdentityPage(entry)) return false;
            const pt: *[page_entries]u64 = @ptrFromInt(entry.* & page_addr_mask);
            const index: usize = @intCast((address >> 12) & 511);
            if ((pt[index] & page_addr_mask) != address or (pt[index] & page_present) == 0)
                return false;
            pt[index] = address | uc_flags;
            address += 4096;
        }
    }
    // No other CPU can have accessed this aperture yet. Reload the current
    // CR3 to discard any local paging-structure translations before use.
    writeCr3(readCr3());
    return true;
}

/// Prove the *current* low identity PTE is explicit UC, rather than trusting
/// a previous map request. AMD IOMMU register access uses this identity VA;
/// a failed/partial split must never be mistaken for a safe MMIO mapping.
pub fn bootIdentityPageIsExplicitUc(page_base: u64) bool {
    if ((page_base & 4095) != 0 or page_base >= physical_layout.identity_limit) return false;
    const pdp_index: usize = @intCast(page_base >> 30);
    const pd_index: usize = @intCast((page_base >> 21) & 511);
    const entry = pd_tables[pdp_index][pd_index];
    const required = page_present | page_rw | page_nx | (1 << 3) | (1 << 4);
    if ((entry & page_present) == 0) return false;
    if ((entry & page_ps) != 0) {
        // For a 2 MiB leaf, bit 12 is PAT. The boot UC mapper selects PAT3.
        return (entry & (required | page_ps)) == (required | page_ps) and
            (entry & (1 << 12)) == 0 and
            (entry & page_addr_mask & ~(two_mib - 1)) == (page_base & ~(two_mib - 1));
    }
    const pt_base = entry & page_addr_mask;
    if (pt_base == 0) return false;
    const pt: *const [page_entries]u64 = @ptrFromInt(pt_base);
    const leaf = pt[@intCast((page_base >> 12) & 511)];
    // For a 4 KiB leaf, bit 7 is PAT. Reject it even if PWT/PCD look UC.
    return (leaf & required) == required and (leaf & page_ps) == 0 and
        (leaf & page_addr_mask) == page_base;
}

test "boot MMIO UC proof checks the live identity leaf and physical page" {
    const base: u64 = 0x0040_0000;
    const old = pd_tables[0][2];
    defer pd_tables[0][2] = old;
    const uc = page_present | page_rw | page_nx | (1 << 3) | (1 << 4);
    var pt: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;

    pd_tables[0][2] = base | page_present | page_rw | page_ps;
    try std.testing.expect(!bootIdentityPageIsExplicitUc(base));
    pd_tables[0][2] = base | uc | page_ps;
    try std.testing.expect(bootIdentityPageIsExplicitUc(base));
    pd_tables[0][2] |= 1 << 12;
    try std.testing.expect(!bootIdentityPageIsExplicitUc(base));

    pd_tables[0][2] = @intFromPtr(&pt) | page_present | page_rw;
    pt[0] = base | uc;
    try std.testing.expect(bootIdentityPageIsExplicitUc(base));
    pt[1] = (base + 0x2000) | uc;
    try std.testing.expect(!bootIdentityPageIsExplicitUc(base + 0x1000));
    pt[1] = (base + 0x1000) | uc | page_ps;
    try std.testing.expect(!bootIdentityPageIsExplicitUc(base + 0x1000));
    pt[1] = (base + 0x1000) | uc;
    try std.testing.expect(bootIdentityPageIsExplicitUc(base + 0x1000));
}

fn highKernelPdSlot(first_pdp_index: usize, pdp_index: usize) ?usize {
    if (pdp_index < first_pdp_index) return null;
    const slot = pdp_index - first_pdp_index;
    if (slot >= high_kernel_pd_table_count) return null;
    return slot;
}

fn installHighKernelMapping() bool {
    if (!kernelHighMappingActive()) return true;
    const kernel_table_flags = page_present | page_rw;
    @memset(high_kernel_pdp_table[0..], 0);
    var pd_slot: usize = 0;
    while (pd_slot < high_kernel_pd_table_count) : (pd_slot += 1) {
        @memset(high_kernel_pd_tables[pd_slot][0..], 0);
    }
    var pt_slot_clear: usize = 0;
    while (pt_slot_clear < high_kernel_pt_table_count) : (pt_slot_clear += 1) {
        @memset(high_kernel_pt_tables[pt_slot_clear][0..], 0);
    }

    const vbase = image_range.virtual_base & ~@as(u64, 0xFFF);
    const base_delta = image_range.virtual_base - vbase;
    const pbase = image_range.base_paddr - base_delta;
    const bytes = if (image_range.size_bytes == 0) @as(usize, @intCast(256 * 1024 * 1024)) else image_range.size_bytes + @as(usize, @intCast(base_delta));
    const vend = (vbase + @as(u64, @intCast(bytes)) + 0xFFF) & ~@as(u64, 0xFFF);

    const pml4_index: usize = @intCast((vbase >> 39) & 0x1FF);
    if (pml4_index >= page_entries) return false;
    const first_pdp_index: usize = @intCast((vbase >> 30) & 0x1FF);
    const high_pdp_pa = kernelPtrPaddr(&high_kernel_pdp_table) orelse return false;
    pml4_table[pml4_index] = high_pdp_pa | kernel_table_flags;

    var va = vbase;
    while (va < vend) {
        const pdp_index: usize = @intCast((va >> 30) & 0x1FF);
        const pd_index: usize = @intCast((va >> 21) & 0x1FF);
        const pd_table_slot = highKernelPdSlot(first_pdp_index, pdp_index) orelse return false;
        const pt_slot: usize = @intCast((va - vbase) >> 21);
        if (pt_slot >= high_kernel_pt_table_count) return false;
        if (high_kernel_pdp_table[pdp_index] == 0) {
            const pd_pa = kernelPtrPaddr(&high_kernel_pd_tables[pd_table_slot]) orelse return false;
            high_kernel_pdp_table[pdp_index] = pd_pa | kernel_table_flags;
        }
        if (high_kernel_pd_tables[pd_table_slot][pd_index] == 0) {
            const pt_pa = kernelPtrPaddr(&high_kernel_pt_tables[pt_slot]) orelse return false;
            high_kernel_pd_tables[pd_table_slot][pd_index] = pt_pa | kernel_table_flags;
        }
        const pt = &high_kernel_pt_tables[pt_slot];
        const pt_index: usize = @intCast((va >> 12) & 0x1FF);
        const phys = pbase + (va - vbase);
        pt[pt_index] = phys | page_present | page_rw;
        va += 4096;
    }
    return true;
}

pub fn mapKernelRuntimeIdentityRange(base: u64, bytes: usize) bool {
    if (kernelHighMappingActive()) {
        if (bytes == 0) return true;
        if (base < image_range.virtual_base) return false;
        const offset = base - image_range.virtual_base;
        return image_range.size_bytes == 0 or offset + @as(u64, @intCast(bytes)) <= image_range.size_bytes;
    }
    return mapKernelIdentityRange(base, bytes);
}

fn mapPerCpuKernelStorage() bool {
    if (kernelHighMappingActive()) return true;
    if (!mapKernelIdentityRange(@intFromPtr(&phys_copy_window_pt), @sizeOf(@TypeOf(phys_copy_window_pt)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&gdt_tables), @sizeOf(@TypeOf(gdt_tables)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&tss_tables), @sizeOf(@TypeOf(tss_tables)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&ap_stack_backing), @sizeOf(@TypeOf(ap_stack_backing)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&ap_stack_pts), @sizeOf(@TypeOf(ap_stack_pts)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&runtime_identity_split_pts), @sizeOf(@TypeOf(runtime_identity_split_pts)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&runtime_identity_split_pt_used), @sizeOf(@TypeOf(runtime_identity_split_pt_used)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&pf_trampoline_page), @sizeOf(@TypeOf(pf_trampoline_page)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&gp_trampoline_page), @sizeOf(@TypeOf(gp_trampoline_page)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&df_trampoline_page), @sizeOf(@TypeOf(df_trampoline_page)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&ud_trampoline_page), @sizeOf(@TypeOf(ud_trampoline_page)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&bp_trampoline_page), @sizeOf(@TypeOf(bp_trampoline_page)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&ts_trampoline_page), @sizeOf(@TypeOf(ts_trampoline_page)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&np_trampoline_page), @sizeOf(@TypeOf(np_trampoline_page)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&ss_trampoline_page), @sizeOf(@TypeOf(ss_trampoline_page)))) return false;
    if (!mapKernelIdentityRange(@intFromPtr(&timer_trampoline_page), @sizeOf(@TypeOf(timer_trampoline_page)))) return false;
    return true;
}

// Called on the BSP before AP startup and before user address spaces exist.
// Backing is lifetime-pinned, but only CPUs actually present consume it.
pub fn allocateApRuntimeStacks(cpu_slot: usize, free_list: anytype) bool {
    if (cpu_slot == 0 or cpu_slot >= max_cpus or !kernelHighMappingActive()) return false;
    if (ap_stack_backing[cpu_slot - 1] != 0) return true;
    const first_pdp: usize = @intCast((image_range.virtual_base >> 30) & 0x1ff);
    const stack_pdp: usize = @intCast((cpu_stacks.base >> 30) & 0x1ff);
    if ((image_range.virtual_base >> 39) != (cpu_stacks.base >> 39)) return false;
    if (image_range.virtual_base + image_range.size_bytes > cpu_stacks.base) return false;
    const pd_slot = highKernelPdSlot(first_pdp, stack_pdp) orelse return false;
    const pd_pa = kernelPtrPaddr(&high_kernel_pd_tables[pd_slot]) orelse return false;
    const pd_index = (cpu_slot - 1) * 2;
    const tables = &ap_stack_pts[cpu_slot - 1];
    const pt0_pa = kernelPtrPaddr(&tables[0]) orelse return false;
    const pt1_pa = kernelPtrPaddr(&tables[1]) orelse return false;
    if (high_kernel_pd_tables[pd_slot][pd_index] != 0 or
        high_kernel_pd_tables[pd_slot][pd_index + 1] != 0) return false;
    const backing = free_list.popContiguousBelow(cpu_stacks.backing_bytes / 4096, physical_layout.identity_limit) catch return false;
    const raw: [*]u8 = @ptrFromInt(backing);
    @memset(raw[0..cpu_stacks.backing_bytes], 0);
    var phys = backing;
    for (cpu_stacks.sizes, 0..) |bytes, stack| {
        var offset = cpu_stacks.offsets[stack];
        const end = offset + bytes;
        while (offset < end) : (offset += 4096) {
            tables[offset / two_mib][(offset / 4096) % page_entries] = phys | page_present | page_rw | page_nx;
            phys += 4096;
        }
    }
    high_kernel_pd_tables[pd_slot][pd_index] = pt0_pa | page_present | page_rw;
    high_kernel_pd_tables[pd_slot][pd_index + 1] = pt1_pa | page_present | page_rw;
    high_kernel_pdp_table[stack_pdp] = pd_pa | page_present | page_rw;
    ap_stack_backing[cpu_slot - 1] = backing;
    return true;
}

pub fn mapCpuRuntimeStacks(cpu_slot: usize) bool {
    if (cpu_slot >= max_cpus) return false;
    if (cpu_slot != 0) return ap_stack_backing[cpu_slot - 1] != 0;
    if (kernelHighMappingActive()) return true;
    if (cpu_slot == 0) {
        if (!mapKernelIdentityRange(stackBottom(alignedStackRegion(ring0_stack_region_raw[0..]), ring0_stack_bytes), ring0_stack_bytes)) return false;
        if (!mapKernelIdentityRange(stackBottom(alignedStackRegion(pf_ist_stack_region_raw[0..]), ist_stack_bytes), ist_stack_bytes)) return false;
        if (!mapKernelIdentityRange(stackBottom(alignedStackRegion(df_ist_stack_region_raw[0..]), ist_stack_bytes), ist_stack_bytes)) return false;
        return true;
    }
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

    const pml4_pa: u64 = kernelPtrPaddr(&pml4_table) orelse return false;
    const pdp_pa: u64 = kernelPtrPaddr(&pdp_table) orelse return false;
    const pd0_pa: u64 = kernelPtrPaddr(&pd_tables[0]) orelse return false;
    const high_pdp_pa: u64 = kernelPtrPaddr(&high_mmio_pdp_table) orelse return false;
    const high_pd0_pa: u64 = kernelPtrPaddr(&high_mmio_pd_tables[0]) orelse return false;
    const phys_copy_window_pt_pa: u64 = kernelPtrPaddr(&phys_copy_window_pt) orelse return false;
    if (pml4_pa >= four_gib or pdp_pa >= four_gib or pd0_pa >= four_gib or high_pdp_pa >= four_gib or high_pd0_pa >= four_gib or phys_copy_window_pt_pa >= four_gib) return false;

    const kernel_table_flags = page_present | page_rw;
    const kernel_large_page_flags = page_present | page_rw | page_ps;

    pml4_table[0] = pdp_pa | kernel_table_flags;
    pd_idx = 0;
    while (pd_idx < pd_table_count) : (pd_idx += 1) {
        const pd_pa: u64 = kernelPtrPaddr(&pd_tables[pd_idx]) orelse return false;
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
        const high_pd_pa: u64 = kernelPtrPaddr(&high_mmio_pd_tables[high_pdp_idx]) orelse return false;
        high_mmio_pdp_table[high_pdp_idx] = high_pd_pa | kernel_table_flags;

        const region_base = (@as(u64, @intCast(high_mmio_pml4_index)) << 39) + (@as(u64, @intCast(high_pdp_idx)) << 30);
        var i: usize = 0;
        while (i < page_entries) : (i += 1) {
            const base = region_base + (@as(u64, @intCast(i)) * two_mib);
            high_mmio_pd_tables[high_pdp_idx][i] = base | kernel_large_page_flags;
        }
    }
    high_mmio_pd_tables[0][0] = phys_copy_window_pt_pa | kernel_table_flags;

    if (!installHighKernelMapping()) return false;
    if (!kernelHighMappingActive()) {
        if (!installGuardedIdentityStackRegion(alignedStackRegion(ring0_stack_region_raw[0..]), ring0_stack_bytes, &ring0_stack_guard_pt)) return false;
        if (!installGuardedIdentityStackRegion(alignedStackRegion(pf_ist_stack_region_raw[0..]), ist_stack_bytes, &pf_ist_stack_guard_pt)) return false;
        if (!installGuardedIdentityStackRegion(alignedStackRegion(df_ist_stack_region_raw[0..]), ist_stack_bytes, &df_ist_stack_guard_pt)) return false;
    }
    if (!mapPerCpuKernelStorage()) return false;

    writeCr3(pml4_pa);
    kernel_cr3_value = pml4_pa;
    return true;
}

pub fn hardenKernelMappingsSupervisorOnly() void {
    pml4_table[0] &= ~page_user;
    pml4_table[high_mmio_pml4_index] &= ~page_user;
    if (kernelHighMappingActive()) {
        const high_index: usize = @intCast((image_range.virtual_base >> 39) & 0x1FF);
        pml4_table[high_index] &= ~page_user;
    }

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

pub fn kernelPointerPaddr(addr: usize) ?u64 {
    return kernelVirtToPhys(@intCast(addr));
}

pub fn seedUserPml4WithKernel(pml4: []u64) void {
    if (!kernelHighMappingActive()) return;
    const pml4_index: usize = @intCast((image_range.virtual_base >> 39) & 0x1FF);
    if (pml4_index >= pml4.len) return;
    const high_pdp_pa = kernelPtrPaddr(&high_kernel_pdp_table) orelse return;
    pml4[pml4_index] = high_pdp_pa | page_present | page_rw;
}

pub fn seedUserPdWithKernelIdentity(pd: []u64) void {
    var i: usize = 0;
    while (i < page_entries and i < pd.len) : (i += 1) {
        pd[i] = pd_tables[0][i] & ~page_user;
    }
}

pub fn seedUserPdpWithKernelIdentity(pdp: []u64) void {
    const kernel_table_flags = page_present | page_rw;
    var pdp_idx: usize = 1;
    while (pdp_idx < pd_table_count and pdp_idx < pdp.len) : (pdp_idx += 1) {
        const pd_pa: u64 = kernelPtrPaddr(&pd_tables[pdp_idx]) orelse return;
        pdp[pdp_idx] = pd_pa | kernel_table_flags;
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
    return gdt_tables[0][3];
}

pub fn userDataDescriptor() u64 {
    return gdt_tables[0][4];
}

pub fn gdtBase() u64 {
    return @intFromPtr(&gdt_tables[0]);
}
