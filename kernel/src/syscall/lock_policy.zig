const abi_root = @import("kernel_abi_root");
const capsule_abi = abi_root.capsule_abi;
const fd_abi = abi_root.fd_abi;
const ipc_abi = abi_root.ipc_abi;
const pager_abi = abi_root.pager_abi;
const process_abi = abi_root.process_abi;
const runtime_abi = abi_root.runtime_abi;
const vm_abi = abi_root.vm_abi;

// Ordinary dispatch owns K for every stateful IPC handler. The dedicated
// entry has its own read-only tryLock admission and never uses this policy.
pub fn needsKernelStateLock(nr: u64) bool {
    // The running CPU pins its principal/thread identity; yield transfers
    // ownership under scheduler locks. Clock resolution reads an immutable
    // source and copies under AS exclusion. Futex WAIT compares under F -> AS
    // and publishes under F -> scheduler; WAKE consumes that same F-owned
    // record. WAKE_WAIT combines these steps under F, and REQUEUE migrates
    // keys under F. None creates K-only retirement work for syscalls.zig.
    // Process exit still takes K -> F for clearTidAndWake.
    if (nr == runtime_abi.syscall_getpid or nr == runtime_abi.syscall_gettid or
        nr == runtime_abi.syscall_clock_getres or nr == runtime_abi.syscall_yield or
        nr == runtime_abi.syscall_nanosleep or nr == runtime_abi.syscall_futex_wait or
        nr == runtime_abi.syscall_futex_wake or nr == runtime_abi.syscall_futex_wake_wait or
        nr == runtime_abi.syscall_futex_requeue or nr == runtime_abi.syscall_futex_wake_op) return false;
    if (nr == fd_abi.syscall_vmo_revoke) return true;
    return process_abi.isProcessSyscall(nr) or
        (nr >= pager_abi.syscall_pager_port_create and nr <= pager_abi.syscall_pager_fail) or
        capsule_abi.isCapsuleSyscall(nr) or
        fd_abi.isFdSyscall(nr) or
        ipc_abi.isIpcSyscall(nr) or
        runtime_abi.isRuntimeSyscall(nr) or
        (nr >= vm_abi.syscall_vm_first and nr <= vm_abi.syscall_vm_last);
}

test "runtime scalar identity scheduler handoff and futex need no K retirement" {
    const std = @import("std");
    for ([_]u64{ runtime_abi.syscall_getpid, runtime_abi.syscall_gettid,
        runtime_abi.syscall_clock_getres, runtime_abi.syscall_yield,
        runtime_abi.syscall_nanosleep, runtime_abi.syscall_futex_wait,
        runtime_abi.syscall_futex_wake, runtime_abi.syscall_futex_wake_wait,
        runtime_abi.syscall_futex_requeue, runtime_abi.syscall_futex_wake_op }) |nr|
        try std.testing.expect(!needsKernelStateLock(nr));
    for ([_]u64{ runtime_abi.syscall_system_info, runtime_abi.syscall_clock_read,
        runtime_abi.syscall_clock_gettime,
        runtime_abi.syscall_clock_page_open, runtime_abi.syscall_getrandom,
        runtime_abi.syscall_power_control }) |nr|
        try std.testing.expect(needsKernelStateLock(nr));
}

test "pager stateful calls retain K through FD and VMO loans" {
    const std = @import("std");
    for (pager_abi.syscall_pager_port_create..pager_abi.syscall_pager_fail + 1) |nr|
        try std.testing.expect(needsKernelStateLock(nr));
}

