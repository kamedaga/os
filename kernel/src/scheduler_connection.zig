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
const scheduler_abi = @import("kernel_abi_root").scheduler_abi;
pub const verified_sched = @import("verified_sched.zig");

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
const max_external_scheduler_events: usize = 256;
const external_runtime_ns_per_tick: u64 = 1_000_000;
const ap_user_dispatch_enabled = true;

const scheduler_trace_commit: u64 = 1 << 0;
const scheduler_trace_ap_entry: u64 = 1 << 1;
const scheduler_trace_ap_tick: u64 = 1 << 2;
const scheduler_trace_ap_block: u64 = 1 << 3;
const scheduler_trace_wake: u64 = 1 << 4;
const scheduler_trace_timer_due: u64 = 1 << 5;
const scheduler_trace_metric: u64 = 1 << 6;

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
    current_thread: usize = idleThreadMarker,
    current_principal: ?kernel.PrincipalId = null,
    current_cr3: u64 = 0,
    idle_thread: usize = idleThreadMarker,
    pending_commit_thread: ?usize = null,
    pending_commit_generation: u32 = 0,
    tick_accum: u64 = 0,
    idle_event_pending: bool = false,
    enabled: bool = false,
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
    stopped: bool = false,
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
pub export var thread_contexts_ptr: *anyopaque = @ptrCast(initial_thread_contexts[0..].ptr);
var initial_thread_hot_states: [initialThreadCapacity]ThreadHotState = buildInitialThreadHotStates();
pub export var lapic_tick_count: u64 = 0;
pub var scheduler_tick_accum: u64 = 0;
pub var scheduler_switch_count: u64 = 0;
pub var scheduler_timer_log_once: u8 = 0;
pub var initial_fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes;
pub var kernel_interrupt_fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes;

pub const ExternalSchedulerEvent = extern struct {
    size: u32 = @intCast(scheduler_abi.sched_event_size),
    version: u16 = scheduler_abi.abi_version,
    event_type: u16 = 0,
    sequence: u64 = 0,
    cpu_id: u32 = 0,
    reserved0: u32 = 0,
    thread_id: u64 = scheduler_abi.no_thread,
    generation: u64 = 0,
    runtime_ns: u64 = 0,
    weight: u64 = 0,
    slice_ns: u64 = 0,
};

const ThreadTableState = struct {
    lock_state: SchedulerSpinLock = .{},
    contexts: []ThreadContext = initial_thread_contexts[0..],
    hot_states: []ThreadHotState = initial_thread_hot_states[0..],
    next_cpu_cursor: usize = bootstrap_cpu_slot,
    ipc_wake_resume_tsc: [initialThreadCapacity]u64 = [_]u64{0} ** initialThreadCapacity,
    ipc_wake_resume_cpu: [initialThreadCapacity]u16 = [_]u16{0} ** initialThreadCapacity,

    fn lock(self: *ThreadTableState) void {
        self.lock_state.lock();
    }

    fn unlock(self: *ThreadTableState) void {
        self.lock_state.unlock();
    }
};

const ExternalSchedulerState = struct {
    lock_state: SchedulerSpinLock = .{},
    enabled: bool = false,
    policy_thread: ?usize = null,
    events: [max_external_scheduler_events]ExternalSchedulerEvent =
        [_]ExternalSchedulerEvent{.{}} ** max_external_scheduler_events,
    event_head: usize = 0,
    event_len: usize = 0,
    next_sequence: u64 = 1,
    trace_mask: u64 = 0,
    ap_claim_trace_count: u32 = 0,
    ready_event_seen: u8 = 0,

    fn lock(self: *ExternalSchedulerState) void {
        self.lock_state.lock();
    }

    fn unlock(self: *ExternalSchedulerState) void {
        self.lock_state.unlock();
    }
};

const VerifiedCoreState = struct {
    lock_state: SchedulerSpinLock = .{},
    state: verified_sched.State = undefined,
    probe_state: verified_sched.State = undefined,
    runqueue_scratch: verified_sched.Runqueue = undefined,
    pick_scratch: verified_sched.PickResult = undefined,
    decision: verified_sched.Decision = undefined,
    initialized: bool = false,
    faulted: bool = false,
    log_count: u32 = 0,

    fn lock(self: *VerifiedCoreState) void {
        self.lock_state.lock();
    }

    fn unlock(self: *VerifiedCoreState) void {
        self.lock_state.unlock();
    }
};

const SchedulerState = struct {
    thread_table: ThreadTableState = .{},
    external: ExternalSchedulerState = .{},
    verified: VerifiedCoreState = .{},
    cpus: [smp.max_cpus]CpuSchedulerState = buildInitialCpuSchedulerStates(),
};

var scheduler_state: SchedulerState = .{};

