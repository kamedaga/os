const lock_diagnostics = @import("../sync/lock_diagnostics.zig");
const std = @import("std");
const builtin = @import("builtin");
const addresses = @import("addresses.zig");
const pt_storage = @import("pt_storage.zig");
const address_space = @import("address_space.zig");
const kernel = @import("../kernel.zig");
const scheduler = @import("../scheduler.zig").connection;
const perf = @import("../smp_perf.zig");

pub const UserAddressSpace = address_space.UserAddressSpace;
pub const UserAddressSpaceTable = address_space.UserAddressSpaceTable;

pub const Hooks = struct {
    pt_allocator: ?pt_storage.Allocator = null,
    retire_user_tlb_for_principal: ?*const fn (kernel.PrincipalId) void = null,
    user_spaces: *UserAddressSpaceTable,
    four_gib: u64,
    physical_map_limit: u64,
    user_low_va: u64,
    user_top_va: u64,
    dynamic_map_base_va: u64,
    dynamic_map_end_va: u64,
    user_va: u64,
    user_stack_page_va: u64,
    canonical_user_limit_exclusive: u64,
    page_entries: usize,
    page_addr_mask: u64,
    page_present: u64,
    page_rw: u64,
    page_user: u64,
    page_ps: u64,
    page_nx: u64,
    flush_user_tlb_for_principal_va: *const fn (principal: kernel.PrincipalId, va: u64) void,
    flush_user_tlb_for_principal_range: *const fn (principal: kernel.PrincipalId, va: u64, size_bytes: usize) void,
    kernel_pointer_paddr: *const fn (addresses.KernelVirtualAddress) ?addresses.PhysicalAddress,
    // Page-table borrowing is distinct from virtual-to-physical translation.
    // Callers retain the table lifetime; this hook checks mapping/alignment.
    seed_user_pml4_with_kernel: *const fn ([]u64) void,
};

var hooks: ?Hooks = null;

// PTE/CR3 encoding is the raw-integer boundary, not the pointer conversion
// boundary. Only an actual kernel page-table pointer enters this translation.
fn tablePaddr(h: anytype, pointer: anytype) ?u64 {
    if (@typeInfo(@TypeOf(pointer)).pointer.child != [512]u64)
        @compileError("page-table translation requires a kernel table pointer");
    const physical = h.kernel_pointer_paddr(addresses.KernelVirtualAddress.fromPointer(pointer)) orelse return null;
    return physical.toInt();
}
// Published direct-map tables need enduring ownership records, not just a
// retained-free-page list, so future writers can reject unknown PDE pointers.
// The outer state/all-VM locks serialize this store with policy publication.
var kernel_ram_split_store: pt_storage.Store = .{};

const AddressSpaceLockState = address_space.AddressSpaceLockState;

fn interruptsEnabled() bool {
    var flags: u64 = 0;
    asm volatile (
        \\pushfq
        \\pop %[flags]
        : [flags] "=r" (flags),
    );
    return (flags & (1 << 9)) != 0;
}

fn waitWithInterruptWindow() void {
    if (builtin.is_test) {
        std.atomic.spinLoopHint();
        return;
    }
    asm volatile ("sti; pause; cli" ::: .{ .memory = true });
}

fn lockState(state: *AddressSpaceLockState, key: lock_diagnostics.order.Key) void {
    var diagnostic_if = lock_diagnostics.maskInterrupts();
    const recursive = lock_diagnostics.recursive(&state.diagnostic_record);
    const cpu = scheduler.currentCpu();
    if (@atomicLoad(u8, &state.value, .acquire) != 0 and state.owner_cpu == cpu) {
        if (comptime lock_diagnostics.enabled) {
            if (!recursive or state.depth == 0) lock_diagnostics.failure(error.InvalidRecursion);
            if (state.depth == std.math.maxInt(u32)) lock_diagnostics.failure(error.DepthOverflow);
        }
        state.depth +%= 1;
        lock_diagnostics.restoreInterrupts(diagnostic_if);
        return;
    }
    if (comptime lock_diagnostics.enabled) {
        if (recursive) lock_diagnostics.failure(error.InvalidRecursion);
    }
    while (true) {
        lock_diagnostics.beforeAcquire(&state.diagnostic_record, key);
        if (@cmpxchgWeak(u8, &state.value, 0, 1, .acquire, .monotonic) == null) {
            state.owner_cpu = cpu;
            state.depth = 1;
            lock_diagnostics.acquired(&state.diagnostic_record, key);
            lock_diagnostics.restoreInterrupts(diagnostic_if);
            return;
        }
        lock_diagnostics.restoreInterrupts(diagnostic_if);
        while (@atomicLoad(u8, &state.value, .monotonic) != 0) {
            const restore_interrupts = interruptsEnabled();
            waitWithInterruptWindow();
            if (!builtin.is_test and restore_interrupts) asm volatile ("sti" ::: .{ .memory = true });
        }
        diagnostic_if = lock_diagnostics.maskInterrupts();
    }
}

fn unlockState(state: *AddressSpaceLockState) void {
    // Preserve the off-build IR shape as well as the recursive semantics.
    if (comptime !lock_diagnostics.enabled) {
        if (state.depth > 1) {
            state.depth -= 1;
            return;
        }
        state.owner_cpu = std.math.maxInt(usize);
        state.depth = 0;
        @atomicStore(u8, &state.value, 0, .release);
        return;
    }

    const diagnostic_if = lock_diagnostics.maskInterrupts();
    defer lock_diagnostics.restoreInterrupts(diagnostic_if);
    lock_diagnostics.assertOwned(&state.diagnostic_record);
    if (comptime lock_diagnostics.enabled) {
        if (state.depth == 0) lock_diagnostics.failure(error.DepthUnderflow);
        if (state.owner_cpu != scheduler.currentCpu()) lock_diagnostics.failure(error.WrongContext);
    }
    if (state.depth > 1) {
        state.depth -= 1;
        return;
    }
    lock_diagnostics.releasing(&state.diagnostic_record);
    state.owner_cpu = std.math.maxInt(usize);
    state.depth = 0;
    @atomicStore(u8, &state.value, 0, .release);
}

// VMA tables are address-space local, but NativeVmo and NativeCowTable
// objects may be shared by forked address spaces.  This lock protects only
// that shared object metadata; page-table and reservation operations use the
// lock embedded in the target UserAddressSpace.
var shared_vm_object_lock: AddressSpaceLockState = .{};

pub fn lockAddressSpace(principal: kernel.PrincipalId) bool {
    const h = hooks orelse return false;
    const space = h.user_spaces.borrow(processIndex(principal) orelse return false) orelse return false;
    lockState(&space.lock_state, .{ .class = .address_space, .order = if (lock_diagnostics.enabled) processIndex(principal).? else 0 });
    // A retirement may have won while this borrower waited for A. It must
    // finish its loan without installing mappings into the withdrawn tree.
    if (space.retiring) {
        unlockState(&space.lock_state);
        h.user_spaces.endBorrow(space);
        return false;
    }
    return true;
}

pub fn unlockAddressSpace(principal: kernel.PrincipalId) void {
    const space = getUserSpace(principal) orelse return;
    unlockState(&space.lock_state);
    hooks.?.user_spaces.endBorrow(space);
}

pub fn lockAddressSpacePair(first: kernel.PrincipalId, second: kernel.PrincipalId) bool {
    const first_index = processIndex(first) orelse return false;
    const second_index = processIndex(second) orelse return false;
    if (first_index == second_index) return lockAddressSpace(first);
    const low = if (first_index < second_index) first else second;
    const high = if (first_index < second_index) second else first;
    if (!lockAddressSpace(low)) return false;
    if (!lockAddressSpace(high)) {
        unlockAddressSpace(low);
        return false;
    }
    return true;
}

pub fn unlockAddressSpacePair(first: kernel.PrincipalId, second: kernel.PrincipalId) void {
    const first_index = processIndex(first) orelse return;
    const second_index = processIndex(second) orelse return;
    if (first_index == second_index) {
        unlockAddressSpace(first);
        return;
    }
    const low = if (first_index < second_index) first else second;
    const high = if (first_index < second_index) second else first;
    unlockAddressSpace(high);
    unlockAddressSpace(low);
}

pub fn lockAllAddressSpaces() void {
    const h = hooks orelse return;
    const capacity = h.user_spaces.capacity();
    var index: usize = 0;
    while (index < capacity) : (index += 1) {
        const space = h.user_spaces.borrow(index) orelse continue;
        lockState(&space.lock_state, .{ .class = .address_space, .order = index });
    }
}

pub fn unlockAllAddressSpaces() void {
    const h = hooks orelse return;
    var index = h.user_spaces.capacity();
    while (index != 0) {
        index -= 1;
        const space = h.user_spaces.get(index) orelse continue;
        if (space.retiring) continue;
        unlockState(&space.lock_state);
        h.user_spaces.endBorrow(space);
    }
}

pub fn lockSharedVmObjects() void {
    lockState(&shared_vm_object_lock, .{ .class = .shared_vm });
}

pub fn unlockSharedVmObjects() void {
    unlockState(&shared_vm_object_lock);
}

pub fn lockVmTransaction(principal: kernel.PrincipalId) bool {
    const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.vm_wait, stage_sample);
    if (!lockAddressSpace(principal)) return false;
    lockSharedVmObjects();
    return true;
}

pub fn unlockVmTransaction(principal: kernel.PrincipalId) void {
    unlockSharedVmObjects();
    unlockAddressSpace(principal);
}

pub fn lockVmTransactionPair(first: kernel.PrincipalId, second: kernel.PrincipalId) bool {
    const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.vm_wait, stage_sample);
    if (!lockAddressSpacePair(first, second)) return false;
    lockSharedVmObjects();
    return true;
}

pub fn unlockVmTransactionPair(first: kernel.PrincipalId, second: kernel.PrincipalId) void {
    unlockSharedVmObjects();
    unlockAddressSpacePair(first, second);
}

pub fn lockAllVmTransactions() void {
    lockAllAddressSpaces();
    lockSharedVmObjects();
}

pub fn unlockAllVmTransactions() void {
    unlockSharedVmObjects();
    unlockAllAddressSpaces();
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

pub fn kernelStaticStorageEndAddr() usize {
    var end = staticStorageEnd(@TypeOf(hooks), &hooks);
    const lock_end = staticStorageEnd(@TypeOf(shared_vm_object_lock), &shared_vm_object_lock);
    if (lock_end > end) end = lock_end;
    const split_end = staticStorageEnd(@TypeOf(kernel_ram_split_store), &kernel_ram_split_store);
    if (split_end > end) end = split_end;
    return end;
}

pub fn init(new_hooks: Hooks) void {
    // Boot installs this configuration before AP/runtime readers start. A
    // const borrow of it is safe only while no writer can replace the value.
    // Host fixtures may reinstall between serial tests, never during a borrow.
    configureHooks(new_hooks, builtin.is_test) catch @panic("VM Hooks already initialized");
}

fn configureHooks(new_hooks: Hooks, comptime allow_fixture_replacement: bool) error{AlreadyInitialized}!void {
    if (!allow_fixture_replacement and hooks != null) return error.AlreadyInitialized;
    hooks = new_hooks;
}

pub fn getUserSpace(principal: kernel.PrincipalId) ?*UserAddressSpace {
    const h = hooks orelse return null;
    const idx = processIndex(principal) orelse return null;
    return h.user_spaces.get(idx);
}

pub const IpcPinTestSupport = if (builtin.is_test) struct {
    pub var table: ?*UserAddressSpaceTable = null;
} else struct {};

fn ipcPinTable() ?*UserAddressSpaceTable {
    if (comptime builtin.is_test) return IpcPinTestSupport.table;
    return if (hooks) |h| h.user_spaces else null;
}

// Lifecycle holds AD and closes admission before checking CPU quiescence.
// Dedicated cross-AS handoffs retain these pins through commit, so CR3
// storage cannot be recycled while an admitted switch still references it.
pub fn ipcAddressSpacePinsEnded(principal: kernel.PrincipalId) bool {
    const table = ipcPinTable() orelse return true;
    const index = processIndex(principal) orelse return true;
    return table.fastPinsEnded(index);
}

pub fn clearUserAddressSpace(principal: kernel.PrincipalId) void {
    if (!lockAddressSpace(principal)) return;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return;
    const space = getUserSpace(principal) orelse return;

    detachAndReleaseUserPtStorage(h, space, principal, null);
    resetUserAddressSpaceStorage(space);
}

/// Final lifecycle only (not exec reset): K and A remain held and the old
/// tree has already completed its existing all-CPU shootdown in clear.
pub fn retireUserAddressSpace(principal: kernel.PrincipalId) void {
    const h = hooks orelse return;
    const index = processIndex(principal) orelse return;
    const space = h.user_spaces.get(index) orelse return;
    std.debug.assert(space.cr3 == 0);
    h.user_spaces.retire(index);
}

fn reservationStorageReturned(table: *UserAddressSpace.ReservationTable) bool {
    table.deinit();
    inline for (.{ &table.directory_owner, &table.metadata_owner, &table.body_owner }) |owner| {
        if (owner.pending != null or owner.retired != null or owner.published != null) return false;
    }
    return true;
}

/// K only, after handler AS/shared-VM defers. No wait: current exits and
/// refused returns remain retired for a subsequent syscall after switching.
pub fn retryRetiredAddressSpaces() void {
    const h = hooks orelse return;
    if (h.user_spaces.retired_head == null and h.user_spaces.body_storage.retired == null) return;
    var retired = h.user_spaces.retired_head;
    while (retired) |space| {
        retired = space.retired_next;
        const principal: kernel.PrincipalId = @enumFromInt(space.directory_index);
        if (!scheduler.addressSpaceUsersEnded(principal) or !h.user_spaces.loansEnded(space)) continue;
        // Publication is closed and all A loans (including waiters and
        // recursion) ended. clear already detached and ACKed this CR3 once;
        // retries only return storage, never issue another shootdown.
        space.pt_store.freeDetached();
        space.pd_store.freeDetached();
        space.pdp_store.freeDetached();
        space.scratch_store.freeDetached();
        const reservations_done = reservationStorageReturned(&space.reservations);
        const draft_done = reservationStorageReturned(&space.reservation_draft);
        if (!reservations_done or !draft_done or space.pt_store.chunks != null or
            space.pd_store.chunks != null or space.pdp_store.chunks != null or space.scratch_store.chunks != null) continue;
        h.user_spaces.reclaimRetiredBody(space);
    }
    h.user_spaces.body_storage.reclaimRetiredAfterBorrowExclusion() catch unreachable;
}

fn detachAndReleaseUserPtStorage(h: Hooks, space: *UserAddressSpace, principal: kernel.PrincipalId, keep_pdp: ?usize) void {
    // A process slot reuses both its page-table storage and its PCID.  Make
    // the old user tree unreachable before flushing that PCID on every CPU;
    // otherwise a later occupant can execute a cached translation after the
    // old VMO page has already been returned to the physical free list.
    // clearUserAddressSpace resets cr3 only AFTER this synchronous retirement.
    // Exec clears the staged AS before moving its VMAs; later cleanup sees zero
    // and must not issue a second shootdown for the already retired context.
    if (space.cr3 != 0) {
        // Keep the kernel half present: the shootdown implementation briefly
        // loads this CR3 while handling an IPI in kernel mode.
        @memset(space.pml4[0..256], 0);
        if (h.retire_user_tlb_for_principal) |retire| retire(principal) else h.flush_user_tlb_for_principal_range(principal, h.user_low_va, @intCast(h.user_top_va - h.user_low_va));
    }
    space.pt_store.freeDetached();
    space.pd_store.freeDetached();
    var it = space.pdp_store.iterator();
    while (it.next()) |slot| {
        if (keep_pdp != slot) space.pdp_store.releasePageDetached(slot);
    }
    space.pdp_store.reclaimEmptyBanks();
    space.scratch_store.freeDetached();
}

pub fn currentUserSpace() *UserAddressSpace {
    return getUserSpace(scheduler.currentPrincipal()).?;
}

pub fn isUserCanonicalVa(va: u64) bool {
    const h = hooks orelse return false;
    return va < h.canonical_user_limit_exclusive;
}

const UserPageIndex = struct {
    pml4: usize,
    pdp: usize,
    pd: usize,
    pt: usize,
};

fn userPageIndexForVa(h: anytype, va: u64) ?UserPageIndex {
    return userPageIndexForVaWithLowPageZero(h, va, false);
}

fn userPageIndexForVaWithLowPageZero(h: anytype, va: u64, allow_low_page_zero: bool) ?UserPageIndex {
    const pml4_index: usize = @intCast((va >> 39) & 0x1FF);
    if (va < h.user_low_va and !(allow_low_page_zero and va < 4096)) return null;
    if (va >= h.user_top_va) return null;
    if (pml4_index >= 256) return null;
    return .{
        .pml4 = pml4_index,
        .pdp = @intCast((va >> 30) & 0x1FF),
        .pd = @intCast((va >> 21) & 0x1FF),
        .pt = @intCast((va >> 12) & 0x1FF),
    };
}

pub fn lookupUserMappedPaddrForVa(principal: kernel.PrincipalId, va: u64) ?u64 {
    if (!lockAddressSpace(principal)) return null;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return null;
    const space = getUserSpace(principal) orelse return null;
    const index = userPageIndexForVa(h, va) orelse return null;
    const slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return null;
    const entry = space.ptPage(slot).*[index.pt];
    const is_user_mapping = (entry & h.page_present) != 0 and (entry & h.page_user) != 0;
    const paddr = entry & h.page_addr_mask;
    if (!is_user_mapping or paddr == 0) return null;
    return paddr;
}

/// The caller must hold the principal's address-space lock.  This is used by
/// the page-fault path to distinguish a real permission violation from a
/// stale CPU translation after a process/PCID slot has been recycled.
pub fn userMappingAllowsAccessWithAddressSpaceLocked(
    principal: kernel.PrincipalId,
    va: u64,
    write_access: bool,
    instruction_fetch: bool,
) bool {
    return lookupUserMappedPaddrForAccessWithAddressSpaceLocked(
        principal,
        va,
        write_access,
        instruction_fetch,
    ) != null;
}

pub fn lookupUserMappedPaddrForAccessWithAddressSpaceLocked(
    principal: kernel.PrincipalId,
    va: u64,
    write_access: bool,
    instruction_fetch: bool,
) ?u64 {
    const physical = lookupUserPhysicalPageWithAddressSpaceLocked(
        principal,
        .fromInt(va),
        write_access,
        instruction_fetch,
    ) orelse return null;
    return physical.toInt();
}

/// A page-base borrow under the caller's AS lock, not an owned page or a
/// durable permission token. Copy callers separately add the VA's offset and
/// keep exclusion through COW resolution, cache selection, and actual access.
pub fn lookupUserPhysicalPageWithAddressSpaceLocked(
    principal: kernel.PrincipalId,
    address: addresses.UserVirtualAddress,
    write_access: bool,
    instruction_fetch: bool,
) ?addresses.PhysicalAddress {
    const profile = @import("../lock_profile.zig");
    const profile_start = profile.copyTimestamp();
    defer profile.copyEnd(.page_walk, profile_start);
    // Configuration is boot-owned and immutable for runtime readers. Borrow
    // it instead of copying every callback/constant for each page lookup.
    const h: *const Hooks = if (hooks) |*configured| configured else return null;
    const space = getUserSpace(principal) orelse return null;
    const index = userPageIndexForVa(h, address.toInt()) orelse return null;
    const slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return null;
    const entry = space.ptPage(slot).*[index.pt];
    if ((entry & h.page_present) == 0 or (entry & h.page_user) == 0) return null;
    const paddr = entry & h.page_addr_mask;
    if (paddr == 0) return null;
    if (write_access and (entry & h.page_rw) == 0) return null;
    if (instruction_fetch and (entry & h.page_nx) != 0) return null;
    return .fromInt(paddr);
}

pub fn resetUserReservations(space: *UserAddressSpace) void {
    space.reservations.deinit();
    space.reservation_generation = 0;
    space.next_dynamic_map_page = 0;
}

pub fn initializeUserAddressSpaceStorage(space: *UserAddressSpace) void {
    address_space.initializeUserAddressSpaceStorage(space);
}

pub fn resetUserAddressSpaceStorage(space: *UserAddressSpace) void {
    address_space.resetUserAddressSpaceStorage(space);
}

fn resetUserPageTablesPreserveReservations(space: *UserAddressSpace, principal: kernel.PrincipalId) error{ OutOfFreePages, InvalidState }!void {
    const h = hooks orelse return error.InvalidState;
    const user_pml4_pa = tablePaddr(h, &space.pml4) orelse return error.InvalidState;
    if (user_pml4_pa >= h.physical_map_limit) return error.InvalidState;
    // Exec must have its new empty PDP and validated addresses BEFORE removing
    // the old tree. Keep this unpublished grant out of the old-tree teardown.
    const slot = space.pdp_store.firstFreeSlot() orelse return error.OutOfFreePages;
    const page = try space.pdp_store.ensurePageChecked(slot, h.pt_allocator orelse return error.InvalidState);
    var installed = false;
    defer if (!installed) {
        space.pdp_store.releasePageDetached(slot);
        space.pdp_store.reclaimEmptyBanks();
    };
    const physical = tablePaddr(h, page) orelse return error.InvalidState;
    if (physical >= h.physical_map_limit) return error.InvalidState;
    detachAndReleaseUserPtStorage(h, space, principal, slot);
    @memset(&space.pml4, 0);
    h.seed_user_pml4_with_kernel(&space.pml4);
    space.pdp_store.resetDetachedContents();
    space.pd_store.resetDetachedContents();
    space.pt_store.resetDetachedContents();
    space.pdpPml4Index(slot).* = 0;
    space.pdp_page_used_len = slot + 1;
    space.pd_page_used_len = 0;
    space.pt_page_used_len = 0;
    space.pml4[0] = physical | h.page_present | h.page_rw | h.page_user;
    space.cr3 = user_pml4_pa;
    installed = true;
}

pub fn cloneAddressSpaceMetadataForFork(from: kernel.PrincipalId, to: kernel.PrincipalId) error{ OutOfFreePages, InvalidState }!void {
    const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.pt_prepare, stage_sample);
    if (!lockAddressSpacePair(from, to)) return error.InvalidState;
    defer unlockAddressSpacePair(from, to);
    const source = getUserSpace(from) orelse return error.InvalidState;
    const dest = getUserSpace(to) orelse return error.InvalidState;
    // Build the complete unpublished copy before touching destination PTEs.
    // A persistent draft owns even rejected rollback returns across retries.
    dest.reservation_draft.deinit();
    defer dest.reservation_draft.deinit();
    const allocator = (hooks orelse return error.InvalidState).pt_allocator orelse return error.InvalidState;
    var records = source.reservations.iterator();
    while (records.next()) |record| {
        const copy = dest.reservation_draft.allocate(allocator, null, std.math.maxInt(u32)) catch |err| return switch (err) {
            error.OutOfMemory, error.IndexExhausted => error.OutOfFreePages,
            else => error.InvalidState,
        };
        copy.value.* = record.value.*;
    }
    try resetUserPageTablesPreserveReservations(dest, to);
    dest.reservations.deinit();
    dest.reservations.takeFrom(&dest.reservation_draft);
    dest.reservation_generation = source.reservation_generation;
    dest.next_dynamic_map_page = source.next_dynamic_map_page;
}

