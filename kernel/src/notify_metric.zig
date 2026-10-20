//! Bounded profile-only NOTIFY census. The syscall owns its CPU row until
//! return or the lock-drop callback; no active scope survives a stack switch.
//! Wake stamps contain numbers only. T publishes them at the winning token
//! claim, before a remote CPU may run the receiver. Its CPU pin then consumes
//! the stamp once, at the last common CR3 helper before SYSRET/IRET.
const std = @import("std");
const smp = @import("smp.zig");
const metric = @import("ipc_metric.zig");
const storage = @import("diagnostic_cpu_storage.zig");
const abi = @import("kernel_abi_root").ipc_abi;
pub const enabled = metric.enabled;
const slots = 8192;
const words = 56;
const Key = [9]u64;
const Row = [words]u64;
const Table = [slots]Row;
const unknown = std.math.maxInt(u64);
const count = if (enabled) 1 else 0;
const Active = struct {
    start: u64 = 0,
    key: Key = @splat(0),
    sending: bool = false,
    coalesced: bool = false,
};
var boot_tables: [count]Table = @splat(@splat(@splat(0)));
var tables: []Table = &boot_tables;
var boot_active: [count]Active = @splat(.{});
var active: []Active = &boot_active;
// sequence, overflow, abandoned scope, overwritten stamp, stale generation,
// backwards TSC, active row writer. Host readers stop all CPUs and never clear these tables.
var boot_meta: [count][7]u64 = @splat(@splat(0));
var meta: [][7]u64 = &boot_meta;
var table_view = storage.Descriptor(Table).init(&boot_tables);
var meta_view = storage.Descriptor([7]u64).init(&boot_meta);
var layout = [_]u64{ slots, words, 9, 256 };
const Segments = [32][3]u64;
var boot_segments: [count]Segments = @splat(@splat(@splat(0)));
var segments: []Segments = &boot_segments;
var segments_view = storage.Descriptor(Segments).init(&boot_segments);
var boot_fast: [count][12]u64 = @splat(@splat(0));
var fast: [][12]u64 = &boot_fast;
var fast_view = storage.Descriptor([12]u64).init(&boot_fast);
comptime {
    if (enabled) {
        @export(&table_view, .{ .name = "notify_perf_rows" });
        @export(&meta_view, .{ .name = "notify_perf_meta" });
        @export(&layout, .{ .name = "notify_perf_layout" });
        @export(&segments_view, .{ .name = "notify_segments" });
        @export(&fast_view, .{ .name = "notify_fast_counts" });
    }
}
pub fn initCpuStorage(n: usize) !void {
    if (comptime !enabled) return;
    segments = try storage.grow(Segments, segments, n, @splat(@splat(0)));
    segments_view = @TypeOf(segments_view).init(segments);
    tables = try storage.grow(Table, tables, n, @splat(@splat(0)));
    fast = try storage.grow([12]u64, fast, n, @splat(0));
    fast_view = @TypeOf(fast_view).init(fast);
    active = try storage.grow(Active, active, n, .{});
    meta = try storage.grow([7]u64, meta, n, @splat(0));
    table_view = @TypeOf(table_view).init(tables);
    meta_view = @TypeOf(meta_view).init(meta);
}
pub fn staticEnd() usize {
    if (comptime !enabled) return 0;
    var end: usize = 0;
    inline for (.{ &boot_segments, &segments, &segments_view, &boot_tables, &tables, &boot_active, &active, &boot_meta, &meta, &table_view, &meta_view, &layout, &boot_fast, &fast, &fast_view }) |p|
        end = @max(end, @intFromPtr(p) + @sizeOf(@TypeOf(p.*)));
    return end;
}
fn cpu() usize {
    return smp.currentCpuSlot();
}
fn add(p: *u64, n: u64) void {
    _ = @atomicRmw(u64, p, .Add, n, .monotonic);
}
fn rowFor(c: usize, key: Key) ?usize {
    var hash: u64 = 0;
    for (key) |v| hash = (hash ^ v) *% 1099511628211;
    for (0..slots) |i| {
        const slot = (hash +% i) % slots;
        const row = &tables[c][slot];
        // nr is nonzero and keys never change after publication. Remote
        // return hooks only update numeric aggregates of an existing row.
        if (row[0] == 0) {
            @memcpy(row[0..9], &key);
            return slot;
        }
        if (std.mem.eql(u64, row[0..9], &key)) return slot;
    }
    add(&meta[c][1], 1);
    return null;
}
pub fn begin(nr: u64, owner: u64, thread: usize, generation: u32) void {
    if (comptime !enabled) return;
    const c = cpu();
    // Snapshot readers must not mistake a partly published scope for idle.
    @atomicStore(u64, &meta[c][6], 1, .seq_cst);
    defer @atomicStore(u64, &meta[c][6], 0, .seq_cst);
    if (active[c].start != 0) add(&meta[c][2], 1);
    active[c] = .{};
    if (nr != abi.syscall_ipc_notify and nr != abi.syscall_ipc_notify_wait) return;
    meta[c][0] +%= 1;
    active[c] = .{
        .start = metric.timestamp(),
        .key = .{ nr, owner, (@as(u64, generation) << 32) | thread, unknown, 0, 2, 0, 0, 0 },
    };
}
// Attempts/hits by opcode, then committed local/remote/immediate/block,
// misses for input/K/authority/scheduler. Distinct from the route cost table.
pub fn fastCount(index: usize) void {
    if (comptime enabled) fast[cpu()][index] +%= 1;
}
pub fn abandonPreflight() void {
    if (comptime !enabled) return;
    const c = cpu();
    if (active[c].start != 0) meta[c][0] -%= 1;
    active[c].start = 0;
    active[c].sending = false;
}
pub fn sending(value: bool) void {
    if (comptime enabled) {
        const a = &active[cpu()];
        a.sending = value;
        if (a.key[0] == abi.syscall_ipc_notify_wait and value) a.key[8] |= 16;
    }
}
pub fn coalesced(value: bool) void {
    if (comptime enabled) active[cpu()].coalesced = value;
}
pub fn outcome(value: u64) void {
    if (comptime enabled) active[cpu()].key[7] = value;
}
pub fn waitOutcome(value: u64) void {
    if (comptime enabled) active[cpu()].key[8] = (active[cpu()].key[8] & 16) | value;
}
pub fn target(t: anytype, mode: u64) void {
    if (comptime !enabled) return;
    const c = cpu();
    const a = &active[c];
    if (a.start == 0) return;
    const target_cpu = @import("scheduler_connection.zig").ipcDiagnosticTargetCpu(t.thread_index, t.thread_generation, t.owner);
    a.key[3] = @intFromEnum(t.owner);
    a.key[4] = (@as(u64, t.thread_generation) << 32) | t.thread_index;
    a.key[5] = if (target_cpu == unknown) 2 else @intFromBool(target_cpu != c);
    a.key[6] = mode;
}
pub fn finish() void {
    if (comptime !enabled) return;
    const c = cpu();
    const a = &active[c];
    if (a.start == 0) return;
    @atomicStore(u64, &meta[c][6], 1, .seq_cst);
    defer @atomicStore(u64, &meta[c][6], 0, .seq_cst);
    const elapsed = metric.timestamp() -% a.start;
    if (rowFor(c, a.key)) |slot| {
        const row = &tables[c][slot];
        add(&row[9], 1);
        add(&row[10], elapsed);
        if (row[11] == 0 or elapsed < row[11]) row[11] = elapsed;
        row[12] = @max(row[12], elapsed);
        if (a.coalesced) add(&row[51], 1);
    }
    a.start = 0;
    a.sending = false;
}
pub const Stamp = struct {
    start: u64 = 0,
    cpu: usize = 0,
    slot: usize = 0,
    generation: u32 = 0,
    token: u64 = 0,
    claimed_at: u64 = 0,
};
/// Only called after validating and winning the exact token under T. This
/// also covers poll completions; an unrelated timeout/signal cannot inherit
/// an active sender because only its masked syscall sets `sending`.
pub fn claimed(stamp: *Stamp, thread: usize, generation: u32, owner: u64, token: u64, target_cpu: usize, direct_wait: bool) void {
    if (comptime !enabled) return;
    const c = cpu();
    const a = &active[c];
    if (a.start == 0 or !a.sending) return;
    @atomicStore(u64, &meta[c][6], 1, .seq_cst);
    defer @atomicStore(u64, &meta[c][6], 0, .seq_cst);
    var key = a.key;
    key[3] = owner;
    key[4] = (@as(u64, generation) << 32) | thread;
    key[5] = @intFromBool(target_cpu != c);
    if (key[6] != 1) key[6] = 2;
    // Keep dedicated/generic origin and notify_peer in winning-wake rows.
    key[7] = if (a.key[7] >= 200) a.key[7] + 100 else 100;
    key[8] = (key[8] & 16) | (if (direct_wait) @as(u64, 32) else 0);
    const slot = rowFor(c, key) orelse return;
    add(&tables[c][slot][13], 1);
    // Sample within each route, so a periodic syscall mix cannot hide an
    // entire sender/receiver pair behind the global sampling phase.
    if (tables[c][slot][13] % 256 != 1) return;
    if (stamp.start != 0) add(&meta[c][3], 1);
    stamp.* = .{ .start = a.start, .cpu = c, .slot = slot, .generation = generation, .token = token, .claimed_at = metric.timestamp() };
    add(&tables[c][slot][14], 1);
}
/// Runs with restored user XState: scalar loads/stores/atomics only. Never
/// copy an aggregate, allocate, format, or call a scheduler locking helper.
/// The remaining CR3/GPR/return instructions are a fixed unmeasured tail.
pub noinline fn userReturn(stamp: *Stamp, generation: u32) void {
    if (comptime !enabled) return;
    const start = stamp.start;
    if (start == 0) return;
    const c = cpu();
    @atomicStore(u64, &meta[c][6], 1, .seq_cst);
    defer @atomicStore(u64, &meta[c][6], 0, .seq_cst);
    stamp.start = 0;
    if (stamp.generation != generation or stamp.token == 0) {
        add(&meta[stamp.cpu][4], 1);
        return;
    }
    const end = metric.timestamp();
    if (end < start or end < stamp.claimed_at) {
        add(&meta[stamp.cpu][5], 1);
        return;
    }
    const ticks = end - start;
    const row = &tables[stamp.cpu][stamp.slot];
    const wake_ticks = end -% stamp.claimed_at;
    add(&row[52], wake_ticks);
    _ = @atomicRmw(u64, &row[53], .Max, wake_ticks, .monotonic);
    _ = @atomicRmw(u64, &row[54], .Max, std.math.maxInt(u64) - wake_ticks, .monotonic);
    if (cpu() != stamp.cpu) add(&row[55], 1);
    add(&row[15], 1);
    add(&row[16], ticks);
    // Values are one greater so zero remains the uninitialized minimum.
    _ = @atomicRmw(u64, &row[17], .Max, std.math.maxInt(u64) - ticks, .monotonic);
    _ = @atomicRmw(u64, &row[18], .Max, ticks, .monotonic);
    recordLatencyHistogram(row, ticks);
}

