const std = @import("std");
const builtin = @import("builtin");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const interrupts = @import("interrupts.zig");
const smp = @import("smp.zig");
const build_workarounds = @import("build_workarounds");
const scheduler_observer = @import("scheduler_observer.zig");
const x86_platform = @import("arch/x86_64/platform.zig");

const TrapFrame = interrupts.TrapFrame;
const UserAddressSpace = capability.UserAddressSpace;
const default_process_principal: kernel.PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
const bootstrap_cpu_slot: usize = 0;
const all_cpu_affinity_mask: u64 = if (smp.max_cpus >= 64) std.math.maxInt(u64) else (@as(u64, 1) << smp.max_cpus) - 1;

const fx_state_bytes: usize = 512;
pub const max_thread_slots: usize = kernel.max_thread_slots;
pub const max_ipc_queue_depth: usize = 8;
pub const idle_thread_marker: usize = max_thread_slots;
pub const handoff_validation_none: u8 = 0;
pub const handoff_validation_ok: u8 = 1;
pub const handoff_validation_bad_thread: u8 = 2;
pub const handoff_validation_not_allocated: u8 = 3;
pub const handoff_validation_not_ready: u8 = 4;
pub const handoff_validation_wrong_cpu: u8 = 5;
pub const handoff_validation_bad_affinity: u8 = 6;
pub const handoff_validation_bad_owner: u8 = 7;
pub const handoff_validation_bad_cr3: u8 = 8;
pub const handoff_validation_bad_frame: u8 = 9;

pub const IpcQueuedMessage = struct {
    endpoint_id: u64 = 0,
    sender_thread: usize = 0,
    grants_reply: bool = false,
    mr0: u64 = 0,
    mr1: u64 = 0,
    mr2: u64 = 0,
    mr3: u64 = 0,
};

const IpcQueue = struct {
    entries: [max_ipc_queue_depth]IpcQueuedMessage = [_]IpcQueuedMessage{.{}} ** max_ipc_queue_depth,
    head: u8 = 0,
    len: u8 = 0,
};

fn schedulerLocksMaskInterrupts() bool {
    return !builtin.is_test;
}

fn interruptsEnabled() bool {
    if (!schedulerLocksMaskInterrupts()) return false;
    var flags: u64 = 0;
    asm volatile ("pushfq; pop %[out]"
        : [out] "=r" (flags),
    );
    return (flags & (@as(u64, 1) << 9)) != 0;
}

fn disableInterruptsForSchedulerLock() void {
    if (!schedulerLocksMaskInterrupts()) return;
    asm volatile ("cli" ::: .{ .memory = true });
}

fn enableInterruptsForSchedulerLock() void {
    if (!schedulerLocksMaskInterrupts()) return;
    asm volatile ("sti" ::: .{ .memory = true });
}

const SchedulerSpinLock = struct {
    value: u8 = 0,
    interrupts_were_enabled: bool = false,

    fn lock(self: *SchedulerSpinLock) void {
        const restore_interrupts = interruptsEnabled();
        disableInterruptsForSchedulerLock();
        while (true) {
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) {
                self.interrupts_were_enabled = restore_interrupts;
                return;
            }
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                asm volatile ("pause");
            }
        }
    }

    fn unlock(self: *SchedulerSpinLock) void {
        const restore_interrupts = self.interrupts_were_enabled;
        @atomicStore(u8, &self.value, 0, .release);
        if (restore_interrupts) enableInterruptsForSchedulerLock();
    }
};

const RunQueue = struct {
    runnable: [max_thread_slots]bool = [_]bool{false} ** max_thread_slots,
    len: usize = 0,

    fn markRunnable(self: *RunQueue, thread_index: usize) void {
        if (thread_index >= max_thread_slots) return;
        if (!self.runnable[thread_index]) {
            self.runnable[thread_index] = true;
            self.len += 1;
        }
    }

    fn markBlocked(self: *RunQueue, thread_index: usize) void {
        if (thread_index >= max_thread_slots) return;
        if (self.runnable[thread_index]) {
            self.runnable[thread_index] = false;
            self.len -= 1;
        }
    }

    fn contains(self: *const RunQueue, thread_index: usize) bool {
        return thread_index < max_thread_slots and self.runnable[thread_index];
    }

    fn pickFirst(self: *const RunQueue) ?usize {
        var i: usize = 0;
        while (i < max_thread_slots) : (i += 1) {
            if (self.contains(i)) return i;
        }
        return null;
    }

    fn pickNextAfter(self: *const RunQueue, current_index: usize) usize {
        if (current_index >= max_thread_slots) return self.pickFirst() orelse 0;
        var step: usize = 1;
        while (step <= max_thread_slots) : (step += 1) {
            const idx = (current_index + step) % max_thread_slots;
            if (self.contains(idx)) return idx;
        }
        return current_index;
    }
};

pub const CpuSchedulerInfo = struct {
    slot: usize,
    smp_state: smp.CpuState,
    current_thread: usize,
    current_principal: ?kernel.PrincipalId,
    current_cr3: u64,
    idle_thread: usize,
    runnable_count: usize,
    idle_ticks: u64,
    observer_ticks: u64,
    observed_runnable_ticks: u64,
    observed_runnable_count: usize,
    observed_next_thread: ?usize,
    handoff_ticks: u64,
    handoff_runnable_count: usize,
    pending_handoff_thread: ?usize,
    handoff_validation_ticks: u64,
    handoff_validation_failures: u64,
    validated_handoff_thread: ?usize,
    handoff_validation_code: u8,
    handoff_consume_ticks: u64,
    consumed_handoff_thread: ?usize,
    handoff_snapshot_ticks: u64,
    handoff_snapshot_failures: u64,
    snapshot_handoff_thread: ?usize,
    snapshot_cr3: u64,
    snapshot_fs_base: u64,
    snapshot_rip: u64,
    snapshot_rsp: u64,
    snapshot_rflags: u64,
    snapshot_cs: u64,
    snapshot_ss: u64,
    handoff_user_entry_ticks: u64,
    handoff_user_entry_failures: u64,
    entered_handoff_thread: ?usize,
    user_entry_requested: bool,
    ap_timer_save_ticks: u64,
    ap_timer_saved_thread: ?usize,
    ap_timer_saved_rip: u64,
    ap_timer_saved_rsp: u64,
    ap_timer_requeue_ticks: u64,
    ap_preempt_switch_ticks: u64,
    ap_preempt_from_thread: ?usize,
    ap_preempt_to_thread: ?usize,
    ap_user_entry_target_cycles: u64,
    schedule_ticks: u64,
    enabled: bool,
    accepts_runnable: bool,
    runnable_acceptance_requested: bool,
    is_idle: bool,
};

const CpuSchedulerState = struct {
    lock: SchedulerSpinLock = .{},
    run_queue: RunQueue = .{},
    current_thread: usize = idle_thread_marker,
    current_principal: ?kernel.PrincipalId = null,
    current_cr3: u64 = 0,
    idle_thread: usize = idle_thread_marker,
    idle_ticks: u64 = 0,
    observer_ticks: u64 = 0,
    observed_runnable_ticks: u64 = 0,
    observed_runnable_count: usize = 0,
    observed_next_thread: ?usize = null,
    handoff_ticks: u64 = 0,
    handoff_runnable_count: usize = 0,
    pending_handoff_thread: ?usize = null,
    handoff_validation_ticks: u64 = 0,
    handoff_validation_failures: u64 = 0,
    validated_handoff_thread: ?usize = null,
    handoff_validation_code: u8 = handoff_validation_none,
    handoff_consume_ticks: u64 = 0,
    consumed_handoff_thread: ?usize = null,
    handoff_snapshot_ticks: u64 = 0,
    handoff_snapshot_failures: u64 = 0,
    snapshot_handoff_thread: ?usize = null,
    snapshot_cr3: u64 = 0,
    snapshot_fs_base: u64 = 0,
    snapshot_rip: u64 = 0,
    snapshot_rsp: u64 = 0,
    snapshot_rflags: u64 = 0,
    snapshot_cs: u64 = 0,
    snapshot_ss: u64 = 0,
    handoff_user_entry_ticks: u64 = 0,
    handoff_user_entry_failures: u64 = 0,
    entered_handoff_thread: ?usize = null,
    user_entry_requested: bool = false,
    ap_timer_save_ticks: u64 = 0,
    ap_timer_saved_thread: ?usize = null,
    ap_timer_saved_rip: u64 = 0,
    ap_timer_saved_rsp: u64 = 0,
    ap_timer_requeue_ticks: u64 = 0,
    ap_preempt_switch_ticks: u64 = 0,
    ap_preempt_from_thread: ?usize = null,
    ap_preempt_to_thread: ?usize = null,
    ap_user_entry_target_cycles: u64 = 0,
    schedule_ticks: u64 = 0,
    enabled: bool = false,
    accepts_runnable: bool = false,
    runnable_acceptance_requested: bool = false,
    is_idle: bool = true,
};

