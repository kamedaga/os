const std = @import("std");
const perf = @import("smp_perf.zig");
const builtin = @import("builtin");
const kernel = @import("kernel.zig");
const kernel_runtime = @import("kernel_runtime.zig");
const address_space = @import("memory/address_space.zig");
const interrupts = @import("interrupts.zig");
const smp = @import("smp.zig");
const scheduler_observer = @import("scheduler_observer.zig");
const user_copy = @import("user_copy.zig");
const x86_platform = @import("arch/x86_64/platform.zig");
const kernel_log = @import("kernel_log.zig");
const log_util = @import("log_util.zig");
pub const verified_sched = @import("verified_sched.zig");
const scheduler_runqueue = @import("scheduler_runqueue.zig");

const TrapFrame = interrupts.TrapFrame;
const process_abi = @import("kernel_abi_root").process_abi;

pub const BeforeCurrentThreadLeaveCallback = struct {
    context: *anyopaque,
    run: *const fn (*anyopaque) void,
};

const UserAddressSpace = address_space.UserAddressSpace;
const UserAddressSpaceTable = address_space.UserAddressSpaceTable;
const default_process_principal: kernel.PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
const bootstrap_cpu_slot: usize = 0;
const all_cpu_affinity_mask: u64 = if (smp.max_cpus >= 64) std.math.maxInt(u64) else (@as(u64, 1) << smp.max_cpus) - 1;

const xstate_bytes: usize = x86_platform.xstate_bytes;
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
        const start = perf.timestamp();
        const restore_interrupts = interruptsEnabled();
        disableInterruptsForSchedulerLock();
        while (true) {
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) {
                self.interrupts_were_enabled = restore_interrupts;
                perf.elapsed(.scheduler_lock_wait_cycles, start);
                perf.add(.scheduler_lock_acquires, 1);
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
    // Opaque identity for a keyed blocking operation.  Zero is reserved for
    // the existing unkeyed FD/IPC waits.  The scheduler never interprets the
    // token; it only prevents a completion left over from an older wait from
    // waking the thread in a newer, unrelated wait.
    active_wait_token: u64 = 0,
    next_wait_token: u64 = 1,
    wait_completion_claimed: bool = false,
    wait_completion_publish_pending: bool = false,
    wait_completion_rax: u64 = 0,
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
    fault_entry: u64 = 0,
    fault_stack_base: u64 = 0,
    fault_stack_size: u64 = 0,
    active_fault_frame: u64 = 0,
    wake_tick: u64 = 0,
    frame: TrapFrame = std.mem.zeroes(TrapFrame),
    x_state: [xstate_bytes]u8 align(x86_platform.xstate_alignment) = [_]u8{0} ** xstate_bytes,
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
    wait_completion_claimed: u8 = 0,
    owner_process: kernel.PrincipalId = default_process_principal,
    pending_signal_mask: u64 = 0,
    wake_tick: u64 = 0,
    cr3: u64 = 0,
};

const ProcessSignalTimer = struct {
    active: bool = false,
    principal: kernel.PrincipalId = default_process_principal,
    signo: u32 = 0,
    deadline_tick: u64 = 0,
    interval_ticks: u64 = 0,
};

pub const ProcessSignalTimerState = struct {
    signo: u32 = 0,
    remaining_ticks: u64 = 0,
    interval_ticks: u64 = 0,
};

pub const ExpiredProcessSignalTimer = struct {
    principal: kernel.PrincipalId,
    signo: u32,
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
var process_signal_timers: [initialThreadCapacity]ProcessSignalTimer =
    [_]ProcessSignalTimer{.{}} ** initialThreadCapacity;
pub export var lapic_tick_count: u64 = 0;
pub var scheduler_tick_accum: u64 = 0;
pub var scheduler_switch_count: u64 = 0;
pub var scheduler_timer_log_once: u8 = 0;
pub var initial_x_state: [xstate_bytes]u8 align(x86_platform.xstate_alignment) = [_]u8{0} ** xstate_bytes;
// A reader outside KernelState must not pair an old gate with a target that
// the lifecycle owner has already replaced. Publish state and target together.
var principal_lifecycle_gate: usize = 0;
var principal_lifecycle_pending_action: u8 = 0;
var principal_lifecycle_pending_code: u32 = 0;
const principal_self_exit_reservation_word_count = (kernel.max_process_slots + 63) / 64;
var principal_self_exit_reservations: [principal_self_exit_reservation_word_count]u64 =
    [_]u64{0} ** principal_self_exit_reservation_word_count;

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
    signal_timers: []ProcessSignalTimer = process_signal_timers[0..],
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
    cpu_count: usize = 1,
    thread_table: ThreadTableState = .{},
    verified: VerifiedCoreState = .{},
    ap_user_dispatch_enabled: u8 = 0,
    cpus: [smp.max_cpus]CpuSchedulerState = buildInitialCpuSchedulerStates(),
};

var scheduler_state: SchedulerState = .{};

fn verifiedCoreCpuCount() usize {
    return @atomicLoad(usize, &scheduler_state.cpu_count, .acquire);
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

fn verifiedWakeThreadGenerationPreferred(
    thread_index: usize,
    generation: u32,
    preferred_cpu: ?usize,
) bool {
    const start = perf.timestamp();
    perf.add(.wake_scan_calls, 1);
    defer perf.elapsed(.wake_scan_cycles, start);
    if (!verifiedCoreReady()) return false;
    const ctx = threadContextMutable(thread_index) orelse return false;
    const node = ctx.scheduler_entity orelse return false;
    const last_cpu = node.cpu_slot;
    const last_state = schedulerStateForSlot(last_cpu) orelse return false;

    var target_cpu = last_cpu;
    var preferred_selected = false;
    if (preferred_cpu) |cpu| {
        if (cpu < verifiedCoreCpuCount() and schedulerCpuEnabled(cpu) and threadAllowsCpu(ctx, cpu)) {
            target_cpu = cpu;
            preferred_selected = true;
        }
    }
    // A valid synchronous IPC handoff selects this CPU regardless of load.
    // Do not inspect every runqueue only to discard that choice afterward.
    var target_count: usize = std.math.maxInt(usize);
    var target_idle = false;
    var last_count: usize = std.math.maxInt(usize);
    var last_idle = false;
    var cpu_id: usize = 0;
    while (!preferred_selected and cpu_id < verifiedCoreCpuCount()) : (cpu_id += 1) {
        const state = schedulerStateForSlot(cpu_id) orelse continue;
        if (!schedulerCpuEnabled(cpu_id) or !threadAllowsCpu(ctx, cpu_id)) continue;
        state.lock.lock();
        const is_idle = state.is_idle;
        state.lock.unlock();
        state.runqueue_lock.lock();
        const count = state.runqueue.count;
        state.runqueue_lock.unlock();
        if (cpu_id == last_cpu) {
            last_count = count;
            last_idle = is_idle;
        }
        if ((is_idle and !target_idle) or
            (is_idle == target_idle and count < target_count))
        {
            target_count = count;
            target_cpu = cpu_id;
            target_idle = is_idle;
        }
    }
    if (!preferred_selected and target_count == std.math.maxInt(usize)) return false;
    const last_allowed = schedulerCpuEnabled(last_cpu) and threadAllowsCpu(ctx, last_cpu);
    if (!preferred_selected and last_allowed and (last_idle or !target_idle) and
        last_count <= target_count +| 1)
    {
        target_cpu = last_cpu;
    }

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
            preferred_selected = false;
        }
        // Scalar ownership becomes blocked before the old CPU completes its
        // kernel-to-idle transition.  Do not publish this generation on a
        // second CPU while the old CPU still executes its block tail.
        if (!last_state.is_idle and last_state.current_thread == thread_index) {
            target_cpu = last_cpu;
            preferred_selected = false;
        }
        // Revalidate the locality threshold under the pair of runqueue locks.
        // A last CPU that is still within one queued entity always wins.
        if (!preferred_selected and target_cpu != last_cpu and last_allowed and
            (last_state.is_idle or !target_state.is_idle) and
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

fn verifiedWakeThreadGeneration(thread_index: usize, generation: u32) bool {
    return verifiedWakeThreadGenerationPreferred(thread_index, generation, null);
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
        false,
        cpu_id,
        thread_index,
        generation,
        current_thread_index,
        current_generation,
    );
}

fn verifiedHandoffToThreadOnCpu(
    comptime only_ready: bool,
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
    // Generic futex wake must not bypass an unrelated runnable competitor.
    // Recheck at the ownership transition, not at an earlier wake hint.
    if (only_ready and state.runqueue.count != 1) {
        state.runqueue_lock.unlock();
        return false;
    }
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
    // Directed IPC migration must obey the same affinity as ordinary wakes;
    // a useful receiver hint is not permission to execute on another CPU.
    if (!threadAllowsCpu(ctx, dst_cpu)) return false;
    const src = schedulerStateForSlot(src_cpu) orelse return false;
    const dst = schedulerStateForSlot(dst_cpu) orelse return false;

    // Preemption/handoff publishes a runnable entity before switchToGeneration
    // saves the outgoing frame and installs the next CPU identity. A remote
    // IPC handoff must not consume that entity's previous saved frame in this
    // interval. Stealing already checks the executing identity; explicit
    // migration needs the same exclusion, held through the queue transfer.
    const first_state = if (src_cpu < dst_cpu) src else dst;
    const second_state = if (src_cpu < dst_cpu) dst else src;
    first_state.lock.lock();
    second_state.lock.lock();
    defer {
        second_state.lock.unlock();
        first_state.lock.unlock();
    }
    if (!src.is_idle and src.current_thread == thread_index) return false;
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
        .x_state_addr = @intFromPtr(&ctx.x_state),
        .pkru = ctx.pkru,
        .frame = ctx.frame,
    };
    return true;
}

test "preferred wake preserves fallback affinity and outgoing block tail" {
    const cases = [_]struct { preferred: ?usize, enabled: bool = true, affinity: u64 = 3, tail: bool = false, expected: usize }{
        .{ .preferred = 1, .expected = 1 },
        .{ .preferred = null, .expected = 0 },
        .{ .preferred = 64, .expected = 0 },
        .{ .preferred = 1, .enabled = false, .expected = 0 },
        .{ .preferred = 1, .affinity = 1, .expected = 0 },
        .{ .preferred = 1, .tail = true, .expected = 0 },
    };
    for (cases) |case| {
        initializeStaticStorage();
        defer initializeStaticStorage();
        const tid = 4;
        const node = &initial_scheduler_entities[tid];
        const ctx = &initial_thread_contexts[tid];
        ctx.* = .{ .id = tid, .allocated = true, .scheduler_entity = node, .cpu_affinity_mask = case.affinity };
        defer ctx.* = .{ .id = tid, .scheduler_entity = node };
        scheduler_state.cpu_count = 2;
        scheduler_state.cpus[0].is_idle = true;
        scheduler_state.cpus[0].current_thread = idleThreadMarker;
        scheduler_state.cpus[1].enabled = case.enabled;
        try std.testing.expect(verifiedAddThread(tid, 1, false));
        if (case.tail) {
            scheduler_state.cpus[0].is_idle = false;
            scheduler_state.cpus[0].current_thread = tid;
        }
        try std.testing.expect(verifiedWakeThreadGenerationPreferred(tid, 1, case.preferred));
        try std.testing.expectEqual(case.expected, ctx.cpu_slot);
        try std.testing.expectEqual(.runnable, node.ownership);
        try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[case.expected].runqueue.count);
        try std.testing.expect(!verifiedWakeThreadGenerationPreferred(tid, 1, case.preferred));
    }
}

test "migration waits for the outgoing CPU context to be saved and released" {
    // Exercise both producers of the runnable-before-context-save interval.
    for (0..8) |scenario| {
        const handoff = scenario % 2 != 0;
        const destination = ([_]usize{ 1, 5, 7, 63 })[scenario / 2];
        initializeStaticStorage();
        defer initializeStaticStorage();
        const tid = 4;
        const generation = 1;
        const node = &initial_scheduler_entities[tid];
        const ctx = &initial_thread_contexts[tid];
        ctx.* = .{ .id = tid, .allocated = true, .ready = true, .scheduler_entity = node };
        defer ctx.* = .{ .id = tid, .scheduler_entity = node };
        try std.testing.expectEqual(.ok, verified_sched.pacha_eevdf_entity_init(
            5,
            generation,
            1024,
            4_000_000,
            0,
            &node.entity,
        ));
        node.ownership = .runnable;
        try std.testing.expect(scheduler_state.cpus[0].runqueue.insert(node));
        const picked = verifiedPickThreadForCpu(0) orelse return error.TestUnexpectedResult;
        try std.testing.expectEqual(@as(usize, tid), picked.thread_index);
        scheduler_state.cpus[0].current_thread = tid;
        scheduler_state.cpus[0].is_idle = false;
        scheduler_state.cpus[destination].enabled = true;
        scheduler_state.cpu_count = destination + 1;

        const target = &initial_scheduler_entities[tid + 1];
        try std.testing.expectEqual(.ok, verified_sched.pacha_eevdf_entity_init(
            6,
            generation,
            1024,
            4_000_000,
            0,
            &target.entity,
        ));
        target.ownership = .runnable;
        try std.testing.expect(scheduler_state.cpus[0].runqueue.insert(target));
        if (handoff) {
            try std.testing.expect(verifiedHandoffToThreadOnCpu(false, 0, tid + 1, generation, tid, generation));
        } else {
            try std.testing.expectEqual(.preempt, verifiedExpireSlice(0, tid, generation, 4_000_000));
        }

        // Handoff/preemption has enqueued the outgoing entity, but its CPU
        // has not saved the live frame yet. The old frame must not be runnable
        // on a second CPU during this interval.
        ctx.frame.rip = 0x1111;
        try std.testing.expect(!migrateRunnableThreadGeneration(tid, generation, destination));
        try std.testing.expectEqual(@as(usize, 0), ctx.cpu_slot);
        try std.testing.expectEqual(@as(usize, if (handoff) 1 else 2), scheduler_state.cpus[0].runqueue.count);
        try std.testing.expectEqual(@as(usize, 0), scheduler_state.cpus[destination].runqueue.count);

        ctx.frame.rip = 0x2222;
        scheduler_state.cpus[0].current_thread = idleThreadMarker;
        scheduler_state.cpus[0].is_idle = true;
        try std.testing.expect(migrateRunnableThreadGeneration(tid, generation, destination));
        try std.testing.expectEqual(destination, ctx.cpu_slot);
        try std.testing.expectEqual(@as(u64, 0x2222), ctx.frame.rip);

        // A handoff to another live thread also releases the outgoing identity;
        // migration must not require the entire source CPU to become idle.
        scheduler_state.cpus[destination].current_thread = tid + 1;
        scheduler_state.cpus[destination].is_idle = false;
        try std.testing.expect(migrateRunnableThreadGeneration(tid, generation, 0));
        try std.testing.expectEqual(@as(usize, 0), ctx.cpu_slot);
    }
}

/// Pull one already-runnable entity from the busiest runqueue into an empty
/// idle CPU.  The operation is allocation-free and publishes cpu_slot only
/// while the thread table and both runqueues are locked.
fn stealRunnableForIdleCpu(dst_cpu: usize) bool {
    const start = perf.timestamp();
    perf.add(.steal_attempts, 1);
    defer perf.elapsed(.steal_cycles, start);
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
    if (!apUserDispatchReady()) return false;
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
    if (activateGeneration(picked.thread_index, picked.generation)) {
        return true;
    }
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
    if (loadContextIntoFrameGeneration(picked.thread_index, picked.generation, frame)) {
        return true;
    }
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
    if (loadNextReadyThread(frame)) return;
    // No saved context is referenced beyond this point.  In particular the
    // BSP must drop ownership of an exited/blocked principal before entering
    // its interruptible wait, otherwise remote lifecycle quiescence waits on
    // a CPU which is already logically idle.
    noteCpuIdleTick(currentCpu());
    while (!loadNextReadyThread(frame)) {
        asm volatile ("sti; hlt; cli" ::: .{ .memory = true });
    }
}

fn switchToNextReadyOnCurrentCpu(frame: *TrapFrame, saved_rax: ?u64) bool {
    const cpu_id = currentCpu();
    const picked = verifiedPickThreadForCpu(cpu_id) orelse return false;
    if (switchToGeneration(picked.thread_index, picked.generation, frame, saved_rax)) {
        return true;
    }
    verifiedRollbackPickedCpu(cpu_id, picked);
    return false;
}

fn nextThreadGeneration(current: u32) u32 {
    const next = current +% 1;
    return if (next == 0) 1 else next;
}

