/// Suspended process construction syscalls for user-space exec loaders.
const std = @import("std");
const kernel = @import("../kernel.zig");
const capability = @import("../capability.zig");
const scheduler = @import("../scheduler.zig");
const user_vm = @import("../memory/user_vm.zig");
const user_copy = @import("../user_copy.zig");
const boot_static = @import("../boot/main_static.zig");
const boot_abi = @import("../boot/abi.zig");
const process_factory = @import("../boot/process_factory.zig");
const abi_root = @import("kernel_abi_root");

const process_builder_abi = boot_abi.process_builder_abi;
const trap_abi = abi_root.trap_abi;

var state_ptr: *kernel.KernelState = undefined;
var free_list_ptr: *kernel.FreePageList = undefined;
var user_spaces_ptr: []boot_static.UserAddressSpace = undefined;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(state_ptr), &state_ptr));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(free_list_ptr), &free_list_ptr));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_spaces_ptr), &user_spaces_ptr));
    return end;
}

pub fn init(
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    user_spaces: []boot_static.UserAddressSpace,
) void {
    state_ptr = state;
    free_list_ptr = free_list;
    user_spaces_ptr = user_spaces;
}

fn isBuilderAuthorized(caller: kernel.PrincipalId) bool {
    // Temporary authority source: init can spawn the user-space exec service as a
    // bootstrap owner. Keep this behind one predicate so it can become a
    // dedicated process_builder capability without changing syscall handlers.
    return state_ptr.isBootstrapOwner(caller);
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

fn principalFromBuilderToken(caller: kernel.PrincipalId, token: u64) ?kernel.PrincipalId {
    const process_slot = process_builder_abi.decodeProcessBuilderToken(token) orelse return null;
    const principal = kernel.processPrincipalFromIndex(@intCast(process_slot)) orelse return null;
    if (!state_ptr.processBuilderOwnerMatches(principal, caller)) return null;
    return principal;
}

fn processSlot(principal: kernel.PrincipalId) ?u64 {
    return @intCast(kernel.processIndexFromPrincipal(principal) orelse return null);
}

fn clearProcessRuntimeState(principal: kernel.PrincipalId) void {
    const process_index = kernel.processIndexFromPrincipal(principal) orelse return;
    if (scheduler.threadSlotForPrincipal(principal)) |thread_index| {
        _ = scheduler.releaseThreadSlot(thread_index);
    }
    state_ptr.releasePrincipalPageCaps(principal, free_list_ptr);
    if (process_index < user_spaces_ptr.len) {
        user_spaces_ptr[process_index] = .{};
    }
    state_ptr.cap_tables[process_index].reset();
    state_ptr.endpoint_tables[process_index] = .{};
    state_ptr.cap_mailboxes[process_index] = .{};
    state_ptr.pending_page_transfers[process_index] = null;
    state_ptr.vm_object_tables[process_index].reset();
    _ = state_ptr.removeProcessDescriptor(principal);
}

pub fn createSuspendedProcess(caller: kernel.PrincipalId) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const created = process_factory.tryCreateSuspendedUserProcess(
        state_ptr,
        "suspended exec",
        user_spaces_ptr,
    ) catch return boot_static.syscall_err_alloc;
    state_ptr.markProcessBuilderSuspended(created.principal, caller) catch {
        clearProcessRuntimeState(created.principal);
        return boot_static.syscall_err_invalid;
    };
    const slot = processSlot(created.principal) orelse {
        clearProcessRuntimeState(created.principal);
        return boot_static.syscall_err_invalid;
    };
    return process_builder_abi.encodeProcessBuilderToken(slot);
}

