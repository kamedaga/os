const std = @import("std");
const kernel = @import("../kernel.zig");

pub const State = struct {
    user_enter_tick: u64 = 0,

    bootlog_virtgpu_prewarm_start: bool = false,
    bootlog_virtgpu_prewarm_start_tick: u64 = 0,
    bootlog_virtgpu_prewarm_ready: bool = false,
    bootlog_virtgpu_prewarm_ready_tick: u64 = 0,
    bootlog_virtgpu_scanout_prewarm_start: bool = false,
    bootlog_virtgpu_scanout_prewarm_start_tick: u64 = 0,
    bootlog_virtgpu_scanout_prewarm_end: bool = false,
    bootlog_virtgpu_scanout_prewarm_end_tick: u64 = 0,
    bootlog_virtgpu_scanout_prewarm_failed: bool = false,

    init_started: bool = false,
    init_started_tick: u64 = 0,
    init_boot_manifest_ok: bool = false,
    init_boot_manifest_ok_tick: u64 = 0,
    init_startup_manifest_ok: bool = false,
    init_startup_manifest_ok_tick: u64 = 0,
    init_manifest_begin: bool = false,
    init_manifest_begin_tick: u64 = 0,
    init_vfs_spawn_done: bool = false,
    init_vfs_spawn_done_tick: u64 = 0,
    init_vfs_connect_done: bool = false,
    init_vfs_connect_done_tick: u64 = 0,
    init_keyboard_spawn_done: bool = false,
    init_keyboard_spawn_done_tick: u64 = 0,
    init_keyboard_shared_ready: bool = false,
    init_keyboard_shared_ready_tick: u64 = 0,
    init_mouse_spawn_done: bool = false,
    init_mouse_spawn_done_tick: u64 = 0,
    init_mouse_shared_ready: bool = false,
    init_mouse_shared_ready_tick: u64 = 0,
    init_compositor_arm_done: bool = false,
    init_compositor_arm_done_tick: u64 = 0,
    init_first_window_spawn_done: bool = false,
    init_first_window_spawn_done_tick: u64 = 0,
    init_startup_manifest_done: bool = false,
    init_startup_manifest_done_tick: u64 = 0,

    vfs_started: bool = false,
    vfs_started_tick: u64 = 0,
    vfs_config_ok: bool = false,
    vfs_config_ok_tick: u64 = 0,
    vfs_bootfs_image_ready: bool = false,
    vfs_bootfs_image_ready_tick: u64 = 0,
    vfs_boot_ready: bool = false,
    vfs_boot_ready_tick: u64 = 0,

    mouse_started: bool = false,
    mouse_start_tick: u64 = 0,
    mouse_queue_ready: bool = false,
    mouse_queue_ready_tick: u64 = 0,

    keyboard_started: bool = false,
    keyboard_start_tick: u64 = 0,
    keyboard_queue_ready: bool = false,
    keyboard_queue_ready_tick: u64 = 0,

    compositor_launch: bool = false,
    compositor_launch_tick: u64 = 0,
    compositor_start_entered: bool = false,
    compositor_start_entered_tick: u64 = 0,
    virtgpu_init_started: bool = false,
    virtgpu_init_start_tick: u64 = 0,
    virtgpu_queue_ready: bool = false,
    virtgpu_queue_ready_tick: u64 = 0,
    compositor_create_fb_start: bool = false,
    compositor_create_fb_start_tick: u64 = 0,
    compositor_create_fb_returned: bool = false,
    compositor_create_fb_returned_tick: u64 = 0,
    compositor_set_scanout_done: bool = false,
    compositor_set_scanout_done_tick: u64 = 0,
    compositor_first_compose_done: bool = false,
    compositor_first_compose_done_tick: u64 = 0,
    compositor_present_transfer_done: bool = false,
    compositor_present_transfer_done_tick: u64 = 0,
    compositor_present_flush_done: bool = false,
    compositor_present_flush_done_tick: u64 = 0,

    terminal_entry: bool = false,
    terminal_entry_tick: u64 = 0,
    terminal_started: bool = false,
    terminal_started_tick: u64 = 0,
    first_window_client_create_begin: bool = false,
    first_window_client_create_begin_tick: u64 = 0,
    first_window_client_create_window_ok: bool = false,
    first_window_client_create_window_ok_tick: u64 = 0,
    first_window_client_publish_ok: bool = false,
    first_window_client_publish_ok_tick: u64 = 0,

    bootlog_summary_emitted: bool = false,
    init_summary_emitted: bool = false,
    vfs_summary_emitted: bool = false,
    mouse_summary_emitted: bool = false,
    keyboard_summary_emitted: bool = false,
    compositor_summary_emitted: bool = false,
    terminal_summary_emitted: bool = false,
    global_summary_emitted: bool = false,
};