pub const ThreadContext = struct {
    id: u32 = 0,
    allocated: bool = false,
    owner_process: kernel.PrincipalId = default_process_principal,
    cpu_slot: usize = bootstrap_cpu_slot,
    cpu_affinity_mask: u64 = all_cpu_affinity_mask,
    cr3: u64 = 0,
    fs_base: u64 = 0,
    ready: bool = false,
    wait_mailbox: bool = false,
    signal_pending: bool = false,
    wake_tick: u64 = 0,
    ipc_cached_endpoint_generation: u64 = std.math.maxInt(u64),
    ipc_cached_endpoint_id: u64 = 0,
    ipc_cached_target: kernel.PrincipalId = default_process_principal,
    ipc_cached_target_thread: usize = 0,
    ipc_reply_token_valid: bool = false,
    ipc_reply_token_target_thread: usize = 0,
    abi_trap_reply_pending: bool = false,
    frame: TrapFrame = std.mem.zeroes(TrapFrame),
    fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes,
};

pub const IpcHotThread = extern struct {
    allocated: u8 = 0,
    ready: u8 = 0,
    signal_pending: u8 = 0,
    wait_mailbox: u8 = 0,
    owner_process: kernel.PrincipalId = default_process_principal,
    _pad0: [3]u8 = [_]u8{0} ** 3,
    wake_tick: u64 = 0,
    cr3: u64 = 0,
    ipc_cached_endpoint_generation: u64 = std.math.maxInt(u64),
    ipc_cached_endpoint_id: u64 = 0,
    ipc_cached_target: kernel.PrincipalId = default_process_principal,
    _pad1: [7]u8 = [_]u8{0} ** 7,
    ipc_cached_target_thread: usize = 0,
    ipc_reply_token_valid: u8 = 0,
    _pad2: [7]u8 = [_]u8{0} ** 7,
    ipc_reply_token_target_thread: usize = 0,
};

pub fn buildInitialThreadContexts() [max_thread_slots]ThreadContext {
    var contexts: [max_thread_slots]ThreadContext = undefined;
    inline for (0..max_thread_slots) |i| {
        contexts[i] = .{ .id = @intCast(i) };
    }
    return contexts;
}

pub fn buildInitialIpcHotThreads() [max_thread_slots]IpcHotThread {
    var hot: [max_thread_slots]IpcHotThread = undefined;
    inline for (0..max_thread_slots) |i| {
        hot[i] = .{};
    }
    return hot;
}

fn buildInitialIpcQueues() [max_thread_slots]IpcQueue {
    var queues: [max_thread_slots]IpcQueue = undefined;
    inline for (0..max_thread_slots) |i| {
        queues[i] = .{};
    }
    return queues;
}

pub var thread_contexts: [max_thread_slots]ThreadContext = buildInitialThreadContexts();
pub export var thread_contexts_ptr: *anyopaque = @ptrCast(&thread_contexts);
pub var ipc_hot_threads: [max_thread_slots]IpcHotThread = buildInitialIpcHotThreads();
pub export var ipc_hot_threads_ptr: *anyopaque = @ptrCast(&ipc_hot_threads);
var ipc_queues: [max_thread_slots]IpcQueue = buildInitialIpcQueues();
var cpu_scheduler_states: [smp.max_cpus]CpuSchedulerState = buildInitialCpuSchedulerStates();
pub var process_thread_slots: [kernel.process_count]?usize = [_]?usize{null} ** kernel.process_count;
pub export var user_cr3_value: u64 = 0;
pub export var current_user_principal: kernel.PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
pub export var current_thread_index: usize = 0;
pub export var lapic_tick_count: u64 = 0;
pub var scheduler_tick_accum: u64 = 0;
pub var scheduler_switch_count: u64 = 0;
pub var scheduler_perf_user_ticks: u64 = 0;
pub var scheduler_perf_priority_owner_ticks: u64 = 0;
pub var scheduler_perf_priority_thread_ticks: u64 = 0;
pub var scheduler_probe_log_count: u64 = 0;
pub var runtime_priority_streak: u64 = 0;
pub var scheduler_int80_log_count: u64 = 0;
pub var scheduler_race_log_count: u64 = 0;
pub var runtime_priority_active = false;
pub var runtime_priority_principal: ?kernel.PrincipalId = null;
pub var initial_fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes;
pub var kernel_interrupt_fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes;
var preferred_ipc_switch_threads: [max_thread_slots]?usize = [_]?usize{null} ** max_thread_slots;

fn buildInitialCpuSchedulerStates() [smp.max_cpus]CpuSchedulerState {
    var states: [smp.max_cpus]CpuSchedulerState = [_]CpuSchedulerState{.{}} ** smp.max_cpus;
    states[bootstrap_cpu_slot].enabled = true;
    states[bootstrap_cpu_slot].accepts_runnable = true;
    states[bootstrap_cpu_slot].runnable_acceptance_requested = true;
    states[bootstrap_cpu_slot].current_thread = 0;
    states[bootstrap_cpu_slot].is_idle = false;
    return states;
}

fn primarySchedulerState() *CpuSchedulerState {
    return &cpu_scheduler_states[bootstrap_cpu_slot];
}

fn schedulerStateForSlot(cpu_slot: usize) ?*CpuSchedulerState {
    if (cpu_slot >= cpu_scheduler_states.len) return null;
    return &cpu_scheduler_states[cpu_slot];
}

fn lockAllCpuSchedulerStates() void {
    var cpu_slot: usize = 0;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        cpu_scheduler_states[cpu_slot].lock.lock();
    }
}

fn unlockAllCpuSchedulerStates() void {
    var remaining = cpu_scheduler_states.len;
    while (remaining != 0) {
        remaining -= 1;
        cpu_scheduler_states[remaining].lock.unlock();
    }
}

fn rebuildRunnableQueuesFromHotThreadsLocked() void {
    var cpu_slot: usize = 0;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        cpu_scheduler_states[cpu_slot].run_queue = .{};
    }
    var i: usize = 0;
    while (i < max_thread_slots) : (i += 1) {
        const hot = getIpcHotThreadConst(i) orelse continue;
        if (hot.allocated != 0 and hot.ready != 0) _ = enqueueRunnableThreadLocked(i);
    }
}

fn rebuildRunnableQueuesFromHotThreads() void {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    rebuildRunnableQueuesFromHotThreadsLocked();
}

fn cpuAcceptsRunnableLocked(cpu_slot: usize) bool {
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    return state.enabled and state.accepts_runnable;
}

fn requestedRunnableAcceptanceAllowed(cpu_slot: usize, accepts: bool) bool {
    if (!accepts) return true;
    if (cpu_slot == bootstrap_cpu_slot) return true;
    return build_workarounds.scheduler_ap_queue_experiment;
}

fn cpuAffinityBit(cpu_slot: usize) ?u64 {
    if (cpu_slot >= 64) return null;
    return @as(u64, 1) << @intCast(cpu_slot);
}

fn threadAssignedCpuSlot(thread_index: usize) usize {
    const ctx = getThreadContextConst(thread_index) orelse return bootstrap_cpu_slot;
    if (!ctx.allocated) return bootstrap_cpu_slot;
    if (ctx.cpu_slot >= cpu_scheduler_states.len) return bootstrap_cpu_slot;
    return ctx.cpu_slot;
}

fn dequeueRunnableThreadFromAllCpusLocked(thread_index: usize) void {
    var cpu_slot: usize = 0;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        const state = &cpu_scheduler_states[cpu_slot];
        state.run_queue.markBlocked(thread_index);
        if (state.pending_handoff_thread == thread_index) {
            state.pending_handoff_thread = null;
            state.validated_handoff_thread = null;
            state.handoff_validation_code = handoff_validation_none;
            state.handoff_runnable_count = state.run_queue.len;
        }
        if (state.consumed_handoff_thread == thread_index) {
            state.consumed_handoff_thread = null;
            if (state.entered_handoff_thread == thread_index) state.entered_handoff_thread = null;
            clearHandoffSnapshotLocked(state);
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
        }
    }
}

