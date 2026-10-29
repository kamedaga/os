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
const smp = @import("smp.zig");
const boot_static = @import("boot/main_static.zig");
const sc = @import("syscall/numbers.zig");
const direct_ipc_entry = @import("arch/x86_64/syscall_entry_code.zig");
const kernel_log = @import("kernel_log.zig");
const kernel_log_abi = @import("kernel_abi_root").kernel_log_abi;
const syscall_lock_policy = @import("syscall/lock_policy.zig");
const capsule_syscalls = @import("syscall/capsule.zig");
const ram_cache_syscalls = @import("syscall/ram_cache.zig");
const fd_syscalls = @import("syscall/fd.zig");
const native_ipc_syscalls = @import("syscall/native_ipc.zig");
const process_syscalls = @import("syscall/process.zig");
const runtime_syscalls = @import("syscall/runtime.zig");
const runtime_nr_profile = @import("runtime_nr_profile.zig");

const TrapFrame = interrupts.TrapFrame;
const preflight_fd_abi = @import("kernel_abi_root").fd_abi;

pub const IovecSnapshot = struct {
    base_va: u64 = 0,
    count: u64 = 0,
    words: [preflight_fd_abi.max_iovecs * 2]u64 = [_]u64{0} ** (preflight_fd_abi.max_iovecs * 2),
};

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
    probe_user_range: ?*const fn (kernel.PrincipalId, u64, usize, bool) user_copy.ProbeResult = null,
    resident_read_user_u32: ?*const fn (kernel.PrincipalId, u64) ?u32 = null,
    futex_lock_address_space: ?*const fn (kernel.PrincipalId) bool = null,
    futex_unlock_address_space: ?*const fn (kernel.PrincipalId) void = null,
    resident_read_user_u32_with_address_space_locked: ?*const fn (kernel.PrincipalId, u64) ?u32 = null,
    resident_atomic_user_u32_with_address_space_locked: ?*const fn (kernel.PrincipalId, u64, @import("kernel_abi_root").runtime_abi.FutexAtomicOperation, u32) ?u32 = null,
    iovec_snapshot: ?*const IovecSnapshot = null,
    pending_signal_frame_snapshot: ?*const process_syscalls.ProcessCloneUserFrame = null,
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
                // Before IF can return: wakes from here to unlock leave after
                // the release instead of extending K by a VM exit each.
                smp.beginKernelStateWakeDeferral();
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
        smp.beginKernelStateWakeDeferral();
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
        // Every publication of this section precedes the release store, so
        // each recorded edge follows the last one it must announce.
        smp.endKernelStateWakeDeferral();
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
    fd_syscalls.completePersistentWaiters(getHooks(), getHooks().state);
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
    lock_profile.parkEnd();
    {
        const cleanup_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.leave_cleanup, cleanup_sample);
        // Nested samples preserve the outer K cleanup total and attribute its
        // work without adding diagnostics to the normal kernel.
        {
            const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            native_ipc_syscalls.drainCancellations(getHooks(), getHooks().state);
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.leave_cancellations, sample);
        }
        {
            const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            fd_syscalls.completePersistentWaiters(getHooks(), getHooks().state);
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.leave_persistent_waiters, sample);
        }
        {
            const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            capsule_syscalls.drainDeviceMemoryRetirements(getHooks().state);
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.leave_device_memory, sample);
        }
        {
            const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
            ram_cache_syscalls.drain(getHooks().state, getHooks().free_list);
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.leave_ram_cache, sample);
        }
    }
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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(irq_fd_completion_pending), &irq_fd_completion_pending));
    end = maxStaticEnd(end, runtime_syscalls.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, runtime_nr_profile.staticEnd());
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
// For IRQ FDs that tick is only the fallback: a deferred device IRQ leaves
// irq_fd_completion_pending set, and the next generic syscall K release
// completes it (see handOffIrqFdWaitersAfterRelease).
fn interruptCanCompleteUserWait(from_user: bool) bool {
    return from_user or scheduler.currentCpuInLockFreeIdleWait();
}

/// Set by a device IRQ after its event counts are published and before it
/// probes K; cleared under K by the completion scan that then observes those
/// counts. It records that IRQ FD waiters may be ready but nobody has scanned
/// for them yet. The scan reads every waiter's published count, so one flag
/// covers all devices; a spurious set only costs one scan.
///
/// Why the K owner, not only the BSP tick: under load the tick's tryLock loses
/// to the same busy K it lost to at the IRQ, so a device completion waited a
/// whole number of 1 ms ticks per lost race (measured: about 20% of NVMe IRQs
/// and most multi-millisecond block I/O stalls while every CPU was idle). The
/// owner that made tryLock fail is the one place guaranteed to leave K soon.
var irq_fd_completion_pending: bool = false;

/// Device-IRQ entry. The seq_cst store is a locked XCHG on x86, so the flag is
/// globally visible before tryLock reads K. handOffIrqFdWaitersAfterRelease
/// fences between its release store and its flag load, which makes the pair a
/// Dekker handshake: either this tryLock observes K free and completes the
/// waiters itself, or the releasing owner observes the flag. A deferral for an
/// ineligible (kernel-mode) interruption keeps the flag for that same owner.
pub fn completeDeviceIrqFdWaitersFromInterrupt(from_user: bool) void {
    @atomicStore(bool, &irq_fd_completion_pending, true, .seq_cst);
    completePendingIrqFdWaitersFromInterrupt(from_user);
}

