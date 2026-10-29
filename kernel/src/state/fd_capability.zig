//! Canonical FD entries and K-owned IPC borrows. Deferred storage return
//! remains owned by the table retirement queues; no runtime FD reader loan.
const std = @import("std");
const types = @import("types.zig");

pub const DirectIpc = struct {
    queue: *types.IpcQueue,
    list: *types.IpcWaitList,
    registration: types.FdWaitRegistrationRef,
};

pub const FullIpc = struct {
    queue: *types.IpcQueue,
    list: *types.IpcWaitList,
    opposite: ?*types.IpcWaitList,
    registration: types.FdWaitRegistrationRef,
    hint: *const types.IpcReceiverHint,
    payload: types.KernelObjectPayload,
};

/// K must cover this lookup and the entire returned borrow. Publication owns
/// the object/body reference; E still protects queue state from cancellation.
pub fn directIpcUnderK(self: anytype, owner: types.PrincipalId, fd: types.Fd, call: bool) ?DirectIpc {
    return resolveIpcUnderK(false, self, owner, fd, if (call) .{ .call = true } else .{ .recv = true, .wait = true }, call);
}

pub fn ipcUnderK(self: anytype, owner: types.PrincipalId, fd: types.Fd, rights: types.FdRights, outbound: bool) ?FullIpc {
    return resolveIpcUnderK(true, self, owner, fd, rights, outbound);
}

/// Both paths share authority validation. The dedicated path needs neither a
/// payload union nor a receiver hint; constructing those would lengthen its
/// hot return. K owns all pointers until the last use, before callbacks/drop K.
fn resolveIpcUnderK(comptime full: bool, self: anytype, owner: types.PrincipalId, fd: types.Fd, rights: types.FdRights, outbound: bool) ?(if (full) FullIpc else DirectIpc) {
    const index = types.processIndexFromPrincipal(owner) orelse return null;
    const table = self.fdTableForProcessIndex(index) orelse return null;
    if (fd >= table.capacity) return null;
    const entry = table.entries.get(fd) orelse return null;
    if (!types.isFdRightsSubset(rights, entry.rights)) return null;
    var result: if (full) FullIpc else DirectIpc = undefined;
    switch (entry.object.kind) {
        .endpoint => {
            const queue = entry.cursor.ipc_queue orelse return null;
            const body: *types.IpcEndpointSlot = @fieldParentPtr("queue", queue);
            if (entry.authority.ipc_generation != body.generation or !body.active or !body.live.isOpen()) return null;
            result.queue = queue;
            result.list = &body.waiters;
            result.registration = .{ .kind = .endpoint, .index = queue.reference_identity.owner, .generation = body.generation };
            if (full) {
                result.opposite = null;
                result.hint = &body.receiver_hint;
                result.payload = .{ .endpoint = .{ .index = queue.reference_identity.owner, .generation = body.generation } };
            }
        },
        .channel => {
            const local = entry.cursor.ipc_queue orelse return null;
            const side = local.reference_identity.direction;
            if (side > 1 or local.reference_identity.container != 2) return null;
            const queues: *[2]types.IpcQueue = @ptrFromInt(@intFromPtr(local) - @as(usize, side) * @sizeOf(types.IpcQueue));
            const body: *types.IpcChannelSlot = @fieldParentPtr("queues", queues);
            if (entry.authority.ipc_generation != body.generation or !body.active or !body.live.isOpen() or body.ref_count != 2) return null;
            const destination = side ^ @as(u8, if (outbound) 1 else 0);
            result.queue = &body.queues[destination];
            result.list = &body.waiters[destination];
            result.registration = .{ .kind = .channel, .index = local.reference_identity.owner, .generation = body.generation, .side = @intCast(destination) };
            if (full) {
                // Both directions share E. Lend the already validated peer
                // list under the same K interval instead of resolving again.
                result.opposite = &body.waiters[1 - destination];
                result.hint = &body.receiver_hints[destination];
                result.payload = .{ .channel = .{ .channel = .{ .index = local.reference_identity.owner, .generation = body.generation }, .side = @intCast(side) } };
            }
        },
        else => return null,
    }
    return result;
}

