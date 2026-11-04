const std = @import("std");
const fixture = @import("memory/physical_access_test.zig");

fn readyPages(entry: fixture.Mapping) usize {
    var count: usize = 0;
    for (entry.page_states.?) |*state| {
        if (@atomicLoad(u8, state, .acquire) == 2) count += 1;
    }
    return count;
}

test "physical fixture poisons only borrowed pages and preserves partial writes" {
    var backing = try fixture.BackingSet.init(&.{.{ .first = 0x100000, .pages = 8 }});
    defer backing.deinit();
    try std.testing.expectEqual(@as(usize, 0), readyPages(backing.owned[0]));
    const first = fixture.slice(u8, 0x100007, 2).?;
    try std.testing.expectEqualSlices(u8, &.{ 0xaa, 0xaa }, first);
    first[0] = 0x35;
    try std.testing.expectEqual(@as(usize, 1), readyPages(backing.owned[0]));
    const cross = fixture.slice(u8, 0x100fff, 2).?;
    try std.testing.expectEqualSlices(u8, &.{ 0xaa, 0xaa }, cross);
    cross[1] = 0x73;
    try std.testing.expectEqual(@as(usize, 2), readyPages(backing.owned[0]));
    try std.testing.expectEqual(@as(u8, 0x35), fixture.slice(u8, 0x100007, 1).?[0]);
    try std.testing.expectEqual(@as(u8, 0x73), fixture.slice(u8, 0x101000, 1).?[0]);
    try std.testing.expectEqual(@as(usize, 2), readyPages(backing.owned[0]));
}

test "physical fixture rejects holes bounds alignment and overflow before touching pages" {
    var backing = try fixture.BackingSet.init(&.{.{ .first = 0x100000, .pages = 2 }});
    defer backing.deinit();
    try std.testing.expect(fixture.slice(u8, 0xfffff, 1) == null);
    try std.testing.expect(fixture.slice(u8, 0x102000, 1) == null);
    try std.testing.expect(fixture.slice(u8, 0x101fff, 2) == null);
    try std.testing.expect(fixture.slice(u64, 0x100001, 1) == null);
    try std.testing.expect(fixture.slice(u8, std.math.maxInt(u64), 2) == null);
    try std.testing.expect(fixture.slice(u8, 0x100000, 0) == null);
    try std.testing.expectEqual(@as(usize, 0), readyPages(backing.owned[0]));
}

test "physical fixture nested ownership restores predecessor contents" {
    var outer = try fixture.BackingSet.init(&.{.{ .first = 0x100000, .pages = 1 }});
    defer outer.deinit();
    fixture.slice(u8, 0x100000, 1).?[0] = 0x31;
    {
        var inner = try fixture.BackingSet.init(&.{.{ .first = 0x100000, .pages = 1 }});
        defer inner.deinit();
        try std.testing.expectEqual(@as(u8, 0xaa), fixture.slice(u8, 0x100000, 1).?[0]);
        fixture.slice(u8, 0x100000, 1).?[0] = 0x52;
    }
    try std.testing.expectEqual(@as(u8, 0x31), fixture.slice(u8, 0x100000, 1).?[0]);
}

test "physical fixture explicit host grants retain caller bytes and take priority" {
    var bytes: [4096]u8 align(4096) = @splat(0x27);
    const address = @intFromPtr(&bytes);
    var host = [_]fixture.Mapping{.{ .physical = address, .bytes = &bytes }};
    const saved = fixture.host_mappings;
    fixture.host_mappings = &host;
    defer fixture.host_mappings = saved;
    var backing = try fixture.BackingSet.init(&.{.{ .first = address, .pages = 1 }});
    defer backing.deinit();
    try std.testing.expectEqual(@as(u8, 0x27), fixture.slice(u8, address, 1).?[0]);
    fixture.slice(u8, address + 1, 1).?[0] = 0x58;
    try std.testing.expectEqual(@as(u8, 0x58), bytes[1]);
    try std.testing.expectEqual(@as(usize, 0), readyPages(backing.owned[0]));
}

test "physical fixture failed construction releases earlier grants and preserves mapping" {
    var outer = try fixture.BackingSet.init(&.{.{ .first = 0x100000, .pages = 1 }});
    defer outer.deinit();
    fixture.slice(u8, 0x100000, 1).?[0] = 0x39;
    try std.testing.expectError(error.OutOfMemory, fixture.BackingSet.init(&.{
        .{ .first = 0x200000, .pages = 2 },
        .{ .first = 0x300000, .pages = std.math.maxInt(usize) },
    }));
    try std.testing.expectEqual(@as(u8, 0x39), fixture.slice(u8, 0x100000, 1).?[0]);
    try std.testing.expect(fixture.slice(u8, 0x200000, 1) == null);
}

test "physical fixture concurrent first borrowers cannot overwrite each other with poison" {
    var backing = try fixture.BackingSet.init(&.{.{ .first = 0x100000, .pages = 1 }});
    defer backing.deinit();
    const Worker = struct {
        fn run(go: *std.atomic.Value(bool), index: usize) void {
            while (!go.load(.acquire)) std.atomic.spinLoopHint();
            const bytes = fixture.slice(u8, 0x100000, 4096).?;
            bytes[index] = @intCast(index + 1);
        }
    };
    // Start each round on an untouched page so this exercises publication,
    // rather than only concurrent lookups of an already initialized page.
    for (0..32) |_| {
        @atomicStore(u8, &backing.owned[0].page_states.?[0], 0, .release);
        var go: std.atomic.Value(bool) = .init(false);
        var threads: [8]std.Thread = undefined;
        var spawned: usize = 0;
        errdefer {
            go.store(true, .release);
            for (threads[0..spawned]) |thread| thread.join();
        }
        for (&threads, 0..) |*thread, index| {
            thread.* = try std.Thread.spawn(.{}, Worker.run, .{ &go, index });
            spawned += 1;
        }
        go.store(true, .release);
        for (threads) |thread| thread.join();
        const bytes = fixture.slice(u8, 0x100000, 4096).?;
        for (0..8) |index| try std.testing.expectEqual(@as(u8, @intCast(index + 1)), bytes[index]);
        try std.testing.expectEqual(@as(usize, 1), readyPages(backing.owned[0]));
    }
}
