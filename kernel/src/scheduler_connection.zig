const std = @import("std");
const builtin = @import("builtin");
const kernel = @import("kernel.zig");
const address_space = @import("memory/address_space.zig");
const interrupts = @import("interrupts.zig");
const smp = @import("smp.zig");
const scheduler_observer = @import("scheduler_observer.zig");
const x86_platform = @import("arch/x86_64/platform.zig");
const kernel_log = @import("kernel_log.zig");
const log_util = @import("log_util.zig");
pub const verified_sched = @import("verified_sched.zig");
const scheduler_runqueue = @import("scheduler_runqueue.zig");

const TrapFrame = interrupts.TrapFrame;

pub const BeforeCurrentThreadLeaveCallback = struct {
    context: *anyopaque,
    run: *const fn (*anyopaque) void,
};
const UserAddressSpace = address_space.UserAddressSpace;
const default_process_principal: kernel.PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
const bootstrap_cpu_slot: usize = 0;
const all_cpu_affinity_mask: u64 = if (smp.max_cpus >= 64) std.math.maxInt(u64) else (@as(u64, 1) << smp.max_cpus) - 1;

const fx_state_bytes: usize = 512;
pub const maxThreadSlots: usize = kernel.max_thread_slots;
pub const initialThreadCapacity: usize = kernel.initial_thread_capacity;
pub const idleThreadMarker: usize = maxThreadSlots;
const runtime_ns_per_tick: u64 = 1_000_000;

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

const CpuSchedulerState = struct {
    lock: SchedulerSpinLock = .{},
    runqueue_lock: SchedulerSpinLock = .{},
    runqueue: scheduler_runqueue.Tree = .{},
    current_entity: ?*scheduler_runqueue.Node = null,
    virtual_time: i64 = 0,
    current_thread: usize = idleThreadMarker,
    current_principal: ?kernel.PrincipalId = null,
    current_cr3: u64 = 0,
    idle_thread: usize = idleThreadMarker,
    tick_accum: u64 = 0,
    deferred_slice_ticks: u64 = 0,
    enabled: bool = false,
    is_idle: bool = true,
};

pub const ThreadContext = struct {
    id: u32 = 0,
    generation: u32 = 1,
    allocated: bool = false,
    scheduler_entity: ?*scheduler_runqueue.Node = null,
    owner_process: kernel.PrincipalId = default_process_principal,
    cpu_slot: usize = bootstrap_cpu_slot,
    cpu_affinity_mask: u64 = all_cpu_affinity_mask,
    cr3: u64 = 0,
    fs_base: u64 = 0,
    gs_base: u64 = 0,
    pkru: u32 = 0,
    ready: bool = false,
    wait_mailbox: bool = false,
    stopped: bool = false,
    resume_after_stop: bool = false,
    pending_signal_mask: u64 = 0,
    signal_interrupt_consumed: bool = false,
    signal_entry: u64 = 0,
    signal_blocked_mask: u64 = 0,
    signal_inhibit_start: u64 = 0,
    signal_inhibit_end: u64 = 0,
    signal_inhibit_secondary_start: u64 = 0,
    signal_inhibit_secondary_end: u64 = 0,
    signal_owner: bool = false,
    wake_tick: u64 = 0,
    frame: TrapFrame = std.mem.zeroes(TrapFrame),
    fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes,
};

pub const SuspendedThreadImage = struct {
    frame: TrapFrame,
    fs_base: u64,
    gs_base: u64,
    pkru: u32,
};

const ThreadHotState = extern struct {
    allocated: u8 = 0,
    ready: u8 = 0,
    stopped: u8 = 0,
    wait_mailbox: u8 = 0,
    owner_process: kernel.PrincipalId = default_process_principal,
    pending_signal_mask: u64 = 0,
    wake_tick: u64 = 0,
    cr3: u64 = 0,
};

fn buildInitialThreadContexts() [initialThreadCapacity]ThreadContext {
    var contexts: [initialThreadCapacity]ThreadContext = undefined;
    inline for (0..initialThreadCapacity) |i| {
        contexts[i] = .{ .id = @intCast(i) };
    }
    return contexts;
}

fn buildInitialThreadHotStates() [initialThreadCapacity]ThreadHotState {
    var hot: [initialThreadCapacity]ThreadHotState = undefined;
    inline for (0..initialThreadCapacity) |i| {
        hot[i] = .{};
    }
    return hot;
}

var initial_thread_contexts: [initialThreadCapacity]ThreadContext = buildInitialThreadContexts();
var initial_scheduler_entities: [initialThreadCapacity]scheduler_runqueue.Node = undefined;
pub export var thread_contexts_ptr: *anyopaque = @ptrCast(initial_thread_contexts[0..].ptr);
var initial_thread_hot_states: [initialThreadCapacity]ThreadHotState = buildInitialThreadHotStates();
pub export var lapic_tick_count: u64 = 0;
pub var scheduler_tick_accum: u64 = 0;
pub var scheduler_switch_count: u64 = 0;
pub var scheduler_timer_log_once: u8 = 0;
pub var initial_fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes;
pub var kernel_interrupt_fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes;
var principal_lifecycle_gate: u8 = 0;
var principal_lifecycle_target_raw: usize = std.math.maxInt(usize);
var principal_lifecycle_pending_action: u8 = 0;
var principal_lifecycle_pending_code: u32 = 0;

pub const PrincipalLifecycleAction = enum(u8) {
    none = 0,
    stop = 1,
    continue_process = 2,
    kill = 3,
};

pub const PendingPrincipalLifecycleAction = struct {
    action: PrincipalLifecycleAction,
    code: u32,
};

const ThreadTableState = struct {
    lock_state: SchedulerSpinLock = .{},
    contexts: []ThreadContext = initial_thread_contexts[0..],
    hot_states: []ThreadHotState = initial_thread_hot_states[0..],
    next_cpu_cursor: usize = bootstrap_cpu_slot,

    fn lock(self: *ThreadTableState) void {
        self.lock_state.lock();
    }

    fn unlock(self: *ThreadTableState) void {
        self.lock_state.unlock();
    }
};

const VerifiedCoreState = struct {
    initialized: bool = false,
    faulted: bool = false,
    log_count: u32 = 0,
};

const SchedulerState = struct {
    thread_table: ThreadTableState = .{},
    verified: VerifiedCoreState = .{},
    cpus: [smp.max_cpus]CpuSchedulerState = buildInitialCpuSchedulerStates(),
};

var scheduler_state: SchedulerState = .{};

fn verifiedCoreCpuCount() usize {
    return smp.max_cpus;
}

fn verifiedThreadIdForGeneration(thread_index: usize, generation: u32) ?i64 {
    if (thread_index >= maxThreadSlots) return null;
    const raw_id =
        (@as(u128, generation) * @as(u128, maxThreadSlots)) +
        @as(u128, thread_index) +
        1;
    if (raw_id > @as(u128, @intCast(std.math.maxInt(i64)))) return null;
    return @intCast(raw_id);
}

fn verifiedThreadCpu(thread_index: usize) usize {
    const cpu_id = threadAssignedCpuSlot(thread_index);
    return if (cpu_id < verifiedCoreCpuCount()) cpu_id else bootstrap_cpu_slot;
}

fn verifiedCoreReady() bool {
    return scheduler_state.verified.initialized and !scheduler_state.verified.faulted;
}

fn noteVerifiedCoreIssue(comptime op: []const u8, rc: verified_sched.SchedRc, thread_index: ?usize) void {
    const old = @atomicRmw(u32, &scheduler_state.verified.log_count, .Add, 1, .acq_rel);
    if (old >= 16) return;
    if (thread_index) |thread| {
        kernel_log.writeFmt("[sched-core] {s} rc={s} thread={}\n", .{ op, @tagName(rc), thread });
    } else {
        kernel_log.writeFmt("[sched-core] {s} rc={s}\n", .{ op, @tagName(rc) });
    }
}

fn schedulerEntity(thread_index: usize) ?*scheduler_runqueue.Node {
    const ctx = threadContext(thread_index) orelse return null;
    return ctx.scheduler_entity;
}

fn cpuRunqueueFloor(state: *const CpuSchedulerState) i64 {
    return state.runqueue.minimumVruntime() orelse state.virtual_time;
}

fn refreshCpuVirtualTime(state: *CpuSchedulerState) void {
    if (state.runqueue.minimumVruntime()) |minimum| {
        if (minimum > state.virtual_time) state.virtual_time = minimum;
    }
}

fn scalarIssue(comptime op: []const u8, rc: verified_sched.EevdfRc, thread_index: usize) bool {
    if (rc == .ok) return false;
    noteVerifiedCoreIssue(op, rc, thread_index);
    return true;
}

fn nodeMatches(node: *const scheduler_runqueue.Node, thread_index: usize, generation: u32, cpu_id: usize) bool {
    return node.thread_index == thread_index and
        node.generation == generation and
        node.entity.generation == @as(i64, @intCast(generation)) and
        node.cpu_slot == cpu_id;
}

fn threadAllowsCpu(ctx: *const ThreadContext, cpu_id: usize) bool {
    return cpu_id < 64 and (ctx.cpu_affinity_mask & (@as(u64, 1) << @intCast(cpu_id))) != 0;
}

fn schedulerCpuEnabled(cpu_id: usize) bool {
    const state = schedulerStateForSlot(cpu_id) orelse return false;
    state.lock.lock();
    const enabled = state.enabled;
    state.lock.unlock();
    return enabled;
}

fn verifiedAddThread(thread_index: usize, generation: u32, ready: bool) bool {
    if (!verifiedCoreReady()) return false;
    const thread_id = verifiedThreadIdForGeneration(thread_index, generation) orelse return false;
    const cpu_id = verifiedThreadCpu(thread_index);
    const state = schedulerStateForSlot(cpu_id) orelse return false;
    const node = schedulerEntity(thread_index) orelse return false;
    state.runqueue_lock.lock();
    node.reset(thread_index, generation);
    node.cpu_slot = cpu_id;
    var entity: verified_sched.Entity = undefined;
    const init_rc = verified_sched.pacha_eevdf_entity_init(
        thread_id,
        @intCast(generation),
        1024,
        4_000_000,
        cpuRunqueueFloor(state),
        &entity,
    );
    if (scalarIssue("add", init_rc, thread_index)) {
        state.runqueue_lock.unlock();
        return false;
    }
    node.entity = entity;
    if (ready) {
        node.ownership = .runnable;
        if (!state.runqueue.insert(node)) {
            state.runqueue_lock.unlock();
            noteVerifiedCoreIssue("add-tree", .state, thread_index);
            return false;
        }
    } else {
        var blocked: verified_sched.Entity = undefined;
        const rc = verified_sched.pacha_eevdf_entity_block(&node.entity, &blocked);
        if (scalarIssue("add-block", rc, thread_index)) {
            state.runqueue_lock.unlock();
            return false;
        }
        node.entity = blocked;
        node.ownership = .blocked;
    }
    state.runqueue_lock.unlock();
    if (ready and cpu_id != currentCpu()) _ = smp.wakeCpu(cpu_id);
    return true;
}