pub const DirectNotify = struct {
    channel: *types.IpcChannelSlot,
    side: u8,
};

/// K retains the canonical entry and body until the caller releases its E
/// guard. Notify accepts channels only; closed channels go to the old path
/// so pending-before-close consumption keeps its existing ABI semantics.
pub fn notifyUnderK(self: anytype, owner: types.PrincipalId, fd: types.Fd, rights: types.FdRights) ?DirectNotify {
    const index = types.processIndexFromPrincipal(owner) orelse return null;
    const table = self.fdTableForProcessIndex(index) orelse return null;
    if (fd >= table.capacity) return null;
    const entry = table.entries.get(fd) orelse return null;
    if (entry.object.kind != .channel or !types.isFdRightsSubset(rights, entry.rights)) return null;
    const local = entry.cursor.ipc_queue orelse return null;
    const side = local.reference_identity.direction;
    if (side > 1 or local.reference_identity.container != 2) return null;
    const queues: *[2]types.IpcQueue = @ptrFromInt(@intFromPtr(local) - @as(usize, side) * @sizeOf(types.IpcQueue));
    const body: *types.IpcChannelSlot = @fieldParentPtr("queues", queues);
    if (entry.authority.ipc_generation != body.generation or !body.active or !body.live.isOpen() or body.ref_count != 2) return null;
    return .{ .channel = body, .side = side };
}

fn pending(table: anytype) bool {
    inline for (.{ &table.directory_owner, &table.metadata_owner, &table.body_owner }) |owner|
        if (owner.retired != null or owner.pending != null) return true;
    return false;
}

pub fn finishGrowth(self: anytype, table: *types.FdTable) void {
    if (pending(&table.entries) and !table.retirement_queued) {
        table.retirement_queued = true;
        table.retired_next = self.retired_fd_tables;
        self.retired_fd_tables = table;
    }
    reclaim(self);
}

pub fn retireTable(self: anytype, table: *types.FdTable) void {
    // Only whole-process teardown changes incarnation. Ordinary FD mutations
    // change only that entry's seq; directory growth invalidates neither.
    const flags = self.object_directory_leaf.lock();
    const epoch = @atomicLoad(u64, &table.incarnation, .seq_cst);
    @atomicStore(u64, &table.incarnation, std.math.add(u64, epoch, 1) catch std.math.maxInt(u64), .seq_cst);
    @atomicStore(usize, &table.capacity, 0, .release);
    table.entries.retire();
    self.object_directory_leaf.unlock(flags);
    if (!table.retirement_queued) {
        table.retirement_queued = true;
        table.retired_next = self.retired_fd_tables;
        self.retired_fd_tables = table;
    }
    reclaim(self);
}

pub fn reclaim(self: anytype) void {
    inline for (.{ "fd_objects", "ipc_endpoints", "ipc_channels" }) |name| @field(self, name).reclaim();
    _ = self.fd_retired.reclaim();
    if (!self.fd_process_retired and self.retired_fd_tables == null and self.retired_fd_objects == null and self.retired_ipc_endpoints == null and self.retired_ipc_channels == null) return;
    if (self.fd_process_retired) {
        self.fd_tables_extra.reclaim();
        self.fd_process_retired = pending(&self.fd_tables_extra);
    }
    var tables = &self.retired_fd_tables;
    while (tables.*) |table| {
        table.entries.reclaim();
        if (pending(&table.entries)) tables = &table.retired_next else {
            tables.* = table.retired_next;
            table.retired_next = null;
            table.retirement_queued = false;
        }
    }
    inline for (.{ .{ "retired_fd_objects", "fd_objects" }, .{ "retired_ipc_endpoints", "ipc_endpoints" }, .{ "retired_ipc_channels", "ipc_channels" } }) |pair| {
        const link = &@field(self, pair[0]);
        while (link.*) |body| {
            link.* = body.retired_next;
            // Close/withdrawal under K and E has excluded every body borrower.
            // Move page ownership before the slot can be reused; a channel's
            // shared middle page is returned exactly once. Rejected grants
            // stay in the independent PMM-visible queue, not this slot.
            if (comptime std.mem.eql(u8, pair[1], "ipc_channels")) {
                const owner = &self.ipc_channels.body_owner;
                for ([_]?*types.IpcMessagePage{ body.queues[0].pages[0], body.queues[0].pages[1], body.queues[1].pages[1] }) |page|
                    owner.retireAllocation(types.IpcMessagePage, @as([*]types.IpcMessagePage, @ptrCast(page.?))) catch unreachable;
            } else if (comptime std.mem.eql(u8, pair[1], "ipc_endpoints")) {
                for (body.queue.pages) |page|
                    self.ipc_endpoints.body_owner.retireAllocation(types.IpcMessagePage, @as([*]types.IpcMessagePage, @ptrCast(page.?))) catch unreachable;
            }
            @field(self, pair[1]).finishWithdrawal(body, &self.object_directory_leaf);
        }
    }
}

