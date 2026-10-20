const b3 = @import("../ipc_path_trace.zig");
const std = @import("std");
const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const ipc_metric = @import("../ipc_metric.zig");
const kernel = @import("../kernel.zig");
const scheduler = @import("../scheduler.zig").connection;
const sc = @import("numbers.zig");
const fd_syscall = @import("fd.zig");

const fd_abi = abi_root.fd_abi;
const ipc_abi = abi_root.ipc_abi;
const TrapFrame = interrupts.TrapFrame;
const first_dynamic_fd: kernel.Fd = fd_abi.first_dynamic_fd;

pub fn drainCancellations(h: anytype, state: *kernel.KernelState) void {
    if (!scheduler.hasPendingIpcCancellations() and !state.ipc_cancel_writable_pending) return;
    state.drainIpcCancellations(h.free_list);
    if (!state.ipc_cancel_writable_pending) return;
    var targets: [32]kernel.ThreadWakeTarget = undefined;
    while (true) {
        const count = state.takeIpcCancellationWritableWaiters(&targets);
        if (count == 0) break;
        _ = fd_syscall.wakeThreadTargets(h, state, targets[0..count]);
    }
}

fn mapError(err: kernel.KernelError) u64 {
    return switch (err) {
        kernel.KernelError.MailboxEmpty => sc.syscall_err_empty,
        kernel.KernelError.MailboxFull => sc.syscall_err_not_ready,
        kernel.KernelError.TableFull, kernel.KernelError.OutOfFreePages => sc.syscall_err_alloc,
        else => sc.syscall_err_invalid,
    };
}

fn fdFlagsFromTransfer(flags_bits: u64, transfer_flags: u64) ?kernel.FdFlags {
    if ((transfer_flags & ~ipc_abi.transfer_known_mask) != 0) return null;
    var bits: u32 = @truncate(flags_bits);
    if ((transfer_flags & ipc_abi.transfer_cloexec) != 0) bits |= fd_abi.flag_cloexec;
    if ((transfer_flags & ipc_abi.transfer_nonblock) != 0) bits |= fd_abi.flag_nonblock;
    if ((transfer_flags & ipc_abi.transfer_inherit) != 0) bits |= fd_abi.flag_inherit;
    if ((transfer_flags & ipc_abi.transfer_private) != 0) bits |= fd_abi.flag_private;
    return kernel.fdFlagsFromBits(bits);
}

fn readIpcMessage(
    h: anytype,
    proc: kernel.PrincipalId,
    frame: *const TrapFrame,
    fd_storage: *[kernel.max_ipc_message_fds]kernel.IpcSendFd,
) ?kernel.IpcSendMessage {
    const start = ipc_metric.timestamp();
    defer ipc_metric.record(.copyin, start);
    const count = ipc_abi.sendCount(frame);
    if (count > kernel.max_ipc_message_fds) return null;
    const fd_count: usize = @intCast(count);
    const fd_array_va = frame.r13;
    if (fd_count != 0 and (fd_array_va == 0 or fd_array_va > std.math.maxInt(u64) - fd_count * ipc_abi.fd_item_size)) return null;
    const words = ipc_abi.inputWords(frame);

    comptime std.debug.assert(ipc_abi.fd_item_size == 32 and ipc_abi.fd_item_fd_offset == 0 and
        ipc_abi.fd_item_rights_offset == 8 and ipc_abi.fd_item_flags_offset == 16 and
        ipc_abi.fd_item_transfer_flags_offset == 24);
    var i: usize = 0;
    while (i < fd_count) : (i += 1) {
        const item_va = fd_array_va + @as(u64, @intCast(i)) * ipc_abi.fd_item_size;
        var item: [4]u64 = undefined;
        if (!h.copy_user_bytes_from_va(proc, item_va, std.mem.sliceAsBytes(item[0..]))) return null;
        const fd = item[0];
        if (fd > std.math.maxInt(kernel.Fd)) return null;
        const rights_bits = item[1];
        const flags_bits = item[2];
        const transfer_flags = item[3];
        fd_storage[i] = .{
            .fd = @intCast(fd),
            .rights = kernel.fdRightsFromBits(rights_bits),
            .flags = fdFlagsFromTransfer(flags_bits, transfer_flags) orelse return null,
            .move = (transfer_flags & ipc_abi.transfer_move) != 0,
        };
    }
    return .{
        .words = words,
        .fds = fd_storage[0..fd_count],
    };
}

const Receive = struct { memory: ipc_abi.ReceiveMemory, capacity: u8 };
pub fn readReceive(frame: *const TrapFrame) ?Receive {
    const capacity = ipc_abi.receiveCapacity(frame);
    if (capacity > kernel.max_ipc_message_fds) return null;
    if (capacity != 0 and (frame.rbx == 0 or
        frame.rbx > std.math.maxInt(u64) - capacity * ipc_abi.fd_item_size)) return null;
    return .{ .memory = .{ .active = true, .array = frame.rbx }, .capacity = @intCast(capacity) };
}

fn writeRecvMessage(h: anytype, target: kernel.ThreadWakeTarget, live_frame: ?*TrapFrame, result: kernel.IpcRecvResult) u64 {
    const start = ipc_metric.timestamp();
    defer ipc_metric.record(.copyout, start);
    if (result.fd_count > target.recv_fd_capacity) return sc.syscall_err_alloc;
    // The destination belongs to the original wait registration. Never trust
    // later register inputs again after descriptors have been installed.
    const memory = target.recv_memory;
    for (result.fds[0..result.fd_count], 0..) |item, i| {
        const words = [4]u64{ item.fd, kernel.fdRightsToBits(item.rights), kernel.fdFlagsToBits(item.flags), 0 };
        if (!h.copy_bytes_to_user_va(target.owner, memory.array + i * ipc_abi.fd_item_size, std.mem.sliceAsBytes(&words))) return sc.syscall_err_invalid;
    }
    if (live_frame) |frame| {
        ipc_abi.outputWords(frame, result.words);
        frame.r15 = result.fd_count;
    } else if (!scheduler.stageIpcResult(target, result.words, @intCast(result.fd_count))) return sc.syscall_err_closed;
    return sc.syscall_ok;
}

