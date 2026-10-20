//! Free range descriptors occupy their first free physical page. Links are PA,
//! so no boot HHDM pointer survives the CR3 transition. The returning owner must
//! finish WB restoration and CPU/DMA alias withdrawal before this boundary.
//! Quarantine is queried BEFORE the PMM lock to preserve driver -> PMM ordering.
const lock_diagnostics = @import("../sync/lock_diagnostics.zig");
// Profile-only observation never participates in allocation or quarantine decisions.
const lock_profile = @import("../lock_profile.zig");
const std = @import("std");
const cache = @import("page_cache.zig");
const builtin = @import("builtin");
const owned_storage = @import("owned_storage.zig");
const KernelError = @import("../state/errors.zig").KernelError;
const hhdm = @import("../arch/x86_64/boot_hhdm.zig");
const layout = @import("../arch/x86_64/physical_layout.zig");

pub const ReturnProbe = if (builtin.is_test) struct {
    pub var before_publish: ?*const fn (u64, usize) void = null;
} else struct {};

// Serial boot only. Seed only HHDM-proven spans; inaccessible usable spans are
// retained by boot until the direct map is active. Every moved descriptor is
// checked again before publication, so a missing boot mapping never faults.
pub var boot_view: ?hhdm.View = null;
pub threadlocal var test_cpu: if (builtin.is_test) usize else void = if (builtin.is_test) 0 else {};
pub const RegionFreeRange = struct {
    region_id: u64,
    len: usize,
    physical_start: u64,
    previous: ?u64 = null,
    next: ?u64 = null,
    tree: TreeNode = .{},
    subtree_first: u64 = 0,
    subtree_end: u64 = 0,
    maximum_run: usize = 0,
};
const TreeNode = struct {
    parent: ?u64 = null,
    child: [2]?u64 = .{ null, null },
    key: u64 = 0,
    red: bool = false,
    linked: bool = false,
};
const RangeIndex = @import("ordered_index.zig").Index(struct {
    pub const Handle = u64;
    pub const Key = u64;
    pub const Node = TreeNode;
    pub fn resolve(pa: u64) *Node {
        return &descriptor(pa).?.tree;
    }
    pub fn less(a: u64, b: u64) bool {
        return a < b;
    }
    pub const track = lock_profile.enabled;
    pub fn update(pa: u64) void {
        const n = descriptor(pa).?;
        n.subtree_first = pa;
        n.subtree_end = endOf(n.*);
        n.maximum_run = n.len;
        if (n.tree.child[0]) |left| {
            const c = descriptor(left).?;
            n.subtree_first = c.subtree_first;
            n.maximum_run = @max(n.maximum_run, c.maximum_run);
        }
        if (n.tree.child[1]) |right| {
            const c = descriptor(right).?;
            n.subtree_end = c.subtree_end;
            n.maximum_run = @max(n.maximum_run, c.maximum_run);
        }
    }
    pub fn validate(pa: u64) !void {
        const n = descriptor(pa).?;
        var maximum = n.len;
        var first = pa;
        var end = endOf(n.*);
        if (n.tree.child[0]) |left| {
            const c = descriptor(left).?;
            first = c.subtree_first;
            maximum = @max(maximum, c.maximum_run);
        }
        if (n.tree.child[1]) |right| {
            const c = descriptor(right).?;
            end = c.subtree_end;
            maximum = @max(maximum, c.maximum_run);
        }
        try std.testing.expectEqual(maximum, n.maximum_run);
        try std.testing.expectEqual(first, n.subtree_first);
        try std.testing.expectEqual(end, n.subtree_end);
    }
});
pub const PageInterval = cache.Interval;
pub const RegionBoundary = struct { id: u64, start: u64, end: u64 };

fn descriptor(pa: u64) ?*RegionFreeRange {
    if (pa == 0 or pa & 4095 != 0) return null;
    if (builtin.is_test) return &(@import("physical_access_test.zig").slice(RegionFreeRange, pa, 1) orelse return null)[0];
    if (boot_view) |view| {
        return &((layout.hhdmSlice(RegionFreeRange, .fromInt(view.base), .fromInt(pa), 1) orelse return null)[0]);
    }
    return &((layout.identitySlice(RegionFreeRange, .fromInt(pa), 1) orelse return null)[0]);
}
fn checkedDescriptor(pa: u64) ?*RegionFreeRange {
    if (!builtin.is_test) if (boot_view) |view| {
        if (!view.contains(pa, 4096)) return null;
    };
    return descriptor(pa);
}
fn endOf(range: RegionFreeRange) u64 {
    return range.physical_start + @as(u64, range.len) * 4096;
}

