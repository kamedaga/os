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
    selection_virtual_time: i64 = 0,
    selection_time: i64 = 0,
    selection: ?*Node = null,

    /// The scheduler publishes each virtual-time change while holding S.
    /// Keeping the exact winner here makes preferred-target admission a single
    /// identity comparison; no eligibility search is deferred to that check.
    pub fn setSelectionVirtualTime(self: *Tree, virtual_time: i64) void {
        if (self.selection_virtual_time == virtual_time) return;
        self.selection_virtual_time = virtual_time;
        self.refreshSelection();
    }

    pub fn cachedSelection(self: *const Tree) ?*Node {
        return self.selection;
    }

    /// Includes the ordinary scheduler's advance when no entity is eligible.
    pub fn cachedSelectionTime(self: *const Tree) i64 {
        return self.selection_time;
    }

    /// Source blocking raises the CPU floor to minimumVruntime. Every valid
    /// entity has vruntime <= eligible_time, so minimumVruntime cannot exceed
    /// minimumEligibleTime. The ordinary no-eligible advance already covers
    /// that floor: the cached winner/time are also the exact post-block view.
    pub fn cachedPostBlockSelection(self: *const Tree) ?*Node {
        return self.selection;
    }

    pub fn cachedPostBlockTime(self: *const Tree) i64 {
        return self.selection_time;
    }

    /// Read-only choice for a newly woken, still unlinked entity. S and the
    /// runqueue lock cover this decision through handoff commit. Inserting an
    /// earlier eligible entity can LOWER selection_time: then every old node
    /// is ineligible, so comparing only against the old winner is incorrect.
    pub fn directWakeSelectionTime(self: *const Tree, candidate: *const verified.Entity) ?i64 {
        return self.directWakeWithSourceSelectionTime(candidate, null);
    }

    /// NOTIFY keeps the sender runnable. Compare its charged/finished entity
    /// as an additional candidate without mutating the tree during preflight.
    pub fn directWakeWithSourceSelectionTime(self: *const Tree, candidate: *const verified.Entity, source: ?*const verified.Entity) ?i64 {
        if (candidate.state != .runnable) return null;
        var earliest = candidate.eligible_time;
        if (self.root != null) earliest = @min(earliest, self.selection_time);
        if (source) |sender| {
            if (sender.state != .runnable) return null;
            earliest = @min(earliest, sender.eligible_time);
        }
        const selected_time = @max(self.selection_virtual_time, earliest);
        if (candidate.eligible_time > selected_time) return null;
        if (self.selection) |old| {
            if (selected_time == self.selection_time and
                (candidate.deadline > old.entity.deadline or
                    (candidate.deadline == old.entity.deadline and candidate.thread_id >= old.entity.thread_id))) return null;
        }
        if (source) |sender| {
            if (sender.eligible_time <= selected_time and
                (candidate.deadline > sender.deadline or
                    (candidate.deadline == sender.deadline and candidate.thread_id >= sender.thread_id))) return null;
        }
        return selected_time;
    }

    /// A runnable hint already participates in the tree. Adding the finished
    /// sender can only keep or lower selection time; if this winner remains
    /// eligible, no other old candidate can overtake it at that earlier time.
    pub fn directRunnableWithSourceSelectionTime(self: *const Tree, candidate: *const Node, source: *const verified.Entity) ?i64 {
        if (candidate.linked_tree != self or candidate.ownership != .runnable or
            source.state != .runnable or self.selection != candidate) return null;
        const time = @max(self.selection_virtual_time, @min(self.selection_time, source.eligible_time));
        if (candidate.entity.eligible_time > time) return null;
        if (source.eligible_time <= time and
            (source.deadline < candidate.entity.deadline or
                (source.deadline == candidate.entity.deadline and source.thread_id <= candidate.entity.thread_id))) return null;
        return time;
    }

    fn refreshSelection(self: *Tree) void {
        self.selection_time = if (self.minimumEligibleTime()) |minimum|
            @max(self.selection_virtual_time, minimum)
        else
            self.selection_virtual_time;
        self.selection = self.bestEligible(self.selection_time);
    }

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
            self.refreshSelection();
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
        self.refreshSelection();
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
        self.refreshSelection();
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
        const effective_time = if (self.minimumEligibleTime()) |minimum|
            @max(self.selection_virtual_time, minimum)
        else
            self.selection_virtual_time;
        return summary.count == self.count and self.selection_time == effective_time and
            self.selection == self.bestEligible(effective_time);
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
    const correct = tree.selection;
    tree.selection = &nodes[0];
    try std.testing.expect(!tree.validate(0));
    tree.selection = correct;
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

