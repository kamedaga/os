const std = @import("std");
const vm = @import("memory/user_vm.zig");
const kernel = @import("kernel.zig");
const mtrr = @import("arch/x86_64/mtrr.zig");

test "MMIO overlay overflow storage tests" {
    _ = @import("memory/pt_storage.zig");
}

const owner = kernel.processPrincipalFromIndex(0).?;
const base: u64 = 1 << 39;
const physical: u64 = 0x8000_0000;
const pt_storage = @import("memory/pt_storage.zig");
var pt_context: u8 = 0;
var pt_alloc_calls: usize = 0;
var pt_fail_at: usize = 0;
var pt_live_pages: usize = 0;
var pt_release_after_flush: ?usize = null;
var teardown_check: ?struct { index: usize, pages: usize } = null;
fn allocatePt(_: *anyopaque) ?pt_storage.PagePointer {
    pt_alloc_calls += 1;
    if (pt_alloc_calls == pt_fail_at) return null;
    const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096) catch return null;
    pt_live_pages += 1;
    return @ptrCast(bytes.ptr);
}
fn releasePt(_: *anyopaque, page: pt_storage.PagePointer) bool {
    if (pt_release_after_flush) |before| std.debug.assert(flushes > before);
    const bytes: []align(4096) u8 = page;
    std.testing.allocator.free(bytes);
    pt_live_pages -= 1;
    return true;
}
fn ptAllocator() pt_storage.Allocator {
    return .{ .context = &pt_context, .allocate = allocatePt, .release = releasePt };
}
// Synthetic host tables are never installed in a CPU; no hardware references
// survive a test. Production callers require completed detach/shootdown.
fn cleanup() void {
    pt_release_after_flush = null;
    for (&spaces) |*space| space.pt_store.freeDetached(ptAllocator());
    std.debug.assert(pt_live_pages == 0);
}
var spaces: [2]vm.UserAddressSpace align(4096) = .{ .{}, .{} };
var table: vm.UserAddressSpaceTable = undefined;
var flushes: usize = 0;
var translations: usize = 0;
var fail_translation: usize = 0;
var reclaim_flush_first: ?usize = null;
var cow_flush_check: ?struct {
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    old_ref: kernel.NativeCowTableRef,
    free_pages: usize,
    checked: *bool,
} = null;

fn translate(address: usize) ?u64 {
    translations += 1;
    if (translations == fail_translation) return null;
    std.debug.assert(address & 4095 == 0);
    return address;
}
fn seed(page: []u64) void {
    @memset(page, 0);
}
fn flushPage(_: kernel.PrincipalId, _: u64) void {
    flushes += 1;
}
fn flushRange(_: kernel.PrincipalId, va: u64, bytes: usize) void {
    flushes += 1;
    if (teardown_check) |check| {
        std.debug.assert(pt_live_pages == check.pages);
        std.debug.assert(std.mem.allEqual(u64, spaces[check.index].pml4[0..256], 0));
        std.debug.assert(va == 4096 and bytes == (1 << 47) - 4096);
    }
    if (reclaim_flush_first) |first| {
        std.debug.assert(va == base + (@as(u64, @intCast(first)) << 21));
        std.debug.assert(bytes == (512 - first) * (1 << 21));
        // All parents are gone, but no storage is reusable before this
        // synchronous shootdown callback returns.
        std.debug.assert(usedPtSlots() == 512);
        for (first..512) |slot| {
            std.debug.assert(spaces[0].pd_pages[0][slot] == 0);
            std.debug.assert(spaces[0].ptPdIndex(slot).* == slot);
        }
    }
    if (cow_flush_check) |check| {
        std.debug.assert(!check.state.nativeCowTableIsUnique(check.old_ref));
        std.debug.assert(check.free_list.len == check.free_pages);
        std.debug.assert(vm.lookupUserMappedPaddrForVa(owner, base) == null);
        check.checked.* = true;
    }
}
fn init() void {
    cleanup();
    for (&spaces) |*space| {
        @import("memory/address_space.zig").initializeUserAddressSpaceStorage(space);
        space.lock_state = .{};
    }
    pt_alloc_calls = 0;
    pt_fail_at = 0;
    teardown_check = null;
    table.init(&spaces);
    flushes = 0;
    translations = 0;
    fail_translation = 0;
    reclaim_flush_first = null;
    cow_flush_check = null;
    vm.init(.{
        .pt_allocator = ptAllocator(),
        .user_spaces = &table,
        .four_gib = 1 << 32,
        .physical_map_limit = 1 << 52,
        .user_low_va = 4096,
        .user_top_va = 1 << 47,
        .dynamic_map_base_va = base,
        .dynamic_map_end_va = base + (1 << 30),
        .user_va = 0x400000,
        .user_stack_page_va = 0x500000,
        .canonical_user_limit_exclusive = 1 << 47,
        .page_entries = 512,
        .page_addr_mask = 0x000f_ffff_ffff_f000,
        .page_present = 1,
        .page_rw = 2,
        .page_user = 4,
        .page_ps = 128,
        .page_nx = @as(u64, 1) << 63,
        .flush_user_tlb_for_principal_va = flushPage,
        .flush_user_tlb_for_principal_range = flushRange,
        .kernel_pointer_paddr = translate,
        .seed_user_pml4_with_kernel = seed,
        .seed_user_pdp_with_kernel_identity = seed,
        .seed_user_pd_with_kernel_identity = seed,
    });
}

test "MMIO overlay PT storage accessors grow without moving existing pages" {
    init();
    defer cleanup();
    const space = &spaces[0];
    const read_only: *const vm.UserAddressSpace = space;
    try std.testing.expectEqual(@as(usize, 0), read_only.ptCapacity());
    const first_slot = vm.ensureUserPtSlotForPd(space, 1, 0, 0).?;
    const first_page = space.ptPage(first_slot);
    first_page[7] = 23;
    for (1..65) |pd| _ = vm.ensureUserPtSlotForPd(space, 1, 0, pd).?;
    try std.testing.expectEqual(@as(usize, 128), read_only.ptCapacity());
    try std.testing.expectEqual(first_page, space.ptPage(first_slot));
    try std.testing.expectEqual(@as(u64, 23), read_only.ptPage(first_slot)[7]);
    comptime {
        std.debug.assert(@typeInfo(@TypeOf(read_only.ptPage(0))).pointer.is_const);
        std.debug.assert(@typeInfo(@TypeOf(read_only.ptPdIndex(0))).pointer.is_const);
        std.debug.assert(!@typeInfo(@TypeOf(space.ptPage(0))).pointer.is_const);
    }
    try std.testing.expectEqual(@as(?usize, 65), read_only.firstFreePtSlot());
    // Metadata-only free slots need not already own a PT page.
    try std.testing.expect(space.pt_store.descriptor(65).?.page == null);
}

