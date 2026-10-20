//! Process-owned stable records. K protects all loans and tree mutations;
//! descriptor snapshots carry only a generation-checked slot reference.
const std = @import("std");
const slots = @import("../memory/slot_table.zig");
const deadlines = @import("../memory/deadline_index.zig");
const types = @import("types.zig");
pub const State = struct { signo: u32 = 0, remaining_ticks: u64 = 0, interval_ticks: u64 = 0 };
pub const Expired = struct { principal: types.PrincipalId, generation: u32, signo: u32 };
pub const Record = struct {
    node: deadlines.Node = .{},
    principal: types.PrincipalId = @enumFromInt(0),
    generation: u32 = 0,
    signo: u32 = 0,
    interval: u64 = 0,
};
pub const Store = struct {
    records: slots.SlotTable(Record) = .{},
    index: deadlines.Index = .{},

    pub fn reserve(self: *Store, allocator: @import("../memory/owned_storage.zig").Allocator, principal: types.PrincipalId, generation: u32) !slots.Ref {
        const allocation = try self.records.allocate(allocator, null, std.math.maxInt(u32));
        allocation.value.* = .{ .principal = principal, .generation = generation };
        return allocation.ref;
    }
    pub fn release(self: *Store, ref: slots.Ref) void {
        const record = self.records.resolve(ref) orelse unreachable;
        self.cancel(record);
        self.records.release(ref) catch unreachable;
        // K excludes every timer borrower; no IRQ retains node pointers.
        self.records.reclaimEmptyBodies();
    }
    pub fn cancel(self: *Store, record: *Record) void {
        if (record.node.linked) self.index.remove(&record.node);
        record.signo = 0;
        record.interval = 0;
    }
    pub fn read(record: *const Record, now: u64) State {
        if (!record.node.linked) return .{};
        return .{ .signo = record.signo, .remaining_ticks = record.node.key.deadline -| now, .interval_ticks = record.interval };
    }
    pub fn set(self: *Store, record: *Record, signo: u32, initial: u64, interval: u64, now: u64) State {
        const old = read(record, now);
        self.cancel(record);
        if (initial != 0) {
            record.signo = signo;
            record.interval = interval;
            self.index.insert(&record.node, .{ .deadline = now +| initial, .id = @intFromEnum(record.principal), .generation = record.generation });
        }
        return old;
    }
    pub fn take(self: *Store, now: u64) ?Expired {
        const node = self.index.first orelse return null;
        if (node.key.deadline > now) return null;
        const record: *Record = @fieldParentPtr("node", node);
        const expired: Expired = .{ .principal = record.principal, .generation = record.generation, .signo = record.signo };
        const deadline = node.key.deadline;
        self.index.remove(node);
        if (record.interval != 0 and now != std.math.maxInt(u64)) {
            // u128 covers (elapsed / interval + 1) * interval, including the
            // u64 endpoint. At max tick there is no representable future:
            // disarm instead of delivering forever in this same IRQ pass.
            const periods = @as(u128, now - deadline) / record.interval + 1;
            const next = @min(@as(u128, deadline) + periods * record.interval, std.math.maxInt(u64));
            self.index.insert(node, .{ .deadline = @intCast(next), .id = @intFromEnum(record.principal), .generation = record.generation });
        } else {
            record.signo = 0;
            record.interval = 0;
        }
        return expired;
    }
};

fn recordFor(state: anytype, principal: types.PrincipalId, generation: u32) ?*Record {
    const descriptor = state.processDescriptor(principal) orelse return null;
    if (descriptor.generation != generation) return null;
    return state.process_timers.records.resolve(descriptor.signal_timer orelse return null);
}
pub fn get(state: anytype, principal: types.PrincipalId, generation: u32, now: u64) State {
    return Store.read(recordFor(state, principal, generation) orelse return .{}, now);
}
pub fn set(state: anytype, principal: types.PrincipalId, generation: u32, signo: u32, initial: u64, interval: u64, now: u64) State {
    return state.process_timers.set(recordFor(state, principal, generation) orelse return .{}, signo, initial, interval, now);
}
pub fn clear(state: anytype, principal: types.PrincipalId, generation: u32) void {
    state.process_timers.cancel(recordFor(state, principal, generation) orelse return);
}