pub fn presentUserPagePaddr(principal: kernel.PrincipalId, va: u64) ?u64 {
    if (!lockAddressSpace(principal)) return null;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return null;
    const space = getUserSpace(principal) orelse return null;
    if ((va & 0xFFF) != 0) return null;
    const index = userPageIndexForVa(h, va) orelse return null;
    const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return null;
    const pt_page: *const [512]u64 = space.ptPage(pt_slot);
    const entry = pt_page[index.pt];
    if ((entry & h.page_present) == 0 or (entry & h.page_user) == 0) return null;
    return ptePaddr(entry);
}

pub fn writeProtectPresentUserPagesForForkCow(principal: kernel.PrincipalId, va_start: u64, size_bytes: u64) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if ((va_start & 0xFFF) != 0 or (size_bytes & 0xFFF) != 0) return false;
    if (size_bytes == 0) return true;
    _ = userRangeEndVa(h, va_start, size_bytes) orelse return false;

    var changed = false;
    var offset: u64 = 0;
    while (offset < size_bytes) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        // All entries until the next 2-MiB boundary share one PT slot.  The
        // former per-page lookup rescanned as many as 512 PT descriptors for
        // every page of large brk arenas during fork.
        const remaining_pages: usize = @intCast((size_bytes - offset) / 4096);
        const segment_pages = @min(remaining_pages, h.page_entries - index.pt);
        if (findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd)) |pt_slot| {
            const pt_page: *[512]u64 = space.ptPage(pt_slot);
            for (pt_page[index.pt .. index.pt + segment_pages]) |*entry| {
                const old_entry = entry.*;
                if ((old_entry & h.page_present) == 0 or (old_entry & h.page_user) == 0) continue;
                if ((old_entry & h.page_rw) == 0) continue;
                entry.* = old_entry & ~h.page_rw;
                changed = true;
            }
        }
        offset += @as(u64, @intCast(segment_pages)) * 4096;
    }
    if (changed) {
        h.flush_user_tlb_for_principal_range(principal, va_start, @intCast(size_bytes));
    }
    return true;
}

fn reservationEndPage(base_va: u64, page_count: u64) ?u64 {
    if (page_count == 0) return null;
    if ((base_va & 0xFFF) != 0) return null;
    const base_page = base_va >> 12;
    const end_page, const overflow = @addWithOverflow(base_page, page_count);
    if (overflow != 0 or end_page > (std.math.maxInt(u64) >> 12)) return null;
    return end_page;
}

fn reservationOverlaps(base_a: u64, count_a: u64, base_b: u64, count_b: u64) bool {
    const end_a = reservationEndPage(base_a, count_a) orelse return true;
    const end_b = reservationEndPage(base_b, count_b) orelse return true;
    const start_a = base_a >> 12;
    const start_b = base_b >> 12;
    return start_a < end_b and start_b < end_a;
}

fn reservationStartPage(reservation: *const UserAddressSpace.Reservation) u64 {
    return reservation.base_va >> 12;
}

fn releaseReservation(space: *UserAddressSpace, record: *UserAddressSpace.Reservation) void {
    const index = UserAddressSpace.ReservationTable.indexOf(record);
    space.reservations.release(.{ .index = index, .generation = space.reservations.generation(index).? }) catch unreachable;
}

fn prepareReservationSlots(space: *UserAddressSpace, count: usize) bool {
    const allocator = (hooks orelse return false).pt_allocator orelse return count == 0;
    // Bodies stay prepared until reset: a caller can reserve several slots
    // before a compound unmap/move commit without allocation during commit.
    while (space.reservations.preparedFreeCount(count) < count) {
        const index = std.math.cast(u32, space.reservations.capacity) orelse return false;
        space.reservations.prepareIndex(index, allocator) catch return false;
    }
    return true;
}

pub fn prepareUserReservationSlots(principal: kernel.PrincipalId, count: usize) bool {
    const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.prepare, stage_sample);
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    return prepareReservationSlots(getUserSpace(principal) orelse return false, count);
}

fn tryMergeUserReservation(space: *UserAddressSpace, base_va: u64, page_count: u64, kind: UserAddressSpace.ReservationKind, writable: bool) bool {
    const start = base_va >> 12;
    const end = reservationEndPage(base_va, page_count) orelse return false;
    var records = space.reservations.iterator();
    while (records.next()) |record| {
        const target = record.value;
        if (target.kind != kind or target.writable != writable) continue;
        const target_end = reservationEndPage(target.base_va, target.page_count) orelse unreachable;
        if (target_end != start and end != reservationStartPage(target)) continue;
        var merged_start = @min(start, reservationStartPage(target));
        var merged_end = @max(end, target_end);
        // All records have checked address-representable endpoints. Union of
        // adjacent records needs no allocation and cannot overflow its count.
        while (true) {
            var absorbed = false;
            var candidates = space.reservations.iterator();
            while (candidates.next()) |candidate| {
                const value = candidate.value;
                if (value == target or value.kind != kind or value.writable != writable) continue;
                const candidate_start = reservationStartPage(value);
                const candidate_end = reservationEndPage(value.base_va, value.page_count) orelse unreachable;
                if (candidate_end != merged_start and merged_end != candidate_start) continue;
                merged_start = @min(merged_start, candidate_start);
                merged_end = @max(merged_end, candidate_end);
                releaseReservation(space, value);
                absorbed = true;
                break;
            }
            if (!absorbed) break;
        }
        target.base_va = merged_start << 12;
        target.page_count = merged_end - merged_start;
        return true;
    }
    return false;
}

pub fn reserveUserMapping(principal: kernel.PrincipalId, base_va: u64, page_count: u64, kind: UserAddressSpace.ReservationKind, writable: bool) bool {
    if (kind == .none) return false;
    const space = getUserSpace(principal) orelse return false;
    _ = reservationEndPage(base_va, page_count) orelse return false;
    var records = space.reservations.iterator();
    while (records.next()) |record| {
        if (reservationOverlaps(base_va, page_count, record.value.base_va, record.value.page_count)) return false;
    }
    if (tryMergeUserReservation(space, base_va, page_count, kind, writable)) return true;
    if (!prepareReservationSlots(space, 1)) return false;
    const record = space.reservations.allocatePrepared() catch return false;
    space.reservation_generation +%= 1;
    record.value.* = .{ .base_va = base_va, .page_count = page_count, .generation = space.reservation_generation, .kind = kind, .writable = writable, .active = true };
    return true;
}

pub fn releaseUserMapping(principal: kernel.PrincipalId, base_va: u64, page_count: u64) bool {
    const space = getUserSpace(principal) orelse return false;
    const end = reservationEndPage(base_va, page_count) orelse return false;
    const start = base_va >> 12;
    if (!prepareReservationSlots(space, reservationSplitSlotsRequired(space, base_va, page_count * 4096))) return false;
    var changed = false;
    var records = space.reservations.iterator();
    while (records.next()) |record| {
        const value = record.value;
        const old_start = reservationStartPage(value);
        const old_end = reservationEndPage(value.base_va, value.page_count) orelse unreachable;
        if (old_start >= end or start >= old_end) continue;
        if (start <= old_start and end >= old_end) {
            releaseReservation(space, value);
        } else if (start <= old_start) {
            value.base_va = end << 12;
            value.page_count = old_end - end;
        } else if (end >= old_end) {
            value.page_count = start - old_start;
        } else {
            const right = space.reservations.allocatePrepared() catch unreachable;
            right.value.* = value.*;
            right.value.base_va = end << 12;
            right.value.page_count = old_end - end;
            value.page_count = start - old_start;
        }
        changed = true;
    }
    return changed;
}

fn userDynamicMapRange(h: Hooks) ?struct { start_page: u64, end_page: u64 } {
    if (h.dynamic_map_base_va >= h.dynamic_map_end_va) return null;
    if (h.dynamic_map_base_va < h.user_low_va) return null;
    if (h.dynamic_map_end_va > h.user_top_va) return null;
    const start_page = (h.dynamic_map_base_va + 0xFFF) >> 12;
    const end_page = h.dynamic_map_end_va >> 12;
    if (start_page >= end_page) return null;
    return .{ .start_page = start_page, .end_page = end_page };
}

fn rangeOverlappingReservationEndPage(space: *const UserAddressSpace, base_va: u64, page_count: u64) ?u64 {
    var skip_to_page: u64 = 0;
    var records = space.reservations.iterator();
    while (records.next()) |record| {
        const reservation = record.value;
        if (!reservation.active) continue;
        if (!reservationOverlaps(base_va, page_count, reservation.base_va, reservation.page_count)) continue;
        const reservation_end_page = reservationStartPage(reservation) + reservation.page_count;
        if (reservation_end_page > skip_to_page) skip_to_page = reservation_end_page;
    }
    return if (skip_to_page == 0) null else skip_to_page;
}

fn rangeHasPresentUserMapping(principal: kernel.PrincipalId, base_va: u64, page_count: u64) bool {
    const h = hooks orelse return true;
    const space = getUserSpace(principal) orelse return true;
    const bytes = std.math.mul(u64, page_count, 4096) catch return true;
    _ = userRangeEndVa(h, base_va, bytes) orelse return true;
    const end = base_va + bytes;
    var pt_it = space.pt_store.iterator();
    while (pt_it.next()) |slot| {
        const span = ptSlotIntersection(space, slot, base_va, end) orelse continue;
        for (space.ptPage(slot).*[span.first..span.last]) |entry| {
            if ((entry & (h.page_present | h.page_user)) == (h.page_present | h.page_user)) return true;
        }
    }
    return false;
}

fn ptSlotIntersection(space: *const UserAddressSpace, slot: usize, start: u64, end: u64) ?struct { first: usize, last: usize } {
    const pml4 = space.ptPml4Index(slot).*;
    const pdp = space.ptPdpIndex(slot).*;
    const pd = space.ptPdIndex(slot).*;
    if (pml4 == UserAddressSpace.no_pd_index or pdp == UserAddressSpace.no_pd_index or
        pd == UserAddressSpace.no_pd_index) return null;
    const base = (@as(u64, pml4) << 39) | (@as(u64, pdp) << 30) | (@as(u64, pd) << 21);
    const lo = @max(start, base);
    const hi = @min(end, base + 2 * 1024 * 1024);
    if (lo >= hi) return null;
    return .{ .first = @intCast((lo - base) >> 12), .last = @intCast((hi - base) >> 12) };
}

pub fn userPageHasMappingOrReservation(principal: kernel.PrincipalId, va: u64) bool {
    const space = getUserSpace(principal) orelse return true;
    if ((va & 0xFFF) != 0) return true;
    if (rangeOverlappingReservationEndPage(space, va, 1) != null) return true;
    return rangeHasPresentUserMapping(principal, va, 1);
}