pub fn initializeStaticStorage() void {
    @atomicStore(usize, &scheduler_state.cpu_count, 1, .release);
    @memset(principal_self_exit_reservations[0..], 0);
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
    wait_token: u64,
    timeout_ticks: u64,
    resume_rax: u64,
    before_block: ?BeforeCurrentThreadLeaveCallback,
) bool {
    const current_thread = currentThread();
    scheduler_state.thread_table.lock();
    if (threadContextMutable(current_thread)) |ctx| {
        if (wait_token != 0) {
            if (!wait_mailbox or ctx.active_wait_token != wait_token) {
                scheduler_state.thread_table.unlock();
                return false;
            }
            if (!ctx.wait_mailbox) {
                // The keyed wake raced with the transition into the blocked
                // state.  It stored the completion in the reserved token;
                // consume it without ever publishing this thread as blocked.
                frame.rax = ctx.frame.rax;
                ctx.active_wait_token = 0;
                ctx.wake_tick = 0;
                ctx.wait_completion_publish_pending = false;
                ctx.wait_completion_rax = 0;
                scheduler_state.thread_table.unlock();
                return true;
            }
        } else if (ctx.active_wait_token != 0) {
            scheduler_state.thread_table.unlock();
            return false;
        }
        // A signal published while this thread was runnable must interrupt the
        // next blocking syscall.  Refuse the block while holding the same lock
        // used by deliverSignal(), so no waiter can become stale between the
        // pending-signal check and ready=false.
        if (firstUnblockedPendingSignal(ctx) != 0 and ctx.signal_entry != 0 and
            !ctx.signal_interrupt_consumed)
        {
            ctx.signal_interrupt_consumed = true;
            if (wait_token != 0) {
                ctx.wait_mailbox = false;
                ctx.active_wait_token = 0;
            }
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
        ctx.active_wait_token = wait_token;
        ctx.wait_completion_claimed = false;
        ctx.wait_completion_publish_pending = false;
        ctx.wait_completion_rax = 0;
        ctx.wake_tick = if (timeout_ticks == 0) 0 else lapic_tick_count +| timeout_ticks;
        ctx.ready = false;
        setThreadHotCr3(current_thread, ctx.cr3);
        setThreadHotWaitState(current_thread, wait_mailbox, ctx.wake_tick, false);
        setThreadHotCompletionClaimed(current_thread, false);
        verifiedBlockThread(current_thread);
        // Publish the local CPU as idle before releasing the thread table.
        // A remote completion takes that same table lock before it selects a
        // wake CPU.  Publishing it afterwards leaves a window where the
        // completion sees this thread as still executing on its old CPU,
        // rejects its preferred handoff CPU, and sends an IPI here.  That IPI
        // may interrupt the remaining ring-0 block tail; the wake handler
        // deliberately returns from ring-0 work, after which this path would
        // enter HLT with the only edge already consumed.
        //
        // Once this state is visible, either a preferred IPC handoff consumes
        // the runnable receiver on the sender CPU, or an ordinary wake leaves
        // a runnable entity visible to apIdleLoop before it executes HLT.
        if (schedulerStateForSlot(currentCpu())) |state| {
            state.lock.lock();
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
            state.lock.unlock();
        }
    }
    scheduler_state.thread_table.unlock();
    if (before_block) |callback| callback.run(callback.context);
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
    const paddr = free_list.popContiguousBelow(page_count, @import("arch/x86_64/physical_layout.zig").identity_limit) catch return null;
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
    const new_signal_timers = allocKernelSlice(ProcessSignalTimer, free_list, capacity) orelse return false;
    const new_entities = allocKernelSlice(scheduler_runqueue.Node, free_list, capacity - old_capacity) orelse return false;

    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    if (scheduler_state.thread_table.contexts.len != old_capacity) {
        return required <= scheduler_state.thread_table.contexts.len;
    }
    @memcpy(new_contexts[0..old_capacity], scheduler_state.thread_table.contexts);
    @memcpy(new_hot_threads[0..old_capacity], scheduler_state.thread_table.hot_states);
    @memcpy(new_signal_timers[0..old_capacity], scheduler_state.thread_table.signal_timers);
    var i = old_capacity;
    while (i < capacity) : (i += 1) {
        const entity = &new_entities[i - old_capacity];
        entity.reset(i, 1);
        new_contexts[i] = .{ .id = @intCast(i), .scheduler_entity = entity };
        new_hot_threads[i] = .{};
        new_signal_timers[i] = .{};
    }

    scheduler_state.thread_table.contexts = new_contexts;
    scheduler_state.thread_table.hot_states = new_hot_threads;
    scheduler_state.thread_table.signal_timers = new_signal_timers;
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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(process_signal_timers), &process_signal_timers));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_scheduler_entities), &initial_scheduler_entities));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_state), &scheduler_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(lapic_tick_count), &lapic_tick_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_tick_accum), &scheduler_tick_accum));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_switch_count), &scheduler_switch_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_x_state), &initial_x_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_lifecycle_gate), &principal_lifecycle_gate));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_lifecycle_pending_action), &principal_lifecycle_pending_action));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_lifecycle_pending_code), &principal_lifecycle_pending_code));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_self_exit_reservations), &principal_self_exit_reservations));
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
fn principalSelfExitReservation(target: kernel.PrincipalId) ?struct { word: *u64, mask: u64 } {
    const process_index = kernel.processIndexFromPrincipal(target) orelse return null;
    const bit: u6 = @intCast(process_index & 63);
    return .{
        .word = &principal_self_exit_reservations[process_index / 64],
        .mask = @as(u64, 1) << bit,
    };
}

fn principalSelfExitIsReserved(target: kernel.PrincipalId) bool {
    const reservation = principalSelfExitReservation(target) orelse return false;
    return (@atomicLoad(u64, reservation.word, .acquire) & reservation.mask) != 0;
}

/// Reserve this target for its own PROCESS_EXIT before the caller temporarily
/// drops the kernel-state lock.  This prevents a remote stop/kill from owning
/// the lifecycle gate for the exiting target and waiting on that same ring-0
/// caller.  The reservation is internal scheduler state, not an ABI surface.
pub fn reservePrincipalSelfExit(target: kernel.PrincipalId) bool {
    const reservation = principalSelfExitReservation(target) orelse return false;
    const previous = @atomicRmw(u64, reservation.word, .Or, reservation.mask, .acq_rel);
    return (previous & reservation.mask) == 0;
}

fn clearPrincipalSelfExitReservation(target_raw: usize) void {
    if (target_raw >= kernel.max_process_slots) return;
    const reservation = principalSelfExitReservation(@enumFromInt(@as(kernel.PrincipalRaw, @intCast(target_raw)))) orelse return;
    _ = @atomicRmw(u64, reservation.word, .And, ~reservation.mask, .acq_rel);
}

fn tryBeginPrincipalLifecycleGate(target: kernel.PrincipalId) bool {
    if (@cmpxchgStrong(usize, &principal_lifecycle_gate, 0, 1, .acquire, .monotonic) != null) {
        return false;
    }
    principal_lifecycle_pending_code = 0;
    @atomicStore(u8, &principal_lifecycle_pending_action, @intFromEnum(PrincipalLifecycleAction.none), .monotonic);
    @atomicStore(usize, &principal_lifecycle_gate, lifecycleWord(target), .release);
    return true;
}

pub fn tryBeginPrincipalLifecycle(target: kernel.PrincipalId) bool {
    if (principalSelfExitIsReserved(target)) return false;
    return tryBeginPrincipalLifecycleGate(target);
}

pub fn tryBeginReservedPrincipalLifecycle(target: kernel.PrincipalId) bool {
    if (!principalSelfExitIsReserved(target)) return false;
    return tryBeginPrincipalLifecycleGate(target);
}

pub fn endPrincipalLifecycle() void {
    @atomicStore(u8, &principal_lifecycle_pending_action, @intFromEnum(PrincipalLifecycleAction.none), .monotonic);
    principal_lifecycle_pending_code = 0;
    const word = @atomicLoad(usize, &principal_lifecycle_gate, .acquire);
    if ((word & 3) == 2) clearPrincipalSelfExitReservation(word >> 2);
    @atomicStore(usize, &principal_lifecycle_gate, 0, .release);
}

pub fn principalLifecycleTargets(principal: kernel.PrincipalId) bool {
    return @atomicLoad(usize, &principal_lifecycle_gate, .acquire) == lifecycleWord(principal);
}

fn lifecycleWord(principal: kernel.PrincipalId) usize {
    comptime std.debug.assert(@bitSizeOf(kernel.PrincipalRaw) + 2 <= @bitSizeOf(usize));
    return (@as(usize, @intFromEnum(principal)) << 2) | 2;
}

pub const ClockReadAdmission = enum { fast, blocked, slow };

fn clockReadAdmissionLocked(
    state: *const CpuSchedulerState,
    ctx: *const ThreadContext,
    hot: *const ThreadHotState,
    node: *const scheduler_runqueue.Node,
    cpu: usize,
    lifecycle: usize,
    self_exit: bool,
) ClockReadAdmission {
    if (lifecycle == 1) return .slow;
    if (!state.enabled or state.is_idle or state.current_entity != node or
        state.current_thread != ctx.id or !ctx.allocated or !ctx.ready or
        ctx.scheduler_entity != node or ctx.generation == 0 or
        node.thread_index != ctx.id or node.generation != ctx.generation or
        node.ownership != .running or node.cpu_slot != cpu or ctx.cpu_slot != cpu or
        hot.allocated == 0 or hot.ready == 0 or
        state.current_principal != ctx.owner_process or hot.owner_process != ctx.owner_process or
        ctx.cr3 == 0 or state.current_cr3 != ctx.cr3 or hot.cr3 != ctx.cr3 or
        (hot.stopped != 0) != ctx.stopped) return .slow;
    if (ctx.stopped or self_exit or lifecycle == lifecycleWord(ctx.owner_process)) return .blocked;
    return .fast;
}

pub fn currentClockReadAdmission() ClockReadAdmission {
    const cpu = currentCpu();
    const state = schedulerStateForSlot(cpu) orelse return .slow;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    state.lock.lock();
    defer state.lock.unlock();
    state.runqueue_lock.lock();
    defer state.runqueue_lock.unlock();
    const ctx = threadContext(state.current_thread) orelse return .slow;
    const hot = getThreadHotStateConst(state.current_thread) orelse return .slow;
    const node = ctx.scheduler_entity orelse return .slow;
    // Copy only the decision out of the lock interval. Running CPU ownership
    // remains intact until the normal ring-3 quiescence/return boundary.
    return clockReadAdmissionLocked(state, ctx, hot, node, cpu, @atomicLoad(usize, &principal_lifecycle_gate, .acquire), principalSelfExitIsReserved(ctx.owner_process));
}

test "clock read admission validates lifetime and atomic lifecycle snapshot" {
    const owner: kernel.PrincipalId = @enumFromInt(7);
    for ([_]kernel.PrincipalRaw{ 0, 7, std.math.maxInt(kernel.PrincipalRaw) }) |raw| {
        const word = lifecycleWord(@enumFromInt(raw));
        try std.testing.expectEqual(@as(usize, 2), word & 3);
        try std.testing.expectEqual(@as(usize, raw), word >> 2);
    }
    for (0..27) |scenario| {
        var node = scheduler_runqueue.Node{ .thread_index = 4, .generation = 9, .cpu_slot = 1, .ownership = .running };
        var ctx = ThreadContext{ .id = 4, .generation = 9, .allocated = true, .ready = true, .owner_process = owner, .cpu_slot = 1, .cr3 = 0x9000, .scheduler_entity = &node };
        var hot = ThreadHotState{ .allocated = 1, .ready = 1, .owner_process = owner, .cr3 = 0x9000 };
        var state = CpuSchedulerState{ .enabled = true, .is_idle = false, .current_thread = 4, .current_principal = owner, .current_cr3 = 0x9000, .current_entity = &node };
        var word: usize = 0;
        var self_exit = false;
        const expected: ClockReadAdmission = switch (scenario) {
            0, 21 => .fast,
            2, 3, 20 => .blocked,
            else => .slow,
        };
        switch (scenario) {
            0 => {},
            1 => word = 1,
            2 => word = lifecycleWord(owner),
            3 => self_exit = true,
            4 => state.enabled = false,
            5 => state.is_idle = true,
            6 => state.current_entity = null,
            7 => state.current_thread = 5,
            8 => ctx.allocated = false,
            9 => ctx.ready = false,
            10 => ctx.scheduler_entity = null,
            11 => ctx.generation = 0,
            12 => node.generation = 8,
            13 => node.thread_index = 5,
            14 => node.ownership = .runnable,
            15 => node.cpu_slot = 0,
            16 => ctx.cpu_slot = 0,
            17 => hot.allocated = 0,
            18 => hot.owner_process = @enumFromInt(8),
            19 => hot.cr3 = 0x8000,
            20 => {
                ctx.stopped = true;
                hot.stopped = 1;
            },
            21 => word = lifecycleWord(@enumFromInt(8)),
            22 => state.current_principal = @enumFromInt(8),
            23 => state.current_cr3 = 0x8000,
            24 => ctx.cr3 = 0,
            25 => hot.ready = 0,
            26 => hot.stopped = 1,
            else => unreachable,
        }
        try std.testing.expectEqual(expected, clockReadAdmissionLocked(&state, &ctx, &hot, &node, 1, word, self_exit));
    }
}

test "clock read admission pins released slot through CPU ownership before reuse" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    try initializeFusedWakeTest(0);
    try std.testing.expectEqual(ClockReadAdmission.fast, currentClockReadAdmission());
    const old_generation = initial_thread_contexts[4].generation;
    try std.testing.expect(releaseThread(4));
    try std.testing.expectEqual(nextThreadGeneration(old_generation), initial_thread_contexts[4].generation);
    try std.testing.expect(!initial_thread_contexts[4].allocated);
    try std.testing.expectEqual(ClockReadAdmission.slow, currentClockReadAdmission());
    try std.testing.expect(threadSlotReferencedByCpu(4));
    noteCpuIdleTick(0);
    try std.testing.expect(!threadSlotReferencedByCpu(4));
    try std.testing.expectEqual(ClockReadAdmission.slow, currentClockReadAdmission());
    const ctx = &initial_thread_contexts[4];
    ctx.allocated = true;
    ctx.ready = true;
    ctx.cpu_slot = 0;
    ctx.cr3 = 0x2000;
    syncHotStateFromContext(4);
    try std.testing.expect(verifiedAddThread(4, ctx.generation, true));
    // A new context alone does not grant CPU ownership. Publish it through
    // the ordinary activation path only after the old identity is idle.
    try std.testing.expectEqual(ClockReadAdmission.slow, currentClockReadAdmission());
    try std.testing.expectEqual(@as(usize, 4), verifiedPickThreadForCpu(0).?.thread_index);
    try std.testing.expect(activateGenerationLockedWithRegisters(ContextTransferTestRegisters, false, 4, ctx.generation));
    try std.testing.expectEqual(ClockReadAdmission.fast, currentClockReadAdmission());
}

test "clock read admission lifecycle publication clears self exit before reuse" {
    const owner: kernel.PrincipalId = @enumFromInt(7);
    const other: kernel.PrincipalId = @enumFromInt(8);
    const saved_gate = principal_lifecycle_gate;
    const saved_pending = principal_lifecycle_pending_action;
    const saved_code = principal_lifecycle_pending_code;
    const saved_reservations = principal_self_exit_reservations;
    defer {
        principal_lifecycle_gate = saved_gate;
        principal_lifecycle_pending_action = saved_pending;
        principal_lifecycle_pending_code = saved_code;
        principal_self_exit_reservations = saved_reservations;
    }
    principal_lifecycle_gate = 0;
    @memset(principal_self_exit_reservations[0..], 0);
    principal_lifecycle_pending_code = 99;
    try std.testing.expect(tryBeginPrincipalLifecycle(owner));
    try std.testing.expect(principalLifecycleTargets(owner));
    try std.testing.expect(!principalLifecycleTargets(other));
    try std.testing.expect(!tryBeginPrincipalLifecycle(other));
    try std.testing.expectEqual(@as(u32, 0), principal_lifecycle_pending_code);
    endPrincipalLifecycle();
    try std.testing.expect(!principalLifecycleTargets(owner));
    try std.testing.expect(reservePrincipalSelfExit(owner));
    try std.testing.expect(!reservePrincipalSelfExit(owner));
    try std.testing.expect(!tryBeginPrincipalLifecycle(owner));
    try std.testing.expect(tryBeginReservedPrincipalLifecycle(owner));
    try std.testing.expect(principalSelfExitIsReserved(owner));
    endPrincipalLifecycle();
    try std.testing.expect(!principalSelfExitIsReserved(owner));
    try std.testing.expect(!tryBeginReservedPrincipalLifecycle(owner));
    try std.testing.expect(tryBeginPrincipalLifecycle(owner));
    endPrincipalLifecycle();
    try std.testing.expect(tryBeginPrincipalLifecycle(other));
    endPrincipalLifecycle();
}

