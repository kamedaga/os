pub const fd_table_entries: u32 = 256;

pub const syscall_fd_first: u64 = 0x100;
pub const syscall_fd_close: u64 = 0x100;
pub const syscall_fd_dup: u64 = 0x101;
pub const syscall_fd_replace: u64 = 0x102;
pub const syscall_fd_get_info: u64 = 0x103;
pub const syscall_fd_set_flags: u64 = 0x104;
pub const syscall_fd_wait: u64 = 0x105;
pub const syscall_fd_poll: u64 = 0x106;
pub const syscall_fd_last: u64 = syscall_fd_poll;
pub const syscall_fd_count: u64 = syscall_fd_last - syscall_fd_first + 1;

pub const flag_cloexec: u32 = 1 << 0;
pub const flag_nonblock: u32 = 1 << 1;
pub const flag_inherit: u32 = 1 << 2;
pub const flag_private: u32 = 1 << 3;
pub const known_flags_mask: u32 =
    flag_cloexec |
    flag_nonblock |
    flag_inherit |
    flag_private;

pub const right_inspect: u64 = 1 << 0;
pub const right_dup: u64 = 1 << 1;
pub const right_transfer: u64 = 1 << 2;
pub const right_wait: u64 = 1 << 3;
pub const right_poll: u64 = 1 << 4;
pub const right_set_flags: u64 = 1 << 5;
pub const right_close: u64 = 1 << 6;
pub const known_common_rights_mask: u64 =
    right_inspect |
    right_dup |
    right_transfer |
    right_wait |
    right_poll |
    right_set_flags |
    right_close;

pub fn isFdSyscall(nr: u64) bool {
    return nr >= syscall_fd_first and nr <= syscall_fd_last;
}
