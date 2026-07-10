const std = @import("std");
const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const scheduler = @import("../scheduler.zig").connection;
const user_vm = @import("../memory/user_vm.zig");
const boot_static = @import("../boot/main_static.zig");
const kernel_log = @import("../kernel_log.zig");
const sc = @import("numbers.zig");
const runtime = @import("runtime.zig");

const fd_abi = abi_root.fd_abi;
const process_abi = abi_root.process_abi;
const vm_abi = abi_root.vm_abi;
const TrapFrame = interrupts.TrapFrame;
const first_dynamic_fd: kernel.Fd = fd_abi.first_dynamic_fd;
const max_process_map_batch_entries: usize = @intCast(process_abi.process_map_batch_max_entries);
const process_map_batch_entry_size: usize = @intCast(process_abi.process_map_batch_entry_size);
const max_pipe_close_wakes = kernel.fd_table_entries;

fn ThreadExitClearContext(comptime Handler: type) type {
    return struct {
        handler: Handler,
        proc: kernel.PrincipalId,
        user_va: u64,

        fn run(raw_context: *anyopaque) void {
            const context: *@This() = @ptrCast(@alignCast(raw_context));
            runtime.clearTidAndWake(context.handler, context.proc, context.user_va);
        }
    };
}

const PendingPipeCloseWake = struct {
    pipe: kernel.PipeRef,
    wake_side_write: bool,
};

const ProcessCloneUserFrame = extern struct {
    r15: u64,
    r14: u64,
    r13: u64,
    r12: u64,
    rbp: u64,
    rbx: u64,
    r11: u64,
    r10: u64,
    r9: u64,
    r8: u64,
    rdi: u64,
    rsi: u64,
    rdx: u64,
    rcx: u64,
    rax: u64,
    rip: u64,
    rsp: u64,
    rflags: u64,
};

comptime {
    std.debug.assert(@sizeOf(ProcessCloneUserFrame) == process_abi.process_clone_user_frame_size);
}

fn protFromBits(bits: u64) ?kernel.VmaProt {
    if ((bits & ~(vm_abi.prot_read | vm_abi.prot_write | vm_abi.prot_exec)) != 0) return null;
    return .{
        .read = (bits & vm_abi.prot_read) != 0,
        .write = (bits & vm_abi.prot_write) != 0,
        .exec = (bits & vm_abi.prot_exec) != 0,
    };
}

fn pageAlignUp(value: u64) ?u64 {
    if (value > std.math.maxInt(u64) - 4095) return null;
    return (value + 4095) & ~@as(u64, 4095);
}

fn isUserEntryVa(va: u64) bool {
    return va != 0 and user_vm.isUserCanonicalVa(va);
}

fn buildUserTrapFrame(entry_rip: u64, stack_rsp: u64) TrapFrame {
    var frame = std.mem.zeroes(TrapFrame);
    frame.rip = entry_rip;
    frame.cs = @as(u64, boot_static.gdt_user_code_selector) | 0x3;
    frame.rflags = boot_static.user_entry_rflags;
    frame.rsp = stack_rsp;
    frame.ss = @as(u64, boot_static.gdt_user_data_selector) | 0x3;
    return frame;
}

fn cloneUserFrameFromVa(h: anytype, proc: kernel.PrincipalId, frame_va: u64, syscall_frame: *const TrapFrame) ?TrapFrame {
    if (frame_va == 0 or !user_vm.isUserCanonicalVa(frame_va)) return null;
    var user_frame: ProcessCloneUserFrame = undefined;
    const bytes = std.mem.asBytes(&user_frame);
    if (!h.copy_user_bytes_from_va(proc, frame_va, bytes)) return null;
    if (!isUserEntryVa(user_frame.rip) or !isUserEntryVa(user_frame.rsp)) return null;

    var child = syscall_frame.*;
    child.r15 = user_frame.r15;
    child.r14 = user_frame.r14;
    child.r13 = user_frame.r13;
    child.r12 = user_frame.r12;
    child.rbp = user_frame.rbp;
    child.rbx = user_frame.rbx;
    child.r11 = user_frame.r11;
    child.r10 = user_frame.r10;
    child.r9 = user_frame.r9;
    child.r8 = user_frame.r8;
    child.rdi = user_frame.rdi;
    child.rsi = user_frame.rsi;
    child.rdx = user_frame.rdx;
    child.rcx = user_frame.rcx;
    child.rax = 0;
    child.rip = user_frame.rip;
    child.rsp = user_frame.rsp;
    child.rflags = (user_frame.rflags & 0x0000000000200ed5) | 0x2;
    child.cs = @as(u64, boot_static.gdt_user_code_selector) | 0x3;
    child.ss = @as(u64, boot_static.gdt_user_data_selector) | 0x3;
    return child;
}

fn writeProtectForkCowVmas(state: *kernel.KernelState, from: kernel.PrincipalId) u64 {
    const source_table = state.getVmaTable(from) orelse return sc.syscall_err_invalid;
    var active_index: usize = 0;
    while (active_index < source_table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(source_table.active_indices[active_index]);
        const source_entry = &source_table.entries[entry_index];
        if (!source_entry.active) continue;
        if (!source_entry.flags.fork_cow) continue;
        if (!user_vm.writeProtectPresentUserPagesForForkCow(from, source_entry.start_va, source_entry.size_bytes)) {
            return sc.syscall_err_map;
        }
    }
    return sc.syscall_ok;
}