pub fn deinit(self: anytype) void {
    inline for (.{ "fd_objects", "ipc_endpoints", "ipc_channels" }) |name| {
        var iterator = @field(self, name).iterator();
        while (iterator.next()) |item| {
            var endpoint_guard = if (comptime @hasField(@TypeOf(item.value.*), "exclusion")) item.value.exclusion.acquire() else @import("../sync/ipc_endpoint.zig").Lock.Guard{ .lock = null, .restore = false };
            defer endpoint_guard.end();
            item.value.live.close();
        }
    }
    for (0..self.process_capacity) |i| {
        const table = if (i < types.process_count) &self.fd_tables[i] else self.fd_tables_extra.get(@intCast(i - types.process_count)) orelse continue;
        retireTable(self, table);
    }
    reclaim(self);
    std.debug.assert(self.retired_fd_objects == null and self.retired_ipc_endpoints == null and self.retired_ipc_channels == null);
}

const TestFixture = struct {
    const kernel = @import("../kernel.zig");
    state: *kernel.KernelState,
    runtime: []align(4096) u8,
    fn init() !TestFixture {
        const runtime = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
        errdefer std.testing.allocator.free(runtime);
        try std.testing.expect(kernel.initRuntimeStorage(runtime));
        const state = try std.testing.allocator.create(kernel.KernelState);
        errdefer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        return .{ .state = state, .runtime = runtime };
    }
    fn deinit(self: *TestFixture) void {
        for (0..self.state.process_capacity) |i| self.state.releaseFdTableForProcessIndex(i);
        self.state.deinitPools();
        std.testing.allocator.destroy(self.state);
        std.testing.allocator.free(self.runtime);
    }
};

