const std = @import("std");
const font = @import("font.zig");
const protocol = @import("window_protocol.zig");
const model = @import("compositor_model.zig");
const virtgpu = @import("virtgpu.zig");

const syscall_map_page: u64 = 0x2;
const syscall_grant_cap: u64 = 0x8;
const syscall_log: u64 = 0x9;
const syscall_recv_cap: u64 = 0xA;

const syscall_ok: u64 = 0;
const syscall_err_empty: u64 = 13;

const shared_page_va: usize = 0x3C00_3000;
const ipc_rx_page_va: usize = 0x3C10_8000;

const shared_magic = protocol.mouse_shared_magic;
const window_cap_magic = protocol.window_cap_magic;
const window_meta_magic = protocol.window_meta_magic;
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
const pixel_pixels: usize = pixel_pitch * pixel_height;

const max_windows: usize = 4;

const background_color: u32 = 0x00FD_FDFD;
const window_border_color: u32 = 0x0087_837E;
const window_header_top_color: u32 = 0x00DA_D6D2;
const window_header_bottom_color: u32 = 0x00E1_DDDA;
const window_header_fill_color: u32 = 0x00DE_DAD7;
const window_header_text_color: u32 = 0x0033_383A;
const window_header_divider_color: u32 = 0x00B8_B4AF;
const window_header_divider_soft_color: u32 = 0x00CC_C7C3;
const window_content_bg_color: u32 = 0x00FF_FFFF;
const close_btn_color: u32 = window_header_fill_color;
const close_btn_glow_color: u32 = 0x00F3_F3F2;
const close_btn_hover_color: u32 = 0x00FF_FFFF;
const close_btn_down_color: u32 = 0x00F1_F0EE;
const close_btn_cross_color: u32 = 0x0054_5A5D;
const window_header_highlight_color: u32 = 0x00FF_FFFF;
const window_shadow_color: u32 = 0x0000_0000;
const window_shadow_right_extent: i32 = 2;
const window_shadow_bottom_extent: i32 = 3;

const window_scale: usize = 10;
const window_border: usize = 1;
const window_header_h: usize = 27;
const window_close_size: usize = 16;
const window_close_margin: usize = 8;
const window_corner_cut: usize = 5;
const close_btn_circle_padding: i32 = 2;
const window_title_y_bias: i32 = 1;
const window_border_i32: i32 = @as(i32, @intCast(window_border));
const window_header_h_i32: i32 = @as(i32, @intCast(window_header_h));
const window_close_size_i32: i32 = @as(i32, @intCast(window_close_size));
const window_close_margin_i32: i32 = @as(i32, @intCast(window_close_margin));
const window_scale_i32: i32 = @as(i32, @intCast(window_scale));

const window_title_offset: usize = 16;
const window_title_max_bytes = protocol.window_title_max_bytes;
const window_map_base_va: usize = 0x3C11_0000;
const window_map_stride_va: usize = 0x3000;
const WindowCap = protocol.WindowCap;
const WindowMeta = protocol.WindowMeta;
const MouseSharedPage = protocol.MouseSharedPage;
const WindowCapSnapshot = struct {
    window_id: u32,
    pixels_cap_paddr: u64,
    pixels_page_count: usize,
    meta_cap_paddr: u64,
    width: usize,
    height: usize,
    pitch: usize,
};
const Rect = model.Rect;
const SourceRegion = model.SourceRegion;
const MouseState = model.MouseState;
const WindowState = model.WindowFrame;
const WindowSource = model.WindowSource;
const WindowStore = model.WindowStore(max_windows);

var back_buffer_storage: [fb_pixels]u32 align(64) = [_]u32{0} ** fb_pixels;
var window_store: WindowStore = .{};
var window_shadow_storage: [max_windows][pixel_pixels]u32 align(64) = [_][pixel_pixels]u32{([_]u32{0} ** pixel_pixels)} ** max_windows;
var window_shadow_valid: [max_windows]bool = [_]bool{false} ** max_windows;
var window_gpu_resources: [max_windows]?*virtgpu.Resource = [_]?*virtgpu.Resource{null} ** max_windows;
var first_compose_logged = false;
var first_present_transfer_logged = false;
var first_present_flush_logged = false;
var perf_frame_counter: u64 = 0;
var mouse_state_storage: MouseState = .{
    .x = @as(i32, @intCast(fb_width / 2)),
    .y = @as(i32, @intCast(fb_height / 2)),
};

const DrawSurface = struct {
    pixels: [*]volatile u32,
    clip: Rect,
};

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

