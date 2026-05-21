const kernel = @import("../kernel.zig");
const interrupts = @import("../interrupts.zig");
const abi_trap_runtime = @import("../runtime/abi_trap.zig");
const ipc_syscalls = @import("ipc.zig");
const sc = @import("numbers.zig");

const TrapFrame = interrupts.TrapFrame;

pub fn dispatch(state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    return switch (frame.rax) {
        sc.syscall_set_abi_trap_delegate => {
            state.setAbiTrapDelegate(proc, frame.rdi, @truncate(frame.rsi), frame.rdx) catch |err| switch (err) {
                kernel.KernelError.EndpointNotFound => return sc.syscall_err_endpoint,
                else => return sc.syscall_err_invalid,
            };
            return sc.syscall_ok;
        },
        sc.syscall_clear_abi_trap_delegate => {
            state.clearAbiTrapDelegate(proc) catch return sc.syscall_err_invalid;
            return sc.syscall_ok;
        },
        sc.syscall_map_abi_trap_reply_target_pages => {
            return abi_trap_runtime.mapPagesToCurrentReplyTarget(state, frame.rdi, frame.rsi, frame.rdx);
        },
        sc.syscall_copy_from_abi_trap_reply_target => {
            return abi_trap_runtime.copyFromCurrentReplyTarget(proc, frame.rdi, frame.rsi, frame.rdx);
        },
        sc.syscall_copy_to_abi_trap_reply_target => {
            return abi_trap_runtime.copyToCurrentReplyTarget(proc, frame.rdi, frame.rsi, frame.rdx);
        },
        sc.syscall_copy_to_abi_trap_reply_target_bulk => {
            return abi_trap_runtime.copyToCurrentReplyTargetBulk(proc, frame.rdi, frame.rsi, frame.rdx);
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
        sc.syscall_reclaim_abi_trap_reply_target_private_pages => {
            return abi_trap_runtime.reclaimCurrentReplyTargetPrivatePages(state);
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
        sc.syscall_start_abi_trap_target => {
            return abi_trap_runtime.startTarget(state, proc, frame.rdi);
        },
        sc.syscall_set_abi_trap_target_request_page => {
            return abi_trap_runtime.setTargetRequestPage(state, proc, frame.rdi, frame.rsi);
        },
        sc.syscall_share_abi_trap_reply_target_pages_to_target => {
            return abi_trap_runtime.shareCurrentReplyTargetPagesToTarget(state, proc, frame.rdi, frame.rsi, frame.rdx, frame.r10);
        },
        sc.syscall_unmap_abi_trap_target_pages => {
            return abi_trap_runtime.unmapTargetPages(state, proc, frame.rdi, frame.rsi, frame.rdx);
        },
        sc.syscall_reserve_abi_trap_reply_target_pages => {
            return abi_trap_runtime.reserveLazyAnonymousPagesForCurrentReplyTarget(state, frame.rdi, frame.rsi, frame.rdx);
        },
        sc.syscall_grant_abi_trap_reply_target_pages_as_ipc_buffers => {
            return abi_trap_runtime.grantCurrentReplyTargetPagesAsIpcBuffers(state, proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx);
        },
        sc.syscall_map_current_pages_to_abi_trap_reply_target => {
            return abi_trap_runtime.mapCurrentPagesToCurrentReplyTarget(state, proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx);
        },
        sc.syscall_cow_abi_trap_reply_target_page => {
            return abi_trap_runtime.cowCurrentReplyTargetPage(state, frame.rdi, frame.rsi);
        },
        else => null,
    };
}
