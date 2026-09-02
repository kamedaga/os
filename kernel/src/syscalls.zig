const std = @import("std");
const kernel = @import("kernel.zig");
const interrupts = @import("interrupts.zig");
const scheduler = @import("scheduler.zig").connection;
const boot_static = @import("boot/main_static.zig");
const sc = @import("syscall/numbers.zig");
const syscall_lock_policy = @import("syscall/lock_policy.zig");
const capsule_syscalls = @import("syscall/capsule.zig");
const fd_syscalls = @import("syscall/fd.zig");
const native_ipc_syscalls = @import("syscall/native_ipc.zig");
const process_syscalls = @import("syscall/process.zig");
const runtime_syscalls = @import("syscall/runtime.zig");

const TrapFrame = interrupts.TrapFrame;

pub const Hooks = struct {
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    user_spaces: *boot_static.UserAddressSpaceTable,
    kernel_state_ready: *const bool,
    write: *const fn ([]const u8) void,
    print_hex: *const fn (u64) void,
    print_number: *const fn (u64) void,
    principal_label: *const fn (kernel.PrincipalId) []const u8,
    read_user_u64: *const fn (kernel.PrincipalId, u64) ?u64,
    write_user_u64: *const fn (kernel.PrincipalId, u64, u64) bool,
    copy_user_bytes_from_va: *const fn (kernel.PrincipalId, u64, []u8) bool,
    copy_bytes_to_user_va: *const fn (kernel.PrincipalId, u64, []const u8) bool,
    wake_waiting_thread_for_principal: *const fn (kernel.PrincipalId) void,
    thread_wake_target_is_live: *const fn (usize, u32, kernel.PrincipalId) bool,
    wake_waiting_thread_generation: *const fn (usize, u32) bool,
    wake_waiting_thread_generation_with_rax: *const fn (usize, u32, u64) bool,
    wake_waiting_thread_generation_with_rax_prefer_current: *const fn (usize, u32, u64) bool,
    wake_blocked_thread_for_principal: *const fn (kernel.PrincipalId) void,
    switch_to_thread: *const fn (usize, *TrapFrame, ?u64) bool,
    before_current_thread_leave: ?scheduler.BeforeCurrentThreadLeaveCallback = null,
    reacquire_kernel_state_lock: ?scheduler.BeforeCurrentThreadLeaveCallback = null,
    block_current_thread_for_event: *const fn (*TrapFrame, bool, u64, u64, u64, ?scheduler.BeforeCurrentThreadLeaveCallback) bool,
    exit_current_process: *const fn (kernel.PrincipalId, *TrapFrame, ?scheduler.BeforeCurrentThreadLeaveCallback, ?scheduler.BeforeCurrentThreadLeaveCallback) void,
    total_usable_memory_bytes: u64,
};

var syscall_hooks_storage: Hooks = undefined;
var syscall_hooks_ready = false;

const KernelStateSpinLock = struct {
    value: u8 = 0,

    fn waitWithInterruptWindow() void {
        // Syscalls enter with IF clear.  A CPU waiting for the global state
        // lock must still be able to acknowledge IPIs (notably a TLB
        // shootdown issued by the lock owner).
        asm volatile ("sti; pause; cli" ::: .{ .memory = true });
    }

    fn lock(self: *KernelStateSpinLock) void {
        while (true) {
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) return;
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                waitWithInterruptWindow();
            }
        }
    }

    fn tryLock(self: *KernelStateSpinLock) bool {
        return @cmpxchgStrong(u8, &self.value, 0, 1, .acquire, .monotonic) == null;
    }

    fn unlock(self: *KernelStateSpinLock) void {
        @atomicStore(u8, &self.value, 0, .release);
    }
};

var kernel_state_lock: KernelStateSpinLock = .{};

pub const ExternalPrincipalTeardownAcquire = enum {
    acquired,
    handled_by_existing_lifecycle,
};

