//! Bootfs pages survive the Limine -> kernel CR3 transition. Records live in
//! scratch until EVERY grant is returned; no record is itself page payload.
//! Serial, pre-AP boot provides exclusion. Zig still permits copying values:
//! identity checks are runtime guards, not an affine type/lifetime proof.
const std = @import("std");
const types = @import("../state/types.zig");
const layout = @import("../arch/x86_64/physical_layout.zig");
pub const hhdm = @import("../arch/x86_64/boot_hhdm.zig");
const storage = @import("../memory/owned_storage.zig");

/// Size the ownership ledger from the input, independent of an image-size
/// policy. The arena/PMM report actual exhaustion; arithmetic must not wrap.
pub fn stagingLayout(bytes: usize) error{ InvalidSize, Overflow }!struct { pages: usize, metadata_bytes: usize } {
    if (bytes == 0) return error.InvalidSize;
    const pages = bytes / 4096 + @intFromBool(bytes % 4096 != 0);
    return .{ .pages = pages, .metadata_bytes = try std.math.mul(usize, pages, @sizeOf(StagedPage)) };
}

pub const StagedPage = struct {
    bound_address: ?*StagedPage = null,
    issuer: ?*types.FreePageList = null,
    grant: ?storage.PhysicalExtent = null,
    borrower: ?*Loan = null,

    /// Initialize fresh storage ONCE. Failed acquisition also binds the
    /// record: resetting/reacquiring it would conceal its previous lifetime.
    pub fn acquire(self: *StagedPage, list: *types.FreePageList) types.KernelError!void {
        if (self.bound_address != null) return error.InvalidState;
        self.bound_address = self;
        self.issuer = list;
        // Bootfs is not DMA32 memory. Prefer high RAM but remain reachable by
        // the RAM direct map. Deliberately exclude page zero: a null physical
        // view is rejected by the architecture access boundary.
        self.grant = list.popContiguousOwnedInRange(1, 1 << 32, layout.allocation_limit) catch |err| switch (err) {
            error.OutOfFreePages => try list.popContiguousOwnedInRange(1, 4096, layout.allocation_limit),
            else => return err,
        };
        const grant = self.grant.?;
        // Retain even a rejected backend grant for explicit return/fatal
        // accounting; never manufacture a pointer from an invalid extent.
        if (grant.pages != 1 or grant.physical.toInt() == 0 or
            grant.physical.toInt() % 4096 != 0 or
            !grant.physical.rangeFits(4096, layout.allocation_limit)) return error.InvalidState;
    }

    /// Serial boot: keep rejected grants owned until their return succeeds.
    /// Search cursors skip absent subtrees, not a fixed number of attempts.
    pub fn acquireMapped(self: *StagedPage, list: *types.FreePageList, search: *Search, view: hhdm.View) types.KernelError!void {
        if (self.bound_address != null) return error.InvalidState;
        self.bound_address = self;
        self.issuer = list;
        while (true) {
            const lower = if (search.high) search.next_high else search.next_low;
            const upper: u64 = if (search.high) layout.allocation_limit else 1 << 32;
            if (lower >= upper) {
                if (!search.high) return error.OutOfFreePages;
                search.high = false;
                continue;
            }
            self.grant = list.popContiguousOwnedInRange(1, lower, upper) catch |err| switch (err) {
                error.OutOfFreePages => {
                    if (!search.high) return err;
                    search.high = false;
                    continue;
                },
                else => return err,
            };
            const pa = self.grant.?.physical.toInt();
            const result = view.inspect(pa);
            if (result.mapped) return;
            // A rejected PMM return is fatal to boot, with the original grant
            // retained in this scratch record; never discard it to keep trying.
            try self.release();
            const next = @max(pa + 4096, result.next);
            if (search.high) search.next_high = next else search.next_low = next;
            search.rejected += 1;
        }
    }

    fn check(self: *StagedPage) types.KernelError!void {
        // Reject copies before following grant/borrow/issuer pointers; the
        // original may already have returned its RAM to the allocator.
        if (self.bound_address != self or self.grant == null) return error.InvalidState;
    }

    pub fn borrowInto(self: *StagedPage, loan: *Loan) types.KernelError!void {
        try self.check();
        if (self.borrower != null or loan.bound_address != null) return error.InvalidState;
        loan.* = .{ .bound_address = loan, .owner = self, .active = true };
        self.borrower = loan;
    }

    /// Failure leaves the WHOLE grant owned for a finite retry. The caller
    /// cannot substitute another PMM list; physical bounds determine the region.
    pub fn release(self: *StagedPage) types.KernelError!void {
        try self.check();
        if (self.borrower != null) return error.InvalidState;
        const grant = self.grant.?;
        self.issuer.?.appendContiguousRange(grant.physical.toInt(), grant.pages) catch |err| switch (err) {
            error.PageQuarantined => return err,
            else => @panic("invalid PMM return"),
        };
        self.grant = null;
    }
};

