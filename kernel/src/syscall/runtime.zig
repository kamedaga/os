const std = @import("std");
const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const rtc = @import("../rtc.zig");
const scheduler = @import("../scheduler.zig").connection;
const smp = @import("../smp.zig");
const sc = @import("numbers.zig");

const runtime_abi = abi_root.runtime_abi;
const TrapFrame = interrupts.TrapFrame;

const max_futex_waiters = 256;

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
    return end;
}

fn readUserU32(h: anytype, proc: kernel.PrincipalId, user_va: u64) ?u32 {
    var bytes: [4]u8 = undefined;
    if (!h.copy_user_bytes_from_va(proc, user_va, bytes[0..])) return null;
    return std.mem.readInt(u32, &bytes, .little);
}

fn writeTimespec(h: anytype, proc: kernel.PrincipalId, out_va: u64, sec: u64, nsec: u64) u64 {
    if (out_va == 0) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + runtime_abi.timespec_sec_offset, sec)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + runtime_abi.timespec_nsec_offset, nsec)) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn monotonicTicksAsTimespec() struct { sec: u64, nsec: u64 } {
    const tick = scheduler.lapic_tick_count;
    return .{
        .sec = tick / 1000,
        .nsec = (tick % 1000) * 1_000_000,
    };
}

fn clockGettime(h: anytype, proc: kernel.PrincipalId, clock_id: u64, out_va: u64) u64 {
    return switch (clock_id) {
        runtime_abi.clock_realtime => writeTimespec(h, proc, out_va, rtc.unixTimeSeconds(), 0),
        runtime_abi.clock_monotonic => blk: {
            const ts = monotonicTicksAsTimespec();
            break :blk writeTimespec(h, proc, out_va, ts.sec, ts.nsec);
        },
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

fn recordFutexWaiter(
    proc: kernel.PrincipalId,
    thread_index: usize,
    thread_generation: u32,
    user_va: u64,
    wait_token: u64,
) bool {
    futex_waiters_lock.lock();
    defer futex_waiters_lock.unlock();
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
    user_va: u64,
    wait_token: u64,
) void {
    futex_waiters_lock.lock();
    defer futex_waiters_lock.unlock();
    for (futex_waiters[0..]) |*waiter| {
        if (!waiter.active) continue;
        if (waiter.principal != proc or
            waiter.thread_index != thread_index or
            waiter.thread_generation != thread_generation or
            waiter.user_va != user_va or
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
    const current = readUserU32(h, proc, user_va) orelse return sc.syscall_err_invalid;
    if (current != expected) return sc.syscall_err_not_ready;

    const thread_index = scheduler.currentThread();
    const thread_generation = scheduler.generationOfThread(thread_index) orelse return sc.syscall_err_invalid;
    const wait_token = scheduler.reserveCurrentWaitToken(thread_generation) orelse return sc.syscall_err_invalid;
    if (!recordFutexWaiter(proc, thread_index, thread_generation, user_va, wait_token)) {
        scheduler.cancelCurrentWaitToken(thread_generation, wait_token);
        return sc.syscall_err_alloc;
    }
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
    clearFutexWaiter(proc, thread_index, thread_generation, user_va, wait_token);
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

pub fn clearTidAndWake(h: anytype, proc: kernel.PrincipalId, user_va: u64) void {
    if (user_va == 0 or (user_va & 0x3) != 0) return;
    const zero = [_]u8{ 0, 0, 0, 0 };
    if (!h.copy_bytes_to_user_va(proc, user_va, zero[0..])) return;
    _ = futexWake(proc, user_va, 1);
}

fn getRandom(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const out_va = frame.rdi;
    const len = frame.rsi;
    const flags = frame.rdx;
    if (flags != 0) return sc.syscall_err_invalid;
    if (len == 0) return 0;
    if (out_va == 0 or len > 4096) return sc.syscall_err_invalid;

    var written: u64 = 0;
    var chunk: [64]u8 = undefined;
    while (written < len) {
        const chunk_len: usize = @intCast(@min(len - written, chunk.len));
        state.fillRandomBytes(proc, chunk[0..chunk_len]);
        if (!h.copy_bytes_to_user_va(proc, out_va + written, chunk[0..chunk_len])) return sc.syscall_err_invalid;
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
        sc.syscall_nanosleep => nanosleep(h, proc, frame),
        sc.syscall_futex_wait => futexWait(h, proc, frame),
        sc.syscall_futex_wake => futexWake(proc, frame.rdi, frame.rsi),
        sc.syscall_getrandom => getRandom(h, state, proc, frame),
        else => null,
    };
}
