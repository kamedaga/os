const std = @import("std");
const address_space = @import("address_space.zig");
const kernel = @import("../kernel.zig");
const scheduler = @import("../scheduler.zig").connection;

pub const UserAddressSpace = address_space.UserAddressSpace;

pub const Hooks = struct {
    user_spaces: []UserAddressSpace,
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
    kernel_pointer_paddr: *const fn (addr: usize) ?u64,
    seed_user_pml4_with_kernel: *const fn ([]u64) void,
    seed_user_pdp_with_kernel_identity: *const fn ([]u64) void,
    seed_user_pd_with_kernel_identity: *const fn ([]u64) void,
};

var hooks: ?Hooks = null;

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
    asm volatile ("sti; pause; cli" ::: .{ .memory = true });
}

fn lockState(state: *AddressSpaceLockState) void {
    const cpu = scheduler.currentCpu();
    if (@atomicLoad(u8, &state.value, .acquire) != 0 and state.owner_cpu == cpu) {
        state.depth +%= 1;
        return;
    }
    while (true) {
        if (@cmpxchgWeak(u8, &state.value, 0, 1, .acquire, .monotonic) == null) {
            state.owner_cpu = cpu;
            state.depth = 1;
            return;
        }
        while (@atomicLoad(u8, &state.value, .monotonic) != 0) {
            const restore_interrupts = interruptsEnabled();
            waitWithInterruptWindow();
            if (restore_interrupts) asm volatile ("sti" ::: .{ .memory = true });
        }
    }
}

fn unlockState(state: *AddressSpaceLockState) void {
    if (state.depth > 1) {
        state.depth -= 1;
        return;
    }
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
    const space = getUserSpace(principal) orelse return false;
    lockState(&space.lock_state);
    return true;
}

pub fn unlockAddressSpace(principal: kernel.PrincipalId) void {
    const space = getUserSpace(principal) orelse return;
    unlockState(&space.lock_state);
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
    for (h.user_spaces) |*space| lockState(&space.lock_state);
}

pub fn unlockAllAddressSpaces() void {
    const h = hooks orelse return;
    var index = h.user_spaces.len;
    while (index != 0) {
        index -= 1;
        unlockState(&h.user_spaces[index].lock_state);
    }
}

pub fn lockSharedVmObjects() void {
    lockState(&shared_vm_object_lock);
}

pub fn unlockSharedVmObjects() void {
    unlockState(&shared_vm_object_lock);
}

pub fn lockVmTransaction(principal: kernel.PrincipalId) bool {
    if (!lockAddressSpace(principal)) return false;
    lockSharedVmObjects();
    return true;
}

pub fn unlockVmTransaction(principal: kernel.PrincipalId) void {
    unlockSharedVmObjects();
    unlockAddressSpace(principal);
}

pub fn lockVmTransactionPair(first: kernel.PrincipalId, second: kernel.PrincipalId) bool {
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
    return end;
}

pub fn init(new_hooks: Hooks) void {
    hooks = new_hooks;
}

pub fn updateUserSpaces(user_spaces: []UserAddressSpace) void {
    if (hooks) |*h| h.user_spaces = user_spaces;
}

pub fn getUserSpace(principal: kernel.PrincipalId) ?*UserAddressSpace {
    const h = hooks orelse return null;
    const idx = processIndex(principal) orelse return null;
    if (idx >= h.user_spaces.len) return null;
    return &h.user_spaces[idx];
}