fn snapshotWindowCap(cap: *const volatile WindowCap) WindowCapSnapshot {
    return .{
        .window_id = cap.window_id,
        .pixels_cap_paddr = cap.pixels_cap_paddr,
        .pixels_page_count = cap.pixels_page_count,
        .meta_cap_paddr = cap.meta_cap_paddr,
        .width = cap.width,
        .height = cap.height,
        .pitch = cap.pixels_per_scan_line,
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

fn recvCap() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_recv_cap),
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

fn fullScreenRect() Rect {
    return .{ .x0 = 0, .y0 = 0, .x1 = fb_width_i32, .y1 = fb_height_i32 };
}

fn hudRect() Rect {
    return .{ .x0 = 14, .y0 = 14, .x1 = 14 + 176, .y1 = 14 + font.lineHeight(1) + 8 };
}

fn setSurfacePixel(surface: *const DrawSurface, x: i32, y: i32, color: u32) void {
    if (x < surface.clip.x0 or x >= surface.clip.x1 or y < surface.clip.y0 or y >= surface.clip.y1) return;
    if (x < 0 or y < 0 or x >= fb_width_i32 or y >= fb_height_i32) return;
    const ux: usize = @intCast(x);
    const uy: usize = @intCast(y);
    surface.pixels[uy * fb_pitch + ux] = color;
}

fn blendSurfacePixel(surface: *const DrawSurface, x: i32, y: i32, color: u32, alpha: u8) void {
    if (x < surface.clip.x0 or x >= surface.clip.x1 or y < surface.clip.y0 or y >= surface.clip.y1) return;
    if (x < 0 or y < 0 or x >= fb_width_i32 or y >= fb_height_i32) return;
    const ux: usize = @intCast(x);
    const uy: usize = @intCast(y);
    const index = uy * fb_pitch + ux;
    surface.pixels[index] = font.blendColor(surface.pixels[index], color, alpha);
}

fn blendSurfacePixelSubpixel(surface: *const DrawSurface, x: i32, y: i32, color: u32, alpha_r: u8, alpha_g: u8, alpha_b: u8) void {
    if (x < surface.clip.x0 or x >= surface.clip.x1 or y < surface.clip.y0 or y >= surface.clip.y1) return;
    if (x < 0 or y < 0 or x >= fb_width_i32 or y >= fb_height_i32) return;
    const ux: usize = @intCast(x);
    const uy: usize = @intCast(y);
    const index = uy * fb_pitch + ux;
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
            surface.pixels[row + @as(usize, @intCast(xx))] = color;
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

    var row: i32 = 0;
    while (row < header_h) : (row += 1) {
        drawSolidRectSurface(surface, header_x, header_y + row, header_w, 1, headerGradientColorAt(row, header_h));
    }

    if (header_h > 2 and header_w > 2) {
        drawSolidRectSurface(surface, header_x + 1, header_y, header_w - 2, 1, window_header_highlight_color);
        var x = header_x + 1;
        while (x < header_x + header_w - 1) : (x += 1) {
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
    drawSolidRectSurface(surface, x, y, w, 1, window_header_divider_soft_color);
    var xx = x;
    while (xx < x + w) : (xx += 1) {
        blendSurfacePixel(surface, xx, y + 1, window_shadow_color, 18);
    }
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

    var xx = x + 3;
    while (xx < x + w - 2) : (xx += 1) {
        blendSurfacePixel(surface, xx, y + h, window_shadow_color, 24);
        blendSurfacePixel(surface, xx, y + h + 1, window_shadow_color, 13);
        blendSurfacePixel(surface, xx, y + h + 2, window_shadow_color, 6);
    }

    var yy = y + window_header_h_i32;
    while (yy < y + h) : (yy += 1) {
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
    const resource = virtgpu.virtgpu_cursor_resource() orelse return false;
    var i: usize = 0;
    while (i < cursor_dim * cursor_dim) : (i += 1) {
        resource.pixels[i] = 0;
    }

    var bit_y: usize = 0;
    while (bit_y < cursor_height) : (bit_y += 1) {
        var bit_x: usize = 0;
        while (bit_x < cursor_width) : (bit_x += 1) {
            const ch = cursor_shape[bit_y][bit_x];
            if (ch == ' ') continue;
            const color: u32 = if (ch == '@') 0xFF00_0000 else 0xFFFF_FFFF;
            resource.pixels[bit_y * cursor_dim + bit_x] = color;
        }
    }

    const full_rect: virtgpu.Rect = .{
        .x = 0,
        .y = 0,
        .width = cursor_dim,
        .height = cursor_dim,
    };
    if (!virtgpu.virtgpu_transfer(resource, full_rect)) return false;
    if (!virtgpu.virtgpu_flush_rect(resource, full_rect)) return false;
    return virtgpu.virtgpu_update_cursor(resource, 0, 0, mouse_x, mouse_y);
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

fn windowContentW(win: *const WindowState) i32 {
    return @as(i32, @intCast(win.src.w * window_scale));
}

fn windowContentH(win: *const WindowState) i32 {
    return @as(i32, @intCast(win.src.h * window_scale));
}

fn windowWidth(win: *const WindowState) i32 {
    return windowContentW(win) + window_border_i32 * 2;
}

fn windowHeight(win: *const WindowState) i32 {
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
    perf_frame_counter +%= 1;
    if (perf_frame_counter != 1 and (perf_frame_counter % 120) != 0) return;

    const width: i32 = if (present_rect.x1 > present_rect.x0) present_rect.x1 - present_rect.x0 else 0;
    const height: i32 = if (present_rect.y1 > present_rect.y0) present_rect.y1 - present_rect.y0 else 0;
    const area: u64 = @as(u64, @intCast(width)) * @as(u64, @intCast(height));

    var buf: [160]u8 = undefined;
    var idx: usize = 0;
    appendText(buf[0..], &idx, "Compositor: perf compose_cpu_ticks=");
    appendU64Decimal(buf[0..], &idx, compose_ticks);
    appendText(buf[0..], &idx, " present_submit_ticks=");
    appendU64Decimal(buf[0..], &idx, present_submit_ticks);
    appendText(buf[0..], &idx, " present_rect_area=");
    appendU64Decimal(buf[0..], &idx, area);
    appendText(buf[0..], &idx, "\n");
    _ = userLog(buf[0..idx]);
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

fn clampWindowX(win: *const WindowState, x: i32) i32 {
    return clampI32(x, 0, fb_width_i32 - windowWidth(win));
}

fn clampWindowY(win: *const WindowState, y: i32) i32 {
    return clampI32(y, 0, fb_height_i32 - windowHeight(win));
}

fn contentOriginX(win: *const WindowState) i32 {
    return win.x + window_border_i32;
}

fn contentOriginY(win: *const WindowState) i32 {
    return win.y + window_header_h_i32;
}

fn pointInCircle(px: i32, py: i32, cx: i32, cy: i32, r: i32) bool {
    const dx = px - cx;
    const dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}

fn pointInCloseButton(px: i32, py: i32, rect: Rect) bool {
    if (!pointInRect(px, py, rect)) return false;
    const cx = @divTrunc(rect.x0 + rect.x1 - 1, 2);
    const cy = @divTrunc(rect.y0 + rect.y1 - 1, 2);
    const r = @divTrunc(rect.x1 - rect.x0, 2);
    return pointInCircle(px, py, cx, cy, r);
}

fn drawCloseButton(back: [*]u32, rect: Rect, hover: bool, down: bool) void {
    const cx = @divTrunc(rect.x0 + rect.x1 - 1, 2);
    const cy = @divTrunc(rect.y0 + rect.y1 - 1, 2);
    const outer_r = @divTrunc(rect.x1 - rect.x0, 2);
    const inner_r = outer_r - close_btn_circle_padding;
    if (inner_r <= 0) return;
    const outer_r2 = outer_r * outer_r;
    const inner_r2 = inner_r * inner_r;
    const bg = if (down) close_btn_down_color else if (hover) close_btn_hover_color else close_btn_color;

    var y = rect.y0;
    while (y < rect.y1) : (y += 1) {
        var x = rect.x0;
        while (x < rect.x1) : (x += 1) {
            const dx = x - cx;
            const dy = y - cy;
            const d2 = dx * dx + dy * dy;
            if (hover and d2 <= outer_r2) {
                setBackPixel(back, x, y, close_btn_glow_color);
            }
            if (d2 <= inner_r2) {
                setBackPixel(back, x, y, bg);
            }
        }
    }

    const cross_r = inner_r - 3;
    if (cross_r <= 0) return;
    var i: i32 = -cross_r;
    while (i <= cross_r) : (i += 1) {
        setBackPixel(back, cx + i, cy + i, close_btn_cross_color);
        setBackPixel(back, cx + i + 1, cy + i, close_btn_cross_color);
        setBackPixel(back, cx - i, cy + i, close_btn_cross_color);
        setBackPixel(back, cx - i - 1, cy + i, close_btn_cross_color);
    }
}

fn drawCloseButtonSurface(surface: *const DrawSurface, rect: Rect, hover: bool, down: bool) void {
    const cx = @divTrunc(rect.x0 + rect.x1 - 1, 2);
    const cy = @divTrunc(rect.y0 + rect.y1 - 1, 2);
    const outer_r = @divTrunc(rect.x1 - rect.x0, 2);
    const inner_r = outer_r - close_btn_circle_padding;
    if (inner_r <= 0) return;
    const outer_r2 = outer_r * outer_r;
    const inner_r2 = inner_r * inner_r;
    const bg = if (down) close_btn_down_color else if (hover) close_btn_hover_color else close_btn_color;

    var y = rect.y0;
    while (y < rect.y1) : (y += 1) {
        var x = rect.x0;
        while (x < rect.x1) : (x += 1) {
            const dx = x - cx;
            const dy = y - cy;
            const d2 = dx * dx + dy * dy;
            if (hover and d2 <= outer_r2) {
                setSurfacePixel(surface, x, y, close_btn_glow_color);
            }
            if (d2 <= inner_r2) {
                setSurfacePixel(surface, x, y, bg);
            }
        }
    }

    const cross_r = inner_r - 3;
    if (cross_r <= 0) return;
    var i: i32 = -cross_r;
    while (i <= cross_r) : (i += 1) {
        setSurfacePixel(surface, cx + i, cy + i, close_btn_cross_color);
        setSurfacePixel(surface, cx + i + 1, cy + i, close_btn_cross_color);
        setSurfacePixel(surface, cx - i, cy + i, close_btn_cross_color);
        setSurfacePixel(surface, cx - i - 1, cy + i, close_btn_cross_color);
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
    win: *const WindowState,
    title: []const u8,
    close_hover: bool,
    close_down: bool,
) void {
    drawWindowShadow(back, win);
    drawSolidRect(back, win.x, win.y, windowWidth(win), windowHeight(win), window_border_color);
    drawWindowHeaderBackground(back, win);
    drawSolidRect(back, contentOriginX(win), contentOriginY(win), windowContentW(win), windowContentH(win), window_content_bg_color);
    drawWindowHeaderDivider(back, win);

    const close_right = win.x + windowWidth(win) - window_close_margin_i32;
    const close_left = close_right - window_close_size_i32;
    const close_top = win.y + @divTrunc(window_header_h_i32 - window_close_size_i32, 2);
    const close_rect: Rect = .{
        .x0 = close_left,
        .y0 = close_top,
        .x1 = close_right,
        .y1 = close_top + window_close_size_i32,
    };
    const title_left = win.x + 8;
    const title_right = win.x + windowWidth(win) - 8;
    const title_width = textPixelWidth(title);
    const centered_title_x = title_left + @divTrunc((title_right - title_left) - title_width, 2);
    const title_x = if (centered_title_x > title_left) centered_title_x else title_left;
    const title_clip_right = close_left - 6;
    const title_y = win.y + @divTrunc(window_header_h_i32 - font.lineHeight(1), 2) + window_title_y_bias;
    if (title.len > 0 and title_x < title_clip_right) {
        var title_buf: [window_title_max_bytes + 3]u8 = undefined;
        const fitted_title = fitTextWithEllipsis(title, title_clip_right - title_x, title_buf[0..]);
        if (fitted_title.len > 0) {
            drawTextSubpixelClipped(back, title_x, title_y, fitted_title, window_header_text_color, title_clip_right);
            drawTextSubpixelClipped(back, title_x + 1, title_y, fitted_title, window_header_text_color, title_clip_right);
        }
    }
    drawCloseButton(back, close_rect, close_hover, close_down);
    applyRoundedCornerCut(back, win);
}

fn drawWindowChromeSurface(
    surface: *const DrawSurface,
    win: *const WindowState,
    title: []const u8,
    close_hover: bool,
    close_down: bool,
) void {
    drawWindowShadowSurface(surface, win);
    drawSolidRectSurface(surface, win.x, win.y, windowWidth(win), windowHeight(win), window_border_color);
    drawWindowHeaderBackgroundSurface(surface, win);
    drawSolidRectSurface(surface, contentOriginX(win), contentOriginY(win), windowContentW(win), windowContentH(win), window_content_bg_color);
    drawWindowHeaderDividerSurface(surface, win);

    const close_right = win.x + windowWidth(win) - window_close_margin_i32;
    const close_left = close_right - window_close_size_i32;
    const close_top = win.y + @divTrunc(window_header_h_i32 - window_close_size_i32, 2);
    const close_rect: Rect = .{
        .x0 = close_left,
        .y0 = close_top,
        .x1 = close_right,
        .y1 = close_top + window_close_size_i32,
    };
    const title_left = win.x + 8;
    const title_right = win.x + windowWidth(win) - 8;
    const title_width = textPixelWidth(title);
    const centered_title_x = title_left + @divTrunc((title_right - title_left) - title_width, 2);
    const title_x = if (centered_title_x > title_left) centered_title_x else title_left;
    const title_clip_right = close_left - 6;
    const title_y = win.y + @divTrunc(window_header_h_i32 - font.lineHeight(1), 2) + window_title_y_bias;
    if (title.len > 0 and title_x < title_clip_right) {
        var title_buf: [window_title_max_bytes + 3]u8 = undefined;
        const fitted_title = fitTextWithEllipsis(title, title_clip_right - title_x, title_buf[0..]);
        if (fitted_title.len > 0) {
            drawTextSubpixelClippedSurface(surface, title_x, title_y, fitted_title, window_header_text_color, title_clip_right);
            drawTextSubpixelClippedSurface(surface, title_x + 1, title_y, fitted_title, window_header_text_color, title_clip_right);
        }
    }
    drawCloseButtonSurface(surface, close_rect, close_hover, close_down);
    applyRoundedCornerCutSurface(surface, win);
}

fn blitScaledPixelToWindow(back: [*]u32, win: *const WindowState, local_vx: usize, local_vy: usize, color: u32) Rect {
    const x0 = contentOriginX(win) + @as(i32, @intCast(local_vx * window_scale));
    const y0 = contentOriginY(win) + @as(i32, @intCast(local_vy * window_scale));
    var sy: i32 = 0;
    while (sy < window_scale_i32) : (sy += 1) {
        var sx: i32 = 0;
        while (sx < window_scale_i32) : (sx += 1) {
            setBackPixel(back, x0 + sx, y0 + sy, color);
        }
    }
    return .{ .x0 = x0, .y0 = y0, .x1 = x0 + window_scale_i32, .y1 = y0 + window_scale_i32 };
}

fn blitScaledPixelToWindowSurface(surface: *const DrawSurface, win: *const WindowState, local_vx: usize, local_vy: usize, color: u32) Rect {
    const x0 = contentOriginX(win) + @as(i32, @intCast(local_vx * window_scale));
    const y0 = contentOriginY(win) + @as(i32, @intCast(local_vy * window_scale));
    var sy: i32 = 0;
    while (sy < window_scale_i32) : (sy += 1) {
        var sx: i32 = 0;
        while (sx < window_scale_i32) : (sx += 1) {
            setSurfacePixel(surface, x0 + sx, y0 + sy, color);
        }
    }
    return .{ .x0 = x0, .y0 = y0, .x1 = x0 + window_scale_i32, .y1 = y0 + window_scale_i32 };
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

    const safe_count = max_windows;
    var i: usize = 0;
    while (i < safe_count) : (i += 1) {
        const slot = &store.slots[i];
        const win = &slot.frame;
        if (!win.active or !win.visible) continue;
        const title = if (win.title_len > 0) win.title[0..win.title_len] else "Window";
        drawWindowChrome(back, win, title, slot.close_hover, slot.close_down);
        blitWindowSourceToWindow(back, i);
    }
    drawMouseCoordHud(back, mouse_x, mouse_y);
}

fn recomputeWindowCount() usize {
    return window_store.visibleCount();
}

fn findWindowSlotById(window_id: u32) ?usize {
    return window_store.findById(window_id);
}

fn findFreeWindowSlot() ?usize {
    return window_store.findFree();
}

fn updateWindowTitleFromMeta(slot: usize) void {
    if (slot >= max_windows) return;
    const slot_ref = &window_store.slots[slot];
    if (!slot_ref.source.active) return;
    const meta_va = slot_ref.source.meta_va;
    const map_lo = window_map_base_va;
    const map_hi = window_map_base_va + max_windows * window_map_stride_va;
    if (meta_va < map_lo or meta_va >= map_hi) return;
    const meta: *const volatile WindowMeta = @ptrFromInt(meta_va);
    if (meta.magic != window_meta_magic) return;
    if (meta.version != protocol.window_protocol_version) return;
    const requested_len: usize = if (meta.title_len < window_title_max_bytes) meta.title_len else window_title_max_bytes;
    var i: usize = 0;
    while (i < requested_len) : (i += 1) {
        slot_ref.frame.title[i] = sanitizeTitleByte(meta.title[i]);
    }
    slot_ref.frame.title_len = requested_len;
}

fn windowBounds(win: *const WindowState) Rect {
    return clipRectToScreen(.{
        .x0 = win.x,
        .y0 = win.y,
        .x1 = win.x + windowWidth(win) + window_shadow_right_extent,
        .y1 = win.y + windowHeight(win) + window_shadow_bottom_extent,
    });
}

fn screenRectForSourceRegion(win: *const WindowState, region: SourceRegion) Rect {
    const x0 = contentOriginX(win) + @as(i32, @intCast(region.x * window_scale));
    const y0 = contentOriginY(win) + @as(i32, @intCast(region.y * window_scale));
    const x1 = x0 + @as(i32, @intCast(region.w * window_scale));
    const y1 = y0 + @as(i32, @intCast(region.h * window_scale));
    return clipRectToScreen(.{ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1 });
}

fn registerWindowCap(page_paddr: u64) ?Rect {
    const cap: *const volatile WindowCap = @ptrFromInt(ipc_rx_page_va);
    if (cap.magic != window_cap_magic or cap.version != 1) return null;
    const rights = protocol.decodeWindowRights(cap.rights_bits);
    if (!rights.dma_pixels) {
        _ = userLog("Compositor: reject window cap (dma_pixels=0)\n");
        return null;
    }
    if (cap.width == 0 or cap.height == 0) return null;
    if (cap.pixels_page_count != 1) {
        _ = userLog("Compositor: pixels_page_count!=1 unsupported\n");
        return null;
    }
    if (cap.pixels_cap_paddr < 0x1000 or cap.meta_cap_paddr < 0x1000) return null;

    const cap_snapshot = snapshotWindowCap(cap);

    const existing = findWindowSlotById(cap_snapshot.window_id);
    const slot = existing orelse (findFreeWindowSlot() orelse return null);
    const slot_base = window_map_base_va + slot * window_map_stride_va;
    const map_meta_va = slot_base + 0x1000;
    const map_pixel_va = slot_base + 0x2000;

    if (mapPage(map_meta_va, cap_snapshot.meta_cap_paddr, false) != syscall_ok) return null;
    if (mapPage(map_pixel_va, cap_snapshot.pixels_cap_paddr, false) != syscall_ok) return null;

    const slot_ref = &window_store.slots[slot];
    slot_ref.reset();
    slot_ref.source = .{
        .active = true,
        .window_id = cap_snapshot.window_id,
        .pixel_va = map_pixel_va,
        .pixels_paddr = cap_snapshot.pixels_cap_paddr,
        .pixels_page_count = cap_snapshot.pixels_page_count,
        .meta_va = map_meta_va,
        .width = cap_snapshot.width,
        .height = cap_snapshot.height,
        .pitch = cap_snapshot.pitch,
    };

    const frame_ptr: *volatile WindowState = &slot_ref.frame;
    frame_ptr.active = true;
    frame_ptr.visible = true;
    frame_ptr.src.x = 0;
    frame_ptr.src.y = 0;
    frame_ptr.src.w = cap_snapshot.width;
    frame_ptr.src.h = cap_snapshot.height;
    frame_ptr.x = @as(i32, @intCast(72 + (slot % 2) * 220));
    frame_ptr.y = @as(i32, @intCast(110 + (slot / 2) * 230));
    frame_ptr.drag_off_x = 0;
    frame_ptr.drag_off_y = 0;
    frame_ptr.prev_close_hover = false;
    frame_ptr.prev_close_down = false;
    frame_ptr.title_len = 0;
    updateWindowTitleFromMeta(slot);
    window_shadow_valid[slot] = false;
    window_gpu_resources[slot] = null;
    _ = userLog("Compositor: registerWindowCap done\n");

    _ = page_paddr;
    return windowBounds(&slot_ref.frame);
}

fn blitWindowSourceToWindow(back: [*]u32, slot: usize) void {
    const slot_ref = &window_store.slots[slot];
    const src = &slot_ref.source;
    if (!src.active) return;
    const win = &slot_ref.frame;
    if (!win.active or !win.visible) return;
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
    if (slot < max_windows) {
        if (window_gpu_resources[slot]) |resource| {
            if (resource.ready) return resource.pixels;
        }
    }
    return @ptrFromInt(window_store.slots[slot].source.pixel_va);
}

fn blitWindowSourceToWindowSurface(surface: *const DrawSurface, slot: usize) void {
    const slot_ref = &window_store.slots[slot];
    const src = &slot_ref.source;
    if (!src.active) return;
    const win = &slot_ref.frame;
    if (!win.active or !win.visible) return;

    const content_x0 = contentOriginX(win);
    const content_y0 = contentOriginY(win);
    const content_rect: Rect = .{
        .x0 = content_x0,
        .y0 = content_y0,
        .x1 = content_x0 + @as(i32, @intCast(src.width * window_scale)),
        .y1 = content_y0 + @as(i32, @intCast(src.height * window_scale)),
    };
    const clipped = intersectRect(content_rect, surface.clip) orelse return;

    const vx_start: i32 = clampI32(@divFloor(clipped.x0 - content_x0, window_scale_i32), 0, @intCast(src.width));
    const vy_start: i32 = clampI32(@divFloor(clipped.y0 - content_y0, window_scale_i32), 0, @intCast(src.height));
    const vx_end: i32 = clampI32(@divFloor((clipped.x1 - 1) - content_x0, window_scale_i32) + 1, 0, @intCast(src.width));
    const vy_end: i32 = clampI32(@divFloor((clipped.y1 - 1) - content_y0, window_scale_i32) + 1, 0, @intCast(src.height));
    if (vx_start >= vx_end or vy_start >= vy_end) return;

    const src_pixels = windowPixelsPtr(slot);
    var vy: i32 = vy_start;
    while (vy < vy_end) : (vy += 1) {
        const vy_u: usize = @intCast(vy);
        const row_start_y = content_y0 + vy * window_scale_i32;
        const row_end_y = row_start_y + window_scale_i32;
        const py0 = if (row_start_y > clipped.y0) row_start_y else clipped.y0;
        const py1 = if (row_end_y < clipped.y1) row_end_y else clipped.y1;
        if (py0 >= py1) continue;

        var vx: i32 = vx_start;
        while (vx < vx_end) : (vx += 1) {
            const vx_u: usize = @intCast(vx);
            const src_index = vy_u * src.pitch + vx_u;
            const color = src_pixels[src_index];
            const col_start_x = content_x0 + vx * window_scale_i32;
            const col_end_x = col_start_x + window_scale_i32;
            const px0 = if (col_start_x > clipped.x0) col_start_x else clipped.x0;
            const px1 = if (col_end_x < clipped.x1) col_end_x else clipped.x1;
            if (px0 >= px1) continue;

            var py: i32 = py0;
            while (py < py1) : (py += 1) {
                const row: usize = @as(usize, @intCast(py)) * fb_pitch;
                var px: i32 = px0;
                while (px < px1) : (px += 1) {
                    surface.pixels[row + @as(usize, @intCast(px))] = color;
                }
            }
        }
    }
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
    var i: usize = 0;
    while (i < max_windows) : (i += 1) {
        const slot = &store.slots[i];
        const win = &slot.frame;
        if (!win.active or !win.visible) continue;
        if (intersectRect(windowBounds(win), surface.clip) == null) continue;
        const title = if (win.title_len > 0) win.title[0..win.title_len] else "Window";
        // Keep per-window ordering (chrome -> content) to preserve layer correctness.
        drawWindowChromeSurface(surface, win, title, slot.close_hover, slot.close_down);
        blitWindowSourceToWindowSurface(surface, i);
    }
}

fn redrawSceneSurface(
    surface: *const DrawSurface,
    store: *const WindowStore,
    mouse_x: i32,
    mouse_y: i32,
    draw_cursor: bool,
) void {
    redrawSceneBackgroundHud(surface, mouse_x, mouse_y);
    redrawSceneWindowsLayered(surface, store);
    if (draw_cursor) drawCursorSurface(surface, mouse_x, mouse_y);
}

fn ensureWindowGpuResource(slot: usize) ?*virtgpu.Resource {
    if (slot >= max_windows) return null;
    const slot_ref = &window_store.slots[slot];
    if (!slot_ref.source.active) return null;
    if (window_gpu_resources[slot]) |resource| {
        if (resource.ready and resource.width == slot_ref.source.width and resource.height == slot_ref.source.height) return resource;
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

fn syncWindowGpuContent(slot: usize) ?Rect {
    if (slot >= max_windows) return null;
    const slot_ref = &window_store.slots[slot];
    if (!slot_ref.source.active or !slot_ref.frame.active or !slot_ref.frame.visible) return null;

    const src = &slot_ref.source;
    const src_pixels: [*]const volatile u32 = @ptrFromInt(src.pixel_va);
    const resource = ensureWindowGpuResource(slot);
    const shadow = &window_shadow_storage[slot];
    var dirty_any = false;
    var dirty_region: SourceRegion = .{};

    var vy: usize = 0;
    while (vy < src.height) : (vy += 1) {
        var vx: usize = 0;
        while (vx < src.width) : (vx += 1) {
            const index = vy * src.pitch + vx;
            const color = src_pixels[index];
            if (window_shadow_valid[slot] and shadow[index] == color) continue;
            shadow[index] = color;
            if (!dirty_any) {
                dirty_any = true;
                dirty_region = .{ .x = vx, .y = vy, .w = 1, .h = 1 };
            } else {
                const x0 = if (vx < dirty_region.x) vx else dirty_region.x;
                const y0 = if (vy < dirty_region.y) vy else dirty_region.y;
                const x1 = if (vx + 1 > dirty_region.x + dirty_region.w) vx + 1 else dirty_region.x + dirty_region.w;
                const y1 = if (vy + 1 > dirty_region.y + dirty_region.h) vy + 1 else dirty_region.y + dirty_region.h;
                dirty_region = .{ .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
            }
        }
    }

    window_shadow_valid[slot] = true;
    if (!dirty_any) return null;
    if (resource) |res| {
        const rect: virtgpu.Rect = .{
            .x = @intCast(dirty_region.x),
            .y = @intCast(dirty_region.y),
            .width = @intCast(dirty_region.w),
            .height = @intCast(dirty_region.h),
        };
        _ = virtgpu.virtgpu_transfer(res, rect);
        _ = virtgpu.virtgpu_flush_rect(res, rect);
    }
    return screenRectForSourceRegion(&slot_ref.frame, dirty_region);
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
    window_gpu_resources = [_]?*virtgpu.Resource{null} ** max_windows;
    var last_cursor_rect = cursorRect(mouse_state_storage.x, mouse_state_storage.y);
    var i: usize = 0;

    var window_count: usize = 0;
    var first_compose_wait_loops: u32 = 0;

    var dragging_index: ?usize = null;
    var prev_left_down = false;
    var force_full = true;
    var virtgpu_init_ok = false;

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
            _ = userLog("Compositor: window cap recv\n");
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

        if (grantCap(process0_id, page_paddr, rights_read_write) != syscall_ok) {
            _ = userLog("Compositor: grant shared cap back failed\n");
            while (true) asm volatile ("pause");
        }
        force_full = true;
    }

    var gpu_resource: ?*virtgpu.Resource = null;
    var virtgpu_active = false;
    var hardware_cursor_active = false;
    if (virtgpu_init_ok) {
        gpu_resource = virtgpu.virtgpu_create_fb(fb_width, fb_height);
        if (gpu_resource) |resource| {
            if (virtgpu.virtgpu_set_scanout(resource)) {
                virtgpu_active = true;
                hardware_cursor_active = initHardwareCursor(mouse_state_storage.x, mouse_state_storage.y);
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
                _ = userLog("Compositor: window cap recv\n");
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
            if (grantCap(process0_id, page_paddr, rights_read_write) != syscall_ok) {
                _ = userLog("Compositor: grant shared cap back failed\n");
                while (true) asm volatile ("pause");
            }
            force_full = true;
        }

        // Boot fast-path: until at least one window is registered, avoid the
        // expensive compose/present path and keep polling mailbox quickly.
        if (recomputeWindowCount() == 0) {
            force_full = false;
            asm volatile ("pause");
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
        const safe_window_count = max_windows;

        if (!first_compose_logged and window_count < 2 and first_compose_wait_loops < 96) {
            first_compose_wait_loops += 1;
            force_full = true;
            asm volatile ("pause");
            continue;
        }

        i = 0;
        while (i < max_windows) : (i += 1) {
            window_store.slots[i].close_hover = false;
            window_store.slots[i].close_down = false;
        }

        i = 0;
        while (i < safe_window_count) : (i += 1) {
            const slot_ref = &window_store.slots[i];
            const win = &slot_ref.frame;
            if (!win.active or !win.visible) continue;
            const close_right = win.x + windowWidth(win) - window_close_margin_i32;
            const close_left = close_right - window_close_size_i32;
            const close_top = win.y + @divTrunc(window_header_h_i32 - window_close_size_i32, 2);
            const close_rect: Rect = .{
                .x0 = close_left,
                .y0 = close_top,
                .x1 = close_right,
                .y1 = close_top + window_close_size_i32,
            };
            slot_ref.close_hover = pointInCloseButton(mouse_state_storage.x, mouse_state_storage.y, close_rect);
            slot_ref.close_down = slot_ref.close_hover and left_down;
            if (slot_ref.close_hover != win.prev_close_hover or slot_ref.close_down != win.prev_close_down) {
                const merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), old_bounds[i]);
                dirty_any = merged.any;
                dirty_rect = merged.rect;
            }
        }

        if (!prev_left_down and left_down) {
            var hit: ?usize = null;
            var j: usize = safe_window_count;
            while (j > 0) {
                const idx = j - 1;
                const win = &window_store.slots[idx].frame;
                const wx0 = win.x;
                const wy0 = win.y;
                const wx1 = win.x + windowWidth(win);
                const wy1 = win.y + windowHeight(win);
                if (win.active and win.visible and mouse_state_storage.x >= wx0 and mouse_state_storage.x < wx1 and mouse_state_storage.y >= wy0 and mouse_state_storage.y < wy1) {
                    hit = idx;
                    break;
                }
                j -= 1;
            }

            if (hit) |idx| {
                const slot_ref = &window_store.slots[idx];
                var win = &slot_ref.frame;
                if (slot_ref.close_hover) {
                    win.visible = false;
                    if (dragging_index != null and dragging_index.? == idx) dragging_index = null;
                    const merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), old_bounds[idx]);
                    dirty_any = merged.any;
                    dirty_rect = merged.rect;
                } else {
                    const hx0 = win.x + window_border_i32;
                    const hy0 = win.y + window_border_i32;
                    const hx1 = win.x + windowWidth(win) - window_border_i32;
                    const hy1 = win.y + window_header_h_i32;
                    if (!(mouse_state_storage.x >= hx0 and mouse_state_storage.x < hx1 and mouse_state_storage.y >= hy0 and mouse_state_storage.y < hy1)) {
                        continue;
                    }
                    win.drag_off_x = mouse_state_storage.x - win.x;
                    win.drag_off_y = mouse_state_storage.y - win.y;
                    dragging_index = idx;
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
        prev_left_down = left_down;

        i = 0;
        while (i < safe_window_count) : (i += 1) {
            window_store.slots[i].frame.prev_close_hover = window_store.slots[i].close_hover;
            window_store.slots[i].frame.prev_close_down = window_store.slots[i].close_down;
        }

        i = 0;
        while (i < safe_window_count) : (i += 1) {
            if (syncWindowGpuContent(i)) |changed_rect| {
                const merged = includeRect(dirty_any, if (dirty_any) dirty_rect else fullScreenRect(), changed_rect);
                dirty_any = merged.any;
                dirty_rect = merged.rect;
            }
        }

        const present_rect = if (force_full) fullScreenRect() else clipRectToScreen(dirty_rect);
        if (force_full or dirty_any) {
            if (rectIsEmpty(present_rect)) {
                force_full = false;
                last_cursor_rect = cursorRect(mouse_state_storage.x, mouse_state_storage.y);
                asm volatile ("pause");
                continue;
            }
        } else {
            last_cursor_rect = cursorRect(mouse_state_storage.x, mouse_state_storage.y);
            asm volatile ("pause");
            continue;
        }

        if (virtgpu_active and gpu_resource != null) {
            const surface = DrawSurface{
                .pixels = gpu_resource.?.pixels,
                .clip = present_rect,
            };
            const compose_start = readTsc();
            redrawSceneSurface(
                &surface,
                &window_store,
                mouse_state_storage.x,
                mouse_state_storage.y,
                !hardware_cursor_active,
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
                _ = userLog("Compositor: present transfer done\n");
            }
            const flush_ok = transfer_ok and virtgpu.virtgpu_flush_rect(gpu_resource.?, transfer_rect);
            const submit_end = readTsc();
            logComposePerf(compose_end - compose_start, submit_end - submit_start, present_rect);
            if (flush_ok and !first_present_flush_logged) {
                first_present_flush_logged = true;
                _ = userLog("Compositor: present flush done\n");
            }
            if (!flush_ok) {
                _ = userLog("GpuCompositor: virtgpu present failed\n");
                while (true) asm volatile ("pause");
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
        asm volatile ("pause");
    }
}