fn stateWord(state: kernel.TaskObjectState) u64 {
    return switch (state) {
        .active => process_abi.state_active,
        .exited => process_abi.state_exited,
        .killed => process_abi.state_killed,
        .stopped => process_abi.state_stopped,
        .continued => process_abi.state_continued,
    };
}

fn processRunnable(state: kernel.TaskObjectState) bool {
    return state == .active or state == .continued;
}

fn writeProcessStatus(h: anytype, proc: kernel.PrincipalId, out_va: u64, process: kernel.ProcessObject) u64 {
    if (out_va == 0) return sc.syscall_ok;
    if (!h.write_user_u64(proc, out_va + process_abi.status_word_state_offset, stateWord(process.state))) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + process_abi.status_word_exit_code_offset, process.exit_code)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + process_abi.status_word_id_offset, process.principal_raw)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + process_abi.status_word_generation_offset, 0)) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn writeThreadStatus(h: anytype, proc: kernel.PrincipalId, out_va: u64, thread: kernel.ThreadObject) u64 {
    if (out_va == 0) return sc.syscall_ok;
    if (!h.write_user_u64(proc, out_va + process_abi.status_word_state_offset, stateWord(thread.state))) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + process_abi.status_word_exit_code_offset, thread.exit_code)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + process_abi.status_word_id_offset, thread.thread_index)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + process_abi.status_word_generation_offset, thread.thread_generation)) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn wakeTaskFdWaiters(h: anytype, state: *kernel.KernelState, principal: kernel.PrincipalId) ?kernel.ThreadWakeTarget {
    var wake_targets: [kernel.max_task_fd_waiters]kernel.ThreadWakeTarget = undefined;
    const wake_count = state.takeTaskReadableWaitersForPrincipal(principal, wake_targets[0..]);
    var handoff_target: ?kernel.ThreadWakeTarget = null;
    for (wake_targets[0..wake_count]) |target| {
        if (target.pollfd_va != 0) {
            _ = h.write_user_u64(target.owner, target.pollfd_va + fd_abi.pollfd_revents_offset, target.revents);
        }
        const woke = h.wake_waiting_thread_generation_with_rax(target.thread_index, target.thread_generation, 1);
        if (woke and handoff_target == null) {
            handoff_target = target;
        }
    }
    return handoff_target;
}

fn wakeThreadTargets(h: anytype, targets: []const kernel.ThreadWakeTarget) void {
    for (targets) |target| {
        if (target.pollfd_va != 0) {
            _ = h.write_user_u64(target.owner, target.pollfd_va + fd_abi.pollfd_revents_offset, target.revents);
        }
        _ = h.wake_waiting_thread_generation_with_rax(target.thread_index, target.thread_generation, 1);
    }
}

fn collectPipeCloseWakesForProcess(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    cloexec_only: bool,
    out: []PendingPipeCloseWake,
) usize {
    const process_index = kernel.processIndexFromPrincipal(principal) orelse return 0;
    const table = state.fdTableForProcessIndexConst(process_index) orelse return 0;
    var count: usize = 0;
    for (table.entries[0..]) |entry| {
        if (entry.object.isNull()) continue;
        if (cloexec_only and !entry.flags.cloexec) continue;
        const slot = state.kernelObjectSlotConst(entry.object) orelse continue;
        const endpoint = kernel.KernelState.pipeEndpointFromPayload(&slot.payload) orelse continue;
        if (count >= out.len) break;
        out[count] = .{
            .pipe = endpoint.pipe,
            .wake_side_write = !endpoint.write,
        };
        count += 1;
    }
    return count;
}

fn wakeReadyPipeCloseWaiters(
    h: anytype,
    state: *kernel.KernelState,
    pending_wakes: []const PendingPipeCloseWake,
) void {
    var wake_storage: [@as(usize, @intCast(fd_abi.max_pollfds))]kernel.ThreadWakeTarget = undefined;
    for (pending_wakes) |pending| {
        const ready_events = state.pipeReadyEventsForSide(pending.pipe, pending.wake_side_write) orelse continue;
        if (ready_events == 0) continue;
        const wake_count = state.takePipeWaiters(pending.pipe, pending.wake_side_write, ready_events, wake_storage[0..]);
        wakeThreadTargets(h, wake_storage[0..wake_count]);
    }
}

fn closeCloexecFdsWithPipeWakes(
    h: anytype,
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
) kernel.KernelError!void {
    var pending_wakes: [max_pipe_close_wakes]PendingPipeCloseWake = undefined;
    const pending_count = collectPipeCloseWakesForProcess(state, principal, true, pending_wakes[0..]);
    try state.closeCloexecFdsWithFreeList(principal, h.free_list);
    wakeReadyPipeCloseWaiters(h, state, pending_wakes[0..pending_count]);
}

fn threadObjectIsLive(thread: kernel.ThreadObject) bool {
    const index: usize = @intCast(thread.thread_index);
    const owner: kernel.PrincipalId = @enumFromInt(thread.owner_principal_raw);
    const generation = scheduler.generationOfThread(index) orelse return false;
    return generation == thread.thread_generation and scheduler.threadOwnedBy(index, owner);
}

