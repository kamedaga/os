//! Ownership of mapped PMM extents, independent of their mapping mechanism.
//! Owners must remain address-stable and all finish/retry operations require
//! the caller's exclusion. Transactions allocate locally outside that lock.
//! Zig permits copies: identity checks reject copies AFTER ownership starts;
//! this is a runtime guard, not a language-level affine/borrow proof.
const std = @import("std");
const addresses = @import("addresses.zig");

pub const PhysicalExtent = struct {
    physical: addresses.PhysicalAddress,
    pages: usize,
    region_id: u64,
};

pub const MappedExtent = struct {
    allocation: PhysicalExtent,
    bytes: []align(4096) u8,
};

pub const Allocator = struct {
    context: *anyopaque,
    // Success transfers a unique, fully mapped extent. Failure retains all
    // backend ownership. Neither callback may reenter its owner's subsystem.
    acquire: *const fn (*anyopaque, usize) ?MappedExtent,
    // False retains the WHOLE accessible extent with the caller for retry.
    release: *const fn (*anyopaque, PhysicalExtent) bool,
    // Optional transfer of the already mapped extent after all loans ended.
    // Caches may reuse the header only after this callback succeeds. The
    // mapping comes from the original grant, never from a reconstructed PA.
    release_mapped: ?*const fn (*anyopaque, MappedExtent) bool = null,

    fn returnRecord(self: Allocator, record: *Record, extent: PhysicalExtent) bool {
        if (self.release_mapped) |release| {
            const bytes: [*]align(4096) u8 = @ptrCast(@alignCast(record));
            return release(self.context, .{ .allocation = extent, .bytes = bytes[0 .. extent.pages * 4096] });
        }
        return self.release(self.context, extent);
    }
};

pub const Error = error{ InvalidState, InvalidSize, UnsupportedAlignment, OutOfMemory };
pub const Purpose = enum { replaceable, stable };
const Life = enum { staged, published, retired, return_pending };
const Record = struct {
    next: ?*Record,
    allocator: Allocator,
    allocation: PhysicalExtent,
    purpose: Purpose,
    life: Life,
};

/// Sole ownership of withdrawn physical allocations, independent of the K
/// owner and slot metadata. PMM may drain this queue without acquiring K.
pub const RetiredQueue = struct {
    lock: @import("page_cache.zig").Cache = .{},
    head: ?*Record = null,
    nonempty: bool = false,
    pub fn push(self: *RetiredQueue, record: *Record) void {
        const mask = @import("page_cache.zig").mask();
        self.lock.lock();
        record.next = self.head;
        self.head = record;
        @atomicStore(bool, &self.nonempty, true, .release);
        self.lock.unlock();
        @import("page_cache.zig").restore(mask);
    }
    pub fn reclaim(self: *RetiredQueue) usize {
        if (!@atomicLoad(bool, &self.nonempty, .acquire)) return 0;
        const mask = @import("page_cache.zig").mask();
        self.lock.lock();
        var cursor = self.head;
        self.head = null;
        @atomicStore(bool, &self.nonempty, false, .release);
        self.lock.unlock();
        @import("page_cache.zig").restore(mask);
        var pages: usize = 0;
        // A private batch prevents a failed return from spinning forever and
        // leaves no cache/queue leaf held across allocator callbacks.
        while (cursor) |record| {
            const next = record.next;
            const allocator = record.allocator;
            const extent = record.allocation;
            if (allocator.returnRecord(record, extent)) {
                pages += extent.pages;
            } else self.push(record);
            cursor = next;
        }
        return pages;
    }
};

pub fn requiredPages(comptime T: type, count: usize) Error!usize {
    if (@alignOf(T) > 4096) return error.UnsupportedAlignment;
    if (count == 0 or @sizeOf(T) == 0) return error.InvalidSize;
    const bytes = std.math.mul(usize, @sizeOf(T), count) catch return error.InvalidSize;
    const offset = std.mem.alignForward(usize, @sizeOf(Record), @alignOf(T));
    const total = std.math.add(usize, offset, bytes) catch return error.InvalidSize;
    return (std.math.add(usize, total, 4095) catch return error.InvalidSize) / 4096;
}

