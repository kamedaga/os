const std = @import("std");
const kernel = @import("../kernel.zig");
const smp = @import("../smp.zig");
const Mode = @import("ram_cache_transition.zig").Mode;
const paging = @import("ram_cache_paging.zig");

pub const Prepared = struct {
    owner: u64,
    pages: []const u64,
    mode: Mode,
    pat: u3,

    /// Both output arrays are caller-owned and must remain alive through the
    /// final reader rendezvous. Pending refers to old policy, never final.
    pub fn policies(self: Prepared, old: *const @import("ram_cache_policy.zig").Snapshot, pending_storage: []@import("ram_cache_policy.zig").Span, final_storage: []@import("ram_cache_policy.zig").Span) !PolicyPair {
        const policy = @import("ram_cache_policy.zig");
        if (old.pending.len != 0 or policy.storageOverlaps(pending_storage, final_storage) or
            policy.storageOverlaps(old.policy, pending_storage) or policy.storageOverlaps(old.policy, final_storage))
            return error.Invalid;
        try paging.validatePages(self.pages, @as(u64, 1) << 52);
        var used: usize = 0;
        for (self.pages) |pa| {
            if (used != 0 and pending_storage[used - 1].base + pending_storage[used - 1].bytes == pa) {
                pending_storage[used - 1].bytes += 4096;
            } else {
                if (used == pending_storage.len) return error.NoSpace;
                pending_storage[used] = .{ .base = pa, .bytes = 4096, .owner = self.owner, .mode = self.mode, .pat_slot = self.pat };
                used += 1;
            }
        }
        const pending = pending_storage[0..used];
        const count = try policy.prepareMany(old.policy, pending, final_storage);
        const pair = PolicyPair{
            .pending = .{ .policy = old.policy, .pending = pending },
            .final = .{ .policy = final_storage[0..count] },
        };
        try (@import("ram_cache_commit.zig").Change{
            .owner = self.owner,
            .pages = self.pages,
            .mode = self.mode,
            .pat = self.pat,
            .old = old,
            .pending = &pair.pending,
            .final = &pair.final,
        }).validate();
        return pair;
    }
};

pub const PolicyPair = struct {
    pending: @import("ram_cache_policy.zig").Snapshot,
    final: @import("ram_cache_policy.zig").Snapshot,
};

test "MMIO overlay RAM request prepares WB pending without bridging scatter holes" {
    const policy = @import("ram_cache_policy.zig");
    const old_spans = [_]policy.Span{.{ .base = 0x100000, .bytes = 0x5000, .owner = 9, .mode = .wc, .pat_slot = 4 }};
    const old: policy.Snapshot = .{ .policy = &old_spans };
    const request = Prepared{ .owner = 9, .pages = &.{ 0x100000, 0x101000, 0x104000 }, .mode = .wb, .pat = 0 };
    var pending: [3]policy.Span = undefined;
    var final: [6]policy.Span = undefined;
    const pair = try request.policies(&old, &pending, &final);
    try std.testing.expectEqual(@as(usize, 2), pair.pending.pending.len);
    try std.testing.expect(pair.pending.query(0x100000) == .blocked);
    try std.testing.expect(pair.pending.query(0x102000) == .cached);
    try std.testing.expect(pair.final.query(0x100000) == .ordinary);
    try std.testing.expect(pair.final.query(0x102000) == .cached);
    try std.testing.expectEqual(@as(usize, 1), pair.final.policy.len);
    try std.testing.expectError(error.NoSpace, request.policies(&old, pending[0..1], &final));
    try std.testing.expectError(error.Invalid, request.policies(&old, &final, &final));
}

/// Internal native preflight. A syscall must first resolve an authorized FD;
/// a borrowed page view is not an authority to retag somebody else's RAM.
/// Caller holds shared-state/all-AS locks and keeps them through commit.
/// Scratch only is modified on failure; no page tables or policy are changed.
pub fn prepare(state: *kernel.KernelState, ref: kernel.NativeVmoRef, first_page: usize, scratch: []u64, mode: Mode) !Prepared {
    return prepareWithProof(state, ref, first_page, scratch, mode, smp.ramCachePatProof);
}