fn recvMessageToUser(
    h: anytype,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    fd: kernel.Fd,
    memory: ipc_abi.ReceiveMemory,
    fd_capacity: usize,
    receiver_thread: usize,
    receiver_generation: u32,
    token: u64,
    frame: ?*TrapFrame,
) u64 {
    const start = ipc_metric.timestamp();
    defer ipc_metric.record(.recv_message, start);
    // Only a second CALL would overwrite the current authority. Ordinary
    // messages remain receivable while serving a CALL (e.g. nested RPC).
    if (scheduler.peekIpcImplicit(receiver_thread, receiver_generation, proc) != null and
        (state.ipcRecvPendingCall(proc, fd) catch |err| return mapError(err))) return sc.syscall_err_invalid;
    var wake_storage: [32]kernel.ThreadWakeTarget = undefined;
    const outcome = state.ipcRecvWithWritableWake(
        proc,
        fd,
        fd_capacity,
        first_dynamic_fd,
        h.free_list,
        wake_storage[0..],
    ) catch |err| return mapError(err);
    _ = fd_syscall.wakeThreadTargets(
        h,
        state,
        wake_storage[0..outcome.writable_wake_count],
    );
    while (true) {
        const count = state.takeIpcWritableWaitersForRecvFd(proc, fd, &wake_storage) catch break;
        if (count == 0) break;
        _ = fd_syscall.wakeThreadTargets(h, state, wake_storage[0..count]);
    }
    const result = outcome.message;
    var status = writeRecvMessage(h, .{ .owner = proc, .thread_index = receiver_thread, .thread_generation = receiver_generation, .wait_token = token, .recv_memory = memory, .recv_fd_capacity = @intCast(fd_capacity) }, frame, result);
    if (!result.call.isNull()) {
        const body = state.ipcReplySlot(result.call).?;
        if (status == sc.syscall_ok and scheduler.bindIpcImplicit(receiver_thread, receiver_generation, proc, &body.implicit)) {
            // The queue authority is now owned by this thread's binding.
        } else {
            state.releaseIpcCallAuthority(result.call);
            status = sc.syscall_err_invalid;
        }
    }
    if (status != sc.syscall_ok) {
        if (frame) |live| {
            ipc_abi.outputWords(live, .{ 0, 0, 0, 0 });
            live.r15 = 0;
        }
        var i: usize = 0;
        while (i < result.fd_count) : (i += 1) {
            state.closeFdWithFreeList(proc, result.fds[i].fd, h.free_list) catch {};
        }
    } else {
        _ = state.noteIpcReceiverForFd(
            proc,
            fd,
            receiver_thread,
            receiver_generation,
        );
    }
    return status;
}

fn completeRecvWaiter(h: anytype, state: *kernel.KernelState, target: kernel.ThreadWakeTarget) u64 {
    if (!target.recv_memory.active) return sc.syscall_err_not_ready;
    return recvMessageToUser(
        h,
        state,
        target.owner,
        target.recv_fd,
        target.recv_memory,
        target.recv_fd_capacity,
        target.thread_index,
        target.thread_generation,
        target.wait_token,
        null,
    );
}

fn wakeIpcWaitersForSendFd(
    h: anytype,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    fd: kernel.Fd,
    handoff_target: ?*?kernel.ThreadWakeTarget,
) bool {
    return wakeIpcWaitersForSendFdMode(false, h, state, proc, fd, handoff_target);
}

fn wakeIpcWaitersForSendFdMode(
    comptime poll_only: bool,
    h: anytype,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    fd: kernel.Fd,
    handoff_target: ?*?kernel.ThreadWakeTarget,
) bool {
    if (handoff_target) |target| target.* = null;
    var all_copies_ok = true;
    while (true) {
        var wake_storage: [32]kernel.ThreadWakeTarget = undefined;
        const wake_count = (if (poll_only)
            state.wakeIpcPollWaitersForSendFd(proc, fd, wake_storage[0..])
        else
            state.wakeIpcWaitersForSendFd(proc, fd, wake_storage[0..])) catch return false;
        if (wake_count == 0) break;
        for (wake_storage[0..wake_count], 0..) |target, target_index| {
            if (target.wait_token == 0 or (target.group.isNull() and target.receive_block.isNull())) continue;
            var already_handled = false;
            for (wake_storage[0..target_index]) |earlier| {
                if (!target.group.isNull() and earlier.wait_token == target.wait_token and
                    earlier.group.matches(target.group))
                {
                    already_handled = true;
                    break;
                }
            }
            if (already_handled) continue;
            const claim = scheduler.claimWaitTokenCompletion(
                target.thread_index,
                target.thread_generation,
                target.owner,
                target.wait_token,
            );
            switch (claim) {
                .stale => {
                    state.cancelFdWaitGroup(target.group, target.wait_token);
                    continue;
                },
                .claimed => {},
            }
            state.detachFdWaitGroup(target.group, target.wait_token);
            if (target.recv_memory.active) {
                state.cancelFdWaitGroup(target.group, target.wait_token);
                // The token claim precedes dequeue.  A timeout/signal winner can
                // therefore never consume a message through a stale waiter.
                const status = if (state.ipcRecvFdMatchesRegistration(
                    target.owner,
                    target.recv_fd,
                    target.registration,
                ))
                    completeRecvWaiter(h, state, target)
                else
                    sc.syscall_err_closed;
                const published = scheduler.publishClaimedWaitCompletionPreferred(
                    target.thread_index,
                    target.thread_generation,
                    target.owner,
                    target.wait_token,
                    status,
                    scheduler.currentCpu(),
                );
                if (published == .published and status == sc.syscall_ok) {
                    if (handoff_target) |selected| {
                        if (selected.* == null) selected.* = target;
                    }
                }
            } else {
                const result = fd_syscall.copyClaimedFdWaitGroup(h, state, target);
                const copy_ok = result.copy_ok;
                const published = scheduler.publishClaimedWaitCompletionPreferred(
                    target.thread_index,
                    target.thread_generation,
                    target.owner,
                    target.wait_token,
                    result.status,
                    scheduler.currentCpu(),
                );
                if (published == .published and copy_ok) {
                    if (handoff_target) |selected| {
                        if (selected.* == null) selected.* = target;
                    }
                }
                if (!copy_ok) all_copies_ok = false;
            }
        }
    }
    return all_copies_ok;
}

fn recvWait(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame, input: TrapFrame) u64 {
    if (frame.rdi > std.math.maxInt(kernel.Fd)) return sc.syscall_err_invalid;
    return recvWaitArgs(h, state, proc, frame, @intCast(frame.rdi), &input, input.rdx, input.rsi, null);
}

// Message commit/receive registration precedes this hook. The specialized
// switch revalidates both generations and keeps T through save + activation.
fn switchIpc(h: anytype, frame: *TrapFrame, flags: u64, token: u64, timeout: u64, preferred: ?kernel.ThreadWakeTarget, ready_status: ?u64) bool {
    if (ready_status) |status| {
        if (!@import("builtin").is_test) @import("../xstate_runtime.zig").saveIpcLiveIfNeeded();
        const target = preferred orelse return false;
        return scheduler.handoffToReadyThreadGenerationWithRax(frame, target.thread_index, target.thread_generation, status, h.before_current_thread_leave);
    }
    if (preferred) |target| {
        if (scheduler.tryIpcDirectSwitch(frame, (flags & ipc_abi.wait_uninterruptible) != 0, token, timeout, target, h.before_current_thread_leave)) return true;
    }
    // Direct admission rejects claimed/published completion under T. Only a
    // failed direct attempt needs a separate completed-wait acquisition of T.
    if (scheduler.tryIpcCompletedWait(frame, token)) return true;
    // The generic callback can switch, idle or process cancellation. Save
    // while source still owns this CPU, never after it installs a target.
    if (!@import("builtin").is_test) @import("../xstate_runtime.zig").saveIpcLiveIfNeeded();
    const HooksType = switch (@typeInfo(@TypeOf(h))) {
        .pointer => |p| p.child,
        else => @TypeOf(h),
    };
    if (@hasField(HooksType, "block_current_thread_for_event_preferred"))
        return h.block_current_thread_for_event_preferred(frame, true, (flags & ipc_abi.wait_uninterruptible) != 0, token, timeout, sc.syscall_err_not_ready, h.before_current_thread_leave, preferred);
    return h.block_current_thread_for_event(frame, true, (flags & ipc_abi.wait_uninterruptible) != 0, token, timeout, sc.syscall_err_not_ready, h.before_current_thread_leave);
}

