const std = @import("std");

pub fn nextGeometricCapacity(
    current_capacity: usize,
    initial_capacity: usize,
    max_capacity: usize,
    required: usize,
) ?usize {
    if (required > max_capacity or initial_capacity == 0 or initial_capacity > max_capacity) return null;
    var capacity = current_capacity;
    if (capacity == 0) capacity = initial_capacity;
    while (capacity < required) {
        const doubled, const overflow = @mulWithOverflow(capacity, @as(usize, 2));
        capacity = if (overflow != 0 or doubled > max_capacity) max_capacity else doubled;
        if (capacity < required and capacity == max_capacity) return null;
    }
    return capacity;
}

test "geometric capacity doubles and clamps at the shared maximum" {
    try std.testing.expectEqual(@as(?usize, 32), nextGeometricCapacity(32, 32, 65536, 32));
    try std.testing.expectEqual(@as(?usize, 64), nextGeometricCapacity(32, 32, 65536, 33));
    try std.testing.expectEqual(@as(?usize, 128), nextGeometricCapacity(64, 32, 65536, 65));
    try std.testing.expectEqual(@as(?usize, 65536), nextGeometricCapacity(32768, 32, 65536, 65536));
    try std.testing.expectEqual(@as(?usize, null), nextGeometricCapacity(65536, 32, 65536, 65537));
}

pub fn StaticPlusExtra(comptime T: type, comptime inline_capacity: usize) type {
    return struct {
        pub const Item = T;
        pub const inline_len = inline_capacity;

        pub fn extraIndex(index: usize) ?usize {
            if (index < inline_capacity) return null;
            return index - inline_capacity;
        }

        pub fn extraCount(capacity: usize) usize {
            if (capacity <= inline_capacity) return 0;
            return capacity - inline_capacity;
        }

        pub fn copyExistingExtra(new_extra: []T, old_extra: []const T, old_capacity: usize) void {
            const old_count = extraCount(old_capacity);
            if (old_count == 0) return;
            @memcpy(new_extra[0..old_count], old_extra[0..old_count]);
        }
    };
}

pub fn extraIndex(comptime inline_capacity: usize, index: usize) ?usize {
    if (index < inline_capacity) return null;
    return index - inline_capacity;
}

pub fn runtimeStorageSlice(
    comptime T: type,
    storage: []align(4096) u8,
    cursor: *usize,
    count: usize,
) ?[]T {
    const start = std.mem.alignForward(usize, cursor.*, @alignOf(T));
    const bytes = @sizeOf(T) * count;
    if (start > storage.len or bytes > storage.len - start) return null;
    cursor.* = start + bytes;
    const ptr: [*]T = @ptrCast(@alignCast(storage.ptr + start));
    return ptr[0..count];
}
