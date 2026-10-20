const std = @import("std");
const vm = @import("memory/user_vm.zig");
const kernel = @import("kernel.zig");
var invocation_allocator = kernel.InvocationStorageTestAllocator{};
const mtrr = @import("arch/x86_64/mtrr.zig");

test "MMIO overlay overflow storage tests" {
    _ = @import("memory/pt_storage.zig");
    _ = @import("page_fault_log.zig");
    _ = @import("state/vma.zig");
    _ = @import("memory/ram_cache_commit.zig");
    _ = @import("memory/ram_cache_request.zig");
    _ = @import("memory/ram_cache_storage.zig");
    _ = @import("syscall/ram_cache.zig");
    _ = @import("memory/pmm.zig");
}

test "MMIO overlay eager scatter mappings inherit mixed RAM policy atomically" {
    const copy = @import("user_copy.zig");
    const policy = @import("memory/ram_cache_policy.zig");
    init();
    defer cleanup();
    defer copy.publishRamCacheSnapshot(null);
    const spans = [_]policy.Span{.{ .base = physical + 4096, .bytes = 4096, .owner = 7, .mode = .wc, .pat_slot = 4 }};
    const pending: policy.Snapshot = .{ .pending = &spans };
    copy.publishRamCacheSnapshot(&pending);
    const pages = [_]u64{ physical, physical + 4096 };
    try std.testing.expect(!vm.mapTrustedUserPaddrsWithProt(owner, base, &pages, .{ .read = true }));
    try std.testing.expectEqual(@as(?u64, null), vm.lookupUserMappedPaddrForVa(owner, base));
    const active: policy.Snapshot = .{ .policy = &spans };
    copy.publishRamCacheSnapshot(&active);
    try std.testing.expect(!vm.mapTrustedUserPaddrsWithProt(owner, base, &pages, .{ .read = true, .exec = true }));
    try std.testing.expectEqual(@as(?u64, null), vm.lookupUserMappedPaddrForVa(owner, base));
    try std.testing.expect(!vm.mapUserMmioLinearRegionWithProt(owner, base, physical, 8192, .{ .read = true }, .uc));
    try std.testing.expectEqual(@as(?u64, null), vm.lookupUserMappedPaddrForVa(owner, base));
    try std.testing.expect(vm.mapTrustedUserPaddrsWithProt(owner, base, &pages, .{ .read = true, .write = true }));
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
    try std.testing.expectEqual(@as(u64, 0), spaces[0].ptPage(slot)[0] & 0x98);
    try std.testing.expectEqual(@as(u64, 0x80), spaces[0].ptPage(slot)[1] & 0x98);
    const saved = spaces[0].ptPage(slot).*;
    for ([_]bool{ false, true }) |sparse| {
        const ok = if (sparse)
            vm.protectPresentUserLinearRegionWithProt(owner, base, 8192, .{ .read = true, .exec = true })
        else
            vm.protectUserLinearRegionWithProt(owner, base, 8192, .{ .read = true, .exec = true });
        try std.testing.expect(!ok);
        try std.testing.expectEqualSlices(u64, &saved, spaces[0].ptPage(slot));
    }
    copy.publishRamCacheSnapshot(&pending);
    try std.testing.expect(!vm.remapTrustedUserPaddrsWithProt(owner, base, &pages, .{ .read = true }));
    try std.testing.expect(!vm.mapOrRemapTrustedUserPaddrsWithProt(owner, base, &pages, .{ .read = true }));
    try std.testing.expectEqualSlices(u64, &saved, spaces[0].ptPage(slot));
    copy.publishRamCacheSnapshot(&active);
    try std.testing.expect(vm.remapTrustedUserPaddrsWithProt(owner, base, &pages, .{ .read = true }));
    try std.testing.expectEqual(@as(u64, 0x80), spaces[0].ptPage(slot)[1] & 0x98);
    try std.testing.expect(vm.mapOrRemapTrustedUserPaddrsWithProt(owner, base, &pages, .{ .read = true, .write = true }));
    spaces[0].ptPage(slot)[1] |= (1 << 5) | (1 << 6);
    try std.testing.expect(vm.protectUserLinearRegionWithProt(owner, base, 8192, .{ .read = true }));
    try std.testing.expectEqual(@as(u64, 0xe0), spaces[0].ptPage(slot)[1] & 0xf8);
    try std.testing.expect(vm.invalidatePresentUserPaddrsIfCurrent(owner, base, &pages));
    copy.publishRamCacheSnapshot(&pending);
    try std.testing.expect(!vm.installInvalidatedUserPaddrsWithProt(owner, base, &pages, .{ .read = true }));
    try std.testing.expectEqual(@as(u64, 0), spaces[0].ptPage(slot)[0]);
    copy.publishRamCacheSnapshot(&active);
    try std.testing.expect(vm.installInvalidatedUserPaddrsWithProt(owner, base, &pages, .{ .read = true }));
    try std.testing.expectEqual(@as(u64, 0x80), spaces[0].ptPage(slot)[1] & 0x98);
}

test "MMIO overlay RAM fault mapping honors physical policy and pending exclusion" {
    const copy = @import("user_copy.zig");
    const policy = @import("memory/ram_cache_policy.zig");
    init();
    defer cleanup();
    defer copy.publishRamCacheSnapshot(null);
    const span = [_]policy.Span{.{
        .base = physical,
        .bytes = 8192,
        .owner = 7,
        .mode = .wc,
        .pat_slot = 4,
    }};
    const snapshot_policy: policy.Snapshot = .{ .policy = &span };
    copy.publishRamCacheSnapshot(&snapshot_policy);
    try std.testing.expect(vm.mapNativeVmaFaultPage(owner, base, .{
        .paddr = physical,
        .prot = .{ .read = true, .write = true },
    }));
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
    try std.testing.expectEqual(@as(u64, 0x80), spaces[0].ptPage(slot)[0] & 0x98);
    try std.testing.expect(!vm.mapNativeVmaFaultPage(owner, base + 4096, .{
        .paddr = physical + 4096,
        .prot = .{ .read = true, .exec = true },
    }));
    const pending: policy.Snapshot = .{ .policy = &span, .pending = &span };
    copy.publishRamCacheSnapshot(&pending);
    try std.testing.expect(!vm.mapNativeVmaFaultPage(owner, base + 4096, .{
        .paddr = physical + 4096,
        .prot = .{ .read = true },
    }));
    try std.testing.expectEqual(@as(?u64, null), vm.lookupUserMappedPaddrForVa(owner, base + 4096));
}

test "MMIO overlay RAM cache captures cross-process aliases and preserves late hardware bits" {
    init();
    defer cleanup();
    const nx: u64 = @as(u64, 1) << 63;
    var leaves: [2]*u64 = undefined;
    for (&spaces, 0..) |*space, i| {
        space.cr3 = 0x1000 + i * 4096;
        const slot = vm.ensureUserPtSlotForPd(space, 1, 0, 0).?;
        leaves[i] = &space.ptPage(slot)[i];
        leaves[i].* = physical | nx | 5 | (if (i == 0) @as(u64, 2) else 0) | (1 << 10);
        // An unrelated physical page must not get swept into the transition.
        space.ptPage(slot)[7] = (physical + 4096) | nx | 7;
    }
    var scratch: [2]vm.RamCacheUserAlias = undefined;
    const allocations = pt_alloc_calls;
    const live = pt_live_pages;
    try std.testing.expectEqual(@as(usize, 2), try vm.prepareRamCacheUserAliases(&.{physical}, &scratch));
    try std.testing.expectEqual(owner, scratch[0].principal);
    try std.testing.expectEqual(kernel.processPrincipalFromIndex(1).?, scratch[1].principal);
    try std.testing.expectEqual(base + 4096, scratch[1].va);
    // Model A set after preflight, then D written back during the shootdown.
    leaves[0].* |= 1 << 5;
    vm.withdrawRamCacheUserAliases(&scratch);
    for (leaves) |leaf| try std.testing.expectEqual(@as(u64, 0), leaf.* & 1);
    leaves[0].* |= 1 << 6;
    const before_flush = flushes;
    vm.flushRamCacheUserAliases(&scratch);
    try std.testing.expectEqual(before_flush + 2, flushes);
    // Synthetic tables only: production publication additionally needs the
    // supervisor withdrawal and all-CPU cache drain, not just this TLB hook.
    vm.publishRamCacheUserAliases(&scratch, 4);
    vm.flushRamCacheUserAliases(&scratch);
    for (leaves, 0..) |leaf, i| {
        try std.testing.expectEqual(@as(u64, 0x80), leaf.* & 0x98);
        try std.testing.expectEqual(@as(u64, if (i == 0) 2 else 0), leaf.* & 2);
        try std.testing.expectEqual(nx | (1 << 10) | 5, leaf.* & (nx | (1 << 10) | 5));
    }
    try std.testing.expectEqual(@as(u64, 0x60), leaves[0].* & 0x60);
    try std.testing.expectEqual(@as(usize, 2), try vm.prepareRamCacheUserAliases(&.{physical}, &scratch));
    vm.withdrawRamCacheUserAliases(&scratch);
    vm.flushRamCacheUserAliases(&scratch);
    vm.publishRamCacheUserAliases(&scratch, 0);
    vm.flushRamCacheUserAliases(&scratch);
    for (leaves) |leaf| try std.testing.expectEqual(@as(u64, 0), leaf.* & 0x98);
    try std.testing.expectEqual(allocations, pt_alloc_calls);
    try std.testing.expectEqual(live, pt_live_pages);
}

test "MMIO overlay RAM alias preflight failure never changes PTEs" {
    init();
    defer cleanup();
    spaces[0].cr3 = 0x1000;
    const nx: u64 = @as(u64, 1) << 63;
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
    const pt = spaces[0].ptPage(slot);
    const entry = physical | nx | 7;
    pt[0] = entry;
    pt[1] = entry;
    var scratch: [2]vm.RamCacheUserAlias = undefined;
    try std.testing.expectError(error.NoSpace, vm.prepareRamCacheUserAliases(&.{physical}, scratch[0..1]));
    try std.testing.expectEqual(entry, pt[0]);
    try std.testing.expectEqual(entry, pt[1]);
    pt[1] &= ~nx;
    try std.testing.expectError(error.ExecutableAlias, vm.prepareRamCacheUserAliases(&.{physical}, &scratch));
    try std.testing.expectEqual(entry, pt[0]);
    pt[1] = entry | (1 << 8);
    try std.testing.expectError(error.GlobalAlias, vm.prepareRamCacheUserAliases(&.{physical}, &scratch));
    pt[1] = entry;
    for ([_][]const u64{ &.{}, &.{0}, &.{physical + 1}, &.{ physical, physical }, &.{ physical + 4096, physical } }) |invalid|
        try std.testing.expectError(error.Invalid, vm.prepareRamCacheUserAliases(invalid, &scratch));
    // A target hidden inside a huge mapping cannot evade the 4 KiB scan.
    _ = spaces[0].pd_store.ensurePage(1, ptAllocator()).?;
    spaces[0].pdPml4Index(1).* = 2;
    spaces[0].pdPage(1).*[0] = physical | nx | 0x87;
    try std.testing.expectError(error.LargeAlias, vm.prepareRamCacheUserAliases(&.{physical + 4096}, &scratch));
    spaces[0].pdPage(1).*[0] = 0;
    _ = spaces[0].pdp_store.ensurePage(1, ptAllocator()).?;
    spaces[0].pdpPml4Index(1).* = 2;
    spaces[0].pdpPage(1).*[0] = physical | nx | 0x87;
    try std.testing.expectError(error.LargeAlias, vm.prepareRamCacheUserAliases(&.{physical + 4096}, &scratch));
    try std.testing.expectEqual(entry, pt[0]);
    try std.testing.expectEqual(entry, pt[1]);
}

const owner = kernel.processPrincipalFromIndex(0).?;
const RamAliasSources = struct {
    const Source = @import("memory/ram_cache_paging.zig").AliasSource;
    items: [16]Source = undefined,
    count: usize = 0,
    fn visit(self: *RamAliasSources, source: Source) !void {
        if (self.count == self.items.len) return error.NoSpace;
        self.items[self.count] = source;
        self.count += 1;
    }
};

test "MMIO overlay RAM aliases contain only user leaves and reject copied supervisor leaves" {
    init();
    defer cleanup();
    const nx: u64 = @as(u64, 1) << 63;
    spaces[0].cr3 = @intFromPtr(&spaces[0].pml4);
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 0, 2, 0).?;
    const leaf = &spaces[0].ptPage(slot)[0];
    leaf.* = physical | nx | 7;
    var sources: RamAliasSources = .{};
    try vm.visitRamCacheProcessAliases(&.{physical}, &sources, RamAliasSources.visit);
    try std.testing.expectEqual(@as(usize, 1), sources.count);
    try std.testing.expect(sources.items[0] == .leaf);
    leaf.* &= ~@as(u64, 4);
    sources.count = 0;
    try std.testing.expectError(error.UnknownTopology, vm.visitRamCacheProcessAliases(&.{physical}, &sources, RamAliasSources.visit));
    try std.testing.expectEqual(@as(usize, 0), sources.count);
}

test "MMIO overlay RAM refuses untracked large user branches without modifying them" {
    init();
    defer cleanup();
    spaces[0].cr3 = @intFromPtr(&spaces[0].pml4);
    _ = vm.ensureUserPtSlotForPd(&spaces[0], 0, 2, 0).?;
    const directory = &spaces[0].pdPage(0)[0];
    directory.* = physical | 0x83;
    const before = directory.*;
    var sources: RamAliasSources = .{};
    try std.testing.expectError(error.UnknownTopology, vm.visitRamCacheProcessAliases(&.{physical}, &sources, RamAliasSources.visit));
    try std.testing.expectEqual(before, directory.*);
    try std.testing.expectEqual(@as(usize, 0), sources.count);
}

test "MMIO overlay RAM alias scan rejects unknown supervisor and global mappings" {
    init();
    defer cleanup();
    spaces[0].cr3 = @intFromPtr(&spaces[0].pml4);
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
    var sources: RamAliasSources = .{};
    spaces[0].ptPage(slot)[0] = physical | 3;
    try std.testing.expectError(error.UnknownTopology, vm.visitRamCacheProcessAliases(&.{physical}, &sources, RamAliasSources.visit));
    spaces[0].ptPage(slot)[0] = physical | 0x107 | (@as(u64, 1) << 63);
    try std.testing.expectError(error.GlobalAlias, vm.visitRamCacheProcessAliases(&.{physical}, &sources, RamAliasSources.visit));
    spaces[0].ptPage(slot)[0] = physical | 7;
    try std.testing.expectError(error.ExecutableAlias, vm.visitRamCacheProcessAliases(&.{physical}, &sources, RamAliasSources.visit));
    try std.testing.expectEqual(@as(usize, 0), sources.count);
}

test "MMIO overlay RAM topology rejects detached PT before publishing aliases" {
    init();
    defer cleanup();
    spaces[0].cr3 = @intFromPtr(&spaces[0].pml4);
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
    const leaf = &spaces[0].ptPage(slot)[0];
    const original = physical | 7 | (@as(u64, 1) << 63);
    leaf.* = original;
    const parent = &spaces[0].pdPage(0).*[0];
    const linked = parent.*;
    var sources: RamAliasSources = .{};
    for ([_]u64{ 0, linked ^ 4096, linked | 0x10, linked | 0x80 }) |bad| {
        parent.* = bad;
        try std.testing.expectError(error.UnknownTopology, vm.visitRamCacheProcessAliases(&.{physical}, &sources, RamAliasSources.visit));
        try std.testing.expectEqual(@as(usize, 0), sources.count);
        try std.testing.expectEqual(original, leaf.*);
    }
    parent.* = linked | (1 << 5);
    try vm.visitRamCacheProcessAliases(&.{physical}, &sources, RamAliasSources.visit);
    try std.testing.expectEqual(@as(usize, 1), sources.count);
}

