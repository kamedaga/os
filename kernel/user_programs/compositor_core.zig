const std = @import("std");
const cap_transfer_abi = @import("cap_transfer_abi.zig");
const font = @import("font.zig");
const protocol = @import("window_protocol.zig");
const model = @import("compositor_model.zig");
const virtgpu = @import("virtgpu.zig");

const syscall_map_page: u64 = 0x2;
const syscall_grant_cap: u64 = 0x8;
const syscall_log: u64 = 0x9;
const syscall_recv_cap: u64 = 0xA;
const syscall_map_pages_batch: u64 = 0x15;
const syscall_wait_event: u64 = 0x17;

const syscall_ok: u64 = 0;
const syscall_err_empty: u64 = 13;

const shared_page_va: usize = 0x3C00_3000;
const taskbar_state_shared_va: usize = 0x3C00_4000;
const taskbar_command_shared_va: usize = 0x3C00_6000;
const ipc_rx_page_va: usize = 0x3C10_8000;

const shared_magic = protocol.mouse_shared_magic;
const taskbar_state_magic = protocol.taskbar_state_magic;
const taskbar_command_magic = protocol.taskbar_command_magic;
const taskbar_protocol_version = protocol.taskbar_protocol_version;
const taskbar_entry_flag_visible = protocol.taskbar_entry_flag_visible;
const taskbar_command_none = protocol.taskbar_command_none;
const taskbar_command_activate = protocol.taskbar_command_activate;
const window_cap_magic = protocol.window_cap_magic;
const window_meta_magic = protocol.window_meta_magic;
const window_flag_low_scale = protocol.window_flag_low_scale;
const window_flag_frameless = protocol.window_flag_frameless;
const process0_id: u64 = 0;
const rights_read_write: u64 = 0x3;

const fb_width: usize = 832;
const fb_height: usize = 624;
const fb_pitch: usize = 832;
const fb_pixels: usize = fb_pitch * fb_height;
const fb_width_i32: i32 = @as(i32, @intCast(fb_width));
const fb_height_i32: i32 = @as(i32, @intCast(fb_height));

const pixel_width: usize = 32;
const pixel_height: usize = 32;
const pixel_pitch: usize = 32;

const max_windows: usize = 5;

const background_color: u32 = 0x00FD_FDFD;
const window_border_color: u32 = 0x0087_837E;
const window_header_top_color: u32 = 0x009A_B6_D2;
const window_header_bottom_color: u32 = 0x00C2_D7_ED;
const window_header_fill_color: u32 = 0x00B1_C6_DC;
const window_header_text_color: u32 = 0x0033_383A;
const window_header_divider_color: u32 = 0x00B8_B4AF;
const window_header_divider_soft_color: u32 = 0x00CC_C7C3;
const window_inner_frame_color: u32 = 0x00D7_E4_F3;
const window_content_bg_color: u32 = 0x00FF_FFFF;
const minimize_btn_top_color: u32 = 0x0099_A7_B7;
const minimize_btn_bottom_color: u32 = 0x008D_9AAA;
const minimize_btn_border_color: u32 = 0x0055_606B;
const minimize_btn_symbol_color: u32 = 0x00F2_F5_F8;
const close_btn_top_color: u32 = 0x00C9_66_53;
const close_btn_bottom_color: u32 = 0x00C5_62_4E;
const close_btn_border_color: u32 = 0x007E_49_4D;
const close_btn_cross_color: u32 = 0x00F7_ED_EA;
const window_header_highlight_color: u32 = 0x00FF_FFFF;
const window_shadow_color: u32 = 0x0000_0000;
const window_shadow_right_extent: i32 = 2;
const window_shadow_bottom_extent: i32 = 3;

const default_window_scale: usize = 10;
const low_window_scale: usize = 1;
const window_border: usize = 1;
const window_header_h: usize = 27;
const window_minimize_width: usize = 22;
const window_minimize_height: usize = 14;
const window_close_width: usize = 22;
const window_close_height: usize = 14;
const window_button_gap: usize = 4;
const window_close_margin: usize = 8;
const window_corner_cut: usize = 5;
const window_title_y_bias: i32 = 1;
const window_border_i32: i32 = @as(i32, @intCast(window_border));
const window_header_h_i32: i32 = @as(i32, @intCast(window_header_h));
const window_minimize_width_i32: i32 = @as(i32, @intCast(window_minimize_width));
const window_minimize_height_i32: i32 = @as(i32, @intCast(window_minimize_height));
const window_close_width_i32: i32 = @as(i32, @intCast(window_close_width));
const window_close_height_i32: i32 = @as(i32, @intCast(window_close_height));
const window_button_gap_i32: i32 = @as(i32, @intCast(window_button_gap));
const window_close_margin_i32: i32 = @as(i32, @intCast(window_close_margin));

const taskbar_window_height: i32 = 32;
const taskbar_process_left_pad: i32 = 8;
const taskbar_process_top_pad: i32 = 0;
const taskbar_process_button_gap: i32 = 6;
const taskbar_process_button_max_width: i32 = 164;
const taskbar_process_right_reserved: i32 = 96;
const taskbar_process_bg: u32 = 0x00AE_C5_DF;
const taskbar_process_grad_start: u32 = 0x00DE_E7_F2;
const taskbar_process_border: u32 = 0x0022_27_2D;
const taskbar_process_border_left_bottom: u32 = 0x006A_79_89;
const taskbar_process_inner_highlight: u32 = 0x00F3_F3_F3;
const taskbar_process_hover_glow_color: u32 = 0x00CE_E2_F0;
const taskbar_process_hidden_text: u32 = 0x00F2_F6_FA;
const taskbar_process_text: u32 = 0x0022_27_2D;
const taskbar_process_radius: i32 = 3;
const taskbar_process_gradient_width: i32 = 48;

const window_title_offset: usize = 16;
const window_title_max_bytes = protocol.window_title_max_bytes;
const window_map_base_va: usize = 0x3C11_0000;
const max_window_pixel_pages: usize = 128;
const max_window_pixel_bytes: usize = max_window_pixel_pages * 4096;
const window_map_stride_va: usize = (2 + max_window_pixel_pages) * 4096;
const WindowCap = protocol.WindowCap;
const WindowMeta = protocol.WindowMeta;
const MouseSharedPage = protocol.MouseSharedPage;
const TaskbarStatePage = protocol.TaskbarStatePage;
const TaskbarCommandPage = protocol.TaskbarCommandPage;
const WindowCapSnapshot = struct {
    window_id: u32,
    pixels_cap_paddr: u64,
    pixels_page_count: usize,
    meta_cap_paddr: u64,
    width: usize,
    height: usize,
    pitch: usize,
    flags: u32,
};
const Rect = model.Rect;
const SourceRegion = model.SourceRegion;
const MouseState = model.MouseState;
const WindowSlot = model.WindowSlot;
const WindowState = model.WindowFrame;
const WindowSource = model.WindowSource;
const WindowStore = model.WindowStore(max_windows);

var back_buffer_storage: [fb_pixels]u32 align(64) = [_]u32{0} ** fb_pixels;
var window_store: WindowStore = .{};
var window_shadow_valid: [max_windows]bool = [_]bool{false} ** max_windows;
var window_gpu_resources: [max_windows]?virtgpu.ResourceHandle = [_]?virtgpu.ResourceHandle{null} ** max_windows;
var logged_invalid_window_source: [max_windows]bool = [_]bool{false} ** max_windows;
var next_window_z_order: u32 = 1;
var window_paint_order_cache: [max_windows]usize = [_]usize{0} ** max_windows;
var window_paint_order_count: usize = 0;
var window_paint_order_dirty = true;
var first_compose_logged = false;
var first_present_transfer_logged = false;
var first_present_flush_logged = false;
var logged_invalid_gpu_slot: ?usize = null;
var logged_first_gpu_sync_slot: bool = false;
var debug_recent_window_slot: ?usize = null;
var debug_recent_window_sync_logged = false;
var debug_recent_window_draw_logged = false;
const compositor_perf_report_min_wall_tsc: u64 = 1_000_000_000;
const CompositorPerfReport = struct {
    start_tsc: u64 = 0,
    total_tsc: u64 = 0,
    sync_tsc: u64 = 0,
    compose_tsc: u64 = 0,
    submit_tsc: u64 = 0,
    frames: u64 = 0,
    presents: u64 = 0,
    idle_loops: u64 = 0,
    no_window_loops: u64 = 0,
    full_frames: u64 = 0,
    dirty_frames: u64 = 0,
    present_area_sum: u64 = 0,
};
var compositor_perf_report: CompositorPerfReport = .{};
var mouse_state_storage: MouseState = .{
    .x = @as(i32, @intCast(fb_width / 2)),
    .y = @as(i32, @intCast(fb_height / 2)),
};

const DrawSurface = struct {
    pixels: [*]volatile u32,
    pixel_capacity: usize,
    clip: Rect,
};

fn surfaceIndex(surface: *const DrawSurface, x: i32, y: i32) ?usize {
    if (x < surface.clip.x0 or x >= surface.clip.x1 or y < surface.clip.y0 or y >= surface.clip.y1) return null;
    if (x < 0 or y < 0 or x >= fb_width_i32 or y >= fb_height_i32) return null;
    const ux: usize = @intCast(x);
    const uy: usize = @intCast(y);
    const index = uy * fb_pitch + ux;
    if (index >= surface.pixel_capacity) return null;
    return index;
}

fn readTsc() u64 {
    var lo: u32 = 0;
    var hi: u32 = 0;
    asm volatile ("rdtsc"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
    );
    return (@as(u64, hi) << 32) | @as(u64, lo);
}

const cursor_width: usize = 15;
const cursor_height: usize = 24;
const cursor_dim: usize = 64;
const cursor_width_i32: i32 = 15;
const cursor_height_i32: i32 = 24;
const cursor_shape = [cursor_height][cursor_width]u8{
    "@              ".*,
    "@@             ".*,
    "@.@            ".*,
    "@..@           ".*,
    "@...@          ".*,
    "@....@         ".*,
    "@.....@        ".*,
    "@......@       ".*,
    "@.......@      ".*,
    "@........@     ".*,
    "@.........@    ".*,
    "@..........@   ".*,
    "@...........@  ".*,
    "@............@ ".*,
    "@......@@@@@@@@".*,
    "@......@       ".*,
    "@....@@.@      ".*,
    "@...@ @.@      ".*,
    "@..@   @.@     ".*,
    "@.@    @.@     ".*,
    "@@      @.@    ".*,
    "@       @.@    ".*,
    "         @.@   ".*,
    "         @@@   ".*,
};

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

pub fn earlyStartLog() void {
    _ = userLog("Compositor: _start\n");
}

fn appendDiagText(buf: []u8, idx: *usize, text: []const u8) void {
    if (idx.* >= buf.len) return;
    const remaining = buf.len - idx.*;
    const copy_len = if (text.len < remaining) text.len else remaining;
    @memcpy(buf[idx.* .. idx.* + copy_len], text[0..copy_len]);
    idx.* += copy_len;
}

fn appendDiagU64Decimal(buf: []u8, idx: *usize, value: u64) void {
    if (idx.* >= buf.len) return;
    const text = std.fmt.bufPrint(buf[idx.*..], "{}", .{value}) catch return;
    idx.* += text.len;
}

fn appendDiagHexU64(buf: []u8, idx: *usize, value: u64) void {
    if (idx.* >= buf.len) return;
    const text = std.fmt.bufPrint(buf[idx.*..], "0x{x}", .{value}) catch return;
    idx.* += text.len;
}

fn appendDiagBoolDigit(buf: []u8, idx: *usize, value: bool) void {
    appendDiagText(buf, idx, if (value) "1" else "0");
}

fn logInvalidGpuSlot(slot: usize) void {
    if (logged_invalid_gpu_slot != null and logged_invalid_gpu_slot.? == slot) return;
    logged_invalid_gpu_slot = slot;
    var buf: [96]u8 = undefined;
    var idx: usize = 0;
    appendDiagText(buf[0..], &idx, "Compositor: invalid gpu slot=");
    appendDiagU64Decimal(buf[0..], &idx, slot);
    appendDiagText(buf[0..], &idx, "\n");
    _ = userLog(buf[0..idx]);
}

fn logInvalidWindowSource(slot: usize, src: *const WindowSource, reason: []const u8) void {
    if (slot < logged_invalid_window_source.len and logged_invalid_window_source[slot]) return;
    if (slot < logged_invalid_window_source.len) logged_invalid_window_source[slot] = true;
    var buf: [256]u8 = undefined;
    var idx: usize = 0;
    appendDiagText(buf[0..], &idx, "Compositor: invalid window source slot=");
    appendDiagU64Decimal(buf[0..], &idx, slot);
    appendDiagText(buf[0..], &idx, " window_id=");
    appendDiagU64Decimal(buf[0..], &idx, src.window_id);
    appendDiagText(buf[0..], &idx, " reason=");
    appendDiagText(buf[0..], &idx, reason);
    appendDiagText(buf[0..], &idx, " width=");
    appendDiagU64Decimal(buf[0..], &idx, src.width);
    appendDiagText(buf[0..], &idx, " height=");
    appendDiagU64Decimal(buf[0..], &idx, src.height);
    appendDiagText(buf[0..], &idx, " pitch=");
    appendDiagU64Decimal(buf[0..], &idx, src.pitch);
    appendDiagText(buf[0..], &idx, " pages=");
    appendDiagU64Decimal(buf[0..], &idx, src.pixels_page_count);
    appendDiagText(buf[0..], &idx, "\n");
    _ = userLog(buf[0..idx]);
}

fn windowSourcePixelsReadable(slot: usize, src: *const WindowSource) bool {
    if (!src.active or src.pixel_va == 0) {
        logInvalidWindowSource(slot, src, "inactive");
        return false;
    }
    if (src.width == 0 or src.height == 0 or src.pitch == 0) {
        logInvalidWindowSource(slot, src, "zero_dim");
        return false;
    }
    if (src.pitch < src.width) {
        logInvalidWindowSource(slot, src, "pitch_lt_width");
        return false;
    }
    if (src.pixels_page_count == 0) {
        logInvalidWindowSource(slot, src, "zero_pages");
        return false;
    }

    const capacity_pixels = (src.pixels_page_count * 4096) / @sizeOf(u32);
    const row_base = std.math.mul(usize, src.height - 1, src.pitch) catch {
        logInvalidWindowSource(slot, src, "row_overflow");
        return false;
    };
    const last_index = std.math.add(usize, row_base, src.width - 1) catch {
        logInvalidWindowSource(slot, src, "index_overflow");
        return false;
    };
    if (last_index >= capacity_pixels) {
        logInvalidWindowSource(slot, src, "out_of_range");
        return false;
    }
    return true;
}

fn logFirstGpuSyncSlot(slot: usize, source: *const WindowSource, frame: *const WindowState) void {
    if (logged_first_gpu_sync_slot) return;
    logged_first_gpu_sync_slot = true;
    var buf: [256]u8 = undefined;
    var idx: usize = 0;
    var src0: u32 = 0;
    var src_center: u32 = 0;
    if (windowSourcePixelsReadable(slot, source)) {
        const src_pixels: [*]const volatile u32 = @ptrFromInt(source.pixel_va);
        src0 = src_pixels[0];
        const center_x = source.width / 2;
        const center_y = source.height / 2;
        src_center = src_pixels[center_y * source.pitch + center_x];
    }
    appendDiagText(buf[0..], &idx, "Compositor: gpu sync slot=");
    appendDiagU64Decimal(buf[0..], &idx, slot);
    appendDiagText(buf[0..], &idx, " window_id=");
    appendDiagU64Decimal(buf[0..], &idx, source.window_id);
    appendDiagText(buf[0..], &idx, " width=");
    appendDiagU64Decimal(buf[0..], &idx, source.width);
    appendDiagText(buf[0..], &idx, " height=");
    appendDiagU64Decimal(buf[0..], &idx, source.height);
    appendDiagText(buf[0..], &idx, " pitch=");
    appendDiagU64Decimal(buf[0..], &idx, source.pitch);
    appendDiagText(buf[0..], &idx, " src_active=");
    appendDiagBoolDigit(buf[0..], &idx, source.active);
    appendDiagText(buf[0..], &idx, " frame_active=");
    appendDiagBoolDigit(buf[0..], &idx, frame.active);
    appendDiagText(buf[0..], &idx, " frame_visible=");
    appendDiagBoolDigit(buf[0..], &idx, frame.visible);
    appendDiagText(buf[0..], &idx, " src0=");
    appendDiagHexU64(buf[0..], &idx, src0);
    appendDiagText(buf[0..], &idx, " src_center=");
    appendDiagHexU64(buf[0..], &idx, src_center);
    appendDiagText(buf[0..], &idx, "\n");
    _ = userLog(buf[0..idx]);
}

