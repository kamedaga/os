const b3 = @import("ipc_path_trace.zig");
const ipc_metric = @import("ipc_metric.zig");
pub const ipc_call_binding = @import("ipc_call_binding.zig");
const deadline_index = @import("memory/deadline_index.zig");
const std = @import("std");
const lock_diagnostics = @import("sync/lock_diagnostics.zig");
const perf = @import("smp_perf.zig");
const wake_trace = @import("wake_trace.zig");
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
const owned_storage = @import("memory/owned_storage.zig");
const identity_allocator = @import("memory/identity_allocator.zig");
const UserVirtualAddress = @import("memory/addresses.zig").UserVirtualAddress;

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
const CpuSet = @import("cpu_set.zig").CpuSet;
const bootstrap_cpu_rows = if (@import("builtin").is_test) 4 else 1;

const xstate_bytes: usize = x86_platform.xstate_bytes;
pub const threadIdValueCount: usize = kernel.thread_id_value_count;
pub const initialThreadCapacity: usize = kernel.initial_thread_capacity;
pub const idleThreadMarker: usize = std.math.maxInt(usize);
const max_thread_generation: u32 = std.math.maxInt(i32);
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

// Host-only rendezvous at the real admission/action boundary. No production
// state or branch is emitted, and no callback runs under the table lock.
const ThreadIdentityTestProbe = struct {
    var admitted: ?*u8 = null;
    var proceed: ?*u8 = null;
    var contended: ?*u8 = null;
};

const DirectoryTestProbe = struct {
    var contended: ?*u8 = null;
};

const SchedulerSpinLock = struct {
    value: u8 = 0,
    interrupts_were_enabled: bool = false,
    diagnostic_record: lock_diagnostics.Record = .{},

    fn diagnosticKey(self: *SchedulerSpinLock) lock_diagnostics.order.Key {
        if (self == &scheduler_state.thread_table.lock_state) return .{ .class = .thread_table };
        if (self == &scheduler_state.thread_table.directory_lock) return .{ .class = .thread_directory };
        for (scheduler_state.cpus, 0..) |*state, cpu| {
            if (self == &state.lock) return .{ .class = .cpu_state, .order = cpu };
            if (self == &state.runqueue_lock) return .{ .class = .runqueue, .order = cpu };
        }
        lock_diagnostics.failure(error.UnclassifiedSchedulerLock);
    }

    fn lock(self: *SchedulerSpinLock) void {
        const start = perf.timestamp();
        const restore_interrupts = interruptsEnabled();
        disableInterruptsForSchedulerLock();
        // Table -> CPU state -> runqueue -> directory (terminal leaf).
        // Pairs within a CPU lock class use CPU order.
        // Check before spinning, while IF=0, so diagnostic reentry cannot hang.
        if (comptime lock_diagnostics.enabled)
            lock_diagnostics.beforeAcquire(&self.diagnostic_record, self.diagnosticKey());
        while (true) {
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) {
                self.interrupts_were_enabled = restore_interrupts;
                if (comptime lock_diagnostics.enabled)
                    lock_diagnostics.acquired(&self.diagnostic_record, self.diagnosticKey());
                perf.elapsed(.scheduler_lock_wait_cycles, start);
                perf.add(.scheduler_lock_acquires, 1);
                return;
            }
            if (builtin.is_test) {
                if (self == &scheduler_state.thread_table.lock_state) {
                    if (ThreadIdentityTestProbe.contended) |probe| @atomicStore(u8, probe, 1, .release);
                }
                if (self == &scheduler_state.thread_table.directory_lock) {
                    if (DirectoryTestProbe.contended) |probe| @atomicStore(u8, probe, 1, .release);
                }
            }
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                asm volatile ("pause");
            }
        }
    }

    fn unlock(self: *SchedulerSpinLock) void {
        // Drop the record before another CPU can acquire and overwrite it.
        if (comptime lock_diagnostics.enabled)
            lock_diagnostics.releasing(&self.diagnostic_record);
        // Membership/context rejection must also precede reading this lock's
        // mutable IF snapshot: a wrong-CPU unlock owns neither field.
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
    // CPU ownership outlives allocated/generation changes during local exit.
    // Slot reuse must wait for this identity to be cleared or transferred,
    // even when the CPU is disabled or its scheduling state says idle.
    current_context: ?*ThreadContext = null,
    current_generation: u32 = 0,
    current_principal: ?kernel.PrincipalId = null,
    current_cr3: u64 = 0,
    idle_thread: usize = idleThreadMarker,
    // Service checkpoints are independent of the CPU's preemption cadence.
    runtime_accounting: @import("scheduler_runtime_accounting.zig").Accounting = .{},
    tick_accum: u64 = 0,
    deferred_slice_ticks: u64 = 0,
    enabled: bool = false,
    is_idle: bool = true,
    // True only across the HLT of loadNextReadyThreadOrIdle, where this CPU
    // holds no lock. Timer IRQs use it to tell that wait apart from the
    // `sti; pause; cli` windows of kernel spin loops, which may own an
    // address-space lock or the TLB-shootdown lock.
    lock_free_idle_wait: bool = false,
};

pub const ThreadContext = struct {
    id: u32 = 0,
    generation: u32 = 1,
    allocated: bool = false,
    scheduler_entity: ?*scheduler_runqueue.Node = null,
    owner_process: kernel.PrincipalId = default_process_principal,
    cpu_slot: usize = bootstrap_cpu_slot,
    // Null is unrestricted; explicit affinity borrows stable topology-sized storage.
    cpu_affinity: ?CpuSet = null,
    cr3: u64 = 0,
    fs_base: u64 = 0,
    gs_base: u64 = 0,
    pkru: u32 = 0,
    ready: bool = false,
    wait_mailbox: bool = false,
    // T protects this admitted wait attribute, including remote signal delivery.
    wait_uninterruptible: bool = false,
    // Opaque identity for a keyed blocking operation.  Zero is reserved for
    // the existing unkeyed FD/IPC waits.  The scheduler never interprets the
    // token; it only prevents a completion left over from an older wait from
    // waking the thread in a newer, unrelated wait.
    active_wait_token: u64 = 0,
    next_wait_token: u64 = 1,
    // Keep profile-only notification metadata in one member: adding members
    // here permutes Zig's automatic layout of the ordinary IPC save state.
    notify_stamp: if (ipc_metric.enabled) struct {
        // T stamps the exact completion claim; the CPU pin consumes it on
        // user return. Neither diagnostic field owns a resource.
        stamp: @import("notify_metric.zig").Stamp = .{},
        // Identifies a dedicated park even after a later generic wait.
        direct_token: u64 = 0,
    } else void = if (ipc_metric.enabled) .{} else {},
    // Independent rights: a server may call a lower service while owing a reply.
    ipc_outgoing: ?*ipc_call_binding.Node = null,
    ipc_implicit: ?*ipc_call_binding.Node = null,
    ipc_receiving: ?*ipc_call_binding.Node = null,
    ipc_block: ?*ipc_call_binding.Node = null,
    ipc_block_generation: u32 = 0,
    // A negative hint contains no borrowed endpoint or authority. Only this
    // thread's masked syscall entry accesses it; CPU pins prevent body reuse.
    // Stale hints merely choose the ordinary path and expire after 31 calls.
    direct_miss_key: u64 = 0,
    // Independent bytes, with independent synchronization: the negative hint
    // is CPU-pin owned; T publishes/consumes native notification provenance.
    // Group their storage to preserve the pre-notification hot-field layout.
    // Do not pack these into shared bits: updates must not become a shared RMW.
    entry_state: struct {
        direct_miss_remaining: u8 = 0,
        // Only saved native notification frames acquire this provenance.
        // Activation and ordinary frame replacement consume/invalidate it.
        notify_sysret_continuation: bool = false,
    } = .{},
    wait_completion_claimed: bool = false,
    wait_completion_publish_pending: bool = false,
    wait_completion_rax: u64 = 0,
    wait_ipc_words: ?[4]u64 = null,
    wait_poll_result: ?struct { index: u64, revents: u64 } = null,
    wait_ipc_fd_count: u8 = 0,
    stopped: bool = false,
    resume_after_stop: bool = false,
    pending_signal_mask: u64 = 0,
    signal_entry: u64 = 0,
    signal_scratch_base: u64 = 0,
    signal_scratch_size: u64 = 0,
    signal_delivery_ready: bool = false,
    signal_blocked_mask: u64 = 0,
    signal_inhibit_start: u64 = 0,
    signal_inhibit_end: u64 = 0,
    signal_inhibit_secondary_start: u64 = 0,
    signal_inhibit_secondary_end: u64 = 0,
    signal_owner: bool = false,
    // T owns this address for the current thread generation. A publisher may
    // finish its copyout after T is dropped; writing a stale 1 into a reused
    // LPR signal slot is only a false positive, never a false negative.
    signal_pending_hint_va: u64 = 0,
    fault_entry: u64 = 0,
    fault_stack_base: u64 = 0,
    fault_stack_size: u64 = 0,
    active_fault_frame: u64 = 0,
    deadline_node: deadline_index.Node = .{},
    retry_link: std.DoublyLinkedList.Node = .{},
    retry_queue: ?u1 = null,
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
const initial_context_pointers: [initialThreadCapacity]*ThreadContext = blk: {
    var pointers: [initialThreadCapacity]*ThreadContext = undefined;
    for (0..initialThreadCapacity) |i| pointers[i] = &initial_thread_contexts[i];
    break :blk pointers;
};
var initial_thread_hot_states: [initialThreadCapacity]ThreadHotState = buildInitialThreadHotStates();
pub export var lapic_tick_count: u64 = 0;
pub var scheduler_tick_accum: u64 = 0;
pub var scheduler_switch_count: u64 = 0;
pub var scheduler_timer_log_once: u8 = 0;
pub var initial_x_state: [xstate_bytes]u8 align(x86_platform.xstate_alignment) = [_]u8{0} ** xstate_bytes;
// A reader outside KernelState must not pair an old gate with a target that
// the lifecycle owner has already replaced. Publish state and target together.
var principal_lifecycle_gate: usize = 0;
var principal_lifecycle_record: lock_diagnostics.Record = .{};
var principal_lifecycle_pending_action: u8 = 0;
var principal_lifecycle_pending_code: u32 = 0;
// First word covers boot principals without allocating. Further words use the
// shared SlotTable, prepared under K before admitting a principal's thread.
// Words stay alive with directory capacity (body reclamation is stage B).
var principal_self_exit_reservations: [1]u64 = .{0};
const ReservationTable = @import("memory/slot_table.zig").SlotTable(u64);
var principal_self_exit_extra: ReservationTable = .{};
var principal_self_exit_leaf: @import("memory/slot_table.zig").Leaf = .{};

fn preparePrincipalSelfExit(target: kernel.PrincipalId, allocator: owned_storage.Allocator) bool {
    const index = kernel.processIndexFromPrincipal(target) orelse return false;
    const word = index / 64;
    // K serializes writers; the leaf protects directory readers. allocate
    // reserves/returns grants outside the leaf and publishes under it.
    while (principal_self_exit_extra.live < word) {
        _ = principal_self_exit_extra.allocate(allocator, &principal_self_exit_leaf, @intCast((kernel.process_principal_value_count - 1) / 64)) catch return false;
    }
    return true;
}

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

// Identity history belongs to the directory, not a reclaimable Context.
// Context.generation is a read-mostly execution cache, written at admission
// and logical release under T. The head slot also describes its shared grants.
const no_reclaim_group = std.math.maxInt(u32);
const ThreadSlot = struct {
    generation: u32 = 1,
    group_start: usize = 0,
    group_count: usize = 0,
    retiring: bool = false,
    live: u32 = 0,
    reclaim_previous: u32 = no_reclaim_group,
    reclaim_next: u32 = no_reclaim_group,
    reclaim_queued: bool = false,
    // Both grants have group_count elements; do not store that length twice.
    contexts: ?[*]ThreadContext = null,
    entities: ?[*]scheduler_runqueue.Node = null,
};
var initial_thread_slots: [initialThreadCapacity]ThreadSlot = [_]ThreadSlot{.{}} ** initialThreadCapacity;
// An absent body has no executable identity. Keeping a non-null inactive view
// preserves the existing hot lookup shape; only admission may replace it.
var absent_thread_context: ThreadContext = .{ .generation = 0 };

const ThreadDirectory = struct {
    contexts: []const *ThreadContext,
    hot_states: []ThreadHotState,
    slots: []ThreadSlot,
};

const initial_thread_directory: ThreadDirectory = .{
    .contexts = &initial_context_pointers,
    .hot_states = &initial_thread_hot_states,
    .slots = &initial_thread_slots,
};

const ThreadTableState = struct {
    lock_state: SchedulerSpinLock = .{},
    deadlines: deadline_index.Index = .{},
    retries: [2]std.DoublyLinkedList = .{ .{}, .{} },
    retry_enqueue: u1 = 0,
    reclaim_head: ?usize = null,
    storage: owned_storage.Owner = .{},
    directory: *const ThreadDirectory = &initial_thread_directory,
    directory_lock: SchedulerSpinLock = .{},
    next_cpu_cursor: usize = bootstrap_cpu_slot,

    fn lock(self: *ThreadTableState) void {
        self.lock_state.lock();
    }

    fn unlock(self: *ThreadTableState) void {
        if (builtin.is_test) _ = @atomicRmw(u64, &WaitAdmissionTest.unlocks, .Add, 1, .monotonic);
        self.lock_state.unlock();
    }
};

fn threadDirectory() *const ThreadDirectory {
    // One acquired root binds lengths and pointers to the SAME publication.
    // Root lengths stay fixed; entries change only under T and the leaf.
    // Caller owns table or the short directory leaf. Metadata may be returned
    // after publication excludes both readers; body return also drains CPU pins.
    return @atomicLoad(*const ThreadDirectory, &scheduler_state.thread_table.directory, .acquire);
}

fn lockThreadDirectory() void {
    // Terminal leaf only: never take table/CPU/runqueue/PMM locks or call
    // logging from here. IRQ masking prevents a reader interrupting itself.
    scheduler_state.thread_table.directory_lock.lock();
}

fn unlockThreadDirectory() void {
    scheduler_state.thread_table.directory_lock.unlock();
}

fn threadContextLocked(thread_index: usize) ?*ThreadContext {
    // Table exclusion protects this directory; the caller also owns the
    // mutable context loan. This is not a pin for use after dropping table.
    const directory = threadDirectory();
    if (thread_index >= directory.contexts.len) return null;
    return directory.contexts[thread_index];
}

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
    cpus: []CpuSchedulerState = &bootstrap_cpu_states,
};

var bootstrap_cpu_states = buildInitialCpuSchedulerStates();
var scheduler_state: SchedulerState = .{};

pub fn initCpuStorage(count: usize) !void {
    // BSP-only, before AP entry: copying lock/context rows is safe only while
    // no remote CPU can retain their addresses. Never truncate boot ownership.
    if (count < scheduler_state.cpus.len or count < base_shadow.len) return error.InvalidCpuCount;
    const storage = @import("arch/x86_64/cpu_runtime.zig");
    const states = try storage.alloc(CpuSchedulerState, count);
    const shadows = try storage.alloc(BaseShadow, count);
    @memset(states, .{});
    @memset(shadows, .{});
    @memcpy(states[0..scheduler_state.cpus.len], scheduler_state.cpus);
    @memcpy(shadows[0..base_shadow.len], base_shadow);
    scheduler_state.cpus = states;
    base_shadow = shadows;
}

fn verifiedCoreCpuCount() usize {
    return @atomicLoad(usize, &scheduler_state.cpu_count, .acquire);
}

fn verifiedThreadIdForGeneration(thread_index: usize, generation: u32) ?i64 {
    // A fixed low word keeps verified tie-break ordering independent of
    // directory capacity. GETTID reserves zero, so index+1 must fit u32.
    if (thread_index >= std.math.maxInt(u32) or generation > max_thread_generation) return null;
    return @intCast((@as(u64, generation) << 32) | (@as(u64, thread_index) + 1));
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

fn schedulerEntityLocked(thread_index: usize) ?*scheduler_runqueue.Node {
    const ctx = threadContextLocked(thread_index) orelse return null;
    return ctx.scheduler_entity;
}

fn cpuRunqueueFloor(state: *const CpuSchedulerState) i64 {
    return state.runqueue.minimumVruntime() orelse state.virtual_time;
}

fn setCpuVirtualTime(state: *CpuSchedulerState, value: i64) void {
    state.virtual_time = value;
    state.runqueue.setSelectionVirtualTime(value);
}

fn refreshCpuVirtualTime(state: *CpuSchedulerState) void {
    if (state.runqueue.minimumVruntime()) |minimum| {
        if (minimum > state.virtual_time) setCpuVirtualTime(state, minimum);
    }
}

const RuntimeAccountingTest = struct {
    var now_ns: ?u64 = null;
};
fn runtimeAccountingNow() ?u64 {
    return if (builtin.is_test) RuntimeAccountingTest.now_ns else @import("realtime_clock.zig").monotonicNs();
}
const PreparedRuntime = struct {
    entity: verified_sched.Entity,
    interval: @import("scheduler_runtime_accounting.zig").Accounting.Prepared,
};
// Caller owns the runqueue lock. Preparation is read-only so rejection of a
// fused transition cannot charge time twice or change its fallback inputs.
fn prepareRuntimeCharge(state: *CpuSchedulerState, node: *scheduler_runqueue.Node, fallback_ns: u64) ?PreparedRuntime {
    const interval = state.runtime_accounting.prepare(runtimeAccountingNow(), fallback_ns);
    if (interval.runtime_ns > std.math.maxInt(i64)) return null;
    var charged = node.entity;
    if (interval.runtime_ns != 0 and verified_sched.pacha_eevdf_entity_charge(&node.entity, @intCast(interval.runtime_ns), cpuRunqueueFloor(state), &charged) != .ok) return null;
    return .{ .entity = charged, .interval = interval };
}
fn chargeRunningTransition(state: *CpuSchedulerState, node: *scheduler_runqueue.Node) bool {
    const prepared = prepareRuntimeCharge(state, node, 0) orelse return false;
    node.entity = prepared.entity;
    state.runtime_accounting.commit(prepared.interval);
    return true;
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
    return if (ctx.cpu_affinity) |set| set.contains(cpu_id) else true;
}

fn schedulerCpuEnabled(cpu_id: usize) bool {
    const state = schedulerStateForSlot(cpu_id) orelse return false;
    state.lock.lock();
    const enabled = state.enabled;
    state.lock.unlock();
    return enabled;
}

fn verifiedAddThread(thread_index: usize, generation: u32, ready: bool) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return verifiedAddThreadLocked(thread_index, generation, ready);
}

fn verifiedAddThreadLocked(thread_index: usize, generation: u32, ready: bool) bool {
    if (!verifiedCoreReady()) return false;
    const thread_id = verifiedThreadIdForGeneration(thread_index, generation) orelse return false;
    const cpu_id = verifiedThreadCpu(thread_index);
    const state = schedulerStateForSlot(cpu_id) orelse return false;
    const node = schedulerEntityLocked(thread_index) orelse return false;
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
    deferred_notify_cpu: ?*?usize,
) bool {
    const start = perf.timestamp();
    perf.add(.wake_scan_calls, 1);
    defer perf.elapsed(.wake_scan_cycles, start);
    if (!verifiedCoreReady()) return false;
    // Wake callers own table through the context and runqueue transition.
    const ctx = threadContextLocked(thread_index) orelse return false;
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
    if (woke) wake_trace.record(.ready_publish, thread_index, 0, thread_index);
    if (woke and target_cpu != currentCpu()) {
        // Notification commit retains K/E/T until publication. Return only a
        // CPU number so its caller can end E before the coalesced wake edge.
        if (deferred_notify_cpu) |out| out.* = target_cpu else _ = smp.wakeCpu(target_cpu);
    }
    return woke;
}

fn verifiedWakeThreadGeneration(thread_index: usize, generation: u32) bool {
    return verifiedWakeThreadGenerationPreferred(thread_index, generation, null, null);
}

fn verifiedWakeThread(thread_index: usize) void {
    const ctx = threadContextLocked(thread_index) orelse return;
    if (!ctx.allocated) return;
    _ = verifiedWakeThreadGeneration(thread_index, ctx.generation);
}

fn verifiedBlockThreadGeneration(thread_index: usize, generation: u32) bool {
    if (!verifiedCoreReady()) return false;
    const node = schedulerEntityLocked(thread_index) orelse return false;
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
            if (!chargeRunningTransition(state, node)) {
                state.runqueue_lock.unlock();
                return false;
            }
            state.current_entity = null;
            state.runtime_accounting.end();
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
    const ctx = threadContextLocked(thread_index) orelse return;
    if (!ctx.allocated) return;
    _ = verifiedBlockThreadGeneration(thread_index, ctx.generation);
}

fn verifiedExitThreadGeneration(thread_index: usize, generation: u32) bool {
    if (!verifiedCoreReady()) return false;
    const node = schedulerEntityLocked(thread_index) orelse return false;
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
            if (!chargeRunningTransition(state, node)) {
                state.runqueue_lock.unlock();
                return false;
            }
            state.current_entity = null;
            state.runtime_accounting.end();
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
    if (!chargeRunningTransition(state, node)) {
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
    state.runtime_accounting.end();
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
    const prepared = prepareRuntimeCharge(state, node, runtime_ns) orelse {
        state.runqueue_lock.unlock();
        return .invalid;
    };
    const charged = prepared.entity;
    state.runtime_accounting.commit(prepared.interval);
    var minimum = charged.vruntime;
    if (state.runqueue.minimumVruntime()) |queued_minimum| minimum = @min(minimum, queued_minimum);
    if (minimum > state.virtual_time) setCpuVirtualTime(state, minimum);
    node.entity = charged;
    // A handed-off sender may retain vruntime below its eligibility time.
    // That queued minimum must not freeze V forever while the receiver runs:
    // it would prevent the sender from publishing the next IPC message. If
    // nobody is eligible, advance toward the first eligibility boundary, but
    // never beyond the service already charged to the running entity.
    if (state.runqueue.bestEligible(state.virtual_time) == null) {
        if (state.runqueue.minimumEligibleTime()) |eligible| {
            const progress = @min(charged.vruntime, eligible);
            if (progress > state.virtual_time) setCpuVirtualTime(state, progress);
        }
    }
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
    state.runtime_accounting.end();
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
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return verifiedPickThreadForCpuLocked(cpu_id);
}

fn verifiedPickThreadForCpuLocked(cpu_id: usize) ?PickedThread {
    return verifiedPickThreadForCpuPreferredLocked(cpu_id, null);
}

fn verifiedPickThreadForCpuPreferredLocked(cpu_id: usize, preferred: ?kernel.ThreadWakeTarget) ?PickedThread {
    if (!verifiedCoreReady()) return null;
    const state = schedulerStateForSlot(cpu_id) orelse return null;
    state.runqueue_lock.lock();
    if (state.current_entity != null) {
        state.runqueue_lock.unlock();
        return null;
    }
    // The cache is maintained by tree/virtual-time updates, not by this
    // predicate. Competitors do not exclude a preferred EEVDF winner.
    var node: ?*scheduler_runqueue.Node = null;
    if (preferred) |hint| {
        if (runnableHandoffTargetLocked(state, cpu_id, hint.thread_index, hint.thread_generation)) |target| {
            if (state.runqueue.selection_virtual_time == state.virtual_time and state.runqueue.cachedSelection() == target) {
                node = target;
                setCpuVirtualTime(state, state.runqueue.cachedSelectionTime());
            }
        }
    }
    if (node == null) node = state.runqueue.bestEligible(state.virtual_time);
    if (node == null) {
        if (state.runqueue.minimumEligibleTime()) |next_eligible| {
            if (next_eligible > state.virtual_time) setCpuVirtualTime(state, next_eligible);
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
    state.runtime_accounting.start(runtimeAccountingNow());
    const thread_index = selected.thread_index;
    const generation = selected.generation;
    state.runqueue_lock.unlock();

    const ctx = threadContextLocked(thread_index) orelse {
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
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    verifiedRollbackHandoffLocked(cpu_id, thread_index, generation);
}

fn verifiedRollbackHandoffLocked(cpu_id: usize, thread_index: usize, generation: u32) void {
    const state = schedulerStateForSlot(cpu_id) orelse return;
    state.runqueue_lock.lock();
    const current = state.current_entity orelse {
        state.runqueue_lock.unlock();
        return;
    };
    const current_thread_index = current.thread_index;
    const current_generation = current.generation;
    state.runqueue_lock.unlock();
    _ = verifiedHandoffToThreadOnCpuLocked(
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
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return verifiedHandoffToThreadOnCpuLocked(only_ready, cpu_id, thread_index, generation, current_thread_index, current_generation);
}

fn runnableHandoffTargetLocked(state: anytype, cpu_id: usize, thread_index: usize, generation: u32) ?*scheduler_runqueue.Node {
    const target = schedulerEntityLocked(thread_index) orelse return null;
    if (!nodeMatches(target, thread_index, generation, cpu_id) or target.ownership != .runnable) return null;
    _ = state;
    return target;
}

fn verifiedHandoffToThreadOnCpuLocked(
    comptime only_ready: bool,
    cpu_id: usize,
    thread_index: usize,
    generation: u32,
    current_thread_index: usize,
    current_generation: u32,
) bool {
    if (!verifiedCoreReady()) return false;
    const state = schedulerStateForSlot(cpu_id) orelse return false;
    const target = schedulerEntityLocked(thread_index) orelse return false;
    state.runqueue_lock.lock();
    // Generic futex wake must not bypass an unrelated runnable competitor.
    // Recheck at the ownership transition, not at an earlier wake hint.
    if (only_ready and state.runqueue.count != 1) {
        state.runqueue_lock.unlock();
        return false;
    }
    if (runnableHandoffTargetLocked(state, cpu_id, thread_index, generation) == null) {
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
    if (!chargeRunningTransition(state, current)) {
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
    const ctx = threadContextLocked(thread_index) orelse return false;
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
    // Caller holds table until the entry's generation is pinned to its CPU.
    const ctx = threadContextLocked(thread_index) orelse return false;
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

// Fixtures that model remote topology own their rows just as boot does.
// They may exceed the small default fixture without creating a runtime cap.
const TestCpuRows = struct {
    old_rows: []CpuSchedulerState,
    old_shadows: []BaseShadow,
    rows: []CpuSchedulerState,
    shadows: []BaseShadow,
    fn init(count: usize) !TestCpuRows {
        if (!builtin.is_test) @compileError("host-only topology fixture");
        const rows = try std.testing.allocator.alloc(CpuSchedulerState, count);
        errdefer std.testing.allocator.free(rows);
        const shadows = try std.testing.allocator.alloc(BaseShadow, count);
        @memset(rows, .{});
        @memset(shadows, .{});
        const result = TestCpuRows{ .old_rows = scheduler_state.cpus, .old_shadows = base_shadow, .rows = rows, .shadows = shadows };
        scheduler_state.cpus = rows;
        base_shadow = shadows;
        return result;
    }
    fn deinit(self: TestCpuRows) void {
        comptime std.debug.assert(builtin.is_test);
        initializeStaticStorage();
        scheduler_state.cpus = self.old_rows;
        base_shadow = self.old_shadows;
        initializeStaticStorage();
        std.testing.allocator.free(self.rows);
        std.testing.allocator.free(self.shadows);
    }
};

// These four borrowed sets belong only to host scheduler scenarios.
var affinity_fixture_words = [_][1]usize{ .{0}, .{1}, .{2}, .{3} };
fn testAffinity(mask: usize) CpuSet {
    std.debug.assert(@import("builtin").is_test and mask < affinity_fixture_words.len);
    return .{ .words = &affinity_fixture_words[mask], .capacity = @bitSizeOf(usize) };
}

test "preferred wake preserves fallback affinity and outgoing block tail" {
    const cases = [_]struct { preferred: ?usize, enabled: bool = true, affinity: usize = 3, tail: bool = false, expected: usize }{
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
        ctx.* = .{ .id = tid, .allocated = true, .scheduler_entity = node, .cpu_affinity = testAffinity(case.affinity) };
        defer ctx.* = .{ .id = tid, .scheduler_entity = node };
        scheduler_state.cpu_count = 2;
        scheduler_state.cpus[0].is_idle = true;
        setTestCurrentThread(&scheduler_state.cpus[0], idleThreadMarker);
        scheduler_state.cpus[1].enabled = case.enabled;
        try std.testing.expect(verifiedAddThread(tid, 1, false));
        if (case.tail) {
            scheduler_state.cpus[0].is_idle = false;
            setTestCurrentThread(&scheduler_state.cpus[0], tid);
        }
        try std.testing.expect(verifiedWakeThreadGenerationPreferred(tid, 1, case.preferred, null));
        try std.testing.expectEqual(case.expected, ctx.cpu_slot);
        try std.testing.expectEqual(.runnable, node.ownership);
        try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[case.expected].runqueue.count);
        try std.testing.expect(!verifiedWakeThreadGenerationPreferred(tid, 1, case.preferred, null));
    }
}

test "migration waits for the outgoing CPU context to be saved and released" {
    const topology_fixture = try TestCpuRows.init(72);
    defer topology_fixture.deinit();
    // Exercise both producers of the runnable-before-context-save interval.
    for (0..8) |scenario| {
        const handoff = scenario % 2 != 0;
        const destination = ([_]usize{ 1, 5, 7, 71 })[scenario / 2];
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
        setTestCurrentThread(&scheduler_state.cpus[0], tid);
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
        setTestCurrentThread(&scheduler_state.cpus[0], idleThreadMarker);
        scheduler_state.cpus[0].is_idle = true;
        try std.testing.expect(migrateRunnableThreadGeneration(tid, generation, destination));
        try std.testing.expectEqual(destination, ctx.cpu_slot);
        try std.testing.expectEqual(@as(u64, 0x2222), ctx.frame.rip);

        // A handoff to another live thread also releases the outgoing identity;
        // migration must not require the entire source CPU to become idle.
        setTestCurrentThread(&scheduler_state.cpus[destination], tid + 1);
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
            const ctx = threadContextLocked(node.thread_index);
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
        const ctx = threadContextLocked(node.thread_index);
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

inline fn requireLocalContextWriter(state: *CpuSchedulerState) void {
    if (!builtin.is_test) {
        const cpu = smp.currentCpuSlotOrInvalid();
        // Entry/return readers take no lock. Only this CPU, with interrupts
        // masked, may replace their pin. Remote observers take state.lock.
        if (cpu >= scheduler_state.cpus.len or state != &scheduler_state.cpus[cpu] or interruptsEnabled()) {
            while (true) asm volatile ("cli; hlt");
        }
    }
}

inline fn installCurrentContextLocked(state: *CpuSchedulerState, thread_index: usize, ctx: *ThreadContext) void {
    requireLocalContextWriter(state);
    // The marker belongs to a deferred syscall return, not to a thread's
    // later user frames. Consume it at pin installation, before any user or
    // interrupt can run, so it cannot survive preemption with the same RIP.
    ctx.entry_state.notify_sysret_continuation = false;
    state.current_context = ctx;
    state.current_generation = ctx.generation;
    state.current_thread = thread_index;
    state.current_principal = ctx.owner_process;
    state.current_cr3 = ctx.cr3;
    state.is_idle = false;
}

inline fn clearCurrentContextLocked(state: *CpuSchedulerState) void {
    requireLocalContextWriter(state);
    state.current_context = null;
    state.current_generation = 0;
    state.current_thread = state.idle_thread;
    state.current_principal = null;
    state.current_cr3 = 0;
    state.is_idle = true;
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
    // The picked generation may have been stopped while stealing/selecting.
    // Revalidate its entry and publish the CPU pin in one table interval;
    // acquiring table after CPU state would invert lifecycle's lock order.
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    state.lock.lock();
    if (!state.enabled or !fillUserEntryForThread(cpu_id, thread_index, picked.generation, out_entry)) {
        state.lock.unlock();
        verifiedRollbackPickedCpu(cpu_id, picked);
        return false;
    }
    const ctx = threadContextLocked(thread_index) orelse {
        state.lock.unlock();
        verifiedRollbackPickedCpu(cpu_id, picked);
        return false;
    };
    installCurrentContextLocked(state, thread_index, ctx);
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
    const b3_select = b3.now();
    const picked = verifiedPickThreadForCpu(cpu_id) orelse {
        b3.end(207, b3_select);
        return false;
    };
    b3.end(207, b3_select);
    const b3_switch = b3.now();
    defer b3.end(208, b3_switch);
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
/// Whether an interrupt on this CPU arrived inside the lock-free idle HLT.
pub fn currentCpuInLockFreeIdleWait() bool {
    const state = schedulerStateForSlot(currentCpu()) orelse return false;
    return @atomicLoad(bool, &state.lock_free_idle_wait, .monotonic);
}

pub fn testSetCurrentCpuLockFreeIdleWait(waiting: bool) void {
    if (!builtin.is_test) @compileError("test-only idle interrupt admission");
    const state = schedulerStateForSlot(currentCpu()) orelse unreachable;
    @atomicStore(bool, &state.lock_free_idle_wait, waiting, .monotonic);
}

pub fn loadNextReadyThreadOrIdle(frame: *TrapFrame) void {
    if (loadNextReadyThread(frame)) return;
    _ = stealRunnableForIdleCpu(currentCpu());
    if (loadNextReadyThread(frame)) return;
    // No saved context is referenced beyond this point.  In particular the
    // BSP must drop ownership of an exited/blocked principal before entering
    // its interruptible wait, otherwise remote lifecycle quiescence waits on
    // a CPU which is already logically idle.
    noteCpuIdleTick(currentCpu());
    const idle_state = schedulerStateForSlot(currentCpu());
    while (!loadNextReadyThread(frame)) {
        if (idle_state) |state| @atomicStore(bool, &state.lock_free_idle_wait, true, .monotonic);
        asm volatile ("sti; hlt; cli" ::: .{ .memory = true });
        if (idle_state) |state| @atomicStore(bool, &state.lock_free_idle_wait, false, .monotonic);
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

fn advanceThreadSlotGenerationLocked(index: usize) u32 {
    const slot = &threadDirectory().slots[index];
    slot.generation = nextThreadGeneration(slot.generation);
    return slot.generation;
}

fn nextThreadGeneration(current: u32) u32 {
    // Saturation retires the slot: admission rejects the terminal generation.
    // Never wrap an identity while an old capability can still name it.
    return if (current >= max_thread_generation) max_thread_generation else current + 1;
}

pub fn initializeStaticStorage() void {
    lock_diagnostics.assertAllQuiescent();
    scheduler_state.thread_table.deadlines = .{};
    scheduler_state.thread_table.retries = .{ .{}, .{} };
    scheduler_state.thread_table.retry_enqueue = 0;
    scheduler_state.thread_table.reclaim_head = null;
    for (&initial_thread_contexts) |*ctx| {
        ctx.deadline_node = .{};
        ctx.retry_link = .{};
        ctx.retry_queue = null;
        // Fresh boot has no waits. Host fixtures rebuild registrations when
        // publishing their replacement contexts, never from saved old values.
        if (!builtin.is_test) std.debug.assert(ctx.wake_tick == 0 and !ctx.wait_completion_publish_pending);
    }
    @atomicStore(usize, &scheduler_state.cpu_count, 1, .release);
    @memset(principal_self_exit_reservations[0..], 0);
    var cpu_slot: usize = 0;
    while (cpu_slot < scheduler_state.cpus.len) : (cpu_slot += 1) {
        scheduler_state.cpus[cpu_slot] = .{};
    }
    scheduler_state.cpus[bootstrap_cpu_slot].enabled = true;
    scheduler_state.cpus[bootstrap_cpu_slot].current_thread = 0;
    // Index zero is only a pre-user placeholder. Pin acquisition occurs at
    // activation, before boot publishes kernel_state_ready, not construction.
    scheduler_state.cpus[bootstrap_cpu_slot].current_context = null;
    scheduler_state.cpus[bootstrap_cpu_slot].current_generation = 0;
    scheduler_state.cpus[bootstrap_cpu_slot].is_idle = false;
    var thread_index: usize = 0;
    while (thread_index < initialThreadCapacity) : (thread_index += 1) {
        if (builtin.is_test) initial_thread_slots[thread_index].generation = initial_thread_contexts[thread_index].generation;
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
    endBaseShadow(cpu_slot);
    const state = schedulerStateForSlot(cpu_slot) orelse return;
    state.lock.lock();
    clearCurrentContextLocked(state);
    state.deferred_slice_ticks = 0;
    state.runqueue_lock.lock();
    state.runtime_accounting.reset();
    state.runqueue_lock.unlock();
    state.lock.unlock();
}

fn claimKernelScheduledUserEntryFromAp(cpu_slot: usize, out_entry: *scheduler_observer.UserEntry) callconv(.c) bool {
    return claimKernelScheduledUserEntry(cpu_slot, out_entry);
}

fn markThreadReadyLocked(thread_index: usize, ready: bool) bool {
    const ctx = threadContextLocked(thread_index) orelse return false;
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

pub fn markThreadReadyGeneration(thread_index: usize, generation: u32, owner: kernel.PrincipalId, ready: bool) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    _ = matchingThreadLocked(thread_index, generation, owner) orelse return false;
    return markThreadReadyLocked(thread_index, ready);
}

// Identity admission and the caller's action must share one table hold. A
// separate liveness probe cannot authorize a later operation on a reused slot.
fn matchingThreadLocked(thread_index: usize, generation: u32, owner: kernel.PrincipalId) ?*ThreadContext {
    const ctx = threadContextLocked(thread_index) orelse return null;
    if (!ctx.allocated or ctx.generation != generation or ctx.owner_process != owner) return null;
    if (builtin.is_test) {
        if (ThreadIdentityTestProbe.admitted) |probe| @atomicStore(u8, probe, 1, .release);
        if (ThreadIdentityTestProbe.proceed) |probe| {
            while (@atomicLoad(u8, probe, .acquire) == 0) std.atomic.spinLoopHint();
        }
    }
    return ctx;
}

pub fn preemptApUserThread(slice_ticks: u64, frame: *const TrapFrame) SliceDecision {
    if (slice_ticks == 0 or slice_ticks > std.math.maxInt(u64) / runtime_ns_per_tick) return .invalid;
    const cpu_slot = currentCpu();
    if (cpu_slot == bootstrap_cpu_slot) return .invalid;
    const state = schedulerStateForSlot(cpu_slot) orelse return .invalid;
    const thread_index = currentThread();
    scheduler_state.thread_table.lock();
    const ctx = threadContextLocked(thread_index) orelse {
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
    ctx.entry_state.notify_sysret_continuation = false;
    ctx.cr3 = currentCr3();
    const live = captureLeavingBases(x86_platform, currentCpu());
    ctx.fs_base = live.fs;
    ctx.gs_base = live.gs;
    ctx.pkru = live.pkru;
    ctx.ready = true;
    setThreadHotCr3(thread_index, ctx.cr3);
    setThreadHotReady(thread_index, true);
    clearCurrentContextLocked(state);
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

/// Install this CPU's next local runnable thread into `frame` after an AP
/// block, holding the thread table. The blocked source is already saved and
/// out of the running state, so this must not recapture it: like the futex
/// wake-wait transfer, only activate and load the picked saved context. Its
/// XState and FS/GS/PKRU are restored by the common syscall user return for
/// the new current thread. Local runqueue only; the idle path still steals.
/// `live` is what the blocking thread's save just read from this CPU's
/// hardware; `cpu_id` must be the executing CPU, because the bases written
/// here and the shadow slot recording them are that CPU's.
fn installNextLocalAfterBlockLocked(comptime Registers: type, cpu_id: usize, frame: *TrapFrame, live: ?LiveBases, preferred: ?kernel.ThreadWakeTarget) bool {
    std.debug.assert(cpu_id == currentCpu());
    const ipc_direction = ipc_metric.syscallDirection(frame.rax);
    const ipc_pick_start = ipc_metric.timestamp();
    const picked = verifiedPickThreadForCpuPreferredLocked(cpu_id, preferred) orelse {
        ipc_metric.phase(ipc_direction, .select, ipc_pick_start);
        return false;
    };
    ipc_metric.phase(ipc_direction, .select, ipc_pick_start);
    const ipc_switch_start = ipc_metric.timestamp();
    defer ipc_metric.phase(ipc_direction, .@"switch", ipc_switch_start);

    // Installing the bases here, over values known to be live, writes only
    // the fields the two threads differ in; leaving them to the user return
    // made applyThreadBases write all three for the newly current thread.
    if (activateGenerationLockedWithRegisters(Registers, false, picked.thread_index, picked.generation) and
        loadContextIntoFrameOverLiveBasesLocked(Registers, cpu_id, picked.thread_index, picked.generation, frame, live))
    {
        // Measured service was settled into the source before block. Keep
        // its timer cadence and fallback credit until timer settlement; a
        // clock loss must not charge that same interval to the new owner.
        // Unmeasured legacy operation retains its former tick-only behavior.
        if (schedulerStateForSlot(cpu_id)) |state| {
            state.lock.lock();
            state.runqueue_lock.lock();
            if (state.runtime_accounting.checkpoint_ns == null and state.runtime_accounting.credit_ns == 0)
                state.deferred_slice_ticks = 0;
            state.runqueue_lock.unlock();
            state.lock.unlock();
        }
        return true;
    }
    // A failed activate may have published the pick as current; the caller's
    // idle publication overwrites it before the table is released.
    verifiedRollbackPickedCpu(cpu_id, picked);
    return false;
}

pub fn tryIpcCompletedWait(frame: *TrapFrame, token: u64) bool {
    if (token == 0) return false;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(currentThread()) orelse return false;
    if (!ctx.allocated or !ctx.ready or ctx.stopped or ctx.active_wait_token != token or ctx.wait_mailbox) return false;
    // A remote completion won before block. Consume precisely the same saved
    // result as the ordinary block path, without severing CPU ownership or
    // materializing XState just to return to this same thread.
    frame.rax = ctx.frame.rax;
    if (ctx.wait_poll_result) |result| {
        frame.r10 = result.index;
        frame.r8 = result.revents;
        ctx.wait_poll_result = null;
    }
    if (ctx.wait_ipc_words) |words| {
        @import("kernel_abi_root").ipc_abi.outputWords(frame, if (frame.rax == 0) words else .{ 0, 0, 0, 0 });
        frame.r15 = if (frame.rax == 0) ctx.wait_ipc_fd_count else 0;
        ctx.wait_ipc_words = null;
    }
    ctx.active_wait_token = 0;
    setWakeTickLocked(ctx, 0);
    ctx.wait_completion_publish_pending = false;
    ctx.wait_completion_rax = 0;
    return true;
}

/// The delivery has committed under K. Re-admit both identities under T:
/// a completion/STOP/exit in the delivery-to-switch interval is a harmless
/// refusal here and remains the responsibility of the ordinary block path.
/// No lock drop is permitted between saving the source and installing target.
pub fn tryIpcDirectSwitch(frame: *TrapFrame, uninterruptible: bool, token: u64, timeout: u64, target: kernel.ThreadWakeTarget, before_leave: ?BeforeCurrentThreadLeaveCallback) bool {
    const direct_start = b3.now();
    defer b3.end(505, direct_start);
    const switched = blk: {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        break :blk tryIpcDirectSwitchLocked(if (builtin.is_test) ContextTransferTestRegisters else x86_platform, frame, uninterruptible, token, timeout, target);
    };
    if (!switched) return false;
    ipc_metric.recordElapsed(.direct_switch, 0);
    if (before_leave) |callback| callback.run(callback.context);
    return true;
}

fn tryIpcDirectSwitchLocked(comptime Registers: type, frame: *TrapFrame, uninterruptible: bool, token: u64, timeout: u64, hint: kernel.ThreadWakeTarget) bool {
    if (!verifiedCoreReady() or token == 0 or timeout != 0 or
        (!builtin.is_test and !isBootstrapSchedulerCpu() and !apUserDispatchReady())) return false;
    const cpu = currentCpu();
    const state = schedulerStateForSlot(cpu) orelse return false;
    // T retains the CPU pin. Check its scalar identity too: replacing the
    // directory lookup must not admit a mismatched current_thread publication.
    const source = pinnedContextForCpu(cpu) orelse return false;
    if (source.id != state.current_thread) return false;
    const peer = threadContextLocked(hint.thread_index) orelse return false;
    if (source == peer or !source.allocated or !source.ready or source.stopped or
        source.cpu_slot != cpu or source.active_wait_token != token or !source.wait_mailbox or
        source.wait_completion_claimed or source.wait_completion_publish_pending or source.wake_tick != 0 or
        principalLifecycleBlocksAdmission(source.owner_process) or
        (!uninterruptible and firstUnblockedPendingSignal(source) != 0 and source.signal_entry != 0 and source.signal_delivery_ready)) return false;
    if (!peer.allocated or peer.generation != hint.thread_generation or peer.owner_process != hint.owner or
        peer.cpu_slot != cpu or !peer.ready or peer.stopped or peer.wait_mailbox or
        peer.wait_completion_claimed or peer.wait_completion_publish_pending or
        principalLifecycleBlocksAdmission(peer.owner_process) or !threadAllowsCpu(peer, cpu) or
        loadableContextLocked(peer, peer.generation) == null) return false;
    const current = source.scheduler_entity orelse return false;
    const target = peer.scheduler_entity orelse return false;
    var live: LiveBases = undefined;
    {
        state.lock.lock();
        defer state.lock.unlock();
        state.runqueue_lock.lock();
        defer state.runqueue_lock.unlock();
        if (!state.enabled or state.is_idle or state.current_context != source or
            state.current_generation != source.generation or state.current_entity != current or
            !nodeMatches(current, source.id, source.generation, cpu) or current.ownership != .running or
            !nodeMatches(target, peer.id, peer.generation, cpu) or target.ownership != .runnable or
            !state.runqueue.contains(target) or state.runqueue.selection_virtual_time != state.virtual_time) return false;
        // Blocking source raises virtual_time to the queued minimum before
        // ordinary selection. The maintained cache includes that
        // advance as well as the no-eligible minimumEligibleTime fallback.
        if (state.runqueue.cachedPostBlockSelection() != target) {
            ipc_metric.recordElapsed(.direct_selection_mismatch, 0);
            return false;
        }
        const selected_time = state.runqueue.cachedPostBlockTime();
        const accounting = prepareRuntimeCharge(state, current, 0) orelse return false;
        if (!accounting.interval.measured) return false;
        var blocked: verified_sched.Entity = undefined;
        var running: verified_sched.Entity = undefined;
        if (verified_sched.pacha_eevdf_entity_block(&accounting.entity, &blocked) != .ok or
            verified_sched.pacha_eevdf_entity_mark_running(&target.entity, &running) != .ok) return false;

        // Commit: every frame/identity check is complete under T. Preserve
        // CPU tick accumulators and the armed one-shot timer; this transfer
        // must not mint a new quantum or reinitialize an entity's service.
        // Entry may still own live user XState. T pins source, and no
        // activation/publication has changed its CPU ownership yet.
        if (!@import("builtin").is_test) @import("xstate_runtime.zig").saveIpcLiveIfNeeded();
        const save_start = b3.now();
        var saved = frame.*;
        saved.rax = @import("syscall/numbers.zig").syscall_err_not_ready;
        live = saveCurrentThreadContextFromFrameLocked(Registers, &saved).?;
        b3.end(104, save_start);
        source.wait_uninterruptible = uninterruptible;
        commitThreadBlockedLocked(source);
        setWakeTickLocked(source, 0);
        setThreadHotWaitState(source.id, true, 0, false);
        setThreadHotCompletionClaimed(source.id, false);
        state.runtime_accounting.commit(accounting.interval);
        current.entity = blocked;
        current.ownership = .blocked;
        if (!state.runqueue.remove(target)) @panic("IPC direct switch lost admitted target");
        target.entity = running;
        target.ownership = .running;
        state.current_entity = target;
        setCpuVirtualTime(state, selected_time);
    }
    // T still pins both frames and excludes cancellation/migration. These
    // installs cannot fail after the readiness/CPU/generation preflight.
    const activation_start = b3.now();
    defer b3.end(208, activation_start);
    if (!activateGenerationLockedWithRegisters(Registers, false, peer.id, peer.generation) or
        !loadContextIntoFrameOverLiveBasesLocked(Registers, cpu, peer.id, peer.generation, frame, live))
        @panic("IPC direct switch violated activation preflight");
    return true;
}

fn commitThreadBlockedLocked(ctx: *ThreadContext) void {
    ctx.ready = false;
    // STOP may have observed this admitted syscall while it was runnable.
    // Once it commits a wait, CONT must wait for that completion too; keeping
    // STOP's earlier runnable snapshot would return the provisional frame.
    if (ctx.stopped) ctx.resume_after_stop = false;
}

// Host-only stop after the real admission/EEVDF block transition. The kernel
// ELF has neither this callback nor a branch; hardware capture uses the
// existing register fixture so tests do not execute privileged instructions.
const WaitAdmissionTest = struct {
    var unlocks: u64 = 0;
    var stopped_at_block: bool = false;
    var stop_after_block: bool = false;
};

pub fn parkApThreadForBlock(
    frame: *TrapFrame,
    wait_mailbox: bool,
    wait_uninterruptible: bool,
    wait_token: u64,
    timeout_ticks: u64,
    resume_rax: u64,
    before_block: ?BeforeCurrentThreadLeaveCallback,
) bool {
    return parkApThreadForBlockPreferred(frame, wait_mailbox, wait_uninterruptible, wait_token, timeout_ticks, resume_rax, before_block, null);
}

pub fn parkApThreadForBlockPreferred(
    frame: *TrapFrame,
    wait_mailbox: bool,
    wait_uninterruptible: bool,
    wait_token: u64,
    timeout_ticks: u64,
    resume_rax: u64,
    before_block: ?BeforeCurrentThreadLeaveCallback,
    preferred: ?kernel.ThreadWakeTarget,
) bool {
    const ipc_start = ipc_metric.timestamp();
    const ipc_direction = ipc_metric.syscallDirection(frame.rax);
    defer ipc_metric.phase(ipc_direction, .block, ipc_start);
    const current_thread = currentThread();
    scheduler_state.thread_table.lock();
    if (threadContextLocked(current_thread)) |ctx| {
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
                if (ctx.wait_poll_result) |result| {
                    frame.r10 = result.index;
                    frame.r8 = result.revents;
                    ctx.wait_poll_result = null;
                }
                if (ctx.wait_ipc_words) |words| {
                    @import("kernel_abi_root").ipc_abi.outputWords(frame, if (frame.rax == 0) words else .{ 0, 0, 0, 0 });
                    frame.r15 = if (frame.rax == 0) ctx.wait_ipc_fd_count else 0;
                    ctx.wait_ipc_words = null;
                }
                ctx.active_wait_token = 0;
                setWakeTickLocked(ctx, 0);
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
        if (firstUnblockedPendingSignal(ctx) != 0 and ctx.signal_entry != 0 and ctx.signal_delivery_ready and
            !wait_uninterruptible)
        {
            if (wait_token != 0) {
                pendIpcBindingLocked(&ctx.ipc_outgoing);
                pendIpcBindingLocked(&ctx.ipc_receiving);
                ctx.wait_uninterruptible = false;
                ctx.wait_mailbox = false;
                ctx.active_wait_token = 0;
            }
            scheduler_state.thread_table.unlock();
            return false;
        }
        var saved = frame.*;
        saved.rax = resume_rax;
        ctx.frame = saved;
        ctx.entry_state.notify_sysret_continuation = false;
        ctx.cr3 = currentCr3();
        // Read once here: the direct install below compares the next
        // thread's bases against these live values (nothing writes the
        // bases in between) to skip rewriting equal fields.
        const live = captureLeavingBases(if (builtin.is_test) ContextTransferTestRegisters else x86_platform, currentCpu());
        ctx.fs_base = live.fs;
        ctx.gs_base = live.gs;
        ctx.pkru = live.pkru;
        ctx.wait_mailbox = wait_mailbox;
        ctx.active_wait_token = wait_token;
        ctx.wait_completion_claimed = false;
        ctx.wait_completion_publish_pending = false;
        ctx.wait_completion_rax = 0;
        setWakeTickLocked(ctx, if (timeout_ticks == 0) 0 else lapic_tick_count +| timeout_ticks);
        ctx.wait_uninterruptible = wait_uninterruptible;
        commitThreadBlockedLocked(ctx);
        setThreadHotCr3(current_thread, ctx.cr3);
        setThreadHotWaitState(current_thread, wait_mailbox, ctx.wake_tick, false);
        setThreadHotCompletionClaimed(current_thread, false);
        verifiedBlockThread(current_thread);
        if (builtin.is_test) {
            std.debug.assert(WaitAdmissionTest.stop_after_block);
            std.debug.assert(scheduler_state.thread_table.lock_state.value == 1);
            WaitAdmissionTest.stopped_at_block = true;
            noteCpuIdleTick(currentCpu());
            scheduler_state.thread_table.unlock();
            if (before_block) |callback| callback.run(callback.context);
            return true;
        }
        // Before releasing the thread table, this CPU must advertise either a
        // runnable current thread or idle, never the blocked thread: a remote
        // completion selects its wake CPU under that same lock, and its block
        // tail checks accept exactly those two states.
        //
        // A thread the blocker handed the CPU to (an IPC sender) is usually
        // runnable right here. Installing it directly skips the idle loop's
        // redispatch, as the BSP path does with loadNextReadyThread. No HLT
        // follows, so a wake IPI racing the tail just lands in its user return.
        if (apUserDispatchReady() and installNextLocalAfterBlockLocked(x86_platform, currentCpu(), frame, live, preferred)) {
            scheduler_state.thread_table.unlock();
            if (before_block) |callback| callback.run(callback.context);
            return true;
        }
        // Nothing local to run: publish idle before the unlock. Publishing it
        // afterwards leaves a window where a completion sees this thread as
        // still executing here, rejects its preferred handoff CPU, and sends
        // an IPI that may interrupt the remaining ring-0 block tail; the wake
        // handler returns from ring-0 work, after which this path would enter
        // HLT with the only edge already consumed. Once idle is visible,
        // either a preferred IPC handoff consumes the runnable receiver on
        // the sender CPU, or an ordinary wake leaves a runnable entity
        // visible to apIdleLoop before it executes HLT.
        if (schedulerStateForSlot(currentCpu())) |state| {
            state.lock.lock();
            clearCurrentContextLocked(state);
            state.lock.unlock();
        }
    }
    scheduler_state.thread_table.unlock();
    if (before_block) |callback| callback.run(callback.context);
    if (builtin.is_test) return false;
    smp.returnCurrentApToIdleFromInterrupt();
}

pub fn exitApThreadToIdle() noreturn {
    exitApThreadToIdleAfter(null);
}

pub fn exitApThreadToIdleAfter(callback: ?BeforeCurrentThreadLeaveCallback) noreturn {
    _ = releaseCurrentThread();
    endBaseShadow(currentCpu());
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

    const generation = currentThreadGeneration() orelse return false;
    if (verifiedExpireSlice(currentCpu(), current_thread, generation, slice_ticks * runtime_ns_per_tick) != .preempt) return false;
    return switchToNextReadyOnCurrentCpu(frame, null);
}

pub fn threadCapacity() usize {
    lockThreadDirectory();
    defer unlockThreadDirectory();
    return threadDirectory().contexts.len;
}

fn allocKernelSlice(comptime T: type, free_list: *kernel.FreePageList, count: usize) ?[]T {
    if (count == 0) return null;
    const bytes = std.math.mul(usize, @sizeOf(T), count) catch return null;
    const rounded = std.math.add(usize, bytes, 4095) catch return null;
    const page_count = rounded / 4096;
    const paddr = free_list.popContiguousBelow(page_count, @import("arch/x86_64/physical_layout.zig").allocation_limit) catch return null;
    // PMM grants the extent; the thread-table publication/retirement protocol
    // owns its lifetime, which pointer conversion alone cannot establish.
    const raw = @import("arch/x86_64/physical_layout.zig").identitySlice(u8, .fromInt(paddr), page_count * 4096) orelse unreachable;
    @memset(raw, 0);
    const ptr: [*]T = @ptrCast(@alignCast(raw.ptr));
    return ptr[0..count];
}

fn nextThreadCapacity(required: usize, old_capacity: usize) ?usize {
    return @import("state/table.zig").nextGeometricCapacity(old_capacity, initialThreadCapacity, threadIdValueCount, required);
}

// Reconstitution and subsequent slot admission share T: an empty group must
// not be reclaimed in a gap between body publication and its first live slot.
// The existing T -> PMM edge is used only on this cold allocation path; no
// CPU/runqueue/directory leaf is held across allocator callbacks.
fn ensureThreadBody(index: usize, allocator: owned_storage.Allocator) bool {
    if (!builtin.is_test) @compileError("body-only admission is a host fixture");
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return ensureThreadBodyLocked(index, allocator);
}

fn ensureThreadBodyLocked(index: usize, allocator: owned_storage.Allocator) bool {
    const directory = threadDirectory();
    if (index >= directory.contexts.len or directory.contexts[index] != &absent_thread_context or
        directory.slots[index].generation >= max_thread_generation) return true;
    const start = directory.slots[index].group_start;
    const count = directory.slots[start].group_count;
    var tx = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, allocator);
    defer if (!tx.finished) tx.rollback() catch @panic("thread body rollback");
    const contexts = tx.reserve(ThreadContext, count, .stable) catch return false;
    const entities = tx.reserve(scheduler_runqueue.Node, count, .stable) catch return false;
    for (contexts, entities, start..) |*ctx, *node, slot_index| {
        const generation = directory.slots[slot_index].generation;
        node.reset(slot_index, generation);
        ctx.* = .{ .id = @intCast(slot_index), .generation = generation, .scheduler_entity = node };
    }
    tx.commitAdditionalStable() catch @panic("thread body publication");
    lockThreadDirectory();
    for (contexts, start..) |*ctx, slot_index| @constCast(directory.contexts)[slot_index] = ctx;
    directory.slots[start].contexts = contexts.ptr;
    directory.slots[start].entities = entities.ptr;
    directory.slots[start].retiring = false;
    unlockThreadDirectory();
    queueReclaimGroupLocked(start);
    return true;
}

// Only this cold lifecycle/retry path detaches bodies. T excludes every
// ordinary borrower; CPU locks cover even offline/local current_context pins,
// and runqueue locks cover selected entities as well as linked tree nodes.
// An empty group waiting for pins stays logically retired in slot metadata;
// the Owner's retired list receives it only after borrow exclusion is proved.
fn reclaimThreadBodies() void {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const directory = threadDirectory();
    var candidate = scheduler_state.thread_table.reclaim_head;
    while (candidate) |start| {
        const group = &directory.slots[start];
        candidate = if (group.reclaim_next == no_reclaim_group) null else group.reclaim_next;
        const count = group.group_count;
        std.debug.assert(count != 0 and group.live == 0);
        const contexts_ptr = group.contexts orelse {
            unqueueReclaimGroupLocked(start);
            continue;
        };
        const contexts = contexts_ptr[0..count];
        const entities = group.entities.?[0..count];
        group.retiring = true;
        lockAllCpuSchedulerStates();
        lockAllCpuRunqueues();
        var referenced = false;
        for (contexts, entities) |*ctx, *node| {
            // The candidate counter selects work, but never authorizes freeing
            // a live body or an intrusive timer/retry reference by itself.
            if (ctx.allocated or ctx.deadline_node.linked or ctx.retry_queue != null) {
                referenced = true;
                break;
            }
            if (node.ownership != .free or node.linked_tree != null or node.left != null or node.right != null or node.parent != null) {
                referenced = true;
                break;
            }
            for (scheduler_state.cpus) |*cpu| {
                if (cpu.current_context == ctx or cpu.current_entity == node or cpu.runqueue.root == node) {
                    referenced = true;
                    break;
                }
            }
            if (referenced) break;
        }
        if (!referenced) {
            // Drain short directory readers before withdrawing the pointers.
            // No allocator callbacks may run under CPU/runqueue/directory locks.
            lockThreadDirectory();
            @memset(@constCast(directory.contexts[start .. start + count]), &absent_thread_context);
            group.contexts = null;
            group.entities = null;
            unlockThreadDirectory();
        }
        unlockAllCpuRunqueues();
        unlockAllCpuSchedulerStates();
        if (referenced) continue;
        unqueueReclaimGroupLocked(start);
        scheduler_state.thread_table.storage.retireAllocation(ThreadContext, contexts.ptr) catch @panic("thread context retirement");
        scheduler_state.thread_table.storage.retireAllocation(scheduler_runqueue.Node, entities.ptr) catch @panic("thread node retirement");
    }
    // Rejected returns retain their original grant+issuer and are attempted
    // once on each lifecycle/ordinary timer retry, even without future growth.
    scheduler_state.thread_table.storage.retryUnpublished() catch @panic("thread body retry");
    scheduler_state.thread_table.storage.reclaimRetiredAfterBorrowExclusion() catch @panic("thread body return");
}

fn ensureThreadCapacity(required: usize, free_list: *kernel.FreePageList) bool {
    const old_capacity = snapshotThreadCapacityAndRetryReturns();
    if (required <= old_capacity) return true;
    const capacity = nextThreadCapacity(required, old_capacity) orelse return false;
    var transaction = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, identity_allocator.allocator(free_list));
    // Allocation is transaction-local outside the table. Rollback's pending
    // list MUST be changed under the table even on partial allocation failure.
    // New lock edge: thread_table -> PMM. This adapter never enters scheduler;
    // PMM's quarantine query takes no IOMMU/thread-table lock.
    defer if (!transaction.finished) {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        transaction.rollback() catch @panic("invalid thread storage rollback");
    };
    const draft = ThreadGrowth.reserve(&transaction, capacity, old_capacity) catch return false;

    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    scheduler_state.thread_table.storage.retryUnpublished() catch @panic("moved thread storage owner");
    return publishThreadGrowthLocked(&transaction, draft, required, old_capacity);
}

fn snapshotThreadCapacityAndRetryReturns() usize {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    // A previous publication already drained the leaf readers of its retired
    // generation. Retry even without growth, using each record's original PMM.
    scheduler_state.thread_table.storage.retryUnpublished() catch @panic("moved thread storage owner");
    scheduler_state.thread_table.storage.reclaimRetiredAfterBorrowExclusion() catch @panic("moved thread storage owner");
    return threadDirectory().contexts.len;
}

const ThreadGrowth = struct {
    directory: *ThreadDirectory,
    contexts: []*ThreadContext,
    slots: []ThreadSlot,
    added_contexts: []ThreadContext,
    hot: []ThreadHotState,
    entities: []scheduler_runqueue.Node,

    fn reserve(transaction: *owned_storage.Transaction, capacity: usize, old_capacity: usize) owned_storage.Error!ThreadGrowth {
        return .{
            .directory = &(try transaction.reserve(ThreadDirectory, 1, .replaceable))[0],
            .contexts = try transaction.reserve(*ThreadContext, capacity, .replaceable),
            .added_contexts = try transaction.reserve(ThreadContext, capacity - old_capacity, .stable),
            .hot = try transaction.reserve(ThreadHotState, capacity, .replaceable),
            .entities = try transaction.reserve(scheduler_runqueue.Node, capacity - old_capacity, .stable),
            .slots = try transaction.reserve(ThreadSlot, capacity, .replaceable),
        };
    }
};

// Separate locked publication from local preparation so a concurrent winner
// can be revalidated without publishing or losing any of the loser's drafts.
fn publishThreadGrowthLocked(transaction: *owned_storage.Transaction, draft: ThreadGrowth, required: usize, old_capacity: usize) bool {
    const old = threadDirectory();
    if (old.contexts.len != old_capacity) {
        return required <= old.contexts.len;
    }
    const new_contexts = draft.contexts;
    const new_hot_threads = draft.hot;
    const new_entities = draft.entities;
    const capacity = new_contexts.len;
    // Copy only identities. Copying live context values loses an entry XSAVE
    // which acquired its target before this publication and writes afterward.
    @memcpy(new_contexts[0..old_capacity], old.contexts);
    @memcpy(draft.slots[0..old_capacity], old.slots);
    @memcpy(new_hot_threads[0..old_capacity], old.hot_states);
    var i = old_capacity;
    while (i < capacity) : (i += 1) {
        const entity = &new_entities[i - old_capacity];
        entity.reset(i, 1);
        const context = &draft.added_contexts[i - old_capacity];
        context.* = .{ .id = @intCast(i), .scheduler_entity = entity };
        new_contexts[i] = context;
        draft.slots[i] = .{ .group_start = old_capacity };
        new_hot_threads[i] = .{};
    }

    draft.slots[old_capacity].group_count = capacity - old_capacity;
    draft.slots[old_capacity].contexts = draft.added_contexts.ptr;
    draft.slots[old_capacity].entities = draft.entities.ptr;
    draft.directory.* = .{ .contexts = new_contexts, .hot_states = new_hot_threads, .slots = draft.slots };
    // Contexts and runqueue nodes retain stable storage across all growths.
    // Table excludes context/hot loans. The leaf drains capacity readers
    // before swapping the root; never invoke PMM while holding that leaf.
    transaction.commit() catch @panic("invalid thread storage publication");
    lockThreadDirectory();
    @atomicStore(*const ThreadDirectory, &scheduler_state.thread_table.directory, draft.directory, .release);
    unlockThreadDirectory();
    queueReclaimGroupLocked(old_capacity);
    scheduler_state.thread_table.storage.reclaimRetiredAfterBorrowExclusion() catch @panic("moved thread storage owner");
    return true;
}

test "thread storage growth rolls back each partial allocation under table exclusion" {
    const Fixture = struct {
        var bytes: [4 * 1024 * 1024]u8 align(4096) = undefined;
    };
    const capacity = initialThreadCapacity * 2;
    const pages = [_]usize{
        try owned_storage.requiredPages(ThreadDirectory, 1),
        try owned_storage.requiredPages(*ThreadContext, capacity),
        try owned_storage.requiredPages(ThreadContext, capacity - initialThreadCapacity),
        try owned_storage.requiredPages(ThreadHotState, capacity),
        try owned_storage.requiredPages(scheduler_runqueue.Node, capacity - initialThreadCapacity),
        try owned_storage.requiredPages(ThreadSlot, capacity),
    };
    var available: usize = 0;
    defer initializeStaticStorage();
    for (pages) |size| {
        initializeStaticStorage();
        var free_list: kernel.FreePageList = .{};
        try free_list.installRegions(&.{.{ .id = 83, .start = @intFromPtr(&Fixture.bytes), .end = (@intFromPtr(&Fixture.bytes)) + (Fixture.bytes.len / 4096) * 4096 }});
        if (available != 0) try free_list.seedContiguousRange(@intFromPtr(&Fixture.bytes), available);
        try std.testing.expect(!ensureThreadCapacity(initialThreadCapacity + 1, &free_list));
        try std.testing.expectEqual(initialThreadCapacity, threadCapacity());
        try std.testing.expectEqual(initial_context_pointers[0..].ptr, threadDirectory().contexts.ptr);
        try std.testing.expectEqual(available, free_list.pageCount());
        for (0..free_list.rangeCount()) |range_index| {
            const range = free_list.rangeAt(range_index);
            try std.testing.expectEqual(@as(u64, 83), range.region_id);
        }
        try std.testing.expect(scheduler_state.thread_table.storage.pending == null);
        available += size;
    }
}

test "thread storage concurrent growth loser returns all unpublished drafts" {
    const Fixture = struct {
        var bytes: [4 * 1024 * 1024]u8 align(4096) = undefined;
    };
    const old_capacity = initialThreadCapacity;
    const capacity = old_capacity * 2;
    initializeStaticStorage();
    defer {
        // This fixture has no running CPU/borrower; restoring static state is
        // test teardown, NOT evidence that production retirement is reclaimable.
        scheduler_state.thread_table = .{};
        initializeStaticStorage();
    }
    var list: kernel.FreePageList = .{};
    try list.installRegions(&.{.{ .id = 97, .start = @intFromPtr(&Fixture.bytes), .end = (@intFromPtr(&Fixture.bytes)) + (Fixture.bytes.len / 4096) * 4096 }});
    try list.seedContiguousRange(@intFromPtr(&Fixture.bytes), Fixture.bytes.len / 4096);
    const initial_pages = list.pageCount();
    var winner = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, identity_allocator.allocator(&list));
    const first = try ThreadGrowth.reserve(&winner, capacity, old_capacity);
    const winner_pages = initial_pages - list.pageCount();
    var loser = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, identity_allocator.allocator(&list));
    const second = try ThreadGrowth.reserve(&loser, capacity, old_capacity);
    {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        try std.testing.expect(publishThreadGrowthLocked(&winner, first, capacity, old_capacity));
        try std.testing.expect(publishThreadGrowthLocked(&loser, second, capacity, old_capacity));
        try std.testing.expect(winner.finished and !loser.finished);
    }
    // Match the real outer defer: the publication hold has ended, and draft
    // rollback reacquires the table before it may mutate pending returns.
    {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        try loser.rollback();
    }
    try std.testing.expectEqual(initial_pages - winner_pages, list.pageCount());
    try std.testing.expectEqual(first.contexts.ptr, threadDirectory().contexts.ptr);
    try std.testing.expectEqual(@as(?*scheduler_runqueue.Node, &first.entities[0]), threadContext(old_capacity).?.scheduler_entity);
    try std.testing.expectEqual(@as(usize, old_capacity), first.entities[0].thread_index);
    for (0..list.rangeCount()) |range_index| {
        const range = list.rangeAt(range_index);
        try std.testing.expectEqual(@as(u64, 97), range.region_id);
    }
}

test "thread storage growth preserves an outstanding XState save target" {
    const Fixture = struct {
        var bytes: [256 * 4096]u8 align(4096) = undefined;

        fn tearDown() void {
            // Host-only fixture: no CPU, runqueue or borrower survives this
            // scope. Drop every published view BEFORE returning its backing.
            // This is not a production reclaim API or a quiescence proof.
            @atomicStore(*const ThreadDirectory, &scheduler_state.thread_table.directory, &initial_thread_directory, .release);
            const owner = &scheduler_state.thread_table.storage;
            inline for (.{ &owner.published, &owner.retired, &owner.pending }) |head| {
                while (head.*) |record| {
                    // Preserve the original issuer and grant, never derive a
                    // physical address from the payload or touch freed headers.
                    const issuer = record.allocator;
                    const grant = record.allocation;
                    head.* = record.next;
                    std.debug.assert(issuer.release(issuer.context, grant));
                }
            }
            owner.* = .{};
        }
    };
    try std.testing.expect(scheduler_state.thread_table.storage.published == null);
    try std.testing.expect(scheduler_state.thread_table.storage.retired == null);
    try std.testing.expect(scheduler_state.thread_table.storage.pending == null);
    try std.testing.expectEqual(initial_context_pointers[0..].ptr, threadDirectory().contexts.ptr);
    try std.testing.expectEqual(initial_thread_hot_states[0..].ptr, threadDirectory().hot_states.ptr);
    initializeStaticStorage();
    const old_slot = initial_thread_contexts[1];
    defer {
        initial_thread_contexts[1] = old_slot;
        initializeStaticStorage();
    }
    initial_thread_contexts[1].allocated = true;
    initial_thread_contexts[1].pkru = 0x10;
    initial_thread_contexts[1].x_state[160] = 0x21;
    var list: kernel.FreePageList = .{};
    try list.installRegions(&.{.{ .id = 103, .start = @intFromPtr(&Fixture.bytes), .end = (@intFromPtr(&Fixture.bytes)) + (Fixture.bytes.len / 4096) * 4096 }});
    try list.seedContiguousRange(@intFromPtr(&Fixture.bytes), Fixture.bytes.len / 4096);
    const initial_pages = list.pageCount();
    {
        defer Fixture.tearDown();
        const old = threadContextMutable(1).?;
        var transaction = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, identity_allocator.allocator(&list));
        defer if (!transaction.finished) {
            scheduler_state.thread_table.lock();
            defer scheduler_state.thread_table.unlock();
            transaction.rollback() catch @panic("fixture rollback failed");
        };
        const capacity = initialThreadCapacity * 2;
        const draft = try ThreadGrowth.reserve(&transaction, capacity, initialThreadCapacity);
        {
            scheduler_state.thread_table.lock();
            defer scheduler_state.thread_table.unlock();
            try std.testing.expect(publishThreadGrowthLocked(&transaction, draft, capacity, initialThreadCapacity));
        }
        const latest = threadContext(1).?;
        try std.testing.expectEqual(@as(u32, 0x10), latest.pkru);
        try std.testing.expectEqual(@as(u8, 0x21), latest.x_state[160]);
        // Model CPU A obtaining its save target before CPU B publishes growth,
        // then saving afterward. No privileged XSAVE instruction is executed.
        old.pkru = 0x40;
        old.x_state[160] = 0x73;
        try std.testing.expectEqual(@as(u32, 0x40), old.pkru);
        try std.testing.expectEqual(@as(u8, 0x73), old.x_state[160]);
        // Growth must preserve the save target, not merely retain an obsolete
        // copy. No privileged XSAVE is needed to make this interleaving exact.
        try std.testing.expectEqual(old, threadContextMutable(1).?);
        try std.testing.expectEqual(@as(u32, 0x40), threadContext(1).?.pkru);
        try std.testing.expectEqual(@as(u8, 0x73), threadContext(1).?.x_state[160]);
        const heap_context = threadContextMutable(initialThreadCapacity).?;
        heap_context.allocated = true;
        heap_context.ready = true;
        syncHotStateFromContext(initialThreadCapacity);
        var ticket: scheduler_observer.UserEntry = undefined;
        try std.testing.expect(fillUserEntryForThread(0, initialThreadCapacity, heap_context.generation, &ticket));
        for (0..2) |_| {
            const before = threadDirectory();
            const before_capacity = before.contexts.len;
            const before_contexts = try std.testing.allocator.dupe(*ThreadContext, before.contexts);
            defer std.testing.allocator.free(before_contexts);
            var growth = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, identity_allocator.allocator(&list));
            defer if (!growth.finished) {
                scheduler_state.thread_table.lock();
                defer scheduler_state.thread_table.unlock();
                growth.rollback() catch @panic("repeat fixture rollback failed");
            };
            const next = try ThreadGrowth.reserve(&growth, before.contexts.len * 2, before.contexts.len);
            {
                scheduler_state.thread_table.lock();
                defer scheduler_state.thread_table.unlock();
                try std.testing.expect(publishThreadGrowthLocked(&growth, next, before.contexts.len * 2, before.contexts.len));
            }
            const after = threadDirectory();
            try std.testing.expect(before != after);
            try std.testing.expectEqual(before_capacity * 2, after.contexts.len);
            for (before_contexts, after.contexts[0..before_capacity]) |prior, current|
                try std.testing.expectEqual(prior, current);
            heap_context.pkru +%= 1;
            heap_context.x_state[160] +%= 1;
            try std.testing.expectEqual(heap_context.pkru, threadContext(initialThreadCapacity).?.pkru);
            try std.testing.expectEqual(heap_context.x_state[160], threadContext(initialThreadCapacity).?.x_state[160]);
            // A dispatch ticket's image address also survives directory growth.
            // Generation reuse/CPU pin release are separate lifetime work.
            try std.testing.expectEqual(ticket.x_state_addr, @as(u64, @intFromPtr(&threadContext(initialThreadCapacity).?.x_state)));
        }
    }
    try std.testing.expectEqual(initial_pages, list.pageCount());
    try std.testing.expectEqual(@as(usize, 1), list.range_len);
    try std.testing.expectEqual(@as(u64, 103), list.rangeAt(0).region_id);
    try std.testing.expectEqual(@as(u64, @intFromPtr(&Fixture.bytes)), list.rangeAt(0).physical_start);
}

test "thread storage metadata return waits for readers and retries original issuer" {
    const Fixture = struct {
        var bytes: [4 * 1024 * 1024]u8 align(4096) = undefined;
        list: *kernel.FreePageList,
        check_locks: bool = true,
        refuse: bool = false,
        returns: usize = 0,

        fn acquire(raw: *anyopaque, pages: usize) ?owned_storage.MappedExtent {
            const self: *@This() = @ptrCast(@alignCast(raw));
            const backend = identity_allocator.allocator(self.list);
            return backend.acquire(backend.context, pages);
        }
        fn release(raw: *anyopaque, grant: owned_storage.PhysicalExtent) bool {
            const self: *@This() = @ptrCast(@alignCast(raw));
            if (self.check_locks) {
                std.debug.assert(scheduler_state.thread_table.lock_state.value == 1);
                std.debug.assert(scheduler_state.thread_table.directory_lock.value == 0);
            }
            if (self.refuse) return false;
            const backend = identity_allocator.allocator(self.list);
            if (!backend.release(backend.context, grant)) return false;
            // This private fixture has no concurrent PMM consumers. Poison a
            // successful return only: a refused grant's header must survive.
            const mapped = @import("arch/x86_64/physical_layout.zig").identitySlice(u8, grant.physical, grant.pages * 4096).?;
            @memset(mapped[@sizeOf(kernel.RegionFreeRange)..], 0xa5);
            self.returns += 1;
            return true;
        }
        fn allocator(self: *@This()) owned_storage.Allocator {
            return .{ .context = self, .acquire = acquire, .release = release };
        }
        fn writer(transaction: *owned_storage.Transaction, draft: ThreadGrowth, old_capacity: usize, done: ?*u8) void {
            defer if (done) |flag| @atomicStore(u8, flag, 1, .release);
            if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu"))
                @import("root").setTestLockDiagnosticsCpu(1);
            scheduler_state.thread_table.lock();
            defer scheduler_state.thread_table.unlock();
            std.debug.assert(publishThreadGrowthLocked(transaction, draft, draft.contexts.len, old_capacity));
        }
    };
    initializeStaticStorage();
    var list: kernel.FreePageList = .{};
    try list.installRegions(&.{.{ .id = 117, .start = @intFromPtr(&Fixture.bytes), .end = (@intFromPtr(&Fixture.bytes)) + (Fixture.bytes.len / 4096) * 4096 }});
    try list.seedContiguousRange(@intFromPtr(&Fixture.bytes), Fixture.bytes.len / 4096);
    const initial_pages = list.pageCount();
    var issuer: Fixture = .{ .list = &list };
    defer {
        issuer.check_locks = false;
        @atomicStore(*const ThreadDirectory, &scheduler_state.thread_table.directory, &initial_thread_directory, .release);
        const owner = &scheduler_state.thread_table.storage;
        inline for (.{ &owner.published, &owner.retired, &owner.pending }) |head| {
            while (head.*) |record| {
                const backend = record.allocator;
                const grant = record.allocation;
                head.* = record.next;
                std.debug.assert(backend.release(backend.context, grant));
            }
        }
        owner.* = .{};
        std.debug.assert(list.pageCount() == initial_pages);
        initializeStaticStorage();
    }
    var first = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, issuer.allocator());
    defer if (!first.finished) {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        first.rollback() catch @panic("fixture first rollback failed");
    };
    const initial = try ThreadGrowth.reserve(&first, initialThreadCapacity * 2, initialThreadCapacity);
    Fixture.writer(&first, initial, initialThreadCapacity, null);
    if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu"))
        @import("root").setTestLockDiagnosticsCpu(0);
    const context = initial.contexts[initialThreadCapacity];
    const node = context.scheduler_entity;
    context.pkru = 37;
    var second = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, issuer.allocator());
    defer if (!second.finished) {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        second.rollback() catch @panic("fixture second rollback failed");
    };
    const grown = try ThreadGrowth.reserve(&second, initial.contexts.len * 2, initial.contexts.len);
    issuer.refuse = true;
    defer issuer.refuse = false;
    var contended: u8 = 0;
    var writer_done: u8 = 0;
    DirectoryTestProbe.contended = &contended;
    defer DirectoryTestProbe.contended = null;
    lockThreadDirectory();
    const reader = threadDirectory();
    const writer = std.Thread.spawn(.{}, Fixture.writer, .{ &second, grown, initial.contexts.len, &writer_done }) catch |err| {
        unlockThreadDirectory();
        return err;
    };
    // Writer owns table but cannot publish or return while this reader owns
    // the leaf. No timeout/sleep is used as evidence that exclusion worked.
    while (@atomicLoad(u8, &contended, .acquire) == 0 and @atomicLoad(u8, &writer_done, .acquire) == 0) std.atomic.spinLoopHint();
    const held_root = threadDirectory();
    const held_returns = issuer.returns;
    unlockThreadDirectory();
    writer.join();
    try std.testing.expectEqual(@as(u8, 1), @atomicLoad(u8, &contended, .acquire));
    try std.testing.expectEqual(reader, held_root);
    try std.testing.expectEqual(@as(usize, 0), held_returns);
    try std.testing.expect(scheduler_state.thread_table.storage.retired != null);
    const current_capacity = threadCapacity();
    var other: kernel.FreePageList = .{};
    try std.testing.expect(ensureThreadCapacity(current_capacity, &other));
    try std.testing.expect(scheduler_state.thread_table.storage.retired != null);
    issuer.refuse = false;
    try std.testing.expect(ensureThreadCapacity(current_capacity, &other));
    try std.testing.expect(scheduler_state.thread_table.storage.retired == null);
    try std.testing.expectEqual(@as(usize, 4), issuer.returns);
    try std.testing.expectEqual(@as(usize, 0), other.pageCount());
    for (0..list.rangeCount()) |range_index| {
        const range = list.rangeAt(range_index);
        try std.testing.expectEqual(@as(u64, 117), range.region_id);
    }
    try std.testing.expectEqual(context, threadContext(initialThreadCapacity).?);
    try std.testing.expectEqual(@as(u32, 37), context.pkru);
    try std.testing.expectEqual(node, context.scheduler_entity);
    try std.testing.expect(ensureThreadCapacity(current_capacity, &other));
    try std.testing.expectEqual(@as(usize, 4), issuer.returns);
}

test "thread storage PMM DMA retention preserves grant until drain permits retry" {
    const Fixture = struct {
        var bytes: [4 * 4096]u8 align(4096) = undefined;
    };
    var owner: owned_storage.Owner = .{};
    var list: kernel.FreePageList = .{};
    try list.installRegions(&.{.{ .id = 61, .start = @intFromPtr(&Fixture.bytes), .end = (@intFromPtr(&Fixture.bytes)) + (4) * 4096 }});
    try list.seedContiguousRange(@intFromPtr(&Fixture.bytes), 4);
    var transaction = owned_storage.Transaction.init(&owner, identity_allocator.allocator(&list));
    _ = try transaction.reserve(u8, 5000, .replaceable);
    var hold = try @import("memory/dma_retention.zig").TestHold.init(@intFromPtr(&Fixture.bytes), 2);
    defer hold.end();
    try transaction.rollback();
    try std.testing.expect(owner.pending != null);
    try owner.retryUnpublished();
    try std.testing.expect(owner.pending != null);
    hold.end();
    try owner.retryUnpublished();
    try std.testing.expect(owner.pending == null);
    const returned = list.rangeAt(0);
    try std.testing.expectEqual(@as(u64, 61), returned.region_id);
    try std.testing.expectEqual(@as(u64, @intFromPtr(&Fixture.bytes)), returned.physical_start);
    try std.testing.expectEqual(@as(usize, 4), returned.len);
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
    end = maxStaticEnd(end, @intFromPtr(&initial_context_pointers) + @sizeOf(@TypeOf(initial_context_pointers)));
    end = maxStaticEnd(end, @intFromPtr(&initial_thread_directory) + @sizeOf(ThreadDirectory));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_thread_hot_states), &initial_thread_hot_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_scheduler_entities), &initial_scheduler_entities));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_state), &scheduler_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(bootstrap_cpu_states), &bootstrap_cpu_states));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(bootstrap_base_shadow), &bootstrap_base_shadow));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(lapic_tick_count), &lapic_tick_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_tick_accum), &scheduler_tick_accum));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(scheduler_switch_count), &scheduler_switch_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_x_state), &initial_x_state));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(base_shadow), &base_shadow));
    if (comptime lock_diagnostics.enabled) end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_lifecycle_record), &principal_lifecycle_record));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_lifecycle_gate), &principal_lifecycle_gate));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_lifecycle_pending_action), &principal_lifecycle_pending_action));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_lifecycle_pending_code), &principal_lifecycle_pending_code));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_self_exit_reservations), &principal_self_exit_reservations));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(initial_thread_slots), &initial_thread_slots));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(absent_thread_context), &absent_thread_context));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_self_exit_extra), &principal_self_exit_extra));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(principal_self_exit_leaf), &principal_self_exit_leaf));
    return end;
}

pub fn currentThread() usize {
    const state = schedulerStateForSlot(currentCpu()) orelse return 0;
    return state.current_thread;
}

/// Scalar ABI is mandatory here: an optional aggregate result can use XMM0
/// before XSAVE. A CPU pin supplies stable identity without a directory
/// borrow. It is not permission to free other outstanding context borrowers.
pub fn notifyDiagnosticUserReturn() void {
    if (comptime !ipc_metric.enabled) return;
    const ctx = pinnedContextForCpu(currentCpu()) orelse return;
    @import("notify_metric.zig").userReturn(&ctx.notify_stamp.stamp, ctx.generation);
}

pub noinline fn currentContextForXState() ?*ThreadContext {
    if (interruptsEnabled()) {
        while (true) asm volatile ("cli; hlt");
    }
    const cpu = if (builtin.is_test) bootstrap_cpu_slot else smp.currentCpuSlotOrInvalid();
    return pinnedContextForCpu(cpu);
}

inline fn pinnedContextForCpu(cpu: usize) ?*ThreadContext {
    const state = schedulerStateForSlot(cpu) orelse return null;
    const ctx = state.current_context orelse return null;
    // Local exit can reset the object while retaining the pin until the
    // next idle/switch publication. Such a pin blocks reuse, not XState save.
    // Conversely remote stop only changes ready: its last save is required.
    if (!ctx.allocated or ctx.generation != state.current_generation) return null;
    return ctx;
}

fn snapshotUserEntryForCpu(cpu: usize, if_enabled: bool, entry: *UserVirtualAddress, stack: *UserVirtualAddress) bool {
    if (if_enabled) return false;
    const ctx = pinnedContextForCpu(cpu) orelse return false;
    // Copy only the needed values while this CPU cannot switch away. No
    // Context pointer survives the call and failed admission changes no out.
    entry.* = .fromInt(ctx.frame.rip);
    stack.* = .fromInt(ctx.frame.rsp);
    return true;
}

pub fn currentUserEntrySnapshot(entry: *UserVirtualAddress, stack: *UserVirtualAddress) bool {
    return snapshotUserEntryForCpu(smp.currentCpuSlotOrInvalid(), interruptsEnabled(), entry, stack);
}

fn snapshotFsBaseForCpu(cpu: usize, if_enabled: bool, out: *UserVirtualAddress) bool {
    if (if_enabled) return false;
    const ctx = pinnedContextForCpu(cpu) orelse return false;
    out.* = .fromInt(ctx.fs_base);
    return true;
}

/// Fault diagnostics can interrupt an ownership transition or lock holder.
/// They may omit unavailable state, but must neither lock nor halt on a bad
/// pin. This is the current pin's value, not a coherent full trap snapshot.
pub fn currentFsBaseSnapshot(out: *UserVirtualAddress) bool {
    return snapshotFsBaseForCpu(smp.currentCpuSlotOrInvalid(), interruptsEnabled(), out);
}

pub fn currentPrincipal() kernel.PrincipalId {
    const cpu = currentCpu();
    const state = schedulerStateForSlot(cpu) orelse return default_process_principal;
    // Fault diagnostics may interrupt a table/directory lock holder. The
    // local CPU pin already excludes reuse; do not reacquire either lock.
    if (pinnedContextForCpu(cpu)) |ctx| return ctx.owner_process;
    return state.current_principal orelse default_process_principal;
}

pub fn currentCr3() u64 {
    const cpu = currentCpu();
    const state = schedulerStateForSlot(cpu) orelse return 0;
    if (pinnedContextForCpu(cpu)) |ctx| {
        if (ctx.cr3 != 0) return ctx.cr3;
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
        .word = if (process_index < 64) &principal_self_exit_reservations[0] else blk: {
            const flags = principal_self_exit_leaf.lock();
            defer principal_self_exit_leaf.unlock(flags);
            // The word loan outlives the leaf: no published word is removed.
            break :blk principal_self_exit_extra.get(@intCast(process_index / 64 - 1)) orelse return null;
        },
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
    if (target_raw >= kernel.process_principal_value_count) return;
    const reservation = principalSelfExitReservation(@enumFromInt(@as(kernel.PrincipalRaw, @intCast(target_raw)))) orelse return;
    _ = @atomicRmw(u64, reservation.word, .And, ~reservation.mask, .acq_rel);
}

fn tryBeginPrincipalLifecycleGate(target: kernel.PrincipalId) bool {
    // Preserve the off-build IR shape, including CAS condition lowering.
    if (comptime !lock_diagnostics.enabled) {
        if (@cmpxchgStrong(usize, &principal_lifecycle_gate, 0, 1, .acquire, .monotonic) != null) {
            return false;
        }
        principal_lifecycle_pending_code = 0;
        @atomicStore(u8, &principal_lifecycle_pending_action, @intFromEnum(PrincipalLifecycleAction.none), .monotonic);
        @atomicStore(usize, &principal_lifecycle_gate, lifecycleWord(target), .release);
        return true;
    }

    // K -> L is a nonwaiting exception; a success still constrains later locks.
    const diagnostic_if = lock_diagnostics.maskInterrupts();
    defer lock_diagnostics.restoreInterrupts(diagnostic_if);
    if (@cmpxchgStrong(usize, &principal_lifecycle_gate, 0, 1, .acquire, .monotonic) != null) {
        return false;
    }
    lock_diagnostics.acquiredTry(&principal_lifecycle_record, .{ .class = .principal_lifecycle });
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
    const diagnostic_if = lock_diagnostics.maskInterrupts();
    defer lock_diagnostics.restoreInterrupts(diagnostic_if);
    lock_diagnostics.assertOwned(&principal_lifecycle_record);
    @atomicStore(u8, &principal_lifecycle_pending_action, @intFromEnum(PrincipalLifecycleAction.none), .monotonic);
    principal_lifecycle_pending_code = 0;
    const word = @atomicLoad(usize, &principal_lifecycle_gate, .acquire);
    if ((word & 3) == 2) clearPrincipalSelfExitReservation(word >> 2);
    lock_diagnostics.releasing(&principal_lifecycle_record);
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
    const ctx = threadContextLocked(state.current_thread) orelse return .slow;
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

fn cpuRunsPrincipal(cpu_slot: usize, principal: kernel.PrincipalId) bool {
    const state = &scheduler_state.cpus[cpu_slot];
    state.lock.lock();
    defer state.lock.unlock();
    return state.enabled and !state.is_idle and state.current_principal == principal;
}

pub fn principalRunningExcept(principal: kernel.PrincipalId, ignored: ?usize) bool {
    if (!@import("memory/user_vm.zig").ipcAddressSpacePinsEnded(principal)) return true;
    for (0..scheduler_state.cpus.len) |cpu| {
        if (ignored == cpu) continue;
        if (cpuRunsPrincipal(cpu, principal)) return true;
    }
    return false;
}

pub fn principalSlotReusable(principal: kernel.PrincipalId) bool {
    return !principalRunningExcept(principal, null);
}

/// K excludes creation while the reaper checks T and every CPU pin. Include
/// the local CPU: self exit still owns its old CR3 until the switch completes.
pub fn addressSpaceUsersEnded(principal: kernel.PrincipalId) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    var index: usize = 0;
    while (index < threadDirectory().contexts.len) : (index += 1) {
        const ctx = threadContextLocked(index) orelse continue;
        if (ctx.allocated and ctx.owner_process == principal) return false;
    }
    // enabled/is_idle are scheduling flags, not withdrawal of an entry/return
    // pin. Even an offline or idle-marked CPU must explicitly clear ownership.
    for (scheduler_state.cpus) |*cpu| {
        cpu.lock.lock();
        const pinned = cpu.current_principal == principal or
            (if (cpu.current_context) |ctx| ctx.owner_process == principal else false);
        cpu.lock.unlock();
        if (pinned) return false;
    }
    return true;
}

/// Actively drive every CPU executing `principal` through the scheduler IPI
/// safe point.  There is intentionally no elapsed-time escape: teardown is
/// permitted only after the ownership snapshot reaches zero.
fn waitForPrincipalQuiescenceIgnoring(principal: kernel.PrincipalId, ignored: ?usize) void {
    lock_diagnostics.assertCanWaitForIpi();
    while (true) {
        var pending = false;
        for (0..scheduler_state.cpus.len) |cpu| {
            if (ignored == cpu or !cpuRunsPrincipal(cpu, principal)) continue;
            pending = true;
            // No CPU state lock survives the interrupt send or IRQ window.
            _ = smp.interruptCpu(cpu);
        }
        if (!pending and @import("memory/user_vm.zig").ipcAddressSpacePinsEnded(principal)) return;
        asm volatile ("sti; pause; cli" ::: .{ .memory = true });
    }
}

pub fn waitForPrincipalQuiescence(principal: kernel.PrincipalId) void {
    waitForPrincipalQuiescenceIgnoring(principal, null);
}

pub fn waitForRemotePrincipalQuiescence(principal: kernel.PrincipalId) void {
    waitForPrincipalQuiescenceIgnoring(principal, currentCpu());
}

fn buildInitialCpuSchedulerStates() [bootstrap_cpu_rows]CpuSchedulerState {
    var states: [bootstrap_cpu_rows]CpuSchedulerState = @splat(.{});
    states[bootstrap_cpu_slot].enabled = true;
    states[bootstrap_cpu_slot].current_thread = 0;
    states[bootstrap_cpu_slot].current_context = null;
    states[bootstrap_cpu_slot].current_generation = 0;
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
    const ctx = threadContextLocked(thread_index) orelse return false;
    var referenced = false;
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    for (scheduler_state.cpus) |*state| {
        if (state.current_context == ctx) {
            referenced = true;
            break;
        }
    }
    return referenced;
}

fn threadSlotReferencedByRemoteCpu(thread_index: usize, local_cpu: usize) bool {
    const ctx = threadContextLocked(thread_index) orelse return false;
    var referenced = false;
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    for (scheduler_state.cpus, 0..) |*state, cpu_slot| {
        if (cpu_slot == local_cpu) continue;
        if (state.current_context == ctx) {
            referenced = true;
            break;
        }
    }
    return referenced;
}

fn threadAssignedCpuSlot(thread_index: usize) usize {
    const ctx = threadContextLocked(thread_index) orelse return bootstrap_cpu_slot;
    if (!ctx.allocated) return bootstrap_cpu_slot;
    if (ctx.cpu_slot >= scheduler_state.cpus.len) return bootstrap_cpu_slot;
    return ctx.cpu_slot;
}

fn hasAllocatedThreadLocked() bool {
    const directory = threadDirectory();
    var i: usize = 0;
    while (i < directory.contexts.len) : (i += 1) {
        if (directory.contexts[i].allocated) return true;
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

fn bootTopologyRefreshAllowed(cpu: ?usize, dispatch_enabled: u8) bool {
    return cpu != null and cpu.? == bootstrap_cpu_slot and dispatch_enabled == 0;
}

pub fn refreshTopology() void {
    // This rebuild discards AP current ownership; it is not a CPU-hotplug
    // operation. Only the known BSP may perform it before user dispatch is
    // published. Unknown identity must not inherit the BSP fallback. A bad
    // runtime caller could hold locks, so fail without logging/GOP reentry.
    if (!bootTopologyRefreshAllowed(smp.currentCpuSlotIfKnown(), @atomicLoad(u8, &scheduler_state.ap_user_dispatch_enabled, .acquire))) {
        while (true) asm volatile ("cli; hlt");
    }
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const count = smp.cpuCount();
    // Runtime rows were sized from this same boot topology. A mismatch is
    // initialization corruption, never permission to silently omit CPUs.
    if (count > scheduler_state.cpus.len) {
        while (true) asm volatile ("cli; hlt");
    }
    @atomicStore(usize, &scheduler_state.cpu_count, count, .release);
    var i: usize = 0;
    while (i < scheduler_state.cpus.len) : (i += 1) {
        const observed = i < count and smp.cpuState(i) != .absent;
        const state = &scheduler_state.cpus[i];
        state.enabled = observed;
        if (!observed or i != bootstrap_cpu_slot) {
            // Boot-only admission above excludes live AP entry/return pins.
            state.current_context = null;
            state.current_generation = 0;
            state.current_thread = state.idle_thread;
            state.current_principal = null;
            state.current_cr3 = 0;
            state.is_idle = true;
        }
    }
}

test "thread storage topology reset requires known BSP before dispatch" {
    for ([_]?usize{ null, bootstrap_cpu_slot, 1 }) |cpu| {
        for ([_]u8{ 0, 1, 255 }) |enabled| {
            const expected = cpu != null and cpu.? == bootstrap_cpu_slot and enabled == 0;
            try std.testing.expectEqual(expected, bootTopologyRefreshAllowed(cpu, enabled));
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
    endBaseShadow(cpu_slot);
    const state = schedulerStateForSlot(cpu_slot) orelse return;
    state.lock.lock();
    defer state.lock.unlock();
    clearCurrentContextLocked(state);
}

pub fn recordApUserTimerFrame(frame: *const TrapFrame) bool {
    _ = frame;
    return false;
}

pub fn apUserThreadCanContinue() bool {
    const cpu_slot = currentCpu();
    if (cpu_slot == bootstrap_cpu_slot) return true;
    const thread_index = currentThread();
    // A remote STOP and table growth must not change the context/hot pair
    // halfway through this timer-boundary decision.
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    if (thread_index >= threadDirectory().contexts.len) return false;
    const ctx = threadContextLocked(thread_index) orelse return false;
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
    return quiesceStoppedCurrentUserThreadWithRegisters(x86_platform, frame);
}

fn quiesceStoppedCurrentUserThreadWithRegisters(comptime Registers: type, frame: *const TrapFrame) bool {
    const cpu_slot = currentCpu();
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    scheduler_state.thread_table.lock();
    state.lock.lock();
    const thread_index = state.current_thread;
    const ctx = threadContextLocked(thread_index) orelse {
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
    ctx.entry_state.notify_sysret_continuation = false;
    if (state.current_cr3 != 0) ctx.cr3 = state.current_cr3;
    const live = captureLeavingBases(Registers, currentCpu());
    ctx.fs_base = live.fs;
    ctx.gs_base = live.gs;
    ctx.pkru = live.pkru;
    ctx.ready = false;
    setThreadHotCr3(thread_index, ctx.cr3);
    setThreadHotReady(thread_index, false);
    clearCurrentContextLocked(state);
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
    return user_spaces.borrow(idx);
}

fn pcidForPrincipal(principal: kernel.PrincipalId) u16 {
    const idx = kernel.processIndexFromPrincipal(principal) orelse return 0;
    return x86_platform.userPcidForProcessIndex(idx);
}

pub fn threadContextMutable(thread_index: usize) ?*ThreadContext {
    // Production borrowers use table-held helpers or the current CPU pin.
    // Only isolated host fixtures may keep this unscoped context reference.
    if (!builtin.is_test) @compileError("unscoped thread context is test-only");
    lockThreadDirectory();
    defer unlockThreadDirectory();
    return threadContextLocked(thread_index);
}

pub fn threadContext(thread_index: usize) ?*const ThreadContext {
    return threadContextMutable(thread_index);
}

fn getThreadHotState(thread_index: usize) ?*ThreadHotState {
    const directory = threadDirectory();
    if (thread_index >= directory.hot_states.len) return null;
    return &directory.hot_states[thread_index];
}

fn getThreadHotStateConst(thread_index: usize) ?*const ThreadHotState {
    const directory = threadDirectory();
    if (thread_index >= directory.hot_states.len) return null;
    return &directory.hot_states[thread_index];
}

fn boolByte(value: bool) u8 {
    return if (value) 1 else 0;
}

// T owns the index together with wait admission/claim/publication. Nodes
// live in stable bodies, not the replaceable hot/directory arrays. Removing
// before mutating the key prevents cancellation from targeting a newer wait.
fn setWakeTickLocked(ctx: *ThreadContext, tick: u64) void {
    const tree = &scheduler_state.thread_table.deadlines;
    if (ctx.deadline_node.linked) tree.remove(&ctx.deadline_node);
    ctx.wake_tick = tick;
    if (tick != 0) {
        std.debug.assert(ctx.allocated);
        tree.insert(&ctx.deadline_node, .{ .deadline = tick, .id = ctx.id, .generation = ctx.generation });
    }
    if (getThreadHotState(ctx.id)) |hot| hot.wake_tick = tick;
}

// Readiness can change without changing the retained wait (STOP/CONT and
// explicit suspend/resume). Index only blocked waits, as the old scan did.
fn reconcileDeadlineLocked(ctx: *ThreadContext) void {
    const tree = &scheduler_state.thread_table.deadlines;
    const eligible = ctx.allocated and !ctx.ready and ctx.wake_tick != 0;
    if (!eligible and ctx.deadline_node.linked) tree.remove(&ctx.deadline_node);
    if (eligible and !ctx.deadline_node.linked)
        tree.insert(&ctx.deadline_node, .{ .deadline = ctx.wake_tick, .id = ctx.id, .generation = ctx.generation });
}

fn updateCompletionRetryLocked(ctx: *ThreadContext) void {
    const table = &scheduler_state.thread_table;
    const needed = ctx.allocated and !ctx.ready and ctx.wait_completion_claimed and
        ctx.wait_completion_publish_pending and ctx.active_wait_token != 0;
    if (needed and ctx.retry_queue == null) {
        ctx.retry_queue = table.retry_enqueue;
        table.retries[table.retry_enqueue].append(&ctx.retry_link);
    } else if (!needed) {
        removeCompletionRetryLocked(ctx);
    }
}

fn removeCompletionRetryLocked(ctx: *ThreadContext) void {
    if (ctx.retry_queue) |q| scheduler_state.thread_table.retries[q].remove(&ctx.retry_link);
    ctx.retry_queue = null;
    ctx.retry_link = .{};
}

// Group links use slot indices: directory relocation may move every slot.
// Only empty groups are candidates; CPU pins may keep such a group queued.
fn queueReclaimGroupLocked(start: usize) void {
    if (start < initialThreadCapacity) return;
    const slots = threadDirectory().slots;
    const group = &slots[start];
    if (group.reclaim_queued) return;
    group.reclaim_next = if (scheduler_state.thread_table.reclaim_head) |head| @intCast(head) else no_reclaim_group;
    group.reclaim_previous = no_reclaim_group;
    if (group.reclaim_next != no_reclaim_group) slots[group.reclaim_next].reclaim_previous = @intCast(start);
    scheduler_state.thread_table.reclaim_head = start;
    group.reclaim_queued = true;
}

fn unqueueReclaimGroupLocked(start: usize) void {
    const slots = threadDirectory().slots;
    const group = &slots[start];
    if (!group.reclaim_queued) return;
    if (group.reclaim_previous != no_reclaim_group) slots[group.reclaim_previous].reclaim_next = group.reclaim_next else scheduler_state.thread_table.reclaim_head = if (group.reclaim_next == no_reclaim_group) null else group.reclaim_next;
    if (group.reclaim_next != no_reclaim_group) slots[group.reclaim_next].reclaim_previous = group.reclaim_previous;
    group.reclaim_next = no_reclaim_group;
    group.reclaim_previous = no_reclaim_group;
    group.reclaim_queued = false;
}

fn updateGroupLiveLocked(index: usize, before: bool, after: bool) void {
    if (index < initialThreadCapacity or before == after) return;
    const start = threadDirectory().slots[index].group_start;
    const group = &threadDirectory().slots[start];
    if (after) {
        group.live += 1;
        group.retiring = false;
        unqueueReclaimGroupLocked(start);
    } else {
        std.debug.assert(group.live > 0);
        group.live -= 1;
        if (group.live == 0) queueReclaimGroupLocked(start);
    }
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
    const ctx = threadContextLocked(thread_index) orelse return;
    const hot = getThreadHotState(thread_index) orelse return;
    updateGroupLiveLocked(thread_index, hot.allocated != 0, ctx.allocated);
    // Struct-literal host fixtures enter through this same publication point.
    reconcileDeadlineLocked(ctx);
    updateCompletionRetryLocked(ctx);
    hot.* = hotStateFromContext(ctx);
}

fn setThreadHotReady(thread_index: usize, ready: bool) void {
    if (threadContextLocked(thread_index)) |ctx| {
        reconcileDeadlineLocked(ctx);
        updateCompletionRetryLocked(ctx);
    }
    if (getThreadHotState(thread_index)) |hot| hot.ready = boolByte(ready);
}

fn setThreadHotPendingSignalMask(thread_index: usize, signal_mask: u64) void {
    if (getThreadHotState(thread_index)) |hot| hot.pending_signal_mask = signal_mask;
}

fn setThreadHotStopped(thread_index: usize, stopped: bool) void {
    if (getThreadHotState(thread_index)) |hot| hot.stopped = boolByte(stopped);
}

fn setThreadHotWaitState(thread_index: usize, wait_mailbox: bool, wake_tick: u64, ready: bool) void {
    if (threadContextLocked(thread_index)) |ctx| {
        reconcileDeadlineLocked(ctx);
        updateCompletionRetryLocked(ctx);
    }
    if (getThreadHotState(thread_index)) |hot| {
        hot.wait_mailbox = boolByte(wait_mailbox);
        hot.wake_tick = wake_tick;
        hot.ready = boolByte(ready);
    }
}

// T owns all hot-state readers and retry/deadline links. Direct admission
// requires both queues empty and wake_tick=0; publish the final wait fields
// once instead of looking up the body and reconciling empty queues twice.
fn publishDirectWaitStateLocked(ctx: *ThreadContext) void {
    if (builtin.is_test or lock_diagnostics.enabled) {
        std.debug.assert(ctx.wake_tick == 0 and !ctx.deadline_node.linked and ctx.retry_queue == null);
    }
    const hot = getThreadHotState(ctx.id) orelse unreachable;
    hot.wait_mailbox = boolByte(ctx.wait_mailbox);
    hot.wake_tick = 0;
    hot.ready = boolByte(ctx.ready);
    hot.wait_completion_claimed = boolByte(ctx.wait_completion_claimed);
}

fn setThreadHotCompletionClaimed(thread_index: usize, claimed: bool) void {
    if (threadContextLocked(thread_index)) |ctx| updateCompletionRetryLocked(ctx);
    if (getThreadHotState(thread_index)) |hot|
        hot.wait_completion_claimed = boolByte(claimed);
}

fn setThreadHotCr3(thread_index: usize, cr3: u64) void {
    if (getThreadHotState(thread_index)) |hot| hot.cr3 = cr3;
}

pub fn threadReady(thread_index: usize) bool {
    if (!builtin.is_test) @compileError("index-only thread operation is host-fixture-only");
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
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
    while (true) : (i += 1) {
        // Logging may reenter unrelated locks. Copy only displayed scalar
        // fields while protected, then drop the table before formatting.
        const row = snapshot: {
            scheduler_state.thread_table.lock();
            defer scheduler_state.thread_table.unlock();
            if (i >= threadDirectory().contexts.len) break;
            const ctx = threadContextLocked(i) orelse continue;
            if (!ctx.allocated) continue;
            const hot = getThreadHotStateConst(i) orelse continue;
            break :snapshot .{
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
            };
        };
        kernel_log.writeFmt(
            "[sched-debug] thread={} gen={} owner={} cpu={} ready={} wait={} stopped={} wake={} hot_ready={} hot_wait={} hot_stopped={} hot_wake={}\n",
            row,
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
    defer user_spaces.endBorrow(space);
    // Caller owns the AS via KernelState or unpublished boot construction;
    // a directory lookup alone does not pin the page tables.
    const user_cr3 = x86_platform.cr3WithUserPcid(space.cr3, pcidForPrincipal(owner_process));
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return initializeThreadContextLocked(thread_index, owner_process, user_cr3, initial_frame, initial_ready);
}

fn initializeThreadContextLocked(
    thread_index: usize,
    owner_process: kernel.PrincipalId,
    user_cr3: u64,
    initial_frame: TrapFrame,
    initial_ready: bool,
) bool {
    // Every user context can reach GETPID; device principals must never be
    // admitted even through the unpublished bootstrap initializer.
    if (kernel.processIndexFromPrincipal(owner_process) == null) return false;
    const ctx = threadContextLocked(thread_index) orelse return false;
    const slot = &threadDirectory().slots[thread_index];
    if (ctx == &absent_thread_context or ctx.allocated or slot.generation >= max_thread_generation) return false;
    // The public initializer must enforce admission too: callers cannot
    // bypass allocateThread's scan while a retiring CPU still owns this slot.
    if (threadSlotReferencedByCpu(thread_index)) return false;
    ctx.id = @intCast(thread_index);
    ctx.generation = slot.generation;
    ctx.allocated = true;
    ctx.owner_process = owner_process;
    ctx.cpu_slot = selectCpuForNewThreadLocked();
    ctx.cpu_affinity = null;
    ctx.cr3 = user_cr3;
    ctx.fs_base = 0;
    ctx.gs_base = 0;
    ctx.pkru = 0;
    ctx.ready = initial_ready;
    ctx.wait_uninterruptible = false;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    ctx.next_wait_token = 1;
    ctx.wait_completion_claimed = false;
    ctx.wait_completion_publish_pending = false;
    ctx.wait_completion_rax = 0;
    ctx.stopped = false;
    ctx.resume_after_stop = false;
    ctx.pending_signal_mask = 0;
    ctx.signal_entry = 0;
    ctx.signal_scratch_base = 0;
    ctx.signal_scratch_size = 0;
    ctx.signal_delivery_ready = false;
    ctx.signal_blocked_mask = 0;
    ctx.signal_inhibit_start = 0;
    ctx.signal_inhibit_end = 0;
    ctx.signal_inhibit_secondary_start = 0;
    ctx.signal_inhibit_secondary_end = 0;
    // A newly created thread inherits the runtime signal trampoline from a
    // sibling but is never the process signal owner; only an explicit REGISTER
    // (the main/event-loop thread) claims ownership.
    ctx.signal_owner = false;
    ctx.signal_pending_hint_va = 0;
    ctx.fault_entry = 0;
    ctx.fault_stack_base = 0;
    ctx.fault_stack_size = 0;
    ctx.active_fault_frame = 0;
    var inherit_index: usize = 0;
    while (inherit_index < threadDirectory().contexts.len) : (inherit_index += 1) {
        if (inherit_index == thread_index) continue;
        const source = threadContextLocked(inherit_index) orelse continue;
        if (!source.allocated or source.owner_process != owner_process or source.signal_entry == 0) continue;
        ctx.signal_entry = source.signal_entry;
        // A scratch belongs to one native thread. Defer delivery until the
        // child registers its own region; never fall back to its caller stack.
        ctx.signal_delivery_ready = source.signal_delivery_ready and source.signal_scratch_size == 0;
        ctx.signal_blocked_mask = source.signal_blocked_mask;
        ctx.signal_inhibit_start = source.signal_inhibit_start;
        ctx.signal_inhibit_end = source.signal_inhibit_end;
        ctx.signal_inhibit_secondary_start = source.signal_inhibit_secondary_start;
        ctx.signal_inhibit_secondary_end = source.signal_inhibit_secondary_end;
        break;
    }
    setWakeTickLocked(ctx, 0);
    ctx.frame = initial_frame;
    ctx.entry_state.notify_sysret_continuation = false;
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
    if (!builtin.is_test) @compileError("index-only thread operation is host-fixture-only");
    return initializeThreadContextWithReadyState(thread_index, owner_process, user_spaces, initial_frame, true);
}

fn setBootstrapThreadEntryForCpu(cpu: ?usize, thread_index: usize, owner: kernel.PrincipalId, entry: UserVirtualAddress, stack: UserVirtualAddress) bool {
    if (kernel_runtime.kernel_state_ready or !bootTopologyRefreshAllowed(cpu, @atomicLoad(u8, &scheduler_state.ap_user_dispatch_enabled, .acquire))) return false;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(thread_index) orelse return false;
    if (!ctx.allocated or ctx.owner_process != owner or threadSlotReferencedByCpu(thread_index)) return false;
    // The ELF/stack builders validated these user values before the call.
    // Keep their potentially allocating work outside table exclusion, and
    // publish both scalars only after all boot/ownership admission succeeds.
    ctx.frame.rip = entry.toInt();
    ctx.frame.rsp = stack.toInt();
    return true;
}

pub fn setBootstrapThreadEntry(thread_index: usize, owner: kernel.PrincipalId, entry: UserVirtualAddress, stack: UserVirtualAddress) bool {
    return setBootstrapThreadEntryForCpu(smp.currentCpuSlotIfKnown(), thread_index, owner, entry, stack);
}

test "thread storage bootstrap entry admission leaves rejected context unchanged" {
    const tid = 4;
    const ctx = &initial_thread_contexts[tid];
    const saved = ctx.*;
    const ready = kernel_runtime.kernel_state_ready;
    const dispatch = @atomicLoad(u8, &scheduler_state.ap_user_dispatch_enabled, .acquire);
    defer {
        ctx.* = saved;
        kernel_runtime.kernel_state_ready = ready;
        @atomicStore(u8, &scheduler_state.ap_user_dispatch_enabled, dispatch, .release);
        initializeStaticStorage();
    }
    for (0..9) |scenario| {
        initializeStaticStorage();
        kernel_runtime.kernel_state_ready = false;
        @atomicStore(u8, &scheduler_state.ap_user_dispatch_enabled, 0, .release);
        ctx.* = .{ .id = tid, .allocated = true, .generation = 17 };
        ctx.frame.rip = 0x1111;
        ctx.frame.rsp = 0x2222;
        var cpu: ?usize = 0;
        var index: usize = tid;
        var owner = default_process_principal;
        switch (scenario) {
            0 => {},
            1 => cpu = 1,
            2 => cpu = null,
            3 => kernel_runtime.kernel_state_ready = true,
            4 => @atomicStore(u8, &scheduler_state.ap_user_dispatch_enabled, 1, .release),
            5 => ctx.allocated = false,
            6 => owner = kernel.processPrincipalFromIndex(1).?,
            7 => installCurrentContextLocked(&scheduler_state.cpus[scheduler_state.cpus.len - 1], tid, ctx),
            8 => index = idleThreadMarker,
            else => unreachable,
        }
        const before = ctx.*;
        const ok = setBootstrapThreadEntryForCpu(cpu, index, owner, .fromInt(0x4000), .fromInt(0x8000));
        try std.testing.expectEqual(scenario == 0, ok);
        if (ok) {
            try std.testing.expectEqual(@as(u64, 0x4000), ctx.frame.rip);
            try std.testing.expectEqual(@as(u64, 0x8000), ctx.frame.rsp);
        } else try std.testing.expectEqualDeep(before, ctx.*);
    }
}

test "thread storage scalar snapshots keep failed outputs and reject invalid pins" {
    const tid = 4;
    const ctx = &initial_thread_contexts[tid];
    const saved = ctx.*;
    defer {
        ctx.* = saved;
        initializeStaticStorage();
    }
    for (0..7) |scenario| {
        initializeStaticStorage();
        ctx.* = .{ .id = tid, .allocated = true, .generation = 17, .fs_base = 0x6000 };
        ctx.frame.rip = 0x4000;
        ctx.frame.rsp = 0x8000;
        installCurrentContextLocked(&scheduler_state.cpus[0], tid, ctx);
        var cpu: usize = 0;
        var if_enabled = false;
        switch (scenario) {
            0 => {}, // ready=false is valid: STOP has not withdrawn the pin.
            1 => if_enabled = true,
            2 => cpu = scheduler_state.cpus.len,
            3 => ctx.allocated = false,
            4 => ctx.generation += 1,
            5 => clearCurrentContextLocked(&scheduler_state.cpus[0]),
            6 => cpu = std.math.maxInt(usize),
            else => unreachable,
        }
        var entry = UserVirtualAddress.fromInt(11);
        var stack = UserVirtualAddress.fromInt(22);
        var fs = UserVirtualAddress.fromInt(33);
        try std.testing.expectEqual(scenario == 0, snapshotUserEntryForCpu(cpu, if_enabled, &entry, &stack));
        try std.testing.expectEqual(scenario == 0, snapshotFsBaseForCpu(cpu, if_enabled, &fs));
        try std.testing.expectEqual(@as(u64, if (scenario == 0) 0x4000 else 11), entry.toInt());
        try std.testing.expectEqual(@as(u64, if (scenario == 0) 0x8000 else 22), stack.toInt());
        try std.testing.expectEqual(@as(u64, if (scenario == 0) 0x6000 else 33), fs.toInt());
    }
}

pub fn threadForPrincipal(principal: kernel.PrincipalId) ?usize {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    var i: usize = 0;
    while (i < threadDirectory().contexts.len) : (i += 1) {
        const ctx = threadContextLocked(i) orelse continue;
        if (ctx.allocated and ctx.owner_process == principal) return i;
    }
    return null;
}

pub fn currentThreadGeneration() ?u32 {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = pinnedContextForCpu(currentCpu()) orelse return null;
    return ctx.generation;
}

pub fn threadOwnedBy(thread_index: usize, principal: kernel.PrincipalId) bool {
    if (!builtin.is_test) @compileError("index-only thread operation is host-fixture-only");
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(thread_index) orelse return false;
    return ctx.allocated and ctx.owner_process == principal;
}

pub fn threadWakeTargetIsLive(thread_index: usize, generation: u32, owner: kernel.PrincipalId) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(thread_index) orelse return false;
    return ctx.allocated and ctx.generation == generation and ctx.owner_process == owner;
}

// T protects every node link/state below. Each binding owns a reference to
// its stable K-owned body. Moving it to pending transfers that reference so
// thread retirement never frees a node still visible to the cancellation drain.
var ipc_cancel_head: ?*ipc_call_binding.Node = null;
var ipc_cancel_tail: ?*ipc_call_binding.Node = null;

fn pendIpcBindingLocked(binding: *?*ipc_call_binding.Node) void {
    const node = binding.* orelse return;
    std.debug.assert(node.state == .bound and node.next == null);
    binding.* = null;
    node.terminal_token = node.wait_token;
    node.state = .pending;
    if (ipc_cancel_tail) |tail| tail.next = node else @atomicStore(?*ipc_call_binding.Node, &ipc_cancel_head, node, .release);
    ipc_cancel_tail = node;
}

fn detachIpcBindingsLocked(ctx: *ThreadContext) void {
    pendIpcBindingLocked(&ctx.ipc_outgoing);
    pendIpcBindingLocked(&ctx.ipc_receiving);
    pendIpcBindingLocked(&ctx.ipc_implicit);
    pendIpcBindingLocked(&ctx.ipc_block);
}

// K owns each block; T transfers independently owned references to pending.
// The ThreadContext may be reset immediately after transfer: no node is in it.
pub fn ipcBlock(index: usize, generation: u32, owner: kernel.PrincipalId) ?*kernel.IpcReplySlot {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = matchingThreadLocked(index, generation, owner) orelse return null;
    const node = ctx.ipc_block orelse return null;
    const block: *kernel.IpcReplySlot = @ptrCast(@alignCast(node.context.?));
    if (block.ref.generation != ctx.ipc_block_generation) return null;
    return block;
}
pub fn ipcBlockReusable(block: *kernel.IpcReplySlot) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return block.lifetime_refs.load(.acquire) == 1 and block.outgoing.state == .detached and block.implicit.state == .detached;
}

pub fn bindIpcBlock(index: usize, generation: u32, owner: kernel.PrincipalId, node: *ipc_call_binding.Node) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = matchingThreadLocked(index, generation, owner) orelse return false;
    if (ctx.ipc_block != null or node.state != .detached) return false;
    node.state = .bound;
    ctx.ipc_block = node;
    const block: *kernel.IpcReplySlot = @ptrCast(@alignCast(node.context.?));
    ctx.ipc_block_generation = block.ref.generation;
    return true;
}
pub fn ipcReceiveDetached(block: *kernel.IpcReplySlot) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return block.receive.state == .detached;
}

pub fn bindIpcReceive(target: kernel.ThreadWakeTarget, node: *ipc_call_binding.Node) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = matchingThreadLocked(target.thread_index, target.thread_generation, target.owner) orelse return false;
    if (ctx.active_wait_token != target.wait_token or target.wait_token == 0 or
        ctx.wait_completion_claimed or ctx.ipc_receiving != null or node.state != .detached) return false;
    node.wait_token = target.wait_token;
    node.state = .bound;
    ctx.ipc_receiving = node;
    return true;
}

fn reserveIpcTokenLocked(ctx: *ThreadContext) u64 {
    return reserveIpcTokenInnerLocked(ctx, true);
}
fn reserveIpcTokenInnerLocked(ctx: *ThreadContext, publish: bool) u64 {
    var token = ctx.next_wait_token;
    if (token == 0) token = 1;
    ctx.next_wait_token = token +% 1;
    if (ctx.next_wait_token == 0) ctx.next_wait_token = 1;
    ctx.active_wait_token = token;
    ctx.wait_mailbox = true;
    ctx.wait_ipc_words = null;
    ctx.wait_poll_result = null;
    ctx.wait_completion_rax = 0;
    ctx.wait_completion_publish_pending = false;
    if (publish) setThreadHotCompletionClaimed(ctx.id, false);
    return token;
}

fn fastIpcSourceLocked(owner: kernel.PrincipalId) ?*ThreadContext {
    const ctx = pinnedContextForCpu(currentCpu()) orelse return null;
    if (!ctx.allocated or ctx.owner_process != owner or !ctx.ready or ctx.stopped or
        ctx.pending_signal_mask != 0 or ctx.active_wait_token != 0 or ctx.wait_completion_claimed or
        principalLifecycleBlocksAdmission(owner)) return null;
    const node = ctx.scheduler_entity orelse return null;
    if (node.ownership != .running or !nodeMatches(node, ctx.id, ctx.generation, currentCpu())) return null;
    return ctx;
}
fn fastIpcTargetLocked(t: kernel.ThreadWakeTarget) ?*ThreadContext {
    const ctx = matchingThreadLocked(t.thread_index, t.thread_generation, t.owner) orelse return null;
    if (ctx.ready or ctx.stopped or ctx.pending_signal_mask != 0 or !ctx.wait_mailbox or
        ctx.active_wait_token != t.wait_token or t.wait_token == 0 or ctx.wait_completion_claimed or
        ctx.wake_tick != 0 or ctx.cpu_slot != currentCpu() or principalLifecycleBlocksAdmission(t.owner)) return null;
    const node = ctx.scheduler_entity orelse return null;
    if (!verifiedCoreReady() or !nodeMatches(node, ctx.id, ctx.generation, currentCpu()) or
        node.ownership != .blocked or node.linked_tree != null or !threadAllowsCpu(ctx, currentCpu())) return null;
    const cpu = schedulerStateForSlot(currentCpu()) orelse return null;
    cpu.lock.lock();
    defer cpu.lock.unlock();
    cpu.runqueue_lock.lock();
    defer cpu.runqueue_lock.unlock();
    if (!cpu.enabled) return null;
    var runnable: verified_sched.Entity = undefined;
    if (verified_sched.pacha_eevdf_entity_wake(&node.entity, cpuRunqueueFloor(cpu), &runnable) != .ok) return null;
    return ctx;
}
fn fastIpcPublishLocked(ctx: *ThreadContext, t: kernel.ThreadWakeTarget, words: [4]u64) void {
    pendIpcBindingLocked(&ctx.ipc_receiving);
    ctx.wait_completion_claimed = true;
    ctx.wait_ipc_words = words;
    ctx.wait_ipc_fd_count = 0;
    setWakeTickLocked(ctx, 0);
    // The read-only EEVDF preflight and this publication share T. The node
    // was blocked/unlinked and cannot migrate or change generation in between.
    std.debug.assert(publishClaimedWaitLocked(t.thread_index, t.thread_generation, t.owner, t.wait_token, 0, currentCpu(), null) == .published);
}

/// Endpoint and reply exclusion protect the pinned bodies; T covers terminal
/// checks and commit. A null result has changed neither registers nor queues.
pub fn tryIpcFastCall(owner: kernel.PrincipalId, receive: *kernel.IpcReplySlot, words: [4]u64, memory: @import("kernel_abi_root").ipc_abi.ReceiveMemory, capacity: u8) ?u64 {
    receive.receive_list.?.assertHeld();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const source = fastIpcSourceLocked(owner) orelse return null;
    const t = receive.receive_target;
    const target = fastIpcTargetLocked(t) orelse return null;
    const source_node = source.ipc_block orelse return null;
    const body: *kernel.IpcReplySlot = @ptrCast(@alignCast(source_node.context.?));
    body.exclusion.assertHeld();
    if (body.ref.generation != source.ipc_block_generation or source == target or source.ipc_outgoing != null or target.ipc_implicit != null or
        body.lifetime_refs.load(.acquire) != @as(usize, 1) or body.outgoing.state != .detached or body.implicit.state != .detached or
        target.ipc_receiving != &receive.receive or receive.receive_list == null or
        receive.ref.index != t.receive_block.index or receive.ref.generation != t.receive_block.generation) return null;
    // Commit: all remaining operations use retained scalar/node storage.
    const token = reserveIpcTokenLocked(source);
    body.caller = .{ .owner = owner, .thread_index = source.id, .thread_generation = source.generation, .wait_token = token, .recv_memory = memory, .recv_fd_capacity = capacity };
    body.sent = false;
    body.queue_key = null;
    body.retain(2);
    body.outgoing.wait_token = token;
    body.outgoing.state = .bound;
    source.ipc_outgoing = &body.outgoing;
    body.implicit.wait_token = token;
    body.implicit.state = .bound;
    target.ipc_implicit = &body.implicit;
    receive.receive_list.?.unlinkReceive(receive);
    fastIpcPublishLocked(target, t, words);
    return token;
}

/// K retains the validated empty endpoint queue; T makes the caller binding
/// and its queue authority visible together. Reuse only an owner-held block:
/// an extracted reply or pending cancellation must keep its separate lifetime.
/// Failure is read-only; success uses no allocation or user memory.
pub fn tryIpcFastEnqueue(owner: kernel.PrincipalId, queue: *kernel.IpcQueue, key: kernel.IpcWaitKey, words: [4]u64, memory: @import("kernel_abi_root").ipc_abi.ReceiveMemory, capacity: u8) ?u64 {
    queue.assertHeld();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const source = fastIpcSourceLocked(owner) orelse return null;
    const node = source.ipc_block orelse return null;
    const body: *kernel.IpcReplySlot = @ptrCast(@alignCast(node.context.?));
    body.exclusion.assertHeld();
    if (!queue.isEmpty() or queue.reference_sequence == std.math.maxInt(u64) or
        body.ref.generation != source.ipc_block_generation or source.ipc_outgoing != null or
        body.lifetime_refs.load(.acquire) != 1 or body.outgoing.state != .detached or body.implicit.state != .detached) return null;
    const token = reserveIpcTokenLocked(source);
    body.caller = .{ .owner = owner, .thread_index = source.id, .thread_generation = source.generation, .wait_token = token, .recv_memory = memory, .recv_fd_capacity = capacity };
    body.sent = false;
    body.queue_key = key;
    body.retain(2); // outgoing binding and queued reply authority
    body.outgoing.wait_token = token;
    body.outgoing.state = .bound;
    source.ipc_outgoing = &body.outgoing;
    // Endpoint exclusion and the capacity/sequence checks above make this
    // infallible. Ordinary dequeue transfers this same authority to the peer.
    queue.push(.{ .sender = owner, .words = words, .call = body.ref }) catch unreachable;
    return token;
}

/// Prepared under K -> endpoint/reply Pair -> T -> S -> runqueue. Success
/// transfers T/S/runqueue ownership to commit; the caller stores this value in
/// CPU-owned storage across the assembly save of the remaining user GPRs.
/// A miss releases those locks without publishing a wait or charging service.
// Diagnostic-only E -> T observation. A matching incarnation gives CPU
// affinity at observation time; it does not claim its wait or eligibility.
pub fn ipcDiagnosticTargetCpu(index: usize, generation: u32, owner: kernel.PrincipalId) u64 {
    if (comptime !ipc_metric.enabled) return std.math.maxInt(u64);
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const target = matchingThreadLocked(index, generation, owner) orelse return std.math.maxInt(u64);
    return target.cpu_slot;
}

pub fn directIpcProbeDue(key: u64) bool {
    return directProbeDue(key, true);
}
pub fn directNotifyProbeDue(key: u64) bool {
    return directProbeDue(key, false);
}
fn directProbeDue(key: u64, comptime record_ipc: bool) bool {
    const ctx = pinnedContextForCpu(currentCpu()) orelse return false;
    if (principalLifecycleBlocksAdmission(ctx.owner_process)) return false;
    if (ctx.direct_miss_key == key and ctx.entry_state.direct_miss_remaining != 0) {
        ctx.entry_state.direct_miss_remaining -= 1;
        if (record_ipc) ipc_metric.entryReason(.hint);
        return false;
    }
    return true;
}
pub fn noteDirectIpcMiss(key: u64) void {
    const ctx = pinnedContextForCpu(currentCpu()) orelse return;
    ctx.direct_miss_key = key;
    ctx.entry_state.direct_miss_remaining = 31;
}

pub const DirectHandoff = struct {
    source: *ThreadContext,
    target: *ThreadContext,
    cpu: *CpuSchedulerState,
    body: *kernel.IpcReplySlot,
    receive: *kernel.IpcReplySlot,
    list: *kernel.IpcWaitList,
    registration: kernel.FdWaitRegistrationRef,
    fd: kernel.Fd,
    call: bool,
    accounting: PreparedRuntime,
    blocked: verified_sched.Entity,
    running: verified_sched.Entity,
    selected_time: i64,
    target_as_pin: ?UserAddressSpaceTable.FastPin,
};

// SYSRET faults in ring 0 on a noncanonical RCX. Only low-half user RIPs and
// ordinary user flags are admitted, before any IPC mutation. The target must
// also be an active native IPC wait below; arbitrary interrupt/signal frames
// are never made SYSRET targets merely because their selectors look alike.
pub fn directSysretFrame(frame: *const TrapFrame) bool {
    return frame.rip < (@as(u64, 1) << 47) and frame.rsp < (@as(u64, 1) << 47) and
        x86_platform.validUserSelectors(frame.cs, frame.ss) and
        frame.rflags & ~@as(u64, 0x240ed7) == 0 and frame.rflags & 0x202 == 0x202;
}

pub fn prepareDirectHandoff(owner: kernel.PrincipalId, frame: *const TrapFrame, resolved: @import("state/fd_capability.zig").DirectIpc, body: *kernel.IpcReplySlot, call: bool, spaces: ?*UserAddressSpaceTable, out: *DirectHandoff) bool {
    resolved.list.assertHeld();
    body.exclusion.assertHeld();
    // The array address is metadata only until an actual FD is delivered.
    if (@import("syscall/native_ipc.zig").readReceive(frame) == null) return false;
    const metric = @import("ipc_metric.zig");
    // Diagnostic precedence attributes structural E-owned blockers before
    // scheduler gates, without changing admission or reading outside E.
    if (comptime metric.enabled) {
        if (resolved.list.dataPollAfter(null)) |node| {
            metric.pollBlocker(node, .direct);
            metric.entryReason(.poll);
            return false;
        }
        if (call and resolved.list.receive_head == null) {
            metric.entryReason(.not_waiting);
            return false;
        }
    }
    if (!directSysretFrame(frame) or !resolved.queue.isEmpty() or resolved.list.dataPollAfter(null) != null or !verifiedCoreReady()) return false;
    scheduler_state.thread_table.lock();
    var admitted = false;
    defer if (!admitted) scheduler_state.thread_table.unlock();
    const source = fastIpcSourceLocked(owner) orelse return false;
    if (source.wake_tick != 0 or source.deadline_node.linked or source.retry_queue != null or
        source.wait_completion_publish_pending) return false;
    const owner_node = source.ipc_block orelse return false;
    const owner_body: *kernel.IpcReplySlot = @ptrCast(@alignCast(owner_node.context.?));
    if (owner_body.ref.generation != source.ipc_block_generation) return false;
    const receive = if (call) resolved.list.receive_head orelse return false else owner_body;
    // Production direct-receive and outgoing bindings are published only by
    // native_ipc SYSCALL handlers. A blocked matching binding/token therefore
    // identifies a saved syscall continuation, never a preempted user frame.
    const t = if (call) receive.receive_target else body.caller;
    const target = matchingThreadLocked(t.thread_index, t.thread_generation, t.owner) orelse return false;
    const slot = currentCpu();
    if (comptime metric.enabled) if (target.cpu_slot != slot) {
        metric.entryReason(.cpu);
        return false;
    };
    if (source == target or target.cr3 == 0 or !target.allocated or target.ready or target.stopped or
        target.pending_signal_mask != 0 or !target.wait_mailbox or target.active_wait_token != t.wait_token or t.wait_token == 0 or
        target.wait_completion_claimed or target.wait_completion_publish_pending or target.wake_tick != 0 or target.deadline_node.linked or target.retry_queue != null or
        target.cpu_slot != slot or principalLifecycleBlocksAdmission(t.owner) or !threadAllowsCpu(target, slot) or
        !directSysretFrame(&target.frame)) return false;
    if (call) {
        if (body != owner_body or source.ipc_outgoing != null or target.ipc_implicit != null or
            body.lifetime_refs.load(.acquire) != 1 or body.outgoing.state != .detached or body.implicit.state != .detached or
            target.ipc_receiving != &receive.receive or receive.receive.state != .bound or receive.receive_list != resolved.list or
            receive.ref.index != t.receive_block.index or receive.ref.generation != t.receive_block.generation or
            !t.registration.matches(resolved.registration)) return false;
    } else {
        if (source.ipc_implicit != &body.implicit or body.implicit.state != .bound or body.sent or
            target.ipc_outgoing != &body.outgoing or body.outgoing.state != .bound or
            source.ipc_receiving != null or receive.receive.state != .detached or receive.receive_list != null) return false;
    }
    // K excludes AS retirement while T validates the native wait and owner.
    // AD is a terminal directory leaf below T; no A lock or IPI wait is taken.
    // The source is retained by this CPU's context pin. For a different AS,
    // bridge the target lookup to ownership until commit installs its CPU pin.
    var target_as_pin: ?UserAddressSpaceTable.FastPin = null;
    defer if (!admitted) {
        if (target_as_pin) |pin| pin.end();
    };
    if (source.cr3 != target.cr3) {
        if (spaces) |table| {
            const index = kernel.processIndexFromPrincipal(target.owner_process) orelse return false;
            const pin = table.pinFast(index) orelse return false;
            target_as_pin = pin;
            if (pin.space.cr3 == 0 or x86_platform.cr3WithUserPcid(pin.space.cr3, pcidForPrincipal(target.owner_process)) != target.cr3) return false;
        } else if (!builtin.is_test) return false;
    }
    const current = source.scheduler_entity orelse return false;
    const next = target.scheduler_entity orelse return false;
    if (next.ownership != .blocked or next.linked_tree != null or !nodeMatches(next, target.id, target.generation, slot)) return false;
    const cpu = schedulerStateForSlot(slot) orelse return false;
    cpu.lock.lock();
    defer if (!admitted) cpu.lock.unlock();
    cpu.runqueue_lock.lock();
    defer if (!admitted) cpu.runqueue_lock.unlock();
    if (!cpu.enabled or cpu.is_idle or cpu.current_context != source or cpu.current_generation != source.generation or
        cpu.current_entity != current or cpu.current_cr3 != source.cr3 or current.linked_tree != null or cpu.runqueue.selection_virtual_time != cpu.virtual_time) return false;
    var runnable: verified_sched.Entity = undefined;
    if (verified_sched.pacha_eevdf_entity_wake(&next.entity, cpuRunqueueFloor(cpu), &runnable) != .ok) return false;
    const selected_time = cpu.runqueue.directWakeSelectionTime(&runnable) orelse return false;
    const accounting = prepareRuntimeCharge(cpu, current, 0) orelse return false;
    if (!accounting.interval.measured) return false;
    var blocked: verified_sched.Entity = undefined;
    var running: verified_sched.Entity = undefined;
    if (verified_sched.pacha_eevdf_entity_block(&accounting.entity, &blocked) != .ok or
        verified_sched.pacha_eevdf_entity_mark_running(&runnable, &running) != .ok) return false;
    out.source = source;
    out.target = target;
    out.cpu = cpu;
    out.body = body;
    out.receive = receive;
    out.list = resolved.list;
    out.registration = resolved.registration;
    out.fd = @intCast(frame.rdi);
    out.call = call;
    // CPU-owned plan storage cannot alias these stack-local scalar results.
    // memcpy selects the audited scalar RAM primitive; aggregate assignment
    // otherwise lowers to the generic overlapping byte-copy loop in -Os.
    @memcpy(std.mem.asBytes(&out.accounting), std.mem.asBytes(&accounting));
    @memcpy(std.mem.asBytes(&out.blocked), std.mem.asBytes(&blocked));
    @memcpy(std.mem.asBytes(&out.running), std.mem.asBytes(&running));
    out.selected_time = selected_time;
    out.target_as_pin = target_as_pin;
    admitted = true;
    return true;
}

pub fn commitDirectHandoff(plan: *DirectHandoff, frame: *TrapFrame) void {
    const Registers = if (builtin.is_test) ContextTransferTestRegisters else x86_platform;
    const source = plan.source;
    const target = plan.target;
    const body = plan.body;
    const receive = plan.receive;
    const cpu = plan.cpu;
    // Eager save precedes changing the CPU pin: no lazy-FPU window exists.
    if (!builtin.is_test) @import("xstate_runtime.zig").saveIpcLiveIfNeeded();
    const words = @import("kernel_abi_root").ipc_abi.inputWords(frame);
    // Snapshot the receiver array without touching user memory. A later FD
    // reply uses the ordinary transfer/copyout path against this saved binding.
    const destination = @import("syscall/native_ipc.zig").readReceive(frame).?;
    const token = reserveIpcTokenInnerLocked(source, false);
    if (plan.call) {
        body.caller = .{ .owner = source.owner_process, .thread_index = source.id, .thread_generation = source.generation, .wait_token = token, .recv_memory = destination.memory, .recv_fd_capacity = destination.capacity };
        body.sent = false;
        body.queue_key = null;
        body.retain(2);
        body.outgoing.wait_token = token;
        body.outgoing.state = .bound;
        source.ipc_outgoing = &body.outgoing;
        body.implicit.wait_token = token;
        body.implicit.state = .bound;
        target.ipc_implicit = &body.implicit;
        plan.list.unlinkReceive(receive);
        target.ipc_receiving = null;
        receive.receive.state = .detached;
    } else {
        receive.receive_target = .{ .owner = source.owner_process, .thread_index = source.id, .thread_generation = source.generation, .wait_token = token, .recv_fd = plan.fd, .recv_memory = destination.memory, .recv_fd_capacity = destination.capacity, .registration = plan.registration, .receive_block = receive.ref };
        receive.retain(1);
        receive.receive.wait_token = token;
        receive.receive.state = .bound;
        source.ipc_receiving = &receive.receive;
        plan.list.appendReceive(receive);
        source.ipc_implicit = null;
        target.ipc_outgoing = null;
        body.implicit.state = .detached;
        body.outgoing.state = .detached;
        body.sent = true;
    }
    source.wait_uninterruptible = frame.rsi & @import("kernel_abi_root").ipc_abi.wait_uninterruptible != 0;
    frame.rax = @import("syscall/numbers.zig").syscall_err_not_ready;
    frame.rdx = 1;
    frame.r15 = 0;
    @import("kernel_abi_root").ipc_abi.outputWords(frame, .{ 0, 0, 0, 0 });
    const live = captureLeavingBases(Registers, currentCpu());
    // The CPU entry frame is distinct from both T-owned thread save images.
    @memcpy(std.mem.asBytes(&source.frame), std.mem.asBytes(frame));
    source.entry_state.notify_sysret_continuation = false;
    source.fs_base = live.fs;
    source.gs_base = live.gs;
    source.pkru = live.pkru;
    commitThreadBlockedLocked(source);
    publishDirectWaitStateLocked(source);
    target.frame.rax = 0;
    target.frame.r15 = 0;
    @import("kernel_abi_root").ipc_abi.outputWords(&target.frame, words);
    target.wait_ipc_words = null;
    target.wait_ipc_fd_count = 0;
    target.wait_completion_rax = 0;
    target.wait_uninterruptible = false;
    target.wait_mailbox = false;
    target.active_wait_token = 0;
    target.wait_completion_claimed = false;
    target.wait_completion_publish_pending = false;
    target.ready = true;
    publishDirectWaitStateLocked(target);
    cpu.runtime_accounting.commit(plan.accounting.interval);
    @memcpy(std.mem.asBytes(&source.scheduler_entity.?.entity), std.mem.asBytes(&plan.blocked));
    source.scheduler_entity.?.ownership = .blocked;
    @memcpy(std.mem.asBytes(&target.scheduler_entity.?.entity), std.mem.asBytes(&plan.running));
    target.scheduler_entity.?.ownership = .running;
    cpu.current_entity = target.scheduler_entity;
    setCpuVirtualTime(cpu, plan.selected_time);
    installCurrentContextLocked(cpu, target.id, target);
    // Retirement must quiesce this installed CPU pin before tearing down the
    // AS. The later user-return CR3 helper performs residency admission and
    // PCID no-flush selection with IF clear through the assembly CR3 load.
    if (plan.target_as_pin) |pin| pin.end();
    if (target.fs_base != live.fs) Registers.writeFsBase(target.fs_base);
    if (target.gs_base != live.gs) Registers.writeGsBase(target.gs_base);
    if (target.pkru != live.pkru) Registers.writePkru(target.pkru);
    publishBaseShadow(Registers, currentCpu(), target.id, target);
    @memcpy(std.mem.asBytes(frame), std.mem.asBytes(&target.frame));
    cpu.runqueue_lock.unlock();
    cpu.lock.unlock();
    scheduler_state.thread_table.unlock();
}

pub const DirectNotifyPlan = struct {
    pub const Mode = enum { immediate, local, remote, hint, block };
    source: *ThreadContext,
    target: ?*ThreadContext = null,
    cpu: ?*CpuSchedulerState = null,
    channel: *kernel.IpcChannelSlot,
    side: u8,
    wait: bool,
    send: bool,
    block_source: bool,
    mode: Mode,
    accounting: PreparedRuntime = undefined,
    source_entity: verified_sched.Entity = undefined,
    target_entity: verified_sched.Entity = undefined,
    selected_time: i64 = 0,
    target_as_pin: ?UserAddressSpaceTable.FastPin = null,
};

/// K/E protect the channel borrow and registration; T then S/runqueue protect
/// the exact token, CPU and EEVDF decision through the single commit below.
/// No latch, token, entity or user frame changes on a failed preflight.
pub fn prepareDirectNotify(owner: kernel.PrincipalId, frame: *const TrapFrame, channel: *kernel.IpcChannelSlot, side: u8, wait: bool, send: bool, spaces: ?*UserAddressSpaceTable, out: *DirectNotifyPlan) bool {
    const b8_start = @import("ipc_metric.zig").timestamp();
    defer @import("notify_metric.zig").segment(1, b8_start);
    channel.exclusion.assertHeld();
    if (!verifiedCoreReady() or !directSysretFrame(frame) or channel.ref_count <= 1) return false;
    if (channel.waiters[side].dataPollAfter(null)) |node| {
        @import("ipc_metric.zig").pollBlocker(node, .notify);
        return false;
    }
    if (channel.waiters[1 - side].dataPollAfter(null)) |node| {
        @import("ipc_metric.zig").pollBlocker(node, .notify);
        return false;
    }
    scheduler_state.thread_table.lock();
    var admitted = false;
    defer if (!admitted) scheduler_state.thread_table.unlock();
    const source = fastIpcSourceLocked(owner) orelse return false;
    if (source.wake_tick != 0 or source.deadline_node.linked or source.retry_queue != null or source.wait_completion_publish_pending) return false;
    const block_source = wait and !channel.notify_pending[side];
    if (block_source and (frame.rsi != std.math.maxInt(u64) or channel.notify_waiters[side].active)) return false;
    out.* = .{ .source = source, .channel = channel, .side = side, .wait = wait, .send = send, .block_source = block_source, .mode = if (block_source) .block else .immediate };
    const waiter = channel.notify_waiters[1 - side];
    const hint = channel.receiver_hints[1 - side];
    const runnable_hint = send and !wait and !waiter.active and hint.active;
    if (!send or (!waiter.active and !runnable_hint)) {
        if (block_source and !prepareDirectNotifyPark(source, out)) return false;
        admitted = true;
        return true;
    }
    const target = if (runnable_hint)
        matchingThreadLocked(hint.thread_index, hint.thread_generation, hint.owner) orelse return false
    else
        matchingThreadLocked(waiter.thread_index, waiter.thread_generation, waiter.owner) orelse return false;
    if (target != source and target.cpu_slot != currentCpu()) {
        if (runnable_hint) return false;
        // Remote waits use the established preferred-publication policy. Its
        // token claim and placement run once in commit under this same T;
        // don't first pin/validate a fixed old-CPU runqueue and redo it later.
        // This may publish on the sender CPU, but never switches CPUs here.
        out.target = target;
        out.mode = .remote;
        admitted = true;
        return true;
    }
    // A matching channel notify registration is produced only by native
    // NOTIFY_WAIT, never by a poll group or an arbitrary preempted user frame.
    if (runnable_hint) {
        // A ready hint can be an arbitrary preempted user context. Only a
        // deferred NOTIFY return, never a selector-shaped IRQ frame, is native.
        if (!target.ready or !target.entry_state.notify_sysret_continuation or target.wait_mailbox or target.active_wait_token != 0) return false;
    } else if (target.ready or !target.wait_mailbox or waiter.wait_token == 0 or target.active_wait_token != waiter.wait_token) return false;
    if (source == target or !target.allocated or target.stopped or target.pending_signal_mask != 0 or
        target.wait_completion_claimed or target.wait_completion_publish_pending or target.wake_tick != 0 or
        target.deadline_node.linked or target.retry_queue != null or target.ipc_receiving != null or
        target.wait_poll_result != null or principalLifecycleBlocksAdmission(target.owner_process) or
        !threadAllowsCpu(target, target.cpu_slot) or target.cr3 == 0 or !directSysretFrame(&target.frame)) return false;
    const next = target.scheduler_entity orelse return false;
    if (!nodeMatches(next, target.id, target.generation, target.cpu_slot)) return false;
    if (runnable_hint) {
        if (next.ownership != .runnable) return false;
    } else if (next.ownership != .blocked or next.linked_tree != null or next.parent != null or next.left != null or next.right != null) return false;
    var pin: ?UserAddressSpaceTable.FastPin = null;
    defer if (!admitted) if (pin) |p| p.end();
    if (source.cr3 != target.cr3) {
        if (spaces) |table| {
            const index = kernel.processIndexFromPrincipal(target.owner_process) orelse return false;
            pin = table.pinFast(index) orelse return false;
            if (pin.?.space.cr3 == 0 or x86_platform.cr3WithUserPcid(pin.?.space.cr3, pcidForPrincipal(target.owner_process)) != target.cr3) return false;
        } else if (!builtin.is_test) return false;
    }
    const cpu = schedulerStateForSlot(target.cpu_slot) orelse return false;
    cpu.lock.lock();
    defer if (!admitted) cpu.lock.unlock();
    cpu.runqueue_lock.lock();
    defer if (!admitted) cpu.runqueue_lock.unlock();
    if (!cpu.enabled or (!cpu.is_idle and cpu.current_thread == target.id) or cpu.runqueue.selection_virtual_time != cpu.virtual_time) return false;
    var runnable: verified_sched.Entity = undefined;
    if (runnable_hint) {
        if (next.linked_tree != &cpu.runqueue) return false;
        @memcpy(std.mem.asBytes(&runnable), std.mem.asBytes(&next.entity));
    } else if (verified_sched.pacha_eevdf_entity_wake(&next.entity, cpuRunqueueFloor(cpu), &runnable) != .ok) return false;
    {
        const current = source.scheduler_entity.?;
        if (cpu.is_idle or cpu.current_context != source or cpu.current_generation != source.generation or
            cpu.current_entity != current or cpu.current_cr3 != source.cr3 or current.linked_tree != null or
            current.parent != null or current.left != null or current.right != null) return false;
        const accounting = prepareRuntimeCharge(cpu, current, 0) orelse return false;
        if (!accounting.interval.measured) return false;
        var finished: verified_sched.Entity = undefined;
        const status = if (block_source) verified_sched.pacha_eevdf_entity_block(&accounting.entity, &finished) else verified_sched.pacha_eevdf_entity_finish(&accounting.entity, &finished);
        if (status != .ok) return false;
        const selected_time = (if (runnable_hint)
            cpu.runqueue.directRunnableWithSourceSelectionTime(next, &finished)
        else
            cpu.runqueue.directWakeWithSourceSelectionTime(&runnable, if (block_source) null else &finished)) orelse {
            @import("notify_metric.zig").segment(5, b8_start);
            return false;
        };
        var running: verified_sched.Entity = undefined;
        if (verified_sched.pacha_eevdf_entity_mark_running(&runnable, &running) != .ok) return false;
        @memcpy(std.mem.asBytes(&out.accounting), std.mem.asBytes(&accounting));
        @memcpy(std.mem.asBytes(&out.source_entity), std.mem.asBytes(&finished));
        @memcpy(std.mem.asBytes(&out.target_entity), std.mem.asBytes(&running));
        out.selected_time = selected_time;
        out.mode = if (runnable_hint) .hint else .local;
    }
    out.target = target;
    out.cpu = cpu;
    out.target_as_pin = pin;
    admitted = true;
    return true;
}

// K/E/T already exclude close, cancellation and signal publication. Retain
// S/runqueue until commit so the exact source charge is calculated once.
fn prepareDirectNotifyPark(source: *ThreadContext, out: *DirectNotifyPlan) bool {
    const cpu = schedulerStateForSlot(currentCpu()) orelse return false;
    cpu.lock.lock();
    var admitted = false;
    defer if (!admitted) cpu.lock.unlock();
    cpu.runqueue_lock.lock();
    defer if (!admitted) cpu.runqueue_lock.unlock();
    const current = source.scheduler_entity orelse return false;
    if (!cpu.enabled or cpu.is_idle or cpu.current_context != source or
        cpu.current_generation != source.generation or cpu.current_entity != current or
        cpu.current_cr3 != source.cr3 or current.linked_tree != null or current.parent != null or
        current.left != null or current.right != null or cpu.runqueue.selection_virtual_time != cpu.virtual_time) return false;
    const accounting = prepareRuntimeCharge(cpu, current, 0) orelse return false;
    if (!accounting.interval.measured) return false;
    var blocked: verified_sched.Entity = undefined;
    if (verified_sched.pacha_eevdf_entity_block(&accounting.entity, &blocked) != .ok) return false;
    out.cpu = cpu;
    @memcpy(std.mem.asBytes(&out.accounting), std.mem.asBytes(&accounting));
    @memcpy(std.mem.asBytes(&out.source_entity), std.mem.asBytes(&blocked));
    admitted = true;
    return true;
}

pub const DirectNotifyResult = struct {
    block_token: u64 = 0,
    remote_cpu: ?usize = null,
    parked: bool = false,
    idle: bool = false,
};

/// All channel borrows and K must end before this potentially nonreturning
/// idle transition. The source's user frame and wait are already published.
pub fn finishDirectNotifyIdle(frame: *TrapFrame) void {
    if (!isBootstrapSchedulerCpu()) smp.returnCurrentApToIdleFromInterrupt();
    loadNextReadyThreadOrIdle(frame);
}

/// Once admitted, no operation can fall back or repeat a notification. A
/// remote wait uses the existing preferred wake placement under T.
/// The caller ends E before any blocking callback can release K.
pub fn commitDirectNotify(plan: *DirectNotifyPlan, frame: *TrapFrame) DirectNotifyResult {
    const Registers = if (builtin.is_test) ContextTransferTestRegisters else x86_platform;
    const source = plan.source;
    const channel = plan.channel;
    channel.exclusion.assertHeld();
    const side = plan.side;
    var result: DirectNotifyResult = .{};
    if ((plan.mode == .local or plan.mode == .hint) and !builtin.is_test) @import("xstate_runtime.zig").saveIpcLiveIfNeeded();
    // Preserve notify_peer ordering: publish/consume the peer notification
    // first, then consume our own latch or register our next wait, under E/T.
    if (plan.send) {
        if (plan.mode == .remote) {
            const waiter = channel.notify_waiters[1 - side];
            channel.notify_waiters[1 - side] = .{};
            channel.notify_pending[1 - side] = true;
            if (waiter.wait_token != 0 and claimWaitTokenCompletionLocked(waiter.thread_index, waiter.thread_generation, waiter.owner, waiter.wait_token) == .claimed) {
                const completion = publishClaimedWaitLocked(waiter.thread_index, waiter.thread_generation, waiter.owner, waiter.wait_token, 0, currentCpu(), &result.remote_cpu);
                // A pending publication owns the completion too. Only a stale
                // registration leaves the latch for a subsequent real wait.
                if (completion != .stale) channel.notify_pending[1 - side] = false;
            }
        } else {
            channel.notify_pending[1 - side] = plan.target == null or plan.mode == .hint;
            if (plan.target != null and plan.mode != .hint) channel.notify_waiters[1 - side] = .{};
        }
    }
    if (plan.wait) {
        channel.receiver_hints[side] = .{ .active = true, .owner = source.owner_process, .thread_index = source.id, .thread_generation = source.generation };
        if (plan.block_source) {
            const token = reserveIpcTokenInnerLocked(source, false);
            channel.notify_waiters[side] = .{ .active = true, .owner = source.owner_process, .thread_index = source.id, .thread_generation = source.generation, .wait_token = token };
            result.block_token = token;
        } else channel.notify_pending[side] = false;
    }
    const direct_target = if (plan.mode == .remote) null else plan.target;
    if (direct_target) |target| {
        const cpu = plan.cpu.?;
        if (plan.mode == .hint) {
            // The native wait may already have completed with CLOSED or an
            // interruption. A ready hint schedules that saved return; it does
            // not win a new token and must not replace its result with OK.
            if (!cpu.runqueue.remove(target.scheduler_entity.?)) @panic("direct notify runnable hint invariant");
        } else {
            if (comptime ipc_metric.enabled) @import("notify_metric.zig").claimed(&target.notify_stamp.stamp, target.id, target.generation, @intFromEnum(target.owner_process), target.active_wait_token, target.cpu_slot, target.notify_stamp.direct_token == target.active_wait_token);
            target.frame.rax = 0;
            target.wait_uninterruptible = false;
            target.wait_mailbox = false;
            target.active_wait_token = 0;
            target.wait_completion_claimed = false;
            target.wait_completion_publish_pending = false;
            target.wait_completion_rax = 0;
            target.ready = true;
            publishDirectWaitStateLocked(target);
        }
        @memcpy(std.mem.asBytes(&target.scheduler_entity.?.entity), std.mem.asBytes(&plan.target_entity));
        {
            frame.rax = if (plan.block_source) @import("syscall/numbers.zig").syscall_err_not_ready else 0;
            const live = captureLeavingBases(Registers, currentCpu());
            @memcpy(std.mem.asBytes(&source.frame), std.mem.asBytes(frame));
            source.entry_state.notify_sysret_continuation = true;
            source.fs_base = live.fs;
            source.gs_base = live.gs;
            source.pkru = live.pkru;
            @memcpy(std.mem.asBytes(&source.scheduler_entity.?.entity), std.mem.asBytes(&plan.source_entity));
            if (plan.block_source) {
                source.wait_uninterruptible = frame.rdx & @import("kernel_abi_root").ipc_abi.wait_uninterruptible != 0;
                commitThreadBlockedLocked(source);
                publishDirectWaitStateLocked(source);
                if (comptime ipc_metric.enabled) source.notify_stamp.direct_token = source.active_wait_token;
                source.scheduler_entity.?.ownership = .blocked;
                result.block_token = 0; // this commit also performs the block
            } else {
                source.scheduler_entity.?.ownership = .runnable;
                if (!cpu.runqueue.insert(source.scheduler_entity.?)) @panic("direct notify source publication invariant");
            }
            target.scheduler_entity.?.ownership = .running;
            cpu.runtime_accounting.commit(plan.accounting.interval);
            cpu.current_entity = target.scheduler_entity;
            setCpuVirtualTime(cpu, plan.selected_time);
            installCurrentContextLocked(cpu, target.id, target);
            if (plan.target_as_pin) |p| p.end();
            if (target.fs_base != live.fs) Registers.writeFsBase(target.fs_base);
            if (target.gs_base != live.gs) Registers.writeGsBase(target.gs_base);
            if (target.pkru != live.pkru) Registers.writePkru(target.pkru);
            publishBaseShadow(Registers, currentCpu(), target.id, target);
            @memcpy(std.mem.asBytes(frame), std.mem.asBytes(&target.frame));
        }
        cpu.runqueue_lock.unlock();
        cpu.lock.unlock();
    } else frame.rax = 0;
    if (plan.mode == .block) {
        const cpu = plan.cpu.?;
        // Save eager XState while the source still owns this CPU pin. No
        // generic blocker may overwrite the saved native syscall frame later.
        if (!builtin.is_test) @import("xstate_runtime.zig").saveIpcLiveIfNeeded();
        const live = captureLeavingBases(Registers, currentCpu());
        frame.rax = @import("syscall/numbers.zig").syscall_err_not_ready;
        @memcpy(std.mem.asBytes(&source.frame), std.mem.asBytes(frame));
        source.entry_state.notify_sysret_continuation = true;
        source.fs_base = live.fs;
        source.gs_base = live.gs;
        source.pkru = live.pkru;
        source.wait_uninterruptible = frame.rdx & @import("kernel_abi_root").ipc_abi.wait_uninterruptible != 0;
        commitThreadBlockedLocked(source);
        publishDirectWaitStateLocked(source);
        if (comptime ipc_metric.enabled) source.notify_stamp.direct_token = source.active_wait_token;
        @memcpy(std.mem.asBytes(&source.scheduler_entity.?.entity), std.mem.asBytes(&plan.source_entity));
        source.scheduler_entity.?.ownership = .blocked;
        cpu.runtime_accounting.commit(plan.accounting.interval);
        cpu.runtime_accounting.end();
        cpu.current_entity = null;
        refreshCpuVirtualTime(cpu);
        cpu.runqueue_lock.unlock();
        cpu.lock.unlock();
        result.block_token = 0;
        result.parked = true;
        // Selection uses the ordinary EEVDF winner. The outgoing frame is
        // a native wait; the selected departure frame may be an interrupt
        // continuation and therefore still returns through IRET.
        if (builtin.is_test or !installNextLocalAfterBlockLocked(Registers, currentCpu(), frame, live, null)) {
            noteCpuIdleTick(currentCpu());
            result.idle = true;
        }
    }
    scheduler_state.thread_table.unlock();
    return result;
}

pub const FastReplyRecv = struct { token: u64, reply: kernel.IpcReplyRef, target: kernel.ThreadWakeTarget };
pub fn tryIpcFastReplyRecv(owner: kernel.PrincipalId, list: *kernel.IpcWaitList, registration: kernel.FdWaitRegistrationRef, fd: kernel.Fd, words: [4]u64, memory: @import("kernel_abi_root").ipc_abi.ReceiveMemory, capacity: u8) ?FastReplyRecv {
    list.assertHeld();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const source = fastIpcSourceLocked(owner) orelse return null;
    const implicit = source.ipc_implicit orelse return null;
    const body: *kernel.IpcReplySlot = @ptrCast(@alignCast(implicit.context.?));
    body.exclusion.assertHeld();
    const t = body.caller;
    const target = fastIpcTargetLocked(t) orelse return null;
    const owner_node = source.ipc_block orelse return null;
    const receive: *kernel.IpcReplySlot = @ptrCast(@alignCast(owner_node.context.?));
    if (receive.ref.generation != source.ipc_block_generation or source == target or body.sent or implicit.state != .bound or target.ipc_outgoing != &body.outgoing or
        body.outgoing.state != .bound or source.ipc_receiving != null or receive.receive.state != .detached or
        receive.receive_list != null) return null;
    // Register the next receive before publishing the replied caller. E/T
    // exclude send, close, cancellation and slot reuse throughout this commit.
    const token = reserveIpcTokenLocked(source);
    receive.receive_target = .{ .owner = owner, .thread_index = source.id, .thread_generation = source.generation, .wait_token = token, .recv_fd = fd, .recv_memory = memory, .recv_fd_capacity = capacity, .registration = registration, .receive_block = receive.ref };
    receive.retain(1);
    receive.receive.wait_token = token;
    receive.receive.state = .bound;
    source.ipc_receiving = &receive.receive;
    list.appendReceive(receive);
    source.ipc_implicit = null;
    target.ipc_outgoing = null;
    body.implicit.state = .detached;
    body.outgoing.state = .detached;
    body.sent = true;
    fastIpcPublishLocked(target, t, words);
    return .{ .token = token, .reply = body.ref, .target = t };
}

// A real binding borrows its stable reply block. Isolated host node fixtures
// have no body; production publishers always set context before exposure.
fn ipcBindingGuard(node: *ipc_call_binding.Node) @import("sync/ipc_endpoint.zig").Lock.Guard {
    if (node.context) |context| {
        const body: *kernel.IpcReplySlot = @ptrCast(@alignCast(context));
        return body.exclusion.acquire();
    }
    std.debug.assert(builtin.is_test);
    return .{ .lock = null, .restore = false };
}

pub fn bindIpcOutgoing(index: usize, generation: u32, owner: kernel.PrincipalId, token: u64, node: *ipc_call_binding.Node) bool {
    var endpoint_guard = ipcBindingGuard(node);
    defer endpoint_guard.end();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = matchingThreadLocked(index, generation, owner) orelse return false;
    if (token == 0 or ctx.active_wait_token != token or !ctx.wait_mailbox or ctx.wait_completion_claimed or ctx.ipc_outgoing != null or node.state != .detached) return false;
    node.kind = .outgoing;
    node.wait_token = token;
    node.state = .bound;
    ctx.ipc_outgoing = node;
    return true;
}

pub fn bindIpcImplicit(index: usize, generation: u32, owner: kernel.PrincipalId, node: *ipc_call_binding.Node) bool {
    var endpoint_guard = ipcBindingGuard(node);
    defer endpoint_guard.end();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = matchingThreadLocked(index, generation, owner) orelse return false;
    if (ctx.ipc_implicit != null or node.state != .detached) return false;
    node.kind = .implicit;
    if (node.context) |context| {
        const body: *kernel.IpcReplySlot = @ptrCast(@alignCast(context));
        node.wait_token = body.caller.wait_token;
    }
    node.state = .bound;
    ctx.ipc_implicit = node;
    return true;
}

// The caller holds K throughout peek/use/take: lifecycle cancellation may move
// the node under T, but its reference cannot be drained/freed behind K's back.
pub fn peekIpcImplicit(index: usize, generation: u32, owner: kernel.PrincipalId) ?*ipc_call_binding.Node {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = matchingThreadLocked(index, generation, owner) orelse return null;
    return ctx.ipc_implicit;
}

pub fn takeIpcImplicit(index: usize, generation: u32, owner: kernel.PrincipalId, expected: *ipc_call_binding.Node) bool {
    var endpoint_guard = ipcBindingGuard(expected);
    defer endpoint_guard.end();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = matchingThreadLocked(index, generation, owner) orelse return false;
    if (ctx.ipc_implicit != expected) return false;
    ctx.ipc_implicit = null;
    std.debug.assert(expected.state == .bound);
    expected.state = .detached;
    return true;
}

pub fn detachIpcOutgoing(index: usize, generation: u32, owner: kernel.PrincipalId, expected: *ipc_call_binding.Node) bool {
    var endpoint_guard = ipcBindingGuard(expected);
    defer endpoint_guard.end();
    return detachIpcOutgoingLocked(index, generation, owner, expected);
}

pub fn detachIpcOutgoingLocked(index: usize, generation: u32, owner: kernel.PrincipalId, expected: *ipc_call_binding.Node) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = matchingThreadLocked(index, generation, owner) orelse return false;
    if (ctx.ipc_outgoing != expected) return false;
    ctx.ipc_outgoing = null;
    std.debug.assert(expected.state == .bound);
    expected.state = .detached;
    return true;
}

// The atomic root is only an empty hint outside T; no node is dereferenced.
// Both publication and last-pop are serialized by T, so a later producer
// cannot have its publication erased by a consumer clearing an old flag.
pub fn hasPendingIpcCancellations() bool {
    return @atomicLoad(?*ipc_call_binding.Node, &ipc_cancel_head, .acquire) != null;
}

pub fn takePendingIpcCancellation() ?*ipc_call_binding.Node {
    if (!hasPendingIpcCancellations()) return null;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const node = @atomicLoad(?*ipc_call_binding.Node, &ipc_cancel_head, .acquire) orelse return null;
    std.debug.assert(node.state == .pending);
    @atomicStore(?*ipc_call_binding.Node, &ipc_cancel_head, node.next, .release);
    if (node.next == null) ipc_cancel_tail = null;
    node.next = null;
    node.state = .draining;
    return node;
}

// Call before releasing the drain's body reference. No node access is allowed
// after that release, and no bound node can be enqueued for a second drain.
// Called after acquiring the owning endpoint, never while taking it from T.
// The pending reference keeps this exact node and token from being rebound.
pub fn validateIpcCancellation(node: *ipc_call_binding.Node, token: u64) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return node.state == .draining and node.wait_token == token and node.terminal_token == token;
}

pub fn finishIpcCancellation(node: *ipc_call_binding.Node) void {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    std.debug.assert(node.state == .draining and node.next == null);
    node.state = .detached;
}

// Process teardown calls this before destroying its address space. Extracted
// reply FDs are deliberately not bindings and survive exec unless CLOEXEC.
pub fn detachPrincipalIpcBindings(owner: kernel.PrincipalId) void {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    for (threadDirectory().contexts) |ctx| {
        if (ctx.allocated and ctx.owner_process == owner) detachIpcBindingsLocked(ctx);
    }
}

pub fn reserveCurrentWaitToken(expected_generation: u32) ?u64 {
    const thread_index = currentThread();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(thread_index) orelse return null;
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
    ctx.wait_ipc_words = null;
    ctx.wait_poll_result = null;
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
    const ctx = threadContextLocked(thread_index) orelse return;
    if (!ctx.allocated or ctx.generation != expected_generation or
        !ctx.ready or ctx.active_wait_token != token)
    {
        return;
    }
    pendIpcBindingLocked(&ctx.ipc_outgoing);
    pendIpcBindingLocked(&ctx.ipc_receiving);
    ctx.wait_uninterruptible = false;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    setWakeTickLocked(ctx, 0);
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
    const ctx = threadContextLocked(thread_index) orelse return false;
    return ctx.allocated and ctx.owner_process == owner and
        ctx.generation == generation and ctx.active_wait_token == token;
}

pub fn liveThreadCount(principal: kernel.PrincipalId) usize {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    var count: usize = 0;
    var i: usize = 0;
    while (i < threadDirectory().contexts.len) : (i += 1) {
        const ctx = threadContextLocked(i) orelse continue;
        if (ctx.allocated and ctx.owner_process == principal) count += 1;
    }
    return count;
}

pub fn allocateReadyThread(owner_process: kernel.PrincipalId, user_spaces: *UserAddressSpaceTable, initial_frame: TrapFrame, free_list: *kernel.FreePageList) ?usize {
    return (allocateThreadWithReadyState(owner_process, user_spaces, initial_frame, true, free_list) orelse return null).index;
}

// Both scalars are captured at node publication; callers must not recover the
// generation later from an index that has already left table exclusion.
pub const ThreadAllocation = struct { index: usize, generation: u32 };

pub fn allocateSuspendedThread(owner_process: kernel.PrincipalId, user_spaces: *UserAddressSpaceTable, initial_frame: TrapFrame, free_list: *kernel.FreePageList) ?ThreadAllocation {
    return allocateThreadWithReadyState(owner_process, user_spaces, initial_frame, false, free_list);
}

fn allocateThreadWithReadyState(owner_process: kernel.PrincipalId, user_spaces: *UserAddressSpaceTable, initial_frame: TrapFrame, initial_ready: bool, free_list: *kernel.FreePageList) ?ThreadAllocation {
    if (!preparePrincipalSelfExit(owner_process, identity_allocator.allocator(free_list))) return null;
    const space = getUserSpace(user_spaces, owner_process) orelse return null;
    defer user_spaces.endBorrow(space);
    const user_cr3 = x86_platform.cr3WithUserPcid(space.cr3, pcidForPrincipal(owner_process));
    while (true) {
        var i: usize = 0;
        while (true) : (i += 1) {
            scheduler_state.thread_table.lock();
            defer scheduler_state.thread_table.unlock();
            if (!ensureThreadBodyLocked(i, identity_allocator.allocator(free_list))) return null;
            const ctx = threadContextLocked(i) orelse break;
            if (ctx.allocated) continue;
            if (threadSlotReferencedByCpu(i)) continue;
            // No candidate pointer or generation crosses the exclusion gap:
            // claim, context initialization and node admission are one action.
            if (!initializeThreadContextLocked(i, owner_process, user_cr3, initial_frame, initial_ready)) continue;
            const generation = ctx.generation;
            if (!verifiedAddThreadLocked(i, generation, initial_ready)) {
                // Failed admission never links the node into a runqueue.
                // Keep hot/context/node generations coherent for the retry.
                const entity = ctx.scheduler_entity;
                const next_generation = advanceThreadSlotGenerationLocked(i);
                if (entity) |node| node.reset(i, next_generation);
                ctx.* = .{
                    .id = @intCast(i),
                    .generation = next_generation,
                    .scheduler_entity = entity,
                };
                syncHotStateFromContext(i);
                return null;
            }
            return .{ .index = i, .generation = generation };
        }
        if (!ensureThreadCapacity(threadCapacity() + 1, free_list)) return null;
    }
}

test "thread storage allocation owns admission rollback and pinned reuse" {
    const saved_contexts = try std.testing.allocator.dupe(ThreadContext, &initial_thread_contexts);
    defer std.testing.allocator.free(saved_contexts);
    const saved_hot = initial_thread_hot_states;
    defer {
        @memcpy(&initial_thread_contexts, saved_contexts);
        initial_thread_hot_states = saved_hot;
        initializeStaticStorage();
    }
    @memset(&initial_thread_contexts, .{});
    @memset(&initial_thread_hot_states, .{});
    initializeStaticStorage();
    const spaces = try std.testing.allocator.alloc(UserAddressSpace, 1);
    defer std.testing.allocator.free(spaces);
    spaces[0] = .{};
    var table: UserAddressSpaceTable = .{};
    table.initializeFreshStorage(spaces);
    var free_list: kernel.FreePageList = .{};
    const frame = std.mem.zeroes(TrapFrame);
    const first_allocation = allocateSuspendedThread(default_process_principal, &table, frame, &free_list).?;
    const first = first_allocation.index;
    try std.testing.expectEqual(first_allocation.generation, initial_thread_contexts[first].generation);
    const second_allocation = allocateSuspendedThread(default_process_principal, &table, frame, &free_list).?;
    const second = second_allocation.index;
    try std.testing.expectEqual(second_allocation.generation, initial_thread_contexts[second].generation);
    try std.testing.expect(first != second);
    const ctx = &initial_thread_contexts[first];
    const generation = ctx.generation;
    installCurrentContextLocked(&scheduler_state.cpus[0], first, ctx);
    try std.testing.expect(releaseThread(first));
    const third_allocation = allocateSuspendedThread(default_process_principal, &table, frame, &free_list).?;
    const third = third_allocation.index;
    try std.testing.expectEqual(third_allocation.generation, initial_thread_contexts[third].generation);
    try std.testing.expect(third != first and third != second);
    clearCurrentContextLocked(&scheduler_state.cpus[0]);
    const reused_allocation = allocateSuspendedThread(default_process_principal, &table, frame, &free_list).?;
    const reused = reused_allocation.index;
    try std.testing.expectEqual(reused_allocation.generation, initial_thread_contexts[reused].generation);
    try std.testing.expectEqual(first, reused);
    try std.testing.expectEqual(nextThreadGeneration(generation), ctx.generation);
    try std.testing.expect(releaseThread(reused));
    const rejected_generation = ctx.generation;
    // Reject node admission after context initialization. No node is linked
    // on this failure; context/hot/node must all describe the rollback.
    scheduler_state.verified.faulted = true;
    try std.testing.expectEqual(@as(?usize, null), allocateReadyThread(default_process_principal, &table, frame, &free_list));
    scheduler_state.verified.faulted = false;
    try std.testing.expect(!ctx.allocated);
    try std.testing.expectEqual(@as(u8, 0), initial_thread_hot_states[first].allocated);
    try std.testing.expectEqual(nextThreadGeneration(rejected_generation), ctx.generation);
    try std.testing.expectEqual(ctx.generation, initial_scheduler_entities[first].generation);
    try std.testing.expectEqual(.free, initial_scheduler_entities[first].ownership);
    try std.testing.expectEqual(@as(usize, 0), scheduler_state.cpus[0].runqueue.count);
    try std.testing.expectEqual(first, allocateReadyThread(default_process_principal, &table, frame, &free_list).?);
    try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[0].runqueue.count);
}

pub fn releaseThreadGeneration(thread_index: usize, generation: u32, owner: kernel.PrincipalId) bool {
    defer reclaimThreadBodies();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    _ = matchingThreadLocked(thread_index, generation, owner) orelse return false;
    return releaseThreadLocked(thread_index);
}

fn releaseCurrentThread() bool {
    defer reclaimThreadBodies();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = pinnedContextForCpu(currentCpu()) orelse return false;
    return releaseThreadLocked(ctx.id);
}

pub fn releaseThread(thread_index: usize) bool {
    if (!builtin.is_test) @compileError("index-only thread operation is host-fixture-only");
    defer reclaimThreadBodies();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return releaseThreadLocked(thread_index);
}

fn releaseThreadLocked(thread_index: usize) bool {
    const ctx = threadContextLocked(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    // Never destroy a context which another CPU can still use for interrupt,
    // syscall-return, or user-entry state.  Lifecycle teardown first stops
    // and quiesces remote owners; suspended rollback has no CPU reference.
    if (threadSlotReferencedByRemoteCpu(thread_index, currentCpu())) return false;
    const generation = ctx.generation;
    if (!verifiedExitThreadGeneration(thread_index, generation)) return false;
    detachIpcBindingsLocked(ctx);
    setWakeTickLocked(ctx, 0);
    removeCompletionRetryLocked(ctx);
    const next_generation = advanceThreadSlotGenerationLocked(thread_index);
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
    const ctx = threadContextLocked(thread_index) orelse return false;
    if (!ctx.allocated or ctx.generation != generation) return false;
    if (ctx.cpu_slot != cpu_slot) return false;
    const state = schedulerStateForSlot(cpu_slot) orelse return false;
    state.lock.lock();
    defer state.lock.unlock();
    if (!state.enabled) return false;
    installCurrentContextLocked(state, thread_index, ctx);
    if (install_bases and !applyThreadBasesWithRegisters(Registers, thread_index)) return false;
    return true;
}

fn activateGeneration(thread_index: usize, generation: u32) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return activateGenerationLocked(thread_index, generation);
}

pub fn activate(thread_index: usize) bool {
    if (!builtin.is_test) @compileError("index-only thread operation is host-fixture-only");
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    return activateGenerationLocked(thread_index, ctx.generation);
}

/// The FS/GS base and PKRU values the kernel last set on this CPU for one
/// thread generation, during that thread's current continuous ownership of the
/// CPU. Every user return installs the current thread's values, yet a syscall
/// returning to the thread that made it changes none of them: the writes
/// bought nothing.
/// While the thread runs, its live registers are authoritative, not ctx: user
/// code may change them itself (WRFSBASE/WRGSBASE, WRPKRU, a segment load).
/// The shadow keeps what the kernel set, so the user return rewrites only a
/// field whose ctx the kernel changed since, and a user-made value survives a
/// syscall that touches another field. It never matches another thread, nor
/// a later generation: a different thread's values are compared only against
/// LiveBases read in the current kernel entry.
/// Every install publishes it after its writes, so user code never runs with
/// it invalid; a setter updates only its own field. It ends when the thread
/// leaves the CPU (captureLeavingBases, endBaseShadow).
const BaseShadow = struct {
    valid: bool = false,
    thread: u32 = 0,
    generation: u32 = 0,
    fs: u64 = 0,
    gs: u64 = 0,
    pkru: u32 = 0,
};
var bootstrap_base_shadow: [bootstrap_cpu_rows]BaseShadow = @splat(.{});
var base_shadow: []BaseShadow = &bootstrap_base_shadow;

/// The shadow describing `Registers` on `cpu`. Test register sets carry their
/// own slot so the shadow logic runs against them as on hardware.
fn baseShadowSlot(comptime Registers: type, cpu: usize) ?*BaseShadow {
    if (Registers == x86_platform) return if (cpu < base_shadow.len) &base_shadow[cpu] else null;
    if (@hasDecl(Registers, "shadow_slots")) return if (cpu < Registers.shadow_slots.len) &Registers.shadow_slots[cpu] else null;
    return null;
}

/// End the shadow of `cpu_slot` when that CPU stops running any thread without
/// a save (thread exit, idle publication): the next owner, even a later
/// generation of the same index, must not inherit it. Idle publication and
/// exit run on the CPU itself, which alone writes its slot otherwise.
fn endBaseShadow(cpu_slot: usize) void {
    if (cpu_slot < base_shadow.len) base_shadow[cpu_slot].valid = false;
}

/// Record `ctx`'s values as what the kernel set on this CPU for that thread
/// generation, in its current continuous ownership: right after an install
/// wrote them (or proved them equal), or after applyThreadBases rewrote the
/// fields whose ctx changed. A field it left alone may hold a value user code
/// set since; the shadow does not claim it equals the hardware, only that the
/// kernel has nothing newer for it.
fn publishBaseShadow(comptime Registers: type, cpu: usize, thread_index: usize, ctx: *const ThreadContext) void {
    const slot = baseShadowSlot(Registers, cpu) orelse return;
    slot.* = .{
        .valid = true,
        .thread = @intCast(thread_index),
        .generation = ctx.generation,
        .fs = ctx.fs_base,
        .gs = ctx.gs_base,
        .pkru = ctx.pkru,
    };
}

/// Write all three of `ctx`'s values and publish them: the install for a
/// thread entering this CPU with nothing known about the hardware.
fn installAllBases(comptime Registers: type, cpu: usize, thread_index: usize, ctx: *const ThreadContext) void {
    Registers.writeFsBase(ctx.fs_base);
    Registers.writeGsBase(ctx.gs_base);
    Registers.writePkru(ctx.pkru);
    publishBaseShadow(Registers, cpu, thread_index, ctx);
}

pub fn applyThreadBases(thread_index: usize) bool {
    return applyPinnedThreadBases(x86_platform, currentCpu(), thread_index);
}

fn applyPinnedThreadBases(comptime Registers: type, cpu: usize, thread_index: usize) bool {
    const state = schedulerStateForSlot(cpu) orelse return false;
    if (state.current_thread != thread_index) return false;
    const ctx = pinnedContextForCpu(cpu) orelse return false;
    // This runs after XRSTOR and can interrupt directory/table owners on a
    // fault return. Use the CPU's existing pin, never a fresh directory loan.
    return applyContextBasesShadowed(Registers, cpu, thread_index, ctx);
}

test "context transfer user return bases use only the current generation pin" {
    initializeStaticStorage();
    defer initializeStaticStorage();
    const index = 4;
    const saved = initial_thread_contexts[index];
    defer initial_thread_contexts[index] = saved;
    const ctx = &initial_thread_contexts[index];
    ctx.* = .{ .id = index, .allocated = true, .fs_base = 0xa000, .gs_base = 0xb000, .pkru = 1 };
    setTestCurrentThread(&scheduler_state.cpus[0], index);
    const Registers = ContextTransferTestRegisters;
    Registers.reset();
    // A diagnostic/user-return borrower must not need either directory or
    // table while the interrupted owner holds them.
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    lockThreadDirectory();
    defer unlockThreadDirectory();
    try std.testing.expect(applyPinnedThreadBases(Registers, 0, index));
    try std.testing.expectEqual([3]usize{ 1, 1, 1 }, Registers.writes);
    try std.testing.expect(!applyPinnedThreadBases(Registers, 0, index + 1));
    ctx.generation += 1;
    try std.testing.expect(!applyPinnedThreadBases(Registers, 0, index));
    ctx.generation -= 1;
    ctx.allocated = false;
    try std.testing.expect(!applyPinnedThreadBases(Registers, 0, index));
    try std.testing.expectEqual([3]usize{ 1, 1, 1 }, Registers.writes);
}

fn applyThreadBasesShadowed(comptime Registers: type, cpu: usize, thread_index: usize) bool {
    // Single-threaded shadow-policy fixtures deliberately model transitions
    // without installing a real CPU pin. Production must use the pinned path.
    if (!builtin.is_test) @compileError("shadow fixture is not a production context borrower");
    const ctx = threadContextLocked(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    return applyContextBasesShadowed(Registers, cpu, thread_index, ctx);
}

fn applyContextBasesShadowed(comptime Registers: type, cpu: usize, thread_index: usize, ctx: *const ThreadContext) bool {
    const shadow = baseShadowSlot(Registers, cpu) orelse {
        installAllBases(Registers, cpu, thread_index, ctx);
        return true;
    };
    const same = shadow.valid and shadow.thread == thread_index and shadow.generation == ctx.generation;
    if (!same or shadow.fs != ctx.fs_base) Registers.writeFsBase(ctx.fs_base);
    if (!same or shadow.gs != ctx.gs_base) Registers.writeGsBase(ctx.gs_base);
    // Entry saves the hardware PKRU into ctx.pkru, so a value the same thread
    // changed in user mode shows up here as a difference and is rewritten.
    if (!same or shadow.pkru != ctx.pkru) Registers.writePkru(ctx.pkru);
    publishBaseShadow(Registers, cpu, thread_index, ctx);
    return true;
}

fn applyThreadBasesWithRegisters(comptime Registers: type, thread_index: usize) bool {
    // Activation owns table; do not hide another lock acquisition here.
    const ctx = threadContextLocked(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    installAllBases(Registers, currentCpu(), thread_index, ctx);
    return true;
}

const BaseField = enum { fs, gs };

/// Set one base of a thread. For the thread running here, only that field is
/// written to hardware, ctx and a shadow naming this generation: invalidating
/// the whole shadow made the user return rewrite the other field from ctx,
/// discarding a value the thread had set itself (WRFSBASE, WRGSBASE). A shadow
/// not yet naming the thread is left as is; setting one field cannot vouch
/// for the others.
fn setThreadBaseWithRegisters(comptime Registers: type, cpu: usize, current: usize, thread_index: usize, comptime field: BaseField, value: u64) bool {
    const ctx = threadContextLocked(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    switch (field) {
        .fs => ctx.fs_base = value,
        .gs => ctx.gs_base = value,
    }
    if (thread_index != current) return true;
    switch (field) {
        .fs => Registers.writeFsBase(value),
        .gs => Registers.writeGsBase(value),
    }
    if (baseShadowSlot(Registers, cpu)) |slot| {
        if (slot.valid and slot.thread == thread_index and slot.generation == ctx.generation) switch (field) {
            .fs => slot.fs = value,
            .gs => slot.gs = value,
        };
    }
    return true;
}

pub fn setCurrentFsBase(fs_base: u64) bool {
    return setCurrentThreadBaseWithRegisters(x86_platform, .fs, fs_base);
}

fn setCurrentThreadBaseWithRegisters(comptime Registers: type, comptime field: BaseField, value: u64) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const cpu = currentCpu();
    const ctx = pinnedContextForCpu(cpu) orelse return false;
    return setThreadBaseWithRegisters(Registers, cpu, ctx.id, ctx.id, field, value);
}

pub fn setThreadBasesGeneration(thread_index: usize, generation: u32, owner: kernel.PrincipalId, fs_base: u64, gs_base: u64) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = matchingThreadLocked(thread_index, generation, owner) orelse return false;
    // Initialization must not write a context whose CPU is still using it.
    if (threadSlotReferencedByCpu(thread_index)) return false;
    ctx.fs_base = fs_base;
    ctx.gs_base = gs_base;
    return true;
}

pub fn setCurrentGsBase(gs_base: u64) bool {
    return setCurrentThreadBaseWithRegisters(x86_platform, .gs, gs_base);
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
    const ctx = threadContextLocked(thread_index) orelse return null;
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
    _ = thread_index;
    if (!ctx.allocated or ctx.generation != generation or ctx.owner_process != owner or
        ctx.ready or ctx.wait_mailbox or ctx.active_wait_token != 0 or
        ctx.wait_completion_claimed or ctx.wait_completion_publish_pending or
        ctx.wake_tick != 0 or ctx.active_fault_frame != 0) return false;
    for (scheduler_state.cpus) |*cpu| {
        if (cpu.current_context == ctx) return false;
    }
    return true;
}

// Host fixtures do not run on the kernel CPU stacks or hold production
// ownership locks. Keep their index and pin coherent without weakening the
// runtime writer rule or allowing a fixture helper into the real kernel.
fn setTestCurrentThread(state: *CpuSchedulerState, thread_index: usize) void {
    if (!builtin.is_test) @compileError("test-only CPU ownership writer");
    state.current_thread = thread_index;
    state.current_context = threadContextMutable(thread_index);
    state.current_generation = if (state.current_context) |ctx| ctx.generation else 0;
}

test "thread storage CPU pin distinguishes lifetime from XState access" {
    initializeStaticStorage();
    const tid = 4;
    const ctx = &initial_thread_contexts[tid];
    const saved = ctx.*;
    defer {
        ctx.* = saved;
        initializeStaticStorage();
    }
    ctx.* = .{ .id = tid, .generation = 17, .allocated = true, .ready = true };
    const cpu = &scheduler_state.cpus[0];
    installCurrentContextLocked(cpu, tid, ctx);
    try std.testing.expectEqual(ctx, currentContextForXState().?);
    ctx.ready = false; // Remote STOP does not revoke the final XSAVE/XRSTOR.
    try std.testing.expectEqual(ctx, currentContextForXState().?);
    cpu.is_idle = true;
    cpu.enabled = false;
    try std.testing.expect(threadSlotReferencedByCpu(tid));
    try std.testing.expectEqual(ctx, currentContextForXState().?);
    ctx.generation += 1;
    try std.testing.expectEqual(@as(?*ThreadContext, null), currentContextForXState());
    try std.testing.expect(threadSlotReferencedByCpu(tid));
    ctx.allocated = false;
    try std.testing.expect(threadSlotReferencedByCpu(tid));
    clearCurrentContextLocked(cpu);
    try std.testing.expect(!threadSlotReferencedByCpu(tid));
    try std.testing.expectEqual(@as(?*ThreadContext, null), currentContextForXState());
}

test "thread storage release retains local pin and rejects every remote pin" {
    initializeStaticStorage();
    const tid = 4;
    const ctx = &initial_thread_contexts[tid];
    const saved = ctx.*;
    const saved_hot = initial_thread_hot_states[tid];
    defer {
        ctx.* = saved;
        initial_thread_hot_states[tid] = saved_hot;
        initializeStaticStorage();
    }
    const spaces = try std.testing.allocator.alloc(UserAddressSpace, 1);
    defer std.testing.allocator.free(spaces);
    spaces[0] = .{};
    var table: UserAddressSpaceTable = .{};
    table.initializeFreshStorage(spaces);
    ctx.* = .{ .id = tid, .generation = 17, .scheduler_entity = &initial_scheduler_entities[tid] };
    initial_thread_slots[tid].generation = 17;
    const frame = std.mem.zeroes(TrapFrame);
    try std.testing.expect(initializeThreadContext(tid, default_process_principal, &table, frame));
    ctx.ready = false;
    syncHotStateFromContext(tid);
    try std.testing.expect(verifiedAddThread(tid, 17, false));
    const local = &scheduler_state.cpus[0];
    const remote = &scheduler_state.cpus[scheduler_state.cpus.len - 1];
    // The remote pin is deliberately outside cpu_count and generation-stale.
    // Scheduling flags must never substitute for ownership withdrawal.
    installCurrentContextLocked(remote, tid, ctx);
    remote.enabled = false;
    remote.is_idle = true;
    remote.current_generation = 16;
    try std.testing.expect(threadSlotReferencedByRemoteCpu(tid, 0));
    try std.testing.expect(!controlledThreadAvailableLocked(ctx, tid, 17, ctx.owner_process));
    try std.testing.expect(!releaseThread(tid));
    try std.testing.expect(ctx.allocated);
    clearCurrentContextLocked(remote);
    installCurrentContextLocked(local, tid, ctx);
    try std.testing.expect(releaseThread(tid));
    try std.testing.expect(!ctx.allocated);
    try std.testing.expectEqual(@as(u32, 18), ctx.generation);
    try std.testing.expectEqual(ctx, local.current_context.?);
    try std.testing.expectEqual(@as(u32, 17), local.current_generation);
    try std.testing.expect(threadSlotReferencedByCpu(tid));
    try std.testing.expectEqual(@as(?*ThreadContext, null), currentContextForXState());
    const retired = ctx.*;
    try std.testing.expect(!initializeThreadContext(tid, default_process_principal, &table, frame));
    try std.testing.expectEqualDeep(retired, ctx.*);
    clearCurrentContextLocked(local);
    try std.testing.expect(!threadSlotReferencedByCpu(tid));
    try std.testing.expect(initializeThreadContext(tid, default_process_principal, &table, frame));
    try std.testing.expectEqual(@as(u32, 18), ctx.generation);
}

test "thread storage AP claim pins the returned XState ticket through idle" {
    initializeStaticStorage();
    const tid = 4;
    const ctx = &initial_thread_contexts[tid];
    const saved = ctx.*;
    const saved_hot = initial_thread_hot_states[tid];
    defer {
        @atomicStore(u8, &scheduler_state.ap_user_dispatch_enabled, 0, .release);
        ctx.* = saved;
        initial_thread_hot_states[tid] = saved_hot;
        initializeStaticStorage();
    }
    ctx.* = .{ .id = tid, .generation = 17, .allocated = true, .ready = true, .cpu_slot = 1, .cpu_affinity = testAffinity(2), .scheduler_entity = &initial_scheduler_entities[tid] };
    scheduler_state.cpu_count = 2;
    scheduler_state.cpus[1].enabled = true;
    syncHotStateFromContext(tid);
    try std.testing.expect(verifiedAddThread(tid, 17, true));
    enableApUserDispatch();
    var ticket: scheduler_observer.UserEntry = undefined;
    try std.testing.expect(claimKernelScheduledUserEntry(1, &ticket));
    const cpu = &scheduler_state.cpus[1];
    try std.testing.expectEqual(ctx, cpu.current_context.?);
    try std.testing.expectEqual(@as(u32, 17), cpu.current_generation);
    try std.testing.expectEqual(@intFromPtr(&cpu.current_context.?.x_state), ticket.x_state_addr);
    // The real claim has released state.lock; its returned image reference
    // still belongs to the CPU and a different CPU may not retire it.
    try std.testing.expect(threadSlotReferencedByRemoteCpu(tid, 0));
    try std.testing.expect(!releaseThread(tid));
    try std.testing.expectEqual(ctx, cpu.current_context.?);
    observeCpuIdleFromAp(1);
    try std.testing.expectEqual(@as(?*ThreadContext, null), cpu.current_context);
    try std.testing.expect(!threadSlotReferencedByRemoteCpu(tid, 0));
}

pub fn getControlledThreadContext(thread_index: usize, generation: u32, owner: kernel.PrincipalId, out: *process_abi.ThreadUserContext) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    lockAllCpuSchedulerStates();
    defer unlockAllCpuSchedulerStates();
    const ctx = threadContextLocked(thread_index) orelse return false;
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
    const ctx = threadContextLocked(thread_index) orelse return false;
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
        ctx.* = .{ .id = tid, .allocated = true, .generation = generation, .owner_process = owner, .scheduler_entity = node, .cpu_affinity = testAffinity(1), .ready = false, .wait_mailbox = true, .active_wait_token = token, .wake_tick = 100 };
        defer ctx.* = .{ .id = tid, .scheduler_entity = node };
        defer initial_thread_hot_states[tid] = .{};
        scheduler_state.cpu_count = 1;
        scheduler_state.cpus[0].is_idle = true;
        setTestCurrentThread(&scheduler_state.cpus[0], idleThreadMarker);
        try std.testing.expect(verifiedAddThread(tid, generation, false));
        syncHotStateFromContext(tid);
        ctx.frame.rax = 123; // Timeout return value already saved by block.
        if (winner == 0) {
            try std.testing.expectEqual(.claimed, claimWaitTokenCompletion(tid, generation, owner, token));
            wakeExpiredTimers(100);
            try std.testing.expect(!ctx.ready and ctx.wait_completion_claimed);
            try std.testing.expectEqual(@as(u64, 0), ctx.wake_tick);
            try std.testing.expectEqual(.published, publishClaimedWaitCompletion(tid, generation, owner, token, 456));
        } else {
            if (winner == 1) {
                wakeExpiredTimers(100);
            } else {
                try std.testing.expectEqual(.woke, wakeIfWaitingTokenGenerationWithRax(tid, generation, owner, token, 123));
            }
            try std.testing.expectEqual(.stale, claimWaitTokenCompletion(tid, generation, owner, token));
            try std.testing.expectEqual(.stale, publishClaimedWaitCompletion(tid, generation, owner, token, 456));
        }
        wakeExpiredTimers(101);
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
    setTestCurrentThread(&scheduler_state.cpus[0], tid);
    try std.testing.expect(!getControlledThreadContext(tid, 17, owner, &image));
    scheduler_state.cpus[0].is_idle = true;
    // Idle alone is not a release: a still-pinned context remains protected.
    try std.testing.expect(!getControlledThreadContext(tid, 17, owner, &image));
    clearCurrentContextLocked(&scheduler_state.cpus[0]);
    try std.testing.expect(getControlledThreadContext(tid, 17, owner, &image));
    try std.testing.expectEqual(@as(u64, 0x4100_0000), image.fs_base);
    image.registers.rax = 123;
    image.gs_base = 0x4200_0000;
    image.xstate[160] = 0x7f;
    ctx.wait_mailbox = true;
    try std.testing.expect(!setControlledThreadContext(tid, 17, owner, &image));
    try std.testing.expectEqual(@as(u64, 0), ctx.frame.rax);
    ctx.wait_uninterruptible = false;
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

test "controlled thread context timer STOP preserves the live frame for CONT" {
    const saved_contexts = initial_thread_contexts[4..5].*;
    const saved_hot = initial_thread_hot_states[4..5].*;
    defer {
        initial_thread_contexts[4..5].* = saved_contexts;
        initial_thread_hot_states[4..5].* = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    const tid = 4;
    const owner = kernel.processPrincipalFromIndex(7).?;
    const ctx = &initial_thread_contexts[tid];
    ctx.* = .{ .id = tid, .allocated = true, .generation = 17, .owner_process = owner, .ready = true, .cpu_affinity = testAffinity(1), .scheduler_entity = &initial_scheduler_entities[tid], .cr3 = 0x2000 };
    ctx.frame.rip = 0x7000; // Previous block inside the LPR dispatch stack.
    ctx.frame.rsp = 0x9000;
    syncHotStateFromContext(tid);
    try std.testing.expect(verifiedAddThread(tid, 17, true));
    const picked = verifiedPickThreadForCpu(0).?;
    try std.testing.expectEqual(@as(usize, tid), picked.thread_index);
    const cpu = &scheduler_state.cpus[0];
    setTestCurrentThread(cpu, tid);
    cpu.current_principal = owner;
    cpu.current_cr3 = 0x2000;
    cpu.is_idle = false;
    var live = ctx.frame;
    live.rip = 0x8000;
    live.rsp = 0xa000;
    live.rax = 1;
    live.cs = 0x23;
    live.ss = 0x1b;
    ContextTransferTestRegisters.reset();
    // STOP removes readiness before its IPI reaches the executing CPU.
    try std.testing.expectEqual(@as(usize, 1), stopPrincipalThreads(owner));
    try std.testing.expect(ctx.stopped and ctx.resume_after_stop and !ctx.ready);
    try std.testing.expect(quiesceStoppedCurrentUserThreadWithRegisters(ContextTransferTestRegisters, &live));
    try std.testing.expect(cpu.is_idle and cpu.current_thread != tid);
    try std.testing.expectEqualDeep(live, ctx.frame);
    try std.testing.expectEqual(@as(u64, 0x1111), ctx.fs_base);
    try std.testing.expectEqual(@as(u64, 0x2222), ctx.gs_base);
    try std.testing.expectEqual(@as(u32, 0x40000000), ctx.pkru);
    try std.testing.expectEqual(@as(usize, 1), continuePrincipalThreads(owner));
    try std.testing.expect(ctx.ready and !ctx.stopped);
    const resumed = verifiedPickThreadForCpu(0).?;
    try std.testing.expectEqual(@as(usize, tid), resumed.thread_index);
    var restored = std.mem.zeroes(TrapFrame);
    try std.testing.expect(loadContextIntoFrameGenerationLockedWithRegisters(ContextTransferTestRegisters, tid, 17, &restored));
    try std.testing.expectEqualDeep(live, restored);
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
    const directory = threadDirectory();
    while (thread_index < directory.contexts.len) : (thread_index += 1) {
        const ctx = directory.contexts[thread_index];
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

    const current_ctx = threadContextLocked(current_thread_index) orelse return null;
    if (!current_ctx.allocated or current_ctx.owner_process != current_owner or current_ctx.cpu_slot != local_cpu) return null;
    const current_node = current_ctx.scheduler_entity orelse return null;
    if (!nodeMatches(current_node, current_thread_index, current_ctx.generation, local_cpu) or
        current_node.ownership != .running or local_state.current_entity != current_node)
    {
        return null;
    }

    const staged_ctx = threadContextLocked(staged_thread_index) orelse return null;
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

    for (scheduler_state.cpus, 0..) |*state, cpu_slot| {
        if (!state.enabled) continue;
        if (state.current_principal) |owner| {
            if (owner == staged_owner) return null;
            if (owner == current_owner and cpu_slot != local_cpu) return null;
        }
        if (state.current_entity) |entity| {
            if (entity.thread_index == staged_thread_index) return null;
            if (entity.thread_index == current_thread_index and cpu_slot != local_cpu) return null;
            const entity_ctx = threadContextLocked(entity.thread_index) orelse return null;
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
    defer user_spaces.endBorrow(space);
    const cr3 = x86_platform.cr3WithUserPcid(space.cr3, pcidForPrincipal(owner_process));
    const current_thread = currentThread();
    var exec_generation: u32 = undefined;
    var exec_context: *ThreadContext = undefined;

    scheduler_state.thread_table.lock();
    {
        const ctx = threadContextLocked(current_thread) orelse {
            scheduler_state.thread_table.unlock();
            return false;
        };
        if (!ctx.allocated or ctx.owner_process != owner_process) {
            scheduler_state.thread_table.unlock();
            return false;
        }
        detachIpcBindingsLocked(ctx);
        exec_generation = ctx.generation;
        exec_context = ctx;
        ctx.cr3 = cr3;
        ctx.fs_base = image.fs_base;
        ctx.gs_base = image.gs_base;
        ctx.pkru = image.pkru;
        ctx.frame = image.frame;
        ctx.entry_state.notify_sysret_continuation = false;
        ctx.x_state = initial_x_state;
        // Signal trampoline addresses belong to the old executable image.
        // Keep pending signals across exec, but require the new runtime to
        // register its own address-bound delivery configuration.
        ctx.signal_entry = 0;
        ctx.signal_scratch_base = 0;
        ctx.signal_scratch_size = 0;
        ctx.signal_delivery_ready = false;
        _ = kernel_runtime.kernel_state_global.clearProcessSignalPendingHintVa(owner_process);
        ctx.signal_inhibit_start = 0;
        ctx.signal_inhibit_end = 0;
        ctx.signal_inhibit_secondary_start = 0;
        ctx.signal_inhibit_secondary_end = 0;
        ctx.signal_owner = false;
        ctx.signal_pending_hint_va = 0;
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
        ctx.ready = true;
        ctx.wait_uninterruptible = false;
        ctx.wait_mailbox = false;
        ctx.active_wait_token = 0;
        ctx.wait_completion_claimed = false;
        ctx.wait_completion_publish_pending = false;
        ctx.wait_completion_rax = 0;
        setWakeTickLocked(ctx, 0);
        ctx.stopped = false;
        ctx.resume_after_stop = false;
        syncHotStateFromContext(current_thread);
    }
    scheduler_state.thread_table.unlock();

    if (schedulerStateForSlot(currentCpu())) |state| {
        state.lock.lock();
        installCurrentContextLocked(state, current_thread, exec_context);
        state.lock.unlock();
    }
    x86_platform.writeFsBase(image.fs_base);
    x86_platform.writeGsBase(image.gs_base);
    x86_platform.writePkru(image.pkru);
    // The new image starts in user mode from here with these values; the
    // shadow must name it before that, like every other install.
    if (baseShadowSlot(x86_platform, currentCpu())) |slot| slot.* = .{
        .valid = true,
        .thread = @intCast(current_thread),
        .generation = exec_generation,
        .fs = image.fs_base,
        .gs = image.gs_base,
        .pkru = image.pkru,
    };
    return true;
}

/// FS/GS base and PKRU as read from this CPU's hardware earlier in the same
/// kernel entry, with no user code and no base write since. Unlike a
/// ThreadContext's saved values, which user code may have changed since (a
/// segment load reloads FS/GS base, WRPKRU with PKE), these equal the live
/// registers, so a different thread's values may be compared against them.
const LiveBases = struct {
    fs: u64,
    gs: u64,
    pkru: u32,
};

/// Read the leaving thread's live bases and end its shadow on this CPU. The
/// shadow may name a thread only while that thread continuously owns the CPU:
/// once saved, STOP/controlled SET may change its ctx, and user code (WRPKRU,
/// a segment load) may already have left the hardware different from the
/// shadow. Were the shadow kept, a later return of the same thread here would
/// compare ctx against stale values and skip a write it needs. An install
/// over these values sets the shadow again for the thread it installs.
fn captureLeavingBases(comptime Registers: type, cpu: usize) LiveBases {
    const live = LiveBases{ .fs = Registers.readFsBase(), .gs = Registers.readGsBase(), .pkru = Registers.readPkru() };
    if (baseShadowSlot(Registers, cpu)) |slot| slot.valid = false;
    return live;
}

/// Returns the hardware values it read, or null when there was no current
/// context to save into and nothing was read.
fn saveCurrentThreadContextFromFrameLocked(comptime Registers: type, frame: *const TrapFrame) ?LiveBases {
    const current_thread = currentThread();
    const ctx = threadContextLocked(current_thread) orelse return null;
    if (!ctx.allocated) return null;
    const live = captureLeavingBases(Registers, currentCpu());
    ctx.frame = frame.*;
    ctx.entry_state.notify_sysret_continuation = false;
    ctx.cr3 = currentCr3();
    ctx.fs_base = live.fs;
    ctx.gs_base = live.gs;
    ctx.pkru = live.pkru;
    setThreadHotCr3(current_thread, ctx.cr3);
    const hot = getThreadHotStateConst(current_thread) orelse return live;
    if (hot.wait_mailbox == 0 and hot.wake_tick == 0) {
        ctx.ready = true;
        setThreadHotReady(current_thread, true);
    }
    return live;
}

fn loadContextIntoFrameGenerationLocked(thread_index: usize, generation: u32, frame: *TrapFrame) bool {
    return loadContextIntoFrameGenerationLockedWithRegisters(x86_platform, thread_index, generation, frame);
}

fn loadContextIntoFrameGenerationLockedWithRegisters(comptime Registers: type, thread_index: usize, generation: u32, frame: *TrapFrame) bool {
    return loadContextIntoFrameGenerationLockedMode(Registers, true, thread_index, generation, frame);
}

/// Every check a frame install needs, before anything is written: callers
/// that fail here leave the hardware bases as they were.
fn loadableContextGenerationLocked(thread_index: usize, generation: u32) ?*const ThreadContext {
    const ctx = threadContextLocked(thread_index) orelse return null;
    return loadableContextLocked(ctx, generation);
}

fn loadableContextLocked(ctx: *const ThreadContext, generation: u32) ?*const ThreadContext {
    const hot = getThreadHotStateConst(ctx.id) orelse return null;
    if (!ctx.allocated or ctx.generation != generation or hot.allocated == 0) return null;
    if (hot.ready == 0) return null;
    schedulerFullMemoryFence();
    return ctx;
}

/// Install the target's bases over `live`, read earlier in this entry, and
/// load its frame. Only fields that differ are written: two threads usually
/// share GS base and PKRU, and each WRMSR/WRPKRU is expensive under a
/// hypervisor. The shadow then names the target with exactly what the
/// hardware holds, so the user return's applyThreadBases writes nothing
/// again. Without `live` nothing is known about the hardware: write all three,
/// then publish them likewise.
fn loadContextIntoFrameOverLiveBasesLocked(comptime Registers: type, cpu: usize, thread_index: usize, generation: u32, frame: *TrapFrame, live: ?LiveBases) bool {
    const ctx = loadableContextGenerationLocked(thread_index, generation) orelse return false;
    const shadow = baseShadowSlot(Registers, cpu);
    if (live) |known| {
        if (ctx.fs_base != known.fs) Registers.writeFsBase(ctx.fs_base);
        if (ctx.gs_base != known.gs) Registers.writeGsBase(ctx.gs_base);
        if (ctx.pkru != known.pkru) Registers.writePkru(ctx.pkru);
        if (shadow) |slot| slot.* = .{
            .valid = true,
            .thread = @intCast(thread_index),
            .generation = ctx.generation,
            .fs = ctx.fs_base,
            .gs = ctx.gs_base,
            .pkru = ctx.pkru,
        };
    } else installAllBases(Registers, cpu, thread_index, ctx);
    frame.* = ctx.frame;
    return true;
}

fn loadContextIntoFrameGenerationLockedMode(comptime Registers: type, comptime install_bases: bool, thread_index: usize, generation: u32, frame: *TrapFrame) bool {
    const ctx = loadableContextGenerationLocked(thread_index, generation) orelse return false;
    if (install_bases) installAllBases(Registers, currentCpu(), thread_index, ctx);
    frame.* = ctx.frame;
    return true;
}

fn loadContextIntoFrameGeneration(thread_index: usize, generation: u32, frame: *TrapFrame) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return loadContextIntoFrameGenerationLocked(thread_index, generation, frame);
}

pub fn loadContextIntoFrame(thread_index: usize, frame: *TrapFrame) bool {
    if (!builtin.is_test) @compileError("index-only thread operation is host-fixture-only");
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(thread_index) orelse return false;
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

// Only NOTIFY's native handler supplies this installer. It marks the saved
// source while T still excludes a concurrent user return or frame replacement.
fn switchToNotifyGenerationLocked(next_thread: usize, next_generation: u32, frame: *TrapFrame, saved_rax: ?u64) bool {
    return switchToNotifyGenerationLockedWithRegisters(x86_platform, next_thread, next_generation, frame, saved_rax);
}
fn switchToNotifyGenerationLockedWithRegisters(comptime Registers: type, next_thread: usize, next_generation: u32, frame: *TrapFrame, saved_rax: ?u64) bool {
    const source = threadContextLocked(currentThread()) orelse return false;
    const installed = switchToGenerationLockedWithRegisters(Registers, next_thread, next_generation, frame, saved_rax);
    if (installed) source.entry_state.notify_sysret_continuation = directSysretFrame(&source.frame);
    return installed;
}

fn switchToGenerationLocked(next_thread: usize, next_generation: u32, frame: *TrapFrame, saved_rax: ?u64) bool {
    return switchToGenerationLockedWithRegisters(x86_platform, next_thread, next_generation, frame, saved_rax);
}

fn switchToGenerationLockedWithRegisters(comptime Registers: type, next_thread: usize, next_generation: u32, frame: *TrapFrame, saved_rax: ?u64) bool {
    if (!isBootstrapSchedulerCpu() and !apUserDispatchReady()) return false;
    if (next_thread >= threadDirectory().contexts.len) return false;
    const current_thread = currentThread();
    const current_ctx = threadContextLocked(current_thread) orelse return false;
    if (!current_ctx.allocated) return false;
    const current_generation = current_ctx.generation;
    if (next_thread == current_thread) {
        if (current_generation != next_generation) return false;
        if (saved_rax) |value| frame.rax = value;
        return true;
    }

    var saved = frame.*;
    if (saved_rax) |value| saved.rax = value;
    const live = saveCurrentThreadContextFromFrameLocked(Registers, &saved);

    // Syscall/interrupt entry keeps IF clear. No external callback may run
    // between publishing the target identity and installing its live bases.
    // Activation here writes no bases, so `live` still equals the hardware
    // when the load installs the target's over it; the shadow it leaves
    // lets the common user-return guard skip writing the same values again.
    if (!activateGenerationLockedWithRegisters(Registers, false, next_thread, next_generation)) return false;
    if (!loadContextIntoFrameOverLiveBasesLocked(Registers, currentCpu(), next_thread, next_generation, frame, live)) {
        restoreSwitchSourceLocked(Registers, current_thread, current_generation, frame);
        return false;
    }
    wake_trace.record(.context_switch, next_thread, current_thread, next_thread);
    return true;
}

const ContextTransferTestRegisters = struct {
    var fs: u64 = 0;
    var gs: u64 = 0;
    var pkru: u32 = 0;
    var reads: [3]usize = .{ 0, 0, 0 };
    var writes: [3]usize = .{ 0, 0, 0 };
    // One per modeled CPU, as base_shadow is, so tests can move a thread.
    var shadow_slots: [2]BaseShadow = .{ .{}, .{} };

    fn reset() void {
        fs = 0x1111;
        gs = 0x2222;
        pkru = 0x40000000;
        reads = .{ 0, 0, 0 };
        writes = .{ 0, 0, 0 };
        shadow_slots = .{ .{}, .{} };
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
    setTestCurrentThread(&scheduler_state.cpus[0], sender);
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
    try std.testing.expectEqual(&initial_thread_contexts[target], scheduler_state.cpus[0].current_context.?);
    try std.testing.expectEqual(initial_thread_contexts[target].generation, scheduler_state.cpus[0].current_generation);
    try std.testing.expectEqual(initial_thread_contexts[target].owner_process, scheduler_state.cpus[0].current_principal.?);
    try std.testing.expectEqual(@as(u64, 0x2000), scheduler_state.cpus[0].current_cr3);

    // Exercise the same rollback helper used after a failed frame install.
    scheduler_state.thread_table.lock();
    restoreSwitchSourceLocked(Registers, sender, 1, &frame);
    scheduler_state.thread_table.unlock();
    try std.testing.expectEqual(@as(usize, sender), currentThread());
    try std.testing.expectEqual(@as(u64, 0x1000), currentCr3());
    try std.testing.expectEqual(source, scheduler_state.cpus[0].current_context.?);
    try std.testing.expectEqual(source.generation, scheduler_state.cpus[0].current_generation);
    try std.testing.expectEqual(source.owner_process, scheduler_state.cpus[0].current_principal.?);
    try std.testing.expectEqual(@as(u64, 0x1000), scheduler_state.cpus[0].current_cr3);
    try std.testing.expectEqualDeep(source.frame, frame);
    try std.testing.expectEqual(@as(u64, 0x1111), Registers.fs);
    try std.testing.expectEqual(@as(u64, 0x2222), Registers.gs);
    try std.testing.expectEqual(@as(u32, 0x40000000), Registers.pkru);
    try std.testing.expectEqual([3]usize{ 2, 2, 2 }, Registers.writes);
    // The rollback is an install too: the sender's values are published.
    try std.testing.expect(Registers.shadow_slots[0].valid and Registers.shadow_slots[0].thread == sender and Registers.shadow_slots[0].fs == 0x1111);

    // Public activation's implementation must retain its standalone writes.
    scheduler_state.thread_table.lock();
    const activated = activateGenerationLockedWithRegisters(Registers, true, target, 1);
    scheduler_state.thread_table.unlock();
    try std.testing.expect(activated);
    try std.testing.expectEqual([3]usize{ 3, 3, 3 }, Registers.writes);
}

test "context transfer writes only the bases that differ from the live values" {
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
        // Two threads of one runtime: distinct TLS (FS), shared GS and PKRU.
        initial_thread_contexts[index] = .{
            .id = @intCast(index),
            .allocated = true,
            .ready = true,
            .scheduler_entity = &initial_scheduler_entities[index],
            .owner_process = kernel.processPrincipalFromIndex(index - sender).?,
            .fs_base = 0x3333,
            .gs_base = 0x2222,
            .pkru = 0x40000000,
        };
        syncHotStateFromContext(index);
    }
    setTestCurrentThread(&scheduler_state.cpus[0], sender);
    Registers.reset();
    var frame = std.mem.zeroes(TrapFrame);
    try std.testing.expect(switchToGenerationWithRegisters(Registers, target, 1, &frame, 0));
    try std.testing.expectEqual([3]usize{ 1, 1, 1 }, Registers.reads);
    try std.testing.expectEqual([3]usize{ 1, 0, 0 }, Registers.writes);
    try std.testing.expectEqual(@as(u64, 0x3333), Registers.fs);
    // The user return after the switch finds the shadow naming the target.
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, target));
    try std.testing.expectEqual([3]usize{ 1, 0, 0 }, Registers.writes);

    // Violations of the shadow match each rewrite: a changed saved value...
    initial_thread_contexts[target].gs_base = 0x5555;
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, target));
    try std.testing.expectEqual([3]usize{ 1, 1, 0 }, Registers.writes);
    // ...another thread whose values happen to equal the shadow...
    initial_thread_contexts[sender].fs_base = 0x3333;
    initial_thread_contexts[sender].gs_base = 0x5555;
    initial_thread_contexts[sender].pkru = 0x40000000;
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, sender));
    try std.testing.expectEqual([3]usize{ 2, 2, 1 }, Registers.writes);
    // ...and the same index in a later generation.
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, target));
    try std.testing.expectEqual([3]usize{ 3, 3, 2 }, Registers.writes);
    Registers.shadow_slots[0].generation +%= 1;
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, target));
    try std.testing.expectEqual([3]usize{ 4, 4, 3 }, Registers.writes);

    // Without live values nothing is known about the hardware: all three are
    // written, then published, so the user return writes nothing again.
    scheduler_state.thread_table.lock();
    const loaded = loadContextIntoFrameOverLiveBasesLocked(Registers, 0, target, 1, &frame, null);
    scheduler_state.thread_table.unlock();
    try std.testing.expect(loaded);
    try std.testing.expectEqual([3]usize{ 5, 5, 4 }, Registers.writes);
    try std.testing.expect(Registers.shadow_slots[0].valid and Registers.shadow_slots[0].thread == target);
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, target));
    try std.testing.expectEqual([3]usize{ 5, 5, 4 }, Registers.writes);

    // A target failing the load checks leaves hardware and shadow untouched.
    Registers.shadow_slots[0] = .{ .valid = true, .thread = target, .generation = 1 };
    scheduler_state.thread_table.lock();
    const rejected = loadContextIntoFrameOverLiveBasesLocked(Registers, 0, target, 2, &frame, .{ .fs = 0, .gs = 0, .pkru = 0 });
    scheduler_state.thread_table.unlock();
    try std.testing.expect(!rejected);
    try std.testing.expectEqual([3]usize{ 5, 5, 4 }, Registers.writes);
    try std.testing.expect(Registers.shadow_slots[0].valid);
}

test "context transfer ends the base shadow when the thread leaves the CPU" {
    const Registers = ContextTransferTestRegisters;
    const saved_contexts = initial_thread_contexts[4..5].*;
    const saved_hot = initial_thread_hot_states[4..5].*;
    defer {
        initial_thread_contexts[4..5].* = saved_contexts;
        initial_thread_hot_states[4..5].* = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    const thread = 4;
    initial_thread_contexts[thread] = .{ .id = thread, .allocated = true, .ready = true, .scheduler_entity = &initial_scheduler_entities[thread], .fs_base = 0xa000, .gs_base = 0xb000, .pkru = 0x40000000 };
    syncHotStateFromContext(thread);
    setTestCurrentThread(&scheduler_state.cpus[0], thread);
    Registers.reset();
    // Installed: the shadow names the thread with the values it received.
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, thread));
    try std.testing.expect(Registers.shadow_slots[0].valid);
    // User code moves FS on its own (a segment load, or WRFSBASE).
    Registers.fs = 0xc000;
    // The thread leaves the CPU: STOP, preemption, block or a switch save it.
    var frame = std.mem.zeroes(TrapFrame);
    scheduler_state.thread_table.lock();
    const live = saveCurrentThreadContextFromFrameLocked(Registers, &frame);
    scheduler_state.thread_table.unlock();
    try std.testing.expectEqual(@as(u64, 0xc000), live.?.fs);
    try std.testing.expectEqual(@as(u64, 0xc000), initial_thread_contexts[thread].fs_base);
    try std.testing.expect(!Registers.shadow_slots[0].valid);
    // A controlled SET restores the old value while it is off the CPU; its
    // return here must write it although ctx equals what the shadow last saw.
    initial_thread_contexts[thread].fs_base = 0xa000;
    const writes = Registers.writes;
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, thread));
    try std.testing.expectEqual(@as(u64, 0xa000), Registers.fs);
    try std.testing.expectEqual(writes[0] + 1, Registers.writes[0]);
}

test "context transfer ends the base shadow when the CPU idles or its thread exits" {
    defer initializeStaticStorage();
    initializeStaticStorage();
    const slot = &base_shadow[0];
    slot.* = .{ .valid = true, .thread = 4, .generation = 1 };
    noteCpuIdleTick(0);
    try std.testing.expect(!slot.valid);
    slot.* = .{ .valid = true, .thread = 4, .generation = 1 };
    observeCpuIdleFromAp(0); // The bootstrap CPU idles through noteCpuIdleTick.
    try std.testing.expect(slot.valid);
    endBaseShadow(0);
    try std.testing.expect(!slot.valid);
}

test "context transfer keeps a user-set base across a setter for the other field" {
    const Registers = ContextTransferTestRegisters;
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    const thread = 4;
    const other = 5;
    for ([_]usize{ thread, other }) |index| {
        initial_thread_contexts[index] = .{ .id = @intCast(index), .allocated = true, .ready = true, .scheduler_entity = &initial_scheduler_entities[index], .fs_base = 0xa000, .gs_base = 0xb000, .pkru = 0x40000000 };
        syncHotStateFromContext(index);
    }
    setTestCurrentThread(&scheduler_state.cpus[0], thread);
    Registers.reset();
    // First entry: activation installs all three and publishes the shadow.
    scheduler_state.thread_table.lock();
    try std.testing.expect(activateGenerationLockedWithRegisters(Registers, true, thread, 1));
    scheduler_state.thread_table.unlock();
    try std.testing.expect(Registers.shadow_slots[0].valid and Registers.shadow_slots[0].thread == thread);
    try std.testing.expectEqual([3]usize{ 1, 1, 1 }, Registers.writes);

    // The thread sets FS itself, then asks the kernel for a new GS.
    Registers.fs = 0xf000;
    try std.testing.expect(setThreadBaseWithRegisters(Registers, 0, thread, thread, .gs, 0xc000));
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, thread));
    try std.testing.expectEqual(@as(u64, 0xf000), Registers.fs);
    try std.testing.expectEqual(@as(u64, 0xc000), Registers.gs);
    try std.testing.expectEqual([3]usize{ 1, 2, 1 }, Registers.writes);

    // And the other way round.
    Registers.gs = 0xe000;
    try std.testing.expect(setThreadBaseWithRegisters(Registers, 0, thread, thread, .fs, 0xd000));
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, thread));
    try std.testing.expectEqual(@as(u64, 0xd000), Registers.fs);
    try std.testing.expectEqual(@as(u64, 0xe000), Registers.gs);
    try std.testing.expectEqual([3]usize{ 2, 2, 1 }, Registers.writes);

    // A thread not running here changes only its ctx.
    try std.testing.expect(setThreadBaseWithRegisters(Registers, 0, thread, other, .fs, 0x1234));
    try std.testing.expectEqual(@as(u64, 0x1234), initial_thread_contexts[other].fs_base);
    try std.testing.expectEqual([3]usize{ 2, 2, 1 }, Registers.writes);
    try std.testing.expect(Registers.shadow_slots[0].thread == thread and Registers.shadow_slots[0].fs == 0xd000);

    // A setter never vouches for a shadow it did not find: still invalid, so
    // the user return installs everything.
    Registers.shadow_slots[0].valid = false;
    try std.testing.expect(setThreadBaseWithRegisters(Registers, 0, thread, thread, .gs, 0x5000));
    try std.testing.expect(!Registers.shadow_slots[0].valid);
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, thread));
    try std.testing.expectEqual([3]usize{ 3, 4, 2 }, Registers.writes);
    try std.testing.expect(Registers.shadow_slots[0].valid);
}

test "context transfer does not trust a shadow left on a CPU the thread moved away from" {
    const Registers = ContextTransferTestRegisters;
    const saved_contexts = initial_thread_contexts[4..5].*;
    const saved_hot = initial_thread_hot_states[4..5].*;
    defer {
        initial_thread_contexts[4..5].* = saved_contexts;
        initial_thread_hot_states[4..5].* = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    const thread = 4;
    const ctx = &initial_thread_contexts[thread];
    ctx.* = .{ .id = thread, .allocated = true, .ready = true, .scheduler_entity = &initial_scheduler_entities[thread], .fs_base = 0xa000, .gs_base = 0xb000, .pkru = 0x40000000 };
    syncHotStateFromContext(thread);
    Registers.reset();
    // Runs on CPU 0, then leaves it.
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, thread));
    const on_a = captureLeavingBases(Registers, 0);
    ctx.fs_base = on_a.fs;
    try std.testing.expect(!Registers.shadow_slots[0].valid);
    // Runs on CPU 1, sets FS itself, is stopped there and a controlled SET
    // restores the value CPU 0 last installed.
    try std.testing.expect(applyThreadBasesShadowed(Registers, 1, thread));
    Registers.fs = 0xc000;
    const on_b = captureLeavingBases(Registers, 1);
    try std.testing.expectEqual(@as(u64, 0xc000), on_b.fs);
    ctx.fs_base = 0xa000;
    // Another thread used CPU 0 meanwhile; back there, FS must be installed.
    Registers.fs = 0xdead;
    const writes = Registers.writes;
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, thread));
    try std.testing.expectEqual(@as(u64, 0xa000), Registers.fs);
    try std.testing.expectEqual(writes[0] + 1, Registers.writes[0]);
    try std.testing.expect(Registers.shadow_slots[0].valid and !Registers.shadow_slots[1].valid);
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
        setTestCurrentThread(&scheduler_state.cpus[0], sender);
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
    return handoffToReadyThreadGenerationInternal(false, false, frame, target_thread, target_generation, sender_rax, before_current_thread_leave);
}

pub fn handoffToOnlyReadyThreadGenerationWithRax(
    frame: *TrapFrame,
    target_thread: usize,
    target_generation: u32,
    sender_rax: u64,
    before_current_thread_leave: ?BeforeCurrentThreadLeaveCallback,
) bool {
    return handoffToReadyThreadGenerationInternal(true, false, frame, target_thread, target_generation, sender_rax, before_current_thread_leave);
}

pub fn handoffNotifyToReadyThreadGenerationWithRax(frame: *TrapFrame, target_thread: usize, target_generation: u32, sender_rax: u64, before_leave: ?BeforeCurrentThreadLeaveCallback) bool {
    return handoffToReadyThreadGenerationInternal(false, true, frame, target_thread, target_generation, sender_rax, before_leave);
}

fn handoffToReadyThreadGenerationInternal(
    comptime only_ready: bool,
    comptime native_notify: bool,
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
    const current_ctx = threadContextLocked(current_thread) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    const target_ctx = threadContextLocked(target_thread) orelse {
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
    if (can_handoff and target_cpu == cpu_id) {
        // Already on this runqueue (an IPC completion published here): no
        // migration, so the check, the ownership transition and the context
        // switch share this one hold instead of retaking the table for each.
        // thread_table -> runqueue_lock is the established order (block).
        const switched = verifiedHandoffToThreadOnCpuLocked(only_ready, cpu_id, target_thread, target_generation, current_thread, current_generation) and
            blk: {
                if (switchToGenerationLocked(target_thread, target_generation, frame, sender_rax)) break :blk true;
                verifiedRollbackHandoffLocked(cpu_id, current_thread, current_generation);
                break :blk false;
            };
        if (native_notify and switched) current_ctx.entry_state.notify_sysret_continuation = directSysretFrame(&current_ctx.frame);
        scheduler_state.thread_table.unlock();
        // As on the general path, the caller's lock drop runs after the table
        // is released and before the user return.
        if (switched) {
            if (before_current_thread_leave) |callback| callback.run(callback.context);
        }
        return switched;
    }
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
    // Preserve the identity admitted by migration and the verified handoff;
    // rereading the slot's generation here could adopt a replacement thread.
    if (switchToGeneration(target_thread, target_generation, frame, sender_rax)) {
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
    setTestCurrentThread(&scheduler_state.cpus[0], sender);
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
    ctx.cpu_affinity = testAffinity(2);
    try std.testing.expect(!handoffToReadyThreadGenerationWithRax(&frame, peer, 1, 0, null));
    scheduler_state.cpu_count = 2;
    scheduler_state.cpus[1].enabled = true;
    ctx.cpu_affinity = testAffinity(1);
    try std.testing.expect(!migrateRunnableThreadGeneration(peer, 1, 1));
    ctx.cpu_affinity = testAffinity(3);
    try std.testing.expect(migrateRunnableThreadGeneration(peer, 1, 1));
    setTestCurrentThread(&scheduler_state.cpus[1], peer);
    scheduler_state.cpus[1].is_idle = false;
    try std.testing.expect(!handoffToReadyThreadGenerationWithRax(&frame, peer, 1, 0, null));
    try std.testing.expectEqual(@as(usize, 1), ctx.cpu_slot);
    try std.testing.expectEqual(.runnable, initial_scheduler_entities[peer].ownership);
    try std.testing.expectEqual(.running, initial_scheduler_entities[sender].ownership);
}

test "yield handoff keeps runnable ownership and rolls back without duplication" {
    const topology_fixture = try TestCpuRows.init(72);
    defer topology_fixture.deinit();
    for ([_]usize{ 0, 1, 5, 71 }) |cpu| {
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
    sender.* = .{ .id = 4, .allocated = true, .ready = true, .owner_process = owner, .scheduler_entity = &initial_scheduler_entities[4], .cpu_affinity = testAffinity(3) };
    try std.testing.expect(verifiedAddThread(4, 1, true));
    try std.testing.expectEqual(@as(usize, 4), verifiedPickThreadForCpu(0).?.thread_index);
    setTestCurrentThread(&scheduler_state.cpus[0], 4);
    scheduler_state.cpus[0].is_idle = false;
    const peer = &initial_thread_contexts[5];
    peer.* = .{ .id = 5, .allocated = true, .generation = 17, .ready = false, .wait_mailbox = true, .active_wait_token = 11, .wake_tick = 100, .owner_process = owner, .cpu_slot = 1, .scheduler_entity = &initial_scheduler_entities[5], .cpu_affinity = testAffinity(3) };
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
        initial_thread_contexts[tid] = .{ .id = @intCast(tid), .allocated = true, .ready = tid != 6, .owner_process = owner, .cpu_slot = if (tid == 6) 1 else 0, .scheduler_entity = &initial_scheduler_entities[tid], .cpu_affinity = testAffinity(3), .wait_mailbox = tid == 6, .active_wait_token = if (tid == 6) 11 else 0 };
        try std.testing.expect(verifiedAddThread(tid, 1, tid != 6));
    }
    try std.testing.expectEqual(@as(usize, 4), verifiedPickThreadForCpu(0).?.thread_index);
    setTestCurrentThread(&scheduler_state.cpus[0], 4);
    scheduler_state.cpus[0].is_idle = false;
    try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[0].runqueue.count);
    try std.testing.expectEqual(.woke, wakeIfWaitingTokenGenerationWithRaxPreferUncontendedCurrent(6, 1, owner, 11, 0));
    try std.testing.expectEqual(@as(usize, 1), initial_thread_contexts[6].cpu_slot);
    try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[0].runqueue.count);
    try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[1].runqueue.count);
}

pub fn exitCurrentThread(
    frame: *TrapFrame,
    _: u64,
    before_ap_idle: ?BeforeCurrentThreadLeaveCallback,
    after_release: ?BeforeCurrentThreadLeaveCallback,
) bool {
    // An exiting thread is never saved, so nothing else ends its shadow.
    if (!isBootstrapSchedulerCpu()) {
        if (!releaseCurrentThread()) return false;
        endBaseShadow(currentCpu());
        if (after_release) |callback| callback.run(callback.context);
        if (before_ap_idle) |callback| callback.run(callback.context);
        smp.returnCurrentApToIdleFromInterrupt();
    }
    if (!releaseCurrentThread()) return false;
    endBaseShadow(currentCpu());
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
    if (!builtin.is_test) @compileError("index-only thread operation is host-fixture-only");
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    return wakeIfWaitingLocked(thread_index);
}

fn wakeIfWaitingLocked(thread_index: usize) bool {
    const ctx = threadContextLocked(thread_index) orelse return false;
    if (!ctx.allocated or !ctx.wait_mailbox or
        ctx.active_wait_token != 0) return false;
    ctx.wait_uninterruptible = false;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    setWakeTickLocked(ctx, 0);
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
    wakeIfTimerExpiredLocked(thread_index, now_tick);
}

fn wakeIfTimerExpiredLocked(thread_index: usize, now_tick: u64) void {
    const ctx = threadContextLocked(thread_index) orelse return;
    // Candidate selection and completion share T. Keep validation here too:
    // explicit completion callers and host fixtures use the same transition.
    if (!ctx.allocated or ctx.ready or ctx.wait_completion_claimed or
        ctx.wake_tick == 0 or now_tick < ctx.wake_tick)
    {
        return;
    }
    const wait_mailbox = ctx.wait_mailbox;
    const wait_token = ctx.active_wait_token;
    const wake_tick = ctx.wake_tick;
    const wait_uninterruptible = ctx.wait_uninterruptible;
    ctx.wait_uninterruptible = false;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    setWakeTickLocked(ctx, 0);
    if (ctx.stopped) {
        pendIpcBindingLocked(&ctx.ipc_outgoing);
        pendIpcBindingLocked(&ctx.ipc_receiving);
        ctx.resume_after_stop = true;
        ctx.ready = false;
        setThreadHotWaitState(thread_index, false, 0, false);
        return;
    }
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    if (!verifiedWakeThreadGeneration(thread_index, ctx.generation)) {
        // Do not strand a blocked scheduler entity behind ready=true.  The
        // timer index must retain the original wait identity, so retain the
        // expired deadline and retry publication on the next BSP tick.
        ctx.wait_mailbox = wait_mailbox;
        ctx.active_wait_token = wait_token;
        setWakeTickLocked(ctx, wake_tick);
        ctx.wait_uninterruptible = wait_uninterruptible;
        ctx.ready = false;
        setThreadHotWaitState(thread_index, wait_mailbox, wake_tick, false);
    } else {
        // Publication failure restores the token; cancellation must likewise
        // remain bound until a timer wake actually wins.
        pendIpcBindingLocked(&ctx.ipc_outgoing);
        pendIpcBindingLocked(&ctx.ipc_receiving);
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
    const ctx = threadContextLocked(thread_index) orelse return false;
    if (!ctx.allocated or ctx.generation != generation) return false;
    if (ctx.wait_completion_claimed) return false;
    if (mailbox_only) {
        if (!ctx.wait_mailbox or ctx.active_wait_token != 0) return false;
    } else if (ctx.ready) {
        return false;
    }
    if (resume_rax) |value| ctx.frame.rax = value;
    ctx.wait_uninterruptible = false;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    setWakeTickLocked(ctx, 0);
    if (ctx.stopped) {
        ctx.resume_after_stop = true;
        ctx.ready = false;
        setThreadHotWaitState(thread_index, false, 0, false);
        return true;
    }
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    _ = verifiedWakeThreadGenerationPreferred(thread_index, generation, preferred_cpu, null);
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
    const ctx = threadContextLocked(thread_index) orelse return .stale;
    if (!ctx.allocated or ctx.owner_process != owner or
        ctx.generation != generation or ctx.active_wait_token != wait_token or
        ctx.wait_completion_claimed)
    {
        return .stale;
    }

    if (ctx.ready) {
        if (!ctx.wait_mailbox) return .stale;
        ctx.frame.rax = resume_rax;
        ctx.wait_uninterruptible = false;
        ctx.wait_mailbox = false;
        setWakeTickLocked(ctx, 0);
        return .woke;
    }
    if (!ctx.wait_mailbox) return .stale;

    const previous_rax = ctx.frame.rax;
    const wake_tick = ctx.wake_tick;
    const wait_uninterruptible = ctx.wait_uninterruptible;
    ctx.frame.rax = resume_rax;
    ctx.wait_uninterruptible = false;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    setWakeTickLocked(ctx, 0);
    if (ctx.stopped) {
        ctx.resume_after_stop = true;
        ctx.ready = false;
        setThreadHotWaitState(thread_index, false, 0, false);
        return .woke;
    }
    ctx.ready = true;
    setThreadHotWaitState(thread_index, false, 0, true);
    if (!verifiedWakeThreadGenerationPreferred(thread_index, generation, preferred_cpu, null)) {
        ctx.frame.rax = previous_rax;
        ctx.wait_mailbox = true;
        ctx.active_wait_token = wait_token;
        setWakeTickLocked(ctx, wake_tick);
        ctx.wait_uninterruptible = wait_uninterruptible;
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
    wait_uninterruptible: bool = false,
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
    const source_ctx = threadContextLocked(source_id) orelse return null;
    const peer = threadContextLocked(request.peer) orelse return null;
    if (!source_ctx.allocated or source_ctx.owner_process != request.owner or
        source_ctx.cpu_slot != cpu or !source_ctx.ready or source_ctx.stopped or
        !source_ctx.wait_mailbox or source_ctx.active_wait_token != request.caller_token or
        source_ctx.wait_completion_claimed or source_ctx.wait_completion_publish_pending) return null;
    if (firstUnblockedPendingSignal(source_ctx) != 0 and source_ctx.signal_entry != 0 and source_ctx.signal_delivery_ready and
        !request.wait_uninterruptible) return null;
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
        const accounting = prepareRuntimeCharge(state, source, 0) orelse break :commit false;
        var running: verified_sched.Entity = undefined;
        var blocked: verified_sched.Entity = undefined;
        if (scalarIssue("wake-wait-wake", verified_sched.pacha_eevdf_entity_wake(&target.entity, cpuRunqueueFloor(state), &awakened), request.peer) or
            scalarIssue("wake-wait-run", verified_sched.pacha_eevdf_entity_mark_running(&awakened, &running), request.peer) or
            scalarIssue("wake-wait-block", verified_sched.pacha_eevdf_entity_block(&accounting.entity, &blocked), source_id)) break :commit false;

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
        _ = saveCurrentThreadContextFromFrameLocked(Registers, &saved);
        if (comptime perf.enabled) {
            perf.handoffElapsed(.capture_cycles, capture_start);
            perf.handoffAdd(.capture_calls, 1);
        }
        const publication_start = perf.timestamp();
        source_ctx.wait_uninterruptible = request.wait_uninterruptible;
        source_ctx.ready = false;
        source_ctx.wait_completion_claimed = false;
        source_ctx.wait_completion_publish_pending = false;
        source_ctx.wait_completion_rax = 0;
        setWakeTickLocked(source_ctx, if (request.timeout_ticks == 0) 0 else lapic_tick_count +| request.timeout_ticks);
        setThreadHotWaitState(source_id, true, source_ctx.wake_tick, false);
        setThreadHotCompletionClaimed(source_id, false);
        source.entity = blocked;
        source.ownership = .blocked;

        target.entity = running;
        target.ownership = .running;
        target.cpu_slot = cpu;
        state.runtime_accounting.commit(accounting.interval);
        state.current_entity = target;
        peer.cpu_slot = cpu;
        peer.frame.rax = 0;
        peer.wait_uninterruptible = false;
        peer.wait_mailbox = false;
        peer.active_wait_token = 0;
        setWakeTickLocked(peer, 0);
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
    clearCurrentContextLocked(state);
    refreshCpuVirtualTime(state);
    return false;
}

pub const IpcNotifyWakeResult = enum {
    /// Completed and this CPU now runs the waiter.
    handed_off,
    /// Completed: runnable (the caller may still try the generic handoff),
    /// or held until CONT if the waiter is stopped.
    published,
    /// Completed; the scheduler retries publication from its tick.
    pending,
    /// No longer that wait: the completion was not taken.
    stale,
};

/// Complete IPC_NOTIFY's detached lean waiter and, when this CPU is
/// uncontended, run it here in the same table hold: the claim, the wake, the
/// waiter's mark_running and the sender's finish form one transition instead
/// of an enqueue that the following handoff immediately dequeues. Every
/// condition the fused path cannot prove falls back, before anything changed,
/// to the ordinary claim and preferred publication. As there, the claim under
/// the thread table is the linearization point: signal and timeout paths see
/// either the untouched wait or its completion.
pub fn completeIpcNotifyWaiterAndHandoff(
    frame: *TrapFrame,
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
    sender_rax: u64,
) IpcNotifyWakeResult {
    return completeIpcNotifyWaiterAndHandoffWithInstall(switchToNotifyGenerationLocked, frame, thread_index, generation, owner, wait_token, resume_rax, sender_rax);
}

fn completeIpcNotifyWaiterAndHandoffWithInstall(
    comptime install: anytype,
    frame: *TrapFrame,
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
    sender_rax: u64,
) IpcNotifyWakeResult {
    if (wait_token == 0) return .stale;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const cpu = currentCpu();
    const uncontended = if (schedulerStateForSlot(cpu)) |state| blk: {
        state.runqueue_lock.lock();
        defer state.runqueue_lock.unlock();
        break :blk state.current_entity != null and state.runqueue.count == 0;
    } else false;
    if (uncontended) {
        // The fused path rechecks every condition under the CPU and runqueue
        // locks; this sample only avoids taking them when it cannot succeed.
        wake_trace.record(.fused_try, thread_index, wait_token, thread_index);
        if (tryFusedKeyedWakeLocked(install, .capability_checked, frame, thread_index, generation, owner, wait_token, resume_rax, sender_rax)) |handed_off| {
            wake_trace.record(if (handed_off) .fused_commit else .fused_reject, thread_index, wait_token, thread_index);
            // A failed install rolled the waiter back to runnable on this
            // CPU; its completion stands.
            return if (handed_off) .handed_off else .published;
        }
        wake_trace.record(.fused_reject, thread_index, wait_token, thread_index);
    }
    if (claimWaitTokenCompletionLocked(thread_index, generation, owner, wait_token) != .claimed) return .stale;
    return switch (publishClaimedWaitLocked(thread_index, generation, owner, wait_token, resume_rax, cpu, null)) {
        .published => .published,
        .pending => .pending,
        .stale => .stale,
    };
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
            wake_trace.record(.fused_try, thread_index, wait_token, thread_index);
            if (tryFusedKeyedWakeLocked(install, .same_process, frame, thread_index, generation, owner, wait_token, resume_rax, sender_rax)) |handed_off| {
                wake_trace.record(if (handed_off) .fused_commit else .fused_reject, thread_index, wait_token, thread_index);
                return .{ .wake = .woke, .handed_off = handed_off };
            }
            wake_trace.record(.fused_reject, thread_index, wait_token, thread_index);
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

/// Who may complete the wait. A futex key is process-local, so its waker must
/// belong to the waiter's process. An IPC notification was authorized by the
/// sender's channel capability instead and crosses processes by design.
const FusedWakeSender = enum { same_process, capability_checked };

fn tryFusedKeyedWakeLocked(
    comptime install: anytype,
    comptime sender_scope: FusedWakeSender,
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
    const sender = threadContextLocked(sender_id) orelse return null;
    const peer = threadContextLocked(thread_index) orelse return null;
    if (!sender.allocated or sender.cpu_slot != cpu or !sender.ready or sender.stopped) return null;
    if (sender_scope == .same_process and sender.owner_process != owner) return null;
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
        const accounting = prepareRuntimeCharge(state, source, 0) orelse break :commit false;
        var running: verified_sched.Entity = undefined;
        var finished_source: verified_sched.Entity = undefined;
        if (scalarIssue("fused-wake", verified_sched.pacha_eevdf_entity_wake(&target.entity, cpuRunqueueFloor(state), &awakened), thread_index) or
            scalarIssue("fused-run", verified_sched.pacha_eevdf_entity_mark_running(&awakened, &running), thread_index) or
            scalarIssue("fused-finish", verified_sched.pacha_eevdf_entity_finish(&accounting.entity, &finished_source), sender_id)) break :commit false;
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
        state.runtime_accounting.commit(accounting.interval);
        state.current_entity = target;
        if (comptime ipc_metric.enabled) @import("notify_metric.zig").claimed(&peer.notify_stamp.stamp, thread_index, generation, @intFromEnum(owner), wait_token, last_cpu, peer.notify_stamp.direct_token == wait_token);
        peer.cpu_slot = cpu;
        peer.frame.rax = resume_rax;
        peer.wait_uninterruptible = false;
        peer.wait_mailbox = false;
        peer.active_wait_token = 0;
        setWakeTickLocked(peer, 0);
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
    const source = threadContextLocked(sender_id) orelse return false;
    const target = threadContextLocked(target_id) orelse return false;
    if (!source.allocated or source.cpu_slot != cpu or source.owner_process != owner or
        !target.allocated or target.owner_process != owner or target.generation != generation or
        !target.ready or target.cpu_slot != cpu or !threadAllowsCpu(target, cpu)) return false;
    const sender_generation = source.generation;
    if (!verifiedHandoffToThreadOnCpuLocked(true, cpu, target_id, generation, sender_id, sender_generation)) return false;
    if (install(target_id, generation, frame, sender_rax)) return true;
    rollbackLocalHandoffLocked(cpu, sender_id, sender_generation, target_id, generation);
    return false;
}

fn rollbackLocalHandoffLocked(cpu: usize, source_id: usize, source_generation: u32, target_id: usize, target_generation: u32) void {
    // A wake has already committed. If its context transfer fails, preserve
    // that wake as runnable, but never report success with uncertain ownership.
    // These checks must remain active in release builds, not debug asserts.
    if (!verifiedHandoffToThreadOnCpuLocked(false, cpu, source_id, source_generation, target_id, target_generation))
        @panic("keyed wake handoff ownership rollback failed");
    const state = schedulerStateForSlot(cpu) orelse @panic("keyed wake rollback lost CPU");
    const source = threadContextLocked(source_id) orelse @panic("keyed wake rollback lost source");
    const target = threadContextLocked(target_id) orelse @panic("keyed wake rollback lost target");
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
    setTestCurrentThread(&scheduler_state.cpus[0], 4);
    scheduler_state.cpus[0].current_principal = source.owner_process;
    scheduler_state.cpus[0].current_cr3 = source.cr3;
    scheduler_state.cpus[0].is_idle = false;
    const peer = &initial_thread_contexts[5];
    peer.* = .{
        .id = 5,
        .generation = 17,
        .allocated = true,
        .cpu_slot = peer_cpu,
        .cpu_affinity = testAffinity(3),
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

test "IPC notify completes another process's waiter and runs it in one transition" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    try initializeFusedWakeTest(0);
    // The sender is the storage owner; the waiter is the client's thread.
    const sender_process = kernel.processPrincipalFromIndex(1).?;
    initial_thread_contexts[4].owner_process = sender_process;
    syncHotStateFromContext(4);
    scheduler_state.cpus[0].current_principal = sender_process;
    var frame = std.mem.zeroes(TrapFrame);
    frame.rip = 0x6000;

    // A futex key is process-local: the same fusion must refuse this waker.
    scheduler_state.thread_table.lock();
    const futex_scope = tryFusedKeyedWakeLocked(FusedWakeTestInstaller.install, .same_process, &frame, 5, 17, default_process_principal, 11, 0, 1);
    scheduler_state.thread_table.unlock();
    try std.testing.expectEqual(@as(?bool, null), futex_scope);
    try std.testing.expectEqual(@as(u64, 11), initial_thread_contexts[5].active_wait_token);
    try std.testing.expectEqual(.blocked, initial_scheduler_entities[5].ownership);

    const result = completeIpcNotifyWaiterAndHandoffWithInstall(FusedWakeTestInstaller.install, &frame, 5, 17, default_process_principal, 11, 0, 3);
    try std.testing.expectEqual(IpcNotifyWakeResult.handed_off, result);
    try std.testing.expectEqual(@as(usize, 1), FusedWakeTestInstaller.calls);
    try std.testing.expectEqual(@as(usize, 5), currentThread());
    try std.testing.expectEqual(@as(u64, 0x8888), frame.rip);
    try std.testing.expectEqual(@as(u64, 0), frame.rax);
    const peer = &initial_thread_contexts[5];
    try std.testing.expect(peer.ready and !peer.wait_mailbox and peer.active_wait_token == 0 and peer.wake_tick == 0);
    try std.testing.expectEqual(.running, initial_scheduler_entities[5].ownership);
    // The sender stays runnable on this CPU, with its own syscall result.
    try std.testing.expectEqual(.runnable, initial_scheduler_entities[4].ownership);
    try std.testing.expect(scheduler_state.cpus[0].runqueue.contains(&initial_scheduler_entities[4]));
    try std.testing.expectEqual(@as(u64, 3), initial_thread_contexts[4].frame.rax);
    try std.testing.expectEqual(@as(u64, 0x6000), initial_thread_contexts[4].frame.rip);
}

test "IPC notify falls back to the ordinary completion when it cannot fuse" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    var frame = std.mem.zeroes(TrapFrame);
    frame.rip = 0x6000;

    // A different wait: nothing is taken or changed.
    try initializeFusedWakeTest(0);
    const before = initial_thread_contexts[5];
    try std.testing.expectEqual(IpcNotifyWakeResult.stale, completeIpcNotifyWaiterAndHandoffWithInstall(FusedWakeTestInstaller.install, &frame, 5, 17, default_process_principal, 12, 0, 3));
    try std.testing.expectEqual(@as(usize, 0), FusedWakeTestInstaller.calls);
    try expectWakeWaitContextUnchanged(before, initial_thread_contexts[5]);
    try std.testing.expectEqual(@as(u64, 0x6000), frame.rip);

    // A stopped waiter is not run: it completes and resumes after CONT.
    try initializeFusedWakeTest(0);
    initial_thread_contexts[5].stopped = true;
    syncHotStateFromContext(5);
    try std.testing.expectEqual(IpcNotifyWakeResult.published, completeIpcNotifyWaiterAndHandoffWithInstall(FusedWakeTestInstaller.install, &frame, 5, 17, default_process_principal, 11, 0, 3));
    try std.testing.expectEqual(@as(usize, 0), FusedWakeTestInstaller.calls);
    try std.testing.expectEqual(@as(usize, 4), currentThread());
    try std.testing.expect(initial_thread_contexts[5].resume_after_stop);
    try std.testing.expectEqual(@as(u64, 0), initial_thread_contexts[5].active_wait_token);

    // A failed install keeps the completion: the waiter is runnable here and
    // the sender still owns the CPU and its frame.
    try initializeFusedWakeTest(0);
    FusedWakeTestInstaller.fail = true;
    try std.testing.expectEqual(IpcNotifyWakeResult.published, completeIpcNotifyWaiterAndHandoffWithInstall(FusedWakeTestInstaller.install, &frame, 5, 17, default_process_principal, 11, 0, 3));
    try std.testing.expectEqual(@as(usize, 1), FusedWakeTestInstaller.calls);
    try std.testing.expectEqual(@as(usize, 4), currentThread());
    try std.testing.expectEqual(@as(u64, 0x6000), frame.rip);
    try std.testing.expect(initial_thread_contexts[5].ready and initial_thread_contexts[5].active_wait_token == 0);
    try std.testing.expectEqual(.runnable, initial_scheduler_entities[5].ownership);
    try std.testing.expect(scheduler_state.cpus[0].runqueue.contains(&initial_scheduler_entities[5]));
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

fn initializeDirectBlockTest(local_runnable: bool) !void {
    initializeStaticStorage();
    for (4..6) |index| {
        initial_thread_contexts[index] = .{ .id = @intCast(index), .scheduler_entity = &initial_scheduler_entities[index] };
        syncHotStateFromContext(index);
    }
    const source = &initial_thread_contexts[4];
    source.* = .{ .id = 4, .allocated = true, .ready = true, .cr3 = 0x1000, .scheduler_entity = &initial_scheduler_entities[4] };
    try std.testing.expect(verifiedAddThread(4, 1, true));
    try std.testing.expectEqual(@as(usize, 4), verifiedPickThreadForCpu(0).?.thread_index);
    setTestCurrentThread(&scheduler_state.cpus[0], 4);
    scheduler_state.cpus[0].is_idle = false;
    const next = &initial_thread_contexts[5];
    next.* = .{ .id = 5, .generation = 17, .allocated = true, .ready = true, .cr3 = 0x2000, .fs_base = 0x3333, .gs_base = 0x4444, .scheduler_entity = &initial_scheduler_entities[5] };
    next.frame.rip = 0x8888;
    next.frame.rax = 77;
    if (local_runnable) try std.testing.expect(verifiedAddThread(5, 17, true));
    syncHotStateFromContext(4);
    syncHotStateFromContext(5);
    ContextTransferTestRegisters.reset();
}

test "AP block installs the next local runnable thread without recapturing the source" {
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    try initializeDirectBlockTest(true);
    initial_thread_contexts[4].ready = false;
    syncHotStateFromContext(4);
    verifiedBlockThread(4);
    scheduler_state.cpus[0].deferred_slice_ticks = 3;
    var frame = std.mem.zeroes(TrapFrame);
    frame.rip = 0x9999;
    // The blocking save read these from hardware; GS already matches the
    // next thread, FS and PKRU do not.
    const Registers = ContextTransferTestRegisters;
    Registers.gs = 0x4444;
    const live = LiveBases{ .fs = Registers.fs, .gs = Registers.gs, .pkru = Registers.pkru };
    scheduler_state.thread_table.lock();
    const installed = installNextLocalAfterBlockLocked(Registers, 0, &frame, live, null);
    scheduler_state.thread_table.unlock();
    try std.testing.expect(installed);
    try std.testing.expectEqual(@as(usize, 5), currentThread());
    try std.testing.expect(!scheduler_state.cpus[0].is_idle);
    try std.testing.expectEqual(@as(u64, 0x2000), scheduler_state.cpus[0].current_cr3);
    try std.testing.expectEqual(@as(u64, 0x8888), frame.rip);
    try std.testing.expectEqual(@as(u64, 77), frame.rax);
    try std.testing.expectEqual(.running, initial_scheduler_entities[5].ownership);
    try std.testing.expectEqual(.blocked, initial_scheduler_entities[4].ownership);
    // The source was saved before it blocked, so nothing is read again; only
    // the fields that differ from the live values are written.
    try std.testing.expectEqual([3]usize{ 0, 0, 0 }, Registers.reads);
    try std.testing.expectEqual([3]usize{ 1, 0, 1 }, Registers.writes);
    try std.testing.expectEqual(@as(u64, 0x3333), Registers.fs);
    try std.testing.expectEqual(@as(u64, 0x4444), Registers.gs);
    try std.testing.expectEqual(@as(u32, 0), Registers.pkru);
    // The shadow names the installed thread, so its user return writes nothing.
    try std.testing.expect(applyThreadBasesShadowed(Registers, 0, 5));
    try std.testing.expectEqual([3]usize{ 1, 0, 1 }, Registers.writes);
    // The blocked syscall's deferred ticks are not charged to the new thread.
    try std.testing.expectEqual(@as(u64, 0), scheduler_state.cpus[0].deferred_slice_ticks);
    // A stop clearing ready while it is still current must not skip its XRSTOR.
    initial_thread_contexts[5].ready = false;
    syncHotStateFromContext(5);
    try std.testing.expectEqual(&initial_thread_contexts[5], currentContextForXState().?);
}

test "AP block falls back to idle when nothing local can run" {
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    // Empty local runqueue.
    try initializeDirectBlockTest(false);
    verifiedBlockThread(4);
    var frame = std.mem.zeroes(TrapFrame);
    frame.rip = 0x9999;
    scheduler_state.thread_table.lock();
    try std.testing.expect(!installNextLocalAfterBlockLocked(ContextTransferTestRegisters, 0, &frame, null, null));
    scheduler_state.thread_table.unlock();
    try std.testing.expectEqual(@as(u64, 0x9999), frame.rip);
    try std.testing.expect(scheduler_state.cpus[0].current_entity == null);
    // The source failed to leave the running state: nothing may be picked.
    try initializeDirectBlockTest(true);
    scheduler_state.thread_table.lock();
    try std.testing.expect(!installNextLocalAfterBlockLocked(ContextTransferTestRegisters, 0, &frame, null, null));
    scheduler_state.thread_table.unlock();
    try std.testing.expectEqual(.running, initial_scheduler_entities[4].ownership);
    try std.testing.expectEqual(.runnable, initial_scheduler_entities[5].ownership);
    try std.testing.expectEqual(@as(u64, 0x9999), frame.rip);
}

test "AP block rolls back the pick when the picked context cannot be activated" {
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    try initializeDirectBlockTest(true);
    verifiedBlockThread(4);
    // The verified pick validates the context; a disabled CPU makes only the
    // activation fail, after the entity was already marked running.
    scheduler_state.cpus[0].enabled = false;
    var frame = std.mem.zeroes(TrapFrame);
    frame.rip = 0x9999;
    scheduler_state.thread_table.lock();
    try std.testing.expect(!installNextLocalAfterBlockLocked(ContextTransferTestRegisters, 0, &frame, null, null));
    scheduler_state.thread_table.unlock();
    try std.testing.expectEqual(.runnable, initial_scheduler_entities[5].ownership);
    try std.testing.expect(scheduler_state.cpus[0].current_entity == null);
    try std.testing.expectEqual(@as(u64, 0x9999), frame.rip);
}

test "AP block after STOP keeps CONT blocked until the wait completes" {
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    const owner = kernel.processPrincipalFromIndex(7).?;
    for (0..4) |completion| {
        try initializeDirectBlockTest(false);
        const ctx = &initial_thread_contexts[4];
        ctx.owner_process = owner;
        syncHotStateFromContext(4);
        try std.testing.expectEqual(@as(usize, 1), stopPrincipalThreads(owner));
        try std.testing.expect(ctx.stopped and ctx.resume_after_stop);

        // The syscall passed admission before STOP, but commits its wait
        // afterwards. Both AP and BSP use this same blocked-state commit.
        scheduler_state.thread_table.lock();
        ctx.frame.rax = 99;
        ctx.wait_mailbox = completion != 2;
        ctx.active_wait_token = if (completion <= 1) 13 else 0;
        setWakeTickLocked(ctx, if (completion == 2) 10 else 0);
        commitThreadBlockedLocked(ctx);
        setThreadHotWaitState(4, ctx.wait_mailbox, ctx.wake_tick, false);
        noteCpuIdleTick(0);
        scheduler_state.thread_table.unlock();
        try std.testing.expect(!ctx.ready and !ctx.resume_after_stop);

        if (completion == 0) {
            try std.testing.expectEqual(@as(usize, 1), continuePrincipalThreads(owner));
            try std.testing.expect(!ctx.ready and ctx.wait_mailbox and ctx.active_wait_token == 13);
            try std.testing.expectEqual(.blocked, initial_scheduler_entities[4].ownership);
            try std.testing.expectEqual(.published, claimAndPublishWaitCompletionPreferred(4, 1, owner, 13, 77));
        } else {
            if (completion == 1) {
                try std.testing.expectEqual(.published, claimAndPublishWaitCompletionPreferred(4, 1, owner, 13, 77));
            } else if (completion == 2) {
                wakeExpiredTimers(10);
            } else {
                try std.testing.expect(wakeIfWaiting(4));
            }
            try std.testing.expect(ctx.stopped and !ctx.ready and ctx.resume_after_stop);
            try std.testing.expectEqual(@as(usize, 1), continuePrincipalThreads(owner));
        }
        try std.testing.expect(ctx.ready and !ctx.stopped and !ctx.resume_after_stop);
        try std.testing.expect(!ctx.wait_mailbox and ctx.active_wait_token == 0 and ctx.wake_tick == 0);
        try std.testing.expectEqual(.runnable, initial_scheduler_entities[4].ownership);
        try std.testing.expectEqual(@as(u64, if (completion <= 1) 77 else 99), ctx.frame.rax);
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
        initial_thread_contexts[5].signal_delivery_ready = true;
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
                source.signal_delivery_ready = true;
            },
            5 => source.stopped = true,
            6 => source.owner_process = kernel.processPrincipalFromIndex(1).?,
            7 => peer.generation += 1,
            8 => peer.active_wait_token += 1,
            9 => peer.wait_completion_claimed = true,
            10 => peer.stopped = true,
            11 => peer.ready = true,
            12 => peer.wait_mailbox = false,
            13 => peer.cpu_affinity = testAffinity(2),
            14 => {
                scheduler_state.cpus[1].is_idle = false;
                setTestCurrentThread(&scheduler_state.cpus[1], 5);
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
            7 => peer.cpu_affinity = testAffinity(2),
            8 => {
                scheduler_state.cpus[1].is_idle = false;
                setTestCurrentThread(&scheduler_state.cpus[1], 5);
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
        const result = tryFusedKeyedWakeLocked(FusedWakeTestInstaller.install, .same_process, &frame, 5, 17, default_process_principal, 11, 0, 1);
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
        peer.cpu_affinity = testAffinity(1);
        peer.ready = true;
        try std.testing.expect(verifiedWakeThreadGeneration(5, 17));
        syncHotStateFromContext(5);
        // Model a change after ordinary publication but before TT reacquire.
        switch (scenario) {
            0 => {},
            1 => peer.generation += 1,
            2 => peer.owner_process = kernel.processPrincipalFromIndex(1).?,
            3 => peer.cpu_slot = 1,
            4 => peer.cpu_affinity = testAffinity(2),
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
    return claimWaitTokenCompletionLocked(thread_index, generation, owner, wait_token);
}

/// Claim and publish under one thread-table hold, preferring this CPU. For a
/// completion with no KernelState work between the two (a notification
/// carries no message to copy out), taking the table twice bought nothing.
pub fn claimAndPublishWaitCompletionPreferred(
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
) ClaimedWaitPublishResult {
    if (wait_token == 0) return .stale;
    const cpu = currentCpu();
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    if (claimWaitTokenCompletionLocked(thread_index, generation, owner, wait_token) != .claimed) return .stale;
    return publishClaimedWaitLocked(thread_index, generation, owner, wait_token, resume_rax, cpu, null);
}

fn claimWaitTokenCompletionLocked(
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
) WaitTokenClaimResult {
    const ctx = threadContextLocked(thread_index) orelse return .stale;
    if (!ctx.allocated or ctx.owner_process != owner or
        ctx.generation != generation or ctx.active_wait_token != wait_token or
        !ctx.wait_mailbox or ctx.wait_completion_claimed)
    {
        return .stale;
    }
    pendIpcBindingLocked(&ctx.ipc_receiving);
    ctx.wait_completion_claimed = true;
    ctx.wait_completion_publish_pending = false;
    if (comptime ipc_metric.enabled) @import("notify_metric.zig").claimed(&ctx.notify_stamp.stamp, thread_index, generation, @intFromEnum(owner), wait_token, ctx.cpu_slot, ctx.notify_stamp.direct_token == wait_token);
    // Claim is the completion linearization point.  Timeout and signal paths
    // must no longer be able to win while KernelState cleanup/copyout runs.
    setWakeTickLocked(ctx, 0);
    setThreadHotWaitState(thread_index, true, 0, ctx.ready);
    setThreadHotCompletionClaimed(thread_index, false);
    wake_trace.record(.wait_claim, @intFromEnum(owner), wait_token, thread_index);
    return .claimed;
}

fn publishClaimedWaitLocked(
    thread_index: usize,
    generation: u32,
    owner: kernel.PrincipalId,
    wait_token: u64,
    resume_rax: u64,
    preferred_cpu: ?usize,
    deferred_notify_cpu: ?*?usize,
) ClaimedWaitPublishResult {
    const ctx = threadContextLocked(thread_index) orelse return .stale;
    if (!ctx.allocated or ctx.owner_process != owner or
        ctx.generation != generation or ctx.active_wait_token != wait_token or
        !ctx.wait_completion_claimed)
    {
        return .stale;
    }
    if (ctx.wait_ipc_words) |words| {
        @import("kernel_abi_root").ipc_abi.outputWords(&ctx.frame, if (resume_rax == 0) words else .{ 0, 0, 0, 0 });
        ctx.frame.r15 = if (resume_rax == 0) ctx.wait_ipc_fd_count else 0;
    }
    if (ctx.wait_poll_result) |result| {
        ctx.frame.r10 = result.index;
        ctx.frame.r8 = result.revents;
    }
    ctx.wait_completion_rax = resume_rax;
    if (ctx.ready) {
        ctx.frame.rax = resume_rax;
        ctx.wait_uninterruptible = false;
        ctx.wait_mailbox = false;
        setWakeTickLocked(ctx, 0);
        ctx.wait_completion_claimed = false;
        ctx.wait_completion_publish_pending = false;
        setThreadHotWaitState(thread_index, false, 0, true);
        setThreadHotCompletionClaimed(thread_index, false);
        return .published;
    }

    ctx.frame.rax = resume_rax;
    ctx.wait_uninterruptible = false;
    ctx.wait_mailbox = false;
    ctx.active_wait_token = 0;
    setWakeTickLocked(ctx, 0);
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
    if (!verifiedWakeThreadGenerationPreferred(thread_index, generation, preferred_cpu, deferred_notify_cpu)) {
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
        null,
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
    while (true) : (i += 1) {
        // Owner selection and wake share one hold: an index cannot be reused
        // for another principal between the filter and its state transition.
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        if (i >= threadDirectory().contexts.len) break;
        const hot = getThreadHotStateConst(i) orelse continue;
        if (hot.allocated == 0 or hot.owner_process != principal) continue;
        if (hot.wait_mailbox == 0) continue;
        if (wakeIfWaitingLocked(i)) return;
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

// Host fixtures observe the actual publication point without executing CR3
// and physical-window instructions. This branch/state is absent in kernel ELF.
pub const SignalHintTest = struct {
    pub var copy: ?*const fn (kernel.PrincipalId, u64, []const u8) bool = null;
};

inline fn copyPendingSignalHint(principal: kernel.PrincipalId, address: u64, bytes: []const u8) void {
    if (builtin.is_test) {
        if (SignalHintTest.copy) |copy| {
            _ = copy(principal, address, bytes);
            return;
        }
    }
    _ = user_copy.copyBytesToUserVa(principal, address, bytes);
}

fn publishPendingSignalHint(principal: kernel.PrincipalId) void {
    const hint_va = kernel_runtime.kernel_state_global.processSignalPendingHintVa(principal);
    if (hint_va == 0) return;
    var pending: u32 = 1;
    copyPendingSignalHint(principal, hint_va, std.mem.asBytes(&pending));
}

pub fn deliverSignal(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    process_generation: u32,
    signo: u32,
    blocked_resume_rax: u64,
) ?SignalDelivery {
    // Caller retains KernelState from this descriptor snapshot through hint
    // copyout. T alone cannot exclude principal reuse once delivery drops T.
    const descriptor = state.processDescriptor(principal) orelse return null;
    if (process_generation == 0 or descriptor.generation != process_generation) return null;
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
    while (i < threadDirectory().contexts.len) : (i += 1) {
        const ctx = threadContextLocked(i) orelse continue;
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
    const ctx = threadContextLocked(target) orelse {
        scheduler_state.thread_table.unlock();
        return null;
    };
    return deliverSignalLocked(state, process_generation, principal, target, ctx, signal_bit, blocked_resume_rax);
}

// Thread capabilities name one generation. Publication and blocked-wait
// cancellation use the same lock as process-directed notification.
pub fn deliverThreadSignal(principal: kernel.PrincipalId, target: usize, generation: u32, signo: u32, blocked_resume_rax: u64) ?SignalDelivery {
    const signal_bit = signalBit(signo) orelse return null;
    scheduler_state.thread_table.lock();
    const ctx = threadContextLocked(target) orelse {
        scheduler_state.thread_table.unlock();
        return null;
    };
    if (ctx.generation != generation) {
        scheduler_state.thread_table.unlock();
        return null;
    }
    return deliverSignalLocked(kernel_runtime.kernel_state_global, null, principal, target, ctx, signal_bit, blocked_resume_rax);
}

fn interruptIpcWaitLocked(ctx: *ThreadContext, blocked_resume_rax: u64) void {
    // The IPC cancellation has effects outside T. Once claimed, failed
    // runqueue publication must retry the completion rather than restore
    // an ordinary wait whose call body is already pending cancellation.
    const token = ctx.active_wait_token;
    const claim = claimWaitTokenCompletionLocked(ctx.id, ctx.generation, ctx.owner_process, token);
    if (claim != .claimed) @panic("IPC signal lost completion claim under T");
    pendIpcBindingLocked(&ctx.ipc_outgoing);
    pendIpcBindingLocked(&ctx.ipc_receiving);
    _ = publishClaimedWaitLocked(ctx.id, ctx.generation, ctx.owner_process, token, blocked_resume_rax, null, null);
}

fn deliverSignalLocked(state: *kernel.KernelState, process_generation: ?u32, principal: kernel.PrincipalId, target: usize, ctx: *ThreadContext, signal_bit: u64, blocked_resume_rax: u64) ?SignalDelivery {
    if (!ctx.allocated or ctx.owner_process != principal or ctx.stopped) {
        scheduler_state.thread_table.unlock();
        return null;
    }
    const newly_pending = (ctx.pending_signal_mask & signal_bit) == 0;
    // A child that inherited a scratch trampoline must finish registering
    // before a signal can interrupt its bootstrap wait. Threads with no
    // trampoline retain native wait-interruption semantics.
    const should_interrupt = newly_pending and (ctx.signal_entry == 0 or ctx.signal_delivery_ready) and (ctx.signal_blocked_mask & signal_bit) == 0;
    const was_blocked = should_interrupt and !ctx.ready and !ctx.wait_uninterruptible and
        (ctx.wait_mailbox or ctx.wake_tick != 0) and
        !ctx.wait_completion_claimed;
    const generation = ctx.generation;
    ctx.pending_signal_mask |= signal_bit;
    // Pending survives an internal wait; only its ordinary completion may wake it.
    setThreadHotPendingSignalMask(target, ctx.pending_signal_mask);
    if (was_blocked and (ctx.ipc_outgoing != null or ctx.ipc_receiving != null)) {
        interruptIpcWaitLocked(ctx, blocked_resume_rax);
    } else if (was_blocked) {
        // Token invalidation and the blocked/runnable transition share this
        // lock with event completion claim.  Exactly one of signal, timeout,
        // or FD completion can own the wait.
        ctx.frame.rax = blocked_resume_rax;
        ctx.wait_uninterruptible = false;
        ctx.wait_mailbox = false;
        ctx.active_wait_token = 0;
        setWakeTickLocked(ctx, 0);
        ctx.wait_completion_claimed = false;
        ctx.wait_completion_publish_pending = false;
        ctx.wait_completion_rax = 0;
        ctx.ready = true;
        setThreadHotWaitState(target, false, 0, true);
        setThreadHotCompletionClaimed(target, false);
        _ = verifiedWakeThreadGeneration(target, generation);
    }
    const cpu_slot = ctx.cpu_slot;
    const thread_hint_va = ctx.signal_pending_hint_va;
    scheduler_state.thread_table.unlock();
    // Publish only after dropping thread_table: the copyout may take the
    // target's address-space lock, break COW and wait for TLB-shootdown acks,
    // none of which may run while other CPUs spin on this lock with IRQs off.
    // verifiedWake above can already make the target runnable before this
    // hint publication. Neither copyout nor the later IPI orders an earlier
    // hint read. Interruptible wait admission therefore checks the true pending
    // mask under T; the hint is never the progress guarantee.
    if (process_generation) |expected| {
        // Revalidate at hint lookup as well; K keeps this identity stable while
        // copyout acquires AS. Never acquire AS while T is held.
        if (state.processDescriptor(principal)) |descriptor| {
            if (descriptor.generation == expected and descriptor.signal_pending_hint_va != 0) {
                var pending: u32 = 1;
                copyPendingSignalHint(principal, descriptor.signal_pending_hint_va, std.mem.asBytes(&pending));
            }
        }
    } else {
        publishPendingSignalHint(principal);
    }
    // Only the target thread clears this word. The LPR slot storage remains
    // mapped until process teardown; a late 1 after thread exit is harmless
    // even if the slot has been reused by a later thread.
    if (thread_hint_va != 0) {
        var pending: u32 = 1;
        copyPendingSignalHint(principal, thread_hint_va, std.mem.asBytes(&pending));
    }
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
    scratch_base: u64,
    scratch_size: u64,
};

pub fn configureCurrentSignalDelivery(
    entry: u64,
    inhibit_start: u64,
    inhibit_end: u64,
    inhibit_secondary_start: u64,
    inhibit_secondary_end: u64,
    scratch_base: u64,
    scratch_size: u64,
    process_owner: bool,
) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(currentThread()) orelse return false;
    if (!ctx.allocated) return false;
    ctx.signal_entry = entry;
    ctx.signal_scratch_base = scratch_base;
    ctx.signal_scratch_size = scratch_size;
    ctx.signal_delivery_ready = true;
    ctx.signal_inhibit_start = inhibit_start;
    ctx.signal_inhibit_end = inhibit_end;
    ctx.signal_inhibit_secondary_start = inhibit_secondary_start;
    ctx.signal_inhibit_secondary_end = inhibit_secondary_end;
    // Per-thread scratch registration must not make every worker the process
    // signal owner. Only the runtime's designated main/event-loop opts in.
    ctx.signal_owner = process_owner;
    return true;
}

// Only this thread can replace its registration. Snapshot under T, then
// probe/copy without T: user memory faults may acquire address-space locks.
pub fn currentSignalStack() ?struct { base: u64, size: u64 } {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(currentThread()) orelse return null;
    if (!ctx.allocated or !ctx.signal_delivery_ready) return null;
    return .{ .base = ctx.signal_scratch_base, .size = ctx.signal_scratch_size };
}

pub fn setCurrentSignalBlockedMask(blocked_mask: u64) bool {
    scheduler_state.thread_table.lock();
    const ctx = threadContextLocked(currentThread()) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    if (!ctx.allocated or ctx.signal_entry == 0) {
        scheduler_state.thread_table.unlock();
        return false;
    }
    ctx.signal_blocked_mask = blocked_mask;
    const should_publish = firstUnblockedPendingSignal(ctx) != 0;
    const principal = ctx.owner_process;
    const thread_hint_va = ctx.signal_pending_hint_va;
    scheduler_state.thread_table.unlock();
    if (should_publish) {
        publishPendingSignalHint(principal);
        if (thread_hint_va != 0) {
            var pending: u32 = 1;
            copyPendingSignalHint(principal, thread_hint_va, std.mem.asBytes(&pending));
        }
    }
    return true;
}

pub fn setCurrentSignalPendingHint(hint_va: u64) bool {
    scheduler_state.thread_table.lock();
    const ctx = threadContextLocked(currentThread()) orelse {
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

pub fn setCurrentThreadSignalPendingHint(hint_va: u64) bool {
    scheduler_state.thread_table.lock();
    const ctx = threadContextLocked(currentThread()) orelse {
        scheduler_state.thread_table.unlock();
        return false;
    };
    if (!ctx.allocated or ctx.signal_entry == 0) {
        scheduler_state.thread_table.unlock();
        return false;
    }
    ctx.signal_pending_hint_va = hint_va;
    const pending = firstUnblockedPendingSignal(ctx) != 0;
    const principal = ctx.owner_process;
    scheduler_state.thread_table.unlock();
    // User copy may fault and acquire AS; never perform it with T held.
    if (pending and hint_va != 0) {
        var value: u32 = 1;
        copyPendingSignalHint(principal, hint_va, std.mem.asBytes(&value));
    }
    return true;
}

pub fn copyCurrentThreadStateToGeneration(target_thread: usize, generation: u32, owner: kernel.PrincipalId) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const source = pinnedContextForCpu(currentCpu()) orelse return false;
    const target = matchingThreadLocked(target_thread, generation, owner) orelse return false;
    if (threadSlotReferencedByCpu(target_thread)) return false;
    target.signal_entry = source.signal_entry;
    target.signal_scratch_base = 0;
    target.signal_scratch_size = 0;
    target.signal_delivery_ready = source.signal_delivery_ready and source.signal_scratch_size == 0;
    target.signal_blocked_mask = source.signal_blocked_mask;
    target.signal_inhibit_start = source.signal_inhibit_start;
    target.signal_inhibit_end = source.signal_inhibit_end;
    target.signal_inhibit_secondary_start = source.signal_inhibit_secondary_start;
    target.signal_inhibit_secondary_end = source.signal_inhibit_secondary_end;
    target.signal_owner = false;
    // The child (thread or forked process) must register its own slot. A VA
    // copied from the parent could be retired or reused before child startup.
    target.signal_pending_hint_va = 0;
    target.x_state = source.x_state;
    target.pkru = source.pkru;
    return true;
}

pub fn claimCurrentSignalForUserReturn(rip: u64) ?ClaimedSignal {
    const thread_index = currentThread();
    scheduler_state.thread_table.lock();
    const ctx = threadContextLocked(thread_index) orelse {
        scheduler_state.thread_table.unlock();
        return null;
    };
    const signo = firstUnblockedPendingSignal(ctx);
    if (!ctx.allocated or signo == 0 or ctx.signal_entry == 0 or !ctx.signal_delivery_ready or ctx.active_fault_frame != 0) {
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
        .scratch_base = ctx.signal_scratch_base,
        .scratch_size = ctx.signal_scratch_size,
    };
    ctx.pending_signal_mask &= ~signalBit(signo).?;
    setThreadHotPendingSignalMask(thread_index, ctx.pending_signal_mask);
    const should_publish = firstUnblockedPendingSignal(ctx) != 0;
    const principal = ctx.owner_process;
    scheduler_state.thread_table.unlock();
    if (should_publish) publishPendingSignalHint(principal);
    return claimed;
}

pub fn restoreClaimedSignal(claimed: ClaimedSignal) void {
    scheduler_state.thread_table.lock();
    const ctx = threadContextLocked(claimed.thread_index) orelse {
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
    setThreadHotPendingSignalMask(claimed.thread_index, ctx.pending_signal_mask);
    const principal = ctx.owner_process;
    scheduler_state.thread_table.unlock();
    publishPendingSignalHint(principal);
}

pub fn copyCurrentSignalXState(out: *[xstate_bytes]u8) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(currentThread()) orelse return false;
    if (!ctx.allocated) return false;
    out.* = ctx.x_state;
    return true;
}

pub fn restoreCurrentSignalXState(saved: *const [xstate_bytes]u8) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(currentThread()) orelse return false;
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
    const ctx = threadContextLocked(currentThread()) orelse return false;
    if (!ctx.allocated or ctx.active_fault_frame != 0) return false;
    ctx.fault_entry = entry;
    ctx.fault_stack_base = stack_base;
    ctx.fault_stack_size = stack_size;
    return true;
}

pub fn claimCurrentFault() ?FaultDelivery {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(currentThread()) orelse return null;
    if (!ctx.allocated or ctx.fault_entry == 0 or ctx.active_fault_frame != 0) return null;
    const frame_va = (ctx.fault_stack_base + ctx.fault_stack_size - process_abi.fault_frame_size) & ~@as(u64, 63);
    ctx.active_fault_frame = frame_va;
    return .{ .entry = ctx.fault_entry, .frame_va = frame_va };
}

pub fn completeCurrentFault(frame_va: u64) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(currentThread()) orelse return false;
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
    while (i < threadDirectory().contexts.len) : (i += 1) {
        const ctx = threadContextLocked(i) orelse continue;
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
    while (i < threadDirectory().contexts.len) : (i += 1) {
        const ctx = threadContextLocked(i) orelse continue;
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
        while (i < threadDirectory().contexts.len) : (i += 1) {
            const ctx = threadContextLocked(i) orelse continue;
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
    if (kernel_runtime.kernel_state_global.processDescriptor(principal)) |descriptor|
        @import("state/process_timer.zig").clear(kernel_runtime.kernel_state_global, principal, descriptor.generation);
    var released: usize = 0;
    var i: usize = 0;
    while (true) : (i += 1) {
        // Lifecycle callers are not the only users (exec staging also calls
        // here). Keep owner/generation selection and release indivisible.
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        if (i >= threadDirectory().contexts.len) break;
        const ctx = threadContextLocked(i) orelse continue;
        if (!ctx.allocated or ctx.owner_process != principal) continue;
        if (releaseThreadLocked(i)) released += 1;
    }
    reclaimThreadBodies();
    return released;
}

fn retryClaimedWaitCompletion(thread_index: usize) void {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    retryClaimedWaitCompletionLocked(thread_index);
}

fn retryClaimedWaitCompletionLocked(thread_index: usize) void {
    const ctx = threadContextLocked(thread_index) orelse return;
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
        null,
    );
}

// Host-only work counters do not add instructions or storage to the IRQ.
const TimerWork = struct {
    var visits: usize = 0;
    var holds: usize = 0;
    var retries: usize = 0;
};

pub fn wakeExpiredTimers(now_tick: u64) void {
    if (!isBootstrapSchedulerCpu()) return;
    var cursor: ?deadline_index.Key = null;
    while (true) {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        if (builtin.is_test) TimerWork.holds += 1;
        const node = scheduler_state.thread_table.deadlines.dueAfter(now_tick, cursor) orelse break;
        // Advance before attempting publication. Failure retains the deadline
        // for the next tick without hiding other expired waits this tick.
        cursor = node.key;
        const ctx: *ThreadContext = @alignCast(@fieldParentPtr("deadline_node", node));
        std.debug.assert(ctx.allocated and ctx.id == node.key.id and ctx.generation == node.key.generation and ctx.wake_tick == node.key.deadline);
        if (builtin.is_test) TimerWork.visits += 1;
        wakeIfTimerExpiredLocked(ctx.id, now_tick);
    }
}

pub fn retryPendingThreadMaintenance() void {
    if (!isBootstrapSchedulerCpu()) return;
    scheduler_state.thread_table.lock();
    const processing = scheduler_state.thread_table.retry_enqueue;
    scheduler_state.thread_table.retry_enqueue ^= 1;
    scheduler_state.thread_table.unlock();
    while (true) {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        const link = scheduler_state.thread_table.retries[processing].popFirst() orelse break;
        const ctx: *ThreadContext = @alignCast(@fieldParentPtr("retry_link", link));
        ctx.retry_queue = null;
        ctx.retry_link = .{};
        if (builtin.is_test) TimerWork.retries += 1;
        // Cleanup/copyout has already completed. Failed publication enqueues
        // on the other queue, never again on this pass's detached work.
        retryClaimedWaitCompletionLocked(ctx.id);
    }
    reclaimThreadBodies();
}

pub fn blockCurrentThread(
    frame: *TrapFrame,
    wait_mailbox: bool,
    wait_uninterruptible: bool,
    wait_token: u64,
    timeout_ticks: u64,
    resume_rax: u64,
    before_block: ?BeforeCurrentThreadLeaveCallback,
) bool {
    return blockCurrentThreadPreferred(frame, wait_mailbox, wait_uninterruptible, wait_token, timeout_ticks, resume_rax, before_block, null);
}

pub fn blockCurrentThreadPreferred(
    frame: *TrapFrame,
    wait_mailbox: bool,
    wait_uninterruptible: bool,
    wait_token: u64,
    timeout_ticks: u64,
    resume_rax: u64,
    before_block: ?BeforeCurrentThreadLeaveCallback,
    preferred: ?kernel.ThreadWakeTarget,
) bool {
    if (!isBootstrapSchedulerCpu()) {
        // AP user threads must leave the CPU for a real blocking receive.
        return parkApThreadForBlockPreferred(frame, wait_mailbox, wait_uninterruptible, wait_token, timeout_ticks, resume_rax, before_block, preferred);
    }
    const ipc_start = ipc_metric.timestamp();
    const ipc_direction = ipc_metric.syscallDirection(frame.rax);
    defer ipc_metric.phase(ipc_direction, .block, ipc_start);
    const current_thread = currentThread();
    scheduler_state.thread_table.lock();
    const ctx = threadContextLocked(current_thread) orelse {
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
            if (ctx.wait_poll_result) |result| {
                frame.r10 = result.index;
                frame.r8 = result.revents;
                ctx.wait_poll_result = null;
            }
            if (ctx.wait_ipc_words) |words| {
                @import("kernel_abi_root").ipc_abi.outputWords(frame, if (frame.rax == 0) words else .{ 0, 0, 0, 0 });
                frame.r15 = if (frame.rax == 0) ctx.wait_ipc_fd_count else 0;
                ctx.wait_ipc_words = null;
            }
            ctx.active_wait_token = 0;
            setWakeTickLocked(ctx, 0);
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
    if (firstUnblockedPendingSignal(ctx) != 0 and ctx.signal_entry != 0 and ctx.signal_delivery_ready and
        !wait_uninterruptible)
    {
        if (wait_token != 0) {
            pendIpcBindingLocked(&ctx.ipc_outgoing);
            pendIpcBindingLocked(&ctx.ipc_receiving);
            ctx.wait_uninterruptible = false;
            ctx.wait_mailbox = false;
            ctx.active_wait_token = 0;
        }
        scheduler_state.thread_table.unlock();
        return false;
    }

    const b3_save = b3.now();
    var saved = frame.*;
    saved.rax = resume_rax;
    ctx.frame = saved;
    ctx.entry_state.notify_sysret_continuation = false;
    ctx.cr3 = currentCr3();
    const live = captureLeavingBases(if (builtin.is_test) ContextTransferTestRegisters else x86_platform, currentCpu());
    ctx.fs_base = live.fs;
    ctx.gs_base = live.gs;
    ctx.pkru = live.pkru;
    b3.end(104, b3_save);
    ctx.wait_mailbox = wait_mailbox;
    ctx.active_wait_token = wait_token;
    ctx.wait_completion_claimed = false;
    ctx.wait_completion_publish_pending = false;
    ctx.wait_completion_rax = 0;
    setWakeTickLocked(ctx, if (timeout_ticks == 0) 0 else lapic_tick_count +| timeout_ticks);
    ctx.wait_uninterruptible = wait_uninterruptible;
    commitThreadBlockedLocked(ctx);
    setThreadHotCr3(current_thread, ctx.cr3);
    setThreadHotWaitState(current_thread, wait_mailbox, ctx.wake_tick, false);
    setThreadHotCompletionClaimed(current_thread, false);
    verifiedBlockThread(current_thread);
    if (builtin.is_test) {
        std.debug.assert(WaitAdmissionTest.stop_after_block);
        std.debug.assert(scheduler_state.thread_table.lock_state.value == 1);
        WaitAdmissionTest.stopped_at_block = true;
        noteCpuIdleTick(currentCpu());
        scheduler_state.thread_table.unlock();
        if (before_block) |callback| callback.run(callback.context);
        return true;
    }
    // Registration is armed and the source is blocked before handing off.
    // Keep T through the ownership transition, and drop K only after T.
    if (preferred != null and installNextLocalAfterBlockLocked(x86_platform, currentCpu(), frame, live, preferred)) {
        scheduler_state.thread_table.unlock();
        if (before_block) |callback| callback.run(callback.context);
        return true;
    }
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

test "thread storage generational operations reject reused identity and wrong owner" {
    const saved_contexts = try std.testing.allocator.dupe(ThreadContext, &initial_thread_contexts);
    defer std.testing.allocator.free(saved_contexts);
    const saved_hot = initial_thread_hot_states;
    defer {
        @memcpy(&initial_thread_contexts, saved_contexts);
        initial_thread_hot_states = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    var spaces = [_]UserAddressSpace{.{}};
    var table: UserAddressSpaceTable = .{};
    table.initializeFreshStorage(&spaces);
    var free_list: kernel.FreePageList = .{};
    const owner = default_process_principal;
    const frame = std.mem.zeroes(TrapFrame);
    const source = allocateSuspendedThread(owner, &table, frame, &free_list).?;
    const source_ctx = threadContextMutable(source.index).?;
    installCurrentContextLocked(&scheduler_state.cpus[0], source.index, source_ctx);
    source_ctx.signal_entry = 0x4000;
    source_ctx.signal_delivery_ready = true;
    try std.testing.expect(setCurrentThreadSignalPendingHint(0x5678));
    try std.testing.expectEqual(@as(u64, 0x5678), source_ctx.signal_pending_hint_va);
    source_ctx.x_state[160] = 0x73;
    source_ctx.pkru = 3;
    const old = allocateSuspendedThread(owner, &table, frame, &free_list).?;
    try std.testing.expect(releaseThreadGeneration(old.index, old.generation, owner));
    const fresh = allocateSuspendedThread(owner, &table, frame, &free_list).?;
    try std.testing.expectEqual(old.index, fresh.index);
    try std.testing.expect(old.generation != fresh.generation);
    const ctx = threadContextMutable(fresh.index).?;
    for (0..2) |scenario| {
        const gen = if (scenario == 0) old.generation else fresh.generation;
        const expected_owner = if (scenario == 0) owner else kernel.processPrincipalFromIndex(1).?;
        const before = ctx.*;
        const hot = initial_thread_hot_states[fresh.index];
        const node = ctx.scheduler_entity.?.*;
        try std.testing.expect(!markThreadReadyGeneration(fresh.index, gen, expected_owner, true));
        try std.testing.expect(!setThreadBasesGeneration(fresh.index, gen, expected_owner, 0x1000, 0x2000));
        try std.testing.expect(!copyCurrentThreadStateToGeneration(fresh.index, gen, expected_owner));
        try std.testing.expect(!releaseThreadGeneration(fresh.index, gen, expected_owner));
        var image: process_abi.ThreadUserContext = std.mem.zeroes(process_abi.ThreadUserContext);
        const saved_image = image;
        try std.testing.expect(!getControlledThreadContext(fresh.index, gen, expected_owner, &image));
        try std.testing.expectEqualDeep(saved_image, image);
        try std.testing.expect(!setControlledThreadContext(fresh.index, gen, expected_owner, &image));
        try std.testing.expect(deliverThreadSignal(expected_owner, fresh.index, gen, 2, 0) == null);
        try std.testing.expectEqualDeep(before, ctx.*);
        try std.testing.expectEqualDeep(hot, initial_thread_hot_states[fresh.index]);
        try std.testing.expectEqualDeep(node, ctx.scheduler_entity.?.*);
    }
    try std.testing.expect(setThreadBasesGeneration(fresh.index, fresh.generation, owner, 0x1000, 0x2000));
    try std.testing.expectEqual(@as(u64, 0x1000), ctx.fs_base);
    try std.testing.expectEqual(@as(u64, 0x2000), ctx.gs_base);
    try std.testing.expect(copyCurrentThreadStateToGeneration(fresh.index, fresh.generation, owner));
    try std.testing.expectEqual(source_ctx.x_state, ctx.x_state);
    try std.testing.expectEqual(source_ctx.pkru, ctx.pkru);
    try std.testing.expectEqual(source_ctx.signal_entry, ctx.signal_entry);
    try std.testing.expectEqual(@as(u64, 0), ctx.signal_pending_hint_va);
    var image: process_abi.ThreadUserContext = undefined;
    try std.testing.expect(getControlledThreadContext(fresh.index, fresh.generation, owner, &image));
    image.registers.rax = 123;
    try std.testing.expect(setControlledThreadContext(fresh.index, fresh.generation, owner, &image));
    try std.testing.expectEqual(@as(u64, 123), ctx.frame.rax);
    try std.testing.expect(markThreadReadyGeneration(fresh.index, fresh.generation, owner, true));
    try std.testing.expect(ctx.ready);
    try std.testing.expect(markThreadReadyGeneration(fresh.index, fresh.generation, owner, false));
    // Wake has no owner argument; its existing generational gate must reject
    // the old allocation even when the new slot is in a wakeable state.
    ctx.wait_mailbox = true;
    try std.testing.expect(!wakeIfWaitingGeneration(fresh.index, old.generation));
    try std.testing.expect(ctx.wait_mailbox);
    try std.testing.expect(wakeIfWaitingGeneration(fresh.index, fresh.generation));
    // Current release consumes a pin, including its generation, not an index.
    try std.testing.expectEqual(@as(?u32, source.generation), currentThreadGeneration());
    try std.testing.expect(!setThreadBasesGeneration(source.index, source.generation, owner, 0, 0));
    try std.testing.expect(!copyCurrentThreadStateToGeneration(source.index, source.generation, owner));
    ContextTransferTestRegisters.reset();
    try std.testing.expect(setCurrentThreadBaseWithRegisters(ContextTransferTestRegisters, .fs, 0x5000));
    try std.testing.expectEqual(@as(u64, 0x5000), source_ctx.fs_base);
    scheduler_state.cpus[0].current_generation +%= 1;
    try std.testing.expectEqual(@as(?u32, null), currentThreadGeneration());
    try std.testing.expect(!setCurrentThreadBaseWithRegisters(ContextTransferTestRegisters, .gs, 0x6000));
    try std.testing.expectEqual(@as(u64, 0), source_ctx.gs_base);
    try std.testing.expect(!releaseCurrentThread());
    try std.testing.expect(!copyCurrentThreadStateToGeneration(fresh.index, fresh.generation, owner));
    scheduler_state.cpus[0].current_generation = source.generation;
    try std.testing.expect(releaseCurrentThread());
    try std.testing.expect(!source_ctx.allocated);
    try std.testing.expect(threadSlotReferencedByCpu(source.index));
    clearCurrentContextLocked(&scheduler_state.cpus[0]);
    try std.testing.expect(releaseThreadGeneration(fresh.index, fresh.generation, owner));
}

test "thread storage identity admission holds table until action completes" {
    const saved_contexts = try std.testing.allocator.dupe(ThreadContext, &initial_thread_contexts);
    defer std.testing.allocator.free(saved_contexts);
    const saved_hot = initial_thread_hot_states;
    defer {
        @memcpy(&initial_thread_contexts, saved_contexts);
        initial_thread_hot_states = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    var spaces = [_]UserAddressSpace{.{}};
    var table: UserAddressSpaceTable = .{};
    table.initializeFreshStorage(&spaces);
    var free_list: kernel.FreePageList = .{};
    const allocated = allocateSuspendedThread(default_process_principal, &table, std.mem.zeroes(TrapFrame), &free_list).?;
    var admitted: u8 = 0;
    var proceed: u8 = 0;
    var contended: u8 = 0;
    var done: u8 = 0;
    ThreadIdentityTestProbe.admitted = &admitted;
    ThreadIdentityTestProbe.proceed = &proceed;
    ThreadIdentityTestProbe.contended = &contended;
    defer {
        ThreadIdentityTestProbe.admitted = null;
        ThreadIdentityTestProbe.proceed = null;
        ThreadIdentityTestProbe.contended = null;
    }
    const Worker = struct {
        fn action(a: ThreadAllocation, finished: *u8) void {
            if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu"))
                @import("root").setTestLockDiagnosticsCpu(0);
            const ok = markThreadReadyGeneration(a.index, a.generation, default_process_principal, true);
            @atomicStore(u8, finished, if (ok) 1 else 2, .release);
        }
        fn reuse(a: ThreadAllocation, spaces_table: *UserAddressSpaceTable, pages: *kernel.FreePageList, result: *?ThreadAllocation) void {
            if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu"))
                @import("root").setTestLockDiagnosticsCpu(1);
            if (!releaseThreadGeneration(a.index, a.generation, default_process_principal)) return;
            result.* = allocateSuspendedThread(default_process_principal, spaces_table, std.mem.zeroes(TrapFrame), pages);
        }
    };
    const action = try std.Thread.spawn(.{}, Worker.action, .{ allocated, &done });
    while (@atomicLoad(u8, &admitted, .acquire) == 0 and @atomicLoad(u8, &done, .acquire) == 0) std.atomic.spinLoopHint();
    var reused: ?ThreadAllocation = null;
    const reuse = std.Thread.spawn(.{}, Worker.reuse, .{ allocated, &table, &free_list, &reused }) catch |err| {
        @atomicStore(u8, &proceed, 1, .release);
        action.join();
        return err;
    };
    while (@atomicLoad(u8, &contended, .acquire) == 0 and @atomicLoad(u8, &done, .acquire) == 0) std.atomic.spinLoopHint();
    const was_done = @atomicLoad(u8, &done, .acquire);
    @atomicStore(u8, &proceed, 1, .release);
    action.join();
    reuse.join();
    try std.testing.expectEqual(@as(u8, 0), was_done);
    try std.testing.expectEqual(@as(u8, 1), contended);
    try std.testing.expectEqual(@as(u8, 1), done);
    const fresh = reused orelse return error.TestUnexpectedResult;
    try std.testing.expectEqual(allocated.index, fresh.index);
    try std.testing.expect(allocated.generation != fresh.generation);
    try std.testing.expect(!markThreadReadyGeneration(allocated.index, allocated.generation, default_process_principal, true));
    try std.testing.expect(markThreadReadyGeneration(fresh.index, fresh.generation, default_process_principal, true));
}

test "uninterruptible wait real BSP AP admission matrix and wait attribute lifetime" {
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        WaitAdmissionTest.stop_after_block = false;
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    // All five keyed WAIT frontends have this admission contract. Nanosleep
    // adds the unkeyed sixth case and must never opt out of signals.
    for (0..2) |path| {
        for (0..6) |kind| {
            for ([_]bool{ false, true }) |uninterruptible| {
                if (kind == 5 and uninterruptible) continue;
                for (0..3) |pending| {
                    try initializeDirectBlockTest(false);
                    WaitAdmissionTest.stop_after_block = true;
                    WaitAdmissionTest.stopped_at_block = false;
                    const ctx = &initial_thread_contexts[4];
                    ctx.signal_entry = 0x4000;
                    ctx.signal_delivery_ready = true;
                    ctx.pending_signal_mask = if (pending == 0) 0 else 1;
                    ctx.signal_blocked_mask = if (pending == 2) 1 else 0;
                    const token = if (kind == 5) 0 else reserveCurrentWaitToken(1).?;
                    var frame = std.mem.zeroes(TrapFrame);
                    const admitted = if (path == 0)
                        blockCurrentThread(&frame, kind != 5, uninterruptible, token, 10, 5, null)
                    else
                        parkApThreadForBlock(&frame, kind != 5, uninterruptible, token, 10, 5, null);
                    const expected = uninterruptible or pending != 1;
                    try std.testing.expectEqual(expected, admitted);
                    try std.testing.expectEqual(expected, WaitAdmissionTest.stopped_at_block);
                    try std.testing.expectEqual(!expected, ctx.ready);
                    try std.testing.expectEqual(expected and uninterruptible, ctx.wait_uninterruptible);
                    if (admitted) {
                        wakeIfTimerExpired(4, lapic_tick_count +| 10);
                        try std.testing.expect(ctx.ready and !ctx.wait_uninterruptible);
                        try std.testing.expectEqual(@as(u64, 5), ctx.frame.rax);
                    }
                }
            }
        }
    }
}

test "uninterruptible wait direct handoff pending matrix rejects before outbound mutation" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    for ([_]bool{ false, true }) |uninterruptible| {
        for (0..3) |pending| {
            var request = try initializeWakeWaitTest(0);
            request.wait_uninterruptible = uninterruptible;
            const ctx = &initial_thread_contexts[4];
            ctx.signal_entry = 0x4000;
            ctx.signal_delivery_ready = true;
            ctx.pending_signal_mask = if (pending == 0) 0 else 1;
            ctx.signal_blocked_mask = if (pending == 2) 1 else 0;
            var frame = std.mem.zeroes(TrapFrame);
            frame.rdx = std.math.maxInt(u64);
            scheduler_state.thread_table.lock();
            const admitted = tryWakeWaitTransferLocked(ContextTransferTestRegisters, WakeWaitTestInstaller.install, &frame, request);
            scheduler_state.thread_table.unlock();
            const expected = uninterruptible or pending != 1;
            try std.testing.expectEqual(expected, admitted != null);
            try std.testing.expectEqual(expected and uninterruptible, ctx.wait_uninterruptible);
            if (!expected) {
                try std.testing.expectEqual(std.math.maxInt(u64), frame.rdx);
                try std.testing.expect(initial_thread_contexts[5].wait_mailbox);
                try std.testing.expectEqual(@as(usize, 0), WakeWaitTestInstaller.calls);
            } else {
                try std.testing.expectEqual(@as(u64, 1), ctx.frame.rdx);
                try std.testing.expect(!initial_thread_contexts[5].wait_uninterruptible);
            }
        }
    }
}

test "uninterruptible wait signal defers U wait but interrupts ordinary wait and refuses rewait" {
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        WaitAdmissionTest.stop_after_block = false;
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    for ([_]bool{ false, true }) |uninterruptible| {
        try initializeDirectBlockTest(false);
        WaitAdmissionTest.stop_after_block = true;
        const ctx = &initial_thread_contexts[4];
        // No descriptor/hint: this isolates the true pending state from any
        // advisory publication. The signal implementation still executes its
        // full wake transition and descriptor revalidation path.
        const owner: kernel.PrincipalId = @enumFromInt(std.math.maxInt(u32));
        ctx.owner_process = owner;
        ctx.signal_entry = 0x4000;
        ctx.signal_delivery_ready = true;
        const token = reserveCurrentWaitToken(1).?;
        var frame = std.mem.zeroes(TrapFrame);
        try std.testing.expect(blockCurrentThread(&frame, true, uninterruptible, token, 10, 5, null));
        const state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(state);
        scheduler_state.thread_table.lock();
        const delivery = deliverSignalLocked(state, 1, owner, 4, ctx, 1, 2).?;
        try std.testing.expectEqual(!uninterruptible, delivery.was_blocked);
        try std.testing.expectEqual(@as(u64, 1), ctx.pending_signal_mask);
        if (uninterruptible) {
            try std.testing.expect(!ctx.ready and ctx.wait_uninterruptible);
            try std.testing.expectEqual(token, ctx.active_wait_token);
            try std.testing.expectEqual(.published, claimAndPublishWaitCompletionPreferred(4, 1, owner, token, 77));
            try std.testing.expect(ctx.ready and !ctx.wait_uninterruptible);
            try std.testing.expectEqual(@as(u64, 77), ctx.frame.rax);
        }
        // Wake happened while hint remained zero. Re-enter the running state
        // as the consumer would, with the pending bit still unclaimed.
        const picked = verifiedPickThreadForCpu(0).?;
        try std.testing.expectEqual(@as(usize, 4), picked.thread_index);
        setTestCurrentThread(&scheduler_state.cpus[0], 4);
        scheduler_state.cpus[0].is_idle = false;
        const again = reserveCurrentWaitToken(1).?;
        try std.testing.expect(!blockCurrentThread(&frame, true, false, again, 0, 2, null));
        try std.testing.expect(ctx.ready and ctx.active_wait_token == 0);
    }
}

test "uninterruptible wait STOP CONT completion and teardown ignore uninterruptible status" {
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        WaitAdmissionTest.stop_after_block = false;
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    for ([_]bool{ false, true }) |teardown| {
        try initializeDirectBlockTest(false);
        WaitAdmissionTest.stop_after_block = true;
        const ctx = &initial_thread_contexts[4];
        const token = reserveCurrentWaitToken(1).?;
        var frame = std.mem.zeroes(TrapFrame);
        try std.testing.expect(blockCurrentThread(&frame, true, true, token, 0, 2, null));
        if (teardown) {
            try std.testing.expect(releaseThreadGeneration(4, 1, default_process_principal));
            try std.testing.expect(!ctx.allocated and !ctx.wait_uninterruptible);
        } else {
            _ = stopPrincipalThreads(default_process_principal);
            try std.testing.expect(ctx.stopped and ctx.wait_uninterruptible);
            _ = continuePrincipalThreads(default_process_principal);
            try std.testing.expect(!ctx.stopped and !ctx.ready and ctx.wait_uninterruptible);
            _ = stopPrincipalThreads(default_process_principal);
            try std.testing.expectEqual(.published, claimAndPublishWaitCompletionPreferred(4, 1, default_process_principal, token, 77));
            try std.testing.expect(ctx.stopped and ctx.resume_after_stop and !ctx.wait_uninterruptible);
            _ = continuePrincipalThreads(default_process_principal);
            try std.testing.expect(ctx.ready and !ctx.stopped and ctx.frame.rax == 77);
        }
    }
}

pub fn testUninterruptibleWaitFrontendMatrix() !void {
    comptime std.debug.assert(builtin.is_test);
    const WaitFrontendFixture = struct {
        var pages: kernel.FreePageList = .{};
        var ap = false;
        var block_calls: usize = 0;
        var copies: usize = 0;
        var bad_lock_interval = false;
        fn copy(_: kernel.PrincipalId, address: u64, bytes: []u8) bool {
            copies += 1;
            @memcpy(bytes, @as([*]const u8, @ptrFromInt(address))[0..bytes.len]);
            return true;
        }
        fn write(_: kernel.PrincipalId, address: u64, bytes: []const u8) bool {
            @memcpy(@as([*]u8, @ptrFromInt(address))[0..bytes.len], bytes);
            return true;
        }
        fn read64(_: kernel.PrincipalId, address: u64) ?u64 {
            return @as(*const u64, @ptrFromInt(address)).*;
        }
        fn write64(_: kernel.PrincipalId, address: u64, value: u64) bool {
            @as(*u64, @ptrFromInt(address)).* = value;
            return true;
        }
        fn block(frame: *TrapFrame, mailbox: bool, uninterruptible: bool, token: u64, ticks: u64, result: u64, callback: ?BeforeCurrentThreadLeaveCallback) bool {
            block_calls += 1;
            const unlocks = @atomicLoad(u64, &WaitAdmissionTest.unlocks, .monotonic);
            const admitted = if (ap) parkApThreadForBlock(frame, mailbox, uninterruptible, token, ticks, result, callback) else blockCurrentThread(frame, mailbox, uninterruptible, token, ticks, result, callback);
            // A dropped/reacquired T admits a sender between pending check and
            // block even if no sender happens to win this particular host run.
            bad_lock_interval = @atomicLoad(u64, &WaitAdmissionTest.unlocks, .monotonic) != unlocks + 1;
            return admitted;
        }
        fn hooks() @TypeOf(.{ .free_list = &pages, .copy_user_bytes_from_va = copy, .copy_bytes_to_user_va = write, .read_user_u64 = read64, .write_user_u64 = write64, .block_current_thread_for_event = block, .before_current_thread_leave = @as(?BeforeCurrentThreadLeaveCallback, null) }) {
            return .{ .free_list = &pages, .copy_user_bytes_from_va = copy, .copy_bytes_to_user_va = write, .read_user_u64 = read64, .write_user_u64 = write64, .block_current_thread_for_event = block, .before_current_thread_leave = null };
        }
    };

    const fd = @import("syscall/fd.zig");
    const ipc = @import("syscall/native_ipc.zig");
    const runtime = @import("syscall/runtime.zig");
    const sc = @import("syscall/numbers.zig");
    // Bits 2..4 select retained FD waits; use an unknown bit for every frontend.
    const invalid_flags: u64 = 1 << 5;
    const storage = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(storage);
    try std.testing.expect(kernel.initRuntimeStorage(storage));
    defer {
        WaitAdmissionTest.stop_after_block = false;
        initializeStaticStorage();
        runtime.WaitTestSupport.reset();
    }
    for (0..2) |path| {
        for (0..5) |kind| {
            for ([_]u64{ 0, 2, invalid_flags }) |flags| {
                for (0..3) |pending| {
                    // Each row owns fresh heap storage; no live owner is reset.
                    const state = try std.testing.allocator.create(kernel.KernelState);
                    defer std.testing.allocator.destroy(state);
                    try state.initFromDetectedRegionsInPlace(1);
                    defer {
                        detachPrincipalIpcBindings(default_process_principal);
                        state.drainIpcCancellations(&WaitFrontendFixture.pages);
                    }
                    try initializeDirectBlockTest(false);
                    runtime.WaitTestSupport.reset();
                    WaitAdmissionTest.stop_after_block = true;
                    WaitAdmissionTest.stopped_at_block = false;
                    WaitFrontendFixture.ap = path == 1;
                    WaitFrontendFixture.block_calls = 0;
                    WaitFrontendFixture.copies = 0;
                    WaitFrontendFixture.bad_lock_interval = false;
                    const ctx = &initial_thread_contexts[4];
                    ctx.signal_entry = 0x4000;
                    ctx.signal_delivery_ready = true;
                    ctx.pending_signal_mask = if (pending == 0) 0 else 1;
                    ctx.signal_blocked_mask = if (pending == 2) 1 else 0;
                    const pair = try state.createIpcChannelPairFds(default_process_principal, .{ .send = true, .recv = true, .wait = true, .poll = true, .close = true }, .{}, 16);
                    var poll = [_]u64{ pair.a, 1, 0, 0 };
                    var word: u32 = 0;
                    var other: u32 = 0;
                    var frame = std.mem.zeroes(TrapFrame);
                    frame.rdi = pair.a;
                    frame.rsi = 0;
                    frame.rdx = 10;
                    frame.r10 = flags;
                    const status = switch (kind) {
                        0 => blk: {
                            frame.rsi = flags;
                            break :blk ipc.WaitTestSupport.recv(WaitFrontendFixture.hooks(), state, default_process_principal, &frame);
                        },
                        1 => blk: {
                            frame.rdi = @intFromPtr(&poll);
                            frame.rsi = 1;
                            break :blk fd.WaitTestSupport.wait(WaitFrontendFixture.hooks(), state, default_process_principal, &frame);
                        },
                        2 => blk: {
                            frame.rsi = 10;
                            frame.rdx = flags;
                            break :blk ipc.WaitTestSupport.notify(WaitFrontendFixture.hooks(), state, default_process_principal, &frame);
                        },
                        3 => blk: {
                            frame.rdi = @intFromPtr(&word);
                            frame.rsi = 0;
                            break :blk runtime.WaitTestSupport.wait(WaitFrontendFixture.hooks(), default_process_principal, &frame);
                        },
                        else => blk: {
                            frame.rdi = @intFromPtr(&other);
                            frame.rsi = @intFromPtr(&word);
                            frame.r10 = 0;
                            frame.r8 = 10;
                            frame.r9 = flags;
                            break :blk runtime.WaitTestSupport.wakeWait(WaitFrontendFixture.hooks(), default_process_principal, &frame);
                        },
                    };
                    const admitted = flags != invalid_flags and (flags == 2 or pending != 1);
                    try std.testing.expectEqual(admitted, WaitAdmissionTest.stopped_at_block);
                    try std.testing.expect(!WaitFrontendFixture.bad_lock_interval);
                    try std.testing.expectEqual(flags != invalid_flags, @as(bool, WaitFrontendFixture.block_calls == 1));
                    try std.testing.expectEqual(admitted and flags == 2, ctx.wait_uninterruptible);
                    if (flags == invalid_flags) {
                        try std.testing.expectEqual(sc.syscall_err_invalid, status);
                        try std.testing.expectEqual(@as(usize, 0), WaitFrontendFixture.copies);
                    } else if (!admitted) try std.testing.expectEqual(sc.syscall_err_not_ready, status);
                    if (kind == 4) try std.testing.expectEqual(if (flags == invalid_flags) std.math.maxInt(u64) else @as(u64, 0), frame.rdx);
                    if (admitted) {
                        if (flags == 2) {
                            scheduler_state.thread_table.lock();
                            const delivery = deliverSignalLocked(state, 1, default_process_principal, 4, ctx, 2, sc.syscall_err_not_ready).?;
                            try std.testing.expect(!delivery.was_blocked and !ctx.ready and ctx.wait_uninterruptible);
                        }
                        if (kind < 3 and pending == 2) {
                            const handle = fd.WaitTestSupport.channelHandle(state, default_process_principal, pair.b).?;
                            const snapshot = state.ipcChannelNotifyWaiterSnapshot(handle);
                            try state.closeFdWithFreeList(default_process_principal, pair.b, &WaitFrontendFixture.pages);
                            fd.WaitTestSupport.closeNotify(state, handle, snapshot);
                            try std.testing.expect(fd.WaitTestSupport.closeWake(WaitFrontendFixture.hooks(), state, handle));
                            try std.testing.expectEqual(if (kind == 1) @as(u64, 1) else sc.syscall_err_closed, ctx.frame.rax);
                        } else if (kind < 3 and pending == 1) {
                            if (kind == 2) {
                                const notice = try state.ipcNotifyPeerForFd(default_process_principal, pair.b);
                                try std.testing.expectEqual(.published, fd.completeIpcNotifyWaiter(notice.waiter.?, sc.syscall_ok));
                            } else {
                                try state.ipcSend(default_process_principal, pair.b, .{ .words = .{ 11, 22, 33, 44 } }, &WaitFrontendFixture.pages);
                                try std.testing.expect(ipc.WaitTestSupport.wakeSend(WaitFrontendFixture.hooks(), state, default_process_principal, pair.b, null));
                                try std.testing.expectEqual(if (kind == 1) @as(u64, 1) else sc.syscall_ok, ctx.frame.rax);
                                if (kind == 0) try std.testing.expectEqual(@as(u64, 11), initial_thread_contexts[4].frame.r10);
                            }
                        } else wakeIfTimerExpired(4, lapic_tick_count +| 10);
                        try std.testing.expect(ctx.ready and !ctx.wait_uninterruptible);
                    }
                }
            }
        }
    }
}

test "thread storage AS reclaim includes local offline and idle pins until withdrawal" {
    initializeStaticStorage();
    const ctx = &initial_thread_contexts[4];
    const saved = ctx.*;
    defer {
        ctx.* = saved;
        initializeStaticStorage();
    }
    const owner = kernel.processPrincipalFromIndex(123).?;
    ctx.* = .{ .id = 4, .generation = 17, .allocated = true, .owner_process = owner, .cr3 = 0x1234000 };
    try std.testing.expect(!addressSpaceUsersEnded(owner));
    const local = &scheduler_state.cpus[0];
    const remote = &scheduler_state.cpus[scheduler_state.cpus.len - 1];
    installCurrentContextLocked(local, 4, ctx);
    installCurrentContextLocked(remote, 4, ctx);
    ctx.allocated = false;
    remote.enabled = false;
    remote.is_idle = true;
    try std.testing.expect(!addressSpaceUsersEnded(owner));
    clearCurrentContextLocked(local);
    try std.testing.expect(!addressSpaceUsersEnded(owner));
    clearCurrentContextLocked(remote);
    try std.testing.expect(addressSpaceUsersEnded(owner));
}

test "thread storage IDs preserve legacy ordering and reject representation overflow" {
    const generations = [_]u32{ 0, 1, 2, 65535, max_thread_generation };
    const indices = [_]usize{ 0, 1, 32768, 65535 };
    for (generations) |ga| for (indices) |ia| {
        for (generations) |gb| for (indices) |ib| {
            const old_a = @as(u64, ga) * 65536 + ia + 1;
            const old_b = @as(u64, gb) * 65536 + ib + 1;
            try std.testing.expectEqual(std.math.order(old_a, old_b), std.math.order(verifiedThreadIdForGeneration(ia, ga).?, verifiedThreadIdForGeneration(ib, gb).?));
        };
    };
    try std.testing.expectEqual(@as(?i64, std.math.maxInt(i64)), verifiedThreadIdForGeneration(std.math.maxInt(u32) - 1, max_thread_generation));
    try std.testing.expect(verifiedThreadIdForGeneration(std.math.maxInt(u32), 1) == null);
    try std.testing.expect(verifiedThreadIdForGeneration(0, max_thread_generation + 1) == null);
    try std.testing.expect(idleThreadMarker > std.math.maxInt(u32));
    try std.testing.expectEqual(@as(?usize, 131072), nextThreadCapacity(65537, 65536));
    try std.testing.expectEqual(@as(?usize, threadIdValueCount), nextThreadCapacity(threadIdValueCount, 1 << 31));
    try std.testing.expect(nextThreadCapacity(threadIdValueCount + 1, threadIdValueCount) == null);
}

test "thread storage creates 65537 identities and retires terminal generation" {
    const saved = try std.testing.allocator.dupe(ThreadContext, &initial_thread_contexts);
    defer std.testing.allocator.free(saved);
    const saved_hot = initial_thread_hot_states;
    initializeStaticStorage();
    const count = 65537;
    var transaction = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, @import("memory/slot_table.zig").hostTestAllocator());
    defer if (!transaction.finished) transaction.rollback() catch unreachable;
    const draft = try ThreadGrowth.reserve(&transaction, count, initialThreadCapacity);
    defer {
        // All fixture CPU/runqueue references end before freeing the host grants.
        @memset(scheduler_state.cpus, .{});
        scheduler_state.thread_table.storage.retireAll() catch unreachable;
        scheduler_state.thread_table.storage.reclaimRetiredAfterBorrowExclusion() catch unreachable;
        scheduler_state.thread_table = .{};
        @memcpy(&initial_thread_contexts, saved);
        initial_thread_hot_states = saved_hot;
        initializeStaticStorage();
    }
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    try std.testing.expect(publishThreadGrowthLocked(&transaction, draft, count, initialThreadCapacity));
    for (0..count) |index| {
        const ctx = threadContextLocked(index).?;
        ctx.allocated = false;
        if (index == 0) try std.testing.expect(!initializeThreadContextLocked(index, .Device0, 0x2000, std.mem.zeroes(TrapFrame), false));
        try std.testing.expect(initializeThreadContextLocked(index, default_process_principal, 0x2000, std.mem.zeroes(TrapFrame), false));
        try std.testing.expect(verifiedAddThreadLocked(index, ctx.generation, false));
    }
    const last = count - 1;
    const ctx = threadContextLocked(last).?;
    const old_generation = ctx.generation;
    try std.testing.expect(releaseThreadLocked(last));
    try std.testing.expect(matchingThreadLocked(last, old_generation, default_process_principal) == null);
    threadDirectory().slots[last].generation = max_thread_generation - 1;
    try std.testing.expect(initializeThreadContextLocked(last, default_process_principal, 0x2000, std.mem.zeroes(TrapFrame), false));
    try std.testing.expect(verifiedAddThreadLocked(last, ctx.generation, false));
    try std.testing.expect(releaseThreadLocked(last));
    try std.testing.expectEqual(max_thread_generation, ctx.generation);
    try std.testing.expect(!initializeThreadContextLocked(last, default_process_principal, 0x2000, std.mem.zeroes(TrapFrame), false));
    try std.testing.expectEqual(max_thread_generation, nextThreadGeneration(max_thread_generation));
}

test "thread storage self exit reservation crosses former principal bound" {
    defer principal_self_exit_extra.deinit();
    const owner: kernel.PrincipalId = @enumFromInt(65536);
    try std.testing.expect(preparePrincipalSelfExit(owner, @import("memory/slot_table.zig").hostTestAllocator()));
    try std.testing.expect(reservePrincipalSelfExit(owner));
    try std.testing.expect(principalSelfExitIsReserved(owner));
    try std.testing.expect(!reservePrincipalSelfExit(owner));
    try std.testing.expect(!tryBeginPrincipalLifecycle(owner));
    clearPrincipalSelfExitReservation(65536);
    try std.testing.expect(!principalSelfExitIsReserved(owner));
    try std.testing.expect(!preparePrincipalSelfExit(.Device0, @import("memory/slot_table.zig").hostTestAllocator()));
}

test "thread storage shared body reclamation preserves generations pins and rejected grants" {
    var backend: owned_storage.TestAllocator = .{};
    defer backend.deinit();
    initializeStaticStorage();
    defer {
        // End every fixture borrower before destroying capacity metadata.
        @memset(scheduler_state.cpus, .{});
        @atomicStore(*const ThreadDirectory, &scheduler_state.thread_table.directory, &initial_thread_directory, .release);
        backend.fail_return = false;
        scheduler_state.thread_table.storage.retireAll() catch unreachable;
        scheduler_state.thread_table.storage.reclaimRetiredAfterBorrowExclusion() catch unreachable;
        std.debug.assert(backend.active() == 0);
        scheduler_state.thread_table = .{};
        initializeStaticStorage();
    }
    const count = 65537;
    var tx = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, backend.interface());
    const draft = try ThreadGrowth.reserve(&tx, count, initialThreadCapacity);
    {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        try std.testing.expect(publishThreadGrowthLocked(&tx, draft, count, initialThreadCapacity));
        for (initialThreadCapacity..count) |index| {
            try std.testing.expect(initializeThreadContextLocked(index, default_process_principal, 0x2000, std.mem.zeroes(TrapFrame), false));
            try std.testing.expect(verifiedAddThreadLocked(index, 1, false));
        }
        for (initialThreadCapacity..count - 1) |index| try std.testing.expect(releaseThreadLocked(index));
    }
    const first = initialThreadCapacity;
    const old = threadDirectory().contexts[first];
    const node = old.scheduler_entity.?;
    reclaimThreadBodies();
    try std.testing.expectEqual(@as(usize, 6), backend.active()); // One live slot retains both shared grants.
    const local = &scheduler_state.cpus[0];
    installCurrentContextLocked(local, first, old);
    {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        try std.testing.expect(releaseThreadLocked(count - 1));
    }
    reclaimThreadBodies();
    try std.testing.expect(threadDirectory().slots[first].retiring);
    try std.testing.expectEqual(old, threadDirectory().contexts[first]);
    try std.testing.expectEqual(@as(usize, 6), backend.active());
    clearCurrentContextLocked(local);
    const remote = &scheduler_state.cpus[scheduler_state.cpus.len - 1];
    installCurrentContextLocked(remote, first, old);
    remote.enabled = false;
    remote.is_idle = true;
    remote.current_generation = 1; // Stale, offline pins still own the address.
    reclaimThreadBodies();
    try std.testing.expectEqual(@as(usize, 6), backend.active());
    clearCurrentContextLocked(remote);
    local.current_entity = node;
    reclaimThreadBodies();
    try std.testing.expectEqual(@as(usize, 6), backend.active());
    local.current_entity = null;
    backend.fail_return = true;
    reclaimThreadBodies();
    try std.testing.expectEqual(@as(usize, 6), backend.active());
    try std.testing.expect(scheduler_state.thread_table.storage.retired != null);
    try std.testing.expectEqual(&absent_thread_context, threadDirectory().contexts[first]);
    try std.testing.expectEqual(@as(u32, 2), threadDirectory().slots[first].generation);
    try std.testing.expect(matchingThreadLocked(first, 1, default_process_principal) == null);
    const attempts = backend.returns;
    reclaimThreadBodies();
    try std.testing.expectEqual(attempts + 2, backend.returns);
    backend.fail_return = false;
    reclaimThreadBodies();
    try std.testing.expectEqual(@as(usize, 4), backend.active()); // Capacity alone survives, not body high water.
    try std.testing.expect(scheduler_state.thread_table.storage.retired == null);
    backend.fail_at = backend.calls + 2;
    backend.fail_return = true;
    try std.testing.expect(!ensureThreadBody(first, backend.interface()));
    try std.testing.expect(scheduler_state.thread_table.storage.pending != null);
    try std.testing.expectEqual(&absent_thread_context, threadDirectory().contexts[first]);
    try std.testing.expectEqual(@as(u32, 2), threadDirectory().slots[first].generation);
    backend.fail_return = false;
    reclaimThreadBodies();
    try std.testing.expect(scheduler_state.thread_table.storage.pending == null);
    try std.testing.expectEqual(@as(usize, 4), backend.active());
    try std.testing.expect(ensureThreadBody(first, backend.interface()));
    {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        // A damaged inactive execution cache cannot reset authoritative history.
        threadDirectory().contexts[first].generation = 99;
        try std.testing.expect(initializeThreadContextLocked(first, default_process_principal, 0x2000, std.mem.zeroes(TrapFrame), false));
        try std.testing.expectEqual(@as(u32, 2), threadDirectory().contexts[first].generation);
        try std.testing.expect(verifiedAddThreadLocked(first, 2, false));
        try std.testing.expect(matchingThreadLocked(first, 1, default_process_principal) == null);
        try std.testing.expect(releaseThreadLocked(first));
        threadDirectory().slots[first].generation = max_thread_generation;
    }
    reclaimThreadBodies();
    try std.testing.expectEqual(@as(usize, 4), backend.active());
    try std.testing.expect(ensureThreadBody(first, backend.interface()));
    try std.testing.expectEqual(&absent_thread_context, threadDirectory().contexts[first]);
    // Replacing/reclaiming the directory itself must preserve absent-body history.
    var growth = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, backend.interface());
    const next = try ThreadGrowth.reserve(&growth, count + 1, count);
    {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        try std.testing.expect(publishThreadGrowthLocked(&growth, next, count + 1, count));
    }
    try std.testing.expectEqual(max_thread_generation, threadDirectory().slots[first].generation);
    try std.testing.expectEqual(@as(u32, 2), threadDirectory().slots[first + 1].generation);
    try std.testing.expect(ensureThreadBody(first + 1, backend.interface()));
    {
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        try std.testing.expect(!initializeThreadContextLocked(first, default_process_principal, 0x2000, std.mem.zeroes(TrapFrame), false));
        try std.testing.expect(matchingThreadLocked(first, 1, default_process_principal) == null);
    }
    reclaimThreadBodies();
    try std.testing.expectEqual(@as(usize, 4), backend.active());
}

// These fixtures exercise the real T-owned index and completion paths; only
// privileged frame installation is replaced by existing host register hooks.
test "deadline indexed expiry scales with due work and allocates nothing" {
    for ([_]usize{ 128, 1024, 8192 }) |n| {
        var backend: owned_storage.TestAllocator = .{};
        defer backend.deinit();
        initializeStaticStorage();
        const capacity = n + initialThreadCapacity;
        var tx = owned_storage.Transaction.init(&scheduler_state.thread_table.storage, backend.interface());
        const draft = try ThreadGrowth.reserve(&tx, capacity, initialThreadCapacity);
        defer {
            scheduler_state.thread_table.deadlines = .{};
            scheduler_state.thread_table.storage.retireAll() catch unreachable;
            scheduler_state.thread_table.storage.reclaimRetiredAfterBorrowExclusion() catch unreachable;
            scheduler_state.thread_table = .{};
            initializeStaticStorage();
        }
        scheduler_state.thread_table.lock();
        try std.testing.expect(publishThreadGrowthLocked(&tx, draft, capacity, initialThreadCapacity));
        for (initialThreadCapacity..capacity) |i| {
            const ctx = threadContextLocked(i).?;
            ctx.allocated = true;
            ctx.stopped = true;
            setWakeTickLocked(ctx, 1000);
            syncHotStateFromContext(i);
        }
        scheduler_state.thread_table.unlock();
        const allocations = backend.calls;
        const tree = &scheduler_state.thread_table.deadlines;
        for ([_]usize{ 0, 1, 64, 128 }) |k| {
            scheduler_state.thread_table.lock();
            for (initialThreadCapacity..capacity) |i| {
                const ctx = threadContextLocked(i).?;
                ctx.resume_after_stop = false;
                setWakeTickLocked(ctx, if (i - initialThreadCapacity < k) 10 else 1000);
            }
            scheduler_state.thread_table.unlock();
            tree.comparisons = 0;
            TimerWork.visits = 0;
            TimerWork.holds = 0;
            const all_holds = WaitAdmissionTest.unlocks;
            wakeExpiredTimers(10);
            try std.testing.expectEqual(k, TimerWork.visits);
            try std.testing.expectEqual(k + 1, TimerWork.holds);
            try std.testing.expectEqual(@as(u64, k + 1), WaitAdmissionTest.unlocks - all_holds);
            try std.testing.expectEqual(n - k, tree.count);
            try std.testing.expect(tree.comparisons <= (k + 1) * 2 * (std.math.log2_int(usize, n) + 1));
            if (k == 0) try std.testing.expectEqual(@as(usize, 0), tree.comparisons);
            std.debug.print("deadline n={d} k={d} visits={d} T={d} comparisons={d} old-slot-visits={d}\n", .{ n, k, TimerWork.visits, TimerWork.holds, tree.comparisons, capacity });
            try tree.validate();
            for (initialThreadCapacity..initialThreadCapacity + k) |i| {
                const ctx = threadContextLocked(i).?;
                try std.testing.expect(ctx.resume_after_stop and !ctx.ready and ctx.wake_tick == 0);
            }
        }
        try std.testing.expectEqual(allocations, backend.calls);
        scheduler_state.thread_table.lock();
        for (initialThreadCapacity..capacity) |i| setWakeTickLocked(threadContextLocked(i).?, 0);
        scheduler_state.thread_table.unlock();
        try tree.validate();
        try std.testing.expectEqual(@as(usize, 0), tree.count);
        for ([_]usize{ 0, 1, 64, 128 }) |r| {
            for (initialThreadCapacity..initialThreadCapacity + r) |i| {
                const ctx = threadContextLocked(i).?;
                ctx.stopped = false;
                ctx.cpu_affinity = testAffinity(0);
                ctx.wait_mailbox = true;
                ctx.active_wait_token = 11;
                try std.testing.expectEqual(.claimed, claimWaitTokenCompletion(i, ctx.generation, ctx.owner_process, 11));
                try std.testing.expectEqual(.pending, publishClaimedWaitCompletion(i, ctx.generation, ctx.owner_process, 11, 123));
            }
            TimerWork.retries = 0;
            const holds_before = WaitAdmissionTest.unlocks;
            retryPendingThreadMaintenance();
            try std.testing.expectEqual(r, TimerWork.retries);
            try std.testing.expectEqual(@as(u64, r + 3), WaitAdmissionTest.unlocks - holds_before);
            try std.testing.expectEqual(allocations, backend.calls);
            std.debug.print("deadline retry n={d} r={d} visits={d} T={d}\n", .{ n, r, TimerWork.retries, WaitAdmissionTest.unlocks - holds_before });
            scheduler_state.thread_table.lock();
            for (initialThreadCapacity..initialThreadCapacity + r) |i| {
                const ctx = threadContextLocked(i).?;
                ctx.wait_completion_claimed = false;
                ctx.wait_completion_publish_pending = false;
                removeCompletionRetryLocked(ctx);
            }
            scheduler_state.thread_table.unlock();
        }
        std.debug.print("deadline memory context={d} node={d} slot={d} table={d}; IRQ/set allocations=0\n", .{ @sizeOf(ThreadContext), @sizeOf(deadline_index.Node), @sizeOf(ThreadSlot), @sizeOf(ThreadTableState) });
    }
}

test "deadline failed publication retains expiry without starving equal deadlines" {
    initializeStaticStorage();
    defer {
        for (4..7) |i| {
            initial_thread_contexts[i] = .{ .id = @intCast(i) };
            initial_thread_hot_states[i] = .{};
        }
        initializeStaticStorage();
    }
    scheduler_state.cpus[0].is_idle = true;
    setTestCurrentThread(&scheduler_state.cpus[0], idleThreadMarker);
    for (4..7) |i| {
        const ctx = &initial_thread_contexts[i];
        ctx.* = .{ .id = @intCast(i), .generation = 17, .allocated = true, .scheduler_entity = &initial_scheduler_entities[i], .cpu_affinity = testAffinity(1), .wait_mailbox = true, .active_wait_token = 11 };
        try std.testing.expect(verifiedAddThread(i, 17, false));
        setWakeTickLocked(ctx, 10);
        syncHotStateFromContext(i);
    }
    // One invalid affinity fails only its wake; later equal deadlines succeed.
    initial_thread_contexts[4].cpu_affinity = testAffinity(0);
    TimerWork.visits = 0;
    wakeExpiredTimers(10);
    try std.testing.expectEqual(@as(usize, 3), TimerWork.visits);
    try std.testing.expectEqual(@as(usize, 1), scheduler_state.thread_table.deadlines.count);
    try std.testing.expect(initial_thread_contexts[5].ready and initial_thread_contexts[6].ready);
    try std.testing.expectEqual(@as(u64, 10), initial_thread_contexts[4].wake_tick);
    try std.testing.expectEqual(@as(u64, 11), initial_thread_contexts[4].active_wait_token);
    initial_thread_contexts[4].cpu_affinity = testAffinity(1);
    wakeExpiredTimers(11);
    try std.testing.expect(initial_thread_contexts[4].ready);
    try std.testing.expectEqual(@as(usize, 0), scheduler_state.thread_table.deadlines.count);
    wakeExpiredTimers(12);
    try std.testing.expectEqual(@as(usize, 3), scheduler_state.cpus[0].runqueue.count);
    for (4..7) |i| try std.testing.expect(releaseThread(i));
    scheduler_state.thread_table.lock();
    const ctx = &initial_thread_contexts[4];
    try std.testing.expect(initializeThreadContextLocked(4, default_process_principal, 0x2000, std.mem.zeroes(TrapFrame), false));
    try std.testing.expect(verifiedAddThreadLocked(4, ctx.generation, false));
    setWakeTickLocked(ctx, 100);
    setWakeTickLocked(ctx, 200);
    scheduler_state.thread_table.unlock();
    wakeExpiredTimers(100);
    try std.testing.expect(!ctx.ready and ctx.wake_tick == 200);
    try std.testing.expectEqual(@as(usize, 1), scheduler_state.thread_table.deadlines.count);
    try std.testing.expect(releaseThread(4));
    wakeExpiredTimers(200);
    try std.testing.expectEqual(@as(usize, 0), scheduler_state.thread_table.deadlines.count);
}

test "deadline claimed completion retries only published pending set and releases generations" {
    initializeStaticStorage();
    defer {
        for (4..8) |i| {
            initial_thread_contexts[i] = .{ .id = @intCast(i) };
            initial_thread_hot_states[i] = .{};
        }
        initializeStaticStorage();
    }
    scheduler_state.cpus[0].is_idle = true;
    setTestCurrentThread(&scheduler_state.cpus[0], idleThreadMarker);
    for (4..8) |i| {
        const ctx = &initial_thread_contexts[i];
        ctx.* = .{ .id = @intCast(i), .generation = 17, .allocated = true, .owner_process = default_process_principal, .scheduler_entity = &initial_scheduler_entities[i], .cpu_affinity = testAffinity(1), .wait_mailbox = true, .active_wait_token = 11 };
        try std.testing.expect(verifiedAddThread(i, 17, false));
        setWakeTickLocked(ctx, 10);
        syncHotStateFromContext(i);
        try std.testing.expectEqual(.claimed, claimWaitTokenCompletion(i, 17, default_process_principal, 11));
        if (i == 7) continue; // Cleanup not yet complete: never retry this one.
        ctx.cpu_affinity = testAffinity(0);
        try std.testing.expectEqual(.pending, publishClaimedWaitCompletion(i, 17, default_process_principal, 11, 123));
    }
    try std.testing.expectEqual(@as(usize, 0), scheduler_state.thread_table.deadlines.count);
    // Explicit ready/suspend transitions must remove and restore membership,
    // just as the former hot scan's ready filter did on the following tick.
    scheduler_state.thread_table.lock();
    initial_thread_contexts[4].ready = true;
    setThreadHotReady(4, true);
    try std.testing.expect(initial_thread_contexts[4].retry_queue == null);
    initial_thread_contexts[4].ready = false;
    setThreadHotReady(4, false);
    try std.testing.expect(initial_thread_contexts[4].retry_queue != null);
    scheduler_state.thread_table.unlock();
    for (0..3) |_| {
        TimerWork.retries = 0;
        retryPendingThreadMaintenance();
        try std.testing.expectEqual(@as(usize, 3), TimerWork.retries);
    }
    try std.testing.expect(releaseThread(4)); // Cancel a queued retry before reuse.
    const generation = initial_thread_contexts[4].generation;
    try std.testing.expect(generation != 17);
    try std.testing.expectEqual(.stale, publishClaimedWaitCompletion(4, 17, default_process_principal, 11, 999));
    initial_thread_contexts[5].cpu_affinity = testAffinity(1);
    initial_thread_contexts[6].stopped = true;
    TimerWork.retries = 0;
    retryPendingThreadMaintenance();
    try std.testing.expectEqual(@as(usize, 2), TimerWork.retries);
    try std.testing.expect(initial_thread_contexts[5].ready);
    try std.testing.expect(initial_thread_contexts[6].resume_after_stop);
    try std.testing.expect(!initial_thread_contexts[7].ready and initial_thread_contexts[7].wait_completion_claimed);
    TimerWork.retries = 0;
    retryPendingThreadMaintenance();
    try std.testing.expectEqual(@as(usize, 0), TimerWork.retries);
    for (5..8) |i| try std.testing.expect(releaseThread(i));
    for (scheduler_state.thread_table.retries) |q| try std.testing.expect(q.first == null and q.last == null);
}

test "migration waits with dynamic 72 CPU affinity and exit pins" {
    initializeStaticStorage();
    const old_rows = scheduler_state.cpus;
    const old_shadows = base_shadow;
    const rows = try std.testing.allocator.alloc(CpuSchedulerState, 72);
    defer std.testing.allocator.free(rows);
    const shadows = try std.testing.allocator.alloc(BaseShadow, 72);
    defer std.testing.allocator.free(shadows);
    @memset(rows, .{});
    @memset(shadows, .{});
    scheduler_state.cpus = rows;
    base_shadow = shadows;
    defer {
        initializeStaticStorage();
        scheduler_state.cpus = old_rows;
        base_shadow = old_shadows;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    scheduler_state.cpu_count = rows.len;
    for (rows) |*row| row.enabled = true;
    var words: [CpuSet.wordCount(72)]usize = undefined;
    const affinity = CpuSet.init(&words, 72);
    affinity.insert(0);
    affinity.insert(71);
    const tid = 4;
    const ctx = &initial_thread_contexts[tid];
    const saved = ctx.*;
    defer ctx.* = saved;
    ctx.* = .{ .id = tid, .generation = 17, .allocated = true, .ready = true, .owner_process = default_process_principal, .scheduler_entity = &initial_scheduler_entities[tid], .cpu_affinity = affinity };
    try std.testing.expect(verifiedAddThread(tid, 17, true));
    try std.testing.expect(!migrateRunnableThreadGeneration(tid, 17, 70));
    try std.testing.expect(migrateRunnableThreadGeneration(tid, 17, 71));
    try std.testing.expectEqual(@as(usize, 71), ctx.cpu_slot);
    const remote = &rows[71];
    installCurrentContextLocked(remote, tid, ctx);
    remote.is_idle = false;
    try std.testing.expect(principalRunningExcept(default_process_principal, 0));
    try std.testing.expect(!principalRunningExcept(default_process_principal, 71));
    try std.testing.expect(!principalSlotReusable(default_process_principal));
    // An exit can clear allocated before the last architectural return pin.
    ctx.allocated = false;
    remote.enabled = false;
    remote.is_idle = true;
    try std.testing.expect(threadSlotReferencedByRemoteCpu(tid, 0));
    try std.testing.expect(!addressSpaceUsersEnded(default_process_principal));
    clearCurrentContextLocked(remote);
    try std.testing.expect(!threadSlotReferencedByRemoteCpu(tid, 0));
}

test "thread storage IPC call bindings keep nested rights and transfer only expected identity" {
    const saved = initial_thread_contexts[4];
    const saved_hot = initial_thread_hot_states[4];
    defer {
        initial_thread_contexts[4] = saved;
        initial_thread_hot_states[4] = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    const owner = default_process_principal;
    initial_thread_contexts[4] = .{ .id = 4, .generation = 17, .allocated = true, .owner_process = owner, .ready = true, .wait_mailbox = true, .active_wait_token = 11 };
    var outgoing: ipc_call_binding.Node = .{};
    var incoming: ipc_call_binding.Node = .{};
    var replacement: ipc_call_binding.Node = .{};
    try std.testing.expect(bindIpcImplicit(4, 17, owner, &incoming));
    try std.testing.expect(!bindIpcImplicit(4, 17, owner, &replacement));
    try std.testing.expect(!bindIpcOutgoing(4, 18, owner, 11, &outgoing));
    try std.testing.expect(!bindIpcOutgoing(4, 17, owner, 12, &outgoing));
    try std.testing.expect(bindIpcOutgoing(4, 17, owner, 11, &outgoing));
    try std.testing.expect(!takeIpcImplicit(4, 17, owner, &replacement));
    try std.testing.expectEqual(&incoming, peekIpcImplicit(4, 17, owner).?);
    // An abandoned lower CALL cancels only its outgoing right. The upper
    // service obligation remains available, including after drain ownership.
    scheduler_state.thread_table.lock();
    pendIpcBindingLocked(&initial_thread_contexts[4].ipc_outgoing);
    scheduler_state.thread_table.unlock();
    try std.testing.expectEqual(&incoming, peekIpcImplicit(4, 17, owner).?);
    try std.testing.expect(!detachIpcOutgoing(4, 17, owner, &outgoing));
    try std.testing.expectEqual(&outgoing, takePendingIpcCancellation().?);
    try std.testing.expect(takePendingIpcCancellation() == null);
    finishIpcCancellation(&outgoing);
    try std.testing.expect(takeIpcImplicit(4, 17, owner, &incoming));
    try std.testing.expect(!takeIpcImplicit(4, 17, owner, &incoming));
    try std.testing.expectEqual(.detached, incoming.state);
}

test "thread storage IPC call timeout rollback and reply claim preserve ownership and RDX" {
    const saved = initial_thread_contexts[4];
    const saved_hot = initial_thread_hot_states[4];
    defer {
        initial_thread_contexts[4] = saved;
        initial_thread_hot_states[4] = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    const ctx = &initial_thread_contexts[4];
    const owner = default_process_principal;
    ctx.* = .{ .id = 4, .generation = 17, .allocated = true, .owner_process = owner, .scheduler_entity = &initial_scheduler_entities[4], .cpu_affinity = testAffinity(1), .wait_mailbox = true, .active_wait_token = 11 };
    try std.testing.expect(verifiedAddThread(4, 17, false));
    var outgoing: ipc_call_binding.Node = .{};
    var incoming: ipc_call_binding.Node = .{};
    try std.testing.expect(bindIpcOutgoing(4, 17, owner, 11, &outgoing));
    try std.testing.expect(bindIpcImplicit(4, 17, owner, &incoming));
    ctx.frame.rdx = 1;
    scheduler_state.thread_table.lock();
    setWakeTickLocked(ctx, 10);
    scheduler_state.thread_table.unlock();
    scheduler_state.cpus[0].enabled = false;
    wakeIfTimerExpired(4, 10);
    try std.testing.expect(!ctx.ready and ctx.wait_mailbox);
    try std.testing.expectEqual(@as(u64, 11), ctx.active_wait_token);
    try std.testing.expectEqual(.bound, outgoing.state);
    try std.testing.expect(takePendingIpcCancellation() == null);
    scheduler_state.cpus[0].enabled = true;
    try std.testing.expectEqual(.claimed, claimWaitTokenCompletion(4, 17, owner, 11));
    try std.testing.expectEqual(.stale, claimWaitTokenCompletion(4, 17, owner, 11));
    wakeIfTimerExpired(4, 11);
    try std.testing.expectEqual(.bound, outgoing.state);
    try std.testing.expect(detachIpcOutgoing(4, 17, owner, &outgoing));
    try std.testing.expectEqual(.published, publishClaimedWaitCompletion(4, 17, owner, 11, 0));
    try std.testing.expectEqual(@as(u64, 1), ctx.frame.rdx);
    try std.testing.expectEqual(&incoming, peekIpcImplicit(4, 17, owner).?);
    try std.testing.expect(takeIpcImplicit(4, 17, owner, &incoming));
}

test "thread storage IPC call exit moves both nodes once and generation reuse rejects old rights" {
    const saved = initial_thread_contexts[4];
    const saved_hot = initial_thread_hot_states[4];
    defer {
        initial_thread_contexts[4] = saved;
        initial_thread_hot_states[4] = saved_hot;
        initializeStaticStorage();
    }
    initializeStaticStorage();
    const owner = default_process_principal;
    const ctx = &initial_thread_contexts[4];
    ctx.* = .{ .id = 4, .generation = 17, .allocated = true, .owner_process = owner, .scheduler_entity = &initial_scheduler_entities[4], .cpu_affinity = testAffinity(1), .wait_mailbox = true, .active_wait_token = 11 };
    try std.testing.expect(verifiedAddThread(4, 17, false));
    var outgoing: ipc_call_binding.Node = .{};
    var incoming: ipc_call_binding.Node = .{};
    try std.testing.expect(bindIpcOutgoing(4, 17, owner, 11, &outgoing));
    try std.testing.expect(bindIpcImplicit(4, 17, owner, &incoming));
    try std.testing.expect(!releaseThreadGeneration(4, 18, owner));
    try std.testing.expectEqual(.bound, outgoing.state);
    try std.testing.expectEqual(.bound, incoming.state);
    try std.testing.expect(releaseThreadGeneration(4, 17, owner));
    try std.testing.expect(!releaseThreadGeneration(4, 17, owner));
    try std.testing.expectEqual(&outgoing, takePendingIpcCancellation().?);
    try std.testing.expectEqual(&incoming, takePendingIpcCancellation().?);
    try std.testing.expect(takePendingIpcCancellation() == null);
    finishIpcCancellation(&outgoing);
    finishIpcCancellation(&incoming);
    try std.testing.expect(!takeIpcImplicit(4, 17, owner, &incoming));
    try std.testing.expect(!detachIpcOutgoing(4, 17, owner, &outgoing));
}

test "thread storage IPC call timeout and signal cancel outgoing without losing incoming" {
    const saved = initial_thread_contexts[4];
    const saved_hot = initial_thread_hot_states[4];
    defer {
        initial_thread_contexts[4] = saved;
        initial_thread_hot_states[4] = saved_hot;
        initializeStaticStorage();
    }
    for (0..4) |scenario| {
        initializeStaticStorage();
        const owner = default_process_principal;
        const ctx = &initial_thread_contexts[4];
        ctx.* = .{ .id = 4, .generation = 17, .allocated = true, .owner_process = owner, .scheduler_entity = &initial_scheduler_entities[4], .cpu_affinity = testAffinity(1), .wait_mailbox = true, .active_wait_token = 11 };
        try std.testing.expect(verifiedAddThread(4, 17, false));
        var outgoing: ipc_call_binding.Node = .{};
        var incoming: ipc_call_binding.Node = .{};
        try std.testing.expect(bindIpcOutgoing(4, 17, owner, 11, &outgoing));
        try std.testing.expect(bindIpcImplicit(4, 17, owner, &incoming));
        ctx.frame.rdx = 1;
        if (scenario < 2) {
            scheduler_state.thread_table.lock();
            setWakeTickLocked(ctx, 10);
            ctx.stopped = scenario == 1;
            scheduler_state.thread_table.unlock();
            wakeIfTimerExpired(4, 10);
            try std.testing.expect(ctx.ready == (scenario == 0));
            try std.testing.expect(ctx.resume_after_stop == (scenario == 1));
        } else {
            scheduler_state.cpus[0].enabled = scenario == 2;
            scheduler_state.thread_table.lock();
            interruptIpcWaitLocked(ctx, 99);
            scheduler_state.thread_table.unlock();
            try std.testing.expect(ctx.ready == (scenario == 2));
            try std.testing.expect(ctx.wait_completion_publish_pending == (scenario == 3));
            if (scenario == 3) {
                scheduler_state.cpus[0].enabled = true;
                retryPendingThreadMaintenance();
                try std.testing.expect(ctx.ready);
                try std.testing.expectEqual(@as(u64, 99), ctx.frame.rax);
            }
        }
        try std.testing.expectEqual(@as(u64, 1), ctx.frame.rdx);
        try std.testing.expectEqual(.stale, claimWaitTokenCompletion(4, 17, owner, 11));
        try std.testing.expectEqual(&outgoing, takePendingIpcCancellation().?);
        try std.testing.expect(takePendingIpcCancellation() == null);
        finishIpcCancellation(&outgoing);
        try std.testing.expectEqual(&incoming, peekIpcImplicit(4, 17, owner).?);
        try std.testing.expect(takeIpcImplicit(4, 17, owner, &incoming));
    }
}

const IpcCallFrontendFixture = struct {
    var pages: kernel.FreePageList = .{};
    var reject_write: u64 = 0;
    var user_accesses: usize = 0;
    var preferred_target: ?kernel.ThreadWakeTarget = null;
    fn copy(_: kernel.PrincipalId, address: u64, bytes: []u8) bool {
        user_accesses += 1;
        if (address == 0) return false;
        @memcpy(bytes, @as([*]const u8, @ptrFromInt(address))[0..bytes.len]);
        return true;
    }
    fn write(_: kernel.PrincipalId, address: u64, bytes: []const u8) bool {
        user_accesses += 1;
        if (address == 0 or address == reject_write) return false;
        @memcpy(@as([*]u8, @ptrFromInt(address))[0..bytes.len], bytes);
        return true;
    }
    fn read64(_: kernel.PrincipalId, address: u64) ?u64 {
        user_accesses += 1;
        if (address == 0) return null;
        return @as(*const u64, @ptrFromInt(address)).*;
    }
    fn write64(_: kernel.PrincipalId, address: u64, value: u64) bool {
        user_accesses += 1;
        if (address == 0 or address == reject_write) return false;
        @as(*u64, @ptrFromInt(address)).* = value;
        return true;
    }
    fn block(frame: *TrapFrame, mailbox: bool, uninterruptible: bool, token: u64, ticks: u64, result: u64, callback: ?BeforeCurrentThreadLeaveCallback) bool {
        return blockCurrentThread(frame, mailbox, uninterruptible, token, ticks, result, callback);
    }
    fn blockPreferred(frame: *TrapFrame, mailbox: bool, uninterruptible: bool, token: u64, ticks: u64, result: u64, callback: ?BeforeCurrentThreadLeaveCallback, preferred: ?kernel.ThreadWakeTarget) bool {
        preferred_target = preferred;
        return blockCurrentThreadPreferred(frame, mailbox, uninterruptible, token, ticks, result, callback, preferred);
    }
    fn hooks() @TypeOf(.{ .free_list = &pages, .copy_user_bytes_from_va = copy, .copy_bytes_to_user_va = write, .read_user_u64 = read64, .write_user_u64 = write64, .block_current_thread_for_event = block, .block_current_thread_for_event_preferred = blockPreferred, .before_current_thread_leave = @as(?BeforeCurrentThreadLeaveCallback, null) }) {
        return .{ .free_list = &pages, .copy_user_bytes_from_va = copy, .copy_bytes_to_user_va = write, .read_user_u64 = read64, .write_user_u64 = write64, .block_current_thread_for_event = block, .block_current_thread_for_event_preferred = blockPreferred, .before_current_thread_leave = null };
    }
};

test "thread storage IPC call actual frontend completion cancellation extraction and partial reply recv" {
    const ipc = @import("syscall/native_ipc.zig").WaitTestSupport;
    const sc = @import("syscall/numbers.zig");
    const abi = @import("kernel_abi_root").ipc_abi;
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        WaitAdmissionTest.stop_after_block = false;
        IpcCallFrontendFixture.reject_write = 0;
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    const storage = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(storage);
    try std.testing.expect(kernel.initRuntimeStorage(storage));
    for (0..24) |scenario| {
        const state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        try initializeDirectBlockTest(true);
        WaitAdmissionTest.stop_after_block = true;
        IpcCallFrontendFixture.reject_write = 0;
        const h = IpcCallFrontendFixture.hooks();
        const owner = default_process_principal;
        const pair = try state.createIpcChannelPairFds(owner, .{ .send = true, .recv = true, .call = true, .wait = true, .poll = true, .close = true }, .{}, 16);
        defer {
            detachPrincipalIpcBindings(owner);
            state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
            state.closeFdWithFreeList(owner, pair.a, &IpcCallFrontendFixture.pages) catch {};
            state.closeFdWithFreeList(owner, pair.b, &IpcCallFrontendFixture.pages) catch {};
        }
        const request = [_]u64{ 11, 22, 33, 44 };
        var call_ext: TestIpcRegisters = .{};
        var recv_ext: TestIpcRegisters = .{};
        var reply_ext: TestIpcRegisters = .{};
        const response = [_]u64{ 55, 66, 77, 88 };
        var transferred_in = [_]u64{0} ** 4;
        var transferred_out = [_]u64{0} ** 4;
        var moved_fd: ?kernel.Fd = null;
        if (scenario == 9 or scenario == 10 or scenario == 12 or scenario == 15 or scenario == 16) {
            const rights: kernel.FdRights = .{ .send = true, .recv = true, .transfer = true, .close = true };
            const attachment = try state.createIpcEndpointFd(owner, rights, .{}, 16);
            moved_fd = attachment;
            transferred_out = .{ attachment, kernel.fdRightsToBits(rights), 0, abi.transfer_move };
            reply_ext.send_array = @intFromPtr(&transferred_out);
            reply_ext.send_count = 1;
            call_ext.receive_array = @intFromPtr(&transferred_in);
            call_ext.receive_capacity = 1;
            if (scenario == 12 or scenario == 15 or scenario == 16) {
                call_ext.send_array = @intFromPtr(&transferred_out);
                call_ext.send_count = 1;
                reply_ext.send_array = 0;
                reply_ext.send_count = 0;
                recv_ext.receive_array = @intFromPtr(&transferred_in);
                recv_ext.receive_capacity = 1;
            }
        }
        if (scenario == 11 or scenario == 12 or scenario == 15 or scenario == 17) {
            if (scenario == 15) recv_ext.receive_capacity = 0;
            // Park the actual receiver first, then return the caller to its
            // running entity without executing privileged context installation.
            scheduler_state.thread_table.lock();
            initial_thread_contexts[4].ready = false;
            verifiedBlockThread(4);
            noteCpuIdleTick(0);
            scheduler_state.thread_table.unlock();
            try std.testing.expectEqual(@as(usize, 5), verifiedPickThreadForCpu(0).?.thread_index);
            setTestCurrentThread(&scheduler_state.cpus[0], 5);
            scheduler_state.cpus[0].is_idle = false;
            var waiting = std.mem.zeroes(TrapFrame);
            waiting.rdi = pair.b;
            recv_ext.apply(&waiting);
            waiting.rdx = 100;
            _ = ipc.recv(h, state, owner, &waiting);
            try std.testing.expect(!initial_thread_contexts[5].ready);
            if (scenario == 17) {
                initial_thread_contexts[6] = .{ .id = 6, .allocated = true, .ready = true, .scheduler_entity = &initial_scheduler_entities[6], .owner_process = owner };
                try std.testing.expect(verifiedAddThread(6, 1, true));
                try std.testing.expectEqual(@as(usize, 6), verifiedPickThreadForCpu(0).?.thread_index);
                setTestCurrentThread(&scheduler_state.cpus[0], 6);
                scheduler_state.cpus[0].is_idle = false;
                waiting.rdx = 100;
                _ = ipc.recv(h, state, owner, &waiting);
                try std.testing.expect(!initial_thread_contexts[6].ready);
                // An older queued message rules out direct delivery. Both
                // waiters are completed by the real queued wake collector.
                try state.ipcSend(owner, pair.a, .{}, &IpcCallFrontendFixture.pages);
            }
            scheduler_state.thread_table.lock();
            initial_thread_contexts[4].ready = true;
            verifiedWakeThread(4);
            scheduler_state.thread_table.unlock();
            try std.testing.expectEqual(@as(usize, 4), verifiedPickThreadForCpu(0).?.thread_index);
            setTestCurrentThread(&scheduler_state.cpus[0], 4);
            scheduler_state.cpus[0].is_idle = false;
            if (scenario == 12) IpcCallFrontendFixture.reject_write = recv_ext.receive_array;
            if (scenario == 17) IpcCallFrontendFixture.reject_write = @intFromPtr(&recv_ext.received_count);
        }
        var frame = std.mem.zeroes(TrapFrame);
        frame.rdi = pair.a;
        abi.outputWords(&frame, request);
        call_ext.apply(&frame);
        frame.rdx = 10;
        const call_status = ipc.call(h, state, owner, &frame);
        const caller = &initial_thread_contexts[4];
        if (scenario == 17) {
            // A poisoned obsolete count address cannot fault either empty
            // delivery. The queued word message and CALL reach distinct waiters.
            try std.testing.expectEqual(sc.syscall_ok, call_status);
            try std.testing.expectEqual(@as(u64, 1), caller.frame.rdx);
            try std.testing.expect(!caller.ready and caller.ipc_outgoing != null);
            try std.testing.expectEqual(sc.syscall_ok, initial_thread_contexts[5].frame.rax);
            try std.testing.expectEqual(sc.syscall_ok, initial_thread_contexts[6].frame.rax);
            try std.testing.expectEqual(@as(u64, 0), initial_thread_contexts[6].frame.r15);
            continue;
        }
        if (scenario == 12) {
            try std.testing.expectEqual(sc.syscall_err_invalid, call_status);
            try std.testing.expectEqual(@as(u64, 1), frame.rdx);
            try std.testing.expect(caller.ready and caller.ipc_outgoing == null);
            try std.testing.expectEqual(sc.syscall_err_invalid, initial_thread_contexts[5].frame.rax);
            try std.testing.expect(peekIpcImplicit(5, 17, owner) == null);
            try std.testing.expect(state.fdEntryConst(owner, moved_fd.?) == null);
            try std.testing.expectEqual(@as(usize, 0), (try state.ipcMessageQueueForSend((state.fdEntryConst(owner, pair.a) orelse unreachable).object)).len);
            continue;
        }
        try std.testing.expect(!caller.ready and caller.ipc_outgoing != null);
        try std.testing.expectEqual(@as(u64, 1), caller.frame.rdx);
        try std.testing.expectEqual(@as(usize, 5), verifiedPickThreadForCpu(0).?.thread_index);
        setTestCurrentThread(&scheduler_state.cpus[0], 5);
        scheduler_state.cpus[0].is_idle = false;
        if (scenario == 16) {
            try state.closeFdWithFreeList(owner, pair.a, &IpcCallFrontendFixture.pages);
            wakeIfTimerExpired(4, std.math.maxInt(u64));
            state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
            const remaining = try state.ipcMessageQueueForRecv((state.fdEntryConst(owner, pair.b) orelse unreachable).object);
            try std.testing.expectEqual(@as(usize, 0), remaining.len);
            continue;
        }
        if (scenario == 15) {
            try std.testing.expectEqual(sc.syscall_err_alloc, initial_thread_contexts[5].frame.rax);
            const pending = try state.ipcMessageQueueForRecv((state.fdEntryConst(owner, pair.b) orelse unreachable).object);
            try std.testing.expectEqual(@as(usize, 1), pending.len);
            try std.testing.expect(state.fdEntryConst(owner, moved_fd.?) == null);
            recv_ext.receive_capacity = 1;
        }
        if (scenario == 1) {
            wakeIfTimerExpired(4, std.math.maxInt(u64));
            state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
            try std.testing.expectEqual(sc.syscall_err_empty, ipc.recvMessage(h, state, owner, pair.b, .{ .active = true, .array = recv_ext.receive_array }, 0, 5, 17, 0, &initial_thread_contexts[5].frame));
            continue;
        }
        if (scenario == 11) {
            try std.testing.expectEqual(sc.syscall_ok, initial_thread_contexts[5].frame.rax);
            try std.testing.expectEqual(@as(usize, 0), (try state.ipcMessageQueueForSend((state.fdEntryConst(owner, pair.a) orelse unreachable).object)).len);
        } else try std.testing.expectEqual(sc.syscall_ok, ipc.recvMessage(h, state, owner, pair.b, .{ .active = true, .array = recv_ext.receive_array }, if (scenario == 15) 1 else 0, 5, 17, 0, &initial_thread_contexts[5].frame));
        if (scenario == 15) try state.closeFdWithFreeList(owner, @intCast(transferred_in[0]), &IpcCallFrontendFixture.pages);
        try std.testing.expectEqualSlices(u64, request[0..4], &abi.inputWords(&initial_thread_contexts[5].frame));
        const node = peekIpcImplicit(5, 17, owner).?;
        const body: *kernel.IpcReplySlot = @ptrCast(@alignCast(node.context.?));
        if (scenario == 18 or scenario == 19) {
            try state.ipcSend(owner, pair.a, .{ .words = .{ 101, 102, 103, 104 } }, &IpcCallFrontendFixture.pages);
            if (scenario == 19) IpcCallFrontendFixture.reject_write = @intFromPtr(&recv_ext.received_count);
            const ordinary = ipc.recvMessage(h, state, owner, pair.b, .{ .active = true, .array = recv_ext.receive_array }, 0, 5, 17, 0, &initial_thread_contexts[5].frame);
            try std.testing.expectEqual(sc.syscall_ok, ordinary);
            IpcCallFrontendFixture.reject_write = 0;
            try std.testing.expectEqual(node, peekIpcImplicit(5, 17, owner).?);
            try std.testing.expect(!caller.ready);
            if (scenario == 18) try std.testing.expectEqual(@as(u64, 101), initial_thread_contexts[5].frame.r10);
        }
        if (scenario == 20 or scenario == 21) {
            var waiting = std.mem.zeroes(TrapFrame);
            waiting.rdi = pair.b;
            recv_ext.apply(&waiting);
            waiting.rdx = 100;
            _ = ipc.recv(h, state, owner, &waiting);
            try std.testing.expect(!initial_thread_contexts[5].ready);
            initial_thread_contexts[6] = .{ .id = 6, .allocated = true, .ready = true, .scheduler_entity = &initial_scheduler_entities[6], .owner_process = owner };
            try std.testing.expect(verifiedAddThread(6, 1, true));
            try std.testing.expectEqual(@as(usize, 6), verifiedPickThreadForCpu(0).?.thread_index);
            setTestCurrentThread(&scheduler_state.cpus[0], 6);
            scheduler_state.cpus[0].is_idle = false;
            if (scenario == 21) IpcCallFrontendFixture.reject_write = @intFromPtr(&recv_ext.received_count);
            const ordinary = ipc.directSend(h, state, owner, pair.a, .{ .words = .{ 201, 202, 203, 204 } }, null).?;
            try std.testing.expectEqual(sc.syscall_ok, ordinary.status);
            IpcCallFrontendFixture.reject_write = 0;
            try std.testing.expectEqual(node, peekIpcImplicit(5, 17, owner).?);
            try std.testing.expect(!caller.ready);
            if (scenario == 20) try std.testing.expectEqual(@as(u64, 201), initial_thread_contexts[5].frame.r10);
            scheduler_state.thread_table.lock();
            initial_thread_contexts[6].ready = false;
            verifiedBlockThread(6);
            noteCpuIdleTick(0);
            scheduler_state.thread_table.unlock();
            try std.testing.expectEqual(@as(usize, 5), verifiedPickThreadForCpu(0).?.thread_index);
            setTestCurrentThread(&scheduler_state.cpus[0], 5);
            scheduler_state.cpus[0].is_idle = false;
        }
        if (scenario == 22) {
            const attachment = try state.createIpcEndpointFd(owner, .{ .send = true, .recv = true, .transfer = true, .close = true }, .{}, 16);
            defer state.closeFdWithFreeList(owner, attachment, &IpcCallFrontendFixture.pages) catch {};
            const descriptors = [_]kernel.IpcSendFd{.{ .fd = attachment, .rights = .{ .send = true, .recv = true, .close = true } }};
            const second = try state.ipcCall(owner, pair.a, .{ .words = .{ 301, 302, 303, 304 }, .fds = &descriptors }, .{ .owner = owner }, &IpcCallFrontendFixture.pages);
            const pending = try state.ipcMessageQueueForRecv(state.fdEntryConst(owner, pair.b).?.object);
            try std.testing.expectEqual(sc.syscall_err_invalid, ipc.recvMessage(h, state, owner, pair.b, .{ .active = true, .array = recv_ext.receive_array }, 0, 5, 17, 0, &initial_thread_contexts[5].frame));
            try std.testing.expectEqual(@as(usize, 1), pending.len);
            try std.testing.expectEqual(second, pending.peek().?.call);
            try std.testing.expectEqual(@as(usize, 1), pending.peek().?.fd_count);
            try std.testing.expectEqual(@as(usize, 2), state.ipcReplySlot(second).?.lifetime_refs.load(.acquire));
            try std.testing.expectEqual(node, peekIpcImplicit(5, 17, owner).?);
            state.ipcCancelCall(second, &IpcCallFrontendFixture.pages);
            state.releaseIpcCallReference(second);
            try std.testing.expect(state.ipcReplySlot(second) == null);
            try std.testing.expectEqual(@as(usize, 0), pending.len);
        }
        if (scenario == 13) {
            const ref = body.ref;
            detachPrincipalIpcBindings(owner);
            state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
            try std.testing.expect(state.ipcReplySlot(ref) == null);
            try std.testing.expect(takePendingIpcCancellation() == null);
            continue;
        }
        if (scenario == 14) {
            try std.testing.expect(releaseThreadGeneration(5, 17, owner));
            state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
            try std.testing.expect(caller.ready);
            try std.testing.expectEqual(sc.syscall_err_closed, caller.frame.rax);
            try std.testing.expectEqual(@as(u64, 0), caller.frame.r10);
            continue;
        }
        var reply_frame = std.mem.zeroes(TrapFrame);
        abi.outputWords(&reply_frame, response);
        reply_ext.apply(&reply_frame);
        if (scenario == 2) {
            wakeIfTimerExpired(4, std.math.maxInt(u64));
            state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
            try std.testing.expectEqual(sc.syscall_err_closed, ipc.reply(h, state, owner, abi.implicit_reply, &reply_frame).status);
            try std.testing.expectEqual(@as(u64, 0), caller.frame.r10);
            continue;
        }
        if (scenario == 7) {
            const extracted: kernel.Fd = @intCast(ipc.extract(state, owner));
            const duplicate = try state.dupFd(owner, extracted, 16, @TypeOf(state.*).replySendRights(), .{});
            try state.closeFdWithFreeList(owner, extracted, &IpcCallFrontendFixture.pages);
            const attachment = [_]kernel.IpcSendFd{.{ .fd = duplicate, .rights = @TypeOf(state.*).replySendRights() }};
            try state.ipcSend(owner, pair.a, .{ .words = .{ 1, 2, 3, 4 }, .fds = &attachment }, &IpcCallFrontendFixture.pages);
            try state.closeFdWithFreeList(owner, duplicate, &IpcCallFrontendFixture.pages);
            try std.testing.expect(!caller.ready); // Queue retain is still an authority.
            const transferred = try state.ipcRecv(owner, pair.b, 1, 16, &IpcCallFrontendFixture.pages);
            try std.testing.expect(!caller.ready);
            try state.closeFdWithFreeList(owner, transferred.fds[0].fd, &IpcCallFrontendFixture.pages);
            try std.testing.expect(caller.ready);
            try std.testing.expectEqual(sc.syscall_err_closed, caller.frame.rax);
            continue;
        }
        if (scenario == 8) {
            var lower = std.mem.zeroes(TrapFrame);
            lower.rdi = pair.b;
            abi.outputWords(&lower, request);
            lower.rdx = 0;
            try std.testing.expectEqual(sc.syscall_err_not_ready, ipc.call(h, state, owner, &lower));
            try std.testing.expectEqual(@as(u64, 1), lower.rdx);
            state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
            try std.testing.expectEqual(node, peekIpcImplicit(5, 17, owner).?);
            try std.testing.expect(!caller.ready);
        }
        if (scenario == 6) {
            // Invalid destination fails FD installation after object allocation.
            // The original implicit binding and caller remain live for retry.
            try std.testing.expectError(error.InvalidState, state.ipcExtractReply(@enumFromInt(std.math.maxInt(u32)), body.ref, 16));
            try std.testing.expectEqual(node, peekIpcImplicit(5, 17, owner).?);
            try std.testing.expect(!caller.ready);
        }
        var reply_fd = abi.implicit_reply;
        if (scenario == 3) {
            reply_fd = ipc.extract(state, owner);
            try std.testing.expect(reply_fd >= 16 and reply_fd < 1000);
            var bypass = std.mem.zeroes(TrapFrame);
            bypass.rax = sc.syscall_ipc_send;
            bypass.rdi = reply_fd;
            abi.outputWords(&bypass, response);
            reply_ext.apply(&bypass);
            try std.testing.expectEqual(sc.syscall_err_invalid, @import("syscall/native_ipc.zig").dispatch(h, state, owner, &bypass).?);
            try std.testing.expect(!caller.ready);
            try std.testing.expect(peekIpcImplicit(5, 17, owner) == null);
            try std.testing.expect(releaseThreadGeneration(5, 17, owner));
            state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
            try std.testing.expect(!caller.ready);
            clearCurrentContextLocked(&scheduler_state.cpus[0]);
            initial_thread_contexts[6] = .{ .id = 6, .allocated = true, .ready = true, .scheduler_entity = &initial_scheduler_entities[6], .owner_process = owner };
            try std.testing.expect(verifiedAddThread(6, 1, true));
            try std.testing.expectEqual(@as(usize, 6), verifiedPickThreadForCpu(0).?.thread_index);
            setTestCurrentThread(&scheduler_state.cpus[0], 6);
            scheduler_state.cpus[0].is_idle = false;
        }
        if (scenario == 4) IpcCallFrontendFixture.reject_write = @intFromPtr(&call_ext.received_count);
        if (scenario == 9) IpcCallFrontendFixture.reject_write = call_ext.receive_array;
        if (scenario == 23) {
            var combined = std.mem.zeroes(TrapFrame);
            combined.rdi = pair.b;
            abi.outputWords(&combined, response);
            recv_ext.apply(&combined);
            combined.rdx = 100;
            _ = ipc.replyRecv(h, state, owner, &combined);
            try std.testing.expectEqual(@as(u32, 4), IpcCallFrontendFixture.preferred_target.?.thread_index);
            try std.testing.expect(!initial_thread_contexts[5].ready);
            try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[5].frame.rdx);
            try std.testing.expectEqual(@as(usize, 0), state.fd_wait_groups.live);
            const block = ipcBlock(5, 17, owner).?;
            try std.testing.expect(block.receive_list != null and block.receive.state == .bound);
        } else if (scenario == 5) {
            var combined = std.mem.zeroes(TrapFrame);
            combined.rdi = pair.b;
            abi.outputWords(&combined, response);
            combined.rsi = 4;
            combined.rdx = 0;
            try std.testing.expectEqual(sc.syscall_err_invalid, ipc.replyRecv(h, state, owner, &combined));
            try std.testing.expectEqual(@as(u64, 1), combined.rdx);
        } else {
            const result = ipc.reply(h, state, owner, reply_fd, &reply_frame);
            try std.testing.expect(result.committed);
            try std.testing.expectEqual(if (scenario == 9) sc.syscall_err_invalid else sc.syscall_ok, result.status);
        }
        try std.testing.expect(caller.ready);
        try std.testing.expectEqual(@as(u64, 1), caller.frame.rdx);
        try std.testing.expectEqual(if (scenario == 9) sc.syscall_err_invalid else sc.syscall_ok, caller.frame.rax);
        if (scenario != 9) try std.testing.expectEqualSlices(u64, response[0..4], &abi.inputWords(&caller.frame));
        try std.testing.expect(ipc.reply(h, state, owner, reply_fd, &reply_frame).status != sc.syscall_ok);
        if (scenario == 3) try state.closeFdWithFreeList(owner, @intCast(reply_fd), &IpcCallFrontendFixture.pages);
        if (if (scenario == 15) null else moved_fd) |attachment| {
            try std.testing.expect((state.fdEntryConst(owner, attachment) != null) == (scenario == 9));
            if (scenario == 9) {
                try state.closeFdWithFreeList(owner, attachment, &IpcCallFrontendFixture.pages);
            } else {
                try std.testing.expectEqual(@as(u64, 1), caller.frame.r15);
                try state.closeFdWithFreeList(owner, @intCast(transferred_in[0]), &IpcCallFrontendFixture.pages);
            }
        }
    }
}

test "AP block preferred caller preserves competing EEVDF choice and saved commitment" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    for (0..3) |scenario| {
        try initializeDirectBlockTest(true);
        initial_thread_contexts[4].ready = false;
        initial_thread_contexts[4].frame.rdx = 1;
        syncHotStateFromContext(4);
        verifiedBlockThread(4);
        if (scenario == 2) {
            initial_thread_contexts[6] = .{ .id = 6, .allocated = true, .ready = true, .cr3 = 0x3000, .scheduler_entity = &initial_scheduler_entities[6] };
            syncHotStateFromContext(6);
            try std.testing.expect(verifiedAddThread(6, 1, true));
        }
        var frame = std.mem.zeroes(TrapFrame);
        scheduler_state.thread_table.lock();
        const state = &scheduler_state.cpus[0];
        state.runqueue_lock.lock();
        const expected = state.runqueue.bestEligible(state.virtual_time) orelse state.runqueue.bestEligible(state.runqueue.minimumEligibleTime().?).?;
        const expected_id = expected.thread_index;
        state.runqueue_lock.unlock();
        const installed = installNextLocalAfterBlockLocked(ContextTransferTestRegisters, 0, &frame, null, .{ .thread_index = 5, .thread_generation = if (scenario == 1) 16 else 17 });
        scheduler_state.thread_table.unlock();
        try std.testing.expect(installed);
        try std.testing.expectEqual(expected_id, currentThread());
        try std.testing.expectEqual(.blocked, initial_scheduler_entities[4].ownership);
        try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[4].frame.rdx);
    }
}

/// K owns delivery; T validates the claimed generation/token before staging.
/// Publication (including a stop or delayed runqueue admission) carries words
/// with the status. A completion before block is consumed by both park paths.
// K owns group/subscription delivery; T validates the terminal publication claim.
pub fn stagePollResult(target: kernel.ThreadWakeTarget, index: u64, revents: u64) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(target.thread_index) orelse return false;
    if (!ctx.allocated or ctx.owner_process != target.owner or ctx.generation != target.thread_generation or
        ctx.active_wait_token != target.wait_token or !ctx.wait_completion_claimed) return false;
    ctx.wait_poll_result = .{ .index = index, .revents = revents };
    return true;
}

pub fn stageIpcWords(target: kernel.ThreadWakeTarget, words: [4]u64) bool {
    return stageIpcResult(target, words, 0);
}
pub fn stageIpcResult(target: kernel.ThreadWakeTarget, words: [4]u64, fd_count: u8) bool {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(target.thread_index) orelse return false;
    if (!ctx.allocated or ctx.owner_process != target.owner or
        ctx.generation != target.thread_generation or ctx.active_wait_token != target.wait_token or
        !ctx.wait_completion_claimed) return false;
    ctx.wait_ipc_words = words;
    ctx.wait_ipc_fd_count = fd_count;
    return true;
}

/// Retained subscriptions never store user pointers. Only a terminal claim
/// lends the blocked invocation's output VA; timeout/signal/exit end that loan
/// without walking the independent, persistent observer nodes.
pub fn claimedWaitOutput(target: kernel.ThreadWakeTarget) ?u64 {
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = threadContextLocked(target.thread_index) orelse return null;
    if (!ctx.allocated or ctx.owner_process != target.owner or
        ctx.generation != target.thread_generation or ctx.active_wait_token != target.wait_token or
        !ctx.wait_completion_claimed) return null;
    return ctx.frame.rdi;
}

test "thread storage IPC register codec words extensions capacity and faults" {
    const abi = @import("kernel_abi_root").ipc_abi;
    const ipc = @import("syscall/native_ipc.zig").WaitTestSupport;
    const Memory = struct {
        var accesses: usize = 0;
        fn copy(_: kernel.PrincipalId, address: u64, bytes: []u8) bool {
            accesses += 1;
            if (address < 4096) return false;
            @memcpy(bytes, @as([*]const u8, @ptrFromInt(address))[0..bytes.len]);
            return true;
        }
    };
    const hooks = .{ .copy_user_bytes_from_va = Memory.copy };
    var descriptors: [kernel.max_ipc_message_fds]kernel.IpcSendFd = undefined;
    for (0..5) |used| {
        var frame = std.mem.zeroes(TrapFrame);
        var words: [4]u64 = .{ 0, 0, 0, 0 };
        for (0..used) |i| words[i] = 0x1234567800000000 + i;
        abi.outputWords(&frame, words);
        Memory.accesses = 0;
        const msg = ipc.readMessage(hooks, default_process_principal, &frame, &descriptors).?;
        try std.testing.expectEqualSlices(u64, &words, &msg.words);
        try std.testing.expectEqual(@as(usize, 0), msg.fds.len);
        const receive = ipc.readReceive(&frame).?;
        try std.testing.expect(receive.memory.active and receive.capacity == 0);
        try std.testing.expectEqual(@as(usize, 0), Memory.accesses);
        frame.rdx = 1;
        abi.outputWords(&frame, .{ 0, 0, 0, 0 });
        try std.testing.expectEqual(@as(u64, 1), frame.rdx);
    }
    var fd_words = [_]u64{ 31, 0, 0, abi.transfer_move };
    var ext: TestIpcRegisters = .{ .send_array = @intFromPtr(&fd_words), .send_count = 1, .receive_array = 0x8000, .receive_capacity = 1 };
    var frame = std.mem.zeroes(TrapFrame);
    ext.apply(&frame);
    const msg = ipc.readMessage(hooks, default_process_principal, &frame, &descriptors).?;
    try std.testing.expectEqual(@as(usize, 1), msg.fds.len);
    try std.testing.expect(msg.fds[0].move and msg.fds[0].fd == 31);
    const snapshot = ipc.readReceive(&frame).?;
    ext.receive_array = 0x9000;
    ext.receive_capacity = 0;
    try std.testing.expectEqual(@as(u64, 0x8000), snapshot.memory.array);
    try std.testing.expectEqual(@as(u8, 1), snapshot.capacity);
    ext.receive_capacity = 20;
    ext.apply(&frame);
    try std.testing.expect(ipc.readReceive(&frame) == null);
    ext.receive_capacity = 1;
    ext.receive_array = std.math.maxInt(u64) - 15;
    ext.apply(&frame);
    try std.testing.expect(ipc.readReceive(&frame) == null);
    ext.send_array = 1;
    ext.apply(&frame);
    try std.testing.expect(ipc.readMessage(hooks, default_process_principal, &frame, &descriptors) == null);
    frame.r13 = 1;
    try std.testing.expect(ipc.readMessage(hooks, default_process_principal, &frame, &descriptors) == null);
    try std.testing.expect(ipc.readReceive(&frame) == null);
    frame.r13 = std.math.maxInt(u64) - 8;
    try std.testing.expect(ipc.readMessage(hooks, default_process_principal, &frame, &descriptors) == null);
}

test "thread storage IPC register completion before BSP and AP block preserves words and commitment" {
    const abi = @import("kernel_abi_root").ipc_abi;
    defer initializeStaticStorage();
    for (0..2) |path| {
        for ([_]u64{ 0, 1 }) |status| {
            try initializeDirectBlockTest(false);
            const ctx = &initial_thread_contexts[4];
            const token = reserveCurrentWaitToken(ctx.generation).?;
            var target: kernel.ThreadWakeTarget = .{ .owner = ctx.owner_process, .thread_index = 4, .thread_generation = ctx.generation, .wait_token = token };
            try std.testing.expect(!stageIpcWords(target, .{ 1, 2, 3, 4 }));
            try std.testing.expectEqual(.claimed, claimWaitTokenCompletion(4, ctx.generation, ctx.owner_process, token));
            target.thread_generation +%= 1;
            try std.testing.expect(!stageIpcWords(target, .{ 1, 2, 3, 4 }));
            target.thread_generation = ctx.generation;
            try std.testing.expect(stageIpcResult(target, .{ 1, 2, 3, 4 }, 3));
            try std.testing.expectEqual(.published, publishClaimedWaitCompletion(4, ctx.generation, ctx.owner_process, token, status));
            var frame = std.mem.zeroes(TrapFrame);
            frame.rdx = 1;
            const completed = if (path == 0) blockCurrentThread(&frame, true, false, token, 0, 3, null) else parkApThreadForBlock(&frame, true, false, token, 0, 3, null);
            try std.testing.expect(completed);
            try std.testing.expectEqual(status, frame.rax);
            try std.testing.expectEqual(@as(u64, if (status == 0) 3 else 0), frame.r15);
            try std.testing.expectEqual(@as(u64, 1), frame.rdx);
            const expected: [4]u64 = if (status == 0) .{ 1, 2, 3, 4 } else .{ 0, 0, 0, 0 };
            try std.testing.expectEqualSlices(u64, &expected, &abi.inputWords(&frame));
        }
    }
}

test "thread storage poll register completion before BSP AP and fast block" {
    defer initializeStaticStorage();
    for (0..3) |path| {
        try initializeDirectBlockTest(false);
        const ctx = &initial_thread_contexts[4];
        const token = reserveCurrentWaitToken(ctx.generation).?;
        var target: kernel.ThreadWakeTarget = .{ .owner = ctx.owner_process, .thread_index = 4, .thread_generation = ctx.generation, .wait_token = token };
        try std.testing.expect(!stagePollResult(target, 199, 4));
        try std.testing.expectEqual(.claimed, claimWaitTokenCompletion(4, ctx.generation, ctx.owner_process, token));
        target.thread_generation +%= 1;
        try std.testing.expect(!stagePollResult(target, 199, 4));
        target.thread_generation = ctx.generation;
        target.wait_token += 1;
        try std.testing.expect(!stagePollResult(target, 199, 4));
        target.wait_token = token;
        try std.testing.expect(stagePollResult(target, 199, 4));
        try std.testing.expectEqual(.stale, claimWaitTokenCompletion(4, ctx.generation, ctx.owner_process, token));
        try std.testing.expectEqual(.published, publishClaimedWaitCompletion(4, ctx.generation, ctx.owner_process, token, 1));
        var frame = std.mem.zeroes(TrapFrame);
        const completed = switch (path) {
            0 => blockCurrentThread(&frame, true, false, token, 0, 3, null),
            1 => parkApThreadForBlock(&frame, true, false, token, 0, 3, null),
            else => tryIpcCompletedWait(&frame, token),
        };
        try std.testing.expect(completed);
        try std.testing.expectEqual(@as(u64, 1), frame.rax);
        try std.testing.expectEqual(@as(u64, 199), frame.r10);
        try std.testing.expectEqual(@as(u64, 4), frame.r8);
        try std.testing.expect(!stagePollResult(target, 0, 1));
        try std.testing.expect(ctx.wait_poll_result == null);
    }
}

// These use the actual native frontend and adapter rather than a model queue.
// Keeping the original thread slot while K drains cancellation reproduces the
// lifetime boundary that made ThreadContext-embedded nodes unsafe.
test "thread storage IPC stable block fast call reply receive and unchanged fallback" {
    const abi = @import("kernel_abi_root").ipc_abi;
    const runtime_abi = @import("kernel_abi_root").runtime_abi;
    const ipc = @import("syscall/native_ipc.zig").WaitTestSupport;
    const saved = try std.testing.allocator.dupe(ThreadContext, &initial_thread_contexts);
    defer {
        @memcpy(&initial_thread_contexts, saved);
        std.testing.allocator.free(saved);
        initializeStaticStorage();
    }
    const storage = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(storage);
    try std.testing.expect(kernel.initRuntimeStorage(storage));
    for (0..5) |completion| {
        const state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        defer state.deinitPools();
        try initializeDirectBlockTest(true);
        WaitAdmissionTest.stop_after_block = true;
        defer WaitAdmissionTest.stop_after_block = false;
        const owner = default_process_principal;
        const h = IpcCallFrontendFixture.hooks();
        IpcCallFrontendFixture.reject_write = 0;
        // Writable debt is K-owned and can outlive the last cancellation node.
        // The atomic queue hint alone must not suppress its final drain.
        try std.testing.expect(!hasPendingIpcCancellations());
        state.ipc_cancel_writable_pending = true;
        // K-free runtime calls fall through the ordinary IPC probe. It must
        // leave K-owned cancellation work for the next K owner.
        for ([_]u64{ runtime_abi.syscall_getpid, runtime_abi.syscall_nanosleep, runtime_abi.syscall_futex_wait, runtime_abi.syscall_futex_wake, runtime_abi.syscall_futex_wake_wait, runtime_abi.syscall_futex_requeue, runtime_abi.syscall_futex_wake_op }) |nr| {
            var probe = std.mem.zeroes(TrapFrame);
            probe.rax = nr;
            try std.testing.expect(@import("syscall/native_ipc.zig").dispatchAfterScalarMiss(h, state, owner, &probe) == null);
            try std.testing.expect(state.ipc_cancel_writable_pending);
        }
        @import("syscall/native_ipc.zig").drainCancellations(h, state);
        try std.testing.expect(!state.ipc_cancel_writable_pending);

        const pair = try state.createIpcChannelPairFds(owner, .{ .send = true, .recv = true, .call = true, .wait = true, .poll = true, .close = true }, .{}, 16);
        defer {
            detachPrincipalIpcBindings(owner);
            state.drainIpcCancellations(h.free_list);
            state.closeFdWithFreeList(owner, pair.a, h.free_list) catch {};
            state.closeFdWithFreeList(owner, pair.b, h.free_list) catch {};
        }
        const Fail = struct {
            fn acquire(_: *anyopaque, _: usize) ?@import("memory/owned_storage.zig").MappedExtent {
                return null;
            }
            fn release(_: *anyopaque, _: @import("memory/owned_storage.zig").PhysicalExtent) bool {
                return false;
            }
        };
        var fail_context: u8 = 0;
        const original_allocator = state.pool_allocator;
        state.pool_allocator = .{ .context = &fail_context, .acquire = Fail.acquire, .release = Fail.release };
        try std.testing.expectError(error.OutOfFreePages, state.ensureIpcBlock(owner, 4, initial_thread_contexts[4].generation));
        state.pool_allocator = original_allocator;
        state.wait_storage_cache = .{};
        const caller_block = try state.ensureIpcBlock(owner, 4, initial_thread_contexts[4].generation);
        scheduler_state.thread_table.lock();
        initial_thread_contexts[4].ready = false;
        verifiedBlockThread(4);
        noteCpuIdleTick(0);
        scheduler_state.thread_table.unlock();
        try std.testing.expectEqual(@as(usize, 5), verifiedPickThreadForCpu(0).?.thread_index);
        setTestCurrentThread(&scheduler_state.cpus[0], 5);
        scheduler_state.cpus[0].is_idle = false;
        var receive = std.mem.zeroes(TrapFrame);
        receive.rdi = pair.b;
        receive.rbx = 1; // unmapped: FD-less delivery must never probe it
        receive.r15 = 19 << 32;
        receive.rdx = std.math.maxInt(u64);
        _ = ipc.recv(h, state, owner, &receive);
        const receiver_block = ipcBlock(5, initial_thread_contexts[5].generation, owner).?;
        const list = receiver_block.receive_list.?;
        try std.testing.expectEqual(@as(usize, 0), state.fd_wait_groups.live);
        try std.testing.expectEqual(receiver_block, list.receive_head.?);
        scheduler_state.thread_table.lock();
        initial_thread_contexts[4].ready = true;
        verifiedWakeThread(4);
        scheduler_state.thread_table.unlock();
        try std.testing.expectEqual(@as(usize, 4), verifiedPickThreadForCpu(0).?.thread_index);
        setTestCurrentThread(&scheduler_state.cpus[0], 4);
        scheduler_state.cpus[0].is_idle = false;
        var call = std.mem.zeroes(TrapFrame);
        call.rax = abi.syscall_ipc_call;
        call.rdi = pair.a;
        call.r13 = 1; // ignored when no FD is sent
        call.rbx = 1;
        call.r15 = 19 << 32;
        call.rdx = std.math.maxInt(u64);
        abi.outputWords(&call, .{ 11, 22, 33, 44 });
        // A timed-out FD wait may leave K-owned storage after T cleared its
        // token. Without the new admission check this fully valid CALL would
        // commit and skip the ordinary stale poll-array cleanup.
        const retained = try state.beginFdWaitGroup(owner, 4, initial_thread_contexts[4].generation, 991);
        try state.armFdWaitGroup(retained);
        const retained_frame = call;
        try std.testing.expect(ipc.fast(h, state, owner, &call) == null);
        try std.testing.expectEqualDeep(retained_frame, call);
        try std.testing.expectEqual(receiver_block, list.receive_head.?);
        try std.testing.expect(initial_thread_contexts[4].ipc_outgoing == null);
        state.cancelFdWaitGroupsForThread(owner, 4, initial_thread_contexts[4].generation);
        try std.testing.expectEqual(@as(usize, 0), state.fd_wait_groups.live);
        try state.ipcSend(owner, pair.a, .{ .words = .{ 9, 8, 7, 6 } }, h.free_list);
        const queued_frame = call;
        try std.testing.expect(ipc.fast(h, state, owner, &call) == null);
        try std.testing.expectEqualDeep(queued_frame, call);
        try std.testing.expectEqual(receiver_block, list.receive_head.?);
        const older = try state.ipcRecv(owner, pair.b, 0, 16, h.free_list);
        try std.testing.expectEqual(@as(u64, 9), older.words[0]);
        const group = try state.beginFdWaitGroup(owner, 6, 1, 987);
        try std.testing.expect(try state.registerIpcPollWaitersForFd(owner, pair.b, 1, 0x4000, 6, 1, 987, group));
        try state.armFdWaitGroup(group);
        try std.testing.expect(ipc.fast(h, state, owner, &call) == null);
        try std.testing.expectEqualDeep(queued_frame, call);
        try std.testing.expectEqual(receiver_block, list.receive_head.?);
        state.cancelFdWaitGroup(group, 987);
        // Each rejected preflight must preserve source registers, FIFO and refs.
        for (0..6) |reason| {
            var rejected = call;
            switch (reason) {
                0 => rejected.rdx = 1,
                1 => rejected.r15 = 1,
                2 => rejected.rsi = 0x8000,
                3 => initial_thread_contexts[5].cpu_slot = 1,
                4 => initial_thread_contexts[5].pending_signal_mask = 1,
                5 => initial_thread_contexts[4].ipc_block_generation +%= 1,
                else => unreachable,
            }
            const before = rejected;
            try std.testing.expect(ipc.fast(h, state, owner, &rejected) == null);
            try std.testing.expectEqualDeep(before, rejected);
            try std.testing.expectEqual(receiver_block, list.receive_head.?);
            try std.testing.expectEqual(@as(usize, 1), caller_block.lifetime_refs.load(.acquire));
            initial_thread_contexts[5].cpu_slot = 0;
            initial_thread_contexts[5].pending_signal_mask = 0;
            initial_thread_contexts[4].ipc_block_generation = caller_block.ref.generation;
        }
        RuntimeAccountingTest.now_ns = 1000;
        defer RuntimeAccountingTest.now_ns = null;
        scheduler_state.cpus[0].runtime_accounting.start(0);
        state.pool_allocator = .{ .context = &fail_context, .acquire = Fail.acquire, .release = Fail.release };
        defer state.pool_allocator = original_allocator;
        try std.testing.expect(ipc.fast(h, state, owner, &call) != null);
        try std.testing.expect(list.receive_head == null);
        try std.testing.expectEqualSlices(u64, &.{ 11, 22, 33, 44 }, &abi.inputWords(&initial_thread_contexts[5].frame));
        try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[4].frame.rdx);
        try std.testing.expect(initial_thread_contexts[4].ipc_outgoing == &caller_block.outgoing);
        state.drainIpcCancellations(h.free_list);
        try std.testing.expectEqual(@as(usize, 5), currentThread());
        try std.testing.expectEqual(@as(i64, 1000), initial_scheduler_entities[4].entity.service_ns);
        RuntimeAccountingTest.now_ns = 2500;
        if (completion == 1) {
            // The caller's slot may retire while the server owns its own entity.
            try std.testing.expect(releaseThreadGeneration(4, initial_thread_contexts[4].generation, owner));
            state.drainIpcCancellations(h.free_list);
            var late = std.mem.zeroes(TrapFrame);
            try std.testing.expectEqual(@import("syscall/numbers.zig").syscall_err_closed, ipc.reply(h, state, owner, abi.implicit_reply, &late).status);
            try std.testing.expectEqual(@as(usize, 5), currentThread());
            continue;
        }
        if (completion == 2) {
            try std.testing.expect(releaseThreadGeneration(5, initial_thread_contexts[5].generation, owner));
            state.drainIpcCancellations(h.free_list);
            try std.testing.expect(initial_thread_contexts[4].ready);
            try std.testing.expectEqual(@import("syscall/numbers.zig").syscall_err_closed, initial_thread_contexts[4].frame.rax);
            continue;
        }
        if (completion == 3) {
            const extracted = ipc.extract(state, owner);
            try std.testing.expect(extracted >= 16 and extracted < 1000);
            try std.testing.expect(!initial_thread_contexts[4].ready);
            try std.testing.expectEqual(.running, initial_scheduler_entities[5].ownership);
            var late = std.mem.zeroes(TrapFrame);
            abi.outputWords(&late, .{ 55, 66, 77, 88 });
            try std.testing.expect(ipc.reply(h, state, owner, extracted, &late).committed);
            try std.testing.expect(initial_thread_contexts[4].ready);
            try state.closeFdWithFreeList(owner, @intCast(extracted), h.free_list);
            continue;
        }
        if (completion == 4) {
            // A nested call with no receiver takes the ordinary queued/cancel
            // route without losing the upper caller's implicit reply authority.
            const upper = initial_thread_contexts[5].ipc_implicit;
            var lower = std.mem.zeroes(TrapFrame);
            lower.rdi = pair.a;
            lower.rdx = 0;
            try std.testing.expectEqual(@import("syscall/numbers.zig").syscall_err_not_ready, ipc.call(h, state, owner, &lower));
            state.drainIpcCancellations(h.free_list);
            try std.testing.expectEqual(upper, initial_thread_contexts[5].ipc_implicit);
            try std.testing.expect(!initial_thread_contexts[4].ready);
        }
        var reply = std.mem.zeroes(TrapFrame);
        reply.rax = abi.syscall_ipc_reply_recv;
        reply.rdi = pair.b;
        reply.rbx = 1;
        reply.r15 = 19 << 32;
        reply.rdx = std.math.maxInt(u64);
        abi.outputWords(&reply, .{ 55, 66, 77, 88 });
        try std.testing.expect(ipc.fast(h, state, owner, &reply) != null);
        try std.testing.expectEqual(receiver_block, list.receive_head.?);
        try std.testing.expectEqual(@as(u8, 19), receiver_block.receive_target.recv_fd_capacity);
        try std.testing.expectEqual(@as(u64, 0), initial_thread_contexts[4].frame.r15);
        try std.testing.expectEqualSlices(u64, &.{ 55, 66, 77, 88 }, &abi.inputWords(&initial_thread_contexts[4].frame));
        try std.testing.expectEqual(@as(usize, 1), caller_block.lifetime_refs.load(.acquire));
        try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[5].frame.rdx);
        try std.testing.expectEqual(@as(usize, 0), state.fd_wait_groups.live);
        try std.testing.expectEqual(@as(usize, 4), currentThread());
        try std.testing.expectEqual(@as(i64, 1500), initial_scheduler_entities[5].entity.service_ns);
    }
}

test "thread storage IPC warm queued call failure cancellation close extraction and reply" {
    const abi = @import("kernel_abi_root").ipc_abi;
    const ipc = @import("syscall/native_ipc.zig").WaitTestSupport;
    const saved = try std.testing.allocator.dupe(ThreadContext, &initial_thread_contexts);
    defer {
        @memcpy(&initial_thread_contexts, saved);
        std.testing.allocator.free(saved);
        initializeStaticStorage();
    }
    const storage = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(storage);
    try std.testing.expect(kernel.initRuntimeStorage(storage));
    for (0..4) |completion| {
        const state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        defer state.deinitPools();
        try initializeDirectBlockTest(true);
        RuntimeAccountingTest.now_ns = 1000;
        defer RuntimeAccountingTest.now_ns = null;
        scheduler_state.cpus[0].runtime_accounting.start(0);
        WaitAdmissionTest.stop_after_block = true;
        defer WaitAdmissionTest.stop_after_block = false;
        const owner = default_process_principal;
        const h = IpcCallFrontendFixture.hooks();
        IpcCallFrontendFixture.reject_write = 1;
        defer IpcCallFrontendFixture.reject_write = 0;
        const pair = try state.createIpcChannelPairFds(owner, .{ .send = true, .recv = true, .call = true, .wait = true, .poll = true, .close = true }, .{}, 16);
        defer {
            detachPrincipalIpcBindings(owner);
            state.drainIpcCancellations(h.free_list);
            state.closeFdWithFreeList(owner, pair.a, h.free_list) catch {};
            state.closeFdWithFreeList(owner, pair.b, h.free_list) catch {};
        }
        const body = try state.ensureIpcBlock(owner, 4, initial_thread_contexts[4].generation);
        _ = try state.ensureIpcBlock(owner, 5, 17);
        try std.testing.expect(state.noteIpcReceiverForFd(owner, pair.b, 5, 17));
        const queue = try state.ipcMessageQueueForSend(state.fdEntryConst(owner, pair.a).?.object);
        var call = std.mem.zeroes(TrapFrame);
        call.rax = abi.syscall_ipc_call;
        call.rdi = pair.a;
        call.rdx = std.math.maxInt(u64);
        call.r13 = 1;
        call.rbx = 1;
        call.r15 = 19 << 32;
        abi.outputWords(&call, .{ 11, 22, 33, 44 });
        const before = call;
        queue.reference_sequence = std.math.maxInt(u64);
        try std.testing.expect(ipc.fast(h, state, owner, &call) == null);
        try std.testing.expectEqualDeep(before, call);
        try std.testing.expectEqual(@as(usize, 1), body.lifetime_refs.load(.acquire));
        try std.testing.expect(initial_thread_contexts[4].ipc_outgoing == null);
        queue.reference_sequence = 0;
        try state.ipcSend(owner, pair.a, .{ .words = .{ 9, 8, 7, 6 } }, h.free_list);
        try std.testing.expect(ipc.fast(h, state, owner, &call) == null);
        try std.testing.expectEqualDeep(before, call);
        const older = try state.ipcRecv(owner, pair.b, 0, 16, h.free_list);
        try std.testing.expectEqual(@as(u64, 9), older.words[0]);
        // Poll completion must not probe the registered user address.
        const poll_token: u64 = 991;
        const poll_base: u64 = 0x8000;
        initial_thread_contexts[5].active_wait_token = poll_token;
        initial_thread_contexts[5].wait_mailbox = true;
        const poll_group = try state.beginFdWaitGroup(owner, 5, 17, poll_token);
        state.fd_wait_groups.get(poll_group.index).?.pollfds_va = poll_base;
        try std.testing.expect(try state.registerIpcPollWaitersForFd(owner, pair.b, 1, poll_base + 199 * 32, 5, 17, poll_token, poll_group));
        try state.armFdWaitGroup(poll_group);
        const extra_group = try state.beginFdWaitGroup(owner, 6, 1, 999);
        try std.testing.expect(try state.registerIpcPollWaitersForFd(owner, pair.b, 1, poll_base, 6, 1, 999, extra_group));
        try state.armFdWaitGroup(extra_group);
        try std.testing.expect(ipc.fast(h, state, owner, &call) == null);
        try std.testing.expectEqualDeep(before, call);
        try std.testing.expect(queue.isEmpty());
        state.cancelFdWaitGroup(extra_group, 999);
        var send = call;
        send.rax = abi.syscall_ipc_send;
        send.rdx = 0;
        IpcCallFrontendFixture.user_accesses = 0;
        try std.testing.expectEqual(@as(?u64, 0), ipc.fast(h, state, owner, &send));
        try std.testing.expectEqual(@as(usize, 0), IpcCallFrontendFixture.user_accesses);
        try std.testing.expectEqual(@as(u64, 199), initial_thread_contexts[5].frame.r10);
        try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[5].frame.r8);
        setTestCurrentThread(&scheduler_state.cpus[0], 5);
        var poll_return = std.mem.zeroes(TrapFrame);
        try std.testing.expect(tryIpcCompletedWait(&poll_return, poll_token));
        try std.testing.expectEqual(@as(u64, 199), poll_return.r10);
        try std.testing.expectEqual(@as(u64, 1), poll_return.r8);
        setTestCurrentThread(&scheduler_state.cpus[0], 4);
        _ = try state.ipcRecv(owner, pair.b, 0, 16, h.free_list);
        scheduler_state.thread_table.lock();
        initial_thread_contexts[5].ready = false;
        initial_thread_contexts[5].wait_mailbox = true;
        initial_thread_contexts[5].active_wait_token = poll_token + 1;
        verifiedBlockThread(5);
        scheduler_state.thread_table.unlock();
        const call_group = try state.beginFdWaitGroup(owner, 5, 17, poll_token + 1);
        state.fd_wait_groups.get(call_group.index).?.pollfds_va = poll_base;
        try std.testing.expect(try state.registerIpcPollWaitersForFd(owner, pair.b, 1, poll_base + 199 * 32, 5, 17, poll_token + 1, call_group));
        try state.armFdWaitGroup(call_group);
        const Fail = struct {
            fn acquire(_: *anyopaque, _: usize) ?@import("memory/owned_storage.zig").MappedExtent {
                return null;
            }
            fn release(_: *anyopaque, _: @import("memory/owned_storage.zig").PhysicalExtent) bool {
                return false;
            }
        };
        var fail_context: u8 = 0;
        const allocator = state.pool_allocator;
        state.pool_allocator = .{ .context = &fail_context, .acquire = Fail.acquire, .release = Fail.release };
        defer state.pool_allocator = allocator;
        IpcCallFrontendFixture.user_accesses = 0;
        try std.testing.expect(ipc.fast(h, state, owner, &call) != null);
        try std.testing.expectEqual(@as(usize, 0), IpcCallFrontendFixture.user_accesses);
        try std.testing.expectEqual(@as(usize, 0), state.fd_wait_groups.live);
        try std.testing.expectEqual(@as(u64, 199), initial_thread_contexts[5].frame.r10);
        try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[5].frame.r8);
        try std.testing.expectEqual(@as(usize, 5), currentThread());
        try std.testing.expectEqual(@as(u8, 1), queue.len);
        try std.testing.expectEqual(@as(usize, 3), body.lifetime_refs.load(.acquire));
        try std.testing.expectEqual(@as(u8, 19), body.caller.recv_fd_capacity);
        if (completion == 1) {
            try std.testing.expect(releaseThreadGeneration(4, initial_thread_contexts[4].generation, owner));
            state.drainIpcCancellations(h.free_list);
            try std.testing.expect(queue.isEmpty());
            continue;
        }
        if (completion == 2) {
            try state.closeFdWithFreeList(owner, pair.b, h.free_list);
            try std.testing.expect(initial_thread_contexts[4].ready);
            try std.testing.expectEqual(@import("syscall/numbers.zig").syscall_err_closed, initial_thread_contexts[4].frame.rax);
            continue;
        }
        var received = std.mem.zeroes(TrapFrame);
        try std.testing.expectEqual(@as(u64, 0), ipc.recvMessage(h, state, owner, pair.b, .{ .active = true, .array = 1 }, 19, 5, 17, 0, &received));
        try std.testing.expectEqualSlices(u64, &.{ 11, 22, 33, 44 }, &abi.inputWords(&received));
        try std.testing.expectEqual(@as(u64, 0), received.r15);
        try std.testing.expect(queue.isEmpty());
        var reply = std.mem.zeroes(TrapFrame);
        reply.rax = abi.syscall_ipc_reply_recv;
        reply.rdi = pair.b;
        reply.rdx = std.math.maxInt(u64);
        reply.rbx = 1;
        reply.r15 = 19 << 32;
        abi.outputWords(&reply, .{ 55, 66, 77, 88 });
        if (completion == 3) {
            const extracted = ipc.extract(state, owner);
            try std.testing.expect(extracted >= 16 and extracted < 1000);
            try std.testing.expect(ipc.reply(h, state, owner, extracted, &reply).committed);
            try state.closeFdWithFreeList(owner, @intCast(extracted), h.free_list);
        } else {
            try std.testing.expect(ipc.fast(h, state, owner, &reply) != null);
            try std.testing.expectEqual(@as(usize, 4), currentThread());
        }
        try std.testing.expect(initial_thread_contexts[4].ready);
        try std.testing.expectEqual(@as(usize, 1), body.lifetime_refs.load(.acquire));
        try std.testing.expectEqual(@as(u64, 0), initial_thread_contexts[4].frame.r15);
        try std.testing.expectEqualSlices(u64, &.{ 55, 66, 77, 88 }, &abi.inputWords(&initial_thread_contexts[4].frame));
    }
}

test "thread storage IPC stable receive lifetime exit exec slot reuse and close" {
    const ipc = @import("syscall/native_ipc.zig").WaitTestSupport;
    const saved = try std.testing.allocator.dupe(ThreadContext, &initial_thread_contexts);
    defer {
        @memcpy(&initial_thread_contexts, saved);
        std.testing.allocator.free(saved);
        initializeStaticStorage();
    }
    const storage = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(storage);
    try std.testing.expect(kernel.initRuntimeStorage(storage));
    for (0..7) |action| {
        const state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        defer state.deinitPools();
        try initializeDirectBlockTest(true);
        WaitAdmissionTest.stop_after_block = true;
        defer WaitAdmissionTest.stop_after_block = false;
        const h = IpcCallFrontendFixture.hooks();
        const owner = default_process_principal;
        const fd = try state.createIpcEndpointFd(owner, .{ .send = true, .recv = true, .wait = true, .close = true }, .{}, 16);
        var frame = std.mem.zeroes(TrapFrame);
        frame.rdi = fd;
        frame.rdx = 20;
        const generation = initial_thread_contexts[4].generation;
        _ = ipc.recv(h, state, owner, &frame);
        const block = ipcBlock(4, generation, owner).?;
        const ref = block.ref;
        const list = block.receive_list.?;
        try std.testing.expectEqual(@as(usize, 2), block.lifetime_refs.load(.acquire));
        if (action == 0) {
            try std.testing.expect(releaseThreadGeneration(4, generation, owner));
            // Context reset has already happened, but the pending node is
            // still the same stable object with an intact FIFO identity.
            try std.testing.expectEqual(block, list.receive_head.?);
            try std.testing.expect(block.receive.state == .pending and block.block_owner.state == .pending);
        } else if (action == 1) {
            detachPrincipalIpcBindings(owner); // exec / forced process teardown
        } else if (action == 2) {
            wakeIfTimerExpired(4, std.math.maxInt(u64));
            try std.testing.expect(block.receive.state == .pending);
            detachPrincipalIpcBindings(owner);
        } else if (action == 4) {
            initial_thread_contexts[4].signal_entry = 0x400000;
            initial_thread_contexts[4].signal_delivery_ready = true;
            scheduler_state.thread_table.lock();
            try std.testing.expect(deliverSignalLocked(state, state.processDescriptor(owner).?.generation, owner, 4, &initial_thread_contexts[4], 2, 3) != null);
            try std.testing.expect(block.receive.state == .pending);
            detachPrincipalIpcBindings(owner);
        } else if (action == 5) {
            _ = stopPrincipalThreads(owner);
            try std.testing.expect(block.receive.state == .bound);
            wakeIfTimerExpired(4, std.math.maxInt(u64));
            detachPrincipalIpcBindings(owner);
        } else if (action == 6) {
            const Race = struct {
                fn claim(target: kernel.ThreadWakeTarget, cpu: usize, result: *bool) void {
                    if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(cpu);
                    result.* = claimWaitTokenCompletion(target.thread_index, target.thread_generation, target.owner, target.wait_token) == .claimed;
                }
            };
            var first = false;
            var second = false;
            const t1 = try std.Thread.spawn(.{}, Race.claim, .{ block.receive_target, 0, &first });
            const t2 = try std.Thread.spawn(.{}, Race.claim, .{ block.receive_target, 1, &second });
            t1.join();
            t2.join();
            try std.testing.expect(first != second);
            try std.testing.expect(block.receive.state == .pending);
            detachPrincipalIpcBindings(owner);
        } else {
            try state.closeFdWithFreeList(owner, fd, h.free_list);
            try std.testing.expect(block.receive_list == null);
            detachPrincipalIpcBindings(owner);
        }
        state.drainIpcCancellations(h.free_list);
        try std.testing.expect(state.ipcReplySlot(ref) == null);
        try std.testing.expectEqual(@as(usize, 0), state.ipc_replies.live);
        state.closeFdWithFreeList(owner, fd, h.free_list) catch {};
        // The slot-table generation invalidates stale queue/block tickets;
        // freed page grants remain reclaimable via the shared B1c cache.
        const fresh = try state.createIpcReply();
        try std.testing.expect(!kernel.KernelState.replyRefEqual(ref, fresh));
        state.clearIpcReplySlot(fresh);
        var reclaimable: @import("memory/reclaim_cache.zig").Registry = .{};
        reclaimable.register(&state.wait_storage_cache);
        try std.testing.expect(reclaimable.retainedPages() != 0);
        try std.testing.expect(reclaimable.reclaim() != 0);
        try std.testing.expectEqual(@as(usize, 0), reclaimable.retainedPages());
    }
}

// Step 3 fixtures exercise the real ownership transition, not a separate
// fairness model. The injected monotonic sample replaces only clock hardware.
const IpcEevdfTest = struct {
    fn competitor() !void {
        initial_thread_contexts[6] = .{ .id = 6, .generation = 18, .allocated = true, .ready = true, .cr3 = 0x3000, .scheduler_entity = &initial_scheduler_entities[6] };
        syncHotStateFromContext(6);
        try std.testing.expect(verifiedAddThread(6, 18, true));
    }
    fn hint(index: usize) kernel.ThreadWakeTarget {
        return .{ .thread_index = @intCast(index), .thread_generation = initial_thread_contexts[index].generation, .owner = initial_thread_contexts[index].owner_process };
    }
    fn ordinaryBlock(frame: *TrapFrame) !void {
        const source = &initial_thread_contexts[currentThread()];
        scheduler_state.thread_table.lock();
        defer scheduler_state.thread_table.unlock();
        _ = saveCurrentThreadContextFromFrameLocked(ContextTransferTestRegisters, frame);
        commitThreadBlockedLocked(source);
        setThreadHotWaitState(source.id, true, 0, false);
        verifiedBlockThread(source.id);
        try std.testing.expect(installNextLocalAfterBlockLocked(ContextTransferTestRegisters, 0, frame, null, null));
    }
    const FairResult = struct { service: [3]i64, direct: usize, mismatch: usize, cpu_preempts: usize, timer_events: usize, max_service_gap_steps: [3]usize = .{ 0, 0, 0 } };
    fn fairness(optimized: bool) !FairResult {
        RuntimeAccountingTest.now_ns = 0;
        try initializeDirectBlockTest(true);
        try competitor();
        var frame = std.mem.zeroes(TrapFrame);
        var result: FairResult = .{ .service = .{ 0, 0, 0 }, .direct = 0, .mismatch = 0, .cpu_preempts = 0, .timer_events = 0 };
        var quantum_remaining_ns: u64 = 4_000_000;
        var last_service_step = [_]usize{ 0, 0, 0 };
        for (0..6000) |step| {
            const source_id = currentThread();
            const service_index = source_id - 4;
            result.max_service_gap_steps[service_index] = @max(result.max_service_gap_steps[service_index], step + 1 - last_service_step[service_index]);
            last_service_step[service_index] = step + 1;
            RuntimeAccountingTest.now_ns.? += 100_000;
            quantum_remaining_ns -= 100_000;
            // The CPU's four-millisecond quantum survives every RPC. Only
            // expiration renews it; the CPU-bound peer never yields early.
            if (quantum_remaining_ns == 0) {
                result.timer_events += 1;
                const decision = verifiedExpireSlice(0, source_id, initial_thread_contexts[source_id].generation, 4_000_000);
                try std.testing.expect(decision != .invalid);
                if (decision == .preempt) {
                    if (source_id == 6) result.cpu_preempts += 1;
                    scheduler_state.thread_table.lock();
                    const installed = installNextLocalAfterBlockLocked(ContextTransferTestRegisters, 0, &frame, null, null);
                    scheduler_state.thread_table.unlock();
                    try std.testing.expect(installed);
                }
                quantum_remaining_ns = 4_000_000;
            } else if (source_id != 6) {
                const peer_id: usize = if (source_id == 4) 5 else 4;
                const peer = &initial_thread_contexts[peer_id];
                scheduler_state.thread_table.lock();
                if (!peer.ready) {
                    peer.wait_mailbox = false;
                    peer.active_wait_token = 0;
                    peer.wait_completion_claimed = false;
                    peer.wait_completion_publish_pending = false;
                    peer.ready = true;
                    syncHotStateFromContext(peer_id);
                    const woke = verifiedWakeThreadGenerationPreferred(peer_id, peer.generation, 0, null);
                    if (!woke) {
                        scheduler_state.thread_table.unlock();
                        return error.TestUnexpectedResult;
                    }
                }
                scheduler_state.thread_table.unlock();
                const token = reserveCurrentWaitToken(initial_thread_contexts[source_id].generation).?;
                const state = &scheduler_state.cpus[0];
                const expected = state.runqueue.bestEligible(state.runqueue.cachedPostBlockTime()).?;
                const expected_id = expected.thread_index;
                if (optimized and tryIpcDirectSwitch(&frame, false, token, 0, hint(peer_id), null)) {
                    result.direct += 1;
                    try std.testing.expectEqual(peer_id, expected_id);
                } else {
                    if (optimized) result.mismatch += 1;
                    try ordinaryBlock(&frame);
                }
                try std.testing.expectEqual(expected_id, currentThread());
            }
        }
        for (0..3) |i| {
            result.service[i] = initial_scheduler_entities[i + 4].entity.service_ns;
            result.max_service_gap_steps[i] = @max(result.max_service_gap_steps[i], 6000 - last_service_step[i]);
        }
        return result;
    }
};

test "thread storage IPC EEVDF preferred winner bypass with competitor and preserved budget" {
    const saved = initial_thread_contexts[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved;
        RuntimeAccountingTest.now_ns = null;
        initializeStaticStorage();
    }
    RuntimeAccountingTest.now_ns = 100;
    try initializeDirectBlockTest(true);
    try IpcEevdfTest.competitor();
    const token = reserveCurrentWaitToken(1).?;
    var frame = std.mem.zeroes(TrapFrame);
    frame.rip = 0x1234;
    frame.rdx = 1;
    const state = &scheduler_state.cpus[0];
    state.tick_accum = 2;
    state.deferred_slice_ticks = 3;
    RuntimeAccountingTest.now_ns = 140;
    try std.testing.expectEqual(@as(usize, 5), state.runqueue.cachedPostBlockSelection().?.thread_index);
    try std.testing.expect(tryIpcDirectSwitch(&frame, false, token, 0, IpcEevdfTest.hint(5), null));
    try std.testing.expectEqual(@as(usize, 5), currentThread());
    try std.testing.expectEqual(@as(i64, 40), initial_scheduler_entities[4].entity.service_ns);
    try std.testing.expectEqual(@as(u64, 2), state.tick_accum);
    try std.testing.expectEqual(@as(u64, 3), state.deferred_slice_ticks);
    try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[4].frame.rdx);
    try std.testing.expectEqual(@as(u64, 0x1234), initial_thread_contexts[4].frame.rip);
    RuntimeAccountingTest.now_ns = 140 + 4_000_000;
    try std.testing.expectEqual(SliceDecision.preempt, verifiedExpireSlice(0, 5, 17, 4_000_000));
    try std.testing.expectEqual(@as(i64, 4_000_000), initial_scheduler_entities[5].entity.service_ns);
}

test "thread storage IPC EEVDF refusal preserves frame token tree and service" {
    const saved = initial_thread_contexts[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved;
        RuntimeAccountingTest.now_ns = null;
        initializeStaticStorage();
    }
    for (0..9) |scenario| {
        RuntimeAccountingTest.now_ns = 100;
        try initializeDirectBlockTest(true);
        try IpcEevdfTest.competitor();
        const token = reserveCurrentWaitToken(1).?;
        var hint = IpcEevdfTest.hint(5);
        var timeout: u64 = 0;
        switch (scenario) {
            0 => hint = IpcEevdfTest.hint(6), // valid target, wrong winner
            1 => timeout = 1,
            2 => {
                initial_thread_contexts[4].signal_entry = 1;
                initial_thread_contexts[4].signal_delivery_ready = true;
                initial_thread_contexts[4].pending_signal_mask = 1;
            },
            3 => initial_thread_contexts[5].stopped = true,
            4 => hint.thread_generation +%= 1,
            5 => initial_thread_contexts[5].cpu_slot = 1,
            6 => initial_thread_contexts[5].allocated = false,
            7 => initial_thread_contexts[4].wait_completion_claimed = true,
            8 => RuntimeAccountingTest.now_ns = null,
            else => unreachable,
        }
        var frame = std.mem.zeroes(TrapFrame);
        frame.rax = 123;
        frame.rdx = 1;
        const before = frame;
        const state = &scheduler_state.cpus[0];
        const selection = state.runqueue.cachedSelection();
        const entity = initial_scheduler_entities[4].entity;
        try std.testing.expect(!tryIpcDirectSwitch(&frame, false, token, timeout, hint, null));
        try std.testing.expectEqualDeep(before, frame);
        try std.testing.expectEqualDeep(entity, initial_scheduler_entities[4].entity);
        try std.testing.expectEqual(token, initial_thread_contexts[4].active_wait_token);
        try std.testing.expectEqual(selection, state.runqueue.cachedSelection());
        try std.testing.expectEqual(@as(usize, 4), currentThread());
    }
}

test "thread storage IPC EEVDF ping pong fairness equals ordinary selection" {
    const saved = initial_thread_contexts[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved;
        RuntimeAccountingTest.now_ns = null;
        initializeStaticStorage();
    }
    const ordinary = try IpcEevdfTest.fairness(false);
    const direct = try IpcEevdfTest.fairness(true);
    try std.testing.expectEqualDeep(ordinary.service, direct.service);
    try std.testing.expectEqualDeep(ordinary.max_service_gap_steps, direct.max_service_gap_steps);
    try std.testing.expect(direct.direct > 0 and direct.mismatch > 0 and direct.cpu_preempts > 0);
    for (direct.service) |service| try std.testing.expect(service > 10_000_000);
    // Each participant receives service within two CPU quanta. Sleeping RPC
    // entities are placed at the normal wake floor, so equal thirds would be
    // an invented policy; the ordinary EEVDF run is the reference allocation.
    for (direct.max_service_gap_steps) |gap| try std.testing.expect(gap <= 80);
    try std.testing.expectEqual(@as(i64, 600_000_000), direct.service[0] + direct.service[1] + direct.service[2]);
    try std.testing.expectEqual(@as(usize, 150), direct.timer_events);
    std.debug.print("B3 four-ms fairness ns caller={} server={} CPU={} direct={} mismatch={} CPU-preempts={} timer-events={} max-gap-steps={any}\n", .{ direct.service[0], direct.service[1], direct.service[2], direct.direct, direct.mismatch, direct.cpu_preempts, direct.timer_events, direct.max_service_gap_steps });
}

test "thread storage IPC scalar raced completion consumes once without switching" {
    const saved = initial_thread_contexts[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved;
        initializeStaticStorage();
    }
    for ([_]u64{ 0, 2 }) |status| {
        try initializeDirectBlockTest(true);
        const ctx = &initial_thread_contexts[4];
        const token = reserveCurrentWaitToken(ctx.generation).?;
        var frame = std.mem.zeroes(TrapFrame);
        frame.rdx = 1;
        try std.testing.expect(!tryIpcCompletedWait(&frame, token));
        try std.testing.expectEqual(WaitTokenClaimResult.claimed, claimWaitTokenCompletion(4, ctx.generation, ctx.owner_process, token));
        try std.testing.expect(stageIpcWords(.{ .owner = ctx.owner_process, .thread_index = 4, .thread_generation = ctx.generation, .wait_token = token }, .{ 11, 22, 33, 44 }));
        try std.testing.expectEqual(ClaimedWaitPublishResult.published, publishClaimedWaitCompletion(4, ctx.generation, ctx.owner_process, token, status));
        // 2a tries direct admission before consuming a raced completion.
        // It must reject without changing the completed source or target.
        const peer = &initial_thread_contexts[5];
        try std.testing.expect(!tryIpcDirectSwitch(&frame, false, token, 0, .{ .owner = peer.owner_process, .thread_index = peer.id, .thread_generation = peer.generation }, null));
        try std.testing.expect(!tryIpcCompletedWait(&frame, token + 1));
        try std.testing.expect(tryIpcCompletedWait(&frame, token));
        try std.testing.expectEqual(status, frame.rax);
        try std.testing.expectEqual(@as(u64, if (status == 0) 11 else 0), frame.r10);
        try std.testing.expectEqual(@as(u64, 1), frame.rdx);
        try std.testing.expectEqual(@as(usize, 4), currentThread());
        try std.testing.expectEqual(.running, initial_scheduler_entities[4].ownership);
        try std.testing.expect(!tryIpcCompletedWait(&frame, token));
    }
}

const TestIpcRegisters = struct {
    send_array: u64 = 0,
    send_count: u64 = 0,
    receive_array: u64 = 0,
    receive_capacity: u64 = 0,
    received_count: u64 = 0, // poison address: no count copyout may touch it
    fn apply(self: @This(), frame: *TrapFrame) void {
        frame.r13 = self.send_array;
        frame.rbx = self.receive_array;
        frame.r15 = self.send_count | (self.receive_capacity << 32);
    }
};

/// Profile-only classification under T; a hint never authorizes delivery.
pub fn ipcReceiverAbsence(hint: ?kernel.ThreadWakeTarget) ipc_metric.Op {
    const target = hint orelse return .no_receiver_unseen;
    scheduler_state.thread_table.lock();
    defer scheduler_state.thread_table.unlock();
    const ctx = matchingThreadLocked(target.thread_index, target.thread_generation, target.owner) orelse return .no_receiver_stale;
    if (!ctx.ready or ctx.stopped) return .no_receiver_waiting;
    const entity = ctx.scheduler_entity orelse return .no_receiver_stale;
    return if (entity.ownership == .running) .no_receiver_running else .no_receiver_runnable;
}

test "IPC host costs warm queued CALL and empty cleanup" {
    const abi = @import("kernel_abi_root").ipc_abi;
    const ipc = @import("syscall/native_ipc.zig");
    const measure = b3.HostCost;
    const saved = try std.testing.allocator.dupe(ThreadContext, &initial_thread_contexts);
    defer {
        @memcpy(&initial_thread_contexts, saved);
        std.testing.allocator.free(saved);
        initializeStaticStorage();
    }
    const storage = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(storage);
    try std.testing.expect(kernel.initRuntimeStorage(storage));
    var samples: [5][64]u64 = undefined;
    for (0..72) |iteration| {
        const state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        defer state.deinitPools();
        try initializeDirectBlockTest(true);
        RuntimeAccountingTest.now_ns = 1000;
        defer RuntimeAccountingTest.now_ns = null;
        scheduler_state.cpus[0].runtime_accounting.start(0);
        WaitAdmissionTest.stop_after_block = true;
        defer WaitAdmissionTest.stop_after_block = false;
        const owner = default_process_principal;
        const h = IpcCallFrontendFixture.hooks();
        const pair = try state.createIpcChannelPairFds(owner, .{ .send = true, .recv = true, .call = true, .wait = true, .poll = true, .close = true }, .{}, 16);
        defer {
            detachPrincipalIpcBindings(owner);
            state.drainIpcCancellations(h.free_list);
            state.closeFdWithFreeList(owner, pair.a, h.free_list) catch {};
            state.closeFdWithFreeList(owner, pair.b, h.free_list) catch {};
        }
        _ = try state.ensureIpcBlock(owner, 4, initial_thread_contexts[4].generation);
        _ = try state.ensureIpcBlock(owner, 5, 17);
        try std.testing.expect(state.noteIpcReceiverForFd(owner, pair.b, 5, 17));
        var call = std.mem.zeroes(TrapFrame);
        call.rax = abi.syscall_ipc_call;
        call.rdi = pair.a;
        call.rdx = std.math.maxInt(u64);
        abi.outputWords(&call, .{ 11, 22, 33, 44 });
        // Same three drains as K-drop cleanup; omit K unlock itself because
        // this host fixture owns no hardware K. No pending work is installed.
        const cleanup_start = measure.stamp();
        ipc.drainCancellations(h, state);
        @import("syscall/capsule.zig").drainDeviceMemoryRetirements(state);
        @import("syscall/ram_cache.zig").drain(state, h.free_list);
        const cleanup = measure.stamp() - cleanup_start;
        measure.last = @splat(0);
        measure.active = true;
        const start = measure.stamp();
        try std.testing.expect(ipc.fastRegistersAdmit(&call));
        const result = ipc.fastDispatchAfterRegisterAdmission(h, state, owner, &call);
        const total = measure.stamp() - start;
        measure.active = false;
        try std.testing.expect(result != null);
        try std.testing.expectEqual(@as(usize, 5), currentThread());
        const lookup = measure.last[101];
        const delivery = measure.last[504];
        const admission = measure.last[505];
        try std.testing.expect(lookup != 0 and delivery != 0 and admission != 0);
        if (iteration >= 8) {
            const i = iteration - 8;
            samples[0][i] = cleanup;
            samples[1][i] = lookup;
            samples[2][i] = delivery;
            samples[3][i] = admission - measure.last[104] - measure.last[208];
            samples[4][i] = total - lookup - delivery - admission;
        }
    }
    for (&samples, 0..) |*values, i| {
        std.mem.sort(u64, values, {}, std.sort.asc(u64));
        std.debug.print("IPC_HOST_COST stage={} n=64 median={} p95={} min={}\n", .{ i, (values[31] + values[32]) / 2, values[60], values[0] });
    }
}

test "thread storage cancellation empty hint publication and last pop interleavings" {
    if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(0);
    try std.testing.expect(!hasPendingIpcCancellations());
    // Empty admission must not acquire T: callers can observe the hint even
    // while T is held. No node is dereferenced until the consuming T section.
    scheduler_state.thread_table.lock();
    const empty = takePendingIpcCancellation();
    scheduler_state.thread_table.unlock();
    try std.testing.expect(empty == null);
    for (0..128) |_| {
        var first: ipc_call_binding.Node = .{ .state = .bound };
        var second: ipc_call_binding.Node = .{ .state = .bound };
        var a: ?*ipc_call_binding.Node = &first;
        var b: ?*ipc_call_binding.Node = &second;
        // Producer after a consumer's empty observation remains discoverable.
        try std.testing.expect(!hasPendingIpcCancellations());
        scheduler_state.thread_table.lock();
        pendIpcBindingLocked(&a);
        scheduler_state.thread_table.unlock();
        try std.testing.expect(hasPendingIpcCancellations());
        try std.testing.expectEqual(&first, takePendingIpcCancellation().?);
        // Last-pop then publish must retain the new root.
        scheduler_state.thread_table.lock();
        pendIpcBindingLocked(&b);
        scheduler_state.thread_table.unlock();
        try std.testing.expect(hasPendingIpcCancellations());
        try std.testing.expectEqual(&second, takePendingIpcCancellation().?);
        try std.testing.expect(!hasPendingIpcCancellations());
        finishIpcCancellation(&first);
        finishIpcCancellation(&second);
        first.state = .bound;
        second.state = .bound;
        a = &first;
        b = &second;
        // Publish before pop: advancing the root must preserve the successor.
        scheduler_state.thread_table.lock();
        pendIpcBindingLocked(&a);
        pendIpcBindingLocked(&b);
        scheduler_state.thread_table.unlock();
        try std.testing.expectEqual(&first, takePendingIpcCancellation().?);
        try std.testing.expect(hasPendingIpcCancellations());
        try std.testing.expectEqual(&second, takePendingIpcCancellation().?);
        finishIpcCancellation(&first);
        finishIpcCancellation(&second);
        try std.testing.expect(!hasPendingIpcCancellations());
    }

    const Producer = struct {
        fn run(nodes: []ipc_call_binding.Node, done: *bool) void {
            // Host threads model distinct CPUs for per-CPU lock diagnostics.
            if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(1);
            for (nodes) |*node| {
                var binding: ?*ipc_call_binding.Node = node;
                scheduler_state.thread_table.lock();
                pendIpcBindingLocked(&binding);
                scheduler_state.thread_table.unlock();
                std.Thread.yield() catch {};
            }
            @atomicStore(bool, done, true, .release);
        }
    };
    const nodes = try std.testing.allocator.alloc(ipc_call_binding.Node, 4096);
    defer std.testing.allocator.free(nodes);
    @memset(nodes, .{ .state = .bound });
    var done = false;
    const producer = try std.Thread.spawn(.{}, Producer.run, .{ nodes, &done });
    defer producer.join();
    var consumed: usize = 0;
    while (true) {
        if (takePendingIpcCancellation()) |node| {
            try std.testing.expectEqual(&nodes[consumed], node);
            consumed += 1;
            finishIpcCancellation(node);
        } else if (@atomicLoad(bool, &done, .acquire) and !hasPendingIpcCancellations()) {
            break;
        } else std.Thread.yield() catch {};
    }
    try std.testing.expectEqual(nodes.len, consumed);
    try std.testing.expect(!hasPendingIpcCancellations());
}

test "IPC endpoint two host threads close call cancellation extract poll notify and direct receive" {
    const native = @import("syscall/native_ipc.zig").WaitTestSupport;
    const fd_abi = @import("kernel_abi_root").fd_abi;
    const saved = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    const memory = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(memory);
    try std.testing.expect(kernel.initRuntimeStorage(memory));
    const Worker = struct {
        state: *kernel.KernelState,
        action: usize,
        a: kernel.Fd,
        b: kernel.Fd,
        group: kernel.FdWaitGroupRef,
        call: kernel.IpcReplyRef,
        generation: u32,
        done: std.atomic.Value(bool) = .init(false),
        extracted: ?kernel.Fd = null,
        fn run(w: *@This()) void {
            if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(1);
            const owner = default_process_principal;
            switch (w.action) {
                0, 8 => w.state.closeFd(owner, w.b) catch unreachable,
                9 => w.state.closeFd(owner, w.a) catch unreachable,
                1 => cancelCurrentWaitToken(w.generation, 71),
                6 => {
                    scheduler_state.thread_table.lock();
                    interruptIpcWaitLocked(&initial_thread_contexts[4], 17);
                    scheduler_state.thread_table.unlock();
                },
                7 => w.state.drainIpcCancellations(&IpcCallFrontendFixture.pages),
                2 => w.extracted = w.state.ipcExtractReply(owner, w.call, 16) catch unreachable,
                3 => std.debug.assert(w.state.registerIpcPollWaitersForFd(owner, w.b, fd_abi.event_readable, 0, 5, initial_thread_contexts[5].generation, 72, w.group) catch unreachable),
                4 => _ = w.state.ipcNotifyPeerForFd(owner, w.a) catch unreachable,
                5 => {
                    const result = native.directSend(IpcCallFrontendFixture.hooks(), w.state, owner, w.a, .{ .words = .{ 11, 22, 33, 44 } }, null).?;
                    std.debug.assert(result.status == 0);
                },
                else => unreachable,
            }
            w.done.store(true, .release);
        }
    };
    for (0..10) |action| {
        if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(0);
        const state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        defer state.deinitPools();
        try initializeDirectBlockTest(true);
        const owner = default_process_principal;
        const pair = try state.createIpcChannelPairFds(owner, .{ .send = true, .recv = true, .call = true, .wait = true, .poll = true, .close = true }, .{}, 16);
        defer {
            detachPrincipalIpcBindings(owner);
            state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
            state.closeFd(owner, pair.a) catch {};
            state.closeFd(owner, pair.b) catch {};
        }
        const channel_ref = state.kernelObjectSlot(state.fdEntryConst(owner, pair.a).?.object).?.payload.channel.channel;
        const channel = state.ipcChannelSlot(channel_ref).?;
        const caller = try state.ensureIpcBlock(owner, 4, initial_thread_contexts[4].generation);
        scheduler_state.thread_table.lock();
        initial_thread_contexts[4].active_wait_token = if (action == 8) 0 else 71;
        initial_thread_contexts[4].wait_mailbox = true;
        initial_thread_contexts[5].active_wait_token = 72;
        initial_thread_contexts[5].wait_mailbox = true;
        initial_thread_contexts[5].ready = false;
        verifiedBlockThread(5);
        scheduler_state.thread_table.unlock();
        var call: kernel.IpcReplyRef = .{};
        if (action <= 2 or action == 6 or action == 7) {
            call = try state.ipcCall(owner, pair.a, .{ .words = .{ 1, 2, 3, 4 } }, .{ .owner = owner, .thread_index = 4, .thread_generation = initial_thread_contexts[4].generation, .wait_token = 71 }, &IpcCallFrontendFixture.pages);
            try std.testing.expect(bindIpcOutgoing(4, initial_thread_contexts[4].generation, owner, 71, &caller.outgoing));
        }
        const group = if (action == 3) try state.beginFdWaitGroup(owner, 5, initial_thread_contexts[5].generation, 72) else kernel.FdWaitGroupRef{};
        defer if (!group.isNull()) state.cancelFdWaitGroup(group, 72);
        if (action == 5) try state.registerIpcDirectReceive(owner, pair.b, .{}, 0, 5, initial_thread_contexts[5].generation, 72);
        if (action == 2 or action == 7) caller.retain(1); // local authority transferred to the extracted FD
        var worker: Worker = .{ .state = state, .action = action, .a = pair.a, .b = pair.b, .group = group, .call = call, .generation = initial_thread_contexts[4].generation };
        if (action == 7) {
            worker.extracted = try state.ipcExtractReply(owner, call, 16);
            cancelCurrentWaitToken(worker.generation, 71);
        }
        const lock = if (action == 2 or action == 7) &caller.exclusion else &channel.exclusion;
        var guard = @import("sync/ipc_endpoint.zig").Pair.acquire(lock, if (action == 8) &caller.exclusion else null);
        const thread = try std.Thread.spawn(.{}, Worker.run, .{&worker});
        if (action == 1 or action == 6) {
            // Terminal recording is T-only: it must finish while E is held.
            while (!worker.done.load(.acquire)) std.atomic.spinLoopHint();
        } else {
            while (lock.waiting.load(.acquire) == 0 and !worker.done.load(.acquire)) std.atomic.spinLoopHint();
            try std.testing.expect(!worker.done.load(.acquire));
        }
        if (action == 8) {
            // Commit the actual fast queued CALL while final peer close waits
            // for the same channel, then let close claim this exact token.
            try std.testing.expect(tryIpcFastEnqueue(owner, &channel.queues[1], .{ .kind = .channel, .index = channel_ref.index, .generation = channel_ref.generation, .side = 1 }, .{ 1, 2, 3, 4 }, .{}, 0) != null);
        }
        if (action == 2) cancelCurrentWaitToken(worker.generation, 71);
        guard.end();
        // These real APIs compete with registration / peer close on another
        // host thread. The untouched FD retains the shared channel body.
        if (action == 3) try state.ipcSend(owner, pair.a, .{ .words = .{ 5, 6, 7, 8 } }, &IpcCallFrontendFixture.pages);
        if (action == 9) _ = try state.ipcNotifyPeerForFd(owner, pair.b);
        thread.join();
        try std.testing.expect(worker.done.load(.acquire));
        switch (action) {
            0, 8 => try std.testing.expect(caller.sent),
            1, 6 => {
                try std.testing.expect(!state.ipcClaimCallReply(call));
                state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
                try std.testing.expect(caller.sent);
            },
            2, 7 => {
                // Extract retained an independent authority through cancellation.
                // The test supplies that authority separately from queued CALL.
                cancelCurrentWaitToken(worker.generation, 71);
                state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
                try state.closeFd(owner, worker.extracted.?);
            },
            3 => {
                try state.armFdWaitGroup(group);
                var ready: [2]kernel.ThreadWakeTarget = undefined;
                try std.testing.expectEqual(@as(usize, 1), try state.wakeIpcPollWaitersForSendFd(owner, pair.a, &ready));
            },
            4 => {
                try state.closeFd(owner, pair.a);
                try std.testing.expectEqual(@import("state/ipc.zig").IpcNotifyConsumeResult.consumed, try state.consumeIpcNotifyForFd(owner, pair.b, false));
                try std.testing.expectEqual(@import("state/ipc.zig").IpcNotifyConsumeResult.closed, try state.consumeIpcNotifyForFd(owner, pair.b, false));
            },
            5 => try std.testing.expect(channel.waiters[1].receive_head == null),
            9 => try std.testing.expect((try state.ipcNotifyPeerForFd(owner, pair.b)).closed),
            else => unreachable,
        }
    }
}

test "IPC endpoint K retains claimed poll group until completion ends" {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    const saved = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(0);
    const memory = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(memory);
    try std.testing.expect(kernel.initRuntimeStorage(memory));
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    try state.initFromDetectedRegionsInPlace(1);
    defer state.deinitPools();
    try initializeDirectBlockTest(true);
    const owner = default_process_principal;
    const pair = try state.createIpcChannelPairFds(owner, .{ .send = true, .recv = true, .call = true, .wait = true, .poll = true, .close = true }, .{}, 16);
    defer {
        detachPrincipalIpcBindings(owner);
        state.drainIpcCancellations(&IpcCallFrontendFixture.pages);
        state.closeFd(owner, pair.a) catch {};
        state.closeFd(owner, pair.b) catch {};
    }
    scheduler_state.thread_table.lock();
    initial_thread_contexts[5].active_wait_token = 72;
    initial_thread_contexts[5].wait_mailbox = true;
    initial_thread_contexts[5].ready = false;
    verifiedBlockThread(5);
    scheduler_state.thread_table.unlock();
    const group = try state.beginFdWaitGroup(owner, 5, initial_thread_contexts[5].generation, 72);
    defer state.cancelFdWaitGroup(group, 72);
    state.fd_wait_groups.get(group.index).?.pollfds_va = 0x1000;
    try std.testing.expect(try state.registerIpcPollWaitersForFd(owner, pair.b, fd_abi.event_readable, 0x1000, 5, initial_thread_contexts[5].generation, 72, group));
    try state.armFdWaitGroup(group);
    try std.testing.expectEqual(currentCpu(), initial_thread_contexts[5].cpu_slot);
    try std.testing.expectEqual(.claimed, claimWaitTokenCompletion(5, initial_thread_contexts[5].generation, owner, 72));
    const k = @import("syscalls.zig").KernelLockTestSupport;
    k.lock();
    const retained = state.fd_wait_groups.get(group.index).?;
    const Worker = struct {
        state: *kernel.KernelState,
        replacement: kernel.FdWaitGroupRef = .{},
        rejected: std.atomic.Value(bool) = .init(false),
        fn run(w: *@This()) void {
            if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(1);
            const lock = @import("syscalls.zig").KernelLockTestSupport;
            std.debug.assert(!lock.tryLock());
            w.rejected.store(true, .release);
            lock.lock();
            defer lock.unlock();
            // Actual cleanupProcess group withdrawal API. The source process
            // of a sender need not be the process being torn down.
            w.state.cancelFdWaitGroupsForOwner(default_process_principal);
            w.replacement = w.state.beginFdWaitGroup(default_process_principal, 6, 1, 73) catch unreachable;
        }
    };
    var worker: Worker = .{ .state = state };
    const thread = try std.Thread.spawn(.{}, Worker.run, .{&worker});
    while (!worker.rejected.load(.acquire)) std.atomic.spinLoopHint();
    try std.testing.expectEqual(retained, state.fd_wait_groups.resolve(.{ .index = group.index, .generation = group.generation }).?);
    // Completion's K ownership ends before cleanup can withdraw the group.
    k.unlock();
    thread.join();
    defer state.cancelFdWaitGroup(worker.replacement, 73);
    const replacement = state.fd_wait_groups.get(worker.replacement.index).?;
    try std.testing.expectEqual(group.index, worker.replacement.index);
    try std.testing.expect(state.fd_wait_groups.resolve(.{ .index = group.index, .generation = group.generation }) == null);

    try std.testing.expect(group.generation != replacement.generation);
}

test "thread storage bootstrap sender resumes after eligibility gap handoff" {
    const saved = initial_thread_contexts[4..6].*;
    defer {
        initial_thread_contexts[4..6].* = saved;
        RuntimeAccountingTest.now_ns = null;
        initializeStaticStorage();
    }
    RuntimeAccountingTest.now_ns = 0;
    try initializeDirectBlockTest(true);
    // The dump's sender was runnable at vruntime 999746778 but eligible at
    // 999747648. Its successful ancillary SEND handed off to the RECV loop.
    // Retaining an old vruntime below eligibility is legal after wake/finish.
    initial_scheduler_entities[4].entity.eligible_time = 870;
    initial_scheduler_entities[4].entity.deadline = 4_000_870;
    try std.testing.expect(verifiedHandoffToThreadOnCpu(false, 0, 5, 17, 4, 1));
    const state = &scheduler_state.cpus[0];
    try std.testing.expect(state.runqueue.bestEligible(state.virtual_time) == null);
    RuntimeAccountingTest.now_ns = 400;
    try std.testing.expectEqual(SliceDecision.continue_running, verifiedExpireSlice(0, 5, 17, 400));
    try std.testing.expectEqual(@as(i64, 400), state.virtual_time);
    try std.testing.expectEqual(@as(usize, 5), state.current_entity.?.thread_index);
    RuntimeAccountingTest.now_ns = 4_000_000;
    // A nonblocking receiver cannot indefinitely consume CPU while its
    // runnable sender needs to publish the final bootstrap FINISH packet.
    try std.testing.expectEqual(SliceDecision.preempt, verifiedExpireSlice(0, 5, 17, 4_000_000));
    try std.testing.expectEqual(@as(usize, 4), verifiedPickThreadForCpu(0).?.thread_index);
}

test "IPC direct handoff preflight misses are read-only and commit preserves bindings" {
    const abi = @import("kernel_abi_root").ipc_abi;
    const caps = @import("state/fd_capability.zig");
    const saved = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        initial_thread_contexts[4..6].* = saved;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    const memory = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(memory);
    try std.testing.expect(kernel.initRuntimeStorage(memory));
    for (0..21) |violation| {
        const state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        defer state.deinitPools();
        try initializeDirectBlockTest(true);
        scheduler_state.cpus[0].current_cr3 = 0x1000;
        const owner = default_process_principal;
        const h = IpcCallFrontendFixture.hooks();
        const fds = try state.createIpcChannelPairFds(owner, .{ .call = true, .recv = true, .wait = true, .close = true }, .{}, 16);
        defer {
            detachPrincipalIpcBindings(owner);
            state.drainIpcCancellations(h.free_list);
            state.closeFd(owner, fds.a) catch {};
            state.closeFd(owner, fds.b) catch {};
        }
        const body = try state.ensureIpcBlock(owner, 4, initial_thread_contexts[4].generation);
        const target = &initial_thread_contexts[5];
        target.cr3 = 0x1000;
        target.active_wait_token = 72;
        target.wait_mailbox = true;
        target.ready = false;
        target.frame.cs = 0x23;
        target.frame.ss = 0x1b;
        target.frame.rflags = 0x202;
        target.frame.rsp = 0x9000;
        verifiedBlockThread(5);
        syncHotStateFromContext(5);
        try state.registerIpcDirectReceive(owner, fds.b, .{}, 0, 5, target.generation, 72);
        const receive = ipcBlock(5, target.generation, owner).?;
        const resolved = caps.directIpcUnderK(state, owner, fds.a, true).?;
        var frame = std.mem.zeroes(TrapFrame);
        frame.rax = abi.syscall_ipc_call;
        frame.rdi = fds.a;
        frame.rdx = std.math.maxInt(u64);
        frame.rip = 0x8000;
        frame.rsp = 0xa000;
        frame.cs = 0x23;
        frame.ss = 0x1b;
        frame.rflags = 0x202;
        abi.outputWords(&frame, .{ 11, 22, 33, 44 });
        RuntimeAccountingTest.now_ns = 1000;
        defer RuntimeAccountingTest.now_ns = null;
        scheduler_state.cpus[0].runtime_accounting.start(0);
        var received_fds: [4]u64 = @splat(0);
        IpcCallFrontendFixture.user_accesses = 0;
        switch (violation) {
            15 => {
                frame.r15 = 19 << 32;
                frame.rbx = 1;
            },
            16 => {
                frame.r15 = 20 << 32;
                frame.rbx = 1;
            },
            17 => {
                frame.r15 = 1 << 32;
                frame.rbx = 0;
            },
            18 => {
                frame.r15 = 1 << 32;
                frame.rbx = std.math.maxInt(u64);
            },
            19, 20 => {
                frame.r15 = 1 << 32;
                frame.rbx = @intFromPtr(&received_fds);
            },
            1 => target.pending_signal_mask = 1,
            2 => target.cpu_slot = 1,
            3 => target.active_wait_token += 1,
            4 => target.frame.rip = 0x800000000000,
            5 => target.frame.rflags |= 1 << 8,
            6 => target.ipc_receiving = null,
            7 => target.cr3 = 0,
            9, 10, 11 => target.cr3 = 0x2000,
            12 => initial_thread_contexts[4].wake_tick = 123,
            13 => initial_thread_contexts[4].retry_queue = 0,
            14 => target.deadline_node.linked = true,
            8 => RuntimeAccountingTest.now_ns = null,
            else => {},
        }
        const before_source = initial_thread_contexts[4];
        const before_target = target.*;
        const before_frame = frame;
        const before_time = scheduler_state.cpus[0].virtual_time;
        var pair = @import("sync/ipc_endpoint.zig").Pair.acquire(resolved.list.exclusion.?, &body.exclusion);
        defer pair.end();
        var table: UserAddressSpaceTable = undefined;
        const space_count = kernel.processIndexFromPrincipal(owner).? + 1;
        const spaces = try std.testing.allocator.alloc(UserAddressSpace, space_count);
        defer std.testing.allocator.free(spaces);
        for (spaces) |*space| {
            space.* = .{};
            address_space.initializeUserAddressSpaceStorage(space);
            space.cr3 = if (violation == 11) 0x3000 else target.cr3;
            space.retiring = violation == 10;
        }
        table.initializeFreshStorage(spaces);
        var plan: DirectHandoff = undefined;
        const accepted = prepareDirectHandoff(owner, &frame, resolved, body, true, &table, &plan);
        if (violation != 0 and violation != 9 and violation != 15 and violation != 19 and violation != 20) {
            pair.end();
            try std.testing.expect(!accepted);
            try std.testing.expect(table.fastPinsEnded(space_count - 1));
            try std.testing.expectEqualDeep(before_source, initial_thread_contexts[4]);
            try std.testing.expectEqualDeep(before_target, target.*);
            try std.testing.expectEqualDeep(before_frame, frame);
            try std.testing.expectEqual(before_time, scheduler_state.cpus[0].virtual_time);
            try std.testing.expectEqual(receive, resolved.list.receive_head.?);
            // Restore only fixture corruption before normal teardown.
            if (violation == 6) target.ipc_receiving = &receive.receive;
            if (violation == 12) initial_thread_contexts[4].wake_tick = 0;
            if (violation == 13) initial_thread_contexts[4].retry_queue = null;
            if (violation == 14) target.deadline_node.linked = false;
            continue;
        }
        try std.testing.expect(accepted);
        commitDirectHandoff(&plan, &frame);
        pair.end();
        try std.testing.expect(table.fastPinsEnded(space_count - 1));
        spaces[space_count - 1].cr3 = initial_thread_contexts[4].cr3;
        state.releaseIpcCallReference(receive.ref);
        try std.testing.expectEqual(@as(usize, 5), currentThread());
        try std.testing.expectEqualSlices(u64, &.{ 11, 22, 33, 44 }, &abi.inputWords(&frame));
        try std.testing.expectEqual(@as(usize, 3), body.lifetime_refs.load(.acquire));
        try std.testing.expect(!hasPendingIpcCancellations());
        try std.testing.expectEqual(@as(usize, 0), IpcCallFrontendFixture.user_accesses);
        try std.testing.expectEqual(before_frame.rbx, body.caller.recv_memory.array);
        try std.testing.expectEqual(@as(u8, @intCast(before_frame.r15 >> 32)), body.caller.recv_fd_capacity);
        if (violation == 19 or violation == 20) {
            const attachment = try state.createIpcEndpointFd(owner, .{ .send = true, .recv = true, .transfer = true, .close = true }, .{}, 16);
            defer state.closeFdWithFreeList(owner, attachment, &IpcCallFrontendFixture.pages) catch {};
            const rights: kernel.FdRights = .{ .send = true, .recv = true, .close = true };
            var descriptor: [4]u64 = .{ attachment, @bitCast(rights), 0, 0 };
            var reply = std.mem.zeroes(TrapFrame);
            reply.r13 = @intFromPtr(&descriptor);
            reply.r15 = 1;
            abi.outputWords(&reply, .{ 55, 66, 77, 88 });
            IpcCallFrontendFixture.reject_write = if (violation == 20) @intFromPtr(&received_fds) else 0;
            defer IpcCallFrontendFixture.reject_write = 0;
            const result = @import("syscall/native_ipc.zig").WaitTestSupport.reply(h, state, owner, abi.implicit_reply, &reply);
            try std.testing.expect(result.committed);
            try std.testing.expectEqual(@as(u64, if (violation == 20) @import("syscall/numbers.zig").syscall_err_invalid else 0), result.status);
            try std.testing.expect(initial_thread_contexts[4].ready);
            try std.testing.expect(IpcCallFrontendFixture.user_accesses != 0);
            if (violation == 19) {
                try std.testing.expectEqual(@as(u64, 1), initial_thread_contexts[4].frame.r15);
                try std.testing.expect(state.fdEntryConst(owner, @intCast(received_fds[0])) != null);
                try state.closeFdWithFreeList(owner, @intCast(received_fds[0]), &IpcCallFrontendFixture.pages);
            }
            continue;
        }
        frame.rax = abi.syscall_ipc_reply_recv;
        frame.rdi = fds.b;
        if (violation == 15) {
            frame.rbx = 2;
            frame.r15 = 19 << 32;
        }
        frame.rdx = std.math.maxInt(u64);
        abi.outputWords(&frame, .{ 55, 66, 77, 88 });
        RuntimeAccountingTest.now_ns = 2500;
        const recv = caps.directIpcUnderK(state, owner, fds.b, false).?;
        pair = @import("sync/ipc_endpoint.zig").Pair.acquire(recv.list.exclusion.?, &body.exclusion);
        try std.testing.expect(prepareDirectHandoff(owner, &frame, recv, body, false, &table, &plan));
        commitDirectHandoff(&plan, &frame);
        pair.end();
        state.releaseIpcCallReference(body.ref);
        state.releaseIpcCallReference(body.ref);
        try std.testing.expectEqual(@as(usize, 4), currentThread());
        try std.testing.expectEqualSlices(u64, &.{ 55, 66, 77, 88 }, &abi.inputWords(&frame));
        try std.testing.expectEqual(@as(usize, 1), body.lifetime_refs.load(.acquire));
        try std.testing.expectEqual(receive, recv.list.receive_head.?);
        if (violation == 15) {
            try std.testing.expectEqual(@as(u64, 2), receive.receive_target.recv_memory.array);
            try std.testing.expectEqual(@as(u8, 19), receive.receive_target.recv_fd_capacity);
            try std.testing.expectEqual(@as(usize, 0), IpcCallFrontendFixture.user_accesses);
        }
        try std.testing.expect(!hasPendingIpcCancellations());
    }
}

test "IPC direct SYSRET rejects dangerous flags addresses and selector pairs" {
    var frame = std.mem.zeroes(TrapFrame);
    frame.rip = 0x7fffffffffff;
    frame.rsp = 0x7fffffffffff;
    frame.rflags = 0x202;
    frame.cs = 0x23;
    frame.ss = 0x1b;
    try std.testing.expect(directSysretFrame(&frame));
    for ([_]u6{ 8, 12, 13, 14, 16, 17, 63 }) |bit| {
        frame.rflags = 0x202 | (@as(u64, 1) << bit);
        try std.testing.expect(!directSysretFrame(&frame));
    }
    frame.rflags = 0x202;
    frame.cs = 0x43;
    frame.ss = 0x3b;
    try std.testing.expect(!directSysretFrame(&frame));
    frame.ss = 0x1b;
    try std.testing.expect(!directSysretFrame(&frame));
    frame.cs = 0x23;
    frame.rsp += 1;
    try std.testing.expect(!directSysretFrame(&frame));
}

test "IPC direct negative hint is bounded and contains no authority" {
    const saved = initial_thread_contexts[4];
    defer {
        initial_thread_contexts[4] = saved;
        initializeStaticStorage();
    }
    try initializeDirectBlockTest(true);
    const key = (@as(u64, 74) << 32) | 7;
    try std.testing.expect(directIpcProbeDue(key));
    noteDirectIpcMiss(key);
    try std.testing.expect(directIpcProbeDue((@as(u64, 76) << 32) | 7));
    try std.testing.expect(directIpcProbeDue((@as(u64, 74) << 32) | 8));
    // Reused FD numbers may still see a negative hint, but it only selects
    // ordinary capability lookup. No object/authority is cached here.
    for (0..31) |_| try std.testing.expect(!directIpcProbeDue(key));
    try std.testing.expect(directIpcProbeDue(key));
    try std.testing.expect(directIpcProbeDue(key));
    try std.testing.expectEqual(@as(u8, 0), initial_thread_contexts[4].entry_state.direct_miss_remaining);
}

test "IPC notify direct admission is read-only and commits latch token and handoff once" {
    const abi = @import("kernel_abi_root").ipc_abi;
    const caps = @import("state/fd_capability.zig");
    const saved = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        initial_thread_contexts[4..6].* = saved;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    const memory = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(memory);
    try std.testing.expect(kernel.initRuntimeStorage(memory));
    for (0..29) |scenario| {
        const state = try std.testing.allocator.create(kernel.KernelState);
        defer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        defer state.deinitPools();
        try initializeDirectBlockTest(true);
        scheduler_state.cpus[0].current_cr3 = 0x1000;
        const owner = default_process_principal;
        const fds = try state.createIpcChannelPairFds(owner, .{ .send = true, .recv = true, .wait = true, .close = true }, .{}, 16);
        defer {
            state.closeFd(owner, fds.a) catch {};
            state.closeFd(owner, fds.b) catch {};
        }
        const target = &initial_thread_contexts[5];
        target.cr3 = 0x1000;
        target.active_wait_token = 72;
        target.wait_mailbox = true;
        target.ready = false;
        target.frame.cs = 0x23;
        target.frame.ss = 0x1b;
        target.frame.rflags = 0x202;
        target.frame.rsp = 0x9000;
        verifiedBlockThread(5);
        syncHotStateFromContext(5);
        try state.registerIpcNotifyWaiterForFd(owner, fds.b, 5, target.generation, 72);
        const found = caps.notifyUnderK(state, owner, fds.a, .{ .send = true }).?;
        var pair = @import("sync/ipc_endpoint.zig").Pair.acquire(&found.channel.exclusion, null);
        defer pair.end();
        const wait = scenario == 1 or scenario == 2 or scenario == 4 or scenario == 5;
        const send = scenario != 4 and scenario != 5;
        var frame = std.mem.zeroes(TrapFrame);
        frame.rax = if (wait) abi.syscall_ipc_notify_wait else abi.syscall_ipc_notify;
        frame.rdi = fds.a;
        frame.rsi = std.math.maxInt(u64);
        frame.rdx = if (send and wait) abi.notify_wait_flag_notify_peer else 0;
        frame.rip = 0x8000;
        frame.rsp = 0xa000;
        frame.cs = 0x23;
        frame.ss = 0x1b;
        frame.rflags = 0x202;
        RuntimeAccountingTest.now_ns = 1000;
        defer RuntimeAccountingTest.now_ns = null;
        scheduler_state.cpus[0].runtime_accounting.start(0);
        if (scenario == 14 or scenario == 15) {
            scheduler_state.cpu_count = 2;
            scheduler_state.cpus[1].enabled = true;
            scheduler_state.cpus[1].is_idle = true;
            target.cpu_slot = 1;
            target.scheduler_entity.?.cpu_slot = 1;
            target.cpu_affinity = testAffinity(3);
            if (scenario == 15) {
                scheduler_state.cpus[1].is_idle = false;
                setTestCurrentThread(&scheduler_state.cpus[1], 5);
            }
        }
        if ((scenario >= 19 and scenario <= 22) or scenario == 24) {
            found.channel.notify_waiters[1 - found.side] = .{};
            found.channel.receiver_hints[1 - found.side] = .{ .active = true, .owner = owner, .thread_index = 5, .thread_generation = target.generation };
            target.ready = true;
            target.wait_mailbox = false;
            target.active_wait_token = 0;
            target.entry_state.notify_sysret_continuation = scenario != 20;
            scheduler_state.thread_table.lock();
            try std.testing.expect(verifiedWakeThreadGenerationPreferred(5, target.generation, 0, null));
            scheduler_state.thread_table.unlock();
            if (scenario == 21) target.pending_signal_mask = 1;
            if (scenario == 24) target.frame.rax = @import("syscall/numbers.zig").syscall_err_closed;
            if (scenario == 22) {
                scheduler_state.cpus[0].runqueue_lock.lock();
                try std.testing.expect(scheduler_state.cpus[0].runqueue.remove(target.scheduler_entity.?));
                scheduler_state.cpus[0].runqueue_lock.unlock();
            }
        }
        var poll_observer: kernel.IpcWaiter = .{ .active = true };
        var persistent_set: kernel.persistent_wait.Set = .{};
        var persistent_observer: kernel.persistent_wait.Item = .{ .set = &persistent_set };
        defer found.channel.waiters[found.side].poll_head = null;
        switch (scenario) {
            2, 4 => found.channel.notify_pending[found.side] = true,
            3 => found.channel.notify_waiters[1 - found.side] = .{},
            6 => target.pending_signal_mask = 1,
            7 => target.active_wait_token += 1,
            8 => target.frame.rip = 0x800000000000,
            9 => target.frame.rflags |= 1 << 8,
            10 => target.deadline_node.linked = true,
            11 => initial_thread_contexts[4].pending_signal_mask = 1,
            12 => RuntimeAccountingTest.now_ns = null,
            13 => target.stopped = true,
            16 => {
                found.channel.waiters[found.side].poll_head = &poll_observer;
            },
            25...28 => {
                poll_observer.binding.persistent = &persistent_observer;
                poll_observer.events = if (scenario >= 26) @import("kernel_abi_root").fd_abi.event_readable else 0;
                persistent_observer.queued = scenario >= 27;
                if (scenario == 28) persistent_set.invocation = .{ .token = 1 };
                found.channel.waiters[found.side].poll_head = &poll_observer;
            },
            17 => found.channel.ref_count = 1,
            23 => found.channel.ref_count = 3,
            18 => {
                found.channel.notify_waiters[1 - found.side] = .{};
                found.channel.receiver_hints[1 - found.side] = .{ .active = true, .owner = owner, .thread_index = 5, .thread_generation = target.generation };
            },
            else => {},
        }
        const before_source_node = initial_thread_contexts[4].scheduler_entity.?.*;
        const before_target_node = target.scheduler_entity.?.*;
        const before_tree = scheduler_state.cpus[0].runqueue;
        const before_source = initial_thread_contexts[4];
        const before_target = target.*;
        const before_frame = frame;
        const before_pending = found.channel.notify_pending;
        const before_waiters = found.channel.notify_waiters;
        var plan: DirectNotifyPlan = undefined;
        const accepted = prepareDirectNotify(owner, &frame, found.channel, found.side, wait, send, null, &plan);
        if (scenario >= 6 and scenario != 14 and scenario != 15 and scenario != 19 and scenario != 23 and scenario != 24 and scenario != 25 and scenario != 27) {
            try std.testing.expect(!accepted);
            // Tree links form cycles; compare fields and pointer identities,
            // then the pointed-to nodes and tree separately, never recurse.
            try std.testing.expect(std.meta.eql(before_source, initial_thread_contexts[4]));
            try std.testing.expect(std.meta.eql(before_target, target.*));
            try std.testing.expect(std.meta.eql(before_source_node, initial_thread_contexts[4].scheduler_entity.?.*));
            try std.testing.expect(std.meta.eql(before_target_node, target.scheduler_entity.?.*));
            try std.testing.expect(std.meta.eql(before_tree, scheduler_state.cpus[0].runqueue));
            try std.testing.expectEqualDeep(before_frame, frame);
            try std.testing.expectEqualDeep(before_pending, found.channel.notify_pending);
            try std.testing.expectEqualDeep(before_waiters, found.channel.notify_waiters);
            target.deadline_node.linked = false;
            found.channel.waiters[found.side].poll_head = null;
            found.channel.ref_count = 2;
            continue;
        }
        try std.testing.expect(accepted);
        const result = commitDirectNotify(&plan, &frame);
        try std.testing.expectEqual(@as(?usize, if (scenario == 15) 1 else null), result.remote_cpu);
        if (scenario == 14 or scenario == 15) {
            const destination: usize = if (scenario == 14) 0 else 1;
            try std.testing.expectEqual(@as(usize, 4), currentThread());
            try std.testing.expectEqual(destination, target.cpu_slot);
            try std.testing.expectEqual(@as(u64, 0), target.active_wait_token);
            try std.testing.expectEqual(@as(usize, 1), scheduler_state.cpus[destination].runqueue.count);
            try std.testing.expectEqual(target.scheduler_entity, scheduler_state.cpus[destination].runqueue.cachedSelection());
            pair.end();
            continue;
        }
        if (scenario <= 2 or scenario == 19 or scenario == 23 or scenario == 24 or scenario == 25 or scenario == 27) {
            try std.testing.expectEqual(@as(usize, 5), currentThread());
            try std.testing.expectEqual(@as(u64, if (scenario == 19 or scenario == 24) before_target.frame.rax else 0), frame.rax);
            if (scenario == 19 or scenario == 24) try std.testing.expectEqualDeep(before_target.frame, frame);
            try std.testing.expectEqual(scenario == 19 or scenario == 24, found.channel.notify_pending[1 - found.side]);
            try std.testing.expect(!found.channel.notify_waiters[1 - found.side].active);
            try std.testing.expectEqual(@as(u64, 0), target.active_wait_token);
            try std.testing.expectEqual(scenario != 1, initial_thread_contexts[4].ready);
            try std.testing.expectEqual(scenario == 1, found.channel.notify_waiters[found.side].active);
            try std.testing.expectEqual(@as(u64, 0), result.block_token);
        } else {
            try std.testing.expectEqual(@as(usize, if (scenario == 5) scheduler_state.cpus[0].idle_thread else 4), currentThread());
            try std.testing.expectEqual(scenario == 3, found.channel.notify_pending[1 - found.side]);
            try std.testing.expectEqual(@as(u64, 0), result.block_token);
            try std.testing.expectEqual(scenario == 5, result.parked);
            if (scenario == 5) {
                try std.testing.expect(!initial_thread_contexts[4].ready);
                try std.testing.expect(found.channel.notify_waiters[found.side].active);
                try std.testing.expectEqual(initial_thread_contexts[4].active_wait_token, found.channel.notify_waiters[found.side].wait_token);
                try std.testing.expectEqual(scheduler_runqueue.Ownership.blocked, initial_thread_contexts[4].scheduler_entity.?.ownership);
                try std.testing.expectEqual(@as(u64, 0x8000), initial_thread_contexts[4].frame.rip);
            }
            try std.testing.expect(!found.channel.notify_pending[found.side]);
        }
        // Detach fixture registrations before channel teardown; assertions
        // above already checked the exact committed single-winner state.
        found.channel.notify_waiters = .{ .{}, .{} };
        found.channel.ref_count = 2;
        pair.end();
    }
}

test "IPC notify saved continuation origin ends at activation and ordinary capture" {
    const saved_contexts = initial_thread_contexts[4..7].*;
    const saved_hot = initial_thread_hot_states[4..7].*;
    defer {
        initial_thread_contexts[4..7].* = saved_contexts;
        initial_thread_hot_states[4..7].* = saved_hot;
        initializeStaticStorage();
    }
    try initializeFusedWakeTest(0);
    var frame = std.mem.zeroes(TrapFrame);
    frame.rip = 0x8000;
    frame.rsp = 0x9000;
    frame.cs = 0x23;
    frame.ss = 0x1b;
    frame.rflags = 0x202;
    frame.rax = @import("kernel_abi_root").ipc_abi.syscall_ipc_notify;
    frame.rbx = 0x1234;
    frame.r13 = 0x5678;
    const source = &initial_thread_contexts[4];
    source.entry_state.notify_sysret_continuation = true;
    scheduler_state.thread_table.lock();
    _ = saveCurrentThreadContextFromFrameLocked(ContextTransferTestRegisters, &frame);
    scheduler_state.thread_table.unlock();
    try std.testing.expect(!source.entry_state.notify_sysret_continuation);
    initial_thread_contexts[5].entry_state.notify_sysret_continuation = true;
    const Installer = struct {
        fn install(t: usize, g: u32, f: *TrapFrame, result: ?u64) bool {
            return switchToNotifyGenerationLockedWithRegisters(ContextTransferTestRegisters, t, g, f, result);
        }
    };
    try std.testing.expectEqual(IpcNotifyWakeResult.handed_off, completeIpcNotifyWaiterAndHandoffWithInstall(Installer.install, &frame, 5, 17, default_process_principal, 11, 0, 0));
    try std.testing.expect(source.entry_state.notify_sysret_continuation);
    try std.testing.expectEqual(@as(u64, 0x1234), source.frame.rbx);
    try std.testing.expectEqual(@as(u64, 0x5678), source.frame.r13);
    try std.testing.expect(!initial_thread_contexts[5].entry_state.notify_sysret_continuation);
    try std.testing.expect(source.entry_state.notify_sysret_continuation);
}

test "native signal scratch is independent after clone and pending survives until registration" {
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    try initializeDirectBlockTest(false);
    const owner = initial_thread_contexts[4].owner_process;
    try std.testing.expect(configureCurrentSignalDelivery(0x4000, 0x4000, 0x5000, 0, 0, 0x10000, 8192, true));
    try std.testing.expect(copyCurrentThreadStateToGeneration(5, 17, owner));
    const child = &initial_thread_contexts[5];
    try std.testing.expectEqual(@as(u64, 0), child.signal_scratch_base);
    try std.testing.expectEqual(@as(u64, 0), child.signal_scratch_size);
    try std.testing.expect(!child.signal_delivery_ready and !child.signal_owner);
    setTestCurrentThread(&scheduler_state.cpus[0], 5);
    child.pending_signal_mask = 1;
    try std.testing.expect(currentSignalStack() == null);
    try std.testing.expect(claimCurrentSignalForUserReturn(0x9000) == null);
    try std.testing.expectEqual(@as(u64, 1), child.pending_signal_mask);
    try std.testing.expect(configureCurrentSignalDelivery(0x4000, 0x4000, 0x5000, 0, 0, 0x20000, 8192, false));
    try std.testing.expect(claimCurrentSignalForUserReturn(0x4001) == null);
    const claimed = claimCurrentSignalForUserReturn(0x9000).?;
    try std.testing.expectEqual(@as(u64, 0x20000), claimed.scratch_base);
    try std.testing.expectEqual(@as(u64, 0x10000), initial_thread_contexts[4].signal_scratch_base);
    try std.testing.expectEqual(@as(u64, 0), child.pending_signal_mask);
    // Delivery copy failure restores the claimed signal through this API.
    child.owner_process = @enumFromInt(std.math.maxInt(u32));
    restoreClaimedSignal(claimed);
    try std.testing.expectEqual(@as(u64, 1), child.pending_signal_mask);
    _ = claimCurrentSignalForUserReturn(0x9000).?;
    try std.testing.expectEqual(@as(u64, 0), child.pending_signal_mask);
    _ = @import("signal_stack.zig").layout(0x20000, 8192, 1).?;
}

test "native signal unregistered child can block with pending and cannot be interrupted before registration" {
    const saved_contexts = initial_thread_contexts[4..6].*;
    const saved_hot = initial_thread_hot_states[4..6].*;
    defer {
        WaitAdmissionTest.stop_after_block = false;
        initial_thread_contexts[4..6].* = saved_contexts;
        initial_thread_hot_states[4..6].* = saved_hot;
        initializeStaticStorage();
    }
    for ([_]bool{ false, true }) |registered| {
        try initializeDirectBlockTest(false);
        WaitAdmissionTest.stop_after_block = true;
        const ctx = &initial_thread_contexts[4];
        const owner: kernel.PrincipalId = @enumFromInt(std.math.maxInt(u32));
        ctx.owner_process = owner;
        ctx.signal_entry = 0x4000;
        ctx.signal_delivery_ready = registered;
        ctx.pending_signal_mask = 1;
        const token = reserveCurrentWaitToken(1).?;
        var frame = std.mem.zeroes(TrapFrame);
        try std.testing.expectEqual(!registered, blockCurrentThread(&frame, true, false, token, 0, 2, null));
        try std.testing.expectEqual(@as(u64, 1), ctx.pending_signal_mask);
        if (!registered) {
            const state = try std.testing.allocator.create(kernel.KernelState);
            defer std.testing.allocator.destroy(state);
            scheduler_state.thread_table.lock();
            const delivery = deliverSignalLocked(state, null, owner, 4, ctx, 2, 2).?;
            try std.testing.expect(!delivery.was_blocked and !delivery.should_interrupt);
            try std.testing.expect(!ctx.ready and ctx.wait_mailbox);
            try std.testing.expectEqual(@as(u64, 3), ctx.pending_signal_mask);
        }
    }
}
