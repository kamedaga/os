//! Untranslated DMA grants join the existing IOMMU admission query. PFNs are
//! recorded before capability publication, so owner death needs no allocation.
//! KernelState serializes writers and ticket lifetimes. The leaf excludes PMM
//! queries only; allocation, PCI waits and metadata returns run outside it.
const std = @import("std");
const storage = @import("owned_storage.zig");
const Leaf = @import("slot_table.zig").Leaf;

pub const Record = struct {
    next: ?*Record = null,
    device: u64,
    first: u64,
    count: usize,
    scatter: []const u64 = &.{},
    retained: bool = false,
    attempted: bool = false,
};
var head: ?*Record = null;
var retained_count: usize = 0;
var leaf: Leaf = .{};
var metadata: storage.Owner = .{};

pub fn prepare(allocator: storage.Allocator, device: u64, first: u64, count: usize, scatter: []const u64) !*Record {
    if (device == 0 or first == 0 or first & 4095 != 0 or count == 0) return error.InvalidState;
    const bytes = try std.math.mul(u64, count, 4096);
    _ = try std.math.add(u64, first, bytes);
    if (scatter.len != 0 and scatter.len != count) return error.InvalidState;
    for (scatter) |pa| if (pa == 0 or pa & 4095 != 0) return error.InvalidState;
    var draft = storage.Transaction.init(&metadata, allocator);
    errdefer draft.rollback() catch @panic("DMA retention draft rollback");
    const record = &(try draft.reserve(Record, 1, .stable))[0];
    var saved: []const u64 = &.{};
    if (scatter.len != 0) {
        const copy = try draft.reserve(u64, scatter.len, .stable);
        @memcpy(copy, scatter);
        saved = copy;
    }
    record.* = .{ .device = device, .first = first, .count = count, .scatter = saved };
    try draft.commitAdditionalStable();
    const enabled = leaf.lock();
    record.next = head;
    head = record;
    leaf.unlock(enabled);
    return record;
}

/// Forced destruction transfers the ticket to device lifetime BEFORE dropping
/// VM pins. VMO/COW pending-return owners retain the actual pages on rejection.
pub fn force(record: *Record) void {
    const enabled = leaf.lock();
    defer leaf.unlock(enabled);
    std.debug.assert(!record.retained);
    record.retained = true;
    _ = @atomicRmw(usize, &retained_count, .Add, 1, .release);
}

pub fn firstRetained() ?*Record {
    // Writer-side traversal only, under KernelState. PMM only reads the list.
    var cursor = head;
    while (cursor) |record| : (cursor = record.next) if (record.retained) return record;
    return null;
}

pub fn nextRetained(after: *const Record) ?*Record {
    var cursor = after.next;
    while (cursor) |record| : (cursor = record.next) if (record.retained) return record;
    return null;
}

pub fn contains(first: u64, count: usize) bool {
    if (!hasRetained()) return false;
    if (count == 0) return false;
    const bytes = std.math.mul(u64, count, 4096) catch return true;
    const end = std.math.add(u64, first, bytes) catch return true;
    const enabled = leaf.lock();
    defer leaf.unlock(enabled);
    var cursor = head;
    while (cursor) |record| : (cursor = record.next) {
        if (!record.retained) continue;
        if (record.scatter.len == 0) {
            if (first < record.first + @as(u64, record.count) * 4096 and record.first < end) return true;
        } else for (record.scatter) |pa| {
            if (first < pa + 4096 and pa < end) return true;
        }
    }
    return false;
}

fn remove(record: *Record) void {
    const enabled = leaf.lock();
    var link = &head;
    while (link.*) |current| {
        if (current == record) break;
        link = &current.next;
    }
    std.debug.assert(link.* == record);
    link.* = record.next;
    if (record.retained) _ = @atomicRmw(usize, &retained_count, .Sub, 1, .release);
    leaf.unlock(enabled);
    // No reader can retain a Record pointer after releasing the leaf. Save
    // every allocation address before reclaim can overwrite intrusive headers.
    if (record.scatter.len != 0)
        metadata.retireAllocation(u64, @constCast(record.scatter.ptr)) catch unreachable;
    metadata.retireAllocation(Record, @as([*]Record, @ptrCast(record))) catch unreachable;
    metadata.reclaimRetiredAfterBorrowExclusion() catch unreachable;
    metadata.retryUnpublished() catch unreachable;
}

/// Explicit close relies on the live, trusted bypass driver's drain contract.
/// A forced ticket no longer belongs to an FD and cannot be closed this way.
pub fn close(record: *Record) void {
    std.debug.assert(!record.retained);
    remove(record);
}

/// Only after BME readback and successful pending-transaction completion, with
/// all BME-enabling authorities excluded. Actual data return uses existing VMO
/// pending-return ownership; the ledger never fabricates a second PMM grant.
pub fn complete(device: u64) void {
    var cursor = head;
    while (cursor) |record| {
        cursor = record.next;
        if (record.device == device and record.retained) remove(record);
    }
}

pub fn hasRetained() bool {
    return @atomicLoad(usize, &retained_count, .acquire) != 0;
}
pub fn attempted(device: u64) void {
    var cursor = head;
    while (cursor) |record| : (cursor = record.next) {
        if (record.device == device and record.retained) record.attempted = true;
    }
}

test "MMIO overlay DMA retention captures scatter PFNs and excludes holes without allocating at death" {
    var allocator = storage.TestAllocator{};
    defer allocator.deinit();
    const pages = [_]u64{ 0x100000, 0x103000 };
    const record = try prepare(allocator.interface(), 0xfeed, pages[0], 2, &pages);
    defer complete(0xfeed);
    const allocated = allocator.active();
    try std.testing.expect(!contains(pages[0], 1));
    force(record);
    try std.testing.expectEqual(allocated, allocator.active());
    try std.testing.expect(contains(pages[0], 1));
    try std.testing.expect(contains(pages[1], 1));
    try std.testing.expect(!contains(0x101000, 2));
    complete(0xfeed);
    try std.testing.expect(!contains(pages[0], 1));
    try std.testing.expectEqual(@as(usize, 0), allocator.active());
}

pub const TestHold = if (@import("builtin").is_test) struct {
    device: u64,
    ended: bool = false,
    pub fn init(first: u64, count: usize) !@This() {
        const record = try prepare(@import("slot_table.zig").hostTestAllocator(), first, first, count, &.{});
        force(record);
        return .{ .device = first };
    }
    pub fn end(self: *@This()) void {
        if (!self.ended) complete(self.device);
        self.ended = true;
    }
} else struct {};

// Independent host KernelState fixtures have no surviving device or owner.
// Production has one global state and must use the device drain proof instead.
pub fn resetForTest() void {
    if (!@import("builtin").is_test) @compileError("host teardown only");
    while (head) |record| remove(record);
}
