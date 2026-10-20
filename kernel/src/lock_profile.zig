//! Only observations: never changes exclusion, ownership, or scheduler policy.
const root = @import("root");
pub const enabled = if (@hasDecl(root, "smp_profile_enabled")) root.smp_profile_enabled else false;
const smp = @import("smp.zig");
const platform = @import("arch/x86_64/platform.zig");
const hist = @import("smp_histogram.zig");
pub const Kind = enum(u8) { other, process, fork, exec, map, unmap, fd, ipc, futex, runtime, capsule, irq, teardown, fd_table, fd_close, fd_dup, fd_get_info, fd_set_flags, fd_read, fd_write, fd_readv, fd_writev, fd_fcntl, fd_poll, fd_wait_many, fd_ioctl, fd_stat, eventfd_create, pipe_create, timerfd_create, timerfd_settime, timerfd_gettime, vmo_create, vmo_grow, vmo_cache_policy, vmo_create_page_view, vmo_revoke, ipc_endpoint_create, ipc_channel_create, ipc_send, ipc_recv, ipc_call, ipc_reply, ipc_recv_wait, ipc_notify, ipc_notify_wait };
pub const Stage = enum(u8) { prepare, commit, retire, copy, zero, pmm, pmm_wait, pt_prepare, ipi_ack, vm_wait, timer_publish };
const kinds = @typeInfo(Kind).@"enum".fields.len;
const stages = @typeInfo(Stage).@"enum".fields.len;
const rows = if (enabled) 1 else 0;
// K distributions retain syscall attribution; nested stage distributions share
// storage, with per-kind exclusive totals below (not a huge Cartesian product).
const series = kinds * 2 + stages;
var distributions_boot: [rows][series]hist.Histogram align(128) = @splat(@splat(.{}));
var distributions: [][series]hist.Histogram = &distributions_boot;
var distributions_view = @import("diagnostic_cpu_storage.zig").Descriptor([series]hist.Histogram).init(&distributions_boot);
var stage_totals_boot: [rows][kinds][stages][3]u64 align(128) = @splat(@splat(@splat(@splat(0))));
var stage_totals: [][kinds][stages][3]u64 = &stage_totals_boot;
var stage_totals_view = @import("diagnostic_cpu_storage.zig").Descriptor([kinds][stages][3]u64).init(&stage_totals_boot);
var events_boot: [rows][kinds][4]u64 align(128) = @splat(@splat(@splat(0)));
var events: [][kinds][4]u64 = &events_boot;
var events_view = @import("diagnostic_cpu_storage.zig").Descriptor([kinds][4]u64).init(&events_boot);
var free_floor_boot: [rows]u64 = @splat(0);
var free_floor: []u64 = &free_floor_boot;
var free_floor_view = @import("diagnostic_cpu_storage.zig").Descriptor(u64).init(&free_floor_boot);
var writers_boot: [rows]u64 = @splat(0);
var writers: []u64 = &writers_boot;
var writers_view = @import("diagnostic_cpu_storage.zig").Descriptor(u64).init(&writers_boot);
var epoch_word: if (enabled) u64 else void = if (enabled) 0 else {};
// A synchronous diagnostic borrower supplies count/ticks sinks. The PMM
// leaf knows only allocation/return, never the subsystem requesting pages.
// The borrower must restore the previous sink before yielding or dropping K;
// storage remains stable for the whole scope and there is no callback into it.
pub const PmmScopeSink = struct { allocated: *[2]u64, returned: *[2]u64 };
pub fn setPmmScopeSink(sink: ?PmmScopeSink) void {
    if (comptime !enabled) return;
    state[cpuSlot()].pmm_scope_sink = sink;
}
pub fn pmmScopeTimestamp() u64 {
    if (comptime !enabled) return 0;
    return if (state[cpuSlot()].pmm_scope_sink != null) now() else 0;
}
pub fn pmmScopeEnd(returned: bool, start: u64) void {
    if (comptime !enabled) return;
    const sink = state[cpuSlot()].pmm_scope_sink orelse return;
    const elapsed = now() -% start;
    const pair = if (returned) sink.returned else sink.allocated;
    _ = @atomicRmw(u64, &pair[0], .Add, 1, .monotonic);
    _ = @atomicRmw(u64, &pair[1], .Add, elapsed, .monotonic);
}
const State = struct { pmm_scope_sink: ?PmmScopeSink = null, active: bool = false, kind: Kind = .other, depth: usize = 0, owner_depth: usize = 0, serial: u64 = 0, children: u64 = 0, pending: [stages][3]u64 = @splat(@splat(0)), hold: Token = .{} };
var state_boot: [rows]State = @splat(.{});
var state: []State = &state_boot;
pub const Token = struct { start: u64 = 0, epoch: u64 = 0, cpu: usize = 0, serial: u64 = 0, children: u64 = 0, active: bool = false };
pub const OptionalKind = if (enabled) Kind else void;
pub const OptionalToken = if (enabled) Token else void;
var layout = [_]u64{ rows, kinds, stages, series, hist.bucket_count, @sizeOf(hist.Histogram), @offsetOf(hist.Histogram, "count") };
const labels = blk: {
    var text: []const u8 = "";
    for (@typeInfo(Kind).@"enum".fields) |f| text = text ++ f.name ++ "\x00";
    for (@typeInfo(Stage).@"enum".fields) |f| text = text ++ f.name ++ "\x00";
    break :blk text;
};
comptime {
    if (enabled) {
        @export(&free_floor_view, .{ .name = "lock_profile_free_floor" });
        @export(&writers_view, .{ .name = "lock_profile_writers" });
        @export(&distributions_view, .{ .name = "lock_profile_histograms" });
        @export(&stage_totals_view, .{ .name = "lock_profile_stages" });
        @export(&events_view, .{ .name = "lock_profile_events" });
        @export(&epoch_word, .{ .name = "lock_profile_epoch" });
        @export(&layout, .{ .name = "lock_profile_layout" });
        @export(labels.ptr, .{ .name = "lock_profile_labels" });
    }
}
// No AP or interrupt may observe the BSP-only migration. Preserve active
// tokens and pending attribution rather than silently dropping boot samples.
pub fn initCpuStorage(count: usize) !void {
    if (comptime !enabled) return;
    const storage = @import("diagnostic_cpu_storage.zig");
    distributions = try storage.grow([series]hist.Histogram, distributions, count, @splat(.{}));
    stage_totals = try storage.grow([kinds][stages][3]u64, stage_totals, count, @splat(@splat(@splat(0))));
    events = try storage.grow([kinds][4]u64, events, count, @splat(@splat(0)));
    free_floor = try storage.grow(u64, free_floor, count, 0);
    writers = try storage.grow(u64, writers, count, 0);
    state = try storage.grow(State, state, count, .{});
    detail = try storage.grow([detail_words]u64, detail, count, @splat(0));
    detail_view = @TypeOf(detail_view).init(detail);
    storage_detail = try storage.grow([storage_words]u64, storage_detail, count, @splat(0));
    storage_view = @TypeOf(storage_view).init(storage_detail);
    copy_detail = try storage.grow([copy_words]u64, copy_detail, count, @splat(0));
    copy_view = @TypeOf(copy_view).init(copy_detail);
    layout[0] = count;
    distributions_view = @TypeOf(distributions_view).init(distributions);
    stage_totals_view = @TypeOf(stage_totals_view).init(stage_totals);
    events_view = @TypeOf(events_view).init(events);
    free_floor_view = @TypeOf(free_floor_view).init(free_floor);
    writers_view = @TypeOf(writers_view).init(writers);
}