fn enqueueRunnableThreadLocked(thread_index: usize) bool {
    const cpu_slot = threadAssignedCpuSlot(thread_index);
    if (!cpuAcceptsRunnableLocked(cpu_slot)) return false;
    const state = &cpu_scheduler_states[cpu_slot];
    if (state.consumed_handoff_thread == thread_index) return true;
    state.run_queue.markRunnable(thread_index);
    return true;
}

fn setRunnableQueueMembership(thread_index: usize, runnable: bool) void {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    dequeueRunnableThreadFromAllCpusLocked(thread_index);
    if (runnable) _ = enqueueRunnableThreadLocked(thread_index);
}

pub fn cpuCount() usize {
    return smp.cpuCount();
}

pub fn currentCpuSlot() usize {
    return smp.currentCpuSlot();
}

pub fn schedulerRunsOnCurrentCpu() bool {
    return smp.isBootstrapCpu();
}

pub fn installCpuIdleObserver() void {
    scheduler_observer.registerIdleObserver(observeCpuIdleFromAp);
    scheduler_observer.registerIdleSchedulerPoll(pollSchedulerFromIdleAp);
    scheduler_observer.registerIdleUserEntryPoll(claimIdleUserEntryFromAp);
}

fn observeCpuIdleFromAp(cpu_slot: usize) callconv(.c) void {
    noteCpuIdleTick(cpu_slot);
}

fn pollSchedulerFromIdleAp(cpu_slot: usize) callconv(.c) void {
    handoffNextThreadToIdleCpu(cpu_slot);
}

fn claimIdleUserEntryFromAp(cpu_slot: usize, out_entry: *scheduler_observer.UserEntry) callconv(.c) bool {
    if (!build_workarounds.scheduler_ap_queue_experiment) return false;
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    if (!state.user_entry_requested) return false;
    if (state.entered_handoff_thread != null) return false;
    const thread_index = state.consumed_handoff_thread orelse return false;
    const code = validateThreadForCpuHandoff(cpu_slot, thread_index);
    if (code != handoff_validation_ok) {
        state.handoff_user_entry_failures +%= 1;
        return false;
    }
    const ctx = getThreadContextConst(thread_index) orelse {
        state.handoff_user_entry_failures +%= 1;
        return false;
    };
    out_entry.* = .{
        .cpu_slot = cpu_slot,
        .thread_index = thread_index,
        .cr3 = ctx.cr3,
        .fs_base = ctx.fs_base,
        .frame = ctx.frame,
    };
    state.current_thread = thread_index;
    state.current_principal = ctx.owner_process;
    state.current_cr3 = ctx.cr3;
    state.is_idle = false;
    state.entered_handoff_thread = thread_index;
    state.handoff_user_entry_ticks +%= 1;
    return true;
}

pub fn refreshCpuTopology() void {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const count = @min(smp.cpuCount(), cpu_scheduler_states.len);
    var i: usize = 0;
    while (i < cpu_scheduler_states.len) : (i += 1) {
        const observed = i < count and smp.cpuState(i) != .absent;
        const state = &cpu_scheduler_states[i];
        state.enabled = observed;
        if (i == bootstrap_cpu_slot) state.runnable_acceptance_requested = true;
        state.accepts_runnable = observed and state.runnable_acceptance_requested and requestedRunnableAcceptanceAllowed(i, true);
        if (!observed) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
        } else if (i != bootstrap_cpu_slot) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
        } else if (state.current_thread == idle_thread_marker) {
            state.current_thread = current_thread_index;
            state.current_principal = current_user_principal;
            state.current_cr3 = user_cr3_value;
            state.is_idle = false;
        }
    }
}

pub fn schedulerApQueueExperimentEnabled() bool {
    return build_workarounds.scheduler_ap_queue_experiment;
}

pub fn cpuRunnableAcceptanceRequested(cpu_slot: usize) bool {
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    return state.runnable_acceptance_requested;
}

pub fn setCpuRunnableAcceptanceForExperiment(cpu_slot: usize, accepts: bool) bool {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    if (!state.enabled) return false;
    if (!requestedRunnableAcceptanceAllowed(cpu_slot, accepts)) return false;
    state.runnable_acceptance_requested = accepts;
    state.accepts_runnable = accepts;
    if (!accepts) {
        var i: usize = 0;
        while (i < max_thread_slots) : (i += 1) {
            state.run_queue.markBlocked(i);
        }
        state.pending_handoff_thread = null;
        state.handoff_runnable_count = 0;
        state.validated_handoff_thread = null;
        state.handoff_validation_code = handoff_validation_none;
        state.consumed_handoff_thread = null;
        state.entered_handoff_thread = null;
        state.user_entry_requested = false;
        state.ap_user_entry_target_cycles = 0;
        state.ap_timer_saved_thread = null;
        state.ap_timer_saved_rip = 0;
        state.ap_timer_saved_rsp = 0;
        state.ap_preempt_from_thread = null;
        state.ap_preempt_to_thread = null;
        clearHandoffSnapshotLocked(state);
        if (state.current_thread != state.idle_thread) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
        }
    }
    rebuildRunnableQueuesFromHotThreadsLocked();
    return true;
}

pub fn setCpuUserEntryForExperiment(cpu_slot: usize, enabled: bool) bool {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    if (!state.enabled) return false;
    if (!build_workarounds.scheduler_ap_queue_experiment) return false;
    if (cpu_slot == bootstrap_cpu_slot and enabled) return false;
    state.user_entry_requested = enabled;
    if (!enabled) state.entered_handoff_thread = null;
    return true;
}

pub fn setCpuUserEntryTargetCyclesForExperiment(cpu_slot: usize, cycles: u64) bool {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    if (!state.enabled) return false;
    if (!build_workarounds.scheduler_ap_queue_experiment) return false;
    if (cpu_slot == bootstrap_cpu_slot and cycles != 0) return false;
    state.ap_user_entry_target_cycles = cycles;
    return true;
}

pub fn threadCpuSlot(thread_index: usize) ?usize {
    const ctx = getThreadContextConst(thread_index) orelse return null;
    if (!ctx.allocated) return null;
    return ctx.cpu_slot;
}

pub fn threadCpuAffinityMask(thread_index: usize) ?u64 {
    const ctx = getThreadContextConst(thread_index) orelse return null;
    if (!ctx.allocated) return null;
    return ctx.cpu_affinity_mask;
}

pub fn assignThreadToCpu(thread_index: usize, cpu_slot: usize) bool {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    if (cpu_slot >= cpu_scheduler_states.len) return false;
    const bit = cpuAffinityBit(cpu_slot) orelse return false;
    if ((ctx.cpu_affinity_mask & bit) == 0) return false;
    if (!cpuAcceptsRunnableLocked(cpu_slot)) return false;
    const was_ready = ctx.ready;
    dequeueRunnableThreadFromAllCpusLocked(thread_index);
    ctx.cpu_slot = cpu_slot;
    if (was_ready) return enqueueRunnableThreadLocked(thread_index);
    return true;
}

