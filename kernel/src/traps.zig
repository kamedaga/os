const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const interrupts = @import("interrupts.zig");
const lapic = @import("lapic.zig");
const scheduler = @import("scheduler.zig");
const smp = @import("smp.zig");
const x86_platform = @import("arch/x86_64/platform.zig");

const TrapFrame = interrupts.TrapFrame;
const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;

const debug_skip_syscall_fx_state = true;
const debug_skip_timer_fx_state = false;
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

var trap_hooks_storage: Hooks = undefined;
var trap_hooks_ready = false;
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
pub export var page_fault_work_frames: [smp.max_cpus]ExceptionTrapFrame = [_]ExceptionTrapFrame{std.mem.zeroes(ExceptionTrapFrame)} ** smp.max_cpus;
pub export var trap_fault_work_frame: TrapFrame = std.mem.zeroes(TrapFrame);
pub export var trap_fault_work_frames: [smp.max_cpus]TrapFrame = [_]TrapFrame{std.mem.zeroes(TrapFrame)} ** smp.max_cpus;
pub export var fatal_exception_resume_work_frame: TrapFrame = std.mem.zeroes(TrapFrame);
pub export var fatal_exception_resume_work_frames: [smp.max_cpus]TrapFrame = [_]TrapFrame{std.mem.zeroes(TrapFrame)} ** smp.max_cpus;
pub export var timer_interrupt_work_frame: TrapFrame = std.mem.zeroes(TrapFrame);
pub export var timer_interrupt_work_frames: [smp.max_cpus]TrapFrame = [_]TrapFrame{std.mem.zeroes(TrapFrame)} ** smp.max_cpus;
pub export var timer_interrupt_work_frame_cpu_slot: usize = 0;
pub export var syscall_interrupt_work_frames: [smp.max_cpus]TrapFrame = [_]TrapFrame{std.mem.zeroes(TrapFrame)} ** smp.max_cpus;
pub export var user_return_saved_gprs_by_cpu: [smp.max_cpus][16]u64 align(16) = [_][16]u64{[_]u64{0} ** 16} ** smp.max_cpus;
pub export var user_return_iret_frames_by_cpu: [smp.max_cpus][8]u64 align(16) = [_][8]u64{[_]u64{0} ** 8} ** smp.max_cpus;
pub export var syscall_interrupt_work_frame_cpu_slot: usize = 0;
pub export var syscall_lstar_cpu_slot: usize = 0;
pub export var syscall_entry_user_rsp: u64 = 0;
pub export var syscall_entry_user_rsps: [smp.max_cpus]u64 = [_]u64{0} ** smp.max_cpus;
pub export var syscall_entry_saved_r15s: [smp.max_cpus]u64 = [_]u64{0} ** smp.max_cpus;
pub export var syscall_entry_is_lstar: u64 = 0;
pub export var syscall_entry_is_lstars: [smp.max_cpus]u64 = [_]u64{0} ** smp.max_cpus;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(trap_hooks_storage), &trap_hooks_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(trap_hooks_ready), &trap_hooks_ready));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(timer_entry_saved_rax), &timer_entry_saved_rax));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(timer_entry_pushed_rax), &timer_entry_pushed_rax));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(timer_stack_rax_post_dispatch), &timer_stack_rax_post_dispatch));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(timer_stack_rax_pre_log), &timer_stack_rax_pre_log));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(last_user_return_rax), &last_user_return_rax));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(last_user_return_rip), &last_user_return_rip));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(last_stage_source_rax), &last_stage_source_rax));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(last_stage_saved_rax), &last_stage_saved_rax));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(last_stage_source_rip), &last_stage_source_rip));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(last_stage_saved_rip), &last_stage_saved_rip));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(page_fault_work_frame), &page_fault_work_frame));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(page_fault_work_frames), &page_fault_work_frames));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(trap_fault_work_frame), &trap_fault_work_frame));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(trap_fault_work_frames), &trap_fault_work_frames));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(fatal_exception_resume_work_frame), &fatal_exception_resume_work_frame));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(fatal_exception_resume_work_frames), &fatal_exception_resume_work_frames));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(timer_interrupt_work_frame), &timer_interrupt_work_frame));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(timer_interrupt_work_frames), &timer_interrupt_work_frames));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(timer_interrupt_work_frame_cpu_slot), &timer_interrupt_work_frame_cpu_slot));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_interrupt_work_frames), &syscall_interrupt_work_frames));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_return_saved_gprs_by_cpu), &user_return_saved_gprs_by_cpu));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_return_iret_frames_by_cpu), &user_return_iret_frames_by_cpu));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_interrupt_work_frame_cpu_slot), &syscall_interrupt_work_frame_cpu_slot));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_lstar_cpu_slot), &syscall_lstar_cpu_slot));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_entry_user_rsp), &syscall_entry_user_rsp));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_entry_user_rsps), &syscall_entry_user_rsps));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_entry_saved_r15s), &syscall_entry_saved_r15s));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_entry_is_lstar), &syscall_entry_is_lstar));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_entry_is_lstars), &syscall_entry_is_lstars));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_return_saved_r10), &user_return_saved_r10));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_return_saved_gprs), &user_return_saved_gprs));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_return_iret_frame), &user_return_iret_frame));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_return_writeback_enabled_by_cpu), &syscall_return_writeback_enabled_by_cpu));
    return end;
}

pub fn init(new_hooks: Hooks) void {
    trap_hooks_storage = new_hooks;
    trap_hooks_ready = true;
}

