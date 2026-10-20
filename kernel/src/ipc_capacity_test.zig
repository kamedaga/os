const std = @import("std");
const kernel = @import("kernel.zig");
const KernelState = kernel.KernelState;
// The VM failed in the channel Body grant, after PMM drained every CPU cache.
// Exercise the real table with a finite page budget, not a fixed FD limit.
test "channel FD4200 fits finite backing without four page bodies" {
    try std.testing.expect(@import("sync/lock_diagnostics.zig").enabled);
    const Allocator = @typeInfo(@FieldType(KernelState, "pool_allocator")).optional.child;
    const Mapped = @typeInfo(@typeInfo(@typeInfo(@FieldType(Allocator, "acquire")).pointer.child).@"fn".return_type.?).optional.child;
    const Extent = @typeInfo(@typeInfo(@FieldType(Allocator, "release")).pointer.child).@"fn".params[1].type.?;
    const Backend = struct {
        pages: usize = 0,
        peak: usize = 0,
        largest: usize = 0,
        failed: usize = 0,
        fn acquire(ctx: *anyopaque, pages: usize) ?Mapped {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            if (pages > 21204 -| self.pages) {
                self.failed = pages;
                return null;
            }
            const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), pages * 4096) catch return null;
            self.pages += pages;
            self.peak = @max(self.peak, self.pages);
            self.largest = @max(self.largest, pages);
            return .{ .allocation = .{ .physical = .fromInt(@intFromPtr(bytes.ptr)), .pages = pages, .region_id = 1 }, .bytes = bytes };
        }
        fn release(ctx: *anyopaque, grant: Extent) bool {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            const ptr: [*]align(4096) u8 = @ptrFromInt(grant.physical.toInt());
            std.testing.allocator.free(ptr[0 .. grant.pages * 4096]);
            self.pages -= grant.pages;
            return true;
        }
    };
    var backend = Backend{};
    const state = try std.testing.allocator.create(KernelState);
    defer std.testing.allocator.destroy(state);
    state.* = .{};
    defer state.deinitPools();
    const allocator: Allocator = .{ .context = &backend, .acquire = Backend.acquire, .release = Backend.release };
    state.pool_allocator = allocator;
    var opened: usize = 0;
    while (opened < 4200) : (opened += 1) {
        _ = state.createIpcChannel() catch |err| {
            std.debug.print("CHANNEL_PRESSURE opened={d} pages={d} failed_grant={d}\n", .{ opened, backend.pages, backend.failed });
            return err;
        };
    }
    try std.testing.expectEqual(@as(usize, 1), backend.largest);
    std.debug.print("CHANNEL_PRESSURE opened={d} peak_pages={d} largest={d}\n", .{ opened, backend.peak, backend.largest });
}

test "channel FD4200 compact ownership preserves full reference identity" {
    const refs = @import("memory/reference_index.zig");
    var lock = @import("sync/ipc_endpoint.zig").Lock{};
    var pages: [2]@import("state/types.zig").IpcMessagePage = @splat(@splat(.{}));
    var queue = kernel.IpcQueue{
        .pages = .{ &pages[0], &pages[1] },
        .exclusion = &lock,
        .reference_identity = .{ .owner = 123, .owner_generation = 456, .container = 2, .direction = 1 },
        .reference_sequence = 99,
        .head = 7,
    };
    var target = refs.Root{ .temporary = 1 };
    var node = refs.Node{ .prepared_target = &target, .identity = .{ .position = 3 } };
    var guard = lock.acquire();
    defer guard.end();
    try queue.push(.{ .reference_head = &node });
    try std.testing.expectEqualDeep(refs.Identity{ .kind = .ipc, .owner = 123, .owner_generation = 456, .container = 2, .direction = 1, .index = 7, .sequence = 100, .position = 3 }, node.identity);
    try std.testing.expectEqual(@as(u32, 1), target.count);
    _ = queue.pop().?;
    try std.testing.expectEqual(@as(u32, 0), target.count);
    try std.testing.expectEqual(@as(u32, 1), target.temporary);
}

