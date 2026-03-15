const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const interrupts = @import("interrupts.zig");
const lapic = @import("lapic.zig");
const scheduler = @import("scheduler.zig");

const TrapFrame = interrupts.TrapFrame;
const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;

const debug_skip_syscall_fx_state = true;
const debug_skip_timer_fx_state = true;
const enable_process5_frame_debug_logs = false;
const trap_frame_qword_count = @sizeOf(TrapFrame) / @sizeOf(u64);
const exception_trap_frame_qword_count = @sizeOf(ExceptionTrapFrame) / @sizeOf(u64);
const user_return_gpr_qword_count = @offsetOf(TrapFrame, "rip") / @sizeOf(u64);
const user_return_iret_qword_count = 5;
const trap_frame_iret_offset = @offsetOf(TrapFrame, "rip");
const exception_trap_frame_iret_offset = @offsetOf(ExceptionTrapFrame, "rip");

pub const Hooks = struct {
    kernel_state_ready: *const bool,
    state: *kernel.KernelState,
    scheduler_quantum_ticks: u64,
    compositor_hold_quanta: u64,
    scheduler_log_switch: bool,
    scheduler_switch_log_max_lines: u64,
    write: *const fn ([]const u8) void,
    write_hex_raw: *const fn (u64) void,
    write_bool01: *const fn (bool) void,
    thread_label: *const fn (usize) []const u8,
    principal_label: *const fn (kernel.PrincipalId) []const u8,
    read_cr2: *const fn () u64,
    read_cr3: *const fn () u64,
    dump_page_walk_for_va: *const fn (u64, u64) void,
    log_page_fault_step2: *const fn (u64, *const ExceptionTrapFrame) void,
    halt_loop: *const fn () noreturn,
    maybe_log_scheduler_perf_tick: *const fn () void,
    try_start_bootlog_gate_deferred_input: *const fn () void,
    try_auto_launch_deferred_compositor: *const fn (*TrapFrame) void,
    switch_to_thread: *const fn (usize, *TrapFrame, ?u64) bool,
    log_race_switch: *const fn (usize, usize, []const u8) void,
};

var hooks: ?Hooks = null;
var process5_frame_log_count: u64 = 0;
const process5_frame_log_max: u64 = 128;
pub export var timer_entry_saved_rax: u64 = 0;
pub export var timer_entry_pushed_rax: u64 = 0;
pub export var timer_stack_rax_post_dispatch: u64 = 0;
pub export var timer_stack_rax_pre_log: u64 = 0;
pub export var last_user_return_rax: u64 = 0;
pub export var last_user_return_rip: u64 = 0;
pub export var last_stage_source_rax: u64 = 0;
pub export var last_stage_saved_rax: u64 = 0;
pub export var last_stage_source_rip: u64 = 0;
pub export var last_stage_saved_rip: u64 = 0;
pub export var page_fault_work_frame: ExceptionTrapFrame = std.mem.zeroes(ExceptionTrapFrame);
pub export var timer_interrupt_work_frame: TrapFrame = std.mem.zeroes(TrapFrame);
pub export var syscall_interrupt_work_frame: TrapFrame = std.mem.zeroes(TrapFrame);

pub fn init(new_hooks: Hooks) void {
    hooks = new_hooks;
}

fn getHooks() *const Hooks {
    return &(hooks orelse unreachable);
}

extern var kernel_cr3_value: u64;
extern var user_cr3_value: u64;
extern var user_return_saved_r10: u64;
extern var user_return_saved_gprs: [15]u64 align(16);
extern var user_return_iret_frame: [5]u64 align(16);
extern var syscall_return_writeback_enabled: u64;

extern fn saveCurrentThreadFxState() callconv(.c) void;
extern fn restoreCurrentThreadFxState() callconv(.c) void;
extern fn syscallDispatch(frame: *TrapFrame) callconv(.c) u64;

fn asmCopyStackFrameToWorkFrame(comptime work_frame_symbol: []const u8, comptime qword_count: usize) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\lea {s}(%rip), %rdi
        \\mov %rsp, %rsi
        \\mov ${d}, %ecx
        \\cld
        \\rep movsq
        \\
    , .{ work_frame_symbol, qword_count });
}

