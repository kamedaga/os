const std = @import("std");

const syscall_log: u64 = 0x9;

const shared_page_va: usize = 0x2000_3000;
const framebuffer_va: usize = 0x2000_4000;

const shared_magic: u64 = 0x4D534852; // "MSHR"
const shared_header_bytes: usize = 128;
const shared_log_max_bytes: usize = 4096 - shared_header_bytes;

const cell_w: usize = 16;
const cell_h: usize = 16;
const glyph_w: usize = 5;
const glyph_h: usize = 7;
const glyph_scale: usize = 2;

const cursor_size_px: usize = 11;
const cursor_size_i32: i32 = 11;
const cursor_half: i32 = cursor_size_i32 / 2;

const default_fg_color: u32 = 0x00FF_FFFF;
const default_bg_color: u32 = 0x0000_0000;

const ParserState = enum {
    normal,
    esc,
    csi,
};

const Console = struct {
    fb: [*]volatile u32,
    width: usize,
    height: usize,
    pitch: usize,
    cursor_x: usize = 0,
    cursor_y: usize = 0,
    fg: u32 = default_fg_color,
    bg: u32 = default_bg_color,

    fn cols(self: *const Console) usize {
        const c = self.width / cell_w;
        return if (c == 0) 1 else c;
    }

    fn rows(self: *const Console) usize {
        const r = self.height / cell_h;
        return if (r == 0) 1 else r;
    }

    fn fillRectPx(self: *Console, x0: usize, y0: usize, width: usize, height: usize, color: u32) void {
        if (width == 0 or height == 0) return;
        if (x0 >= self.width or y0 >= self.height) return;

        const end_x = if (x0 + width > self.width) self.width else x0 + width;
        const end_y = if (y0 + height > self.height) self.height else y0 + height;

        var y: usize = y0;
        while (y < end_y) : (y += 1) {
            const row = y * self.pitch;
            var x: usize = x0;
            while (x < end_x) : (x += 1) {
                self.fb[row + x] = color;
            }
        }
    }

    fn clear(self: *Console) void {
        self.fillRectPx(0, 0, self.width, self.height, self.bg);
        self.cursor_x = 0;
        self.cursor_y = 0;
    }

    fn clearCell(self: *Console, col: usize, row: usize) void {
        self.fillRectPx(col * cell_w, row * cell_h, cell_w, cell_h, self.bg);
    }

    fn clearCellsInRow(self: *Console, row: usize, start_col: usize, end_col_exclusive: usize) void {
        const ccols = self.cols();
        if (row >= self.rows()) return;
        if (start_col >= ccols) return;
        const end_col = if (end_col_exclusive > ccols) ccols else end_col_exclusive;
        if (start_col >= end_col) return;
        self.fillRectPx(start_col * cell_w, row * cell_h, (end_col - start_col) * cell_w, cell_h, self.bg);
    }

    fn scrollUp(self: *Console) void {
        if (self.height <= cell_h) {
            self.clear();
            return;
        }
        var y: usize = 0;
        while (y + cell_h < self.height) : (y += 1) {
            const dst_row = y * self.pitch;
            const src_row = (y + cell_h) * self.pitch;
            var x: usize = 0;
            while (x < self.width) : (x += 1) {
                self.fb[dst_row + x] = self.fb[src_row + x];
            }
        }
        self.fillRectPx(0, self.height - cell_h, self.width, cell_h, self.bg);
    }

    fn setCursor(self: *Console, col: usize, row: usize) void {
        const ccols = self.cols();
        const crows = self.rows();
        self.cursor_x = if (col < ccols) col else ccols - 1;
        self.cursor_y = if (row < crows) row else crows - 1;
    }

    fn newline(self: *Console) void {
        const crows = self.rows();
        self.cursor_x = 0;
        self.cursor_y += 1;
        if (self.cursor_y >= crows) {
            self.scrollUp();
            self.cursor_y = crows - 1;
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
                    const draw_y = base_y + sy;
                    if (draw_y >= self.height) continue;
                    const row = draw_y * self.pitch;
                    var sx: usize = 0;
                    while (sx < glyph_scale) : (sx += 1) {
                        const draw_x = base_x + sx;
                        if (draw_x >= self.width) continue;
                        self.fb[row + draw_x] = self.fg;
                    }
                }
            }
        }
    }

    fn putPrintable(self: *Console, ch: u8) void {
        const ccols = self.cols();
        if (self.cursor_x >= ccols) self.newline();
        self.clearCell(self.cursor_x, self.cursor_y);
        self.drawGlyph(ch);
        self.cursor_x += 1;
        if (self.cursor_x >= ccols) self.newline();
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
                const ccols = self.cols();
                if (self.cursor_x > 0) {
                    self.cursor_x -= 1;
                } else if (self.cursor_y > 0) {
                    self.cursor_y -= 1;
                    self.cursor_x = ccols - 1;
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
        const ccols = self.cols();
        const crows = self.rows();
        switch (mode) {
            2 => self.clear(),
            1 => {
                var r: usize = 0;
                while (r < self.cursor_y) : (r += 1) {
                    self.clearCellsInRow(r, 0, ccols);
                }
                self.clearCellsInRow(self.cursor_y, 0, self.cursor_x + 1);
            },
            else => {
                self.clearCellsInRow(self.cursor_y, self.cursor_x, ccols);
                var r: usize = self.cursor_y + 1;
                while (r < crows) : (r += 1) {
                    self.clearCellsInRow(r, 0, ccols);
                }
            },
        }
    }

    fn eraseLine(self: *Console, mode: u16) void {
        const ccols = self.cols();
        switch (mode) {
            2 => self.clearCellsInRow(self.cursor_y, 0, ccols),
            1 => self.clearCellsInRow(self.cursor_y, 0, self.cursor_x + 1),
            else => self.clearCellsInRow(self.cursor_y, self.cursor_x, ccols),
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
            0x00000000,
            0x00AA0000,
            0x0000AA00,
            0x00AA5500,
            0x000000AA,
            0x00AA00AA,
            0x0000AAAA,
            0x00AAAAAA,
        };
        const bright_set = [_]u32{
            0x00555555,
            0x00FF5555,
            0x0055FF55,
            0x00FFFF55,
            0x005555FF,
            0x00FF55FF,
            0x0055FFFF,
            0x00FFFFFF,
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
        const ccols = c.cols();
        const crows = c.rows();
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
                c.cursor_y = if (y < crows) y else crows - 1;
            },
            'C' => {
                const n = self.paramOr(0, 1);
                const x = c.cursor_x + n;
                c.cursor_x = if (x < ccols) x else ccols - 1;
            },
            'D' => {
                const n = self.paramOr(0, 1);
                c.cursor_x = if (n > c.cursor_x) 0 else c.cursor_x - n;
            },
            'G' => {
                const col1 = self.paramOr(0, 1);
                const col0 = if (col1 > 0) col1 - 1 else 0;
                c.cursor_x = if (col0 < ccols) col0 else ccols - 1;
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

const CursorOverlay = struct {
    valid: bool = false,
    x0: i32 = 0,
    y0: i32 = 0,
    w: i32 = 0,
    h: i32 = 0,
    saved: [cursor_size_px * cursor_size_px]u32 = [_]u32{0} ** (cursor_size_px * cursor_size_px),
};

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .memory = true }
    );
}