fn fastMiss(reason: ipc_metric.Op) ?u64 {
    ipc_metric.recordElapsed(reason, 0);
    return null;
}
// B3 admission counters describe CALL/REPLY_RECV, excluding SEND.
fn fastMissFor(frame: *const TrapFrame, reason: ipc_metric.Op) ?u64 {
    return if (frame.rax == sc.syscall_ipc_send) null else fastMiss(reason);
}
// Reject register-only misses before acquiring K at the live-XState entry.
// FD transfers and finite waits must not pay for two global lock acquisitions.
pub fn fastRegistersAdmit(frame: *const TrapFrame) bool {
    const reason: ?ipc_metric.Op = if (ipc_abi.sendCount(frame) != 0) .fast_extension else if ((frame.rsi & ~ipc_abi.wait_uninterruptible) != 0) .fast_flags else if (frame.rdx != (if (frame.rax == sc.syscall_ipc_send) @as(u64, 0) else fd_abi.wait_forever)) .fast_timeout else if (frame.rdi > std.math.maxInt(kernel.Fd)) .fast_authority else null;
    if (reason) |miss| {
        _ = fastMissFor(frame, miss);
        return false;
    }
    return true;
}

pub fn fastDispatch(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    const is_call = frame.rax == sc.syscall_ipc_call;
    const is_send = frame.rax == sc.syscall_ipc_send;
    if (!is_send and !is_call and frame.rax != sc.syscall_ipc_reply_recv) return null;
    if (!fastRegistersAdmit(frame)) return null;
    return fastDispatchAfterRegisterAdmission(h, state, proc, frame);
}

// Caller has checked opcode and fastRegistersAdmit on this unchanged local
// frame. K retains borrowed resources; the endpoint Pair covers
// capability validation and FIFO commit before any switch or fallback.
pub fn fastDispatchAfterRegisterAdmission(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    const is_call = frame.rax == sc.syscall_ipc_call;
    const is_send = frame.rax == sc.syscall_ipc_send;
    // FD_WAIT_MANY cancellation belongs to generic admission. A retained
    // group may still refer to the caller's poll array after timeout/signal;
    // do not skip that cleanup just because the scheduler token is now zero.
    const tid = scheduler.currentThread();
    if (tid <= std.math.maxInt(u32) and state.fd_wait_group_thread_counts.get(@intCast(tid)) != null)
        return fastMissFor(frame, .fast_retained_fd_wait);
    const receive = readReceive(frame) orelse return fastMissFor(frame, .fast_state);
    const fd: kernel.Fd = @intCast(frame.rdi);
    const lookup_start = b3.now();
    const borrowed = @import("../state/fd_capability.zig").ipcUnderK(state, proc, fd, if (is_call) .{ .call = true } else if (is_send) .{ .send = true } else .{ .recv = true, .wait = true }, is_call or is_send) orelse return fastMissFor(frame, .fast_authority);
    const exclusion = borrowed.list.exclusion.?;
    const generation = scheduler.currentThreadGeneration() orelse return fastMissFor(frame, .fast_state);
    const call_block: ?*kernel.IpcReplySlot = if (is_call)
        scheduler.ipcBlock(tid, generation, proc)
    else if (!is_send) blk: {
        const implicit = scheduler.peekIpcImplicit(tid, generation, proc) orelse return fastMissFor(frame, .fast_state);
        break :blk @ptrCast(@alignCast(implicit.context.?));
    } else null;
    if (!is_send and call_block == null) return fastMissFor(frame, .fast_state);
    var endpoint_guard = @import("../sync/ipc_endpoint.zig").Pair.acquire(exclusion, if (call_block) |body| &body.exclusion else null);
    defer endpoint_guard.end();
    const view = .{ .payload = &borrowed.payload };
    const resolved = .{ .queue = borrowed.queue, .list = borrowed.list, .hint = borrowed.hint.* };
    const queue = resolved.queue;
    const list = resolved.list;
    if (comptime ipc_metric.enabled) ipc_metric.route(2, frame.rax, proc, (tid << 32) | generation, frame.r15 >> 32, list, resolved.hint);
    if (!queue.isEmpty()) return fastMissFor(frame, .fast_queued);
    const poll_ready = if (list.poll_head) |waiter|
        (is_call or is_send) and list.receive_head == null and waiter.next == null and
            !waiter.recv_memory.active and (waiter.events & fd_abi.event_readable) != 0
    else
        false;
    if (list.poll_head) |waiter| {
        if (!poll_ready) {
            if (comptime ipc_metric.enabled) {
                if (!is_send) ipc_metric.recordElapsed(if (waiter.recv_memory.active) .generic_receive else .generic_poll, 0);
            }
            return fastMissFor(frame, .fast_generic_waiter);
        }
    }
    if (is_send) {
        if (!poll_ready or queue.reference_sequence == std.math.maxInt(u64)) return null;
        // The empty fixed queue has capacity. Endpoint exclusion protects FIFO
        // publication; wake claims T before exposing register results.
        queue.push(.{ .sender = proc, .words = ipc_abi.inputWords(frame) }) catch unreachable;
        endpoint_guard.end();
        // Copyout in the wake callback can fault and enter PMM reclamation.
        // K retains the committed queue; no borrowed pointer is used below.
        _ = wakeIpcWaitersForSendFd(h, state, proc, fd, null);
        ipc_abi.outputWords(frame, .{ 0, 0, 0, 0 });
        frame.r15 = 0;
        frame.rdx = 0;
        return sc.syscall_ok;
    }
    b3.end(101, lookup_start);
    const flags = frame.rsi;
    const delivery_start = b3.now();
    var preferred: ?kernel.ThreadWakeTarget = null;
    var queued = false;
    const token = if (is_call) blk: {
        const receiver = list.receive_head orelse {
            preferred = if (resolved.hint.active) .{ .owner = resolved.hint.owner, .thread_index = resolved.hint.thread_index, .thread_generation = resolved.hint.thread_generation } else null;
            const key = kernel.KernelState.ipcRecvWaitKeyFromSendPayload(view.payload).?;
            const token = scheduler.tryIpcFastEnqueue(proc, queue, key, ipc_abi.inputWords(frame), receive.memory, receive.capacity) orelse {
                if (comptime ipc_metric.enabled) ipc_metric.recordElapsed(scheduler.ipcReceiverAbsence(preferred), 0);
                return fastMissFor(frame, .fast_no_receiver);
            };
            queued = true;
            // K excludes other readiness writers. Exactly one poll node on
            // this endpoint yields one ready entry, even for a large group.
            // Timeout/signal can win T, in which case wake only cancels it.
            endpoint_guard.end();
            // The committed binding and K retain all wake inputs. The
            // borrowed pointer is no longer used after this callback.
            if (poll_ready) _ = wakeIpcWaitersForSendFd(h, state, proc, fd, &preferred);
            break :blk token;
        };
        const target = receiver.receive_target;
        if (!state.ipcRecvFdMatchesRegistration(target.owner, target.recv_fd, target.registration)) return fastMissFor(frame, .fast_authority);
        const token = scheduler.tryIpcFastCall(proc, receiver, ipc_abi.inputWords(frame), receive.memory, receive.capacity) orelse return fastMissFor(frame, .fast_state);
        endpoint_guard.end();
        if (comptime ipc_metric.enabled) _ = state.noteIpcReceiverForFd(target.owner, target.recv_fd, target.thread_index, target.thread_generation);
        preferred = target;
        break :blk token;
    } else blk: {
        const key = kernel.KernelState.ipcRecvWaitKeyFromPayload(view.payload).?;
        const registration: kernel.FdWaitRegistrationRef = .{ .kind = if (key.kind == .endpoint) .endpoint else .channel, .index = key.index, .generation = key.generation, .side = key.side };
        const committed = scheduler.tryIpcFastReplyRecv(proc, list, registration, fd, ipc_abi.inputWords(frame), receive.memory, receive.capacity) orelse return fastMissFor(frame, .fast_state);
        preferred = committed.target;
        endpoint_guard.end();
        state.releaseIpcCallReference(committed.reply);
        state.releaseIpcCallReference(committed.reply);
        break :blk committed.token;
    };
    endpoint_guard.end();
    b3.end(504, delivery_start);
    ipc_metric.recordElapsed(.fast_success, 0);
    ipc_metric.recordElapsed(if (queued) .queued_success else .direct_delivery_success, 0);
    frame.rdx = 1;
    ipc_abi.outputWords(frame, .{ 0, 0, 0, 0 });
    frame.r15 = 0;
    // No borrowed pointer is used after this boundary: the AP idle tail can
    // abandon the stack, and only the committed binding retains ownership.
    // No slow syscall replay is permitted after commit, even if the source
    // is cancelled during block admission. Cleanup uses its retained token.
    if (switchIpc(h, frame, flags, token, 0, preferred, null)) return frame.rax;
    scheduler.cancelCurrentWaitToken(generation, token);
    state.drainIpcCancellations(h.free_list);
    return sc.syscall_err_not_ready;
}