pub fn mapKernelRuntimeStorage(map_identity_range: *const fn (u64, usize) bool) bool {
    if (!map_identity_range(@intFromPtr(&trap_hooks_storage), @sizeOf(@TypeOf(trap_hooks_storage)))) return false;
    if (!map_identity_range(@intFromPtr(&trap_hooks_ready), @sizeOf(@TypeOf(trap_hooks_ready)))) return false;
    if (!map_identity_range(@intFromPtr(&timer_entry_saved_rax), @sizeOf(@TypeOf(timer_entry_saved_rax)))) return false;
    if (!map_identity_range(@intFromPtr(&timer_entry_pushed_rax), @sizeOf(@TypeOf(timer_entry_pushed_rax)))) return false;
    if (!map_identity_range(@intFromPtr(&timer_stack_rax_post_dispatch), @sizeOf(@TypeOf(timer_stack_rax_post_dispatch)))) return false;
    if (!map_identity_range(@intFromPtr(&timer_stack_rax_pre_log), @sizeOf(@TypeOf(timer_stack_rax_pre_log)))) return false;
    if (!map_identity_range(@intFromPtr(&user_return_saved_r10), @sizeOf(@TypeOf(user_return_saved_r10)))) return false;
    if (!map_identity_range(@intFromPtr(&user_return_saved_gprs), @sizeOf(@TypeOf(user_return_saved_gprs)))) return false;
    if (!map_identity_range(@intFromPtr(&user_return_iret_frame), @sizeOf(@TypeOf(user_return_iret_frame)))) return false;
    if (!map_identity_range(@intFromPtr(&last_user_return_rax), @sizeOf(@TypeOf(last_user_return_rax)))) return false;
    if (!map_identity_range(@intFromPtr(&last_user_return_rip), @sizeOf(@TypeOf(last_user_return_rip)))) return false;
    if (!map_identity_range(@intFromPtr(&last_stage_source_rax), @sizeOf(@TypeOf(last_stage_source_rax)))) return false;
    if (!map_identity_range(@intFromPtr(&last_stage_saved_rax), @sizeOf(@TypeOf(last_stage_saved_rax)))) return false;
    if (!map_identity_range(@intFromPtr(&last_stage_source_rip), @sizeOf(@TypeOf(last_stage_source_rip)))) return false;
    if (!map_identity_range(@intFromPtr(&last_stage_saved_rip), @sizeOf(@TypeOf(last_stage_saved_rip)))) return false;
    if (!map_identity_range(@intFromPtr(&page_fault_work_frame), @sizeOf(@TypeOf(page_fault_work_frame)))) return false;
    if (!map_identity_range(@intFromPtr(&page_fault_work_frames), @sizeOf(@TypeOf(page_fault_work_frames)))) return false;
    if (!map_identity_range(@intFromPtr(&trap_fault_work_frame), @sizeOf(@TypeOf(trap_fault_work_frame)))) return false;
    if (!map_identity_range(@intFromPtr(&trap_fault_work_frames), @sizeOf(@TypeOf(trap_fault_work_frames)))) return false;
    if (!map_identity_range(@intFromPtr(&fatal_exception_resume_work_frame), @sizeOf(@TypeOf(fatal_exception_resume_work_frame)))) return false;
    if (!map_identity_range(@intFromPtr(&fatal_exception_resume_work_frames), @sizeOf(@TypeOf(fatal_exception_resume_work_frames)))) return false;
    if (!map_identity_range(@intFromPtr(&timer_interrupt_work_frame), @sizeOf(@TypeOf(timer_interrupt_work_frame)))) return false;
    if (!map_identity_range(@intFromPtr(&timer_interrupt_work_frames), @sizeOf(@TypeOf(timer_interrupt_work_frames)))) return false;
    if (!map_identity_range(@intFromPtr(&timer_interrupt_work_frame_cpu_slot), @sizeOf(@TypeOf(timer_interrupt_work_frame_cpu_slot)))) return false;
    if (!map_identity_range(@intFromPtr(&syscall_interrupt_work_frames), @sizeOf(@TypeOf(syscall_interrupt_work_frames)))) return false;
    if (!map_identity_range(@intFromPtr(&user_return_saved_gprs_by_cpu), @sizeOf(@TypeOf(user_return_saved_gprs_by_cpu)))) return false;
    if (!map_identity_range(@intFromPtr(&user_return_iret_frames_by_cpu), @sizeOf(@TypeOf(user_return_iret_frames_by_cpu)))) return false;
    if (!map_identity_range(@intFromPtr(&syscall_interrupt_work_frame_cpu_slot), @sizeOf(@TypeOf(syscall_interrupt_work_frame_cpu_slot)))) return false;
    if (!map_identity_range(@intFromPtr(&syscall_lstar_cpu_slot), @sizeOf(@TypeOf(syscall_lstar_cpu_slot)))) return false;
    if (!map_identity_range(@intFromPtr(&syscall_entry_user_rsp), @sizeOf(@TypeOf(syscall_entry_user_rsp)))) return false;
    if (!map_identity_range(@intFromPtr(&syscall_entry_user_rsps), @sizeOf(@TypeOf(syscall_entry_user_rsps)))) return false;
    if (!map_identity_range(@intFromPtr(&syscall_entry_saved_r15s), @sizeOf(@TypeOf(syscall_entry_saved_r15s)))) return false;
    if (!map_identity_range(@intFromPtr(&syscall_entry_is_lstar), @sizeOf(@TypeOf(syscall_entry_is_lstar)))) return false;
    if (!map_identity_range(@intFromPtr(&syscall_entry_is_lstars), @sizeOf(@TypeOf(syscall_entry_is_lstars)))) return false;
    return true;
}

fn getHooks() *const Hooks {
    if (!trap_hooks_ready) unreachable;
    return &trap_hooks_storage;
}
extern var kernel_cr3_value: u64;
extern var kernel_syscall_stack_top: u64;
extern var kernel_syscall_stack_tops: [smp.max_cpus]u64;
extern var user_cr3_value: u64;
extern var user_cr3_values: [smp.max_cpus]u64;
extern var current_user_principal: kernel.PrincipalId;
extern var current_user_principals: [smp.max_cpus]kernel.PrincipalId;
extern var current_thread_index: usize;
extern var current_thread_indices: [smp.max_cpus]usize;
extern var thread_contexts_ptr: *anyopaque;
extern var ipc_hot_threads_ptr: *anyopaque;
extern var endpoint_generation_fast_mirror: u64;
extern var lapic_tick_count: u64;
extern var user_return_saved_r10: u64;
extern var user_return_saved_gprs: [15]u64 align(16);
extern var user_return_iret_frame: [5]u64 align(16);
extern var syscall_return_writeback_enabled_by_cpu: [smp.max_cpus]u64;