pub const Owner = struct {
    // Capacity planners need the real contiguous requirement, including the
    // ownership header and alignment, rather than just sizeof(T) * count.
    pub const allocationPages = requiredPages;
    bound_address: ?*Owner = null,
    published: ?*Record = null,
    retired: ?*Record = null,
    pending: ?*Record = null,

    fn bind(self: *Owner) Error!void {
        // Check BEFORE dereferencing any header: a copied owner may refer to
        // pages already returned by the original instance.
        if (self.bound_address) |address| {
            if (address != self) return error.InvalidState;
        } else self.bound_address = self;
    }

    /// Remove a published grant after its logical references have been
    /// withdrawn. This does not return memory: callers must separately prove
    /// borrower exclusion before reclaiming retired grants. The pointer must
    /// be the start of a reserve(T, ...) result, never an interior body pointer.
    pub fn retireAllocation(self: *Owner, comptime T: type, ptr: [*]T) Error!void {
        try self.bind();
        const offset = std.mem.alignForward(usize, @sizeOf(Record), @alignOf(T));
        const address = @intFromPtr(ptr);
        var link = &self.published;
        while (link.*) |record| {
            if (@intFromPtr(record) + offset == address) {
                link.* = record.next;
                record.life = .retired;
                record.next = self.retired;
                self.retired = record;
                return;
            }
            link = &record.next;
        }
        return error.InvalidState;
    }

    /// Destruction only, after all references to every published grant have
    /// ended. Retrying a rejected return never resets the owner's identity.
    pub fn retireAll(self: *Owner) Error!void {
        try self.bind();
        while (self.published) |record| {
            self.published = record.next;
            record.life = .retired;
            record.next = self.retired;
            self.retired = record;
        }
    }

    /// A containing resource may die while PMM rejects a return. Move those
    /// already-unborrowed grants to its longer-lived owner before freeing the
    /// containing body; dropping the child Owner would otherwise lose them.
    pub fn transferUnreturned(self: *Owner, destination: *Owner) Error!void {
        try self.bind();
        try destination.bind();
        if (self == destination or self.published != null) return error.InvalidState;
        inline for (.{ &self.retired, &self.pending }) |head| {
            while (head.*) |record| {
                head.* = record.next;
                record.life = .return_pending;
                record.next = destination.pending;
                destination.pending = record;
            }
        }
    }

    /// Move ownership without moving grants or their borrowers' body addresses.
    /// Both owners remain address-bound; the caller excludes all directory
    /// loans and transfers the corresponding roots in the same writer hold.
    pub fn takePublished(self: *Owner, source: *Owner) Error!void {
        try self.bind();
        try source.bind();
        if (self == source) return error.InvalidState;
        while (source.published) |record| {
            source.published = record.next;
            record.next = self.published;
            self.published = record;
        }
        try source.transferUnreturned(self);
    }

    /// Retry only never-published storage. Retired storage still has possible
    /// borrowers; replacement alone is NOT evidence that it can be reclaimed.
    pub fn retryUnpublished(self: *Owner) Error!void {
        try self.bind();
        var cursor = self.pending;
        self.pending = null;
        // Snapshot the old list so a failed return is attempted once per pass,
        // rather than spinning forever when PMM's range metadata is full.
        while (cursor) |record| {
            const next = record.next;
            const allocator = record.allocator;
            const extent = record.allocation;
            if (!allocator.returnRecord(record, extent)) {
                record.next = self.pending;
                self.pending = record;
            }
            // Successful release invalidates record, including its next link.
            cursor = next;
        }
    }

    /// K/owner exclusion has ended every borrow before this call. Transfer
    /// all retired grants to the independent queue, including rejected returns:
    /// PMM may retry them during OOM without reentering this live owner or K.
    pub fn transferRetiredAfterBorrowExclusion(self: *Owner, queue: *RetiredQueue) Error!void {
        try self.bind();
        while (self.retired) |record| {
            self.retired = record.next;
            queue.push(record);
        }
        // Rollback grants were never published, so they have no borrowers.
        // Move them too: a rejected return must not depend on a later syscall
        // revisiting this owner, which may itself be withdrawn meanwhile.
        while (self.pending) |record| {
            self.pending = record.next;
            queue.push(record);
        }
        _ = queue.reclaim();
    }

    /// Only after EVERY retired reference has been excluded by the caller's
    /// publication protocol. The owner's writer exclusion must remain held,
    /// but reader/IRQ locks must be dropped before invoking allocator code.
    /// Audited callers: AS/VMA/FD directories and descriptor snapshots, plus
    /// stable FD entries at process teardown after K borrows end,
    /// and replaceable thread
    /// metadata drained under table + directory leaf (not stable contexts).
    pub fn reclaimRetiredAfterBorrowExclusion(self: *Owner) Error!void {
        try self.bind();
        var cursor = self.retired;
        self.retired = null;
        while (cursor) |record| {
            const next = record.next;
            const allocator = record.allocator;
            const extent = record.allocation;
            if (!allocator.returnRecord(record, extent)) {
                record.next = self.retired;
                self.retired = record;
            }
            // Success invalidates the intrusive header; never read it again.
            cursor = next;
        }
    }
};

