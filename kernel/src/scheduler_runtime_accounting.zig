const std = @import("std");

/// CPU-local elapsed-service accounting, protected by the owning runqueue lock.
/// This tracks ownership intervals, never the timer/preemption budget. Keeping
/// those separate allows an IPC transfer without granting a fresh quantum.
pub const Accounting = struct {
    checkpoint_ns: ?u64 = null,
    credit_ns: u64 = 0,

    pub const Prepared = struct {
        runtime_ns: u64,
        next_checkpoint_ns: ?u64,
        measured: bool,
        next_credit_ns: u64,
    };

    pub fn start(self: *Accounting, now_ns: ?u64) void {
        self.checkpoint_ns = now_ns;
    }

    pub fn end(self: *Accounting) void {
        self.checkpoint_ns = null;
    }

    /// Actual idle/reset starts a new timer accounting period. Ordinary
    /// ownership changes retain credit already charged within the old period.
    pub fn reset(self: *Accounting) void {
        self.* = .{};
    }

    /// Pure preflight: a failed entity transition must not consume service.
    /// A direct transfer requires measured=true; ordinary timer paths retain
    /// their existing tick estimate when a reliable interval is unavailable.
    pub fn prepare(self: *const Accounting, now_ns: ?u64, fallback_ns: u64) Prepared {
        if (self.checkpoint_ns) |previous| {
            if (now_ns) |now| {
                if (now >= previous) return .{
                    .runtime_ns = now - previous,
                    .next_checkpoint_ns = now,
                    .measured = true,
                    .next_credit_ns = if (fallback_ns != 0) 0 else self.credit_ns +| (now - previous),
                };
            }
        }
        // Rebase after fallback. Retaining the old checkpoint would charge
        // the fallback interval a second time when the clock becomes usable.
        return .{
            .runtime_ns = fallback_ns -| self.credit_ns,
            .next_checkpoint_ns = now_ns,
            .measured = false,
            .next_credit_ns = if (fallback_ns != 0) 0 else self.credit_ns,
        };
    }

    pub fn commit(self: *Accounting, prepared: Prepared) void {
        self.checkpoint_ns = prepared.next_checkpoint_ns;
        self.credit_ns = prepared.next_credit_ns;
    }
};

test "runtime accounting partitions owner intervals without renewing timer budget" {
    var accounting: Accounting = .{};
    accounting.start(100);
    const first = accounting.prepare(140, 1000);
    try std.testing.expect(first.measured);
    try std.testing.expectEqual(@as(u64, 40), first.runtime_ns);
    // Rejected preflight consumes nothing.
    try std.testing.expectEqual(@as(?u64, 100), accounting.checkpoint_ns);
    accounting.commit(first);
    const second = accounting.prepare(155, 1000);
    try std.testing.expectEqual(@as(u64, 15), second.runtime_ns);
    accounting.commit(second);
    const at_timer = accounting.prepare(200, 1000);
    try std.testing.expectEqual(@as(u64, 45), at_timer.runtime_ns);
    try std.testing.expectEqual(@as(u64, 100), first.runtime_ns + second.runtime_ns + at_timer.runtime_ns);
    accounting.commit(at_timer);
    try std.testing.expectEqual(@as(u64, 0), accounting.prepare(200, 1000).runtime_ns);
    accounting.end();
    try std.testing.expect(!accounting.prepare(250, 1000).measured);
    accounting.start(300);
    try std.testing.expectEqual(@as(u64, 10), accounting.prepare(310, 1000).runtime_ns);
}

test "runtime accounting unavailable and rewound samples rebase fallback exactly once" {
    var accounting: Accounting = .{};
    accounting.start(100);
    const absent = accounting.prepare(null, 30);
    try std.testing.expect(!absent.measured);
    try std.testing.expectEqual(@as(u64, 30), absent.runtime_ns);
    accounting.commit(absent);
    try std.testing.expectEqual(@as(?u64, null), accounting.checkpoint_ns);
    const restored = accounting.prepare(160, 30);
    try std.testing.expect(!restored.measured);
    accounting.commit(restored);
    try std.testing.expectEqual(@as(u64, 10), accounting.prepare(170, 30).runtime_ns);
    const rewound = accounting.prepare(150, 30);
    try std.testing.expect(!rewound.measured);
    try std.testing.expectEqual(@as(u64, 30), rewound.runtime_ns);
    accounting.commit(rewound);
    try std.testing.expectEqual(@as(u64, 10), accounting.prepare(160, 30).runtime_ns);
}

test "runtime accounting measured interval spans the unsigned clock range" {
    var accounting: Accounting = .{};
    accounting.start(0);
    const elapsed = accounting.prepare(std.math.maxInt(u64), 0);
    try std.testing.expect(elapsed.measured);
    try std.testing.expectEqual(std.math.maxInt(u64), elapsed.runtime_ns);
    // The caller validates this unsigned duration against the scalar entity
    // API's signed range before committing either the entity or checkpoint.
    try std.testing.expectEqual(@as(?u64, 0), accounting.checkpoint_ns);
}

test "runtime accounting clock loss settles only uncharged timer remainder" {
    var accounting: Accounting = .{};
    accounting.start(100);
    const source = accounting.prepare(140, 0);
    accounting.commit(source);
    accounting.end();
    accounting.start(null);
    try std.testing.expectEqual(@as(u64, 40), accounting.credit_ns);
    const timer = accounting.prepare(null, 100);
    try std.testing.expectEqual(@as(u64, 60), timer.runtime_ns);
    accounting.commit(timer);
    try std.testing.expectEqual(@as(u64, 0), accounting.credit_ns);
    try std.testing.expectEqual(@as(u64, 100), source.runtime_ns + timer.runtime_ns);
    accounting.start(200);
    accounting.commit(accounting.prepare(350, 0));
    const saturated = accounting.prepare(null, 100);
    try std.testing.expectEqual(@as(u64, 0), saturated.runtime_ns);
    accounting.commit(saturated);
    try std.testing.expectEqual(@as(u64, 0), accounting.credit_ns);
    accounting.start(400);
    accounting.commit(accounting.prepare(410, 0));
    accounting.reset();
    try std.testing.expectEqual(@as(?u64, null), accounting.checkpoint_ns);
    try std.testing.expectEqual(@as(u64, 0), accounting.credit_ns);
}