fn cleanupProcess(h: anytype, state: *kernel.KernelState, principal: kernel.PrincipalId, exit_state: kernel.TaskObjectState, exit_code: u32) void {
    const process_index = kernel.processIndexFromPrincipal(principal) orelse return;
    var pending_pipe_wakes: [max_pipe_close_wakes]PendingPipeCloseWake = undefined;
    const pending_pipe_wake_count = collectPipeCloseWakesForProcess(state, principal, false, pending_pipe_wakes[0..]);
    state.markThreadObjectsExitedForPrincipal(principal, exit_state, exit_code);
    state.markProcessObjectsExited(principal, exit_state, exit_code);
    _ = wakeTaskFdWaiters(h, state, principal);
    _ = scheduler.releasePrincipalThreads(principal);
    user_vm.lockAddressSpaces();
    user_vm.clearUserAddressSpace(principal);
    state.releasePrincipalNativeMemory(principal, h.free_list);
    state.resetProcessRuntimeTables(process_index);
    user_vm.unlockAddressSpaces();
    wakeReadyPipeCloseWaiters(h, state, pending_pipe_wakes[0..pending_pipe_wake_count]);
    _ = state.unpublishServiceEndpointsForTarget(principal);
    _ = state.markProcessExited(principal);
}

fn createProcess(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    if ((frame.rdi & ~process_abi.process_known_flags_mask) != 0) return sc.syscall_err_invalid;
    const principal = state.createProcessDescriptorWithCapacity("fd-process", h.free_list) orelse return sc.syscall_err_alloc;
    if (!user_vm.buildEmptyUserAddressSpace(principal)) {
        _ = state.removeProcessDescriptor(principal);
        return sc.syscall_err_map;
    }
    state.inheritFdsForProcessCreate(proc, principal) catch |err| {
        state.releasePrincipalNativeMemory(principal, h.free_list);
        _ = state.removeProcessDescriptor(principal);
        return switch (err) {
            kernel.KernelError.TableFull => sc.syscall_err_alloc,
            else => sc.syscall_err_invalid,
        };
    };
    const rights = kernel.fdRightsFromBits(frame.rsi);
    return state.createProcessFd(proc, .{
        .principal_raw = @intFromEnum(principal),
        .state = .active,
        .exit_code = 0,
    }, rights, kernel.fdFlagsFromBits(@truncate(frame.rdx)), first_dynamic_fd) catch |err| switch (err) {
        kernel.KernelError.TableFull => sc.syscall_err_alloc,
        else => sc.syscall_err_invalid,
    };
}

fn createThread(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    if ((frame.r10 & ~process_abi.thread_known_flags_mask) != 0) return sc.syscall_err_invalid;
    if (!isUserEntryVa(frame.rsi) or !isUserEntryVa(frame.rdx)) return sc.syscall_err_invalid;
    if (frame.r8 != 0 and !user_vm.isUserCanonicalVa(frame.r8)) return sc.syscall_err_invalid;
    const owner = if (frame.rdi == process_abi.process_self_fd)
        proc
    else blk: {
        const process = state.processObjectForFd(proc, @intCast(frame.rdi), .{ .spawn = true, .set_context = true }) orelse return sc.syscall_err_invalid;
        if (!processRunnable(process.state)) return sc.syscall_err_invalid;
        break :blk @as(kernel.PrincipalId, @enumFromInt(process.principal_raw));
    };
    if (!state.hasActivePrincipal(owner)) return sc.syscall_err_invalid;
    const thread_index = scheduler.allocateSuspendedThread(owner, h.user_spaces, buildUserTrapFrame(frame.rsi, frame.rdx), h.free_list) orelse return sc.syscall_err_alloc;
    if (!scheduler.setFsBase(thread_index, frame.r8)) {
        _ = scheduler.releaseThread(thread_index);
        return sc.syscall_err_invalid;
    }
    const generation = scheduler.generationOfThread(thread_index) orelse {
        _ = scheduler.releaseThread(thread_index);
        return sc.syscall_err_invalid;
    };
    const rights = kernel.fdRightsFromBits(frame.r9);
    return state.createThreadFd(proc, .{
        .owner_principal_raw = @intFromEnum(owner),
        .thread_index = @intCast(thread_index),
        .thread_generation = generation,
        .state = .active,
        .exit_code = 0,
    }, rights, .{}, first_dynamic_fd) catch |err| {
        _ = scheduler.releaseThread(thread_index);
        return switch (err) {
            kernel.KernelError.TableFull => sc.syscall_err_alloc,
            else => sc.syscall_err_invalid,
        };
    };
}

