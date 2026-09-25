const std = @import("std");

/// Paint a conspicuous upper-screen checkerboard without allocating memory or
/// changing page tables. This is diagnostic-only: no console or TTY is added.
pub fn paint(address: ?*anyopaque, width: u64, height: u64, pitch: u64,
    bpp: u16, rgb_model: bool) bool
{
    if (address == null or !rgb_model or bpp != 32 or width == 0 or height == 0 or
        pitch == 0 or (pitch & 3) != 0 or width > pitch / 4) return false;
    const rows = @min(height, 768);
    const bytes = std.math.mul(u64, pitch, rows) catch return false;
    if (bytes > std.math.maxInt(usize)) return false;
    // Byte stores avoid assuming that a firmware-provided framebuffer base is
    // naturally aligned for u32, which would make a failed probe ambiguous.
    const pixels: [*]volatile u8 = @ptrCast(address.?);
    const row_pitch: usize = @intCast(pitch);
    for (0..@intCast(rows)) |y| {
        for (0..@intCast(width)) |x| {
            // White and magenta remain distinguishable with either common
            // red/blue mask ordering used by UEFI GOP implementations.
            const color: u32 = if ((((x / 64) ^ (y / 64)) & 1) == 0)
                0x00ffffff else 0x00ff00ff;
            const offset = y * row_pitch + x * 4;
            pixels[offset] = @truncate(color);
            pixels[offset + 1] = @truncate(color >> 8);
            pixels[offset + 2] = @truncate(color >> 16);
            pixels[offset + 3] = 0;
        }
    }
    // Limine may map framebuffer memory write-combining on physical GPUs.
    // Order the stores before halting so a queued write is not mistaken for
    // an absent GOP response in this one-shot visual diagnostic.
    asm volatile ("sfence" ::: .{ .memory = true });
    return true;
}

test "GOP probe paints a bounded visible pattern" {
    var pixels: [128 * 128]u32 = @splat(0);
    try std.testing.expect(paint(&pixels, 128, 128, 128 * 4, 32, true));
    try std.testing.expectEqual(@as(u32, 0x00ffffff), pixels[0]);
    try std.testing.expectEqual(@as(u32, 0x00ff00ff), pixels[64]);
    try std.testing.expectEqual(@as(u32, 0x00ff00ff), pixels[64 * 128]);
    try std.testing.expectEqual(@as(u32, 0x00ffffff), pixels[64 * 128 + 64]);
    try std.testing.expect(!paint(&pixels, 128, 128, 127 * 4, 32, true));
    try std.testing.expect(!paint(&pixels, 128, 128, 128 * 4, 24, true));
}
