const std = @import("std");
const kernel = @import("kernel.zig");
const interrupts = @import("interrupts.zig");
const serial = @import("serial.zig");
const user_programs = @import("user_programs.zig");
const uefi = std.os.uefi;
const TrapFrame = interrupts.TrapFrame;
const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;

const page_entries: usize = 512;
const four_gib: u64 = 4 * 1024 * 1024 * 1024;
const two_mib: u64 = 2 * 1024 * 1024;
const pd_table_count: usize = 4; // 4 * 1GiB = 4GiB
const user_va: u64 = 0x20000000;
const user_stack_top: u64 = 0x20002000;
const user_stack_page_va: u64 = user_stack_top - 0x1000;
const user_unmapped_test_va: u64 = 0x20100000;
const user_dma_verify_va: u64 = 0x20110000;
const user_recovery_stop_va: u64 = 0x20200000;
const reserved_low_mem_end: u64 = 64 * 1024 * 1024;
const page_addr_mask: u64 = 0x000F_FFFF_FFFF_F000;
const canonical_user_limit_exclusive: u64 = 0x0000_8000_0000_0000;
const gdt_kernel_code_selector: u16 = 0x08;
const gdt_kernel_data_selector: u16 = 0x10;
const gdt_user_code_selector: u16 = 0x18;
const gdt_user_data_selector: u16 = 0x20;
const gdt_tss_selector: u16 = 0x28;

const page_present: u64 = 1 << 0;
const page_rw: u64 = 1 << 1;
const page_user: u64 = 1 << 2;
const page_ps: u64 = 1 << 7;

const debug_skip_exit_boot_services = false;
const debug_skip_cr3_switch = false;
const debug_trigger_page_fault_test = false;
const debug_trigger_general_protection_test = false;
const debug_trigger_pf_recovery_demo = false;
const debug_trigger_dma_unmap_verify_demo = false;
const user_process_count: usize = 2;

const UserAddressSpace = struct {
    pml4: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries,
    pdp: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries,
    pd: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries,
    pt: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries,
    cr3: u64 = 0,
};

