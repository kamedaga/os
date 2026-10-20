//! Bounded CPU caches and a shared PFN ownership map. The map detects duplicate
//! returns across caches without taking P. Reserved return prefixes are never
//! allocation candidates, and are not counted free until publication commits.
const std = @import("std");
const builtin = @import("builtin");
const diagnostics = @import("../sync/lock_diagnostics.zig");
const profile = @import("../lock_profile.zig");
const Error = @import("../state/errors.zig").KernelError;
pub const capacity = 32;
pub const batch = 16;
// A cache may satisfy a request without inspecting individual pages only if
// its entire half-open interval is contained in that request. These boundaries
// cover KernelState/DMA32 (zone 0), cpu_runtime/platform allocation_limit
// (zones 0+1), and high RAM (zones 1+2) from one physical-layout definition.
const physical = @import("../arch/x86_64/physical_layout.zig");
pub const Interval = struct {
    minimum: u64,
    limit: u64,
    pub fn intersects(self: Interval, minimum: u64, limit: u64) bool {
        return self.minimum < limit and minimum < self.limit;
    }
    pub fn contained(self: Interval, minimum: u64, limit: u64) bool {
        return minimum <= self.minimum and self.limit <= limit;
    }
};
pub const intervals = [_]Interval{
    .{ .minimum = 4096, .limit = physical.low_memory_limit },
    .{ .minimum = physical.low_memory_limit, .limit = physical.allocation_limit },
    .{ .minimum = physical.allocation_limit, .limit = std.math.maxInt(u64) },
};
comptime {
    std.debug.assert(physical.low_memory_limit < physical.allocation_limit);
    std.debug.assert(physical.allocation_limit % 4096 == 0);
}
pub fn zone(pa: u64) usize {
    if (pa < intervals[0].limit) return 0;
    if (pa < intervals[1].limit) return 1;
    return 2;
}
pub fn mask() bool {
    if (builtin.is_test) return false;
    const flags = asm volatile ("pushfq; pop %[flags]; cli"
        : [flags] "=r" (-> u64),
        :
        : .{ .memory = true });
    return flags & (1 << 9) != 0;
}
pub fn restore(enabled: bool) void {
    if (!builtin.is_test and enabled) asm volatile ("sti" ::: .{ .memory = true });
}
pub const Cache = struct {
    byte: diagnostics.Byte = .{},
    pages: [capacity]u64 = undefined,
    len: usize = 0,
    // Lockless eligibility hint only; pages/len still require the cache lock.
    available: usize = 0,
    test_locks: if (builtin.is_test) usize else void = if (builtin.is_test) 0 else {},
    pub fn lowerBound(self: *const Cache, minimum: u64) usize {
        if (self.len == 0 or self.pages[0] >= minimum) return 0;
        var lo: usize = 0;
        var hi = self.len;
        while (lo < hi) {
            const mid = lo + (hi - lo) / 2;
            if (self.pages[mid] < minimum) lo = mid + 1 else hi = mid;
        }
        return lo;
    }
    pub fn insert(self: *Cache, pa: u64) void {
        std.debug.assert(pa != 0 and pa % 4096 == 0);
        std.debug.assert(self.len < capacity);
        const at = self.lowerBound(pa);
        std.mem.copyBackwards(u64, self.pages[at + 1 .. self.len + 1], self.pages[at..self.len]);
        self.pages[at] = pa;
        self.len += 1;
        @atomicStore(usize, &self.available, self.len, .release);
    }
    pub fn remove(self: *Cache, at: usize) u64 {
        const pa = self.pages[at];
        std.mem.copyForwards(u64, self.pages[at .. self.len - 1], self.pages[at + 1 .. self.len]);
        self.len -= 1;
        @atomicStore(usize, &self.available, self.len, .release);
        return pa;
    }
    pub fn lock(self: *Cache) void {
        if (builtin.is_test) _ = @atomicRmw(usize, &self.test_locks, .Add, 1, .monotonic);
        const key = diagnostics.order.Key{ .class = .pmm_cache, .order = @intFromPtr(self) };
        diagnostics.beforeAcquire(&self.byte.record, key);
        if (@cmpxchgStrong(u8, &self.byte.value, 0, 1, .acquire, .monotonic) != null) {
            const waiting = if (comptime profile.enabled) profile.begin() else {};
            while (@cmpxchgWeak(u8, &self.byte.value, 0, 1, .acquire, .monotonic) != null) std.atomic.spinLoopHint();
            if (comptime profile.enabled) profile.pmmCacheWait(waiting);
        }
        diagnostics.acquired(&self.byte.record, key);
    }
    pub fn unlock(self: *Cache) void {
        diagnostics.releasing(&self.byte.record);
        @atomicStore(u8, &self.byte.value, 0, .release);
    }
};
pub const State = enum(u2) { allocated, tree, cache, reserved };
const Region = struct { start: u64, end: u64, offset: usize };
pub const Span = struct { first: usize, count: usize };
pub fn alloc(comptime T: type, count: usize) ![]T {
    if (builtin.is_test) return std.testing.allocator.alloc(T, count);
    return @import("../arch/x86_64/cpu_runtime.zig").alloc(T, count);
}
pub const Ownership = struct {
    regions: []Region = &.{},
    words: []u64 = &.{},
    pub fn create(regions: anytype) !Ownership {
        const catalog = try alloc(Region, regions.len);
        errdefer if (builtin.is_test) std.testing.allocator.free(catalog);
        var pages: usize = 0;
        for (regions, catalog) |source, *dest| {
            dest.* = .{ .start = source.start, .end = source.end, .offset = pages };
            pages = try std.math.add(usize, pages, @intCast((source.end - source.start) / 4096));
        }
        const words = try alloc(u64, (try std.math.add(usize, pages, 31)) / 32);
        @memset(words, 0);
        return .{ .regions = catalog, .words = words };
    }
    pub fn deinit(self: *Ownership) void {
        if (!builtin.is_test) @compileError("boot ownership storage has kernel lifetime");
        std.testing.allocator.free(self.words);
        std.testing.allocator.free(self.regions);
        self.* = .{};
    }
    pub fn span(self: *const Ownership, start: u64, count: usize) Error!Span {
        if (start & 4095 != 0) return error.InvalidState;
        const end = std.math.add(u64, start, std.math.mul(u64, count, 4096) catch return error.InvalidState) catch return error.InvalidState;
        var low: usize = 0;
        var high = self.regions.len;
        while (low < high) {
            const mid = low + (high - low) / 2;
            if (self.regions[mid].end <= start) low = mid + 1 else high = mid;
        }
        if (low == self.regions.len or self.regions[low].start > start) return error.RegionNotFound;
        const first = self.regions[low].offset + @as(usize, @intCast((start - self.regions[low].start) / 4096));
        while (self.regions[low].end < end) {
            if (low + 1 == self.regions.len or self.regions[low].end != self.regions[low + 1].start) return error.RegionNotFound;
            low += 1;
        }
        return .{ .first = first, .count = count };
    }
    pub fn get(self: *const Ownership, page: usize) State {
        const shift: u6 = @intCast((page % 32) * 2);
        return @enumFromInt(@as(u2, @truncate(@atomicLoad(u64, &self.words[page / 32], .acquire) >> shift)));
    }
    pub fn change(self: *Ownership, page: usize, from: State, to: State) bool {
        const sample = if (comptime profile.enabled) profile.begin() else {};
        var attempts: usize = 0;
        var retries: usize = 0;
        var mismatch = false;
        defer if (comptime profile.enabled) profile.pmmCas(sample, attempts, retries, mismatch);
        const word = &self.words[page / 32];
        const shift: u6 = @intCast((page % 32) * 2);
        const bits = @as(u64, 3) << shift;
        var old = @atomicLoad(u64, word, .acquire);
        while (true) {
            if ((old & bits) >> shift != @intFromEnum(from)) {
                mismatch = true;
                return false;
            }
            const new = (old & ~bits) | (@as(u64, @intFromEnum(to)) << shift);
            // Retry from the returned word so unrelated PFNs in this word
            // retain updates made concurrently by other CPUs.
            if (comptime profile.enabled) attempts += 1;
            if (@cmpxchgWeak(u64, word, old, new, .acq_rel, .acquire)) |actual| {
                old = actual;
                if (comptime profile.enabled) retries += 1;
            } else return true;
        }
    }
    pub fn move(self: *Ownership, span_: Span, from: State, to: State) void {
        for (span_.first..span_.first + span_.count) |page| {
            if (!self.change(page, from, to)) @panic("PMM ownership transition");
        }
    }
    pub fn reserve(self: *Ownership, span_: Span) Error!void {
        var claimed: usize = 0;
        errdefer self.move(.{ .first = span_.first, .count = claimed }, .reserved, .allocated);
        while (claimed < span_.count) : (claimed += 1) {
            if (!self.change(span_.first + claimed, .allocated, .reserved)) return error.FreeRangeOverlap;
        }
    }
};