pub fn schedulerCpuInfo(cpu_slot: usize) ?CpuSchedulerInfo {
    if (cpu_slot >= cpu_scheduler_states.len) return null;
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    rebuildRunnableQueuesFromHotThreadsLocked();
    const state = &cpu_scheduler_states[cpu_slot];
    return .{
        .slot = cpu_slot,
        .smp_state = smp.cpuState(cpu_slot),
        .current_thread = state.current_thread,
        .current_principal = state.current_principal,
        .current_cr3 = state.current_cr3,
        .idle_thread = state.idle_thread,
        .runnable_count = state.run_queue.len,
        .idle_ticks = state.idle_ticks,
        .observer_ticks = state.observer_ticks,
        .observed_runnable_ticks = state.observed_runnable_ticks,
        .observed_runnable_count = state.observed_runnable_count,
        .observed_next_thread = state.observed_next_thread,
        .handoff_ticks = state.handoff_ticks,
        .handoff_runnable_count = state.handoff_runnable_count,
        .pending_handoff_thread = state.pending_handoff_thread,
        .handoff_validation_ticks = state.handoff_validation_ticks,
        .handoff_validation_failures = state.handoff_validation_failures,
        .validated_handoff_thread = state.validated_handoff_thread,
        .handoff_validation_code = state.handoff_validation_code,
        .handoff_consume_ticks = state.handoff_consume_ticks,
        .consumed_handoff_thread = state.consumed_handoff_thread,
        .handoff_snapshot_ticks = state.handoff_snapshot_ticks,
        .handoff_snapshot_failures = state.handoff_snapshot_failures,
        .snapshot_handoff_thread = state.snapshot_handoff_thread,
        .snapshot_cr3 = state.snapshot_cr3,
        .snapshot_fs_base = state.snapshot_fs_base,
        .snapshot_rip = state.snapshot_rip,
        .snapshot_rsp = state.snapshot_rsp,
        .snapshot_rflags = state.snapshot_rflags,
        .snapshot_cs = state.snapshot_cs,
        .snapshot_ss = state.snapshot_ss,
        .handoff_user_entry_ticks = state.handoff_user_entry_ticks,
        .handoff_user_entry_failures = state.handoff_user_entry_failures,
        .entered_handoff_thread = state.entered_handoff_thread,
        .user_entry_requested = state.user_entry_requested,
        .ap_timer_save_ticks = state.ap_timer_save_ticks,
        .ap_timer_saved_thread = state.ap_timer_saved_thread,
        .ap_timer_saved_rip = state.ap_timer_saved_rip,
        .ap_timer_saved_rsp = state.ap_timer_saved_rsp,
        .ap_timer_requeue_ticks = state.ap_timer_requeue_ticks,
        .ap_preempt_switch_ticks = state.ap_preempt_switch_ticks,
        .ap_preempt_from_thread = state.ap_preempt_from_thread,
        .ap_preempt_to_thread = state.ap_preempt_to_thread,
        .ap_user_entry_target_cycles = state.ap_user_entry_target_cycles,
        .schedule_ticks = state.schedule_ticks,
        .enabled = state.enabled,
        .accepts_runnable = state.accepts_runnable,
        .runnable_acceptance_requested = state.runnable_acceptance_requested,
        .is_idle = state.is_idle,
    };
}

pub fn noteCurrentCpuIdleTick() void {
    noteCpuIdleTick(currentCpuSlot());
}

fn noteCpuIdleTick(cpu_slot: usize) void {
    const state = schedulerStateForSlot(cpu_slot) orelse return;
    state.lock.lock();
    defer state.lock.unlock();
    if (state.consumed_handoff_thread) |thread| {
        state.current_thread = thread;
        if (getThreadContextConst(thread)) |ctx| {
            state.current_principal = ctx.owner_process;
            state.current_cr3 = ctx.cr3;
        }
        state.is_idle = false;
    } else {
        state.current_thread = state.idle_thread;
        state.current_principal = null;
        state.current_cr3 = 0;
        state.is_idle = true;
    }
    state.idle_ticks +%= 1;
    state.observer_ticks +%= 1;
    if (state.enabled and state.accepts_runnable and state.run_queue.len != 0) {
        state.observed_runnable_ticks +%= 1;
        state.observed_runnable_count = state.run_queue.len;
        state.observed_next_thread = state.run_queue.pickFirst();
    } else {
        state.observed_runnable_count = 0;
        state.observed_next_thread = null;
    }
}

pub fn saveApUserTimerFrame(frame: *const TrapFrame) bool {
    if (!build_workarounds.scheduler_ap_queue_experiment) return false;
    const cpu_slot = currentCpuSlot();
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    const thread_index = state.entered_handoff_thread orelse state.current_thread;
    if (thread_index >= max_thread_slots) {
        state.handoff_user_entry_failures +%= 1;
        return false;
    }
    const ctx = getThreadContext(thread_index) orelse {
        state.handoff_user_entry_failures +%= 1;
        return false;
    };
    const hot = getIpcHotThread(thread_index) orelse {
        state.handoff_user_entry_failures +%= 1;
        return false;
    };
    if (!ctx.allocated or hot.allocated == 0) {
        state.handoff_user_entry_failures +%= 1;
        return false;
    }
    ctx.frame = frame.*;
    state.consumed_handoff_thread = null;
    state.pending_handoff_thread = null;
    state.validated_handoff_thread = null;
    state.handoff_validation_code = handoff_validation_none;
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
    state.ap_timer_saved_thread = thread_index;
    state.ap_timer_saved_rip = frame.rip;
    state.ap_timer_saved_rsp = frame.rsp;
    state.ap_timer_save_ticks +%= 1;
    state.entered_handoff_thread = null;

    const reached_target = state.ap_user_entry_target_cycles != 0 and state.ap_timer_save_ticks >= state.ap_user_entry_target_cycles;
    if (reached_target) {
        var i: usize = 0;
        while (i < max_thread_slots) : (i += 1) {
            if (getThreadContext(i)) |slot_ctx| {
                if (slot_ctx.allocated and slot_ctx.cpu_slot == cpu_slot) {
                    slot_ctx.ready = false;
                    if (getIpcHotThread(i)) |slot_hot| slot_hot.ready = 0;
                }
            }
            state.run_queue.markBlocked(i);
        }
        state.user_entry_requested = false;
        clearHandoffSnapshotLocked(state);
        return true;
    }

    ctx.ready = true;
    hot.ready = 1;
    state.run_queue.markRunnable(thread_index);

    const next_thread = state.run_queue.pickNextAfter(thread_index);
    const code = validateThreadForCpuHandoff(cpu_slot, next_thread);
    state.handoff_validation_code = code;
    if (code != handoff_validation_ok) {
        state.validated_handoff_thread = null;
        state.user_entry_requested = false;
        state.handoff_validation_failures +%= 1;
        return true;
    }

    state.pending_handoff_thread = next_thread;
    state.handoff_runnable_count = state.run_queue.len;
    state.handoff_ticks +%= 1;
    state.validated_handoff_thread = next_thread;
    state.handoff_validation_ticks +%= 1;
    consumeValidatedHandoffLocked(state, next_thread);
    if (state.consumed_handoff_thread == next_thread) {
        state.user_entry_requested = true;
        state.ap_timer_requeue_ticks +%= 1;
        state.ap_preempt_switch_ticks +%= 1;
        state.ap_preempt_from_thread = thread_index;
        state.ap_preempt_to_thread = next_thread;
        snapshotConsumedHandoffLocked(state, cpu_slot, next_thread);
    }
    return true;
}

fn handoffNextThreadToIdleCpu(cpu_slot: usize) void {
    if (!build_workarounds.scheduler_ap_queue_experiment) return;
    if (cpu_slot == bootstrap_cpu_slot) return;
    const state = schedulerStateForSlot(cpu_slot) orelse return;
    state.lock.lock();
    defer state.lock.unlock();
    if (state.consumed_handoff_thread) |thread| {
        snapshotConsumedHandoffLocked(state, cpu_slot, thread);
        return;
    }
    if (!state.enabled or !state.accepts_runnable or state.run_queue.len == 0) {
        state.pending_handoff_thread = null;
        state.handoff_runnable_count = 0;
        state.validated_handoff_thread = null;
        state.handoff_validation_code = handoff_validation_none;
        clearHandoffSnapshotLocked(state);
        return;
    }
    const next_thread = state.run_queue.pickFirst() orelse {
        state.pending_handoff_thread = null;
        state.handoff_runnable_count = 0;
        state.validated_handoff_thread = null;
        state.handoff_validation_code = handoff_validation_none;
        clearHandoffSnapshotLocked(state);
        return;
    };
    state.handoff_runnable_count = state.run_queue.len;
    if (state.pending_handoff_thread != next_thread) {
        state.pending_handoff_thread = next_thread;
        state.handoff_ticks +%= 1;
    }
    validatePendingHandoffLocked(state, cpu_slot, next_thread);
    consumeValidatedHandoffLocked(state, next_thread);
    if (state.consumed_handoff_thread == next_thread) {
        snapshotConsumedHandoffLocked(state, cpu_slot, next_thread);
    }
}

fn validatePendingHandoffLocked(state: *CpuSchedulerState, cpu_slot: usize, thread_index: usize) void {
    const code = validateThreadForCpuHandoff(cpu_slot, thread_index);
    state.handoff_validation_code = code;
    if (code == handoff_validation_ok) {
        if (state.validated_handoff_thread != thread_index) {
            state.validated_handoff_thread = thread_index;
            state.handoff_validation_ticks +%= 1;
        }
    } else {
        state.validated_handoff_thread = null;
        state.handoff_validation_failures +%= 1;
    }
}

