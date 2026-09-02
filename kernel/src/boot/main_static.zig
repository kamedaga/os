const kernel = @import("../kernel.zig");
const address_space = @import("../memory/address_space.zig");
const pmm = @import("../memory/pmm.zig");
const scheduler = @import("../scheduler.zig").connection;
const syscall_numbers = @import("../syscall/numbers.zig");
const x86_platform = @import("../arch/x86_64/platform.zig");
const boot_layout = @import("boot_layout.zig");

pub const four_gib = boot_layout.four_gib;
pub const page_entries = boot_layout.page_entries;
pub const user_va = boot_layout.user_va;
pub const user_low_va = boot_layout.user_low_va;
pub const user_top_va = boot_layout.user_top_va;
pub const dynamic_map_base_va = boot_layout.dynamic_map_base_va;
pub const dynamic_map_end_va = boot_layout.dynamic_map_end_va;
pub const user_elf_base_va = boot_layout.user_elf_base_va;
pub const user_stack_top = boot_layout.user_stack_top;
pub const initial_user_stack_pages = boot_layout.initial_user_stack_pages;
pub const initial_user_stack_bytes = boot_layout.initial_user_stack_bytes;
pub const user_stack_base_va = boot_layout.user_stack_base_va;
pub const user_stack_page_va = boot_layout.user_stack_page_va;
pub const user_entry_rsp = boot_layout.user_entry_rsp;
pub const user_program_max_load_bytes = boot_layout.user_program_max_load_bytes;
pub const boot_log_console_stack_page_va = boot_layout.boot_log_console_stack_page_va;
pub const boot_log_console_stack_top = boot_layout.boot_log_console_stack_top;
pub const boot_log_console_entry_rsp = boot_layout.boot_log_console_entry_rsp;
pub const boot_log_user_va = boot_layout.boot_log_user_va;
pub const framebuffer_window_bytes = boot_layout.framebuffer_window_bytes;
pub const phys_copy_window_va = boot_layout.phys_copy_window_va;

pub const user_entry_rflags: u64 = 0x202;
pub const reserved_low_mem_end: u64 = 64 * 1024 * 1024;
pub const canonical_user_limit_exclusive: u64 = 0x0000_8000_0000_0000;

pub const user_unmapped_test_va: u64 = 0x20100000;
pub const user_dma_verify_va: u64 = 0x20110000;
pub const user_recovery_stop_va: u64 = 0x20200000;
pub const page_addr_mask: u64 = 0x000F_FFFF_FFFF_F000;
pub const physical_map_limit_exclusive: u64 = page_addr_mask + 0x1000;
pub const gdt_kernel_code_selector: u16 = x86_platform.gdt_kernel_code_selector;
pub const gdt_kernel_data_selector: u16 = x86_platform.gdt_kernel_data_selector;
pub const gdt_user_code_selector: u16 = x86_platform.gdt_user_code_selector;
pub const gdt_user_data_selector: u16 = x86_platform.gdt_user_data_selector;
pub const gdt_tss_selector: u16 = x86_platform.gdt_tss_selector;

pub const page_present: u64 = x86_platform.page_present;
pub const page_rw: u64 = x86_platform.page_rw;
pub const page_user: u64 = x86_platform.page_user;
pub const page_ps: u64 = x86_platform.page_ps;
pub const page_nx: u64 = x86_platform.page_nx;
pub const lapic_timer_vector: u8 = 0x40;
pub const scheduler_wake_ipi_vector: u8 = 0x42;
pub const lapic_timer_initial_count: u32 = 50_000;
// Midpoint of the measured 25.959-48.361 us one-shot interrupt/rearm cost.
pub const lapic_timer_rearm_overhead_ns: u64 = 37_160;
pub const scheduler_slice_ticks: u64 = 4;
pub const bootlog_auto_launch_min_visible_ticks: u64 = 1;
pub const user_log_max_bytes: usize = 256;
pub const boot_log_max_bytes: usize = 32 * 1024;
pub const boot_log_page_header_bytes: usize = 8;
pub const boot_log_page_payload_bytes: usize = 4096 - boot_log_page_header_bytes;
pub const boot_log_status_offset: usize = 4;

pub const debug_skip_exit_boot_services = false;
pub const debug_skip_cr3_switch = false;
pub const debug_trigger_page_fault_test = false;
pub const user_process_count: usize = kernel.process_count;
pub const user_thread_count: usize = scheduler.maxThreadSlots;
pub const UserAddressSpace = address_space.UserAddressSpace;
pub const UserAddressSpaceTable = address_space.UserAddressSpaceTable;

pub const syscall_ok: u64 = syscall_numbers.syscall_ok;
pub const syscall_err_invalid = syscall_numbers.syscall_err_invalid;
pub const syscall_err_not_ready = syscall_numbers.syscall_err_not_ready;
pub const syscall_err_alloc = syscall_numbers.syscall_err_alloc;
pub const syscall_err_map = syscall_numbers.syscall_err_map;
pub const syscall_err_empty = syscall_numbers.syscall_err_empty;

pub const MemoryStats = pmm.MemoryStats;
