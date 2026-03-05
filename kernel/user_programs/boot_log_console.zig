const framebuffer_va: usize = 0x3C00_5000;
const boot_log_page_va: usize = 0x3C00_1000;

const fb_width: usize = 832;
const fb_height: usize = 624;
const fb_pitch: usize = 832;
const cell_w: usize = 16;
const cell_h: usize = 16;
const glyph_w: usize = 5;
const glyph_h: usize = 7;
const glyph_scale: usize = 2;
const cols: usize = fb_width / cell_w;
const rows: usize = fb_height / cell_h;
const log_header_bytes: usize = 8;
const log_payload_bytes: usize = 4096 - log_header_bytes;

const default_fg_color: u32 = 0x00FF_FFFF;
const default_bg_color: u32 = 0x0000_0000;
const prompt_fg_color: u32 = 0x00FF_FF55;
const enter_prompt_text = "Press Enter to launch compositor.\n";

const ParserState = enum {
    normal,
    esc,
    csi,
};

const Console = struct {
    fb: [*]volatile u32,
    cursor_x: usize = 0,
    cursor_y: usize = 0,
    fg: u32 = default_fg_color,
    bg: u32 = default_bg_color,

    fn fillRectPx(self: *Console, x0: usize, y0: usize, width: usize, height: usize, color: u32) void {
        if (width == 0 or height == 0) return;
        if (x0 >= fb_width or y0 >= fb_height) return;

        const end_x = if (x0 + width > fb_width) fb_width else x0 + width;
        const end_y = if (y0 + height > fb_height) fb_height else y0 + height;

        var y: usize = y0;
        while (y < end_y) : (y += 1) {
            const row = y * fb_pitch;
            var x: usize = x0;
            while (x < end_x) : (x += 1) {
                self.fb[row + x] = color;
            }
        }
    }

    fn clear(self: *Console) void {
        self.fillRectPx(0, 0, fb_width, fb_height, self.bg);
        self.cursor_x = 0;
        self.cursor_y = 0;
    }

    fn clearCell(self: *Console, col: usize, row: usize) void {
        self.fillRectPx(col * cell_w, row * cell_h, cell_w, cell_h, self.bg);
    }

    fn clearCellsInRow(self: *Console, row: usize, start_col: usize, end_col_exclusive: usize) void {
        if (row >= rows) return;
        if (start_col >= cols) return;
        const end_col = if (end_col_exclusive > cols) cols else end_col_exclusive;
        if (start_col >= end_col) return;
        self.fillRectPx(start_col * cell_w, row * cell_h, (end_col - start_col) * cell_w, cell_h, self.bg);
    }

    fn scrollUp(self: *Console) void {
        var y: usize = 0;
        while (y < fb_height - cell_h) : (y += 1) {
            const dst_row = y * fb_pitch;
            const src_row = (y + cell_h) * fb_pitch;
            var x: usize = 0;
            while (x < fb_width) : (x += 1) {
                self.fb[dst_row + x] = self.fb[src_row + x];
            }
        }
        self.fillRectPx(0, fb_height - cell_h, fb_width, cell_h, self.bg);
    }

    fn setCursor(self: *Console, col: usize, row: usize) void {
        self.cursor_x = if (col < cols) col else cols - 1;
        self.cursor_y = if (row < rows) row else rows - 1;
    }

    fn newline(self: *Console) void {
        self.cursor_x = 0;
        self.cursor_y += 1;
        if (self.cursor_y >= rows) {
            self.scrollUp();
            self.cursor_y = rows - 1;
        }
    }

    fn drawGlyph(self: *Console, ch: u8) void {
        const glyph = glyphRows(ch);
        const px = self.cursor_x * cell_w + 2;
        const py = self.cursor_y * cell_h + 1;

        var gy: usize = 0;
        while (gy < glyph_h) : (gy += 1) {
            const bits = glyph[gy];
            const base_y = py + gy * glyph_scale;
            var gx: usize = 0;
            while (gx < glyph_w) : (gx += 1) {
                if ((bits & (@as(u8, 1) << @intCast((glyph_w - 1) - gx))) == 0) continue;
                const base_x = px + gx * glyph_scale;
                var sy: usize = 0;
                while (sy < glyph_scale) : (sy += 1) {
                    const row = (base_y + sy) * fb_pitch;
                    var sx: usize = 0;
                    while (sx < glyph_scale) : (sx += 1) {
                        self.fb[row + base_x + sx] = self.fg;
                    }
                }
            }
        }
    }

    fn putPrintable(self: *Console, ch: u8) void {
        if (self.cursor_x >= cols) self.newline();
        self.clearCell(self.cursor_x, self.cursor_y);
        self.drawGlyph(ch);
        self.cursor_x += 1;
        if (self.cursor_x >= cols) self.newline();
    }

    fn putChar(self: *Console, ch: u8) void {
        switch (ch) {
            '\n' => {
                self.newline();
                return;
            },
            '\r' => {
                self.cursor_x = 0;
                return;
            },
            '\t' => {
                var i: usize = 0;
                while (i < 4) : (i += 1) self.putPrintable(' ');
                return;
            },
            '\x08' => {
                if (self.cursor_x > 0) {
                    self.cursor_x -= 1;
                } else if (self.cursor_y > 0) {
                    self.cursor_y -= 1;
                    self.cursor_x = cols - 1;
                }
                self.clearCell(self.cursor_x, self.cursor_y);
                return;
            },
            else => {},
        }

        if (ch < 0x20) return;
        self.putPrintable(ch);
    }

    fn eraseDisplay(self: *Console, mode: u16) void {
        switch (mode) {
            2 => self.clear(),
            1 => {
                var r: usize = 0;
                while (r < self.cursor_y) : (r += 1) {
                    self.clearCellsInRow(r, 0, cols);
                }
                self.clearCellsInRow(self.cursor_y, 0, self.cursor_x + 1);
            },
            else => {
                self.clearCellsInRow(self.cursor_y, self.cursor_x, cols);
                var r: usize = self.cursor_y + 1;
                while (r < rows) : (r += 1) {
                    self.clearCellsInRow(r, 0, cols);
                }
            },
        }
    }

    fn eraseLine(self: *Console, mode: u16) void {
        switch (mode) {
            2 => self.clearCellsInRow(self.cursor_y, 0, cols),
            1 => self.clearCellsInRow(self.cursor_y, 0, self.cursor_x + 1),
            else => self.clearCellsInRow(self.cursor_y, self.cursor_x, cols),
        }
    }
};

