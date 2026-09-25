const std = @import("std");

pub const FramebufferInfo = struct {
    paddr: u64,
    size_bytes: usize,
    width: u32,
    height: u32,
    pixels_per_scan_line: u32,
    pixel_format: u32,
    mode: u32,
};

pub const FramebufferValidationError = error{ InvalidGeometry, SizeOverflow, PhysicalRange };

pub fn framebufferBytes(
    paddr: u64,
    width: u64,
    height: u64,
    pitch: u64,
    physical_limit: u64,
) FramebufferValidationError!usize {
    // The framebuffer is only described to init here. Its userland BAR lease
    // maps the actual size later, so the old fixed boot-console window and
    // 32-bit physical-address ceiling must not constrain a UEFI GOP mode.
    if (width == 0 or height == 0 or pitch == 0 or (pitch & 3) != 0 or
        width > std.math.maxInt(u32) or height > std.math.maxInt(u32) or
        pitch / 4 > std.math.maxInt(u32) or width > pitch / 4)
        return error.InvalidGeometry;
    const bytes = std.math.mul(u64, pitch, height) catch return error.SizeOverflow;
    if (bytes == 0 or bytes > std.math.maxInt(usize)) return error.SizeOverflow;
    if (paddr == 0 or paddr >= physical_limit or bytes > physical_limit - paddr)
        return error.PhysicalRange;
    return @intCast(bytes);
}

test "UEFI GOP framebuffers are not limited to 2 MiB or below 4 GiB" {
    const limit: u64 = @as(u64, 1) << 52;
    try std.testing.expectEqual(@as(usize, 1920 * 1080 * 4),
        try framebufferBytes(0x1_0000_0123, 1920, 1080, 1920 * 4, limit));
    try std.testing.expectEqual(@as(usize, 3840 * 2160 * 4),
        try framebufferBytes(0x1_0000_0123, 3840, 2160, 3840 * 4, limit));
    try std.testing.expectError(error.InvalidGeometry,
        framebufferBytes(0x1_0000_0123, 1921, 1080, 1920 * 4, limit));
    try std.testing.expectError(error.InvalidGeometry,
        framebufferBytes(0x1_0000_0123, 1920, 1080, 7681, limit));
    try std.testing.expectError(error.SizeOverflow,
        framebufferBytes(0x1000, 1, std.math.maxInt(u32),
            @as(u64, std.math.maxInt(u32)) * 4, limit));
    try std.testing.expectError(error.PhysicalRange,
        framebufferBytes(limit - 4096, 1920, 1080, 1920 * 4, limit));
}

pub const BootFile = struct {
    name: []const u8,
    bytes: []const u8,
};