pub fn clearUserAddressSpace(principal: kernel.PrincipalId) void {
    if (!lockAddressSpace(principal)) return;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return;
    const space = getUserSpace(principal) orelse return;

    // A process slot reuses both its page-table storage and its PCID.  Make
    // the old user tree unreachable before flushing that PCID on every CPU;
    // otherwise a later occupant can execute a cached translation after the
    // old VMO page has already been returned to the physical free list.
    if (space.cr3 != 0) {
        // Keep the kernel half present: the shootdown implementation briefly
        // loads this CR3 while handling an IPI in kernel mode.
        @memset(space.pml4[0..256], 0);
        h.flush_user_tlb_for_principal_range(
            principal,
            h.user_low_va,
            @intCast(h.user_top_va - h.user_low_va),
        );
    }
    resetUserAddressSpaceStorage(space);
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

fn userPageIndexForVa(h: Hooks, va: u64) ?UserPageIndex {
    return userPageIndexForVaWithLowPageZero(h, va, false);
}

fn userPageIndexForVaWithLowPageZero(h: Hooks, va: u64, allow_low_page_zero: bool) ?UserPageIndex {
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
    const entry = space.pt_pages[slot][index.pt];
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
    const h = hooks orelse return null;
    const space = getUserSpace(principal) orelse return null;
    const index = userPageIndexForVa(h, va) orelse return null;
    const slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return null;
    const entry = space.pt_pages[slot][index.pt];
    if ((entry & h.page_present) == 0 or (entry & h.page_user) == 0) return null;
    const paddr = entry & h.page_addr_mask;
    if (paddr == 0) return null;
    if (write_access and (entry & h.page_rw) == 0) return null;
    if (instruction_fetch and (entry & h.page_nx) != 0) return null;
    return paddr;
}

pub fn resetUserReservations(space: *UserAddressSpace) void {
    var index: usize = 0;
    while (index < UserAddressSpace.max_reservations) : (index += 1) {
        space.reservations[index].base_va = 0;
        space.reservations[index].page_count = 0;
        space.reservations[index].generation = 0;
        space.reservations[index].kind = .none;
        space.reservations[index].writable = false;
        space.reservations[index].active = false;
    }
    space.reservation_generation = 0;
    space.next_dynamic_map_page = 0;
}

pub fn resetUserAddressSpaceStorage(space: *UserAddressSpace) void {
    @memset(space.pml4[0..], 0);
    var pdp_slot_init: usize = 0;
    while (pdp_slot_init < UserAddressSpace.max_dynamic_pdp_pages) : (pdp_slot_init += 1) {
        space.pdp_page_pml4_index[pdp_slot_init] = UserAddressSpace.no_pd_index;
        @memset(space.pdp_pages[pdp_slot_init][0..], 0);
    }
    var pd_slot_init: usize = 0;
    while (pd_slot_init < UserAddressSpace.max_dynamic_pd_pages) : (pd_slot_init += 1) {
        space.pd_page_pml4_index[pd_slot_init] = UserAddressSpace.no_pd_index;
        space.pd_page_pdp_index[pd_slot_init] = UserAddressSpace.no_pd_index;
        @memset(space.pd_pages[pd_slot_init][0..], 0);
    }
    var pt_slot_init: usize = 0;
    while (pt_slot_init < UserAddressSpace.max_dynamic_pt_pages) : (pt_slot_init += 1) {
        space.pt_page_pml4_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        space.pt_page_pdp_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        space.pt_page_pd_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        @memset(space.pt_pages[pt_slot_init][0..], 0);
    }
    space.pdp_page_used_len = 0;
    space.pd_page_used_len = 0;
    space.pt_page_used_len = 0;
    space.cr3 = 0;
    resetUserReservations(space);
}

fn resetUserPageTablesPreserveReservations(space: *UserAddressSpace) bool {
    const h = hooks orelse return false;
    // Validate every fallible physical-address dependency before destroying
    // the live tables.  Exec must be able to return an error with its old
    // address space intact.
    const user_pml4_pa: u64 = h.kernel_pointer_paddr(@intFromPtr(&space.pml4)) orelse return false;
    if (user_pml4_pa >= h.four_gib) return false;
    const first_pdp_pa: u64 = h.kernel_pointer_paddr(@intFromPtr(&space.pdp_pages[0])) orelse return false;
    if (first_pdp_pa >= h.physical_map_limit) return false;

    @memset(space.pml4[0..], 0);
    h.seed_user_pml4_with_kernel(space.pml4[0..]);
    var pdp_slot_init: usize = 0;
    while (pdp_slot_init < UserAddressSpace.max_dynamic_pdp_pages) : (pdp_slot_init += 1) {
        space.pdp_page_pml4_index[pdp_slot_init] = UserAddressSpace.no_pd_index;
        @memset(space.pdp_pages[pdp_slot_init][0..], 0);
    }
    var pd_slot_init: usize = 0;
    while (pd_slot_init < UserAddressSpace.max_dynamic_pd_pages) : (pd_slot_init += 1) {
        space.pd_page_pml4_index[pd_slot_init] = UserAddressSpace.no_pd_index;
        space.pd_page_pdp_index[pd_slot_init] = UserAddressSpace.no_pd_index;
        @memset(space.pd_pages[pd_slot_init][0..], 0);
    }
    var pt_slot_init: usize = 0;
    while (pt_slot_init < UserAddressSpace.max_dynamic_pt_pages) : (pt_slot_init += 1) {
        space.pt_page_pml4_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        space.pt_page_pdp_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        space.pt_page_pd_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        @memset(space.pt_pages[pt_slot_init][0..], 0);
    }
    space.pdp_page_used_len = 0;
    space.pd_page_used_len = 0;
    space.pt_page_used_len = 0;
    _ = ensureUserPdpSlotForPml4(space, 0) orelse return false;
    space.cr3 = user_pml4_pa;
    return true;
}

pub fn cloneAddressSpaceMetadataForFork(from: kernel.PrincipalId, to: kernel.PrincipalId) bool {
    if (!lockAddressSpacePair(from, to)) return false;
    defer unlockAddressSpacePair(from, to);
    const source = getUserSpace(from) orelse return false;
    const dest = getUserSpace(to) orelse return false;
    if (!resetUserPageTablesPreserveReservations(dest)) return false;
    dest.reservations = source.reservations;
    dest.reservation_generation = source.reservation_generation;
    dest.next_dynamic_map_page = source.next_dynamic_map_page;
    return true;
}

pub fn presentUserPagePaddr(principal: kernel.PrincipalId, va: u64) ?u64 {
    if (!lockAddressSpace(principal)) return null;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return null;
    const space = getUserSpace(principal) orelse return null;
    if ((va & 0xFFF) != 0) return null;
    const index = userPageIndexForVa(h, va) orelse return null;
    const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return null;
    const pt_page: *const [512]u64 = &space.pt_pages[pt_slot];
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
            const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
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
    if (overflow != 0) return null;
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

fn tryMergeUserReservation(
    space: *UserAddressSpace,
    base_va: u64,
    page_count: u64,
    kind: UserAddressSpace.ReservationKind,
    writable: bool,
) bool {
    const new_start_page = base_va >> 12;
    const new_end_page = reservationEndPage(base_va, page_count) orelse return false;
    var merge_index: ?usize = null;

    var index: usize = 0;
    while (index < UserAddressSpace.max_reservations) : (index += 1) {
        const reservation = &space.reservations[index];
        if (!reservation.active or reservation.kind != kind or reservation.writable != writable) continue;
        const record_start_page = reservationStartPage(reservation);
        const record_end_page = record_start_page + reservation.page_count;
        if (record_end_page == new_start_page or new_end_page == record_start_page) {
            merge_index = index;
            break;
        }
    }

    const target_index = merge_index orelse return false;
    var target = &space.reservations[target_index];
    var merged_start_page = @min(new_start_page, reservationStartPage(target));
    var merged_end_page = @max(new_end_page, reservationStartPage(target) + target.page_count);

    while (true) {
        const merged_count = merged_end_page - merged_start_page;
        if (merged_count == 0 or merged_count > @import("std").math.maxInt(u32)) return false;

        target.base_va = merged_start_page << 12;
        target.page_count = @intCast(merged_count);

        var absorbed = false;
        index = 0;
        while (index < UserAddressSpace.max_reservations) : (index += 1) {
            if (index == target_index) continue;
            const candidate = &space.reservations[index];
            if (!candidate.active or candidate.kind != kind or candidate.writable != writable) continue;
            const candidate_start_page = reservationStartPage(candidate);
            const candidate_end_page = candidate_start_page + candidate.page_count;
            if (candidate_end_page != merged_start_page and merged_end_page != candidate_start_page) continue;

            merged_start_page = @min(merged_start_page, candidate_start_page);
            merged_end_page = @max(merged_end_page, candidate_end_page);
            candidate.* = .{};
            absorbed = true;
            break;
        }
        if (!absorbed) return true;
    }
}

pub fn reserveUserMapping(
    principal: kernel.PrincipalId,
    base_va: u64,
    page_count: u64,
    kind: UserAddressSpace.ReservationKind,
    writable: bool,
) bool {
    if (kind == .none) return false;
    if (page_count > @import("std").math.maxInt(u32)) return false;
    const space = getUserSpace(principal) orelse return false;
    _ = reservationEndPage(base_va, page_count) orelse return false;
    for (&space.reservations) |*reservation| {
        if (!reservation.active) continue;
        if (!reservationOverlaps(base_va, page_count, reservation.base_va, reservation.page_count)) continue;
        return false;
    }
    if (tryMergeUserReservation(space, base_va, page_count, kind, writable)) return true;
    for (&space.reservations) |*reservation| {
        if (reservation.active) continue;
        space.reservation_generation +%= 1;
        reservation.* = .{
            .base_va = base_va,
            .page_count = @intCast(page_count),
            .generation = space.reservation_generation,
            .kind = kind,
            .writable = writable,
            .active = true,
        };
        return true;
    }
    return false;
}

fn releaseReservationRecord(
    space: *UserAddressSpace,
    index: usize,
    release_start_page: u64,
    release_end_page: u64,
) bool {
    const reservation = &space.reservations[index];
    const record_start_page = reservation.base_va >> 12;
    const record_end_page = record_start_page + reservation.page_count;
    if (release_start_page <= record_start_page and release_end_page >= record_end_page) {
        reservation.* = .{};
        return true;
    }
    if (release_start_page <= record_start_page) {
        reservation.base_va = release_end_page << 12;
        reservation.page_count = @intCast(record_end_page - release_end_page);
        return true;
    }
    if (release_end_page >= record_end_page) {
        reservation.page_count = @intCast(release_start_page - record_start_page);
        return true;
    }

    const right_count = record_end_page - release_end_page;
    for (&space.reservations) |*free_record| {
        if (free_record.active) continue;
        space.reservation_generation +%= 1;
        free_record.* = .{
            .base_va = release_end_page << 12,
            .page_count = @intCast(right_count),
            .generation = space.reservation_generation,
            .kind = reservation.kind,
            .writable = reservation.writable,
            .active = true,
        };
        reservation.page_count = @intCast(release_start_page - record_start_page);
        return true;
    }
    return false;
}

pub fn releaseUserMapping(principal: kernel.PrincipalId, base_va: u64, page_count: u64) bool {
    const space = getUserSpace(principal) orelse return false;
    const release_end_page = reservationEndPage(base_va, page_count) orelse return false;
    const release_start_page = base_va >> 12;
    var changed = false;
    var index: usize = 0;
    while (index < UserAddressSpace.max_reservations) : (index += 1) {
        const reservation = &space.reservations[index];
        if (!reservation.active) continue;
        const record_start_page = reservation.base_va >> 12;
        const record_end_page = record_start_page + reservation.page_count;
        if (record_start_page >= release_end_page or release_start_page >= record_end_page) continue;
        const overlap_start = if (release_start_page > record_start_page) release_start_page else record_start_page;
        const overlap_end = if (release_end_page < record_end_page) release_end_page else record_end_page;
        if (!releaseReservationRecord(space, index, overlap_start, overlap_end)) return false;
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
    for (&space.reservations) |*reservation| {
        if (!reservation.active) continue;
        if (!reservationOverlaps(base_va, page_count, reservation.base_va, reservation.page_count)) continue;
        const reservation_end_page = reservationStartPage(reservation) + reservation.page_count;
        if (reservation_end_page > skip_to_page) skip_to_page = reservation_end_page;
    }
    return if (skip_to_page == 0) null else skip_to_page;
}

fn rangeHasPresentUserMapping(principal: kernel.PrincipalId, base_va: u64, page_count: u64) bool {
    var page_index: u64 = 0;
    while (page_index < page_count) : (page_index += 1) {
        const va = base_va + page_index * 4096;
        if (lookupUserMappedPaddrForVa(principal, va) != null) return true;
    }
    return false;
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
    const slot_limit = ptSlotScanLimit(space);
    var slot: usize = 0;
    while (slot < slot_limit) : (slot += 1) {
        if (space.pt_page_pml4_index[slot] == UserAddressSpace.no_pd_index) continue;
        if (space.pt_page_pdp_index[slot] == UserAddressSpace.no_pd_index) continue;
        if (space.pt_page_pd_index[slot] == UserAddressSpace.no_pd_index) continue;
        if (space.pt_page_pml4_index[slot] != pml4_index) continue;
        if (space.pt_page_pdp_index[slot] != pdp_index) continue;
        if (space.pt_page_pd_index[slot] == pd_index) return slot;
    }
    return null;
}

fn userPtSlotHasUserMappings(h: Hooks, space: *const UserAddressSpace, slot: usize) bool {
    if (slot >= UserAddressSpace.max_dynamic_pt_pages) return true;
    const pt_page: *const [512]u64 = &space.pt_pages[slot];
    var i: usize = 0;
    while (i < h.page_entries) : (i += 1) {
        const entry = pt_page[i];
        if ((entry & h.page_present) != 0 and (entry & h.page_user) != 0) return true;
    }
    return false;
}

fn releaseUserPtSlotIfEmpty(h: Hooks, space: *UserAddressSpace, slot: usize) void {
    if (slot >= UserAddressSpace.max_dynamic_pt_pages) return;
    if (userPtSlotHasUserMappings(h, space, slot)) return;
    const pml4_index = space.pt_page_pml4_index[slot];
    const pdp_index = space.pt_page_pdp_index[slot];
    const pd_index = space.pt_page_pd_index[slot];
    if (pml4_index == UserAddressSpace.no_pd_index or
        pdp_index == UserAddressSpace.no_pd_index or
        pd_index == UserAddressSpace.no_pd_index)
    {
        return;
    }
    if (findUserPdSlotForPdp(space, pml4_index, pdp_index)) |pd_slot| {
        const pd_page: *[512]u64 = &space.pd_pages[pd_slot];
        const entry = pd_page[pd_index];
        const pt_pa: u64 = h.kernel_pointer_paddr(@intFromPtr(&space.pt_pages[slot])) orelse return;
        if ((entry & h.page_addr_mask) == pt_pa) {
            pd_page[pd_index] = 0;
        }
    }
    @memset(space.pt_pages[slot][0..], 0);
    space.pt_page_pml4_index[slot] = UserAddressSpace.no_pd_index;
    space.pt_page_pdp_index[slot] = UserAddressSpace.no_pd_index;
    space.pt_page_pd_index[slot] = UserAddressSpace.no_pd_index;
}

fn ptSlotScanLimit(space: *const UserAddressSpace) usize {
    const used_len = @min(@as(usize, @intCast(space.pt_page_used_len)), UserAddressSpace.max_dynamic_pt_pages);
    return if (used_len == 0) UserAddressSpace.max_dynamic_pt_pages else used_len;
}

fn pdSlotScanLimit(space: *const UserAddressSpace) usize {
    const used_len = @min(@as(usize, @intCast(space.pd_page_used_len)), UserAddressSpace.max_dynamic_pd_pages);
    return if (used_len == 0) UserAddressSpace.max_dynamic_pd_pages else used_len;
}

fn pdpSlotScanLimit(space: *const UserAddressSpace) usize {
    const used_len = @min(@as(usize, @intCast(space.pdp_page_used_len)), UserAddressSpace.max_dynamic_pdp_pages);
    return if (used_len == 0) UserAddressSpace.max_dynamic_pdp_pages else used_len;
}

fn findUserPdpSlotForPml4(space: *const UserAddressSpace, pml4_index: usize) ?usize {
    const slot_limit = pdpSlotScanLimit(space);
    var slot: usize = 0;
    while (slot < slot_limit) : (slot += 1) {
        if (space.pdp_page_pml4_index[slot] == UserAddressSpace.no_pd_index) continue;
        if (space.pdp_page_pml4_index[slot] == pml4_index) return slot;
    }
    return null;
}

fn findUserPdSlotForPdp(space: *const UserAddressSpace, pml4_index: usize, pdp_index: usize) ?usize {
    const slot_limit = pdSlotScanLimit(space);
    var slot: usize = 0;
    while (slot < slot_limit) : (slot += 1) {
        if (space.pd_page_pml4_index[slot] == UserAddressSpace.no_pd_index) continue;
        if (space.pd_page_pdp_index[slot] == UserAddressSpace.no_pd_index) continue;
        if (space.pd_page_pml4_index[slot] != pml4_index) continue;
        if (space.pd_page_pdp_index[slot] == pdp_index) return slot;
    }
    return null;
}

fn seedPdSlotFromExistingPdp(h: Hooks, space: *UserAddressSpace, slot: usize, pml4_index: usize, pdp_index: usize, existing_pdpe: u64) void {
    const pd_page: *[512]u64 = &space.pd_pages[slot];
    @memset(pd_page[0..], 0);
    if ((existing_pdpe & h.page_present) != 0 and (existing_pdpe & h.page_ps) == 0 and (existing_pdpe & h.page_user) == 0) {
        const src_pd_addr = existing_pdpe & h.page_addr_mask;
        if (src_pd_addr != 0 and src_pd_addr < h.physical_map_limit) {
            const src_pd: *const [512]u64 = @ptrFromInt(src_pd_addr);
            var copy_index: usize = 0;
            while (copy_index < h.page_entries) : (copy_index += 1) {
                pd_page[copy_index] = src_pd[copy_index] & ~h.page_user;
            }
            return;
        }
    }
    if (pml4_index == 0 and pdp_index == 0) {
        h.seed_user_pd_with_kernel_identity(pd_page[0..]);
    }
}

fn ensureUserPdpSlotForPml4(space: *UserAddressSpace, pml4_index: usize) ?usize {
    const h = hooks orelse return null;
    if (pml4_index >= 256) return null;
    if (findUserPdpSlotForPml4(space, pml4_index)) |slot| return slot;

    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pdp_pages and space.pdp_page_pml4_index[slot] != UserAddressSpace.no_pd_index) : (slot += 1) {}
    if (slot >= UserAddressSpace.max_dynamic_pdp_pages) return null;
    space.pdp_page_pml4_index[slot] = @intCast(pml4_index);
    const pdp_page: *[512]u64 = &space.pdp_pages[slot];
    @memset(pdp_page[0..], 0);
    if (pml4_index == 0) {
        h.seed_user_pdp_with_kernel_identity(pdp_page[0..]);
    }
    const pdp_pa: u64 = h.kernel_pointer_paddr(@intFromPtr(pdp_page)) orelse return null;
    if (pdp_pa >= h.physical_map_limit) return null;
    space.pml4[pml4_index] = pdp_pa | h.page_present | h.page_rw | h.page_user;
    const next_len = slot + 1;
    if (next_len > space.pdp_page_used_len) {
        space.pdp_page_used_len = @intCast(next_len);
    }
    return slot;
}

fn ensureUserPdSlotForPdp(space: *UserAddressSpace, pml4_index: usize, pdp_index: usize) ?usize {
    const h = hooks orelse return null;
    if (pml4_index >= 256) return null;
    if (pdp_index >= h.page_entries) return null;
    const pdp_slot = ensureUserPdpSlotForPml4(space, pml4_index) orelse return null;
    if (findUserPdSlotForPdp(space, pml4_index, pdp_index)) |slot| return slot;

    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pd_pages and space.pd_page_pdp_index[slot] != UserAddressSpace.no_pd_index) : (slot += 1) {}
    if (slot >= UserAddressSpace.max_dynamic_pd_pages) return null;
    space.pd_page_pml4_index[slot] = @intCast(pml4_index);
    space.pd_page_pdp_index[slot] = @intCast(pdp_index);
    const pdp_page: *[512]u64 = &space.pdp_pages[pdp_slot];
    const existing_pdpe = pdp_page[pdp_index];
    seedPdSlotFromExistingPdp(h, space, slot, pml4_index, pdp_index, existing_pdpe);
    const pd_pa: u64 = h.kernel_pointer_paddr(@intFromPtr(&space.pd_pages[slot])) orelse return null;
    if (pd_pa >= h.physical_map_limit) return null;
    pdp_page[pdp_index] = pd_pa | h.page_present | h.page_rw | h.page_user;
    const next_len = slot + 1;
    if (next_len > space.pd_page_used_len) {
        space.pd_page_used_len = @intCast(next_len);
    }
    return slot;
}

fn seedPtSlotFromExistingPd(space: *UserAddressSpace, slot: usize, existing_pde: u64) void {
    const h = hooks orelse return;
    const pt_page: *[512]u64 = &space.pt_pages[slot];
    @memset(pt_page[0..], 0);
    if ((existing_pde & h.page_present) == 0) return;
    if ((existing_pde & h.page_ps) == 0) {
        if ((existing_pde & h.page_user) != 0) return;
        const src_pt_addr = existing_pde & h.page_addr_mask;
        if (src_pt_addr == 0 or src_pt_addr >= h.physical_map_limit) return;
        const src_pt: *const [512]u64 = @ptrFromInt(src_pt_addr);
        var copy_index: usize = 0;
        while (copy_index < h.page_entries) : (copy_index += 1) {
            pt_page[copy_index] = src_pt[copy_index] & ~h.page_user;
        }
        return;
    }

    const two_mib_mask = ~@as(u64, 0x1F_FFFF);
    const page_base = existing_pde & two_mib_mask;
    const pte_flags = (existing_pde & 0xFFF) & ~h.page_ps & ~h.page_user;
    var pt_index: usize = 0;
    while (pt_index < h.page_entries) : (pt_index += 1) {
        const paddr = page_base + @as(u64, @intCast(pt_index)) * 4096;
        pt_page[pt_index] = paddr | pte_flags;
    }
}

pub fn ensureUserPtSlotForPd(space: *UserAddressSpace, pml4_index: usize, pdp_index: usize, pd_index: usize) ?usize {
    const h = hooks orelse return null;
    if (pml4_index >= 256) return null;
    if (pdp_index >= h.page_entries) return null;
    if (pd_index >= h.page_entries) return null;
    const pd_slot = ensureUserPdSlotForPdp(space, pml4_index, pdp_index) orelse return null;
    if (findUserPtSlotForPd(space, pml4_index, pdp_index, pd_index)) |slot| return slot;

    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        if (space.pt_page_pd_index[slot] != UserAddressSpace.no_pd_index) continue;
        break;
    }
    if (slot >= UserAddressSpace.max_dynamic_pt_pages) return null;
    space.pt_page_pml4_index[slot] = @intCast(pml4_index);
    space.pt_page_pdp_index[slot] = @intCast(pdp_index);
    space.pt_page_pd_index[slot] = @intCast(pd_index);
    const pd_page: *[512]u64 = &space.pd_pages[pd_slot];
    const existing_pde = pd_page[pd_index];
    seedPtSlotFromExistingPd(space, slot, existing_pde);
    const pt_pa: u64 = h.kernel_pointer_paddr(@intFromPtr(&space.pt_pages[slot])) orelse return null;
    if (pt_pa >= h.physical_map_limit) return null;
    pd_page[pd_index] = pt_pa | h.page_present | h.page_rw | h.page_user;
    const next_len = slot + 1;
    if (next_len > space.pt_page_used_len) {
        space.pt_page_used_len = @intCast(next_len);
    }
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

fn pteFlagsForUncachedProt(h: Hooks, prot: kernel.MapProt) ?u64 {
    const flags = pteFlagsForProt(h, prot) orelse return null;
    return flags | pte_cache_write_through | pte_cache_disable;
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
    return mapUserLinearRegionWithPteFlags(principal, va_start, paddr_start, size_bytes, prot, false);
}

pub fn mapUserUncachedLinearRegionWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddr_start: u64,
    size_bytes: usize,
    prot: kernel.MapProt,
) bool {
    return mapUserLinearRegionWithPteFlags(principal, va_start, paddr_start, size_bytes, prot, true);
}

