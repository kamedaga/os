//! A cache owns only returned grants, with no remaining resource borrowers.
//! OOM reclamation therefore never enters K/T or a live storage owner. Registry
//! membership is fixed before concurrent allocation; cache contents use a leaf.
const std = @import("std");
const storage = @import("owned_storage.zig");
const page_cache = @import("page_cache.zig");

pub const Registry = struct {
    head: ?*Cache = null,
    // Withdrawn grants already have no borrowers. Rejected physical returns
    // remain independently owned and reachable from PMM pressure.
    fd_retired: ?*storage.RetiredQueue = null,
    pub fn register(self: *Registry, cache: *Cache) void {
        // Boot/test setup only, before either allocator is published.
        std.debug.assert(!cache.registered);
        cache.next = self.head;
        cache.registered = true;
        self.head = cache;
    }
    pub fn retainedPages(self: *Registry) usize {
        var pages: usize = 0;
        var cursor = self.head;
        while (cursor) |cache| : (cursor = cache.next) {
            const irq = page_cache.mask();
            cache.lock.lock();
            pages += cache.pages;
            cache.lock.unlock();
            page_cache.restore(irq);
        }
        return pages;
    }
    pub fn reclaim(self: *Registry) usize {
        var pages: usize = if (self.fd_retired) |domain| domain.reclaim() else 0;
        var cursor = self.head;
        while (cursor) |cache| : (cursor = cache.next) pages += cache.reclaim();
        return pages;
    }
};
const Entry = struct { next: ?*Entry, extent: storage.MappedExtent };
pub const Cache = struct {
    lock: page_cache.Cache = .{},
    head: ?*Entry = null,
    next: ?*Cache = null,
    registered: bool = false,
    backing: ?storage.Allocator = null,
    pages: usize = 0,
    pub fn allocator(self: *Cache, backing: storage.Allocator) storage.Allocator {
        // Called under the owner's setup exclusion. The original issuer must
        // outlive both live allocations and all returned cache entries.
        if (self.backing) |original| {
            std.debug.assert(original.context == backing.context and original.acquire == backing.acquire and
                original.release == backing.release and original.release_mapped == backing.release_mapped);
        } else self.backing = backing;
        return .{ .context = self, .acquire = acquire, .release = unmappedRelease, .release_mapped = release };
    }
    fn acquire(context: *anyopaque, pages: usize) ?storage.MappedExtent {
        const self: *Cache = @ptrCast(@alignCast(context));
        const irq = page_cache.mask();
        self.lock.lock();
        var link = &self.head;
        while (link.*) |entry| : (link = &entry.next) {
            if (entry.extent.allocation.pages != pages) continue;
            const result = entry.extent;
            link.* = entry.next;
            self.pages -= pages;
            self.lock.unlock();
            page_cache.restore(irq);
            return result;
        }
        self.lock.unlock();
        page_cache.restore(irq);
        const backing = self.backing.?;
        // No cache lock survives allocation; PMM may call this cache's reclaimer.
        return backing.acquire(backing.context, pages);
    }
    fn unmappedRelease(_: *anyopaque, _: storage.PhysicalExtent) bool {
        // Cache grants are issued only to owned_storage, which returns the
        // original mapped grant after excluding borrowers.
        unreachable;
    }
    fn release(context: *anyopaque, extent: storage.MappedExtent) bool {
        const self: *Cache = @ptrCast(@alignCast(context));
        const entry: *Entry = @ptrCast(extent.bytes.ptr);
        const irq = page_cache.mask();
        defer page_cache.restore(irq);
        self.lock.lock();
        defer self.lock.unlock();
        entry.* = .{ .next = self.head, .extent = extent };
        self.head = entry;
        self.pages += extent.allocation.pages;
        return true;
    }
    pub fn reclaim(self: *Cache) usize {
        const irq = page_cache.mask();
        self.lock.lock();
        var cursor = self.head;
        self.head = null;
        self.pages = 0;
        self.lock.unlock();
        page_cache.restore(irq);
        var pages: usize = 0;
        while (cursor) |entry| {
            const next = entry.next;
            const extent = entry.extent;
            const backing = self.backing.?;
            // Drop the leaf before PMM/quarantine. A rejected return remains
            // in this cache; never lose an extent or inspect a freed header.
            if (backing.release_mapped) |mapped| {
                if (mapped(backing.context, extent)) pages += extent.allocation.pages else _ = release(self, extent);
            } else if (backing.release(backing.context, extent.allocation)) {
                pages += extent.allocation.pages;
            } else _ = release(self, extent);
            cursor = next;
        }
        return pages;
    }
};

