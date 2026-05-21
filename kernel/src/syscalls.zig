const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const abi_root = @import("kernel_abi_root");
const ipc_buffer_abi = abi_root.ipc_buffer_abi;
const image_abi = abi_root.image_abi;
const trap_abi = abi_root.trap_abi;
const process_abi = abi_root.process_abi;
const process_builder_abi = abi_root.process_builder_abi;
const interrupts = @import("interrupts.zig");
const scheduler = @import("scheduler.zig");
const smp = @import("smp.zig");
const rtc = @import("rtc.zig");
const abi_trap_runtime = @import("runtime/abi_trap.zig");
const sc = @import("syscall/numbers.zig");
const syscall_lock_policy = @import("syscall/lock_policy.zig");
const memory_syscalls = @import("syscall/memory_helpers.zig");
const device_syscalls = @import("syscall/device.zig");
const process_syscalls = @import("syscall/process.zig");
const image_dispatch = @import("syscall/image.zig");
const ipc_syscalls = @import("syscall/ipc.zig");
const abi_trap_syscalls = @import("syscall/abi_trap.zig");

const TrapFrame = interrupts.TrapFrame;

pub const Hooks = struct {
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    kernel_state_ready: *const bool,
    write: *const fn ([]const u8) void,
    print_hex: *const fn (u64) void,
    print_number: *const fn (u64) void,
    principal_label: *const fn (kernel.PrincipalId) []const u8,
    principal_from_process_slot: *const fn (u64) ?kernel.PrincipalId,
    read_user_u64: *const fn (kernel.PrincipalId, u64) ?u64,
    write_user_u64: *const fn (kernel.PrincipalId, u64, u64) bool,
    copy_user_bytes_from_va: *const fn (kernel.PrincipalId, u64, []u8) bool,
    copy_bytes_to_user_va: *const fn (kernel.PrincipalId, u64, []const u8) bool,
    wake_waiting_thread_for_principal: *const fn (kernel.PrincipalId) void,
    wake_blocked_thread_for_principal: *const fn (kernel.PrincipalId) void,
    consume_pending_signal_for_principal: *const fn (kernel.PrincipalId) bool,
    switch_to_thread: *const fn (usize, *TrapFrame, ?u64) bool,
    block_current_thread_for_event: *const fn (*TrapFrame, bool, u64, u64) bool,
    exit_current_process: *const fn (kernel.PrincipalId, u8, *TrapFrame) void,
    total_usable_memory_bytes: u64,
};

var syscall_hooks_storage: Hooks = undefined;
var syscall_hooks_ready = false;
pub export var syscall_return_writeback_enabled: u64 = 1;
pub export var syscall_return_writeback_enabled_by_cpu: [smp.max_cpus]u64 = [_]u64{1} ** smp.max_cpus;
const syscall_fast_handled_mask: u64 = 1 << 63;
extern var syscall_entry_is_lstars: [smp.max_cpus]u64;

const KernelStateSpinLock = struct {
    value: u8 = 0,

    fn lock(self: *KernelStateSpinLock) void {
        while (true) {
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) return;
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                asm volatile ("pause");
            }
        }
    }

    fn unlock(self: *KernelStateSpinLock) void {
        @atomicStore(u8, &self.value, 0, .release);
    }
};

var kernel_state_lock: KernelStateSpinLock = .{};
var vm_object_page_scratch: [kernel.max_image_backing_pages]u64 = undefined;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_hooks_storage), &syscall_hooks_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_hooks_ready), &syscall_hooks_ready));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_return_writeback_enabled), &syscall_return_writeback_enabled));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_return_writeback_enabled_by_cpu), &syscall_return_writeback_enabled_by_cpu));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_entry_is_lstars), &syscall_entry_is_lstars));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_state_lock), &kernel_state_lock));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(vm_object_page_scratch), &vm_object_page_scratch));
    return end;
}

pub fn init(new_hooks: Hooks) void {
    syscall_hooks_storage = new_hooks;
    syscall_hooks_ready = true;
}

fn getHooks() *const Hooks {
    if (!syscall_hooks_ready) unreachable;
    return &syscall_hooks_storage;
}

fn currentCpuSlotBounded() usize {
    const cpu_slot = scheduler.currentCpuSlot();
    return if (cpu_slot < smp.max_cpus) cpu_slot else 0;
}

fn setSyscallReturnWritebackEnabled(enabled: bool) void {
    const value: u64 = if (enabled) 1 else 0;
    syscall_return_writeback_enabled = value;
    syscall_return_writeback_enabled_by_cpu[currentCpuSlotBounded()] = value;
}

fn currentSyscallEntryIsLstar() bool {
    return syscall_entry_is_lstars[currentCpuSlotBounded()] != 0;
}

