pub const spawn_flag_bootstrap_page_writable: u64 = 1 << 0;

pub const syscall_process_first: u64 = 2;
pub const syscall_process_create: u64 = 2;
pub const syscall_process_kill: u64 = 3;
pub const syscall_process_wait: u64 = 4;
pub const syscall_process_exit: u64 = 5;
pub const syscall_thread_create: u64 = 6;
pub const syscall_thread_start: u64 = 7;
pub const syscall_thread_kill: u64 = 8;
pub const syscall_thread_wait: u64 = 9;
pub const syscall_thread_exit: u64 = 10;
pub const syscall_thread_set_fs_base: u64 = 11;
pub const syscall_thread_set_gs_base: u64 = 12;
pub const syscall_process_map: u64 = 13;
pub const syscall_process_last: u64 = syscall_process_map;
pub const syscall_process_count: u64 = syscall_process_last - syscall_process_first + 1;

pub const process_flag_none: u64 = 0;
pub const process_known_flags_mask: u64 = process_flag_none;

pub const thread_flag_none: u64 = 0;
pub const thread_known_flags_mask: u64 = thread_flag_none;

pub const status_word_state_offset: u64 = 0;
pub const status_word_exit_code_offset: u64 = 8;
pub const status_word_id_offset: u64 = 16;
pub const status_word_generation_offset: u64 = 24;
pub const status_size: u64 = 32;

pub const state_active: u64 = 1;
pub const state_exited: u64 = 2;
pub const state_killed: u64 = 3;

pub const process_map_flag_none: u64 = 0;
pub const process_map_known_flags_mask: u64 = process_map_flag_none;

pub const aux_base_va: u64 = 0x0000_7000_0000_0000;
pub const aux_page_bytes: u64 = 0x1000;
pub const standard_config_target_va: u64 = auxPageVa(2);
pub const default_stack_top_va: u64 = aux_base_va;
pub const default_stack_bytes: u64 = 0x2_0000;
pub const user_aslr_base_va: u64 = 0x0000_0100_0000_0000;
pub const user_aslr_end_va: u64 = 0x0000_0100_4000_0000;
pub const user_aslr_granule: u64 = 0x1_0000;
pub const at_pacha_bootstrap_fd: u64 = 0x7000_0000;

pub fn auxPageVa(page_index: u64) u64 {
    return aux_base_va + page_index * aux_page_bytes;
}

pub fn isProcessSyscall(nr: u64) bool {
    return nr >= syscall_process_first and nr <= syscall_process_last;
}
