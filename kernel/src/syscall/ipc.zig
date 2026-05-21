const kernel = @import("../kernel.zig");
const interrupts = @import("../interrupts.zig");
const scheduler = @import("../scheduler.zig");
const ipc_runtime = @import("../runtime/ipc.zig");
const x86_platform = @import("../arch/x86_64/platform.zig");
const abi_root = @import("kernel_abi_root");
const ipc_buffer_abi = abi_root.ipc_buffer_abi;
const sc = @import("numbers.zig");

const TrapFrame = interrupts.TrapFrame;

pub const SignalTarget = struct {
    principal: kernel.PrincipalId,
    thread_index: usize,
};

pub const SignalSave = extern struct {
    r15: u64,
    r14: u64,
    r13: u64,
    r12: u64,
    rbp: u64,
    rbx: u64,
    rip: u64,
    rflags: u64,
    rsp: u64,
    mr0: u64,
    mr1: u64,
    mr2: u64,
    mr3: u64,
};

pub const ReplyTargetResult = union(enum) {
    ok: SignalTarget,
    not_ready,
    endpoint,
};

pub const ImmediateResult = union(enum) {
    ready: u64,
    pending,
};

pub const SparseTarget = struct {
    target: SignalTarget,
    ctx: *scheduler.ThreadContext,
    hot: *const scheduler.IpcHotThread,
};

pub const SparseTargetResult = union(enum) {
    ok: SparseTarget,
    no_reply_target,
    no_reply_token,
    no_target,
    no_target_ctx,
    no_target_hot,
    stale_target,
    self_target,
};

pub fn parseIpcBufferRights(bits: u64) ?kernel.IpcBufferRights {
    if ((bits & ~@as(u64, 0xF)) != 0) return null;
    const abi_rights = ipc_buffer_abi.rightsFromBits(bits);
    return @bitCast(abi_rights);
}

pub fn parseIpcBufferRole(value: u64) ?kernel.IpcBufferRole {
    return switch (value) {
        0 => .generic,
        1 => .request,
        2 => .response,
        3 => .bulk,
        else => null,
    };
}

pub fn transferPageCapOnEndpoint(
    state: *kernel.KernelState,
    h: anytype,
    proc: kernel.PrincipalId,
    endpoint_id: u64,
    paddr: u64,
    retain_sender: bool,
) u64 {
    const to = state.endpointTargetFor(proc, endpoint_id) orelse return sc.syscall_err_endpoint;
    if (retain_sender) {
        state.shareCapOnEndpoint(proc, endpoint_id, paddr) catch |err| switch (err) {
            kernel.KernelError.EndpointNotFound => return sc.syscall_err_endpoint,
            kernel.KernelError.CapabilityNotFound => return sc.syscall_err_send,
            else => return sc.syscall_err_send,
        };
    } else {
        state.sendCapOnEndpoint(proc, endpoint_id, paddr) catch |err| switch (err) {
            kernel.KernelError.EndpointNotFound => return sc.syscall_err_endpoint,
            kernel.KernelError.CapabilityNotFound => return sc.syscall_err_send,
            else => return sc.syscall_err_send,
        };
    }
    h.wake_waiting_thread_for_principal(to);
    return sc.syscall_ok;
}

pub fn transferIpcBufferCapOnEndpoint(
    state: *kernel.KernelState,
    h: anytype,
    proc: kernel.PrincipalId,
    endpoint_id: u64,
    token: u64,
    rights_bits: u64,
) u64 {
    const cap_id = ipc_buffer_abi.decodeIpcBufferToken(token) orelse return sc.syscall_err_invalid;
    const to = state.endpointTargetFor(proc, endpoint_id) orelse return sc.syscall_err_endpoint;
    const rights = parseIpcBufferRights(rights_bits) orelse return sc.syscall_err_invalid;
    state.shareIpcBufferCapOnEndpoint(proc, endpoint_id, cap_id, rights) catch |err| switch (err) {
        kernel.KernelError.EndpointNotFound => return sc.syscall_err_endpoint,
        kernel.KernelError.CapabilityNotFound => return sc.syscall_err_send,
        else => return sc.syscall_err_send,
    };
    h.wake_waiting_thread_for_principal(to);
    return sc.syscall_ok;
}

