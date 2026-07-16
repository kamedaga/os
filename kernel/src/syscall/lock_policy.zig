const abi_root = @import("kernel_abi_root");
const capsule_abi = abi_root.capsule_abi;
const fd_abi = abi_root.fd_abi;
const ipc_abi = abi_root.ipc_abi;
const process_abi = abi_root.process_abi;
const runtime_abi = abi_root.runtime_abi;
const vm_abi = abi_root.vm_abi;

pub fn needsKernelStateLock(nr: u64) bool {
    if (nr == runtime_abi.syscall_nanosleep) return false;
    if (nr == fd_abi.syscall_vmo_revoke) return true;
    return process_abi.isProcessSyscall(nr) or
        capsule_abi.isCapsuleSyscall(nr) or
        fd_abi.isFdSyscall(nr) or
        ipc_abi.isIpcSyscall(nr) or
        runtime_abi.isRuntimeSyscall(nr) or
        (nr >= vm_abi.syscall_vm_first and nr <= vm_abi.syscall_vm_last);
}