pub fn WithQuarantine(comptime quarantined: fn (u64, usize) bool) type {
    return struct {
        const FreePageList = @This();
        lock_word: u8 = 0,
        test_locks: if (builtin.is_test) usize else void = if (builtin.is_test) 0 else {},
        profile_maintenance: if (lock_profile.enabled) [4]u64 else void = if (lock_profile.enabled) @splat(0) else {},
        profile_hold: lock_profile.OptionalToken = if (lock_profile.enabled) .{} else {},
        diagnostic_record: lock_diagnostics.Record = .{},
        len: usize = 0,
        range_len: usize = 0,
        // P owns updates; atomic readers only skip speculative refill attempts.
        tree_pages: [cache.intervals.len]usize = @splat(0),
        index: RangeIndex = .{},
        head: ?u64 = null,
        tail: ?u64 = null,
        regions: ?[]const RegionBoundary = null,
        caches: []cache.Cache = &.{},
        ownership: cache.Ownership = .{},
        total_free: usize = 0,
        reclaimable: @import("reclaim_cache.zig").Registry = .{},
        fn reclaimForAllocation(self: *FreePageList, count: usize, caller: lock_profile.PmmCaller) usize {
            const retained = if (comptime lock_profile.enabled) self.reclaimable.retainedPages() else 0;
            const free = if (comptime lock_profile.enabled) self.pageCount() else 0;
            const returned = self.reclaimable.reclaim();
            if (comptime lock_profile.enabled) lock_profile.reclaimRecovery(caller, count, free, retained, returned);
            return returned;
        }
        pub fn initCaches(self: *FreePageList, cpus: usize) !void {
            if (cpus == 0 or self.caches.len != 0 or boot_view != null) return error.InvalidState;
            // BSP-only, IF=0, before AP publication. Allocate ALL metadata
            // through the uncached allocator before snapshotting free PFNs.
            const slots = try cache.alloc(cache.Cache, try std.math.mul(usize, cpus, cache.intervals.len));
            errdefer if (builtin.is_test) std.testing.allocator.free(slots);
            for (slots) |*slot| slot.* = .{};
            var ownership = try cache.Ownership.create(self.regions orelse return error.InvalidState);
            var cursor = self.head;
            while (cursor) |pa| {
                const n = descriptor(pa).?.*;
                ownership.move(try ownership.span(pa, n.len), .allocated, .tree);
                cursor = n.next;
            }
            self.total_free = self.len;
            self.ownership = ownership;
            self.caches = slots;
        }
        pub fn deinitCaches(self: *FreePageList) void {
            if (!builtin.is_test) @compileError("kernel lifetime cache storage");
            const irq = self.lockCaches();
            self.lock();
            self.drainLocked();
            self.unlock();
            self.unlockCaches(irq);
            self.ownership.deinit();
            std.testing.allocator.free(self.caches);
            self.caches = &.{};
        }
        fn local(self: *FreePageList) *cache.Cache {
            return self.localZone(0);
        }
        fn localZone(self: *FreePageList, zone: usize) *cache.Cache {
            // The caller masks IRQs BEFORE selecting the CPU to prohibit
            // migration between slot selection and cache-lock acquisition.
            const slot = if (builtin.is_test) test_cpu else @import("../smp.zig").currentCpuSlot();
            std.debug.assert(slot < self.caches.len / cache.intervals.len);
            return &self.caches[slot * cache.intervals.len + zone];
        }
        fn lockCaches(self: *const FreePageList) bool {
            const irq = cache.mask();
            // Slot storage is contiguous, so ascending slot matches lock rank.
            // No caller retains a local cache lock or P when entering here.
            for (self.caches) |*c| c.lock();
            return irq;
        }
        fn unlockCaches(self: *const FreePageList, irq: bool) void {
            var remaining = self.caches.len;
            while (remaining != 0) {
                remaining -= 1;
                self.caches[remaining].unlock();
            }
            cache.restore(irq);
        }
        fn drainOneLocked(self: *FreePageList, c: *cache.Cache, count: usize) void {
            for (0..count) |_| {
                const pa = c.pages[c.len - 1];
                // This is a free-to-free ownership move. Never call the driver
                // quarantine callback while holding cache/P: driver precedes P.
                self.appendLocked(pa, 1, pa + 4096, false) catch @panic("PMM cache drain invariant");
                self.ownership.move(self.ownership.span(pa, 1) catch unreachable, .cache, .tree);
                _ = c.remove(c.len - 1);
            }
        }
        fn drainLocked(self: *FreePageList) void {
            // Includes offline/not-yet-online slots. No IPI, scheduling, or
            // allocation is required, even when the caller entered with IF=0.
            for (self.caches) |*c| self.drainOneLocked(c, c.len);
        }
        fn drainMatchingLocked(self: *FreePageList, minimum: u64, limit: u64) void {
            // A failed high-memory preference must not evict low-memory caches
            // used by its fallback. Intersection (not containment) also recovers
            // remote pages for arbitrary requests clipped inside a cache zone.
            for (self.caches, 0..) |*c, index| {
                if (cache.intervals[index % cache.intervals.len].intersects(minimum, limit))
                    self.drainOneLocked(c, c.len);
            }
        }
        fn boundedSingle(self: *FreePageList, minimum: u64, limit: u64) KernelError!owned_storage.PhysicalExtent {
            return self.cachedSingle(minimum, limit, if (minimum != 0 or limit != std.math.maxInt(u64)) 3 else 2);
        }
        fn cachedPop(self: *FreePageList) KernelError!u64 {
            return (try self.cachedSingle(0, std.math.maxInt(u64), 1)).physical.toInt();
        }
        fn cachedExtent(self: *FreePageList, c: *cache.Cache) owned_storage.PhysicalExtent {
            const pa = c.remove(c.len - 1);
            self.ownership.move(self.ownership.span(pa, 1) catch unreachable, .cache, .allocated);
            const before = @atomicRmw(usize, &self.total_free, .Sub, 1, .acq_rel);
            if (comptime lock_profile.enabled) lock_profile.freePages(before - 1);
            var lo: usize = 0;
            var hi = self.regions.?.len;
            while (lo < hi) {
                const mid = lo + (hi - lo) / 2;
                if (self.regions.?[mid].end <= pa) lo = mid + 1 else hi = mid;
            }
            return .{ .physical = .fromInt(pa), .pages = 1, .region_id = self.regions.?[lo].id };
        }
        fn cachedSingle(self: *FreePageList, minimum: u64, limit: u64, kind: usize) KernelError!owned_storage.PhysicalExtent {
            return self.prioritizedSingle(&.{.{ .minimum = minimum, .limit = limit }}, kind, if (kind == 1) .single else .bounded);
        }
        /// Preferences are best effort: all local hits precede all tree refills.
        /// Failure of a preferred interval is not OOM for the combined request.
        pub fn popPagePreferred(self: *FreePageList, requests: []const PageInterval) KernelError!u64 {
            const ipc_start = lock_profile.pmmScopeTimestamp();
            defer lock_profile.pmmScopeEnd(false, ipc_start);
            const stage_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.pmm, stage_sample);
            return (try self.prioritizedSingle(requests, 3, .physical)).physical.toInt();
        }
        fn prioritizedSingle(self: *FreePageList, requests: []const PageInterval, kind: usize, caller: lock_profile.PmmCaller) KernelError!owned_storage.PhysicalExtent {
            return self.prioritizedSingleWithoutReclaim(requests, kind, caller) catch |err| {
                if (err != error.OutOfFreePages) return err;
                // The failed search has released P and every CPU cache leaf.
                // Only detached grants are reclaimed; no K/T acquisition or
                // live owner callback is allowed on this common OOM path.
                if (self.reclaimForAllocation(1, caller) == 0) return err;
                return self.prioritizedSingleWithoutReclaim(requests, kind, caller);
            };
        }
        fn prioritizedSingleWithoutReclaim(self: *FreePageList, requests: []const PageInterval, kind: usize, caller: lock_profile.PmmCaller) KernelError!owned_storage.PhysicalExtent {
            const sample = if (comptime lock_profile.enabled) lock_profile.begin() else {};
            var failed = false;
            var route: lock_profile.PmmRoute = .failed;
            defer if (comptime lock_profile.enabled) lock_profile.pmmDetail(sample, caller, route, 1, failed);
            errdefer failed = true;
            var visits: usize = 0;
            const irq = cache.mask();
            defer cache.restore(irq);
            if (self.caches.len != 0) {
                // Empty hints may race with a return/drain. They never decide
                // OOM: the final locked search below ignores all hints.
                for (requests) |request| {
                    for (cache.intervals, 0..) |interval, zone| {
                        if (!interval.contained(request.minimum, request.limit)) continue;
                        const c = self.localZone(zone);
                        if (@atomicLoad(usize, &c.available, .acquire) == 0) continue;
                        c.lock();
                        defer c.unlock();
                        if (c.len != 0) {
                            if (comptime lock_profile.enabled) lock_profile.pmmCacheHit(kind);
                            route = .hit;
                            return self.cachedExtent(c);
                        }
                    }
                }
            }
            for (requests) |request| {
                const minimum = (std.math.add(u64, request.minimum, 4095) catch continue) & ~@as(u64, 4095);
                if (minimum >= request.limit) continue;
                for (cache.intervals, 0..) |interval, zone| {
                    if (!interval.intersects(minimum, request.limit) or @atomicLoad(usize, &self.tree_pages[zone], .acquire) == 0) continue;
                    const c = if (self.caches.len != 0) self.localZone(zone) else null;
                    if (c) |slot| slot.lock();
                    defer if (c) |slot| slot.unlock();
                    self.lock();
                    defer self.unlock();
                    const lo = @max(minimum, interval.minimum);
                    const hi = @min(request.limit, interval.limit);
                    if (self.findFit(self.index.root, lo, hi, 1, &visits)) |pa| {
                        defer if (comptime lock_profile.enabled) lock_profile.pmmOperation(kind, visits, self.range_len);
                        const range = descriptor(pa).?.*;
                        const start = @max(pa, lo);
                        const batch = c != null and interval.contained(minimum, request.limit);
                        // A speculative empty hint does not establish spare
                        // capacity. Recheck under the lock before inserting.
                        if (batch and c.?.len != 0) {
                            route = .hit;
                            return self.cachedExtent(c.?);
                        }
                        route = if (batch) .refill else .tree;
                        if (batch) {
                            // Fragmented RAM must not collapse a batch to one
                            // page per P acquisition. Each extent remains in
                            // its own region; only individual PAs enter cache.
                            // Publish each successful take before the next:
                            // a later preflight failure retains those free
                            // pages in cache with total_free unchanged.
                            var next: ?u64 = pa;
                            while (next) |candidate| {
                                const current = descriptor(candidate).?.*;
                                const first = @max(candidate, lo);
                                const count = @min(cache.batch - c.?.len, (@min(endOf(current), hi) - first) / 4096);
                                const extent = try self.take(current, @intCast((first - candidate) / 4096), count, true);
                                for (0..count) |i| c.?.insert(extent.physical.toInt() + i * 4096);
                                if (c.?.len == cache.batch) break;
                                next = self.findFit(self.index.root, lo, hi, 1, &visits);
                            }
                            return self.cachedExtent(c.?);
                        }
                        return self.take(range, @intCast((start - pa) / 4096), 1, false);
                    }
                }
            }
            // No local/P lock survives here. Only the combined failure takes
            // all cache locks in address order -> P. Hints cannot hide pages
            // returned concurrently: drain and search every allowed interval.
            const all_irq = self.lockCaches();
            defer self.unlockCaches(all_irq);
            self.lock();
            defer self.unlock();
            route = .reclaim;
            for (requests) |request| self.drainMatchingLocked(request.minimum, request.limit);
            defer if (comptime lock_profile.enabled) lock_profile.pmmOperation(kind, visits, self.range_len);
            for (requests) |request| {
                const minimum = (std.math.add(u64, request.minimum, 4095) catch continue) & ~@as(u64, 4095);
                if (minimum >= request.limit) continue;
                if (self.findFit(self.index.root, minimum, request.limit, 1, &visits)) |pa| {
                    return self.take(descriptor(pa).?.*, @intCast((@max(pa, minimum) - pa) / 4096), 1, false);
                }
            }
            return error.OutOfFreePages;
        }
        fn accountTree(self: *FreePageList, start: u64, end: u64, add: bool) void {
            // Called only after successful mutation preflight under P (or
            // serial boot). Split counts even when a descriptor spans zones.
            for (cache.intervals, 0..) |interval, zone| {
                const lo = @max(start, interval.minimum);
                const hi = @min(end, interval.limit);
                if (lo >= hi) continue;
                const count: usize = @intCast((hi - lo) / 4096);
                const old = @atomicLoad(usize, &self.tree_pages[zone], .monotonic);
                @atomicStore(usize, &self.tree_pages[zone], if (add) old + count else old - count, .release);
            }
        }
        fn lock(self: *const FreePageList) void {
            if (builtin.is_test) _ = @atomicRmw(usize, @constCast(&self.test_locks), .Add, 1, .monotonic);
            const stage_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.pmm_wait, stage_sample);
            const word = @constCast(&self.lock_word);
            while (true) {
                const diagnostic_if = lock_diagnostics.maskInterrupts();
                lock_diagnostics.beforeAcquire(@constCast(&self.diagnostic_record), .{ .class = .pmm, .order = @intFromPtr(self) });
                if (@cmpxchgWeak(u8, word, 0, 1, .acquire, .monotonic) == null) {
                    lock_diagnostics.acquired(@constCast(&self.diagnostic_record), .{ .class = .pmm, .order = @intFromPtr(self) });
                    if (comptime lock_profile.enabled) {
                        @constCast(self).profile_hold = lock_profile.begin();
                        @constCast(self).profile_maintenance = self.index.maintenance;
                    }
                    lock_diagnostics.restoreInterrupts(diagnostic_if);
                    return;
                }
                lock_diagnostics.restoreInterrupts(diagnostic_if);
                while (@atomicLoad(u8, word, .monotonic) != 0) {
                    asm volatile ("pause");
                }
            }
        }

        fn unlock(self: *const FreePageList) void {
            const diagnostic_if = lock_diagnostics.maskInterrupts();
            defer lock_diagnostics.restoreInterrupts(diagnostic_if);
            if (comptime lock_profile.enabled) {
                lock_profile.pmmRelease(self.profile_hold);
                for (self.index.maintenance, self.profile_maintenance, 0..) |after, before, kind| lock_profile.pmmMaintenance(kind, after -% before);
            }
            lock_diagnostics.releasing(@constCast(&self.diagnostic_record));
            @atomicStore(u8, @constCast(&self.lock_word), 0, .release);
        }

        pub fn pageCount(self: *const FreePageList) usize {
            if (self.caches.len != 0) return @atomicLoad(usize, &self.total_free, .acquire);
            self.lock();
            defer self.unlock();
            return self.len;
        }
        pub fn rangeCount(self: *const FreePageList) usize {
            const irq = self.lockCaches();
            defer self.unlockCaches(irq);
            self.lock();
            defer self.unlock();
            @constCast(self).drainLocked();
            return self.range_len;
        }
        // A diagnostic decomposition into aligned power-of-two free blocks,
        // not a claim that PMM uses a buddy allocator. Caller owns P and has
        // drained every cache, so these are exactly the ranges findFit saw.
        pub const Fragmentation = struct {
            total: usize = 0,
            largest: usize = 0,
            orders: [64]usize = @splat(0),
        };
        fn fragmentationLocked(self: *const FreePageList, minimum: u64, limit: u64) Fragmentation {
            var result: Fragmentation = .{};
            var cursor = self.head;
            while (cursor) |pa| {
                const range = descriptor(pa).?.*;
                const start = @max(pa, minimum);
                const end = @min(endOf(range), limit);
                if (start < end) {
                    var pages = (end - start) / 4096;
                    var pfn = start / 4096;
                    result.total += pages;
                    result.largest = @max(result.largest, pages);
                    while (pages != 0) {
                        const order: u6 = @intCast(@min(@ctz(pfn), 63 - @clz(pages)));
                        const block = @as(u64, 1) << order;
                        result.orders[order] += 1;
                        pfn += block;
                        pages -= block;
                    }
                }
                cursor = range.next;
            }
            return result;
        }
        pub const AvailablePages = struct { total: usize = 0, largest_run: usize = 0 };
        pub fn availableBelow(self: *const FreePageList, limit: u64) AvailablePages {
            const irq = self.lockCaches();
            defer self.unlockCaches(irq);
            self.lock();
            defer self.unlock();
            @constCast(self).drainLocked();
            var result: AvailablePages = .{};
            var cursor = self.head;
            while (cursor) |pa| {
                const range = descriptor(pa).?.*;
                if (pa >= limit) break;
                const pages = @min(range.len, (limit - pa) / 4096);
                result.total += pages;
                result.largest_run = @max(result.largest_run, pages);
                cursor = range.next;
            }
            return result;
        }
        pub fn overlaps(self: *const FreePageList, start: u64, end: u64) bool {
            if (self.caches.len != 0) {
                const irq = self.lockCaches();
                defer self.unlockCaches(irq);
                self.lock();
                defer self.unlock();
                // Stop allocation/publication on every CPU. Reservations may
                // only add busy PFNs, so this is a conservative admission query.
                for (self.ownership.regions) |region| {
                    const lo = @max(start, region.start) & ~@as(u64, 4095);
                    const hi = @min(end, region.end);
                    if (lo >= hi) continue;
                    var pa = lo;
                    while (pa < hi) : (pa += 4096) {
                        const i = region.offset + @as(usize, @intCast((pa - region.start) / 4096));
                        if (self.ownership.get(i) != .allocated) return true;
                    }
                }
                return false;
            }
            self.lock();
            defer self.unlock();
            var cursor = self.head;
            while (cursor) |pa| {
                if (pa >= end) break;
                const range = descriptor(pa).?.*;
                if (start < endOf(range)) return true;
                cursor = range.next;
            }
            return false;
        }
        /// A copied diagnostic value owns no descriptor borrow. Indexing is
        /// deliberately test-only; runtime callers must use coherent queries.
        pub fn rangeAt(self: *const FreePageList, index: usize) RegionFreeRange {
            if (!builtin.is_test) @compileError("PMM range indexing is test-only");
            self.lock();
            defer self.unlock();
            var cursor = self.head;
            var i: usize = 0;
            while (cursor) |pa| : (i += 1) {
                const range = descriptor(pa).?.*;
                if (i == index) {
                    var snapshot = range;
                    snapshot.previous = null;
                    snapshot.next = null;
                    snapshot.tree = .{};
                    snapshot.subtree_first = 0;
                    snapshot.subtree_end = 0;
                    snapshot.maximum_run = 0;
                    return snapshot;
                }
                cursor = range.next;
            }
            unreachable;
        }
        fn link(self: *FreePageList, pa: u64, previous: ?u64, next: ?u64) void {
            self.index.insert(pa, pa);
            if (previous) |p| descriptor(p).?.next = pa else self.head = pa;
            if (next) |n| descriptor(n).?.previous = pa else self.tail = pa;
        }
        fn unlink(self: *FreePageList, range: RegionFreeRange) void {
            self.index.remove(range.physical_start);
            if (range.previous) |p| descriptor(p).?.next = range.next else self.head = range.next;
            if (range.next) |n| descriptor(n).?.previous = range.previous else self.tail = range.previous;
            self.range_len -= 1;
        }
        pub fn installRegions(self: *FreePageList, regions: []const RegionBoundary) KernelError!void {
            if (self.regions != null or self.len != 0 or self.range_len != 0) return error.InvalidState;
            for (regions, 0..) |region, i| {
                if (region.start % 4096 != 0 or region.end % 4096 != 0 or region.start >= region.end)
                    return error.InvalidState;
                if (i != 0 and regions[i - 1].end > region.start) return error.InvalidState;
                for (regions[0..i]) |previous| {
                    if (previous.id == region.id) return error.InvalidState;
                }
            }
            self.regions = regions;
        }

        pub fn seedContiguousRange(self: *FreePageList, start: u64, pages: usize) KernelError!void {
            return self.appendContiguousRange(start, pages);
        }
        pub fn appendPage(self: *FreePageList, pa: u64) KernelError!void {
            return self.appendContiguousRange(pa, 1);
        }
        /// No metadata allocation, fixed capacity, or data-page write before
        /// every ownership/overlap/reachability check succeeds. Region IDs are
        /// diagnostic provenance; immutable physical boundaries grant return.
        pub fn appendContiguousRange(self: *FreePageList, start: u64, pages: usize) KernelError!void {
            const ipc_start = lock_profile.pmmScopeTimestamp();
            defer lock_profile.pmmScopeEnd(true, ipc_start);
            const stage_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.pmm, stage_sample);
            const sample = if (comptime lock_profile.enabled) lock_profile.begin() else {};
            var failed = false;
            var route: lock_profile.PmmRoute = .return_tree;
            defer if (comptime lock_profile.enabled) lock_profile.pmmDetail(sample, .returned, route, pages, failed);
            errdefer failed = true;
            if (start == 0 or start & 4095 != 0) return error.InvalidState;
            const bytes = std.math.mul(u64, pages, 4096) catch return error.InvalidState;
            const end = std.math.add(u64, start, bytes) catch return error.InvalidState;
            if (quarantined(start, pages)) return error.PageQuarantined;
            if (self.caches.len != 0 and pages != 0) {
                const span = try self.ownership.span(start, pages);
                // Cache admission preserves the descriptor reachability gate
                // even though this return will not write a descriptor yet.
                _ = checkedDescriptor(start) orelse return error.InvalidState;
                try self.ownership.reserve(span);
                errdefer self.ownership.move(span, .reserved, .allocated);
                if (pages == 1) {
                    route = .return_cache;
                    const irq = cache.mask();
                    defer cache.restore(irq);
                    const c = self.localZone(cache.zone(start));
                    c.lock();
                    defer c.unlock();
                    if (c.len == cache.capacity) {
                        route = .return_spill;
                        self.lock();
                        self.drainOneLocked(c, cache.batch);
                        self.unlock();
                    }
                    c.insert(start);
                    self.ownership.move(span, .reserved, .cache);
                    _ = @atomicRmw(usize, &self.total_free, .Add, 1, .acq_rel);
                    if (comptime lock_profile.enabled) lock_profile.pmmCacheHit(0);
                    return;
                }
                // Range returns retain cache -> P order so the all-cache
                // OOM drain never competes with a reverse lock dependency.
                const irq = cache.mask();
                defer cache.restore(irq);
                const c = self.local();
                c.lock();
                defer c.unlock();
                self.lock();
                defer self.unlock();
                try self.appendLocked(start, pages, end, true);
                self.ownership.move(span, .reserved, .tree);
                _ = @atomicRmw(usize, &self.total_free, .Add, pages, .acq_rel);
                return;
            }
            self.lock();
            defer self.unlock();
            return self.appendLocked(start, pages, end, true);
        }
        fn appendLocked(self: *FreePageList, start: u64, pages: usize, end: u64, observe: bool) KernelError!void {
            var visited: usize = 0;
            defer if (comptime lock_profile.enabled) {
                if (observe) lock_profile.pmmOperation(0, visited, self.range_len);
            };
            const regions = self.regions orelse return error.InvalidState;
            if (pages == 0) return;
            var low: usize = 0;
            var high = regions.len;
            while (low < high) {
                const mid = low + (high - low) / 2;
                if (regions[mid].end <= start) low = mid + 1 else high = mid;
            }
            const first = low;
            if (first == regions.len or regions[first].start > start) return error.RegionNotFound;
            var last = first;
            while (regions[last].end < end) {
                if (last + 1 == regions.len or regions[last + 1].start != regions[last].end) return error.RegionNotFound;
                last += 1;
            }
            var previous: ?u64 = null;
            var next = self.head;
            // A predecessor/successor lookup checks every possible overlap;
            // the address-ordered tree avoids walking unrelated free ranges.
            var cursor = self.index.root;
            next = null;
            while (cursor) |pa| {
                if (comptime lock_profile.enabled) visited += 1;
                const range = descriptor(pa).?.*;
                if (pa < start) {
                    previous = pa;
                    cursor = range.tree.child[1];
                } else {
                    next = pa;
                    cursor = range.tree.child[0];
                }
            }
            if (previous) |pa| if (endOf(descriptor(pa).?.*) > start) return error.FreeRangeOverlap;
            if (next) |pa| if (pa < end) return error.FreeRangeOverlap;
            const merge_previous = if (previous) |pa| blk: {
                const range = descriptor(pa).?.*;
                break :blk endOf(range) == start and range.region_id == regions[first].id;
            } else false;
            const new_pages = std.math.add(usize, self.len, pages) catch return error.InvalidState;
            for (regions[first .. last + 1], first..) |region, index| {
                if (index == first and merge_previous) continue;
                _ = checkedDescriptor(@max(start, region.start)) orelse return error.InvalidState;
            }
            if (builtin.is_test) if (ReturnProbe.before_publish) |probe| probe(start, pages);
            for (regions[first .. last + 1], first..) |region, index| {
                const begin = @max(start, region.start);
                const stop = @min(end, region.end);
                const count: usize = @intCast((stop - begin) / 4096);
                var pa = begin;
                if (index == first and merge_previous) {
                    pa = previous.?;
                    descriptor(pa).?.len += count;
                    self.index.refresh(pa);
                } else {
                    descriptor(pa).?.* = .{ .region_id = region.id, .physical_start = pa, .len = count, .previous = previous, .next = next };
                    self.link(pa, previous, next);
                    self.range_len += 1;
                }
                if (index == last) {
                    if (next) |n| {
                        const after = descriptor(n).?.*;
                        if (n == end and after.region_id == region.id) {
                            descriptor(pa).?.len += after.len;
                            self.unlink(after);
                            self.index.refresh(pa);
                        }
                    }
                }
                previous = pa;
            }
            self.len = new_pages;
            self.accountTree(start, end, true);
        }
        fn take(self: *FreePageList, range: RegionFreeRange, skip: usize, count: usize, move_to_cache: bool) KernelError!owned_storage.PhysicalExtent {
            const start = range.physical_start + @as(u64, skip) * 4096;
            const tail_pages = range.len - skip - count;
            const tail_pa = start + @as(u64, count) * 4096;
            // The new head/tail still belongs to the free range. Preflight
            // before changing links or page accounting; errors retain it all.
            const tail_node = if (tail_pages != 0) checkedDescriptor(tail_pa) orelse return error.InvalidState else null;
            // Every cached page can later become a range head during drain.
            // Preflight the bounded batch before removing any tree ownership.
            if (move_to_cache) for (0..count) |i| {
                _ = checkedDescriptor(start + i * 4096) orelse return error.InvalidState;
            };
            self.unlink(range);
            var previous = range.previous;
            if (skip != 0) {
                descriptor(range.physical_start).?.* = .{ .region_id = range.region_id, .physical_start = range.physical_start, .len = skip, .previous = previous, .next = range.next };
                self.link(range.physical_start, previous, range.next);
                self.range_len += 1;
                previous = range.physical_start;
            }
            if (tail_node) |node| {
                node.* = .{ .region_id = range.region_id, .physical_start = tail_pa, .len = tail_pages, .previous = previous, .next = range.next };
                self.link(tail_pa, previous, range.next);
                self.range_len += 1;
            }
            self.len -= count;
            self.accountTree(start, tail_pa, false);
            if (self.caches.len != 0) {
                self.ownership.move(self.ownership.span(start, count) catch unreachable, .tree, if (move_to_cache) .cache else .allocated);
                if (!move_to_cache) _ = @atomicRmw(usize, &self.total_free, .Sub, count, .acq_rel);
            }
            if (comptime lock_profile.enabled) lock_profile.freePages(if (self.caches.len == 0) self.len else @atomicLoad(usize, &self.total_free, .acquire));
            // All source fields are copied; the returned first page may be
            // overwritten immediately and is never read as metadata again.
            return .{ .physical = .fromInt(start), .pages = count, .region_id = range.region_id };
        }
        pub fn popFront(self: *FreePageList) KernelError!u64 {
            return self.popFrontWithoutReclaim() catch |err| {
                if (err != error.OutOfFreePages) return err;
                // The failed search has released P and every CPU cache leaf.
                // Only detached grants are reclaimed; no K/T acquisition or
                // live owner callback is allowed on this common OOM path.
                if (self.reclaimForAllocation(1, .single) == 0) return err;
                return self.popFrontWithoutReclaim();
            };
        }
        fn popFrontWithoutReclaim(self: *FreePageList) KernelError!u64 {
            const ipc_start = lock_profile.pmmScopeTimestamp();
            defer lock_profile.pmmScopeEnd(false, ipc_start);
            const stage_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.pmm, stage_sample);
            if (self.caches.len != 0) return self.cachedPop();
            self.lock();
            defer self.unlock();
            var visited: usize = 0;
            defer if (comptime lock_profile.enabled) lock_profile.pmmOperation(1, visited, self.range_len);
            if (comptime lock_profile.enabled) visited = 1;
            const pa = self.head orelse return error.OutOfFreePages;
            return (try self.take(descriptor(pa).?.*, 0, 1, false)).physical.toInt();
        }
        pub fn popBack(self: *FreePageList) KernelError!u64 {
            return self.popBackWithoutReclaim() catch |err| {
                if (err != error.OutOfFreePages) return err;
                // The failed search has released P and every CPU cache leaf.
                // Only detached grants are reclaimed; no K/T acquisition or
                // live owner callback is allowed on this common OOM path.
                if (self.reclaimForAllocation(1, .single) == 0) return err;
                return self.popBackWithoutReclaim();
            };
        }
        fn popBackWithoutReclaim(self: *FreePageList) KernelError!u64 {
            const ipc_start = lock_profile.pmmScopeTimestamp();
            defer lock_profile.pmmScopeEnd(false, ipc_start);
            const stage_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.pmm, stage_sample);
            const irq = self.lockCaches();
            defer self.unlockCaches(irq);
            self.lock();
            defer self.unlock();
            self.drainLocked();
            var visited: usize = 0;
            defer if (comptime lock_profile.enabled) lock_profile.pmmOperation(1, visited, self.range_len);
            if (comptime lock_profile.enabled) visited = 1;
            const pa = self.tail orelse return error.OutOfFreePages;
            const range = descriptor(pa).?.*;
            return (try self.take(range, range.len - 1, 1, false)).physical.toInt();
        }
        pub fn popFrontBelow(self: *FreePageList, limit: u64) KernelError!u64 {
            return self.popContiguousBelow(1, limit);
        }
        pub fn popFrontAtOrAbove(self: *FreePageList, minimum: u64) KernelError!u64 {
            return self.popContiguousAtOrAbove(1, minimum);
        }
        pub fn popContiguousAtOrAbove(self: *FreePageList, count: usize, minimum: u64) KernelError!u64 {
            return self.popContiguousInRange(count, minimum, std.math.maxInt(u64));
        }
        pub fn popContiguousBelow(self: *FreePageList, count: usize, limit: u64) KernelError!u64 {
            return self.popContiguousInRange(count, 0, limit);
        }
        pub fn popContiguousInRange(self: *FreePageList, count: usize, minimum: u64, limit: u64) KernelError!u64 {
            return (try self.popContiguousOwnedInRange(count, minimum, limit)).physical.toInt();
        }
        pub fn popContiguousOwnedInRange(self: *FreePageList, count: usize, minimum: u64, limit: u64) KernelError!owned_storage.PhysicalExtent {
            return self.popContiguousOwnedTagged(count, minimum, limit, if (count == 1) .bounded else .contiguous);
        }
        pub fn popContiguousOwnedTagged(self: *FreePageList, count: usize, minimum: u64, limit: u64, caller: lock_profile.PmmCaller) KernelError!owned_storage.PhysicalExtent {
            return self.popContiguousOwnedTaggedWithoutReclaim(count, minimum, limit, caller) catch |err| {
                if (err != error.OutOfFreePages) return err;
                // The failed search has released P and every CPU cache leaf.
                // Only detached grants are reclaimed; no K/T acquisition or
                // live owner callback is allowed on this common OOM path.
                if (self.reclaimForAllocation(count, caller) == 0) return err;
                return self.popContiguousOwnedTaggedWithoutReclaim(count, minimum, limit, caller);
            };
        }
        fn popContiguousOwnedTaggedWithoutReclaim(self: *FreePageList, count: usize, minimum: u64, limit: u64, caller: lock_profile.PmmCaller) KernelError!owned_storage.PhysicalExtent {
            const ipc_start = lock_profile.pmmScopeTimestamp();
            defer lock_profile.pmmScopeEnd(false, ipc_start);
            const stage_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.pmm, stage_sample);
            const kind: usize = if (minimum != 0 or limit != std.math.maxInt(u64)) 3 else 2;
            if (count == 1 and self.caches.len != 0) return self.prioritizedSingle(&.{.{ .minimum = minimum, .limit = limit }}, kind, caller);
            const sample = if (comptime lock_profile.enabled) lock_profile.begin() else {};
            var failed = false;
            var route: lock_profile.PmmRoute = .tree;
            defer if (comptime lock_profile.enabled) lock_profile.pmmDetail(sample, caller, route, count, failed);
            errdefer failed = true;
            if (count == 0) return error.InvalidState;
            if (minimum >= limit) return error.OutOfFreePages;
            const rounded = std.math.add(u64, minimum, 4095) catch return error.OutOfFreePages;
            const aligned = rounded & ~@as(u64, 4095);
            var visited: usize = 0;
            // The requested policy is first-fit within the tree before cache
            // recovery. No hidden cached PA forces global drain on success.
            {
                const irq = cache.mask();
                defer cache.restore(irq);
                const c = if (self.caches.len != 0) self.local() else null;
                if (c) |slot| slot.lock();
                defer if (c) |slot| slot.unlock();
                self.lock();
                defer self.unlock();
                if (self.findFit(self.index.root, aligned, limit, count, &visited)) |pa| {
                    if (comptime lock_profile.enabled) lock_profile.pmmOperation(kind, visited, self.range_len);
                    return self.take(descriptor(pa).?.*, @intCast((@max(pa, aligned) - pa) / 4096), count, false);
                }
            }
            // The defer runs after P/cache unlock. Record the failed search,
            // before reclaim, without allocating or printing under any leaf.
            var fragmentation: if (lock_profile.enabled) ?Fragmentation else void = if (lock_profile.enabled) null else {};
            defer if (comptime lock_profile.enabled) {
                if (fragmentation) |snapshot| lock_profile.fragmentationFailure(caller, count, aligned, limit, snapshot);
            };
            // Release local/P before entering the unchanged all-cache order.
            route = .reclaim;
            const irq = self.lockCaches();
            defer self.unlockCaches(irq);
            self.lock();
            defer self.unlock();
            self.drainLocked();
            defer if (comptime lock_profile.enabled) lock_profile.pmmOperation(kind, visited, self.range_len);
            if (self.findFit(self.index.root, aligned, limit, count, &visited)) |pa| {
                return self.take(descriptor(pa).?.*, @intCast((@max(pa, aligned) - pa) / 4096), count, false);
            }
            if (comptime lock_profile.enabled) fragmentation = self.fragmentationLocked(aligned, limit);
            return error.OutOfFreePages;
        }
        fn findFit(self: *const FreePageList, cursor: ?u64, minimum: u64, limit: u64, count: usize, visited: *usize) ?u64 {
            const pa = cursor orelse return null;
            if (comptime lock_profile.enabled or builtin.is_test) visited.* += 1;
            const n = descriptor(pa).?.*;
            if (n.maximum_run < count or n.subtree_end <= minimum or n.subtree_first >= limit) return null;
            // Whole interior subtrees with a sufficient maximum contain a fit.
            // Only the two clipped boundary paths can fail after this pruning;
            // descending left first therefore preserves first-fit in O(log n).
            if (self.findFit(n.tree.child[0], minimum, limit, count, visited)) |found| return found;
            const start = @max(pa, minimum);
            const end = @min(endOf(n), limit);
            if (start < end and (end - start) / 4096 >= count) return pa;
            return self.findFit(n.tree.child[1], minimum, limit, count, visited);
        }
        pub fn validate(self: *const FreePageList) !void {
            try self.index.validate();
            try std.testing.expectEqual(self.range_len, self.index.count);
            var cursor = self.head;
            var previous: ?u64 = null;
            var pages: usize = 0;
            var ranges: usize = 0;
            var by_zone: [cache.intervals.len]usize = @splat(0);
            while (cursor) |pa| {
                const n = descriptor(pa).?.*;
                try std.testing.expect(n.previous == previous and n.tree.linked);
                if (previous) |p| try std.testing.expect(endOf(descriptor(p).?.*) <= pa);
                if (self.caches.len != 0) {
                    const span = try self.ownership.span(pa, n.len);
                    for (span.first..span.first + span.count) |page| try std.testing.expectEqual(cache.State.tree, self.ownership.get(page));
                }
                for (cache.intervals, 0..) |interval, zone| {
                    const lo = @max(pa, interval.minimum);
                    const hi = @min(endOf(n), interval.limit);
                    if (lo < hi) by_zone[zone] += @intCast((hi - lo) / 4096);
                }
                pages += n.len;
                ranges += 1;
                previous = pa;
                cursor = n.next;
            }
            try std.testing.expect(previous == self.tail);
            try std.testing.expectEqual(self.len, pages);
            for (by_zone, 0..) |count, zone| try std.testing.expectEqual(count, @atomicLoad(usize, &self.tree_pages[zone], .acquire));
            try std.testing.expectEqual(self.range_len, ranges);
            if (self.caches.len != 0) {
                const last = self.ownership.regions[self.ownership.regions.len - 1];
                const total = last.offset + @as(usize, @intCast((last.end - last.start) / 4096));
                const seen = try std.testing.allocator.alloc(bool, total);
                defer std.testing.allocator.free(seen);
                @memset(seen, false);
                var tree_cursor = self.head;
                while (tree_cursor) |pa| {
                    const n = descriptor(pa).?.*;
                    const span = try self.ownership.span(pa, n.len);
                    for (span.first..span.first + span.count) |page| {
                        try std.testing.expect(!seen[page]);
                        seen[page] = true;
                    }
                    tree_cursor = n.next;
                }
                var cached: usize = 0;
                for (self.caches, 0..) |c, slot| {
                    try std.testing.expect(c.len <= cache.capacity);
                    try std.testing.expectEqual(c.len, @atomicLoad(usize, &c.available, .acquire));
                    cached += c.len;
                    for (c.pages[0..c.len], 0..) |pa, i| {
                        try std.testing.expectEqual(slot % cache.intervals.len, cache.zone(pa));
                        if (i != 0) try std.testing.expect(c.pages[i - 1] < pa);
                        const span = try self.ownership.span(pa, 1);
                        try std.testing.expectEqual(cache.State.cache, self.ownership.get(span.first));
                        try std.testing.expect(!seen[span.first]);
                        seen[span.first] = true;
                    }
                }
                var tree_pages: usize = 0;
                var cache_pages: usize = 0;
                for (self.ownership.regions) |region| {
                    const count: usize = @intCast((region.end - region.start) / 4096);
                    for (region.offset..region.offset + count) |page| {
                        try std.testing.expectEqual(self.ownership.get(page) != .allocated, seen[page]);
                        switch (self.ownership.get(page)) {
                            .allocated => {},
                            .tree => tree_pages += 1,
                            .cache => cache_pages += 1,
                            .reserved => return error.InvalidState,
                        }
                    }
                }
                try std.testing.expectEqual(self.len, tree_pages);
                try std.testing.expectEqual(cached, cache_pages);
                try std.testing.expectEqual(self.len + cached, self.pageCount());
            }
        }
    };
}

