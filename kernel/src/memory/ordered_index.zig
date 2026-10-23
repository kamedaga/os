//! Intrusive red-black tree with owner-defined handles and augmentation.
//! Handles may be pointers or physical addresses. Resolved pointers are never
//! retained across calls; the owner supplies exclusion and descriptor lifetime.
const std = @import("std");
const testing = @import("builtin").is_test;
pub fn Index(comptime A: type) type {
    return struct {
        const Self = @This();
        const H = A.Handle;
        const Key = A.Key;
        const tracked = testing or (if (@hasDecl(A, "track")) A.track else false);
        maintenance: if (tracked) [4]u64 else void = if (tracked) @splat(0) else {},
        root: ?H = null,
        first: ?H = null,
        count: usize = 0,
        tree_steps: if (testing) usize else void = if (testing) 0 else {},
        comparisons: if (testing) usize else void = if (testing) 0 else {},
        fn node(h: H) *A.Node {
            return A.resolve(h);
        }
        fn visit(self: *Self, kind: usize) void {
            if (comptime tracked) self.maintenance[kind] +%= 1;
        }
        fn less(self: *Self, a: Key, b: Key) bool {
            if (testing) self.comparisons += 1;
            return A.less(a, b);
        }
        fn red(h: ?H) bool {
            return if (h) |n| node(n).red else false;
        }
        pub fn minimum(self: *Self, start: H) H {
            var n = start;
            while (node(n).child[0]) |next| {
                self.visit(0);
                if (testing) self.tree_steps += 1;
                n = next;
            }
            return n;
        }
        /// The owner must keep the tree stable until the returned handle is used.
        pub fn lastAtOrBefore(self: *Self, key: Key) ?H {
            var cursor = self.root;
            var result: ?H = null;
            while (cursor) |n| {
                if (testing) self.tree_steps += 1;
                if (!self.less(key, node(n).key)) {
                    result = n;
                    cursor = node(n).child[1];
                } else cursor = node(n).child[0];
            }
            return result;
        }
        /// Parent links avoid another root search for each item in a stable walk.
        /// No cursor may survive an insertion, removal or key change.
        pub fn successor(self: *Self, n: H) ?H {
            std.debug.assert(node(n).linked);
            if (node(n).child[1]) |right| return self.minimum(right);
            var child = n;
            var parent = node(child).parent;
            while (parent) |p| {
                if (testing) self.tree_steps += 1;
                if (node(p).child[0] == child) return p;
                child = p;
                parent = node(p).parent;
            }
            return null;
        }
        fn side(n: H) usize {
            return @intFromBool(node(node(n).parent.?).child[1] == n);
        }
        fn replace(self: *Self, old: H, new: ?H) void {
            if (node(old).parent) |p| node(p).child[side(old)] = new else self.root = new;
            if (new) |n| node(n).parent = node(old).parent;
        }
        pub fn refresh(self: *Self, start: ?H) void {
            if (comptime @hasDecl(A, "augmented")) {
                if (!A.augmented) return;
            }
            var cursor = start;
            while (cursor) |n| {
                self.visit(1);
                A.update(n);
                cursor = node(n).parent;
            }
        }
        fn rotate(self: *Self, n: H, direction: usize) void {
            self.visit(2);
            const up = node(n).child[1 - direction].?;
            node(n).child[1 - direction] = node(up).child[direction];
            if (node(up).child[direction]) |c| node(c).parent = n;
            self.replace(n, up);
            node(up).child[direction] = n;
            node(n).parent = up;
            // Children change before summaries. Ancestors describe the same
            // set after rotation and therefore need no propagation.
            A.update(n);
            A.update(up);
        }
        pub fn insert(self: *Self, n: H, key: Key) void {
            std.debug.assert(!node(n).linked);
            node(n).* = .{ .key = key, .red = true, .linked = true };
            var parent: ?H = null;
            var cursor = self.root;
            var direction: usize = 0;
            while (cursor) |p| {
                self.visit(0);
                std.debug.assert(!std.meta.eql(key, node(p).key));
                parent = p;
                direction = @intFromBool(!self.less(key, node(p).key));
                cursor = node(p).child[direction];
            }
            if (parent) |p| node(p).child[direction] = n else self.root = n;
            node(n).parent = parent;
            if (self.first == null or self.less(key, node(self.first.?).key)) self.first = n;
            self.count += 1;
            self.refresh(n);
            var x = n;
            while (red(node(x).parent)) {
                self.visit(3);
                const p = node(x).parent.?;
                const g = node(p).parent.?;
                const d = side(p);
                const uncle = node(g).child[1 - d];
                if (red(uncle)) {
                    node(p).red = false;
                    node(uncle.?).red = false;
                    node(g).red = true;
                    x = g;
                } else {
                    if (side(x) != d) {
                        x = p;
                        self.rotate(x, d);
                    }
                    node(node(x).parent.?).red = false;
                    const grand = node(node(x).parent.?).parent.?;
                    node(grand).red = true;
                    self.rotate(grand, 1 - d);
                }
            }
            node(self.root.?).red = false;
        }
        pub fn remove(self: *Self, z: H) void {
            std.debug.assert(node(z).linked);
            var y = z;
            var was_red = node(y).red;
            var x: ?H = null;
            var parent: ?H = null;
            var direction: usize = 0;
            if (node(z).child[0] == null or node(z).child[1] == null) {
                x = node(z).child[if (node(z).child[0] == null) @as(usize, 1) else 0];
                parent = node(z).parent;
                if (parent != null) direction = side(z);
                self.replace(z, x);
            } else {
                y = self.minimum(node(z).child[1].?);
                was_red = node(y).red;
                x = node(y).child[1];
                if (node(y).parent == z) {
                    parent = y;
                    direction = 1;
                    if (x) |c| node(c).parent = y;
                } else {
                    parent = node(y).parent;
                    direction = side(y);
                    self.replace(y, x);
                    node(y).child[1] = node(z).child[1];
                    node(node(y).child[1].?).parent = y;
                }
                self.replace(z, y);
                node(y).child[0] = node(z).child[0];
                node(node(y).child[0].?).parent = y;
                node(y).red = node(z).red;
            }
            // The old successor-parent chain now passes through the promoted
            // successor. Recompute it before deletion repair rotates nodes.
            self.refresh(parent);
            if (!was_red) self.repairDelete(x, parent, direction);
            node(z).* = .{};
            self.count -= 1;
            self.first = if (self.root) |r| self.minimum(r) else null;
        }
        fn repairDelete(self: *Self, initial: ?H, initial_parent: ?H, initial_direction: usize) void {
            var x = initial;
            var parent = initial_parent;
            var d = initial_direction;
            while (x != self.root and !red(x)) {
                self.visit(3);
                if (testing) self.tree_steps += 1;
                const p = parent.?;
                var sibling = node(p).child[1 - d].?;
                if (node(sibling).red) {
                    node(sibling).red = false;
                    node(p).red = true;
                    self.rotate(p, d);
                    sibling = node(p).child[1 - d].?;
                }
                if (!red(node(sibling).child[0]) and !red(node(sibling).child[1])) {
                    node(sibling).red = true;
                    x = p;
                    parent = node(p).parent;
                    if (parent != null) d = side(p);
                } else {
                    if (!red(node(sibling).child[1 - d])) {
                        node(node(sibling).child[d].?).red = false;
                        node(sibling).red = true;
                        self.rotate(sibling, 1 - d);
                        sibling = node(p).child[1 - d].?;
                    }
                    node(sibling).red = node(p).red;
                    node(p).red = false;
                    node(node(sibling).child[1 - d].?).red = false;
                    self.rotate(p, d);
                    x = self.root;
                }
            }
            if (x) |n| node(n).red = false;
        }
        pub fn dueAfter(self: *Self, now: u64, after: ?Key) ?H {
            const first = self.first orelse return null;
            if (!A.due(node(first).key, now)) return null;
            if (after == null) return first;
            var cursor = self.root;
            var result: ?H = null;
            while (cursor) |p| {
                if (self.less(after.?, node(p).key)) {
                    result = p;
                    cursor = node(p).child[0];
                } else cursor = node(p).child[1];
            }
            const found = result orelse return null;
            return if (A.due(node(found).key, now)) found else null;
        }
        pub fn validate(self: *const Self) !void {
            if (!testing) @compileError("host-only tree validation");
            if (self.root) |r| {
                try std.testing.expect(node(r).parent == null and !node(r).red);
                var seen: usize = 0;
                _ = try validateNode(r, null, null, &seen);
                try std.testing.expectEqual(self.count, seen);
                var first = r;
                while (node(first).child[0]) |next| first = next;
                try std.testing.expect(self.first == first);
            } else try std.testing.expect(self.count == 0 and self.first == null);
        }
        fn validateNode(n: H, lo: ?Key, hi: ?Key, seen: *usize) anyerror!usize {
            try std.testing.expect(node(n).linked);
            if (lo) |k| try std.testing.expect(A.less(k, node(n).key));
            if (hi) |k| try std.testing.expect(A.less(node(n).key, k));
            if (node(n).red) try std.testing.expect(!red(node(n).child[0]) and !red(node(n).child[1]));
            var heights = [2]usize{ 1, 1 };
            for (node(n).child, 0..) |c, d| if (c) |p| {
                try std.testing.expect(node(p).parent == n);
                heights[d] = try validateNode(p, if (d == 0) lo else node(n).key, if (d == 0) node(n).key else hi, seen);
            };
            try std.testing.expectEqual(heights[0], heights[1]);
            try A.validate(n);
            seen.* += 1;
            return heights[0] + @intFromBool(!node(n).red);
        }
    };
}

