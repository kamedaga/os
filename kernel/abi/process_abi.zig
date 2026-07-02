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
pub const syscall_process_clone: u64 = 13;
pub const syscall_process_map: u64 = 14;
pub const syscall_process_map_batch: u64 = 15;
pub const syscall_process_last: u64 = syscall_process_map_batch;
pub const syscall_process_count: u64 = syscall_process_last - syscall_process_first + 1;

pub const process_flag_none: u64 = 0;
pub const process_known_flags_mask: u64 = process_flag_none;

pub const process_clone_flag_none: u64 = 0;
pub const process_clone_flag_current_thread: u64 = 1 << 0;
pub const process_clone_flag_user_frame: u64 = 1 << 1;
pub const process_clone_known_flags_mask: u64 = process_clone_flag_current_thread | process_clone_flag_user_frame;

pub const process_clone_user_frame_size: u64 = 18 * 8;
pub const process_clone_user_frame_r15_offset: u64 = 0 * 8;
pub const process_clone_user_frame_r14_offset: u64 = 1 * 8;
pub const process_clone_user_frame_r13_offset: u64 = 2 * 8;
pub const process_clone_user_frame_r12_offset: u64 = 3 * 8;
pub const process_clone_user_frame_rbp_offset: u64 = 4 * 8;
pub const process_clone_user_frame_rbx_offset: u64 = 5 * 8;
pub const process_clone_user_frame_r11_offset: u64 = 6 * 8;
pub const process_clone_user_frame_r10_offset: u64 = 7 * 8;
pub const process_clone_user_frame_r9_offset: u64 = 8 * 8;
pub const process_clone_user_frame_r8_offset: u64 = 9 * 8;
pub const process_clone_user_frame_rdi_offset: u64 = 10 * 8;
pub const process_clone_user_frame_rsi_offset: u64 = 11 * 8;
pub const process_clone_user_frame_rdx_offset: u64 = 12 * 8;
pub const process_clone_user_frame_rcx_offset: u64 = 13 * 8;
pub const process_clone_user_frame_rax_offset: u64 = 14 * 8;
pub const process_clone_user_frame_rip_offset: u64 = 15 * 8;
pub const process_clone_user_frame_rsp_offset: u64 = 16 * 8;
pub const process_clone_user_frame_rflags_offset: u64 = 17 * 8;

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
pub const process_map_flag_private: u64 = 1 << 0;
pub const process_map_flag_shared: u64 = 1 << 1;
pub const process_map_known_flags_mask: u64 = process_map_flag_private | process_map_flag_shared;
pub const process_map_offset_low_bits: u64 = 0xfff;
pub const process_map_anywhere_va: u64 = 0xffff_ffff_ffff_ffff;
pub const process_map_batch_max_entries: u64 = 32;
pub const process_map_batch_entry_vmo_fd_offset: u64 = 0;
pub const process_map_batch_entry_target_va_offset: u64 = 8;
pub const process_map_batch_entry_size_offset: u64 = 16;
pub const process_map_batch_entry_prot_offset: u64 = 24;
pub const process_map_batch_entry_vmo_offset_offset: u64 = 32;
pub const process_map_batch_entry_flags_offset: u64 = 40;
pub const process_map_batch_entry_size: u64 = 48;

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
