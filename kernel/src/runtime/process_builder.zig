/// Suspended process construction syscalls for user-space exec loaders.
const kernel = @import("../kernel.zig");
const capability = @import("../capability.zig");
const scheduler = @import("../scheduler.zig");
const user_vm = @import("../memory/user_vm.zig");
const user_copy = @import("../user_copy.zig");
const boot_static = @import("../boot/main_static.zig");
const boot_abi = @import("../boot/abi.zig");
const process_factory = @import("../boot/process_factory.zig");

const process_builder_abi = boot_abi.process_builder_abi;

var state_ptr: *kernel.KernelState = undefined;
var free_list_ptr: *kernel.FreePageList = undefined;
var user_spaces_ptr: []boot_static.UserAddressSpace = undefined;

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
    // Temporary authority source: init can spawn the future exec_loader as a
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
    if (process_index < user_spaces_ptr.len) {
        user_spaces_ptr[process_index] = .{};
    }
    state_ptr.cap_tables[process_index] = .{};
    state_ptr.untyped_tables[process_index] = .{};
    state_ptr.endpoint_tables[process_index] = .{};
    state_ptr.cap_mailboxes[process_index] = .{};
    state_ptr.pending_page_transfers[process_index] = null;
    state_ptr.vm_object_tables[process_index] = .{};
    state_ptr.exec_image_tables[process_index] = .{};
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
    const vm_cap_id = boot_abi.image_abi.decodeVmObjectToken(vm_token) orelse return boot_static.syscall_err_invalid;
    const vm_cap = state_ptr.getVmObjectTableConst(caller).findByCapId(vm_cap_id) orelse return boot_static.syscall_err_invalid;
    if (!vm_cap.rights.read or !vm_cap.rights.map) return boot_static.syscall_err_invalid;
    if (prot.write and !vm_cap.rights.write) return boot_static.syscall_err_invalid;
    if (vm_cap.backing.page_offset_bytes != 0) return boot_static.syscall_err_invalid;
    if ((target_va & 0xFFF) != 0) return boot_static.syscall_err_invalid;

    var page_index: usize = 0;
    while (page_index < vm_cap.backing.page_count) {
        const run_start = page_index;
        const run_paddr = vm_cap.backing.page_paddrs[run_start];
        var run_len: usize = 1;
        while (run_start + run_len < vm_cap.backing.page_count) : (run_len += 1) {
            const expected = run_paddr + @as(u64, @intCast(run_len)) * 4096;
            if (vm_cap.backing.page_paddrs[run_start + run_len] != expected) break;
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

pub fn setInitialContext(caller: kernel.PrincipalId, token: u64, rip: u64, rsp: u64) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    if (!capability.isUserCanonicalVa(rip)) return boot_static.syscall_err_invalid;
    if (!capability.isUserCanonicalVa(rsp)) return boot_static.syscall_err_invalid;
    const thread_index = scheduler.threadSlotForPrincipal(target) orelse return boot_static.syscall_err_invalid;
    const ctx = scheduler.getThreadContext(thread_index) orelse return boot_static.syscall_err_invalid;
    if (!ctx.allocated or ctx.ready) return boot_static.syscall_err_invalid;
    ctx.frame.rip = rip;
    ctx.frame.rsp = rsp;
    return boot_static.syscall_ok;
}

pub fn startProcess(caller: kernel.PrincipalId, token: u64) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    const thread_index = scheduler.threadSlotForPrincipal(target) orelse return boot_static.syscall_err_invalid;
    state_ptr.clearProcessBuilderSuspended(target) catch return boot_static.syscall_err_invalid;
    if (!scheduler.setThreadReady(thread_index, true)) return boot_static.syscall_err_not_ready;
    const slot = processSlot(target) orelse return boot_static.syscall_err_invalid;
    return boot_abi.process_abi.encodeSpawnedProcess(slot, @intCast(thread_index));
}

pub fn abortProcess(caller: kernel.PrincipalId, token: u64) u64 {
    if (!isBuilderAuthorized(caller)) return boot_static.syscall_err_invalid;
    const target = principalFromBuilderToken(caller, token) orelse return boot_static.syscall_err_invalid;
    clearProcessRuntimeState(target);
    return boot_static.syscall_ok;
}
