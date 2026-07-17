const std = @import("std");
const verified = @import("verified_sched.zig");

pub const Ownership = enum(u8) {
    free,
    blocked,
    runnable,
    running,
    dead,
};

/// Scheduler nodes are allocated with the thread slot and never move while the
/// slot is live.  The runqueue only links these nodes; insert/remove/pick never
/// allocate and therefore remain usable from wake and interrupt paths.
pub const Node = struct {
    thread_index: usize = 0,
    generation: u32 = 1,
    cpu_slot: usize = 0,
    ownership: Ownership = .free,
    entity: verified.Entity = emptyEntity(),

    left: ?*Node = null,
    right: ?*Node = null,
    parent: ?*Node = null,
    linked_tree: ?*Tree = null,
    priority: u64 = 0,
    subtree_min_eligible: i64 = std.math.maxInt(i64),
    subtree_min_vruntime: i64 = std.math.maxInt(i64),
    subtree_count: usize = 1,

    pub fn reset(self: *Node, thread_index: usize, generation: u32) void {
        self.* = .{
            .thread_index = thread_index,
            .generation = generation,
            .priority = nodePriority(thread_index, generation),
        };
    }
};

pub const Tree = struct {
    root: ?*Node = null,
    count: usize = 0,

    pub fn insert(self: *Tree, node: *Node) bool {
        if (node.linked_tree != null or node.parent != null or node.left != null or node.right != null) return false;
        if (node.ownership != .runnable or node.entity.state != .runnable) return false;
        node.subtree_min_eligible = node.entity.eligible_time;
        node.subtree_min_vruntime = node.entity.vruntime;
        node.subtree_count = 1;

        if (self.root == null) {
            node.linked_tree = self;
            self.root = node;
            self.count = 1;
            return true;
        }

        var parent = self.root.?;
        while (true) {
            if (keyLess(node, parent)) {
                if (parent.left) |left| {
                    parent = left;
                } else {
                    parent.left = node;
                    node.parent = parent;
                    break;
                }
            } else {
                if (sameKey(node, parent)) return false;
                if (parent.right) |right| {
                    parent = right;
                } else {
                    parent.right = node;
                    node.parent = parent;
                    break;
                }
            }
        }
        self.count += 1;
        node.linked_tree = self;
        refreshAncestors(node.parent);
        while (node.parent) |parent_node| {
            if (parent_node.priority <= node.priority) break;
            if (parent_node.left == node) {
                self.rotateRight(parent_node);
            } else {
                self.rotateLeft(parent_node);
            }
        }
        refreshAncestors(node);
        return true;
    }

    pub fn remove(self: *Tree, node: *Node) bool {
        if (!self.contains(node)) return false;
        while (node.left != null and node.right != null) {
            if (node.left.?.priority <= node.right.?.priority) {
                self.rotateRight(node);
            } else {
                self.rotateLeft(node);
            }
        }
        const replacement = node.left orelse node.right;
        const parent = node.parent;
        self.replaceAtParent(node, replacement);
        node.left = null;
        node.right = null;
        node.parent = null;
        node.linked_tree = null;
        node.subtree_min_eligible = node.entity.eligible_time;
        node.subtree_min_vruntime = node.entity.vruntime;
        node.subtree_count = 1;
        self.count -= 1;
        refreshAncestors(parent);
        return true;
    }

    /// Return the earliest-deadline entity whose eligibility boundary has
    /// passed.  The min-eligible augmentation prunes whole ineligible subtrees.
    pub fn bestEligible(self: *const Tree, virtual_time: i64) ?*Node {
        var cursor = self.root;
        while (cursor) |node| {
            if (node.left) |left| {
                if (left.subtree_min_eligible <= virtual_time) {
                    cursor = left;
                    continue;
                }
            }
            if (node.entity.eligible_time <= virtual_time) return node;
            if (node.right) |right| {
                if (right.subtree_min_eligible <= virtual_time) {
                    cursor = right;
                    continue;
                }
            }
            return null;
        }
        return null;
    }

    pub fn minimumEligibleTime(self: *const Tree) ?i64 {
        return if (self.root) |root| root.subtree_min_eligible else null;
    }

    pub fn minimumVruntime(self: *const Tree) ?i64 {
        return if (self.root) |root| root.subtree_min_vruntime else null;
    }

    pub fn firstByDeadline(self: *const Tree) ?*Node {
        var cursor = self.root orelse return null;
        while (cursor.left) |left| cursor = left;
        return cursor;
    }

    pub fn nextByDeadline(self: *const Tree, node: *const Node) ?*Node {
        if (!self.contains(node)) return null;
        if (node.right) |right| {
            var cursor = right;
            while (cursor.left) |left| cursor = left;
            return cursor;
        }
        var cursor = node;
        while (cursor.parent) |parent| {
            if (parent.left == cursor) return parent;
            cursor = parent;
        }
        return null;
    }

    pub fn contains(self: *const Tree, node: *const Node) bool {
        return node.linked_tree == self;
    }

    pub fn validate(self: *const Tree, cpu_slot: usize) bool {
        const summary = validateNode(self.root, null, null, cpu_slot) orelse return false;
        return summary.count == self.count;
    }

    fn replaceAtParent(self: *Tree, old: *Node, replacement: ?*Node) void {
        if (old.parent) |parent| {
            if (parent.left == old) {
                parent.left = replacement;
            } else {
                parent.right = replacement;
            }
        } else {
            self.root = replacement;
        }
        if (replacement) |child| child.parent = old.parent;
    }

    fn rotateLeft(self: *Tree, pivot: *Node) void {
        const child = pivot.right orelse return;
        pivot.right = child.left;
        if (child.left) |middle| middle.parent = pivot;
        self.replaceAtParent(pivot, child);
        child.left = pivot;
        pivot.parent = child;
        refresh(pivot);
        refresh(child);
    }

    fn rotateRight(self: *Tree, pivot: *Node) void {
        const child = pivot.left orelse return;
        pivot.left = child.right;
        if (child.right) |middle| middle.parent = pivot;
        self.replaceAtParent(pivot, child);
        child.right = pivot;
        pivot.parent = child;
        refresh(pivot);
        refresh(child);
    }
};

