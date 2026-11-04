/// Internal waits finish before signal delivery; teardown is never deferred.
pub const wait_uninterruptible: u64 = 1 << 1;

pub const syscall_runtime_first: u64 = 26;
pub const syscall_getpid: u64 = 26;
pub const syscall_gettid: u64 = 27;
pub const syscall_system_info: u64 = 28;
// CLOCK_READ: RDI=clock id, RAX=status, RDX=nanoseconds (zero on error).
// Value-only reads need neither a user pointer nor a timespec copyout.
pub const syscall_clock_read: u64 = 29;
pub const syscall_clock_gettime: u64 = 30;
pub const syscall_clock_getres: u64 = 31;
// CLOCK_PAGE_OPEN: no arguments. Returns a new FD for the read-only monotonic
// clock page (rights INSPECT|CLOSE|MAP_READ|DUP|TRANSFER), or a negative native
// status. The page lets a reader skip CLOCK_READ while the kernel's KVM clock
// is stable; every failed fast read must fall back to CLOCK_READ.
pub const syscall_clock_page_open: u64 = 32;
pub const syscall_yield: u64 = 33;
pub const syscall_nanosleep: u64 = 34;
// FUTEX_WAIT: RDI=key, RSI=expected, RDX=ticks, R10=wait flags.
pub const syscall_futex_wait: u64 = 35;
pub const syscall_futex_wake: u64 = 36;
// FUTEX_WAKE_WAIT: RDI=wake key, RSI=wait key, R10=expected u32, R8=ticks, R9=wait flags.
// RAX is wait status; RDX is committed wake count (0/1), or UINT64_MAX
// when wake was unperformed/failed. A completed wake must not be retried.
pub const syscall_futex_wake_wait: u64 = 37;
pub const syscall_futex_requeue: u64 = 38;
// FUTEX_WAKE_OP: RDI/RSI=keys, RDX/R10=wake limits, R8=FutexWakeOp pointer.
// Atomically update the second word and select both wakes under the futex
// queue lock. Return a nonnegative wake count, or a negative native status;
// failure commits neither the update nor a wake. Keys remain principal+VA.
// The updated word requires a writable ordinary-RAM VMA; device and legacy
// bootstrap-only mappings without that retained backing authority fail MAP.
pub const syscall_futex_wake_op: u64 = 39;
// Owner-aware mutex slowpath: RDI=word, RSI=operation, RDX=absolute CLOCK_MONOTONIC milliseconds
// (zero=infinite), R10=key flags. Caller identity is derived by the kernel.
// All admitted threads currently have the same scheduling priority. A future
// unequal-priority policy must extend ownership inheritance before admission.
pub const syscall_futex_pi: u64 = 40;
pub const syscall_getrandom: u64 = 41;
pub const syscall_power_control: u64 = 42;
// GETRANDOM: flags must be zero, length <= 4096. Returns bytes written or
// negative native status (INVALID=1, MAP=4); positive errors are not counts.
pub const syscall_runtime_last: u64 = syscall_power_control;
pub const syscall_runtime_count: u64 = syscall_runtime_last - syscall_runtime_first + 1;

pub const clock_realtime: u64 = 0;
pub const clock_monotonic: u64 = 1;
pub const clock_realtime_resolution_nsec: u64 = 1_000_000_000;
pub const clock_monotonic_resolution_nsec: u64 = 1_000_000;