fn alignPageForward(page: u64, alignment: u64) u64 {
    const align_mask = alignment - 1;
    if ((alignment & align_mask) == 0) return (page + align_mask) & ~align_mask;
    const rem = page % alignment;
    return if (rem == 0) page else page + alignment - rem;
}

fn findFreeUserMappingRangeInSpan(
    principal: kernel.PrincipalId,
    space: *UserAddressSpace,
    page_count: u64,
    alignment: u64,
    start_page: u64,
    end_page: u64,
) ?u64 {
    var base_page = alignPageForward(start_page, alignment);
    while (base_page < end_page and page_count <= end_page - base_page) {
        const base_va = base_page << 12;
        if (rangeOverlappingReservationEndPage(space, base_va, page_count)) |skip_to_page| {
            const next_page = if (skip_to_page > base_page) skip_to_page else base_page + 1;
            base_page = alignPageForward(next_page, alignment);
            continue;
        }
        if (rangeHasPresentUserMapping(principal, base_va, page_count)) {
            base_page += alignment;
            continue;
        }
        space.next_dynamic_map_page = base_page + page_count;
        return base_va;
    }
    return null;
}

pub fn findFreeUserMappingRange(principal: kernel.PrincipalId, page_count: u64, align_pages: u64) ?u64 {
    const h = hooks orelse return null;
    if (page_count == 0) return null;
    const alignment = if (align_pages == 0) @as(u64, 1) else align_pages;
    const space = getUserSpace(principal) orelse return null;
    const range = userDynamicMapRange(h) orelse return null;
    const cursor_page = if (space.next_dynamic_map_page >= range.start_page and space.next_dynamic_map_page < range.end_page)
        space.next_dynamic_map_page
    else
        range.start_page;

    if (findFreeUserMappingRangeInSpan(principal, space, page_count, alignment, cursor_page, range.end_page)) |base_va| {
        return base_va;
    }
    if (cursor_page > range.start_page) {
        return findFreeUserMappingRangeInSpan(principal, space, page_count, alignment, range.start_page, cursor_page);
    }
    return null;
}

pub fn advanceFreeUserMappingSearch(principal: kernel.PrincipalId, next_va: u64) bool {
    const space = getUserSpace(principal) orelse return false;
    const next_page, const overflow = @addWithOverflow(next_va, @as(u64, 0xFFF));
    if (overflow != 0) return false;
    space.next_dynamic_map_page = next_page >> 12;
    return true;
}

fn findUserPtSlotForPd(space: *const UserAddressSpace, pml4_index: usize, pdp_index: usize, pd_index: usize) ?usize {
    if (pml4_index >= 256 or pdp_index >= 512 or pd_index >= 512) return null;
    var it = space.pt_store.iterator();
    while (it.next()) |slot| {
        const entry = it.current.?;
        if (entry.pml4 == pml4_index and entry.pdp == pdp_index and entry.pd == pd_index) return slot;
    }
    return null;
}

fn userPtSlotHasEntries(space: *const UserAddressSpace, slot: usize) bool {
    if (slot >= space.ptCapacity()) return true;
    return !std.mem.allEqual(u64, space.ptPage(slot), 0);
}

fn detachUserPtSlotIfEmpty(h: Hooks, space: *UserAddressSpace, slot: usize) bool {
    if (slot >= space.ptCapacity()) return false;
    if (space.ptPdIndex(slot).* == UserAddressSpace.no_pd_index) return false;
    if (userPtSlotHasEntries(space, slot)) return false;
    const pml4_index = space.ptPml4Index(slot).*;
    const pdp_index = space.ptPdpIndex(slot).*;
    const pd_index = space.ptPdIndex(slot).*;
    if (pml4_index == UserAddressSpace.no_pd_index or
        pdp_index == UserAddressSpace.no_pd_index or
        pd_index == UserAddressSpace.no_pd_index)
    {
        return false;
    }
    if (findUserPdSlotForPdp(space, pml4_index, pdp_index)) |pd_slot| {
        const pd_page: *[512]u64 = space.pdPage(pd_slot);
        const entry = pd_page[pd_index];
        const pt_pa: u64 = tablePaddr(h, space.ptPage(slot)) orelse return false;
        if ((entry & h.page_addr_mask) == pt_pa) {
            pd_page[pd_index] = 0;
            return true;
        }
    }
    return false;
}

// All detached descriptors remain assigned across the ACK; the marker is
// owned by the AS lock and is never an allocator free-slot indication.
fn retireEmptyUserPtSlots(h: Hooks, space: *UserAddressSpace, principal: kernel.PrincipalId, va_start: u64, size_bytes: usize, touched_slots: []usize, leaf_changed: bool, pre_detached: usize) void {
    const detach_start = perf.timestamp();
    var retired: usize = pre_detached;
    for (touched_slots) |slot| {
        if (detachUserPtSlotIfEmpty(h, space, slot)) {
            space.pt_store.descriptor(slot).?.retire_pending = true;
            retired += 1;
        }
    }
    // The range caller supplies its detached count, so a small leaf-only
    // mutation need not rescan every PT merely to discover no retirements.
    const pd_count = detachEmptyUserParents(h, space, .pd, va_start, size_bytes);
    const pdp_count = detachEmptyUserParents(h, space, .pdp, va_start, size_bytes);
    perf.ptRetireElapsed(.detach_cycles, detach_start);
    perf.ptRetireAdd(.retired_pts, retired);
    if (leaf_changed or retired != 0 or pd_count != 0 or pdp_count != 0) {
        // A removed parent can cover translations outside the changed leaf
        // range. Full-context invalidation also retires paging-structure caches.
        const tables = retired != 0 or pd_count != 0 or pdp_count != 0;
        perf.ptRetireAdd(.flush_calls, 1);
        const flush_start = perf.timestamp();
        h.flush_user_tlb_for_principal_range(principal, if (tables) h.user_low_va else va_start, if (tables) @intCast(h.user_top_va - h.user_low_va) else size_bytes);
        perf.ptRetireElapsed(.flush_cycles, flush_start);
    }
    const release_start = perf.timestamp();
    inline for (.{ TableLevel.pt, TableLevel.pd, TableLevel.pdp }) |level| {
        const count = switch (level) {
            .pt => retired,
            .pd => pd_count,
            .pdp => pdp_count,
        };
        const store = levelStore(space, level);
        var pending = store.iterator();
        var remaining = count;
        while (remaining != 0) {
            const slot = pending.next() orelse break;
            if (pending.current.?.retire_pending) {
                // A rejected allocator return retains an ACK-safe page for
                // reuse; the store clears its coordinates and pending marker.
                store.releasePageDetached(slot);
                remaining -= 1;
            }
        }
        std.debug.assert(remaining == 0);
    }
    perf.ptRetireElapsed(.release_cycles, release_start);
}

fn retireEmptyUserPtRange(h: Hooks, space: *UserAddressSpace, principal: kernel.PrincipalId, start: u64, bytes: usize, leaf_changed: bool) void {
    const range_start = perf.timestamp();
    perf.ptRetireAdd(.range_calls, 1);
    defer perf.ptRetireElapsed(.range_total_cycles, range_start);
    var candidates: usize = 0;
    var pt_it = space.pt_store.iterator();
    while (pt_it.next()) |slot| {
        _ = ptSlotIntersection(space, slot, start, start + bytes) orelse continue;
        if (detachUserPtSlotIfEmpty(h, space, slot)) {
            space.pt_store.descriptor(slot).?.retire_pending = true;
            candidates += 1;
        }
    }
    retireEmptyUserPtSlots(h, space, principal, start, bytes, &.{}, leaf_changed, candidates);
    reclaimTableMetadata(space);
    perf.ptRetireAdd(.range_candidate_pts, candidates);
}

/// Only the user-mode fault entry may call this, before starting COW or mapping
/// preparation. An empty PT inside an in-progress preflight is not reclaimable;
/// do not call from ensure/map helpers or a reentrant user-copy fault path.
pub fn reclaimEmptyUserPtSlotsForFaultWithAddressSpaceLocked(principal: kernel.PrincipalId, fault_va: u64) void {
    const h = hooks orelse return;
    const space = getUserSpace(principal) orelse return;
    if (space.lock_state.depth != 1 or
        space.lock_state.owner_cpu != scheduler.currentCpu() or
        space.pt_page_used_len < space.ptCapacity()) return;
    const index = userPageIndexForVaWithLowPageZero(h, fault_va, true) orelse return;
    if (findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) != null) return;
    if (space.firstFreePtSlot() != null) return;

    var candidates: [64]usize = undefined;
    var count: usize = 0;
    var first_va: u64 = std.math.maxInt(u64);
    var end_va: u64 = 0;
    var all_end: u64 = 0;
    var pt_it = space.pt_store.iterator();
    while (pt_it.next()) |slot| {
        if (space.ptPdIndex(slot).* == UserAddressSpace.no_pd_index) continue;
        const page = space.ptPage(slot);
        const pml4 = space.ptPml4Index(slot).*;
        const pdp = space.ptPdpIndex(slot).*;
        const pd = space.ptPdIndex(slot).*;
        if (pml4 >= 256 or pdp >= h.page_entries or pd >= h.page_entries) continue;
        // Every reclaim path requires an entirely empty owned table.
        var empty = true;
        for (page) |entry| {
            if (entry != 0) {
                empty = false;
                break;
            }
        }
        if (!empty) continue;
        const pd_slot = findUserPdSlotForPdp(space, pml4, pdp) orelse continue;
        const parent = space.pdPage(pd_slot).*[pd];
        if (parent & h.page_present == 0 or parent & h.page_ps != 0) continue;
        const pt_pa = tablePaddr(h, page) orelse continue;
        if (parent & h.page_addr_mask != pt_pa) continue;
        const va = (@as(u64, pml4) << 39) | (@as(u64, pdp) << 30) | (@as(u64, pd) << 21);
        all_end = @max(all_end, va + (1 << 21));
        first_va = @min(first_va, va);
        end_va = @max(end_va, va + (1 << 21));
        candidates[count] = @intCast(slot);
        count += 1;
        if (count == candidates.len) {
            retireEmptyUserPtSlots(h, space, principal, first_va, @intCast(end_va - first_va), candidates[0..count], false, 0);
            count = 0;
            first_va = std.math.maxInt(u64);
            end_va = 0;
        }
    }
    if (all_end == 0) return;
    // The synchronous barrier covers every detached PDE, not merely fault_va.
    // Metadata remains assigned until that barrier has completed.
    perf.ptRetireAdd(.fault_reclaim_calls, 1);
    if (count != 0) retireEmptyUserPtSlots(h, space, principal, first_va, @intCast(end_va - first_va), candidates[0..count], false, 0);
    reclaimTableMetadata(space);
}

fn findUserPdpSlotForPml4(space: *const UserAddressSpace, pml4_index: usize) ?usize {
    if (pml4_index >= 256) return null;
    var it = space.pdp_store.iterator();
    while (it.next()) |slot| {
        const entry = it.current.?;
        if (entry.pml4 == pml4_index) return slot;
    }
    return null;
}

fn findUserPdSlotForPdp(space: *const UserAddressSpace, pml4_index: usize, pdp_index: usize) ?usize {
    if (pml4_index >= 256 or pdp_index >= 512) return null;
    var it = space.pd_store.iterator();
    while (it.next()) |slot| {
        const entry = it.current.?;
        if (entry.pml4 == pml4_index and entry.pdp == pdp_index) return slot;
    }
    return null;
}

const TableLevel = enum { pdp, pd, pt };
const TableLink = struct { parent: *u64, previous: u64, slot: usize, level: TableLevel, physical: u64 };
fn levelStore(space: *UserAddressSpace, level: TableLevel) *pt_storage.Store {
    return switch (level) {
        .pdp => &space.pdp_store,
        .pd => &space.pd_store,
        .pt => &space.pt_store,
    };
}

// One hardware path has exactly three non-root levels. These are prepared
// completely before publication, so an OOM never leaves an empty new parent.
const TablePathPreparation = struct {
    links: [3]TableLink = undefined,
    count: usize = 0,
    committed: bool = false,
    index: UserPageIndex,

    fn allocate(self: *@This(), h: Hooks, space: *UserAddressSpace, level: TableLevel, parent: *u64) ?usize {
        // A present parent without an owned descriptor is corruption, not
        // an inherited kernel seed that may be copied or replaced.
        if (parent.* & h.page_present != 0) return null;
        const store = levelStore(space, level);
        const slot = store.firstFreeSlot() orelse return null;
        const page = store.ensurePage(slot, h.pt_allocator orelse return null) orelse return null;
        // Record ownership BEFORE any fallible address validation.
        self.links[self.count] = .{ .parent = parent, .previous = parent.*, .slot = slot, .level = level, .physical = 0 };
        self.count += 1;
        const physical = tablePaddr(h, page) orelse return null;
        if (physical >= h.physical_map_limit) return null;
        self.links[self.count - 1].physical = physical;
        return slot;
    }

    fn prepare(self: *@This(), h: Hooks, space: *UserAddressSpace, last: TableLevel) ?usize {
        const index = self.index;
        const pdp_slot = findUserPdpSlotForPml4(space, index.pml4) orelse blk: {
            const slot = self.allocate(h, space, .pdp, &space.pml4[index.pml4]) orelse return null;
            break :blk slot;
        };
        if (last == .pdp) return pdp_slot;
        const pdp = space.pdpPage(pdp_slot);
        const pd_slot = findUserPdSlotForPdp(space, index.pml4, index.pdp) orelse blk: {
            const slot = self.allocate(h, space, .pd, &pdp[index.pdp]) orelse return null;
            @memset(space.pdPage(slot), 0);
            break :blk slot;
        };
        if (last == .pd) return pd_slot;
        const pd = space.pdPage(pd_slot);
        const slot = self.allocate(h, space, .pt, &pd[index.pd]) orelse return null;
        @memset(space.ptPage(slot), 0);
        return slot;
    }

    fn publish(self: *@This(), h: Hooks, space: *UserAddressSpace) void {
        // Publish bottom-up: any newly reachable parent already has initialized
        // children. No allocation or validation remains after the first store.
        var remaining = self.count;
        while (remaining != 0) {
            remaining -= 1;
            const link = self.links[remaining];
            const entry = levelStore(space, link.level).descriptor(link.slot).?;
            entry.pml4 = @intCast(self.index.pml4);
            if (link.level != .pdp) entry.pdp = @intCast(self.index.pdp);
            if (link.level == .pt) entry.pd = @intCast(self.index.pd);
            link.parent.* = link.physical | h.page_present | h.page_rw | h.page_user;
            const used = switch (link.level) {
                .pdp => &space.pdp_page_used_len,
                .pd => &space.pd_page_used_len,
                .pt => &space.pt_page_used_len,
            };
            used.* = @max(used.*, link.slot + 1); // ensurePage checked the rounded limit.
        }
        self.committed = true;
    }

    fn finish(self: *@This(), space: *UserAddressSpace) void {
        if (self.committed) return;
        // Nothing was published: no CPU barrier is needed for this rollback.
        for (self.links[0..self.count]) |link| levelStore(space, link.level).releasePageDetached(link.slot);
        reclaimTableMetadata(space);
    }
};

fn reclaimTableMetadata(space: *UserAddressSpace) void {
    space.pt_store.reclaimEmptyBanks();
    space.pd_store.reclaimEmptyBanks();
    space.pdp_store.reclaimEmptyBanks();
}

fn ensureUserPdpSlotForPml4(space: *UserAddressSpace, pml4_index: usize) ?usize {
    const h = hooks orelse return null;
    if (pml4_index >= 256) return null;
    if (findUserPdpSlotForPml4(space, pml4_index)) |slot| return slot;
    var path = TablePathPreparation{ .index = .{ .pml4 = pml4_index, .pdp = 0, .pd = 0, .pt = 0 } };
    defer path.finish(space);
    const slot = path.prepare(h, space, .pdp) orelse return null;
    path.publish(h, space);
    return slot;
}

pub fn ensureUserPtSlotForPd(space: *UserAddressSpace, pml4_index: usize, pdp_index: usize, pd_index: usize) ?usize {
    const h = hooks orelse return null;
    if (pml4_index >= 256 or pdp_index >= h.page_entries or pd_index >= h.page_entries) return null;
    if (findUserPtSlotForPd(space, pml4_index, pdp_index, pd_index)) |slot| return slot;
    var path = TablePathPreparation{ .index = .{ .pml4 = pml4_index, .pdp = pdp_index, .pd = pd_index, .pt = 0 } };
    defer path.finish(space);
    const slot = path.prepare(h, space, .pt) orelse return null;
    path.publish(h, space);
    return slot;
}

const pte_addr_mask: u64 = 0x000f_ffff_ffff_f000;
const pte_cache_write_through: u64 = 1 << 3;
const pte_cache_disable: u64 = 1 << 4;
const pte_pkey_shift: u6 = 59;

fn ptePaddr(entry: u64) u64 {
    return entry & pte_addr_mask;
}

