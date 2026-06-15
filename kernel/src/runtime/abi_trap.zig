const kernel = @import("../kernel.zig");
const capability = @import("../capability.zig");
const interrupts = @import("../interrupts.zig");
const scheduler = @import("../scheduler.zig");
const user_vm = @import("../memory/user_vm.zig");
const boot_static = @import("../boot/main_static.zig");
const abi_root = @import("kernel_abi_root");
const ipc = @import("ipc.zig");

const TrapFrame = interrupts.TrapFrame;
const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;
const trap_abi = abi_root.trap_abi;
const process_builder_abi = abi_root.process_builder_abi;

const AbiTrapSpinLock = struct {
    value: u8 = 0,

    fn lock(self: *AbiTrapSpinLock) void {
        while (true) {
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) return;
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                asm volatile ("pause");
            }
        }
    }

    fn unlock(self: *AbiTrapSpinLock) void {
        @atomicStore(u8, &self.value, 0, .release);
    }
};

var dispatch_delegate_lock: AbiTrapSpinLock = .{};

pub const Hooks = struct {
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    user_spaces: []boot_static.UserAddressSpace,
    write: *const fn ([]const u8) void,
    print_hex: *const fn (u64) void,
    print_number: *const fn (u64) void,
    principal_label: *const fn (kernel.PrincipalId) []const u8,
    write_user_u64: *const fn (kernel.PrincipalId, u64, u64) bool,
    copy_user_bytes_from_va: *const fn (kernel.PrincipalId, u64, []u8) bool,
    copy_bytes_to_user_va: *const fn (kernel.PrincipalId, u64, []const u8) bool,
    consume_pending_signal_for_principal: *const fn (kernel.PrincipalId) bool,
    block_current_thread_for_event: *const fn (*TrapFrame, bool, u64, u64) bool,
    exit_current_process: *const fn (kernel.PrincipalId, u8, *TrapFrame) void,
};

var abi_trap_hooks_storage: Hooks = undefined;
var abi_trap_hooks_ready = false;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(dispatch_delegate_lock), &dispatch_delegate_lock));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(abi_trap_hooks_storage), &abi_trap_hooks_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(abi_trap_hooks_ready), &abi_trap_hooks_ready));
    return end;
}

pub fn init(new_hooks: Hooks) void {
    abi_trap_hooks_storage = new_hooks;
    abi_trap_hooks_ready = true;
}

pub fn updateUserSpaces(user_spaces: []boot_static.UserAddressSpace) void {
    if (!abi_trap_hooks_ready) return;
    abi_trap_hooks_storage.user_spaces = user_spaces;
}

fn getHooks() *const Hooks {
    if (!abi_trap_hooks_ready) unreachable;
    return &abi_trap_hooks_storage;
}

fn hooksForState(state: *kernel.KernelState) Hooks {
    var h = getHooks().*;
    h.state = state;
    return h;
}

fn writeRequestU64(h: *const Hooks, target: kernel.PrincipalId, request_page_va: u64, offset: u64, value: u64) bool {
    return h.write_user_u64(target, request_page_va + offset, value);
}

fn writeRequest(
    h: *const Hooks,
    target: kernel.PrincipalId,
    request_page_va: u64,
    caller: kernel.PrincipalId,
    thread_id: u64,
    flavor: u32,
    frame: *const TrapFrame,
    fs_base: u64,
    gs_base: u64,
) bool {
    return writeRequestEx(h, target, request_page_va, caller, thread_id, flavor, @intFromEnum(trap_abi.TrapKind.abi_syscall), 0, 0, frame, fs_base, gs_base);
}

fn writeRequestEx(
    h: *const Hooks,
    target: kernel.PrincipalId,
    request_page_va: u64,
    caller: kernel.PrincipalId,
    thread_id: u64,
    flavor: u32,
    kind: u32,
    fault_addr: u64,
    error_code: u64,
    frame: *const TrapFrame,
    fs_base: u64,
    gs_base: u64,
) bool {
    const version_kind = @as(u64, trap_abi.version) |
        (@as(u64, kind) << 32);
    const flavor_reserved = @as(u64, flavor);

    if (!writeRequestU64(h, target, request_page_va, 0x00, trap_abi.magic)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x08, version_kind)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x10, flavor_reserved)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x18, @intFromEnum(caller))) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x20, thread_id)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x28, frame.rip)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x30, frame.rsp)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x38, fault_addr)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x40, error_code)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x48, frame.rax)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x50, frame.rdi)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x58, frame.rsi)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x60, frame.rdx)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x68, frame.r10)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x70, frame.r8)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x78, frame.r9)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x80, frame.r15)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x88, frame.r14)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x90, frame.r13)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x98, frame.r12)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xA0, frame.r11)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xA8, frame.r10)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xB0, frame.r9)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xB8, frame.r8)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xC0, frame.rbp)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xC8, frame.rdi)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xD0, frame.rsi)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xD8, frame.rdx)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xE0, frame.rcx)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xE8, frame.rbx)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xF0, frame.rax)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0xF8, frame.rflags)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x100, fs_base)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x108, gs_base)) return false;
    return true;
}