fn verifiedWakeThreadGeneration(thread_index: usize, generation: u32) bool {
    if (!verifiedCoreReady()) return false;
    const ctx = threadContextMutable(thread_index) orelse return false;
    const node = ctx.scheduler_entity orelse return false;
    const last_cpu = node.cpu_slot;
    const last_state = schedulerStateForSlot(last_cpu) orelse return false;

    var target_cpu = last_cpu;
    var target_count: usize = std.math.maxInt(usize);
    var last_count: usize = std.math.maxInt(usize);
    var cpu_id: usize = 0;
    while (cpu_id < verifiedCoreCpuCount()) : (cpu_id += 1) {
        const state = schedulerStateForSlot(cpu_id) orelse continue;
        if (!schedulerCpuEnabled(cpu_id) or !threadAllowsCpu(ctx, cpu_id)) continue;
        state.runqueue_lock.lock();
        const count = state.runqueue.count;
        state.runqueue_lock.unlock();
        if (cpu_id == last_cpu) last_count = count;
        if (count < target_count) {
            target_count = count;
            target_cpu = cpu_id;
        }
    }
    if (target_count == std.math.maxInt(usize)) return false;
    const last_allowed = schedulerCpuEnabled(last_cpu) and threadAllowsCpu(ctx, last_cpu);
    if (last_allowed and last_count <= target_count +| 1) target_cpu = last_cpu;

    const target_state = schedulerStateForSlot(target_cpu) orelse return false;
    const first = if (last_cpu <= target_cpu) last_state else target_state;
    const second = if (last_cpu <= target_cpu) target_state else last_state;
    const woke = wake_locked: {
        first.lock.lock();
        if (second != first) second.lock.lock();
        defer {
            if (second != first) second.lock.unlock();
            first.lock.unlock();
        }
        first.runqueue_lock.lock();
        if (second != first) second.runqueue_lock.lock();
        defer {
            if (second != first) second.runqueue_lock.unlock();
            first.runqueue_lock.unlock();
        }
        if (!nodeMatches(node, thread_index, generation, last_cpu) or
            node.ownership != .blocked or ctx.generation != generation or ctx.cpu_slot != last_cpu)
        {
            break :wake_locked false;
        }
        if (!target_state.enabled) {
            if (!last_state.enabled or !threadAllowsCpu(ctx, last_cpu)) break :wake_locked false;
            target_cpu = last_cpu;
        }
        // Scalar ownership becomes blocked before the old CPU completes its
        // kernel-to-idle transition.  Do not publish this generation on a
        // second CPU while the old CPU still executes its block tail.
        if (!last_state.is_idle and last_state.current_thread == thread_index) {
            target_cpu = last_cpu;
        }
        // Revalidate the locality threshold under the pair of runqueue locks.
        // A last CPU that is still within one queued entity always wins.
        if (target_cpu != last_cpu and last_allowed and
            last_state.runqueue.count <= target_state.runqueue.count +| 1)
        {
            target_cpu = last_cpu;
        }
        const destination = schedulerStateForSlot(target_cpu) orelse break :wake_locked false;
        var runnable: verified_sched.Entity = undefined;
        const rc = verified_sched.pacha_eevdf_entity_wake(&node.entity, cpuRunqueueFloor(destination), &runnable);
        if (scalarIssue("wake", rc, thread_index)) break :wake_locked false;
        const blocked = node.entity;
        node.entity = runnable;
        node.cpu_slot = target_cpu;
        node.ownership = .runnable;
        if (!destination.runqueue.insert(node)) {
            node.entity = blocked;
            node.cpu_slot = last_cpu;
            node.ownership = .blocked;
            noteVerifiedCoreIssue("wake-tree", .state, thread_index);
            break :wake_locked false;
        }
        ctx.cpu_slot = target_cpu;
        break :wake_locked true;
    };
    if (woke and target_cpu != currentCpu()) _ = smp.wakeCpu(target_cpu);
    return woke;
}

fn verifiedWakeThread(thread_index: usize) void {
    const ctx = threadContext(thread_index) orelse return;
    if (!ctx.allocated) return;
    _ = verifiedWakeThreadGeneration(thread_index, ctx.generation);
}

fn verifiedBlockThreadGeneration(thread_index: usize, generation: u32) bool {
    if (!verifiedCoreReady()) return false;
    const node = schedulerEntity(thread_index) orelse return false;
    const cpu_id = node.cpu_slot;
    const state = schedulerStateForSlot(cpu_id) orelse return false;
    state.runqueue_lock.lock();
    if (!nodeMatches(node, thread_index, generation, cpu_id)) {
        state.runqueue_lock.unlock();
        return false;
    }
    switch (node.ownership) {
        .runnable => if (!state.runqueue.remove(node)) {
            state.runqueue_lock.unlock();
            noteVerifiedCoreIssue("block-tree", .state, thread_index);
            return false;
        },
        .running => {
            if (state.current_entity != node) {
                state.runqueue_lock.unlock();
                noteVerifiedCoreIssue("block-owner", .state, thread_index);
                return false;
            }
            state.current_entity = null;
        },
        .blocked => {
            state.runqueue_lock.unlock();
            return true;
        },
        else => {
            state.runqueue_lock.unlock();
            return false;
        },
    }
    var blocked: verified_sched.Entity = undefined;
    const rc = verified_sched.pacha_eevdf_entity_block(&node.entity, &blocked);
    if (scalarIssue("block", rc, thread_index)) {
        state.runqueue_lock.unlock();
        return false;
    }
    node.entity = blocked;
    node.ownership = .blocked;
    refreshCpuVirtualTime(state);
    state.runqueue_lock.unlock();
    return true;
}

fn verifiedBlockThread(thread_index: usize) void {
    const ctx = threadContext(thread_index) orelse return;
    if (!ctx.allocated) return;
    _ = verifiedBlockThreadGeneration(thread_index, ctx.generation);
}

fn verifiedExitThreadGeneration(thread_index: usize, generation: u32) bool {
    if (!verifiedCoreReady()) return false;
    const node = schedulerEntity(thread_index) orelse return false;
    const cpu_id = node.cpu_slot;
    const state = schedulerStateForSlot(cpu_id) orelse return false;
    state.runqueue_lock.lock();
    if (!nodeMatches(node, thread_index, generation, cpu_id)) {
        state.runqueue_lock.unlock();
        return false;
    }
    switch (node.ownership) {
        .runnable => if (!state.runqueue.remove(node)) {
            state.runqueue_lock.unlock();
            return false;
        },
        .running => {
            if (state.current_entity != node) {
                state.runqueue_lock.unlock();
                return false;
            }
            state.current_entity = null;
        },
        .blocked => {},
        .dead => {
            state.runqueue_lock.unlock();
            return true;
        },
        .free => {
            state.runqueue_lock.unlock();
            return false;
        },
    }
    var exited: verified_sched.Entity = undefined;
    const rc = verified_sched.pacha_eevdf_entity_exit(&node.entity, &exited);
    if (scalarIssue("exit", rc, thread_index)) {
        state.runqueue_lock.unlock();
        return false;
    }
    node.entity = exited;
    node.ownership = .dead;
    refreshCpuVirtualTime(state);
    state.runqueue_lock.unlock();
    return true;
}

fn verifiedFinishCpuGeneration(cpu_id: usize, thread_index: usize, generation: u32) void {
    if (!verifiedCoreReady()) return;
    const state = schedulerStateForSlot(cpu_id) orelse return;
    state.runqueue_lock.lock();
    const node = state.current_entity orelse {
        state.runqueue_lock.unlock();
        return;
    };
    if (!nodeMatches(node, thread_index, generation, cpu_id) or node.ownership != .running) {
        state.runqueue_lock.unlock();
        return;
    }
    var runnable: verified_sched.Entity = undefined;
    const rc = verified_sched.pacha_eevdf_entity_finish(&node.entity, &runnable);
    if (scalarIssue("finish", rc, node.thread_index)) {
        state.runqueue_lock.unlock();
        return;
    }
    state.current_entity = null;
    node.entity = runnable;
    node.ownership = .runnable;
    if (!state.runqueue.insert(node)) noteVerifiedCoreIssue("finish-tree", .state, node.thread_index);
    state.runqueue_lock.unlock();
}

/// Charge one expired slice and request preemption only when another eligible
/// entity is ready to compete.  With no competitor the current entity remains
/// running and never takes the park/reinsert path.
pub const SliceDecision = enum {
    continue_running,
    preempt,
    invalid,
};

fn verifiedExpireSlice(cpu_id: usize, thread_index: usize, generation: u32, runtime_ns: u64) SliceDecision {
    if (!verifiedCoreReady() or runtime_ns > @as(u64, @intCast(std.math.maxInt(i64)))) return .invalid;
    const state = schedulerStateForSlot(cpu_id) orelse return .invalid;
    state.runqueue_lock.lock();
    const node = state.current_entity orelse {
        state.runqueue_lock.unlock();
        return .invalid;
    };
    if (!nodeMatches(node, thread_index, generation, cpu_id) or node.ownership != .running) {
        state.runqueue_lock.unlock();
        noteVerifiedCoreIssue("timer-owner", .state, thread_index);
        return .invalid;
    }
    var charged: verified_sched.Entity = undefined;
    const charge_rc = verified_sched.pacha_eevdf_entity_charge(
        &node.entity,
        @intCast(runtime_ns),
        cpuRunqueueFloor(state),
        &charged,
    );
    if (scalarIssue("timer", charge_rc, thread_index)) {
        state.runqueue_lock.unlock();
        return .invalid;
    }
    var minimum = charged.vruntime;
    if (state.runqueue.minimumVruntime()) |queued_minimum| minimum = @min(minimum, queued_minimum);
    if (minimum > state.virtual_time) state.virtual_time = minimum;
    node.entity = charged;
    if (state.runqueue.bestEligible(state.virtual_time) == null) {
        state.runqueue_lock.unlock();
        return .continue_running;
    }
    var runnable: verified_sched.Entity = undefined;
    const finish_rc = verified_sched.pacha_eevdf_entity_finish(&charged, &runnable);
    if (scalarIssue("timer-finish", finish_rc, thread_index)) {
        state.runqueue_lock.unlock();
        return .invalid;
    }
    state.current_entity = null;
    node.entity = runnable;
    node.ownership = .runnable;
    if (!state.runqueue.insert(node)) {
        node.entity = charged;
        node.ownership = .running;
        state.current_entity = node;
        state.runqueue_lock.unlock();
        noteVerifiedCoreIssue("timer-tree", .state, thread_index);
        return .invalid;
    }
    refreshCpuVirtualTime(state);
    state.runqueue_lock.unlock();
    return .preempt;
}

const PickedThread = struct {
    thread_index: usize,
    generation: u32,
};

fn verifiedPickThreadForCpu(cpu_id: usize) ?PickedThread {
    if (!verifiedCoreReady()) return null;
    const state = schedulerStateForSlot(cpu_id) orelse return null;
    state.runqueue_lock.lock();
    if (state.current_entity != null) {
        state.runqueue_lock.unlock();
        return null;
    }
    var node = state.runqueue.bestEligible(state.virtual_time);
    if (node == null) {
        if (state.runqueue.minimumEligibleTime()) |next_eligible| {
            if (next_eligible > state.virtual_time) state.virtual_time = next_eligible;
            node = state.runqueue.bestEligible(state.virtual_time);
        }
    }
    const selected = node orelse {
        state.runqueue_lock.unlock();
        return null;
    };
    if (!state.runqueue.remove(selected)) {
        state.runqueue_lock.unlock();
        noteVerifiedCoreIssue("pick-tree", .state, selected.thread_index);
        return null;
    }
    var running: verified_sched.Entity = undefined;
    const rc = verified_sched.pacha_eevdf_entity_mark_running(&selected.entity, &running);
    if (scalarIssue("pick", rc, selected.thread_index)) {
        selected.ownership = .runnable;
        _ = state.runqueue.insert(selected);
        state.runqueue_lock.unlock();
        return null;
    }
    selected.entity = running;
    selected.ownership = .running;
    state.current_entity = selected;
    const thread_index = selected.thread_index;
    const generation = selected.generation;
    state.runqueue_lock.unlock();

    const ctx = threadContext(thread_index) orelse {
        verifiedFinishCpuGeneration(cpu_id, thread_index, generation);
        return null;
    };
    if (!ctx.allocated or !ctx.ready or ctx.generation != generation or ctx.cpu_slot != cpu_id) {
        verifiedFinishCpuGeneration(cpu_id, thread_index, generation);
        return null;
    }
    return .{ .thread_index = thread_index, .generation = generation };
}

fn verifiedRollbackPickedCpu(cpu_id: usize, picked: PickedThread) void {
    verifiedFinishCpuGeneration(cpu_id, picked.thread_index, picked.generation);
}

