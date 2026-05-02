const kernel = @import("../kernel.zig");
const capability = @import("../capability.zig");
const interrupts = @import("../interrupts.zig");
const scheduler = @import("../scheduler.zig");
const user_vm = @import("../memory/user_vm.zig");
const boot_static = @import("../boot/main_static.zig");
const abi_root = @import("kernel_abi_root");
const process_factory = @import("../boot/process_factory.zig");
const ipc = @import("ipc.zig");

const TrapFrame = interrupts.TrapFrame;
const trap_abi = abi_root.trap_abi;
const process_abi = abi_root.process_abi;
const process_builder_abi = abi_root.process_builder_abi;

pub const Hooks = struct {
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    user_spaces: []boot_static.UserAddressSpace,
    write: *const fn ([]const u8) void,
    principal_label: *const fn (kernel.PrincipalId) []const u8,
    write_user_u64: *const fn (kernel.PrincipalId, u64, u64) bool,
    copy_user_bytes_from_va: *const fn (kernel.PrincipalId, u64, []u8) bool,
    copy_bytes_to_user_va: *const fn (kernel.PrincipalId, u64, []const u8) bool,
    consume_pending_signal_for_principal: *const fn (kernel.PrincipalId) bool,
    block_current_thread_for_event: *const fn (*TrapFrame, bool, u64, u64) bool,
    exit_current_process: *const fn (kernel.PrincipalId, u8, *TrapFrame) void,
};

var hooks: ?Hooks = null;

pub fn init(new_hooks: Hooks) void {
    hooks = new_hooks;
}

fn getHooks() *const Hooks {
    return &(hooks orelse unreachable);
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
) bool {
    const version_kind = @as(u64, trap_abi.version) |
        (@as(u64, @intFromEnum(trap_abi.TrapKind.abi_syscall)) << 32);
    const flavor_reserved = @as(u64, flavor);

    if (!writeRequestU64(h, target, request_page_va, 0x00, trap_abi.magic)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x08, version_kind)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x10, flavor_reserved)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x18, @intFromEnum(caller))) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x20, thread_id)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x28, frame.rip)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x30, frame.rsp)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x38, 0)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x40, 0)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x48, frame.rax)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x50, frame.rdi)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x58, frame.rsi)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x60, frame.rdx)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x68, frame.r10)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x70, frame.r8)) return false;
    if (!writeRequestU64(h, target, request_page_va, 0x78, frame.r9)) return false;
    return true;
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

fn currentReplyTargetPrincipal() ?kernel.PrincipalId {
    const current_thread = scheduler.current_thread_index;
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return null;
    if (current_hot.ipc_reply_token_valid == 0) return null;
    const target_thread = current_hot.ipc_reply_token_target_thread;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return null;
    if (!target_ctx.allocated) return null;
    return target_ctx.owner_process;
}

fn currentReplyTargetThread() ?usize {
    const current_thread = scheduler.current_thread_index;
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return null;
    if (current_hot.ipc_reply_token_valid == 0) return null;
    const target_thread = current_hot.ipc_reply_token_target_thread;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return null;
    if (!target_ctx.allocated) return null;
    return target_thread;
}

pub fn setCurrentReplyTargetFsBase(fs_base: u64) u64 {
    if (fs_base != 0 and !capability.isUserCanonicalVa(fs_base)) return boot_static.syscall_err_invalid;
    const target_thread = currentReplyTargetThread() orelse return boot_static.syscall_err_endpoint;
    if (!scheduler.setThreadFsBase(target_thread, fs_base)) return boot_static.syscall_err_not_ready;
    return boot_static.syscall_ok;
}

pub fn copyFromCurrentReplyTarget(proc: kernel.PrincipalId, dst_current_va: u64, src_target_va: u64, len_u64: u64) u64 {
    const h = getHooks();
    if (len_u64 > @as(u64, trap_abi.abi_trap_copy_max_bytes)) return boot_static.syscall_err_invalid;
    const target_proc = currentReplyTargetPrincipal() orelse return boot_static.syscall_err_endpoint;
    const len: usize = @intCast(len_u64);
    if (len == 0) return 0;

    var scratch: [trap_abi.abi_trap_copy_max_bytes]u8 = undefined;
    const bytes = scratch[0..len];
    if (!h.copy_user_bytes_from_va(target_proc, src_target_va, bytes)) return boot_static.syscall_err_invalid;
    if (!h.copy_bytes_to_user_va(proc, dst_current_va, bytes)) return boot_static.syscall_err_invalid;
    return len_u64;
}