fn trapFrameFromExceptionFrame(frame: *const ExceptionTrapFrame) TrapFrame {
    return .{
        .r15 = frame.r15,
        .r14 = frame.r14,
        .r13 = frame.r13,
        .r12 = frame.r12,
        .r11 = frame.r11,
        .r10 = frame.r10,
        .r9 = frame.r9,
        .r8 = frame.r8,
        .rbp = frame.rbp,
        .rdi = frame.rdi,
        .rsi = frame.rsi,
        .rdx = frame.rdx,
        .rcx = frame.rcx,
        .rbx = frame.rbx,
        .rax = frame.rax,
        .rip = frame.rip,
        .cs = frame.cs,
        .rflags = frame.rflags,
        .rsp = frame.rsp,
        .ss = frame.ss,
    };
}

fn writeTrapFrameToExceptionFrame(frame: *ExceptionTrapFrame, trap_frame: TrapFrame) void {
    frame.r15 = trap_frame.r15;
    frame.r14 = trap_frame.r14;
    frame.r13 = trap_frame.r13;
    frame.r12 = trap_frame.r12;
    frame.r11 = trap_frame.r11;
    frame.r10 = trap_frame.r10;
    frame.r9 = trap_frame.r9;
    frame.r8 = trap_frame.r8;
    frame.rbp = trap_frame.rbp;
    frame.rdi = trap_frame.rdi;
    frame.rsi = trap_frame.rsi;
    frame.rdx = trap_frame.rdx;
    frame.rcx = trap_frame.rcx;
    frame.rbx = trap_frame.rbx;
    frame.rax = trap_frame.rax;
    frame.rip = trap_frame.rip;
    frame.cs = trap_frame.cs;
    frame.rflags = trap_frame.rflags;
    frame.rsp = trap_frame.rsp;
    frame.ss = trap_frame.ss;
}

fn sanitizeUserRflags(rflags: u64) u64 {
    const clear_mask = (@as(u64, 3) << 12) | (@as(u64, 1) << 14) | (@as(u64, 1) << 16) | (@as(u64, 1) << 17);
    return (rflags | boot_static.user_entry_rflags | @as(u64, 2)) & ~clear_mask;
}

fn trapFrameFromUserContext(ctx: trap_abi.UserContext) ?TrapFrame {
    if (ctx.rip == 0 or ctx.rsp == 0) return null;
    if (!capability.isUserCanonicalVa(ctx.rip) or !capability.isUserCanonicalVa(ctx.rsp)) return null;
    if (ctx.fs_base != 0 and !capability.isUserCanonicalVa(ctx.fs_base)) return null;
    if (ctx.gs_base != 0 and !capability.isUserCanonicalVa(ctx.gs_base)) return null;
    const frame: TrapFrame = .{
        .r15 = ctx.r15,
        .r14 = ctx.r14,
        .r13 = ctx.r13,
        .r12 = ctx.r12,
        .r11 = ctx.r11,
        .r10 = ctx.r10,
        .r9 = ctx.r9,
        .r8 = ctx.r8,
        .rbp = ctx.rbp,
        .rdi = ctx.rdi,
        .rsi = ctx.rsi,
        .rdx = ctx.rdx,
        .rcx = ctx.rcx,
        .rbx = ctx.rbx,
        .rax = ctx.rax,
        .rip = ctx.rip,
        .cs = @as(u64, boot_static.gdt_user_code_selector) | 0x3,
        .rflags = sanitizeUserRflags(ctx.rflags),
        .rsp = ctx.rsp,
        .ss = @as(u64, boot_static.gdt_user_data_selector) | 0x3,
    };
    return frame;
}

fn findCapLinear(table: *const kernel.CNode, paddr: u64) ?kernel.Capability {
    var index: usize = 0;
    while (index < table.len) : (index += 1) {
        const cap = table.get(index) orelse return null;
        if (cap.paddr == paddr) return cap;
    }
    return null;
}

fn grantQueuedReplyToken(receiver_thread: usize, msg: scheduler.IpcQueuedMessage) void {
    if (!msg.grants_reply) return;
    scheduler.setIpcReplyTokenForThread(receiver_thread, true, msg.sender_thread);
}