fn recvWaitArgs(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame, fd: kernel.Fd, input: *const TrapFrame, timeout_ticks: u64, flags: u64, preferred: ?kernel.ThreadWakeTarget) u64 {
    const metric_start = ipc_metric.timestamp();
    const metric_direction = ipc_metric.syscallDirection(frame.rax);
    var metric_active = true;
    const previous = ipc_metric.enter(metric_direction);
    defer if (metric_active) {
        ipc_metric.phase(metric_direction, .register, metric_start);
        _ = ipc_metric.enter(previous);
    };
    if ((flags & ~ipc_abi.wait_uninterruptible) != 0) return sc.syscall_err_invalid;
    const receive = readReceive(input) orelse return sc.syscall_err_invalid;
    const fd_capacity_u8 = receive.capacity;

    const current_thread = scheduler.currentThread();
    const current_generation = scheduler.currentThreadGeneration() orelse return sc.syscall_err_invalid;
    const immediate = recvMessageToUser(
        h,
        state,
        proc,
        fd,
        receive.memory,
        fd_capacity_u8,
        current_thread,
        current_generation,
        0,
        frame,
    );
    if (immediate != sc.syscall_err_empty) return immediate;
    // Drain queued messages before reporting peer death. RECV_WAIT needs
    // recv/wait authority, not the unrelated POLL right.
    if (state.fdPayloadWithRightsConst(proc, fd, .{ .recv = true })) |view| {
        if (view.payload.* == .channel) {
            const channel = state.ipcChannelSlotConst(view.payload.channel.channel) orelse
                return sc.syscall_err_invalid;
            var peer_guard = @constCast(channel).exclusion.acquire();
            defer peer_guard.end();
            if (channel.ref_count == 1) return sc.syscall_err_closed;
        }
    }
    if (timeout_ticks == 0) return sc.syscall_err_empty;

    state.drainIpcCancellations(h.free_list);
    state.unregisterFdWaitersForThread(proc, current_thread, current_generation);
    const wait_token = scheduler.reserveCurrentWaitToken(current_generation) orelse
        return sc.syscall_err_invalid;
    state.registerIpcDirectReceive(proc, fd, receive.memory, fd_capacity_u8, current_thread, current_generation, wait_token) catch |err| {
        scheduler.cancelCurrentWaitToken(current_generation, wait_token);
        return mapError(err);
    };
    ipc_metric.phase(metric_direction, .register, metric_start);
    _ = ipc_metric.enter(previous);
    metric_active = false;
    const block_ticks = if (timeout_ticks == fd_abi.wait_forever) 0 else timeout_ticks;
    // The completed caller is only offered after registration and arm. The
    // scheduler commits this thread's block before selecting that generation.
    if (switchIpc(h, frame, flags, wait_token, block_ticks, preferred, null)) {
        return frame.rax;
    }
    scheduler.cancelCurrentWaitToken(current_generation, wait_token);
    state.drainIpcCancellations(h.free_list);
    return sc.syscall_err_not_ready;
}

// Doorbells are notify-only: the channel capability authenticates the peer and
// the shared ring carries the work.  A message doorbell paid a header copy-in,
// queue push/pop and three copy-outs into the receiver; a VMO futex could not
// sit in FD_WAIT_MANY beside other FDs or report peer death, and an eventfd
// would need a write plus a WAIT_MANY and read with no directed handoff.
/// `handoff_frame` is the sender's frame when it may hand this CPU to the
/// woken waiter right away (IPC_NOTIFY), null when the sender blocks next
/// (NOTIFY_WAIT with notify_peer). After a handoff the frame holds the
/// waiter's context and `handed_off` is set.
fn notifyPeer(
    h: anytype,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    fd: kernel.Fd,
    handoff_frame: ?*TrapFrame,
    handoff_target: *?kernel.ThreadWakeTarget,
    handed_off: *bool,
) u64 {
    handoff_target.* = null;
    handed_off.* = false;
    @import("../notify_metric.zig").sending(true);
    defer @import("../notify_metric.zig").sending(false);
    const b8_start = @import("../ipc_metric.zig").timestamp();
    defer @import("../notify_metric.zig").segment(3, b8_start);
    const result = state.ipcNotifyPeerForFd(proc, fd) catch |err| return mapError(err);
    if (result.closed) return sc.syscall_err_closed;
    if (result.waiter) |target| {
        @import("../notify_metric.zig").target(target, 1);
        const completion: kernel.IpcNotifyCompletion = if (handoff_frame) |frame|
            switch (scheduler.completeIpcNotifyWaiterAndHandoff(frame, target.thread_index, target.thread_generation, target.owner, target.wait_token, sc.syscall_ok, sc.syscall_ok)) {
                .handed_off => .handed_off,
                .published => .published,
                .pending => .pending,
                .stale => .stale,
            }
        else switch (fd_syscall.completeIpcNotifyWaiter(target, sc.syscall_ok)) {
            .published => .published,
            .pending => .pending,
            .stale => .stale,
        };
        @import("../notify_metric.zig").outcome(1 + @intFromEnum(completion));
        state.settleIpcPeerNotifyForFd(proc, fd, completion);
        switch (completion) {
            .handed_off => {
                handed_off.* = true;
                return sc.syscall_ok;
            },
            .published => {
                handoff_target.* = target;
                return sc.syscall_ok;
            },
            // Scheduler-owned retry will run it with OK.
            .pending => return sc.syscall_ok,
            // The waiter vanished after detach; the latch was left for the
            // next NOTIFY_WAIT or WAIT_MANY on that side.
            .stale => {},
        }
    }
    // Message RECV_WAIT registrations stay armed: a notification is not one.
    if (!wakeIpcWaitersForSendFdMode(true, h, state, proc, fd, handoff_target)) return sc.syscall_err_invalid;
    if (handoff_target.*) |target| {
        @import("../notify_metric.zig").target(target, 2);
    } else {
        handoff_target.* = state.ipcReceiverHandoffHintForSendFd(proc, fd);
        if (handoff_target.*) |target| @import("../notify_metric.zig").target(target, 3);
    }
    return sc.syscall_ok;
}

