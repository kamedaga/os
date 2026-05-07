const std = @import("std");
const builtin = @import("builtin");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const interrupts = @import("interrupts.zig");
const smp = @import("smp.zig");
const build_workarounds = @import("build_workarounds");
const scheduler_observer = @import("scheduler_observer.zig");
const x86_platform = @import("arch/x86_64/platform.zig");
const kernel_log = @import("kernel_log.zig");
const log_util = @import("log_util.zig");

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
    ap_probe_user_entry_target_cycles: u64,
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
    ap_probe_user_entry_target_cycles: u64 = 0,
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
var ap_runnable_policy_enabled: bool = false;
var auto_cpu_choice_cursor: usize = 1;
pub var process_thread_slots: [kernel.process_count]?usize = [_]?usize{null} ** kernel.process_count;
pub export var user_cr3_value: u64 = 0;
pub export var current_user_principal: kernel.PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
pub export var current_thread_index: usize = 0;
pub export var user_cr3_values: [smp.max_cpus]u64 = [_]u64{0} ** smp.max_cpus;
pub export var current_user_principals: [smp.max_cpus]kernel.PrincipalId = [_]kernel.PrincipalId{kernel.processPrincipalFromIndex(0) orelse unreachable} ** smp.max_cpus;
pub export var current_thread_indices: [smp.max_cpus]usize = [_]usize{0} ** smp.max_cpus;
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

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(thread_contexts), &thread_contexts));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(thread_contexts_ptr), &thread_contexts_ptr));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ipc_hot_threads), &ipc_hot_threads));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ipc_hot_threads_ptr), &ipc_hot_threads_ptr));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ipc_queues), &ipc_queues));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(cpu_scheduler_states), &cpu_scheduler_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ap_runnable_policy_enabled), &ap_runnable_policy_enabled));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(auto_cpu_choice_cursor), &auto_cpu_choice_cursor));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(process_thread_slots), &process_thread_slots));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_cr3_value), &user_cr3_value));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(current_user_principal), &current_user_principal));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(current_thread_index), &current_thread_index));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_cr3_values), &user_cr3_values));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(current_user_principals), &current_user_principals));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(current_thread_indices), &current_thread_indices));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(lapic_tick_count), &lapic_tick_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_tick_accum), &scheduler_tick_accum));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_switch_count), &scheduler_switch_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_perf_user_ticks), &scheduler_perf_user_ticks));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_perf_priority_owner_ticks), &scheduler_perf_priority_owner_ticks));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_perf_priority_thread_ticks), &scheduler_perf_priority_thread_ticks));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_probe_log_count), &scheduler_probe_log_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(runtime_priority_streak), &runtime_priority_streak));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_int80_log_count), &scheduler_int80_log_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_race_log_count), &scheduler_race_log_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(runtime_priority_active), &runtime_priority_active));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(runtime_priority_principal), &runtime_priority_principal));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_fx_state), &initial_fx_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_interrupt_fx_state), &kernel_interrupt_fx_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(preferred_ipc_switch_threads), &preferred_ipc_switch_threads));
    return end;
}

fn mirrorBootstrapCurrentState(thread_index: usize, principal: kernel.PrincipalId, cr3: u64) void {
    current_thread_index = thread_index;
    current_user_principal = principal;
    user_cr3_value = cr3;
    current_thread_indices[bootstrap_cpu_slot] = thread_index;
    current_user_principals[bootstrap_cpu_slot] = principal;
    user_cr3_values[bootstrap_cpu_slot] = cr3;
}

fn mirrorBootstrapCurrentStateFromPerCpu() void {
    current_thread_index = current_thread_indices[bootstrap_cpu_slot];
    current_user_principal = current_user_principals[bootstrap_cpu_slot];
    user_cr3_value = user_cr3_values[bootstrap_cpu_slot];
}

fn syncBootstrapCurrentStateFromMirrorLocked() void {
    mirrorBootstrapCurrentStateFromPerCpu();
    const state = &cpu_scheduler_states[bootstrap_cpu_slot];
    state.current_thread = current_thread_index;
    state.current_principal = current_user_principal;
    state.current_cr3 = user_cr3_value;
    state.is_idle = false;
}

pub fn syncBootstrapCurrentStateFromMirror() void {
    const state = schedulerStateForSlot(bootstrap_cpu_slot) orelse return;
    state.lock.lock();
    defer state.lock.unlock();
    syncBootstrapCurrentStateFromMirrorLocked();
}

pub export fn schedulerSyncBootstrapCurrentStateFromMirrorFromAsm() callconv(.c) void {
    syncBootstrapCurrentStateFromMirror();
}

fn setCurrentExecutionForCpu(cpu_slot: usize, thread_index: usize, principal: kernel.PrincipalId, cr3: u64) bool {
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    if (!state.enabled) return false;
    state.current_thread = thread_index;
    state.current_principal = principal;
    state.current_cr3 = cr3;
    state.is_idle = false;
    current_thread_indices[cpu_slot] = thread_index;
    current_user_principals[cpu_slot] = principal;
    user_cr3_values[cpu_slot] = cr3;
    if (cpu_slot == bootstrap_cpu_slot) mirrorBootstrapCurrentState(thread_index, principal, cr3);
    return true;
}

pub fn currentThreadIndex() usize {
    const cpu_slot = currentCpuSlot();
    if (cpu_slot == bootstrap_cpu_slot) return current_thread_indices[bootstrap_cpu_slot];
    const state = schedulerStateForSlot(cpu_slot) orelse return current_thread_index;
    return state.current_thread;
}

