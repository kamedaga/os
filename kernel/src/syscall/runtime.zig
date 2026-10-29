const slot_storage = @import("../memory/slot_table.zig");
const lock_diagnostics = @import("../sync/lock_diagnostics.zig");
const std = @import("std");
const acpi_power = @import("../acpi_power.zig");
const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const realtime_clock = @import("../realtime_clock.zig");
const scheduler = @import("../scheduler.zig").connection;
const smp = @import("../smp.zig");
const sc = @import("numbers.zig");
const perf = @import("../smp_perf.zig");
const lock_profile = @import("../lock_profile.zig");
const wake_trace = @import("../wake_trace.zig");
const user_vm = @import("../memory/user_vm.zig");

const runtime_abi = abi_root.runtime_abi;
const TrapFrame = interrupts.TrapFrame;

// Shared by every process, including native GPU workers and Linux threads.
// A multi-process browser can exceed 256 live waits during tab restoration.
// Keep the table bounded; stale-token reclamation still runs before failure.

const FutexSpinLock = struct {
    value: u8 = 0,
    diagnostic_record: lock_diagnostics.Record = .{},

    fn interruptsEnabled() bool {
        if (comptime @import("builtin").is_test) return false;
        const flags = asm volatile ("pushfq; pop %[flags]"
            : [flags] "=r" (-> u64),
            :
            : .{ .memory = true });
        return (flags & (1 << 9)) != 0;
    }

    fn waitWithInterruptWindow(restore_interrupts: bool) void {
        if (comptime @import("builtin").is_test) {
            if (futex_test_interrupt_ack) |ack| ack.store(true, .release);
            std.atomic.spinLoopHint();
            return;
        }
        // Syscalls enter with IF clear. A CPU waiting for F must still
        // acknowledge a shootdown from the A owner; that owner may be the
        // reason another F contender cannot finish its A lookup.
        asm volatile ("sti; pause; cli" ::: .{ .memory = true });
        if (restore_interrupts) asm volatile ("sti" ::: .{ .memory = true });
    }

    fn lock(self: *FutexSpinLock) void {
        const restore_interrupts = interruptsEnabled();
        while (true) {
            const diagnostic_if = lock_diagnostics.maskInterrupts();
            lock_diagnostics.beforeAcquire(&self.diagnostic_record, .{ .class = .futex });
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) {
                lock_diagnostics.acquired(&self.diagnostic_record, .{ .class = .futex });
                lock_diagnostics.restoreInterrupts(diagnostic_if);
                return;
            }
            lock_diagnostics.restoreInterrupts(diagnostic_if);
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                // The window is only in the unowned wait; each CAS and lock
                // diagnostic still runs with its original IF discipline.
                waitWithInterruptWindow(restore_interrupts);
            }
        }
    }

    fn unlock(self: *FutexSpinLock) void {
        const diagnostic_if = lock_diagnostics.maskInterrupts();
        defer lock_diagnostics.restoreInterrupts(diagnostic_if);
        lock_diagnostics.releasing(&self.diagnostic_record);
        @atomicStore(u8, &self.value, 0, .release);
    }
};

var futex_test_interrupt_ack: if (@import("builtin").is_test) ?*std.atomic.Value(bool) else void =
    if (@import("builtin").is_test) null else {};

const FutexWaiter = struct {
    active: bool = false,
    principal: kernel.PrincipalId = @enumFromInt(0),
    thread_index: usize = 0,
    thread_generation: u32 = 0,
    user_va: u64 = 0,
    wait_token: u64 = 0,
    key_prev: ?*FutexWaiter = null,
    key_next: ?*FutexWaiter = null,
};

var futex_waiters: slot_storage.SlotTable(FutexWaiter) = .{};
var futex_directory_leaf: slot_storage.Leaf = .{};
var futex_waiters_lock: FutexSpinLock = .{};
const FutexThreadCacheEntry = struct {
    principal: kernel.PrincipalId = @enumFromInt(0),
    thread_index: usize = std.math.maxInt(usize),
    thread_generation: u32 = 0,
    waiter: ?*FutexWaiter = null,
    retired: bool = false,
};
// Advisory, direct-mapped identity hints. F owns both these pointers and the
// waiter table; an eviction only restores the thread-identity directory search.
var futex_thread_cache: [64]FutexThreadCacheEntry = [_]FutexThreadCacheEntry{.{}} ** 64;
const FutexKeyHint = struct {
    principal: kernel.PrincipalId = @enumFromInt(0),
    user_va: u64 = 0,
    count: usize = 0,
    unique: ?*FutexWaiter = null,
    conflicted: bool = false,
};
// F owns these key hints. A collision or multiple waiters restores the indexed
// bucket scan; an empty hint bucket proves no registered waiter has this key.
var futex_key_hints: [256]FutexKeyHint = [_]FutexKeyHint{.{}} ** 256;
// F owns every link. The hash narrows a lookup to one bucket, and insertion
// orders its records by SlotTable index so wake still selects the same waiter
// as the former ascending directory walk, even after a slot is reused.
var futex_key_buckets: [1024]?*FutexWaiter = [_]?*FutexWaiter{null} ** 1024;
var futex_test_key_visits: if (@import("builtin").is_test) usize else void = if (@import("builtin").is_test) 0 else {};

fn futexKeyHash(proc: kernel.PrincipalId, user_va: u64) u64 {
    const hash_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_key_hash, hash_stage);
    const mixed = (@as(u64, @intFromEnum(proc)) *% 0x9e3779b97f4a7c15) ^
        (user_va >> 2) ^ (user_va >> 17);
    return mixed ^ (mixed >> 33);
}

fn futexKeyHint(proc: kernel.PrincipalId, user_va: u64) *FutexKeyHint {
    return &futex_key_hints[@as(usize, @truncate(futexKeyHash(proc, user_va))) & (futex_key_hints.len - 1)];
}

fn futexKeyBucket(proc: kernel.PrincipalId, user_va: u64) *?*FutexWaiter {
    return &futex_key_buckets[@as(usize, @truncate(futexKeyHash(proc, user_va))) & (futex_key_buckets.len - 1)];
}

const FutexKeyIterator = struct {
    cursor: ?*FutexWaiter,
    proc: kernel.PrincipalId,
    user_va: u64,

    fn next(self: *FutexKeyIterator) ?*FutexWaiter {
        while (self.cursor) |waiter| {
            // Advance before the caller may retire or requeue this record.
            self.cursor = waiter.key_next;
            if (comptime @import("builtin").is_test) futex_test_key_visits += 1;
            if (waiter.active and waiter.principal == self.proc and waiter.user_va == self.user_va) return waiter;
        }
        return null;
    }
};

fn futexKeyIterator(proc: kernel.PrincipalId, user_va: u64) FutexKeyIterator {
    return .{ .cursor = futexKeyBucket(proc, user_va).*, .proc = proc, .user_va = user_va };
}

fn addFutexKeyHint(waiter: *FutexWaiter) void {
    std.debug.assert(waiter.active and waiter.key_prev == null and waiter.key_next == null);
    const bucket = futexKeyBucket(waiter.principal, waiter.user_va);
    const index = @TypeOf(futex_waiters).indexOf(waiter);
    var link = bucket;
    var previous: ?*FutexWaiter = null;
    while (link.*) |current| {
        std.debug.assert(current != waiter);
        if (@TypeOf(futex_waiters).indexOf(current) > index) break;
        previous = current;
        link = &current.key_next;
    }
    waiter.key_prev = previous;
    waiter.key_next = link.*;
    if (link.*) |next| next.key_prev = waiter;
    link.* = waiter;
    const hint = futexKeyHint(waiter.principal, waiter.user_va);
    if (hint.count == 0) {
        hint.* = .{ .principal = waiter.principal, .user_va = waiter.user_va,
            .count = 1, .unique = waiter };
        return;
    }
    hint.count += 1;
    if (hint.principal != waiter.principal or hint.user_va != waiter.user_va)
        hint.conflicted = true;
    hint.unique = null;
}

fn removeFutexKeyHint(waiter: *FutexWaiter) void {
    if (!waiter.active) return;
    // Unlink before SlotTable can retire or reuse the body. Keeping an old
    // pointer here could wake a different thread through a recycled slot.
    if (waiter.key_prev) |previous| {
        std.debug.assert(previous.key_next == waiter);
        previous.key_next = waiter.key_next;
    } else {
        const bucket = futexKeyBucket(waiter.principal, waiter.user_va);
        std.debug.assert(bucket.* == waiter);
        bucket.* = waiter.key_next;
    }
    if (waiter.key_next) |next| {
        std.debug.assert(next.key_prev == waiter);
        next.key_prev = waiter.key_prev;
    }
    waiter.key_prev = null;
    waiter.key_next = null;
    const hint = futexKeyHint(waiter.principal, waiter.user_va);
    // Every active waiter, including test fixtures, is published under F.
    // An absent count would let the empty-key fast path lose a wake.
    std.debug.assert(hint.count != 0);
    hint.count -= 1;
    if (hint.count == 0) hint.* = .{} else hint.unique = null;
}

fn futexKeyKnownEmpty(proc: kernel.PrincipalId, user_va: u64) bool {
    const hint = futexKeyHint(proc, user_va);
    return hint.count == 0 or
        (hint.count == 1 and !hint.conflicted and
        (hint.principal != proc or hint.user_va != user_va));
}

fn uniqueFutexKeyWaiter(proc: kernel.PrincipalId, user_va: u64) ?*FutexWaiter {
    const hint = futexKeyHint(proc, user_va);
    if (hint.count != 1 or hint.conflicted or hint.principal != proc or hint.user_va != user_va) return null;
    const waiter = hint.unique orelse return null;
    if (!waiter.active or waiter.principal != proc or waiter.user_va != user_va) return null;
    return waiter;
}
// The table stays fully available, but short waits must not scan its unused
// tail under global exclusion. This is the active-record extent, not a count
// of live scheduler tokens: timeout/signal may leave stale records behind.
var futex_test_fail_growth = false;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(futex_waiters), &futex_waiters));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(futex_waiters_lock), &futex_waiters_lock));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(futex_directory_leaf), &futex_directory_leaf));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(futex_thread_cache), &futex_thread_cache));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(futex_key_hints), &futex_key_hints));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(futex_key_buckets), &futex_key_buckets));
    end = maxStaticEnd(end, realtime_clock.kernelStaticStorageEndAddr());
    return end;
}

fn readUserU32(h: anytype, proc: kernel.PrincipalId, user_va: u64) ?u32 {
    var bytes: [4]u8 = undefined;
    if (!h.copy_user_bytes_from_va(proc, user_va, bytes[0..])) return null;
    return std.mem.readInt(u32, &bytes, .little);
}

fn lockFutexUserAddressSpace(h: anytype, proc: kernel.PrincipalId) bool {
    const HookType = if (@typeInfo(@TypeOf(h)) == .pointer) @typeInfo(@TypeOf(h)).pointer.child else @TypeOf(h);
    if (comptime @hasField(HookType, "futex_lock_address_space")) {
        if (h.futex_lock_address_space) |lock| return lock(proc);
    }
    if (comptime !@import("builtin").is_test) @panic("futex AS lock hook missing");
    return true;
}

fn unlockFutexUserAddressSpace(h: anytype, proc: kernel.PrincipalId) void {
    const HookType = if (@typeInfo(@TypeOf(h)) == .pointer) @typeInfo(@TypeOf(h)).pointer.child else @TypeOf(h);
    if (comptime @hasField(HookType, "futex_unlock_address_space")) {
        if (h.futex_unlock_address_space) |unlock| {
            unlock(proc);
            return;
        }
    }
    if (comptime !@import("builtin").is_test) @panic("futex AS unlock hook missing");
}

fn readUserU32UnderFutexAndAddressSpaceLocks(h: anytype, proc: kernel.PrincipalId, user_va: u64) ?u32 {
    const HookType = if (@typeInfo(@TypeOf(h)) == .pointer) @typeInfo(@TypeOf(h)).pointer.child else @TypeOf(h);
    if (comptime @hasField(HookType, "resident_read_user_u32_with_address_space_locked")) {
        if (h.resident_read_user_u32_with_address_space_locked) |resident| return resident(proc, user_va);
    }
    if (comptime @hasField(HookType, "resident_read_user_u32")) {
        if (comptime @import("builtin").is_test) {
            if (h.resident_read_user_u32) |resident| return resident(proc, user_va);
        }
    }
    // Small host fixtures supply only a checked-copy mock. Production hooks
    // install the locked resident reader. F -> AS never faults or allocates.
    if (comptime !@import("builtin").is_test) unreachable;
    return readUserU32(h, proc, user_va);
}

