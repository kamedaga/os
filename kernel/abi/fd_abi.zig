pub const fd_table_entries: u32 = 256;
pub const first_dynamic_fd: u32 = 16;

pub const syscall_fd_first: u64 = 0x100;
pub const syscall_fd_close: u64 = 0x100;
pub const syscall_fd_dup: u64 = 0x101;
pub const syscall_fd_replace: u64 = 0x102;
pub const syscall_fd_get_info: u64 = 0x103;
pub const syscall_fd_set_flags: u64 = 0x104;
pub const syscall_fd_wait: u64 = 0x105;
pub const syscall_fd_poll: u64 = 0x106;
pub const syscall_vmo_create: u64 = 0x107;
pub const syscall_vmo_from_current_pages: u64 = 0x108;
pub const syscall_mmap: u64 = 0x109;
pub const syscall_munmap: u64 = 0x10A;
pub const syscall_fd_last: u64 = syscall_munmap;
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
pub const right_map_read: u64 = 1 << 13;
pub const right_map_write: u64 = 1 << 14;
pub const right_map_exec: u64 = 1 << 15;
pub const right_resize: u64 = 1 << 16;
pub const right_share: u64 = 1 << 17;
pub const right_pager_attach: u64 = 1 << 18;
pub const right_pager_fault: u64 = 1 << 19;
pub const known_common_rights_mask: u64 =
    right_inspect |
    right_dup |
    right_transfer |
    right_wait |
    right_poll |
    right_set_flags |
    right_close |
    right_map_read |
    right_map_write |
    right_map_exec |
    right_resize |
    right_share |
    right_pager_attach |
    right_pager_fault;

pub const prot_read: u64 = 1 << 0;
pub const prot_write: u64 = 1 << 1;
pub const prot_exec: u64 = 1 << 2;

pub const mmap_fixed: u64 = 1 << 0;
pub const mmap_fixed_noreplace: u64 = 1 << 1;
pub const mmap_private: u64 = 1 << 2;
pub const mmap_shared: u64 = 1 << 3;
pub const mmap_anonymous: u64 = 1 << 4;
pub const mmap_noreserve: u64 = 1 << 5;
pub const mmap_pkey_shift: u64 = 8;
pub const mmap_pkey_mask: u64 = 0xF << mmap_pkey_shift;

pub const fd_kind_none: u64 = 0;
pub const fd_kind_vmo: u64 = 6;
pub const fd_kind_endpoint: u64 = 7;
pub const fd_kind_channel: u64 = 8;
pub const fd_kind_reply: u64 = 9;

pub const fd_info_kind_offset: u64 = 0;
pub const fd_info_rights_offset: u64 = 8;
pub const fd_info_flags_offset: u64 = 16;
pub const fd_info_size_offset: u64 = 24;
pub const fd_info_extra_offset: u64 = 32;
pub const fd_info_size: u64 = 40;

pub fn isFdSyscall(nr: u64) bool {
    return nr >= syscall_fd_first and nr <= syscall_fd_last;
}
