const std = @import("std");
const kernel = @import("kernel.zig");

pub var global_free_list: *kernel.FreePageList = undefined;
pub var kernel_state_global: *kernel.KernelState = undefined;
pub var kernel_state_ready: bool = false;

fn staticStorageStart(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr);
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn minStaticStart(a: usize, b: usize) usize {
    return if (a < b) a else b;
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageStartAddr() usize {
    var start: usize = staticStorageStart(@TypeOf(global_free_list), &global_free_list);
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_state_global), &kernel_state_global));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_state_ready), &kernel_state_ready));
    return start;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = staticStorageEnd(@TypeOf(global_free_list), &global_free_list);
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_state_global), &kernel_state_global));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_state_ready), &kernel_state_ready));
    return end;
}

pub fn threadLabel(thread_index: usize) []const u8 {
    const labels = comptime blk: {
        var items: [kernel.initial_thread_capacity][]const u8 = undefined;
        for (0..kernel.initial_thread_capacity) |i| {
            items[i] = std.fmt.comptimePrint("Thread{}", .{i});
        }
        break :blk items;
    };
    if (thread_index < labels.len) return labels[thread_index];
    return "Thread?";
}

pub fn principalLabel(principal: kernel.PrincipalId) []const u8 {
    if (kernel_state_ready) {
        if (kernel_state_global.processDescriptor(principal)) |desc| {
            return desc.label;
        }
    }
    return kernel.principalLabel(principal);
}