fn writeTimespec(h: anytype, proc: kernel.PrincipalId, out_va: u64, sec: u64, nsec: u64) u64 {
    if (out_va == 0) return sc.syscall_err_invalid;
    const last, const overflow = @addWithOverflow(out_va, runtime_abi.timespec_size - 1);
    _ = last;
    if (overflow != 0) return sc.syscall_err_invalid;
    var bytes: [runtime_abi.timespec_size]u8 = undefined;
    std.mem.writeInt(u64, bytes[runtime_abi.timespec_sec_offset..][0..8], sec, .little);
    std.mem.writeInt(u64, bytes[runtime_abi.timespec_nsec_offset..][0..8], nsec, .little);
    // One checked copyout avoids resolving/mapping the same output page twice.
    // The shared copy path still owns permissions, COW and page boundaries;
    // a failed cross-page copy may leave partial output, as before.
    const copy_start = perf.runtimeTimestamp(.clock);
    defer {
        perf.runtimeAdd(.clock, .copy_calls, 1);
        perf.runtimeElapsed(.clock, .copy_cycles, copy_start);
    }
    if (!h.copy_bytes_to_user_va(proc, out_va, bytes[0..])) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

test "timespec copyout uses one checked ABI-sized copy" {
    const Probe = struct {
        var calls: usize = 0;
        var address: u64 = 0;
        var principal: kernel.PrincipalId = @enumFromInt(0);
        var output: [runtime_abi.timespec_size]u8 = undefined;
        var fail: bool = false;
        fn copy(proc: kernel.PrincipalId, dest: u64, bytes: []const u8) bool {
            calls += 1;
            address = dest;
            principal = proc;
            std.debug.assert(bytes.len == output.len);
            if (fail) return false;
            @memcpy(&output, bytes);
            return true;
        }
    };
    const hooks = .{ .copy_bytes_to_user_va = Probe.copy };
    const proc: kernel.PrincipalId = @enumFromInt(7);
    const expected = [_]u8{ 8, 7, 6, 5, 4, 3, 2, 1, 24, 23, 22, 21, 20, 19, 18, 17 };
    // Includes unaligned, cross-page and the final non-overflowing address.
    // The probe checks delegation; actual page permissions/COW are copyout's
    // responsibility and need guest coverage, not simulated success here.
    for ([_]u64{ 0x1000, 0x1003, 0x1ffb, std.math.maxInt(u64) - 15 }) |address| {
        Probe.calls = 0;
        Probe.fail = false;
        try std.testing.expectEqual(sc.syscall_ok, writeTimespec(hooks, proc, address, 0x0102030405060708, 0x1112131415161718));
        try std.testing.expectEqual(@as(usize, 1), Probe.calls);
        try std.testing.expectEqual(address, Probe.address);
        try std.testing.expectEqual(proc, Probe.principal);
        try std.testing.expectEqualSlices(u8, &expected, &Probe.output);
    }
    Probe.calls = 0;
    Probe.fail = true;
    try std.testing.expectEqual(sc.syscall_err_invalid, writeTimespec(hooks, proc, 0x1ffb, 1, 2));
    try std.testing.expectEqual(@as(usize, 1), Probe.calls);
    for ([_]u64{ 0, std.math.maxInt(u64) - 14, std.math.maxInt(u64) }) |address| {
        Probe.calls = 0;
        try std.testing.expectEqual(sc.syscall_err_invalid, writeTimespec(hooks, proc, address, 1, 2));
        try std.testing.expectEqual(@as(usize, 0), Probe.calls);
    }
}

fn monotonicTicksAsTimespec() struct { sec: u64, nsec: u64 } {
    if (realtime_clock.monotonicNs()) |ns| return .{ .sec = ns / 1_000_000_000, .nsec = ns % 1_000_000_000 };
    const tick = scheduler.lapic_tick_count;
    return .{
        .sec = tick / 1000,
        .nsec = (tick % 1000) * 1_000_000,
    };
}

fn clockGettime(h: anytype, proc: kernel.PrincipalId, clock_id: u64, out_va: u64) u64 {
    const body_start = perf.runtimeTimestamp(.clock);
    defer {
        perf.runtimeAdd(.clock, .body_calls, 1);
        perf.runtimeElapsed(.clock, .body_cycles, body_start);
    }
    return switch (clock_id) {
        runtime_abi.clock_realtime => if (realtime_clock.realtimeNs()) |ns|
            writeTimespec(h, proc, out_va, ns / 1_000_000_000, ns % 1_000_000_000)
        else
            writeTimespec(h, proc, out_va, realtime_clock.unixTimeSeconds(), 0),
        runtime_abi.clock_monotonic => blk: {
            const ts = monotonicTicksAsTimespec();
            break :blk writeTimespec(h, proc, out_va, ts.sec, ts.nsec);
        },
        else => sc.syscall_err_invalid,
    };
}

fn clockGettimeMaySkipKWithSource(comptime Source: type, clock_id: u64) bool {
    return switch (clock_id) {
        runtime_abi.clock_monotonic => Source.monotonicSourceReady(),
        // The published epoch is atomic and never withdrawn. Without either
        // source, unixTimeSeconds may touch the shared CMOS index/data pair.
        runtime_abi.clock_realtime => Source.monotonicSourceReady() or Source.publishedUnixTimeSeconds() != null,
        else => true, // invalid IDs return before reading a clock source
    };
}

pub fn clockGettimeMaySkipK(clock_id: u64) bool {
    return clockGettimeMaySkipKWithSource(realtime_clock, clock_id);
}

test "K-free clock admission rejects mutable fallback and accepts permanent publication" {
    const Source = struct {
        var highres = false;
        var seconds: ?u64 = null;
        fn monotonicSourceReady() bool { return highres; }
        fn publishedUnixTimeSeconds() ?u64 { return seconds; }
    };
    try std.testing.expect(!clockGettimeMaySkipKWithSource(Source, runtime_abi.clock_monotonic));
    try std.testing.expect(!clockGettimeMaySkipKWithSource(Source, runtime_abi.clock_realtime));
    Source.seconds = 42;
    try std.testing.expect(clockGettimeMaySkipKWithSource(Source, runtime_abi.clock_realtime));
    try std.testing.expect(!clockGettimeMaySkipKWithSource(Source, runtime_abi.clock_monotonic));
    Source.highres = true;
    try std.testing.expect(clockGettimeMaySkipKWithSource(Source, runtime_abi.clock_monotonic));
    try std.testing.expect(clockGettimeMaySkipKWithSource(Source, runtime_abi.clock_realtime));
    Source.highres = false;
    Source.seconds = null;
}

const ClockReadSource = struct {
    const monotonicNs = realtime_clock.monotonicNs;
    const realtimeNs = realtime_clock.realtimeNs;
    const publishedUnixTimeSeconds = realtime_clock.publishedUnixTimeSeconds;
    const unixTimeSeconds = realtime_clock.unixTimeSeconds;
    fn monotonicFallbackNs() u64 {
        return scheduler.lapic_tick_count *| 1_000_000;
    }
};

fn tryClockReadWithSource(comptime Source: type, frame: *TrapFrame) ?u64 {
    frame.rdx = 0;
    const value = switch (frame.rdi) {
        runtime_abi.clock_monotonic => Source.monotonicNs() orelse Source.monotonicFallbackNs(),
        runtime_abi.clock_realtime => Source.realtimeNs() orelse blk: {
            // CMOS fallback is serialized by the generic KernelState path;
            // do not introduce concurrent index/data reads on native APs.
            const seconds = Source.publishedUnixTimeSeconds() orelse return null;
            break :blk seconds *| 1_000_000_000;
        },
        else => return sc.syscall_err_invalid,
    };
    frame.rdx = value;
    return sc.syscall_ok;
}

pub fn tryClockRead(frame: *TrapFrame) ?u64 {
    return tryClockReadWithSource(ClockReadSource, frame);
}

/// The page is kernel-image memory published read-only. Its physical address
/// is fixed for the kernel's lifetime, so every open shares one VMO.
fn clockPageOpen(state: *kernel.KernelState, proc: kernel.PrincipalId, free_list: *kernel.FreePageList) u64 {
    const paddr = realtime_clock.clockPagePaddr(@import("../arch/x86_64/platform.zig").kernelPointerPaddr) orelse
        return sc.syscall_err_invalid;
    user_vm.lockSharedVmObjects();
    defer user_vm.unlockSharedVmObjects();
    const fd = state.openKernelClockPageFd(proc, paddr, .{}, abi_root.fd_abi.first_dynamic_fd, free_list) catch |err|
        return switch (err) {
            kernel.KernelError.TableFull, kernel.KernelError.OutOfFreePages => sc.syscall_err_alloc,
            else => sc.syscall_err_invalid,
        };
    return fd;
}

fn clockReadLockedWithSource(comptime Source: type, frame: *TrapFrame) u64 {
    if (tryClockReadWithSource(Source, frame)) |result| return result;
    frame.rdx = Source.unixTimeSeconds() *| 1_000_000_000;
    return sc.syscall_ok;
}

fn clockReadLocked(frame: *TrapFrame) u64 {
    return clockReadLockedWithSource(ClockReadSource, frame);
}

test "clock read register result keeps epochs and excludes RTC from fast path" {
    const Source = struct {
        var mono: ?u64 = 1234567890;
        var real: ?u64 = 9876543210;
        var seconds: ?u64 = 42;
        var rtc_calls: usize = 0;
        fn unixTimeSeconds() u64 {
            rtc_calls += 1;
            return 91;
        }
        fn monotonicNs() ?u64 {
            return mono;
        }
        fn realtimeNs() ?u64 {
            return real;
        }
        fn publishedUnixTimeSeconds() ?u64 {
            return seconds;
        }
        fn monotonicFallbackNs() u64 {
            return 12000000;
        }
    };
    var frame = std.mem.zeroes(TrapFrame);
    frame.rdi = runtime_abi.clock_monotonic;
    try std.testing.expectEqual(@as(?u64, sc.syscall_ok), tryClockReadWithSource(Source, &frame));
    try std.testing.expectEqual(@as(u64, 1234567890), frame.rdx);
    Source.mono = null;
    try std.testing.expectEqual(@as(?u64, sc.syscall_ok), tryClockReadWithSource(Source, &frame));
    try std.testing.expectEqual(@as(u64, 12000000), frame.rdx);
    frame.rdi = runtime_abi.clock_realtime;
    try std.testing.expectEqual(@as(?u64, sc.syscall_ok), tryClockReadWithSource(Source, &frame));
    try std.testing.expectEqual(@as(u64, 9876543210), frame.rdx);
    Source.real = null;
    try std.testing.expectEqual(@as(?u64, sc.syscall_ok), tryClockReadWithSource(Source, &frame));
    try std.testing.expectEqual(@as(u64, 42000000000), frame.rdx);
    Source.seconds = null;
    try std.testing.expectEqual(@as(?u64, null), tryClockReadWithSource(Source, &frame));
    try std.testing.expectEqual(@as(u64, 0), frame.rdx);
    try std.testing.expectEqual(@as(usize, 0), Source.rtc_calls);
    try std.testing.expectEqual(sc.syscall_ok, clockReadLockedWithSource(Source, &frame));
    try std.testing.expectEqual(@as(u64, 91000000000), frame.rdx);
    try std.testing.expectEqual(@as(usize, 1), Source.rtc_calls);
    frame.rdi = 99;
    frame.rdx = 123;
    try std.testing.expectEqual(@as(?u64, sc.syscall_err_invalid), tryClockReadWithSource(Source, &frame));
    try std.testing.expectEqual(@as(u64, 0), frame.rdx);
    Source.mono = 1234567890;
    Source.real = 9876543210;
    Source.seconds = 42;
    Source.rtc_calls = 0;
}

fn clockGetres(h: anytype, proc: kernel.PrincipalId, clock_id: u64, out_va: u64) u64 {
    if (clock_id == runtime_abi.clock_monotonic or clock_id == runtime_abi.clock_realtime) {
        if (realtime_clock.resolutionNs()) |resolution|
            return writeTimespec(h, proc, out_va, 0, resolution);
    }
    return switch (clock_id) {
        runtime_abi.clock_realtime => writeTimespec(
            h,
            proc,
            out_va,
            runtime_abi.clock_realtime_resolution_nsec / 1_000_000_000,
            runtime_abi.clock_realtime_resolution_nsec % 1_000_000_000,
        ),
        runtime_abi.clock_monotonic => writeTimespec(
            h,
            proc,
            out_va,
            runtime_abi.clock_monotonic_resolution_nsec / 1_000_000_000,
            runtime_abi.clock_monotonic_resolution_nsec % 1_000_000_000,
        ),
        else => sc.syscall_err_invalid,
    };
}

test "K-free clock resolution copies through checked user hook without KernelState" {
    const Probe = struct {
        var calls: usize = 0;
        fn copy(_: kernel.PrincipalId, _: u64, bytes: []const u8) bool {
            std.debug.assert(bytes.len == runtime_abi.timespec_size);
            calls += 1;
            return true;
        }
    };
    const hooks = .{ .copy_bytes_to_user_va = Probe.copy };
    const proc: kernel.PrincipalId = @enumFromInt(7);
    Probe.calls = 0;
    try std.testing.expectEqual(sc.syscall_ok, clockGetres(hooks, proc, runtime_abi.clock_monotonic, 0x1000));
    try std.testing.expectEqual(sc.syscall_ok, clockGetres(hooks, proc, runtime_abi.clock_realtime, 0x1000));
    try std.testing.expectEqual(sc.syscall_err_invalid, clockGetres(hooks, proc, 99, 0x1000));
    try std.testing.expectEqual(@as(usize, 2), Probe.calls);
}

fn systemInfo(h: anytype, proc: kernel.PrincipalId, out_va: u64) u64 {
    if (out_va == 0) return sc.syscall_err_invalid;
    const free_pages: u64 = @intCast(h.free_list.pageCount());
    const free_bytes, const free_overflow = @mulWithOverflow(free_pages, 4096);
    if (free_overflow != 0) return sc.syscall_err_invalid;
    var online_cpu_count: u64 = smp.onlineCpuSet().count();
    if (online_cpu_count == 0) online_cpu_count = 1;
    if (!h.write_user_u64(
        proc,
        out_va + runtime_abi.system_info_total_usable_memory_bytes_offset,
        h.total_usable_memory_bytes,
    )) return sc.syscall_err_invalid;
    if (!h.write_user_u64(
        proc,
        out_va + runtime_abi.system_info_free_memory_bytes_offset,
        free_bytes,
    )) return sc.syscall_err_invalid;
    if (!h.write_user_u64(
        proc,
        out_va + runtime_abi.system_info_online_cpu_count_offset,
        online_cpu_count,
    )) return sc.syscall_err_invalid;
    if (!h.write_user_u64(
        proc,
        out_va + runtime_abi.system_info_reserved_offset,
        0,
    )) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn nanosToTicks(nsec: u64) u64 {
    const tick_nsec: u64 = 1_000_000;
    return (nsec + tick_nsec - 1) / tick_nsec;
}

fn timespecToTicks(sec: u64, nsec: u64) ?u64 {
    if (nsec >= 1_000_000_000) return null;
    const sec_ticks, const sec_overflow = @mulWithOverflow(sec, 1000);
    if (sec_overflow != 0) return null;
    const extra = nanosToTicks(nsec);
    const ticks, const add_overflow = @addWithOverflow(sec_ticks, extra);
    if (add_overflow != 0) return null;
    return ticks;
}

fn nanosleep(h: anytype, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const req_va = frame.rdi;
    if (req_va == 0) return sc.syscall_err_invalid;
    const sec = h.read_user_u64(proc, req_va + runtime_abi.timespec_sec_offset) orelse return sc.syscall_err_invalid;
    const nsec = h.read_user_u64(proc, req_va + runtime_abi.timespec_nsec_offset) orelse return sc.syscall_err_invalid;
    const ticks = timespecToTicks(sec, nsec) orelse return sc.syscall_err_invalid;
    if (ticks == 0) return sc.syscall_ok;
    if (h.block_current_thread_for_event(frame, false, false, 0, ticks, sc.syscall_ok, h.before_current_thread_leave)) return frame.rax;
    return sc.syscall_err_not_ready;
}

fn yieldThread(comptime handoff: anytype, h: anytype, frame: *TrapFrame) u64 {
    if (handoff(frame, sc.syscall_ok, h.before_current_thread_leave)) {
        // The trap frame now belongs to the selected thread. Its return value
        // must survive; our zero was saved in the outgoing thread's context.
        return frame.rax;
    }
    return sc.syscall_ok;
}

test "yield result preserves the selected frame and saves caller success" {
    const Probe = struct {
        var switched = false;
        var calls: usize = 0;
        var releases: usize = 0;
        fn release(context: *anyopaque) void {
            const count: *usize = @ptrCast(@alignCast(context));
            count.* += 1;
        }
        fn handoff(frame: *TrapFrame, outgoing: u64, callback: ?scheduler.BeforeCurrentThreadLeaveCallback) bool {
            std.debug.assert(outgoing == 0);
            calls += 1;
            if (switched) {
                frame.rax = 0x12345678;
                if (callback) |cb| cb.run(cb.context);
            }
            return switched;
        }
    };
    const hooks = .{ .before_current_thread_leave = null };
    var frame = std.mem.zeroes(TrapFrame);
    frame.rax = sc.syscall_yield;
    Probe.switched = false;
    Probe.calls = 0;
    try std.testing.expectEqual(sc.syscall_ok, yieldThread(Probe.handoff, hooks, &frame));
    try std.testing.expectEqual(sc.syscall_yield, frame.rax);
    Probe.switched = true;
    try std.testing.expectEqual(@as(u64, 0x12345678), yieldThread(Probe.handoff, hooks, &frame));
    try std.testing.expectEqual(@as(usize, 2), Probe.calls);
    const locked_hooks = .{ .before_current_thread_leave = scheduler.BeforeCurrentThreadLeaveCallback{
        .context = &Probe.releases,
        .run = Probe.release,
    } };
    Probe.releases = 0;
    Probe.switched = false;
    try std.testing.expectEqual(sc.syscall_ok, yieldThread(Probe.handoff, locked_hooks, &frame));
    try std.testing.expectEqual(@as(usize, 0), Probe.releases);
    Probe.switched = true;
    try std.testing.expectEqual(@as(u64, 0x12345678), yieldThread(Probe.handoff, locked_hooks, &frame));
    try std.testing.expectEqual(@as(usize, 1), Probe.releases);
    // Production yield passes no K release callback once its scheduler-only
    // handoff is K-free. The explicit callback probe above still checks that
    // a handoff never drops a supplied lock on the no-switch branch.
    try std.testing.expect(!@import("lock_policy.zig").needsKernelStateLock(sc.syscall_yield));
}

fn scalarIdentity(nr: u64, proc: kernel.PrincipalId, thread: usize) ?u64 {
    return switch (nr) {
        sc.syscall_getpid => @intFromEnum(proc),
        sc.syscall_gettid => @as(u64, @intCast(thread)) + 1,
        else => null,
    };
}

test "K-free identity uses only pinned caller values" {
    const proc: kernel.PrincipalId = @enumFromInt(7);
    try std.testing.expectEqual(@as(?u64, 7), scalarIdentity(sc.syscall_getpid, proc, 42));
    try std.testing.expectEqual(@as(?u64, 43), scalarIdentity(sc.syscall_gettid, proc, 42));
    try std.testing.expectEqual(@as(?u64, null), scalarIdentity(sc.syscall_yield, proc, 42));
}

fn retireFutexWaiterLocked(waiter: *FutexWaiter) void {
    const removal_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_removal, removal_stage);
    removeFutexKeyHint(waiter);
    const cache = &futex_thread_cache[waiter.thread_index % futex_thread_cache.len];
    if (cache.waiter == waiter and cache.thread_index == waiter.thread_index and
        cache.thread_generation == waiter.thread_generation and cache.principal == waiter.principal)
    {
        // Remove the pointer before SlotTable can release/reuse its body.
        cache.waiter = null;
        cache.retired = false;
    }
    futex_waiters.remove(waiter, &futex_directory_leaf);
}

fn retainRetiredFutexWaiterLocked(waiter: *FutexWaiter) void {
    const removal_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_removal, removal_stage);
    const cache = &futex_thread_cache[waiter.thread_index % futex_thread_cache.len];
    if (cache.waiter == waiter and cache.thread_index == waiter.thread_index and
        cache.thread_generation == waiter.thread_generation and cache.principal == waiter.principal)
    {
        removeFutexKeyHint(waiter);
        // A completed wait keeps at most one inactive slot per cache bucket.
        // The slot body stays published, so the next wait neither scans the
        // directory nor rebuilds a detached SlotTable body. An evicted active
        // record is retired normally when its own completion arrives.
        waiter.active = false;
        cache.retired = true;
        return;
    }
    retireFutexWaiterLocked(waiter);
}