pub fn principalLifecycleBlocksAdmission(principal: kernel.PrincipalId) bool {
    return principalLifecycleTargets(principal) or principalSelfExitIsReserved(principal);
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

pub fn principalSlotReusable(principal: kernel.PrincipalId) bool {
    return cpuMaskRunningPrincipal(principal) == 0;
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

fn lockAllCpuRunqueues() void {
    var cpu_slot: usize = 0;
    while (cpu_slot < scheduler_state.cpus.len) : (cpu_slot += 1) {
        scheduler_state.cpus[cpu_slot].runqueue_lock.lock();
    }
}

fn unlockAllCpuRunqueues() void {
    var remaining = scheduler_state.cpus.len;
    while (remaining != 0) {
        remaining -= 1;
        scheduler_state.cpus[remaining].runqueue_lock.unlock();
    }
}

/// An unallocated context can still be the identity of the kernel path which
/// is retiring it.  Keep that slot unavailable until the owning CPU has
/// actually published idle (AP) or selected its next context (BSP).
fn threadSlotReferencedByCpu(thread_index: usize) bool {
    var referenced = false;
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    for (&scheduler_state.cpus) |*state| {
        if (state.enabled and !state.is_idle and state.current_thread == thread_index) {
            referenced = true;
            break;
        }
    }
    return referenced;
}

fn threadSlotReferencedByRemoteCpu(thread_index: usize, local_cpu: usize) bool {
    var referenced = false;
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    for (&scheduler_state.cpus, 0..) |*state, cpu_slot| {
        if (cpu_slot == local_cpu) continue;
        if (state.enabled and !state.is_idle and state.current_thread == thread_index) {
            referenced = true;
            break;
        }
    }
    return referenced;
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
    @atomicStore(usize, &scheduler_state.cpu_count, count, .release);
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
    return verifiedCoreReady() and
        @atomicLoad(u8, &scheduler_state.ap_user_dispatch_enabled, .acquire) != 0;
}

pub fn enableApUserDispatch() void {
    @atomicStore(u8, &scheduler_state.ap_user_dispatch_enabled, 1, .release);
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

fn getUserSpace(user_spaces: *UserAddressSpaceTable, principal: kernel.PrincipalId) ?*UserAddressSpace {
    const idx = kernel.processIndexFromPrincipal(principal) orelse return null;
    return user_spaces.get(idx);
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
        .wait_completion_claimed = boolByte(ctx.wait_completion_publish_pending),
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

fn setThreadHotCompletionClaimed(thread_index: usize, claimed: bool) void {
    if (getThreadHotState(thread_index)) |hot|
        hot.wait_completion_claimed = boolByte(claimed);
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
    user_spaces: *UserAddressSpaceTable,
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
    ctx.active_wait_token = 0;
    ctx.next_wait_token = 1;
    ctx.wait_completion_claimed = false;
    ctx.wait_completion_publish_pending = false;
    ctx.wait_completion_rax = 0;
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
    ctx.fault_entry = 0;
    ctx.fault_stack_base = 0;
    ctx.fault_stack_size = 0;
    ctx.active_fault_frame = 0;
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
    ctx.x_state = initial_x_state;
    syncHotStateFromContext(thread_index);
    return true;
}

pub fn initializeThreadContext(
    thread_index: usize,
    owner_process: kernel.PrincipalId,
    user_spaces: *UserAddressSpaceTable,
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

pub fn reserveCurrentWaitToken(expected_generation: u32) ?u64 {
    const thread_index = currentThread();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return null;
    if (!ctx.allocated or ctx.generation != expected_generation or
        !ctx.ready or ctx.active_wait_token != 0 or ctx.wait_completion_claimed)
    {
        return null;
    }
    var token = ctx.next_wait_token;
    if (token == 0) token = 1;
    ctx.next_wait_token = token +% 1;
    if (ctx.next_wait_token == 0) ctx.next_wait_token = 1;
    // Publish the reservation while holding the same lock used by keyed
    // wake.  A wake arriving before blockCurrentThread() can then complete
    // this token instead of being lost in the record-to-park window.
    ctx.wait_mailbox = true;
    ctx.active_wait_token = token;
    ctx.wait_completion_publish_pending = false;
    ctx.wait_completion_rax = 0;
    setThreadHotCompletionClaimed(thread_index, false);
    return token;
}

pub fn cancelCurrentWaitToken(expected_generation: u32, token: u64) void {
    if (token == 0) return;
    const thread_index = currentThread();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return;
    if (!ctx.allocated or ctx.generation != expected_generation or
        !ctx.ready or ctx.active_wait_token != token)
    {
        return;
    }
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    ctx.wake_tick = 0;
    ctx.wait_completion_claimed = false;
    ctx.wait_completion_publish_pending = false;
    ctx.wait_completion_rax = 0;
    setThreadHotCompletionClaimed(thread_index, false);
}

pub fn waitTokenIsCurrent(
    owner: kernel.PrincipalId,
    thread_index: usize,
    generation: u32,
    token: u64,
) bool {
    if (token == 0) return false;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContext(thread_index) orelse return false;
    return ctx.allocated and ctx.owner_process == owner and
        ctx.generation == generation and ctx.active_wait_token == token;
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

pub fn allocateReadyThread(owner_process: kernel.PrincipalId, user_spaces: *UserAddressSpaceTable, initial_frame: TrapFrame, free_list: *kernel.FreePageList) ?usize {
    return allocateThreadWithReadyState(owner_process, user_spaces, initial_frame, true, free_list);
}

pub fn allocateSuspendedThread(owner_process: kernel.PrincipalId, user_spaces: *UserAddressSpaceTable, initial_frame: TrapFrame, free_list: *kernel.FreePageList) ?usize {
    return allocateThreadWithReadyState(owner_process, user_spaces, initial_frame, false, free_list);
}

fn allocateThreadWithReadyState(owner_process: kernel.PrincipalId, user_spaces: *UserAddressSpaceTable, initial_frame: TrapFrame, initial_ready: bool, free_list: *kernel.FreePageList) ?usize {
    while (true) {
        var i: usize = 0;
        while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
            const ctx = threadContext(i) orelse continue;
            if (ctx.allocated) continue;
            if (threadSlotReferencedByCpu(i)) continue;
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
    // Never destroy a context which another CPU can still use for interrupt,
    // syscall-return, or user-entry state.  Lifecycle teardown first stops
    // and quiesces remote owners; suspended rollback has no CPU reference.
    if (threadSlotReferencedByRemoteCpu(thread_index, currentCpu())) return false;
    const generation = ctx.generation;
    if (!verifiedExitThreadGeneration(thread_index, generation)) return false;
    const next_generation = nextThreadGeneration(ctx.generation);
    const entity = ctx.scheduler_entity orelse return false;
    // Do not publish CPU-idle here.  The caller is still executing on behalf
    // of this generation until the AP idle path or a BSP context switch says
    // otherwise.  Allocation checks CPU references before reusing the slot.
    entity.reset(thread_index, next_generation);
    ctx.* = .{ .id = @intCast(thread_index), .generation = next_generation, .scheduler_entity = entity };
    syncHotStateFromContext(thread_index);
    return true;
}

fn activateGenerationLocked(thread_index: usize, generation: u32) bool {
    return activateGenerationLockedWithRegisters(x86_platform, true, thread_index, generation);
}

fn activateGenerationLockedWithRegisters(comptime Registers: type, comptime install_bases: bool, thread_index: usize, generation: u32) bool {
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
    if (install_bases and !applyThreadBasesWithRegisters(Registers, thread_index)) return false;
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
    return applyThreadBasesWithRegisters(x86_platform, thread_index);
}

fn applyThreadBasesWithRegisters(comptime Registers: type, thread_index: usize) bool {
    const ctx = threadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    Registers.writeFsBase(ctx.fs_base);
    Registers.writeGsBase(ctx.gs_base);
    Registers.writePkru(ctx.pkru);
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
        ctx.active_wait_token != 0 or
        ctx.wait_completion_claimed or
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

// The caller holds thread_table and all CPU ownership locks. A stopped
// syscall wait can still receive a completion, so it is not an editable frame.
fn controlledThreadAvailableLocked(ctx: *const ThreadContext, thread_index: usize, generation: u32, owner: kernel.PrincipalId) bool {
    if (!ctx.allocated or ctx.generation != generation or ctx.owner_process != owner or
        ctx.ready or ctx.wait_mailbox or ctx.active_wait_token != 0 or
        ctx.wait_completion_claimed or ctx.wait_completion_publish_pending or
        ctx.wake_tick != 0 or ctx.active_fault_frame != 0) return false;
    for (scheduler_state.cpus[0..scheduler_state.cpu_count]) |*cpu| {
        if (!cpu.is_idle and cpu.current_thread == thread_index) return false;
    }
    return true;
}

pub fn getControlledThreadContext(thread_index: usize, generation: u32, owner: kernel.PrincipalId, out: *process_abi.ThreadUserContext) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const ctx = threadContext(thread_index) orelse return false;
    if (!controlledThreadAvailableLocked(ctx, thread_index, generation, owner)) return false;
    out.* = .{ .size = process_abi.thread_user_context_size, .xstate_features = process_abi.signal_xstate_feature_mask, .fs_base = ctx.fs_base, .gs_base = ctx.gs_base, .pkru = ctx.pkru, .reserved = .{ 0, 0, 0 }, .registers = undefined, .xstate = ctx.x_state };
    inline for (std.meta.fields(process_abi.ThreadRegisters)) |field|
        @field(out.registers, field.name) = @field(ctx.frame, field.name);
    return true;
}

/// The syscall validated the complete architectural image before this commit.
pub fn setControlledThreadContext(thread_index: usize, generation: u32, owner: kernel.PrincipalId, image: *const process_abi.ThreadUserContext) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!controlledThreadAvailableLocked(ctx, thread_index, generation, owner)) return false;
    inline for (std.meta.fields(TrapFrame)) |field|
        @field(ctx.frame, field.name) = @field(image.registers, field.name);
    ctx.fs_base = image.fs_base;
    ctx.gs_base = image.gs_base;
    ctx.pkru = @intCast(image.pkru);
    ctx.x_state = image.xstate;
    return true;
}

test "controlled thread context timed wait completion ordering" {
    for (0..3) |winner| {
        initializeStaticStorage();
        defer initializeStaticStorage();
        const tid = 4;
        const generation = 17;
        const token = 11;
        const owner = kernel.processPrincipalFromIndex(0).?;
        const node = &initial_scheduler_entities[tid];
        const ctx = &initial_thread_contexts[tid];
        ctx.* = .{ .id = tid, .allocated = true, .generation = generation, .owner_process = owner, .scheduler_entity = node, .cpu_affinity_mask = 1, .ready = false, .wait_mailbox = true, .active_wait_token = token, .wake_tick = 100 };
        defer ctx.* = .{ .id = tid, .scheduler_entity = node };
        defer initial_thread_hot_states[tid] = .{};
        scheduler_state.cpu_count = 1;
        scheduler_state.cpus[0].is_idle = true;
        scheduler_state.cpus[0].current_thread = idleThreadMarker;
        try std.testing.expect(verifiedAddThread(tid, generation, false));
        syncHotStateFromContext(tid);
        ctx.frame.rax = 123; // Timeout return value already saved by block.
        if (winner == 0) {
            try std.testing.expectEqual(.claimed, claimWaitTokenCompletion(tid, generation, owner, token));
            wakeIfTimerExpired(tid, 100);
            try std.testing.expect(!ctx.ready and ctx.wait_completion_claimed);
            try std.testing.expectEqual(@as(u64, 0), ctx.wake_tick);
            try std.testing.expectEqual(.published, publishClaimedWaitCompletion(tid, generation, owner, token, 456));
        } else {
            if (winner == 1) {
                wakeIfTimerExpired(tid, 100);
            } else {
                try std.testing.expectEqual(.woke, wakeIfWaitingTokenGenerationWithRax(tid, generation, owner, token, 123));
            }
            try std.testing.expectEqual(.stale, claimWaitTokenCompletion(tid, generation, owner, token));
            try std.testing.expectEqual(.stale, publishClaimedWaitCompletion(tid, generation, owner, token, 456));
        }
        wakeIfTimerExpired(tid, 101);
        try std.testing.expect(ctx.ready and !ctx.wait_mailbox);
        try std.testing.expectEqual(@as(u64, 0), ctx.active_wait_token);
        try std.testing.expectEqual(@as(u64, if (winner == 0) 456 else 123), ctx.frame.rax);
        try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[0].runqueue.count);
    }
}

test "controlled thread context requires quiescence and keeps wait lifecycle intact" {
    initializeStaticStorage();
    defer initializeStaticStorage();
    const tid = 4;
    const ctx = &initial_thread_contexts[tid];
    const owner = kernel.processPrincipalFromIndex(0).?;
    ctx.* = .{ .id = tid, .allocated = true, .generation = 17, .owner_process = owner, .stopped = true, .resume_after_stop = true, .fs_base = 0x4100_0000 };
    defer ctx.* = .{ .id = tid, .scheduler_entity = &initial_scheduler_entities[tid] };
    var image: process_abi.ThreadUserContext = undefined;
    try std.testing.expect(!getControlledThreadContext(tid, 16, owner, &image));
    try std.testing.expect(!getControlledThreadContext(tid, 17, kernel.processPrincipalFromIndex(1).?, &image));
    scheduler_state.cpus[0].is_idle = false;
    scheduler_state.cpus[0].current_thread = tid;
    try std.testing.expect(!getControlledThreadContext(tid, 17, owner, &image));
    scheduler_state.cpus[0].is_idle = true;
    try std.testing.expect(getControlledThreadContext(tid, 17, owner, &image));
    try std.testing.expectEqual(@as(u64, 0x4100_0000), image.fs_base);
    image.registers.rax = 123;
    image.gs_base = 0x4200_0000;
    image.xstate[160] = 0x7f;
    ctx.wait_mailbox = true;
    try std.testing.expect(!setControlledThreadContext(tid, 17, owner, &image));
    try std.testing.expectEqual(@as(u64, 0), ctx.frame.rax);
    ctx.wait_mailbox = false;
    ctx.wait_completion_publish_pending = true;
    try std.testing.expect(!setControlledThreadContext(tid, 17, owner, &image));
    ctx.wait_completion_publish_pending = false;
    ctx.active_fault_frame = 4096;
    try std.testing.expect(!getControlledThreadContext(tid, 17, owner, &image));
    ctx.active_fault_frame = 0;
    ctx.ready = true;
    try std.testing.expect(!setControlledThreadContext(tid, 17, owner, &image));
    ctx.ready = false;
    try std.testing.expect(setControlledThreadContext(tid, 17, owner, &image));
    try std.testing.expectEqual(@as(u64, 123), ctx.frame.rax);
    try std.testing.expectEqual(@as(u64, 0x4200_0000), ctx.gs_base);
    try std.testing.expectEqual(@as(u8, 0x7f), ctx.x_state[160]);
    try std.testing.expect(ctx.stopped and ctx.resume_after_stop and !ctx.ready);
}

/// Capture the sole suspended thread of a staged process only while neither
/// address space can have another executing or schedulable thread.  Holding
/// the thread table, every CPU ownership record, and every runqueue together
/// closes the interval between a runqueue pick and current-principal
/// publication that separate live-count/CPU-mask checks cannot observe.
pub fn singleThreadExecImage(
    current_owner: kernel.PrincipalId,
    staged_owner: kernel.PrincipalId,
    staged_thread_index: usize,
    staged_generation: u32,
) ?SuspendedThreadImage {
    if (current_owner == staged_owner) return null;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    lockAllCpuRunqueues();
    defer unlockAllCpuRunqueues();

    const local_cpu = currentCpu();
    if (local_cpu >= scheduler_state.cpus.len) return null;
    const local_state = &scheduler_state.cpus[local_cpu];
    if (!local_state.enabled or local_state.is_idle or local_state.current_principal != current_owner) return null;
    const current_thread_index = local_state.current_thread;

    var current_count: usize = 0;
    var staged_count: usize = 0;
    var thread_index: usize = 0;
    while (thread_index < scheduler_state.thread_table.contexts.len) : (thread_index += 1) {
        const ctx = &scheduler_state.thread_table.contexts[thread_index];
        if (!ctx.allocated) continue;
        if (ctx.owner_process == current_owner) {
            current_count += 1;
            if (thread_index != current_thread_index) return null;
        } else if (ctx.owner_process == staged_owner) {
            staged_count += 1;
            if (thread_index != staged_thread_index) return null;
        }
    }
    if (current_count != 1 or staged_count != 1) return null;

    const current_ctx = threadContext(current_thread_index) orelse return null;
    if (!current_ctx.allocated or current_ctx.owner_process != current_owner or current_ctx.cpu_slot != local_cpu) return null;
    const current_node = current_ctx.scheduler_entity orelse return null;
    if (!nodeMatches(current_node, current_thread_index, current_ctx.generation, local_cpu) or
        current_node.ownership != .running or local_state.current_entity != current_node)
    {
        return null;
    }

    const staged_ctx = threadContext(staged_thread_index) orelse return null;
    if (!staged_ctx.allocated or staged_ctx.generation != staged_generation or
        staged_ctx.owner_process != staged_owner or staged_ctx.cpu_slot >= scheduler_state.cpus.len or
        staged_ctx.ready or staged_ctx.wait_mailbox or staged_ctx.active_wait_token != 0 or
        staged_ctx.wait_completion_claimed or
        staged_ctx.wake_tick != 0 or
        staged_ctx.stopped or staged_ctx.resume_after_stop)
    {
        return null;
    }
    const staged_hot = getThreadHotStateConst(staged_thread_index) orelse return null;
    if (staged_hot.allocated == 0 or staged_hot.ready != 0 or staged_hot.wait_mailbox != 0 or
        staged_hot.stopped != 0 or staged_hot.wake_tick != 0 or staged_hot.owner_process != staged_owner)
    {
        return null;
    }
    const staged_node = staged_ctx.scheduler_entity orelse return null;
    if (!nodeMatches(staged_node, staged_thread_index, staged_generation, staged_ctx.cpu_slot) or
        staged_node.ownership != .blocked or staged_node.entity.state != .blocked or
        staged_node.linked_tree != null)
    {
        return null;
    }

    for (&scheduler_state.cpus, 0..) |*state, cpu_slot| {
        if (!state.enabled) continue;
        if (state.current_principal) |owner| {
            if (owner == staged_owner) return null;
            if (owner == current_owner and cpu_slot != local_cpu) return null;
        }
        if (state.current_entity) |entity| {
            if (entity.thread_index == staged_thread_index) return null;
            if (entity.thread_index == current_thread_index and cpu_slot != local_cpu) return null;
            const entity_ctx = threadContext(entity.thread_index) orelse return null;
            if (entity_ctx.owner_process == staged_owner) return null;
            if (entity_ctx.owner_process == current_owner and cpu_slot != local_cpu) return null;
        }
    }

    return .{
        .frame = staged_ctx.frame,
        .fs_base = staged_ctx.fs_base,
        .gs_base = staged_ctx.gs_base,
        .pkru = staged_ctx.pkru,
    };
}

pub fn installExecContextForCurrentThread(
    user_spaces: *UserAddressSpaceTable,
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
        ctx.x_state = initial_x_state;
        // Signal trampoline addresses belong to the old executable image.
        // Keep pending signals across exec, but require the new runtime to
        // register its own address-bound delivery configuration.
        ctx.signal_entry = 0;
        _ = kernel_runtime.kernel_state_global.clearProcessSignalPendingHintVa(owner_process);
        ctx.signal_inhibit_start = 0;
        ctx.signal_inhibit_end = 0;
        ctx.signal_inhibit_secondary_start = 0;
        ctx.signal_inhibit_secondary_end = 0;
        ctx.signal_owner = false;
        ctx.fault_entry = 0;
        ctx.fault_stack_base = 0;
        ctx.fault_stack_size = 0;
        ctx.active_fault_frame = 0;
        // The current private exec image contract does not carry a blocked
        // mask.  Keep kernel and the new LPR BSS in sync instead of retaining
        // a kernel-only value that userland would immediately overwrite.
        // Fork registration does preserve its copied mask because this reset
        // is confined to exec installation, not generic REGISTER.
        ctx.signal_blocked_mask = 0;
        ctx.signal_interrupt_consumed = firstUnblockedPendingSignal(ctx) == 0;
        ctx.ready = true;
        ctx.wait_mailbox = false;
        ctx.active_wait_token = 0;
        ctx.wait_completion_claimed = false;
        ctx.wait_completion_publish_pending = false;
        ctx.wait_completion_rax = 0;
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

fn saveCurrentThreadContextFromFrameLocked(comptime Registers: type, frame: *const TrapFrame) void {
    const current_thread = currentThread();
    const ctx = threadContextMutable(current_thread) orelse return;
    if (!ctx.allocated) return;
    ctx.frame = frame.*;
    ctx.cr3 = currentCr3();
    ctx.fs_base = Registers.readFsBase();
    ctx.gs_base = Registers.readGsBase();
    ctx.pkru = Registers.readPkru();
    setThreadHotCr3(current_thread, ctx.cr3);
    const hot = getThreadHotStateConst(current_thread) orelse return;
    if (hot.wait_mailbox == 0 and hot.wake_tick == 0) {
        ctx.ready = true;
        setThreadHotReady(current_thread, true);
    }
}

fn loadContextIntoFrameGenerationLocked(thread_index: usize, generation: u32, frame: *TrapFrame) bool {
    return loadContextIntoFrameGenerationLockedWithRegisters(x86_platform, thread_index, generation, frame);
}

fn loadContextIntoFrameGenerationLockedWithRegisters(comptime Registers: type, thread_index: usize, generation: u32, frame: *TrapFrame) bool {
    return loadContextIntoFrameGenerationLockedMode(Registers, true, thread_index, generation, frame);
}

fn loadContextIntoFrameGenerationLockedMode(comptime Registers: type, comptime install_bases: bool, thread_index: usize, generation: u32, frame: *TrapFrame) bool {
    const ctx = threadContext(thread_index) orelse return false;
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    if (!ctx.allocated or ctx.generation != generation or hot.allocated == 0) return false;
    if (hot.ready == 0) return false;
    schedulerFullMemoryFence();
    if (install_bases) {
        Registers.writeFsBase(ctx.fs_base);
        Registers.writeGsBase(ctx.gs_base);
        Registers.writePkru(ctx.pkru);
    }
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
    return switchToGenerationWithRegisters(x86_platform, next_thread, next_generation, frame, saved_rax);
}

fn restoreSwitchSourceLocked(comptime Registers: type, thread_index: usize, generation: u32, frame: *TrapFrame) void {
    _ = activateGenerationLockedWithRegisters(Registers, false, thread_index, generation);
    _ = loadContextIntoFrameGenerationLockedWithRegisters(Registers, thread_index, generation, frame);
}

fn switchToGenerationWithRegisters(comptime Registers: type, next_thread: usize, next_generation: u32, frame: *TrapFrame, saved_rax: ?u64) bool {
    // The current/next contexts must remain pinned from hardware capture to
    // frame installation. Splitting these steps reacquires the table and
    // writes the same target bases in both activation and frame loading.
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return switchToGenerationLockedWithRegisters(Registers, next_thread, next_generation, frame, saved_rax);
}

fn switchToGenerationLocked(next_thread: usize, next_generation: u32, frame: *TrapFrame, saved_rax: ?u64) bool {
    return switchToGenerationLockedWithRegisters(x86_platform, next_thread, next_generation, frame, saved_rax);
}

fn switchToGenerationLockedWithRegisters(comptime Registers: type, next_thread: usize, next_generation: u32, frame: *TrapFrame, saved_rax: ?u64) bool {
    if (!isBootstrapSchedulerCpu() and !apUserDispatchReady()) return false;
    if (next_thread >= scheduler_state.thread_table.contexts.len) return false;
    const current_thread = currentThread();
    const current_ctx = threadContext(current_thread) orelse return false;
    if (!current_ctx.allocated) return false;
    const current_generation = current_ctx.generation;
    if (next_thread == current_thread) {
        if (current_generation != next_generation) return false;
        if (saved_rax) |value| frame.rax = value;
        return true;
    }

    var saved = frame.*;
    if (saved_rax) |value| saved.rax = value;
    saveCurrentThreadContextFromFrameLocked(Registers, &saved);

    // Syscall/interrupt entry keeps IF clear. No external callback may run
    // between publishing the target identity and installing its live bases.
    // Standalone activation and the common user-return guard still install
    // bases; only this table-locked transfer avoids the duplicate write.
    if (!activateGenerationLockedWithRegisters(Registers, false, next_thread, next_generation)) return false;
    if (!loadContextIntoFrameGenerationLockedWithRegisters(Registers, next_thread, next_generation, frame)) {
        restoreSwitchSourceLocked(Registers, current_thread, current_generation, frame);
        return false;
    }
    return true;
}

pub fn switchTo(next_thread: usize, frame: *TrapFrame, saved_rax: ?u64) bool {
    const generation = generationOfThread(next_thread) orelse return false;
    return switchToGeneration(next_thread, generation, frame, saved_rax);
}

const ContextTransferTestRegisters = struct {
    var fs: u64 = 0;
    var gs: u64 = 0;
    var pkru: u32 = 0;
    var reads: [3]usize = .{ 0, 0, 0 };
    var writes: [3]usize = .{ 0, 0, 0 };

    fn reset() void {
        fs = 0x1111;
        gs = 0x2222;
        pkru = 0x40000000;
        reads = .{ 0, 0, 0 };
        writes = .{ 0, 0, 0 };
    }
    fn readFsBase() u64 {
        reads[0] += 1;
        return fs;
    }
    fn readGsBase() u64 {
        reads[1] += 1;
        return gs;
    }
    fn readPkru() u32 {
        reads[2] += 1;
        return pkru;
    }
    fn writeFsBase(value: u64) void {
        writes[0] += 1;
        fs = value;
    }
    fn writeGsBase(value: u64) void {
        writes[1] += 1;
        gs = value;
    }
    fn writePkru(value: u32) void {
        writes[2] += 1;
        pkru = value;
    }
};

test "context transfer captures live sender and installs target bases once" {
    const Registers = ContextTransferTestRegisters;
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    const sender = 4;
    const target = 5;
    for (sender..target + 1) |index| {
        initial_thread_contexts[index] = .{
            .id = @intCast(index),
            .allocated = true,
            .ready = true,
            .scheduler_entity = &initial_scheduler_entities[index],
            .cr3 = if (index == sender) 0x1000 else 0x2000,
            .owner_process = kernel.processPrincipalFromIndex(index - sender).?,
            .fs_base = 0x3333,
            .gs_base = 0x4444,
            .pkru = 0x80000000,
        };
        initial_thread_contexts[index].frame.rax = 81;
        initial_thread_contexts[index].frame.rip = 0x5000;
        syncHotStateFromContext(index);
    }
    scheduler_state.cpus[0].current_thread = sender;
    Registers.reset();
    var frame = std.mem.zeroes(TrapFrame);
    frame.rax = 13;
    frame.rip = 0x6000;
    try std.testing.expect(switchToGenerationWithRegisters(Registers, sender, 1, &frame, 7));
    try std.testing.expectEqual(@as(u64, 7), frame.rax);
    try std.testing.expectEqual([3]usize{ 0, 0, 0 }, Registers.reads);
    try std.testing.expectEqual([3]usize{ 0, 0, 0 }, Registers.writes);
    try std.testing.expect(!switchToGenerationWithRegisters(Registers, sender, 2, &frame, 8));
    try std.testing.expectEqual(@as(u64, 7), frame.rax);

    try std.testing.expect(switchToGenerationWithRegisters(Registers, target, 1, &frame, 42));
    const source = &initial_thread_contexts[sender];
    try std.testing.expectEqual(@as(u64, 42), source.frame.rax);
    try std.testing.expectEqual(@as(u64, 0x6000), source.frame.rip);
    // The cached source bases deliberately differ from actual hardware.
    try std.testing.expectEqual(@as(u64, 0x1111), source.fs_base);
    try std.testing.expectEqual(@as(u64, 0x2222), source.gs_base);
    try std.testing.expectEqual(@as(u32, 0x40000000), source.pkru);
    try std.testing.expectEqual([3]usize{ 1, 1, 1 }, Registers.reads);
    try std.testing.expectEqual([3]usize{ 1, 1, 1 }, Registers.writes);
    try std.testing.expectEqual(@as(u64, 0x3333), Registers.fs);
    try std.testing.expectEqual(@as(u64, 0x4444), Registers.gs);
    try std.testing.expectEqual(@as(u32, 0x80000000), Registers.pkru);
    try std.testing.expectEqualDeep(initial_thread_contexts[target].frame, frame);
    try std.testing.expectEqual(@as(usize, target), currentThread());
    try std.testing.expectEqual(@as(u64, 0x2000), currentCr3());
    try std.testing.expectEqual(initial_thread_contexts[target].owner_process, scheduler_state.cpus[0].current_principal.?);
    try std.testing.expectEqual(@as(u64, 0x2000), scheduler_state.cpus[0].current_cr3);

    // Exercise the same rollback helper used after a failed frame install.
    scheduler_state.thread_table.lock();
    restoreSwitchSourceLocked(Registers, sender, 1, &frame);
    scheduler_state.thread_table.unlock();
    try std.testing.expectEqual(@as(usize, sender), currentThread());
    try std.testing.expectEqual(@as(u64, 0x1000), currentCr3());
    try std.testing.expectEqual(source.owner_process, scheduler_state.cpus[0].current_principal.?);
    try std.testing.expectEqual(@as(u64, 0x1000), scheduler_state.cpus[0].current_cr3);
    try std.testing.expectEqualDeep(source.frame, frame);
    try std.testing.expectEqual(@as(u64, 0x1111), Registers.fs);
    try std.testing.expectEqual(@as(u64, 0x2222), Registers.gs);
    try std.testing.expectEqual(@as(u32, 0x40000000), Registers.pkru);
    try std.testing.expectEqual([3]usize{ 2, 2, 2 }, Registers.writes);

    // Public activation's implementation must retain its standalone writes.
    scheduler_state.thread_table.lock();
    const activated = activateGenerationLockedWithRegisters(Registers, true, target, 1);
    scheduler_state.thread_table.unlock();
    try std.testing.expect(activated);
    try std.testing.expectEqual([3]usize{ 3, 3, 3 }, Registers.writes);
}

test "context transfer rejects stale unready wrong CPU and disabled destination" {
    const Registers = ContextTransferTestRegisters;
    for (0..6) |scenario| {
        const saved_contexts = initial_thread_contexts[4..6].*;
        const saved_hot = initial_thread_hot_states[4..6].*;
        defer {
            initial_thread_contexts[4..6].* = saved_contexts;
            initial_thread_hot_states[4..6].* = saved_hot;
            initializeStaticStorage();
        }
        initializeStaticStorage();
        const sender = 4;
        const target = 5;
        for (sender..target + 1) |index| {
            initial_thread_contexts[index] = .{ .id = @intCast(index), .allocated = true, .ready = true, .scheduler_entity = &initial_scheduler_entities[index] };
            syncHotStateFromContext(index);
        }
        scheduler_state.cpus[0].current_thread = sender;
        const ctx = &initial_thread_contexts[target];
        switch (scenario) {
            0 => ctx.generation = 2,
            1 => ctx.ready = false,
            2 => ctx.cpu_slot = 1,
            3 => ctx.allocated = false,
            4 => scheduler_state.cpus[0].enabled = false,
            5 => initial_thread_contexts[sender].allocated = false,
            else => unreachable,
        }
        syncHotStateFromContext(target);
        Registers.reset();
        var frame = std.mem.zeroes(TrapFrame);
        frame.rax = 9;
        const before = frame;
        try std.testing.expect(!switchToGenerationWithRegisters(Registers, target, 1, &frame, 71));
        try std.testing.expectEqual(@as(usize, sender), currentThread());
        try std.testing.expectEqualDeep(before, frame);
        try std.testing.expectEqual([3]usize{ 0, 0, 0 }, Registers.writes);
        try std.testing.expectEqual(@as(u64, 0x1111), Registers.fs);
        try std.testing.expectEqual(@as(u64, 0x2222), Registers.gs);
        try std.testing.expectEqual(@as(u32, 0x40000000), Registers.pkru);
    }
}

pub fn handoffToReadyThreadGenerationWithRax(
    frame: *TrapFrame,
    target_thread: usize,
    target_generation: u32,
    sender_rax: u64,
    before_current_thread_leave: ?BeforeCurrentThreadLeaveCallback,
) bool {
    return handoffToReadyThreadGenerationInternal(false, frame, target_thread, target_generation, sender_rax, before_current_thread_leave);
}

pub fn handoffToOnlyReadyThreadGenerationWithRax(
    frame: *TrapFrame,
    target_thread: usize,
    target_generation: u32,
    sender_rax: u64,
    before_current_thread_leave: ?BeforeCurrentThreadLeaveCallback,
) bool {
    return handoffToReadyThreadGenerationInternal(true, frame, target_thread, target_generation, sender_rax, before_current_thread_leave);
}

fn handoffToReadyThreadGenerationInternal(
    comptime only_ready: bool,
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
    const target_cpu = target_ctx.cpu_slot;
    const can_handoff =
        current_ctx.allocated and
        target_ctx.allocated and
        target_ctx.generation == target_generation and
        target_ctx.ready and
        threadAllowsCpu(target_ctx, cpu_id) and
        current_ctx.cpu_slot == cpu_id;
    scheduler_state.thread_table.unlock();
    if (!can_handoff) return false;
    if (only_ready and target_cpu != cpu_id) return false;
    // IPC completion may have made the receiver runnable on another CPU.
    // Move only that runnable generation into the sender's runqueue before
    // consuming it there: migrateRunnableThreadGeneration serializes both
    // runqueues and republishes cpu_slot, so the target never has two owners.
    // If its old CPU has already selected it, migration fails and the normal
    // remote wake remains the safe fallback.
    if (target_cpu != cpu_id and
        !migrateRunnableThreadGeneration(target_thread, target_generation, cpu_id))
    {
        return false;
    }
    if (!verifiedHandoffToThreadOnCpu(only_ready, cpu_id, target_thread, target_generation, current_thread, current_generation)) return false;
    // Keep the caller's global state lock through the scheduler transition.
    // Once the verified core has made the target the running entity, dropping
    // that lock before installing its saved context lets a remote lifecycle
    // operation invalidate either side of the handoff.  A failed switch would
    // then return to the syscall with both a rolled-back scheduler transition
    // and an already-released lock.  The callback is still before the actual
    // user return when it runs after the in-kernel context installation.
    if (switchTo(target_thread, frame, sender_rax)) {
        if (before_current_thread_leave) |callback| callback.run(callback.context);
        return true;
    }

    verifiedRollbackHandoff(cpu_id, current_thread, current_generation);
    return false;
}

/// Consume one local runnable entity at a user-mode wake-IPI boundary.  The
/// normal remote-wake path has no sender trap frame to name a particular
/// receiver, but it must still not leave a newly runnable entity behind an
/// unrelated userspace timeslice.  `handoffToReadyThreadGenerationWithRax`
/// keeps the runqueue ownership transition atomic and preserves the
/// interrupted thread as runnable on this CPU.
pub fn handoffToBestReadyThreadForWakeIpi(frame: *TrapFrame) bool {
    return handoffToBestReadyThreadWithRax(frame, frame.rax, null);
}

// Yield and wake-IPI handoff differ only in the outgoing return value and
// the syscall lock-release callback. Keep selection and generation checks
// shared so neither path can enqueue a running thread twice.
pub fn handoffToBestReadyThreadWithRax(
    frame: *TrapFrame,
    sender_rax: u64,
    before_current_thread_leave: ?BeforeCurrentThreadLeaveCallback,
) bool {
    if (!verifiedCoreReady()) return false;
    const cpu_id = currentCpu();
    if (cpu_id >= verifiedCoreCpuCount()) return false;
    const current_thread = currentThread();
    const target = bestReadyThreadForCpu(cpu_id, current_thread) orelse return false;

    return handoffToReadyThreadGenerationWithRax(
        frame,
        target.thread_index,
        target.generation,
        sender_rax,
        before_current_thread_leave,
    );
}

fn bestReadyThreadForCpu(cpu_id: usize, current_thread: usize) ?PickedThread {
    const state = schedulerStateForSlot(cpu_id) orelse return null;

    state.runqueue_lock.lock();
    const target = state.runqueue.bestEligible(state.virtual_time) orelse {
        state.runqueue_lock.unlock();
        return null;
    };
    const target_thread = target.thread_index;
    const target_generation = target.generation;
    state.runqueue_lock.unlock();
    if (target_thread == current_thread) return null;
    return .{ .thread_index = target_thread, .generation = target_generation };
}

test "yield handoff rejects stale blocked affinity and executing receivers" {
    initializeStaticStorage();
    defer initializeStaticStorage();
    const sender = 4;
    const peer = sender + 1;
    for (sender..peer + 1) |index| {
        const node = &initial_scheduler_entities[index];
        initial_thread_contexts[index] = .{ .id = @intCast(index), .allocated = true, .ready = true, .scheduler_entity = node };
        try std.testing.expect(verifiedAddThread(index, 1, true));
    }
    defer for (sender..peer + 1) |index| {
        initial_thread_contexts[index] = .{ .id = @intCast(index), .scheduler_entity = &initial_scheduler_entities[index] };
    };
    const picked = verifiedPickThreadForCpu(0).?;
    try std.testing.expectEqual(@as(usize, sender), picked.thread_index);
    scheduler_state.cpus[0].current_thread = sender;
    var frame: TrapFrame = undefined;
    try std.testing.expect(!handoffToReadyThreadGenerationWithRax(&frame, peer, 2, 0, null));
    try std.testing.expect(threadWakeTargetIsLive(peer, 1, default_process_principal));
    try std.testing.expect(!threadWakeTargetIsLive(peer, 2, default_process_principal));
    try std.testing.expect(!threadWakeTargetIsLive(peer, 1, kernel.processPrincipalFromIndex(1).?));
    const ctx = &initial_thread_contexts[peer];
    ctx.ready = false;
    try std.testing.expect(!handoffToReadyThreadGenerationWithRax(&frame, peer, 1, 0, null));
    try std.testing.expect(!ctx.ready);
    ctx.ready = true;
    ctx.cpu_affinity_mask = 2;
    try std.testing.expect(!handoffToReadyThreadGenerationWithRax(&frame, peer, 1, 0, null));
    scheduler_state.cpu_count = 2;
    scheduler_state.cpus[1].enabled = true;
    ctx.cpu_affinity_mask = 1;
    try std.testing.expect(!migrateRunnableThreadGeneration(peer, 1, 1));
    ctx.cpu_affinity_mask = 3;
    try std.testing.expect(migrateRunnableThreadGeneration(peer, 1, 1));
    scheduler_state.cpus[1].current_thread = peer;
    scheduler_state.cpus[1].is_idle = false;
    try std.testing.expect(!handoffToReadyThreadGenerationWithRax(&frame, peer, 1, 0, null));
    try std.testing.expectEqual(@as(usize, 1), ctx.cpu_slot);
    try std.testing.expectEqual(.runnable, initial_scheduler_entities[peer].ownership);
    try std.testing.expectEqual(.running, initial_scheduler_entities[sender].ownership);
}

test "yield handoff keeps runnable ownership and rolls back without duplication" {
    for ([_]usize{ 0, 1, 5, 63 }) |cpu| {
        initializeStaticStorage();
        defer initializeStaticStorage();
        scheduler_state.cpu_count = cpu + 1;
        const state = &scheduler_state.cpus[cpu];
        state.enabled = true;
        const tid = 4;
        for (tid..tid + 2) |index| {
            const node = &initial_scheduler_entities[index];
            const ctx = &initial_thread_contexts[index];
            ctx.* = .{ .id = @intCast(index), .allocated = true, .ready = true, .scheduler_entity = node, .cpu_slot = cpu };
            node.cpu_slot = cpu;
            try std.testing.expectEqual(.ok, verified_sched.pacha_eevdf_entity_init(
                @intCast(index + 1),
                1,
                1024,
                4_000_000,
                0,
                &node.entity,
            ));
            node.ownership = .runnable;
            try std.testing.expect(state.runqueue.insert(node));
            if (index == tid) {
                const picked = verifiedPickThreadForCpu(cpu) orelse return error.TestUnexpectedResult;
                try std.testing.expectEqual(@as(usize, tid), picked.thread_index);
                try std.testing.expect(bestReadyThreadForCpu(cpu, tid) == null);
            }
        }
        defer for (tid..tid + 2) |index| {
            initial_thread_contexts[index] = .{ .id = @intCast(index), .scheduler_entity = &initial_scheduler_entities[index] };
        };
        const peer = bestReadyThreadForCpu(cpu, tid) orelse return error.TestUnexpectedResult;
        try std.testing.expectEqual(@as(usize, tid + 1), peer.thread_index);
        try std.testing.expect(!verifiedHandoffToThreadOnCpu(false, cpu, peer.thread_index, peer.generation + 1, tid, 1));
        try std.testing.expectEqual(@as(usize, 1), state.runqueue.count);
        try std.testing.expect(verifiedHandoffToThreadOnCpu(false, cpu, peer.thread_index, peer.generation, tid, 1));
        try std.testing.expectEqual(.runnable, initial_scheduler_entities[tid].ownership);
        try std.testing.expectEqual(.running, initial_scheduler_entities[tid + 1].ownership);
        try std.testing.expectEqual(@as(usize, 1), state.runqueue.count);
        try std.testing.expect(!verifiedHandoffToThreadOnCpu(false, cpu, tid + 1, 1, tid, 1));
        verifiedRollbackHandoff(cpu, tid, 1);
        try std.testing.expectEqual(.running, initial_scheduler_entities[tid].ownership);
        try std.testing.expectEqual(.runnable, initial_scheduler_entities[tid + 1].ownership);
        try std.testing.expectEqual(@as(usize, 1), state.runqueue.count);
        try std.testing.expect(state.current_entity == &initial_scheduler_entities[tid]);
    }
}

test "futex sole handoff refuses a competitor queued after the wake hint" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    for (4..7) |tid| {
        const node = &initial_scheduler_entities[tid];
        initial_thread_contexts[tid] = .{ .id = @intCast(tid), .allocated = true, .ready = true, .scheduler_entity = node };
        try std.testing.expect(verifiedAddThread(tid, 1, true));
    }
    try std.testing.expectEqual(@as(usize, 4), verifiedPickThreadForCpu(0).?.thread_index);
    const state = &scheduler_state.cpus[0];
    try std.testing.expectEqual(@as(usize, 2), state.runqueue.count);
    try std.testing.expect(!verifiedHandoffToThreadOnCpu(true, 0, 5, 1, 4, 1));
    try std.testing.expectEqual(.running, initial_scheduler_entities[4].ownership);
    try std.testing.expectEqual(.runnable, initial_scheduler_entities[5].ownership);
    try std.testing.expectEqual(.runnable, initial_scheduler_entities[6].ownership);
    try std.testing.expect(state.runqueue.remove(&initial_scheduler_entities[6]));
    try std.testing.expect(verifiedHandoffToThreadOnCpu(true, 0, 5, 1, 4, 1));
    try std.testing.expectEqual(.runnable, initial_scheduler_entities[4].ownership);
    try std.testing.expectEqual(.running, initial_scheduler_entities[5].ownership);
    try std.testing.expectEqual(@as(usize, 1), state.runqueue.count);
}

test "futex local wake keeps token authority and publication rollback" {
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    scheduler_state.cpu_count = 2;
    scheduler_state.cpus[1].enabled = true;
    const owner = kernel.processPrincipalFromIndex(0).?;
    const sender = &initial_thread_contexts[4];
    sender.* = .{ .id = 4, .allocated = true, .ready = true, .owner_process = owner, .scheduler_entity = &initial_scheduler_entities[4], .cpu_affinity_mask = 3 };
    try std.testing.expect(verifiedAddThread(4, 1, true));
    try std.testing.expectEqual(@as(usize, 4), verifiedPickThreadForCpu(0).?.thread_index);
    scheduler_state.cpus[0].current_thread = 4;
    scheduler_state.cpus[0].is_idle = false;
    const peer = &initial_thread_contexts[5];
    peer.* = .{ .id = 5, .allocated = true, .generation = 17, .ready = false, .wait_mailbox = true, .active_wait_token = 11, .wake_tick = 100, .owner_process = owner, .cpu_slot = 1, .scheduler_entity = &initial_scheduler_entities[5], .cpu_affinity_mask = 3 };
    try std.testing.expect(verifiedAddThread(5, 17, false));
    syncHotStateFromContext(5);
    try std.testing.expectEqual(.stale, wakeIfWaitingTokenGenerationWithRaxPreferUncontendedCurrent(5, 18, owner, 11, 0));
    try std.testing.expectEqual(.stale, wakeIfWaitingTokenGenerationWithRaxPreferUncontendedCurrent(5, 17, owner, 12, 0));
    try std.testing.expectEqual(.stale, wakeIfWaitingTokenGenerationWithRaxPreferUncontendedCurrent(5, 17, kernel.processPrincipalFromIndex(1).?, 11, 0));
    scheduler_state.cpus[0].enabled = false;
    scheduler_state.cpus[1].enabled = false;
    try std.testing.expectEqual(.publish_failed, wakeIfWaitingTokenGenerationWithRaxPreferUncontendedCurrent(5, 17, owner, 11, 0));
    try std.testing.expect(!peer.ready and peer.wait_mailbox);
    try std.testing.expectEqual(@as(u64, 11), peer.active_wait_token);
    try std.testing.expectEqual(@as(u64, 100), peer.wake_tick);
    try std.testing.expectEqual(.blocked, initial_scheduler_entities[5].ownership);
    scheduler_state.cpus[0].enabled = true;
    scheduler_state.cpus[1].enabled = true;
    try std.testing.expectEqual(.woke, wakeIfWaitingTokenGenerationWithRaxPreferUncontendedCurrent(5, 17, owner, 11, 0));
    try std.testing.expectEqual(@as(usize, 0), peer.cpu_slot);
    try std.testing.expectEqual(@as(u64, 0), peer.active_wait_token);
    try std.testing.expectEqual(.runnable, initial_scheduler_entities[5].ownership);
    try std.testing.expectEqual(.stale, wakeIfWaitingTokenGenerationWithRaxPreferUncontendedCurrent(5, 17, owner, 11, 0));
}

test "futex competing local queue falls back to ordinary CPU placement" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    scheduler_state.cpu_count = 2;
    scheduler_state.cpus[1].enabled = true;
    scheduler_state.cpus[1].is_idle = true;
    const owner = kernel.processPrincipalFromIndex(0).?;
    for (4..7) |tid| {
        initial_thread_contexts[tid] = .{ .id = @intCast(tid), .allocated = true, .ready = tid != 6, .owner_process = owner, .cpu_slot = if (tid == 6) 1 else 0, .scheduler_entity = &initial_scheduler_entities[tid], .cpu_affinity_mask = 3, .wait_mailbox = tid == 6, .active_wait_token = if (tid == 6) 11 else 0 };
        try std.testing.expect(verifiedAddThread(tid, 1, tid != 6));
    }
    try std.testing.expectEqual(@as(usize, 4), verifiedPickThreadForCpu(0).?.thread_index);
    scheduler_state.cpus[0].current_thread = 4;
    scheduler_state.cpus[0].is_idle = false;
    try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[0].runqueue.count);
    try std.testing.expectEqual(.woke, wakeIfWaitingTokenGenerationWithRaxPreferUncontendedCurrent(6, 1, owner, 11, 0));
    try std.testing.expectEqual(@as(usize, 1), initial_thread_contexts[6].cpu_slot);
    try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[0].runqueue.count);
    try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[1].runqueue.count);
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
    return verifiedHandoffToThreadOnCpu(false, cpu_id, target_thread, target_generation, current_thread, current_generation);
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
        if (!releaseThread(current_thread)) return false;
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
    if (before_ap_idle) |callback| callback.run(callback.context);
    loadNextReadyThreadOrIdle(frame);
    return true;
}