pub fn consumeQueuedReplyForPrincipal(principal: kernel.PrincipalId, frame: *TrapFrame) bool {
    const h = getHooks();
    const thread_index = scheduler.threadSlotForPrincipal(principal) orelse return false;
    if (scheduler.dequeueIpcMessageForThread(thread_index)) |msg| {
        grantQueuedReplyToken(thread_index, msg);
        frame.rax = msg.mr0;
        if (msg.mr2 != 0) frame.rip = msg.mr2;
        if (msg.mr3 != 0) frame.rsp = msg.mr3;
        if ((msg.mr1 & trap_abi.response_flag_exit) != 0) {
            h.exit_current_process(principal, @truncate(msg.mr0), frame);
        }
        return true;
    }
    return false;
}

fn consumeQueuedReplyForPrincipalFromSender(h: *const Hooks, principal: kernel.PrincipalId, sender_thread: usize, frame: *TrapFrame) bool {
    const thread_index = scheduler.threadSlotForPrincipal(principal) orelse return false;
    if (scheduler.dequeueIpcReplyMessageForThreadFromSender(thread_index, sender_thread)) |msg| {
        frame.rax = msg.mr0;
        if (msg.mr2 != 0) frame.rip = msg.mr2;
        if (msg.mr3 != 0) frame.rsp = msg.mr3;
        if ((msg.mr1 & trap_abi.response_flag_exit) != 0) {
            h.exit_current_process(principal, @truncate(msg.mr0), frame);
        }
        return true;
    }
    return false;
}

const ResolvedTarget = struct {
    thread: usize,
    ctx: *scheduler.ThreadContext,
    proc: kernel.PrincipalId,
};

fn currentReplyTarget() ?ResolvedTarget {
    const current_thread = scheduler.currentThreadIndex();
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return null;
    if (current_hot.ipc_reply_token_valid == 0) return null;
    const target_thread = current_hot.ipc_reply_token_target_thread;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return null;
    if (!target_ctx.allocated) return null;
    return .{ .thread = target_thread, .ctx = target_ctx, .proc = target_ctx.owner_process };
}

pub fn setCurrentReplyTargetFsBase(fs_base: u64) u64 {
    if (fs_base != 0 and !capability.isUserCanonicalVa(fs_base)) return boot_static.syscall_err_invalid;
    const target = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    if (!scheduler.setThreadFsBase(target.thread, fs_base)) return boot_static.syscall_err_not_ready;
    return boot_static.syscall_ok;
}

pub fn setCurrentReplyTargetGsBase(gs_base: u64) u64 {
    if (gs_base != 0 and !capability.isUserCanonicalVa(gs_base)) return boot_static.syscall_err_invalid;
    const target = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    if (!scheduler.setThreadGsBase(target.thread, gs_base)) return boot_static.syscall_err_not_ready;
    return boot_static.syscall_ok;
}

pub fn copyFromCurrentReplyTarget(proc: kernel.PrincipalId, dst_current_va: u64, src_target_va: u64, len_u64: u64) u64 {
    const h = getHooks();
    const target = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    return copyBetweenPrincipals(h, target.proc, src_target_va, proc, dst_current_va, len_u64);
}

pub fn copyToCurrentReplyTarget(proc: kernel.PrincipalId, dst_target_va: u64, src_current_va: u64, len_u64: u64) u64 {
    const h = getHooks();
    const target = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    return copyBetweenPrincipals(h, proc, src_current_va, target.proc, dst_target_va, len_u64);
}

const TargetResolveUse = enum {
    deferred_reply,
    delegated,
    exit_response,
};

fn targetPrincipalFromRaw(target_raw: u64) ?kernel.PrincipalId {
    if (target_raw > @as(u64, @intCast(@import("std").math.maxInt(usize)))) return null;
    const index: usize = @intCast(target_raw);
    return kernel.processPrincipalFromIndex(index);
}

fn delegateTargetsServer(h: *const Hooks, server_proc: kernel.PrincipalId, target_proc: kernel.PrincipalId) bool {
    const delegate = h.state.abiTrapDelegateFor(target_proc) orelse return false;
    const delegate_target = h.state.endpointTargetFor(target_proc, delegate.endpoint_id) orelse return false;
    return delegate_target == server_proc;
}

fn endpointTableTargetsServer(h: *const Hooks, server_proc: kernel.PrincipalId, target_proc: kernel.PrincipalId) bool {
    const table = h.state.getEndpointTableConst(target_proc);
    const limit = @min(table.len, table.caps.len);
    var i: usize = 0;
    while (i < limit) : (i += 1) {
        if (table.caps[i].target == server_proc) return true;
    }
    return false;
}

fn resolveTarget(h: *const Hooks, server_proc: kernel.PrincipalId, target_raw: u64, use: TargetResolveUse) ?ResolvedTarget {
    const target_proc = targetPrincipalFromRaw(target_raw) orelse return null;
    const target_thread = scheduler.threadSlotForPrincipal(target_proc) orelse return null;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return null;
    if (!target_ctx.allocated) return null;
    const accepted = switch (use) {
        .deferred_reply => target_ctx.abi_trap_reply_pending and delegateTargetsServer(h, server_proc, target_proc),
        .delegated => delegateTargetsServer(h, server_proc, target_proc),
        .exit_response => delegateTargetsServer(h, server_proc, target_proc) or endpointTableTargetsServer(h, server_proc, target_proc),
    };
    if (!accepted) return null;
    return .{ .thread = target_thread, .ctx = target_ctx, .proc = target_proc };
}