const AnsiParser = struct {
    state: ParserState = .normal,
    params: [8]u16 = [_]u16{0} ** 8,
    param_count: usize = 0,
    current: u16 = 0,
    saw_digit: bool = false,

    fn resetCsi(self: *AnsiParser) void {
        self.param_count = 0;
        self.current = 0;
        self.saw_digit = false;
    }

    fn pushParam(self: *AnsiParser) void {
        if (self.param_count >= self.params.len) return;
        self.params[self.param_count] = if (self.saw_digit) self.current else 0;
        self.param_count += 1;
    }

    fn paramRaw(self: *const AnsiParser, idx: usize, default_value: u16) u16 {
        if (idx >= self.param_count) return default_value;
        return self.params[idx];
    }

    fn paramOr(self: *const AnsiParser, idx: usize, default_value: usize) usize {
        if (idx >= self.param_count) return default_value;
        const v = self.params[idx];
        return if (v == 0) default_value else @as(usize, v);
    }

    fn ansiColor(index: u16, bright: bool) u32 {
        const normal = [_]u32{
            0x00000000, // black
            0x00AA0000, // red
            0x0000AA00, // green
            0x00AA5500, // yellow
            0x000000AA, // blue
            0x00AA00AA, // magenta
            0x0000AAAA, // cyan
            0x00AAAAAA, // white
        };
        const bright_set = [_]u32{
            0x00555555, // bright black (gray)
            0x00FF5555, // bright red
            0x0055FF55, // bright green
            0x00FFFF55, // bright yellow
            0x005555FF, // bright blue
            0x00FF55FF, // bright magenta
            0x0055FFFF, // bright cyan
            0x00FFFFFF, // bright white
        };

        const idx = if (index < 8) index else 7;
        return if (bright) bright_set[idx] else normal[idx];
    }

    fn applySgr(self: *AnsiParser, c: *Console) void {
        if (self.param_count == 0) {
            c.fg = default_fg_color;
            c.bg = default_bg_color;
            return;
        }

        var i: usize = 0;
        while (i < self.param_count) : (i += 1) {
            const p = self.params[i];
            switch (p) {
                0 => {
                    c.fg = default_fg_color;
                    c.bg = default_bg_color;
                },
                39 => c.fg = default_fg_color,
                49 => c.bg = default_bg_color,
                30...37 => c.fg = ansiColor(p - 30, false),
                90...97 => c.fg = ansiColor(p - 90, true),
                40...47 => c.bg = ansiColor(p - 40, false),
                100...107 => c.bg = ansiColor(p - 100, true),
                else => {},
            }
        }
    }

    fn applyCsi(self: *AnsiParser, c: *Console, final: u8) void {
        switch (final) {
            'm' => self.applySgr(c),
            'H', 'f' => {
                const row1 = self.paramOr(0, 1);
                const col1 = self.paramOr(1, 1);
                const row0 = if (row1 > 0) row1 - 1 else 0;
                const col0 = if (col1 > 0) col1 - 1 else 0;
                c.setCursor(col0, row0);
            },
            'A' => {
                const n = self.paramOr(0, 1);
                c.cursor_y = if (n > c.cursor_y) 0 else c.cursor_y - n;
            },
            'B' => {
                const n = self.paramOr(0, 1);
                const y = c.cursor_y + n;
                c.cursor_y = if (y < rows) y else rows - 1;
            },
            'C' => {
                const n = self.paramOr(0, 1);
                const x = c.cursor_x + n;
                c.cursor_x = if (x < cols) x else cols - 1;
            },
            'D' => {
                const n = self.paramOr(0, 1);
                c.cursor_x = if (n > c.cursor_x) 0 else c.cursor_x - n;
            },
            'G' => {
                const col1 = self.paramOr(0, 1);
                const col0 = if (col1 > 0) col1 - 1 else 0;
                c.cursor_x = if (col0 < cols) col0 else cols - 1;
            },
            'J' => c.eraseDisplay(self.paramRaw(0, 0)),
            'K' => c.eraseLine(self.paramRaw(0, 0)),
            else => {},
        }
    }

    fn feed(self: *AnsiParser, c: *Console, ch: u8) void {
        switch (self.state) {
            .normal => {
                if (ch == 0x1B) {
                    self.state = .esc;
                    return;
                }
                c.putChar(ch);
            },
            .esc => {
                if (ch == '[') {
                    self.resetCsi();
                    self.state = .csi;
                    return;
                }
                self.state = .normal;
            },
            .csi => {
                if (ch >= '0' and ch <= '9') {
                    const digit: u16 = @intCast(ch - '0');
                    const next: u16 = self.current * 10 + digit;
                    self.current = if (next > 999) 999 else next;
                    self.saw_digit = true;
                    return;
                }

                if (ch == ';') {
                    self.pushParam();
                    self.current = 0;
                    self.saw_digit = false;
                    return;
                }

                if (ch >= 0x40 and ch <= 0x7E) {
                    if (self.saw_digit or self.param_count > 0) {
                        self.pushParam();
                    }
                    self.applyCsi(c, ch);
                }
                self.state = .normal;
            },
        }
    }
};

