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
