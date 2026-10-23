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
};

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
comptime {
    if (enabled) {
        @export(&rows, .{ .name = "wake_trace_rows" });
        @export(&heads, .{ .name = "wake_trace_heads" });
        @export(&skipped_cpu, .{ .name = "wake_trace_skipped_cpu" });
    }
}

pub inline fn record(kind: Kind, key: u64, token: u64, thread: usize) void {
    if (comptime !enabled) return;
    const cpu = smp.currentCpuSlotIfKnown() orelse return;
    if (cpu >= count) {
        _ = @atomicRmw(u64, &skipped_cpu, .Add, 1, .monotonic);
        return;
    }
    // A local interrupt can nest here. Reserve first, then release-commit;
    // no lock can be acquired inside the scheduler/futex critical sections.
    const seq = @atomicRmw(u64, &heads[cpu], .Add, 1, .monotonic) + 1;
    const row = &rows[cpu][(seq - 1) % row_capacity];
    @atomicStore(u64, &row.seq, 0, .monotonic);
    row.tsc = platform.readTimestampCounter();
    row.key = key;
    row.token = token;
    row.thread = @truncate(thread);
    row.cpu = @truncate(cpu);
    row.kind = @intFromEnum(kind);
    @atomicStore(u64, &row.seq, seq, .release);
}

pub fn staticEnd() usize {
    if (comptime !enabled) return 0;
    return @max(@intFromPtr(&rows) + @sizeOf(@TypeOf(rows)),
        @max(@intFromPtr(&heads) + @sizeOf(@TypeOf(heads)),
            @intFromPtr(&skipped_cpu) + @sizeOf(@TypeOf(skipped_cpu))));
}
