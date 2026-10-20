const std = @import("std");
const paging = @import("ram_cache_paging.zig");
const vm = @import("user_vm.zig");
const platform = @import("../arch/x86_64/platform.zig");

pub const Requirements = struct {
    sources: usize = 0,
    leaves: usize = 0,
    splits: usize = 0,
};

fn visitSources(pages: []const u64, context: anytype, comptime visit: anytype) !void {
    try platform.validateRamCachePages(pages);
    try platform.visitRamCacheDirectAliases(pages, context, visit);
    try vm.visitRamCacheProcessAliases(pages, context, visit);
}

/// Counts are upper bounds when sources share a directory/leaf; preparation
/// deduplicates pointer identity. Callers reserve all scratch before commit.
pub fn measure(pages: []const u64) !Requirements {
    const Counter = struct {
        pages: []const u64,
        result: Requirements = .{},
        fn visit(self: *@This(), source: paging.AliasSource) !void {
            self.result.sources = try std.math.add(usize, self.result.sources, 1);
            const count: usize = switch (source) {
                .leaf => 1,
                .identity => |identity| blk: {
                    self.result.splits = try std.math.add(usize, self.result.splits, 1);
                    var n: usize = 0;
                    var last_chunk: ?u64 = null;
                    if (identity.size == .page) return error.Invalid;
                    for (self.pages) |pa| if (pa >= identity.base and pa - identity.base < identity.size.bytes()) {
                        n += 1;
                        if (identity.size == .huge and last_chunk != pa >> 21) {
                            self.result.splits = try std.math.add(usize, self.result.splits, 1);
                            last_chunk = pa >> 21;
                        }
                    };
                    break :blk n;
                },
            };
            self.result.leaves = try std.math.add(usize, self.result.leaves, count);
        }
    };
    var counter: Counter = .{ .pages = pages };
    try visitSources(pages, &counter, Counter.visit);
    return counter.result;
}

pub fn collect(pages: []const u64, storage: []paging.AliasSource) ![]paging.AliasSource {
    const Collector = struct {
        storage: []paging.AliasSource,
        used: usize = 0,
        fn visit(self: *@This(), source: paging.AliasSource) !void {
            if (self.used == self.storage.len) return error.NoSpace;
            self.storage[self.used] = source;
            self.used += 1;
        }
    };
    var collector: Collector = .{ .storage = storage };
    try visitSources(pages, &collector, Collector.visit);
    return storage[0..collector.used];
}

pub const Split = union(enum) {
    kernel: vm.RamCacheKernelSplit,

    fn mapping(self: *const Split) *const paging.IdentitySplit {
        return switch (self.*) {
            inline else => |*value| &value.split,
        };
    }

    fn publish(self: *Split) void {
        switch (self.*) {
            inline else => |*value| value.publish(),
        }
    }

    fn release(self: *Split) void {
        switch (self.*) {
            inline else => |*value| value.releaseReservation(),
        }
    }
};

/// An all-or-nothing native PTE plan. This is not an authority check or a
/// standalone cache transition: the writer also validates VMO/VMA/DMA/PAT,
/// publishes pending policy, retires copy windows, and performs both all-CPU
/// TLB barriers and cache drain around withdraw/publish.
pub const Plan = struct {
    leaves: []paging.Leaf,
    splits: []Split,
    state: enum { prepared, withdrawn, published, released } = .prepared,

    /// A previously unknown conflicting alias must not be silently retagged.
    /// Policy ownership and PAT/MTRR proof belong to the writer, but every
    /// saved leaf must agree with the old physical policy before withdrawal.
    pub fn validatePolicy(self: *const Plan, snapshot: *const @import("ram_cache_policy.zig").Snapshot) !void {
        std.debug.assert(self.state == .prepared);
        for (self.splits) |*split| {
            const source = split.mapping();
            if ((@atomicLoad(u64, source.directory, .acquire) ^ source.previous) & ~@as(u64, 0x60) != 0)
                return error.StaleAlias;
        }
        for (self.leaves) |leaf| {
            const old_pat: u3 = @intCast(((leaf.previous >> 3) & 3) | ((leaf.previous >> 5) & 4));
            const expected: u3 = switch (snapshot.query(leaf.previous & 0x000f_ffff_ffff_f000)) {
                .blocked => return error.PendingTransition,
                .ordinary => 0,
                .cached => |span| span.pat_slot,
            };
            if (old_pat != expected) return error.CacheAliasConflict;
        }
    }

    pub fn withdraw(self: *Plan) void {
        std.debug.assert(self.state == .prepared);
        for (self.splits) |*split| split.publish();
        for (self.leaves) |*leaf| leaf.withdraw();
        self.state = .withdrawn;
    }

    pub fn validateStorage(self: *const Plan, pages: []const u64) !void {
        for (self.splits) |*split| {
            if (paging.pagesIntersect(pages, split.mapping().table_paddr, 4096))
                return error.MetadataOverlap;
        }
    }

    pub fn publish(self: *Plan, pat: u3) void {
        std.debug.assert(self.state == .withdrawn);
        for (self.leaves) |*leaf| leaf.publish(pat);
        self.state = .published;
    }

    pub fn release(self: *Plan) void {
        std.debug.assert(self.state != .withdrawn);
        if (self.state == .released) return;
        for (self.splits) |*split| split.release();
        self.state = .released;
    }
};

