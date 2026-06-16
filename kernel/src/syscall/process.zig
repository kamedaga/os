const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const process_builder = @import("../runtime/process_builder.zig");
const process_builder_abi = abi_root.process_builder_abi;

const TrapFrame = interrupts.TrapFrame;

pub fn dispatch(
    _: anytype,
    _: *kernel.KernelState,
    proc: kernel.PrincipalId,
    frame: *TrapFrame,
) ?u64 {
    return switch (frame.rax) {
        process_builder_abi.syscall_create_suspended_process => process_builder.createSuspendedProcess(proc),
        process_builder_abi.syscall_map_vm_object_to_process => process_builder.mapVmObjectToProcess(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx),
        process_builder_abi.syscall_map_vm_object_range_to_process => process_builder.mapVmObjectRangeToProcess(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx, frame.r8, frame.r9),
        process_builder_abi.syscall_alloc_map_pages_to_process => process_builder.allocMapPagesToProcess(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx, frame.r8),
        process_builder_abi.syscall_set_process_initial_context => process_builder.setInitialContext(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx),
        process_builder_abi.syscall_start_process => process_builder.startProcess(proc, frame.rdi),
        process_builder_abi.syscall_abort_process => process_builder.abortProcess(proc, frame.rdi),
        process_builder_abi.syscall_copy_to_process => process_builder.copyToProcess(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx),
        process_builder_abi.syscall_copy_from_process_to_process => process_builder.copyFromProcessToProcess(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx, frame.r8),
        process_builder_abi.syscall_share_process_pages_to_process => process_builder.shareProcessPagesToProcess(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx, frame.r8),
        process_builder_abi.syscall_mprotect_self => process_builder.mprotectSelf(proc, frame.rdi, frame.rsi, frame.rdx),
        process_builder_abi.syscall_set_process_bootstrap_owner => process_builder.setBootstrapOwner(proc, frame.rdi, frame.rsi),
        process_builder_abi.syscall_transfer_fd_to_process => process_builder.transferFdToProcess(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx, frame.r8),
        process_builder_abi.syscall_set_process_abi_trap_delegate => process_builder.setAbiTrapDelegate(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx, frame.r8),
        else => null,
    };
}
