const syscall_log: u64 = 0x9;
const syscall_wait_event: u64 = 0x17;
const font = @import("font.zig");
const mouse_input = @import("mouse_input.zig");
const process_abi = @import("process_abi.zig");
const protocol = @import("window_protocol.zig");
const taskbar_bootstrap = @import("taskbar_bootstrap_abi.zig");
const window_client = @import("window_client.zig");

const window_pixels_va: usize = 0x2030_0000;
const window_meta_shared_va: usize = 0x3C00_7000;
const window_cap_tmp_va: u64 = 0x3C10_0000;

const TaskbarStatePage = protocol.TaskbarStatePage;
const TaskbarCommandPage = protocol.TaskbarCommandPage;
const TaskbarEntry = protocol.TaskbarEntry;

const taskbar_config_magic = taskbar_bootstrap.config_magic;
const taskbar_state_magic = protocol.taskbar_state_magic;
const taskbar_command_magic = protocol.taskbar_command_magic;
const taskbar_protocol_version = protocol.taskbar_protocol_version;
const taskbar_entry_flag_visible = protocol.taskbar_entry_flag_visible;
const taskbar_command_activate = protocol.taskbar_command_activate;

const taskbar_height: usize = 32;
const taskbar_height_i32: i32 = 32;
const window_flags: u32 = window_client.window_flag_low_scale | window_client.window_flag_frameless;
const clock_color: u32 = 0x00EA_E9EC;
const clock_timeout_ticks: u64 = 20;
const button_width: usize = 11;
const button_height: usize = taskbar_height;
const button_margin_right: usize = 0;
const button_gap_from_clock: usize = 8;

const process_left_pad: i32 = 8;
const process_top_pad: i32 = 0;
const process_button_gap: i32 = 6;
const process_button_max_width: i32 = 164;
const process_right_reserved: i32 = 96;
const process_bg: u32 = 0x00AE_C5_DF;
const process_grad_start: u32 = 0x00DE_E7_F2;
const process_border: u32 = 0x0022_27_2D;
const process_border_left_bottom: u32 = 0x006A_79_89;
const process_inner_highlight: u32 = 0x00F3_F3_F3;
const process_hover_glow_color: u32 = 0x00CE_E2_F0;
const process_hidden_text: u32 = 0x00F2_F6_FA;
const process_text: u32 = 0x0022_27_2D;
const process_radius: i32 = 2;
const process_gradient_width: i32 = 48;

const Rect = struct {
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
};