test "MMIO overlay dynamic PTs cross 64 and 512 with bounded retirement batches" {
    for ([_]usize{ 65, 513, 640 }) |count| {
        init();
        defer cleanup();
        for (0..count) |i| {
            const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, i / 512, i % 512).?;
            spaces[0].ptPage(slot)[0] = physical | 7;
        }
        try std.testing.expectEqual(count, usedPtSlots());
        const metadata_pages = (count + 63) / 64;
        try std.testing.expectEqual(count + metadata_pages, pt_live_pages);
        pt_release_after_flush = flushes;
        try std.testing.expect(vm.unmapPresentRemoteUserRegion(owner, base, count * (1 << 21)));
        try std.testing.expectEqual(@as(usize, 0), usedPtSlots());
        try std.testing.expectEqual(metadata_pages, pt_live_pages);
        try std.testing.expect(flushes >= metadata_pages);
        // Freed slots are reused and lazily acquire new bodies, not new banks.
        const capacity = spaces[0].ptCapacity();
        try std.testing.expectEqual(@as(?usize, 0), vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0));
        try std.testing.expectEqual(capacity, spaces[0].ptCapacity());
        try std.testing.expectEqual(metadata_pages + 1, pt_live_pages);
    }
}

test "MMIO overlay PT lookup distinguishes tuples holes and reused bank boundaries" {
    init();
    defer cleanup();
    const space = &spaces[0];
    for (0..513) |i| {
        const slot = vm.ensureUserPtSlotForPd(space, 1, i / 512, i % 512).?;
        space.ptPage(slot)[0] = (physical + i * 4096) | 7;
    }
    // The same PD number in another PML4 must not alias either PDP's slot.
    const other = vm.ensureUserPtSlotForPd(space, 2, 0, 0).?;
    space.ptPage(other)[0] = (physical + 0x800000) | 7;
    const before_alloc = pt_alloc_calls;
    for ([_]usize{ 0, 63, 64, 511, 512 }) |i| {
        const va = base + (i << 21);
        try std.testing.expectEqual(@as(?u64, physical + i * 4096), vm.lookupUserMappedPaddrForVa(owner, va));
    }
    try std.testing.expectEqual(@as(?u64, physical + 0x800000), vm.lookupUserMappedPaddrForVa(owner, 2 << 39));
    try std.testing.expectEqual(before_alloc, pt_alloc_calls);

    // Each missing index independently makes a descriptor unavailable.
    const entry = space.pt_store.descriptor(64).?;
    for ([_]*u16{ &entry.pml4, &entry.pdp, &entry.pd }) |index| {
        const saved = index.*;
        index.* = vm.UserAddressSpace.no_pd_index;
        try std.testing.expectEqual(@as(?u64, null), vm.lookupUserMappedPaddrForVa(owner, base + (64 << 21)));
        index.* = saved;
    }
    const retired_va = base + (63 << 21);
    try std.testing.expect(vm.unmapPresentRemoteUserRegion(owner, retired_va, 1 << 21));
    try std.testing.expectEqual(@as(?u64, null), vm.lookupUserMappedPaddrForVa(owner, retired_va));
    const capacity = space.ptCapacity();
    const reused = vm.ensureUserPtSlotForPd(space, 2, 1, 63).?;
    try std.testing.expectEqual(@as(usize, 63), reused);
    try std.testing.expectEqual(capacity, space.ptCapacity());
    space.ptPage(reused)[0] = (physical + 0x900000) | 7;
    try std.testing.expectEqual(@as(?u64, physical + 0x900000), vm.lookupUserMappedPaddrForVa(owner, (2 << 39) + (1 << 30) + (63 << 21)));
    try std.testing.expectEqual(@as(?u64, null), vm.lookupUserMappedPaddrForVa(owner, retired_va));
    for ([_][3]usize{ .{ 256, 0, 0 }, .{ 1, 512, 0 }, .{ 1, 0, 512 }, .{ 0xffff, 0xffff, 0xffff } }) |indices|
        try std.testing.expectEqual(@as(?usize, null), vm.ensureUserPtSlotForPd(space, indices[0], indices[1], indices[2]));
}

test "MMIO overlay dynamic PT clear and exec detach before storage return" {
    init();
    defer cleanup();
    const target = kernel.processPrincipalFromIndex(1).?;
    // The source must survive replacement of the destination, including a
    // page beyond the former 512-table limit.
    for (0..513) |i| {
        const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, i / 512, i % 512).?;
        spaces[0].ptPage(slot)[0] = physical | 7;
    }
    const last = spaces[0].ptPage(512);
    _ = vm.ensureUserPtSlotForPd(&spaces[1], 2, 0, 0).?;
    spaces[1].ptPage(0)[0] = (physical + 4096) | 7;
    spaces[1].cr3 = @intFromPtr(&spaces[1].pml4);
    const old_target_page = spaces[1].ptPage(0);
    const old_target_pml4 = spaces[1].pml4;
    const initial_pages = pt_live_pages;
    const initial_flushes = flushes;
    fail_translation = translations + 1;
    try std.testing.expect(!vm.cloneAddressSpaceMetadataForFork(owner, target));
    try std.testing.expectEqual(initial_pages, pt_live_pages);
    try std.testing.expectEqual(initial_flushes, flushes);
    try std.testing.expectEqual(old_target_page, spaces[1].ptPage(0));
    try std.testing.expectEqualSlices(u64, &old_target_pml4, &spaces[1].pml4);
    fail_translation = 0;
    teardown_check = .{ .index = 1, .pages = pt_live_pages };
    pt_release_after_flush = flushes;
    try std.testing.expect(vm.cloneAddressSpaceMetadataForFork(owner, target));
    teardown_check = null;
    try std.testing.expectEqual(initial_pages - 2, pt_live_pages);
    try std.testing.expectEqual(@as(usize, 0), spaces[1].ptCapacity());
    try std.testing.expectEqual(last, spaces[0].ptPage(512));
    try std.testing.expectEqual(@as(u64, physical | 7), last[0]);
    try std.testing.expect(spaces[1].pml4[0] != 0);
    spaces[0].cr3 = @intFromPtr(&spaces[0].pml4);
    teardown_check = .{ .index = 0, .pages = pt_live_pages };
    pt_release_after_flush = flushes;
    vm.clearUserAddressSpace(owner);
    teardown_check = null;
    try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
    try std.testing.expectEqual(@as(usize, 0), spaces[0].ptCapacity());
}

