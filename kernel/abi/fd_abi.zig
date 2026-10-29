/// Internal waits finish before signal delivery; teardown is never deferred.
pub const wait_uninterruptible: u64 = 1 << 1;
// A thread-generation-owned subscription set, separate from each wait token.
// REPLACE snapshots and atomically replaces the set; REUSE only supplies this
// invocation's output array (same count); CLEAR requires zero array/count/time.
// No output VA is retained in idle subscriptions. Source descriptor mutation
// invalidates the set; callers must REPLACE before reuse. One-shot waits do not
// alter it. Thread/process teardown and CLEAR release all observer storage.
pub const wait_set_replace: u64 = 1 << 2;
pub const wait_set_reuse: u64 = 1 << 3;
pub const wait_set_clear: u64 = 1 << 4;
pub const wait_set_invalidated: u64 = 0 -% @as(u64, 7);

pub const fd_table_entries: u32 = 256;
// FD_TABLE(minimum_capacity): zero queries, otherwise grow without shrinking.
// Returns capacity, FD-number capacity (excluding reserved top u32 values),
// and free slots from first_dynamic_fd as three u64s.
pub const fd_table_info_size: u64 = 24;
pub const first_dynamic_fd: u32 = 16;
// FD_DUP only: mint an observation/notification capability for the calling thread's
// current generation, including the initial thread created by PROCESS_CLONE.
pub const thread_self_fd: u32 = 0xffff_fffe;

pub const syscall_fd_first: u64 = 42;
pub const syscall_fd_table: u64 = 42;
pub const syscall_fd_close: u64 = 43;
pub const syscall_fd_dup: u64 = 44;
pub const syscall_fd_get_info: u64 = 45;
pub const syscall_fd_set_flags: u64 = 46;
pub const syscall_fd_read: u64 = 47;
pub const syscall_fd_write: u64 = 48;
pub const syscall_fd_readv: u64 = 49;
pub const syscall_fd_writev: u64 = 50;
pub const syscall_fd_fcntl: u64 = 51;
pub const syscall_fd_poll: u64 = 52;
pub const syscall_fd_wait_many: u64 = 53;
pub const syscall_fd_ioctl: u64 = 54;
pub const syscall_fd_stat: u64 = 55;
pub const syscall_eventfd_create: u64 = 56;
pub const syscall_pipe_create: u64 = 57;
pub const syscall_timerfd_create: u64 = 58;
pub const syscall_timerfd_settime: u64 = 59;
pub const syscall_timerfd_gettime: u64 = 60;
pub const syscall_vmo_create: u64 = 61;
// VMO_GROW(fd, page-aligned capacity): append zeroed backing without replacing
// live aliases. Requires RESIZE; shrinking and page-view growth are rejected.
pub const syscall_vmo_grow: u64 = 62;
// Changes the physical cache type of populated canonical RAM, including
// aliases held by other processes. Requires an explicit cache-policy right.
pub const syscall_vmo_cache_policy: u64 = 63;
pub const vmo_cache_wb: u64 = 0;
pub const vmo_cache_uc_minus: u64 = 1;
pub const vmo_cache_uc: u64 = 2;
pub const vmo_cache_wc: u64 = 3;
pub const syscall_vmo_create_page_view: u64 = 64;
// Fixed-length borrowed backing replacement; requires both the revocation
// capability and the same parent's SHARE + original maximum MAP authority.
pub const syscall_vmo_refresh_page_view: u64 = 65;
// VMO_COLLECT_DIRTY(view_fd, bitmap, word_count, flags=0). REVOKE controls
// destructive acknowledgement of CPU-write evidence. Bits use logical view
// page indices. Exact word count required; failed copyout retains all bits.
// Output must not physically alias any page of the collected view. DMA writes
// are outside this CPU-only contract; the DMA owner keeps its own protocol.
pub const syscall_vmo_collect_dirty: u64 = 66;
pub const syscall_vmo_revoke: u64 = 67;
pub const syscall_fd_last: u64 = syscall_vmo_revoke;
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

// CREATE only, not an FD flag: capacity is zero-filled on first access rather
// than physically reserved at creation. Allocation failure may occur at fault.
pub const vmo_create_zero_on_demand: u64 = 1 << 4;
// VMO_CREATE(size=RDI, rights=RSI, flags=RDX) with this flag creates an
// unpopulated pager VMO. A zero rights mask grants attach/map-read/map-write
// in addition to the common control rights; EXEC requires explicit authority.
pub const vmo_create_pager: u64 = 1 << 5;
pub const vmo_create_known_flags_mask: u64 = known_flags_mask | vmo_create_zero_on_demand | vmo_create_pager;