pub fn wakeIfWaiting(thread_index: usize) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!ctx.allocated or !ctx.wait_mailbox or
        ctx.active_wait_token != 0) return false;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
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

fn wakeIfTimerExpired(thread_index: usize, now_tick: u64) void {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return;
    // The lockless hot-state scan is only a candidate filter.  The thread may
    // have completed that wait and entered a newer one before we acquire the
    // table lock, so revalidate the current deadline before publishing ready.
    if (!ctx.allocated or ctx.ready or
        ctx.wake_tick == 0 or now_tick < ctx.wake_tick)
    {
        return;
    }
    const wait_mailbox = ctx.wait_mailbox;
    const wait_token = ctx.active_wait_token;
    const wake_tick = ctx.wake_tick;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    ctx.wake_tick = 0;
    if (ctx.stopped) {
        ctx.resume_after_stop = true;
        ctx.ready = false;
        setThreadHotWaitState(thread_index, false, 0, false);
        return;
    }
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    if (!verifiedWakeThreadGeneration(thread_index, ctx.generation)) {
        // Do not strand a blocked scheduler entity behind ready=true.  The
        // lockless timer scan treats ready as authoritative, so retain the
        // expired deadline and retry publication on the next BSP tick.
        ctx.wait_mailbox = wait_mailbox;
        ctx.active_wait_token = wait_token;
        ctx.wake_tick = wake_tick;
        ctx.ready = false;
        setThreadHotWaitState(thread_index, wait_mailbox, wake_tick, false);
    }
}

