const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const interrupts = @import("interrupts.zig");
const lapic = @import("lapic.zig");
const scheduler = @import("scheduler.zig");
const x86_platform = @import("arch/x86_64/platform.zig");

const TrapFrame = interrupts.TrapFrame;
const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;

const debug_skip_syscall_fx_state = true;
const debug_skip_timer_fx_state = true;
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
    priority_hold_quanta: u64,
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
    resume_after_fatal_user_exception: *const fn (kernel.PrincipalId, u8, *TrapFrame) void,
    switch_to_thread: *const fn (usize, *TrapFrame, ?u64) bool,
    log_race_switch: *const fn (usize, usize, []const u8) void,
};

var hooks: ?Hooks = null;
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
pub export var exception_fault_work_frame: ExceptionTrapFrame = std.mem.zeroes(ExceptionTrapFrame);
pub export var trap_fault_work_frame: TrapFrame = std.mem.zeroes(TrapFrame);
pub export var fatal_exception_resume_work_frame: TrapFrame = std.mem.zeroes(TrapFrame);
pub export var timer_interrupt_work_frame: TrapFrame = std.mem.zeroes(TrapFrame);
pub export var syscall_interrupt_work_frame: TrapFrame = std.mem.zeroes(TrapFrame);
pub export var syscall_entry_user_rsp: u64 = 0;
pub export var syscall_entry_is_lstar: u64 = 0;

pub fn init(new_hooks: Hooks) void {
    hooks = new_hooks;
}

fn getHooks() *const Hooks {
    return &(hooks orelse unreachable);
}

extern var kernel_cr3_value: u64;
extern var kernel_syscall_stack_top: u64;
extern var user_cr3_value: u64;
extern var current_user_principal: kernel.PrincipalId;
extern var current_thread_index: usize;
extern var thread_contexts_ptr: *anyopaque;
extern var ipc_hot_threads_ptr: *anyopaque;
extern var endpoint_generation_fast_mirror: u64;
extern var lapic_tick_count: u64;
extern var user_return_saved_r10: u64;
extern var user_return_saved_gprs: [15]u64 align(16);
extern var user_return_iret_frame: [5]u64 align(16);
extern var syscall_return_writeback_enabled: u64;

extern fn saveCurrentThreadFxState() callconv(.c) void;
extern fn restoreCurrentThreadFxState() callconv(.c) void;
extern fn syscallDispatch(frame: *TrapFrame) callconv(.c) u64;
extern fn syscallIpcDispatch(frame: *TrapFrame) callconv(.c) u64;
extern fn syscallIpcCallReplyRecvSignalOnlyDispatch(frame: *TrapFrame) callconv(.c) u64;
extern fn syscallIpcCallReplyRecvSignalOnlySparse(endpoint_id: u64, save: *const anyopaque, out_frame: *TrapFrame) callconv(.c) usize;
extern fn syscallIpcFastDispatch(nr: u64, arg0: u64, arg1: u64, arg2: u64) callconv(.c) u64;

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

fn asmCallAligned(comptime target: []const u8) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\call {s}
        \\mov %r15, %rsp
        \\
    , .{target});
}

fn asmCallSyscallDispatchFromStackFrame() []const u8 {
    return
        \\
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\mov %r15, %rcx
        \\mov 112(%r15), %rax
        \\cmp $0x6, %rax
        \\je 30f
        \\cmp $0x17, %rax
        \\je 30f
        \\cmp $0x2a, %rax
        \\je 30f
        \\cmp $0x2b, %rax
        \\je 30f
        \\cmp $0x2c, %rax
        \\je 30f
        \\cmp $0x40, %rax
        \\je 30f
        \\call syscallDispatch
        \\jmp 31f
        \\30:
        \\call syscallIpcDispatch
        \\31:
        \\mov %r15, %rsp
        \\
    ;
}