test "MMIO overlay dynamic PT journal boundary rolls back after 638 new tables" {
    for ([_]usize{ 637, 638 }) |count| {
        init();
        defer cleanup();
        const before = try snapshot();
        defer destroySnapshot(before);
        // A contiguous range needs one PDP, two PDs, and count PT links.
        const ok = vm.mapUserMmioOverlay(owner, base, physical, count * (1 << 21), .{ .read = true });
        if (count == 637) {
            try std.testing.expect(ok);
            try std.testing.expectEqual(count, usedPtSlots());
            try std.testing.expect(vm.unmapUserMmioOverlay(owner, base, physical, count * (1 << 21)));
            try noTables();
        } else {
            try std.testing.expect(!ok);
            try unchanged(before);
            try noTables();
            // Only descriptor banks remain owned; all provisional PT bodies
            // were returned after the rollback barrier.
            try std.testing.expectEqual(spaces[0].ptCapacity() / 64, pt_live_pages);
        }
    }
}

test "MMIO overlay COW invalidates before releasing shared dirty backing" {
    init();
    defer cleanup();
    const runtime_storage = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(runtime_storage);
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage));
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    try state.initFromDetectedRegionsInPlace(1);
    var free_list = kernel.FreePageList{};
    try free_list.appendContiguousRange(0, physical, 16);
    _ = try state.createAnonymousVmaWithPages(owner, base, 4096, .{ .read = true, .write = true }, .{ .read = true, .write = true }, .{ .private = true, .anonymous = true, .fork_cow = true }, &free_list);
    const initial = state.ensureNativeVmaCowMappingLockedSlow(owner, base, true, false, &free_list) orelse return error.TestUnexpectedResult;
    const old_ref = state.vmaEntryForVaConst(owner, base).?.cow_table;
    const child = kernel.processPrincipalFromIndex(1).?;
    const before_fork = free_list.len;
    try state.cloneVmaTableForFork(owner, child);
    state.markForkCowVmasCommitted(owner);
    try std.testing.expectEqual(before_fork, free_list.len);
    try std.testing.expect(!state.dirtyCowMappingProt(state.vmaEntryForVaConst(child, base).?).write);
    try std.testing.expectEqual(initial.paddr, state.entryDirtyPagePaddr(state.vmaEntryForVaConst(child, base).?, base).?);
    try std.testing.expect(vm.mapLazyUserPaddrsWithProt(owner, base, &.{initial.paddr}, .{ .read = true }));
    var checked = false;
    cow_flush_check = .{ .state = state, .free_list = &free_list, .old_ref = old_ref, .free_pages = free_list.len, .checked = &checked };
    defer cow_flush_check = null;
    try std.testing.expect(vm.lockAddressSpace(owner));
    const mapping = @import("user_copy.zig").resolveNativeVmaCowMappingWithAddressSpaceLocked(state, &free_list, owner, base, true, false);
    vm.unlockAddressSpace(owner);
    cow_flush_check = null;
    try std.testing.expect(checked);
    const resolved = mapping orelse return error.TestUnexpectedResult;
    try std.testing.expect(resolved.paddr != initial.paddr);
    try std.testing.expect(resolved.prot.write);
    try std.testing.expectEqual(@as(u64, 0), resolved.invalidate_size_bytes);
    try std.testing.expect(state.nativeCowTableIsUnique(old_ref));
    try std.testing.expectEqual(before_fork - 1, free_list.len);
    state.releasePrincipalNativeMemory(owner, &free_list);
    state.releasePrincipalNativeMemory(child, &free_list);
    try std.testing.expectEqual(@as(usize, 16), free_list.len);
}

fn usedPtSlots() usize {
    var count: usize = 0;
    for (0..spaces[0].ptCapacity()) |slot|
        count += @intFromBool(spaces[0].ptPdIndex(slot).* != vm.UserAddressSpace.no_pd_index);
    return count;
}

fn usedSlots(indices: []const u16) usize {
    var count: usize = 0;
    for (indices) |index| count += @intFromBool(index != vm.UserAddressSpace.no_pd_index);
    return count;
}

