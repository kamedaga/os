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
    signal_pending: bool = false,
    wake_tick: u64 = 0,
    frame: TrapFrame = std.mem.zeroes(TrapFrame),
    fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes,
};

const ThreadHotState = extern struct {
    allocated: u8 = 0,
    ready: u8 = 0,
    signal_pending: u8 = 0,
    wait_mailbox: u8 = 0,
    owner_process: kernel.PrincipalId = default_process_principal,
    _pad0: [8]u8 = [_]u8{0} ** 8,
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
var thread_contexts: []ThreadContext = initial_thread_contexts[0..];
pub export var thread_contexts_ptr: *anyopaque = @ptrCast(initial_thread_contexts[0..].ptr);
var initial_thread_hot_states: [initialThreadCapacity]ThreadHotState = buildInitialThreadHotStates();
var thread_hot_states: []ThreadHotState = initial_thread_hot_states[0..];
var cpu_scheduler_states: [smp.max_cpus]CpuSchedulerState = buildInitialCpuSchedulerStates();
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

var external_scheduler_enabled: bool = false;
var external_scheduler_policy_thread: ?usize = null;
var external_scheduler_event_lock: SchedulerSpinLock = .{};
var external_scheduler_events: [max_external_scheduler_events]ExternalSchedulerEvent =
    [_]ExternalSchedulerEvent{.{}} ** max_external_scheduler_events;
var external_scheduler_event_head: usize = 0;
var external_scheduler_event_len: usize = 0;
var external_scheduler_next_sequence: u64 = 1;
var external_scheduler_ap_commit_log_mask: u64 = 0;
var external_scheduler_ap_entry_log_mask: u64 = 0;
var external_scheduler_ap_tick_log_mask: u64 = 0;
var external_scheduler_ap_block_log_mask: u64 = 0;
var external_scheduler_wake_log_mask: u64 = 0;
var external_scheduler_timer_due_log_mask: u64 = 0;
var external_scheduler_ap_claim_trace_count: u32 = 0;
var external_scheduler_ready_event_seen: u8 = 0;
var thread_table_lock: SchedulerSpinLock = .{};
var verified_core_lock: SchedulerSpinLock = .{};
var verified_core_state: verified_sched.State = undefined;
var verified_core_probe_state: verified_sched.State = undefined;
var verified_core_runqueue_scratch: verified_sched.Runqueue = undefined;
var verified_core_pick_scratch: verified_sched.PickResult = undefined;
var verified_core_decision: verified_sched.Decision = undefined;
var verified_core_initialized: bool = false;
var verified_core_faulted: bool = false;
var verified_core_log_count: u32 = 0;

fn verifiedCoreCpuCount() usize {
    return @min(smp.max_cpus, verified_sched.sched_max_cpus);
}

fn verifiedThreadId(thread_index: usize) ?i64 {
    const raw_id = thread_index + 1;
    if (raw_id == 0 or raw_id > @as(usize, @intCast(std.math.maxInt(i64)))) return null;
    return @intCast(raw_id);
}

fn verifiedCoreReady() bool {
    return verified_core_initialized and !verified_core_faulted;
}

fn noteVerifiedCoreIssue(comptime op: []const u8, rc: verified_sched.SchedRc, thread_index: ?usize) void {
    const old = @atomicRmw(u32, &verified_core_log_count, .Add, 1, .acq_rel);
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
    const old = @atomicRmw(u32, &verified_core_log_count, .Add, 1, .acq_rel);
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
    verified_core_lock.lock();
    defer verified_core_lock.unlock();
    const add_rc = verified_sched.pacha_sched_add_thread(
        &verified_core_state,
        thread_id,
        @intCast(generation),
        1024,
        4_000_000,
        &verified_core_decision,
        &verified_core_runqueue_scratch,
    );
    if (add_rc != .ok) {
        noteVerifiedCoreIssue("add", add_rc, thread_index);
        return;
    }
    if (!ready) {
        const block_rc = verified_sched.pacha_sched_block_thread(
            &verified_core_state,
            thread_id,
            &verified_core_decision,
            &verified_core_runqueue_scratch,
        );
        if (block_rc != .ok) noteVerifiedCoreIssue("add-block", block_rc, thread_index);
    }
}

fn verifiedWakeThread(thread_index: usize) void {
    if (!verifiedCoreReady()) return;
    if (externalSchedulerOwnsThread(thread_index)) return;
    const thread_id = verifiedThreadId(thread_index) orelse return;
    verified_core_lock.lock();
    defer verified_core_lock.unlock();
    const rc = verified_sched.pacha_sched_wake_thread(
        &verified_core_state,
        thread_id,
        &verified_core_decision,
        &verified_core_runqueue_scratch,
    );
    if (rc != .ok) noteVerifiedCoreIssue("wake", rc, thread_index);
}

fn verifiedBlockThread(thread_index: usize) void {
    if (!verifiedCoreReady()) return;
    if (externalSchedulerOwnsThread(thread_index)) return;
    const thread_id = verifiedThreadId(thread_index) orelse return;
    verified_core_lock.lock();
    defer verified_core_lock.unlock();
    const rc = verified_sched.pacha_sched_block_thread(
        &verified_core_state,
        thread_id,
        &verified_core_decision,
        &verified_core_runqueue_scratch,
    );
    if (rc != .ok) noteVerifiedCoreIssue("block", rc, thread_index);
}

fn verifiedExitThread(thread_index: usize) void {
    if (!verifiedCoreReady()) return;
    const thread_id = verifiedThreadId(thread_index) orelse return;
    verified_core_lock.lock();
    defer verified_core_lock.unlock();
    const rc = verified_sched.pacha_sched_exit_thread(
        &verified_core_state,
        thread_id,
        &verified_core_decision,
        &verified_core_runqueue_scratch,
    );
    if (rc != .ok) noteVerifiedCoreIssue("exit", rc, thread_index);
}

fn verifiedFinishCpu(cpu_id: usize) void {
    if (!verifiedCoreReady()) return;
    if (cpu_id >= verifiedCoreCpuCount()) return;
    verified_core_lock.lock();
    defer verified_core_lock.unlock();
    const rc = verified_sched.pacha_sched_finish_current(
        &verified_core_state,
        cpu_id,
        &verified_core_decision,
        &verified_core_runqueue_scratch,
    );
    if (rc != .ok) noteVerifiedCoreIssue("finish", rc, null);
}

fn verifiedChargeAndFinish(cpu_id: usize, thread_index: usize, runtime_ns: u64) void {
    if (!verifiedCoreReady()) return;
    if (cpu_id >= verifiedCoreCpuCount()) return;
    if (runtime_ns > @as(u64, @intCast(std.math.maxInt(i64)))) return;
    verified_core_lock.lock();
    defer verified_core_lock.unlock();
    const timer_rc = verified_sched.pacha_sched_on_timer(
        &verified_core_state,
        cpu_id,
        @intCast(runtime_ns),
        &verified_core_decision,
        &verified_core_runqueue_scratch,
    );
    if (timer_rc != .ok) {
        noteVerifiedCoreIssue("timer", timer_rc, thread_index);
        return;
    }
    const finish_rc = verified_sched.pacha_sched_finish_current(
        &verified_core_state,
        cpu_id,
        &verified_core_decision,
        &verified_core_runqueue_scratch,
    );
    if (finish_rc != .ok) noteVerifiedCoreIssue("timer-finish", finish_rc, thread_index);
}

fn verifiedApplyReadyEventLocked(event: ExternalSchedulerEvent) void {
    if (event.thread_id == scheduler_abi.no_thread) return;
    if (event.thread_id > @as(u64, @intCast(std.math.maxInt(i64)))) return;
    if (event.generation > @as(u64, @intCast(std.math.maxInt(i64)))) return;
    const weight = if (event.weight == 0) @as(u64, 1024) else event.weight;
    const slice_ns = if (event.slice_ns == 0) @as(u64, 4_000_000) else event.slice_ns;
    if (weight > @as(u64, @intCast(std.math.maxInt(i64))) or slice_ns > @as(u64, @intCast(std.math.maxInt(i64)))) return;
    const add_rc = verified_sched.pacha_sched_add_thread(
        &verified_core_state,
        @intCast(event.thread_id),
        @intCast(event.generation),
        @intCast(weight),
        @intCast(slice_ns),
        &verified_core_decision,
        &verified_core_runqueue_scratch,
    );
    if (add_rc == .ok) return;
    const wake_rc = verified_sched.pacha_sched_wake_thread(
        &verified_core_state,
        @intCast(event.thread_id),
        &verified_core_decision,
        &verified_core_runqueue_scratch,
    );
    if (wake_rc != .ok and wake_rc != .state) noteVerifiedCoreIssue("event-ready", wake_rc, threadIndexFromExternalId(event.thread_id));
}

fn verifiedApplyExternalEvent(event: ExternalSchedulerEvent) void {
    if (!verifiedCoreReady()) return;
    verified_core_lock.lock();
    defer verified_core_lock.unlock();
    switch (event.event_type) {
        scheduler_abi.event_thread_ready => verifiedApplyReadyEventLocked(event),
        scheduler_abi.event_thread_blocked => {
            if (event.thread_id == scheduler_abi.no_thread or event.thread_id > @as(u64, @intCast(std.math.maxInt(i64)))) return;
            const rc = verified_sched.pacha_sched_block_thread(
                &verified_core_state,
                @intCast(event.thread_id),
                &verified_core_decision,
                &verified_core_runqueue_scratch,
            );
            if (rc != .ok and rc != .invalid and rc != .state) noteVerifiedCoreIssue("event-block", rc, threadIndexFromExternalId(event.thread_id));
        },
        scheduler_abi.event_thread_exited => {
            if (event.thread_id == scheduler_abi.no_thread or event.thread_id > @as(u64, @intCast(std.math.maxInt(i64)))) return;
            const rc = verified_sched.pacha_sched_exit_thread(
                &verified_core_state,
                @intCast(event.thread_id),
                &verified_core_decision,
                &verified_core_runqueue_scratch,
            );
            if (rc != .ok and rc != .invalid and rc != .state) noteVerifiedCoreIssue("event-exit", rc, threadIndexFromExternalId(event.thread_id));
        },
        scheduler_abi.event_thread_yield, scheduler_abi.event_tick => {
            if (event.cpu_id >= verifiedCoreCpuCount()) return;
            if (event.thread_id != scheduler_abi.no_thread) {
                if (event.runtime_ns > @as(u64, @intCast(std.math.maxInt(i64)))) return;
                const timer_rc = verified_sched.pacha_sched_on_timer(
                    &verified_core_state,
                    @intCast(event.cpu_id),
                    @intCast(event.runtime_ns),
                    &verified_core_decision,
                    &verified_core_runqueue_scratch,
                );
                if (timer_rc != .ok) {
                    noteVerifiedCoreIssue("event-timer", timer_rc, threadIndexFromExternalId(event.thread_id));
                    return;
                }
            }
            const finish_rc = verified_sched.pacha_sched_finish_current(
                &verified_core_state,
                @intCast(event.cpu_id),
                &verified_core_decision,
                &verified_core_runqueue_scratch,
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
    return decision.kind == .run_thread and
        decision.cpu_id == cpu_id and
        decision.thread_id == @as(i64, @intCast(thread_id)) and
        decision.generation == @as(i64, @intCast(generation));
}

fn verifiedProbeCommit(cpu_id: usize, thread_id: u64, generation: u64) void {
    if (!verifiedCoreReady()) return;
    if (cpu_id >= verifiedCoreCpuCount()) return;
    verified_core_lock.lock();
    defer verified_core_lock.unlock();
    verified_core_probe_state = verified_core_state;
    const rc = verified_sched.pacha_sched_pick(
        &verified_core_probe_state,
        cpu_id,
        &verified_core_decision,
        &verified_core_pick_scratch,
        &verified_core_runqueue_scratch,
    );
    if (rc != .ok) {
        noteVerifiedCoreIssue("pick", rc, null);
        return;
    }
    if (!verifiedCommitMatches(cpu_id, thread_id, generation, verified_core_decision)) {
        noteVerifiedCoreMismatch(cpu_id, thread_id, generation, verified_core_decision);
        return;
    }
    verified_core_state = verified_core_probe_state;
}

fn nextThreadGeneration(current: u32) u32 {
    const next = current +% 1;
    return if (next == 0) 1 else next;
}

pub fn initializeStaticStorage() void {
    var cpu_slot: usize = 0;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        cpu_scheduler_states[cpu_slot] = .{};
    }
    cpu_scheduler_states[bootstrap_cpu_slot].enabled = true;
    cpu_scheduler_states[bootstrap_cpu_slot].current_thread = 0;
    cpu_scheduler_states[bootstrap_cpu_slot].is_idle = false;
    external_scheduler_enabled = false;
    external_scheduler_policy_thread = null;
    external_scheduler_event_head = 0;
    external_scheduler_event_len = 0;
    external_scheduler_next_sequence = 1;
    external_scheduler_ap_commit_log_mask = 0;
    external_scheduler_ap_entry_log_mask = 0;
    external_scheduler_ap_tick_log_mask = 0;
    external_scheduler_ap_block_log_mask = 0;
    external_scheduler_wake_log_mask = 0;
    external_scheduler_timer_due_log_mask = 0;
    external_scheduler_ap_claim_trace_count = 0;
    @atomicStore(u8, &external_scheduler_ready_event_seen, 0, .release);
    verified_sched.pacha_sched_empty_state(verifiedCoreCpuCount(), &verified_core_state);
    verified_sched.pacha_sched_empty_state(verifiedCoreCpuCount(), &verified_core_probe_state);
    verified_core_initialized = true;
    verified_core_faulted = false;
    verified_core_log_count = 0;
}

fn externalThreadId(thread_index: usize) u64 {
    return @as(u64, @intCast(thread_index)) + 1;
}

fn threadIndexFromExternalId(thread_id: u64) ?usize {
    if (thread_id == scheduler_abi.no_thread) return null;
    const index = thread_id - 1;
    if (index > @as(u64, @intCast(std.math.maxInt(usize)))) return null;
    const thread_index: usize = @intCast(index);
    if (thread_index >= thread_contexts.len) return null;
    return thread_index;
}

fn externalSchedulerOwnsThread(thread_index: usize) bool {
    return external_scheduler_policy_thread != null and external_scheduler_policy_thread.? == thread_index;
}

fn threadActiveOnDifferentCpu(thread_index: usize, target_cpu: usize) bool {
    var cpu_slot: usize = 0;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        if (cpu_slot == target_cpu) continue;
        if (cpu_scheduler_states[cpu_slot].current_thread == thread_index) return true;
        if (cpu_scheduler_states[cpu_slot].pending_commit_thread == thread_index) return true;
    }
    return false;
}

pub fn attachPolicyThread(thread_index: usize) bool {
    const ctx = threadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    external_scheduler_policy_thread = thread_index;
    external_scheduler_enabled = true;
    verifiedExitThread(thread_index);
    return true;
}

pub fn policyActive() bool {
    return external_scheduler_enabled and external_scheduler_policy_thread != null;
}

pub fn eventQueueReadable() bool {
    external_scheduler_event_lock.lock();
    defer external_scheduler_event_lock.unlock();
    return external_scheduler_event_len != 0;
}

pub fn pendingEventCount() u64 {
    external_scheduler_event_lock.lock();
    defer external_scheduler_event_lock.unlock();
    return @intCast(external_scheduler_event_len);
}

fn dropExternalSchedulerEventAtLocked(remove_offset: usize) void {
    if (remove_offset >= external_scheduler_event_len) return;
    var offset = remove_offset;
    while (offset + 1 < external_scheduler_event_len) : (offset += 1) {
        const dst = (external_scheduler_event_head + offset) % external_scheduler_events.len;
        const src = (external_scheduler_event_head + offset + 1) % external_scheduler_events.len;
        external_scheduler_events[dst] = external_scheduler_events[src];
    }
    external_scheduler_event_len -= 1;
}

fn dropOneExternalSchedulerIdleEventLocked() bool {
    var offset: usize = 0;
    while (offset < external_scheduler_event_len) : (offset += 1) {
        const index = (external_scheduler_event_head + offset) % external_scheduler_events.len;
        const event = external_scheduler_events[index];
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
    external_scheduler_event_lock.lock();
    defer external_scheduler_event_lock.unlock();
    if (external_scheduler_event_len >= external_scheduler_events.len) {
        if (event_type == scheduler_abi.event_cpu_idle or !dropOneExternalSchedulerIdleEventLocked()) {
            return false;
        }
    }
    const tail = (external_scheduler_event_head + external_scheduler_event_len) % external_scheduler_events.len;
    var event = ExternalSchedulerEvent{
        .event_type = event_type,
        .sequence = external_scheduler_next_sequence,
        .cpu_id = @intCast(cpu_id),
        .runtime_ns = runtime_ns,
    };
    external_scheduler_next_sequence +%= 1;
    if (thread_index) |idx| {
        const ctx = threadContext(idx) orelse return false;
        event.thread_id = externalThreadId(idx);
        event.generation = ctx.generation;
        event.weight = 1024;
        event.slice_ns = 4_000_000;
    }
    external_scheduler_events[tail] = event;
    external_scheduler_event_len += 1;
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
    if (queued) @atomicStore(u8, &external_scheduler_ready_event_seen, 1, .release);
}

pub fn readPolicyEventBytes(out: []u8) ?usize {
    if (out.len < scheduler_abi.sched_event_size) return null;
    external_scheduler_event_lock.lock();
    defer external_scheduler_event_lock.unlock();
    if (external_scheduler_event_len == 0) return 0;
    const event = external_scheduler_events[external_scheduler_event_head];
    external_scheduler_event_head = (external_scheduler_event_head + 1) % external_scheduler_events.len;
    external_scheduler_event_len -= 1;
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
        return true;
    }
    if (generation > std.math.maxInt(u32)) return false;
    const thread_index = threadIndexFromExternalId(thread_id) orelse return false;
    const expected_generation: u32 = @intCast(generation);
    thread_table_lock.lock();
    const initial_ctx = threadContextMutable(thread_index) orelse {
        thread_table_lock.unlock();
        return false;
    };
    if (!initial_ctx.allocated or initial_ctx.generation != expected_generation) {
        thread_table_lock.unlock();
        return false;
    }
    thread_table_lock.unlock();
    if (externalSchedulerOwnsThread(thread_index)) return false;
    verifiedProbeCommit(target_cpu, thread_id, generation);

    if (target_cpu == currentCpu()) {
        if (threadActiveOnDifferentCpu(thread_index, target_cpu)) return false;
        thread_table_lock.lock();
        const ctx = threadContextMutable(thread_index) orelse {
            thread_table_lock.unlock();
            return false;
        };
        if (!ctx.allocated or ctx.generation != expected_generation) {
            thread_table_lock.unlock();
            return false;
        }
        ctx.cpu_slot = target_cpu;
        if (!markThreadReadyLocked(thread_index, true, false)) {
            thread_table_lock.unlock();
            return false;
        }
        thread_table_lock.unlock();
        const current_thread = currentThread();
        if (thread_index == current_thread) {
            frame.rax = saved_rax;
            return true;
        }
        return switchTo(thread_index, frame, saved_rax);
    }

    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    if (!target_state.enabled) return false;
    if (target_state.current_thread == thread_index) return true;
    if (target_state.pending_commit_thread == thread_index) return true;
    if (threadActiveOnDifferentCpu(thread_index, target_cpu)) return false;
    thread_table_lock.lock();
    const ctx = threadContextMutable(thread_index) orelse {
        thread_table_lock.unlock();
        return false;
    };
    if (!ctx.allocated or ctx.generation != expected_generation) {
        thread_table_lock.unlock();
        return false;
    }
    ctx.cpu_slot = target_cpu;
    if (!markThreadReadyLocked(thread_index, true, false)) {
        thread_table_lock.unlock();
        return false;
    }
    target_state.pending_commit_thread = thread_index;
    target_state.pending_commit_generation = @intCast(generation);
    target_state.idle_event_pending = false;
    thread_table_lock.unlock();
    logExternalSchedulerApCommitOnce(target_cpu, thread_index);
    _ = smp.wakeCpu(target_cpu);
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
    if (ctx.generation != generation or !ctx.ready or hot.ready == 0) return false;
    if (ctx.cpu_slot != cpu_slot) return false;
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
    state.pending_commit_thread = null;
    state.pending_commit_generation = 0;
    state.idle_event_pending = false;
    state.is_idle = false;
    logExternalSchedulerApEntryOnce(cpu_slot, thread_index);
    return true;
}

fn logExternalSchedulerApClaimTrace(cpu_slot: usize, thread_index: usize, generation: u32, ctx: *const ThreadContext, hot: *const ThreadHotState) void {
    _ = cpu_slot;
    _ = thread_index;
    _ = generation;
    _ = ctx;
    _ = hot;
}

fn logExternalSchedulerApEntryOnce(cpu_slot: usize, thread_index: usize) void {
    _ = cpu_slot;
    _ = thread_index;
}

fn logExternalSchedulerApCommitOnce(cpu_slot: usize, thread_index: usize) void {
    _ = cpu_slot;
    _ = thread_index;
}

fn logExternalSchedulerApTickOnce(cpu_slot: usize, thread_index: usize) void {
    _ = cpu_slot;
    _ = thread_index;
}

fn logExternalSchedulerApBlockOnce(cpu_slot: usize, thread_index: usize, wait_mailbox: bool, timeout_ticks: u64, wake_tick: u64) void {
    _ = cpu_slot;
    _ = thread_index;
    _ = wait_mailbox;
    _ = timeout_ticks;
    _ = wake_tick;
}

fn logExternalSchedulerWakeOnce(thread_index: usize) void {
    _ = thread_index;
}

fn logExternalSchedulerTimerDueOnce(thread_index: usize, now_tick: u64, wake_tick: u64) void {
    _ = thread_index;
    _ = now_tick;
    _ = wake_tick;
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
    const ready_seen = @atomicLoad(u8, &external_scheduler_ready_event_seen, .acquire) != 0;
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
    return claimExternalCommittedUserEntry(cpu_slot, out_entry);
}

fn markThreadReadyLocked(thread_index: usize, ready: bool, notify: bool) bool {
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!ctx.allocated) return false;
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
    thread_table_lock.lock();
    defer thread_table_lock.unlock();
    return markThreadReadyLocked(thread_index, ready, notify);
}

pub fn markThreadReady(thread_index: usize, ready: bool) bool {
    return markThreadReadyInternal(thread_index, ready, true);
}

pub fn saveAndParkApUserThread(frame: *const TrapFrame, runtime_ns: u64) bool {
    if (!policyActive()) return false;
    const cpu_slot = currentCpu();
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    const thread_index = currentThread();
    if (thread_index >= thread_contexts.len) return false;
    if (externalSchedulerOwnsThread(thread_index)) return false;
    state.lock.lock();
    thread_table_lock.lock();
    const ctx = threadContextMutable(thread_index) orelse {
        thread_table_lock.unlock();
        state.lock.unlock();
        return false;
    };
    if (!ctx.allocated) {
        thread_table_lock.unlock();
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
    thread_table_lock.unlock();
    state.lock.unlock();
    if (!policyActive()) verifiedChargeAndFinish(cpu_slot, thread_index, runtime_ns);
    if (!enqueueExternalSchedulerEvent(
        scheduler_abi.event_tick,
        cpu_slot,
        thread_index,
        runtime_ns,
    )) {
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
    if (!policyActive()) return false;
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
) noreturn {
    const current_thread = currentThread();
    thread_table_lock.lock();
    if (threadContextMutable(current_thread)) |ctx| {
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
        _ = enqueueExternalSchedulerEvent(
            scheduler_abi.event_thread_blocked,
            currentCpu(),
            current_thread,
            0,
        );
        logExternalSchedulerApBlockOnce(currentCpu(), current_thread, wait_mailbox, timeout_ticks, ctx.wake_tick);
    }
    thread_table_lock.unlock();
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
    const policy_thread = external_scheduler_policy_thread.?;
    if (!markThreadReadyInternal(policy_thread, true, false)) return false;
    if (policy_thread == currentThread()) {
        if (saved_rax) |value| frame.rax = value;
        return true;
    }
    return switchTo(policy_thread, frame, saved_rax);
}

pub fn preemptBootstrapThread(quantum_ticks: u64, frame: *TrapFrame) bool {
    if (!policyActive()) return false;
    if (quantum_ticks == 0) return false;
    const current_thread = currentThread();
    if (externalSchedulerOwnsThread(current_thread)) return false;

    const state = schedulerStateForSlot(currentCpu()) orelse return false;
    state.lock.lock();
    state.tick_accum +%= 1;
    const should_preempt = state.tick_accum >= quantum_ticks;
    if (should_preempt) state.tick_accum = 0;
    state.lock.unlock();
    if (!should_preempt) return false;

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
    const policy_thread = external_scheduler_policy_thread.?;
    if (!markThreadReadyInternal(policy_thread, true, false)) return false;
    if (!activate(policy_thread)) return false;
    return loadContextIntoFrame(policy_thread, frame);
}

pub fn threadCapacity() usize {
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
    if (required > maxThreadSlots) return null;
    var capacity = thread_contexts.len;
    if (capacity == 0) capacity = initialThreadCapacity;
    while (capacity < required) {
        const doubled = capacity * 2;
        capacity = if (doubled > maxThreadSlots) maxThreadSlots else doubled;
        if (capacity < required and capacity == maxThreadSlots) return null;
    }
    return capacity;
}

fn ensureThreadCapacity(required: usize, free_list: *kernel.FreePageList) bool {
    if (required <= thread_contexts.len) return true;
    const capacity = nextThreadCapacity(required) orelse return false;
    const new_contexts = allocKernelSlice(ThreadContext, free_list, capacity) orelse return false;
    const new_hot_threads = allocKernelSlice(ThreadHotState, free_list, capacity) orelse return false;

    @memcpy(new_contexts[0..thread_contexts.len], thread_contexts);
    @memcpy(new_hot_threads[0..thread_hot_states.len], thread_hot_states);
    var i = thread_contexts.len;
    while (i < capacity) : (i += 1) {
        new_contexts[i] = .{ .id = @intCast(i) };
        new_hot_threads[i] = .{};
    }

    thread_contexts = new_contexts;
    thread_hot_states = new_hot_threads;
    thread_contexts_ptr = @ptrCast(thread_contexts.ptr);
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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(thread_contexts), &thread_contexts));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(thread_contexts_ptr), &thread_contexts_ptr));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_thread_hot_states), &initial_thread_hot_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(thread_hot_states), &thread_hot_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(cpu_scheduler_states), &cpu_scheduler_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(lapic_tick_count), &lapic_tick_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_tick_accum), &scheduler_tick_accum));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_switch_count), &scheduler_switch_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_fx_state), &initial_fx_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_interrupt_fx_state), &kernel_interrupt_fx_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(external_scheduler_ap_commit_log_mask), &external_scheduler_ap_commit_log_mask));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(external_scheduler_ap_entry_log_mask), &external_scheduler_ap_entry_log_mask));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(verified_core_state), &verified_core_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(verified_core_probe_state), &verified_core_probe_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(verified_core_runqueue_scratch), &verified_core_runqueue_scratch));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(verified_core_pick_scratch), &verified_core_pick_scratch));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(verified_core_decision), &verified_core_decision));
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

