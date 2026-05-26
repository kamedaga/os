const kernel = @import("../kernel.zig");
const interrupts = @import("../interrupts.zig");
const abi_trap_runtime = @import("../runtime/abi_trap.zig");
const ipc_syscalls = @import("ipc.zig");
const sc = @import("numbers.zig");

const TrapFrame = interrupts.TrapFrame;

pub fn dispatch(state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    return switch (frame.rax) {
        sc.syscall_map_abi_trap_reply_target_pages => {
            return abi_trap_runtime.mapPagesToCurrentReplyTarget(state, frame.rdi, frame.rsi, frame.rdx);
        },
        sc.syscall_copy_from_abi_trap_reply_target => {
            return abi_trap_runtime.copyFromCurrentReplyTarget(proc, frame.rdi, frame.rsi, frame.rdx);
        },
        sc.syscall_copy_to_abi_trap_reply_target => {
            return abi_trap_runtime.copyToCurrentReplyTarget(proc, frame.rdi, frame.rsi, frame.rdx);
        },
        sc.syscall_set_abi_trap_reply_target_fs_base => {
            return abi_trap_runtime.setCurrentReplyTargetFsBase(frame.rdi);
        },
        sc.syscall_protect_abi_trap_reply_target_pages => {
            return abi_trap_runtime.protectCurrentReplyTargetPages(frame.rdi, frame.rsi, frame.rdx);
        },
        sc.syscall_unmap_abi_trap_reply_target_pages => {
            return abi_trap_runtime.unmapCurrentReplyTargetPages(state, frame.rdi, frame.rsi);
        },
        sc.syscall_map_abi_trap_reply_target_vm_object => {
            return abi_trap_runtime.mapVmObjectRangeToCurrentReplyTarget(state, proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx, frame.r8);
        },
        sc.syscall_reply_abi_trap_target => {
            return abi_trap_runtime.replyToTarget(state, proc, frame.rdi, frame.rsi, frame.rdx);
        },
        sc.syscall_reply_abi_trap_target_context => {
            return abi_trap_runtime.replyToTargetContext(state, proc, frame.rdi, frame.rsi, frame.rdx, frame.r10);
        },
        sc.syscall_detach_abi_trap_reply_token => {
            return ipc_syscalls.detachCurrentReplyToken();
        },
        sc.syscall_copy_to_abi_trap_target => {
            return abi_trap_runtime.copyToTarget(state, proc, frame.rdi, frame.rsi, frame.rdx, frame.r10);
        },
        sc.syscall_copy_from_abi_trap_target => {
            return abi_trap_runtime.copyFromTarget(state, proc, frame.rdi, frame.rsi, frame.rdx, frame.r10);
        },
        sc.syscall_set_abi_trap_target_request_page => {
            return abi_trap_runtime.setTargetRequestPage(state, proc, frame.rdi, frame.rsi);
        },
        else => null,
    };
}