fn logWindowRegistered(slot: usize, window_id: u32, width: usize, height: usize, flags: u32) void {
    var buf: [160]u8 = undefined;
    var idx: usize = 0;
    appendDiagText(buf[0..], &idx, "Compositor: window registered slot=");
    appendDiagU64Decimal(buf[0..], &idx, slot);
    appendDiagText(buf[0..], &idx, " id=");
    appendDiagU64Decimal(buf[0..], &idx, window_id);
    appendDiagText(buf[0..], &idx, " width=");
    appendDiagU64Decimal(buf[0..], &idx, width);
    appendDiagText(buf[0..], &idx, " height=");
    appendDiagU64Decimal(buf[0..], &idx, height);
    appendDiagText(buf[0..], &idx, " flags=");
    appendDiagHexU64(buf[0..], &idx, flags);
    appendDiagText(buf[0..], &idx, "\n");
    _ = userLog(buf[0..idx]);
}

fn logRecentWindowStage(slot: usize, stage: []const u8) void {
    if (debug_recent_window_slot == null or debug_recent_window_slot.? != slot) return;
    var buf: [96]u8 = undefined;
    var idx: usize = 0;
    appendDiagText(buf[0..], &idx, "Compositor: recent window slot=");
    appendDiagU64Decimal(buf[0..], &idx, slot);
    appendDiagText(buf[0..], &idx, " stage=");
    appendDiagText(buf[0..], &idx, stage);
    appendDiagText(buf[0..], &idx, "\n");
    _ = userLog(buf[0..idx]);
}

fn logWindowRegisterRejected(reason: []const u8, page_paddr: u64) void {
    var buf: [160]u8 = undefined;
    var idx: usize = 0;
    appendDiagText(buf[0..], &idx, "Compositor: window rejected reason=");
    appendDiagText(buf[0..], &idx, reason);
    appendDiagText(buf[0..], &idx, " paddr=");
    appendDiagHexU64(buf[0..], &idx, page_paddr);
    appendDiagText(buf[0..], &idx, "\n");
    _ = userLog(buf[0..idx]);
}

fn invalidateWindowPaintOrderCache() void {
    window_paint_order_dirty = true;
}

fn setSchedulerBoost(active: bool, state: *bool) void {
    if (state.* == active) return;
    state.* = active;
    _ = userLog(if (active) "Compositor: drag boost on\n" else "Compositor: drag boost off\n");
}

fn invalidateWindowBodyCache(slot: usize) void {
    _ = slot;
}

fn snapshotWindowCap(cap: *const volatile WindowCap) WindowCapSnapshot {
    return .{
        .window_id = cap.window_id,
        .pixels_cap_paddr = cap.pixels_cap_paddr,
        .pixels_page_count = cap.pixels_page_count,
        .meta_cap_paddr = cap.meta_cap_paddr,
        .width = cap.width,
        .height = cap.height,
        .pitch = cap.pixels_per_scan_line,
        .flags = cap.flags,
    };
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapPagesBatch(base_va: u64, paddr_list_va: u64, page_count: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_pages_batch),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (paddr_list_va),
          [arg2] "{rdx}" (page_count),
          [arg3] "{rcx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn recvCap() u64 {
    const transfer = recvCapTransfer();
    if (transfer < cap_transfer_abi.transfer_id_min) return transfer;
    return acceptCapTransfer(transfer);
}