fn asmStageUserReturnFromWorkFrame(comptime work_frame_symbol: []const u8, comptime iret_offset: usize) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\lea {s}(%rip), %rsi
        \\lea user_return_saved_gprs(%rip), %rdi
        \\mov ${d}, %ecx
        \\cld
        \\rep movsq
        \\mov {s}+112(%rip), %r10
        \\mov %r10, last_stage_source_rax(%rip)
        \\mov user_return_saved_gprs+112(%rip), %r10
        \\mov %r10, last_stage_saved_rax(%rip)
        \\mov {s}+{d}(%rip), %r10
        \\mov %r10, last_stage_source_rip(%rip)
        \\lea {s}+{d}(%rip), %rsi
        \\lea user_return_iret_frame(%rip), %rdi
        \\mov ${d}, %ecx
        \\cld
        \\rep movsq
        \\mov user_return_iret_frame(%rip), %r10
        \\mov %r10, last_stage_saved_rip(%rip)
        \\
    , .{
        work_frame_symbol,
        user_return_gpr_qword_count,
        work_frame_symbol,
        work_frame_symbol,
        iret_offset,
        work_frame_symbol,
        iret_offset,
        user_return_iret_qword_count,
    });
}

fn exceptionName(vec: u64) []const u8 {
    return switch (vec) {
        13 => "GENERAL PROTECTION",
        14 => "PAGE FAULT",
        else => "EXCEPTION",
    };
}

fn writeReg(h: *const Hooks, label: []const u8, value: u64) void {
    h.write("  ");
    h.write(label);
    h.write("=");
    h.write_hex_raw(value);
    h.write("\n");
}

fn writeByteHex(h: *const Hooks, byte: u8) void {
    var buf: [2]u8 = undefined;
    buf[0] = std.fmt.digitToChar(@intCast((byte >> 4) & 0xF), .lower);
    buf[1] = std.fmt.digitToChar(@intCast(byte & 0xF), .lower);
    h.write(buf[0..]);
}

fn logProcess5RipBytes(h: *const Hooks, rip: u64, label: []const u8, before: usize, after: usize) void {
    if (rip < 0x20000000) return;
    const rip_page_va = rip & ~@as(u64, 0xFFF);
    const rip_page_paddr = capability.lookupUserMappedPaddrForVa(.Process5, rip_page_va);
    if (rip_page_paddr) |page_paddr| {
        const page_bytes: [*]const u8 = @ptrFromInt(page_paddr);
        const rip_off: usize = @intCast(rip & 0xFFF);
        const start_off: usize = rip_off -| before;
        const end_off: usize = @min(rip_off + after, 4095);

        h.write("  ");
        h.write(label);
        h.write("=");
        var off: usize = start_off;
        while (off <= end_off) : (off += 1) {
            if (off != start_off) h.write(" ");
            writeByteHex(h, page_bytes[off]);
        }
        h.write("\n");
    }
}

fn logProcess5StackQwords(h: *const Hooks, rsp: u64, label: []const u8, max_qwords: usize) void {
    const stack_page_va = rsp & ~@as(u64, 0xFFF);
    const stack_page_paddr = capability.lookupUserMappedPaddrForVa(.Process5, stack_page_va);
    h.write("  ");
    h.write(label);
    h.write("_PAGE=");
    if (stack_page_paddr) |paddr| {
        h.write_hex_raw(paddr);
        h.write("\n");

        const page_words: [*]const u64 = @ptrFromInt(paddr);
        const word_off: usize = @intCast((rsp & 0xFFF) / @sizeOf(u64));
        const words_left = 512 - word_off;
        const count: usize = @min(max_qwords, words_left);

        h.write("  ");
        h.write(label);
        h.write("=");
        var i: usize = 0;
        while (i < count) : (i += 1) {
            if (i != 0) h.write(" ");
            h.write_hex_raw(page_words[word_off + i]);
        }
        h.write("\n");
    } else {
        h.write("none\n");
    }
}