/// IRQ publication itself is lock-free, but completing an FD wait mutates its
/// generational wait group and must use the same KernelState lock as syscalls.
/// Interrupt context must never spin on a lock held by the interrupted CPU.
/// A device IRQ that loses this try-lock race is completed by the K owner at
/// its release; the BSP timer retries it only as the fallback for owners that
/// release K without that handoff.
pub fn completePendingIrqFdWaitersFromInterrupt(from_user: bool) void {
    if (!interruptCanCompleteUserWait(from_user)) return;
    if (!syscall_hooks_ready) return;
    // Counter publication is already complete. Leave registrations pending
    // for the BSP timer if copyout could reenter this CPU's TLB shootdown.
    if (user_copy.tlbShootdownActiveOnCurrentCpu()) return;
    const h = getHooks();
    if (!h.kernel_state_ready.*) return;
    if (!kernel_state_lock.tryLock()) return;

    native_ipc_syscalls.drainCancellations(h, h.state);
    completePendingIrqFdWaitersLocked(h);
    kernel_state_lock.unlock();
    // This context passed the copyout checks above, so it can also take over
    // a device IRQ that lost tryLock to this hold.
    handOffIrqFdWaitersAfterRelease();
}

/// Under K at a generic syscall release: complete IRQ FD waiters whose device
/// IRQ lost tryLock to this hold, before the release instead of after it.
/// Called after the handler's defers dropped inner AS/shared-VM transactions,
/// where foreign copyout is already used by persistent and data-return drains.
fn completeDeferredIrqFdWaitersLocked(h: *const Hooks) void {
    if (@atomicLoad(bool, &irq_fd_completion_pending, .monotonic))
        completePendingIrqFdWaitersLocked(h);
}

/// After a K release in a context where the K-holding completion is legal
/// (generic syscall release, or an IRQ that passed the copyout checks): pick
/// up a device IRQ that lost tryLock during the hold just released. If K was
/// taken again meanwhile, that owner inherits the work at its own release.
/// Releases that skip this check fall back to the next checked release or the
/// BSP tick: the scalar IPC fast paths (their callees are scalar-gated) and
/// the blocking-leave release, which can run inside a handler that still holds
/// its VM transaction and therefore must not start a foreign copyout.
fn handOffIrqFdWaitersAfterRelease() void {
    if (!syscall_hooks_ready or !getHooks().kernel_state_ready.*) return;
    while (true) {
        // x86 may reorder the plain release store of K with this later load;
        // without the fence both sides could miss each other (see above).
        asm volatile ("mfence" ::: .{ .memory = true });
        if (!@atomicLoad(bool, &irq_fd_completion_pending, .monotonic)) return;
        if (user_copy.tlbShootdownActiveOnCurrentCpu()) return;
        if (!kernel_state_lock.tryLock()) return;
        const h = getHooks();
        native_ipc_syscalls.drainCancellations(h, h.state);
        completePendingIrqFdWaitersLocked(h);
        kernel_state_lock.unlock();
    }
}

// Keep the seed batch out of the admission frame: rejected/nested IRQs
// must not reserve it. The caller retains K through collection and publication,
// so persistent group links preserve the whole ready set across copy batches.
noinline fn completePendingIrqFdWaitersLocked(h: *const Hooks) void {
    // Clear before scanning. Acquire on a set flag synchronizes with the IRQ
    // that published its counts before setting it, so this scan sees them; an
    // IRQ ordered after this exchange leaves the flag set for a later pass.
    _ = @atomicRmw(bool, &irq_fd_completion_pending, .Xchg, false, .acquire);
    var targets: [32]kernel.ThreadWakeTarget = undefined;
    while (true) {
        const count = h.state.takeReadyIrqWaiters(targets[0..]);
        if (count == 0) break;
        _ = fd_syscalls.wakeThreadTargets(h, h.state, targets[0..count]);
    }
    fd_syscalls.completePersistentWaiters(h, h.state);
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
    fd_syscalls.completePersistentWaiters(h, h.state);
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
        sc.syscall_fd_read, sc.syscall_fd_write, sc.syscall_fd_readv, sc.syscall_fd_writev, sc.syscall_getrandom, sc.syscall_futex_wake_op => 0 -% sc.syscall_err_not_ready,
        else => sc.syscall_err_not_ready,
    };
}

fn copyFaultResult(nr: u64) u64 {
    return switch (nr) {
        sc.syscall_fd_read, sc.syscall_fd_write, sc.syscall_fd_readv, sc.syscall_fd_writev, sc.syscall_getrandom, sc.syscall_futex_wake_op => 0 -% sc.syscall_err_map,
        else => sc.syscall_err_map,
    };
}

fn probeBuffer(h: anytype, proc: kernel.PrincipalId, frame: *TrapFrame, va: u64, len: u64, write: bool) ?u64 {
    if (len == 0) return null;
    if (va == 0 or len > std.math.maxInt(usize)) return copyFaultResult(frame.rax);
    const probe = h.probe_user_range orelse return null;
    switch (probe(proc, va, @intCast(len), write)) {
        .resident => return null,
        .invalid_address => return copyFaultResult(frame.rax),
        .backing_absent => |missing_va| {
            frame.rdx = missing_va;
            return sc.syscall_err_pager_absent;
        },
    }
}