extern fn saveCurrentThreadFxState() callconv(.c) void;
extern fn restoreCurrentThreadFxState() callconv(.c) void;
extern fn syscallDispatch(frame: *TrapFrame) callconv(.c) u64;
extern fn syscallLstarDispatch(frame: *TrapFrame) callconv(.c) u64;
extern fn syscallIpcDispatch(frame: *TrapFrame) callconv(.c) u64;
extern fn syscallIpcCallReplyRecvSignalOnlyDispatch(frame: *TrapFrame) callconv(.c) u64;
extern fn syscallIpcCallReplyRecvSignalOnlySparse(endpoint_id: u64, save: *const anyopaque, out_frame: *TrapFrame) callconv(.c) usize;
extern fn syscallIpcFastDispatch(nr: u64, arg0: u64, arg1: u64, arg2: u64) callconv(.c) u64;
extern fn schedulerSyncBootstrapCurrentStateFromMirrorFromAsm() callconv(.c) void;

pub export fn syscallInterruptWorkFrameForCurrentCpuFromAsm() callconv(.c) *TrapFrame {
    const cpu_slot = scheduler.currentCpuSlot();
    const bounded_slot = if (cpu_slot < syscall_interrupt_work_frames.len) cpu_slot else 0;
    syscall_interrupt_work_frame_cpu_slot = bounded_slot;
    return &syscall_interrupt_work_frames[bounded_slot];
}

pub export fn timerInterruptWorkFrameForCurrentCpuFromAsm() callconv(.c) *TrapFrame {
    const cpu_slot = scheduler.currentCpuSlot();
    const bounded_slot = if (cpu_slot < timer_interrupt_work_frames.len) cpu_slot else 0;
    timer_interrupt_work_frame_cpu_slot = bounded_slot;
    return &timer_interrupt_work_frames[bounded_slot];
}

fn boundedCurrentCpuSlot() usize {
    const cpu_slot = scheduler.currentCpuSlot();
    return if (cpu_slot < smp.max_cpus) cpu_slot else 0;
}

pub export fn pageFaultWorkFrameForCurrentCpuFromAsm() callconv(.c) *ExceptionTrapFrame {
    return &page_fault_work_frames[boundedCurrentCpuSlot()];
}

pub export fn trapFaultWorkFrameForCurrentCpuFromAsm() callconv(.c) *TrapFrame {
    return &trap_fault_work_frames[boundedCurrentCpuSlot()];
}

pub export fn fatalExceptionResumeWorkFrameForCurrentCpuFromAsm() callconv(.c) *TrapFrame {
    return &fatal_exception_resume_work_frames[boundedCurrentCpuSlot()];
}

pub export fn markSyscallEntryInt80ForCurrentCpuFromAsm() callconv(.c) void {
    const cpu_slot = boundedCurrentCpuSlot();
    syscall_entry_is_lstar = 0;
    syscall_entry_is_lstars[cpu_slot] = 0;
}

pub export fn syscallReturnWritebackEnabledForCurrentCpuFromAsm() callconv(.c) u64 {
    return syscall_return_writeback_enabled_by_cpu[boundedCurrentCpuSlot()];
}

pub export fn stageUserReturnFromFramePointerForCurrentCpu(frame_addr: usize, iret_offset: usize) callconv(.c) void {
    const cpu_slot = boundedCurrentCpuSlot();
    const src: [*]const u64 = @ptrFromInt(frame_addr);
    const iret_src: [*]const u64 = @ptrFromInt(frame_addr + iret_offset);
    var gpr_index: usize = 0;
    while (gpr_index < user_return_gpr_qword_count) : (gpr_index += 1) {
        user_return_saved_gprs_by_cpu[cpu_slot][gpr_index] = src[gpr_index];
    }
    var iret_index: usize = 0;
    while (iret_index < user_return_iret_qword_count) : (iret_index += 1) {
        user_return_iret_frames_by_cpu[cpu_slot][iret_index] = iret_src[iret_index];
    }
    last_stage_source_rax = src[@offsetOf(TrapFrame, "rax") / @sizeOf(u64)];
    last_stage_saved_rax = user_return_saved_gprs_by_cpu[cpu_slot][@offsetOf(TrapFrame, "rax") / @sizeOf(u64)];
    last_stage_source_rip = iret_src[0];
    last_stage_saved_rip = user_return_iret_frames_by_cpu[cpu_slot][0];
}

pub export fn userReturnSavedGprsForCurrentCpuFromAsm() callconv(.c) *u64 {
    const cpu_slot = boundedCurrentCpuSlot();
    return &user_return_saved_gprs_by_cpu[cpu_slot][0];
}

pub export fn userReturnIretFrameForCurrentCpuFromAsm() callconv(.c) *u64 {
    const cpu_slot = boundedCurrentCpuSlot();
    return &user_return_iret_frames_by_cpu[cpu_slot][0];
}

pub export fn userReturnCr3ForCurrentCpuFromAsm() callconv(.c) u64 {
    return scheduler.currentUserCr3();
}

pub export fn applyCurrentThreadFsBaseForUserReturnFromAsm() callconv(.c) void {
    _ = scheduler.applyThreadFsBase(scheduler.currentThreadIndex());
}

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

fn asmCopyStackFrameToWorkFramePointer(comptime qword_count: usize) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\mov %rax, %rdi
        \\mov %rsp, %rsi
        \\mov ${d}, %ecx
        \\cld
        \\rep movsq
        \\
    , .{qword_count});
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

fn asmWriteSyscallReturnToWorkFramePointer() []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\push %rax
        \\push %r10
        \\
    , .{}) ++ asmCallAligned("syscallReturnWritebackEnabledForCurrentCpuFromAsm") ++ std.fmt.comptimePrint(
        \\
        \\mov %rax, %r11
        \\pop %r10
        \\pop %rax
        \\cmpq $0, %r11
        \\je 7f
        \\mov %r10, 112(%rax)
        \\7:
        \\
    , .{});
}

fn asmCallSyscallDispatchFromStackFrame(comptime target: []const u8) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\mov %r15, %rcx
        \\call {s}
        \\31:
        \\mov %r15, %rsp
        \\
    , .{target});
}