inline fn recordLatencyHistogram(row: *Row, ticks: u64) void {
    // @min narrows its result to u5 even with a u64 operand. Widen before
    // adding the histogram base: u5 addition wraps into immutable route keys
    // for bucket >= 13 in unchecked builds, corrupting the entire census.
    const bucket: usize = @min(@as(u64, 31), 63 - @clz(ticks | 1));
    add(&row[19 + bucket], 1);
}

test "principal syscall counts notify histogram high buckets preserve route keys and other aggregates" {
    var row: Row = @splat(0);
    for (row[0..19], 0..) |*value, i| value.* = 100 + i;
    const original = row;
    inline for (.{ 0, 12, 13, 14, 30, 31, 63 }) |bit| {
        const before = row;
        recordLatencyHistogram(&row, @as(u64, 1) << bit);
        for (row, 0..) |value, i| {
            const expected = before[i] + @as(u64, @intFromBool(i == 19 + @min(bit, 31)));
            try std.testing.expectEqual(expected, value);
        }
    }
    try std.testing.expectEqualSlices(u64, original[0..19], row[0..19]);
}

test "principal syscall counts notify claim to return clock reversal is excluded before unsigned subtraction" {
    if (comptime !enabled) return;
    tables = &boot_tables;
    meta = &boot_meta;
    boot_tables[0][0] = @splat(0);
    boot_meta[0] = @splat(0);
    var stamp: Stamp = .{ .start = 1, .claimed_at = std.math.maxInt(u64), .generation = 1, .token = 1 };
    userReturn(&stamp, 1);
    try std.testing.expectEqual(@as(u64, 1), meta[0][5]);
    try std.testing.expectEqual(@as(u64, 0), stamp.start);
    for (boot_tables[0][0]) |value| try std.testing.expectEqual(@as(u64, 0), value);
}