fn recvCapTransfer() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_recv_cap),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn acceptCapTransfer(transfer_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (cap_transfer_abi.syscall_accept_cap_transfer),
          [arg0] "{rdi}" (transfer_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCap(to_process: u64, paddr: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (to_process),
          [arg2] "{rdx}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn clampI32(v: i32, lo: i32, hi: i32) i32 {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

fn decodeSharedCoord(raw: u64, limit: i32, fallback: i32) i32 {
    if (limit <= 0) return fallback;
    const max_ok: u64 = @intCast(limit - 1);
    if (raw > max_ok) return fallback;
    return @intCast(raw);
}

fn syncMouseState(force: bool) void {
    if (!mouse_state_storage.ready) return;
    const shared_now: *const volatile MouseSharedPage = @ptrFromInt(shared_page_va);
    if (shared_now.magic != shared_magic) return;
    const seq = shared_now.seq;
    if (!force and seq == mouse_state_storage.seq) return;
    mouse_state_storage.seq = seq;
    mouse_state_storage.x = decodeSharedCoord(shared_now.cursor_x, fb_width_i32, mouse_state_storage.x);
    mouse_state_storage.y = decodeSharedCoord(shared_now.cursor_y, fb_height_i32, mouse_state_storage.y);
    mouse_state_storage.buttons = shared_now.buttons;
}

fn setBackPixel(back: [*]u32, x: i32, y: i32, color: u32) void {
    if (x < 0 or y < 0 or x >= fb_width_i32 or y >= fb_height_i32) return;
    const ux: usize = @intCast(x);
    const uy: usize = @intCast(y);
    back[uy * fb_pitch + ux] = color;
}

fn blendBackPixel(back: [*]u32, x: i32, y: i32, color: u32, alpha: u8) void {
    if (x < 0 or y < 0 or x >= fb_width_i32 or y >= fb_height_i32) return;
    const ux: usize = @intCast(x);
    const uy: usize = @intCast(y);
    const index = uy * fb_pitch + ux;
    back[index] = font.blendColor(back[index], color, alpha);
}

fn blendBackPixelSubpixel(back: [*]u32, x: i32, y: i32, color: u32, alpha_r: u8, alpha_g: u8, alpha_b: u8) void {
    if (x < 0 or y < 0 or x >= fb_width_i32 or y >= fb_height_i32) return;
    const ux: usize = @intCast(x);
    const uy: usize = @intCast(y);
    const index = uy * fb_pitch + ux;
    back[index] = font.blendSubpixelColor(back[index], color, alpha_r, alpha_g, alpha_b);
}

fn drawSolidRect(back: [*]u32, x: i32, y: i32, w: i32, h: i32, color: u32) void {
    if (w <= 0 or h <= 0) return;
    var x0 = x;
    var y0 = y;
    var x1 = x + w;
    var y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_width_i32) x1 = fb_width_i32;
    if (y1 > fb_height_i32) y1 = fb_height_i32;
    if (x0 >= x1 or y0 >= y1) return;

    const ux0: usize = @intCast(x0);
    const ux1: usize = @intCast(x1);
    var yy: usize = @intCast(y0);
    const yy1: usize = @intCast(y1);
    while (yy < yy1) : (yy += 1) {
        const row = yy * fb_pitch;
        var xx: usize = ux0;
        while (xx < ux1) : (xx += 1) {
            back[row + xx] = color;
        }
    }
}

fn pointInRect(px: i32, py: i32, rect: Rect) bool {
    return px >= rect.x0 and px < rect.x1 and py >= rect.y0 and py < rect.y1;
}

fn includeRect(any: bool, accum: Rect, add: Rect) struct { any: bool, rect: Rect } {
    if (!any) {
        return .{ .any = true, .rect = add };
    }
    var out = accum;
    if (add.x0 < out.x0) out.x0 = add.x0;
    if (add.y0 < out.y0) out.y0 = add.y0;
    if (add.x1 > out.x1) out.x1 = add.x1;
    if (add.y1 > out.y1) out.y1 = add.y1;
    return .{ .any = true, .rect = out };
}

fn rectIsEmpty(rect: Rect) bool {
    return rect.x0 >= rect.x1 or rect.y0 >= rect.y1;
}

fn clipRectToScreen(rect: Rect) Rect {
    return .{
        .x0 = clampI32(rect.x0, 0, fb_width_i32),
        .y0 = clampI32(rect.y0, 0, fb_height_i32),
        .x1 = clampI32(rect.x1, 0, fb_width_i32),
        .y1 = clampI32(rect.y1, 0, fb_height_i32),
    };
}

fn intersectRect(a: Rect, b: Rect) ?Rect {
    const rect: Rect = .{
        .x0 = if (a.x0 > b.x0) a.x0 else b.x0,
        .y0 = if (a.y0 > b.y0) a.y0 else b.y0,
        .x1 = if (a.x1 < b.x1) a.x1 else b.x1,
        .y1 = if (a.y1 < b.y1) a.y1 else b.y1,
    };
    if (rectIsEmpty(rect)) return null;
    return rect;
}

fn rectContainsRect(outer: Rect, inner: Rect) bool {
    return inner.x0 >= outer.x0 and
        inner.y0 >= outer.y0 and
        inner.x1 <= outer.x1 and
        inner.y1 <= outer.y1;
}

fn fullScreenRect() Rect {
    return .{ .x0 = 0, .y0 = 0, .x1 = fb_width_i32, .y1 = fb_height_i32 };
}

fn hudRect() Rect {
    return .{ .x0 = 14, .y0 = 14, .x1 = 14 + 176, .y1 = 14 + font.lineHeight(1) + 8 };
}

fn setSurfacePixel(surface: *const DrawSurface, x: i32, y: i32, color: u32) void {
    const index = surfaceIndex(surface, x, y) orelse return;
    surface.pixels[index] = color;
}

fn blendSurfacePixel(surface: *const DrawSurface, x: i32, y: i32, color: u32, alpha: u8) void {
    const index = surfaceIndex(surface, x, y) orelse return;
    surface.pixels[index] = font.blendColor(surface.pixels[index], color, alpha);
}

fn blendSurfacePixelSubpixel(surface: *const DrawSurface, x: i32, y: i32, color: u32, alpha_r: u8, alpha_g: u8, alpha_b: u8) void {
    const index = surfaceIndex(surface, x, y) orelse return;
    surface.pixels[index] = font.blendSubpixelColor(surface.pixels[index], color, alpha_r, alpha_g, alpha_b);
}

fn drawSolidRectSurface(surface: *const DrawSurface, x: i32, y: i32, w: i32, h: i32, color: u32) void {
    if (w <= 0 or h <= 0) return;
    const rect = clipRectToScreen(.{ .x0 = x, .y0 = y, .x1 = x + w, .y1 = y + h });
    const clipped = intersectRect(rect, surface.clip) orelse return;
    var yy = clipped.y0;
    while (yy < clipped.y1) : (yy += 1) {
        const row: usize = @as(usize, @intCast(yy)) * fb_pitch;
        var xx = clipped.x0;
        while (xx < clipped.x1) : (xx += 1) {
            const index = row + @as(usize, @intCast(xx));
            if (index >= surface.pixel_capacity) continue;
            surface.pixels[index] = color;
        }
    }
}

fn lerpColor(a: u32, b: u32, num: i32, den: i32) u32 {
    if (den <= 0 or num <= 0) return a;
    if (num >= den) return b;

    const ua = @as(u32, @intCast(den - num));
    const ub = @as(u32, @intCast(num));
    const uden = @as(u32, @intCast(den));

    const ar = (a >> 16) & 0xFF;
    const ag = (a >> 8) & 0xFF;
    const ab = a & 0xFF;
    const br = (b >> 16) & 0xFF;
    const bg = (b >> 8) & 0xFF;
    const bb = b & 0xFF;

    const r = (ar * ua + br * ub + uden / 2) / uden;
    const g = (ag * ua + bg * ub + uden / 2) / uden;
    const blue = (ab * ua + bb * ub + uden / 2) / uden;
    return (r << 16) | (g << 8) | blue;
}

fn headerGradientColorAt(offset_y: i32, height: i32) u32 {
    if (height <= 1) return window_header_fill_color;
    return lerpColor(window_header_top_color, window_header_bottom_color, offset_y, height - 1);
}

fn drawWindowHeaderBackground(back: [*]u32, win: *const WindowState) void {
    const header_x = win.x + window_border_i32;
    const header_y = win.y + window_border_i32;
    const header_w = windowWidth(win) - window_border_i32 * 2;
    const header_h = window_header_h_i32 - window_border_i32;
    if (header_w <= 0 or header_h <= 0) return;

    var row: i32 = 0;
    while (row < header_h) : (row += 1) {
        drawSolidRect(back, header_x, header_y + row, header_w, 1, headerGradientColorAt(row, header_h));
    }

    if (header_h > 2 and header_w > 2) {
        drawSolidRect(back, header_x + 1, header_y, header_w - 2, 1, window_header_highlight_color);
        var x = header_x + 1;
        while (x < header_x + header_w - 1) : (x += 1) {
            blendBackPixel(back, x, header_y + 1, window_header_highlight_color, 72);
        }
    }
}

fn drawWindowHeaderBackgroundSurface(surface: *const DrawSurface, win: *const WindowState) void {
    const header_x = win.x + window_border_i32;
    const header_y = win.y + window_border_i32;
    const header_w = windowWidth(win) - window_border_i32 * 2;
    const header_h = window_header_h_i32 - window_border_i32;
    if (header_w <= 0 or header_h <= 0) return;
    const header_rect: Rect = .{
        .x0 = header_x,
        .y0 = header_y,
        .x1 = header_x + header_w,
        .y1 = header_y + header_h,
    };
    const clipped = intersectRect(header_rect, surface.clip) orelse return;

    var row = clipped.y0 - header_y;
    while (row < clipped.y1 - header_y) : (row += 1) {
        drawSolidRectSurface(surface, header_x, header_y + row, header_w, 1, headerGradientColorAt(row, header_h));
    }

    if (header_h > 2 and header_w > 2 and clipped.y0 <= header_y + 1 and clipped.y1 > header_y) {
        drawSolidRectSurface(surface, header_x + 1, header_y, header_w - 2, 1, window_header_highlight_color);
        const x0 = if (clipped.x0 > header_x + 1) clipped.x0 else header_x + 1;
        const x1 = if (clipped.x1 < header_x + header_w - 1) clipped.x1 else header_x + header_w - 1;
        var x = x0;
        while (x < x1) : (x += 1) {
            blendSurfacePixel(surface, x, header_y + 1, window_header_highlight_color, 72);
        }
    }
}

fn drawWindowHeaderDivider(back: [*]u32, win: *const WindowState) void {
    const x = contentOriginX(win);
    const y = contentOriginY(win) - 1;
    const w = windowContentW(win);
    if (w <= 0) return;
    drawSolidRect(back, x, y, w, 1, window_header_divider_soft_color);
    var xx = x;
    while (xx < x + w) : (xx += 1) {
        blendBackPixel(back, xx, y + 1, window_shadow_color, 18);
    }
}

fn drawWindowHeaderDividerSurface(surface: *const DrawSurface, win: *const WindowState) void {
    const x = contentOriginX(win);
    const y = contentOriginY(win) - 1;
    const w = windowContentW(win);
    if (w <= 0) return;
    if (intersectRect(.{ .x0 = x, .y0 = y, .x1 = x + w, .y1 = y + 2 }, surface.clip) == null) return;
    drawSolidRectSurface(surface, x, y, w, 1, window_header_divider_soft_color);
    const clipped = intersectRect(.{ .x0 = x, .y0 = y + 1, .x1 = x + w, .y1 = y + 2 }, surface.clip) orelse return;
    var xx = clipped.x0;
    while (xx < clipped.x1) : (xx += 1) {
        blendSurfacePixel(surface, xx, y + 1, window_shadow_color, 18);
    }
}

fn drawWindowInnerFrame(back: [*]u32, win: *const WindowState) void {
    const x = win.x + window_border_i32;
    const y = win.y + window_border_i32;
    const w = windowWidth(win) - window_border_i32 * 2;
    const h = windowHeight(win) - window_border_i32 * 2;
    const thickness: i32 = 2;
    if (w <= thickness * 2 or h <= thickness) return;

    drawSolidRect(back, x, y, thickness, h, window_inner_frame_color);
    drawSolidRect(back, x + w - thickness, y, thickness, h, window_inner_frame_color);
    drawSolidRect(back, x, y + h - thickness, w, thickness, window_inner_frame_color);
}

fn drawWindowInnerFrameSurface(surface: *const DrawSurface, win: *const WindowState) void {
    const x = win.x + window_border_i32;
    const y = win.y + window_border_i32;
    const w = windowWidth(win) - window_border_i32 * 2;
    const h = windowHeight(win) - window_border_i32 * 2;
    const thickness: i32 = 2;
    if (w <= thickness * 2 or h <= thickness) return;
    if (intersectRect(.{ .x0 = x, .y0 = y, .x1 = x + w, .y1 = y + h }, surface.clip) == null) return;

    drawSolidRectSurface(surface, x, y, thickness, h, window_inner_frame_color);
    drawSolidRectSurface(surface, x + w - thickness, y, thickness, h, window_inner_frame_color);
    drawSolidRectSurface(surface, x, y + h - thickness, w, thickness, window_inner_frame_color);
}

fn drawWindowShadow(back: [*]u32, win: *const WindowState) void {
    const x = win.x;
    const y = win.y;
    const w = windowWidth(win);
    const h = windowHeight(win);
    if (w <= 0 or h <= 0) return;

    var xx = x + 3;
    while (xx < x + w - 2) : (xx += 1) {
        blendBackPixel(back, xx, y + h, window_shadow_color, 24);
        blendBackPixel(back, xx, y + h + 1, window_shadow_color, 13);
        blendBackPixel(back, xx, y + h + 2, window_shadow_color, 6);
    }

    var yy = y + window_header_h_i32;
    while (yy < y + h) : (yy += 1) {
        blendBackPixel(back, x + w, yy, window_shadow_color, 12);
        blendBackPixel(back, x + w + 1, yy, window_shadow_color, 6);
    }
}

fn drawWindowShadowSurface(surface: *const DrawSurface, win: *const WindowState) void {
    const x = win.x;
    const y = win.y;
    const w = windowWidth(win);
    const h = windowHeight(win);
    if (w <= 0 or h <= 0) return;
    const shadow_rect: Rect = .{
        .x0 = x,
        .y0 = y,
        .x1 = x + w + window_shadow_right_extent,
        .y1 = y + h + window_shadow_bottom_extent,
    };
    if (intersectRect(shadow_rect, surface.clip) == null) return;

    const bottom_x0 = if (surface.clip.x0 > x + 3) surface.clip.x0 else x + 3;
    const bottom_x1 = if (surface.clip.x1 < x + w - 2) surface.clip.x1 else x + w - 2;
    var xx = bottom_x0;
    while (xx < bottom_x1) : (xx += 1) {
        blendSurfacePixel(surface, xx, y + h, window_shadow_color, 24);
        blendSurfacePixel(surface, xx, y + h + 1, window_shadow_color, 13);
        blendSurfacePixel(surface, xx, y + h + 2, window_shadow_color, 6);
    }

    const side_y0 = if (surface.clip.y0 > y + window_header_h_i32) surface.clip.y0 else y + window_header_h_i32;
    const side_y1 = if (surface.clip.y1 < y + h) surface.clip.y1 else y + h;
    var yy = side_y0;
    while (yy < side_y1) : (yy += 1) {
        blendSurfacePixel(surface, x + w, yy, window_shadow_color, 12);
        blendSurfacePixel(surface, x + w + 1, yy, window_shadow_color, 6);
    }
}

fn cursorRect(cx: i32, cy: i32) Rect {
    return .{
        .x0 = cx,
        .y0 = cy,
        .x1 = cx + cursor_width_i32,
        .y1 = cy + cursor_height_i32,
    };
}

fn drawCursorSurface(surface: *const DrawSurface, cx: i32, cy: i32) void {
    const fill_color: u32 = 0x00FF_FFFF;
    var bit_y: usize = 0;
    while (bit_y < cursor_height) : (bit_y += 1) {
        var bit_x: usize = 0;
        while (bit_x < cursor_width) : (bit_x += 1) {
            const ch = cursor_shape[bit_y][bit_x];
            if (ch == ' ') continue;
            const x = cx + @as(i32, @intCast(bit_x));
            const y = cy + @as(i32, @intCast(bit_y));
            setSurfacePixel(surface, x, y, if (ch == '@') 0x0000_0000 else fill_color);
        }
    }
}

fn initHardwareCursor(mouse_x: i32, mouse_y: i32) bool {
    const resource_handle = virtgpu.virtgpu_cursor_resource() orelse return false;
    const pixels = virtgpu.virtgpu_pixels(resource_handle) orelse return false;
    var i: usize = 0;
    while (i < cursor_dim * cursor_dim) : (i += 1) {
        pixels[i] = 0;
    }

    var bit_y: usize = 0;
    while (bit_y < cursor_height) : (bit_y += 1) {
        var bit_x: usize = 0;
        while (bit_x < cursor_width) : (bit_x += 1) {
            const ch = cursor_shape[bit_y][bit_x];
            if (ch == ' ') continue;
            const color: u32 = if (ch == '@') 0xFF00_0000 else 0xFFFF_FFFF;
            pixels[bit_y * cursor_dim + bit_x] = color;
        }
    }

    const full_rect: virtgpu.Rect = .{
        .x = 0,
        .y = 0,
        .width = cursor_dim,
        .height = cursor_dim,
    };
    if (!virtgpu.virtgpu_transfer(resource_handle, full_rect)) return false;
    if (!virtgpu.virtgpu_flush_rect(resource_handle, full_rect)) return false;
    return virtgpu.virtgpu_update_cursor(resource_handle, 0, 0, mouse_x, mouse_y);
}

fn clearBackBuffer(back: [*]u32, color: u32) void {
    var i: usize = 0;
    while (i < fb_pixels) : (i += 1) {
        back[i] = color;
    }
}

fn drawTextClipped(back: [*]u32, x: i32, y: i32, text: []const u8, color: u32, max_x: i32) void {
    font.drawUtf8TextClipped([*]u32, blendBackPixel, back, x, y, text, color, 1, max_x);
}

fn drawTextClippedSurface(surface: *const DrawSurface, x: i32, y: i32, text: []const u8, color: u32, max_x: i32) void {
    font.drawUtf8TextClipped(*const DrawSurface, blendSurfacePixel, surface, x, y, text, color, 1, max_x);
}

fn drawTextSubpixelClipped(back: [*]u32, x: i32, y: i32, text: []const u8, color: u32, max_x: i32) void {
    font.drawUtf8TextSubpixelClipped([*]u32, blendBackPixelSubpixel, back, x, y, text, color, 1, max_x);
}

fn drawTextSubpixelClippedSurface(surface: *const DrawSurface, x: i32, y: i32, text: []const u8, color: u32, max_x: i32) void {
    font.drawUtf8TextSubpixelClipped(*const DrawSurface, blendSurfacePixelSubpixel, surface, x, y, text, color, 1, max_x);
}

fn sanitizeTitleByte(raw: u8) u8 {
    if (raw < 0x20 or raw == 0x7F) return ' ';
    return raw;
}

fn textPixelWidth(text: []const u8) i32 {
    return font.measureUtf8Text(text, 1);
}

fn windowScale(win: *const WindowState) usize {
    return if (win.content_scale > 0) win.content_scale else default_window_scale;
}

fn windowScaleI32(win: *const WindowState) i32 {
    return @as(i32, @intCast(windowScale(win)));
}

fn windowContentW(win: *const WindowState) i32 {
    return @as(i32, @intCast(win.src.w * windowScale(win)));
}

fn windowContentH(win: *const WindowState) i32 {
    return @as(i32, @intCast(win.src.h * windowScale(win)));
}

fn windowHasFrame(win: *const WindowState) bool {
    return (win.flags & window_flag_frameless) == 0;
}

fn windowWidth(win: *const WindowState) i32 {
    if (!windowHasFrame(win)) return windowContentW(win);
    return windowContentW(win) + window_border_i32 * 2;
}

fn windowHeight(win: *const WindowState) i32 {
    if (!windowHasFrame(win)) return windowContentH(win);
    return window_header_h_i32 + windowContentH(win) + window_border_i32;
}

fn appendText(buf: []u8, index: *usize, text: []const u8) void {
    var i: usize = 0;
    while (i < text.len and index.* < buf.len) : (i += 1) {
        buf[index.*] = text[i];
        index.* += 1;
    }
}

fn appendBytesBounded(buf: []u8, index: *usize, text: []const u8) bool {
    if (index.* + text.len > buf.len) return false;
    appendText(buf, index, text);
    return true;
}

fn appendU32Decimal(buf: []u8, index: *usize, value: u32) void {
    if (index.* >= buf.len) return;
    if (value == 0) {
        buf[index.*] = '0';
        index.* += 1;
        return;
    }

    var tmp: [10]u8 = undefined;
    var n: usize = 0;
    var v = value;
    while (v > 0 and n < tmp.len) : (n += 1) {
        tmp[n] = @as(u8, @intCast('0' + (v % 10)));
        v /= 10;
    }
    while (n > 0 and index.* < buf.len) {
        n -= 1;
        buf[index.*] = tmp[n];
        index.* += 1;
    }
}

fn appendU64Decimal(buf: []u8, index: *usize, value: u64) void {
    if (index.* >= buf.len) return;
    if (value == 0) {
        buf[index.*] = '0';
        index.* += 1;
        return;
    }

    var tmp: [20]u8 = undefined;
    var n: usize = 0;
    var v = value;
    while (v > 0 and n < tmp.len) : (n += 1) {
        tmp[n] = @as(u8, @intCast('0' + (v % 10)));
        v /= 10;
    }
    while (n > 0 and index.* < buf.len) {
        n -= 1;
        buf[index.*] = tmp[n];
        index.* += 1;
    }
}

fn logComposePerf(compose_ticks: u64, present_submit_ticks: u64, present_rect: Rect) void {
    _ = compose_ticks;
    _ = present_submit_ticks;
    _ = present_rect;
}

fn resetCompositorPerfReport(now_tsc: u64) void {
    compositor_perf_report = .{ .start_tsc = now_tsc };
}

fn maybeLogCompositorPerf(now_tsc: u64) void {
    if (compositor_perf_report.start_tsc == 0) {
        resetCompositorPerfReport(now_tsc);
        return;
    }
    if (compositor_perf_report.total_tsc < compositor_perf_report_min_wall_tsc) return;
    var buf: [256]u8 = undefined;
    var idx: usize = 0;
    const frames = compositor_perf_report.frames;
    const presents = compositor_perf_report.presents;
    const avg_compose = if (frames != 0) compositor_perf_report.compose_tsc / frames else 0;
    const avg_submit = if (presents != 0) compositor_perf_report.submit_tsc / presents else 0;
    const avg_area = if (presents != 0) compositor_perf_report.present_area_sum / presents else 0;
    appendDiagText(buf[0..], &idx, "Compositor: perf frames=");
    appendDiagU64Decimal(buf[0..], &idx, frames);
    appendDiagText(buf[0..], &idx, " presents=");
    appendDiagU64Decimal(buf[0..], &idx, presents);
    appendDiagText(buf[0..], &idx, " avg_compose_tsc=");
    appendDiagU64Decimal(buf[0..], &idx, avg_compose);
    appendDiagText(buf[0..], &idx, " avg_submit_tsc=");
    appendDiagU64Decimal(buf[0..], &idx, avg_submit);
    appendDiagText(buf[0..], &idx, " idle=");
    appendDiagU64Decimal(buf[0..], &idx, compositor_perf_report.idle_loops);
    appendDiagText(buf[0..], &idx, " no_window=");
    appendDiagU64Decimal(buf[0..], &idx, compositor_perf_report.no_window_loops);
    appendDiagText(buf[0..], &idx, " full=");
    appendDiagU64Decimal(buf[0..], &idx, compositor_perf_report.full_frames);
    appendDiagText(buf[0..], &idx, " dirty=");
    appendDiagU64Decimal(buf[0..], &idx, compositor_perf_report.dirty_frames);
    appendDiagText(buf[0..], &idx, " avg_area=");
    appendDiagU64Decimal(buf[0..], &idx, avg_area);
    appendDiagText(buf[0..], &idx, " sync_tsc=");
    appendDiagU64Decimal(buf[0..], &idx, compositor_perf_report.sync_tsc);
    appendDiagText(buf[0..], &idx, "\n");
    _ = userLog(buf[0..idx]);
    resetCompositorPerfReport(now_tsc);
}

fn fitTextWithEllipsis(text: []const u8, max_width: i32, scratch: []u8) []const u8 {
    if (text.len == 0 or max_width <= 0) return "";
    if (textPixelWidth(text) <= max_width) return text;

    const ellipsis = "...";
    const ellipsis_width = textPixelWidth(ellipsis);
    if (ellipsis_width > max_width or scratch.len < ellipsis.len) return "";

    var src_index: usize = 0;
    var dst_index: usize = 0;
    var width: i32 = 0;
    while (src_index < text.len) {
        const cp_start = src_index;
        _ = font.decodeNextUtf8(text, &src_index);
        const cp_bytes = text[cp_start..src_index];
        const cp_width = textPixelWidth(cp_bytes);
        if (width + cp_width + ellipsis_width > max_width) break;
        if (!appendBytesBounded(scratch, &dst_index, cp_bytes)) break;
        width += cp_width;
    }

    if (dst_index == 0) return "";
    _ = appendBytesBounded(scratch, &dst_index, ellipsis);
    return scratch[0..dst_index];
}

fn refreshWindowTitleCache(slot_ref: *WindowSlot) void {
    slot_ref.title_cache_len = 0;
    slot_ref.title_draw_x_off = 8;
    slot_ref.title_clip_right_off = windowWidth(&slot_ref.frame) - window_close_margin_i32 - window_close_width_i32 - window_button_gap_i32 - window_minimize_width_i32 - 6;

    const title = slot_ref.frame.title[0..slot_ref.frame.title_len];
    if (title.len == 0) return;

    const title_x = 8;
    const title_clip_right = windowWidth(&slot_ref.frame) - window_close_margin_i32 - window_close_width_i32 - window_button_gap_i32 - window_minimize_width_i32 - 6;
    slot_ref.title_draw_x_off = title_x;
    slot_ref.title_clip_right_off = title_clip_right;
    if (title_x >= title_clip_right) return;

    var scratch: [window_title_max_bytes + 3]u8 = undefined;
    const fitted_title = fitTextWithEllipsis(title, title_clip_right - title_x, scratch[0..]);
    if (fitted_title.len == 0) return;
    @memcpy(slot_ref.title_cache[0..fitted_title.len], fitted_title);
    slot_ref.title_cache_len = fitted_title.len;
}

fn buildMouseCoordText(buf: *[32]u8, mouse_x: i32, mouse_y: i32) []const u8 {
    var idx: usize = 0;
    appendText(buf[0..], &idx, "MOUSE X ");
    appendU32Decimal(buf[0..], &idx, @as(u32, @intCast(if (mouse_x < 0) 0 else mouse_x)));
    appendText(buf[0..], &idx, " Y ");
    appendU32Decimal(buf[0..], &idx, @as(u32, @intCast(if (mouse_y < 0) 0 else mouse_y)));
    return buf[0..idx];
}

fn drawMouseCoordHud(back: [*]u32, mouse_x: i32, mouse_y: i32) void {
    var text_buf: [32]u8 = undefined;
    const text = buildMouseCoordText(&text_buf, mouse_x, mouse_y);
    const hud_x: i32 = 14;
    const hud_y: i32 = 14;
    const hud_w: i32 = 176;
    const hud_h = font.lineHeight(1) + 8;
    const text_y = hud_y + @divTrunc(hud_h - font.lineHeight(1), 2);
    drawSolidRect(back, hud_x, hud_y, hud_w, hud_h, 0x00F1_F0EF);
    drawTextClipped(back, hud_x + 5, text_y, text, 0x0030_3030, hud_x + hud_w - 4);
}

fn drawMouseCoordHudSurface(surface: *const DrawSurface, mouse_x: i32, mouse_y: i32) void {
    var text_buf: [32]u8 = undefined;
    const text = buildMouseCoordText(&text_buf, mouse_x, mouse_y);
    const hud_x: i32 = 14;
    const hud_y: i32 = 14;
    const hud_w: i32 = 176;
    const hud_h = font.lineHeight(1) + 8;
    const text_y = hud_y + @divTrunc(hud_h - font.lineHeight(1), 2);
    drawSolidRectSurface(surface, hud_x, hud_y, hud_w, hud_h, 0x00F1_F0EF);
    drawTextClippedSurface(surface, hud_x + 5, text_y, text, 0x0030_3030, hud_x + hud_w - 4);
}

fn windowTaskbarLabel(slot_ref: *const WindowSlot, max_width: i32, scratch: []u8) []const u8 {
    const raw_title = slot_ref.frame.title[0..slot_ref.frame.title_len];
    const title = if (raw_title.len == 0) "Window" else raw_title;
    return fitTextWithEllipsis(title, max_width, scratch);
}

fn insideRoundedRectI32(x: i32, y: i32, w: i32, h: i32, radius: i32, inset: i32) bool {
    if (w <= inset * 2 or h <= inset * 2) return false;
    const left = inset;
    const top = inset;
    const right = w - inset;
    const bottom = h - inset;
    if (x < left or x >= right or y < top or y >= bottom) return false;

    const r = radius - inset;
    if (r <= 0) return true;

    const inner_left = left + r;
    const inner_right = right - r;
    const inner_top = top + r;
    const inner_bottom = bottom - r;
    if ((x >= inner_left and x < inner_right) or (y >= inner_top and y < inner_bottom)) return true;

    var cx = inner_left;
    if (x >= inner_right) cx = inner_right - 1;
    var cy = inner_top;
    if (y >= inner_bottom) cy = inner_bottom - 1;

    const dx = x - cx;
    const dy = y - cy;
    const rr = r - 1;
    return dx * dx + dy * dy <= rr * rr;
}

const TaskbarProcessStyle = struct {
    bg: u32,
    grad_start: u32,
    border: u32,
    border_left_bottom: u32,
    inner_highlight: u32,
    text: u32,
};

fn taskbarProcessStyle(hidden: bool) TaskbarProcessStyle {
    if (!hidden) {
        return .{
            .bg = taskbar_process_bg,
            .grad_start = taskbar_process_grad_start,
            .border = taskbar_process_border,
            .border_left_bottom = taskbar_process_border_left_bottom,
            .inner_highlight = taskbar_process_inner_highlight,
            .text = taskbar_process_text,
        };
    }
    return .{
        .bg = font.blendColor(taskbar_process_bg, 0x0000_0000, 36),
        .grad_start = font.blendColor(taskbar_process_grad_start, 0x0000_0000, 48),
        .border = taskbar_process_border,
        .border_left_bottom = font.blendColor(taskbar_process_border_left_bottom, 0x0000_0000, 28),
        .inner_highlight = font.blendColor(taskbar_process_inner_highlight, 0x0000_0000, 22),
        .text = taskbar_process_hidden_text,
    };
}

fn drawTaskbarProcessHoverGlow(back: [*]u32, rect: Rect) void {
    const cx = @divTrunc(rect.x0 + rect.x1, 2);
    const rx = @max(18, @divTrunc(rect.x1 - rect.x0, 3));
    const ry = @max(8, @divTrunc(rect.y1 - rect.y0, 3));
    const cy = rect.y1 + @divTrunc(ry, 2);
    const rx2 = rx * rx;
    const ry2 = ry * ry;
    const limit = rx2 * ry2;

    var y = rect.y1 - ry;
    while (y < rect.y1 + ry) : (y += 1) {
        var x = cx - rx;
        while (x < cx + rx) : (x += 1) {
            const dx = x - cx;
            const dy = y - cy;
            const dist = dx * dx * ry2 + dy * dy * rx2;
            if (dist >= limit) continue;
            const alpha = @as(u8, @intCast(@min(72, @divFloor((limit - dist) * 72, limit))));
            if (alpha == 0) continue;
            blendBackPixel(back, x, y, taskbar_process_hover_glow_color, alpha);
        }
    }
}

fn drawTaskbarProcessHoverGlowSurface(surface: *const DrawSurface, rect: Rect) void {
    const cx = @divTrunc(rect.x0 + rect.x1, 2);
    const rx = @max(18, @divTrunc(rect.x1 - rect.x0, 3));
    const ry = @max(8, @divTrunc(rect.y1 - rect.y0, 3));
    const cy = rect.y1 + @divTrunc(ry, 2);
    const rx2 = rx * rx;
    const ry2 = ry * ry;
    const limit = rx2 * ry2;

    var y = rect.y1 - ry;
    while (y < rect.y1 + ry) : (y += 1) {
        var x = cx - rx;
        while (x < cx + rx) : (x += 1) {
            const dx = x - cx;
            const dy = y - cy;
            const dist = dx * dx * ry2 + dy * dy * rx2;
            if (dist >= limit) continue;
            const alpha = @as(u8, @intCast(@min(72, @divFloor((limit - dist) * 72, limit))));
            if (alpha == 0) continue;
            blendSurfacePixel(surface, x, y, taskbar_process_hover_glow_color, alpha);
        }
    }
}

fn drawTaskbarProcessButton(back: [*]u32, rect: Rect, title: []const u8, hidden: bool, hovered: bool) void {
    const style = taskbarProcessStyle(hidden);
    const w = rect.x1 - rect.x0;
    const h = rect.y1 - rect.y0;
    if (w <= 0 or h <= 0) return;

    if (hovered) drawTaskbarProcessHoverGlow(back, rect);

    var yy: i32 = 0;
    while (yy < h) : (yy += 1) {
        var xx: i32 = 0;
        while (xx < w) : (xx += 1) {
            if (!insideRoundedRectI32(xx, yy, w, h, taskbar_process_radius, 0)) continue;
            const color = if (xx < taskbar_process_gradient_width)
                lerpColor(style.grad_start, style.bg, @min(xx + yy, taskbar_process_gradient_width - 1), taskbar_process_gradient_width - 1)
            else
                style.bg;
            setBackPixel(back, rect.x0 + xx, rect.y0 + yy, color);
        }
    }

    yy = 0;
    while (yy < h) : (yy += 1) {
        var xx: i32 = 0;
        while (xx < w) : (xx += 1) {
            if (!insideRoundedRectI32(xx, yy, w, h, taskbar_process_radius, 0)) continue;
            const x = rect.x0 + xx;
            const y = rect.y0 + yy;
            if (!insideRoundedRectI32(xx, yy, w, h, taskbar_process_radius, 1)) {
                setBackPixel(back, x, y, if (xx == 0)
                    lerpColor(style.border, style.border_left_bottom, yy, h - 1)
                else
                    style.border);
            } else if (!insideRoundedRectI32(xx, yy, w, h, taskbar_process_radius, 2)) {
                setBackPixel(back, x, y, style.inner_highlight);
            }
        }
    }

    const text_y = rect.y0 + @divTrunc(h - font.lineHeight(1), 2);
    drawTextClipped(back, rect.x0 + 14, text_y, title, style.text, rect.x1 - 8);
}

fn drawTaskbarProcessButtonSurface(surface: *const DrawSurface, rect: Rect, title: []const u8, hidden: bool, hovered: bool) void {
    const style = taskbarProcessStyle(hidden);
    const w = rect.x1 - rect.x0;
    const h = rect.y1 - rect.y0;
    if (w <= 0 or h <= 0) return;

    if (hovered) drawTaskbarProcessHoverGlowSurface(surface, rect);

    var yy: i32 = 0;
    while (yy < h) : (yy += 1) {
        var xx: i32 = 0;
        while (xx < w) : (xx += 1) {
            if (!insideRoundedRectI32(xx, yy, w, h, taskbar_process_radius, 0)) continue;
            const color = if (xx < taskbar_process_gradient_width)
                lerpColor(style.grad_start, style.bg, @min(xx + yy, taskbar_process_gradient_width - 1), taskbar_process_gradient_width - 1)
            else
                style.bg;
            setSurfacePixel(surface, rect.x0 + xx, rect.y0 + yy, color);
        }
    }

    yy = 0;
    while (yy < h) : (yy += 1) {
        var xx: i32 = 0;
        while (xx < w) : (xx += 1) {
            if (!insideRoundedRectI32(xx, yy, w, h, taskbar_process_radius, 0)) continue;
            const x = rect.x0 + xx;
            const y = rect.y0 + yy;
            if (!insideRoundedRectI32(xx, yy, w, h, taskbar_process_radius, 1)) {
                setSurfacePixel(surface, x, y, if (xx == 0)
                    lerpColor(style.border, style.border_left_bottom, yy, h - 1)
                else
                    style.border);
            } else if (!insideRoundedRectI32(xx, yy, w, h, taskbar_process_radius, 2)) {
                setSurfacePixel(surface, x, y, style.inner_highlight);
            }
        }
    }

    const text_y = rect.y0 + @divTrunc(h - font.lineHeight(1), 2);
    drawTextClippedSurface(surface, rect.x0 + 14, text_y, title, style.text, rect.x1 - 8);
}

fn drawTaskbarProcessList(back: [*]u32, store: *const WindowStore, hovered_slot: ?usize) void {
    const taskbar_slot = findTaskbarVisibleSlot() orelse return;
    const taskbar_rect = taskbarRectForSlot(&store.slots[taskbar_slot]);
    var slots: [max_windows]usize = undefined;
    const count = collectTaskbarProcessSlots(&slots);
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const button_rect = taskbarProcessButtonRect(taskbar_rect, i, count) orelse continue;
        const slot_ref = &store.slots[slots[i]];
        var label_buf: [window_title_max_bytes + 3]u8 = undefined;
        const label = windowTaskbarLabel(slot_ref, button_rect.x1 - button_rect.x0 - 18, label_buf[0..]);
        drawTaskbarProcessButton(back, button_rect, label, !slot_ref.frame.visible, hovered_slot != null and hovered_slot.? == slots[i]);
    }
}

fn drawTaskbarProcessListSurface(surface: *const DrawSurface, store: *const WindowStore, hovered_slot: ?usize) void {
    const taskbar_slot = findTaskbarVisibleSlot() orelse return;
    const taskbar_rect = taskbarRectForSlot(&store.slots[taskbar_slot]);
    if (intersectRect(taskbar_rect, surface.clip) == null) return;

    var slots: [max_windows]usize = undefined;
    const count = collectTaskbarProcessSlots(&slots);
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const button_rect = taskbarProcessButtonRect(taskbar_rect, i, count) orelse continue;
        if (intersectRect(button_rect, surface.clip) == null) continue;
        const slot_ref = &store.slots[slots[i]];
        var label_buf: [window_title_max_bytes + 3]u8 = undefined;
        const label = windowTaskbarLabel(slot_ref, button_rect.x1 - button_rect.x0 - 18, label_buf[0..]);
        drawTaskbarProcessButtonSurface(surface, button_rect, label, !slot_ref.frame.visible, hovered_slot != null and hovered_slot.? == slots[i]);
    }
}