fn verifiedRollbackHandoff(cpu_id: usize, thread_index: usize, generation: u32) void {
    const state = schedulerStateForSlot(cpu_id) orelse return;
    state.runqueue_lock.lock();
    const current = state.current_entity orelse {
        state.runqueue_lock.unlock();
        return;
    };
    const current_thread_index = current.thread_index;
    const current_generation = current.generation;
    state.runqueue_lock.unlock();
    _ = verifiedHandoffToThreadOnCpu(
        cpu_id,
        thread_index,
        generation,
        current_thread_index,
        current_generation,
    );
}

fn verifiedHandoffToThreadOnCpu(
    cpu_id: usize,
    thread_index: usize,
    generation: u32,
    current_thread_index: usize,
    current_generation: u32,
) bool {
    if (!verifiedCoreReady()) return false;
    const state = schedulerStateForSlot(cpu_id) orelse return false;
    const target = schedulerEntity(thread_index) orelse return false;
    state.runqueue_lock.lock();
    if (!nodeMatches(target, thread_index, generation, cpu_id) or target.ownership != .runnable) {
        state.runqueue_lock.unlock();
        return false;
    }
    const current = state.current_entity orelse {
        state.runqueue_lock.unlock();
        return false;
    };
    if (!nodeMatches(current, current_thread_index, current_generation, cpu_id) or current.ownership != .running) {
        state.runqueue_lock.unlock();
        return false;
    }
    var runnable_current: verified_sched.Entity = undefined;
    const finish_rc = verified_sched.pacha_eevdf_entity_finish(&current.entity, &runnable_current);
    if (scalarIssue("handoff-finish", finish_rc, current.thread_index)) {
        state.runqueue_lock.unlock();
        return false;
    }
    if (!state.runqueue.remove(target)) {
        state.runqueue_lock.unlock();
        return false;
    }
    var running: verified_sched.Entity = undefined;
    const run_rc = verified_sched.pacha_eevdf_entity_mark_running(&target.entity, &running);
    if (scalarIssue("handoff", run_rc, thread_index)) {
        target.ownership = .runnable;
        _ = state.runqueue.insert(target);
        state.runqueue_lock.unlock();
        return false;
    }
    const old_current_entity = current.entity;
    current.entity = runnable_current;
    current.ownership = .runnable;
    if (!state.runqueue.insert(current)) {
        current.entity = old_current_entity;
        current.ownership = .running;
        target.ownership = .runnable;
        _ = state.runqueue.insert(target);
        state.runqueue_lock.unlock();
        noteVerifiedCoreIssue("handoff-current-tree", .state, current.thread_index);
        return false;
    }
    target.entity = running;
    target.ownership = .running;
    state.current_entity = target;
    state.runqueue_lock.unlock();
    return true;
}

/// Move a runnable entity without allocating.  The thread-table lock freezes
/// generation and ownership publication; runqueue locks are always acquired in
/// CPU-number order so concurrent migrations cannot invert the lock graph.
pub fn migrateRunnableThreadGeneration(thread_index: usize, generation: u32, dst_cpu: usize) bool {
    if (!verifiedCoreReady() or dst_cpu >= verifiedCoreCpuCount()) return false;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return false;
    const node = ctx.scheduler_entity orelse return false;
    const src_cpu = node.cpu_slot;
    if (!ctx.allocated or ctx.generation != generation or ctx.cpu_slot != src_cpu or src_cpu == dst_cpu) return false;
    const src = schedulerStateForSlot(src_cpu) orelse return false;
    const dst = schedulerStateForSlot(dst_cpu) orelse return false;
    if (!dst.enabled) return false;

    const first = if (src_cpu < dst_cpu) src else dst;
    const second = if (src_cpu < dst_cpu) dst else src;
    first.runqueue_lock.lock();
    second.runqueue_lock.lock();
    defer {
        second.runqueue_lock.unlock();
        first.runqueue_lock.unlock();
    }
    if (!nodeMatches(node, thread_index, generation, src_cpu) or node.ownership != .runnable) return false;
    if (!src.runqueue.remove(node)) return false;
    var migrated: verified_sched.Entity = undefined;
    const rc = verified_sched.pacha_eevdf_entity_migrate(&node.entity, cpuRunqueueFloor(dst), &migrated);
    if (scalarIssue("migrate", rc, thread_index)) {
        _ = src.runqueue.insert(node);
        return false;
    }
    const old_entity = node.entity;
    node.entity = migrated;
    node.cpu_slot = dst_cpu;
    if (!dst.runqueue.insert(node)) {
        node.cpu_slot = src_cpu;
        node.entity = old_entity;
        _ = src.runqueue.insert(node);
        noteVerifiedCoreIssue("migrate-tree", .state, thread_index);
        return false;
    }
    ctx.cpu_slot = dst_cpu;
    refreshCpuVirtualTime(src);
    return true;
}

fn fillUserEntryForThread(cpu_id: usize, thread_index: usize, generation: u32, out_entry: *scheduler_observer.UserEntry) bool {
    const ctx = threadContext(thread_index) orelse return false;
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    if (!ctx.allocated or hot.allocated == 0) return false;
    if (ctx.generation != generation) return false;
    if (!ctx.ready or hot.ready == 0) return false;
    if (ctx.cpu_slot != cpu_id) return false;
    out_entry.* = .{
        .cpu_slot = cpu_id,
        .thread_index = thread_index,
        .cr3 = ctx.cr3,
        .fs_base = ctx.fs_base,
        .gs_base = ctx.gs_base,
        .fx_state_addr = @intFromPtr(&ctx.fx_state),
        .pkru = ctx.pkru,
        .frame = ctx.frame,
    };
    return true;
}

/// Pull one already-runnable entity from the busiest runqueue into an empty
/// idle CPU.  The operation is allocation-free and publishes cpu_slot only
/// while the thread table and both runqueues are locked.
fn stealRunnableForIdleCpu(dst_cpu: usize) bool {
    if (!verifiedCoreReady()) return false;
    const dst = schedulerStateForSlot(dst_cpu) orelse return false;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();

    dst.lock.lock();
    const destination_idle = dst.enabled and dst.is_idle;
    dst.lock.unlock();
    if (!destination_idle) return false;
    dst.runqueue_lock.lock();
    const destination_empty = dst.current_entity == null and dst.runqueue.count == 0;
    dst.runqueue_lock.unlock();
    if (!destination_empty) return false;

    const scan_limit: usize = 8;
    var src_cpu: ?usize = null;
    var busiest_count: usize = 0;
    var cpu_id: usize = 0;
    while (cpu_id < verifiedCoreCpuCount()) : (cpu_id += 1) {
        if (cpu_id == dst_cpu) continue;
        const state = schedulerStateForSlot(cpu_id) orelse continue;
        state.lock.lock();
        const enabled = state.enabled;
        const executing_thread = if (state.is_idle) idleThreadMarker else state.current_thread;
        state.lock.unlock();
        if (!enabled) continue;

        state.runqueue_lock.lock();
        const count = state.runqueue.count;
        var candidate = state.runqueue.firstByDeadline();
        var scanned: usize = 0;
        var compatible = false;
        while (candidate != null and scanned < scan_limit) : (scanned += 1) {
            const node = candidate.?;
            const ctx = threadContext(node.thread_index);
            if (node.thread_index != executing_thread and ctx != null and
                ctx.?.allocated and ctx.?.ready and ctx.?.generation == node.generation and
                ctx.?.cpu_slot == cpu_id and threadAllowsCpu(ctx.?, dst_cpu))
            {
                compatible = true;
                break;
            }
            candidate = state.runqueue.nextByDeadline(node);
        }
        state.runqueue_lock.unlock();
        if (compatible and count > busiest_count) {
            busiest_count = count;
            src_cpu = cpu_id;
        }
    }
    const source_cpu = src_cpu orelse return false;
    const src = schedulerStateForSlot(source_cpu) orelse return false;

    const first_state = if (source_cpu < dst_cpu) src else dst;
    const second_state = if (source_cpu < dst_cpu) dst else src;
    first_state.lock.lock();
    second_state.lock.lock();
    defer {
        second_state.lock.unlock();
        first_state.lock.unlock();
    }
    if (!src.enabled or !dst.enabled or !dst.is_idle) return false;
    const executing_thread = if (src.is_idle) idleThreadMarker else src.current_thread;

    const first_rq = if (source_cpu < dst_cpu) src else dst;
    const second_rq = if (source_cpu < dst_cpu) dst else src;
    first_rq.runqueue_lock.lock();
    second_rq.runqueue_lock.lock();
    defer {
        second_rq.runqueue_lock.unlock();
        first_rq.runqueue_lock.unlock();
    }
    if (dst.current_entity != null or dst.runqueue.count != 0 or src.runqueue.count == 0) return false;

    var candidate = src.runqueue.firstByDeadline();
    var scanned: usize = 0;
    while (candidate != null and scanned < scan_limit) : (scanned += 1) {
        const node = candidate.?;
        const ctx = threadContextMutable(node.thread_index);
        if (node.thread_index != executing_thread and
            node.ownership == .runnable and node.cpu_slot == source_cpu and
            ctx != null and ctx.?.allocated and ctx.?.ready and
            ctx.?.generation == node.generation and ctx.?.cpu_slot == source_cpu and
            threadAllowsCpu(ctx.?, dst_cpu))
        {
            if (!src.runqueue.remove(node)) return false;
            var migrated: verified_sched.Entity = undefined;
            const rc = verified_sched.pacha_eevdf_entity_migrate(
                &node.entity,
                cpuRunqueueFloor(dst),
                &migrated,
            );
            if (scalarIssue("steal", rc, node.thread_index)) {
                _ = src.runqueue.insert(node);
                return false;
            }
            const old_entity = node.entity;
            node.entity = migrated;
            node.cpu_slot = dst_cpu;
            if (!dst.runqueue.insert(node)) {
                node.entity = old_entity;
                node.cpu_slot = source_cpu;
                _ = src.runqueue.insert(node);
                noteVerifiedCoreIssue("steal-tree", .state, node.thread_index);
                return false;
            }
            ctx.?.cpu_slot = dst_cpu;
            refreshCpuVirtualTime(src);
            return true;
        }
        candidate = src.runqueue.nextByDeadline(node);
    }
    return false;
}

fn claimKernelScheduledUserEntry(cpu_id: usize, out_entry: *scheduler_observer.UserEntry) bool {
    if (cpu_id == bootstrap_cpu_slot) return false;
    var picked_opt = verifiedPickThreadForCpu(cpu_id);
    if (picked_opt == null and stealRunnableForIdleCpu(cpu_id)) {
        picked_opt = verifiedPickThreadForCpu(cpu_id);
    }
    const picked = picked_opt orelse return false;
    const thread_index = picked.thread_index;
    const state = schedulerStateForSlot(cpu_id) orelse {
        verifiedRollbackPickedCpu(cpu_id, picked);
        return false;
    };
    state.lock.lock();
    if (!state.enabled or !fillUserEntryForThread(cpu_id, thread_index, picked.generation, out_entry)) {
        state.lock.unlock();
        verifiedRollbackPickedCpu(cpu_id, picked);
        return false;
    }
    const ctx = threadContext(thread_index) orelse {
        state.lock.unlock();
        verifiedRollbackPickedCpu(cpu_id, picked);
        return false;
    };
    state.current_thread = thread_index;
    state.current_principal = ctx.owner_process;
    state.current_cr3 = ctx.cr3;
    state.is_idle = false;
    state.lock.unlock();
    return true;
}

pub fn activateNextReadyOnCurrentCpu() bool {
    const cpu_id = currentCpu();
    const picked = verifiedPickThreadForCpu(cpu_id) orelse return false;
    if (activateGeneration(picked.thread_index, picked.generation)) return true;
    verifiedRollbackPickedCpu(cpu_id, picked);
    return false;
}

pub fn loadNextReadyThread(frame: *TrapFrame) bool {
    const cpu_id = currentCpu();
    const picked = verifiedPickThreadForCpu(cpu_id) orelse return false;
    if (!activateGeneration(picked.thread_index, picked.generation)) {
        verifiedRollbackPickedCpu(cpu_id, picked);
        return false;
    }
    if (loadContextIntoFrameGeneration(picked.thread_index, picked.generation, frame)) return true;
    verifiedRollbackPickedCpu(cpu_id, picked);
    return false;
}