pub const Transaction = struct {
    owner: *Owner,
    allocator: Allocator,
    head: ?*Record = null,
    bound_address: ?*Transaction = null,
    finished: bool = false,

    pub fn init(owner: *Owner, allocator: Allocator) Transaction {
        return .{ .owner = owner, .allocator = allocator };
    }

    fn check(self: *Transaction) Error!void {
        if (self.finished) return error.InvalidState;
        if (self.bound_address) |address| {
            if (address != self) return error.InvalidState;
        } else self.bound_address = self;
    }

    /// The returned slice is borrowed from this transaction until commit; on
    /// rollback every draft reference must be discarded, even if return waits.
    pub fn reserve(self: *Transaction, comptime T: type, count: usize, purpose: Purpose) Error![]T {
        try self.check();
        const pages = try requiredPages(T, count);
        // Reject unsupported alignment before acquiring PMM pages. Keeping
        // this branch comptime also prevents an impossible alignCast below.
        if (comptime @alignOf(T) > 4096) return error.UnsupportedAlignment;
        const allocator = if (comptime @import("../lock_profile.zig").enabled) @import("identity_allocator.zig").forType(self.allocator, T) else {};
        const mapped = (if (comptime @import("../lock_profile.zig").enabled) allocator.acquire(allocator.context, pages) else self.allocator.acquire(self.allocator.context, pages)) orelse return error.OutOfMemory;
        std.debug.assert(mapped.allocation.pages == pages and mapped.bytes.len == pages * 4096);
        @memset(mapped.bytes, 0);
        const record: *Record = @ptrCast(mapped.bytes.ptr);
        record.* = .{
            .next = self.head,
            .allocator = if (comptime @import("../lock_profile.zig").enabled) allocator else self.allocator,
            .allocation = mapped.allocation,
            .purpose = purpose,
            .life = .staged,
        };
        self.head = record;
        const offset = std.mem.alignForward(usize, @sizeOf(Record), @alignOf(T));
        const data: [*]T = @ptrCast(@alignCast(mapped.bytes.ptr + offset));
        return data[0..count];
    }

    /// Append stable grants without walking existing published storage. AS
    /// bodies never replace one another; scanning all bodies on each insertion
    /// would introduce quadratic work inside their IRQ-disabled publication.
    pub fn commitAdditionalStable(self: *Transaction) Error!void {
        try self.check();
        try self.owner.bind();
        var cursor = self.head;
        while (cursor) |record| : (cursor = record.next) {
            if (record.purpose != .stable or record.life != .staged) return error.InvalidState;
        }
        // Validate the whole draft before modifying either list. A rejected
        // mixed transaction is still private and can be rolled back normally.
        cursor = self.head;
        while (cursor) |record| {
            const next = record.next;
            record.life = .published;
            record.next = self.owner.published;
            self.owner.published = record;
            cursor = next;
        }
        self.head = null;
        self.finished = true;
    }

    /// Caller holds the publication lock and installs the new slices in that
    /// SAME hold. Stable records (runqueue Nodes) survive future replacements.
    /// Old replaceable records are tracked, but borrow exclusion is still
    /// required before their eventual reclamation can be implemented.
    pub fn commit(self: *Transaction) Error!void {
        try self.check();
        try self.owner.bind();
        var link = &self.owner.published;
        while (link.*) |record| {
            if (record.purpose == .stable) {
                link = &record.next;
            } else {
                link.* = record.next;
                record.life = .retired;
                record.next = self.owner.retired;
                self.owner.retired = record;
            }
        }
        var cursor = self.head;
        while (cursor) |record| {
            const next = record.next;
            record.life = .published;
            record.next = self.owner.published;
            self.owner.published = record;
            cursor = next;
        }
        self.head = null;
        self.finished = true;
    }

    /// Caller holds the owner's lock, including when reserve failed outside
    /// it. Failed PMM returns remain explicitly owned, not silently dropped.
    pub fn rollback(self: *Transaction) Error!void {
        try self.check();
        try self.owner.bind();
        var cursor = self.head;
        self.head = null;
        self.finished = true;
        while (cursor) |record| {
            const next = record.next;
            const allocator = record.allocator;
            const extent = record.allocation;
            if (!allocator.returnRecord(record, extent)) {
                record.life = .return_pending;
                record.next = self.owner.pending;
                self.owner.pending = record;
            }
            cursor = next;
        }
    }
};

