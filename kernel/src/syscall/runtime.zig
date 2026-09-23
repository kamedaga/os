const std = @import("std");
const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const realtime_clock = @import("../realtime_clock.zig");
const scheduler = @import("../scheduler.zig").connection;
const smp = @import("../smp.zig");
const sc = @import("numbers.zig");
const perf = @import("../smp_perf.zig");

const runtime_abi = abi_root.runtime_abi;
const TrapFrame = interrupts.TrapFrame;

// Shared by every process, including native GPU workers and Linux threads.
// A multi-process browser can exceed 256 live waits during tab restoration.
// Keep the table bounded; stale-token reclamation still runs before failure.
const max_futex_waiters = 1024;

const FutexSpinLock = struct {
    value: u8 = 0,

    fn lock(self: *FutexSpinLock) void {
        while (true) {
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) return;
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                asm volatile ("pause");
            }
        }
    }

    fn unlock(self: *FutexSpinLock) void {
        @atomicStore(u8, &self.value, 0, .release);
    }
};

const FutexWaiter = struct {
    active: bool = false,
    principal: kernel.PrincipalId = @enumFromInt(0),
    thread_index: usize = 0,
    thread_generation: u32 = 0,
    user_va: u64 = 0,
    wait_token: u64 = 0,
};

var futex_waiters: [max_futex_waiters]FutexWaiter = [_]FutexWaiter{.{}} ** max_futex_waiters;
var futex_waiters_lock: FutexSpinLock = .{};

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
    end = maxStaticEnd(end, realtime_clock.kernelStaticStorageEndAddr());
    return end;
}