/// Enter the same lifecycle/state-lock exclusion used by PROCESS_EXIT when a
/// fatal user exception or inactive-principal fallback starts outside syscall
/// dispatch. On success the lifecycle gate and KernelState lock remain held.
pub fn beginExternalPrincipalTeardown(
    principal: kernel.PrincipalId,
) ExternalPrincipalTeardownAcquire {
    while (!scheduler.tryBeginPrincipalLifecycle(principal)) {
        if (scheduler.principalLifecycleTargets(principal)) {
            return .handled_by_existing_lifecycle;
        }
        KernelStateSpinLock.waitWithInterruptWindow();
    }

    kernel_state_lock.lock();
    while (true) {
        _ = scheduler.stopPrincipalThreads(principal);
        kernel_state_lock.unlock();
        scheduler.waitForRemotePrincipalQuiescence(principal);
        kernel_state_lock.lock();

        // A syscall or THREAD_CREATE admitted before the lifecycle gate may
        // have finished while the lock was dropped. Stop its result and scan
        // CPU ownership again before permitting teardown.
        _ = scheduler.stopPrincipalThreads(principal);
        const current_cpu = scheduler.currentCpu();
        const ignored_mask = if (current_cpu < 64)
            @as(u64, 1) << @intCast(current_cpu)
        else
            0;
        if ((scheduler.cpuMaskRunningPrincipal(principal) & ~ignored_mask) == 0) {
            return .acquired;
        }
    }
}

pub fn endExternalPrincipalTeardown() void {
    scheduler.endPrincipalLifecycle();
    kernel_state_lock.unlock();
}

const KernelStateLockDropContext = struct {
    lock_held: *bool,
};

fn dropKernelStateLockBeforeCurrentThreadLeave(ctx_ptr: *anyopaque) void {
    const ctx: *KernelStateLockDropContext = @ptrCast(@alignCast(ctx_ptr));
    if (!ctx.lock_held.*) return;
    kernel_state_lock.unlock();
    ctx.lock_held.* = false;
}

fn reacquireKernelStateLockAfterRemoteQuiescence(ctx_ptr: *anyopaque) void {
    const ctx: *KernelStateLockDropContext = @ptrCast(@alignCast(ctx_ptr));
    if (ctx.lock_held.*) return;
    kernel_state_lock.lock();
    ctx.lock_held.* = true;
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_hooks_storage), &syscall_hooks_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_hooks_ready), &syscall_hooks_ready));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_state_lock), &kernel_state_lock));
    end = maxStaticEnd(end, runtime_syscalls.kernelStaticStorageEndAddr());
    return end;
}

pub fn init(new_hooks: Hooks) void {
    syscall_hooks_storage = new_hooks;
    syscall_hooks_ready = true;
}

pub fn updateUserSpaces(user_spaces: *boot_static.UserAddressSpaceTable) void {
    if (!syscall_hooks_ready) return;
    syscall_hooks_storage.user_spaces = user_spaces;
}

fn getHooks() *const Hooks {
    if (!syscall_hooks_ready) unreachable;
    return &syscall_hooks_storage;
}

/// IRQ publication itself is lock-free, but completing an FD wait mutates its
/// generational wait group and must use the same KernelState lock as syscalls.
/// Interrupt context must never spin on a lock held by the interrupted CPU;
/// the BSP timer retries any publication that loses this try-lock race.
pub fn completePendingIrqFdWaitersFromInterrupt() void {
    if (!syscall_hooks_ready) return;
    const h = getHooks();
    if (!h.kernel_state_ready.*) return;
    if (!kernel_state_lock.tryLock()) return;
    defer kernel_state_lock.unlock();

    var targets: [kernel.fd_table_entries]kernel.ThreadWakeTarget = undefined;
    const count = h.state.takeReadyIrqWaiters(targets[0..]);
    _ = fd_syscalls.wakeThreadTargets(h, h.state, targets[0..count]);
}

fn hasExplicitUserLogLabel(message: []const u8) bool {
    if (message.len < 3 or message[0] != '[') return false;
    const close_index = std.mem.indexOfScalar(u8, message, ']') orelse return false;
    if (close_index <= 1) return false;
    return close_index + 1 == message.len or message[close_index + 1] == ' ';
}

fn writeThreadUserLogPrefix(h: *const Hooks, thread_index: usize) void {
    h.write("[Thread ");
    h.print_number(@intCast(thread_index));
    h.write("] ");
}

