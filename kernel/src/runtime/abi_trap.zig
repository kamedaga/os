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
var dispatch_delegate_backpressure_log_count: u64 = 0;
const dispatch_delegate_backpressure_log_limit: u64 = 1;
const delegate_transport_queue_limit: usize = 1;

const ProfileSlot = struct {
    count: u64 = 0,
    cycles: u64 = 0,
    max_cycles: u64 = 0,
};

const AbiTrapProfile = struct {
    unmap_collect: ProfileSlot = .{},
    unmap_collected_mismatch: ProfileSlot = .{},
    unmap_cap_scan: ProfileSlot = .{},
    unmap_cap_scan_fail: ProfileSlot = .{},
    unmap_cap_linear_found: ProfileSlot = .{},
    unmap_cap_linear_missing: ProfileSlot = .{},
    unmap_cap_scan_skip: ProfileSlot = .{},
    unmap_pte: ProfileSlot = .{},
    unmap_reclaim: ProfileSlot = .{},
};

var abi_trap_profile: AbiTrapProfile = .{};

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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(dispatch_delegate_failure_log_count), &dispatch_delegate_failure_log_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(dispatch_delegate_lookup_log_count), &dispatch_delegate_lookup_log_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(dispatch_delegate_backpressure_log_count), &dispatch_delegate_backpressure_log_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(abi_trap_profile), &abi_trap_profile));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(dispatch_delegate_lock), &dispatch_delegate_lock));
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

fn readTsc() u64 {
    var lo: u32 = 0;
    var hi: u32 = 0;
    asm volatile ("rdtsc"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
    );
    return (@as(u64, hi) << 32) | @as(u64, lo);
}

fn profileRecord(slot: *ProfileSlot, cycles: u64) void {
    slot.count +%= 1;
    slot.cycles +%= cycles;
    if (cycles > slot.max_cycles) slot.max_cycles = cycles;
}

pub fn profileReset() void {
    abi_trap_profile = .{};
}

fn profileReportSlot(
    write: *const fn ([]const u8) void,
    print_number: *const fn (u64) void,
    name: []const u8,
    slot: ProfileSlot,
) void {
    if (slot.count == 0) return;
    write("AbiTrapProfile.item ");
    write(name);
    write(" count=");
    print_number(slot.count);
    write(" cycles=");
    print_number(slot.cycles);
    write(" max=");
    print_number(slot.max_cycles);
    write("\n");
}