fn notifyWait(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const b8_start = @import("../ipc_metric.zig").timestamp();
    defer @import("../notify_metric.zig").segment(4, b8_start);
    const fd: kernel.Fd = @intCast(frame.rdi);
    // Profile-only: a wait with no outgoing notification still needs the
    // producer's last known incarnation/CPU. This hint is observational, not
    // a claim that the producer is currently waiting or owns this CPU forever.
    if (comptime ipc_metric.enabled) {
        if (state.ipcReceiverHandoffHintForSendFd(proc, fd)) |peer|
            @import("../notify_metric.zig").target(peer, 3);
    }
    const timeout_ticks = frame.rsi;
    const flags = frame.rdx;
    if ((flags & ~ipc_abi.notify_wait_known_flags) != 0) return sc.syscall_err_invalid;
    if ((flags & ipc_abi.notify_wait_flag_notify_peer) != 0) {
        // No handoff: this thread blocks next, and the woken peer is already
        // queued on this CPU by the preferred publication.
        var ignored: ?kernel.ThreadWakeTarget = null;
        var ignored_handoff = false;
        const status = notifyPeer(h, state, proc, fd, null, &ignored, &ignored_handoff);
        if (status != sc.syscall_ok) return status;
    }
    const current_thread = scheduler.currentThread();
    const current_generation = scheduler.currentThreadGeneration() orelse return sc.syscall_err_invalid;
    const consumed = state.consumeIpcNotifyForFd(proc, fd, timeout_ticks != 0) catch |err| return mapError(err);
    switch (consumed) {
        .consumed => {
            @import("../notify_metric.zig").waitOutcome(1);
            // Senders hand off through this hint while the receiver is
            // runnable and not parked in the slot.
            _ = state.noteIpcReceiverForFd(proc, fd, current_thread, current_generation);
            return sc.syscall_ok;
        },
        .closed => return sc.syscall_err_closed,
        .empty => {},
    }
    if (timeout_ticks == 0) {
        @import("../notify_metric.zig").waitOutcome(2);
        return sc.syscall_err_empty;
    }
    // One elected receiver per doorbell. A live foreign waiter is a caller
    // protocol error; one left by an ended wait is overwritten.
    if (state.ipcNotifyWaiterForFd(proc, fd)) |existing| {
        if ((existing.thread_index != current_thread or existing.thread_generation != current_generation) and
            scheduler.waitTokenIsCurrent(existing.owner, existing.thread_index, existing.thread_generation, existing.wait_token))
        {
            return sc.syscall_err_alloc;
        }
    }
    const wait_token = scheduler.reserveCurrentWaitToken(current_generation) orelse
        return sc.syscall_err_invalid;
    state.registerIpcNotifyWaiterForFd(proc, fd, current_thread, current_generation, wait_token) catch |err| {
        scheduler.cancelCurrentWaitToken(current_generation, wait_token);
        return mapError(err);
    };
    _ = state.noteIpcReceiverForFd(proc, fd, current_thread, current_generation);
    // Registration and block stay under the one global-state critical section;
    // the lock drops only in before_current_thread_leave after the thread is
    // marked blocked, so a NOTIFY on another CPU cannot claim it earlier.
    const block_ticks = if (timeout_ticks == fd_abi.wait_forever) 0 else timeout_ticks;
    @import("../notify_metric.zig").waitOutcome(3);
    if (h.block_current_thread_for_event(frame, true, (flags & ipc_abi.wait_uninterruptible) != 0, wait_token, block_ticks, sc.syscall_err_not_ready, h.before_current_thread_leave)) {
        return frame.rax;
    }
    scheduler.cancelCurrentWaitToken(current_generation, wait_token);
    state.unregisterIpcNotifyWaiterForFd(proc, fd, current_thread, current_generation, wait_token);
    return sc.syscall_err_not_ready;
}

fn maybeHandoffToRecvWaiter(
    h: anytype,
    frame: *TrapFrame,
    handoff_target: ?kernel.ThreadWakeTarget,
    sender_rax: u64,
) ?u64 {
    const target = handoff_target orelse {
        return null;
    };
    // The handoff itself checks generation, readiness and CPU ownership under
    // the thread table; a separate liveness probe only retook it.
    const start = ipc_metric.timestamp();
    defer ipc_metric.record(.handoff, start);
    if (!switchIpc(h, frame, 0, 0, 0, target, sender_rax)) return null;
    return frame.rax;
}

fn maybeHandoffNotify(h: anytype, frame: *TrapFrame, handoff_target: ?kernel.ThreadWakeTarget, status: u64) ?u64 {
    const target = handoff_target orelse return null;
    if (!@import("builtin").is_test) @import("../xstate_runtime.zig").saveIpcLiveIfNeeded();
    if (!scheduler.handoffNotifyToReadyThreadGenerationWithRax(frame, target.thread_index, target.thread_generation, status, h.before_current_thread_leave)) return null;
    return frame.rax;
}

const DirectResult = struct { status: u64, ref: kernel.IpcReplyRef = .{}, target: ?kernel.ThreadWakeTarget = null };

