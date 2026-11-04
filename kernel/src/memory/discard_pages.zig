//! A detached anonymous page must remain owned even if PMM rejects its return.
//! Records live in separate mapped RAM: quarantined data may still receive DMA.
const std = @import("std");
const storage = @import("owned_storage.zig");
const backing = @import("vmo_backing.zig");
const cache = @import("page_cache.zig");
pub const capacity = 64;
pub const Phase = enum(u8) { prepared, detached, queued, reclaiming };
pub const Batch = struct {
    next: ?*Batch = null,
    allocator: storage.Allocator,
    metadata: storage.MappedExtent,
    data_context: *anyopaque,
    return_data: *const fn (*anyopaque, u64) bool,
    store: backing.Ref,
    page_count: u32,
    count: usize = 0,
    owned: u64 = 0,
    phase: Phase = .prepared,
    entries: [capacity]backing.BackedPage = undefined,
};
comptime {
    std.debug.assert(capacity == 64 and @sizeOf(Batch) == 1160 and @alignOf(Batch) == 8);
}
pub fn prepare(allocator: storage.Allocator, context: *anyopaque, return_data: *const fn (*anyopaque, u64) bool, store: backing.Ref, page_count: u32, pages: []const backing.BackedPage) !*Batch {
    if (pages.len == 0 or pages.len > capacity) return error.InvalidState;
    const mapped = allocator.acquire(allocator.context, 1) orelse return error.OutOfMemory;
    // Save the exact mapped grant, including a possible cache's release_mapped.
    // No PA reconstructed from the record pointer is ever used for return.
    const batch: *Batch = @ptrCast(mapped.bytes.ptr);
    batch.* = .{ .allocator = allocator, .metadata = mapped, .data_context = context, .return_data = return_data, .store = store, .page_count = page_count, .count = pages.len };
    @memcpy(batch.entries[0..pages.len], pages);
    return batch;
}

/// Caller holds K + owner AS + shared VM through every batch. No PMM callback
/// or allocation under the store leaf. Validate ALL entries before withdrawing
/// any; setting owned in the same exclusion closes the intermediate owner gap.
pub fn checkedDetach(batch: *Batch) !void {
    if (batch.phase != .prepared or batch.owned != 0 or batch.count == 0 or batch.count > capacity) return error.InvalidState;
    const store = batch.store.store orelse return error.InvalidState;
    store.lock.lock();
    defer store.lock.unlock();
    if (!store.matches(batch.store, batch.store.owner)) return error.InvalidState;
    for (batch.entries[0..batch.count], 0..) |page, i| {
        if (page.index >= batch.page_count or page.paddr == 0 or page.paddr & 4095 != 0 or
            (i != 0 and batch.entries[i - 1].index >= page.index) or
            store.getLocked(page.index) != page.paddr) return error.InvalidState;
    }
    for (batch.entries[0..batch.count]) |page| store.putLocked(page.index, 0);
    batch.owned = @as(u64, std.math.maxInt(u64)) >> @as(u6, @intCast(capacity - batch.count));
    batch.phase = .detached;
}

pub const Queue = struct {
    lock: cache.Cache = .{},
    head: ?*Batch = null,
    available: bool = false,
    pub fn push(self: *Queue, batch: *Batch) !void {
        if (batch.phase != .prepared and batch.phase != .detached and batch.phase != .reclaiming) return error.InvalidState;
        // Once queued, the batch must outlive even destruction of its VMO.
        // Reclaim has no dependency on that store, process, K or VM locks.
        batch.store = .{};
        batch.phase = .queued;
        const irq = cache.mask();
        defer cache.restore(irq);
        self.lock.lock();
        batch.next = self.head;
        self.head = batch;
        @atomicStore(bool, &self.available, true, .release);
        self.lock.unlock();
    }
    pub fn reclaim(self: *Queue) usize {
        if (!@atomicLoad(bool, &self.available, .acquire)) return 0;
        const irq = cache.mask();
        self.lock.lock();
        var cursor = self.head;
        self.head = null;
        @atomicStore(bool, &self.available, false, .release);
        self.lock.unlock();
        cache.restore(irq);
        var returned: usize = 0;
        // Detach a snapshot so callbacks/reentrant OOM cannot see these records.
        // A rejected batch is retried once per pass, never in a spinning loop.
        while (cursor) |batch| {
            const next = batch.next;
            batch.phase = .reclaiming;
            for (batch.entries[0..batch.count], 0..) |page, i| {
                const bit = @as(u64, 1) << @as(u6, @intCast(i));
                if (batch.owned & bit != 0 and batch.return_data(batch.data_context, page.paddr)) {
                    batch.owned &= ~bit;
                    returned += 1;
                }
            }
            var released = false;
            if (batch.owned == 0) {
                const allocator = batch.allocator;
                const mapped = batch.metadata;
                released = if (allocator.release_mapped) |release| release(allocator.context, mapped) else allocator.release(allocator.context, mapped.allocation);
                if (released) returned += mapped.allocation.pages;
            }
            // A true return invalidates the entire record, including next.
            if (!released) self.push(batch) catch @panic("discard queue ownership");
            cursor = next;
        }
        return returned;
    }
};
pub var global: Queue = .{};

