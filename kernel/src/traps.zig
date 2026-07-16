const std = @import("std");
const kernel = @import("kernel.zig");
const boot_static = @import("boot/main_static.zig");
const halt = @import("halt.zig");
const kernel_log = @import("kernel_log.zig");
const kernel_runtime = @import("kernel_runtime.zig");
const kernel_vm = @import("memory/kernel_vm.zig");
const page_fault_log = @import("page_fault_log.zig");
const x86_platform = @import("arch/x86_64/platform.zig");
const interrupts = @import("interrupts.zig");
const lapic = @import("lapic.zig");
const process_abi = @import("kernel_abi_root").process_abi;
const user_copy = @import("user_copy.zig");
const user_vm = @import("memory/user_vm.zig");
const scheduler = @import("scheduler.zig").connection;
const smp = @import("smp.zig");

const TrapFrame = interrupts.TrapFrame;
const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;

const debug_skip_timer_fx_state = false;
const generic_device_interrupt_vector: u8 = 0x41;
const device_interrupt_vector_count: u8 = 1;
const trap_frame_qword_count = @sizeOf(TrapFrame) / @sizeOf(u64);
const exception_trap_frame_qword_count = @sizeOf(ExceptionTrapFrame) / @sizeOf(u64);
const user_return_gpr_qword_count = @offsetOf(TrapFrame, "rip") / @sizeOf(u64);
const user_return_iret_qword_count = 5;
const trap_frame_iret_offset = @offsetOf(TrapFrame, "rip");
const exception_trap_frame_iret_offset = @offsetOf(ExceptionTrapFrame, "rip");

pub export var page_fault_work_frames: [smp.max_cpus]ExceptionTrapFrame = [_]ExceptionTrapFrame{std.mem.zeroes(ExceptionTrapFrame)} ** smp.max_cpus;
pub export var trap_fault_work_frames: [smp.max_cpus]TrapFrame = [_]TrapFrame{std.mem.zeroes(TrapFrame)} ** smp.max_cpus;
pub export var fatal_exception_resume_work_frames: [smp.max_cpus]TrapFrame = [_]TrapFrame{std.mem.zeroes(TrapFrame)} ** smp.max_cpus;
pub export var timer_interrupt_work_frames: [smp.max_cpus]TrapFrame = [_]TrapFrame{std.mem.zeroes(TrapFrame)} ** smp.max_cpus;
pub export var syscall_work_frame: TrapFrame = std.mem.zeroes(TrapFrame);
pub export var syscall_work_frames: [smp.max_cpus]TrapFrame = [_]TrapFrame{std.mem.zeroes(TrapFrame)} ** smp.max_cpus;
pub export var syscall_entry_lock: u64 = 0;
pub export var user_return_saved_gprs_by_cpu: [smp.max_cpus][16]u64 align(16) = [_][16]u64{[_]u64{0} ** 16} ** smp.max_cpus;
pub export var user_return_iret_frames_by_cpu: [smp.max_cpus][8]u64 align(16) = [_][8]u64{[_]u64{0} ** 8} ** smp.max_cpus;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

fn updateStaticEnd(end: *usize, comptime ptr: anytype) void {
    end.* = maxStaticEnd(end.*, staticStorageEnd(@TypeOf(ptr.*), ptr));
}

fn mapStaticStorage(map_identity_range: *const fn (u64, usize) bool, comptime ptr: anytype) bool {
    return map_identity_range(@intFromPtr(ptr), @sizeOf(@TypeOf(ptr.*)));
}

const runtime_storage_ptrs = .{
    &page_fault_work_frames,
    &trap_fault_work_frames,
    &fatal_exception_resume_work_frames,
    &timer_interrupt_work_frames,
    &syscall_work_frame,
    &syscall_work_frames,
    &syscall_entry_lock,
    &user_return_saved_gprs_by_cpu,
    &user_return_iret_frames_by_cpu,
    &user_return_saved_r10,
    &user_return_saved_gprs,
    &user_return_iret_frame,
};

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    inline for (runtime_storage_ptrs) |ptr| updateStaticEnd(&end, ptr);
    return end;
}

