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

const TrapFrame = interrupts.TrapFrame;
const UserAddressSpace = address_space.UserAddressSpace;
const default_process_principal: kernel.PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
const bootstrap_cpu_slot: usize = 0;
const all_cpu_affinity_mask: u64 = if (smp.max_cpus >= 64) std.math.maxInt(u64) else (@as(u64, 1) << smp.max_cpus) - 1;

const fx_state_bytes: usize = 512;
pub const max_thread_slots: usize = kernel.max_thread_slots;
pub const initial_thread_capacity: usize = kernel.initial_thread_capacity;
pub const idle_thread_marker: usize = max_thread_slots;
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
    current_thread: usize = idle_thread_marker,
    current_principal: ?kernel.PrincipalId = null,
    current_cr3: u64 = 0,
    idle_thread: usize = idle_thread_marker,
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
var thread_contexts: []ThreadContext = initial_thread_contexts[0..];
pub export var thread_contexts_ptr: *anyopaque = @ptrCast(initial_thread_contexts[0..].ptr);
var initial_thread_hot_states: [initial_thread_capacity]ThreadHotState = buildInitialThreadHotStates();
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

fn nextThreadGeneration(current: u32) u32 {
    const next = current +% 1;
    return if (next == 0) 1 else next;
}

pub fn initStaticStorage() void {
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

pub fn installExternalSchedulerPolicyThread(thread_index: usize) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    external_scheduler_policy_thread = thread_index;
    external_scheduler_enabled = true;
    return true;
}

pub fn externalSchedulerActive() bool {
    return external_scheduler_enabled and external_scheduler_policy_thread != null;
}

pub fn externalSchedulerEventReadable() bool {
    external_scheduler_event_lock.lock();
    defer external_scheduler_event_lock.unlock();
    return external_scheduler_event_len != 0;
}

pub fn externalSchedulerPendingEventCount() u64 {
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
    if (!externalSchedulerActive()) return false;
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
        const ctx = getThreadContextConst(idx) orelse return false;
        event.thread_id = externalThreadId(idx);
        event.generation = ctx.generation;
        event.weight = 1024;
        event.slice_ns = 4_000_000;
    }
    external_scheduler_events[tail] = event;
    external_scheduler_event_len += 1;
    return true;
}

pub fn notifyExternalThreadReady(thread_index: usize) void {
    if (externalSchedulerOwnsThread(thread_index)) return;
    _ = enqueueExternalSchedulerEvent(
        scheduler_abi.event_thread_ready,
        threadAssignedCpuSlot(thread_index),
        thread_index,
        0,
    );
}

pub fn readExternalSchedulerEventBytes(out: []u8) ?usize {
    if (out.len < scheduler_abi.sched_event_size) return null;
    external_scheduler_event_lock.lock();
    defer external_scheduler_event_lock.unlock();
    if (external_scheduler_event_len == 0) return 0;
    const event = external_scheduler_events[external_scheduler_event_head];
    external_scheduler_event_head = (external_scheduler_event_head + 1) % external_scheduler_events.len;
    external_scheduler_event_len -= 1;
    const bytes = std.mem.asBytes(&event);
    @memcpy(out[0..bytes.len], bytes);
    return bytes.len;
}