const ValidationSummary = struct {
    count: usize,
    min_eligible: i64,
    min_vruntime: i64,
};

fn validateNode(node_opt: ?*Node, lower: ?*const Node, upper: ?*const Node, cpu_slot: usize) ?ValidationSummary {
    const node = node_opt orelse return .{
        .count = 0,
        .min_eligible = std.math.maxInt(i64),
        .min_vruntime = std.math.maxInt(i64),
    };
    if (node.ownership != .runnable or node.entity.state != .runnable) return null;
    if (node.cpu_slot != cpu_slot) return null;
    if (node.generation != @as(u32, @intCast(node.entity.generation))) return null;
    if (verified.pacha_eevdf_entity_validate(&node.entity) != .ok) return null;
    if (lower) |bound| if (!keyLess(bound, node)) return null;
    if (upper) |bound| if (!keyLess(node, bound)) return null;
    if (node.left) |left| {
        if (left.parent != node or left.priority < node.priority) return null;
    }
    if (node.right) |right| {
        if (right.parent != node or right.priority < node.priority) return null;
    }
    const left = validateNode(node.left, lower, node, cpu_slot) orelse return null;
    const right = validateNode(node.right, node, upper, cpu_slot) orelse return null;
    const count = left.count + right.count + 1;
    const min_eligible = @min(node.entity.eligible_time, @min(left.min_eligible, right.min_eligible));
    const min_vruntime = @min(node.entity.vruntime, @min(left.min_vruntime, right.min_vruntime));
    if (node.subtree_count != count or
        node.subtree_min_eligible != min_eligible or
        node.subtree_min_vruntime != min_vruntime)
    {
        return null;
    }
    return .{ .count = count, .min_eligible = min_eligible, .min_vruntime = min_vruntime };
}

fn refreshAncestors(start: ?*Node) void {
    var cursor = start;
    while (cursor) |node| {
        refresh(node);
        cursor = node.parent;
    }
}

