//! Intrusive deadline ordering. The owner supplies exclusion and stable node
//! lifetime; this module never allocates, locks, or invokes owner callbacks.
const std = @import("std");
pub const Key = struct {
    deadline: u64,
    id: u32,
    generation: u32,
    pub fn less(a: DeadlineKey, b: DeadlineKey) bool {
        if (a.deadline != b.deadline) return a.deadline < b.deadline;
        if (a.id != b.id) return a.id < b.id;
        return a.generation < b.generation;
    }
};
pub const Node = struct {
    parent: ?*Node = null,
    child: [2]?*Node = .{ null, null },
    key: Key = .{ .deadline = 0, .id = 0, .generation = 0 },
    red: bool = false,
    linked: bool = false,
};
const DeadlineNode = Node;
const DeadlineKey = Key;
pub const Index = @import("ordered_index.zig").Index(struct {
    pub const Handle = *DeadlineNode;
    pub const Node = DeadlineNode;
    pub const Key = DeadlineKey;
    pub const augmented = false;
    pub fn resolve(h: Handle) *DeadlineNode {
        return h;
    }
    pub fn less(a: DeadlineKey, b: DeadlineKey) bool {
        return DeadlineKey.less(a, b);
    }
    pub fn due(k: DeadlineKey, now: u64) bool {
        return k.deadline <= now;
    }
    pub fn update(_: Handle) void {}
    pub fn validate(_: Handle) !void {}
});

test "deadline index random cancellation reset and ordered simultaneous expiry" {
    var tree: Index = .{};
    var nodes: [257]Node = @splat(.{});
    var random = std.Random.DefaultPrng.init(0xdea11);
    for (0..10000) |_| {
        const id = random.random().uintLessThan(usize, nodes.len);
        const n = &nodes[id];
        if (n.linked) tree.remove(n) else tree.insert(n, .{ .deadline = random.random().uintLessThan(u64, 16), .id = @intCast(id), .generation = 3 });
        try tree.validate();
    }
    var last: ?Key = null;
    while (tree.dueAfter(100, last)) |n| {
        const key = n.key;
        if (last) |p| try std.testing.expect(Key.less(p, key));
        last = key;
        tree.remove(n);
        try tree.validate();
    }
    try std.testing.expectEqual(@as(usize, 0), tree.count);
    for (&nodes) |*n| try std.testing.expect(!n.linked);
}