pub fn mapKernelRuntimeStorage(map_identity_range: *const fn (u64, usize) bool) bool {
    inline for (runtime_storage_ptrs) |ptr| {
        if (!mapStaticStorage(map_identity_range, ptr)) return false;
    }
    return true;
}

extern var kernel_cr3_value: u64;
extern var kernel_syscall_stack_top: u64;
extern var kernel_syscall_stack_tops: [smp.max_cpus]u64;
extern var x86_rdtscp_supported: u64;
extern var thread_contexts_ptr: *anyopaque;
extern var lapic_tick_count: u64;
extern var user_return_saved_r10: u64;
extern var user_return_saved_gprs: [15]u64 align(16);
extern var user_return_iret_frame: [5]u64 align(16);

extern fn saveCurrentThreadFxState() callconv(.winapi) void;
extern fn restoreCurrentThreadFxState() callconv(.c) void;
extern fn syscallDispatch(frame: *TrapFrame) callconv(.winapi) u64;
extern fn resumeAfterFatalUserException(principal: kernel.PrincipalId, fault_vector: u8, out_frame: *TrapFrame) callconv(.winapi) void;

pub export fn timerInterruptWorkFrameForCurrentCpuFromAsm() callconv(.winapi) *TrapFrame {
    const cpu_slot = scheduler.currentCpu();
    const bounded_slot = if (cpu_slot < timer_interrupt_work_frames.len) cpu_slot else 0;
    return &timer_interrupt_work_frames[bounded_slot];
}

pub export fn syscallWorkFrameFromAsm() callconv(.winapi) *TrapFrame {
    return &syscall_work_frame;
}

fn boundedCurrentCpuSlot() usize {
    const cpu_slot = scheduler.currentCpu();
    return if (cpu_slot < smp.max_cpus) cpu_slot else 0;
}

pub export fn pageFaultWorkFrameForCurrentCpuFromAsm() callconv(.winapi) *ExceptionTrapFrame {
    return &page_fault_work_frames[boundedCurrentCpuSlot()];
}

pub export fn trapFaultWorkFrameForCurrentCpuFromAsm() callconv(.winapi) *TrapFrame {
    return &trap_fault_work_frames[boundedCurrentCpuSlot()];
}

pub export fn fatalExceptionResumeWorkFrameForCurrentCpuFromAsm() callconv(.winapi) *TrapFrame {
    return &fatal_exception_resume_work_frames[boundedCurrentCpuSlot()];
}

pub export fn stageUserReturnFromFramePointerForCurrentCpu(frame_addr: usize, iret_offset: usize) callconv(.winapi) void {
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
}

pub export fn userReturnSavedGprsForCurrentCpuFromAsm() callconv(.winapi) *u64 {
    const cpu_slot = boundedCurrentCpuSlot();
    return &user_return_saved_gprs_by_cpu[cpu_slot][0];
}

pub export fn userReturnIretFrameForCurrentCpuFromAsm() callconv(.winapi) *u64 {
    const cpu_slot = boundedCurrentCpuSlot();
    return &user_return_iret_frames_by_cpu[cpu_slot][0];
}

pub export fn userReturnCr3ForCurrentCpuFromAsm() callconv(.winapi) u64 {
    return scheduler.currentCr3();
}

pub export fn applyCurrentThreadFsBaseForUserReturnFromAsm() callconv(.winapi) void {
    _ = scheduler.applyThreadBases(scheduler.currentThread());
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

fn asmCpuSlotIntoEcx() []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\cmpq $0, x86_rdtscp_supported(%rip)
        \\je 3f
        \\rdtscp
        \\cmp ${d}, %ecx
        \\jb 4f
        \\3:
        \\mov $1, %eax
        \\cpuid
        \\shr $24, %ebx
        \\xor %ecx, %ecx
        \\lea runtime_lapic_ids(%rip), %rax
        \\5:
        \\cmpb %bl, (%rax,%rcx,1)
        \\je 4f
        \\inc %ecx
        \\cmp ${d}, %ecx
        \\jb 5b
        \\xor %ecx, %ecx
        \\4:
        \\
    , .{ smp.max_cpus, smp.max_cpus });
}