pub fn mapVmObjectToProcess(caller: kernel.PrincipalId, token: u64, vm_token: u64, target_va: u64, prot_bits: u64) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    const vm_cap_id = kernel.decodeVmObjectToken(vm_token) orelse return boot_static.syscall_err_invalid;
    const vm_cap = state_ptr.getVmObjectTableConst(caller).findByCapId(vm_cap_id) orelse return boot_static.syscall_err_invalid;
    if (!vm_cap.rights.read or !vm_cap.rights.map) return boot_static.syscall_err_invalid;
    if (prot.write and !vm_cap.rights.write) return boot_static.syscall_err_invalid;
    if (vm_cap.backing.page_offset_bytes != 0) return boot_static.syscall_err_invalid;
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;

    var page_index: usize = 0;
    while (page_index < vm_cap.backing.page_count) {
        const run_start = page_index;
        const run_paddr = vm_cap.backing.pagePaddr(run_start) orelse return boot_static.syscall_err_invalid;
        var run_len: usize = 1;
        while (run_start + run_len < vm_cap.backing.page_count) : (run_len += 1) {
            const expected = run_paddr + @as(u64, @intCast(run_len)) * 4096;
            if ((vm_cap.backing.pagePaddr(run_start + run_len) orelse break) != expected) break;
        }
        const run_va = target_va + @as(u64, @intCast(run_start)) * 4096;
        const run_bytes = run_len * 4096;
        if (!user_vm.mapUserLinearRegionWithProt(target, run_va, run_paddr, run_bytes, prot)) {
            return boot_static.syscall_err_map;
        }
        page_index = run_start + run_len;
    }
    return boot_static.syscall_ok;
}

pub fn allocMapPagesToProcess(
    caller: kernel.PrincipalId,
    token: u64,
    target_va: u64,
    page_count: u64,
    prot_bits: u64,
    out_paddr_list_va: u64,
) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;
    if (page_count == 0 or page_count > boot_static.syscall_batch_max_pages) return boot_static.syscall_err_invalid;

    var page_index: u64 = 0;
    while (page_index < page_count) : (page_index += 1) {
        const page = state_ptr.allocPageTo(target, free_list_ptr) catch return boot_static.syscall_err_alloc;
        const map_va = target_va + page_index * 4096;
        if (!user_vm.mapUserLinearRegionWithProt(target, map_va, page.paddr, 4096, prot)) {
            return boot_static.syscall_err_map;
        }
        if (out_paddr_list_va != 0) {
            const list_va = out_paddr_list_va + page_index * @sizeOf(u64);
            if (!user_copy.writeUserU64(caller, list_va, page.paddr)) return boot_static.syscall_err_map;
        }
    }
    return boot_static.syscall_ok;
}

pub fn copyToProcess(caller: kernel.PrincipalId, token: u64, dest_va: u64, src_va: u64, byte_len: u64) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    if (byte_len > process_builder_abi.copy_to_process_max_bytes) return boot_static.syscall_err_invalid;
    if (byte_len == 0) return boot_static.syscall_ok;

    var buf: [512]u8 = undefined;
    var copied: u64 = 0;
    while (copied < byte_len) {
        const remaining = byte_len - copied;
        const chunk_len_u64 = if (remaining < buf.len) remaining else buf.len;
        const chunk_len: usize = @intCast(chunk_len_u64);
        const src_cur, const src_overflow = @addWithOverflow(src_va, copied);
        if (src_overflow != 0) return boot_static.syscall_err_invalid;
        const dest_cur, const dest_overflow = @addWithOverflow(dest_va, copied);
        if (dest_overflow != 0) return boot_static.syscall_err_invalid;

        const chunk = buf[0..chunk_len];
        if (!user_copy.copyUserBytesFromVa(caller, src_cur, chunk)) return boot_static.syscall_err_invalid;
        if (!user_copy.copyBytesToUserVa(target, dest_cur, chunk)) return boot_static.syscall_err_map;
        copied += chunk_len_u64;
    }
    return boot_static.syscall_ok;
}

