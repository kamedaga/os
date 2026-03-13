const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const interrupts = @import("interrupts.zig");

const TrapFrame = interrupts.TrapFrame;
const UserAddressSpace = capability.UserAddressSpace;

const fx_state_bytes: usize = 512;
const user_thread_count: usize = kernel.process_count;

pub const ThreadContext = struct {
    id: u32 = 0,
    owner_process: kernel.PrincipalId,
    cr3: u64 = 0,
    ready: bool = false,
    wait_mailbox: bool = false,
    wake_tick: u64 = 0,
    frame: TrapFrame = std.mem.zeroes(TrapFrame),
    fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes,
};

pub fn buildInitialThreadContexts() [user_thread_count]ThreadContext {
    var contexts: [user_thread_count]ThreadContext = undefined;
    inline for (0..user_thread_count) |i| {
        contexts[i] = .{
            .id = @intCast(i),
            .owner_process = kernel.processPrincipalFromIndex(i) orelse unreachable,
        };
    }
    return contexts;
}

pub var thread_contexts: [user_thread_count]ThreadContext = buildInitialThreadContexts();
pub export var user_cr3_value: u64 = 0;
pub var current_user_principal: kernel.PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
pub var current_thread_index: usize = 0;
pub var lapic_tick_count: u64 = 0;
pub var scheduler_tick_accum: u64 = 0;
pub var scheduler_switch_count: u64 = 0;
pub var scheduler_perf_user_ticks: u64 = 0;
pub var scheduler_perf_process1_ticks: u64 = 0;
pub var scheduler_perf_thread1_ticks: u64 = 0;
pub var scheduler_probe_log_count: u64 = 0;
pub var compositor_thread1_priority_streak: u64 = 0;
pub var scheduler_int80_log_count: u64 = 0;
pub var scheduler_race_log_count: u64 = 0;
pub var compositor_thread1_priority_active = false;
pub var initial_fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes;
pub var kernel_interrupt_fx_state: [fx_state_bytes]u8 align(16) = [_]u8{0} ** fx_state_bytes;

fn getUserSpace(user_spaces: []UserAddressSpace, principal: kernel.PrincipalId) ?*UserAddressSpace {
    const idx = kernel.processIndexFromPrincipal(principal) orelse return null;
    if (idx >= user_spaces.len) return null;
    return &user_spaces[idx];
}

pub fn getThreadContext(thread_index: usize) ?*ThreadContext {
    if (thread_index >= user_thread_count) return null;
    return &thread_contexts[thread_index];
}

pub fn getThreadContextConst(thread_index: usize) ?*const ThreadContext {
    if (thread_index >= user_thread_count) return null;
    return &thread_contexts[thread_index];
}

pub fn initThreadContextWithSpaces(
    thread_index: usize,
    owner_process: kernel.PrincipalId,
    user_spaces: []UserAddressSpace,
    initial_frame: TrapFrame,
) bool {
    const space = getUserSpace(user_spaces, owner_process) orelse return false;
    const ctx = getThreadContext(thread_index) orelse return false;
    ctx.id = @intCast(thread_index);
    ctx.owner_process = owner_process;
    ctx.cr3 = space.cr3;
    ctx.ready = true;
    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    ctx.frame = initial_frame;
    ctx.fx_state = initial_fx_state;
    return true;
}

fn expectedPrincipalForThread(thread_index: usize) ?kernel.PrincipalId {
    return kernel.processPrincipalFromIndex(thread_index);
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
    const expected = expectedPrincipalForThread(thread_index) orelse return true;
    const owner_raw = rawOwnerTag(ctx);
    const owner = principalFromRawTag(owner_raw) orelse return true;
    if (owner != expected) return true;
    if ((ctx.cr3 & 0xFFF) != 0) return true;
    if (ctx.cr3 == 0) return true;
    return false;
}

pub fn repairThreadContextWithSpaces(thread_index: usize, user_spaces: []UserAddressSpace, initial_frame: TrapFrame) bool {
    const expected = expectedPrincipalForThread(thread_index) orelse return false;
    return initThreadContextWithSpaces(thread_index, expected, user_spaces, initial_frame);
}

pub fn sanitizeAllThreadContextsWithSpaces(user_spaces: []UserAddressSpace, initial_frame: TrapFrame) void {
    var i: usize = 0;
    while (i < user_thread_count) : (i += 1) {
        const ctx = getThreadContext(i) orelse continue;
        const was_ready = ctx.ready;
        if (!repairThreadContextWithSpaces(i, user_spaces, initial_frame)) continue;
        if (!was_ready) ctx.ready = false;
    }
}

pub fn activateThread(thread_index: usize) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.ready) return false;
    current_thread_index = thread_index;
    current_user_principal = ctx.owner_process;
    user_cr3_value = ctx.cr3;
    return true;
}

pub fn saveCurrentThreadContextFromFrame(frame: *const TrapFrame) void {
    const ctx = getThreadContext(current_thread_index) orelse return;
    ctx.frame = frame.*;
    ctx.cr3 = user_cr3_value;
    if (!ctx.wait_mailbox and ctx.wake_tick == 0) {
        ctx.ready = true;
    }
}

pub fn loadThreadContextToFrame(thread_index: usize, frame: *TrapFrame) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.ready) return false;
    frame.* = ctx.frame;
    return true;
}

pub fn switchToThread(next_thread: usize, frame: *TrapFrame, saved_rax: ?u64) bool {
    if (next_thread >= user_thread_count) return false;
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
    if (current_index >= user_thread_count) return 0;
    if (compositor_thread1_priority_active) {
        const compositor_ctx = getThreadContextConst(1) orelse return current_index;
        if (current_index != 1 and compositor_ctx.ready) return 1;
    }
    var step: usize = 1;
    while (step <= user_thread_count) : (step += 1) {
        const idx = (current_index + step) % user_thread_count;
        const ctx = getThreadContextConst(idx) orelse continue;
        if (ctx.ready) return idx;
    }
    return current_index;
}

pub fn wakeThreadIfWaiting(thread_index: usize) void {
    const ctx = getThreadContext(thread_index) orelse return;
    ctx.wait_mailbox = false;
    ctx.wake_tick = 0;
    ctx.ready = true;
}

pub fn wakeWaitingThreadForPrincipal(principal: kernel.PrincipalId) void {
    const thread_index = kernel.processIndexFromPrincipal(principal) orelse return;
    const ctx = getThreadContext(thread_index) orelse return;
    if (!ctx.wait_mailbox) return;
    wakeThreadIfWaiting(thread_index);
}

pub fn wakeThreadsForTimer(now_tick: u64) void {
    var i: usize = 0;
    while (i < user_thread_count) : (i += 1) {
        const ctx = getThreadContext(i) orelse continue;
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
