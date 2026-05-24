/// Process creation helpers.
/// All functions are stateless — callers pass KernelState, FreePageList, and
/// user_spaces explicitly so this module has no global dependencies.
const kernel = @import("../kernel.zig");
const boot_static = @import("main_static.zig");
const user_vm = @import("../memory/user_vm.zig");
const scheduler = @import("../scheduler.zig");
const interrupts = @import("../interrupts.zig");
const halt = @import("../halt.zig");
const log_util = @import("../log_util.zig");

const TrapFrame = interrupts.TrapFrame;

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

pub const CreatedUserProcess = struct {
    user_page: kernel.PageCapability,
    user_stack_page: kernel.PageCapability,
    thread_slot: usize,
};

pub const DynamicUserProcess = struct {
    principal: kernel.PrincipalId,
    process: CreatedUserProcess,
};

pub const SuspendedUserProcess = struct {
    principal: kernel.PrincipalId,
    thread_slot: usize,
};

pub const CreateUserProcessError = error{CreateFailed};
pub const CreateDynamicUserProcessError = error{ CreateFailed, NoFreeProcess };

// ---------------------------------------------------------------------------
// Capability rights helpers
// ---------------------------------------------------------------------------

pub fn ownedUserPageRights(writable: bool) kernel.Rights {
    return .{
        .cpu_read = true,
        .cpu_write = writable,
        .dma = false,
        .grant = true,
    };
}

pub fn derivedUserPageRights(writable: bool) kernel.Rights {
    return .{
        .cpu_read = true,
        .cpu_write = writable,
        .dma = false,
    };
}

// ---------------------------------------------------------------------------
// Initial trap frame
// ---------------------------------------------------------------------------

pub fn buildInitialUserTrapFrame() TrapFrame {
    var frame: TrapFrame = @import("std").mem.zeroes(TrapFrame);
    frame.rip = boot_static.user_va;
    frame.cs = @as(u64, boot_static.gdt_user_code_selector) | 0x3;
    frame.rflags = boot_static.user_entry_rflags;
    frame.rsp = boot_static.user_entry_rsp;
    frame.ss = @as(u64, boot_static.gdt_user_data_selector) | 0x3;
    return frame;
}

fn releaseStaleThreadSlot(principal: kernel.PrincipalId) void {
    if (scheduler.threadSlotForPrincipal(principal)) |thread_slot| {
        _ = scheduler.releaseThreadSlot(thread_slot);
    }
}

// ---------------------------------------------------------------------------
// Address space helpers
// ---------------------------------------------------------------------------

pub fn buildUserAddressSpaceFromCapabilities(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    user_page: kernel.PageCapability,
    user_stack_page: kernel.PageCapability,
) bool {
    const table = state.getTableConst(principal);
    const user_cap = table.find(user_page.paddr) orelse return false;
    const stack_cap = table.find(user_stack_page.paddr) orelse return false;
    if (!user_cap.rights.cpu_read or !user_cap.rights.cpu_write) return false;
    if (!stack_cap.rights.cpu_read or !stack_cap.rights.cpu_write) return false;
    return user_vm.buildUserAddressSpace(principal, user_cap.paddr, stack_cap.paddr);
}

// ---------------------------------------------------------------------------
// Process creation
// ---------------------------------------------------------------------------

pub fn tryCreateUserProcess(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    role_label: []const u8,
    free_list: *kernel.FreePageList,
    user_spaces: []boot_static.UserAddressSpace,
) CreateUserProcessError!CreatedUserProcess {
    if (!state.ensureProcessDescriptor(principal, role_label)) {
        log_util.logPrefixedLabelMessage("ensureProcessDescriptor failed for ", role_label, "");
        return error.CreateFailed;
    }
    const user_page = state.allocLowPageTo(principal, free_list) catch |err| {
        log_util.logLabelStepError("allocLowPageTo for ", role_label, " user map failed", err);
        return error.CreateFailed;
    };
    const user_stack_page = state.allocLowPageTo(principal, free_list) catch |err| {
        log_util.logLabelStepError("allocLowPageTo for ", role_label, " user stack failed", err);
        return error.CreateFailed;
    };
    if (!buildUserAddressSpaceFromCapabilities(state, principal, user_page, user_stack_page)) {
        log_util.logLabelMessage(role_label, " process page table build failed");
        return error.CreateFailed;
    }
    const thread_slot = scheduler.allocateThreadSlot(principal, user_spaces, buildInitialUserTrapFrame()) orelse {
        log_util.logLabelMessage(role_label, " thread context init failed");
        return error.CreateFailed;
    };
    return .{
        .user_page = user_page,
        .user_stack_page = user_stack_page,
        .thread_slot = thread_slot,
    };
}

pub fn createUserProcess(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    role_label: []const u8,
    free_list: *kernel.FreePageList,
    user_spaces: []boot_static.UserAddressSpace,
) CreatedUserProcess {
    return tryCreateUserProcess(state, principal, role_label, free_list, user_spaces) catch halt.haltLoop();
}