fn hasExplicitUserLogLabel(message: []const u8) bool {
    if (message.len < 3 or message[0] != '[') return false;
    const close_index = std.mem.indexOfScalar(u8, message, ']') orelse return false;
    if (close_index <= 1) return false;
    return close_index + 1 == message.len or message[close_index + 1] == ' ';
}

fn writeThreadUserLogPrefix(h: *const Hooks, thread_index: usize) void {
    h.write("[Thread ");
    h.print_number(@intCast(thread_index));
    h.write("] ");
}

fn readUserPaddrBatch(
    h: *const Hooks,
    proc: kernel.PrincipalId,
    list_va: u64,
    page_count_u64: u64,
    out_paddrs: *[sc.syscall_batch_max_pages]u64,
) bool {
    var i: u64 = 0;
    while (i < page_count_u64) : (i += 1) {
        const entry_va = list_va + i * 8;
        out_paddrs[@intCast(i)] = h.read_user_u64(proc, entry_va) orelse return false;
    }
    return true;
}

fn dispatchIpcSyscall(
    h: *const Hooks,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    frame: *TrapFrame,
) ?u64 {
    if (ipc_syscalls.isCapTransferSyscall(frame.rax)) {
        kernel_state_lock.lock();
        defer kernel_state_lock.unlock();
        return ipc_syscalls.dispatchCapTransferSyscallLocked(state, h, proc, frame.rax, frame.rdi, frame.rsi) orelse sc.syscall_err_invalid;
    }
    return switch (frame.rax) {
        sc.syscall_signal_endpoint => blk: {
            kernel_state_lock.lock();
            defer kernel_state_lock.unlock();
            break :blk signalEndpointMessage(state, proc, frame.rdi, false, 0, 0, 0, 0);
        },
        sc.syscall_wait_event => blk: {
            const wait_mailbox = (frame.rdi & 0x1) != 0;
            const preserve_ipc_queue = (frame.rdi & 0x2) != 0;
            const timeout_ticks = frame.rsi;
            kernel_state_lock.lock();
            switch (ipc_syscalls.waitEventImmediate(state, h, proc, frame, wait_mailbox, preserve_ipc_queue)) {
                .ready => |result| {
                    kernel_state_lock.unlock();
                    break :blk result;
                },
                .pending => {},
            }
            if (preserve_ipc_queue) {
                kernel_state_lock.unlock();
                break :blk ipc_syscalls.blockWaitEvent(h, frame, wait_mailbox, true, timeout_ticks);
            }
            kernel_state_lock.unlock();
            break :blk ipc_syscalls.blockWaitEvent(h, frame, wait_mailbox, false, timeout_ticks);
        },
        sc.syscall_ipc_call_reply_recv, sc.syscall_ipc_call_reply_recv_fast => blk: {
            const endpoint_id = frame.rsi;
            const flags = frame.rdx;
            const signal_only = (flags & sc.ipc_call_flag_signal_only) != 0;
            kernel_state_lock.lock();
            if (ipc_syscalls.sendCallPayload(state, h, proc, endpoint_id, flags, frame.rdi, frame.r8, frame.r9, frame.r10)) |status| {
                if (status != sc.syscall_ok) {
                    kernel_state_lock.unlock();
                    break :blk status;
                }
            } else {
                const status = replyToCurrentIpcToken(h, frame.rdi, frame.r8, frame.r9, frame.r10);
                if (status != sc.syscall_ok) {
                    kernel_state_lock.unlock();
                    break :blk status;
                }
            }
            switch (ipc_syscalls.callReplyRecvImmediate(state, h, proc, frame, signal_only)) {
                .ready => |result| {
                    kernel_state_lock.unlock();
                    break :blk result;
                },
                .pending => {},
            }
            kernel_state_lock.unlock();
            break :blk ipc_syscalls.blockCallReplyRecv(h, frame, signal_only);
        },
        else => null,
    };
}

pub export fn syscallIpcDispatch(frame: *TrapFrame) callconv(.c) u64 {
    const h = getHooks();
    const entry_thread = scheduler.currentThreadIndex();
    setSyscallReturnWritebackEnabled(true);
    defer {
        if (scheduler.currentThreadIndex() != entry_thread) {
            setSyscallReturnWritebackEnabled(false);
        }
    }
    if (!h.kernel_state_ready.*) return sc.syscall_err_not_ready;

    const state = h.state;
    const proc = scheduler.currentUserPrincipal();
    return dispatchIpcSyscall(h, state, proc, frame) orelse sc.syscall_err_invalid;
}