fn asmCopyUserInterruptFrameToCpuWorkFrame(comptime work_frame_symbol: []const u8) []const u8 {
    return std.fmt.comptimePrint(asmCpuSlotIntoEcx() ++
        \\
        \\mov %ecx, %r14d
        \\mov %r14, %r13
        \\shl $3, %r13
        \\imul ${d}, %r14, %r14
        \\lea {s}(%rip), %r12
        \\add %r14, %r12
        \\mov %r12, %rdi
        \\mov %rsp, %rsi
        \\mov ${d}, %ecx
        \\cld
        \\rep movsq
        \\lea kernel_syscall_stack_tops(%rip), %r14
        \\mov (%r14,%r13,1), %rsp
        \\push %r12
        \\
    , .{ @sizeOf(TrapFrame), work_frame_symbol, trap_frame_qword_count });
}

fn asmCopyFramePointerToWorkFrameOnKernelStack(
    comptime frame_reg: []const u8,
    comptime work_frame_symbol: []const u8,
    comptime cpu_slot: usize,
    comptime qword_count: usize,
) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\mov {s}, %r12
        \\mov kernel_syscall_stack_tops+{d}(%rip), %rsp
        \\lea {s}(%rip), %rdi
        \\mov %r12, %rsi
        \\mov ${d}, %ecx
        \\cld
        \\rep movsq
        \\
    , .{ frame_reg, cpu_slot * @sizeOf(u64), work_frame_symbol, qword_count });
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

fn asmDispatchKernelInterruptPreservingFx(comptime target: []const u8) []const u8 {
    return std.fmt.comptimePrint(
        \\
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $544, %rsp
        \\fxsave64 32(%rsp)
        \\mov %r15, %rcx
        \\call {s}
        \\fxrstor64 32(%rsp)
        \\mov %r15, %rsp
        \\
    , .{target});
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

fn writeReg(label: []const u8, value: u64) void {
    kernel_log.write("  ");
    kernel_log.write(label);
    kernel_log.write("=");
    kernel_log.writeHexRaw(value);
    kernel_log.write("\n");
}

fn writeCommonTrapRegs(frame: anytype) void {
    const cpu_slot = scheduler.currentCpu();
    writeReg("CPU", @intCast(cpu_slot));
    if (scheduler.threadContext(scheduler.currentThread())) |ctx| {
        writeReg("FS_BASE", ctx.fs_base);
    }
    writeReg("RAX", frame.rax);
    writeReg("RBX", frame.rbx);
    writeReg("RCX", frame.rcx);
    writeReg("RDX", frame.rdx);
    writeReg("RSI", frame.rsi);
    writeReg("RDI", frame.rdi);
    writeReg("R8", frame.r8);
    writeReg("R9", frame.r9);
    writeReg("R10", frame.r10);
    writeReg("R11", frame.r11);
    writeReg("R12", frame.r12);
    writeReg("R13", frame.r13);
    writeReg("R14", frame.r14);
    writeReg("R15", frame.r15);
    writeReg("RBP", frame.rbp);
    writeReg("RSP", frame.rsp);
}

fn writeExceptionWithErrorSummary(vec: u64, frame: *const ExceptionTrapFrame) void {
    kernel_log.write(exceptionName(vec));
    kernel_log.write("\n");
    kernel_log.write("  THREAD=");
    kernel_log.write(kernel_runtime.threadLabel(scheduler.currentThread()));
    kernel_log.write("\n");
    kernel_log.write("  PRINCIPAL=");
    kernel_log.write(kernel_runtime.principalLabel(scheduler.currentPrincipal()));
    kernel_log.write("\n");
    if (vec == 14) {
        const cr2 = x86_platform.readCr2();
        const user_mode = (frame.error_code & (1 << 2)) != 0;
        kernel_log.write("  CR2=");
        kernel_log.writeHexRaw(cr2);
        kernel_log.write("\n");
        page_fault_log.dumpPageWalkForVa(if (user_mode) scheduler.currentCr3() else kernel_vm.readCr3(), cr2);
        page_fault_log.logStep2(cr2, frame);
    }
    kernel_log.write("  EC=");
    kernel_log.writeHexRaw(frame.error_code);
    kernel_log.write("\n");
    kernel_log.write("  RIP=");
    kernel_log.writeHexRaw(frame.rip);
    kernel_log.write("\n");
    kernel_log.write("  CS=");
    kernel_log.writeHexRaw(frame.cs);
    kernel_log.write("\n");
    kernel_log.write("  SS=");
    kernel_log.writeHexRaw(frame.ss);
    kernel_log.write("\n");
    writeCommonTrapRegs(frame);
}

fn writeTrapSummary(label: []const u8, frame: *const TrapFrame) void {
    kernel_log.write(label);
    kernel_log.write("\n");
    kernel_log.write("  THREAD=");
    kernel_log.write(kernel_runtime.threadLabel(scheduler.currentThread()));
    kernel_log.write("\n");
    kernel_log.write("  PRINCIPAL=");
    kernel_log.write(kernel_runtime.principalLabel(scheduler.currentPrincipal()));
    kernel_log.write("\n");
    kernel_log.write("  RIP=");
    kernel_log.writeHexRaw(frame.rip);
    kernel_log.write("\n");
    writeCommonTrapRegs(frame);
}

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
        \\pop %r10
        \\# Keep user RAX live across the CR3 switch; R10 is restored below.
        \\mov 112(%rsi), %rax
        \\mov %r10, %cr3
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
        \\mov %rdi, %rsp
        \\mov 40(%rsi), %r10
        \\mov 72(%rsi), %rdi
        \\mov 80(%rsi), %rsi
        \\iretq
    );
}

