pub const enable_framebuffer_server_step1 = true;
pub const enable_boot_log_console_process = true;
pub const enable_vfs_process = true;
pub const enable_init_process = true;
pub const enable_virtio_input_mouse = true;
pub const enable_virtio_input_keyboard = true;
pub const enable_bootlog_wait_for_enter = false;
pub const enable_title_only_ready_logs = true;
pub const enable_cap_table_dump_logs = false;
pub const enable_scheduler_perf_logs = false;
pub const suppress_compositor_perf_user_logs = true;
pub const enable_iommu_no_cap_driver = true;
pub const enforce_iommu_no_cap_driver = false;
pub const boot_display_process_index: usize = 1;

pub const user_entry_rflags: u64 = 0x202;
pub const reserved_low_mem_end: u64 = 64 * 1024 * 1024;
pub const canonical_user_limit_exclusive: u64 = 0x0000_8000_0000_0000;
