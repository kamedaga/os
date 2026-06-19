const std = @import("std");
const builtin = @import("builtin");
const kernel = @import("kernel.zig");
const address_space = @import("memory/address_space.zig");
const interrupts = @import("interrupts.zig");
const smp = @import("smp.zig");
const build_workarounds = @import("build_workarounds");
const scheduler_observer = @import("scheduler_observer.zig");
const x86_platform = @import("arch/x86_64/platform.zig");
const kernel_log = @import("kernel_log.zig");
const log_util = @import("log_util.zig");

const TrapFrame = interrupts.TrapFrame;
const UserAddressSpace = address_space.UserAddressSpace;
const default_process_principal: kernel.PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
const bootstrap_cpu_slot: usize = 0;
const all_cpu_affinity_mask: u64 = if (smp.max_cpus >= 64) std.math.maxInt(u64) else (@as(u64, 1) << smp.max_cpus) - 1;

const fx_state_bytes: usize = 512;
pub const max_thread_slots: usize = kernel.max_thread_slots;
pub const initial_thread_capacity: usize = kernel.initial_thread_capacity;
pub const idle_thread_marker: usize = max_thread_slots;

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

fn schedulerFullMemoryFence() void {
    asm volatile ("mfence" ::: .{ .memory = true });
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

var empty_runnable_storage: [0]bool = .{};
var initial_run_queue_storage: [smp.max_cpus][initial_thread_capacity]bool = [_][initial_thread_capacity]bool{[_]bool{false} ** initial_thread_capacity} ** smp.max_cpus;

const RunQueue = struct {
    runnable: []bool = empty_runnable_storage[0..],
    len: usize = 0,

    fn markRunnable(self: *RunQueue, thread_index: usize) void {
        if (thread_index >= self.runnable.len) return;
        if (!self.runnable[thread_index]) {
            self.runnable[thread_index] = true;
            self.len += 1;
        }
    }

    fn markBlocked(self: *RunQueue, thread_index: usize) void {
        if (thread_index >= self.runnable.len) return;
        if (self.runnable[thread_index]) {
            self.runnable[thread_index] = false;
            self.len -= 1;
        }
    }

    fn contains(self: *const RunQueue, thread_index: usize) bool {
        return thread_index < self.runnable.len and self.runnable[thread_index];
    }

    fn pickFirst(self: *const RunQueue) ?usize {
        var i: usize = 0;
        while (i < self.runnable.len) : (i += 1) {
            if (self.contains(i)) return i;
        }
        return null;
    }

    fn pickNextAfter(self: *const RunQueue, current_index: usize) usize {
        if (current_index >= self.runnable.len) return self.pickFirst() orelse 0;
        var step: usize = 1;
        while (step <= self.runnable.len) : (step += 1) {
            const idx = (current_index + step) % self.runnable.len;
            if (self.contains(idx)) return idx;
        }
        return current_index;
    }
};

const CpuSchedulerState = struct {
    lock: SchedulerSpinLock = .{},
    run_queue: RunQueue = .{},
    current_thread: usize = idle_thread_marker,
    current_principal: ?kernel.PrincipalId = null,
    current_cr3: u64 = 0,
    idle_thread: usize = idle_thread_marker,
    pending_handoff_thread: ?usize = null,
    validated_handoff_thread: ?usize = null,
    consumed_handoff_thread: ?usize = null,
    entered_handoff_thread: ?usize = null,
    user_entry_requested: bool = false,
    enabled: bool = false,
    accepts_runnable: bool = false,
    runnable_acceptance_requested: bool = false,
    is_idle: bool = true,
};

pub const ThreadContext = struct {
    id: u32 = 0,
    generation: u32 = 1,
    allocated: bool = false,
    owner_process: kernel.PrincipalId = default_process_principal,
    cpu_slot: usize = bootstrap_cpu_slot,
    cpu_affinity_mask: u64 = all_cpu_affinity_mask,
    cr3: u64 = 0,
    fs_base: u64 = 0,
    gs_base: u64 = 0,
    pkru: u32 = 0,
    ready: bool = false,
    wait_mailbox: bool = false,
    signal_pending: bool = false,
    wake_tick: u64 = 0,
    frame: TrapFrame = std.mem.zeroes(TrapFrame),
    fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes,
};

pub const ThreadHotState = extern struct {
    allocated: u8 = 0,
    ready: u8 = 0,
    signal_pending: u8 = 0,
    wait_mailbox: u8 = 0,
    owner_process: kernel.PrincipalId = default_process_principal,
    _pad0: [8]u8 = [_]u8{0} ** 8,
    wake_tick: u64 = 0,
    cr3: u64 = 0,
};

fn buildInitialThreadContexts() [initial_thread_capacity]ThreadContext {
    var contexts: [initial_thread_capacity]ThreadContext = undefined;
    inline for (0..initial_thread_capacity) |i| {
        contexts[i] = .{ .id = @intCast(i) };
    }
    return contexts;
}

fn buildInitialThreadHotStates() [initial_thread_capacity]ThreadHotState {
    var hot: [initial_thread_capacity]ThreadHotState = undefined;
    inline for (0..initial_thread_capacity) |i| {
        hot[i] = .{};
    }
    return hot;
}

var initial_thread_contexts: [initial_thread_capacity]ThreadContext = buildInitialThreadContexts();
pub var thread_contexts: []ThreadContext = initial_thread_contexts[0..];
pub export var thread_contexts_ptr: *anyopaque = @ptrCast(initial_thread_contexts[0..].ptr);
var initial_thread_hot_states: [initial_thread_capacity]ThreadHotState = buildInitialThreadHotStates();
pub var thread_hot_states: []ThreadHotState = initial_thread_hot_states[0..];
pub export var thread_hot_states_ptr: *anyopaque = @ptrCast(initial_thread_hot_states[0..].ptr);
var cpu_scheduler_states: [smp.max_cpus]CpuSchedulerState = buildInitialCpuSchedulerStates();
var ap_runnable_policy_enabled: bool = false;
pub export var lapic_tick_count: u64 = 0;
pub var scheduler_tick_accum: u64 = 0;
pub var scheduler_switch_count: u64 = 0;
pub var initial_fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes;
pub var kernel_interrupt_fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes;

fn nextThreadGeneration(current: u32) u32 {
    const next = current +% 1;
    return if (next == 0) 1 else next;
}

pub fn initStaticStorage() void {
    var cpu_slot: usize = 0;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        @memset(initial_run_queue_storage[cpu_slot][0..], false);
        cpu_scheduler_states[cpu_slot].run_queue = .{
            .runnable = initial_run_queue_storage[cpu_slot][0..],
            .len = 0,
        };
    }
    cpu_scheduler_states[bootstrap_cpu_slot].enabled = true;
    cpu_scheduler_states[bootstrap_cpu_slot].accepts_runnable = true;
    cpu_scheduler_states[bootstrap_cpu_slot].runnable_acceptance_requested = true;
    cpu_scheduler_states[bootstrap_cpu_slot].current_thread = 0;
    cpu_scheduler_states[bootstrap_cpu_slot].is_idle = false;
}

