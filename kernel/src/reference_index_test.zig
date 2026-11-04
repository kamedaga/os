const std = @import("std");
const refs = @import("memory/reference_index.zig");
const storage = @import("memory/owned_storage.zig");
test {
    _ = refs;
}

test "reference reservation every allocator OOM and rejected return" {
    var successful_calls: usize = 0;
    {
        var backend: storage.TestAllocator = .{};
        defer backend.deinit();
        var pool: refs.Pool = .{};
        const node = try pool.reserve(backend.interface());
        successful_calls = backend.calls;
        refs.Pool.cancel(node);
        pool.deinit();
        try std.testing.expectEqual(@as(usize, 0), backend.active());
    }
    for (1..successful_calls + 1) |position| {
        var backend: storage.TestAllocator = .{ .fail_at = position };
        defer backend.deinit();
        var pool: refs.Pool = .{};
        try std.testing.expectError(error.OutOfMemory, pool.reserve(backend.interface()));
        try std.testing.expectEqual(@as(usize, 0), pool.nodes.live);
        pool.deinit();
        try std.testing.expectEqual(@as(usize, 0), backend.active());
    }
    var backend: storage.TestAllocator = .{};
    defer backend.deinit();
    var pool: refs.Pool = .{};
    const first = try pool.reserve(backend.interface());
    const old = first.allocation;
    refs.Pool.cancel(first);
    const second = try pool.reserve(backend.interface());
    try std.testing.expect(pool.nodes.resolve(old) == null);
    refs.Pool.cancel(second);
    backend.fail_return = true;
    pool.reclaim();
    try std.testing.expect(pool.needs_reclaim);
    backend.fail_return = false;
    pool.reclaim();
    try std.testing.expect(!pool.needs_reclaim);
    pool.deinit();
    try std.testing.expectEqual(@as(usize, 0), backend.active());
    std.debug.print("reference reservation OOM positions={} slot-reuse and rejected-return PASS\n", .{successful_calls});
}

fn pinRandom(state: *u64) u64 {
    state.* = state.* *% 6364136223846793005 +% 1;
    return state.* >> 16;
}
test "pin index oracle random policies updates generation overflow and zero" {
    const nodes = try std.testing.allocator.alloc(refs.Node, 128);
    defer std.testing.allocator.free(nodes);
    @memset(nodes, .{});
    var root: refs.Root = .{};
    var index: refs.PinIndex = .{};
    var random: u64 = 0x551177;
    for (0..10000) |iteration| {
        const slot: usize = @intCast(pinRandom(&random) % nodes.len);
        const edge = &nodes[slot];
        if (edge.pin.linked) {
            index.remove(edge);
            refs.Root.disconnect(edge, false);
        }
        if (pinRandom(&random) % 5 != 0) {
            const class: refs.PinClass = @enumFromInt(pinRandom(&random) % 4);
            const generation = edge.identity.generation +% 1;
            const start = if (iteration % 101 == 0) std.math.maxInt(u64) - 8 else pinRandom(&random) % 16384;
            root.connect(edge, .{ .kind = .pinned_object, .container = @as(u16, @intFromEnum(class)) + 1, .index = @intCast(slot), .generation = generation, .owner_generation = 1 }, .{ .start = start, .size = pinRandom(&random) % 512 }, false);
            index.insert(edge, class);
        }
        const start = if (iteration % 103 == 0) std.math.maxInt(u64) - 2 else pinRandom(&random) % 16384;
        const size = pinRandom(&random) % 512;
        const mask: u4 = @truncate(pinRandom(&random));
        const except: ?refs.Identity = if (iteration % 2 == 0) .{ .container = nodes[slot].identity.container, .index = @intCast(slot), .generation = nodes[slot].identity.generation + (if (iteration % 4 == 0) @as(u32, 0) else 1) } else null;
        var expected = false;
        for (nodes) |*candidate| if (candidate.pin.linked) {
            const equal = if (except) |e| e.container == candidate.identity.container and e.index == candidate.identity.index and e.generation == candidate.identity.generation else false;
            if (mask & (@as(u4, 1) << @intFromEnum(candidate.pin.class)) != 0 and !equal and refs.overlap(start, size, candidate.range.start, candidate.range.size)) expected = true;
        };
        try std.testing.expectEqual(expected, index.anyOverlap(start, size, mask, except));
        try index.validate();
    }
    for (nodes) |*edge| if (edge.pin.linked) {
        index.remove(edge);
        refs.Root.disconnect(edge, false);
    };
    try std.testing.expectEqual(@as(u32, 0), root.count);
}
test "pin index dense aliases logarithmic queries and negative augmentation" {
    const nodes = try std.testing.allocator.alloc(refs.Node, 4000);
    defer std.testing.allocator.free(nodes);
    @memset(nodes, .{});
    var root: refs.Root = .{};
    var index: refs.PinIndex = .{};
    for (nodes, 0..) |*edge, i| {
        root.connect(edge, .{ .kind = .pinned_object, .index = @intCast(i), .generation = 1 }, .{ .start = 4096, .size = 4096 }, false);
        index.insert(edge, .linear);
    }
    refs.pin_query_visits = 0;
    try std.testing.expect(!index.anyOverlap(4096, 4096, 3, null));
    try std.testing.expectEqual(@as(usize, 0), refs.pin_query_visits);
    refs.pin_query_visits = 0;
    try std.testing.expect(index.anyOverlap(4096, 4096, 15, null));
    try std.testing.expect(refs.pin_query_visits < 64);
    refs.pin_query_visits = 0;
    try std.testing.expect(!index.anyOverlap(0, 1, 15, null));
    try std.testing.expect(refs.pin_query_visits < 64);
    const top = index.trees[2].root.?;
    const saved = top.pin.max_end;
    top.pin.max_end = 0;
    try std.testing.expectError(error.PinAugmentation, index.validate());
    top.pin.max_end = saved;
    const saved_class = top.pin.class;
    top.pin.class = .mmio;
    try std.testing.expectError(error.PinClass, index.validate());
    top.pin.class = saved_class;
    for (nodes) |*edge| {
        index.remove(edge);
        refs.Root.disconnect(edge, false);
    }
}

test "pin index omitted unlink cannot pass retirement" {
    var root: refs.Root = .{};
    var index: refs.PinIndex = .{};
    var edge: refs.Node = .{};
    root.connect(&edge, .{ .kind = .pinned_object }, .{ .start = 4096, .size = 4096 }, false);
    index.insert(&edge, .buffer);
    refs.Root.disconnect(&edge, false);
    try std.testing.expectError(error.PinRetirement, index.requireEmpty(root.count));
    index.remove(&edge);
    try index.requireEmpty(root.count);
}
