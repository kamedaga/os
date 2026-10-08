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
// BSP-only successful absolute program, not proof that the IRQ was consumed.
// Wake IPIs may observe an expired countdown with its timer IRQ still pending.
var programmed_deadline_ns: u64 = 0;

// Host tests exercise publication without issuing privileged APIC operations.
// The notifier remains mandatory so a missing notification cannot pass silently.
var test_notifier: ?*const fn () void = null;
pub const TestState = struct { deadline: u64, retry: u64, programmed: u64, notifier: ?*const fn () void };

pub fn testInstallNotifier(notifier: *const fn () void) TestState {
    if (!@import("builtin").is_test) @compileError("test-only clock notifier");
    const saved = TestState{ .deadline = waiter_deadline_ns, .retry = contention_retry_ns, .programmed = programmed_deadline_ns, .notifier = test_notifier };
    waiter_deadline_ns = 0;
    contention_retry_ns = 0;
    programmed_deadline_ns = 0;
    test_notifier = notifier;
    return saved;
}

pub fn testRestoreState(saved: TestState) void {
    if (!@import("builtin").is_test) @compileError("test-only clock state");
    waiter_deadline_ns = saved.deadline;
    contention_retry_ns = saved.retry;
    programmed_deadline_ns = saved.programmed;
    test_notifier = saved.notifier;
}

pub fn kernelStaticStorageEndAddr() usize {
    return @max(lapic.kernelStaticStorageEndAddr(), @max(@intFromPtr(&programmed_deadline_ns) + @sizeOf(u64), @max(@intFromPtr(&waiter_deadline_ns) + @sizeOf(u64), @intFromPtr(&contention_retry_ns) + @sizeOf(u64))));
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
    rearmForEvent(false);
}

pub fn rearmAfterTimerInterrupt() void {
    rearmForEvent(true);
}

fn rearmForEvent(timer_expired: bool) void {
    if (!scheduler.isBootstrapSchedulerCpu()) return;
    rearmWithClock(lapic, clock.monotonicNs(), timer_expired);
}

fn rearmWithClock(comptime Timer: type, now: ?u64, timer_expired: bool) void {
    if (now) |now_ns| {
        const deadline = selectDeadline(now_ns, @atomicLoad(u64, &waiter_deadline_ns, .acquire), contention_retry_ns);
        // Leave a matching countdown (or its pending IRQ) untouched on wake.
        // A real timer IRQ must renew even an equal already-due deadline; the
        // cache never replaces remote publication or waiter token validation.
        if (!timer_expired and programmed_deadline_ns != 0 and programmed_deadline_ns == deadline) return;
        if (Timer.armDeadline(now_ns, deadline)) {
            programmed_deadline_ns = deadline;
            return;
        }
    }
    // Failure/fallback does not prove the absolute countdown is installed.
    // Clear before retry so a later successful clock cannot skip its program.
    programmed_deadline_ns = 0;
    // Without an absolute clockevent, a wake/maintenance IPI must leave the
    // existing countdown alone. Reloading 1 ms at each RPC wake starves time.
    // Only the actual timer IRQ may reload, including when expiry is already
    // pending while a higher-priority IPI sees current_count == 0.
    if (fallbackShouldReload(timer_expired))
        _ = Timer.armTimer(Timer.timerInitialCount(@import("boot/main_static.zig").lapic_timer_initial_count));
}

const TestTimer = struct {
    var absolute_calls: usize = 0;
    var fallback_calls: usize = 0;
    var deadline: u64 = 0;
    var succeeds: bool = true;

    fn armDeadline(_: u64, wanted: u64) bool {
        absolute_calls += 1;
        deadline = wanted;
        return succeeds;
    }

    fn armTimer(_: u32) bool {
        fallback_calls += 1;
        return succeeds;
    }

    fn timerInitialCount(fallback: u32) u32 {
        return fallback;
    }

    fn reset() void {
        absolute_calls = 0;
        fallback_calls = 0;
        deadline = 0;
        succeeds = true;
    }
};

test "clockevent suppresses equal wake programs but renews actual expiry" {
    const saved = testInstallNotifier(struct {
        fn notify() void {}
    }.notify);
    defer testRestoreState(saved);
    TestTimer.reset();
    rearmWithClock(TestTimer, 1_100_000, false);
    try std.testing.expectEqual(@as(usize, 1), TestTimer.absolute_calls);
    try std.testing.expectEqual(@as(u64, 2_000_000), programmed_deadline_ns);
    rearmWithClock(TestTimer, 1_900_000, false);
    try std.testing.expectEqual(@as(usize, 1), TestTimer.absolute_calls);

    _ = publish(1_950_000);
    rearmWithClock(TestTimer, 1_900_000, false);
    try std.testing.expectEqual(@as(usize, 2), TestTimer.absolute_calls);
    // Maintenance must not reload a one-shot whose IRQ is still pending.
    rearmWithClock(TestTimer, 1_960_000, false);
    try std.testing.expectEqual(@as(usize, 2), TestTimer.absolute_calls);
    rearmWithClock(TestTimer, 1_970_000, true);
    try std.testing.expectEqual(@as(usize, 3), TestTimer.absolute_calls);
    try std.testing.expectEqual(@as(u64, 1_950_000), programmed_deadline_ns);
    rearmWithClock(TestTimer, 1_980_000, true);
    try std.testing.expectEqual(@as(usize, 4), TestTimer.absolute_calls);
}