pub fn recvCap(state: *kernel.KernelState, proc: kernel.PrincipalId) u64 {
    const received = state.recvCap(proc) catch |err| switch (err) {
        kernel.KernelError.MailboxEmpty => return sc.syscall_err_empty,
        else => return sc.syscall_err_send,
    };
    return received;
}

pub fn acceptCapTransfer(state: *kernel.KernelState, proc: kernel.PrincipalId, transfer_id: u64) u64 {
    const received = state.acceptCapTransfer(proc, transfer_id) catch |err| switch (err) {
        kernel.KernelError.MailboxEmpty => return sc.syscall_err_empty,
        kernel.KernelError.InvalidState => return sc.syscall_err_invalid,
        kernel.KernelError.CapabilityNotFound => return sc.syscall_err_send,
        kernel.KernelError.TableFull => return sc.syscall_err_alloc,
        else => return sc.syscall_err_send,
    };
    return received;
}

pub fn dispatchCapTransferSyscallLocked(
    state: *kernel.KernelState,
    h: anytype,
    proc: kernel.PrincipalId,
    nr: u64,
    arg0: u64,
    arg1: u64,
) ?u64 {
    return switch (nr) {
        sc.syscall_send_cap => transferPageCapOnEndpoint(state, h, proc, arg1, arg0, false),
        sc.syscall_share_cap => transferPageCapOnEndpoint(state, h, proc, arg1, arg0, true),
        sc.syscall_recv_cap => recvCap(state, proc),
        sc.syscall_accept_cap_transfer => acceptCapTransfer(state, proc, arg0),
        else => null,
    };
}

pub fn isCapTransferSyscall(nr: u64) bool {
    return switch (nr) {
        sc.syscall_send_cap,
        sc.syscall_share_cap,
        sc.syscall_recv_cap,
        sc.syscall_accept_cap_transfer,
        => true,
        else => false,
    };
}

pub fn recvAnyCapTransferImmediate(state: *kernel.KernelState, proc: kernel.PrincipalId) ImmediateResult {
    const received = state.recvAnyCapTransfer(proc) catch |err| switch (err) {
        kernel.KernelError.MailboxEmpty => return .pending,
        else => return .{ .ready = sc.syscall_err_send },
    };
    return if (received >= abi_root.cap_transfer_abi.transfer_id_min)
        .{ .ready = received }
    else
        .pending;
}

pub fn applyQueuedMessageToFrame(frame: *TrapFrame, msg: scheduler.IpcQueuedMessage) void {
    frame.rax = sc.syscall_ok;
    frame.rdi = msg.mr0;
    frame.rsi = msg.mr1;
    frame.rdx = msg.mr2;
    frame.r8 = msg.mr3;
}

pub fn grantQueuedReplyToken(receiver_thread: usize, msg: scheduler.IpcQueuedMessage) void {
    if (!msg.grants_reply) return;
    scheduler.setIpcReplyTokenForThread(receiver_thread, true, msg.sender_thread);
}

pub fn consumeQueuedMessageForThread(thread_index: usize, frame: *TrapFrame) bool {
    if (takeQueuedMessageForThread(thread_index)) |msg| {
        grantQueuedReplyToken(thread_index, msg);
        applyQueuedMessageToFrame(frame, msg);
        return true;
    }
    return false;
}

pub fn consumeQueuedMessageForPrincipal(principal: kernel.PrincipalId, frame: *TrapFrame) bool {
    const thread_index = scheduler.threadSlotForPrincipal(principal) orelse return false;
    return consumeQueuedMessageForThread(thread_index, frame);
}

pub fn waitEventImmediate(
    state: *kernel.KernelState,
    h: anytype,
    proc: kernel.PrincipalId,
    frame: *TrapFrame,
    wait_mailbox: bool,
    preserve_ipc_queue: bool,
) ImmediateResult {
    if (wait_mailbox) {
        switch (recvAnyCapTransferImmediate(state, proc)) {
            .ready => |result| return .{ .ready = result },
            .pending => {},
        }
    }
    if (!preserve_ipc_queue and consumeQueuedMessageForPrincipal(proc, frame)) return .{ .ready = sc.syscall_ok };
    if (h.consume_pending_signal_for_principal(proc)) return .{ .ready = sc.syscall_ok };
    return .pending;
}