test "cached EEVDF choice agrees after tree mutations and virtual time shifts" {
    // More than the retired capacity, with both forward and backward clock
    // shifts: eligibility is not a one-way partition during CPU migration.
    var nodes: [513]Node = undefined;
    var linked = [_]bool{false} ** nodes.len;
    var tree: Tree = .{};
    var random: u64 = 0x418fa2b163e74509;
    for (&nodes, 0..) |*node, i| node.reset(i, 9);
    for (0..12000) |step| {
        random = random *% 6364136223846793005 +% 1442695040888963407;
        const index: usize = @intCast((random >> 16) % nodes.len);
        const node = &nodes[index];
        if (linked[index]) {
            try std.testing.expect(tree.remove(node));
            linked[index] = false;
        } else {
            const eligible: i64 = @as(i64, @intCast((random >> 32) % 401));
            const slice: i64 = @intCast(1 + (random >> 48) % 100);
            node.ownership = .runnable;
            node.entity = emptyEntity();
            node.entity.thread_id = @intCast(index + 1);
            node.entity.generation = 9;
            node.entity.weight = 1024;
            node.entity.vruntime = eligible;
            node.entity.eligible_time = eligible;
            node.entity.slice_ns = slice;
            node.entity.deadline = eligible + slice;
            node.entity.state = .runnable;
            try std.testing.expect(tree.insert(node));
            linked[index] = true;
        }
        const virtual_time: i64 = if (step % 11 == 0) -300 else @as(i64, @intCast((random >> 20) % 701)) - 350;
        tree.setSelectionVirtualTime(virtual_time);
        const effective = @max(virtual_time, tree.minimumEligibleTime() orelse virtual_time);
        // Linear oracle does not share bestEligible's subtree augmentation.
        var expected: ?*Node = null;
        for (&nodes, linked) |*candidate, present| {
            if (!present or candidate.entity.eligible_time > effective) continue;
            if (expected == null or keyLess(candidate, expected.?)) expected = candidate;
        }
        try std.testing.expectEqual(expected, tree.cachedSelection());
        try std.testing.expectEqual(expected, tree.bestEligible(effective));
        try std.testing.expectEqual(effective, tree.cachedSelectionTime());
        const post_block_time = @max(effective, tree.minimumVruntime() orelse virtual_time);
        var post_block_expected: ?*Node = null;
        for (&nodes, linked) |*candidate, present| {
            if (!present or candidate.entity.eligible_time > post_block_time) continue;
            if (post_block_expected == null or keyLess(candidate, post_block_expected.?)) post_block_expected = candidate;
        }
        try std.testing.expectEqual(post_block_expected, tree.cachedPostBlockSelection());
        try std.testing.expectEqual(post_block_time, tree.cachedPostBlockTime());
        try std.testing.expect(tree.validate(0));
    }
    for (&nodes, linked) |*node, present| {
        if (present) try std.testing.expect(tree.remove(node));
    }
    try std.testing.expectEqual(@as(?*Node, null), tree.cachedSelection());
    try std.testing.expectEqual(tree.selection_virtual_time, tree.cachedSelectionTime());
}

// Compare the preflight against ordinary insertion and selection, including
// ties and a candidate that lowers the cached no-eligible advance.
test "direct wake preflight agrees with ordinary EEVDF insertion" {
    var nodes: [4]Node = undefined;
    var tree: Tree = .{};
    for (&nodes, 0..) |*node, i| {
        node.reset(i, 1);
        node.ownership = .runnable;
        node.entity = emptyEntity();
        node.entity.thread_id = @intCast(2 * i + 2);
        node.entity.state = .runnable;
        node.entity.eligible_time = @as(i64, @intCast(i)) * 20 - 10;
        node.entity.vruntime = node.entity.eligible_time;
        node.entity.deadline = node.entity.eligible_time + 30;
    }
    for (0..5) |count| {
        for (nodes[0..count]) |*node| try std.testing.expect(tree.insert(node));
        for (0..10) |clock| {
            tree.setSelectionVirtualTime(@as(i64, @intCast(clock)) * 15 - 50);
            for (0..12) |eligible| {
                for (0..12) |deadline| {
                    for (0..10) |id| {
                        var candidate: Node = .{};
                        candidate.reset(99, 1);
                        candidate.ownership = .runnable;
                        candidate.entity = emptyEntity();
                        candidate.entity.state = .runnable;
                        candidate.entity.thread_id = @intCast(id);
                        candidate.entity.eligible_time = @as(i64, @intCast(eligible)) * 10 - 40;
                        candidate.entity.vruntime = candidate.entity.eligible_time;
                        candidate.entity.deadline = candidate.entity.eligible_time + @as(i64, @intCast(deadline)) * 10 + 10;
                        var duplicate = false;
                        for (nodes[0..count]) |*node| duplicate = duplicate or sameKey(node, &candidate);
                        if (duplicate) continue;
                        const before_count = tree.count;
                        const before_time = tree.selection_time;
                        const before_selection = tree.selection;
                        const predicted = tree.directWakeSelectionTime(&candidate.entity);
                        try std.testing.expectEqual(before_count, tree.count);
                        try std.testing.expectEqual(before_time, tree.selection_time);
                        try std.testing.expectEqual(before_selection, tree.selection);
                        try std.testing.expect(tree.insert(&candidate));
                        const actual: ?i64 = if (tree.cachedPostBlockSelection() == &candidate) tree.cachedPostBlockTime() else null;
                        try std.testing.expectEqual(actual, predicted);
                        try std.testing.expect(tree.remove(&candidate));
                    }
                }
            }
        }
        for (nodes[0..count]) |*node| try std.testing.expect(tree.remove(node));
    }
}