test "MMIO overlay failure PT accounting distinguishes live empty and supervisor tables" {
    init();
    defer cleanup();
    try std.testing.expectEqual(vm.UserPtUsage{}, vm.userPtUsage(&spaces[0]));
    for (0..5) |pd| _ = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, pd).?;
    // Nonpresent, supervisor-only, user-only, mixed, and zero respectively. Nonpresent
    // entries and stale bytes in an unassigned slot must not count as live.
    spaces[0].ptPage(0).*[0] = physical | 4;
    spaces[0].ptPage(1).*[0] = physical | 3;
    spaces[0].ptPage(2).*[0] = physical | 5;
    spaces[0].ptPage(3).*[0] = physical | 7;
    spaces[0].ptPage(3).*[1] = (physical + 4096) | 3;
    _ = spaces[0].pt_store.ensurePage(5, ptAllocator()).?;
    spaces[0].ptPage(5).*[0] = physical | 7;
    const before = try snapshot();
    defer destroySnapshot(before);
    try std.testing.expectEqual(vm.UserPtUsage{
        .used = 5,
        .empty = 1,
        .user_tables = 2,
        .supervisor_only = 1,
        .nonpresent_tables = 1,
        .user_entries = 2,
        .supervisor_entries = 2,
        .nonpresent_entries = 1,
    }, vm.userPtUsage(&spaces[0]));
    try unchanged(before);
    try std.testing.expectEqual(@as(usize, 0), flushes);
}
fn noTables() !void {
    try std.testing.expectEqual(@as(usize, 0), usedPtSlots());
    try std.testing.expectEqual(@as(usize, 0), usedSlots(&spaces[0].pd_page_pdp_index));
    try std.testing.expectEqual(@as(usize, 0), usedSlots(&spaces[0].pdp_page_pml4_index));
}
const SnapshotPt = struct {
    pml4: u16,
    pdp: u16,
    pd: u16,
    address: usize,
    data: [512]u64,
};
const Snapshot = struct { space: vm.UserAddressSpace, pts: []SnapshotPt };
fn snapshot() !*Snapshot {
    const copy = try std.testing.allocator.create(Snapshot);
    errdefer std.testing.allocator.destroy(copy);
    copy.space = spaces[0];
    copy.pts = try std.testing.allocator.alloc(SnapshotPt, spaces[0].ptCapacity());
    for (copy.pts, 0..) |*item, slot| {
        const entry = spaces[0].pt_store.descriptorConst(slot).?;
        item.* = .{
            .pml4 = entry.pml4,
            .pdp = entry.pdp,
            .pd = entry.pd,
            .address = if (entry.page) |page| @intFromPtr(page) else 0,
            .data = if (entry.page) |page| page.* else @splat(0),
        };
    }
    return copy;
}
fn destroySnapshot(copy: *Snapshot) void {
    std.testing.allocator.free(copy.pts);
    std.testing.allocator.destroy(copy);
}
fn unchanged(copy: *const Snapshot) !void {
    // Empty metadata banks may be retained after a failed transaction. All
    // preexisting mappings, bytes, indexes, parents and high-water state must
    // remain unchanged; a shallow copy of Store would not verify PT contents.
    var before = copy.space;
    var after = spaces[0];
    before.pt_store = .{};
    after.pt_store = .{};
    try std.testing.expectEqualSlices(u8, std.mem.asBytes(&before), std.mem.asBytes(&after));
    for (copy.pts, 0..) |item, slot| {
        const entry = spaces[0].pt_store.descriptorConst(slot).?;
        try std.testing.expectEqual(item.pml4, entry.pml4);
        try std.testing.expectEqual(item.pdp, entry.pdp);
        try std.testing.expectEqual(item.pd, entry.pd);
        try std.testing.expectEqual(item.address, if (entry.page) |page| @intFromPtr(page) else @as(usize, 0));
        if (entry.page) |page| try std.testing.expectEqualSlices(u64, &item.data, page);
    }
    for (copy.pts.len..spaces[0].ptCapacity()) |slot| {
        const entry = spaces[0].pt_store.descriptorConst(slot).?;
        try std.testing.expectEqual(vm.UserAddressSpace.no_pd_index, entry.pd);
        try std.testing.expect(entry.page == null);
    }
}

fn fillFaultPtSlots(count: usize) !void {
    for (0..count) |pd| {
        const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, pd) orelse
            return error.TestUnexpectedResult;
        spaces[0].ptPage(slot).*[0] = physical | 7;
    }
}

test "user fault pressure retires only zero PTs after shootdown and preserves reservations" {
    init();
    defer cleanup();
    try fillFaultPtSlots(512);
    for (501..512) |slot| spaces[0].ptPage(slot).*[0] = 0;
    spaces[0].ptPage(0).*[0] = physical | 3; // Supervisor only.
    spaces[0].ptPage(1).*[0] = physical | 4; // Nonpresent, nonzero.
    spaces[0].ptPage(2).*[1] = (physical + 4096) | 3; // Mixed.
    try std.testing.expect(vm.reserveUserMapping(owner, base, 2, .linear_region, true));
    const reservations = spaces[0].reservations;
    const generation = spaces[0].reservation_generation;
    const pd_used = spaces[0].pd_page_used_len;
    const pdp_used = spaces[0].pdp_page_used_len;
    const target = base + (1 << 30);
    try std.testing.expect(vm.lockAddressSpace(owner));
    defer vm.unlockAddressSpace(owner);
    reclaim_flush_first = 501;
    vm.reclaimEmptyUserPtSlotsForFaultWithAddressSpaceLocked(owner, target);
    reclaim_flush_first = null;
    try std.testing.expectEqual(@as(usize, 1), flushes);
    try std.testing.expectEqual(@as(usize, 501), usedPtSlots());
    try std.testing.expectEqual(@as(u64, physical | 3), spaces[0].ptPage(0).*[0]);
    try std.testing.expectEqual(@as(u64, physical | 4), spaces[0].ptPage(1).*[0]);
    try std.testing.expectEqual(@as(u64, physical | 7), spaces[0].ptPage(2).*[0]);
    try std.testing.expectEqual(@as(u64, (physical + 4096) | 3), spaces[0].ptPage(2).*[1]);
    try std.testing.expectEqual(pd_used, spaces[0].pd_page_used_len);
    try std.testing.expectEqual(pdp_used, spaces[0].pdp_page_used_len);
    try std.testing.expectEqualDeep(reservations, spaces[0].reservations);
    try std.testing.expectEqual(generation, spaces[0].reservation_generation);
    try std.testing.expect(vm.mapLazyUserPaddrsWithProt(owner, target, &.{physical + 8192}, .{ .read = true, .write = true }));
    try std.testing.expectEqual(@as(?u64, physical + 8192), vm.lookupUserMappedPaddrForVa(owner, target));
    try std.testing.expectEqual(@as(usize, 502), usedPtSlots());
}