pub fn commitExternalSchedule(
    cpu_id: u32,
    thread_id: u64,
    generation: u64,
    frame: *TrapFrame,
    saved_rax: u64,
) bool {
    if (!externalSchedulerActive()) return false;
    if (cpu_id >= smp.max_cpus) return false;
    const target_cpu: usize = @intCast(cpu_id);
    const target_state = schedulerStateForSlot(target_cpu) orelse return false;
    if (thread_id == scheduler_abi.no_thread) {
        target_state.lock.lock();
        target_state.pending_commit_thread = null;
        target_state.pending_commit_generation = 0;
        target_state.idle_event_pending = target_cpu != bootstrap_cpu_slot;
        target_state.lock.unlock();
        return true;
    }
    if (generation > std.math.maxInt(u32)) return false;
    const thread_index = threadIndexFromExternalId(thread_id) orelse return false;
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated or ctx.generation != @as(u32, @intCast(generation))) return false;
    if (externalSchedulerOwnsThread(thread_index)) return false;

    if (target_cpu == currentCpuSlot()) {
        if (threadActiveOnDifferentCpu(thread_index, target_cpu)) return false;
        ctx.cpu_slot = target_cpu;
        if (!setThreadReadyInternal(thread_index, true, false)) return false;
        const current_thread = currentThreadIndex();
        if (thread_index == current_thread) {
            frame.rax = saved_rax;
            return true;
        }
        return switchToThread(thread_index, frame, saved_rax);
    }

    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    if (!target_state.enabled) return false;
    if (target_state.current_thread == thread_index) return true;
    if (target_state.pending_commit_thread == thread_index) return true;
    if (threadActiveOnDifferentCpu(thread_index, target_cpu)) return false;
    ctx.cpu_slot = target_cpu;
    if (!setThreadReadyInternal(thread_index, true, false)) return false;
    target_state.pending_commit_thread = thread_index;
    target_state.pending_commit_generation = @intCast(generation);
    target_state.idle_event_pending = false;
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
    const ctx = getThreadContextConst(thread_index) orelse return false;
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
    if (thread_index < 3 and cpu_slot < 2) return;
    const old = @atomicRmw(u32, &external_scheduler_ap_claim_trace_count, .Add, 1, .acq_rel);
    if (old >= 64) return;
    kernel_log.writeFmt(
        "[sched] ap claim cpu={} thread={} gen={} ctx_gen={} ready={} hot_ready={} ctx_cpu={}\n",
        .{ cpu_slot, thread_index, generation, ctx.generation, ctx.ready, hot.ready, ctx.cpu_slot },
    );
}

fn logExternalSchedulerApEntryOnce(cpu_slot: usize, thread_index: usize) void {
    _ = thread_index;
    if (cpu_slot >= 64) return;
    const bit = @as(u64, 1) << @intCast(cpu_slot);
    const old = @atomicRmw(u64, &external_scheduler_ap_entry_log_mask, .Or, bit, .acq_rel);
    if ((old & bit) != 0) return;
    kernel_log.writeOnly("[sched] ap user cpu=");
    kernel_log.writeOnlyThreadIndexLabel(cpu_slot);
    kernel_log.writeOnly("\n");
}

fn logExternalSchedulerApCommitOnce(cpu_slot: usize, thread_index: usize) void {
    if (cpu_slot >= 64) return;
    const bit = @as(u64, 1) << @intCast(cpu_slot);
    const old = @atomicRmw(u64, &external_scheduler_ap_commit_log_mask, .Or, bit, .acq_rel);
    if ((old & bit) != 0) return;
    kernel_log.writeFmt("[sched] ap commit cpu={} thread={}\n", .{ cpu_slot, thread_index });
}

fn logExternalSchedulerApTickOnce(cpu_slot: usize, thread_index: usize) void {
    if (cpu_slot >= 64) return;
    const bit = @as(u64, 1) << @intCast(cpu_slot);
    const old = @atomicRmw(u64, &external_scheduler_ap_tick_log_mask, .Or, bit, .acq_rel);
    if ((old & bit) != 0) return;
    kernel_log.writeFmt("[sched] ap tick cpu={} thread={}\n", .{ cpu_slot, thread_index });
}

fn logExternalSchedulerApBlockOnce(cpu_slot: usize, thread_index: usize, wait_mailbox: bool, timeout_ticks: u64, wake_tick: u64) void {
    if (cpu_slot >= 64) return;
    const bit = @as(u64, 1) << @intCast(cpu_slot);
    const old = @atomicRmw(u64, &external_scheduler_ap_block_log_mask, .Or, bit, .acq_rel);
    if ((old & bit) != 0) return;
    kernel_log.writeFmt("[sched] ap block cpu={} thread={} wait={} timeout={} now={} wake={}\n", .{ cpu_slot, thread_index, wait_mailbox, timeout_ticks, lapic_tick_count, wake_tick });
}

