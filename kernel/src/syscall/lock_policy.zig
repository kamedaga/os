const abi_root = @import("kernel_abi_root");
const capsule_abi = abi_root.capsule_abi;
const fd_abi = abi_root.fd_abi;
const ipc_abi = abi_root.ipc_abi;
const process_abi = abi_root.process_abi;

pub fn needsKernelStateLock(nr: u64) bool {
    return process_abi.isProcessSyscall(nr) or
        capsule_abi.isCapsuleSyscall(nr) or
        fd_abi.isFdSyscall(nr) or
        ipc_abi.isIpcSyscall(nr);
}
