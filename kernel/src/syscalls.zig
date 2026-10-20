const b3 = @import("ipc_path_trace.zig");
const process_timer = @import("state/process_timer.zig");
const lock_diagnostics = @import("sync/lock_diagnostics.zig");
const std = @import("std");
const user_copy = @import("user_copy.zig");
const perf = @import("smp_perf.zig");
const lock_profile = @import("lock_profile.zig");
const kernel = @import("kernel.zig");
const interrupts = @import("interrupts.zig");
const scheduler = @import("scheduler.zig").connection;
const boot_static = @import("boot/main_static.zig");
const sc = @import("syscall/numbers.zig");
const kernel_log = @import("kernel_log.zig");
const kernel_log_abi = @import("kernel_abi_root").kernel_log_abi;
const syscall_lock_policy = @import("syscall/lock_policy.zig");
const capsule_syscalls = @import("syscall/capsule.zig");
const ram_cache_syscalls = @import("syscall/ram_cache.zig");
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
    write_user: ?*const fn ([]const u8) void = null,
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
    before_current_thread_leave: ?scheduler.BeforeCurrentThreadLeaveCallback = null,
    reacquire_kernel_state_lock: ?scheduler.BeforeCurrentThreadLeaveCallback = null,
    block_current_thread_for_event_preferred: *const fn (*TrapFrame, bool, bool, u64, u64, u64, ?scheduler.BeforeCurrentThreadLeaveCallback, ?kernel.ThreadWakeTarget) bool = scheduler.blockCurrentThreadPreferred,
    block_current_thread_for_event: *const fn (*TrapFrame, bool, bool, u64, u64, u64, ?scheduler.BeforeCurrentThreadLeaveCallback) bool,
    exit_current_process: *const fn (kernel.PrincipalId, *TrapFrame, ?scheduler.BeforeCurrentThreadLeaveCallback, ?scheduler.BeforeCurrentThreadLeaveCallback) void,
    total_usable_memory_bytes: u64,
};

var syscall_hooks_storage: Hooks = undefined;
var syscall_hooks_ready = false;

const KernelStateSpinLock = struct {
    diagnostic_record: lock_diagnostics.Record = .{},
    value: u8 = 0,
    profile_start: u64 = 0,

    fn waitWithInterruptWindow() void {
        // Syscalls enter with IF clear.  A CPU waiting for the global state
        // lock must still be able to acknowledge IPIs (notably a TLB
        // shootdown issued by the lock owner).
        if (@import("builtin").is_test) {
            std.atomic.spinLoopHint();
            return;
        }
        asm volatile ("sti; pause; cli" ::: .{ .memory = true });
    }

    fn lock(self: *KernelStateSpinLock, kind: lock_profile.OptionalKind) void {
        const lock_sample = if (comptime lock_profile.enabled) lock_profile.begin() else {};
        const start = perf.timestamp();
        var contended = false;
        while (true) {
            const diagnostic_if = lock_diagnostics.maskInterrupts();
            lock_diagnostics.beforeAcquire(&self.diagnostic_record, .{ .class = .kernel_state });
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) {
                lock_diagnostics.acquired(&self.diagnostic_record, .{ .class = .kernel_state });
                lock_diagnostics.restoreInterrupts(diagnostic_if);
                if (perf.enabled) {
                    self.profile_start = perf.timestamp();
                    lock_profile.acquired(kind, lock_sample, false);
                    perf.add(.kernel_lock_wait_cycles, self.profile_start -% start);
                    perf.add(.kernel_lock_acquires, 1);
                    if (contended) perf.add(.kernel_lock_contended, 1);
                }
                return;
            }
            lock_diagnostics.restoreInterrupts(diagnostic_if);
            contended = true;
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                waitWithInterruptWindow();
            }
        }
    }

    fn tryLock(self: *KernelStateSpinLock) bool {
        const diagnostic_if = lock_diagnostics.maskInterrupts();
        defer lock_diagnostics.restoreInterrupts(diagnostic_if);
        if (@cmpxchgStrong(u8, &self.value, 0, 1, .acquire, .monotonic) != null) {
            if (comptime lock_profile.enabled) lock_profile.event(.irq, 1);
            return false;
        }
        lock_diagnostics.acquiredTry(&self.diagnostic_record, .{ .class = .kernel_state });
        if (perf.enabled) {
            self.profile_start = perf.timestamp();
            lock_profile.acquired(.irq, lock_profile.begin(), true);
            perf.add(.kernel_lock_acquires, 1);
        }
        return true;
    }

    fn unlock(self: *KernelStateSpinLock) void {
        // Timer publication still belongs to K; unlink only at the release.
        const diagnostic_if = lock_diagnostics.maskInterrupts();
        defer lock_diagnostics.restoreInterrupts(diagnostic_if);
        lock_diagnostics.assertOwned(&self.diagnostic_record);
        const clockevent = @import("clockevent.zig");
        const changed = if (syscall_hooks_ready and syscall_hooks_storage.kernel_state_ready.*) blk: {
            const state = syscall_hooks_storage.state;
            // Rescan only after a change to the scan's inputs. A missed mark
            // would leave a late deadline that nothing heals, so safety-checked
            // builds verify the skip against a full rescan.
            if (!state.takeTimerDeadlineDirty()) {
                if (std.debug.runtime_safety)
                    std.debug.assert(clockevent.publishedWaiterDeadline() == (state.nextTimerWaiterDeadline() orelse 0));
                break :blk false;
            }
            const start = perf.timestamp();
            const timer_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.timer_publish, timer_sample);
            const result = clockevent.publish(state.nextTimerWaiterDeadline());
            perf.timerScanRecord(start, result);
            break :blk result;
        } else false;
        perf.elapsed(.kernel_lock_hold_cycles, self.profile_start);
        if (comptime lock_profile.enabled) lock_profile.release();
        lock_diagnostics.releasing(&self.diagnostic_record);
        @atomicStore(u8, &self.value, 0, .release);
        if (changed) clockevent.notifyChanged();
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

    kernel_state_lock.lock(if (lock_profile.enabled) .teardown else {});
    while (true) {
        _ = scheduler.stopPrincipalThreads(principal);
        kernel_state_lock.unlock();
        scheduler.waitForRemotePrincipalQuiescence(principal);
        kernel_state_lock.lock(if (lock_profile.enabled) .teardown else {});

        // A syscall or THREAD_CREATE admitted before the lifecycle gate may
        // have finished while the lock was dropped. Stop its result and scan
        // CPU ownership again before permitting teardown.
        _ = scheduler.stopPrincipalThreads(principal);
        const current_cpu = scheduler.currentCpu();
        if (!scheduler.principalRunningExcept(principal, current_cpu)) {
            return .acquired;
        }
    }
}

pub fn endExternalPrincipalTeardown() void {
    native_ipc_syscalls.drainCancellations(getHooks(), getHooks().state);
    capsule_syscalls.drainDeviceMemoryRetirements(getHooks().state);
    ram_cache_syscalls.drain(getHooks().state, getHooks().free_list);
    getHooks().state.reclaimFdCapabilities();
    scheduler.endPrincipalLifecycle();
    kernel_state_lock.unlock();
}

const KernelStateLockDropContext = if (lock_profile.enabled) struct {
    lock_held: *bool,
    kind: lock_profile.Kind = .other,
} else struct {
    lock_held: *bool,
};

fn dropKernelStateLockBeforeCurrentThreadLeave(ctx_ptr: *anyopaque) void {
    const ctx: *KernelStateLockDropContext = @ptrCast(@alignCast(ctx_ptr));
    if (!ctx.lock_held.*) return;
    const cleanup_start = b3.now();
    defer b3.end(511, cleanup_start);
    native_ipc_syscalls.drainCancellations(getHooks(), getHooks().state);
    capsule_syscalls.drainDeviceMemoryRetirements(getHooks().state);
    ram_cache_syscalls.drain(getHooks().state, getHooks().free_list);
    kernel_state_lock.unlock();
    ctx.lock_held.* = false;
    @import("notify_metric.zig").finish();
}

fn reacquireKernelStateLockAfterRemoteQuiescence(ctx_ptr: *anyopaque) void {
    const ctx: *KernelStateLockDropContext = @ptrCast(@alignCast(ctx_ptr));
    if (ctx.lock_held.*) return;
    if (lock_profile.enabled) lock_profile.event(ctx.kind, 2);
    kernel_state_lock.lock(if (lock_profile.enabled) ctx.kind else {});
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
    end = maxStaticEnd(end, ram_cache_syscalls.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, @import("ipc_metric.zig").staticEnd());
    end = maxStaticEnd(end, b3.staticEnd());
    end = maxStaticEnd(end, @import("clockevent.zig").kernelStaticStorageEndAddr());
    return end;
}

