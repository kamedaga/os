//! Address-space ownership pins bridge scheduler/context transitions.
//! FD lookup borrows under K; these pins remain for AS teardown.
const std = @import("std");

pub const Error = error{ Overflow, ReleasedGuard, PinsOutstanding, NotClosed };

/// Closing removes the admission bit in the same CAS word as the pin count.
/// An in-flight reader can keep memory alive, but cannot reopen a last-closed
/// object. The writer owns the initial admission reference, not a reader pin.
pub const Live = struct {
    const closed = @as(usize, 1) << (@bitSizeOf(usize) - 1);
    const mask = closed - 1;
    word: usize = closed,

    // Private construction only, before publishing the object anywhere.
    pub fn init() Live {
        return .{ .word = 0 };
    }

    pub fn tryPin(self: *Live) bool {
        var value = @atomicLoad(usize, &self.word, .acquire);
        while (true) {
            if (value & closed != 0 or value & mask == mask) return false;
            if (@cmpxchgWeak(usize, &self.word, value, value + 1, .acquire, .monotonic)) |actual| {
                value = actual;
            } else return true;
        }
    }

    pub fn unpin(self: *Live) void {
        const old = @atomicRmw(usize, &self.word, .Sub, 1, .release);
        std.debug.assert(old & mask != 0);
    }

    /// Idempotent logical close. Physical reclamation additionally needs all
    /// guarded readers gone, including readers not yet inside tryPin().
    pub fn close(self: *Live) void {
        _ = @atomicRmw(usize, &self.word, .Or, closed, .acq_rel);
    }

    pub fn pinsEnded(self: *const Live) bool {
        return @atomicLoad(usize, &self.word, .acquire) & mask == 0;
    }

    pub fn reclaimable(self: *const Live) Error!void {
        const value = @atomicLoad(usize, &self.word, .acquire);
        if (value & closed == 0) return error.NotClosed;
        if (value & mask != 0) return error.PinsOutstanding;
    }
};

test "IPC lifetime last close cannot be resurrected by a reader pin" {
    var live = Live.init();
    try std.testing.expectError(error.NotClosed, live.reclaimable());
    try std.testing.expect(live.tryPin());
    live.close();
    try std.testing.expect(!live.tryPin());
    try std.testing.expectError(error.PinsOutstanding, live.reclaimable());
    live.unpin();
    try live.reclaimable();
    try std.testing.expect(!live.tryPin());
    live.close();
    try live.reclaimable();
}

/// Logical close under endpoint exclusion. K owns the bytes, while endpoint
/// and scheduler admission must still reject a capability after withdrawal.
pub const Admission = struct {
    open: bool = false,
    pub fn init() Admission {
        return .{ .open = true };
    }
    pub fn isOpen(self: *const Admission) bool {
        return @atomicLoad(bool, &self.open, .acquire);
    }
    pub fn close(self: *Admission) void {
        @atomicStore(bool, &self.open, false, .release);
    }
};