pub fn copyFromProcessToProcess(
    caller: kernel.PrincipalId,
    token: u64,
    source_process_slot: u64,
    dest_va: u64,
    src_va: u64,
    byte_len: u64,
) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    const source = kernel.processPrincipalFromIndex(@intCast(source_process_slot)) orelse return boot_static.syscall_err_invalid;
    if (byte_len > process_builder_abi.copy_to_process_max_bytes) return boot_static.syscall_err_invalid;
    if (byte_len == 0) return boot_static.syscall_ok;

    var buf: [512]u8 = undefined;
    var copied: u64 = 0;
    while (copied < byte_len) {
        const remaining = byte_len - copied;
        const chunk_len_u64 = if (remaining < buf.len) remaining else buf.len;
        const chunk_len: usize = @intCast(chunk_len_u64);
        const src_cur, const src_overflow = @addWithOverflow(src_va, copied);
        if (src_overflow != 0) return boot_static.syscall_err_invalid;
        const dest_cur, const dest_overflow = @addWithOverflow(dest_va, copied);
        if (dest_overflow != 0) return boot_static.syscall_err_invalid;

        const chunk = buf[0..chunk_len];
        if (!user_copy.copyUserBytesFromVa(source, src_cur, chunk)) return boot_static.syscall_err_invalid;
        if (!user_copy.copyBytesToUserVa(target, dest_cur, chunk)) return boot_static.syscall_err_map;
        copied += chunk_len_u64;
    }
    return boot_static.syscall_ok;
}

pub fn shareProcessPagesToProcess(
    caller: kernel.PrincipalId,
    token: u64,
    source_process_slot: u64,
    target_va: u64,
    page_count: u64,
    prot_bits: u64,
) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    const source = kernel.processPrincipalFromIndex(@intCast(source_process_slot)) orelse return boot_static.syscall_err_invalid;
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;
    if (page_count == 0 or page_count > boot_static.syscall_batch_max_pages) return boot_static.syscall_err_invalid;

    var page_index: u64 = 0;
    while (page_index < page_count) : (page_index += 1) {
        const va = target_va + page_index * 4096;
        const paddr = capability.lookupUserMappedPaddrForVa(source, va) orelse return boot_static.syscall_err_invalid;
        if (!user_vm.mapUserLinearRegionWithProt(target, va, paddr, 4096, prot)) {
            return boot_static.syscall_err_map;
        }
    }
    return boot_static.syscall_ok;
}

pub fn mprotectSelf(caller: kernel.PrincipalId, target_va: u64, byte_len: u64, prot_bits: u64) u64 {
    const prot = protFromBits(prot_bits) orelse return boot_static.syscall_err_invalid;
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;
    if (byte_len == 0 or (byte_len & 0xFFF) != 0) return boot_static.syscall_err_invalid;
    const end_va, const overflow = @addWithOverflow(target_va, byte_len - 1);
    if (overflow != 0) return boot_static.syscall_err_invalid;
    if (!capability.isUserCanonicalVa(target_va) or !capability.isUserCanonicalVa(end_va)) return boot_static.syscall_err_invalid;
    if (byte_len > std.math.maxInt(usize)) return boot_static.syscall_err_invalid;
    if (!user_vm.protectUserLinearRegionWithProt(caller, target_va, @intCast(byte_len), prot)) {
        return boot_static.syscall_err_map;
    }
    return boot_static.syscall_ok;
}

pub fn setAbiTrapDelegate(
    caller: kernel.PrincipalId,
    token: u64,
    endpoint_id: u64,
    target_process_slot: u64,
    flavor: u64,
    request_page_va: u64,
) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    const endpoint_target = kernel.processPrincipalFromIndex(@intCast(target_process_slot)) orelse return boot_static.syscall_err_invalid;
    state_ptr.installEndpoint(target, endpoint_id, endpoint_target) catch |err| switch (err) {
        kernel.KernelError.EndpointNotFound => return boot_static.syscall_err_endpoint,
        else => return boot_static.syscall_err_invalid,
    };
    state_ptr.setAbiTrapDelegate(target, endpoint_id, @truncate(flavor), request_page_va) catch |err| switch (err) {
        kernel.KernelError.EndpointNotFound => return boot_static.syscall_err_endpoint,
        else => return boot_static.syscall_err_invalid,
    };
    return boot_static.syscall_ok;
}

fn sanitizeUserRflags(rflags: u64) u64 {
    const clear_mask = (@as(u64, 3) << 12) | (@as(u64, 1) << 14) | (@as(u64, 1) << 16) | (@as(u64, 1) << 17);
    return (rflags | boot_static.user_entry_rflags | @as(u64, 2)) & ~clear_mask;
}