pub export fn syscallIpcCallReplyRecvSignalOnlyDispatch(frame: *TrapFrame) callconv(.c) u64 {
    const h = getHooks();
    const entry_thread = scheduler.currentThreadIndex();
    setSyscallReturnWritebackEnabled(true);
    defer {
        if (scheduler.currentThreadIndex() != entry_thread) {
            setSyscallReturnWritebackEnabled(false);
        }
    }
    if (!h.kernel_state_ready.*) return sc.syscall_err_not_ready;

    const proc = scheduler.currentUserPrincipal();
    const status = signalEndpointMessage(h.state, proc, frame.rsi, true, frame.rdi, frame.r8, frame.r9, frame.r10);
    if (status != sc.syscall_ok) return status;
    switch (ipc_syscalls.callReplyRecvImmediate(h.state, h, proc, frame, true)) {
        .ready => |result| return result,
        .pending => {},
    }
    return ipc_syscalls.blockCallReplyRecv(h, frame, true);
}

const IpcSignalSave = ipc_syscalls.SignalSave;

fn signalEndpointMessage(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    endpoint_id: u64,
    grants_reply: bool,
    mr0: u64,
    mr1: u64,
    mr2: u64,
    mr3: u64,
) u64 {
    return ipc_syscalls.signalEndpointMessage(
        state,
        owner,
        scheduler.currentThreadIndex(),
        endpoint_id,
        grants_reply,
        mr0,
        mr1,
        mr2,
        mr3,
    );
}

fn exitAbiTrapReplyTargetIfRequested(
    h: *const Hooks,
    target: ipc_syscalls.SignalTarget,
    target_ctx: *scheduler.ThreadContext,
    response_flags: u64,
) bool {
    const target_has_abi_delegate = h.state.abiTrapDelegateFor(target.principal) != null;
    if ((response_flags & trap_abi.response_flag_exit) == 0) return false;
    if (!target_ctx.abi_trap_reply_pending and !target_has_abi_delegate) return false;

    ipc_syscalls.clearCurrentReplyToken();
    _ = scheduler.releaseThreadSlot(target.thread_index);
    _ = h.state.markProcessExited(target.principal);
    if ((response_flags & trap_abi.response_flag_skip_reclaim) == 0) {
        _ = abi_trap_runtime.reclaimPrivatePagesForProcess(h.state, target.principal);
    }
    return true;
}

fn markAbiTrapReplyPendingIfDelegated(
    h: *const Hooks,
    target: ipc_syscalls.SignalTarget,
    target_ctx: *scheduler.ThreadContext,
) void {
    if (h.state.abiTrapDelegateFor(target.principal) != null) {
        target_ctx.abi_trap_reply_pending = true;
    }
}

fn replyToCurrentIpcToken(h: *const Hooks, mr0: u64, mr1: u64, mr2: u64, mr3: u64) u64 {
    const target = switch (ipc_syscalls.currentReplyTarget()) {
        .ok => |reply_target| reply_target,
        .not_ready => return sc.syscall_err_not_ready,
        .endpoint => return sc.syscall_err_endpoint,
    };
    const target_thread = target.thread_index;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return sc.syscall_err_endpoint;
    if (exitAbiTrapReplyTargetIfRequested(h, target, target_ctx, mr1)) {
        return sc.syscall_ok;
    }
    markAbiTrapReplyPendingIfDelegated(h, target, target_ctx);
    return ipc_syscalls.replyToTargetFromCurrent(target_thread, mr0, mr1, mr2, mr3);
}