fn consumeValidatedHandoffLocked(state: *CpuSchedulerState, thread_index: usize) void {
    if (state.validated_handoff_thread != thread_index) return;
    if (state.handoff_validation_code != handoff_validation_ok) return;
    state.run_queue.markBlocked(thread_index);
    state.pending_handoff_thread = null;
    state.current_thread = thread_index;
    if (getThreadContextConst(thread_index)) |ctx| {
        state.current_principal = ctx.owner_process;
        state.current_cr3 = ctx.cr3;
    }
    state.is_idle = false;
    if (state.consumed_handoff_thread != thread_index) {
        state.consumed_handoff_thread = thread_index;
        state.handoff_consume_ticks +%= 1;
        state.schedule_ticks +%= 1;
    }
}

fn clearHandoffSnapshotLocked(state: *CpuSchedulerState) void {
    state.snapshot_handoff_thread = null;
    state.snapshot_cr3 = 0;
    state.snapshot_fs_base = 0;
    state.snapshot_rip = 0;
    state.snapshot_rsp = 0;
    state.snapshot_rflags = 0;
    state.snapshot_cs = 0;
    state.snapshot_ss = 0;
}

fn snapshotConsumedHandoffLocked(state: *CpuSchedulerState, cpu_slot: usize, thread_index: usize) void {
    const code = validateThreadForCpuHandoff(cpu_slot, thread_index);
    if (code != handoff_validation_ok) {
        if (state.snapshot_handoff_thread == thread_index) clearHandoffSnapshotLocked(state);
        state.handoff_snapshot_failures +%= 1;
        return;
    }
    const ctx = getThreadContextConst(thread_index) orelse {
        state.handoff_snapshot_failures +%= 1;
        clearHandoffSnapshotLocked(state);
        return;
    };
    const changed = state.snapshot_handoff_thread != thread_index or
        state.snapshot_cr3 != ctx.cr3 or
        state.snapshot_fs_base != ctx.fs_base or
        state.snapshot_rip != ctx.frame.rip or
        state.snapshot_rsp != ctx.frame.rsp or
        state.snapshot_rflags != ctx.frame.rflags or
        state.snapshot_cs != ctx.frame.cs or
        state.snapshot_ss != ctx.frame.ss;
    state.snapshot_handoff_thread = thread_index;
    state.snapshot_cr3 = ctx.cr3;
    state.snapshot_fs_base = ctx.fs_base;
    state.snapshot_rip = ctx.frame.rip;
    state.snapshot_rsp = ctx.frame.rsp;
    state.snapshot_rflags = ctx.frame.rflags;
    state.snapshot_cs = ctx.frame.cs;
    state.snapshot_ss = ctx.frame.ss;
    if (changed) state.handoff_snapshot_ticks +%= 1;
}

fn validateThreadForCpuHandoff(cpu_slot: usize, thread_index: usize) u8 {
    if (thread_index >= max_thread_slots) return handoff_validation_bad_thread;
    const ctx = getThreadContextConst(thread_index) orelse return handoff_validation_bad_thread;
    const hot = getIpcHotThreadConst(thread_index) orelse return handoff_validation_bad_thread;
    if (!ctx.allocated or hot.allocated == 0) return handoff_validation_not_allocated;
    if (!ctx.ready or hot.ready == 0) return handoff_validation_not_ready;
    if (ctx.cpu_slot != cpu_slot) return handoff_validation_wrong_cpu;
    const affinity_bit = cpuAffinityBit(cpu_slot) orelse return handoff_validation_bad_affinity;
    if ((ctx.cpu_affinity_mask & affinity_bit) == 0) return handoff_validation_bad_affinity;
    if (threadSlotForPrincipal(ctx.owner_process) != thread_index) return handoff_validation_bad_owner;
    if (hot.owner_process != ctx.owner_process) return handoff_validation_bad_owner;
    const cr3_addr = x86_platform.cr3AddressPart(ctx.cr3);
    if (ctx.cr3 != hot.cr3 or cr3_addr == 0 or (cr3_addr & 0xFFF) != 0) return handoff_validation_bad_cr3;
    if (ctx.frame.rip == 0 or ctx.frame.rsp == 0) return handoff_validation_bad_frame;
    return handoff_validation_ok;
}

pub fn runnableThreadCount() usize {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    rebuildRunnableQueuesFromHotThreadsLocked();
    return primarySchedulerState().run_queue.len;
}

pub fn runnableThreadCountForCpu(cpu_slot: usize) usize {
    const state = schedulerStateForSlot(cpu_slot) orelse return 0;
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    rebuildRunnableQueuesFromHotThreadsLocked();
    return state.run_queue.len;
}

pub fn pickNextReadyThreadIndexForCpu(cpu_slot: usize, current_index: usize) ?usize {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const state = schedulerStateForSlot(cpu_slot) orelse return null;
    if (!state.enabled or !state.accepts_runnable) return null;
    rebuildRunnableQueuesFromHotThreadsLocked();
    if (state.run_queue.len == 0) return null;
    return state.run_queue.pickNextAfter(current_index);
}

pub const RaceLogHooks = struct {
    write: *const fn ([]const u8) void,
    print_hex: *const fn (u64) void,
    principal_label: *const fn (kernel.PrincipalId) []const u8,
};

pub const PerfReport = struct {
    ticks: u64,
    priority_owner_ticks: u64,
    priority_thread_ticks: u64,
    switch_delta: u64,
    runtime_priority_active: bool,
    runtime_priority_streak: u64,
};

pub fn noteUserTimerTick() void {
    if (!schedulerRunsOnCurrentCpu()) return;
    scheduler_perf_user_ticks +%= 1;
    if (runtime_priority_principal) |principal| {
        if (getThreadContextConst(current_thread_index)) |ctx| {
            if (ctx.allocated and ctx.owner_process == principal) scheduler_perf_priority_owner_ticks +%= 1;
        }
        if (threadSlotForPrincipal(principal)) |slot| {
            if (current_thread_index == slot) scheduler_perf_priority_thread_ticks +%= 1;
        }
    }
}

pub fn chooseNextThreadForTimerPreempt(quantum_ticks: u64, priority_hold_quanta: u64) ?usize {
    if (!schedulerRunsOnCurrentCpu()) return null;
    if (quantum_ticks == 0) return null;

    scheduler_tick_accum +%= 1;
    if (scheduler_tick_accum < quantum_ticks) return null;
    scheduler_tick_accum = 0;

    const current_thread = current_thread_index;
    if (!runtime_priority_active) {
        runtime_priority_streak = 0;
    } else if (runtime_priority_principal) |priority_principal| {
        if (threadSlotForPrincipal(priority_principal)) |priority_slot| {
            if (current_thread != priority_slot) {
                runtime_priority_streak = 0;
            } else {
                const priority_hot = getIpcHotThreadConst(priority_slot) orelse null;
                if (priority_hot != null and priority_hot.?.allocated != 0 and priority_hot.?.ready != 0 and runtime_priority_streak + 1 < priority_hold_quanta) {
                    runtime_priority_streak +%= 1;
                    return null;
                }
                runtime_priority_streak = 0;
            }
        } else {
            runtime_priority_streak = 0;
        }
    } else {
        runtime_priority_streak = 0;
    }

    const next_thread = pickNextReadyThreadIndex(current_thread);
    if (next_thread == current_thread) return null;
    return next_thread;
}

pub fn takePerfReport(report_interval_ticks: u64, last_switch_count: *u64) ?PerfReport {
    if (scheduler_perf_user_ticks < report_interval_ticks) return null;
    const report: PerfReport = .{
        .ticks = scheduler_perf_user_ticks,
        .priority_owner_ticks = scheduler_perf_priority_owner_ticks,
        .priority_thread_ticks = scheduler_perf_priority_thread_ticks,
        .switch_delta = scheduler_switch_count - last_switch_count.*,
        .runtime_priority_active = runtime_priority_active,
        .runtime_priority_streak = runtime_priority_streak,
    };
    last_switch_count.* = scheduler_switch_count;
    scheduler_perf_user_ticks = 0;
    scheduler_perf_priority_owner_ticks = 0;
    scheduler_perf_priority_thread_ticks = 0;
    return report;
}

pub fn setRuntimePriorityPrincipal(principal: ?kernel.PrincipalId) void {
    runtime_priority_principal = principal;
    if (principal == null) {
        runtime_priority_active = false;
        runtime_priority_streak = 0;
    }
}