pub fn threadSlotCapacity() usize {
    return thread_contexts.len;
}

fn allocKernelSlice(comptime T: type, free_list: *kernel.FreePageList, count: usize) ?[]T {
    if (count == 0) return null;
    const bytes = @sizeOf(T) * count;
    const page_count = (bytes + 4095) / 4096;
    const paddr = free_list.popContiguousAtOrAbove(page_count, 0) catch return null;
    const raw: [*]u8 = @ptrFromInt(paddr);
    @memset(raw[0 .. page_count * 4096], 0);
    const ptr: [*]T = @ptrCast(@alignCast(raw));
    return ptr[0..count];
}

fn nextThreadCapacity(required: usize) ?usize {
    if (required > max_thread_slots) return null;
    var capacity = thread_contexts.len;
    if (capacity == 0) capacity = initial_thread_capacity;
    while (capacity < required) {
        const doubled = capacity * 2;
        capacity = if (doubled > max_thread_slots) max_thread_slots else doubled;
        if (capacity < required and capacity == max_thread_slots) return null;
    }
    return capacity;
}

fn ensureThreadCapacity(required: usize, free_list: *kernel.FreePageList) bool {
    if (required <= thread_contexts.len) return true;
    const capacity = nextThreadCapacity(required) orelse return false;
    const new_contexts = allocKernelSlice(ThreadContext, free_list, capacity) orelse return false;
    const new_hot_threads = allocKernelSlice(ThreadHotState, free_list, capacity) orelse return false;
    const new_run_queue_storage = allocKernelSlice(bool, free_list, capacity * smp.max_cpus) orelse return false;

    @memcpy(new_contexts[0..thread_contexts.len], thread_contexts);
    @memcpy(new_hot_threads[0..thread_hot_states.len], thread_hot_states);
    var i = thread_contexts.len;
    while (i < capacity) : (i += 1) {
        new_contexts[i] = .{ .id = @intCast(i) };
        new_hot_threads[i] = .{};
    }

    var cpu_slot: usize = 0;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        const start = cpu_slot * capacity;
        const new_runnable = new_run_queue_storage[start .. start + capacity];
        @memset(new_runnable, false);
        const old_runnable = cpu_scheduler_states[cpu_slot].run_queue.runnable;
        @memcpy(new_runnable[0..old_runnable.len], old_runnable);
        cpu_scheduler_states[cpu_slot].run_queue.runnable = new_runnable;
    }

    thread_contexts = new_contexts;
    thread_hot_states = new_hot_threads;
    thread_contexts_ptr = @ptrCast(thread_contexts.ptr);
    thread_hot_states_ptr = @ptrCast(thread_hot_states.ptr);
    return true;
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_thread_contexts), &initial_thread_contexts));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(thread_contexts), &thread_contexts));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(thread_contexts_ptr), &thread_contexts_ptr));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_thread_hot_states), &initial_thread_hot_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(thread_hot_states), &thread_hot_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(thread_hot_states_ptr), &thread_hot_states_ptr));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(cpu_scheduler_states), &cpu_scheduler_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(empty_runnable_storage), &empty_runnable_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_run_queue_storage), &initial_run_queue_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ap_runnable_policy_enabled), &ap_runnable_policy_enabled));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(lapic_tick_count), &lapic_tick_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_tick_accum), &scheduler_tick_accum));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_switch_count), &scheduler_switch_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_fx_state), &initial_fx_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_interrupt_fx_state), &kernel_interrupt_fx_state));
    return end;
}

