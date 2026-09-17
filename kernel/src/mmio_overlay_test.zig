const std = @import("std");
const vm = @import("memory/user_vm.zig");
const kernel = @import("kernel.zig");
const mtrr = @import("arch/x86_64/mtrr.zig");

const owner = kernel.processPrincipalFromIndex(0).?;
const base: u64 = 1 << 39;
const physical: u64 = 0x8000_0000;
var spaces: [1]vm.UserAddressSpace align(4096) = .{.{}};
var table: vm.UserAddressSpaceTable = undefined;
var flushes: usize = 0;
var translations: usize = 0;
var fail_translation: usize = 0;

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
fn flushRange(_: kernel.PrincipalId, _: u64, _: usize) void {
    flushes += 1;
}
fn init() void {
    @import("memory/address_space.zig").resetUserAddressSpaceStorage(&spaces[0]);
    spaces[0].lock_state = .{};
    table.init(&spaces);
    flushes = 0;
    translations = 0;
    fail_translation = 0;
    vm.init(.{
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

fn usedSlots(indices: []const u16) usize {
    var count: usize = 0;
    for (indices) |index| count += @intFromBool(index != vm.UserAddressSpace.no_pd_index);
    return count;
}
fn noTables() !void {
    try std.testing.expectEqual(@as(usize, 0), usedSlots(&spaces[0].pt_page_pd_index));
    try std.testing.expectEqual(@as(usize, 0), usedSlots(&spaces[0].pd_page_pdp_index));
    try std.testing.expectEqual(@as(usize, 0), usedSlots(&spaces[0].pdp_page_pml4_index));
}
fn snapshot() !*vm.UserAddressSpace {
    const copy = try std.testing.allocator.create(vm.UserAddressSpace);
    @memcpy(std.mem.asBytes(copy), std.mem.asBytes(&spaces[0]));
    return copy;
}
fn unchanged(copy: *const vm.UserAddressSpace) !void {
    try std.testing.expectEqualSlices(u8, std.mem.asBytes(copy), std.mem.asBytes(&spaces[0]));
}

test "MMIO overlay no-op protection preserves range validation and PTEs" {
    init();
    try std.testing.expect(vm.mapTrustedLowPageZeroPaddrsWithProt(owner, 0, &.{physical}, .{ .read = true, .exec = true }));
    const before = try snapshot();
    defer std.testing.allocator.destroy(before);
    try std.testing.expect(vm.validateUserLinearRegion(owner, base, 4096));
    for ([_][2]u64{
        .{ 0, 4096 },       .{ base + 1, 4096 },         .{ base, 0 },
        .{ 1 << 47, 4096 }, .{ (1 << 47) - 4096, 8192 }, .{ 0xffff_ffff_ffff_f000, 8192 },
    }) |range| {
        try std.testing.expect(!vm.validateUserLinearRegion(owner, range[0], @intCast(range[1])));
    }
    try std.testing.expect(!vm.validateUserLinearRegion(kernel.processPrincipalFromIndex(1).?, base, 4096));
    try unchanged(before);
    try std.testing.expectEqual(@as(usize, 0), flushes);
}

test "MMIO overlay trusted remap preserves identical PTEs including hardware A/D" {
    init();
    const paddrs = [_]u64{ physical, physical + 4096 };
    const prot: kernel.MapProt = .{ .read = true, .write = true };
    try std.testing.expect(vm.mapTrustedUserPaddrsWithProt(owner, base, &paddrs, prot));
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
    spaces[0].pt_pages[slot][0] |= 1 << 5;
    spaces[0].pt_pages[slot][1] |= (1 << 5) | (1 << 6);
    const before = try snapshot();
    defer std.testing.allocator.destroy(before);
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
        const paddrs = [_]u64{ physical, physical + 4096 };
        try std.testing.expect(vm.mapTrustedUserPaddrsWithProt(owner, base, &paddrs, prot));
        const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
        const expected = spaces[0].pt_pages[slot][0..2].*;
        spaces[0].pt_pages[slot][0] |= (1 << 5) | (1 << 6);
        spaces[0].pt_pages[slot][1] ^= @as(u64, 1) << bit;
        try std.testing.expect(vm.remapTrustedUserPaddrsWithProt(owner, base, &paddrs, prot));
        try std.testing.expectEqualSlices(u64, &expected, spaces[0].pt_pages[slot][0..2]);
        try std.testing.expectEqual(@as(usize, 1), flushes);
    }
    // A missing PT must take the original allocation path, not report a no-op.
    init();
    try std.testing.expect(vm.remapTrustedUserPaddrsWithProt(owner, base, &.{physical}, prot));
    try std.testing.expectEqual(@as(?u64, physical), vm.lookupUserMappedPaddrForVa(owner, base));
    try std.testing.expectEqual(@as(usize, 1), flushes);
}

test "MMIO overlay retains reservations and retires PT PD PDP on repeated last-close" {
    init();
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
        for (&spaces[0].pt_pages, 0..) |*page, slot| {
            if (spaces[0].pt_page_pd_index[slot] == vm.UserAddressSpace.no_pd_index) continue;
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
    try std.testing.expect(vm.mapUserMmioOverlayWithCache(owner, base, physical, 8192, .{ .read = true, .write = true }, .uc_minus));
    for (&spaces[0].pt_pages, 0..) |*page, slot| {
        if (spaces[0].pt_page_pd_index[slot] == vm.UserAddressSpace.no_pd_index) continue;
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
        const copy = try snapshot();
        defer std.testing.allocator.destroy(copy);
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

test "MMIO overlay ordinary MMIO UC_MINUS preserves the same PTE encoding" {
    init();
    try std.testing.expect(vm.mapUserMmioLinearRegionWithProt(owner, base, physical, 4096, .{ .read = true }, .uc_minus));
    for (&spaces[0].pt_pages, 0..) |*page, slot| {
        if (spaces[0].pt_page_pd_index[slot] == vm.UserAddressSpace.no_pd_index) continue;
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
        const before = try snapshot();
        defer std.testing.allocator.destroy(before);
        fail_translation = failure;
        try std.testing.expect(!vm.mapUserMmioOverlay(owner, base, physical, 4096, .{ .read = true }));
        try unchanged(before);
        try noTables();
    }
    init();
    // Leave one PT available, then request two PTs across a 2 MiB boundary.
    for (1..vm.UserAddressSpace.max_dynamic_pt_pages) |pd|
        try std.testing.expect(vm.ensureUserPtSlotForPd(&spaces[0], 2, 0, pd) != null);
    const before = try snapshot();
    defer std.testing.allocator.destroy(before);
    for (0..8) |_| {
        try std.testing.expect(!vm.mapUserMmioOverlay(owner, base + 0x1ff000, physical, 8192, .{ .read = true }));
        try unchanged(before);
    }
}

test "MMIO overlay rejects existing PTEs and wrong-backing close without changing either page" {
    init();
    const target = base + 0x1ff000;
    try std.testing.expect(vm.mapUserMmioOverlay(owner, target + 4096, physical + 4096, 4096, .{ .read = true, .write = true }));
    const before = try snapshot();
    defer std.testing.allocator.destroy(before);
    try std.testing.expect(!vm.mapUserMmioOverlay(owner, target, physical, 8192, .{ .read = true }));
    try unchanged(before);
    try std.testing.expect(!vm.unmapUserMmioOverlay(owner, target, physical + 4096, 8192));
    try unchanged(before);
    try std.testing.expect(vm.unmapUserMmioOverlay(owner, target, physical, 8192));
    try noTables();
}

test "MMIO overlay last-close preserves neighboring supervisor PTEs and their parents" {
    init();
    const slot = vm.ensureUserPtSlotForPd(&spaces[0], 1, 0, 0).?;
    const neighbor: u64 = physical + 4096 | 3; // Present writable supervisor page.
    spaces[0].pt_pages[slot][1] = neighbor;
    try std.testing.expect(vm.mapUserMmioOverlay(owner, base, physical, 4096, .{ .read = true }));
    try std.testing.expect(vm.unmapUserMmioOverlay(owner, base, physical, 4096));
    try std.testing.expectEqual(neighbor, spaces[0].pt_pages[slot][1]);
    try std.testing.expectEqual(@as(u64, 0), spaces[0].pt_pages[slot][0]);
    try std.testing.expectEqual(@as(usize, 1), usedSlots(&spaces[0].pt_page_pd_index));
    try std.testing.expectEqual(@as(usize, 1), usedSlots(&spaces[0].pd_page_pdp_index));
    try std.testing.expectEqual(@as(usize, 1), usedSlots(&spaces[0].pdp_page_pml4_index));
    const before = try snapshot();
    defer std.testing.allocator.destroy(before);
    try std.testing.expect(!vm.mapUserMmioOverlay(owner, base + 4096, physical, 4096, .{ .read = true }));
    try unchanged(before);
    spaces[0].pt_pages[slot][1] = 0;
    try std.testing.expect(vm.unmapUserMmioOverlay(owner, base, physical, 4096));
    try noTables();
}
