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

var dispatch_delegate_failure_log_count: u64 = 0;
const dispatch_delegate_failure_log_limit: u64 = 16;
var dispatch_delegate_lookup_log_count: u64 = 0;
const dispatch_delegate_lookup_log_limit: u64 = 16;

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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(dispatch_delegate_failure_log_count), &dispatch_delegate_failure_log_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(dispatch_delegate_lookup_log_count), &dispatch_delegate_lookup_log_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(abi_trap_hooks_storage), &abi_trap_hooks_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(abi_trap_hooks_ready), &abi_trap_hooks_ready));
    return end;
}

pub fn init(new_hooks: Hooks) void {
    abi_trap_hooks_storage = new_hooks;
    abi_trap_hooks_ready = true;
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

fn currentReplyTargetPrincipal() ?kernel.PrincipalId {
    const current_thread = scheduler.currentThreadIndex();
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return null;
    if (current_hot.ipc_reply_token_valid == 0) return null;
    const target_thread = current_hot.ipc_reply_token_target_thread;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return null;
    if (!target_ctx.allocated) return null;
    return target_ctx.owner_process;
}

fn currentReplyTargetThread() ?usize {
    const current_thread = scheduler.currentThreadIndex();
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

fn abiTrapTargetThread(h: *const Hooks, server_proc: kernel.PrincipalId, target_raw: u64) ?usize {
    if (target_raw >= kernel.process_count) return null;
    const target_proc: kernel.PrincipalId = @enumFromInt(@as(u8, @intCast(target_raw)));
    const target_thread = scheduler.threadSlotForPrincipal(target_proc) orelse return null;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return null;
    if (!target_ctx.allocated) return null;
    const delegate = h.state.abiTrapDelegateFor(target_proc) orelse return null;
    const delegate_target = h.state.endpointTargetFor(target_proc, delegate.endpoint_id) orelse return null;
    if (delegate_target != server_proc) return null;
    return target_thread;
}

fn inactiveTargetThreadWithEndpointToServer(h: *const Hooks, server_proc: kernel.PrincipalId, target_raw: u64) ?usize {
    if (target_raw >= kernel.process_count) return null;
    const target_proc: kernel.PrincipalId = @enumFromInt(@as(u8, @intCast(target_raw)));
    const target_thread = scheduler.threadSlotForPrincipal(target_proc) orelse return null;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return null;
    if (!target_ctx.allocated) return null;
    const table = h.state.getEndpointTableConst(target_proc);
    const limit = @min(table.len, table.caps.len);
    var i: usize = 0;
    while (i < limit) : (i += 1) {
        if (table.caps[i].target == server_proc) return target_thread;
    }
    return null;
}

pub fn copyToTarget(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, dst_target_va: u64, src_current_va: u64, len_u64: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
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

pub fn replyToTarget(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, result: u64, flags: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const exit_response = (flags & trap_abi.response_flag_exit) != 0;
    const target_thread = if (exit_response)
        (deferredTargetThread(h, proc, target_raw) orelse
            abiTrapTargetThread(h, proc, target_raw) orelse
            inactiveTargetThreadWithEndpointToServer(h, proc, target_raw) orelse
            return boot_static.syscall_err_endpoint)
    else
        (deferredTargetThread(h, proc, target_raw) orelse return boot_static.syscall_err_endpoint);
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    const target_proc = target_ctx.owner_process;
    if (exit_response) {
        _ = scheduler.releaseThreadSlot(target_thread);
        _ = h.state.markProcessExited(target_proc);
        _ = reclaimPrivatePagesForProcessWithHooks(h, target_proc);
        return boot_static.syscall_ok;
    }
    return ipc.deliverOrQueueMessageToThread(target_thread, 0, scheduler.currentThreadIndex(), false, result, flags, 0, 0);
}

pub fn startTarget(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    if (target_raw >= kernel.process_count) return boot_static.syscall_err_invalid;
    const target_proc: kernel.PrincipalId = @enumFromInt(@as(u8, @intCast(target_raw)));
    const target_thread = scheduler.threadSlotForPrincipal(target_proc) orelse return boot_static.syscall_err_endpoint;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    if (!target_ctx.allocated or target_ctx.ready or target_ctx.abi_trap_reply_pending) return boot_static.syscall_err_invalid;
    const delegate = h.state.abiTrapDelegateFor(target_proc) orelse return boot_static.syscall_err_endpoint;
    const delegate_target = h.state.endpointTargetFor(target_proc, delegate.endpoint_id) orelse return boot_static.syscall_err_endpoint;
    if (delegate_target != proc) return boot_static.syscall_err_endpoint;
    if (!scheduler.setThreadReady(target_thread, true)) return boot_static.syscall_err_not_ready;
    scheduler.wakeAssignedApForRunnableThread(target_thread);
    return boot_static.syscall_ok;
}

pub fn setTargetRequestPage(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, request_page_va: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    if (target_raw >= kernel.process_count) return boot_static.syscall_err_invalid;
    if (request_page_va == 0 or (request_page_va & 0xFFF) != 0 or !capability.isUserCanonicalVa(request_page_va)) return boot_static.syscall_err_invalid;
    const target_proc: kernel.PrincipalId = @enumFromInt(@as(u8, @intCast(target_raw)));
    const target_thread = scheduler.threadSlotForPrincipal(target_proc) orelse return boot_static.syscall_err_endpoint;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    if (!target_ctx.allocated or target_ctx.ready) return boot_static.syscall_err_invalid;
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

pub fn mapPagesToCurrentReplyTarget(state: *kernel.KernelState, target_va: u64, page_count: u64, prot_bits: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
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

pub fn unmapCurrentReplyTargetPages(state: *kernel.KernelState, target_va: u64, page_count: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;
    if (page_count == 0 or page_count > boot_static.syscall_batch_max_pages) return boot_static.syscall_err_invalid;
    const target_proc = currentReplyTargetPrincipal() orelse return boot_static.syscall_err_endpoint;
    const byte_len: usize = @intCast(page_count * 4096);
    var paddrs: [boot_static.syscall_batch_max_pages]u64 = undefined;
    var reclaimable: [boot_static.syscall_batch_max_pages]bool = [_]bool{false} ** boot_static.syscall_batch_max_pages;
    const collected = user_vm.collectUserLinearRegionPaddrs(target_proc, target_va, byte_len, paddrs[0..]) orelse return boot_static.syscall_err_map;
    if (collected != page_count) return boot_static.syscall_err_map;
    var page_index: usize = 0;
    while (page_index < collected) : (page_index += 1) {
        const paddr = paddrs[page_index];
        const cap = h.state.getTableConst(target_proc).find(paddr) orelse return boot_static.syscall_err_invalid;
        if (cap.cap_id == cap.root_cap_id and cap.parent_cap_id == 0) {
            switch (h.state.scanCapTables(paddr)) {
                .owner => |actual_owner| {
                    if (actual_owner != target_proc) return boot_static.syscall_err_invalid;
                    if (!h.free_list.canAppendPage(0, paddr)) return boot_static.syscall_err_alloc;
                    reclaimable[page_index] = true;
                },
                .shared => {},
                .none => return boot_static.syscall_err_invalid,
            }
        }
    }
    if (!user_vm.unmapUserLinearRegion(target_proc, target_va, byte_len)) {
        return boot_static.syscall_err_map;
    }
    page_index = 0;
    while (page_index < collected) : (page_index += 1) {
        if (!reclaimable[page_index]) continue;
        const paddr = paddrs[page_index];
        h.state.reclaimExclusiveRootPage(target_proc, paddr, h.free_list) catch return boot_static.syscall_err_invalid;
    }
    return boot_static.syscall_ok;
}

pub fn unmapTargetPages(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, target_va: u64, page_count: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;
    if (page_count == 0 or page_count > boot_static.syscall_batch_max_pages) return boot_static.syscall_err_invalid;
    const target_thread = abiTrapTargetThread(h, proc, target_raw) orelse return boot_static.syscall_err_endpoint;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    const target_proc = target_ctx.owner_process;
    const byte_len: usize = @intCast(page_count * 4096);
    if (!user_vm.unmapUserLinearRegion(target_proc, target_va, byte_len)) {
        return boot_static.syscall_err_map;
    }
    return boot_static.syscall_ok;
}

pub fn shareCurrentReplyTargetPagesToTarget(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, target_va: u64, page_count: u64, prot_bits: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;
    if (page_count == 0 or page_count > boot_static.syscall_batch_max_pages) return boot_static.syscall_err_invalid;
    const source_proc = currentReplyTargetPrincipal() orelse return boot_static.syscall_err_endpoint;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    const target_thread = abiTrapTargetThread(h, proc, target_raw) orelse return boot_static.syscall_err_endpoint;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    const target_proc = target_ctx.owner_process;
    if (target_proc == source_proc) return boot_static.syscall_ok;

    const byte_len: usize = @intCast(page_count * 4096);
    var paddrs: [boot_static.syscall_batch_max_pages]u64 = undefined;
    const collected = user_vm.collectUserLinearRegionPaddrs(source_proc, target_va, byte_len, paddrs[0..]) orelse return boot_static.syscall_err_map;
    if (collected != page_count) return boot_static.syscall_err_map;

    var page_index: u64 = 0;
    while (page_index < page_count) : (page_index += 1) {
        const va = target_va + page_index * 4096;
        const paddr = paddrs[@intCast(page_index)];

        if (capability.lookupUserMappedPaddrForVa(target_proc, va)) |existing_paddr| {
            if (existing_paddr != paddr) return boot_static.syscall_err_map;
            const target_cap = h.state.getTableConst(target_proc).find(paddr) orelse return boot_static.syscall_err_invalid;
            if (!target_cap.rights.cpu_read or (prot.write and !target_cap.rights.cpu_write)) return boot_static.syscall_err_grant;
            if (!user_vm.protectUserLinearRegionWithProt(target_proc, va, 4096, prot)) return boot_static.syscall_err_map;
            continue;
        }

        if (h.state.getTableConst(target_proc).find(paddr)) |target_cap| {
            if (!target_cap.rights.cpu_read or (prot.write and !target_cap.rights.cpu_write)) return boot_static.syscall_err_grant;
        } else {
            const source_cap = h.state.getTableConst(source_proc).find(paddr) orelse return boot_static.syscall_err_invalid;
            if (!source_cap.rights.cpu_read or (prot.write and !source_cap.rights.cpu_write)) return boot_static.syscall_err_grant;
            h.state.deriveCapForSharedAddressSpace(
                source_proc,
                target_proc,
                paddr,
                .{
                    .cpu_read = true,
                    .cpu_write = source_cap.rights.cpu_write,
                    .dma = false,
                },
            ) catch return boot_static.syscall_err_grant;
        }
        if (!user_vm.mapUserLinearRegionWithProt(target_proc, va, paddr, 4096, prot)) return boot_static.syscall_err_map;
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

fn reclaimPrivatePagesForProcessWithHooks(h: *const Hooks, target_proc: kernel.PrincipalId) u64 {
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

pub fn reclaimPrivatePagesForProcess(state: *kernel.KernelState, target_proc: kernel.PrincipalId) u64 {
    var h_storage = hooksForState(state);
    return reclaimPrivatePagesForProcessWithHooks(&h_storage, target_proc);
}

pub fn reclaimCurrentReplyTargetPrivatePages(state: *kernel.KernelState) u64 {
    var h_storage = hooksForState(state);
    const target_proc = currentReplyTargetPrincipal() orelse return boot_static.syscall_err_endpoint;
    return reclaimPrivatePagesForProcessWithHooks(&h_storage, target_proc);
}

fn abortForkedProcess(h: *const Hooks, principal: kernel.PrincipalId) void {
    _ = reclaimPrivatePagesForProcessWithHooks(h, principal);
    if (scheduler.threadSlotForPrincipal(principal)) |thread_index| {
        _ = scheduler.releaseThreadSlot(thread_index);
    }
    _ = h.state.markProcessExited(principal);
}

fn abortSharedCloneProcess(h: *const Hooks, principal: kernel.PrincipalId) void {
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

const CloneShareContext = struct {
    h: *const Hooks,
    parent: kernel.PrincipalId,
    child: kernel.PrincipalId,
    scratch: *[4096]u8,
    status: u64 = boot_static.syscall_ok,
};

fn copyCloneMappedPage(ctx: *CloneShareContext, page: user_vm.MappedUserPage) bool {
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

fn shareCloneMappedPage(context: *anyopaque, page: user_vm.MappedUserPage) bool {
    const ctx: *CloneShareContext = @ptrCast(@alignCast(context));
    const src_cap = ctx.h.state.getTableConst(ctx.parent).find(page.paddr) orelse {
        return copyCloneMappedPage(ctx, page);
    };
    if (!src_cap.rights.cpu_read) {
        return copyCloneMappedPage(ctx, page);
    }
    ctx.h.state.deriveCapForSharedAddressSpace(
        ctx.parent,
        ctx.child,
        page.paddr,
        .{
            .cpu_read = true,
            .cpu_write = src_cap.rights.cpu_write,
            .dma = false,
        },
    ) catch {
        ctx.status = boot_static.syscall_err_grant;
        return false;
    };
    if (!user_vm.mapUserLinearRegionWithProt(ctx.child, page.va, page.paddr, 4096, .{ .read = true, .write = page.writable, .exec = true })) {
        ctx.status = boot_static.syscall_err_map;
        return false;
    }
    return true;
}

pub fn forkCurrentReplyTarget(state: *kernel.KernelState) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
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
        abortForkedProcess(h, child);
        return if (copy_context.status != boot_static.syscall_ok) copy_context.status else boot_static.syscall_err_invalid;
    }

    h.state.installEndpoint(child, delegate.endpoint_id, delegate_target) catch {
        abortForkedProcess(h, child);
        return boot_static.syscall_err_endpoint;
    };
    h.state.setAbiTrapDelegate(child, delegate.endpoint_id, delegate.flavor, delegate.request_page_va) catch {
        abortForkedProcess(h, child);
        return boot_static.syscall_err_invalid;
    };

    const child_thread = scheduler.threadSlotForPrincipal(child) orelse {
        abortForkedProcess(h, child);
        return boot_static.syscall_err_invalid;
    };
    const child_ctx = scheduler.getThreadContext(child_thread) orelse {
        abortForkedProcess(h, child);
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

pub fn cloneCurrentReplyTargetShared(state: *kernel.KernelState, child_rsp: u64, child_fs_base: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const target_thread = currentReplyTargetThread() orelse return boot_static.syscall_err_endpoint;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    if (!target_ctx.allocated) return boot_static.syscall_err_endpoint;
    const target_proc = target_ctx.owner_process;
    const delegate = h.state.abiTrapDelegateFor(target_proc) orelse return boot_static.syscall_err_endpoint;
    const delegate_target = h.state.endpointTargetFor(target_proc, delegate.endpoint_id) orelse return boot_static.syscall_err_endpoint;

    const created = process_factory.tryCreateSuspendedUserProcess(
        h.state,
        "linux thread",
        h.user_spaces,
    ) catch return boot_static.syscall_err_alloc;
    const child = created.principal;
    var scratch: [4096]u8 = undefined;
    var share_context = CloneShareContext{
        .h = h,
        .parent = target_proc,
        .child = child,
        .scratch = &scratch,
    };
    if (!user_vm.forEachUserMappedPage(target_proc, @ptrCast(&share_context), shareCloneMappedPage)) {
        abortSharedCloneProcess(h, child);
        return if (share_context.status != boot_static.syscall_ok) share_context.status else boot_static.syscall_err_invalid;
    }

    h.state.installEndpoint(child, delegate.endpoint_id, delegate_target) catch {
        abortSharedCloneProcess(h, child);
        return boot_static.syscall_err_endpoint;
    };
    h.state.setAbiTrapDelegate(child, delegate.endpoint_id, delegate.flavor, delegate.request_page_va) catch {
        abortSharedCloneProcess(h, child);
        return boot_static.syscall_err_invalid;
    };

    const child_thread = scheduler.threadSlotForPrincipal(child) orelse {
        abortSharedCloneProcess(h, child);
        return boot_static.syscall_err_invalid;
    };
    const child_ctx = scheduler.getThreadContext(child_thread) orelse {
        abortSharedCloneProcess(h, child);
        return boot_static.syscall_err_invalid;
    };
    child_ctx.frame = target_ctx.frame;
    child_ctx.frame.rax = 0;
    if (child_rsp != 0) child_ctx.frame.rsp = child_rsp;
    child_ctx.fs_base = if (child_fs_base != 0) child_fs_base else target_ctx.fs_base;
    child_ctx.fx_state = target_ctx.fx_state;
    child_ctx.abi_trap_reply_pending = false;
    const slot = kernel.processIndexFromPrincipal(child) orelse return boot_static.syscall_err_invalid;
    return process_abi.encodeSpawnedProcess(@intCast(slot), @intCast(child_thread));
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

fn dispatchKnownDelegateWithHooks(h: *const Hooks, proc: kernel.PrincipalId, delegate: kernel.AbiTrapDelegate, frame: *TrapFrame) ?u64 {
    const current_thread = scheduler.currentThreadIndex();
    const current_ctx = scheduler.getThreadContext(current_thread) orelse return boot_static.syscall_err_not_ready;
    const target_principal = h.state.endpointTargetForKnownActiveOwner(proc, delegate.endpoint_id) orelse {
        if (dispatch_delegate_lookup_log_count < dispatch_delegate_lookup_log_limit) {
            h.write("abi dispatch endpoint lookup miss proc=");
            h.print_hex(@intFromEnum(proc));
            h.write(" state=");
            h.print_hex(@intFromPtr(h.state));
            h.write(" endpoint=");
            h.print_hex(delegate.endpoint_id);
            h.write(" owner_active=");
            h.print_number(if (h.state.hasActivePrincipal(proc)) 1 else 0);
            if (kernel.processIndexFromPrincipal(proc)) |proc_index| {
                const desc = h.state.process_descriptors[proc_index];
                h.write(" desc_active=");
                h.print_number(if (desc.active) 1 else 0);
                h.write(" desc_delegate=");
                h.print_hex(desc.abi_trap_delegate_endpoint_id);
                h.write(" desc_request=");
                h.print_hex(desc.abi_trap_request_page_va);
                const table = h.state.endpoint_tables[proc_index];
                h.write(" ep_len=");
                h.print_number(@intCast(table.len));
                if (table.find(delegate.endpoint_id)) |ep| {
                    h.write(" ep_target=");
                    h.print_hex(@intFromEnum(ep.target));
                    h.write(" ep_target_active=");
                    h.print_number(if (h.state.hasActivePrincipal(ep.target)) 1 else 0);
                } else {
                    h.write(" ep_missing=1");
                }
            }
            if (h.state.published_service_endpoints.find(delegate.endpoint_id)) |published| {
                h.write(" pub_target=");
                h.print_hex(@intFromEnum(published.target));
                h.write(" pub_active=");
                h.print_number(if (h.state.hasActivePrincipal(published.target)) 1 else 0);
            }
            h.write("\n");
            dispatch_delegate_lookup_log_count += 1;
        }
        return boot_static.syscall_err_endpoint;
    };
    const target_thread = scheduler.threadSlotForPrincipal(target_principal) orelse {
        if (dispatch_delegate_lookup_log_count < dispatch_delegate_lookup_log_limit) {
            h.write("abi dispatch target thread miss proc=");
            h.print_hex(@intFromEnum(proc));
            h.write(" target_proc=");
            h.print_hex(@intFromEnum(target_principal));
            h.write(" target_active=");
            h.print_number(if (h.state.hasActivePrincipal(target_principal)) 1 else 0);
            h.write("\n");
            dispatch_delegate_lookup_log_count += 1;
        }
        return boot_static.syscall_err_endpoint;
    };
    const stale_replies = scheduler.discardIpcReplyMessagesForThreadFromSender(current_thread, target_thread);
    if (stale_replies != 0) current_ctx.abi_trap_reply_pending = false;
    if (!writeRequest(h, target_principal, delegate.request_page_va, proc, @intCast(current_thread), delegate.flavor, frame)) {
        h.write("abi_trap request write failed target=");
        h.write(h.principal_label(target_principal));
        h.write("\n");
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
    if (status != boot_static.syscall_ok) {
        if (dispatch_delegate_failure_log_count < dispatch_delegate_failure_log_limit) {
            h.write("abi dispatch deliver failed status=");
            h.print_hex(status);
            h.write(" proc=");
            h.print_hex(@intFromEnum(proc));
            h.write(" target_proc=");
            h.print_hex(@intFromEnum(target_principal));
            h.write(" target_thread=");
            h.print_number(@intCast(target_thread));
            if (scheduler.getThreadContext(target_thread)) |target_ctx| {
                h.write(" ctx_alloc=");
                h.print_number(if (target_ctx.allocated) 1 else 0);
                h.write(" ctx_ready=");
                h.print_number(if (target_ctx.ready) 1 else 0);
            } else {
                h.write(" ctx_missing=1");
            }
            if (scheduler.getIpcHotThreadConst(target_thread)) |target_hot| {
                h.write(" hot_alloc=");
                h.print_number(target_hot.allocated);
                h.write(" hot_ready=");
                h.print_number(target_hot.ready);
            } else {
                h.write(" hot_missing=1");
            }
            h.write("\n");
            dispatch_delegate_failure_log_count += 1;
        }
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