fn mapUserLinearRegionWithPteFlags(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddr_start: u64,
    size_bytes: usize,
    prot: kernel.MapProt,
    uncached: bool,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    const pte_flags = if (uncached) pteFlagsForUncachedProt(h, prot) orelse return false else pteFlagsForProt(h, prot) orelse return false;
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
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) != 0 and (old_entry & h.page_user) != 0) return false;
    }
    if (!reserveUserMapping(principal, va_start, page_count_u64, .linear_region, prot.write)) return false;

    offset = 0;
    while (offset < size_bytes) : (offset += 4096) {
        const va = va_start + offset;
        const paddr = paddr_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        pt_page[index.pt] = paddr | pte_flags;
    }

    return true;
}

fn mapTrustedUserPaddrsWithProtInternal(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
    reserve_pages: bool,
    allow_low_page_zero: bool,
) bool {
    if (!lockAddressSpace(principal)) return false;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    const pte_flags = pteFlagsForProt(h, prot) orelse return false;
    if (paddrs.len == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const page_count_u64: u64 = @intCast(paddrs.len);
    const size_u64 = page_count_u64 * 4096;
    _ = userRangeEndVaWithLowPageZero(h, va_start, size_u64, allow_low_page_zero) orelse return false;

    var page_index: usize = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const paddr = paddrs[page_index];
        if ((paddr & 0xFFF) != 0 or paddr >= h.physical_map_limit) return false;

        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVaWithLowPageZero(h, va, allow_low_page_zero) orelse return false;
        const pt_slot = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) != 0 and (old_entry & h.page_user) != 0) return false;
    }
    if (reserve_pages and !reserveUserMapping(principal, va_start, page_count_u64, .linear_region, prot.write)) return false;

    page_index = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVaWithLowPageZero(h, va, allow_low_page_zero) orelse return false;
        const pt_slot = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        pt_page[index.pt] = paddrs[page_index] | pte_flags;
    }

    return true;
}

pub fn mapTrustedUserPaddrsWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
) bool {
    return mapTrustedUserPaddrsWithProtInternal(principal, va_start, paddrs, prot, true, false);
}

pub fn mapTrustedLowPageZeroPaddrsWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
) bool {
    if (va_start != 0 or paddrs.len != 1) return false;
    if (!prot.read or prot.write or !prot.exec) return false;
    return mapTrustedUserPaddrsWithProtInternal(principal, va_start, paddrs, prot, true, true);
}

pub fn mapLazyLowPageZeroPaddrsWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
) bool {
    if (va_start != 0 or paddrs.len != 1) return false;
    if (!prot.read or prot.write or !prot.exec) return false;
    return mapTrustedUserPaddrsWithProtInternal(principal, va_start, paddrs, prot, false, true);
}

pub fn mapLazyUserPaddrsWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddrs: []const u64,
    prot: kernel.MapProt,
) bool {
    return mapTrustedUserPaddrsWithProtInternal(principal, va_start, paddrs, prot, false, false);
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
    const pte_flags = pteFlagsForProt(h, prot) orelse return false;
    if (paddrs.len == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const page_count_u64: u64 = @intCast(paddrs.len);
    const size_u64 = page_count_u64 * 4096;
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;

    var page_index: usize = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const paddr = paddrs[page_index];
        if ((paddr & 0xFFF) != 0 or paddr >= h.physical_map_limit) return false;
    }

    page_index = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = ensureUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        pt_page[index.pt] = paddrs[page_index] | pte_flags;
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
    const pte_flags = pteFlagsForProt(h, prot) orelse return false;
    if (paddrs.len == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const page_count_u64: u64 = @intCast(paddrs.len);
    const size_u64 = page_count_u64 * 4096;
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;

    var page_index: usize = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const paddr = paddrs[page_index];
        if ((paddr & 0xFFF) != 0 or paddr >= h.physical_map_limit) return false;
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse return false;
        if (findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd)) |pt_slot| {
            const old_entry = space.pt_pages[pt_slot][index.pt];
            if ((old_entry & h.page_present) != 0 and (old_entry & h.page_user) == 0) return false;
        }
    }

    page_index = 0;
    while (page_index < paddrs.len) : (page_index += 1) {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse return false;
        var present = false;
        if (findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd)) |pt_slot| {
            const old_entry = space.pt_pages[pt_slot][index.pt];
            present = (old_entry & h.page_present) != 0 and (old_entry & h.page_user) != 0;
        }
        if (!present and !reserveUserMapping(principal, va, 1, .linear_region, prot.write)) {
            var rollback_index: usize = 0;
            while (rollback_index < page_index) : (rollback_index += 1) {
                const rollback_va = va_start + @as(u64, @intCast(rollback_index)) * 4096;
                const rollback_page = userPageIndexForVa(h, rollback_va) orelse continue;
                var rollback_present = false;
                if (findUserPtSlotForPd(space, rollback_page.pml4, rollback_page.pdp, rollback_page.pd)) |pt_slot| {
                    const old_entry = space.pt_pages[pt_slot][rollback_page.pt];
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
        space.pt_pages[pt_slot][index.pt] = paddrs[page_index] | pte_flags;
    }
    h.flush_user_tlb_for_principal_range(principal, va_start, @intCast(size_u64));

    return true;
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

    var offset: u64 = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) == 0) return false;
        if ((old_entry & h.page_user) == 0) return false;
        const paddr = ptePaddr(old_entry);
        pt_page[index.pt] = paddr | pte_flags;
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

    var offset: u64 = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse continue;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) == 0) continue;
        if ((old_entry & h.page_user) == 0) return false;
        pt_page[index.pt] = ptePaddr(old_entry) | pte_flags;
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

    var offset: u64 = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *const [512]u64 = &space.pt_pages[pt_slot];
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) == 0) return false;
        if ((old_entry & h.page_user) == 0) return false;
    }

    var touched_slots: [UserAddressSpace.max_dynamic_pt_pages]u16 = undefined;
    var touched_count: usize = 0;

    offset = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        pt_page[index.pt] = 0;
        var seen = false;
        var touched_index: usize = 0;
        while (touched_index < touched_count) : (touched_index += 1) {
            if (touched_slots[touched_index] == pt_slot) {
                seen = true;
                break;
            }
        }
        if (!seen and touched_count < touched_slots.len) {
            touched_slots[touched_count] = @intCast(pt_slot);
            touched_count += 1;
        }
    }
    if (!releaseUserMapping(principal, va_start, size_u64 / 4096)) return false;
    h.flush_user_tlb_for_principal_range(principal, va_start, size_bytes);
    var touched_index: usize = 0;
    while (touched_index < touched_count) : (touched_index += 1) {
        releaseUserPtSlotIfEmpty(h, space, touched_slots[touched_index]);
    }

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
    if (!lockAddressSpace(principal)) return null;
    defer unlockAddressSpace(principal);
    const h = hooks orelse return null;
    const space = getUserSpace(principal) orelse return null;
    if (size_bytes == 0 or (va_start & 0xFFF) != 0) return null;
    const size_u64: u64 = @intCast(size_bytes);
    _ = userRangeEndVa(h, va_start, size_u64) orelse return null;

    var offset: u64 = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return null;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse continue;
        const entry = space.pt_pages[pt_slot][index.pt];
        if ((entry & h.page_present) != 0 and (entry & h.page_user) == 0) return null;
    }

    const release_start_page = va_start >> 12;
    const release_end_page = release_start_page + size_u64 / 4096;
    var needs_split_slot = false;
    for (&space.reservations) |reservation| {
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

pub fn freeUserReservationSlotCount(principal: kernel.PrincipalId) usize {
    if (!lockAddressSpace(principal)) return 0;
    defer unlockAddressSpace(principal);
    const space = getUserSpace(principal) orelse return 0;
    var count: usize = 0;
    for (&space.reservations) |reservation| {
        if (!reservation.active) count += 1;
    }
    return count;
}

pub fn unmapPresentUserLinearRegion(
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

    var touched_slots: [UserAddressSpace.max_dynamic_pt_pages]u16 = undefined;
    var touched_count: usize = 0;

    var offset: u64 = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse continue;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) == 0) continue;
        if ((old_entry & h.page_user) == 0) return false;
        pt_page[index.pt] = 0;

        var seen = false;
        var touched_index: usize = 0;
        while (touched_index < touched_count) : (touched_index += 1) {
            if (touched_slots[touched_index] == pt_slot) {
                seen = true;
                break;
            }
        }
        if (!seen and touched_count < touched_slots.len) {
            touched_slots[touched_count] = @intCast(pt_slot);
            touched_count += 1;
        }
    }

    _ = releaseUserMapping(principal, va_start, size_u64 / 4096);
    h.flush_user_tlb_for_principal_range(principal, va_start, size_bytes);
    var touched_index: usize = 0;
    while (touched_index < touched_count) : (touched_index += 1) {
        releaseUserPtSlotIfEmpty(h, space, touched_slots[touched_index]);
    }

    return true;
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

    var offset: u64 = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse continue;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        const old_entry = pt_page[index.pt];
        if ((old_entry & h.page_present) == 0) continue;
        if ((old_entry & h.page_user) == 0) continue;
        pt_page[index.pt] = 0;
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
        const old_entry = space.pt_pages[pt_slot][index.pt];
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
        space.pt_pages[pt_slot][index.pt] = 0;
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
    const pte_flags = pteFlagsForProt(h, prot) orelse return false;
    if (paddrs.len == 0 or (va_start & 0xFFF) != 0) return false;
    const size_u64, const size_overflow = @mulWithOverflow(
        @as(u64, @intCast(paddrs.len)),
        @as(u64, 4096),
    );
    if (size_overflow != 0 or size_u64 > std.math.maxInt(usize)) return false;
    _ = userRangeEndVa(h, va_start, size_u64) orelse return false;

    for (paddrs, 0..) |paddr, page_index| {
        if (paddr == 0 or (paddr & 0xFFF) != 0 or paddr >= h.physical_map_limit) return false;
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse return false;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse return false;
        if ((space.pt_pages[pt_slot][index.pt] & h.page_present) != 0) return false;
    }
    for (paddrs, 0..) |paddr, page_index| {
        const va = va_start + @as(u64, @intCast(page_index)) * 4096;
        const index = userPageIndexForVa(h, va) orelse unreachable;
        const pt_slot = findUserPtSlotForPd(space, index.pml4, index.pdp, index.pd) orelse unreachable;
        space.pt_pages[pt_slot][index.pt] = paddr | pte_flags;
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
        const pt_page: *const [512]u64 = &space.pt_pages[pt_slot];
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
    const slot_limit = ptSlotScanLimit(space);
    var slot: usize = 0;
    while (slot < slot_limit) : (slot += 1) {
        const pd_index_meta = space.pt_page_pd_index[slot];
        const pml4_index_meta = space.pt_page_pml4_index[slot];
        const pdp_index_meta = space.pt_page_pdp_index[slot];
        if (pml4_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pdp_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pd_index_meta == UserAddressSpace.no_pd_index) continue;
        const pml4_index: usize = @intCast(pml4_index_meta);
        const pdp_index: usize = @intCast(pdp_index_meta);
        const pd_index: usize = @intCast(pd_index_meta);
        var pt_index: usize = 0;
        while (pt_index < h.page_entries) : (pt_index += 1) {
            const pt_page: *[512]u64 = &space.pt_pages[slot];
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
            h.flush_user_tlb_for_principal_va(principal, va);
            removed += 1;
        }
    }
    return removed;
}