test "notify direct wake with runnable sender agrees with ordinary insertion" {
    var tree: Tree = .{};
    var old: Node = .{};
    old.reset(2, 1);
    old.ownership = .runnable;
    old.entity = emptyEntity();
    old.entity.state = .runnable;
    old.entity.thread_id = 2;
    old.entity.eligible_time = 20;
    old.entity.deadline = 40;
    for (0..2) |populated| {
        if (populated != 0) try std.testing.expect(tree.insert(&old));
        for (0..8) |clock| {
            tree.setSelectionVirtualTime(@as(i64, @intCast(clock)) * 10 - 30);
            for (0..8) |sender_time| {
                for (0..8) |receiver_time| {
                    for (0..8) |deadline| {
                        var sender: Node = .{};
                        sender.reset(1, 1);
                        sender.ownership = .runnable;
                        sender.entity = emptyEntity();
                        sender.entity.state = .runnable;
                        sender.entity.thread_id = 1;
                        sender.entity.eligible_time = @as(i64, @intCast(sender_time)) * 10 - 20;
                        sender.entity.deadline = 40;
                        var receiver: Node = .{};
                        receiver.reset(3, 1);
                        receiver.ownership = .runnable;
                        receiver.entity = emptyEntity();
                        receiver.entity.state = .runnable;
                        receiver.entity.thread_id = 3;
                        receiver.entity.eligible_time = @as(i64, @intCast(receiver_time)) * 10 - 20;
                        receiver.entity.deadline = @as(i64, @intCast(deadline)) * 10;
                        const before = tree.count;
                        const predicted = tree.directWakeWithSourceSelectionTime(&receiver.entity, &sender.entity);
                        try std.testing.expectEqual(before, tree.count);
                        try std.testing.expect(tree.insert(&sender));
                        try std.testing.expect(tree.insert(&receiver));
                        const actual: ?i64 = if (tree.cachedSelection() == &receiver) tree.cachedSelectionTime() else null;
                        try std.testing.expectEqual(actual, predicted);
                        try std.testing.expect(tree.remove(&receiver));
                        try std.testing.expect(tree.remove(&sender));
                    }
                }
            }
        }
        if (populated != 0) try std.testing.expect(tree.remove(&old));
    }
}

test "notify runnable hint winner agrees with ordinary sender insertion" {
    var tree: Tree = .{};
    var hint: Node = .{};
    var source: Node = .{};
    var other: Node = .{};
    hint.reset(2, 1);
    source.reset(1, 1);
    other.reset(3, 1);
    inline for (.{ &hint, &source, &other }, .{ @as(i64, 2), @as(i64, 1), @as(i64, 3) }) |node, id| {
        node.ownership = .runnable;
        node.entity = emptyEntity();
        node.entity.state = .runnable;
        node.entity.thread_id = id;
    }
    other.entity.eligible_time = 20;
    other.entity.deadline = 40;
    for (0..2) |populated| {
        if (populated != 0) try std.testing.expect(tree.insert(&other));
        for (0..8) |clock| {
            tree.setSelectionVirtualTime(@as(i64, @intCast(clock)) * 10 - 30);
            for (0..8) |sender_time| {
                for (0..8) |hint_time| {
                    for (0..8) |deadline| {
                        source.entity.eligible_time = @as(i64, @intCast(sender_time)) * 10 - 30;
                        source.entity.deadline = 55;
                        hint.entity.eligible_time = @as(i64, @intCast(hint_time)) * 10 - 30;
                        hint.entity.deadline = @as(i64, @intCast(deadline)) * 10;
                        try std.testing.expect(tree.insert(&hint));
                        const before = tree.count;
                        const predicted = tree.directRunnableWithSourceSelectionTime(&hint, &source.entity);
                        try std.testing.expectEqual(before, tree.count);
                        try std.testing.expect(tree.insert(&source));
                        const actual: ?i64 = if (tree.cachedSelection() == &hint) tree.cachedSelectionTime() else null;
                        try std.testing.expectEqual(actual, predicted);
                        try std.testing.expect(tree.remove(&source));
                        try std.testing.expect(tree.remove(&hint));
                    }
                }
            }
        }
        if (populated != 0) try std.testing.expect(tree.remove(&other));
    }
}
