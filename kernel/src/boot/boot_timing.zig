const std = @import("std");
const kernel = @import("../kernel.zig");
const scheduler = @import("../scheduler.zig");

pub const State = struct {
    user_enter_tick: u64 = 0,
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
    virtgpu_init_started: bool = false,
    virtgpu_init_start_tick: u64 = 0,
    virtgpu_queue_ready: bool = false,
    virtgpu_queue_ready_tick: u64 = 0,
    mouse_demo_window_publish_after: bool = false,
    mouse_demo_window_publish_after_tick: u64 = 0,
    compositor_window_cap_recv: bool = false,
    compositor_window_cap_recv_tick: u64 = 0,
    compositor_register_window_cap_done: bool = false,
    compositor_register_window_cap_done_tick: u64 = 0,
    compositor_first_compose_done: bool = false,
    compositor_first_compose_done_tick: u64 = 0,
    compositor_present_transfer_done: bool = false,
    compositor_present_transfer_done_tick: u64 = 0,
    compositor_present_flush_done: bool = false,
    compositor_present_flush_done_tick: u64 = 0,
};

pub const TraceEvent = struct {
    label: []const u8,
    tick: u64,
    has_delta: bool,
    delta: u64,
};

pub var state: State = .{};
var trace: [16]TraceEvent = undefined;
var trace_len: usize = 0;
var trace_flushed = false;

fn containsBytes(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
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
    if (containsBytes(message, "MouseDriver: started")) {
        if (!state.mouse_started) {
            state.mouse_started = true;
            state.mouse_start_tick = now_tick;
            recordEvent("mouse_started", state.mouse_start_tick, null);
        }
    }
    if (containsBytes(message, "MouseDriver: queue ready")) {
        if (!state.mouse_queue_ready) {
            state.mouse_queue_ready = true;
            state.mouse_queue_ready_tick = now_tick;
            recordEvent(
                "mouse_queue_ready",
                state.mouse_queue_ready_tick,
                if (state.mouse_started) state.mouse_queue_ready_tick - state.mouse_start_tick else null,
            );
        }
    }
    if (containsBytes(message, "KeyboardDriver: started")) {
        if (!state.keyboard_started) {
            state.keyboard_started = true;
            state.keyboard_start_tick = now_tick;
            recordEvent("keyboard_started", state.keyboard_start_tick, null);
        }
    }
    if (containsBytes(message, "KeyboardDriver: queue ready")) {
        if (!state.keyboard_queue_ready) {
            state.keyboard_queue_ready = true;
            state.keyboard_queue_ready_tick = now_tick;
            recordEvent(
                "keyboard_queue_ready",
                state.keyboard_queue_ready_tick,
                if (state.keyboard_started) state.keyboard_queue_ready_tick - state.keyboard_start_tick else null,
            );
        }
    }
    if (containsBytes(message, "Compositor: virtgpu init start")) {
        if (!state.virtgpu_init_started) {
            state.virtgpu_init_started = true;
            state.virtgpu_init_start_tick = now_tick;
            recordEvent(
                "virtgpu_init_start",
                state.virtgpu_init_start_tick,
                if (state.compositor_launch) state.virtgpu_init_start_tick - state.compositor_launch_tick else null,
            );
        }
    }
    if (containsBytes(message, "Compositor: virtgpu queue ready")) {
        if (!state.virtgpu_queue_ready) {
            state.virtgpu_queue_ready = true;
            state.virtgpu_queue_ready_tick = now_tick;
            scheduler.compositor_thread1_priority_active = false;
            recordEvent(
                "virtgpu_queue_ready",
                state.virtgpu_queue_ready_tick,
                if (state.virtgpu_init_started) state.virtgpu_queue_ready_tick - state.virtgpu_init_start_tick else null,
            );
        }
    }
    if (containsBytes(message, "MouseButtonDemo: createAndPublishWindow after")) {
        if (!state.mouse_demo_window_publish_after) {
            state.mouse_demo_window_publish_after = true;
            state.mouse_demo_window_publish_after_tick = now_tick;
            recordEvent(
                "mouse_demo_window_publish_after",
                state.mouse_demo_window_publish_after_tick,
                if (state.virtgpu_queue_ready) state.mouse_demo_window_publish_after_tick - state.virtgpu_queue_ready_tick else null,
            );
        }
    }
    if (containsBytes(message, "Compositor: window cap recv")) {
        if (!state.compositor_window_cap_recv) {
            state.compositor_window_cap_recv = true;
            state.compositor_window_cap_recv_tick = now_tick;
            scheduler.compositor_thread1_priority_active = true;
            recordEvent(
                "compositor_window_cap_recv",
                state.compositor_window_cap_recv_tick,
                if (state.mouse_demo_window_publish_after) state.compositor_window_cap_recv_tick - state.mouse_demo_window_publish_after_tick else null,
            );
        }
    }
    if (containsBytes(message, "Compositor: registerWindowCap done")) {
        if (!state.compositor_register_window_cap_done) {
            state.compositor_register_window_cap_done = true;
            state.compositor_register_window_cap_done_tick = now_tick;
            recordEvent(
                "compositor_register_window_cap_done",
                state.compositor_register_window_cap_done_tick,
                if (state.compositor_window_cap_recv) state.compositor_register_window_cap_done_tick - state.compositor_window_cap_recv_tick else null,
            );
        }
    }
    if (containsBytes(message, "Compositor: first compose done")) {
        if (!state.compositor_first_compose_done) {
            state.compositor_first_compose_done = true;
            state.compositor_first_compose_done_tick = now_tick;
            scheduler.compositor_thread1_priority_active = false;
            recordEvent(
                "compositor_first_compose_done",
                state.compositor_first_compose_done_tick,
                if (state.compositor_register_window_cap_done) state.compositor_first_compose_done_tick - state.compositor_register_window_cap_done_tick else null,
            );
        }
    }
    if (containsBytes(message, "Compositor: drag boost on")) {
        scheduler.compositor_thread1_priority_active = true;
        scheduler.compositor_thread1_priority_streak = 0;
    }
    if (containsBytes(message, "Compositor: drag boost off")) {
        scheduler.compositor_thread1_priority_active = false;
        scheduler.compositor_thread1_priority_streak = 0;
    }
    if (containsBytes(message, "Compositor: present transfer done")) {
        if (!state.compositor_present_transfer_done) {
            state.compositor_present_transfer_done = true;
            state.compositor_present_transfer_done_tick = now_tick;
            recordEvent(
                "compositor_present_transfer_done",
                state.compositor_present_transfer_done_tick,
                if (state.compositor_first_compose_done) state.compositor_present_transfer_done_tick - state.compositor_first_compose_done_tick else null,
            );
        }
    }
    if (containsBytes(message, "Compositor: present flush done")) {
        if (!state.compositor_present_flush_done) {
            state.compositor_present_flush_done = true;
            state.compositor_present_flush_done_tick = now_tick;
            recordEvent(
                "compositor_present_flush_done",
                state.compositor_present_flush_done_tick,
                if (state.compositor_present_transfer_done) state.compositor_present_flush_done_tick - state.compositor_present_transfer_done_tick else null,
            );
            flushTrace(writeFn);
        }
    }
}