fn asmCallIpcFastDispatchNoCr3() []const u8 {
    return
        \\
        \\push %rax
        \\push %rdi
        \\push %rsi
        \\push %rdx
        \\push %r8
        \\push %r9
        \\push %r10
        \\push %r12
        \\push %r13
        \\push %r15
        \\mov %rcx, %r12
        \\mov %r11, %r13
        \\mov %rax, %rcx
        \\mov %rdi, %rdx
        \\mov %rsi, %r8
        \\mov 48(%rsp), %r9
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\call syscallIpcFastDispatch
        \\mov %r15, %rsp
        \\btr $63, %rax
        \\jnc 25f
        \\mov %r12, %rcx
        \\mov %r13, %r11
        \\pop %r15
        \\pop %r13
        \\pop %r12
        \\pop %r10
        \\pop %r9
        \\pop %r8
        \\pop %rdx
        \\pop %rsi
        \\pop %rdi
        \\add $8, %rsp
        \\jmp 23b
        \\25:
        \\mov %r12, %rcx
        \\mov %r13, %r11
        \\pop %r15
        \\pop %r13
        \\pop %r12
        \\pop %r10
        \\pop %r9
        \\pop %r8
        \\pop %rdx
        \\pop %rsi
        \\pop %rdi
        \\pop %rax
        \\jmp 28f
        \\
    ;
}

fn asmIpcFrameDispatchNoCr3(comptime user_cs: u64, comptime user_ss: u64) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\28:
        \\pushq ${d}
        \\pushq syscall_entry_user_rsp(%rip)
        \\push %r11
        \\pushq ${d}
        \\push %rcx
        \\push %rax
        \\push %rbx
        \\push %r10
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
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\mov %r15, %rcx
        \\call syscallIpcDispatch
        \\mov %r15, %rsp
        \\cmpq $0, syscall_return_writeback_enabled(%rip)
        \\je 7f
        \\mov %rax, 112(%rsp)
        \\7:
    , .{ user_ss, user_cs });
}

