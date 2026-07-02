const std = @import("std");
const kernel = @import("kernel.zig");
const interrupts = @import("interrupts.zig");
const scheduler = @import("scheduler.zig").connection;
const boot_static = @import("boot/main_static.zig");
const sc = @import("syscall/numbers.zig");
const syscall_lock_policy = @import("syscall/lock_policy.zig");
const capsule_syscalls = @import("syscall/capsule.zig");
const fd_syscalls = @import("syscall/fd.zig");
const native_ipc_syscalls = @import("syscall/native_ipc.zig");
const process_syscalls = @import("syscall/process.zig");
const runtime_syscalls = @import("syscall/runtime.zig");

const TrapFrame = interrupts.TrapFrame;

pub const Hooks = struct {
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    user_spaces: []boot_static.UserAddressSpace,
    kernel_state_ready: *const bool,
    write: *const fn ([]const u8) void,
    print_hex: *const fn (u64) void,
    print_number: *const fn (u64) void,
    principal_label: *const fn (kernel.PrincipalId) []const u8,
    read_user_u64: *const fn (kernel.PrincipalId, u64) ?u64,
    write_user_u64: *const fn (kernel.PrincipalId, u64, u64) bool,
    copy_user_bytes_from_va: *const fn (kernel.PrincipalId, u64, []u8) bool,
    copy_bytes_to_user_va: *const fn (kernel.PrincipalId, u64, []const u8) bool,
    wake_waiting_thread_for_principal: *const fn (kernel.PrincipalId) void,
    wake_waiting_thread_generation: *const fn (usize, u32) bool,
    wake_waiting_thread_generation_with_rax: *const fn (usize, u32, u64) bool,
    wake_blocked_thread_for_principal: *const fn (kernel.PrincipalId) void,
    consume_pending_signal_for_principal: *const fn (kernel.PrincipalId) bool,
    switch_to_thread: *const fn (usize, *TrapFrame, ?u64) bool,
    before_current_thread_leave: ?scheduler.BeforeCurrentThreadLeaveCallback = null,
    block_current_thread_for_event: *const fn (*TrapFrame, bool, u64, u64, ?scheduler.BeforeCurrentThreadLeaveCallback) bool,
    exit_current_process: *const fn (kernel.PrincipalId, u8, *TrapFrame, ?scheduler.BeforeCurrentThreadLeaveCallback, ?kernel.ThreadWakeTarget) void,
    total_usable_memory_bytes: u64,
};

var syscall_hooks_storage: Hooks = undefined;
var syscall_hooks_ready = false;

const KernelStateSpinLock = struct {
    value: u8 = 0,

    fn lock(self: *KernelStateSpinLock) void {
        while (true) {
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) return;
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                asm volatile ("pause");
            }
        }
    }

    fn unlock(self: *KernelStateSpinLock) void {
        @atomicStore(u8, &self.value, 0, .release);
    }
};

var kernel_state_lock: KernelStateSpinLock = .{};

const KernelStateLockDropContext = struct {
    lock_held: *bool,
};

fn dropKernelStateLockBeforeCurrentThreadLeave(ctx_ptr: *anyopaque) void {
    const ctx: *KernelStateLockDropContext = @ptrCast(@alignCast(ctx_ptr));
    if (!ctx.lock_held.*) return;
    kernel_state_lock.unlock();
    ctx.lock_held.* = false;
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_hooks_storage), &syscall_hooks_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(syscall_hooks_ready), &syscall_hooks_ready));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_state_lock), &kernel_state_lock));
    end = maxStaticEnd(end, runtime_syscalls.kernelStaticStorageEndAddr());
    return end;
}

pub fn init(new_hooks: Hooks) void {
    syscall_hooks_storage = new_hooks;
    syscall_hooks_ready = true;
}

pub fn updateUserSpaces(user_spaces: []boot_static.UserAddressSpace) void {
    if (!syscall_hooks_ready) return;
    syscall_hooks_storage.user_spaces = user_spaces;
}

