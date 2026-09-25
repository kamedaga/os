const std = @import("std");

const block_size: u64 = 40;
const gap: u64 = 12;
const margin: u64 = 8;
const colors = [_]u32{
    0x00ff3030, // entered the kernel
    0x00ff9c30, // boot prelude completed
    0x00ffff30, // Limine resources probed
    0x0030ff30, // SMP resources checked
    0x0030cfff, // kernel storage ready
    0x00ff30ff, // boot resources persisted
    0x0030ff30, // diagnostic framebuffer mapping admission passed
    0x00ff3030, // diagnostic framebuffer mapping admission failed
};

/// Temporary real-machine diagnosis before the kernel replaces Limine's page
/// tables. This writes only a bounded row of pixels, not a console or TTY.
/// Never call it after the CR3 switch: Limine's framebuffer VA may be gone.
pub fn mark(address: ?*anyopaque, width: u64, height: u64, pitch: u64,
    bpp: u16, rgb_model: bool, stage: usize) bool
{
    if (address == null or stage >= colors.len or !rgb_model or bpp != 32 or
        pitch == 0 or (pitch & 3) != 0 or width == 0 or height == 0 or
        width > pitch / 4) return false;
    // Either admission result occupies the same seventh slot.
    const x0 = margin + @as(u64, @intCast(@min(stage, 6))) * (block_size + gap);
    const y0 = margin;
    if (x0 + block_size > width or y0 + block_size > height) return false;
    const bytes = std.math.mul(u64, pitch, height) catch return false;
    if (bytes > std.math.maxInt(usize)) return false;
    const pixels: [*]volatile u8 = @ptrCast(address.?);
    const color = colors[stage];
    for (0..block_size) |row| {
        for (0..block_size) |column| {
            const offset: usize = @intCast((y0 + row) * pitch +
                (x0 + column) * 4);
            pixels[offset] = @truncate(color);
            pixels[offset + 1] = @truncate(color >> 8);
            pixels[offset + 2] = @truncate(color >> 16);
            pixels[offset + 3] = 0;
        }
    }
    return true;
}

test "early GOP markers are bounded and distinguish boot stages" {
    var pixels: [320 * 64 * 4]u8 = @splat(0);
    try std.testing.expect(mark(&pixels, 320, 64, 320 * 4, 32, true, 0));
    try std.testing.expect(mark(&pixels, 320, 64, 320 * 4, 32, true, 5));
    const first = (8 * 320 + 8) * 4;
    const last = (8 * 320 + (8 + 5 * 52)) * 4;
    try std.testing.expectEqual(@as(u8, 0x30), pixels[first]);
    try std.testing.expectEqual(@as(u8, 0xff), pixels[first + 2]);
    try std.testing.expectEqual(@as(u8, 0xff), pixels[last]);
    try std.testing.expectEqual(@as(u8, 0xff), pixels[last + 2]);
    try std.testing.expectEqual(@as(u8, 0), pixels[0]);
    try std.testing.expect(!mark(&pixels, 320, 64, 320 * 4, 32, true, 6));
    try std.testing.expect(!mark(&pixels, 320, 64, 100, 32, true, 0));
    try std.testing.expect(!mark(&pixels, 320, 64, 320 * 4, 24, true, 0));
}