fn prepareWithProof(state: anytype, ref: kernel.NativeVmoRef, first_page: usize, scratch: []u64, mode: Mode, comptime prove: anytype) !Prepared {
    const owner = try state.nativeRamCachePages(ref, first_page, scratch);
    std.mem.sort(u64, scratch, {}, std.sort.asc(u64));
    // Reject duplicate backing rather than shrinking the requested logical
    // range and accidentally validating fewer VMA/DMA aliases.
    try paging.validatePages(scratch, @as(u64, 1) << 52);
    try state.validateRamCacheMappings(ref, first_page, scratch);
    try validateDmaQuarantine(scratch);
    const pat = try prove(scratch, mode);
    return .{ .owner = owner, .pages = scratch, .mode = mode, .pat = pat };
}

pub fn validateDmaQuarantine(pages: []const u64) !void {
    try validateQuarantineWith(pages, @import("../iommu.zig").physicalRangeQuarantined);
}

pub fn requireTranslatedDma(active: bool) !void {
    // Bypass DMA close currently has no hardware-drain proof. Do not infer
    // quiescence from disappearing FD pins on that path.
    if (!active) return error.UntranslatedDmaUnsupported;
}

test "MMIO overlay RAM cache requires translated DMA teardown contract" {
    try requireTranslatedDma(true);
    try std.testing.expectError(error.UntranslatedDmaUnsupported, requireTranslatedDma(false));
}

fn validateQuarantineWith(pages: []const u64, comptime quarantined: anytype) !void {
    // Forced FD cleanup may have removed all VMA pins while a failed device
    // drain still owns the PFNs. PMM rejection alone is too late: even their
    // cache type must remain unchanged until hardware quiescence is proven.
    for (pages) |pa| if (quarantined(pa, 1)) return error.DmaQuarantined;
}

test "MMIO overlay RAM transition rejects quarantined DMA after pins disappear" {
    const Probe = struct {
        fn quarantined(pa: u64, count: usize) bool {
            std.debug.assert(count == 1);
            return pa == 0x900000;
        }
    };
    try validateQuarantineWith(&.{0x800000}, Probe.quarantined);
    try std.testing.expectError(error.DmaQuarantined, validateQuarantineWith(&.{ 0x800000, 0x900000 }, Probe.quarantined));
}

test "MMIO overlay RAM request preserves logical range while sorting scatter PFNs" {
    const Fake = struct {
        duplicate: bool = false,
        pinned: bool = false,
        validated: bool = false,
        pub fn nativeRamCachePages(self: *@This(), _: kernel.NativeVmoRef, first: usize, pages: []u64) !u64 {
            if (first != 6 or pages.len != 2) return error.Invalid;
            pages[0] = 0x900000;
            pages[1] = if (self.duplicate) pages[0] else 0x800000;
            return 17;
        }
        pub fn validateRamCacheMappings(self: *@This(), _: kernel.NativeVmoRef, first: usize, pages: []const u64) !void {
            try std.testing.expectEqual(@as(usize, 6), first);
            try std.testing.expectEqualSlices(u64, &.{ 0x800000, 0x900000 }, pages);
            if (self.pinned) return error.DmaPinned;
            self.validated = true;
        }
        fn proof(pages: []const u64, mode: Mode) !u3 {
            try std.testing.expectEqualSlices(u64, &.{ 0x800000, 0x900000 }, pages);
            try std.testing.expectEqual(Mode.wc, mode);
            return 4;
        }
    };
    var state = Fake{};
    var scratch: [2]u64 = undefined;
    const ref: kernel.NativeVmoRef = .{ .index = 1, .generation = 7 };
    const ready = try prepareWithProof(&state, ref, 6, &scratch, .wc, Fake.proof);
    try std.testing.expect(state.validated);
    try std.testing.expectEqual(@as(u64, 17), ready.owner);
    try std.testing.expectEqual(@as(u3, 4), ready.pat);
    state = .{ .duplicate = true };
    try std.testing.expectError(error.Invalid, prepareWithProof(&state, ref, 6, &scratch, .wc, Fake.proof));
    try std.testing.expect(!state.validated);
    state = .{ .pinned = true };
    try std.testing.expectError(error.DmaPinned, prepareWithProof(&state, ref, 6, &scratch, .wc, Fake.proof));
    try std.testing.expect(!state.validated);
}