fn pteFlagsForProt(h: Hooks, prot: kernel.MapProt) ?u64 {
    if (!prot.read) return null;
    return h.page_present |
        h.page_user |
        (if (prot.write) h.page_rw else 0) |
        (if (!prot.exec) h.page_nx else 0) |
        (@as(u64, prot.pkey) << pte_pkey_shift);
}

pub const MmioCache = enum {
    uc,
    uc_minus,
    wc_pat0,
    wc_pat1,
    wc_pat4,
    wc_pat5,
    wc_pat6,
    wc_pat7,

    pub fn patSlot(self: MmioCache) u3 {
        return switch (self) {
            .uc => 3,
            .uc_minus => 2,
            .wc_pat0 => 0,
            .wc_pat1 => 1,
            .wc_pat4 => 4,
            .wc_pat5 => 5,
            .wc_pat6 => 6,
            .wc_pat7 => 7,
        };
    }

    pub fn wcForPatSlot(slot: u3) ?MmioCache {
        return switch (slot) {
            0 => .wc_pat0,
            1 => .wc_pat1,
            4 => .wc_pat4,
            5 => .wc_pat5,
            6 => .wc_pat6,
            7 => .wc_pat7,
            else => null,
        };
    }
};

/// RAM cache changes invalidate actual user mappings in every active AS.
/// The caller holds all VM transactions; kernel aliases are flushed separately.
pub fn flushAllUserContexts() void {
    const h = hooks orelse return;
    for (0..h.user_spaces.capacity()) |space_index| {
        const space = h.user_spaces.get(space_index) orelse continue;
        if (space.cr3 == 0) continue;
        const principal = kernel.processPrincipalFromIndex(space_index) orelse continue;
        // Cache-attribute transitions invalidate aliases at unrelated VAs;
        // the old dummy page range only worked while all requests were full.
        h.flush_user_tlb_for_principal_range(principal, h.user_low_va, @intCast(h.user_top_va - h.user_low_va));
    }
}

fn pteFlagsForMmioProt(h: Hooks, prot: kernel.MapProt, cache: MmioCache) ?u64 {
    const flags = pteFlagsForProt(h, prot) orelse return null;
    // 4 KiB leaf PAT index = (PAT << 2) | (PCD << 1) | PWT. The slot
    // selected for WC is proven against every online CPU before this call.
    const cache_flags: u64 = switch (cache) {
        .uc => pte_cache_disable | pte_cache_write_through,
        .uc_minus => pte_cache_disable,
        .wc_pat0 => 0,
        .wc_pat1 => pte_cache_write_through,
        .wc_pat4 => 1 << 7,
        .wc_pat5 => (1 << 7) | pte_cache_write_through,
        .wc_pat6 => (1 << 7) | pte_cache_disable,
        .wc_pat7 => (1 << 7) | pte_cache_disable | pte_cache_write_through,
    };
    return flags | cache_flags;
}

// Caller holds the AS lock through preflight and publication. The physical
// policy writer must hold every AS lock, so a newly created alias cannot
// silently restore WB while another alias of the same RAM remains WC/UC.
fn pteFlagsForPhysicalPolicy(h: Hooks, paddr: u64, prot: kernel.MapProt, explicit: ?MmioCache) ?u64 {
    const flags = if (explicit) |cache| pteFlagsForMmioProt(h, prot, cache) orelse return null else pteFlagsForProt(h, prot) orelse return null;
    return switch (@import("../user_copy.zig").ramCachePolicyForLockedPage(paddr)) {
        .ordinary => flags,
        .blocked => null,
        .cached => |policy| blk: {
            if (prot.exec) break :blk null;
            const mode: MmioCache = switch (policy.mode) {
                .wb => break :blk null,
                .uc => if (policy.pat_slot == 3) .uc else break :blk null,
                .uc_minus => if (policy.pat_slot == 2) .uc_minus else break :blk null,
                .wc => MmioCache.wcForPatSlot(policy.pat_slot) orelse break :blk null,
            };
            const inherited = pteFlagsForMmioProt(h, prot, mode) orelse break :blk null;
            if (explicit != null and flags & 0x98 != inherited & 0x98) break :blk null;
            break :blk inherited;
        },
    };
}

fn userRangeEndVa(h: Hooks, va_start: u64, size_bytes: u64) ?u64 {
    return userRangeEndVaWithLowPageZero(h, va_start, size_bytes, false);
}

fn userRangeEndVaWithLowPageZero(h: Hooks, va_start: u64, size_bytes: u64, allow_low_page_zero: bool) ?u64 {
    if (size_bytes == 0) return null;
    if ((va_start & 0xFFF) != 0) return null;
    const end_va, const overflow = @addWithOverflow(va_start, size_bytes - 1);
    if (overflow != 0) return null;
    _ = userPageIndexForVaWithLowPageZero(h, va_start, allow_low_page_zero) orelse return null;
    _ = userPageIndexForVaWithLowPageZero(h, end_va, allow_low_page_zero) orelse return null;
    return end_va;
}

pub fn mapUserLinearRegion(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddr_start: u64,
    size_bytes: usize,
    writable: bool,
) bool {
    return mapUserLinearRegionWithProt(principal, va_start, paddr_start, size_bytes, .{
        .read = true,
        .write = writable,
        .exec = false,
    });
}

pub fn mapUserLinearRegionWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddr_start: u64,
    size_bytes: usize,
    prot: kernel.MapProt,
) bool {
    return mapUserLinearRegionWithPteFlags(principal, va_start, paddr_start, size_bytes, prot, null);
}

pub fn mapUserUncachedLinearRegionWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddr_start: u64,
    size_bytes: usize,
    prot: kernel.MapProt,
) bool {
    return mapUserMmioLinearRegionWithProt(principal, va_start, paddr_start, size_bytes, prot, .uc);
}

/// Caller proves the requested PAT/MTRR mode before publishing an MMIO FD.
pub fn mapUserMmioLinearRegionWithProt(principal: kernel.PrincipalId, va_start: u64, paddr_start: u64, size_bytes: usize, prot: kernel.MapProt, cache: MmioCache) bool {
    return mapUserLinearRegionWithPteFlags(principal, va_start, paddr_start, size_bytes, prot, cache);
}

/// Newly installed paging-structure links, not mappings or reservations. Keep
/// their old parent entries so a failed BAR admission can undo even a partial
/// ensureUserPtSlotForPd without consuming any PT/PD/PDP slots.
const MmioTablePreparation = struct {
    // One page is only the journal packing unit, never the transaction limit.
    const links_per_page = 4096 / @sizeOf(TableLink);
    count: usize = 0,
    old_pdp_used: usize,
    old_pd_used: usize,
    old_pt_used: usize,

    fn linkAt(space: *UserAddressSpace, index: usize) *TableLink {
        const page = space.scratch_store.descriptor(index / links_per_page).?.page.?;
        const links: *[links_per_page]TableLink = @ptrCast(page);
        return &links[index % links_per_page];
    }

    fn prepare(self: *@This(), space: *UserAddressSpace, index: UserPageIndex) ?usize {
        const h = hooks orelse return null;
        if (findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd)) |slot| return slot;
        var path = TablePathPreparation{ .index = index };
        defer path.finish(space);
        const slot = path.prepare(h, space, .pt) orelse return null;
        const end = std.math.add(usize, self.count, path.count) catch return null;
        // Reserve every journal page before any of this path becomes visible.
        for (self.count..end) |i| {
            _ = space.scratch_store.ensurePage(i / links_per_page, h.pt_allocator orelse return null) orelse return null;
        }
        for (path.links[0..path.count]) |link| {
            linkAt(space, self.count).* = link;
            self.count += 1;
        }
        path.publish(h, space);
        return slot;
    }

    fn rollback(self: *@This(), h: Hooks, space: *UserAddressSpace, principal: kernel.PrincipalId) void {
        if (self.count == 0) return;
        var remaining = self.count;
        while (remaining != 0) {
            remaining -= 1;
            const link = linkAt(space, remaining).*;
            link.parent.* = link.previous;
        }
        // Keep all metadata and pages assigned until every CPU acknowledges.
        h.flush_user_tlb_for_principal_range(principal, h.user_low_va, @intCast(h.user_top_va - h.user_low_va));
        for (0..self.count) |i| {
            const link = linkAt(space, i).*;
            levelStore(space, link.level).releasePageDetached(link.slot);
        }
        reclaimTableMetadata(space);
        space.pdp_page_used_len = self.old_pdp_used;
        space.pd_page_used_len = self.old_pd_used;
        space.pt_page_used_len = self.old_pt_used;
    }
};

/// Caller holds a VM transaction and has validated an unbacked PROT_NONE VMA
/// plus acquired an unpublished MMIO FD. This operation never changes either
/// VMA or low-level reservation state. All failures precede the PTE stores.
pub fn mapUserMmioOverlay(principal: kernel.PrincipalId, va_start: u64, paddr_start: u64, size_bytes: usize, prot: kernel.MapProt) bool {
    return mapUserMmioOverlayWithCache(principal, va_start, paddr_start, size_bytes, prot, .uc);
}

pub fn mapUserMmioOverlayWithCache(principal: kernel.PrincipalId, va_start: u64, paddr_start: u64, size_bytes: usize, prot: kernel.MapProt, cache: MmioCache) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (size_bytes == 0 or ((va_start | paddr_start | size_bytes) & 4095) != 0 or
        paddr_start == 0 or paddr_start >= h.physical_map_limit or
        size_bytes > h.physical_map_limit - paddr_start or prot.exec or prot.pkey != 0) return false;
    _ = userRangeEndVa(h, va_start, @intCast(size_bytes)) orelse return false;
    const flags = pteFlagsForMmioProt(h, prot, cache) orelse return false;
    var preparation = MmioTablePreparation{
        .old_pdp_used = space.pdp_page_used_len,
        .old_pd_used = space.pd_page_used_len,
        .old_pt_used = space.pt_page_used_len,
    };
    var committed = false;
    defer {
        if (!committed) preparation.rollback(h, space, principal);
        space.scratch_store.freeDetached();
    }

    var offset: u64 = 0;
    while (offset < size_bytes) : (offset += 4096) {
        const index = userPageIndexForVa(h, va_start + offset) orelse return false;
        const slot = preparation.prepare(space, index) orelse return false;
        const old = space.ptPage(slot).*[index.pt];
        if (old & h.page_present != 0) return false;
    }
    offset = 0;
    while (offset < size_bytes) : (offset += 4096) {
        const index = userPageIndexForVa(h, va_start + offset).?;
        const slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd).?;
        space.ptPage(slot).*[index.pt] = (paddr_start + offset) | flags;
    }
    committed = true;
    return true;
}

/// Detach only: the caller combines both parent levels with its PT batch
/// before ACK. Descriptors stay assigned, so neither slots nor PFNs can be
/// reused while a remote CPU still holds the old paging structure.
fn detachEmptyUserParents(h: Hooks, space: *UserAddressSpace, comptime level: TableLevel, start: u64, length: usize) usize {
    const end = start + length;
    const store = levelStore(space, level);
    var count: usize = 0;
    var it = store.iterator();
    while (it.next()) |slot| {
        const entry = it.current.?;
        if (entry.pml4 == UserAddressSpace.no_pd_index) continue;
        const shift = if (level == .pd) 30 else 39;
        const base = (@as(u64, entry.pml4) << 39) | (if (level == .pd) @as(u64, entry.pdp) << 30 else 0);
        if (base >= end or base + (@as(u64, 1) << shift) <= start or !std.mem.allEqual(u64, entry.page.?, 0)) continue;
        const parent: *u64 = if (level == .pd) blk: {
            const pdp = findUserPdpSlotForPml4(space, entry.pml4) orelse continue;
            break :blk &space.pdpPage(pdp)[entry.pdp];
        } else &space.pml4[entry.pml4];
        const pa = tablePaddr(h, entry.page.?) orelse continue;
        if (parent.* & h.page_addr_mask != pa) continue;
        parent.* = 0;
        store.descriptor(slot).?.retire_pending = true;
        count += 1;
    }
    return count;
}

/// Last-close and teardown are idempotent if PTEs have already gone. Validate
/// every remaining user PTE before removing any; never clear another backing.
pub fn unmapUserMmioOverlay(principal: kernel.PrincipalId, va_start: u64, paddr_start: u64, size_bytes: usize) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (size_bytes == 0 or ((va_start | paddr_start | size_bytes) & 4095) != 0 or
        paddr_start > std.math.maxInt(u64) - size_bytes) return false;
    _ = userRangeEndVa(h, va_start, @intCast(size_bytes)) orelse return false;
    var offset: u64 = 0;
    while (offset < size_bytes) : (offset += 4096) {
        const index = userPageIndexForVa(h, va_start + offset).?;
        const slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse continue;
        const pte = space.ptPage(slot).*[index.pt];
        if (pte & (h.page_present | h.page_user) != (h.page_present | h.page_user)) continue;
        if (ptePaddr(pte) != paddr_start + offset) return false;
    }
    offset = 0;
    while (offset < size_bytes) : (offset += 4096) {
        const index = userPageIndexForVa(h, va_start + offset).?;
        const slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse continue;
        const pte = &space.ptPage(slot)[index.pt];
        if (pte.* & (h.page_present | h.page_user) == (h.page_present | h.page_user)) pte.* = 0;
    }
    retireEmptyUserPtRange(h, space, principal, va_start, size_bytes, true);
    return true;
}

fn mapUserLinearRegionWithPteFlags(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddr_start: u64,
    size_bytes: usize,
    prot: kernel.MapProt,
    cache: ?MmioCache,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (size_bytes == 0) return false;
    if ((va_start & 0xFFF) != 0 or (paddr_start & 0xFFF) != 0) return false;

    const size_u64: u64 = @intCast(size_bytes);
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;
    const map_end_pa = paddr_start + size_bytes - 1;
    if (map_end_pa >= h.physical_map_limit) return false;
    if ((size_bytes & 0xFFF) != 0) return false;

    const page_count_u64: u64 = @intCast(size_bytes / 4096);
    var offset: u64 = 0;
    while (offset < size_bytes) : (offset += 4096) {
        _ = pteFlagsForPhysicalPolicy(h, paddr_start + offset, prot, cache) orelse return false;
    }
    offset = 0;
    while (offset < size_bytes) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = space.ptPage(pt_slot);
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) != 0) return false;
    }
    if (!reserveUserMapping(principal, va_start, page_count_u64, .linear_region, prot.write)) return false;

    offset = 0;
    while (offset < size_bytes) : (offset += 4096) {
        const va = va_start + offset;
        const paddr = paddr_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = space.ptPage(pt_slot);
        pt_page[index.pt] = paddr | pteFlagsForPhysicalPolicy(h, paddr, prot, cache).?;
    }

    return true;
}

fn slicePageAt(pages: []const u64, index: usize) ?u64 {
    return pages[index];
}

fn mapTrustedUserPaddrsWithProtInternal(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
    reserve_pages: bool,
    allow_low_page_zero: bool,
    device_cache: ?MmioCache,
) bool {
    return mapUserPageSequence(principal, va_start, paddrs.len, paddrs, slicePageAt, prot, reserve_pages, allow_low_page_zero, device_cache);
}

/// Caller keeps KernelState and the target VM transaction held. pageAt must
/// be repeatable and side-effect-free throughout validation, preparation and
/// publication. A failed lookup is allowed only during preflight; the same
/// validated sequence cannot change while the caller's exclusion is held.
pub fn mapTrustedUserPageSequence(principal: kernel.PrincipalId, va_start: u64, count: usize, context: anytype, comptime pageAt: anytype, prot: kernel.MapProt, low_page_zero: bool) bool {
    if (low_page_zero and (va_start != 0 or count != 1 or !prot.read or prot.write or !prot.exec)) return false;
    return mapUserPageSequence(principal, va_start, count, context, pageAt, prot, true, low_page_zero, null);
}

fn mapUserPageSequence(principal: kernel.PrincipalId, va_start: u64, count: usize, context: anytype, comptime pageAt: anytype, prot: kernel.MapProt, reserve_pages: bool, allow_low_page_zero: bool, device_cache: ?MmioCache) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (count == 0 or (va_start & 0xFFF) != 0) return false;
    const page_count_u64: u64 = @intCast(count);
    const size_u64 = std.math.mul(u64, page_count_u64, 4096) catch return false;
    _ = userRangeEndVaWithLowPageZero(h, va_start, size_u64, allow_low_page_zero) orelse return false;

    // Validate the complete source and every existing private PTE before
    // allocating tables. No prefix is published if a late PFN/attribute fails.
    for (0..count) |page_index| {
        const paddr = pageAt(context, page_index) orelse return false;
        if ((paddr & 0xFFF) != 0 or paddr >= h.physical_map_limit) return false;
        _ = pteFlagsForPhysicalPolicy(h, paddr, prot, device_cache) orelse return false;
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVaWithLowPageZero(h, va, allow_low_page_zero) orelse return false;
        if (findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd)) |pt_slot| {
            const old_entry = space.ptPage(pt_slot)[index.pt];
            if ((old_entry & h.page_present) != 0) return false;
        }
    }
    // Prepare the entire range before its single reservation/publication.
    // As before, failed preparation may retain empty table capacity, but never
    // publishes user leaves or a partial reservation.
    for (0..count) |page_index| {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVaWithLowPageZero(h, va, allow_low_page_zero).?;
        const pt_slot = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const old_entry = space.ptPage(pt_slot)[index.pt];
        if ((old_entry & h.page_present) != 0) return false;
    }
    if (reserve_pages and !reserveUserMapping(principal, va_start, page_count_u64, .linear_region, prot.write)) return false;

    // No fallible operation remains: do not reallocate PTs or publish batches.
    for (0..count) |page_index| {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVaWithLowPageZero(h, va, allow_low_page_zero).?;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd).?;
        const paddr = pageAt(context, page_index).?;
        space.ptPage(pt_slot)[index.pt] = paddr | pteFlagsForPhysicalPolicy(h, paddr, prot, device_cache).?;
    }
    return true;
}

