const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x5845_434C_4452_3031; // "XECLDR01"
pub const version: u64 = 2;
pub const target_va: u64 = process_abi.standard_config_target_va;
pub const max_argv: usize = 8;
pub const max_envp: usize = 16;
pub const max_arg_data_bytes: usize = 2048;

pub const Config = extern struct {
    magic: u64 = magic,
    version: u64 = version,
    executable_vm_token: u64 = 0,
    executable_file_bytes: u64 = 0,
    flags: u64 = 0,
    interpreter_vm_token: u64 = 0,
    interpreter_file_bytes: u64 = 0,
    bootfs_vm_token: u64 = 0,
    bootfs_file_bytes: u64 = 0,
    fs_endpoint_id: u64 = 0,
    fs_compat_process_slot: u64 = 0,
    abi_trap_endpoint_id: u64 = 0,
    abi_trap_endpoint_process_slot: u64 = 0,
    abi_trap_flavor: u64 = 0,
    abi_trap_request_page_va: u64 = 0,
    execfn_offset: u16 = 0,
    execfn_bytes: u16 = 0,
    argv_count: u16 = 0,
    envp_count: u16 = 0,
    arg_data_bytes: u16 = 0,
    reserved_arg0: u16 = 0,
    argv_offsets: [max_argv]u16 = [_]u16{0} ** max_argv,
    argv_bytes: [max_argv]u16 = [_]u16{0} ** max_argv,
    envp_offsets: [max_envp]u16 = [_]u16{0} ** max_envp,
    envp_bytes: [max_envp]u16 = [_]u16{0} ** max_envp,
    arg_data: [max_arg_data_bytes]u8 = [_]u8{0} ** max_arg_data_bytes,
};