fn verifiedCoreCpuCount() usize {
    return @min(smp.max_cpus, verified_sched.sched_max_cpus);
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

fn verifiedThreadId(thread_index: usize) ?i64 {
    const ctx = threadContext(thread_index) orelse return null;
    if (!ctx.allocated) return null;
    return verifiedThreadIdForGeneration(thread_index, ctx.generation);
}

fn threadIndexFromVerifiedThreadId(thread_id: i64) ?usize {
    if (thread_id <= 0) return null;
    const raw: u128 = @intCast(thread_id - 1);
    const index: usize = @intCast(raw % @as(u128, maxThreadSlots));
    if (index >= scheduler_state.thread_table.contexts.len) return null;
    return index;
}

fn verifiedThreadIdFromExternal(thread_id: u64, generation: u64) ?i64 {
    if (generation > std.math.maxInt(u32)) return null;
    const thread_index = threadIndexFromExternalId(thread_id) orelse return null;
    return verifiedThreadIdForGeneration(thread_index, @intCast(generation));
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

fn noteVerifiedCoreMismatch(
    cpu_id: usize,
    expected_thread_id: u64,
    expected_generation: u64,
    decision: verified_sched.Decision,
) void {
    const old = @atomicRmw(u32, &scheduler_state.verified.log_count, .Add, 1, .acq_rel);
    if (old >= 16) return;
    kernel_log.writeFmt(
        "[sched-core] commit mismatch cpu={} expected_thread={} expected_gen={} decision={s} core_thread={} core_gen={}\n",
        .{ cpu_id, expected_thread_id, expected_generation, @tagName(decision.kind), decision.thread_id, decision.generation },
    );
}

fn verifiedAddThread(thread_index: usize, generation: u32, ready: bool) void {
    if (!verifiedCoreReady()) return;
    if (externalSchedulerOwnsThread(thread_index)) return;
    const thread_id = verifiedThreadId(thread_index) orelse return;
    const cpu_id = verifiedThreadCpu(thread_index);
    var should_wake_cpu = false;
    scheduler_state.verified.lock();
    const add_rc = verified_sched.pacha_kernel_sched_add_thread(
        &scheduler_state.verified.state,
        cpu_id,
        thread_id,
        @intCast(generation),
        1024,
        4_000_000,
        &scheduler_state.verified.decision,
        &scheduler_state.verified.runqueue_scratch,
    );
    if (add_rc != .ok) {
        scheduler_state.verified.unlock();
        noteVerifiedCoreIssue("add", add_rc, thread_index);
        return;
    }
    if (!ready) {
        const block_rc = verified_sched.pacha_kernel_sched_block_thread_on_cpu(
            &scheduler_state.verified.state,
            cpu_id,
            thread_id,
            &scheduler_state.verified.decision,
        );
        if (block_rc != .ok) noteVerifiedCoreIssue("add-block", block_rc, thread_index);
    }
    should_wake_cpu = ready and cpu_id != currentCpu();
    scheduler_state.verified.unlock();
    if (should_wake_cpu) _ = smp.wakeCpu(cpu_id);
}

fn verifiedWakeThread(thread_index: usize) void {
    if (!verifiedCoreReady()) return;
    if (externalSchedulerOwnsThread(thread_index)) return;
    const thread_id = verifiedThreadId(thread_index) orelse return;
    const cpu_id = verifiedThreadCpu(thread_index);
    scheduler_state.verified.lock();
    const rc = verified_sched.pacha_kernel_sched_wake_thread_on_cpu(
        &scheduler_state.verified.state,
        cpu_id,
        thread_id,
        &scheduler_state.verified.decision,
    );
    scheduler_state.verified.unlock();
    if (rc != .ok) noteVerifiedCoreIssue("wake", rc, thread_index);
    if (rc == .ok and cpu_id != currentCpu()) _ = smp.wakeCpu(cpu_id);
}

fn verifiedBlockThread(thread_index: usize) void {
    if (!verifiedCoreReady()) return;
    if (externalSchedulerOwnsThread(thread_index)) return;
    const thread_id = verifiedThreadId(thread_index) orelse return;
    const cpu_id = verifiedThreadCpu(thread_index);
    scheduler_state.verified.lock();
    const rc = verified_sched.pacha_kernel_sched_block_thread_on_cpu(
        &scheduler_state.verified.state,
        cpu_id,
        thread_id,
        &scheduler_state.verified.decision,
    );
    scheduler_state.verified.unlock();
    if (rc != .ok) noteVerifiedCoreIssue("block", rc, thread_index);
}

fn verifiedExitThread(thread_index: usize) void {
    if (!verifiedCoreReady()) return;
    const thread_id = verifiedThreadId(thread_index) orelse return;
    const cpu_id = verifiedThreadCpu(thread_index);
    scheduler_state.verified.lock();
    defer scheduler_state.verified.unlock();
    const rc = verified_sched.pacha_kernel_sched_exit_thread_on_cpu(
        &scheduler_state.verified.state,
        cpu_id,
        thread_id,
        &scheduler_state.verified.decision,
    );
    if (rc != .ok) noteVerifiedCoreIssue("exit", rc, thread_index);
}

fn verifiedFinishCpu(cpu_id: usize) void {
    if (!verifiedCoreReady()) return;
    if (cpu_id >= verifiedCoreCpuCount()) return;
    scheduler_state.verified.lock();
    defer scheduler_state.verified.unlock();
    const rc = verified_sched.pacha_kernel_sched_finish_current(
        &scheduler_state.verified.state,
        cpu_id,
        &scheduler_state.verified.decision,
        &scheduler_state.verified.runqueue_scratch,
    );
    if (rc != .ok) noteVerifiedCoreIssue("finish", rc, null);
}

fn verifiedChargeAndFinish(cpu_id: usize, thread_index: usize, runtime_ns: u64) void {
    if (!verifiedCoreReady()) return;
    if (cpu_id >= verifiedCoreCpuCount()) return;
    if (runtime_ns > @as(u64, @intCast(std.math.maxInt(i64)))) return;
    scheduler_state.verified.lock();
    defer scheduler_state.verified.unlock();
    const timer_rc = verified_sched.pacha_kernel_sched_on_timer(
        &scheduler_state.verified.state,
        cpu_id,
        @intCast(runtime_ns),
        &scheduler_state.verified.decision,
        &scheduler_state.verified.runqueue_scratch,
    );
    if (timer_rc != .ok) {
        noteVerifiedCoreIssue("timer", timer_rc, thread_index);
        return;
    }
    const finish_rc = verified_sched.pacha_kernel_sched_finish_current(
        &scheduler_state.verified.state,
        cpu_id,
        &scheduler_state.verified.decision,
        &scheduler_state.verified.runqueue_scratch,
    );
    if (finish_rc != .ok) noteVerifiedCoreIssue("timer-finish", finish_rc, thread_index);
}

fn verifiedApplyReadyEventLocked(event: ExternalSchedulerEvent) void {
    if (event.thread_id == scheduler_abi.no_thread) return;
    if (event.thread_id > @as(u64, @intCast(std.math.maxInt(i64)))) return;
    if (event.generation > @as(u64, @intCast(std.math.maxInt(i64)))) return;
    const internal_thread_id = verifiedThreadIdFromExternal(event.thread_id, event.generation) orelse return;
    const weight = if (event.weight == 0) @as(u64, 1024) else event.weight;
    const slice_ns = if (event.slice_ns == 0) @as(u64, 4_000_000) else event.slice_ns;
    if (weight > @as(u64, @intCast(std.math.maxInt(i64))) or slice_ns > @as(u64, @intCast(std.math.maxInt(i64)))) return;
    const add_rc = verified_sched.pacha_kernel_sched_add_thread(
        &scheduler_state.verified.state,
        if (event.cpu_id < verifiedCoreCpuCount()) @intCast(event.cpu_id) else bootstrap_cpu_slot,
        internal_thread_id,
        @intCast(event.generation),
        @intCast(weight),
        @intCast(slice_ns),
        &scheduler_state.verified.decision,
        &scheduler_state.verified.runqueue_scratch,
    );
    if (add_rc == .ok) return;
    const wake_rc = verified_sched.pacha_kernel_sched_wake_thread_on_cpu(
        &scheduler_state.verified.state,
        if (event.cpu_id < verifiedCoreCpuCount()) @intCast(event.cpu_id) else bootstrap_cpu_slot,
        internal_thread_id,
        &scheduler_state.verified.decision,
    );
    if (wake_rc != .ok and wake_rc != .state) noteVerifiedCoreIssue("event-ready", wake_rc, threadIndexFromExternalId(event.thread_id));
}

fn verifiedApplyExternalEvent(event: ExternalSchedulerEvent) void {
    if (!verifiedCoreReady()) return;
    scheduler_state.verified.lock();
    defer scheduler_state.verified.unlock();
    switch (event.event_type) {
        scheduler_abi.event_thread_ready => verifiedApplyReadyEventLocked(event),
        scheduler_abi.event_thread_blocked => {
            if (event.thread_id == scheduler_abi.no_thread or event.thread_id > @as(u64, @intCast(std.math.maxInt(i64)))) return;
            const internal_thread_id = verifiedThreadIdFromExternal(event.thread_id, event.generation) orelse return;
            const rc = verified_sched.pacha_kernel_sched_block_thread_on_cpu(
                &scheduler_state.verified.state,
                if (event.cpu_id < verifiedCoreCpuCount()) @intCast(event.cpu_id) else bootstrap_cpu_slot,
                internal_thread_id,
                &scheduler_state.verified.decision,
            );
            if (rc != .ok and rc != .invalid and rc != .state) noteVerifiedCoreIssue("event-block", rc, threadIndexFromExternalId(event.thread_id));
        },
        scheduler_abi.event_thread_exited => {
            if (event.thread_id == scheduler_abi.no_thread or event.thread_id > @as(u64, @intCast(std.math.maxInt(i64)))) return;
            const internal_thread_id = verifiedThreadIdFromExternal(event.thread_id, event.generation) orelse return;
            const rc = verified_sched.pacha_kernel_sched_exit_thread_on_cpu(
                &scheduler_state.verified.state,
                if (event.cpu_id < verifiedCoreCpuCount()) @intCast(event.cpu_id) else bootstrap_cpu_slot,
                internal_thread_id,
                &scheduler_state.verified.decision,
            );
            if (rc != .ok and rc != .invalid and rc != .state) noteVerifiedCoreIssue("event-exit", rc, threadIndexFromExternalId(event.thread_id));
        },
        scheduler_abi.event_thread_yield, scheduler_abi.event_tick => {
            if (event.cpu_id >= verifiedCoreCpuCount()) return;
            if (event.thread_id != scheduler_abi.no_thread) {
                if (event.runtime_ns > @as(u64, @intCast(std.math.maxInt(i64)))) return;
                const timer_rc = verified_sched.pacha_kernel_sched_on_timer(
                    &scheduler_state.verified.state,
                    @intCast(event.cpu_id),
                    @intCast(event.runtime_ns),
                    &scheduler_state.verified.decision,
                    &scheduler_state.verified.runqueue_scratch,
                );
                if (timer_rc != .ok) {
                    noteVerifiedCoreIssue("event-timer", timer_rc, threadIndexFromExternalId(event.thread_id));
                    return;
                }
            }
            const finish_rc = verified_sched.pacha_kernel_sched_finish_current(
                &scheduler_state.verified.state,
                @intCast(event.cpu_id),
                &scheduler_state.verified.decision,
                &scheduler_state.verified.runqueue_scratch,
            );
            if (finish_rc != .ok and finish_rc != .state) noteVerifiedCoreIssue("event-finish", finish_rc, null);
        },
        scheduler_abi.event_cpu_idle => {},
        else => {},
    }
}

fn verifiedCommitMatches(
    cpu_id: usize,
    thread_id: u64,
    generation: u64,
    decision: verified_sched.Decision,
) bool {
    if (thread_id == scheduler_abi.no_thread) {
        return decision.kind == .idle and decision.cpu_id == cpu_id;
    }
    if (thread_id > @as(u64, @intCast(std.math.maxInt(i64)))) return false;
    if (generation > @as(u64, @intCast(std.math.maxInt(i64)))) return false;
    const expected_thread_index = threadIndexFromExternalId(thread_id) orelse return false;
    const decision_thread_index = threadIndexFromVerifiedThreadId(decision.thread_id) orelse return false;
    return decision.kind == .run_thread and
        decision.cpu_id == cpu_id and
        decision_thread_index == expected_thread_index and
        decision.generation == @as(i64, @intCast(generation));
}

fn verifiedProbeCommit(cpu_id: usize, thread_id: u64, generation: u64) void {
    if (!verifiedCoreReady()) return;
    if (cpu_id >= verifiedCoreCpuCount()) return;
    scheduler_state.verified.lock();
    defer scheduler_state.verified.unlock();
    scheduler_state.verified.probe_state = scheduler_state.verified.state;
    const rc = verified_sched.pacha_kernel_sched_pick_cpu(
        &scheduler_state.verified.probe_state,
        cpu_id,
        &scheduler_state.verified.decision,
        &scheduler_state.verified.pick_scratch,
        &scheduler_state.verified.runqueue_scratch,
    );
    if (rc != .ok) {
        noteVerifiedCoreIssue("pick", rc, null);
        return;
    }
    if (!verifiedCommitMatches(cpu_id, thread_id, generation, scheduler_state.verified.decision)) {
        noteVerifiedCoreMismatch(cpu_id, thread_id, generation, scheduler_state.verified.decision);
        return;
    }
    scheduler_state.verified.state = scheduler_state.verified.probe_state;
}

fn verifiedPickThreadForCpu(cpu_id: usize) ?usize {
    if (!verifiedCoreReady()) return null;
    if (cpu_id >= verifiedCoreCpuCount()) return null;
    scheduler_state.verified.lock();
    defer scheduler_state.verified.unlock();
    const rc = verified_sched.pacha_kernel_sched_pick_cpu(
        &scheduler_state.verified.state,
        cpu_id,
        &scheduler_state.verified.decision,
        &scheduler_state.verified.pick_scratch,
        &scheduler_state.verified.runqueue_scratch,
    );
    if (rc != .ok) {
        noteVerifiedCoreIssue("pick", rc, null);
        return null;
    }
    switch (scheduler_state.verified.decision.kind) {
        .none, .idle => return null,
        .run_thread => {
            if (scheduler_state.verified.decision.cpu_id != cpu_id) {
                noteVerifiedCoreMismatch(cpu_id, scheduler_abi.no_thread, 0, scheduler_state.verified.decision);
                return null;
            }
            if (scheduler_state.verified.decision.thread_id <= 0) return null;
            if (scheduler_state.verified.decision.generation < 0) return null;
            if (scheduler_state.verified.decision.generation > @as(i64, @intCast(std.math.maxInt(u32)))) return null;
            const thread_index = threadIndexFromVerifiedThreadId(scheduler_state.verified.decision.thread_id) orelse return null;
            const ctx = threadContext(thread_index) orelse return null;
            if (!ctx.allocated or !ctx.ready) return null;
            if (ctx.generation != @as(u32, @intCast(scheduler_state.verified.decision.generation))) return null;
            if (ctx.cpu_slot != cpu_id) return null;
            return thread_index;
        },
    }
}

fn verifiedRollbackPickedCpu(cpu_id: usize) void {
    verifiedFinishCpu(cpu_id);
}

fn verifiedRollbackHandoff(cpu_id: usize, thread_index: usize, generation: u32) void {
    const thread_id = verifiedThreadIdForGeneration(thread_index, generation) orelse return;
    scheduler_state.verified.lock();
    const rc = verified_sched.pacha_kernel_sched_handoff_to_thread_on_cpu(
        &scheduler_state.verified.state,
        cpu_id,
        thread_id,
        &scheduler_state.verified.decision,
        &scheduler_state.verified.runqueue_scratch,
    );
    scheduler_state.verified.unlock();
    if (rc != .ok) noteVerifiedCoreIssue("handoff-rollback", rc, thread_index);
}

fn verifiedHandoffToThreadOnCpu(
    cpu_id: usize,
    thread_index: usize,
    generation: u32,
    rollback_thread_index: usize,
    rollback_generation: u32,
) bool {
    if (!verifiedCoreReady()) return false;
    if (cpu_id >= verifiedCoreCpuCount()) return false;
    const thread_id = verifiedThreadIdForGeneration(thread_index, generation) orelse return false;
    scheduler_state.verified.lock();
    const rc = verified_sched.pacha_kernel_sched_handoff_to_thread_on_cpu(
        &scheduler_state.verified.state,
        cpu_id,
        thread_id,
        &scheduler_state.verified.decision,
        &scheduler_state.verified.runqueue_scratch,
    );
    const decision = scheduler_state.verified.decision;
    scheduler_state.verified.unlock();
    if (rc != .ok) {
        noteVerifiedCoreIssue("handoff", rc, thread_index);
        return false;
    }
    const decision_thread_index = threadIndexFromVerifiedThreadId(decision.thread_id) orelse {
        noteVerifiedCoreMismatch(cpu_id, externalThreadId(thread_index), generation, decision);
        verifiedRollbackHandoff(cpu_id, rollback_thread_index, rollback_generation);
        return false;
    };
    if (decision.kind != .run_thread or
        decision.cpu_id != cpu_id or
        decision_thread_index != thread_index or
        decision.generation != @as(i64, @intCast(generation)))
    {
        noteVerifiedCoreMismatch(cpu_id, externalThreadId(thread_index), generation, decision);
        verifiedRollbackHandoff(cpu_id, rollback_thread_index, rollback_generation);
        return false;
    }
    return true;
}

fn fillUserEntryForThread(cpu_id: usize, thread_index: usize, out_entry: *scheduler_observer.UserEntry) bool {
    const ctx = threadContext(thread_index) orelse return false;
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    if (!ctx.allocated or hot.allocated == 0) return false;
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

fn claimKernelScheduledUserEntry(cpu_id: usize, out_entry: *scheduler_observer.UserEntry) bool {
    if (policyActive()) return false;
    if (cpu_id == bootstrap_cpu_slot) return false;
    const thread_index = verifiedPickThreadForCpu(cpu_id) orelse return false;
    const state = schedulerStateForSlot(cpu_id) orelse {
        verifiedRollbackPickedCpu(cpu_id);
        return false;
    };
    state.lock.lock();
    if (!state.enabled or !fillUserEntryForThread(cpu_id, thread_index, out_entry)) {
        state.lock.unlock();
        verifiedRollbackPickedCpu(cpu_id);
        return false;
    }
    const ctx = threadContext(thread_index) orelse {
        state.lock.unlock();
        verifiedRollbackPickedCpu(cpu_id);
        return false;
    };
    state.current_thread = thread_index;
    state.current_principal = ctx.owner_process;
    state.current_cr3 = ctx.cr3;
    state.pending_commit_thread = null;
    state.pending_commit_generation = 0;
    state.idle_event_pending = false;
    state.is_idle = false;
    state.lock.unlock();
    return true;
}

pub fn activateNextReadyOnCurrentCpu() bool {
    const cpu_id = currentCpu();
    const thread_index = verifiedPickThreadForCpu(cpu_id) orelse return false;
    if (activate(thread_index)) return true;
    verifiedRollbackPickedCpu(cpu_id);
    return false;
}

pub fn loadNextReadyThread(frame: *TrapFrame) bool {
    const cpu_id = currentCpu();
    const thread_index = verifiedPickThreadForCpu(cpu_id) orelse return false;
    if (!activate(thread_index)) {
        verifiedRollbackPickedCpu(cpu_id);
        return false;
    }
    if (loadContextIntoFrame(thread_index, frame)) return true;
    verifiedRollbackPickedCpu(cpu_id);
    return false;
}

fn switchToNextReadyOnCurrentCpu(frame: *TrapFrame, saved_rax: ?u64) bool {
    const cpu_id = currentCpu();
    const thread_index = verifiedPickThreadForCpu(cpu_id) orelse return false;
    if (switchTo(thread_index, frame, saved_rax)) return true;
    verifiedRollbackPickedCpu(cpu_id);
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
    scheduler_state.external.enabled = false;
    scheduler_state.external.policy_thread = null;
    scheduler_state.external.event_head = 0;
    scheduler_state.external.event_len = 0;
    scheduler_state.external.next_sequence = 1;
    scheduler_state.external.trace_mask = 0;
    scheduler_state.external.ap_claim_trace_count = 0;
    @atomicStore(u8, &scheduler_state.external.ready_event_seen, 0, .release);
    verified_sched.pacha_kernel_sched_empty_state(verifiedCoreCpuCount(), &scheduler_state.verified.state);
    verified_sched.pacha_kernel_sched_empty_state(verifiedCoreCpuCount(), &scheduler_state.verified.probe_state);
    scheduler_state.verified.initialized = true;
    scheduler_state.verified.faulted = false;
    scheduler_state.verified.log_count = 0;
    scheduler_state.thread_table.next_cpu_cursor = bootstrap_cpu_slot;
}

fn externalThreadId(thread_index: usize) u64 {
    return @as(u64, @intCast(thread_index)) + 1;
}

fn threadIndexFromExternalId(thread_id: u64) ?usize {
    if (thread_id == scheduler_abi.no_thread) return null;
    const index = thread_id - 1;
    if (index > @as(u64, @intCast(std.math.maxInt(usize)))) return null;
    const thread_index: usize = @intCast(index);
    if (thread_index >= scheduler_state.thread_table.contexts.len) return null;
    return thread_index;
}

fn externalSchedulerOwnsThread(thread_index: usize) bool {
    return scheduler_state.external.policy_thread != null and scheduler_state.external.policy_thread.? == thread_index;
}

fn threadActiveOnDifferentCpu(thread_index: usize, target_cpu: usize) bool {
    var cpu_slot: usize = 0;
    while (cpu_slot < scheduler_state.cpus.len) : (cpu_slot += 1) {
        if (cpu_slot == target_cpu) continue;
        if (scheduler_state.cpus[cpu_slot].current_thread == thread_index) return true;
        if (scheduler_state.cpus[cpu_slot].pending_commit_thread == thread_index) return true;
    }
    return false;
}

pub fn attachPolicyThread(thread_index: usize) bool {
    const ctx = threadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    scheduler_state.external.policy_thread = thread_index;
    scheduler_state.external.enabled = true;
    verifiedExitThread(thread_index);
    return true;
}

pub fn policyActive() bool {
    return scheduler_state.external.enabled and scheduler_state.external.policy_thread != null;
}

pub fn eventQueueReadable() bool {
    scheduler_state.external.lock();
    defer scheduler_state.external.unlock();
    return scheduler_state.external.event_len != 0;
}

pub fn pendingEventCount() u64 {
    scheduler_state.external.lock();
    defer scheduler_state.external.unlock();
    return @intCast(scheduler_state.external.event_len);
}

fn schedulerTraceEnabled(mask: u64) bool {
    return (scheduler_state.external.trace_mask & mask) != 0;
}

fn traceSchedulerEvent(comptime event: []const u8, mask: u64, a0: u64, a1: u64, a2: u64, a3: u64, a4: u64, a5: u64) void {
    if (!schedulerTraceEnabled(mask)) return;
    kernel_log.writeFmt(
        "[trace] c=sched e={s} tsc={} a0={} a1={} a2={} a3={} a4={} a5={}\n",
        .{ event, x86_platform.readTimestampCounter(), a0, a1, a2, a3, a4, a5 },
    );
}

fn noteExternalSchedulerEventEnqueued(sequence: u64, event_type: u16, queue_len_after_enqueue: usize) void {
    traceSchedulerEvent("event.enqueue", scheduler_trace_metric, sequence, event_type, queue_len_after_enqueue, 0, 0, 0);
}

fn noteExternalSchedulerEventRead(sequence: u64) void {
    traceSchedulerEvent("event.read", scheduler_trace_metric, sequence, 0, 0, 0, 0, 0);
}

pub fn externalSchedulerTraceTimestamp() u64 {
    return x86_platform.readTimestampCounter();
}

pub fn traceExternalSchedulerCommitIoctl(start_tsc: u64) void {
    const cycles = x86_platform.readTimestampCounter() -% start_tsc;
    traceSchedulerEvent("commit.ioctl", scheduler_trace_metric, cycles, 0, 0, 0, 0, 0);
}

fn traceExternalSchedulerCommitPath(comptime path: []const u8, cpu_slot: usize, thread_index: usize) void {
    traceSchedulerEvent("commit.path." ++ path, scheduler_trace_metric, cpu_slot, thread_index, 0, 0, 0, 0);
}

fn dropExternalSchedulerEventAtLocked(remove_offset: usize) void {
    if (remove_offset >= scheduler_state.external.event_len) return;
    var offset = remove_offset;
    while (offset + 1 < scheduler_state.external.event_len) : (offset += 1) {
        const dst = (scheduler_state.external.event_head + offset) % scheduler_state.external.events.len;
        const src = (scheduler_state.external.event_head + offset + 1) % scheduler_state.external.events.len;
        scheduler_state.external.events[dst] = scheduler_state.external.events[src];
    }
    scheduler_state.external.event_len -= 1;
}

fn dropOneExternalSchedulerIdleEventLocked() bool {
    var offset: usize = 0;
    while (offset < scheduler_state.external.event_len) : (offset += 1) {
        const index = (scheduler_state.external.event_head + offset) % scheduler_state.external.events.len;
        const event = scheduler_state.external.events[index];
        if (event.event_type == scheduler_abi.event_cpu_idle and event.thread_id == scheduler_abi.no_thread) {
            dropExternalSchedulerEventAtLocked(offset);
            return true;
        }
    }
    return false;
}

fn enqueueExternalSchedulerEvent(
    event_type: u16,
    cpu_id: usize,
    thread_index: ?usize,
    runtime_ns: u64,
) bool {
    if (!policyActive()) return false;
    scheduler_state.external.lock();
    defer scheduler_state.external.unlock();
    if (scheduler_state.external.event_len >= scheduler_state.external.events.len) {
        if (event_type == scheduler_abi.event_cpu_idle or !dropOneExternalSchedulerIdleEventLocked()) {
            return false;
        }
    }
    const tail = (scheduler_state.external.event_head + scheduler_state.external.event_len) % scheduler_state.external.events.len;
    var event = ExternalSchedulerEvent{
        .event_type = event_type,
        .sequence = scheduler_state.external.next_sequence,
        .cpu_id = @intCast(cpu_id),
        .runtime_ns = runtime_ns,
    };
    scheduler_state.external.next_sequence +%= 1;
    if (thread_index) |idx| {
        const ctx = threadContext(idx) orelse return false;
        event.thread_id = externalThreadId(idx);
        event.generation = ctx.generation;
        event.weight = 1024;
        event.slice_ns = 4_000_000;
    }
    scheduler_state.external.events[tail] = event;
    scheduler_state.external.event_len += 1;
    noteExternalSchedulerEventEnqueued(event.sequence, event.event_type, scheduler_state.external.event_len);
    return true;
}

pub fn publishThreadReady(thread_index: usize) void {
    if (externalSchedulerOwnsThread(thread_index)) return;
    const queued = enqueueExternalSchedulerEvent(
        scheduler_abi.event_thread_ready,
        threadAssignedCpuSlot(thread_index),
        thread_index,
        0,
    );
    if (queued) @atomicStore(u8, &scheduler_state.external.ready_event_seen, 1, .release);
}

pub fn readPolicyEventBytes(out: []u8) ?usize {
    if (out.len < scheduler_abi.sched_event_size) return null;
    scheduler_state.external.lock();
    defer scheduler_state.external.unlock();
    if (scheduler_state.external.event_len == 0) return 0;
    const event = scheduler_state.external.events[scheduler_state.external.event_head];
    scheduler_state.external.event_head = (scheduler_state.external.event_head + 1) % scheduler_state.external.events.len;
    scheduler_state.external.event_len -= 1;
    noteExternalSchedulerEventRead(event.sequence);
    verifiedApplyExternalEvent(event);
    const bytes = std.mem.asBytes(&event);
    @memcpy(out[0..bytes.len], bytes);
    return bytes.len;
}

pub fn commitPolicyDecision(
    cpu_id: u32,
    thread_id: u64,
    generation: u64,
    frame: *TrapFrame,
    saved_rax: u64,
) bool {
    if (!policyActive()) return false;
    if (cpu_id >= smp.max_cpus) return false;
    const target_cpu: usize = @intCast(cpu_id);
    const target_state = schedulerStateForSlot(target_cpu) orelse return false;
    if (thread_id == scheduler_abi.no_thread) {
        verifiedProbeCommit(target_cpu, thread_id, generation);
        target_state.lock.lock();
        target_state.pending_commit_thread = null;
        target_state.pending_commit_generation = 0;
        target_state.idle_event_pending = target_cpu != bootstrap_cpu_slot;
        target_state.lock.unlock();
        traceExternalSchedulerCommitPath("idle", target_cpu, 0);
        return true;
    }
    if (generation > std.math.maxInt(u32)) return false;
    const thread_index = threadIndexFromExternalId(thread_id) orelse return false;
    const expected_generation: u32 = @intCast(generation);
    scheduler_state.thread_table.lock();
    const initial_ctx = threadContextMutable(thread_index) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    if (!initial_ctx.allocated or initial_ctx.generation != expected_generation) {
        scheduler_state.thread_table.unlock();
        return false;
    }
    scheduler_state.thread_table.unlock();
    if (externalSchedulerOwnsThread(thread_index)) return false;
    verifiedProbeCommit(target_cpu, thread_id, generation);

    if (target_cpu == currentCpu()) {
        if (threadActiveOnDifferentCpu(thread_index, target_cpu)) return false;
        scheduler_state.thread_table.lock();
        const ctx = threadContextMutable(thread_index) orelse {
            scheduler_state.thread_table.unlock();
            return false;
        };
        if (!ctx.allocated or ctx.generation != expected_generation) {
            scheduler_state.thread_table.unlock();
            return false;
        }
        ctx.cpu_slot = target_cpu;
        if (!markThreadReadyLocked(thread_index, true, false)) {
            scheduler_state.thread_table.unlock();
            return false;
        }
        scheduler_state.thread_table.unlock();
        const current_thread = currentThread();
        if (thread_index == current_thread) {
            frame.rax = saved_rax;
            traceExternalSchedulerCommitPath("same_no_switch", target_cpu, thread_index);
            return true;
        }
        const switched = switchTo(thread_index, frame, saved_rax);
        if (switched) traceExternalSchedulerCommitPath("same_switch", target_cpu, thread_index);
        return switched;
    }

    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    if (!target_state.enabled) return false;
    if (target_state.current_thread == thread_index) {
        traceExternalSchedulerCommitPath("remote_noop", target_cpu, thread_index);
        return true;
    }
    if (target_state.pending_commit_thread == thread_index) {
        traceExternalSchedulerCommitPath("remote_noop", target_cpu, thread_index);
        return true;
    }
    if (threadActiveOnDifferentCpu(thread_index, target_cpu)) return false;
    scheduler_state.thread_table.lock();
    const ctx = threadContextMutable(thread_index) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    if (!ctx.allocated or ctx.generation != expected_generation) {
        scheduler_state.thread_table.unlock();
        return false;
    }
    ctx.cpu_slot = target_cpu;
    if (!markThreadReadyLocked(thread_index, true, false)) {
        scheduler_state.thread_table.unlock();
        return false;
    }
    target_state.pending_commit_thread = thread_index;
    target_state.pending_commit_generation = @intCast(generation);
    target_state.idle_event_pending = false;
    scheduler_state.thread_table.unlock();
    logExternalSchedulerApCommitOnce(target_cpu, thread_index);
    _ = smp.wakeCpu(target_cpu);
    traceExternalSchedulerCommitPath("remote_pending", target_cpu, thread_index);
    return true;
}

fn claimExternalCommittedUserEntry(cpu_slot: usize, out_entry: *scheduler_observer.UserEntry) bool {
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    const thread_index = state.pending_commit_thread orelse return false;
    const generation = state.pending_commit_generation;
    const ctx = threadContext(thread_index) orelse return false;
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    logExternalSchedulerApClaimTrace(cpu_slot, thread_index, generation, ctx, hot);
    if (!ctx.allocated or hot.allocated == 0) return false;
    if (ctx.generation != generation) return false;
    if (!fillUserEntryForThread(cpu_slot, thread_index, out_entry)) return false;
    state.current_thread = thread_index;
    state.current_principal = ctx.owner_process;
    state.current_cr3 = ctx.cr3;
    state.pending_commit_thread = null;
    state.pending_commit_generation = 0;
    state.idle_event_pending = false;
    state.is_idle = false;
    logExternalSchedulerApEntryOnce(cpu_slot, thread_index);
    return true;
}

fn logExternalSchedulerApClaimTrace(cpu_slot: usize, thread_index: usize, generation: u32, ctx: *const ThreadContext, hot: *const ThreadHotState) void {
    if (!schedulerTraceEnabled(scheduler_trace_ap_entry)) return;
    const old = @atomicRmw(u32, &scheduler_state.external.ap_claim_trace_count, .Add, 1, .acq_rel);
    traceSchedulerEvent(
        "ap.claim",
        scheduler_trace_ap_entry,
        cpu_slot,
        thread_index,
        generation,
        if (ctx.allocated) 1 else 0,
        hot.allocated,
        old,
    );
}

fn logExternalSchedulerApEntryOnce(cpu_slot: usize, thread_index: usize) void {
    traceSchedulerEvent("ap.entry", scheduler_trace_ap_entry, cpu_slot, thread_index, 0, 0, 0, 0);
}

fn logExternalSchedulerApCommitOnce(cpu_slot: usize, thread_index: usize) void {
    traceSchedulerEvent("ap.commit", scheduler_trace_commit, cpu_slot, thread_index, 0, 0, 0, 0);
}

fn logExternalSchedulerApTickOnce(cpu_slot: usize, thread_index: usize) void {
    traceSchedulerEvent("ap.tick", scheduler_trace_ap_tick, cpu_slot, thread_index, 0, 0, 0, 0);
}

fn logExternalSchedulerApBlockOnce(cpu_slot: usize, thread_index: usize, wait_mailbox: bool, timeout_ticks: u64, wake_tick: u64) void {
    traceSchedulerEvent(
        "ap.block",
        scheduler_trace_ap_block,
        cpu_slot,
        thread_index,
        if (wait_mailbox) 1 else 0,
        timeout_ticks,
        wake_tick,
        0,
    );
}

fn logExternalSchedulerWakeOnce(thread_index: usize) void {
    traceSchedulerEvent("wake", scheduler_trace_wake, thread_index, currentCpu(), 0, 0, 0, 0);
}

fn logExternalSchedulerTimerDueOnce(thread_index: usize, now_tick: u64, wake_tick: u64) void {
    traceSchedulerEvent("timer.due", scheduler_trace_timer_due, thread_index, now_tick, wake_tick, 0, 0, 0);
}

fn observeCpuIdleFromAp(cpu_slot: usize) callconv(.c) void {
    if (cpu_slot == bootstrap_cpu_slot) return;
    const state = schedulerStateForSlot(cpu_slot) orelse return;
    var should_emit = false;
    state.lock.lock();
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
    const ready_seen = @atomicLoad(u8, &scheduler_state.external.ready_event_seen, .acquire) != 0;
    if (ap_user_dispatch_enabled and ready_seen and policyActive() and state.pending_commit_thread == null and !state.idle_event_pending) {
        state.idle_event_pending = true;
        should_emit = true;
    }
    state.lock.unlock();
    if (should_emit) {
        if (!enqueueExternalSchedulerEvent(scheduler_abi.event_cpu_idle, cpu_slot, null, 0)) {
            state.lock.lock();
            state.idle_event_pending = false;
            state.lock.unlock();
        }
    }
}

fn claimExternalCommittedUserEntryFromAp(cpu_slot: usize, out_entry: *scheduler_observer.UserEntry) callconv(.c) bool {
    if (policyActive()) return claimExternalCommittedUserEntry(cpu_slot, out_entry);
    return claimKernelScheduledUserEntry(cpu_slot, out_entry);
}

fn markThreadReadyLocked(thread_index: usize, ready: bool, notify: bool) bool {
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    if (ready and ctx.stopped) return false;
    const was_ready = ctx.ready;
    if (ready) schedulerFullMemoryFence();
    ctx.ready = ready;
    setThreadHotReady(thread_index, ready);
    if (ready and !was_ready) {
        if (!policyActive()) verifiedWakeThread(thread_index);
    } else if (!ready and was_ready) {
        if (!policyActive()) verifiedBlockThread(thread_index);
    }
    if (notify and ready and !was_ready) publishThreadReady(thread_index);
    return true;
}

fn markThreadReadyInternal(thread_index: usize, ready: bool, notify: bool) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return markThreadReadyLocked(thread_index, ready, notify);
}

pub fn markThreadReady(thread_index: usize, ready: bool) bool {
    return markThreadReadyInternal(thread_index, ready, true);
}

pub fn saveAndParkApUserThread(frame: *const TrapFrame, runtime_ns: u64) bool {
    const cpu_slot = currentCpu();
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    const thread_index = currentThread();
    if (thread_index >= scheduler_state.thread_table.contexts.len) return false;
    if (externalSchedulerOwnsThread(thread_index)) return false;
    state.lock.lock();
    scheduler_state.thread_table.lock();
    const ctx = threadContextMutable(thread_index) orelse {
        scheduler_state.thread_table.unlock();
        state.lock.unlock();
        return false;
    };
    if (!ctx.allocated) {
        scheduler_state.thread_table.unlock();
        state.lock.unlock();
        return false;
    }
    ctx.frame = frame.*;
    ctx.cr3 = currentCr3();
    ctx.fs_base = x86_platform.readFsBase();
    ctx.gs_base = x86_platform.readGsBase();
    ctx.pkru = x86_platform.readPkru();
    ctx.ready = true;
    setThreadHotCr3(thread_index, ctx.cr3);
    setThreadHotReady(thread_index, true);
    const owner_process = ctx.owner_process;
    const cr3 = ctx.cr3;
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
    state.tick_accum = 0;
    scheduler_state.thread_table.unlock();
    state.lock.unlock();
    if (!policyActive()) {
        verifiedChargeAndFinish(cpu_slot, thread_index, runtime_ns);
        return true;
    }
    if (!enqueueExternalSchedulerEvent(scheduler_abi.event_tick, cpu_slot, thread_index, runtime_ns)) {
        state.lock.lock();
        state.current_thread = thread_index;
        state.current_principal = owner_process;
        state.current_cr3 = cr3;
        state.is_idle = false;
        state.lock.unlock();
        return false;
    }
    logExternalSchedulerApTickOnce(cpu_slot, thread_index);
    return true;
}

pub fn preemptApUserThread(quantum_ticks: u64, frame: *const TrapFrame) bool {
    if (quantum_ticks == 0) return false;
    const cpu_slot = currentCpu();
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    state.tick_accum +%= 1;
    const should_preempt = state.tick_accum >= quantum_ticks;
    state.lock.unlock();
    if (!should_preempt) return false;
    return saveAndParkApUserThread(frame, quantum_ticks * external_runtime_ns_per_tick);
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
        if (!policyActive()) verifiedBlockThread(current_thread);
        if (policyActive()) {
            _ = enqueueExternalSchedulerEvent(
                scheduler_abi.event_thread_blocked,
                currentCpu(),
                current_thread,
                0,
            );
        }
        logExternalSchedulerApBlockOnce(currentCpu(), current_thread, wait_mailbox, timeout_ticks, ctx.wake_tick);
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

fn switchToExternalScheduler(frame: *TrapFrame, saved_rax: ?u64) bool {
    if (!policyActive()) return false;
    const policy_thread = scheduler_state.external.policy_thread.?;
    if (!markThreadReadyInternal(policy_thread, true, false)) return false;
    if (policy_thread == currentThread()) {
        if (saved_rax) |value| frame.rax = value;
        return true;
    }
    return switchTo(policy_thread, frame, saved_rax);
}

pub fn preemptBootstrapThread(quantum_ticks: u64, frame: *TrapFrame) bool {
    if (quantum_ticks == 0) return false;
    const current_thread = currentThread();
    if (policyActive() and externalSchedulerOwnsThread(current_thread)) return false;

    const state = schedulerStateForSlot(currentCpu()) orelse return false;
    state.lock.lock();
    state.tick_accum +%= 1;
    const should_preempt = state.tick_accum >= quantum_ticks;
    if (should_preempt) state.tick_accum = 0;
    state.lock.unlock();
    if (!should_preempt) return false;

    if (!policyActive()) {
        verifiedChargeAndFinish(currentCpu(), current_thread, quantum_ticks * external_runtime_ns_per_tick);
        return switchToNextReadyOnCurrentCpu(frame, null);
    }

    _ = enqueueExternalSchedulerEvent(
        scheduler_abi.event_tick,
        currentCpu(),
        current_thread,
        quantum_ticks * external_runtime_ns_per_tick,
    );
    if (!policyActive()) verifiedChargeAndFinish(currentCpu(), current_thread, quantum_ticks * external_runtime_ns_per_tick);
    return switchToExternalScheduler(frame, null);
}

pub fn loadPolicyThread(frame: *TrapFrame) bool {
    if (!policyActive()) return false;
    const policy_thread = scheduler_state.external.policy_thread.?;
    if (!markThreadReadyInternal(policy_thread, true, false)) return false;
    if (!activate(policy_thread)) return false;
    return loadContextIntoFrame(policy_thread, frame);
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
    const capacity = nextThreadCapacity(required) orelse return false;
    const new_contexts = allocKernelSlice(ThreadContext, free_list, capacity) orelse return false;
    const new_hot_threads = allocKernelSlice(ThreadHotState, free_list, capacity) orelse return false;

    @memcpy(new_contexts[0..scheduler_state.thread_table.contexts.len], scheduler_state.thread_table.contexts);
    @memcpy(new_hot_threads[0..scheduler_state.thread_table.hot_states.len], scheduler_state.thread_table.hot_states);
    var i = scheduler_state.thread_table.contexts.len;
    while (i < capacity) : (i += 1) {
        new_contexts[i] = .{ .id = @intCast(i) };
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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_state), &scheduler_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(lapic_tick_count), &lapic_tick_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_tick_accum), &scheduler_tick_accum));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_switch_count), &scheduler_switch_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_fx_state), &initial_fx_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_interrupt_fx_state), &kernel_interrupt_fx_state));
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