/// Complete a transition to idle without treating an empty local runqueue as
/// a global scheduler failure.  Steal is attempted once at the transition;
/// subsequent timer interrupts only retry the local queue, so an idle BSP does
/// not turn its periodic timekeeping tick into a cross-CPU polling loop.
pub fn loadNextReadyThreadOrIdle(frame: *TrapFrame) void {
    if (loadNextReadyThread(frame)) return;
    _ = stealRunnableForIdleCpu(currentCpu());
    while (!loadNextReadyThread(frame)) {
        asm volatile ("sti; hlt; cli" ::: .{ .memory = true });
    }
}

fn switchToNextReadyOnCurrentCpu(frame: *TrapFrame, saved_rax: ?u64) bool {
    const cpu_id = currentCpu();
    const picked = verifiedPickThreadForCpu(cpu_id) orelse return false;
    if (switchToGeneration(picked.thread_index, picked.generation, frame, saved_rax)) return true;
    verifiedRollbackPickedCpu(cpu_id, picked);
    return false;
}

fn nextThreadGeneration(current: u32) u32 {
    const next = current +% 1;
    return if (next == 0) 1 else next;
}

pub fn initializeStaticStorage() void {
    var cpu_slot: usize = 0;
    while (cpu_slot < scheduler_state.cpus.len) : (cpu_slot += 1) {
        scheduler_state.cpus[cpu_slot] = .{};
    }
    scheduler_state.cpus[bootstrap_cpu_slot].enabled = true;
    scheduler_state.cpus[bootstrap_cpu_slot].current_thread = 0;
    scheduler_state.cpus[bootstrap_cpu_slot].is_idle = false;
    var thread_index: usize = 0;
    while (thread_index < initialThreadCapacity) : (thread_index += 1) {
        initial_scheduler_entities[thread_index].reset(thread_index, initial_thread_contexts[thread_index].generation);
        initial_thread_contexts[thread_index].scheduler_entity = &initial_scheduler_entities[thread_index];
    }
    scheduler_state.verified.initialized = true;
    scheduler_state.verified.faulted = false;
    scheduler_state.verified.log_count = 0;
    scheduler_state.thread_table.next_cpu_cursor = bootstrap_cpu_slot;
}

fn observeCpuIdleFromAp(cpu_slot: usize) callconv(.c) void {
    if (cpu_slot == bootstrap_cpu_slot) return;
    const state = schedulerStateForSlot(cpu_slot) orelse return;
    state.lock.lock();
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
    state.deferred_slice_ticks = 0;
    state.lock.unlock();
}

fn claimKernelScheduledUserEntryFromAp(cpu_slot: usize, out_entry: *scheduler_observer.UserEntry) callconv(.c) bool {
    return claimKernelScheduledUserEntry(cpu_slot, out_entry);
}

fn markThreadReadyLocked(thread_index: usize, ready: bool) bool {
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    if (ready and ctx.stopped) return false;
    const was_ready = ctx.ready;
    if (ready) schedulerFullMemoryFence();
    ctx.ready = ready;
    setThreadHotReady(thread_index, ready);
    if (ready and !was_ready) {
        verifiedWakeThread(thread_index);
    } else if (!ready and was_ready) {
        verifiedBlockThread(thread_index);
    }
    return true;
}

fn markThreadReadyInternal(thread_index: usize, ready: bool) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return markThreadReadyLocked(thread_index, ready);
}

pub fn markThreadReady(thread_index: usize, ready: bool) bool {
    return markThreadReadyInternal(thread_index, ready);
}

pub fn preemptApUserThread(slice_ticks: u64, frame: *const TrapFrame) SliceDecision {
    if (slice_ticks == 0 or slice_ticks > std.math.maxInt(u64) / runtime_ns_per_tick) return .invalid;
    const cpu_slot = currentCpu();
    if (cpu_slot == bootstrap_cpu_slot) return .invalid;
    const state = schedulerStateForSlot(cpu_slot) orelse return .invalid;
    const thread_index = currentThread();
    if (thread_index >= scheduler_state.thread_table.contexts.len) return .invalid;
    scheduler_state.thread_table.lock();
    const ctx = threadContextMutable(thread_index) orelse {
        scheduler_state.thread_table.unlock();
        return .invalid;
    };
    if (!ctx.allocated or ctx.cpu_slot != cpu_slot) {
        scheduler_state.thread_table.unlock();
        return .invalid;
    }
    const generation = ctx.generation;
    state.lock.lock();
    const deferred_ticks = state.deferred_slice_ticks;
    state.deferred_slice_ticks = 0;
    state.lock.unlock();
    const charged_ticks = slice_ticks +| deferred_ticks;
    const decision = verifiedExpireSlice(cpu_slot, thread_index, generation, charged_ticks *| runtime_ns_per_tick);
    if (decision != .preempt) {
        scheduler_state.thread_table.unlock();
        return decision;
    }
    state.lock.lock();
    ctx.frame = frame.*;
    ctx.cr3 = currentCr3();
    ctx.fs_base = x86_platform.readFsBase();
    ctx.gs_base = x86_platform.readGsBase();
    ctx.pkru = x86_platform.readPkru();
    ctx.ready = true;
    setThreadHotCr3(thread_index, ctx.cr3);
    setThreadHotReady(thread_index, true);
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
    state.lock.unlock();
    scheduler_state.thread_table.unlock();
    return .preempt;
}

pub fn deferApUserSlice(slice_ticks: u64) void {
    const cpu_slot = currentCpu();
    if (cpu_slot == bootstrap_cpu_slot or slice_ticks == 0) return;
    const state = schedulerStateForSlot(cpu_slot) orelse return;
    state.lock.lock();
    if (!state.is_idle) state.deferred_slice_ticks +|= slice_ticks;
    state.lock.unlock();
}

pub fn parkApThreadForBlock(
    frame: *TrapFrame,
    wait_mailbox: bool,
    timeout_ticks: u64,
    resume_rax: u64,
    before_block: ?BeforeCurrentThreadLeaveCallback,
) bool {
    const current_thread = currentThread();
    scheduler_state.thread_table.lock();
    if (threadContextMutable(current_thread)) |ctx| {
        // A signal published while this thread was runnable must interrupt the
        // next blocking syscall.  Refuse the block while holding the same lock
        // used by deliverSignal(), so no waiter can become stale between the
        // pending-signal check and ready=false.
        if (firstUnblockedPendingSignal(ctx) != 0 and ctx.signal_entry != 0 and
            !ctx.signal_interrupt_consumed)
        {
            ctx.signal_interrupt_consumed = true;
            scheduler_state.thread_table.unlock();
            return false;
        }
        var saved = frame.*;
        saved.rax = resume_rax;
        ctx.frame = saved;
        ctx.cr3 = currentCr3();
        ctx.fs_base = x86_platform.readFsBase();
        ctx.gs_base = x86_platform.readGsBase();
        ctx.pkru = x86_platform.readPkru();
        ctx.wait_mailbox = wait_mailbox;
        ctx.wake_tick = if (timeout_ticks == 0) 0 else lapic_tick_count + timeout_ticks;
        ctx.ready = false;
        setThreadHotCr3(current_thread, ctx.cr3);
        setThreadHotWaitState(current_thread, wait_mailbox, ctx.wake_tick, false);
        verifiedBlockThread(current_thread);
    }
    scheduler_state.thread_table.unlock();
    if (before_block) |callback| callback.run(callback.context);
    if (schedulerStateForSlot(currentCpu())) |state| {
        state.lock.lock();
        state.current_thread = state.idle_thread;
        state.current_principal = null;
        state.current_cr3 = 0;
        state.is_idle = true;
        state.lock.unlock();
    }
    smp.returnCurrentApToIdleFromInterrupt();
}

pub fn exitApThreadToIdle() noreturn {
    exitApThreadToIdleAfter(null);
}

pub fn exitApThreadToIdleAfter(callback: ?BeforeCurrentThreadLeaveCallback) noreturn {
    const current_thread = currentThread();
    _ = releaseThread(current_thread);
    if (callback) |cb| cb.run(cb.context);
    smp.returnCurrentApToIdleFromInterrupt();
}

pub fn preemptBootstrapThread(slice_ticks: u64, frame: *TrapFrame) bool {
    if (slice_ticks == 0 or slice_ticks > std.math.maxInt(u64) / runtime_ns_per_tick) return false;
    const current_thread = currentThread();

    const state = schedulerStateForSlot(currentCpu()) orelse return false;
    state.lock.lock();
    state.tick_accum +%= 1;
    const should_preempt = state.tick_accum >= slice_ticks;
    if (should_preempt) state.tick_accum = 0;
    state.lock.unlock();
    if (!should_preempt) return false;

    const generation = generationOfThread(current_thread) orelse return false;
    if (verifiedExpireSlice(currentCpu(), current_thread, generation, slice_ticks * runtime_ns_per_tick) != .preempt) return false;
    return switchToNextReadyOnCurrentCpu(frame, null);
}

pub fn threadCapacity() usize {
    return scheduler_state.thread_table.contexts.len;
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
    if (required > maxThreadSlots) return null;
    var capacity = scheduler_state.thread_table.contexts.len;
    if (capacity == 0) capacity = initialThreadCapacity;
    while (capacity < required) {
        const doubled = capacity * 2;
        capacity = if (doubled > maxThreadSlots) maxThreadSlots else doubled;
        if (capacity < required and capacity == maxThreadSlots) return null;
    }
    return capacity;
}

fn ensureThreadCapacity(required: usize, free_list: *kernel.FreePageList) bool {
    if (required <= scheduler_state.thread_table.contexts.len) return true;
    const old_capacity = scheduler_state.thread_table.contexts.len;
    const capacity = nextThreadCapacity(required) orelse return false;
    const new_contexts = allocKernelSlice(ThreadContext, free_list, capacity) orelse return false;
    const new_hot_threads = allocKernelSlice(ThreadHotState, free_list, capacity) orelse return false;
    const new_entities = allocKernelSlice(scheduler_runqueue.Node, free_list, capacity - old_capacity) orelse return false;

    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    if (scheduler_state.thread_table.contexts.len != old_capacity) {
        return required <= scheduler_state.thread_table.contexts.len;
    }
    @memcpy(new_contexts[0..old_capacity], scheduler_state.thread_table.contexts);
    @memcpy(new_hot_threads[0..old_capacity], scheduler_state.thread_table.hot_states);
    var i = old_capacity;
    while (i < capacity) : (i += 1) {
        const entity = &new_entities[i - old_capacity];
        entity.reset(i, 1);
        new_contexts[i] = .{ .id = @intCast(i), .scheduler_entity = entity };
        new_hot_threads[i] = .{};
    }

    scheduler_state.thread_table.contexts = new_contexts;
    scheduler_state.thread_table.hot_states = new_hot_threads;
    thread_contexts_ptr = @ptrCast(scheduler_state.thread_table.contexts.ptr);
    return true;
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn staticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_thread_contexts), &initial_thread_contexts));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(thread_contexts_ptr), &thread_contexts_ptr));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_thread_hot_states), &initial_thread_hot_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_scheduler_entities), &initial_scheduler_entities));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_state), &scheduler_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(lapic_tick_count), &lapic_tick_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_tick_accum), &scheduler_tick_accum));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_switch_count), &scheduler_switch_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_fx_state), &initial_fx_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_interrupt_fx_state), &kernel_interrupt_fx_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_lifecycle_gate), &principal_lifecycle_gate));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_lifecycle_target_raw), &principal_lifecycle_target_raw));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_lifecycle_pending_action), &principal_lifecycle_pending_action));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_lifecycle_pending_code), &principal_lifecycle_pending_code));
    return end;
}

pub fn currentThread() usize {
    const state = schedulerStateForSlot(currentCpu()) orelse return 0;
    return state.current_thread;
}