fn futexHasPreparedFree() bool {
    const flags = futex_directory_leaf.lock();
    defer futex_directory_leaf.unlock(flags);
    return futex_waiters.hasPreparedFree();
}

// No F or directory leaf is held during acquisition. Reacquire F before
// touching the owners, revalidate capacity, then publish under the leaf. The
// caller must reread the user word under F before registering a wait token.
fn growFutexStorage(h: anytype) bool {
    const growth_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_growth, growth_stage);
    if (@import("builtin").is_test and futex_test_fail_growth) return false;
    const allocator = if (@import("builtin").is_test)
        slot_storage.hostTestAllocator()
    else
        @import("../memory/identity_allocator.zig").allocator(h.free_list);
    var draft = @TypeOf(futex_waiters).Prepared.init(&futex_waiters, allocator);
    const reserved = draft.reserve();
    futex_waiters_lock.lock();
    defer futex_waiters_lock.unlock();
    defer draft.finish();
    reserved catch return false;
    const flags = futex_directory_leaf.lock();
    const result = futex_waiters.commitPrepared(&draft, std.math.maxInt(u32));
    futex_directory_leaf.unlock(flags);
    if (result) |path| futex_waiters.retirePath(path) else |err| {
        if (err != error.StalePreparation) return false;
    }
    futex_waiters.reclaim();
    return true;
}

fn reclaimStaleFutexWaitersLocked(comptime is_current: anytype) void {
    var iterator = futex_waiters.iteratorLocked(&futex_directory_leaf);
    while (iterator.next()) |item| {
        const waiter = item.value;
        if (!waiter.active) continue;
        if (is_current(
            waiter.principal,
            waiter.thread_index,
            waiter.thread_generation,
            waiter.wait_token,
        )) continue;
        retireFutexWaiterLocked(waiter);
    }
}

fn recordFutexWaiterLocked(
    proc: kernel.PrincipalId,
    thread_index: usize,
    thread_generation: u32,
    user_va: u64,
    wait_token: u64,
) bool {
    return recordFutexWaiterWithTokenCheckLocked(scheduler.waitTokenIsCurrent, proc, thread_index, thread_generation, user_va, wait_token);
}

fn recordFutexWaiterWithTokenCheckLocked(
    comptime is_current: anytype,
    proc: kernel.PrincipalId,
    thread_index: usize,
    thread_generation: u32,
    user_va: u64,
    wait_token: u64,
) bool {
    const cache = &futex_thread_cache[thread_index % futex_thread_cache.len];
    var reusable: ?*FutexWaiter = null;
    const matches = cache.thread_index == thread_index and
        cache.thread_generation == thread_generation and cache.principal == proc;
    if (matches) {
        if (cache.waiter) |waiter| {
            if (waiter.principal == proc and
                waiter.thread_index == thread_index and waiter.thread_generation == thread_generation)
            {
                removeFutexKeyHint(waiter);
                waiter.* = .{ .active = true, .principal = proc,
                    .thread_index = thread_index, .thread_generation = thread_generation,
                    .user_va = user_va, .wait_token = wait_token };
                addFutexKeyHint(waiter);
                cache.retired = false;
                return true;
            }
            // A stale hint is never authority for skipping the directory.
            if (cache.retired and !waiter.active) reusable = waiter;
        }
    } else if (cache.waiter) |waiter| {
        // A collision never removes a live waiter. It may leave another
        // active record for this thread, so search before reusing the cached
        // inactive slot. Removing a last live body first would detach the
        // SlotTable and make the following allocation fail under F.
        if (cache.retired and !waiter.active) reusable = waiter;
    }
    cache.* = .{};
    // An earlier hole must not hide this thread's registration at a higher
    // slot. Finish the replacement search before inserting into a free slot.
    var iterator = futex_waiters.iteratorLocked(&futex_directory_leaf);
    while (iterator.next()) |item| {
        const waiter = item.value;
        if (waiter.active and waiter.principal == proc and waiter.thread_index == thread_index) {
            if (reusable) |old| futex_waiters.remove(old, &futex_directory_leaf);
            removeFutexKeyHint(waiter);
            waiter.* = .{
                .active = true,
                .principal = proc,
                .thread_index = thread_index,
                .thread_generation = thread_generation,
                .user_va = user_va,
                .wait_token = wait_token,
            };
            addFutexKeyHint(waiter);
            cache.* = .{ .principal = proc, .thread_index = thread_index,
                .thread_generation = thread_generation, .waiter = waiter };
            return true;
        }
    }
    if (reusable) |waiter| {
        waiter.* = .{ .active = true, .principal = proc,
            .thread_index = thread_index, .thread_generation = thread_generation,
            .user_va = user_va, .wait_token = wait_token };
        addFutexKeyHint(waiter);
        cache.* = .{ .principal = proc, .thread_index = thread_index,
            .thread_generation = thread_generation, .waiter = waiter };
        return true;
    }
    if (recordFutexWaiterInFreeSlot(proc, thread_index, thread_generation, user_va, wait_token)) |waiter| {
        cache.* = .{ .principal = proc, .thread_index = thread_index,
            .thread_generation = thread_generation, .waiter = waiter };
        return true;
    }
    reclaimStaleFutexWaitersLocked(is_current);
    if (recordFutexWaiterInFreeSlot(proc, thread_index, thread_generation, user_va, wait_token)) |waiter| {
        cache.* = .{ .principal = proc, .thread_index = thread_index,
            .thread_generation = thread_generation, .waiter = waiter };
        return true;
    }
    return false;
}

fn recordFutexWaiterInFreeSlot(
    proc: kernel.PrincipalId,
    thread_index: usize,
    thread_generation: u32,
    user_va: u64,
    wait_token: u64,
) ?*FutexWaiter {
    const flags = futex_directory_leaf.lock();
    const allocation = futex_waiters.allocatePrepared();
    futex_directory_leaf.unlock(flags);
    const slot = allocation catch return null;
    slot.value.* = .{
        .active = true,
        .principal = proc,
        .thread_index = thread_index,
        .thread_generation = thread_generation,
        .user_va = user_va,
        .wait_token = wait_token,
    };
    addFutexKeyHint(slot.value);
    return slot.value;
}

fn clearFutexWaiter(
    proc: kernel.PrincipalId,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
) void {
    futex_waiters_lock.lock();
    defer futex_waiters_lock.unlock();
    var iterator = futex_waiters.iteratorLocked(&futex_directory_leaf);
    while (iterator.next()) |item| {
        const waiter = item.value;
        if (!waiter.active) continue;
        if (waiter.principal != proc or
            waiter.thread_index != thread_index or
            waiter.thread_generation != thread_generation or
            waiter.wait_token != wait_token) continue;
        retireFutexWaiterLocked(waiter);
        return;
    }
}

const FutexBlockProfile = struct {
    original: ?scheduler.BeforeCurrentThreadLeaveCallback,
    start: u64,
    recorded: bool = false,

    fn beforeLeave(context: *anyopaque) void {
        const self: *FutexBlockProfile = @ptrCast(@alignCast(context));
        if (self.original) |callback| callback.run(callback.context);
        // AP block never returns through the syscall's C frame. Record at
        // the real leave boundary; BSP return must not count it again. The
        // interval includes the original KernelState lock-drop callback.
        perf.runtimeElapsed(.futex_wait, .body_cycles, self.start);
        perf.runtimeAdd(.futex_wait, .reserved, 1);
        self.recorded = true;
    }
};

test "futex block profile records the leave boundary before a nonreturning park" {
    const Probe = struct {
        fn release(context: *anyopaque) void {
            const calls: *usize = @ptrCast(@alignCast(context));
            calls.* += 1;
        }
    };
    var calls: usize = 0;
    var profile = FutexBlockProfile{
        .original = .{ .context = &calls, .run = Probe.release },
        .start = 0,
    };
    try std.testing.expect(!profile.recorded);
    FutexBlockProfile.beforeLeave(&profile);
    try std.testing.expect(profile.recorded);
    try std.testing.expectEqual(@as(usize, 1), calls);
    // The returning BSP path uses this same flag to avoid a second record.
    if (!profile.recorded) FutexBlockProfile.beforeLeave(&profile);
    try std.testing.expectEqual(@as(usize, 1), calls);
    profile = .{ .original = null, .start = 0 };
    FutexBlockProfile.beforeLeave(&profile);
    try std.testing.expect(profile.recorded);
}