pub fn refreshRuntimePriorityActive(requested: bool, allow_active: bool) void {
    runtime_priority_active = requested and allow_active and runtime_priority_principal != null;
    if (!runtime_priority_active) {
        runtime_priority_streak = 0;
    }
}

fn getUserSpace(user_spaces: []UserAddressSpace, principal: kernel.PrincipalId) ?*UserAddressSpace {
    const idx = kernel.processIndexFromPrincipal(principal) orelse return null;
    if (idx >= user_spaces.len) return null;
    return &user_spaces[idx];
}

fn pcidForPrincipal(principal: kernel.PrincipalId) u16 {
    const idx = kernel.processIndexFromPrincipal(principal) orelse return 0;
    return @intCast(idx + 1);
}

pub fn getThreadContext(thread_index: usize) ?*ThreadContext {
    if (thread_index >= max_thread_slots) return null;
    return &thread_contexts[thread_index];
}

pub fn getThreadContextConst(thread_index: usize) ?*const ThreadContext {
    if (thread_index >= max_thread_slots) return null;
    return &thread_contexts[thread_index];
}

pub fn getIpcHotThread(thread_index: usize) ?*IpcHotThread {
    if (thread_index >= max_thread_slots) return null;
    return &ipc_hot_threads[thread_index];
}

pub fn getIpcHotThreadConst(thread_index: usize) ?*const IpcHotThread {
    if (thread_index >= max_thread_slots) return null;
    return &ipc_hot_threads[thread_index];
}

fn boolByte(value: bool) u8 {
    return if (value) 1 else 0;
}

fn hotThreadFromContext(ctx: *const ThreadContext) IpcHotThread {
    return .{
        .allocated = boolByte(ctx.allocated),
        .ready = boolByte(ctx.ready),
        .signal_pending = boolByte(ctx.signal_pending),
        .wait_mailbox = boolByte(ctx.wait_mailbox),
        .owner_process = ctx.owner_process,
        .wake_tick = ctx.wake_tick,
        .cr3 = ctx.cr3,
        .ipc_cached_endpoint_generation = ctx.ipc_cached_endpoint_generation,
        .ipc_cached_endpoint_id = ctx.ipc_cached_endpoint_id,
        .ipc_cached_target = ctx.ipc_cached_target,
        .ipc_cached_target_thread = ctx.ipc_cached_target_thread,
        .ipc_reply_token_valid = boolByte(ctx.ipc_reply_token_valid),
        .ipc_reply_token_target_thread = ctx.ipc_reply_token_target_thread,
    };
}

fn syncHotThreadFromContext(thread_index: usize) void {
    const ctx = getThreadContextConst(thread_index) orelse return;
    const hot = getIpcHotThread(thread_index) orelse return;
    hot.* = hotThreadFromContext(ctx);
    setRunnableQueueMembership(thread_index, ctx.allocated and ctx.ready);
}

pub fn setIpcHotReady(thread_index: usize, ready: bool) void {
    const runnable = blk: {
        if (getIpcHotThread(thread_index)) |hot| {
            hot.ready = boolByte(ready);
            break :blk ready and hot.allocated != 0;
        }
        break :blk false;
    };
    setRunnableQueueMembership(thread_index, runnable);
}

pub fn setIpcHotSignalPending(thread_index: usize, pending: bool) void {
    if (getIpcHotThread(thread_index)) |hot| hot.signal_pending = boolByte(pending);
}

pub fn setIpcHotWaitState(thread_index: usize, wait_mailbox: bool, wake_tick: u64, ready: bool) void {
    var runnable = false;
    if (getIpcHotThread(thread_index)) |hot| {
        hot.wait_mailbox = boolByte(wait_mailbox);
        hot.wake_tick = wake_tick;
        hot.ready = boolByte(ready);
        runnable = ready and hot.allocated != 0;
    }
    setRunnableQueueMembership(thread_index, runnable);
}

pub fn setIpcHotCr3(thread_index: usize, cr3: u64) void {
    if (getIpcHotThread(thread_index)) |hot| hot.cr3 = cr3;
}

pub fn setIpcReplyTokenForThread(thread_index: usize, valid: bool, target_thread: usize) void {
    if (getThreadContext(thread_index)) |ctx| {
        ctx.ipc_reply_token_valid = valid;
        ctx.ipc_reply_token_target_thread = if (valid) target_thread else 0;
    }
    if (getIpcHotThread(thread_index)) |hot| {
        hot.ipc_reply_token_valid = boolByte(valid);
        hot.ipc_reply_token_target_thread = if (valid) target_thread else 0;
    }
}

pub fn setIpcEndpointCacheForThread(
    thread_index: usize,
    generation: u64,
    endpoint_id: u64,
    target: kernel.PrincipalId,
    target_thread: usize,
) void {
    if (getThreadContext(thread_index)) |ctx| {
        ctx.ipc_cached_endpoint_generation = generation;
        ctx.ipc_cached_endpoint_id = endpoint_id;
        ctx.ipc_cached_target = target;
        ctx.ipc_cached_target_thread = target_thread;
    }
    if (getIpcHotThread(thread_index)) |hot| {
        hot.ipc_cached_endpoint_generation = generation;
        hot.ipc_cached_endpoint_id = endpoint_id;
        hot.ipc_cached_target = target;
        hot.ipc_cached_target_thread = target_thread;
    }
}

pub fn clearIpcEndpointCacheForThread(thread_index: usize) void {
    setIpcEndpointCacheForThread(thread_index, std.math.maxInt(u64), 0, default_process_principal, 0);
}

pub fn isThreadReady(thread_index: usize) bool {
    const hot = getIpcHotThreadConst(thread_index) orelse return false;
    return hot.allocated != 0 and hot.ready != 0;
}

fn threadLabel(thread_index: usize) []const u8 {
    const labels = comptime blk: {
        var items: [max_thread_slots][]const u8 = undefined;
        for (0..max_thread_slots) |i| {
            items[i] = std.fmt.comptimePrint("Thread{}", .{i});
        }
        break :blk items;
    };
    if (thread_index < labels.len) return labels[thread_index];
    return "Thread?";
}

fn tryBeginSchedulerRaceLog(hooks: RaceLogHooks, max_lines: u64) bool {
    if (scheduler_race_log_count >= max_lines) return false;
    scheduler_race_log_count +%= 1;
    hooks.write("SCHED race ");
    return true;
}

pub fn logRaceSendCap(
    hooks: RaceLogHooks,
    max_lines: u64,
    from: kernel.PrincipalId,
    to: ?kernel.PrincipalId,
    endpoint_id: u64,
    paddr: u64,
    reason: []const u8,
) void {
    if (!tryBeginSchedulerRaceLog(hooks, max_lines)) return;
    hooks.write("send_cap from=");
    hooks.write(hooks.principal_label(from));
    hooks.write(" to=");
    if (to) |target| {
        hooks.write(hooks.principal_label(target));
    } else {
        hooks.write("unknown");
    }
    hooks.write(" ep=");
    hooks.print_hex(endpoint_id);
    hooks.write(" paddr=");
    hooks.print_hex(paddr);
    hooks.write(" reason=");
    hooks.write(reason);
    hooks.write("\n");
}

pub fn logRaceSwitch(
    hooks: RaceLogHooks,
    max_lines: u64,
    current_thread: usize,
    target_thread: usize,
    reason: []const u8,
) void {
    if (!tryBeginSchedulerRaceLog(hooks, max_lines)) return;
    hooks.write("switch_thread from=");
    hooks.write(threadLabel(current_thread));
    hooks.write(" to=");
    hooks.write(threadLabel(target_thread));
    hooks.write(" reason=");
    hooks.write(reason);
    hooks.write("\n");
}

pub fn setThreadReady(thread_index: usize, ready: bool) bool {
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    ctx.ready = ready;
    setIpcHotReady(thread_index, ready);
    return true;
}