test "user fault PT pressure fast paths and reentry do not alter prepared tables" {
    for (0..4) |mode| {
        init();
        defer cleanup();
        try fillFaultPtSlots(if (mode == 0) 511 else 512);
        spaces[0].ptPage(0).*[0] = 0;
        // A formerly full table with a free slot also needs no reclamation.
        if (mode == 1) spaces[0].ptPdIndex(511).* = vm.UserAddressSpace.no_pd_index;
        try std.testing.expect(vm.lockAddressSpace(owner));
        defer vm.unlockAddressSpace(owner);
        if (mode == 3) try std.testing.expect(vm.lockAddressSpace(owner));
        defer if (mode == 3) vm.unlockAddressSpace(owner);
        const before = try snapshot();
        defer destroySnapshot(before);
        const before_translations = translations;
        vm.reclaimEmptyUserPtSlotsForFaultWithAddressSpaceLocked(owner, if (mode == 2) base else base + (1 << 30));
        try unchanged(before);
        try std.testing.expectEqual(@as(usize, 0), flushes);
        try std.testing.expectEqual(before_translations, translations);
    }
}

test "user fault PT pressure preserves full live tables and unproven parent links" {
    for (0..5) |mode| {
        init();
        defer cleanup();
        try fillFaultPtSlots(512);
        if (mode != 0) {
            spaces[0].ptPage(511).*[0] = 0;
            switch (mode) {
                1 => spaces[0].pd_pages[0][511] = 0,
                2 => spaces[0].pd_pages[0][511] |= 128,
                3 => spaces[0].pd_pages[0][511] = physical | 7,
                4 => fail_translation = translations + 1,
                else => unreachable,
            }
        }
        try std.testing.expect(vm.lockAddressSpace(owner));
        defer vm.unlockAddressSpace(owner);
        const before = try snapshot();
        defer destroySnapshot(before);
        vm.reclaimEmptyUserPtSlotsForFaultWithAddressSpaceLocked(owner, base + (1 << 30));
        try unchanged(before);
        try std.testing.expectEqual(@as(usize, 0), flushes);
    }
}

test "MMIO overlay sparse 128 GiB unmap retains admission policy and retires PTs" {
    init();
    defer cleanup();
    const size: u64 = 128 << 30;
    const middle = base + size / 2;
    for ([_]u64{ base, middle, base + size - 4096 }) |va|
        try std.testing.expect(vm.mapUserMmioOverlay(owner, va, physical, 4096, .{ .read = true }));
    const supervisor_slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 1, 0).?;
    spaces[0].ptPage(supervisor_slot).*[0] = physical | 3;
    try std.testing.expect(vm.unmapPresentUserLinearRegionSplitSlotsRequired(owner, base, size) == null);
    const before = try snapshot();
    defer destroySnapshot(before);
    const before_flushes = flushes;
    try std.testing.expect(!vm.unmapPresentUserLinearRegion(owner, base, size));
    try unchanged(before);
    try std.testing.expectEqual(before_flushes, flushes);
    try std.testing.expectEqual(@as(?usize, 0), vm.unmapPresentVmaSourceSplitSlotsRequired(owner, base, size));
    try std.testing.expect(vm.invalidatePresentUserLinearRegionPtes(owner, base, size));
    try std.testing.expectEqual(physical | 3, spaces[0].ptPage(supervisor_slot).*[0]);
    try std.testing.expect(vm.unmapPresentVmaSource(owner, base, size));
    // Ordinary VMA-source retirement, unlike MMIO-overlay close, may discard
    // supervisor-only seed PTs once no user PTE remains.
    try std.testing.expectEqual(@as(usize, 0), usedPtSlots());
}

test "MMIO overlay ordinary empty unmap skips only unchanged translations" {
    init();
    defer cleanup();
    // No PT exists for this untouched range.
    const free_reservations = vm.freeUserReservationSlotCount(owner);
    try std.testing.expect(vm.reserveUserMapping(owner, base, 1, .linear_region, true));
    try std.testing.expect(vm.unmapPresentUserLinearRegion(owner, base, 4096));
    try std.testing.expectEqual(@as(usize, 0), flushes);
    try std.testing.expectEqual(free_reservations, vm.freeUserReservationSlotCount(owner));
    // A hole in a live PT must leave its adjacent mapping intact.
    try std.testing.expect(vm.mapUserMmioOverlay(owner, base + 4096, physical, 4096, .{ .read = true }));
    flushes = 0;
    try std.testing.expect(vm.unmapPresentUserLinearRegion(owner, base, 4096));
    try std.testing.expectEqual(@as(usize, 0), flushes);
    try std.testing.expectEqual(@as(?u64, physical), vm.lookupUserMappedPaddrForVa(owner, base + 4096));
    // A mixed hole/present span changes a leaf and retires its last PT.
    try std.testing.expect(vm.unmapPresentUserLinearRegion(owner, base, 8192));
    try std.testing.expectEqual(@as(usize, 1), flushes);
    try std.testing.expectEqual(@as(usize, 0), usedPtSlots());
    try std.testing.expect(vm.unmapPresentUserLinearRegion(owner, base, 8192));
    try std.testing.expectEqual(@as(usize, 1), flushes);
}

test "MMIO overlay ordinary unmap flushes empty PT retirement alone" {
    init();
    defer cleanup();
    try std.testing.expect(vm.mapUserMmioOverlay(owner, base, physical, 4096, .{ .read = true }));
    try std.testing.expect(vm.invalidatePresentUserLinearRegionPtes(owner, base, 4096));
    try std.testing.expectEqual(@as(?u64, null), vm.lookupUserMappedPaddrForVa(owner, base));
    try std.testing.expectEqual(@as(usize, 1), usedPtSlots());
    flushes = 0;
    try std.testing.expect(vm.unmapPresentVmaSource(owner, base, 4096));
    try std.testing.expectEqual(@as(usize, 1), flushes);
    try std.testing.expectEqual(@as(usize, 0), usedPtSlots());
}

test "MMIO overlay partial removal retains shootdown without PT retirement" {
    for ([_]bool{ false, true }) |overlay| {
        init();
        defer cleanup();
        try std.testing.expect(vm.mapUserMmioOverlay(owner, base, physical, 8192, .{ .read = true }));
        flushes = 0;
        try std.testing.expect(if (overlay)
            vm.unmapUserMmioOverlay(owner, base, physical, 4096)
        else
            vm.unmapPresentUserLinearRegion(owner, base, 4096));
        try std.testing.expectEqual(@as(usize, 1), flushes);
        try std.testing.expectEqual(@as(?u64, physical + 4096), vm.lookupUserMappedPaddrForVa(owner, base + 4096));
        try std.testing.expectEqual(@as(usize, 1), usedPtSlots());
    }
}