pub fn currentUserPrincipal() kernel.PrincipalId {
    const cpu_slot = currentCpuSlot();
    if (cpu_slot == bootstrap_cpu_slot) return current_user_principals[bootstrap_cpu_slot];
    const state = schedulerStateForSlot(cpu_slot) orelse return current_user_principal;
    const thread_index = state.current_thread;
    if (getThreadContextConst(thread_index)) |ctx| {
        if (ctx.allocated) return ctx.owner_process;
    }
    return state.current_principal orelse current_user_principal;
}

pub fn currentUserCr3() u64 {
    const cpu_slot = currentCpuSlot();
    if (cpu_slot == bootstrap_cpu_slot) return user_cr3_values[bootstrap_cpu_slot];
    const state = schedulerStateForSlot(cpu_slot) orelse return user_cr3_value;
    const thread_index = state.current_thread;
    if (getThreadContextConst(thread_index)) |ctx| {
        if (ctx.allocated and ctx.cr3 != 0) return ctx.cr3;
    }
    if (state.current_cr3 != 0) return state.current_cr3;
    return user_cr3_value;
}

pub fn setCurrentExecutionFromHotThread(thread_index: usize) bool {
    const hot = getIpcHotThreadConst(thread_index) orelse return false;
    if (hot.allocated == 0) return false;
    return setCurrentExecutionForCpu(currentCpuSlot(), thread_index, hot.owner_process, hot.cr3);
}

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

fn clearCpuRunnableAcceptanceLocked(cpu_slot: usize, state: *CpuSchedulerState) void {
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
    state.ap_probe_user_entry_target_cycles = 0;
    state.ap_timer_saved_thread = null;
    state.ap_timer_saved_rip = 0;
    state.ap_timer_saved_rsp = 0;
    state.ap_preempt_from_thread = null;
    state.ap_preempt_to_thread = null;
    if (state.current_thread != state.idle_thread) {
        state.current_thread = state.idle_thread;
        state.current_principal = null;
        state.current_cr3 = 0;
        state.is_idle = true;
        current_thread_indices[cpu_slot] = state.idle_thread;
        current_user_principals[cpu_slot] = default_process_principal;
        user_cr3_values[cpu_slot] = 0;
    }
}

fn requestedRunnableAcceptanceAllowed(cpu_slot: usize, accepts: bool) bool {
    if (!accepts) return true;
    if (cpu_slot == bootstrap_cpu_slot) return true;
    return apUserSchedulingFeatureEnabled();
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
        const associated_with_cpu = state.current_thread == thread_index or
            state.pending_handoff_thread == thread_index or
            state.validated_handoff_thread == thread_index or
            state.consumed_handoff_thread == thread_index or
            state.entered_handoff_thread == thread_index;
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
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
        }
        if (associated_with_cpu) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
            current_thread_indices[cpu_slot] = state.idle_thread;
            current_user_principals[cpu_slot] = default_process_principal;
            user_cr3_values[cpu_slot] = 0;
        }
    }
}

fn removeRunnableQueueMembershipFromAllCpusLocked(thread_index: usize) void {
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
    }
}

fn threadAssociatedWithAnyCpuLocked(thread_index: usize) bool {
    var cpu_slot: usize = 0;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        const state = &cpu_scheduler_states[cpu_slot];
        if (state.current_thread == thread_index or
            state.pending_handoff_thread == thread_index or
            state.validated_handoff_thread == thread_index or
            state.consumed_handoff_thread == thread_index or
            state.entered_handoff_thread == thread_index)
        {
            return true;
        }
    }
    return false;
}

fn clearIdleApThreadAssociationsLocked(thread_index: usize) void {
    var cpu_slot: usize = 1;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        if (smp.cpuState(cpu_slot) == .user) continue;
        clearApThreadAssociationLocked(cpu_slot, &cpu_scheduler_states[cpu_slot], thread_index);
    }
}

fn clearApThreadAssociationLocked(cpu_slot: usize, state: *CpuSchedulerState, thread_index: usize) void {
    if (cpu_slot == bootstrap_cpu_slot) return;
    if (state.current_thread == thread_index) {
        state.current_thread = state.idle_thread;
        state.current_principal = null;
        state.current_cr3 = 0;
        state.is_idle = true;
        current_thread_indices[cpu_slot] = state.idle_thread;
        current_user_principals[cpu_slot] = default_process_principal;
        user_cr3_values[cpu_slot] = 0;
    }
    if (state.pending_handoff_thread == thread_index) {
        state.pending_handoff_thread = null;
        state.validated_handoff_thread = null;
        state.handoff_validation_code = handoff_validation_none;
    }
    if (state.validated_handoff_thread == thread_index) {
        state.validated_handoff_thread = null;
        state.handoff_validation_code = handoff_validation_none;
    }
    if (state.consumed_handoff_thread == thread_index) state.consumed_handoff_thread = null;
    if (state.entered_handoff_thread == thread_index) state.entered_handoff_thread = null;
    state.user_entry_requested = state.consumed_handoff_thread != null;
    state.handoff_runnable_count = state.run_queue.len;
    if (getThreadContextConst(thread_index)) |ctx| {
        if (ctx.allocated and ctx.ready and !ctx.abi_trap_reply_pending and ctx.cpu_slot == cpu_slot) {
            if (getIpcHotThread(thread_index)) |hot| {
                hot.ready = 1;
                hot.wait_mailbox = 0;
                hot.wake_tick = 0;
            }
            state.run_queue.markRunnable(thread_index);
            state.user_entry_requested = true;
            state.handoff_runnable_count = state.run_queue.len;
        }
    }
}