fn cloneCurrentProcessForFork(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const rights = kernel.fdRightsFromBits(frame.rdi);
    if ((frame.rsi & ~process_abi.process_clone_known_flags_mask) != 0) return sc.syscall_err_invalid;
    if ((frame.rsi & process_abi.process_clone_flag_current_thread) == 0) return sc.syscall_err_invalid;

    const child = state.createProcessDescriptorWithCapacity("fd-process-clone", h.free_list) orelse return sc.syscall_err_alloc;
    var child_active = true;
    defer if (child_active) {
        if (kernel.processIndexFromPrincipal(child)) |child_index| {
            state.releasePrincipalNativeMemory(child, h.free_list);
            state.resetProcessRuntimeTables(child_index);
        }
        user_vm.lockAddressSpaces();
        user_vm.clearUserAddressSpace(child);
        user_vm.unlockAddressSpaces();
        _ = state.removeProcessDescriptor(child);
    };

    if (!user_vm.buildEmptyUserAddressSpace(child)) return sc.syscall_err_map;
    state.cloneFdTableForFork(proc, child) catch |err| return switch (err) {
        kernel.KernelError.TableFull => sc.syscall_err_alloc,
        else => sc.syscall_err_invalid,
    };
    state.cloneVmaTableForFork(proc, child) catch |err| return switch (err) {
        kernel.KernelError.TableFull => sc.syscall_err_alloc,
        else => sc.syscall_err_invalid,
    };
    const protect_status = writeProtectForkCowVmas(state, proc);
    if (protect_status != sc.syscall_ok) return protect_status;
    if (!user_vm.cloneAddressSpaceMetadataForFork(proc, child)) return sc.syscall_err_map;

    const child_frame = if ((frame.rsi & process_abi.process_clone_flag_user_frame) != 0)
        cloneUserFrameFromVa(h, proc, frame.rdx, frame) orelse return sc.syscall_err_invalid
    else blk: {
        var copied = frame.*;
        copied.rax = 0;
        break :blk copied;
    };
    const thread_index = scheduler.allocateReadyThread(child, h.user_spaces, child_frame, h.free_list) orelse return sc.syscall_err_alloc;
    if (!scheduler.copySignalDeliveryConfig(scheduler.currentThread(), thread_index)) {
        _ = scheduler.releaseThread(thread_index);
        return sc.syscall_err_not_ready;
    }
    if (!scheduler.setFsBase(thread_index, scheduler.currentFsBase())) {
        _ = scheduler.releaseThread(thread_index);
        return sc.syscall_err_invalid;
    }
    if (!scheduler.setThreadGsBase(thread_index, scheduler.currentGsBase())) {
        _ = scheduler.releaseThread(thread_index);
        return sc.syscall_err_invalid;
    }

    const process_fd = state.createProcessFd(proc, .{
        .principal_raw = @intFromEnum(child),
        .state = .active,
        .exit_code = 0,
    }, rights, .{}, first_dynamic_fd) catch |err| switch (err) {
        kernel.KernelError.TableFull => return sc.syscall_err_alloc,
        else => return sc.syscall_err_invalid,
    };
    child_active = false;
    return process_fd;
}