pub const Search = struct {
    next_high: u64 = 1 << 32,
    next_low: u64 = 4096,
    high: bool = true,
    rejected: usize = 0,
};

pub const Loan = struct {
    bound_address: ?*Loan = null,
    owner: ?*StagedPage = null,
    active: bool = false,

    fn check(self: *Loan) types.KernelError!*StagedPage {
        // Validate our identity before touching a possibly stale owner.
        if (self.bound_address != self or !self.active) return error.InvalidState;
        const owner = self.owner orelse return error.InvalidState;
        try owner.check();
        if (owner.borrower != self) return error.InvalidState;
        return owner;
    }

    pub fn physical(self: *Loan) types.KernelError!layout.PhysicalAddress {
        const grant = (try self.check()).grant.?;
        if (grant.pages != 1 or grant.physical.toInt() == 0 or
            grant.physical.toInt() % 4096 != 0 or
            !grant.physical.rangeFits(4096, layout.allocation_limit)) return error.InvalidState;
        return grant.physical;
    }

    pub fn identityBytes(self: *Loan) types.KernelError![]const u8 {
        return layout.identitySlice(u8, try self.physical(), 4096) orelse error.InvalidState;
    }

    pub fn hhdmBytes(self: *Loan, base: layout.KernelVirtualAddress, view: hhdm.View) types.KernelError![]u8 {
        // Only persistBootResourcesOrHalt, before the CR3 switch, may use
        // Limine's HHDM. An active loan does not extend that mapping's life.
        const address = try self.physical();
        if (base.toInt() != view.base or !view.contains(address.toInt(), 4096)) return error.InvalidState;
        return layout.hhdmSlice(u8, base, address, 4096) orelse error.InvalidState;
    }

    pub fn end(self: *Loan) types.KernelError!void {
        const owner = try self.check();
        owner.borrower = null;
        self.active = false;
        self.owner = null;
    }
};

fn testList() !*types.FreePageList {
    // Keep fixture ownership explicit across loan/copy lifetime checks.
    const list = try std.testing.allocator.create(types.FreePageList);
    list.* = .{};
    return list;
}

test "staged page prefers high RAM and returns to its original region and issuer" {
    var backing = try @import("../memory/physical_access_test.zig").BackingSet.init(&.{
        .{ .first = 4096, .pages = 8 }, .{ .first = 1 << 32, .pages = 4 }, .{ .first = @as(u64, 5) << 30, .pages = 1 },
    });
    defer backing.deinit();
    const list = try testList();
    defer std.testing.allocator.destroy(list);
    try list.installRegions(&.{ .{ .id = 17, .start = 4096, .end = 12288 }, .{ .id = 91, .start = 1 << 32, .end = (1 << 32) + 8192 } });
    try list.seedContiguousRange(4096, 2);
    try list.seedContiguousRange(1 << 32, 2);
    var page: StagedPage = .{};
    try page.acquire(list);
    try std.testing.expectEqual(@as(u64, 91), page.grant.?.region_id);
    try std.testing.expectEqual(@as(usize, 3), list.len);
    var loan: Loan = .{};
    try page.borrowInto(&loan);
    try std.testing.expectEqual(@as(u64, 1 << 32), (try loan.physical()).toInt());
    try loan.end();
    try page.release();
    try std.testing.expectEqual(@as(usize, 4), list.len);
    try std.testing.expectEqual(@as(usize, 2), list.range_len);
    try std.testing.expectError(error.InvalidState, page.acquire(list));
    try std.testing.expectError(error.InvalidState, page.release());
}