fn asmCallIpcFastDispatchNoCr3() []const u8 {
    return std.fmt.comptimePrint(
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
    , .{});
}

fn asmIpcFrameDispatchNoCr3(
    comptime user_cs: u64,
    comptime user_ss: u64,
    comptime user_rsp_symbol: []const u8,
    comptime writeback_symbol: []const u8,
) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\28:
        \\pushq ${d}
        \\pushq {s}(%rip)
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
        \\cmpq $0, {s}(%rip)
        \\je 7f
        \\mov %rax, 112(%rsp)
        \\7:
    , .{ user_ss, user_rsp_symbol, user_cs, writeback_symbol });
}

fn asmIpcCallReplyRecvSignalOnlyNoCr3(
    comptime user_cs: u64,
    comptime user_ss: u64,
    comptime user_rsp_symbol: []const u8,
    comptime current_thread_symbol: []const u8,
    comptime current_principal_symbol: []const u8,
    comptime user_cr3_symbol: []const u8,
) []const u8 {
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
        \\mov {s}(%rip), %r8
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
    , .{
        current_thread_symbol,
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
    }) ++ std.fmt.comptimePrint(
        \\
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
        \\mov {s}(%rip), %r9
        \\mov %r9, {d}(%r10)
        \\movb $0, {d}(%r10)
        \\movq $0, {d}(%r10)
        \\movb $0, {d}(%r10)
        \\mov %rdi, {s}(%rip)
        \\movb {d}(%rax), %r9b
        \\movb %r9b, {s}(%rip)
        \\mov {d}(%rax), %r9
        \\mov %r9, {s}(%rip)
        \\mov thread_contexts_ptr(%rip), %r9
        \\mov %r8, %r10
        \\imul ${d}, %r10, %r10
        \\lea (%r9,%r10), %r10
        \\mov %rdi, %rax
        \\imul ${d}, %rax, %rax
        \\lea (%r9,%rax), %rax
    , .{
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
        user_cr3_symbol,
        @offsetOf(scheduler.IpcHotThread, "cr3"),
        @offsetOf(scheduler.IpcHotThread, "wait_mailbox"),
        @offsetOf(scheduler.IpcHotThread, "wake_tick"),
        @offsetOf(scheduler.IpcHotThread, "ready"),
        current_thread_symbol,
        @offsetOf(scheduler.IpcHotThread, "owner_process"),
        current_principal_symbol,
        @offsetOf(scheduler.IpcHotThread, "cr3"),
        user_cr3_symbol,
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
        \\mov {s}(%rip), %r9
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
        \\push %rax
    ++ asmCallAligned("schedulerSyncBootstrapCurrentStateFromMirrorFromAsm") ++
        \\pop %rax
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
        \\mov {s}(%rip), %r11
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
        user_rsp_symbol,
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rsp"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rax"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rdi"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rsi"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "rdx"),
        @offsetOf(scheduler.ThreadContext, "frame") + @offsetOf(TrapFrame, "r8"),
        @offsetOf(scheduler.ThreadContext, "frame"),
        user_rsp_symbol,
    });
}

fn asmStageUserReturnFromWorkFrame(comptime work_frame_symbol: []const u8, comptime iret_offset: usize) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\lea {s}(%rip), %rcx
        \\mov ${d}, %rdx
    , .{ work_frame_symbol, iret_offset }) ++ asmCallAligned("stageUserReturnFromFramePointerForCurrentCpu");
}

fn asmStageUserReturnFromWorkFramePointer(comptime iret_offset: usize) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\mov %rax, %rcx
        \\mov ${d}, %rdx
    , .{iret_offset}) ++ asmCallAligned("stageUserReturnFromFramePointerForCurrentCpu");
}

fn asmExceptionWithErrorHandlerBody(comptime vec_number: u64) []const u8 {
    return asmCallAligned("pageFaultWorkFrameForCurrentCpuFromAsm") ++
        \\mov %rax, %r12
    ++ asmCopyStackFrameToWorkFramePointer(exception_trap_frame_qword_count) ++
        std.fmt.comptimePrint(
            \\mov 136(%r12), %rax
            \\and $0x3, %rax
            \\cmp $0x3, %rax
            \\jne 1f
            \\mov ${d}, %rcx
            \\mov %r12, %rdx
        , .{vec_number}) ++
        asmCallAligned("fatalUserExceptionWithErrorDispatch") ++
        asmCallAligned("restoreCurrentThreadFxState") ++
        asmCallAligned("fatalExceptionResumeWorkFrameForCurrentCpuFromAsm") ++
        asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\1:
    ++ std.fmt.comptimePrint(
        \\mov ${d}, %rcx
        \\mov %r12, %rdx
    , .{vec_number}) ++
        asmCallAligned("exceptionWithErrorCommon") ++
        \\ud2
    ;
}

fn asmSysretReturnFromWorkFrame(comptime work_frame_symbol: []const u8, comptime user_cr3_symbol: []const u8) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\mov {s}+120(%rip), %rcx
        \\mov %rcx, %r10
        \\shl $16, %r10
        \\sar $16, %r10
        \\cmp %rcx, %r10
        \\jne 8f
        \\mov {s}+136(%rip), %r11
        \\mov {s}(%rip), %r10
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
        user_cr3_symbol,
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

fn asmSysretReturnFromStackFrame(comptime user_cr3_symbol: []const u8) []const u8 {
    _ = user_cr3_symbol;
    return "";
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
    const cpu_slot = scheduler.currentCpuSlot();
    writeReg(h, "CPU", @intCast(cpu_slot));
    if (scheduler.getThreadContextConst(scheduler.currentThreadIndex())) |ctx| {
        writeReg(h, "FS_BASE", ctx.fs_base);
    }
    writeReg(h, "RAX", frame.rax);
    writeReg(h, "RBX", frame.rbx);
    writeReg(h, "RCX", frame.rcx);
    writeReg(h, "RDX", frame.rdx);
    writeReg(h, "RSI", frame.rsi);
    writeReg(h, "RDI", frame.rdi);
    writeReg(h, "R8", frame.r8);
    writeReg(h, "R9", frame.r9);
    writeReg(h, "R10", frame.r10);
    writeReg(h, "R11", frame.r11);
    writeReg(h, "R12", frame.r12);
    writeReg(h, "R13", frame.r13);
    writeReg(h, "R14", frame.r14);
    writeReg(h, "R15", frame.r15);
    writeReg(h, "RBP", frame.rbp);
    writeReg(h, "RSP", frame.rsp);
}

