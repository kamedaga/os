const syscall_map_page: u64 = 0x2;
const syscall_grant_cap: u64 = 0x8;
const syscall_log: u64 = 0x9;
const syscall_recv_cap: u64 = 0xA;

const syscall_ok: u64 = 0;
const syscall_err_empty: u64 = 13;

const shared_page_va: usize = 0x3C00_3000;
const window_meta_va: usize = 0x3C00_7000;
const framebuffer_va: usize = 0x3C00_5000;
const virtual_framebuffer_va: usize = 0x3C00_4000;
const ipc_rx_page_va: usize = 0x201F_F000;

const shared_magic: u64 = 0x4D534852; // "MSHR"
const window_cap_magic: u32 = 0x57434150; // 'WCAP'
const window_meta_magic: u32 = 0x574D5441; // 'WMTA'
const process0_id: u64 = 0;
const rights_read_write: u64 = 0x3;

const fb_width: usize = 832;
const fb_height: usize = 624;
const fb_pitch: usize = 832;
const fb_pixels: usize = fb_pitch * fb_height;
const fb_width_i32: i32 = @as(i32, @intCast(fb_width));
const fb_height_i32: i32 = @as(i32, @intCast(fb_height));

const vfb_width: usize = 32;
const vfb_height: usize = 32;
const vfb_pitch: usize = 32;
const vfb_pixels: usize = vfb_pitch * vfb_height;

const max_windows: usize = 4;

const background_color: u32 = 0x00FD_FDFD;
const window_border_color: u32 = 0x00D8_D7D6;
const window_header_color: u32 = 0x00F6_F5F4;
const window_header_text_color: u32 = 0x00D7_D7D6;
const window_content_bg_color: u32 = 0x00FF_FFFF;
const close_btn_color: u32 = 0x00E9_E8E7;
const close_btn_hover_color: u32 = 0x00DF_DEDD;
const close_btn_down_color: u32 = 0x00D2_D1D0;
const close_btn_cross_color: u32 = 0x006E_6D6C;

const window_scale: usize = 10;
const window_border: usize = 2;
const window_header_h: usize = 28;
const window_close_size: usize = 14;
const window_close_margin: usize = 6;
const window_corner_cut: usize = 2;
const window_border_i32: i32 = @as(i32, @intCast(window_border));
const window_header_h_i32: i32 = @as(i32, @intCast(window_header_h));
const window_close_size_i32: i32 = @as(i32, @intCast(window_close_size));
const window_close_margin_i32: i32 = @as(i32, @intCast(window_close_margin));
const window_scale_i32: i32 = @as(i32, @intCast(window_scale));

const window_title_offset: usize = 16;
const window_title_max_bytes: usize = 64;
const window_map_base_va: usize = 0x2021_0000;
const window_map_stride_va: usize = 0x3000;

var shadow_storage: [vfb_pixels]u32 align(64) = [_]u32{0} ** vfb_pixels;
var back_buffer_storage: [fb_pixels]u32 align(64) = [_]u32{0} ** fb_pixels;
var shared_title_seq: u64 = 0;
var shared_title_buf: [window_title_max_bytes]u8 = [_]u8{0} ** window_title_max_bytes;
var shared_title_len: usize = 0;
var windows_storage: [max_windows]WindowState = undefined;
var detected_regions_storage: [max_windows]SourceRegion = undefined;
var last_regions_storage: [max_windows]SourceRegion = undefined;
var close_hover_storage: [max_windows]bool = [_]bool{false} ** max_windows;
var close_down_storage: [max_windows]bool = [_]bool{false} ** max_windows;

const WindowCap = packed struct {
    magic: u32,
    version: u16,
    rights_bits: u16,
    window_id: u32,
    owner_pid: u32,
    vfb_cap_paddr: u64,
    meta_cap_paddr: u64,
    vfb_size_bytes: u32,
    vfb_page_count: u16,
    pixels_per_scan_line: u16,
    pixel_format: u32,
    evt_cap_paddr: u64,
    width: u16,
    height: u16,
    min_width: u16,
    min_height: u16,
    flags: u32,
    z_hint: i32,
    reserved0: u32,
};

const WindowMeta = extern struct {
    magic: u32,
    version: u16,
    state: u16,
    seq: u64,
    pos_x: i32,
    pos_y: i32,
    width: u16,
    height: u16,
    title_len: u16,
    title: [64]u8,
};