const TestList = if (builtin.is_test) WithQuarantine(struct {
    fn allowed(_: u64, _: usize) bool {
        return false;
    }
}.allowed) else struct {};

pub const TestBacking = if (builtin.is_test) struct {
    bytes: []align(4096) u8,
    saved: ?@import("physical_access_test.zig").Mapping,
    pub fn init(first: u64, pages: usize) !@This() {
        const fixture = @import("physical_access_test.zig");
        const bytes = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), pages * 4096);
        const saved = fixture.mapping;
        fixture.mapping = .{ .physical = first, .bytes = bytes };
        return .{ .bytes = bytes, .saved = saved };
    }
    pub fn deinit(self: *@This()) void {
        @import("physical_access_test.zig").mapping = self.saved;
        std.testing.allocator.free(self.bytes);
    }
} else struct {};

test "free page list range storage and empty initial state" {
    var list = TestList{};
    try std.testing.expect(@sizeOf(TestList) < 4096);
    try std.testing.expect(@sizeOf(RegionFreeRange) < 4096);
    try std.testing.expectEqual(@as(usize, 0), list.pageCount());
    try std.testing.expectEqual(@as(usize, 0), list.rangeCount());
}

test "free page list quarantine is called once before lock and preserves rejected state" {
    const Query = struct {
        var calls: usize = 0;
        fn rejected(_: u64, _: usize) bool {
            calls += 1;
            return true;
        }
    };
    var list = WithQuarantine(Query.rejected){};
    list.lock_word = 1;
    Query.calls = 0;
    try std.testing.expectError(error.PageQuarantined, list.appendPage(4096));
    try std.testing.expectEqual(@as(usize, 1), Query.calls);
    try std.testing.expectError(error.PageQuarantined, list.appendContiguousRange(8192, 3));
    try std.testing.expectEqual(@as(usize, 2), Query.calls);
    try std.testing.expectEqual(@as(usize, 0), list.len);
    try std.testing.expectEqual(@as(u8, 1), list.lock_word);
}