fn buildInitialCpuSchedulerStates() [smp.max_cpus]CpuSchedulerState {
    var states: [smp.max_cpus]CpuSchedulerState = [_]CpuSchedulerState{.{}} ** smp.max_cpus;
    states[bootstrap_cpu_slot].enabled = true;
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

fn threadAssignedCpuSlot(thread_index: usize) usize {
    const ctx = threadContext(thread_index) orelse return bootstrap_cpu_slot;
    if (!ctx.allocated) return bootstrap_cpu_slot;
    if (ctx.cpu_slot >= cpu_scheduler_states.len) return bootstrap_cpu_slot;
    return ctx.cpu_slot;
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
    const count = @min(smp.cpuCount(), cpu_scheduler_states.len);
    var i: usize = 0;
    while (i < cpu_scheduler_states.len) : (i += 1) {
        const observed = i < count and smp.cpuState(i) != .absent;
        const state = &cpu_scheduler_states[i];
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
    return policyActive();
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
    if (thread_index >= thread_contexts.len) return false;
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
    return @intCast(idx + 1);
}

pub fn threadContextMutable(thread_index: usize) ?*ThreadContext {
    if (thread_index >= thread_contexts.len) return null;
    return &thread_contexts[thread_index];
}

pub fn threadContext(thread_index: usize) ?*const ThreadContext {
    if (thread_index >= thread_contexts.len) return null;
    return &thread_contexts[thread_index];
}

fn getThreadHotState(thread_index: usize) ?*ThreadHotState {
    if (thread_index >= thread_contexts.len) return null;
    return &thread_hot_states[thread_index];
}

fn getThreadHotStateConst(thread_index: usize) ?*const ThreadHotState {
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
    const ctx = threadContext(thread_index) orelse return;
    const hot = getThreadHotState(thread_index) orelse return;
    hot.* = hotStateFromContext(ctx);
}

fn setThreadHotReady(thread_index: usize, ready: bool) void {
    if (getThreadHotState(thread_index)) |hot| hot.ready = boolByte(ready);
}

fn setThreadHotSignalPending(thread_index: usize, pending: bool) void {
    if (getThreadHotState(thread_index)) |hot| hot.signal_pending = boolByte(pending);
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

fn initializeThreadContextWithReadyState(
    thread_index: usize,
    owner_process: kernel.PrincipalId,
    user_spaces: []UserAddressSpace,
    initial_frame: TrapFrame,
    initial_ready: bool,
) bool {
    const space = getUserSpace(user_spaces, owner_process) orelse return false;
    thread_table_lock.lock();
    defer thread_table_lock.unlock();
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (ctx.allocated) return false;
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
    while (i < thread_contexts.len) : (i += 1) {
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

pub fn liveThreadCount(principal: kernel.PrincipalId) usize {
    var count: usize = 0;
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
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
        while (i < thread_contexts.len) : (i += 1) {
            const ctx = threadContext(i) orelse continue;
            if (ctx.allocated) continue;
            if (!initializeThreadContextWithReadyState(i, owner_process, user_spaces, initial_frame, initial_ready)) return null;
            if (!policyActive()) verifiedAddThread(i, thread_contexts[i].generation, initial_ready);
            return i;
        }
        if (!ensureThreadCapacity(thread_contexts.len + 1, free_list)) return null;
    }
}

pub fn releaseThread(thread_index: usize) bool {
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    thread_table_lock.lock();
    defer thread_table_lock.unlock();

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
        external_scheduler_enabled = false;
        external_scheduler_policy_thread = null;
    }
    syncHotStateFromContext(thread_index);
    syncHotStateFromContext(thread_index);
    return true;
}

fn clearThreadFromCpuSchedulerStatesLocked(thread_index: usize) void {
    var cpu_slot: usize = 0;
    while (cpu_slot < cpu_scheduler_states.len) : (cpu_slot += 1) {
        const state = &cpu_scheduler_states[cpu_slot];
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
    if (!ctx.allocated or ctx.cpu_slot != cpu_slot) return false;
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
    thread_table_lock.lock();
    defer thread_table_lock.unlock();
    const ctx = threadContextMutable(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    ctx.fs_base = fs_base;
    if (thread_index == currentThread()) x86_platform.writeFsBase(fs_base);
    return true;
}

pub fn setCurrentGsBase(gs_base: u64) bool {
    thread_table_lock.lock();
    defer thread_table_lock.unlock();
    const ctx = threadContextMutable(currentThread()) orelse return false;
    if (!ctx.allocated) return false;
    ctx.gs_base = gs_base;
    x86_platform.writeGsBase(gs_base);
    return true;
}

fn saveCurrentThreadContextFromFrame(frame: *const TrapFrame) void {
    const current_thread = currentThread();
    thread_table_lock.lock();
    defer thread_table_lock.unlock();
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
    return true;
}

pub fn switchTo(next_thread: usize, frame: *TrapFrame, saved_rax: ?u64) bool {
    if (!isBootstrapSchedulerCpu() and !apUserDispatchReady()) return false;
    if (next_thread >= thread_contexts.len) return false;
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

pub fn exitCurrentThread(frame: *TrapFrame, saved_rax: u64, before_ap_idle: ?BeforeCurrentThreadLeaveCallback) bool {
    if (!policyActive()) return false;
    if (!isBootstrapSchedulerCpu()) {
        exitApThreadToIdleAfter(before_ap_idle);
    }
    const current_thread = currentThread();
    if (externalSchedulerOwnsThread(current_thread)) return false;
    if (!releaseThread(current_thread)) return false;
    return switchToExternalScheduler(frame, saved_rax);
}

pub fn wakeIfWaiting(thread_index: usize) void {
    thread_table_lock.lock();
    defer thread_table_lock.unlock();
    const ctx = threadContextMutable(thread_index) orelse return;
    if (!ctx.allocated) return;
    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    if (!policyActive()) verifiedWakeThread(thread_index);
    logExternalSchedulerWakeOnce(thread_index);
    publishThreadReady(thread_index);
}

pub fn wakeMailboxWaiter(principal: kernel.PrincipalId) void {
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0 or hot.owner_process != principal) continue;
        if (hot.wait_mailbox == 0) continue;
        wakeIfWaiting(i);
        return;
    }
}

pub fn wakeBlockedThread(principal: kernel.PrincipalId) void {
    var first_ready: ?usize = null;
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0 or hot.owner_process != principal) continue;
        if (hot.ready == 0) {
            wakeIfWaiting(i);
            return;
        }
        if (first_ready == null) first_ready = i;
    }
    if (first_ready) |thread_index| {
        thread_table_lock.lock();
        defer thread_table_lock.unlock();
        const ctx = threadContextMutable(thread_index) orelse return;
        if (!ctx.allocated or ctx.owner_process != principal) return;
        ctx.signal_pending = true;
        setThreadHotSignalPending(thread_index, true);
    }
}

pub fn consumeSignal(principal: kernel.PrincipalId) bool {
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0 or hot.owner_process != principal or hot.signal_pending == 0) continue;
        thread_table_lock.lock();
        defer thread_table_lock.unlock();
        const ctx = threadContextMutable(i) orelse return false;
        if (!ctx.allocated or ctx.owner_process != principal or !ctx.signal_pending) return false;
        ctx.signal_pending = false;
        setThreadHotSignalPending(i, false);
        return true;
    }
    return false;
}

