const generated = @import("generated_font.zig");

// Keep font data in writable storage so user programs do not depend on direct
// reads from generated .rodata.
fn buildRuntimeGlyphs() [generated.glyphs.len]generated.Glyph {
    @setEvalBranchQuota(generated.glyphs.len * 4);
    var data: [generated.glyphs.len]generated.Glyph = undefined;
    inline for (generated.glyphs, 0..) |glyph_data, i| {
        data[i] = glyph_data;
    }
    return data;
}

fn buildRuntimeGlyphMaskBytes() [generated.glyph_mask_bytes.len]u8 {
    @setEvalBranchQuota(generated.glyph_mask_bytes.len * 4);
    var data: [generated.glyph_mask_bytes.len]u8 = undefined;
    inline for (generated.glyph_mask_bytes, 0..) |byte, i| {
        data[i] = byte;
    }
    return data;
}

var runtime_glyphs = buildRuntimeGlyphs();
var runtime_glyph_mask_bytes = buildRuntimeGlyphMaskBytes();

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

const ScaleRatio = struct {
    num: i64,
    den: i64,
};

fn normalizedScaleRatio(scale_num: i32, scale_den: i32) ScaleRatio {
    const den: i64 = if (scale_den <= 0) 1 else scale_den;
    const num: i64 = if (scale_num <= 0) den else scale_num;
    return .{ .num = num, .den = den };
}

fn divFloorI64(numer: i64, denom: i64) i64 {
    var q = @divTrunc(numer, denom);
    const r = @rem(numer, denom);
    if (r != 0 and ((r < 0) != (denom < 0))) q -= 1;
    return q;
}

fn divCeilI64(numer: i64, denom: i64) i64 {
    var q = @divTrunc(numer, denom);
    const r = @rem(numer, denom);
    if (r != 0 and ((r < 0) == (denom < 0))) q += 1;
    return q;
}

fn scalePositiveCeil(value: usize, scale: ScaleRatio) usize {
    if (value == 0) return 0;
    const scaled = divCeilI64(@as(i64, @intCast(value)) * scale.num, scale.den);
    const out: usize = @intCast(scaled);
    return if (out == 0) 1 else out;
}

fn scaleSignedFloor(value: i32, scale: ScaleRatio) i32 {
    return @intCast(divFloorI64(@as(i64, value) * scale.num, scale.den));
}

fn scaleSignedCeil(value: i32, scale: ScaleRatio) i32 {
    return @intCast(divCeilI64(@as(i64, value) * scale.num, scale.den));
}

const ScaledGlyph = struct {
    advance: i32,
    bearing_x: i32,
    top_offset: i32,
    width: usize,
    height: usize,
};

fn scaledGlyph(g: Glyph, scale: ScaleRatio) ScaledGlyph {
    const advance = scaleSignedCeil(g.advance, scale);
    if (g.width == 0 or g.height == 0) {
        return .{
            .advance = advance,
            .bearing_x = scaleSignedFloor(g.bearing_x, scale),
            .top_offset = scaleSignedFloor(g.top_offset, scale),
            .width = 0,
            .height = 0,
        };
    }

    const width = scalePositiveCeil(g.width, scale);
    const right = scaleSignedCeil(g.bearing_x + @as(i32, @intCast(g.width)), scale);
    const height = scalePositiveCeil(g.height, scale);
    const bottom = scaleSignedCeil(g.top_offset + @as(i32, @intCast(g.height)), scale);
    return .{
        .advance = advance,
        .bearing_x = right - @as(i32, @intCast(width)),
        .top_offset = bottom - @as(i32, @intCast(height)),
        .width = width,
        .height = height,
    };
}

pub fn scaledGlyphWidth(scale: i32) i32 {
    return @as(i32, @intCast(glyph_width)) * normalizedScale(scale);
}

pub fn scaledGlyphWidthRatio(scale_num: i32, scale_den: i32) i32 {
    return @intCast(scalePositiveCeil(glyph_width, normalizedScaleRatio(scale_num, scale_den)));
}

pub fn scaledGlyphHeight(scale: i32) i32 {
    return @as(i32, @intCast(glyph_height)) * normalizedScale(scale);
}

pub fn scaledGlyphHeightRatio(scale_num: i32, scale_den: i32) i32 {
    return @intCast(scalePositiveCeil(glyph_height, normalizedScaleRatio(scale_num, scale_den)));
}

pub fn scaledAdvance(scale: i32) i32 {
    return @as(i32, @intCast(glyph_advance)) * normalizedScale(scale);
}

pub fn scaledAdvanceRatio(scale_num: i32, scale_den: i32) i32 {
    return @intCast(scalePositiveCeil(glyph_advance, normalizedScaleRatio(scale_num, scale_den)));
}