// Explicit caller arguments avoid CPU-sticky attribution across IRQ/migration.
// Rows are CPU-local and cache-line sized; observations add no allocator lock.
pub const PmmCaller = enum { single, bounded, contiguous, physical, identity, vmo_backing, pt_storage, returned, invocation, slot_directory, slot_metadata, slot_body, reference_index, fd_table_storage, process_storage, thread_storage, address_space_storage, vma_storage };
pub const PmmRoute = enum { hit, refill, tree, reclaim, failed, return_cache, return_tree, return_spill };
const detail_words = 8 * 8 * 5 + 5 * 8 * 5 + 8;
var detail_boot: [rows][detail_words]u64 align(64) = @splat(@splat(0));
var detail: [][detail_words]u64 = &detail_boot;
var detail_view = @import("diagnostic_cpu_storage.zig").Descriptor([detail_words]u64).init(&detail_boot);
comptime {
    if (enabled) @export(&detail_view, .{ .name = "pmm_profile_detail" });
}
pub fn pmmDetail(t: OptionalToken, caller: PmmCaller, route: PmmRoute, pages: usize, failed: bool) void {
    if (comptime !enabled) return;
    _ = @atomicRmw(u64, &writers[t.cpu], .Add, 1, .monotonic);
    defer _ = @atomicRmw(u64, &writers[t.cpu], .Sub, 1, .monotonic);
    const row = &detail[t.cpu];
    const entry: usize = if (caller == .returned) 4 else if (caller == .physical) 1 else if (caller == .single) 0 else if (pages == 1) 2 else 3;
    const offsets = [_]usize{ (@as(usize, @intFromEnum(if (@intFromEnum(caller) >= 8) PmmCaller.identity else caller)) * 8 + @intFromEnum(route)) * 5, 320 + (entry * 8 + @intFromEnum(route)) * 5 };
    const elapsed = now() -% t.start;
    for (offsets) |offset| {
        if (t.epoch != epoch()) {
            _ = @atomicRmw(u64, &row[offset + 3], .Add, 1, .monotonic);
            continue;
        }
        _ = @atomicRmw(u64, &row[offset], .Add, 1, .monotonic);
        _ = @atomicRmw(u64, &row[offset + 1], .Add, elapsed, .monotonic);
        _ = @atomicRmw(u64, &row[offset + 2], .Add, pages, .monotonic);
        _ = @atomicRmw(u64, &row[offset + 4], .Add, @intFromBool(failed), .monotonic);
    }
}
pub fn pmmCas(t: OptionalToken, attempts: usize, retries: usize, mismatch: bool) void {
    if (comptime !enabled) return;
    _ = @atomicRmw(u64, &writers[t.cpu], .Add, 1, .monotonic);
    defer _ = @atomicRmw(u64, &writers[t.cpu], .Sub, 1, .monotonic);
    const row = &detail[t.cpu];
    if (t.epoch != epoch()) {
        _ = @atomicRmw(u64, &row[526], .Add, 1, .monotonic);
        return;
    }
    const values = [_]u64{ attempts, retries, @intFromBool(mismatch), now() -% t.start, 1, @intFromBool(!mismatch) };
    for (values, 0..) |v, i| _ = @atomicRmw(u64, &row[520 + i], .Add, v, .monotonic);
}
pub fn classify(nr: u64) Kind {
    if (!enabled) return .other;
    const abi = @import("kernel_abi_root");
    if (nr == abi.process_abi.syscall_process_clone) return .fork;
    if (nr == abi.process_abi.syscall_process_exec_from) return .exec;
    if (nr == abi.vm_abi.syscall_mmap or nr == abi.process_abi.syscall_process_map or nr == abi.process_abi.syscall_process_map_batch) return .map;
    if (nr == abi.vm_abi.syscall_munmap or nr == abi.process_abi.syscall_process_unmap) return .unmap;
    if (abi.process_abi.isProcessSyscall(nr)) return .process;
    if (nr == abi.fd_abi.syscall_fd_table) return .fd_table;
    if (nr == abi.fd_abi.syscall_fd_close) return .fd_close;
    if (nr == abi.fd_abi.syscall_fd_dup) return .fd_dup;
    if (nr == abi.fd_abi.syscall_fd_get_info) return .fd_get_info;
    if (nr == abi.fd_abi.syscall_fd_set_flags) return .fd_set_flags;
    if (nr == abi.fd_abi.syscall_fd_read) return .fd_read;
    if (nr == abi.fd_abi.syscall_fd_write) return .fd_write;
    if (nr == abi.fd_abi.syscall_fd_readv) return .fd_readv;
    if (nr == abi.fd_abi.syscall_fd_writev) return .fd_writev;
    if (nr == abi.fd_abi.syscall_fd_fcntl) return .fd_fcntl;
    if (nr == abi.fd_abi.syscall_fd_poll) return .fd_poll;
    if (nr == abi.fd_abi.syscall_fd_wait_many) return .fd_wait_many;
    if (nr == abi.fd_abi.syscall_fd_ioctl) return .fd_ioctl;
    if (nr == abi.fd_abi.syscall_fd_stat) return .fd_stat;
    if (nr == abi.fd_abi.syscall_eventfd_create) return .eventfd_create;
    if (nr == abi.fd_abi.syscall_pipe_create) return .pipe_create;
    if (nr == abi.fd_abi.syscall_timerfd_create) return .timerfd_create;
    if (nr == abi.fd_abi.syscall_timerfd_settime) return .timerfd_settime;
    if (nr == abi.fd_abi.syscall_timerfd_gettime) return .timerfd_gettime;
    if (nr == abi.fd_abi.syscall_vmo_create) return .vmo_create;
    if (nr == abi.fd_abi.syscall_vmo_grow) return .vmo_grow;
    if (nr == abi.fd_abi.syscall_vmo_cache_policy) return .vmo_cache_policy;
    if (nr == abi.fd_abi.syscall_vmo_create_page_view) return .vmo_create_page_view;
    if (nr == abi.fd_abi.syscall_vmo_revoke) return .vmo_revoke;
    if (abi.fd_abi.isFdSyscall(nr)) return .fd;
    if (nr == abi.ipc_abi.syscall_ipc_endpoint_create) return .ipc_endpoint_create;
    if (nr == abi.ipc_abi.syscall_ipc_channel_create) return .ipc_channel_create;
    if (nr == abi.ipc_abi.syscall_ipc_send) return .ipc_send;
    if (nr == abi.ipc_abi.syscall_ipc_recv) return .ipc_recv;
    if (nr == abi.ipc_abi.syscall_ipc_call) return .ipc_call;
    if (nr == abi.ipc_abi.syscall_ipc_reply) return .ipc_reply;
    if (nr == abi.ipc_abi.syscall_ipc_recv_wait) return .ipc_recv_wait;
    if (nr == abi.ipc_abi.syscall_ipc_notify) return .ipc_notify;
    if (nr == abi.ipc_abi.syscall_ipc_notify_wait) return .ipc_notify_wait;
    if (abi.ipc_abi.isIpcSyscall(nr)) return .ipc;
    if (nr == abi.runtime_abi.syscall_futex_wait or nr == abi.runtime_abi.syscall_futex_wake or nr == abi.runtime_abi.syscall_futex_wake_wait) return .futex;
    if (abi.runtime_abi.isRuntimeSyscall(nr)) return .runtime;
    if (abi.capsule_abi.isCapsuleSyscall(nr)) return .capsule;
    return .other;
}
fn epoch() u64 {
    return @atomicLoad(u64, &epoch_word, .acquire);
}
fn cpuSlot() usize {
    if (@import("builtin").is_test) return checkedSlot(root.profile_test_cpu);
    return checkedSlot(smp.currentCpuSlot());
}
fn checkedSlot(slot: usize) usize {
    if (slot >= state.len) @panic("boot CPU exceeds lock profile capacity");
    return slot;
}
fn now() u64 {
    if (@import("builtin").is_test) return root.profile_test_time;
    var lo: u32 = 0;
    var hi: u32 = 0;
    asm volatile ("lfence; rdtsc; lfence"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
        :
        : .{ .memory = true });
    return (@as(u64, hi) << 32) | lo;
}
pub fn begin() OptionalToken {
    if (comptime !enabled) return {};
    // Read epoch first: a host stop between these reads must leave an old
    // epoch, never a pre-stop timestamp paired with the new epoch.
    const started_epoch = epoch();
    return .{ .start = now(), .epoch = started_epoch, .cpu = cpuSlot(), .active = true };
}
pub fn event(kind: Kind, column: usize) void {
    if (comptime enabled) {
        _ = @atomicRmw(u64, &events[cpuSlot()][@intFromEnum(kind)][column], .Add, 1, .monotonic);
    }
}
fn record(t: Token, index: usize, kind: Kind, elapsed: u64, exclusive: u64) void {
    _ = @atomicRmw(u64, &writers[t.cpu], .Add, 1, .monotonic);
    defer {
        _ = @atomicRmw(u64, &writers[t.cpu], .Sub, 1, .monotonic);
    }
    const h = &distributions[t.cpu][index];
    if (t.epoch != epoch()) {
        _ = @atomicRmw(u64, &h.excluded, .Add, 1, .monotonic);
        event(kind, 3);
        return;
    }
    h.record(elapsed, exclusive);
}
pub fn acquired(kind: Kind, wait: OptionalToken, is_try: bool) void {
    if (comptime enabled) {
        const hold = begin();
        if (!is_try) record(wait, @as(usize, @intFromEnum(kind)) * 2, kind, hold.start -% wait.start, 0);
        const cpu = cpuSlot();
        const s = &state[cpu];
        s.active = true;
        s.kind = kind;
        s.owner_depth = s.depth;
        s.serial +%= 1;
        s.children = 0;
        s.pending = @splat(@splat(0));
        s.hold = hold;
        copyEnd(.invocation, copyTimestamp());
    }
}
pub fn release() void {
    if (comptime enabled) {
        const cpu = cpuSlot();
        _ = @atomicRmw(u64, &writers[cpu], .Add, 1, .monotonic);
        defer {
            _ = @atomicRmw(u64, &writers[cpu], .Sub, 1, .monotonic);
        }
        const s = &state[cpuSlot()];
        if (s.hold.epoch == epoch()) {
            for (&s.pending, 0..) |*row, stage| for (row, 0..) |v, col| {
                _ = @atomicRmw(u64, &stage_totals[s.hold.cpu][@intFromEnum(s.kind)][stage][col], .Add, v, .monotonic);
            };
        }
        record(s.hold, @as(usize, @intFromEnum(s.kind)) * 2 + 1, s.kind, now() -% s.hold.start, 0);
        s.active = false;
    }
}
pub fn irqEnter(from_user: bool) void {
    if (comptime enabled) {
        if (!from_user) state[cpuSlot()].depth += 1;
    }
}
pub fn irqLeave(from_user: bool) void {
    if (comptime enabled) {
        if (!from_user) state[cpuSlot()].depth -= 1;
    }
}
pub fn stageBegin() OptionalToken {
    if (comptime !enabled) return {};
    const cpu = cpuSlot();
    const s = &state[cpu];
    // An IRQ interrupting an owner is not part of that owner's copy/PMM work.
    if (!s.active or s.depth != s.owner_depth) return .{};
    var t: Token = begin();
    t.serial = s.serial;
    t.children = s.children;
    return t;
}
pub fn stageEnd(stage: Stage, t: OptionalToken) void {
    if (comptime enabled) {
        if (!t.active) return;
        const s = &state[t.cpu];
        if (!s.active or s.serial != t.serial or t.cpu != cpuSlot()) return;
        const elapsed = now() -% t.start;
        const exclusive = elapsed -| (s.children -% t.children);
        s.children +%= exclusive;
        record(t, kinds * 2 + @intFromEnum(stage), s.kind, elapsed, exclusive);
        // A hold crossing the epoch is excluded as a whole. Exclude its stage
        // totals too, so the numerator and hold denominator describe one set.
        if (t.epoch != epoch() or s.hold.epoch != epoch()) return;
        const row = &s.pending[@intFromEnum(stage)];
        _ = @atomicRmw(u64, &row[0], .Add, 1, .monotonic);
        _ = @atomicRmw(u64, &row[1], .Add, elapsed, .monotonic);
        _ = @atomicRmw(u64, &row[2], .Add, exclusive, .monotonic);
    }
}

