const std = @import("std");
const lapic = @import("lapic.zig");
const clock = @import("realtime_clock.zig");
const scheduler = @import("scheduler_connection.zig");
const smp = @import("smp.zig");

// KernelState serializes publication; only the BSP programs this timer.
// Zero means no registered native timer waiter. FD/object generations and
// wait tokens are validated under KernelState, never trusted from an IRQ.
var waiter_deadline_ns: u64 = 0;
var contention_retry_ns: u64 = 0; // BSP only, with local IRQs disabled.

// Host tests exercise publication without issuing privileged APIC operations.
// The notifier remains mandatory so a missing notification cannot pass silently.
var test_notifier: ?*const fn () void = null;
pub const TestState = struct { deadline: u64, retry: u64, notifier: ?*const fn () void };

pub fn testInstallNotifier(notifier: *const fn () void) TestState {
    if (!@import("builtin").is_test) @compileError("test-only clock notifier");
    const saved = TestState{ .deadline = waiter_deadline_ns, .retry = contention_retry_ns, .notifier = test_notifier };
    waiter_deadline_ns = 0;
    contention_retry_ns = 0;
    test_notifier = notifier;
    return saved;
}

pub fn testRestoreState(saved: TestState) void {
    if (!@import("builtin").is_test) @compileError("test-only clock state");
    waiter_deadline_ns = saved.deadline;
    contention_retry_ns = saved.retry;
    test_notifier = saved.notifier;
}

pub fn kernelStaticStorageEndAddr() usize {
    return @max(lapic.kernelStaticStorageEndAddr(), @max(@intFromPtr(&waiter_deadline_ns) + @sizeOf(u64), @intFromPtr(&contention_retry_ns) + @sizeOf(u64)));
}

pub fn publish(deadline: ?u64) bool {
    const next = deadline orelse 0;
    return @atomicRmw(u64, &waiter_deadline_ns, .Xchg, next, .release) != next;
}

// Called after dropping KernelState. A later publisher may already have
// replaced the deadline; rearm always loads the newest value, not an argument
// captured before unlocking. Duplicate maintenance IPIs are harmless.
pub fn notifyChanged() void {
    if (@import("builtin").is_test) {
        const notify = test_notifier orelse @panic("clockevent: missing host test notifier");
        notify();
        return;
    }
    if (scheduler.isBootstrapSchedulerCpu()) {
        contention_retry_ns = 0;
        rearm();
    } else if (!smp.interruptCpu(0)) {
        @panic("clockevent: cannot notify deadline owner CPU");
    }
}

pub fn waiterIsDue(now_ns: u64) bool {
    const deadline = @atomicLoad(u64, &waiter_deadline_ns, .acquire);
    return deadline != 0 and deadline <= now_ns;
}

// An interrupt must not spin on the interrupted CPU's KernelState lock, nor
// storm at a one-count deadline before that CPU can resume and release it.
// This bounded retry applies to actual lock contention or unsafe interrupted
// copyout (TLB shootdown), not ordinary timer waits. A publisher can bring the
// deadline forward again on unlock.
pub fn deferContendedDelivery(now_ns: u64) void {
    contention_retry_ns = now_ns +| 50_000;
}

pub fn selectDeadline(now_ns: u64, waiter: u64, retry: u64) u64 {
    const next_tick = now_ns +| (1_000_000 - now_ns % 1_000_000);
    if (waiter == 0) return next_tick;
    return @min(next_tick, @max(waiter, retry));
}

// IF is clear. The existing scheduler tick remains an absolute 1 ms boundary;
// inserting a sub-ms timer interrupt does not move that boundary or charge a
// second scheduler tick. Long deadlines are automatically revisited at ticks.
pub fn rearm() void {
    if (!scheduler.isBootstrapSchedulerCpu()) return;
    if (clock.monotonicNs()) |now_ns| {
        const deadline = selectDeadline(now_ns, @atomicLoad(u64, &waiter_deadline_ns, .acquire), contention_retry_ns);
        if (lapic.armDeadline(now_ns, deadline)) return;
    }
    _ = lapic.armTimer(lapic.timerInitialCount(@import("boot/main_static.zig").lapic_timer_initial_count));
}

test "clockevent deadline selection preserves tick and cancellation" {
    try std.testing.expectEqual(@as(u64, 1_250_000), selectDeadline(1_100_000, 1_250_000, 0));
    try std.testing.expectEqual(@as(u64, 2_000_000), selectDeadline(1_100_000, 0, 0));
    try std.testing.expectEqual(@as(u64, 2_000_000), selectDeadline(1_100_000, 5_000_000, 0));
    try std.testing.expectEqual(@as(u64, 1_150_000), selectDeadline(1_100_000, 1_050_000, 1_150_000));
    try std.testing.expectEqual(@as(u64, 3_000_000), selectDeadline(2_000_000, 0, 0));
}
