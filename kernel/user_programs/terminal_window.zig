const window_client = @import("window_client.zig");

const syscall_log: u64 = 0x9;
const syscall_launch_pie_user: u64 = 0x16;
const syscall_ok: u64 = 0;

const keyboard_shared_page_va: usize = 0x3C00_6000;
const keyboard_shared_magic: u64 = 0x4B534852; // "KSHR"

// Terminal uses a larger backing store than the tiny demo windows, so keep it
// away from the aux/shared-page area and from the PF recovery test VA.
const window_pixels_va: usize = 0x2030_0000;
const window_meta_shared_va: usize = 0x3C00_7000;
const window_cap_tmp_va: u64 = 0x3C10_0000;
const window_flags: u32 = window_client.window_flag_low_scale;

const pixel_width: usize = 320;
const pixel_height: usize = 160;
const pixel_pitch: usize = 320;
const glyph_w: usize = 5;
const glyph_h: usize = 7;
const glyph_scale: usize = 2;
const cell_w: usize = glyph_w * glyph_scale + 2;
const cell_h: usize = glyph_h * glyph_scale + 2;
const cols: usize = pixel_width / cell_w;
const rows: usize = pixel_height / cell_h;

const bg_color: u32 = 0x00F5_EFDF;
const header_bg_color: u32 = 0x00E3_D8C4;
const prompt_bg_color: u32 = 0x00F0_E5D4;
const border_color: u32 = 0x0095_8875;
const fg_color: u32 = 0x002A_241E;
const title_color: u32 = 0x001E_6170;
const prompt_color: u32 = 0x00A0_4A00;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn launchUserProgram(selector: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_launch_pie_user),
          [arg0] "{rdi}" (selector),
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

fn drawGlyph(vfb: [*]volatile u32, x: usize, y: usize, ch: u8, color: u32) void {
    const glyph = glyphRows(ch);
    var gy: usize = 0;
    while (gy < glyph_h) : (gy += 1) {
        const bits = glyph[gy];
        var gx: usize = 0;
        while (gx < glyph_w) : (gx += 1) {
            if ((bits & (@as(u8, 1) << @intCast((glyph_w - 1) - gx))) == 0) continue;
            const px = x + gx * glyph_scale;
            const py = y + gy * glyph_scale;
            var sy: usize = 0;
            while (sy < glyph_scale and py + sy < pixel_height) : (sy += 1) {
                const row = (py + sy) * pixel_pitch;
                var sx: usize = 0;
                while (sx < glyph_scale and px + sx < pixel_width) : (sx += 1) {
                    vfb[row + px + sx] = color;
                }
            }
        }
    }
}

fn asciiLower(ch: u8) u8 {
    if (ch >= 'A' and ch <= 'Z') return ch + 32;
    return ch;
}

fn trimSpaces(text: []const u8) []const u8 {
    var s: usize = 0;
    var e: usize = text.len;
    while (s < e and (text[s] == ' ' or text[s] == '\t')) : (s += 1) {}
    while (e > s and (text[e - 1] == ' ' or text[e - 1] == '\t')) : (e -= 1) {}
    return text[s..e];
}