pub fn callReplyRecvImmediate(
    state: *kernel.KernelState,
    h: anytype,
    proc: kernel.PrincipalId,
    frame: *TrapFrame,
    signal_only: bool,
) ImmediateResult {
    if (!signal_only) {
        switch (recvAnyCapTransferImmediate(state, proc)) {
            .ready => |result| return .{ .ready = result },
            .pending => {},
        }
    }
    if (consumeQueuedMessageForPrincipal(proc, frame)) return .{ .ready = sc.syscall_ok };
    if (h.consume_pending_signal_for_principal(proc)) return .{ .ready = sc.syscall_ok };
    return .pending;
}

pub fn blockWaitEvent(
    h: anytype,
    frame: *TrapFrame,
    wait_mailbox: bool,
    preserve_ipc_queue: bool,
    timeout_ticks: u64,
) u64 {
    if (preserve_ipc_queue) {
        if (!scheduler.blockCurrentThreadForEventPreservingIpc(frame, timeout_ticks, sc.syscall_ok)) {
            return sc.syscall_err_not_ready;
        }
        return sc.syscall_ok;
    }
    if (!h.block_current_thread_for_event(frame, wait_mailbox, timeout_ticks, sc.syscall_ok)) {
        return sc.syscall_err_not_ready;
    }
    return sc.syscall_ok;
}

pub fn blockCallReplyRecv(h: anytype, frame: *TrapFrame, signal_only: bool) u64 {
    if (!h.block_current_thread_for_event(frame, !signal_only, 0, sc.syscall_ok)) {
        return sc.syscall_err_not_ready;
    }
    return sc.syscall_ok;
}

pub fn blockSparseCurrent(out_frame: *TrapFrame, save: *const SignalSave) bool {
    out_frame.* = trapFrameFromSignalSave(save, sc.syscall_ok);
    return scheduler.blockCurrentThreadForEvent(out_frame, false, 0, sc.syscall_ok);
}

pub fn switchSparseToTarget(
    current_thread: usize,
    current_ctx: *scheduler.ThreadContext,
    target_thread: usize,
    target_ctx: *scheduler.ThreadContext,
    save: *const SignalSave,
) usize {
    saveSignalFrameToContext(current_ctx, save, sc.syscall_ok);
    current_ctx.cr3 = scheduler.currentUserCr3();
    current_ctx.wait_mailbox = false;
    current_ctx.wait_preserve_ipc_queue = false;
    current_ctx.wake_tick = 0;
    current_ctx.ready = false;
    scheduler.setIpcHotCr3(current_thread, scheduler.currentUserCr3());
    scheduler.setIpcHotWaitState(current_thread, false, 0, false);

    _ = scheduler.setCurrentExecutionFromHotThread(target_thread);
    _ = scheduler.applyThreadFsBase(target_thread);
    return @intFromPtr(&target_ctx.frame);
}

pub fn sendCallPayload(
    state: *kernel.KernelState,
    h: anytype,
    proc: kernel.PrincipalId,
    endpoint_id: u64,
    flags: u64,
    mr0: u64,
    mr1: u64,
    mr2: u64,
    mr3: u64,
) ?u64 {
    const signal_only = (flags & sc.ipc_call_flag_signal_only) != 0;
    if (signal_only and endpoint_id == 0) return null;
    if (signal_only) {
        return signalEndpointMessage(
            state,
            proc,
            scheduler.currentThreadIndex(),
            endpoint_id,
            true,
            mr0,
            mr1,
            mr2,
            mr3,
        );
    }
    return transferPageCapOnEndpoint(
        state,
        h,
        proc,
        endpoint_id,
        mr0,
        (flags & sc.ipc_call_flag_retain_sender) != 0,
    );
}

pub fn takeQueuedMessageForThread(thread_index: usize) ?scheduler.IpcQueuedMessage {
    return scheduler.dequeueIpcMessageForThread(thread_index);
}