fn preflightUserBuffers(h: anytype, proc: kernel.PrincipalId, frame: *TrapFrame, iovecs: *IovecSnapshot, signal_frame: *process_syscalls.ProcessCloneUserFrame) ?u64 {
    const abi = @import("kernel_abi_root");
    const nr = frame.rax;
    // These ranges are examined before K and before any descriptor, pipe,
    // timer or IPC state changes. Pager pages stay resident after supply;
    // a later failed copy can therefore only follow a concurrent VMA change.
    switch (nr) {
        sc.syscall_madvise => {
            const vm = abi.vm_abi;
            if (frame.rdx != vm.madvise_populate_read and frame.rdx != vm.madvise_populate_write) return null;
            if ((frame.rdi & 4095) != 0 or frame.rsi > std.math.maxInt(u64) - 4095)
                return sc.syscall_err_invalid;
            const length = (frame.rsi + 4095) & ~@as(u64, 4095);
            if (frame.rdi > std.math.maxInt(u64) - length) return sc.syscall_err_invalid;
            if (length == 0) return null;
            // Populating is a guarantee, not an optional hint. Resolve before
            // K/VM transactions, using the same pager-absent retry as copyin.
            // A supplied VMO/COW page remains resident through its live refs,
            // including PROT_NONE. A future evictor must preserve this contract
            // or provide residency ownership before reclaiming these pages.
            if (h.probe_user_range == null) return sc.syscall_err_not_ready;
            return probeBuffer(h, proc, frame, frame.rdi, length, frame.rdx == vm.madvise_populate_write);
        },
        sc.syscall_log => return probeBuffer(h, proc, frame, frame.rdi, @min(frame.rsi, sc.user_log_max_bytes), false),
        sc.syscall_kernel_log_read => return probeBuffer(h, proc, frame, frame.rdi, @sizeOf(kernel_log_abi.Read), true),
        sc.syscall_system_info => return probeBuffer(h, proc, frame, frame.rdi, abi.runtime_abi.system_info_size, true),
        sc.syscall_clock_gettime, sc.syscall_clock_getres => return probeBuffer(h, proc, frame, frame.rsi, abi.runtime_abi.timespec_size, true),
        sc.syscall_nanosleep => return probeBuffer(h, proc, frame, frame.rdi, abi.runtime_abi.timespec_size, false),
        sc.syscall_futex_wait => return probeBuffer(h, proc, frame, frame.rdi, 4, false),
        sc.syscall_futex_wake_wait => return probeBuffer(h, proc, frame, frame.rsi, 4, false),
        sc.syscall_futex_wake_op => {
            if (frame.rdi == 0 or (frame.rdi & 3) != 0 or frame.rsi == 0 or (frame.rsi & 3) != 0)
                return 0 -% sc.syscall_err_invalid;
            if (probeBuffer(h, proc, frame, frame.r8, @sizeOf(abi.runtime_abi.FutexWakeOp), false)) |status| return status;
            return probeBuffer(h, proc, frame, frame.rsi, 4, true);
        },
        sc.syscall_getrandom => return probeBuffer(h, proc, frame, frame.rdi, frame.rsi, true),
        sc.syscall_fd_table => return probeBuffer(h, proc, frame, frame.rsi, preflight_fd_abi.fd_table_info_size, true),
        sc.syscall_fd_get_info => return probeBuffer(h, proc, frame, frame.rsi, preflight_fd_abi.fd_info_size, true),
        sc.syscall_fd_stat => return probeBuffer(h, proc, frame, frame.rsi, preflight_fd_abi.stat_size, true),
        sc.syscall_fd_read => return probeBuffer(h, proc, frame, frame.rsi, frame.rdx, true),
        sc.syscall_fd_write => return probeBuffer(h, proc, frame, frame.rsi, frame.rdx, false),
        sc.syscall_fd_readv, sc.syscall_fd_writev => {
            if (frame.rdx == 0 or frame.rdx > preflight_fd_abi.max_iovecs) return sc.syscall_err_invalid;
            if (probeBuffer(h, proc, frame, frame.rsi, frame.rdx * preflight_fd_abi.iovec_size, false)) |status| return status;
            const count: usize = @intCast(frame.rdx);
            var bytes: [preflight_fd_abi.max_iovecs * preflight_fd_abi.iovec_size]u8 = undefined;
            if (!h.copy_user_bytes_from_va(proc, frame.rsi, bytes[0 .. count * preflight_fd_abi.iovec_size])) return sc.syscall_err_invalid;
            iovecs.base_va = frame.rsi;
            iovecs.count = frame.rdx;
            for (0..count) |i| {
                const offset = i * preflight_fd_abi.iovec_size;
                const base = std.mem.readInt(u64, bytes[offset..][0..8], .little);
                const len = std.mem.readInt(u64, bytes[offset + 8 ..][0..8], .little);
                iovecs.words[2 * i] = base;
                iovecs.words[2 * i + 1] = len;
                if (probeBuffer(h, proc, frame, base, len, nr == sc.syscall_fd_readv)) |status| return status;
            }
            return null;
        },
        sc.syscall_fd_poll, sc.syscall_fd_wait_many => {
            if (frame.rsi > preflight_fd_abi.max_pollfds) return sc.syscall_err_invalid;
            return probeBuffer(h, proc, frame, frame.rdi, frame.rsi * preflight_fd_abi.pollfd_size, true);
        },
        sc.syscall_fd_ioctl => {
            if (frame.rsi == preflight_fd_abi.ioctl_tiocgwinsz)
                return probeBuffer(h, proc, frame, frame.rdx, preflight_fd_abi.winsize_size, true);
            return null;
        },
        sc.syscall_pipe_create => return probeBuffer(h, proc, frame, frame.rdi, preflight_fd_abi.pipe_pair_size, true),
        sc.syscall_timerfd_settime => {
            if (probeBuffer(h, proc, frame, frame.rdx, preflight_fd_abi.timerfd_spec_size, false)) |status| return status;
            if (frame.r10 != 0) return probeBuffer(h, proc, frame, frame.r10, preflight_fd_abi.timerfd_spec_size, true);
            return null;
        },
        sc.syscall_timerfd_gettime => return probeBuffer(h, proc, frame, frame.rsi, preflight_fd_abi.timerfd_spec_size, true),
        sc.syscall_process_create => {
            if (frame.r8 > abi.process_abi.process_create_max_grants) return sc.syscall_err_invalid;
            return probeBuffer(h, proc, frame, frame.r10, frame.r8 * @sizeOf(abi.process_abi.ProcessFdGrant), false);
        },
        sc.syscall_process_clone => {
            if ((frame.rsi & abi.process_abi.process_clone_flag_user_frame) == 0) return null;
            return probeBuffer(h, proc, frame, frame.rdx, abi.process_abi.process_clone_user_frame_size, false);
        },
        sc.syscall_process_wait, sc.syscall_thread_wait => return probeBuffer(h, proc, frame, frame.rsi, abi.process_abi.status_size, true),
        sc.syscall_thread_context => return probeBuffer(h, proc, frame, frame.rdx, abi.process_abi.thread_user_context_size, frame.rsi == abi.process_abi.thread_context_get),
        sc.syscall_process_map_batch => {
            if (frame.rdx > abi.process_abi.process_map_batch_max_entries) return sc.syscall_err_invalid;
            return probeBuffer(h, proc, frame, frame.rsi, frame.rdx * abi.process_abi.process_map_batch_entry_size, false);
        },
        sc.syscall_process_signal_ctl => {
            return switch (frame.rdi) {
                abi.process_abi.signal_ctl_register => probeBuffer(h, proc, frame, frame.rsi, @sizeOf(abi.process_abi.SignalRegistration), false),
                abi.process_abi.signal_ctl_return => probeBuffer(h, proc, frame, frame.rsi, abi.process_abi.signal_frame_size, false),
                abi.process_abi.signal_ctl_deliver_pending_frame => blk: {
                    if (probeBuffer(h, proc, frame, frame.rsi, abi.process_abi.process_clone_user_frame_size, false)) |status| break :blk status;
                    if (!h.copy_user_bytes_from_va(proc, frame.rsi, std.mem.asBytes(signal_frame))) break :blk copyFaultResult(nr);
                    const stack = scheduler.currentSignalStack() orelse break :blk null;
                    if (@import("signal_stack.zig").contains(stack.base, stack.size, frame.rsp))
                        break :blk sc.syscall_err_invalid;
                    const destination = @import("signal_stack.zig").layout(stack.base, stack.size, signal_frame.rsp) orelse break :blk sc.syscall_err_invalid;
                    break :blk probeBuffer(h, proc, frame, destination.frame_va, abi.process_abi.signal_frame_size, true);
                },
                abi.process_abi.signal_ctl_get_timer => probeBuffer(h, proc, frame, frame.rsi, abi.process_abi.signal_timer_state_size, true),
                abi.process_abi.signal_ctl_set_timer => if (frame.r8 == 0) null else probeBuffer(h, proc, frame, frame.r8, abi.process_abi.signal_timer_state_size, true),
                else => null,
            };
        },
        sc.syscall_vmo_collect_dirty => {
            if (frame.rdx > (kernel.max_vmo_logical_pages + 63) / 64) return sc.syscall_err_invalid;
            return probeBuffer(h, proc, frame, frame.rsi, frame.rdx * 8, true);
        },
        sc.syscall_vmo_create_page_view, sc.syscall_vmo_refresh_page_view => {
            // The index list is read into unpublished metadata before any
            // VMO becomes visible. Keep every page of it resident first.
            const count = if (nr == sc.syscall_vmo_create_page_view) frame.rdx else frame.r10;
            if (count > kernel.max_vmo_logical_pages) return sc.syscall_err_invalid;
            const indices_va = if (nr == sc.syscall_vmo_create_page_view) frame.rsi else frame.rdx;
            return probeBuffer(h, proc, frame, indices_va, count * preflight_fd_abi.vmo_page_view_index_size, false);
        },
        sc.syscall_pager_supply => return probeBuffer(h, proc, frame, frame.rsi, @sizeOf(abi.pager_abi.Supply), false),
        sc.syscall_ipc_channel_create => return probeBuffer(h, proc, frame, frame.rdi, 16, true),
        sc.syscall_ipc_send, sc.syscall_ipc_recv, sc.syscall_ipc_call, sc.syscall_ipc_reply, sc.syscall_ipc_reply_recv, sc.syscall_ipc_recv_wait => {
            const send_count = abi.ipc_abi.sendCount(frame);
            const recv_count = abi.ipc_abi.receiveCapacity(frame);
            if (send_count > abi.ipc_abi.max_transfer_fds or recv_count > abi.ipc_abi.max_transfer_fds)
                return sc.syscall_err_invalid;
            if (probeBuffer(h, proc, frame, frame.r13, send_count * abi.ipc_abi.fd_item_size, false)) |status| return status;
            return probeBuffer(h, proc, frame, frame.rbx, recv_count * abi.ipc_abi.fd_item_size, true);
        },
        sc.syscall_capsule_query => return probeBuffer(h, proc, frame, frame.rsi, abi.capsule_abi.snapshot_word_count * 8, true),
        sc.syscall_capsule_irq_route => return probeBuffer(h, proc, frame, frame.rsi, abi.capsule_abi.irq_route_word_count * 8, true),
        sc.syscall_capsule_irq_poll => return probeBuffer(h, proc, frame, frame.rdx, 8, true),
        sc.syscall_capsule_pci_enumerate => return probeBuffer(h, proc, frame, frame.rsi, if (frame.rdi == std.math.maxInt(u64)) 8 else abi.capsule_abi.pci_function_word_count * 8, true),
        sc.syscall_capsule_pci_config_read, sc.syscall_capsule_pci_config_write => return probeBuffer(h, proc, frame, frame.rdx, @min(frame.r10, 4), nr == sc.syscall_capsule_pci_config_read),
        sc.syscall_capsule_pci_bar_info => return probeBuffer(h, proc, frame, frame.rdx, abi.capsule_abi.bar_info_word_count * 8, true),
        sc.syscall_capsule_derive_dma_mapping_pages => {
            const layout = abi.capsule_abi.dmaMappingPagesLayout(frame.rsi, frame.rdx) orelse return sc.syscall_err_invalid;
            return probeBuffer(h, proc, frame, frame.r8, layout.output_size_bytes, true);
        },
        sc.syscall_capsule_derive_mmio => {
            if ((frame.r8 & abi.capsule_abi.mmio_map_flag_export_vmo) == 0) return null;
            if (frame.r10 / 4096 > kernel.max_vmo_logical_pages) return sc.syscall_err_invalid;
            return probeBuffer(h, proc, frame, frame.rdx, (frame.r10 / 4096) * 8, false);
        },
        else => return null,
    }
}