const TestReturn = struct {
    calls: usize = 0,
    rejects: bool = false,
    ready: bool = false,
    queue: ?*Queue = null,
    reenter: bool = false,
    seen: [8]u64 = @splat(0),
    fn give(context: *anyopaque, pa: u64) bool {
        const self: *TestReturn = @ptrCast(@alignCast(context));
        std.debug.assert(self.ready); // Shootdown completion precedes returns.
        self.calls += 1;
        if (self.reenter) {
            self.reenter = false;
            std.debug.assert(self.queue.?.reclaim() == 0);
        }
        if (self.rejects) return false;
        for (&self.seen) |*seen| {
            std.debug.assert(seen.* != pa);
            if (seen.* == 0) {
                seen.* = pa;
                break;
            }
        }
        return true;
    }
};

test "discard ownership prepare rollback and mapped metadata return rejection" {
    var issuer: storage.TestAllocator = .{};
    defer issuer.deinit();
    var data: TestReturn = .{};
    var queue: Queue = .{};
    const batch = try prepare(issuer.interface(), &data, TestReturn.give, .{}, 4, &.{.{ .index = 1, .paddr = 4096 }});
    try queue.push(batch);
    try std.testing.expectError(error.InvalidState, queue.push(batch));
    issuer.fail_return = true;
    try std.testing.expectEqual(@as(usize, 0), queue.reclaim());
    try std.testing.expectEqual(@as(usize, 0), data.calls);
    try std.testing.expectEqual(@as(usize, 1), issuer.returns);
    issuer.fail_return = false;
    try std.testing.expectEqual(@as(usize, 1), queue.reclaim());
    try std.testing.expectEqual(@as(usize, 0), issuer.active());
    issuer.fail_at = issuer.calls + 1;
    try std.testing.expectError(error.OutOfMemory, prepare(issuer.interface(), &data, TestReturn.give, .{}, 4, &.{.{ .index = 1, .paddr = 4096 }}));
}

test "discard ownership checked detach retains all failures without old content or reentry double return" {
    var issuer: storage.TestAllocator = .{};
    defer issuer.deinit();
    var store: backing.Store = .{};
    const ref = store.init(17);
    defer std.debug.assert(store.clear(ref));
    try store.prepare(ref, 1, issuer.interface());
    store.putLocked(1, 4096);
    store.putLocked(2, 8192);
    var data: TestReturn = .{};
    var queue: Queue = .{};
    const batch = try prepare(issuer.interface(), &data, TestReturn.give, ref, 4, &.{ .{ .index = 1, .paddr = 4096 }, .{ .index = 2, .paddr = 8192 } });
    // An incorrect second PA must not detach even the valid first entry.
    batch.entries[1].paddr = 12288;
    try std.testing.expectError(error.InvalidState, checkedDetach(batch));
    try std.testing.expectEqual(@as(u64, 4096), store.getLocked(1));
    batch.entries[1].paddr = 8192;
    batch.store.owner ^= 1;
    try std.testing.expectError(error.InvalidState, checkedDetach(batch));
    batch.store = ref;
    batch.entries[1].index = 4;
    try std.testing.expectError(error.InvalidState, checkedDetach(batch));
    batch.entries[1].index = 2;
    try checkedDetach(batch);
    try std.testing.expectEqual(@as(u64, 3), batch.owned);
    try std.testing.expectEqual(@as(u64, 0), store.getLocked(1));
    try std.testing.expectEqual(@as(u64, 0), store.getLocked(2));
    try std.testing.expectEqual(@as(usize, 0), data.calls);
    try queue.push(batch);
    try std.testing.expect(batch.store.store == null);
    data.ready = true;
    data.rejects = true; // Both quarantine and generic errors use this path.
    data.queue = &queue;
    data.reenter = true;
    try std.testing.expectEqual(@as(usize, 0), queue.reclaim());
    try std.testing.expectEqual(@as(usize, 2), data.calls);
    try std.testing.expectEqual(@as(u64, 0), store.getLocked(1));
    try std.testing.expectEqual(@as(u64, 3), batch.owned);
    data.rejects = false;
    issuer.fail_return = true;
    try std.testing.expectEqual(@as(usize, 2), queue.reclaim());
    try std.testing.expectEqual(@as(u64, 0), batch.owned);
    issuer.fail_return = false;
    try std.testing.expectEqual(@as(usize, 1), queue.reclaim());
    try std.testing.expect(queue.head == null);
    try std.testing.expectEqual(@as(usize, 4), data.calls);
    std.debug.print("DISCARD_LAYOUT size={} align={} capacity={}\n", .{ @sizeOf(Batch), @alignOf(Batch), capacity });
}

test "discard ownership metadata returns the original mapped issuer" {
    const Callback = struct {
        fn unmapped(_: *anyopaque, _: storage.PhysicalExtent) bool { @panic("mapped issuer bypassed"); }
        fn mapped(context: *anyopaque, extent: storage.MappedExtent) bool {
            const issuer: *storage.TestAllocator = @ptrCast(@alignCast(context));
            const original = issuer.interface();
            return original.release(original.context, extent.allocation);
        }
    };
    var issuer: storage.TestAllocator = .{};
    defer issuer.deinit();
    var interface = issuer.interface();
    interface.release = Callback.unmapped;
    interface.release_mapped = Callback.mapped;
    var data: TestReturn = .{};
    var queue: Queue = .{};
    const batch = try prepare(interface, &data, TestReturn.give, .{}, 1, &.{.{ .index = 0, .paddr = 4096 }});
    try queue.push(batch);
    issuer.fail_return = true;
    try std.testing.expectEqual(@as(usize, 0), queue.reclaim());
    issuer.fail_return = false;
    try std.testing.expectEqual(@as(usize, 1), queue.reclaim());
    try std.testing.expectEqual(@as(usize, 2), issuer.returns);
    try std.testing.expectEqual(@as(usize, 0), issuer.active());
}
