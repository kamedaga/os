const std = @import("std");

pub const image_bytes: usize = 832;
pub const image_alignment: usize = 64;
pub const feature_mask: u64 = 0x7; // x87, SSE, AVX

const mxcsr_offset: usize = 24;
const xsave_header_offset: usize = 512;
const xstate_bv_offset: usize = xsave_header_offset;
const xcomp_bv_offset: usize = xsave_header_offset + 8;
const header_reserved_offset: usize = xsave_header_offset + 16;
const header_reserved_end: usize = xsave_header_offset + 64;

fn readLe(comptime T: type, bytes: []const u8) T {
    var value: T = 0;
    for (bytes[0..@sizeOf(T)], 0..) |byte, shift| {
        value |= @as(T, byte) << @intCast(shift * 8);
    }
    return value;
}

pub fn mxcsrMaskFromFxsave(image: *const [512]u8) u32 {
    return readLe(u32, image[28..32]);
}

/// Validate the standard (non-compacted) XSAVE image accepted from a signal
/// frame before the kernel can pass it to XRSTOR. The caller supplies the
/// current CPU's FXSAVE-reported MXCSR mask.
pub fn validateStandardUserImage(
    image: *const [image_bytes]u8,
    mxcsr_valid_mask: u32,
) bool {
    const xstate_bv = readLe(u64, image[xstate_bv_offset..][0..8]);
    if ((xstate_bv & ~feature_mask) != 0) return false;
    if (readLe(u64, image[xcomp_bv_offset..][0..8]) != 0) return false;
    for (image[header_reserved_offset..header_reserved_end]) |byte| {
        if (byte != 0) return false;
    }
    const mxcsr = readLe(u32, image[mxcsr_offset..][0..4]);
    if ((mxcsr & ~mxcsr_valid_mask) != 0) return false;
    return true;
}

fn writeLe(comptime T: type, bytes: []u8, value: T) void {
    for (bytes[0..@sizeOf(T)], 0..) |*byte, shift| {
        byte.* = @truncate(value >> @intCast(shift * 8));
    }
}

test "standard user xstate rejects XRSTOR-invalid metadata" {
    var image = [_]u8{0} ** image_bytes;
    writeLe(u32, image[mxcsr_offset..][0..4], 0x1f80);
    writeLe(u64, image[xstate_bv_offset..][0..8], feature_mask);
    try std.testing.expect(validateStandardUserImage(&image, 0x0000_ffbf));

    writeLe(u64, image[xstate_bv_offset..][0..8], feature_mask | (1 << 3));
    try std.testing.expect(!validateStandardUserImage(&image, 0x0000_ffbf));
    writeLe(u64, image[xstate_bv_offset..][0..8], feature_mask);

    writeLe(u64, image[xcomp_bv_offset..][0..8], 1 << 63);
    try std.testing.expect(!validateStandardUserImage(&image, 0x0000_ffbf));
    writeLe(u64, image[xcomp_bv_offset..][0..8], 0);

    image[header_reserved_offset] = 1;
    try std.testing.expect(!validateStandardUserImage(&image, 0x0000_ffbf));
    image[header_reserved_offset] = 0;

    writeLe(u32, image[mxcsr_offset..][0..4], 1 << 16);
    try std.testing.expect(!validateStandardUserImage(&image, 0x0000_ffbf));
}
