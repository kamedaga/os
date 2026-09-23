const std = @import("std");
const root = @import("root");
const smp = @import("smp.zig");
const clock = @import("realtime_clock.zig");
const types = @import("state/types.zig");
const abi = @import("kernel_abi_root");

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
// Slot totals, not persistent process identities: slots can be reused. Host
// analysis must correlate short interval deltas with descriptor generations.
// Entry counts include failed attempts; no per-call logging or elapsed clocks.
const principal_rows = if (enabled) types.max_process_slots else 0;
var principal_counts: [principal_rows][6]u64 align(128) =
    std.mem.zeroes([principal_rows][6]u64);
comptime {
    if (enabled) @export(&counters, .{ .name = "ipc_perf_counters" });
    if (enabled) @export(&syscall_counts, .{ .name = "ipc_syscall_counts" });
    if (enabled) @export(&principal_counts, .{ .name = "ipc_principal_syscall_counts" });
}

fn principalColumn(nr: u64) ?usize {
    return switch (nr) {
        abi.runtime_abi.syscall_clock_gettime => 0,
        abi.vm_abi.syscall_munmap => 1,
        abi.vm_abi.syscall_mmap => 2,
        abi.process_abi.syscall_process_signal_ctl => 3,
        abi.runtime_abi.syscall_gettid => 4,
        abi.process_abi.syscall_thread_set_gs_base => 5,
        else => null,
    };
}

fn recordPrincipal(nr: u64, principal: types.PrincipalId) void {
    if (!enabled) return;
    const column = principalColumn(nr) orelse return;
    const index = types.processIndexFromPrincipal(principal) orelse return;
    _ = @atomicRmw(u64, &principal_counts[index][column], .Add, 1, .monotonic);
}

pub fn recordSyscall(nr: u64, principal: types.PrincipalId) void {
    if (!enabled or nr >= 256) return;
    recordPrincipal(nr, principal);
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
    return @max(@max(@intFromPtr(&counters) + @sizeOf(@TypeOf(counters)), @intFromPtr(&syscall_counts) + @sizeOf(@TypeOf(syscall_counts))), @intFromPtr(&principal_counts) + @sizeOf(@TypeOf(principal_counts)));
}

test "principal syscall counts are bounded atomic slot totals" {
    if (!enabled) return;
    const calls = [_]u64{ abi.runtime_abi.syscall_clock_gettime, abi.vm_abi.syscall_munmap, abi.vm_abi.syscall_mmap, abi.process_abi.syscall_process_signal_ctl, abi.runtime_abi.syscall_gettid, abi.process_abi.syscall_thread_set_gs_base };
    for ([_]usize{ 0, types.max_process_slots - 1 }) |index| {
        const previous = principal_counts[index];
        defer principal_counts[index] = previous;
        @memset(&principal_counts[index], 0);
        for (calls, 0..) |nr, column| {
            try std.testing.expectEqual(column, principalColumn(nr).?);
            recordPrincipal(nr, @enumFromInt(index));
        }
        recordPrincipal(255, @enumFromInt(index));
        try std.testing.expectEqual([_]u64{ 1, 1, 1, 1, 1, 1 }, principal_counts[index]);
    }
    try std.testing.expectEqual(null, principalColumn(255));
    // Invalid principals must not alias either boundary slot.
    const first = principal_counts[0];
    const last = principal_counts[types.max_process_slots - 1];
    recordPrincipal(calls[0], .Device0);
    recordPrincipal(calls[0], @enumFromInt(std.math.maxInt(types.PrincipalRaw)));
    try std.testing.expectEqual(first, principal_counts[0]);
    try std.testing.expectEqual(last, principal_counts[types.max_process_slots - 1]);
    const Concurrent = struct {
        fn run() void {
            for (0..10000) |_| recordPrincipal(abi.vm_abi.syscall_munmap, @enumFromInt(1));
        }
    };
    const previous = principal_counts[1];
    defer principal_counts[1] = previous;
    @memset(&principal_counts[1], 0);
    const worker = try std.Thread.spawn(.{}, Concurrent.run, .{});
    Concurrent.run();
    worker.join();
    try std.testing.expectEqual(@as(u64, 20000), principal_counts[1][1]);
    try std.testing.expect(staticEnd() >= @intFromPtr(&principal_counts) + @sizeOf(@TypeOf(principal_counts)));
}