pub fn initThreadContextWithSpaces(
    thread_index: usize,
    owner_process: kernel.PrincipalId,
    user_spaces: []UserAddressSpace,
    initial_frame: TrapFrame,
) bool {
    const space = getUserSpace(user_spaces, owner_process) orelse return false;
    const ctx = getThreadContext(thread_index) orelse return false;
    if (kernel.processIndexFromPrincipal(owner_process)) |owner_index| {
        process_thread_slots[owner_index] = thread_index;
    } else {
        return false;
    }
    ctx.id = @intCast(thread_index);
    ctx.allocated = true;
    ctx.owner_process = owner_process;
    ctx.cpu_slot = bootstrap_cpu_slot;
    ctx.cpu_affinity_mask = all_cpu_affinity_mask;
    ctx.cr3 = x86_platform.cr3WithUserPcid(space.cr3, pcidForPrincipal(owner_process));
    ctx.fs_base = 0;
    ctx.ready = true;
    ctx.wait_mailbox = false;
    ctx.signal_pending = false;
    ctx.wake_tick = 0;
    ctx.frame = initial_frame;
    ctx.fx_state = initial_fx_state;
    syncHotThreadFromContext(thread_index);
    return true;
}

pub fn threadSlotForPrincipal(principal: kernel.PrincipalId) ?usize {
    const idx = kernel.processIndexFromPrincipal(principal) orelse return null;
    return process_thread_slots[idx];
}

pub fn allocateThreadSlot(owner_process: kernel.PrincipalId, user_spaces: []UserAddressSpace, initial_frame: TrapFrame) ?usize {
    if (threadSlotForPrincipal(owner_process)) |existing| return existing;
    var i: usize = 0;
    while (i < max_thread_slots) : (i += 1) {
        const ctx = getThreadContextConst(i) orelse continue;
        if (ctx.allocated) continue;
        if (!initThreadContextWithSpaces(i, owner_process, user_spaces, initial_frame)) return null;
        return i;
    }
    return null;
}

pub fn releaseThreadSlot(thread_index: usize) bool {
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    if (kernel.processIndexFromPrincipal(ctx.owner_process)) |owner_index| {
        if (process_thread_slots[owner_index] == thread_index) {
            process_thread_slots[owner_index] = null;
        }
    }
    ctx.* = .{ .id = @intCast(thread_index) };
    syncHotThreadFromContext(thread_index);
    preferred_ipc_switch_threads[thread_index] = null;
    var i: usize = 0;
    while (i < preferred_ipc_switch_threads.len) : (i += 1) {
        if (preferred_ipc_switch_threads[i] == thread_index) preferred_ipc_switch_threads[i] = null;
    }
    invalidateIpcFastpathForThread(thread_index);
    syncHotThreadFromContext(thread_index);
    return true;
}

fn resetIpcQueueForThread(thread_index: usize) void {
    if (thread_index >= max_thread_slots) return;
    ipc_queues[thread_index] = .{};
}

fn removeIpcMessagesFromSender(queue: *IpcQueue, sender_thread: usize) void {
    var compacted: IpcQueue = .{};
    var index: usize = 0;
    while (index < queue.len) : (index += 1) {
        const source_index = (@as(usize, queue.head) + index) % max_ipc_queue_depth;
        const msg = queue.entries[source_index];
        if (msg.sender_thread == sender_thread) continue;
        const tail = (@as(usize, compacted.head) + @as(usize, compacted.len)) % max_ipc_queue_depth;
        compacted.entries[tail] = msg;
        compacted.len += 1;
    }
    queue.* = compacted;
}

pub fn purgeIpcMessagesForThread(thread_index: usize) void {
    if (thread_index >= max_thread_slots) return;
    resetIpcQueueForThread(thread_index);
    var i: usize = 0;
    while (i < max_thread_slots) : (i += 1) {
        removeIpcMessagesFromSender(&ipc_queues[i], thread_index);
    }
}

pub fn enqueueIpcMessageForThread(
    target_thread: usize,
    endpoint_id: u64,
    sender_thread: usize,
    grants_reply: bool,
    mr0: u64,
    mr1: u64,
    mr2: u64,
    mr3: u64,
) bool {
    if (target_thread >= max_thread_slots or sender_thread >= max_thread_slots) return false;
    const target_ctx = getThreadContextConst(target_thread) orelse return false;
    const sender_ctx = getThreadContextConst(sender_thread) orelse return false;
    if (!target_ctx.allocated or !sender_ctx.allocated) return false;
    var queue = &ipc_queues[target_thread];
    if (queue.len >= max_ipc_queue_depth) return false;
    const tail = (@as(usize, queue.head) + @as(usize, queue.len)) % max_ipc_queue_depth;
    queue.entries[tail] = .{
        .endpoint_id = endpoint_id,
        .sender_thread = sender_thread,
        .grants_reply = grants_reply,
        .mr0 = mr0,
        .mr1 = mr1,
        .mr2 = mr2,
        .mr3 = mr3,
    };
    queue.len += 1;
    if (getThreadContext(target_thread)) |ctx| {
        ctx.signal_pending = true;
    }
    setIpcHotSignalPending(target_thread, true);
    return true;
}

pub fn dequeueIpcMessageForThread(thread_index: usize) ?IpcQueuedMessage {
    if (thread_index >= max_thread_slots) return null;
    var queue = &ipc_queues[thread_index];
    if (queue.len == 0) return null;
    const index: usize = queue.head;
    const msg = queue.entries[index];
    queue.entries[index] = .{};
    queue.head = @intCast((@as(usize, queue.head) + 1) % max_ipc_queue_depth);
    queue.len -= 1;
    if (queue.len == 0) {
        if (getThreadContext(thread_index)) |ctx| {
            ctx.signal_pending = false;
        }
        setIpcHotSignalPending(thread_index, false);
    }
    return msg;
}

pub fn dequeueIpcMessageForPrincipal(principal: kernel.PrincipalId) ?IpcQueuedMessage {
    const thread_index = threadSlotForPrincipal(principal) orelse return null;
    return dequeueIpcMessageForThread(thread_index);
}

pub fn invalidateIpcFastpathForThread(thread_index: usize) void {
    var i: usize = 0;
    while (i < max_thread_slots) : (i += 1) {
        const hot = getIpcHotThreadConst(i) orelse continue;
        if (hot.ipc_cached_target_thread == thread_index) {
            clearIpcEndpointCacheForThread(i);
        }
        if (hot.ipc_reply_token_valid != 0 and hot.ipc_reply_token_target_thread == thread_index) {
            setIpcReplyTokenForThread(i, false, 0);
        }
    }
    purgeIpcMessagesForThread(thread_index);
}

pub fn invalidateAllIpcFastpathState() void {
    var i: usize = 0;
    while (i < max_thread_slots) : (i += 1) {
        const ctx = getThreadContext(i) orelse continue;
        _ = ctx;
        clearIpcEndpointCacheForThread(i);
        setIpcReplyTokenForThread(i, false, 0);
        resetIpcQueueForThread(i);
        setIpcHotSignalPending(i, false);
    }
}

fn rawOwnerTag(ctx: *const ThreadContext) u8 {
    const raw: *const u8 = @ptrCast(&ctx.owner_process);
    return raw.*;
}

fn principalFromRawTag(raw: u8) ?kernel.PrincipalId {
    if (raw >= kernel.principal_count) return null;
    return @enumFromInt(raw);
}

pub fn threadContextLooksCorrupted(thread_index: usize) bool {
    const ctx = getThreadContextConst(thread_index) orelse return true;
    if (!ctx.allocated) return false;
    const owner_raw = rawOwnerTag(ctx);
    const owner = principalFromRawTag(owner_raw) orelse return true;
    if (owner != ctx.owner_process) return true;
    if (threadSlotForPrincipal(owner) != thread_index) return true;
    const cr3_addr = x86_platform.cr3AddressPart(ctx.cr3);
    if ((cr3_addr & 0xFFF) != 0) return true;
    if (cr3_addr == 0) return true;
    return false;
}

pub fn repairThreadContextWithSpaces(thread_index: usize, user_spaces: []UserAddressSpace, initial_frame: TrapFrame) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    const owner = ctx.owner_process;
    const was_ready = ctx.ready;
    const ok = initThreadContextWithSpaces(thread_index, owner, user_spaces, initial_frame);
    if (ok and !was_ready) {
        if (getThreadContext(thread_index)) |mutable| mutable.ready = false;
        setIpcHotReady(thread_index, false);
    }
    return ok;
}

pub fn sanitizeAllThreadContextsWithSpaces(user_spaces: []UserAddressSpace, initial_frame: TrapFrame) void {
    var i: usize = 0;
    while (i < max_thread_slots) : (i += 1) {
        const ctx = getThreadContext(i) orelse continue;
        if (!ctx.allocated) continue;
        const was_ready = ctx.ready;
        if (!repairThreadContextWithSpaces(i, user_spaces, initial_frame)) continue;
        if (!was_ready) {
            ctx.ready = false;
            setIpcHotReady(i, false);
        }
    }
}