test "free page list physical lookup survives exhausted ranges and ignores caller grant tag" {
    var backing = try TestBacking.init(4096, 3);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 17, .start = 4096, .end = 16384 }});
    try list.seedContiguousRange(4096, 3);
    var grant = try list.popContiguousOwnedInRange(3, 4096, 16384);
    grant.region_id = 999;
    @memset(backing.bytes, 0x5a); // Allocator may not retain any descriptor here.
    try list.appendContiguousRange(grant.physical.toInt(), grant.pages);
    try std.testing.expectEqual(@as(u64, 17), list.rangeAt(0).region_id);
    try std.testing.expectEqual(@as(usize, 3), list.pageCount());
}

test "free page list duplicate and interval overlap checks do not trust region tags" {
    var backing = try TestBacking.init(4096, 7);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 17, .start = 4096, .end = 32768 }});
    try list.seedContiguousRange(12288, 2);
    const saved = list.rangeAt(0);
    for ([_][2]usize{ .{ 8192, 2 }, .{ 16384, 2 }, .{ 4096, 6 }, .{ 12288, 1 } }) |input| {
        try std.testing.expectError(error.FreeRangeOverlap, list.appendContiguousRange(input[0], input[1]));
        try std.testing.expectEqualDeep(saved, list.rangeAt(0));
    }
    descriptor(12288).?.region_id = 0;
    try std.testing.expectError(error.FreeRangeOverlap, list.appendPage(12288));
}