pub fn init(new_hooks: Hooks) void {
    const realtime_clock = @import("realtime_clock.zig");
    realtime_clock.initialize(@import("arch/x86_64/platform.zig").kernelPointerPaddr);
    new_hooks.write(if (realtime_clock.usesKvmClock()) "clock: source=kvm\n" else "clock: source=tsc-or-rtc\n");
    new_hooks.write("clock: realtime counter_hz=");
    new_hooks.print_number(realtime_clock.counterFrequencyHz());
    new_hooks.write("\n");
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

// A kernel-mode IRQ may interrupt a K-free page fault holding AS/shared VM
// inside an interrupt window. K tryLock alone cannot make user copyout safe:
// AS's CPU-only recursive owner would admit this IRQ as the interrupted owner.
// Keep registrations/deadlines intact until user mode or the lock-free idle
// HLT. The BSP revisits timer FDs on every timer IRQ and IRQ FDs on each tick,
// even without another device edge; the next eligible tick retries delivery
// (subject to the existing K/shootdown contention checks). Clockevent's retry
// delay retains the periodic tick, so deferral does not disarm that opportunity.
fn interruptCanCompleteUserWait(from_user: bool) bool {
    return from_user or scheduler.currentCpuInLockFreeIdleWait();
}

/// IRQ publication itself is lock-free, but completing an FD wait mutates its
/// generational wait group and must use the same KernelState lock as syscalls.
/// Interrupt context must never spin on a lock held by the interrupted CPU;
/// the BSP timer retries any publication that loses this try-lock race.
pub fn completePendingIrqFdWaitersFromInterrupt(from_user: bool) void {
    if (!interruptCanCompleteUserWait(from_user)) return;
    if (!syscall_hooks_ready) return;
    // Counter publication is already complete. Leave registrations pending
    // for the BSP timer if copyout could reenter this CPU's TLB shootdown.
    if (user_copy.tlbShootdownActiveOnCurrentCpu()) return;
    const h = getHooks();
    if (!h.kernel_state_ready.*) return;
    if (!kernel_state_lock.tryLock()) return;
    defer kernel_state_lock.unlock();

    native_ipc_syscalls.drainCancellations(h, h.state);
    completePendingIrqFdWaitersLocked(h);
}

// Keep the seed batch out of the admission frame: rejected/nested IRQs
// must not reserve it. The caller retains K through collection and publication,
// so persistent group links preserve the whole ready set across copy batches.
noinline fn completePendingIrqFdWaitersLocked(h: *const Hooks) void {
    var targets: [32]kernel.ThreadWakeTarget = undefined;
    while (true) {
        const count = h.state.takeReadyIrqWaiters(targets[0..]);
        if (count == 0) break;
        _ = fd_syscalls.wakeThreadTargets(h, h.state, targets[0..count]);
    }
}

pub fn completePendingTimerFdWaitersFromInterrupt(now_ns: u64, from_user: bool) void {
    if (!syscall_hooks_ready or !getHooks().kernel_state_ready.*) return;
    const clockevent = @import("clockevent.zig");
    if (!clockevent.waiterIsDue(now_ns)) return;
    if (!interruptCanCompleteUserWait(from_user) or user_copy.tlbShootdownActiveOnCurrentCpu()) {
        clockevent.deferContendedDelivery(now_ns);
        return;
    }
    if (!kernel_state_lock.tryLock()) {
        clockevent.deferContendedDelivery(now_ns);
        return;
    }
    defer kernel_state_lock.unlock();
    completePendingTimerFdWaitersLocked(getHooks(), now_ns);
}

// Only an eligible interrupt holding K completes groups. Persistent links
// preserve the whole ready set while claimed groups are copied in batches.
noinline fn completePendingTimerFdWaitersLocked(h: *const Hooks, now_ns: u64) void {
    // The published deadline is due: always republish on this unlock, even
    // if no waiter matched, or the expired value would rearm immediately.
    h.state.markTimerDeadlineDirty();
    var targets: [32]kernel.ThreadWakeTarget = undefined;
    while (true) {
        const count = h.state.takeReadyTimerWaiters(now_ns, &targets);
        if (count == 0) break;
        _ = fd_syscalls.wakeThreadTargets(h, h.state, targets[0..count]);
    }
}

// Traps cannot hold the private KernelState lock. Keep expiry consumption,
// delivery and copyout in this entry's single tryLock interval, exactly like
// IRQ FD completion. Failure must leave the deadline for the next BSP tick.
// The timer caller enters with IF clear; T is released before K -> AS copyout.
// AS/TLB waits use the existing interrupt windows, while a nested IRQ loses
// this tryLock without consuming an expiry. Never hold T while waiting for AS.
pub fn completeProcessSignalTimersFromInterrupt(now_tick: u64, from_user: bool) void {
    if (!interruptCanCompleteUserWait(from_user)) return;
    if (!syscall_hooks_ready or !getHooks().kernel_state_ready.*) return;
    if (user_copy.tlbShootdownActiveOnCurrentCpu()) return;
    if (!kernel_state_lock.tryLock()) return;
    defer kernel_state_lock.unlock();
    const state = getHooks().state;
    while (state.process_timers.take(now_tick)) |expired| {
        completeProcessSignalTimerLocked(state, expired);
    }
}

fn completeProcessSignalTimerLocked(state: *kernel.KernelState, expired: process_timer.Expired) void {
    // A saved notification may outlive its principal. Check before any effect;
    // generation-qualified clear also cannot erase a replacement timer.
    const descriptor = state.processDescriptor(expired.principal) orelse return;
    if (descriptor.generation != expired.generation) return;
    _ = scheduler.deliverSignal(state, expired.principal, expired.generation, expired.signo, sc.syscall_err_not_ready) orelse {
        process_timer.clear(state, expired.principal, expired.generation);
        return;
    };
    if (comptime !@import("builtin").is_test and @import("state/reference_check.zig").enabled)
        @import("kernel_log.zig").writeFmt("process-timer: delivered principal={} generation={} signo={}\n", .{ @intFromEnum(expired.principal), expired.generation, expired.signo });
}

fn hasExplicitUserLogLabel(message: []const u8) bool {
    if (message.len < 3 or message[0] != '[') return false;
    const close_index = std.mem.indexOfScalar(u8, message, ']') orelse return false;
    if (close_index <= 1) return false;
    return close_index + 1 == message.len or message[close_index + 1] == ' ';
}

fn writeThreadUserLogPrefix(h: *const Hooks, thread_index: usize) void {
    var buffer: [40]u8 = undefined;
    const prefix = std.fmt.bufPrint(&buffer, "[Thread {d}] ", .{thread_index}) catch return;
    (h.write_user orelse h.write)(prefix);
}

fn lifecycleAdmissionRejectedResult(nr: u64) u64 {
    // Byte-count syscalls already use negative native errors. Admission has
    // performed no I/O, so +NOT_READY would falsely report two bytes written
    // (or read) while a remote STOP owns the process lifecycle gate.
    return switch (nr) {
        sc.syscall_fd_read, sc.syscall_fd_write, sc.syscall_fd_readv, sc.syscall_fd_writev, sc.syscall_getrandom => 0 -% sc.syscall_err_not_ready,
        else => sc.syscall_err_not_ready,
    };
}

test "lifecycle admission does not report byte progress for a rejected syscall" {
    const not_ready: u64 = 0 -% sc.syscall_err_not_ready;
    for ([_]u64{ sc.syscall_fd_read, sc.syscall_fd_write, sc.syscall_fd_readv, sc.syscall_fd_writev, sc.syscall_getrandom }) |nr| {
        try std.testing.expectEqual(not_ready, lifecycleAdmissionRejectedResult(nr));
    }
    for ([_]u64{ sc.syscall_process_signal_ctl, sc.syscall_fd_close, sc.syscall_ipc_notify, sc.syscall_futex_wait }) |nr| {
        try std.testing.expectEqual(sc.syscall_err_not_ready, lifecycleAdmissionRejectedResult(nr));
    }
}

var reference_audit_calls: if (@import("state/reference_check.zig").enabled) u64 else void = if (@import("state/reference_check.zig").enabled) 0 else {};
var reference_audit_count: if (@import("state/reference_check.zig").enabled) u64 else void = if (@import("state/reference_check.zig").enabled) 0 else {};

fn dispatchCompactSyscall(frame: *TrapFrame) u64 {
    const generic_admission = b3.now();
    const runtime_path = perf.runtimePath(frame.rax, frame.rdi);
    const profile_kind = if (lock_profile.enabled) lock_profile.classify(frame.rax) else {};
    if (frame.rax == sc.syscall_clock_read) frame.rdx = 0;
    // Admission may reject before runtime dispatch. Output-only RDX must
    // still distinguish an unperformed wake from a committed wake count.
    if (frame.rax == sc.syscall_futex_wake_wait) frame.rdx = std.math.maxInt(u64);
    const base_hooks = getHooks();
    if (!base_hooks.kernel_state_ready.*) return sc.syscall_err_not_ready;
    if (frame.rax == sc.syscall_clock_read) {
        // Scalar time reads touch no capability, user mapping or KernelState
        // object. Keep the live CPU reference and ordinary signal/user return;
        // uncertain admission and CMOS fall back to the existing exclusion.
        if ((frame.cs & 3) == 3 and (frame.ss & 3) == 3) {
            switch (scheduler.currentClockReadAdmission()) {
                .blocked => return sc.syscall_err_not_ready,
                .fast => {
                    if (runtime_syscalls.tryClockRead(frame)) |result| {
                        perf.runtimeAdd(runtime_path, .reserved, 1);
                        if (comptime lock_profile.enabled) lock_profile.event(.runtime, 0);
                        return result;
                    }
                },
                .slow => {},
            }
        }
        perf.runtimeAdd(runtime_path, .no_claim, 1);
    }
    const state = base_hooks.state;
    const proc = scheduler.currentPrincipal();
    @import("ipc_metric.zig").recordSyscall(state, frame.rax, proc);
    @import("notify_metric.zig").begin(frame.rax, @intFromEnum(proc), scheduler.currentThread(), scheduler.currentThreadGeneration() orelse 0);
    defer @import("notify_metric.zig").finish();
    // Recorded on entry, not at deschedule: the trap frame's rax carries the
    // syscall number in and the result out, so by the time a thread is placed
    // again the number is gone.  rdi is kept alongside because it is the fd or
    // futex address the spin is going around.
    const hold_kernel_state_lock = syscall_lock_policy.needsKernelStateLock(frame.rax);
    var lock_held = false;
    var lock_drop_context = KernelStateLockDropContext{ .lock_held = &lock_held };
    if (lock_profile.enabled) lock_drop_context.kind = profile_kind;
    if (comptime lock_profile.enabled) {
        if (!hold_kernel_state_lock) lock_profile.event(profile_kind, 0);
    }
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

    b3.end(506, generic_admission);
    const runtime_lock_start = perf.runtimeTimestamp(runtime_path);
    if (hold_kernel_state_lock) {
        const b3_lock = b3.now();
        kernel_state_lock.lock(profile_kind);
        b3.end(100, b3_lock);
        lock_held = true;
        const drain_start = b3.now();
        native_ipc_syscalls.drainCancellations(h, state);
        // Reevaluate a previous self-exit after the switch, before process
        // allocation can select its now CPU-quiescent slot for reuse.
        @import("memory/user_vm.zig").retryRetiredAddressSpaces();
        b3.end(507, drain_start);
    }
    perf.runtimeElapsed(runtime_path, .lock_wait_cycles, runtime_lock_start);
    const runtime_body_start = perf.runtimeTimestamp(runtime_path);
    defer {
        perf.runtimeElapsed(runtime_path, .locked_body_cycles, runtime_body_start);
        const runtime_unlock_start = perf.runtimeTimestamp(runtime_path);
        const trace_retire = b3.now();
        defer b3.end(508, trace_retire);
        if (lock_held) {
            const retire_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            @import("memory/user_vm.zig").retryRetiredAddressSpaces();
            state.retryInvocationStorage();
            state.retryReferenceStorage();
            native_ipc_syscalls.drainCancellations(h, state);
            capsule_syscalls.drainDeviceMemoryRetirements(state);
            ram_cache_syscalls.drain(state, h.free_list);
            if (state.hasPendingDataReturns()) {
                // Never acquire all-AS while a syscall's inner AS/shared
                // transaction is held. Handler defers have run before this
                // outer drain, and KernelState still excludes VMO finalizers.
                @import("memory/user_vm.zig").lockAllVmTransactions();
                state.drainPendingDataReturns();
                @import("memory/user_vm.zig").unlockAllVmTransactions();
            }
            if (comptime @import("state/reference_check.zig").enabled) {
                reference_audit_calls +%= 1;
                if (reference_audit_calls % 1024 == 0 or frame.rax == sc.syscall_vmo_revoke) {
                    // Handler defers have dropped inner locks. K -> all AS ->
                    // shared VM fixes both old sources and new edge identities.
                    @import("memory/user_vm.zig").lockAllVmTransactions();
                    @import("state/reference_check.zig").check(state) catch |err| @import("state/reference_check.zig").failure(err);
                    const lifetime = if (reference_audit_count % 64 == 0) @import("state/reference_check.zig").census(state, h.free_list) else null;
                    @import("memory/user_vm.zig").unlockAllVmTransactions();
                    if (lifetime) |value| @import("state/reference_check.zig").report(value);
                    reference_audit_count += 1;
                    if (std.math.isPowerOfTwo(reference_audit_count)) @import("kernel_log.zig").writeFmt("reference-check comparisons={} mismatches=0\n", .{reference_audit_count});
                }
            }
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.retire, retire_sample);
            kernel_state_lock.unlock();
        }
        perf.runtimeElapsed(runtime_path, .unlock_cycles, runtime_unlock_start);
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
        return lifecycleAdmissionRejectedResult(frame.rax);
    }

    // Timeout/signal completion invalidates the scheduler token without
    // traversing KernelState.  Reclaim that thread's exact generational wait
    // group at its next stateful syscall before any user poll array can be
    // reused for an unrelated FD.
    if (hold_kernel_state_lock) {
        const fd_cancel_start = b3.now();
        defer b3.end(509, fd_cancel_start);
        const thread_index = scheduler.currentThread();
        if (scheduler.currentThreadGeneration()) |thread_generation| {
            state.cancelFdWaitGroupsForThread(proc, thread_index, thread_generation);
        }
    }

    const dispatch_probe_start = b3.now();
    if (process_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (capsule_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (fd_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    b3.end(510, dispatch_probe_start);
    if (native_ipc_syscalls.dispatchAfterScalarMiss(h, state, proc, frame)) |result| {
        return result;
    }
    if (runtime_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }

    return switch (frame.rax) {
        sc.syscall_kernel_log_read => blk: {
            if (comptime !kernel_log.ring_enabled) break :blk sc.syscall_err_not_ready;
            if (frame.rsi != @sizeOf(kernel_log_abi.Read)) break :blk sc.syscall_err_invalid;
            var reply = std.mem.zeroes(kernel_log_abi.Read);
            if (!h.copy_user_bytes_from_va(proc, frame.rdi, std.mem.asBytes(&reply)[0..32]))
                break :blk sc.syscall_err_invalid;
            if (!kernel_log_abi.requestIsValid(&reply))
                break :blk sc.syscall_err_invalid;
            if (!kernel_log.readDebug(reply.cursor, reply.bytes[0..@intCast(reply.limit)], &reply))
                break :blk sc.syscall_err_empty;
            if (!h.copy_bytes_to_user_va(proc, frame.rdi, std.mem.asBytes(&reply)))
                break :blk sc.syscall_err_invalid;
            break :blk sc.syscall_ok;
        },
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
            (h.write_user orelse h.write)(msg);
            break :blk sc.syscall_ok;
        },
        else => sc.syscall_err_invalid,
    };
}

const DirectEntryPlan = struct {
    handoff: scheduler.DirectHandoff,
    pair: @import("sync/ipc_endpoint.zig").Pair,
    notify: scheduler.DirectNotifyPlan,
};
var direct_entry_plans: []DirectEntryPlan = &.{};
pub fn initDirectIpcStorage(count: usize) !void {
    direct_entry_plans = try @import("arch/x86_64/cpu_runtime.zig").alloc(DirectEntryPlan, count);
}

// Called before saving callee-saved user GPRs. Win64 preserves those registers;
// all code reachable here is covered by the live-XState scalar ELF gate.
pub export fn syscallDirectIpcPrepare(frame: *const TrapFrame) callconv(.winapi) u64 {
    if (@import("builtin").is_test) return 0;
    if (frame.rax == sc.syscall_ipc_notify or frame.rax == sc.syscall_ipc_notify_wait) return prepareDirectNotifyEntry(frame);
    const metric = @import("ipc_metric.zig");
    metric.entryBegin();
    var census_success = false;
    defer metric.entryEnd(census_success);
    metric.recordElapsed(if (frame.rax == sc.syscall_ipc_call) .entry_call_attempt else .entry_reply_attempt, 0);
    if ((frame.r15 & 0xffff_ffff) != 0 or frame.rdi > std.math.maxInt(u32) or frame.rdx != std.math.maxInt(u64) or
        frame.rsi & ~@as(u64, @import("kernel_abi_root").ipc_abi.wait_uninterruptible) != 0 or !syscall_hooks_ready)
    {
        if (comptime metric.enabled) {
            if (frame.r15 & 0xffff_ffff != 0) metric.entryReason(.fd_transfer) else if (frame.r15 >> 32 != 0) metric.entryReason(.receive_capacity);
        }
        return 0;
    }
    if (native_ipc_syscalls.readReceive(frame) == null) {
        metric.entryReason(.receive_capacity);
        return 0;
    }
    const call = frame.rax == sc.syscall_ipc_call;
    if (!call and frame.rax != sc.syscall_ipc_reply_recv) return 0;
    if (!syscall_hooks_storage.kernel_state_ready.*) return 0;
    const probe_key = (frame.rax << 32) | frame.rdi;
    if (!scheduler.directIpcProbeDue(probe_key)) return 0;
    // Waiting with an IRQ window would expose an incomplete frame. A busy K
    // is a read-only miss; the ordinary entry completes its frame before wait.
    if (!kernel_state_lock.tryLock()) return 0;
    var admitted = false;
    defer if (!admitted) {
        scheduler.noteDirectIpcMiss(probe_key);
        kernel_state_lock.unlock();
    };
    const state = syscall_hooks_storage.state;
    const owner = scheduler.currentPrincipal();
    const tid = scheduler.currentThread();
    if (tid > std.math.maxInt(u32) or state.fd_wait_group_thread_counts.get(@intCast(tid)) != null) return 0;
    const generation = scheduler.currentThreadGeneration() orelse return 0;
    const resolved = @import("state/fd_capability.zig").directIpcUnderK(state, owner, @intCast(frame.rdi), call) orelse return 0;
    const body = if (call) scheduler.ipcBlock(tid, generation, owner) orelse return 0 else blk: {
        const node = scheduler.peekIpcImplicit(tid, generation, owner) orelse return 0;
        break :blk @as(*kernel.IpcReplySlot, @ptrCast(@alignCast(node.context.?)));
    };
    var pair = @import("sync/ipc_endpoint.zig").Pair.acquire(resolved.list.exclusion.?, &body.exclusion);
    defer if (!admitted) pair.end();
    if (comptime metric.enabled) metric.route(0, frame.rax, owner, (tid << 32) | generation, frame.r15 >> 32, resolved.list, null);
    const plan = &direct_entry_plans[scheduler.currentCpu()];
    if (!scheduler.prepareDirectHandoff(owner, frame, resolved, body, call, syscall_hooks_storage.user_spaces, &plan.handoff)) return 0;
    // Move the guard into CPU-owned storage; no pointer into this call's stack
    // survives the return to assembly. Interrupts remain masked until SYSRET.
    plan.pair = pair;
    census_success = true;
    admitted = true;
    return 1;
}

pub export fn syscallDirectIpcCommit(frame: *TrapFrame) callconv(.winapi) u64 {
    if (@import("builtin").is_test) return 0;
    if (frame.rax == sc.syscall_ipc_notify or frame.rax == sc.syscall_ipc_notify_wait) return commitDirectNotifyEntry(frame);
    const plan = &direct_entry_plans[scheduler.currentCpu()];
    @import("xstate_runtime.zig").beginIpcLive();
    scheduler.commitDirectHandoff(&plan.handoff, frame);
    @import("ipc_metric.zig").recordElapsed(if (plan.handoff.call) .entry_call_success else .entry_reply_success, 0);
    @import("ipc_metric.zig").recordCpuSyscall(if (plan.handoff.call) sc.syscall_ipc_call else sc.syscall_ipc_reply_recv);
    plan.pair.end();
    const state = syscall_hooks_storage.state;
    // Drop transferred authorities only after E/T end. K still retains the
    // owner blocks, so these releases cannot reclaim a borrowed peer frame.
    if (plan.handoff.call) {
        state.releaseIpcCallReference(plan.handoff.receive.ref);
    } else {
        state.releaseIpcCallReference(plan.handoff.body.ref);
        state.releaseIpcCallReference(plan.handoff.body.ref);
    }
    kernel_state_lock.unlock();
    return 1;
}

noinline fn prepareDirectNotifyEntry(frame: *const TrapFrame) u64 {
    const b8_start = @import("ipc_metric.zig").timestamp();
    defer @import("notify_metric.zig").segment(0, b8_start);
    const metric = @import("notify_metric.zig");
    const abi = @import("kernel_abi_root").ipc_abi;
    const wait = frame.rax == sc.syscall_ipc_notify_wait;
    const send = !wait or frame.rdx & abi.notify_wait_flag_notify_peer != 0;
    metric.fastCount(if (wait) 1 else 0);
    if (!syscall_hooks_ready or !syscall_hooks_storage.kernel_state_ready.* or frame.rdi > std.math.maxInt(u32) or
        (wait and (frame.rdx & ~abi.notify_wait_known_flags != 0 or (frame.rsi != 0 and frame.rsi != std.math.maxInt(u64)))))
    {
        metric.fastCount(8);
        return 0;
    }
    const probe_key = (frame.rax << 32) | frame.rdi;
    // A miss hint suppresses optimization attempts only. It retains no FD,
    // waiter or authority and cannot admit a syscall. Repeated losing NOTIFY
    // handoffs must take the established wake once, before K/E/preflight, not
    // pay full speculative admission plus the same wake on every operation.
    if (!wait and !scheduler.directNotifyProbeDue(probe_key)) {
        metric.fastCount(11);
        return 0;
    }
    if (!kernel_state_lock.tryLock()) {
        metric.fastCount(9);
        return 0;
    }
    var admitted = false;
    defer if (!admitted) kernel_state_lock.unlock();
    const state = syscall_hooks_storage.state;
    const owner = scheduler.currentPrincipal();
    const tid = scheduler.currentThread();
    if (tid > std.math.maxInt(u32) or state.fd_wait_group_thread_counts.get(@intCast(tid)) != null) {
        metric.fastCount(11);
        return 0;
    }
    const found = @import("state/fd_capability.zig").notifyUnderK(state, owner, @intCast(frame.rdi), .{
        .send = send,
        .recv = wait,
        .wait = wait and frame.rsi != 0,
    }) orelse {
        metric.fastCount(10);
        return 0;
    };
    var pair = @import("sync/ipc_endpoint.zig").Pair.acquire(&found.channel.exclusion, null);
    defer if (!admitted) pair.end();
    if (comptime metric.enabled) {
        metric.begin(frame.rax, @intFromEnum(owner), tid, scheduler.currentThreadGeneration() orelse 0);
        metric.sending(send);
        metric.coalesced(send and found.channel.notify_pending[1 - found.side]);
        const waiter = found.channel.notify_waiters[1 - found.side];
        if (waiter.active) metric.target(.{ .owner = waiter.owner, .thread_index = waiter.thread_index, .thread_generation = waiter.thread_generation, .wait_token = waiter.wait_token }, 1) else {
            const hint = found.channel.receiver_hints[1 - found.side];
            if (hint.active) metric.target(.{ .owner = hint.owner, .thread_index = hint.thread_index, .thread_generation = hint.thread_generation }, 3);
        }
    }
    const plan = &direct_entry_plans[scheduler.currentCpu()];
    if (!scheduler.prepareDirectNotify(owner, frame, found.channel, found.side, wait, send, syscall_hooks_storage.user_spaces, &plan.notify)) {
        metric.fastCount(11);
        if (!wait) scheduler.noteDirectIpcMiss(probe_key);
        metric.abandonPreflight();
        return 0;
    }
    plan.pair = pair;
    admitted = true;
    return 1;
}

noinline fn commitDirectNotifyEntry(frame: *TrapFrame) u64 {
    const b8_start = @import("ipc_metric.zig").timestamp();
    defer @import("notify_metric.zig").segment(2, b8_start);
    const metric = @import("notify_metric.zig");
    const plan = &direct_entry_plans[scheduler.currentCpu()];
    const nr = frame.rax;
    const owner = plan.notify.source.owner_process;
    const tid = plan.notify.source.id;
    const generation = plan.notify.source.generation;
    const fd: kernel.Fd = @intCast(frame.rdi);
    const uninterruptible = frame.rdx & @import("kernel_abi_root").ipc_abi.wait_uninterruptible != 0;
    metric.fastCount(if (plan.notify.wait) 3 else 2);
    const mode: usize = switch (plan.notify.mode) {
        .local, .hint => 4,
        .remote => 5,
        .immediate => 6,
        .block => 7,
    };
    metric.fastCount(mode);
    metric.outcome(if (plan.notify.mode == .hint) 209 else 200 + mode);
    if (plan.notify.wait) metric.waitOutcome(if (plan.notify.mode == .block or (plan.notify.mode == .local and plan.notify.block_source)) 4 else if (plan.notify.block_source) 3 else 1);
    @import("ipc_metric.zig").recordCpuSyscall(nr);
    @import("xstate_runtime.zig").beginIpcLive();
    const result = scheduler.commitDirectNotify(&plan.notify, frame);
    plan.pair.end();
    // Publication precedes the wake edge. Never retain E or a plan pointer
    // across a callback that abandons this CPU's syscall stack.
    if (result.remote_cpu) |cpu| {
        lock_diagnostics.assertOwned(&kernel_state_lock.diagnostic_record);
        if (!@import("smp.zig").wakeCpuForNotify(cpu)) @panic("direct notify wake IPI failed");
    }
    if (result.parked) {
        // Settle the source's census before dropping K or abandoning the AP
        // stack. A later NOTIFY completes the saved native wait directly.
        metric.finish();
        kernel_state_lock.unlock();
        if (result.idle) scheduler.finishDirectNotifyIdle(frame);
        @import("traps.zig").stagePendingSignalForUserReturn(frame);
        return 2;
    }
    if (result.block_token != 0) {
        // The generic blocker may abandon this stack for AP idle or select a
        // different context. Save while the source still owns the CPU pin.
        @import("xstate_runtime.zig").saveIpcLiveIfNeeded();
        var held = true;
        var drop = KernelStateLockDropContext{ .lock_held = &held };
        if (lock_profile.enabled) drop.kind = lock_profile.classify(nr);
        const ok = syscall_hooks_storage.block_current_thread_for_event(frame, true, uninterruptible, result.block_token, 0, sc.syscall_err_not_ready, .{ .context = &drop, .run = dropKernelStateLockBeforeCurrentThreadLeave });
        if (held) {
            if (!ok) {
                scheduler.cancelCurrentWaitToken(generation, result.block_token);
                syscall_hooks_storage.state.unregisterIpcNotifyWaiterForFd(owner, fd, tid, generation, result.block_token);
                frame.rax = sc.syscall_err_not_ready;
            }
            kernel_state_lock.unlock();
        }
        metric.finish();
        @import("traps.zig").stagePendingSignalForUserReturn(frame);
        // A generic block may select an interrupt continuation. It must use
        // IRET, never assume the selected frame has native SYSCALL provenance.
        return 2;
    }
    kernel_state_lock.unlock();
    metric.finish();
    return 1;
}

// This root and every indirect callee are covered by the scalar ELF gate.
// A miss retains the original register frame and returns to assembly's XSAVE
// entry. After delivery commits, only the existing block/cancel path may run.
pub export fn syscallIpcScalarDispatch(frame: *TrapFrame) callconv(.winapi) u64 {
    if (@import("builtin").is_test) return 0;
    if (frame.rax == sc.syscall_ipc_send) @import("ipc_metric.zig").recordElapsed(.entry_fail_opcode, 0);
    b3.begin(frame);
    const nr = frame.rax;
    const admission_start = b3.now();
    if (frame.rax != sc.syscall_ipc_send and frame.rax != sc.syscall_ipc_call and frame.rax != sc.syscall_ipc_reply_recv) return 0;
    if (!native_ipc_syscalls.fastRegistersAdmit(frame)) return 0;
    if (!syscall_hooks_ready) return 0;
    const base_hooks = getHooks();
    if (!base_hooks.kernel_state_ready.*) return 0;
    @import("xstate_runtime.zig").beginIpcLive();
    const proc = scheduler.currentPrincipal();
    const state = base_hooks.state;
    var lock_held = false;
    var drop = KernelStateLockDropContext{ .lock_held = &lock_held };
    if (lock_profile.enabled) drop.kind = lock_profile.classify(frame.rax);
    var hooks = base_hooks.*;
    hooks.before_current_thread_leave = null;
    hooks.reacquire_kernel_state_lock = null;
    b3.end(502, admission_start);
    defer if (lock_held) kernel_state_lock.unlock();
    const result = blk: {
        // The dedicated entry already attempted direct handoff. Its miss
        // proceeds once under K; no intermediate lock-free lookup is replayed.
        const lock_start = b3.now();
        kernel_state_lock.lock(if (lock_profile.enabled) lock_profile.classify(nr) else {});
        lock_held = true;
        b3.end(100, lock_start);
        hooks.before_current_thread_leave = .{ .context = &drop, .run = dropKernelStateLockBeforeCurrentThreadLeave };
        if (!state.hasActivePrincipal(proc) or scheduler.principalLifecycleBlocksAdmission(proc)) return 0;
        break :blk native_ipc_syscalls.fastDispatchAfterRegisterAdmission(&hooks, state, proc, frame) orelse return 0;
    };
    @import("ipc_metric.zig").recordCpuSyscall(nr);
    frame.rax = result;
    if (lock_held) {
        kernel_state_lock.unlock();
        lock_held = false;
    }
    const signal_start = b3.now();
    @import("traps.zig").stagePendingSignalForUserReturn(frame);
    b3.end(503, signal_start);
    b3.finish();
    return 1;
}

pub export fn syscallDispatch(frame: *TrapFrame) callconv(.winapi) u64 {
    defer b3.finish();
    if (b3.enabled and frame.rax == b3.empty_nr) {
        frame.rax = 0;
        return 0;
    }
    // Save classification before delivery can replace the syscall frame.
    const runtime_path = perf.runtimePath(frame.rax, frame.rdi);
    const runtime_start = perf.runtimeTimestamp(runtime_path);
    defer {
        perf.runtimeAdd(runtime_path, .calls, 1);
        perf.runtimeElapsed(runtime_path, .total_cycles, runtime_start);
    }
    const result = dispatchCompactSyscall(frame);
    frame.rax = result;
    const runtime_stage_start = perf.runtimeTimestamp(runtime_path);
    @import("traps.zig").stagePendingSignalForUserReturn(frame);
    perf.runtimeElapsed(runtime_path, .return_stage_cycles, runtime_stage_start);
    return frame.rax;
}

test "explicit userlog label detection" {
    try std.testing.expect(hasExplicitUserLogLabel("[seed] ready\n"));
    try std.testing.expect(hasExplicitUserLogLabel("[seed]"));
    try std.testing.expect(!hasExplicitUserLogLabel("seed ready\n"));
    try std.testing.expect(!hasExplicitUserLogLabel("[seed"));
    try std.testing.expect(!hasExplicitUserLogLabel("[] bad\n"));
}

const InterruptWaiterTest = struct {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    const clockevent = @import("clockevent.zig");
    const owner = kernel.processPrincipalFromIndex(0).?;
    const tid = 7;
    const generation = 11;
    const token = 41;
    const poll_address = 0x1000;
    var storage: [kernel.runtimeStorageBytes()]u8 align(4096) = undefined;
    var state_ready = true;
    var copies: usize = 0;
    var notifications: usize = 0;

    fn notify() void {
        notifications += 1;
    }

    fn context() *scheduler.ThreadContext {
        return scheduler.threadContextMutable(tid).?;
    }

    fn copy(principal: kernel.PrincipalId, address: u64, value: u64) bool {
        std.debug.assert(principal == owner);
        std.debug.assert(address == poll_address + fd_abi.pollfd_revents_offset);
        std.debug.assert(value == fd_abi.event_readable);
        copies += 1;
        return true;
    }

    fn registered(state: *const kernel.KernelState) usize {
        var count: usize = 0;
        var iterator = state.task_fd_waiters.iterator();
        while (iterator.next()) |entry| {
            const waiter = entry.value;
            if (waiter.active) {
                std.debug.assert(!waiter.binding.completion_pending);
                count += 1;
            }
        }
        return count;
    }

    fn complete(timer: bool, from_user: bool) void {
        if (timer) completePendingTimerFdWaitersFromInterrupt(100, from_user) else completePendingIrqFdWaitersFromInterrupt(from_user);
    }

    fn run(timer: bool, kernel_interruption: bool, resume_idle: bool) !void {
        comptime std.debug.assert(@import("builtin").is_test);
        scheduler.initializeStaticStorage();
        defer scheduler.initializeStaticStorage();
        const ctx = context();
        const previous_context = ctx.*;
        defer ctx.* = previous_context;
        // A stopped native waiter can complete without dispatching a real
        // host CPU. Its resume-after-stop state still uses the real token path.
        ctx.* = .{ .id = tid, .allocated = true, .generation = generation, .owner_process = owner, .stopped = true, .wait_mailbox = true, .active_wait_token = token };
        try std.testing.expect(kernel.initRuntimeStorage(&storage));
        const state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        const saved_ready = syscall_hooks_ready;
        const saved_hooks = if (saved_ready) syscall_hooks_storage else undefined;
        defer {
            syscall_hooks_ready = saved_ready;
            syscall_hooks_storage = saved_hooks;
        }
        const clock_state = clockevent.testInstallNotifier(notify);
        defer clockevent.testRestoreState(clock_state);
        copies = 0;
        notifications = 0;
        state_ready = true;
        var free_list = kernel.FreePageList{};
        syscall_hooks_storage = undefined;
        syscall_hooks_storage.state = state;
        syscall_hooks_storage.free_list = &free_list;
        syscall_hooks_storage.kernel_state_ready = &state_ready;
        syscall_hooks_storage.write_user_u64 = copy;
        syscall_hooks_ready = true;
        const rights = kernel.FdRights{ .poll = true, .read = true, .irq_wait = true, .close = true };
        const fd = if (timer) try state.createTimerFd(owner, 100, 0, .{}, rights, 16) else try state.createIrqFd(owner, .{ .device = 0x1001, .kind = .msix, .vector = 1 }, rights, .{}, 16);
        const group = try state.beginFdWaitGroup(owner, tid, generation, token);
        state.fd_wait_groups.get(group.index).?.pollfds_va = poll_address;
        try std.testing.expect(try state.registerTaskReadableWaiterForFd(owner, fd, fd_abi.event_readable, poll_address, tid, generation, token, group));
        try state.armFdWaitGroup(group);
        if (timer) {
            _ = clockevent.publish(100);
        } else {
            var owners: [4]kernel.PrincipalId = undefined;
            try std.testing.expectEqual(@as(usize, 1), state.recordDeviceInterruptEvent(0x1001, 1, &owners));
        }
        const cpu = scheduler.currentCpu();
        user_copy.testSetTlbShootdownActive(cpu, !kernel_interruption);
        defer user_copy.testSetTlbShootdownActive(cpu, false);
        for (0..8) |_| {
            complete(timer, !kernel_interruption);
            try std.testing.expectEqual(@as(usize, 1), registered(state));
            try std.testing.expectEqual(@as(u64, token), ctx.active_wait_token);
            try std.testing.expect(ctx.wait_mailbox and !ctx.wait_completion_claimed);
            try std.testing.expectEqual(@as(usize, 0), copies);
            try std.testing.expectEqual(@as(usize, 0), notifications);
            if (timer) {
                try std.testing.expectEqual(@as(u64, 100), clockevent.publishedWaiterDeadline());
                try std.testing.expectEqual(@as(u64, 100), state.nextTimerWaiterDeadline().?);
                const deferred_clock = clockevent.testInstallNotifier(notify);
                clockevent.testRestoreState(deferred_clock);
                try std.testing.expectEqual(@as(u64, 50_100), deferred_clock.retry);
                try std.testing.expect(clockevent.selectDeadline(100, deferred_clock.deadline, deferred_clock.retry) <= 1_000_000);
            }
        }
        user_copy.testSetTlbShootdownActive(cpu, false);
        // Losing K after privilege admission must not consume a snapshot.
        try std.testing.expect(kernel_state_lock.tryLock());
        for (0..3) |_| complete(timer, true);
        kernel_state_lock.unlock();
        try std.testing.expectEqual(@as(usize, 1), registered(state));
        try std.testing.expectEqual(@as(usize, 0), copies);
        scheduler.testSetCurrentCpuLockFreeIdleWait(resume_idle);
        defer scheduler.testSetCurrentCpuLockFreeIdleWait(false);
        complete(timer, !resume_idle);
        try std.testing.expectEqual(@as(usize, 0), registered(state));
        try std.testing.expectEqual(@as(usize, 0), copies);
        try std.testing.expectEqual(@as(u64, 0), ctx.frame.r10);
        try std.testing.expectEqual(fd_abi.event_readable, ctx.frame.r8);
        try std.testing.expectEqual(@as(u64, 0), ctx.active_wait_token);
        try std.testing.expectEqual(@as(u64, 1), ctx.frame.rax);
        try std.testing.expect(ctx.resume_after_stop and !ctx.wait_mailbox and !ctx.wait_completion_claimed);
        for (0..8) |_| complete(timer, !resume_idle);
        try std.testing.expectEqual(@as(usize, 0), copies);
        try std.testing.expectEqual(@as(u64, 0), ctx.frame.r10);
        try std.testing.expectEqual(fd_abi.event_readable, ctx.frame.r8);
        try std.testing.expectEqual(@as(usize, if (timer) 1 else 0), notifications);
    }
};

test "shootdown interrupt guard preserves timer waiters until one completion" {
    try InterruptWaiterTest.run(true, false, false);
}

test "shootdown interrupt guard preserves IRQ waiters until one completion" {
    try InterruptWaiterTest.run(false, false, false);
}

test "shootdown interrupt guard process timer generation and contention" {
    const Fixture = struct {
        var ready: bool = true;
        var hint: [4]u8 = .{ 0x42, 0x42, 0x42, 0x42 };
        var copies: usize = 0;
        fn copy(_: kernel.PrincipalId, address: u64, bytes: []const u8) bool {
            std.debug.assert(address == 0x1000);
            // Publication must retain K even after T was released.
            std.debug.assert(!kernel_state_lock.tryLock());
            @memcpy(&hint, bytes);
            copies += 1;
            return true;
        }
    };
    scheduler.initializeStaticStorage();
    defer scheduler.initializeStaticStorage();
    try std.testing.expect(kernel.initRuntimeStorage(&InterruptWaiterTest.storage));
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    try state.initFromDetectedRegionsInPlace(1);
    const saved_ready = syscall_hooks_ready;
    const saved_hooks = if (saved_ready) syscall_hooks_storage else undefined;
    defer {
        syscall_hooks_ready = saved_ready;
        syscall_hooks_storage = saved_hooks;
    }
    syscall_hooks_storage = undefined;
    syscall_hooks_storage.state = state;
    syscall_hooks_storage.kernel_state_ready = &Fixture.ready;
    syscall_hooks_ready = true;
    const saved_copy = scheduler.SignalHintTest.copy;
    scheduler.SignalHintTest.copy = Fixture.copy;
    defer scheduler.SignalHintTest.copy = saved_copy;
    Fixture.hint = .{ 0x42, 0x42, 0x42, 0x42 };
    Fixture.copies = 0;
    const owner = state.createProcessDescriptor("old timer").?;
    const old_generation = state.processDescriptor(owner).?.generation;
    _ = process_timer.set(state, owner, old_generation, 4, 10, 0, 0);
    const saved = state.process_timers.take(10).?;
    try std.testing.expectEqual(old_generation, saved.generation);
    try std.testing.expect(state.removeProcessDescriptor(owner));
    try std.testing.expectEqual(owner, state.createProcessDescriptor("replacement timer").?);
    const generation = state.processDescriptor(owner).?.generation;
    try std.testing.expect(generation != old_generation);
    try std.testing.expect(state.setProcessSignalPendingHintVa(owner, 0x1000));
    const ctx = scheduler.threadContextMutable(7).?;
    const original = ctx.*;
    defer ctx.* = original;
    ctx.* = .{ .id = 7, .allocated = true, .ready = true, .generation = 17, .owner_process = owner };
    _ = process_timer.set(state, owner, generation, 4, 10, 10, 0);
    const replacement = process_timer.get(state, owner, generation, 0);
    try std.testing.expect(kernel_state_lock.tryLock());
    completeProcessSignalTimerLocked(state, saved);
    try std.testing.expect(scheduler.deliverSignal(state, owner, old_generation, 4, 0) == null);
    process_timer.clear(state, owner, old_generation);
    kernel_state_lock.unlock();
    try std.testing.expectEqual(@as(u64, 0), ctx.pending_signal_mask);
    try std.testing.expectEqualSlices(u8, &.{ 0x42, 0x42, 0x42, 0x42 }, &Fixture.hint);
    try std.testing.expectEqualDeep(replacement, process_timer.get(state, owner, generation, 0));
    // Contention must not consume or advance the deadline, even on repeat IRQs.
    try std.testing.expect(kernel_state_lock.tryLock());
    for (0..3) |_| completeProcessSignalTimersFromInterrupt(10, true);
    try std.testing.expectEqualDeep(replacement, process_timer.get(state, owner, generation, 0));
    kernel_state_lock.unlock();
    const cpu = scheduler.currentCpu();
    user_copy.testSetTlbShootdownActive(cpu, true);
    completeProcessSignalTimersFromInterrupt(10, true);
    user_copy.testSetTlbShootdownActive(cpu, false);
    try std.testing.expectEqualDeep(replacement, process_timer.get(state, owner, generation, 0));
    completeProcessSignalTimersFromInterrupt(10, true);
    try std.testing.expectEqual(@as(u64, 8), ctx.pending_signal_mask);
    try std.testing.expectEqualSlices(u8, &.{ 1, 0, 0, 0 }, &Fixture.hint);
    try std.testing.expectEqual(@as(usize, 1), Fixture.copies);
    for (0..3) |_| completeProcessSignalTimersFromInterrupt(10, true);
    try std.testing.expectEqual(@as(usize, 1), Fixture.copies);
    try std.testing.expectEqual(@as(u64, 10), process_timer.get(state, owner, generation, 10).remaining_ticks);
    // Correct generation with no recipient clears only its periodic timer.
    ctx.allocated = false;
    completeProcessSignalTimersFromInterrupt(20, true);
    try std.testing.expectEqualDeep(process_timer.State{}, process_timer.get(state, owner, generation, 20));
}

test "shootdown interrupt guard kernel IRQ defers timer FD until user tick" {
    try InterruptWaiterTest.run(true, true, false);
}

test "shootdown interrupt guard kernel IRQ defers IRQ FD until user tick" {
    try InterruptWaiterTest.run(false, true, false);
}

test "shootdown interrupt guard kernel IRQ defers timer FD until idle tick" {
    try InterruptWaiterTest.run(true, true, true);
}

test "shootdown interrupt guard kernel IRQ defers IRQ FD until idle tick" {
    try InterruptWaiterTest.run(false, true, true);
}

test "lock ordering K actual blocking and IRQ try failure success" {
    if (!lock_diagnostics.enabled) return;
    var state_lock: KernelStateSpinLock = .{};
    state_lock.lock(if (lock_profile.enabled) .other else {});
    try std.testing.expectEqual(lock_diagnostics.order.Mode.blocking, state_lock.diagnostic_record.mode);
    const irq = lock_diagnostics.enterInterrupt(false);
    try std.testing.expect(!state_lock.tryLock());
    try std.testing.expect(lock_diagnostics.testCpu().head == &state_lock.diagnostic_record);
    irq.end();
    state_lock.unlock();
    // A try success may invert rank, including an interrupted higher hold.
    var high: lock_diagnostics.Record = .{};
    lock_diagnostics.acquired(&high, .{ .class = .address_space });
    const next_irq = lock_diagnostics.enterInterrupt(false);
    try std.testing.expect(state_lock.tryLock());
    try std.testing.expectEqual(lock_diagnostics.order.Mode.try_once, state_lock.diagnostic_record.mode);
    state_lock.unlock();
    next_irq.end();
    lock_diagnostics.releasing(&high);
    lock_diagnostics.assertRootEmpty();
}

test "lock admission actual K try L exception retains both holds" {
    if (!lock_diagnostics.enabled) return;
    var k: KernelStateSpinLock = .{};
    k.lock(if (lock_profile.enabled) .other else {});
    try std.testing.expect(scheduler.tryBeginPrincipalLifecycle(@enumFromInt(7)));
    try std.testing.expectEqual(lock_diagnostics.order.Class.principal_lifecycle, lock_diagnostics.testCpu().head.?.key.class);
    try std.testing.expectEqual(lock_diagnostics.order.Mode.try_once, lock_diagnostics.testCpu().head.?.mode);
    var later: lock_diagnostics.Record = .{};
    try std.testing.expectError(error.ReverseOrder, lock_diagnostics.testCpu().beforeAcquire(&later, .{ .class = .principal_lifecycle }));
    scheduler.endPrincipalLifecycle();
    k.unlock();
    // L -> K is the ordinary order, and L permits lifecycle IPI waiting.
    try std.testing.expect(scheduler.tryBeginPrincipalLifecycle(@enumFromInt(7)));
    lock_diagnostics.assertCanWaitForIpi();
    k.lock(if (lock_profile.enabled) .other else {});
    k.unlock();
    scheduler.endPrincipalLifecycle();
    lock_diagnostics.assertRootEmpty();
}

const InterruptSnapshotTest = struct {
    const kernel_runtime = @import("kernel_runtime.zig");
    const F = InterruptWaiterTest;
    const fd_abi = F.fd_abi;
    const Race = enum { none, timeout, signal, generation, close, alias_or };
    var copies: usize = 0;
    var fail_copy: bool = false;
    var expected_events: u64 = fd_abi.event_readable;
    var seen: [kernel.fd_table_entries]bool = undefined;

    fn copy(principal: kernel.PrincipalId, address: u64, value: u64) bool {
        std.debug.assert(principal == F.owner);
        const index = (address - F.poll_address - fd_abi.pollfd_revents_offset) / fd_abi.pollfd_size;
        std.debug.assert(index < seen.len and !seen[index]);
        std.debug.assert(value == expected_events);
        // A competing timeout/signal cannot win after claim, even if copyout
        // fails. Claim keeps the thread non-runnable until final publication.
        std.debug.assert(scheduler.claimWaitTokenCompletion(F.tid, F.generation, F.owner, F.token) == .stale);
        seen[index] = true;
        copies += 1;
        return !fail_copy or index != 1;
    }

    fn run(timer: bool, fail: bool, race: Race) !void {
        comptime std.debug.assert(@import("builtin").is_test);
        scheduler.initializeStaticStorage();
        defer scheduler.initializeStaticStorage();
        const ctx = F.context();
        const saved_ctx = ctx.*;
        defer ctx.* = saved_ctx;
        ctx.* = .{ .id = F.tid, .allocated = true, .generation = F.generation, .owner_process = F.owner, .stopped = true, .wait_mailbox = true, .active_wait_token = F.token };
        try std.testing.expect(kernel.initRuntimeStorage(&F.storage));
        const state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        const saved_state = kernel_runtime.kernel_state_global;
        kernel_runtime.kernel_state_global = state;
        defer kernel_runtime.kernel_state_global = saved_state;
        const saved_ready = syscall_hooks_ready;
        const saved_hooks = if (saved_ready) syscall_hooks_storage else undefined;
        defer {
            syscall_hooks_ready = saved_ready;
            syscall_hooks_storage = saved_hooks;
        }
        F.state_ready = true;
        syscall_hooks_storage = undefined;
        syscall_hooks_storage.state = state;
        syscall_hooks_storage.kernel_state_ready = &F.state_ready;
        syscall_hooks_storage.write_user_u64 = copy;
        syscall_hooks_ready = true;
        const clock_state = F.clockevent.testInstallNotifier(F.notify);
        defer F.clockevent.testRestoreState(clock_state);
        copies = 0;
        fail_copy = fail;
        seen = @splat(false);
        expected_events = fd_abi.event_readable;
        const rights = kernel.FdRights{ .poll = true, .read = true, .irq_wait = true, .close = true };
        const fd = if (timer) try state.createTimerFd(F.owner, 100, 0, .{}, rights, 16) else try state.createIrqFd(F.owner, .{ .device = 0x1001, .kind = .msix, .vector = 1 }, rights, .{}, 16);
        const group = try state.beginFdWaitGroup(F.owner, F.tid, F.generation, F.token);
        for (0..kernel.fd_table_entries) |index| {
            try std.testing.expect(try state.registerTaskReadableWaiterForFd(F.owner, fd, fd_abi.event_readable, F.poll_address + index * fd_abi.pollfd_size, F.tid, F.generation, F.token, group));
        }
        try state.armFdWaitGroup(group);
        if (timer) {
            _ = F.clockevent.publish(100);
        } else {
            var owners: [4]kernel.PrincipalId = undefined;
            try std.testing.expectEqual(@as(usize, 1), state.recordDeviceInterruptEvent(0x1001, 1, &owners));
        }
        if (race == .none) {
            F.complete(timer, true);
        } else {
            // Interleave after the entire snapshot, before claim/copy/publish.
            var targets: [32]kernel.ThreadWakeTarget = undefined;
            const count = if (timer) state.takeReadyTimerWaiters(100, &targets) else state.takeReadyIrqWaiters(&targets);
            try std.testing.expectEqual(targets.len, count);
            switch (race) {
                .timeout => {
                    _ = scheduler.wakeIfWaitingTokenGenerationWithRax(F.tid, F.generation, F.owner, F.token, 77);
                    try std.testing.expectEqual(@as(u64, 77), ctx.frame.rax);
                },
                .signal => {
                    ctx.stopped = false;
                    const delivery = scheduler.deliverThreadSignal(F.owner, F.tid, F.generation, 1, 77).?;
                    try std.testing.expect(delivery.was_blocked);
                    try std.testing.expectEqual(@as(u64, 77), ctx.frame.rax);
                },
                .generation => ctx.generation += 1,
                .close => try state.closeFd(F.owner, fd),
                .alias_or => {
                    // Duplicate readiness for one poll item must OR its bits
                    // without inflating the number of completed poll items.
                    const links = state.fd_wait_groups.resolve(.{ .index = group.index, .generation = group.generation }).?.links.?;
                    var iterator = links.iterator();
                    while (iterator.next()) |item| {
                        if (item.value.ready) |*ready| {
                            ready.revents |= fd_abi.event_hangup;
                            if (item.ref.index == kernel.fd_table_entries - 1) ready.pollfd_va = F.poll_address;
                        }
                    }
                    expected_events |= fd_abi.event_hangup;
                },
                .none => unreachable,
            }
            _ = fd_syscalls.wakeThreadTargets(getHooks(), state, &targets);
        }
        if (race == .timeout or race == .signal or race == .generation) {
            try std.testing.expectEqual(@as(usize, 0), copies);
        } else {
            const expected_count = kernel.fd_table_entries - @as(usize, if (race == .alias_or) 1 else 0);
            try std.testing.expectEqual(expected_count, copies);
            try std.testing.expectEqual(@as(u64, if (fail) sc.syscall_err_invalid else expected_count), ctx.frame.rax);
            try std.testing.expect(!ctx.wait_completion_claimed and !ctx.wait_mailbox);
        }
        try std.testing.expectEqual(@as(usize, 0), F.registered(state));
        const completed_copies = copies;
        F.complete(timer, true);
        try std.testing.expectEqual(completed_copies, copies);
    }
};

test "shootdown interrupt guard full 256 target snapshot and copyout failure" {
    for ([_]bool{ false, true }) |timer| {
        try InterruptSnapshotTest.run(timer, false, .none);
        try InterruptSnapshotTest.run(timer, true, .none);
    }
}

test "shootdown interrupt guard snapshot group OR signal timeout close generation races" {
    for ([_]bool{ false, true }) |timer| {
        try InterruptSnapshotTest.run(timer, false, .alias_or);
        try InterruptSnapshotTest.run(timer, false, .timeout);
        try InterruptSnapshotTest.run(timer, false, .signal);
        try InterruptSnapshotTest.run(timer, false, .generation);
        try InterruptSnapshotTest.run(timer, false, .close);
    }
}

test "process timer index admission failure leaves descriptor unpublished and teardown detaches" {
    const Fixture = struct {
        fn fail(_: *anyopaque, _: usize) ?@import("memory/owned_storage.zig").MappedExtent {
            return null;
        }
        fn release(_: *anyopaque, _: @import("memory/owned_storage.zig").PhysicalExtent) bool {
            unreachable;
        }
    };
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    try state.initFromDetectedRegionsInPlace(1);
    defer state.deinitPools();
    state.clearPrincipalState();
    var context: u8 = 0;
    state.pool_allocator = .{ .context = &context, .acquire = Fixture.fail, .release = Fixture.release };
    try std.testing.expect(state.createProcessDescriptor("allocation failure") == null);
    try std.testing.expectEqual(@as(usize, 0), state.active_process_count);
    try std.testing.expectEqual(@as(usize, 0), state.process_timers.records.live);
    state.pool_allocator = null;
    const owner = state.createProcessDescriptor("timer owner").?;
    const generation = state.processDescriptor(owner).?.generation;
    _ = process_timer.set(state, owner, generation, 4, 10, 10, 0);
    process_timer.clear(state, owner, generation); // exec cancellation preserves record
    try std.testing.expect(state.process_timers.take(100) == null);
    _ = process_timer.set(state, owner, generation, 4, 10, 10, 0);
    try std.testing.expect(state.markProcessExited(owner));
    try std.testing.expectEqual(@as(usize, 0), state.process_timers.index.count);
    try std.testing.expectEqual(@as(usize, 0), state.process_timers.records.live);
    try std.testing.expectEqual(owner, state.createProcessDescriptor("reused").?);
    const next = state.processDescriptor(owner).?.generation;
    _ = process_timer.set(state, owner, next, 4, 10, 10, 0);
    process_timer.clear(state, owner, generation);
    try std.testing.expectEqual(@as(usize, 1), state.process_timers.index.count);
    try std.testing.expect(state.markProcessFaulted(owner, 14));
    try std.testing.expectEqual(@as(usize, 0), state.process_timers.index.count);
    try std.testing.expectEqual(@as(usize, 0), state.process_timers.records.live);
}

test "shootdown interrupt guard IPC cancellation survives contention and drains from idle" {
    scheduler.initializeStaticStorage();
    defer scheduler.initializeStaticStorage();
    const owner = InterruptWaiterTest.owner;
    const ctx = scheduler.threadContextMutable(7).?;
    const previous = ctx.*;
    defer ctx.* = previous;
    ctx.* = .{ .id = 7, .allocated = true, .generation = 11, .owner_process = owner, .stopped = true, .wait_mailbox = true, .active_wait_token = 41 };
    try std.testing.expect(kernel.initRuntimeStorage(&InterruptWaiterTest.storage));
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    try state.initFromDetectedRegionsInPlace(1);
    defer state.deinitPools();
    var free_list = kernel.FreePageList{};
    const endpoint = try state.createIpcEndpointFd(owner, kernel.KernelState.endpointRights(), .{}, 16);
    const ref = try state.ipcCall(owner, endpoint, .{}, .{ .owner = owner, .thread_index = 7, .thread_generation = 11, .wait_token = 41 }, &free_list);
    try std.testing.expect(scheduler.bindIpcOutgoing(7, 11, owner, 41, &state.ipcReplySlot(ref).?.outgoing));
    scheduler.detachPrincipalIpcBindings(owner);
    const saved_ready = syscall_hooks_ready;
    const saved_hooks = if (saved_ready) syscall_hooks_storage else undefined;
    defer {
        syscall_hooks_ready = saved_ready;
        syscall_hooks_storage = saved_hooks;
    }
    syscall_hooks_storage = undefined;
    syscall_hooks_storage.state = state;
    syscall_hooks_storage.free_list = &free_list;
    syscall_hooks_storage.kernel_state_ready = &InterruptWaiterTest.state_ready;
    syscall_hooks_ready = true;
    InterruptWaiterTest.state_ready = true;
    const object = state.fdEntryConst(owner, endpoint).?.object;
    try std.testing.expect(kernel_state_lock.tryLock());
    for (0..3) |_| completePendingIrqFdWaitersFromInterrupt(true);
    kernel_state_lock.unlock();
    try std.testing.expect(state.ipcReplySlot(ref) != null);
    try std.testing.expectEqual(@as(u8, 1), (try state.ipcMessageQueueForRecv(object)).len);
    scheduler.testSetCurrentCpuLockFreeIdleWait(true);
    defer scheduler.testSetCurrentCpuLockFreeIdleWait(false);
    completePendingIrqFdWaitersFromInterrupt(false);
    try std.testing.expect(state.ipcReplySlot(ref) == null);
    try std.testing.expectEqual(@as(u8, 0), (try state.ipcMessageQueueForRecv(object)).len);
    completePendingIrqFdWaitersFromInterrupt(false);
    try state.closeFdWithFreeList(owner, endpoint, &free_list);
}

// Host races use the actual K implementation and diagnostic class, so the
// poll regression exercises the exclusion which production already relies on.
pub const KernelLockTestSupport = if (@import("builtin").is_test) struct {
    pub fn tryLock() bool {
        return kernel_state_lock.tryLock();
    }
    pub fn lock() void {
        kernel_state_lock.lock(if (lock_profile.enabled) .other else {});
    }
    pub fn unlock() void {
        kernel_state_lock.unlock();
    }
} else struct {};