pub fn currentThreadIndex() usize {
    const state = schedulerStateForSlot(currentCpuSlot()) orelse return 0;
    return state.current_thread;
}

pub fn currentUserPrincipal() kernel.PrincipalId {
    const state = schedulerStateForSlot(currentCpuSlot()) orelse return default_process_principal;
    const thread_index = state.current_thread;
    if (getThreadContextConst(thread_index)) |ctx| {
        if (ctx.allocated) return ctx.owner_process;
    }
    return state.current_principal orelse default_process_principal;
}

pub fn currentUserCr3() u64 {
    const state = schedulerStateForSlot(currentCpuSlot()) orelse return 0;
    const thread_index = state.current_thread;
    if (getThreadContextConst(thread_index)) |ctx| {
        if (ctx.allocated and ctx.cr3 != 0) return ctx.cr3;
    }
    return state.current_cr3;
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
        @memset(cpu_scheduler_states[cpu_slot].run_queue.runnable, false);
        cpu_scheduler_states[cpu_slot].run_queue.len = 0;
    }
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
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

fn removeRunnableQueueMembershipFromAllCpusLocked(thread_index: usize) void {
    var cpu_slot: usize = 0;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        const state = &cpu_scheduler_states[cpu_slot];
        state.run_queue.markBlocked(thread_index);
        if (state.pending_handoff_thread == thread_index) {
            state.pending_handoff_thread = null;
            state.validated_handoff_thread = null;
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
    }
    if (state.pending_handoff_thread == thread_index) {
        state.pending_handoff_thread = null;
        state.validated_handoff_thread = null;
    }
    if (state.validated_handoff_thread == thread_index) {
        state.validated_handoff_thread = null;
    }
    if (state.consumed_handoff_thread == thread_index) state.consumed_handoff_thread = null;
    if (state.entered_handoff_thread == thread_index) state.entered_handoff_thread = null;
    state.user_entry_requested = state.consumed_handoff_thread != null;
    if (getThreadContextConst(thread_index)) |ctx| {
        if (ctx.allocated and ctx.ready and ctx.cpu_slot == cpu_slot) {
            if (getThreadHotState(thread_index)) |hot| {
                hot.ready = 1;
                hot.wait_mailbox = 0;
                hot.wake_tick = 0;
            }
            state.run_queue.markRunnable(thread_index);
            state.user_entry_requested = true;
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

fn prepareBlockedThreadForWake(thread_index: usize) void {
    if (!userApSchedulingReady()) return;
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
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    if (!ctx.allocated or hot.allocated == 0) return false;
    if (!ctx.ready or hot.ready == 0) return false;
    if (ctx.cpu_slot != cpu_slot) return false;
    return true;
}

fn enqueueRunnableThreadLocked(thread_index: usize) bool {
    const cpu_slot = threadAssignedCpuSlot(thread_index);
    if (!cpuAcceptsRunnableLocked(cpu_slot)) return false;
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.allocated or !ctx.ready) return false;
    if (threadAssociatedWithAnyCpuLocked(thread_index)) {
        clearIdleApThreadAssociationsLocked(thread_index);
        if (threadAssociatedWithAnyCpuLocked(thread_index)) return false;
    }
    const state = &cpu_scheduler_states[cpu_slot];
    if (state.consumed_handoff_thread == thread_index) return true;
    state.run_queue.markRunnable(thread_index);
    return true;
}

pub fn threadAssociatedWithAnyCpu(thread_index: usize) bool {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    return threadAssociatedWithAnyCpuLocked(thread_index);
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
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    if (!state.user_entry_requested) return false;
    if (state.entered_handoff_thread != null) return false;
    const thread_index = state.consumed_handoff_thread orelse return false;
    if (!validateThreadForCpuHandoff(cpu_slot, thread_index)) return false;
    const ctx = getThreadContextConst(thread_index) orelse return false;
    out_entry.* = .{
        .cpu_slot = cpu_slot,
        .thread_index = thread_index,
        .cr3 = ctx.cr3,
        .fs_base = ctx.fs_base,
        .gs_base = ctx.gs_base,
        .fx_state_addr = @intFromPtr(&ctx.fx_state),
        .pkru = ctx.pkru,
        .frame = ctx.frame,
    };
    state.current_thread = thread_index;
    state.current_principal = ctx.owner_process;
    state.current_cr3 = ctx.cr3;
    state.is_idle = false;
    state.entered_handoff_thread = thread_index;
    state.user_entry_requested = false;
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
        state.runnable_acceptance_requested = if (i == bootstrap_cpu_slot) true else ap_runnable_policy_enabled;
        state.accepts_runnable = observed and state.runnable_acceptance_requested;
        if (!observed or i != bootstrap_cpu_slot) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
        }
    }
}