test "MMIO overlay RAM topology rejects supervisor parents in every permission combination" {
    init();
    defer cleanup();
    spaces[0].cr3 = @intFromPtr(&spaces[0].pml4);
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 0, 2, 0).?;
    const nx: u64 = @as(u64, 1) << 63;
    const parent = &spaces[0].pdPage(0).*[0];
    const table_pa = parent.* & 0x000f_ffff_ffff_f000;
    for ([_]u64{ 1, 3, 1 | nx, 3 | nx }) |flags| {
        parent.* = table_pa | flags;
        spaces[0].ptPage(slot)[0] = physical | flags;
        for (0..2) |_| {
            var sources: RamAliasSources = .{};
            try std.testing.expectError(error.UnknownTopology, vm.visitRamCacheProcessAliases(&.{physical}, &sources, RamAliasSources.visit));
            try std.testing.expectEqual(@as(usize, 0), sources.count);
            try std.testing.expectEqual(table_pa | flags, parent.*);
        }
    }
}

const base: u64 = 1 << 39;
const physical: u64 = 0x8000_0000;
const pt_storage = @import("memory/pt_storage.zig");
var pt_context: u8 = 0;
var pt_alloc_calls: usize = 0;
var pt_fail_at: usize = 0;
var pt_live_pages: usize = 0;
var pt_release_fail: bool = false;

var teardown_check: ?struct { index: usize, pages: usize } = null;
fn allocatePt(_: *anyopaque, pages: usize) ?pt_storage.MappedExtent {
    std.debug.assert(pages == 1);
    pt_alloc_calls += 1;
    if (pt_alloc_calls == pt_fail_at) {
        if (g3_capture_on_failure) g3_retirement.?.* = G3Retirement.capture();
        return null;
    }
    const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096) catch return null;
    pt_live_pages += 1;
    return .{ .allocation = .{ .physical = .fromInt(@intFromPtr(bytes.ptr)), .pages = 1, .region_id = 91 }, .bytes = bytes };
}
fn releasePt(_: *anyopaque, grant: pt_storage.PhysicalExtent) bool {
    if (g3_retirement) |probe| probe.returning(grant.physical.toInt());
    std.debug.assert(grant.pages == 1 and grant.region_id == 91);
    const page: pt_storage.PagePointer = @ptrFromInt(grant.physical.toInt());
    if (pt_release_fail) return false;
    retirement.returning(grant.physical.toInt(), grant.pages, .pt);
    const bytes: []align(4096) u8 = page;
    std.testing.allocator.free(bytes);
    pt_live_pages -= 1;
    return true;
}

test "MMIO overlay RAM kernel split rollback retains failed PMM returns for reuse" {
    init();
    defer cleanup();
    var directory: u64 = physical | 0x83;
    const source: RamAliasSources.Source = .{ .identity = .{ .directory = &directory, .base = physical, .alias_va = @import("arch/x86_64/direct_map.zig").base + (physical) } };
    var reservation = try vm.reserveRamCacheKernelSplit(source);
    const page = reservation.split.table;
    try std.testing.expectEqual(physical | 0x83, directory);
    pt_release_fail = true;
    reservation.releaseReservation();
    reservation.releaseReservation();
    try std.testing.expectEqual(@as(usize, 3), pt_live_pages); // PT, bank and directory.
    const allocated = pt_alloc_calls;
    pt_release_fail = false;
    reservation = try vm.reserveRamCacheKernelSplit(source);
    try std.testing.expectEqual(page, reservation.split.table);
    try std.testing.expectEqual(allocated, pt_alloc_calls);
    reservation.releaseReservation();
    try std.testing.expectEqual(@as(usize, 0), pt_live_pages); // Empty bank and directory returned.
    try std.testing.expectEqual(physical | 0x83, directory);
}

test "MMIO overlay RAM PTE plan deduplicates aliases and commits reserved splits without allocation" {
    const plan_module = @import("memory/ram_cache_vm_plan.zig");
    const paging = @import("memory/ram_cache_paging.zig");
    init();
    defer cleanup();
    spaces[0].cr3 = 0x1000;
    _ = spaces[0].pd_store.ensurePage(0, ptAllocator()).?;
    spaces[0].pdPml4Index(0).* = 0;
    spaces[0].pdPdpIndex(0).* = 2;
    spaces[0].pd_page_used_len = 1;
    const original = physical | 0x83;
    spaces[0].pdPage(0).*[0] = original;
    var kernel_directory = original;
    var user_leaf = physical | 7 | (@as(u64, 1) << 63);
    const kernel_source: paging.AliasSource = .{ .identity = .{ .directory = &kernel_directory, .base = physical, .alias_va = @import("arch/x86_64/direct_map.zig").base + (physical) } };
    var leaves: [3]paging.Leaf = undefined;
    var splits: [2]plan_module.Split = undefined;
    var sources = [_]paging.AliasSource{ .{ .leaf = &user_leaf }, kernel_source, kernel_source, .{ .leaf = &user_leaf } };
    var plan = try plan_module.prepare(&.{physical}, &sources, &leaves, &splits);
    try std.testing.expectEqual(@as(usize, 2), plan.leaves.len);
    try std.testing.expectEqual(@as(usize, 1), plan.splits.len);
    try std.testing.expectEqual(original, kernel_directory);
    try std.testing.expectEqual(original, spaces[0].pdPage(0).*[0]);
    const allocated = pt_alloc_calls;
    try plan.validatePolicy(&.{});
    plan.withdraw();
    for (plan.leaves) |leaf| try std.testing.expectEqual(@as(u64, 0), leaf.entry.* & 1);
    // Host mock PTEs only. The real writer must complete the external
    // all-CPU TLB/cache barriers before this publication.
    plan.publish(4);
    for (plan.leaves) |leaf| {
        try std.testing.expectEqual(@as(u64, 0x80), leaf.entry.* & 0x98);
        try std.testing.expectEqual(@as(u64, 1), leaf.entry.* & 1);
    }
    try std.testing.expectEqual(allocated, pt_alloc_calls);
    const kernel_page = blk: {
        for (plan.splits) |split| switch (split) {
            .kernel => |owned| break :blk owned.split.table,
        };
        unreachable;
    };
    try std.testing.expect(vm.ramCacheKernelAliasSplitOwns(@import("arch/x86_64/direct_map.zig").base + physical, .large, @intFromPtr(kernel_page)));
    try std.testing.expect(!vm.ramCacheKernelAliasSplitOwns(@import("arch/x86_64/direct_map.zig").base + physical + (1 << 21), .large, @intFromPtr(kernel_page)));
    const pages_before_release = pt_live_pages;
    plan.release();
    plan.release();
    try std.testing.expectEqual(pages_before_release, pt_live_pages);
    // Synthetic kernel directory has no hardware readers. The kernel-owned
    // page would persist in production; detach explicitly to end this fixture.
    kernel_directory = original;
    vm.releaseRamCacheKernelSplitsForTest();
    try std.testing.expect(!vm.ramCacheKernelAliasSplitOwns(@import("arch/x86_64/direct_map.zig").base + physical, .large, @intFromPtr(kernel_page)));
}

test "MMIO overlay RAM direct huge and user aliases split together and retain ownership" {
    const plan_api = @import("memory/ram_cache_vm_plan.zig");
    const paging = @import("memory/ram_cache_paging.zig");
    const direct = @import("arch/x86_64/direct_map.zig");
    init();
    defer cleanup();
    const pa: u64 = 20 << 30;
    const pages = [_]u64{ pa + 4096, pa + (1 << 21) + 8192 };
    var pdp: [512]u64 align(4096) = @splat(0);
    const huge = &pdp[(pa >> 30) & 511];
    huge.* = pa | (@as(u64, 1) << 63) | 0x83;
    var records = [_]direct.Table{.{ .physical = @intFromPtr(&pdp), .words = &pdp }};
    var draft = direct.Draft{ .tables = &records, .published = true };
    draft.roots[0] = @intFromPtr(&pdp) | (@as(u64, 1) << 63) | 3;
    const layout = try direct.Layout.init(&.{.{ .base = pa, .bytes = 1 << 30, .usable = true }}, &.{}, 48, true);
    const SplitView = struct {
        pub fn borrow(_: @This(), va: u64, size: paging.LeafSize, address: u64) ?*[512]u64 {
            if (!vm.ramCacheKernelAliasSplitOwns(va, size, address)) return null;
            return @ptrFromInt(address);
        }
    };
    const Collector = struct {
        entries: []paging.AliasSource,
        used: usize = 0,
        pub fn visit(self: *@This(), source: paging.AliasSource) !void {
            if (self.used == self.entries.len) return error.NoSpace;
            self.entries[self.used] = source;
            self.used += 1;
        }
    };
    var direct_sources: [2]paging.AliasSource = undefined;
    var collector = Collector{ .entries = &direct_sources };
    try draft.visitAliases(layout, &pages, SplitView{}, &collector, Collector.visit);
    try std.testing.expectEqual(@as(usize, 1), collector.used);
    var user: u64 = pages[0] | (@as(u64, 1) << 63) | 7;
    var sources = [_]paging.AliasSource{
        direct_sources[0],
        .{ .leaf = &user },
    };
    var leaves: [4]paging.Leaf = undefined;
    var splits: [4]plan_api.Split = undefined;
    var plan = try plan_api.prepare(&pages, &sources, &leaves, &splits);
    defer plan.release();
    try std.testing.expectEqual(@as(usize, 3), plan.splits.len);
    try std.testing.expectEqual(@as(usize, 3), plan.leaves.len);
    try plan.validatePolicy(&.{});
    try plan.validateStorage(&pages);
    const allocation_calls = pt_alloc_calls;
    plan.withdraw();
    for (plan.leaves) |leaf| try std.testing.expectEqual(@as(u64, 0), leaf.entry.* & 1);
    // Native commit supplies the all-CPU ACK/cache barrier here.
    plan.publish(4);
    for (plan.leaves) |leaf| try std.testing.expectEqual(@as(u64, 0x81), leaf.entry.* & 0x99);
    try std.testing.expectEqual(allocation_calls, pt_alloc_calls);
    collector.used = 0;
    try draft.visitAliases(layout, &pages, SplitView{}, &collector, Collector.visit);
    try std.testing.expectEqual(@as(usize, 2), collector.used);
    for (direct_sources) |source| {
        try std.testing.expect(source == .leaf);
        try std.testing.expectEqual(@as(u64, 0x81), source.leaf.* & 0x99);
    }
    const saved_link = huge.*;
    huge.* = 0xdead000 | (@as(u64, 1) << 63) | 3;
    collector.used = 0;
    try std.testing.expectError(error.InvalidTable, draft.visitAliases(layout, &pages, SplitView{}, &collector, Collector.visit));
    huge.* = saved_link;
    const pd: *[512]u64 = @ptrFromInt(huge.* & 0x000f_ffff_ffff_f000);
    try std.testing.expect(vm.ramCacheKernelAliasSplitOwns(direct.base + pa, .huge, @intFromPtr(pd)));
    const pt: *[512]u64 = @ptrFromInt(pd[0] & 0x000f_ffff_ffff_f000);
    try std.testing.expect(vm.ramCacheKernelAliasSplitOwns(direct.base + pa, .large, @intFromPtr(pt)));
    try std.testing.expect(!vm.ramCacheKernelAliasSplitOwns(pa, .large, @intFromPtr(pt)));
    try std.testing.expectEqual(@as(u64, 0), pt[0] & 0x98); // Neighbor remains WB.
    try std.testing.expectEqual((pa + (2 << 21)) | (@as(u64, 1) << 63) | 0xe3, pd[2]);
}

test "MMIO overlay RAM huge split preparation failure keeps every live alias unchanged" {
    const plan_api = @import("memory/ram_cache_vm_plan.zig");
    const paging = @import("memory/ram_cache_paging.zig");
    const direct = @import("arch/x86_64/direct_map.zig");
    for (0..3) |capacity| {
        init();
        defer cleanup();
        const pa: u64 = 20 << 30;
        const original = pa | (@as(u64, 1) << 63) | 0x83;
        var huge = original;
        var sources = [_]paging.AliasSource{.{ .identity = .{
            .directory = &huge,
            .base = pa,
            .size = .huge,
            .alias_va = direct.base + pa,
        } }};
        var leaves: [2]paging.Leaf = undefined;
        var splits: [3]plan_api.Split = undefined;
        try std.testing.expectError(error.NoSpace, plan_api.prepare(&.{ pa + 4096, pa + (1 << 21) }, &sources, &leaves, splits[0..capacity]));
        try std.testing.expectEqual(original, huge);
        try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
    }
}

test "MMIO overlay RAM huge split allocation failures never expose partial PD PT chains" {
    const plan_api = @import("memory/ram_cache_vm_plan.zig");
    const paging = @import("memory/ram_cache_paging.zig");
    const direct = @import("arch/x86_64/direct_map.zig");
    var failures: usize = 0;
    var failure: usize = 1;
    while (true) : (failure += 1) {
        init();
        defer cleanup();
        const pa: u64 = 20 << 30;
        const original = pa | (@as(u64, 1) << 63) | 0x83;
        var huge = original;
        var sources = [_]paging.AliasSource{.{ .identity = .{
            .directory = &huge,
            .base = pa,
            .size = .huge,
            .alias_va = direct.base + pa,
        } }};
        var leaves: [2]paging.Leaf = undefined;
        var splits: [3]plan_api.Split = undefined;
        pt_fail_at = pt_alloc_calls + failure;
        if (plan_api.prepare(&.{ pa + 4096, pa + (1 << 21) }, &sources, &leaves, &splits)) |prepared| {
            var plan = prepared;
            plan.release();
            try std.testing.expectEqual(original, huge);
            try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
            break;
        } else |err| {
            try std.testing.expectEqual(error.OutOfMemory, err);
            failures += 1;
            try std.testing.expectEqual(original, huge);
            try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
        }
    }
    try std.testing.expect(failures >= 3); // PD plus two distinct PTs, including storage metadata.
}

test "MMIO overlay RAM PTE plan abort unwinds all unpublished split pages" {
    const plan_module = @import("memory/ram_cache_vm_plan.zig");
    const paging = @import("memory/ram_cache_paging.zig");
    init();
    defer cleanup();
    // The directory is lower in memory than the invalid leaf, so source
    // sorting still allocates a split before the second source fails.
    var entries: extern struct { directory: u64, unrelated: u64 } = .{
        .directory = physical | 0x83,
        .unrelated = physical + 4096 | 7,
    };
    const source: paging.AliasSource = .{ .identity = .{ .directory = &entries.directory, .base = physical, .alias_va = @import("arch/x86_64/direct_map.zig").base + (physical) } };
    var leaves: [2]paging.Leaf = undefined;
    var splits: [1]plan_module.Split = undefined;
    var sources = [_]paging.AliasSource{source};
    try std.testing.expectError(error.NoSpace, plan_module.prepare(&.{physical}, &sources, leaves[0..0], &splits));
    try std.testing.expectEqual(@as(usize, 0), pt_live_pages); // Unpublished storage fully reclaimed.
    try std.testing.expectEqual(physical | 0x83, entries.directory);
    var invalid_sources = [_]paging.AliasSource{ source, .{ .leaf = &entries.unrelated } };
    try std.testing.expectError(error.Invalid, plan_module.prepare(&.{physical}, &invalid_sources, &leaves, &splits));
    try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
    try std.testing.expectEqual(physical | 0x83, entries.directory);
    var plan = try plan_module.prepare(&.{physical}, &sources, &leaves, &splits);
    try std.testing.expect(!vm.ramCacheKernelAliasSplitOwns(@import("arch/x86_64/direct_map.zig").base + physical, .large, splits[0].kernel.split.table_paddr));
    plan.release();
    try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
    try std.testing.expectEqual(physical | 0x83, entries.directory);
}