pub fn currentPrincipal() kernel.PrincipalId {
    const state = schedulerStateForSlot(currentCpu()) orelse return default_process_principal;
    const thread_index = state.current_thread;
    if (threadContext(thread_index)) |ctx| {
        if (ctx.allocated) return ctx.owner_process;
    }
    return state.current_principal orelse default_process_principal;
}

pub fn currentCr3() u64 {
    const state = schedulerStateForSlot(currentCpu()) orelse return 0;
    const thread_index = state.current_thread;
    if (threadContext(thread_index)) |ctx| {
        if (ctx.allocated and ctx.cr3 != 0) return ctx.cr3;
    }
    return state.current_cr3;
}

/// Serialize process lifecycle transitions while a remote principal is being
/// driven to a user/kernel boundary.  Callers hold the kernel-state lock when
/// acquiring this gate, so acquisition must never spin: a quiescing caller has
/// deliberately dropped that lock and may be waiting for this CPU.
pub fn tryBeginPrincipalLifecycle(target: kernel.PrincipalId) bool {
    if (@cmpxchgStrong(u8, &principal_lifecycle_gate, 0, 1, .acquire, .monotonic) != null) {
        return false;
    }
    principal_lifecycle_target_raw = @intFromEnum(target);
    principal_lifecycle_pending_code = 0;
    @atomicStore(u8, &principal_lifecycle_pending_action, @intFromEnum(PrincipalLifecycleAction.none), .monotonic);
    @atomicStore(u8, &principal_lifecycle_gate, 2, .release);
    return true;
}

pub fn endPrincipalLifecycle() void {
    @atomicStore(u8, &principal_lifecycle_pending_action, @intFromEnum(PrincipalLifecycleAction.none), .monotonic);
    principal_lifecycle_pending_code = 0;
    principal_lifecycle_target_raw = std.math.maxInt(usize);
    @atomicStore(u8, &principal_lifecycle_gate, 0, .release);
}

pub fn principalLifecycleTargets(principal: kernel.PrincipalId) bool {
    return @atomicLoad(u8, &principal_lifecycle_gate, .acquire) == 2 and
        principal_lifecycle_target_raw == @intFromEnum(principal);
}

/// Record the last non-terminal lifecycle request while the owner has dropped
/// the kernel-state lock. Kill is terminal and cannot be overwritten.
pub fn requestPrincipalLifecycleAction(
    principal: kernel.PrincipalId,
    action: PrincipalLifecycleAction,
    code: u32,
) bool {
    if (action == .none or !principalLifecycleTargets(principal)) return false;
    const current: PrincipalLifecycleAction = @enumFromInt(
        @atomicLoad(u8, &principal_lifecycle_pending_action, .acquire),
    );
    if (current == .kill) return action == .kill;
    principal_lifecycle_pending_code = code;
    @atomicStore(u8, &principal_lifecycle_pending_action, @intFromEnum(action), .release);
    return true;
}

pub fn takePrincipalLifecycleAction(principal: kernel.PrincipalId) PendingPrincipalLifecycleAction {
    if (!principalLifecycleTargets(principal)) return .{ .action = .none, .code = 0 };
    const raw = @atomicRmw(
        u8,
        &principal_lifecycle_pending_action,
        .Xchg,
        @intFromEnum(PrincipalLifecycleAction.none),
        .acq_rel,
    );
    return .{ .action = @enumFromInt(raw), .code = principal_lifecycle_pending_code };
}

pub fn cpuMaskRunningPrincipal(principal: kernel.PrincipalId) u64 {
    var mask: u64 = 0;
    var cpu_slot: usize = 0;
    while (cpu_slot < scheduler_state.cpus.len and cpu_slot < 64) : (cpu_slot += 1) {
        const state = &scheduler_state.cpus[cpu_slot];
        state.lock.lock();
        const owns_principal = state.enabled and !state.is_idle and
            state.current_principal != null and state.current_principal.? == principal;
        state.lock.unlock();
        if (owns_principal) mask |= @as(u64, 1) << @intCast(cpu_slot);
    }
    return mask;
}

/// Actively drive every CPU executing `principal` through the scheduler IPI
/// safe point.  There is intentionally no elapsed-time escape: teardown is
/// permitted only after the ownership snapshot reaches zero.
fn waitForPrincipalQuiescenceIgnoring(principal: kernel.PrincipalId, ignored_mask: u64) void {
    var iterations: u64 = 0;
    while (true) {
        const mask = cpuMaskRunningPrincipal(principal) & ~ignored_mask;
        if (mask == 0) return;
        iterations +%= 1;
        if (iterations == 100_000) {
            kernel_log.writeFmt(
                "[kernel] lifecycle quiescence pending principal={} mask=0x{x}\n",
                .{ @intFromEnum(principal), mask },
            );
        }
        var cpu_slot: usize = 0;
        while (cpu_slot < scheduler_state.cpus.len and cpu_slot < 64) : (cpu_slot += 1) {
            if ((mask & (@as(u64, 1) << @intCast(cpu_slot))) == 0) continue;
            _ = smp.interruptCpu(cpu_slot);
        }
        // Syscalls enter with IF clear.  Open an interrupt window so this CPU
        // can still acknowledge cross-CPU maintenance while it coordinates.
        asm volatile ("sti; pause; cli" ::: .{ .memory = true });
    }
}

pub fn waitForPrincipalQuiescence(principal: kernel.PrincipalId) void {
    waitForPrincipalQuiescenceIgnoring(principal, 0);
}

pub fn waitForRemotePrincipalQuiescence(principal: kernel.PrincipalId) void {
    const cpu_slot = currentCpu();
    const ignored = if (cpu_slot < 64) @as(u64, 1) << @intCast(cpu_slot) else 0;
    waitForPrincipalQuiescenceIgnoring(principal, ignored);
}

fn buildInitialCpuSchedulerStates() [smp.max_cpus]CpuSchedulerState {
    var states: [smp.max_cpus]CpuSchedulerState = [_]CpuSchedulerState{.{}} ** smp.max_cpus;
    states[bootstrap_cpu_slot].enabled = true;
    states[bootstrap_cpu_slot].current_thread = 0;
    states[bootstrap_cpu_slot].is_idle = false;
    return states;
}

fn primarySchedulerState() *CpuSchedulerState {
    return &scheduler_state.cpus[bootstrap_cpu_slot];
}

fn schedulerStateForSlot(cpu_slot: usize) ?*CpuSchedulerState {
    if (cpu_slot >= scheduler_state.cpus.len) return null;
    return &scheduler_state.cpus[cpu_slot];
}

fn lockAllCpuSchedulerStates() void {
    var cpu_slot: usize = 0;
    while (cpu_slot < scheduler_state.cpus.len) : (cpu_slot += 1) {
        scheduler_state.cpus[cpu_slot].lock.lock();
    }
}

fn unlockAllCpuSchedulerStates() void {
    var remaining = scheduler_state.cpus.len;
    while (remaining != 0) {
        remaining -= 1;
        scheduler_state.cpus[remaining].lock.unlock();
    }
}

fn threadAssignedCpuSlot(thread_index: usize) usize {
    const ctx = threadContext(thread_index) orelse return bootstrap_cpu_slot;
    if (!ctx.allocated) return bootstrap_cpu_slot;
    if (ctx.cpu_slot >= scheduler_state.cpus.len) return bootstrap_cpu_slot;
    return ctx.cpu_slot;
}

fn hasAllocatedThreadLocked() bool {
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        if (scheduler_state.thread_table.contexts[i].allocated) return true;
    }
    return false;
}

fn selectCpuForNewThreadLocked() usize {
    if (!verifiedCoreReady()) return bootstrap_cpu_slot;
    if (!hasAllocatedThreadLocked()) return bootstrap_cpu_slot;
    const cpu_count = verifiedCoreCpuCount();
    if (cpu_count <= 1) return bootstrap_cpu_slot;
    var attempts: usize = 0;
    var cursor = scheduler_state.thread_table.next_cpu_cursor;
    if (cursor >= cpu_count) cursor = bootstrap_cpu_slot;
    while (attempts < cpu_count) : (attempts += 1) {
        const cpu_slot = cursor;
        cursor = (cursor + 1) % cpu_count;
        if (schedulerStateForSlot(cpu_slot)) |state| {
            if (state.enabled) {
                scheduler_state.thread_table.next_cpu_cursor = cursor;
                return cpu_slot;
            }
        }
    }
    return bootstrap_cpu_slot;
}

pub fn currentCpu() usize {
    return smp.currentCpuSlot();
}

pub fn isBootstrapSchedulerCpu() bool {
    return smp.isBootstrapCpu();
}

pub fn installIdleHooks() void {
    scheduler_observer.registerIdleObserver(observeCpuIdleFromAp);
    scheduler_observer.registerIdleUserEntryPoll(claimKernelScheduledUserEntryFromAp);
}

pub fn refreshTopology() void {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const count = @min(smp.cpuCount(), scheduler_state.cpus.len);
    var i: usize = 0;
    while (i < scheduler_state.cpus.len) : (i += 1) {
        const observed = i < count and smp.cpuState(i) != .absent;
        const state = &scheduler_state.cpus[i];
        state.enabled = observed;
        if (!observed or i != bootstrap_cpu_slot) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
        }
    }
}

pub fn apUserDispatchReady() bool {
    return verifiedCoreReady();
}

pub fn parkApAfterThreadStopped() noreturn {
    while (true) {
        asm volatile ("cli; hlt");
    }
}

pub fn noteIdleTick() void {
    noteCpuIdleTick(currentCpu());
}

fn noteCpuIdleTick(cpu_slot: usize) void {
    const state = schedulerStateForSlot(cpu_slot) orelse return;
    state.lock.lock();
    defer state.lock.unlock();
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
}

pub fn recordApUserTimerFrame(frame: *const TrapFrame) bool {
    _ = frame;
    return false;
}

pub fn apUserThreadCanContinue() bool {
    const cpu_slot = currentCpu();
    if (cpu_slot == bootstrap_cpu_slot) return true;
    const thread_index = currentThread();
    if (thread_index >= scheduler_state.thread_table.contexts.len) return false;
    const ctx = threadContext(thread_index) orelse return false;
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    return ctx.allocated and
        hot.allocated != 0 and
        ctx.ready and
        hot.ready != 0 and
        ctx.cpu_slot == cpu_slot;
}

/// Save a stopped thread at an interrupt boundary and sever the scheduler's
/// CPU ownership before its process can be torn down.  The caller has already
/// saved the architectural FX state for the interrupted user context.
pub fn quiesceStoppedCurrentUserThread(frame: *const TrapFrame) bool {
    const cpu_slot = currentCpu();
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    scheduler_state.thread_table.lock();
    state.lock.lock();
    const thread_index = state.current_thread;
    const ctx = threadContextMutable(thread_index) orelse {
        state.lock.unlock();
        scheduler_state.thread_table.unlock();
        return false;
    };
    if (!ctx.allocated or !ctx.stopped or ctx.owner_process != state.current_principal) {
        state.lock.unlock();
        scheduler_state.thread_table.unlock();
        return false;
    }
    ctx.frame = frame.*;
    if (state.current_cr3 != 0) ctx.cr3 = state.current_cr3;
    ctx.fs_base = x86_platform.readFsBase();
    ctx.gs_base = x86_platform.readGsBase();
    ctx.pkru = x86_platform.readPkru();
    ctx.ready = false;
    setThreadHotCr3(thread_index, ctx.cr3);
    setThreadHotReady(thread_index, false);
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
    state.tick_accum = 0;
    state.lock.unlock();
    scheduler_state.thread_table.unlock();
    return true;
}