fn readU32LE(ptr: [*]const volatile u8, offset: usize) u32 {
    return @as(u32, ptr[offset]) |
        (@as(u32, ptr[offset + 1]) << 8) |
        (@as(u32, ptr[offset + 2]) << 16) |
        (@as(u32, ptr[offset + 3]) << 24);
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
        else => .{ 0x0E, 0x11, 0x01, 0x06, 0x04, 0x00, 0x04 },
    };
}

fn renderBootLogAndPrompt(c: *Console, log: [*]const volatile u8) void {
    c.clear();
    var parser = AnsiParser{};
    var text_len: usize = @intCast(readU32LE(log, 0));
    if (text_len > log_payload_bytes) text_len = log_payload_bytes;

    var i: usize = 0;
    while (i < text_len) : (i += 1) {
        parser.feed(c, log[log_header_bytes + i]);
    }

    c.newline();
    c.fg = prompt_fg_color;
    for (enter_prompt_text) |ch| {
        c.putChar(ch);
    }
    c.fg = default_fg_color;
}

pub export fn _start() noreturn {
    const fb: [*]volatile u32 = @ptrFromInt(framebuffer_va);
    const log: [*]const volatile u8 = @ptrFromInt(boot_log_page_va);

    var c = Console{ .fb = fb };
    renderBootLogAndPrompt(&c, log);
    while (true) {
        asm volatile ("pause");
    }
}
