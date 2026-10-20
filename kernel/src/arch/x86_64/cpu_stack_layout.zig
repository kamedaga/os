const std = @import("std");

// BSP sets this once to the runtime allocator's reserved stack window.
pub var base: u64 = 0xffff_ff80_4000_0000;

// CPU lookup runs before XSAVE and after XRSTOR. Returning ?usize makes a
// two-word aggregate that LLVM can copy through XMM0, corrupting live user
// state. Keep the complete classification chain scalar, with no extra CPU
// width limit. The ELF gate checks the resulting call tree, not just this type.
pub const CpuIdentity = enum(usize) {
    bootstrap = 0,
    unknown = std.math.maxInt(usize),
    _,

    pub fn fromIndex(slot: usize) CpuIdentity {
        std.debug.assert(slot < @intFromEnum(CpuIdentity.unknown));
        return @enumFromInt(slot);
    }

    pub fn indexOr(self: CpuIdentity, fallback: usize) usize {
        return if (self == .unknown) fallback else @intFromEnum(self);
    }

    // Optional conversion is for callers which have already saved user
    // XState. Entry/return CPU lookup must use the scalar indexOr instead.
    pub fn index(self: CpuIdentity) ?usize {
        return if (self == .unknown) null else @intFromEnum(self);
    }
};

comptime {
    if (@sizeOf(CpuIdentity) != @sizeOf(usize))
        @compileError("CPU identity must remain a machine-word scalar");
}
// A runtime-reserved supervisor-only window outside the kernel image.
// The unchanged guarded stride may span as many PDP entries as topology needs.
pub const stride: usize = 4 * 1024 * 1024;
pub const kernel_bytes: usize = 512 * 1024;
pub const pf_ist_bytes: usize = 128 * 1024;
pub const df_ist_bytes: usize = 256 * 1024;
pub const backing_bytes: usize = kernel_bytes + pf_ist_bytes + df_ist_bytes;
pub const offsets = [_]usize{ 4096, kernel_bytes + 8192, kernel_bytes + pf_ist_bytes + 12288 };
pub const sizes = [_]usize{ kernel_bytes, pf_ist_bytes, df_ist_bytes };

pub fn stackBase(cpu: usize, stack: usize) ?u64 {
    if (cpu == 0 or cpu > (std.math.maxInt(u64) - base) / stride or stack >= sizes.len) return null;
    return base + (cpu - 1) * stride + offsets[stack];
}

/// The AP slot whose kernel or IST stack contains `rsp`, or .unknown. Each slot's
/// stacks lie inside its own `stride` window (the test below checks it), so
/// the window index names the only candidate: currentCpu() runs this on
/// every scheduler and syscall step, and a scan over all slots grew with the
/// CPU number. A slot without backing never had its stacks mapped.
pub fn apSlotForStackPointer(rsp: u64, backing: []const u64) CpuIdentity {
    if (rsp < base) return .unknown;
    const window = (rsp - base) / stride;
    if (window >= backing.len) return .unknown;
    const cpu: usize = @intCast(window + 1);
    if (backing[cpu - 1] == 0) return .unknown;
    for (sizes, 0..) |bytes, stack| {
        const offset = (rsp - base) % stride;
        if (offset >= offsets[stack] and offset <= offsets[stack] + bytes) return .fromIndex(cpu);
    }
    return .unknown;
}

test "CPU identity scalar preserves every supported slot and unknown" {
    for (0..513) |cpu| {
        const identity = CpuIdentity.fromIndex(cpu);
        try std.testing.expect(identity != .unknown);
        try std.testing.expectEqual(cpu, identity.indexOr(513));
        try std.testing.expectEqual(@as(?usize, cpu), identity.index());
    }
    try std.testing.expectEqual(@as(?usize, null), CpuIdentity.unknown.index());
    try std.testing.expectEqual(@as(usize, 0), CpuIdentity.unknown.indexOr(0));
    try std.testing.expectEqual(@as(usize, 42), CpuIdentity.unknown.indexOr(42));
}

test "an AP stack pointer names its slot and nothing else does" {
    var backing = [_]u64{1} ** (513 - 1);
    for (1..513) |cpu| {
        for (sizes, 0..) |bytes, stack| {
            const start = stackBase(cpu, stack).?;
            // Both ends are inclusive: an empty stack's rsp is its top.
            try std.testing.expectEqual(CpuIdentity.fromIndex(cpu), apSlotForStackPointer(start, &backing));
            try std.testing.expectEqual(CpuIdentity.fromIndex(cpu), apSlotForStackPointer(start + bytes, &backing));
            try std.testing.expectEqual(CpuIdentity.fromIndex(cpu), apSlotForStackPointer(start + bytes / 2, &backing));
            try std.testing.expectEqual(CpuIdentity.unknown, apSlotForStackPointer(start + bytes + 1, &backing));
            // The guard page below each stack belongs to no CPU.
            try std.testing.expectEqual(CpuIdentity.unknown, apSlotForStackPointer(start - 1, &backing));
        }
    }
    try std.testing.expectEqual(CpuIdentity.unknown, apSlotForStackPointer(base - 8, &backing));
    try std.testing.expectEqual(CpuIdentity.unknown, apSlotForStackPointer(base + (513 - 1) * stride + offsets[0], &backing));
    try std.testing.expectEqual(CpuIdentity.unknown, apSlotForStackPointer(std.math.maxInt(u64), &backing));
    // An unbacked slot's window is never a CPU, even at its stack addresses.
    backing[2] = 0;
    try std.testing.expectEqual(CpuIdentity.unknown, apSlotForStackPointer(stackBase(3, 0).? + 64, &backing));
    try std.testing.expectEqual(CpuIdentity.fromIndex(4), apSlotForStackPointer(stackBase(4, 0).? + 64, &backing));
}

test "AP stack slots cross PDP boundaries without overlap" {
    try std.testing.expectEqual(@as(?u64, null), stackBase(0, 0));
    try std.testing.expect(stackBase(512, 0) != null);
    try std.testing.expectEqual(@as(?u64, null), stackBase(1, 3));
    var previous_end = base;
    for (1..513) |cpu| {
        for (sizes, 0..) |bytes, stack| {
            const start = stackBase(cpu, stack).?;
            try std.testing.expect(start >= previous_end + 4096);
            try std.testing.expectEqual(@as(u64, 0), start & 4095);

            previous_end = start + bytes;
            try std.testing.expect(previous_end + 4096 <= base + cpu * stride);
        }
    }
}

test "AP backing contains only kernel PF and unchanged DF usable pages" {
    try std.testing.expectEqual(@as(usize, 896 * 1024), backing_bytes);
    try std.testing.expectEqual(@as(usize, 512 * 1024), sizes[0]);
    try std.testing.expectEqual(@as(usize, 128 * 1024), sizes[1]);
    try std.testing.expectEqual(@as(usize, 256 * 1024), sizes[2]);
    try std.testing.expectEqual(backing_bytes, sizes[0] + sizes[1] + sizes[2]);
    try std.testing.expectEqual(offsets[0] + sizes[0] + 4096, offsets[1]);
    try std.testing.expectEqual(offsets[1] + sizes[1] + 4096, offsets[2]);
}
