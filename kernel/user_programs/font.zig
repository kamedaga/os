const generated = @import("generated_font.zig");

pub const glyph_width: usize = generated.cell_width;
pub const glyph_height: usize = generated.cell_height;
pub const glyph_advance: usize = generated.console_advance;
pub const fallback_codepoint: u21 = '?';

const Glyph = struct {
    advance: i32,
    bearing_x: i32,
    top_offset: i32,
    width: usize,
    height: usize,
    mask_offset: usize,
};

fn normalizedScale(scale: i32) i32 {
    return if (scale <= 0) 1 else scale;
}

pub fn scaledGlyphWidth(scale: i32) i32 {
    return @as(i32, @intCast(glyph_width)) * normalizedScale(scale);
}

pub fn scaledGlyphHeight(scale: i32) i32 {
    return @as(i32, @intCast(glyph_height)) * normalizedScale(scale);
}

pub fn scaledAdvance(scale: i32) i32 {
    return @as(i32, @intCast(glyph_advance)) * normalizedScale(scale);
}

pub fn lineHeight(scale: i32) i32 {
    return @as(i32, @intCast(generated.line_height)) * normalizedScale(scale);
}

pub fn consoleAdvance(scale: i32) i32 {
    return @as(i32, @intCast(generated.console_advance)) * normalizedScale(scale);
}

