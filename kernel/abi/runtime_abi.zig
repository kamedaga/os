pub const syscall_runtime_first: u64 = 22;
pub const syscall_getpid: u64 = 22;
pub const syscall_gettid: u64 = 23;
pub const syscall_clock_gettime: u64 = 24;
pub const syscall_nanosleep: u64 = 25;
pub const syscall_futex_wait: u64 = 26;
pub const syscall_futex_wake: u64 = 27;
pub const syscall_getrandom: u64 = 28;
pub const syscall_runtime_last: u64 = syscall_getrandom;
pub const syscall_runtime_count: u64 = syscall_runtime_last - syscall_runtime_first + 1;

pub const clock_realtime: u64 = 0;
pub const clock_monotonic: u64 = 1;

pub const timespec_sec_offset: u64 = 0;
pub const timespec_nsec_offset: u64 = 8;
pub const timespec_size: u64 = 16;

pub const futex_wake_all: u64 = ~@as(u64, 0);

pub fn isRuntimeSyscall(nr: u64) bool {
    return nr >= syscall_runtime_first and nr <= syscall_runtime_last;
}

test "runtime syscall range is contiguous" {
    const std = @import("std");
    const expected = [_]u64{
        syscall_getpid,
        syscall_gettid,
        syscall_clock_gettime,
        syscall_nanosleep,
        syscall_futex_wait,
        syscall_futex_wake,
        syscall_getrandom,
    };
    try std.testing.expectEqual(@as(usize, syscall_runtime_count), expected.len);
    for (expected, 0..) |nr, offset| {
        try std.testing.expectEqual(syscall_runtime_first + @as(u64, @intCast(offset)), nr);
    }
}