/// Snapshot CPUs that currently own `target_cr3`.  User CR3 loads never set
/// the no-flush bit, so a CPU that enters this address space after the
/// snapshot will invalidate the PCID as part of that load.
pub fn cpuMaskRunningCr3(target_cr3: u64) u64 {
    if (target_cr3 == 0) return 0;
    var mask: u64 = 0;
    var cpu_slot: usize = 0;
    while (cpu_slot < scheduler_state.cpus.len and cpu_slot < 64) : (cpu_slot += 1) {
        const state = &scheduler_state.cpus[cpu_slot];
        state.lock.lock();
        const owns_cr3 = state.enabled and !state.is_idle and state.current_cr3 == target_cr3;
        state.lock.unlock();
        if (owns_cr3) mask |= @as(u64, 1) << @intCast(cpu_slot);
    }
    return mask;
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
    if (!verifiedCoreReady() or policyActive()) return bootstrap_cpu_slot;
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
    scheduler_observer.registerIdleUserEntryPoll(claimExternalCommittedUserEntryFromAp);
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
    return policyActive() or verifiedCoreReady();
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
        "[sched-debug] reason={s} cpu={} policy={} verified={}\n",
        .{
            reason,
            currentCpu(),
            if (policyActive()) @as(u8, 1) else @as(u8, 0),
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
            if (!policyActive()) {
                verifiedAddThread(i, scheduler_state.thread_table.contexts[i].generation, initial_ready);
            } else if (initial_ready) {
                publishThreadReady(i);
            }
            return i;
        }
        if (!ensureThreadCapacity(scheduler_state.thread_table.contexts.len + 1, free_list)) return null;
    }
}