fn writeExceptionWithErrorSummary(h: *const Hooks, vec: u64, frame: *const ExceptionTrapFrame) void {
    h.write(exceptionName(vec));
    h.write("\n");
    h.write("  THREAD=");
    h.write(h.thread_label(scheduler.currentThreadIndex()));
    h.write("\n");
    h.write("  PRINCIPAL=");
    h.write(h.principal_label(scheduler.currentUserPrincipal()));
    h.write("\n");
    if (vec == 14) {
        const cr2 = h.read_cr2();
        const user_mode = (frame.error_code & (1 << 2)) != 0;
        h.write("  CR2=");
        h.write_hex_raw(cr2);
        h.write("\n");
        h.dump_page_walk_for_va(if (user_mode) scheduler.currentUserCr3() else h.read_cr3(), cr2);
        h.log_page_fault_step2(cr2, frame);
    }
    h.write("  EC=");
    h.write_hex_raw(frame.error_code);
    h.write("\n");
    h.write("  RIP=");
    h.write_hex_raw(frame.rip);
    h.write("\n");
    h.write("  CS=");
    h.write_hex_raw(frame.cs);
    h.write("\n");
    h.write("  SS=");
    h.write_hex_raw(frame.ss);
    h.write("\n");
    h.write("  LAST_USER_RIP=");
    h.write_hex_raw(last_user_return_rip);
    h.write("\n");
    h.write("  LAST_STAGE_RIP=");
    h.write_hex_raw(last_stage_saved_rip);
    h.write("\n");
    writeCommonTrapRegs(h, frame);
}

fn writeTrapSummary(h: *const Hooks, label: []const u8, frame: *const TrapFrame) void {
    h.write(label);
    h.write("\n");
    h.write("  THREAD=");
    h.write(h.thread_label(scheduler.currentThreadIndex()));
    h.write("\n");
    h.write("  PRINCIPAL=");
    h.write(h.principal_label(scheduler.currentUserPrincipal()));
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
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\call applyCurrentThreadFsBaseForUserReturnFromAsm
        \\mov %r15, %rsp
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\call userReturnCr3ForCurrentCpuFromAsm
        \\mov %r15, %rsp
        \\push %rax
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\call userReturnIretFrameForCurrentCpuFromAsm
        \\mov %r15, %rsp
        \\push %rax
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\call userReturnSavedGprsForCurrentCpuFromAsm
        \\mov %r15, %rsp
        \\mov %rax, %rsi
        \\pop %rdi
        \\pop %rax
        \\mov %rax, %cr3
        \\mov 112(%rsi), %r10
        \\mov %r10, last_user_return_rax(%rip)
        \\mov (%rdi), %r10
        \\mov %r10, last_user_return_rip(%rip)
        \\mov 0(%rsi), %r15
        \\mov 8(%rsi), %r14
        \\mov 16(%rsi), %r13
        \\mov 24(%rsi), %r12
        \\mov 32(%rsi), %r11
        \\mov 48(%rsi), %r9
        \\mov 56(%rsi), %r8
        \\mov 64(%rsi), %rbp
        \\mov 88(%rsi), %rdx
        \\mov 96(%rsi), %rcx
        \\mov 104(%rsi), %rbx
        \\mov 112(%rsi), %rax
        \\mov %rdi, %rsp
        \\mov 40(%rsi), %r10
        \\mov 72(%rsi), %rdi
        \\mov 80(%rsi), %rsi
        \\iretq
    );
}