test "clockevent cache tracks earlier later cancelled retry and tick deadlines" {
    const saved = testInstallNotifier(struct {
        fn notify() void {}
    }.notify);
    defer testRestoreState(saved);
    TestTimer.reset();
    rearmWithClock(TestTimer, 1_100_000, false);
    _ = publish(1_250_000);
    rearmWithClock(TestTimer, 1_100_000, false);
    try std.testing.expectEqual(@as(u64, 1_250_000), TestTimer.deadline);
    _ = publish(1_750_000);
    rearmWithClock(TestTimer, 1_100_000, false);
    try std.testing.expectEqual(@as(u64, 1_750_000), TestTimer.deadline);
    _ = publish(null);
    rearmWithClock(TestTimer, 1_100_000, false);
    try std.testing.expectEqual(@as(u64, 2_000_000), TestTimer.deadline);
    _ = publish(1_050_000);
    deferContendedDelivery(1_100_000);
    rearmWithClock(TestTimer, 1_100_000, false);
    try std.testing.expectEqual(@as(u64, 1_150_000), TestTimer.deadline);
    deferContendedDelivery(1_110_000);
    rearmWithClock(TestTimer, 1_110_000, false);
    try std.testing.expectEqual(@as(u64, 1_160_000), TestTimer.deadline);
    _ = publish(null);
    rearmWithClock(TestTimer, 2_000_000, false);
    try std.testing.expectEqual(@as(u64, 3_000_000), TestTimer.deadline);
    try std.testing.expectEqual(@as(usize, 7), TestTimer.absolute_calls);

    const nested = testInstallNotifier(struct {
        fn notify() void {}
    }.notify);
    try std.testing.expectEqual(@as(u64, 0), programmed_deadline_ns);
    testRestoreState(nested);
    try std.testing.expectEqual(@as(u64, 3_000_000), programmed_deadline_ns);
}

test "clockevent invalidates unsuccessful or unavailable absolute programs" {
    const saved = testInstallNotifier(struct {
        fn notify() void {}
    }.notify);
    defer testRestoreState(saved);
    TestTimer.reset();
    rearmWithClock(TestTimer, 1_100_000, false);
    rearmWithClock(TestTimer, null, false);
    try std.testing.expectEqual(@as(u64, 0), programmed_deadline_ns);
    try std.testing.expectEqual(@as(usize, 0), TestTimer.fallback_calls);
    rearmWithClock(TestTimer, 1_100_000, false);
    try std.testing.expectEqual(@as(usize, 2), TestTimer.absolute_calls);
    TestTimer.succeeds = false;
    _ = publish(1_250_000);
    rearmWithClock(TestTimer, 1_100_000, false);
    try std.testing.expectEqual(@as(u64, 0), programmed_deadline_ns);
    rearmWithClock(TestTimer, 1_100_000, false);
    try std.testing.expectEqual(@as(usize, 4), TestTimer.absolute_calls);
    rearmWithClock(TestTimer, 1_100_000, true);
    try std.testing.expectEqual(@as(usize, 1), TestTimer.fallback_calls);
    try std.testing.expectEqual(@as(u64, 0), programmed_deadline_ns);
    TestTimer.succeeds = true;
    rearmWithClock(TestTimer, 1_100_000, false);
    try std.testing.expectEqual(@as(u64, 1_250_000), programmed_deadline_ns);
    rearmWithClock(TestTimer, null, true);
    try std.testing.expectEqual(@as(usize, 2), TestTimer.fallback_calls);
    try std.testing.expectEqual(@as(u64, 0), programmed_deadline_ns);
}

fn fallbackShouldReload(timer_expired: bool) bool {
    return timer_expired;
}

test "fallback clockevent notifications cannot postpone the pending tick" {
    var countdown: u32 = 1000;
    for (0..999) |_| {
        if (fallbackShouldReload(false)) countdown = 1000;
        countdown -= 1;
    }
    try std.testing.expectEqual(@as(u32, 1), countdown);
    // Even an expired countdown may have an IRQ already pending: an IPI
    // must not manufacture a second reload before its handler runs.
    try std.testing.expect(!fallbackShouldReload(false));
    try std.testing.expect(fallbackShouldReload(true));
}

test "clockevent deadline selection preserves tick and cancellation" {
    try std.testing.expectEqual(@as(u64, 1_250_000), selectDeadline(1_100_000, 1_250_000, 0));
    try std.testing.expectEqual(@as(u64, 2_000_000), selectDeadline(1_100_000, 0, 0));
    try std.testing.expectEqual(@as(u64, 2_000_000), selectDeadline(1_100_000, 5_000_000, 0));
    try std.testing.expectEqual(@as(u64, 1_150_000), selectDeadline(1_100_000, 1_050_000, 1_150_000));
    try std.testing.expectEqual(@as(u64, 3_000_000), selectDeadline(2_000_000, 0, 0));
}