var pml4_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var pd_tables: [pd_table_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** pd_table_count;
var user_spaces: [user_process_count]UserAddressSpace = .{ .{}, .{} };
var global_free_list: kernel.FreePageList = .{};
var idt: [256]interrupts.IdtEntry align(16) = [_]interrupts.IdtEntry{interrupts.zeroIdtEntry()} ** 256;
var gdt: [7]u64 align(16) = .{
    0x0000000000000000, // 0x00 null
    0x00AF9A000000FFFF, // 0x08 kernel code (DPL=0)
    0x00AF92000000FFFF, // 0x10 kernel data (DPL=0)
    0x00AFFA000000FFFF, // 0x18 user code (DPL=3)
    0x00AFF2000000FFFF, // 0x20 user data (DPL=3)
    0x0000000000000000, // 0x28 TSS low
    0x0000000000000000, // 0x30 TSS high
};
var ring0_stack: [64 * 1024]u8 align(16) = [_]u8{0} ** (64 * 1024);
var tss: Tss = std.mem.zeroes(Tss);
var kernel_state_global: kernel.KernelState = undefined;
var kernel_state_ready = false;
var int80_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var pf_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var gp_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var df_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ud_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ts_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var np_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ss_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var int80_trampoline_entry: usize = 0;
var pf_trampoline_entry: usize = 0;
var gp_trampoline_entry: usize = 0;
var df_trampoline_entry: usize = 0;
var ud_trampoline_entry: usize = 0;
var ts_trampoline_entry: usize = 0;
var np_trampoline_entry: usize = 0;
var ss_trampoline_entry: usize = 0;
export var kernel_cr3_value: u64 = 0;
export var user_cr3_value: u64 = 0;
var current_user_principal: kernel.PrincipalId = .Process0;

const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_move_cap: u64 = 0x3;
const syscall_drop_present: u64 = 0x4;
const syscall_switch_process: u64 = 0x5;

const user_program_cfg: user_programs.Config = .{
    .syscall_alloc_page = syscall_alloc_page,
    .syscall_map_page = syscall_map_page,
    .syscall_move_cap = syscall_move_cap,
    .syscall_drop_present = syscall_drop_present,
    .syscall_switch_process = syscall_switch_process,
    .user_unmapped_test_va = user_unmapped_test_va,
    .user_dma_verify_va = user_dma_verify_va,
    .user_recovery_stop_va = user_recovery_stop_va,
};

const syscall_ok: u64 = 0;
const syscall_err_invalid = 1;
const syscall_err_not_ready = 2;
const syscall_err_alloc = 4;
const syscall_err_map = 5;
const syscall_err_move = 6;
const syscall_err_drop_present = 7;

const MemoryStats = struct {
    detected_regions: usize,
    total_usable_bytes: u64,
};

const ReservedRange = struct {
    start: u64,
    end: u64, // exclusive
};

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

const PageFaultCapability = struct {
    principal: kernel.PrincipalId,
    fault_va: u64,
    fault_page_va: u64,
    fault_rip: u64,
    present_violation: bool,
    write_access: bool,
    instruction_fetch: bool,
    candidate_paddr: ?u64,
};

fn serialInit() void {
    serial.init();
}

fn serialWrite(text: []const u8) void {
    serial.write(text);
}

fn serialWriteHexRaw(value: u64) void {
    serial.writeHexRaw(value);
}

fn serialWriteBool01(value: bool) void {
    serial.writeBool01(value);
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

    out[off] = 0x50; // push rax
    off += 1;
    out[off] = 0x48; // mov rax, imm64
    out[off + 1] = 0xB8;
    writeU64LEBytes(out, off + 2, kernel_cr3_value);
    off += 10;
    out[off] = 0x0F; // mov cr3, rax
    out[off + 1] = 0x22;
    out[off + 2] = 0xD8;
    off += 3;
    out[off] = 0x58; // pop rax
    off += 1;
    out[off] = 0xFF; // jmp qword ptr [rip+0]
    out[off + 1] = 0x25;
    out[off + 2] = 0x00;
    out[off + 3] = 0x00;
    out[off + 4] = 0x00;
    out[off + 5] = 0x00;
    off += 6;
    writeU64LEBytes(out, off, target);
    return @intFromPtr(page);
}

fn installInterruptTrampolines() void {
    int80_trampoline_entry = buildCr3SwitchTrampoline(&int80_trampoline_page, @intFromPtr(&syscallHandlerStub));
    pf_trampoline_entry = buildCr3SwitchTrampoline(&pf_trampoline_page, @intFromPtr(&pageFaultHandlerStub));
    gp_trampoline_entry = buildCr3SwitchTrampoline(&gp_trampoline_page, @intFromPtr(&generalProtectionHandlerStub));
    df_trampoline_entry = buildCr3SwitchTrampoline(&df_trampoline_page, @intFromPtr(&doubleFaultHandlerStub));
    ud_trampoline_entry = buildCr3SwitchTrampoline(&ud_trampoline_page, @intFromPtr(&invalidOpcodeHandlerStub));
    ts_trampoline_entry = buildCr3SwitchTrampoline(&ts_trampoline_page, @intFromPtr(&invalidTssHandlerStub));
    np_trampoline_entry = buildCr3SwitchTrampoline(&np_trampoline_page, @intFromPtr(&segmentNotPresentHandlerStub));
    ss_trampoline_entry = buildCr3SwitchTrampoline(&ss_trampoline_page, @intFromPtr(&stackSegmentFaultHandlerStub));
}

fn readCr2() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr2, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

fn exceptionName(vec: u64) []const u8 {
    return switch (vec) {
        13 => "GENERAL PROTECTION",
        14 => "PAGE FAULT",
        else => "EXCEPTION",
    };
}

fn processIndex(principal: kernel.PrincipalId) ?usize {
    return switch (principal) {
        .Process0 => 0,
        .Process1 => 1,
        else => null,
    };
}

fn getUserSpace(principal: kernel.PrincipalId) ?*UserAddressSpace {
    const idx = processIndex(principal) orelse return null;
    return &user_spaces[idx];
}

fn currentUserSpace() *UserAddressSpace {
    return getUserSpace(current_user_principal).?;
}

fn isUserCanonicalVa(va: u64) bool {
    return va < canonical_user_limit_exclusive;
}

fn lookupUserMappedPaddrForVa(principal: kernel.PrincipalId, va: u64) ?u64 {
    const space = getUserSpace(principal) orelse return null;
    const pml4_index: usize = @intCast((va >> 39) & 0x1FF);
    const pdp_index: usize = @intCast((va >> 30) & 0x1FF);
    const pd_index: usize = @intCast((va >> 21) & 0x1FF);
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const user_pdp_index: usize = @intCast((user_va >> 30) & 0x1FF);
    const user_pd_index: usize = @intCast((user_va >> 21) & 0x1FF);

    // Current stage maps only one PT under the user_va PD slot.
    if (pml4_index != 0 or pdp_index != user_pdp_index or pd_index != user_pd_index) return null;

    const entry = space.pt[pt_index];
    const paddr = entry & page_addr_mask;
    if (paddr == 0) return null;
    return paddr;
}

fn issuePageFaultCapability(frame: *const ExceptionTrapFrame, cr2: u64) ?PageFaultCapability {
    const ec = frame.error_code;
    const user_mode = (ec & (1 << 2)) != 0;
    if (!user_mode) return null;
    if (!isUserCanonicalVa(cr2)) return null;

    const fault_page_va = pageAlignDown(cr2);
    return .{
        .principal = current_user_principal,
        .fault_va = cr2,
        .fault_page_va = fault_page_va,
        .fault_rip = frame.rip,
        .present_violation = (ec & (1 << 0)) != 0,
        .write_access = (ec & (1 << 1)) != 0,
        .instruction_fetch = (ec & (1 << 4)) != 0,
        .candidate_paddr = lookupUserMappedPaddrForVa(current_user_principal, fault_page_va),
    };
}

fn resolvePageFaultCapability(pf_cap: PageFaultCapability) bool {
    // Step3 policy: only user-mode not-present faults are recoverable.
    if (pf_cap.present_violation) return false;
    if (!kernel_state_ready) return false;

    const candidate_paddr = pf_cap.candidate_paddr orelse return false;
    const cap = kernel_state_global.getTableConst(pf_cap.principal).find(candidate_paddr) orelse return false;
    if (!cap.rights.cpu_read) return false;
    if (pf_cap.write_access and !cap.rights.cpu_write) return false;
    if (pf_cap.instruction_fetch) return false;

    return mapUserPageFromCapability(
        &kernel_state_global,
        pf_cap.principal,
        pf_cap.fault_page_va,
        candidate_paddr,
        cap.rights.cpu_write,
    );
}

fn logPageFaultStep2(cr2: u64, frame: *const ExceptionTrapFrame) void {
    const ec_user = (frame.error_code & (1 << 2)) != 0;
    const va_user = isUserCanonicalVa(cr2);

    serialWrite("  USER_MODE=");
    serialWriteBool01(ec_user);
    serialWrite("\n");
    serialWrite("  USER_VA=");
    serialWriteBool01(va_user);
    serialWrite("\n");

    const pf_cap = issuePageFaultCapability(frame, cr2) orelse {
        serialWrite("  PF_CAP=none\n");
        serialWrite("  CAP_LOOKUP=skip\n");
        return;
    };
    serialWrite("  PF_CAP=issued\n");

    const candidate_paddr = pf_cap.candidate_paddr orelse {
        serialWrite("  CAND_PADDR=none\n");
        serialWrite("  CAP_LOOKUP=none\n");
        return;
    };
    serialWrite("  CAND_PADDR=");
    serialWriteHexRaw(candidate_paddr);
    serialWrite("\n");

    if (!kernel_state_ready) {
        serialWrite("  CAP_LOOKUP=kernel_state_not_ready\n");
        return;
    }

    const has_cap = kernel_state_global.getTableConst(pf_cap.principal).find(candidate_paddr) != null;
    serialWrite("  CAP_LOOKUP=");
    serialWrite(if (has_cap) "found(current)\n" else "none(current)\n");
}

pub export fn pageFaultDispatch(frame: *const ExceptionTrapFrame) callconv(.c) u64 {
    const cr2 = readCr2();
    const pf_cap = issuePageFaultCapability(frame, cr2) orelse return 0;
    if (!resolvePageFaultCapability(pf_cap)) return 0;

    serialWrite("PAGE FAULT RESOLVED\n");
    serialWrite("  CR2=");
    serialWriteHexRaw(cr2);
    serialWrite("\n");
    serialWrite("  PF_CAP=consumed\n");
    return 1;
}

pub export fn exceptionWithErrorCommon(vec: u64, frame: *const ExceptionTrapFrame) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite(exceptionName(vec));
    serialWrite("\n");
    if (vec == 14) {
        serialWrite("  CR2=");
        const cr2 = readCr2();
        serialWriteHexRaw(cr2);
        serialWrite("\n");
        logPageFaultStep2(cr2, frame);
    }
    serialWrite("  EC=");
    serialWriteHexRaw(frame.error_code);
    serialWrite("\n");
    serialWrite("  RIP=");
    serialWriteHexRaw(frame.rip);
    serialWrite("\n");
    haltLoop();
}