fn asmIpcCallReplyRecvSignalOnlyNoCr3(comptime user_cs: u64, comptime user_ss: u64) []const u8 {
    _ = user_cs;
    _ = user_ss;
    comptime {
        if (@sizeOf(kernel.PrincipalId) != 1) @compileError("asm IPC fastpath expects PrincipalId to fit in one byte");
    }
    return std.fmt.comptimePrint(
        \\
        \\27:
        \\test $2, %rdx
        \\jz 28f
        \\sub $32, %rsp
        \\mov %rdi, 0(%rsp)
        \\mov %r8, 8(%rsp)
        \\mov %r9, 16(%rsp)
        \\mov %r10, 24(%rsp)
        \\cmp $2, %rdx
        \\jne 270f
        \\mov current_thread_index(%rip), %r8
        \\mov ipc_hot_threads_ptr(%rip), %r9
        \\mov %r8, %r10
        \\imul ${d}, %r10, %r10
        \\lea (%r9,%r10), %r10
        \\cmpb $0, {d}(%r10)
        \\je 270f
        \\cmpb $0, {d}(%r10)
        \\jne 270f
        \\mov endpoint_generation_fast_mirror(%rip), %rdi
        \\cmp {d}(%r10), %rdi
        \\jne 270f
        \\cmp {d}(%r10), %rsi
        \\jne 270f
        \\mov {d}(%r10), %rdi
        \\cmp %r8, %rdi
        \\je 270f
        \\mov %rdi, %rax
        \\imul ${d}, %rax, %rax
        \\lea (%r9,%rax), %rax
        \\movzbq {d}(%r10), %r9
        \\cmpb %r9b, {d}(%rax)
        \\jne 270f
        \\cmpb $0, {d}(%rax)
        \\je 270f
        \\cmpb $0, {d}(%r10)
        \\je 268f
        \\cmp {d}(%r10), %rdi
        \\jne 270f
        \\movb $0, {d}(%r10)
        \\movq $0, {d}(%r10)
        \\268:
        \\cmpb $0, {d}(%rax)
        \\jne 270f
        \\movb $1, {d}(%rax)
        \\mov %r8, {d}(%rax)
        \\movb $0, {d}(%rax)
        \\movq $0, {d}(%rax)
        \\movb $1, {d}(%rax)
        \\movb $0, {d}(%rax)
        \\mov user_cr3_value(%rip), %r9
        \\mov %r9, {d}(%r10)
        \\movb $0, {d}(%r10)
        \\movq $0, {d}(%r10)
        \\movb $0, {d}(%r10)
        \\mov %rdi, current_thread_index(%rip)
        \\movb {d}(%rax), %r9b
        \\movb %r9b, current_user_principal(%rip)
        \\mov {d}(%rax), %r9
        \\mov %r9, user_cr3_value(%rip)
        \\mov thread_contexts_ptr(%rip), %r9
        \\mov %r8, %r10
        \\imul ${d}, %r10, %r10
        \\lea (%r9,%r10), %r10
        \\mov %rdi, %rax
        \\imul ${d}, %rax, %rax
        \\lea (%r9,%rax), %rax
    , .{
        @sizeOf(scheduler.IpcHotThread),
        @offsetOf(scheduler.IpcHotThread, "allocated"),
        @offsetOf(scheduler.IpcHotThread, "signal_pending"),
        @offsetOf(scheduler.IpcHotThread, "ipc_cached_endpoint_generation"),
        @offsetOf(scheduler.IpcHotThread, "ipc_cached_endpoint_id"),
        @offsetOf(scheduler.IpcHotThread, "ipc_cached_target_thread"),
        @sizeOf(scheduler.IpcHotThread),
        @offsetOf(scheduler.IpcHotThread, "ipc_cached_target"),
        @offsetOf(scheduler.IpcHotThread, "owner_process"),
        @offsetOf(scheduler.IpcHotThread, "allocated"),
        @offsetOf(scheduler.IpcHotThread, "ipc_reply_token_valid"),
        @offsetOf(scheduler.IpcHotThread, "ipc_reply_token_target_thread"),
        @offsetOf(scheduler.IpcHotThread, "ipc_reply_token_valid"),
        @offsetOf(scheduler.IpcHotThread, "ipc_reply_token_target_thread"),
        @offsetOf(scheduler.IpcHotThread, "ready"),
        @offsetOf(scheduler.IpcHotThread, "ipc_reply_token_valid"),
        @offsetOf(scheduler.IpcHotThread, "ipc_reply_token_target_thread"),
        @offsetOf(scheduler.IpcHotThread, "wait_mailbox"),
        @offsetOf(scheduler.IpcHotThread, "wake_tick"),
        @offsetOf(scheduler.IpcHotThread, "ready"),
        @offsetOf(scheduler.IpcHotThread, "signal_pending"),
        @offsetOf(scheduler.IpcHotThread, "cr3"),
        @offsetOf(scheduler.IpcHotThread, "wait_mailbox"),
        @offsetOf(scheduler.IpcHotThread, "wake_tick"),
        @offsetOf(scheduler.IpcHotThread, "ready"),
        @offsetOf(scheduler.IpcHotThread, "owner_process"),
        @offsetOf(scheduler.IpcHotThread, "cr3"),
        @sizeOf(scheduler.ThreadContext),
        @sizeOf(scheduler.ThreadContext),
    }) ++ std.fmt.comptimePrint(
        \\
        \\mov %r15, {d}(%r10)
        \\mov %r14, {d}(%r10)
        \\mov %r13, {d}(%r10)
        \\mov %r12, {d}(%r10)
        \\mov %rbp, {d}(%r10)
        \\mov %rbx, {d}(%r10)
        \\mov %rcx, {d}(%r10)
        \\mov %rcx, {d}(%r10)
        \\mov %r11, {d}(%r10)
        \\mov syscall_entry_user_rsp(%rip), %r9
        \\mov %r9, {d}(%r10)
        \\movq $0, {d}(%rax)
        \\mov 0(%rsp), %r9
        \\mov %r9, {d}(%rax)
        \\mov 8(%rsp), %r9
        \\mov %r9, {d}(%rax)
        \\mov 16(%rsp), %r9
        \\mov %r9, {d}(%rax)
        \\mov 24(%rsp), %r9
        \\mov %r9, {d}(%rax)
        \\add $32, %rsp
        \\lea {d}(%rax), %rsp
        \\jmp 273f
        \\270:
        \\mov 0(%rsp), %rdi
        \\mov 8(%rsp), %r8
        \\mov 16(%rsp), %r9
        \\mov 24(%rsp), %r10
        \\add $32, %rsp
        \\sub $264, %rsp
        \\mov %r15, 160(%rsp)
        \\mov %r14, 168(%rsp)
        \\mov %r13, 176(%rsp)
        \\mov %r12, 184(%rsp)
        \\mov %rbp, 192(%rsp)
        \\mov %rbx, 200(%rsp)
        \\mov %rcx, 208(%rsp)
        \\mov %r11, 216(%rsp)
        \\mov syscall_entry_user_rsp(%rip), %r11
        \\mov %r11, 224(%rsp)
        \\mov %rdi, 232(%rsp)
        \\mov %r8, 240(%rsp)
        \\mov %r9, 248(%rsp)
        \\mov %r10, 256(%rsp)
        \\mov %rsp, %r15
        \\mov %rsi, %rcx
        \\lea 160(%r15), %rdx
        \\mov %r15, %r8
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\call syscallIpcCallReplyRecvSignalOnlySparse
        \\mov %rax, %rsp
        \\273:
    , .{
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "r15"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "r14"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "r13"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "r12"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rbp"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rbx"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rcx"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rip"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rflags"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rsp"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rax"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rdi"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rsi"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rdx"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "r8"),
        @offsetOf(scheduler.ThreadContext, "frame"),
    });
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