test "MMIO overlay RAM PTE plan refuses conflicting or pending physical policy" {
    const plan_module = @import("memory/ram_cache_vm_plan.zig");
    const paging = @import("memory/ram_cache_paging.zig");
    const policy = @import("memory/ram_cache_policy.zig");
    init();
    defer cleanup();
    var entry = physical | 0x87 | (@as(u64, 1) << 63);
    var leaves: [1]paging.Leaf = undefined;
    var splits: [0]plan_module.Split = .{};
    var sources = [_]paging.AliasSource{.{ .leaf = &entry }};
    var plan = try plan_module.prepare(&.{physical}, &sources, &leaves, &splits);
    defer plan.release();
    try std.testing.expectError(error.CacheAliasConflict, plan.validatePolicy(&.{}));
    const wc = [_]policy.Span{.{ .base = physical, .bytes = 4096, .owner = 7, .mode = .wc, .pat_slot = 4 }};
    try plan.validatePolicy(&.{ .policy = &wc });
    try std.testing.expectError(error.PendingTransition, plan.validatePolicy(&.{ .policy = &wc, .pending = &wc }));
    try std.testing.expectEqual(physical | 0x87 | (@as(u64, 1) << 63), entry);
}
fn ptAllocator() pt_storage.Allocator {
    return .{ .context = &pt_context, .acquire = allocatePt, .release = releasePt };
}
// Synthetic host tables are never installed in a CPU; no hardware references
// survive a test. Production callers require completed detach/shootdown.
fn cleanup() void {
    retirement = .{};
    @import("memory/free_page_list.zig").ReturnProbe.before_publish = null;
    for (&spaces) |*space| {
        space.pt_store.freeDetached();
        space.pd_store.freeDetached();
        space.pdp_store.freeDetached();
        space.scratch_store.freeDetached();
        space.reservations.deinit();
        space.reservation_draft.deinit();
    }
    vm.releaseRamCacheKernelSplitsForTest();
    std.debug.assert(pt_live_pages == 0);
}
var spaces: [2]vm.UserAddressSpace align(4096) = .{ .{}, .{} };
var table: vm.UserAddressSpaceTable = undefined;
// One serial, fixture-local history for both PT storage and actual PMM returns.
// Every captured alias is a separate obligation; another CR3's flush cannot
// discharge it. Capture again on a new allocation/publication lifetime.
const RetirementOrder = struct {
    const Kind = enum { data, pt };
    const Record = struct {
        kind: Kind,
        space: usize,
        cr3: u64,
        slot: usize,
        va: u64,
        paddr: u64,
        root: *const u64,
        pdp: *const u64,
        parent: *const u64,
        leaf: ?*const u64,
        pt_paddr: u64,
        ack: usize = 0,
        returned: bool = false,

        fn reachable(self: Record) bool {
            if (self.root.* & 1 == 0 or self.pdp.* & 1 == 0 or
                self.parent.* & 1 == 0 or self.parent.* & 0x000f_ffff_ffff_f000 != self.pt_paddr) return false;
            if (self.leaf) |entry| return entry.* & 5 == 5 and entry.* & 0x000f_ffff_ffff_f000 == self.paddr;
            return true;
        }
    };
    records: [4096]Record = undefined,
    len: usize = 0,
    sequence: usize = 0,
    returns: usize = 0,
    acknowledgements: [2]usize = .{ 0, 0 },
    cow: ?struct {
        state: *kernel.KernelState,
        free_list: *kernel.FreePageList,
        old_ref: kernel.NativeCowTableRef,
        free_pages: usize,
        checked: *bool,
    } = null,
    // The source must still own its VMA when staged clear reaches the barrier.
    exec_source: ?*kernel.VmaTable = null,

    fn capture(self: *@This()) void {
        self.* = .{};
        for (&spaces, 0..) |*space, si| {
            if (space.cr3 == 0) space.cr3 = @intFromPtr(&space.pml4);
            var pt_it = space.pt_store.iterator();
            while (pt_it.next()) |slot| {
                const desc = space.pt_store.descriptor(slot).?;
                if (desc.page == null or desc.pml4 >= 256 or desc.pdp >= 512 or desc.pd >= 512) continue;
                const pdp: *[512]u64 = @ptrFromInt(space.pml4[desc.pml4] & 0x000f_ffff_ffff_f000);
                const pd: *[512]u64 = @ptrFromInt(pdp[desc.pdp] & 0x000f_ffff_ffff_f000);
                const record = Record{
                    .kind = .pt,
                    .space = si,
                    .cr3 = space.cr3,
                    .slot = slot,
                    .va = (@as(u64, desc.pml4) << 39) | (@as(u64, desc.pdp) << 30) | (@as(u64, desc.pd) << 21),
                    .paddr = @intFromPtr(desc.page.?),
                    .pt_paddr = @intFromPtr(desc.page.?),
                    .root = &space.pml4[desc.pml4],
                    .pdp = &pdp[desc.pdp],
                    .parent = &pd[desc.pd],
                    .leaf = null,
                };
                self.records[self.len] = record;
                self.len += 1;
                for (desc.page.?, 0..) |*entry, page_index| {
                    if (entry.* & 5 != 5) continue;
                    var data = record;
                    data.kind = .data;
                    data.va += page_index * 4096;
                    data.paddr = entry.* & 0x000f_ffff_ffff_f000;
                    data.leaf = entry;
                    self.records[self.len] = data;
                    self.len += 1;
                }
            }
        }
        @import("memory/free_page_list.zig").ReturnProbe.before_publish = observeDataReturn;
    }

    fn flushed(self: *@This(), principal: kernel.PrincipalId, va: u64, bytes: usize) void {
        const si = kernel.processIndexFromPrincipal(principal).?;
        self.sequence += 1;
        self.acknowledgements[si] += 1;
        if (self.exec_source) |source| {
            if (si == 1) std.debug.assert(source.active_count != 0); // exec ownership before ACK
        }
        // Leaf ACKs only cover their requested pages. A detached table needs
        // the full user-context request, independent of the leaf range.
        for (self.records[0..self.len]) |*record| {
            if (record.returned or record.space != si or record.cr3 != spaces[si].cr3) continue;
            const full = va == 4096 and bytes == (1 << 47) - 4096;
            const covered = if (record.kind == .pt) full else full or (record.va >= va and record.va - va < bytes);
            if (covered and !record.reachable()) record.ack = self.sequence;
        }
    }

    fn returning(self: *@This(), pa: u64, pages: usize, kind: Kind) void {
        for (self.records[0..self.len]) |*record| {
            if (record.kind != kind or record.paddr < pa or record.paddr >= pa + pages * 4096) continue;
            std.debug.assert(!record.returned);
            std.debug.assert(record.ack != 0); // return requires this CR3's completed withdrawal
            std.debug.assert(!record.reachable());
            record.returned = true;
            self.returns += 1;
        }
        self.sequence += 1;
    }
};
var retirement: RetirementOrder = .{};
fn observeDataReturn(pa: u64, pages: usize) void {
    retirement.returning(pa, pages, .data);
}

var flushes: usize = 0;
var expect_full_context: bool = false;
var translations: usize = 0;
var fail_translation: usize = 0;
var reclaim_flush_first: ?usize = null;

const addresses = @import("memory/addresses.zig");

fn translate(address: addresses.KernelVirtualAddress) ?addresses.PhysicalAddress {
    translations += 1;
    if (translations == fail_translation) return null;
    std.debug.assert(address.toInt() & 4095 == 0);
    return .fromInt(address.toInt());
}

fn borrowHostTable(address: addresses.PhysicalAddress) ?*const [512]u64 {
    // This fixture encodes host pointers as fake PTE physical addresses;
    // it must not claim that host allocations inhabit the kernel identity map.
    if (address.toInt() == 0 or address.toInt() & 4095 != 0) return null;
    return @ptrFromInt(address.toInt());
}
fn seed(page: []u64) void {
    @memset(page, 0);
}
fn flushPage(principal: kernel.PrincipalId, va: u64) void {
    flushes += 1;
    retirement.flushed(principal, va & ~@as(u64, 4095), 4096);
}
fn flushRange(principal: kernel.PrincipalId, va: u64, bytes: usize) void {
    if (expect_full_context) std.debug.assert(va == 4096 and bytes == (1 << 47) - 4096);
    if (g3_retirement) |probe| probe.acknowledge(principal);
    flushes += 1;
    if (teardown_check) |check| {
        std.debug.assert(pt_live_pages == check.pages);
        std.debug.assert(std.mem.allEqual(u64, spaces[check.index].pml4[0..256], 0));
        std.debug.assert(va == 4096 and bytes == (1 << 47) - 4096);
    }
    if (reclaim_flush_first) |first| {
        std.debug.assert(va == 4096 and bytes == (1 << 47) - 4096);
        // All parents are gone, but no storage is reusable before this
        // synchronous shootdown callback returns.
        std.debug.assert(usedPtSlots() == 512);
        for (first..512) |slot| {
            std.debug.assert(spaces[0].pdPage(0).*[slot] == 0);
            std.debug.assert(spaces[0].ptPdIndex(slot).* == slot);
        }
    }
    if (retirement.cow) |check| {
        std.debug.assert(!check.state.nativeCowTableIsUnique(check.old_ref));
        std.debug.assert(check.free_list.len == check.free_pages);
        std.debug.assert(vm.lookupUserMappedPaddrForVa(owner, base) == null);
        check.checked.* = true;
    }
    retirement.flushed(principal, va, bytes);
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
    table.initializeFreshStorage(&spaces);
    flushes = 0;
    translations = 0;
    fail_translation = 0;
    reclaim_flush_first = null;
    retirement.cow = null;
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

test "MMIO overlay dynamic PTs cross 64 and 512 with one acknowledged retirement" {
    for ([_]usize{ 65, 513, 640 }) |count| {
        init();
        defer cleanup();
        for (0..count) |i| {
            const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, i / 512, i % 512).?;
            spaces[0].ptPage(slot)[0] = physical | 7;
        }
        try std.testing.expectEqual(count, usedPtSlots());
        const metadata_pages = (count + 63) / 64;
        try std.testing.expectEqual(count + metadata_pages + 6 + (count + 511) / 512, pt_live_pages);
        retirement.capture();
        try std.testing.expect(vm.unmapPresentRemoteUserRegion(owner, base, count * (1 << 21)));
        try std.testing.expectEqual(@as(usize, 0), usedPtSlots());
        try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
        try std.testing.expectEqual(@as(usize, 1), flushes);
        // Freed slots are reused and lazily acquire new bodies, not new banks.
        try std.testing.expectEqual(@as(usize, 0), spaces[0].ptCapacity());
        try std.testing.expectEqual(@as(?usize, 0), vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0));
        try std.testing.expectEqual(@as(usize, 64), spaces[0].ptCapacity());
        try std.testing.expectEqual(@as(usize, 9), pt_live_pages);
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
    try std.testing.expectError(error.InvalidState, vm.cloneAddressSpaceMetadataForFork(owner, target));
    try std.testing.expectEqual(initial_pages, pt_live_pages);
    try std.testing.expectEqual(initial_flushes, flushes);
    try std.testing.expectEqual(old_target_page, spaces[1].ptPage(0));
    try std.testing.expectEqualSlices(u64, &old_target_pml4, &spaces[1].pml4);
    fail_translation = 0;
    teardown_check = .{ .index = 1, .pages = pt_live_pages + 1 }; // Prepared replacement PDP.
    retirement.capture();
    try vm.cloneAddressSpaceMetadataForFork(owner, target);
    teardown_check = null;
    try std.testing.expectEqual(initial_pages - 6, pt_live_pages);
    try std.testing.expectEqual(@as(usize, 0), spaces[1].ptCapacity());
    try std.testing.expectEqual(last, spaces[0].ptPage(512));
    try std.testing.expectEqual(@as(u64, physical | 7), last[0]);
    try std.testing.expect(spaces[1].pml4[0] != 0);
    spaces[0].cr3 = @intFromPtr(&spaces[0].pml4);
    teardown_check = .{ .index = 0, .pages = pt_live_pages };
    retirement.capture();
    vm.clearUserAddressSpace(owner);
    teardown_check = null;
    try std.testing.expectEqual(@as(usize, 3), pt_live_pages); // Destination empty PDP.
    vm.clearUserAddressSpace(target);
    try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
    try std.testing.expectEqual(@as(usize, 0), spaces[0].ptCapacity());
}

test "MMIO overlay dynamic PT journal grows beyond 638 new tables" {
    for ([_]usize{ 637, 638, 1025 }) |count| {
        init();
        defer cleanup();
        try std.testing.expect(vm.mapUserMmioOverlay(owner, base, physical, count * (1 << 21), .{ .read = true }));
        try std.testing.expectEqual(count, usedPtSlots());
        try std.testing.expect(vm.unmapUserMmioOverlay(owner, base, physical, count * (1 << 21)));
        try noTables();
        try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
    }
}

test "MMIO overlay COW invalidates before releasing shared dirty backing" {
    var backing = try @import("memory/physical_access_test.zig").BackingSet.initMixed(&.{.{ .first = physical, .pages = 16 }});
    defer backing.deinit();
    init();
    defer cleanup();
    const runtime_storage = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(runtime_storage);
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage));
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    try state.initFromDetectedRegionsInPlace(1);
    state.invocation_test_allocator = invocation_allocator.interface();
    var free_list = kernel.FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = physical, .end = (physical) + (16) * 4096 }});
    try free_list.seedContiguousRange(physical, 16);
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
    retirement.capture();
    var checked = false;
    retirement.cow = .{ .state = state, .free_list = &free_list, .old_ref = old_ref, .free_pages = free_list.len, .checked = &checked };
    defer retirement.cow = null;
    try std.testing.expect(vm.lockAddressSpace(owner));
    const mapping = @import("user_copy.zig").resolveNativeVmaCowMappingWithAddressSpaceLocked(state, &free_list, owner, base, true, false);
    vm.unlockAddressSpace(owner);
    retirement.cow = null;
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
    return usedSlots(&spaces[0].pt_store);
}
fn usedSlots(store: *const pt_storage.Store) usize {
    var count: usize = 0;
    var it = store.iterator();
    while (it.next()) |slot| count += @intFromBool(store.descriptorConst(slot).?.pml4 != vm.UserAddressSpace.no_pd_index);
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
    try std.testing.expectEqual(@as(usize, 0), usedSlots(&spaces[0].pd_store));
    try std.testing.expectEqual(@as(usize, 0), usedSlots(&spaces[0].pdp_store));
}
const SnapshotPt = struct {
    pml4: u16,
    pdp: u16,
    pd: u16,
    address: usize,
    data: [512]u64,
};
const Snapshot = struct { reservations: []vm.UserAddressSpace.Reservation, space: vm.UserAddressSpace, pts: []SnapshotPt, pds: []SnapshotPt, pdps: []SnapshotPt };
fn snapshotReservations() ![]vm.UserAddressSpace.Reservation {
    const records = try std.testing.allocator.alloc(vm.UserAddressSpace.Reservation, @intCast(spaces[0].reservations.live));
    var iterator = spaces[0].reservations.iterator();
    for (records) |*record| record.* = iterator.next().?.value.*;
    return records;
}
fn unchangedReservations(before: []const vm.UserAddressSpace.Reservation) !void {
    const after = try snapshotReservations();
    defer std.testing.allocator.free(after);
    try std.testing.expectEqualDeep(before, after);
}
fn snapshotStore(store: *const pt_storage.Store) ![]SnapshotPt {
    const result = try std.testing.allocator.alloc(SnapshotPt, store.capacity());
    for (result, 0..) |*item, slot| {
        const entry = store.descriptorConst(slot) orelse {
            item.* = .{ .pml4 = 0xffff, .pdp = 0xffff, .pd = 0xffff, .address = 0, .data = @splat(0) };
            continue;
        };
        item.* = .{ .pml4 = entry.pml4, .pdp = entry.pdp, .pd = entry.pd, .address = if (entry.page) |page| @intFromPtr(page) else 0, .data = if (entry.page) |page| page.* else @splat(0) };
    }
    return result;
}
fn snapshot() !*Snapshot {
    const copy = try std.testing.allocator.create(Snapshot);
    errdefer std.testing.allocator.destroy(copy);
    copy.reservations = try snapshotReservations();
    errdefer std.testing.allocator.free(copy.reservations);
    copy.space = spaces[0];
    copy.pts = try snapshotStore(&spaces[0].pt_store);
    errdefer std.testing.allocator.free(copy.pts);
    copy.pds = try snapshotStore(&spaces[0].pd_store);
    errdefer std.testing.allocator.free(copy.pds);
    copy.pdps = try snapshotStore(&spaces[0].pdp_store);
    return copy;
}
fn destroySnapshot(copy: *Snapshot) void {
    std.testing.allocator.free(copy.reservations);
    std.testing.allocator.free(copy.pts);
    std.testing.allocator.free(copy.pds);
    std.testing.allocator.free(copy.pdps);
    std.testing.allocator.destroy(copy);
}
fn unchangedStore(before: []const SnapshotPt, store: *const pt_storage.Store) !void {
    for (before, 0..) |item, slot| {
        if (item.address == 0 and item.pml4 == 0xffff) continue;
        const entry = store.descriptorConst(slot) orelse return error.TestUnexpectedResult;
        try std.testing.expectEqual(item.pml4, entry.pml4);
        try std.testing.expectEqual(item.pdp, entry.pdp);
        try std.testing.expectEqual(item.pd, entry.pd);
        try std.testing.expectEqual(item.address, if (entry.page) |page| @intFromPtr(page) else @as(usize, 0));
        if (entry.page) |page| try std.testing.expectEqualSlices(u64, &item.data, page);
    }
    var it = store.iterator();
    while (it.next()) |slot| {
        if (slot < before.len and before[slot].address != 0) continue;
        const entry = store.descriptorConst(slot).?;
        try std.testing.expectEqual(@as(u16, 0xffff), entry.pml4);
        try std.testing.expect(entry.page == null);
    }
}
fn unchanged(copy: *const Snapshot) !void {
    // Snapshot all three levels: a shallow Store copy cannot detect writes to
    // the now out-of-line parent tables. Empty metadata may shrink on rollback.
    var before = copy.space;
    var after = spaces[0];
    if (@import("sync/lock_diagnostics.zig").enabled) {
        try std.testing.expect(!after.lock_state.diagnostic_record.linked);
        before.lock_state.diagnostic_record = .{};
        after.lock_state.diagnostic_record = .{};
    }
    inline for (.{ "pt_store", "pd_store", "pdp_store", "scratch_store", "reservations", "reservation_draft" }) |field| {
        @field(before, field) = .{};
        @field(after, field) = .{};
    }
    try std.testing.expectEqualDeep(before, after);
    try unchangedReservations(copy.reservations);
    try unchangedStore(copy.pts, &spaces[0].pt_store);
    try unchangedStore(copy.pds, &spaces[0].pd_store);
    try unchangedStore(copy.pdps, &spaces[0].pdp_store);
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
    const reservations = try snapshotReservations();
    defer std.testing.allocator.free(reservations);
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
    try unchangedReservations(reservations);
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
                1 => spaces[0].pdPage(0).*[511] = 0,
                2 => spaces[0].pdPage(0).*[511] |= 128,
                3 => spaces[0].pdPage(0).*[511] = physical | 7,
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
    try std.testing.expect(vm.unmapPresentVmaSourceSplitSlotsRequired(owner, base, size) == null);
    try std.testing.expect(vm.unmapPresentRemoteUserRegionSplitSlotsRequired(owner, base, size) == null);
    try std.testing.expect(!vm.unmapPresentVmaSource(owner, base, size));
    try std.testing.expect(!vm.unmapPresentRemoteUserRegion(owner, base, size));
    try unchanged(before);
    spaces[0].ptPage(supervisor_slot).*[0] = 0;
    try std.testing.expectEqual(@as(?usize, 0), vm.unmapPresentVmaSourceSplitSlotsRequired(owner, base, size));
    try std.testing.expect(vm.unmapPresentVmaSource(owner, base, size));
    try std.testing.expectEqual(@as(usize, 0), usedPtSlots());
}