fn wakeIfWaitingGenerationInternal(
    thread_index: usize,
    generation: u32,
    resume_rax: ?u64,
    mailbox_only: bool,
    preferred_cpu: ?usize,
) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!ctx.allocated or ctx.generation != generation) return false;
    if (ctx.wait_completion_claimed) return false;
    if (mailbox_only) {
        if (!ctx.wait_mailbox or ctx.active_wait_token != 0) return false;
    } else if (ctx.ready) {
        return false;
    }
    if (resume_rax) |value| ctx.frame.rax = value;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    ctx.wake_tick = 0;
    if (ctx.stopped) {
        ctx.resume_after_stop = true;
        ctx.ready = false;
        setThreadHotWaitState(thread_index, false, 0, false);
        return true;
    }
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    _ = verifiedWakeThreadGenerationPreferred(thread_index, generation, preferred_cpu);
    return true;
}

pub const KeyedWakeResult = enum {
    woke,
    stale,
    publish_failed,
};

pub fn wakeIfWaitingTokenGenerationWithRax(
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
) KeyedWakeResult {
    return wakeIfWaitingTokenGenerationInternal(thread_index, generation, owner, wait_token, resume_rax, null);
}

pub fn wakeIfWaitingTokenGenerationWithRaxPreferUncontendedCurrent(
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
) KeyedWakeResult {
    const cpu = currentCpu();
    const preferred = if (schedulerStateForSlot(cpu)) |state| blk: {
        state.runqueue_lock.lock();
        const uncontended = state.current_entity != null and state.runqueue.count == 0;
        state.runqueue_lock.unlock();
        break :blk if (uncontended) cpu else null;
    } else null;
    return wakeIfWaitingTokenGenerationInternal(thread_index, generation, owner, wait_token, resume_rax, preferred);
}

fn wakeIfWaitingTokenGenerationInternal(
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
    preferred_cpu: ?usize,
) KeyedWakeResult {
    if (wait_token == 0) return .stale;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return wakeIfWaitingTokenGenerationLocked(thread_index, generation, owner, wait_token, resume_rax, preferred_cpu);
}

fn wakeIfWaitingTokenGenerationLocked(
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
    preferred_cpu: ?usize,
) KeyedWakeResult {
    if (wait_token == 0) return .stale;
    const ctx = threadContextMutable(thread_index) orelse return .stale;
    if (!ctx.allocated or ctx.owner_process != owner or
        ctx.generation != generation or ctx.active_wait_token != wait_token or
        ctx.wait_completion_claimed)
    {
        return .stale;
    }

    if (ctx.ready) {
        if (!ctx.wait_mailbox) return .stale;
        ctx.frame.rax = resume_rax;
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        return .woke;
    }
    if (!ctx.wait_mailbox) return .stale;

    const previous_rax = ctx.frame.rax;
    const wake_tick = ctx.wake_tick;
    ctx.frame.rax = resume_rax;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    ctx.wake_tick = 0;
    if (ctx.stopped) {
        ctx.resume_after_stop = true;
        ctx.ready = false;
        setThreadHotWaitState(thread_index, false, 0, false);
        return .woke;
    }
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    if (!verifiedWakeThreadGenerationPreferred(thread_index, generation, preferred_cpu)) {
        ctx.frame.rax = previous_rax;
        ctx.wait_mailbox = true;
        ctx.active_wait_token = wait_token;
        ctx.wake_tick = wake_tick;
        ctx.ready = false;
        setThreadHotWaitState(thread_index, true, wake_tick, false);
        return .publish_failed;
    }
    return .woke;
}

pub const KeyedWakeHandoffResult = struct {
    wake: KeyedWakeResult,
    handed_off: bool = false,
};

pub const WakeWaitTransfer = struct {
    peer: usize,
    peer_generation: u32,
    owner: kernel.PrincipalId,
    peer_token: u64,
    caller_token: u64,
    timeout_ticks: u64,
    resume_rax: u64,
};

// null has no effects; true installs the receiver; false has committed the
// wake and blocked the source but requires idle/reselection. The futex caller
// must retire the peer record before running its leave callback, exactly once.
pub fn tryWakeWaitTransfer(frame: *TrapFrame, request: WakeWaitTransfer) ?bool {
    if (comptime perf.enabled) perf.handoffAdd(.attempts, 1);
    const table_wait_start = perf.timestamp();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    if (comptime perf.enabled) perf.handoffElapsed(.table_wait_cycles, table_wait_start);
    const result = tryWakeWaitTransferLocked(x86_platform, installWakeWaitReceiver, frame, request);
    if (comptime perf.enabled) {
        if (result == null) perf.handoffAdd(.rejected, 1);
    }
    return result;
}

fn installWakeWaitReceiver(thread: usize, generation: u32, frame: *TrapFrame) bool {
    // The source is already saved and blocked. Ordinary switchTo would capture
    // it again and could overwrite a completion arriving after publication.
    if (!activateGenerationLockedWithRegisters(x86_platform, false, thread, generation)) return false;
    // Wake/wait returns through the common user-return guard after XRSTOR.
    // Its kernel-only retirement and signal staging need metadata, not the
    // receiver's live bases. Installing them here repeats privileged writes;
    // standalone/generic loads and rollback must retain their eager contract.
    return loadContextIntoFrameGenerationLockedMode(x86_platform, false, thread, generation, frame);
}

pub fn resumeAfterWakeWaitInstallFailure(frame: *TrapFrame) void {
    // The caller has already saved its frame and become blocked. Reusing the
    // ordinary switch/block path here would capture it again or block the idle
    // identity. The futex frontend retires its peer and drops all locks first.
    if (!isBootstrapSchedulerCpu()) smp.returnCurrentApToIdleFromInterrupt();
    loadNextReadyThreadOrIdle(frame);
}

fn tryWakeWaitTransferLocked(comptime Registers: type, comptime install: anytype, frame: *TrapFrame, request: WakeWaitTransfer) ?bool {
    if (!verifiedCoreReady() or (!isBootstrapSchedulerCpu() and !apUserDispatchReady())) return null;
    const cpu = currentCpu();
    const source_id = currentThread();
    if (request.peer == source_id or request.peer_token == 0 or request.caller_token == 0) return null;
    const source_ctx = threadContextMutable(source_id) orelse return null;
    const peer = threadContextMutable(request.peer) orelse return null;
    if (!source_ctx.allocated or source_ctx.owner_process != request.owner or
        source_ctx.cpu_slot != cpu or !source_ctx.ready or source_ctx.stopped or
        !source_ctx.wait_mailbox or source_ctx.active_wait_token != request.caller_token or
        source_ctx.wait_completion_claimed or source_ctx.wait_completion_publish_pending) return null;
    if (firstUnblockedPendingSignal(source_ctx) != 0 and source_ctx.signal_entry != 0 and
        !source_ctx.signal_interrupt_consumed) return null;
    if (!peer.allocated or peer.owner_process != request.owner or peer.generation != request.peer_generation or
        peer.active_wait_token != request.peer_token or peer.wait_completion_claimed or
        peer.ready or !peer.wait_mailbox or peer.stopped or !threadAllowsCpu(peer, cpu)) return null;
    const target = peer.scheduler_entity orelse return null;
    const last_cpu = target.cpu_slot;
    if (peer.cpu_slot != last_cpu) return null;
    const state = schedulerStateForSlot(cpu) orelse return null;
    const last = schedulerStateForSlot(last_cpu) orelse return null;
    const first = if (cpu <= last_cpu) state else last;
    const second = if (cpu <= last_cpu) last else state;
    var awakened: verified_sched.Entity = undefined;
    const committed = commit: {
        if (comptime perf.enabled) perf.handoffAdd(.lock_pairs, 1);
        const cpu_wait_start = perf.timestamp();
        first.lock.lock();
        if (second != first) second.lock.lock();
        if (comptime perf.enabled) perf.handoffElapsed(.cpu_wait_cycles, cpu_wait_start);
        defer {
            if (second != first) second.lock.unlock();
            first.lock.unlock();
        }
        const rq_wait_start = perf.timestamp();
        first.runqueue_lock.lock();
        if (second != first) second.runqueue_lock.lock();
        if (comptime perf.enabled) perf.handoffElapsed(.rq_wait_cycles, rq_wait_start);
        defer {
            if (second != first) second.runqueue_lock.unlock();
            first.runqueue_lock.unlock();
        }
        const scalar_start = perf.timestamp();
        const source = state.current_entity orelse break :commit false;
        if (!state.enabled or state.is_idle or state.current_thread != source_id or
            state.runqueue.count != 0 or source_ctx.scheduler_entity != source or
            !nodeMatches(source, source_id, source_ctx.generation, cpu) or
            source.ownership != .running or source.linked_tree != null or source.parent != null or
            source.left != null or source.right != null or
            !nodeMatches(target, request.peer, request.peer_generation, last_cpu) or
            target.ownership != .blocked or target.linked_tree != null or target.parent != null or
            target.left != null or target.right != null) break :commit false;
        // A remote CPU may still be finishing the peer's previous block tail;
        // scalar blocked ownership alone cannot grant us its saved frame.
        if (!last.is_idle and last.current_thread == request.peer) break :commit false;
        var running: verified_sched.Entity = undefined;
        var blocked: verified_sched.Entity = undefined;
        if (scalarIssue("wake-wait-wake", verified_sched.pacha_eevdf_entity_wake(&target.entity, cpuRunqueueFloor(state), &awakened), request.peer) or
            scalarIssue("wake-wait-run", verified_sched.pacha_eevdf_entity_mark_running(&awakened, &running), request.peer) or
            scalarIssue("wake-wait-block", verified_sched.pacha_eevdf_entity_block(&source.entity, &blocked), source_id)) break :commit false;

        // All rejection points precede capture. Record partial completion in
        // both live and saved caller frames before installing any peer frame.
        frame.rdx = 1;
        var saved = frame.*;
        saved.rax = request.resume_rax;
        // Successful scalar validation is distinct from hardware capture;
        // neither interval is inferred by subtracting another inclusive timer.
        if (comptime perf.enabled) {
            perf.handoffElapsed(.scalar_cycles, scalar_start);
            perf.handoffAdd(.scalar_calls, 1);
        }
        const capture_start = perf.timestamp();
        saveCurrentThreadContextFromFrameLocked(Registers, &saved);
        if (comptime perf.enabled) {
            perf.handoffElapsed(.capture_cycles, capture_start);
            perf.handoffAdd(.capture_calls, 1);
        }
        const publication_start = perf.timestamp();
        source_ctx.ready = false;
        source_ctx.wait_completion_claimed = false;
        source_ctx.wait_completion_publish_pending = false;
        source_ctx.wait_completion_rax = 0;
        source_ctx.wake_tick = if (request.timeout_ticks == 0) 0 else lapic_tick_count +| request.timeout_ticks;
        setThreadHotWaitState(source_id, true, source_ctx.wake_tick, false);
        setThreadHotCompletionClaimed(source_id, false);
        source.entity = blocked;
        source.ownership = .blocked;

        target.entity = running;
        target.ownership = .running;
        target.cpu_slot = cpu;
        state.current_entity = target;
        peer.cpu_slot = cpu;
        peer.frame.rax = 0;
        peer.wait_mailbox = false;
        peer.active_wait_token = 0;
        peer.wake_tick = 0;
        peer.ready = true;
        setThreadHotWaitState(request.peer, false, 0, true);
        if (comptime perf.enabled) {
            perf.handoffElapsed(.publication_cycles, publication_start);
            perf.handoffAdd(.publication_calls, 1);
        }
        break :commit true;
    };
    if (!committed) return null;
    const install_start = perf.timestamp();
    const installed = install(request.peer, request.peer_generation, frame);
    // Installation may replace the live frame. Only its result and earlier
    // local timestamp are used by the probe, never the receiver's ABI inputs.
    if (comptime perf.enabled) {
        perf.handoffElapsed(.install_cycles, install_start);
        perf.handoffAdd(.install_calls, 1);
        perf.handoffAdd(if (installed) .installed else .install_failed, 1);
    }
    if (installed) return true;

    // The wake is irrevocable; an install failure must not make the blocked
    // caller runnable or send the peer's wake twice. The peer has executed no
    // user instructions, so retain its original awakened scalar entity.
    state.lock.lock();
    defer state.lock.unlock();
    state.runqueue_lock.lock();
    defer state.runqueue_lock.unlock();
    const source = source_ctx.scheduler_entity orelse @panic("wake-wait rollback lost source");
    if (source.ownership != .blocked or source_ctx.ready or
        state.current_entity != target or target.ownership != .running or !peer.ready or
        !nodeMatches(target, request.peer, request.peer_generation, cpu))
        @panic("wake-wait rollback ownership uncertain");
    target.entity = awakened;
    target.ownership = .runnable;
    if (!state.runqueue.insert(target)) @panic("wake-wait rollback could not publish receiver");
    state.current_entity = null;
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
    refreshCpuVirtualTime(state);
    return false;
}