test "profile epoch IRQ handoff and exclusive attribution" {
    if (comptime !enabled) return;
    const std = @import("std");
    // Boot can already have observed PMM allocation before CPU enumeration.
    free_floor[0] = 42;
    try initCpuStorage(72);
    try std.testing.expectEqual(@as(u64, 42), free_floor[0]);
    try std.testing.expectEqual(@as(u64, 0), free_floor[71]);
    defer {
        std.testing.allocator.free(distributions);
        std.testing.allocator.free(stage_totals);
        std.testing.allocator.free(events);
        std.testing.allocator.free(free_floor);
        std.testing.allocator.free(writers);
        std.testing.allocator.free(state);
        std.testing.allocator.free(detail);
        detail = &detail_boot;
        detail_view = @TypeOf(detail_view).init(detail);
        std.testing.allocator.free(storage_detail);
        storage_detail = &storage_boot;
        storage_view = @TypeOf(storage_view).init(storage_detail);
        distributions = &distributions_boot;
        stage_totals = &stage_totals_boot;
        events = &events_boot;
        free_floor = &free_floor_boot;
        writers = &writers_boot;
        state = &state_boot;
    }
    root.profile_test_cpu = 0;
    root.profile_test_time = 100;
    epoch_word = 1;
    const waiting = begin();
    // An IRQ can acquire K while this CPU is still a syscall contender.
    irqEnter(false);
    acquired(.irq, begin(), true);
    root.profile_test_time += 10;
    release();
    irqLeave(false);
    acquired(.map, waiting, false);
    const parent = stageBegin();
    root.profile_test_time += 10;
    const nested = stageBegin();
    root.profile_test_time += 20;
    stageEnd(.zero, nested);
    root.profile_test_time += 10;
    stageEnd(.prepare, parent);
    irqEnter(false);
    try std.testing.expect(!stageBegin().active);
    irqLeave(false);
    root.profile_test_time += 10;
    release();
    try std.testing.expectEqual(@as(u64, 1), distributions[0][@intFromEnum(Kind.map) * 2].count);
    try std.testing.expectEqual(@as(u64, 10), distributions[0][@intFromEnum(Kind.map) * 2].cycles);
    try std.testing.expectEqual(@as(u64, 50), distributions[0][@intFromEnum(Kind.map) * 2 + 1].cycles);
    try std.testing.expectEqual(@as(u64, 20), stage_totals[0][@intFromEnum(Kind.map)][@intFromEnum(Stage.prepare)][2]);
    try std.testing.expectEqual(@as(u64, 20), stage_totals[0][@intFromEnum(Kind.map)][@intFromEnum(Stage.zero)][2]);
    // Reacquire after a block is a new segment; epoch reset never touches tokens.
    acquired(.map, begin(), false);
    const crossing = stageBegin();
    root.profile_test_time += 100;
    epoch_word = 2;
    stageEnd(.copy, crossing);
    release();
    try std.testing.expectEqual(@as(u64, 1), distributions[0][@intFromEnum(Kind.map) * 2 + 1].excluded);
    try std.testing.expectEqual(@as(u64, 0), stage_totals[0][@intFromEnum(Kind.map)][@intFromEnum(Stage.copy)][0]);
    try std.testing.expect(!stageBegin().active);
    // New CPU after block gets its own row and the original kind.
    root.profile_test_cpu = 71;
    acquired(.map, begin(), false);
    root.profile_test_time += 30;
    release();
    try std.testing.expectEqual(@as(u64, 30), distributions[71][@intFromEnum(Kind.map) * 2 + 1].cycles);
    // A stage completed before a later stop is not counted in the ratio if
    // the containing hold crosses that stop.
    acquired(.fd, begin(), false);
    const partial = stageBegin();
    root.profile_test_time += 15;
    stageEnd(.pmm, partial);
    epoch_word = 3;
    release();
    try std.testing.expectEqual(@as(u64, 0), stage_totals[71][@intFromEnum(Kind.fd)][@intFromEnum(Stage.pmm)][0]);
}