const WindowSource = struct {
    active: bool = false,
    window_id: u32 = 0,
    vfb_va: usize = 0,
    meta_va: usize = 0,
    width: usize = 0,
    height: usize = 0,
    pitch: usize = 0,
};

var window_sources_storage: [max_windows]WindowSource = [_]WindowSource{.{}} ** max_windows;

const cursor_width: usize = 15;
const cursor_height: usize = 24;
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
    "@......@@@@@@@@" .*,
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

const Rect = struct {
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
};

const SourceRegion = struct {
    x: usize,
    y: usize,
    w: usize,
    h: usize,
};

const WindowState = struct {
    active: bool,
    visible: bool,
    src: SourceRegion,
    x: i32,
    y: i32,
    drag_off_x: i32,
    drag_off_y: i32,
    prev_close_hover: bool,
    prev_close_down: bool,
    title: [window_title_max_bytes]u8,
    title_len: usize,
};

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .memory = true });
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .memory = true });
}

fn recvCap() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_recv_cap),
        : .{ .memory = true });
}

fn grantCap(to_process: u64, paddr: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (to_process),
          [arg2] "{rdx}" (rights_bits),
        : .{ .memory = true });
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

fn setBackPixel(back: [*]u32, x: i32, y: i32, color: u32) void {
    if (x < 0 or y < 0 or x >= fb_width_i32 or y >= fb_height_i32) return;
    const ux: usize = @intCast(x);
    const uy: usize = @intCast(y);
    back[uy * fb_pitch + ux] = color;
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

fn cursorRect(cx: i32, cy: i32) Rect {
    return .{
        .x0 = cx,
        .y0 = cy,
        .x1 = cx + cursor_width_i32,
        .y1 = cy + cursor_height_i32,
    };
}

fn copyRectBackToFbWithCursor(
    back_buffer: [*]u32,
    fb: [*]volatile u32,
    rect: Rect,
    cx: i32,
    cy: i32,
    buttons: u64,
) void {
    var x0 = rect.x0;
    var y0 = rect.y0;
    var x1 = rect.x1;
    var y1 = rect.y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_width_i32) x1 = fb_width_i32;
    if (y1 > fb_height_i32) y1 = fb_height_i32;
    if (x0 >= x1 or y0 >= y1) return;

    _ = cx;
    _ = cy;
    _ = buttons;
    const fill_color: u32 = 0x00FF_FFFF;
    const cursor_x0 = @divTrunc(fb_width_i32 - cursor_width_i32, 2);
    const cursor_y0 = @divTrunc(fb_height_i32 - cursor_height_i32, 2);
    const cursor_x1 = cursor_x0 + cursor_width_i32;
    const cursor_y1 = cursor_y0 + cursor_height_i32;

    var y: i32 = y0;
    while (y < y1) : (y += 1) {
        const row: usize = @as(usize, @intCast(y)) * fb_pitch;
        var x: i32 = x0;
        while (x < x1) : (x += 1) {
            var out = back_buffer[row + @as(usize, @intCast(x))];
            if (x >= cursor_x0 and x < cursor_x1 and y >= cursor_y0 and y < cursor_y1) {
                const bit_y: usize = @intCast(y - cursor_y0);
                const bit_x: usize = @intCast(x - cursor_x0);
                const ch = cursor_shape[bit_y][bit_x];
                if (ch == '@') {
                    out = 0x0000_0000;
                } else if (ch == '.') {
                    out = fill_color;
                }
            }
            fb[row + @as(usize, @intCast(x))] = out;
        }
    }
}

fn clearBackBuffer(back: [*]u32, color: u32) void {
    var i: usize = 0;
    while (i < fb_pixels) : (i += 1) {
        back[i] = color;
    }
}

