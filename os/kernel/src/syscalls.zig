const std = @import("std");
const kernel = @import("kernel.zig");
const interrupts = @import("interrupts.zig");
const scheduler = @import("scheduler.zig");
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
    wake_blocked_thread_for_principal: *const fn (kernel.PrincipalId) void,
    consume_pending_signal_for_principal: *const fn (kernel.PrincipalId) bool,
    switch_to_thread: *const fn (usize, *TrapFrame, ?u64) bool,
    block_current_thread_for_event: *const fn (*TrapFrame, bool, u64, u64) bool,
    exit_current_process: *const fn (kernel.PrincipalId, u8, *TrapFrame) void,
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
    const h = getHooks();
    if (!h.kernel_state_ready.*) return sc.syscall_err_not_ready;

    const state = h.state;
    const proc = scheduler.currentUserPrincipal();
    if (!state.hasActivePrincipal(proc)) {
        h.write("syscall from inactive principal proc=");
        h.write(h.principal_label(proc));
        h.write(" thread=");
        h.print_number(@intCast(scheduler.currentThreadIndex()));
        h.write("\n");
        h.exit_current_process(proc, 0, frame);
        return sc.syscall_ok;
    }

    const hold_kernel_state_lock = syscall_lock_policy.needsKernelStateLock(frame.rax);
    if (hold_kernel_state_lock) {
        kernel_state_lock.lock();
    }
    defer {
        if (hold_kernel_state_lock) kernel_state_lock.unlock();
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
    if (runtime_syscalls.dispatch(h, proc, frame)) |result| {
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
                writeThreadUserLogPrefix(h, scheduler.currentThreadIndex());
            }
            h.write(msg);
            break :blk sc.syscall_ok;
        },
        else => sc.syscall_err_invalid,
    };
}

pub export fn syscallDispatch(frame: *TrapFrame) callconv(.c) u64 {
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