fn logProcess5FaultBytes(h: *const Hooks, frame: *const ExceptionTrapFrame) void {
    if (scheduler.current_user_principal != .Process5) return;
    if (frame.rip < 0x20000000) return;

    const rip_page_va = frame.rip & ~@as(u64, 0xFFF);
    const rip_page_paddr = capability.lookupUserMappedPaddrForVa(.Process5, rip_page_va);
    const vfb_page_paddr = capability.lookupUserMappedPaddrForVa(.Process5, 0x2030_0000);

    h.write("  PROC5.RIP_PAGE=");
    if (rip_page_paddr) |paddr| {
        h.write_hex_raw(paddr);
    } else {
        h.write("none");
    }
    h.write("\n");

    h.write("  PROC5.VFB_PAGE=");
    if (vfb_page_paddr) |paddr| {
        h.write_hex_raw(paddr);
    } else {
        h.write("none");
    }
    h.write("\n");

    logProcess5RipBytes(h, frame.rip, "PROC5.RIP_BYTES", 8, 8);
    logProcess5RipBytes(h, frame.rip, "PROC5.RIP_BYTES_WIDE", 24, 8);
}

fn maybeLogProcess5Frame(stage: []const u8, principal: kernel.PrincipalId, frame: *const TrapFrame) void {
    if (!enable_process5_frame_debug_logs) return;
    if (principal != .Process5) return;
    if (frame.rip < 0x20001810 or frame.rip > 0x20001832) return;
    if (process5_frame_log_count >= process5_frame_log_max) return;
    if (!std.mem.eql(u8, stage, "timer-ret") and !std.mem.eql(u8, stage, "staged-ret")) return;
    process5_frame_log_count +%= 1;

    const h = getHooks();
    h.write("PROC5 frame ");
    h.write(stage);
    h.write("\n");
    writeReg(h, "RIP", frame.rip);
    writeReg(h, "ENTRY_RAX", timer_entry_saved_rax);
    writeReg(h, "PUSHED_RAX", timer_entry_pushed_rax);
    writeReg(h, "STACK_RAX_POST_DISPATCH", timer_stack_rax_post_dispatch);
    writeReg(h, "STACK_RAX_PRE_LOG", timer_stack_rax_pre_log);
    writeReg(h, "STAGE_SRC_RIP", last_stage_source_rip);
    writeReg(h, "STAGE_SRC_RAX", last_stage_source_rax);
    writeReg(h, "STAGE_SAVED_RIP", last_stage_saved_rip);
    writeReg(h, "STAGE_SAVED_RAX", last_stage_saved_rax);
    writeReg(h, "LAST_IRET_RIP", last_user_return_rip);
    writeReg(h, "LAST_IRET_RAX", last_user_return_rax);
    writeReg(h, "RAX", frame.rax);
    writeReg(h, "RDI", frame.rdi);
    writeReg(h, "RSI", frame.rsi);
    writeReg(h, "RDX", frame.rdx);
    writeReg(h, "RCX", frame.rcx);
    writeReg(h, "R8", frame.r8);
    writeReg(h, "R9", frame.r9);
    writeReg(h, "RSP", frame.rsp);
    if (frame.rax == 0) {
        logProcess5RipBytes(h, frame.rip, "PROC5.TIMER_RIP_BYTES_WIDE", 24, 8);
        logProcess5StackQwords(h, frame.rsp, "PROC5.STACK_QWORDS", 8);
    }
}

pub export fn logTimerReturnFrame(frame: *const TrapFrame) callconv(.c) void {
    maybeLogProcess5Frame("timer-ret", scheduler.current_user_principal, frame);
}