fn directSend(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, msg: kernel.IpcSendMessage, caller: ?kernel.ThreadWakeTarget) ?DirectResult {
    // The register payload and attachment list remain stack-local; waiting peers
    // bypass queue assembly and use the same install/MOVE rules as dequeue.
    var prepared = blk_metric: {
        const metric_start = ipc_metric.timestamp();
        const previous = ipc_metric.enter(.request);
        defer _ = ipc_metric.enter(previous);
        defer ipc_metric.phase(.request, .build, metric_start);
        break :blk_metric state.buildIpcMessage(proc, msg, h.free_list);
    } catch |err| return .{ .status = mapError(err) };
    defer state.releaseIpcMessageWithFreeList(&prepared, h.free_list);
    while (true) {
        const b3_peer = b3.now();
        const target = (state.ipcTakeDirectReceiver(proc, fd, caller != null) catch |err| return .{ .status = mapError(err) }) orelse return null;
        b3.end(102, b3_peer);
        if (blk_metric: {
            const metric_start = ipc_metric.timestamp();
            const previous = ipc_metric.enter(.request);
            defer _ = ipc_metric.enter(previous);
            defer ipc_metric.phase(.request, .claim, metric_start);
            break :blk_metric scheduler.claimWaitTokenCompletion(target.thread_index, target.thread_generation, target.owner, target.wait_token);
        } == .stale) {
            state.cancelFdWaitGroup(target.group, target.wait_token);
            continue;
        }
        const b3_detach = b3.now();
        state.detachFdWaitGroup(target.group, target.wait_token);
        state.cancelFdWaitGroup(target.group, target.wait_token);
        b3.end(103, b3_detach);
        var ref: kernel.IpcReplyRef = .{};
        var status: u64 = sc.syscall_ok;
        if (!state.ipcRecvFdMatchesRegistration(target.owner, target.recv_fd, target.registration) or
            (caller != null and scheduler.peekIpcImplicit(target.thread_index, target.thread_generation, target.owner) != null))
        {
            status = sc.syscall_err_closed;
        }
        if (status == sc.syscall_ok) {
            if (caller) |identity| {
                ref = state.ipcCreateCall(identity) catch |err| blk: {
                    status = mapError(err);
                    break :blk .{};
                };
            }
        }
        var received: ?kernel.IpcRecvResult = null;
        if (status == sc.syscall_ok) {
            received = blk_metric: {
                const metric_start = ipc_metric.timestamp();
                const previous = ipc_metric.enter(.request);
                defer _ = ipc_metric.enter(previous);
                defer ipc_metric.phase(.request, .install, metric_start);
                break :blk_metric state.ipcInstallMessage(target.owner, &prepared, target.recv_fd_capacity, first_dynamic_fd, h.free_list);
            } catch |err| blk: {
                status = mapError(err);
                break :blk null;
            };
        }
        if (received) |result| {
            // Installation/dequeue commits the send. A later copy fault has
            // the same MOVE ownership as the queued receive path.
            state.closeMovedIpcSendFdsWithFreeList(proc, msg.fds, h.free_list);
            status = blk_metric: {
                const metric_start = ipc_metric.timestamp();
                const previous = ipc_metric.enter(.request);
                defer _ = ipc_metric.enter(previous);
                defer ipc_metric.phase(.request, .copy, metric_start);
                break :blk_metric writeRecvMessage(h, target, null, result);
            };
        } else {
            const allocation_failed = caller != null and ref.isNull() and status == sc.syscall_err_alloc;
            if (!ref.isNull()) {
                // No request was published. Dropping the provisional authority
                // must not claim the caller's still-live reserved wait token.
                state.releaseIpcCallReference(ref);
                state.releaseIpcCallReference(ref);
            }
            {
                const metric_start = ipc_metric.timestamp();
                _ = scheduler.publishClaimedWaitCompletionPreferred(target.thread_index, target.thread_generation, target.owner, target.wait_token, status, scheduler.currentCpu());
                ipc_metric.phase(.request, .complete, metric_start);
            }
            if (allocation_failed) return .{ .status = status };
            // A queued receive leaves its request intact when FD admission
            // fails. Try another receiver or enqueue without consuming MOVE.
            continue;
        }
        if (status == sc.syscall_ok and !ref.isNull()) {
            const body = state.ipcReplySlot(ref).?;
            if (!scheduler.bindIpcImplicit(target.thread_index, target.thread_generation, target.owner, &body.implicit)) status = sc.syscall_err_closed;
        }
        if (status != sc.syscall_ok) {
            if (received) |result| for (result.fds[0..result.fd_count]) |item| {
                state.closeFdWithFreeList(target.owner, item.fd, h.free_list) catch {};
            };
            if (!ref.isNull()) {
                state.releaseIpcCallAuthority(ref);
                state.releaseIpcCallReference(ref);
                ref = .{};
            }
        } else _ = state.noteIpcReceiverForFd(target.owner, target.recv_fd, target.thread_index, target.thread_generation);
        {
            const metric_start = ipc_metric.timestamp();
            _ = scheduler.publishClaimedWaitCompletionPreferred(target.thread_index, target.thread_generation, target.owner, target.wait_token, status, scheduler.currentCpu());
            ipc_metric.phase(.request, .complete, metric_start);
        }
        return .{ .status = status, .ref = ref, .target = target };
    }
}

fn callSync(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame, input: TrapFrame) u64 {
    const timeout = input.rdx;
    const flags = input.rsi;
    if (input.rdi > std.math.maxInt(kernel.Fd) or (flags & ~ipc_abi.wait_uninterruptible) != 0) return sc.syscall_err_invalid;
    const receive = readReceive(&input) orelse return sc.syscall_err_invalid;
    var fd_storage: [kernel.max_ipc_message_fds]kernel.IpcSendFd = undefined;
    const msg = readIpcMessage(h, proc, &input, &fd_storage) orelse return sc.syscall_err_invalid;
    const b3_fd = b3.now();
    const endpoint = state.fdPayloadWithRightsConst(proc, @intCast(frame.rdi), .{ .call = true }) orelse return sc.syscall_err_invalid;
    b3.end(101, b3_fd);
    if (endpoint.payload.* == .channel) {
        const channel = state.ipcChannelSlotConst(endpoint.payload.channel.channel) orelse return sc.syscall_err_invalid;
        var peer_guard = @constCast(channel).exclusion.acquire();
        defer peer_guard.end();
        if (channel.ref_count <= 1) return sc.syscall_err_closed;
    }
    const index = scheduler.currentThread();
    const generation = scheduler.currentThreadGeneration() orelse return sc.syscall_err_invalid;
    _ = state.ensureIpcBlock(proc, index, generation) catch |err| return mapError(err);
    const token = scheduler.reserveCurrentWaitToken(generation) orelse return sc.syscall_err_invalid;
    const caller: kernel.ThreadWakeTarget = .{ .owner = proc, .thread_index = index, .thread_generation = generation, .wait_token = token, .recv_memory = receive.memory, .recv_fd_capacity = receive.capacity };
    const fd: kernel.Fd = @intCast(frame.rdi);
    const direct = directSend(h, state, proc, fd, msg, caller);
    const ref = if (direct) |result| blk: {
        if (result.target != null) frame.rdx = 1;
        if (result.status != sc.syscall_ok) {
            scheduler.cancelCurrentWaitToken(generation, token);
            return result.status;
        }
        break :blk result.ref;
    } else state.ipcCall(proc, fd, msg, caller, h.free_list) catch |err| {
        scheduler.cancelCurrentWaitToken(generation, token);
        return mapError(err);
    };
    frame.rdx = 1;
    const body = state.ipcReplySlot(ref).?;
    if (!scheduler.bindIpcOutgoing(index, generation, proc, token, &body.outgoing)) {
        state.ipcCancelCall(ref, h.free_list);
        state.releaseIpcCallReference(ref);
        scheduler.cancelCurrentWaitToken(generation, token);
        return sc.syscall_err_invalid;
    }
    // Completion during wake may retire both published references. Keep a
    // local K-owned reference until the terminal result has been inspected.
    body.retain(1);
    // Publication consumes MOVE descriptors even when the later wait fails.
    frame.rdx = 1;
    if (direct == null) _ = wakeIpcWaitersForSendFd(h, state, proc, fd, null);
    // Dequeue/copy failure can complete a reserved token before block. Its
    // terminal marker prevents sleeping after the authority was discarded.
    const sent = blk: {
        var reply_guard = body.exclusion.acquire();
        defer reply_guard.end();
        break :blk body.sent;
    };
    if (sent) {
        scheduler.cancelCurrentWaitToken(generation, token);
        if (scheduler.detachIpcOutgoing(index, generation, proc, &body.outgoing)) state.releaseIpcCallReference(ref);
        state.releaseIpcCallReference(ref);
        return sc.syscall_err_closed;
    }
    state.releaseIpcCallReference(ref);
    if (timeout != 0 and switchIpc(h, frame, flags, token, if (timeout == fd_abi.wait_forever) 0 else timeout, if (direct) |d| d.target else null, null)) return frame.rax;
    scheduler.cancelCurrentWaitToken(generation, token);
    if (scheduler.detachIpcOutgoing(index, generation, proc, &body.outgoing)) {
        state.ipcCancelCall(ref, h.free_list);
        state.releaseIpcCallReference(ref);
    }
    return sc.syscall_err_not_ready;
}