// Read-only clock page, one 4096-byte page (all fields little-endian).
// fast is 1 while fast reads are allowed and becomes 0 at most once, never 1
// again. A reader computes ns = ns_origin + (pvclock(pv0) - pv_origin) from a
// stable, even-version pv0 snapshot, rejects ns > ceiling_ns, returns
// max(ns, floor_ns), and accepts the result only if fast is still 1 afterwards.
// floor_ns is the kernel's published monotonic floor; ceiling_ns bounds every
// accepted fast value and is where a later fallback clock starts at the latest.
pub const clock_page_size: u64 = 4096;
pub const clock_page_fast_offset: u64 = 0;
pub const clock_page_pv_origin_offset: u64 = 8;
pub const clock_page_ns_origin_offset: u64 = 16;
pub const clock_page_floor_ns_offset: u64 = 24;
pub const clock_page_ceiling_ns_offset: u64 = 32;
// Kernel-published pvclock-format conversion record, 32 bytes: supplied by
// KVM (vCPU 0) or synthesized by the kernel. Stable bit 0 promises that this
// record and any online CPU's TSC yield the kernel's monotonic domain after
// epoch conversion. Synthetic records have bits 1..7 zero and are immutable.
// Layout, flag bits and syscall numbers are unchanged; no ABI version added.
pub const clock_page_pv0_offset: u64 = 64;
pub const clock_page_pv_version_offset: u64 = 0;
pub const clock_page_pv_tsc_timestamp_offset: u64 = 8;
pub const clock_page_pv_system_time_offset: u64 = 16;
pub const clock_page_pv_multiplier_offset: u64 = 24;
pub const clock_page_pv_shift_offset: u64 = 28;
pub const clock_page_pv_flags_offset: u64 = 29;
pub const clock_page_pv_stable_flag: u64 = 1;

pub const system_info_total_usable_memory_bytes_offset: u64 = 0;
pub const system_info_free_memory_bytes_offset: u64 = 8;
pub const system_info_online_cpu_count_offset: u64 = 16;
pub const system_info_reserved_offset: u64 = 24;
pub const system_info_size: u64 = 32;

pub const timespec_sec_offset: u64 = 0;
pub const timespec_nsec_offset: u64 = 8;
pub const timespec_size: u64 = 16;

pub const futex_wake_all: u64 = ~@as(u64, 0);
pub const FutexAtomicOperation = enum(u32) { set, add, bit_or, and_not, bit_xor };
pub const FutexComparison = enum(u32) { equal, not_equal, signed_less, signed_less_equal, signed_greater, signed_greater_equal };
// Raw tags are validated before use; untrusted enum values must never become
// an invalid Zig enum. The descriptor is copied before either key is touched.
pub const FutexWakeOp = extern struct {
    operation: u32,
    operand: u32,
    comparison: u32,
    compare_operand: u32,
};
// FUTEX_WAIT uses the existing EMPTY status to distinguish timer expiry from
// NOT_READY (value mismatch or signal interruption) without adding a status
// number to the native ABI.
pub const futex_wait_timed_out_status: u64 = 5;

pub fn isRuntimeSyscall(nr: u64) bool {
    return nr >= syscall_runtime_first and nr <= syscall_runtime_last;
}

test "runtime syscall range is contiguous" {
    const std = @import("std");
    const expected = [_]u64{
        syscall_getpid,
        syscall_gettid,
        syscall_system_info,
        syscall_clock_read,
        syscall_clock_gettime,
        syscall_clock_getres,
        syscall_clock_page_open,
        syscall_yield,
        syscall_nanosleep,
        syscall_futex_wait,
        syscall_futex_wake,
        syscall_futex_wake_wait,
        syscall_futex_requeue,
        syscall_futex_wake_op,
        syscall_futex_pi,
        syscall_getrandom,
        syscall_power_control,
    };
    try std.testing.expectEqual(@as(usize, syscall_runtime_count), expected.len);
    for (expected, 0..) |nr, offset| {
        try std.testing.expectEqual(syscall_runtime_first + @as(u64, @intCast(offset)), nr);
    }
}

// The native owner word permits atomic userspace acquire/release fastpaths.
// Recovery-marked words and truly shared keys are not supported yet.
pub const futex_pi_owner_mask: u32 = 0x3fffffff;
pub const futex_pi_recovery: u32 = 0x40000000;
pub const futex_pi_waiters: u32 = 0x80000000;
pub const futex_pi_private_key: u64 = 1;
pub const FutexPiOperation = enum(u64) { lock, try_lock, unlock };
pub const FutexPiResult = enum(u64) {
    acquired = 0,
    invalid = 1,
    interrupted_or_busy = 2,
    allocation = 3,
    bad_memory = 4,
    timed_out = 5,
    not_owner = 7,
    deadlock = 8,
    owner_missing = 9,
    unsupported = 10,
};
