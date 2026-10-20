const std = @import("std");
const smp = @import("smp.zig");
const clock = @import("realtime_clock.zig");
const types = @import("state/types.zig");
const abi = @import("kernel_abi_root");

pub const enabled = types.principal_syscall_columns != 0;

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
    request_build,
    request_claim,
    request_install,
    request_copy,
    request_complete,
    request_register,
    request_block,
    request_select,
    request_switch,
    request_pmm_alloc,
    request_pmm_free,
    reply_build,
    reply_claim,
    reply_install,
    reply_copy,
    reply_complete,
    reply_register,
    reply_block,
    reply_select,
    reply_switch,
    reply_pmm_alloc,
    reply_pmm_free,
    fast_success,
    queued_success,
    direct_delivery_success,
    fast_extension,
    fast_flags,
    fast_timeout,
    fast_authority,
    fast_queued,
    fast_generic_waiter,
    fast_no_receiver,
    no_receiver_unseen,
    no_receiver_stale,
    no_receiver_running,
    no_receiver_runnable,
    no_receiver_waiting,
    generic_poll,
    generic_receive,
    fast_state,
    fast_retained_fd_wait,
    direct_switch,
    direct_selection_mismatch,
    kfree_success,
    kfree_send_success,
    kfree_send_poll,
    kfree_send_fallback,
    kfree_poll,
    kfree_authority,
    kfree_state,
    kfree_queue,
    kfree_no_receiver,
    entry_call_attempt,
    entry_reply_attempt,
    entry_call_success,
    entry_reply_success,
    xstate_save,
    xstate_restore,
    entry_fail_poll,
    entry_fail_not_waiting,
    entry_fail_cpu,
    entry_fail_fd_transfer,
    entry_fail_words,
    entry_fail_opcode,
    entry_fail_hint,
    entry_fail_other,
    entry_fail_receive_capacity,
    entry_route_overflow,
};
pub const Direction = enum { request, reply };
pub const Phase = enum { build, claim, install, copy, complete, register, block, select, @"switch", pmm_alloc, pmm_free };
// Scoped only across synchronous kernel work; never retained across block or
// a lock-drop callback. Per-CPU storage follows the existing profile capacity.
var direction_boot: [cpu_rows]?Direction = @splat(null);
var directions: []?Direction = &direction_boot;
pub fn enter(direction: ?Direction) ?Direction {
    if (!enabled) return null;
    const cpu = smp.currentCpuSlot();
    if (cpu >= directions.len) return null;
    const old = directions[cpu];
    directions[cpu] = direction;
    const sink: ?@import("lock_profile.zig").PmmScopeSink = if (direction) |d| blk: {
        const base = 2 * (@intFromEnum(Op.request_pmm_alloc) + @intFromEnum(d) * @typeInfo(Phase).@"enum".fields.len);
        break :blk .{ .allocated = counters[cpu][base..][0..2], .returned = counters[cpu][base + 2 ..][0..2] };
    } else null;
    @import("lock_profile.zig").setPmmScopeSink(sink);
    return old;
}
pub fn phase(direction: ?Direction, kind: Phase, start: u64) void {
    if (!enabled) return;
    const d = direction orelse return;
    if (@import("ipc_path_trace.zig").enabled) @import("ipc_path_trace.zig").span(200 + @as(u64, @intFromEnum(kind)), start, timestamp());
    const base = @intFromEnum(Op.request_build);
    const stride = @typeInfo(Phase).@"enum".fields.len;
    record(@enumFromInt(base + @intFromEnum(d) * stride + @intFromEnum(kind)), start);
}
pub fn syscallDirection(nr: u64) ?Direction {
    return if (nr == abi.ipc_abi.syscall_ipc_call) .request else if (nr == abi.ipc_abi.syscall_ipc_reply_recv) .reply else null;
}
const row_words = 2 * @typeInfo(Op).@"enum".fields.len;
const cpu_rows = if (enabled) 1 else 0;
var counters_boot: [cpu_rows][row_words]u64 align(128) = @splat(@splat(0));
var syscall_boot: [cpu_rows][256]u64 align(128) = @splat(@splat(0));
var counters: [][row_words]u64 = &counters_boot;
var counters_view = @import("diagnostic_cpu_storage.zig").Descriptor([row_words]u64).init(&counters_boot);
var syscall_counts: [][256]u64 = &syscall_boot;
var syscall_counts_view = @import("diagnostic_cpu_storage.zig").Descriptor([256]u64).init(&syscall_boot);
var ipc_perf_layout = [_]u64{ cpu_rows, row_words, 256 };
pub fn initCpuStorage(count: usize) !void {
    if (comptime !enabled) return;
    const storage = @import("diagnostic_cpu_storage.zig");
    try initCensus(count);
    try @import("notify_metric.zig").initCpuStorage(count);
    directions = try storage.grow(?Direction, directions, count, null);
    counters = try storage.grow([row_words]u64, counters, count, @splat(0));
    syscall_counts = try storage.grow([256]u64, syscall_counts, count, @splat(0));
    ipc_perf_layout[0] = count;
    counters_view = @TypeOf(counters_view).init(counters);
    syscall_counts_view = @TypeOf(syscall_counts_view).init(syscall_counts);
}
// Marker for host readers: rows now live in the process descriptor directory.
var principal_counts_present: u8 = 0;
comptime {
    if (enabled) @export(&ipc_perf_layout, .{ .name = "ipc_perf_layout" });
    if (enabled) @export(&counters_view, .{ .name = "ipc_perf_counters" });
    if (enabled) @export(&syscall_counts_view, .{ .name = "ipc_syscall_counts" });
    if (enabled) @export(&principal_counts_present, .{ .name = "ipc_principal_syscall_counts" });
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

fn recordPrincipal(state: anytype, nr: u64, principal: types.PrincipalId) void {
    if (!enabled) return;
    const column = principalColumn(nr) orelse return;
    @import("state/process.zig").recordPrincipalSyscall(state, principal, column);
}

pub fn recordSyscall(state: anytype, nr: u64, principal: types.PrincipalId) void {
    if (!enabled or nr >= 256) return;
    recordPrincipal(state, nr, principal);
    recordCpuSyscall(nr);
}

// Does not borrow KernelState; direct handoff may already have dropped K.
pub fn recordCpuSyscall(nr: u64) void {
    if (!enabled or nr >= 256) return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= counters.len) return;
    _ = @atomicRmw(u64, &syscall_counts[cpu][@intCast(nr)], .Add, 1, .monotonic);
}

