const std = @import("std");
const policy = @import("ram_cache_policy.zig");
const kernel = @import("../kernel.zig");
const paging = @import("ram_cache_paging.zig");

pub const Block = struct {
    owner: *Store,
    next: ?*Block = null,
    pages: usize,
    available: bool = false,
    published: bool = false,
    snapshot: policy.Snapshot = .{},

    pub fn spans(self: *Block) []policy.Span {
        const begin = std.mem.alignForward(usize, @sizeOf(Block), @alignOf(policy.Span));
        const ptr: [*]policy.Span = @ptrFromInt(@intFromPtr(self) + begin);
        return ptr[0 .. (self.pages * 4096 - begin) / @sizeOf(policy.Span)];
    }

    pub fn overlapsTargets(self: *const Block, pages: []const u64) bool {
        return paging.pagesIntersect(pages, @intFromPtr(self), self.pages * 4096);
    }

    pub fn items(self: *Block, comptime T: type, count: usize) ![]T {
        if (@alignOf(T) > 4096) return error.UnsupportedAlignment;
        const begin = std.mem.alignForward(usize, @sizeOf(Block), @alignOf(T));
        const end = try std.math.add(usize, begin, try std.math.mul(usize, count, @sizeOf(T)));
        if (end > self.pages * 4096) return error.NoSpace;
        const ptr: [*]T = @ptrFromInt(@intFromPtr(self) + begin);
        return ptr[0..count];
    }
};