pub fn profileReport(write: *const fn ([]const u8) void, print_number: *const fn (u64) void) void {
    write("AbiTrapProfile.begin\n");
    profileReportSlot(write, print_number, "unmap_collect", abi_trap_profile.unmap_collect);
    profileReportSlot(write, print_number, "unmap_collected_mismatch", abi_trap_profile.unmap_collected_mismatch);
    profileReportSlot(write, print_number, "unmap_cap_scan", abi_trap_profile.unmap_cap_scan);
    profileReportSlot(write, print_number, "unmap_cap_scan_fail", abi_trap_profile.unmap_cap_scan_fail);
    profileReportSlot(write, print_number, "unmap_cap_linear_found", abi_trap_profile.unmap_cap_linear_found);
    profileReportSlot(write, print_number, "unmap_cap_linear_missing", abi_trap_profile.unmap_cap_linear_missing);
    profileReportSlot(write, print_number, "unmap_cap_scan_skip", abi_trap_profile.unmap_cap_scan_skip);
    profileReportSlot(write, print_number, "unmap_pte", abi_trap_profile.unmap_pte);
    profileReportSlot(write, print_number, "unmap_reclaim", abi_trap_profile.unmap_reclaim);
    write("AbiTrapProfile.end\n");
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
    if (target_raw >= kernel.process_count) return null;
    return @enumFromInt(@as(u8, @intCast(target_raw)));
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

pub fn replyToTarget(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, result: u64, flags: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const exit_response = (flags & trap_abi.response_flag_exit) != 0;
    const target = resolveTarget(h, proc, target_raw, if (exit_response) .exit_response else .deferred_reply) orelse
        return boot_static.syscall_err_endpoint;
    if (exit_response) {
        _ = scheduler.releaseThreadSlot(target.thread);
        _ = h.state.markProcessExited(target.proc);
        _ = reclaimPrivatePagesForProcessWithHooks(h, target.proc);
        return boot_static.syscall_ok;
    }
    return ipc.deliverOrQueueMessageToThread(target.thread, 0, scheduler.currentThreadIndex(), false, result, flags, 0, 0);
}

pub fn startTarget(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    if (targetPrincipalFromRaw(target_raw) == null) return boot_static.syscall_err_invalid;
    const target = resolveTarget(h, proc, target_raw, .delegated) orelse return boot_static.syscall_err_endpoint;
    if (target.ctx.ready or target.ctx.abi_trap_reply_pending) return boot_static.syscall_err_invalid;
    if (!scheduler.setThreadReady(target.thread, true)) return boot_static.syscall_err_not_ready;
    scheduler.wakeAssignedApForRunnableThread(target.thread);
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

pub fn unmapCurrentReplyTargetPages(state: *kernel.KernelState, target_va: u64, page_count: u64) u64 {
    _ = state;
    const byte_len = pageBatchByteLen(target_va, page_count) orelse return boot_static.syscall_err_invalid;
    const target = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    var paddrs: [boot_static.syscall_batch_max_pages]u64 = undefined;
    var profile_start = readTsc();
    const collected = user_vm.collectUserLinearRegionPaddrs(target.proc, target_va, byte_len, paddrs[0..]) orelse return boot_static.syscall_err_map;
    profileRecord(&abi_trap_profile.unmap_collect, readTsc() - profile_start);
    if (collected != page_count) {
        profileRecord(&abi_trap_profile.unmap_collected_mismatch, 0);
        return boot_static.syscall_err_map;
    }
    profile_start = readTsc();
    profileRecord(&abi_trap_profile.unmap_cap_scan_skip, readTsc() - profile_start);
    profile_start = readTsc();
    if (!user_vm.unmapUserLinearRegion(target.proc, target_va, byte_len)) {
        return boot_static.syscall_err_map;
    }
    profileRecord(&abi_trap_profile.unmap_pte, readTsc() - profile_start);
    return boot_static.syscall_ok;
}

pub fn unmapTargetPages(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, target_va: u64, page_count: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const byte_len = pageBatchByteLen(target_va, page_count) orelse return boot_static.syscall_err_invalid;
    const target = resolveTarget(h, proc, target_raw, .delegated) orelse return boot_static.syscall_err_endpoint;
    if (!user_vm.unmapUserLinearRegion(target.proc, target_va, byte_len)) {
        return boot_static.syscall_err_map;
    }
    return boot_static.syscall_ok;
}

pub fn shareCurrentReplyTargetPagesToTarget(state: *kernel.KernelState, proc: kernel.PrincipalId, target_raw: u64, target_va: u64, page_count: u64, prot_bits: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const byte_len = pageBatchByteLen(target_va, page_count) orelse return boot_static.syscall_err_invalid;
    const source = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    const target = resolveTarget(h, proc, target_raw, .delegated) orelse return boot_static.syscall_err_endpoint;
    const target_proc = target.proc;
    if (target_proc == source.proc) return boot_static.syscall_ok;

    var paddrs: [boot_static.syscall_batch_max_pages]u64 = undefined;
    const collected = user_vm.collectUserLinearRegionPaddrs(source.proc, target_va, byte_len, paddrs[0..]) orelse return boot_static.syscall_err_map;
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
            const source_cap = h.state.getTableConst(source.proc).find(paddr) orelse return boot_static.syscall_err_invalid;
            if (!source_cap.rights.cpu_read or (prot.write and !source_cap.rights.cpu_write)) return boot_static.syscall_err_grant;
            h.state.deriveCapForSharedAddressSpace(
                source.proc,
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
    var total_reclaimed: u64 = 0;
    while (true) {
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
        if (count == 0) break;

        var reclaimed_this_round: u64 = 0;
        for (pages[0..count]) |page| {
            if (!user_vm.unmapUserLinearRegion(target_proc, page.va, 4096)) continue;
            const paddr = page.paddr;
            h.state.reclaimExclusiveRootPage(target_proc, paddr, h.free_list) catch return boot_static.syscall_err_invalid;
            reclaimed_this_round += 1;
        }
        if (reclaimed_this_round == 0) break;
        total_reclaimed += reclaimed_this_round;
    }
    return total_reclaimed;
}

pub fn reclaimPrivatePagesForProcess(state: *kernel.KernelState, target_proc: kernel.PrincipalId) u64 {
    var h_storage = hooksForState(state);
    return reclaimPrivatePagesForProcessWithHooks(&h_storage, target_proc);
}

pub fn reclaimCurrentReplyTargetPrivatePages(state: *kernel.KernelState) u64 {
    var h_storage = hooksForState(state);
    const target = currentReplyTarget() orelse return boot_static.syscall_err_endpoint;
    return reclaimPrivatePagesForProcessWithHooks(&h_storage, target.proc);
}

fn abortChildProcess(h: *const Hooks, principal: kernel.PrincipalId, reclaim_pages: bool) void {
    if (reclaim_pages) _ = reclaimPrivatePagesForProcessWithHooks(h, principal);
    if (scheduler.threadSlotForPrincipal(principal)) |thread_index| {
        _ = scheduler.releaseThreadSlot(thread_index);
    }
    _ = h.state.markProcessExited(principal);
}

const ReplyTarget = struct {
    ctx: *scheduler.ThreadContext,
    proc: kernel.PrincipalId,
    delegate: kernel.AbiTrapDelegate,
    delegate_target: kernel.PrincipalId,
};

fn currentReplyTargetWithDelegate(h: *const Hooks) ?ReplyTarget {
    const target = currentReplyTarget() orelse return null;
    const delegate = h.state.abiTrapDelegateFor(target.proc) orelse return null;
    const delegate_target = h.state.endpointTargetFor(target.proc, delegate.endpoint_id) orelse return null;
    return .{
        .ctx = target.ctx,
        .proc = target.proc,
        .delegate = delegate,
        .delegate_target = delegate_target,
    };
}

const ChildPageContext = struct {
    h: *const Hooks,
    parent: kernel.PrincipalId,
    child: kernel.PrincipalId,
    scratch: *[4096]u8,
    status: u64 = boot_static.syscall_ok,

    fn copyMappedPage(ctx: *ChildPageContext, page: user_vm.MappedUserPage) bool {
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
};

fn copyChildMappedPage(context: *anyopaque, page: user_vm.MappedUserPage) bool {
    const ctx: *ChildPageContext = @ptrCast(@alignCast(context));
    return ctx.copyMappedPage(page);
}

fn shareChildMappedPage(context: *anyopaque, page: user_vm.MappedUserPage) bool {
    const ctx: *ChildPageContext = @ptrCast(@alignCast(context));
    const src_cap = ctx.h.state.getTableConst(ctx.parent).find(page.paddr) orelse {
        return ctx.copyMappedPage(page);
    };
    if (!src_cap.rights.cpu_read) {
        return ctx.copyMappedPage(page);
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

fn installChildDelegate(h: *const Hooks, child: kernel.PrincipalId, target: ReplyTarget) u64 {
    h.state.installEndpoint(child, target.delegate.endpoint_id, target.delegate_target) catch return boot_static.syscall_err_endpoint;
    h.state.setAbiTrapDelegate(child, target.delegate.endpoint_id, target.delegate.flavor, target.delegate.request_page_va) catch return boot_static.syscall_err_invalid;
    return boot_static.syscall_ok;
}

fn initChildThreadFromTarget(child: kernel.PrincipalId, target: ReplyTarget, child_rsp: u64, child_fs_base: u64) ?usize {
    const child_thread = scheduler.threadSlotForPrincipal(child) orelse return null;
    const child_ctx = scheduler.getThreadContext(child_thread) orelse return null;
    child_ctx.frame = target.ctx.frame;
    child_ctx.frame.rax = 0;
    if (child_rsp != 0) child_ctx.frame.rsp = child_rsp;
    child_ctx.fs_base = if (child_fs_base != 0) child_fs_base else target.ctx.fs_base;
    child_ctx.fx_state = target.ctx.fx_state;
    child_ctx.abi_trap_reply_pending = false;
    return child_thread;
}

fn encodeChildProcess(child: kernel.PrincipalId, child_thread: usize) u64 {
    const slot = kernel.processIndexFromPrincipal(child) orelse return boot_static.syscall_err_invalid;
    return process_abi.encodeSpawnedProcess(@intCast(slot), @intCast(child_thread));
}

const ChildMapMode = enum { copy_private, share_readable };

fn spawnReplyTargetChild(
    h: *const Hooks,
    target: ReplyTarget,
    name: []const u8,
    mode: ChildMapMode,
    child_rsp: u64,
    child_fs_base: u64,
) u64 {
    const created = process_factory.tryCreateSuspendedUserProcess(h.state, name, h.user_spaces) catch return boot_static.syscall_err_alloc;
    const child = created.principal;
    var scratch: [4096]u8 = undefined;
    var context = ChildPageContext{
        .h = h,
        .parent = target.proc,
        .child = child,
        .scratch = &scratch,
    };
    const visitor: user_vm.MappedUserPageVisitor = switch (mode) {
        .copy_private => copyChildMappedPage,
        .share_readable => shareChildMappedPage,
    };
    if (!user_vm.forEachUserMappedPage(target.proc, @ptrCast(&context), visitor)) {
        abortChildProcess(h, child, mode == .copy_private);
        return if (context.status != boot_static.syscall_ok) context.status else boot_static.syscall_err_invalid;
    }
    const delegate_status = installChildDelegate(h, child, target);
    if (delegate_status != boot_static.syscall_ok) {
        abortChildProcess(h, child, mode == .copy_private);
        return delegate_status;
    }
    const child_thread = initChildThreadFromTarget(child, target, child_rsp, child_fs_base) orelse {
        abortChildProcess(h, child, mode == .copy_private);
        return boot_static.syscall_err_invalid;
    };
    return encodeChildProcess(child, child_thread);
}

pub fn forkCurrentReplyTarget(state: *kernel.KernelState) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const target = currentReplyTargetWithDelegate(h) orelse return boot_static.syscall_err_endpoint;
    return spawnReplyTargetChild(h, target, "forked linux", .copy_private, 0, 0);
}

pub fn cloneCurrentReplyTargetShared(state: *kernel.KernelState, child_rsp: u64, child_fs_base: u64) u64 {
    var h_storage = hooksForState(state);
    const h = &h_storage;
    const target = currentReplyTargetWithDelegate(h) orelse return boot_static.syscall_err_endpoint;
    return spawnReplyTargetChild(h, target, "linux thread", .share_readable, child_rsp, child_fs_base);
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

fn deliverDelegateRequestLocked(
    h: *const Hooks,
    proc: kernel.PrincipalId,
    target_principal: kernel.PrincipalId,
    target_thread: usize,
    delegate: kernel.AbiTrapDelegate,
    frame: *TrapFrame,
    current_thread: usize,
    current_ctx: *scheduler.ThreadContext,
) u64 {
    const stale_replies = scheduler.discardIpcReplyMessagesForThreadFromSender(current_thread, target_thread);
    if (stale_replies != 0) current_ctx.abi_trap_reply_pending = false;
    const stale_requests = scheduler.discardIpcMessagesForThreadFromSenderOnEndpoint(target_thread, current_thread, delegate.endpoint_id, true);
    if (stale_requests != 0) current_ctx.abi_trap_reply_pending = false;
    if (!writeRequest(h, target_principal, delegate.request_page_va, proc, @intCast(current_thread), delegate.flavor, frame)) {
        h.write("abi_trap request write failed target=");
        h.write(h.principal_label(target_principal));
        h.write("\n");
        return boot_static.syscall_err_invalid;
    }

    current_ctx.abi_trap_reply_pending = true;
    if (scheduler.ipcQueueLenForThreadOnEndpoint(target_thread, delegate.endpoint_id, true) >= delegate_transport_queue_limit) {
        if (scheduler.enqueueDelegateSendPending(target_thread, delegate.endpoint_id, current_thread, delegate.request_page_va)) {
            if (dispatch_delegate_backpressure_log_count < dispatch_delegate_backpressure_log_limit) {
                h.write("abi dispatch backpressure proc=");
                h.print_hex(@intFromEnum(proc));
                h.write(" target_proc=");
                h.print_hex(@intFromEnum(target_principal));
                h.write(" target_thread=");
                h.print_number(@intCast(target_thread));
                h.write(" qlen=");
                h.print_number(@intCast(scheduler.ipcQueueLenForThread(target_thread)));
                h.write(" delegate_qlen=");
                h.print_number(@intCast(scheduler.ipcQueueLenForThreadOnEndpoint(target_thread, delegate.endpoint_id, true)));
                h.write(" pending=");
                h.print_number(@intCast(scheduler.delegateSendPendingLenForThread(target_thread)));
                h.write("\n");
                dispatch_delegate_backpressure_log_count += 1;
            }
            scheduler.wakeAssignedApForRunnableThread(target_thread);
            scheduler.preferIpcSwitchToThread(target_thread);
            return boot_static.syscall_ok;
        }
        current_ctx.abi_trap_reply_pending = false;
        return boot_static.syscall_err_not_ready;
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
    if (status == boot_static.syscall_ok) return boot_static.syscall_ok;

    if (status == boot_static.syscall_err_not_ready) {
        if (scheduler.enqueueDelegateSendPending(target_thread, delegate.endpoint_id, current_thread, delegate.request_page_va)) {
            if (dispatch_delegate_backpressure_log_count < dispatch_delegate_backpressure_log_limit) {
                h.write("abi dispatch backpressure proc=");
                h.print_hex(@intFromEnum(proc));
                h.write(" target_proc=");
                h.print_hex(@intFromEnum(target_principal));
                h.write(" target_thread=");
                h.print_number(@intCast(target_thread));
                h.write(" qlen=");
                h.print_number(@intCast(scheduler.ipcQueueLenForThread(target_thread)));
                h.write(" delegate_qlen=");
                h.print_number(@intCast(scheduler.ipcQueueLenForThreadOnEndpoint(target_thread, delegate.endpoint_id, true)));
                h.write(" pending=");
                h.print_number(@intCast(scheduler.delegateSendPendingLenForThread(target_thread)));
                h.write("\n");
                dispatch_delegate_backpressure_log_count += 1;
            }
            scheduler.wakeAssignedApForRunnableThread(target_thread);
            scheduler.preferIpcSwitchToThread(target_thread);
            return boot_static.syscall_ok;
        }
        if (dispatch_delegate_backpressure_log_count < dispatch_delegate_backpressure_log_limit) {
            h.write("abi dispatch backpressure drop proc=");
            h.print_hex(@intFromEnum(proc));
            h.write(" target_proc=");
            h.print_hex(@intFromEnum(target_principal));
            h.write(" target_thread=");
            h.print_number(@intCast(target_thread));
            h.write(" qlen=");
            h.print_number(@intCast(scheduler.ipcQueueLenForThread(target_thread)));
            h.write(" delegate_qlen=");
            h.print_number(@intCast(scheduler.ipcQueueLenForThreadOnEndpoint(target_thread, delegate.endpoint_id, true)));
            h.write(" pending=");
            h.print_number(@intCast(scheduler.delegateSendPendingLenForThread(target_thread)));
            h.write("\n");
            dispatch_delegate_backpressure_log_count += 1;
        }
    }
    current_ctx.abi_trap_reply_pending = false;
    if (status != boot_static.syscall_err_not_ready) logDelegateDeliveryFailure(h, proc, target_principal, target_thread, status);
    return status;
}

fn logDelegateDeliveryFailure(
    h: *const Hooks,
    proc: kernel.PrincipalId,
    target_principal: kernel.PrincipalId,
    target_thread: usize,
    status: u64,
) void {
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
    dispatch_delegate_lock.lock();
    const status = deliverDelegateRequestLocked(h, proc, target_principal, target_thread, delegate, frame, current_thread, current_ctx);
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
