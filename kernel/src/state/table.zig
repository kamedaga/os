const std = @import("std");

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