const ReplyResult = struct { status: u64, committed: bool = false, target: ?kernel.ThreadWakeTarget = null };

fn sendReply(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: u64, input: *const TrapFrame) ReplyResult {
    var storage: [kernel.max_ipc_message_fds]kernel.IpcSendFd = undefined;
    const msg = readIpcMessage(h, proc, input, &storage) orelse return .{ .status = sc.syscall_err_invalid };
    const index = scheduler.currentThread();
    const generation = scheduler.currentThreadGeneration() orelse return .{ .status = sc.syscall_err_invalid };
    const implicit = fd == ipc_abi.implicit_reply;
    const b3_peer = b3.now();
    const body: *kernel.IpcReplySlot = if (implicit) blk: {
        const node = scheduler.peekIpcImplicit(index, generation, proc) orelse return .{ .status = sc.syscall_err_invalid };
        break :blk @ptrCast(@alignCast(node.context.?));
    } else blk: {
        if (fd > std.math.maxInt(kernel.Fd)) return .{ .status = sc.syscall_err_invalid };
        const view = state.fdPayloadWithRightsConst(proc, @intCast(fd), .{ .send = true }) orelse return .{ .status = sc.syscall_err_invalid };
        if (view.payload.* != .reply) return .{ .status = sc.syscall_err_invalid };
        break :blk state.ipcReplySlot(view.payload.reply) orelse return .{ .status = sc.syscall_err_invalid };
    };
    b3.end(102, b3_peer);
    const ref = body.ref;
    const target = body.caller;
    // Validate and retain attachments before claiming. A rejected sender keeps
    // MOVE ownership and the server can retry a corrected reply.
    var prepared = blk_metric: {
        const metric_start = ipc_metric.timestamp();
        const previous = ipc_metric.enter(.reply);
        defer _ = ipc_metric.enter(previous);
        defer ipc_metric.phase(.reply, .build, metric_start);
        break :blk_metric state.buildIpcMessage(proc, msg, h.free_list);
    } catch |err| return .{ .status = mapError(err) };
    defer state.releaseIpcMessageWithFreeList(&prepared, h.free_list);
    if (!blk_metric: {
        const metric_start = ipc_metric.timestamp();
        const previous = ipc_metric.enter(.reply);
        defer _ = ipc_metric.enter(previous);
        defer ipc_metric.phase(.reply, .claim, metric_start);
        break :blk_metric state.ipcClaimCallReply(ref);
    }) {
        if (implicit and scheduler.takeIpcImplicit(index, generation, proc, &body.implicit)) state.releaseIpcCallAuthority(ref);
        return .{ .status = sc.syscall_err_closed };
    }
    const result = blk_metric: {
        const metric_start = ipc_metric.timestamp();
        const previous = ipc_metric.enter(.reply);
        defer _ = ipc_metric.enter(previous);
        defer ipc_metric.phase(.reply, .install, metric_start);
        break :blk_metric state.ipcInstallMessage(target.owner, &prepared, target.recv_fd_capacity, first_dynamic_fd, h.free_list);
    } catch |err| {
        state.ipcFinishCallReply(ref, mapError(err));
        if (implicit and scheduler.takeIpcImplicit(index, generation, proc, &body.implicit)) state.releaseIpcCallAuthority(ref);
        return .{ .status = mapError(err), .committed = true };
    };
    const status = blk_metric: {
        const metric_start = ipc_metric.timestamp();
        const previous = ipc_metric.enter(.reply);
        defer _ = ipc_metric.enter(previous);
        defer ipc_metric.phase(.reply, .copy, metric_start);
        break :blk_metric writeRecvMessage(h, target, null, result);
    };
    if (status != sc.syscall_ok) {
        for (result.fds[0..result.fd_count]) |item| state.closeFdWithFreeList(target.owner, item.fd, h.free_list) catch {};
    } else state.closeMovedIpcSendFdsWithFreeList(proc, msg.fds, h.free_list);
    {
        const metric_start = ipc_metric.timestamp();
        state.ipcFinishCallReply(ref, status);
        ipc_metric.phase(.reply, .complete, metric_start);
    }
    if (implicit and scheduler.takeIpcImplicit(index, generation, proc, &body.implicit)) state.releaseIpcCallAuthority(ref);
    return .{ .status = status, .committed = true, .target = if (status == sc.syscall_ok) target else null };
}

fn extractReply(state: *kernel.KernelState, proc: kernel.PrincipalId) u64 {
    const index = scheduler.currentThread();
    const generation = scheduler.currentThreadGeneration() orelse return sc.syscall_err_invalid;
    const node = scheduler.peekIpcImplicit(index, generation, proc) orelse return sc.syscall_err_invalid;
    const body: *kernel.IpcReplySlot = @ptrCast(@alignCast(node.context.?));
    // T may transfer the binding to cancellation even while K is held.
    // Take its authority before allocating; the local reference survives a
    // concurrent drain. Failure rebinds only if this server identity is live.
    if (!scheduler.takeIpcImplicit(index, generation, proc, node)) return sc.syscall_err_closed;
    const fd = state.ipcExtractReply(proc, body.ref, first_dynamic_fd) catch |err| {
        if (!scheduler.bindIpcImplicit(index, generation, proc, node)) state.releaseIpcCallAuthority(body.ref);
        return mapError(err);
    };
    return fd;
}

fn replyThenRecv(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame, input: TrapFrame) u64 {
    if (frame.rdi > std.math.maxInt(kernel.Fd)) {
        frame.rdx = 0;
        return sc.syscall_err_invalid;
    }
    const endpoint: kernel.Fd = @intCast(frame.rdi);
    const next = &input;
    const timeout = input.rdx;
    const flags = input.rsi;
    frame.rdx = 0;
    const result = sendReply(h, state, proc, ipc_abi.implicit_reply, &input);
    if (result.committed) frame.rdx = 1;
    if (result.status != sc.syscall_ok) return result.status;
    // RDX is already the reply commitment before the saved context is
    // blocked. Never rewrite it after block replaced frame's owner.
    return recvWaitArgs(h, state, proc, frame, endpoint, next, timeout, flags, result.target);
}

