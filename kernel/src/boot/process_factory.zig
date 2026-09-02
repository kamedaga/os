/// Process creation helpers.
/// All functions are stateless — callers pass KernelState, FreePageList, and
/// user_spaces explicitly so this module has no global dependencies.
const kernel = @import("../kernel.zig");
const boot_static = @import("main_static.zig");
const elf_loader = @import("../elf_loader.zig");
const user_vm = @import("../memory/user_vm.zig");
const scheduler = @import("../scheduler.zig").connection;
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

const at_null: u64 = 0;
const at_phdr: u64 = 3;
const at_phent: u64 = 4;
const at_phnum: u64 = 5;
const at_pagesz: u64 = 6;
const at_entry: u64 = 9;
const at_uid: u64 = 11;
const at_euid: u64 = 12;
const at_gid: u64 = 13;
const at_egid: u64 = 14;
const at_secure: u64 = 23;
const at_random: u64 = 25;
const at_execfn: u64 = 31;

const user_stack_bytes: usize = 4096;

fn stackVaFromOffset(offset: usize) u64 {
    return boot_static.user_stack_page_va + @as(u64, @intCast(offset));
}

fn pushStackBytes(stack: []u8, cursor: *usize, bytes: []const u8) u64 {
    if (bytes.len > cursor.*) halt.haltWithMessage("initial user stack overflow");
    cursor.* -= bytes.len;
    @memcpy(stack[cursor.* .. cursor.* + bytes.len], bytes);
    return stackVaFromOffset(cursor.*);
}

pub fn installInitialUserStackOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    user_stack_paddr: u64,
    loaded: elf_loader.Image,
    argv0: []const u8,
) u64 {
    if ((user_stack_paddr & 0xFFF) != 0) halt.haltWithMessage("initial user stack paddr unaligned");
    if (argv0.len == 0 or argv0.len >= 128) halt.haltWithMessage("initial user argv0 invalid");

    const stack_ptr: [*]u8 = @ptrFromInt(user_stack_paddr);
    const stack = stack_ptr[0..user_stack_bytes];
    @memset(stack, 0);

    var cursor: usize = user_stack_bytes;
    var random_seed = [_]u8{0} ** 16;
    state.fillRandomBytes(principal, random_seed[0..]);
    const random_va = pushStackBytes(stack, &cursor, random_seed[0..]);
    cursor &= ~@as(usize, 15);

    var argv0_buf: [128]u8 = undefined;
    @memcpy(argv0_buf[0..argv0.len], argv0);
    argv0_buf[argv0.len] = 0;
    const argv0_va = pushStackBytes(stack, &cursor, argv0_buf[0 .. argv0.len + 1]);

    const phdr_va = loaded.programHeaderVirtualAddress(boot_static.user_elf_base_va) orelse
        halt.haltWithMessage("initial user ELF PHDR unavailable");

    const aux_word_count: usize = 13 * 2;
    const word_count: usize = 1 + 2 + 1 + aux_word_count;
    if (cursor < word_count * 8) halt.haltWithMessage("initial user stack words overflow");
    cursor = (cursor - word_count * 8) & ~@as(usize, 15);

    const words: [*]u64 = @ptrCast(@alignCast(stack[cursor..].ptr));
    var wi: usize = 0;
    words[wi] = 1;
    wi += 1;
    words[wi] = argv0_va;
    wi += 1;
    words[wi] = 0;
    wi += 1;
    words[wi] = 0;
    wi += 1;

    const auxv = [_]u64{
        at_pagesz, 4096,
        at_phdr,   phdr_va,
        at_phent,  loaded.phentsize,
        at_phnum,  loaded.phnum,
        at_entry,  loaded.entry,
        at_uid,    0,
        at_euid,   0,
        at_gid,    0,
        at_egid,   0,
        at_secure, 0,
        at_random, random_va,
        at_execfn, argv0_va,
        at_null,   0,
    };
    for (auxv) |value| {
        words[wi] = value;
        wi += 1;
    }

    return stackVaFromOffset(cursor);
}

fn releaseStaleThreadSlot(principal: kernel.PrincipalId) void {
    if (scheduler.threadForPrincipal(principal)) |thread_slot| {
        _ = scheduler.releaseThread(thread_slot);
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

fn mapAdditionalInitialStackPagesOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    role_label: []const u8,
    free_list: *kernel.FreePageList,
) void {
    var page_index: usize = 1;
    while (page_index < boot_static.initial_user_stack_pages) : (page_index += 1) {
        const page = state.allocLowPhysicalPage(free_list) catch |err| {
            log_util.logIndexedError("allocLowPhysicalPage for initial stack failed idx=", page_index, err);
            halt.haltWithRolePageError(role_label, "user stack", "alloc lower stack page", err);
        };
        const va_start = boot_static.user_stack_page_va - (@as(u64, @intCast(page_index)) * 4096);
        mapUserPageOrHalt(
            state,
            principal,
            va_start,
            page,
            true,
            role_label,
            "user stack",
            free_list,
        );
    }
}

// ---------------------------------------------------------------------------
// Process creation
// ---------------------------------------------------------------------------

pub fn tryCreateUserProcess(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    role_label: []const u8,
    free_list: *kernel.FreePageList,
    user_spaces: *boot_static.UserAddressSpaceTable,
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
    trackMappedNativePageOrHalt(state, principal, boot_static.user_va, user_page, false, true, role_label, "user map", free_list);
    trackMappedNativePageOrHalt(state, principal, boot_static.user_stack_page_va, user_stack_page, true, false, role_label, "user stack", free_list);
    mapAdditionalInitialStackPagesOrHalt(state, principal, role_label, free_list);
    const thread_slot = scheduler.allocateReadyThread(principal, user_spaces, buildInitialUserTrapFrame(), free_list) orelse {
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
    user_spaces: *boot_static.UserAddressSpaceTable,
) CreatedUserProcess {
    return tryCreateUserProcess(state, principal, role_label, free_list, user_spaces) catch halt.haltLoop();
}

pub fn tryCreateDynamicUserProcess(
    state: *kernel.KernelState,
    role_label: []const u8,
    free_list: *kernel.FreePageList,
    user_spaces: *boot_static.UserAddressSpaceTable,
) CreateDynamicUserProcessError!DynamicUserProcess {
    const principal = state.createProcessDescriptorWithUserAddressSpaceChecked(
        role_label,
        free_list,
        user_spaces,
        scheduler.principalSlotReusable,
    ) orelse return error.NoFreeProcess;
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
    user_spaces: *boot_static.UserAddressSpaceTable,
) CreateDynamicUserProcessError!SuspendedUserProcess {
    const principal = state.createProcessDescriptorWithUserAddressSpaceChecked(
        role_label,
        free_list,
        user_spaces,
        scheduler.principalSlotReusable,
    ) orelse return error.NoFreeProcess;
    releaseStaleThreadSlot(principal);
    if (!user_vm.buildEmptyUserAddressSpace(principal)) {
        _ = state.removeProcessDescriptor(principal);
        return error.CreateFailed;
    }
    const thread_slot = scheduler.allocateSuspendedThread(principal, user_spaces, buildInitialUserTrapFrame(), free_list) orelse {
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
            // Executable bootstrap pages are populated by the kernel before
            // entry and may later be finalized as data by the ELF loader.
            .map_write = writable or executable,
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