test "free page list cross region return is atomic and never coalesces region boundaries" {
    var backing = try TestBacking.init(4096, 8);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{
        .{ .id = 17, .start = 4096, .end = 16384 },
        .{ .id = 91, .start = 16384, .end = 24576 },
        .{ .id = 45, .start = 24576, .end = 36864 },
    });
    try list.seedContiguousRange(4096, 1);
    try list.seedContiguousRange(32768, 1);
    try list.appendContiguousRange(8192, 6);
    try std.testing.expectEqual(@as(usize, 8), list.pageCount());
    try std.testing.expectEqual(@as(usize, 3), list.rangeCount());
    try std.testing.expectEqual(@as(usize, 3), list.rangeAt(0).len);
    try std.testing.expectEqual(@as(usize, 2), list.rangeAt(1).len);
    try std.testing.expectEqual(@as(u64, 91), list.rangeAt(1).region_id);
    try std.testing.expectEqual(@as(usize, 3), list.rangeAt(2).len);
    try std.testing.expectError(error.OutOfFreePages, list.popContiguousBelow(4, 36864));
}

test "free page list outside holes alignment overflow and initialization reject without mutation" {
    var list = TestList{};
    try std.testing.expectError(error.InvalidState, list.appendPage(4096));
    try list.installRegions(&.{ .{ .id = 1, .start = 4096, .end = 12288 }, .{ .id = 2, .start = 16384, .end = 24576 } });
    try std.testing.expectError(error.InvalidState, list.installRegions(&.{}));
    try std.testing.expectError(error.InvalidState, list.appendPage(0));
    try std.testing.expectError(error.RegionNotFound, list.appendPage(24576));
    try std.testing.expectError(error.RegionNotFound, list.appendContiguousRange(8192, 4));
    try std.testing.expectError(error.InvalidState, list.appendPage(4097));
    try std.testing.expectError(error.InvalidState, list.appendPage(0xffff_ffff_ffff_f000));
    try std.testing.expectError(error.InvalidState, list.appendContiguousRange(4096, std.math.maxInt(usize)));
    try list.appendContiguousRange(4096, 0);
    try std.testing.expectEqual(@as(usize, 0), list.pageCount());
}

test "free page list invalid catalogs do not publish and region count has no 256 ceiling" {
    var backing = try TestBacking.init(4096, 300);
    defer backing.deinit();
    var list = TestList{};
    try std.testing.expectError(error.InvalidState, list.installRegions(&.{.{ .id = 1, .start = 1, .end = 8192 }}));
    try std.testing.expectError(error.InvalidState, list.installRegions(&.{.{ .id = 1, .start = 8192, .end = 8192 }}));
    try std.testing.expectError(error.InvalidState, list.installRegions(&.{ .{ .id = 1, .start = 4096, .end = 12288 }, .{ .id = 2, .start = 8192, .end = 16384 } }));
    try std.testing.expectError(error.InvalidState, list.installRegions(&.{ .{ .id = 1, .start = 4096, .end = 8192 }, .{ .id = 1, .start = 8192, .end = 16384 } }));
    var regions: [300]RegionBoundary = undefined;
    for (&regions, 0..) |*region, i| region.* = .{ .id = i, .start = (i + 1) * 4096, .end = (i + 2) * 4096 };
    try list.installRegions(&regions);
    try list.appendContiguousRange(4096, 300);
    try std.testing.expectEqual(@as(usize, 300), list.rangeCount());
}

test "free page list exceeds old 65536 descriptors and splits and bridges without metadata allocation" {
    const fragments = 65537;
    var backing = try TestBacking.init(4096, 2 * fragments + 4);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 7, .start = 4096, .end = 4096 + (2 * fragments + 4) * 4096 }});
    for (0..fragments) |i| try list.appendPage(4096 + i * 8192);
    try std.testing.expectEqual(@as(usize, fragments), list.rangeCount());
    try list.appendPage(8192); // Bridge removes a descriptor, no spare slot needed.
    try std.testing.expectEqual(@as(usize, fragments - 1), list.rangeCount());
    try std.testing.expectEqual(@as(u64, 8192), try list.popFrontAtOrAbove(8192));
    try std.testing.expectEqual(@as(usize, fragments), list.rangeCount());
    try list.appendContiguousRange(4096 + fragments * 8192, 4);
    try std.testing.expectEqual(@as(u64, 4096 + fragments * 8192 + 4096), try list.popContiguousAtOrAbove(2, 4096 + fragments * 8192 + 4096));
    try std.testing.expectEqual(@as(usize, fragments + 2), list.rangeCount());
}

test "free page list inaccessible moved descriptor leaves ownership unchanged" {
    var backing = try TestBacking.init(20 << 30, 1);
    defer backing.deinit();
    var list = TestList{};
    const start: u64 = 20 << 30;
    try list.installRegions(&.{.{ .id = 7, .start = start, .end = start + 4 * 4096 }});
    try list.seedContiguousRange(start, 4);
    const saved = list.rangeAt(0);
    try std.testing.expectError(error.InvalidState, list.popFront());
    try std.testing.expectError(error.InvalidState, list.popContiguousAtOrAbove(1, start + 4096));
    try std.testing.expectEqualDeep(saved, list.rangeAt(0));
    try std.testing.expectEqual(@as(usize, 4), list.pageCount());
    // Tail-only allocation never moves the head into an inaccessible page.
    try std.testing.expectEqual(start + 3 * 4096, try list.popBack());
}

test "free page list simultaneous double return has exactly one successful publisher" {
    var backing = try TestBacking.init(4096, 1);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 73, .start = 4096, .end = 8192 }});
    const Worker = struct {
        list: *TestList,
        ready: *u32,
        result: ?KernelError = null,
        fn run(self: *@This()) void {
            _ = @atomicRmw(u32, self.ready, .Add, 1, .acq_rel);
            while (@atomicLoad(u32, self.ready, .acquire) < 2) std.atomic.spinLoopHint();
            self.list.appendPage(4096) catch |err| {
                self.result = err;
            };
        }
    };
    var ready: u32 = 0;
    var a = Worker{ .list = &list, .ready = &ready };
    var b = Worker{ .list = &list, .ready = &ready };
    const first = try std.Thread.spawn(.{}, Worker.run, .{&a});
    const second = std.Thread.spawn(.{}, Worker.run, .{&b}) catch |err| {
        @atomicStore(u32, &ready, 2, .release);
        first.join();
        return err;
    };
    first.join();
    second.join();
    try std.testing.expect((a.result == null and (b.result orelse error.InvalidState) == error.FreeRangeOverlap) or
        (b.result == null and (a.result orelse error.InvalidState) == error.FreeRangeOverlap));
    try std.testing.expectEqual(@as(usize, 1), list.pageCount());
}