/// Serialized by the physical-cache writer. Free-list insertion can fail;
/// retained blocks remain owned WB metadata and are reused, never forgotten.
/// Store itself must have a stable address for every block's lifetime.
pub const Store = struct {
    unused: ?*Block = null,
    active: ?*Block = null,

    /// The native caller has authorized this canonical VMO and holds the
    /// state/all-AS/shared-VM transaction continuously across this call.
    pub fn change(self: *Store, state: *kernel.KernelState, ref: kernel.NativeVmoRef, first: usize, count: usize, mode: @import("ram_cache_transition.zig").Mode, free_list: *kernel.FreePageList, cr3: u64) !void {
        if (count == 0 or cr3 == 0 or cr3 & 4095 != 0) return error.Invalid;
        const trace = count >= 128;
        if (trace) @import("../kernel_log.zig").writeFmt("ram-cache: change begin pages={} mode={}\n", .{ count, @intFromEnum(mode) });
        try @import("ram_cache_request.zig").requireTranslatedDma(@import("../iommu.zig").isActive());
        const pages_block = try self.allocateArray(free_list, u64, count);
        defer self.releaseDraft(free_list, pages_block) catch unreachable;
        const pages = try pages_block.items(u64, count);
        const request = try @import("ram_cache_request.zig").prepare(state, ref, first, pages, mode);
        if (trace) @import("../kernel_log.zig").write("ram-cache: request prepared\n");
        if (pages_block.overlapsTargets(pages)) return error.MetadataOverlap;
        try self.applyRequest(state, ref, request, free_list, cr3, false);
        if (trace) @import("../kernel_log.zig").write("ram-cache: change complete\n");
    }

    /// Same outer lock ordering as change. Hardware DMA must be revoked
    /// before final-drop cleanup is entered; failures retain the zero-ref VMO.
    pub fn retire(self: *Store, state: *kernel.KernelState, ref: kernel.NativeVmoRef, free_list: *kernel.FreePageList, cr3: u64) !void {
        if (cr3 == 0 or cr3 & 4095 != 0) return error.Invalid;
        try @import("ram_cache_request.zig").requireTranslatedDma(@import("../iommu.zig").isActive());
        const active = self.active orelse return error.InvalidState;
        if (!@import("../user_copy.zig").ramCacheSnapshotIsCurrent(&active.snapshot)) return error.ForeignPublication;
        const owner = try state.retiredRamCacheOwner(ref);
        var count: usize = 0;
        for (active.snapshot.policy) |span| {
            if (span.owner == owner) count = try std.math.add(usize, count, @intCast(span.bytes / 4096));
        }
        if (count != 0) {
            const pages_block = try self.allocateArray(free_list, u64, count);
            defer self.releaseDraft(free_list, pages_block) catch unreachable;
            const pages = try pages_block.items(u64, count);
            var used: usize = 0;
            for (active.snapshot.policy) |span| {
                if (span.owner != owner) continue;
                var pa = span.base;
                while (pa < span.base + span.bytes) : (pa += 4096) {
                    pages[used] = pa;
                    used += 1;
                }
            }
            const seen_block = try self.allocateArray(free_list, bool, count);
            defer self.releaseDraft(free_list, seen_block) catch unreachable;
            if (pages_block.overlapsTargets(pages) or seen_block.overlapsTargets(pages)) return error.MetadataOverlap;
            _ = try state.validateRetiredRamCachePages(ref, pages, try seen_block.items(bool, count));
            try @import("ram_cache_request.zig").validateDmaQuarantine(pages);
            const pat = try @import("../smp.zig").ramCachePatProof(pages, .wb);
            try self.applyRequest(state, ref, .{ .owner = owner, .pages = pages, .mode = .wb, .pat = pat }, free_list, cr3, true);
        }
        // On PMM failure this leaves the retiring owner with only the pages
        // not yet returned. A retry sees no cached spans and only frees WB.
        try state.finishRamCacheRetirement(ref, &self.active.?.snapshot, free_list);
    }

    fn applyRequest(self: *Store, state: *kernel.KernelState, ref: kernel.NativeVmoRef, request: @import("ram_cache_request.zig").Prepared, free_list: *kernel.FreePageList, cr3: u64, retiring: bool) !void {
        try @import("../arch/x86_64/platform.zig").validateRamCacheKernelRoots();
        const plan_api = @import("ram_cache_vm_plan.zig");
        const pages = request.pages;
        const trace = pages.len >= 128;
        const need = try plan_api.measure(pages);
        if (trace) @import("../kernel_log.zig").writeFmt("ram-cache: aliases measured sources={} leaves={} splits={}\n", .{ need.sources, need.leaves, need.splits });
        const sources_block = try self.allocateArray(free_list, paging.AliasSource, need.sources);
        defer self.releaseDraft(free_list, sources_block) catch unreachable;
        const leaves_block = try self.allocateArray(free_list, paging.Leaf, need.leaves);
        defer self.releaseDraft(free_list, leaves_block) catch unreachable;
        const splits_block = try self.allocateArray(free_list, plan_api.Split, need.splits);
        defer self.releaseDraft(free_list, splits_block) catch unreachable;
        for ([_]*Block{ sources_block, leaves_block, splits_block }) |block|
            if (block.overlapsTargets(pages)) return error.MetadataOverlap;
        const sources = try plan_api.collect(pages, try sources_block.items(paging.AliasSource, need.sources));
        if (trace) @import("../kernel_log.zig").write("ram-cache: aliases collected\n");
        var plan = try plan_api.prepare(pages, sources, try leaves_block.items(paging.Leaf, need.leaves), try splits_block.items(plan_api.Split, need.splits));
        if (trace) @import("../kernel_log.zig").writeFmt("ram-cache: aliases prepared leaves={} splits={}\n", .{ plan.leaves.len, plan.splits.len });
        // This runs before scratch defers. Published split PTs are retained
        // by the kernel/process PT store; only unpublished reservations free.
        defer plan.release();
        try plan.validateStorage(pages);
        if (retiring) {
            if (request.mode != .wb or request.pat != 0 or try state.retiredRamCacheOwner(ref) != request.owner) return error.InvalidState;
            var backend = Native{ .free_list = free_list };
            try self.applyWith(&backend, request, &plan, cr3, @import("ram_cache_commit.zig").commit);
        } else {
            try self.apply(state, ref, free_list, request, &plan, cr3);
        }
        if (trace) @import("../kernel_log.zig").write("ram-cache: policy committed\n");
    }

    fn arrayCapacity(comptime T: type, count: usize) !usize {
        if (@alignOf(T) > 4096) return error.UnsupportedAlignment;
        const payload = try std.math.mul(usize, count, @sizeOf(T));
        const padding = @alignOf(T) - 1;
        const bytes = try std.math.add(usize, payload, padding);
        return (try std.math.add(usize, bytes, @sizeOf(policy.Span) - 1)) / @sizeOf(policy.Span);
    }

    fn allocateArray(self: *Store, free_list: *kernel.FreePageList, comptime T: type, count: usize) !*Block {
        return self.allocate(free_list, try arrayCapacity(T, count));
    }

    /// Internal writer transaction; not a syscall. Canonical VMO lifetime
    /// must be prevalidated by the caller and updated infallibly before its
    /// VM locks are released. execute errors must precede all publication.
    pub fn apply(self: *Store, state: *kernel.KernelState, ref: kernel.NativeVmoRef, free_list: *kernel.FreePageList, request: @import("ram_cache_request.zig").Prepared, plan: *@import("ram_cache_vm_plan.zig").Plan, cr3: u64) !void {
        const lifetime = try state.prepareNativeRamCacheLifetime(ref);
        if (lifetime.owner != request.owner) return error.OwnerConflict;
        var backend = Native{ .free_list = free_list };
        try self.applyWith(&backend, request, plan, cr3, @import("ram_cache_commit.zig").commit);
        lifetime.finish(&self.active.?.snapshot);
    }

    fn applyWith(self: *Store, backend: anytype, request: @import("ram_cache_request.zig").Prepared, plan: anytype, cr3: u64, comptime execute: anytype) !void {
        const copy = @import("../user_copy.zig");
        const previous = self.active;
        if (!copy.ramCacheSnapshotIsCurrent(if (previous) |b| &b.snapshot else null)) return error.ForeignPublication;
        if (previous) |b| {
            if (b.owner != self or b.available or !b.published) return error.InvalidState;
            if (b.overlapsTargets(request.pages)) return error.MetadataOverlap;
        }
        const empty: policy.Snapshot = .{};
        const old = if (previous) |b| &b.snapshot else &empty;
        const pair = try self.prepareWith(backend, request, old);
        // Protection is established before execute can publish either block.
        pair.pending.published = true;
        pair.final.published = true;
        execute(plan, .{
            .pages = request.pages,
            .owner = request.owner,
            .mode = request.mode,
            .pat = request.pat,
            .old = old,
            .pending = &pair.pending.snapshot,
            .final = &pair.final.snapshot,
        }, cr3) catch |err| {
            std.debug.assert(copy.ramCacheSnapshotIsCurrent(if (previous) |b| &b.snapshot else null));
            pair.pending.published = false;
            pair.final.published = false;
            self.releaseWith(backend, pair.pending) catch unreachable;
            self.releaseWith(backend, pair.final) catch unreachable;
            return err;
        };
        // execute returns only after the final acknowledged reader barrier.
        // Publication alone would not permit either of these reclamations.
        std.debug.assert(copy.ramCacheSnapshotIsCurrent(&pair.final.snapshot));
        self.active = pair.final;
        pair.pending.published = false;
        self.releaseWith(backend, pair.pending) catch unreachable;
        if (previous) |b| {
            b.published = false;
            self.releaseWith(backend, b) catch unreachable;
        }
    }

    pub const DraftPair = struct { pending: *Block, final: *Block };

    pub fn prepare(self: *Store, free_list: *kernel.FreePageList, request: @import("ram_cache_request.zig").Prepared, old: *const policy.Snapshot) !DraftPair {
        var backend = Native{ .free_list = free_list };
        return self.prepareWith(&backend, request, old);
    }

    fn prepareWith(self: *Store, backend: anytype, request: @import("ram_cache_request.zig").Prepared, old: *const policy.Snapshot) !DraftPair {
        const final_capacity = try std.math.add(usize, old.policy.len, try std.math.mul(usize, request.pages.len, 2));
        const pending = try self.allocateWith(backend, request.pages.len);
        errdefer self.releaseWith(backend, pending) catch unreachable;
        const final = try self.allocateWith(backend, final_capacity);
        errdefer self.releaseWith(backend, final) catch unreachable;
        // Writer metadata must stay reachable as WB while target aliases are
        // withdrawn; never include these allocations in its own transition.
        if (pending.overlapsTargets(request.pages) or final.overlapsTargets(request.pages)) return error.MetadataOverlap;
        const pair = try request.policies(old, pending.spans(), final.spans());
        pending.snapshot = pair.pending;
        final.snapshot = pair.final;
        return .{ .pending = pending, .final = final };
    }

    pub fn allocate(self: *Store, free_list: *kernel.FreePageList, capacity: usize) !*Block {
        var backend = Native{ .free_list = free_list };
        return self.allocateWith(&backend, capacity);
    }

    fn allocateWith(self: *Store, backend: anytype, capacity: usize) !*Block {
        const head = std.mem.alignForward(usize, @sizeOf(Block), @alignOf(policy.Span));
        const bytes = try std.math.add(usize, head, try std.math.mul(usize, capacity, @sizeOf(policy.Span)));
        const pages = (try std.math.add(usize, bytes, 4095)) / 4096;
        var link = &self.unused;
        while (link.*) |block| {
            if (block.pages >= pages) {
                link.* = block.next;
                block.next = null;
                block.available = false;
                block.snapshot = .{};
                return block;
            }
            link = &block.next;
        }
        const address = try backend.take(pages);
        const block: *Block = @ptrFromInt(address);
        block.* = .{ .owner = self, .pages = pages };
        return block;
    }

    /// Draft-only rollback. A published block cannot pass this gate; the
    /// transaction owner must separately acknowledge its reader retirement
    /// before converting it back to draft. No published block is recycled by
    /// this module on the strength of a mere publication replacement.
    pub fn releaseDraft(self: *Store, free_list: *kernel.FreePageList, block: *Block) !void {
        var backend = Native{ .free_list = free_list };
        try self.releaseWith(&backend, block);
    }

    fn releaseWith(self: *Store, backend: anytype, block: *Block) !void {
        if (block.owner != self or block.available or block.published) return error.InvalidState;
        // Save everything before give(): another PMM consumer may overwrite
        // this memory immediately after successful insertion.
        const address = @intFromPtr(block);
        const pages = block.pages;
        if (backend.give(address, pages)) return;
        block.snapshot = .{};
        block.available = true;
        block.next = self.unused;
        self.unused = block;
    }
};