test "FD capability direct cache preserves size directions rights and independent generation" {
    var fixture = try TestFixture.init();
    defer fixture.deinit();
    const state = fixture.state;
    const owner = types.processPrincipalFromIndex(0).?;
    const rights: types.FdRights = .{ .call = true, .recv = true, .wait = true, .dup = true, .close = true };
    const pair = try state.createIpcChannelPairFds(owner, rights, .{}, 16);
    const a = directIpcUnderK(state, owner, pair.a, false).?;
    const b = directIpcUnderK(state, owner, pair.b, false).?;
    try std.testing.expect(a.queue != b.queue);
    try std.testing.expectEqual(a.queue, directIpcUnderK(state, owner, pair.b, true).?.queue);
    try std.testing.expectEqual(b.queue, directIpcUnderK(state, owner, pair.a, true).?.queue);
    try std.testing.expect(a.registration.side != b.registration.side);
    inline for (.{ false, true }) |outbound| {
        for ([_]types.Fd{ pair.a, pair.b }) |fd| {
            const small = directIpcUnderK(state, owner, fd, outbound).?;
            const full = ipcUnderK(state, owner, fd, if (outbound) .{ .call = true } else .{ .recv = true, .wait = true }, outbound).?;
            try std.testing.expectEqual(small.queue, full.queue);
            try std.testing.expectEqual(small.list, full.list);
            try std.testing.expect(full.opposite.? != full.list);
            try std.testing.expectEqual(full.list.exclusion, full.opposite.?.exclusion);
            try std.testing.expectEqualDeep(small.registration, full.registration);
            try std.testing.expectEqual(full.payload.channel.side ^ @as(u1, if (outbound) 1 else 0), full.registration.side);
            const body = state.ipcChannelSlot(full.payload.channel.channel).?;
            try std.testing.expectEqual(&body.receiver_hints[full.registration.side], full.hint);
            try std.testing.expectEqual(&body.waiters[1 - full.registration.side], full.opposite.?);
        }
    }
    const weak = try state.dupFd(owner, pair.a, 16, .{ .recv = true, .wait = true, .close = true }, .{});
    try std.testing.expect(directIpcUnderK(state, owner, weak, true) == null);
    try std.testing.expectEqual(a.queue, directIpcUnderK(state, owner, weak, false).?.queue);
    const entry = (try state.fdTableForActiveProcess(owner)).at(pair.a);
    const generation = entry.authority.ipc_generation;
    entry.authority.ipc_generation +%= 1;
    try std.testing.expect(directIpcUnderK(state, owner, pair.a, true) == null);
    entry.authority.ipc_generation = generation;
    try state.closeFd(owner, pair.a);
    try std.testing.expect(directIpcUnderK(state, owner, pair.a, false) == null);
    const endpoint = try state.createIpcEndpointFd(owner, rights, .{}, pair.a);
    try std.testing.expectEqual(pair.a, endpoint);
    const ep = directIpcUnderK(state, owner, endpoint, false).?;
    try std.testing.expect(ipcUnderK(state, owner, endpoint, .{ .recv = true, .wait = true }, false).?.opposite == null);
    try std.testing.expectEqual(ep.queue, directIpcUnderK(state, owner, endpoint, true).?.queue);
    try std.testing.expectEqual(@TypeOf(ep.registration.kind).endpoint, ep.registration.kind);
    try state.closeFd(owner, endpoint);
    const vmo = try state.createAnonymousVmoFd(owner, 4096, .{ .read = true, .close = true }, .{}, endpoint);
    try std.testing.expectEqual(endpoint, vmo);
    try std.testing.expect(directIpcUnderK(state, owner, vmo, false) == null);
    try std.testing.expect(state.fdEntryConst(owner, vmo).?.authority.reference != null);
    try state.closeFd(owner, vmo);
    const reused = try state.createIpcEndpointFd(owner, rights, .{}, endpoint);
    try std.testing.expectEqual(endpoint, reused);
    try std.testing.expect(directIpcUnderK(state, owner, reused, true) != null);
    try std.testing.expectEqual(@as(usize, 48), @sizeOf(types.FdEntry));
    try std.testing.expectEqual(@as(usize, 8), @alignOf(types.FdEntry));
}

test "FD capability K withdrawal permits immediate reuse but rejects old generation" {
    var fixture = try TestFixture.init();
    defer fixture.deinit();
    const state = fixture.state;
    const owner = types.processPrincipalFromIndex(0).?;
    const fd = try state.createIpcEndpointFd(owner, .{ .call = true, .close = true }, .{}, 16);
    const old = state.fdEntryConst(owner, fd).?.object;
    try std.testing.expect(ipcUnderK(state, owner, fd, .{ .call = true }, true) != null);
    // The K borrow ends before the writer. Only numeric identity survives.
    try state.closeFd(owner, fd);
    state.reclaimFdCapabilities();
    try std.testing.expect(ipcUnderK(state, owner, fd, .{ .call = true }, true) == null);
    const replacement = try state.createKernelObject(.event, .{ .event = 9 });
    try std.testing.expectEqual(old.index, replacement.index);
    try std.testing.expect(old.generation != replacement.generation);
    try std.testing.expect(state.fd_objects.resolve(.{ .index = old.index, .generation = old.generation }) == null);
}