test "pager preflight reports absent VA before a side effect" {
    try std.testing.expectEqual(0 -% sc.syscall_err_map, copyFaultResult(sc.syscall_fd_read));
    try std.testing.expectEqual(sc.syscall_err_map, copyFaultResult(sc.syscall_fd_stat));
    const Mock = struct {
        var probes: usize = 0;
        var copies: usize = 0;
        fn probe(_: kernel.PrincipalId, va: u64, len: usize, write: bool) user_copy.ProbeResult {
            probes += 1;
            std.debug.assert(write and len == 8192 and va == 0x1000);
            return .{ .backing_absent = 0x2000 };
        }
        fn copy(_: kernel.PrincipalId, _: u64, _: []u8) bool {
            copies += 1;
            return true;
        }
    };
    Mock.probes = 0;
    Mock.copies = 0;
    const h = .{
        .probe_user_range = @as(?*const fn (kernel.PrincipalId, u64, usize, bool) user_copy.ProbeResult, Mock.probe),
        .copy_user_bytes_from_va = Mock.copy,
    };
    var frame = std.mem.zeroes(TrapFrame);
    frame.rax = sc.syscall_fd_read;
    frame.rsi = 0x1000;
    frame.rdx = 8192;
    var iovecs = IovecSnapshot{};
    var signal_frame: process_syscalls.ProcessCloneUserFrame = undefined;
    try std.testing.expectEqual(sc.syscall_err_pager_absent, preflightUserBuffers(&h, @enumFromInt(1), &frame, &iovecs, &signal_frame).?);
    try std.testing.expectEqual(@as(u64, 0x2000), frame.rdx);
    try std.testing.expectEqual(@as(usize, 1), Mock.probes);
    try std.testing.expectEqual(@as(usize, 0), Mock.copies);
}

