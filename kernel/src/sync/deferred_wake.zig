//! Wake edges published while a CPU owns the KernelState lock K.
//!
//! Why: an x2APIC ICR write is a VM exit, and under nested virtualization
//! (KVM inside Hyper-V/WSL2) a single exit costs tens of microseconds. K is a
//! global lock, so every exit taken while holding it stalls all other CPUs
//! that need K. A K owner therefore only records the target CPU here and the
//! edge is sent right after the release store of K.
//!
//! Wake-loss argument: every runnable publication happens under K before the
//! release store, and the recorded edge is sent after it, so the edge always
//! follows the last publication of this critical section. A target that
//! consumes an earlier edge and re-checks its queue before HLT either sees the
//! publication or receives this later edge (sti;hlt keeps a pending vector).
//! Coalescing is limited to one owner's critical section for the same reason.
//!
//! Each row belongs to one owner CPU. The owner and an interrupt handler
//! nested on that same CPU are the only writers, so row updates are atomic
//! RMWs but need no cross-CPU ordering beyond the K release itself.
const std = @import("std");

pub const Table = struct {
    capacity: usize = 0,
    words_per_cpu: usize = 0,
    wakes: []usize = &.{},
    notifies: []usize = &.{},
    active: []u8 = &.{},

    const bits = @bitSizeOf(usize);

    pub fn wordsPerCpu(capacity: usize) usize {
        return capacity / bits + @intFromBool(capacity % bits != 0);
    }

    pub fn init(wakes: []usize, notifies: []usize, active: []u8, capacity: usize) Table {
        const words = wordsPerCpu(capacity);
        std.debug.assert(wakes.len == capacity * words and notifies.len == capacity * words);
        std.debug.assert(active.len == capacity);
        @memset(wakes, 0);
        @memset(notifies, 0);
        @memset(active, 0);
        return .{ .capacity = capacity, .words_per_cpu = words, .wakes = wakes, .notifies = notifies, .active = active };
    }

    fn row(self: *const Table, words: []usize, owner: usize) []usize {
        return words[owner * self.words_per_cpu ..][0..self.words_per_cpu];
    }

    /// `owner` has just acquired K. K is not recursive, so a still-active row
    /// means an earlier release skipped `end` and its edges were lost.
    pub fn begin(self: *Table, owner: usize) void {
        if (owner >= self.capacity) return;
        std.debug.assert(@atomicLoad(u8, &self.active[owner], .monotonic) == 0);
        @atomicStore(u8, &self.active[owner], 1, .monotonic);
    }

    /// True when the edge to `target` will be sent by `owner`'s `end`.
    /// False means the caller is not inside a K critical section and must
    /// send the edge itself.
    pub fn record(self: *Table, owner: usize, target: usize, notify: bool) bool {
        if (owner >= self.capacity or target >= self.capacity) return false;
        if (@atomicLoad(u8, &self.active[owner], .monotonic) == 0) return false;
        const mask = @as(usize, 1) << @intCast(target % bits);
        _ = @atomicRmw(usize, &self.row(self.wakes, owner)[target / bits], .Or, mask, .monotonic);
        if (notify) _ = @atomicRmw(usize, &self.row(self.notifies, owner)[target / bits], .Or, mask, .monotonic);
        return true;
    }

    /// `owner` has released K. Sends each recorded target exactly once and
    /// returns the first NOTIFY target whose edge failed: a later publisher
    /// may already have skipped its own edge because the token was pending,
    /// so the caller must not continue with that wake lost.
    pub fn end(self: *Table, owner: usize, context: anytype, comptime send: fn (@TypeOf(context), usize) bool) ?usize {
        if (owner >= self.capacity) return null;
        if (@atomicLoad(u8, &self.active[owner], .monotonic) == 0) return null;
        // Clear first: an edge requested from here on is outside K and is
        // sent directly by its caller instead of waiting for a later release.
        @atomicStore(u8, &self.active[owner], 0, .monotonic);
        const wakes = self.row(self.wakes, owner);
        const notifies = self.row(self.notifies, owner);
        var failed_notify: ?usize = null;
        for (wakes, notifies, 0..) |*wake_word, *notify_word, word_index| {
            var pending = @atomicRmw(usize, wake_word, .Xchg, 0, .monotonic);
            const notify_bits = @atomicRmw(usize, notify_word, .Xchg, 0, .monotonic);
            while (pending != 0) {
                const bit: std.math.Log2Int(usize) = @intCast(@ctz(pending));
                pending &= pending - 1;
                const target = word_index * bits + bit;
                if (send(context, target)) continue;
                if (failed_notify == null and notify_bits & (@as(usize, 1) << bit) != 0) failed_notify = target;
            }
        }
        return failed_notify;
    }
};