/// Complete a self-stop at the syscall boundary. The stopped frame remains
/// dormant until PROCESS_CONTINUE republishes it; this syscall never returns
/// to the stopped user context merely because a timer tick has not arrived.
pub fn parkStoppedCurrentThread(
    frame: *TrapFrame,
    resume_rax: u64,
    before_leave: ?BeforeCurrentThreadLeaveCallback,
) bool {
    var stopped_frame = frame.*;
    stopped_frame.rax = resume_rax;
    if (!quiesceStoppedCurrentUserThread(&stopped_frame)) return false;
    if (before_leave) |callback| callback.run(callback.context);
    if (!isBootstrapSchedulerCpu()) {
        smp.returnCurrentApToIdleFromInterrupt();
    }
    loadNextReadyThreadOrIdle(frame);
    return true;
}

pub fn shouldPreemptApUserThread() bool {
    return false;
}

fn getUserSpace(user_spaces: []UserAddressSpace, principal: kernel.PrincipalId) ?*UserAddressSpace {
    const idx = kernel.processIndexFromPrincipal(principal) orelse return null;
    if (idx >= user_spaces.len) return null;
    return &user_spaces[idx];
}

fn pcidForPrincipal(principal: kernel.PrincipalId) u16 {
    const idx = kernel.processIndexFromPrincipal(principal) orelse return 0;
    return x86_platform.userPcidForProcessIndex(idx);
}

pub fn threadContextMutable(thread_index: usize) ?*ThreadContext {
    if (thread_index >= scheduler_state.thread_table.contexts.len) return null;
    return &scheduler_state.thread_table.contexts[thread_index];
}

pub fn threadContext(thread_index: usize) ?*const ThreadContext {
    if (thread_index >= scheduler_state.thread_table.contexts.len) return null;
    return &scheduler_state.thread_table.contexts[thread_index];
}

fn getThreadHotState(thread_index: usize) ?*ThreadHotState {
    if (thread_index >= scheduler_state.thread_table.contexts.len) return null;
    return &scheduler_state.thread_table.hot_states[thread_index];
}

fn getThreadHotStateConst(thread_index: usize) ?*const ThreadHotState {
    if (thread_index >= scheduler_state.thread_table.contexts.len) return null;
    return &scheduler_state.thread_table.hot_states[thread_index];
}

fn boolByte(value: bool) u8 {
    return if (value) 1 else 0;
}

fn hotStateFromContext(ctx: *const ThreadContext) ThreadHotState {
    return .{
        .allocated = boolByte(ctx.allocated),
        .ready = boolByte(ctx.ready),
        .stopped = boolByte(ctx.stopped),
        .wait_mailbox = boolByte(ctx.wait_mailbox),
        .owner_process = ctx.owner_process,
        .pending_signal_mask = ctx.pending_signal_mask,
        .wake_tick = ctx.wake_tick,
        .cr3 = ctx.cr3,
    };
}

fn syncHotStateFromContext(thread_index: usize) void {
    const ctx = threadContext(thread_index) orelse return;
    const hot = getThreadHotState(thread_index) orelse return;
    hot.* = hotStateFromContext(ctx);
}

fn setThreadHotReady(thread_index: usize, ready: bool) void {
    if (getThreadHotState(thread_index)) |hot| hot.ready = boolByte(ready);
}

fn setThreadHotPendingSignalMask(thread_index: usize, signal_mask: u64) void {
    if (getThreadHotState(thread_index)) |hot| hot.pending_signal_mask = signal_mask;
}

fn setThreadHotStopped(thread_index: usize, stopped: bool) void {
    if (getThreadHotState(thread_index)) |hot| hot.stopped = boolByte(stopped);
}

fn setThreadHotWaitState(thread_index: usize, wait_mailbox: bool, wake_tick: u64, ready: bool) void {
    if (getThreadHotState(thread_index)) |hot| {
        hot.wait_mailbox = boolByte(wait_mailbox);
        hot.wake_tick = wake_tick;
        hot.ready = boolByte(ready);
    }
}

fn setThreadHotCr3(thread_index: usize, cr3: u64) void {
    if (getThreadHotState(thread_index)) |hot| hot.cr3 = cr3;
}

pub fn threadReady(thread_index: usize) bool {
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    return hot.allocated != 0 and hot.ready != 0;
}

pub fn debugDumpRunnableState(reason: []const u8) void {
    kernel_log.writeFmt(
        "[sched-debug] reason={s} cpu={} verified={}\n",
        .{
            reason,
            currentCpu(),
            if (verifiedCoreReady()) @as(u8, 1) else @as(u8, 0),
        },
    );
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const ctx = threadContext(i) orelse continue;
        if (!ctx.allocated) continue;
        const hot = getThreadHotStateConst(i) orelse continue;
        kernel_log.writeFmt(
            "[sched-debug] thread={} gen={} owner={} cpu={} ready={} wait={} stopped={} wake={} hot_ready={} hot_wait={} hot_stopped={} hot_wake={}\n",
            .{
                i,
                ctx.generation,
                @intFromEnum(ctx.owner_process),
                ctx.cpu_slot,
                if (ctx.ready) @as(u8, 1) else @as(u8, 0),
                if (ctx.wait_mailbox) @as(u8, 1) else @as(u8, 0),
                if (ctx.stopped) @as(u8, 1) else @as(u8, 0),
                ctx.wake_tick,
                hot.ready,
                hot.wait_mailbox,
                hot.stopped,
                hot.wake_tick,
            },
        );
    }
}

fn initializeThreadContextWithReadyState(
    thread_index: usize,
    owner_process: kernel.PrincipalId,
    user_spaces: []UserAddressSpace,
    initial_frame: TrapFrame,
    initial_ready: bool,
) bool {
    const space = getUserSpace(user_spaces, owner_process) orelse return false;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (ctx.allocated) return false;
    ctx.id = @intCast(thread_index);
    ctx.allocated = true;
    ctx.owner_process = owner_process;
    ctx.cpu_slot = selectCpuForNewThreadLocked();
    ctx.cpu_affinity_mask = all_cpu_affinity_mask;
    ctx.cr3 = x86_platform.cr3WithUserPcid(space.cr3, pcidForPrincipal(owner_process));
    ctx.fs_base = 0;
    ctx.gs_base = 0;
    ctx.pkru = 0;
    ctx.ready = initial_ready;
    ctx.wait_mailbox = false;
    ctx.stopped = false;
    ctx.resume_after_stop = false;
    ctx.pending_signal_mask = 0;
    ctx.signal_interrupt_consumed = false;
    ctx.signal_entry = 0;
    ctx.signal_blocked_mask = 0;
    ctx.signal_inhibit_start = 0;
    ctx.signal_inhibit_end = 0;
    ctx.signal_inhibit_secondary_start = 0;
    ctx.signal_inhibit_secondary_end = 0;
    // A newly created thread inherits the runtime signal trampoline from a
    // sibling but is never the process signal owner; only an explicit REGISTER
    // (the main/event-loop thread) claims ownership.
    ctx.signal_owner = false;
    var inherit_index: usize = 0;
    while (inherit_index < scheduler_state.thread_table.contexts.len) : (inherit_index += 1) {
        if (inherit_index == thread_index) continue;
        const source = threadContext(inherit_index) orelse continue;
        if (!source.allocated or source.owner_process != owner_process or source.signal_entry == 0) continue;
        ctx.signal_entry = source.signal_entry;
        ctx.signal_blocked_mask = source.signal_blocked_mask;
        ctx.signal_inhibit_start = source.signal_inhibit_start;
        ctx.signal_inhibit_end = source.signal_inhibit_end;
        ctx.signal_inhibit_secondary_start = source.signal_inhibit_secondary_start;
        ctx.signal_inhibit_secondary_end = source.signal_inhibit_secondary_end;
        break;
    }
    ctx.wake_tick = 0;
    ctx.frame = initial_frame;
    ctx.fx_state = initial_fx_state;
    syncHotStateFromContext(thread_index);
    return true;
}

pub fn initializeThreadContext(
    thread_index: usize,
    owner_process: kernel.PrincipalId,
    user_spaces: []UserAddressSpace,
    initial_frame: TrapFrame,
) bool {
    return initializeThreadContextWithReadyState(thread_index, owner_process, user_spaces, initial_frame, true);
}

pub fn threadForPrincipal(principal: kernel.PrincipalId) ?usize {
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const ctx = threadContext(i) orelse continue;
        if (ctx.allocated and ctx.owner_process == principal) return i;
    }
    return null;
}

pub fn generationOfThread(thread_index: usize) ?u32 {
    const ctx = threadContext(thread_index) orelse return null;
    if (!ctx.allocated) return null;
    return ctx.generation;
}

pub fn threadOwnedBy(thread_index: usize, principal: kernel.PrincipalId) bool {
    const ctx = threadContext(thread_index) orelse return false;
    return ctx.allocated and ctx.owner_process == principal;
}

pub fn threadWakeTargetIsLive(thread_index: usize, generation: u32, owner: kernel.PrincipalId) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContext(thread_index) orelse return false;
    return ctx.allocated and ctx.generation == generation and ctx.owner_process == owner;
}

pub fn liveThreadCount(principal: kernel.PrincipalId) usize {
    var count: usize = 0;
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const ctx = threadContext(i) orelse continue;
        if (ctx.allocated and ctx.owner_process == principal) count += 1;
    }
    return count;
}

pub fn allocateReadyThread(owner_process: kernel.PrincipalId, user_spaces: []UserAddressSpace, initial_frame: TrapFrame, free_list: *kernel.FreePageList) ?usize {
    return allocateThreadWithReadyState(owner_process, user_spaces, initial_frame, true, free_list);
}

pub fn allocateSuspendedThread(owner_process: kernel.PrincipalId, user_spaces: []UserAddressSpace, initial_frame: TrapFrame, free_list: *kernel.FreePageList) ?usize {
    return allocateThreadWithReadyState(owner_process, user_spaces, initial_frame, false, free_list);
}

fn allocateThreadWithReadyState(owner_process: kernel.PrincipalId, user_spaces: []UserAddressSpace, initial_frame: TrapFrame, initial_ready: bool, free_list: *kernel.FreePageList) ?usize {
    while (true) {
        var i: usize = 0;
        while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
            const ctx = threadContext(i) orelse continue;
            if (ctx.allocated) continue;
            if (!initializeThreadContextWithReadyState(i, owner_process, user_spaces, initial_frame, initial_ready)) return null;
            const generation = scheduler_state.thread_table.contexts[i].generation;
            if (!verifiedAddThread(i, generation, initial_ready)) {
                scheduler_state.thread_table.lock();
                if (threadContextMutable(i)) |failed_ctx| {
                    if (failed_ctx.allocated and failed_ctx.generation == generation) {
                        const entity = failed_ctx.scheduler_entity;
                        const next_generation = nextThreadGeneration(generation);
                        if (entity) |node| node.reset(i, next_generation);
                        failed_ctx.* = .{
                            .id = @intCast(i),
                            .generation = next_generation,
                            .scheduler_entity = entity,
                        };
                        syncHotStateFromContext(i);
                    }
                }
                scheduler_state.thread_table.unlock();
                return null;
            }
            return i;
        }
        if (!ensureThreadCapacity(scheduler_state.thread_table.contexts.len + 1, free_list)) return null;
    }
}

pub fn releaseThread(thread_index: usize) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();

    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    const generation = ctx.generation;
    const cpu_slot = ctx.cpu_slot;
    if (!verifiedExitThreadGeneration(thread_index, generation)) return false;
    const next_generation = nextThreadGeneration(ctx.generation);
    const entity = ctx.scheduler_entity orelse return false;
    if (schedulerStateForSlot(cpu_slot)) |state| {
        state.lock.lock();
        if (state.current_thread == thread_index) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
        }
        state.lock.unlock();
    }
    entity.reset(thread_index, next_generation);
    ctx.* = .{ .id = @intCast(thread_index), .generation = next_generation, .scheduler_entity = entity };
    syncHotStateFromContext(thread_index);
    return true;
}

fn activateGenerationLocked(thread_index: usize, generation: u32) bool {
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    if (hot.allocated == 0) return false;
    if (hot.ready == 0) return false;
    const cpu_slot = currentCpu();
    const ctx = threadContext(thread_index) orelse return false;
    if (!ctx.allocated or ctx.generation != generation) return false;
    if (ctx.cpu_slot != cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    if (!state.enabled) return false;
    state.current_thread = thread_index;
    state.current_principal = hot.owner_process;
    state.current_cr3 = hot.cr3;
    state.is_idle = false;
    if (!applyThreadBases(thread_index)) return false;
    return true;
}

fn activateGeneration(thread_index: usize, generation: u32) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return activateGenerationLocked(thread_index, generation);
}