test "iovec preflight captures one immutable descriptor before transfer" {
    const Mock = struct {
        var copies: usize = 0;
        fn probe(_: kernel.PrincipalId, _: u64, _: usize, _: bool) user_copy.ProbeResult {
            return .resident;
        }
        fn copy(_: kernel.PrincipalId, va: u64, dest: []u8) bool {
            std.debug.assert(va == 0x3000 and dest.len == 16);
            copies += 1;
            std.mem.writeInt(u64, dest[0..8], 0x5000, .little);
            std.mem.writeInt(u64, dest[8..16], 8192, .little);
            return true;
        }
    };
    Mock.copies = 0;
    const h = .{
        .probe_user_range = @as(?*const fn (kernel.PrincipalId, u64, usize, bool) user_copy.ProbeResult, Mock.probe),
        .copy_user_bytes_from_va = Mock.copy,
    };
    var frame = std.mem.zeroes(TrapFrame);
    frame.rax = sc.syscall_fd_writev;
    frame.rsi = 0x3000;
    frame.rdx = 1;
    var iovecs = IovecSnapshot{};
    var signal_frame: process_syscalls.ProcessCloneUserFrame = undefined;
    try std.testing.expect(preflightUserBuffers(&h, @enumFromInt(1), &frame, &iovecs, &signal_frame) == null);
    try std.testing.expectEqual(@as(usize, 1), Mock.copies);
    try std.testing.expectEqual(@as(u64, 0x5000), iovecs.words[0]);
    try std.testing.expectEqual(@as(u64, 8192), iovecs.words[1]);
}