test "MMIO overlay ordinary empty unmap skips only unchanged translations" {
    init();
    defer cleanup();
    // No PT exists for this untouched range.
    const reservation_count = spaces[0].reservations.live;
    try std.testing.expect(vm.reserveUserMapping(owner, base, 1, .linear_region, true));
    try std.testing.expect(vm.unmapPresentUserLinearRegion(owner, base, 4096));
    try std.testing.expectEqual(@as(usize, 0), flushes);
    try std.testing.expectEqual(reservation_count, spaces[0].reservations.live);
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
        const reservations = try snapshotReservations();
        defer std.testing.allocator.free(reservations);
        const generation = spaces[0].reservation_generation;
        try std.testing.expect(vm.mapUserMmioOverlay(owner, target, physical, 8192, .{ .read = true }));
        try std.testing.expectEqual(@as(?u64, physical), vm.lookupUserMappedPaddrForVa(owner, target));
        try std.testing.expectEqual(@as(?u64, physical + 4096), vm.lookupUserMappedPaddrForVa(owner, target + 4096));
        var pt_it = spaces[0].pt_store.iterator();
        while (pt_it.next()) |slot| {
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
        try unchangedReservations(reservations);
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
    var pt_it = spaces[0].pt_store.iterator();
    while (pt_it.next()) |slot| {
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

test "MMIO overlay WC uses proven PAT slot four and unmaps cleanly" {
    init();
    defer cleanup();
    try std.testing.expect(vm.mapUserMmioOverlayWithCache(owner, base, physical, 8192, .{ .read = true, .write = true }, .wc_pat4));
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
    for (spaces[0].ptPage(slot).*[0..2]) |pte| {
        try std.testing.expectEqual(@as(u64, 1 << 7), pte & ((1 << 7) | (1 << 4) | (1 << 3)));
        try std.testing.expect((pte & 0x7) == 0x7);
    }
    try std.testing.expect(vm.unmapUserMmioOverlay(owner, base, physical, 8192));
    try noTables();
}

test "MMIO overlay starts with absent leaves in the formerly reserved low window" {
    init();
    defer cleanup();
    const target: u64 = 0x71c0_0000;
    const bytes: usize = 512 << 10;
    const index: usize = @intCast((target >> 21) & 0x1ff);
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 0, 1, index).?;
    const page = spaces[0].ptPage(slot);
    for (0..bytes / 4096) |i|
        try std.testing.expectEqual(@as(u64, 0), page[i]);

    try std.testing.expect(vm.mapUserMmioOverlayWithCache(owner, target, physical, bytes, .{ .read = true, .write = true }, .uc_minus));
    try std.testing.expectEqual(@as(usize, 0), flushes); // No old translation exists.
    try std.testing.expectEqual(@as(?u64, physical), vm.lookupUserMappedPaddrForVa(owner, target));
    const mapped = try snapshot();
    defer destroySnapshot(mapped);
    try std.testing.expect(!vm.mapUserMmioOverlay(owner, target, physical, 4096, .{ .read = true }));
    try unchanged(mapped); // Existing user PTE cannot be replaced.

    try std.testing.expect(vm.unmapUserMmioOverlay(owner, target, physical, bytes));
    try std.testing.expectEqual(@as(?u64, null), vm.lookupUserMappedPaddrForVa(owner, target));
    const before_reuse = flushes;
    try std.testing.expect(vm.mapUserMmioOverlay(owner, target, physical, 4096, .{ .read = true }));
    try std.testing.expectEqual(before_reuse, flushes); // Cleared PTE needs no publish shootdown.
    try std.testing.expect(vm.unmapUserMmioOverlay(owner, target, physical, 4096));
}

test "MMIO overlay preflight rejects a supervisor PTE atomically" {
    init();
    defer cleanup();
    const target: u64 = 0x71c0_0000;
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 0, 1, @intCast((target >> 21) & 0x1ff)).?;
    const page = spaces[0].ptPage(slot);
    page[127] = physical | 3; // Unexpected supervisor leaf must never be replaced.
    const before = try snapshot();
    defer destroySnapshot(before);
    try std.testing.expect(!vm.mapUserMmioOverlay(owner, target, physical, 512 << 10, .{ .read = true }));
    try unchanged(before);
    try std.testing.expectEqual(@as(usize, 0), flushes);
    page[127] = target + 127 * 4096 | 7; // User-owned identity mapping is not replaceable.
    const with_user = try snapshot();
    defer destroySnapshot(with_user);
    try std.testing.expect(!vm.mapUserMmioOverlay(owner, target, physical, 512 << 10, .{ .read = true }));
    try unchanged(with_user);
}

test "MMIO overlay identity preflight rolls back a later PT allocation failure" {
    init();
    defer cleanup();
    const target: u64 = 0x71df_f000;
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 0, 1, @intCast((target >> 21) & 0x1ff)).?;
    try std.testing.expectEqual(@as(u64, 0), spaces[0].ptPage(slot).*[511]);
    const before = try snapshot();
    defer destroySnapshot(before);
    pt_fail_at = pt_alloc_calls + 1;
    try std.testing.expect(!vm.mapUserMmioOverlay(owner, target, physical, 8192, .{ .read = true }));
    try unchanged(before);
    try std.testing.expectEqual(@as(usize, 0), flushes); // New path never became reachable.
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
    var pt_it = spaces[0].pt_store.iterator();
    while (pt_it.next()) |slot| {
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
    try std.testing.expectEqual(@as(usize, 1), usedSlots(&spaces[0].pd_store));
    try std.testing.expectEqual(@as(usize, 1), usedSlots(&spaces[0].pdp_store));
    const before = try snapshot();
    defer destroySnapshot(before);
    try std.testing.expect(!vm.mapUserMmioOverlay(owner, base + 4096, physical, 4096, .{ .read = true }));
    try unchanged(before);
    spaces[0].ptPage(slot).*[1] = 0;
    try std.testing.expect(vm.unmapUserMmioOverlay(owner, base, physical, 4096));
    try noTables();
}

const MmioRetirementFixture = struct {
    backing: @import("memory/physical_access_test.zig").BackingSet,
    storage: []align(4096) u8,
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,

    fn start() !@This() {
        init();
        var backing = try @import("memory/physical_access_test.zig").BackingSet.initMixed(&.{ .{ .first = physical, .pages = 513 }, .{ .first = 0x201000, .pages = 3 } });
        errdefer backing.deinit();
        const storage = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
        errdefer std.testing.allocator.free(storage);
        try std.testing.expect(kernel.initRuntimeStorage(storage));
        const state = try std.testing.allocator.create(kernel.KernelState);
        errdefer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        state.invocation_test_allocator = invocation_allocator.interface();
        const free_list = try std.testing.allocator.create(kernel.FreePageList);
        errdefer std.testing.allocator.destroy(free_list);
        free_list.* = .{};
        return .{ .backing = backing, .storage = storage, .state = state, .free_list = free_list };
    }
    fn end(self: *@This()) void {
        cleanup();
        self.backing.deinit();
        std.testing.allocator.destroy(self.free_list);
        std.testing.allocator.destroy(self.state);
        std.testing.allocator.free(self.storage);
    }
    fn map(self: *@This(), principal: kernel.PrincipalId, va: u64, count: usize) !void {
        _ = try self.state.createAnonymousVmaWithPages(principal, va, count * 4096, .{ .read = true, .write = true }, .{ .read = true, .write = true }, .{ .private = true, .anonymous = true }, self.free_list);
        for (0..count) |i| {
            const mapping = @import("user_copy.zig").resolveNativeVmaFaultMappingWithAddressSpaceLocked(self.state, self.free_list, principal, va + i * 4096, false, false).?;
            try std.testing.expect(vm.mapNativeVmaFaultPage(principal, va + i * 4096, mapping));
        }
    }
    fn syscall(self: *@This(), number: u64, args: [6]u64) !u64 {
        // Only VM/VMO operations below execute. All other hook fields retain
        // poison so an accidental unrelated callback cannot silently succeed.
        var h: @import("syscalls.zig").Hooks = undefined;
        h.free_list = self.free_list;
        var frame = std.mem.zeroes(@import("interrupts.zig").TrapFrame);
        frame.rax = number;
        frame.rdi = args[0];
        frame.rsi = args[1];
        frame.rdx = args[2];
        frame.r10 = args[3];
        frame.r8 = args[4];
        frame.r9 = args[5];
        return @import("syscall/fd.zig").dispatch(h, self.state, owner, &frame) orelse error.TestUnexpectedResult;
    }
};

test "MMIO overlay production munmap returns only acknowledged whole trim and middle pages" {
    const sc = @import("syscall/numbers.zig");
    for ([_][2]usize{ .{ 0, 3 }, .{ 0, 1 }, .{ 2, 1 }, .{ 1, 1 } }) |cut| {
        var f = try MmioRetirementFixture.start();
        defer f.end();
        try f.free_list.installRegions(&.{.{ .id = 0, .start = physical, .end = physical + 128 * 4096 }});
        try f.free_list.seedContiguousRange(physical, 128);
        try f.map(owner, base, 3);
        retirement.capture();
        const before = f.free_list.len;
        try std.testing.expectEqual(sc.syscall_ok, try f.syscall(sc.syscall_munmap, .{ base + cut[0] * 4096, cut[1] * 4096, 0, 0, 0, 0 }));
        try std.testing.expectEqual(before + cut[1], f.free_list.len);
        try std.testing.expect(retirement.returns >= cut[1]);
        try std.testing.expect(retirement.acknowledgements[0] != 0);
        vm.clearUserAddressSpace(owner);
        f.state.releasePrincipalNativeMemory(owner, f.free_list);
        try std.testing.expectEqual(@as(usize, 128), f.free_list.len);
    }
}

test "MMIO overlay production MAP_FIXED and mremap retire source and target before return" {
    const sc = @import("syscall/numbers.zig");
    const abi = @import("kernel_abi_root").vm_abi;
    for (0..3) |mode| {
        var f = try MmioRetirementFixture.start();
        defer f.end();
        try f.free_list.installRegions(&.{.{ .id = 0, .start = physical, .end = physical + 128 * 4096 }});
        try f.free_list.seedContiguousRange(physical, 128);
        try f.map(owner, base, 3);
        if (mode == 2) try f.map(owner, base + 0x10000, 3);
        retirement.capture();
        const result = if (mode == 0)
            try f.syscall(sc.syscall_mmap, .{ 0, base, 3 * 4096, abi.prot_read | abi.prot_write, abi.mmap_fixed | abi.mmap_anonymous | abi.mmap_private, 0 })
        else
            try f.syscall(sc.syscall_mremap, .{ base, 3 * 4096, 4096, if (mode == 2) abi.mremap_maymove | abi.mremap_fixed else 0, if (mode == 2) base + 0x10000 else 0, 0 });
        try std.testing.expectEqual(if (mode == 2) base + 0x10000 else base, result);
        try std.testing.expect(retirement.returns >= 2);
        vm.clearUserAddressSpace(owner);
        f.state.releasePrincipalNativeMemory(owner, f.free_list);
        try std.testing.expectEqual(@as(usize, 128), f.free_list.len);
    }
}

test "MMIO overlay production revoke waits for both address spaces before final FD return" {
    const sc = @import("syscall/numbers.zig");
    var f = try MmioRetirementFixture.start();
    defer f.end();
    try f.free_list.installRegions(&.{.{ .id = 0, .start = physical, .end = physical + 128 * 4096 }});
    try f.free_list.seedContiguousRange(physical, 128);
    const child = kernel.processPrincipalFromIndex(1).?;
    const fd = try f.state.createAnonymousVmoFdWithPages(owner, 4096, .{ .read = true, .write = true, .map_read = true, .map_write = true, .close = true, .revoke = true }, .{}, 0, f.free_list);
    _ = try f.state.mmapFd(owner, fd, base, 4096, .{ .read = true, .write = true }, .{ .shared = true }, 0);
    try f.state.cloneVmaTableForFork(owner, child);
    const pa = f.state.nativeVmaInitialMapping(owner, base).?.paddr;
    try std.testing.expect(vm.mapLazyUserPaddrsWithProt(owner, base, &.{pa}, .{ .read = true }));
    try std.testing.expect(vm.mapLazyUserPaddrsWithProt(child, base, &.{pa}, .{ .read = true }));
    retirement.capture();
    try std.testing.expectEqual(sc.syscall_ok, try f.syscall(sc.syscall_vmo_revoke, .{ fd, 0, 0, 0, 0, 0 }));
    try std.testing.expectEqual(@as(usize, 1), retirement.acknowledgements[0]);
    try std.testing.expectEqual(@as(usize, 1), retirement.acknowledgements[1]);
    try std.testing.expectEqual(@as(usize, 128), f.free_list.len);
}

test "MMIO overlay exec handoff retires staged CR3 once with unchanged shootdown count" {
    var counts: [2]usize = undefined;
    for (0..2) |variant| {
        var f = try MmioRetirementFixture.start();
        defer f.end();
        try f.free_list.installRegions(&.{.{ .id = 0, .start = physical, .end = physical + 128 * 4096 }});
        try f.free_list.seedContiguousRange(physical, 128);
        const staged = kernel.processPrincipalFromIndex(1).?;
        try f.map(owner, base, 1);
        try f.map(staged, base, 1);
        retirement.capture();
        const before = flushes;
        try std.testing.expect(vm.lockVmTransactionPair(staged, owner));
        defer vm.unlockVmTransactionPair(staged, owner);
        try vm.cloneAddressSpaceMetadataForFork(staged, owner);
        // exec's explicit synchronizeUserMemoryForPrincipal is a privileged
        // CR3 rendezvous. Model that unchanged call through the same hook;
        // the production transfer and both actual clear calls run below.
        flushPage(owner, 0);
        if (variant == 0) {
            // Baseline: the old implementation moved VMA ownership first.
            try f.state.replaceVmaTableForExec(owner, staged, f.free_list);
        } else {
            retirement.exec_source = f.state.getVmaTable(staged).?;
            @import("syscall/process.zig").ExecTransferTest.run(f.state, f.free_list, owner, staged);
            retirement.exec_source = null;
            try std.testing.expectEqual(@as(u64, 0), spaces[1].cr3);
            for (retirement.records[0..retirement.len]) |record| {
                if (record.space == 1) try std.testing.expect(record.ack != 0);
            }
        }
        vm.clearUserAddressSpace(staged); // cleanupProcess's existing clear
        counts[variant] = flushes - before;
        try std.testing.expectEqual(@as(usize, 1), retirement.acknowledgements[1]);
        f.state.releasePrincipalNativeMemory(staged, f.free_list);
        vm.clearUserAddressSpace(owner);
        f.state.releasePrincipalNativeMemory(owner, f.free_list);
        try std.testing.expectEqual(@as(usize, 128), f.free_list.len);
    }
    try std.testing.expectEqual(@as(usize, 3), counts[0]);
    try std.testing.expectEqual(counts[0], counts[1]);
    std.debug.print("exec shootdowns: before={d} after={d} staged=1 cleanup-extra=0 (unchanged explicit destination rendezvous modeled)\n", .{ counts[0], counts[1] });
}

test "MMIO overlay PMM probe observes successful publication only" {
    var f = try MmioRetirementFixture.start();
    defer f.end();
    try f.free_list.installRegions(&.{.{ .id = 0, .start = physical, .end = physical + 128 * 4096 }});
    try f.free_list.seedContiguousRange(physical, 128);
    retirement.capture();
    const before = retirement.sequence;
    try std.testing.expectError(error.InvalidState, f.free_list.appendPage(physical + 1));
    try std.testing.expectError(error.RegionNotFound, f.free_list.appendPage(physical - 4096));
    try std.testing.expectError(error.FreeRangeOverlap, f.free_list.appendPage(physical));
    try std.testing.expectEqual(before, retirement.sequence);
    const pa = try f.free_list.popFront();
    try f.free_list.appendPage(pa);
    try std.testing.expectEqual(before + 1, retirement.sequence);
}

// Dispatch links trap entry code, but these VM tests must never enter a trap
// or switch hardware contexts. Fail instead of emulating privileged execution.
export fn saveCurrentThreadXState() callconv(.winapi) void {
    @panic("unexpected host XState save");
}
export fn restoreCurrentThreadXState() callconv(.c) void {
    @panic("unexpected host XState restore");
}
export fn resumeAfterFatalUserException(_: kernel.PrincipalId, _: u8, _: *@import("interrupts.zig").TrapFrame) callconv(.winapi) void {
    @panic("unexpected host fatal exception");
}

test "MMIO overlay last FD return inherits both alias acknowledgements" {
    const sc = @import("syscall/numbers.zig");
    var f = try MmioRetirementFixture.start();
    defer f.end();
    try f.free_list.installRegions(&.{.{ .id = 0, .start = physical, .end = physical + 128 * 4096 }});
    try f.free_list.seedContiguousRange(physical, 128);
    const fd = try f.state.createAnonymousVmoFdWithPages(owner, 4096, .{ .map_read = true, .map_write = true, .close = true }, .{}, 0, f.free_list);
    const ref = f.state.nativeVmoRefForFd(owner, fd).?;
    const pa = f.state.nativeVmoPagePaddr(ref, 0).?;
    for ([_]u64{ base, base + 8192 }) |va| {
        _ = try f.state.mmapFd(owner, fd, va, 4096, .{ .read = true }, .{ .shared = true }, 0);
        try std.testing.expect(vm.mapLazyUserPaddrsWithProt(owner, va, &.{pa}, .{ .read = true }));
    }
    retirement.capture();
    for ([_]u64{ base, base + 8192 }) |va| {
        try std.testing.expectEqual(sc.syscall_ok, try f.syscall(sc.syscall_munmap, .{ va, 4096, 0, 0, 0, 0 }));
        try std.testing.expectEqual(@as(usize, 127), f.free_list.len);
    }
    try f.state.closeFdWithFreeList(owner, fd, f.free_list);
    try std.testing.expectEqual(@as(usize, 128), f.free_list.len);
    for (retirement.records[0..retirement.len]) |record| try std.testing.expect(record.returned);
}

test "MMIO overlay remote process unmap uses target CR3 retirement" {
    const sc = @import("syscall/numbers.zig");
    var f = try MmioRetirementFixture.start();
    defer f.end();
    try f.free_list.installRegions(&.{.{ .id = 0, .start = physical, .end = physical + 128 * 4096 }});
    try f.free_list.seedContiguousRange(physical, 128);
    const child = kernel.processPrincipalFromIndex(1).?;
    try f.map(child, base, 2);
    const fd = try f.state.createProcessFd(owner, .{ .principal_raw = @intFromEnum(child) }, .{ .map_into = true }, .{}, 0);
    var h: @import("syscalls.zig").Hooks = undefined;
    h.free_list = f.free_list;
    var frame = std.mem.zeroes(@import("interrupts.zig").TrapFrame);
    frame.rax = sc.syscall_process_unmap;
    frame.rdi = fd;
    frame.rsi = base;
    frame.rdx = 8192;
    retirement.capture();
    try std.testing.expectEqual(@as(?u64, sc.syscall_ok), @import("syscall/process.zig").dispatch(h, f.state, owner, &frame));
    try std.testing.expectEqual(@as(usize, 0), retirement.acknowledgements[0]);
    try std.testing.expectEqual(@as(usize, 1), retirement.acknowledgements[1]);
    try std.testing.expectEqual(@as(usize, 128), f.free_list.len);
}

test "MMIO overlay unpublished PT rollback requires no acknowledgement" {
    init();
    defer cleanup();
    retirement.capture();
    // PDP and PD translation succeed; the new PT address fails before the
    // parent can expose it. All newly acquired storage is returned.
    fail_translation = translations + 3;
    try std.testing.expectEqual(@as(?usize, null), vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0));
    try std.testing.expectEqual(@as(usize, 0), flushes);
    try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
}