pub fn lineHeight(scale: i32) i32 {
    return @as(i32, @intCast(generated.line_height)) * normalizedScale(scale);
}

pub fn lineHeightRatio(scale_num: i32, scale_den: i32) i32 {
    return @intCast(scalePositiveCeil(generated.line_height, normalizedScaleRatio(scale_num, scale_den)));
}

pub fn consoleAdvance(scale: i32) i32 {
    return @as(i32, @intCast(generated.console_advance)) * normalizedScale(scale);
}

pub fn consoleAdvanceRatio(scale_num: i32, scale_den: i32) i32 {
    return @intCast(scalePositiveCeil(generated.console_advance, normalizedScaleRatio(scale_num, scale_den)));
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
    var lo: usize = 0;
    var hi: usize = runtime_glyphs.len;
    while (lo < hi) {
        const mid = lo + (hi - lo) / 2;
        const candidate = runtime_glyphs[mid].codepoint;
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
    const raw = runtime_glyphs[glyphIndex(codepoint)];
    return .{
        .advance = raw.advance,
        .bearing_x = raw.bearing_x,
        .top_offset = raw.top_offset,
        .width = raw.width,
        .height = raw.height,
        .mask_offset = raw.mask_offset,
    };
}

fn glyphMaskByte(offset: usize) u8 {
    return runtime_glyph_mask_bytes[offset];
}

pub fn glyphAdvance(codepoint: u21, scale: i32) i32 {
    return glyph(codepoint).advance * normalizedScale(scale);
}

pub fn glyphAdvanceRatio(codepoint: u21, scale_num: i32, scale_den: i32) i32 {
    return scaledGlyph(glyph(codepoint), normalizedScaleRatio(scale_num, scale_den)).advance;
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

    var gy: usize = 0;
    while (gy < g.height) : (gy += 1) {
        const row_y = y + g.top_offset * scale_px + @as(i32, @intCast(gy)) * scale_px;
        var gx: usize = 0;
        while (gx < g.width) : (gx += 1) {
            const alpha = glyphMaskByte(g.mask_offset + gy * g.width + gx);
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

pub fn drawGlyphRatio(
    comptime Context: type,
    comptime blendPixel: fn (ctx: Context, x: i32, y: i32, color: u32, alpha: u8) void,
    ctx: Context,
    x: i32,
    y: i32,
    codepoint: u21,
    color: u32,
    scale_num: i32,
    scale_den: i32,
) void {
    const scale = normalizedScaleRatio(scale_num, scale_den);
    const g = glyph(codepoint);
    const scaled = scaledGlyph(g, scale);
    if (scaled.width == 0 or scaled.height == 0) return;

    var gy: usize = 0;
    while (gy < scaled.height) : (gy += 1) {
        const src_y: usize = @intCast(@divTrunc(@as(i64, @intCast(gy)) * scale.den, scale.num));
        const row_y = y + scaled.top_offset + @as(i32, @intCast(gy));
        var gx: usize = 0;
        while (gx < scaled.width) : (gx += 1) {
            const src_x: usize = @intCast(@divTrunc(@as(i64, @intCast(gx)) * scale.den, scale.num));
            const alpha = glyphMaskByte(g.mask_offset + src_y * g.width + src_x);
            if (alpha == 0) continue;
            const col_x = x + scaled.bearing_x + @as(i32, @intCast(gx));
            blendPixel(ctx, col_x, row_y, color, alpha);
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

    var gy: usize = 0;
    while (gy < g.height) : (gy += 1) {
        const row_y = y + g.top_offset * scale_px + @as(i32, @intCast(gy)) * scale_px;
        var gx: usize = 0;
        while (gx < g.width) : (gx += 1) {
            const mask_index = g.mask_offset + gy * g.width + gx;
            const alpha_g = glyphMaskByte(mask_index);
            if (alpha_g == 0) continue;

            const alpha_r = subpixelAlpha(alpha_g, if (gx > 0) glyphMaskByte(mask_index - 1) else 0);
            const alpha_b = subpixelAlpha(alpha_g, if (gx + 1 < g.width) glyphMaskByte(mask_index + 1) else 0);
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

pub fn drawAsciiTextClippedRatio(
    comptime Context: type,
    comptime blendPixel: fn (ctx: Context, x: i32, y: i32, color: u32, alpha: u8) void,
    ctx: Context,
    x: i32,
    y: i32,
    text: []const u8,
    color: u32,
    scale_num: i32,
    scale_den: i32,
    max_x: i32,
) void {
    var pen_x = x;

    for (text) |ch| {
        const advance = glyphAdvanceRatio(ch, scale_num, scale_den);
        if (pen_x + advance > max_x) break;
        drawGlyphRatio(Context, blendPixel, ctx, pen_x, y, ch, color, scale_num, scale_den);
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
