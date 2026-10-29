//! Diagnostic-only syscall mix for the lock-profile measurement.
const lock_profile = @import("lock_profile.zig");
const runtime_abi = @import("kernel_abi_root").runtime_abi;
const smp = @import("smp.zig");
const std = @import("std");

const cpu_rows = if (lock_profile.enabled) 256 else 0;
const nr_count: usize = runtime_abi.syscall_runtime_count;
const row_words: usize = 16;
// CPU-local atomic rows avoid creating another contended global lock. The
// padded stride prevents adjacent CPUs from sharing a cache line. The profile
// run uses four CPUs; an unsupported larger slot is counted below.
var counts: [cpu_rows][row_words]u64 align(128) = @splat(@splat(0));
var locked_counts: [cpu_rows][row_words]u64 align(128) = @splat(@splat(0));
var overflow: if (lock_profile.enabled) u64 else void = if (lock_profile.enabled) 0 else {};

pub fn staticEnd() usize {
    if (comptime !lock_profile.enabled) return 0;
    return @max(@intFromPtr(&counts) + @sizeOf(@TypeOf(counts)),
        @max(@intFromPtr(&locked_counts) + @sizeOf(@TypeOf(locked_counts)),
        @intFromPtr(&overflow) + @sizeOf(@TypeOf(overflow))));
}
comptime {
    if (lock_profile.enabled) {
        @export(&counts, .{ .name = "runtime_nr_profile_counts" });
        @export(&locked_counts, .{ .name = "runtime_nr_profile_locked" });
        @export(&overflow, .{ .name = "runtime_nr_profile_overflow" });
    }
}

pub fn recordLocked(nr: u64) void {
    if (comptime !lock_profile.enabled) return;
    if (!runtime_abi.isRuntimeSyscall(nr)) return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= locked_counts.len) {
        _ = @atomicRmw(u64, &overflow, .Add, 1, .monotonic);
        return;
    }
    _ = @atomicRmw(u64, &locked_counts[cpu][@intCast(nr - runtime_abi.syscall_runtime_first)], .Add, 1, .monotonic);
}

pub fn record(nr: u64) void {
    if (comptime !lock_profile.enabled) return;
    if (!runtime_abi.isRuntimeSyscall(nr)) return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= counts.len) {
        _ = @atomicRmw(u64, &overflow, .Add, 1, .monotonic);
        return;
    }
    _ = @atomicRmw(u64, &counts[cpu][@intCast(nr - runtime_abi.syscall_runtime_first)], .Add, 1, .monotonic);
}

test "profile slots match the unchanged runtime ABI range" {
    try std.testing.expectEqual(@as(usize, 15), nr_count);
    try std.testing.expectEqual(@as(usize, 128), row_words * @sizeOf(u64));
    for (runtime_abi.syscall_runtime_first..runtime_abi.syscall_runtime_last + 1) |nr|
        try std.testing.expect(@as(usize, @intCast(nr - runtime_abi.syscall_runtime_first)) < nr_count);
}