pub fn staticEnd() usize {
    if (comptime !enabled) return 0;
    var end: usize = 0;
    inline for (.{ &distributions, &stage_totals, &events, &writers, &epoch_word, &state, &free_floor, &layout, &distributions_view, &stage_totals_view, &events_view, &free_floor_view, &writers_view, &distributions_boot, &stage_totals_boot, &events_boot, &writers_boot, &state_boot, &free_floor_boot, &pmm_counts, &pmm_aux }) |ptr| {
        end = @max(end, @intFromPtr(ptr) + @sizeOf(@TypeOf(ptr.*)));
    }
    return end;
}

// Called under the existing PMM exclusion after each successful removal.
// Inverted free count permits an atomic maximum with zero as "unobserved".
pub fn freePages(pages: usize) void {
    if (comptime enabled) {
        _ = @atomicRmw(u64, &free_floor[cpuSlot()], .Max, ~@as(u64, pages), .monotonic);
    }
}

// PMM serializes tree observations with P and cached observations with cache
// locks. Atomic updates and the existing
// writer/epoch handshake let stopped host snapshots reject partial records.
// No allocator decision depends on this profile-only storage.
var pmm_counts: if (enabled) [20]u64 else void = if (enabled) @splat(0) else {};
comptime {
    if (enabled) @export(&pmm_counts, .{ .name = "pmm_profile_counts" });
}
pub fn pmmOperation(kind: usize, visited: usize, ranges: usize) void {
    if (comptime !enabled) return;
    const cpu = cpuSlot();
    _ = @atomicRmw(u64, &writers[cpu], .Add, 1, .monotonic);
    defer _ = @atomicRmw(u64, &writers[cpu], .Sub, 1, .monotonic);
    _ = @atomicRmw(u64, &pmm_counts[kind * 3], .Add, 1, .monotonic);
    _ = @atomicRmw(u64, &pmm_counts[kind * 3 + 1], .Add, visited, .monotonic);
    _ = @atomicRmw(u64, &pmm_counts[kind * 3 + 2], .Max, visited, .monotonic);
    _ = @atomicRmw(u64, &pmm_counts[12], .Add, ranges, .monotonic);
    _ = @atomicRmw(u64, &pmm_counts[13], .Max, ranges, .monotonic);
    @atomicStore(u64, &pmm_counts[14], ranges, .monotonic);
    _ = @atomicRmw(u64, &pmm_counts[19], .Add, 1, .monotonic);
}
pub fn pmmRelease(t: OptionalToken) void {
    if (comptime !enabled) return;
    const elapsed = now() -% t.start;
    _ = @atomicRmw(u64, &writers[t.cpu], .Add, 1, .monotonic);
    defer _ = @atomicRmw(u64, &writers[t.cpu], .Sub, 1, .monotonic);
    if (t.epoch != epoch()) {
        _ = @atomicRmw(u64, &pmm_counts[18], .Add, 1, .monotonic);
        return;
    }
    _ = @atomicRmw(u64, &pmm_counts[15], .Add, 1, .monotonic);
    _ = @atomicRmw(u64, &pmm_counts[16], .Add, elapsed, .monotonic);
    _ = @atomicRmw(u64, &pmm_counts[17], .Max, elapsed, .monotonic);
}

