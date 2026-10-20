//! Object-local IPC exclusion. Acquire before T/S; never carry across a
//! stack-abandoning scheduler switch or release of an attached capability
//! (which can close a different endpoint). The direct IPC commit may change
//! the CPU context pin within the same masked kernel continuation; its caller
//! must end Pair before restoring user XState or reopening interrupts. A guard
//! borrows a separately retained stable body.
const std = @import("std");
const builtin = @import("builtin");
const diagnostics = @import("lock_diagnostics.zig");

pub const Lock = struct {
    byte: diagnostics.Byte = .{},
    waiting: if (builtin.is_test) std.atomic.Value(usize) else void = if (builtin.is_test) .init(0) else {},
    // Affine caller contract: do not copy a live guard. end is idempotent only
    // on the same value, allowing an early drop plus lexical error cleanup.
    pub const Guard = struct {
        lock: ?*Lock,
        restore: bool,
        pub fn end(self: *Guard) void {
            const lock = self.lock orelse return;
            self.lock = null;
            diagnostics.releasing(&lock.byte.record);
            @atomicStore(u8, &lock.byte.value, 0, .release);
            if (!builtin.is_test and self.restore) asm volatile ("sti" ::: .{ .memory = true });
        }
    };
    pub fn acquire(self: *Lock) Guard {
        var flags: u64 = 0;
        if (!builtin.is_test) asm volatile ("pushfq; pop %[flags]; cli"
            : [flags] "=r" (flags),
            :
            : .{ .memory = true });
        const key: diagnostics.order.Key = .{ .class = .ipc_endpoint, .order = @intFromPtr(self) };
        diagnostics.beforeAcquire(&self.byte.record, key);
        if (builtin.is_test) _ = self.waiting.fetchAdd(1, .release);
        while (@cmpxchgWeak(u8, &self.byte.value, 0, 1, .acquire, .monotonic) != null) std.atomic.spinLoopHint();
        if (builtin.is_test) _ = self.waiting.fetchSub(1, .release);
        diagnostics.acquired(&self.byte.record, key);
        return .{ .lock = self, .restore = flags & (1 << 9) != 0 };
    }
    pub fn assertHeld(self: *Lock) void {
        diagnostics.assertOwned(&self.byte.record);
    }
};

/// REPLY_RECV may receive on a different endpoint from the reply block.
/// Resolve and retain both identities before acquiring either lock; order by
/// stable address, then release in reverse. Never discover a second lock in T.
pub const Pair = struct {
    first: Lock.Guard,
    second: Lock.Guard,
    pub fn acquire(a: *Lock, b: ?*Lock) Pair {
        if (b == null or b.? == a) return .{ .first = a.acquire(), .second = .{ .lock = null, .restore = false } };
        const low = if (@intFromPtr(a) < @intFromPtr(b.?)) a else b.?;
        const high = if (low == a) b.? else a;
        return .{ .first = low.acquire(), .second = high.acquire() };
    }
    pub fn end(self: *Pair) void {
        self.second.end();
        self.first.end();
    }
};