const TestAdapter = struct {
    pub const Key = usize;
    pub const Node = struct {
        key: usize = 0,
        parent: ?*@This() = null,
        child: [2]?*@This() = .{ null, null },
        red: bool = false,
        linked: bool = false,
    };
    pub const Handle = *Node;
    pub const augmented = false;
    pub fn resolve(h: Handle) *Node {
        return h;
    }
    pub fn less(a: Key, b: Key) bool {
        return a < b;
    }
    pub fn update(_: Handle) void {}
    pub fn validate(_: Handle) !void {}
};

test "ordered index floor and successor after random rotations and deletion fixups" {
    var index: Index(TestAdapter) = .{};
    var nodes: [257]TestAdapter.Node = @splat(.{});
    var rng = std.Random.DefaultPrng.init(0x766d615f696478);
    for (0..6000) |_| {
        const slot = rng.random().uintLessThan(usize, nodes.len);
        if (nodes[slot].linked) index.remove(&nodes[slot]) else index.insert(&nodes[slot], slot * 2 + 1);
        try index.validate();
        var cursor = index.first;
        var previous: ?*TestAdapter.Node = null;
        for (&nodes, 0..) |*n, i| {
            const steps = index.tree_steps;
            try std.testing.expectEqual(previous, index.lastAtOrBefore(i * 2));
            try std.testing.expect(index.tree_steps - steps <= 2 * (std.math.log2_int(usize, index.count + 1) + 1));
            if (n.linked) {
                try std.testing.expectEqual(n, cursor.?);
                cursor = index.successor(cursor.?);
                previous = n;
            }
            try std.testing.expectEqual(previous, index.lastAtOrBefore(i * 2 + 1));
        }
        try std.testing.expect(cursor == null);
        try std.testing.expectEqual(previous, index.lastAtOrBefore(std.math.maxInt(usize)));
    }
    for (&nodes) |*n| if (n.linked) {
        index.remove(n);
    };
    try index.validate();
    try std.testing.expect(index.lastAtOrBefore(0) == null);
}