// 512 is a PT leaf span, not an admission limit or publication batch.
const SequenceFixture = struct {
    count: usize,
    fail_at: ?usize = null,
    bad_at: ?usize = null,
    calls: usize = 0,
    pub fn at(self: *@This(), index: usize) ?u64 {
        // During the entire preflight no earlier leaf may have been published.
        if (self.calls < self.count)
            std.debug.assert(vm.lookupUserMappedPaddrForVa(owner, base) == null);
        self.calls += 1;
        if (self.fail_at == index) return null;
        return physical + @as(u64, @intCast(index)) * 4096 + @as(u64, if (self.bad_at == index) 1 else 0);
    }
};

test "MMIO overlay page iterator spans PT boundaries and exceeds former PFN array" {
    for ([_]usize{ 511, 512, 513, 131072, 131073, 131079 }) |count| {
        init();
        var source = SequenceFixture{ .count = count };
        try std.testing.expect(vm.lockVmTransaction(owner));
        defer vm.unlockVmTransaction(owner);
        try std.testing.expect(vm.mapTrustedUserPageSequence(owner, base, count, &source, SequenceFixture.at, .{ .read = true, .write = true }, false));
        try std.testing.expectEqual(count * 2, source.calls);
        for (0..count) |i| try std.testing.expectEqual(@as(?u64, physical + i * 4096), vm.lookupUserMappedPaddrForVa(owner, base + i * 4096));
        try std.testing.expectEqual(@as(usize, 1), spaces[0].reservations.capacity - vm.freeUserReservationSlotCount(owner));
        cleanup();
    }
}

test "MMIO overlay page iterator late failure and preparation failure publish nothing" {
    for (0..4) |failure| {
        init();
        defer cleanup();
        const count = 513;
        var source = SequenceFixture{ .count = count };
        if (failure == 0) source.fail_at = count - 1;
        if (failure == 1) source.bad_at = count - 1;
        if (failure == 2) pt_fail_at = 3; // descriptor bank + first PT, then next PT
        if (failure == 3) pt_fail_at = 5; // reservation directory preparation after both PTs
        const reservations = try snapshotReservations();
        defer std.testing.allocator.free(reservations);
        try std.testing.expect(!vm.mapTrustedUserPageSequence(owner, base, count, &source, SequenceFixture.at, .{ .read = true }, false));
        for (0..count) |i| try std.testing.expect(vm.lookupUserMappedPaddrForVa(owner, base + i * 4096) == null);
        try unchangedReservations(reservations);
        if (failure < 2) try std.testing.expectEqual(@as(usize, 0), pt_alloc_calls);
    }
}

test "MMIO overlay page iterator rejects last PTE collision and preserves low zero policy" {
    init();
    defer cleanup();
    const count = 513;
    try std.testing.expect(vm.mapTrustedUserPaddrsWithProt(owner, base + (count - 1) * 4096, &.{physical}, .{ .read = true }));
    var source = SequenceFixture{ .count = count };
    const allocations = pt_alloc_calls;
    try std.testing.expect(!vm.mapTrustedUserPageSequence(owner, base, count, &source, SequenceFixture.at, .{ .read = true }, false));
    try std.testing.expectEqual(allocations, pt_alloc_calls);
    try std.testing.expect(vm.lookupUserMappedPaddrForVa(owner, base) == null);
    try std.testing.expectEqual(@as(?u64, physical), vm.lookupUserMappedPaddrForVa(owner, base + (count - 1) * 4096));
    const One = struct {
        fn at(_: void, _: usize) ?u64 {
            return physical;
        }
    };
    try std.testing.expect(!vm.mapTrustedUserPageSequence(owner, 0, 1, {}, One.at, .{ .read = true, .write = true, .exec = true }, true));
    try std.testing.expect(vm.mapTrustedUserPageSequence(owner, 0, 1, {}, One.at, .{ .read = true, .exec = true }, true));
}

var iterator_batch_entries: [2][6]u64 = undefined;
fn copyIteratorBatch(_: kernel.PrincipalId, address: u64, out: []u8) bool {
    if (address != 0x1000 or out.len != @sizeOf(@TypeOf(iterator_batch_entries))) return false;
    @memcpy(out, std.mem.asBytes(&iterator_batch_entries));
    return true;
}

test "MMIO overlay FD iterator maps shared and preserves private ZOD branches" {
    const abi = @import("kernel_abi_root").vm_abi;
    const sc = @import("syscall/numbers.zig");
    for (0..3) |mode| {
        var f = try MmioRetirementFixture.start();
        defer f.end();
        try f.free_list.installRegions(&.{.{ .id = 0, .start = physical, .end = physical + 513 * 4096 }});
        try f.free_list.seedContiguousRange(physical, 513);
        const rights = kernel.FdRights{ .map_read = true, .map_write = true, .share = true, .close = true };
        const fd = if (mode == 2)
            try f.state.createZeroOnDemandVmoFd(owner, 513 * 4096, rights, .{}, 16)
        else
            try f.state.createAnonymousVmoFdWithPages(owner, 513 * 4096, rights, .{}, 16, f.free_list);
        const flags = if (mode == 1) abi.mmap_private else abi.mmap_shared;
        try std.testing.expectEqual(sc.syscall_err_invalid, try f.syscall(sc.syscall_mmap, .{ fd, base, 131073 * 4096, abi.prot_read, flags, 0 }));
        try std.testing.expectEqual(base, try f.syscall(sc.syscall_mmap, .{ fd, base, 513 * 4096, abi.prot_read | abi.prot_write, flags, 0 }));
        if (mode == 0) {
            try std.testing.expectEqual(@as(?u64, physical + 512 * 4096), vm.lookupUserMappedPaddrForVa(owner, base + 512 * 4096));
        } else try std.testing.expect(vm.lookupUserMappedPaddrForVa(owner, base) == null);
    }
}

test "MMIO overlay process map iterator batch rolls back earlier published entry" {
    const abi = @import("kernel_abi_root").process_abi;
    const fd_abi = @import("kernel_abi_root").vm_abi;
    const sc = @import("syscall/numbers.zig");
    for ([_]bool{ true, false }) |missing_last| {
        var f = try MmioRetirementFixture.start();
        defer f.end();
        const child = kernel.processPrincipalFromIndex(1).?;
        const process_fd = try f.state.createProcessFd(owner, .{ .principal_raw = @intFromEnum(child), .state = .active, .exit_code = 0 }, .{ .map_into = true, .close = true }, .{}, 16);
        const fd = try f.state.createAnonymousVmoFd(owner, 3 * 4096, .{ .map_read = true, .map_write = true, .share = true, .close = true }, .{}, 16);
        const ref = f.state.nativeVmoRefForFd(owner, fd).?;
        try f.state.installNativeVmoPages(ref, 0, &.{ physical, physical + 4096 }, null);
        if (!missing_last) try f.state.installNativeVmoPages(ref, 2, &.{physical + 8192}, null);
        iterator_batch_entries = .{
            .{ fd, base, 4096, fd_abi.prot_read, 0, abi.process_map_flag_shared },
            .{ fd, base + (1 << 21), 8192, fd_abi.prot_read, 4096, abi.process_map_flag_shared },
        };
        var h: @import("syscalls.zig").Hooks = undefined;
        h.free_list = f.free_list;
        h.copy_user_bytes_from_va = copyIteratorBatch;
        var frame = std.mem.zeroes(@import("interrupts.zig").TrapFrame);
        frame.rax = sc.syscall_process_map_batch;
        frame.rdi = process_fd;
        frame.rsi = 0x1000;
        frame.rdx = 2;
        const result = @import("syscall/process.zig").dispatch(h, f.state, owner, &frame).?;
        if (missing_last) {
            try std.testing.expect(result != sc.syscall_ok);
            try std.testing.expect(f.state.vmaEntryForVaConst(child, base) == null);
            try std.testing.expect(f.state.vmaEntryForVaConst(child, base + (1 << 21)) == null);
            try std.testing.expect(vm.lookupUserMappedPaddrForVa(child, base) == null);
            try std.testing.expect(vm.lookupUserMappedPaddrForVa(child, base + (1 << 21)) == null);
            try std.testing.expectEqual(spaces[1].reservations.capacity, vm.freeUserReservationSlotCount(child));
        } else {
            try std.testing.expectEqual(sc.syscall_ok, result);
            try std.testing.expectEqual(@as(?u64, physical), vm.lookupUserMappedPaddrForVa(child, base));
            try std.testing.expectEqual(@as(?u64, physical + 8192), vm.lookupUserMappedPaddrForVa(child, base + (1 << 21) + 4096));
        }
    }
}

