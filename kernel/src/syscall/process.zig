const std = @import("std");
const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const scheduler = @import("../scheduler.zig").connection;
const user_vm = @import("../memory/user_vm.zig");
const boot_static = @import("../boot/main_static.zig");
const kernel_log = @import("../kernel_log.zig");
const sc = @import("numbers.zig");

const fd_abi = abi_root.fd_abi;
const process_abi = abi_root.process_abi;
const vm_abi = abi_root.vm_abi;
const TrapFrame = interrupts.TrapFrame;
const first_dynamic_fd: kernel.Fd = fd_abi.first_dynamic_fd;
const max_process_map_batch_entries: usize = @intCast(process_abi.process_map_batch_max_entries);
const process_map_batch_entry_size: usize = @intCast(process_abi.process_map_batch_entry_size);

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

fn threadObjectIsLive(thread: kernel.ThreadObject) bool {
    const index: usize = @intCast(thread.thread_index);
    const owner: kernel.PrincipalId = @enumFromInt(thread.owner_principal_raw);
    const generation = scheduler.generationOfThread(index) orelse return false;
    return generation == thread.thread_generation and scheduler.threadOwnedBy(index, owner);
}

fn cleanupProcess(h: anytype, state: *kernel.KernelState, principal: kernel.PrincipalId, exit_state: kernel.TaskObjectState, exit_code: u32) void {
    const process_index = kernel.processIndexFromPrincipal(principal) orelse return;
    state.markThreadObjectsExitedForPrincipal(principal, exit_state, exit_code);
    state.markProcessObjectsExited(principal, exit_state, exit_code);
    _ = wakeTaskFdWaiters(h, state, principal);
    _ = scheduler.releasePrincipalThreads(principal);
    user_vm.lockAddressSpaces();
    user_vm.clearUserAddressSpace(principal);
    state.releasePrincipalNativeMemory(principal, h.free_list);
    state.resetProcessRuntimeTables(process_index);
    user_vm.unlockAddressSpaces();
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
        .owner_principal_raw = process.principal_raw,
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

fn startThread(state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd) u64 {
    const thread = state.threadObjectForFd(proc, fd, .{ .start = true }) orelse return sc.syscall_err_invalid;
    if (thread.state != .active or !threadObjectIsLive(thread)) return sc.syscall_err_invalid;
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
    if (process.state != .active) {
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
    if (process.state != .active) return sc.syscall_err_invalid;
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

fn exitCurrentThread(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame, code: u32) u64 {
    const current = scheduler.currentThread();
    const generation = scheduler.generationOfThread(current) orelse {
        state.markThreadObjectsExitedForPrincipal(proc, .exited, code);
        state.markProcessObjectsExited(proc, .exited, code);
        const handoff_target = wakeTaskFdWaiters(h, state, proc);
        h.exit_current_process(proc, @truncate(code), frame, h.before_current_thread_leave, handoff_target);
        return sc.syscall_ok;
    };
    state.markThreadObjectsExitedBySlot(current, generation, .exited, code);
    if (scheduler.liveThreadCount(proc) <= 1) {
        state.markProcessObjectsExited(proc, .exited, code);
        const handoff_target = wakeTaskFdWaiters(h, state, proc);
        h.exit_current_process(proc, @truncate(code), frame, h.before_current_thread_leave, handoff_target);
        return sc.syscall_ok;
    }
    if (!scheduler.exitCurrentThread(frame, sc.syscall_ok, h.before_current_thread_leave)) {
        h.exit_current_process(proc, @truncate(code), frame, h.before_current_thread_leave, null);
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
                state.markProcessObjectsExited(proc, .killed, @truncate(frame.rsi));
                state.markThreadObjectsExitedForPrincipal(proc, .killed, @truncate(frame.rsi));
                const handoff_target = wakeTaskFdWaiters(h, state, proc);
                h.exit_current_process(proc, @truncate(frame.rsi), frame, h.before_current_thread_leave, handoff_target);
            } else {
                cleanupProcess(h, state, target, .killed, @truncate(frame.rsi));
            }
            break :blk sc.syscall_ok;
        },
        sc.syscall_process_wait => waitProcess(h, state, proc, @intCast(frame.rdi), frame.rsi),
        sc.syscall_process_exit => blk: {
            state.markProcessObjectsExited(proc, .exited, @truncate(frame.rdi));
            state.markThreadObjectsExitedForPrincipal(proc, .exited, @truncate(frame.rdi));
            const handoff_target = wakeTaskFdWaiters(h, state, proc);
            h.exit_current_process(proc, @truncate(frame.rdi), frame, h.before_current_thread_leave, handoff_target);
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
            if (!scheduler.setFsBase(scheduler.currentThread(), fs_base)) break :blk sc.syscall_err_not_ready;
            break :blk sc.syscall_ok;
        },
        sc.syscall_thread_set_gs_base => blk: {
            const gs_base = frame.rdi;
            if (gs_base != 0 and !user_vm.isUserCanonicalVa(gs_base)) break :blk sc.syscall_err_invalid;
            if (!scheduler.setCurrentGsBase(gs_base)) break :blk sc.syscall_err_not_ready;
            break :blk sc.syscall_ok;
        },
        sc.syscall_process_map => mapIntoProcess(state, proc, h.free_list, frame),
        sc.syscall_process_map_batch => mapBatchIntoProcess(h, state, proc, h.free_list, frame),
        else => null,
    };
}