pub export fn pageFaultDispatch(frame: *const ExceptionTrapFrame) callconv(.c) u64 {
    const h = getHooks();
    const cr2 = h.read_cr2();
    const pf_cap = capability.issuePageFaultCapability(scheduler.currentUserPrincipal(), frame, cr2) orelse return 0;
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
        scheduler.currentUserPrincipal(),
        @intCast(vec),
        fatalExceptionResumeWorkFrameForCurrentCpuFromAsm(),
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

pub export fn divideErrorHandlerCommon(frame: *const TrapFrame) callconv(.c) noreturn {
    const h = getHooks();
    asm volatile ("cli");
    writeTrapSummary(h, "DIVIDE ERROR", frame);
    h.halt_loop();
}

pub export fn fatalUserTrapDispatch(vec: u64, frame: *const TrapFrame) callconv(.c) void {
    const h = getHooks();
    asm volatile ("cli");
    const label = switch (vec) {
        0 => "DIVIDE ERROR",
        6 => "INVALID OPCODE",
        else => "TRAP",
    };
    writeTrapSummary(h, label, frame);
    h.write("  ACTION=terminate process\n");
    h.resume_after_fatal_user_exception(
        scheduler.currentUserPrincipal(),
        @intCast(vec),
        fatalExceptionResumeWorkFrameForCurrentCpuFromAsm(),
    );
}

pub export fn timerInterruptDispatch(frame: *TrapFrame) callconv(.c) void {
    const h = getHooks();
    lapic.eoiLegacyPicMaster();
    lapic.eoi();
    const user_mode = ((frame.cs & 0x3) == 0x3) and ((frame.ss & 0x3) == 0x3);
    if (!scheduler.schedulerRunsOnCurrentCpu()) {
        if (user_mode and !scheduler.currentApUserThreadCanContinue()) {
            smp.returnCurrentApToIdleFromInterrupt();
        }
        if (user_mode and scheduler.apUserTimerPreemptionEnabled() and scheduler.shouldPreemptCurrentApUserThread() and scheduler.saveApUserTimerFrame(frame)) {
            smp.returnCurrentApToIdleFromInterrupt();
        }
        scheduler.noteCurrentCpuIdleTick();
        return;
    }
    scheduler.lapic_tick_count +%= 1;
    if (!h.kernel_state_ready.*) return;
    scheduler.wakeThreadsForTimer(scheduler.lapic_tick_count);
    if (h.scheduler_quantum_ticks == 0) return;
    if (!user_mode) return;
    scheduler.noteUserTimerTick();

    const current_thread = scheduler.currentThreadIndex();
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
    ++ asmCallAligned("saveCurrentThreadFxState") ++ asmCallAligned("pageFaultWorkFrameForCurrentCpuFromAsm") ++
        \\mov %rax, %r12
    ++ asmCopyStackFrameToWorkFramePointer(exception_trap_frame_qword_count) ++
        \\mov %r12, %rcx
    ++ asmCallAligned("pageFaultDispatch") ++
        \\test %rax, %rax
        \\jz 8f
    ++ asmCallAligned("restoreCurrentThreadFxState") ++
        \\mov %r12, %rax
    ++ asmStageUserReturnFromWorkFramePointer(exception_trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\8:
        \\mov $14, %rcx
        \\mov %r12, %rdx
    ++ asmCallAligned("fatalUserExceptionWithErrorDispatch") ++ asmCallAligned("restoreCurrentThreadFxState") ++ asmCallAligned("fatalExceptionResumeWorkFrameForCurrentCpuFromAsm") ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++
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
        \\mov %rsp, %rax
    ++ asmStageUserReturnFromWorkFramePointer(exception_trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\2:
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
        ++ asmCallAligned("markSyscallEntryInt80ForCurrentCpuFromAsm") ++ asmCallAligned("syscallInterruptWorkFrameForCurrentCpuFromAsm") ++ asmCopyStackFrameToWorkFramePointer(trap_frame_qword_count) ++
            \\mov %rax, %rcx
        ++ asmCallAligned("syscallDispatch") ++
            \\push %rax
        ++ asmCallAligned("syscallInterruptWorkFrameForCurrentCpuFromAsm") ++
            \\pop %r10
        ++ asmWriteSyscallReturnToWorkFramePointer() ++ asmCallAligned("syscallInterruptWorkFrameForCurrentCpuFromAsm") ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
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
        ++ asmCallAligned("markSyscallEntryInt80ForCurrentCpuFromAsm") ++ asmCallAligned("saveCurrentThreadFxState") ++ asmCallAligned("syscallInterruptWorkFrameForCurrentCpuFromAsm") ++ asmCopyStackFrameToWorkFramePointer(trap_frame_qword_count) ++
            \\mov %rax, %rcx
        ++ asmCallAligned("syscallDispatch") ++
            \\push %rax
        ++ asmCallAligned("syscallInterruptWorkFrameForCurrentCpuFromAsm") ++
            \\pop %r10
        ++ asmWriteSyscallReturnToWorkFramePointer() ++ asmCallAligned("restoreCurrentThreadFxState") ++ asmCallAligned("syscallInterruptWorkFrameForCurrentCpuFromAsm") ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
        );
    }
}

fn lstarUserRspSymbol(comptime cpu_slot: usize) []const u8 {
    return std.fmt.comptimePrint("syscall_entry_user_rsps+{d}", .{cpu_slot * @sizeOf(u64)});
}

fn lstarWorkFrameSymbol(comptime cpu_slot: usize) []const u8 {
    return std.fmt.comptimePrint("syscall_interrupt_work_frames+{d}", .{cpu_slot * @sizeOf(TrapFrame)});
}

fn lstarCurrentThreadSymbol(comptime cpu_slot: usize) []const u8 {
    return std.fmt.comptimePrint("current_thread_indices+{d}", .{cpu_slot * @sizeOf(usize)});
}

fn lstarCurrentPrincipalSymbol(comptime cpu_slot: usize) []const u8 {
    return std.fmt.comptimePrint("current_user_principals+{d}", .{cpu_slot * @sizeOf(kernel.PrincipalId)});
}

fn lstarUserCr3Symbol(comptime cpu_slot: usize) []const u8 {
    return std.fmt.comptimePrint("user_cr3_values+{d}", .{cpu_slot * @sizeOf(u64)});
}

fn lstarReturnWritebackSymbol(comptime cpu_slot: usize) []const u8 {
    return std.fmt.comptimePrint("syscall_return_writeback_enabled_by_cpu+{d}", .{cpu_slot * @sizeOf(u64)});
}

fn lstarEntryIsLstarSymbol(comptime cpu_slot: usize) []const u8 {
    return std.fmt.comptimePrint("syscall_entry_is_lstars+{d}", .{cpu_slot * @sizeOf(u64)});
}

fn asmLstarEntryPrefix(comptime cpu_slot: usize) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\mov %rsp, syscall_entry_user_rsp(%rip)
        \\mov %rsp, syscall_entry_user_rsps+{d}(%rip)
        \\mov %r15, syscall_entry_saved_r15s+{d}(%rip)
        \\mov kernel_cr3_value(%rip), %r15
        \\mov %r15, %cr3
        \\movq ${d}, syscall_lstar_cpu_slot(%rip)
        \\movq ${d}, syscall_interrupt_work_frame_cpu_slot(%rip)
        \\mov kernel_syscall_stack_tops+{d}(%rip), %rsp
        \\mov syscall_entry_saved_r15s+{d}(%rip), %r15
        \\
    , .{
        cpu_slot * @sizeOf(u64),
        cpu_slot * @sizeOf(u64),
        cpu_slot,
        cpu_slot,
        cpu_slot * @sizeOf(u64),
        cpu_slot * @sizeOf(u64),
    });
}