fn dispatchCompactSyscall(frame: *TrapFrame) u64 {
    const base_hooks = getHooks();
    if (!base_hooks.kernel_state_ready.*) return sc.syscall_err_not_ready;

    const state = base_hooks.state;
    const proc = scheduler.currentPrincipal();
    // Recorded on entry, not at deschedule: the trap frame's rax carries the
    // syscall number in and the result out, so by the time a thread is placed
    // again the number is gone.  rdi is kept alongside because it is the fd or
    // futex address the spin is going around.
    const hold_kernel_state_lock = syscall_lock_policy.needsKernelStateLock(frame.rax);
    var lock_held = false;
    var lock_drop_context = KernelStateLockDropContext{ .lock_held = &lock_held };
    var hooks = base_hooks.*;
    hooks.before_current_thread_leave = if (hold_kernel_state_lock)
        scheduler.BeforeCurrentThreadLeaveCallback{
            .context = &lock_drop_context,
            .run = dropKernelStateLockBeforeCurrentThreadLeave,
        }
    else
        null;
    hooks.reacquire_kernel_state_lock = if (hold_kernel_state_lock)
        scheduler.BeforeCurrentThreadLeaveCallback{
            .context = &lock_drop_context,
            .run = reacquireKernelStateLockAfterRemoteQuiescence,
        }
    else
        null;
    const h = &hooks;

    if (hold_kernel_state_lock) {
        kernel_state_lock.lock();
        lock_held = true;
    }
    defer {
        if (lock_held) kernel_state_lock.unlock();
    }

    if (!state.hasActivePrincipal(proc)) {
        base_hooks.write("syscall from inactive principal proc=");
        base_hooks.write(base_hooks.principal_label(proc));
        base_hooks.write(" thread=");
        base_hooks.print_number(@intCast(scheduler.currentThread()));
        base_hooks.write("\n");
        if (lock_held) {
            kernel_state_lock.unlock();
            lock_held = false;
        }
        base_hooks.exit_current_process(proc, frame, null, null);
        return frame.rax;
    }

    // A remote lifecycle owner may temporarily drop the global state lock to
    // drive this principal through an interrupt/syscall boundary. Do not admit
    // another target syscall into that window; the owner will rescan contexts
    // before teardown or publish the final stopped state.
    if (scheduler.principalLifecycleBlocksAdmission(proc)) {
        return sc.syscall_err_not_ready;
    }

    // Timeout/signal completion invalidates the scheduler token without
    // traversing KernelState.  Reclaim that thread's exact generational wait
    // group at its next stateful syscall before any user poll array can be
    // reused for an unrelated FD.
    if (hold_kernel_state_lock) {
        const thread_index = scheduler.currentThread();
        if (scheduler.generationOfThread(thread_index)) |thread_generation| {
            state.cancelFdWaitGroupsForThread(proc, thread_index, thread_generation);
        }
    }

    if (process_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (capsule_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (fd_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (native_ipc_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (runtime_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }

    return switch (frame.rax) {
        sc.syscall_log => blk: {
            const req_len_u64 = frame.rsi;
            if (req_len_u64 == 0) break :blk sc.syscall_ok;
            if (req_len_u64 > sc.user_log_max_bytes) break :blk sc.syscall_err_invalid;
            const req_len: usize = @intCast(req_len_u64);
            var buf: [sc.user_log_max_bytes]u8 = undefined;
            const msg = buf[0..req_len];
            if (!h.copy_user_bytes_from_va(proc, frame.rdi, msg)) break :blk sc.syscall_err_invalid;
            if (!hasExplicitUserLogLabel(msg)) {
                writeThreadUserLogPrefix(h, scheduler.currentThread());
            }
            h.write(msg);
            break :blk sc.syscall_ok;
        },
        else => sc.syscall_err_invalid,
    };
}

pub export fn syscallDispatch(frame: *TrapFrame) callconv(.winapi) u64 {
    const result = dispatchCompactSyscall(frame);
    frame.rax = result;
    return result;
}

test "explicit userlog label detection" {
    try std.testing.expect(hasExplicitUserLogLabel("[seed] ready\n"));
    try std.testing.expect(hasExplicitUserLogLabel("[seed]"));
    try std.testing.expect(!hasExplicitUserLogLabel("seed ready\n"));
    try std.testing.expect(!hasExplicitUserLogLabel("[seed"));
    try std.testing.expect(!hasExplicitUserLogLabel("[] bad\n"));
}
