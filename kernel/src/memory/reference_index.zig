//! Reverse edges represent existing ownership; they never acquire a retain.
//! Source pools are protected by the source transaction (K or AS/shared VM).
//! Each target has its own terminal leaf. No allocation, return, callback or
//! source lookup is allowed while that leaf is held.
const std = @import("std");
const slots = @import("slot_table.zig");
const storage = @import("owned_storage.zig");
pub const Allocator = storage.Allocator;
pub const MappedExtent = storage.MappedExtent;
pub const PhysicalExtent = storage.PhysicalExtent;
pub const checks_enabled = if (@hasDecl(@import("root"), "kernel_reference_checks_enabled")) @import("root").kernel_reference_checks_enabled else false;

pub const Kind = enum { vma, fd_object, child, fd_entry, ipc, pinned_object, kernel };
pub const Identity = struct {
    kind: Kind = .vma,
    owner: u32 = 0,
    owner_generation: u32 = 0,
    index: u32 = 0,
    generation: u32 = 0,
    sequence: u64 = 0,
    position: u8 = 0,
    direction: u8 = 0,
    container: u16 = 0,

    pub fn eql(a: Identity, b: Identity) bool {
        return std.meta.eql(a, b);
    }
};
pub const Range = struct {
    start: u64 = 0,
    size: u64 = 0,
    offset: u64 = 0,
};
pub const Node = struct {
    pool: ?*Pool = null,
    allocation: slots.Ref = .{ .index = 0, .generation = 0 },
    prepared_target: ?*Root = null,
    message_next: ?*Node = null,
    target: ?*Root = null,
    previous: ?*Node = null,
    next: ?*Node = null,
    identity: Identity = .{},
    range: Range = .{},
};

pub fn pendingStorage(table: anytype) bool {
    inline for (.{ "directory_owner", "metadata_owner", "body_owner" }) |name| {
        const owner = &@field(table, name);
        if (owner.pending != null or owner.retired != null) return true;
    }
    return false;
}

pub const Pool = struct {
    nodes: slots.SlotTable(Node) = .{},
    removals: u32 = 0,
    needs_reclaim: bool = false,

    /// Reserve before publishing any source or consuming its provisional
    /// retain. The returned node stays private until Root.connect succeeds.
    pub fn reserve(self: *Pool, allocator: storage.Allocator) slots.Error!*Node {
        const allocation = self.nodes.allocate(allocator, null, std.math.maxInt(u32)) catch |err| {
            // A failed preparation may retain rejected grants in any owner.
            self.needs_reclaim = true;
            return err;
        };
        allocation.value.pool = self;
        allocation.value.allocation = allocation.ref;
        return allocation.value;
    }

    pub fn cancel(node: *Node) void {
        std.debug.assert(node.target == null);
        const pool = node.pool orelse unreachable;
        pool.nodes.release(node.allocation) catch unreachable;
        pool.removals +%= 1;
        if (pool.nodes.live == 0 or pool.removals % 256 == 0) pool.needs_reclaim = true;
    }

    pub fn reclaim(self: *Pool) void {
        self.nodes.reclaimEmptyBodies();
        self.needs_reclaim = pendingStorage(&self.nodes);
    }

    pub fn deinit(self: *Pool) void {
        std.debug.assert(self.nodes.live == 0);
        self.nodes.deinit();
    }
};

pub const Root = struct {
    checked_seen: if (@import("builtin").is_test or checks_enabled) u32 else void = if (@import("builtin").is_test or checks_enabled) 0 else {},
    // Reuse the terminal leaf protocol, not a shared/global lock instance.
    // Callers must finish source/table lookups before taking this lock.
    leaf: slots.Leaf = .{},
    head: ?*Node = null,
    count: u32 = 0,
    temporary: u32 = 0,

    pub fn retainTemporary(self: *Root) void {
        const flags = self.leaf.lock();
        defer self.leaf.unlock(flags);
        std.debug.assert(self.temporary != std.math.maxInt(u32));
        self.temporary += 1;
    }

    pub fn releaseTemporary(self: *Root) void {
        const flags = self.leaf.lock();
        defer self.leaf.unlock(flags);
        std.debug.assert(self.temporary != 0);
        self.temporary -= 1;
    }

    pub fn connect(self: *Root, node: *Node, identity: Identity, range: Range, strong: bool) void {
        const flags = self.leaf.lock();
        defer self.leaf.unlock(flags);
        std.debug.assert(node.target == null and node.previous == null and node.next == null);
        if (strong) {
            std.debug.assert(self.temporary != 0);
            self.temporary -= 1;
        }
        node.identity = identity;
        node.range = range;
        node.target = self;
        node.next = self.head;
        if (self.head) |head| head.previous = node;
        self.head = node;
        self.count += 1;
    }

    /// Unpublication converts the edge back to temporary ownership. The
    /// caller releases/transfers that retain after dropping this target leaf.
    pub fn disconnect(node: *Node, strong: bool) void {
        const self = node.target orelse return;
        const flags = self.leaf.lock();
        defer self.leaf.unlock(flags);
        if (node.previous) |previous| previous.next = node.next else self.head = node.next;
        if (node.next) |next| next.previous = node.previous;
        self.count -= 1;
        if (strong) self.temporary += 1;
        node.target = null;
        node.previous = null;
        node.next = null;
    }

    pub fn update(node: *Node, identity: Identity, range: Range) void {
        const self = node.target orelse unreachable;
        const flags = self.leaf.lock();
        defer self.leaf.unlock(flags);
        node.identity = identity;
        node.range = range;
    }

    /// Scalar snapshot only. Source manipulation needs a retain/pin and a
    /// second identity check under the source's outer exclusion, never a
    /// borrowed Node pointer after this leaf is released.
    pub fn ownership(self: *Root) struct { edges: u32, temporary: u32 } {
        const flags = self.leaf.lock();
        defer self.leaf.unlock(flags);
        return .{ .edges = self.count, .temporary = self.temporary };
    }

    /// Inspection only, with all relevant source writers already excluded.
    /// O(n²) duplicate detection belongs exclusively to validation builds.
    pub fn validate(self: *Root, ref_count: ?u32) error{ BrokenLink, Duplicate, Count, Ownership }!void {
        const flags = self.leaf.lock();
        defer self.leaf.unlock(flags);
        var cursor = self.head;
        var previous: ?*Node = null;
        var count: u32 = 0;
        while (cursor) |node| : (cursor = node.next) {
            if (count >= self.count) return error.Count;
            if (node.target != self or node.previous != previous) return error.BrokenLink;
            var other = self.head;
            var scanned: u32 = 0;
            while (other) |candidate| : (other = candidate.next) {
                if (candidate == node) break;
                if (scanned >= self.count) return error.Count;
                if (candidate.identity.eql(node.identity)) return error.Duplicate;
                scanned += 1;
            }
            previous = node;
            count += 1;
        }
        if (count != self.count) return error.Count;
        if (ref_count) |expected| if (@as(u64, self.count) + self.temporary != expected) return error.Ownership;
    }
};