pub export fn doubleFaultHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("DOUBLE FAULT\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    while (true) {
        asm volatile ("hlt");
    }
}

fn haltLoop() noreturn {
    while (true) asm volatile ("hlt");
}

pub export fn invalidTssHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("INVALID TSS\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    haltLoop();
}

pub export fn segmentNotPresentHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("SEGMENT NOT PRESENT\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    haltLoop();
}

pub export fn stackSegmentFaultHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("STACK SEGMENT FAULT\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    haltLoop();
}

pub export fn invalidOpcodeHandlerCommon() callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("INVALID OPCODE\n");
    haltLoop();
}

fn parseRights(bits: u64) kernel.Rights {
    return .{
        .cpu_read = (bits & 0x1) != 0,
        .cpu_write = (bits & 0x2) != 0,
        .dma = (bits & 0x4) != 0,
    };
}

fn mapUserPageFromCapability(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    va: u64,
    paddr: u64,
    writable: bool,
) bool {
    const space = getUserSpace(principal) orelse return false;
    if ((va & 0xFFF) != 0) return false;
    if ((paddr & 0xFFF) != 0) return false;
    if (paddr >= four_gib) return false;

    const pml4_index: usize = @intCast((va >> 39) & 0x1FF);
    const pdp_index: usize = @intCast((va >> 30) & 0x1FF);
    const pd_index: usize = @intCast((va >> 21) & 0x1FF);
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const user_pdp_index: usize = @intCast((user_va >> 30) & 0x1FF);
    const user_pd_index: usize = @intCast((user_va >> 21) & 0x1FF);

    // 現段階は user_pt_table (1本) の範囲に限定する。
    if (pml4_index != 0 or pdp_index != user_pdp_index or pd_index != user_pd_index) return false;

    const cap = state.getTableConst(principal).find(paddr) orelse return false;
    if (!cap.rights.cpu_read) return false;
    if (writable and !cap.rights.cpu_write) return false;

    // Strict 1:1 mapping: clear alias mappings of the same paddr first.
    var i: usize = 0;
    while (i < page_entries) : (i += 1) {
        if (i == pt_index) continue;
        const entry = space.pt[i];
        if ((entry & page_addr_mask) != paddr) continue;
        if (entry == 0) continue;
        space.pt[i] = 0;
        const alias_va = (user_va & ~@as(u64, 0x1F_FFFF)) + (@as(u64, i) * 4096);
        flushUserTlbForPrincipalVa(principal, alias_va);
    }

    space.pt[pt_index] = paddr | page_present | page_user | (if (writable) page_rw else 0);
    flushUserTlbForPrincipalVa(principal, va);
    return true;
}

fn dropPresentForUserMappedPaddr(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    paddr: u64,
) bool {
    const space = getUserSpace(principal) orelse return false;
    _ = state.getTableConst(principal).find(paddr) orelse return false;
    const user_pt_base_va = user_va & ~@as(u64, 0x1F_FFFF);

    var i: usize = 0;
    while (i < page_entries) : (i += 1) {
        const entry = space.pt[i];
        if ((entry & page_addr_mask) != paddr) continue;
        if ((entry & page_present) == 0) continue;
        space.pt[i] = entry & ~page_present;
        flushUserTlbForPrincipalVa(principal, user_pt_base_va + (@as(u64, i) * 4096));
        return true;
    }
    return false;
}

pub export fn syscallDispatch(frame: *TrapFrame) callconv(.c) u64 {
    if (!kernel_state_ready) return syscall_err_not_ready;
    serialWrite("INT80 dispatch\n");
    const state = &kernel_state_global;
    const proc = current_user_principal;

    switch (frame.rax) {
        syscall_alloc_page => {
            const cap = state.allocPageTo(proc, &global_free_list) catch return syscall_err_alloc;
            return cap.paddr;
        },
        syscall_map_page => {
            const writable = (frame.rdx & 0x1) != 0;
            if (mapUserPageFromCapability(state, proc, frame.rdi, frame.rsi, writable)) {
                return syscall_ok;
            }
            return syscall_err_map;
        },
        syscall_move_cap => {
            const to = switch (frame.rsi) {
                0 => proc,
                1 => kernel.PrincipalId.Device0,
                else => return syscall_err_invalid,
            };
            const from = if (to == proc) kernel.PrincipalId.Device0 else proc;
            const rights = parseRights(frame.rdx);
            state.moveCap(from, to, frame.rdi, rights) catch return syscall_err_move;
            return syscall_ok;
        },
        syscall_drop_present => {
            if (dropPresentForUserMappedPaddr(state, proc, frame.rdi)) {
                return syscall_ok;
            }
            return syscall_err_drop_present;
        },
        syscall_switch_process => {
            const target = switch (frame.rdi) {
                0 => kernel.PrincipalId.Process0,
                1 => kernel.PrincipalId.Process1,
                else => return syscall_err_invalid,
            };
            if (target == proc) return syscall_ok;

            current_user_principal = target;
            user_cr3_value = currentUserSpace().cr3;
            frame.rip = user_va;
            frame.rsp = user_stack_top;
            return syscall_ok;
        },
        else => return syscall_err_invalid,
    }
}