pub const right_inspect: u64 = 1 << 0;
pub const right_dup: u64 = 1 << 1;
pub const right_transfer: u64 = 1 << 2;
pub const right_wait: u64 = 1 << 3;
pub const right_poll: u64 = 1 << 4;
pub const right_set_flags: u64 = 1 << 5;
pub const right_close: u64 = 1 << 6;
pub const right_process_signal: u64 = 1 << 27;
pub const thread_self_rights_mask: u64 =
    right_inspect | right_wait | right_poll | right_transfer | right_close | right_process_signal;
pub const right_controller_write: u64 = 1 << 31;
pub const right_read: u64 = 1 << 44;
pub const right_write: u64 = 1 << 45;
pub const right_revoke: u64 = 1 << 46;
pub const right_map_read: u64 = 1 << 13;
pub const right_map_write: u64 = 1 << 14;
pub const right_map_exec: u64 = 1 << 15;
pub const right_resize: u64 = 1 << 16;
pub const right_cache_policy: u64 = 1 << 17;
pub const right_share: u64 = 1 << 18;
pub const right_pager_attach: u64 = 1 << 19;
pub const right_pager_fault: u64 = 1 << 20;
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
    right_cache_policy |
    right_share |
    right_pager_attach |
    right_pager_fault |
    right_revoke;

// VMO_CREATE_PAGE_VIEW(parent_fd, page_indices, page_count, rights, flags)
// takes an array of parent-VMO page indices. A view may only map or transport
// the borrowed pages; it cannot read/write through FD offsets, resize,
// share again, or act as a pager.
pub const vmo_page_view_index_size: u64 = 8;
// Explicit holes are unavailable borrowed backing, never anonymous zero-fill.
pub const vmo_page_view_sparse: u64 = 1 << 4;
pub const vmo_page_view_hole: u64 = 0xffff_ffff_ffff_ffff;
pub const vmo_page_view_known_flags_mask: u64 = known_flags_mask | vmo_page_view_sparse;
pub const vmo_page_view_rights_mask: u64 =
    right_inspect |
    right_dup |
    right_transfer |
    right_close |
    right_map_read |
    right_map_write |
    right_map_exec |
    right_revoke;

pub const fd_kind_none: u64 = 0;
pub const fd_kind_process: u64 = 1;
pub const fd_kind_thread: u64 = 2;
pub const fd_kind_event: u64 = 3;
pub const fd_kind_vmo: u64 = 4;
pub const fd_kind_endpoint: u64 = 5;
pub const fd_kind_channel: u64 = 6;
pub const fd_kind_reply: u64 = 7;
pub const fd_kind_device: u64 = 8;
pub const fd_kind_platform_device: u64 = 9;
pub const fd_kind_mmio_region: u64 = 10;
pub const fd_kind_dma_buffer: u64 = 11;
pub const fd_kind_dma_mapping: u64 = 12;
pub const fd_kind_irq: u64 = 13;
pub const fd_kind_timer: u64 = 14;
pub const fd_kind_serial: u64 = 15;
pub const fd_kind_pipe: u64 = 16;
pub const fd_kind_pager_port: u64 = 17;

pub const fd_info_kind_offset: u64 = 0;
pub const fd_info_rights_offset: u64 = 8;
pub const fd_info_flags_offset: u64 = 16;
pub const fd_info_size_offset: u64 = 24;
pub const fd_info_extra_offset: u64 = 32;
pub const fd_info_size: u64 = 40;

// VMO + INSPECT: a consistent lifetime snapshot, not a lease or a revoke.
// Low u32: kernel-object references (FDs and queued IPC transfers).
// High u32: native-VMO references (object, VMAs and derived backing views).
// Zero means unavailable. Owners must serialize their own new references
// before using an exclusive snapshot to release an ordinary cache reference.
pub const vmo_info_object_refs_mask: u64 = 0xffff_ffff;
pub const vmo_info_native_refs_shift: u6 = 32;

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
pub const pollfd_min_write_bytes_offset: u64 = 16;
pub const pollfd_revents_offset: u64 = 24;
pub const pollfd_size: u64 = 32;
// POLL/WAIT_MANY: R10=index, R8=revents when RAX=1; no copyout.
// R8=0 on failure/non-singleton (INVALID also has numeric value 1).
// Otherwise only ready entries are copied out. User wrappers clear revents.
// One FD_POLL request snapshot, independent of descriptor table capacity.
pub const max_pollfds: u64 = 256;
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
