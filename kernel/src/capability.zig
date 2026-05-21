const std = @import("std");
const kernel = @import("kernel.zig");
const interrupts = @import("interrupts.zig");

pub const UserAddressSpace = struct {
    // Includes user VA mappings plus supervisor-only helper PTs for return stacks.
    pub const max_dynamic_pt_pages: usize = 320;
    pub const max_reservations: usize = 2048;
    pub const no_pd_index: u16 = 0xFFFF;

    pub const ReservationKind = enum(u8) {
        none = 0,
        bootstrap = 1,
        capability_page = 2,
        fresh_page = 3,
        linear_region = 4,
    };

    pub const Reservation = struct {
        base_va: u64 = 0,
        page_count: u32 = 0,
        generation: u32 = 0,
        kind: ReservationKind = .none,
        writable: bool = false,
        active: bool = false,
    };

    pml4: [512]u64 align(4096) = [_]u64{0} ** 512,
    pdp: [512]u64 align(4096) = [_]u64{0} ** 512,
    pd: [512]u64 align(4096) = [_]u64{0} ** 512,
    pt_pages: [max_dynamic_pt_pages][512]u64 align(4096) = [_][512]u64{[_]u64{0} ** 512} ** max_dynamic_pt_pages,
    pt_page_pd_index: [max_dynamic_pt_pages]u16 = [_]u16{no_pd_index} ** max_dynamic_pt_pages,
    pt_page_used_len: u16 = 0,
    reservations: [max_reservations]Reservation = [_]Reservation{.{}} ** max_reservations,
    reservation_generation: u32 = 0,
    next_dynamic_map_page: u64 = 0,
    cr3: u64 = 0,
};

pub const RuntimeConfig = struct {
    user_spaces: []UserAddressSpace,
    user_va: u64,
    physical_map_limit: u64,
    page_entries: usize,
    page_addr_mask: u64,
    page_present: u64,
    page_rw: u64,
    page_user: u64,
    canonical_user_limit_exclusive: u64,
    serial_write: *const fn (text: []const u8) void,
    print_hex: *const fn (value: u64) void,
    principal_label: *const fn (principal: kernel.PrincipalId) []const u8,
    flush_user_tlb_for_principal_va: *const fn (principal: kernel.PrincipalId, va: u64) void,
};

pub const PageFaultCapability = struct {
    principal: kernel.PrincipalId,
    fault_va: u64,
    fault_page_va: u64,
    fault_rip: u64,
    present_violation: bool,
    write_access: bool,
    instruction_fetch: bool,
    candidate_paddr: ?u64,
};

var runtime_ready = false;
var runtime: RuntimeConfig = undefined;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(runtime_ready), &runtime_ready));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(runtime), &runtime));
    return end;
}

pub fn init(config: RuntimeConfig) void {
    runtime = config;
    runtime_ready = true;
}

fn processIndex(principal: kernel.PrincipalId) ?usize {
    return kernel.processIndexFromPrincipal(principal);
}

fn principalFromProcessIndex(index: usize) ?kernel.PrincipalId {
    return kernel.processPrincipalFromIndex(index);
}

fn getUserSpace(principal: kernel.PrincipalId) ?*UserAddressSpace {
    if (!runtime_ready) return null;
    const index = processIndex(principal) orelse return null;
    if (index >= runtime.user_spaces.len) return null;
    return &runtime.user_spaces[index];
}

pub fn isUserCanonicalVa(va: u64) bool {
    if (!runtime_ready) return false;
    return va < runtime.canonical_user_limit_exclusive;
}

pub fn lookupUserMappedPaddrForVa(principal: kernel.PrincipalId, va: u64) ?u64 {
    const space = getUserSpace(principal) orelse return null;
    const pd_index = userPdIndexForVa(va) orelse return null;
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const slot = findPtSlotForPd(space, pd_index) orelse return null;
    const pt_page: *const [512]u64 = &space.pt_pages[slot];
    const entry = pt_page[pt_index];
    const is_user_mapping = (entry & runtime.page_present) != 0 and (entry & runtime.page_user) != 0;
    const paddr = entry & runtime.page_addr_mask;
    if (!is_user_mapping or paddr == 0) return null;
    return paddr;
}

pub fn parseRights(bits: u64) kernel.Rights {
    return .{
        .cpu_read = (bits & 0x1) != 0,
        .cpu_write = (bits & 0x2) != 0,
        .dma = (bits & 0x4) != 0,
        .grant = (bits & 0x8) != 0,
    };
}

fn pageAlignDown(addr: u64) u64 {
    return addr & ~@as(u64, 4095);
}