fn clearCurrentApThreadAssociationForBlock(cpu_slot: usize, thread_index: usize) void {
    if (cpu_slot == bootstrap_cpu_slot) return;
    const state = schedulerStateForSlot(cpu_slot) orelse return;
    state.lock.lock();
    defer state.lock.unlock();
    clearApThreadAssociationLocked(cpu_slot, state, thread_index);
}

pub fn prepareBlockedThreadForWake(thread_index: usize) void {
    if (!spawnExecApUserSchedulingReady()) return;
    const ctx = getThreadContextConst(thread_index) orelse return;
    if (!ctx.allocated) return;
    const cpu_slot = ctx.cpu_slot;
    if (cpu_slot == bootstrap_cpu_slot) return;
    const state = schedulerStateForSlot(cpu_slot) orelse return;
    state.lock.lock();
    defer state.lock.unlock();
    clearApThreadAssociationLocked(cpu_slot, state, thread_index);
}

fn threadReadyForCpuLocked(thread_index: usize, cpu_slot: usize) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    const hot = getIpcHotThreadConst(thread_index) orelse return false;
    if (!ctx.allocated or hot.allocated == 0) return false;
    if (!ctx.ready or hot.ready == 0 or ctx.abi_trap_reply_pending) return false;
    if (ctx.cpu_slot != cpu_slot) return false;
    return true;
}

fn enqueueRunnableThreadLocked(thread_index: usize) bool {
    const cpu_slot = threadAssignedCpuSlot(thread_index);
    if (!cpuAcceptsRunnableLocked(cpu_slot)) return false;
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.allocated or !ctx.ready or ctx.abi_trap_reply_pending) return false;
    if (threadAssociatedWithAnyCpuLocked(thread_index)) {
        clearIdleApThreadAssociationsLocked(thread_index);
        if (threadAssociatedWithAnyCpuLocked(thread_index)) return false;
    }
    const state = &cpu_scheduler_states[cpu_slot];
    if (state.consumed_handoff_thread == thread_index) return true;
    state.run_queue.markRunnable(thread_index);
    return true;
}

fn setRunnableQueueMembership(thread_index: usize, runnable: bool) void {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    removeRunnableQueueMembershipFromAllCpusLocked(thread_index);
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
    if (!apUserSchedulingFeatureEnabled()) return false;
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
        .fx_state_addr = @intFromPtr(&ctx.fx_state),
        .frame = ctx.frame,
    };
    state.current_thread = thread_index;
    state.current_principal = ctx.owner_process;
    state.current_cr3 = ctx.cr3;
    state.is_idle = false;
    current_thread_indices[cpu_slot] = thread_index;
    current_user_principals[cpu_slot] = ctx.owner_process;
    user_cr3_values[cpu_slot] = ctx.cr3;
    state.entered_handoff_thread = thread_index;
    state.user_entry_requested = false;
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
        if (i == bootstrap_cpu_slot) {
            state.runnable_acceptance_requested = true;
        } else {
            state.runnable_acceptance_requested = ap_runnable_policy_enabled and apUserSchedulingFeatureEnabled();
        }
        state.accepts_runnable = observed and state.runnable_acceptance_requested and requestedRunnableAcceptanceAllowed(i, true);
        if (!observed) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
            current_thread_indices[i] = state.idle_thread;
            current_user_principals[i] = default_process_principal;
            user_cr3_values[i] = 0;
        } else if (i != bootstrap_cpu_slot) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
            current_thread_indices[i] = state.idle_thread;
            current_user_principals[i] = default_process_principal;
            user_cr3_values[i] = 0;
        } else if (i == bootstrap_cpu_slot) {
            syncBootstrapCurrentStateFromMirrorLocked();
        }
    }
}

pub fn apUserSchedulingDiagnosticsEnabled() bool {
    return build_workarounds.scheduler_ap_user_diagnostics;
}

pub fn spawnExecApUserSchedulingEnabled() bool {
    return build_workarounds.spawn_exec_ap_user_scheduling;
}

fn apUserSchedulingFeatureEnabled() bool {
    return build_workarounds.scheduler_ap_user_diagnostics or build_workarounds.spawn_exec_ap_user_scheduling;
}

pub const SpawnExecApUserSchedulingBlock = enum(u8) {
    none,
    flag_disabled,
    ap_user_policy_disabled,
    no_ap,
    bootstrap_path,
};

pub fn spawnExecApUserSchedulingBlockReason() SpawnExecApUserSchedulingBlock {
    if (!build_workarounds.spawn_exec_ap_user_scheduling) return .flag_disabled;
    if (!apUserSchedulingFeatureEnabled()) return .ap_user_policy_disabled;
    if (smp.cpuCount() <= 1) return .no_ap;
    return .none;
}

pub fn spawnExecApUserSchedulingReady() bool {
    return spawnExecApUserSchedulingBlockReason() == .none;
}

pub fn apUserTimerPreemptionEnabled() bool {
    return apUserSchedulingFeatureEnabled() and build_workarounds.ap_user_timer_preemption;
}

pub fn cpuRunnableAcceptanceRequested(cpu_slot: usize) bool {
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    return state.runnable_acceptance_requested;
}

pub fn setCpuRunnableAcceptanceForDiagnostics(cpu_slot: usize, accepts: bool) bool {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    if (!state.enabled) return false;
    if (!requestedRunnableAcceptanceAllowed(cpu_slot, accepts)) return false;
    state.runnable_acceptance_requested = accepts;
    state.accepts_runnable = accepts;
    if (!accepts) clearCpuRunnableAcceptanceLocked(cpu_slot, state);
    rebuildRunnableQueuesFromHotThreadsLocked();
    return true;
}