test "indexed PMM random fragmentation preserves first-fit and all pages" {
    const count = 257;
    var backing = try TestBacking.init(4096, count);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{ .{ .id = 1, .start = 4096, .end = 129 * 4096 }, .{ .id = 2, .start = 129 * 4096, .end = (count + 1) * 4096 } });
    try list.seedContiguousRange(4096, count);
    var free: [count]bool = @splat(true);
    var rng = std.Random.DefaultPrng.init(0x9f17);
    for (0..6000) |_| {
        const random = rng.random();
        if (random.boolean()) {
            const page = random.uintLessThan(usize, count);
            if (free[page]) {
                try std.testing.expectError(error.FreeRangeOverlap, list.appendPage((page + 1) * 4096));
            } else {
                try list.appendPage((page + 1) * 4096);
                free[page] = true;
            }
        } else {
            const pages = 1 + random.uintLessThan(usize, 8);
            const lo = random.uintLessThan(usize, count);
            const hi = lo + random.uintLessThan(usize, count - lo + 1);
            var expected: ?usize = null;
            for (lo..hi) |first| {
                if (first + pages > hi or (first < 128 and first + pages > 128)) continue;
                var available = true;
                for (free[first .. first + pages]) |yes| available = available and yes;
                if (available) {
                    expected = first;
                    break;
                }
            }
            const result = list.popContiguousInRange(pages, (lo + 1) * 4096, (hi + 1) * 4096);
            if (expected) |first| {
                try std.testing.expectEqual(@as(u64, (first + 1) * 4096), try result);
                @memset(free[first .. first + pages], false);
            } else try std.testing.expectError(error.OutOfFreePages, result);
        }
        try list.validate();
        var free_count: usize = 0;
        for (free) |yes| free_count += @intFromBool(yes);
        try std.testing.expectEqual(free_count, list.pageCount());
    }
}

test "CPU caches preserve ownership recover remote OOM and constrained first-fit" {
    const count = 129;
    var backing = try TestBacking.init(4096, count);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = (count + 1) * 4096 }});
    try list.seedContiguousRange(4096, count);
    try list.initCaches(4);
    defer list.deinitCaches();
    defer test_cpu = 0;
    var held: [count]u64 = undefined;
    var seen: [count]bool = @splat(false);
    for (&held, 0..) |*pa, i| {
        test_cpu = i % 4;
        pa.* = try list.popFront();
        const index: usize = @intCast(pa.* / 4096 - 1);
        try std.testing.expect(index < count and !seen[index]);
        seen[index] = true;
        try std.testing.expectEqual(count - i - 1, list.pageCount());
        try list.validate();
    }
    try std.testing.expectError(error.OutOfFreePages, list.popFront());
    // Return on a slot that represents an offline CPU. Another CPU must drain
    // its cache without an IPI before declaring OOM or missing a contiguous fit.
    test_cpu = 3;
    try list.appendPage(held[0]);
    test_cpu = 0;
    try std.testing.expectError(error.FreeRangeOverlap, list.appendPage(held[0]));
    try std.testing.expectEqual(held[0], try list.popFront());
    for (held, 0..) |pa, i| {
        test_cpu = i % 4;
        try list.appendPage(pa);
    }
    try list.validate();
    try std.testing.expectEqual(@as(usize, count), list.pageCount());
    try std.testing.expectEqual(@as(u64, 4096), try list.popContiguousBelow(64, 65 * 4096));
    try list.appendContiguousRange(4096, 64);
    try std.testing.expectEqual(@as(usize, count), list.availableBelow((count + 1) * 4096).largest_run);
    try list.validate();
    try std.testing.expectError(error.RegionNotFound, list.appendPage((count + 1) * 4096));
    try std.testing.expectError(error.InvalidState, list.appendPage(4097));
}

test "cached range reservation rollback and quarantine preserve all state" {
    const Query = struct {
        var deny: bool = false;
        fn check(_: u64, _: usize) bool {
            return deny;
        }
    };
    var backing = try TestBacking.init(4096, 8);
    defer backing.deinit();
    var list = WithQuarantine(Query.check){};
    try list.installRegions(&.{ .{ .id = 1, .start = 4096, .end = 5 * 4096 }, .{ .id = 2, .start = 5 * 4096, .end = 9 * 4096 } });
    try list.seedContiguousRange(4096, 8);
    try list.initCaches(2);
    defer list.deinitCaches();
    defer test_cpu = 0;
    try std.testing.expectEqual(@as(u64, 4096), try list.popContiguousBelow(4, 5 * 4096));
    test_cpu = 1;
    try list.appendPage(3 * 4096);
    // The first two reservations must roll back when the third is cached.
    try std.testing.expectError(error.FreeRangeOverlap, list.appendContiguousRange(4096, 4));
    try list.validate();
    try std.testing.expectEqual(@as(usize, 5), list.pageCount());
    try std.testing.expectEqual(cache.State.allocated, list.ownership.get(0));
    try std.testing.expectEqual(cache.State.allocated, list.ownership.get(1));
    try std.testing.expect(list.overlaps(3 * 4096, 4 * 4096));
    Query.deny = true;
    try std.testing.expectError(error.PageQuarantined, list.appendPage(4096));
    Query.deny = false;
    try list.appendContiguousRange(4096, 2);
    try list.appendPage(4 * 4096);
    try std.testing.expectError(error.OutOfFreePages, list.popContiguousBelow(8, 9 * 4096));
    try list.validate();
    try std.testing.expectEqual(@as(usize, 8), list.pageCount());
}

test "CPU caches race ownership across four CPUs and reject simultaneous double return" {
    var backing = try TestBacking.init(4096, 256);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 257 * 4096 }});
    try list.seedContiguousRange(4096, 256);
    try list.initCaches(4);
    defer list.deinitCaches();
    const Worker = struct {
        list: *TestList,
        cpu: usize,
        failed: *u32,
        fn run(w: @This()) void {
            test_cpu = w.cpu;
            for (0..3000) |_| {
                const pa = w.list.popFront() catch {
                    _ = @atomicRmw(u32, w.failed, .Add, 1, .monotonic);
                    return;
                };
                w.list.appendPage(pa) catch {
                    _ = @atomicRmw(u32, w.failed, .Add, 1, .monotonic);
                    return;
                };
            }
        }
    };
    var failures: u32 = 0;
    var threads: [4]std.Thread = undefined;
    for (&threads, 0..) |*thread, cpu| thread.* = try std.Thread.spawn(.{}, Worker.run, .{Worker{ .list = &list, .cpu = cpu, .failed = &failures }});
    for (threads) |thread| thread.join();
    try std.testing.expectEqual(@as(u32, 0), failures);
    try list.validate();
    try std.testing.expectEqual(@as(usize, 256), list.pageCount());
    const pa = try list.popFront();
    const Return = struct {
        list: *TestList,
        pa: u64,
        cpu: usize,
        ready: *u32,
        winners: *u32,
        fn run(w: @This()) void {
            test_cpu = w.cpu;
            _ = @atomicRmw(u32, w.ready, .Add, 1, .acq_rel);
            while (@atomicLoad(u32, w.ready, .acquire) < 2) std.atomic.spinLoopHint();
            w.list.appendPage(w.pa) catch |err| {
                std.debug.assert(err == error.FreeRangeOverlap);
                return;
            };
            _ = @atomicRmw(u32, w.winners, .Add, 1, .monotonic);
        }
    };
    var ready: u32 = 0;
    var winners: u32 = 0;
    const a = try std.Thread.spawn(.{}, Return.run, .{Return{ .list = &list, .pa = pa, .cpu = 1, .ready = &ready, .winners = &winners }});
    const b = try std.Thread.spawn(.{}, Return.run, .{Return{ .list = &list, .pa = pa, .cpu = 3, .ready = &ready, .winners = &winners }});
    a.join();
    b.join();
    try std.testing.expectEqual(@as(u32, 1), winners);
    try list.validate();
    try std.testing.expectEqual(@as(usize, 256), list.pageCount());
}

test "cache bounded boundary choices and mixed concurrent drain" {
    var backing = try TestBacking.init(4096, 256);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 257 * 4096 }});
    try list.seedContiguousRange(4096, 256);
    try list.initCaches(4);
    defer list.deinitCaches();
    defer test_cpu = 0;
    const first = try list.popContiguousInRange(1, 2 * 4096 + 1, 10 * 4096 + 4095);
    try std.testing.expectEqual(@as(u64, 3 * 4096), first);
    try std.testing.expectEqual(@as(u64, 4096), try list.popContiguousBelow(1, 3 * 4096));
    try std.testing.expectError(error.OutOfFreePages, list.popContiguousInRange(1, 2 * 4096, 3 * 4096 - 1));
    test_cpu = 3;
    try list.appendPage(first);
    try list.appendPage(4096);
    test_cpu = 0;
    const fitting = try list.popContiguousBelow(1, 257 * 4096);
    try std.testing.expect(fitting >= 4096 and fitting < 257 * 4096);
    try list.appendPage(fitting);
    const Worker = struct {
        list: *TestList,
        cpu: usize,
        failures: *u32,
        fn work(w: @This()) !void {
            test_cpu = w.cpu;
            for (0..400) |iteration| {
                const pages: usize = if (w.cpu == 0) 2 else 1;
                const pa = if (w.cpu == 2) try w.list.popFront() else try w.list.popContiguousInRange(pages, 4096, 257 * 4096);
                if (w.cpu == 3 and iteration % 8 == 0) _ = w.list.availableBelow(257 * 4096);
                try w.list.appendContiguousRange(pa, pages);
            }
        }
        fn run(w: @This()) void {
            w.work() catch {
                _ = @atomicRmw(u32, w.failures, .Add, 1, .monotonic);
            };
        }
    };
    var failures: u32 = 0;
    var threads: [4]std.Thread = undefined;
    for (&threads, 0..) |*thread, cpu| thread.* = try std.Thread.spawn(.{}, Worker.run, .{Worker{ .list = &list, .cpu = cpu, .failures = &failures }});
    for (threads) |thread| thread.join();
    try std.testing.expectEqual(@as(u32, 0), failures);
    try list.validate();
    try std.testing.expectEqual(@as(usize, 256), list.pageCount());
}

test "cache conservation validator rejects duplicate and orphan cancellation" {
    var backing = try TestBacking.init(4096, 32);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 33 * 4096 }});
    try list.seedContiguousRange(4096, 32);
    try list.initCaches(1);
    defer list.deinitCaches();
    _ = try list.popFrontBelow(cache.intervals[0].limit);
    const c = &list.caches[0];
    try std.testing.expect(c.len >= 2);
    const saved = c.pages[1];
    c.pages[1] = c.pages[0];
    try std.testing.expectError(error.TestUnexpectedResult, list.validate());
    c.pages[1] = saved;
    try list.validate();
}