// Maintenance traversals and cache contention are separate from the original
// first-fit/predecessor search counters, so moving work cannot hide its cost.
var pmm_aux: if (enabled) [8]u64 else void = if (enabled) @splat(0) else {};
comptime {
    if (enabled) @export(&pmm_aux, .{ .name = "pmm_profile_aux" });
}
pub fn pmmMaintenance(kind: usize, visits: u64) void {
    if (comptime !enabled) return;
    _ = @atomicRmw(u64, &pmm_aux[kind], .Add, visits, .monotonic);
}
pub fn pmmCacheHit(kind: usize) void {
    if (comptime !enabled) return;
    const cpu = cpuSlot();
    _ = @atomicRmw(u64, &writers[cpu], .Add, 1, .monotonic);
    defer _ = @atomicRmw(u64, &writers[cpu], .Sub, 1, .monotonic);
    _ = @atomicRmw(u64, &pmm_counts[kind * 3], .Add, 1, .monotonic);
    _ = @atomicRmw(u64, &pmm_aux[4], .Add, 1, .monotonic);
}
pub fn pmmCachedSearch(kind: usize, visits: usize, ranges: usize) void {
    if (comptime !enabled) return;
    pmmOperation(kind, visits, ranges);
    _ = @atomicRmw(u64, &pmm_aux[4], .Add, 1, .monotonic);
}
pub fn pmmCacheWait(t: OptionalToken) void {
    if (comptime !enabled) return;
    const elapsed = now() -% t.start;
    const cpu = cpuSlot();
    _ = @atomicRmw(u64, &writers[cpu], .Add, 1, .monotonic);
    defer _ = @atomicRmw(u64, &writers[cpu], .Sub, 1, .monotonic);
    if (t.epoch != epoch()) return;
    _ = @atomicRmw(u64, &pmm_aux[5], .Add, 1, .monotonic);
    _ = @atomicRmw(u64, &pmm_aux[6], .Add, elapsed, .monotonic);
    _ = @atomicRmw(u64, &pmm_aux[7], .Max, elapsed, .monotonic);
}