pub fn timestamp() u64 {
    return if (enabled) clock.readCounter() else 0;
}

pub fn record(op: Op, start_tsc: u64) void {
    if (@import("ipc_path_trace.zig").enabled) @import("ipc_path_trace.zig").span(300 + @as(u64, @intFromEnum(op)), start_tsc, timestamp());
    if (enabled) recordElapsed(op, timestamp() -% start_tsc);
}

pub fn recordElapsed(op: Op, cycles: u64) void {
    if (!enabled) return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= counters.len) return;
    const index = 2 * @intFromEnum(op);
    _ = @atomicRmw(u64, &counters[cpu][index], .Add, 1, .monotonic);
    _ = @atomicRmw(u64, &counters[cpu][index + 1], .Add, cycles, .monotonic);
}

// QMP snapshots are live: a count/cycles pair is not one atomic transaction.
// Use long interval differences as approximations, not exact per-call traces.
pub fn staticEnd() usize {
    if (!enabled) return 0;
    var end: usize = 0;
    inline for (.{ &counters, &syscall_counts, &counters_boot, &syscall_boot, &ipc_perf_layout, &counters_view, &syscall_counts_view, &principal_counts_present, &directions, &direction_boot, &reason_boot, &reasons, &route_boot, &routes, &route_view, &route_writers, &route_writers_boot, &route_writers_view }) |ptr| end = @max(end, @intFromPtr(ptr) + @sizeOf(@TypeOf(ptr.*)));
    return @max(end, @import("notify_metric.zig").staticEnd());
}