fn clampI32(v: i32, lo: i32, hi: i32) i32 {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

fn absI32(v: i32) i32 {
    return if (v < 0) -v else v;
}

fn restoreCursor(overlay: *CursorOverlay, fb: [*]volatile u32, pitch: usize) void {
    if (!overlay.valid) return;
    const w: usize = @intCast(overlay.w);
    const h: usize = @intCast(overlay.h);
    var py: usize = 0;
    while (py < h) : (py += 1) {
        const row: usize = @as(usize, @intCast(overlay.y0 + @as(i32, @intCast(py)))) * pitch;
        var px: usize = 0;
        while (px < w) : (px += 1) {
            const idx = row + @as(usize, @intCast(overlay.x0 + @as(i32, @intCast(px))));
            fb[idx] = overlay.saved[py * cursor_size_px + px];
        }
    }
    overlay.valid = false;
}

fn saveAndDrawCursor(
    overlay: *CursorOverlay,
    fb: [*]volatile u32,
    width: i32,
    height: i32,
    pitch: usize,
    cx: i32,
    cy: i32,
    buttons: u64,
) void {
    var x0 = cx - cursor_half;
    var y0 = cy - cursor_half;
    var x1 = cx + cursor_half + 1;
    var y1 = cy + cursor_half + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    if (x0 >= x1 or y0 >= y1) {
        overlay.valid = false;
        return;
    }

    const color: u32 = if ((buttons & 0x1) != 0) 0x00FF_4040 else 0x00FF_FFFF;
    const w: usize = @intCast(x1 - x0);
    const h: usize = @intCast(y1 - y0);
    overlay.x0 = x0;
    overlay.y0 = y0;
    overlay.w = @intCast(w);
    overlay.h = @intCast(h);
    overlay.valid = true;

    var py: usize = 0;
    while (py < h) : (py += 1) {
        const gy = y0 + @as(i32, @intCast(py));
        const row = @as(usize, @intCast(gy)) * pitch;
        var px: usize = 0;
        while (px < w) : (px += 1) {
            const gx = x0 + @as(i32, @intCast(px));
            const idx = row + @as(usize, @intCast(gx));
            overlay.saved[py * cursor_size_px + px] = fb[idx];

            const dx = gx - cx;
            const dy = gy - cy;
            const edge = absI32(dx) == cursor_half or absI32(dy) == cursor_half;
            const cross = dx == 0 or dy == 0;
            const diagonal = absI32(dx) == absI32(dy);
            if (edge or cross or diagonal) {
                fb[idx] = color;
            }
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
        else => .{ 0x0E, 0x11, 0x01, 0x06, 0x04, 0x00, 0x04 },
    };
}

pub export fn _start() noreturn {
    _ = userLog("Compositor: started\n");

    const shared_words: [*]volatile u64 = @ptrFromInt(shared_page_va);
    if (shared_words[0] != shared_magic) {
        _ = userLog("Compositor: shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }

    const width: usize = @intCast(shared_words[1]);
    const height: usize = @intCast(shared_words[2]);
    const pitch: usize = @intCast(shared_words[3]);
    if (width < cell_w or height < cell_h or pitch < width or width > @as(usize, std.math.maxInt(i32)) or height > @as(usize, std.math.maxInt(i32))) {
        _ = userLog("Compositor: invalid framebuffer info\n");
        while (true) asm volatile ("pause");
    }

    const width_i32: i32 = @intCast(width);
    const height_i32: i32 = @intCast(height);
    const fb: [*]volatile u32 = @ptrFromInt(framebuffer_va);
    const shared_bytes: [*]const volatile u8 = @ptrFromInt(shared_page_va);

    var console = Console{
        .fb = fb,
        .width = width,
        .height = height,
        .pitch = pitch,
    };
    console.clear();

    var parser = AnsiParser{};
    var text_len: usize = @intCast(shared_words[9]);
    if (text_len > shared_log_max_bytes) text_len = shared_log_max_bytes;
    var i: usize = 0;
    while (i < text_len) : (i += 1) {
        parser.feed(&console, shared_bytes[shared_header_bytes + i]);
    }
    _ = userLog("Compositor: boot log rendered\n");

    var overlay = CursorOverlay{};
    var x: i32 = clampI32(@intCast(shared_words[4]), 0, width_i32 - 1);
    var y: i32 = clampI32(@intCast(shared_words[5]), 0, height_i32 - 1);
    var buttons: u64 = shared_words[6];
    saveAndDrawCursor(&overlay, fb, width_i32, height_i32, pitch, x, y, buttons);

    var last_seq: u64 = shared_words[7];
    while (true) {
        const seq = shared_words[7];
        if (seq == last_seq) {
            asm volatile ("pause");
            continue;
        }
        last_seq = seq;

        restoreCursor(&overlay, fb, pitch);
        x = clampI32(@intCast(shared_words[4]), 0, width_i32 - 1);
        y = clampI32(@intCast(shared_words[5]), 0, height_i32 - 1);
        buttons = shared_words[6];
        saveAndDrawCursor(&overlay, fb, width_i32, height_i32, pitch, x, y, buttons);
    }
}