pub export fn pageFaultHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\push %rax
        \\push %rbx
        \\push %rcx
        \\push %rdx
        \\push %rsi
        \\push %rdi
        \\push %rbp
        \\push %r8
        \\push %r9
        \\push %r10
        \\push %r11
        \\push %r12
        \\push %r13
        \\push %r14
        \\push %r15
        \\mov %rsp, %rcx
        // Keep original stack pointer in a callee-saved register across the C call.
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\call pageFaultDispatch
        \\mov %r15, %rsp
        \\test %rax, %rax
        \\jz 1f
        \\pop %r15
        \\pop %r14
        \\pop %r13
        \\pop %r12
        \\pop %r11
        \\pop %r10
        \\pop %r9
        \\pop %r8
        \\pop %rbp
        \\pop %rdi
        \\pop %rsi
        \\pop %rdx
        \\pop %rcx
        \\pop %rbx
        \\pop %rax
        \\push %r10
        \\mov user_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        // #PF has error_code on stack; remove it before iretq.
        \\add $8, %rsp
        \\iretq
        \\1:
        \\mov %rsp, %rdx
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\mov $14, %rcx
        \\call exceptionWithErrorCommon
        \\ud2
    );
}

pub export fn doubleFaultHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp doubleFaultHandlerCommon
    );
}

pub export fn syscallHandlerStub() callconv(.naked) noreturn {
    asm volatile (
    // ring3 -> kernel entry: scratch 利用前に r10 を退避し、完全保存を維持する。
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\push %rax
        \\push %rbx
        \\push %rcx
        \\push %rdx
        \\push %rsi
        \\push %rdi
        \\push %rbp
        \\push %r8
        \\push %r9
        \\push %r10
        \\push %r11
        \\push %r12
        \\push %r13
        \\push %r14
        \\push %r15
        // Win64: 32-byte shadow space を確保しつつ call 前 16-byte alignment を維持する。
        \\sub $32, %rsp
        \\lea 32(%rsp), %rdi
        \\mov %rdi, %rcx
        \\call syscallDispatch
        \\add $32, %rsp
        // TrapFrame.rax へ戻り値を書き戻す（offsetは comptime で検証済み）
        \\mov %rax, 112(%rsp)
        \\pop %r15
        \\pop %r14
        \\pop %r13
        \\pop %r12
        \\pop %r11
        \\pop %r10
        \\pop %r9
        \\pop %r8
        \\pop %rbp
        \\pop %rdi
        \\pop %rsi
        \\pop %rdx
        \\pop %rcx
        \\pop %rbx
        \\pop %rax
        // kernel -> ring3 return: r10 を壊さず user CR3 を復帰して iretq する。
        \\push %r10
        \\mov user_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\iretq
    );
}

pub export fn generalProtectionHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\push %rax
        \\push %rbx
        \\push %rcx
        \\push %rdx
        \\push %rsi
        \\push %rdi
        \\push %rbp
        \\push %r8
        \\push %r9
        \\push %r10
        \\push %r11
        \\push %r12
        \\push %r13
        \\push %r14
        \\push %r15
        \\mov %rsp, %r8
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\mov $13, %rcx
        \\mov %r8, %rdx
        \\call exceptionWithErrorCommon
        \\ud2
    );
}

pub export fn invalidTssHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp invalidTssHandlerCommon
    );
}

pub export fn segmentNotPresentHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp segmentNotPresentHandlerCommon
    );
}

pub export fn stackSegmentFaultHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp stackSegmentFaultHandlerCommon
    );
}

pub export fn invalidOpcodeHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp invalidOpcodeHandlerCommon
    );
}

fn loadGdtAndReloadSegments() void {
    const tss_base = @intFromPtr(&tss);
    const tss_limit: u64 = @sizeOf(Tss) - 1;
    tss.rsp0 = @intFromPtr(&ring0_stack) + ring0_stack.len;
    tss.iomap_base = @sizeOf(Tss);
    gdt[5] =
        (tss_limit & 0xFFFF) |
        ((tss_base & 0x00FF_FFFF) << 16) |
        (@as(u64, 0x89) << 40) |
        (((tss_limit >> 16) & 0xF) << 48) |
        (((tss_base >> 24) & 0xFF) << 56);
    gdt[6] = (tss_base >> 32) & 0xFFFF_FFFF;

    const gdt_ptr = GdtPtr{
        .limit = @as(u16, @intCast(@sizeOf(@TypeOf(gdt)) - 1)),
        .base = @intFromPtr(&gdt),
    };
    asm volatile ("lgdt (%[ptr])"
        :
        : [ptr] "r" (&gdt_ptr),
        : .{ .memory = true });

    // CS/SS/DS を新しい GDT の selector へ揃える。
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
}

fn initIdtPageFaultOnly() void {
    interrupts.clearIdt(&idt);
    interrupts.setIdtEntry(&idt, 6, gdt_kernel_code_selector, ud_trampoline_entry, 0x8E); // #UD
    interrupts.setIdtEntry(&idt, 10, gdt_kernel_code_selector, ts_trampoline_entry, 0x8E); // #TS
    interrupts.setIdtEntry(&idt, 11, gdt_kernel_code_selector, np_trampoline_entry, 0x8E); // #NP
    interrupts.setIdtEntry(&idt, 12, gdt_kernel_code_selector, ss_trampoline_entry, 0x8E); // #SS
    interrupts.setIdtEntry(&idt, 13, gdt_kernel_code_selector, gp_trampoline_entry, 0x8E); // #GP
    interrupts.setIdtEntry(&idt, 14, gdt_kernel_code_selector, pf_trampoline_entry, 0x8E); // #PF
    interrupts.setIdtEntry(&idt, 8, gdt_kernel_code_selector, df_trampoline_entry, 0x8E); // #DF
    // DPL=3 trap gate to allow ring3 software syscall entry.
    interrupts.setIdtEntry(&idt, 0x80, gdt_kernel_code_selector, int80_trampoline_entry, 0xEF);
    interrupts.loadIdt(&idt);
}

fn triggerPageFaultTest() noreturn {
    serialWrite("triggering page fault test...\n");
    const bad_ptr: *volatile u64 = @ptrFromInt(0xFFFF_8000_0000_0000);
    bad_ptr.* = 0xDEADBEEF;
    while (true) {
        asm volatile ("hlt");
    }
}

fn printNumber(value: anytype) void {
    serial.printNumber(value);
}

fn printHex(value: u64) void {
    serial.printHex(value);
}

