const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const interrupts = @import("interrupts.zig");

const TrapFrame = interrupts.TrapFrame;
const UserAddressSpace = capability.UserAddressSpace;
const default_process_principal: kernel.PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;

const fx_state_bytes: usize = 512;
pub const max_thread_slots: usize = kernel.max_thread_slots;

pub const ThreadContext = struct {
    id: u32 = 0,
    allocated: bool = false,
    owner_process: kernel.PrincipalId = default_process_principal,
    cr3: u64 = 0,
    ready: bool = false,
    wait_mailbox: bool = false,
    signal_pending: bool = false,
    wake_tick: u64 = 0,
    frame: TrapFrame = std.mem.zeroes(TrapFrame),
    fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes,
};

pub fn buildInitialThreadContexts() [max_thread_slots]ThreadContext {
    var contexts: [max_thread_slots]ThreadContext = undefined;
    inline for (0..max_thread_slots) |i| {
        contexts[i] = .{ .id = @intCast(i) };
    }
    return contexts;
}

pub var thread_contexts: [max_thread_slots]ThreadContext = buildInitialThreadContexts();
pub var process_thread_slots: [kernel.process_count]?usize = [_]?usize{null} ** kernel.process_count;
pub export var user_cr3_value: u64 = 0;
pub var current_user_principal: kernel.PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
pub var current_thread_index: usize = 0;
pub var lapic_tick_count: u64 = 0;
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

pub fn chooseNextThreadForTimerPreempt(quantum_ticks: u64, compositor_hold_quanta: u64) ?usize {
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
                const priority_ctx = getThreadContextConst(priority_slot) orelse null;
                if (priority_ctx != null and priority_ctx.?.allocated and priority_ctx.?.ready and runtime_priority_streak + 1 < compositor_hold_quanta) {
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

pub fn getThreadContext(thread_index: usize) ?*ThreadContext {
    if (thread_index >= max_thread_slots) return null;
    return &thread_contexts[thread_index];
}

pub fn getThreadContextConst(thread_index: usize) ?*const ThreadContext {
    if (thread_index >= max_thread_slots) return null;
    return &thread_contexts[thread_index];
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
    ctx.cr3 = space.cr3;
    ctx.ready = true;
    ctx.wait_mailbox = false;
    ctx.signal_pending = false;
    ctx.wake_tick = 0;
    ctx.frame = initial_frame;
    ctx.fx_state = initial_fx_state;
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
    return true;
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
    if ((ctx.cr3 & 0xFFF) != 0) return true;
    if (ctx.cr3 == 0) return true;
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
        if (!was_ready) ctx.ready = false;
    }
}

pub fn activateThread(thread_index: usize) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    if (!ctx.ready) return false;
    current_thread_index = thread_index;
    current_user_principal = ctx.owner_process;
    user_cr3_value = ctx.cr3;
    return true;
}

pub fn saveCurrentThreadContextFromFrame(frame: *const TrapFrame) void {
    const ctx = getThreadContext(current_thread_index) orelse return;
    if (!ctx.allocated) return;
    ctx.frame = frame.*;
    ctx.cr3 = user_cr3_value;
    if (!ctx.wait_mailbox and ctx.wake_tick == 0) {
        ctx.ready = true;
    }
}

pub fn loadThreadContextToFrame(thread_index: usize, frame: *TrapFrame) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.allocated) return false;
    if (!ctx.ready) return false;
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
                const priority_ctx = getThreadContextConst(priority_slot) orelse return current_index;
                if (current_index != priority_slot and priority_ctx.allocated and priority_ctx.ready) return priority_slot;
            }
        }
    }
    var step: usize = 1;
    while (step <= max_thread_slots) : (step += 1) {
        const idx = (current_index + step) % max_thread_slots;
        const ctx = getThreadContextConst(idx) orelse continue;
        if (!ctx.allocated) continue;
        if (ctx.ready) return idx;
    }
    return current_index;
}

pub fn wakeThreadIfWaiting(thread_index: usize) void {
    const ctx = getThreadContext(thread_index) orelse return;
    if (!ctx.allocated) return;
    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    ctx.ready = true;
}

pub fn wakeWaitingThreadForPrincipal(principal: kernel.PrincipalId) void {
    const thread_index = threadSlotForPrincipal(principal) orelse return;
    const ctx = getThreadContext(thread_index) orelse return;
    if (!ctx.wait_mailbox) return;
    wakeThreadIfWaiting(thread_index);
}

pub fn wakeBlockedThreadForPrincipal(principal: kernel.PrincipalId) void {
    const thread_index = threadSlotForPrincipal(principal) orelse return;
    const ctx = getThreadContext(thread_index) orelse return;
    if (!ctx.allocated) return;
    if (!ctx.ready) {
        wakeThreadIfWaiting(thread_index);
        return;
    }
    ctx.signal_pending = true;
}

pub fn consumePendingSignalForPrincipal(principal: kernel.PrincipalId) bool {
    const thread_index = threadSlotForPrincipal(principal) orelse return false;
    const ctx = getThreadContext(thread_index) orelse return false;
    if (!ctx.allocated or !ctx.signal_pending) return false;
    ctx.signal_pending = false;
    return true;
}

pub fn wakeThreadsForTimer(now_tick: u64) void {
    var i: usize = 0;
    while (i < max_thread_slots) : (i += 1) {
        const ctx = getThreadContext(i) orelse continue;
        if (!ctx.allocated) continue;
        if (ctx.ready) continue;
        if (ctx.wake_tick == 0 or now_tick < ctx.wake_tick) continue;
        wakeThreadIfWaiting(i);
    }
}

pub fn blockCurrentThreadForEvent(frame: *TrapFrame, wait_mailbox: bool, timeout_ticks: u64, resume_rax: u64) bool {
    const current_thread = current_thread_index;
    const ctx = getThreadContext(current_thread) orelse return false;

    var saved = frame.*;
    saved.rax = resume_rax;
    ctx.frame = saved;
    ctx.cr3 = user_cr3_value;
    ctx.wait_mailbox = wait_mailbox;
    ctx.wake_tick = if (timeout_ticks == 0) 0 else lapic_tick_count + timeout_ticks;
    ctx.ready = false;

    const next_thread = pickNextReadyThreadIndex(current_thread);
    if (next_thread == current_thread) {
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.ready = true;
        return false;
    }
    if (!activateThread(next_thread)) {
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.ready = true;
        return false;
    }
    if (!loadThreadContextToFrame(next_thread, frame)) {
        ctx.wait_mailbox = false;
        ctx.wake_tick = 0;
        ctx.ready = true;
        _ = activateThread(current_thread);
        _ = loadThreadContextToFrame(current_thread, frame);
        return false;
    }
    return true;
}