pub const TestAllocator = struct {
    pub const allocationPages = requiredPages;
    last_pages: usize = 0,
    // Fixture capacity only; production bookkeeping is intrusive in extents.
    slots: [16]?MappedExtent = .{null} ** 16,
    calls: usize = 0,
    fail_at: usize = 0,
    fail_return: bool = false,
    returns: usize = 0,

    pub fn interface(self: *TestAllocator) Allocator {
        return .{ .context = self, .acquire = acquire, .release = release };
    }
    fn acquire(context: *anyopaque, pages: usize) ?MappedExtent {
        const self: *TestAllocator = @ptrCast(@alignCast(context));
        self.calls += 1;
        self.last_pages = pages;
        if (self.calls == self.fail_at) return null;
        for (&self.slots, 0..) |*slot, i| {
            if (slot.* != null) continue;
            const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), pages * 4096) catch return null;
            const mapped: MappedExtent = .{
                // Deliberately unrelated to the host pointer: returns must use
                // the original grant rather than reverse-converting the slice.
                .allocation = .{ .physical = .fromInt((i + 1) * 0x100000), .pages = pages, .region_id = 71 + i },
                .bytes = bytes,
            };
            slot.* = mapped;
            return mapped;
        }
        return null;
    }
    fn release(context: *anyopaque, extent: PhysicalExtent) bool {
        const self: *TestAllocator = @ptrCast(@alignCast(context));
        self.returns += 1;
        if (self.fail_return) return false;
        for (&self.slots) |*slot| {
            const mapped = slot.* orelse continue;
            if (mapped.allocation.physical.toInt() != extent.physical.toInt()) continue;
            std.debug.assert(mapped.allocation.pages == extent.pages and mapped.allocation.region_id == extent.region_id);
            @memset(mapped.bytes, 0xa5);
            std.testing.allocator.free(mapped.bytes);
            slot.* = null;
            return true;
        }
        @panic("duplicate or fabricated extent return");
    }
    pub fn active(self: *TestAllocator) usize {
        var count: usize = 0;
        for (self.slots) |slot| if (slot != null) {
            count += 1;
        };
        return count;
    }
    pub fn deinit(self: *TestAllocator) void {
        // Tests have no surviving borrowers. Not a production reclaim API.
        for (&self.slots) |*slot| if (slot.*) |mapped| {
            std.testing.allocator.free(mapped.bytes);
            slot.* = null;
        };
    }
};