fn readUserU32(h: anytype, proc: kernel.PrincipalId, user_va: u64) ?u32 {
    var bytes: [4]u8 = undefined;
    if (!h.copy_user_bytes_from_va(proc, user_va, bytes[0..])) return null;
    return std.mem.readInt(u32, &bytes, .little);
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

fn systemInfo(h: anytype, proc: kernel.PrincipalId, out_va: u64) u64 {
    if (out_va == 0) return sc.syscall_err_invalid;
    const free_pages: u64 = @intCast(h.free_list.pageCount());
    const free_bytes, const free_overflow = @mulWithOverflow(free_pages, 4096);
    if (free_overflow != 0) return sc.syscall_err_invalid;
    var online_cpu_count: u64 = @popCount(smp.onlineCpuMask());
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
    if (h.block_current_thread_for_event(frame, false, 0, ticks, sc.syscall_ok, h.before_current_thread_leave)) return frame.rax;
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
    try std.testing.expect(@import("lock_policy.zig").needsKernelStateLock(sc.syscall_yield));
}

fn reclaimStaleFutexWaitersLocked() void {
    for (futex_waiters[0..]) |*waiter| {
        if (!waiter.active) continue;
        if (scheduler.waitTokenIsCurrent(
            waiter.principal,
            waiter.thread_index,
            waiter.thread_generation,
            waiter.wait_token,
        )) continue;
        waiter.* = .{};
    }
}

fn recordFutexWaiterLocked(
    proc: kernel.PrincipalId,
    thread_index: usize,
    thread_generation: u32,
    user_va: u64,
    wait_token: u64,
) bool {
    for (futex_waiters[0..]) |*waiter| {
        if (waiter.active and waiter.principal == proc and waiter.thread_index == thread_index) {
            waiter.* = .{
                .active = true,
                .principal = proc,
                .thread_index = thread_index,
                .thread_generation = thread_generation,
                .user_va = user_va,
                .wait_token = wait_token,
            };
            return true;
        }
    }
    if (recordFutexWaiterInFreeSlot(proc, thread_index, thread_generation, user_va, wait_token)) return true;
    reclaimStaleFutexWaitersLocked();
    return recordFutexWaiterInFreeSlot(proc, thread_index, thread_generation, user_va, wait_token);
}

fn recordFutexWaiterInFreeSlot(
    proc: kernel.PrincipalId,
    thread_index: usize,
    thread_generation: u32,
    user_va: u64,
    wait_token: u64,
) bool {
    for (futex_waiters[0..]) |*waiter| {
        if (waiter.active) continue;
        waiter.* = .{
            .active = true,
            .principal = proc,
            .thread_index = thread_index,
            .thread_generation = thread_generation,
            .user_va = user_va,
            .wait_token = wait_token,
        };
        return true;
    }
    return false;
}

fn clearFutexWaiter(
    proc: kernel.PrincipalId,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
) void {
    futex_waiters_lock.lock();
    defer futex_waiters_lock.unlock();
    for (futex_waiters[0..]) |*waiter| {
        if (!waiter.active) continue;
        if (waiter.principal != proc or
            waiter.thread_index != thread_index or
            waiter.thread_generation != thread_generation or
            waiter.wait_token != wait_token) continue;
        waiter.* = .{};
        return;
    }
}

fn futexWait(h: anytype, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const user_va = frame.rdi;
    const expected: u32 = @truncate(frame.rsi);
    const timeout_ticks = frame.rdx;
    if (user_va == 0 or (user_va & 0x3) != 0) return sc.syscall_err_invalid;

    const thread_index = scheduler.currentThread();
    const thread_generation = scheduler.generationOfThread(thread_index) orelse return sc.syscall_err_invalid;

    // FUTEX_WAIT must compare the userspace word and enqueue the waiter
    // atomically with respect to FUTEX_WAKE.  A wake performs its userspace
    // store before entering futexWake(), then takes this same lock.  Holding
    // the lock across the comparison and waiter publication therefore gives
    // the two operations a single order: the waiter either observes the new
    // value or the subsequent wake observes the waiter.
    futex_waiters_lock.lock();
    const current = readUserU32(h, proc, user_va) orelse {
        futex_waiters_lock.unlock();
        return sc.syscall_err_invalid;
    };
    if (current != expected) {
        futex_waiters_lock.unlock();
        return sc.syscall_err_not_ready;
    }
    const wait_token = scheduler.reserveCurrentWaitToken(thread_generation) orelse {
        futex_waiters_lock.unlock();
        return sc.syscall_err_invalid;
    };
    if (!recordFutexWaiterLocked(proc, thread_index, thread_generation, user_va, wait_token)) {
        futex_waiters_lock.unlock();
        scheduler.cancelCurrentWaitToken(thread_generation, wait_token);
        return sc.syscall_err_alloc;
    }
    futex_waiters_lock.unlock();

    const timeout_result = if (timeout_ticks == 0)
        sc.syscall_err_not_ready
    else
        runtime_abi.futex_wait_timed_out_status;
    if (h.block_current_thread_for_event(
        frame,
        true,
        wait_token,
        timeout_ticks,
        timeout_result,
        h.before_current_thread_leave,
    )) return frame.rax;
    scheduler.cancelCurrentWaitToken(thread_generation, wait_token);
    // FUTEX_REQUEUE may have changed this waiter's key while it slept.  The
    // reserved token uniquely identifies the wait, so cleanup must not depend
    // on the original userspace address.
    clearFutexWaiter(proc, thread_index, thread_generation, wait_token);
    return sc.syscall_err_not_ready;
}

fn futexWake(proc: kernel.PrincipalId, user_va: u64, max_count: u64) u64 {
    if (user_va == 0 or (user_va & 0x3) != 0) return sc.syscall_err_invalid;
    if (max_count == 0) return 0;
    futex_waiters_lock.lock();
    defer futex_waiters_lock.unlock();
    var woke: u64 = 0;
    for (futex_waiters[0..]) |*waiter| {
        if (!waiter.active) continue;
        if (waiter.principal != proc or waiter.user_va != user_va) continue;
        const thread_index = waiter.thread_index;
        const thread_generation = waiter.thread_generation;
        const wait_token = waiter.wait_token;
        switch (scheduler.wakeIfWaitingTokenGenerationWithRax(
            thread_index,
            thread_generation,
            proc,
            wait_token,
            sc.syscall_ok,
        )) {
            .woke => {
                waiter.* = .{};
                woke += 1;
                if (max_count != runtime_abi.futex_wake_all and woke >= max_count) break;
            },
            .stale => waiter.* = .{},
            .publish_failed => {},
        }
    }
    return woke;
}

fn futexRequeue(
    proc: kernel.PrincipalId,
    source_va: u64,
    max_wake: u64,
    max_requeue: u64,
    target_va: u64,
) u64 {
    if (source_va == 0 or target_va == 0 or
        (source_va & 0x3) != 0 or (target_va & 0x3) != 0 or
        source_va == target_va)
    {
        return sc.syscall_err_invalid;
    }
    if (max_wake == 0 and max_requeue == 0) return 0;

    // Wake and key migration must be one operation with respect to FUTEX_WAIT
    // and FUTEX_WAKE.  The waiter records are kernel-owned, so keeping this
    // lock across both phases provides the atomic requeue primitive that
    // userland cannot reproduce without a lost-wakeup interval.
    futex_waiters_lock.lock();
    defer futex_waiters_lock.unlock();

    var woke: u64 = 0;
    var requeued: u64 = 0;
    for (futex_waiters[0..]) |*waiter| {
        if (!waiter.active) continue;
        if (waiter.principal != proc or waiter.user_va != source_va) continue;

        if (woke < max_wake) {
            switch (scheduler.wakeIfWaitingTokenGenerationWithRax(
                waiter.thread_index,
                waiter.thread_generation,
                proc,
                waiter.wait_token,
                sc.syscall_ok,
            )) {
                .woke => {
                    waiter.* = .{};
                    woke += 1;
                },
                .stale => waiter.* = .{},
                .publish_failed => {},
            }
            continue;
        }
        if (requeued < max_requeue) {
            waiter.user_va = target_va;
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
        sc.syscall_getpid => @intFromEnum(proc),
        // Zero is reserved by libc/POSIX thread state for "no thread".  The
        // scheduler uses a zero-based internal slot, so expose a one-based
        // opaque TID rather than leaking slot 0 as an invalid userspace ID.
        sc.syscall_gettid => @as(u64, @intCast(scheduler.currentThread())) + 1,
        sc.syscall_system_info => systemInfo(h, proc, frame.rdi),
        sc.syscall_clock_gettime => clockGettime(h, proc, frame.rdi, frame.rsi),
        sc.syscall_clock_getres => clockGetres(h, proc, frame.rdi, frame.rsi),
        sc.syscall_yield => yieldThread(scheduler.handoffToBestReadyThreadWithRax, h, frame),
        sc.syscall_nanosleep => nanosleep(h, proc, frame),
        sc.syscall_futex_wait => futexWait(h, proc, frame),
        sc.syscall_futex_wake => futexWake(proc, frame.rdi, frame.rsi),
        sc.syscall_futex_requeue => futexRequeue(
            proc,
            frame.rdi,
            frame.rsi,
            frame.rdx,
            frame.r10,
        ),
        sc.syscall_getrandom => getRandom(h, state, proc, frame),
        else => null,
    };
}