pub fn releaseThread(thread_index: usize) bool {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();

    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    const was_external_policy = externalSchedulerOwnsThread(thread_index);
    if (!was_external_policy and !policyActive()) verifiedExitThread(thread_index);
    if (!was_external_policy) {
        _ = enqueueExternalSchedulerEvent(
            scheduler_abi.event_thread_exited,
            threadAssignedCpuSlot(thread_index),
            thread_index,
            0,
        );
    }
    const next_generation = nextThreadGeneration(ctx.generation);
    clearThreadFromCpuSchedulerStatesLocked(thread_index);
    ctx.* = .{ .id = @intCast(thread_index), .generation = next_generation };
    if (was_external_policy) {
        scheduler_state.external.enabled = false;
        scheduler_state.external.policy_thread = null;
    }
    syncHotStateFromContext(thread_index);
    syncHotStateFromContext(thread_index);
    return true;
}

fn clearThreadFromCpuSchedulerStatesLocked(thread_index: usize) void {
    var cpu_slot: usize = 0;
    while (cpu_slot < scheduler_state.cpus.len) : (cpu_slot += 1) {
        const state = &scheduler_state.cpus[cpu_slot];
        if (state.current_thread == thread_index) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
        }
    }
}

pub fn activate(thread_index: usize) bool {
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    if (hot.allocated == 0) return false;
    if (hot.ready == 0) return false;
    const cpu_slot = currentCpu();
    const ctx = threadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
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

pub fn loadContextIntoFrame(thread_index: usize, frame: *TrapFrame) bool {
    const ctx = threadContext(thread_index) orelse return false;
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    if (hot.allocated == 0) return false;
    if (hot.ready == 0) return false;
    schedulerFullMemoryFence();
    x86_platform.writeFsBase(ctx.fs_base);
    x86_platform.writeGsBase(ctx.gs_base);
    x86_platform.writePkru(ctx.pkru);
    frame.* = ctx.frame;
    if (thread_index < scheduler_state.thread_table.ipc_wake_resume_tsc.len) {
        const wake_tsc = @atomicRmw(u64, &scheduler_state.thread_table.ipc_wake_resume_tsc[thread_index], .Xchg, 0, .acq_rel);
        if (wake_tsc != 0) {
            const elapsed = x86_platform.readTimestampCounter() -% wake_tsc;
            const wake_cpu = @atomicLoad(u16, &scheduler_state.thread_table.ipc_wake_resume_cpu[thread_index], .acquire);
            const resume_cpu: u16 = @intCast(@min(currentCpu(), std.math.maxInt(u16)));
            traceSchedulerEvent(
                "wake.resume",
                scheduler_trace_metric,
                thread_index,
                elapsed,
                wake_cpu,
                resume_cpu,
                if (wake_cpu == resume_cpu) 1 else 0,
                0,
            );
        }
    }
    return true;
}

pub fn switchTo(next_thread: usize, frame: *TrapFrame, saved_rax: ?u64) bool {
    if (!isBootstrapSchedulerCpu() and !apUserDispatchReady()) return false;
    if (next_thread >= scheduler_state.thread_table.contexts.len) return false;
    const current_thread = currentThread();
    if (next_thread == current_thread) {
        if (saved_rax) |value| frame.rax = value;
        return true;
    }

    var saved = frame.*;
    if (saved_rax) |value| saved.rax = value;
    saveCurrentThreadContextFromFrame(&saved);

    if (!activate(next_thread)) return false;
    if (!loadContextIntoFrame(next_thread, frame)) {
        _ = activate(current_thread);
        _ = loadContextIntoFrame(current_thread, frame);
        return false;
    }
    return true;
}

pub fn handoffToReadyThreadGenerationWithRax(
    frame: *TrapFrame,
    target_thread: usize,
    target_generation: u32,
    sender_rax: u64,
    before_current_thread_leave: ?BeforeCurrentThreadLeaveCallback,
) bool {
    if (policyActive() or !verifiedCoreReady()) return false;
    const cpu_id = currentCpu();
    if (cpu_id >= verifiedCoreCpuCount()) return false;
    const current_thread = currentThread();
    if (target_thread == current_thread) return false;
    if (externalSchedulerOwnsThread(current_thread) or externalSchedulerOwnsThread(target_thread)) return false;

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
    if (threadActiveOnDifferentCpu(target_thread, cpu_id)) {
        noteVerifiedCoreIssue("handoff-active-other-cpu", .state, target_thread);
        return false;
    }

    if (!verifiedHandoffToThreadOnCpu(cpu_id, target_thread, target_generation, current_thread, current_generation)) return false;
    if (before_current_thread_leave) |callback| callback.run(callback.context);
    if (switchTo(target_thread, frame, sender_rax)) return true;

    verifiedRollbackHandoff(cpu_id, current_thread, current_generation);
    return false;
}

pub fn prepareExitHandoffToReadyThreadGeneration(target_thread: usize, target_generation: u32) bool {
    if (policyActive() or !verifiedCoreReady()) return false;
    const cpu_id = currentCpu();
    if (cpu_id >= verifiedCoreCpuCount()) return false;
    const current_thread = currentThread();
    if (target_thread == current_thread) return false;
    if (externalSchedulerOwnsThread(current_thread) or externalSchedulerOwnsThread(target_thread)) return false;

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
    if (threadActiveOnDifferentCpu(target_thread, cpu_id)) {
        noteVerifiedCoreIssue("exit-handoff-active-other-cpu", .state, target_thread);
        return false;
    }
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
    saved_rax: u64,
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
    if (externalSchedulerOwnsThread(current_thread)) return false;
    if (!releaseThread(current_thread)) return false;
    if (after_release) |callback| callback.run(callback.context);
    if (!policyActive()) return switchToNextReadyOnCurrentCpu(frame, null);
    return switchToExternalScheduler(frame, saved_rax);
}

pub fn wakeIfWaiting(thread_index: usize) void {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextMutable(thread_index) orelse return;
    if (!ctx.allocated) return;
    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    if (ctx.stopped) {
        ctx.ready = false;
        setThreadHotWaitState(thread_index, false, 0, false);
        return;
    }
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    if (!policyActive()) verifiedWakeThread(thread_index);
    if (thread_index < scheduler_state.thread_table.ipc_wake_resume_tsc.len) {
        @atomicStore(u16, &scheduler_state.thread_table.ipc_wake_resume_cpu[thread_index], @intCast(@min(currentCpu(), std.math.maxInt(u16))), .release);
        @atomicStore(u64, &scheduler_state.thread_table.ipc_wake_resume_tsc[thread_index], x86_platform.readTimestampCounter(), .release);
    }
    logExternalSchedulerWakeOnce(thread_index);
    publishThreadReady(thread_index);
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
        ctx.ready = false;
        setThreadHotWaitState(thread_index, false, 0, false);
        return true;
    }
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    if (!policyActive()) verifiedWakeThread(thread_index);
    if (thread_index < scheduler_state.thread_table.ipc_wake_resume_tsc.len) {
        @atomicStore(u16, &scheduler_state.thread_table.ipc_wake_resume_cpu[thread_index], @intCast(@min(currentCpu(), std.math.maxInt(u16))), .release);
        @atomicStore(u64, &scheduler_state.thread_table.ipc_wake_resume_tsc[thread_index], x86_platform.readTimestampCounter(), .release);
    }
    logExternalSchedulerWakeOnce(thread_index);
    publishThreadReady(thread_index);
    return true;
}