const ProcessButtonStyle = struct {
    bg: u32,
    grad_start: u32,
    border: u32,
    border_left_bottom: u32,
    inner_highlight: u32,
    text: u32,
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

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn fillRect(vfb: [*]volatile u32, pitch: usize, width: usize, height: usize, x: usize, y: usize, w: usize, h: usize, color: u32) void {
    if (w == 0 or h == 0) return;
    if (x >= width or y >= height) return;
    const x1 = @min(width, x + w);
    const y1 = @min(height, y + h);
    var yy: usize = y;
    while (yy < y1) : (yy += 1) {
        const row = yy * pitch;
        var xx: usize = x;
        while (xx < x1) : (xx += 1) {
            vfb[row + xx] = color;
        }
    }
}

fn fillHorizontalLine(vfb: [*]volatile u32, pitch: usize, width: usize, y: usize, color: u32) void {
    if (y >= taskbar_height) return;
    const row = y * pitch;
    var x: usize = 0;
    while (x < width) : (x += 1) {
        vfb[row + x] = color;
    }
}

fn lerpChannel(a: u32, b: u32, num: usize, den: usize) u32 {
    if (den == 0) return a;
    return @intCast((a * @as(u32, @intCast(den - num)) + b * @as(u32, @intCast(num))) / @as(u32, @intCast(den)));
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
    return (((ar * ua + br * ub + uden / 2) / uden) << 16) |
        (((ag * ua + bg * ub + uden / 2) / uden) << 8) |
        ((ab * ua + bb * ub + uden / 2) / uden);
}

fn taskbarBackgroundColorAt(x: usize, width: usize) u32 {
    const taskbar_fill: u32 = 0x00A7_C0_DC;
    const right_fill: u32 = 0x0081_94_AA;
    const transition_width: usize = 18;
    const right_zone_width = button_margin_right + button_width + button_gap_from_clock + 40;
    const right_zone_start_raw = if (width > right_zone_width) width - right_zone_width else 0;
    const right_zone_start = if (right_zone_start_raw > 18) right_zone_start_raw - 18 else 0;
    const transition_start = if (right_zone_start > transition_width) right_zone_start - transition_width else 0;

    return if (x < transition_start)
        taskbar_fill
    else if (x < right_zone_start)
        lerpColor(taskbar_fill, right_fill, @intCast(x - transition_start), @intCast(right_zone_start - transition_start))
    else
        right_fill;
}

fn blendPixel(vfb: [*]volatile u32, x: i32, y: i32, color: u32, alpha: u8) void {
    if (x < 0 or y < 0) return;
    const ux: usize = @intCast(x);
    const uy: usize = @intCast(y);
    if (uy >= taskbar_height) return;
    const index = uy * @as(usize, @intCast(window_width_cache)) + ux;
    vfb[index] = font.blendColor(vfb[index], color, alpha);
}

var window_width_cache: i32 = 0;

fn drawTaskbar(vfb: [*]volatile u32, width: usize, height: usize) void {
    const top_dark_line: u32 = 0x001B_1D_1E;
    const top_highlight: u32 = 0x00EA_E9EC;
    fillHorizontalLine(vfb, width, width, 0, top_dark_line);
    fillHorizontalLine(vfb, width, width, 1, top_highlight);
    if (height > 2) {
        var y: usize = 2;
        while (y < height) : (y += 1) {
            var x: usize = 0;
            while (x < width) : (x += 1) {
                vfb[y * width + x] = taskbarBackgroundColorAt(x, width);
            }
        }
    }
}

fn drawPlaceholderButton(vfb: [*]volatile u32, width: usize) void {
    const outer_left_top: u32 = 0x001B_1D_1E;
    const inner_highlight_top: u32 = 0x00F3_F3_F3;
    const inner_highlight_bottom: u32 = 0x008B_93_9C;
    const outer_right_bottom: u32 = 0x0068_76_85;
    const fill: u32 = 0x005B_68_78;
    const highlight_fill: u32 = 0x008B_93_9C;
    const gradient_start_y: usize = 12;
    const x = width - button_margin_right - button_width;
    const y: usize = 0;
    var yy: usize = 0;
    while (yy < button_height) : (yy += 1) {
        var xx: usize = 0;
        while (xx < button_width) : (xx += 1) {
            const diag = yy + xx;
            const color = if (diag <= gradient_start_y)
                highlight_fill
            else if (diag >= button_height - 1 + button_width - 1)
                fill
            else
                lerpColor(highlight_fill, fill, @intCast(diag - gradient_start_y), @intCast((button_height - 1 + button_width - 1) - gradient_start_y));
            vfb[(y + yy) * width + (x + xx)] = color;
        }
    }

    fillRect(vfb, width, width, taskbar_height, x, y, button_width, 1, outer_left_top);
    fillRect(vfb, width, width, taskbar_height, x, y, 1, button_height, outer_left_top);

    if (button_width > 2 and button_height > 2) {
        fillRect(vfb, width, width, taskbar_height, x + 1, y + 1, button_width - 2, 1, inner_highlight_top);
        var inner_y: usize = 0;
        while (inner_y < button_height - 2) : (inner_y += 1) {
            const color = lerpColor(inner_highlight_top, inner_highlight_bottom, @intCast(inner_y), @intCast(button_height - 3));
            vfb[(y + 1 + inner_y) * width + (x + 1)] = color;
        }
    }

    fillRect(vfb, width, width, taskbar_height, x + button_width - 1, y, 1, button_height, outer_right_bottom);
    fillRect(vfb, width, width, taskbar_height, x, y + button_height - 1, button_width, 1, outer_right_bottom);
}

fn drawClock(vfb: [*]volatile u32, width: usize, elapsed_seconds: u64) void {
    var buf: [5]u8 = .{ '0', '0', ':', '0', '0' };
    const minutes: u64 = (elapsed_seconds / 60) % 100;
    const seconds: u64 = elapsed_seconds % 60;
    buf[0] = @intCast('0' + ((minutes / 10) % 10));
    buf[1] = @intCast('0' + (minutes % 10));
    buf[3] = @intCast('0' + ((seconds / 10) % 10));
    buf[4] = @intCast('0' + (seconds % 10));

    const text_w = font.measureAsciiText(buf[0..], 1);
    const right_reserved = button_margin_right + button_width + button_gap_from_clock;
    const text_x: i32 = @intCast(width - right_reserved - @as(usize, @intCast(text_w)));
    const text_y: i32 = 10;
    font.drawAsciiTextClipped([*]volatile u32, blendPixel, vfb, text_x, text_y, buf[0..], clock_color, 1, @intCast(width - 8));
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

fn pointInRect(px: i32, py: i32, rect: Rect) bool {
    return px >= rect.x0 and px < rect.x1 and py >= rect.y0 and py < rect.y1;
}

fn processButtonStyle(hidden: bool) ProcessButtonStyle {
    if (!hidden) {
        return .{
            .bg = process_bg,
            .grad_start = process_grad_start,
            .border = process_border,
            .border_left_bottom = process_border_left_bottom,
            .inner_highlight = process_inner_highlight,
            .text = process_text,
        };
    }
    return .{
        .bg = font.blendColor(process_bg, 0x0000_0000, 36),
        .grad_start = font.blendColor(process_grad_start, 0x0000_0000, 48),
        .border = process_border,
        .border_left_bottom = font.blendColor(process_border_left_bottom, 0x0000_0000, 28),
        .inner_highlight = font.blendColor(process_inner_highlight, 0x0000_0000, 22),
        .text = process_hidden_text,
    };
}

fn drawHoverGlow(vfb: [*]volatile u32, rect: Rect) void {
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
            blendPixel(vfb, x, y, process_hover_glow_color, alpha);
        }
    }
}