test "MMIO overlay capsule DMA resolution owns invocation workspace through failure and success" {
    const sc = @import("syscall/numbers.zig");
    const abi = @import("kernel_abi_root").capsule_abi;
    var f = try MmioRetirementFixture.start();
    defer f.end();
    var backend = kernel.InvocationStorageTestAllocator{};
    defer backend.deinit();
    f.state.invocation_test_allocator = backend.interface();
    try std.testing.expect(!@import("iommu.zig").isActive());
    const fd = try f.state.createAnonymousVmoFd(owner, 3 * 4096, .{ .map_read = true, .map_write = true, .share = true, .close = true }, .{}, 16);
    const ref = f.state.nativeVmoRefForFd(owner, fd).?;
    try f.state.installNativeVmoPages(ref, 0, &.{ physical, physical + 4096 }, null);
    _ = try f.state.mmapFd(owner, fd, base, 3 * 4096, .{ .read = true, .write = true }, .{ .shared = true }, 0);
    const device = try f.state.createDeviceFd(owner, 0x1001, .{ .query = true, .derive_dma = true, .dma_read = true, .dma_write = true, .close = true }, .{}, 16);
    var h: @import("syscalls.zig").Hooks = undefined;
    h.free_list = f.free_list;
    var frame = std.mem.zeroes(@import("interrupts.zig").TrapFrame);
    frame.rax = sc.syscall_capsule_derive_dma_buffer;
    frame.rdi = device;
    frame.rsi = base + 7;
    frame.rdx = abi.dma_iova_kernel_choose;
    frame.r10 = 3 * 4096 - 7;
    backend.fail_at = backend.calls + 1;
    try std.testing.expectEqual(sc.syscall_err_invalid, @import("syscall/capsule.zig").dispatch(h, f.state, owner, &frame).?);
    try std.testing.expectEqual(@as(usize, 0), backend.active());
    backend.fail_at = 0;
    try f.state.setVmaProtRange(owner, base + 8192, 4096, .{ .read = true });
    backend.fail_return = true;
    try std.testing.expectEqual(sc.syscall_err_invalid, @import("syscall/capsule.zig").dispatch(h, f.state, owner, &frame).?);
    try std.testing.expect(f.state.descriptor_storage.pending != null);
    // Backing PFNs remain VMO-owned even though the temporary list is pending.
    try std.testing.expectEqual(@as(?u64, physical), f.state.nativeVmoPagePaddr(ref, 0));
    backend.fail_return = false;
    f.state.retryInvocationStorage();
    try f.state.setVmaProtRange(owner, base + 8192, 4096, .{ .read = true, .write = true });
    try f.state.installNativeVmoPages(ref, 2, &.{physical + 8192}, null);
    const dma_fd = @import("syscall/capsule.zig").dispatch(h, f.state, owner, &frame).?;
    try std.testing.expect(dma_fd >= @import("kernel_abi_root").fd_abi.first_dynamic_fd);
    const dma = f.state.dmaBufferObjectForFd(owner, @intCast(dma_fd), .{}).?;
    try std.testing.expectEqual(physical + 7, dma.iova);
    try std.testing.expectEqual(@as(usize, 0), backend.active());
    try std.testing.expectEqual(@as(?u64, physical + 8192), f.state.nativeVmoPagePaddr(ref, 2));
}

test "MMIO overlay scratch returned PMM page backs VMO and supports UC WC restoration without image neighbor retagging" {
    const platform = @import("arch/x86_64/platform.zig");
    const T = platform.ScratchReturnTest;
    const plan = @import("memory/ram_cache_vm_plan.zig");
    const paging = @import("memory/ram_cache_paging.zig");
    var f = try MmioRetirementFixture.start();
    defer f.end();
    try T.begin();
    defer T.end();
    try std.testing.expectError(error.KernelImageAlias, plan.measure(&.{T.tail}));
    try T.withdraw();
    try T.ack();
    try std.testing.expectError(error.InvalidState, T.giveBack(f.free_list));
    try std.testing.expectEqual(@as(usize, 0), f.free_list.pageCount());
    try std.testing.expectError(error.KernelImageAlias, plan.measure(&.{T.tail}));
    try f.free_list.installRegions(&.{.{ .id = 0, .start = T.pa, .end = T.pa + 3 * 4096 }});
    try T.giveBack(f.free_list);
    try std.testing.expectEqual(@as(usize, 2), f.free_list.pageCount());
    const fd = try f.state.createAnonymousVmoFdWithPages(owner, 4096, .{ .read = true, .write = true, .map_read = true, .map_write = true, .close = true }, .{}, 0, f.free_list);
    _ = try f.state.mmapFd(owner, fd, base, 4096, .{ .read = true, .write = true }, .{ .shared = true }, 0);
    const pa = f.state.nativeVmaInitialMapping(owner, base).?.paddr;
    try std.testing.expect(pa >= T.tail and pa < T.tail + 8192);
    try std.testing.expectEqual(@as(usize, 1), f.free_list.pageCount());
    try std.testing.expect(vm.mapLazyUserPaddrsWithProt(owner, base, &.{pa}, .{ .read = true }));
    spaces[0].cr3 = @intFromPtr(&spaces[0].pml4);
    const persistent_leaf = T.leaf(T.pa).*;
    const adjacent_leaf = T.leaf(T.pa + 3 * 4096).*;
    for ([_]u3{ 3, 0, 4, 0 }) |pat| {
        const requirements = try plan.measure(&.{pa});
        try std.testing.expect(requirements.leaves >= 2);
        try std.testing.expectEqual(@as(usize, 0), requirements.splits);
        const sources = try std.testing.allocator.alloc(paging.AliasSource, requirements.sources);
        defer std.testing.allocator.free(sources);
        const leaves = try std.testing.allocator.alloc(paging.Leaf, requirements.leaves);
        defer std.testing.allocator.free(leaves);
        var prepared = try plan.prepare(&.{pa}, try plan.collect(&.{pa}, sources), leaves, &.{});
        defer prepared.release();
        prepared.withdraw();
        try std.testing.expectEqual(@as(u64, 0), T.leaf(pa).* & 1);
        // Privileged all-CPU barriers are covered by the cache writer's host
        // architecture adapter; this test checks actual captured VMO/identity PTEs.
        prepared.publish(pat);
        for (prepared.leaves) |leaf| {
            const value = leaf.entry.*;
            const actual: u3 = @intCast(((value >> 3) & 3) | ((value >> 5) & 4));
            try std.testing.expectEqual(pat, actual);
            try std.testing.expect(value & 1 != 0);
        }
        try std.testing.expectEqual(persistent_leaf, T.leaf(T.pa).*);
        try std.testing.expectEqual(adjacent_leaf, T.leaf(T.pa + 3 * 4096).*);
    }
    try std.testing.expectError(error.KernelImageAlias, plan.measure(&.{T.pa}));
    try std.testing.expectError(error.KernelImageAlias, plan.measure(&.{T.pa + 3 * 4096}));
}

test "MMIO overlay G3 PDP PD and PT banks exceed old limits through fault lookup unmap" {
    init();
    defer cleanup();
    // 65 distinct PML4 slots require 65 PDPs; avoid allocating user backing by
    // mapping the fixture's physical page at sparse, real hardware VA tuples.
    for (2..67) |pml4| {
        const va = @as(u64, @intCast(pml4)) << 39;
        try std.testing.expect(vm.mapNativeVmaFaultPage(owner, va, .{ .paddr = physical, .prot = .{ .read = true, .write = true } }));
        try std.testing.expectEqual(@as(?u64, physical), vm.lookupUserMappedPaddrForVa(owner, va));
    }
    try std.testing.expectEqual(@as(usize, 65), usedSlots(&spaces[0].pdp_store));
    try std.testing.expectEqual(@as(usize, 65), usedSlots(&spaces[0].pd_store));
    for (2..67) |pml4| {
        const va = @as(u64, @intCast(pml4)) << 39;
        try std.testing.expect(vm.unmapPresentRemoteUserRegion(owner, va, 4096));
        try std.testing.expectEqual(@as(?u64, null), vm.lookupUserMappedPaddrForVa(owner, va));
    }
    try std.testing.expectEqual(@as(usize, 0), pt_live_pages);

    const count = 512 * pt_storage.slots_per_bank + 1;
    for (0..count) |i| {
        // The normal mapper and the fault mapper share ensureUserPtSlotForPd.
        // Materialize one leaf per 2 MiB to exercise 513 real descriptor banks.
        const va = base + (@as(u64, @intCast(i)) << 21);
        try std.testing.expect(vm.mapNativeVmaFaultPage(owner, va, .{ .paddr = physical, .prot = .{ .read = true, .write = true } }));
    }
    try std.testing.expect(spaces[0].ptCapacity() / pt_storage.slots_per_bank >= 513);
    try std.testing.expect(usedSlots(&spaces[0].pd_store) >= 65);
    for ([_]usize{ 0, 32767, 32768 }) |i| {
        const va = base + (@as(u64, @intCast(i)) << 21);
        try std.testing.expectEqual(@as(?u64, physical), vm.lookupUserMappedPaddrForVa(owner, va));
        // Fault into the last table, whose descriptor is in bank 513.
        try std.testing.expect(vm.mapNativeVmaFaultPage(owner, va + 4096, .{ .paddr = physical + 4096, .prot = .{ .read = true } }));
        try std.testing.expectEqual(@as(?u64, physical + 4096), vm.lookupUserMappedPaddrForVa(owner, va + 4096));
    }
    try std.testing.expect(vm.unmapPresentRemoteUserRegion(owner, base, count * (1 << 21)));
    try noTables();
    try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
}

test "MMIO overlay G3 every path and journal acquisition rolls back with original grants" {
    // First path: three directory/bank/page triples, then journal's triple.
    for (1..13) |failure| {
        for ([_]bool{ false, true }) |reject_returns| {
            init();
            defer cleanup();
            const saved = try snapshot();
            defer destroySnapshot(saved);
            pt_fail_at = failure;
            pt_release_fail = reject_returns;
            defer pt_release_fail = false;
            try std.testing.expect(!vm.mapUserMmioOverlay(owner, base, physical, 4096, .{ .read = true }));
            try std.testing.expectEqual(@as(usize, 0), usedPtSlots());
            try std.testing.expect(std.mem.allEqual(u64, &spaces[0].pml4, 0));
            try std.testing.expectEqual(@as(usize, 0), flushes); // No path published yet.
            pt_release_fail = false;
            vm.clearUserAddressSpace(owner);
            try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
            try unchanged(saved);
        }
    }
    // Fail growth while another complete mapping and its parent pages live.
    for (1..13) |failure| {
        init();
        defer cleanup();
        try std.testing.expect(vm.mapNativeVmaFaultPage(owner, base, .{ .paddr = physical, .prot = .{ .read = true } }));
        const saved = try snapshot();
        defer destroySnapshot(saved);
        const acquired = pt_alloc_calls;
        pt_fail_at = acquired + failure;
        const ok = vm.mapUserMmioOverlay(owner, @as(u64, 66) << 39, physical, 4096, .{ .read = true });
        if (!ok) try unchanged(saved);
        try std.testing.expectEqual(@as(?u64, physical), vm.lookupUserMappedPaddrForVa(owner, base));
    }
}

test "MMIO overlay G3 footprint observation" {
    init();
    defer cleanup();
    std.debug.print("G3 sizeof_AS={d} initial_grants={d} initial_table_zero_bytes={d}\n", .{ @sizeOf(vm.UserAddressSpace), pt_live_pages, @sizeOf(@TypeOf(spaces[0].pml4)) });
    try std.testing.expect(@sizeOf(vm.UserAddressSpace) < 64 * 1024);
    try vm.buildEmptyUserAddressSpace(owner);
    std.debug.print("G3 empty_AS_grants={d}\n", .{pt_live_pages});
    try std.testing.expect(vm.mapNativeVmaFaultPage(owner, base, .{ .paddr = physical, .prot = .{ .read = true } }));
    std.debug.print("G3 one_fault_grants={d}\n", .{pt_live_pages});
    vm.clearUserAddressSpace(owner);
    try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
}

// Model the interval before synchronous ACK completion. A child may become
// unreachable through any ancestor, but its owned descriptor cannot be reused.
const G3Retirement = struct {
    const Record = struct {
        store: *pt_storage.Store,
        slot: usize,
        pa: u64,
        root: *const u64,
        upper: *const u64,
        parent: *const u64,
        ack: bool = false,
        returned: bool = false,
        fn reachable(self: @This()) bool {
            return self.root.* & 1 != 0 and self.upper.* & 1 != 0 and
                self.parent.* & 1 != 0 and self.parent.* & 0x000f_ffff_ffff_f000 == self.pa;
        }
    };
    records: [3]Record,
    acknowledgements: usize = 0,
    fn capture() G3Retirement {
        const space = &spaces[0];
        const root = &space.pml4[1];
        const pdp = space.pdpPage(0);
        const pd = space.pdPage(0);
        return .{ .records = .{
            .{ .store = &space.pdp_store, .slot = 0, .pa = @intFromPtr(pdp), .root = root, .upper = root, .parent = root },
            .{ .store = &space.pd_store, .slot = 0, .pa = @intFromPtr(pd), .root = root, .upper = root, .parent = &pdp[0] },
            .{ .store = &space.pt_store, .slot = 0, .pa = @intFromPtr(space.ptPage(0)), .root = root, .upper = &pdp[0], .parent = &pd[0] },
        } };
    }
    fn acknowledge(self: *@This(), principal: kernel.PrincipalId) void {
        std.debug.assert(principal == owner);
        // These checks run BEFORE the emulated remote CPU sends its ACK.
        for (&self.records) |*record| {
            if (record.ack) continue;
            const entry = record.store.descriptor(record.slot).?;
            std.debug.assert(@intFromPtr(entry.page.?) == record.pa);
            std.debug.assert(entry.pml4 != pt_storage.no_index);
            std.debug.assert(record.store.firstFreeSlot() != record.slot);
            if (!record.reachable()) record.ack = true;
        }
        self.acknowledgements += 1;
    }
    fn returning(self: *@This(), pa: u64) void {
        for (&self.records) |*record| {
            if (record.pa != pa) continue;
            std.debug.assert(record.ack and !record.returned and !record.reachable());
            // Rejection retains ownership; only successful return is terminal.
            if (!pt_release_fail) record.returned = true;
        }
    }
};
var g3_retirement: ?*G3Retirement = null;

test "MMIO overlay G3 ACK protects PDP PD PT PFNs and slots through return rejection" {
    for ([_]bool{ false, true }) |clear| {
        init();
        defer cleanup();
        try std.testing.expect(vm.mapNativeVmaFaultPage(owner, base, .{ .paddr = physical, .prot = .{ .read = true } }));
        spaces[0].cr3 = @intFromPtr(&spaces[0].pml4);
        var probe = G3Retirement.capture();
        g3_retirement = &probe;
        defer g3_retirement = null;
        pt_release_fail = true;
        defer pt_release_fail = false;
        if (clear) vm.clearUserAddressSpace(owner) else try std.testing.expect(vm.unmapPresentRemoteUserRegion(owner, base, 4096));
        try std.testing.expectEqual(@as(usize, 1), probe.acknowledgements);
        for (probe.records) |record| {
            try std.testing.expect(record.ack and !record.returned);
            try std.testing.expect(record.store.descriptor(record.slot).?.page != null);
        }
        pt_release_fail = false;
        vm.clearUserAddressSpace(owner);
        for (probe.records) |record| try std.testing.expect(record.returned);
        try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
    }
}

var g3_capture_on_failure = false;
test "MMIO overlay G3 ACK protects published preparation rollback" {
    init();
    defer cleanup();
    // First path: nine page grants + three journal grants. The second PT
    // fails after the first path is already visible, but before leaf stores.
    var probe: G3Retirement = undefined;
    g3_retirement = &probe;
    g3_capture_on_failure = true;
    defer {
        g3_retirement = null;
        g3_capture_on_failure = false;
    }
    pt_fail_at = 13;
    try std.testing.expect(!vm.mapUserMmioOverlay(owner, base + (1 << 21) - 4096, physical, 8192, .{ .read = true }));
    try std.testing.expectEqual(@as(usize, 1), probe.acknowledgements);
    for (probe.records) |record| try std.testing.expect(record.returned and record.ack);
    try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
}

test "MMIO overlay G3 shootdown counts cover complete trees and empty parent batches" {
    for ([_]bool{ false, true }) |parents_only| {
        init();
        defer cleanup();
        for (2..67) |pml4| {
            const va = @as(u64, @intCast(pml4)) << 39;
            try std.testing.expect(vm.mapNativeVmaFaultPage(owner, va, .{ .paddr = physical, .prot = .{ .read = true } }));
        }
        if (parents_only) {
            // Model a previously detached/acknowledged PT whose empty parents
            // are still owned. This synthetic fixture has no hardware readers.
            var it = spaces[0].pd_store.iterator();
            while (it.next()) |_| {
                const entry = it.current.?;
                if (entry.pml4 != vm.UserAddressSpace.no_pd_index) @memset(entry.page.?, 0);
            }
            spaces[0].pt_store.freeDetached();
        }
        flushes = 0;
        try std.testing.expect(vm.unmapPresentRemoteUserRegion(owner, 2 << 39, 65 * (@as(usize, 1) << 39)));
        try std.testing.expectEqual(@as(usize, 1), flushes); // All 65 trees share one ACK.
        try noTables();
        try std.testing.expectEqual(@as(usize, 0), pt_live_pages);
    }
}