fn futexWait(h: anytype, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    if (comptime lock_profile.enabled) lock_profile.futexCall(.wait, @intFromEnum(proc), kernel.principalLabel(proc));
    const validation_profile = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    const user_va = frame.rdi;
    const expected: u32 = @truncate(frame.rsi);
    const timeout_ticks = frame.rdx;
    const flags = frame.r10;
    if ((flags & ~runtime_abi.wait_uninterruptible) != 0 or user_va == 0 or (user_va & 0x3) != 0) {
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_profile);
        return sc.syscall_err_invalid;
    }

    const thread_index = scheduler.currentThread();
    const thread_generation = scheduler.currentThreadGeneration() orelse {
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_profile);
        return sc.syscall_err_invalid;
    };
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_profile);
    wake_trace.record(.futex_wait_enter, user_va, 0, thread_index);

    // FUTEX_WAIT must compare the userspace word and enqueue the waiter
    // atomically with respect to FUTEX_WAKE.  A wake performs its userspace
    // store before entering futexWake(), then takes this same lock.  Holding
    // the lock across the comparison and waiter publication therefore gives
    // the two operations a single order: the waiter either observes the new
    // value or the subsequent wake observes the waiter.
    var wait_token: u64 = 0;
    var claim_start: u64 = 0;
    while (true) {
        const lock_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        futex_waiters_lock.lock();
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_lock, lock_stage);
        wake_trace.record(.futex_lock, user_va, 0, thread_index);
        // K-free WAIT needs the same mapping through comparison and F
        // publication. F -> A is the established order; A is released before
        // storage growth or park, which can allocate or switch context.
        if (!lockFutexUserAddressSpace(h, proc)) {
            futex_waiters_lock.unlock();
            return sc.syscall_err_invalid;
        }
        const word_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        const copy_start = perf.runtimeTimestamp(.futex_wait);
        const current = readUserU32UnderFutexAndAddressSpaceLocks(h, proc, user_va) orelse {
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_user_word, word_stage);
            perf.runtimeAdd(.futex_wait, .invalid, 1);
            perf.runtimeAdd(.futex_wait, .copy_calls, 1);
            perf.runtimeElapsed(.futex_wait, .copy_cycles, copy_start);
            unlockFutexUserAddressSpace(h, proc);
            futex_waiters_lock.unlock();
            return sc.syscall_err_invalid;
        };
        wake_trace.record(.futex_word, user_va, current, thread_index);
        perf.runtimeAdd(.futex_wait, .copy_calls, 1);
        perf.runtimeElapsed(.futex_wait, .copy_cycles, copy_start);
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_user_word, word_stage);
        if (current != expected) {
            if (comptime lock_profile.enabled) {
                const mismatch_stage = lock_profile.stageBegin();
                lock_profile.stageEnd(.futex_wait_mismatch, mismatch_stage);
                lock_profile.futexOutcome(.wait_mismatch);
            }
            perf.runtimeAdd(.futex_wait, .no_claim, 1);
            unlockFutexUserAddressSpace(h, proc);
            futex_waiters_lock.unlock();
            return sc.syscall_err_not_ready;
        }
        const registration_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        if (!futexHasPreparedFree()) {
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_registration, registration_stage);
            unlockFutexUserAddressSpace(h, proc);
            futex_waiters_lock.unlock();
            if (!growFutexStorage(h)) return sc.syscall_err_alloc;
            continue;
        }
        claim_start = perf.runtimeTimestamp(.futex_wait);
        wait_token = scheduler.reserveCurrentWaitToken(thread_generation) orelse {
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_registration, registration_stage);
            perf.runtimeAdd(.futex_wait, .invalid, 1);
            unlockFutexUserAddressSpace(h, proc);
            futex_waiters_lock.unlock();
            return sc.syscall_err_invalid;
        };
        wake_trace.record(.futex_token, user_va, wait_token, thread_index);
        if (!recordFutexWaiterLocked(proc, thread_index, thread_generation, user_va, wait_token)) {
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_registration, registration_stage);
            perf.runtimeAdd(.futex_wait, .invalid, 1);
            unlockFutexUserAddressSpace(h, proc);
            futex_waiters_lock.unlock();
            scheduler.cancelCurrentWaitToken(thread_generation, wait_token);
            return sc.syscall_err_alloc;
        }
        wake_trace.record(.futex_record, user_va, wait_token, thread_index);
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_registration, registration_stage);
        unlockFutexUserAddressSpace(h, proc);
        futex_waiters_lock.unlock();
        break;
    }
    perf.runtimeAdd(.futex_wait, .claim_calls, 1);
    perf.runtimeElapsed(.futex_wait, .claim_cycles, claim_start);
    wake_trace.record(.futex_wait_reserved, user_va, wait_token, thread_index);

    const timeout_result = if (timeout_ticks == 0)
        sc.syscall_err_not_ready
    else
        runtime_abi.futex_wait_timed_out_status;
    const block_start = perf.runtimeTimestamp(.futex_wait);
    var block_profile = FutexBlockProfile{
        .original = h.before_current_thread_leave,
        .start = block_start,
    };
    const before_block = if (comptime perf.enabled)
        scheduler.BeforeCurrentThreadLeaveCallback{ .context = &block_profile, .run = FutexBlockProfile.beforeLeave }
    else
        h.before_current_thread_leave;
    perf.runtimeAdd(.futex_wait, .body_calls, 1);
    wake_trace.record(.futex_wait_block, user_va, wait_token, thread_index);
    const timer_park_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    lock_profile.parkBegin();
    defer lock_profile.parkEnd();
    const blocked = h.block_current_thread_for_event(
        frame,
        true,
        (flags & runtime_abi.wait_uninterruptible) != 0,
        wait_token,
        timeout_ticks,
        timeout_result,
        before_block,
    );
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_timer_park, timer_park_stage);
    if (comptime perf.enabled) {
        if (!block_profile.recorded) {
            perf.runtimeElapsed(.futex_wait, .body_cycles, block_start);
            perf.runtimeAdd(.futex_wait, if (blocked) .claimed else .invalid, 1);
        }
    }
    if (blocked) {
        wake_trace.record(.futex_wait_return, user_va, wait_token, thread_index);
        return frame.rax;
    }
    const cleanup_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    scheduler.cancelCurrentWaitToken(thread_generation, wait_token);
    // FUTEX_REQUEUE may have changed this waiter's key while it slept.  The
    // reserved token uniquely identifies the wait, so cleanup must not depend
    // on the original userspace address.
    clearFutexWaiter(proc, thread_index, thread_generation, wait_token);
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_cleanup, cleanup_stage);
    wake_trace.record(.futex_wait_return, user_va, wait_token, thread_index);
    return sc.syscall_err_not_ready;
}

fn futexWake(proc: kernel.PrincipalId, user_va: u64, max_count: u64) u64 {
    return futexWakeCollect(scheduler.wakeIfWaitingTokenGenerationWithRax, proc, user_va, max_count, null);
}

const WakeWaitScheduler = struct {
    const currentThread = scheduler.currentThread;
    const currentThreadGeneration = scheduler.currentThreadGeneration;
    const reserveCurrentWaitToken = scheduler.reserveCurrentWaitToken;
    const cancelCurrentWaitToken = scheduler.cancelCurrentWaitToken;
    const waitTokenIsCurrent = scheduler.waitTokenIsCurrent;
    const tryTransfer = scheduler.tryWakeWaitTransfer;
    const wake = scheduler.wakeIfWaitingTokenGenerationWithRax;
    const reselectBlocked = scheduler.resumeAfterWakeWaitInstallFailure;
};

const WakeWaitPeerResult = struct {
    count: u64 = 0,
    failed: bool = false,
    transfer: ?bool = null,
};

fn wakeWaitPeerLocked(comptime Ops: type, proc: kernel.PrincipalId, wake_va: u64, frame: *TrapFrame, caller_id: usize, caller_generation: u32, caller_token: u64, registered: bool, timeout_ticks: u64, resume_rax: u64, wait_uninterruptible: bool) WakeWaitPeerResult {
    const hint_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    const empty = futexKeyKnownEmpty(proc, wake_va);
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_hint, hint_stage);
    if (empty) return .{};
    const scan_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_scan, scan_stage);
    var iterator = futexKeyIterator(proc, wake_va);
    while (iterator.next()) |waiter| {
        // Conceptual wake-before-own-wait ordering must survive alias keys:
        // registering the caller early cannot let it consume its own wake.
        if (caller_token != 0 and waiter.thread_index == caller_id and
            waiter.thread_generation == caller_generation and waiter.wait_token == caller_token) continue;
        if (registered) {
            if (comptime perf.enabled) perf.runtimeAdd(.futex_wake_wait, .claim_calls, 1);
            const transfer_start = perf.runtimeTimestamp(.futex_wake_wait);
            const wake_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            const transferred = Ops.tryTransfer(frame, .{
                .peer = waiter.thread_index,
                .peer_generation = waiter.thread_generation,
                .owner = proc,
                .peer_token = waiter.wait_token,
                .caller_token = caller_token,
                .wait_uninterruptible = wait_uninterruptible,
                .timeout_ticks = timeout_ticks,
                .resume_rax = resume_rax,
            });
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_wake_dispatch, wake_stage);
            // The live frame may now be the receiver. This measurement uses
            // only the pre-transfer timestamp and returned result, not its args.
            perf.runtimeElapsed(.futex_wake_wait, .body_cycles, transfer_start);
            if (transferred) |installed| {
                if (comptime perf.enabled) perf.runtimeAdd(.futex_wake_wait, if (installed) .claimed else .reserved, 1);
                retainRetiredFutexWaiterLocked(waiter);
                return .{ .count = 1, .transfer = installed };
            }
            if (comptime perf.enabled) perf.runtimeAdd(.futex_wake_wait, .no_claim, 1);
        }
        const wake_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        const wake_result = Ops.wake(waiter.thread_index, waiter.thread_generation, proc, waiter.wait_token, sc.syscall_ok);
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_wake_dispatch, wake_stage);
        switch (wake_result) {
            .woke => {
                retainRetiredFutexWaiterLocked(waiter);
                return .{ .count = 1 };
            },
            .stale => retireFutexWaiterLocked(waiter),
            // No wake was committed. Retain the exact peer record and return
            // failure rather than confusing publication failure with wake0.
            .publish_failed => return .{ .failed = true },
        }
    }
    return .{};
}

fn futexWakeWaitWithOps(comptime Ops: type, h: anytype, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    if (comptime lock_profile.enabled) lock_profile.futexCall(.wake_wait, @intFromEnum(proc), kernel.principalLabel(proc));
    const validation_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    // This denominator precedes every possible non-returning AP block tail.
    // Columns 0..5 remain dispatcher timings, not frontend substage timings.
    // Here body_calls=entered, copy_calls=registered, claim_calls=attempted,
    // claimed=installed, reserved=install failed, no_claim=rejected,
    // invalid=outbound publish failed. copy_cycles is checked-copy elapsed,
    // claim_cycles is inclusive registration (including copy), body_cycles
    // is inclusive backend transfer. Amortize copy/registration by entered,
    // not successful registrations; transfer uses the attempt denominator.
    if (comptime perf.enabled) perf.runtimeAdd(.futex_wake_wait, .body_calls, 1);
    const registration_start = perf.runtimeTimestamp(.futex_wake_wait);
    const wake_va = frame.rdi;
    const wait_va = frame.rsi;
    const expected: u32 = @truncate(frame.r10);
    const timeout_ticks = frame.r8;
    // RDX has no input role. The common entry also sets this before admission,
    // so every failure has a defined outbound-completion result.
    frame.rdx = std.math.maxInt(u64);
    const flags = frame.r9;
    if ((flags & ~runtime_abi.wait_uninterruptible) != 0) {
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_stage);
        return sc.syscall_err_invalid;
    }
    const wait_uninterruptible = (flags & runtime_abi.wait_uninterruptible) != 0;
    if (wake_va == 0 or (wake_va & 3) != 0) {
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_stage);
        return sc.syscall_err_invalid;
    }
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_stage);
    const caller_id = Ops.currentThread();
    const generation = Ops.currentThreadGeneration();
    const caller_generation = generation orelse 0;
    var caller_token: u64 = 0;
    var registered = false;
    var wait_status = sc.syscall_err_invalid;
    const resume_rax = if (timeout_ticks == 0) sc.syscall_err_not_ready else runtime_abi.futex_wait_timed_out_status;

    // Comparison, registration and outbound selection share the existing
    // futex order. A concurrent wake either sees the new record or changes
    // the checked word before publication; no transport/user bytes are reused.
    var capacity_failed = false;
    while (true) {
        const lock_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        futex_waiters_lock.lock();
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_lock, lock_stage);
        if (wait_va != 0 and (wait_va & 3) == 0 and generation != null) {
            // Keep A under F until this waiter's token and key are published.
            // The outbound scheduler transfer below runs only after A drops.
            if (!lockFutexUserAddressSpace(h, proc)) break;
            const word_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            const copy_start = perf.runtimeTimestamp(.futex_wake_wait);
            const user_word = readUserU32UnderFutexAndAddressSpaceLocks(h, proc, wait_va);
            perf.runtimeElapsed(.futex_wake_wait, .copy_cycles, copy_start);
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_user_word, word_stage);
            if (user_word) |current| {
                if (current != expected) {
                    if (comptime lock_profile.enabled) {
                        const mismatch_stage = lock_profile.stageBegin();
                        lock_profile.stageEnd(.futex_wait_mismatch, mismatch_stage);
                    }
                    wait_status = sc.syscall_err_not_ready;
                } else if (!capacity_failed and !futexHasPreparedFree()) {
                    unlockFutexUserAddressSpace(h, proc);
                    futex_waiters_lock.unlock();
                    capacity_failed = !growFutexStorage(h);
                    continue;
                } else if (capacity_failed) {
                    wait_status = sc.syscall_err_alloc;
                } else if (Ops.reserveCurrentWaitToken(caller_generation)) |token| {
                    const registration_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
                    caller_token = token;
                    registered = recordFutexWaiterWithTokenCheckLocked(Ops.waitTokenIsCurrent, proc, caller_id, caller_generation, wait_va, token);
                    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_registration, registration_stage);
                    wait_status = if (registered) sc.syscall_ok else sc.syscall_err_alloc;
                }
            }
            unlockFutexUserAddressSpace(h, proc);
        }
        break;
    }
    if (comptime perf.enabled) {
        if (registered) perf.runtimeAdd(.futex_wake_wait, .copy_calls, 1);
    }
    // This interval ends before outbound selection can replace the frame or
    // invoke a non-returning leave. It includes key-lock wait, copy and token.
    perf.runtimeElapsed(.futex_wake_wait, .claim_cycles, registration_start);
    // Wait-side failure must not suppress a valid outbound wake, nor invite a
    // retry of this compound operation. Wake0 is a successful outbound step.
    const peer = wakeWaitPeerLocked(Ops, proc, wake_va, frame, caller_id, caller_generation, caller_token, registered, timeout_ticks, resume_rax, wait_uninterruptible);
    futex_waiters_lock.unlock();
    if (peer.transfer) |installed| {
        // A transfer has saved the caller's result and may have replaced the
        // live frame with the receiver. Never overwrite that receiver's RDX.
        if (h.before_current_thread_leave) |callback| callback.run(callback.context);
        if (!installed) Ops.reselectBlocked(frame);
        return frame.rax;
    }
    if (peer.failed or !registered) {
        if (comptime perf.enabled) {
            if (peer.failed) perf.runtimeAdd(.futex_wake_wait, .invalid, 1);
        }
        if (caller_token != 0) {
            Ops.cancelCurrentWaitToken(caller_generation, caller_token);
            if (registered) clearFutexWaiter(proc, caller_id, caller_generation, caller_token);
        }
        if (peer.failed) return sc.syscall_err_alloc;
        frame.rdx = peer.count;
        return wait_status;
    }
    frame.rdx = peer.count;
    const timer_park_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    lock_profile.parkBegin();
    defer lock_profile.parkEnd();
    if (h.block_current_thread_for_event(frame, true, wait_uninterruptible, caller_token, timeout_ticks, resume_rax, h.before_current_thread_leave)) return frame.rax;
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_timer_park, timer_park_stage);
    const cleanup_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    Ops.cancelCurrentWaitToken(caller_generation, caller_token);
    // REQUEUE may have changed the key; completion ownership is this token,
    // not the original wait address. Outbound wake is never sent again.
    clearFutexWaiter(proc, caller_id, caller_generation, caller_token);
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_cleanup, cleanup_stage);
    return sc.syscall_err_not_ready;
}

fn futexWakeWait(h: anytype, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    return futexWakeWaitWithOps(WakeWaitScheduler, h, proc, frame);
}

fn resetFutexTestStorage() void {
    futex_waiters.deinit();
    @memset(&futex_thread_cache, .{});
    @memset(&futex_key_hints, .{});
    @memset(&futex_key_buckets, null);
    futex_test_key_visits = 0;
    futex_test_fail_growth = false;
    _ = futex_waiters.prepareFree(slot_storage.hostTestAllocator()) catch unreachable;
}