/// A borrowed view of a VMA's initial PFNs, not a VM policy or ownership layer.
/// K + the target VM transaction keep the VMA, backing and attributes stable.
pub const InitialVmaPages = struct {
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    base: u64,
    prot: kernel.MapProt,

    pub fn at(self: @This(), index: usize) ?u64 {
        const mapping = self.state.nativeVmaInitialMapping(self.principal, self.base + @as(u64, @intCast(index)) * 4096) orelse return null;
        // The mapper accepts one protection for the whole sequence. Refuse a
        // heterogeneous source rather than silently applying the first page's.
        if (!std.meta.eql(mapping.prot, self.prot)) return null;
        return mapping.paddr;
    }
};

pub fn mapTrustedUserPaddrsWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
) bool {
    return mapTrustedUserPaddrsWithProtInternal(principal, va_start, paddrs, prot, true, false, null);
}

pub fn mapTrustedLowPageZeroPaddrsWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
) bool {
    if (va_start != 0 or paddrs.len != 1) return false;
    if (!prot.read or prot.write or !prot.exec) return false;
    return mapTrustedUserPaddrsWithProtInternal(principal, va_start, paddrs, prot, true, true, null);
}

pub fn mapLazyLowPageZeroPaddrsWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
) bool {
    if (va_start != 0 or paddrs.len != 1) return false;
    if (!prot.read or prot.write or !prot.exec) return false;
    return mapTrustedUserPaddrsWithProtInternal(principal, va_start, paddrs, prot, false, true, null);
}

pub const UserPtUsage = struct {
    used: usize = 0,
    empty: usize = 0,
    user_tables: usize = 0,
    supervisor_only: usize = 0,
    nonpresent_tables: usize = 0,
    user_entries: usize = 0,
    supervisor_entries: usize = 0,
    nonpresent_entries: usize = 0,
};

// Failure-only diagnostic. The caller holds the address-space lock; do not
// scan these tables on successful faults or mistake reserved empty PTs for
// live mappings. Supervisor-only helper tables are not reclaimable by count.
// Borrow each PT under that lock: a full aligned table copy would inflate the
// fault failure path's IST frame even when the diagnostic is not executed.
pub fn userPtUsage(space: *const UserAddressSpace) UserPtUsage {
    const h = hooks orelse return .{};
    var usage = UserPtUsage{};
    var pt_it = space.pt_store.iterator();
    while (pt_it.next()) |slot| {
        const pd = space.ptPdIndex(slot).*;
        if (pd == UserAddressSpace.no_pd_index) continue;
        usage.used += 1;
        var user: usize = 0;
        var supervisor: usize = 0;
        var nonpresent: usize = 0;
        for (space.ptPage(slot)) |entry| {
            if (entry & h.page_present == 0) {
                if (entry != 0) nonpresent += 1;
                continue;
            }
            if (entry & h.page_user != 0) user += 1 else supervisor += 1;
        }
        usage.user_entries += user;
        usage.supervisor_entries += supervisor;
        usage.nonpresent_entries += nonpresent;
        if (user != 0) {
            usage.user_tables += 1;
        } else if (supervisor != 0) {
            usage.supervisor_only += 1;
        } else if (nonpresent != 0) {
            usage.nonpresent_tables += 1;
        } else {
            usage.empty += 1;
        }
    }
    return usage;
}

pub fn mapLazyUserPaddrsWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
) bool {
    if (mapTrustedUserPaddrsWithProtInternal(principal, va_start, paddrs, prot, false, false, null)) return true;
    if (lockAddressSpace(principal)) {
        defer unlockAddressSpace(principal);
        if (getUserSpace(principal)) |space| {
            const usage = userPtUsage(space);
            @import("../kernel_log.zig").writeFmt(
                "vm: fault pte failed principal={} va=0x{x} pages={} pt_used={}/{} pt_empty={} pt_user={} pt_supervisor={} pt_nonpresent={} pte_user={} pte_supervisor={} pte_nonpresent={} pd_high={} pdp_high={}\n",
                .{ @intFromEnum(principal), va_start, paddrs.len, usage.used, space.ptCapacity(), usage.empty, usage.user_tables, usage.supervisor_only, usage.nonpresent_tables, usage.user_entries, usage.supervisor_entries, usage.nonpresent_entries, space.pd_page_used_len, space.pdp_page_used_len },
            );
        }
    }
    return false;
}

pub fn mapNativeVmaFaultPage(principal: kernel.PrincipalId, va: u64, mapping: kernel.NativeVmaFaultMapping) bool {
    // The cache snapshot cannot be replaced while this address space is
    // locked; fault-time aliases therefore join the same physical policy.
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const paddrs = [_]u64{mapping.paddr};
    switch (@import("../user_copy.zig").ramCachePolicyForLockedPage(mapping.paddr)) {
        .blocked => return false,
        .ordinary => {},
        .cached => |policy| {
            if (mapping.device_pat_slot != null or mapping.prot.exec) return false;
            const cache: MmioCache = switch (policy.mode) {
                .wb => return false,
                .uc => if (policy.pat_slot == 3) .uc else return false,
                .uc_minus => if (policy.pat_slot == 2) .uc_minus else return false,
                .wc => MmioCache.wcForPatSlot(policy.pat_slot) orelse return false,
            };
            return mapTrustedUserPaddrsWithProtInternal(principal, va, &paddrs, mapping.prot, false, false, cache);
        },
    }
    if (mapping.device_pat_slot) |pat| {
        const cache: MmioCache = if (pat == 3) .uc else MmioCache.wcForPatSlot(pat) orelse return false;
        return mapTrustedUserPaddrsWithProtInternal(principal, va, &paddrs, mapping.prot, false, false, cache);
    }
    return mapLazyUserPaddrsWithProt(principal, va, &paddrs, mapping.prot);
}

pub fn remapTrustedUserPaddrsWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (paddrs.len == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const page_count_u64: u64 = @intCast(paddrs.len);
    const size_u64 = page_count_u64 * 4096;
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;

    var page_index: usize = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const paddr = paddrs[page_index];
        if ((paddr & 0xFFF) != 0 or paddr >= h.physical_map_limit) return false;
        _ = pteFlagsForPhysicalPolicy(h, paddr, prot, null) orelse return false;
    }

    // Hardware A/D updates do not change mapping identity. Leave an entirely
    // identical range untouched, including its A/D bits, without a shootdown.
    const accessed_dirty: u64 = (1 << 5) | (1 << 6);
    page_index = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse break;
        const old = space.ptPage(pt_slot).*[index.pt];
        const desired = paddrs[page_index] | pteFlagsForPhysicalPolicy(h, paddrs[page_index], prot, null).?;
        if ((old & ~accessed_dirty) != (desired & ~accessed_dirty)) break;
    }
    if (page_index == paddrs.len) return true;

    // Reserve all page tables before replacing the first live mapping.
    for (paddrs, 0..) |_, i| {
        const index = userPageIndexForVa(h, va_start + @as(u64, @intCast(i)) * 4096).?;
        _ = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
    }
    page_index = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = space.ptPage(pt_slot);
        pt_page[index.pt] = paddrs[page_index] | pteFlagsForPhysicalPolicy(h, paddrs[page_index], prot, null).?;
    }
    h.flush_user_tlb_for_principal_range(principal, va_start, @intCast(size_u64));

    return true;
}

pub fn mapOrRemapTrustedUserPaddrsWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (paddrs.len == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const page_count_u64: u64 = @intCast(paddrs.len);
    const size_u64 = page_count_u64 * 4096;
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;

    var page_index: usize = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const paddr = paddrs[page_index];
        if ((paddr & 0xFFF) != 0 or paddr >= h.physical_map_limit) return false;
        _ = pteFlagsForPhysicalPolicy(h, paddr, prot, null) orelse return false;
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse return false;
        if (findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd)) |pt_slot| {
            const old_entry = space.ptPage(pt_slot).*[index.pt];
            if ((old_entry & h.page_present) != 0 and (old_entry & h.page_user) == 0) return false;
        }
    }

    // Page-table allocation must finish before accounting reservations and
    // PTE publication; a late allocation failure must not leave a prefix live.
    for (paddrs, 0..) |_, i| {
        const index = userPageIndexForVa(h, va_start + @as(u64, @intCast(i)) * 4096).?;
        _ = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
    }
    page_index = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse return false;
        var present = false;
        if (findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd)) |pt_slot| {
            const old_entry = space.ptPage(pt_slot).*[index.pt];
            present = (old_entry & h.page_present) != 0 and (old_entry & h.page_user) != 0;
        }
        if (!present and !reserveUserMapping(principal, va, 1, .linear_region, prot.write)) {
            var rollback_index: usize = 0;
            while (rollback_index < page_index) : (rollback_index += 1) {
                const rollback_va = va_start + @as(u64, @intCast(rollback_index)) * 4096;
                const rollback_page = userPageIndexForVa(h, rollback_va) orelse continue;
                var rollback_present = false;
                if (findUserPtSlotForPd(space, rollback_page.pml4, rollback_page.pdp, rollback_page.pd)) |pt_slot| {
                    const old_entry = space.ptPage(pt_slot).*[rollback_page.pt];
                    rollback_present = (old_entry & h.page_present) != 0 and (old_entry & h.page_user) != 0;
                }
                if (!rollback_present) _ = releaseUserMapping(principal, rollback_va, 1);
            }
            return false;
        }
    }

    page_index = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        space.ptPage(pt_slot).*[index.pt] = paddrs[page_index] | pteFlagsForPhysicalPolicy(h, paddrs[page_index], prot, null).?;
    }
    h.flush_user_tlb_for_principal_range(principal, va_start, @intCast(size_u64));

    return true;
}

fn preflightProtection(h: Hooks, space: *UserAddressSpace, va_start: u64, bytes: u64, prot: kernel.MapProt, sparse: bool) bool {
    var offset: u64 = 0;
    while (offset < bytes) : (offset += 4096) {
        const index = userPageIndexForVa(h, va_start + offset) orelse return false;
        const slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse {
            if (sparse) continue;
            return false;
        };
        const old = space.ptPage(slot)[index.pt];
        if (old & h.page_present == 0) {
            if (sparse) continue;
            return false;
        }
        if (old & h.page_user == 0) return false;
        const flags = pteFlagsForPhysicalPolicy(h, ptePaddr(old), prot, null) orelse return false;
        switch (@import("../user_copy.zig").ramCachePolicyForLockedPage(ptePaddr(old))) {
            .cached => if (flags & 0x98 != old & 0x98) return false,
            .blocked => return false,
            .ordinary => {},
        }
    }
    return true;
}

fn publishProtection(entry: *u64, flags: u64) void {
    // A/D can change after preflight even with the AS lock held. Retry the
    // permission update rather than losing a hardware write to these bits.
    var old = @atomicLoad(u64, entry, .acquire);
    while (true) {
        const desired = ptePaddr(old) | flags | (old & (0x98 | (1 << 5) | (1 << 6)));
        old = @cmpxchgWeak(u64, entry, old, desired, .acq_rel, .acquire) orelse return;
    }
}

pub fn protectUserLinearRegionWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
    prot: kernel.MapProt,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    const pte_flags = pteFlagsForProt(h, prot) orelse return false;
    if (size_bytes == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const size_u64: u64 = @intCast(size_bytes);
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;
    if (!preflightProtection(h, space, va_start, size_u64, prot, false)) return false;

    var offset: u64 = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = space.ptPage(pt_slot);
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) == 0) return false;
        if ((old_entry & h.page_user) == 0) return false;
        // Permission changes must not change the physical memory type.
        publishProtection(&pt_page[index.pt], pte_flags);
    }
    h.flush_user_tlb_for_principal_range(principal, va_start, size_bytes);

    return true;
}

pub fn protectPresentUserLinearRegionWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
    prot: kernel.MapProt,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    const pte_flags = pteFlagsForProt(h, prot) orelse return false;
    if (size_bytes == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const size_u64: u64 = @intCast(size_bytes);
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;
    if (!preflightProtection(h, space, va_start, size_u64, prot, true)) return false;

    var offset: u64 = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse continue;
        const pt_page: *[512]u64 = space.ptPage(pt_slot);
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) == 0) continue;
        if ((old_entry & h.page_user) == 0) return false;
        publishProtection(&pt_page[index.pt], pte_flags);
    }
    h.flush_user_tlb_for_principal_range(principal, va_start, size_bytes);

    return true;
}

pub fn unmapUserLinearRegion(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (size_bytes == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const size_u64: u64 = @intCast(size_bytes);
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;

    if (!prepareReservationSlots(space, reservationSplitSlotsRequired(space, va_start, size_u64))) return false;
    var offset: u64 = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *const [512]u64 = space.ptPage(pt_slot);
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) == 0) return false;
        if ((old_entry & h.page_user) == 0) return false;
    }

    offset = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = space.ptPage(pt_slot);
        pt_page[index.pt] = 0;
    }
    if (!releaseUserMapping(principal, va_start, size_u64 / 4096)) return false;
    retireEmptyUserPtRange(h, space, principal, va_start, size_bytes, true);

    return true;
}

/// Validate every fallible condition of `unmapPresentUserLinearRegion`
/// without changing PTEs or reservations.  Callers may then prepare external
/// backing state before crossing a no-return commit boundary.
pub fn unmapPresentUserLinearRegionSplitSlotsRequired(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
) ?usize {
    return unmapPresentSplitSlotsCommon(principal, va_start, size_bytes);
}

/// Caller holds the VM transaction and has validated this entire source span
/// lies in its native VMA. Absent lazy holes require no leaf mutation.
pub fn unmapPresentVmaSourceSplitSlotsRequired(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
) ?usize {
    return unmapPresentSplitSlotsCommon(principal, va_start, size_bytes);
}

fn unmapPresentSplitSlotsCommon(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
) ?usize {
    if (!lockAddressSpace(principal)) return null;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return null;
    const space = getUserSpace(principal) orelse return null;
    if (size_bytes == 0 or (va_start & 0xFFF) != 0) return null;
    const size_u64: u64 = @intCast(size_bytes);
    _ = userRangeEndVa(h, va_start, size_u64) orelse return null;

    if (!userLeavesOnly(h, space, va_start, va_start + size_u64)) return null;

    return reservationSplitSlotsRequired(space, va_start, size_u64);
}

fn userLeavesOnly(h: Hooks, space: *const UserAddressSpace, start: u64, end: u64) bool {
    // Empty user trees never inherit supervisor leaves. Refuse corruption
    // before mutation rather than silently retaining or reclaiming an alias.
    var it = space.pt_store.iterator();
    while (it.next()) |slot| {
        const span = ptSlotIntersection(space, slot, start, end) orelse continue;
        for (space.ptPage(slot).*[span.first..span.last]) |entry| {
            if (entry & h.page_present != 0 and entry & h.page_user == 0) return false;
        }
    }
    return true;
}

fn reservationSplitSlotsRequired(space: *const UserAddressSpace, va_start: u64, size_u64: u64) usize {
    const release_start_page = va_start >> 12;
    const release_end_page = release_start_page + size_u64 / 4096;
    var needs_split_slot = false;
    var records = space.reservations.iterator();
    while (records.next()) |record| {
        const reservation = record.value;
        if (!reservation.active) continue;
        const record_start_page = reservation.base_va >> 12;
        const record_end_page = record_start_page + reservation.page_count;
        if (record_start_page >= release_end_page or release_start_page >= record_end_page) continue;
        if (release_start_page > record_start_page and release_end_page < record_end_page) {
            needs_split_slot = true;
            break;
        }
    }
    return @intFromBool(needs_split_slot);
}

/// The caller holds the VM transaction, MAP_INTO authority, and has rejected
/// pinned target ranges. This preflight and its commit inspect only allocated
/// tables, so large absent ranges do not require a page-by-page walk.
pub fn unmapPresentRemoteUserRegionSplitSlotsRequired(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
) ?usize {
    if (!lockAddressSpace(principal)) return null;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return null;
    const space = getUserSpace(principal) orelse return null;
    if (size_bytes == 0 or ((va_start | size_bytes) & 0xfff) != 0) return null;
    _ = userRangeEndVa(h, va_start, @intCast(size_bytes)) orelse return null;
    if (!userLeavesOnly(h, space, va_start, va_start + size_bytes)) return null;
    return reservationSplitSlotsRequired(space, va_start, @intCast(size_bytes));
}

