const std = @import("std");
pub const stride = 512;
// Save RAX with moffs64 before borrowing it as the frame pointer. All other
// user GPRs then go directly to that CPU's frame; no user stack or GS value
// is trusted, and no SIMD instruction can run before XSAVE.
pub fn generate(out: []u8, frame: u64, slot: usize, target: u64) void {
    @memset(out, 0xcc);
    var n: usize = 0;
    out[0] = 0x48;
    out[1] = 0xa3;
    std.mem.writeInt(u64, out[2..10], frame + 112, .little);
    out[10] = 0x48;
    out[11] = 0xb8;
    std.mem.writeInt(u64, out[12..20], frame, .little);
    n = 20;
    const registers = [_]u8{ 15, 14, 13, 12, 10, 9, 8, 5, 7, 6, 2, 1, 3, 11, 4 };
    const offsets = [_]u8{ 0, 8, 16, 24, 40, 48, 56, 64, 72, 80, 88, 120, 104, 136, 144 };
    for (registers, offsets) |reg, offset| {
        out[n] = if (reg >= 8) 0x4c else 0x48;
        out[n + 1] = 0x89;
        out[n + 2] = 0x80 | ((reg & 7) << 3);
        std.mem.writeInt(u32, out[n + 3 ..][0..4], offset, .little);
        n += 7;
    }
    for ([_]u8{ 32, 96, 128, 152 }, [_]u32{ 0, 0, 0x23, 0x1b }) |offset, value| {
        out[n] = 0x48;
        out[n + 1] = 0xc7;
        out[n + 2] = 0x80;
        std.mem.writeInt(u32, out[n + 3 ..][0..4], offset, .little);
        std.mem.writeInt(u32, out[n + 7 ..][0..4], value, .little);
        n += 11;
    }
    out[n] = 0x49;
    out[n + 1] = 0xbc;
    std.mem.writeInt(u64, out[n + 2 ..][0..8], slot, .little);
    n += 10;
    out[n] = 0xff;
    out[n + 1] = 0x25;
    @memset(out[n + 2 ..][0..4], 0);
    std.mem.writeInt(u64, out[n + 6 ..][0..8], target, .little);
}

// Decode the emitted subset, tracking all architectural GPR stores. This
// checks the entry's contract rather than comparing a second byte template.
test "generated syscall entry preserves every live GPR before borrowing scratch" {
    var code: [stride]u8 = undefined;
    const frame: u64 = 0xffff_ff80_9876_5000;
    const target: u64 = 0xffff_ffff_8123_4000;
    generate(&code, frame, 70000, target);
    try std.testing.expectEqual(@as(u8, 0xa3), code[1]);
    try std.testing.expectEqual(frame + 112, std.mem.readInt(u64, code[2..10], .little));
    try std.testing.expectEqual(@as(u8, 0xb8), code[11]);
    try std.testing.expectEqual(frame, std.mem.readInt(u64, code[12..20], .little));
    var seen: [16]bool = @splat(false);
    seen[0] = true;
    var destinations: [20]?u8 = @splat(null);
    destinations[14] = 0;
    var offset: usize = 20;
    while (code[offset + 1] == 0x89) : (offset += 7) {
        try std.testing.expect(code[offset] == 0x48 or code[offset] == 0x4c);
        try std.testing.expectEqual(@as(u8, 0x80), code[offset + 2] & 0xc7);
        const reg = ((code[offset + 2] >> 3) & 7) + (if (code[offset] == 0x4c) @as(u8, 8) else 0);
        try std.testing.expect(!seen[reg]);
        seen[reg] = true;
        const dest = std.mem.readInt(u32, code[offset + 3 ..][0..4], .little);
        try std.testing.expect(dest < 160 and dest % 8 == 0);
        try std.testing.expect(destinations[dest / 8] == null);
        destinations[dest / 8] = reg;
    }
    for (seen) |saved| try std.testing.expect(saved);
    try std.testing.expectEqual(@as(?u8, 4), destinations[18]); // original RSP
    try std.testing.expectEqual(@as(?u8, 1), destinations[15]); // SYSCALL RCX -> RIP
    try std.testing.expectEqual(@as(?u8, 11), destinations[17]); // R11 -> flags
    while (code[offset + 1] == 0xc7) : (offset += 11) {
        const dest = std.mem.readInt(u32, code[offset + 3 ..][0..4], .little);
        try std.testing.expect(dest < 160 and destinations[dest / 8] == null);
        const value = std.mem.readInt(u32, code[offset + 7 ..][0..4], .little);
        try std.testing.expectEqual(@as(u32, if (dest == 128) 0x23 else if (dest == 152) 0x1b else 0), value);
    }
    try std.testing.expectEqual(@as(u8, 0xbc), code[offset + 1]);
    try std.testing.expectEqual(@as(u64, 70000), std.mem.readInt(u64, code[offset + 2 ..][0..8], .little));
    offset += 10;
    try std.testing.expectEqualSlices(u8, &.{ 0xff, 0x25, 0, 0, 0, 0 }, code[offset..][0..6]);
    try std.testing.expectEqual(target, std.mem.readInt(u64, code[offset + 6 ..][0..8], .little));
    try std.testing.expect(offset + 14 <= stride);
}