fn logExternalSchedulerWakeOnce(thread_index: usize) void {
    if (thread_index >= 64) return;
    const bit = @as(u64, 1) << @intCast(thread_index);
    const old = @atomicRmw(u64, &external_scheduler_wake_log_mask, .Or, bit, .acq_rel);
    if ((old & bit) != 0) return;
    kernel_log.writeFmt("[sched] wake thread={}\n", .{thread_index});
}

fn logExternalSchedulerTimerDueOnce(thread_index: usize, now_tick: u64, wake_tick: u64) void {
    if (thread_index >= 64) return;
    const bit = @as(u64, 1) << @intCast(thread_index);
    const old = @atomicRmw(u64, &external_scheduler_timer_due_log_mask, .Or, bit, .acq_rel);
    if ((old & bit) != 0) return;
    kernel_log.writeFmt("[sched] timer due thread={} now={} wake={}\n", .{ thread_index, now_tick, wake_tick });
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
    if (ap_user_dispatch_enabled and externalSchedulerActive() and state.pending_commit_thread == null and !state.idle_event_pending) {
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

fn setThreadReadyInternal(thread_index: usize, ready: bool, notify: bool) bool {
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    const was_ready = ctx.ready;
    if (ready) schedulerFullMemoryFence();
    ctx.ready = ready;
    setThreadHotReady(thread_index, ready);
    if (notify and ready and !was_ready) notifyExternalThreadReady(thread_index);
    return true;
}

pub fn setThreadReady(thread_index: usize, ready: bool) bool {
    return setThreadReadyInternal(thread_index, ready, true);
}

pub fn saveAndParkCurrentApUserThread(frame: *const TrapFrame, runtime_ns: u64) bool {
    if (!externalSchedulerActive()) return false;
    const cpu_slot = currentCpuSlot();
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    const thread_index = currentThreadIndex();
    if (thread_index >= thread_contexts.len) return false;
    if (externalSchedulerOwnsThread(thread_index)) return false;
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    ctx.frame = frame.*;
    ctx.cr3 = currentUserCr3();
    ctx.fs_base = x86_platform.readFsBase();
    ctx.gs_base = x86_platform.readGsBase();
    ctx.pkru = x86_platform.readPkru();
    ctx.ready = true;
    setThreadHotCr3(thread_index, ctx.cr3);
    setThreadHotReady(thread_index, true);
    state.lock.lock();
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
    state.tick_accum = 0;
    state.lock.unlock();
    if (!enqueueExternalSchedulerEvent(
        scheduler_abi.event_tick,
        cpu_slot,
        thread_index,
        runtime_ns,
    )) {
        state.lock.lock();
        state.current_thread = thread_index;
        state.current_principal = ctx.owner_process;
        state.current_cr3 = ctx.cr3;
        state.is_idle = false;
        state.lock.unlock();
        return false;
    }
    logExternalSchedulerApTickOnce(cpu_slot, thread_index);
    return true;
}

pub fn preemptCurrentApThreadForExternalScheduler(quantum_ticks: u64, frame: *const TrapFrame) bool {
    if (!externalSchedulerActive()) return false;
    if (quantum_ticks == 0) return false;
    const cpu_slot = currentCpuSlot();
    if (cpu_slot == bootstrap_cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    state.tick_accum +%= 1;
    const should_preempt = state.tick_accum >= quantum_ticks;
    state.lock.unlock();
    if (!should_preempt) return false;
    return saveAndParkCurrentApUserThread(frame, quantum_ticks * external_runtime_ns_per_tick);
}

pub fn blockCurrentApThreadForExternalScheduler(frame: *TrapFrame, wait_mailbox: bool, timeout_ticks: u64, resume_rax: u64) noreturn {
    const current_thread = currentThreadIndex();
    if (getThreadContext(current_thread)) |ctx| {
        var saved = frame.*;
        saved.rax = resume_rax;
        ctx.frame = saved;
        ctx.cr3 = currentUserCr3();
        ctx.fs_base = x86_platform.readFsBase();
        ctx.gs_base = x86_platform.readGsBase();
        ctx.pkru = x86_platform.readPkru();
        ctx.wait_mailbox = wait_mailbox;
        ctx.wake_tick = if (timeout_ticks == 0) 0 else lapic_tick_count + timeout_ticks;
        ctx.ready = false;
        setThreadHotCr3(current_thread, ctx.cr3);
        setThreadHotWaitState(current_thread, wait_mailbox, ctx.wake_tick, false);
        _ = enqueueExternalSchedulerEvent(
            scheduler_abi.event_thread_blocked,
            currentCpuSlot(),
            current_thread,
            0,
        );
        logExternalSchedulerApBlockOnce(currentCpuSlot(), current_thread, wait_mailbox, timeout_ticks, ctx.wake_tick);
    }
    if (schedulerStateForSlot(currentCpuSlot())) |state| {
        state.lock.lock();
        state.current_thread = state.idle_thread;
        state.current_principal = null;
        state.current_cr3 = 0;
        state.is_idle = true;
        state.lock.unlock();
    }
    smp.returnCurrentApToIdleFromInterrupt();
}

pub fn exitCurrentApThreadToExternalScheduler() noreturn {
    const current_thread = currentThreadIndex();
    _ = releaseThreadSlot(current_thread);
    smp.returnCurrentApToIdleFromInterrupt();
}

fn switchToExternalScheduler(frame: *TrapFrame, saved_rax: ?u64) bool {
    if (!externalSchedulerActive()) return false;
    const policy_thread = external_scheduler_policy_thread.?;
    if (!setThreadReadyInternal(policy_thread, true, false)) return false;
    if (policy_thread == currentThreadIndex()) {
        if (saved_rax) |value| frame.rax = value;
        return true;
    }
    return switchToThread(policy_thread, frame, saved_rax);
}

pub fn preemptCurrentThreadForExternalScheduler(quantum_ticks: u64, frame: *TrapFrame) bool {
    if (!externalSchedulerActive()) return false;
    if (quantum_ticks == 0) return false;
    const current_thread = currentThreadIndex();
    if (externalSchedulerOwnsThread(current_thread)) return false;

    const state = schedulerStateForSlot(currentCpuSlot()) orelse return false;
    state.lock.lock();
    state.tick_accum +%= 1;
    const should_preempt = state.tick_accum >= quantum_ticks;
    if (should_preempt) state.tick_accum = 0;
    state.lock.unlock();
    if (!should_preempt) return false;

    _ = enqueueExternalSchedulerEvent(
        scheduler_abi.event_tick,
        currentCpuSlot(),
        current_thread,
        quantum_ticks * external_runtime_ns_per_tick,
    );
    return switchToExternalScheduler(frame, null);
}

pub fn loadExternalSchedulerPolicyThread(frame: *TrapFrame) bool {
    if (!externalSchedulerActive()) return false;
    const policy_thread = external_scheduler_policy_thread.?;
    if (!setThreadReadyInternal(policy_thread, true, false)) return false;
    if (!activateThread(policy_thread)) return false;
    return loadThreadContextToFrame(policy_thread, frame);
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

pub fn kernelStaticStorageEndAddr() usize {
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
    const ctx = getThreadContextConst(thread_index) orelse return bootstrap_cpu_slot;
    if (!ctx.allocated) return bootstrap_cpu_slot;
    if (ctx.cpu_slot >= cpu_scheduler_states.len) return bootstrap_cpu_slot;
    return ctx.cpu_slot;
}

pub fn currentCpuSlot() usize {
    return smp.currentCpuSlot();
}

pub fn schedulerRunsOnCurrentCpu() bool {
    return smp.isBootstrapCpu();
}

pub fn installCpuIdleObserver() void {
    scheduler_observer.registerIdleObserver(observeCpuIdleFromAp);
    scheduler_observer.registerIdleUserEntryPoll(claimExternalCommittedUserEntryFromAp);
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
        if (!observed or i != bootstrap_cpu_slot) {
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
        }
    }
}

pub fn userApSchedulingReady() bool {
    return externalSchedulerActive();
}

pub fn parkCurrentApAfterCurrentThreadStopped() noreturn {
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
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
}

pub fn saveApUserTimerFrame(frame: *const TrapFrame) bool {
    _ = frame;
    return false;
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

pub fn getThreadContext(thread_index: usize) ?*ThreadContext {
    if (thread_index >= thread_contexts.len) return null;
    return &thread_contexts[thread_index];
}

pub fn getThreadContextConst(thread_index: usize) ?*const ThreadContext {
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
    const ctx = getThreadContextConst(thread_index) orelse return;
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

pub fn isThreadReady(thread_index: usize) bool {
    const hot = getThreadHotStateConst(thread_index) orelse return false;
    return hot.allocated != 0 and hot.ready != 0;
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
    const was_external_policy = externalSchedulerOwnsThread(thread_index);
    if (!was_external_policy) {
        _ = enqueueExternalSchedulerEvent(
            scheduler_abi.event_thread_exited,
            threadAssignedCpuSlot(thread_index),
            thread_index,
            0,
        );
    }
    const next_generation = nextThreadGeneration(ctx.generation);
    lockAllCpuSchedulerStates();
    clearThreadFromCpuSchedulerStatesLocked(thread_index);
    unlockAllCpuSchedulerStates();
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

pub fn exitCurrentThreadToExternalScheduler(frame: *TrapFrame, saved_rax: u64) bool {
    if (!externalSchedulerActive()) return false;
    if (!schedulerRunsOnCurrentCpu()) {
        exitCurrentApThreadToExternalScheduler();
    }
    const current_thread = currentThreadIndex();
    if (externalSchedulerOwnsThread(current_thread)) return false;
    if (!releaseThreadSlot(current_thread)) return false;
    return switchToExternalScheduler(frame, saved_rax);
}

pub fn wakeThreadIfWaiting(thread_index: usize) void {
    const ctx = getThreadContext(thread_index) orelse return;
    if (!ctx.allocated) return;
    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    logExternalSchedulerWakeOnce(thread_index);
    notifyExternalThreadReady(thread_index);
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
        logExternalSchedulerTimerDueOnce(i, now_tick, hot.wake_tick);
        wakeThreadIfWaiting(i);
    }
}

pub fn blockCurrentThreadForEvent(frame: *TrapFrame, wait_mailbox: bool, timeout_ticks: u64, resume_rax: u64) bool {
    if (!schedulerRunsOnCurrentCpu()) {
        if (externalSchedulerActive()) {
            blockCurrentApThreadForExternalScheduler(frame, wait_mailbox, timeout_ticks, resume_rax);
        }
        return false;
    }
    const current_thread = currentThreadIndex();
    const ctx = getThreadContext(current_thread) orelse return false;

    var saved = frame.*;
    saved.rax = resume_rax;
    ctx.frame = saved;
    ctx.cr3 = currentUserCr3();
    ctx.fs_base = x86_platform.readFsBase();
    ctx.gs_base = x86_platform.readGsBase();
    ctx.pkru = x86_platform.readPkru();
    ctx.wait_mailbox = wait_mailbox;
    ctx.wake_tick = if (timeout_ticks == 0) 0 else lapic_tick_count + timeout_ticks;
    ctx.ready = false;
    setThreadHotCr3(current_thread, ctx.cr3);
    setThreadHotWaitState(current_thread, wait_mailbox, ctx.wake_tick, false);

    if (externalSchedulerActive() and !externalSchedulerOwnsThread(current_thread)) {
        _ = enqueueExternalSchedulerEvent(
            scheduler_abi.event_thread_blocked,
            currentCpuSlot(),
            current_thread,
            0,
        );
        if (switchToExternalScheduler(frame, resume_rax)) return true;
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.ready = true;
        setThreadHotWaitState(current_thread, false, 0, true);
        return false;
    }

    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    ctx.ready = true;
    setThreadHotWaitState(current_thread, false, 0, true);
    return false;
}