fn copyBetweenPrincipals(h: *const Hooks, src_proc: kernel.PrincipalId, src_va: u64, dst_proc: kernel.PrincipalId, dst_va: u64, len_u64: u64) u64 {
    if (len_u64 > @as(u64, trap_abi.abi_trap_copy_max_bytes)) return boot_static.syscall_err_invalid;
    const len: usize = @intCast(len_u64);
    if (len == 0) return 0;
    var scratch: [trap_abi.abi_trap_copy_max_bytes]u8 = undefined;
    const bytes = scratch[0..len];
    if (!h.copy_user_bytes_from_va(src_proc, src_va, bytes)) return boot_static.syscall_err_invalid;
    if (!h.copy_bytes_to_user_va(dst_proc, dst_va, bytes)) return boot_static.syscall_err_invalid;
    return len_u64;
}

pub fn copyToTarget(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, dst_target_va: u64, src_current_va: u64, len_u64: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const target = resolveTarget(h, proc, target_raw, .deferred_reply) orelse return boot_static.syscall_err_endpoint;
    return copyBetweenPrincipals(h, proc, src_current_va, target.proc, dst_target_va, len_u64);
}

pub fn copyFromTarget(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, dst_current_va: u64, src_target_va: u64, len_u64: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const target = resolveTarget(h, proc, target_raw, .delegated) orelse return boot_static.syscall_err_endpoint;
    return copyBetweenPrincipals(h, target.proc, src_target_va, proc, dst_current_va, len_u64);
}

fn teardownTrapTargetProcess(h: *const Hooks, target_proc: kernel.PrincipalId, target_thread: usize) void {
    _ = scheduler.releaseThreadSlot(target_thread);
    _ = h.state.markProcessExited(target_proc);
    h.state.releasePrincipalPageCaps(target_proc, h.free_list);
    if (kernel.processIndexFromPrincipal(target_proc)) |process_index| {
        if (process_index < h.user_spaces.len) {
            h.user_spaces[process_index] = .{};
        }
        h.state.releasePrincipalVmObjectCaps(target_proc, h.free_list);
        h.state.resetProcessRuntimeTables(process_index);
    }
    _ = h.state.unpublishServiceEndpointsForTarget(target_proc);
}

pub fn replyToTarget(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, result: u64, flags: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const exit_response = (flags & trap_abi.response_flag_exit) != 0;
    const target = resolveTarget(h, proc, target_raw, if (exit_response) .exit_response else .deferred_reply) orelse
        return boot_static.syscall_err_endpoint;
    if (exit_response) {
        teardownTrapTargetProcess(h, target.proc, target.thread);
        return boot_static.syscall_ok;
    }
    return ipc.deliverOrQueueMessageToThread(target.thread, 0, scheduler.currentThreadIndex(), false, result, flags, 0, 0);
}

pub fn replyToTargetContext(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, context_va: u64, context_len: u64, flags: u64) u64 {
    _ = flags;
    if (context_len < @sizeOf(trap_abi.UserContext)) return boot_static.syscall_err_invalid;
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const target = resolveTarget(h, proc, target_raw, .deferred_reply) orelse return boot_static.syscall_err_endpoint;
    var user_context: trap_abi.UserContext = .{};
    const bytes = @as([*]u8, @ptrCast(&user_context))[0..@sizeOf(trap_abi.UserContext)];
    if (!h.copy_user_bytes_from_va(proc, context_va, bytes)) return boot_static.syscall_err_invalid;
    const frame = trapFrameFromUserContext(user_context) orelse return boot_static.syscall_err_invalid;
    if (target.ctx.abi_trap_reply_pending and target.ctx.ready) {
        target.ctx.abi_trap_context_frame = frame;
        target.ctx.abi_trap_context_fs_base = user_context.fs_base;
        target.ctx.abi_trap_context_gs_base = user_context.gs_base;
        target.ctx.abi_trap_context_reply_pending = true;
        target.ctx.signal_pending = true;
        scheduler.setIpcHotSignalPending(target.thread, true);
        scheduler.wakeAssignedApForRunnableThread(target.thread);
        scheduler.preferIpcSwitchToThread(target.thread);
        return boot_static.syscall_ok;
    }
    scheduler.prepareBlockedThreadForWake(target.thread);
    target.ctx.frame = frame;
    target.ctx.fs_base = user_context.fs_base;
    target.ctx.gs_base = user_context.gs_base;
    target.ctx.abi_trap_reply_pending = false;
    target.ctx.abi_trap_context_reply_pending = false;
    target.ctx.signal_pending = false;
    target.ctx.wait_mailbox = false;
    target.ctx.wait_preserve_ipc_queue = false;
    target.ctx.wake_tick = 0;
    if (!scheduler.setThreadReady(target.thread, true)) return boot_static.syscall_err_not_ready;
    scheduler.setIpcHotSignalPending(target.thread, false);
    scheduler.setIpcHotWaitState(target.thread, false, 0, true);
    scheduler.wakeAssignedApForRunnableThread(target.thread);
    scheduler.preferIpcSwitchToThread(target.thread);
    return boot_static.syscall_ok;
}

