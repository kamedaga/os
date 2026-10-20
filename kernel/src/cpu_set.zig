//! Borrowed CPU membership storage. The boot topology owns its capacity;
//! neither the word width nor a diagnostic configuration limits CPU count.
const std = @import("std");

pub const CpuSet = struct {
    words: []usize,
    capacity: usize,
    const bits = @bitSizeOf(usize);

    pub fn wordCount(capacity: usize) usize {
        return capacity / bits + @intFromBool(capacity % bits != 0);
    }

    pub fn init(words: []usize, capacity: usize) CpuSet {
        std.debug.assert(words.len == wordCount(capacity));
        @memset(words, 0);
        return .{ .words = words, .capacity = capacity };
    }

    pub fn contains(self: CpuSet, cpu: usize) bool {
        if (cpu >= self.capacity) return false;
        return (@atomicLoad(usize, &self.words[cpu / bits], .acquire) & (@as(usize, 1) << @intCast(cpu % bits))) != 0;
    }

    pub fn insert(self: CpuSet, cpu: usize) void {
        std.debug.assert(cpu < self.capacity);
        self.words[cpu / bits] |= @as(usize, 1) << @intCast(cpu % bits);
    }

    // Serial BSP startup publishes newly ready APs while earlier APs may
    // inspect the online set. Runtime topology is immutable after startup.
    pub fn insertAtomic(self: CpuSet, cpu: usize) void {
        std.debug.assert(cpu < self.capacity);
        _ = @atomicRmw(usize, &self.words[cpu / bits], .Or, @as(usize, 1) << @intCast(cpu % bits), .release);
    }

    pub fn remove(self: CpuSet, cpu: usize) void {
        std.debug.assert(cpu < self.capacity);
        self.words[cpu / bits] &= ~(@as(usize, 1) << @intCast(cpu % bits));
    }

    pub fn clear(self: CpuSet) void {
        @memset(self.words, 0);
    }

    pub fn count(self: CpuSet) usize {
        var total: usize = 0;
        for (self.words) |*word| total += @popCount(@atomicLoad(usize, word, .acquire));
        return total;
    }

    pub fn eql(self: CpuSet, other: CpuSet) bool {
        if (self.capacity != other.capacity) return false;
        for (self.words, other.words) |*a, *b| {
            if (@atomicLoad(usize, a, .acquire) != @atomicLoad(usize, b, .acquire)) return false;
        }
        return true;
    }

    pub fn copyFrom(self: CpuSet, other: CpuSet) void {
        std.debug.assert(self.capacity == other.capacity);
        for (self.words, other.words) |*dst, *src| dst.* = @atomicLoad(usize, src, .acquire);
    }

    pub fn next(self: CpuSet, start: usize) ?usize {
        var cpu = start;
        while (cpu < self.capacity) : (cpu += 1) if (self.contains(cpu)) return cpu;
        return null;
    }
};

test "CPU membership crosses words and does not invent tail CPUs" {
    var backing: [CpuSet.wordCount(513)]usize = undefined;
    const set = CpuSet.init(&backing, 513);
    for ([_]usize{ 0, 63, 64, 71, 255, 256, 512 }) |cpu| set.insert(cpu);
    try std.testing.expectEqual(@as(usize, 7), set.count());
    try std.testing.expect(set.contains(64));
    try std.testing.expect(!set.contains(513));
    try std.testing.expectEqual(@as(?usize, 512), set.next(257));
    set.remove(64);
    try std.testing.expect(!set.contains(64));
    var other_backing: [backing.len]usize = undefined;
    const other = CpuSet.init(&other_backing, 513);
    other.copyFrom(set);
    try std.testing.expect(other.eql(set));
    set.clear();
    try std.testing.expectEqual(@as(usize, 0), set.count());
    try std.testing.expectEqual(@as(usize, 6), other.count());
}
