pub const spawn_flag_bootstrap_page_writable: u64 = 1 << 0;

pub const syscall_process_first: u64 = 2;
pub const syscall_process_create: u64 = 2;
// PROCESS_CREATE: rdi=process flags, rsi=process handle rights,
// rdx=handle flags, r10=grant array, r8=count. No ambient FD inheritance.
pub const process_create_max_grants: usize = 64;
pub const ProcessFdGrant = extern struct {
    source_fd: u64,
    target_fd: u64,
    rights: u64,
    flags: u64,
};
pub const syscall_process_kill: u64 = 3;
pub const syscall_process_wait: u64 = 4;
pub const syscall_process_exit: u64 = 5;
pub const syscall_thread_create: u64 = 6;
pub const syscall_thread_start: u64 = 7;
// THREAD_CONTEXT: rdi=thread capability, rsi=GET/SET, rdx=context, r10=size.
// Only a suspended, CPU-quiescent thread without an active wait/fault qualifies.
pub const syscall_thread_context: u64 = 8;
pub const syscall_thread_kill: u64 = 9;
pub const syscall_thread_wait: u64 = 10;
pub const syscall_thread_exit: u64 = 11;
pub const syscall_thread_signal: u64 = 12;
pub const syscall_process_signal: u64 = 13;
pub const syscall_process_signal_ctl: u64 = 14;
pub const syscall_process_stop: u64 = 15;
pub const syscall_process_continue: u64 = 16;
pub const syscall_thread_set_fs_base: u64 = 17;
pub const syscall_thread_set_gs_base: u64 = 18;
pub const syscall_process_clone: u64 = 19;
pub const syscall_process_map: u64 = 20;
pub const syscall_process_map_batch: u64 = 21;
// UNMAP: rdi=process MAP_INTO capability, rsi=VA, rdx=length, r10=0.
// Synchronous range invalidation, including every CPU's cached translations.
pub const syscall_process_unmap: u64 = 22;
pub const syscall_process_exec_from: u64 = 23;
pub const syscall_process_memory_barrier: u64 = 24;
pub const syscall_process_last: u64 = syscall_process_memory_barrier;
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
pub const thread_context_get: u64 = 0;
pub const thread_context_set: u64 = 1;
pub const ThreadRegisters = extern struct {
    r15: u64, r14: u64, r13: u64, r12: u64, r11: u64, r10: u64, r9: u64, r8: u64,
    rbp: u64, rdi: u64, rsi: u64, rdx: u64, rcx: u64, rbx: u64, rax: u64,
    rip: u64, cs: u64, rflags: u64, rsp: u64, ss: u64,
};
// Native architectural state, not a Linux ptrace or syscall frame. XSAVE uses
// the same validated standard x87/SSE/AVX format as native notification return.
pub const ThreadUserContext = extern struct {
    size: u64,
    xstate_features: u64,
    fs_base: u64,
    gs_base: u64,
    pkru: u64,
    reserved: [3]u64,
    registers: ThreadRegisters,
    xstate: [832]u8,
};
pub const thread_user_context_size: u64 = @sizeOf(ThreadUserContext);
comptime {
    if (@sizeOf(ThreadRegisters) != 160 or @sizeOf(ThreadUserContext) != 1056 or
        @offsetOf(ThreadUserContext, "registers") != 64 or @offsetOf(ThreadUserContext, "xstate") != 224)
        @compileError("native thread context ABI layout mismatch");
}
pub const thread_known_flags_mask: u64 = thread_flag_none;
pub const process_self_fd: u32 = 0xffff_ffff;
pub const thread_exit_flag_clear_tid: u64 = 1 << 0;
pub const thread_exit_known_flags_mask: u64 = thread_exit_flag_clear_tid;

pub const status_word_state_offset: u64 = 0;
pub const status_word_exit_code_offset: u64 = 8;
pub const status_word_id_offset: u64 = 16;
pub const status_word_generation_offset: u64 = 24;
pub const status_size: u64 = 32;

pub const state_active: u64 = 1;
pub const state_exited: u64 = 2;
pub const state_killed: u64 = 3;
pub const state_stopped: u64 = 4;
pub const state_continued: u64 = 5;

pub const signal_max: u32 = 64;
pub const signal_kill: u32 = 9;
pub const signal_ctl_register: u64 = 1;
pub const signal_ctl_set_mask: u64 = 2;
pub const signal_ctl_return: u64 = 3;
pub const signal_ctl_deliver_pending_frame: u64 = 4;
pub const signal_ctl_get_timer: u64 = 5;
pub const signal_ctl_set_timer: u64 = 6;
pub const signal_ctl_set_pending_hint: u64 = 7;
// Current thread only: rsi=entry, rdx=dedicated stack base, r10=stack size.
// All zero unregisters. Synchronous CPU faults are not maskable signals.
pub const signal_ctl_register_fault: u64 = 8;
pub const signal_timer_state_signo_offset: u64 = 0;
pub const signal_timer_state_remaining_ticks_offset: u64 = 8;
pub const signal_timer_state_interval_ticks_offset: u64 = 16;
pub const signal_timer_state_size: u64 = 24;
pub const signal_frame_magic: u64 = 0x5041_4348_4153_4947;
pub const signal_frame_header_size: u64 = 32;
pub const signal_frame_context_offset: u64 = signal_frame_header_size;
pub const signal_xstate_feature_mask: u64 = 0x7;
pub const signal_xstate_size: u64 = 832;
pub const signal_frame_xstate_offset: u64 = signal_frame_context_offset + 20 * 8;
pub const signal_frame_size: u64 = signal_frame_xstate_offset + signal_xstate_size;
// A synchronous fault uses the same context/XState prefix and RETURN, signo=0,
// followed by raw architecture exception vector, error code and fault address.
pub const fault_frame_size: u64 = signal_frame_size + 3 * 8;
pub const signal_red_zone_size: u64 = 128;
pub const signal_runtime_stack_size: u64 = 4096;

pub const process_map_flag_none: u64 = 0;
pub const process_map_flag_private: u64 = 1 << 0;
pub const process_map_flag_shared: u64 = 1 << 1;
// A private, demand-zero mapping authorized by the process MAP_INTO right.
// There is no backing FD: vmo_fd and vmo_offset must both be zero.
pub const process_map_flag_anonymous: u64 = 1 << 2;
// Failure-atomic replacement at an explicit address. Not valid with ANYWHERE
// or PROCESS_MAP_BATCH. Source authority remains in the calling process.
pub const process_map_flag_replace: u64 = 1 << 3;
pub const process_map_known_flags_mask: u64 = process_map_flag_private | process_map_flag_shared | process_map_flag_anonymous | process_map_flag_replace;
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

pub const process_exec_from_flag_none: u64 = 0;
pub const process_exec_from_known_flags_mask: u64 = process_exec_from_flag_none;

pub const process_memory_barrier_flag_none: u64 = 0;
pub const process_memory_barrier_known_flags_mask: u64 = process_memory_barrier_flag_none;

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