fn refresh(node: *Node) void {
    node.subtree_count = 1;
    node.subtree_min_eligible = node.entity.eligible_time;
    node.subtree_min_vruntime = node.entity.vruntime;
    if (node.left) |left| {
        node.subtree_count += left.subtree_count;
        node.subtree_min_eligible = @min(node.subtree_min_eligible, left.subtree_min_eligible);
        node.subtree_min_vruntime = @min(node.subtree_min_vruntime, left.subtree_min_vruntime);
    }
    if (node.right) |right| {
        node.subtree_count += right.subtree_count;
        node.subtree_min_eligible = @min(node.subtree_min_eligible, right.subtree_min_eligible);
        node.subtree_min_vruntime = @min(node.subtree_min_vruntime, right.subtree_min_vruntime);
    }
}

fn keyLess(a: *const Node, b: *const Node) bool {
    if (a.entity.deadline != b.entity.deadline) return a.entity.deadline < b.entity.deadline;
    return a.entity.thread_id < b.entity.thread_id;
}

fn sameKey(a: *const Node, b: *const Node) bool {
    return a.entity.deadline == b.entity.deadline and a.entity.thread_id == b.entity.thread_id;
}

fn nodePriority(thread_index: usize, generation: u32) u64 {
    var value = @as(u64, @intCast(thread_index)) ^ (@as(u64, generation) << 32);
    value +%= 0x9e3779b97f4a7c15;
    value = (value ^ (value >> 30)) *% 0xbf58476d1ce4e5b9;
    value = (value ^ (value >> 27)) *% 0x94d049bb133111eb;
    return value ^ (value >> 31);
}

fn emptyEntity() verified.Entity {
    return .{
        .thread_id = 0,
        .generation = 0,
        .weight = 0,
        .slice_ns = 0,
        .service_ns = 0,
        .vruntime = 0,
        .eligible_time = 0,
        .deadline = 0,
        .state = .empty,
    };
}

test "intrusive tree picks eligible deadline and unlinks without allocation" {
    var nodes: [4]Node = undefined;
    var tree: Tree = .{};
    const deadlines = [_]i64{ 40, 60, 30, 20 };
    const eligible = [_]i64{ 0, 50, 0, 0 };
    for (&nodes, 0..) |*node, i| {
        node.reset(i, 1);
        node.ownership = .runnable;
        node.entity = emptyEntity();
        node.entity.thread_id = @intCast(i + 1);
        node.entity.generation = 1;
        node.entity.weight = 1024;
        node.entity.slice_ns = deadlines[i] - eligible[i];
        node.entity.deadline = deadlines[i];
        node.entity.eligible_time = eligible[i];
        node.entity.state = .runnable;
        try std.testing.expect(tree.insert(node));
    }
    try std.testing.expectEqual(@as(i64, 20), tree.bestEligible(0).?.entity.deadline);
    try std.testing.expectEqual(@as(i64, 20), tree.firstByDeadline().?.entity.deadline);
    try std.testing.expectEqual(@as(i64, 30), tree.nextByDeadline(&nodes[3]).?.entity.deadline);
    try std.testing.expect(tree.validate(0));
    try std.testing.expect(tree.remove(&nodes[3]));
    try std.testing.expectEqual(@as(i64, 30), tree.bestEligible(0).?.entity.deadline);
    try std.testing.expect(tree.validate(0));
}

test "intrusive tree exceeds the retired 256 entity capacity" {
    var nodes: [300]Node = undefined;
    var tree: Tree = .{};
    for (&nodes, 0..) |*node, i| {
        node.reset(i, 7);
        node.ownership = .runnable;
        node.entity = emptyEntity();
        node.entity.thread_id = @intCast(i + 1);
        node.entity.generation = 7;
        node.entity.weight = 1024;
        node.entity.slice_ns = @intCast(i + 1);
        node.entity.deadline = @intCast(i + 1);
        node.entity.state = .runnable;
        try std.testing.expect(tree.insert(node));
    }
    try std.testing.expectEqual(@as(usize, 300), tree.count);
    try std.testing.expect(tree.validate(0));
    for (&nodes) |*node| try std.testing.expect(tree.remove(node));
    try std.testing.expectEqual(@as(usize, 0), tree.count);
}