fn testFutexSlot(index: u32) *FutexWaiter {
    futex_waiters.prepareIndex(index, slot_storage.hostTestAllocator()) catch unreachable;
    return futex_waiters.claimIndex(index) catch unreachable;
}

fn testFutexValue(index: u32) FutexWaiter {
    if (futex_waiters.get(index)) |value| {
        var snapshot = value.*;
        // Link pointers describe a mutable F-owned index, not waiter identity.
        // A value snapshot must not recursively compare another live record.
        snapshot.key_prev = null;
        snapshot.key_next = null;
        return snapshot;
    }
    return .{};
}

fn testSetFutexSlot(index: u32, value: FutexWaiter) void {
    // These fixtures need the same publication invariant as production:
    // a live slot without its hint makes the empty-key fast path lose wakes.
    if (futex_waiters.get(index)) |old| removeFutexKeyHint(old);
    const slot = testFutexSlot(index);
    slot.* = value;
    slot.key_prev = null;
    slot.key_next = null;
    if (slot.active) addFutexKeyHint(slot);
}

const WakeWaitProbe = struct {
    const owner: kernel.PrincipalId = @enumFromInt(44);
    var word: u32 = 0;
    var copy_ok = true;
    var generation: ?u32 = 7;
    var reserve_ok = true;
    var transfer: ?bool = null;
    var publication_failed = false;
    var stale_id: ?usize = null;
    var block_mode: enum { interrupt, early, switch_context } = .interrupt;
    var requeue_on_block = false;
    var copies: usize = 0;
    var reserves: usize = 0;
    var cancels: usize = 0;
    var wakes: usize = 0;
    var transfers: usize = 0;
    var blocks: usize = 0;
    var leaves: usize = 0;
    var reselects: usize = 0;
    var observed_count: u64 = 99;
    var saved_count: u64 = 99;
    var peer_index: usize = 1;
    var as_locked = false;

    fn reset() TrapFrame {
        resetFutexTestStorage();

        testSetFutexSlot(@intCast(1), .{ .active = true, .principal = owner, .thread_index = 9, .thread_generation = 17, .user_va = 0x1000, .wait_token = 55 });
        testSetFutexSlot(@intCast(2), .{ .active = true, .principal = @enumFromInt(45), .thread_index = 99, .thread_generation = 18, .user_va = 0x1000, .wait_token = 56 });
        word = 0;
        copy_ok = true;
        generation = 7;
        reserve_ok = true;
        transfer = null;
        publication_failed = false;
        stale_id = null;
        block_mode = .interrupt;
        requeue_on_block = false;
        copies = 0;
        reserves = 0;
        cancels = 0;
        wakes = 0;
        transfers = 0;
        blocks = 0;
        leaves = 0;
        reselects = 0;
        observed_count = 99;
        saved_count = 99;
        peer_index = 1;
        as_locked = false;
        var frame = std.mem.zeroes(TrapFrame);
        frame.rdi = 0x1000;
        frame.rsi = 0x2000;
        frame.r10 = 0;
        frame.r8 = 0;
        frame.rdx = 0xcccc;
        return frame;
    }
    fn currentThread() usize {
        return 4;
    }
    fn currentThreadGeneration() ?u32 {
        const id = currentThread();
        std.debug.assert(id == 4);
        return generation;
    }
    fn reserveCurrentWaitToken(gen: u32) ?u64 {
        std.debug.assert(futex_waiters_lock.value == 1 and as_locked and gen == 7);
        reserves += 1;
        return if (reserve_ok) 101 else null;
    }
    fn cancelCurrentWaitToken(gen: u32, token: u64) void {
        std.debug.assert(futex_waiters_lock.value == 0 and gen == 7 and token == 101);
        cancels += 1;
    }
    fn waitTokenIsCurrent(_: kernel.PrincipalId, _: usize, _: u32, _: u64) bool {
        return true;
    }
    fn tryTransfer(frame: *TrapFrame, request: scheduler.WakeWaitTransfer) ?bool {
        std.debug.assert(futex_waiters_lock.value == 1 and !as_locked and request.owner == owner and
            request.peer != 4 and request.caller_token == 101);
        transfers += 1;
        if (transfer) |installed| {
            saved_count = 1;
            frame.rdx = 1;
            if (installed) {
                frame.rax = 0x7788;
                frame.rdx = 0xef01;
            }
        }
        return transfer;
    }
    fn wake(id: usize, gen: u32, principal: kernel.PrincipalId, token: u64, result: u64) scheduler.KeyedWakeResult {
        std.debug.assert(futex_waiters_lock.value == 1 and principal == owner and result == 0);
        std.debug.assert(!(id == 4 and gen == 7 and token == 101));
        wakes += 1;
        if (stale_id == id) return .stale;
        return if (publication_failed) .publish_failed else .woke;
    }
    fn copy(_: kernel.PrincipalId, _: u64, bytes: []u8) bool {
        _ = bytes;
        @panic("checked user copy under futex lock");
    }
    fn resident(_: kernel.PrincipalId, _: u64) ?u32 {
        std.debug.assert(futex_waiters_lock.value == 1 and as_locked);
        copies += 1;
        return if (copy_ok) word else null;
    }
    fn lockA(_: kernel.PrincipalId) bool {
        std.debug.assert(futex_waiters_lock.value == 1 and !as_locked);
        as_locked = true;
        return true;
    }
    fn unlockA(_: kernel.PrincipalId) void {
        std.debug.assert(futex_waiters_lock.value == 1 and as_locked);
        as_locked = false;
    }
    fn leave(context: *anyopaque) void {
        const count: *usize = @ptrCast(@alignCast(context));
        std.debug.assert(futex_waiters_lock.value == 0 and !testFutexValue(@intCast(peer_index)).active);
        count.* += 1;
    }
    fn reselectBlocked(frame: *TrapFrame) void {
        std.debug.assert(futex_waiters_lock.value == 0 and leaves == 1 and saved_count == 1);
        reselects += 1;
        frame.rax = 0x9988;
        frame.rdx = 0xabcd;
    }
    fn block(frame: *TrapFrame, mailbox: bool, _: bool, token: u64, _: u64, _: u64, callback: ?scheduler.BeforeCurrentThreadLeaveCallback) bool {
        std.debug.assert(futex_waiters_lock.value == 0 and mailbox and token == 101);
        blocks += 1;
        observed_count = frame.rdx;
        if (requeue_on_block) {
            std.debug.assert(futexRequeueWithWake(wake, owner, 0x2000, 0, 1, 0x7000) == 1);
            std.debug.assert(testFutexValue(@intCast(0)).user_va == 0x7000);
        }
        switch (block_mode) {
            .interrupt => return false,
            .early => {
                // Model an early keyed wake retiring our record and storing
                // RAX before block. Existing block leaves live RDX untouched.
                clearFutexWaiter(owner, 4, 7, token);
                frame.rax = 0;
                return true;
            },
            .switch_context => {
                saved_count = frame.rdx;
                if (callback) |cb| cb.run(cb.context);
                frame.rax = 0x9988;
                frame.rdx = 0xabcd;
                return true;
            },
        }
    }
    fn run(frame: *TrapFrame) u64 {
        const callback: ?scheduler.BeforeCurrentThreadLeaveCallback = .{ .context = &leaves, .run = leave };
        return futexWakeWaitWithOps(@This(), .{
            .copy_user_bytes_from_va = copy,
            .futex_lock_address_space = @as(?*const fn (kernel.PrincipalId) bool, lockA),
            .futex_unlock_address_space = @as(?*const fn (kernel.PrincipalId) void, unlockA),
            .resident_read_user_u32_with_address_space_locked = @as(?*const fn (kernel.PrincipalId, u64) ?u32, resident),
            .before_current_thread_leave = callback,
            .block_current_thread_for_event = block,
        }, owner, frame);
    }
};

test "futex wake wait frontend alias exclusion and signal rejection retain completed wake" {
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    var frame = WakeWaitProbe.reset();
    frame.rsi = frame.rdi;
    try std.testing.expectEqual(sc.syscall_err_not_ready, WakeWaitProbe.run(&frame));
    try std.testing.expectEqual(@as(u64, 1), frame.rdx);
    try std.testing.expectEqual(@as(usize, 1), WakeWaitProbe.wakes);
    try std.testing.expectEqual(@as(usize, 1), WakeWaitProbe.blocks);
    try std.testing.expectEqual(@as(usize, 1), WakeWaitProbe.cancels);
    try std.testing.expect(!testFutexValue(@intCast(0)).active and !testFutexValue(@intCast(1)).active);
    try std.testing.expect(testFutexValue(@intCast(2)).active);
    try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.leaves);
}

test "futex wake wait frontend signal rejection cancels requeued caller by token" {
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    var frame = WakeWaitProbe.reset();
    WakeWaitProbe.requeue_on_block = true;
    try std.testing.expectEqual(sc.syscall_err_not_ready, WakeWaitProbe.run(&frame));
    try std.testing.expectEqual(@as(u64, 1), frame.rdx);
    try std.testing.expectEqual(@as(usize, 1), WakeWaitProbe.wakes);
    try std.testing.expectEqual(@as(usize, 1), WakeWaitProbe.cancels);
    try std.testing.expect(!testFutexValue(@intCast(0)).active and !testFutexValue(@intCast(1)).active);
    try std.testing.expect(testFutexValue(@intCast(2)).active);
}

test "futex wake wait frontend failure never suppresses valid wake or parks uncertain publication" {
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    for (0..7) |scenario| {
        var frame = WakeWaitProbe.reset();
        switch (scenario) {
            0 => WakeWaitProbe.word = 1,
            1 => WakeWaitProbe.copy_ok = false,
            2 => frame.rsi += 1,
            3 => WakeWaitProbe.generation = null,
            4 => WakeWaitProbe.reserve_ok = false,
            5 => WakeWaitProbe.publication_failed = true,
            6 => frame.rdi += 1,
            else => unreachable,
        }
        const status = WakeWaitProbe.run(&frame);
        try std.testing.expectEqual(if (scenario == 0) sc.syscall_err_not_ready else if (scenario == 5)
            sc.syscall_err_alloc
        else
            sc.syscall_err_invalid, status);
        try std.testing.expectEqual(if (scenario >= 5) std.math.maxInt(u64) else @as(u64, 1), frame.rdx);
        try std.testing.expectEqual(if (scenario == 6) @as(usize, 0) else @as(usize, 1), WakeWaitProbe.wakes);
        try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.blocks);
        try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.leaves);
        try std.testing.expectEqual(scenario >= 5, testFutexValue(@intCast(1)).active);
        try std.testing.expect(!testFutexValue(@intCast(0)).active and testFutexValue(@intCast(2)).active);
        try std.testing.expectEqual(if (scenario == 5) @as(usize, 1) else @as(usize, 0), WakeWaitProbe.cancels);
    }
}

test "futex wake wait frontend record exhaustion wakes once and cancels unpublished caller" {
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    var frame = WakeWaitProbe.reset();
    for (0..futex_waiters.capacity) |index| testSetFutexSlot(@intCast(index), .{ .active = true, .principal = WakeWaitProbe.owner, .thread_index = 10 + index, .thread_generation = 17, .user_va = 0x3000, .wait_token = 55 });
    var first = testFutexValue(0);
    first.user_va = frame.rdi;
    testSetFutexSlot(0, first);

    futex_test_fail_growth = true;
    try std.testing.expectEqual(sc.syscall_err_alloc, WakeWaitProbe.run(&frame));
    try std.testing.expectEqual(@as(u64, 1), frame.rdx);
    try std.testing.expectEqual(@as(usize, 1), WakeWaitProbe.wakes);
    try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.cancels);
    try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.blocks);
    try std.testing.expect(!testFutexValue(@intCast(0)).active);
}

test "futex wake wait frontend transfer retires peer before leave and reselects failed install once" {
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    for ([_]bool{ true, false }) |installed| {
        var frame = WakeWaitProbe.reset();
        WakeWaitProbe.transfer = installed;
        try std.testing.expectEqual(if (installed) @as(u64, 0x7788) else @as(u64, 0x9988), WakeWaitProbe.run(&frame));
        try std.testing.expectEqual(if (installed) @as(u64, 0xef01) else @as(u64, 0xabcd), frame.rdx);
        try std.testing.expectEqual(@as(u64, 1), WakeWaitProbe.saved_count);
        try std.testing.expect(testFutexValue(@intCast(0)).active and !testFutexValue(@intCast(1)).active and testFutexValue(@intCast(2)).active);
        try std.testing.expectEqual(@as(usize, 1), WakeWaitProbe.leaves);
        try std.testing.expectEqual(if (installed) @as(usize, 0) else @as(usize, 1), WakeWaitProbe.reselects);
        try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.wakes);
        try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.blocks);
        try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.cancels);
    }
}

test "futex wake wait frontend early completion fallback wake0 and stale peer preserve frame ownership" {
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    var frame = WakeWaitProbe.reset();
    WakeWaitProbe.block_mode = .early;
    try std.testing.expectEqual(sc.syscall_ok, WakeWaitProbe.run(&frame));
    try std.testing.expectEqual(@as(u64, 1), frame.rdx);
    try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.leaves);
    try std.testing.expect(!testFutexValue(@intCast(0)).active);

    frame = WakeWaitProbe.reset();
    WakeWaitProbe.block_mode = .switch_context;
    try std.testing.expectEqual(@as(u64, 0x9988), WakeWaitProbe.run(&frame));
    try std.testing.expectEqual(@as(u64, 0xabcd), frame.rdx);
    try std.testing.expectEqual(@as(u64, 1), WakeWaitProbe.saved_count);
    try std.testing.expectEqual(@as(usize, 1), WakeWaitProbe.leaves);

    frame = WakeWaitProbe.reset();
    var inactive = testFutexValue(1);
    inactive.active = false;
    testSetFutexSlot(1, inactive);
    try std.testing.expectEqual(sc.syscall_err_not_ready, WakeWaitProbe.run(&frame));
    try std.testing.expectEqual(@as(u64, 0), frame.rdx);
    try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.wakes);

    frame = WakeWaitProbe.reset();
    var duplicate = testFutexValue(1);
    duplicate.thread_index = 10;
    testSetFutexSlot(3, duplicate);

    WakeWaitProbe.stale_id = 9;
    try std.testing.expectEqual(sc.syscall_err_not_ready, WakeWaitProbe.run(&frame));
    try std.testing.expectEqual(@as(u64, 1), frame.rdx);
    try std.testing.expectEqual(@as(usize, 2), WakeWaitProbe.wakes);
    try std.testing.expect(!testFutexValue(@intCast(1)).active and !testFutexValue(@intCast(3)).active);
}