pub export fn syscallEntryStub() callconv(.naked) noreturn {
    asm volatile (
        \\1:
        \\lock btsq $0, syscall_entry_lock(%rip)
        \\jc 1b
        \\mov %r15, syscall_work_frame+0(%rip)
        \\mov %r14, syscall_work_frame+8(%rip)
        \\mov %r13, syscall_work_frame+16(%rip)
        \\mov %r12, syscall_work_frame+24(%rip)
        \\movq $0, syscall_work_frame+32(%rip)
        \\mov %r10, syscall_work_frame+40(%rip)
        \\mov %r9, syscall_work_frame+48(%rip)
        \\mov %r8, syscall_work_frame+56(%rip)
        \\mov %rbp, syscall_work_frame+64(%rip)
        \\mov %rdi, syscall_work_frame+72(%rip)
        \\mov %rsi, syscall_work_frame+80(%rip)
        \\mov %rdx, syscall_work_frame+88(%rip)
        \\movq $0, syscall_work_frame+96(%rip)
        \\mov %rcx, syscall_work_frame+120(%rip)
        \\mov %rbx, syscall_work_frame+104(%rip)
        \\mov %rax, syscall_work_frame+112(%rip)
        \\movq $0x1b, syscall_work_frame+128(%rip)
        \\mov %r11, syscall_work_frame+136(%rip)
        \\mov %rsp, syscall_work_frame+144(%rip)
        \\movq $0x23, syscall_work_frame+152(%rip)
    ++ asmCpuSlotIntoEcx() ++
        \\
        \\mov %ecx, %r12d
        \\mov %r12, %r14
        \\shl $3, %r14
        \\imul %[trap_frame_size], %r12, %r13
        \\lea syscall_work_frames(%rip), %rdi
        \\add %r13, %rdi
        \\lea syscall_work_frame(%rip), %rsi
        \\mov %[trap_frame_qwords], %ecx
        \\cld
        \\rep movsq
        \\lea syscall_work_frames(%rip), %r12
        \\add %r13, %r12
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\lea kernel_syscall_stack_tops(%rip), %r13
        \\mov (%r13,%r14,1), %rsp
        \\push %r12
        \\movq $0, syscall_entry_lock(%rip)
    ++ asmCallAligned("saveCurrentThreadFxState") ++
        \\mov (%rsp), %r12
        \\mov %r12, %rcx
    ++ asmCallAligned("syscallDispatch") ++
        \\mov (%rsp), %r12
    ++ asmCallAligned("restoreCurrentThreadFxState") ++
        \\mov (%rsp), %r12
        \\add $8, %rsp
        \\mov %r12, %rax
    ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        :
        : [trap_frame_size] "i" (@sizeOf(TrapFrame)),
          [trap_frame_qwords] "i" (trap_frame_qword_count),
    );
}