pub fn blendColor(dst: u32, src: u32, alpha: u8) u32 {
    if (alpha == 0) return dst;
    if (alpha == 255) return src;

    const src_a: u32 = alpha;
    const inv_a: u32 = 255 - src_a;

    const sr = (src >> 16) & 0xFF;
    const sg = (src >> 8) & 0xFF;
    const sb = src & 0xFF;
    const dr = (dst >> 16) & 0xFF;
    const dg = (dst >> 8) & 0xFF;
    const db = dst & 0xFF;

    const r = (sr * src_a + dr * inv_a + 127) / 255;
    const g = (sg * src_a + dg * inv_a + 127) / 255;
    const b = (sb * src_a + db * inv_a + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

pub fn blendSubpixelColor(dst: u32, src: u32, alpha_r: u8, alpha_g: u8, alpha_b: u8) u32 {
    const sr = (src >> 16) & 0xFF;
    const sg = (src >> 8) & 0xFF;
    const sb = src & 0xFF;
    const dr = (dst >> 16) & 0xFF;
    const dg = (dst >> 8) & 0xFF;
    const db = dst & 0xFF;

    const r = (sr * @as(u32, alpha_r) + dr * @as(u32, 255 - alpha_r) + 127) / 255;
    const g = (sg * @as(u32, alpha_g) + dg * @as(u32, 255 - alpha_g) + 127) / 255;
    const b = (sb * @as(u32, alpha_b) + db * @as(u32, 255 - alpha_b) + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

pub fn measureAsciiText(text: []const u8, scale: i32) i32 {
    if (text.len == 0) return 0;
    var width: i32 = 0;
    for (text) |ch| {
        width += glyphAdvance(ch, scale);
    }
    return width;
}

fn isContinuationByte(byte: u8) bool {
    return (byte & 0xC0) == 0x80;
}

pub fn decodeNextUtf8(text: anytype, index: *usize) u21 {
    if (index.* >= text.len) return fallback_codepoint;

    const first = text[index.*];
    if (first < 0x80) {
        index.* += 1;
        return first;
    }

    var needed: usize = 0;
    var codepoint: u21 = 0;
    var minimum: u21 = 0;

    if (first >= 0xC2 and first <= 0xDF) {
        needed = 1;
        codepoint = first & 0x1F;
        minimum = 0x80;
    } else if (first >= 0xE0 and first <= 0xEF) {
        needed = 2;
        codepoint = first & 0x0F;
        minimum = 0x800;
    } else if (first >= 0xF0 and first <= 0xF4) {
        needed = 3;
        codepoint = first & 0x07;
        minimum = 0x10000;
    } else {
        index.* += 1;
        return fallback_codepoint;
    }

    if (index.* + needed >= text.len) {
        index.* += 1;
        return fallback_codepoint;
    }

    var i: usize = 1;
    while (i <= needed) : (i += 1) {
        const byte = text[index.* + i];
        if (!isContinuationByte(byte)) {
            index.* += 1;
            return fallback_codepoint;
        }
        codepoint = (codepoint << 6) | @as(u21, byte & 0x3F);
    }

    if (codepoint < minimum or codepoint > 0x10FFFF or (codepoint >= 0xD800 and codepoint <= 0xDFFF)) {
        index.* += 1;
        return fallback_codepoint;
    }

    index.* += needed + 1;
    return codepoint;
}

pub fn measureUtf8Text(text: []const u8, scale: i32) i32 {
    if (text.len == 0) return 0;

    var width: i32 = 0;
    var index: usize = 0;
    while (index < text.len) {
        const codepoint = decodeNextUtf8(text, &index);
        width += glyphAdvance(codepoint, scale);
    }
    return width;
}

fn glyphIndex(codepoint: u21) usize {
    const glyphs = generated.glyphs[0..];
    var lo: usize = 0;
    var hi: usize = glyphs.len;
    while (lo < hi) {
        const mid = lo + (hi - lo) / 2;
        const candidate = glyphs[mid].codepoint;
        if (candidate == codepoint) return mid;
        if (candidate < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return generated.fallback_index;
}

fn glyph(codepoint: u21) Glyph {
    const raw = generated.glyphs[glyphIndex(codepoint)];
    return .{
        .advance = raw.advance,
        .bearing_x = raw.bearing_x,
        .top_offset = raw.top_offset,
        .width = raw.width,
        .height = raw.height,
        .mask_offset = raw.mask_offset,
    };
}

pub fn glyphAdvance(codepoint: u21, scale: i32) i32 {
    return glyph(codepoint).advance * normalizedScale(scale);
}

pub fn drawGlyph(
    comptime Context: type,
    comptime blendPixel: fn (ctx: Context, x: i32, y: i32, color: u32, alpha: u8) void,
    ctx: Context,
    x: i32,
    y: i32,
    codepoint: u21,
    color: u32,
    scale: i32,
) void {
    const g = glyph(codepoint);
    if (g.width == 0 or g.height == 0) return;
    const scale_px = normalizedScale(scale);
    const mask = generated.glyph_mask_bytes[g.mask_offset .. g.mask_offset + g.width * g.height];

    var gy: usize = 0;
    while (gy < g.height) : (gy += 1) {
        const row_y = y + g.top_offset * scale_px + @as(i32, @intCast(gy)) * scale_px;
        var gx: usize = 0;
        while (gx < g.width) : (gx += 1) {
            const alpha = mask[gy * g.width + gx];
            if (alpha == 0) continue;

            const col_x = x + g.bearing_x * scale_px + @as(i32, @intCast(gx)) * scale_px;
            var sy: i32 = 0;
            while (sy < scale_px) : (sy += 1) {
                var sx: i32 = 0;
                while (sx < scale_px) : (sx += 1) {
                    blendPixel(ctx, col_x + sx, row_y + sy, color, alpha);
                }
            }
        }
    }
}

fn subpixelAlpha(center: u8, neighbor: u8) u8 {
    return @intCast((@as(u32, center) * 5 + @as(u32, neighbor) * 3 + 4) / 8);
}

pub fn drawGlyphSubpixel(
    comptime Context: type,
    comptime blendPixel: fn (ctx: Context, x: i32, y: i32, color: u32, alpha_r: u8, alpha_g: u8, alpha_b: u8) void,
    ctx: Context,
    x: i32,
    y: i32,
    codepoint: u21,
    color: u32,
    scale: i32,
) void {
    const g = glyph(codepoint);
    if (g.width == 0 or g.height == 0) return;
    const scale_px = normalizedScale(scale);
    const mask = generated.glyph_mask_bytes[g.mask_offset .. g.mask_offset + g.width * g.height];

    var gy: usize = 0;
    while (gy < g.height) : (gy += 1) {
        const row_y = y + g.top_offset * scale_px + @as(i32, @intCast(gy)) * scale_px;
        var gx: usize = 0;
        while (gx < g.width) : (gx += 1) {
            const alpha_g = mask[gy * g.width + gx];
            if (alpha_g == 0) continue;

            const alpha_r = subpixelAlpha(alpha_g, if (gx > 0) mask[gy * g.width + gx - 1] else 0);
            const alpha_b = subpixelAlpha(alpha_g, if (gx + 1 < g.width) mask[gy * g.width + gx + 1] else 0);
            const col_x = x + g.bearing_x * scale_px + @as(i32, @intCast(gx)) * scale_px;

            var sy: i32 = 0;
            while (sy < scale_px) : (sy += 1) {
                var sx: i32 = 0;
                while (sx < scale_px) : (sx += 1) {
                    blendPixel(ctx, col_x + sx, row_y + sy, color, alpha_r, alpha_g, alpha_b);
                }
            }
        }
    }
}

pub fn drawAsciiTextClipped(
    comptime Context: type,
    comptime blendPixel: fn (ctx: Context, x: i32, y: i32, color: u32, alpha: u8) void,
    ctx: Context,
    x: i32,
    y: i32,
    text: []const u8,
    color: u32,
    scale: i32,
    max_x: i32,
) void {
    var pen_x = x;

    for (text) |ch| {
        const advance = glyphAdvance(ch, scale);
        if (pen_x + advance > max_x) break;
        drawGlyph(Context, blendPixel, ctx, pen_x, y, ch, color, scale);
        pen_x += advance;
    }
}

pub fn drawUtf8TextClipped(
    comptime Context: type,
    comptime blendPixel: fn (ctx: Context, x: i32, y: i32, color: u32, alpha: u8) void,
    ctx: Context,
    x: i32,
    y: i32,
    text: []const u8,
    color: u32,
    scale: i32,
    max_x: i32,
) void {
    var pen_x = x;
    var index: usize = 0;

    while (index < text.len) {
        const codepoint = decodeNextUtf8(text, &index);
        const advance = glyphAdvance(codepoint, scale);
        if (pen_x + advance > max_x) break;
        drawGlyph(Context, blendPixel, ctx, pen_x, y, codepoint, color, scale);
        pen_x += advance;
    }
}

pub fn drawUtf8TextSubpixelClipped(
    comptime Context: type,
    comptime blendPixel: fn (ctx: Context, x: i32, y: i32, color: u32, alpha_r: u8, alpha_g: u8, alpha_b: u8) void,
    ctx: Context,
    x: i32,
    y: i32,
    text: []const u8,
    color: u32,
    scale: i32,
    max_x: i32,
) void {
    var pen_x = x;
    var index: usize = 0;

    while (index < text.len) {
        const codepoint = decodeNextUtf8(text, &index);
        const advance = glyphAdvance(codepoint, scale);
        if (pen_x + advance > max_x) break;
        drawGlyphSubpixel(Context, blendPixel, ctx, pen_x, y, codepoint, color, scale);
        pen_x += advance;
    }
}
