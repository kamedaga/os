//! Bounded, opt-in wake timeline. Host tooling reads this from a stopped QEMU;
//! no guest ABI or synchronization state depends on it.
const root = @import("root");
const smp = @import("smp.zig");
const platform = @import("arch/x86_64/platform.zig");

pub const enabled = if (@hasDecl(root, "wake_trace_enabled")) root.wake_trace_enabled else false;
pub const cpu_capacity = 64;
pub const row_capacity = 8192;
pub const Kind = enum(u16) {
    futex_wait_enter = 1,
    futex_wait_reserved,
    futex_wait_block,
    futex_wait_return,
    futex_wake_enter,
    futex_wake_claim,
    futex_wake_handoff,
    futex_wake_return,
    wait_claim,
    ready_publish,
    ipi_send,
    ipi_receive,
    context_switch,
    user_return,
    ipc_call_enter,
    ipc_reply_enter,
    ipc_wait_begin,
    ipc_wait_end,
    fused_try,
    fused_commit,
    fused_reject,
    syscall_enter,
    syscall_exit,
    fd_wait_enter,
    fd_wait_register,
    fd_wait_repoll,
    fd_wait_block,
    page_fault,
    futex_lock,
    futex_word,
    futex_token,
    futex_record,
    futex_found,
    futex_retired,
    device_not_available,
    wake_select_begin,
    wake_select_end,
    wake_enqueue_begin,
    wake_enqueued,
    wake_notify_deferred,
    wake_policy_ineligible,
    wake_policy_keep,
    wake_policy_protected,
    wake_policy_switch,
    wake_local_projection,
    wake_local_eligible,
    wake_local_domain,
    wake_origin_domain,
    wake_origin_protection,
    wake_local_occupancy,
    wake_local_choice,
    wake_placed,
    wake_policy_service,
    wake_destination_domain,
    context_install,
    timer_receive,
};

// Numeric diagnostic identity, never an authority or a retained reference.
// Widths are explicit so a generation cannot alias an adjacent owner value.
pub inline fn identity(owner: u32, generation: u32) u64 {
    return (@as(u64, generation) << 32) | owner;
}

pub const Row = extern struct {
    // Published last. A stopped host reader rejects seq != the expected
    // per-CPU ring position, including rows overwritten during the window.
    seq: u64 = 0,
    tsc: u64 = 0,
    key: u64 = 0,
    token: u64 = 0,
    thread: u32 = 0,
    cpu: u16 = 0,
    kind: u16 = 0,
};

const count = if (enabled) cpu_capacity else 0;
var rows: [count][row_capacity]Row align(128) = @splat(@splat(.{}));
var heads: [count]u64 align(128) = @splat(0);
var skipped_cpu: u64 = 0;
var skipped_unknown: u64 = 0;
const kind_capacity = @typeInfo(Kind).@"enum".fields.len + 1; // Kind starts at 1.
var kind_totals: [count][kind_capacity]u64 align(128) = @splat(@splat(0));
var first_tsc: [count]u64 align(128) = @splat(0);
const totals_shape = [_]u64{ cpu_capacity, kind_capacity };
comptime {
    if (kind_capacity > 128 or @sizeOf(Row) != 40) @compileError("bounded wake trace shape changed");
    for (@typeInfo(Kind).@"enum".fields) |field| {
        if (field.value >= kind_capacity) @compileError("wake trace kind outside bounded totals");
    }
}
comptime {
    if (enabled) {
        @export(&rows, .{ .name = "wake_trace_rows" });
        @export(&heads, .{ .name = "wake_trace_heads" });
        @export(&skipped_cpu, .{ .name = "wake_trace_skipped_cpu" });
        @export(&skipped_unknown, .{ .name = "wake_trace_skipped_unknown" });
        @export(&kind_totals, .{ .name = "wake_trace_kind_totals" });
        @export(&first_tsc, .{ .name = "wake_trace_first_tsc" });
        @export(&totals_shape, .{ .name = "wake_trace_totals_shape" });
    }
}