pub export fn pageFaultDispatch(frame: *ExceptionTrapFrame) callconv(.winapi) u64 {
    const cr2 = x86_platform.readCr2();
    const ec = frame.error_code;
    const user_mode = (ec & (1 << 2)) != 0;
    if (!user_mode or !user_vm.isUserCanonicalVa(cr2)) return 0;
    if (!kernel_runtime.kernel_state_ready) return 0;
    const principal = scheduler.currentPrincipal();
    const fault_page_va = cr2 & ~@as(u64, 4095);
    const write_access = (ec & (1 << 1)) != 0;
    const instruction_fetch = (ec & (1 << 4)) != 0;
    user_vm.lockAddressSpaces();
    defer user_vm.unlockAddressSpaces();
    if (write_access) {
        if (kernel_runtime.kernel_state_global.ensureNativeVmaCowMapping(principal, fault_page_va, write_access, instruction_fetch, kernel_runtime.global_free_list)) |mapping| {
            if (mapping.invalidate_size_bytes != 0) {
                if (mapping.invalidate_size_bytes > std.math.maxInt(usize)) return 0;
                if (!user_vm.invalidatePresentUserLinearRegionPtes(
                    principal,
                    mapping.invalidate_start_va,
                    @intCast(mapping.invalidate_size_bytes),
                )) return 0;
            }
            var paddrs = [_]u64{mapping.paddr};
            if (user_vm.lookupUserMappedPaddrForVa(principal, fault_page_va) != null) {
                if (user_vm.remapTrustedUserPaddrsWithProt(
                    principal,
                    fault_page_va,
                    paddrs[0..],
                    mapping.prot,
                )) return 1;
            } else if (user_vm.mapLazyUserPaddrsWithProt(
                principal,
                fault_page_va,
                paddrs[0..],
                mapping.prot,
            )) {
                return 1;
            }
        }
    }
    if (user_vm.lookupUserMappedPaddrForVa(principal, fault_page_va) != null) {
        // Another thread in this address space may have won the same lazy
        // fault while this CPU waited for the address-space lock.  The
        // hardware error still describes the earlier non-present PTE, while
        // the locked page-table view now contains the valid mapping.  Return
        // through the normal user CR3 reload and retry instead of treating
        // that resolved race as a fatal user fault.
        const not_present_fault = (ec & 1) == 0;
        return if (not_present_fault) 1 else 0;
    }
    if (kernel_runtime.kernel_state_global.ensureNativeVmaFaultMapping(principal, fault_page_va, write_access, instruction_fetch, kernel_runtime.global_free_list)) |mapping| {
        var paddrs = [_]u64{mapping.paddr};
        const mapped = if (fault_page_va == 0 and mapping.prot.read and !mapping.prot.write and mapping.prot.exec)
            user_vm.mapLazyLowPageZeroPaddrsWithProt(
                principal,
                fault_page_va,
                paddrs[0..],
                mapping.prot,
            )
        else
            user_vm.mapLazyUserPaddrsWithProt(
                principal,
                fault_page_va,
                paddrs[0..],
                mapping.prot,
            );
        if (mapped) return 1;
    }
    return 0;
}

pub export fn exceptionWithErrorCommon(vec: u64, frame: *const ExceptionTrapFrame) callconv(.winapi) noreturn {
    asm volatile ("cli");
    writeExceptionWithErrorSummary(vec, frame);
    halt.haltLoop();
}