test "process timer index simultaneous expiry cancellation reset and independent n k work" {
    for ([_]usize{ 128, 1024, 8192 }) |n| {
        for ([_]usize{ 0, 1, 64, 128 }) |k| {
            var store: Store = .{};
            defer store.records.deinit();
            const refs = try std.testing.allocator.alloc(slots.Ref, n);
            defer std.testing.allocator.free(refs);
            for (refs, 0..) |*ref, i| {
                ref.* = try store.reserve(slots.hostTestAllocator(), @enumFromInt(i + 1), 7);
                _ = store.set(store.records.resolve(ref.*).?, 4, if (i < k) 10 else 1000, 0, 0);
            }
            try store.index.validate();
            store.index.comparisons = 0;
            store.index.tree_steps = 0;
            var visited: usize = 0;
            while (store.take(10)) |expired| {
                visited += 1;
                try std.testing.expectEqual(visited, @as(usize, @intFromEnum(expired.principal)));
                try std.testing.expectEqual(@as(u32, 7), expired.generation);
            }
            try std.testing.expectEqual(k, visited);
            std.debug.print("process-timer n={} k={} visits={} comparisons={} tree-steps={} IRQ-K=1 timer-allocation=0\n", .{ n, k, visited, store.index.comparisons, store.index.tree_steps });
            try std.testing.expect(store.index.comparisons <= (k + 1) * 64);
            try std.testing.expect(store.index.tree_steps <= k * 64);
            for (refs) |ref| {
                const record = store.records.resolve(ref).?;
                _ = store.set(record, 5, 20, 0, 10);
                _ = store.set(record, 6, 50, 0, 10);
                store.cancel(record);
            }
            try std.testing.expect(store.take(std.math.maxInt(u64)) == null);
            try std.testing.expectEqual(@as(usize, 0), store.index.count);
            try store.index.validate();
            for (refs) |ref| store.release(ref);
            try std.testing.expectEqual(@as(usize, 0), store.records.live);
        }
    }
}

test "process timer index missed periods overflow and endpoint disarm" {
    var store: Store = .{};
    defer store.records.deinit();
    const ref = try store.reserve(slots.hostTestAllocator(), @enumFromInt(1), 1);
    const record = store.records.resolve(ref).?;
    _ = store.set(record, 4, 10, 7, 0);
    try std.testing.expect(store.take(100) != null);
    try std.testing.expectEqual(@as(u64, 1), Store.read(record, 100).remaining_ticks);
    try std.testing.expect(store.take(100) == null);
    _ = store.set(record, 4, 1, std.math.maxInt(u64), 0);
    try std.testing.expect(store.take(2) != null);
    try std.testing.expectEqual(std.math.maxInt(u64), record.node.key.deadline);
    try std.testing.expect(store.take(std.math.maxInt(u64)) != null);
    try std.testing.expect(store.take(std.math.maxInt(u64)) == null);
    _ = store.set(record, 4, 1, 1, 0);
    try std.testing.expect(store.take(std.math.maxInt(u64)) != null);
    try std.testing.expect(store.take(std.math.maxInt(u64)) == null);
    store.release(ref);
    try std.testing.expectEqual(@as(usize, 0), store.index.count);
    const next = try store.reserve(slots.hostTestAllocator(), @enumFromInt(1), 2);
    try std.testing.expect(store.records.resolve(ref) == null);
    try std.testing.expectEqual(@as(u32, 2), store.records.resolve(next).?.generation);
    store.release(next);
}

test "process timer index reservation rollback before publication" {
    const Backend = struct {
        calls: usize = 0,
        live: usize = 0,
        fail: usize,
        fn acquire(ctx: *anyopaque, pages: usize) ?@import("../memory/owned_storage.zig").MappedExtent {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            self.calls += 1;
            if (self.calls == self.fail) return null;
            const allocator = slots.hostTestAllocator();
            const grant = allocator.acquire(allocator.context, pages) orelse return null;
            self.live += 1;
            return grant;
        }
        fn release(ctx: *anyopaque, grant: @import("../memory/owned_storage.zig").PhysicalExtent) bool {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            const allocator = slots.hostTestAllocator();
            if (!allocator.release(allocator.context, grant)) return false;
            self.live -= 1;
            return true;
        }
    };
    for (1..10) |fail| {
        var backend: Backend = .{ .fail = fail };
        var store: Store = .{};
        const result = store.reserve(.{ .context = &backend, .acquire = Backend.acquire, .release = Backend.release }, @enumFromInt(1), 1);
        if (result) |ref| store.release(ref) else |_| {
            try std.testing.expectEqual(@as(usize, 0), store.records.live);
            try std.testing.expectEqual(@as(usize, 0), store.index.count);
            try std.testing.expectEqual(@as(usize, 0), backend.live);
        }
        store.records.deinit();
        try std.testing.expectEqual(@as(usize, 0), backend.live);
    }
}