pub fn resetUserReservations(space: *UserAddressSpace) void {
    @memset(space.reservations[0..], .{});
    space.reservation_generation = 0;
    space.next_dynamic_map_page = 0;
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
        if (merged_count == 0 or merged_count > std.math.maxInt(u32)) return false;

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

pub fn canReserveUserMapping(principal: kernel.PrincipalId, base_va: u64, page_count: u64) bool {
    if (!runtime_ready) return false;
    const space = getUserSpace(principal) orelse return false;
    _ = reservationEndPage(base_va, page_count) orelse return false;
    for (&space.reservations) |*reservation| {
        if (!reservation.active) continue;
        if (!reservationOverlaps(base_va, page_count, reservation.base_va, reservation.page_count)) continue;
        return false;
    }
    return true;
}

pub fn reserveUserMapping(
    principal: kernel.PrincipalId,
    base_va: u64,
    page_count: u64,
    kind: UserAddressSpace.ReservationKind,
    writable: bool,
) bool {
    if (!runtime_ready) return false;
    if (kind == .none) return false;
    if (page_count > std.math.maxInt(u32)) return false;
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
        const new_count = record_end_page - release_end_page;
        reservation.base_va = release_end_page << 12;
        reservation.page_count = @intCast(new_count);
        return true;
    }
    if (release_end_page >= record_end_page) {
        const new_count = release_start_page - record_start_page;
        reservation.page_count = @intCast(new_count);
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
    if (!runtime_ready) return false;
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

pub fn userMappingRangeCoveredByKind(
    principal: kernel.PrincipalId,
    base_va: u64,
    page_count: u64,
    kind: UserAddressSpace.ReservationKind,
) bool {
    if (!runtime_ready) return false;
    if (kind == .none) return false;
    const space = getUserSpace(principal) orelse return false;
    const target_end_page = reservationEndPage(base_va, page_count) orelse return false;
    var cursor_page = base_va >> 12;
    while (cursor_page < target_end_page) {
        var next_page = cursor_page;
        for (&space.reservations) |*reservation| {
            if (!reservation.active) continue;
            if (reservation.kind != kind) continue;
            const record_start_page = reservation.base_va >> 12;
            if (record_start_page > cursor_page) continue;
            const record_end_page = record_start_page + reservation.page_count;
            if (record_end_page <= cursor_page) continue;
            if (record_end_page > next_page) {
                next_page = if (record_end_page < target_end_page) record_end_page else target_end_page;
            }
        }
        if (next_page == cursor_page) return false;
        cursor_page = next_page;
    }
    return true;
}

fn userDynamicMapRange() ?struct { start_page: u64, end_page: u64 } {
    const base = runtime.user_va + 0x0300_0000;
    const layout_end = runtime.user_va + 0x1C00_0000;
    const pdp_base = (runtime.user_va >> 30) << 30;
    const pdp_end = pdp_base + 0x4000_0000;
    const end = if (layout_end < pdp_end) layout_end else pdp_end;
    const start_page = (base + 0xFFF) >> 12;
    const end_page = end >> 12;
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
    if (!runtime_ready) return null;
    if (page_count == 0) return null;
    const alignment = if (align_pages == 0) @as(u64, 1) else align_pages;
    const space = getUserSpace(principal) orelse return null;
    const range = userDynamicMapRange() orelse return null;
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

fn userPdIndexForVa(va: u64) ?usize {
    const pml4_index: usize = @intCast((va >> 39) & 0x1FF);
    const pdp_index: usize = @intCast((va >> 30) & 0x1FF);
    const pd_index: usize = @intCast((va >> 21) & 0x1FF);
    const user_pdp_index: usize = @intCast((runtime.user_va >> 30) & 0x1FF);

    if (pml4_index != 0 or pdp_index != user_pdp_index) return null;
    return pd_index;
}

fn aliasVaForPdPt(pd_index: usize, pt_index: usize) u64 {
    const user_pdp_index: u64 = (runtime.user_va >> 30) & 0x1FF;
    return (user_pdp_index << 30) |
        (@as(u64, @intCast(pd_index)) << 21) |
        (@as(u64, @intCast(pt_index)) << 12);
}

fn slotPtPa(space: *const UserAddressSpace, slot: usize) u64 {
    return @intFromPtr(&space.pt_pages[slot]);
}

fn slotHasAnyMappedPaddr(space: *const UserAddressSpace, slot: usize) bool {
    if (slot >= UserAddressSpace.max_dynamic_pt_pages) return true;
    const pt_page: *const [512]u64 = &space.pt_pages[slot];
    var i: usize = 0;
    while (i < runtime.page_entries) : (i += 1) {
        if ((pt_page[i] & runtime.page_addr_mask) != 0) return true;
    }
    return false;
}

fn ptSlotScanLimit(space: *const UserAddressSpace) usize {
    const used_len = @min(@as(usize, @intCast(space.pt_page_used_len)), UserAddressSpace.max_dynamic_pt_pages);
    return if (used_len == 0) UserAddressSpace.max_dynamic_pt_pages else used_len;
}

fn pdIndexForPtSlot(space: *const UserAddressSpace, slot: usize) ?usize {
    if (slot >= UserAddressSpace.max_dynamic_pt_pages) return null;
    const pt_pa = slotPtPa(space, slot);
    var pd_index: usize = 0;
    while (pd_index < 512) : (pd_index += 1) {
        const pde = space.pd[pd_index];
        if ((pde & runtime.page_present) == 0) continue;
        if ((pde & (@as(u64, 1) << 7)) != 0) continue; // huge page
        if ((pde & runtime.page_addr_mask) == pt_pa) return pd_index;
    }
    return null;
}

fn findPtSlotForPd(space: *UserAddressSpace, pd_index: usize) ?usize {
    if (pd_index >= 512) return null;

    const pde = space.pd[pd_index];
    if ((pde & runtime.page_present) != 0 and (pde & (@as(u64, 1) << 7)) == 0) {
        const pt_pa = pde & runtime.page_addr_mask;
        var slot_by_pd: usize = 0;
        while (slot_by_pd < UserAddressSpace.max_dynamic_pt_pages) : (slot_by_pd += 1) {
            if (slotPtPa(space, slot_by_pd) == pt_pa) {
                return slot_by_pd;
            }
        }
    }

    // Fallback to metadata if pd entry was not initialized yet.
    var slot_meta: usize = 0;
    while (slot_meta < UserAddressSpace.max_dynamic_pt_pages) : (slot_meta += 1) {
        if (space.pt_page_pd_index[slot_meta] == UserAddressSpace.no_pd_index) continue;
        if (space.pt_page_pd_index[slot_meta] == pd_index) {
            // Self-heal stale/missing PD entry from metadata.
            const pt_pa = slotPtPa(space, slot_meta);
            space.pd[pd_index] = pt_pa | runtime.page_present | runtime.page_rw | runtime.page_user;
            const next_len = slot_meta + 1;
            if (next_len > space.pt_page_used_len) {
                space.pt_page_used_len = @intCast(next_len);
            }
            return slot_meta;
        }
    }
    return null;
}

fn seedPtSlotFromExistingPd(space: *UserAddressSpace, slot: usize, existing_pde: u64) void {
    const page_ps: u64 = 1 << 7;
    const pt_page: *[512]u64 = &space.pt_pages[slot];
    @memset(pt_page[0..], 0);
    if ((existing_pde & runtime.page_present) == 0) return;
    if ((existing_pde & page_ps) == 0) {
        if ((existing_pde & runtime.page_user) != 0) return;
        const src_pt_addr = existing_pde & runtime.page_addr_mask;
        if (src_pt_addr == 0 or src_pt_addr >= runtime.physical_map_limit) return;
        const src_pt: *const [512]u64 = @ptrFromInt(src_pt_addr);
        var copy_index: usize = 0;
        while (copy_index < runtime.page_entries) : (copy_index += 1) {
            pt_page[copy_index] = src_pt[copy_index] & ~runtime.page_user;
        }
        return;
    }

    const two_mib_mask = ~@as(u64, 0x1F_FFFF);
    const page_base = existing_pde & runtime.page_addr_mask & two_mib_mask;
    const pte_flags = (existing_pde & 0xFFF) & ~page_ps & ~runtime.page_user;
    var pt_index: usize = 0;
    while (pt_index < runtime.page_entries) : (pt_index += 1) {
        const paddr = page_base + @as(u64, @intCast(pt_index)) * 4096;
        pt_page[pt_index] = paddr | pte_flags;
    }
}

fn ensurePtSlotForPd(space: *UserAddressSpace, pd_index: usize) ?usize {
    if (pd_index >= 512) return null;
    if (findPtSlotForPd(space, pd_index)) |existing| return existing;

    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        if (space.pt_page_pd_index[slot] != UserAddressSpace.no_pd_index) continue;
        if (pdIndexForPtSlot(space, slot) != null) continue;
        if (slotHasAnyMappedPaddr(space, slot)) continue;
        break;
    }
    if (slot >= UserAddressSpace.max_dynamic_pt_pages) return null;
    space.pt_page_pd_index[slot] = @intCast(pd_index);
    const existing_pde = space.pd[pd_index];
    seedPtSlotFromExistingPd(space, slot, existing_pde);
    const pt_pa = @intFromPtr(&space.pt_pages[slot]);
    space.pd[pd_index] = pt_pa | runtime.page_present | runtime.page_rw | runtime.page_user;
    const next_len = slot + 1;
    if (next_len > space.pt_page_used_len) {
        space.pt_page_used_len = @intCast(next_len);
    }
    return slot;
}

noinline fn clearAliasMappings(
    space: *UserAddressSpace,
    principal: kernel.PrincipalId,
    paddr: u64,
    map_slot: usize,
    pt_index: usize,
) void {
    const slot_limit = ptSlotScanLimit(space);
    var slot: usize = 0;
    while (slot < slot_limit) : (slot += 1) {
        const slot_pd_index = pdIndexForPtSlot(space, slot) orelse continue;
        const pt_page = &space.pt_pages[slot];
        var i: usize = 0;
        while (i < runtime.page_entries) : (i += 1) {
            if (slot == map_slot and i == pt_index) continue;
            const entry = pt_page[i];
            if ((entry & runtime.page_addr_mask) != paddr) continue;
            if (entry == 0) continue;
            pt_page[i] = 0;
            const alias_va = aliasVaForPdPt(slot_pd_index, i);
            _ = releaseUserMapping(principal, alias_va, 1);
            runtime.flush_user_tlb_for_principal_va(principal, alias_va);
        }
    }
}

pub fn issuePageFaultCapability(
    principal: kernel.PrincipalId,
    frame: *const interrupts.ExceptionTrapFrame,
    cr2: u64,
) ?PageFaultCapability {
    const ec = frame.error_code;
    const user_mode = (ec & (1 << 2)) != 0;
    if (!user_mode) return null;
    if (!isUserCanonicalVa(cr2)) return null;

    const fault_page_va = pageAlignDown(cr2);
    return .{
        .principal = principal,
        .fault_va = cr2,
        .fault_page_va = fault_page_va,
        .fault_rip = frame.rip,
        .present_violation = (ec & (1 << 0)) != 0,
        .write_access = (ec & (1 << 1)) != 0,
        .instruction_fetch = (ec & (1 << 4)) != 0,
        .candidate_paddr = lookupUserMappedPaddrForVa(principal, fault_page_va),
    };
}

pub fn resolvePageFaultCapability(state: *const kernel.KernelState, pf_cap: PageFaultCapability) bool {
    if (pf_cap.present_violation) return false;
    const candidate_paddr = pf_cap.candidate_paddr orelse return false;
    const cap = state.getTableConst(pf_cap.principal).find(candidate_paddr) orelse return false;
    if (!cap.rights.cpu_read) return false;
    if (pf_cap.write_access and !cap.rights.cpu_write) return false;
    if (pf_cap.instruction_fetch) return false;

    return mapUserPageFromCapability(
        state,
        pf_cap.principal,
        pf_cap.fault_page_va,
        candidate_paddr,
        cap.rights.cpu_write,
    );
}

pub fn mapUserPageFromCapability(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    va: u64,
    paddr: u64,
    writable: bool,
) bool {
    if (!runtime_ready) return false;
    const space = getUserSpace(principal) orelse return false;
    if ((va & 0xFFF) != 0) return false;
    if ((paddr & 0xFFF) != 0) return false;
    if (paddr >= runtime.physical_map_limit) return false;

    const pd_index = userPdIndexForVa(va) orelse return false;
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const map_slot = ensurePtSlotForPd(space, pd_index) orelse return false;

    const cap = state.getTableConst(principal).find(paddr) orelse return false;
    if (!cap.rights.cpu_read) return false;
    if (writable and !cap.rights.cpu_write) return false;

    const map_pt_page: *[512]u64 = &space.pt_pages[map_slot];
    const old_entry = map_pt_page[pt_index];
    if ((old_entry & runtime.page_present) != 0 and (old_entry & runtime.page_user) != 0) return false;
    if (!reserveUserMapping(principal, va, 1, .capability_page, writable)) return false;

    clearAliasMappings(space, principal, paddr, map_slot, pt_index);

    map_pt_page[pt_index] = paddr | runtime.page_present | runtime.page_user | (if (writable) runtime.page_rw else 0);
    runtime.flush_user_tlb_for_principal_va(principal, va);
    return true;
}

pub fn mapTrustedUserPage(
    principal: kernel.PrincipalId,
    va: u64,
    paddr: u64,
    writable: bool,
) bool {
    if (!runtime_ready) return false;
    const space = getUserSpace(principal) orelse return false;
    if ((va & 0xFFF) != 0) return false;
    if ((paddr & 0xFFF) != 0) return false;
    if (paddr >= runtime.physical_map_limit) return false;

    const pd_index = userPdIndexForVa(va) orelse return false;
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const map_slot = ensurePtSlotForPd(space, pd_index) orelse return false;

    const map_pt_page: *[512]u64 = &space.pt_pages[map_slot];
    const old_entry = map_pt_page[pt_index];
    if ((old_entry & runtime.page_present) != 0 and (old_entry & runtime.page_user) != 0) return false;
    if (!reserveUserMapping(principal, va, 1, .capability_page, writable)) return false;

    clearAliasMappings(space, principal, paddr, map_slot, pt_index);
    map_pt_page[pt_index] = paddr | runtime.page_present | runtime.page_user | (if (writable) runtime.page_rw else 0);
    runtime.flush_user_tlb_for_principal_va(principal, va);
    return true;
}

fn mapUserPageFromCapabilityNoAlias(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    va: u64,
    paddr: u64,
    writable: bool,
) bool {
    if (!runtime_ready) return false;
    const space = getUserSpace(principal) orelse return false;
    if ((va & 0xFFF) != 0) return false;
    if ((paddr & 0xFFF) != 0) return false;
    if (paddr >= runtime.physical_map_limit) return false;

    const pd_index = userPdIndexForVa(va) orelse return false;
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const map_slot = ensurePtSlotForPd(space, pd_index) orelse return false;

    const cap = state.getTableConst(principal).find(paddr) orelse return false;
    if (!cap.rights.cpu_read) return false;
    if (writable and !cap.rights.cpu_write) return false;

    const map_pt_page: *[512]u64 = &space.pt_pages[map_slot];
    const old_entry = map_pt_page[pt_index];
    if ((old_entry & runtime.page_present) != 0 and (old_entry & runtime.page_user) != 0) return false;
    if (!reserveUserMapping(principal, va, 1, .capability_page, writable)) return false;
    map_pt_page[pt_index] = paddr | runtime.page_present | runtime.page_user | (if (writable) runtime.page_rw else 0);
    runtime.flush_user_tlb_for_principal_va(principal, va);
    return true;
}

pub fn mapUserPagesFromCapabilityBatch(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    base_va: u64,
    paddrs: []const u64,
    writable: bool,
) bool {
    if (!runtime_ready) return false;
    if ((base_va & 0xFFF) != 0) return false;

    var i: usize = 0;
    while (i < paddrs.len) : (i += 1) {
        const paddr = paddrs[i];
        if ((paddr & 0xFFF) != 0) return false;
        if (paddr >= runtime.physical_map_limit) return false;

        var j: usize = 0;
        while (j < i) : (j += 1) {
            if (paddrs[j] == paddr) return false;
        }

        const cap = state.getTableConst(principal).find(paddr) orelse return false;
        if (!cap.rights.cpu_read) return false;
        if (writable and !cap.rights.cpu_write) return false;
    }

    i = 0;
    while (i < paddrs.len) : (i += 1) {
        const offset: u64 = @intCast(i * 4096);
        const va, const va_overflow = @addWithOverflow(base_va, offset);
        if (va_overflow != 0) return false;
        if (!mapUserPageFromCapabilityNoAlias(state, principal, va, paddrs[i], writable)) return false;
    }
    return true;
}

pub fn mapFreshUserPage(
    principal: kernel.PrincipalId,
    va: u64,
    paddr: u64,
    writable: bool,
) bool {
    if (!runtime_ready) return false;
    const space = getUserSpace(principal) orelse return false;
    if ((va & 0xFFF) != 0) return false;
    if ((paddr & 0xFFF) != 0) return false;
    if (paddr >= runtime.physical_map_limit) return false;

    const pd_index = userPdIndexForVa(va) orelse return false;
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const map_slot = ensurePtSlotForPd(space, pd_index) orelse return false;
    const map_pt_page: *[512]u64 = &space.pt_pages[map_slot];
    const old_entry = map_pt_page[pt_index];
    if ((old_entry & runtime.page_present) != 0 and (old_entry & runtime.page_user) != 0) return false;
    if (!reserveUserMapping(principal, va, 1, .fresh_page, writable)) return false;

    map_pt_page[pt_index] = paddr | runtime.page_present | runtime.page_user | (if (writable) runtime.page_rw else 0);
    runtime.flush_user_tlb_for_principal_va(principal, va);
    return true;
}

pub fn dropPresentForUserMappedPaddr(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    paddr: u64,
) bool {
    if (!runtime_ready) return false;
    const space = getUserSpace(principal) orelse return false;
    _ = state.getTableConst(principal).find(paddr) orelse return false;
    const slot_limit = ptSlotScanLimit(space);
    var slot: usize = 0;
    while (slot < slot_limit) : (slot += 1) {
        const slot_pd_index = pdIndexForPtSlot(space, slot) orelse continue;
        const pt_page: *[512]u64 = &space.pt_pages[slot];
        var i: usize = 0;
        while (i < runtime.page_entries) : (i += 1) {
            const entry = pt_page[i];
            if ((entry & runtime.page_addr_mask) != paddr) continue;
            if ((entry & runtime.page_present) == 0) continue;
            pt_page[i] = entry & ~runtime.page_present;
            const va = aliasVaForPdPt(slot_pd_index, i);
            _ = releaseUserMapping(principal, va, 1);
            runtime.flush_user_tlb_for_principal_va(principal, va);
            return true;
        }
    }
    return false;
}

noinline fn syncPageTableRightsScan(
    space: *UserAddressSpace,
    principal: kernel.PrincipalId,
    paddr: u64,
    cap: ?*const kernel.Capability,
) void {
    var kept_one = false;
    const slot_limit = ptSlotScanLimit(space);
    var slot: usize = 0;
    while (slot < slot_limit) : (slot += 1) {
        const slot_pd_index = pdIndexForPtSlot(space, slot) orelse continue;
        const pt_page: *[512]u64 = &space.pt_pages[slot];
        var i: usize = 0;
        while (i < runtime.page_entries) : (i += 1) {
            const old_entry = pt_page[i];
            const mapped_paddr = old_entry & runtime.page_addr_mask;
            if (mapped_paddr != paddr) continue;

            var new_entry: u64 = 0;
            if (cap) |c| {
                if (c.rights.cpu_read and !kept_one) {
                    new_entry = paddr | runtime.page_user | runtime.page_present;
                    if (c.rights.cpu_write) new_entry |= runtime.page_rw;
                    kept_one = true;
                }
            }

            if (new_entry == old_entry) continue;
            pt_page[i] = new_entry;
            const va = aliasVaForPdPt(slot_pd_index, i);
            if (new_entry == 0) {
                _ = releaseUserMapping(principal, va, 1);
            }
            runtime.flush_user_tlb_for_principal_va(principal, va);
        }
    }
}

pub fn principalHasMappedPaddr(principal: kernel.PrincipalId, paddr: u64) bool {
    if (!runtime_ready) return false;
    const space = getUserSpace(principal) orelse return false;
    const slot_limit = ptSlotScanLimit(space);
    var slot: usize = 0;
    while (slot < slot_limit) : (slot += 1) {
        const slot_pd_index = pdIndexForPtSlot(space, slot) orelse {
            continue;
        };
        _ = slot_pd_index;
        const pt_page: *const [512]u64 = &space.pt_pages[slot];
        var i: usize = 0;
        while (i < runtime.page_entries) : (i += 1) {
            const entry = pt_page[i];
            if ((entry & runtime.page_present) == 0) continue;
            if ((entry & runtime.page_addr_mask) == paddr) return true;
        }
    }
    return false;
}

pub fn syncPageTableRightsForPrincipalPaddr(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    paddr: u64,
) void {
    if (!runtime_ready) return;
    const space = getUserSpace(principal) orelse return;
    const cap = state.getTableConst(principal).find(paddr);
    syncPageTableRightsScan(space, principal, paddr, cap);
}

pub fn syncPageTableRightsForPaddr(state: *const kernel.KernelState, paddr: u64) void {
    if (!runtime_ready) return;

    var process_index: usize = 0;
    while (process_index < runtime.user_spaces.len) : (process_index += 1) {
        const principal = principalFromProcessIndex(process_index) orelse continue;
        syncPageTableRightsForPrincipalPaddr(state, principal, paddr);
    }
}

fn logWrite(text: []const u8) void {
    if (!runtime_ready) return;
    runtime.serial_write(text);
}

fn logHex(value: u64) void {
    if (!runtime_ready) return;
    runtime.print_hex(value);
}

pub fn dumpPrincipalCaps(state: *const kernel.KernelState, principal: kernel.PrincipalId, label: []const u8) void {
    logWrite(label);
    logWrite(" caps:\n");

    const table = state.getTableConst(principal);
    if (table.len == 0) {
        logWrite("  none\n");
        return;
    }

    var i: usize = 0;
    while (i < table.len) : (i += 1) {
        const cap = table.get(i) orelse break;
        logWrite("  ");
        logHex(cap.paddr);
        logWrite(" id=");
        logHex(cap.cap_id);
        logWrite(" root=");
        logHex(cap.root_cap_id);
        logWrite(" parent=");
        logHex(cap.parent_cap_id);
        logWrite(" rights=");
        if (cap.rights.cpu_read) logWrite("r");
        if (cap.rights.cpu_write) logWrite("w");
        if (cap.rights.dma) logWrite("d");
        if (cap.rights.grant) logWrite("g");
        logWrite("\n");
    }
}

pub fn dumpCapabilityView(state: *const kernel.KernelState) void {
    var i: usize = 0;
    while (i < kernel.process_count) : (i += 1) {
        const principal = kernel.processPrincipalFromIndex(i) orelse unreachable;
        dumpPrincipalCaps(state, principal, kernel.principalLabel(principal));
    }
    dumpPrincipalCaps(state, .Device0, "Device0");
}

pub fn dumpPrincipalEndpoints(state: *const kernel.KernelState, principal: kernel.PrincipalId, label: []const u8) void {
    logWrite(label);
    logWrite(" endpoints:\n");
    const table = state.getEndpointTableConst(principal);
    if (table.len == 0) {
        logWrite("  none\n");
        return;
    }

    var i: usize = 0;
    while (i < table.len) : (i += 1) {
        const ep = table.caps[i];
        logWrite("  ep=");
        logHex(ep.endpoint_id);
        logWrite(" -> ");
        logWrite(runtime.principal_label(ep.target));
        logWrite("\n");
    }
}

fn testSerialWrite(_: []const u8) void {}
fn testPrintHex(_: u64) void {}
fn testPrincipalLabel(_: kernel.PrincipalId) []const u8 {
    return "test";
}
fn testFlushUserTlb(_: kernel.PrincipalId, _: u64) void {}

test "lookupUserMappedPaddrForVa rejects supervisor-only entries" {
    var spaces = [_]UserAddressSpace{.{}} ** 1;
    spaces[0].pt_page_pd_index[0] = 0;
    spaces[0].pt_pages[0][1] = 0x1234_5000 | 0x1 | 0x2;

    init(.{
        .user_spaces = spaces[0..],
        .user_va = 0,
        .physical_map_limit = 0x1_0000_0000,
        .page_entries = 512,
        .page_addr_mask = 0x000f_ffff_ffff_f000,
        .page_present = 0x1,
        .page_rw = 0x2,
        .page_user = 0x4,
        .canonical_user_limit_exclusive = 0x0000_8000_0000_0000,
        .serial_write = testSerialWrite,
        .print_hex = testPrintHex,
        .principal_label = testPrincipalLabel,
        .flush_user_tlb_for_principal_va = testFlushUserTlb,
    });

    try std.testing.expect(lookupUserMappedPaddrForVa(.Process0, 0x1000) == null);

    spaces[0].pt_pages[0][1] |= 0x4;
    try std.testing.expectEqual(@as(?u64, 0x1234_5000), lookupUserMappedPaddrForVa(.Process0, 0x1000));
}

test "user reservations reject overlap and split on release" {
    var spaces = [_]UserAddressSpace{.{}} ** 1;

    init(.{
        .user_spaces = spaces[0..],
        .user_va = 0,
        .physical_map_limit = 0x1_0000_0000,
        .page_entries = 512,
        .page_addr_mask = 0x000f_ffff_ffff_f000,
        .page_present = 0x1,
        .page_rw = 0x2,
        .page_user = 0x4,
        .canonical_user_limit_exclusive = 0x0000_8000_0000_0000,
        .serial_write = testSerialWrite,
        .print_hex = testPrintHex,
        .principal_label = testPrincipalLabel,
        .flush_user_tlb_for_principal_va = testFlushUserTlb,
    });

    try std.testing.expect(reserveUserMapping(.Process0, 0x2000, 4, .linear_region, true));
    try std.testing.expect(!reserveUserMapping(.Process0, 0x3000, 1, .fresh_page, true));
    try std.testing.expect(releaseUserMapping(.Process0, 0x3000, 1));
    try std.testing.expect(reserveUserMapping(.Process0, 0x3000, 1, .fresh_page, true));
    try std.testing.expect(!reserveUserMapping(.Process0, 0x5000, 1, .fresh_page, true));
}

test "findFreeUserMappingRange skips active reservations" {
    var spaces = [_]UserAddressSpace{.{}} ** 1;

    init(.{
        .user_spaces = spaces[0..],
        .user_va = 0x2000_0000,
        .physical_map_limit = 0x1_0000_0000,
        .page_entries = 512,
        .page_addr_mask = 0x000f_ffff_ffff_f000,
        .page_present = 0x1,
        .page_rw = 0x2,
        .page_user = 0x4,
        .canonical_user_limit_exclusive = 0x0000_8000_0000_0000,
        .serial_write = testSerialWrite,
        .print_hex = testPrintHex,
        .principal_label = testPrincipalLabel,
        .flush_user_tlb_for_principal_va = testFlushUserTlb,
    });

    const first = findFreeUserMappingRange(.Process0, 2, 1).?;
    try std.testing.expectEqual(@as(u64, 0x2300_0000), first);
    try std.testing.expect(reserveUserMapping(.Process0, first, 2, .linear_region, true));
    const second = findFreeUserMappingRange(.Process0, 2, 1).?;
    try std.testing.expectEqual(@as(u64, 0x2300_2000), second);
    try std.testing.expectEqual(@as(u64, 0x2300_4000 >> 12), spaces[0].next_dynamic_map_page);

    try std.testing.expect(releaseUserMapping(.Process0, first, 2));
    spaces[0].next_dynamic_map_page = 0x3bff_f000 >> 12;
    try std.testing.expect(reserveUserMapping(.Process0, 0x3bff_f000, 1, .linear_region, true));
    const wrapped = findFreeUserMappingRange(.Process0, 2, 1).?;
    try std.testing.expectEqual(first, wrapped);

    resetUserReservations(&spaces[0]);
    try std.testing.expectEqual(@as(u64, 0), spaces[0].next_dynamic_map_page);
}

test "user reservations merge adjacent compatible ranges" {
    var spaces = [_]UserAddressSpace{.{}} ** 1;

    init(.{
        .user_spaces = spaces[0..],
        .user_va = 0,
        .physical_map_limit = 0x1_0000_0000,
        .page_entries = 512,
        .page_addr_mask = 0x000f_ffff_ffff_f000,
        .page_present = 0x1,
        .page_rw = 0x2,
        .page_user = 0x4,
        .canonical_user_limit_exclusive = 0x0000_8000_0000_0000,
        .serial_write = testSerialWrite,
        .print_hex = testPrintHex,
        .principal_label = testPrincipalLabel,
        .flush_user_tlb_for_principal_va = testFlushUserTlb,
    });

    try std.testing.expect(reserveUserMapping(.Process0, 0x4000, 1, .linear_region, true));
    try std.testing.expect(reserveUserMapping(.Process0, 0x6000, 1, .linear_region, true));
    try std.testing.expect(reserveUserMapping(.Process0, 0x5000, 1, .linear_region, true));

    var active_count: usize = 0;
    var merged: ?UserAddressSpace.Reservation = null;
    for (spaces[0].reservations) |reservation| {
        if (!reservation.active) continue;
        active_count += 1;
        merged = reservation;
    }

    try std.testing.expectEqual(@as(usize, 1), active_count);
    try std.testing.expectEqual(@as(u64, 0x4000), merged.?.base_va);
    try std.testing.expectEqual(@as(u32, 3), merged.?.page_count);
}

test "ipc buffer token encode decode and rights subset" {
    const ipc_buffer_abi = @import("kernel_abi_root").ipc_buffer_abi;
    const token = ipc_buffer_abi.encodeIpcBufferToken(0x1234);
    try std.testing.expectEqual(@as(?u64, 0x1234), ipc_buffer_abi.decodeIpcBufferToken(token));
    try std.testing.expectEqual(@as(?u64, null), ipc_buffer_abi.decodeIpcBufferToken(0x1234));

    var state: kernel.KernelState = .{};
    try std.testing.expect(state.ensureProcessDescriptor(.Process0, "p0"));
    try state.installCap(.Process0, 0x4000, .{ .cpu_read = true, .cpu_write = true, .dma = false, .grant = true });

    const root_id = try state.createIpcBufferFromPage(.Process0, 0x4000, .{
        .read = true,
        .write = true,
        .map = true,
        .grant = true,
    }, .request);
    try std.testing.expect(root_id != 0);
    try state.installCap(.Process0, 0x7000, .{ .cpu_read = true, .cpu_write = false, .dma = false, .grant = true });
    try std.testing.expectError(kernel.KernelError.InvalidState, state.createIpcBufferFromPage(.Process0, 0x7000, .{
        .read = true,
        .write = true,
        .map = true,
        .grant = true,
    }, .request));
}

test "ipc buffer grant tree and wrong-kind accept does not consume page transfer" {
    var state: kernel.KernelState = .{};
    try std.testing.expect(state.ensureProcessDescriptor(.Process0, "p0"));
    try std.testing.expect(state.ensureProcessDescriptor(.Process1, "p1"));

    try state.installCap(.Process0, 0x5000, .{ .cpu_read = true, .cpu_write = true, .dma = false, .grant = true });
    const root_id = try state.createIpcBufferFromPage(.Process0, 0x5000, .{
        .read = true,
        .write = true,
        .map = true,
        .grant = true,
    }, .response);
    const child_id = try state.grantIpcBufferCap(.Process0, .Process1, root_id, .{
        .read = true,
        .write = false,
        .map = true,
        .grant = false,
    });
    const child = state.getIpcBufferTableConst(.Process1).findByCapId(child_id) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(@as(u64, 0x5000), child.paddr);
    try std.testing.expectEqual(root_id, child.root_cap_id);
    try std.testing.expectEqual(root_id, child.parent_cap_id);
    try std.testing.expect(child.rights.read);
    try std.testing.expect(!child.rights.write);
    try std.testing.expect(child.rights.map);
    try std.testing.expect(!child.rights.grant);

    try state.installEndpoint(.Process0, 0x80, .Process1);
    try state.installCap(.Process0, 0x6000, .{ .cpu_read = true, .cpu_write = true, .dma = false, .grant = true });
    try state.shareCapOnEndpoint(.Process0, 0x80, 0x6000);
    const page_transfer_id = try state.recvCap(.Process1);
    try std.testing.expectError(kernel.KernelError.MailboxEmpty, state.acceptIpcBufferTransfer(.Process1, page_transfer_id));
    try std.testing.expectEqual(@as(u64, 0x6000), try state.acceptCapTransfer(.Process1, page_transfer_id));
}
