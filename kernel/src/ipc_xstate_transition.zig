//! Local syscall XState ownership; IRQs never transfer this ownership.
//! false also covers a saved source awaiting target restore. Only saving the
//! current SOURCE may clear live; activation itself must never set it again.
pub const Ownership = struct {
    // Independent CPUs must not bounce the live-entry ownership cache line.
    live: bool align(128) = false,
    pub fn begin(self: *Ownership) void {
        self.live = true;
    }
    pub fn saved(self: *Ownership) void {
        self.live = false;
    }
    pub fn finish(self: *Ownership) bool {
        const restore = !self.live;
        self.live = false;
        return restore;
    }
};
const std = @import("std");
test "IPC XState same-thread skips both operations including repeated entries" {
    var mode: Ownership = .{};
    for (0..20) |_| {
        mode.begin();
        try std.testing.expect(!mode.finish());
    }
}
test "IPC XState source save then target activation requires restore" {
    var mode: Ownership = .{};
    mode.begin();
    mode.saved();
    try std.testing.expect(!mode.live);
    try std.testing.expect(mode.finish());
}
test "IPC XState signal and generic miss materialize image once" {
    var mode: Ownership = .{};
    for (0..2) |_| {
        mode.begin();
        var saves: usize = 0;
        for (0..3) |_| if (mode.live) {
            saves += 1;
            mode.saved();
        };
        try std.testing.expectEqual(@as(usize, 1), saves);
        try std.testing.expect(mode.finish());
    }
}
test "IPC XState nested kernel interrupts retain local live ownership" {
    var mode: Ownership = .{};
    mode.begin();
    const interrupted = mode;
    // A kernel IRQ's FXSAVE/FXRSTOR is private stack storage, not a source
    // context save. The whole-ELF gate bans upper-lane writes in its callees.
    try std.testing.expectEqual(interrupted, mode);
    try std.testing.expect(!mode.finish());
}