fn taskbarStateEntryTitle(entry: *const volatile TaskbarEntry, scratch: []u8) []const u8 {
    const raw_len: usize = if (entry.title_len < protocol.window_title_max_bytes) entry.title_len else protocol.window_title_max_bytes;
    if (raw_len == 0) {
        const fallback = "Window";
        @memcpy(scratch[0..fallback.len], fallback);
        return scratch[0..fallback.len];
    }
    var i: usize = 0;
    while (i < raw_len) : (i += 1) {
        scratch[i] = entry.title[i];
    }
    return scratch[0..raw_len];
}

fn fitProcessLabel(text: []const u8, max_width: i32, scratch: []u8) []const u8 {
    if (text.len == 0 or max_width <= 0) return "";
    if (font.measureAsciiText(text, 1) <= max_width) return text;

    const ellipsis = "...";
    const ellipsis_w = font.measureAsciiText(ellipsis, 1);
    if (ellipsis_w > max_width or scratch.len < ellipsis.len) return "";

    var out_len: usize = 0;
    var width: i32 = 0;
    var i: usize = 0;
    while (i < text.len and out_len < scratch.len) : (i += 1) {
        const next_w = font.measureAsciiText(text[i .. i + 1], 1);
        if (width + next_w + ellipsis_w > max_width) break;
        scratch[out_len] = text[i];
        out_len += 1;
        width += next_w;
    }
    if (out_len == 0) return "";
    @memcpy(scratch[out_len .. out_len + ellipsis.len], ellipsis);
    return scratch[0 .. out_len + ellipsis.len];
}

fn processButtonRect(index: usize, count: usize, width: usize) ?Rect {
    if (count == 0) return null;
    const usable_w = @as(i32, @intCast(width)) - process_left_pad - process_right_reserved;
    const usable_h = taskbar_height_i32 - process_top_pad * 2;
    if (usable_w <= 0 or usable_h <= 0) return null;

    const gap_total = process_button_gap * @as(i32, @intCast(count - 1));
    const max_per_button = @divFloor(usable_w - gap_total, @as(i32, @intCast(count)));
    if (max_per_button <= 0) return null;
    const button_w = if (max_per_button < process_button_max_width) max_per_button else process_button_max_width;
    const step = button_w + process_button_gap;
    const x0 = process_left_pad + @as(i32, @intCast(index)) * step;
    const x1 = x0 + button_w;
    const limit_x = @as(i32, @intCast(width)) - process_right_reserved;
    if (x0 >= limit_x) return null;

    return .{
        .x0 = x0,
        .y0 = process_top_pad,
        .x1 = if (x1 < limit_x) x1 else limit_x,
        .y1 = taskbar_height_i32 - process_top_pad,
    };
}

fn hoveredEntryIndex(state_page: *const volatile TaskbarStatePage, mouse_x: i32, mouse_y: i32, width: usize) ?usize {
    const count: usize = if (state_page.entry_count < protocol.taskbar_entry_max) state_page.entry_count else protocol.taskbar_entry_max;
    if (mouse_y < 0 or mouse_y >= taskbar_height_i32) return null;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const rect = processButtonRect(i, count, width) orelse continue;
        if (pointInRect(mouse_x, mouse_y, rect)) return i;
    }
    return null;
}