pub export fn fatalUserExceptionWithErrorDispatch(vec: u64, frame: *const ExceptionTrapFrame) callconv(.winapi) void {
    asm volatile ("cli");
    writeExceptionWithErrorSummary(vec, frame);
    kernel_log.write("  ACTION=terminate process\n");
    resumeAfterFatalUserException(
        scheduler.currentPrincipal(),
        @intCast(vec),
        fatalExceptionResumeWorkFrameForCurrentCpuFromAsm(),
    );
}

pub export fn doubleFaultHandlerCommon(error_code: u64) callconv(.winapi) noreturn {
    asm volatile ("cli");
    kernel_log.write("DOUBLE FAULT\n");
    kernel_log.write("  EC=");
    kernel_log.writeHexRaw(error_code);
    kernel_log.write("\n");
    halt.haltLoop();
}

pub export fn invalidTssHandlerCommon(error_code: u64) callconv(.winapi) noreturn {
    asm volatile ("cli");
    kernel_log.write("INVALID TSS\n");
    kernel_log.write("  EC=");
    kernel_log.writeHexRaw(error_code);
    kernel_log.write("\n");
    halt.haltLoop();
}

pub export fn segmentNotPresentHandlerCommon(error_code: u64) callconv(.winapi) noreturn {
    asm volatile ("cli");
    kernel_log.write("SEGMENT NOT PRESENT\n");
    kernel_log.write("  EC=");
    kernel_log.writeHexRaw(error_code);
    kernel_log.write("\n");
    halt.haltLoop();
}

pub export fn stackSegmentFaultHandlerCommon(error_code: u64) callconv(.winapi) noreturn {
    asm volatile ("cli");
    kernel_log.write("STACK SEGMENT FAULT\n");
    kernel_log.write("  EC=");
    kernel_log.writeHexRaw(error_code);
    kernel_log.write("\n");
    halt.haltLoop();
}

pub export fn invalidOpcodeHandlerCommon(frame: *const TrapFrame) callconv(.winapi) noreturn {
    asm volatile ("cli");
    writeTrapSummary("INVALID OPCODE", frame);
    halt.haltLoop();
}

pub export fn divideErrorHandlerCommon(frame: *const TrapFrame) callconv(.winapi) noreturn {
    asm volatile ("cli");
    writeTrapSummary("DIVIDE ERROR", frame);
    halt.haltLoop();
}

pub export fn fatalUserTrapDispatch(vec: u64, frame: *const TrapFrame) callconv(.winapi) void {
    asm volatile ("cli");
    const label = switch (vec) {
        0 => "DIVIDE ERROR",
        6 => "INVALID OPCODE",
        else => "TRAP",
    };
    writeTrapSummary(label, frame);
    kernel_log.write("  ACTION=terminate process\n");
    resumeAfterFatalUserException(
        scheduler.currentPrincipal(),
        @intCast(vec),
        fatalExceptionResumeWorkFrameForCurrentCpuFromAsm(),
    );
}

const NativeSignalFrame = extern struct {
    magic: u64,
    size: u64,
    signo: u64,
    reserved0: u64,
    context: TrapFrame,
    fx_state: [process_abi.signal_fx_state_size]u8,
};

comptime {
    if (@offsetOf(NativeSignalFrame, "context") != process_abi.signal_frame_context_offset) @compileError("native signal frame context offset mismatch");
    if (@offsetOf(NativeSignalFrame, "fx_state") != process_abi.signal_frame_fx_state_offset) @compileError("native signal frame fx state offset mismatch");
    if (@sizeOf(NativeSignalFrame) != process_abi.signal_frame_size) @compileError("native signal frame size mismatch");
}

