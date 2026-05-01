const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const interrupts = @import("interrupts.zig");
const x86_platform = @import("arch/x86_64/platform.zig");

const TrapFrame = interrupts.TrapFrame;
const UserAddressSpace = capability.UserAddressSpace;
const default_process_principal: kernel.PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;

const fx_state_bytes: usize = 512;
pub const max_thread_slots: usize = kernel.max_thread_slots;
pub const max_ipc_queue_depth: usize = 8;

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

pub const ThreadContext = struct {
    id: u32 = 0,
    allocated: bool = false,
    owner_process: kernel.PrincipalId = default_process_principal,
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
}

pub fn setIpcHotReady(thread_index: usize, ready: bool) void {
    if (getIpcHotThread(thread_index)) |hot| hot.ready = boolByte(ready);
}

pub fn setIpcHotSignalPending(thread_index: usize, pending: bool) void {
    if (getIpcHotThread(thread_index)) |hot| hot.signal_pending = boolByte(pending);
}

pub fn setIpcHotWaitState(thread_index: usize, wait_mailbox: bool, wake_tick: u64, ready: bool) void {
    if (getIpcHotThread(thread_index)) |hot| {
        hot.wait_mailbox = boolByte(wait_mailbox);
        hot.wake_tick = wake_tick;
        hot.ready = boolByte(ready);
    }
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
    const hot = getIpcHotThreadConst(thread_index) orelse return false;
    if (hot.allocated == 0) return false;
    if (hot.ready == 0) return false;
    current_thread_index = thread_index;
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
    if (runtime_priority_active) {
        if (runtime_priority_principal) |priority_principal| {
            if (threadSlotForPrincipal(priority_principal)) |priority_slot| {
                const priority_hot = getIpcHotThreadConst(priority_slot) orelse return current_index;
                if (current_index != priority_slot and priority_hot.allocated != 0 and priority_hot.ready != 0) return priority_slot;
            }
        }
    }
    var step: usize = 1;
    while (step <= max_thread_slots) : (step += 1) {
        const idx = (current_index + step) % max_thread_slots;
        const hot = getIpcHotThreadConst(idx) orelse continue;
        if (hot.allocated == 0) continue;
        if (hot.ready != 0) return idx;
    }
    return current_index;
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
