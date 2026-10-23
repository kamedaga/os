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
    if (nr == runtime_abi.syscall_nanosleep) return false;
    if (nr == fd_abi.syscall_vmo_revoke) return true;
    return process_abi.isProcessSyscall(nr) or
        (nr >= pager_abi.syscall_pager_port_create and nr <= pager_abi.syscall_pager_fail) or
        capsule_abi.isCapsuleSyscall(nr) or
        fd_abi.isFdSyscall(nr) or
        ipc_abi.isIpcSyscall(nr) or
        runtime_abi.isRuntimeSyscall(nr) or
        (nr >= vm_abi.syscall_vm_first and nr <= vm_abi.syscall_vm_last);
}

test "pager stateful calls retain K through FD and VMO loans" {
    const std = @import("std");
    for (pager_abi.syscall_pager_port_create..pager_abi.syscall_pager_fail + 1) |nr|
        try std.testing.expect(needsKernelStateLock(nr));
}