pub fn activate(thread_index: usize) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    return activateGenerationLocked(thread_index, ctx.generation);
}

pub fn applyThreadBases(thread_index: usize) bool {
    const ctx = threadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    x86_platform.writeFsBase(ctx.fs_base);
    x86_platform.writeGsBase(ctx.gs_base);
    x86_platform.writePkru(ctx.pkru);
    return true;
}

pub fn setFsBase(thread_index: usize, fs_base: u64) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    ctx.fs_base = fs_base;
    if (thread_index == currentThread()) x86_platform.writeFsBase(fs_base);
    return true;
}

pub fn setCurrentGsBase(gs_base: u64) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(currentThread()) orelse return false;
    if (!ctx.allocated) return false;
    ctx.gs_base = gs_base;
    x86_platform.writeGsBase(gs_base);
    return true;
}

pub fn setThreadGsBase(thread_index: usize, gs_base: u64) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    ctx.gs_base = gs_base;
    if (thread_index == currentThread()) x86_platform.writeGsBase(gs_base);
    return true;
}

pub fn currentFsBase() u64 {
    return x86_platform.readFsBase();
}

pub fn currentGsBase() u64 {
    return x86_platform.readGsBase();
}

pub fn suspendedThreadImage(
    thread_index: usize,
    expected_generation: u32,
    owner_process: kernel.PrincipalId,
) ?SuspendedThreadImage {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContext(thread_index) orelse return null;
    if (!ctx.allocated or
        ctx.generation != expected_generation or
        ctx.owner_process != owner_process or
        ctx.ready or
        ctx.wait_mailbox or
        ctx.wake_tick != 0)
    {
        return null;
    }
    return .{
        .frame = ctx.frame,
        .fs_base = ctx.fs_base,
        .gs_base = ctx.gs_base,
        .pkru = ctx.pkru,
    };
}

pub fn installExecContextForCurrentThread(
    user_spaces: []UserAddressSpace,
    owner_process: kernel.PrincipalId,
    image: SuspendedThreadImage,
) bool {
    const space = getUserSpace(user_spaces, owner_process) orelse return false;
    const cr3 = x86_platform.cr3WithUserPcid(space.cr3, pcidForPrincipal(owner_process));
    const current_thread = currentThread();

    scheduler_state.thread_table.lock();
    {
        const ctx = threadContextMutable(current_thread) orelse {
            scheduler_state.thread_table.unlock();
            return false;
        };
        if (!ctx.allocated or ctx.owner_process != owner_process) {
            scheduler_state.thread_table.unlock();
            return false;
        }
        ctx.cr3 = cr3;
        ctx.fs_base = image.fs_base;
        ctx.gs_base = image.gs_base;
        ctx.pkru = image.pkru;
        ctx.frame = image.frame;
        ctx.ready = true;
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.stopped = false;
        ctx.resume_after_stop = false;
        syncHotStateFromContext(current_thread);
    }
    scheduler_state.thread_table.unlock();

    if (schedulerStateForSlot(currentCpu())) |state| {
        state.lock.lock();
        state.current_thread = current_thread;
        state.current_principal = owner_process;
        state.current_cr3 = cr3;
        state.is_idle = false;
        state.lock.unlock();
    }
    x86_platform.writeFsBase(image.fs_base);
    x86_platform.writeGsBase(image.gs_base);
    x86_platform.writePkru(image.pkru);
    return true;
}

fn saveCurrentThreadContextFromFrame(frame: *const TrapFrame) void {
    const current_thread = currentThread();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(current_thread) orelse return;
    if (!ctx.allocated) return;
    ctx.frame = frame.*;
    ctx.cr3 = currentCr3();
    ctx.fs_base = x86_platform.readFsBase();
    ctx.gs_base = x86_platform.readGsBase();
    ctx.pkru = x86_platform.readPkru();
    setThreadHotCr3(current_thread, ctx.cr3);
    const hot = getThreadHotStateConst(current_thread) orelse return;
    if (hot.wait_mailbox == 0 and hot.wake_tick == 0) {
        ctx.ready = true;
        setThreadHotReady(current_thread, true);
    }
}

fn loadContextIntoFrameGenerationLocked(thread_index: usize, generation: u32, frame: *TrapFrame) bool {
    const ctx = threadContext(thread_index) orelse return false;
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    if (!ctx.allocated or ctx.generation != generation or hot.allocated == 0) return false;
    if (hot.ready == 0) return false;
    schedulerFullMemoryFence();
    x86_platform.writeFsBase(ctx.fs_base);
    x86_platform.writeGsBase(ctx.gs_base);
    x86_platform.writePkru(ctx.pkru);
    frame.* = ctx.frame;
    return true;
}

fn loadContextIntoFrameGeneration(thread_index: usize, generation: u32, frame: *TrapFrame) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return loadContextIntoFrameGenerationLocked(thread_index, generation, frame);
}

pub fn loadContextIntoFrame(thread_index: usize, frame: *TrapFrame) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    return loadContextIntoFrameGenerationLocked(thread_index, ctx.generation, frame);
}

fn switchToGeneration(next_thread: usize, next_generation: u32, frame: *TrapFrame, saved_rax: ?u64) bool {
    if (!isBootstrapSchedulerCpu() and !apUserDispatchReady()) return false;
    if (next_thread >= scheduler_state.thread_table.contexts.len) return false;
    const current_thread = currentThread();
    const current_generation = generationOfThread(current_thread) orelse return false;
    if (next_thread == current_thread) {
        if (current_generation != next_generation) return false;
        if (saved_rax) |value| frame.rax = value;
        return true;
    }

    var saved = frame.*;
    if (saved_rax) |value| saved.rax = value;
    saveCurrentThreadContextFromFrame(&saved);

    if (!activateGeneration(next_thread, next_generation)) return false;
    if (!loadContextIntoFrameGeneration(next_thread, next_generation, frame)) {
        _ = activateGeneration(current_thread, current_generation);
        _ = loadContextIntoFrameGeneration(current_thread, current_generation, frame);
        return false;
    }
    return true;
}

pub fn switchTo(next_thread: usize, frame: *TrapFrame, saved_rax: ?u64) bool {
    const generation = generationOfThread(next_thread) orelse return false;
    return switchToGeneration(next_thread, generation, frame, saved_rax);
}

pub fn handoffToReadyThreadGenerationWithRax(
    frame: *TrapFrame,
    target_thread: usize,
    target_generation: u32,
    sender_rax: u64,
    before_current_thread_leave: ?BeforeCurrentThreadLeaveCallback,
) bool {
    if (!verifiedCoreReady()) return false;
    const cpu_id = currentCpu();
    if (cpu_id >= verifiedCoreCpuCount()) return false;
    const current_thread = currentThread();
    if (target_thread == current_thread) return false;

    scheduler_state.thread_table.lock();
    const current_ctx = threadContext(current_thread) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    const target_ctx = threadContext(target_thread) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    const current_generation = current_ctx.generation;
    const can_handoff =
        current_ctx.allocated and
        target_ctx.allocated and
        target_ctx.generation == target_generation and
        target_ctx.ready and
        current_ctx.cpu_slot == cpu_id and
        target_ctx.cpu_slot == cpu_id;
    scheduler_state.thread_table.unlock();
    if (!can_handoff) return false;
    if (!verifiedHandoffToThreadOnCpu(cpu_id, target_thread, target_generation, current_thread, current_generation)) return false;
    if (before_current_thread_leave) |callback| callback.run(callback.context);
    if (switchTo(target_thread, frame, sender_rax)) return true;

    verifiedRollbackHandoff(cpu_id, current_thread, current_generation);
    return false;
}

pub fn prepareExitHandoffToReadyThreadGeneration(target_thread: usize, target_generation: u32) bool {
    if (!verifiedCoreReady()) return false;
    const cpu_id = currentCpu();
    if (cpu_id >= verifiedCoreCpuCount()) return false;
    const current_thread = currentThread();
    if (target_thread == current_thread) return false;

    scheduler_state.thread_table.lock();
    const current_ctx = threadContext(current_thread) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    const target_ctx = threadContext(target_thread) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    const current_generation = current_ctx.generation;
    const can_handoff =
        current_ctx.allocated and
        target_ctx.allocated and
        target_ctx.generation == target_generation and
        target_ctx.ready and
        current_ctx.cpu_slot == cpu_id and
        target_ctx.cpu_slot == cpu_id;
    scheduler_state.thread_table.unlock();
    if (!can_handoff) return false;
    return verifiedHandoffToThreadOnCpu(cpu_id, target_thread, target_generation, current_thread, current_generation);
}

pub fn loadExitHandoffThread(frame: *TrapFrame, target_thread: usize, target_generation: u32) bool {
    const cpu_id = currentCpu();
    scheduler_state.thread_table.lock();
    const target_ctx = threadContext(target_thread) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    const can_load =
        target_ctx.allocated and
        target_ctx.generation == target_generation and
        target_ctx.ready and
        target_ctx.cpu_slot == cpu_id;
    scheduler_state.thread_table.unlock();
    if (!can_load) return false;
    if (!activate(target_thread)) return false;
    return loadContextIntoFrame(target_thread, frame);
}

pub fn exitCurrentThread(
    frame: *TrapFrame,
    _: u64,
    before_ap_idle: ?BeforeCurrentThreadLeaveCallback,
    after_release: ?BeforeCurrentThreadLeaveCallback,
) bool {
    if (!isBootstrapSchedulerCpu()) {
        const current_thread = currentThread();
        _ = releaseThread(current_thread);
        if (after_release) |callback| callback.run(callback.context);
        if (before_ap_idle) |callback| callback.run(callback.context);
        smp.returnCurrentApToIdleFromInterrupt();
    }
    const current_thread = currentThread();
    if (!releaseThread(current_thread)) return false;
    if (after_release) |callback| callback.run(callback.context);
    if (switchToNextReadyOnCurrentCpu(frame, null)) return true;
    // A bootstrap-CPU thread may be the last local runnable entity while work
    // remains queued elsewhere.  Reclaim one runnable generation before
    // waiting interruptibly for a later local wake.
    loadNextReadyThreadOrIdle(frame);
    return true;
}

pub fn wakeIfWaiting(thread_index: usize) void {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return;
    if (!ctx.allocated) return;
    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    if (ctx.stopped) {
        ctx.resume_after_stop = true;
        ctx.ready = false;
        setThreadHotWaitState(thread_index, false, 0, false);
        return;
    }
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    verifiedWakeThread(thread_index);
}

fn wakeIfWaitingGenerationInternal(
    thread_index: usize,
    generation: u32,
    resume_rax: ?u64,
    mailbox_only: bool,
) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!ctx.allocated or ctx.generation != generation) return false;
    if (mailbox_only) {
        if (!ctx.wait_mailbox) return false;
    } else if (ctx.ready) {
        return false;
    }
    if (resume_rax) |value| ctx.frame.rax = value;
    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    if (ctx.stopped) {
        ctx.resume_after_stop = true;
        ctx.ready = false;
        setThreadHotWaitState(thread_index, false, 0, false);
        return true;
    }
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    verifiedWakeThread(thread_index);
    return true;
}

pub fn wakeIfWaitingGeneration(thread_index: usize, generation: u32) bool {
    return wakeIfWaitingGenerationInternal(thread_index, generation, null, true);
}

pub fn wakeIfWaitingGenerationWithRax(thread_index: usize, generation: u32, resume_rax: u64) bool {
    return wakeIfWaitingGenerationInternal(thread_index, generation, resume_rax, true);
}

pub fn wakeBlockedGenerationWithRax(thread_index: usize, generation: u32, resume_rax: u64) bool {
    return wakeIfWaitingGenerationInternal(thread_index, generation, resume_rax, false);
}