/// Commit a validated remote range without walking its potentially enormous
/// unmapped gaps. Only allocated PTs intersecting the range are inspected.
/// Existing retirement detaches an empty user PT before the synchronous
/// shootdown and reuses its storage only after all target CPUs acknowledge.
pub fn unmapPresentRemoteUserRegion(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (size_bytes == 0 or ((va_start | size_bytes) & 0xfff) != 0) return false;
    _ = userRangeEndVa(h, va_start, @intCast(size_bytes)) orelse return false;
    if (!userLeavesOnly(h, space, va_start, va_start + size_bytes)) return false;
    if (!prepareReservationSlots(space, reservationSplitSlotsRequired(space, va_start, @intCast(size_bytes)))) return false;
    const end = va_start + size_bytes;

    var pt_it = space.pt_store.iterator();
    while (pt_it.next()) |slot| {
        const pml4 = space.ptPml4Index(slot).*;
        const pdp = space.ptPdpIndex(slot).*;
        const pd = space.ptPdIndex(slot).*;
        if (pml4 == UserAddressSpace.no_pd_index or
            pdp == UserAddressSpace.no_pd_index or pd == UserAddressSpace.no_pd_index) continue;
        const base = (@as(u64, pml4) << 39) | (@as(u64, pdp) << 30) | (@as(u64, pd) << 21);
        const overlap_start = @max(va_start, base);
        const overlap_end = @min(end, base + 2 * 1024 * 1024);
        if (overlap_start >= overlap_end) continue;
        const first: usize = @intCast((overlap_start - base) >> 12);
        const last: usize = @intCast((overlap_end - base) >> 12);
        for (space.ptPage(slot).*[first..last]) |*entry| {
            if ((entry.* & (h.page_present | h.page_user)) == (h.page_present | h.page_user))
                entry.* = 0;
        }
    }
    _ = releaseUserMapping(principal, va_start, @as(u64, @intCast(size_bytes)) / 4096);
    retireEmptyUserPtRange(h, space, principal, va_start, size_bytes, true);
    return true;
}

pub fn freeUserReservationSlotCount(principal: kernel.PrincipalId) usize {
    if (!lockAddressSpace(principal)) return 0;
    defer unlockAddressSpace(principal);
    const space = getUserSpace(principal) orelse return 0;
    return space.reservations.preparedFreeCount(std.math.maxInt(usize));
}

pub fn unmapPresentUserLinearRegion(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
) bool {
    const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.commit, stage_sample);
    return unmapPresentCommon(principal, va_start, size_bytes);
}

/// Only for a source span validated under the native VMA transaction before
/// commit; not for arbitrary fixed targets or capability-owned mappings.
pub fn unmapPresentVmaSource(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
) bool {
    const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.commit, stage_sample);
    return unmapPresentCommon(principal, va_start, size_bytes);
}

fn unmapPresentCommon(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (size_bytes == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const size_u64: u64 = @intCast(size_bytes);
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;

    if (!prepareReservationSlots(space, reservationSplitSlotsRequired(space, va_start, @intCast(size_bytes)))) return false;
    if (!userLeavesOnly(h, space, va_start, va_start + size_u64)) return false;
    var leaf_changed = false;
    var pt_it = space.pt_store.iterator();
    while (pt_it.next()) |slot| {
        const span = ptSlotIntersection(space, slot, va_start, va_start + size_u64) orelse continue;
        for (space.ptPage(slot).*[span.first..span.last]) |*entry| {
            if ((entry.* & (h.page_present | h.page_user)) == (h.page_present | h.page_user)) {
                entry.* = 0;
                leaf_changed = true;
            }
        }
    }

    _ = releaseUserMapping(principal, va_start, size_u64 / 4096);
    retireEmptyUserPtRange(h, space, principal, va_start, size_bytes, leaf_changed);

    return true;
}

/// Validate a no-op VM operation without changing PTEs or issuing a shootdown.
pub fn validateUserLinearRegion(principal: kernel.PrincipalId, va_start: u64, size_bytes: usize) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    _ = getUserSpace(principal) orelse return false;
    return userRangeEndVa(h, va_start, @intCast(size_bytes)) != null;
}

pub fn invalidatePresentUserLinearRegionPtes(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (size_bytes == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const size_u64: u64 = @intCast(size_bytes);
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;

    if (!userLeavesOnly(h, space, va_start, va_start + size_u64)) return false;
    var pt_it = space.pt_store.iterator();
    while (pt_it.next()) |slot| {
        const span = ptSlotIntersection(space, slot, va_start, va_start + size_u64) orelse continue;
        for (space.ptPage(slot).*[span.first..span.last]) |*entry| {
            if ((entry.* & (h.page_present | h.page_user)) == (h.page_present | h.page_user)) entry.* = 0;
        }
    }

    h.flush_user_tlb_for_principal_range(principal, va_start, size_bytes);

    return true;
}

/// Clear a fully-present user range only when every PTE still names the
/// expected physical page. Validation precedes mutation, and the range TLB
/// shootdown completes before return, so callers may then copy and retire the
/// old pages without racing remote user-mode accesses.
pub fn invalidatePresentUserPaddrsIfCurrent(
    principal: kernel.PrincipalId,
    va_start: u64,
    expected_paddrs: []const u64,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (expected_paddrs.len == 0 or (va_start & 0xFFF) != 0) return false;
    const size_u64, const size_overflow = @mulWithOverflow(
        @as(u64, @intCast(expected_paddrs.len)),
        @as(u64, 4096),
    );
    if (size_overflow != 0 or size_u64 > std.math.maxInt(usize)) return false;
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;

    for (expected_paddrs, 0..) |expected, page_index| {
        if (expected == 0 or (expected & 0xFFF) != 0) return false;
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const old_entry = space.ptPage(pt_slot).*[index.pt];
        if ((old_entry & h.page_present) == 0 or (old_entry & h.page_user) == 0 or
            (old_entry & h.page_addr_mask) != expected)
        {
            return false;
        }
    }
    for (expected_paddrs, 0..) |_, page_index| {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse unreachable;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse unreachable;
        space.ptPage(pt_slot).*[index.pt] = 0;
    }
    h.flush_user_tlb_for_principal_range(principal, va_start, @intCast(size_u64));
    return true;
}

/// Install a range previously invalidated by
/// invalidatePresentUserPaddrsIfCurrent. Every destination PTE and physical
/// address is checked before the first store, making failure non-mutating.
pub fn installInvalidatedUserPaddrsWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (paddrs.len == 0 or (va_start & 0xFFF) != 0) return false;
    const size_u64, const size_overflow = @mulWithOverflow(
        @as(u64, @intCast(paddrs.len)),
        @as(u64, 4096),
    );
    if (size_overflow != 0 or size_u64 > std.math.maxInt(usize)) return false;
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;

    for (paddrs, 0..) |paddr, page_index| {
        if (paddr == 0 or (paddr & 0xFFF) != 0 or paddr >= h.physical_map_limit) return false;
        _ = pteFlagsForPhysicalPolicy(h, paddr, prot, null) orelse return false;
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        if ((space.ptPage(pt_slot).*[index.pt] & h.page_present) != 0) return false;
    }
    for (paddrs, 0..) |paddr, page_index| {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse unreachable;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse unreachable;
        space.ptPage(pt_slot).*[index.pt] = paddr | pteFlagsForPhysicalPolicy(h, paddr, prot, null).?;
    }
    h.flush_user_tlb_for_principal_range(principal, va_start, @intCast(size_u64));
    return true;
}

pub fn collectUserLinearRegionPaddrs(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
    out_paddrs: []u64,
) ?usize {
    const h = hooks orelse return null;
    const space = getUserSpace(principal) orelse return null;
    if (size_bytes == 0) return null;
    if ((va_start & 0xFFF) != 0) return null;

    const size_u64: u64 = @intCast(size_bytes);
    if ((size_u64 & 0xFFF) != 0) return null;
    const page_count: usize = @intCast(size_u64 / 4096);
    if (page_count == 0 or page_count > out_paddrs.len) return null;
    _ = userRangeEndVa(h, va_start, size_u64) orelse return null;

    var page_index: usize = 0;
    while (page_index < page_count) : (page_index += 1) {
        const va = va_start + @as(u64, @intCast(page_index * 4096));
        const index = userPageIndexForVa(h, va) orelse return null;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return null;
        const pt_page: *const [512]u64 = space.ptPage(pt_slot);
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) == 0) return null;
        if ((old_entry & h.page_user) == 0) return null;
        out_paddrs[page_index] = ptePaddr(old_entry);
    }

    return page_count;
}

pub fn unmapUserMappedPaddr(principal: kernel.PrincipalId, paddr: u64) usize {
    const h = hooks orelse return 0;
    const space = getUserSpace(principal) orelse return 0;
    if ((paddr & 0xFFF) != 0) return 0;

    var removed: usize = 0;
    var first: u64 = std.math.maxInt(u64);
    var end: u64 = 0;
    var it = space.pt_store.iterator();
    while (it.next()) |slot| {
        const pd_index_meta = space.ptPdIndex(slot).*;
        const pml4_index_meta = space.ptPml4Index(slot).*;
        const pdp_index_meta = space.ptPdpIndex(slot).*;
        if (pml4_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pdp_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pd_index_meta == UserAddressSpace.no_pd_index) continue;
        const pml4_index: usize = @intCast(pml4_index_meta);
        const pdp_index: usize = @intCast(pdp_index_meta);
        const pd_index: usize = @intCast(pd_index_meta);
        var pt_index: usize = 0;
        while (pt_index < h.page_entries) : (pt_index += 1) {
            const pt_page: *[512]u64 = space.ptPage(slot);
            const old_entry = pt_page[pt_index];
            if ((old_entry & h.page_present) == 0) continue;
            if ((old_entry & h.page_user) == 0) continue;
            if (ptePaddr(old_entry) != paddr) continue;
            pt_page[pt_index] = 0;
            const va = (@as(u64, @intCast(pml4_index)) << 39) |
                (@as(u64, @intCast(pdp_index)) << 30) |
                (@as(u64, @intCast(pd_index)) << 21) |
                (@as(u64, @intCast(pt_index)) << 12);
            _ = releaseUserMapping(principal, va, 1);
            first = @min(first, va);
            end = @max(end, va + 4096);
            removed += 1;
        }
    }
    // The caller owns the physical page until this synchronous ACK returns.
    if (removed != 0) h.flush_user_tlb_for_principal_range(principal, first, @intCast(end - first));
    return removed;
}

pub fn collectUserMappedPaddrs(principal: kernel.PrincipalId, out_paddrs: []u64) usize {
    const h = hooks orelse return 0;
    const space = getUserSpace(principal) orelse return 0;
    var count: usize = 0;
    var it = space.pt_store.iterator();
    while (it.next()) |slot| {
        if (space.ptPml4Index(slot).* == UserAddressSpace.no_pd_index) continue;
        if (space.ptPdpIndex(slot).* == UserAddressSpace.no_pd_index) continue;
        if (space.ptPdIndex(slot).* == UserAddressSpace.no_pd_index) continue;
        const pt_page: *const [512]u64 = space.ptPage(slot);
        var pt_index: usize = 0;
        while (pt_index < h.page_entries) : (pt_index += 1) {
            const entry = pt_page[pt_index];
            if ((entry & h.page_present) == 0) continue;
            if ((entry & h.page_user) == 0) continue;
            const paddr = ptePaddr(entry);
            if (paddr == 0) continue;
            var duplicate = false;
            for (out_paddrs[0..count]) |seen| {
                if (seen == paddr) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            if (count >= out_paddrs.len) return count;
            out_paddrs[count] = paddr;
            count += 1;
        }
    }
    return count;
}

pub const MappedUserPage = struct {
    va: u64,
    paddr: u64,
    writable: bool,
};

/// These records borrow live PT storage, so the outer state and all VM locks
/// must remain held from preparation through publication. They do not grant
/// ownership or validate VMA private/COW, DMA, or supervisor aliases.
pub const RamCacheUserAlias = struct {
    principal: kernel.PrincipalId,
    va: u64,
    leaf: *u64,
    original: u64,
};

pub const RamCacheAliasError = error{ Invalid, NoSpace, ExecutableAlias, LargeAlias, GlobalAlias };

fn ramCacheParentMatches(actual: u64, expected: u64) bool {
    // Non-leaf tables acquire Accessed, not Dirty. Cache/permission/address
    // differences are semantic and must never be hidden by this comparison.
    return (actual & ~@as(u64, 1 << 5)) == (expected & ~@as(u64, 1 << 5));
}

// Index by the already-validated parent PD slot, not by virtual address.
// This rejects even two descriptors naming the same PT page/key, while the
// alias visitor can distinguish tracked leaves from unknown PDEs in O(1).
const RamCacheTrackedPts = struct {
    store: *pt_storage.Store,
    allocator: pt_storage.Allocator,
    const bits_per_page = 4096 * 8;
    fn reset(self: *@This()) void {
        self.store.resetDetachedContents();
    }
    fn set(self: *@This(), key: usize) !void {
        const page = self.store.ensurePage(key / bits_per_page, self.allocator) orelse return error.OutOfMemory;
        page[(key % bits_per_page) / 64] |= @as(u64, 1) << @as(u6, @intCast(key % 64));
    }
    fn isSet(self: *const @This(), key: usize) bool {
        const entry = self.store.descriptorConst(key / bits_per_page) orelse return false;
        const page = entry.page orelse return false;
        return page[(key % bits_per_page) / 64] & (@as(u64, 1) << @as(u6, @intCast(key % 64))) != 0;
    }
};

fn ramCachePtIsTracked(tracked_pts: *const RamCacheTrackedPts, key: usize) bool {
    return tracked_pts.isSet(key);
}

test "MMIO overlay RAM tracked PT membership crosses word and parent boundaries" {
    const PageAllocator = struct {
        fn allocate(_: *anyopaque, pages: usize) ?pt_storage.MappedExtent {
            std.debug.assert(pages == 1);
            const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096) catch return null;
            return .{ .allocation = .{ .physical = .fromInt(@intFromPtr(bytes.ptr)), .pages = 1, .region_id = 83 }, .bytes = bytes };
        }
        fn release(_: *anyopaque, grant: pt_storage.PhysicalExtent) bool {
            std.debug.assert(grant.pages == 1 and grant.region_id == 83);
            const page: pt_storage.PagePointer = @ptrFromInt(grant.physical.toInt());
            const bytes: []align(4096) u8 = page;
            std.testing.allocator.free(bytes);
            return true;
        }
    };
    var dummy: u8 = 0;
    const allocator = pt_storage.Allocator{ .context = &dummy, .acquire = PageAllocator.allocate, .release = PageAllocator.release };
    var store = pt_storage.Store{};
    defer store.freeDetached();
    var tracked_pts = RamCacheTrackedPts{ .store = &store, .allocator = allocator };
    const keys = [_]usize{ 0, 63, 64, 511, 512, 32767, 32768, 65536 };
    for (keys) |key| {
        try std.testing.expect(!ramCachePtIsTracked(&tracked_pts, key));
        try tracked_pts.set(key);
    }
    for (0..65538) |key| {
        const expected = std.mem.indexOfScalar(usize, &keys, key) != null;
        try std.testing.expectEqual(expected, ramCachePtIsTracked(&tracked_pts, key));
    }
}

fn ramCachePtKey(pd_slot: usize, pd: usize) !usize {
    const base = try std.math.mul(usize, pd_slot, 512);
    return std.math.add(usize, base, pd);
}