// Real PMM ranges separated by allocated guard pages: aggregate capacity is
// unchanged, but no two free pages are physically adjacent. Mapping the grant
// through TestBacking preserves the production allocator's ownership contract.
test "channel FD4200 fragmented PMM reaches full capacity without contiguous grants" {
    const FragmentedBacking = struct {
        const pmm = @import("memory/free_page_list.zig");
        const storage = @import("memory/owned_storage.zig");
        const List = pmm.WithQuarantine(struct {
            fn query(_: u64, _: usize) bool {
                return false;
            }
        }.query);
        backing: pmm.TestBacking,
        free: List = .{},
        failed: usize = 0,
        largest: usize = 0,
        peak: usize = 0,
        pages: usize = 0,
        fn init() !@This() {
            return .{ .backing = try pmm.TestBacking.init(4096, 21204 * 2) };
        }
        fn seed(self: *@This()) !void {
            try self.free.installRegions(&.{.{ .id = 0, .start = 4096, .end = (21204 * 2 + 1) * 4096 }});
            for (0..21204) |i| try self.free.appendContiguousRange((i * 2 + 1) * 4096, 1);
        }
        fn allocator(self: *@This()) storage.Allocator {
            return .{ .context = self, .acquire = acquire, .release = release };
        }
        fn acquire(ctx: *anyopaque, pages: usize) ?storage.MappedExtent {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            const grant = self.free.popContiguousOwnedInRange(pages, 4096, (21204 * 2 + 1) * 4096) catch {
                self.failed = pages;
                const snapshot = self.free.availableBelow((21204 * 2 + 1) * 4096);
                std.debug.print("CHANNEL_FRAGMENT requested={d} free={d} largest={d} order0={d} higher_orders=0\n", .{ pages, snapshot.total, snapshot.largest_run, snapshot.total });
                return null;
            };
            self.largest = @max(self.largest, pages);
            self.pages += pages;
            self.peak = @max(self.peak, self.pages);
            const offset = grant.physical.toInt() - 4096;
            return .{ .allocation = grant, .bytes = @alignCast(self.backing.bytes[offset..][0 .. pages * 4096]) };
        }
        fn release(ctx: *anyopaque, grant: storage.PhysicalExtent) bool {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            self.free.appendContiguousRange(grant.physical.toInt(), grant.pages) catch unreachable;
            self.pages -= grant.pages;
            return true;
        }
    };
    var backend = try FragmentedBacking.init();
    defer backend.backing.deinit();
    try backend.seed();
    const state = try std.testing.allocator.create(KernelState);
    defer std.testing.allocator.destroy(state);
    state.* = .{ .pool_allocator = backend.allocator() };
    for (0..4200) |_| _ = try state.createIpcChannel();
    try std.testing.expectEqual(@as(usize, 0), backend.failed);
    try std.testing.expectEqual(@as(usize, 1), backend.largest);
    std.debug.print("CHANNEL_FRAGMENT_PASS opened=4200 peak_pages={d} largest_grant={d} initial_free=21204 initial_largest=1\n", .{ backend.peak, backend.largest });
    state.deinitPools();
    try std.testing.expectEqual(@as(usize, 0), backend.pages);
}

const PageBackend = struct {
    const storage = @import("memory/owned_storage.zig");
    live: usize = 0,
    calls: usize = 0,
    fail_at: usize = std.math.maxInt(usize),
    reject_returns: bool = false,
    fn allocator(self: *@This()) storage.Allocator {
        return .{ .context = self, .acquire = acquire, .release = release };
    }
    fn acquire(ctx: *anyopaque, pages: usize) ?storage.MappedExtent {
        const self: *@This() = @ptrCast(@alignCast(ctx));
        const call = self.calls;
        self.calls += 1;
        if (call == self.fail_at) return null;
        std.debug.assert(pages == 1);
        const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096) catch return null;
        self.live += 1;
        return .{ .allocation = .{ .physical = .fromInt(@intFromPtr(bytes.ptr)), .pages = 1, .region_id = 1 }, .bytes = bytes };
    }
    fn release(ctx: *anyopaque, grant: storage.PhysicalExtent) bool {
        const self: *@This() = @ptrCast(@alignCast(ctx));
        if (self.reject_returns) return false;
        const ptr: [*]align(4096) u8 = @ptrFromInt(grant.physical.toInt());
        const bytes: []align(4096) u8 = ptr[0..4096];
        std.testing.allocator.free(bytes);
        self.live -= 1;
        return true;
    }
};

test "channel FD4200 page and slot OOM rollback retains rejected returns" {
    for (0..14) |failure| {
        var backend: PageBackend = .{ .fail_at = failure, .reject_returns = true };
        const state = try std.testing.allocator.create(KernelState);
        defer std.testing.allocator.destroy(state);
        state.* = .{ .pool_allocator = backend.allocator() };
        const result = state.createIpcChannel();
        if (result) |channel| {
            try std.testing.expect(failure >= backend.calls);
            state.releaseIpcChannelHandle(.{ .channel = channel, .side = 0 });
            state.releaseIpcChannelHandle(.{ .channel = channel, .side = 1 });
        } else |err| {
            try std.testing.expectEqual(error.OutOfFreePages, err);
            try std.testing.expectEqual(@as(usize, 0), state.ipc_channels.live);
        }
        // No unrelated syscall or owner teardown is needed for rejected
        // rollback grants to become reachable from PMM pressure.
        backend.reject_returns = false;
        var registry: @import("memory/reclaim_cache.zig").Registry = .{ .fd_retired = &state.fd_retired };
        _ = registry.reclaim();
        if (result) |_| {} else |_| try std.testing.expectEqual(@as(usize, 0), backend.live);
        state.deinitPools();
        try std.testing.expectEqual(@as(usize, 0), backend.live);
    }
}