pub fn copyToCurrentReplyTarget(proc: kernel.PrincipalId, dst_target_va: u64, src_current_va: u64, len_u64: u64) u64 {
    const h = getHooks();
    if (len_u64 > @as(u64, trap_abi.abi_trap_copy_max_bytes)) return boot_static.syscall_err_invalid;
    const target_proc = currentReplyTargetPrincipal() orelse return boot_static.syscall_err_endpoint;
    const len: usize = @intCast(len_u64);
    if (len == 0) return 0;

    var scratch: [trap_abi.abi_trap_copy_max_bytes]u8 = undefined;
    const bytes = scratch[0..len];
    if (!h.copy_user_bytes_from_va(proc, src_current_va, bytes)) return boot_static.syscall_err_invalid;
    if (!h.copy_bytes_to_user_va(target_proc, dst_target_va, bytes)) return boot_static.syscall_err_invalid;
    return len_u64;
}

fn deferredTargetThread(h: *const Hooks, server_proc: kernel.PrincipalId, target_raw: u64) ?usize {
    if (target_raw >= kernel.process_count) return null;
    const target_proc: kernel.PrincipalId = @enumFromInt(@as(u8, @intCast(target_raw)));
    const target_thread = scheduler.threadSlotForPrincipal(target_proc) orelse return null;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return null;
    if (!target_ctx.allocated or !target_ctx.abi_trap_reply_pending) return null;
    const delegate = h.state.abiTrapDelegateFor(target_proc) orelse return null;
    const delegate_target = h.state.endpointTargetFor(target_proc, delegate.endpoint_id) orelse return null;
    if (delegate_target != server_proc) return null;
    return target_thread;
}

pub fn copyToTarget(proc: kernel.PrincipalId, target_raw: u64, dst_target_va: u64, src_current_va: u64, len_u64: u64) u64 {
    const h = getHooks();
    if (len_u64 > @as(u64, trap_abi.abi_trap_copy_max_bytes)) return boot_static.syscall_err_invalid;
    const target_thread = deferredTargetThread(h, proc, target_raw) orelse return boot_static.syscall_err_endpoint;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    const target_proc = target_ctx.owner_process;
    const len: usize = @intCast(len_u64);
    if (len == 0) return 0;

    var scratch: [trap_abi.abi_trap_copy_max_bytes]u8 = undefined;
    const bytes = scratch[0..len];
    if (!h.copy_user_bytes_from_va(proc, src_current_va, bytes)) return boot_static.syscall_err_invalid;
    if (!h.copy_bytes_to_user_va(target_proc, dst_target_va, bytes)) return boot_static.syscall_err_invalid;
    return len_u64;
}

pub fn replyToTarget(proc: kernel.PrincipalId, target_raw: u64, result: u64, flags: u64) u64 {
    const h = getHooks();
    const target_thread = deferredTargetThread(h, proc, target_raw) orelse return boot_static.syscall_err_endpoint;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    const target_proc = target_ctx.owner_process;
    if ((flags & trap_abi.response_flag_exit) != 0) {
        _ = reclaimPrivatePagesForProcess(target_proc);
        target_ctx.abi_trap_reply_pending = false;
        _ = h.state.markProcessExited(target_proc);
        _ = scheduler.releaseThreadSlot(target_thread);
        return boot_static.syscall_ok;
    }
    return ipc.deliverOrQueueMessageToThread(target_thread, 0, scheduler.current_thread_index, false, result, flags, 0, 0);
}