fn validateRamCacheTopology(h: anytype, space: *const UserAddressSpace, comptime shared: type, tracked_pts: *RamCacheTrackedPts) !void {
    // The caller holds all VM transactions. Borrow both tables and their
    // parallel indices; copying either array is unnecessary under exclusion.
    tracked_pts.reset();
    const pml4_pa = tablePaddr(h, &space.pml4) orelse return error.UnknownTopology;
    // CR3 may carry PCID or PWT/PCD in its low bits, but no unknown high bits.
    if ((space.cr3 & h.page_addr_mask) != pml4_pa or
        (space.cr3 & ~(h.page_addr_mask | @as(u64, 4095))) != 0)
        return error.UnknownTopology;
    const flags = h.page_present | h.page_rw | h.page_user;
    for (&space.pml4, 0..) |entry, index| {
        if ((entry & h.page_present) == 0) continue;
        if (index < 256) {
            if (findUserPdpSlotForPml4(space, index)) |slot| {
                const pa = tablePaddr(h, space.pdpPage(slot)) orelse return error.UnknownTopology;
                if (!ramCacheParentMatches(entry, pa | flags)) return error.UnknownTopology;
                continue;
            }
        }
        const expected = shared.ramCacheSharedPml4Entry(index) orelse return error.UnknownTopology;
        if (!ramCacheParentMatches(entry, expected)) return error.UnknownTopology;
    }
    var pdp_it = space.pdp_store.iterator();
    while (pdp_it.next()) |slot| {
        const descriptor = space.pdp_store.descriptorConst(slot).?;
        if (descriptor.page == null) continue;
        const pdp = descriptor.page.?;
        const pml4 = descriptor.pml4;
        if (pml4 == UserAddressSpace.no_pd_index) continue;
        if (pml4 >= 256 or findUserPdpSlotForPml4(space, pml4) != slot) return error.UnknownTopology;
        const pa = tablePaddr(h, pdp) orelse return error.UnknownTopology;
        if (!ramCacheParentMatches(space.pml4[pml4], pa | flags)) return error.UnknownTopology;
        for (pdp, 0..) |entry, index| {
            if ((entry & h.page_present) == 0) continue;
            // This runtime creates no private 1 GiB leaves. An unknown huge
            // mapping cannot be omitted merely because its metadata is absent.
            if ((entry & h.page_ps) != 0) return error.UnknownTopology;
            const expected = if (findUserPdSlotForPdp(space, pml4, index)) |pd_slot| blk: {
                const pd_pa = tablePaddr(h, space.pdPage(pd_slot)) orelse return error.UnknownTopology;
                break :blk pd_pa | flags;
            } else return error.UnknownTopology;
            if (!ramCacheParentMatches(entry, expected)) return error.UnknownTopology;
        }
    }
    var pd_it = space.pd_store.iterator();
    while (pd_it.next()) |slot| {
        const descriptor = space.pd_store.descriptorConst(slot).?;
        if (descriptor.page == null) continue;
        const pd = descriptor.page.?;
        const pml4 = descriptor.pml4;
        const pdp = descriptor.pdp;
        if (pml4 == UserAddressSpace.no_pd_index and pdp == UserAddressSpace.no_pd_index) continue;
        if (pml4 >= 256 or pdp >= 512 or findUserPdSlotForPdp(space, pml4, pdp) != slot) return error.UnknownTopology;
        const parent = findUserPdpSlotForPml4(space, pml4) orelse return error.UnknownTopology;
        const pa = tablePaddr(h, pd) orelse return error.UnknownTopology;
        if (!ramCacheParentMatches(space.pdpPage(parent).*[pdp], pa | flags)) return error.UnknownTopology;
    }
    var pt_it = space.pt_store.iterator();
    while (pt_it.next()) |slot| {
        const pml4 = space.ptPml4Index(slot).*;
        const pdp = space.ptPdpIndex(slot).*;
        const pd = space.ptPdIndex(slot).*;
        if (pml4 == UserAddressSpace.no_pd_index and pdp == UserAddressSpace.no_pd_index and pd == UserAddressSpace.no_pd_index) continue;
        if (pml4 >= 256 or pdp >= 512 or pd >= 512)
            return error.UnknownTopology;
        const parent = findUserPdSlotForPdp(space, pml4, pdp) orelse return error.UnknownTopology;
        const key = try ramCachePtKey(parent, pd);
        if (ramCachePtIsTracked(tracked_pts, key)) return error.UnknownTopology;
        try tracked_pts.set(key);
        const pa = tablePaddr(h, space.ptPage(slot)) orelse return error.UnknownTopology;
        // Every low branch belongs to this AS; supervisor tables are shared
        // exclusively through PML4[511], never grafted into user parents.
        const entry = space.pdPage(parent).*[pd];
        const expected = pa | flags;
        if (!ramCacheParentMatches(entry, expected)) return error.UnknownTopology;
    }
}

test "VM Hooks reject runtime replacement and permit serial fixture replacement" {
    // No runtime/parallel borrower exists in this host-only test. Undefined
    // callbacks are intentionally never invoked: this tests install policy.
    const previous = hooks;
    defer hooks = previous;
    hooks = null;
    var configuration: Hooks = undefined;
    configuration.four_gib = 4;
    try configureHooks(configuration, false);
    configuration.four_gib = 8;
    try std.testing.expectError(error.AlreadyInitialized, configureHooks(configuration, false));
    try std.testing.expectEqual(@as(u64, 4), hooks.?.four_gib);
    try configureHooks(configuration, true);
    try std.testing.expectEqual(@as(u64, 8), hooks.?.four_gib);
}

test "MMIO overlay RAM topology rejects hidden roots and detached tracked parents" {
    const Fake = struct {
        page_addr_mask: u64 = 0x000f_ffff_ffff_f000,
        page_present: u64 = 1,
        page_rw: u64 = 2,
        page_user: u64 = 4,
        page_ps: u64 = 128,
        page_nx: u64 = @as(u64, 1) << 63,
        kernel_pointer_paddr: *const fn (addresses.KernelVirtualAddress) ?addresses.PhysicalAddress = pointer,
        fn pointer(address: addresses.KernelVirtualAddress) ?addresses.PhysicalAddress {
            return .fromInt(address.toInt());
        }
        pub fn ramCacheSharedPml4Entry(index: usize) ?u64 {
            return if (index == 511) 0x100003 else null;
        }
        pub fn ramCacheSharedPdpEntry(pml4: usize, index: usize) ?u64 {
            return if (pml4 == 0 and index == 1) 0x200003 else null;
        }
    };
    const PageAllocator = struct {
        fn allocate(_: *anyopaque, pages: usize) ?pt_storage.MappedExtent {
            std.debug.assert(pages == 1);
            const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096) catch return null;
            return .{ .allocation = .{ .physical = .fromInt(@intFromPtr(bytes.ptr)), .pages = 1, .region_id = 83 }, .bytes = bytes };
        }
        fn release(_: *anyopaque, grant: pt_storage.PhysicalExtent) bool {
            std.debug.assert(grant.pages == 1 and grant.region_id == 83);
            const page: pt_storage.PagePointer = @ptrFromInt(grant.physical.toInt());
            const bytes: []align(4096) u8 = page;
            std.testing.allocator.free(bytes);
            return true;
        }
    };
    var dummy: u8 = 0;
    const allocator = pt_storage.Allocator{ .context = &dummy, .acquire = PageAllocator.allocate, .release = PageAllocator.release };
    const space = try std.testing.allocator.create(UserAddressSpace);
    defer std.testing.allocator.destroy(space);
    space.* = .{};
    space.cr3 = @intFromPtr(&space.pml4) | 7;
    space.pml4[511] = 0x100023;
    _ = space.pdp_store.ensurePage(0, allocator) orelse return error.OutOfMemory;
    defer space.pdp_store.freeDetached();
    defer space.pd_store.freeDetached();
    space.pdpPml4Index(0).* = 0;
    space.pml4[0] = @intFromPtr(space.pdpPage(0)) | 7;
    space.pdpPage(0).*[1] = 0;
    var tracked_pts = RamCacheTrackedPts{ .store = &space.scratch_store, .allocator = allocator };
    defer space.scratch_store.freeDetached();
    try validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts);
    space.pdpPage(0).*[1] = 0x200023;
    try std.testing.expectError(error.UnknownTopology, validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts));
    space.pdpPage(0).*[1] = 0;
    space.pml4[510] = 0x300003;
    try std.testing.expectError(error.UnknownTopology, validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts));
    space.pml4[510] = 0;
    space.pml4[511] ^= 4;
    try std.testing.expectError(error.UnknownTopology, validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts));
    space.pml4[511] ^= 4;
    space.pdpPage(0).*[2] = 0x400003;
    try std.testing.expectError(error.UnknownTopology, validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts));
    _ = space.pd_store.ensurePage(0, allocator) orelse return error.OutOfMemory;
    space.pdPml4Index(0).* = 0;
    space.pdPdpIndex(0).* = 2;
    space.pdpPage(0).*[2] = @intFromPtr(space.pdPage(0)) | 7;
    try validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts);
    space.pdpPage(0).*[2] = 0;
    try std.testing.expectError(error.UnknownTopology, validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts));
    space.pdpPage(0).*[2] = @intFromPtr(space.pdPage(0)) | 7;
    space.cr3 ^= 4096;
    try std.testing.expectError(error.UnknownTopology, validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts));
}

test "RAM cache PT index proves many parents and rejects duplicate descriptors" {
    const Fake = struct {
        page_addr_mask: u64 = 0x000f_ffff_ffff_f000,
        page_present: u64 = 1,
        page_rw: u64 = 2,
        page_user: u64 = 4,
        page_ps: u64 = 128,
        page_nx: u64 = @as(u64, 1) << 63,
        kernel_pointer_paddr: *const fn (addresses.KernelVirtualAddress) ?addresses.PhysicalAddress = pointer,
        fn pointer(address: addresses.KernelVirtualAddress) ?addresses.PhysicalAddress {
            return .fromInt(address.toInt());
        }
        pub fn ramCacheSharedPml4Entry(index: usize) ?u64 {
            return if (index == 511) 0x100003 else null;
        }
        pub fn ramCacheSharedPdpEntry(_: usize, _: usize) ?u64 {
            return null;
        }
    };
    const PageAllocator = struct {
        fn allocate(_: *anyopaque, pages: usize) ?pt_storage.MappedExtent {
            std.debug.assert(pages == 1);
            const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096) catch return null;
            return .{ .allocation = .{ .physical = .fromInt(@intFromPtr(bytes.ptr)), .pages = 1, .region_id = 83 }, .bytes = bytes };
        }
        fn release(_: *anyopaque, grant: pt_storage.PhysicalExtent) bool {
            std.debug.assert(grant.pages == 1 and grant.region_id == 83);
            const page: pt_storage.PagePointer = @ptrFromInt(grant.physical.toInt());
            const bytes: []align(4096) u8 = page;
            std.testing.allocator.free(bytes);
            return true;
        }
    };
    const space = try std.testing.allocator.create(UserAddressSpace);
    defer std.testing.allocator.destroy(space);
    space.* = .{};
    var dummy: u8 = 0;
    const allocator = pt_storage.Allocator{
        .context = &dummy,
        .acquire = PageAllocator.allocate,
        .release = PageAllocator.release,
    };
    defer space.pt_store.freeDetached();
    space.cr3 = @intFromPtr(&space.pml4);
    space.pml4[511] = 0x100003;
    _ = space.pdp_store.ensurePage(0, allocator) orelse return error.OutOfMemory;
    defer space.pdp_store.freeDetached();
    defer space.pd_store.freeDetached();
    space.pdpPml4Index(0).* = 0;
    space.pml4[0] = @intFromPtr(space.pdpPage(0)) | 7;
    for (0..2) |pd_slot| {
        _ = space.pd_store.ensurePage(pd_slot, allocator) orelse return error.OutOfMemory;
        space.pdPml4Index(pd_slot).* = 0;
        space.pdPdpIndex(pd_slot).* = @intCast(pd_slot + 2);
        space.pdpPage(0).*[pd_slot + 2] = @intFromPtr(space.pdPage(pd_slot)) | 7;
    }
    // Two full descriptor banks exercise the former descriptor-by-descriptor
    // linear search, including distinct parent PD slots.
    for (0..128) |slot| {
        const page = space.pt_store.ensurePage(slot, allocator) orelse return error.OutOfMemory;
        const descriptor = space.pt_store.descriptor(slot).?;
        const pd_slot = slot / 64;
        const pd = slot % 64;
        descriptor.pml4 = 0;
        descriptor.pdp = @intCast(pd_slot + 2);
        descriptor.pd = @intCast(pd);
        space.pdPage(pd_slot).*[pd] = @intFromPtr(page) | 7;
    }
    var tracked_pts = RamCacheTrackedPts{ .store = &space.scratch_store, .allocator = allocator };
    defer space.scratch_store.freeDetached();
    try validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts);
    try std.testing.expect(tracked_pts.isSet(try ramCachePtKey(0, 0)));
    try std.testing.expect(tracked_pts.isSet(try ramCachePtKey(1, 63)));
    try std.testing.expect(!tracked_pts.isSet(try ramCachePtKey(0, 64)));

    const duplicate_page = space.pt_store.ensurePage(128, allocator) orelse return error.OutOfMemory;
    const duplicate = space.pt_store.descriptor(128).?;
    duplicate.pml4 = 0;
    duplicate.pdp = 2;
    duplicate.pd = 0;
    try std.testing.expectError(error.UnknownTopology, validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts));
    // Even an impossible duplicated page pointer must not make two metadata
    // owners of one PDE appear safe. Restore the unique allocator-owned page
    // before the fixture releases its storage.
    duplicate.page = space.pt_store.descriptor(0).?.page;
    try std.testing.expectError(error.UnknownTopology, validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts));
    duplicate.page = duplicate_page;
    duplicate.pml4 = UserAddressSpace.no_pd_index;
    duplicate.pdp = UserAddressSpace.no_pd_index;
    duplicate.pd = UserAddressSpace.no_pd_index;

    space.pdPage(0).*[0] ^= 2; // User PDE must retain RW; supervisor PDEs are forbidden.
    try std.testing.expectError(error.UnknownTopology, validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts));
    space.pdPage(0).*[0] ^= 2; // User PDE must retain RW; supervisor PDEs are forbidden.
    try validateRamCacheTopology(Fake{}, space, Fake, &tracked_pts);
}

fn sortedPagesIntersect(pages: []const u64, base_pa: u64, bytes: u64) bool {
    return @import("ram_cache_paging.zig").pagesIntersect(pages, base_pa, bytes);
}

/// Include supervisor copies as well as user mappings. Shared kernel PD/PT
/// pointers are enumerated by platform; private PTs must be enumerated here
/// because a process can have copied them before an earlier WC transition.
/// The caller holds outer state and all VM locks through commit or rollback.
pub fn visitRamCacheProcessAliases(pages: []const u64, context: anytype, comptime visit: anytype) !void {
    const h = hooks orelse return error.Invalid;
    const paging = @import("ram_cache_paging.zig");
    const platform = @import("../arch/x86_64/platform.zig");
    try paging.validatePages(pages, h.physical_map_limit);
    for (0..h.user_spaces.capacity()) |space_index| {
        const space = h.user_spaces.get(space_index) orelse continue;
        if (space.cr3 == 0) continue;
        var tracked_pts = RamCacheTrackedPts{ .store = &space.scratch_store, .allocator = h.pt_allocator orelse return error.OutOfMemory };
        defer space.scratch_store.freeDetached();
        try validateRamCacheTopology(h, space, platform, &tracked_pts);
        var pd_it = space.pd_store.iterator();
        while (pd_it.next()) |pd_slot| {
            const descriptor = space.pd_store.descriptorConst(pd_slot).?;
            if (descriptor.page == null) continue;
            const pd = descriptor.page.?;
            const pml4 = descriptor.pml4;
            const pdp = descriptor.pdp;
            if (pml4 >= 256 or pdp >= 512) continue;
            for (pd, 0..) |*directory, index| {
                const entry = @atomicLoad(u64, directory, .acquire);
                if ((entry & h.page_present) == 0) continue;
                // User trees contain only tracked 4 KiB user leaves. A
                // supervisor or large entry here is corruption, not a seed.
                if (entry & h.page_ps != 0 or
                    !ramCachePtIsTracked(&tracked_pts, try ramCachePtKey(pd_slot, index)))
                    return error.UnknownTopology;
            }
        }
        var pt_it = space.pt_store.iterator();
        while (pt_it.next()) |slot| {
            const pml4 = space.ptPml4Index(slot).*;
            const pdp = space.ptPdpIndex(slot).*;
            const pd = space.ptPdIndex(slot).*;
            if (pml4 >= 256 or pdp >= 512 or pd >= 512) continue;
            for (space.ptPage(slot)) |*leaf| {
                const entry = @atomicLoad(u64, leaf, .acquire);
                if ((entry & h.page_present) == 0) continue;
                const pa = entry & h.page_addr_mask;
                if (!paging.pagesIntersect(pages, pa, 4096)) continue;
                if ((entry & (1 << 8)) != 0) return error.GlobalAlias;
                if (entry & h.page_user == 0) return error.UnknownTopology;
                if (entry & h.page_nx == 0) return error.ExecutableAlias;
                try visit(context, paging.AliasSource{ .leaf = leaf });
            }
        }
    }
}

pub const RamCacheKernelSplit = struct {
    split: @import("ram_cache_paging.zig").IdentitySplit,
    slot: usize,
    published: bool = false,
    released: bool = false,

    pub fn publish(self: *RamCacheKernelSplit) void {
        std.debug.assert(!self.published and !self.released);
        const descriptor = kernel_ram_split_store.descriptor(self.slot).?;
        std.debug.assert((descriptor.pd >= 512 and descriptor.pd < 1024) or descriptor.pd == 1536);
        descriptor.pd -= 512;
        self.split.publishEquivalent();
        self.published = true;
    }

    pub fn releaseReservation(self: *RamCacheKernelSplit) void {
        if (self.published or self.released) return;
        kernel_ram_split_store.releasePageDetached(self.slot);
        kernel_ram_split_store.reclaimEmptyBanks();
        self.released = true;
    }
};