pub inline fn record(kind: Kind, key: u64, token: u64, thread: usize) void {
    if (comptime !enabled) return;
    const cpu = smp.currentCpuSlotIfKnown() orelse {
        _ = @atomicRmw(u64, &skipped_unknown, .Add, 1, .monotonic);
        return;
    };
    if (cpu >= count) {
        _ = @atomicRmw(u64, &skipped_cpu, .Add, 1, .monotonic);
        return;
    }
    // A local interrupt can nest here. Reserve first, then release-commit;
    // no lock can be acquired inside the scheduler/futex critical sections.
    const stamp = platform.readTimestampCounter();
    if (@atomicLoad(u64, &first_tsc[cpu], .monotonic) == 0)
        _ = @cmpxchgStrong(u64, &first_tsc[cpu], 0, stamp, .monotonic, .monotonic);
    _ = @atomicRmw(u64, &kind_totals[cpu][@intFromEnum(kind)], .Add, 1, .monotonic);
    const seq = @atomicRmw(u64, &heads[cpu], .Add, 1, .monotonic) + 1;
    const row = &rows[cpu][(seq - 1) % row_capacity];
    @atomicStore(u64, &row.seq, 0, .monotonic);
    row.tsc = stamp;
    row.key = key;
    row.token = token;
    row.thread = @truncate(thread);
    row.cpu = @truncate(cpu);
    row.kind = @intFromEnum(kind);
    @atomicStore(u64, &row.seq, seq, .release);
}

pub fn staticEnd() usize {
    if (comptime !enabled) return 0;
    var end: usize = 0;
    inline for (.{ &rows, &heads, &skipped_cpu, &skipped_unknown, &kind_totals, &first_tsc, &totals_shape }) |ptr|
        end = @max(end, @intFromPtr(ptr) + @sizeOf(@TypeOf(ptr.*)));
    return end;
}

// A cold snapshot only; never called by record(). The shared log has 64KiB,
// so cap the export at 256 forty-byte rows encoded as 80 hex characters.
// Round-robin per-CPU tails expose migration without pretending to be a
// simultaneous stop. Sequence validation rejects a row overwritten mid-copy.
// The complete 8192-row CPU rings remain available to stopped QEMU tooling.
fn snapshotSequenceMatches(expected: u64, before: u64, copied: u64, after: u64) bool {
    return expected != 0 and before == expected and copied == expected and after == expected;
}

pub fn dumpRecent() void {
    if (comptime !enabled) return;
    const log = @import("kernel_log.zig");
    const std = @import("std");
    const cpus = @min(smp.cpuCapacity(), cpu_capacity);
    if (cpus == 0) return;
    const per_cpu: usize = @as(usize, 256) / cpus;
    var missing: usize = 0;
    for (0..cpus) |cpu| {
        const head = @atomicLoad(u64, &heads[cpu], .acquire);
        const first = head - @min(head, per_cpu) + 1;
        log.writeProfileFmt("wf11 trace cpu={} first={} head={}\n", .{ cpu, first, head });
        for (0..@min(head, per_cpu)) |offset| {
            const seq = first + offset;
            const row = &rows[cpu][(seq - 1) % row_capacity];
            if (@atomicLoad(u64, &row.seq, .acquire) != seq) { missing += 1; continue; }
            var copy: Row = undefined;
            inline for (@typeInfo(Row).@"struct".fields) |field| {
                @field(copy, field.name) = @atomicLoad(field.type, &@field(row.*, field.name), .monotonic);
            }
            if (!snapshotSequenceMatches(seq, seq, copy.seq, @atomicLoad(u64, &row.seq, .acquire))) { missing += 1; continue; }
            const hex = std.fmt.bytesToHex(std.mem.asBytes(&copy).*, .lower);
            log.writeProfileFmt("wf11 t {s}\n", .{hex});
        }
    }
    log.writeProfileFmt("wf11 trace END missing={} skipped_cpu={} cpus={} budget=256\n", .{ missing, @atomicLoad(u64, &skipped_cpu, .monotonic), cpus });
}

test "principal syscall counts wf11 stopped trace rejects unpublished and overwritten sequence" {
    const std = @import("std");
    try std.testing.expect(snapshotSequenceMatches(1,1,1,1));
    try std.testing.expect(!snapshotSequenceMatches(0,0,0,0));
    try std.testing.expect(!snapshotSequenceMatches(2,0,2,2));
    try std.testing.expect(!snapshotSequenceMatches(2,2,0,2));
    try std.testing.expect(!snapshotSequenceMatches(2,2,2,8194));
    try std.testing.expectEqual(@as(usize,40),@sizeOf(Row));
}

test "principal syscall counts wf11 trace identity separates owner and reused generations" {
    const std = @import("std");
    try std.testing.expect(identity(7,1) != identity(7,2));
    try std.testing.expect(identity(7,1) != identity(8,1));
    try std.testing.expectEqual(@as(u64,0xffffffffffffffff),identity(0xffffffff,0xffffffff));
    try std.testing.expectEqual(@as(u64,0xffffffff00000000),identity(0,0xffffffff));
}