pub fn userApSchedulingReady() bool {
    return smp.cpuCount() > 1;
}

pub fn apUserTimerPreemptionEnabled() bool {
    return build_workarounds.ap_user_timer_preemption;
}

fn enableApRunnablePolicyLocked() bool {
    ap_runnable_policy_enabled = true;
    var applied = false;
    var cpu_slot: usize = 1;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        const state = &cpu_scheduler_states[cpu_slot];
        if (!state.enabled) continue;
        state.runnable_acceptance_requested = true;
        state.accepts_runnable = true;
        applied = true;
    }
    rebuildRunnableQueuesFromHotThreadsLocked();
    return applied;
}

fn requestCpuUserEntryLocked(cpu_slot: usize, state: *CpuSchedulerState) void {
    if (cpu_slot == bootstrap_cpu_slot) return;
    if (!state.enabled) return;
    state.user_entry_requested = true;
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

fn wakeAssignedApForRunnableThread(thread_index: usize) void {
    const ctx = getThreadContextConst(thread_index) orelse return;
    if (!ctx.allocated or !ctx.ready) return;
    const cpu_slot = ctx.cpu_slot;
    if (cpu_slot == bootstrap_cpu_slot) return;
    if (!userApSchedulingReady()) return;
    lockAllCpuSchedulerStates();
    {
        defer unlockAllCpuSchedulerStates();
        if (!enableApRunnablePolicyLocked()) return;
        const refreshed_ctx = getThreadContextConst(thread_index) orelse return;
        if (!refreshed_ctx.allocated or !refreshed_ctx.ready) return;
        if (refreshed_ctx.cpu_slot != cpu_slot) return;
        _ = enqueueRunnableThreadLocked(thread_index);
        const state = schedulerStateForSlot(cpu_slot) orelse return;
        if (!state.enabled) return;
        requestCpuUserEntryLocked(cpu_slot, state);
    }
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
        state.validated_handoff_thread = null;
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
        state.lock.unlock();
    }
    smp.returnCurrentApToIdleFromInterrupt();
}

pub fn parkCurrentApAfterCurrentThreadStopped() noreturn {
    const cpu_slot = currentCpuSlot();
    if (cpu_slot != bootstrap_cpu_slot and userApSchedulingReady()) {
        parkCurrentApAfterBlocking(cpu_slot, null);
    }
    while (true) {
        asm volatile ("cli; hlt");
    }
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
}

pub fn saveApUserTimerFrame(frame: *const TrapFrame) bool {
    const cpu_slot = currentCpuSlot();
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    const thread_index = state.entered_handoff_thread orelse state.current_thread;
    if (thread_index >= thread_contexts.len) return false;
    const ctx = getThreadContext(thread_index) orelse return false;
    const hot = getThreadHotState(thread_index) orelse return false;
    if (!ctx.allocated or hot.allocated == 0) return false;
    ctx.frame = frame.*;
    ctx.pkru = x86_platform.readPkru();
    state.consumed_handoff_thread = null;
    state.pending_handoff_thread = null;
    state.validated_handoff_thread = null;
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
    state.entered_handoff_thread = null;

    ctx.ready = true;
    hot.ready = 1;
    state.run_queue.markRunnable(thread_index);

    const next_thread = state.run_queue.pickNextAfter(thread_index);
    if (!validateThreadForCpuHandoff(cpu_slot, next_thread)) {
        state.validated_handoff_thread = null;
        state.user_entry_requested = false;
        return true;
    }

    state.pending_handoff_thread = next_thread;
    state.validated_handoff_thread = next_thread;
    consumeValidatedHandoffLocked(state, next_thread);
    if (state.consumed_handoff_thread == next_thread) {
        state.user_entry_requested = true;
    }
    return true;
}

pub fn currentApUserThreadCanContinue() bool {
    const cpu_slot = currentCpuSlot();
    if (cpu_slot == bootstrap_cpu_slot) return true;
    const thread_index = currentThreadIndex();
    if (thread_index >= thread_contexts.len) return false;
    const ctx = getThreadContextConst(thread_index) orelse return false;
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    return ctx.allocated and
        hot.allocated != 0 and
        ctx.ready and
        hot.ready != 0 and
        ctx.cpu_slot == cpu_slot;
}