pub export fn logCurrentStagedUserReturnFrame() callconv(.c) void {
    var frame: TrapFrame = std.mem.zeroes(TrapFrame);
    frame.r15 = user_return_saved_gprs[0];
    frame.r14 = user_return_saved_gprs[1];
    frame.r13 = user_return_saved_gprs[2];
    frame.r12 = user_return_saved_gprs[3];
    frame.r11 = user_return_saved_gprs[4];
    frame.r10 = user_return_saved_gprs[5];
    frame.r9 = user_return_saved_gprs[6];
    frame.r8 = user_return_saved_gprs[7];
    frame.rbp = user_return_saved_gprs[8];
    frame.rdi = user_return_saved_gprs[9];
    frame.rsi = user_return_saved_gprs[10];
    frame.rdx = user_return_saved_gprs[11];
    frame.rcx = user_return_saved_gprs[12];
    frame.rbx = user_return_saved_gprs[13];
    frame.rax = user_return_saved_gprs[14];
    frame.rip = user_return_iret_frame[0];
    frame.cs = user_return_iret_frame[1];
    frame.rflags = user_return_iret_frame[2];
    frame.rsp = user_return_iret_frame[3];
    frame.ss = user_return_iret_frame[4];
    maybeLogProcess5Frame("staged-ret", scheduler.current_user_principal, &frame);
}

pub export fn userReturnToSavedFrame() callconv(.naked) noreturn {
    asm volatile (
        \\mov user_return_saved_gprs+112(%rip), %r10
        \\mov %r10, last_user_return_rax(%rip)
        \\mov user_return_iret_frame(%rip), %r10
        \\mov %r10, last_user_return_rip(%rip)
        \\mov user_return_saved_gprs(%rip), %r15
        \\mov user_return_saved_gprs+8(%rip), %r14
        \\mov user_return_saved_gprs+16(%rip), %r13
        \\mov user_return_saved_gprs+24(%rip), %r12
        \\mov user_return_saved_gprs+32(%rip), %r11
        \\mov user_return_saved_gprs+48(%rip), %r9
        \\mov user_return_saved_gprs+56(%rip), %r8
        \\mov user_return_saved_gprs+64(%rip), %rbp
        \\mov user_return_saved_gprs+72(%rip), %rdi
        \\mov user_return_saved_gprs+80(%rip), %rsi
        \\mov user_return_saved_gprs+88(%rip), %rdx
        \\mov user_return_saved_gprs+96(%rip), %rcx
        \\mov user_return_saved_gprs+104(%rip), %rbx
        \\mov user_return_saved_gprs+112(%rip), %rax
        \\lea user_return_iret_frame(%rip), %rsp
        \\mov user_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\mov user_return_saved_gprs+40(%rip), %r10
        \\iretq
    );
}

pub export fn pageFaultDispatch(frame: *const ExceptionTrapFrame) callconv(.c) u64 {
    const h = getHooks();
    const cr2 = h.read_cr2();
    const pf_cap = capability.issuePageFaultCapability(scheduler.current_user_principal, frame, cr2) orelse return 0;
    if (!h.kernel_state_ready.*) return 0;
    if (!capability.resolvePageFaultCapability(h.state, pf_cap)) return 0;

    h.write("PAGE FAULT RESOLVED\n");
    h.write("  CR2=");
    h.write_hex_raw(cr2);
    h.write("\n");
    h.write("  PF_CAP=consumed\n");
    return 1;
}

pub export fn exceptionWithErrorCommon(vec: u64, frame: *const ExceptionTrapFrame) callconv(.c) noreturn {
    const h = getHooks();
    asm volatile ("cli");
    h.write(exceptionName(vec));
    h.write("\n");
    h.write("  THREAD=");
    h.write(h.thread_label(scheduler.current_thread_index));
    h.write("\n");
    h.write("  PRINCIPAL=");
    h.write(h.principal_label(scheduler.current_user_principal));
    h.write("\n");
    if (vec == 14) {
        const cr2 = h.read_cr2();
        h.write("  CR2=");
        h.write_hex_raw(cr2);
        h.write("\n");
        h.dump_page_walk_for_va(h.read_cr3(), cr2);
        h.log_page_fault_step2(cr2, frame);
    }
    h.write("  EC=");
    h.write_hex_raw(frame.error_code);
    h.write("\n");
    h.write("  RIP=");
    h.write_hex_raw(frame.rip);
    h.write("\n");
    writeReg(h, "RAX", frame.rax);
    writeReg(h, "RBX", frame.rbx);
    writeReg(h, "RCX", frame.rcx);
    writeReg(h, "RDX", frame.rdx);
    writeReg(h, "RSI", frame.rsi);
    writeReg(h, "RDI", frame.rdi);
    writeReg(h, "R8", frame.r8);
    writeReg(h, "R9", frame.r9);
    writeReg(h, "RBP", frame.rbp);
    writeReg(h, "RSP", frame.rsp);
    if (vec == 14) {
        logProcess5FaultBytes(h, frame);
        if (scheduler.current_user_principal == .Process5 and frame.rax == 0) {
            logProcess5StackQwords(h, frame.rsp, "PROC5.STACK_QWORDS", 8);
        }
    }
    h.halt_loop();
}