fn stagePendingSignalForUserReturn(frame: *TrapFrame) void {
    const claimed = scheduler.claimCurrentSignalForUserReturn(frame.rip) orelse return;
    const stack_cost = process_abi.signal_red_zone_size +
        process_abi.signal_frame_size + process_abi.signal_runtime_stack_size;
    if (frame.rsp <= stack_cost) {
        scheduler.restoreClaimedSignal(claimed);
        return;
    }
    const signal_frame_va = (frame.rsp - process_abi.signal_red_zone_size -
        process_abi.signal_frame_size) & ~@as(u64, 15);
    var signal_frame = NativeSignalFrame{
        .magic = process_abi.signal_frame_magic,
        .size = process_abi.signal_frame_size,
        .signo = claimed.signo,
        .reserved0 = 0,
        .context = frame.*,
        .fx_state = undefined,
    };
    if (!scheduler.copyCurrentSignalFxState(&signal_frame.fx_state)) {
        scheduler.restoreClaimedSignal(claimed);
        return;
    }
    if (!user_copy.copyBytesToUserVa(
        scheduler.currentPrincipal(),
        signal_frame_va,
        std.mem.asBytes(&signal_frame),
    )) {
        scheduler.restoreClaimedSignal(claimed);
        return;
    }
    frame.rdi = signal_frame_va;
    frame.rip = claimed.entry;
    frame.rsp = signal_frame_va - process_abi.signal_runtime_stack_size;
}

pub export fn timerInterruptDispatch(frame: *TrapFrame) callconv(.winapi) void {
    lapic.eoiLegacyPicMaster();
    lapic.eoi();
    const user_mode = ((frame.cs & 0x3) == 0x3) and ((frame.ss & 0x3) == 0x3);
    if (!scheduler.isBootstrapSchedulerCpu()) {
        if (user_mode) {
            if (!scheduler.apUserThreadCanContinue()) {
                smp.returnCurrentApToIdleFromInterrupt();
            }
            if (scheduler.preemptApUserThread(boot_static.scheduler_quantum_ticks, frame)) {
                smp.returnCurrentApToIdleFromInterrupt();
            }
            stagePendingSignalForUserReturn(frame);
            return;
        }
        // AP timer interrupts can arrive while a user thread is executing a
        // syscall or another kernel path. That CPU is not idle; keep its
        // current thread/cr3 intact and return to the interrupted kernel frame.
        return;
    }
    scheduler.lapic_tick_count +%= 1;
    if (!kernel_runtime.kernel_state_ready) return;
    scheduler.wakeExpiredTimers(scheduler.lapic_tick_count);
    if (!user_mode) return;
    if (boot_static.scheduler_quantum_ticks != 0) {
        _ = scheduler.preemptBootstrapThread(boot_static.scheduler_quantum_ticks, frame);
    }
    stagePendingSignalForUserReturn(frame);
}

pub export fn deviceInterruptDispatch(frame: *TrapFrame) callconv(.winapi) void {
    const active_vector = lapic.activeInterruptVectorInRange(
        generic_device_interrupt_vector,
        device_interrupt_vector_count,
    ) orelse generic_device_interrupt_vector;
    lapic.eoi();
    _ = frame;
    if (!kernel_runtime.kernel_state_ready) return;
    var wake_owners: [16]kernel.PrincipalId = undefined;
    const wake_count = kernel_runtime.kernel_state_global.recordDeviceInterruptEvent(active_vector, wake_owners[0..]);
    var i: usize = 0;
    while (i < wake_count) : (i += 1) {
        scheduler.wakeMailboxWaiter(wake_owners[i]);
    }
}

pub export fn schedulerMaintenanceIpiDispatch(frame: *TrapFrame) callconv(.winapi) void {
    _ = frame;
    user_copy.acknowledgePendingTlbShootdown();
    lapic.eoi();
}

