const boot_static = @import("../boot/main_static.zig");
const scheduler = @import("../scheduler.zig");

pub fn deliverMessageToContext(ctx: *scheduler.ThreadContext, mr0: u64, mr1: u64, mr2: u64, mr3: u64) void {
    if (ctx.abi_trap_reply_pending) {
        ctx.abi_trap_reply_pending = false;
        ctx.frame.rax = mr0;
        if (mr2 != 0) ctx.frame.rip = mr2;
        if (mr3 != 0) ctx.frame.rsp = mr3;
        return;
    }
    ctx.frame.rax = boot_static.syscall_ok;
    ctx.frame.rdi = mr0;
    ctx.frame.rsi = mr1;
    ctx.frame.rdx = mr2;
    ctx.frame.r8 = mr3;
}

pub fn deliverOrQueueMessageToThread(
    target_thread: usize,
    endpoint_id: u64,
    sender_thread: usize,
    grants_reply: bool,
    mr0: u64,
    mr1: u64,
    mr2: u64,
    mr3: u64,
) u64 {
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return boot_static.syscall_err_endpoint;
    const target_hot = scheduler.getIpcHotThreadConst(target_thread) orelse return boot_static.syscall_err_endpoint;
    if (target_hot.allocated == 0) return boot_static.syscall_err_endpoint;
    if (target_hot.ready == 0) {
        target_ctx.wait_mailbox = false;
        target_ctx.wake_tick = 0;
        target_ctx.ready = true;
        scheduler.setIpcHotWaitState(target_thread, false, 0, true);
        if (grants_reply) {
            scheduler.setIpcReplyTokenForThread(target_thread, true, sender_thread);
        }
        deliverMessageToContext(target_ctx, mr0, mr1, mr2, mr3);
        scheduler.preferIpcSwitchToThread(target_thread);
        return boot_static.syscall_ok;
    }
    if (!scheduler.enqueueIpcMessageForThread(target_thread, endpoint_id, sender_thread, grants_reply, mr0, mr1, mr2, mr3)) {
        return boot_static.syscall_err_not_ready;
    }
    scheduler.preferIpcSwitchToThread(target_thread);
    return boot_static.syscall_ok;
}