fn drawProcessButton(vfb: [*]volatile u32, width: usize, rect: Rect, title: []const u8, hidden: bool, hovered: bool) void {
    const style = processButtonStyle(hidden);
    const w = rect.x1 - rect.x0;
    const h = rect.y1 - rect.y0;
    if (w <= 0 or h <= 0) return;

    if (hovered) drawHoverGlow(vfb, rect);

    var yy: i32 = 0;
    while (yy < h) : (yy += 1) {
        var xx: i32 = 0;
        while (xx < w) : (xx += 1) {
            if (!insideRoundedRectI32(xx, yy, w, h, process_radius, 0)) continue;
            const color = if (xx < process_gradient_width)
                lerpColor(style.grad_start, style.bg, @min(xx + yy, process_gradient_width - 1), process_gradient_width - 1)
            else
                style.bg;
            const px: usize = @intCast(rect.x0 + xx);
            const py: usize = @intCast(rect.y0 + yy);
            vfb[py * width + px] = color;
        }
    }

    yy = 0;
    while (yy < h) : (yy += 1) {
        var xx: i32 = 0;
        while (xx < w) : (xx += 1) {
            if (!insideRoundedRectI32(xx, yy, w, h, process_radius, 0)) continue;
            const px: usize = @intCast(rect.x0 + xx);
            const py: usize = @intCast(rect.y0 + yy);
            if (!insideRoundedRectI32(xx, yy, w, h, process_radius, 1)) {
                vfb[py * width + px] = if (xx == 0)
                    lerpColor(style.border, style.border_left_bottom, yy, h - 1)
                else
                    style.border;
            } else if (!insideRoundedRectI32(xx, yy, w, h, process_radius, 2)) {
                vfb[py * width + px] = style.inner_highlight;
            }
        }
    }

    const text_y: i32 = rect.y0 + @divTrunc(h - font.lineHeight(1), 2);
    font.drawAsciiTextClipped([*]volatile u32, blendPixel, vfb, rect.x0 + 14, text_y, title, style.text, 1, rect.x1 - 8);
}

fn drawProcessList(vfb: [*]volatile u32, width: usize, state_page: *const volatile TaskbarStatePage, hovered_index: ?usize) void {
    const count: usize = if (state_page.entry_count < protocol.taskbar_entry_max) state_page.entry_count else protocol.taskbar_entry_max;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const rect = processButtonRect(i, count, width) orelse continue;
        const entry = &state_page.entries[i];
        const hidden = (entry.flags & taskbar_entry_flag_visible) == 0;
        var raw_buf: [protocol.window_title_max_bytes]u8 = undefined;
        const raw = taskbarStateEntryTitle(entry, raw_buf[0..]);
        var fit_buf: [protocol.window_title_max_bytes + 3]u8 = undefined;
        const title = fitProcessLabel(raw, rect.x1 - rect.x0 - 22, fit_buf[0..]);
        drawProcessButton(vfb, width, rect, title, hidden, hovered_index != null and hovered_index.? == i);
    }
}

fn redrawClockArea(vfb: [*]volatile u32, width: usize) void {
    const clock_region_w: usize = 64;
    const region_x = width - (button_margin_right + button_width + button_gap_from_clock + clock_region_w);
    var yy: usize = 2;
    while (yy < taskbar_height) : (yy += 1) {
        var xx: usize = 0;
        while (xx < clock_region_w) : (xx += 1) {
            const x = region_x + xx;
            vfb[yy * width + x] = taskbarBackgroundColorAt(x, width);
        }
    }
}

fn redrawAll(vfb: [*]volatile u32, width: usize, state_page: *const volatile TaskbarStatePage, hovered_index: ?usize, elapsed_seconds: u64) void {
    drawTaskbar(vfb, width, taskbar_height);
    drawProcessList(vfb, width, state_page, hovered_index);
    drawPlaceholderButton(vfb, width);
    drawClock(vfb, width, elapsed_seconds);
}

fn sendActivateCommand(command_page: *volatile TaskbarCommandPage, window_id: u32) void {
    command_page.command = taskbar_command_activate;
    command_page.window_id = window_id;
    command_page.seq +%= 1;
}