fn sourceAddress(source: paging.AliasSource) usize {
    return switch (source) {
        .leaf => |pointer| @intFromPtr(pointer),
        .identity => |identity| @intFromPtr(identity.directory),
    };
}

fn sourceLessThan(_: void, left: paging.AliasSource, right: paging.AliasSource) bool {
    return sourceAddress(left) < sourceAddress(right);
}

/// The collected sources are scratch owned by the writer. A source pointer
/// denotes exactly one live page-table entry; two interpretations of the same
/// entry are not safe to silently merge. Deduplication here makes the later
/// leaf capture linear even with a private identity copy in every process.
fn uniqueSources(sources: []paging.AliasSource) ![]paging.AliasSource {
    // Alias order has no stable-order contract. Heap sort needs constant
    // workspace, unlike mem.sort's element-size-dependent stack scratch.
    // Equal addresses still reach the ambiguity check below before capture.
    std.sort.heap(paging.AliasSource, sources, {}, sourceLessThan);
    var used: usize = 0;
    for (sources) |source| {
        if (used != 0 and sourceAddress(sources[used - 1]) == sourceAddress(source)) {
            if (!std.meta.eql(sources[used - 1], source)) return error.AmbiguousAlias;
            continue;
        }
        sources[used] = source;
        used += 1;
    }
    return sources[0..used];
}

fn capture(leaves: []paging.Leaf, used: *usize, pointer: *u64, pages: []const u64) !void {
    if (used.* == leaves.len) return error.NoSpace;
    const leaf = try paging.Leaf.capture(pointer);
    if (!paging.pagesIntersect(pages, leaf.previous & 0x000f_ffff_ffff_f000, 4096)) return error.Invalid;
    leaves[used.*] = leaf;
    used.* += 1;
}

pub fn prepare(pages: []const u64, sources: []paging.AliasSource, leaf_storage: []paging.Leaf, split_storage: []Split) !Plan {
    try paging.validatePages(pages, @as(u64, 1) << 52);
    const unique = try uniqueSources(sources);
    var leaf_count: usize = 0;
    var split_count: usize = 0;
    errdefer for (split_storage[0..split_count]) |*split| split.release();
    for (unique) |source| switch (source) {
        .leaf => |pointer| try capture(leaf_storage, &leaf_count, pointer, pages),
        .identity => |identity| {
            if (split_count == split_storage.len) return error.NoSpace;
            split_storage[split_count] = .{ .kernel = try vm.reserveRamCacheKernelSplit(source) };
            const split = split_storage[split_count].mapping();
            split_count += 1;
            var last_chunk: ?u64 = null;
            var leaf_table = split.table;
            for (pages) |pa| {
                if (pa < identity.base or pa - identity.base >= identity.size.bytes()) continue;
                if (identity.size == .huge and last_chunk != pa >> 21) {
                    if (split_count == split_storage.len) return error.NoSpace;
                    const chunk = pa & ~@as(u64, (1 << 21) - 1);
                    const va = identity.alias_va orelse return error.Invalid;
                    split_storage[split_count] = .{ .kernel = try vm.reserveRamCacheKernelSplit(.{ .identity = .{
                        .directory = &split.table[@intCast((chunk - identity.base) >> 21)],
                        .base = chunk,
                        .alias_va = va + (chunk - identity.base),
                    } }) };
                    leaf_table = split_storage[split_count].mapping().table;
                    split_count += 1;
                    last_chunk = pa >> 21;
                }
                try capture(leaf_storage, &leaf_count, &leaf_table[@intCast((pa >> 12) & 511)], pages);
            }
        },
    };
    return .{ .leaves = leaf_storage[0..leaf_count], .splits = split_storage[0..split_count] };
}