fn eqAsciiNoCase(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    var i: usize = 0;
    while (i < a.len) : (i += 1) {
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return true;
}

const TerminalState = struct {
    lines: [rows][cols]u8 = [_][cols]u8{[_]u8{' '} ** cols} ** rows,
    line_len: [rows]usize = [_]usize{0} ** rows,
    cur_row: usize = 1,
    cmd: [64]u8 = undefined,
    cmd_len: usize = 0,

    fn clearRow(self: *TerminalState, row: usize) void {
        self.lines[row] = [_]u8{' '} ** cols;
        self.line_len[row] = 0;
    }

    fn scroll(self: *TerminalState) void {
        var r: usize = 2;
        while (r < rows) : (r += 1) {
            self.lines[r - 1] = self.lines[r];
            self.line_len[r - 1] = self.line_len[r];
        }
        self.clearRow(rows - 1);
        self.cur_row = rows - 1;
    }

    fn writeLine(self: *TerminalState, text: []const u8) void {
        if (self.cur_row >= rows) self.scroll();
        self.clearRow(self.cur_row);
        const n: usize = if (text.len < cols) text.len else cols;
        if (n > 0) {
            @memcpy(self.lines[self.cur_row][0..n], text[0..n]);
        }
        self.line_len[self.cur_row] = n;
        self.cur_row += 1;
        if (self.cur_row >= rows) self.scroll();
    }
};

fn drawLine(vfb: [*]volatile u32, row: usize, text: []const u8, color: u32) void {
    var i: usize = 0;
    while (i < text.len and i < cols) : (i += 1) {
        drawGlyph(vfb, i * cell_w, row * cell_h, text[i], color);
    }
}

fn clearRow(vfb: [*]volatile u32, row: usize, color: u32) void {
    fillRect(vfb, 0, row * cell_h, pixel_width, cell_h, color);
}

fn markPromptDirty() void {
    window_client.markWindowDirtyRect(window_meta_shared_va, 0, (rows - 1) * cell_h, pixel_width, cell_h);
}

fn renderPrompt(vfb: [*]volatile u32, st: *const TerminalState) void {
    clearRow(vfb, rows - 1, prompt_bg_color);
    fillRect(vfb, 0, (rows - 1) * cell_h, pixel_width, 1, border_color);

    var prompt_buf: [cols]u8 = [_]u8{' '} ** cols;
    prompt_buf[0] = '>';
    prompt_buf[1] = ' ';
    var i: usize = 0;
    while (i < st.cmd_len and i + 2 < cols) : (i += 1) {
        prompt_buf[i + 2] = st.cmd[i];
    }
    drawLine(vfb, rows - 1, prompt_buf[0..], prompt_color);
}

fn render(vfb: [*]volatile u32, st: *const TerminalState) void {
    fillRect(vfb, 0, 0, pixel_width, pixel_height, bg_color);
    fillRect(vfb, 0, 0, pixel_width, cell_h, header_bg_color);
    fillRect(vfb, 0, (rows - 1) * cell_h, pixel_width, cell_h, prompt_bg_color);
    fillRect(vfb, 0, 0, pixel_width, 2, border_color);
    fillRect(vfb, 0, pixel_height - 2, pixel_width, 2, border_color);
    fillRect(vfb, 0, 0, 2, pixel_height, border_color);
    fillRect(vfb, pixel_width - 2, 0, 2, pixel_height, border_color);
    fillRect(vfb, 0, cell_h, pixel_width, 1, border_color);
    fillRect(vfb, 0, (rows - 1) * cell_h, pixel_width, 1, border_color);

    drawLine(vfb, 0, "terminal  help clear", title_color);

    var r: usize = 1;
    while (r + 1 < rows) : (r += 1) {
        const n = st.line_len[r];
        drawLine(vfb, r, st.lines[r][0..n], fg_color);
    }

    renderPrompt(vfb, st);
}

fn executeCommand(st: *TerminalState, cmd_text: []const u8) void {
    const cmd = trimSpaces(cmd_text);
    if (cmd.len == 0) return;

    if (eqAsciiNoCase(cmd, "help")) {
        st.writeLine("help pie pie_user mouse mouse_demo keyboard keyboard_demo clear");
        return;
    }
    if (eqAsciiNoCase(cmd, "clear")) {
        var r: usize = 1;
        while (r < rows - 1) : (r += 1) {
            st.clearRow(r);
        }
        st.cur_row = 1;
        return;
    }
    if (eqAsciiNoCase(cmd, "pie") or eqAsciiNoCase(cmd, "pie_user")) {
        if (launchUserProgram(0) == syscall_ok) {
            st.writeLine("launch pie_user ok");
            st.writeLine("see serial userlog");
        } else {
            st.writeLine("launch pie_user failed");
        }
        return;
    }
    if (eqAsciiNoCase(cmd, "mouse") or eqAsciiNoCase(cmd, "mouse_demo")) {
        if (launchUserProgram(1) == syscall_ok) {
            st.writeLine("launch mouse_demo ok");
            st.writeLine("see serial userlog");
        } else {
            st.writeLine("launch mouse_demo failed");
        }
        return;
    }
    if (eqAsciiNoCase(cmd, "keyboard") or eqAsciiNoCase(cmd, "keyboard_demo")) {
        if (launchUserProgram(2) == syscall_ok) {
            st.writeLine("launch keyboard_demo ok");
            st.writeLine("see serial userlog");
        } else {
            st.writeLine("launch keyboard_demo failed");
        }
        return;
    }
    st.writeLine("unknown command");
}

pub export fn _start() noreturn {
    _ = userLog("TerminalWindow: started\n");

    const keyboard_shared: [*]const volatile u64 = @ptrFromInt(keyboard_shared_page_va);
    if (keyboard_shared[0] != keyboard_shared_magic) {
        _ = userLog("TerminalWindow: keyboard shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }

    const window_created = window_client.createAndPublishWindow(
        @intCast(pixel_width),
        @intCast(pixel_height),
        window_flags,
        window_cap_tmp_va,
        window_pixels_va,
        window_meta_shared_va,
    );
    if (!window_created) {
        _ = userLog("TerminalWindow: create window failed\n");
        while (true) asm volatile ("pause");
    }
    window_client.setWindowTitle(window_meta_shared_va, "Terminal");

    const vfb: [*]volatile u32 = @ptrFromInt(window_pixels_va);
    var st = TerminalState{};
    st.writeLine("ready");
    st.writeLine("type help");
    render(vfb, &st);
    window_client.markWindowDirty(window_meta_shared_va);

    var last_kbd_seq: u64 = keyboard_shared[1];
    while (true) {
        const kbd_seq = keyboard_shared[1];
        if (kbd_seq != last_kbd_seq) {
            last_kbd_seq = kbd_seq;
            const key_value = keyboard_shared[4];
            if (key_value != 0) {
                const ascii: u8 = @intCast(keyboard_shared[2] & 0xFF);
                switch (ascii) {
                    '\n', '\r' => {
                        executeCommand(&st, st.cmd[0..st.cmd_len]);
                        st.cmd_len = 0;
                        render(vfb, &st);
                        window_client.markWindowDirty(window_meta_shared_va);
                    },
                    '\x08', '\x7f' => {
                        if (st.cmd_len > 0) {
                            st.cmd_len -= 1;
                            renderPrompt(vfb, &st);
                            markPromptDirty();
                        }
                    },
                    else => {
                        if (ascii >= 0x20 and ascii <= 0x7E and st.cmd_len < st.cmd.len) {
                            st.cmd[st.cmd_len] = ascii;
                            st.cmd_len += 1;
                            renderPrompt(vfb, &st);
                            markPromptDirty();
                        }
                    },
                }
            }
        }
        asm volatile ("pause");
    }
}