pub export fn _start() noreturn {
    _ = userLog("Taskbar: entry\n");
    const cfg: [*]const volatile u64 = @ptrFromInt(process_abi.standard_config_target_va);
    if (cfg[0] != taskbar_config_magic or cfg[1] != taskbar_bootstrap.config_version) {
        _ = userLog("Taskbar: config magic mismatch\n");
        while (true) asm volatile ("pause");
    }

    if (!window_client.initServiceBindingFromRegistryPage(cfg[taskbar_bootstrap.service_registry_va_index])) {
        _ = userLog("Taskbar: window service bind failed\n");
        while (true) asm volatile ("pause");
    }
    mouse_input.setSharedPageVa(cfg[taskbar_bootstrap.pointer_shared_va_index]);
    _ = userLog("Taskbar: started\n");

    const taskbar_state_shared_va: usize = @intCast(cfg[taskbar_bootstrap.state_page_va_index]);
    const taskbar_command_shared_va: usize = @intCast(cfg[taskbar_bootstrap.command_page_va_index]);
    const state_page: *const volatile TaskbarStatePage = @ptrFromInt(taskbar_state_shared_va);
    const command_page: *volatile TaskbarCommandPage = @ptrFromInt(taskbar_command_shared_va);
    if (mouse_input.sharedPage() == null) {
        _ = userLog("Taskbar: mouse shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }
    if (state_page.magic != taskbar_state_magic or state_page.version != taskbar_protocol_version) {
        _ = userLog("Taskbar: state page magic mismatch\n");
        while (true) asm volatile ("pause");
    }
    if (command_page.magic != taskbar_command_magic or command_page.version != taskbar_protocol_version) {
        _ = userLog("Taskbar: command page magic mismatch\n");
        while (true) asm volatile ("pause");
    }

    const screen_width: usize = @intCast(cfg[2]);
    const screen_height: usize = @intCast(cfg[3]);
    if (screen_width == 0 or screen_height < taskbar_height) {
        _ = userLog("Taskbar: invalid screen size\n");
        while (true) asm volatile ("pause");
    }

    const window_created = window_client.createAndPublishWindow(
        @intCast(screen_width),
        taskbar_height,
        window_flags,
        window_cap_tmp_va,
        window_pixels_va,
        window_meta_shared_va,
    );
    if (!window_created) {
        _ = userLog("Taskbar: create window failed\n");
        while (true) asm volatile ("pause");
    }

    window_client.setWindowTitle(window_meta_shared_va, "Taskbar");
    window_client.setWindowPosition(window_meta_shared_va, 0, @intCast(screen_height - taskbar_height));

    const vfb: [*]volatile u32 = @ptrFromInt(window_pixels_va);
    window_width_cache = @intCast(screen_width);

    var elapsed_seconds: u64 = 0;
    var tick_accum: u64 = 0;
    var last_state_seq: u64 = 0;
    var last_left_down = false;
    var last_hovered_index: ?usize = null;
    var mouse_reader = mouse_input.Reader.init(0, @intCast(screen_height - taskbar_height));

    redrawAll(vfb, screen_width, state_page, null, elapsed_seconds);
    window_client.markWindowDirty(window_meta_shared_va);

    while (true) {
        _ = waitEvent(false, 1);
        tick_accum +%= 1;

        const mouse = mouse_reader.read() orelse {
            _ = userLog("Taskbar: mouse shared page unavailable\n");
            while (true) asm volatile ("pause");
        };
        const left_down = mouse.leftDown();
        const hovered_index = hoveredEntryIndex(state_page, mouse.local_x, mouse.local_y, screen_width);

        if (mouse.leftJustPressed() and hovered_index != null and hovered_index.? < state_page.entry_count) {
            const entry = &state_page.entries[hovered_index.?];
            if (entry.window_id != 0) {
                sendActivateCommand(command_page, entry.window_id);
            }
        }

        var redraw = false;
        if (mouse.changed and (hovered_index != last_hovered_index or left_down != last_left_down)) redraw = true;
        if (state_page.seq != last_state_seq) {
            last_state_seq = state_page.seq;
            redraw = true;
        }
        if (tick_accum >= clock_timeout_ticks) {
            tick_accum = 0;
            elapsed_seconds +%= 1;
            redraw = true;
        }

        if (redraw) {
            redrawAll(vfb, screen_width, state_page, hovered_index, elapsed_seconds);
            window_client.markWindowDirty(window_meta_shared_va);
        }

        last_left_down = left_down;
        last_hovered_index = hovered_index;
    }
}