test "PMM counters distinguish searches cache hits holds and crossed epochs" {
    if (comptime !enabled) return;
    const std = @import("std");
    root.profile_test_cpu = 0;
    root.profile_test_time = 10;
    epoch_word = 100;
    @memset(&pmm_counts, 0);
    @memset(&pmm_aux, 0);
    pmmOperation(0, 7, 3);
    pmmCacheHit(0);
    pmmCachedSearch(3, 2, 4);
    const held = begin();
    root.profile_test_time = 30;
    pmmRelease(held);
    pmmCacheWait(held);
    epoch_word += 1;
    pmmRelease(held);
    pmmCacheWait(held);
    pmmMaintenance(1, 9);
    try std.testing.expectEqual(@as(u64, 2), pmm_counts[0]);
    try std.testing.expectEqual(@as(u64, 7), pmm_counts[1]);
    try std.testing.expectEqual(@as(u64, 1), pmm_counts[9]);
    try std.testing.expectEqual(@as(u64, 1), pmm_counts[15]);
    try std.testing.expectEqual(@as(u64, 20), pmm_counts[16]);
    try std.testing.expectEqual(@as(u64, 1), pmm_counts[18]);
    try std.testing.expectEqual(@as(u64, 2), pmm_counts[19]);
    try std.testing.expectEqual(@as(u64, 9), pmm_aux[1]);
    try std.testing.expectEqual(@as(u64, 2), pmm_aux[4]);
    try std.testing.expectEqual(@as(u64, 1), pmm_aux[5]);
    try std.testing.expectEqual(@as(u64, 20), pmm_aux[6]);
    try std.testing.expectEqual(@as(u64, 0), writers[0]);
}