test "cache refill preflights every future descriptor without losing pages" {
    var backing = try TestBacking.init(4096, 1);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 5 * 4096 }});
    try list.seedContiguousRange(4096, 4);
    try list.initCaches(1);
    defer list.deinitCaches();
    try std.testing.expectError(error.InvalidState, list.popFront());
    try std.testing.expectEqual(@as(usize, 4), list.pageCount());
    try std.testing.expectEqual(@as(usize, 0), list.caches[0].len);
    try list.validate();
}

test "cached pages above four GiB cannot satisfy DMA32 requests" {
    const cutoff: u64 = 1 << 32;
    var backing = try TestBacking.init(cutoff - 8192, 6);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = cutoff - 8192, .end = cutoff + 16384 }});
    try list.seedContiguousRange(cutoff - 8192, 6);
    try list.initCaches(2);
    defer list.deinitCaches();
    defer test_cpu = 0;
    test_cpu = 1;
    const high = try list.popFrontAtOrAbove(cutoff);
    try std.testing.expect(high >= cutoff);
    test_cpu = 0;
    const low_a = try list.popFrontBelow(cutoff);
    const low_b = try list.popFrontBelow(cutoff);
    try std.testing.expect(low_a != low_b and low_a < cutoff and low_b < cutoff);
    try std.testing.expectError(error.OutOfFreePages, list.popFrontBelow(cutoff));
    try list.appendPage(high);
    try std.testing.expectError(error.OutOfFreePages, list.popFrontBelow(cutoff));
    try list.validate();
}

fn resetTestLocks(list: *TestList) void {
    list.test_locks = 0;
    for (list.caches) |*c| c.test_locks = 0;
}
fn testCacheLocks(list: *const TestList) usize {
    var total: usize = 0;
    for (list.caches) |c| total += c.test_locks;
    return total;
}
test "bounded single uses only local zone on 72 CPUs and drains remote on OOM" {
    const start = cache.intervals[1].minimum;
    var backing = try TestBacking.init(start, 16);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = start, .end = start + 16 * 4096 }});
    try list.seedContiguousRange(start, 16);
    try list.initCaches(72);
    defer list.deinitCaches();
    defer test_cpu = 0;
    test_cpu = 71;
    resetTestLocks(&list);
    const first = try list.popFrontAtOrAbove(start);
    try std.testing.expectEqual(@as(usize, 1), list.test_locks);
    try std.testing.expectEqual(@as(usize, 1), testCacheLocks(&list));
    try std.testing.expectEqual(@as(usize, 1), list.caches[71 * cache.intervals.len + 1].test_locks);
    std.debug.print("\nBOUNDED_LOCKS cpus=72 refill P={d} cache={d} remote=0\n", .{ list.test_locks, testCacheLocks(&list) });
    resetTestLocks(&list);
    const second = try list.popFrontAtOrAbove(start);
    try std.testing.expectEqual(@as(usize, 0), list.test_locks);
    try std.testing.expectEqual(@as(usize, 1), testCacheLocks(&list));
    try std.testing.expectEqual(@as(usize, 1), list.caches[71 * cache.intervals.len + 1].test_locks);
    std.debug.print("BOUNDED_LOCKS cpus=72 hit P={d} cache={d} remote=0\n", .{ list.test_locks, testCacheLocks(&list) });
    try list.appendPage(first);
    try list.appendPage(second);
    try std.testing.expectEqual(@as(usize, 0), list.len);
    test_cpu = 0;
    resetTestLocks(&list);
    const recovered = try list.popFrontAtOrAbove(start);
    for (list.caches) |c| try std.testing.expect(c.test_locks != 0);
    try std.testing.expectEqual(@as(usize, 15), list.pageCount());
    std.debug.print("BOUNDED_LOCKS cpus=72 OOM-recovery P={d} cache={d}\n", .{ list.test_locks, testCacheLocks(&list) });
    try list.appendPage(recovered);
    try list.validate();
}

test "cache refill and returns classify exact four GiB and allocation limit boundaries" {
    defer test_cpu = 0;
    for ([_]u64{ cache.intervals[0].limit, cache.intervals[1].limit }) |boundary| {
        var backing = try TestBacking.init(boundary - 4096, 3);
        defer backing.deinit();
        var list = TestList{};
        try list.installRegions(&.{.{ .id = 1, .start = boundary - 4096, .end = boundary + 8192 }});
        try list.seedContiguousRange(boundary - 4096, 3);
        try list.initCaches(2);
        defer list.deinitCaches();
        const below = try list.popFrontBelow(boundary);
        try std.testing.expectEqual(boundary - 4096, below);
        const above = try list.popFrontAtOrAbove(boundary);
        try std.testing.expect(above >= boundary);
        try list.validate();
        try list.appendPage(below);
        try list.appendPage(above);
        try list.validate();
        // A partially contained zone is never used blindly. Both pages below
        // boundary+4096 are cached; OOM recovery must find the exact boundary.
        const exact = try list.popContiguousInRange(1, boundary, boundary + 4096);
        try std.testing.expectEqual(boundary, exact);
        try list.appendPage(exact);
        try std.testing.expectEqual(@as(usize, 3), list.pageCount());
        try list.validate();
    }
}

test "audit main bounded API routes include both eligible intervals" {
    const Case = struct {
        label: []const u8,
        start: u64,
        minimum: u64,
        limit: u64,
        api: enum { above, below, contiguous },
        selected: usize,
        attempts: usize,
        fn allocate(c: @This(), list: *TestList) !u64 {
            return switch (c.api) {
                .above => list.popFrontAtOrAbove(c.minimum),
                .below => list.popFrontBelow(c.limit),
                .contiguous => list.popContiguousBelow(1, c.limit),
            };
        }
    };
    const low = layout.low_memory_limit;
    const direct = layout.allocation_limit;
    const cases = [_]Case{
        .{ .label = "allocPhysicalPage-high-zone1", .start = low, .minimum = low, .limit = std.math.maxInt(u64), .api = .above, .selected = 1, .attempts = 1 },
        .{ .label = "allocPhysicalPage-high-zone2", .start = direct, .minimum = low, .limit = std.math.maxInt(u64), .api = .above, .selected = 2, .attempts = 1 },
        .{ .label = "allocPhysicalPage-low", .start = 4096, .minimum = 0, .limit = low, .api = .below, .selected = 0, .attempts = 1 },
        .{ .label = "cpu_runtime-platform-low", .start = 4096, .minimum = 0, .limit = direct, .api = .contiguous, .selected = 0, .attempts = 1 },
        .{ .label = "cpu_runtime-platform-high", .start = low, .minimum = 0, .limit = direct, .api = .contiguous, .selected = 1, .attempts = 1 },
        .{ .label = "vtd-amd_iommu-DMA32", .start = 4096, .minimum = 0, .limit = low, .api = .below, .selected = 0, .attempts = 1 },
    };
    defer test_cpu = 0;
    for (cases) |c| {
        var backing = try TestBacking.init(c.start, 32);
        defer backing.deinit();
        var list = TestList{};
        try list.installRegions(&.{.{ .id = 1, .start = c.start, .end = c.start + 32 * 4096 }});
        try list.seedContiguousRange(c.start, 32);
        try list.initCaches(72);
        defer list.deinitCaches();
        test_cpu = 7;
        for (0..2) |trial| {
            resetTestLocks(&list);
            const pa = try c.allocate(&list);
            try std.testing.expect(pa >= c.minimum and pa + 4096 <= c.limit);
            try std.testing.expectEqual(c.attempts - trial, list.test_locks);
            try std.testing.expectEqual(c.attempts, testCacheLocks(&list));
            // Batch ownership proves this was refill/hit, not one-page generic
            // tree allocation. Remote slot counters must remain untouched.
            try std.testing.expectEqual(cache.batch - 1 - trial, list.caches[7 * cache.intervals.len + c.selected].len);
            try std.testing.expectEqual(@as(usize, 32 - cache.batch), list.len);
            for (list.caches, 0..) |slot, index| {
                if (index / cache.intervals.len != 7) try std.testing.expectEqual(@as(usize, 0), slot.test_locks);
            }
            std.debug.print("\nAUDIT_ROUTE {s} {s} P={d} cache={d} remote=0 generic=0 cached={d}\n", .{ c.label, if (trial == 0) "refill" else "hit", list.test_locks, testCacheLocks(&list), list.caches[7 * cache.intervals.len + c.selected].len });
            try list.validate();
        }
    }
}

test "audit high exhausted fallback preserves unrelated local and remote caches" {
    var backing = try TestBacking.init(4096, 32);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 33 * 4096 }});
    try list.seedContiguousRange(4096, 32);
    try list.initCaches(72);
    defer list.deinitCaches();
    defer test_cpu = 0;
    test_cpu = 3;
    try list.appendPage(try list.popFrontBelow(layout.low_memory_limit));
    test_cpu = 7;
    for (0..2) |trial| {
        const before = list.caches[7 * cache.intervals.len].len;
        resetTestLocks(&list);
        try std.testing.expectError(error.OutOfFreePages, list.popFrontAtOrAbove(layout.low_memory_limit));
        try std.testing.expectEqual(@as(usize, 1), list.test_locks);
        try std.testing.expectEqual(@as(usize, 216), testCacheLocks(&list));
        try std.testing.expectEqual(before, list.caches[7 * cache.intervals.len].len);
        try std.testing.expectEqual(@as(usize, 16), list.caches[3 * cache.intervals.len].len);
        std.debug.print("\nAUDIT_ROUTE high-exhausted trial={d} OOM P={d} cache={d} low-before={d} low-after={d} remote-low=16\n", .{ trial, list.test_locks, testCacheLocks(&list), before, before });
        resetTestLocks(&list);
        const pa = try list.popFrontBelow(layout.low_memory_limit);
        try std.testing.expectEqual(@as(usize, 1) - trial, list.test_locks);
        try std.testing.expectEqual(@as(usize, 1), testCacheLocks(&list));
        try std.testing.expectEqual(@as(usize, 15), list.caches[7 * cache.intervals.len].len);
        std.debug.print("AUDIT_ROUTE low-fallback trial={d} P={d} cache=1 remote=0\n", .{ trial, list.test_locks });
        try list.appendPage(pa);
        try list.validate();
    }
}

test "clipped single request recovers matching remote cache before OOM" {
    var backing = try TestBacking.init(4096, 16);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 17 * 4096 }});
    try list.seedContiguousRange(4096, 16);
    try list.initCaches(4);
    defer list.deinitCaches();
    defer test_cpu = 0;
    test_cpu = 3;
    try list.appendPage(try list.popFrontBelow(layout.low_memory_limit));
    try std.testing.expectEqual(@as(usize, 0), list.len);
    test_cpu = 0;
    const exact = try list.popContiguousInRange(1, 3 * 4096, 4 * 4096);
    try std.testing.expectEqual(@as(u64, 3 * 4096), exact);
    try std.testing.expectEqual(@as(usize, 0), list.caches[3 * cache.intervals.len].len);
    try std.testing.expectEqual(@as(usize, 15), list.pageCount());
    try list.appendPage(exact);
    try list.validate();
}
test "boot bounded allocation before cache initialization uses tree" {
    var backing = try TestBacking.init(4096, 16);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 17 * 4096 }});
    try list.seedContiguousRange(4096, 16);
    resetTestLocks(&list);
    try std.testing.expectEqual(@as(u64, 4096), try list.popContiguousBelow(1, layout.allocation_limit));
    try std.testing.expectEqual(@as(usize, 1), list.test_locks);
    try std.testing.expectEqual(@as(usize, 0), testCacheLocks(&list));
    try list.validate();
}