pub fn wakeIfWaitingGeneration(thread_index: usize, generation: u32) bool {
    return wakeIfWaitingGenerationInternal(thread_index, generation, null, true);
}

pub fn wakeIfWaitingGenerationWithRax(thread_index: usize, generation: u32, resume_rax: u64) bool {
    return wakeIfWaitingGenerationInternal(thread_index, generation, resume_rax, true);
}

pub fn wakeBlockedGeneration(thread_index: usize, generation: u32) bool {
    return wakeIfWaitingGenerationInternal(thread_index, generation, null, false);
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
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    var i: usize = 0;
    while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
        const ctx = threadContextMutable(i) orelse continue;
        if (!ctx.allocated or ctx.owner_process != principal) continue;
        ctx.stopped = true;
        ctx.ready = false;
        setThreadHotStopped(i, true);
        setThreadHotReady(i, false);
        if (!policyActive()) verifiedBlockThread(i);
        stopped_count += 1;
    }
    return stopped_count;
}

pub fn continuePrincipalThreads(principal: kernel.PrincipalId) usize {
    var continued_count: usize = 0;
    var publish_buf: [initialThreadCapacity]usize = undefined;
    var publish_count: usize = 0;
    {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        var i: usize = 0;
        while (i < scheduler_state.thread_table.contexts.len) : (i += 1) {
            const ctx = threadContextMutable(i) orelse continue;
            if (!ctx.allocated or ctx.owner_process != principal or !ctx.stopped) continue;
            ctx.stopped = false;
            ctx.ready = true;
            setThreadHotStopped(i, false);
            setThreadHotReady(i, true);
            if (!policyActive()) verifiedWakeThread(i);
            if (publish_count < publish_buf.len) {
                publish_buf[publish_count] = i;
                publish_count += 1;
            }
            continued_count += 1;
        }
    }
    var i: usize = 0;
    while (i < publish_count) : (i += 1) {
        publishThreadReady(publish_buf[i]);
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
        logExternalSchedulerTimerDueOnce(i, now_tick, hot.wake_tick);
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
        // AP user threads must leave the CPU for both the built-in and the
        // external scheduler.  Returning false here turns a real blocking
        // receive into NOT_READY as soon as a process is scheduled on an AP.
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
    if (!policyActive()) verifiedBlockThread(current_thread);

    if (!policyActive()) {
        scheduler_state.thread_table.unlock();
        if (before_block) |callback| callback.run(callback.context);
        if (switchToNextReadyOnCurrentCpu(frame, resume_rax)) return true;
        // There is no userspace context to return to while the caller is
        // blocked.  Keep CPU 0 interruptible until a wakeup publishes a ready
        // thread, then load the wake-provided frame without overwriting rax
        // with the original syscall's resume value.
        while (!loadNextReadyThread(frame)) {
            asm volatile ("sti; hlt; cli" ::: .{ .memory = true });
        }
        return true;
    }

    if (policyActive() and !externalSchedulerOwnsThread(current_thread)) {
        _ = enqueueExternalSchedulerEvent(
            scheduler_abi.event_thread_blocked,
            currentCpu(),
            current_thread,
            0,
        );
        scheduler_state.thread_table.unlock();
        if (before_block) |callback| callback.run(callback.context);
        if (switchToExternalScheduler(frame, resume_rax)) return true;
        scheduler_state.thread_table.lock();
        if (threadContextMutable(current_thread)) |restored_ctx| {
            if (restored_ctx.allocated) {
                restored_ctx.wait_mailbox = false;
                restored_ctx.wake_tick = 0;
                restored_ctx.ready = true;
                setThreadHotWaitState(current_thread, false, 0, true);
            }
        }
        scheduler_state.thread_table.unlock();
        return false;
    }

    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    ctx.ready = true;
    setThreadHotWaitState(current_thread, false, 0, true);
    scheduler_state.thread_table.unlock();
    return false;
}
