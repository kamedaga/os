const syscall_log: u64 = 0x9;
const font = @import("font.zig");
const user_vm = @import("user_vm.zig");
const window_client = @import("window_client.zig");

const keyboard_shared_page_va: usize = 0x3C00_6000;
const window_pixels_va: usize = 0x3C00_4000;
const window_meta_shared_va: usize = 0x3C00_7000;

const keyboard_shared_magic: u64 = 0x4B534852; // "KSHR"
const pixel_width: usize = 32;
const pixel_height: usize = 32;
const pixel_pitch: usize = 32;

const panel_x: usize = 0;
const panel_y: usize = 0;
const panel_w: usize = 32;
const panel_h: usize = 32;

const glyph_w: usize = 5;
const glyph_h: usize = 7;

const history_capacity: usize = 12;
const history_cols: usize = 4;
const history_rows: usize = 3;
const history_cell_w: usize = 6;
const history_cell_h: usize = 8;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn fillRect(vfb: [*]volatile u32, x: usize, y: usize, w: usize, h: usize, color: u32) void {
    if (w == 0 or h == 0) return;
    var yy: usize = y;
    while (yy < y + h and yy < pixel_height) : (yy += 1) {
        const row = yy * pixel_pitch;
        var xx: usize = x;
        while (xx < x + w and xx < pixel_width) : (xx += 1) {
            vfb[row + xx] = color;
        }
    }
}

fn setVfbPixel(vfb: [*]volatile u32, x: i32, y: i32, color: u32, alpha: u8) void {
    if (x < 0 or y < 0) return;
    if (x >= pixel_width or y >= pixel_height) return;
    const ux: usize = @intCast(x);
    const uy: usize = @intCast(y);
    const index = uy * pixel_pitch + ux;
    vfb[index] = font.blendColor(vfb[index], color, alpha);
}

fn glyphRows(raw: u8) [glyph_h]u8 {
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
        '.' => .{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 },
        ',' => .{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08 },
        ':' => .{ 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00 },
        ';' => .{ 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08 },
        '-' => .{ 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00 },
        '_' => .{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F },
        '/' => .{ 0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00 },
        '\\' => .{ 0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00 },
        '[' => .{ 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E },
        ']' => .{ 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E },
        '(' => .{ 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 },
        ')' => .{ 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 },
        '+' => .{ 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 },
        '=' => .{ 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 },
        '*' => .{ 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00 },
        '#' => .{ 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A },
        '!' => .{ 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 },
        '?' => .{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 },
        '|' => .{ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
        '<' => .{ 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 },
        '>' => .{ 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 },
        '\'' => .{ 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 },
        '"' => .{ 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00 },
        '`' => .{ 0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00 },
        '~' => .{ 0x00, 0x09, 0x16, 0x00, 0x00, 0x00, 0x00 },
        else => .{ 0x0E, 0x11, 0x01, 0x06, 0x04, 0x00, 0x04 },
    };
}

fn drawTinyGlyph(vfb: [*]volatile u32, x: usize, y: usize, ch: u8, color: u32) void {
    const glyph = glyphRows(ch);
    var gy: usize = 0;
    while (gy < glyph_h) : (gy += 1) {
        const bits = glyph[gy];
        const py = y + gy;
        if (py >= pixel_height) continue;
        const row = py * pixel_pitch;
        var gx: usize = 0;
        while (gx < glyph_w) : (gx += 1) {
            if ((bits & (@as(u8, 1) << @intCast((glyph_w - 1) - gx))) == 0) continue;
            const px = x + gx;
            if (px >= pixel_width) continue;
            vfb[row + px] = color;
        }
    }
}

fn normalizeHistoryChar(ch: u8) u8 {
    if (ch == ' ') return '_';
    if (ch == '\n') return 'E';
    if (ch == '\t') return 'T';
    if (ch < 0x20 or ch > 0x7E) return '?';
    return ch;
}

fn pushAsciiHistory(history: *[history_capacity]u8, history_len: *usize, ch: u8) void {
    if (history_len.* < history_capacity) {
        history[history_len.*] = ch;
        history_len.* += 1;
        return;
    }

    var i: usize = 1;
    while (i < history_capacity) : (i += 1) {
        history[i - 1] = history[i];
    }
    history[history_capacity - 1] = ch;
}

fn drawKeyboardHistoryPanel(vfb: [*]volatile u32, history: []const u8) void {
    fillRect(vfb, panel_x, panel_y, panel_w, panel_h, 0x00FD_FDFD);
    fillRect(vfb, panel_x + 1, 1, 30, 30, 0x0060_6060);
    fillRect(vfb, panel_x + 2, 2, 28, 28, 0x00EE_EEEE);

    var i: usize = 0;
    while (i < history.len) : (i += 1) {
        const row = i / history_cols;
        if (row >= history_rows) break;
        const col = i % history_cols;
        const x = panel_x + 4 + col * history_cell_w;
        const y = panel_y + 4 + row * history_cell_h;
        drawTinyGlyph(vfb, x, y, normalizeHistoryChar(history[i]), 0x0010_1010);
    }
}

pub export fn _start() noreturn {
    _ = userLog("KeyboardAsciiDemo: started\n");
    if (!window_client.initServiceBindingFromConfigPage()) {
        _ = userLog("KeyboardAsciiDemo: window service bind failed\n");
        while (true) asm volatile ("pause");
    }

    const keyboard_shared: [*]const volatile u64 = @ptrFromInt(keyboard_shared_page_va);
    if (keyboard_shared[0] != keyboard_shared_magic) {
        _ = userLog("KeyboardAsciiDemo: keyboard shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }
    const window_cap_tmp_va: u64 = @intCast(user_vm.reservePages(1) orelse {
        _ = userLog("KeyboardAsciiDemo: reserve cap tmp failed\n");
        while (true) asm volatile ("pause");
    });

    const window_created = window_client.createAndPublishWindow(
        @intCast(pixel_width),
        @intCast(pixel_height),
        0,
        window_cap_tmp_va,
        window_pixels_va,
        window_meta_shared_va,
    );
    if (!window_created) {
        _ = userLog("KeyboardAsciiDemo: create window failed\n");
        while (true) asm volatile ("pause");
    }
    window_client.setWindowTitle(window_meta_shared_va, "Keyboard Demo");

    const vfb: [*]volatile u32 = @ptrFromInt(window_pixels_va);
    var last_kbd_seq: u64 = 0;
    var ascii_history: [history_capacity]u8 = [_]u8{' '} ** history_capacity;
    var ascii_history_len: usize = 0;

    drawKeyboardHistoryPanel(vfb, ascii_history[0..ascii_history_len]);
    window_client.markWindowDirty(window_meta_shared_va);

    while (true) {
        const kbd_seq = keyboard_shared[1];
        if (kbd_seq != last_kbd_seq) {
            last_kbd_seq = kbd_seq;
            const key_value = keyboard_shared[4];
            if (key_value != 0) {
                const ascii: u8 = @intCast(keyboard_shared[2] & 0xFF);
                pushAsciiHistory(&ascii_history, &ascii_history_len, ascii);
            }
            drawKeyboardHistoryPanel(vfb, ascii_history[0..ascii_history_len]);
            window_client.markWindowDirty(window_meta_shared_va);
        }
        asm volatile ("pause");
    }
}