pub export fn doubleFaultHandlerCommon(error_code: u64) callconv(.c) noreturn {
    const h = getHooks();
    asm volatile ("cli");
    h.write("DOUBLE FAULT\n");
    h.write("  EC=");
    h.write_hex_raw(error_code);
    h.write("\n");
    h.halt_loop();
}

pub export fn invalidTssHandlerCommon(error_code: u64) callconv(.c) noreturn {
    const h = getHooks();
    asm volatile ("cli");
    h.write("INVALID TSS\n");
    h.write("  EC=");
    h.write_hex_raw(error_code);
    h.write("\n");
    h.halt_loop();
}

pub export fn segmentNotPresentHandlerCommon(error_code: u64) callconv(.c) noreturn {
    const h = getHooks();
    asm volatile ("cli");
    h.write("SEGMENT NOT PRESENT\n");
    h.write("  EC=");
    h.write_hex_raw(error_code);
    h.write("\n");
    h.halt_loop();
}

pub export fn stackSegmentFaultHandlerCommon(error_code: u64) callconv(.c) noreturn {
    const h = getHooks();
    asm volatile ("cli");
    h.write("STACK SEGMENT FAULT\n");
    h.write("  EC=");
    h.write_hex_raw(error_code);
    h.write("\n");
    h.halt_loop();
}

pub export fn invalidOpcodeHandlerCommon() callconv(.c) noreturn {
    const h = getHooks();
    asm volatile ("cli");
    h.write("INVALID OPCODE\n");
    h.halt_loop();
}