/// Opcode routing precedes all frame stores. The direct entry saves only ABI
/// inputs and volatile state; RBX holds the deferred FD destination and must be saved before preflight.
/// Win64 preflight preserves RBP/R13/R14 until
/// assembly saves them after admission (or before falling back).
pub fn generateDirect(out: []u8, frame: u64, slot: usize, slow: u64, direct: u64) void {
    @memset(out, 0xcc);
    const slow_offset = 64;
    const direct_offset = 320;
    generate(out[slow_offset..direct_offset], frame, slot, slow);
    var n: usize = 0;
    for ([_]u8{ 74, 76, 79, 80 }) |nr| {
        @memcpy(out[n..][0..4], &[_]u8{ 0x48, 0x83, 0xf8, nr });
        out[n + 4] = 0x0f;
        out[n + 5] = 0x84;
        std.mem.writeInt(u32, out[n + 6 ..][0..4], @intCast(direct_offset - (n + 10)), .little);
        n += 10;
    }
    out[n] = 0xe9;
    std.mem.writeInt(u32, out[n + 1 ..][0..4], @intCast(slow_offset - (n + 5)), .little);
    n = direct_offset;
    out[n] = 0x48;
    out[n + 1] = 0xa3;
    std.mem.writeInt(u64, out[n + 2 ..][0..8], frame + 112, .little);
    out[n + 10] = 0x48;
    out[n + 11] = 0xb8;
    std.mem.writeInt(u64, out[n + 12 ..][0..8], frame, .little);
    n += 20;
    for ([_]u8{ 15, 12, 10, 9, 8, 7, 6, 2, 1, 11, 4, 3 }, [_]u8{ 0, 24, 40, 48, 56, 72, 80, 88, 120, 136, 144, 104 }) |reg, offset| {
        out[n] = if (reg >= 8) 0x4c else 0x48;
        out[n + 1] = 0x89;
        out[n + 2] = 0x80 | ((reg & 7) << 3);
        std.mem.writeInt(u32, out[n + 3 ..][0..4], offset, .little);
        n += 7;
    }
    for ([_]u8{ 32, 96, 128, 152 }, [_]u32{ 0, 0, 0x23, 0x1b }) |offset, value| {
        @memcpy(out[n..][0..3], &[_]u8{ 0x48, 0xc7, 0x80 });
        std.mem.writeInt(u32, out[n + 3 ..][0..4], offset, .little);
        std.mem.writeInt(u32, out[n + 7 ..][0..4], value, .little);
        n += 11;
    }
    out[n] = 0x49;
    out[n + 1] = 0xbb; // r11 is scratch only after original flags are saved
    std.mem.writeInt(u64, out[n + 2 ..][0..8], slot, .little);
    n += 10;
    @memcpy(out[n..][0..6], &[_]u8{ 0xff, 0x25, 0, 0, 0, 0 });
    std.mem.writeInt(u64, out[n + 6 ..][0..8], direct, .little);
    std.debug.assert(n + 14 <= stride);
}

test "direct syscall entry routes CALL REPLY_RECV NOTIFY and NOTIFY_WAIT before partial save" {
    var code: [stride]u8 = undefined;
    generateDirect(&code, 0x12340000, 3, 0x45670000, 0x56780000);
    for (0..256) |nr| {
        var pc: usize = 0;
        var equal = false;
        while (pc < 64) {
            if (code[pc] == 0x48) {
                try std.testing.expectEqualSlices(u8, &.{ 0x48, 0x83, 0xf8 }, code[pc..][0..3]);
                equal = nr == code[pc + 3];
                pc += 4;
            } else if (code[pc] == 0x0f) {
                const delta = std.mem.readInt(u32, code[pc + 2 ..][0..4], .little);
                pc += 6;
                if (equal) pc += delta;
            } else {
                try std.testing.expectEqual(@as(u8, 0xe9), code[pc]);
                const delta = std.mem.readInt(u32, code[pc + 1 ..][0..4], .little);
                pc += 5 + delta;
            }
        }
        try std.testing.expectEqual(@as(usize, if (nr == 74 or nr == 76 or nr == 79 or nr == 80) 320 else 64), pc);
    }
    var offset: usize = 340;
    var seen: [16]bool = @splat(false);
    seen[0] = true;
    while (code[offset + 1] == 0x89) : (offset += 7) {
        const reg = ((code[offset + 2] >> 3) & 7) + (if (code[offset] == 0x4c) @as(u8, 8) else 0);
        try std.testing.expect(!seen[reg]);
        seen[reg] = true;
    }
    for (seen, 0..) |saved, reg| try std.testing.expectEqual(reg != 5 and reg != 13 and reg != 14, saved);
}