test "PMM detail attributes explicit callers and CAS retries and excludes epoch crossing" {
    if (comptime !enabled) return;
    const std = @import("std");
    @memset(&detail[0], 0);
    root.profile_test_cpu = 0;
    root.profile_test_time = 100;
    const t = begin();
    root.profile_test_time = 130;
    pmmDetail(t, .vmo_backing, .refill, 1, false);
    pmmCas(t, 3, 2, false);
    try std.testing.expectEqual(@as(u64, 1), detail[0][(5 * 8 + 1) * 5]);
    try std.testing.expectEqual(@as(u64, 30), detail[0][(5 * 8 + 1) * 5 + 1]);
    try std.testing.expectEqual(@as(u64, 3), detail[0][520]);
    try std.testing.expectEqual(@as(u64, 2), detail[0][521]);
    epoch_word += 1;
    pmmDetail(t, .identity, .tree, 1, true);
    try std.testing.expectEqual(@as(u64, 0), detail[0][(4 * 8 + 2) * 5]);
    try std.testing.expectEqual(@as(u64, 1), detail[0][(4 * 8 + 2) * 5 + 3]);
}

// Callback owner is carried by the allocator stored in each grant. K kind is
// sampled only while its existing active hold is valid; no sticky syscall tag.
const storage_owners = @typeInfo(PmmCaller).@"enum".fields.len;
const storage_words = storage_owners * kinds * 2 * 5;
var storage_boot: [rows][storage_words]u64 align(64) = @splat(@splat(0));
var storage_detail: [][storage_words]u64 = &storage_boot;
var storage_view = @import("diagnostic_cpu_storage.zig").Descriptor([storage_words]u64).init(&storage_boot);
var storage_layout = [_]u64{ storage_owners, kinds, 2, 5 };
const storage_labels = blk: {
    var text: []const u8 = "";
    for (@typeInfo(PmmCaller).@"enum".fields) |f| text = text ++ f.name ++ "\x00";
    break :blk text;
};
comptime {
    if (enabled) {
        @export(&storage_view, .{ .name = "storage_profile_detail" });
        @export(&storage_layout, .{ .name = "storage_profile_layout" });
        @export(storage_labels.ptr, .{ .name = "storage_profile_labels" });
    }
}
pub const StorageToken = if (enabled) struct { time: Token, kind: Kind, serial: u64, depth: usize, hold_epoch: u64, attributed: bool } else void;
pub fn storageBegin() StorageToken {
    if (comptime !enabled) return {};
    const t = begin();
    const current = &state[t.cpu];
    const attributed = current.active and current.depth == current.owner_depth;
    return .{ .time = t, .kind = if (attributed) current.kind else .other, .serial = current.serial, .depth = current.depth, .hold_epoch = current.hold.epoch, .attributed = attributed };
}
pub fn storageDetail(sample: StorageToken, caller: PmmCaller, returned: bool, pages: usize, failed: bool) void {
    if (comptime !enabled) return;
    const t = sample.time;
    _ = @atomicRmw(u64, &writers[t.cpu], .Add, 1, .monotonic);
    defer _ = @atomicRmw(u64, &writers[t.cpu], .Sub, 1, .monotonic);
    const current = &state[t.cpu];
    const offset = ((@as(usize, @intFromEnum(caller)) * kinds + @intFromEnum(sample.kind)) * 2 + @intFromBool(returned)) * 5;
    const row = &storage_detail[t.cpu];
    // IRQ nesting and a replaced/released hold cannot inherit a syscall tag.
    // Align accepted K samples with the epoch of the denominator's hold.
    if (t.epoch != epoch() or t.cpu != cpuSlot() or current.depth != sample.depth or
        (sample.attributed and (!current.active or current.serial != sample.serial or sample.hold_epoch != t.epoch)))
    {
        _ = @atomicRmw(u64, &row[offset + 4], .Add, 1, .monotonic);
        return;
    }
    const values = [_]u64{ 1, now() -% t.start, pages, @intFromBool(failed) };
    for (values, 0..) |v, i| _ = @atomicRmw(u64, &row[offset + i], .Add, v, .monotonic);
}

test "storage callback owner and return attribution survives epoch rejection" {
    if (comptime !enabled) return;
    const std = @import("std");
    root.profile_test_cpu = 0;
    root.profile_test_time = 100;
    epoch_word = 31;
    storage_boot[0] = @splat(0);
    state[0] = .{ .active = true, .kind = .fd_close, .hold = .{ .epoch = 31 } };
    const token = storageBegin();
    root.profile_test_time = 130;
    storageDetail(token, .invocation, true, 4, true);
    const offset = ((@as(usize, @intFromEnum(PmmCaller.invocation)) * kinds + @intFromEnum(Kind.fd_close)) * 2 + 1) * 5;
    try std.testing.expectEqualSlices(u64, &.{ 1, 30, 4, 1, 0 }, storage_detail[0][offset..][0..5]);
    epoch_word += 1;
    storageDetail(token, .invocation, true, 4, false);
    try std.testing.expectEqualSlices(u64, &.{ 1, 30, 4, 1, 1 }, storage_detail[0][offset..][0..5]);
    state[0].depth = 1;
    const irq_sample = storageBegin();
    try std.testing.expectEqual(Kind.other, irq_sample.kind);
    state[0].depth = 0;
    storageDetail(irq_sample, .invocation, true, 4, false);
    const other_offset = (@as(usize, @intFromEnum(PmmCaller.invocation)) * kinds * 2 + 1) * 5;
    try std.testing.expectEqual(@as(u64, 1), storage_detail[0][other_offset + 4]);
    state[0] = .{};
}

