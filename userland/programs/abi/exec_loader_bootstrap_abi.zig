const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x5845_434C_4452_3031; // "XECLDR01"
pub const version: u64 = 2;
pub const target_va: u64 = process_abi.standard_config_target_va;
pub const max_argv: usize = 8;
pub const max_envp: usize = 16;
pub const max_arg_data_bytes: usize = 2048;
pub const flag_service_mode: u64 = 1 << 0;
pub const service_endpoint_id: u64 = 0x93;
pub const service_request_magic: u64 = 0x5845_434C_4453_5651; // "XECLDSVQ"
pub const service_response_magic: u64 = 0x5845_434C_4453_5652; // "XECLDSVR"
pub const service_version: u64 = 1;
pub const service_op_launch: u64 = 1;
pub const service_status_ok: u64 = 0;
pub const service_status_invalid: u64 = 1;
pub const service_status_map_failed: u64 = 2;
pub const service_status_launch_failed: u64 = 3;

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

pub const ServiceRequest = extern struct {
    magic: u64 = service_request_magic,
    version: u64 = service_version,
    op: u64 = service_op_launch,
    seq: u64 = 0,
    response_paddr: u64 = 0,
    config: Config = .{},
};

pub const ServiceResponse = extern struct {
    magic: u64 = service_response_magic,
    version: u64 = service_version,
    op: u64 = service_op_launch,
    seq: u64 = 0,
    status: u64 = service_status_invalid,
    child_process_slot: u64 = 0,
};