test "MMIO overlay G3 shootdown counts for unmap clear and reset" {
    for (0..5) |operation| {
        init();
        defer cleanup();
        try std.testing.expect(vm.mapUserMmioOverlay(owner, base, physical, 4096, .{ .read = true }));
        spaces[0].cr3 = @intFromPtr(&spaces[0].pml4);
        flushes = 0;
        switch (operation) {
            0 => try std.testing.expect(vm.unmapPresentRemoteUserRegion(owner, base, 4096)),
            1 => try std.testing.expect(vm.unmapUserMmioOverlay(owner, base, physical, 4096)),
            2 => vm.clearUserAddressSpace(owner),
            3 => try vm.buildEmptyUserAddressSpace(owner),
            4 => try vm.cloneAddressSpaceMetadataForFork(kernel.processPrincipalFromIndex(1).?, owner),
            else => unreachable,
        }
        try std.testing.expectEqual(@as(usize, 1), flushes);
        std.debug.print("G3 shootdown operation={d} count={d}\n", .{ operation, flushes });
    }
}

test "MMIO overlay reservation storage exceeds 2048 and checked u64 records split and merge" {
    init();
    defer cleanup();
    for (0..2049) |index| try std.testing.expect(vm.reserveUserMapping(owner, base + index * 8192, 1, .linear_region, false));
    try std.testing.expectEqual(@as(u64, 2049), spaces[0].reservations.live);
    const wide_base: u64 = 1 << 44;
    const count: u64 = @as(u64, std.math.maxInt(u32)) + 1;
    try std.testing.expect(vm.reserveUserMapping(owner, wide_base, count, .linear_region, true));
    try std.testing.expect(vm.releaseUserMapping(owner, wide_base + 4096, 1));
    try std.testing.expect(vm.reserveUserMapping(owner, wide_base + 4096, 1, .linear_region, true));
    var records = spaces[0].reservations.iterator();
    var found = false;
    while (records.next()) |record| if (record.value.base_va == wide_base) {
        try std.testing.expectEqual(count, record.value.page_count);
        found = true;
    };
    try std.testing.expect(found);
    try std.testing.expect(!vm.reserveUserMapping(owner, 4096, std.math.maxInt(u64), .linear_region, false));
    try std.testing.expectEqual(@as(usize, 0), flushes);
}

test "MMIO overlay reservation central unmap allocation failure preserves record and PTE" {
    init();
    defer cleanup();
    var source = SequenceFixture{ .count = 3 };
    try std.testing.expect(vm.mapTrustedUserPageSequence(owner, base, 3, &source, SequenceFixture.at, .{ .read = true }, false));
    var index: usize = 0;
    while (spaces[0].reservations.live < spaces[0].reservations.capacity) : (index += 1)
        try std.testing.expect(vm.reserveUserMapping(owner, base + (1 << 28) + index * 8192, 1, .linear_region, false));
    const live = spaces[0].reservations.live;
    const old_flushes = flushes;
    pt_fail_at = pt_alloc_calls + 1;
    try std.testing.expect(!vm.unmapPresentUserLinearRegion(owner, base + 4096, 4096));
    try std.testing.expectEqual(live, spaces[0].reservations.live);
    try std.testing.expectEqual(@as(?u64, physical + 4096), vm.lookupUserMappedPaddrForVa(owner, base + 4096));
    try std.testing.expectEqual(old_flushes, flushes);
    pt_fail_at = 0;
    try std.testing.expect(vm.unmapPresentUserLinearRegion(owner, base + 4096, 4096));
    try std.testing.expectEqual(live + 1, spaces[0].reservations.live);
}

test "MMIO overlay AS retirement closes new loans but retains recursive and waiting loans" {
    init();
    defer cleanup();
    const first = table.borrow(0).?;
    const waiting = table.borrow(0).?;
    try std.testing.expect(vm.lockAddressSpace(owner));
    try std.testing.expect(vm.lockAddressSpace(owner));
    vm.retireUserAddressSpace(owner);
    try std.testing.expect(table.borrow(0) == null);
    try std.testing.expect(!table.loansEnded(first));
    vm.unlockAddressSpace(owner);
    vm.unlockAddressSpace(owner);
    table.endBorrow(first);
    try std.testing.expect(!table.loansEnded(waiting));
    table.endBorrow(waiting);
    try std.testing.expect(table.loansEnded(first));
    table.reclaimRetiredBody(first);
    try std.testing.expect(table.retired_head == null);
    try std.testing.expect(table.borrow(0) == null);
    var empty: kernel.FreePageList = .{};
    try std.testing.expect(table.ensureSlot(0, &empty) == first);
    const republished = table.borrow(0).?;
    table.endBorrow(republished);
}

test "MMIO overlay fork reservation OOM leaves destination records and PTEs unchanged" {
    for (1..6) |failure| {
        init();
        defer cleanup();
        var source = SequenceFixture{ .count = 3 };
        try std.testing.expect(vm.mapTrustedUserPageSequence(owner, base, 3, &source, SequenceFixture.at, .{ .read = true }, false));
        const child = kernel.processPrincipalFromIndex(1).?;
        for (0..70) |index| try std.testing.expect(vm.reserveUserMapping(child, base + index * 8192, 1, .linear_region, false));
        const before = try snapshot();
        defer destroySnapshot(before);
        const old_flushes = flushes;
        pt_fail_at = pt_alloc_calls + failure;
        try std.testing.expectError(error.OutOfFreePages, vm.cloneAddressSpaceMetadataForFork(child, owner));
        try unchanged(before);
        try std.testing.expectEqual(old_flushes, flushes);
    }
}

test "MMIO overlay AS waiter loan survives withdrawal until A admission finishes" {
    init();
    defer cleanup();
    const space = &spaces[0];
    // Model another CPU holding A while the actual admission path waits.
    // Host tests cannot execute STI/CLI, but use the production atomics and
    // publication/borrow protocol; the test-only wait emits a CPU pause.
    space.lock_state.owner_cpu = 1;
    space.lock_state.depth = 1;
    @atomicStore(u8, &space.lock_state.value, 1, .release);
    const Waiter = struct {
        admitted: bool = true,
        fn run(self: *@This()) void {
            if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(1);
            self.admitted = vm.lockAddressSpace(owner);
            if (self.admitted) vm.unlockAddressSpace(owner);
        }
    };
    var waiter = Waiter{};
    const thread = try std.Thread.spawn(.{}, Waiter.run, .{&waiter});
    var joined = false;
    defer if (!joined) {
        @atomicStore(u8, &space.lock_state.value, 0, .release);
        thread.join();
    };
    while (table.loansEnded(space)) std.atomic.spinLoopHint();
    table.retire(0);
    try std.testing.expect(!table.loansEnded(space));
    @atomicStore(u8, &space.lock_state.value, 0, .release);
    thread.join();
    joined = true;
    try std.testing.expect(!waiter.admitted);
    try std.testing.expect(table.loansEnded(space));
    table.reclaimRetiredBody(space);
    var empty: kernel.FreePageList = .{};
    _ = table.ensureSlot(0, &empty).?;
}

test "MMIO overlay high window maps high PA independently and keeps failed mappings private" {
    init();
    defer cleanup();
    const mmio = @import("memory/mmio.zig");
    const nx: u64 = @as(u64, 1) << 63;
    var root: [512]u64 align(4096) = @splat(0);
    var first: [512]u64 align(4096) = @splat(0);
    root[0] = @intFromPtr(&first) | 3 | nx;
    first[0] = 0x1003; // synthetic copy-window PT, owned by the fixture
    var window = @import("arch/x86_64/platform.zig").MmioWindow{ .root = &root, .first_pd = .{ .physical = @intFromPtr(&first), .words = &first }, .allocator = ptAllocator(), .physical_limit = @as(u64, 1) << 52 };
    defer window.storage.freeDetached(); // host root detached; no CPU borrowed it
    const pa: u64 = (@as(u64, 1) << 48) + 4096;
    const va = try window.map(pa, 8192, nx | 0x1b);
    try std.testing.expectEqual(mmio.window_base + (1 << 21), va);
    // VT-d discovers the extended aperture after reading its first page.
    // Cache evidence cannot lend a larger extent before mapping succeeds.
    try std.testing.expect(window.lookup(pa, 12288, nx | 0x1b) == null);
    pt_fail_at = pt_alloc_calls + 1;
    try std.testing.expectError(error.OutOfMemory, window.map(pa, 12288, nx | 0x1b));
    try std.testing.expect(window.lookup(pa, 12288, nx | 0x1b) == null);
    try std.testing.expectEqual(@as(?u64, va), window.lookup(pa, 8192, nx | 0x1b));
    pt_fail_at = 0;
    const extended = try window.map(pa, 12288, nx | 0x1b);
    try std.testing.expectEqual(@as(?u64, extended), window.lookup(pa, 12288, nx | 0x1b));
    try std.testing.expectEqual(@as(?u64, extended + 17), window.lookup(pa + 17, 4000, nx | 0x1b));
    try std.testing.expectEqual(@as(u64, 0x1003), first[0]); // copy-window slot untouched
    try std.testing.expect(window.overlaps(pa, 4096));
    try std.testing.expect(window.deviceCacheAllowed(pa, 4096, false));
    try std.testing.expect(!window.deviceCacheAllowed(pa, 4096, true));
    try std.testing.expectError(error.CacheConflict, window.map(pa, 4096, nx | 1));
    try std.testing.expectError(error.InvalidRange, window.map(window.physical_limit - 4096, 8192, nx | 0x1b));
    pt_fail_at = pt_alloc_calls + 1;
    try std.testing.expectError(error.OutOfMemory, window.map(pa + 0x10000, 4096, nx | 1));
    try std.testing.expect(window.lookup(pa + 0x10000, 4096, nx | 1) == null);
    pt_fail_at = 0;
    _ = try window.map(pa + 0x10000, 4096, nx | 1);
    try std.testing.expect(!window.deviceCacheAllowed(pa + 0x10000, 4096, false));
    try window.validate(0x1000);
    // A user bit at any kernel parent or leaf invalidates the ownership proof.
    root[0] |= 4;
    try std.testing.expectError(error.UnknownTable, window.validate(0x1000));
    root[0] &= ~@as(u64, 4);
    first[1] |= 4;
    try std.testing.expectError(error.UnknownTable, window.validate(0x1000));
    first[1] &= ~@as(u64, 4);
    const window_pt: *[512]u64 = @ptrFromInt(first[1] & 0x000f_ffff_ffff_f000);
    window_pt[0] |= 4;
    try std.testing.expectError(error.UnknownTable, window.validate(0x1000));
    window_pt[0] &= ~@as(u64, 4);
    // A foreign PTE must not be normalized into a valid UC record.
    const valid_leaf = window_pt[0];
    window_pt[0] = (pa + 0x100000) | nx | 0x1b;
    const foreign_leaves = window_pt.*;
    try std.testing.expectError(error.UnknownTable, window.validate(0x1000));
    try std.testing.expectEqualSlices(u64, &foreign_leaves, window_pt);
    window_pt[0] = valid_leaf;
    // Last page of slot 510 is usable; the next page belongs to image slot 511.
    const end = mmio.window_base + mmio.window_bytes;
    window.next_va = end - 4096;
    const edge_pa = pa + 0x40000;
    try std.testing.expectEqual(end - 4096, try window.map(edge_pa, 4096, nx | 0x1b));
    try std.testing.expectEqual(@as(?u64, end - 1), window.lookup(edge_pa + 4095, 1, nx | 0x1b));
    try std.testing.expect(window.lookup(edge_pa + 4095, 2, nx | 0x1b) == null);
    try std.testing.expect(window.lookup(edge_pa, 4096, nx | 1) == null);
    try std.testing.expectError(error.NoVirtualSpace, window.map(edge_pa + 4096, 4096, nx | 0x1b));
    try std.testing.expectError(error.InvalidRange, window.map(std.math.maxInt(u64) - 4095, 8192, nx | 0x1b));
    try window.validate(0x1000);
    try std.testing.expectEqual(valid_leaf, window_pt[0]);
    try std.testing.expectEqual(@as(u64, 0x1003), first[0]);
    window.sealed = true;
    try std.testing.expectEqual(extended, try window.map(pa, 8192, nx | 0x1b));
    try std.testing.expectError(error.Sealed, window.map(pa + 0x20000, 4096, nx | 1));
}

test "MMIO overlay intrusive PMM must retain untranslated DMA backing until drain" {
    // A descriptor write requires withdrawal of device access. FD disappearance
    // alone cannot revoke the raw bus address already handed to a device.
    var f = try MmioRetirementFixture.start();
    defer f.end();
    defer @import("memory/dma_retention.zig").complete(0x1001);
    const bytes = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096);
    defer std.testing.allocator.free(bytes);
    @memset(bytes, 0x5a);
    const page: u64 = @intFromPtr(bytes.ptr);
    const regions = [_]@import("memory/free_page_list.zig").RegionBoundary{.{ .id = 1, .start = page, .end = page + 4096 }};
    try f.free_list.installRegions(&regions);
    try std.testing.expect(!@import("iommu.zig").isActive());
    const fd = try f.state.createAnonymousVmoFd(owner, 4096, .{ .map_read = true, .map_write = true, .share = true, .close = true }, .{}, 16);
    const ref = f.state.nativeVmoRefForFd(owner, fd).?;
    try f.state.installNativeVmoPages(ref, 0, &.{page}, null);
    _ = try f.state.mmapFd(owner, fd, base, 4096, .{ .read = true, .write = true }, .{ .shared = true }, 0);
    const device = try f.state.createDeviceFd(owner, 0x1001, .{ .query = true, .derive_dma = true, .dma_read = true, .dma_write = true, .close = true }, .{}, 16);
    const sc = @import("syscall/numbers.zig");
    var h: @import("syscalls.zig").Hooks = undefined;
    h.free_list = f.free_list;
    var frame = std.mem.zeroes(@import("interrupts.zig").TrapFrame);
    frame.rax = sc.syscall_capsule_derive_dma_buffer;
    frame.rdi = device;
    frame.rsi = base;
    frame.rdx = @import("kernel_abi_root").capsule_abi.dma_iova_kernel_choose;
    frame.r10 = 4096;
    const dma_fd = @import("syscall/capsule.zig").dispatch(h, f.state, owner, &frame).?;
    const dma = f.state.dmaBufferObjectForFd(owner, @intCast(dma_fd), .{}).?;
    try std.testing.expectEqual(page, dma.iova);
    try std.testing.expect(f.state.rangeOverlapsPinnedUserObject(owner, base, 4096));
    // Forced owner cleanup cannot assume that the driver quiesced the device.
    // This path has neither an IOMMU withdrawal nor a hardware drain ACK.
    f.state.releasePrincipalNativeMemory(owner, f.free_list);
    try std.testing.expect(!f.state.rangeOverlapsPinnedUserObject(owner, base, 4096));
    try std.testing.expect(@import("iommu.zig").physicalRangeQuarantined(page, 1));
    try std.testing.expectEqual(@as(usize, 0), f.free_list.pageCount());
}

