//! BSP-only migration before AP publication. Runtime allocations are owned by
//! the boot CPU directory for the kernel lifetime; the static BSP seed remains
//! mapped as part of the linked image; no AP is permitted to retain a seed
//! pointer. Migrated rows preserve boot counters and intrusive record heads.
const std = @import("std");
pub fn grow(comptime T: type, seed: []const T, count: usize, initial: T) ![]T {
    if (count < seed.len) return error.CpuStorageShrinks;
    const storage = if (@import("builtin").is_test)
        try std.testing.allocator.alloc(T, count)
    else
        try @import("arch/x86_64/cpu_runtime.zig").alloc(T, count);
    @memset(storage, initial);
    @memcpy(storage[0..seed.len], seed);
    return storage;
}

// C-layout descriptor is diagnostic metadata, not a guest ABI. Exporting a Zig
// slice is forbidden; spell out the pointer/count for exact-ELF host readers.
pub fn Descriptor(comptime T: type) type {
    return extern struct {
        pointer: [*]T,
        count: usize,
        pub fn init(slice: []T) @This() {
            return .{ .pointer = slice.ptr, .count = slice.len };
        }
    };
}
