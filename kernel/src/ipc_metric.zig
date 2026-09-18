const std = @import("std");
const root = @import("root");
const smp = @import("smp.zig");
const clock = @import("realtime_clock.zig");

pub const enabled = if (@hasDecl(root, "ipc_profile_enabled")) root.ipc_profile_enabled else false;

// Pairs of count/cumulative TSC cycles, across all native IPC callers.
// send_wake includes recv_message and copyout; recv_message includes copyout.
// handoff measures frame-switch preparation, not sender resumption latency.
pub const Op = enum(usize) {
    wait_register,
    wait_repoll,
    send_enqueue,
    send_wake,
    copyin,
    copyout,
    handoff,
    recv_message,
};
const row_words = 2 * @typeInfo(Op).@"enum".fields.len;
const cpu_rows = if (enabled) smp.max_cpus else 0;
var counters: [cpu_rows][row_words]u64 align(128) =
    std.mem.zeroes([cpu_rows][row_words]u64);
var syscall_counts: [cpu_rows][256]u64 align(128) =
    std.mem.zeroes([cpu_rows][256]u64);
comptime {
    if (enabled) @export(&counters, .{ .name = "ipc_perf_counters" });
    if (enabled) @export(&syscall_counts, .{ .name = "ipc_syscall_counts" });
}

pub fn recordSyscall(nr: u64) void {
    if (!enabled or nr >= 256) return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= smp.max_cpus) return;
    _ = @atomicRmw(u64, &syscall_counts[cpu][@intCast(nr)], .Add, 1, .monotonic);
}

pub fn timestamp() u64 {
    return if (enabled) clock.readCounter() else 0;
}

pub fn record(op: Op, start_tsc: u64) void {
    if (enabled) recordElapsed(op, timestamp() -% start_tsc);
}

pub fn recordElapsed(op: Op, cycles: u64) void {
    if (!enabled) return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= smp.max_cpus) return;
    const index = 2 * @intFromEnum(op);
    _ = @atomicRmw(u64, &counters[cpu][index], .Add, 1, .monotonic);
    _ = @atomicRmw(u64, &counters[cpu][index + 1], .Add, cycles, .monotonic);
}

// QMP snapshots are live: a count/cycles pair is not one atomic transaction.
// Use long interval differences as approximations, not exact per-call traces.
pub fn staticEnd() usize {
    if (!enabled) return 0;
    return @max(@intFromPtr(&counters) + @sizeOf(@TypeOf(counters)), @intFromPtr(&syscall_counts) + @sizeOf(@TypeOf(syscall_counts)));
}