pub export fn syscallIpcCallReplyRecvSignalOnlySparse(endpoint_id: u64, save: *const IpcSignalSave, out_frame: *TrapFrame) callconv(.c) usize {
    const h = getHooks();
    if (!h.kernel_state_ready.*) return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_not_ready);

    const proc = scheduler.currentUserPrincipal();
    const current_thread = scheduler.currentThreadIndex();
    const current_ctx = scheduler.getThreadContext(current_thread) orelse {
        return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_not_ready);
    };
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse {
        return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_not_ready);
    };
    const reply_to_token = endpoint_id == 0;
    const target_info = switch (ipc_syscalls.resolveSparseTarget(h.state, proc, current_thread, endpoint_id)) {
        .ok => |info| info,
        .no_reply_target => {
            return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_not_ready);
        },
        .no_reply_token => {
            return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_endpoint);
        },
        .no_target => {
            return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_endpoint);
        },
        .no_target_ctx => {
            return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_endpoint);
        },
        .no_target_hot => {
            return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_endpoint);
        },
        .stale_target => {
            return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_endpoint);
        },
        .self_target => {
            return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_not_ready);
        },
    };
    const target = target_info.target;
    const target_ctx = target_info.ctx;
    const target_hot = target_info.hot;

    const target_was_ready = target_hot.ready != 0;
    const send_status = if (reply_to_token) blk: {
        ipc_syscalls.clearCurrentReplyToken();
        if (exitAbiTrapReplyTargetIfRequested(h, target, target_ctx, save.mr1)) {
            if (ipc_syscalls.takeQueuedMessageForThread(current_thread)) |msg| {
                return ipc_syscalls.writeSignalQueuedReturn(out_frame, save, msg);
            }
            if (current_hot.signal_pending != 0) {
                ipc_syscalls.clearSignalPendingForThread(current_thread, current_ctx);
                return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_ok);
            }
            if (!ipc_syscalls.blockSparseCurrent(out_frame, save)) {
                return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_not_ready);
            }
            return @intFromPtr(out_frame);
        }
        markAbiTrapReplyPendingIfDelegated(h, target, target_ctx);
        const status = ipc_syscalls.deliverOrQueueMessageToThread(
            target.thread_index,
            0,
            current_thread,
            false,
            save.mr0,
            save.mr1,
            save.mr2,
            save.mr3,
        );
        break :blk status;
    } else ipc_syscalls.deliverOrQueueMessageToThread(
        target.thread_index,
        endpoint_id,
        current_thread,
        true,
        save.mr0,
        save.mr1,
        save.mr2,
        save.mr3,
    );
    if (send_status != sc.syscall_ok) {
        return ipc_syscalls.writeSignalReturn(out_frame, save, send_status);
    }

    if (ipc_syscalls.takeQueuedMessageForThread(current_thread)) |msg| {
        return ipc_syscalls.writeSignalQueuedReturn(out_frame, save, msg);
    }

    if (current_hot.signal_pending != 0) {
        ipc_syscalls.clearSignalPendingForThread(current_thread, current_ctx);
        return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_ok);
    }

    if (current_hot.allocated == 0) {
        return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_not_ready);
    }
    if (target_was_ready) {
        if (!ipc_syscalls.blockSparseCurrent(out_frame, save)) {
            return ipc_syscalls.writeSignalReturn(out_frame, save, sc.syscall_err_not_ready);
        }
        return @intFromPtr(out_frame);
    }

    return ipc_syscalls.switchSparseToTarget(current_thread, current_ctx, target.thread_index, target_ctx, save);
}

pub export fn syscallIpcFastDispatch(nr: u64, arg0: u64, arg1: u64, arg2: u64) callconv(.c) u64 {
    const h = getHooks();
    if (!h.kernel_state_ready.*) return syscall_fast_handled_mask | sc.syscall_err_not_ready;

    const state = h.state;
    const proc = scheduler.currentUserPrincipal();
    const result = ipc_syscalls.fastDispatch(state, h, proc, nr, arg0, arg1, arg2) orelse return 0;
    return syscall_fast_handled_mask | result;
}