fn startThread(state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd) u64 {
    const thread = state.threadObjectForFd(proc, fd, .{ .start = true }) orelse return sc.syscall_err_invalid;
    if (!processRunnable(thread.state) or !threadObjectIsLive(thread)) return sc.syscall_err_invalid;
    if (!scheduler.markThreadReady(@intCast(thread.thread_index), true)) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn killThread(state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, code: u32) u64 {
    const thread = state.setThreadObjectStateForFd(proc, fd, .{ .kill = true }, .killed, code) catch return sc.syscall_err_invalid;
    if (threadObjectIsLive(thread)) {
        _ = scheduler.releaseThread(@intCast(thread.thread_index));
    }
    return sc.syscall_ok;
}

fn killProcess(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, code: u32, frame: *TrapFrame) u64 {
    const process = state.setProcessObjectStateForFd(proc, fd, .{ .kill = true }, .killed, code) catch return sc.syscall_err_invalid;
    const target: kernel.PrincipalId = @enumFromInt(process.principal_raw);
    if (target == proc) {
        state.markProcessObjectsExited(proc, .killed, code);
        state.markThreadObjectsExitedForPrincipal(proc, .killed, code);
        const handoff_target = wakeTaskFdWaiters(h, state, proc);
        h.exit_current_process(proc, @truncate(code), frame, h.before_current_thread_leave, handoff_target);
        return frame.rax;
    }
    cleanupProcess(h, state, target, .killed, code);
    return sc.syscall_ok;
}

fn signalProcess(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, signo: u32, frame: *TrapFrame) u64 {
    if (signo == 0 or signo > process_abi.signal_max) return sc.syscall_err_invalid;
    if (signo == process_abi.signal_kill) return killProcess(h, state, proc, fd, signo, frame);
    const process = state.processObjectForFd(proc, fd, .{ .kill = true }) orelse return sc.syscall_err_invalid;
    if (!processRunnable(process.state)) return sc.syscall_err_invalid;
    const target: kernel.PrincipalId = @enumFromInt(process.principal_raw);
    if (!state.hasActivePrincipal(target)) return sc.syscall_err_invalid;
    if (!scheduler.deliverSignal(target, signo)) return sc.syscall_err_not_ready;
    return sc.syscall_ok;
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

fn validReturnedSignalContext(context: *const TrapFrame) bool {
    const user_cs = @as(u64, boot_static.gdt_user_code_selector) | 0x3;
    const user_ss = @as(u64, boot_static.gdt_user_data_selector) | 0x3;
    return context.cs == user_cs and context.ss == user_ss and
        isUserEntryVa(context.rip) and isUserEntryVa(context.rsp);
}

fn signalControl(h: anytype, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    switch (frame.rdi) {
        process_abi.signal_ctl_register => {
            const entry = frame.rsi;
            const inhibit_start = frame.rdx;
            const inhibit_end = frame.r10;
            if (!isUserEntryVa(entry) or !isUserEntryVa(inhibit_start) or
                !isUserEntryVa(inhibit_end) or inhibit_start >= inhibit_end or
                entry < inhibit_start or entry >= inhibit_end)
            {
                return sc.syscall_err_invalid;
            }
            return if (scheduler.configureCurrentSignalDelivery(entry, inhibit_start, inhibit_end)) sc.syscall_ok else sc.syscall_err_not_ready;
        },
        process_abi.signal_ctl_return => {
            var returned: NativeSignalFrame = undefined;
            if (!h.copy_user_bytes_from_va(proc, frame.rsi, std.mem.asBytes(&returned))) return sc.syscall_err_invalid;
            if (returned.magic != process_abi.signal_frame_magic or
                returned.size != process_abi.signal_frame_size or
                returned.signo == 0 or returned.signo > process_abi.signal_max or
                !validReturnedSignalContext(&returned.context))
            {
                return sc.syscall_err_invalid;
            }
            returned.context.rflags &= ~(@as(u64, 3) << 12);
            returned.context.rflags &= ~(@as(u64, 1) << 14);
            returned.context.rflags &= ~(@as(u64, 1) << 17);
            returned.context.rflags |= (@as(u64, 1) << 1) | (@as(u64, 1) << 9);
            if (!scheduler.restoreCurrentSignalFxState(&returned.fx_state)) return sc.syscall_err_not_ready;
            frame.* = returned.context;
            return returned.context.rax;
        },
        else => return sc.syscall_err_invalid,
    }
}

fn stopProcess(state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, code: u32) u64 {
    const process = state.setProcessObjectStateForFd(proc, fd, .{ .kill = true }, .stopped, code) catch return sc.syscall_err_invalid;
    const target: kernel.PrincipalId = @enumFromInt(process.principal_raw);
    if (!state.hasActivePrincipal(target)) return sc.syscall_err_invalid;
    state.markProcessObjectsExited(target, .stopped, code);
    state.markThreadObjectsExitedForPrincipal(target, .stopped, code);
    _ = scheduler.stopPrincipalThreads(target);
    return sc.syscall_ok;
}

fn continueProcess(state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, code: u32) u64 {
    const process = state.setProcessObjectStateForFd(proc, fd, .{ .kill = true }, .continued, code) catch return sc.syscall_err_invalid;
    const target: kernel.PrincipalId = @enumFromInt(process.principal_raw);
    if (!state.hasActivePrincipal(target)) return sc.syscall_err_invalid;
    state.markProcessObjectsExited(target, .continued, code);
    state.markThreadObjectsExitedForPrincipal(target, .continued, code);
    _ = scheduler.continuePrincipalThreads(target);
    return sc.syscall_ok;
}

fn waitProcess(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, out_va: u64) u64 {
    const process = state.processObjectForFd(proc, fd, .{ .wait = true }) orelse return sc.syscall_err_invalid;
    if (process.state == .active) return sc.syscall_err_not_ready;
    return writeProcessStatus(h, proc, out_va, process);
}

fn waitThread(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, out_va: u64) u64 {
    const thread = state.threadObjectForFd(proc, fd, .{ .wait = true }) orelse return sc.syscall_err_invalid;
    if (thread.state == .active) return sc.syscall_err_not_ready;
    return writeThreadStatus(h, proc, out_va, thread);
}

const ProcessMapRequest = struct {
    vmo_fd: kernel.Fd,
    target_va: u64,
    size_bytes: u64,
    aligned_size: u64,
    prot: kernel.VmaProt,
    flags: kernel.MmapFlags,
    vmo_offset: u64,
};

const ProcessMapPrepareResult = struct {
    status: u64,
    request: ProcessMapRequest = undefined,
};

const ProcessMapInstallResult = struct {
    status: u64,
    mapped_va: u64 = 0,
};

fn prepareProcessMapRequest(
    vmo_fd: kernel.Fd,
    target_va: u64,
    size_bytes: u64,
    prot_bits: u64,
    vmo_offset: u64,
    map_flags_bits: u64,
    allow_anywhere: bool,
) ProcessMapPrepareResult {
    const anywhere = target_va == process_abi.process_map_anywhere_va;
    if (anywhere and !allow_anywhere) return .{ .status = sc.syscall_err_invalid };
    if ((map_flags_bits & ~process_abi.process_map_known_flags_mask) != 0) return .{ .status = sc.syscall_err_invalid };
    if ((map_flags_bits & process_abi.process_map_flag_private) != 0 and
        (map_flags_bits & process_abi.process_map_flag_shared) != 0)
    {
        return .{ .status = sc.syscall_err_invalid };
    }
    if ((!anywhere and (target_va & 0xFFF) != 0) or (vmo_offset & 0xFFF) != 0) {
        kernel_log.write("process.map failed args\n");
        return .{ .status = sc.syscall_err_invalid };
    }
    if (size_bytes == 0) {
        kernel_log.write("process.map failed size\n");
        return .{ .status = sc.syscall_err_invalid };
    }
    const aligned_size = pageAlignUp(size_bytes) orelse {
        kernel_log.write("process.map failed align\n");
        return .{ .status = sc.syscall_err_invalid };
    };
    if (aligned_size / 4096 > kernel.max_vmo_backing_pages) {
        kernel_log.write("process.map failed too-large\n");
        return .{ .status = sc.syscall_err_alloc };
    }
    const prot = protFromBits(prot_bits) orelse {
        kernel_log.write("process.map failed prot\n");
        return .{ .status = sc.syscall_err_invalid };
    };
    const private_map = (map_flags_bits & process_abi.process_map_flag_private) != 0;
    return .{
        .status = sc.syscall_ok,
        .request = .{
            .vmo_fd = vmo_fd,
            .target_va = target_va,
            .size_bytes = size_bytes,
            .aligned_size = aligned_size,
            .prot = prot,
            .flags = .{
                .fixed = true,
                .private = private_map,
                .shared = !private_map,
            },
            .vmo_offset = vmo_offset,
        },
    };
}

fn installProcessMapLocked(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    free_list: *kernel.FreePageList,
    process_fd: kernel.Fd,
    target_owner: kernel.PrincipalId,
    req: ProcessMapRequest,
) ProcessMapInstallResult {
    var target_va = req.target_va;
    const anywhere = target_va == process_abi.process_map_anywhere_va;
    if (anywhere) {
        const purpose = 0x5052_4f43_4d41_5000 ^ scheduler.lapic_tick_count ^ (@as(u64, process_fd) << 32) ^ @as(u64, req.vmo_fd);
        target_va = state.findRandomizedFreeUserMapVa(target_owner, req.aligned_size, purpose) catch |err| switch (err) {
            kernel.KernelError.TableFull => {
                kernel_log.write("process.map failed aslr-full\n");
                return .{ .status = sc.syscall_err_alloc };
            },
            else => {
                kernel_log.write("process.map failed aslr\n");
                return .{ .status = sc.syscall_err_map };
            },
        };
    }

    _ = state.mmapFdIntoProcess(proc, req.vmo_fd, target_owner, target_va, req.aligned_size, req.prot, req.flags, req.vmo_offset) catch |err| switch (err) {
        kernel.KernelError.TableFull => {
            kernel_log.write("process.map failed vma-table-full\n");
            return .{ .status = sc.syscall_err_alloc };
        },
        else => {
            kernel_log.write("process.map failed vma-install\n");
            return .{ .status = sc.syscall_err_invalid };
        },
    };

    if (req.flags.private and !req.flags.shared) {
        return .{ .status = sc.syscall_ok, .mapped_va = target_va };
    }
    if (!req.prot.read and !req.prot.write and !req.prot.exec) return .{ .status = sc.syscall_ok, .mapped_va = target_va };

    var paddrs: [kernel.max_vmo_backing_pages]u64 = undefined;
    const page_count: usize = @intCast(req.aligned_size / 4096);
    var pte_prot: ?kernel.MapProt = null;
    var i: usize = 0;
    while (i < page_count) : (i += 1) {
        const mapping = state.nativeVmaInitialMapping(target_owner, target_va + @as(u64, @intCast(i)) * 4096) orelse {
            kernel_log.write("process.map failed missing-page\n");
            state.munmapRangeWithFreeList(target_owner, target_va, req.aligned_size, free_list) catch {};
            return .{ .status = sc.syscall_err_map };
        };
        paddrs[i] = mapping.paddr;
        if (pte_prot == null) pte_prot = mapping.prot;
    }
    const low_page_zero_map =
        !anywhere and
        target_va == 0 and
        req.aligned_size == 4096 and
        req.vmo_offset == 0 and
        req.prot.read and
        !req.prot.write and
        req.prot.exec;
    const initial_prot = pte_prot orelse kernel.MapProt{};
    const map_ok = if (low_page_zero_map)
        user_vm.mapTrustedLowPageZeroPaddrsWithProt(target_owner, target_va, paddrs[0..page_count], .{
            .read = initial_prot.read,
            .write = initial_prot.write,
            .exec = initial_prot.exec,
            .pkey = initial_prot.pkey,
        })
    else
        user_vm.mapTrustedUserPaddrsWithProt(target_owner, target_va, paddrs[0..page_count], initial_prot);
    if (!map_ok) {
        kernel_log.write("process.map failed map-paddrs\n");
        state.munmapRangeWithFreeList(target_owner, target_va, req.aligned_size, free_list) catch {};
        return .{ .status = sc.syscall_err_map };
    }
    return .{ .status = sc.syscall_ok, .mapped_va = target_va };
}

fn mapIntoProcess(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    free_list: *kernel.FreePageList,
    frame: *TrapFrame,
) u64 {
    const process_fd: kernel.Fd = @intCast(frame.rdi);
    const vmo_fd: kernel.Fd = @intCast(frame.rsi);
    const map_flags_bits = frame.r9 & process_abi.process_map_offset_low_bits;
    const vmo_offset = frame.r9 & ~process_abi.process_map_offset_low_bits;
    const prepared = prepareProcessMapRequest(vmo_fd, frame.rdx, frame.r10, frame.r8, vmo_offset, map_flags_bits, true);
    if (prepared.status != sc.syscall_ok) return prepared.status;

    const process = state.processObjectForFd(proc, process_fd, .{ .map_into = true }) orelse {
        kernel_log.write("process.map failed process-fd\n");
        return sc.syscall_err_invalid;
    };
    if (!processRunnable(process.state)) {
        kernel_log.write("process.map failed process-state\n");
        return sc.syscall_err_invalid;
    }
    const target_owner: kernel.PrincipalId = @enumFromInt(process.principal_raw);
    if (!state.hasActivePrincipal(target_owner)) {
        kernel_log.write("process.map failed inactive-target\n");
        return sc.syscall_err_invalid;
    }
    user_vm.lockAddressSpaces();
    defer user_vm.unlockAddressSpaces();
    const installed = installProcessMapLocked(state, proc, free_list, process_fd, target_owner, prepared.request);
    return if (installed.status == sc.syscall_ok) installed.mapped_va else installed.status;
}

fn readBatchEntryU64(bytes: []const u8, entry_index: usize, field_offset: u64) u64 {
    const offset = entry_index * process_map_batch_entry_size + @as(usize, @intCast(field_offset));
    return std.mem.readInt(u64, bytes[offset..][0..8], .little);
}

fn mapBatchIntoProcess(
    h: anytype,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    free_list: *kernel.FreePageList,
    frame: *TrapFrame,
) u64 {
    const process_fd: kernel.Fd = @intCast(frame.rdi);
    const entries_va = frame.rsi;
    const entry_count_u64 = frame.rdx;
    if (entries_va == 0 or entry_count_u64 == 0 or entry_count_u64 > process_abi.process_map_batch_max_entries) {
        return sc.syscall_err_invalid;
    }
    const entry_count: usize = @intCast(entry_count_u64);
    var bytes: [max_process_map_batch_entries * process_map_batch_entry_size]u8 = undefined;
    const bytes_len = entry_count * process_map_batch_entry_size;
    if (!h.copy_user_bytes_from_va(proc, entries_va, bytes[0..bytes_len])) return sc.syscall_err_invalid;

    var requests: [max_process_map_batch_entries]ProcessMapRequest = undefined;
    var i: usize = 0;
    while (i < entry_count) : (i += 1) {
        const vmo_fd_raw = readBatchEntryU64(bytes[0..bytes_len], i, process_abi.process_map_batch_entry_vmo_fd_offset);
        if (vmo_fd_raw > std.math.maxInt(kernel.Fd)) return sc.syscall_err_invalid;
        const prepared = prepareProcessMapRequest(
            @intCast(vmo_fd_raw),
            readBatchEntryU64(bytes[0..bytes_len], i, process_abi.process_map_batch_entry_target_va_offset),
            readBatchEntryU64(bytes[0..bytes_len], i, process_abi.process_map_batch_entry_size_offset),
            readBatchEntryU64(bytes[0..bytes_len], i, process_abi.process_map_batch_entry_prot_offset),
            readBatchEntryU64(bytes[0..bytes_len], i, process_abi.process_map_batch_entry_vmo_offset_offset),
            readBatchEntryU64(bytes[0..bytes_len], i, process_abi.process_map_batch_entry_flags_offset),
            false,
        );
        if (prepared.status != sc.syscall_ok) return prepared.status;
        requests[i] = prepared.request;
    }

    const process = state.processObjectForFd(proc, process_fd, .{ .map_into = true }) orelse return sc.syscall_err_invalid;
    if (!processRunnable(process.state)) return sc.syscall_err_invalid;
    const target_owner: kernel.PrincipalId = @enumFromInt(process.principal_raw);
    if (!state.hasActivePrincipal(target_owner)) return sc.syscall_err_invalid;

    user_vm.lockAddressSpaces();
    defer user_vm.unlockAddressSpaces();
    var mapped_vas: [max_process_map_batch_entries]u64 = undefined;
    var mapped_sizes: [max_process_map_batch_entries]u64 = undefined;
    var mapped_count: usize = 0;
    i = 0;
    while (i < entry_count) : (i += 1) {
        const installed = installProcessMapLocked(state, proc, free_list, process_fd, target_owner, requests[i]);
        if (installed.status != sc.syscall_ok) {
            while (mapped_count > 0) {
                mapped_count -= 1;
                state.munmapRangeWithFreeList(target_owner, mapped_vas[mapped_count], mapped_sizes[mapped_count], free_list) catch {};
            }
            return installed.status;
        }
        mapped_vas[mapped_count] = installed.mapped_va;
        mapped_sizes[mapped_count] = requests[i].aligned_size;
        mapped_count += 1;
    }
    return sc.syscall_ok;
}

fn execFromStagedProcess(
    h: anytype,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    frame: *TrapFrame,
) u64 {
    const process_fd: kernel.Fd = @intCast(frame.rdi);
    const thread_fd: kernel.Fd = @intCast(frame.rsi);
    const flags = frame.rdx;
    if ((flags & ~process_abi.process_exec_from_known_flags_mask) != 0) return sc.syscall_err_invalid;

    const process = state.processObjectForFd(proc, process_fd, .{ .kill = true, .set_context = true }) orelse return sc.syscall_err_invalid;
    if (!processRunnable(process.state)) return sc.syscall_err_invalid;
    const staged_owner: kernel.PrincipalId = @enumFromInt(process.principal_raw);
    if (staged_owner == proc or !state.hasActivePrincipal(staged_owner)) return sc.syscall_err_invalid;

    const thread = state.threadObjectForFd(proc, thread_fd, .{ .set_context = true }) orelse return sc.syscall_err_invalid;
    if (!processRunnable(thread.state) or !threadObjectIsLive(thread)) return sc.syscall_err_invalid;
    if (thread.owner_principal_raw != process.principal_raw) return sc.syscall_err_invalid;

    const image = scheduler.suspendedThreadImage(
        @intCast(thread.thread_index),
        thread.thread_generation,
        staged_owner,
    ) orelse return sc.syscall_err_invalid;

    if (!user_vm.cloneAddressSpaceMetadataForFork(staged_owner, proc)) return sc.syscall_err_map;
    state.replaceVmaTableForExec(proc, staged_owner, h.free_list) catch |err| return switch (err) {
        kernel.KernelError.TableFull => sc.syscall_err_alloc,
        else => sc.syscall_err_invalid,
    };

    cleanupProcess(h, state, staged_owner, .exited, 0);
    closeCloexecFdsWithPipeWakes(h, state, proc) catch return sc.syscall_err_invalid;
    if (!scheduler.installExecContextForCurrentThread(h.user_spaces, proc, image)) return sc.syscall_err_invalid;
    frame.* = image.frame;
    return sc.syscall_ok;
}

fn exitCurrentThread(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame, code: u32) u64 {
    const exit_flags = frame.rdx;
    if ((exit_flags & ~process_abi.thread_exit_known_flags_mask) != 0) return sc.syscall_err_invalid;
    const clear_tid = (exit_flags & process_abi.thread_exit_flag_clear_tid) != 0;
    const current = scheduler.currentThread();
    const generation = scheduler.generationOfThread(current) orelse {
        if (clear_tid) runtime.clearTidAndWake(h, proc, frame.rsi);
        state.markThreadObjectsExitedForPrincipal(proc, .exited, code);
        state.markProcessObjectsExited(proc, .exited, code);
        const handoff_target = wakeTaskFdWaiters(h, state, proc);
        h.exit_current_process(proc, @truncate(code), frame, h.before_current_thread_leave, handoff_target);
        return frame.rax;
    };
    state.markThreadObjectsExitedBySlot(current, generation, .exited, code);
    if (scheduler.liveThreadCount(proc) <= 1) {
        if (clear_tid) runtime.clearTidAndWake(h, proc, frame.rsi);
        state.markProcessObjectsExited(proc, .exited, code);
        const handoff_target = wakeTaskFdWaiters(h, state, proc);
        h.exit_current_process(proc, @truncate(code), frame, h.before_current_thread_leave, handoff_target);
        return frame.rax;
    }
    const ClearContext = ThreadExitClearContext(@TypeOf(h));
    var clear_context = ClearContext{ .handler = h, .proc = proc, .user_va = frame.rsi };
    const clear_callback: ?scheduler.BeforeCurrentThreadLeaveCallback = if (clear_tid) .{
        .context = @ptrCast(&clear_context),
        .run = ClearContext.run,
    } else null;
    if (!scheduler.exitCurrentThread(frame, sc.syscall_ok, h.before_current_thread_leave, clear_callback)) {
        h.exit_current_process(proc, @truncate(code), frame, h.before_current_thread_leave, null);
    }
    return frame.rax;
}

pub fn dispatch(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    return switch (frame.rax) {
        sc.syscall_process_create => createProcess(h, state, proc, frame),
        sc.syscall_process_kill => killProcess(h, state, proc, @intCast(frame.rdi), @truncate(frame.rsi), frame),
        sc.syscall_process_wait => waitProcess(h, state, proc, @intCast(frame.rdi), frame.rsi),
        sc.syscall_process_exit => blk: {
            state.markProcessObjectsExited(proc, .exited, @truncate(frame.rdi));
            state.markThreadObjectsExitedForPrincipal(proc, .exited, @truncate(frame.rdi));
            const handoff_target = wakeTaskFdWaiters(h, state, proc);
            h.exit_current_process(proc, @truncate(frame.rdi), frame, h.before_current_thread_leave, handoff_target);
            break :blk frame.rax;
        },
        sc.syscall_thread_create => createThread(h, state, proc, frame),
        sc.syscall_thread_start => startThread(state, proc, @intCast(frame.rdi)),
        sc.syscall_thread_kill => killThread(state, proc, @intCast(frame.rdi), @truncate(frame.rsi)),
        sc.syscall_thread_wait => waitThread(h, state, proc, @intCast(frame.rdi), frame.rsi),
        sc.syscall_thread_exit => exitCurrentThread(h, state, proc, frame, @truncate(frame.rdi)),
        sc.syscall_process_signal => signalProcess(h, state, proc, @intCast(frame.rdi), @truncate(frame.rsi), frame),
        sc.syscall_process_signal_ctl => signalControl(h, proc, frame),
        sc.syscall_process_stop => stopProcess(state, proc, @intCast(frame.rdi), @truncate(frame.rsi)),
        sc.syscall_process_continue => continueProcess(state, proc, @intCast(frame.rdi), @truncate(frame.rsi)),
        sc.syscall_thread_set_fs_base => blk: {
            const fs_base = frame.rdi;
            if (fs_base != 0 and !user_vm.isUserCanonicalVa(fs_base)) break :blk sc.syscall_err_invalid;
            if (!scheduler.setFsBase(scheduler.currentThread(), fs_base)) break :blk sc.syscall_err_not_ready;
            break :blk sc.syscall_ok;
        },
        sc.syscall_thread_set_gs_base => blk: {
            const gs_base = frame.rdi;
            if (gs_base != 0 and !user_vm.isUserCanonicalVa(gs_base)) break :blk sc.syscall_err_invalid;
            if (!scheduler.setCurrentGsBase(gs_base)) break :blk sc.syscall_err_not_ready;
            break :blk sc.syscall_ok;
        },
        sc.syscall_process_clone => cloneCurrentProcessForFork(h, state, proc, frame),
        sc.syscall_process_map => mapIntoProcess(state, proc, h.free_list, frame),
        sc.syscall_process_map_batch => mapBatchIntoProcess(h, state, proc, h.free_list, frame),
        sc.syscall_process_exec_from => execFromStagedProcess(h, state, proc, frame),
        else => null,
    };
}