test "detached cache preserves original mapped grants and rejected returns" {
    var backing = storage.TestAllocator{};
    defer backing.deinit();
    var cache = Cache{};
    var registry = Registry{};
    registry.register(&cache);
    var owner = storage.Owner{};
    const allocator = cache.allocator(backing.interface());
    var transaction = storage.Transaction.init(&owner, allocator);
    const values = try transaction.reserve(u64, 32, .stable);
    @memset(values, 0x12345678);
    try transaction.commitAdditionalStable();
    // A published grant is not a cache entry, even during pressure.
    try std.testing.expectEqual(@as(usize, 0), registry.reclaim());
    try std.testing.expectEqual(@as(u64, 0x12345678), values[31]);
    try owner.retireAll();
    try owner.reclaimRetiredAfterBorrowExclusion();
    try std.testing.expectEqual(@as(usize, 1), registry.retainedPages());
    backing.fail_return = true;
    try std.testing.expectEqual(@as(usize, 0), registry.reclaim());
    try std.testing.expectEqual(@as(usize, 1), backing.active());
    try std.testing.expectEqual(@as(usize, 1), registry.retainedPages());
    backing.fail_return = false;
    try std.testing.expectEqual(@as(usize, 1), registry.reclaim());
    try std.testing.expectEqual(@as(usize, 0), backing.active());
    try std.testing.expectEqual(@as(usize, 0), registry.retainedPages());
}

test "detached cache races pressure without reclaiming live grants" {
    const Backend = struct {
        mutex: u8 = 0,
        memory: storage.TestAllocator = .{},
        fn enter(self: *@This()) void {
            while (@cmpxchgWeak(u8, &self.mutex, 0, 1, .acquire, .monotonic) != null) std.atomic.spinLoopHint();
        }
        fn leave(self: *@This()) void {
            @atomicStore(u8, &self.mutex, 0, .release);
        }
        fn acquire(context: *anyopaque, pages: usize) ?storage.MappedExtent {
            const self: *@This() = @ptrCast(@alignCast(context));
            self.enter();
            defer self.leave();
            const allocator = self.memory.interface();
            return allocator.acquire(allocator.context, pages);
        }
        fn release(context: *anyopaque, extent: storage.PhysicalExtent) bool {
            const self: *@This() = @ptrCast(@alignCast(context));
            self.enter();
            defer self.leave();
            const allocator = self.memory.interface();
            return allocator.release(allocator.context, extent);
        }
    };
    const Pressure = struct {
        registry: *Registry,
        done: bool = false,
        fn run(self: *@This()) void {
            while (!@atomicLoad(bool, &self.done, .acquire)) {
                _ = self.registry.reclaim();
                std.Thread.yield() catch {};
            }
        }
    };
    var backend = Backend{};
    defer backend.memory.deinit();
    var cache = Cache{};
    var registry = Registry{};
    registry.register(&cache);
    const allocator = cache.allocator(.{ .context = &backend, .acquire = Backend.acquire, .release = Backend.release });
    var pressure = Pressure{ .registry = &registry };
    const worker = try std.Thread.spawn(.{}, Pressure.run, .{&pressure});
    defer {
        @atomicStore(bool, &pressure.done, true, .release);
        worker.join();
        _ = registry.reclaim();
        std.debug.assert(backend.memory.active() == 0);
    }
    for (0..256) |i| {
        var owner = storage.Owner{};
        var transaction = storage.Transaction.init(&owner, allocator);
        const values = try transaction.reserve(u64, 64, .stable);
        @memset(values, i);
        try transaction.commitAdditionalStable();
        std.Thread.yield() catch {};
        for (values) |value| try std.testing.expectEqual(i, value);
        try owner.retireAll();
        try owner.reclaimRetiredAfterBorrowExclusion();
    }
}