fn asmSysretReturnFromWorkFrame(comptime work_frame_symbol: []const u8) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\mov {s}+120(%rip), %rcx
        \\mov %rcx, %r10
        \\shl $16, %r10
        \\sar $16, %r10
        \\cmp %rcx, %r10
        \\jne 8f
        \\mov {s}+136(%rip), %r11
        \\mov user_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\mov {s}+0(%rip), %r15
        \\mov {s}+8(%rip), %r14
        \\mov {s}+16(%rip), %r13
        \\mov {s}+24(%rip), %r12
        \\mov {s}+56(%rip), %r8
        \\mov {s}+64(%rip), %rbp
        \\mov {s}+72(%rip), %rdi
        \\mov {s}+80(%rip), %rsi
        \\mov {s}+88(%rip), %rdx
        \\mov {s}+104(%rip), %rbx
        \\mov {s}+144(%rip), %rsp
        \\mov {s}+112(%rip), %rax
        \\mov {s}+48(%rip), %r9
        \\mov {s}+40(%rip), %r10
        \\sysretq
        \\8:
        \\
    , .{
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
        work_frame_symbol,
    });
}

fn asmSysretReturnFromStackFrame() []const u8 {
    return
        \\
        \\mov 120(%rsp), %rcx
        \\mov %rcx, %r10
        \\shl $16, %r10
        \\sar $16, %r10
        \\cmp %rcx, %r10
        \\jne 8f
        \\mov 136(%rsp), %r11
        \\mov user_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\mov 0(%rsp), %r15
        \\mov 8(%rsp), %r14
        \\mov 16(%rsp), %r13
        \\mov 24(%rsp), %r12
        \\mov 56(%rsp), %r8
        \\mov 64(%rsp), %rbp
        \\mov 72(%rsp), %rdi
        \\mov 80(%rsp), %rsi
        \\mov 88(%rsp), %rdx
        \\mov 104(%rsp), %rbx
        \\mov 112(%rsp), %rax
        \\mov 48(%rsp), %r9
        \\mov 40(%rsp), %r10
        \\mov 144(%rsp), %rsp
        \\sysretq
        \\8:
        \\
    ;
}

fn exceptionName(vec: u64) []const u8 {
    return switch (vec) {
        10 => "INVALID TSS",
        11 => "SEGMENT NOT PRESENT",
        12 => "STACK SEGMENT FAULT",
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

fn writeCommonTrapRegs(h: *const Hooks, frame: anytype) void {
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
}

fn writeExceptionWithErrorSummary(h: *const Hooks, vec: u64, frame: *const ExceptionTrapFrame) void {
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
        const user_mode = (frame.error_code & (1 << 2)) != 0;
        h.write("  CR2=");
        h.write_hex_raw(cr2);
        h.write("\n");
        h.dump_page_walk_for_va(if (user_mode) user_cr3_value else h.read_cr3(), cr2);
        h.log_page_fault_step2(cr2, frame);
    }
    h.write("  EC=");
    h.write_hex_raw(frame.error_code);
    h.write("\n");
    h.write("  RIP=");
    h.write_hex_raw(frame.rip);
    h.write("\n");
    writeCommonTrapRegs(h, frame);
}

fn writeTrapSummary(h: *const Hooks, label: []const u8, frame: *const TrapFrame) void {
    h.write(label);
    h.write("\n");
    h.write("  THREAD=");
    h.write(h.thread_label(scheduler.current_thread_index));
    h.write("\n");
    h.write("  PRINCIPAL=");
    h.write(h.principal_label(scheduler.current_user_principal));
    h.write("\n");
    h.write("  RIP=");
    h.write_hex_raw(frame.rip);
    h.write("\n");
    writeCommonTrapRegs(h, frame);
}

pub export fn logTimerReturnFrame(_: *const TrapFrame) callconv(.c) void {}

pub export fn logCurrentStagedUserReturnFrame() callconv(.c) void {}

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
    writeExceptionWithErrorSummary(h, vec, frame);
    h.halt_loop();
}