fn futexWakeOpWithWake(comptime wake: anytype, h: anytype, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const first = frame.rdi;
    const second = frame.rsi;
    if (first == 0 or second == 0 or ((first | second) & 3) != 0)
        return 0 -% sc.syscall_err_invalid;
    var bytes: [@sizeOf(runtime_abi.FutexWakeOp)]u8 = undefined;
    // Snapshot before F. No copyout follows the RMW, so a committed update
    // can never be replayed by a pager retry.
    if (!h.copy_user_bytes_from_va(proc, frame.r8, &bytes)) return 0 -% sc.syscall_err_map;
    const operation = std.enums.fromInt(runtime_abi.FutexAtomicOperation,
        std.mem.readInt(u32, bytes[0..4], .little)) orelse return 0 -% sc.syscall_err_invalid;
    const operand = std.mem.readInt(u32, bytes[4..8], .little);
    const comparison = std.enums.fromInt(runtime_abi.FutexComparison,
        std.mem.readInt(u32, bytes[8..12], .little)) orelse return 0 -% sc.syscall_err_invalid;
    const compare_operand = std.mem.readInt(u32, bytes[12..16], .little);
    const HookType = if (@typeInfo(@TypeOf(h)) == .pointer) @typeInfo(@TypeOf(h)).pointer.child else @TypeOf(h);
    if (comptime !@hasField(HookType, "resident_atomic_user_u32_with_address_space_locked")) {
        return 0 -% sc.syscall_err_not_ready;
    } else {
        const atomic = h.resident_atomic_user_u32_with_address_space_locked orelse
            return 0 -% sc.syscall_err_not_ready;
        // WAIT compares and publishes under F -> AS. Holding F through both
        // selections also orders competing WAKE_OP and REQUEUE operations.
        futex_waiters_lock.lock();
        defer futex_waiters_lock.unlock();
        const old = blk: {
            if (!lockFutexUserAddressSpace(h, proc)) return 0 -% sc.syscall_err_map;
            defer unlockFutexUserAddressSpace(h, proc);
            break :blk atomic(proc, second, operation, operand) orelse return 0 -% sc.syscall_err_map;
        };
        const signed_old: i32 = @bitCast(old);
        const signed_compare: i32 = @bitCast(compare_operand);
        const condition = switch (comparison) {
            .equal => old == compare_operand,
            .not_equal => old != compare_operand,
            .signed_less => signed_old < signed_compare,
            .signed_less_equal => signed_old <= signed_compare,
            .signed_greater => signed_old > signed_compare,
            .signed_greater_equal => signed_old >= signed_compare,
        };
        const first_count = futexWakeCollectLocked(wake, proc, first, frame.rdx, null);
        // Equal keys are valid: phase two visits the still-queued remainder.
        return first_count + if (condition) futexWakeCollectLocked(wake, proc, second, frame.r10, null) else @as(u64, 0);
    }
}

fn futexWakeCollect(
    comptime wake: anytype,
    proc: kernel.PrincipalId,
    user_va: u64,
    max_count: u64,
    handoff_target: ?*?kernel.ThreadWakeTarget,
) u64 {
    const validation_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    if (handoff_target) |hint| hint.* = null;
    if (user_va == 0 or (user_va & 0x3) != 0) {
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_stage);
        return sc.syscall_err_invalid;
    }
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_stage);
    if (max_count == 0) {
        if (comptime lock_profile.enabled) {
            lock_profile.stageEnd(.futex_zero_wake, validation_stage);
            lock_profile.futexOutcome(.zero_wake);
        }
        return 0;
    }
    const lock_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    futex_waiters_lock.lock();
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_lock, lock_stage);
    defer futex_waiters_lock.unlock();
    wake_trace.record(.futex_lock, user_va, 0, scheduler.currentThread());
    return futexWakeCollectLocked(wake, proc, user_va, max_count, handoff_target);
}

const WakeOpProbe = struct {
    const owner: kernel.PrincipalId = @enumFromInt(77);
    var descriptor: runtime_abi.FutexWakeOp = undefined;
    var word: u32 = 0;
    var expected_word: u32 = 0;
    var updates: usize = 0;
    var wakes: usize = 0;
    var copy_ok: bool = true;
    var mapping_ok: bool = true;
    var as_locked: bool = false;
    fn copy(_: kernel.PrincipalId, _: u64, output: []u8) bool {
        std.debug.assert(futex_waiters_lock.value == 0);
        if (!copy_ok) return false;
        @memcpy(output, std.mem.asBytes(&descriptor));
        return true;
    }
    fn lockA(_: kernel.PrincipalId) bool {
        std.debug.assert(futex_waiters_lock.value == 1 and !as_locked);
        as_locked = true;
        return true;
    }
    fn unlockA(_: kernel.PrincipalId) void {
        std.debug.assert(futex_waiters_lock.value == 1 and as_locked);
        as_locked = false;
    }
    fn atomic(_: kernel.PrincipalId, _: u64, operation: runtime_abi.FutexAtomicOperation, operand: u32) ?u32 {
        std.debug.assert(futex_waiters_lock.value == 1 and as_locked);
        if (!mapping_ok) return null;
        updates += 1;
        const old = word;
        word = switch (operation) {
            .set => operand, .add => old +% operand, .bit_or => old | operand,
            .and_not => old & ~operand, .bit_xor => old ^ operand,
        };
        // A caller may change its descriptor immediately after copyin.
        descriptor.comparison = 99;
        return old;
    }
    fn wake(_: usize, _: u32, principal: kernel.PrincipalId, _: u64, _: u64) scheduler.KeyedWakeResult {
        std.debug.assert(principal == owner and futex_waiters_lock.value == 1 and !as_locked);
        std.debug.assert(updates == 1 and word == expected_word);
        wakes += 1;
        return .woke;
    }
    fn reset() TrapFrame {
        resetFutexTestStorage();
        word = 0xfffffffe;
        expected_word = 1;
        updates = 0; wakes = 0; copy_ok = true; mapping_ok = true; as_locked = false;
        descriptor = .{ .operation = @intFromEnum(runtime_abi.FutexAtomicOperation.add), .operand = 3,
            .comparison = @intFromEnum(runtime_abi.FutexComparison.not_equal), .compare_operand = 0 };
        for (0..4) |i| testSetFutexSlot(@intCast(i), .{ .active = true, .principal = owner,
            .thread_index = i + 1, .thread_generation = 1, .user_va = if (i < 2) 0x1000 else 0x2000, .wait_token = i + 1 });
        var frame = std.mem.zeroes(TrapFrame);
        frame.rdi = 0x1000; frame.rsi = 0x2000; frame.rdx = 1; frame.r10 = 1; frame.r8 = 0x3000;
        return frame;
    }
    fn run(frame: *TrapFrame) u64 {
        return futexWakeOpWithWake(wake, .{
            .copy_user_bytes_from_va = copy,
            .futex_lock_address_space = @as(?*const fn (kernel.PrincipalId) bool, lockA),
            .futex_unlock_address_space = @as(?*const fn (kernel.PrincipalId) void, unlockA),
            .resident_atomic_user_u32_with_address_space_locked = @as(?*const fn (kernel.PrincipalId, u64, runtime_abi.FutexAtomicOperation, u32) ?u32, atomic),
        }, owner, frame);
    }
};

test "futex wake op validates snapshot and orders word update with both wake phases" {
    defer resetFutexTestStorage();
    for ([_]struct { op: runtime_abi.FutexAtomicOperation, result: u32 }{
        .{ .op = .set, .result = 3 }, .{ .op = .add, .result = 1 }, .{ .op = .bit_or, .result = 0xffffffff },
        .{ .op = .and_not, .result = 0xfffffffc }, .{ .op = .bit_xor, .result = 0xfffffffd },
    }) |case| {
        var frame = WakeOpProbe.reset();
        WakeOpProbe.descriptor.operation = @intFromEnum(case.op);
        WakeOpProbe.expected_word = case.result;
        try std.testing.expectEqual(@as(u64, 2), WakeOpProbe.run(&frame));
        try std.testing.expectEqual(@as(usize, 1), WakeOpProbe.updates);
        try std.testing.expectEqual(@as(usize, 2), WakeOpProbe.wakes);
    }
    // Comparison uses signed OLD value, not the result of the update.
    for ([_]bool{ false, true, true, true, false, false }, 0..) |condition, comparison| {
        var frame = WakeOpProbe.reset();
        WakeOpProbe.descriptor.comparison = @intCast(comparison);
        WakeOpProbe.descriptor.compare_operand = 0xffffffff;
        try std.testing.expectEqual(@as(u64, if (condition) 2 else 1), WakeOpProbe.run(&frame));
    }
    var frame = WakeOpProbe.reset();
    frame.rsi = frame.rdi; frame.r10 = 2;
    testSetFutexSlot(2, .{ .active = true, .principal = WakeOpProbe.owner, .thread_index = 3,
        .thread_generation = 1, .user_va = frame.rdi, .wait_token = 3 });
    try std.testing.expectEqual(@as(u64, 3), WakeOpProbe.run(&frame));
    frame = WakeOpProbe.reset(); frame.rdx = 0; frame.r10 = 0;
    try std.testing.expectEqual(@as(u64, 0), WakeOpProbe.run(&frame));
    try std.testing.expectEqual(@as(u32, 1), WakeOpProbe.word);
    frame = WakeOpProbe.reset(); resetFutexTestStorage();
    try std.testing.expectEqual(@as(u64, 0), WakeOpProbe.run(&frame));
    try std.testing.expectEqual(@as(u32, 1), WakeOpProbe.word);
    for (0..4) |failure| {
        frame = WakeOpProbe.reset();
        switch (failure) {
            0 => WakeOpProbe.copy_ok = false,
            1 => WakeOpProbe.descriptor.operation = 99,
            2 => WakeOpProbe.descriptor.comparison = 99,
            3 => WakeOpProbe.mapping_ok = false,
            else => unreachable,
        }
        const expected = if (failure == 1 or failure == 2) sc.syscall_err_invalid else sc.syscall_err_map;
        try std.testing.expectEqual(0 -% expected, WakeOpProbe.run(&frame));
        try std.testing.expectEqual(@as(usize, 0), WakeOpProbe.updates);
        try std.testing.expectEqual(@as(usize, 0), WakeOpProbe.wakes);
        try std.testing.expect(!WakeOpProbe.as_locked and futex_waiters_lock.value == 0);
        try std.testing.expect(testFutexValue(0).active and testFutexValue(2).active);
    }
}

// Caller owns F. WAKE_OP uses this same selection twice without opening a
// registration/requeue window between its atomic word update and the wakes.
fn futexWakeCollectLocked(
    comptime wake: anytype,
    proc: kernel.PrincipalId,
    user_va: u64,
    max_count: u64,
    handoff_target: ?*?kernel.ThreadWakeTarget,
) u64 {
    if (max_count == 0) return 0;
    const hint_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    const empty = futexKeyKnownEmpty(proc, user_va);
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_hint, hint_stage);
    if (empty) {
        // This second stage is an outcome count. Its inclusive time overlaps
        // the hint; exclusive totals still partition the K interval.
        if (comptime lock_profile.enabled) {
            lock_profile.stageEnd(.futex_empty_wake, hint_stage);
            lock_profile.futexOutcome(.empty_wake);
        }
        return 0;
    }
    const scan_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_scan, scan_stage);
    var woke: u64 = 0;
    var iterator = futexKeyIterator(proc, user_va);
    while (iterator.next()) |waiter| {
        const thread_index = waiter.thread_index;
        const thread_generation = waiter.thread_generation;
        const wait_token = waiter.wait_token;
        const wake_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        const wake_result = wake(
            thread_index,
            thread_generation,
            proc,
            wait_token,
            sc.syscall_ok,
        );
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_wake_dispatch, wake_stage);
        switch (wake_result) {
            .woke => {
                wake_trace.record(.futex_wake_claim, user_va, wait_token, thread_index);
                retainRetiredFutexWaiterLocked(waiter);
                woke += 1;
                if (handoff_target) |hint| hint.* = .{
                    .owner = proc,
                    .thread_index = thread_index,
                    .thread_generation = thread_generation,
                };
                if (max_count != runtime_abi.futex_wake_all and woke >= max_count) break;
            },
            .stale => retireFutexWaiterLocked(waiter),
            .publish_failed => {},
        }
    }
    return woke;
}

const FutexWakeOneResult = struct { count: u64 = 0, handed_off: bool = false };

fn futexWakeOneCollect(comptime wake_and_handoff: anytype, proc: kernel.PrincipalId, user_va: u64, frame: *TrapFrame) FutexWakeOneResult {
    const validation_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    if (user_va == 0 or (user_va & 3) != 0) {
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_stage);
        return .{ .count = sc.syscall_err_invalid };
    }
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_stage);
    const lock_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    futex_waiters_lock.lock();
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_lock, lock_stage);
    defer futex_waiters_lock.unlock();
    wake_trace.record(.futex_lock, user_va, 0, scheduler.currentThread());
    const hint_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    const empty = futexKeyKnownEmpty(proc, user_va);
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_hint, hint_stage);
    if (empty) {
        if (comptime lock_profile.enabled) {
            lock_profile.stageEnd(.futex_empty_wake, hint_stage);
            lock_profile.futexOutcome(.empty_wake);
        }
        return .{};
    }
    const scan_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_scan, scan_stage);
    if (uniqueFutexKeyWaiter(proc, user_va)) |waiter| {
        wake_trace.record(.futex_found, user_va, waiter.wait_token, waiter.thread_index);
        const wake_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        const result = wake_and_handoff(frame, waiter.thread_index, waiter.thread_generation, proc, waiter.wait_token, sc.syscall_ok, 1);
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_wake_dispatch, wake_stage);
        switch (result.wake) {
            .woke => {
                wake_trace.record(.futex_wake_claim, user_va, waiter.wait_token, waiter.thread_index);
                const retired_token = waiter.wait_token;
                retainRetiredFutexWaiterLocked(waiter);
                wake_trace.record(.futex_retired, user_va, retired_token, scheduler.currentThread());
                return .{ .count = 1, .handed_off = result.handed_off };
            },
            .stale => {
                retireFutexWaiterLocked(waiter);
                return .{};
            },
            .publish_failed => return .{},
        }
    }
    var iterator = futexKeyIterator(proc, user_va);
    while (iterator.next()) |waiter| {
        wake_trace.record(.futex_found, user_va, waiter.wait_token, waiter.thread_index);
        const wake_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        const result = wake_and_handoff(frame, waiter.thread_index, waiter.thread_generation, proc, waiter.wait_token, sc.syscall_ok, 1);
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_wake_dispatch, wake_stage);
        switch (result.wake) {
            .woke => {
                wake_trace.record(.futex_wake_claim, user_va, waiter.wait_token, waiter.thread_index);
                // Context installation may have replaced the caller's frame,
                // but the exact waiter remains pinned until this retirement.
                // Never read a new key/count from that receiver frame.
                const retired_token = waiter.wait_token;
                retainRetiredFutexWaiterLocked(waiter);
                wake_trace.record(.futex_retired, user_va, retired_token, scheduler.currentThread());
                return .{ .count = 1, .handed_off = result.handed_off };
            },
            .stale => retireFutexWaiterLocked(waiter),
            .publish_failed => {},
        }
    }
    return .{};
}