pub fn trapFrameFromSignalSave(save: *const SignalSave, rax: u64) TrapFrame {
    return .{
        .r15 = save.r15,
        .r14 = save.r14,
        .r13 = save.r13,
        .r12 = save.r12,
        .r11 = save.rflags,
        .r10 = 0,
        .r9 = 0,
        .r8 = 0,
        .rbp = save.rbp,
        .rdi = 0,
        .rsi = 0,
        .rdx = 0,
        .rcx = save.rip,
        .rbx = save.rbx,
        .rax = rax,
        .rip = save.rip,
        .cs = @as(u64, x86_platform.gdt_user_code_selector) | 0x3,
        .rflags = save.rflags,
        .rsp = save.rsp,
        .ss = @as(u64, x86_platform.gdt_user_data_selector) | 0x3,
    };
}

pub fn saveSignalFrameToContext(ctx: *scheduler.ThreadContext, save: *const SignalSave, rax: u64) void {
    ctx.frame.r15 = save.r15;
    ctx.frame.r14 = save.r14;
    ctx.frame.r13 = save.r13;
    ctx.frame.r12 = save.r12;
    ctx.frame.rbp = save.rbp;
    ctx.frame.rcx = save.rip;
    ctx.frame.rbx = save.rbx;
    ctx.frame.rax = rax;
    ctx.frame.rip = save.rip;
    ctx.frame.rflags = save.rflags;
    ctx.frame.rsp = save.rsp;
}

pub fn writeSignalQueuedReturn(out_frame: *TrapFrame, save: *const SignalSave, msg: scheduler.IpcQueuedMessage) usize {
    out_frame.* = trapFrameFromSignalSave(save, sc.syscall_ok);
    grantQueuedReplyToken(scheduler.currentThreadIndex(), msg);
    applyQueuedMessageToFrame(out_frame, msg);
    return @intFromPtr(out_frame);
}

pub fn writeSignalReturn(out_frame: *TrapFrame, save: *const SignalSave, rax: u64) usize {
    out_frame.* = trapFrameFromSignalSave(save, rax);
    return @intFromPtr(out_frame);
}

pub fn clearSignalPendingForThread(thread_index: usize, ctx: *scheduler.ThreadContext) void {
    ctx.signal_pending = false;
    scheduler.setIpcHotSignalPending(thread_index, false);
}

pub fn deliverOrQueueMessageToThread(
    target_thread: usize,
    endpoint_id: u64,
    sender_thread: usize,
    grants_reply: bool,
    mr0: u64,
    mr1: u64,
    mr2: u64,
    mr3: u64,
) u64 {
    return ipc_runtime.deliverOrQueueMessageToThread(target_thread, endpoint_id, sender_thread, grants_reply, mr0, mr1, mr2, mr3);
}

pub fn currentReplyTarget() ReplyTargetResult {
    const current_thread = scheduler.currentThreadIndex();
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return .not_ready;
    if (current_hot.ipc_reply_token_valid == 0) return .endpoint;
    const target_thread = current_hot.ipc_reply_token_target_thread;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return .endpoint;
    if (!target_ctx.allocated) return .endpoint;
    return .{ .ok = .{
        .principal = target_ctx.owner_process,
        .thread_index = target_thread,
    } };
}

pub fn clearCurrentReplyToken() void {
    scheduler.setIpcReplyTokenForThread(scheduler.currentThreadIndex(), false, 0);
}

pub fn replyToTargetFromCurrent(target_thread: usize, mr0: u64, mr1: u64, mr2: u64, mr3: u64) u64 {
    const current_thread = scheduler.currentThreadIndex();
    clearCurrentReplyToken();
    return deliverOrQueueMessageToThread(target_thread, 0, current_thread, false, mr0, mr1, mr2, mr3);
}

pub fn detachCurrentReplyToken() u64 {
    return switch (currentReplyTarget()) {
        .ok => {
            clearCurrentReplyToken();
            return sc.syscall_ok;
        },
        .not_ready => sc.syscall_err_not_ready,
        .endpoint => sc.syscall_err_endpoint,
    };
}