pub const TraceEvent = struct {
    label: []const u8,
    tick: u64,
    has_delta: bool,
    delta: u64,
};

const SummarySegment = struct {
    label: []const u8,
    value: ?u64,
};

pub var state: State = .{};
var trace: [96]TraceEvent = undefined;
var trace_len: usize = 0;
var trace_flushed = false;

fn containsBytes(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

fn appendFmt(buf: []u8, idx: *usize, comptime fmt: []const u8, args: anytype) void {
    if (idx.* >= buf.len) return;
    const text = std.fmt.bufPrint(buf[idx.*..], fmt, args) catch return;
    idx.* += text.len;
}

fn elapsed(end_seen: bool, end_tick: u64, start_seen: bool, start_tick: u64) ?u64 {
    if (!end_seen or !start_seen) return null;
    if (end_tick < start_tick) return null;
    return end_tick - start_tick;
}

fn noteEvent(flag: *bool, tick_ptr: *u64, now_tick: u64, label: []const u8, delta: ?u64) void {
    if (flag.*) return;
    flag.* = true;
    tick_ptr.* = now_tick;
    recordEvent(label, now_tick, delta);
}

fn writeSummary(
    writeFn: fn ([]const u8) void,
    process_name: []const u8,
    finish_tick: u64,
    start_tick: u64,
    segments: []const SummarySegment,
    result_note: ?[]const u8,
) void {
    var line_buf: [512]u8 = undefined;
    var idx: usize = 0;
    appendFmt(
        line_buf[0..],
        &idx,
        "boot_time {s} total_ticks={} since_user={}",
        .{ process_name, finish_tick - start_tick, finish_tick - state.user_enter_tick },
    );
    for (segments) |segment| {
        if (segment.value) |value| {
            appendFmt(line_buf[0..], &idx, " {s}={}", .{ segment.label, value });
        }
    }
    if (result_note) |note| {
        appendFmt(line_buf[0..], &idx, " result={s}", .{note});
    }
    appendFmt(line_buf[0..], &idx, "\n", .{});
    writeFn(line_buf[0..idx]);
}

fn maybeEmitBootLogSummary(writeFn: fn ([]const u8) void) void {
    if (state.bootlog_summary_emitted) return;
    if (!state.bootlog_virtgpu_prewarm_ready or !state.bootlog_virtgpu_scanout_prewarm_end) return;
    state.bootlog_summary_emitted = true;
    const segments = [_]SummarySegment{
        .{ .label = "prewarm", .value = elapsed(state.bootlog_virtgpu_prewarm_ready, state.bootlog_virtgpu_prewarm_ready_tick, state.bootlog_virtgpu_prewarm_start, state.bootlog_virtgpu_prewarm_start_tick) },
        .{ .label = "scanout", .value = elapsed(state.bootlog_virtgpu_scanout_prewarm_end, state.bootlog_virtgpu_scanout_prewarm_end_tick, state.bootlog_virtgpu_scanout_prewarm_start, state.bootlog_virtgpu_scanout_prewarm_start_tick) },
    };
    writeSummary(
        writeFn,
        "BootLogConsole",
        state.bootlog_virtgpu_scanout_prewarm_end_tick,
        state.bootlog_virtgpu_prewarm_start_tick,
        segments[0..],
        if (state.bootlog_virtgpu_scanout_prewarm_failed) "failed" else "ok",
    );
}

fn maybeEmitInitSummary(writeFn: fn ([]const u8) void) void {
    if (state.init_summary_emitted) return;
    if (!state.init_started or !state.init_startup_manifest_done) return;
    state.init_summary_emitted = true;
    const segments = [_]SummarySegment{
        .{ .label = "boot_manifest", .value = elapsed(state.init_boot_manifest_ok, state.init_boot_manifest_ok_tick, state.init_started, state.init_started_tick) },
        .{ .label = "manifest_load", .value = elapsed(state.init_startup_manifest_ok, state.init_startup_manifest_ok_tick, state.init_boot_manifest_ok, state.init_boot_manifest_ok_tick) },
        .{ .label = "manifest_run", .value = elapsed(state.init_startup_manifest_done, state.init_startup_manifest_done_tick, state.init_manifest_begin, state.init_manifest_begin_tick) },
        .{ .label = "vfs_spawn", .value = elapsed(state.init_vfs_spawn_done, state.init_vfs_spawn_done_tick, state.init_manifest_begin, state.init_manifest_begin_tick) },
        .{ .label = "vfs_connect", .value = elapsed(state.init_vfs_connect_done, state.init_vfs_connect_done_tick, state.init_vfs_spawn_done, state.init_vfs_spawn_done_tick) },
        .{ .label = "kbd_spawn", .value = elapsed(state.init_keyboard_spawn_done, state.init_keyboard_spawn_done_tick, state.init_vfs_spawn_done, state.init_vfs_spawn_done_tick) },
        .{ .label = "kbd_shared", .value = elapsed(state.init_keyboard_shared_ready, state.init_keyboard_shared_ready_tick, state.init_keyboard_spawn_done, state.init_keyboard_spawn_done_tick) },
        .{ .label = "mouse_spawn", .value = elapsed(state.init_mouse_spawn_done, state.init_mouse_spawn_done_tick, state.init_keyboard_spawn_done, state.init_keyboard_spawn_done_tick) },
        .{ .label = "mouse_shared", .value = elapsed(state.init_mouse_shared_ready, state.init_mouse_shared_ready_tick, state.init_mouse_spawn_done, state.init_mouse_spawn_done_tick) },
        .{ .label = "comp_arm", .value = elapsed(state.init_compositor_arm_done, state.init_compositor_arm_done_tick, state.init_mouse_shared_ready, state.init_mouse_shared_ready_tick) },
        .{ .label = "first_window", .value = elapsed(state.init_first_window_spawn_done, state.init_first_window_spawn_done_tick, state.init_compositor_arm_done, state.init_compositor_arm_done_tick) },
    };
    writeSummary(writeFn, "Init", state.init_startup_manifest_done_tick, state.init_started_tick, segments[0..], null);
}

fn maybeEmitVfsSummary(writeFn: fn ([]const u8) void) void {
    if (state.vfs_summary_emitted) return;
    if (!state.vfs_started or !state.vfs_boot_ready) return;
    state.vfs_summary_emitted = true;
    const segments = [_]SummarySegment{
        .{ .label = "config", .value = elapsed(state.vfs_config_ok, state.vfs_config_ok_tick, state.vfs_started, state.vfs_started_tick) },
        .{ .label = "bootfs_image", .value = elapsed(state.vfs_bootfs_image_ready, state.vfs_bootfs_image_ready_tick, state.vfs_config_ok, state.vfs_config_ok_tick) },
        .{ .label = "boot_ready", .value = elapsed(state.vfs_boot_ready, state.vfs_boot_ready_tick, state.vfs_bootfs_image_ready, state.vfs_bootfs_image_ready_tick) },
    };
    writeSummary(writeFn, "VFS", state.vfs_boot_ready_tick, state.vfs_started_tick, segments[0..], null);
}

fn maybeEmitMouseSummary(writeFn: fn ([]const u8) void) void {
    if (state.mouse_summary_emitted) return;
    if (!state.mouse_started or !state.mouse_queue_ready) return;
    state.mouse_summary_emitted = true;
    const segments = [_]SummarySegment{
        .{ .label = "queue", .value = elapsed(state.mouse_queue_ready, state.mouse_queue_ready_tick, state.mouse_started, state.mouse_start_tick) },
    };
    writeSummary(writeFn, "MouseDriver", state.mouse_queue_ready_tick, state.mouse_start_tick, segments[0..], null);
}

fn maybeEmitKeyboardSummary(writeFn: fn ([]const u8) void) void {
    if (state.keyboard_summary_emitted) return;
    if (!state.keyboard_started or !state.keyboard_queue_ready) return;
    state.keyboard_summary_emitted = true;
    const segments = [_]SummarySegment{
        .{ .label = "queue", .value = elapsed(state.keyboard_queue_ready, state.keyboard_queue_ready_tick, state.keyboard_started, state.keyboard_start_tick) },
    };
    writeSummary(writeFn, "KeyboardDriver", state.keyboard_queue_ready_tick, state.keyboard_start_tick, segments[0..], null);
}

fn maybeEmitTerminalSummary(writeFn: fn ([]const u8) void) void {
    if (state.terminal_summary_emitted) return;
    if (!state.terminal_entry or !state.first_window_client_publish_ok) return;
    state.terminal_summary_emitted = true;
    const segments = [_]SummarySegment{
        .{ .label = "spawn_gap", .value = elapsed(state.terminal_entry, state.terminal_entry_tick, state.init_first_window_spawn_done, state.init_first_window_spawn_done_tick) },
        .{ .label = "entry_start", .value = elapsed(state.terminal_started, state.terminal_started_tick, state.terminal_entry, state.terminal_entry_tick) },
        .{ .label = "bind", .value = elapsed(state.first_window_client_create_begin, state.first_window_client_create_begin_tick, state.terminal_started, state.terminal_started_tick) },
        .{ .label = "create", .value = elapsed(state.first_window_client_create_window_ok, state.first_window_client_create_window_ok_tick, state.first_window_client_create_begin, state.first_window_client_create_begin_tick) },
        .{ .label = "publish", .value = elapsed(state.first_window_client_publish_ok, state.first_window_client_publish_ok_tick, state.first_window_client_create_window_ok, state.first_window_client_create_window_ok_tick) },
    };
    writeSummary(writeFn, "TerminalWindow", state.first_window_client_publish_ok_tick, state.terminal_entry_tick, segments[0..], null);
}

fn compositorStartTick() ?u64 {
    if (state.compositor_start_entered) return state.compositor_start_entered_tick;
    if (state.virtgpu_init_started) return state.virtgpu_init_start_tick;
    if (state.compositor_launch) return state.compositor_launch_tick;
    return null;
}

fn maybeEmitCompositorSummary(writeFn: fn ([]const u8) void) void {
    if (state.compositor_summary_emitted) return;
    if (!state.compositor_first_compose_done) return;
    const start_tick = compositorStartTick() orelse return;
    state.compositor_summary_emitted = true;
    const segments = [_]SummarySegment{
        .{ .label = "launch", .value = elapsed(state.compositor_start_entered, state.compositor_start_entered_tick, state.compositor_launch, state.compositor_launch_tick) },
        .{ .label = "virtgpu", .value = elapsed(state.virtgpu_queue_ready, state.virtgpu_queue_ready_tick, state.virtgpu_init_started, state.virtgpu_init_start_tick) },
        .{ .label = "create_fb", .value = elapsed(state.compositor_create_fb_returned, state.compositor_create_fb_returned_tick, state.compositor_create_fb_start, state.compositor_create_fb_start_tick) },
        .{ .label = "scanout", .value = elapsed(state.compositor_set_scanout_done, state.compositor_set_scanout_done_tick, state.compositor_create_fb_returned, state.compositor_create_fb_returned_tick) },
        .{ .label = "first_compose", .value = elapsed(state.compositor_first_compose_done, state.compositor_first_compose_done_tick, state.compositor_set_scanout_done, state.compositor_set_scanout_done_tick) },
    };
    writeSummary(writeFn, "Compositor", state.compositor_first_compose_done_tick, start_tick, segments[0..], null);
}

fn maybeEmitGlobalSummary(writeFn: fn ([]const u8) void) void {
    if (state.global_summary_emitted) return;
    if (!state.compositor_first_compose_done) return;
    state.global_summary_emitted = true;

    var line_buf: [320]u8 = undefined;
    var idx: usize = 0;
    appendFmt(line_buf[0..], &idx, "boot_time CriticalPath total_ticks={}", .{state.compositor_first_compose_done_tick - state.user_enter_tick});
    if (state.bootlog_virtgpu_scanout_prewarm_end) {
        appendFmt(line_buf[0..], &idx, " bootlog={}", .{state.bootlog_virtgpu_scanout_prewarm_end_tick - state.bootlog_virtgpu_prewarm_start_tick});
    }
    if (state.init_first_window_spawn_done) {
        appendFmt(line_buf[0..], &idx, " init_to_first_window={}", .{state.init_first_window_spawn_done_tick - state.init_started_tick});
    }
    if (state.first_window_client_publish_ok) {
        appendFmt(line_buf[0..], &idx, " terminal_publish={}", .{state.first_window_client_publish_ok_tick - state.terminal_entry_tick});
    }
    if (compositorStartTick()) |start_tick| {
        appendFmt(line_buf[0..], &idx, " compositor={}", .{state.compositor_first_compose_done_tick - start_tick});
    }
    appendFmt(line_buf[0..], &idx, "\n", .{});
    writeFn(line_buf[0..idx]);
}

fn maybeEmitSummaries(writeFn: fn ([]const u8) void) void {
    maybeEmitBootLogSummary(writeFn);
    maybeEmitVfsSummary(writeFn);
    maybeEmitKeyboardSummary(writeFn);
    maybeEmitMouseSummary(writeFn);
    maybeEmitInitSummary(writeFn);
    maybeEmitTerminalSummary(writeFn);
    maybeEmitCompositorSummary(writeFn);
    maybeEmitGlobalSummary(writeFn);
}

pub fn reset() void {
    state = .{};
    trace_len = 0;
    trace_flushed = false;
}

pub fn recordEvent(label: []const u8, tick: u64, delta: ?u64) void {
    if (trace_len >= trace.len) return;
    trace[trace_len] = .{
        .label = label,
        .tick = tick,
        .has_delta = delta != null,
        .delta = delta orelse 0,
    };
    trace_len += 1;
}

pub fn flushTrace(writeFn: fn ([]const u8) void) void {
    if (trace_flushed) return;
    if (trace_len == 0) return;
    var i: usize = 0;
    while (i < trace_len) : (i += 1) {
        const event = trace[i];
        var line_buf: [160]u8 = undefined;
        const line = if (event.has_delta)
            std.fmt.bufPrint(
                line_buf[0..],
                "boot_timing {s} tick={} since_user={} delta={}\n",
                .{ event.label, event.tick, event.tick - state.user_enter_tick, event.delta },
            ) catch break
        else
            std.fmt.bufPrint(
                line_buf[0..],
                "boot_timing {s} tick={} since_user={}\n",
                .{ event.label, event.tick, event.tick - state.user_enter_tick },
            ) catch break;
        writeFn(line);
    }
    trace_flushed = true;
}

pub fn updateFromUserLog(proc: kernel.PrincipalId, message: []const u8, now_tick: u64, writeFn: fn ([]const u8) void) void {
    _ = proc;

    if (containsBytes(message, "BootLogConsole: virtgpu prewarm start")) {
        noteEvent(&state.bootlog_virtgpu_prewarm_start, &state.bootlog_virtgpu_prewarm_start_tick, now_tick, "bootlog_virtgpu_prewarm_start", null);
    }
    if (containsBytes(message, "BootLogConsole: virtgpu prewarm ready")) {
        noteEvent(
            &state.bootlog_virtgpu_prewarm_ready,
            &state.bootlog_virtgpu_prewarm_ready_tick,
            now_tick,
            "bootlog_virtgpu_prewarm_ready",
            elapsed(true, now_tick, state.bootlog_virtgpu_prewarm_start, state.bootlog_virtgpu_prewarm_start_tick),
        );
    }
    if (containsBytes(message, "BootLogConsole: virtgpu scanout prewarm start")) {
        noteEvent(
            &state.bootlog_virtgpu_scanout_prewarm_start,
            &state.bootlog_virtgpu_scanout_prewarm_start_tick,
            now_tick,
            "bootlog_scanout_prewarm_start",
            elapsed(true, now_tick, state.bootlog_virtgpu_prewarm_ready, state.bootlog_virtgpu_prewarm_ready_tick),
        );
    }
    if (containsBytes(message, "BootLogConsole: virtgpu scanout prewarm ready")) {
        noteEvent(
            &state.bootlog_virtgpu_scanout_prewarm_end,
            &state.bootlog_virtgpu_scanout_prewarm_end_tick,
            now_tick,
            "bootlog_scanout_prewarm_ready",
            elapsed(true, now_tick, state.bootlog_virtgpu_scanout_prewarm_start, state.bootlog_virtgpu_scanout_prewarm_start_tick),
        );
        state.bootlog_virtgpu_scanout_prewarm_failed = false;
    }
    if (containsBytes(message, "BootLogConsole: virtgpu scanout prewarm failed")) {
        noteEvent(
            &state.bootlog_virtgpu_scanout_prewarm_end,
            &state.bootlog_virtgpu_scanout_prewarm_end_tick,
            now_tick,
            "bootlog_scanout_prewarm_failed",
            elapsed(true, now_tick, state.bootlog_virtgpu_scanout_prewarm_start, state.bootlog_virtgpu_scanout_prewarm_start_tick),
        );
        state.bootlog_virtgpu_scanout_prewarm_failed = true;
    }

    if (containsBytes(message, "Init: started")) {
        noteEvent(&state.init_started, &state.init_started_tick, now_tick, "init_started", null);
    }
    if (containsBytes(message, "Init: boot manifest ok")) {
        noteEvent(
            &state.init_boot_manifest_ok,
            &state.init_boot_manifest_ok_tick,
            now_tick,
            "init_boot_manifest_ok",
            elapsed(true, now_tick, state.init_started, state.init_started_tick),
        );
    }
    if (containsBytes(message, "Init: startup manifest ok")) {
        noteEvent(
            &state.init_startup_manifest_ok,
            &state.init_startup_manifest_ok_tick,
            now_tick,
            "init_startup_manifest_ok",
            elapsed(true, now_tick, state.init_boot_manifest_ok, state.init_boot_manifest_ok_tick),
        );
    }
    if (containsBytes(message, "Init: startup manifest begin")) {
        noteEvent(
            &state.init_manifest_begin,
            &state.init_manifest_begin_tick,
            now_tick,
            "init_manifest_begin",
            elapsed(true, now_tick, state.init_startup_manifest_ok, state.init_startup_manifest_ok_tick),
        );
    }
    if (containsBytes(message, "Init: VFS spawn done")) {
        noteEvent(
            &state.init_vfs_spawn_done,
            &state.init_vfs_spawn_done_tick,
            now_tick,
            "init_vfs_spawn_done",
            elapsed(true, now_tick, state.init_manifest_begin, state.init_manifest_begin_tick),
        );
    }
    if (containsBytes(message, "Init: VFS connect done")) {
        noteEvent(
            &state.init_vfs_connect_done,
            &state.init_vfs_connect_done_tick,
            now_tick,
            "init_vfs_connect_done",
            elapsed(true, now_tick, state.init_vfs_spawn_done, state.init_vfs_spawn_done_tick),
        );
    }
    if (containsBytes(message, "Init: keyboard spawn done")) {
        noteEvent(
            &state.init_keyboard_spawn_done,
            &state.init_keyboard_spawn_done_tick,
            now_tick,
            "init_keyboard_spawn_done",
            elapsed(true, now_tick, state.init_vfs_spawn_done, state.init_vfs_spawn_done_tick),
        );
    }
    if (containsBytes(message, "Init: keyboard shared ready")) {
        noteEvent(
            &state.init_keyboard_shared_ready,
            &state.init_keyboard_shared_ready_tick,
            now_tick,
            "init_keyboard_shared_ready",
            elapsed(true, now_tick, state.init_keyboard_spawn_done, state.init_keyboard_spawn_done_tick),
        );
    }
    if (containsBytes(message, "Init: mouse spawn done")) {
        noteEvent(
            &state.init_mouse_spawn_done,
            &state.init_mouse_spawn_done_tick,
            now_tick,
            "init_mouse_spawn_done",
            elapsed(true, now_tick, state.init_keyboard_spawn_done, state.init_keyboard_spawn_done_tick),
        );
    }
    if (containsBytes(message, "Init: mouse shared ready")) {
        noteEvent(
            &state.init_mouse_shared_ready,
            &state.init_mouse_shared_ready_tick,
            now_tick,
            "init_mouse_shared_ready",
            elapsed(true, now_tick, state.init_mouse_spawn_done, state.init_mouse_spawn_done_tick),
        );
    }
    if (containsBytes(message, "Init: arm deferred compositor ok")) {
        noteEvent(
            &state.init_compositor_arm_done,
            &state.init_compositor_arm_done_tick,
            now_tick,
            "init_compositor_arm_done",
            if (state.init_mouse_shared_ready)
                now_tick - state.init_mouse_shared_ready_tick
            else
                elapsed(true, now_tick, state.init_keyboard_shared_ready, state.init_keyboard_shared_ready_tick),
        );
    }
    if (containsBytes(message, "Init: first window spawn done")) {
        noteEvent(
            &state.init_first_window_spawn_done,
            &state.init_first_window_spawn_done_tick,
            now_tick,
            "init_first_window_spawn_done",
            elapsed(true, now_tick, state.init_compositor_arm_done, state.init_compositor_arm_done_tick),
        );
    }
    if (containsBytes(message, "Init: startup manifest done")) {
        noteEvent(
            &state.init_startup_manifest_done,
            &state.init_startup_manifest_done_tick,
            now_tick,
            "init_startup_manifest_done",
            elapsed(true, now_tick, state.init_manifest_begin, state.init_manifest_begin_tick),
        );
    }

    if (containsBytes(message, "VFS: started")) {
        noteEvent(&state.vfs_started, &state.vfs_started_tick, now_tick, "vfs_started", null);
    }
    if (containsBytes(message, "VFS: config ok")) {
        noteEvent(&state.vfs_config_ok, &state.vfs_config_ok_tick, now_tick, "vfs_config_ok", elapsed(true, now_tick, state.vfs_started, state.vfs_started_tick));
    }
    if (containsBytes(message, "VFS: bootfs image ready")) {
        noteEvent(
            &state.vfs_bootfs_image_ready,
            &state.vfs_bootfs_image_ready_tick,
            now_tick,
            "vfs_bootfs_image_ready",
            elapsed(true, now_tick, state.vfs_config_ok, state.vfs_config_ok_tick),
        );
    }
    if (containsBytes(message, "VFS: boot ready")) {
        noteEvent(
            &state.vfs_boot_ready,
            &state.vfs_boot_ready_tick,
            now_tick,
            "vfs_boot_ready",
            if (state.vfs_bootfs_image_ready)
                now_tick - state.vfs_bootfs_image_ready_tick
            else
                elapsed(true, now_tick, state.vfs_started, state.vfs_started_tick),
        );
    }

    if (containsBytes(message, "MouseDriver: started")) {
        noteEvent(&state.mouse_started, &state.mouse_start_tick, now_tick, "mouse_started", null);
    }
    if (containsBytes(message, "MouseDriver: queue ready")) {
        noteEvent(
            &state.mouse_queue_ready,
            &state.mouse_queue_ready_tick,
            now_tick,
            "mouse_queue_ready",
            elapsed(true, now_tick, state.mouse_started, state.mouse_start_tick),
        );
    }

    if (containsBytes(message, "KeyboardDriver: started")) {
        noteEvent(&state.keyboard_started, &state.keyboard_start_tick, now_tick, "keyboard_started", null);
    }
    if (containsBytes(message, "KeyboardDriver: queue ready")) {
        noteEvent(
            &state.keyboard_queue_ready,
            &state.keyboard_queue_ready_tick,
            now_tick,
            "keyboard_queue_ready",
            elapsed(true, now_tick, state.keyboard_started, state.keyboard_start_tick),
        );
    }

    if (containsBytes(message, "Compositor: _start")) {
        noteEvent(
            &state.compositor_start_entered,
            &state.compositor_start_entered_tick,
            now_tick,
            "compositor_start",
            elapsed(true, now_tick, state.compositor_launch, state.compositor_launch_tick),
        );
    }
    if (containsBytes(message, "Compositor: virtgpu init start")) {
        noteEvent(
            &state.virtgpu_init_started,
            &state.virtgpu_init_start_tick,
            now_tick,
            "virtgpu_init_start",
            if (state.compositor_start_entered)
                now_tick - state.compositor_start_entered_tick
            else
                elapsed(true, now_tick, state.compositor_launch, state.compositor_launch_tick),
        );
    }
    if (containsBytes(message, "Compositor: virtgpu queue ready")) {
        noteEvent(
            &state.virtgpu_queue_ready,
            &state.virtgpu_queue_ready_tick,
            now_tick,
            "virtgpu_queue_ready",
            elapsed(true, now_tick, state.virtgpu_init_started, state.virtgpu_init_start_tick),
        );
    }
    if (containsBytes(message, "Compositor: create_fb start")) {
        noteEvent(
            &state.compositor_create_fb_start,
            &state.compositor_create_fb_start_tick,
            now_tick,
            "compositor_create_fb_start",
            elapsed(true, now_tick, state.virtgpu_queue_ready, state.virtgpu_queue_ready_tick),
        );
    }
    if (containsBytes(message, "Compositor: create_fb returned")) {
        noteEvent(
            &state.compositor_create_fb_returned,
            &state.compositor_create_fb_returned_tick,
            now_tick,
            "compositor_create_fb_returned",
            elapsed(true, now_tick, state.compositor_create_fb_start, state.compositor_create_fb_start_tick),
        );
    }
    if (containsBytes(message, "Compositor: set_scanout done")) {
        noteEvent(
            &state.compositor_set_scanout_done,
            &state.compositor_set_scanout_done_tick,
            now_tick,
            "compositor_set_scanout_done",
            elapsed(true, now_tick, state.compositor_create_fb_returned, state.compositor_create_fb_returned_tick),
        );
    }
    if (containsBytes(message, "Compositor: first compose done")) {
        noteEvent(
            &state.compositor_first_compose_done,
            &state.compositor_first_compose_done_tick,
            now_tick,
            "compositor_first_compose_done",
            if (state.compositor_set_scanout_done)
                now_tick - state.compositor_set_scanout_done_tick
            else
                elapsed(true, now_tick, state.compositor_start_entered, state.compositor_start_entered_tick),
        );
    }
    if (containsBytes(message, "Compositor: present transfer done")) {
        noteEvent(
            &state.compositor_present_transfer_done,
            &state.compositor_present_transfer_done_tick,
            now_tick,
            "compositor_present_transfer_done",
            elapsed(true, now_tick, state.compositor_first_compose_done, state.compositor_first_compose_done_tick),
        );
    }
    if (containsBytes(message, "Compositor: present flush done")) {
        noteEvent(
            &state.compositor_present_flush_done,
            &state.compositor_present_flush_done_tick,
            now_tick,
            "compositor_present_flush_done",
            elapsed(true, now_tick, state.compositor_present_transfer_done, state.compositor_present_transfer_done_tick),
        );
    }

    if (containsBytes(message, "TerminalWindow: entry")) {
        noteEvent(&state.terminal_entry, &state.terminal_entry_tick, now_tick, "terminal_entry", null);
    }
    if (containsBytes(message, "TerminalWindow: started")) {
        noteEvent(
            &state.terminal_started,
            &state.terminal_started_tick,
            now_tick,
            "terminal_started",
            elapsed(true, now_tick, state.terminal_entry, state.terminal_entry_tick),
        );
    }
    if (containsBytes(message, "window_client: create begin")) {
        noteEvent(
            &state.first_window_client_create_begin,
            &state.first_window_client_create_begin_tick,
            now_tick,
            "first_window_client_create_begin",
            elapsed(true, now_tick, state.terminal_started, state.terminal_started_tick),
        );
    }
    if (containsBytes(message, "window_client: create_window ok")) {
        noteEvent(
            &state.first_window_client_create_window_ok,
            &state.first_window_client_create_window_ok_tick,
            now_tick,
            "first_window_client_create_window_ok",
            elapsed(true, now_tick, state.first_window_client_create_begin, state.first_window_client_create_begin_tick),
        );
    }
    if (containsBytes(message, "window_client: publish ok cap=")) {
        noteEvent(
            &state.first_window_client_publish_ok,
            &state.first_window_client_publish_ok_tick,
            now_tick,
            "first_window_client_publish_ok",
            elapsed(true, now_tick, state.first_window_client_create_window_ok, state.first_window_client_create_window_ok_tick),
        );
    }

    maybeEmitSummaries(writeFn);

    if (state.compositor_present_flush_done) {
        flushTrace(writeFn);
    }
}