pub fn setTargetRequestPage(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, request_page_va: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const target_proc = targetPrincipalFromRaw(target_raw) orelse return boot_static.syscall_err_invalid;
    if (request_page_va == 0 or (request_page_va & 0xFFF) != 0 or !capability.isUserCanonicalVa(request_page_va)) return boot_static.syscall_err_invalid;
    const target = resolveTarget(h, proc, target_raw, .delegated) orelse return boot_static.syscall_err_endpoint;
    if (target.ctx.ready) return boot_static.syscall_err_invalid;
    const delegate = h.state.abiTrapDelegateFor(target_proc) orelse return boot_static.syscall_err_endpoint;
    h.state.setAbiTrapDelegate(target_proc, delegate.endpoint_id, delegate.flavor, request_page_va) catch return boot_static.syscall_err_invalid;
    return boot_static.syscall_ok;
}

pub fn interruptTarget(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const target = resolveTarget(h, proc, target_raw, .delegated) orelse return boot_static.syscall_err_endpoint;
    target.ctx.signal_pending = true;
    scheduler.setIpcHotSignalPending(target.thread, true);
    scheduler.wakeAssignedApForRunnableThread(target.thread);
    scheduler.preferIpcSwitchToThread(target.thread);
    return boot_static.syscall_ok;
}

fn protFromBits(bits: u64) ?kernel.MapProt {
    const abi_prot = process_builder_abi.mapProtFromBits(bits);
    const prot = kernel.MapProt{
        .read = abi_prot.read,
        .write = abi_prot.write,
        .exec = abi_prot.exec,
    };
    if (!prot.read) return null;
    if (prot.write and prot.exec) return null;
    return prot;
}

fn pageBatchByteLen(target_va: u64, page_count: u64) ?usize {
    if ((target_va & 0xFFF) != 0) return null;
    if (page_count == 0 or page_count > boot_static.syscall_batch_max_pages) return null;
    return @intCast(page_count * 4096);
}

const PaddrMappedContext = struct {
    paddr: u64,
    found: bool = false,
};

fn paddrStillMappedVisitor(context: *anyopaque, page: user_vm.MappedUserPage) bool {
    const state: *PaddrMappedContext = @ptrCast(@alignCast(context));
    if (page.paddr == state.paddr) {
        state.found = true;
        return false;
    }
    return true;
}

fn userPaddrStillMapped(principal: kernel.PrincipalId, paddr: u64) bool {
    var context = PaddrMappedContext{ .paddr = paddr };
    _ = user_vm.forEachUserMappedPage(principal, &context, paddrStillMappedVisitor);
    return context.found;
}

fn dropUnmappedSharedCapIfUnused(h: *const Hooks, target: kernel.PrincipalId, paddr: u64) void {
    if (userPaddrStillMapped(target, paddr)) return;
    const cap = h.state.getTableConst(target).find(paddr) orelse return;
    if (cap.cap_id == cap.root_cap_id and cap.parent_cap_id == 0) return;
    _ = h.state.getTable(target).removeByPaddr(paddr);
}

fn sharedRightsForProt(prot: kernel.MapProt) kernel.Rights {
    return .{
        .cpu_read = true,
        .cpu_write = prot.write,
        .dma = false,
        .grant = false,
    };
}

pub fn mapPagesToCurrentReplyTarget(state: *kernel.KernelState, target_va: u64, page_count: u64, prot_bits: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    _ = pageBatchByteLen(target_va, page_count) orelse return boot_static.syscall_err_invalid;
    const target = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    const page_count_usize: usize = @intCast(page_count);

    var pages: [boot_static.syscall_batch_max_pages]kernel.PageCapability = undefined;
    var allocated: usize = 0;
    const cap_table_start_index = h.state.getTableConst(target.proc).len;
    while (allocated < page_count_usize) : (allocated += 1) {
        pages[allocated] = h.state.allocPageTo(target.proc, h.free_list) catch {
            for (pages[0..allocated]) |page| {
                h.state.reclaimExclusiveRootPage(target.proc, page.paddr, h.free_list) catch {};
            }
            return boot_static.syscall_err_alloc;
        };
    }
    if (!user_vm.mapFreshUserPageCapabilitiesWithProt(h.state, target.proc, target_va, pages[0..allocated], cap_table_start_index, prot)) {
        for (pages[0..allocated]) |page| {
            h.state.reclaimExclusiveRootPage(target.proc, page.paddr, h.free_list) catch {};
        }
        return boot_static.syscall_err_map;
    }
    return boot_static.syscall_ok;
}

