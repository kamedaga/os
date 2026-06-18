const std = @import("std");
const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const scheduler = @import("../scheduler.zig");
const user_vm = @import("../memory/user_vm.zig");
const boot_static = @import("../boot/main_static.zig");
const sc = @import("numbers.zig");

const fd_abi = abi_root.fd_abi;
const process_abi = abi_root.process_abi;
const TrapFrame = interrupts.TrapFrame;
const first_dynamic_fd: kernel.Fd = fd_abi.first_dynamic_fd;

fn protFromBits(bits: u64) ?kernel.VmaProt {
    if ((bits & ~(fd_abi.prot_read | fd_abi.prot_write | fd_abi.prot_exec)) != 0) return null;
    return .{
        .read = (bits & fd_abi.prot_read) != 0,
        .write = (bits & fd_abi.prot_write) != 0,
        .exec = (bits & fd_abi.prot_exec) != 0,
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

fn stateWord(state: kernel.TaskObjectState) u64 {
    return switch (state) {
        .active => process_abi.state_active,
        .exited => process_abi.state_exited,
        .killed => process_abi.state_killed,
    };
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

fn threadObjectIsLive(thread: kernel.ThreadObject) bool {
    const index: usize = @intCast(thread.thread_index);
    const owner: kernel.PrincipalId = @enumFromInt(thread.owner_principal_raw);
    const generation = scheduler.threadGeneration(index) orelse return false;
    return generation == thread.thread_generation and scheduler.threadBelongsToPrincipal(index, owner);
}

fn cleanupProcess(h: anytype, state: *kernel.KernelState, principal: kernel.PrincipalId, exit_state: kernel.TaskObjectState, exit_code: u32) void {
    const process_index = kernel.processIndexFromPrincipal(principal) orelse return;
    state.markThreadObjectsExitedForPrincipal(principal, exit_state, exit_code);
    state.markProcessObjectsExited(principal, exit_state, exit_code);
    _ = scheduler.releaseThreadsForPrincipal(principal);
    user_vm.clearUserAddressSpace(principal);
    state.releasePrincipalNativeMemory(principal, h.free_list);
    state.resetProcessRuntimeTables(process_index);
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
    const process = state.processObjectForFd(proc, @intCast(frame.rdi), .{ .spawn = true, .set_context = true }) orelse return sc.syscall_err_invalid;
    if (process.state != .active) return sc.syscall_err_invalid;
    const owner: kernel.PrincipalId = @enumFromInt(process.principal_raw);
    if (!state.hasActivePrincipal(owner)) return sc.syscall_err_invalid;
    const thread_index = scheduler.allocateSuspendedThreadSlot(owner, h.user_spaces, buildUserTrapFrame(frame.rsi, frame.rdx), h.free_list) orelse return sc.syscall_err_alloc;
    if (!scheduler.setThreadFsBase(thread_index, frame.r8)) {
        _ = scheduler.releaseThreadSlot(thread_index);
        return sc.syscall_err_invalid;
    }
    const generation = scheduler.threadGeneration(thread_index) orelse {
        _ = scheduler.releaseThreadSlot(thread_index);
        return sc.syscall_err_invalid;
    };
    const rights = kernel.fdRightsFromBits(frame.r9);
    return state.createThreadFd(proc, .{
        .owner_principal_raw = process.principal_raw,
        .thread_index = @intCast(thread_index),
        .thread_generation = generation,
        .state = .active,
        .exit_code = 0,
    }, rights, .{}, first_dynamic_fd) catch |err| {
        _ = scheduler.releaseThreadSlot(thread_index);
        return switch (err) {
            kernel.KernelError.TableFull => sc.syscall_err_alloc,
            else => sc.syscall_err_invalid,
        };
    };
}

fn startThread(state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd) u64 {
    const thread = state.threadObjectForFd(proc, fd, .{ .start = true }) orelse return sc.syscall_err_invalid;
    if (thread.state != .active or !threadObjectIsLive(thread)) return sc.syscall_err_invalid;
    if (!scheduler.setThreadReady(@intCast(thread.thread_index), true)) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn killThread(state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, code: u32) u64 {
    const thread = state.setThreadObjectStateForFd(proc, fd, .{ .kill = true }, .killed, code) catch return sc.syscall_err_invalid;
    if (threadObjectIsLive(thread)) {
        _ = scheduler.releaseThreadSlot(@intCast(thread.thread_index));
    }
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

fn mapIntoProcess(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    free_list: *kernel.FreePageList,
    frame: *TrapFrame,
) u64 {
    const process_fd: kernel.Fd = @intCast(frame.rdi);
    const vmo_fd: kernel.Fd = @intCast(frame.rsi);
    const target_va = frame.rdx;
    const size_bytes = frame.r10;
    const prot_bits = frame.r8;
    const vmo_offset = frame.r9;
    if (target_va == 0 or (target_va & 0xFFF) != 0 or (vmo_offset & 0xFFF) != 0) return sc.syscall_err_invalid;
    if (size_bytes == 0) return sc.syscall_err_invalid;
    const aligned_size = pageAlignUp(size_bytes) orelse return sc.syscall_err_invalid;
    if (aligned_size / 4096 > kernel.max_vmo_backing_pages) return sc.syscall_err_invalid;
    const prot = protFromBits(prot_bits) orelse return sc.syscall_err_invalid;
    const flags: kernel.MmapFlags = .{ .fixed = true, .shared = true };

    const process = state.processObjectForFd(proc, process_fd, .{ .map_into = true }) orelse return sc.syscall_err_invalid;
    if (process.state != .active) return sc.syscall_err_invalid;
    const target_owner: kernel.PrincipalId = @enumFromInt(process.principal_raw);
    if (!state.hasActivePrincipal(target_owner)) return sc.syscall_err_invalid;

    _ = state.mmapFdIntoProcess(proc, vmo_fd, target_owner, target_va, aligned_size, prot, flags, vmo_offset) catch |err| switch (err) {
        kernel.KernelError.TableFull => return sc.syscall_err_alloc,
        else => return sc.syscall_err_invalid,
    };

    var paddrs: [kernel.max_vmo_backing_pages]u64 = undefined;
    const page_count: usize = @intCast(aligned_size / 4096);
    const offset_pages: usize = @intCast(vmo_offset / 4096);
    const vmo_ref = state.nativeVmoRefForFd(proc, vmo_fd) orelse {
        state.munmapExactWithFreeList(target_owner, target_va, aligned_size, free_list) catch {};
        return sc.syscall_err_invalid;
    };
    var i: usize = 0;
    while (i < page_count) : (i += 1) {
        paddrs[i] = state.nativeVmoPagePaddr(vmo_ref, offset_pages + i) orelse {
            state.munmapExactWithFreeList(target_owner, target_va, aligned_size, free_list) catch {};
            return sc.syscall_err_map;
        };
    }
    if (!prot.read and !prot.write and !prot.exec) return sc.syscall_ok;
    if (!user_vm.mapTrustedUserPaddrsWithProt(target_owner, target_va, paddrs[0..page_count], .{
        .read = prot.read,
        .write = prot.write,
        .exec = prot.exec,
        .pkey = prot.pkey,
    })) {
        state.munmapExactWithFreeList(target_owner, target_va, aligned_size, free_list) catch {};
        return sc.syscall_err_map;
    }
    return sc.syscall_ok;
}

fn exitCurrentThread(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame, code: u32) u64 {
    const current = scheduler.currentThreadIndex();
    const generation = scheduler.threadGeneration(current) orelse {
        h.exit_current_process(proc, @truncate(code), frame);
        return sc.syscall_ok;
    };
    state.markThreadObjectsExitedBySlot(current, generation, .exited, code);
    if (scheduler.liveThreadCountForPrincipal(proc) <= 1) {
        state.markProcessObjectsExited(proc, .exited, code);
        h.exit_current_process(proc, @truncate(code), frame);
        return sc.syscall_ok;
    }
    const next = scheduler.nextReadyThreadForPrincipalAfter(proc, current) orelse {
        state.markProcessObjectsExited(proc, .exited, code);
        h.exit_current_process(proc, @truncate(code), frame);
        return sc.syscall_ok;
    };
    _ = scheduler.releaseThreadSlot(current);
    if (!scheduler.activateThread(next) or !scheduler.loadThreadContextToFrame(next, frame)) {
        h.exit_current_process(proc, @truncate(code), frame);
    }
    return sc.syscall_ok;
}

pub fn dispatch(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    return switch (frame.rax) {
        sc.syscall_process_create => createProcess(h, state, proc, frame),
        sc.syscall_process_kill => blk: {
            const process = state.setProcessObjectStateForFd(proc, @intCast(frame.rdi), .{ .kill = true }, .killed, @truncate(frame.rsi)) catch break :blk sc.syscall_err_invalid;
            const target: kernel.PrincipalId = @enumFromInt(process.principal_raw);
            if (target == proc) {
                h.exit_current_process(proc, @truncate(frame.rsi), frame);
            } else {
                cleanupProcess(h, state, target, .killed, @truncate(frame.rsi));
            }
            break :blk sc.syscall_ok;
        },
        sc.syscall_process_wait => waitProcess(h, state, proc, @intCast(frame.rdi), frame.rsi),
        sc.syscall_process_exit => blk: {
            state.markProcessObjectsExited(proc, .exited, @truncate(frame.rdi));
            state.markThreadObjectsExitedForPrincipal(proc, .exited, @truncate(frame.rdi));
            h.exit_current_process(proc, @truncate(frame.rdi), frame);
            break :blk sc.syscall_ok;
        },
        sc.syscall_thread_create => createThread(h, state, proc, frame),
        sc.syscall_thread_start => startThread(state, proc, @intCast(frame.rdi)),
        sc.syscall_thread_kill => killThread(state, proc, @intCast(frame.rdi), @truncate(frame.rsi)),
        sc.syscall_thread_wait => waitThread(h, state, proc, @intCast(frame.rdi), frame.rsi),
        sc.syscall_thread_exit => exitCurrentThread(h, state, proc, frame, @truncate(frame.rdi)),
        sc.syscall_thread_set_fs_base => blk: {
            const fs_base = frame.rdi;
            if (fs_base != 0 and !user_vm.isUserCanonicalVa(fs_base)) break :blk sc.syscall_err_invalid;
            if (!scheduler.setCurrentThreadFsBase(fs_base)) break :blk sc.syscall_err_not_ready;
            break :blk sc.syscall_ok;
        },
        sc.syscall_thread_set_gs_base => blk: {
            const gs_base = frame.rdi;
            if (gs_base != 0 and !user_vm.isUserCanonicalVa(gs_base)) break :blk sc.syscall_err_invalid;
            if (!scheduler.setCurrentThreadGsBase(gs_base)) break :blk sc.syscall_err_not_ready;
            break :blk sc.syscall_ok;
        },
        sc.syscall_process_map => mapIntoProcess(state, proc, h.free_list, frame),
        else => null,
    };
}