pub export fn fatalUserExceptionWithErrorDispatch(vec: u64, frame: *const ExceptionTrapFrame) callconv(.c) void {
    const h = getHooks();
    asm volatile ("cli");
    writeExceptionWithErrorSummary(h, vec, frame);
    h.write("  ACTION=terminate process\n");
    h.resume_after_fatal_user_exception(
        scheduler.current_user_principal,
        @intCast(vec),
        &fatal_exception_resume_work_frame,
    );
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

pub export fn invalidOpcodeHandlerCommon(frame: *const TrapFrame) callconv(.c) noreturn {
    const h = getHooks();
    asm volatile ("cli");
    writeTrapSummary(h, "INVALID OPCODE", frame);
    h.halt_loop();
}

pub export fn fatalUserTrapDispatch(vec: u64, frame: *const TrapFrame) callconv(.c) void {
    const h = getHooks();
    asm volatile ("cli");
    const label = switch (vec) {
        6 => "INVALID OPCODE",
        else => "TRAP",
    };
    writeTrapSummary(h, label, frame);
    h.write("  ACTION=terminate process\n");
    h.resume_after_fatal_user_exception(
        scheduler.current_user_principal,
        @intCast(vec),
        &fatal_exception_resume_work_frame,
    );
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

    const current_thread = scheduler.current_thread_index;
    const next_thread = scheduler.chooseNextThreadForTimerPreempt(
        h.scheduler_quantum_ticks,
        h.priority_hold_quanta,
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
    ++ asmCallAligned("saveCurrentThreadFxState")
    ++ asmCopyStackFrameToWorkFrame("page_fault_work_frame", exception_trap_frame_qword_count) ++
        \\lea page_fault_work_frame(%rip), %rcx
    ++ asmCallAligned("pageFaultDispatch") ++
        \\test %rax, %rax
        \\jz 8f
    ++ asmCallAligned("restoreCurrentThreadFxState")
    ++ asmStageUserReturnFromWorkFrame("page_fault_work_frame", exception_trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\8:
        \\mov $14, %rcx
        \\lea page_fault_work_frame(%rip), %rdx
    ++ asmCallAligned("fatalUserExceptionWithErrorDispatch")
    ++ asmCallAligned("restoreCurrentThreadFxState")
    ++ asmStageUserReturnFromWorkFrame("fatal_exception_resume_work_frame", trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
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
    ++ asmCallAligned("restoreCurrentThreadFxState") ++
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
            \\movq $0, syscall_entry_is_lstar(%rip)
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
            \\lea syscall_interrupt_work_frame(%rip), %rcx
        ++ asmCallAligned("syscallDispatch") ++
            \\cmpq $0, syscall_return_writeback_enabled(%rip)
            \\je 7f
            \\mov %rax, syscall_interrupt_work_frame+112(%rip)
            \\7:
        ++ asmStageUserReturnFromWorkFrame("syscall_interrupt_work_frame", trap_frame_iret_offset)
        ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
        );
    } else {
        asm volatile (
            \\push %r10
            \\mov kernel_cr3_value(%rip), %r10
            \\mov %r10, %cr3
            \\pop %r10
            \\movq $0, syscall_entry_is_lstar(%rip)
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
        ++ asmCallAligned("saveCurrentThreadFxState")
        ++ asmCopyStackFrameToWorkFrame("syscall_interrupt_work_frame", trap_frame_qword_count) ++
            \\lea syscall_interrupt_work_frame(%rip), %rcx
        ++ asmCallAligned("syscallDispatch") ++
            \\cmpq $0, syscall_return_writeback_enabled(%rip)
            \\je 7f
            \\mov %rax, syscall_interrupt_work_frame+112(%rip)
            \\7:
        ++ asmCallAligned("restoreCurrentThreadFxState")
        ++ asmStageUserReturnFromWorkFrame("syscall_interrupt_work_frame", trap_frame_iret_offset)
        ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
        );
    }
}

pub export fn syscallLstarHandlerStub() callconv(.naked) noreturn {
    const user_cs: u64 = @as(u64, x86_platform.gdt_user_code_selector) | 0x3;
    const user_ss: u64 = @as(u64, x86_platform.gdt_user_data_selector) | 0x3;
    if (debug_skip_syscall_fx_state) {
        asm volatile (
            \\mov %rsp, syscall_entry_user_rsp(%rip)
            \\mov kernel_syscall_stack_top(%rip), %rsp
            \\cmp $0x2d, %rax
            \\je 20f
            \\cmp $0x2e, %rax
            \\je 21f
            \\cmp $0xffff, %rax
            \\je 22f
            \\cmp $0x17, %rax
            \\je 28f
            \\cmp $0x6, %rax
            \\je 24f
            \\cmp $0x2a, %rax
            \\je 24f
            \\cmp $0x2b, %rax
            \\je 24f
            \\cmp $0x2c, %rax
            \\je 24f
            \\cmp $0x40, %rax
            \\je 27f
            \\jmp 29f
            \\20:
            \\mov lapic_tick_count(%rip), %rax
            \\jmp 23f
            \\21:
            \\movzbq current_user_principal(%rip), %rax
            \\cmp $32, %rax
            \\jb 23f
            \\mov $1, %rax
            \\jmp 23f
            \\22:
            \\mov $1, %rax
            \\23:
            \\mov syscall_entry_user_rsp(%rip), %rsp
            \\sysretq
            \\24:
        ++ asmCallIpcFastDispatchNoCr3() ++
        asmIpcCallReplyRecvSignalOnlyNoCr3(user_cs, user_ss)
        ++ asmSysretReturnFromStackFrame()
        ++ asmCopyStackFrameToWorkFrame("syscall_interrupt_work_frame", trap_frame_qword_count)
        ++ asmStageUserReturnFromWorkFrame("syscall_interrupt_work_frame", trap_frame_iret_offset)
        ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
        ++
        asmIpcFrameDispatchNoCr3(user_cs, user_ss)
        ++ asmSysretReturnFromStackFrame()
        ++ asmCopyStackFrameToWorkFrame("syscall_interrupt_work_frame", trap_frame_qword_count)
        ++ asmStageUserReturnFromWorkFrame("syscall_interrupt_work_frame", trap_frame_iret_offset)
        ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
            \\29:
            \\push %r15
            \\mov kernel_cr3_value(%rip), %r15
            \\mov %r15, %cr3
            \\pop %r15
            \\movq $1, syscall_entry_is_lstar(%rip)
            \\pushq %[user_ss]
            \\pushq syscall_entry_user_rsp(%rip)
            \\push %r11
            \\pushq %[user_cs]
            \\push %rcx
            \\push %rax
            \\push %rbx
            \\push %r10
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
        ++ asmCallSyscallDispatchFromStackFrame() ++
            \\cmpq $0, syscall_return_writeback_enabled(%rip)
            \\je 7f
            \\mov %rax, 112(%rsp)
            \\7:
        ++ asmSysretReturnFromStackFrame()
        ++ asmCopyStackFrameToWorkFrame("syscall_interrupt_work_frame", trap_frame_qword_count)
        ++ asmStageUserReturnFromWorkFrame("syscall_interrupt_work_frame", trap_frame_iret_offset)
        ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
            :
            : [user_cs] "i" (user_cs),
              [user_ss] "i" (user_ss),
        );
    } else {
        asm volatile (
            \\mov %rsp, syscall_entry_user_rsp(%rip)
            \\mov kernel_syscall_stack_top(%rip), %rsp
            \\cmp $0x2d, %rax
            \\je 20f
            \\cmp $0x2e, %rax
            \\je 21f
            \\cmp $0xffff, %rax
            \\je 22f
            \\cmp $0x17, %rax
            \\je 28f
            \\cmp $0x6, %rax
            \\je 24f
            \\cmp $0x2a, %rax
            \\je 24f
            \\cmp $0x2b, %rax
            \\je 24f
            \\cmp $0x2c, %rax
            \\je 24f
            \\cmp $0x40, %rax
            \\je 27f
            \\jmp 29f
            \\20:
            \\mov lapic_tick_count(%rip), %rax
            \\jmp 23f
            \\21:
            \\movzbq current_user_principal(%rip), %rax
            \\cmp $32, %rax
            \\jb 23f
            \\mov $1, %rax
            \\jmp 23f
            \\22:
            \\mov $1, %rax
            \\23:
            \\mov syscall_entry_user_rsp(%rip), %rsp
            \\sysretq
            \\24:
        ++ asmCallIpcFastDispatchNoCr3() ++
        asmIpcCallReplyRecvSignalOnlyNoCr3(user_cs, user_ss)
        ++ asmSysretReturnFromStackFrame()
        ++ asmCopyStackFrameToWorkFrame("syscall_interrupt_work_frame", trap_frame_qword_count)
        ++ asmStageUserReturnFromWorkFrame("syscall_interrupt_work_frame", trap_frame_iret_offset)
        ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
        ++
        asmIpcFrameDispatchNoCr3(user_cs, user_ss)
        ++ asmSysretReturnFromStackFrame()
        ++ asmCopyStackFrameToWorkFrame("syscall_interrupt_work_frame", trap_frame_qword_count)
        ++ asmStageUserReturnFromWorkFrame("syscall_interrupt_work_frame", trap_frame_iret_offset)
        ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
            \\29:
            \\push %r15
            \\mov kernel_cr3_value(%rip), %r15
            \\mov %r15, %cr3
            \\pop %r15
            \\movq $1, syscall_entry_is_lstar(%rip)
            \\pushq %[user_ss]
            \\pushq syscall_entry_user_rsp(%rip)
            \\push %r11
            \\pushq %[user_cs]
            \\push %rcx
            \\push %rax
            \\push %rbx
            \\push %r10
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
        ++ asmCallAligned("saveCurrentThreadFxState") ++
            \\mov %rsp, %rcx
        ++ asmCallSyscallDispatchFromStackFrame() ++
            \\cmpq $0, syscall_return_writeback_enabled(%rip)
            \\je 7f
            \\mov %rax, 112(%rsp)
            \\7:
        ++ asmCallAligned("restoreCurrentThreadFxState")
        ++ asmSysretReturnFromStackFrame()
        ++ asmCopyStackFrameToWorkFrame("syscall_interrupt_work_frame", trap_frame_qword_count)
        ++ asmStageUserReturnFromWorkFrame("syscall_interrupt_work_frame", trap_frame_iret_offset)
        ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
            :
            : [user_cs] "i" (user_cs),
              [user_ss] "i" (user_ss),
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
            \\lea timer_interrupt_work_frame(%rip), %rcx
        ++ asmCallAligned("timerInterruptDispatch") ++
            \\mov timer_interrupt_work_frame+112(%rip), %r10
            \\mov %r10, timer_stack_rax_post_dispatch(%rip)
            \\mov %r10, timer_stack_rax_pre_log(%rip)
            \\lea timer_interrupt_work_frame(%rip), %rcx
        ++ asmCallAligned("logTimerReturnFrame")
        ++ asmStageUserReturnFromWorkFrame("timer_interrupt_work_frame", trap_frame_iret_offset)
        ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
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
        ++ asmCallAligned("saveCurrentThreadFxState")
        ++ asmCopyStackFrameToWorkFrame("timer_interrupt_work_frame", trap_frame_qword_count) ++
            \\lea timer_interrupt_work_frame(%rip), %rcx
        ++ asmCallAligned("timerInterruptDispatch") ++
            \\mov timer_interrupt_work_frame+112(%rip), %r10
            \\mov %r10, timer_stack_rax_post_dispatch(%rip)
        ++ asmCallAligned("restoreCurrentThreadFxState") ++
            \\mov timer_interrupt_work_frame+112(%rip), %r10
            \\mov %r10, timer_stack_rax_pre_log(%rip)
            \\lea timer_interrupt_work_frame(%rip), %rcx
        ++ asmCallAligned("logTimerReturnFrame")
        ++ asmStageUserReturnFromWorkFrame("timer_interrupt_work_frame", trap_frame_iret_offset)
        ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
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
    ++ asmCopyStackFrameToWorkFrame("exception_fault_work_frame", exception_trap_frame_qword_count) ++
        \\mov exception_fault_work_frame+136(%rip), %rax
        \\and $0x3, %rax
        \\cmp $0x3, %rax
        \\jne 1f
        \\mov $13, %rcx
        \\lea exception_fault_work_frame(%rip), %rdx
    ++ asmCallAligned("fatalUserExceptionWithErrorDispatch")
    ++ asmCallAligned("restoreCurrentThreadFxState")
    ++ asmStageUserReturnFromWorkFrame("fatal_exception_resume_work_frame", trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\1:
        \\mov $13, %rcx
        \\lea exception_fault_work_frame(%rip), %rdx
    ++ asmCallAligned("exceptionWithErrorCommon") ++
        \\ud2
    );
}

pub export fn invalidTssHandlerStub() callconv(.naked) noreturn {
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
    ++ asmCopyStackFrameToWorkFrame("exception_fault_work_frame", exception_trap_frame_qword_count) ++
        \\mov exception_fault_work_frame+136(%rip), %rax
        \\and $0x3, %rax
        \\cmp $0x3, %rax
        \\jne 1f
        \\mov $10, %rcx
        \\lea exception_fault_work_frame(%rip), %rdx
    ++ asmCallAligned("fatalUserExceptionWithErrorDispatch")
    ++ asmCallAligned("restoreCurrentThreadFxState")
    ++ asmStageUserReturnFromWorkFrame("fatal_exception_resume_work_frame", trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\1:
        \\mov $10, %rcx
        \\lea exception_fault_work_frame(%rip), %rdx
    ++ asmCallAligned("exceptionWithErrorCommon") ++
        \\ud2
    );
}

pub export fn segmentNotPresentHandlerStub() callconv(.naked) noreturn {
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
    ++ asmCopyStackFrameToWorkFrame("exception_fault_work_frame", exception_trap_frame_qword_count) ++
        \\mov exception_fault_work_frame+136(%rip), %rax
        \\and $0x3, %rax
        \\cmp $0x3, %rax
        \\jne 1f
        \\mov $11, %rcx
        \\lea exception_fault_work_frame(%rip), %rdx
    ++ asmCallAligned("fatalUserExceptionWithErrorDispatch")
    ++ asmCallAligned("restoreCurrentThreadFxState")
    ++ asmStageUserReturnFromWorkFrame("fatal_exception_resume_work_frame", trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\1:
        \\mov $11, %rcx
        \\lea exception_fault_work_frame(%rip), %rdx
    ++ asmCallAligned("exceptionWithErrorCommon") ++
        \\ud2
    );
}

pub export fn stackSegmentFaultHandlerStub() callconv(.naked) noreturn {
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
    ++ asmCopyStackFrameToWorkFrame("exception_fault_work_frame", exception_trap_frame_qword_count) ++
        \\mov exception_fault_work_frame+136(%rip), %rax
        \\and $0x3, %rax
        \\cmp $0x3, %rax
        \\jne 1f
        \\mov $12, %rcx
        \\lea exception_fault_work_frame(%rip), %rdx
    ++ asmCallAligned("fatalUserExceptionWithErrorDispatch")
    ++ asmCallAligned("restoreCurrentThreadFxState")
    ++ asmStageUserReturnFromWorkFrame("fatal_exception_resume_work_frame", trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\1:
        \\mov $12, %rcx
        \\lea exception_fault_work_frame(%rip), %rdx
    ++ asmCallAligned("exceptionWithErrorCommon") ++
        \\ud2
    );
}

pub export fn invalidOpcodeHandlerStub() callconv(.naked) noreturn {
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
    ++ asmCopyStackFrameToWorkFrame("trap_fault_work_frame", trap_frame_qword_count) ++
        \\mov trap_fault_work_frame+128(%rip), %rax
        \\and $0x3, %rax
        \\cmp $0x3, %rax
        \\jne 1f
        \\mov $6, %rcx
        \\lea trap_fault_work_frame(%rip), %rdx
    ++ asmCallAligned("fatalUserTrapDispatch")
    ++ asmCallAligned("restoreCurrentThreadFxState")
    ++ asmStageUserReturnFromWorkFrame("fatal_exception_resume_work_frame", trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\1:
        \\lea trap_fault_work_frame(%rip), %rcx
    ++ asmCallAligned("invalidOpcodeHandlerCommon") ++
        \\ud2
    );
}