test "owned storage excluded retirement returns original issuer once and retries rejection" {
    var first: TestAllocator = .{};
    defer first.deinit();
    var second: TestAllocator = .{};
    defer second.deinit();
    var owner: Owner = .{};
    var old = Transaction.init(&owner, first.interface());
    _ = try old.reserve(u64, 700, .replaceable);
    _ = try old.reserve(u64, 1, .stable);
    try old.commit();
    var replacement = Transaction.init(&owner, second.interface());
    _ = try replacement.reserve(u64, 1500, .replaceable);
    try replacement.commit();
    first.fail_return = true;
    try owner.reclaimRetiredAfterBorrowExclusion();
    try std.testing.expectEqual(@as(usize, 1), first.returns);
    try std.testing.expect(owner.retired != null);
    try std.testing.expectEqual(@as(usize, 0), second.returns);
    first.fail_return = false;
    try owner.reclaimRetiredAfterBorrowExclusion();
    try std.testing.expect(owner.retired == null);
    try std.testing.expectEqual(@as(usize, 1), first.active());
    try std.testing.expectEqual(@as(usize, 1), second.active());
    try owner.reclaimRetiredAfterBorrowExclusion();
    try std.testing.expectEqual(@as(usize, 2), first.returns);
    var copied = owner;
    try std.testing.expectError(error.InvalidState, copied.reclaimRetiredAfterBorrowExclusion());
}

test "owned storage stable append rejects mixed drafts without retiring published grants" {
    var backend: TestAllocator = .{};
    defer backend.deinit();
    var owner: Owner = .{};
    var original = Transaction.init(&owner, backend.interface());
    _ = try original.reserve(u64, 1, .replaceable);
    try original.commit();
    const published = owner.published;
    var mixed = Transaction.init(&owner, backend.interface());
    _ = try mixed.reserve(u64, 1, .replaceable);
    _ = try mixed.reserve(u64, 1, .stable);
    const head = mixed.head;
    try std.testing.expectError(error.InvalidState, mixed.commitAdditionalStable());
    try std.testing.expectEqual(head, mixed.head);
    try std.testing.expectEqual(published, owner.published);
    try std.testing.expect(!mixed.finished and owner.retired == null);
    try mixed.rollback();
    var added = Transaction.init(&owner, backend.interface());
    _ = try added.reserve(u64, 1, .stable);
    try added.commitAdditionalStable();
    try std.testing.expectEqual(published, owner.published.?.next);
    try std.testing.expect(owner.retired == null);
    try std.testing.expectError(error.InvalidState, added.commitAdditionalStable());
    var copied_owner = owner;
    var wrong = Transaction.init(&copied_owner, backend.interface());
    try std.testing.expectError(error.InvalidState, wrong.commitAdditionalStable());
}

test "owned storage rolls back each of four allocation failure positions" {
    for (1..5) |failure| {
        var backend: TestAllocator = .{ .fail_at = failure };
        defer backend.deinit();
        var owner: Owner = .{};
        var tx = Transaction.init(&owner, backend.interface());
        for (0..4) |_| {
            _ = tx.reserve(u64, 600, .replaceable) catch |err| {
                try std.testing.expectEqual(error.OutOfMemory, err);
                break;
            };
        }
        try tx.rollback();
        try std.testing.expectEqual(@as(usize, 0), backend.active());
        try std.testing.expect(owner.pending == null and owner.published == null);
        try std.testing.expectError(error.InvalidState, tx.rollback());
        try std.testing.expectError(error.InvalidState, tx.commit());
    }
}

test "owned storage failed return is retained and retried once per pass" {
    var backend: TestAllocator = .{ .fail_return = true };
    defer backend.deinit();
    var owner: Owner = .{};
    var tx = Transaction.init(&owner, backend.interface());
    _ = try tx.reserve(u8, 17, .replaceable);
    _ = try tx.reserve(u64, 800, .replaceable);
    try tx.rollback();
    try std.testing.expectEqual(@as(usize, 2), backend.active());
    try owner.retryUnpublished();
    try std.testing.expectEqual(@as(usize, 4), backend.returns);
    backend.fail_return = false;
    try owner.retryUnpublished();
    try std.testing.expectEqual(@as(usize, 0), backend.active());
    try std.testing.expect(owner.pending == null);
    try owner.retryUnpublished();
    try std.testing.expectEqual(@as(usize, 6), backend.returns);
}