test "cache interval table matches DMA32 high RAM and direct allocation bounds" {
    const low = physical.low_memory_limit;
    const direct = physical.allocation_limit;
    try std.testing.expectEqual(@as(usize, 0), zone(low - 4096));
    try std.testing.expectEqual(@as(usize, 1), zone(low));
    try std.testing.expectEqual(@as(usize, 1), zone(direct - 4096));
    try std.testing.expectEqual(@as(usize, 2), zone(direct));
    const requests = [_]struct { minimum: u64, limit: u64, eligible: [3]bool }{
        .{ .minimum = 0, .limit = low, .eligible = .{ true, false, false } },
        .{ .minimum = low, .limit = std.math.maxInt(u64), .eligible = .{ false, true, true } },
        .{ .minimum = 0, .limit = direct, .eligible = .{ true, true, false } },
        .{ .minimum = 0, .limit = low - 1, .eligible = .{ false, false, false } },
        .{ .minimum = low + 1, .limit = direct, .eligible = .{ false, false, false } },
    };
    for (requests) |request| for (intervals, request.eligible) |interval, eligible| {
        try std.testing.expectEqual(eligible, interval.contained(request.minimum, request.limit));
    };
}

test "cache interval intersection includes clipped requests and excludes touching boundaries" {
    for (intervals[0..2]) |interval| {
        const boundary = interval.limit;
        try std.testing.expect(interval.intersects(boundary - 1, boundary));
        try std.testing.expect(!interval.intersects(boundary, boundary + 1));
        try std.testing.expect(!interval.intersects(boundary + 1, boundary + 2));
    }
    for (intervals) |interval| try std.testing.expect(interval.intersects(0, std.math.maxInt(u64)));
}