test "staged page low fallback retains nonzero region" {
    var backing = try @import("../memory/physical_access_test.zig").BackingSet.init(&.{
        .{ .first = 4096, .pages = 8 }, .{ .first = 1 << 32, .pages = 4 }, .{ .first = @as(u64, 5) << 30, .pages = 1 },
    });
    defer backing.deinit();
    const list = try testList();
    defer std.testing.allocator.destroy(list);
    try list.installRegions(&.{.{ .id = 45, .start = 8192, .end = (8192) + (1) * 4096 }});
    try list.seedContiguousRange(8192, 1);
    var page: StagedPage = .{};
    try page.acquire(list);
    try std.testing.expectEqual(@as(u64, 45), page.grant.?.region_id);
    page.grant.?.region_id = 999; // PMM derives the return region, even for stale tags.
    try page.release();
    try std.testing.expectEqual(@as(u64, 45), list.rangeAt(0).region_id);
}

test "staged page rejects copies, nested borrow, borrowed return and stale loan" {
    var backing = try @import("../memory/physical_access_test.zig").BackingSet.init(&.{
        .{ .first = 4096, .pages = 8 }, .{ .first = 1 << 32, .pages = 4 }, .{ .first = @as(u64, 5) << 30, .pages = 1 },
    });
    defer backing.deinit();
    const list = try testList();
    defer std.testing.allocator.destroy(list);
    try list.installRegions(&.{.{ .id = 3, .start = 4096, .end = (4096) + (1) * 4096 }});
    try list.seedContiguousRange(4096, 1);
    var page: StagedPage = .{};
    try page.acquire(list);
    var copy = page;
    try std.testing.expectError(error.InvalidState, copy.release());
    var loan: Loan = .{};
    try std.testing.expectError(error.InvalidState, copy.borrowInto(&loan));
    try page.borrowInto(&loan);
    try std.testing.expectError(error.InvalidState, page.release());
    var second: Loan = .{};
    try std.testing.expectError(error.InvalidState, page.borrowInto(&second));
    var copied_loan = loan;
    try std.testing.expectError(error.InvalidState, copied_loan.physical());
    try std.testing.expectError(error.InvalidState, copied_loan.end());
    try std.testing.expectEqual(@as(u64, 4096), @intFromPtr((try loan.identityBytes()).ptr));
    try std.testing.expectEqual(@as(u64, 0xffff_8000_0000_1000), @intFromPtr((try loan.hhdmBytes(.fromInt(0xffff_8000_0000_0000), hhdm.Fake.reset())).ptr));
    try std.testing.expectError(error.InvalidState, loan.hhdmBytes(.fromInt(std.math.maxInt(u64)), hhdm.Fake.reset()));
    try loan.end();
    try std.testing.expectError(error.InvalidState, loan.end());
    try std.testing.expectError(error.InvalidState, loan.physical());
    try page.release();
    try std.testing.expectError(error.InvalidState, copied_loan.physical());
    try std.testing.expectError(error.InvalidState, copy.release());
}