pub fn wakeIfWaitingTokenGenerationWithRaxAndHandoff(
    frame: *TrapFrame,
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
    sender_rax: u64,
) KeyedWakeHandoffResult {
    return wakeIfWaitingTokenAndHandoffWithInstall(switchToGenerationLocked, frame, thread_index, generation, owner, wait_token, resume_rax, sender_rax);
}

fn wakeIfWaitingTokenAndHandoffWithInstall(
    comptime install: anytype,
    frame: *TrapFrame,
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
    sender_rax: u64,
) KeyedWakeHandoffResult {
    const result = wake: {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        const cpu = currentCpu();
        const preferred = if (schedulerStateForSlot(cpu)) |state| blk: {
            state.runqueue_lock.lock();
            const uncontended = state.current_entity != null and state.runqueue.count == 0;
            state.runqueue_lock.unlock();
            break :blk if (uncontended) cpu else null;
        } else null;
        if (preferred != null) {
            if (tryFusedKeyedWakeLocked(install, frame, thread_index, generation, owner, wait_token, resume_rax, sender_rax)) |handed_off| {
                return .{ .wake = .woke, .handed_off = handed_off };
            }
        }
        // A miss has not consumed the wait. Preserve ordinary early-wake,
        // stopped-thread and failed-publication behavior. Fusion's completed
        // wake with a failed handoff returned above must never be sent again.
        break :wake wakeIfWaitingTokenGenerationLocked(thread_index, generation, owner, wait_token, resume_rax, preferred);
    };
    if (result != .woke) return .{ .wake = result };
    // A contended ordinary wake must not extend its global table critical
    // section through context capture/installation. Reacquire with only the
    // value identity; local handoff revalidates owner/generation/CPU/affinity
    // and sole runnable ownership rather than retaining a context pointer.
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return .{ .wake = result, .handed_off = handoffOnlyReadyLocked(install, frame, thread_index, generation, owner, sender_rax) };
}

fn tryFusedKeyedWakeLocked(
    comptime install: anytype,
    frame: *TrapFrame,
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
    sender_rax: u64,
) ?bool {
    if (!verifiedCoreReady() or (!isBootstrapSchedulerCpu() and !apUserDispatchReady())) return null;
    const cpu = currentCpu();
    const sender_id = currentThread();
    if (thread_index == sender_id or wait_token == 0) return null;
    const sender = threadContext(sender_id) orelse return null;
    const peer = threadContextMutable(thread_index) orelse return null;
    if (!sender.allocated or sender.owner_process != owner or sender.cpu_slot != cpu or !sender.ready or sender.stopped) return null;
    if (!peer.allocated or peer.owner_process != owner or peer.generation != generation or
        peer.active_wait_token != wait_token or peer.wait_completion_claimed or
        peer.ready or !peer.wait_mailbox or peer.stopped or !threadAllowsCpu(peer, cpu)) return null;
    const target = peer.scheduler_entity orelse return null;
    const last_cpu = target.cpu_slot;
    if (peer.cpu_slot != last_cpu) return null;
    const state = schedulerStateForSlot(cpu) orelse return null;
    const last = schedulerStateForSlot(last_cpu) orelse return null;
    const first = if (cpu <= last_cpu) state else last;
    const second = if (cpu <= last_cpu) last else state;
    const sender_generation = sender.generation;
    const committed = commit: {
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
        const source = state.current_entity orelse break :commit false;
        // Recheck the empty destination at the ownership transition. A
        // preference sampled earlier must never bypass a queued competitor.
        if (!state.enabled or state.is_idle or state.current_thread != sender_id or
            state.runqueue.count != 0 or !nodeMatches(source, sender_id, sender_generation, cpu) or
            source.ownership != .running or !nodeMatches(target, thread_index, generation, last_cpu) or
            target.ownership != .blocked or target.linked_tree != null or target.parent != null or
            target.left != null or target.right != null) break :commit false;
        // The previous CPU may still execute the outgoing block tail even
        // after scalar ownership became blocked. Its saved frame is not yet
        // ours to install on a second CPU.
        if (!last.is_idle and last.current_thread == thread_index) break :commit false;
        var awakened: verified_sched.Entity = undefined;
        var running: verified_sched.Entity = undefined;
        var finished_source: verified_sched.Entity = undefined;
        if (scalarIssue("fused-wake", verified_sched.pacha_eevdf_entity_wake(&target.entity, cpuRunqueueFloor(state), &awakened), thread_index) or
            scalarIssue("fused-run", verified_sched.pacha_eevdf_entity_mark_running(&awakened, &running), thread_index) or
            scalarIssue("fused-finish", verified_sched.pacha_eevdf_entity_finish(&source.entity, &finished_source), sender_id)) break :commit false;
        const old_source = source.entity;
        source.entity = finished_source;
        source.ownership = .runnable;
        if (!state.runqueue.insert(source)) {
            source.entity = old_source;
            source.ownership = .running;
            break :commit false;
        }
        // No target tree insertion/removal is needed: this sole receiver
        // transitions blocked -> runnable -> running under the same locks.
        target.entity = running;
        target.ownership = .running;
        target.cpu_slot = cpu;
        state.current_entity = target;
        peer.cpu_slot = cpu;
        peer.frame.rax = resume_rax;
        peer.wait_mailbox = false;
        peer.active_wait_token = 0;
        peer.wake_tick = 0;
        peer.ready = true;
        setThreadHotWaitState(thread_index, false, 0, true);
        break :commit true;
    };
    if (!committed) return null;
    if (install(thread_index, generation, frame, sender_rax)) return true;
    rollbackLocalHandoffLocked(cpu, sender_id, sender_generation, thread_index, generation);
    return false;
}

fn handoffOnlyReadyLocked(comptime install: anytype, frame: *TrapFrame, target_id: usize, generation: u32, owner: kernel.PrincipalId, sender_rax: u64) bool {
    if (!verifiedCoreReady() or (!isBootstrapSchedulerCpu() and !apUserDispatchReady())) return false;
    const cpu = currentCpu();
    const sender_id = currentThread();
    if (sender_id == target_id) return false;
    const source = threadContext(sender_id) orelse return false;
    const target = threadContext(target_id) orelse return false;
    if (!source.allocated or source.cpu_slot != cpu or source.owner_process != owner or
        !target.allocated or target.owner_process != owner or target.generation != generation or
        !target.ready or target.cpu_slot != cpu or !threadAllowsCpu(target, cpu)) return false;
    const sender_generation = source.generation;
    if (!verifiedHandoffToThreadOnCpu(true, cpu, target_id, generation, sender_id, sender_generation)) return false;
    if (install(target_id, generation, frame, sender_rax)) return true;
    rollbackLocalHandoffLocked(cpu, sender_id, sender_generation, target_id, generation);
    return false;
}

fn rollbackLocalHandoffLocked(cpu: usize, source_id: usize, source_generation: u32, target_id: usize, target_generation: u32) void {
    // A wake has already committed. If its context transfer fails, preserve
    // that wake as runnable, but never report success with uncertain ownership.
    // These checks must remain active in release builds, not debug asserts.
    if (!verifiedHandoffToThreadOnCpu(false, cpu, source_id, source_generation, target_id, target_generation))
        @panic("keyed wake handoff ownership rollback failed");
    const state = schedulerStateForSlot(cpu) orelse @panic("keyed wake rollback lost CPU");
    const source = threadContext(source_id) orelse @panic("keyed wake rollback lost source");
    const target = threadContext(target_id) orelse @panic("keyed wake rollback lost target");
    state.lock.lock();
    defer state.lock.unlock();
    state.runqueue_lock.lock();
    defer state.runqueue_lock.unlock();
    const source_node = source.scheduler_entity orelse @panic("keyed wake rollback lost source entity");
    const target_node = target.scheduler_entity orelse @panic("keyed wake rollback lost target entity");
    if (state.current_thread != source_id or state.current_principal != source.owner_process or state.current_cr3 != source.cr3 or
        state.current_entity != source_node or !nodeMatches(source_node, source_id, source_generation, cpu) or
        source_node.ownership != .running or source_node.linked_tree != null or
        !nodeMatches(target_node, target_id, target_generation, cpu) or target_node.ownership != .runnable or
        !target.ready or !state.runqueue.contains(target_node)) @panic("keyed wake handoff rollback postcondition failed");
}

pub const WaitTokenClaimResult = enum {
    claimed,
    stale,
};

const FusedWakeTestInstaller = struct {
    var fail: bool = false;
    var calls: usize = 0;
    fn install(thread: usize, generation: u32, frame: *TrapFrame, rax: ?u64) bool {
        calls += 1;
        std.debug.assert(scheduler_state.thread_table.lock_state.value == 1);
        for (scheduler_state.cpus[0..2]) |*state| {
            std.debug.assert(state.lock.value == 0 and state.runqueue_lock.value == 0);
        }
        if (fail) return false;
        return switchToGenerationLockedWithRegisters(ContextTransferTestRegisters, thread, generation, frame, rax);
    }
};

fn initializeFusedWakeTest(peer_cpu: usize) !void {
    initializeStaticStorage();
    scheduler_state.cpu_count = 2;
    scheduler_state.cpus[1].enabled = true;
    for (4..7) |index| {
        initial_thread_contexts[index] = .{ .id = @intCast(index), .scheduler_entity = &initial_scheduler_entities[index] };
        syncHotStateFromContext(index);
    }
    const source = &initial_thread_contexts[4];
    source.* = .{ .id = 4, .allocated = true, .ready = true, .cr3 = 0x1000, .scheduler_entity = &initial_scheduler_entities[4] };
    try std.testing.expect(verifiedAddThread(4, 1, true));
    try std.testing.expectEqual(@as(usize, 4), verifiedPickThreadForCpu(0).?.thread_index);
    scheduler_state.cpus[0].current_thread = 4;
    scheduler_state.cpus[0].current_principal = source.owner_process;
    scheduler_state.cpus[0].current_cr3 = source.cr3;
    scheduler_state.cpus[0].is_idle = false;
    const peer = &initial_thread_contexts[5];
    peer.* = .{
        .id = 5,
        .generation = 17,
        .allocated = true,
        .cpu_slot = peer_cpu,
        .cpu_affinity_mask = 3,
        .wait_mailbox = true,
        .active_wait_token = 11,
        .wake_tick = 100,
        .cr3 = 0x1000,
        .fs_base = 0x3333,
        .gs_base = 0x4444,
        .pkru = 0x80000000,
        .scheduler_entity = &initial_scheduler_entities[5],
    };
    peer.frame.rip = 0x8888;
    peer.frame.rax = 77;
    try std.testing.expect(verifiedAddThread(5, 17, false));
    syncHotStateFromContext(4);
    syncHotStateFromContext(5);
    FusedWakeTestInstaller.fail = false;
    FusedWakeTestInstaller.calls = 0;
    ContextTransferTestRegisters.reset();
}

fn expectFusedWakeNodeUnchanged(expected: scheduler_runqueue.Node, actual: scheduler_runqueue.Node) !void {
    // Deep comparison follows pointer links back into the tree indefinitely;
    // links must retain their identity, not recursively compare their targets.
    inline for (std.meta.fields(scheduler_runqueue.Node)) |field| {
        if (comptime std.mem.eql(u8, field.name, "entity")) {
            try std.testing.expectEqualDeep(@field(expected, field.name), @field(actual, field.name));
        } else {
            try std.testing.expectEqual(@field(expected, field.name), @field(actual, field.name));
        }
    }
}

const WakeWaitTestInstaller = struct {
    var failure: u8 = 0;
    var calls: usize = 0;
    fn install(thread: usize, generation: u32, frame: *TrapFrame) bool {
        calls += 1;
        std.debug.assert(scheduler_state.thread_table.lock_state.value == 1);
        for (scheduler_state.cpus[0..2]) |*state|
            std.debug.assert(state.lock.value == 0 and state.runqueue_lock.value == 0);
        std.debug.assert(!initial_thread_contexts[4].ready and
            initial_scheduler_entities[4].ownership == .blocked and
            initial_thread_contexts[4].frame.rdx == 1);
        if (failure == 1) return false;
        if (!activateGenerationLockedWithRegisters(ContextTransferTestRegisters, false, thread, generation)) return false;
        if (failure == 2) return false;
        return loadContextIntoFrameGenerationLockedMode(ContextTransferTestRegisters, false, thread, generation, frame);
    }
};

fn initializeWakeWaitTest(peer_cpu: usize) !WakeWaitTransfer {
    try initializeFusedWakeTest(peer_cpu);
    const token = reserveCurrentWaitToken(1) orelse return error.ReservationFailed;
    initial_thread_contexts[5].frame.rdx = 0xbeef;
    WakeWaitTestInstaller.failure = 0;
    WakeWaitTestInstaller.calls = 0;
    return .{ .peer = 5, .peer_generation = 17, .owner = default_process_principal, .peer_token = 11, .caller_token = token, .timeout_ticks = 0, .resume_rax = 2 };
}

fn expectWakeWaitContextUnchanged(expected: ThreadContext, actual: ThreadContext) !void {
    // Deliberately invalid entity links in rejection tests may be cyclic.
    // Compare the context's node identity here and its value fields separately.
    inline for (std.meta.fields(ThreadContext)) |field| {
        if (comptime std.mem.eql(u8, field.name, "scheduler_entity")) {
            try std.testing.expectEqual(@field(expected, field.name), @field(actual, field.name));
        } else {
            try std.testing.expectEqualDeep(@field(expected, field.name), @field(actual, field.name));
        }
    }
}

test "futex wake wait transfers directly without runnable sender and captures bases once" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    for (0..2) |old_cpu| {
        const request = try initializeWakeWaitTest(old_cpu);
        const old_source = initial_scheduler_entities[4].entity;
        var blocked: verified_sched.Entity = undefined;
        try std.testing.expectEqual(.ok, verified_sched.pacha_eevdf_entity_block(&old_source, &blocked));
        var frame = std.mem.zeroes(TrapFrame);
        frame.rax = 901;
        frame.rdx = std.math.maxInt(u64);
        frame.rip = 0x9999;
        scheduler_state.thread_table.lock();
        const result = tryWakeWaitTransferLocked(ContextTransferTestRegisters, WakeWaitTestInstaller.install, &frame, request);
        scheduler_state.thread_table.unlock();
        try std.testing.expectEqual(@as(?bool, true), result);
        try std.testing.expectEqual(@as(usize, 1), WakeWaitTestInstaller.calls);
        try std.testing.expectEqual(@as(usize, 0), scheduler_state.cpus[0].runqueue.count);
        try std.testing.expectEqual(.blocked, initial_scheduler_entities[4].ownership);
        try std.testing.expectEqualDeep(blocked, initial_scheduler_entities[4].entity);
        try std.testing.expect(!initial_thread_contexts[4].ready);
        try std.testing.expect(initial_thread_contexts[4].wait_mailbox);
        try std.testing.expectEqual(request.caller_token, initial_thread_contexts[4].active_wait_token);
        try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[4].frame.rdx);
        try std.testing.expectEqual(@as(u64, 2), initial_thread_contexts[4].frame.rax);
        try std.testing.expectEqual(@as(u64, 0x9999), initial_thread_contexts[4].frame.rip);
        try std.testing.expectEqual(.running, initial_scheduler_entities[5].ownership);
        try std.testing.expectEqual(@as(usize, 5), currentThread());
        try std.testing.expectEqual(@as(u64, 0), frame.rax);
        try std.testing.expectEqual(@as(u64, 0xbeef), frame.rdx);
        try std.testing.expectEqual(@as(u64, 0x8888), frame.rip);
        try std.testing.expectEqual([3]usize{ 1, 1, 1 }, ContextTransferTestRegisters.reads);
        try std.testing.expectEqual([3]usize{ 0, 0, 0 }, ContextTransferTestRegisters.writes);
        try std.testing.expectEqual(@as(u64, 0x1111), ContextTransferTestRegisters.fs);
        try std.testing.expectEqual(@as(u64, 0x2222), ContextTransferTestRegisters.gs);
        // Signal-frame staging runs before common user return and must not
        // recapture the receiver from the still-live source hardware bases.
        initial_thread_contexts[5].signal_entry = 0x7777;
        initial_thread_contexts[5].pending_signal_mask = signalBit(12).?;
        const signal = claimCurrentSignalForUserReturn(frame.rip) orelse return error.SignalNotClaimed;
        try std.testing.expectEqual(@as(usize, 5), signal.thread_index);
        try std.testing.expectEqual([3]usize{ 1, 1, 1 }, ContextTransferTestRegisters.reads);
        try std.testing.expectEqual([3]usize{ 0, 0, 0 }, ContextTransferTestRegisters.writes);
        // Model the guard after XState restore, including any restored PKRU.
        ContextTransferTestRegisters.pkru = 0;
        try std.testing.expect(applyThreadBasesWithRegisters(ContextTransferTestRegisters, 5));
        try std.testing.expectEqual([3]usize{ 1, 1, 1 }, ContextTransferTestRegisters.writes);
        try std.testing.expectEqual(initial_thread_contexts[5].fs_base, ContextTransferTestRegisters.fs);
        try std.testing.expectEqual(initial_thread_contexts[5].gs_base, ContextTransferTestRegisters.gs);
        try std.testing.expectEqual(initial_thread_contexts[5].pkru, ContextTransferTestRegisters.pkru);
        // Completion changes RAX only; partial-wake result belongs to the
        // caller frame and must survive its later wake, not the peer's frame.
        try std.testing.expectEqual(.woke, wakeIfWaitingTokenGenerationWithRax(4, 1, request.owner, request.caller_token, 0));
        try std.testing.expectEqual(@as(u64, 0), initial_thread_contexts[4].frame.rax);
        try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[4].frame.rdx);
    }
}