fn clampWindowX(win: *const WindowState, x: i32) i32 {
    return clampI32(x, 0, fb_width_i32 - windowWidth(win));
}

fn clampWindowY(win: *const WindowState, y: i32) i32 {
    return clampI32(y, 0, fb_height_i32 - windowHeight(win));
}

fn contentOriginX(win: *const WindowState) i32 {
    return if (windowHasFrame(win)) win.x + window_border_i32 else win.x;
}

fn contentOriginY(win: *const WindowState) i32 {
    return if (windowHasFrame(win)) win.y + window_header_h_i32 else win.y;
}

fn pointInCircle(px: i32, py: i32, cx: i32, cy: i32, r: i32) bool {
    const dx = px - cx;
    const dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}

fn pointInCloseButton(px: i32, py: i32, rect: Rect) bool {
    return pointInRect(px, py, rect);
}

fn pointInMinimizeButton(px: i32, py: i32, rect: Rect) bool {
    return pointInRect(px, py, rect);
}

fn windowCloseButtonRect(win: *const WindowState) Rect {
    const close_right = win.x + windowWidth(win) - window_close_margin_i32;
    const close_left = close_right - window_close_width_i32;
    const close_top = win.y + @divTrunc(window_header_h_i32 - window_close_height_i32, 2);
    return .{
        .x0 = close_left,
        .y0 = close_top,
        .x1 = close_right,
        .y1 = close_top + window_close_height_i32,
    };
}

fn windowMinimizeButtonRect(win: *const WindowState) Rect {
    const close_rect = windowCloseButtonRect(win);
    const minimize_right = close_rect.x0 - window_button_gap_i32;
    const minimize_left = minimize_right - window_minimize_width_i32;
    const minimize_top = win.y + @divTrunc(window_header_h_i32 - window_minimize_height_i32, 2);
    return .{
        .x0 = minimize_left,
        .y0 = minimize_top,
        .x1 = minimize_right,
        .y1 = minimize_top + window_minimize_height_i32,
    };
}

fn drawMinimizeButton(back: [*]u32, rect: Rect, hover: bool, down: bool) void {
    const top_color = if (hover) font.blendColor(minimize_btn_top_color, 0x00FF_FFFF, 24) else if (down) font.blendColor(minimize_btn_top_color, 0x0000_0000, 18) else minimize_btn_top_color;
    const bottom_color = if (hover) font.blendColor(minimize_btn_bottom_color, 0x00FF_FFFF, 18) else if (down) font.blendColor(minimize_btn_bottom_color, 0x0000_0000, 18) else minimize_btn_bottom_color;
    const line_y = rect.y0 + @divTrunc(rect.y1 - rect.y0, 2) + 1;

    var y = rect.y0;
    while (y < rect.y1) : (y += 1) {
        const bg = if (y - rect.y0 < @divTrunc(rect.y1 - rect.y0, 2)) top_color else bottom_color;
        var x = rect.x0;
        while (x < rect.x1) : (x += 1) {
            const is_border = y == rect.y0 or y == rect.y1 - 1 or x == rect.x0 or x == rect.x1 - 1;
            setBackPixel(back, x, y, if (is_border) minimize_btn_border_color else bg);
        }
    }

    var x = rect.x0 + 5;
    while (x < rect.x1 - 5) : (x += 1) {
        setBackPixel(back, x, line_y, minimize_btn_symbol_color);
        setBackPixel(back, x, line_y + 1, minimize_btn_symbol_color);
    }
}

fn drawMinimizeButtonSurface(surface: *const DrawSurface, rect: Rect, hover: bool, down: bool) void {
    const top_color = if (hover) font.blendColor(minimize_btn_top_color, 0x00FF_FFFF, 24) else if (down) font.blendColor(minimize_btn_top_color, 0x0000_0000, 18) else minimize_btn_top_color;
    const bottom_color = if (hover) font.blendColor(minimize_btn_bottom_color, 0x00FF_FFFF, 18) else if (down) font.blendColor(minimize_btn_bottom_color, 0x0000_0000, 18) else minimize_btn_bottom_color;
    const line_y = rect.y0 + @divTrunc(rect.y1 - rect.y0, 2) + 1;

    var y = rect.y0;
    while (y < rect.y1) : (y += 1) {
        const bg = if (y - rect.y0 < @divTrunc(rect.y1 - rect.y0, 2)) top_color else bottom_color;
        var x = rect.x0;
        while (x < rect.x1) : (x += 1) {
            const is_border = y == rect.y0 or y == rect.y1 - 1 or x == rect.x0 or x == rect.x1 - 1;
            setSurfacePixel(surface, x, y, if (is_border) minimize_btn_border_color else bg);
        }
    }

    var x = rect.x0 + 5;
    while (x < rect.x1 - 5) : (x += 1) {
        setSurfacePixel(surface, x, line_y, minimize_btn_symbol_color);
        setSurfacePixel(surface, x, line_y + 1, minimize_btn_symbol_color);
    }
}

fn drawCloseButton(back: [*]u32, rect: Rect, hover: bool, down: bool) void {
    const top_color = if (hover) font.blendColor(close_btn_top_color, 0x00FF_FFFF, 24) else if (down) font.blendColor(close_btn_top_color, 0x0000_0000, 18) else close_btn_top_color;
    const bottom_color = if (hover) font.blendColor(close_btn_bottom_color, 0x00FF_FFFF, 18) else if (down) font.blendColor(close_btn_bottom_color, 0x0000_0000, 18) else close_btn_bottom_color;
    const cx = @divTrunc(rect.x0 + rect.x1 - 1, 2);
    const cy = @divTrunc(rect.y0 + rect.y1 - 1, 2);

    var y = rect.y0;
    while (y < rect.y1) : (y += 1) {
        const bg = if (y - rect.y0 < @divTrunc(rect.y1 - rect.y0, 2)) top_color else bottom_color;
        var x = rect.x0;
        while (x < rect.x1) : (x += 1) {
            const is_border = y == rect.y0 or y == rect.y1 - 1 or x == rect.x0 or x == rect.x1 - 1;
            setBackPixel(back, x, y, if (is_border) close_btn_border_color else bg);
        }
    }

    var i: i32 = -2;
    while (i <= 2) : (i += 1) {
        setBackPixel(back, cx + i, cy + i, close_btn_cross_color);
        setBackPixel(back, cx + i, cy + i + 1, close_btn_cross_color);
        setBackPixel(back, cx - i, cy + i, close_btn_cross_color);
        setBackPixel(back, cx - i, cy + i + 1, close_btn_cross_color);
    }
}