pub export fn timerInterruptDispatch(frame: *TrapFrame) callconv(.c) void {
    const h = getHooks();
    scheduler.lapic_tick_count +%= 1;
    lapic.eoiLegacyPicMaster();
    lapic.eoi();
    if (!h.kernel_state_ready.*) return;
    scheduler.wakeThreadsForTimer(scheduler.lapic_tick_count);
    if (h.scheduler_quantum_ticks == 0) return;
    const user_mode = ((frame.cs & 0x3) == 0x3) and ((frame.ss & 0x3) == 0x3);
    if (!user_mode) return;
    scheduler.noteUserTimerTick();
    h.maybe_log_scheduler_perf_tick();
    h.try_start_bootlog_gate_deferred_input();
    h.try_auto_launch_deferred_compositor(frame);

    const current_thread = scheduler.current_thread_index;
    const next_thread = scheduler.chooseNextThreadForTimerPreempt(
        h.scheduler_quantum_ticks,
        h.compositor_hold_quanta,
    ) orelse return;
    if (!h.switch_to_thread(next_thread, frame, null)) {
        h.log_race_switch(current_thread, next_thread, "timer_preempt_switch_failed");
        return;
    }
    scheduler.scheduler_switch_count +%= 1;
    if (h.scheduler_log_switch and scheduler.scheduler_switch_count <= h.scheduler_switch_log_max_lines) {
        const current_ctx = scheduler.getThreadContextConst(current_thread).?;
        const next_ctx = scheduler.getThreadContextConst(next_thread).?;
        h.write("SCHED switch ");
        h.write(h.thread_label(current_thread));
        h.write("/");
        h.write(h.principal_label(current_ctx.owner_process));
        h.write(" -> ");
        h.write(h.thread_label(next_thread));
        h.write("/");
        h.write(h.principal_label(next_ctx.owner_process));
        h.write("\n");
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
        \\mov 136(%rsp), %rax
        \\and $0x3, %rax
        \\cmp $0x3, %rax
        \\jne 9f
        \\sub $32, %rsp
        \\call saveCurrentThreadFxState
        \\add $32, %rsp
    ++ asmCopyStackFrameToWorkFrame("page_fault_work_frame", exception_trap_frame_qword_count) ++
        \\sub $32, %rsp
        \\lea page_fault_work_frame(%rip), %rcx
        \\call pageFaultDispatch
        \\add $32, %rsp
        \\test %rax, %rax
        \\jz 8f
        \\sub $32, %rsp
        \\call restoreCurrentThreadFxState
        \\add $32, %rsp
    ++ asmStageUserReturnFromWorkFrame("page_fault_work_frame", exception_trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\8:
        \\sub $32, %rsp
        \\mov $14, %rcx
        \\lea page_fault_work_frame(%rip), %rdx
        \\call exceptionWithErrorCommon
        \\ud2
        \\9:
        \\mov %rsp, %rcx
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\call pageFaultDispatch
        \\mov %r15, %rsp
        \\test %rax, %rax
        \\jz 1f
        \\mov 136(%rsp), %rax
        \\and $0x3, %rax
        \\cmp $0x3, %rax
        \\jne 2f
        \\sub $32, %rsp
        \\call restoreCurrentThreadFxState
        \\add $32, %rsp
        \\2:
        \\mov 128(%rsp), %r10
        \\mov %r10, user_return_iret_frame(%rip)
        \\mov 136(%rsp), %r10
        \\mov %r10, user_return_iret_frame+8(%rip)
        \\mov 144(%rsp), %r10
        \\mov %r10, user_return_iret_frame+16(%rip)
        \\mov 152(%rsp), %r10
        \\mov %r10, user_return_iret_frame+24(%rip)
        \\mov 160(%rsp), %r10
        \\mov %r10, user_return_iret_frame+32(%rip)
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
        \\mov %r10, user_return_saved_r10(%rip)
        \\lea user_return_iret_frame(%rip), %rsp
        \\mov user_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\mov user_return_saved_r10(%rip), %r10
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
    if (debug_skip_syscall_fx_state) {
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
        ++ asmCopyStackFrameToWorkFrame("syscall_interrupt_work_frame", trap_frame_qword_count) ++
            \\sub $32, %rsp
            \\lea syscall_interrupt_work_frame(%rip), %rcx
            \\call syscallDispatch
            \\add $32, %rsp
            \\cmpq $0, syscall_return_writeback_enabled(%rip)
            \\je 7f
            \\mov %rax, syscall_interrupt_work_frame+112(%rip)
            \\7:
        ++ asmStageUserReturnFromWorkFrame("syscall_interrupt_work_frame", trap_frame_iret_offset) ++
            \\jmp userReturnToSavedFrame
        );
    } else {
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
            \\sub $32, %rsp
            \\call saveCurrentThreadFxState
            \\add $32, %rsp
        ++ asmCopyStackFrameToWorkFrame("syscall_interrupt_work_frame", trap_frame_qword_count) ++
            \\sub $32, %rsp
            \\lea syscall_interrupt_work_frame(%rip), %rcx
            \\call syscallDispatch
            \\add $32, %rsp
            \\cmpq $0, syscall_return_writeback_enabled(%rip)
            \\je 7f
            \\mov %rax, syscall_interrupt_work_frame+112(%rip)
            \\7:
            \\sub $32, %rsp
            \\call restoreCurrentThreadFxState
            \\add $32, %rsp
        ++ asmStageUserReturnFromWorkFrame("syscall_interrupt_work_frame", trap_frame_iret_offset) ++
            \\jmp userReturnToSavedFrame
        );
    }
}