const Native = struct {
    free_list: *kernel.FreePageList,
    fn take(self: *@This(), pages: usize) !u64 {
        return self.free_list.popContiguousBelow(pages, @import("../arch/x86_64/physical_layout.zig").identity_limit);
    }
    fn give(self: *@This(), address: u64, pages: usize) bool {
        self.free_list.appendContiguousRange(0, address, pages) catch return false;
        return true;
    }
};

test "MMIO overlay snapshot owner reclaims only after final reader barrier" {
    const copy = @import("../user_copy.zig");
    defer copy.publishRamCacheSnapshot(null);
    copy.publishRamCacheSnapshot(null);
    const Backend = struct {
        memory: [4][4096]u8 align(4096) = undefined,
        used: [4]bool = .{ false, false, false, false },
        barrier: bool = false,
        publication_started: bool = false,
        released: usize = 0,
        fn take(self: *@This(), pages: usize) !u64 {
            if (pages != 1) return error.OutOfMemory;
            for (&self.used, 0..) |*used, i| {
                if (used.*) continue;
                used.* = true;
                return @intFromPtr(&self.memory[i]);
            }
            return error.OutOfMemory;
        }
        fn give(self: *@This(), address: u64, _: usize) bool {
            const block: *Block = @ptrFromInt(address);
            std.debug.assert(!copy.ramCacheSnapshotIsCurrent(&block.snapshot));
            std.debug.assert(!self.publication_started or self.barrier);
            for (&self.used, 0..) |*used, i| {
                if (address != @intFromPtr(&self.memory[i])) continue;
                std.debug.assert(used.*);
                used.* = false;
                self.released += 1;
                return true;
            }
            unreachable;
        }
        fn live(self: *@This()) usize {
            var count: usize = 0;
            for (self.used) |used| count += @intFromBool(used);
            return count;
        }
    };
    const Executor = struct {
        backend: *Backend,
        fail: bool = false,
        fn run(self: *@This(), change: @import("ram_cache_commit.zig").Change, _: u64) !void {
            try change.validate();
            if (self.fail) return error.InjectedPreflight;
            const released = self.backend.released;
            self.backend.barrier = false;
            self.backend.publication_started = true;
            copy.publishRamCacheSnapshot(change.pending);
            try std.testing.expect(copy.ramCachePolicyForLockedPage(change.pages[0]) == .blocked);
            copy.publishRamCacheSnapshot(change.final);
            try std.testing.expectEqual(released, self.backend.released);
            self.backend.barrier = true; // Models commit's final acknowledged rendezvous.
        }
    };
    var store = Store{};
    var backend = Backend{};
    var executor = Executor{ .backend = &backend };
    var request = @import("ram_cache_request.zig").Prepared{
        .owner = 7,
        .pages = &.{0x200000},
        .mode = .wc,
        .pat = 4,
    };
    try store.applyWith(&backend, request, &executor, 0x1000, Executor.run);
    try std.testing.expectEqual(@as(usize, 1), backend.live());
    try std.testing.expect(store.active.?.published);
    const first = store.active.?;
    executor.fail = true;
    try std.testing.expectError(error.InjectedPreflight, store.applyWith(&backend, request, &executor, 0x1000, Executor.run));
    try std.testing.expect(store.active.? == first);
    try std.testing.expect(copy.ramCacheSnapshotIsCurrent(&first.snapshot));
    try std.testing.expectEqual(@as(usize, 1), backend.live());
    executor.fail = false;
    request.mode = .wb;
    request.pat = 0;
    try store.applyWith(&backend, request, &executor, 0x1000, Executor.run);
    try std.testing.expectEqual(@as(usize, 1), backend.live());
    try std.testing.expect(copy.ramCachePolicyForLockedPage(0x200000) == .ordinary);
    const final = store.active.?;
    try std.testing.expect(copy.ramCacheSnapshotIsCurrent(&final.snapshot));
    const foreign: policy.Snapshot = .{};
    copy.publishRamCacheSnapshot(&foreign);
    try std.testing.expectError(error.ForeignPublication, store.applyWith(&backend, request, &executor, 0x1000, Executor.run));
    try std.testing.expectEqual(@as(usize, 1), backend.live());
    // Test owns no concurrent readers; model completed final teardown.
    copy.publishRamCacheSnapshot(null);
    store.active = null;
    final.published = false;
    try store.releaseWith(&backend, final);
    try std.testing.expectEqual(@as(usize, 0), backend.live());
}

