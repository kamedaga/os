/// Internal waits finish before signal delivery; teardown is never deferred.
pub const wait_uninterruptible: u64 = 1 << 1;

pub const syscall_ipc_first: u64 = 78;
pub const syscall_ipc_endpoint_create: u64 = 78;
pub const syscall_ipc_channel_create: u64 = 79;
pub const syscall_ipc_send: u64 = 80;
pub const syscall_ipc_recv: u64 = 81;
pub const syscall_ipc_call: u64 = 82;
pub const syscall_ipc_reply: u64 = 83;
pub const syscall_ipc_reply_recv: u64 = 84;
pub const syscall_ipc_reply_extract: u64 = 85;
pub const implicit_reply: u64 = ~@as(u64, 0);
pub const syscall_ipc_recv_wait: u64 = 86;
pub const syscall_ipc_notify: u64 = 87;
pub const syscall_ipc_notify_wait: u64 = 88;
pub const syscall_ipc_last: u64 = syscall_ipc_notify_wait;
pub const syscall_ipc_count: u64 = syscall_ipc_last - syscall_ipc_first + 1;

pub const max_inline_words: u64 = 4;

// IPC_NOTIFY_WAIT first latches the peer, so a caller that has just published
// shared-ring work and must now wait for the answer pays one syscall, not two.
pub const notify_wait_flag_notify_peer: u64 = 1 << 0;
pub const notify_wait_known_flags: u64 = notify_wait_flag_notify_peer | wait_uninterruptible;
pub const max_transfer_fds: u64 = 19;

/// Payload: R10/R8/R9/R12; capability: RDI; flags: RSI; timeout/commitment: RDX.
/// R13 is the send array, RBX the receive array. R15 carries send count in
/// low 32 bits and receive capacity in high 32 bits, then received count out.
/// No header is dereferenced. Unused arrays need not be mapped.
pub fn sendCount(frame: anytype) u64 {
    return frame.r15 & 0xffff_ffff;
}
pub fn receiveCapacity(frame: anytype) u64 {
    return frame.r15 >> 32;
}

/// Kernel snapshot, never re-read from userspace after a wait is published.
pub const ReceiveMemory = struct {
    active: bool = false,
    array: u64 = 0,
};

pub fn inputWords(frame: anytype) [4]u64 {
    return .{ frame.r10, frame.r8, frame.r9, frame.r12 };
}
pub fn outputWords(frame: anytype, words: [4]u64) void {
    frame.r10 = words[0];
    frame.r8 = words[1];
    frame.r9 = words[2];
    frame.r12 = words[3];
}

pub const fd_item_fd_offset: u64 = 0;
pub const fd_item_rights_offset: u64 = 8;
pub const fd_item_flags_offset: u64 = 16;
pub const fd_item_transfer_flags_offset: u64 = 24;
pub const fd_item_size: u64 = 32;

pub const transfer_move: u64 = 1 << 0;
pub const transfer_cloexec: u64 = 1 << 1;
pub const transfer_nonblock: u64 = 1 << 2;
pub const transfer_inherit: u64 = 1 << 3;
pub const transfer_private: u64 = 1 << 4;
pub const transfer_known_mask: u64 =
    transfer_move |
    transfer_cloexec |
    transfer_nonblock |
    transfer_inherit |
    transfer_private;

pub fn isIpcSyscall(nr: u64) bool {
    return nr >= syscall_ipc_first and nr <= syscall_ipc_last;
}