// Host writer tests retain a value, never an entry/body pointer across mutation.
const Snapshot = struct {
    state: *@import("../kernel.zig").KernelState,
    owner: types.PrincipalId,
    fd: types.Fd,
    entry: types.FdCapability,
    fn current(self: Snapshot) bool {
        const entry = self.state.fdEntryConst(self.owner, self.fd) orelse return false;
        return entry.current(self.entry.generation);
    }
};
fn snapshot(state: *@import("../kernel.zig").KernelState, owner: types.PrincipalId, fd: types.Fd, rights: types.FdRights) ?Snapshot {
    if (!@import("builtin").is_test) @compileError("host writer snapshot only");
    const entry = (state.fdEntryConst(owner, fd) orelse return null).read() orelse return null;
    if (!types.isFdRightsSubset(rights, entry.rights)) return null;
    return .{ .state = state, .owner = owner, .fd = fd, .entry = entry };
}

test "FD capability entry seq rejects torn odd and saturated reads" {
    var entry: types.FdEntry = .{};
    entry.replace(.{ .object = .{ .kind = .event, .index = 7, .generation = 2 }, .rights = .{ .read = true } });
    const seq = @atomicLoad(u64, &entry.seq, .seq_cst);
    const first = entry.readFields(seq);
    // Deliberately interleave two parts of a sample with a real writer.
    entry.replace(.{ .object = .{ .kind = .event, .index = 9, .generation = 3 }, .rights = .{ .write = true } });
    const second = entry.readFields(seq);
    try std.testing.expect(first.object.index != second.object.index);
    try std.testing.expect(!entry.current(seq));
    try std.testing.expect(entry.read().?.rights.write);
    @atomicStore(u64, &entry.seq, 5, .seq_cst);
    try std.testing.expect(entry.read() == null);
    @atomicStore(u64, &entry.seq, std.math.maxInt(u64) - 1, .seq_cst);
    entry.replace(.{});
    try std.testing.expect(entry.read() == null);
    entry.replace(.{});
    try std.testing.expect(entry.read() == null);
}

test "FD capability every capability writer publishes rights flags and slot replacement" {
    var fixture = try TestFixture.init();
    defer fixture.deinit();
    const state = fixture.state;
    const owner = types.processPrincipalFromIndex(0).?;
    const child = types.processPrincipalFromIndex(1).?;
    const forked = types.processPrincipalFromIndex(2).?;
    const rights: types.FdRights = .{ .call = true, .close = true, .dup = true, .transfer = true, .set_flags = true };
    const fd = try state.createIpcEndpointFd(owner, rights, .{}, 16);
    const before = snapshot(state, owner, fd, .{ .call = true }).?;
    const copy = try state.dupFd(owner, fd, 17, rights, .{ .cloexec = true });
    try std.testing.expect(before.current());
    const copied = snapshot(state, owner, copy, .{ .call = true }).?;
    try std.testing.expect(copied.entry.flags.cloexec);
    try std.testing.expectEqualDeep(state.fdEntryConst(owner, copy).?.object, copied.entry.object);
    const replacement = try state.createIpcEndpointFd(owner, rights, .{}, 18);
    const old = snapshot(state, owner, copy, .{}).?;
    const epoch = old.entry.generation;
    try state.replaceFd(owner, copy, replacement, .{ .close = true }, .{});
    try std.testing.expect(!old.current());
    try std.testing.expect(snapshot(state, owner, copy, .{ .call = true }) == null);
    const replaced = snapshot(state, owner, copy, .{ .close = true }).?;
    try std.testing.expect(replaced.entry.generation != epoch);
    try std.testing.expectEqualDeep(state.fdEntryConst(owner, replacement).?.object, replaced.entry.object);
    const cached_queue = directIpcUnderK(state, owner, fd, true).?.queue;
    const moved = try state.transferFd(owner, child, fd, 16, rights, .{}, .move);
    try std.testing.expect(directIpcUnderK(state, owner, fd, true) == null);
    try std.testing.expectEqual(cached_queue, directIpcUnderK(state, child, moved, true).?.queue);
    try std.testing.expect(snapshot(state, owner, fd, .{}) == null);
    try std.testing.expect(snapshot(state, child, moved, .{ .call = true }) != null);
    try state.setFdFlags(child, moved, .{ .cloexec = true }, .{ .cloexec = true });
    const flags = snapshot(state, child, moved, .{}).?;
    try std.testing.expect(flags.entry.flags.cloexec);
    const fork_table = state.fdTableForProcessIndex(2).?;
    for (0..fork_table.capacity) |i| {
        if (!fork_table.at(i).isEmpty()) try state.closeFd(forked, @intCast(i));
    }
    try state.cloneFdTableForFork(child, forked);
    try std.testing.expectEqual(cached_queue, directIpcUnderK(state, forked, moved, true).?.queue);
    const inherited = snapshot(state, forked, moved, .{ .call = true }).?;
    try std.testing.expect(inherited.entry.flags.cloexec);
    var pages: types.FreePageList = .{};
    try state.grantFdsForProcessCreate(child, owner, &.{.{
        .source_fd = moved,
        .target_fd = fd,
        .rights = types.fdRightsToBits(rights),
        .flags = 0,
    }}, &pages);
    const granted = snapshot(state, owner, fd, .{ .call = true }).?;
    try std.testing.expectEqualDeep(state.fdEntryConst(child, moved).?.object, granted.entry.object);
    try std.testing.expectEqual(cached_queue, directIpcUnderK(state, owner, fd, true).?.queue);
    try state.closeFd(child, moved);
    try std.testing.expectEqual(cached_queue, directIpcUnderK(state, forked, moved, true).?.queue);
    try std.testing.expectEqual(cached_queue, directIpcUnderK(state, owner, fd, true).?.queue);
}

