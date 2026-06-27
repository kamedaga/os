const std = @import("std");

pub const default_bytes: usize = 16 * 1024 * 1024;

var empty_scratch: [0]u8 align(4096) = .{};
var scratch: []align(4096) u8 = empty_scratch[0..];
var scratch_used: usize = 0;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn staticStorageStart(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

fn minStaticStart(a: usize, b: usize) usize {
    return if (a < b) a else b;
}

pub fn kernelStaticStorageStartAddr() usize {
    var start = staticStorageStart(@TypeOf(scratch), &scratch);
    start = minStaticStart(start, staticStorageStart(@TypeOf(scratch_used), &scratch_used));
    return start;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end = staticStorageEnd(@TypeOf(scratch), &scratch);
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scratch_used), &scratch_used));
    return end;
}

pub fn install(buffer: []align(4096) u8) void {
    scratch = buffer;
    scratch_used = 0;
}

pub fn allocate(bytes: usize) ?[]align(8) u8 {
    const aligned_bytes = std.mem.alignForward(usize, bytes, 8);
    const start = std.mem.alignForward(usize, scratch_used, 8);
    if (aligned_bytes > scratch.len - start) return null;
    scratch_used = start + aligned_bytes;
    const ptr: [*]align(8) u8 = @ptrCast(@alignCast(scratch.ptr + start));
    return ptr[0..bytes];
}

pub fn free(buf: []align(8) u8) void {
    if (scratch.len == 0) return;
    const base = @intFromPtr(scratch.ptr);
    const limit = base + scratch.len;
    const ptr = @intFromPtr(buf.ptr);
    if (ptr < base or ptr > limit) return;
    const offset = ptr - base;
    const aligned_bytes = std.mem.alignForward(usize, buf.len, 8);
    if (offset + aligned_bytes == scratch_used) {
        scratch_used = offset;
    }
}
