const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x5845_434C_4452_3031; // "XECLDR01"
pub const version: u64 = 4;
pub const target_va: u64 = process_abi.standard_config_target_va;

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
    fs_process_handle: u64 = 0,
    abi_trap_endpoint_id: u64 = 0,
    abi_trap_endpoint_target_token: u64 = 0,
    abi_trap_flavor: u64 = 0,
};