test "FD capability canonical 4200 FD writers allocate zero and touch one entry" {
    const storage = @import("../memory/owned_storage.zig");
    const slots = @import("../memory/slot_table.zig");
    const Counter = struct {
        upstream: storage.Allocator,
        calls: usize = 0,
        fail: bool = false,
        fn interface(self: *@This()) storage.Allocator {
            return .{ .context = self, .acquire = acquire, .release = release };
        }
        fn acquire(ctx: *anyopaque, pages: usize) ?storage.MappedExtent {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            self.calls += 1;
            if (self.fail) return null;
            return self.upstream.acquire(self.upstream.context, pages);
        }
        fn release(ctx: *anyopaque, extent: storage.PhysicalExtent) bool {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            return self.upstream.release(self.upstream.context, extent);
        }
    };
    for ([_]usize{ 32, 4200 }) |fd_count| {
        var counter: Counter = undefined;
        var fixture = try TestFixture.init();
        defer fixture.deinit();
        const state = fixture.state;
        counter = .{ .upstream = try state.poolAllocator() };
        state.pool_allocator = counter.interface();
        const owner = types.processPrincipalFromIndex(0).?;
        const table = state.getFdTable(owner).?;
        const capacity: usize = if (fd_count == 32) 64 else 8192;
        const old_capacity = table.capacity;
        slots.test_directory_copy_bytes = 0;
        counter.calls = 0;
        try table.ensureCapacity(capacity, counter.interface(), &state.object_directory_leaf, &state.fd_retired);
        const Table = slots.SlotTable(types.FdEntry);
        const chunks = (table.capacity - old_capacity + Table.block_slots - 1) / Table.block_slots;
        try std.testing.expect(slots.test_directory_copy_bytes <= chunks * Table.max_path_copy_bytes);
        try std.testing.expect(counter.calls <= chunks * Table.max_chunk_allocations);
        std.debug.print("FD growth added={} allocations={} bounded-path-copy-bytes={}\n", .{ table.capacity - old_capacity, counter.calls, slots.test_directory_copy_bytes });
        const rights: types.FdRights = .{ .close = true, .dup = true, .set_flags = true };
        const object = try state.createKernelObject(.event, .{ .event = 91 });
        const first = try state.installFd(owner, object, rights, .{}, 16);
        for (1..fd_count) |_| _ = try state.dupFd(owner, first, 16, rights, .{});
        const victim: types.Fd = @intCast(16 + fd_count / 2);
        const unaffected = snapshot(state, owner, first, .{}).?;
        counter.calls = 0;
        counter.fail = true; // Memory pressure cannot disable existing authority.
        slots.test_directory_copy_bytes = 0;
        slots.test_mark_search_nodes = 0;
        types.fd_test_write_bytes = 0;
        for (0..32) |_| {
            try state.closeFd(owner, victim);
            try std.testing.expectEqual(@as(types.Fd, victim), try state.dupFd(owner, first, 16, rights, .{}));
            try state.setFdFlags(owner, victim, .{ .cloexec = true }, .{ .cloexec = true });
            try std.testing.expect(unaffected.current());
            const changed = snapshot(state, owner, victim, .{}).?;
            try std.testing.expect(changed.current() and changed.entry.flags.cloexec);
        }
        try std.testing.expectEqual(@as(usize, 0), counter.calls);
        try std.testing.expectEqual(@as(usize, 0), slots.test_directory_copy_bytes);
        try std.testing.expectEqual(@as(usize, 32 * 3 * 22), types.fd_test_write_bytes);
        try std.testing.expect(slots.test_mark_search_nodes <= 32 * 12);
        std.debug.print("FD{} entry writers: operations=96 allocations={} directory-copy-bytes={} capability-field-bytes={} free-search-nodes={}\n", .{ fd_count, counter.calls, slots.test_directory_copy_bytes, types.fd_test_write_bytes, slots.test_mark_search_nodes });
    }
}

