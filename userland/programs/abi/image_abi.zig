const fd_abi = @import("fd_abi.zig");

pub const exec_vmo_rights: u64 =
    fd_abi.right_inspect |
    fd_abi.right_dup |
    fd_abi.right_transfer |
    fd_abi.right_close |
    fd_abi.right_map_read |
    fd_abi.right_map_write |
    fd_abi.right_map_exec |
    fd_abi.right_share;

pub fn isVmoFd(value: u64) bool {
    return value >= fd_abi.first_dynamic_fd and value < fd_abi.fd_table_entries;
}