test "pager preflight populate validates access and preserves retry address" {
    const Mock = struct {
        var writes: bool = false;
        var calls: usize = 0;
        var outcome: user_copy.ProbeResult = .resident;
        fn probe(_: kernel.PrincipalId, va: u64, len: usize, write: bool) user_copy.ProbeResult {
            std.debug.assert(va == 0x1000 and len == 8192);
            calls += 1;
            writes = write;
            return outcome;
        }
        fn copy(_: kernel.PrincipalId, _: u64, _: []u8) bool {
            unreachable;
        }
    };
    const Probe = *const fn (kernel.PrincipalId, u64, usize, bool) user_copy.ProbeResult;
    const h = .{ .probe_user_range = @as(?Probe, Mock.probe), .copy_user_bytes_from_va = Mock.copy };
    const missing = .{ .probe_user_range = @as(?Probe, null), .copy_user_bytes_from_va = Mock.copy };
    var iovecs = IovecSnapshot{};
    var signal_frame: process_syscalls.ProcessCloneUserFrame = undefined;
    var frame = std.mem.zeroes(TrapFrame);
    frame.rax = sc.syscall_madvise;
    frame.rdi = 0x1000;
    frame.rsi = 4097;
    const vm = @import("kernel_abi_root").vm_abi;
    Mock.calls = 0;
    Mock.outcome = .resident;
    for ([_]u64{ vm.madvise_populate_read, vm.madvise_populate_write }) |advice| {
        frame.rdx = advice;
        try std.testing.expect(preflightUserBuffers(&h, @enumFromInt(1), &frame, &iovecs, &signal_frame) == null);
        try std.testing.expectEqual(advice == vm.madvise_populate_write, Mock.writes);
    }
    Mock.outcome = .{ .backing_absent = 0x2000 };
    try std.testing.expectEqual(sc.syscall_err_pager_absent, preflightUserBuffers(&h, @enumFromInt(1), &frame, &iovecs, &signal_frame).?);
    try std.testing.expectEqual(@as(u64, 0x2000), frame.rdx);
    frame.rdx = vm.madvise_populate_write;
    try std.testing.expectEqual(sc.syscall_err_not_ready, preflightUserBuffers(&missing, @enumFromInt(1), &frame, &iovecs, &signal_frame).?);
    const calls = Mock.calls;
    frame.rdi += 1;
    try std.testing.expectEqual(sc.syscall_err_invalid, preflightUserBuffers(&h, @enumFromInt(1), &frame, &iovecs, &signal_frame).?);
    frame.rdi = 0x1000;
    frame.rsi = std.math.maxInt(u64);
    try std.testing.expectEqual(sc.syscall_err_invalid, preflightUserBuffers(&h, @enumFromInt(1), &frame, &iovecs, &signal_frame).?);
    frame.rdi = std.math.maxInt(u64) & ~@as(u64, 4095);
    frame.rsi = 8192;
    try std.testing.expectEqual(sc.syscall_err_invalid, preflightUserBuffers(&h, @enumFromInt(1), &frame, &iovecs, &signal_frame).?);
    try std.testing.expectEqual(calls, Mock.calls);
}

test "lifecycle admission does not report byte progress for a rejected syscall" {
    const not_ready: u64 = 0 -% sc.syscall_err_not_ready;
    for ([_]u64{ sc.syscall_fd_read, sc.syscall_fd_write, sc.syscall_fd_readv, sc.syscall_fd_writev, sc.syscall_getrandom, sc.syscall_futex_wake_op }) |nr| {
        try std.testing.expectEqual(not_ready, lifecycleAdmissionRejectedResult(nr));
    }
    for ([_]u64{ sc.syscall_process_signal_ctl, sc.syscall_fd_close, sc.syscall_ipc_notify, sc.syscall_futex_wait }) |nr| {
        try std.testing.expectEqual(sc.syscall_err_not_ready, lifecycleAdmissionRejectedResult(nr));
    }
}

var reference_audit_calls: if (@import("state/reference_check.zig").enabled) u64 else void = if (@import("state/reference_check.zig").enabled) 0 else {};
var reference_audit_count: if (@import("state/reference_check.zig").enabled) u64 else void = if (@import("state/reference_check.zig").enabled) 0 else {};