/// Direct-map split pages persist with the kernel root. Their ownership is
/// recorded in the same dynamic PT store used by the cache transaction.
pub fn reserveRamCacheKernelSplit(source: @import("ram_cache_paging.zig").AliasSource) !RamCacheKernelSplit {
    const h = hooks orelse return error.Invalid;
    const identity = switch (source) {
        .identity => |value| value,
        else => return error.Invalid,
    };
    const paging = @import("ram_cache_paging.zig");
    const direct = @import("../arch/x86_64/direct_map.zig");
    const va = identity.alias_va orelse return error.Invalid;
    if (identity.base >= direct.aperture_bytes or va != direct.base + identity.base) return error.Invalid;
    try paging.IdentitySplit.validateSourceSized(@atomicLoad(u64, identity.directory, .acquire), identity.base, identity.size);
    const allocator = h.pt_allocator orelse return error.Invalid;
    const pml4: u16 = @intCast((va >> 39) & 511);
    const pdp: u16 = @intCast((va >> 30) & 511);
    // pd=1024 owns a 1-GiB split's PD; pd=0..511 owns a 2-MiB split's PT.
    // Adding 512 denotes an unpublished reservation for either level. This
    // separates the two hardware levels without a second PT store.
    const pd: u16 = if (identity.size == .huge) 1024 else @intCast((va >> 21) & 511);
    const slot = kernel_ram_split_store.firstFreeSlot() orelse return error.OutOfMemory;
    var split_it = kernel_ram_split_store.iterator();
    while (split_it.next()) |index| {
        const entry = kernel_ram_split_store.descriptor(index).?;
        if (entry.pml4 == pml4 and entry.pdp == pdp and
            (entry.pd == pd or entry.pd == pd + 512)) return error.Invalid;
    }
    const page = kernel_ram_split_store.ensurePage(slot, allocator) orelse return error.OutOfMemory;
    errdefer {
        kernel_ram_split_store.releasePageDetached(slot);
        kernel_ram_split_store.reclaimEmptyBanks();
    }
    const pa = tablePaddr(h, page) orelse return error.Invalid;
    if (pa >= h.physical_map_limit) return error.Invalid;
    const split = try paging.IdentitySplit.prepareSized(identity.directory, identity.base, identity.size, page, pa);
    const entry = kernel_ram_split_store.descriptor(slot).?;
    entry.pml4 = pml4;
    entry.pdp = pdp;
    entry.pd = pd + 512;
    return .{ .split = split, .slot = slot };
}

pub fn ramCacheKernelAliasSplitOwns(va: u64, size: @import("ram_cache_paging.zig").LeafSize, pa: u64) bool {
    const h = hooks orelse return false;
    if (size == .page or va & (size.bytes() - 1) != 0 or pa & 4095 != 0) return false;
    const pml4: u16 = @intCast((va >> 39) & 511);
    const pdp: u16 = @intCast((va >> 30) & 511);
    const pd: u16 = if (size == .huge) 1024 else @intCast((va >> 21) & 511);
    var split_it = kernel_ram_split_store.iterator();
    while (split_it.next()) |slot| {
        const entry = kernel_ram_split_store.descriptorConst(slot).?;
        if (entry.pml4 != pml4 or entry.pdp != pdp or entry.pd != pd) continue;
        const page = entry.page orelse return false;
        return (tablePaddr(h, page) orelse return false) == pa;
    }
    return false;
}

pub fn releaseRamCacheKernelSplitsForTest() void {
    if (!@import("builtin").is_test) @compileError("synthetic detached tables only");
    if (kernel_ram_split_store.chunks == null) return;
    kernel_ram_split_store.freeDetached();
}

/// Capture every present user alias, not merely the requesting VMO's VA.
/// Scratch may be partially filled on failure, but live PTEs are untouched.
/// Large user leaves are rejected before commit rather than partially split
/// after another process has already lost its old mapping.
pub fn prepareRamCacheUserAliases(pages: []const u64, scratch: []RamCacheUserAlias) RamCacheAliasError!usize {
    const h = hooks orelse return error.Invalid;
    if (pages.len == 0) return error.Invalid;
    var previous: u64 = 0;
    for (pages) |pa| {
        if (pa == 0 or pa & 4095 != 0 or pa <= previous or pa >= h.physical_map_limit)
            return error.Invalid;
        previous = pa;
    }
    var count: usize = 0;
    for (0..h.user_spaces.capacity()) |space_index| {
        const space = h.user_spaces.get(space_index) orelse continue;
        if (space.cr3 == 0) continue;
        const principal = kernel.processPrincipalFromIndex(space_index) orelse return error.Invalid;
        const large_flags = h.page_present | h.page_user | h.page_ps;
        var pdp_it = space.pdp_store.iterator();
        while (pdp_it.next()) |pdp_slot| {
            const descriptor = space.pdp_store.descriptorConst(pdp_slot).?;
            if (descriptor.page == null) continue;
            const pdp = descriptor.page.?;
            const pml4 = descriptor.pml4;
            if (pml4 >= 256) continue;
            for (pdp) |entry| {
                if ((entry & large_flags) == large_flags and
                    sortedPagesIntersect(pages, entry & h.page_addr_mask & ~@as(u64, 0x3fff_ffff), 1 << 30))
                    return error.LargeAlias;
            }
        }
        var pd_it = space.pd_store.iterator();
        while (pd_it.next()) |pd_slot| {
            const descriptor = space.pd_store.descriptorConst(pd_slot).?;
            if (descriptor.page == null) continue;
            const pd = descriptor.page.?;
            const pml4 = descriptor.pml4;
            if (pml4 >= 256) continue;
            for (pd) |entry| {
                if ((entry & large_flags) == large_flags and
                    sortedPagesIntersect(pages, entry & h.page_addr_mask & ~@as(u64, 0x1f_ffff), 1 << 21))
                    return error.LargeAlias;
            }
        }
        var pt_it = space.pt_store.iterator();
        while (pt_it.next()) |slot| {
            const pml4 = space.ptPml4Index(slot).*;
            const pdp = space.ptPdpIndex(slot).*;
            const pd = space.ptPdIndex(slot).*;
            if (pml4 >= 256 or pdp >= 512 or pd >= 512) continue;
            for (space.ptPage(slot), 0..) |*leaf, pt| {
                const entry = @atomicLoad(u64, leaf, .acquire);
                if ((entry & (h.page_present | h.page_user)) != (h.page_present | h.page_user)) continue;
                if (!sortedPagesIntersect(pages, entry & h.page_addr_mask, 4096)) continue;
                if ((entry & h.page_nx) == 0) return error.ExecutableAlias;
                if ((entry & (1 << 8)) != 0) return error.GlobalAlias;
                if (count == scratch.len) return error.NoSpace;
                scratch[count] = .{
                    .principal = principal,
                    .va = (@as(u64, pml4) << 39) | (@as(u64, pdp) << 30) |
                        (@as(u64, pd) << 21) | (@as(u64, @intCast(pt)) << 12),
                    .leaf = leaf,
                    .original = entry,
                };
                count += 1;
            }
        }
    }
    return count;
}

/// No allocation or PT retirement occurs here. VM locks exclude software
/// changes, not hardware A/D updates: atomically capture the live entry when
/// removing Present. The caller must next invalidate every old alias on all
/// CPUs and drain caches before invoking publication.
pub fn withdrawRamCacheUserAliases(aliases: []RamCacheUserAlias) void {
    const h = hooks orelse unreachable;
    const accessed_dirty: u64 = (1 << 5) | (1 << 6);
    for (aliases) |*alias| {
        const entry = @atomicRmw(u64, alias.leaf, .And, ~h.page_present, .seq_cst);
        std.debug.assert((entry & ~accessed_dirty) == (alias.original & ~accessed_dirty));
        alias.original = entry;
    }
}

pub fn flushRamCacheUserAliases(aliases: []const RamCacheUserAlias) void {
    const h = hooks orelse unreachable;
    // Preparation emits all aliases of an AS consecutively. Keep this API
    // robust to other callers too: one request per distinct owner, even if
    // aliases are unsorted. No page/table is returned or republished here.
    for (aliases, 0..) |alias, index| {
        if (index != 0 and aliases[index - 1].principal == alias.principal) continue;
        var seen = false;
        for (aliases[0..index]) |previous| if (previous.principal == alias.principal) {
            seen = true;
            break;
        };
        if (seen) continue;
        var first = alias.va;
        var end = alias.va + 4096;
        for (aliases[index + 1 ..]) |other| {
            if (other.principal != alias.principal) continue;
            first = @min(first, other.va);
            end = @max(end, other.va + 4096);
        }
        h.flush_user_tlb_for_principal_range(alias.principal, first, @intCast(end - first));
    }
}

/// Called only after the external all-CPU old-type barrier. Preserve late
/// hardware A/D updates to withdrawn leaves and every permission/software
/// bit; replacing only PAT/PWT/PCD avoids silently changing mapping rights.
pub fn publishRamCacheUserAliases(aliases: []const RamCacheUserAlias, pat_slot: u3) void {
    const h = hooks orelse unreachable;
    const accessed_dirty: u64 = (1 << 5) | (1 << 6);
    const cache_mask: u64 = (1 << 3) | (1 << 4) | (1 << 7);
    const cache_bits = (@as(u64, pat_slot & 3) << 3) | (@as(u64, pat_slot >> 2) << 7);
    for (aliases) |alias| {
        const late = @atomicLoad(u64, alias.leaf, .acquire);
        std.debug.assert((late & ~(accessed_dirty | h.page_present)) ==
            (alias.original & ~(accessed_dirty | h.page_present)) and (late & h.page_present) == 0);
        const entry = (alias.original | (late & accessed_dirty)) & ~cache_mask;
        @atomicStore(u64, alias.leaf, entry | cache_bits, .release);
    }
}

pub const MappedUserPageVisitor = *const fn (context: *anyopaque, page: MappedUserPage) bool;

pub fn forEachUserMappedPage(principal: kernel.PrincipalId, context: *anyopaque, visitor: MappedUserPageVisitor) bool {
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    var it = space.pt_store.iterator();
    while (it.next()) |slot| {
        const pd_index_meta = space.ptPdIndex(slot).*;
        const pml4_index_meta = space.ptPml4Index(slot).*;
        const pdp_index_meta = space.ptPdpIndex(slot).*;
        if (pml4_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pdp_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pd_index_meta == UserAddressSpace.no_pd_index) continue;
        const pml4_index: usize = @intCast(pml4_index_meta);
        const pdp_index: usize = @intCast(pdp_index_meta);
        const pd_index: usize = @intCast(pd_index_meta);
        const pt_page: *const [512]u64 = space.ptPage(slot);
        var pt_index: usize = 0;
        while (pt_index < h.page_entries) : (pt_index += 1) {
            const entry = pt_page[pt_index];
            if ((entry & h.page_present) == 0) continue;
            if ((entry & h.page_user) == 0) continue;
            const paddr = ptePaddr(entry);
            if (paddr == 0) continue;
            const va = (@as(u64, @intCast(pml4_index)) << 39) |
                (@as(u64, @intCast(pdp_index)) << 30) |
                (@as(u64, @intCast(pd_index)) << 21) |
                (@as(u64, @intCast(pt_index)) << 12);
            if (!visitor(context, .{
                .va = va,
                .paddr = paddr,
                .writable = (entry & h.page_rw) != 0,
            })) return false;
        }
    }
    return true;
}

pub fn collectUserMappedPages(principal: kernel.PrincipalId, out_pages: []MappedUserPage) usize {
    const h = hooks orelse return 0;
    const space = getUserSpace(principal) orelse return 0;
    var count: usize = 0;
    var it = space.pt_store.iterator();
    while (it.next()) |slot| {
        const pd_index_meta = space.ptPdIndex(slot).*;
        const pml4_index_meta = space.ptPml4Index(slot).*;
        const pdp_index_meta = space.ptPdpIndex(slot).*;
        if (pml4_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pdp_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pd_index_meta == UserAddressSpace.no_pd_index) continue;
        const pml4_index: usize = @intCast(pml4_index_meta);
        const pdp_index: usize = @intCast(pdp_index_meta);
        const pd_index: usize = @intCast(pd_index_meta);
        const pt_page: *const [512]u64 = space.ptPage(slot);
        var pt_index: usize = 0;
        while (pt_index < h.page_entries) : (pt_index += 1) {
            const entry = pt_page[pt_index];
            if ((entry & h.page_present) == 0) continue;
            if ((entry & h.page_user) == 0) continue;
            const paddr = ptePaddr(entry);
            if (paddr == 0) continue;
            var duplicate = false;
            for (out_pages[0..count]) |seen| {
                if (seen.paddr == paddr) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            if (count >= out_pages.len) return count;
            const va = (@as(u64, @intCast(pml4_index)) << 39) |
                (@as(u64, @intCast(pdp_index)) << 30) |
                (@as(u64, @intCast(pd_index)) << 21) |
                (@as(u64, @intCast(pt_index)) << 12);
            out_pages[count] = .{
                .va = va,
                .paddr = paddr,
                .writable = (entry & h.page_rw) != 0,
            };
            count += 1;
        }
    }
    return count;
}

/// Bootstrap builder: caller owns an unpublished/quiescent address space.
/// Published address spaces require the caller's address-space exclusion.
pub fn buildUserAddressSpace(principal: kernel.PrincipalId, user_page_paddr: u64, user_stack_paddr: u64) bool {
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    resetUserPageTablesPreserveReservations(space, principal) catch return false;
    resetUserReservations(space);

    const user_pml4_pa: u64 = tablePaddr(h, &space.pml4) orelse return false;
    if (user_pml4_pa >= h.physical_map_limit) return false;
    if (user_page_paddr >= h.four_gib or user_stack_paddr >= h.four_gib) return false;

    const pml4_index: usize = @intCast((h.user_va >> 39) & 0x1FF);
    const pdp_index: usize = @intCast((h.user_va >> 30) & 0x1FF);
    const pd_index_base: usize = @intCast((h.user_va >> 21) & 0x1FF);
    const user_pt_index: usize = @intCast((h.user_va >> 12) & 0x1FF);
    const stack_pt_index: usize = @intCast((h.user_stack_page_va >> 12) & 0x1FF);
    const stack_pml4_index: usize = @intCast((h.user_stack_page_va >> 39) & 0x1FF);
    const stack_pdp_index: usize = @intCast((h.user_stack_page_va >> 30) & 0x1FF);
    const stack_pd_index: usize = @intCast((h.user_stack_page_va >> 21) & 0x1FF);

    const user_slot = ensureUserPtSlotForPd(space, pml4_index, pdp_index, pd_index_base) orelse return false;
    const stack_slot = ensureUserPtSlotForPd(space, stack_pml4_index, stack_pdp_index, stack_pd_index) orelse return false;
    const user_pt_page: *[512]u64 = space.ptPage(user_slot);
    const stack_pt_page: *[512]u64 = space.ptPage(stack_slot);
    if (!reserveUserMapping(principal, h.user_va, 1, .bootstrap, true)) return false;
    if (!reserveUserMapping(principal, h.user_stack_page_va, 1, .bootstrap, true)) return false;
    user_pt_page[user_pt_index] = user_page_paddr | h.page_present | h.page_user;
    stack_pt_page[stack_pt_index] = user_stack_paddr | h.page_present | h.page_rw | h.page_user | h.page_nx;
    space.cr3 = user_pml4_pa;
    return true;
}

/// Staging builder with the same unpublished/quiescent ownership as above.
pub fn buildEmptyUserAddressSpace(principal: kernel.PrincipalId) error{ OutOfFreePages, InvalidState }!void {
    const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.pt_prepare, stage_sample);
    const space = getUserSpace(principal) orelse return error.InvalidState;
    try resetUserPageTablesPreserveReservations(space, principal);
    resetUserReservations(space);
}

fn processIndex(principal: kernel.PrincipalId) ?usize {
    return kernel.processIndexFromPrincipal(principal);
}

test "lock ordering AS shared actual recursion keeps first link and last unlink" {
    if (!lock_diagnostics.enabled) return;
    inline for (.{ lock_diagnostics.order.Class.address_space, lock_diagnostics.order.Class.shared_vm }) |class| {
        var state: AddressSpaceLockState = .{};
        const key: lock_diagnostics.order.Key = .{ .class = class, .order = 3 };
        lockState(&state, key);
        lockState(&state, key);
        try std.testing.expectEqual(@as(u32, 2), state.depth);
        try std.testing.expect(lock_diagnostics.testCpu().head == &state.diagnostic_record);
        try std.testing.expect(state.diagnostic_record.next == null);
        const irq = lock_diagnostics.enterInterrupt(false);
        try std.testing.expectError(error.WrongContext, lock_diagnostics.testCpu().recursive(&state.diagnostic_record));
        irq.end();
        unlockState(&state);
        try std.testing.expect(state.diagnostic_record.linked);
        unlockState(&state);
        try std.testing.expect(!state.diagnostic_record.linked);
        lock_diagnostics.assertRootEmpty();
    }
}