pub export fn timerInterruptHandlerStub() callconv(.naked) noreturn {
    if (debug_skip_timer_fx_state) {
        asm volatile (
            \\push %r10
            \\mov kernel_cr3_value(%rip), %r10
            \\mov %r10, %cr3
            \\pop %r10
            \\mov %rax, timer_entry_saved_rax(%rip)
            \\push %rax
            \\mov %rax, timer_entry_pushed_rax(%rip)
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
            \\mov 128(%rsp), %rax
            \\and $0x3, %rax
            \\cmp $0x3, %rax
            \\jne 9f
        ++ asmCopyStackFrameToWorkFrame("timer_interrupt_work_frame", trap_frame_qword_count) ++
            \\sub $32, %rsp
            \\lea timer_interrupt_work_frame(%rip), %rcx
            \\call timerInterruptDispatch
            \\add $32, %rsp
            \\mov timer_interrupt_work_frame+112(%rip), %r10
            \\mov %r10, timer_stack_rax_post_dispatch(%rip)
            \\mov %r10, timer_stack_rax_pre_log(%rip)
            \\sub $32, %rsp
            \\lea timer_interrupt_work_frame(%rip), %rcx
            \\call logTimerReturnFrame
            \\add $32, %rsp
        ++ asmStageUserReturnFromWorkFrame("timer_interrupt_work_frame", trap_frame_iret_offset) ++
            \\sub $32, %rsp
            \\call logCurrentStagedUserReturnFrame
            \\add $32, %rsp
            \\jmp userReturnToSavedFrame
            \\9:
            \\sub $512, %rsp
            \\lea 512(%rsp), %rdi
            \\mov %rdi, %rcx
            \\call timerInterruptDispatch
            \\add $512, %rsp
            \\mov 112(%rsp), %r10
            \\mov %r10, timer_stack_rax_post_dispatch(%rip)
            \\mov %r10, timer_stack_rax_pre_log(%rip)
            \\sub $512, %rsp
            \\lea 512(%rsp), %rcx
            \\call logTimerReturnFrame
            \\add $512, %rsp
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
            \\mov %r10, user_return_saved_r10(%rip)
            \\mov kernel_cr3_value(%rip), %r10
            \\mov %r10, %cr3
            \\mov user_return_saved_r10(%rip), %r10
            \\iretq
        );
    } else {
        asm volatile (
            \\push %r10
            \\mov kernel_cr3_value(%rip), %r10
            \\mov %r10, %cr3
            \\pop %r10
            \\mov %rax, timer_entry_saved_rax(%rip)
            \\push %rax
            \\mov %rax, timer_entry_pushed_rax(%rip)
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
            \\mov 128(%rsp), %rax
            \\and $0x3, %rax
            \\cmp $0x3, %rax
            \\jne 9f
            \\sub $32, %rsp
            \\call saveCurrentThreadFxState
            \\add $32, %rsp
        ++ asmCopyStackFrameToWorkFrame("timer_interrupt_work_frame", trap_frame_qword_count) ++
            \\sub $32, %rsp
            \\lea timer_interrupt_work_frame(%rip), %rcx
            \\call timerInterruptDispatch
            \\add $32, %rsp
            \\mov timer_interrupt_work_frame+112(%rip), %r10
            \\mov %r10, timer_stack_rax_post_dispatch(%rip)
            \\sub $32, %rsp
            \\call restoreCurrentThreadFxState
            \\add $32, %rsp
            \\mov timer_interrupt_work_frame+112(%rip), %r10
            \\mov %r10, timer_stack_rax_pre_log(%rip)
            \\sub $32, %rsp
            \\lea timer_interrupt_work_frame(%rip), %rcx
            \\call logTimerReturnFrame
            \\add $32, %rsp
        ++ asmStageUserReturnFromWorkFrame("timer_interrupt_work_frame", trap_frame_iret_offset) ++
            \\sub $32, %rsp
            \\call logCurrentStagedUserReturnFrame
            \\add $32, %rsp
            \\jmp userReturnToSavedFrame
            \\9:
            \\sub $512, %rsp
            \\lea 512(%rsp), %rdi
            \\mov %rdi, %rcx
            \\call timerInterruptDispatch
            \\add $512, %rsp
            \\mov 112(%rsp), %r10
            \\mov %r10, timer_stack_rax_post_dispatch(%rip)
            \\mov %r10, timer_stack_rax_pre_log(%rip)
            \\sub $512, %rsp
            \\lea 512(%rsp), %rcx
            \\call logTimerReturnFrame
            \\add $512, %rsp
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
            \\mov %r10, user_return_saved_r10(%rip)
            \\mov kernel_cr3_value(%rip), %r10
            \\mov %r10, %cr3
            \\mov user_return_saved_r10(%rip), %r10
            \\iretq
        );
    }
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
