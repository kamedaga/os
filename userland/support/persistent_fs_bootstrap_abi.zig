pub const config_magic: u64 = 0x5046_5343; // "PFSC"
pub const config_version: u64 = 2;
pub const endpoint_id_index: usize = 2;
pub const admin_token_index: usize = 3;
pub const fs_start_block_index: usize = 4;
pub const server_status_index: usize = 5;
pub const server_status_ready: u64 = 0x4653_5244_59; // "FSRDY"

pub fn writeConfigPage(base_va: u64, endpoint_id: u64, fs_start_block: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }
    words[0] = config_magic;
    words[1] = config_version;
    words[endpoint_id_index] = endpoint_id;
    words[fs_start_block_index] = fs_start_block;
}

pub fn writeServerStatus(base_va: u64, status: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[server_status_index] = status;
}