pub fn shouldPreemptCurrentApUserThread() bool {
    const cpu_slot = currentCpuSlot();
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    if (!state.enabled or !state.accepts_runnable) return false;
    return state.run_queue.len != 0;
}

fn handoffNextThreadToIdleCpu(cpu_slot: usize) void {
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
        state.validated_handoff_thread = null;
        return;
    }
    const next_thread = state.run_queue.pickFirst() orelse {
        state.pending_handoff_thread = null;
        state.validated_handoff_thread = null;
        return;
    };
    state.pending_handoff_thread = next_thread;
    validatePendingHandoffLocked(state, cpu_slot, next_thread);
    consumeValidatedHandoffLocked(state, next_thread);
}

fn validatePendingHandoffLocked(state: *CpuSchedulerState, cpu_slot: usize, thread_index: usize) void {
    if (validateThreadForCpuHandoff(cpu_slot, thread_index)) {
        state.validated_handoff_thread = thread_index;
    } else {
        state.validated_handoff_thread = null;
    }
}

fn consumeValidatedHandoffLocked(state: *CpuSchedulerState, thread_index: usize) void {
    if (state.validated_handoff_thread != thread_index) return;
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
    }
}

fn validateThreadForCpuHandoff(cpu_slot: usize, thread_index: usize) bool {
    if (thread_index >= thread_contexts.len) return false;
    const ctx = getThreadContextConst(thread_index) orelse return false;
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    if (!ctx.allocated or hot.allocated == 0) return false;
    if (!ctx.ready or hot.ready == 0) return false;
    if (ctx.cpu_slot != cpu_slot) return false;
    const affinity_bit = cpuAffinityBit(cpu_slot) orelse return false;
    if ((ctx.cpu_affinity_mask & affinity_bit) == 0) return false;
    if (hot.owner_process != ctx.owner_process) return false;
    const cr3_addr = x86_platform.cr3AddressPart(ctx.cr3);
    if (ctx.cr3 != hot.cr3 or cr3_addr == 0 or (cr3_addr & 0xFFF) != 0) return false;
    if (ctx.frame.rip == 0 or ctx.frame.rsp == 0) return false;
    return true;
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

pub fn chooseNextThreadForTimerPreempt(quantum_ticks: u64) ?usize {
    if (!schedulerRunsOnCurrentCpu()) return null;
    if (quantum_ticks == 0) return null;

    scheduler_tick_accum +%= 1;
    if (scheduler_tick_accum < quantum_ticks) return null;
    scheduler_tick_accum = 0;

    const current_thread = currentThreadIndex();
    const next_thread = pickNextReadyThreadIndex(current_thread);
    if (next_thread == current_thread) return null;
    return next_thread;
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
    if (thread_index >= thread_contexts.len) return null;
    return &thread_contexts[thread_index];
}

pub fn getThreadContextConst(thread_index: usize) ?*const ThreadContext {
    if (thread_index >= thread_contexts.len) return null;
    return &thread_contexts[thread_index];
}

pub fn getThreadHotState(thread_index: usize) ?*ThreadHotState {
    if (thread_index >= thread_contexts.len) return null;
    return &thread_hot_states[thread_index];
}

pub fn getThreadHotStateConst(thread_index: usize) ?*const ThreadHotState {
    if (thread_index >= thread_contexts.len) return null;
    return &thread_hot_states[thread_index];
}

fn boolByte(value: bool) u8 {
    return if (value) 1 else 0;
}

fn hotStateFromContext(ctx: *const ThreadContext) ThreadHotState {
    return .{
        .allocated = boolByte(ctx.allocated),
        .ready = boolByte(ctx.ready),
        .signal_pending = boolByte(ctx.signal_pending),
        .wait_mailbox = boolByte(ctx.wait_mailbox),
        .owner_process = ctx.owner_process,
        .wake_tick = ctx.wake_tick,
        .cr3 = ctx.cr3,
    };
}

fn syncHotStateFromContext(thread_index: usize) void {
    const ctx = getThreadContextConst(thread_index) orelse return;
    const hot = getThreadHotState(thread_index) orelse return;
    hot.* = hotStateFromContext(ctx);
    setRunnableQueueMembership(thread_index, ctx.allocated and ctx.ready);
}

pub fn setThreadHotReady(thread_index: usize, ready: bool) void {
    const runnable = blk: {
        if (getThreadHotState(thread_index)) |hot| {
            hot.ready = boolByte(ready);
            break :blk ready and hot.allocated != 0;
        }
        break :blk false;
    };
    setRunnableQueueMembership(thread_index, runnable);
}

pub fn setThreadHotSignalPending(thread_index: usize, pending: bool) void {
    if (getThreadHotState(thread_index)) |hot| hot.signal_pending = boolByte(pending);
}

pub fn setThreadHotWaitState(thread_index: usize, wait_mailbox: bool, wake_tick: u64, ready: bool) void {
    var runnable = false;
    if (getThreadHotState(thread_index)) |hot| {
        hot.wait_mailbox = boolByte(wait_mailbox);
        hot.wake_tick = wake_tick;
        hot.ready = boolByte(ready);
        runnable = ready and hot.allocated != 0;
    }
    setRunnableQueueMembership(thread_index, runnable);
}

pub fn setThreadHotCr3(thread_index: usize, cr3: u64) void {
    if (getThreadHotState(thread_index)) |hot| hot.cr3 = cr3;
}

pub fn isThreadReady(thread_index: usize) bool {
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    return hot.allocated != 0 and hot.ready != 0;
}

pub fn setThreadReady(thread_index: usize, ready: bool) bool {
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    if (ready) schedulerFullMemoryFence();
    ctx.ready = ready;
    setThreadHotReady(thread_index, ready);
    return true;
}

fn initThreadContextWithSpacesReady(
    thread_index: usize,
    owner_process: kernel.PrincipalId,
    user_spaces: []UserAddressSpace,
    initial_frame: TrapFrame,
    initial_ready: bool,
) bool {
    const space = getUserSpace(user_spaces, owner_process) orelse return false;
    const ctx = getThreadContext(thread_index) orelse return false;
    ctx.id = @intCast(thread_index);
    ctx.allocated = true;
    ctx.owner_process = owner_process;
    ctx.cpu_slot = bootstrap_cpu_slot;
    ctx.cpu_affinity_mask = all_cpu_affinity_mask;
    ctx.cr3 = x86_platform.cr3WithUserPcid(space.cr3, pcidForPrincipal(owner_process));
    ctx.fs_base = 0;
    ctx.gs_base = 0;
    ctx.pkru = 0;
    ctx.ready = initial_ready;
    ctx.wait_mailbox = false;
    ctx.signal_pending = false;
    ctx.wake_tick = 0;
    ctx.frame = initial_frame;
    ctx.fx_state = initial_fx_state;
    syncHotStateFromContext(thread_index);
    return true;
}

pub fn initThreadContextWithSpaces(
    thread_index: usize,
    owner_process: kernel.PrincipalId,
    user_spaces: []UserAddressSpace,
    initial_frame: TrapFrame,
) bool {
    return initThreadContextWithSpacesReady(thread_index, owner_process, user_spaces, initial_frame, true);
}

pub fn threadSlotForPrincipal(principal: kernel.PrincipalId) ?usize {
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const ctx = getThreadContextConst(i) orelse continue;
        if (ctx.allocated and ctx.owner_process == principal) return i;
    }
    return null;
}