test "futex wake wait rejects early completion signals competitors stale identity and block tail" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    for (0..24) |scenario| {
        var request = try initializeWakeWaitTest(1);
        const source = &initial_thread_contexts[4];
        const peer = &initial_thread_contexts[5];
        switch (scenario) {
            0 => request.caller_token += 1,
            1 => source.wait_mailbox = false,
            2 => source.wait_completion_claimed = true,
            3 => source.wait_completion_publish_pending = true,
            4 => {
                source.pending_signal_mask = 4;
                source.signal_entry = 0x1234;
            },
            5 => source.stopped = true,
            6 => source.owner_process = kernel.processPrincipalFromIndex(1).?,
            7 => peer.generation += 1,
            8 => peer.active_wait_token += 1,
            9 => peer.wait_completion_claimed = true,
            10 => peer.stopped = true,
            11 => peer.ready = true,
            12 => peer.wait_mailbox = false,
            13 => peer.cpu_affinity_mask = 2,
            14 => {
                scheduler_state.cpus[1].is_idle = false;
                scheduler_state.cpus[1].current_thread = 5;
            },
            15 => {
                initial_thread_contexts[6].allocated = true;
                initial_thread_contexts[6].ready = true;
                try std.testing.expect(verifiedAddThread(6, 1, true));
            },
            16 => initial_scheduler_entities[4].parent = &initial_scheduler_entities[4],
            17 => initial_scheduler_entities[5].parent = &initial_scheduler_entities[5],
            18 => scheduler_state.cpus[0].enabled = false,
            19 => source.cpu_slot = 1,
            20 => peer.cpu_slot = 0,
            21 => source.ready = false,
            22 => peer.allocated = false,
            23 => source.scheduler_entity = &initial_scheduler_entities[6],
            else => unreachable,
        }
        const source_before = source.*;
        const peer_before = peer.*;
        const source_node = initial_scheduler_entities[4];
        const peer_node = initial_scheduler_entities[5];
        const count_before = scheduler_state.cpus[0].runqueue.count;
        var frame = std.mem.zeroes(TrapFrame);
        frame.rdx = std.math.maxInt(u64);
        const frame_before = frame;
        scheduler_state.thread_table.lock();
        const result = tryWakeWaitTransferLocked(ContextTransferTestRegisters, WakeWaitTestInstaller.install, &frame, request);
        scheduler_state.thread_table.unlock();
        try std.testing.expect(result == null);
        try expectWakeWaitContextUnchanged(source_before, source.*);
        try expectWakeWaitContextUnchanged(peer_before, peer.*);
        try expectFusedWakeNodeUnchanged(source_node, initial_scheduler_entities[4]);
        try expectFusedWakeNodeUnchanged(peer_node, initial_scheduler_entities[5]);
        try std.testing.expectEqualDeep(frame_before, frame);
        try std.testing.expectEqual(count_before, scheduler_state.cpus[0].runqueue.count);
        try std.testing.expectEqual(@as(usize, 0), WakeWaitTestInstaller.calls);
        try std.testing.expectEqual([3]usize{ 0, 0, 0 }, ContextTransferTestRegisters.reads);
    }
}

test "futex wake wait install failure keeps source blocked and committed receiver runnable" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    for (1..3) |failure| {
        var request = try initializeWakeWaitTest(1);
        request.timeout_ticks = 12;
        request.resume_rax = 5;
        var expected_peer: verified_sched.Entity = undefined;
        try std.testing.expectEqual(.ok, verified_sched.pacha_eevdf_entity_wake(&initial_scheduler_entities[5].entity, cpuRunqueueFloor(&scheduler_state.cpus[0]), &expected_peer));
        WakeWaitTestInstaller.failure = @intCast(failure);
        var frame = std.mem.zeroes(TrapFrame);
        frame.rdx = std.math.maxInt(u64);
        scheduler_state.thread_table.lock();
        const result = tryWakeWaitTransferLocked(ContextTransferTestRegisters, WakeWaitTestInstaller.install, &frame, request);
        scheduler_state.thread_table.unlock();
        try std.testing.expectEqual(@as(?bool, false), result);
        try std.testing.expectEqual(@as(usize, 1), WakeWaitTestInstaller.calls);
        try std.testing.expectEqual(.blocked, initial_scheduler_entities[4].ownership);
        try std.testing.expect(!initial_thread_contexts[4].ready);
        try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[4].frame.rdx);
        try std.testing.expectEqual(@as(u64, 5), initial_thread_contexts[4].frame.rax);
        try std.testing.expectEqual(lapic_tick_count +| 12, initial_thread_contexts[4].wake_tick);
        try std.testing.expectEqual(.runnable, initial_scheduler_entities[5].ownership);
        try std.testing.expectEqualDeep(expected_peer, initial_scheduler_entities[5].entity);
        try std.testing.expect(initial_thread_contexts[5].ready and !initial_thread_contexts[5].wait_mailbox);
        try std.testing.expectEqual(@as(u64, 0), initial_thread_contexts[5].active_wait_token);
        try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[0].runqueue.count);
        try std.testing.expect(scheduler_state.cpus[0].runqueue.contains(&initial_scheduler_entities[5]));
        try std.testing.expect(scheduler_state.cpus[0].runqueue.validate(0));
        try std.testing.expect(scheduler_state.cpus[0].is_idle and scheduler_state.cpus[0].current_entity == null);
        try std.testing.expectEqual(scheduler_state.cpus[0].idle_thread, scheduler_state.cpus[0].current_thread);
        try std.testing.expect(scheduler_state.cpus[0].current_principal == null);
        try std.testing.expectEqual(@as(u64, 0), scheduler_state.cpus[0].current_cr3);
        try std.testing.expectEqual([3]usize{ 1, 1, 1 }, ContextTransferTestRegisters.reads);
        try std.testing.expectEqual([3]usize{ 0, 0, 0 }, ContextTransferTestRegisters.writes);
    }
}

test "futex fused wake installs sole receiver from local or remote blocked CPU" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    for (0..2) |old_cpu| {
        try initializeFusedWakeTest(old_cpu);
        const old_peer = initial_scheduler_entities[5].entity;
        var awake: verified_sched.Entity = undefined;
        var expected: verified_sched.Entity = undefined;
        try std.testing.expectEqual(.ok, verified_sched.pacha_eevdf_entity_wake(&old_peer, cpuRunqueueFloor(&scheduler_state.cpus[0]), &awake));
        try std.testing.expectEqual(.ok, verified_sched.pacha_eevdf_entity_mark_running(&awake, &expected));
        var frame = std.mem.zeroes(TrapFrame);
        frame.rax = 901;
        frame.rip = 0x9999;
        const result = wakeIfWaitingTokenAndHandoffWithInstall(FusedWakeTestInstaller.install, &frame, 5, 17, default_process_principal, 11, 0, 1);
        try std.testing.expectEqualDeep(KeyedWakeHandoffResult{ .wake = .woke, .handed_off = true }, result);
        try std.testing.expectEqual(@as(u8, 0), scheduler_state.thread_table.lock_state.value);
        try std.testing.expectEqual(@as(usize, 1), FusedWakeTestInstaller.calls);
        try std.testing.expectEqualDeep(expected, initial_scheduler_entities[5].entity);
        try std.testing.expectEqual(.running, initial_scheduler_entities[5].ownership);
        try std.testing.expect(initial_scheduler_entities[5].linked_tree == null);
        try std.testing.expectEqual(.runnable, initial_scheduler_entities[4].ownership);
        try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[0].runqueue.count);
        try std.testing.expect(scheduler_state.cpus[0].runqueue.contains(&initial_scheduler_entities[4]));
        try std.testing.expect(scheduler_state.cpus[0].runqueue.validate(0));
        try std.testing.expectEqual(@as(usize, 5), currentThread());
        try std.testing.expectEqual(@as(usize, 0), initial_thread_contexts[5].cpu_slot);
        try std.testing.expectEqual(@as(u64, 0), frame.rax);
        try std.testing.expectEqual(@as(u64, 0x8888), frame.rip);
        try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[4].frame.rax);
        try std.testing.expectEqual(@as(u64, 0x9999), initial_thread_contexts[4].frame.rip);
        try std.testing.expectEqual(@as(u64, 0), initial_thread_contexts[5].active_wait_token);
        try std.testing.expect(!initial_thread_contexts[5].wait_mailbox and initial_thread_contexts[5].ready);
        try std.testing.expectEqual(@as(u64, 0), initial_thread_contexts[5].wake_tick);
        try std.testing.expectEqual([3]usize{ 1, 1, 1 }, ContextTransferTestRegisters.reads);
        try std.testing.expectEqual([3]usize{ 1, 1, 1 }, ContextTransferTestRegisters.writes);
        try std.testing.expectEqual(@as(u64, 0x1111), initial_thread_contexts[4].fs_base);
        try std.testing.expectEqual(@as(u64, 0x3333), ContextTransferTestRegisters.fs);
    }
}

test "futex fused rejection leaves peer wait and entity unchanged" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    for (0..17) |scenario| {
        try initializeFusedWakeTest(1);
        const source = &initial_thread_contexts[4];
        const peer = &initial_thread_contexts[5];
        switch (scenario) {
            0 => peer.generation += 1,
            1 => peer.owner_process = kernel.processPrincipalFromIndex(1).?,
            2 => peer.active_wait_token += 1,
            3 => peer.wait_completion_claimed = true,
            4 => peer.ready = true,
            5 => peer.stopped = true,
            6 => peer.wait_mailbox = false,
            7 => peer.cpu_affinity_mask = 2,
            8 => {
                scheduler_state.cpus[1].is_idle = false;
                scheduler_state.cpus[1].current_thread = 5;
            },
            9 => {
                initial_thread_contexts[6].allocated = true;
                initial_thread_contexts[6].ready = true;
                try std.testing.expect(verifiedAddThread(6, 1, true));
            },
            10 => source.owner_process = kernel.processPrincipalFromIndex(1).?,
            11 => source.cpu_slot = 1,
            12 => source.stopped = true,
            13 => initial_scheduler_entities[5].ownership = .runnable,
            14 => scheduler_state.cpus[0].enabled = false,
            15 => initial_scheduler_entities[4].parent = &initial_scheduler_entities[4],
            16 => peer.allocated = false,
            else => unreachable,
        }
        syncHotStateFromContext(5);
        const peer_before = peer.*;
        const peer_entity_before = initial_scheduler_entities[5];
        const source_before = initial_scheduler_entities[4];
        const count_before = scheduler_state.cpus[0].runqueue.count;
        var frame = std.mem.zeroes(TrapFrame);
        const frame_before = frame;
        scheduler_state.thread_table.lock();
        const result = tryFusedKeyedWakeLocked(FusedWakeTestInstaller.install, &frame, 5, 17, default_process_principal, 11, 0, 1);
        scheduler_state.thread_table.unlock();
        try std.testing.expect(result == null);
        try std.testing.expectEqualDeep(peer_before, peer.*);
        try expectFusedWakeNodeUnchanged(peer_entity_before, initial_scheduler_entities[5]);
        try expectFusedWakeNodeUnchanged(source_before, initial_scheduler_entities[4]);
        try std.testing.expectEqual(count_before, scheduler_state.cpus[0].runqueue.count);
        try std.testing.expectEqualDeep(frame_before, frame);
        try std.testing.expectEqual(@as(usize, 0), FusedWakeTestInstaller.calls);
    }
}

test "futex fused context failure retains completed wake and restores sender ownership" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    try initializeFusedWakeTest(1);
    FusedWakeTestInstaller.fail = true;
    var frame = std.mem.zeroes(TrapFrame);
    frame.rax = 81;
    const result = wakeIfWaitingTokenAndHandoffWithInstall(FusedWakeTestInstaller.install, &frame, 5, 17, default_process_principal, 11, 0, 1);
    try std.testing.expectEqualDeep(KeyedWakeHandoffResult{ .wake = .woke }, result);
    try std.testing.expectEqual(@as(usize, 1), FusedWakeTestInstaller.calls);
    try std.testing.expectEqual(@as(usize, 4), currentThread());
    try std.testing.expectEqual(@as(u64, 81), frame.rax);
    try std.testing.expectEqual(.running, initial_scheduler_entities[4].ownership);
    try std.testing.expectEqual(.runnable, initial_scheduler_entities[5].ownership);
    try std.testing.expect(scheduler_state.cpus[0].runqueue.contains(&initial_scheduler_entities[5]));
    try std.testing.expect(scheduler_state.cpus[0].runqueue.validate(0));
    try std.testing.expect(initial_thread_contexts[5].ready and !initial_thread_contexts[5].wait_mailbox);
    try std.testing.expectEqual(@as(u64, 0), initial_thread_contexts[5].active_wait_token);
    try std.testing.expectEqual([3]usize{ 0, 0, 0 }, ContextTransferTestRegisters.writes);
}

test "futex fused ordinary fallback preserves competitors stopped early and publication failure" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    for (0..4) |scenario| {
        try initializeFusedWakeTest(1);
        const peer = &initial_thread_contexts[5];
        switch (scenario) {
            0 => {
                initial_thread_contexts[6].allocated = true;
                initial_thread_contexts[6].ready = true;
                try std.testing.expect(verifiedAddThread(6, 1, true));
            },
            1 => peer.stopped = true,
            2 => {
                peer.ready = true;
                try std.testing.expect(verifiedWakeThreadGeneration(5, 17));
            },
            3 => {
                scheduler_state.cpus[0].enabled = false;
                scheduler_state.cpus[1].enabled = false;
            },
            else => unreachable,
        }
        syncHotStateFromContext(5);
        const before = peer.*;
        var frame = std.mem.zeroes(TrapFrame);
        const result = wakeIfWaitingTokenAndHandoffWithInstall(FusedWakeTestInstaller.install, &frame, 5, 17, default_process_principal, 11, 0, 1);
        try std.testing.expect(!result.handed_off);
        try std.testing.expectEqual(@as(usize, 4), currentThread());
        try std.testing.expectEqual(@as(usize, 0), FusedWakeTestInstaller.calls);
        if (scenario == 3) {
            try std.testing.expectEqual(.publish_failed, result.wake);
            try std.testing.expectEqualDeep(before, peer.*);
        } else {
            try std.testing.expectEqual(.woke, result.wake);
            try std.testing.expect(!peer.wait_mailbox);
            if (scenario == 0) {
                try std.testing.expectEqual(.runnable, initial_scheduler_entities[6].ownership);
                try std.testing.expect(scheduler_state.cpus[0].runqueue.contains(&initial_scheduler_entities[6]));
            }
            if (scenario == 1) try std.testing.expect(peer.resume_after_stop and !peer.ready);
        }
    }
}

test "futex fused ordinary handoff revalidates wake identity and sole ownership" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    for (0..7) |scenario| {
        try initializeFusedWakeTest(0);
        const peer = &initial_thread_contexts[5];
        peer.cpu_affinity_mask = 1;
        peer.ready = true;
        try std.testing.expect(verifiedWakeThreadGeneration(5, 17));
        syncHotStateFromContext(5);
        // Model a change after ordinary publication but before TT reacquire.
        switch (scenario) {
            0 => {},
            1 => peer.generation += 1,
            2 => peer.owner_process = kernel.processPrincipalFromIndex(1).?,
            3 => peer.cpu_slot = 1,
            4 => peer.cpu_affinity_mask = 2,
            5 => peer.ready = false,
            6 => {
                initial_thread_contexts[6].allocated = true;
                initial_thread_contexts[6].ready = true;
                try std.testing.expect(verifiedAddThread(6, 1, true));
            },
            else => unreachable,
        }
        var frame = std.mem.zeroes(TrapFrame);
        scheduler_state.thread_table.lock();
        const handed_off = handoffOnlyReadyLocked(FusedWakeTestInstaller.install, &frame, 5, 17, default_process_principal, 1);
        scheduler_state.thread_table.unlock();
        try std.testing.expectEqual(scenario == 0, handed_off);
        try std.testing.expectEqual(@as(usize, if (scenario == 0) 1 else 0), FusedWakeTestInstaller.calls);
        if (scenario != 0) {
            try std.testing.expectEqual(@as(usize, 4), currentThread());
            try std.testing.expectEqual(.running, initial_scheduler_entities[4].ownership);
            try std.testing.expect(scheduler_state.cpus[0].runqueue.contains(&initial_scheduler_entities[5]));
        }
    }
}

pub const ClaimedWaitPublishResult = enum {
    published,
    pending,
    stale,
};

pub fn claimWaitTokenCompletion(
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
) WaitTokenClaimResult {
    if (wait_token == 0) return .stale;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return .stale;
    if (!ctx.allocated or ctx.owner_process != owner or
        ctx.generation != generation or ctx.active_wait_token != wait_token or
        !ctx.wait_mailbox or ctx.wait_completion_claimed)
    {
        return .stale;
    }
    ctx.wait_completion_claimed = true;
    ctx.wait_completion_publish_pending = false;
    // Claim is the completion linearization point.  Timeout and signal paths
    // must no longer be able to win while KernelState cleanup/copyout runs.
    ctx.wake_tick = 0;
    setThreadHotWaitState(thread_index, true, 0, ctx.ready);
    setThreadHotCompletionClaimed(thread_index, false);
    return .claimed;
}

fn publishClaimedWaitLocked(
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
    preferred_cpu: ?usize,
) ClaimedWaitPublishResult {
    const ctx = threadContextMutable(thread_index) orelse return .stale;
    if (!ctx.allocated or ctx.owner_process != owner or
        ctx.generation != generation or ctx.active_wait_token != wait_token or
        !ctx.wait_completion_claimed)
    {
        return .stale;
    }
    ctx.wait_completion_rax = resume_rax;
    if (ctx.ready) {
        ctx.frame.rax = resume_rax;
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.wait_completion_claimed = false;
        ctx.wait_completion_publish_pending = false;
        setThreadHotWaitState(thread_index, false, 0, true);
        setThreadHotCompletionClaimed(thread_index, false);
        return .published;
    }

    ctx.frame.rax = resume_rax;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    ctx.wake_tick = 0;
    if (ctx.stopped) {
        ctx.resume_after_stop = true;
        ctx.wait_completion_claimed = false;
        ctx.wait_completion_publish_pending = false;
        setThreadHotWaitState(thread_index, false, 0, false);
        setThreadHotCompletionClaimed(thread_index, false);
        return .published;
    }
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    if (!verifiedWakeThreadGenerationPreferred(thread_index, generation, preferred_cpu)) {
        // Registrations and user copyout have already completed.  Keep this
        // scheduler-owned completion non-runnable and retry publication from
        // the maintenance tick; it must never revert to an ordinary wait.
        ctx.active_wait_token = wait_token;
        ctx.ready = false;
        ctx.wait_completion_claimed = true;
        ctx.wait_completion_publish_pending = true;
        setThreadHotWaitState(thread_index, false, 0, false);
        setThreadHotCompletionClaimed(thread_index, true);
        return .pending;
    }
    ctx.wait_completion_claimed = false;
    ctx.wait_completion_publish_pending = false;
    setThreadHotCompletionClaimed(thread_index, false);
    return .published;
}