fn syscallDispatchFrom(frame: *TrapFrame, entry_is_lstar: bool) u64 {
    const h = getHooks();
    const entry_thread = scheduler.currentThreadIndex();
    setSyscallReturnWritebackEnabled(true);
    defer {
        if (scheduler.currentThreadIndex() != entry_thread) {
            // When a syscall returns to a different thread, the loaded frame
            // already belongs to that target thread and must not inherit the
            // caller's syscall result in RAX.
            setSyscallReturnWritebackEnabled(false);
        }
    }
    if (!h.kernel_state_ready.*) return sc.syscall_err_not_ready;

    const state = h.state;
    const proc = scheduler.currentUserPrincipal();
    if (!state.hasActivePrincipal(proc)) {
        h.write("syscall from inactive principal proc=");
        h.write(h.principal_label(proc));
        h.write(" thread=");
        h.print_number(@intCast(scheduler.currentThreadIndex()));
        h.write("\n");
        h.exit_current_process(proc, 0, frame);
        return sc.syscall_ok;
    }
    const abi_delegate = state.abiTrapDelegateFor(proc);
    const has_abi_delegate = abi_delegate != null;
    if (entry_is_lstar or has_abi_delegate) {
        if (abi_delegate) |delegate| {
            if (abi_trap_runtime.dispatchKnownDelegate(state, proc, delegate, frame)) |result| {
                return result;
            }
        } else if (abi_trap_runtime.dispatchDelegate(state, proc, frame)) |result| {
            return result;
        }
    }
    if (dispatchIpcSyscall(h, state, proc, frame)) |result| return result;
    const hold_kernel_state_lock = syscall_lock_policy.needsKernelStateLock(frame.rax);
    if (hold_kernel_state_lock) {
        kernel_state_lock.lock();
    }
    defer {
        if (hold_kernel_state_lock) kernel_state_lock.unlock();
    }

    if (device_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (process_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (image_dispatch.dispatch(h, state, proc, frame, &vm_object_page_scratch)) |result| {
        return result;
    }
    if (abi_trap_syscalls.dispatch(state, proc, frame)) |result| {
        return result;
    }

    switch (frame.rax) {
        sc.syscall_alloc_page => {
            const cap = state.allocPageTo(proc, h.free_list) catch return sc.syscall_err_alloc;
            return cap.paddr;
        },
        sc.syscall_map_page, sc.syscall_map_mmio => {
            const writable = (frame.rdx & 0x1) != 0;
            if (capability.mapUserPageFromCapability(state, proc, frame.rdi, frame.rsi, writable)) {
                return sc.syscall_ok;
            }
            return sc.syscall_err_map;
        },
        sc.syscall_map_page_anywhere => {
            const writable = (frame.rsi & 0x1) != 0;
            const map_va = capability.findFreeUserMappingRange(proc, 1, 1) orelse return sc.syscall_err_map;
            if (capability.mapUserPageFromCapability(state, proc, map_va, frame.rdi, writable)) {
                return map_va;
            }
            return sc.syscall_err_map;
        },
        sc.syscall_create_ipc_buffer_from_page => {
            const role = ipc_syscalls.parseIpcBufferRole(frame.rdx) orelse return sc.syscall_err_invalid;
            const rights = ipc_syscalls.parseIpcBufferRights(frame.rsi) orelse return sc.syscall_err_invalid;
            const cap_id = state.createIpcBufferFromPage(proc, frame.rdi, rights, role) catch |err| switch (err) {
                kernel.KernelError.CapabilityNotFound => return sc.syscall_err_send,
                kernel.KernelError.InvalidState => return sc.syscall_err_invalid,
                kernel.KernelError.TableFull => return sc.syscall_err_alloc,
                else => return sc.syscall_err_grant,
            };
            return ipc_buffer_abi.encodeIpcBufferToken(cap_id);
        },
        sc.syscall_grant_ipc_buffer_on_endpoint => {
            const cap_id = ipc_buffer_abi.decodeIpcBufferToken(frame.rdi) orelse return sc.syscall_err_invalid;
            const rights = ipc_syscalls.parseIpcBufferRights(frame.rdx) orelse return sc.syscall_err_invalid;
            const child_id = state.grantIpcBufferCapOnEndpoint(proc, frame.rsi, cap_id, rights) catch |err| switch (err) {
                kernel.KernelError.EndpointNotFound => return sc.syscall_err_endpoint,
                kernel.KernelError.CapabilityNotFound => return sc.syscall_err_send,
                kernel.KernelError.InvalidState => return sc.syscall_err_invalid,
                kernel.KernelError.TableFull => return sc.syscall_err_alloc,
                else => return sc.syscall_err_grant,
            };
            return ipc_buffer_abi.encodeIpcBufferToken(child_id);
        },
        sc.syscall_share_ipc_buffer_on_endpoint => {
            return ipc_syscalls.transferIpcBufferCapOnEndpoint(state, h, proc, frame.rsi, frame.rdi, frame.rdx);
        },
        sc.syscall_accept_ipc_buffer_transfer => {
            const cap_id = state.acceptIpcBufferTransfer(proc, frame.rdi) catch |err| switch (err) {
                kernel.KernelError.MailboxEmpty => return sc.syscall_err_empty,
                kernel.KernelError.InvalidState => return sc.syscall_err_invalid,
                kernel.KernelError.CapabilityNotFound => return sc.syscall_err_send,
                kernel.KernelError.TableFull => return sc.syscall_err_alloc,
                else => return sc.syscall_err_send,
            };
            return ipc_buffer_abi.encodeIpcBufferToken(cap_id);
        },
        sc.syscall_map_ipc_buffer_anywhere => {
            const cap_id = ipc_buffer_abi.decodeIpcBufferToken(frame.rdi) orelse return sc.syscall_err_invalid;
            const ipc_cap = state.getIpcBufferTableConst(proc).findByCapId(cap_id) orelse return sc.syscall_err_invalid;
            const writable = (frame.rsi & 0x1) != 0;
            if (!ipc_cap.rights.map) return sc.syscall_err_invalid;
            if (writable) {
                if (!ipc_cap.rights.write) return sc.syscall_err_map;
            } else if (!ipc_cap.rights.read) {
                return sc.syscall_err_map;
            }
            const map_va = capability.findFreeUserMappingRange(proc, 1, 1) orelse return sc.syscall_err_map;
            if (capability.mapTrustedUserPage(proc, map_va, ipc_cap.paddr, writable)) {
                return map_va;
            }
            return sc.syscall_err_map;
        },
        sc.syscall_map_pages_batch => {
            const page_count_u64 = frame.rdx;
            if (page_count_u64 == 0 or page_count_u64 > sc.syscall_batch_max_pages) return sc.syscall_err_invalid;
            var paddrs: [sc.syscall_batch_max_pages]u64 = undefined;
            const page_count: usize = @intCast(page_count_u64);
            const buf = std.mem.sliceAsBytes(paddrs[0..page_count]);
            if (!h.copy_user_bytes_from_va(proc, frame.rsi, buf)) return sc.syscall_err_invalid;
            if (capability.mapUserPagesFromCapabilityBatch(state, proc, frame.rdi, paddrs[0..page_count], (frame.rcx & 0x1) != 0)) {
                return sc.syscall_ok;
            }
            return sc.syscall_err_map;
        },
        sc.syscall_alloc_map_pages => {
            const page_count_u64 = frame.rsi;
            if (page_count_u64 == 0 or page_count_u64 > sc.syscall_batch_max_pages) return sc.syscall_err_invalid;
            memory_syscalls.allocMapPages(.{
                .state = state,
                .proc = proc,
                .free_list = h.free_list,
                .base_va = frame.rdi,
                .page_count = @intCast(page_count_u64),
                .writable = (frame.rdx & 0x1) != 0,
                .drop_cap_after_map = (frame.rdx & sc.syscall_alloc_map_drop_cap_flag) != 0,
                .out_paddr_list_va = frame.rcx,
                .write_user_u64 = h.write_user_u64,
            }) catch |err| switch (err) {
                error.InvalidArgument => return sc.syscall_err_invalid,
                error.AllocationFailed => return sc.syscall_err_alloc,
                error.MapFailed => return sc.syscall_err_map,
            };
            return sc.syscall_ok;
        },
        sc.syscall_alloc_map_pages_anywhere => {
            const page_count_u64 = frame.rdi;
            if (page_count_u64 == 0 or page_count_u64 > sc.syscall_batch_max_pages) return sc.syscall_err_invalid;
            return memory_syscalls.allocMapPagesAnywhere(.{
                .state = state,
                .proc = proc,
                .free_list = h.free_list,
                .base_va = 0,
                .page_count = @intCast(page_count_u64),
                .writable = (frame.rsi & 0x1) != 0,
                .drop_cap_after_map = (frame.rsi & sc.syscall_alloc_map_drop_cap_flag) != 0,
                .out_paddr_list_va = frame.rdx,
                .write_user_u64 = h.write_user_u64,
            }) catch |err| switch (err) {
                error.InvalidArgument => return sc.syscall_err_invalid,
                error.AllocationFailed => return sc.syscall_err_alloc,
                error.MapFailed => return sc.syscall_err_map,
            };
        },
        sc.syscall_move_cap => {
            const to = switch (frame.rsi) {
                0 => proc,
                1 => kernel.PrincipalId.Device0,
                else => return sc.syscall_err_invalid,
            };
            const from = if (to == proc) kernel.PrincipalId.Device0 else proc;
            const rights = capability.parseRights(frame.rdx);
            state.moveCap(from, to, frame.rdi, rights) catch return sc.syscall_err_move;
            return sc.syscall_ok;
        },
        sc.syscall_grant_cap => {
            const to = h.principal_from_process_slot(frame.rsi) orelse return sc.syscall_err_invalid;
            const rights = capability.parseRights(frame.rdx);
            state.grantCap(proc, to, frame.rdi, rights) catch return sc.syscall_err_grant;
            return sc.syscall_ok;
        },
        sc.syscall_grant_caps_batch => {
            const page_count_u64 = frame.rsi;
            if (page_count_u64 == 0 or page_count_u64 > sc.syscall_batch_max_pages) return sc.syscall_err_invalid;
            const to = h.principal_from_process_slot(frame.rdx) orelse return sc.syscall_err_invalid;
            const rights = capability.parseRights(frame.rcx);
            var paddrs: [sc.syscall_batch_max_pages]u64 = undefined;
            if (!readUserPaddrBatch(h, proc, frame.rdi, page_count_u64, &paddrs)) return sc.syscall_err_invalid;
            state.grantCapsBatch(proc, to, paddrs[0..@intCast(page_count_u64)], rights) catch return sc.syscall_err_grant;
            return sc.syscall_ok;
        },
        sc.syscall_install_caps_batch => {
            if (!state.isBootstrapOwner(proc)) return sc.syscall_err_invalid;
            const page_count_u64 = frame.rsi;
            if (page_count_u64 == 0 or page_count_u64 > sc.syscall_batch_max_pages) return sc.syscall_err_invalid;
            const rights = capability.parseRights(frame.rdx);
            var paddrs: [sc.syscall_batch_max_pages]u64 = undefined;
            if (!readUserPaddrBatch(h, proc, frame.rdi, page_count_u64, &paddrs)) return sc.syscall_err_invalid;
            var i: usize = 0;
            while (i < page_count_u64) : (i += 1) {
                state.installCap(proc, paddrs[i], rights) catch return sc.syscall_err_grant;
            }
            return sc.syscall_ok;
        },
        sc.syscall_grant_cap_on_endpoint => {
            const endpoint_id = frame.rsi;
            const rights = capability.parseRights(frame.rdx);
            state.grantCapOnEndpoint(proc, endpoint_id, frame.rdi, rights) catch |err| switch (err) {
                kernel.KernelError.EndpointNotFound => return sc.syscall_err_endpoint,
                else => return sc.syscall_err_grant,
            };
            return sc.syscall_ok;
        },
        sc.syscall_grant_caps_batch_on_endpoint => {
            const page_count_u64 = frame.rsi;
            if (page_count_u64 == 0 or page_count_u64 > sc.syscall_batch_max_pages) return sc.syscall_err_invalid;
            const endpoint_id = frame.rdx;
            const rights = capability.parseRights(frame.rcx);
            var paddrs: [sc.syscall_batch_max_pages]u64 = undefined;
            if (!readUserPaddrBatch(h, proc, frame.rdi, page_count_u64, &paddrs)) return sc.syscall_err_invalid;
            state.grantCapsBatchOnEndpoint(proc, endpoint_id, paddrs[0..@intCast(page_count_u64)], rights) catch |err| switch (err) {
                kernel.KernelError.EndpointNotFound => return sc.syscall_err_endpoint,
                else => return sc.syscall_err_grant,
            };
            return sc.syscall_ok;
        },
        sc.syscall_install_endpoint => {
            const target = h.principal_from_process_slot(frame.rdx) orelse return sc.syscall_err_invalid;
            state.installEndpoint(proc, frame.rsi, target) catch |err| switch (err) {
                kernel.KernelError.InvalidState => return sc.syscall_err_invalid,
                kernel.KernelError.TableFull => return sc.syscall_err_alloc,
                else => return sc.syscall_err_endpoint,
            };
            return sc.syscall_ok;
        },
        sc.syscall_signal_endpoint => {
            return signalEndpointMessage(state, proc, frame.rdi, false, 0, 0, 0, 0);
        },
        sc.syscall_get_tick_count => {
            return scheduler.lapic_tick_count;
        },
        sc.syscall_get_rtc_unix_time => {
            return rtc.unixTimeSeconds();
        },
        sc.syscall_get_process_slot => {
            const slot = kernel.processIndexFromPrincipal(proc) orelse return sc.syscall_err_invalid;
            return @intCast(slot);
        },
        sc.syscall_set_fs_base_self => {
            const fs_base = frame.rdi;
            if (fs_base != 0 and !capability.isUserCanonicalVa(fs_base)) return sc.syscall_err_invalid;
            if (!scheduler.setCurrentThreadFsBase(fs_base)) return sc.syscall_err_not_ready;
            return sc.syscall_ok;
        },
        sc.syscall_get_process_status => {
            const target = h.principal_from_process_slot(frame.rdi) orelse return sc.syscall_err_invalid;
            const status = state.processStatus(target);
            const kind: process_abi.ProcessStatusKind = if (status.active)
                .active
            else if (status.faulted)
                .faulted
            else
                .inactive;
            return process_abi.encodeProcessStatus(kind, status.fault_vector);
        },
        sc.syscall_get_memory_stats => {
            const out_va = frame.rdi;
            if ((out_va & 0x7) != 0) return sc.syscall_err_invalid;
            const free_bytes = @as(u64, @intCast(h.free_list.len)) * 4096;
            const total_bytes = h.total_usable_memory_bytes;
            const used_bytes = if (total_bytes >= free_bytes) total_bytes - free_bytes else 0;
            if (!h.write_user_u64(proc, out_va + 0, total_bytes)) return sc.syscall_err_invalid;
            if (!h.write_user_u64(proc, out_va + 8, used_bytes)) return sc.syscall_err_invalid;
            if (!h.write_user_u64(proc, out_va + 16, free_bytes)) return sc.syscall_err_invalid;
            if (!h.write_user_u64(proc, out_va + 24, 4096)) return sc.syscall_err_invalid;
            return sc.syscall_ok;
        },
        sc.syscall_process_exit => {
            h.exit_current_process(proc, @truncate(frame.rdi), frame);
            return sc.syscall_ok;
        },
        sc.syscall_register_iommu_driver => {
            return sc.syscall_ok;
        },
        sc.syscall_send_cap => {
            return ipc_syscalls.transferPageCapOnEndpoint(state, h, proc, frame.rsi, frame.rdi, false);
        },
        sc.syscall_share_cap => {
            return ipc_syscalls.transferPageCapOnEndpoint(state, h, proc, frame.rsi, frame.rdi, true);
        },
        sc.syscall_recv_cap => {
            return ipc_syscalls.recvCap(state, proc);
        },
        sc.syscall_install_mmio_cap => {
            if (!state.isBootstrapOwner(proc)) return sc.syscall_err_invalid;
            const paddr = frame.rdi;
            const rights = capability.parseRights(frame.rsi);
            state.installCap(proc, paddr, rights) catch return sc.syscall_err_grant;
            return sc.syscall_ok;
        },
        sc.syscall_accept_cap_transfer => {
            return ipc_syscalls.acceptCapTransfer(state, proc, frame.rdi);
        },
        sc.syscall_publish_service_endpoint => {
            if (!state.isBootstrapOwner(proc)) return sc.syscall_err_invalid;
            const target = h.principal_from_process_slot(frame.rsi) orelse return sc.syscall_err_invalid;
            state.publishServiceEndpoint(frame.rdi, target) catch |err| switch (err) {
                kernel.KernelError.InvalidState => return sc.syscall_err_invalid,
                kernel.KernelError.TableFull => return sc.syscall_err_alloc,
                else => return sc.syscall_err_endpoint,
            };
            return sc.syscall_ok;
        },
        sc.syscall_wait_event => {
            const wait_mailbox = (frame.rdi & 0x1) != 0;
            const preserve_ipc_queue = (frame.rdi & 0x2) != 0;
            const timeout_ticks = frame.rsi;
            switch (ipc_syscalls.waitEventImmediate(state, h, proc, frame, wait_mailbox, preserve_ipc_queue)) {
                .ready => |result| return result,
                .pending => {},
            }
            if (preserve_ipc_queue) {
                return ipc_syscalls.blockWaitEvent(h, frame, wait_mailbox, true, timeout_ticks);
            }
            return ipc_syscalls.blockWaitEvent(h, frame, wait_mailbox, false, timeout_ticks);
        },
        sc.syscall_revoke_tree => {
            state.revokeCapTree(proc, frame.rdi) catch return sc.syscall_err_revoke;
            state.bumpEndpointGeneration();
            scheduler.invalidateAllIpcFastpathState();
            return sc.syscall_ok;
        },
        sc.syscall_drop_present => {
            if (capability.dropPresentForUserMappedPaddr(state, proc, frame.rdi)) {
                state.bumpEndpointGeneration();
                scheduler.invalidateAllIpcFastpathState();
                return sc.syscall_ok;
            }
            return sc.syscall_err_drop_present;
        },
        sc.syscall_switch_thread => {
            const target_thread: usize = @intCast(frame.rdi);
            if (target_thread >= scheduler.max_thread_slots) return sc.syscall_err_invalid;
            if (!scheduler.isThreadReady(target_thread)) return sc.syscall_err_not_ready;
            if (!h.switch_to_thread(target_thread, frame, sc.syscall_ok)) return sc.syscall_err_not_ready;
            return sc.syscall_ok;
        },
        sc.syscall_log => {
            const req_len_u64 = frame.rsi;
            if (req_len_u64 == 0) return sc.syscall_ok;
            if (req_len_u64 > sc.user_log_max_bytes) return sc.syscall_err_invalid;
            const req_len: usize = @intCast(req_len_u64);
            var buf: [sc.user_log_max_bytes]u8 = undefined;
            const msg = buf[0..req_len];
            if (!h.copy_user_bytes_from_va(proc, frame.rdi, msg)) return sc.syscall_err_invalid;
            if (!hasExplicitUserLogLabel(msg)) {
                writeThreadUserLogPrefix(h, scheduler.currentThreadIndex());
            }
            h.write(msg);
            return sc.syscall_ok;
        },
        else => return sc.syscall_err_invalid,
    }
}

pub export fn syscallDispatch(frame: *TrapFrame) callconv(.c) u64 {
    return syscallDispatchFrom(frame, currentSyscallEntryIsLstar());
}

fn syscallLstarDelegateDispatch(frame: *TrapFrame) ?u64 {
    const h = getHooks();
    const entry_thread = scheduler.currentThreadIndex();
    setSyscallReturnWritebackEnabled(true);
    defer {
        if (scheduler.currentThreadIndex() != entry_thread) {
            setSyscallReturnWritebackEnabled(false);
        }
    }
    if (!h.kernel_state_ready.*) return sc.syscall_err_not_ready;

    const proc = scheduler.currentUserPrincipal();
    const state = h.state;
    if (abi_trap_runtime.dispatchDelegate(state, proc, frame)) |result| return result;
    return null;
}

pub export fn syscallLstarDispatch(frame: *TrapFrame) callconv(.c) u64 {
    if (syscallLstarDelegateDispatch(frame)) |result| return result;
    return syscallDispatchFrom(frame, false);
}

test "explicit userlog label detection" {
    try std.testing.expect(hasExplicitUserLogLabel("[seed] ready\n"));
    try std.testing.expect(hasExplicitUserLogLabel("[seed]"));
    try std.testing.expect(!hasExplicitUserLogLabel("seed ready\n"));
    try std.testing.expect(!hasExplicitUserLogLabel("[seed"));
    try std.testing.expect(!hasExplicitUserLogLabel("[] bad\n"));
}