test "staged page failed PMM return retains grant and partial retry never double returns" {
    var backing = try @import("../memory/physical_access_test.zig").BackingSet.init(&.{
        .{ .first = 4096, .pages = 8 }, .{ .first = 1 << 32, .pages = 4 }, .{ .first = @as(u64, 5) << 30, .pages = 1 },
    });
    defer backing.deinit();
    const list = try testList();
    defer std.testing.allocator.destroy(list);
    try list.installRegions(&.{.{ .id = 6, .start = 4096, .end = 16384 }});
    try list.seedContiguousRange(4096, 1);
    var pages = [_]StagedPage{ .{}, .{} };
    try pages[0].acquire(list);
    try list.seedContiguousRange(12288, 1);
    try pages[1].acquire(list);
    try pages[0].release();
    var hold = try @import("../memory/dma_retention.zig").TestHold.init(12288, 1);
    defer hold.end();
    const before = list.len;
    try std.testing.expectError(error.PageQuarantined, pages[1].release());
    try std.testing.expectEqual(before, list.len);
    try std.testing.expect(pages[1].grant != null);
    try std.testing.expectError(error.InvalidState, pages[0].release());
    hold.end();
    try pages[1].release();
    try std.testing.expectEqual(before + 1, list.len);
    try std.testing.expectError(error.InvalidState, pages[1].release());
}

test "staged page cannot seed physical zero as intrusive RAM" {
    var backing = try @import("../memory/physical_access_test.zig").BackingSet.init(&.{
        .{ .first = 4096, .pages = 8 }, .{ .first = 1 << 32, .pages = 4 }, .{ .first = @as(u64, 5) << 30, .pages = 1 },
    });
    defer backing.deinit();
    const list = try testList();
    defer std.testing.allocator.destroy(list);
    try list.installRegions(&.{.{ .id = 4, .start = 0, .end = 8192 }});
    try std.testing.expectError(error.InvalidState, list.seedContiguousRange(0, 1));
    try std.testing.expectEqual(@as(usize, 0), list.len);
    var page: StagedPage = .{};
    try std.testing.expectError(error.OutOfFreePages, page.acquire(list));
    try list.seedContiguousRange(4096, 1);
    var usable: StagedPage = .{};
    try usable.acquire(list);
    try std.testing.expectEqual(@as(u64, 4096), usable.grant.?.physical.toInt());
    try usable.release();
}

test "staged page skips unmapped high subtree, then falls back low, preserving PMM pages" {
    var backing = try @import("../memory/physical_access_test.zig").BackingSet.init(&.{
        .{ .first = 4096, .pages = 8 }, .{ .first = 1 << 32, .pages = 4 }, .{ .first = @as(u64, 5) << 30, .pages = 1 },
    });
    defer backing.deinit();
    const list = try testList();
    defer std.testing.allocator.destroy(list);
    try list.installRegions(&.{ .{ .id = 1, .start = 4096, .end = 8192 }, .{ .id = 2, .start = 1 << 32, .end = (@as(u64, 5) << 30) + 4096 } });
    try list.seedContiguousRange(4096, 1);
    try list.seedContiguousRange(1 << 32, 1);
    try list.seedContiguousRange(@as(u64, 5) << 30, 1);
    const view = hhdm.Fake.reset();
    hhdm.Fake.pdp[4] = 0;
    var search: Search = .{};
    var first: StagedPage = .{};
    try first.acquireMapped(list, &search, view);
    try std.testing.expectEqual(@as(u64, 5) << 30, first.grant.?.physical.toInt());
    var second: StagedPage = .{};
    try second.acquireMapped(list, &search, view);
    try std.testing.expectEqual(@as(u64, 4096), second.grant.?.physical.toInt());
    try std.testing.expectEqual(@as(usize, 1), list.len);
    try std.testing.expectEqual(@as(usize, 1), search.rejected);
    try first.release();
    try second.release();
    try std.testing.expectEqual(@as(usize, 3), list.len);
}