pub fn setApRunnablePolicyEnabled(enabled: bool) bool {
    if (!apUserSchedulingFeatureEnabled()) return false;
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    return setApRunnablePolicyEnabledLocked(enabled);
}

fn setApRunnablePolicyEnabledLocked(enabled: bool) bool {
    ap_runnable_policy_enabled = enabled;
    var applied = false;
    var cpu_slot: usize = 1;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        const state = &cpu_scheduler_states[cpu_slot];
        if (!state.enabled and enabled) continue;
        state.runnable_acceptance_requested = enabled;
        state.accepts_runnable = enabled and state.enabled and requestedRunnableAcceptanceAllowed(cpu_slot, true);
        if (!enabled) clearCpuRunnableAcceptanceLocked(cpu_slot, state);
        if (state.enabled) applied = true;
    }
    rebuildRunnableQueuesFromHotThreadsLocked();
    return applied;
}

pub fn requestCpuUserEntry(cpu_slot: usize, enabled: bool) bool {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    if (!state.enabled) return false;
    if (!apUserSchedulingFeatureEnabled()) return false;
    if (cpu_slot == bootstrap_cpu_slot and enabled) return false;
    requestCpuUserEntryLocked(cpu_slot, state, enabled);
    return true;
}

fn requestCpuUserEntryLocked(cpu_slot: usize, state: *CpuSchedulerState, enabled: bool) void {
    state.user_entry_requested = enabled;
    if (!enabled) {
        state.entered_handoff_thread = null;
        return;
    }
    if (cpu_slot == bootstrap_cpu_slot) return;
    if (state.entered_handoff_thread != null) return;

    const idle_now = smp.cpuState(cpu_slot) != .user and
        state.current_thread == state.idle_thread and
        state.is_idle;
    if (idle_now and state.consumed_handoff_thread == null) {
        handoffNextThreadToIdleCpuLocked(cpu_slot, state);
    }
    if (state.consumed_handoff_thread != null) {
        state.user_entry_requested = true;
    }
}