pub fn threadGeneration(thread_index: usize) ?u32 {
    const ctx = getThreadContextConst(thread_index) orelse return null;
    if (!ctx.allocated) return null;
    return ctx.generation;
}

pub fn threadBelongsToPrincipal(thread_index: usize, principal: kernel.PrincipalId) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    return ctx.allocated and ctx.owner_process == principal;
}

pub fn liveThreadCountForPrincipal(principal: kernel.PrincipalId) usize {
    var count: usize = 0;
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const ctx = getThreadContextConst(i) orelse continue;
        if (ctx.allocated and ctx.owner_process == principal) count += 1;
    }
    return count;
}

pub fn nextReadyThreadForPrincipalAfter(principal: kernel.PrincipalId, current_index: usize) ?usize {
    if (thread_contexts.len == 0) return null;
    var offset: usize = 1;
    while (offset <= thread_contexts.len) : (offset += 1) {
        const index = (current_index + offset) % thread_contexts.len;
        const hot = getThreadHotStateConst(index) orelse continue;
        if (hot.allocated == 0 or hot.ready == 0 or hot.owner_process != principal) continue;
        return index;
    }
    return null;
}

pub fn allocateThreadSlot(owner_process: kernel.PrincipalId, user_spaces: []UserAddressSpace, initial_frame: TrapFrame, free_list: *kernel.FreePageList) ?usize {
    return allocateThreadSlotReady(owner_process, user_spaces, initial_frame, true, free_list);
}

pub fn allocateSuspendedThreadSlot(owner_process: kernel.PrincipalId, user_spaces: []UserAddressSpace, initial_frame: TrapFrame, free_list: *kernel.FreePageList) ?usize {
    return allocateThreadSlotReady(owner_process, user_spaces, initial_frame, false, free_list);
}

fn allocateThreadSlotReady(owner_process: kernel.PrincipalId, user_spaces: []UserAddressSpace, initial_frame: TrapFrame, initial_ready: bool, free_list: *kernel.FreePageList) ?usize {
    while (true) {
        var i: usize = 0;
        while (i < thread_contexts.len) : (i += 1) {
            const ctx = getThreadContextConst(i) orelse continue;
            if (ctx.allocated) continue;
            if (!initThreadContextWithSpacesReady(i, owner_process, user_spaces, initial_frame, initial_ready)) return null;
            return i;
        }
        if (!ensureThreadCapacity(thread_contexts.len + 1, free_list)) return null;
    }
}