pub fn protectCurrentReplyTargetPages(target_va: u64, page_count: u64, prot_bits: u64) u64 {
    const byte_len = pageBatchByteLen(target_va, page_count) orelse return boot_static.syscall_err_invalid;
    const target = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    if (!user_vm.protectUserLinearRegionWithProt(target.proc, target_va, byte_len, prot)) {
        return boot_static.syscall_err_map;
    }
    return boot_static.syscall_ok;
}

pub fn mapVmObjectRangeToCurrentReplyTarget(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    vm_token: u64,
    object_page_offset: u64,
    target_va: u64,
    page_count_raw: u64,
    prot_bits: u64,
) u64 {
    const target = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    const vm_cap_id = kernel.decodeVmObjectToken(vm_token) orelse return boot_static.syscall_err_invalid;
    const vm_cap = state.getVmObjectTableConst(proc).findByCapId(vm_cap_id) orelse return boot_static.syscall_err_invalid;
    if (!vm_cap.rights.read or !vm_cap.rights.map) return boot_static.syscall_err_invalid;
    if (prot.write and !vm_cap.rights.write) return boot_static.syscall_err_invalid;
    if (vm_cap.backing.page_offset_bytes != 0) return boot_static.syscall_err_invalid;
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;
    if (page_count_raw == 0 or page_count_raw > vm_cap.backing.page_count) return boot_static.syscall_err_invalid;
    if (object_page_offset > vm_cap.backing.page_count) return boot_static.syscall_err_invalid;
    if (page_count_raw > vm_cap.backing.page_count - object_page_offset) return boot_static.syscall_err_invalid;

    const page_count: usize = @intCast(page_count_raw);
    const offset: usize = @intCast(object_page_offset);
    var page_index: usize = 0;
    while (page_index < page_count) {
        const run_start = page_index;
        const run_paddr = vm_cap.backing.pagePaddr(offset + run_start) orelse return boot_static.syscall_err_invalid;
        var run_len: usize = 1;
        while (run_start + run_len < page_count) : (run_len += 1) {
            const expected = run_paddr + @as(u64, @intCast(run_len)) * 4096;
            if ((vm_cap.backing.pagePaddr(offset + run_start + run_len) orelse break) != expected) break;
        }
        const run_va = target_va + @as(u64, @intCast(run_start)) * 4096;
        const run_bytes = run_len * 4096;
        if (!user_vm.mapUserLinearRegionWithProt(target.proc, run_va, run_paddr, run_bytes, prot)) {
            return boot_static.syscall_err_map;
        }
        page_index = run_start + run_len;
    }
    return boot_static.syscall_ok;
}

pub fn unmapCurrentReplyTargetPages(state: *kernel.KernelState, target_va: u64, page_count: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const byte_len = pageBatchByteLen(target_va, page_count) orelse return boot_static.syscall_err_invalid;
    const target = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    var paddrs: [boot_static.syscall_batch_max_pages]u64 = undefined;
    const collected = user_vm.collectUserLinearRegionPaddrs(target.proc, target_va, byte_len, paddrs[0..]) orelse return boot_static.syscall_err_map;
    if (collected != page_count) {
        return boot_static.syscall_err_map;
    }
    if (!user_vm.unmapUserLinearRegion(target.proc, target_va, byte_len)) {
        return boot_static.syscall_err_map;
    }
    for (paddrs[0..@intCast(collected)]) |paddr| {
        h.state.reclaimExclusiveRootPage(target.proc, paddr, h.free_list) catch {};
    }
    return boot_static.syscall_ok;
}

pub fn shareCurrentReplyTargetPagesToTarget(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    target_raw: u64,
    target_va: u64,
    page_count: u64,
    prot_bits: u64,
) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    _ = pageBatchByteLen(target_va, page_count) orelse return boot_static.syscall_err_invalid;
    const source = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    const target = resolveTarget(h, proc, target_raw, .delegated) orelse return boot_static.syscall_err_endpoint;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    if (source.proc == target.proc) return boot_static.syscall_ok;

    var page_index: u64 = 0;
    while (page_index < page_count) : (page_index += 1) {
        const va = target_va + page_index * 4096;
        const paddr = capability.lookupUserMappedPaddrForVa(source.proc, va) orelse return boot_static.syscall_err_map;
        h.state.deriveCapForSharedAddressSpace(source.proc, target.proc, paddr, sharedRightsForProt(prot)) catch |err| switch (err) {
            kernel.KernelError.TableFull => return boot_static.syscall_err_alloc,
            else => return boot_static.syscall_err_grant,
        };
        if (!user_vm.mapUserLinearRegionWithProt(target.proc, va, paddr, 4096, prot)) {
            return boot_static.syscall_err_map;
        }
    }
    return boot_static.syscall_ok;
}