test "MMIO overlay RAM snapshot storage retains failed returns and protects published blocks" {
    const Backend = struct {
        memory: [16384]u8 align(4096) = undefined,
        used: bool = false,
        fail_take: bool = false,
        fail_return: bool = false,
        takes: usize = 0,
        gives: usize = 0,
        fn take(self: *@This(), pages: usize) !u64 {
            self.takes += 1;
            if (self.used or self.fail_take or pages > 4) return error.OutOfMemory;
            self.used = true;
            return @intFromPtr(&self.memory);
        }
        fn give(self: *@This(), address: u64, _: usize) bool {
            std.debug.assert(address == @intFromPtr(&self.memory) and self.used);
            self.gives += 1;
            if (self.fail_return) return false;
            self.used = false;
            return true;
        }
    };
    var store = Store{};
    var backend = Backend{};
    const first = try store.allocateWith(&backend, 100);
    try std.testing.expect(first.spans().len >= 100);
    const split_items = try first.items(@import("ram_cache_vm_plan.zig").Split, 1);
    try std.testing.expect(@intFromPtr(split_items.ptr) % @alignOf(@import("ram_cache_vm_plan.zig").Split) == 0);
    try std.testing.expectError(error.NoSpace, first.items(u64, 10000));
    try std.testing.expectError(error.Overflow, first.items(u64, std.math.maxInt(usize)));
    try std.testing.expect(first.overlapsTargets(&.{@intFromPtr(first)}));
    first.published = true;
    try std.testing.expectError(error.InvalidState, store.releaseWith(&backend, first));
    try std.testing.expectEqual(@as(usize, 0), backend.gives);
    first.published = false; // Fixture models completed reader retirement.
    backend.fail_return = true;
    try store.releaseWith(&backend, first);
    try std.testing.expectError(error.InvalidState, store.releaseWith(&backend, first));
    const again = try store.allocateWith(&backend, 50);
    try std.testing.expect(first == again);
    try std.testing.expectEqual(@as(usize, 1), backend.takes);
    backend.fail_return = false;
    try store.releaseWith(&backend, again);
    try std.testing.expect(!backend.used and store.unused == null);
    backend.fail_take = true;
    try std.testing.expectError(error.OutOfMemory, store.allocateWith(&backend, 100));
    try std.testing.expectError(error.Overflow, store.allocateWith(&backend, std.math.maxInt(usize)));
    backend.fail_take = false;
    const old: policy.Snapshot = .{};
    const request = @import("ram_cache_request.zig").Prepared{
        .owner = 7,
        .pages = &.{0x200000},
        .mode = .wc,
        .pat = 4,
    };
    // Backend has one allocation: second allocation fails. The first draft
    // must be returned, with no policy published and no storage stranded.
    try std.testing.expectError(error.OutOfMemory, store.prepareWith(&backend, request, &old));
    try std.testing.expect(!backend.used and store.unused == null);
    backend.fail_return = true;
    try std.testing.expectError(error.OutOfMemory, store.prepareWith(&backend, request, &old));
    try std.testing.expect(backend.used and store.unused != null);
    const retained = try store.allocateWith(&backend, 1);
    backend.fail_return = false;
    try store.releaseWith(&backend, retained);
    try std.testing.expect(!backend.used and store.unused == null);
}

test "MMIO overlay native RAM change rejects empty requests before touching state" {
    var store = Store{};
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    const free_list = try std.testing.allocator.create(kernel.FreePageList);
    defer std.testing.allocator.destroy(free_list);
    try std.testing.expectError(error.Invalid, store.change(state, .{}, 0, 0, .wc, free_list, 0));
    try std.testing.expectError(error.Invalid, store.retire(state, .{}, free_list, 0));
    try std.testing.expect(store.active == null and store.unused == null);
    try std.testing.expectError(error.Overflow, Store.arrayCapacity(u64, std.math.maxInt(usize)));
}