fn drawCloseButtonSurface(surface: *const DrawSurface, rect: Rect, hover: bool, down: bool) void {
    const top_color = if (hover) font.blendColor(close_btn_top_color, 0x00FF_FFFF, 24) else if (down) font.blendColor(close_btn_top_color, 0x0000_0000, 18) else close_btn_top_color;
    const bottom_color = if (hover) font.blendColor(close_btn_bottom_color, 0x00FF_FFFF, 18) else if (down) font.blendColor(close_btn_bottom_color, 0x0000_0000, 18) else close_btn_bottom_color;
    const cx = @divTrunc(rect.x0 + rect.x1 - 1, 2);
    const cy = @divTrunc(rect.y0 + rect.y1 - 1, 2);

    var y = rect.y0;
    while (y < rect.y1) : (y += 1) {
        const bg = if (y - rect.y0 < @divTrunc(rect.y1 - rect.y0, 2)) top_color else bottom_color;
        var x = rect.x0;
        while (x < rect.x1) : (x += 1) {
            const is_border = y == rect.y0 or y == rect.y1 - 1 or x == rect.x0 or x == rect.x1 - 1;
            setSurfacePixel(surface, x, y, if (is_border) close_btn_border_color else bg);
        }
    }

    var i: i32 = -2;
    while (i <= 2) : (i += 1) {
        setSurfacePixel(surface, cx + i, cy + i, close_btn_cross_color);
        setSurfacePixel(surface, cx + i, cy + i + 1, close_btn_cross_color);
        setSurfacePixel(surface, cx - i, cy + i, close_btn_cross_color);
        setSurfacePixel(surface, cx - i, cy + i + 1, close_btn_cross_color);
    }
}

fn applyRoundedCornerCut(back: [*]u32, win: *const WindowState) void {
    if (window_corner_cut == 0) return;
    const w = windowWidth(win);
    const x = win.x;
    const y = win.y;
    const radius: i32 = @intCast(window_corner_cut);
    const border_width = window_border_i32;
    if (w <= radius * 2 or window_header_h_i32 <= radius or radius <= border_width) return;

    const outer_diameter = radius * 2 - 1;
    const inner_radius = radius - border_width;
    const inner_diameter = inner_radius * 2 - 1;
    const outer_limit = outer_diameter * outer_diameter;
    const inner_limit = inner_diameter * inner_diameter;

    var py: i32 = 0;
    while (py < radius) : (py += 1) {
        var px: i32 = 0;
        while (px < radius) : (px += 1) {
            // Evaluate against pixel centers for a smoother quarter-circle.
            const lx = (radius - 1 - px) * 2 + 1;
            const ly = (radius - 1 - py) * 2 + 1;
            const d2 = lx * lx + ly * ly;

            const inner_color = headerGradientColorAt(py - window_border_i32, window_header_h_i32 - window_border_i32);
            const left_color = if (d2 >= outer_limit)
                background_color
            else if (d2 >= inner_limit)
                window_border_color
            else if (py == window_border_i32)
                window_header_highlight_color
            else if (py == window_border_i32 + 1)
                font.blendColor(inner_color, window_header_highlight_color, 88)
            else
                inner_color;
            const right_color = left_color;

            setBackPixel(back, x + px, y + py, left_color);
            setBackPixel(back, x + w - 1 - px, y + py, right_color);
        }
    }
}

fn applyRoundedCornerCutSurface(surface: *const DrawSurface, win: *const WindowState) void {
    if (window_corner_cut == 0) return;
    const w = windowWidth(win);
    const x = win.x;
    const y = win.y;
    const radius: i32 = @intCast(window_corner_cut);
    const border_width = window_border_i32;
    if (w <= radius * 2 or window_header_h_i32 <= radius or radius <= border_width) return;

    const outer_diameter = radius * 2 - 1;
    const inner_radius = radius - border_width;
    const inner_diameter = inner_radius * 2 - 1;
    const outer_limit = outer_diameter * outer_diameter;
    const inner_limit = inner_diameter * inner_diameter;

    var py: i32 = 0;
    while (py < radius) : (py += 1) {
        var px: i32 = 0;
        while (px < radius) : (px += 1) {
            const lx = (radius - 1 - px) * 2 + 1;
            const ly = (radius - 1 - py) * 2 + 1;
            const d2 = lx * lx + ly * ly;

            const inner_color = headerGradientColorAt(py - window_border_i32, window_header_h_i32 - window_border_i32);
            const left_color = if (d2 >= outer_limit)
                background_color
            else if (d2 >= inner_limit)
                window_border_color
            else if (py == window_border_i32)
                window_header_highlight_color
            else if (py == window_border_i32 + 1)
                font.blendColor(inner_color, window_header_highlight_color, 88)
            else
                inner_color;

            setSurfacePixel(surface, x + px, y + py, left_color);
            setSurfacePixel(surface, x + w - 1 - px, y + py, left_color);
        }
    }
}

fn drawWindowChrome(
    back: [*]u32,
    slot_ref: *const WindowSlot,
) void {
    const win = &slot_ref.frame;
    if (!windowHasFrame(win)) return;
    drawWindowShadow(back, win);
    drawSolidRect(back, win.x, win.y, windowWidth(win), windowHeight(win), window_border_color);
    drawWindowHeaderBackground(back, win);
    drawWindowInnerFrame(back, win);
    drawSolidRect(back, contentOriginX(win), contentOriginY(win), windowContentW(win), windowContentH(win), window_content_bg_color);
    drawWindowHeaderDivider(back, win);

    const minimize_rect = windowMinimizeButtonRect(win);
    const close_rect = windowCloseButtonRect(win);
    const title_x = win.x + slot_ref.title_draw_x_off;
    const title_clip_right = win.x + slot_ref.title_clip_right_off;
    const title_y = win.y + @divTrunc(window_header_h_i32 - font.lineHeight(1), 2) + window_title_y_bias;
    const fitted_title = slot_ref.title_cache[0..slot_ref.title_cache_len];
    if (fitted_title.len > 0 and title_x < title_clip_right) {
        drawTextSubpixelClipped(back, title_x, title_y, fitted_title, window_header_text_color, title_clip_right);
        drawTextSubpixelClipped(back, title_x + 1, title_y, fitted_title, window_header_text_color, title_clip_right);
    }
    drawMinimizeButton(back, minimize_rect, slot_ref.minimize_hover, slot_ref.minimize_down);
    drawCloseButton(back, close_rect, slot_ref.close_hover, slot_ref.close_down);
    applyRoundedCornerCut(back, win);
}

fn drawWindowChromeSurface(
    surface: *const DrawSurface,
    slot_ref: *const WindowSlot,
) void {
    const win = &slot_ref.frame;
    if (!windowHasFrame(win)) return;
    const bounds = windowBounds(win);
    const clipped = intersectRect(bounds, surface.clip) orelse return;
    _ = clipped;
    const frame_rect: Rect = .{
        .x0 = win.x,
        .y0 = win.y,
        .x1 = win.x + windowWidth(win),
        .y1 = win.y + windowHeight(win),
    };
    const content_rect: Rect = .{
        .x0 = contentOriginX(win),
        .y0 = contentOriginY(win),
        .x1 = contentOriginX(win) + windowContentW(win),
        .y1 = contentOriginY(win) + windowContentH(win),
    };
    if (rectContainsRect(content_rect, surface.clip)) return;
    const header_rect: Rect = .{
        .x0 = win.x + window_border_i32,
        .y0 = win.y + window_border_i32,
        .x1 = win.x + windowWidth(win) - window_border_i32,
        .y1 = win.y + window_header_h_i32,
    };

    drawWindowShadowSurface(surface, win);
    if (intersectRect(frame_rect, surface.clip) != null) {
        drawSolidRectSurface(surface, win.x, win.y, windowWidth(win), windowHeight(win), window_border_color);
        drawWindowInnerFrameSurface(surface, win);
    }
    if (intersectRect(header_rect, surface.clip) != null) {
        drawWindowHeaderBackgroundSurface(surface, win);
        drawWindowHeaderDividerSurface(surface, win);
    }
    if (intersectRect(content_rect, surface.clip) != null) {
        drawSolidRectSurface(surface, contentOriginX(win), contentOriginY(win), windowContentW(win), windowContentH(win), window_content_bg_color);
    }

    const minimize_rect = windowMinimizeButtonRect(win);
    const close_rect = windowCloseButtonRect(win);
    const title_x = win.x + slot_ref.title_draw_x_off;
    const title_clip_right = win.x + slot_ref.title_clip_right_off;
    const title_y = win.y + @divTrunc(window_header_h_i32 - font.lineHeight(1), 2) + window_title_y_bias;
    const fitted_title = slot_ref.title_cache[0..slot_ref.title_cache_len];
    const title_rect: Rect = .{
        .x0 = title_x,
        .y0 = title_y,
        .x1 = title_clip_right,
        .y1 = title_y + font.lineHeight(1),
    };
    if (fitted_title.len > 0 and title_x < title_clip_right and intersectRect(title_rect, surface.clip) != null) {
        drawTextSubpixelClippedSurface(surface, title_x, title_y, fitted_title, window_header_text_color, title_clip_right);
        drawTextSubpixelClippedSurface(surface, title_x + 1, title_y, fitted_title, window_header_text_color, title_clip_right);
    }
    if (intersectRect(minimize_rect, surface.clip) != null) {
        drawMinimizeButtonSurface(surface, minimize_rect, slot_ref.minimize_hover, slot_ref.minimize_down);
    }
    if (intersectRect(close_rect, surface.clip) != null) {
        drawCloseButtonSurface(surface, close_rect, slot_ref.close_hover, slot_ref.close_down);
    }
    if (surface.clip.y0 < win.y + @as(i32, @intCast(window_corner_cut)) and surface.clip.y1 > win.y) {
        applyRoundedCornerCutSurface(surface, win);
    }
}

fn blitScaledPixelToWindow(back: [*]u32, win: *const WindowState, local_vx: usize, local_vy: usize, color: u32) Rect {
    const scale = windowScale(win);
    const scale_i32 = windowScaleI32(win);
    const x0 = contentOriginX(win) + @as(i32, @intCast(local_vx * scale));
    const y0 = contentOriginY(win) + @as(i32, @intCast(local_vy * scale));
    var sy: i32 = 0;
    while (sy < scale_i32) : (sy += 1) {
        var sx: i32 = 0;
        while (sx < scale_i32) : (sx += 1) {
            setBackPixel(back, x0 + sx, y0 + sy, color);
        }
    }
    return .{ .x0 = x0, .y0 = y0, .x1 = x0 + scale_i32, .y1 = y0 + scale_i32 };
}

fn blitScaledPixelToWindowSurface(surface: *const DrawSurface, win: *const WindowState, local_vx: usize, local_vy: usize, color: u32) Rect {
    const scale = windowScale(win);
    const scale_i32 = windowScaleI32(win);
    const x0 = contentOriginX(win) + @as(i32, @intCast(local_vx * scale));
    const y0 = contentOriginY(win) + @as(i32, @intCast(local_vy * scale));
    var sy: i32 = 0;
    while (sy < scale_i32) : (sy += 1) {
        var sx: i32 = 0;
        while (sx < scale_i32) : (sx += 1) {
            setSurfacePixel(surface, x0 + sx, y0 + sy, color);
        }
    }
    return .{ .x0 = x0, .y0 = y0, .x1 = x0 + scale_i32, .y1 = y0 + scale_i32 };
}

fn blitShadowRegionToWindow(back: [*]u32, shadow: [*]u32, win: *const WindowState) void {
    var vy: usize = 0;
    while (vy < win.src.h) : (vy += 1) {
        var vx: usize = 0;
        while (vx < win.src.w) : (vx += 1) {
            const src_index = (win.src.y + vy) * pixel_pitch + win.src.x + vx;
            _ = blitScaledPixelToWindow(back, win, vx, vy, shadow[src_index]);
        }
    }
}

fn isContentPixel(color: u32) bool {
    return color != 0 and color != background_color;
}

fn detectSourceRegions(shadow: [*]u32, out_regions: *[max_windows]SourceRegion) usize {
    var active_col: [pixel_width]bool = [_]bool{false} ** pixel_width;

    var x: usize = 0;
    while (x < pixel_width) : (x += 1) {
        var y: usize = 0;
        while (y < pixel_height) : (y += 1) {
            if (isContentPixel(shadow[y * pixel_pitch + x])) {
                active_col[x] = true;
                break;
            }
        }
    }

    var count: usize = 0;
    x = 0;
    while (x < pixel_width and count < max_windows) {
        if (!active_col[x]) {
            x += 1;
            continue;
        }

        const x_start = x;
        while (x < pixel_width and active_col[x]) : (x += 1) {}
        const x_end = x;

        var found = false;
        var y_min: usize = pixel_height;
        var y_max: usize = 0;

        var yy: usize = 0;
        while (yy < pixel_height) : (yy += 1) {
            var xx: usize = x_start;
            while (xx < x_end) : (xx += 1) {
                if (!isContentPixel(shadow[yy * pixel_pitch + xx])) continue;
                found = true;
                if (yy < y_min) y_min = yy;
                if (yy > y_max) y_max = yy;
            }
        }
        if (!found) continue;

        const rx0 = if (x_start > 0) x_start - 1 else x_start;
        const rx1 = if (x_end < pixel_width) x_end + 1 else x_end;
        const ry0 = if (y_min > 0) y_min - 1 else y_min;
        const y_hi = y_max + 2;
        const ry1 = if (y_hi < pixel_height) y_hi else pixel_height;
        if (rx1 <= rx0 or ry1 <= ry0) continue;

        out_regions[count] = .{ .x = rx0, .y = ry0, .w = rx1 - rx0, .h = ry1 - ry0 };
        count += 1;
    }

    return count;
}

fn redrawScene(
    back: [*]u32,
    store: *const WindowStore,
    mouse_x: i32,
    mouse_y: i32,
) void {
    clearBackBuffer(back, background_color);

    const order = currentWindowPaintOrder(store);
    var i: usize = 0;
    while (i < order.len) : (i += 1) {
        const slot_index = order[i];
        const slot = &store.slots[slot_index];
        const win = &slot.frame;
        if (!win.active or !win.visible) continue;
        drawWindowChrome(back, slot);
        blitWindowSourceToWindow(back, slot_index);
    }
    drawMouseCoordHud(back, mouse_x, mouse_y);
}

fn recomputeWindowCount() usize {
    return window_store.visibleCount();
}

fn isTaskbarWindowSlot(slot_ref: *const WindowSlot) bool {
    if (!slot_ref.source.active or !slot_ref.frame.active) return false;
    if (windowHasFrame(&slot_ref.frame)) return false;
    return slot_ref.frame.x == 0 and
        windowWidth(&slot_ref.frame) == fb_width_i32 and
        windowHeight(&slot_ref.frame) == taskbar_window_height and
        slot_ref.frame.y >= fb_height_i32 - taskbar_window_height;
}

fn taskbarRectForSlot(slot_ref: *const WindowSlot) Rect {
    const win = &slot_ref.frame;
    return clipRectToScreen(.{
        .x0 = win.x,
        .y0 = win.y,
        .x1 = win.x + windowWidth(win),
        .y1 = win.y + windowHeight(win),
    });
}

fn findTaskbarVisibleSlot() ?usize {
    var i: usize = 0;
    while (i < max_windows) : (i += 1) {
        const slot_ref = &window_store.slots[i];
        if (!slot_ref.frame.visible) continue;
        if (isTaskbarWindowSlot(slot_ref)) return i;
    }
    return null;
}

fn collectTaskbarProcessSlots(out: *[max_windows]usize) usize {
    var count: usize = 0;
    var i: usize = 0;
    while (i < max_windows) : (i += 1) {
        const slot_ref = &window_store.slots[i];
        if (!slot_ref.source.active or !slot_ref.frame.active) continue;
        if (!windowHasFrame(&slot_ref.frame)) continue;
        out[count] = i;
        count += 1;
    }

    var scan: usize = 1;
    while (scan < count) : (scan += 1) {
        const idx = out[scan];
        const z = window_store.slots[idx].z_order;
        var pos = scan;
        while (pos > 0 and window_store.slots[out[pos - 1]].z_order > z) : (pos -= 1) {
            out[pos] = out[pos - 1];
        }
        out[pos] = idx;
    }
    return count;
}

fn taskbarProcessButtonRect(taskbar_rect: Rect, button_index: usize, button_count: usize) ?Rect {
    if (button_count == 0) return null;
    const usable_w = taskbar_rect.x1 - taskbar_rect.x0 - taskbar_process_left_pad - taskbar_process_right_reserved;
    const usable_h = taskbar_rect.y1 - taskbar_rect.y0 - taskbar_process_top_pad * 2;
    if (usable_w <= 0 or usable_h <= 0) return null;

    const gap_total = taskbar_process_button_gap * @as(i32, @intCast(button_count - 1));
    const max_per_button = @divFloor(usable_w - gap_total, @as(i32, @intCast(button_count)));
    if (max_per_button <= 0) return null;
    const button_w = if (max_per_button < taskbar_process_button_max_width) max_per_button else taskbar_process_button_max_width;
    const step = button_w + taskbar_process_button_gap;
    const x0 = taskbar_rect.x0 + taskbar_process_left_pad + @as(i32, @intCast(button_index)) * step;
    const x1 = x0 + button_w;
    const limit_x = taskbar_rect.x1 - taskbar_process_right_reserved;
    if (x0 >= limit_x) return null;

    return .{
        .x0 = x0,
        .y0 = taskbar_rect.y0 + taskbar_process_top_pad,
        .x1 = if (x1 < limit_x) x1 else limit_x,
        .y1 = taskbar_rect.y1 - taskbar_process_top_pad,
    };
}

