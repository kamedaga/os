const abi_root = @import("kernel_abi_root");
const process_abi = abi_root.process_abi;
const kernel_vm = @import("../memory/kernel_vm.zig");
const x86_platform = @import("../arch/x86_64/platform.zig");

pub const four_gib: u64 = 4 * 1024 * 1024 * 1024;
pub const one_tib: u64 = 1024 * 1024 * 1024 * 1024;
pub const two_mib: u64 = x86_platform.two_mib;
pub const page_entries: usize = x86_platform.page_entries;

pub const user_va: u64 = 0x20000000;
pub const user_low_va: u64 = 0x00400000;
pub const user_elf_base_va: u64 = user_va;
pub const user_stack_top: u64 = 0x3C00_0000;
pub const user_stack_page_va: u64 = user_stack_top - 0x1000;
pub const user_entry_rsp: u64 = user_stack_top - 8;
pub const user_aux_base_va: u64 = process_abi.aux_base_va;
pub const user_aux_reserved_va: u64 = user_aux_base_va;
pub const user_top_va: u64 = 0x0000_8000_0000_0000;
pub const dynamic_map_base_va: u64 = user_va + 0x0300_0000;
pub const dynamic_map_end_va: u64 = user_aux_reserved_va;
pub const user_program_max_load_bytes: usize = @intCast(user_aux_reserved_va - user_va);

pub const boot_log_console_stack_page_va: u64 = process_abi.auxPageVa(0);
pub const boot_log_console_stack_top: u64 = boot_log_console_stack_page_va + 0x1000;
pub const boot_log_console_entry_rsp: u64 = boot_log_console_stack_top - 8;
pub const boot_log_user_va: u64 = process_abi.auxPageVa(1);

pub const framebuffer_window_bytes: u64 =
    two_mib - (process_abi.auxPageVa(5) - user_aux_base_va);

pub const phys_copy_window_va: u64 = kernel_vm.phys_copy_window_va;