fn applyInitialUserContext(target: kernel.PrincipalId, ctx: *scheduler.ThreadContext, user_context: trap_abi.UserContext) u64 {
    if (user_context.rip == 0 or user_context.rsp == 0) return boot_static.syscall_err_invalid;
    if (!capability.isUserCanonicalVa(user_context.rip) or !capability.isUserCanonicalVa(user_context.rsp)) return boot_static.syscall_err_invalid;
    if (user_context.fs_base != 0 and !capability.isUserCanonicalVa(user_context.fs_base)) return boot_static.syscall_err_invalid;
    _ = target;
    ctx.frame.r15 = user_context.r15;
    ctx.frame.r14 = user_context.r14;
    ctx.frame.r13 = user_context.r13;
    ctx.frame.r12 = user_context.r12;
    ctx.frame.r11 = user_context.r11;
    ctx.frame.r10 = user_context.r10;
    ctx.frame.r9 = user_context.r9;
    ctx.frame.r8 = user_context.r8;
    ctx.frame.rbp = user_context.rbp;
    ctx.frame.rdi = user_context.rdi;
    ctx.frame.rsi = user_context.rsi;
    ctx.frame.rdx = user_context.rdx;
    ctx.frame.rcx = user_context.rcx;
    ctx.frame.rbx = user_context.rbx;
    ctx.frame.rax = user_context.rax;
    ctx.frame.rip = user_context.rip;
    ctx.frame.rflags = sanitizeUserRflags(user_context.rflags);
    ctx.frame.rsp = user_context.rsp;
    ctx.fs_base = user_context.fs_base;
    return boot_static.syscall_ok;
}

pub fn setInitialContext(caller: kernel.PrincipalId, token: u64, rip: u64, rsp: u64, context_va: u64) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    const thread_index = scheduler.threadSlotForPrincipal(target) orelse return boot_static.syscall_err_invalid;
    const ctx = scheduler.getThreadContext(thread_index) orelse return boot_static.syscall_err_invalid;
    if (!ctx.allocated or ctx.ready) return boot_static.syscall_err_invalid;
    if (context_va != 0) {
        var user_context: trap_abi.UserContext = .{};
        const bytes = @as([*]u8, @ptrCast(&user_context))[0..@sizeOf(trap_abi.UserContext)];
        if (!user_copy.copyUserBytesFromVa(caller, context_va, bytes)) return boot_static.syscall_err_invalid;
        return applyInitialUserContext(target, ctx, user_context);
    }
    if (!capability.isUserCanonicalVa(rip)) return boot_static.syscall_err_invalid;
    if (!capability.isUserCanonicalVa(rsp)) return boot_static.syscall_err_invalid;
    ctx.frame.rip = rip;
    ctx.frame.rsp = rsp;
    return boot_static.syscall_ok;
}

pub fn setBootstrapOwner(caller: kernel.PrincipalId, token: u64, enabled: u64) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    if (enabled > 1) return boot_static.syscall_err_invalid;
    state_ptr.setBootstrapOwner(target, enabled != 0) catch return boot_static.syscall_err_invalid;
    return boot_static.syscall_ok;
}

pub fn startProcess(caller: kernel.PrincipalId, token: u64) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    const thread_index = scheduler.threadSlotForPrincipal(target) orelse return boot_static.syscall_err_invalid;
    const slot = processSlot(target) orelse return boot_static.syscall_err_invalid;
    state_ptr.clearProcessBuilderSuspended(target) catch return boot_static.syscall_err_invalid;
    if (!scheduler.setThreadReady(thread_index, true)) return boot_static.syscall_err_not_ready;
    scheduler.wakeAssignedApForRunnableThread(thread_index);
    scheduler.preferIpcSwitchToThread(thread_index);
    return boot_abi.process_abi.encodeSpawnedProcess(slot, @intCast(thread_index));
}

pub fn abortProcess(caller: kernel.PrincipalId, token: u64) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    clearProcessRuntimeState(target);
    return boot_static.syscall_ok;
}