fn dumpPrincipalCaps(state: *const kernel.KernelState, principal: kernel.PrincipalId, label: []const u8) void {
    serialWrite(label);
    serialWrite(" caps:\n");

    const table = state.getTableConst(principal);
    if (table.len == 0) {
        serialWrite("  none\n");
        return;
    }

    var i: usize = 0;
    while (i < table.len) : (i += 1) {
        const cap = table.caps[i];
        serialWrite("  ");
        printHex(cap.paddr);
        if (!cap.rights.cpu_read and !cap.rights.cpu_write and cap.rights.dma) {
            serialWrite(" (dma)");
        }
        serialWrite("\n");
    }
}

fn dumpCapabilityView(state: *const kernel.KernelState) void {
    dumpPrincipalCaps(state, .Process0, "Process0");
    dumpPrincipalCaps(state, .Process1, "Process1");
    dumpPrincipalCaps(state, .Device0, "Device0");
}

const ExitBootResult = enum {
    success,
    failed,
};

fn exitBootServicesWithRetry() ExitBootResult {
    var mmap_buffer: [64 * 1024]u8 align(@alignOf(uefi.tables.MemoryDescriptor)) = undefined;

    const st = uefi.system_table;
    const bs = st.boot_services orelse return .failed;

    var attempt: usize = 0;
    while (attempt < 8) : (attempt += 1) {
        const mmap = bs.getMemoryMap(mmap_buffer[0..]) catch return .failed;
        bs.exitBootServices(uefi.handle, mmap.info.key) catch |err| switch (err) {
            // map key 競合は再取得で回復可能。
            error.InvalidParameter => continue,
            else => return .failed,
        };

        // UEFI仕様: ExitBootServices 後に該当ポインタを null 化し、CRC を再計算する。
        st.console_in_handle = null;
        st.con_in = null;
        st.console_out_handle = null;
        st.con_out = null;
        st.standard_error_handle = null;
        st.std_err = null;
        st.boot_services = null;
        st.hdr.crc32 = 0;
        const st_bytes = @as([*]u8, @ptrCast(st))[0..@as(usize, st.hdr.header_size)];
        st.hdr.crc32 = std.hash.Crc32.hash(st_bytes);

        return .success;
    }

    return .failed;
}