fn glyphRows(raw: u8) [7]u8 {
    const ch: u8 = if (raw >= 'a' and raw <= 'z') raw - 32 else raw;
    return switch (ch) {
        'A' => .{ 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
        'B' => .{ 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
        'C' => .{ 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
        'D' => .{ 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
        'E' => .{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
        'F' => .{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
        'G' => .{ 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E },
        'H' => .{ 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
        'I' => .{ 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F },
        'J' => .{ 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E },
        'K' => .{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
        'L' => .{ 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
        'M' => .{ 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
        'N' => .{ 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
        'O' => .{ 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        'P' => .{ 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
        'Q' => .{ 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
        'R' => .{ 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
        'S' => .{ 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
        'T' => .{ 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
        'U' => .{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        'V' => .{ 0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04 },
        'W' => .{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A },
        'X' => .{ 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
        'Y' => .{ 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
        'Z' => .{ 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
        '0' => .{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
        '1' => .{ 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
        '2' => .{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
        '3' => .{ 0x1E, 0x01, 0x01, 0x06, 0x01, 0x01, 0x1E },
        '4' => .{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
        '5' => .{ 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E },
        '6' => .{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
        '7' => .{ 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
        '8' => .{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
        '9' => .{ 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C },
        ' ' => .{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        '-' => .{ 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00 },
        '_' => .{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F },
        '.' => .{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 },
        else => .{ 0x0E, 0x11, 0x01, 0x06, 0x04, 0x00, 0x04 },
    };
}

fn drawGlyphBack(back: [*]u32, x: i32, y: i32, ch: u8, color: u32) void {
    const glyph = glyphRows(ch);
    var gy: usize = 0;
    while (gy < 7) : (gy += 1) {
        const bits = glyph[gy];
        var gx: usize = 0;
        while (gx < 5) : (gx += 1) {
            if ((bits & (@as(u8, 1) << @intCast(4 - gx))) == 0) continue;
            setBackPixel(back, x + @as(i32, @intCast(gx)), y + @as(i32, @intCast(gy)), color);
        }
    }
}

fn drawTextClipped(back: [*]u32, x: i32, y: i32, text: []const u8, color: u32, max_x: i32) void {
    var pen_x = x;
    var i: usize = 0;
    while (i < text.len) : (i += 1) {
        if (pen_x + 5 > max_x) break;
        drawGlyphBack(back, pen_x, y, text[i], color);
        pen_x += 6;
    }
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
    const hud_h: i32 = 18;
    drawSolidRect(back, hud_x, hud_y, hud_w, hud_h, 0x00F1_F0EF);
    drawTextClipped(back, hud_x + 5, hud_y + 5, text, 0x0030_3030, hud_x + hud_w - 4);
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
    const bg = if (down) close_btn_down_color else if (hover) close_btn_hover_color else close_btn_color;
    const cx = @divTrunc(rect.x0 + rect.x1 - 1, 2);
    const cy = @divTrunc(rect.y0 + rect.y1 - 1, 2);
    const r = @divTrunc(rect.x1 - rect.x0, 2);
    const r2 = r * r;

    var y = rect.y0;
    while (y < rect.y1) : (y += 1) {
        var x = rect.x0;
        while (x < rect.x1) : (x += 1) {
            const dx = x - cx;
            const dy = y - cy;
            if (dx * dx + dy * dy <= r2) {
                setBackPixel(back, x, y, bg);
            }
        }
    }

    const cross_r = r - 3;
    if (cross_r <= 0) return;
    var i: i32 = -cross_r;
    while (i <= cross_r) : (i += 1) {
        setBackPixel(back, cx + i, cy + i, close_btn_cross_color);
        setBackPixel(back, cx - i, cy + i, close_btn_cross_color);
    }
}

fn applyRoundedCornerCut(back: [*]u32, win: *const WindowState) void {
    if (window_corner_cut < 2) return;
    const w = windowWidth(win);
    const h = windowHeight(win);
    const x = win.x;
    const y = win.y;

    setBackPixel(back, x, y, background_color);
    setBackPixel(back, x + 1, y, background_color);
    setBackPixel(back, x, y + 1, background_color);

    setBackPixel(back, x + w - 1, y, background_color);
    setBackPixel(back, x + w - 2, y, background_color);
    setBackPixel(back, x + w - 1, y + 1, background_color);

    setBackPixel(back, x, y + h - 1, background_color);
    setBackPixel(back, x + 1, y + h - 1, background_color);
    setBackPixel(back, x, y + h - 2, background_color);

    setBackPixel(back, x + w - 1, y + h - 1, background_color);
    setBackPixel(back, x + w - 2, y + h - 1, background_color);
    setBackPixel(back, x + w - 1, y + h - 2, background_color);
}

fn drawWindowChrome(
    back: [*]u32,
    win: *const WindowState,
    title: []const u8,
    close_hover: bool,
    close_down: bool,
) void {
    drawSolidRect(back, win.x, win.y, windowWidth(win), windowHeight(win), window_border_color);
    drawSolidRect(
        back,
        win.x + window_border_i32,
        win.y + window_border_i32,
        windowWidth(win) - window_border_i32 * 2,
        window_header_h_i32 - window_border_i32,
        window_header_color,
    );
    drawSolidRect(back, contentOriginX(win), contentOriginY(win), windowContentW(win), windowContentH(win), window_content_bg_color);

    const close_right = win.x + windowWidth(win) - window_close_margin_i32;
    const close_left = close_right - window_close_size_i32;
    const close_top = win.y + @divTrunc(window_header_h_i32 - window_close_size_i32, 2);
    const close_rect: Rect = .{
        .x0 = close_left,
        .y0 = close_top,
        .x1 = close_right,
        .y1 = close_top + window_close_size_i32,
    };
    const title_max_x = close_rect.x0 - 6;
    drawTextClipped(back, win.x + 10, win.y + 9, title, window_header_text_color, title_max_x);
    drawCloseButton(back, close_rect, close_hover, close_down);
    applyRoundedCornerCut(back, win);
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

fn blitShadowRegionToWindow(back: [*]u32, shadow: [*]u32, win: *const WindowState) void {
    var vy: usize = 0;
    while (vy < win.src.h) : (vy += 1) {
        var vx: usize = 0;
        while (vx < win.src.w) : (vx += 1) {
            const src_index = (win.src.y + vy) * vfb_pitch + win.src.x + vx;
            _ = blitScaledPixelToWindow(back, win, vx, vy, shadow[src_index]);
        }
    }
}

fn copyTitle(dst: *[window_title_max_bytes]u8, dst_len: *usize, src: []const u8) void {
    const n: usize = if (src.len < window_title_max_bytes - 1) src.len else window_title_max_bytes - 1;
    var i: usize = 0;
    while (i < n) : (i += 1) {
        dst[i] = src[i];
    }
    dst_len.* = n;
}

fn setDefaultWindowTitle() void {
    copyTitle(&shared_title_buf, &shared_title_len, "Mouse Demo");
}

fn setIndexedTitle(win: *WindowState, index: usize) void {
    const label = switch (index) {
        0 => "Window 1",
        1 => "Window 2",
        2 => "Window 3",
        else => "Window 4",
    };
    copyTitle(&win.title, &win.title_len, label);
}

fn isContentPixel(color: u32) bool {
    return color != 0 and color != background_color;
}

fn detectSourceRegions(shadow: [*]u32, out_regions: *[max_windows]SourceRegion) usize {
    var active_col: [vfb_width]bool = [_]bool{false} ** vfb_width;

    var x: usize = 0;
    while (x < vfb_width) : (x += 1) {
        var y: usize = 0;
        while (y < vfb_height) : (y += 1) {
            if (isContentPixel(shadow[y * vfb_pitch + x])) {
                active_col[x] = true;
                break;
            }
        }
    }

    var count: usize = 0;
    x = 0;
    while (x < vfb_width and count < max_windows) {
        if (!active_col[x]) {
            x += 1;
            continue;
        }

        const x_start = x;
        while (x < vfb_width and active_col[x]) : (x += 1) {}
        const x_end = x;

        var found = false;
        var y_min: usize = vfb_height;
        var y_max: usize = 0;

        var yy: usize = 0;
        while (yy < vfb_height) : (yy += 1) {
            var xx: usize = x_start;
            while (xx < x_end) : (xx += 1) {
                if (!isContentPixel(shadow[yy * vfb_pitch + xx])) continue;
                found = true;
                if (yy < y_min) y_min = yy;
                if (yy > y_max) y_max = yy;
            }
        }
        if (!found) continue;

        const rx0 = if (x_start > 0) x_start - 1 else x_start;
        const rx1 = if (x_end < vfb_width) x_end + 1 else x_end;
        const ry0 = if (y_min > 0) y_min - 1 else y_min;
        const y_hi = y_max + 2;
        const ry1 = if (y_hi < vfb_height) y_hi else vfb_height;
        if (rx1 <= rx0 or ry1 <= ry0) continue;

        out_regions[count] = .{ .x = rx0, .y = ry0, .w = rx1 - rx0, .h = ry1 - ry0 };
        count += 1;
    }

    return count;
}

fn regionsEqual(a: *const [max_windows]SourceRegion, b: *const [max_windows]SourceRegion, count: usize) bool {
    var i: usize = 0;
    while (i < count) : (i += 1) {
        if (a[i].x != b[i].x or a[i].y != b[i].y or a[i].w != b[i].w or a[i].h != b[i].h) return false;
    }
    return true;
}

fn configureWindowsFromRegions(windows: *[max_windows]WindowState, regions: *const [max_windows]SourceRegion, count: usize) usize {
    var i: usize = 0;
    while (i < max_windows) : (i += 1) {
        windows[i].active = false;
        windows[i].visible = false;
        windows[i].src = .{ .x = 0, .y = 0, .w = 0, .h = 0 };
        windows[i].x = 0;
        windows[i].y = 0;
        windows[i].drag_off_x = 0;
        windows[i].drag_off_y = 0;
        windows[i].prev_close_hover = false;
        windows[i].prev_close_down = false;
        windows[i].title_len = 0;
    }

    var cursor_x: i32 = 72;
    var cursor_y: i32 = 120;
    var row_h: i32 = 0;

    i = 0;
    while (i < count and i < max_windows) : (i += 1) {
        var win = &windows[i];
        win.active = true;
        win.visible = true;
        win.src = regions[i];
        win.title_len = 0;

        const w = windowWidth(win);
        const h = windowHeight(win);
        if (cursor_x + w > fb_width_i32 - 24) {
            cursor_x = 72;
            cursor_y += row_h + 24;
            row_h = 0;
        }

        win.x = clampWindowX(win, cursor_x);
        win.y = clampWindowY(win, cursor_y);
        cursor_x += w + 24;
        if (h > row_h) row_h = h;
    }

    return if (count < max_windows) count else max_windows;
}

fn readWindowTitleFromMeta(force: bool) bool {
    const words: [*]const volatile u64 = @ptrFromInt(window_meta_va);
    if (words[0] != window_meta_magic) {
        if (force) {
            setDefaultWindowTitle();
            return true;
        }
        return false;
    }

    const seq = words[1];
    if (!force and seq == shared_title_seq) return false;
    shared_title_seq = seq;

    const bytes: [*]const volatile u8 = @ptrFromInt(window_meta_va);
    var len: usize = 0;
    while (len < window_title_max_bytes - 1) : (len += 1) {
        const ch: u8 = bytes[window_title_offset + len];
        if (ch == 0) break;
        shared_title_buf[len] = ch;
    }
    if (len == 0) {
        setDefaultWindowTitle();
    } else {
        shared_title_len = len;
    }
    return true;
}

fn redrawScene(
    back: [*]u32,
    windows: *const [max_windows]WindowState,
    close_hover: *const [max_windows]bool,
    close_down: *const [max_windows]bool,
    mouse_x: i32,
    mouse_y: i32,
) void {
    clearBackBuffer(back, background_color);

    const safe_count = max_windows;
    var i: usize = 0;
    while (i < safe_count) : (i += 1) {
        const win = &windows[i];
        if (!win.active or !win.visible) continue;
        const title = if (win.title_len > 0) win.title[0..win.title_len] else "Window";
        drawWindowChrome(back, win, title, close_hover[i], close_down[i]);
        blitWindowSourceToWindow(back, i);
    }
    drawMouseCoordHud(back, mouse_x, mouse_y);
}

fn mapPixelToWindowLocal(win: *const WindowState, vx: usize, vy: usize, local_x: *usize, local_y: *usize) bool {
    if (vx < win.src.x or vy < win.src.y) return false;
    if (vx >= win.src.x + win.src.w or vy >= win.src.y + win.src.h) return false;
    local_x.* = vx - win.src.x;
    local_y.* = vy - win.src.y;
    return true;
}

fn recomputeWindowCount() usize {
    var count: usize = 0;
    var i: usize = 0;
    while (i < max_windows) : (i += 1) {
        if (window_sources_storage[i].active and windows_storage[i].active and windows_storage[i].visible) {
            count += 1;
        }
    }
    return count;
}

fn findWindowSlotById(window_id: u32) ?usize {
    var i: usize = 0;
    while (i < max_windows) : (i += 1) {
        if (window_sources_storage[i].active and window_sources_storage[i].window_id == window_id) return i;
    }
    return null;
}

fn findFreeWindowSlot() ?usize {
    var i: usize = 0;
    while (i < max_windows) : (i += 1) {
        if (!window_sources_storage[i].active) return i;
    }
    return null;
}

fn updateWindowTitleFromMeta(slot: usize) void {
    if (slot >= max_windows) return;
    if (!window_sources_storage[slot].active) return;
    const meta_va = window_sources_storage[slot].meta_va;
    const map_lo = window_map_base_va;
    const map_hi = window_map_base_va + max_windows * window_map_stride_va;
    if (meta_va < map_lo or meta_va >= map_hi) return;
    const meta: *const volatile WindowMeta = @ptrFromInt(meta_va);
    if (meta.magic != window_meta_magic) return;
    const requested_len: usize = if (meta.title_len < 63) meta.title_len else 63;
    var i: usize = 0;
    while (i < requested_len) : (i += 1) {
        windows_storage[slot].title[i] = meta.title[i];
    }
    windows_storage[slot].title_len = requested_len;
}

fn registerWindowCap(page_paddr: u64) bool {
    const cap: *const volatile WindowCap = @ptrFromInt(ipc_rx_page_va);
    if (cap.magic != window_cap_magic or cap.version != 1) return false;
    if (cap.width == 0 or cap.height == 0) return false;
    if (cap.vfb_page_count != 1) {
        _ = userLog("Compositor: vfb_page_count!=1 unsupported\n");
        return false;
    }
    if (cap.vfb_cap_paddr < 0x1000 or cap.meta_cap_paddr < 0x1000) return false;

    const existing = findWindowSlotById(cap.window_id);
    const slot = existing orelse (findFreeWindowSlot() orelse return false);
    const slot_base = window_map_base_va + slot * window_map_stride_va;
    const map_meta_va = slot_base + 0x1000;
    const map_vfb_va = slot_base + 0x2000;

    if (mapPage(map_meta_va, cap.meta_cap_paddr, false) != syscall_ok) return false;
    if (mapPage(map_vfb_va, cap.vfb_cap_paddr, false) != syscall_ok) return false;

    window_sources_storage[slot] = .{
        .active = true,
        .window_id = cap.window_id,
        .vfb_va = map_vfb_va,
        .meta_va = map_meta_va,
        .width = cap.width,
        .height = cap.height,
        .pitch = cap.pixels_per_scan_line,
    };

    windows_storage[slot].active = true;
    windows_storage[slot].visible = true;
    windows_storage[slot].src = .{ .x = 0, .y = 0, .w = cap.width, .h = cap.height };
    windows_storage[slot].drag_off_x = 0;
    windows_storage[slot].drag_off_y = 0;
    windows_storage[slot].prev_close_hover = false;
    windows_storage[slot].prev_close_down = false;
    windows_storage[slot].x = @as(i32, @intCast(72 + (slot % 2) * 220));
    windows_storage[slot].y = @as(i32, @intCast(110 + (slot / 2) * 230));
    updateWindowTitleFromMeta(slot);

    _ = page_paddr;
    return true;
}

fn blitWindowSourceToWindow(back: [*]u32, slot: usize) void {
    const src = &window_sources_storage[slot];
    if (!src.active) return;
    const win = &windows_storage[slot];
    if (!win.active or !win.visible) return;
    const src_pixels: [*]const volatile u32 = @ptrFromInt(src.vfb_va);
    var vy: usize = 0;
    while (vy < src.height) : (vy += 1) {
        var vx: usize = 0;
        while (vx < src.width) : (vx += 1) {
            const src_index = vy * src.pitch + vx;
            _ = blitScaledPixelToWindow(back, win, vx, vy, src_pixels[src_index]);
        }
    }
}

pub export fn _start() noreturn {
    _ = userLog("Compositor: started\n");

    var shared_ready = false;
    var cursor_x: i32 = @intCast(fb_width / 2);
    var cursor_y: i32 = @intCast(fb_height / 2);
    var cursor_buttons: u64 = 0;
    var mouse_x: i32 = cursor_x;
    var mouse_y: i32 = cursor_y;
    var last_mouse_seq: u64 = 0;
    var cursor_drawn = false;
    var last_cursor_rect = cursorRect(cursor_x, cursor_y);
    var last_cursor_buttons: u64 = 0;

    var i: usize = 0;
    while (i < max_windows) : (i += 1) {
        windows_storage[i] = .{
            .active = false,
            .visible = false,
            .src = .{ .x = 0, .y = 0, .w = 0, .h = 0 },
            .x = 0,
            .y = 0,
            .drag_off_x = 0,
            .drag_off_y = 0,
            .prev_close_hover = false,
            .prev_close_down = false,
            .title = [_]u8{0} ** window_title_max_bytes,
            .title_len = 0,
        };
    }

    var window_count: usize = 0;

    var dragging_index: ?usize = null;
    var prev_left_down = false;
    var force_full = true;

    setDefaultWindowTitle();

    while (true) {
        const page_paddr = recvCap();
        if (page_paddr == syscall_err_empty) break;
        if (page_paddr < 0x1000) break;
        if (mapPage(ipc_rx_page_va, page_paddr, false) != syscall_ok) continue;

        const msg_words: [*]const volatile u64 = @ptrFromInt(ipc_rx_page_va);
        const magic32: u32 = @truncate(msg_words[0]);
        if (magic32 == window_cap_magic) {
            if (registerWindowCap(page_paddr)) {
                _ = userLog("Compositor: window cap received via IPC\n");
                force_full = true;
            }
            continue;
        }
        if (msg_words[0] != shared_magic or shared_ready) continue;
        if (mapPage(shared_page_va, page_paddr, false) != syscall_ok) continue;

        const shared_words_now: [*]const volatile u64 = @ptrFromInt(shared_page_va);
        if (shared_words_now[0] != shared_magic) continue;

        cursor_x = decodeSharedCoord(shared_words_now[4], fb_width_i32, cursor_x);
        cursor_y = decodeSharedCoord(shared_words_now[5], fb_height_i32, cursor_y);
        mouse_x = cursor_x;
        mouse_y = cursor_y;
        cursor_buttons = shared_words_now[6];
        last_mouse_seq = shared_words_now[7];
        shared_ready = true;

        if (grantCap(process0_id, page_paddr, rights_read_write) != syscall_ok) {
            _ = userLog("Compositor: grant shared cap back failed\n");
            while (true) asm volatile ("pause");
        }
        _ = userLog("Compositor: mouse shared page received via IPC\n");
        force_full = true;
    }

    const back: [*]u32 = back_buffer_storage[0..].ptr;
    const fb: [*]volatile u32 = @ptrFromInt(framebuffer_va);

    _ = userLog("Compositor: vfb compose ready\n");

    while (true) {
        while (true) {
            const page_paddr = recvCap();
            if (page_paddr == syscall_err_empty) break;
            if (page_paddr < 0x1000) break;
            if (mapPage(ipc_rx_page_va, page_paddr, false) != syscall_ok) continue;

            const msg_words: [*]const volatile u64 = @ptrFromInt(ipc_rx_page_va);
            const magic32: u32 = @truncate(msg_words[0]);
            if (magic32 == window_cap_magic) {
                if (registerWindowCap(page_paddr)) {
                    _ = userLog("Compositor: window cap received via IPC\n");
                    force_full = true;
                }
                continue;
            }
            if (msg_words[0] != shared_magic or shared_ready) continue;
            if (mapPage(shared_page_va, page_paddr, false) != syscall_ok) continue;

            const shared_words_now: [*]const volatile u64 = @ptrFromInt(shared_page_va);
            if (shared_words_now[0] != shared_magic) continue;

            cursor_x = decodeSharedCoord(shared_words_now[4], fb_width_i32, cursor_x);
            cursor_y = decodeSharedCoord(shared_words_now[5], fb_height_i32, cursor_y);
            mouse_x = cursor_x;
            mouse_y = cursor_y;
            cursor_buttons = shared_words_now[6];
            last_mouse_seq = shared_words_now[7];
            shared_ready = true;
            if (grantCap(process0_id, page_paddr, rights_read_write) != syscall_ok) {
                _ = userLog("Compositor: grant shared cap back failed\n");
                while (true) asm volatile ("pause");
            }
            _ = userLog("Compositor: mouse shared page received via IPC\n");
            force_full = true;
        }

        if (shared_ready) {
            const shared_words_now: [*]const volatile u64 = @ptrFromInt(shared_page_va);
            const seq = shared_words_now[7];
            if (seq != last_mouse_seq) {
                last_mouse_seq = seq;
                cursor_x = decodeSharedCoord(shared_words_now[4], fb_width_i32, cursor_x);
                cursor_y = decodeSharedCoord(shared_words_now[5], fb_height_i32, cursor_y);
                mouse_x = cursor_x;
                mouse_y = cursor_y;
                cursor_buttons = shared_words_now[6];
            }
        }

        _ = detected_regions_storage;
        _ = last_regions_storage;
        window_count = recomputeWindowCount();
        // Title metadata is updated when a new window cap is registered.

        if (window_count > max_windows) window_count = max_windows;
        cursor_x = @divTrunc(fb_width_i32, 2);
        cursor_y = @divTrunc(fb_height_i32, 2);
        cursor_buttons = 0;
        const left_down = false;
        const safe_window_count = max_windows;

        i = 0;
        while (i < max_windows) : (i += 1) {
            close_hover_storage[i] = false;
            close_down_storage[i] = false;
        }

        i = 0;
        while (i < safe_window_count) : (i += 1) {
            const win = &windows_storage[i];
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
            close_hover_storage[i] = pointInCloseButton(cursor_x, cursor_y, close_rect);
            close_down_storage[i] = close_hover_storage[i] and left_down;
            if (close_hover_storage[i] != win.prev_close_hover or close_down_storage[i] != win.prev_close_down) {
                force_full = true;
            }
        }

        if (!prev_left_down and left_down) {
            var hit: ?usize = null;
            var j: usize = safe_window_count;
            while (j > 0) {
                const idx = j - 1;
                const win = &windows_storage[idx];
                const wx0 = win.x;
                const wy0 = win.y;
                const wx1 = win.x + windowWidth(win);
                const wy1 = win.y + windowHeight(win);
                if (win.active and win.visible and cursor_x >= wx0 and cursor_x < wx1 and cursor_y >= wy0 and cursor_y < wy1) {
                    hit = idx;
                    break;
                }
                j -= 1;
            }

            if (hit) |idx| {
                var win = &windows_storage[idx];
                if (close_hover_storage[idx]) {
                    win.visible = false;
                    if (dragging_index != null and dragging_index.? == idx) dragging_index = null;
                    force_full = true;
                } else {
                    const hx0 = win.x + window_border_i32;
                    const hy0 = win.y + window_border_i32;
                    const hx1 = win.x + windowWidth(win) - window_border_i32;
                    const hy1 = win.y + window_header_h_i32;
                    if (!(cursor_x >= hx0 and cursor_x < hx1 and cursor_y >= hy0 and cursor_y < hy1)) {
                        continue;
                    }
                    win.drag_off_x = cursor_x - win.x;
                    win.drag_off_y = cursor_y - win.y;
                    dragging_index = idx;
                }
            }
        }

        if (dragging_index) |idx| {
            if (idx >= safe_window_count) {
                dragging_index = null;
            } else {
                var win = &windows_storage[idx];
                if (!left_down or !win.visible) {
                    dragging_index = null;
                } else {
                    const next_x = clampWindowX(win, cursor_x - win.drag_off_x);
                    const next_y = clampWindowY(win, cursor_y - win.drag_off_y);
                    if (next_x != win.x or next_y != win.y) {
                        win.x = next_x;
                        win.y = next_y;
                        force_full = true;
                    }
                }
            }
        }

        if (!left_down) dragging_index = null;
        prev_left_down = left_down;

        i = 0;
        while (i < safe_window_count) : (i += 1) {
            windows_storage[i].prev_close_hover = close_hover_storage[i];
            windows_storage[i].prev_close_down = close_down_storage[i];
        }

        // Temporarily keep a fixed primary title; meta polling is disabled
        // while diagnosing early control-flow corruption.

        // Stability mode: full-screen present every frame.
        redrawScene(back, &windows_storage, &close_hover_storage, &close_down_storage, mouse_x, mouse_y);
        const full_rect: Rect = .{ .x0 = 0, .y0 = 0, .x1 = fb_width_i32, .y1 = fb_height_i32 };
        copyRectBackToFbWithCursor(back, fb, full_rect, cursor_x, cursor_y, cursor_buttons);

        last_cursor_rect = cursorRect(cursor_x, cursor_y);
        cursor_drawn = true;
        last_cursor_buttons = cursor_buttons;

        force_full = false;
        asm volatile ("pause");
    }
}

