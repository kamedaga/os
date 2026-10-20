//! B3-only single-CPU experiment. No user pointer is trusted and no syscall
//! semantics change except the explicitly enabled empty diagnostic number.
//! Entry/exit scratch is supervisor-only under both roots, like trap frames.
//! Completed rows are immutable; exhaustion is reported, never wrapped.
const std = @import("std");
pub const enabled = if (@hasDecl(@import("root"), "b3_profile_enabled")) @import("root").b3_profile_enabled else false;
pub const empty_nr = 0x42330000;
pub const magic: u64 = 0x4233000000000000;
pub const words = 192;
const capacity = if (enabled) 1024 else 0;
var rows: [capacity][words]u64 = @splat(@splat(0));
var scratch: [*]u64 = undefined;
var count: usize = 0;
var active: ?usize = null;
var pending: ?usize = null;
var dropped: u64 = 0;
comptime {
    if (enabled) {
        @export(&scratch, .{ .name = "b3_scratch" });
        @export(&rows, .{ .name = "b3_trace" });
        @export(&dropped, .{ .name = "b3_dropped" });
    }
}
pub fn init(cpus: usize) !void {
    if (!enabled) return;
    if (cpus != 1) return error.B3RequiresSingleCpu;
    scratch = (try @import("arch/x86_64/cpu_runtime.zig").alloc(u64, 16)).ptr;
}
pub fn begin(frame: anytype) void {
    if (!enabled) return;
    if (pending) |i| {
        @memcpy(rows[i][4..9], scratch[4..9]);
        rows[i][189] = scratch[11];
        rows[i][15] = 1; // published only after the prior return finished
    }
    pending = null;
    active = null;
    if (frame.r14 & 0xffff000000000000 != magic) return;
    if (count == capacity) { dropped += 1; return; }
    const i = count;
    count += 1;
    active = i;
    @memcpy(rows[i][0..4], scratch[0..4]);
    rows[i][191] = scratch[9];
    rows[i][188] = scratch[10];
    rows[i][9] = frame.rax;
    rows[i][10] = frame.r14;
    rows[i][11] = @import("scheduler_connection.zig").currentThread();
    rows[i][12] = @import("scheduler_connection.zig").currentCr3();
}
pub fn finish() void {
    if (!enabled) return;
    pending = active;
    active = null;
}
// Host-only measurement sink. It is absent from the production ELF, and is
// activated solely around the measured operation (fixture setup is excluded).
pub const HostCost = if (@import("builtin").is_test) struct {
    pub var active = false;
    pub var last: [600]u64 = @splat(0);
    pub fn stamp() u64 {
        return asm volatile ("lfence; rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : [t] "={rax}" (-> u64) :: .{ .rdx = true, .memory = true });
    }
} else struct {};
pub fn now() u64 {
    if (comptime @import("builtin").is_test) if (HostCost.active) return HostCost.stamp();
    if (!enabled) return 0;
    return asm volatile ("lfence; rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : [t] "={rax}" (-> u64) :: .{ .rdx = true, .memory = true });
}
// Intervals may nest. The decoder splits at every boundary and assigns the
// most specific interval, preserving an explicit remainder without overlap.
pub fn span(kind: u64, start: u64, stop: u64) void {
    if (!enabled) return;
    const i = active orelse return;
    const n = rows[i][13];
    if (16 + (n + 1) * 3 > 188) { rows[i][14] += 1; return; }
    rows[i][16 + n * 3] = kind;
    rows[i][17 + n * 3] = start;
    rows[i][18 + n * 3] = stop;
    rows[i][13] += 1;
}
pub fn end(kind: u64, start: u64) void {
    if (comptime @import("builtin").is_test) if (HostCost.active) {
        HostCost.last[kind] = HostCost.stamp() - start;
        return;
    };
    span(kind, start, now());
}
// No calls, stack accesses or SIMD before XSAVE / after XRSTOR. RAX/RDX/R11
// are scratch here; every user GPR has already been saved by the entry stub.
pub fn stamp(comptime index: usize) []const u8 {
    if (!enabled) return "\n";
    return std.fmt.comptimePrint(
        \\
        \\mov b3_scratch(%rip), %r11
        \\lfence
        \\rdtsc
        \\shl $32, %rdx
        \\or %rdx, %rax
        \\mov %rax, {d}(%r11)
        \\
    , .{index * 8});
}
pub fn staticEnd() usize {
    if (!enabled) return 0;
    var result: usize = 0;
    inline for (.{ &rows, &scratch, &count, &active, &pending, &dropped }) |p| result = @max(result, @intFromPtr(p) + @sizeOf(@TypeOf(p.*)));
    return result;
}