fn asmLstarHandlerForCpu(comptime cpu_slot: usize, comptime user_cs: u64, comptime user_ss: u64) []const u8 {
    const user_rsp_symbol = lstarUserRspSymbol(cpu_slot);
    const work_frame_symbol = lstarWorkFrameSymbol(cpu_slot);
    const current_thread_symbol = lstarCurrentThreadSymbol(cpu_slot);
    const current_principal_symbol = lstarCurrentPrincipalSymbol(cpu_slot);
    const user_cr3_symbol = lstarUserCr3Symbol(cpu_slot);
    const writeback_symbol = lstarReturnWritebackSymbol(cpu_slot);
    const entry_is_lstar_symbol = lstarEntryIsLstarSymbol(cpu_slot);
    // LSTAR is also the Linux ABI syscall entry. Do not pre-dispatch raw
    // syscall numbers here: Linux recvfrom/sendmsg are 0x2d/0x2e and must
    // reach the ABI delegate before any CapabilityOS syscall fast path.
    const slowpath_gate =
        \\jmp 29f
        \\
    ;
    if (debug_skip_syscall_fx_state) {
        return asmLstarEntryPrefix(cpu_slot) ++ slowpath_gate ++ std.fmt.comptimePrint(
            \\
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
            \\movzbq {s}(%rip), %rax
            \\cmp $32, %rax
            \\jb 23f
            \\mov $1, %rax
            \\jmp 23f
            \\22:
            \\mov $1, %rax
            \\23:
            \\mov {s}(%rip), %r10
            \\mov %r10, %cr3
            \\mov {s}(%rip), %rsp
            \\sysretq
            \\24:
        , .{ current_principal_symbol, user_cr3_symbol, user_rsp_symbol }) ++ asmCallIpcFastDispatchNoCr3() ++ asmIpcCallReplyRecvSignalOnlyNoCr3(user_cs, user_ss, user_rsp_symbol, current_thread_symbol, current_principal_symbol, user_cr3_symbol) ++ asmSysretReturnFromStackFrame(user_cr3_symbol) ++ asmCopyStackFrameToWorkFrame(work_frame_symbol, trap_frame_qword_count) ++ asmStageUserReturnFromWorkFrame(work_frame_symbol, trap_frame_iret_offset) ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
            \\
        ++ asmIpcFrameDispatchNoCr3(user_cs, user_ss, user_rsp_symbol, writeback_symbol) ++ asmSysretReturnFromStackFrame(user_cr3_symbol) ++ asmCopyStackFrameToWorkFrame(work_frame_symbol, trap_frame_qword_count) ++ asmStageUserReturnFromWorkFrame(work_frame_symbol, trap_frame_iret_offset) ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            std.fmt.comptimePrint(
                \\jmp userReturnToSavedFrame
                \\29:
                \\push %r15
                \\mov kernel_cr3_value(%rip), %r15
                \\mov %r15, %cr3
                \\pop %r15
                \\movq $1, syscall_entry_is_lstar(%rip)
                \\movq $1, {s}(%rip)
                \\pushq %[user_ss]
                \\pushq {s}(%rip)
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
            , .{ entry_is_lstar_symbol, user_rsp_symbol }) ++ asmCallSyscallDispatchFromStackFrame("syscallLstarDispatch") ++
            std.fmt.comptimePrint(
                \\cmpq $0, {s}(%rip)
                \\je 7f
                \\mov %rax, 112(%rsp)
                \\7:
                \\
            , .{writeback_symbol}) ++ asmSysretReturnFromStackFrame(user_cr3_symbol) ++ asmCopyStackFrameToWorkFrame(work_frame_symbol, trap_frame_qword_count) ++ asmStageUserReturnFromWorkFrame(work_frame_symbol, trap_frame_iret_offset) ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
            \\
        ;
    } else {
        return asmLstarEntryPrefix(cpu_slot) ++ slowpath_gate ++ std.fmt.comptimePrint(
            \\
            \\jmp 29f
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
            \\movzbq {s}(%rip), %rax
            \\cmp $32, %rax
            \\jb 23f
            \\mov $1, %rax
            \\jmp 23f
            \\22:
            \\mov $1, %rax
            \\23:
            \\mov {s}(%rip), %r10
            \\mov %r10, %cr3
            \\mov {s}(%rip), %rsp
            \\sysretq
            \\24:
        , .{ current_principal_symbol, user_cr3_symbol, user_rsp_symbol }) ++ asmCallIpcFastDispatchNoCr3() ++ asmIpcCallReplyRecvSignalOnlyNoCr3(user_cs, user_ss, user_rsp_symbol, current_thread_symbol, current_principal_symbol, user_cr3_symbol) ++ asmSysretReturnFromStackFrame(user_cr3_symbol) ++ asmCopyStackFrameToWorkFrame(work_frame_symbol, trap_frame_qword_count) ++ asmStageUserReturnFromWorkFrame(work_frame_symbol, trap_frame_iret_offset) ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
            \\
        ++ asmIpcFrameDispatchNoCr3(user_cs, user_ss, user_rsp_symbol, writeback_symbol) ++ asmSysretReturnFromStackFrame(user_cr3_symbol) ++ asmCopyStackFrameToWorkFrame(work_frame_symbol, trap_frame_qword_count) ++ asmStageUserReturnFromWorkFrame(work_frame_symbol, trap_frame_iret_offset) ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            std.fmt.comptimePrint(
                \\jmp userReturnToSavedFrame
                \\29:
                \\push %r15
                \\mov kernel_cr3_value(%rip), %r15
                \\mov %r15, %cr3
                \\pop %r15
                \\movq $1, syscall_entry_is_lstar(%rip)
                \\movq $1, {s}(%rip)
                \\pushq %[user_ss]
                \\pushq {s}(%rip)
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
            , .{ entry_is_lstar_symbol, user_rsp_symbol }) ++ asmCallAligned("saveCurrentThreadFxState") ++
            \\mov %rsp, %rcx
            \\
        ++ asmCallSyscallDispatchFromStackFrame("syscallLstarDispatch") ++
            std.fmt.comptimePrint(
                \\cmpq $0, {s}(%rip)
                \\je 7f
                \\mov %rax, 112(%rsp)
                \\7:
                \\
            , .{writeback_symbol}) ++ asmCallAligned("restoreCurrentThreadFxState") ++ asmSysretReturnFromStackFrame(user_cr3_symbol) ++ asmCopyStackFrameToWorkFrame(work_frame_symbol, trap_frame_qword_count) ++ asmStageUserReturnFromWorkFrame(work_frame_symbol, trap_frame_iret_offset) ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
            \\jmp userReturnToSavedFrame
            \\
        ;
    }
}

