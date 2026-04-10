const std = @import("std");
const kernel = @import("../kernel.zig");

pub const Kind = enum {
    classic,
    gpu,
};

pub var launch_armed = false;
pub var launched = false;
pub var target_principal: ?kernel.PrincipalId = null;
pub var user_page_paddr: u64 = 0;
pub var user_stack_page_paddr: u64 = 0;
pub var auto_launch_armed = false;
pub var auto_launch_tick: u64 = 0;
pub var wait_mouse_queue_ready = false;
pub var wait_keyboard_queue_ready = false;
pub var auto_debug_last_mask: u32 = 0xFFFF_FFFF;
pub var auto_debug_last_tick: u64 = 0;
pub var backing_classic: ?kernel.ImageBacking = null;
pub var backing_gpu: ?kernel.ImageBacking = null;
pub var selected_kind: Kind = .classic;
pub var keyboard_source_principal: ?kernel.PrincipalId = null;

fn containsBytes(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

pub fn reset() void {
    launch_armed = false;
    launched = false;
    target_principal = null;
    user_page_paddr = 0;
    user_stack_page_paddr = 0;
    auto_launch_armed = false;
    auto_launch_tick = 0;
    wait_mouse_queue_ready = false;
    wait_keyboard_queue_ready = false;
    auto_debug_last_mask = 0xFFFF_FFFF;
    auto_debug_last_tick = 0;
    backing_classic = null;
    backing_gpu = null;
    selected_kind = .classic;
    keyboard_source_principal = null;
}

pub fn arm(
    classic_backing: ?kernel.ImageBacking,
    gpu_backing: ?kernel.ImageBacking,
    launch_target_principal: kernel.PrincipalId,
    launch_keyboard_source_principal: ?kernel.PrincipalId,
    target_user_page_paddr: u64,
    target_user_stack_paddr: u64,
    auto_launch: bool,
    auto_launch_tick_value: u64,
    wait_mouse_ready: bool,
    wait_keyboard_ready: bool,
) void {
    backing_classic = classic_backing;
    backing_gpu = gpu_backing;
    target_principal = launch_target_principal;
    keyboard_source_principal = launch_keyboard_source_principal;
    user_page_paddr = target_user_page_paddr;
    user_stack_page_paddr = target_user_stack_paddr;
    launch_armed = true;
    launched = false;
    selected_kind = if (gpu_backing != null) .gpu else .classic;
    auto_launch_armed = auto_launch;
    auto_launch_tick = auto_launch_tick_value;
    wait_mouse_queue_ready = wait_mouse_ready;
    wait_keyboard_queue_ready = wait_keyboard_ready;
}

pub fn label(kind: Kind) []const u8 {
    return switch (kind) {
        .classic => "classic",
        .gpu => "gpu",
    };
}

pub fn currentImage() ?kernel.ImageBacking {
    return switch (selected_kind) {
        .classic => backing_classic orelse backing_gpu,
        .gpu => backing_gpu orelse backing_classic,
    };
}

pub fn markLoadFailed() void {
    launch_armed = false;
    auto_launch_armed = false;
    wait_mouse_queue_ready = false;
    wait_keyboard_queue_ready = false;
}

pub fn markLaunched() void {
    launched = true;
    launch_armed = false;
    auto_launch_armed = false;
    wait_mouse_queue_ready = false;
    wait_keyboard_queue_ready = false;
}

pub fn handleKeyboardLog(proc: anytype, message: []const u8, writeFn: fn ([]const u8) void) bool {
    if (!launch_armed or launched) return false;
    if (keyboard_source_principal) |principal| {
        if (proc != principal) return false;
    }
    if (containsBytes(message, "key code=0x21 value=1")) {
        selected_kind = .classic;
        writeFn("deferred compositor mode: classic\n");
        return false;
    }
    if (containsBytes(message, "key code=0x22 value=1")) {
        if (backing_gpu != null) {
            selected_kind = .gpu;
            writeFn("deferred compositor mode: gpu\n");
        } else {
            writeFn("deferred compositor mode: gpu unavailable\n");
        }
        return false;
    }
    return containsBytes(message, "key code=0x1c value=1") or
        containsBytes(message, "key code=0x60 value=1");
}

pub fn shouldAutoLaunch(
    now_tick: u64,
    bootlog_entered_user_tick: u64,
    bootlog_auto_min_visible_ticks: u64,
    boot_log_status_flags: u32,
    mouse_ready_flag: u32,
    keyboard_ready_flag: u32,
) bool {
    if (!auto_launch_armed) return false;
    if (!launch_armed or launched) {
        auto_launch_armed = false;
        return false;
    }
    var block_mask: u32 = 0;
    if (wait_mouse_queue_ready and (boot_log_status_flags & mouse_ready_flag) == 0) block_mask |= 1 << 0;
    if (wait_keyboard_queue_ready and (boot_log_status_flags & keyboard_ready_flag) == 0) block_mask |= 1 << 1;
    if (now_tick < bootlog_entered_user_tick + bootlog_auto_min_visible_ticks) block_mask |= 1 << 2;
    if (now_tick < auto_launch_tick) block_mask |= 1 << 3;
    if (block_mask != 0) {
        auto_debug_last_mask = block_mask;
        auto_debug_last_tick = now_tick;
        return false;
    }
    auto_debug_last_mask = 0;
    auto_debug_last_tick = now_tick;
    return true;
}