pub fn releaseThreadSlot(thread_index: usize) bool {
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    const next_generation = nextThreadGeneration(ctx.generation);
    const release_cpu_slot = ctx.cpu_slot;
    const releasing_from_cpu = currentCpuSlot();
    lockAllCpuSchedulerStates();
    const wake_mask = clearThreadFromCpuSchedulerStatesLocked(thread_index);
    unlockAllCpuSchedulerStates();
    ctx.* = .{ .id = @intCast(thread_index), .generation = next_generation };
    syncHotStateFromContext(thread_index);
    syncHotStateFromContext(thread_index);
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
        const entered_on_user_ap = cpu_slot != bootstrap_cpu_slot and
            state.entered_handoff_thread == thread_index and
            smp.cpuState(cpu_slot) == .user;
        const running_on_ap = cpu_slot != bootstrap_cpu_slot and
            (state.current_thread == thread_index or entered_on_user_ap);
        state.run_queue.markBlocked(thread_index);
        if (associated_with_cpu) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
                        if (running_on_ap) {
                if (cpuAffinityBit(cpu_slot)) |bit| wake_mask |= bit;
            }
        }
        if (state.pending_handoff_thread == thread_index) state.pending_handoff_thread = null;
        if (state.validated_handoff_thread == thread_index) state.validated_handoff_thread = null;
        if (state.consumed_handoff_thread == thread_index) state.consumed_handoff_thread = null;
        if (state.entered_handoff_thread == thread_index) state.entered_handoff_thread = null;
        state.user_entry_requested = state.consumed_handoff_thread != null;
    }
    return wake_mask;
}

fn stopReleasedThreadOnAp(release_cpu_slot: usize, releasing_from_cpu: usize, wake_mask: u64) void {
    if (!userApSchedulingReady()) return;
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
    }
    cpu_slot = 1;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        const bit = cpuAffinityBit(cpu_slot) orelse {
            continue;
        };
        if ((wake_mask & bit) == 0) continue;
        if (cpu_slot == releasing_from_cpu) continue;
        waitForReleasedApToPark(cpu_slot);
    }
}

fn waitForReleasedApToPark(cpu_slot: usize) void {
    var spins: usize = 0;
    while (spins < 20_000_000) : (spins += 1) {
        if (smp.cpuState(cpu_slot) != .user) return;
        asm volatile ("pause");
    }
}

pub fn activateThread(thread_index: usize) bool {
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    if (hot.allocated == 0) return false;
    if (hot.ready == 0) return false;
    const cpu_slot = currentCpuSlot();
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.allocated or ctx.cpu_slot != cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    if (!state.enabled) return false;
    state.current_thread = thread_index;
    state.current_principal = hot.owner_process;
    state.current_cr3 = hot.cr3;
    state.is_idle = false;
    if (cpu_slot != bootstrap_cpu_slot and userApSchedulingReady()) {
        state.run_queue.markBlocked(thread_index);
        state.pending_handoff_thread = null;
        state.validated_handoff_thread = null;
        state.consumed_handoff_thread = thread_index;
        state.entered_handoff_thread = thread_index;
        state.user_entry_requested = false;
    }
    if (!applyThreadFsBase(thread_index)) return false;
    return true;
}

pub fn applyThreadFsBase(thread_index: usize) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    x86_platform.writeFsBase(ctx.fs_base);
    x86_platform.writeGsBase(ctx.gs_base);
    x86_platform.writePkru(ctx.pkru);
    return true;
}

pub fn setThreadFsBase(thread_index: usize, fs_base: u64) bool {
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    ctx.fs_base = fs_base;
    if (thread_index == currentThreadIndex()) x86_platform.writeFsBase(fs_base);
    return true;
}

pub fn setCurrentThreadGsBase(gs_base: u64) bool {
    const ctx = getThreadContext(currentThreadIndex()) orelse return false;
    if (!ctx.allocated) return false;
    ctx.gs_base = gs_base;
    x86_platform.writeGsBase(gs_base);
    return true;
}

fn saveCurrentThreadContextFromFrame(frame: *const TrapFrame) void {
    const current_thread = currentThreadIndex();
    const ctx = getThreadContext(current_thread) orelse return;
    if (!ctx.allocated) return;
    ctx.frame = frame.*;
    ctx.cr3 = currentUserCr3();
    ctx.pkru = x86_platform.readPkru();
    setThreadHotCr3(current_thread, ctx.cr3);
    const hot = getThreadHotStateConst(current_thread) orelse return;
    if (hot.wait_mailbox == 0 and hot.wake_tick == 0) {
        ctx.ready = true;
        setThreadHotReady(current_thread, true);
    }
}