pub fn activateThread(thread_index: usize) bool {
    if (!schedulerRunsOnCurrentCpu()) return false;
    const hot = getIpcHotThreadConst(thread_index) orelse return false;
    if (hot.allocated == 0) return false;
    if (hot.ready == 0) return false;
    const previous_thread = current_thread_index;
    current_thread_index = thread_index;
    const state = primarySchedulerState();
    state.lock.lock();
    defer state.lock.unlock();
    state.current_thread = thread_index;
    state.current_principal = hot.owner_process;
    state.current_cr3 = hot.cr3;
    state.is_idle = false;
    if (previous_thread != thread_index) state.schedule_ticks +%= 1;
    current_user_principal = hot.owner_process;
    user_cr3_value = hot.cr3;
    if (!applyThreadFsBase(thread_index)) return false;
    return true;
}

pub fn applyThreadFsBase(thread_index: usize) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    x86_platform.writeFsBase(ctx.fs_base);
    return true;
}

pub fn setCurrentThreadFsBase(fs_base: u64) bool {
    const ctx = getThreadContext(current_thread_index) orelse return false;
    if (!ctx.allocated) return false;
    ctx.fs_base = fs_base;
    x86_platform.writeFsBase(fs_base);
    return true;
}

pub fn setThreadFsBase(thread_index: usize, fs_base: u64) bool {
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    ctx.fs_base = fs_base;
    if (thread_index == current_thread_index) x86_platform.writeFsBase(fs_base);
    return true;
}

pub fn saveCurrentThreadContextFromFrame(frame: *const TrapFrame) void {
    const ctx = getThreadContext(current_thread_index) orelse return;
    if (!ctx.allocated) return;
    ctx.frame = frame.*;
    ctx.cr3 = user_cr3_value;
    setIpcHotCr3(current_thread_index, user_cr3_value);
    const hot = getIpcHotThreadConst(current_thread_index) orelse return;
    if (hot.wait_mailbox == 0 and hot.wake_tick == 0) {
        ctx.ready = true;
        setIpcHotReady(current_thread_index, true);
    }
}

pub fn loadThreadContextToFrame(thread_index: usize, frame: *TrapFrame) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    const hot = getIpcHotThreadConst(thread_index) orelse return false;
    if (hot.allocated == 0) return false;
    if (hot.ready == 0) return false;
    x86_platform.writeFsBase(ctx.fs_base);
    frame.* = ctx.frame;
    return true;
}

pub fn switchToThread(next_thread: usize, frame: *TrapFrame, saved_rax: ?u64) bool {
    if (!schedulerRunsOnCurrentCpu()) return false;
    if (next_thread >= max_thread_slots) return false;
    const current_thread = current_thread_index;
    if (next_thread == current_thread) {
        if (saved_rax) |value| frame.rax = value;
        return true;
    }

    var saved = frame.*;
    if (saved_rax) |value| saved.rax = value;
    saveCurrentThreadContextFromFrame(&saved);

    if (!activateThread(next_thread)) return false;
    if (!loadThreadContextToFrame(next_thread, frame)) {
        _ = activateThread(current_thread);
        _ = loadThreadContextToFrame(current_thread, frame);
        return false;
    }
    return true;
}

pub fn pickNextReadyThreadIndex(current_index: usize) usize {
    if (current_index >= max_thread_slots) return 0;
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    rebuildRunnableQueuesFromHotThreadsLocked();
    if (runtime_priority_active) {
        if (runtime_priority_principal) |priority_principal| {
            if (threadSlotForPrincipal(priority_principal)) |priority_slot| {
                const priority_hot = getIpcHotThreadConst(priority_slot) orelse return current_index;
                if (current_index != priority_slot and priority_hot.allocated != 0 and priority_hot.ready != 0 and threadAssignedCpuSlot(priority_slot) == bootstrap_cpu_slot) return priority_slot;
            }
        }
    }
    return primarySchedulerState().run_queue.pickNextAfter(current_index);
}

pub fn wakeThreadIfWaiting(thread_index: usize) void {
    const ctx = getThreadContext(thread_index) orelse return;
    if (!ctx.allocated) return;
    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    ctx.ready = true;
    setIpcHotWaitState(thread_index, false, 0, true);
}

pub fn preferIpcSwitchToThread(thread_index: usize) void {
    if (thread_index >= max_thread_slots) return;
    if (thread_index == current_thread_index) return;
    const target = getIpcHotThreadConst(thread_index) orelse return;
    if (target.allocated == 0) return;
    preferred_ipc_switch_threads[current_thread_index] = thread_index;
}

pub fn wakeWaitingThreadForPrincipal(principal: kernel.PrincipalId) void {
    const thread_index = threadSlotForPrincipal(principal) orelse return;
    const hot = getIpcHotThreadConst(thread_index) orelse return;
    if (hot.wait_mailbox == 0) return;
    wakeThreadIfWaiting(thread_index);
    preferIpcSwitchToThread(thread_index);
}

pub fn wakeBlockedThreadForPrincipal(principal: kernel.PrincipalId) void {
    const thread_index = threadSlotForPrincipal(principal) orelse return;
    const ctx = getThreadContext(thread_index) orelse return;
    const hot = getIpcHotThreadConst(thread_index) orelse return;
    if (hot.allocated == 0) return;
    if (hot.ready == 0) {
        wakeThreadIfWaiting(thread_index);
        preferIpcSwitchToThread(thread_index);
        return;
    }
    ctx.signal_pending = true;
    setIpcHotSignalPending(thread_index, true);
    preferIpcSwitchToThread(thread_index);
}

pub fn consumePendingSignalForPrincipal(principal: kernel.PrincipalId) bool {
    const thread_index = threadSlotForPrincipal(principal) orelse return false;
    const ctx = getThreadContext(thread_index) orelse return false;
    const hot = getIpcHotThreadConst(thread_index) orelse return false;
    if (hot.allocated == 0 or hot.signal_pending == 0) return false;
    ctx.signal_pending = false;
    setIpcHotSignalPending(thread_index, false);
    return true;
}

pub fn wakeThreadsForTimer(now_tick: u64) void {
    if (!schedulerRunsOnCurrentCpu()) return;
    var i: usize = 0;
    while (i < max_thread_slots) : (i += 1) {
        const hot = getIpcHotThreadConst(i) orelse continue;
        if (hot.allocated == 0) continue;
        if (hot.ready != 0) continue;
        if (hot.wake_tick == 0 or now_tick < hot.wake_tick) continue;
        wakeThreadIfWaiting(i);
    }
}

pub fn blockCurrentThreadForEvent(frame: *TrapFrame, wait_mailbox: bool, timeout_ticks: u64, resume_rax: u64) bool {
    if (!schedulerRunsOnCurrentCpu()) return false;
    const current_thread = current_thread_index;
    const ctx = getThreadContext(current_thread) orelse return false;
    const preferred_thread = preferred_ipc_switch_threads[current_thread];
    preferred_ipc_switch_threads[current_thread] = null;

    var saved = frame.*;
    saved.rax = resume_rax;
    ctx.frame = saved;
    ctx.cr3 = user_cr3_value;
    ctx.wait_mailbox = wait_mailbox;
    ctx.wake_tick = if (timeout_ticks == 0) 0 else lapic_tick_count + timeout_ticks;
    ctx.ready = false;
    setIpcHotCr3(current_thread, user_cr3_value);
    setIpcHotWaitState(current_thread, wait_mailbox, ctx.wake_tick, false);

    const next_thread = blk: {
        if (preferred_thread) |thread_index| {
            if (thread_index != current_thread) {
                if (getIpcHotThreadConst(thread_index)) |target_hot| {
                    if (target_hot.allocated != 0 and target_hot.ready != 0) break :blk thread_index;
                }
            }
        }
        break :blk pickNextReadyThreadIndex(current_thread);
    };
    if (next_thread == current_thread) {
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.ready = true;
        setIpcHotWaitState(current_thread, false, 0, true);
        return false;
    }
    if (!activateThread(next_thread)) {
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.ready = true;
        setIpcHotWaitState(current_thread, false, 0, true);
        return false;
    }
    if (!loadThreadContextToFrame(next_thread, frame)) {
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.ready = true;
        setIpcHotWaitState(current_thread, false, 0, true);
        _ = activateThread(current_thread);
        _ = loadThreadContextToFrame(current_thread, frame);
        return false;
    }
    return true;
}