test "channel FD4200 split page wrap and final close lifetime" {
    var backend: PageBackend = .{};
    const state = try std.testing.allocator.create(KernelState);
    defer std.testing.allocator.destroy(state);
    state.* = .{ .pool_allocator = backend.allocator() };
    const channel = try state.createIpcChannel();
    const body = state.ipcChannelSlot(channel).?;
    try std.testing.expectEqual(body.queues[0].pages[1], body.queues[1].pages[0]);
    {
        var guard = body.exclusion.acquire();
        defer guard.end();
        for (0..3) |round| {
            for (0..16) |i| for (&body.queues, 0..) |*q, side| {
                try q.push(.{ .words = .{ round, side, i, 123 } });
            };
            for (&body.queues) |*q| try std.testing.expectError(error.MailboxFull, q.push(.{}));
            // Pop half and refill to force both physical-page and ring wrap.
            for (0..8) |i| for (&body.queues, 0..) |*q, side| {
                try std.testing.expectEqualDeep([4]u64{ round, side, i, 123 }, q.pop().?.words);
                try q.push(.{ .words = .{ round, side, 16 + i, 123 } });
            };
            for (8..24) |i| for (&body.queues, 0..) |*q, side| {
                try std.testing.expectEqualDeep([4]u64{ round, side, i, 123 }, q.pop().?.words);
            };
        }
    }
    const before = backend.live;
    state.releaseIpcChannelHandle(.{ .channel = channel, .side = 0 });
    try std.testing.expectEqual(before, backend.live);
    state.releaseIpcChannelHandle(.{ .channel = channel, .side = 1 });
    try std.testing.expect(state.ipcChannelSlot(channel) == null);
    state.reclaimFdCapabilities();
    state.ipc_channels.reclaim();
    try std.testing.expect(backend.live <= before - 3);
    state.deinitPools();
    try std.testing.expectEqual(@as(usize, 0), backend.live);
}

test "channel FD4200 shared queue type endpoint rollback and rejected return ownership" {
    for (0..13) |failure| {
        var backend: PageBackend = .{ .fail_at = failure, .reject_returns = true };
        const state = try std.testing.allocator.create(KernelState);
        defer std.testing.allocator.destroy(state);
        state.* = .{ .pool_allocator = backend.allocator() };
        const result = state.createIpcEndpoint();
        if (result) |endpoint| {
            const before = backend.live;
            state.clearIpcEndpointSlot(endpoint);
            try std.testing.expectEqual(before, backend.live);
            state.reclaimFdCapabilities();
        } else |err| {
            try std.testing.expectEqual(error.OutOfFreePages, err);
            try std.testing.expectEqual(@as(usize, 0), state.ipc_endpoints.live);
        }
        backend.reject_returns = false;
        var registry: @import("memory/reclaim_cache.zig").Registry = .{ .fd_retired = &state.fd_retired };
        _ = registry.reclaim();
        try std.testing.expect(state.ipc_endpoints.body_owner.pending == null);
        if (result) |_| {} else |_| try std.testing.expectEqual(@as(usize, 0), backend.live);
        state.deinitPools();
        try std.testing.expectEqual(@as(usize, 0), backend.live);
    }
}


test "channel FD4200 successful body reuse transfers rejected unused drafts to PMM" {
    var backend: PageBackend = .{};
    const state = try std.testing.allocator.create(KernelState);
    defer std.testing.allocator.destroy(state);
    state.* = .{ .pool_allocator = backend.allocator() };
    const old = try state.createIpcChannel();
    state.releaseIpcChannelHandle(.{ .channel = old, .side = 0 });
    state.releaseIpcChannelHandle(.{ .channel = old, .side = 1 });
    backend.reject_returns = true;
    const next = try state.createIpcChannel();
    try std.testing.expectEqual(old.index, next.index);
    try std.testing.expect(old.generation != next.generation);
    try std.testing.expect(state.ipc_channels.directory_owner.pending == null);
    try std.testing.expect(state.ipc_channels.metadata_owner.pending == null);
    try std.testing.expect(state.ipc_channels.body_owner.pending == null);
    const before = backend.live;
    backend.reject_returns = false;
    var registry: @import("memory/reclaim_cache.zig").Registry = .{ .fd_retired = &state.fd_retired };
    try std.testing.expect(registry.reclaim() != 0);
    try std.testing.expect(backend.live < before);
    // The live channel keeps its published pages and still has full capacity.
    const body = state.ipcChannelSlot(next).?;
    var guard = body.exclusion.acquire();
    for (0..16) |_| try body.queues[0].push(.{});
    guard.end();
    state.deinitPools();
    try std.testing.expectEqual(@as(usize, 0), backend.live);
}