test "reverse edges consume temporary ownership and unlink without allocation" {
    var pool: Pool = .{};
    defer pool.deinit();
    var root: Root = .{};
    const a = try pool.reserve(slots.hostTestAllocator());
    const b = try pool.reserve(slots.hostTestAllocator());
    root.retainTemporary();
    root.retainTemporary();
    root.connect(a, .{ .index = 1, .generation = 1 }, .{}, true);
    root.connect(b, .{ .index = 2, .generation = 1 }, .{}, true);
    try root.validate(2);
    Root.disconnect(a, true);
    root.releaseTemporary();
    Pool.cancel(a);
    try root.validate(1);
    Root.disconnect(b, true);
    root.releaseTemporary();
    Pool.cancel(b);
    try root.validate(0);
    pool.reclaim();
}

test "reverse validation rejects missing duplicate and stale ownership" {
    var root: Root = .{};
    var a: Node = .{};
    var b: Node = .{};
    root.retainTemporary();
    root.retainTemporary();
    root.connect(&a, .{ .index = 1, .generation = 1 }, .{}, true);
    root.connect(&b, .{ .index = 1, .generation = 1 }, .{}, true);
    try std.testing.expectError(error.Duplicate, root.validate(2));
    Root.update(&b, .{ .index = 1, .generation = 2 }, .{});
    try root.validate(2);
    try std.testing.expectError(error.Ownership, root.validate(3));
    root.count = 1;
    try std.testing.expectError(error.Count, root.validate(2));
    root.count = 2;
    Root.disconnect(&a, true);
    Root.disconnect(&b, true);
    root.releaseTemporary();
    root.releaseTemporary();
    try root.validate(0);
}

pub var visits: if (@import("builtin").is_test) usize else void = if (@import("builtin").is_test) 0 else {};
pub const Edge = struct { identity: Identity, range: Range };
// The source transaction (K/all-AS, or the owning AS for a read) excludes
// publishers and preserves the cursor's successor. Only the returned edge may
// be withdrawn during iteration. Never retain a cursor across dropping that
// outer transaction. Source lookup/callbacks happen AFTER next releases leaf.
pub const Cursor = struct {
    root: ?*Root = null,
    pending: ?*Node = null,
    pub fn init(root: *Root) Cursor {
        const flags = root.leaf.lock();
        defer root.leaf.unlock(flags);
        return .{ .root = root, .pending = root.head };
    }
    pub fn after(root: *Root, node: *const Node) Cursor {
        const flags = root.leaf.lock();
        defer root.leaf.unlock(flags);
        std.debug.assert(node.target == root);
        return .{ .root = root, .pending = node.next };
    }
    pub fn next(self: *Cursor) ?Edge {
        const node = self.pending orelse return null;
        const root = self.root orelse return null;
        const flags = root.leaf.lock();
        defer root.leaf.unlock(flags);
        std.debug.assert(node.target == root);
        self.pending = node.next;
        if (@import("builtin").is_test) visits += 1;
        return .{ .identity = node.identity, .range = node.range };
    }
};
pub fn first(root: *Root, kind: Kind) ?Edge {
    var cursor = Cursor.init(root);
    while (cursor.next()) |edge| if (edge.identity.kind == kind) return edge;
    return null;
}
pub fn overlap(a: u64, n: u64, b: u64, m: u64) bool {
    if (n == 0 or m == 0) return false;
    const ae = std.math.add(u64, a, n) catch return true;
    const be = std.math.add(u64, b, m) catch return true;
    return a < be and b < ae;
}
pub fn backingCarrier(root: *Root, first_page: usize, pages: usize) bool {
    const flags = root.leaf.lock();
    defer root.leaf.unlock(flags);
    if (root.temporary != 0) return true;
    var cursor = root.head;
    while (cursor) |node| : (cursor = node.next) {
        if (@import("builtin").is_test) visits += 1;
        switch (node.identity.kind) {
            .child, .fd_object => return true,
            .vma => if (overlap(first_page * 4096, pages * 4096, node.range.offset, node.range.size)) return true,
            else => {},
        }
    }
    return false;
}