test "principal syscall counts follow descriptor capacity and serialize writers" {
    const KernelState = @import("kernel.zig").KernelState;
    const state = try std.testing.allocator.create(KernelState);
    defer std.testing.allocator.destroy(state);
    state.* = .{};
    const count = 65537;
    const extra = try std.testing.allocator.alloc(types.ProcessDescriptor, count - types.process_count);
    defer std.testing.allocator.free(extra);
    @memset(extra, .{});
    state.process_descriptors_extra = extra;
    state.process_capacity = count;
    const calls = [_]u64{ abi.runtime_abi.syscall_clock_gettime, abi.vm_abi.syscall_munmap, abi.vm_abi.syscall_mmap, abi.process_abi.syscall_process_signal_ctl, abi.runtime_abi.syscall_gettid, abi.process_abi.syscall_thread_set_gs_base };
    for ([_]usize{ 0, count - 1 }) |index| {
        for (calls) |nr| recordPrincipal(state, nr, @enumFromInt(index));
        const row = state.processDescriptorSlotConst(index).?.syscall_counts;
        try std.testing.expectEqual([_]u64{ 1, 1, 1, 1, 1, 1 }, row);
    }
    recordPrincipal(state, calls[0], .Device0);
    recordPrincipal(state, calls[0], @enumFromInt(count));
    recordPrincipal(state, 255, @enumFromInt(0));
    for ([_]usize{ 0, count - 1 }) |index|
        try std.testing.expectEqual([_]u64{ 1, 1, 1, 1, 1, 1 }, state.processDescriptorSlotConst(index).?.syscall_counts);
    const Concurrent = struct {
        fn run(s: *KernelState) void {
            for (0..10000) |_| recordPrincipal(s, abi.vm_abi.syscall_munmap, @enumFromInt(1));
        }
    };
    const worker = try std.Thread.spawn(.{}, Concurrent.run, .{state});
    Concurrent.run(state);
    worker.join();
    try std.testing.expectEqual(@as(u64, 20000), state.processDescriptorSlotConst(1).?.syscall_counts[1]);
}

// One terminal reason per dedicated attempt. SEND never enters that assembly
// branch; its opcode rejection is counted once at the ordinary scalar entry.
// Negative hints hide receiver state and must not be reported as poll misses.
pub const EntryReason = enum { poll, not_waiting, cpu, fd_transfer, words, opcode, hint, other, receive_capacity };
var reason_boot: [cpu_rows]EntryReason = @splat(.other);
var reasons: []EntryReason = &reason_boot;
pub fn entryBegin() void {
    if (comptime enabled) reasons[smp.currentCpuSlot()] = .other;
}
pub fn entryReason(reason: EntryReason) void {
    if (comptime enabled) reasons[smp.currentCpuSlot()] = reason;
}
pub fn entryEnd(success: bool) void {
    if (comptime !enabled) return;
    if (success) return; // The existing commit counters account for success.
    recordElapsed(@enumFromInt(@intFromEnum(Op.entry_fail_poll) + @intFromEnum(reasons[smp.currentCpuSlot()])), 0);
}
// Aggregate numerical route observations, not syscall totals: stage 0 is the
// dedicated probe and stage 2 is the sole K fallback. Stage 1 is no longer
// emitted. The K observation exposes traffic hidden by negative hints.
// E retains waiter nodes. Copy only identities; no pointer escapes the borrow.
const route_slots = 1024;
const RouteRow = [11]u64;
var route_boot: [cpu_rows][route_slots]RouteRow = @splat(@splat(@splat(0)));
var routes: [][route_slots]RouteRow = &route_boot;
var route_writers_boot: [cpu_rows]u64 = @splat(0);
var route_writers: []u64 = &route_writers_boot;
var route_writers_view = @import("diagnostic_cpu_storage.zig").Descriptor(u64).init(&route_writers_boot);
var route_view = @import("diagnostic_cpu_storage.zig").Descriptor([route_slots]RouteRow).init(&route_boot);
comptime {
    if (enabled) @export(&route_view, .{ .name = "ipc_entry_routes" });
    if (enabled) @export(&route_writers_view, .{ .name = "ipc_entry_route_writers" });
}
fn initCensus(count: usize) !void {
    if (comptime !enabled) return;
    reasons = try @import("diagnostic_cpu_storage.zig").grow(EntryReason, reasons, count, .other);
    routes = try @import("diagnostic_cpu_storage.zig").grow([route_slots]RouteRow, routes, count, @splat(@splat(0)));
    route_view = @TypeOf(route_view).init(routes);
    route_writers = try @import("diagnostic_cpu_storage.zig").grow(u64, route_writers, count, 0);
    route_writers_view = @TypeOf(route_writers_view).init(route_writers);
}
pub fn route(stage: u64, nr: u64, sender: types.PrincipalId, source_identity: u64, receive_capacity: u64, list: *const types.IpcWaitList, hint: ?types.IpcReceiverHint) void {
    if (comptime !enabled) return;
    list.assertHeld();
    // The stopped host may clear aggregates only when every writer is zero;
    // otherwise an interrupted row update could resurrect a partial identity.
    const writing = &route_writers[smp.currentCpuSlot()];
    @atomicStore(u64, writing, 1, .seq_cst);
    defer @atomicStore(u64, writing, 0, .seq_cst);
    var key: [10]u64 = .{ stage, nr, @intFromEnum(sender), 0, 0, 0, 0, source_identity, receive_capacity, std.math.maxInt(u64) };
    if (list.poll_head) |node| {
        key[3] = 1;
        key[4] = @intFromEnum(node.owner);
        key[5] = node.thread_index;
        key[6] = node.thread_generation;
    } else if (list.receive_head) |node| {
        const t = node.receive_target;
        key[3] = 2;
        key[4] = @intFromEnum(t.owner);
        key[5] = t.thread_index;
        key[6] = t.thread_generation;
    } else if (hint) |h| {
        if (h.active) {
            key[3] = 3;
            key[4] = @intFromEnum(h.owner);
            key[5] = h.thread_index;
            key[6] = h.thread_generation;
        }
    }
    if (key[3] != 0) key[9] = @import("scheduler_connection.zig").ipcDiagnosticTargetCpu(@intCast(key[5]), @intCast(key[6]), @enumFromInt(key[4]));
    var hash: u64 = 0;
    for (key) |word| hash = (hash ^ word) *% 1099511628211;
    const table = &routes[smp.currentCpuSlot()];
    for (0..route_slots) |step| {
        const row = &table[(hash +% step) % route_slots];
        if (row[10] == 0 or std.mem.eql(u64, row[0..10], &key)) {
            @memcpy(row[0..10], &key);
            row[10] += 1;
            return;
        }
    }
    recordElapsed(.entry_route_overflow, 0);
}