pub fn startTarget(proc: kernel.PrincipalId, target_raw: u64) u64 {
    const h = getHooks();
    if (target_raw >= kernel.process_count) return boot_static.syscall_err_invalid;
    const target_proc: kernel.PrincipalId = @enumFromInt(@as(u8, @intCast(target_raw)));
    const target_thread = scheduler.threadSlotForPrincipal(target_proc) orelse return boot_static.syscall_err_endpoint;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    if (!target_ctx.allocated or target_ctx.ready or target_ctx.abi_trap_reply_pending) return boot_static.syscall_err_invalid;
    const delegate = h.state.abiTrapDelegateFor(target_proc) orelse return boot_static.syscall_err_endpoint;
    const delegate_target = h.state.endpointTargetFor(target_proc, delegate.endpoint_id) orelse return boot_static.syscall_err_endpoint;
    if (delegate_target != proc) return boot_static.syscall_err_endpoint;
    return if (scheduler.setThreadReady(target_thread, true)) boot_static.syscall_ok else boot_static.syscall_err_not_ready;
}

pub fn setTargetRequestPage(proc: kernel.PrincipalId, target_raw: u64, request_page_va: u64) u64 {
    const h = getHooks();
    if (target_raw >= kernel.process_count) return boot_static.syscall_err_invalid;
    if (request_page_va == 0 or (request_page_va & 0xFFF) != 0 or !capability.isUserCanonicalVa(request_page_va)) return boot_static.syscall_err_invalid;
    const target_proc: kernel.PrincipalId = @enumFromInt(@as(u8, @intCast(target_raw)));
    const target_thread = scheduler.threadSlotForPrincipal(target_proc) orelse return boot_static.syscall_err_endpoint;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    if (!target_ctx.allocated or target_ctx.ready or target_ctx.abi_trap_reply_pending) return boot_static.syscall_err_invalid;
    const delegate = h.state.abiTrapDelegateFor(target_proc) orelse return boot_static.syscall_err_endpoint;
    const delegate_target = h.state.endpointTargetFor(target_proc, delegate.endpoint_id) orelse return boot_static.syscall_err_endpoint;
    if (delegate_target != proc) return boot_static.syscall_err_endpoint;
    h.state.setAbiTrapDelegate(target_proc, delegate.endpoint_id, delegate.flavor, request_page_va) catch return boot_static.syscall_err_invalid;
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

pub fn mapPagesToCurrentReplyTarget(target_va: u64, page_count: u64, prot_bits: u64) u64 {
    const h = getHooks();
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;
    if (page_count == 0 or page_count > boot_static.syscall_batch_max_pages) return boot_static.syscall_err_invalid;
    const target_proc = currentReplyTargetPrincipal() orelse return boot_static.syscall_err_endpoint;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;

    var page_index: u64 = 0;
    while (page_index < page_count) : (page_index += 1) {
        const page = h.state.allocPageTo(target_proc, h.free_list) catch return boot_static.syscall_err_alloc;
        const va = target_va + page_index * 4096;
        if (!user_vm.mapUserLinearRegionWithProt(target_proc, va, page.paddr, 4096, prot)) return boot_static.syscall_err_map;
    }
    return boot_static.syscall_ok;
}

pub fn protectCurrentReplyTargetPages(target_va: u64, page_count: u64, prot_bits: u64) u64 {
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;
    if (page_count == 0 or page_count > boot_static.syscall_batch_max_pages) return boot_static.syscall_err_invalid;
    const target_proc = currentReplyTargetPrincipal() orelse return boot_static.syscall_err_endpoint;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    if (!user_vm.protectUserLinearRegionWithProt(target_proc, target_va, @intCast(page_count * 4096), prot)) {
        return boot_static.syscall_err_map;
    }
    return boot_static.syscall_ok;
}

pub fn unmapCurrentReplyTargetPages(target_va: u64, page_count: u64) u64 {
    const h = getHooks();
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;
    if (page_count == 0 or page_count > boot_static.syscall_batch_max_pages) return boot_static.syscall_err_invalid;
    const target_proc = currentReplyTargetPrincipal() orelse return boot_static.syscall_err_endpoint;
    const byte_len: usize = @intCast(page_count * 4096);
    var paddrs: [boot_static.syscall_batch_max_pages]u64 = undefined;
    const collected = user_vm.collectUserLinearRegionPaddrs(target_proc, target_va, byte_len, paddrs[0..]) orelse return boot_static.syscall_err_map;
    if (collected != page_count) return boot_static.syscall_err_map;
    for (paddrs[0..collected]) |paddr| {
        const cap = h.state.getTableConst(target_proc).find(paddr) orelse return boot_static.syscall_err_invalid;
        if (cap.cap_id != cap.root_cap_id or cap.parent_cap_id != 0) return boot_static.syscall_err_invalid;
        switch (h.state.scanCapTables(paddr)) {
            .owner => |actual_owner| if (actual_owner != target_proc) return boot_static.syscall_err_invalid,
            .shared, .none => return boot_static.syscall_err_invalid,
        }
        if (!h.free_list.canAppendPage(0, paddr)) return boot_static.syscall_err_alloc;
    }
    if (!user_vm.unmapUserLinearRegion(target_proc, target_va, byte_len)) {
        return boot_static.syscall_err_map;
    }
    for (paddrs[0..collected]) |paddr| {
        h.state.reclaimExclusiveRootPage(target_proc, paddr, h.free_list) catch return boot_static.syscall_err_invalid;
    }
    return boot_static.syscall_ok;
}

fn isExclusiveRootMappedPage(h: *const Hooks, target_proc: kernel.PrincipalId, paddr: u64) bool {
    const cap = h.state.getTableConst(target_proc).find(paddr) orelse return false;
    if (cap.cap_id != cap.root_cap_id or cap.parent_cap_id != 0) return false;
    switch (h.state.scanCapTables(paddr)) {
        .owner => |actual_owner| if (actual_owner != target_proc) return false,
        .shared, .none => return false,
    }
    return true;
}

pub fn reclaimPrivatePagesForProcess(target_proc: kernel.PrincipalId) u64 {
    const h = getHooks();
    var mapped_pages: [512]user_vm.MappedUserPage = undefined;
    var pages: [boot_static.syscall_batch_max_pages]user_vm.MappedUserPage = undefined;
    const mapped_count = user_vm.collectUserMappedPages(target_proc, mapped_pages[0..]);
    var count: usize = 0;
    var i: usize = 0;
    while (i < mapped_count) : (i += 1) {
        const paddr = mapped_pages[i].paddr;
        if (!isExclusiveRootMappedPage(h, target_proc, paddr)) continue;
        if (count >= pages.len) break;
        if (!h.free_list.canAppendPage(0, paddr)) return boot_static.syscall_err_alloc;
        pages[count] = mapped_pages[i];
        count += 1;
    }
    var reclaimed: u64 = 0;
    for (pages[0..count]) |page| {
        if (!user_vm.unmapUserLinearRegion(target_proc, page.va, 4096)) continue;
        const paddr = page.paddr;
        h.state.reclaimExclusiveRootPage(target_proc, paddr, h.free_list) catch return boot_static.syscall_err_invalid;
        reclaimed += 1;
    }
    return reclaimed;
}

pub fn reclaimCurrentReplyTargetPrivatePages() u64 {
    const target_proc = currentReplyTargetPrincipal() orelse return boot_static.syscall_err_endpoint;
    return reclaimPrivatePagesForProcess(target_proc);
}

fn abortForkedProcess(principal: kernel.PrincipalId) void {
    const h = getHooks();
    _ = reclaimPrivatePagesForProcess(principal);
    if (scheduler.threadSlotForPrincipal(principal)) |thread_index| {
        _ = scheduler.releaseThreadSlot(thread_index);
    }
    _ = h.state.markProcessExited(principal);
}

const ForkCopyContext = struct {
    h: *const Hooks,
    parent: kernel.PrincipalId,
    child: kernel.PrincipalId,
    scratch: *[4096]u8,
    status: u64 = boot_static.syscall_ok,
};

fn copyForkMappedPage(context: *anyopaque, page: user_vm.MappedUserPage) bool {
    const ctx: *ForkCopyContext = @ptrCast(@alignCast(context));
    if (!ctx.h.copy_user_bytes_from_va(ctx.parent, page.va, ctx.scratch[0..])) {
        ctx.status = boot_static.syscall_err_invalid;
        return false;
    }
    const child_page = ctx.h.state.allocPageTo(ctx.child, ctx.h.free_list) catch {
        ctx.status = boot_static.syscall_err_alloc;
        return false;
    };
    if (!user_vm.mapUserLinearRegionWithProt(ctx.child, page.va, child_page.paddr, 4096, .{ .read = true, .write = page.writable, .exec = true })) {
        ctx.h.state.reclaimExclusiveRootPage(ctx.child, child_page.paddr, ctx.h.free_list) catch {};
        ctx.status = boot_static.syscall_err_map;
        return false;
    }
    if (!ctx.h.copy_bytes_to_user_va(ctx.child, page.va, ctx.scratch[0..])) {
        ctx.status = boot_static.syscall_err_map;
        return false;
    }
    return true;
}

pub fn forkCurrentReplyTarget() u64 {
    const h = getHooks();
    const target_thread = currentReplyTargetThread() orelse return boot_static.syscall_err_endpoint;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    if (!target_ctx.allocated) return boot_static.syscall_err_endpoint;
    const target_proc = target_ctx.owner_process;
    const delegate = h.state.abiTrapDelegateFor(target_proc) orelse return boot_static.syscall_err_endpoint;
    const delegate_target = h.state.endpointTargetFor(target_proc, delegate.endpoint_id) orelse return boot_static.syscall_err_endpoint;

    const created = process_factory.tryCreateSuspendedUserProcess(
        h.state,
        "forked linux",
        h.user_spaces,
    ) catch return boot_static.syscall_err_alloc;
    const child = created.principal;
    var scratch: [4096]u8 = undefined;
    var copy_context = ForkCopyContext{
        .h = h,
        .parent = target_proc,
        .child = child,
        .scratch = &scratch,
    };
    if (!user_vm.forEachUserMappedPage(target_proc, @ptrCast(&copy_context), copyForkMappedPage)) {
        abortForkedProcess(child);
        return if (copy_context.status != boot_static.syscall_ok) copy_context.status else boot_static.syscall_err_invalid;
    }

    h.state.installEndpoint(child, delegate.endpoint_id, delegate_target) catch {
        abortForkedProcess(child);
        return boot_static.syscall_err_endpoint;
    };
    h.state.setAbiTrapDelegate(child, delegate.endpoint_id, delegate.flavor, delegate.request_page_va) catch {
        abortForkedProcess(child);
        return boot_static.syscall_err_invalid;
    };

    const child_thread = scheduler.threadSlotForPrincipal(child) orelse {
        abortForkedProcess(child);
        return boot_static.syscall_err_invalid;
    };
    const child_ctx = scheduler.getThreadContext(child_thread) orelse {
        abortForkedProcess(child);
        return boot_static.syscall_err_invalid;
    };
    child_ctx.frame = target_ctx.frame;
    child_ctx.frame.rax = 0;
    child_ctx.fs_base = target_ctx.fs_base;
    child_ctx.fx_state = target_ctx.fx_state;
    child_ctx.abi_trap_reply_pending = false;
    const slot = kernel.processIndexFromPrincipal(child) orelse return boot_static.syscall_err_invalid;
    return process_abi.encodeSpawnedProcess(@intCast(slot), @intCast(child_thread));
}

pub fn dispatchDelegate(proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    const h = getHooks();
    const delegate = h.state.abiTrapDelegateFor(proc) orelse return null;
    const current_thread = scheduler.current_thread_index;
    const current_ctx = scheduler.getThreadContext(current_thread) orelse return boot_static.syscall_err_not_ready;
    const target_principal = h.state.endpointTargetFor(proc, delegate.endpoint_id) orelse return boot_static.syscall_err_endpoint;
    const target_thread = scheduler.threadSlotForPrincipal(target_principal) orelse return boot_static.syscall_err_endpoint;
    if (!writeRequest(h, target_principal, delegate.request_page_va, proc, @intCast(current_thread), delegate.flavor, frame)) {
        h.write("abi_trap request write failed target=");
        h.write(h.principal_label(target_principal));
        h.write("\n");
        return boot_static.syscall_err_invalid;
    }

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
    if (status != boot_static.syscall_ok) return status;

    if (consumeQueuedReplyForPrincipal(proc, frame)) return frame.rax;
    if (h.consume_pending_signal_for_principal(proc)) return boot_static.syscall_ok;

    current_ctx.abi_trap_reply_pending = true;
    if (!h.block_current_thread_for_event(frame, false, 0, boot_static.syscall_err_not_ready)) {
        current_ctx.abi_trap_reply_pending = false;
        return boot_static.syscall_err_not_ready;
    }
    return boot_static.syscall_ok;
}