const Recorder = struct {
    sent: [4]usize = @splat(0),
    fail: ?usize = null,
    fn send(self: *Recorder, target: usize) bool {
        self.sent[target] += 1;
        return self.fail != target;
    }
};

fn testTable(wakes: []usize, notifies: []usize, active: []u8) Table {
    return Table.init(wakes, notifies, active, 4);
}

test "edges outside K are not recorded" {
    var wakes: [4]usize = undefined;
    var notifies: [4]usize = undefined;
    var active: [4]u8 = undefined;
    var table = testTable(&wakes, &notifies, &active);
    try std.testing.expect(!table.record(0, 2, false));
    var recorder: Recorder = .{};
    try std.testing.expectEqual(@as(?usize, null), table.end(0, &recorder, Recorder.send));
    try std.testing.expectEqualSlices(usize, &.{ 0, 0, 0, 0 }, &recorder.sent);
}

test "edges inside K coalesce per target and are sent once at release" {
    var wakes: [4]usize = undefined;
    var notifies: [4]usize = undefined;
    var active: [4]u8 = undefined;
    var table = testTable(&wakes, &notifies, &active);
    table.begin(1);
    try std.testing.expect(table.record(1, 2, false));
    try std.testing.expect(table.record(1, 2, true));
    try std.testing.expect(table.record(1, 3, false));
    // Another owner's row is independent of this critical section.
    try std.testing.expect(!table.record(0, 3, false));
    var recorder: Recorder = .{};
    try std.testing.expectEqual(@as(?usize, null), table.end(1, &recorder, Recorder.send));
    try std.testing.expectEqualSlices(usize, &.{ 0, 0, 1, 1 }, &recorder.sent);
    // The release consumed the row; a second release sends nothing.
    try std.testing.expectEqual(@as(?usize, null), table.end(1, &recorder, Recorder.send));
    try std.testing.expectEqualSlices(usize, &.{ 0, 0, 1, 1 }, &recorder.sent);
    try std.testing.expect(!table.record(1, 2, false));
}

test "a later critical section needs its own edge" {
    var wakes: [4]usize = undefined;
    var notifies: [4]usize = undefined;
    var active: [4]u8 = undefined;
    var table = testTable(&wakes, &notifies, &active);
    var recorder: Recorder = .{};
    table.begin(0);
    try std.testing.expect(table.record(0, 1, false));
    _ = table.end(0, &recorder, Recorder.send);
    table.begin(0);
    try std.testing.expect(table.record(0, 1, false));
    _ = table.end(0, &recorder, Recorder.send);
    try std.testing.expectEqual(@as(usize, 2), recorder.sent[1]);
}

test "only a failed NOTIFY edge is reported" {
    var wakes: [4]usize = undefined;
    var notifies: [4]usize = undefined;
    var active: [4]u8 = undefined;
    var table = testTable(&wakes, &notifies, &active);
    var plain: Recorder = .{ .fail = 2 };
    table.begin(0);
    try std.testing.expect(table.record(0, 2, false));
    try std.testing.expectEqual(@as(?usize, null), table.end(0, &plain, Recorder.send));
    var notify: Recorder = .{ .fail = 3 };
    table.begin(0);
    try std.testing.expect(table.record(0, 1, true));
    try std.testing.expect(table.record(0, 3, true));
    try std.testing.expectEqual(@as(?usize, 3), table.end(0, &notify, Recorder.send));
    // The failure does not suppress the remaining edges of the section.
    try std.testing.expectEqual(@as(usize, 1), notify.sent[1]);
}