test "FD capability concurrent seq samples never combine two capabilities" {
    const Race = struct {
        entry: types.FdEntry = .{},
        done: std.atomic.Value(bool) = .init(false),
        fn run(self: *@This()) void {
            for (0..10000) |i| self.entry.replace(.{
                .object = .{ .kind = .event, .index = @intCast(i & 1), .generation = @intCast((i & 1) + 1) },
                .rights = if (i & 1 == 0) .{ .read = true } else .{ .write = true },
            });
            self.done.store(true, .release);
        }
    };
    var race: Race = .{};
    const thread = try std.Thread.spawn(.{}, Race.run, .{&race});
    defer thread.join();
    while (!race.done.load(.acquire)) {
        const value = race.entry.read() orelse continue;
        if (value.object.isNull()) continue;
        try std.testing.expectEqual(value.object.index + 1, value.object.generation);
        try std.testing.expectEqual(value.object.index == 0, value.rights.read);
        try std.testing.expectEqual(value.object.index == 1, value.rights.write);
    }
}

test "FD capability rejected grants remain reachable from PMM without K" {
    const storage = @import("../memory/owned_storage.zig");
    const Backend = struct {
        upstream: storage.Allocator,
        reject: bool = false,
        returns: usize = 0,
        fn interface(self: *@This()) storage.Allocator {
            return .{ .context = self, .acquire = acquire, .release = release };
        }
        fn acquire(ctx: *anyopaque, pages: usize) ?storage.MappedExtent {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            return self.upstream.acquire(self.upstream.context, pages);
        }
        fn release(ctx: *anyopaque, extent: storage.PhysicalExtent) bool {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            self.returns += 1;
            if (self.reject) return false;
            return self.upstream.release(self.upstream.context, extent);
        }
    };
    var backend: Backend = undefined;
    var fixture = try TestFixture.init();
    defer fixture.deinit();
    const state = fixture.state;
    backend = .{ .upstream = try state.poolAllocator() };
    const owner = types.processPrincipalFromIndex(0).?;
    state.releaseFdTableForProcessIndex(0);
    state.pool_allocator = backend.interface();
    _ = try state.createIpcEndpointFd(owner, .{ .call = true, .close = true }, .{}, 16);
    backend.returns = 0;
    backend.reject = true;
    state.releaseFdTableForProcessIndex(0);
    try std.testing.expect(state.fd_retired.head != null);
    reclaim(state);
    try std.testing.expect(backend.returns != 0);
    try std.testing.expect(state.fd_retired.head != null);
    backend.reject = false;
    var registry: @import("../memory/reclaim_cache.zig").Registry = .{ .fd_retired = &state.fd_retired };
    try std.testing.expect(registry.reclaim() != 0);
    try std.testing.expect(state.fd_retired.head == null);
    try std.testing.expect(state.getFdTable(owner).?.entries.body_owner.retired == null);
}