fn taskbarProcessListRect() ?Rect {
    const taskbar_slot = findTaskbarVisibleSlot() orelse return null;
    var slots: [max_windows]usize = undefined;
    const count = collectTaskbarProcessSlots(&slots);
    if (count == 0) return taskbarRectForSlot(&window_store.slots[taskbar_slot]);

    const taskbar_rect = taskbarRectForSlot(&window_store.slots[taskbar_slot]);
    var rect = taskbarRectForSlot(&window_store.slots[taskbar_slot]);
    var any = false;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const button_rect = taskbarProcessButtonRect(taskbar_rect, i, count) orelse continue;
        const merged = includeRect(any, rect, button_rect);
        any = merged.any;
        rect = merged.rect;
    }
    return if (any) rect else taskbar_rect;
}

fn taskbarProcessHitSlot(x: i32, y: i32) ?usize {
    const taskbar_slot = findTaskbarVisibleSlot() orelse return null;
    const taskbar_rect = taskbarRectForSlot(&window_store.slots[taskbar_slot]);
    if (!pointInRect(x, y, taskbar_rect)) return null;

    var slots: [max_windows]usize = undefined;
    const count = collectTaskbarProcessSlots(&slots);
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const button_rect = taskbarProcessButtonRect(taskbar_rect, i, count) orelse continue;
        if (pointInRect(x, y, button_rect)) return slots[i];
    }
    return null;
}

fn closeWindowSlot(slot: usize) bool {
    if (slot >= max_windows) return false;
    const slot_ref = &window_store.slots[slot];
    if (!slot_ref.source.active or !slot_ref.frame.active) return false;
    slot_ref.reset();
    window_shadow_valid[slot] = false;
    window_gpu_resources[slot] = null;
    logged_invalid_window_source[slot] = false;
    if (debug_recent_window_slot != null and debug_recent_window_slot.? == slot) {
        debug_recent_window_slot = null;
        debug_recent_window_sync_logged = false;
        debug_recent_window_draw_logged = false;
    }
    invalidateWindowPaintOrderCache();
    invalidateWindowBodyCache(slot);
    return true;
}

fn minimizeWindowSlot(slot: usize) bool {
    if (slot >= max_windows) return false;
    const slot_ref = &window_store.slots[slot];
    if (!slot_ref.source.active or !slot_ref.frame.active or !slot_ref.frame.visible) return false;
    slot_ref.frame.visible = false;
    slot_ref.minimize_hover = false;
    slot_ref.minimize_down = false;
    slot_ref.close_hover = false;
    slot_ref.close_down = false;
    slot_ref.frame.prev_minimize_hover = false;
    slot_ref.frame.prev_minimize_down = false;
    slot_ref.frame.prev_close_hover = false;
    slot_ref.frame.prev_close_down = false;
    invalidateWindowPaintOrderCache();
    invalidateWindowBodyCache(slot);
    return true;
}

fn activateWindowSlot(slot: usize) bool {
    if (slot >= max_windows) return false;
    const slot_ref = &window_store.slots[slot];
    if (!slot_ref.source.active or !slot_ref.frame.active) return false;

    const was_visible = slot_ref.frame.visible;
    const order = currentWindowPaintOrder(&window_store);
    const already_top = was_visible and order.len != 0 and order[order.len - 1] == slot;
    if (was_visible and already_top) return false;

    slot_ref.frame.visible = true;
    slot_ref.z_order = allocWindowZOrder();
    invalidateWindowPaintOrderCache();
    invalidateWindowBodyCache(slot);
    return true;
}

fn taskbarStatePagePtr() ?*volatile TaskbarStatePage {
    const page: *volatile TaskbarStatePage = @ptrFromInt(taskbar_state_shared_va);
    if (page.magic != taskbar_state_magic or page.version != taskbar_protocol_version) return null;
    return page;
}

fn taskbarCommandPagePtr() ?*volatile TaskbarCommandPage {
    const page: *volatile TaskbarCommandPage = @ptrFromInt(taskbar_command_shared_va);
    if (page.magic != taskbar_command_magic or page.version != taskbar_protocol_version) return null;
    return page;
}

fn syncTaskbarStatePage() void {
    const page = taskbarStatePagePtr() orelse return;
    var slots: [max_windows]usize = undefined;
    const count = collectTaskbarProcessSlots(&slots);

    var changed = @as(usize, page.entry_count) != count;
    var i: usize = 0;
    while (i < protocol.taskbar_entry_max) : (i += 1) {
        const src_slot = if (i < count) &window_store.slots[slots[i]] else null;
        const dst = &page.entries[i];
        const next_window_id: u32 = if (src_slot) |slot_ref| slot_ref.source.window_id else 0;
        const next_flags: u32 = if (src_slot != null and src_slot.?.frame.visible) taskbar_entry_flag_visible else 0;
        const next_title_len: u16 = if (src_slot) |slot_ref| @intCast(slot_ref.frame.title_len) else 0;
        if (!changed and (dst.window_id != next_window_id or dst.flags != next_flags or dst.title_len != next_title_len)) {
            changed = true;
        }
        if (src_slot) |slot_ref| {
            var title_i: usize = 0;
            while (title_i < window_title_max_bytes) : (title_i += 1) {
                const next = if (title_i < slot_ref.frame.title_len) slot_ref.frame.title[title_i] else 0;
                if (!changed and dst.title[title_i] != next) changed = true;
                dst.title[title_i] = next;
            }
        } else {
            var title_i: usize = 0;
            while (title_i < window_title_max_bytes) : (title_i += 1) {
                if (!changed and dst.title[title_i] != 0) changed = true;
                dst.title[title_i] = 0;
            }
        }
        dst.window_id = next_window_id;
        dst.flags = next_flags;
        dst.title_len = next_title_len;
    }

    if (changed) {
        page.entry_count = @intCast(count);
        page.seq +%= 1;
    }
}

fn processTaskbarCommand(observed_seq: *u64) ?Rect {
    const page = taskbarCommandPagePtr() orelse return null;
    if (page.seq == observed_seq.*) return null;
    observed_seq.* = page.seq;
    if (page.command == taskbar_command_none) return null;
    if (page.command != taskbar_command_activate) return null;

    const slot = findWindowSlotById(page.window_id) orelse return null;
    const was_visible = window_store.slots[slot].frame.visible;
    const old_rect = if (was_visible) windowBounds(&window_store.slots[slot].frame) else fullScreenRect();
    if (!activateWindowSlot(slot)) return if (was_visible) windowBounds(&window_store.slots[slot].frame) else null;

    if (!was_visible) {
        return windowBounds(&window_store.slots[slot].frame);
    }
    var merged = includeRect(false, fullScreenRect(), old_rect);
    merged = includeRect(merged.any, merged.rect, windowBounds(&window_store.slots[slot].frame));
    return merged.rect;
}

fn allocWindowZOrder() u32 {
    const z = if (next_window_z_order == 0) @as(u32, 1) else next_window_z_order;
    next_window_z_order +%= 1;
    if (next_window_z_order == 0) next_window_z_order = 1;
    return z;
}

fn currentWindowPaintOrder(store: *const WindowStore) []const usize {
    if (!window_paint_order_dirty) return window_paint_order_cache[0..window_paint_order_count];

    var count: usize = 0;
    var i: usize = 0;
    while (i < max_windows) : (i += 1) {
        const slot = &store.slots[i];
        if (!slot.frame.active or !slot.frame.visible) continue;
        window_paint_order_cache[count] = i;
        count += 1;
    }

    var scan: usize = 1;
    while (scan < count) : (scan += 1) {
        const idx = window_paint_order_cache[scan];
        const z = store.slots[idx].z_order;
        var pos = scan;
        while (pos > 0) {
            const prev = window_paint_order_cache[pos - 1];
            const prev_z = store.slots[prev].z_order;
            if (prev_z < z or (prev_z == z and prev < idx)) break;
            window_paint_order_cache[pos] = prev;
            pos -= 1;
        }
        window_paint_order_cache[pos] = idx;
    }

    var taskbar_slot: ?usize = null;
    i = 0;
    while (i < count) : (i += 1) {
        if (isTaskbarWindowSlot(&store.slots[window_paint_order_cache[i]])) {
            taskbar_slot = i;
            break;
        }
    }
    if (taskbar_slot) |slot_pos| {
        const idx = window_paint_order_cache[slot_pos];
        i = slot_pos;
        while (i + 1 < count) : (i += 1) {
            window_paint_order_cache[i] = window_paint_order_cache[i + 1];
        }
        window_paint_order_cache[count - 1] = idx;
    }

    window_paint_order_count = count;
    window_paint_order_dirty = false;
    return window_paint_order_cache[0..window_paint_order_count];
}

fn findTopWindowAt(x: i32, y: i32) ?usize {
    if (findTaskbarVisibleSlot()) |taskbar_slot| {
        if (pointInRect(x, y, taskbarRectForSlot(&window_store.slots[taskbar_slot]))) return null;
    }
    const order = currentWindowPaintOrder(&window_store);
    var i = order.len;
    while (i > 0) {
        const idx = order[i - 1];
        const win = &window_store.slots[idx].frame;
        if (!windowHasFrame(win)) {
            i -= 1;
            continue;
        }
        const bounds = windowBounds(win);
        if (x >= bounds.x0 and x < bounds.x1 and y >= bounds.y0 and y < bounds.y1) return idx;
        i -= 1;
    }
    return null;
}

fn bringWindowToFront(slot: usize) bool {
    if (slot >= max_windows) return false;
    if (!window_store.slots[slot].frame.active or !window_store.slots[slot].frame.visible) return false;
    const order = currentWindowPaintOrder(&window_store);
    if (order.len == 0 or order[order.len - 1] == slot) return false;
    window_store.slots[slot].z_order = allocWindowZOrder();
    invalidateWindowPaintOrderCache();
    return true;
}

fn sendWindowToBack(slot: usize) bool {
    if (slot >= max_windows) return false;
    if (!window_store.slots[slot].frame.active or !window_store.slots[slot].frame.visible) return false;
    const order = currentWindowPaintOrder(&window_store);
    if (order.len <= 1 or order[0] == slot) return false;

    var next_z: u32 = 1;
    window_store.slots[slot].z_order = next_z;
    next_z += 1;

    var i: usize = 0;
    while (i < order.len) : (i += 1) {
        const idx = order[i];
        if (idx == slot) continue;
        window_store.slots[idx].z_order = next_z;
        next_z += 1;
    }
    next_window_z_order = next_z;
    if (next_window_z_order == 0) next_window_z_order = 1;
    invalidateWindowPaintOrderCache();
    return true;
}

fn findWindowSlotById(window_id: u32) ?usize {
    return window_store.findById(window_id);
}

fn findFreeWindowSlot() ?usize {
    return window_store.findFree();
}

fn windowMetaPtr(slot: usize) ?*const volatile WindowMeta {
    if (slot >= max_windows) return null;
    const slot_ref = &window_store.slots[slot];
    if (!slot_ref.source.active) return null;
    const meta_va = slot_ref.source.meta_va;
    const map_lo = window_map_base_va;
    const map_hi = window_map_base_va + max_windows * window_map_stride_va;
    if (meta_va < map_lo or meta_va >= map_hi) return null;
    const meta: *const volatile WindowMeta = @ptrFromInt(meta_va);
    if (meta.magic != window_meta_magic) return null;
    if (meta.version != protocol.window_protocol_version) return null;
    return meta;
}

fn updateWindowTitleFromMeta(slot_ref: *WindowSlot, meta: *const volatile WindowMeta) bool {
    const requested_len: usize = if (meta.title_len < window_title_max_bytes) meta.title_len else window_title_max_bytes;
    var title_changed = slot_ref.frame.title_len != requested_len;
    var i: usize = 0;
    while (i < requested_len) : (i += 1) {
        const next = sanitizeTitleByte(meta.title[i]);
        if (!title_changed and slot_ref.frame.title[i] != next) title_changed = true;
        slot_ref.frame.title[i] = next;
    }
    slot_ref.frame.title_len = requested_len;
    if (title_changed) refreshWindowTitleCache(slot_ref);
    return title_changed;
}

fn syncWindowPositionFromMeta(win: *WindowState, meta: *const volatile WindowMeta) bool {
    const next_x = clampWindowX(win, meta.pos_x);
    const next_y = clampWindowY(win, meta.pos_y);
    if (win.x == next_x and win.y == next_y) return false;
    win.x = next_x;
    win.y = next_y;
    return true;
}

fn readWindowDirtyRegion(src: *const WindowSource, meta: *const volatile WindowMeta) SourceRegion {
    if (src.width == 0 or src.height == 0) return .{};

    const x0 = @as(usize, meta.dirty_x);
    const y0 = @as(usize, meta.dirty_y);
    const w = @as(usize, meta.dirty_w);
    const h = @as(usize, meta.dirty_h);
    if (x0 >= src.width or y0 >= src.height or w == 0 or h == 0) {
        return .{ .x = 0, .y = 0, .w = src.width, .h = src.height };
    }

    const clamped_w = if (w < src.width - x0) w else src.width - x0;
    const clamped_h = if (h < src.height - y0) h else src.height - y0;
    if (clamped_w == 0 or clamped_h == 0) {
        return .{ .x = 0, .y = 0, .w = src.width, .h = src.height };
    }

    return .{
        .x = x0,
        .y = y0,
        .w = clamped_w,
        .h = clamped_h,
    };
}

fn windowBounds(win: *const WindowState) Rect {
    const shadow_right = if (windowHasFrame(win)) window_shadow_right_extent else 0;
    const shadow_bottom = if (windowHasFrame(win)) window_shadow_bottom_extent else 0;
    return clipRectToScreen(.{
        .x0 = win.x,
        .y0 = win.y,
        .x1 = win.x + windowWidth(win) + shadow_right,
        .y1 = win.y + windowHeight(win) + shadow_bottom,
    });
}

fn screenRectForSourceRegion(win: *const WindowState, region: SourceRegion) Rect {
    const scale = windowScale(win);
    const x0 = contentOriginX(win) + @as(i32, @intCast(region.x * scale));
    const y0 = contentOriginY(win) + @as(i32, @intCast(region.y * scale));
    const x1 = x0 + @as(i32, @intCast(region.w * scale));
    const y1 = y0 + @as(i32, @intCast(region.h * scale));
    return clipRectToScreen(.{ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1 });
}

fn windowSourceUsesDma(src: *const WindowSource) bool {
    return src.dma_pixels;
}