pub fn wakeMailboxWaiter(principal: kernel.PrincipalId) void {
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0 or hot.owner_process != principal) continue;
        if (hot.wait_mailbox == 0) continue;
        wakeIfWaiting(i);
        return;
    }
}

pub fn wakeBlockedThread(principal: kernel.PrincipalId) void {
    wakeMailboxWaiter(principal);
}

pub const SignalDelivery = struct {
    thread_index: usize,
    thread_generation: u32,
    should_interrupt: bool,
    was_blocked: bool,
};

fn signalBit(signo: u32) ?u64 {
    if (signo == 0 or signo > 64) return null;
    return @as(u64, 1) << @intCast(signo - 1);
}

fn firstUnblockedPendingSignal(ctx: *const ThreadContext) u32 {
    const deliverable = ctx.pending_signal_mask & ~ctx.signal_blocked_mask;
    if (deliverable == 0) return 0;
    return @as(u32, @intCast(@ctz(deliverable))) + 1;
}

pub fn deliverSignal(principal: kernel.PrincipalId, signo: u32) ?SignalDelivery {
    const signal_bit = signalBit(signo) orelse return null;
    scheduler_state.thread_table.lock();
    // Select the delivery target under the table lock so the pending signal is
    // published before any wake.  Prefer the registered signal owner (the
    // process's main/event-loop thread); a process-directed signal must not be
    // consumed by an arbitrary worker that happens to block first.  Fall back to
    // any runnable/blocked thread only when no owner has registered.
    var owner_index: ?usize = null;
    var first_blocked: ?usize = null;
    var first_ready: ?usize = null;
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const ctx = threadContext(i) orelse continue;
        if (!ctx.allocated or ctx.owner_process != principal or ctx.stopped) continue;
        if (ctx.signal_owner and owner_index == null) owner_index = i;
        if (!ctx.ready) {
            if (first_blocked == null) first_blocked = i;
        } else if (first_ready == null) {
            first_ready = i;
        }
    }
    const target = owner_index orelse first_blocked orelse first_ready orelse {
        scheduler_state.thread_table.unlock();
        return null;
    };
    const ctx = threadContextMutable(target) orelse {
        scheduler_state.thread_table.unlock();
        return null;
    };
    if (!ctx.allocated or ctx.owner_process != principal or ctx.stopped) {
        scheduler_state.thread_table.unlock();
        return null;
    }
    const newly_pending = (ctx.pending_signal_mask & signal_bit) == 0;
    const should_interrupt = newly_pending and (ctx.signal_blocked_mask & signal_bit) == 0;
    const was_blocked = should_interrupt and !ctx.ready;
    const generation = ctx.generation;
    ctx.pending_signal_mask |= signal_bit;
    // A blocked thread is interrupted immediately below by signalProcess().
    // A runnable thread consumes the interruption when it next attempts to
    // block. Standard signals coalesce by bit, while different blocked signals
    // remain pending until their mask permits delivery.
    if (should_interrupt) {
        ctx.signal_interrupt_consumed = was_blocked;
    } else if (firstUnblockedPendingSignal(ctx) == 0) {
        ctx.signal_interrupt_consumed = true;
    }
    setThreadHotPendingSignalMask(target, ctx.pending_signal_mask);
    scheduler_state.thread_table.unlock();
    return .{
        .thread_index = target,
        .thread_generation = generation,
        .should_interrupt = should_interrupt,
        .was_blocked = was_blocked,
    };
}

pub const ClaimedSignal = struct {
    thread_index: usize,
    signo: u32,
    entry: u64,
};

pub fn configureCurrentSignalDelivery(
    entry: u64,
    inhibit_start: u64,
    inhibit_end: u64,
    inhibit_secondary_start: u64,
    inhibit_secondary_end: u64,
) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(currentThread()) orelse return false;
    if (!ctx.allocated) return false;
    ctx.signal_entry = entry;
    ctx.signal_blocked_mask = 0;
    ctx.signal_inhibit_start = inhibit_start;
    ctx.signal_inhibit_end = inhibit_end;
    ctx.signal_inhibit_secondary_start = inhibit_secondary_start;
    ctx.signal_inhibit_secondary_end = inhibit_secondary_end;
    // The thread that registers the runtime signal trampoline is the process's
    // signal owner (the main/event-loop thread).  Process-directed signals are
    // delivered here so teardown runs on the loop that can service them.
    ctx.signal_owner = true;
    return true;
}

pub fn setCurrentSignalBlockedMask(blocked_mask: u64) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(currentThread()) orelse return false;
    if (!ctx.allocated or ctx.signal_entry == 0) return false;
    ctx.signal_blocked_mask = blocked_mask;
    ctx.signal_interrupt_consumed = firstUnblockedPendingSignal(ctx) == 0;
    return true;
}

pub fn copySignalDeliveryConfig(source_thread: usize, target_thread: usize) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const source = threadContext(source_thread) orelse return false;
    const target = threadContextMutable(target_thread) orelse return false;
    if (!source.allocated or !target.allocated) return false;
    target.signal_entry = source.signal_entry;
    target.signal_blocked_mask = source.signal_blocked_mask;
    target.signal_inhibit_start = source.signal_inhibit_start;
    target.signal_inhibit_end = source.signal_inhibit_end;
    target.signal_inhibit_secondary_start = source.signal_inhibit_secondary_start;
    target.signal_inhibit_secondary_end = source.signal_inhibit_secondary_end;
    // Inheriting the trampoline does not transfer signal ownership.
    target.signal_owner = false;
    return true;
}

pub fn claimCurrentSignalForUserReturn(rip: u64) ?ClaimedSignal {
    const thread_index = currentThread();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return null;
    const signo = firstUnblockedPendingSignal(ctx);
    if (!ctx.allocated or signo == 0 or ctx.signal_entry == 0) return null;
    if (ctx.signal_inhibit_start < ctx.signal_inhibit_end and
        rip >= ctx.signal_inhibit_start and rip < ctx.signal_inhibit_end)
    {
        return null;
    }
    if (ctx.signal_inhibit_secondary_start < ctx.signal_inhibit_secondary_end and
        rip >= ctx.signal_inhibit_secondary_start and rip < ctx.signal_inhibit_secondary_end)
    {
        return null;
    }
    const claimed = ClaimedSignal{
        .thread_index = thread_index,
        .signo = signo,
        .entry = ctx.signal_entry,
    };
    ctx.pending_signal_mask &= ~signalBit(signo).?;
    ctx.signal_interrupt_consumed = firstUnblockedPendingSignal(ctx) == 0;
    setThreadHotPendingSignalMask(thread_index, ctx.pending_signal_mask);
    return claimed;
}

pub fn restoreClaimedSignal(claimed: ClaimedSignal) void {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(claimed.thread_index) orelse return;
    if (!ctx.allocated) return;
    ctx.pending_signal_mask |= signalBit(claimed.signo) orelse return;
    ctx.signal_interrupt_consumed = firstUnblockedPendingSignal(ctx) == 0;
    setThreadHotPendingSignalMask(claimed.thread_index, ctx.pending_signal_mask);
}

pub fn copyCurrentSignalFxState(out: *[fx_state_bytes]u8) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContext(currentThread()) orelse return false;
    if (!ctx.allocated) return false;
    out.* = ctx.fx_state;
    return true;
}

pub fn restoreCurrentSignalFxState(saved: *const [fx_state_bytes]u8) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(currentThread()) orelse return false;
    if (!ctx.allocated) return false;
    ctx.fx_state = saved.*;
    return true;
}

pub fn stopPrincipalThreads(principal: kernel.PrincipalId) usize {
    var stopped_count: usize = 0;
    // Serialize against the final state.current_principal publication in an
    // idle-CPU claim.  After these locks are released, a stopped thread is
    // either already visible in a CPU ownership slot or cannot be claimed.
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    lockAllCpuSchedulerStates();
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const ctx = threadContextMutable(i) orelse continue;
        if (!ctx.allocated or ctx.owner_process != principal) continue;
        if (!ctx.stopped) {
            ctx.stopped = true;
            ctx.resume_after_stop = ctx.ready;
        }
        ctx.ready = false;
        setThreadHotStopped(i, true);
        setThreadHotReady(i, false);
        stopped_count += 1;
    }
    unlockAllCpuSchedulerStates();
    i = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const ctx = threadContext(i) orelse continue;
        if (!ctx.allocated or ctx.owner_process != principal or !ctx.stopped) continue;
        verifiedBlockThread(i);
    }
    return stopped_count;
}

pub fn continuePrincipalThreads(principal: kernel.PrincipalId) usize {
    var continued_count: usize = 0;
    {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        var i: usize = 0;
        while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
            const ctx = threadContextMutable(i) orelse continue;
            if (!ctx.allocated or ctx.owner_process != principal or !ctx.stopped) continue;
            const ready = ctx.resume_after_stop;
            ctx.stopped = false;
            ctx.resume_after_stop = false;
            ctx.ready = ready;
            setThreadHotStopped(i, false);
            setThreadHotReady(i, ready);
            if (ready) verifiedWakeThread(i);
            continued_count += 1;
        }
    }
    return continued_count;
}

pub fn releasePrincipalThreads(principal: kernel.PrincipalId) usize {
    var released: usize = 0;
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const ctx = threadContext(i) orelse continue;
        if (!ctx.allocated or ctx.owner_process != principal) continue;
        if (releaseThread(i)) released += 1;
    }
    return released;
}

pub fn wakeExpiredTimers(now_tick: u64) void {
    if (!isBootstrapSchedulerCpu()) return;
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0) continue;
        if (hot.ready != 0) continue;
        if (hot.wake_tick == 0 or now_tick < hot.wake_tick) continue;
        wakeIfWaiting(i);
    }
}

pub fn blockCurrentThread(
    frame: *TrapFrame,
    wait_mailbox: bool,
    timeout_ticks: u64,
    resume_rax: u64,
    before_block: ?BeforeCurrentThreadLeaveCallback,
) bool {
    if (!isBootstrapSchedulerCpu()) {
        // AP user threads must leave the CPU for a real blocking receive.
        return parkApThreadForBlock(frame, wait_mailbox, timeout_ticks, resume_rax, before_block);
    }
    const current_thread = currentThread();
    scheduler_state.thread_table.lock();
    const ctx = threadContextMutable(current_thread) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    // Keep signal publication and the transition to blocked atomic with
    // respect to each other.  The syscall caller will remove any waiter it
    // registered and return NOT_READY/EAGAIN, allowing delivery at a safe
    // user-frame boundary instead of leaving a completion waiter behind.
    if (firstUnblockedPendingSignal(ctx) != 0 and ctx.signal_entry != 0 and
        !ctx.signal_interrupt_consumed)
    {
        ctx.signal_interrupt_consumed = true;
        scheduler_state.thread_table.unlock();
        return false;
    }

    var saved = frame.*;
    saved.rax = resume_rax;
    ctx.frame = saved;
    ctx.cr3 = currentCr3();
    ctx.fs_base = x86_platform.readFsBase();
    ctx.gs_base = x86_platform.readGsBase();
    ctx.pkru = x86_platform.readPkru();
    ctx.wait_mailbox = wait_mailbox;
    ctx.wake_tick = if (timeout_ticks == 0) 0 else lapic_tick_count + timeout_ticks;
    ctx.ready = false;
    setThreadHotCr3(current_thread, ctx.cr3);
    setThreadHotWaitState(current_thread, wait_mailbox, ctx.wake_tick, false);
    verifiedBlockThread(current_thread);
    scheduler_state.thread_table.unlock();
    if (before_block) |callback| callback.run(callback.context);
    if (switchToNextReadyOnCurrentCpu(frame, resume_rax)) return true;
    // There is no userspace context to return to while the caller is blocked.
    // Keep CPU 0 interruptible until a wakeup publishes a ready thread, then
    // load the wake-provided frame without overwriting rax with the original
    // syscall's resume value.
    loadNextReadyThreadOrIdle(frame);
    return true;
}