test "MMIO overlay free range search rejects a present last page" {
    init();
    defer cleanup();
    // Overlay PTEs deliberately have no reservation record: the search must
    // detect them from the page tables, including a one-page candidate.
    try std.testing.expect(vm.mapUserMmioOverlay(owner, base, physical, 4096, .{ .read = true }));
    try std.testing.expectEqual(@as(?u64, base + 4096), vm.findFreeUserMappingRange(owner, 1, 1));
    try std.testing.expect(vm.advanceFreeUserMappingSearch(owner, base + 8192));
    try std.testing.expect(vm.mapUserMmioOverlay(owner, base + 3 * 4096, physical, 4096, .{ .read = true }));
    try std.testing.expectEqual(@as(?u64, base + 4 * 4096), vm.findFreeUserMappingRange(owner, 2, 1));
}

test "MMIO overlay no-op protection preserves range validation and PTEs" {
    init();
    defer cleanup();
    try std.testing.expect(vm.mapTrustedLowPageZeroPaddrsWithProt(owner, 0, &.{physical}, .{ .read = true, .exec = true }));
    const before = try snapshot();
    defer destroySnapshot(before);
    try std.testing.expect(vm.validateUserLinearRegion(owner, base, 4096));
    for ([_][2]u64{
        .{ 0, 4096 },       .{ base + 1, 4096 },         .{ base, 0 },
        .{ 1 << 47, 4096 }, .{ (1 << 47) - 4096, 8192 }, .{ 0xffff_ffff_ffff_f000, 8192 },
    }) |range| {
        try std.testing.expect(!vm.validateUserLinearRegion(owner, range[0], @intCast(range[1])));
    }
    try std.testing.expect(!vm.validateUserLinearRegion(kernel.processPrincipalFromIndex(2).?, base, 4096));
    try unchanged(before);
    try std.testing.expectEqual(@as(usize, 0), flushes);
}

test "MMIO overlay trusted remap preserves identical PTEs including hardware A/D" {
    init();
    defer cleanup();
    const paddrs = [_]u64{ physical, physical + 4096 };
    const prot: kernel.MapProt = .{ .read = true, .write = true };
    try std.testing.expect(vm.mapTrustedUserPaddrsWithProt(owner, base, &paddrs, prot));
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
    spaces[0].ptPage(slot).*[0] |= 1 << 5;
    spaces[0].ptPage(slot).*[1] |= (1 << 5) | (1 << 6);
    const before = try snapshot();
    defer destroySnapshot(before);
    try std.testing.expect(vm.remapTrustedUserPaddrsWithProt(owner, base, &paddrs, prot));
    try unchanged(before);
    try std.testing.expectEqual(@as(usize, 0), flushes);
    // Validate the entire input before the equality fast path or any store.
    try std.testing.expect(!vm.remapTrustedUserPaddrsWithProt(owner, base, &.{ physical + 8192, physical + 1 }, prot));
    try unchanged(before);
    try std.testing.expectEqual(@as(usize, 0), flushes);
}

test "MMIO overlay trusted remap keeps full-range shootdown for every non-AD difference" {
    const prot: kernel.MapProt = .{ .read = true, .write = true };
    // Present, RW, user, PWT, PCD, PAT, physical address, pkey and NX.
    for ([_]u6{ 0, 1, 2, 3, 4, 7, 12, 59, 63 }) |bit| {
        init();
        defer cleanup();
        const paddrs = [_]u64{ physical, physical + 4096 };
        try std.testing.expect(vm.mapTrustedUserPaddrsWithProt(owner, base, &paddrs, prot));
        const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
        const expected = spaces[0].ptPage(slot).*[0..2].*;
        spaces[0].ptPage(slot).*[0] |= (1 << 5) | (1 << 6);
        spaces[0].ptPage(slot).*[1] ^= @as(u64, 1) << bit;
        try std.testing.expect(vm.remapTrustedUserPaddrsWithProt(owner, base, &paddrs, prot));
        try std.testing.expectEqualSlices(u64, &expected, spaces[0].ptPage(slot).*[0..2]);
        try std.testing.expectEqual(@as(usize, 1), flushes);
    }
    // A missing PT must take the original allocation path, not report a no-op.
    init();
    defer cleanup();
    try std.testing.expect(vm.remapTrustedUserPaddrsWithProt(owner, base, &.{physical}, prot));
    try std.testing.expectEqual(@as(?u64, physical), vm.lookupUserMappedPaddrForVa(owner, base));
    try std.testing.expectEqual(@as(usize, 1), flushes);
}

test "MMIO overlay retains reservations and retires PT PD PDP on repeated last-close" {
    init();
    defer cleanup();
    // The VMA admission test is separate. Test both low-level reservation
    // presence and absence: an untouched anonymous VMA may have no record.
    for (0..16) |round| {
        const target = base + round * (1 << 30) + 0x1ff000;
        if (round % 2 == 0) try std.testing.expect(vm.reserveUserMapping(owner, target, 2, .linear_region, false));
        const reservations = spaces[0].reservations;
        const generation = spaces[0].reservation_generation;
        try std.testing.expect(vm.mapUserMmioOverlay(owner, target, physical, 8192, .{ .read = true }));
        try std.testing.expectEqual(@as(?u64, physical), vm.lookupUserMappedPaddrForVa(owner, target));
        try std.testing.expectEqual(@as(?u64, physical + 4096), vm.lookupUserMappedPaddrForVa(owner, target + 4096));
        for (0..spaces[0].ptCapacity()) |slot| {
            if (spaces[0].ptPdIndex(slot).* == vm.UserAddressSpace.no_pd_index) continue;
            const page = spaces[0].ptPage(slot);
            for (page) |pte| {
                if (pte == 0) continue;
                try std.testing.expectEqual(@as(u64, 0x1d), pte & 0x1f); // Present, user, UC, RO.
                try std.testing.expect(pte & (@as(u64, 1) << 63) != 0);
            }
        }
        try std.testing.expect(vm.unmapUserMmioOverlay(owner, target, physical, 8192));
        try std.testing.expect(flushes > 0);
        try std.testing.expectEqual(@as(?u64, null), vm.lookupUserMappedPaddrForVa(owner, target));
        try std.testing.expectEqualDeep(reservations, spaces[0].reservations);
        try std.testing.expectEqual(generation, spaces[0].reservation_generation);
        try noTables();
        try std.testing.expect(vm.unmapUserMmioOverlay(owner, target, physical, 8192));
        if (round % 2 == 0) try std.testing.expect(vm.releaseUserMapping(owner, target, 2));
    }
}