pub fn releasePrincipalThreads(principal: kernel.PrincipalId) usize {
    var released: usize = 0;
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
        const ctx = threadContext(i) orelse continue;
        if (!ctx.allocated or ctx.owner_process != principal) continue;
        if (releaseThread(i)) released += 1;
    }
    return released;
}

pub fn wakeExpiredTimers(now_tick: u64) void {
    if (!isBootstrapSchedulerCpu()) return;
    var i: usize = 0;
    while (i < thread_contexts.len) : (i += 1) {
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
        if (policyActive()) {
            parkApThreadForBlock(frame, wait_mailbox, timeout_ticks, resume_rax, before_block);
        }
        return false;
    }
    const current_thread = currentThread();
    thread_table_lock.lock();
    const ctx = threadContextMutable(current_thread) orelse {
        thread_table_lock.unlock();
        return false;
    };

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

    if (policyActive() and !externalSchedulerOwnsThread(current_thread)) {
        _ = enqueueExternalSchedulerEvent(
            scheduler_abi.event_thread_blocked,
            currentCpu(),
            current_thread,
            0,
        );
        thread_table_lock.unlock();
        if (before_block) |callback| callback.run(callback.context);
        if (switchToExternalScheduler(frame, resume_rax)) return true;
        thread_table_lock.lock();
        if (threadContextMutable(current_thread)) |restored_ctx| {
            if (restored_ctx.allocated) {
                restored_ctx.wait_mailbox = false;
                restored_ctx.wake_tick = 0;
                restored_ctx.ready = true;
                setThreadHotWaitState(current_thread, false, 0, true);
            }
        }
        thread_table_lock.unlock();
        return false;
    }

    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    ctx.ready = true;
    setThreadHotWaitState(current_thread, false, 0, true);
    thread_table_lock.unlock();
    return false;
}