pub fn protectTargetPages(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    target_raw: u64,
    target_va: u64,
    page_count: u64,
    prot_bits: u64,
) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const byte_len = pageBatchByteLen(target_va, page_count) orelse return boot_static.syscall_err_invalid;
    const target = resolveTarget(h, proc, target_raw, .delegated) orelse return boot_static.syscall_err_endpoint;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    if (!user_vm.protectUserLinearRegionWithProt(target.proc, target_va, byte_len, prot)) {
        return boot_static.syscall_err_map;
    }
    return boot_static.syscall_ok;
}

pub fn unmapTargetPages(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    target_raw: u64,
    target_va: u64,
    page_count: u64,
) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const byte_len = pageBatchByteLen(target_va, page_count) orelse return boot_static.syscall_err_invalid;
    const target = resolveTarget(h, proc, target_raw, .delegated) orelse return boot_static.syscall_err_endpoint;
    var paddrs: [boot_static.syscall_batch_max_pages]u64 = undefined;
    const collected = user_vm.collectUserLinearRegionPaddrs(target.proc, target_va, byte_len, paddrs[0..]) orelse return boot_static.syscall_err_map;
    if (collected != page_count) return boot_static.syscall_err_map;
    if (!user_vm.unmapUserLinearRegion(target.proc, target_va, byte_len)) {
        return boot_static.syscall_err_map;
    }
    for (paddrs[0..@intCast(collected)]) |paddr| {
        dropUnmappedSharedCapIfUnused(h, target.proc, paddr);
    }
    return boot_static.syscall_ok;
}

pub fn dispatchDelegate(state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    var h_storage = hooksForState(state);
    const delegate = state.abiTrapDelegateFor(proc) orelse return null;
    return dispatchKnownDelegateWithHooks(&h_storage, proc, delegate, frame);
}

pub fn dispatchKnownDelegate(state: *kernel.KernelState, proc: kernel.PrincipalId, delegate: kernel.AbiTrapDelegate, frame: *TrapFrame) ?u64 {
    var h_storage = hooksForState(state);
    return dispatchKnownDelegateWithHooks(&h_storage, proc, delegate, frame);
}

pub fn dispatchPendingSignalDelegate(state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) bool {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const delegate = state.abiTrapDelegateFor(proc) orelse return false;
    const current_thread = scheduler.currentThreadIndex();
    const current_ctx = scheduler.getThreadContext(current_thread) orelse return false;
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return false;
    if (!current_ctx.allocated or current_hot.signal_pending == 0) return false;
    const target_principal = h.state.endpointTargetForKnownActiveOwner(proc, delegate.endpoint_id) orelse return false;
    const target_thread = scheduler.threadSlotForPrincipal(target_principal) orelse return false;

    dispatch_delegate_lock.lock();
    const status = deliverDelegateRequestLocked(
        h,
        proc,
        target_principal,
        target_thread,
        delegate,
        @intFromEnum(trap_abi.TrapKind.async_signal),
        0,
        0,
        frame,
        current_thread,
        current_ctx,
    );
    dispatch_delegate_lock.unlock();
    if (status != boot_static.syscall_ok) {
        current_ctx.abi_trap_reply_pending = false;
        return false;
    }

    current_ctx.signal_pending = false;
    scheduler.setIpcHotSignalPending(current_thread, false);

    if (consumeQueuedReplyForPrincipalFromSender(h, proc, target_thread, frame)) {
        current_ctx.abi_trap_reply_pending = false;
        return true;
    }

    if (!h.block_current_thread_for_event(frame, false, 0, boot_static.syscall_err_not_ready)) {
        if (consumeQueuedReplyForPrincipalFromSender(h, proc, target_thread, frame)) {
            current_ctx.abi_trap_reply_pending = false;
            return true;
        }
        current_ctx.abi_trap_reply_pending = false;
        return false;
    }
    return true;
}