pub fn loadThreadContextToFrame(thread_index: usize, frame: *TrapFrame) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    if (hot.allocated == 0) return false;
    if (hot.ready == 0) return false;
    schedulerFullMemoryFence();
    x86_platform.writeFsBase(ctx.fs_base);
    x86_platform.writeGsBase(ctx.gs_base);
    x86_platform.writePkru(ctx.pkru);
    frame.* = ctx.frame;
    return true;
}

pub fn switchToThread(next_thread: usize, frame: *TrapFrame, saved_rax: ?u64) bool {
    if (!schedulerRunsOnCurrentCpu() and !userApSchedulingReady()) return false;
    if (next_thread >= thread_contexts.len) return false;
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
    if (current_index >= thread_contexts.len) return 0;
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    rebuildRunnableQueuesFromHotThreadsLocked();
    return primarySchedulerState().run_queue.pickNextAfter(current_index);
}

pub fn wakeThreadIfWaiting(thread_index: usize) void {
    const ctx = getThreadContext(thread_index) orelse return;
    if (!ctx.allocated) return;
    prepareBlockedThreadForWake(thread_index);
    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    wakeAssignedApForRunnableThread(thread_index);
}

pub fn wakeWaitingThreadForPrincipal(principal: kernel.PrincipalId) void {
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0 or hot.owner_process != principal) continue;
        if (hot.wait_mailbox == 0) continue;
        wakeThreadIfWaiting(i);
        return;
    }
}

pub fn wakeBlockedThreadForPrincipal(principal: kernel.PrincipalId) void {
    var first_ready: ?usize = null;
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0 or hot.owner_process != principal) continue;
        if (hot.ready == 0) {
            wakeThreadIfWaiting(i);
            return;
        }
        if (first_ready == null) first_ready = i;
    }
    if (first_ready) |thread_index| {
        const ctx = getThreadContext(thread_index) orelse return;
        ctx.signal_pending = true;
        setThreadHotSignalPending(thread_index, true);
    }
}

pub fn consumePendingSignalForPrincipal(principal: kernel.PrincipalId) bool {
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0 or hot.owner_process != principal or hot.signal_pending == 0) continue;
        const ctx = getThreadContext(i) orelse return false;
        ctx.signal_pending = false;
        setThreadHotSignalPending(i, false);
        return true;
    }
    return false;
}

pub fn releaseThreadsForPrincipal(principal: kernel.PrincipalId) usize {
    var released: usize = 0;
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const ctx = getThreadContextConst(i) orelse continue;
        if (!ctx.allocated or ctx.owner_process != principal) continue;
        if (releaseThreadSlot(i)) released += 1;
    }
    return released;
}

pub fn wakeThreadsForTimer(now_tick: u64) void {
    if (!schedulerRunsOnCurrentCpu()) return;
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0) continue;
        if (hot.ready != 0) continue;
        if (hot.wake_tick == 0 or now_tick < hot.wake_tick) continue;
        wakeThreadIfWaiting(i);
    }
}

pub fn blockCurrentThreadForEvent(frame: *TrapFrame, wait_mailbox: bool, timeout_ticks: u64, resume_rax: u64) bool {
    if (!schedulerRunsOnCurrentCpu() and !userApSchedulingReady()) return false;
    const current_thread = currentThreadIndex();
    const ctx = getThreadContext(current_thread) orelse return false;

    var saved = frame.*;
    saved.rax = resume_rax;
    ctx.frame = saved;
    ctx.cr3 = currentUserCr3();
    ctx.pkru = x86_platform.readPkru();
    ctx.wait_mailbox = wait_mailbox;
    ctx.wake_tick = if (timeout_ticks == 0) 0 else lapic_tick_count + timeout_ticks;
    ctx.ready = false;
    setThreadHotCr3(current_thread, ctx.cr3);
    setThreadHotWaitState(current_thread, wait_mailbox, ctx.wake_tick, false);
    clearCurrentApThreadAssociationForBlock(currentCpuSlot(), current_thread);

    const next_thread = blk: {
        const cpu_slot = currentCpuSlot();
        if (cpu_slot == bootstrap_cpu_slot) {
            break :blk pickNextReadyThreadIndex(current_thread);
        }
        break :blk pickNextReadyThreadIndexForCpu(cpu_slot, current_thread) orelse current_thread;
    };
    if (next_thread == current_thread) {
        const cpu_slot = currentCpuSlot();
        if (cpu_slot != bootstrap_cpu_slot and userApSchedulingReady()) {
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
        setThreadHotWaitState(current_thread, false, 0, true);
        return false;
    }
    if (!activateThread(next_thread)) {
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.ready = true;
        setThreadHotWaitState(current_thread, false, 0, true);
        return false;
    }
    if (!loadThreadContextToFrame(next_thread, frame)) {
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.ready = true;
        setThreadHotWaitState(current_thread, false, 0, true);
        _ = activateThread(current_thread);
        _ = loadThreadContextToFrame(current_thread, frame);
        return false;
    }
    return true;
}
