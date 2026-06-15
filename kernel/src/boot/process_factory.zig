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

pub fn buildUserAddressSpaceFromPages(
    principal: kernel.PrincipalId,
    user_page: kernel.PageCapability,
    user_stack_page: kernel.PageCapability,
) bool {
    if ((user_page.paddr & 0xFFF) != 0 or (user_stack_page.paddr & 0xFFF) != 0) return false;
    return user_vm.buildUserAddressSpace(principal, user_page.paddr, user_stack_page.paddr);
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
    const user_page = state.allocLowPhysicalPage(free_list) catch |err| {
        log_util.logLabelStepError("allocLowPhysicalPage for ", role_label, " user map failed", err);
        return error.CreateFailed;
    };
    const user_stack_page = state.allocLowPhysicalPage(free_list) catch |err| {
        log_util.logLabelStepError("allocLowPhysicalPage for ", role_label, " user stack failed", err);
        return error.CreateFailed;
    };
    if (!buildUserAddressSpaceFromPages(principal, user_page, user_stack_page)) {
        log_util.logLabelMessage(role_label, " process page table build failed");
        return error.CreateFailed;
    }
    trackMappedNativePageOrHalt(state, principal, boot_static.user_va, user_page, true, true, role_label, "user map", free_list);
    trackMappedNativePageOrHalt(state, principal, boot_static.user_stack_page_va, user_stack_page, true, false, role_label, "user stack", free_list);
    const thread_slot = scheduler.allocateThreadSlot(principal, user_spaces, buildInitialUserTrapFrame(), free_list) orelse {
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
    const principal = state.createProcessDescriptorWithCapacity(role_label, free_list) orelse return error.NoFreeProcess;
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
    free_list: *kernel.FreePageList,
    user_spaces: []boot_static.UserAddressSpace,
) CreateDynamicUserProcessError!SuspendedUserProcess {
    const principal = state.createProcessDescriptorWithCapacity(role_label, free_list) orelse return error.NoFreeProcess;
    releaseStaleThreadSlot(principal);
    if (!user_vm.buildEmptyUserAddressSpace(principal)) {
        _ = state.removeProcessDescriptor(principal);
        return error.CreateFailed;
    }
    const thread_slot = scheduler.allocateSuspendedThreadSlot(principal, user_spaces, buildInitialUserTrapFrame(), free_list) orelse {
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
    state.requireActiveProcess(principal) catch |err| {
        halt.haltWithRolePageError(role_label, page_label, "require active process", err);
    };
    return state.allocPhysicalPage(free_list) catch |err| {
        halt.haltWithRolePageError(role_label, page_label, "allocPhysicalPage", err);
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
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    va_start: u64,
    page: kernel.PageCapability,
    writable: bool,
    role_label: []const u8,
    page_label: []const u8,
    free_list: *kernel.FreePageList,
) void {
    if (!user_vm.mapUserLinearRegion(principal, va_start, page.paddr, 4096, writable)) {
        halt.haltWithRolePageMessage(role_label, page_label, "map failed");
    }
    trackMappedNativePageOrHalt(state, principal, va_start, page, writable, false, role_label, page_label, free_list);
}

pub fn trackMappedNativePageOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    va_start: u64,
    page: kernel.PageCapability,
    writable: bool,
    executable: bool,
    role_label: []const u8,
    page_label: []const u8,
    free_list: *kernel.FreePageList,
) void {
    if ((va_start & 0xFFF) != 0 or (page.paddr & 0xFFF) != 0) {
        halt.haltWithRolePageMessage(role_label, page_label, "unaligned native VMA page");
    }
    const fd = state.createAnonymousVmoFd(
        principal,
        4096,
        .{
            .map_read = true,
            .map_write = writable,
            .map_exec = executable,
            .close = true,
        },
        .{ .private = true },
        0,
    ) catch |err| {
        halt.haltWithRolePageError(role_label, page_label, "create native VMO fd", err);
    };
    const vmo_ref = state.nativeVmoRefForFd(principal, fd) orelse {
        halt.haltWithRolePageMessage(role_label, page_label, "native VMO fd missing");
    };
    var paddrs = [_]u64{page.paddr};
    state.installNativeVmoPages(vmo_ref, 0, paddrs[0..]) catch |err| {
        _ = state.closeFdWithFreeList(principal, fd, free_list) catch {};
        halt.haltWithRolePageError(role_label, page_label, "install native VMO page", err);
    };
    _ = state.mmapFd(
        principal,
        fd,
        va_start,
        4096,
        .{ .read = true, .write = writable, .exec = executable },
        .{ .anonymous = true, .private = true, .fixed = true },
        0,
    ) catch |err| {
        _ = state.closeFdWithFreeList(principal, fd, free_list) catch {};
        halt.haltWithRolePageError(role_label, page_label, "track native VMA", err);
    };
    state.closeFdWithFreeList(principal, fd, free_list) catch |err| {
        halt.haltWithRolePageError(role_label, page_label, "close native VMO fd", err);
    };
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
    mapUserPageOrHalt(state, principal, va_start, page, writable, role_label, page_label, free_list);
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
    if (!user_vm.mapUserLinearRegion(principal, va_start, page.paddr, 4096, writable)) {
        halt.haltWithRolePageMessage(role_label, page_label, "map failed");
    }
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
    if (!user_vm.mapUserLinearRegion(to_principal, va_start, page.paddr, 4096, writable)) {
        halt.haltWithRolePageMessage(role_label, page_label, "map failed");
    }
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