test "MMIO overlay UC_MINUS uses PAT entry two and retains failure rollback" {
    init();
    defer cleanup();
    try std.testing.expect(vm.mapUserMmioOverlayWithCache(owner, base, physical, 8192, .{ .read = true, .write = true }, .uc_minus));
    for (0..spaces[0].ptCapacity()) |slot| {
        if (spaces[0].ptPdIndex(slot).* == vm.UserAddressSpace.no_pd_index) continue;
        const page = spaces[0].ptPage(slot);
        for (page) |pte| {
            if (pte == 0) continue;
            try std.testing.expectEqual(@as(u64, 0x17), pte & 0x9f); // PCD, not PWT/PAT.
            try std.testing.expect(pte & (@as(u64, 1) << 63) != 0);
        }
    }
    try std.testing.expect(vm.unmapUserMmioOverlay(owner, base, physical, 8192));
    try noTables();
    for (1..4) |failure| {
        init();
        defer cleanup();
        const copy = try snapshot();
        defer destroySnapshot(copy);
        fail_translation = failure;
        try std.testing.expect(!vm.mapUserMmioOverlayWithCache(owner, base, physical, 4096, .{ .read = true }, .uc_minus));
        try unchanged(copy);
    }
}

test "MMIO overlay cache proof checks PAT and every MTRR page across CPU snapshots" {
    var boot = mtrr.CacheSnapshot{
        .pat = 6 | (@as(u64, 7) << 16),
        .physical_bits = 36,
        .capability = 1 | (1 << 10),
        .default_type = (1 << 11),
    };
    try std.testing.expect(boot.valid());
    try std.testing.expect(boot.rangeIsUncached(0xfe000000, 8192));
    var high = boot;
    high.physical_bits = 46;
    try std.testing.expect(high.rangeIsUncached(0x380000000000, 4096));
    try std.testing.expect(!boot.rangeIsUncached(0x380000000000, 4096));
    try std.testing.expect(!high.rangeIsUncached(1 << 46, 4096));
    high.default_type |= 6; // High MMIO cannot silently become WB.
    try std.testing.expect(!high.rangeIsUncached(0x380000000000, 4096));
    try std.testing.expect(high.rangeSupportsUncachedMapping(0x380000000000, 4096));
    try std.testing.expect(!high.rangeSupportsUncachedMapping(1 << 46, 4096));
    try std.testing.expect(!high.rangeSupportsUncachedMapping(0xfffffffffffff000, 8192));
    try std.testing.expect(!high.rangeSupportsUncachedMapping(0x380000000001, 4096));
    high.pat |= @as(u64, 6) << 24; // Entry 3 must really be UC.
    try std.testing.expect(!high.rangeSupportsUncachedMapping(0x380000000000, 4096));
    var other = boot;
    try std.testing.expect(boot.matches(&other));
    for ([_]u6{ 8, 32, 40, 48, 56 }) |shift| {
        other = boot;
        other.pat ^= @as(u64, 6) << shift;
        try std.testing.expect(boot.matches(&other));
    }
    for ([_]u6{ 0, 16, 24 }) |shift| {
        other = boot;
        other.pat ^= @as(u64, 1) << shift;
        try std.testing.expect(!boot.matches(&other));
    }
    // Firmware/bootloader PAT pair observed on the native 2-vCPU fixture.
    boot.pat = 0x0000010500070406;
    other = boot;
    other.pat = 0x0007040600070406;
    try std.testing.expect(boot.matches(&other));
    other = boot;
    other.pat ^= 1;
    try std.testing.expect(!boot.matches(&other));
    other = boot;
    other.pat &= ~(@as(u64, 0xff) << 16);
    try std.testing.expect(!other.valid());
    other = boot;
    other.pat |= @as(u64, 6) << 24;
    try std.testing.expect(!other.valid());
    other = boot;
    other.physical_bits = 40;
    try std.testing.expect(!boot.matches(&other));
    other = boot;
    other.default_type |= 6;
    try std.testing.expect(!boot.matches(&other));
    try std.testing.expect(!other.rangeIsUncached(0xfe000000, 4096));
    other.default_type &= ~@as(u64, 1 << 11);
    try std.testing.expect(!other.valid());
    const address_mask: u64 = 0xffffff000;
    // A non-UC page between UC endpoints must reject the entire mapping.
    boot.ranges[0] = .{ .base = 0xfe001000 | 1, .mask = address_mask | (1 << 11) };
    try std.testing.expect(boot.valid());
    try std.testing.expect(boot.rangeIsUncached(0xfe000000, 4096));
    try std.testing.expect(boot.rangeIsUncached(0xfe002000, 4096));
    try std.testing.expect(!boot.rangeIsUncached(0xfe000000, 12288));
    try std.testing.expect(!boot.rangeIsUncached(0xfe001000, 4096)); // MTRR WC is not UC.
    boot.ranges[0].base = 0xfe001000 | 6;
    try std.testing.expect(!boot.rangeIsUncached(0xfe001000, 4096));
    other = boot;
    other.ranges[0].base = 0xfe002000 | 6;
    try std.testing.expect(!boot.matches(&other));
    boot.ranges[0].base = 0xfe001000;
    boot.default_type |= 6;
    try std.testing.expect(boot.rangeIsUncached(0xfe001000, 4096));
    try std.testing.expect(!boot.rangeIsUncached(0xfe000000, 8192));
    boot.ranges[0].mask &= ~@as(u64, 1 << 13); // Noncontiguous variable mask.
    try std.testing.expect(!boot.valid());
    boot.ranges[0] = .{};
    boot.default_type &= ~@as(u64, 0xff);
    try std.testing.expect(!boot.rangeIsUncached(0, 4096));
    try std.testing.expect(!boot.rangeIsUncached(0xfffff, 4096));
    try std.testing.expect(!boot.rangeIsUncached(0x100000, 0));
    try std.testing.expect(!boot.rangeIsUncached(0x100000, 4097));
    try std.testing.expect(!boot.rangeIsUncached(0xfffffffffffff000, 8192));
    try std.testing.expect(!boot.rangeIsUncached(0xffffff000, 8192));
}