pub export fn schedulerWakeIpiDispatch(frame: *TrapFrame) callconv(.winapi) void {
    schedulerMaintenanceIpiDispatch(frame);
    if (!scheduler.quiesceStoppedCurrentUserThread(frame)) return;
    if (!scheduler.isBootstrapSchedulerCpu()) {
        smp.returnCurrentApToIdleFromInterrupt();
    }
    while (!scheduler.loadNextReadyThread(frame)) {
        asm volatile ("sti; hlt; cli" ::: .{ .memory = true });
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

pub export fn timerInterruptHandlerStub() callconv(.naked) noreturn {
    if (debug_skip_timer_fx_state) {
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
            \\mov 128(%rsp), %rax
            \\and $0x3, %rax
            \\cmp $0x3, %rax
            \\jne 9f
        ++ asmCopyUserInterruptFrameToCpuWorkFrame("timer_interrupt_work_frames") ++
            \\mov (%rsp), %r12
            \\mov %r12, %rcx
        ++ asmCallAligned("timerInterruptDispatch") ++
            \\mov (%rsp), %r12
            \\add $8, %rsp
            \\mov %r12, %rax
        ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++
            \\jmp userReturnToSavedFrame
            \\9:
        ++ asmDispatchKernelInterruptPreservingFx("timerInterruptDispatch") ++
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
            \\iretq
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
            \\mov 128(%rsp), %rax
            \\and $0x3, %rax
            \\cmp $0x3, %rax
            \\jne 9f
        ++ asmCopyUserInterruptFrameToCpuWorkFrame("timer_interrupt_work_frames") ++
            asmCallAligned("saveCurrentThreadFxState") ++
            \\mov (%rsp), %r12
            \\mov %r12, %rcx
        ++ asmCallAligned("timerInterruptDispatch") ++ asmCallAligned("restoreCurrentThreadFxState") ++
            \\mov (%rsp), %r12
            \\add $8, %rsp
            \\mov %r12, %rax
        ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++
            \\jmp userReturnToSavedFrame
            \\9:
        ++ asmDispatchKernelInterruptPreservingFx("timerInterruptDispatch") ++
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
            \\iretq
        );
    }
}

pub export fn deviceInterruptHandlerStub() callconv(.naked) noreturn {
    if (debug_skip_timer_fx_state) {
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
            \\mov 128(%rsp), %rax
            \\and $0x3, %rax
            \\cmp $0x3, %rax
            \\jne 9f
        ++ asmCopyUserInterruptFrameToCpuWorkFrame("timer_interrupt_work_frames") ++
            \\mov (%rsp), %r12
            \\mov %r12, %rcx
        ++ asmCallAligned("deviceInterruptDispatch") ++
            \\mov (%rsp), %r12
            \\add $8, %rsp
            \\mov %r12, %rax
        ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++
            \\jmp userReturnToSavedFrame
            \\9:
        ++ asmDispatchKernelInterruptPreservingFx("deviceInterruptDispatch") ++
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
            \\iretq
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
            \\mov 128(%rsp), %rax
            \\and $0x3, %rax
            \\cmp $0x3, %rax
            \\jne 9f
        ++ asmCopyUserInterruptFrameToCpuWorkFrame("timer_interrupt_work_frames") ++
            asmCallAligned("saveCurrentThreadFxState") ++
            \\mov (%rsp), %r12
            \\mov %r12, %rcx
        ++ asmCallAligned("deviceInterruptDispatch") ++ asmCallAligned("restoreCurrentThreadFxState") ++
            \\mov (%rsp), %r12
            \\add $8, %rsp
            \\mov %r12, %rax
        ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++
            \\jmp userReturnToSavedFrame
            \\9:
        ++ asmDispatchKernelInterruptPreservingFx("deviceInterruptDispatch") ++
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
            \\iretq
        );
    }
}

pub export fn schedulerWakeIpiHandlerStub() callconv(.naked) noreturn {
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
        \\mov 128(%rsp), %rax
        \\and $0x3, %rax
        \\cmp $0x3, %rax
        \\jne 9f
    ++ asmCopyUserInterruptFrameToCpuWorkFrame("timer_interrupt_work_frames") ++
        asmCallAligned("saveCurrentThreadFxState") ++
        \\mov (%rsp), %r12
        \\mov %r12, %rcx
    ++ asmCallAligned("schedulerWakeIpiDispatch") ++ asmCallAligned("restoreCurrentThreadFxState") ++
        \\mov (%rsp), %r12
        \\add $8, %rsp
        \\mov %r12, %rax
    ++ asmStageUserReturnFromWorkFramePointer(trap_frame_iret_offset) ++
        \\jmp userReturnToSavedFrame
        \\9:
    ++ asmDispatchKernelInterruptPreservingFx("schedulerMaintenanceIpiDispatch") ++
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