fn finishFutexWakeOne(frame: *TrapFrame, result: FutexWakeOneResult, before_leave: ?scheduler.BeforeCurrentThreadLeaveCallback) u64 {
    if (result.handed_off) {
        // TT and futex locks have been released and the waiter retired; the
        // caller's KernelState lock remains held until this one callback.
        if (before_leave) |callback| callback.run(callback.context);
        perf.runtimeAdd(.futex_wake, .reserved, 1);
        return frame.rax;
    }
    return result.count;
}

fn futexWakeAndHandoff(h: anytype, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    if (comptime lock_profile.enabled) lock_profile.futexCall(.wake, @intFromEnum(proc), kernel.principalLabel(proc));
    const user_va = frame.rdi;
    const max_count = frame.rsi;
    wake_trace.record(.futex_wake_enter, user_va, max_count, scheduler.currentThread());
    if (max_count != 1) {
        const start = perf.runtimeTimestamp(.futex_wake);
        const woke = futexWake(proc, user_va, max_count);
        perf.runtimeAdd(.futex_wake, .claim_calls, 1);
        perf.runtimeElapsed(.futex_wake, .claim_cycles, start);
        wake_trace.record(.futex_wake_return, user_va, woke, scheduler.currentThread());
        return woke;
    }
    // The one-wake collector now includes context installation. Its claim
    // interval is not comparable to the old collector-only diagnostic; body
    // below measures only finish/lock-drop callback, not the scheduler switch.
    const collect_start = perf.runtimeTimestamp(.futex_wake);
    const result = futexWakeOneCollect(scheduler.wakeIfWaitingTokenGenerationWithRaxAndHandoff, proc, user_va, frame);
    perf.runtimeAdd(.futex_wake, .claim_calls, 1);
    perf.runtimeElapsed(.futex_wake, .claim_cycles, collect_start);
    perf.runtimeAdd(.futex_wake, if (result.count == 1) .claimed else .no_claim, 1);
    const handoff_start = perf.runtimeTimestamp(.futex_wake);
    const returned = finishFutexWakeOne(frame, result, h.before_current_thread_leave);
    wake_trace.record(.futex_wake_handoff, user_va, @intFromBool(result.handed_off), scheduler.currentThread());
    perf.runtimeAdd(.futex_wake, .body_calls, 1);
    perf.runtimeElapsed(.futex_wake, .body_cycles, handoff_start);
    wake_trace.record(.futex_wake_return, user_va, result.count, scheduler.currentThread());
    return returned;
}

test "futex handoff collector preserves failed publication and exact wake hint" {
    const Probe = struct {
        var calls: usize = 0;
        var fail: bool = true;
        fn wake(_: usize, _: u32, _: kernel.PrincipalId, token: u64, result: u64) scheduler.KeyedWakeResult {
            std.debug.assert(futex_waiters_lock.value == 1 and result == sc.syscall_ok);
            calls += 1;
            if (token == 1) return .stale;
            return if (fail) .publish_failed else .woke;
        }
    };
    // Host tests are serialized; restore the table rather than leaking fake
    // registrations into subsequent runtime tests.
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    resetFutexTestStorage();
    const owner: kernel.PrincipalId = @enumFromInt(7);
    testSetFutexSlot(@intCast(0), .{ .active = true, .principal = @enumFromInt(8), .user_va = 0x1000, .wait_token = 2 });
    testSetFutexSlot(@intCast(1), .{ .active = true, .principal = owner, .user_va = 0x2000, .wait_token = 2 });
    testSetFutexSlot(@intCast(2), .{ .active = true, .principal = owner, .user_va = 0x1000, .wait_token = 1 });
    testSetFutexSlot(@intCast(3), .{ .active = true, .principal = owner, .thread_index = 5, .thread_generation = 17, .user_va = 0x1000, .wait_token = 2 });

    const pending = testFutexValue(@intCast(3));
    var hint: ?kernel.ThreadWakeTarget = null;
    Probe.calls = 0;
    Probe.fail = true;
    try std.testing.expectEqual(@as(u64, 0), futexWakeCollect(Probe.wake, owner, 0x1000, 1, &hint));
    try std.testing.expectEqual(@as(usize, 2), Probe.calls);
    try std.testing.expect(hint == null and !testFutexValue(@intCast(2)).active);
    try std.testing.expectEqualDeep(pending, testFutexValue(@intCast(3)));
    try std.testing.expect(testFutexValue(@intCast(0)).active and testFutexValue(@intCast(1)).active);
    Probe.fail = false;
    try std.testing.expectEqual(@as(u64, 1), futexWakeCollect(Probe.wake, owner, 0x1000, 1, &hint));
    try std.testing.expectEqual(@as(usize, 5), hint.?.thread_index);
    try std.testing.expectEqual(@as(u32, 17), hint.?.thread_generation);
    try std.testing.expectEqual(owner, hint.?.owner);
    try std.testing.expectEqual(@as(u8, 0), futex_waiters_lock.value);
    try std.testing.expectEqual(@as(u64, 0), futexWakeCollect(Probe.wake, owner, 0x1000, 1, &hint));
    try std.testing.expect(hint == null);
    testSetFutexSlot(@intCast(3), pending);
    testSetFutexSlot(@intCast(4), pending);

    try std.testing.expectEqual(@as(u64, 0), futexWakeCollect(Probe.wake, owner, 0x1000, 0, null));
    try std.testing.expectEqual(sc.syscall_err_invalid, futexWakeCollect(Probe.wake, owner, 0x1001, 1, null));
    try std.testing.expectEqual(@as(u64, 2), futexWakeCollect(Probe.wake, owner, 0x1000, runtime_abi.futex_wake_all, null));
}

test "futex grows beyond former limit and replaces above holes" {
    const former_max_futex_waiters = 1024;
    const Probe = struct {
        fn current(_: kernel.PrincipalId, _: usize, _: u32, _: u64) bool {
            return true;
        }
    };
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    const owner: kernel.PrincipalId = @enumFromInt(7);
    for (0..former_max_futex_waiters + 33) |thread| {
        if (!futexHasPreparedFree()) try std.testing.expect(growFutexStorage(.{}));
        try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, thread, 1, 0x1000, thread + 1));
    }
    try std.testing.expectEqual(former_max_futex_waiters + 33, futex_waiters.live);
    clearFutexWaiter(owner, 0, 1, 1);
    try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, former_max_futex_waiters, 2, 0x2000, 6000));
    try std.testing.expectEqual(former_max_futex_waiters + 32, futex_waiters.live);
    try std.testing.expect(futex_waiters.get(0) == null);
    try std.testing.expectEqual(@as(u64, 6000), futex_waiters.get(former_max_futex_waiters).?.wait_token);
}

test "futex retired thread hint cannot redirect a reused slot or generation" {
    const Probe = struct {
        var observed: [4]u64 = .{ 0, 0, 0, 0 };
        var count: usize = 0;
        fn current(_: kernel.PrincipalId, _: usize, _: u32, _: u64) bool {
            return true;
        }
        fn wake(_: usize, _: u32, _: kernel.PrincipalId, token: u64, _: u64) scheduler.KeyedWakeResult {
            observed[count] = token;
            count += 1;
            return .woke;
        }
    };
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    Probe.observed = .{ 0, 0, 0, 0 };
    Probe.count = 0;
    const owner: kernel.PrincipalId = @enumFromInt(7);
    try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, 5, 3, 0x1000, 11));
    try std.testing.expectEqual(@as(u64, 1), futexWakeCollect(Probe.wake, owner, 0x1000, 1, null));
    try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, 5, 3, 0x1000, 12));
    try std.testing.expectEqual(@as(u64, 1), futexWakeCollect(Probe.wake, owner, 0x1000, 1, null));
    // Thread 69 evicts thread 5 from the same direct-mapped cache bucket.
    // Recycling both the table slot and thread index must retain exact keys.
    try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, 69, 1, 0x2000, 21));
    try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, 5, 4, 0x3000, 31));
    try std.testing.expectEqual(@as(u64, 1), futexWakeCollect(Probe.wake, owner, 0x2000, 1, null));
    try std.testing.expectEqual(@as(u64, 1), futexWakeCollect(Probe.wake, owner, 0x3000, 1, null));
    try std.testing.expectEqualSlices(u64, &.{ 11, 12, 21, 31 }, &Probe.observed);
}

test "futex key hint admits one waiter and falls back for shared keys" {
    const Probe = struct {
        fn current(_: kernel.PrincipalId, _: usize, _: u32, _: u64) bool {
            return true;
        }
    };
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    const owner: kernel.PrincipalId = @enumFromInt(71);
    try std.testing.expect(futexKeyKnownEmpty(owner, 0x1000));
    try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, 4, 2, 0x1000, 11));
    const first = uniqueFutexKeyWaiter(owner, 0x1000) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(@as(usize, 4), first.thread_index);
    try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, 5, 2, 0x1000, 12));
    try std.testing.expect(uniqueFutexKeyWaiter(owner, 0x1000) == null);
    try std.testing.expect(!futexKeyKnownEmpty(owner, 0x1000));
    clearFutexWaiter(owner, 4, 2, 11);
    try std.testing.expect(!futexKeyKnownEmpty(owner, 0x1000));
    clearFutexWaiter(owner, 5, 2, 12);
    try std.testing.expect(futexKeyKnownEmpty(owner, 0x1000));
    if (!futexHasPreparedFree()) try std.testing.expect(growFutexStorage(.{}));
    try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, 4, 2, 0x1000, 13));
    try std.testing.expectEqual(@as(u64, 13), uniqueFutexKeyWaiter(owner, 0x1000).?.wait_token);
}

test "futex known-empty hint cannot skip a registered wake" {
    const Probe = struct {
        fn current(_: kernel.PrincipalId, _: usize, _: u32, _: u64) bool { return true; }
        fn wake(_: usize, _: u32, _: kernel.PrincipalId, token: u64, _: u64) scheduler.KeyedWakeResult {
            std.debug.assert(token == 91);
            return .woke;
        }
    };
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    const owner: kernel.PrincipalId = @enumFromInt(72);
    try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, 6, 2, 0x5000, 91));
    try std.testing.expect(!futexKeyKnownEmpty(owner, 0x5000));
    try std.testing.expectEqual(@as(u64, 1), futexWakeCollect(Probe.wake, owner, 0x5000, 1, null));
}

test "futex key index visits only its bucket and wakes in slot order" {
    const Probe = struct {
        var tokens: [4]u64 = undefined;
        var count: usize = 0;
        fn wake(_: usize, _: u32, _: kernel.PrincipalId, token: u64, _: u64) scheduler.KeyedWakeResult {
            if (count < tokens.len) tokens[count] = token;
            count += 1;
            return .woke;
        }
    };
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    const owner: kernel.PrincipalId = @enumFromInt(73);
    // Insert in reverse order to prove selection follows the old directory
    // iterator's slot order, not registration order.
    for ([_]u32{ 401, 301, 201, 101 }) |index|
        testSetFutexSlot(index, .{ .active = true, .principal = owner,
            .thread_index = index, .user_va = 0x8000, .wait_token = index });
    for (0..512) |i| {
        if (i == 101 or i == 201 or i == 301 or i == 401) continue;
        testSetFutexSlot(@intCast(i), .{ .active = true, .principal = owner,
            .thread_index = i + 1000, .user_va = 0x10000 + @as(u64, @intCast(i)) * 4,
            .wait_token = i + 1000 });
    }
    futex_test_key_visits = 0;
    Probe.count = 0;
    try std.testing.expectEqual(@as(u64, 4), futexWakeCollect(Probe.wake, owner, 0x8000, runtime_abi.futex_wake_all, null));
    try std.testing.expectEqualSlices(u64, &.{ 101, 201, 301, 401 }, &Probe.tokens);
    try std.testing.expectEqual(@as(usize, 4), Probe.count);
    try std.testing.expect(futex_test_key_visits >= 4 and futex_test_key_visits < 32);
}

test "futex concurrent registration and removal retain exact key membership" {
    const Worker = struct {
        fn current(_: kernel.PrincipalId, _: usize, _: u32, _: u64) bool { return true; }
        fn run(thread: usize) void {
            const owner: kernel.PrincipalId = @enumFromInt(74);
            const key = 0x20000 + @as(u64, @intCast(thread)) * 4;
            for (0..128) |round| {
                futex_waiters_lock.lock();
                const registered = recordFutexWaiterWithTokenCheckLocked(current, owner, thread, 1, key, round + 1);
                futex_waiters_lock.unlock();
                std.debug.assert(registered);
                clearFutexWaiter(owner, thread, 1, round + 1);
            }
        }
    };
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    // Keep the prepared body alive while workers repeatedly retire their
    // records; otherwise this fixture would require unrelated PMM growth.
    testSetFutexSlot(0, .{ .active = true, .principal = @enumFromInt(75),
        .thread_index = 100, .user_va = 0x30000, .wait_token = 1 });
    var workers: [4]std.Thread = undefined;
    for (&workers, 0..) |*worker, i| worker.* = try std.Thread.spawn(.{}, Worker.run, .{i});
    for (&workers) |*worker| worker.join();
    for (0..4) |i| {
        const key = 0x20000 + @as(u64, @intCast(i)) * 4;
        try std.testing.expect(futexKeyKnownEmpty(@enumFromInt(74), key));
        var iterator = futexKeyIterator(@enumFromInt(74), key);
        try std.testing.expect(iterator.next() == null);
    }
}