pub fn syscallLstarEntryForCpu(cpu_slot: usize) usize {
    return switch (cpu_slot) {
        0 => @intFromPtr(&syscallLstarHandlerStub),
        1 => @intFromPtr(&syscallLstarHandlerStubCpu1),
        2 => @intFromPtr(&syscallLstarHandlerStubCpu2),
        3 => @intFromPtr(&syscallLstarHandlerStubCpu3),
        else => @intFromPtr(&syscallLstarHandlerStub),
    };
}

pub fn syscallLstarEntries() [smp.max_cpus]usize {
    var entries: [smp.max_cpus]usize = [_]usize{0} ** smp.max_cpus;
    inline for (0..smp.max_cpus) |cpu_slot| {
        entries[cpu_slot] = syscallLstarEntryForCpu(cpu_slot);
    }
    return entries;
}

pub export fn syscallLstarHandlerStub() callconv(.naked) noreturn {
    const user_cs: u64 = @as(u64, x86_platform.gdt_user_code_selector) | 0x3;
    const user_ss: u64 = @as(u64, x86_platform.gdt_user_data_selector) | 0x3;
    asm volatile (asmLstarHandlerForCpu(0, user_cs, user_ss)
        :
        : [user_cs] "i" (user_cs),
          [user_ss] "i" (user_ss),
    );
}

pub export fn syscallLstarHandlerStubCpu1() callconv(.naked) noreturn {
    const user_cs: u64 = @as(u64, x86_platform.gdt_user_code_selector) | 0x3;
    const user_ss: u64 = @as(u64, x86_platform.gdt_user_data_selector) | 0x3;
    asm volatile (asmLstarHandlerForCpu(1, user_cs, user_ss)
        :
        : [user_cs] "i" (user_cs),
          [user_ss] "i" (user_ss),
    );
}

pub export fn syscallLstarHandlerStubCpu2() callconv(.naked) noreturn {
    const user_cs: u64 = @as(u64, x86_platform.gdt_user_code_selector) | 0x3;
    const user_ss: u64 = @as(u64, x86_platform.gdt_user_data_selector) | 0x3;
    asm volatile (asmLstarHandlerForCpu(2, user_cs, user_ss)
        :
        : [user_cs] "i" (user_cs),
          [user_ss] "i" (user_ss),
    );
}

pub export fn syscallLstarHandlerStubCpu3() callconv(.naked) noreturn {
    const user_cs: u64 = @as(u64, x86_platform.gdt_user_code_selector) | 0x3;
    const user_ss: u64 = @as(u64, x86_platform.gdt_user_data_selector) | 0x3;
    asm volatile (asmLstarHandlerForCpu(3, user_cs, user_ss)
        :
        : [user_cs] "i" (user_cs),
          [user_ss] "i" (user_ss),
    );
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
        ++ asmCallAligned("timerInterruptWorkFrameForCurrentCpuFromAsm") ++
            \\mov %rax, %r12
        ++ asmCopyStackFrameToWorkFramePointer(trap_frame_qword_count) ++
            \\mov %r12, %rcx
        ++ asmCallAligned("timerInterruptDispatch") ++
            \\mov 112(%r12), %r10
            \\mov %r10, timer_stack_rax_post_dispatch(%rip)
            \\mov %r10, timer_stack_rax_pre_log(%rip)
            \\mov %r12, %rcx
        ++ asmCallAligned("logTimerReturnFrame") ++
            \\mov %r12, %rax
        ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
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
        ++ asmCallAligned("saveCurrentThreadFxState") ++ asmCallAligned("timerInterruptWorkFrameForCurrentCpuFromAsm") ++
            \\mov %rax, %r12
        ++ asmCopyStackFrameToWorkFramePointer(trap_frame_qword_count) ++
            \\mov %r12, %rcx
        ++ asmCallAligned("timerInterruptDispatch") ++
            \\mov 112(%r12), %r10
            \\mov %r10, timer_stack_rax_post_dispatch(%rip)
        ++ asmCallAligned("restoreCurrentThreadFxState") ++
            \\mov 112(%r12), %r10
            \\mov %r10, timer_stack_rax_pre_log(%rip)
            \\mov %r12, %rcx
        ++ asmCallAligned("logTimerReturnFrame") ++
            \\mov %r12, %rax
        ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++ asmCallAligned("logCurrentStagedUserReturnFrame") ++
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
    ++ asmExceptionWithErrorHandlerBody(13));
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
    ++ asmExceptionWithErrorHandlerBody(10));
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
    ++ asmExceptionWithErrorHandlerBody(11));
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
    ++ asmExceptionWithErrorHandlerBody(12));
}

pub export fn divideErrorHandlerStub() callconv(.naked) noreturn {
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
    ++ asmCallAligned("trapFaultWorkFrameForCurrentCpuFromAsm") ++
        \\mov %rax, %r12
    ++ asmCopyStackFrameToWorkFramePointer(trap_frame_qword_count) ++
        \\mov 128(%r12), %rax
        \\and $0x3, %rax
        \\cmp $0x3, %rax
        \\jne 1f
        \\mov $0, %rcx
        \\mov %r12, %rdx
    ++ asmCallAligned("fatalUserTrapDispatch") ++ asmCallAligned("restoreCurrentThreadFxState") ++ asmCallAligned("fatalExceptionResumeWorkFrameForCurrentCpuFromAsm") ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\1:
        \\mov %r12, %rcx
    ++ asmCallAligned("divideErrorHandlerCommon") ++
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
    ++ asmCallAligned("trapFaultWorkFrameForCurrentCpuFromAsm") ++
        \\mov %rax, %r12
    ++ asmCopyStackFrameToWorkFramePointer(trap_frame_qword_count) ++
        \\mov 128(%r12), %rax
        \\and $0x3, %rax
        \\cmp $0x3, %rax
        \\jne 1f
        \\mov $6, %rcx
        \\mov %r12, %rdx
    ++ asmCallAligned("fatalUserTrapDispatch") ++ asmCallAligned("restoreCurrentThreadFxState") ++ asmCallAligned("fatalExceptionResumeWorkFrameForCurrentCpuFromAsm") ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\1:
        \\mov %r12, %rcx
    ++ asmCallAligned("invalidOpcodeHandlerCommon") ++
        \\ud2
    );
}