fn writeCr3(value: u64) void {
    asm volatile ("mov %[value], %%cr3"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

fn readCr3() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr3, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

fn invlpg(addr: u64) void {
    asm volatile ("invlpg (%[addr])"
        :
        : [addr] "r" (addr),
        : .{ .memory = true });
}

fn flushTlbForCr3Va(target_cr3: u64, va: u64) void {
    if (target_cr3 == 0) return;
    const current_cr3 = readCr3();
    if (current_cr3 == target_cr3) {
        invlpg(va);
        return;
    }

    writeCr3(target_cr3);
    invlpg(va);
    writeCr3(current_cr3);
}

fn flushUserTlbForPrincipalVa(principal: kernel.PrincipalId, va: u64) void {
    const space = getUserSpace(principal) orelse return;
    flushTlbForCr3Va(space.cr3, va);
}

fn installIdentityPageTables0To1GiB() bool {
    @memset(pml4_table[0..], 0);
    @memset(pdp_table[0..], 0);
    var pd_idx: usize = 0;
    while (pd_idx < pd_table_count) : (pd_idx += 1) {
        @memset(pd_tables[pd_idx][0..], 0);
    }

    const pml4_pa: u64 = @intFromPtr(&pml4_table);
    const pdp_pa: u64 = @intFromPtr(&pdp_table);
    const pd0_pa: u64 = @intFromPtr(&pd_tables[0]);

    // この段階では 0..4GiB を identity map する。テーブル実体も 4GiB 未満前提。
    if (pml4_pa >= four_gib or pdp_pa >= four_gib or pd0_pa >= four_gib) return false;

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

    writeCr3(pml4_pa);
    kernel_cr3_value = pml4_pa;
    return true;
}

fn hardenKernelMappingsSupervisorOnly() void {
    // kernel の既存 map は ring3 から見えないよう User ビットを強制的に落とす。
    pml4_table[0] &= ~page_user;

    var pdp_idx: usize = 0;
    while (pdp_idx < pd_table_count) : (pdp_idx += 1) {
        pdp_table[pdp_idx] &= ~page_user;

        var pd_idx: usize = 0;
        while (pd_idx < page_entries) : (pd_idx += 1) {
            pd_tables[pdp_idx][pd_idx] &= ~page_user;
        }
    }
}

fn buildUserAddressSpace(principal: kernel.PrincipalId, user_page_paddr: u64, user_stack_paddr: u64) bool {
    const space = getUserSpace(principal) orelse return false;
    @memset(space.pml4[0..], 0);
    @memset(space.pdp[0..], 0);
    @memset(space.pd[0..], 0);
    @memset(space.pt[0..], 0);

    const user_pml4_pa: u64 = @intFromPtr(&space.pml4);
    const user_pdp_pa: u64 = @intFromPtr(&space.pdp);
    const user_pd_pa: u64 = @intFromPtr(&space.pd);
    const user_pt_pa: u64 = @intFromPtr(&space.pt);
    if (user_pml4_pa >= four_gib or user_pdp_pa >= four_gib or user_pd_pa >= four_gib or user_pt_pa >= four_gib) return false;
    if (user_page_paddr >= four_gib or user_stack_paddr >= four_gib) return false;

    const pdp_index: usize = @intCast((user_va >> 30) & 0x1FF);
    const pd_index: usize = @intCast((user_va >> 21) & 0x1FF);
    const user_pt_index: usize = @intCast((user_va >> 12) & 0x1FF);
    const stack_pt_index: usize = @intCast((user_stack_page_va >> 12) & 0x1FF);
    const stack_pd_index: usize = @intCast((user_stack_page_va >> 21) & 0x1FF);
    if (stack_pd_index != pd_index) return false;

    // user CR3 は最小構成: user mapping + 例外/割り込み入口に必要な supervisor bridge のみ。
    space.pml4[0] = user_pdp_pa | page_present | page_rw | page_user;

    space.pdp[pdp_index] = user_pd_pa | page_present | page_rw | page_user;
    space.pd[pd_index] = user_pt_pa | page_present | page_rw | page_user;
    space.pt[user_pt_index] = user_page_paddr | page_present | page_rw | page_user;
    space.pt[stack_pt_index] = user_stack_paddr | page_present | page_rw | page_user;

    const bridge_ranges = [_]struct { start: u64, len: usize }{
        .{ .start = @intFromPtr(&int80_trampoline_page), .len = @sizeOf(@TypeOf(int80_trampoline_page)) },
        .{ .start = @intFromPtr(&pf_trampoline_page), .len = @sizeOf(@TypeOf(pf_trampoline_page)) },
        .{ .start = @intFromPtr(&gp_trampoline_page), .len = @sizeOf(@TypeOf(gp_trampoline_page)) },
        .{ .start = @intFromPtr(&df_trampoline_page), .len = @sizeOf(@TypeOf(df_trampoline_page)) },
        .{ .start = @intFromPtr(&ud_trampoline_page), .len = @sizeOf(@TypeOf(ud_trampoline_page)) },
        .{ .start = @intFromPtr(&ts_trampoline_page), .len = @sizeOf(@TypeOf(ts_trampoline_page)) },
        .{ .start = @intFromPtr(&np_trampoline_page), .len = @sizeOf(@TypeOf(np_trampoline_page)) },
        .{ .start = @intFromPtr(&ss_trampoline_page), .len = @sizeOf(@TypeOf(ss_trampoline_page)) },
        .{ .start = @intFromPtr(&enterUserModeIretq), .len = 1 },
        .{ .start = @intFromPtr(&syscallHandlerStub), .len = 1 },
        .{ .start = @intFromPtr(&pageFaultHandlerStub), .len = 1 },
        .{ .start = @intFromPtr(&generalProtectionHandlerStub), .len = 1 },
        .{ .start = @intFromPtr(&doubleFaultHandlerStub), .len = 1 },
        .{ .start = @intFromPtr(&invalidOpcodeHandlerStub), .len = 1 },
        .{ .start = @intFromPtr(&invalidTssHandlerStub), .len = 1 },
        .{ .start = @intFromPtr(&segmentNotPresentHandlerStub), .len = 1 },
        .{ .start = @intFromPtr(&stackSegmentFaultHandlerStub), .len = 1 },
        .{ .start = @intFromPtr(&idt), .len = @sizeOf(@TypeOf(idt)) },
        .{ .start = @intFromPtr(&gdt), .len = @sizeOf(@TypeOf(gdt)) },
        .{ .start = @intFromPtr(&tss), .len = @sizeOf(@TypeOf(tss)) },
        .{ .start = @intFromPtr(&ring0_stack), .len = ring0_stack.len },
    };
    for (bridge_ranges) |r| {
        const start = pageAlignDown(r.start);
        const end = pageAlignUp(r.start + r.len);
        var va = start;
        while (va < end) : (va += 4096) {
            if (va >= four_gib) return false;
            const b_pdp_idx: usize = @intCast((va >> 30) & 0x1FF);
            const b_pd_idx: usize = @intCast((va >> 21) & 0x1FF);
            const b_pt_idx: usize = @intCast((va >> 12) & 0x1FF);
            if (b_pdp_idx != pdp_index) return false;
            if (b_pd_idx == pd_index) {
                if (b_pt_idx == user_pt_index or b_pt_idx == stack_pt_index) continue;
                space.pt[b_pt_idx] = va | page_present | page_rw;
            } else {
                space.pd[b_pd_idx] = pd_tables[0][b_pd_idx] & ~page_user;
            }
        }
    }

    space.cr3 = user_pml4_pa;
    return true;
}

fn buildUserAddressSpaceFromCapabilities(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    user_page: kernel.PageCapability,
    user_stack_page: kernel.PageCapability,
) bool {
    const table = state.getTableConst(principal);
    const user_cap = table.find(user_page.paddr) orelse return false;
    const stack_cap = table.find(user_stack_page.paddr) orelse return false;
    if (!user_cap.rights.cpu_read or !user_cap.rights.cpu_write) return false;
    if (!stack_cap.rights.cpu_read or !stack_cap.rights.cpu_write) return false;
    return buildUserAddressSpace(principal, user_cap.paddr, stack_cap.paddr);
}

fn installUserMemoryWritePfTestCode(user_page_paddr: u64) void {
    user_programs.installMemoryWritePfTestCode(user_program_cfg, user_page_paddr);
}

fn installUserGeneralProtectionTestCode(user_page_paddr: u64) void {
    user_programs.installGeneralProtectionTestCode(user_page_paddr);
}

fn installUserPfRecoveryDemoCode(user_page_paddr: u64) void {
    user_programs.installPfRecoveryDemoCode(user_program_cfg, user_page_paddr);
}

fn installUserPfRecoveryThenSwitchCode(user_page_paddr: u64, target_process: u64) void {
    user_programs.installPfRecoveryThenSwitchCode(user_program_cfg, user_page_paddr, target_process);
}

fn installUserDmaUnmapVerifyCode(user_page_paddr: u64) void {
    user_programs.installDmaUnmapVerifyCode(user_program_cfg, user_page_paddr);
}

fn enterUserModeIretq(user_entry_va: u64, user_rsp: u64) noreturn {
    const user_cs: u64 = gdt_user_code_selector | 0x3;
    const user_ss: u64 = gdt_user_data_selector | 0x3;
    const user_rflags: u64 = 0x2; // IF=0 で外部割り込みを抑止
    const kernel_transition_rsp: u64 = @intFromPtr(&ring0_stack) + ring0_stack.len;

    asm volatile (
        \\mov %[k_rsp], %%rsp
        \\pushq %[ss]
        \\pushq %[rsp]
        \\pushq %[rflags]
        \\pushq %[cs]
        \\pushq %[rip]
        \\mov %[ucr3], %%rax
        \\mov %%rax, %%cr3
        \\iretq
        :
        : [ss] "r" (user_ss),
          [rsp] "r" (user_rsp),
          [rflags] "r" (user_rflags),
          [cs] "r" (user_cs),
          [rip] "r" (user_entry_va),
          [k_rsp] "r" (kernel_transition_rsp),
          [ucr3] "r" (user_cr3_value),
        : .{ .memory = true });
    unreachable;
}

fn syncPageTableRightsForPaddr(state: *const kernel.KernelState, paddr: u64) void {
    const user_pt_base_va = user_va & ~@as(u64, 0x1F_FFFF);
    const principals = [_]kernel.PrincipalId{ .Process0, .Process1 };
    for (principals) |principal| {
        const space = getUserSpace(principal) orelse continue;
        const cap = state.getTableConst(principal).find(paddr);
        var kept_one = false;

        var i: usize = 0;
        while (i < page_entries) : (i += 1) {
            const old_entry = space.pt[i];
            const mapped_paddr = old_entry & page_addr_mask;
            if (mapped_paddr != paddr) continue;

            var new_entry: u64 = 0;
            if (cap) |c| {
                if (c.rights.cpu_read and !kept_one) {
                    new_entry = paddr | page_user | page_present;
                    if (c.rights.cpu_write) new_entry |= page_rw;
                    kept_one = true;
                }
            }

            if (new_entry == old_entry) continue;
            space.pt[i] = new_entry;
            const va = user_pt_base_va + (@as(u64, i) * 4096);
            flushUserTlbForPrincipalVa(principal, va);
        }
    }
}

fn pageAlignDown(addr: u64) u64 {
    return addr & ~@as(u64, 4095);
}

fn pageAlignUp(addr: u64) u64 {
    return (addr + 4095) & ~@as(u64, 4095);
}

fn isReserved(paddr: u64, reserved: []const ReservedRange) bool {
    for (reserved) |r| {
        if (paddr >= r.start and paddr < r.end) return true;
    }
    return false;
}

fn collectMemoryStatsAndFreePages(
    bs: *uefi.tables.BootServices,
    free_list: *kernel.FreePageList,
) ?MemoryStats {
    var mmap_buffer: [64 * 1024]u8 align(@alignOf(uefi.tables.MemoryDescriptor)) = undefined;
    const mmap = bs.getMemoryMap(mmap_buffer[0..]) catch return null;

    free_list.* = .{};
    var detected_regions: usize = 0;
    var total_usable_bytes: u64 = 0;
    const pml4_start = pageAlignDown(@intFromPtr(&pml4_table));
    const pml4_end = pageAlignUp(@intFromPtr(&pml4_table) + @sizeOf(@TypeOf(pml4_table)));
    const pdp_start = pageAlignDown(@intFromPtr(&pdp_table));
    const pdp_end = pageAlignUp(@intFromPtr(&pdp_table) + @sizeOf(@TypeOf(pdp_table)));
    const pd_start = pageAlignDown(@intFromPtr(&pd_tables));
    const pd_end = pageAlignUp(@intFromPtr(&pd_tables) + @sizeOf(@TypeOf(pd_tables)));
    const user_spaces_start = pageAlignDown(@intFromPtr(&user_spaces));
    const user_spaces_end = pageAlignUp(@intFromPtr(&user_spaces) + @sizeOf(@TypeOf(user_spaces)));
    const free_list_start = pageAlignDown(@intFromPtr(&global_free_list));
    const free_list_end = pageAlignUp(@intFromPtr(&global_free_list) + @sizeOf(@TypeOf(global_free_list)));
    const kernel_state_start = pageAlignDown(@intFromPtr(&kernel_state_global));
    const kernel_state_end = pageAlignUp(@intFromPtr(&kernel_state_global) + @sizeOf(@TypeOf(kernel_state_global)));
    const ring0_stack_start = pageAlignDown(@intFromPtr(&ring0_stack));
    const ring0_stack_end = pageAlignUp(@intFromPtr(&ring0_stack) + @sizeOf(@TypeOf(ring0_stack)));
    const gdt_start = pageAlignDown(@intFromPtr(&gdt));
    const gdt_end = pageAlignUp(@intFromPtr(&gdt) + @sizeOf(@TypeOf(gdt)));
    const idt_start = pageAlignDown(@intFromPtr(&idt));
    const idt_end = pageAlignUp(@intFromPtr(&idt) + @sizeOf(@TypeOf(idt)));
    const tss_start = pageAlignDown(@intFromPtr(&tss));
    const tss_end = pageAlignUp(@intFromPtr(&tss) + @sizeOf(@TypeOf(tss)));
    const tramp_start = pageAlignDown(@intFromPtr(&int80_trampoline_page));
    const tramp_end = pageAlignUp(@intFromPtr(&ss_trampoline_page) + @sizeOf(@TypeOf(ss_trampoline_page)));
    const mmap_start = pageAlignDown(@intFromPtr(&mmap_buffer));
    const mmap_end = pageAlignUp(@intFromPtr(&mmap_buffer) + mmap_buffer.len);
    // カーネル自身が使う最低限の領域は free list から除外する。
    const reserved = [_]ReservedRange{
        .{ .start = 0, .end = reserved_low_mem_end },
        .{ .start = pml4_start, .end = pml4_end },
        .{ .start = pdp_start, .end = pdp_end },
        .{ .start = pd_start, .end = pd_end },
        .{ .start = user_spaces_start, .end = user_spaces_end },
        .{ .start = free_list_start, .end = free_list_end },
        .{ .start = kernel_state_start, .end = kernel_state_end },
        .{ .start = ring0_stack_start, .end = ring0_stack_end },
        .{ .start = gdt_start, .end = gdt_end },
        .{ .start = idt_start, .end = idt_end },
        .{ .start = tss_start, .end = tss_end },
        .{ .start = tramp_start, .end = tramp_end },
        .{ .start = mmap_start, .end = mmap_end },
    };

    var it = mmap.iterator();
    while (it.next()) |desc| {
        if (desc.type == .conventional_memory) {
            const region_id = detected_regions;
            var i: u64 = 0;
            while (i < desc.number_of_pages) : (i += 1) {
                const paddr = desc.physical_start + (i * 4096);
                if (isReserved(paddr, reserved[0..])) continue;
                free_list.appendPage(region_id, paddr) catch return null;
            }
            detected_regions += 1;
            total_usable_bytes += desc.number_of_pages * 4096;
        }
    }

    return .{
        .detected_regions = detected_regions,
        .total_usable_bytes = total_usable_bytes,
    };
}

pub fn main() void {
    serialInit();
    serialWrite("[stage] boot entry\n");
    serialWrite("SakuraMicroKernel Phase1 boot\n");
    _ = gdt_user_code_selector;
    _ = gdt_user_data_selector;

    const bs = uefi.system_table.boot_services orelse {
        serialWrite("boot services missing\n");
        while (true) asm volatile ("hlt");
    };
    const memory_stats = collectMemoryStatsAndFreePages(bs, &global_free_list) orelse {
        serialWrite("memory map parse failed\n");
        while (true) asm volatile ("hlt");
    };
    serialWrite("[stage] memory stats collected\n");
    serialWrite("Detected ");
    printNumber(memory_stats.detected_regions);
    serialWrite(" regions\n");

    const total_usable_mb = memory_stats.total_usable_bytes / (1024 * 1024);
    serialWrite("Total usable memory: ");
    printNumber(total_usable_mb);
    serialWrite("MB\n");
    serialWrite("free pages: ");
    printNumber(global_free_list.len);
    serialWrite("\n");

    if (debug_skip_exit_boot_services) {
        serialWrite("[debug] skip ExitBootServices\n");
    } else {
        serialWrite("try ExitBootServices...\n");
        switch (exitBootServicesWithRetry()) {
            .success => serialWrite("ExitBootServices success\n"),
            .failed => {
                serialWrite("ExitBootServices failed\n");
                while (true) asm volatile ("hlt");
            },
        }
        serialWrite("UEFI services terminated\n");
    }
    loadGdtAndReloadSegments();
    serialWrite("GDT loaded (kernel/user segments)\n");

    if (debug_skip_cr3_switch) {
        serialWrite("[debug] skip CR3 switch\n");
    } else {
        serialWrite("build page tables (identity 0..4GiB)\n");
        if (!installIdentityPageTables0To1GiB()) {
            serialWrite("page table install failed\n");
            while (true) asm volatile ("hlt");
        }
        hardenKernelMappingsSupervisorOnly();
        installInterruptTrampolines();
        serialWrite("CR3 switched to custom PML4\n");
    }
    initIdtPageFaultOnly();
    serialWrite("IDT loaded (#PF/#GP/#DF/#INT80)\n");
    if (debug_trigger_page_fault_test) {
        triggerPageFaultTest();
    }
    serialWrite("enter bare-metal capability demo\n");

    var state = kernel.KernelState.initFromDetectedRegions(memory_stats.detected_regions) catch |err| {
        serialWrite("region init failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    state.pte_sync_hook = syncPageTableRightsForPaddr;
    const user_page = state.allocPageTo(.Process0, &global_free_list) catch |err| {
        serialWrite("allocPageTo for user map failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    const user_stack_page = state.allocPageTo(.Process0, &global_free_list) catch |err| {
        serialWrite("allocPageTo for user stack failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    if (!buildUserAddressSpaceFromCapabilities(&state, .Process0, user_page, user_stack_page)) {
        serialWrite("user page table build failed\n");
        while (true) asm volatile ("hlt");
    }
    const user_page_p1 = state.allocPageTo(.Process1, &global_free_list) catch |err| {
        serialWrite("allocPageTo for user map p1 failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    const user_stack_page_p1 = state.allocPageTo(.Process1, &global_free_list) catch |err| {
        serialWrite("allocPageTo for user stack p1 failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    if (!buildUserAddressSpaceFromCapabilities(&state, .Process1, user_page_p1, user_stack_page_p1)) {
        serialWrite("user page table build failed (p1)\n");
        while (true) asm volatile ("hlt");
    }

    current_user_principal = .Process0;
    user_cr3_value = currentUserSpace().cr3;
    serialWrite("user page table ready\n");
    serialWrite("  user_va=");
    printHex(user_va);
    serialWrite("\n");
    serialWrite("  user_pa=");
    printHex(user_page.paddr);
    serialWrite("\n");
    serialWrite("  user_stack_top=");
    printHex(user_stack_top);
    serialWrite("\n");
    serialWrite("  user_stack_pa=");
    printHex(user_stack_page.paddr);
    serialWrite("\n");
    serialWrite("  process_count=");
    printNumber(user_process_count);
    serialWrite("\n");
    serialWrite("  process0_cr3=");
    printHex(user_spaces[0].cr3);
    serialWrite("\n");
    serialWrite("  process1_cr3=");
    printHex(user_spaces[1].cr3);
    serialWrite("\n");
    if (debug_trigger_general_protection_test) {
        installUserGeneralProtectionTestCode(user_page.paddr);
    } else if (debug_trigger_dma_unmap_verify_demo) {
        installUserDmaUnmapVerifyCode(user_page.paddr);
    } else if (debug_trigger_pf_recovery_demo) {
        installUserPfRecoveryDemoCode(user_page.paddr);
    } else {
        // 標準フロー:
        // Process0 で #PF recover を確認し、syscall で Process1 へ切替、
        // Process1 でも同シナリオを実行して最終 fatal #PF で停止する。
        installUserPfRecoveryThenSwitchCode(user_page.paddr, 1);
        installUserPfRecoveryDemoCode(user_page_p1.paddr);
    }

    var allocated: [3]u64 = undefined;
    var i: usize = 0;
    while (i < 3) : (i += 1) {
        // free list から Process0 へ 3ページ配布する。
        const cap = state.allocPageTo(.Process0, &global_free_list) catch |err| {
            serialWrite("allocPageTo failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
        allocated[i] = cap.paddr;
    }

    dumpCapabilityView(&state);
    serialWrite("\nstart_dma ");
    printHex(allocated[1]);
    serialWrite("\n\n");

    if (debug_trigger_dma_unmap_verify_demo) {
        if (!mapUserPageFromCapability(&state, .Process0, user_dma_verify_va, allocated[1], true)) {
            serialWrite("dma verify map setup failed\n");
            while (true) asm volatile ("hlt");
        }
        serialWrite("dma verify map prepared: ");
        printHex(user_dma_verify_va);
        serialWrite(" -> ");
        printHex(allocated[1]);
        serialWrite("\n");
    }

    state.startDma(allocated[1]) catch |err| {
        serialWrite("DMA start failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        return;
    };
    dumpCapabilityView(&state);
    kernel_state_global = state;
    kernel_state_ready = true;
    if (debug_trigger_general_protection_test) {
        serialWrite("\nenter ring3 with iretq (expected #GP by user CLI)\n");
    } else if (debug_trigger_dma_unmap_verify_demo) {
        serialWrite("\nenter ring3 with iretq (expected immediate #PF after start_dma unmap)\n");
    } else if (debug_trigger_pf_recovery_demo) {
        serialWrite("\nenter ring3 with iretq (expected #PF recover + final fatal #PF)\n");
    } else {
        serialWrite("\nenter ring3 with iretq (std #PF recover: Process0 then Process1)\n");
    }
    enterUserModeIretq(user_va, user_stack_top);
}