test "staged page fails closed when an intermediate table or every candidate is missing" {
    var backing = try @import("../memory/physical_access_test.zig").BackingSet.init(&.{
        .{ .first = 4096, .pages = 8 }, .{ .first = 1 << 32, .pages = 4 }, .{ .first = @as(u64, 5) << 30, .pages = 1 },
    });
    defer backing.deinit();
    const list = try testList();
    defer std.testing.allocator.destroy(list);
    try list.installRegions(&.{ .{ .id = 1, .start = 4096, .end = 8192 }, .{ .id = 2, .start = 1 << 32, .end = (1 << 32) + 4096 } });
    try list.seedContiguousRange(4096, 1);
    try list.seedContiguousRange(1 << 32, 1);
    for ([_]bool{ false, true }) |missing_table| {
        const view = hhdm.Fake.reset();
        hhdm.Fake.table_missing = missing_table;
        if (!missing_table) @memset(&hhdm.Fake.pdp, 0);
        var search: Search = .{};
        var page: StagedPage = .{};
        try std.testing.expectError(error.OutOfFreePages, page.acquireMapped(list, &search, view));
        try std.testing.expect(page.grant == null);
        try std.testing.expectEqual(@as(usize, 2), list.len);
    }
}

test "staged page mapping rejection retains grant when PMM return fails" {
    var backing = try @import("../memory/physical_access_test.zig").BackingSet.init(&.{
        .{ .first = 4096, .pages = 8 }, .{ .first = 1 << 32, .pages = 4 }, .{ .first = @as(u64, 5) << 30, .pages = 1 },
    });
    defer backing.deinit();
    const list = try testList();
    defer std.testing.allocator.destroy(list);
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 8192 }});
    try list.seedContiguousRange(4096, 1);
    const Reject = struct {
        var hold: ?@import("../memory/dma_retention.zig").TestHold = null;
        fn read(_: u64) ?u64 {
            if (hold == null) hold = @import("../memory/dma_retention.zig").TestHold.init(4096, 1) catch unreachable;
            return null;
        }
    };
    Reject.hold = null;
    defer if (Reject.hold) |*hold| hold.end();
    var view = hhdm.Fake.reset();
    view.read_word = Reject.read;
    var search: Search = .{};
    var page: StagedPage = .{};
    try std.testing.expectError(error.PageQuarantined, page.acquireMapped(list, &search, view));
    try std.testing.expectEqual(@as(u64, 4096), page.grant.?.physical.toInt());
    try std.testing.expectEqual(@as(usize, 0), search.rejected);
    Reject.hold.?.end();
    try page.release();
    try std.testing.expect(page.grant == null);
}

test "staged page bootfs staging sizes inputs beyond 192 MiB without a policy ceiling" {
    const bytes = 193 * 1024 * 1024 + 1;
    const sized = try stagingLayout(bytes);
    try std.testing.expectEqual(@as(usize, 49409), sized.pages);
    try std.testing.expectEqual(sized.pages * @sizeOf(StagedPage), sized.metadata_bytes);
    const scratch = @import("boot_scratch.zig");
    const buffer = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), std.mem.alignForward(usize, sized.metadata_bytes + 4096, 4096));
    defer std.testing.allocator.free(buffer);
    var arena = scratch.Arena{};
    try arena.install(buffer);
    const allocation = try arena.reserveTemporary(sized.metadata_bytes);
    var loan: scratch.Borrow = .{};
    try allocation.borrowInto(&loan);
    const records = std.mem.bytesAsSlice(StagedPage, try loan.bytes());
    for (records) |*record| record.* = .{};
    try std.testing.expectEqual(sized.pages, records.len);
    try loan.end();
    try allocation.release();
    try std.testing.expectEqual(@as(usize, 0), arena.used);
    try std.testing.expectError(error.InvalidSize, stagingLayout(0));
    // Even maximal usize input rounds without bytes+4095 overflow. Its
    // metadata is smaller than the input because each record owns a page.
    const largest = try stagingLayout(std.math.maxInt(usize));
    try std.testing.expectEqual(std.math.maxInt(usize) / 4096 + 1, largest.pages);
}