test "owned storage rejects copied transactions and owners before header access" {
    var backend: TestAllocator = .{ .fail_return = true };
    defer backend.deinit();
    var owner: Owner = .{};
    var tx = Transaction.init(&owner, backend.interface());
    _ = try tx.reserve(u8, 12, .replaceable);
    var copied = tx;
    try tx.rollback();
    var copied_owner = owner;
    // The copy contains a real pending header; reclaim via the original and
    // then prove the copy is rejected without touching that poisoned/freed page.
    backend.fail_return = false;
    try owner.retryUnpublished();
    try std.testing.expectError(error.InvalidState, copied.rollback());
    try std.testing.expectError(error.InvalidState, copied.commit());
    try std.testing.expectError(error.InvalidState, copied_owner.retryUnpublished());
}

test "owned storage preserves stable nodes and retires replaced borrowed arrays" {
    var backend: TestAllocator = .{};
    defer backend.deinit();
    var owner: Owner = .{};
    var first = Transaction.init(&owner, backend.interface());
    const old = try first.reserve(u64, 32, .replaceable);
    old[0] = 123;
    const stable = try first.reserve(u64, 4, .stable);
    stable[0] = 456;
    try first.commit();
    try std.testing.expectError(error.InvalidState, first.commit());
    try std.testing.expectError(error.InvalidState, first.rollback());
    var next = Transaction.init(&owner, backend.interface());
    const new = try next.reserve(u64, 64, .replaceable);
    @memcpy(new[0..old.len], old);
    try next.commit();
    try owner.retryUnpublished();
    try std.testing.expectEqual(@as(u64, 123), old[0]);
    try std.testing.expectEqual(@as(u64, 123), new[0]);
    try std.testing.expectEqual(@as(u64, 456), stable[0]);
    try std.testing.expectEqual(@as(usize, 3), backend.active());
    try std.testing.expect(owner.retired != null and owner.retired.?.life == .retired);
    try std.testing.expectEqual(@as(usize, 0), backend.returns);
}

test "owned storage validates sizes and alignment before PMM acquisition" {
    var backend: TestAllocator = .{};
    defer backend.deinit();
    var owner: Owner = .{};
    var tx = Transaction.init(&owner, backend.interface());
    try std.testing.expectError(error.InvalidSize, tx.reserve(u64, 0, .replaceable));
    try std.testing.expectError(error.InvalidSize, tx.reserve(u64, std.math.maxInt(usize), .replaceable));
    try std.testing.expectError(error.InvalidSize, tx.reserve(u8, std.math.maxInt(usize), .replaceable));
    const Wide = struct { bytes: [8192]u8 align(8192) };
    try std.testing.expectError(error.UnsupportedAlignment, tx.reserve(Wide, 1, .replaceable));
    try std.testing.expectEqual(@as(usize, 0), backend.calls);
    const Page = struct { bytes: [4096]u8 align(4096) };
    const page = try tx.reserve(Page, 1, .replaceable);
    try std.testing.expectEqual(@as(usize, 0), @intFromPtr(page.ptr) % 4096);
    try tx.rollback();
}

test "owned storage invocation u64 workspace exceeds legacy count and retains failed return" {
    var backend = TestAllocator{};
    defer backend.deinit();
    var owner = Owner{};
    for ([_]usize{ 511, 512, 513, 131072, 131073, 131079 }) |count| {
        var first = Transaction.init(&owner, backend.interface());
        const pages = try first.reserve(u64, count, .replaceable);
        for (pages, 0..) |*value, i| value.* = i * 4096;
        var second = Transaction.init(&owner, backend.interface());
        const other = try second.reserve(u64, 3, .replaceable);
        try std.testing.expect(pages.ptr != other.ptr);
        @memset(other, 99);
        try second.rollback();
        try std.testing.expectEqual(@as(u64, (count - 1) * 4096), pages[count - 1]);
        backend.fail_return = true;
        try first.rollback();
        try std.testing.expect(owner.pending != null);
        const attempts = backend.returns;
        try owner.retryUnpublished();
        try std.testing.expectEqual(attempts + 1, backend.returns);
        try std.testing.expect(owner.pending != null);
        backend.fail_return = false;
        try owner.retryUnpublished();
        try std.testing.expect(owner.pending == null);
        try std.testing.expectEqual(@as(usize, 0), backend.active());
    }
}