pub fn collectUserMappedPaddrs(principal: kernel.PrincipalId, out_paddrs: []u64) usize {
    const h = hooks orelse return 0;
    const space = getUserSpace(principal) orelse return 0;
    var count: usize = 0;
    const slot_limit = ptSlotScanLimit(space);
    var slot: usize = 0;
    while (slot < slot_limit) : (slot += 1) {
        if (space.pt_page_pml4_index[slot] == UserAddressSpace.no_pd_index) continue;
        if (space.pt_page_pdp_index[slot] == UserAddressSpace.no_pd_index) continue;
        if (space.pt_page_pd_index[slot] == UserAddressSpace.no_pd_index) continue;
        const pt_page: *const [512]u64 = &space.pt_pages[slot];
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

pub const MappedUserPageVisitor = *const fn (context: *anyopaque, page: MappedUserPage) bool;

pub fn forEachUserMappedPage(principal: kernel.PrincipalId, context: *anyopaque, visitor: MappedUserPageVisitor) bool {
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    const slot_limit = ptSlotScanLimit(space);
    var slot: usize = 0;
    while (slot < slot_limit) : (slot += 1) {
        const pd_index_meta = space.pt_page_pd_index[slot];
        const pml4_index_meta = space.pt_page_pml4_index[slot];
        const pdp_index_meta = space.pt_page_pdp_index[slot];
        if (pml4_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pdp_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pd_index_meta == UserAddressSpace.no_pd_index) continue;
        const pml4_index: usize = @intCast(pml4_index_meta);
        const pdp_index: usize = @intCast(pdp_index_meta);
        const pd_index: usize = @intCast(pd_index_meta);
        const pt_page: *const [512]u64 = &space.pt_pages[slot];
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
    const slot_limit = ptSlotScanLimit(space);
    var slot: usize = 0;
    while (slot < slot_limit) : (slot += 1) {
        const pd_index_meta = space.pt_page_pd_index[slot];
        const pml4_index_meta = space.pt_page_pml4_index[slot];
        const pdp_index_meta = space.pt_page_pdp_index[slot];
        if (pml4_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pdp_index_meta == UserAddressSpace.no_pd_index) continue;
        if (pd_index_meta == UserAddressSpace.no_pd_index) continue;
        const pml4_index: usize = @intCast(pml4_index_meta);
        const pdp_index: usize = @intCast(pdp_index_meta);
        const pd_index: usize = @intCast(pd_index_meta);
        const pt_page: *const [512]u64 = &space.pt_pages[slot];
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

pub fn buildUserAddressSpace(principal: kernel.PrincipalId, user_page_paddr: u64, user_stack_paddr: u64) bool {
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    @memset(space.pml4[0..], 0);
    h.seed_user_pml4_with_kernel(space.pml4[0..]);
    var pdp_slot_init: usize = 0;
    while (pdp_slot_init < UserAddressSpace.max_dynamic_pdp_pages) : (pdp_slot_init += 1) {
        space.pdp_page_pml4_index[pdp_slot_init] = UserAddressSpace.no_pd_index;
        const pdp_page: *[512]u64 = &space.pdp_pages[pdp_slot_init];
        @memset(pdp_page[0..], 0);
    }
    var pd_slot_init: usize = 0;
    while (pd_slot_init < UserAddressSpace.max_dynamic_pd_pages) : (pd_slot_init += 1) {
        space.pd_page_pml4_index[pd_slot_init] = UserAddressSpace.no_pd_index;
        space.pd_page_pdp_index[pd_slot_init] = UserAddressSpace.no_pd_index;
        const pd_page: *[512]u64 = &space.pd_pages[pd_slot_init];
        @memset(pd_page[0..], 0);
    }
    var pt_slot_init: usize = 0;
    while (pt_slot_init < UserAddressSpace.max_dynamic_pt_pages) : (pt_slot_init += 1) {
        space.pt_page_pml4_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        space.pt_page_pdp_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        space.pt_page_pd_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot_init];
        @memset(pt_page[0..], 0);
    }
    space.pdp_page_used_len = 0;
    space.pd_page_used_len = 0;
    space.pt_page_used_len = 0;
    resetUserReservations(space);

    const user_pml4_pa: u64 = h.kernel_pointer_paddr(@intFromPtr(&space.pml4)) orelse return false;
    if (user_pml4_pa >= h.four_gib) return false;
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
    const user_pt_page: *[512]u64 = &space.pt_pages[user_slot];
    const stack_pt_page: *[512]u64 = &space.pt_pages[stack_slot];
    if (!reserveUserMapping(principal, h.user_va, 1, .bootstrap, true)) return false;
    if (!reserveUserMapping(principal, h.user_stack_page_va, 1, .bootstrap, true)) return false;
    user_pt_page[user_pt_index] = user_page_paddr | h.page_present | h.page_user;
    stack_pt_page[stack_pt_index] = user_stack_paddr | h.page_present | h.page_rw | h.page_user | h.page_nx;
    space.cr3 = user_pml4_pa;
    return true;
}

pub fn buildEmptyUserAddressSpace(principal: kernel.PrincipalId) bool {
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    @memset(space.pml4[0..], 0);
    h.seed_user_pml4_with_kernel(space.pml4[0..]);
    var pdp_slot_init: usize = 0;
    while (pdp_slot_init < UserAddressSpace.max_dynamic_pdp_pages) : (pdp_slot_init += 1) {
        space.pdp_page_pml4_index[pdp_slot_init] = UserAddressSpace.no_pd_index;
        const pdp_page: *[512]u64 = &space.pdp_pages[pdp_slot_init];
        @memset(pdp_page[0..], 0);
    }
    var pd_slot_init: usize = 0;
    while (pd_slot_init < UserAddressSpace.max_dynamic_pd_pages) : (pd_slot_init += 1) {
        space.pd_page_pml4_index[pd_slot_init] = UserAddressSpace.no_pd_index;
        space.pd_page_pdp_index[pd_slot_init] = UserAddressSpace.no_pd_index;
        const pd_page: *[512]u64 = &space.pd_pages[pd_slot_init];
        @memset(pd_page[0..], 0);
    }
    var pt_slot_init: usize = 0;
    while (pt_slot_init < UserAddressSpace.max_dynamic_pt_pages) : (pt_slot_init += 1) {
        space.pt_page_pml4_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        space.pt_page_pdp_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        space.pt_page_pd_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot_init];
        @memset(pt_page[0..], 0);
    }
    space.pdp_page_used_len = 0;
    space.pd_page_used_len = 0;
    space.pt_page_used_len = 0;
    resetUserReservations(space);

    const user_pml4_pa: u64 = h.kernel_pointer_paddr(@intFromPtr(&space.pml4)) orelse return false;
    if (user_pml4_pa >= h.four_gib) return false;

    _ = ensureUserPdpSlotForPml4(space, 0) orelse return false;
    space.cr3 = user_pml4_pa;
    return true;
}

fn processIndex(principal: kernel.PrincipalId) ?usize {
    return kernel.processIndexFromPrincipal(principal);
}