pub fn dispatch(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    if (fastDispatch(h, state, proc, frame)) |result| return result;
    return dispatchAfterScalarMiss(h, state, proc, frame);
}

pub fn dispatchAfterScalarMiss(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    state.drainIpcCancellations(h.free_list);
    const input = frame.*;
    switch (input.rax) {
        sc.syscall_ipc_send, sc.syscall_ipc_recv, sc.syscall_ipc_call, sc.syscall_ipc_reply, sc.syscall_ipc_reply_recv, sc.syscall_ipc_recv_wait => {
            ipc_abi.outputWords(frame, .{ 0, 0, 0, 0 });
            frame.r15 = 0;
            frame.rdx = 0;
            if (input.rdi > std.math.maxInt(kernel.Fd) and !(input.rax == sc.syscall_ipc_reply and input.rdi == ipc_abi.implicit_reply)) return sc.syscall_err_invalid;
        },
        else => {},
    }
    return switch (frame.rax) {
        sc.syscall_ipc_endpoint_create => state.createIpcEndpointFd(
            proc,
            kernel.fdRightsFromBits(frame.rdi),
            kernel.fdFlagsFromBits(@truncate(frame.rsi)),
            first_dynamic_fd,
        ) catch |err| mapError(err),
        sc.syscall_ipc_channel_create => blk: {
            if (frame.rdi == 0) break :blk sc.syscall_err_invalid;
            const pair = state.createIpcChannelPairFds(
                proc,
                kernel.fdRightsFromBits(frame.rsi),
                kernel.fdFlagsFromBits(@truncate(frame.rdx)),
                first_dynamic_fd,
            ) catch |err| break :blk mapError(err);
            if (!h.write_user_u64(proc, frame.rdi, pair.a)) break :blk sc.syscall_err_invalid;
            if (!h.write_user_u64(proc, frame.rdi + 8, pair.b)) break :blk sc.syscall_err_invalid;
            break :blk sc.syscall_ok;
        },
        sc.syscall_ipc_send => blk: {
            var fd_storage: [kernel.max_ipc_message_fds]kernel.IpcSendFd = undefined;
            const msg = readIpcMessage(h, proc, &input, &fd_storage) orelse break :blk sc.syscall_err_invalid;
            const send_fd: kernel.Fd = @intCast(frame.rdi);
            if (directSend(h, state, proc, send_fd, msg, null)) |result| {
                if (result.status != sc.syscall_ok) break :blk result.status;
                if (maybeHandoffToRecvWaiter(h, frame, result.target, sc.syscall_ok)) |rax| break :blk rax;
                break :blk sc.syscall_ok;
            }
            const enqueue_start = ipc_metric.timestamp();
            state.ipcSend(proc, send_fd, msg, h.free_list) catch |err| break :blk mapError(err);
            ipc_metric.record(.send_enqueue, enqueue_start);
            const wake_start = ipc_metric.timestamp();
            var handoff_target: ?kernel.ThreadWakeTarget = null;
            if (!wakeIpcWaitersForSendFd(h, state, proc, send_fd, &handoff_target)) break :blk sc.syscall_err_invalid;
            // Fast ring producers can publish while the receiver is already
            // runnable. No wait token exists then, but its last confirmed
            // receive still identifies a useful handoff without waking it.
            if (handoff_target == null) {
                handoff_target = state.ipcReceiverHandoffHintForSendFd(proc, send_fd);
            }
            ipc_metric.record(.send_wake, wake_start);
            if (maybeHandoffToRecvWaiter(h, frame, handoff_target, sc.syscall_ok)) |handoff_rax| {
                break :blk handoff_rax;
            }
            break :blk sc.syscall_ok;
        },
        sc.syscall_ipc_recv => blk: {
            const receive = readReceive(&input) orelse break :blk sc.syscall_err_invalid;
            const current_thread = scheduler.currentThread();
            const current_generation = scheduler.currentThreadGeneration() orelse
                break :blk sc.syscall_err_invalid;
            break :blk recvMessageToUser(
                h,
                state,
                proc,
                @intCast(frame.rdi),
                receive.memory,
                receive.capacity,
                current_thread,
                current_generation,
                0,
                frame,
            );
        },
        sc.syscall_ipc_recv_wait => recvWait(h, state, proc, frame, input),
        sc.syscall_ipc_notify => blk: {
            var handoff_target: ?kernel.ThreadWakeTarget = null;
            var handed_off = false;
            const status = notifyPeer(h, state, proc, @intCast(frame.rdi), frame, &handoff_target, &handed_off);
            if (status != sc.syscall_ok) break :blk status;
            // The frame is the waiter's now; its rax is the completion status.
            if (handed_off) break :blk frame.rax;
            if (maybeHandoffNotify(h, frame, handoff_target, sc.syscall_ok)) |handoff_rax| {
                break :blk handoff_rax;
            }
            break :blk sc.syscall_ok;
        },
        sc.syscall_ipc_notify_wait => notifyWait(h, state, proc, frame),
        sc.syscall_ipc_call => callSync(h, state, proc, frame, input),
        sc.syscall_ipc_reply => blk: {
            const result = sendReply(h, state, proc, input.rdi, &input);
            frame.rdx = @intFromBool(result.committed);
            if (result.status == sc.syscall_ok) {
                if (maybeHandoffToRecvWaiter(h, frame, result.target, sc.syscall_ok)) |rax| break :blk rax;
            }
            break :blk result.status;
        },
        sc.syscall_ipc_reply_recv => replyThenRecv(h, state, proc, frame, input),
        sc.syscall_ipc_reply_extract => extractReply(state, proc),
        else => null,
    };
}

// Host tests enter the actual frontend without compiling unrelated dispatch arms.
pub const WaitTestSupport = if (@import("builtin").is_test) struct {
    pub const fast = fastDispatch;
    pub fn recv(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
        const input = frame.*;
        ipc_abi.outputWords(frame, .{ 0, 0, 0, 0 });
        frame.r15 = 0;
        frame.rdx = 0;
        return recvWait(h, state, proc, frame, input);
    }
    pub const recvMessage = recvMessageToUser;
    pub fn call(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
        const input = frame.*;
        ipc_abi.outputWords(frame, .{ 0, 0, 0, 0 });
        frame.r15 = 0;
        frame.rdx = 0;
        return callSync(h, state, proc, frame, input);
    }
    pub const reply = sendReply;
    pub const extract = extractReply;
    pub fn replyRecv(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
        const input = frame.*;
        ipc_abi.outputWords(frame, .{ 0, 0, 0, 0 });
        frame.r15 = 0;
        frame.rdx = 0;
        return replyThenRecv(h, state, proc, frame, input);
    }
    pub const readReceive = @import("native_ipc.zig").readReceive;
    pub const readMessage = readIpcMessage;
    pub const directSend = @import("native_ipc.zig").directSend;
    pub const wakeSend = wakeIpcWaitersForSendFd;
    pub const notify = notifyWait;
} else struct {};