fn getHooks() *const Hooks {
    if (!syscall_hooks_ready) unreachable;
    return &syscall_hooks_storage;
}

fn hasExplicitUserLogLabel(message: []const u8) bool {
    if (message.len < 3 or message[0] != '[') return false;
    const close_index = std.mem.indexOfScalar(u8, message, ']') orelse return false;
    if (close_index <= 1) return false;
    return close_index + 1 == message.len or message[close_index + 1] == ' ';
}

fn writeThreadUserLogPrefix(h: *const Hooks, thread_index: usize) void {
    h.write("[Thread ");
    h.print_number(@intCast(thread_index));
    h.write("] ");
}

fn dispatchCompactSyscall(frame: *TrapFrame) u64 {
    const base_hooks = getHooks();
    if (!base_hooks.kernel_state_ready.*) return sc.syscall_err_not_ready;

    const state = base_hooks.state;
    const proc = scheduler.currentPrincipal();
    const hold_kernel_state_lock = syscall_lock_policy.needsKernelStateLock(frame.rax);
    var lock_held = false;
    var lock_drop_context = KernelStateLockDropContext{ .lock_held = &lock_held };
    var hooks = base_hooks.*;
    hooks.before_current_thread_leave = if (hold_kernel_state_lock)
        scheduler.BeforeCurrentThreadLeaveCallback{
            .context = &lock_drop_context,
            .run = dropKernelStateLockBeforeCurrentThreadLeave,
        }
    else
        null;
    const h = &hooks;

    if (hold_kernel_state_lock) {
        kernel_state_lock.lock();
        lock_held = true;
    }
    defer {
        if (lock_held) kernel_state_lock.unlock();
    }

    if (!state.hasActivePrincipal(proc)) {
        base_hooks.write("syscall from inactive principal proc=");
        base_hooks.write(base_hooks.principal_label(proc));
        base_hooks.write(" thread=");
        base_hooks.print_number(@intCast(scheduler.currentThread()));
        base_hooks.write("\n");
        if (lock_held) {
            kernel_state_lock.unlock();
            lock_held = false;
        }
        base_hooks.exit_current_process(proc, 0, frame, null, null);
        return sc.syscall_ok;
    }

    if (process_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (capsule_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (fd_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (native_ipc_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }
    if (runtime_syscalls.dispatch(h, state, proc, frame)) |result| {
        return result;
    }

    return switch (frame.rax) {
        sc.syscall_log => blk: {
            const req_len_u64 = frame.rsi;
            if (req_len_u64 == 0) break :blk sc.syscall_ok;
            if (req_len_u64 > sc.user_log_max_bytes) break :blk sc.syscall_err_invalid;
            const req_len: usize = @intCast(req_len_u64);
            var buf: [sc.user_log_max_bytes]u8 = undefined;
            const msg = buf[0..req_len];
            if (!h.copy_user_bytes_from_va(proc, frame.rdi, msg)) break :blk sc.syscall_err_invalid;
            if (!hasExplicitUserLogLabel(msg)) {
                writeThreadUserLogPrefix(h, scheduler.currentThread());
            }
            h.write(msg);
            break :blk sc.syscall_ok;
        },
        else => sc.syscall_err_invalid,
    };
}

pub export fn syscallDispatch(frame: *TrapFrame) callconv(.winapi) u64 {
    const result = dispatchCompactSyscall(frame);
    frame.rax = result;
    return result;
}

test "explicit userlog label detection" {
    try std.testing.expect(hasExplicitUserLogLabel("[seed] ready\n"));
    try std.testing.expect(hasExplicitUserLogLabel("[seed]"));
    try std.testing.expect(!hasExplicitUserLogLabel("seed ready\n"));
    try std.testing.expect(!hasExplicitUserLogLabel("[seed"));
    try std.testing.expect(!hasExplicitUserLogLabel("[] bad\n"));
}
