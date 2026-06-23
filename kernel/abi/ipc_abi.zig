pub const syscall_ipc_first: u64 = 42;
pub const syscall_ipc_endpoint_create: u64 = 42;
pub const syscall_ipc_channel_create: u64 = 43;
pub const syscall_ipc_send: u64 = 44;
pub const syscall_ipc_recv: u64 = 45;
pub const syscall_ipc_call: u64 = 46;
pub const syscall_ipc_reply: u64 = 47;
pub const syscall_ipc_last: u64 = syscall_ipc_reply;
pub const syscall_ipc_count: u64 = syscall_ipc_last - syscall_ipc_first + 1;

pub const max_inline_words: u64 = 4;
pub const max_transfer_fds: u64 = 8;

pub const msg_word0_offset: u64 = 0;
pub const msg_word1_offset: u64 = 8;
pub const msg_word2_offset: u64 = 16;
pub const msg_word3_offset: u64 = 24;
pub const msg_fd_array_offset: u64 = 32;
pub const msg_fd_count_offset: u64 = 40;
pub const msg_fd_capacity_offset: u64 = 48;
pub const msg_flags_offset: u64 = 56;
pub const msg_size: u64 = 64;

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