test "MMIO overlay forced bypass DMA drains after last enabling authority and retains on timeout" {
    const retention = @import("memory/dma_retention.zig");
    const pci = @import("pci.zig");
    const Stop = struct {
        var calls: usize = 0;
        var succeeds: bool = false;
        fn run(_: u64) bool {
            calls += 1;
            return succeeds;
        }
    };
    // config_write is separately exercised: its existing BME authority must
    // not disappear from the lifetime proof merely because bus_master is false.
    for ([_]u8{ 0, 1, 2, 3, 4 }) |scenario| {
        var f = try MmioRetirementFixture.start();
        defer f.end();
        const device = pci.resourceIdFromLocation(.{ .bus = 0, .device = 3, .function = 0 });
        defer retention.complete(device);
        const bytes = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096);
        defer std.testing.allocator.free(bytes);
        const page: u64 = @intFromPtr(bytes.ptr);
        const regions = [_]@import("memory/free_page_list.zig").RegionBoundary{.{ .id = 1, .start = page, .end = page + 4096 }};
        try f.free_list.installRegions(&regions);
        const fd = try f.state.createAnonymousVmoFd(owner, 4096, .{ .map_read = true, .map_write = true, .close = true }, .{}, 16);
        const ref = f.state.nativeVmoRefForFd(owner, fd).?;
        try f.state.installNativeVmoPages(ref, 0, &.{page}, null);
        _ = try f.state.mmapFd(owner, fd, base, 4096, .{ .read = true, .write = true }, .{ .shared = true }, 0);
        _ = try f.state.createDmaMappingFd(owner, .{ .device = device, .user_va = base, .iova = page, .size = 4096 }, .{ .close = true }, .{}, 16);
        const other = kernel.processPrincipalFromIndex(1).?;
        const held = scenario >= 2;
        const other_fd = if (held) try f.state.createDeviceFd(other, device, .{
            .close = true,
            .bus_master = scenario == 2,
            .config_write = scenario == 3,
        }, .{}, 16) else 0;
        const carried = if (scenario == 4) f.state.fdEntryConst(other, other_fd).?.object else kernel.KernelObjectRef{};
        if (scenario == 4) try f.state.retainKernelObject(carried);
        Stop.calls = 0;
        Stop.succeeds = scenario != 1;
        pci.BypassDmaTest.stop = Stop.run;
        defer pci.BypassDmaTest.stop = null;
        f.state.releasePrincipalNativeMemory(owner, f.free_list);
        try std.testing.expectEqual(@as(usize, 0), f.free_list.pageCount());
        try std.testing.expect(retention.contains(page, 1));
        f.state.drainPendingDataReturns();
        if (held) {
            try std.testing.expectEqual(@as(usize, 0), Stop.calls);
            try std.testing.expectEqual(@as(usize, 0), f.free_list.pageCount());
            try f.state.closeFdWithFreeList(other, other_fd, f.free_list);
            if (scenario == 4) {
                f.state.drainPendingDataReturns();
                try std.testing.expectEqual(@as(usize, 0), Stop.calls);
                f.state.releaseKernelObject(carried);
            }
            f.state.drainPendingDataReturns();
        }
        try std.testing.expectEqual(@as(usize, 1), Stop.calls);
        if (scenario == 1) {
            try std.testing.expect(retention.contains(page, 1));
            try std.testing.expectEqual(@as(usize, 0), f.free_list.pageCount());
            f.state.drainPendingDataReturns();
            try std.testing.expectEqual(@as(usize, 1), Stop.calls);
        } else {
            try std.testing.expect(!retention.contains(page, 1));
            try std.testing.expectEqual(page, try f.free_list.popFront());
        }
    }
}

test "MMIO overlay bypass explicit DMA close keeps live driver drain contract" {
    var f = try MmioRetirementFixture.start();
    defer f.end();
    const retention = @import("memory/dma_retention.zig");
    const fd = try f.state.createDmaMappingFd(owner, .{ .device = 0x1001, .user_va = base, .iova = physical, .size = 4096 }, .{ .close = true }, .{}, 16);
    try f.state.closeFdWithFreeList(owner, fd, f.free_list);
    try std.testing.expect(!retention.contains(physical, 1));
}

const ForkBoundary = struct {
    var allocated: bool = false;
    var ready: bool = false;
    var releases: usize = 0;
    var copied: bool = false;
    var bases: bool = false;
    var child: kernel.PrincipalId = undefined;
    pub fn principalSlotReusable(p: kernel.PrincipalId) bool {
        return kernel.processIndexFromPrincipal(p).? == 1;
    }
    pub fn allocateSuspendedThread(p: kernel.PrincipalId, _: *vm.UserAddressSpaceTable, _: @import("interrupts.zig").TrapFrame, _: *kernel.FreePageList) ?@import("scheduler_connection.zig").ThreadAllocation {
        std.debug.assert(!allocated and !ready);
        child = p;
        allocated = true;
        return .{ .index = 42, .generation = 7 };
    }
    fn check(i: usize, g: u32, p: kernel.PrincipalId) void {
        std.debug.assert(allocated and i == 42 and g == 7 and p == child);
    }
    pub fn releaseThreadGeneration(i: usize, g: u32, p: kernel.PrincipalId) bool {
        check(i, g, p);
        std.debug.assert(!ready);
        allocated = false;
        releases += 1;
        return true;
    }
    pub fn copyCurrentThreadStateToGeneration(i: usize, g: u32, p: kernel.PrincipalId) bool {
        check(i, g, p);
        copied = true;
        return true;
    }
    pub fn currentFsBase() u64 {
        return 0x12345000;
    }
    pub fn currentGsBase() u64 {
        return 0x23456000;
    }
    pub fn setThreadBasesGeneration(i: usize, g: u32, p: kernel.PrincipalId, fs: u64, gs: u64) bool {
        check(i, g, p);
        std.debug.assert(copied and fs == currentFsBase() and gs == currentGsBase());
        bases = true;
        return true;
    }
    pub fn markThreadReadyGeneration(i: usize, g: u32, p: kernel.PrincipalId, value: bool) bool {
        check(i, g, p);
        std.debug.assert(copied and bases and value);
        ready = true;
        return true;
    }
    fn reset() void {
        allocated = false;
        ready = false;
        releases = 0;
        copied = false;
        bases = false;
    }
};
const ForkPoolFailure = struct {
    upstream: kernel.ReferenceIndex.Allocator,
    calls: usize = 0,
    fail_at: usize = 0,
    fn interface(self: *@This()) kernel.ReferenceIndex.Allocator {
        return .{ .context = self, .acquire = acquire, .release = release };
    }
    fn acquire(context: *anyopaque, count: usize) ?kernel.ReferenceIndex.MappedExtent {
        const self: *@This() = @ptrCast(@alignCast(context));
        self.calls += 1;
        if (self.calls == self.fail_at) return null;
        return self.upstream.acquire(self.upstream.context, count);
    }
    fn release(context: *anyopaque, grant: kernel.ReferenceIndex.PhysicalExtent) bool {
        const self: *@This() = @ptrCast(@alignCast(context));
        return self.upstream.release(self.upstream.context, grant);
    }
};
var fork_memory: [1024 * 4096]u8 align(4096) = undefined;
test "MMIO overlay clone preparation OOM status rollback and ready publication" {
    const Case = struct {
        fn run(pool_failure: usize, pt_failure: usize, invalid_mapping: bool) ![2]usize {
            const sc = @import("syscall/numbers.zig");
            const abi = @import("kernel_abi_root").process_abi;
            var f = try MmioRetirementFixture.start();
            defer f.end();
            const host_grants = [_]kernel.PhysicalAccessTest.Mapping{.{ .physical = @intFromPtr(&fork_memory), .bytes = &fork_memory }};
            kernel.PhysicalAccessTest.host_mappings = &host_grants;
            defer kernel.PhysicalAccessTest.host_mappings = &.{};
            try f.free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(&fork_memory), .end = @intFromPtr(&fork_memory) + fork_memory.len }});
            try f.free_list.seedContiguousRange(@intFromPtr(&fork_memory), fork_memory.len / 4096);
            // Child descriptor storage is prepared, but inactive; this exercises the
            // real admission/rollback without growing the unrelated process directory.
            const child = kernel.processPrincipalFromIndex(1).?;
            _ = f.state.removeProcessDescriptor(child);
            _ = try f.state.poolAllocator();
            const vmo = try f.state.createAnonymousVmoFdWithPages(owner, 4096, .{ .read = true, .write = true, .map_read = true, .map_write = true, .close = true }, .{}, 16, f.free_list);
            _ = try f.state.mmapFd(owner, vmo, base, 4096, .{ .read = true, .write = true }, .{ .private = true }, 0);
            const pa = f.state.nativeVmaInitialMapping(owner, base).?.paddr;
            try std.testing.expect(vm.mapLazyUserPaddrsWithProt(owner, base, &.{pa}, .{ .read = true, .write = true }));
            try std.testing.expect(vm.reserveUserMapping(owner, base + 8192, 1, .linear_region, false));
            const before = try snapshot();
            defer destroySnapshot(before);
            const old_vma = f.state.getVmaTable(owner).?.entries.get(0).?.*;
            const old_fd = f.state.getFdTable(owner).?.at(vmo).*;
            var failure = ForkPoolFailure{ .upstream = try f.state.poolAllocator(), .fail_at = pool_failure };
            f.state.pool_allocator = failure.interface();
            f.state.getVmaTable(child).?.allocator = failure.interface();
            // Reserve-node allocation must not silently reuse a free node when testing
            // its OOM path. Drafts remain owned here and are cancelled after rollback.
            var drafts: [128]?*kernel.ReferenceIndex.Node = .{null} ** 128;
            var n: usize = 0;
            while (f.state.reference_nodes.nodes.hasPreparedFree()) : (n += 1) drafts[n] = try f.state.reference_nodes.reserve(failure.upstream);
            const refs_with_drafts = f.state.reference_nodes.nodes.live;
            pt_fail_at = if (pt_failure == 0) 0 else pt_alloc_calls + pt_failure;
            const pt_start = pt_alloc_calls;
            const old_pt_pages = pt_live_pages;
            const old_free_pages = f.free_list.len;
            const old_vmo_refs = f.state.nativeVmoSlot(old_vma.vmo).?.ref_count;
            if (invalid_mapping) fail_translation = translations + 1;
            ForkBoundary.reset();
            var h: @import("syscalls.zig").Hooks = undefined;
            h.free_list = f.free_list;
            h.user_spaces = &table;
            var frame = std.mem.zeroes(@import("interrupts.zig").TrapFrame);
            frame.rdi = kernel.fdRightsToBits(.{ .close = true, .inspect = true });
            frame.rsi = abi.process_clone_flag_current_thread;
            const status = @import("syscall/process.zig").ForkPreparationTest.run(ForkBoundary, h, f.state, owner, &frame);
            const calls = [2]usize{ failure.calls, pt_alloc_calls - pt_start };
            if (pool_failure != 0 or pt_failure != 0 or invalid_mapping) {
                try std.testing.expectEqual(@as(u64, if (invalid_mapping) sc.syscall_err_map else sc.syscall_err_alloc), status);
                try std.testing.expect(!ForkBoundary.ready and !ForkBoundary.allocated);
                try std.testing.expectEqual(@as(usize, if (ForkBoundary.copied) 1 else 0), ForkBoundary.releases);
                try std.testing.expect(!f.state.hasActivePrincipal(child));
                try std.testing.expect(std.meta.eql(old_vma, f.state.getVmaTable(owner).?.entries.get(0).?.*));
                const current_fd = f.state.getFdTable(owner).?.at(vmo);
                try std.testing.expectEqual(old_fd.seq, current_fd.seq);
                try std.testing.expectEqualDeep(old_fd.object, current_fd.object);
                try std.testing.expectEqualDeep(old_fd.rights, current_fd.rights);
                try std.testing.expectEqualDeep(old_fd.flags, current_fd.flags);
                try std.testing.expectEqual(old_fd.authority.reference, current_fd.authority.reference);
                try std.testing.expectEqual(old_fd.cursor.offset, current_fd.cursor.offset);
                // Compare parent only: child unallocated tables are expected cleared.
                try unchanged(before);
                try std.testing.expectEqual(refs_with_drafts, f.state.reference_nodes.nodes.live);
                // Assert before fixture cleanup: a second teardown must not conceal
                // resources that the real syscall rollback forgot to return.
                try std.testing.expectEqual(old_vmo_refs, f.state.nativeVmoSlot(old_vma.vmo).?.ref_count);
                try std.testing.expectEqual(old_pt_pages, pt_live_pages);
                try std.testing.expectEqual(old_free_pages, f.free_list.len);
                try std.testing.expectEqual(@as(u64, 0), spaces[1].cr3);
                try std.testing.expect(spaces[1].pt_store.chunks == null and spaces[1].pd_store.chunks == null and spaces[1].pdp_store.chunks == null);
                try std.testing.expectEqual(@as(usize, 0), spaces[1].reservations.live);
                try std.testing.expectEqual(@as(usize, 0), spaces[1].reservation_draft.live);
                try std.testing.expectEqual(@as(usize, 0), f.state.getVmaTable(child).?.active_count);
                const child_fds = f.state.getFdTable(child).?;
                for (0..child_fds.capacity) |index| try std.testing.expect(child_fds.at(index).*.isEmpty());
            } else {
                try std.testing.expect(status >= 16 and status < 0xffff0000);
                try std.testing.expect(ForkBoundary.ready and ForkBoundary.allocated and ForkBoundary.releases == 0);
                try std.testing.expect(f.state.hasActivePrincipal(child));
                try f.state.closeFdWithFreeList(owner, @intCast(status), f.free_list);
            }
            for (drafts) |d| if (d) |node| kernel.ReferenceIndex.Pool.cancel(node);
            pt_fail_at = 0;
            vm.clearUserAddressSpace(child);
            f.state.releasePrincipalNativeMemory(child, f.free_list);
            f.state.resetProcessRuntimeTables(1);
            _ = f.state.removeProcessDescriptor(child);
            vm.clearUserAddressSpace(owner);
            f.state.releasePrincipalNativeMemory(owner, f.free_list);
            f.state.deinitPools();
            return calls;
        }
    };
    const counts = try Case.run(0, 0, false);
    try std.testing.expect(counts[0] > 0 and counts[1] > 0);
    for (1..counts[0] + 1) |fail| _ = try Case.run(fail, 0, false);
    for (1..counts[1] + 1) |fail| _ = try Case.run(0, fail, false);
    _ = try Case.run(0, 0, true);
    std.debug.print("clone rollback: pool OOM positions={}, PT/reservation OOM positions={}, invalid mapping and success controls PASS\n", .{ counts[0], counts[1] });
}

test "MMIO overlay physical withdrawal batches aliases before one ACK and reuses PT safely" {
    init();
    defer cleanup();
    try std.testing.expect(vm.mapNativeVmaFaultPage(owner, base, .{ .paddr = physical, .prot = .{ .read = true } }));
    try std.testing.expect(vm.mapNativeVmaFaultPage(owner, base + 8192, .{ .paddr = physical, .prot = .{ .read = true } }));
    flushes = 0;
    try std.testing.expectEqual(@as(usize, 2), vm.unmapUserMappedPaddr(owner, physical));
    try std.testing.expectEqual(@as(usize, 1), flushes);
    try std.testing.expect(vm.lookupUserMappedPaddrForVa(owner, base) == null);
    try std.testing.expect(vm.lookupUserMappedPaddrForVa(owner, base + 8192) == null);
    try std.testing.expect(vm.unmapPresentRemoteUserRegion(owner, base, 12288));
    try noTables();
    try std.testing.expect(vm.mapNativeVmaFaultPage(owner, base, .{ .paddr = physical, .prot = .{ .read = true } }));
    // An old retire marker must not return a freshly reused, live table.
    try std.testing.expect(vm.unmapPresentRemoteUserRegion(owner, base + 4096, 4096));
    try std.testing.expectEqual(physical, vm.lookupUserMappedPaddrForVa(owner, base).?);
}

test "MMIO overlay RAM alias group flushes once per owner even unsorted" {
    init();
    defer cleanup();
    var leaves = [_]u64{0} ** 3;
    const aliases = [_]vm.RamCacheUserAlias{
        .{ .principal = owner, .va = base, .leaf = &leaves[0], .original = 0 },
        .{ .principal = kernel.processPrincipalFromIndex(1).?, .va = base, .leaf = &leaves[1], .original = 0 },
        .{ .principal = owner, .va = base + 8192, .leaf = &leaves[2], .original = 0 },
    };
    flushes = 0;
    vm.flushRamCacheUserAliases(&aliases);
    try std.testing.expectEqual(@as(usize, 2), flushes);
}

test "MMIO overlay cache transition invalidates full contexts not a dummy VA" {
    init();
    defer cleanup();
    spaces[0].cr3 = @intFromPtr(&spaces[0].pml4);
    spaces[1].cr3 = @intFromPtr(&spaces[1].pml4);
    expect_full_context = true;
    defer expect_full_context = false;
    flushes = 0;
    vm.flushAllUserContexts();
    try std.testing.expectEqual(@as(usize, 2), flushes);
}

test {
    _ = @import("cpu_set.zig");
    _ = @import("cpu_topology.zig");
    _ = @import("tlb_residency.zig");
    _ = @import("arch/x86_64/syscall_entry_code.zig");
}