fn dispatchCompactSyscall(frame: *TrapFrame) u64 {
    const nr = frame.rax;
    // Only wake-relevant syscall entry/exit markers enter the diagnostic
    // ring. General call markers would bury a request in GUI background IO.
    const trace_call = @import("wake_trace.zig").enabled and (nr == sc.syscall_futex_wait or nr == sc.syscall_futex_wake or
        nr == sc.syscall_futex_wake_wait or nr == sc.syscall_ipc_call or
        nr == sc.syscall_ipc_reply or nr == sc.syscall_ipc_notify or
        nr == sc.syscall_ipc_notify_wait or nr == sc.syscall_ipc_recv_wait or
        nr == sc.syscall_process_signal_ctl or nr == sc.syscall_gettid or
        nr == sc.syscall_fd_read or nr == sc.syscall_fd_write or
        nr == sc.syscall_fd_wait_many or nr == sc.syscall_fd_poll);
    const trace_thread = if (trace_call) scheduler.currentThread() else 0;
    const trace_arg = if (trace_call) frame.rdi else 0;
    if (trace_call) @import("wake_trace.zig").record(.syscall_enter, nr, trace_arg, trace_thread);
    defer if (trace_call) @import("wake_trace.zig").record(.syscall_exit, nr, trace_arg, trace_thread);
    user_copy.beginSyscallCopyTracking(scheduler.currentPrincipal());
    const result = dispatchCompactSyscallBody(frame);
    if (user_copy.takeSyscallCopyAbsent()) |absent| {
        // A covered VA became absent only after another thread replaced the
        // mapping. Retrying the syscall could duplicate an earlier effect.
        if (absent.was_probed) return copyFaultResult(nr);
        if (std.debug.runtime_safety) @panic("syscall copied a pager page without a preflight probe");
        frame.rdx = absent.va;
        return sc.syscall_err_pager_absent;
    }
    return result;
}