pub fn signalEndpointMessage(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    sender_thread: usize,
    endpoint_id: u64,
    grants_reply: bool,
    mr0: u64,
    mr1: u64,
    mr2: u64,
    mr3: u64,
) u64 {
    const target_principal = state.endpointTargetFor(owner, endpoint_id) orelse return sc.syscall_err_endpoint;
    const target_thread = scheduler.threadSlotForPrincipal(target_principal) orelse return sc.syscall_err_endpoint;
    return deliverOrQueueMessageToThread(
        target_thread,
        endpoint_id,
        sender_thread,
        grants_reply,
        mr0,
        mr1,
        mr2,
        mr3,
    );
}

pub fn resolveSignalTargetThread(
    state: *const kernel.KernelState,
    owner: kernel.PrincipalId,
    endpoint_id: u64,
) ?SignalTarget {
    const generation = state.endpoint_generation;
    const current_thread = scheduler.currentThreadIndex();
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return null;
    if (current_hot.ipc_cached_endpoint_generation == generation and
        current_hot.ipc_cached_endpoint_id == endpoint_id)
    {
        return .{
            .principal = current_hot.ipc_cached_target,
            .thread_index = current_hot.ipc_cached_target_thread,
        };
    }

    const target = state.endpointTargetFor(owner, endpoint_id) orelse return null;
    const thread_index = scheduler.threadSlotForPrincipal(target) orelse return null;
    scheduler.setIpcEndpointCacheForThread(current_thread, generation, endpoint_id, target, thread_index);
    return .{ .principal = target, .thread_index = thread_index };
}

pub fn resolveSparseTarget(
    state: *const kernel.KernelState,
    owner: kernel.PrincipalId,
    current_thread: usize,
    endpoint_id: u64,
) SparseTargetResult {
    const target = if (endpoint_id == 0) blk: {
        break :blk switch (currentReplyTarget()) {
            .ok => |reply_target| reply_target,
            .not_ready => return .no_reply_target,
            .endpoint => return .no_reply_token,
        };
    } else resolveSignalTargetThread(state, owner, endpoint_id) orelse return .no_target;

    const target_ctx = scheduler.getThreadContext(target.thread_index) orelse return .no_target_ctx;
    const target_hot = scheduler.getIpcHotThreadConst(target.thread_index) orelse return .no_target_hot;
    if (target_hot.allocated == 0 or target_hot.owner_process != target.principal) return .stale_target;
    if (target.thread_index == current_thread) return .self_target;

    return .{ .ok = .{
        .target = target,
        .ctx = target_ctx,
        .hot = target_hot,
    } };
}

pub fn fastDispatch(
    state: *kernel.KernelState,
    h: anytype,
    proc: kernel.PrincipalId,
    nr: u64,
    arg0: u64,
    arg1: u64,
    arg2: u64,
) ?u64 {
    return switch (nr) {
        sc.syscall_send_cap => transferPageCapOnEndpoint(state, h, proc, arg1, arg0, false),
        sc.syscall_share_cap => transferPageCapOnEndpoint(state, h, proc, arg1, arg0, true),
        sc.syscall_recv_cap => recvCap(state, proc),
        sc.syscall_accept_cap_transfer => acceptCapTransfer(state, proc, arg0),
        sc.syscall_signal_endpoint => signalEndpointMessage(state, proc, scheduler.currentThreadIndex(), arg0, false, 0, 0, 0, 0),
        sc.syscall_ipc_call_reply_recv, sc.syscall_ipc_call_reply_recv_fast => blk: {
            const endpoint_id = arg1;
            const flags = arg2;
            const to = state.endpointTargetFor(proc, endpoint_id) orelse break :blk sc.syscall_err_endpoint;
            if (to != proc) return null;
            if ((flags & sc.ipc_call_flag_signal_only) != 0) {
                h.wake_blocked_thread_for_principal(to);
                if (h.consume_pending_signal_for_principal(proc)) break :blk sc.syscall_ok;
                break :blk sc.syscall_err_not_ready;
            }
            const status = transferPageCapOnEndpoint(
                state,
                h,
                proc,
                endpoint_id,
                arg0,
                (flags & sc.ipc_call_flag_retain_sender) != 0,
            );
            if (status != sc.syscall_ok) break :blk status;
            break :blk switch (recvAnyCapTransferImmediate(state, proc)) {
                .ready => |result| result,
                .pending => sc.syscall_err_empty,
            };
        },
        else => sc.syscall_err_invalid,
    };
}