test "RAM cache source normalization rejects conflicting meanings of one entry" {
    var first: u64 = 0;
    var second: u64 = 0;
    const first_identity: paging.AliasSource = .{ .identity = .{ .directory = &first, .base = 0x200000 } };
    const second_identity: paging.AliasSource = .{ .identity = .{ .directory = &second, .base = 0x400000 } };
    var repeated = [_]paging.AliasSource{ second_identity, first_identity, first_identity, second_identity };
    const normalized = try uniqueSources(&repeated);
    try std.testing.expectEqual(@as(usize, 2), normalized.len);
    try std.testing.expect(sourceAddress(normalized[0]) < sourceAddress(normalized[1]));

    var mismatched_identity = [_]paging.AliasSource{
        first_identity,
        .{ .identity = .{ .directory = &first, .base = 0x400000 } },
    };
    try std.testing.expectError(error.AmbiguousAlias, uniqueSources(&mismatched_identity));
    var mixed = [_]paging.AliasSource{ first_identity, .{ .leaf = &first } };
    try std.testing.expectError(error.AmbiguousAlias, uniqueSources(&mixed));
}

test "RAM cache preparation deduplicates 512 pages across 24 alias sets" {
    const allocator = std.testing.allocator;
    const count = 512;
    const alias_sets = 24;
    const alias_count = count * alias_sets;
    const pages = try allocator.alloc(u64, count);
    defer allocator.free(pages);
    const entries = try allocator.alloc(u64, alias_count);
    defer allocator.free(entries);
    const sources = try allocator.alloc(paging.AliasSource, alias_count * 2);
    defer allocator.free(sources);
    const leaves = try allocator.alloc(paging.Leaf, alias_count);
    defer allocator.free(leaves);
    for (0..count) |index| {
        pages[index] = 0x200000 + @as(u64, @intCast(index)) * 4096;
    }
    for (0..alias_count) |index| {
        const pa = pages[index % count];
        entries[index] = pa | 0x7;
        sources[index * 2] = .{ .leaf = &entries[alias_count - 1 - index] };
        sources[index * 2 + 1] = .{ .leaf = &entries[alias_count - 1 - index] };
    }
    var splits: [0]Split = .{};
    var plan = try prepare(pages, sources, leaves, &splits);
    defer plan.release();
    try std.testing.expectEqual(@as(usize, alias_count), plan.leaves.len);
    try std.testing.expectEqual(@as(usize, 0), plan.splits.len);
    for (plan.leaves) |leaf| {
        try std.testing.expect((leaf.previous & 0x000f_ffff_ffff_f000) >= pages[0]);
        try std.testing.expect((leaf.previous & 0x000f_ffff_ffff_f000) <= pages[count - 1]);
    }
    entries[0] = 0;
    try std.testing.expectError(error.Unsupported, prepare(pages, sources, leaves, &splits));
}

test "RAM cache source heap ordering handles empty sorted reversed and all equal aliases" {
    var entries: [513]u64 = @splat(0);
    var sources: [513]paging.AliasSource = undefined;
    try std.testing.expectEqual(@as(usize, 0), (try uniqueSources(sources[0..0])).len);
    for ([_]bool{ false, true }) |reverse| {
        for (&sources, 0..) |*source, index| source.* = .{ .leaf = &entries[if (reverse) sources.len - 1 - index else index] };
        const normalized = try uniqueSources(&sources);
        try std.testing.expectEqual(sources.len, normalized.len);
        for (normalized, 0..) |source, index| try std.testing.expectEqual(@intFromPtr(&entries[index]), sourceAddress(source));
    }
    for (&sources) |*source| source.* = .{ .leaf = &entries[0] };
    try std.testing.expectEqual(@as(usize, 1), (try uniqueSources(&sources)).len);
    // A late conflicting interpretation must not disappear amid duplicates.
    sources[sources.len - 1] = .{ .identity = .{ .directory = &entries[0], .base = 0x200000 } };
    try std.testing.expectError(error.AmbiguousAlias, uniqueSources(&sources));
}