pub fn setCpuProbeUserEntryTargetCycles(cpu_slot: usize, cycles: u64) bool {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    if (!state.enabled) return false;
    if (!apUserSchedulingFeatureEnabled()) return false;
    if (cpu_slot == bootstrap_cpu_slot and cycles != 0) return false;
    state.ap_probe_user_entry_target_cycles = cycles;
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

fn cpuCanRunThreadLocked(cpu_slot: usize, ctx: *const ThreadContext, allow_bootstrap: bool) bool {
    if (cpu_slot == bootstrap_cpu_slot and !allow_bootstrap) return false;
    if (cpu_slot >= cpu_scheduler_states.len) return false;
    if (!cpuAcceptsRunnableLocked(cpu_slot)) return false;
    const bit = cpuAffinityBit(cpu_slot) orelse return false;
    return (ctx.cpu_affinity_mask & bit) != 0;
}

fn chooseCpuForThreadLocked(thread_index: usize, allow_bootstrap: bool) ?usize {
    const ctx = getThreadContextConst(thread_index) orelse return null;
    if (!ctx.allocated) return null;
    var chosen: ?usize = null;
    var chosen_load: usize = std.math.maxInt(usize);
    const start = if (cpu_scheduler_states.len == 0) 0 else auto_cpu_choice_cursor % cpu_scheduler_states.len;
    var offset: usize = 0;
    while (offset < cpu_scheduler_states.len) : (offset += 1) {
        const cpu_slot = (start + offset) % cpu_scheduler_states.len;
        if (!cpuCanRunThreadLocked(cpu_slot, ctx, allow_bootstrap)) continue;
        const state = &cpu_scheduler_states[cpu_slot];
        var load = state.run_queue.len;
        if (!state.is_idle or
            state.current_thread != state.idle_thread or
            state.consumed_handoff_thread != null or
            state.entered_handoff_thread != null)
        {
            load += max_thread_slots;
        }
        if (state.pending_handoff_thread != null or state.validated_handoff_thread != null) {
            load += 1;
        }
        if (chosen == null or load < chosen_load) {
            chosen = cpu_slot;
            chosen_load = load;
        }
    }
    return chosen;
}

fn advanceAutoCpuChoiceCursor(chosen_cpu: usize) void {
    auto_cpu_choice_cursor = (chosen_cpu + 1) % cpu_scheduler_states.len;
    if (auto_cpu_choice_cursor == bootstrap_cpu_slot) auto_cpu_choice_cursor = (bootstrap_cpu_slot + 1) % cpu_scheduler_states.len;
}

pub fn chooseCpuForThread(thread_index: usize, allow_bootstrap: bool) ?usize {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    rebuildRunnableQueuesFromHotThreadsLocked();
    return chooseCpuForThreadLocked(thread_index, allow_bootstrap);
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

pub fn assignThreadToChosenCpu(thread_index: usize, allow_bootstrap: bool) ?usize {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    rebuildRunnableQueuesFromHotThreadsLocked();
    const cpu_slot = chooseCpuForThreadLocked(thread_index, allow_bootstrap) orelse return null;
    const ctx = getThreadContext(thread_index) orelse return null;
    const was_ready = ctx.ready;
    dequeueRunnableThreadFromAllCpusLocked(thread_index);
    ctx.cpu_slot = cpu_slot;
    if (was_ready and !enqueueRunnableThreadLocked(thread_index)) return null;
    advanceAutoCpuChoiceCursor(cpu_slot);
    return cpu_slot;
}

pub fn assignThreadPairToChosenCpu(first_thread: usize, second_thread: usize, allow_bootstrap: bool) ?usize {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    rebuildRunnableQueuesFromHotThreadsLocked();
    const cpu_slot = chooseCpuForThreadLocked(first_thread, allow_bootstrap) orelse return null;
    const first_ctx = getThreadContext(first_thread) orelse return null;
    const second_ctx = getThreadContext(second_thread) orelse return null;
    if (!first_ctx.allocated or !second_ctx.allocated) return null;
    if (!cpuCanRunThreadLocked(cpu_slot, second_ctx, allow_bootstrap)) return null;
    const first_was_ready = first_ctx.ready;
    const second_was_ready = second_ctx.ready;
    dequeueRunnableThreadFromAllCpusLocked(first_thread);
    dequeueRunnableThreadFromAllCpusLocked(second_thread);
    first_ctx.cpu_slot = cpu_slot;
    second_ctx.cpu_slot = cpu_slot;
    if (first_was_ready and !enqueueRunnableThreadLocked(first_thread)) return null;
    if (second_was_ready and !enqueueRunnableThreadLocked(second_thread)) return null;
    advanceAutoCpuChoiceCursor(cpu_slot);
    return cpu_slot;
}

pub fn assignSpawnExecThreadToApIfReady(thread_index: usize) ?usize {
    return assignReadyUserThreadToApIfReady(thread_index);
}

pub fn readySpawnExecThreadOnApIfReady(thread_index: usize) ?usize {
    if (!spawnExecApUserSchedulingReady()) return null;
    var chosen_cpu: ?usize = null;
    lockAllCpuSchedulerStates();
    {
        defer unlockAllCpuSchedulerStates();
        if (!setApRunnablePolicyEnabledLocked(true)) return null;
        const ctx = getThreadContext(thread_index) orelse return null;
        const hot = getIpcHotThread(thread_index) orelse return null;
        if (!ctx.allocated or hot.allocated == 0) return null;
        const cpu_slot = chooseCpuForThreadLocked(thread_index, false) orelse return null;
        dequeueRunnableThreadFromAllCpusLocked(thread_index);
        ctx.cpu_slot = cpu_slot;
        ctx.ready = true;
        hot.ready = 1;
        if (!enqueueRunnableThreadLocked(thread_index)) {
            ctx.ready = false;
            hot.ready = 0;
            return null;
        }
        const state = schedulerStateForSlot(cpu_slot) orelse return null;
        requestCpuUserEntryLocked(cpu_slot, state, true);
        advanceAutoCpuChoiceCursor(cpu_slot);
        chosen_cpu = cpu_slot;
    }
    if (chosen_cpu) |cpu_slot| _ = smp.wakeCpu(cpu_slot);
    return chosen_cpu;
}

pub fn assignReadyUserThreadToApIfReady(thread_index: usize) ?usize {
    if (!spawnExecApUserSchedulingReady()) return null;
    _ = setApRunnablePolicyEnabled(true);
    const cpu_slot = assignThreadToChosenCpu(thread_index, false) orelse return null;
    if (!requestCpuUserEntry(cpu_slot, true)) return null;
    _ = smp.wakeCpu(cpu_slot);
    return cpu_slot;
}

pub fn wakeAssignedApForRunnableThread(thread_index: usize) void {
    const ctx = getThreadContextConst(thread_index) orelse return;
    if (!ctx.allocated or !ctx.ready or ctx.abi_trap_reply_pending) return;
    const cpu_slot = ctx.cpu_slot;
    if (cpu_slot == bootstrap_cpu_slot) return;
    if (!spawnExecApUserSchedulingReady()) return;
    _ = setApRunnablePolicyEnabled(true);
    if (!requestCpuUserEntry(cpu_slot, true)) return;
    _ = smp.wakeCpu(cpu_slot);
}

fn parkCurrentApAfterBlocking(cpu_slot: usize, preserve_thread: ?usize) noreturn {
    if (schedulerStateForSlot(cpu_slot)) |state| {
        state.lock.lock();
        state.current_thread = state.idle_thread;
        state.current_principal = null;
        state.current_cr3 = 0;
        state.is_idle = true;
        state.pending_handoff_thread = null;
        state.handoff_runnable_count = state.run_queue.len;
        state.validated_handoff_thread = null;
        state.handoff_validation_code = handoff_validation_none;
        state.consumed_handoff_thread = null;
        state.entered_handoff_thread = null;
        state.user_entry_requested = false;
        if (preserve_thread) |thread| {
            if (threadReadyForCpuLocked(thread, cpu_slot)) {
                state.run_queue.markRunnable(thread);
            }
        }
        handoffNextThreadToIdleCpuLocked(cpu_slot, state);
        state.user_entry_requested = state.consumed_handoff_thread != null;
        current_thread_indices[cpu_slot] = state.idle_thread;
        current_user_principals[cpu_slot] = default_process_principal;
        user_cr3_values[cpu_slot] = 0;
        state.lock.unlock();
    }
    smp.returnCurrentApToIdleFromInterrupt();
}

pub fn parkCurrentApAfterCurrentThreadStopped() noreturn {
    const cpu_slot = currentCpuSlot();
    if (cpu_slot != bootstrap_cpu_slot and spawnExecApUserSchedulingReady()) {
        parkCurrentApAfterBlocking(cpu_slot, null);
    }
    while (true) {
        asm volatile ("cli; hlt");
    }
}

pub fn schedulerCpuInfo(cpu_slot: usize) ?CpuSchedulerInfo {
    if (cpu_slot >= cpu_scheduler_states.len) return null;
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    syncBootstrapCurrentStateFromMirrorLocked();
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
        .ap_probe_user_entry_target_cycles = state.ap_probe_user_entry_target_cycles,
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
    if (!apUserSchedulingFeatureEnabled()) return false;
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
    if (ctx.abi_trap_reply_pending) {
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
    current_thread_indices[cpu_slot] = state.idle_thread;
    current_user_principals[cpu_slot] = default_process_principal;
    user_cr3_values[cpu_slot] = 0;
    state.ap_timer_saved_thread = thread_index;
    state.ap_timer_saved_rip = frame.rip;
    state.ap_timer_saved_rsp = frame.rsp;
    state.ap_timer_save_ticks +%= 1;
    state.entered_handoff_thread = null;

    const reached_target = state.ap_probe_user_entry_target_cycles != 0 and state.ap_timer_save_ticks >= state.ap_probe_user_entry_target_cycles;
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
    }
    return true;
}

pub fn currentApUserThreadCanContinue() bool {
    if (!apUserSchedulingFeatureEnabled()) return true;
    const cpu_slot = currentCpuSlot();
    if (cpu_slot == bootstrap_cpu_slot) return true;
    const thread_index = currentThreadIndex();
    if (thread_index >= max_thread_slots) return false;
    const ctx = getThreadContextConst(thread_index) orelse return false;
    const hot = getIpcHotThreadConst(thread_index) orelse return false;
    return ctx.allocated and
        hot.allocated != 0 and
        ctx.ready and
        hot.ready != 0 and
        ctx.cpu_slot == cpu_slot and
        !ctx.abi_trap_reply_pending;
}

pub fn shouldPreemptCurrentApUserThread() bool {
    if (!apUserSchedulingFeatureEnabled()) return false;
    const cpu_slot = currentCpuSlot();
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    if (!state.enabled or !state.accepts_runnable) return false;
    return state.run_queue.len != 0;
}

fn handoffNextThreadToIdleCpu(cpu_slot: usize) void {
    if (!apUserSchedulingFeatureEnabled()) return;
    if (cpu_slot == bootstrap_cpu_slot) return;
    const state = schedulerStateForSlot(cpu_slot) orelse return;
    state.lock.lock();
    defer state.lock.unlock();
    handoffNextThreadToIdleCpuLocked(cpu_slot, state);
}

fn handoffNextThreadToIdleCpuLocked(cpu_slot: usize, state: *CpuSchedulerState) void {
    if (state.consumed_handoff_thread != null) return;
    if (!state.enabled or !state.accepts_runnable or state.run_queue.len == 0) {
        state.pending_handoff_thread = null;
        state.handoff_runnable_count = 0;
        state.validated_handoff_thread = null;
        state.handoff_validation_code = handoff_validation_none;
        return;
    }
    const next_thread = state.run_queue.pickFirst() orelse {
        state.pending_handoff_thread = null;
        state.handoff_runnable_count = 0;
        state.validated_handoff_thread = null;
        state.handoff_validation_code = handoff_validation_none;
        return;
    };
    state.handoff_runnable_count = state.run_queue.len;
    if (state.pending_handoff_thread != next_thread) {
        state.pending_handoff_thread = next_thread;
        state.handoff_ticks +%= 1;
    }
    validatePendingHandoffLocked(state, cpu_slot, next_thread);
    consumeValidatedHandoffLocked(state, next_thread);
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


fn validateThreadForCpuHandoff(cpu_slot: usize, thread_index: usize) u8 {
    if (thread_index >= max_thread_slots) return handoff_validation_bad_thread;
    const ctx = getThreadContextConst(thread_index) orelse return handoff_validation_bad_thread;
    const hot = getIpcHotThreadConst(thread_index) orelse return handoff_validation_bad_thread;
    if (!ctx.allocated or hot.allocated == 0) return handoff_validation_not_allocated;
    if (ctx.abi_trap_reply_pending) return handoff_validation_not_ready;
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
        const current_thread = currentThreadIndex();
        if (getThreadContextConst(current_thread)) |ctx| {
            if (ctx.allocated and ctx.owner_process == principal) scheduler_perf_priority_owner_ticks +%= 1;
        }
        if (threadSlotForPrincipal(principal)) |slot| {
            if (current_thread == slot) scheduler_perf_priority_thread_ticks +%= 1;
        }
    }
}

pub fn chooseNextThreadForTimerPreempt(quantum_ticks: u64, priority_hold_quanta: u64) ?usize {
    if (!schedulerRunsOnCurrentCpu()) return null;
    if (quantum_ticks == 0) return null;

    scheduler_tick_accum +%= 1;
    if (scheduler_tick_accum < quantum_ticks) return null;
    scheduler_tick_accum = 0;

    const current_thread = currentThreadIndex();
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
    setRunnableQueueMembership(thread_index, ctx.allocated and ctx.ready and !ctx.abi_trap_reply_pending);
}

pub fn setIpcHotReady(thread_index: usize, ready: bool) void {
    const runnable = blk: {
        if (getIpcHotThread(thread_index)) |hot| {
            hot.ready = boolByte(ready);
            const ctx = getThreadContextConst(thread_index) orelse break :blk false;
            break :blk ready and hot.allocated != 0 and !ctx.abi_trap_reply_pending;
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
        const ctx = getThreadContextConst(thread_index) orelse return;
        runnable = ready and hot.allocated != 0 and !ctx.abi_trap_reply_pending;
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
    const release_cpu_slot = ctx.cpu_slot;
    const releasing_from_cpu = currentCpuSlot();
    lockAllCpuSchedulerStates();
    const wake_mask = clearThreadFromCpuSchedulerStatesLocked(thread_index);
    unlockAllCpuSchedulerStates();
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
    stopReleasedThreadOnAp(release_cpu_slot, releasing_from_cpu, wake_mask);
    return true;
}

fn clearThreadFromCpuSchedulerStatesLocked(thread_index: usize) u64 {
    var wake_mask: u64 = 0;
    var cpu_slot: usize = 0;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        const state = &cpu_scheduler_states[cpu_slot];
        const associated_with_cpu = state.current_thread == thread_index or
            state.pending_handoff_thread == thread_index or
            state.validated_handoff_thread == thread_index or
            state.consumed_handoff_thread == thread_index or
            state.entered_handoff_thread == thread_index;
        const running_on_ap = cpu_slot != bootstrap_cpu_slot and
            (state.current_thread == thread_index or state.entered_handoff_thread == thread_index);
        state.run_queue.markBlocked(thread_index);
        if (associated_with_cpu) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
            current_thread_indices[cpu_slot] = state.idle_thread;
            current_user_principals[cpu_slot] = default_process_principal;
            user_cr3_values[cpu_slot] = 0;
            if (running_on_ap) {
                if (cpuAffinityBit(cpu_slot)) |bit| wake_mask |= bit;
            }
        }
        if (state.pending_handoff_thread == thread_index) state.pending_handoff_thread = null;
        if (state.validated_handoff_thread == thread_index) state.validated_handoff_thread = null;
        if (state.consumed_handoff_thread == thread_index) state.consumed_handoff_thread = null;
        if (state.entered_handoff_thread == thread_index) state.entered_handoff_thread = null;
        if (state.observed_next_thread == thread_index) state.observed_next_thread = null;
        if (state.ap_timer_saved_thread == thread_index) state.ap_timer_saved_thread = null;
        state.user_entry_requested = state.consumed_handoff_thread != null;
        state.handoff_runnable_count = state.run_queue.len;
        if (state.pending_handoff_thread == null) state.handoff_validation_code = handoff_validation_none;
    }
    return wake_mask;
}

fn stopReleasedThreadOnAp(release_cpu_slot: usize, releasing_from_cpu: usize, wake_mask: u64) void {
    if (!spawnExecApUserSchedulingReady()) return;
    var target_mask = wake_mask;
    if (release_cpu_slot != bootstrap_cpu_slot and release_cpu_slot != releasing_from_cpu) {
        if (cpuAffinityBit(release_cpu_slot)) |bit| target_mask |= bit;
    }
    if (target_mask == 0) return;
    var cpu_slot: usize = 1;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        const bit = cpuAffinityBit(cpu_slot) orelse {
            continue;
        };
        if ((target_mask & bit) == 0) continue;
        if (cpu_slot == releasing_from_cpu) continue;
        _ = smp.wakeCpu(cpu_slot);
        if (cpu_slot == release_cpu_slot) waitForReleasedApToPark(cpu_slot);
    }
}

fn waitForReleasedApToPark(cpu_slot: usize) void {
    var spins: usize = 0;
    while (spins < 20_000_000) : (spins += 1) {
        if (smp.cpuState(cpu_slot) != .user) return;
        asm volatile ("pause");
    }
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
    const queue = &ipc_queues[thread_index];
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

pub fn dequeueIpcReplyMessageForThreadFromSender(thread_index: usize, sender_thread: usize) ?IpcQueuedMessage {
    if (thread_index >= max_thread_slots or sender_thread >= max_thread_slots) return null;
    const queue = &ipc_queues[thread_index];
    if (queue.len == 0) return null;

    var offset: usize = 0;
    while (offset < queue.len) : (offset += 1) {
        const index = (@as(usize, queue.head) + offset) % max_ipc_queue_depth;
        const msg = queue.entries[index];
        if (msg.sender_thread != sender_thread or msg.endpoint_id != 0 or msg.grants_reply) continue;

        var shift = offset;
        while (shift + 1 < queue.len) : (shift += 1) {
            const dst = (@as(usize, queue.head) + shift) % max_ipc_queue_depth;
            const src = (@as(usize, queue.head) + shift + 1) % max_ipc_queue_depth;
            queue.entries[dst] = queue.entries[src];
        }
        const tail = (@as(usize, queue.head) + @as(usize, queue.len) - 1) % max_ipc_queue_depth;
        queue.entries[tail] = .{};
        queue.len -= 1;
        if (queue.len == 0) {
            if (getThreadContext(thread_index)) |ctx| {
                ctx.signal_pending = false;
            }
            setIpcHotSignalPending(thread_index, false);
        }
        return msg;
    }
    return null;
}

pub fn discardIpcReplyMessagesForThreadFromSender(thread_index: usize, sender_thread: usize) usize {
    if (thread_index >= max_thread_slots or sender_thread >= max_thread_slots) return 0;
    const queue = &ipc_queues[thread_index];
    if (queue.len == 0) return 0;

    var compacted: IpcQueue = .{};
    var removed: usize = 0;
    var offset: usize = 0;
    while (offset < queue.len) : (offset += 1) {
        const index = (@as(usize, queue.head) + offset) % max_ipc_queue_depth;
        const msg = queue.entries[index];
        if (msg.sender_thread == sender_thread and msg.endpoint_id == 0 and !msg.grants_reply) {
            removed += 1;
            continue;
        }
        const tail = (@as(usize, compacted.head) + @as(usize, compacted.len)) % max_ipc_queue_depth;
        compacted.entries[tail] = msg;
        compacted.len += 1;
    }
    queue.* = compacted;
    if (queue.len == 0) {
        if (getThreadContext(thread_index)) |ctx| {
            ctx.signal_pending = false;
        }
        setIpcHotSignalPending(thread_index, false);
    }
    return removed;
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
    const hot = getIpcHotThreadConst(thread_index) orelse return false;
    if (hot.allocated == 0) return false;
    if (hot.ready == 0) return false;
    const cpu_slot = currentCpuSlot();
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.allocated or ctx.cpu_slot != cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    if (!state.enabled) return false;
    const previous_thread = state.current_thread;
    state.current_thread = thread_index;
    state.current_principal = hot.owner_process;
    state.current_cr3 = hot.cr3;
    state.is_idle = false;
    if (cpu_slot != bootstrap_cpu_slot and spawnExecApUserSchedulingReady()) {
        state.run_queue.markBlocked(thread_index);
        state.pending_handoff_thread = null;
        state.handoff_runnable_count = state.run_queue.len;
        state.validated_handoff_thread = null;
        state.handoff_validation_code = handoff_validation_none;
        state.consumed_handoff_thread = thread_index;
        state.entered_handoff_thread = thread_index;
        state.user_entry_requested = false;
    }
    current_thread_indices[cpu_slot] = thread_index;
    current_user_principals[cpu_slot] = hot.owner_process;
    user_cr3_values[cpu_slot] = hot.cr3;
    if (previous_thread != thread_index) state.schedule_ticks +%= 1;
    if (cpu_slot == bootstrap_cpu_slot) mirrorBootstrapCurrentState(thread_index, hot.owner_process, hot.cr3);
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
    const ctx = getThreadContext(currentThreadIndex()) orelse return false;
    if (!ctx.allocated) return false;
    ctx.fs_base = fs_base;
    x86_platform.writeFsBase(fs_base);
    return true;
}

pub fn setThreadFsBase(thread_index: usize, fs_base: u64) bool {
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    ctx.fs_base = fs_base;
    if (thread_index == currentThreadIndex()) x86_platform.writeFsBase(fs_base);
    return true;
}

pub fn saveCurrentThreadContextFromFrame(frame: *const TrapFrame) void {
    const current_thread = currentThreadIndex();
    const ctx = getThreadContext(current_thread) orelse return;
    if (!ctx.allocated) return;
    ctx.frame = frame.*;
    ctx.cr3 = currentUserCr3();
    setIpcHotCr3(current_thread, ctx.cr3);
    const hot = getIpcHotThreadConst(current_thread) orelse return;
    if (hot.wait_mailbox == 0 and hot.wake_tick == 0) {
        ctx.ready = true;
        setIpcHotReady(current_thread, true);
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
    if (!schedulerRunsOnCurrentCpu() and !spawnExecApUserSchedulingReady()) return false;
    if (next_thread >= max_thread_slots) return false;
    const current_thread = currentThreadIndex();
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
    prepareBlockedThreadForWake(thread_index);
    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    ctx.ready = true;
    setIpcHotWaitState(thread_index, false, 0, true);
    wakeAssignedApForRunnableThread(thread_index);
}

pub fn preferIpcSwitchToThread(thread_index: usize) void {
    if (thread_index >= max_thread_slots) return;
    const current_thread = currentThreadIndex();
    if (thread_index == current_thread) return;
    const target = getIpcHotThreadConst(thread_index) orelse return;
    if (target.allocated == 0) return;
    preferred_ipc_switch_threads[current_thread] = thread_index;
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
    if (!schedulerRunsOnCurrentCpu() and !spawnExecApUserSchedulingReady()) return false;
    const current_thread = currentThreadIndex();
    const ctx = getThreadContext(current_thread) orelse return false;
    const preferred_thread = preferred_ipc_switch_threads[current_thread];
    preferred_ipc_switch_threads[current_thread] = null;

    var saved = frame.*;
    saved.rax = resume_rax;
    ctx.frame = saved;
    ctx.cr3 = currentUserCr3();
    ctx.wait_mailbox = wait_mailbox;
    ctx.wake_tick = if (timeout_ticks == 0) 0 else lapic_tick_count + timeout_ticks;
    ctx.ready = false;
    setIpcHotCr3(current_thread, ctx.cr3);
    setIpcHotWaitState(current_thread, wait_mailbox, ctx.wake_tick, false);
    if (ctx.abi_trap_reply_pending and ctx.signal_pending) {
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.ready = true;
        setIpcHotWaitState(current_thread, false, 0, true);
        return false;
    }
    clearCurrentApThreadAssociationForBlock(currentCpuSlot(), current_thread);

    const next_thread = blk: {
        const cpu_slot = currentCpuSlot();
        if (preferred_thread) |thread_index| {
            if (thread_index != current_thread) {
                if (getIpcHotThreadConst(thread_index)) |target_hot| {
                    if (target_hot.allocated != 0 and target_hot.ready != 0 and threadAssignedCpuSlot(thread_index) == cpu_slot) break :blk thread_index;
                }
            }
        }
        if (cpu_slot == bootstrap_cpu_slot) {
            break :blk pickNextReadyThreadIndex(current_thread);
        }
        break :blk pickNextReadyThreadIndexForCpu(cpu_slot, current_thread) orelse current_thread;
    };
    if (next_thread == current_thread) {
        const cpu_slot = currentCpuSlot();
        if (cpu_slot != bootstrap_cpu_slot and spawnExecApUserSchedulingReady()) {
            if (threadReadyForCpuLocked(current_thread, cpu_slot)) {
                if (activateThread(current_thread) and loadThreadContextToFrame(current_thread, frame)) {
                    return true;
                }
            }
            parkCurrentApAfterBlocking(cpu_slot, current_thread);
        }
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