test "principal syscall counts notify census exact completion incarnation and single return" {
    if (comptime !enabled) return;
    tables = &boot_tables;
    active = &boot_active;
    meta = &boot_meta;
    @memset(&boot_tables[0], @splat(0));
    boot_meta[0] = @splat(0);
    active[0] = .{};
    var stamp: Stamp = .{};
    begin(abi.syscall_ipc_notify, 1, 4, 7);
    sending(true);
    claimed(&stamp, 8, 9, 2, 11, 1, false);
    const slot = stamp.slot;
    try std.testing.expectEqual(@as(u64, 1), tables[0][slot][13]);
    userReturn(&stamp, 9);
    userReturn(&stamp, 9);
    try std.testing.expectEqual(@as(u64, 1), tables[0][slot][15]);
    for (0..256) |_| claimed(&stamp, 8, 9, 2, 12, 1, false);
    userReturn(&stamp, 10);
    try std.testing.expectEqual(@as(u64, 1), meta[0][4]);
    finish();
    try std.testing.expectEqual(@as(u64, 0), active[0].start);
    claimed(&stamp, 8, 9, 2, 12, 1, false);
    try std.testing.expectEqual(@as(u64, 0), stamp.start);
}

test "principal syscall counts notify preflight abandonment and peer flag are exact" {
    if (comptime !enabled) return;
    tables = &boot_tables;
    active = &boot_active;
    meta = &boot_meta;
    @memset(&boot_tables[0], @splat(0));
    boot_meta[0] = @splat(0);
    active[0] = .{};
    begin(abi.syscall_ipc_notify_wait, 1, 4, 7);
    sending(true);
    waitOutcome(3);
    try std.testing.expectEqual(@as(u64, 19), active[0].key[8]);
    abandonPreflight();
    try std.testing.expectEqual(@as(u64, 0), meta[0][0]);
    try std.testing.expectEqual(@as(u64, 0), active[0].start);
    begin(abi.syscall_ipc_notify_wait, 1, 4, 7);
    sending(true);
    waitOutcome(3);
    outcome(204);
    var stamp: Stamp = .{};
    claimed(&stamp, 8, 9, 2, 11, 0, false);
    try std.testing.expectEqual(@as(u64, 304), tables[0][stamp.slot][7]);
    try std.testing.expectEqual(@as(u64, 16), tables[0][stamp.slot][8]);
    finish();
    try std.testing.expectEqual(@as(u64, 1), meta[0][0]);
}

// Profile-only intervals are CPU-local with IF clear. No borrowed reference
// survives a call; snapshot readers stop every CPU before copying these rows.
pub fn segment(comptime index: usize, start: u64) void {
    if (comptime !enabled) return;
    const row = &segments[cpu()][index];
    row[0] +%= 1;
    const elapsed = metric.timestamp() -% start;
    row[1] +%= elapsed;
    row[2] = @max(row[2], elapsed);
}
