pub const fd_table_entries: u32 = 256;
pub const first_dynamic_fd: u32 = 16;

pub const syscall_fd_first: u64 = 28;
pub const syscall_fd_close: u64 = 28;
pub const syscall_fd_dup: u64 = 29;
pub const syscall_fd_get_info: u64 = 30;
pub const syscall_fd_set_flags: u64 = 31;
pub const syscall_fd_read: u64 = 32;
pub const syscall_fd_write: u64 = 33;
pub const syscall_fd_readv: u64 = 34;
pub const syscall_fd_writev: u64 = 35;
pub const syscall_fd_fcntl: u64 = 36;
pub const syscall_fd_poll: u64 = 37;
pub const syscall_fd_wait_many: u64 = 38;
pub const syscall_fd_ioctl: u64 = 39;
pub const syscall_fd_stat: u64 = 40;
pub const syscall_eventfd_create: u64 = 41;
pub const syscall_pipe_create: u64 = 42;
pub const syscall_timerfd_create: u64 = 43;
pub const syscall_timerfd_settime: u64 = 44;
pub const syscall_timerfd_gettime: u64 = 45;
pub const syscall_vmo_create: u64 = 46;
pub const syscall_fd_last: u64 = syscall_vmo_create;
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
pub const right_read: u64 = 1 << 42;
pub const right_write: u64 = 1 << 43;
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
    right_read |
    right_write |
    right_map_read |
    right_map_write |
    right_map_exec |
    right_resize |
    right_share |
    right_pager_attach |
    right_pager_fault;

pub const fd_kind_none: u64 = 0;
pub const fd_kind_process: u64 = 1;
pub const fd_kind_thread: u64 = 2;
pub const fd_kind_event: u64 = 3;
pub const fd_kind_vmo: u64 = 4;
pub const fd_kind_endpoint: u64 = 5;
pub const fd_kind_channel: u64 = 6;
pub const fd_kind_reply: u64 = 7;
pub const fd_kind_device: u64 = 8;
pub const fd_kind_mmio_region: u64 = 9;
pub const fd_kind_dma_buffer: u64 = 10;
pub const fd_kind_dma_mapping: u64 = 11;
pub const fd_kind_irq: u64 = 12;
pub const fd_kind_timer: u64 = 13;
pub const fd_kind_serial: u64 = 14;
pub const fd_kind_schedctl: u64 = 15;
pub const fd_kind_sched_event: u64 = 16;
pub const fd_kind_pipe: u64 = 17;

pub const fd_info_kind_offset: u64 = 0;
pub const fd_info_rights_offset: u64 = 8;
pub const fd_info_flags_offset: u64 = 16;
pub const fd_info_size_offset: u64 = 24;
pub const fd_info_extra_offset: u64 = 32;
pub const fd_info_size: u64 = 40;

pub const stat_dev_offset: u64 = 0;
pub const stat_ino_offset: u64 = 8;
pub const stat_nlink_offset: u64 = 16;
pub const stat_mode_offset: u64 = 24;
pub const stat_uid_offset: u64 = 28;
pub const stat_gid_offset: u64 = 32;
pub const stat_pad0_offset: u64 = 36;
pub const stat_rdev_offset: u64 = 40;
pub const stat_size_offset: u64 = 48;
pub const stat_blksize_offset: u64 = 56;
pub const stat_blocks_offset: u64 = 64;
pub const stat_atime_sec_offset: u64 = 72;
pub const stat_atime_nsec_offset: u64 = 80;
pub const stat_mtime_sec_offset: u64 = 88;
pub const stat_mtime_nsec_offset: u64 = 96;
pub const stat_ctime_sec_offset: u64 = 104;
pub const stat_ctime_nsec_offset: u64 = 112;
pub const stat_unused0_offset: u64 = 120;
pub const stat_unused1_offset: u64 = 128;
pub const stat_unused2_offset: u64 = 136;
pub const stat_size: u64 = 144;

pub const stat_mode_ififo: u64 = 0o010000;
pub const stat_mode_ifchr: u64 = 0o020000;
pub const stat_mode_ifreg: u64 = 0o100000;
pub const stat_mode_irusr: u64 = 0o400;
pub const stat_mode_iwusr: u64 = 0o200;
pub const stat_mode_ixusr: u64 = 0o100;

pub const ioctl_tiocgwinsz: u64 = 0x5413;
pub const ioctl_tiocswinsz: u64 = 0x5414;
pub const winsize_rows_offset: u64 = 0;
pub const winsize_cols_offset: u64 = 2;
pub const winsize_xpixel_offset: u64 = 4;
pub const winsize_ypixel_offset: u64 = 6;
pub const winsize_size: u64 = 8;

pub const fcntl_get_flags: u64 = 1;
pub const fcntl_set_flags: u64 = 2;
pub const fcntl_dup: u64 = 3;

pub const event_readable: u64 = 1 << 0;
pub const event_writable: u64 = 1 << 1;
pub const event_error: u64 = 1 << 2;
pub const event_hangup: u64 = 1 << 3;
pub const event_known_mask: u64 =
    event_readable |
    event_writable |
    event_error |
    event_hangup;

pub const pollfd_fd_offset: u64 = 0;
pub const pollfd_events_offset: u64 = 8;
pub const pollfd_revents_offset: u64 = 16;
pub const pollfd_size: u64 = 24;
pub const max_pollfds: u64 = 64;
pub const wait_forever: u64 = ~@as(u64, 0);

pub const iovec_base_offset: u64 = 0;
pub const iovec_len_offset: u64 = 8;
pub const iovec_size: u64 = 16;
pub const max_iovecs: u64 = 16;

pub const pipe_pair_read_fd_offset: u64 = 0;
pub const pipe_pair_write_fd_offset: u64 = 8;
pub const pipe_pair_size: u64 = 16;
pub const pipe_buffer_bytes: u64 = 4096;
pub const pipe_buf: u64 = 4096;

pub const timerfd_clock_monotonic: u64 = 1;
pub const timerfd_flag_abstime: u64 = 1 << 0;
pub const timerfd_known_flags_mask: u64 = timerfd_flag_abstime;
pub const timerfd_spec_interval_sec_offset: u64 = 0;
pub const timerfd_spec_interval_nsec_offset: u64 = 8;
pub const timerfd_spec_value_sec_offset: u64 = 16;
pub const timerfd_spec_value_nsec_offset: u64 = 24;
pub const timerfd_spec_size: u64 = 32;
pub const timerfd_read_size: u64 = 8;
pub const eventfd_read_size: u64 = 8;
pub const eventfd_write_size: u64 = 8;

pub fn isFdSyscall(nr: u64) bool {
    return nr >= syscall_fd_first and nr <= syscall_fd_last;
}
