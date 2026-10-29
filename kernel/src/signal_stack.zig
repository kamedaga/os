//! Native delivery layout. Userland owns capacity and lifetime; kernel owns
//! checking the destination before touching an interrupted task's memory.
const std = @import("std");
const abi = @import("kernel_abi_root").process_abi;

pub const Layout = struct { frame_va: u64, runtime_rsp: u64 };
fn userAddress(va: u64) bool {
    return va >= 4096 and va < 0x0000_8000_0000_0000;
}

pub fn registeredRangeValid(base: u64, size: u64) bool {
    if (base == 0 or size == 0) return base == 0 and size == 0;
    const end = std.math.add(u64, base, size) catch return false;
    return userAddress(base) and userAddress(end - 1) and
        size >= abi.signal_frame_size + abi.signal_runtime_stack_size + 63;
}

pub fn scratchRegistrationValid(base: u64, size: u64, entry: u64, inhibit_start: u64, inhibit_end: u64) bool {
    return registeredRangeValid(base, size) and
        (size == 0 or (inhibit_start < inhibit_end and entry >= inhibit_start and entry < inhibit_end));
}

pub fn layout(base: u64, size: u64, interrupted_rsp: u64) ?Layout {
    if (!registeredRangeValid(base, size)) return null;
    const top = if (size != 0) base + size else blk: {
        if (!userAddress(interrupted_rsp) or interrupted_rsp <= abi.signal_red_zone_size)
            return null;
        break :blk interrupted_rsp - abi.signal_red_zone_size;
    };
    if (top <= abi.signal_frame_size) return null;
    const frame = (top - abi.signal_frame_size) & ~@as(u64, 63);
    const bottom = if (size != 0) base else 4096;
    if (frame < bottom or frame - bottom < abi.signal_runtime_stack_size) return null;
    // C grows down from frame, using the reserved runtime space. Subtracting
    // that reservation from RSP would skip it and write outside the range.
    return .{ .frame_va = frame, .runtime_rsp = frame };
}

pub fn contains(base: u64, size: u64, rsp: u64) bool {
    return size != 0 and rsp >= base and rsp - base < size;
}

test "native signal stack validates boundaries and never falls back" {
    try std.testing.expect(registeredRangeValid(0, 0));
    try std.testing.expect(scratchRegistrationValid(0, 0, 0x4000, 0, 0));
    try std.testing.expect(!scratchRegistrationValid(0x10000, 8192, 0x4000, 0, 0));
    try std.testing.expect(!scratchRegistrationValid(0x10000, 8192, 0x4000, 0x5000, 0x6000));
    try std.testing.expect(scratchRegistrationValid(0x10000, 8192, 0x4000, 0x4000, 0x5000));
    try std.testing.expect(!registeredRangeValid(0, 8192));
    try std.testing.expect(!registeredRangeValid(8192, 0));
    try std.testing.expect(!registeredRangeValid(0xffff_ffff_ffff_f000, 8192));
    try std.testing.expect(!registeredRangeValid(0x7fff_ffff_f000, 8192));
    try std.testing.expect(!registeredRangeValid(0x10000, 5120));
    const l = layout(0x10000, 8192, 1).?;
    try std.testing.expectEqual(@as(u64, 0x11c00), l.frame_va);
    try std.testing.expectEqual(l.frame_va, l.runtime_rsp);
    try std.testing.expect(layout(0x10000, 5120, 0x800000) == null);
    const borrowed = layout(0, 0, 0x800000).?;
    try std.testing.expect(borrowed.frame_va + abi.signal_frame_size <= 0x800000 - abi.signal_red_zone_size);
    try std.testing.expect(layout(0, 0, 4096) == null);
    try std.testing.expect(contains(0x10000, 8192, l.runtime_rsp));
    try std.testing.expect(!contains(0x10000, 8192, 0x12000));
}