test "MMIO overlay UC_MINUS admits proven WB and rejects conflicting MTRRs" {
    const address: u64 = 0x380000000000;
    var proof = mtrr.CacheSnapshot{
        .pat = 6 | (@as(u64, 7) << 16),
        .physical_bits = 46,
        .capability = 3 | (1 << 10),
        .default_type = (1 << 11) | 6,
    };
    try std.testing.expect(proof.rangeIsUcMinusUncached(address, 8192));
    try std.testing.expect(!proof.rangeIsUncached(address, 8192));
    const mask: u64 = (((@as(u64, 1) << 46) - 1) & ~@as(u64, 4095)) | (1 << 11);
    proof.ranges[0] = .{ .base = address | 6, .mask = mask };
    try std.testing.expect(proof.rangeIsUcMinusUncached(address, 4096));
    proof.ranges[1] = proof.ranges[0];
    try std.testing.expect(proof.rangeIsUcMinusUncached(address, 4096));
    for ([_]u8{ 1, 4, 5 }) |conflict| {
        proof.ranges[1].base = address | conflict;
        try std.testing.expect(!proof.rangeIsUcMinusUncached(address, 4096));
    }
    proof.ranges[2] = .{ .base = address, .mask = mask }; // UC dominates.
    try std.testing.expect(proof.rangeIsUcMinusUncached(address, 4096));
    proof.ranges[2] = .{ .base = (address + 4096) | 1, .mask = mask };
    proof.ranges[1] = proof.ranges[0];
    try std.testing.expect(!proof.rangeIsUcMinusUncached(address, 8192));
    try std.testing.expect(!proof.rangeIsUcMinusUncached(address + 1, 4096));
    try std.testing.expect(!proof.rangeIsUcMinusUncached(address, 0));
    proof.pat ^= @as(u64, 1) << 16;
    try std.testing.expect(!proof.rangeIsUcMinusUncached(address, 4096));
}

test "MMIO overlay ordinary MMIO UC_MINUS preserves the same PTE encoding" {
    init();
    defer cleanup();
    try std.testing.expect(vm.mapUserMmioLinearRegionWithProt(owner, base, physical, 4096, .{ .read = true }, .uc_minus));
    for (0..spaces[0].ptCapacity()) |slot| {
        if (spaces[0].ptPdIndex(slot).* == vm.UserAddressSpace.no_pd_index) continue;
        const page = spaces[0].ptPage(slot);
        for (page) |pte| {
            if (pte == 0) continue;
            try std.testing.expectEqual(@as(u64, 0x15), pte & 0x9f);
            try std.testing.expect(pte & (@as(u64, 1) << 63) != 0);
        }
    }
}

test "MMIO overlay translation and table-capacity failures roll back before PTE publication" {
    // Fail each of the new PDP/PD/PT physical-address lookups.
    for (1..4) |failure| {
        init();
        defer cleanup();
        const before = try snapshot();
        defer destroySnapshot(before);
        fail_translation = failure;
        try std.testing.expect(!vm.mapUserMmioOverlay(owner, base, physical, 4096, .{ .read = true }));
        try unchanged(before);
        try noTables();
    }
    init();
    defer cleanup();
    // Leave one descriptor available, then fail allocation of the next bank.
    for (1..512) |pd|
        try std.testing.expect(vm.ensureUserPtSlotForPd(&spaces[0], 2, 0, pd) != null);
    const before = try snapshot();
    defer destroySnapshot(before);
    for (0..8) |_| {
        pt_fail_at = pt_alloc_calls + 2;
        try std.testing.expect(!vm.mapUserMmioOverlay(owner, base + 0x1ff000, physical, 8192, .{ .read = true }));
        try unchanged(before);
    }
}

test "MMIO overlay rejects existing PTEs and wrong-backing close without changing either page" {
    init();
    defer cleanup();
    const target = base + 0x1ff000;
    try std.testing.expect(vm.mapUserMmioOverlay(owner, target + 4096, physical + 4096, 4096, .{ .read = true, .write = true }));
    const before = try snapshot();
    defer destroySnapshot(before);
    try std.testing.expect(!vm.mapUserMmioOverlay(owner, target, physical, 8192, .{ .read = true }));
    try unchanged(before);
    try std.testing.expect(!vm.unmapUserMmioOverlay(owner, target, physical + 4096, 8192));
    try unchanged(before);
    try std.testing.expect(vm.unmapUserMmioOverlay(owner, target, physical, 8192));
    try noTables();
}

test "MMIO overlay last-close preserves neighboring supervisor PTEs and their parents" {
    init();
    defer cleanup();
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
    const neighbor: u64 = physical + 4096 | 3; // Present writable supervisor page.
    spaces[0].ptPage(slot).*[1] = neighbor;
    try std.testing.expect(vm.mapUserMmioOverlay(owner, base, physical, 4096, .{ .read = true }));
    try std.testing.expect(vm.unmapUserMmioOverlay(owner, base, physical, 4096));
    try std.testing.expectEqual(neighbor, spaces[0].ptPage(slot).*[1]);
    try std.testing.expectEqual(@as(u64, 0), spaces[0].ptPage(slot).*[0]);
    try std.testing.expectEqual(@as(usize, 1), usedPtSlots());
    try std.testing.expectEqual(@as(usize, 1), usedSlots(&spaces[0].pd_page_pdp_index));
    try std.testing.expectEqual(@as(usize, 1), usedSlots(&spaces[0].pdp_page_pml4_index));
    const before = try snapshot();
    defer destroySnapshot(before);
    try std.testing.expect(!vm.mapUserMmioOverlay(owner, base + 4096, physical, 4096, .{ .read = true }));
    try unchanged(before);
    spaces[0].ptPage(slot).*[1] = 0;
    try std.testing.expect(vm.unmapUserMmioOverlay(owner, base, physical, 4096));
    try noTables();
}