test "futex active extent requeue and wake all preserve failed publication" {
    const Probe = struct {
        var fail: bool = true;
        var calls: usize = 0;
        fn current(_: kernel.PrincipalId, _: usize, _: u32, _: u64) bool {
            return true;
        }
        fn wake(_: usize, _: u32, _: kernel.PrincipalId, token: u64, result: u64) scheduler.KeyedWakeResult {
            std.debug.assert(futex_waiters_lock.value == 1 and result == sc.syscall_ok);
            calls += 1;
            return if (fail and token == 8) .publish_failed else .woke;
        }
    };
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    resetFutexTestStorage();

    const owner: kernel.PrincipalId = @enumFromInt(7);
    for (0..8) |thread| {
        if (!futexHasPreparedFree()) try std.testing.expect(growFutexStorage(.{}));
        try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, thread, 3, 0x1000, thread + 1));
    }
    Probe.calls = 0;
    Probe.fail = true;
    try std.testing.expectEqual(@as(u64, 8), futexRequeueWithWake(Probe.wake, owner, 0x1000, 2, 6, 0x2000));
    try std.testing.expectEqual(@as(usize, 2), Probe.calls);
    try std.testing.expectEqual(@as(u64, 0), futexWakeCollect(Probe.wake, owner, 0x1000, runtime_abi.futex_wake_all, null));
    clearFutexWaiter(owner, 6, 3, 7);
    try std.testing.expectEqual(@as(u64, 4), futexWakeCollect(Probe.wake, owner, 0x2000, runtime_abi.futex_wake_all, null));
    try std.testing.expect(testFutexValue(@intCast(7)).active);
    Probe.fail = false;
    try std.testing.expectEqual(@as(u64, 1), futexRequeueWithWake(Probe.wake, owner, 0x2000, 1, 0, 0x3000));
    // Capture a nonempty slice, then retire every record while its bound shrinks.
    for (0..8) |thread| {
        if (!futexHasPreparedFree()) try std.testing.expect(growFutexStorage(.{}));
        try std.testing.expect(recordFutexWaiterWithTokenCheckLocked(Probe.current, owner, thread, 3, 0x1000, thread + 1));
    }
    try std.testing.expectEqual(@as(u64, 8), futexWakeCollect(Probe.wake, owner, 0x1000, runtime_abi.futex_wake_all, null));
    var active: usize = 0;
    var remaining = futex_waiters.iteratorLocked(&futex_directory_leaf);
    while (remaining.next()) |item| {
        if (item.value.active) active += 1;
    }
    try std.testing.expectEqual(@as(usize, 0), active);
    try std.testing.expect(futex_waiters.live <= futex_thread_cache.len);
}

test "futex fused collector retires exact waiter before one leave callback" {
    const Probe = struct {
        var calls: usize = 0;
        var leave_calls: usize = 0;
        var fail: bool = true;
        var switched: bool = true;
        fn wake(frame: *TrapFrame, thread: usize, generation: u32, owner: kernel.PrincipalId, token: u64, resumed_rax: u64, sender: u64) scheduler.KeyedWakeHandoffResult {
            std.debug.assert(futex_waiters_lock.value == 1 and resumed_rax == 0 and sender == 1);
            std.debug.assert(owner == @as(kernel.PrincipalId, @enumFromInt(7)));
            calls += 1;
            if (token == 1) return .{ .wake = .stale };
            std.debug.assert(thread == 5 and generation == 17 and token == 2);
            if (fail) return .{ .wake = .publish_failed };
            if (switched) {
                frame.rax = 123;
                frame.rdi = 0x2000;
                frame.rsi = 999;
            }
            return .{ .wake = .woke, .handed_off = switched };
        }
        fn leave(_: *anyopaque) void {
            std.debug.assert(futex_waiters_lock.value == 0);
            std.debug.assert(!testFutexValue(@intCast(3)).active and testFutexValue(@intCast(4)).active);
            leave_calls += 1;
        }
    };
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    resetFutexTestStorage();
    const owner: kernel.PrincipalId = @enumFromInt(7);
    testSetFutexSlot(@intCast(0), .{ .active = true, .principal = @enumFromInt(8), .user_va = 0x1000, .wait_token = 2 });
    testSetFutexSlot(@intCast(1), .{ .active = true, .principal = owner, .user_va = 0x2000, .wait_token = 2 });
    testSetFutexSlot(@intCast(2), .{ .active = true, .principal = owner, .user_va = 0x1000, .wait_token = 1 });
    testSetFutexSlot(@intCast(3), .{ .active = true, .principal = owner, .thread_index = 5, .thread_generation = 17, .user_va = 0x1000, .wait_token = 2 });
    testSetFutexSlot(@intCast(4), .{ .active = true, .principal = owner, .user_va = 0x2000, .wait_token = 9 });

    const pending = testFutexValue(@intCast(3));
    var frame = std.mem.zeroes(TrapFrame);
    Probe.calls = 0;
    Probe.leave_calls = 0;
    Probe.fail = true;
    Probe.switched = true;
    try std.testing.expectEqualDeep(FutexWakeOneResult{}, futexWakeOneCollect(Probe.wake, owner, 0x1000, &frame));
    try std.testing.expectEqual(@as(usize, 2), Probe.calls);
    try std.testing.expect(!testFutexValue(@intCast(2)).active);
    try std.testing.expectEqualDeep(pending, testFutexValue(@intCast(3)));
    Probe.fail = false;
    const result = futexWakeOneCollect(Probe.wake, owner, 0x1000, &frame);
    try std.testing.expectEqualDeep(FutexWakeOneResult{ .count = 1, .handed_off = true }, result);
    try std.testing.expect(!testFutexValue(@intCast(3)).active and testFutexValue(@intCast(4)).active);
    var context: usize = 0;
    const callback = scheduler.BeforeCurrentThreadLeaveCallback{ .context = &context, .run = Probe.leave };
    try std.testing.expectEqual(@as(u64, 123), finishFutexWakeOne(&frame, result, callback));
    try std.testing.expectEqual(@as(usize, 1), Probe.leave_calls);
    Probe.switched = false;
    testSetFutexSlot(@intCast(3), pending);
    try std.testing.expectEqual(@as(u64, 1), finishFutexWakeOne(&frame, futexWakeOneCollect(Probe.wake, owner, 0x1000, &frame), callback));
    try std.testing.expectEqual(@as(usize, 1), Probe.leave_calls);
    try std.testing.expectEqual(@as(u64, sc.syscall_err_invalid), futexWakeOneCollect(Probe.wake, owner, 0x1001, &frame).count);
    try std.testing.expectEqual(@as(usize, 4), Probe.calls);
}

fn futexRequeue(
    proc: kernel.PrincipalId,
    source_va: u64,
    max_wake: u64,
    max_requeue: u64,
    target_va: u64,
) u64 {
    if (comptime lock_profile.enabled) lock_profile.futexCall(.requeue, @intFromEnum(proc), kernel.principalLabel(proc));
    return futexRequeueWithWake(scheduler.wakeIfWaitingTokenGenerationWithRax, proc, source_va, max_wake, max_requeue, target_va);
}

fn futexRequeueWithWake(
    comptime wake: anytype,
    proc: kernel.PrincipalId,
    source_va: u64,
    max_wake: u64,
    max_requeue: u64,
    target_va: u64,
) u64 {
    const validation_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    if (source_va == 0 or target_va == 0 or
        (source_va & 0x3) != 0 or (target_va & 0x3) != 0 or
        source_va == target_va)
    {
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_stage);
        return sc.syscall_err_invalid;
    }
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_validate, validation_stage);
    if (max_wake == 0 and max_requeue == 0) return 0;

    // Wake and key migration must be one operation with respect to FUTEX_WAIT
    // and FUTEX_WAKE.  The waiter records are kernel-owned, so keeping this
    // lock across both phases provides the atomic requeue primitive that
    // userland cannot reproduce without a lost-wakeup interval.
    const lock_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    futex_waiters_lock.lock();
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_lock, lock_stage);
    defer futex_waiters_lock.unlock();
    const hint_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    const empty = futexKeyKnownEmpty(proc, source_va);
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_hint, hint_stage);
    if (empty) return 0;
    const scan_stage = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.futex_scan, scan_stage);

    var woke: u64 = 0;
    var requeued: u64 = 0;
    var iterator = futexKeyIterator(proc, source_va);
    while (iterator.next()) |waiter| {

        if (woke < max_wake) {
            switch (wake(
                waiter.thread_index,
                waiter.thread_generation,
                proc,
                waiter.wait_token,
                sc.syscall_ok,
            )) {
                .woke => {
                    retainRetiredFutexWaiterLocked(waiter);
                    woke += 1;
                },
                .stale => retireFutexWaiterLocked(waiter),
                .publish_failed => {},
            }
            continue;
        }
        if (requeued < max_requeue) {
            removeFutexKeyHint(waiter);
            waiter.user_va = target_va;
            addFutexKeyHint(waiter);
            requeued += 1;
        }
        if (woke >= max_wake and requeued >= max_requeue) break;
    }
    return woke + requeued;
}

pub fn clearTidAndWake(h: anytype, proc: kernel.PrincipalId, user_va: u64) void {
    if (user_va == 0 or (user_va & 0x3) != 0) return;
    const zero = [_]u8{ 0, 0, 0, 0 };
    if (!h.copy_bytes_to_user_va(proc, user_va, zero[0..])) return;
    _ = futexWake(proc, user_va, 1);
}

fn getRandom(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    // This operation returns a byte count, not a status. Errors must be
    // negative native statuses: +INVALID would be indistinguishable from
    // a successful one-byte read. Linux flag/errno translation stays in LPR.
    const invalid: u64 = @bitCast(-@as(i64, sc.syscall_err_invalid));
    const map_error: u64 = @bitCast(-@as(i64, sc.syscall_err_map));
    const out_va = frame.rdi;
    const len = frame.rsi;
    const flags = frame.rdx;
    if (flags != 0 or len > 4096) return invalid;
    if (len == 0) return 0;
    if (out_va == 0) return map_error;
    const end, const overflow = @addWithOverflow(out_va, len);
    _ = end;
    if (overflow != 0) return map_error;

    var written: u64 = 0;
    var chunk: [64]u8 = undefined;
    while (written < len) {
        const chunk_len: usize = @intCast(@min(len - written, chunk.len));
        state.fillRandomBytes(proc, chunk[0..chunk_len]);
        if (!h.copy_bytes_to_user_va(proc, out_va + written, chunk[0..chunk_len]))
            return if (written != 0) written else map_error;
        written += chunk_len;
    }
    return written;
}

pub fn dispatch(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    return switch (frame.rax) {
        sc.syscall_getpid => scalarIdentity(frame.rax, proc, scheduler.currentThread()).?,
        // Zero is reserved by libc/POSIX thread state for "no thread".  The
        // scheduler uses a zero-based internal slot, so expose a one-based
        // opaque TID rather than leaking slot 0 as an invalid userspace ID.
        sc.syscall_gettid => scalarIdentity(frame.rax, proc, scheduler.currentThread()).?,
        sc.syscall_clock_read => clockReadLocked(frame),
        sc.syscall_system_info => systemInfo(h, proc, frame.rdi),
        sc.syscall_clock_gettime => clockGettime(h, proc, frame.rdi, frame.rsi),
        sc.syscall_clock_getres => clockGetres(h, proc, frame.rdi, frame.rsi),
        sc.syscall_clock_page_open => clockPageOpen(state, proc, h.free_list),
        sc.syscall_yield => yieldThread(scheduler.handoffToBestReadyThreadWithRax, h, frame),
        sc.syscall_nanosleep => nanosleep(h, proc, frame),
        sc.syscall_futex_wait => futexWait(h, proc, frame),
        sc.syscall_futex_wake => futexWakeAndHandoff(h, proc, frame),
        sc.syscall_futex_wake_wait => futexWakeWait(h, proc, frame),
        sc.syscall_futex_wake_op => futexWakeOpWithWake(scheduler.wakeIfWaitingTokenGenerationWithRax, h, proc, frame),
        sc.syscall_futex_requeue => futexRequeue(
            proc,
            frame.rdi,
            frame.rsi,
            frame.rdx,
            frame.r10,
        ),
        sc.syscall_getrandom => getRandom(h, state, proc, frame),
        sc.syscall_power_control => blk: {
            // The boot owner is the sole receiver of authenticated shutdown
            // requests. Linux guests and ordinary native processes cannot
            // issue platform power transitions directly.
            if (!state.isBootstrapOwner(proc)) break :blk sc.syscall_err_invalid;
            const action: acpi_power.Action = switch (frame.rdi) {
                1 => .poweroff,
                2 => .reboot,
                else => break :blk sc.syscall_err_invalid,
            };
            break :blk if (acpi_power.execute(action)) sc.syscall_ok else sc.syscall_err_not_ready;
        },
        else => null,
    };
}

test "lock ordering futex actual lock constrains IPI waiting" {
    if (!lock_diagnostics.enabled) return;
    var lock_state: FutexSpinLock = .{};
    lock_state.lock();
    try std.testing.expectEqual(lock_diagnostics.order.Class.futex, lock_diagnostics.testCpu().head.?.key.class);
    try std.testing.expectError(error.IpiWaitWithBlockingLock, lock_diagnostics.testCpu().assertCanWaitForIpi());
    lock_state.unlock();
    lock_diagnostics.assertRootEmpty();
}

test "contended futex lock acknowledges a pending IPI before its owner releases" {
    const Worker = struct {
        fn run(lock_state: *FutexSpinLock) void {
            lock_state.lock();
            lock_state.unlock();
        }
    };
    var lock_state: FutexSpinLock = .{};
    var acknowledged: std.atomic.Value(bool) = .init(false);
    futex_test_interrupt_ack = &acknowledged;
    defer futex_test_interrupt_ack = null;
    lock_state.lock();
    const worker = try std.Thread.spawn(.{}, Worker.run, .{&lock_state});
    var spins: usize = 0;
    while (!acknowledged.load(.acquire) and spins < 50_000_000) : (spins += 1)
        std.atomic.spinLoopHint();
    lock_state.unlock();
    worker.join();
    // Without the contender's interrupt window, the simulated shootdown
    // cannot be acknowledged while the owner still holds F.
    try std.testing.expect(acknowledged.load(.acquire));
}

test "uninterruptible wait invalid futex flags have no outbound or copy side effects" {
    resetFutexTestStorage();
    defer resetFutexTestStorage();
    var frame = WakeWaitProbe.reset();
    frame.r9 = 1;
    try std.testing.expectEqual(sc.syscall_err_invalid, WakeWaitProbe.run(&frame));
    try std.testing.expectEqual(std.math.maxInt(u64), frame.rdx);
    try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.blocks);
    try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.copies);
    try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.wakes);
    try std.testing.expectEqual(@as(usize, 0), WakeWaitProbe.reserves);
}

// Host tests enter the actual frontend without compiling unrelated dispatch arms.
pub const WaitTestSupport = if (@import("builtin").is_test) struct {
    pub const wait = futexWait;
    pub fn wakeWait(h: anytype, p: kernel.PrincipalId, f: *TrapFrame) u64 {
        return futexWakeWaitWithOps(WakeWaitScheduler, h, p, f);
    }
    pub fn reset() void {
        resetFutexTestStorage();
    }
} else struct {};