fn dispatchCompactSyscallBody(frame: *TrapFrame) u64 {
    const generic_admission = b3.now();
    const runtime_path = perf.runtimePath(frame.rax, frame.rdi);
    const profile_kind = if (lock_profile.enabled) lock_profile.classify(frame.rax) else {};
    if (frame.rax == sc.syscall_clock_read) frame.rdx = 0;
    // Admission may reject before runtime dispatch. Output-only RDX must
    // still distinguish an unperformed wake from a committed wake count.
    if (frame.rax == sc.syscall_futex_wake_wait) frame.rdx = std.math.maxInt(u64);
    const base_hooks = getHooks();
    if (!base_hooks.kernel_state_ready.*) return sc.syscall_err_not_ready;
    if (comptime lock_profile.enabled) runtime_nr_profile.record(frame.rax);
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
    var iovec_snapshot = IovecSnapshot{};
    var pending_signal_frame_snapshot: process_syscalls.ProcessCloneUserFrame = undefined;
    if (preflightUserBuffers(base_hooks, proc, frame, &iovec_snapshot, &pending_signal_frame_snapshot)) |status| return status;
    @import("ipc_metric.zig").recordSyscall(state, frame.rax, proc);
    @import("notify_metric.zig").begin(frame.rax, @intFromEnum(proc), scheduler.currentThread(), scheduler.currentThreadGeneration() orelse 0);
    defer @import("notify_metric.zig").finish();
    // Recorded on entry, not at deschedule: the trap frame's rax carries the
    // syscall number in and the result out, so by the time a thread is placed
    // again the number is gone.  rdi is kept alongside because it is the fd or
    // futex address the spin is going around.
    // A published clock source cannot be withdrawn during this syscall.
    // Clock copyout uses the address-space/shared-VM transaction used by
    // preflight and by the existing K-free nanosleep copy; neither path adds
    // K-only retirement work. An unready source retains K for CMOS/tick use.
    const hold_kernel_state_lock = syscall_lock_policy.needsKernelStateLock(frame.rax) and
        !(frame.rax == sc.syscall_clock_gettime and runtime_syscalls.clockGettimeMaySkipK(frame.rdi));
    if (comptime lock_profile.enabled) {
        if (hold_kernel_state_lock) runtime_nr_profile.recordLocked(frame.rax);
    }
    var lock_held = false;
    var lock_drop_context = KernelStateLockDropContext{ .lock_held = &lock_held };
    if (lock_profile.enabled) lock_drop_context.kind = profile_kind;
    if (comptime lock_profile.enabled) {
        if (!hold_kernel_state_lock) lock_profile.event(profile_kind, 0);
    }
    var hooks = base_hooks.*;
    hooks.iovec_snapshot = if (iovec_snapshot.count != 0) &iovec_snapshot else null;
    hooks.pending_signal_frame_snapshot = if (frame.rax == sc.syscall_process_signal_ctl and
        frame.rdi == @import("kernel_abi_root").process_abi.signal_ctl_deliver_pending_frame)
        &pending_signal_frame_snapshot
    else
        null;
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
            {
                const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
                @import("memory/user_vm.zig").retryRetiredAddressSpaces();
                if (comptime lock_profile.enabled) lock_profile.stageEnd(.retire_address_spaces, sample);
            }
            {
                const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
                state.retryInvocationStorage();
                if (comptime lock_profile.enabled) lock_profile.stageEnd(.retire_invocation_storage, sample);
            }
            {
                const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
                state.retryReferenceStorage();
                if (comptime lock_profile.enabled) lock_profile.stageEnd(.retire_reference_storage, sample);
            }
            {
                const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
                native_ipc_syscalls.drainCancellations(h, state);
                if (comptime lock_profile.enabled) lock_profile.stageEnd(.retire_cancellations, sample);
            }
            {
                const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
                capsule_syscalls.drainDeviceMemoryRetirements(state);
                if (comptime lock_profile.enabled) lock_profile.stageEnd(.retire_device_memory, sample);
            }
            {
                const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
                ram_cache_syscalls.drain(state, h.free_list);
                if (comptime lock_profile.enabled) lock_profile.stageEnd(.retire_ram_cache, sample);
            }
            // A device IRQ that lost tryLock to this hold completes here,
            // ahead of the persistent drain that delivers retained sets.
            completeDeferredIrqFdWaitersLocked(h);
            // E guards have ended. Complete retained native subscriptions
            // under K before publishing any user return or releasing storage.
            {
                const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
                fd_syscalls.completePersistentWaiters(h, state);
                if (comptime lock_profile.enabled) lock_profile.stageEnd(.retire_persistent_waiters, sample);
            }
            {
                const sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
                if (state.hasPendingDataReturns()) {
                    // Never acquire all-AS while a syscall's inner AS/shared
                    // transaction is held. Handler defers have run before this
                    // outer drain, and KernelState still excludes VMO finalizers.
                    @import("memory/user_vm.zig").lockAllVmTransactions();
                    state.drainPendingDataReturns();
                    @import("memory/user_vm.zig").unlockAllVmTransactions();
                }
                if (comptime lock_profile.enabled) lock_profile.stageEnd(.retire_pending_returns, sample);
            }
            const audit_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
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
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.retire_reference_audit, audit_sample);
            if (comptime lock_profile.enabled) lock_profile.stageEnd(.retire, retire_sample);
            kernel_state_lock.unlock();
            handOffIrqFdWaitersAfterRelease();
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
// Branches on the same table that generated the entry routing, so a renumbered
// ABI cannot route a syscall here that prepare would misclassify.
pub export fn syscallDirectIpcPrepare(frame: *const TrapFrame) callconv(.winapi) u64 {
    if (@import("builtin").is_test) return 0;
    const kind = direct_ipc_entry.directIpcKind(frame.rax) orelse return 0;
    // A receive FD array needs an AS probe before K can publish the wait.
    // The compact path owns that probe and can return PAGER_ABSENT + VA.
    if ((kind == .call or kind == .reply_recv) and
        @import("kernel_abi_root").ipc_abi.receiveCapacity(frame) != 0) return 0;
    if (kind == .notify or kind == .notify_wait) return prepareDirectNotifyEntry(frame);
    const metric = @import("ipc_metric.zig");
    metric.entryBegin();
    var census_success = false;
    defer metric.entryEnd(census_success);
    metric.recordElapsed(if (kind == .call) .entry_call_attempt else .entry_reply_attempt, 0);
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
    const call = kind == .call;
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
    // Reached only after prepare admitted this frame, so the kind exists.
    switch (direct_ipc_entry.directIpcKind(frame.rax).?) {
        .notify, .notify_wait => return commitDirectNotifyEntry(frame),
        .call, .reply_recv => {},
    }
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

    /// Who completes the waiter once the deferral checks have run: an eligible
    /// interrupt, or (IRQ FDs only) the K owner whose hold made a device IRQ
    /// lose tryLock, either before its release or right after it.
    const Completion = enum { interrupt, owner_drain, release_handoff };

    fn run(timer: bool, kernel_interruption: bool, resume_idle: bool, completion: Completion) !void {
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
        const fd = if (timer) try state.createTimerFd(owner, 100, 0, .{}, rights, 16) else try state.createIrqFd(owner, .{ .source = .{ .pci = 0x1001 }, .kind = .msix, .vector = 1 }, rights, .{}, 16);
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
        switch (completion) {
            .interrupt => complete(timer, !resume_idle),
            .owner_drain, .release_handoff => {
                std.debug.assert(!timer);
                // An eligible device IRQ that loses tryLock to a K owner.
                try std.testing.expect(kernel_state_lock.tryLock());
                completeDeviceIrqFdWaitersFromInterrupt(true);
                try std.testing.expectEqual(@as(usize, 1), registered(state));
                if (completion == .owner_drain) {
                    completeDeferredIrqFdWaitersLocked(getHooks());
                    try std.testing.expectEqual(@as(usize, 0), registered(state));
                    kernel_state_lock.unlock();
                } else {
                    // A release without the handoff (a fast path) leaves it
                    // pending; only the checked release completes it.
                    kernel_state_lock.unlock();
                    try std.testing.expectEqual(@as(usize, 1), registered(state));
                    handOffIrqFdWaitersAfterRelease();
                }
                try std.testing.expect(!irq_fd_completion_pending);
            },
        }
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
    try InterruptWaiterTest.run(true, false, false, .interrupt);
}

test "shootdown interrupt guard preserves IRQ waiters until one completion" {
    try InterruptWaiterTest.run(false, false, false, .interrupt);
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
    try InterruptWaiterTest.run(true, true, false, .interrupt);
}

test "shootdown interrupt guard kernel IRQ defers IRQ FD until user tick" {
    try InterruptWaiterTest.run(false, true, false, .interrupt);
}

test "shootdown interrupt guard kernel IRQ defers timer FD until idle tick" {
    try InterruptWaiterTest.run(true, true, true, .interrupt);
}

test "shootdown interrupt guard kernel IRQ defers IRQ FD until idle tick" {
    try InterruptWaiterTest.run(false, true, true, .interrupt);
}

test "device IRQ losing K is completed by that owner before its release" {
    try InterruptWaiterTest.run(false, false, false, .owner_drain);
}

test "device IRQ losing K is completed by the owner's checked release" {
    try InterruptWaiterTest.run(false, true, false, .release_handoff);
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
        const fd = if (timer) try state.createTimerFd(F.owner, 100, 0, .{}, rights, 16) else try state.createIrqFd(F.owner, .{ .source = .{ .pci = 0x1001 }, .kind = .msix, .vector = 1 }, rights, .{}, 16);
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