test "priority allocation high exhaustion avoids premature global OOM" {
    const low = layout.low_memory_limit;
    var backing = try TestBacking.init(low - 32 * 4096, 33);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = low - 32 * 4096, .end = low + 4096 }});
    try list.seedContiguousRange(low - 32 * 4096, 33);
    try list.initCaches(72);
    defer list.deinitCaches();
    defer test_cpu = 0;
    test_cpu = 7;
    const requests = [_]PageInterval{
        .{ .minimum = low, .limit = std.math.maxInt(u64) },
        .{ .minimum = 0, .limit = low },
    };
    try std.testing.expectEqual(low, try list.popPagePreferred(&requests));
    try std.testing.expectEqual(@as(usize, 0), list.tree_pages[1]);
    for (0..32) |i| {
        resetTestLocks(&list);
        const pa = try list.popPagePreferred(&requests);
        try std.testing.expect(pa < low);
        try std.testing.expectEqual(@as(usize, if (i % cache.batch == 0) 1 else 0), list.test_locks);
        try std.testing.expectEqual(@as(usize, 1), testCacheLocks(&list));
        for (list.caches, 0..) |slot, index| {
            if (index != 7 * cache.intervals.len) try std.testing.expectEqual(@as(usize, 0), slot.test_locks);
        }
        try list.validate();
    }
    resetTestLocks(&list);
    try std.testing.expectError(error.OutOfFreePages, list.popPagePreferred(&requests));
    try std.testing.expectEqual(@as(usize, 1), list.test_locks);
    try std.testing.expectEqual(@as(usize, 216), testCacheLocks(&list));
    std.debug.print("\nPRIORITY_LOCKS cpus=72 exhausted-high low-hit P=0 local=1 remote=0; refill P=1 local=1 remote=0; true-OOM P=1 cache=216\n", .{});
}
test "priority uses every local cache before preferred tree and recovers remote only on combined miss" {
    const low = layout.low_memory_limit;
    var backing = try TestBacking.init(low - 16 * 4096, 17);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = low - 16 * 4096, .end = low + 4096 }});
    try list.seedContiguousRange(low - 16 * 4096, 17);
    try list.initCaches(2);
    defer list.deinitCaches();
    defer test_cpu = 0;
    const requests = [_]PageInterval{ .{ .minimum = low, .limit = std.math.maxInt(u64) }, .{ .minimum = 0, .limit = low } };
    const first = try list.popFrontBelow(low);
    resetTestLocks(&list);
    const second = try list.popPagePreferred(&requests);
    try std.testing.expect(second < low);
    try std.testing.expectEqual(@as(usize, 0), list.test_locks);
    try std.testing.expectEqual(@as(usize, 1), list.tree_pages[1]);
    test_cpu = 1;
    try std.testing.expectEqual(low, try list.popPagePreferred(&requests));
    resetTestLocks(&list);
    const recovered = try list.popPagePreferred(&requests);
    try std.testing.expect(recovered < low);
    try std.testing.expectEqual(@as(usize, 1), list.test_locks);
    try std.testing.expectEqual(@as(usize, 6), testCacheLocks(&list));
    try list.appendPage(first);
    try list.appendPage(second);
    try list.appendPage(recovered);
    try list.validate();
}
test "stale empty cache hint cannot declare false OOM" {
    var backing = try TestBacking.init(4096, 1);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 8192 }});
    try list.seedContiguousRange(4096, 1);
    try list.initCaches(1);
    defer list.deinitCaches();
    // Models a return after a speculative zero observation. The final
    // recovery must ignore the hint and inspect locked cache state.
    const pa = try list.popFront();
    try list.appendPage(pa);
    @atomicStore(usize, &list.caches[0].available, 0, .release);
    try std.testing.expectEqual(pa, try list.popFrontBelow(layout.low_memory_limit));
    try list.validate();
}

test "false empty hint with full cache and nonempty tree does not overfill" {
    var backing = try TestBacking.init(4096, 64);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 65 * 4096 }});
    try list.seedContiguousRange(4096, 64);
    try list.initCaches(1);
    defer list.deinitCaches();
    var allocated: [32]u64 = undefined;
    for (&allocated) |*pa| pa.* = try list.popFront();
    for (allocated) |pa| try list.appendPage(pa);
    try std.testing.expectEqual(cache.capacity, list.caches[0].len);
    try std.testing.expectEqual(@as(usize, 32), list.len);
    @atomicStore(usize, &list.caches[0].available, 0, .release);
    _ = try list.popFrontBelow(layout.low_memory_limit);
    try std.testing.expectEqual(@as(usize, 31), list.caches[0].len);
    try std.testing.expectEqual(@as(usize, 32), list.len);
    try list.validate();
}

test "page zero is never admitted and identity lower bound hits low cache" {
    var backing = try TestBacking.init(4096, 32);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 0, .end = 33 * 4096 }});
    try std.testing.expectError(error.InvalidState, list.seedContiguousRange(0, 1));
    try list.seedContiguousRange(4096, 32);
    try list.initCaches(72);
    defer list.deinitCaches();
    try std.testing.expectError(error.InvalidState, list.appendPage(0));
    for (0..32) |i| {
        resetTestLocks(&list);
        _ = try list.popContiguousOwnedInRange(1, 4096, layout.allocation_limit);
        try std.testing.expectEqual(@as(usize, if (i % cache.batch == 0) 1 else 0), list.test_locks);
        try std.testing.expectEqual(@as(usize, 1), testCacheLocks(&list));
        for (list.caches[3..]) |c| try std.testing.expectEqual(@as(usize, 0), c.test_locks);
        try list.validate();
    }
}
test "contiguous uses tree first then recovers caches only on miss" {
    var backing = try TestBacking.init(4096, 32);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 33 * 4096 }});
    try list.seedContiguousRange(4096, 32);
    try list.initCaches(72);
    defer list.deinitCaches();
    try list.appendPage(try list.popFront());
    resetTestLocks(&list);
    _ = try list.popContiguousBelow(16, layout.allocation_limit);
    try std.testing.expectEqual(@as(usize, 1), list.test_locks);
    try std.testing.expectEqual(@as(usize, 1), testCacheLocks(&list));
    try std.testing.expectEqual(@as(usize, 16), list.caches[0].len);
    resetTestLocks(&list);
    _ = try list.popContiguousBelow(16, layout.allocation_limit);
    try std.testing.expectEqual(@as(usize, 2), list.test_locks);
    try std.testing.expectEqual(@as(usize, 217), testCacheLocks(&list));
    try std.testing.expectEqual(@as(usize, 0), list.pageCount());
    try list.validate();
}

test "fragmented single refill batches across ranges and region boundaries" {
    const low = layout.low_memory_limit;
    var backing = try TestBacking.init(low - 64 * 4096, 65);
    defer backing.deinit();
    var list = TestList{};
    const first = low - 64 * 4096;
    try list.installRegions(&.{ .{ .id = 1, .start = first, .end = first + 16 * 4096 }, .{ .id = 2, .start = first + 16 * 4096, .end = low + 4096 } });
    for (0..32) |i| try list.seedContiguousRange(first + i * 8192, 1);
    try list.seedContiguousRange(low, 1);
    try list.initCaches(72);
    defer list.deinitCaches();
    for (0..16) |i| {
        resetTestLocks(&list);
        const extent = try list.popContiguousOwnedInRange(1, 4096, low);
        try std.testing.expect(extent.physical.toInt() < low);
        try std.testing.expectEqual(@as(u64, if (extent.physical.toInt() < first + 16 * 4096) 1 else 2), extent.region_id);
        try std.testing.expectEqual(@as(usize, if (i == 0) 1 else 0), list.test_locks);
        try std.testing.expectEqual(@as(usize, 1), testCacheLocks(&list));
        try list.validate();
    }
    try std.testing.expectEqual(@as(usize, 1), list.tree_pages[1]);
}
test "partial batch preflight failure retains previously cached free pages" {
    var backing = try TestBacking.init(4096, 4);
    defer backing.deinit();
    var list = TestList{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 5 * 4096 }});
    try list.seedContiguousRange(4096, 1);
    try list.seedContiguousRange(3 * 4096, 2);
    try list.initCaches(1);
    defer list.deinitCaches();
    const fixture = @import("physical_access_test.zig");
    fixture.mapping.?.bytes = backing.bytes[0 .. 3 * 4096];
    defer fixture.mapping.?.bytes = backing.bytes;
    try std.testing.expectError(error.InvalidState, list.popFrontBelow(layout.low_memory_limit));
    try std.testing.expectEqual(@as(usize, 1), list.caches[0].len);
    try std.testing.expectEqual(@as(usize, 2), list.len);
    try std.testing.expectEqual(@as(usize, 3), list.pageCount());
    fixture.mapping.?.bytes = backing.bytes;
    try list.validate();
    const retained = try list.popFrontBelow(layout.low_memory_limit);
    try std.testing.expectEqual(@as(u64, 4096), retained);
    try list.appendPage(retained);
    try list.validate();
}

test "free page list fragmentation snapshot distinguishes aggregate free from fitting runs" {
    var backing = try TestBacking.init(4096, 32);
    defer backing.deinit();
    var list: TestList = .{};
    try list.installRegions(&.{.{ .id = 0, .start = 4096, .end = 33 * 4096 }});
    for ([_]u64{ 1, 5, 9, 13 }) |pfn| try list.appendContiguousRange(pfn * 4096, 2);
    try std.testing.expectError(error.OutOfFreePages, list.popContiguousOwnedInRange(3, 4096, 33 * 4096));
    list.lock();
    const snapshot = list.fragmentationLocked(4096, 33 * 4096);
    const clipped = list.fragmentationLocked(2 * 4096, 10 * 4096);
    list.unlock();
    try std.testing.expectEqual(@as(usize, 8), snapshot.total);
    try std.testing.expectEqual(@as(usize, 2), snapshot.largest);
    // Each run starts at an odd PFN: aligned decomposition gives singles,
    // even though first-fit can satisfy an unaligned two-page allocation.
    try std.testing.expectEqual(@as(usize, 8), snapshot.orders[0]);
    try std.testing.expectEqual(@as(usize, 0), snapshot.orders[1]);
    try std.testing.expectEqual(@as(usize, 4), clipped.total);
    try std.testing.expectEqual(@as(usize, 2), clipped.largest);
    _ = try list.popContiguousOwnedInRange(2, 4096, 33 * 4096);
}
