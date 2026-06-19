const std = @import("std");
const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const rtc = @import("../rtc.zig");
const scheduler = @import("../scheduler.zig");
const sc = @import("numbers.zig");

const runtime_abi = abi_root.runtime_abi;
const TrapFrame = interrupts.TrapFrame;

const max_futex_waiters = 256;

const FutexWaiter = struct {
    active: bool = false,
    principal: kernel.PrincipalId = @enumFromInt(0),
    thread_index: usize = 0,
    user_va: u64 = 0,
};

var futex_waiters: [max_futex_waiters]FutexWaiter = [_]FutexWaiter{.{}} ** max_futex_waiters;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(futex_waiters), &futex_waiters));
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
    if (h.block_current_thread_for_event(frame, false, ticks, sc.syscall_ok)) return frame.rax;
    return sc.syscall_err_not_ready;
}

fn recordFutexWaiter(proc: kernel.PrincipalId, thread_index: usize, user_va: u64) bool {
    for (futex_waiters[0..]) |*waiter| {
        if (waiter.active and waiter.principal == proc and waiter.thread_index == thread_index) {
            waiter.user_va = user_va;
            return true;
        }
    }
    for (futex_waiters[0..]) |*waiter| {
        if (waiter.active) continue;
        waiter.* = .{
            .active = true,
            .principal = proc,
            .thread_index = thread_index,
            .user_va = user_va,
        };
        return true;
    }
    return false;
}

fn clearFutexWaiter(proc: kernel.PrincipalId, thread_index: usize, user_va: u64) void {
    for (futex_waiters[0..]) |*waiter| {
        if (!waiter.active) continue;
        if (waiter.principal != proc or waiter.thread_index != thread_index or waiter.user_va != user_va) continue;
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

    const thread_index = scheduler.currentThreadIndex();
    if (!recordFutexWaiter(proc, thread_index, user_va)) return sc.syscall_err_alloc;
    if (h.block_current_thread_for_event(frame, true, timeout_ticks, sc.syscall_err_not_ready)) return frame.rax;
    clearFutexWaiter(proc, thread_index, user_va);
    return sc.syscall_err_not_ready;
}

fn futexWake(proc: kernel.PrincipalId, user_va: u64, max_count: u64) u64 {
    if (user_va == 0 or (user_va & 0x3) != 0) return sc.syscall_err_invalid;
    var woke: u64 = 0;
    for (futex_waiters[0..]) |*waiter| {
        if (!waiter.active) continue;
        if (waiter.principal != proc or waiter.user_va != user_va) continue;
        const thread_index = waiter.thread_index;
        waiter.* = .{};
        scheduler.wakeThreadIfWaiting(thread_index);
        woke += 1;
        if (max_count != runtime_abi.futex_wake_all and woke >= max_count) break;
    }
    return woke;
}

pub fn dispatch(h: anytype, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    return switch (frame.rax) {
        sc.syscall_getpid => @intFromEnum(proc),
        sc.syscall_gettid => @as(u64, @intCast(scheduler.currentThreadIndex())),
        sc.syscall_clock_gettime => clockGettime(h, proc, frame.rdi, frame.rsi),
        sc.syscall_nanosleep => nanosleep(h, proc, frame),
        sc.syscall_futex_wait => futexWait(h, proc, frame),
        sc.syscall_futex_wake => futexWake(proc, frame.rdi, frame.rsi),
        else => null,
    };
}