fn registerWindowCap(page_paddr: u64) ?Rect {
    const cap: *const volatile WindowCap = @ptrFromInt(ipc_rx_page_va);
    if (cap.magic != window_cap_magic) {
        logWindowRegisterRejected("bad_magic", page_paddr);
        return null;
    }
    if (cap.version != 1) {
        logWindowRegisterRejected("bad_version", page_paddr);
        return null;
    }
    const rights = protocol.decodeWindowRights(cap.rights_bits);
    if (cap.width == 0 or cap.height == 0) {
        logWindowRegisterRejected("bad_size", page_paddr);
        return null;
    }
    if (cap.pixels_page_count == 0) {
        _ = userLog("Compositor: pixels_page_count==0\n");
        return null;
    }
    const page_count: usize = @intCast(cap.pixels_page_count);
    if (page_count > max_window_pixel_pages) {
        _ = userLog("Compositor: pixels_page_count too large\n");
        return null;
    }
    const cap_size = @sizeOf(WindowCap);
    const list_capacity = (4096 - cap_size) / @sizeOf(u64);
    if (page_count > list_capacity) {
        _ = userLog("Compositor: cap paddr list overflow\n");
        return null;
    }
    if (cap.pixels_cap_paddr < 0x1000 or cap.meta_cap_paddr < 0x1000) {
        logWindowRegisterRejected("bad_cap_paddr", page_paddr);
        return null;
    }

    const cap_snapshot = snapshotWindowCap(cap);

    const existing = findWindowSlotById(cap_snapshot.window_id);
    const slot = existing orelse (findFreeWindowSlot() orelse return null);
    const prior_z = if (existing != null) window_store.slots[slot].z_order else 0;
    const slot_base = window_map_base_va + slot * window_map_stride_va;
    const map_meta_va = slot_base + 0x1000;
    const map_pixel_va = slot_base + 0x2000;
    const dma_pixels = rights.dma_pixels and ((cap_snapshot.flags & protocol.window_flag_allow_pixels_dma) != 0);

    if (mapPage(map_meta_va, cap_snapshot.meta_cap_paddr, false) != syscall_ok) {
        logWindowRegisterRejected("map_meta_failed", page_paddr);
        return null;
    }
    const paddr_list: [*]const volatile u64 = @ptrFromInt(ipc_rx_page_va + cap_size);
    var i: usize = 0;
    while (i < page_count) : (i += 1) {
        if (paddr_list[i] < 0x1000) {
            logWindowRegisterRejected("bad_pixel_paddr", page_paddr);
            return null;
        }
    }
    if (mapPagesBatch(map_pixel_va, ipc_rx_page_va + cap_size, cap_snapshot.pixels_page_count, false) != syscall_ok) {
        logWindowRegisterRejected("map_pixels_failed", page_paddr);
        return null;
    }

    const slot_ref = &window_store.slots[slot];
    slot_ref.reset();
    slot_ref.z_order = if (prior_z != 0) prior_z else allocWindowZOrder();
    slot_ref.source = .{
        .active = true,
        .window_id = cap_snapshot.window_id,
        .pixel_va = map_pixel_va,
        .pixels_paddr = cap_snapshot.pixels_cap_paddr,
        .pixels_page_count = cap_snapshot.pixels_page_count,
        .dma_pixels = dma_pixels,
        .meta_va = map_meta_va,
        .width = cap_snapshot.width,
        .height = cap_snapshot.height,
        .pitch = cap_snapshot.pitch,
        .flags = cap_snapshot.flags,
    };

    const frame_ptr: *volatile WindowState = &slot_ref.frame;
    frame_ptr.active = true;
    frame_ptr.visible = true;
    frame_ptr.src.x = 0;
    frame_ptr.src.y = 0;
    frame_ptr.src.w = cap_snapshot.width;
    frame_ptr.src.h = cap_snapshot.height;
    frame_ptr.content_scale = @intCast(if ((cap_snapshot.flags & window_flag_low_scale) != 0) low_window_scale else default_window_scale);
    frame_ptr.flags = cap_snapshot.flags;
    frame_ptr.x = @as(i32, @intCast(72 + (slot % 2) * 220));
    frame_ptr.y = @as(i32, @intCast(110 + (slot / 2) * 230));
    frame_ptr.drag_off_x = 0;
    frame_ptr.drag_off_y = 0;
    frame_ptr.prev_minimize_hover = false;
    frame_ptr.prev_minimize_down = false;
    frame_ptr.prev_close_hover = false;
    frame_ptr.prev_close_down = false;
    frame_ptr.title_len = 0;
    if (windowMetaPtr(slot)) |meta| {
        _ = updateWindowTitleFromMeta(slot_ref, meta);
    }
    window_shadow_valid[slot] = false;
    window_gpu_resources[slot] = null;
    logged_invalid_window_source[slot] = false;
    debug_recent_window_slot = slot;
    debug_recent_window_sync_logged = false;
    debug_recent_window_draw_logged = false;
    invalidateWindowPaintOrderCache();
    invalidateWindowBodyCache(slot);
    logWindowRegistered(slot, cap_snapshot.window_id, cap_snapshot.width, cap_snapshot.height, cap_snapshot.flags);
    return windowBounds(&slot_ref.frame);
}

fn blitWindowSourceToWindow(back: [*]u32, slot: usize) void {
    const slot_ref = &window_store.slots[slot];
    const src = &slot_ref.source;
    if (!src.active) return;
    const win = &slot_ref.frame;
    if (!win.active or !win.visible) return;
    if (!windowSourcePixelsReadable(slot, src)) return;
    const src_pixels: [*]const volatile u32 = @ptrFromInt(src.pixel_va);
    var vy: usize = 0;
    while (vy < src.height) : (vy += 1) {
        var vx: usize = 0;
        while (vx < src.width) : (vx += 1) {
            const src_index = vy * src.pitch + vx;
            _ = blitScaledPixelToWindow(back, win, vx, vy, src_pixels[src_index]);
        }
    }
}

fn windowPixelsPtr(slot: usize) [*]const volatile u32 {
    return @ptrFromInt(window_store.slots[slot].source.pixel_va);
}

fn blitWindowSourceToSurface(surface: *const DrawSurface, slot: usize, win: *const WindowState) void {
    const slot_ref = &window_store.slots[slot];
    const src = &slot_ref.source;
    if (!src.active) return;
    if (!win.active or !win.visible) return;
    if (!windowSourcePixelsReadable(slot, src)) return;
    const scale = windowScale(win);

    const content_x0 = contentOriginX(win);
    const content_y0 = contentOriginY(win);
    const content_rect: Rect = .{
        .x0 = content_x0,
        .y0 = content_y0,
        .x1 = content_x0 + @as(i32, @intCast(src.width * scale)),
        .y1 = content_y0 + @as(i32, @intCast(src.height * scale)),
    };
    const clipped = intersectRect(content_rect, surface.clip) orelse return;
    const src_pixels = windowPixelsPtr(slot);

    if (scale == 1) {
        const src_x0: usize = @intCast(clipped.x0 - content_x0);
        const src_y0: usize = @intCast(clipped.y0 - content_y0);
        const copy_w: usize = @intCast(clipped.x1 - clipped.x0);
        const copy_h: usize = @intCast(clipped.y1 - clipped.y0);
        var row: usize = 0;
        while (row < copy_h) : (row += 1) {
            const src_row = (src_y0 + row) * src.pitch + src_x0;
            const dst_row = (@as(usize, @intCast(clipped.y0)) + row) * fb_pitch + @as(usize, @intCast(clipped.x0));
            var col: usize = 0;
            while (col < copy_w) : (col += 1) {
                const index = dst_row + col;
                if (index >= surface.pixel_capacity) continue;
                surface.pixels[index] = src_pixels[src_row + col];
            }
        }
        return;
    }

    if (scale == 2) {
        const dst_pixels = surface.pixels;
        const vx_start: i32 = clampI32(@divFloor(clipped.x0 - content_x0, 2), 0, @intCast(src.width));
        const vy_start: i32 = clampI32(@divFloor(clipped.y0 - content_y0, 2), 0, @intCast(src.height));
        const vx_end: i32 = clampI32(@divFloor((clipped.x1 - 1) - content_x0, 2) + 1, 0, @intCast(src.width));
        const vy_end: i32 = clampI32(@divFloor((clipped.y1 - 1) - content_y0, 2) + 1, 0, @intCast(src.height));
        if (vx_start >= vx_end or vy_start >= vy_end) return;

        var vy: i32 = vy_start;
        while (vy < vy_end) : (vy += 1) {
            const vy_u: usize = @intCast(vy);
            const src_row = vy_u * src.pitch;
            const row_y = content_y0 + vy * 2;
            const py0 = if (row_y > clipped.y0) row_y else clipped.y0;
            const py1 = if (row_y + 2 < clipped.y1) row_y + 2 else clipped.y1;
            if (py0 >= py1) continue;

            var py = py0;
            while (py < py1) : (py += 1) {
                const dst_row = @as(usize, @intCast(py)) * fb_pitch;
                var vx: i32 = vx_start;
                while (vx < vx_end) : (vx += 1) {
                    const vx_u: usize = @intCast(vx);
                    const color = src_pixels[src_row + vx_u];
                    const col_x = content_x0 + vx * 2;
                    const px0 = if (col_x > clipped.x0) col_x else clipped.x0;
                    const px1 = if (col_x + 2 < clipped.x1) col_x + 2 else clipped.x1;
                    var px = px0;
                    while (px < px1) : (px += 1) {
                        const index = dst_row + @as(usize, @intCast(px));
                        if (index >= surface.pixel_capacity) continue;
                        dst_pixels[index] = color;
                    }
                }
            }
        }
        return;
    }

    const scale_i32 = windowScaleI32(win);
    const vx_start: i32 = clampI32(@divFloor(clipped.x0 - content_x0, scale_i32), 0, @intCast(src.width));
    const vy_start: i32 = clampI32(@divFloor(clipped.y0 - content_y0, scale_i32), 0, @intCast(src.height));
    const vx_end: i32 = clampI32(@divFloor((clipped.x1 - 1) - content_x0, scale_i32) + 1, 0, @intCast(src.width));
    const vy_end: i32 = clampI32(@divFloor((clipped.y1 - 1) - content_y0, scale_i32) + 1, 0, @intCast(src.height));
    if (vx_start >= vx_end or vy_start >= vy_end) return;

    var vy: i32 = vy_start;
    while (vy < vy_end) : (vy += 1) {
        const vy_u: usize = @intCast(vy);
        const row_start_y = content_y0 + vy * scale_i32;
        const row_end_y = row_start_y + scale_i32;
        const py0 = if (row_start_y > clipped.y0) row_start_y else clipped.y0;
        const py1 = if (row_end_y < clipped.y1) row_end_y else clipped.y1;
        if (py0 >= py1) continue;

        var vx: i32 = vx_start;
        while (vx < vx_end) : (vx += 1) {
            const vx_u: usize = @intCast(vx);
            const src_index = vy_u * src.pitch + vx_u;
            const color = src_pixels[src_index];
            const col_start_x = content_x0 + vx * scale_i32;
            const col_end_x = col_start_x + scale_i32;
            const px0 = if (col_start_x > clipped.x0) col_start_x else clipped.x0;
            const px1 = if (col_end_x < clipped.x1) col_end_x else clipped.x1;
            if (px0 >= px1) continue;

            var py: i32 = py0;
            while (py < py1) : (py += 1) {
                const row: usize = @as(usize, @intCast(py)) * fb_pitch;
                var px: i32 = px0;
                while (px < px1) : (px += 1) {
                    const index = row + @as(usize, @intCast(px));
                    if (index >= surface.pixel_capacity) continue;
                    surface.pixels[index] = color;
                }
            }
        }
    }
}

fn blitWindowSourceToWindowSurface(surface: *const DrawSurface, slot: usize) void {
    if (debug_recent_window_slot != null and debug_recent_window_slot.? == slot and !debug_recent_window_draw_logged) {
        debug_recent_window_draw_logged = true;
        logRecentWindowStage(slot, "draw");
    }
    blitWindowSourceToSurface(surface, slot, &window_store.slots[slot].frame);
}

fn redrawSceneBackgroundHud(surface: *const DrawSurface, mouse_x: i32, mouse_y: i32) void {
    drawSolidRectSurface(
        surface,
        surface.clip.x0,
        surface.clip.y0,
        surface.clip.x1 - surface.clip.x0,
        surface.clip.y1 - surface.clip.y0,
        background_color,
    );
    drawMouseCoordHudSurface(surface, mouse_x, mouse_y);
}

fn redrawSceneWindowsLayered(surface: *const DrawSurface, store: *const WindowStore) void {
    const order = currentWindowPaintOrder(store);
    var i: usize = 0;
    while (i < order.len) : (i += 1) {
        const slot_index = order[i];
        const slot = &store.slots[slot_index];
        const win = &slot.frame;
        if (!win.active or !win.visible) continue;
        if (intersectRect(windowBounds(win), surface.clip) == null) continue;
        drawWindowChromeSurface(surface, slot);
        blitWindowSourceToWindowSurface(surface, slot_index);
    }
}

fn redrawSceneSurface(
    surface: *const DrawSurface,
    store: *const WindowStore,
    mouse_x: i32,
    mouse_y: i32,
    draw_cursor: bool,
    dragging_slot: ?usize,
) void {
    _ = dragging_slot;
    redrawSceneBackgroundHud(surface, mouse_x, mouse_y);
    redrawSceneWindowsLayered(surface, store);
    if (draw_cursor) drawCursorSurface(surface, mouse_x, mouse_y);
}

fn ensureWindowGpuResource(slot: usize) ?virtgpu.ResourceHandle {
    if (slot >= max_windows) {
        logInvalidGpuSlot(slot);
        return null;
    }
    const slot_ref = &window_store.slots[slot];
    if (!slot_ref.source.active) return null;
    if (slot >= window_gpu_resources.len) {
        logInvalidGpuSlot(slot);
        return null;
    }
    if (window_gpu_resources[slot]) |resource_handle| {
        if (virtgpu.virtgpu_dimensions(resource_handle)) |dims| {
            if (dims.width == slot_ref.source.width and dims.height == slot_ref.source.height) return resource_handle;
        }
    }
    const source = &slot_ref.source;
    const created = virtgpu.virtgpu_create_resource_from_single_page(
        source.width,
        source.height,
        source.pixels_paddr,
        source.pixel_va,
    ) orelse virtgpu.virtgpu_create_resource(source.width, source.height) orelse return null;
    window_gpu_resources[slot] = created;
    window_shadow_valid[slot] = false;
    return created;
}

fn syncWindowGpuContent(slot: usize, dirty_region: SourceRegion) ?Rect {
    if (slot >= max_windows) {
        logInvalidGpuSlot(slot);
        return null;
    }
    const slot_ref = &window_store.slots[slot];
    if (!slot_ref.source.active or !slot_ref.frame.active or !slot_ref.frame.visible) return null;
    if (slot >= window_shadow_valid.len or slot >= window_gpu_resources.len) {
        logInvalidGpuSlot(slot);
        return null;
    }
    if (dirty_region.w == 0 or dirty_region.h == 0) return null;

    const src = &slot_ref.source;
    logFirstGpuSyncSlot(slot, src, &slot_ref.frame);
    if (windowSourceUsesDma(src)) {
        // DMA-backed windows already expose their pixel pages directly to the compositor.
        window_shadow_valid[slot] = true;
        return screenRectForSourceRegion(&slot_ref.frame, dirty_region);
    }
    // CPU-backed windows are also composed directly from their mapped source pages.
    // Keeping this branch explicit documents that there is no staging copy for either path.
    window_shadow_valid[slot] = true;
    return screenRectForSourceRegion(&slot_ref.frame, dirty_region);
}

fn syncWindow(slot: usize) ?Rect {
    if (slot >= max_windows) return null;
    if (debug_recent_window_slot != null and debug_recent_window_slot.? == slot and !debug_recent_window_sync_logged) {
        debug_recent_window_sync_logged = true;
        logRecentWindowStage(slot, "sync");
    }
    const slot_ref = &window_store.slots[slot];
    const meta = windowMetaPtr(slot) orelse return null;
    const seq = meta.seq;
    if (seq == slot_ref.source.observed_meta_seq and window_shadow_valid[slot]) return null;
    slot_ref.source.observed_meta_seq = seq;

    const old_bounds = windowBounds(&slot_ref.frame);
    const position_changed = syncWindowPositionFromMeta(&slot_ref.frame, meta);
    const title_changed = updateWindowTitleFromMeta(slot_ref, meta);
    var dirty_region = readWindowDirtyRegion(&slot_ref.source, meta);
    if (!window_shadow_valid[slot]) {
        dirty_region = .{
            .x = 0,
            .y = 0,
            .w = slot_ref.source.width,
            .h = slot_ref.source.height,
        };
    }
    const content_rect = syncWindowGpuContent(slot, dirty_region);
    if (position_changed or title_changed or content_rect != null) invalidateWindowBodyCache(slot);
    if (position_changed) {
        var merged = includeRect(false, fullScreenRect(), old_bounds);
        merged = includeRect(merged.any, merged.rect, windowBounds(&slot_ref.frame));
        return merged.rect;
    }
    if (title_changed) {
        if (windowHasFrame(&slot_ref.frame)) {
            var merged = includeRect(false, fullScreenRect(), windowBounds(&slot_ref.frame));
            if (taskbarProcessListRect()) |taskbar_rect| {
                merged = includeRect(merged.any, merged.rect, taskbar_rect);
            }
            return merged.rect;
        }
        return windowBounds(&slot_ref.frame);
    }
    return content_rect;
}