pub fn dispatchPageFaultDelegate(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    fault_va: u64,
    error_code: u64,
    frame: *ExceptionTrapFrame,
) bool {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const delegate = state.abiTrapDelegateFor(proc) orelse return false;
    const current_thread = scheduler.currentThreadIndex();
    const current_ctx = scheduler.getThreadContext(current_thread) orelse return false;
    const target_principal = h.state.endpointTargetForKnownActiveOwner(proc, delegate.endpoint_id) orelse return false;
    const target_thread = scheduler.threadSlotForPrincipal(target_principal) orelse return false;
    var trap_frame = trapFrameFromExceptionFrame(frame);

    dispatch_delegate_lock.lock();
    const status = deliverDelegateRequestLocked(
        h,
        proc,
        target_principal,
        target_thread,
        delegate,
        @intFromEnum(trap_abi.TrapKind.page_fault),
        fault_va,
        error_code,
        &trap_frame,
        current_thread,
        current_ctx,
    );
    dispatch_delegate_lock.unlock();
    if (status != boot_static.syscall_ok) {
        current_ctx.abi_trap_reply_pending = false;
        return false;
    }

    if (consumeQueuedReplyForPrincipalFromSender(h, proc, target_thread, &trap_frame)) {
        current_ctx.abi_trap_reply_pending = false;
        writeTrapFrameToExceptionFrame(frame, trap_frame);
        return true;
    }

    if (!h.block_current_thread_for_event(&trap_frame, false, 0, boot_static.syscall_err_not_ready)) {
        if (consumeQueuedReplyForPrincipalFromSender(h, proc, target_thread, &trap_frame)) {
            current_ctx.abi_trap_reply_pending = false;
            writeTrapFrameToExceptionFrame(frame, trap_frame);
            return true;
        }
        current_ctx.abi_trap_reply_pending = false;
        return false;
    }
    writeTrapFrameToExceptionFrame(frame, trap_frame);
    return true;
}

fn deliverDelegateRequestLocked(
    h: *const Hooks,
    proc: kernel.PrincipalId,
    target_principal: kernel.PrincipalId,
    target_thread: usize,
    delegate: kernel.AbiTrapDelegate,
    kind: u32,
    fault_addr: u64,
    error_code: u64,
    frame: *TrapFrame,
    current_thread: usize,
    current_ctx: *scheduler.ThreadContext,
) u64 {
    const stale_replies = scheduler.discardIpcReplyMessagesForThreadFromSender(current_thread, target_thread);
    if (stale_replies != 0) current_ctx.abi_trap_reply_pending = false;
    const stale_requests = scheduler.discardIpcMessagesForThreadFromSenderOnEndpoint(target_thread, current_thread, delegate.endpoint_id, true);
    if (stale_requests != 0) current_ctx.abi_trap_reply_pending = false;
    if (!writeRequestEx(
        h,
        target_principal,
        delegate.request_page_va,
        proc,
        @intCast(current_thread),
        delegate.flavor,
        kind,
        fault_addr,
        error_code,
        frame,
        current_ctx.fs_base,
        current_ctx.gs_base,
    )) {
        return boot_static.syscall_err_invalid;
    }

    current_ctx.abi_trap_reply_pending = true;
    const status = ipc.deliverOrQueueMessageToThread(
        target_thread,
        delegate.endpoint_id,
        current_thread,
        true,
        delegate.request_page_va,
        0,
        0,
        0,
    );
    if (status == boot_static.syscall_ok) return boot_static.syscall_ok;
    current_ctx.abi_trap_reply_pending = false;
    return status;
}

fn dispatchKnownDelegateWithHooks(h: *const Hooks, proc: kernel.PrincipalId, delegate: kernel.AbiTrapDelegate, frame: *TrapFrame) ?u64 {
    const current_thread = scheduler.currentThreadIndex();
    const current_ctx = scheduler.getThreadContext(current_thread) orelse return boot_static.syscall_err_not_ready;
    const target_principal = h.state.endpointTargetForKnownActiveOwner(proc, delegate.endpoint_id) orelse return boot_static.syscall_err_endpoint;
    const target_thread = scheduler.threadSlotForPrincipal(target_principal) orelse return boot_static.syscall_err_endpoint;
    dispatch_delegate_lock.lock();
    const status = deliverDelegateRequestLocked(
        h,
        proc,
        target_principal,
        target_thread,
        delegate,
        @intFromEnum(trap_abi.TrapKind.abi_syscall),
        0,
        0,
        frame,
        current_thread,
        current_ctx,
    );
    dispatch_delegate_lock.unlock();
    if (status != boot_static.syscall_ok) {
        current_ctx.abi_trap_reply_pending = false;
        return status;
    }

    if (consumeQueuedReplyForPrincipalFromSender(h, proc, target_thread, frame)) {
        current_ctx.abi_trap_reply_pending = false;
        return frame.rax;
    }

    if (!h.block_current_thread_for_event(frame, false, 0, boot_static.syscall_err_not_ready)) {
        if (consumeQueuedReplyForPrincipalFromSender(h, proc, target_thread, frame)) {
            current_ctx.abi_trap_reply_pending = false;
            return frame.rax;
        }
        current_ctx.abi_trap_reply_pending = false;
        return boot_static.syscall_err_not_ready;
    }
    return boot_static.syscall_ok;
}