pub fn publishClaimedWaitCompletion(
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
) ClaimedWaitPublishResult {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return publishClaimedWaitLocked(
        thread_index,
        generation,
        owner,
        wait_token,
        resume_rax,
        null,
    );
}

/// Publish an IPC completion directly on the sender's CPU.  The caller can
/// then hand off to that same runnable generation without first waking its old
/// CPU and racing it into the running state.
pub fn publishClaimedWaitCompletionPreferred(
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
    preferred_cpu: usize,
) ClaimedWaitPublishResult {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return publishClaimedWaitLocked(
        thread_index,
        generation,
        owner,
        wait_token,
        resume_rax,
        preferred_cpu,
    );
}

pub fn wakeIfWaitingGeneration(thread_index: usize, generation: u32) bool {
    return wakeIfWaitingGenerationInternal(thread_index, generation, null, true, null);
}

pub fn wakeIfWaitingGenerationWithRax(thread_index: usize, generation: u32, resume_rax: u64) bool {
    return wakeIfWaitingGenerationInternal(thread_index, generation, resume_rax, true, null);
}

pub fn wakeIfWaitingGenerationWithRaxPreferCurrent(thread_index: usize, generation: u32, resume_rax: u64) bool {
    return wakeIfWaitingGenerationInternal(thread_index, generation, resume_rax, true, currentCpu());
}

pub fn wakeBlockedGenerationWithRax(thread_index: usize, generation: u32, resume_rax: u64) bool {
    return wakeIfWaitingGenerationInternal(thread_index, generation, resume_rax, false, null);
}

pub fn wakeMailboxWaiter(principal: kernel.PrincipalId) void {
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0 or hot.owner_process != principal) continue;
        if (hot.wait_mailbox == 0) continue;
        if (wakeIfWaiting(i)) return;
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

fn publishPendingSignalHint(principal: kernel.PrincipalId) void {
    const hint_va = kernel_runtime.kernel_state_global.processSignalPendingHintVa(principal);
    if (hint_va == 0) return;
    var pending: u32 = 1;
    _ = user_copy.copyBytesToUserVa(principal, hint_va, std.mem.asBytes(&pending));
}

fn processSignalTimerState(
    timer: *const ProcessSignalTimer,
    now_tick: u64,
) ProcessSignalTimerState {
    if (!timer.active) return .{};
    return .{
        .signo = timer.signo,
        .remaining_ticks = if (timer.deadline_tick > now_tick)
            timer.deadline_tick - now_tick
        else
            0,
        .interval_ticks = timer.interval_ticks,
    };
}

pub fn getProcessSignalTimer(
    principal: kernel.PrincipalId,
    now_tick: u64,
) ProcessSignalTimerState {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    for (scheduler_state.thread_table.signal_timers) |*timer| {
        if (timer.active and timer.principal == principal)
            return processSignalTimerState(timer, now_tick);
    }
    return .{};
}

pub fn setProcessSignalTimer(
    principal: kernel.PrincipalId,
    signo: u32,
    initial_ticks: u64,
    interval_ticks: u64,
    now_tick: u64,
) error{Full}!ProcessSignalTimerState {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();

    var free_slot: ?*ProcessSignalTimer = null;
    for (scheduler_state.thread_table.signal_timers) |*timer| {
        if (timer.active and timer.principal == principal) {
            const old = processSignalTimerState(timer, now_tick);
            if (initial_ticks == 0) {
                timer.* = .{};
                return old;
            }
            const deadline, const overflow = @addWithOverflow(now_tick, initial_ticks);
            timer.* = .{
                .active = true,
                .principal = principal,
                .signo = signo,
                .deadline_tick = if (overflow == 0) deadline else std.math.maxInt(u64),
                .interval_ticks = interval_ticks,
            };
            return old;
        }
        if (!timer.active and free_slot == null) free_slot = timer;
    }

    if (initial_ticks == 0) return .{};
    const timer = free_slot orelse return error.Full;
    const deadline, const overflow = @addWithOverflow(now_tick, initial_ticks);
    timer.* = .{
        .active = true,
        .principal = principal,
        .signo = signo,
        .deadline_tick = if (overflow == 0) deadline else std.math.maxInt(u64),
        .interval_ticks = interval_ticks,
    };
    return .{};
}

pub fn clearProcessSignalTimer(principal: kernel.PrincipalId) void {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    for (scheduler_state.thread_table.signal_timers) |*timer| {
        if (timer.active and timer.principal == principal) timer.* = .{};
    }
}

pub fn takeExpiredProcessSignalTimer(now_tick: u64) ?ExpiredProcessSignalTimer {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    for (scheduler_state.thread_table.signal_timers) |*timer| {
        if (!timer.active or timer.deadline_tick == 0 or now_tick < timer.deadline_tick)
            continue;
        const expired = ExpiredProcessSignalTimer{
            .principal = timer.principal,
            .signo = timer.signo,
        };
        if (timer.interval_ticks == 0) {
            timer.* = .{};
        } else {
            const elapsed = now_tick - timer.deadline_tick;
            const periods = elapsed / timer.interval_ticks + 1;
            const advance, const mul_overflow =
                @mulWithOverflow(periods, timer.interval_ticks);
            const next, const add_overflow =
                @addWithOverflow(timer.deadline_tick, advance);
            timer.deadline_tick = if (mul_overflow == 0 and add_overflow == 0)
                next
            else
                std.math.maxInt(u64);
        }
        return expired;
    }
    return null;
}

pub fn deliverSignal(
    principal: kernel.PrincipalId,
    signo: u32,
    blocked_resume_rax: u64,
) ?SignalDelivery {
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
    return deliverSignalLocked(principal, target, ctx, signal_bit, blocked_resume_rax);
}

// Thread capabilities name one generation. Publication and blocked-wait
// cancellation use the same lock as process-directed notification.
pub fn deliverThreadSignal(principal: kernel.PrincipalId, target: usize, generation: u32, signo: u32, blocked_resume_rax: u64) ?SignalDelivery {
    const signal_bit = signalBit(signo) orelse return null;
    scheduler_state.thread_table.lock();
    const ctx = threadContextMutable(target) orelse {
        scheduler_state.thread_table.unlock();
        return null;
    };
    if (ctx.generation != generation) {
        scheduler_state.thread_table.unlock();
        return null;
    }
    return deliverSignalLocked(principal, target, ctx, signal_bit, blocked_resume_rax);
}

fn deliverSignalLocked(principal: kernel.PrincipalId, target: usize, ctx: *ThreadContext, signal_bit: u64, blocked_resume_rax: u64) ?SignalDelivery {
    if (!ctx.allocated or ctx.owner_process != principal or ctx.stopped) {
        scheduler_state.thread_table.unlock();
        return null;
    }
    const newly_pending = (ctx.pending_signal_mask & signal_bit) == 0;
    const should_interrupt = newly_pending and (ctx.signal_blocked_mask & signal_bit) == 0;
    const was_blocked = should_interrupt and !ctx.ready and
        (ctx.wait_mailbox or ctx.wake_tick != 0) and
        !ctx.wait_completion_claimed;
    const generation = ctx.generation;
    ctx.pending_signal_mask |= signal_bit;
    publishPendingSignalHint(principal);
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
    if (was_blocked) {
        // Token invalidation and the blocked/runnable transition share this
        // lock with event completion claim.  Exactly one of signal, timeout,
        // or FD completion can own the wait.
        ctx.frame.rax = blocked_resume_rax;
        ctx.wait_mailbox = false;
        ctx.active_wait_token = 0;
        ctx.wake_tick = 0;
        ctx.wait_completion_claimed = false;
        ctx.wait_completion_publish_pending = false;
        ctx.wait_completion_rax = 0;
        ctx.ready = true;
        setThreadHotWaitState(target, false, 0, true);
        setThreadHotCompletionClaimed(target, false);
        _ = verifiedWakeThreadGeneration(target, generation);
    }
    const cpu_slot = ctx.cpu_slot;
    scheduler_state.thread_table.unlock();
    // A CPU-bound target need not make another syscall to observe a signal.
    // If it migrates during publication, the normal return/tick path still
    // observes the per-thread pending bit; the IPI is only the prompt kick.
    if (should_interrupt) _ = smp.interruptCpu(cpu_slot);
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
    ctx.signal_inhibit_start = inhibit_start;
    ctx.signal_inhibit_end = inhibit_end;
    ctx.signal_inhibit_secondary_start = inhibit_secondary_start;
    ctx.signal_inhibit_secondary_end = inhibit_secondary_end;
    // The thread that registers the runtime signal trampoline is the process's
    // signal owner (the main/event-loop thread).  Process-directed signals are
    // delivered here so teardown runs on the loop that can service them.
    ctx.signal_owner = true;
    ctx.signal_interrupt_consumed = firstUnblockedPendingSignal(ctx) == 0;
    return true;
}

pub fn setCurrentSignalBlockedMask(blocked_mask: u64) bool {
    scheduler_state.thread_table.lock();
    const ctx = threadContextMutable(currentThread()) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    if (!ctx.allocated or ctx.signal_entry == 0) {
        scheduler_state.thread_table.unlock();
        return false;
    }
    ctx.signal_blocked_mask = blocked_mask;
    ctx.signal_interrupt_consumed = firstUnblockedPendingSignal(ctx) == 0;
    const should_publish = !ctx.signal_interrupt_consumed;
    const principal = ctx.owner_process;
    scheduler_state.thread_table.unlock();
    if (should_publish) publishPendingSignalHint(principal);
    return true;
}

pub fn setCurrentSignalPendingHint(hint_va: u64) bool {
    scheduler_state.thread_table.lock();
    const ctx = threadContextMutable(currentThread()) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    if (!ctx.allocated or ctx.signal_entry == 0 or !ctx.signal_owner) {
        scheduler_state.thread_table.unlock();
        return false;
    }
    const should_publish = firstUnblockedPendingSignal(ctx) != 0;
    const principal = ctx.owner_process;
    if (!kernel_runtime.kernel_state_global.setProcessSignalPendingHintVa(principal, hint_va)) {
        scheduler_state.thread_table.unlock();
        return false;
    }
    scheduler_state.thread_table.unlock();
    if (should_publish) publishPendingSignalHint(principal);
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

pub fn copyThreadXState(source_thread: usize, target_thread: usize) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const source = threadContext(source_thread) orelse return false;
    const target = threadContextMutable(target_thread) orelse return false;
    if (!source.allocated or !target.allocated) return false;
    target.x_state = source.x_state;
    target.pkru = source.pkru;
    return true;
}

pub fn claimCurrentSignalForUserReturn(rip: u64) ?ClaimedSignal {
    const thread_index = currentThread();
    scheduler_state.thread_table.lock();
    const ctx = threadContextMutable(thread_index) orelse {
        scheduler_state.thread_table.unlock();
        return null;
    };
    const signo = firstUnblockedPendingSignal(ctx);
    if (!ctx.allocated or signo == 0 or ctx.signal_entry == 0 or ctx.active_fault_frame != 0) {
        scheduler_state.thread_table.unlock();
        return null;
    }
    if (ctx.signal_inhibit_start < ctx.signal_inhibit_end and
        rip >= ctx.signal_inhibit_start and rip < ctx.signal_inhibit_end)
    {
        scheduler_state.thread_table.unlock();
        return null;
    }
    if (ctx.signal_inhibit_secondary_start < ctx.signal_inhibit_secondary_end and
        rip >= ctx.signal_inhibit_secondary_start and rip < ctx.signal_inhibit_secondary_end)
    {
        scheduler_state.thread_table.unlock();
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
    const should_publish = !ctx.signal_interrupt_consumed;
    const principal = ctx.owner_process;
    scheduler_state.thread_table.unlock();
    if (should_publish) publishPendingSignalHint(principal);
    return claimed;
}

pub fn restoreClaimedSignal(claimed: ClaimedSignal) void {
    scheduler_state.thread_table.lock();
    const ctx = threadContextMutable(claimed.thread_index) orelse {
        scheduler_state.thread_table.unlock();
        return;
    };
    if (!ctx.allocated) {
        scheduler_state.thread_table.unlock();
        return;
    }
    const signal_bit = signalBit(claimed.signo) orelse {
        scheduler_state.thread_table.unlock();
        return;
    };
    ctx.pending_signal_mask |= signal_bit;
    ctx.signal_interrupt_consumed = firstUnblockedPendingSignal(ctx) == 0;
    setThreadHotPendingSignalMask(claimed.thread_index, ctx.pending_signal_mask);
    const principal = ctx.owner_process;
    scheduler_state.thread_table.unlock();
    publishPendingSignalHint(principal);
}

pub fn copyCurrentSignalXState(out: *[xstate_bytes]u8) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContext(currentThread()) orelse return false;
    if (!ctx.allocated) return false;
    out.* = ctx.x_state;
    return true;
}

pub fn restoreCurrentSignalXState(saved: *const [xstate_bytes]u8) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(currentThread()) orelse return false;
    if (!ctx.allocated) return false;
    ctx.x_state = saved.*;
    return true;
}

pub const FaultDelivery = struct {
    entry: u64,
    frame_va: u64,
};

pub fn configureCurrentFaultDelivery(entry: u64, stack_base: u64, stack_size: u64) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(currentThread()) orelse return false;
    if (!ctx.allocated or ctx.active_fault_frame != 0) return false;
    ctx.fault_entry = entry;
    ctx.fault_stack_base = stack_base;
    ctx.fault_stack_size = stack_size;
    return true;
}

pub fn claimCurrentFault() ?FaultDelivery {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(currentThread()) orelse return null;
    if (!ctx.allocated or ctx.fault_entry == 0 or ctx.active_fault_frame != 0) return null;
    const frame_va = (ctx.fault_stack_base + ctx.fault_stack_size - process_abi.fault_frame_size) & ~@as(u64, 63);
    ctx.active_fault_frame = frame_va;
    return .{ .entry = ctx.fault_entry, .frame_va = frame_va };
}

pub fn completeCurrentFault(frame_va: u64) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(currentThread()) orelse return false;
    if (!ctx.allocated or frame_va == 0 or ctx.active_fault_frame != frame_va) return false;
    ctx.active_fault_frame = 0;
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
    clearProcessSignalTimer(principal);
    var released: usize = 0;
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const ctx = threadContext(i) orelse continue;
        if (!ctx.allocated or ctx.owner_process != principal) continue;
        if (releaseThread(i)) released += 1;
    }
    return released;
}

fn retryClaimedWaitCompletion(thread_index: usize) void {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return;
    if (!ctx.allocated or ctx.ready or !ctx.wait_completion_claimed or
        !ctx.wait_completion_publish_pending or
        ctx.active_wait_token == 0)
    {
        return;
    }
    _ = publishClaimedWaitLocked(
        thread_index,
        ctx.generation,
        ctx.owner_process,
        ctx.active_wait_token,
        ctx.wait_completion_rax,
        null,
    );
}

pub fn wakeExpiredTimers(now_tick: u64) void {
    if (!isBootstrapSchedulerCpu()) return;
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0) continue;
        if (hot.ready != 0) continue;
        if (hot.wait_completion_claimed != 0) {
            retryClaimedWaitCompletion(i);
            continue;
        }
        if (hot.wake_tick == 0 or now_tick < hot.wake_tick) continue;
        wakeIfTimerExpired(i, now_tick);
    }
}

pub fn blockCurrentThread(
    frame: *TrapFrame,
    wait_mailbox: bool,
    wait_token: u64,
    timeout_ticks: u64,
    resume_rax: u64,
    before_block: ?BeforeCurrentThreadLeaveCallback,
) bool {
    if (!isBootstrapSchedulerCpu()) {
        // AP user threads must leave the CPU for a real blocking receive.
        return parkApThreadForBlock(frame, wait_mailbox, wait_token, timeout_ticks, resume_rax, before_block);
    }
    const current_thread = currentThread();
    scheduler_state.thread_table.lock();
    const ctx = threadContextMutable(current_thread) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    if (wait_token != 0) {
        if (!wait_mailbox or ctx.active_wait_token != wait_token) {
            scheduler_state.thread_table.unlock();
            return false;
        }
        if (!ctx.wait_mailbox) {
            frame.rax = ctx.frame.rax;
            ctx.active_wait_token = 0;
            ctx.wake_tick = 0;
            ctx.wait_completion_publish_pending = false;
            ctx.wait_completion_rax = 0;
            scheduler_state.thread_table.unlock();
            return true;
        }
    } else if (ctx.active_wait_token != 0) {
        scheduler_state.thread_table.unlock();
        return false;
    }
    // Keep signal publication and the transition to blocked atomic with
    // respect to each other.  The syscall caller will remove any waiter it
    // registered and return NOT_READY/EAGAIN, allowing delivery at a safe
    // user-frame boundary instead of leaving a completion waiter behind.
    if (firstUnblockedPendingSignal(ctx) != 0 and ctx.signal_entry != 0 and
        !ctx.signal_interrupt_consumed)
    {
        ctx.signal_interrupt_consumed = true;
        if (wait_token != 0) {
            ctx.wait_mailbox = false;
            ctx.active_wait_token = 0;
        }
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
    ctx.active_wait_token = wait_token;
    ctx.wait_completion_claimed = false;
    ctx.wait_completion_publish_pending = false;
    ctx.wait_completion_rax = 0;
    ctx.wake_tick = if (timeout_ticks == 0) 0 else lapic_tick_count +| timeout_ticks;
    ctx.ready = false;
    setThreadHotCr3(current_thread, ctx.cr3);
    setThreadHotWaitState(current_thread, wait_mailbox, ctx.wake_tick, false);
    setThreadHotCompletionClaimed(current_thread, false);
    verifiedBlockThread(current_thread);
    // Match the AP block ordering: a keyed remote completion selects its
    // destination while holding this table lock.  It must observe either the
    // current runnable thread or a CPU that is already logically idle, never
    // a blocked thread still advertised as the CPU's current owner.
    noteCpuIdleTick(currentCpu());
    scheduler_state.thread_table.unlock();
    if (before_block) |callback| callback.run(callback.context);
    // The blocked context was saved before its scheduler entity left the
    // running state.  A remote CPU may complete this wait after the table lock
    // is dropped and publish a different rax into ctx.frame.  Do not use the
    // ordinary switch path here: it saves the live syscall frame again and can
    // overwrite that completion (including when the just-woken current thread
    // is picked).  Only activate and load the selected saved context.
    if (loadNextReadyThread(frame)) return true;
    // There is no userspace context to return to while the caller is blocked.
    // Keep CPU 0 interruptible until a wakeup publishes a ready thread, then
    // load the wake-provided frame without overwriting rax with the original
    // syscall's resume value.
    loadNextReadyThreadOrIdle(frame);
    return true;
}