pub fn run(comptime gpu_mode: bool) noreturn {
    if (!gpu_mode) {
        _ = userLog("Compositor: non-gpu mode disabled\n");
        while (true) asm volatile ("pause");
    }

    mouse_state_storage = .{
        .x = @as(i32, @intCast(fb_width / 2)),
        .y = @as(i32, @intCast(fb_height / 2)),
    };
    window_store = .{};
    window_shadow_valid = [_]bool{false} ** max_windows;
    window_gpu_resources = [_]?virtgpu.ResourceHandle{null} ** max_windows;
    logged_invalid_window_source = [_]bool{false} ** max_windows;
    next_window_z_order = 1;
    window_paint_order_count = 0;
    window_paint_order_dirty = true;
    logged_invalid_gpu_slot = null;
    logged_first_gpu_sync_slot = false;
    debug_recent_window_slot = null;
    debug_recent_window_sync_logged = false;
    debug_recent_window_draw_logged = false;
    var last_cursor_rect = cursorRect(mouse_state_storage.x, mouse_state_storage.y);
    var i: usize = 0;

    var window_count: usize = 0;

    var dragging_index: ?usize = null;
    var drag_boost_logged = false;
    var scheduler_boost_active = false;
    var prev_left_down = false;
    var prev_right_down = false;
    var observed_taskbar_command_seq: u64 = 0;
    var force_full = true;
    var virtgpu_init_ok = false;
    resetCompositorPerfReport(readTsc());

    if (gpu_mode) {
        virtgpu_init_ok = virtgpu.virtgpu_init();
    }

    while (true) {
        const page_paddr = recvCap();
        if (page_paddr == syscall_err_empty) break;
        if (page_paddr < 0x1000) break;
        if (mapPage(ipc_rx_page_va, page_paddr, true) != syscall_ok) continue;

        const msg_words: [*]const volatile u64 = @ptrFromInt(ipc_rx_page_va);
        const magic32: u32 = @truncate(msg_words[0]);
        if (magic32 == window_cap_magic) {
            if (registerWindowCap(page_paddr) != null) {
                force_full = true;
            }
            continue;
        }
        if (msg_words[0] != shared_magic or mouse_state_storage.ready) continue;
        if (mapPage(shared_page_va, page_paddr, false) != syscall_ok) continue;

        const shared_now: *const volatile MouseSharedPage = @ptrFromInt(shared_page_va);
        if (shared_now.magic != shared_magic) continue;

        mouse_state_storage.ready = true;
        syncMouseState(true);
        force_full = true;
    }

    var gpu_resource: ?virtgpu.ResourceHandle = null;
    var virtgpu_active = false;
    var hardware_cursor_active = false;
    if (virtgpu_init_ok) {
        _ = userLog("Compositor: create_fb start\n");
        gpu_resource = virtgpu.virtgpu_create_fb(fb_width, fb_height);
        _ = userLog("Compositor: create_fb returned\n");
        if (gpu_resource) |resource_handle| {
            if (virtgpu.virtgpu_set_scanout(resource_handle)) {
                virtgpu_active = true;
                hardware_cursor_active = initHardwareCursor(mouse_state_storage.x, mouse_state_storage.y);
                _ = userLog("Compositor: set_scanout done\n");
            } else {
                _ = userLog("GpuCompositor: virtgpu set_scanout failed\n");
                while (true) asm volatile ("pause");
            }
        } else {
            _ = userLog("GpuCompositor: virtgpu create_fb failed\n");
            while (true) asm volatile ("pause");
        }
    } else {
        virtgpu.logInitFailureOnce();
        while (true) asm volatile ("pause");
    }

    while (true) {
        const iter_start_tsc = readTsc();
        var dirty_any = false;
        var dirty_rect = fullScreenRect();
        var old_bounds: [max_windows]Rect = undefined;
        var old_visible: [max_windows]bool = undefined;

        while (true) {
            const page_paddr = recvCap();
            if (page_paddr == syscall_err_empty) break;
            if (page_paddr < 0x1000) break;
            if (mapPage(ipc_rx_page_va, page_paddr, true) != syscall_ok) continue;

            const msg_words: [*]const volatile u64 = @ptrFromInt(ipc_rx_page_va);
            const magic32: u32 = @truncate(msg_words[0]);
            if (magic32 == window_cap_magic) {
                if (registerWindowCap(page_paddr) != null) {
                    force_full = true;
                }
                continue;
            }
            if (msg_words[0] != shared_magic or mouse_state_storage.ready) continue;
            if (mapPage(shared_page_va, page_paddr, false) != syscall_ok) continue;

            const shared_now: *const volatile MouseSharedPage = @ptrFromInt(shared_page_va);
            if (shared_now.magic != shared_magic) continue;

            mouse_state_storage.ready = true;
            syncMouseState(true);
            force_full = true;
        }

        // Boot fast-path: until at least one window is registered, avoid the
        // expensive compose/present path and keep polling mailbox quickly.
        if (recomputeWindowCount() == 0) {
            setSchedulerBoost(false, &scheduler_boost_active);
            force_full = false;
            compositor_perf_report.no_window_loops +%= 1;
            compositor_perf_report.idle_loops +%= 1;
            const iter_end_tsc = readTsc();
            compositor_perf_report.total_tsc +%= iter_end_tsc - iter_start_tsc;
            maybeLogCompositorPerf(iter_end_tsc);
            _ = waitEvent(true, 1);
            continue;
        }

        i = 0;
        while (i < max_windows) : (i += 1) {
            old_visible[i] = window_store.slots[i].frame.active and window_store.slots[i].frame.visible;
            old_bounds[i] = if (old_visible[i]) windowBounds(&window_store.slots[i].frame) else fullScreenRect();
        }

        syncMouseState(false);
        if (!force_full) {
            const next_cursor_rect = cursorRect(mouse_state_storage.x, mouse_state_storage.y);
            const mouse_moved = next_cursor_rect.x0 != last_cursor_rect.x0 or next_cursor_rect.y0 != last_cursor_rect.y0;
            if (mouse_moved) {
                var merged = includeRect(false, fullScreenRect(), hudRect());
                if (!hardware_cursor_active) {
                    merged = includeRect(merged.any, merged.rect, last_cursor_rect);
                    merged = includeRect(merged.any, merged.rect, next_cursor_rect);
                }
                dirty_any = merged.any;
                dirty_rect = merged.rect;
            }
        }

        if (hardware_cursor_active and !virtgpu.virtgpu_move_cursor(mouse_state_storage.x, mouse_state_storage.y)) {
            hardware_cursor_active = false;
            force_full = true;
        }

        window_count = recomputeWindowCount();
        if (window_count > max_windows) window_count = max_windows;
        const left_down = (mouse_state_storage.buttons & 0x1) != 0;
        const right_down = (mouse_state_storage.buttons & 0x2) != 0;
        const safe_window_count = max_windows;

        if (processTaskbarCommand(&observed_taskbar_command_seq)) |changed_rect| {
            const merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), changed_rect);
            dirty_any = merged.any;
            dirty_rect = merged.rect;
        }

        i = 0;
        while (i < max_windows) : (i += 1) {
            window_store.slots[i].minimize_hover = false;
            window_store.slots[i].minimize_down = false;
            window_store.slots[i].close_hover = false;
            window_store.slots[i].close_down = false;
        }

        const hovered_slot = findTopWindowAt(mouse_state_storage.x, mouse_state_storage.y);
        i = 0;
        while (i < safe_window_count) : (i += 1) {
            const slot_ref = &window_store.slots[i];
            const win = &slot_ref.frame;
            if (!win.active or !win.visible) continue;
            if (!windowHasFrame(win)) {
                slot_ref.minimize_hover = false;
                slot_ref.minimize_down = false;
                slot_ref.close_hover = false;
                slot_ref.close_down = false;
                continue;
            }
            const hover_target = hovered_slot != null and hovered_slot.? == i;
            const minimize_rect = windowMinimizeButtonRect(win);
            const close_rect = windowCloseButtonRect(win);
            slot_ref.minimize_hover = hover_target and pointInMinimizeButton(mouse_state_storage.x, mouse_state_storage.y, minimize_rect);
            slot_ref.minimize_down = slot_ref.minimize_hover and left_down;
            slot_ref.close_hover = hover_target and pointInCloseButton(mouse_state_storage.x, mouse_state_storage.y, close_rect);
            slot_ref.close_down = slot_ref.close_hover and left_down;
            if (slot_ref.minimize_hover != win.prev_minimize_hover or
                slot_ref.minimize_down != win.prev_minimize_down or
                slot_ref.close_hover != win.prev_close_hover or
                slot_ref.close_down != win.prev_close_down)
            {
                invalidateWindowBodyCache(i);
                const merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), old_bounds[i]);
                dirty_any = merged.any;
                dirty_rect = merged.rect;
            }
        }

        if (!prev_right_down and right_down) {
            if (hovered_slot) |idx| {
                const old_rect = windowBounds(&window_store.slots[idx].frame);
                if (sendWindowToBack(idx)) {
                    if (dragging_index != null and dragging_index.? == idx) dragging_index = null;
                    const new_rect = windowBounds(&window_store.slots[idx].frame);
                    var merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), old_rect);
                    merged = includeRect(merged.any, merged.rect, new_rect);
                    dirty_any = merged.any;
                    dirty_rect = merged.rect;
                }
            }
        }

        if (!prev_left_down and left_down) {
            if (hovered_slot) |idx| {
                const slot_ref = &window_store.slots[idx];
                var win = &slot_ref.frame;
                const old_rect = windowBounds(win);
                if (slot_ref.close_hover) {
                    if (closeWindowSlot(idx)) {
                        if (dragging_index != null and dragging_index.? == idx) dragging_index = null;
                        const merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), old_rect);
                        dirty_any = merged.any;
                        dirty_rect = merged.rect;
                    }
                } else if (slot_ref.minimize_hover) {
                    if (minimizeWindowSlot(idx)) {
                        if (dragging_index != null and dragging_index.? == idx) dragging_index = null;
                        const merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), old_rect);
                        dirty_any = merged.any;
                        dirty_rect = merged.rect;
                    }
                } else {
                    if (bringWindowToFront(idx)) {
                        const new_rect = windowBounds(win);
                        var merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), old_rect);
                        merged = includeRect(merged.any, merged.rect, new_rect);
                        dirty_any = merged.any;
                        dirty_rect = merged.rect;
                    }
                    if (windowHasFrame(win)) {
                        const hx0 = win.x + window_border_i32;
                        const hy0 = win.y + window_border_i32;
                        const hx1 = win.x + windowWidth(win) - window_border_i32;
                        const hy1 = win.y + window_header_h_i32;
                        if (!(mouse_state_storage.x >= hx0 and mouse_state_storage.x < hx1 and mouse_state_storage.y >= hy0 and mouse_state_storage.y < hy1)) {
                            // Click landed inside the window but outside the draggable title bar.
                            // Keep the frame pipeline running so button-edge state stays coherent.
                        } else {
                            win.drag_off_x = mouse_state_storage.x - win.x;
                            win.drag_off_y = mouse_state_storage.y - win.y;
                            dragging_index = idx;
                            const merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), old_bounds[idx]);
                            dirty_any = merged.any;
                            dirty_rect = merged.rect;
                            drag_boost_logged = true;
                        }
                    }
                }
            }
        }

        if (dragging_index) |idx| {
            if (idx >= safe_window_count) {
                dragging_index = null;
            } else {
                var win = &window_store.slots[idx].frame;
                if (!left_down or !win.visible) {
                    dragging_index = null;
                } else {
                    const old_rect = windowBounds(win);
                    const next_x = clampWindowX(win, mouse_state_storage.x - win.drag_off_x);
                    const next_y = clampWindowY(win, mouse_state_storage.y - win.drag_off_y);
                    if (next_x != win.x or next_y != win.y) {
                        win.x = next_x;
                        win.y = next_y;
                        var merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), old_rect);
                        merged = includeRect(merged.any, merged.rect, windowBounds(win));
                        dirty_any = merged.any;
                        dirty_rect = merged.rect;
                    }
                }
            }
        }

        if (!left_down) dragging_index = null;
        if (dragging_index == null and drag_boost_logged) {
            drag_boost_logged = false;
        }
        prev_left_down = left_down;
        prev_right_down = right_down;

        i = 0;
        while (i < safe_window_count) : (i += 1) {
            window_store.slots[i].frame.prev_minimize_hover = window_store.slots[i].minimize_hover;
            window_store.slots[i].frame.prev_minimize_down = window_store.slots[i].minimize_down;
            window_store.slots[i].frame.prev_close_hover = window_store.slots[i].close_hover;
            window_store.slots[i].frame.prev_close_down = window_store.slots[i].close_down;
        }

        const sync_start_tsc = readTsc();
        i = 0;
        while (i < safe_window_count) : (i += 1) {
            if (syncWindow(i)) |changed_rect| {
                const merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), changed_rect);
                dirty_any = merged.any;
                dirty_rect = merged.rect;
            }
        }
        syncTaskbarStatePage();
        compositor_perf_report.sync_tsc +%= readTsc() - sync_start_tsc;

        const present_rect = if (force_full) fullScreenRect() else clipRectToScreen(dirty_rect);
        if (force_full or dirty_any) {
            if (rectIsEmpty(present_rect)) {
                setSchedulerBoost(false, &scheduler_boost_active);
                force_full = false;
                last_cursor_rect = cursorRect(mouse_state_storage.x, mouse_state_storage.y);
                compositor_perf_report.idle_loops +%= 1;
                const iter_end_tsc = readTsc();
                compositor_perf_report.total_tsc +%= iter_end_tsc - iter_start_tsc;
                maybeLogCompositorPerf(iter_end_tsc);
                _ = waitEvent(true, 1);
                continue;
            }
        } else {
            setSchedulerBoost(false, &scheduler_boost_active);
            last_cursor_rect = cursorRect(mouse_state_storage.x, mouse_state_storage.y);
            compositor_perf_report.idle_loops +%= 1;
            const iter_end_tsc = readTsc();
            compositor_perf_report.total_tsc +%= iter_end_tsc - iter_start_tsc;
            maybeLogCompositorPerf(iter_end_tsc);
            _ = waitEvent(true, 1);
            continue;
        }

        setSchedulerBoost(true, &scheduler_boost_active);
        if (virtgpu_active and gpu_resource != null) {
            const gpu_pixels = virtgpu.virtgpu_pixels(gpu_resource.?) orelse {
                _ = userLog("GpuCompositor: virtgpu pixels lost\n");
                while (true) asm volatile ("pause");
            };
            const surface = DrawSurface{
                .pixels = gpu_pixels,
                .pixel_capacity = fb_pixels,
                .clip = present_rect,
            };
            const compose_start = readTsc();
            redrawSceneSurface(
                &surface,
                &window_store,
                mouse_state_storage.x,
                mouse_state_storage.y,
                !hardware_cursor_active,
                dragging_index,
            );
            const compose_end = readTsc();
            const transfer_rect: virtgpu.Rect = .{
                .x = @intCast(present_rect.x0),
                .y = @intCast(present_rect.y0),
                .width = @intCast(present_rect.x1 - present_rect.x0),
                .height = @intCast(present_rect.y1 - present_rect.y0),
            };
            const submit_start = readTsc();
            const transfer_ok = virtgpu.virtgpu_transfer(gpu_resource.?, transfer_rect);
            if (transfer_ok and !first_present_transfer_logged) {
                first_present_transfer_logged = true;
            }
            const flush_ok = transfer_ok and virtgpu.virtgpu_flush_rect(gpu_resource.?, transfer_rect);
            const submit_end = readTsc();
            compositor_perf_report.compose_tsc +%= compose_end - compose_start;
            compositor_perf_report.submit_tsc +%= submit_end - submit_start;
            compositor_perf_report.frames +%= 1;
            compositor_perf_report.presents +%= 1;
            compositor_perf_report.present_area_sum +%= @as(u64, transfer_rect.width) * @as(u64, transfer_rect.height);
            if (force_full) {
                compositor_perf_report.full_frames +%= 1;
            } else {
                compositor_perf_report.dirty_frames +%= 1;
            }
            logComposePerf(compose_end - compose_start, submit_end - submit_start, present_rect);
            if (flush_ok and !first_present_flush_logged) {
                first_present_flush_logged = true;
            }
            if (!flush_ok) {
                _ = userLog("GpuCompositor: virtgpu present failed\n");
                setSchedulerBoost(false, &scheduler_boost_active);
                compositor_perf_report.idle_loops +%= 1;
                const iter_end_tsc = readTsc();
                compositor_perf_report.total_tsc +%= iter_end_tsc - iter_start_tsc;
                maybeLogCompositorPerf(iter_end_tsc);
                _ = waitEvent(true, 1);
                continue;
            }
        } else {
            _ = userLog("GpuCompositor: virtgpu inactive\n");
            while (true) asm volatile ("pause");
        }

        last_cursor_rect = cursorRect(mouse_state_storage.x, mouse_state_storage.y);
        force_full = false;
        if (!first_compose_logged) {
            first_compose_logged = true;
            _ = userLog("Compositor: first compose done\n");
        }
        const iter_end_tsc = readTsc();
        compositor_perf_report.total_tsc +%= iter_end_tsc - iter_start_tsc;
        maybeLogCompositorPerf(iter_end_tsc);
        _ = waitEvent(true, 1);
    }
}