test "profile epoch independent PMM scope counts only its borrowed sink" {
    if (comptime !enabled) return;
    const std = @import("std");
    root.profile_test_cpu = 0;
    root.profile_test_time = 100;
    var allocated: [2]u64 = .{ 0, 0 };
    var returned: [2]u64 = .{ 0, 0 };
    setPmmScopeSink(.{ .allocated = &allocated, .returned = &returned });
    defer setPmmScopeSink(null);
    const start = pmmScopeTimestamp();
    root.profile_test_time = 130;
    pmmScopeEnd(false, start);
    pmmScopeEnd(true, start);
    setPmmScopeSink(null);
    root.profile_test_time = 160;
    pmmScopeEnd(false, start);
    try std.testing.expectEqualSlices(u64, &.{ 1, 30 }, &allocated);
    try std.testing.expectEqualSlices(u64, &.{ 1, 30 }, &returned);
    try std.testing.expectEqual(@as(u64, 0), pmmScopeTimestamp());
}

// OOM-only opt-in event: the failed allocation has dropped all PMM/cache locks.
// Raw serial avoids allocating or entering a resource owner during recovery.
var reclaim_sequence: if (enabled) usize else void = if (enabled) 0 else {};
pub fn reclaimRecovery(caller: PmmCaller, requested: usize, free: usize, retained: usize, returned: usize) void {
    if (comptime !enabled or @import("builtin").is_test) return;
    const sequence = @atomicRmw(usize, &reclaim_sequence, .Add, 1, .monotonic);
    var buffer: [224]u8 = undefined;
    const message = @import("std").fmt.bufPrint(&buffer, "PMM_OOM seq={d} caller={s} requested={d} free={d} wait_cache={d} reclaimed={d}\n", .{ sequence, @tagName(caller), requested, free, retained, returned }) catch return;
    @import("serial.zig").writeRaw(message);
}

// Copy observations retain the K owner/IRQ attribution, but do not enter the
// nested stage accounting: these overlapping intervals must not be summed.
pub const CopyPart = enum { call, cr3_read, cr3_write, as_lock, as_hold, resolve, cache, guard, window, invlpg, memcpy, invocation, vma_lookup, page_fault, page_walk, device_lock };
const copy_parts = @typeInfo(CopyPart).@"enum".fields.len;
const copy_words = kinds * copy_parts * 2;
var copy_boot: [rows][copy_words]u64 align(128) = @splat(@splat(0));
var copy_detail: [][copy_words]u64 = &copy_boot;
var copy_view = @import("diagnostic_cpu_storage.zig").Descriptor([copy_words]u64).init(&copy_boot);
comptime {
    if (enabled) @export(&copy_view, .{ .name = "user_copy_detail" });
}
pub fn copyTimestamp() u64 {
    if (comptime !enabled) return 0;
    return now();
}
pub fn copyEnd(part: CopyPart, start: u64) void {
    if (comptime !enabled) return;
    const elapsed = now() -% start;
    const cpu = cpuSlot();
    const s = &state[cpu];
    const kind: Kind = if (s.active and s.depth == s.owner_depth) s.kind else .other;
    const index = (@intFromEnum(kind) * copy_parts + @intFromEnum(part)) * 2;
    _ = @atomicRmw(u64, &copy_detail[cpu][index], .Add, 1, .monotonic);
    _ = @atomicRmw(u64, &copy_detail[cpu][index + 1], .Add, elapsed, .monotonic);
}

// Emitted only after the failed contiguous search releases all PMM/cache locks.
// Each complete line has its own ID; concurrent recovery may change free RAM
// after this snapshot, but cannot change the captured failed-search evidence.
var fragmentation_sequence: if (enabled) usize else void = if (enabled) 0 else {};
pub fn fragmentationFailure(caller: PmmCaller, requested: usize, minimum: u64, limit: u64, snapshot: anytype) void {
    if (comptime !enabled or @import("builtin").is_test) return;
    const sequence = @atomicRmw(usize, &fragmentation_sequence, .Add, 1, .monotonic);
    var buffer: [2048]u8 = undefined;
    const prefix = @import("std").fmt.bufPrint(&buffer, "PMM_FRAGMENT seq={d} phase=after_cache_drain_failed_search caller={s} requested={d} minimum={d} limit={d} free={d} largest={d} orders=", .{ sequence, @tagName(caller), requested, minimum, limit, snapshot.total, snapshot.largest }) catch return;
    var len = prefix.len;
    for (snapshot.orders, 0..) |blocks, order| {
        const part = @import("std").fmt.bufPrint(buffer[len..], "{d}:{d},", .{ order, blocks }) catch return;
        len += part.len;
    }
    buffer[len] = '\n';
    @import("serial.zig").writeRaw(buffer[0 .. len + 1]);
}