pub fn tryCreateDynamicUserProcess(
    state: *kernel.KernelState,
    role_label: []const u8,
    free_list: *kernel.FreePageList,
    user_spaces: []boot_static.UserAddressSpace,
) CreateDynamicUserProcessError!DynamicUserProcess {
    const principal = state.createProcessDescriptor(role_label) orelse return error.NoFreeProcess;
    releaseStaleThreadSlot(principal);
    const process = tryCreateUserProcess(state, principal, role_label, free_list, user_spaces) catch return error.CreateFailed;
    return .{
        .principal = principal,
        .process = process,
    };
}

pub fn tryCreateSuspendedUserProcess(
    state: *kernel.KernelState,
    role_label: []const u8,
    user_spaces: []boot_static.UserAddressSpace,
) CreateDynamicUserProcessError!SuspendedUserProcess {
    const principal = state.createProcessDescriptor(role_label) orelse return error.NoFreeProcess;
    releaseStaleThreadSlot(principal);
    if (!user_vm.buildEmptyUserAddressSpace(principal)) {
        _ = state.removeProcessDescriptor(principal);
        return error.CreateFailed;
    }
    const thread_slot = scheduler.allocateSuspendedThreadSlot(principal, user_spaces, buildInitialUserTrapFrame()) orelse {
        _ = state.removeProcessDescriptor(principal);
        return error.CreateFailed;
    };
    return .{
        .principal = principal,
        .thread_slot = thread_slot,
    };
}

// ---------------------------------------------------------------------------
// Page allocation helpers
// ---------------------------------------------------------------------------

pub fn allocPageForProcessOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    role_label: []const u8,
    page_label: []const u8,
    free_list: *kernel.FreePageList,
) kernel.PageCapability {
    return state.allocPageTo(principal, free_list) catch |err| {
        halt.haltWithRolePageError(role_label, page_label, "allocPageTo", err);
    };
}

pub fn installPageForProcessOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page: kernel.PageCapability,
    writable: bool,
    role_label: []const u8,
    page_label: []const u8,
) void {
    state.installCap(principal, page.paddr, ownedUserPageRights(writable)) catch |err| {
        halt.haltWithRolePageError(role_label, page_label, "cap install", err);
    };
}

pub fn grantPageForProcessOrHalt(
    state: *kernel.KernelState,
    from_principal: kernel.PrincipalId,
    to_principal: kernel.PrincipalId,
    page: kernel.PageCapability,
    writable: bool,
    role_label: []const u8,
    page_label: []const u8,
) void {
    state.grantCap(from_principal, to_principal, page.paddr, derivedUserPageRights(writable)) catch |err| {
        halt.haltWithRolePageError(role_label, page_label, "cap grant", err);
    };
}

// ---------------------------------------------------------------------------
// Page mapping helpers
// ---------------------------------------------------------------------------

pub fn mapUserLinearRegionOrHalt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddr_start: u64,
    size_bytes: usize,
    writable: bool,
    what: []const u8,
) void {
    if (!user_vm.mapUserLinearRegion(principal, va_start, paddr_start, size_bytes, writable)) {
        halt.haltWithMessage(what);
    }
}

pub fn mapUserPageOrHalt(
    principal: kernel.PrincipalId,
    va_start: u64,
    page: kernel.PageCapability,
    writable: bool,
    role_label: []const u8,
    page_label: []const u8,
) void {
    if (!user_vm.mapUserLinearRegion(principal, va_start, page.paddr, 4096, writable)) {
        halt.haltWithRolePageMessage(role_label, page_label, "map failed");
    }
}

// ---------------------------------------------------------------------------
// Combined alloc+map+install helpers
// ---------------------------------------------------------------------------

pub fn allocAndMapOwnedPageForProcessOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    role_label: []const u8,
    page_label: []const u8,
    va_start: u64,
    writable: bool,
    free_list: *kernel.FreePageList,
) kernel.PageCapability {
    const page = allocPageForProcessOrHalt(state, principal, role_label, page_label, free_list);
    mapUserPageOrHalt(principal, va_start, page, writable, role_label, page_label);
    return page;
}

pub fn installAndMapPageForProcessOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page: kernel.PageCapability,
    va_start: u64,
    writable: bool,
    role_label: []const u8,
    page_label: []const u8,
) void {
    installPageForProcessOrHalt(state, principal, page, writable, role_label, page_label);
    mapUserPageOrHalt(principal, va_start, page, writable, role_label, page_label);
}

pub fn grantAndMapPageForProcessOrHalt(
    state: *kernel.KernelState,
    from_principal: kernel.PrincipalId,
    to_principal: kernel.PrincipalId,
    page: kernel.PageCapability,
    va_start: u64,
    writable: bool,
    role_label: []const u8,
    page_label: []const u8,
) void {
    grantPageForProcessOrHalt(state, from_principal, to_principal, page, writable, role_label, page_label);
    mapUserPageOrHalt(to_principal, va_start, page, writable, role_label, page_label);
}

// ---------------------------------------------------------------------------
// Endpoint installation
// ---------------------------------------------------------------------------

pub fn installSpawnParentEndpoint(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    parent: kernel.PrincipalId,
) !void {
    const endpoint_to_spawn_parent: u64 = 0x14;
    if (!state.hasActivePrincipal(parent)) return;
    try state.installEndpoint(principal, endpoint_to_spawn_parent, parent);
}