test "principal syscall counts dedicated rejection taxonomy is exclusive" {
    @memset(&counters_boot[0], 0);
    counters = &counters_boot;
    reasons = &reason_boot;
    inline for (@typeInfo(EntryReason).@"enum".fields) |field| {
        entryBegin();
        entryReason(@enumFromInt(field.value));
        entryEnd(false);
    }
    entryBegin();
    entryEnd(true);
    var total: u64 = 0;
    for (@intFromEnum(Op.entry_fail_poll)..@intFromEnum(Op.entry_route_overflow)) |i| {
        try std.testing.expectEqual(@as(u64, 1), counters[0][2 * i]);
        total += counters[0][2 * i];
    }
    try std.testing.expectEqual(@as(u64, 9), total);
    entryBegin();
    entryEnd(false);
    try std.testing.expectEqual(@as(u64, 2), counters[0][2 * @intFromEnum(Op.entry_fail_other)]);
}

test "principal syscall counts route overflow and source incarnation are explicit" {
    routes = &route_boot;
    route_writers = &route_writers_boot;
    counters = &counters_boot;
    @memset(&route_boot[0], @splat(0));
    @memset(&counters_boot[0], 0);
    const list: types.IpcWaitList = .{};
    for (0..route_slots) |generation|
        route(1, 74, @enumFromInt(1), generation, 19, &list, null);
    route(1, 74, @enumFromInt(1), 0, 19, &list, null);
    route(1, 74, @enumFromInt(1), route_slots, 19, &list, null);
    var total: u64 = 0;
    for (route_boot[0]) |row| total += row[10];
    try std.testing.expectEqual(@as(u64, route_slots + 1), total);
    try std.testing.expectEqual(@as(u64, 1), counters[0][2 * @intFromEnum(Op.entry_route_overflow)]);
    try std.testing.expectEqual(@as(u64, 0), route_writers[0]);
}
