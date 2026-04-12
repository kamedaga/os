pub const config_magic: u64 = 0x4246_5343; // "BFSC"
pub const config_version: u64 = 1;
pub const endpoint_id_index: usize = 2;
pub const admin_token_index: usize = 3;
pub const rootfs_start_block_index: usize = 4;
pub const common_page_paddr_index: usize = 5;
pub const notify_page_paddr_index: usize = 6;
pub const isr_page_paddr_index: usize = 7;
pub const device_page_paddr_index: usize = 8;
pub const common_page_offset_index: usize = 9;
pub const notify_page_offset_index: usize = 10;
pub const isr_page_offset_index: usize = 11;
pub const device_page_offset_index: usize = 12;
pub const notify_off_multiplier_index: usize = 13;
pub const queue_submit_token_index: usize = 14;
pub const queue_notify_token_index: usize = 15;
pub const capacity_sectors_index: usize = 16;
pub const logical_block_size_index: usize = 17;

pub const BlockConfig = struct {
    rootfs_start_block: u64,
    common_page_paddr: u64,
    notify_page_paddr: u64,
    isr_page_paddr: u64,
    device_page_paddr: u64,
    common_page_offset: u64,
    notify_page_offset: u64,
    isr_page_offset: u64,
    device_page_offset: u64,
    notify_off_multiplier: u64,
    queue_submit_token: u64 = 0,
    queue_notify_token: u64 = 0,
    capacity_sectors: u64 = 0,
    logical_block_size: u64 = 0,
};

pub fn writeConfigPage(
    base_va: u64,
    endpoint_id: u64,
    block: BlockConfig,
) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }
    words[0] = config_magic;
    words[1] = config_version;
    words[endpoint_id_index] = endpoint_id;
    words[rootfs_start_block_index] = block.rootfs_start_block;
    words[common_page_paddr_index] = block.common_page_paddr;
    words[notify_page_paddr_index] = block.notify_page_paddr;
    words[isr_page_paddr_index] = block.isr_page_paddr;
    words[device_page_paddr_index] = block.device_page_paddr;
    words[common_page_offset_index] = block.common_page_offset;
    words[notify_page_offset_index] = block.notify_page_offset;
    words[isr_page_offset_index] = block.isr_page_offset;
    words[device_page_offset_index] = block.device_page_offset;
    words[notify_off_multiplier_index] = block.notify_off_multiplier;
    words[queue_submit_token_index] = block.queue_submit_token;
    words[queue_notify_token_index] = block.queue_notify_token;
    words[capacity_sectors_index] = block.capacity_sectors;
    words[logical_block_size_index] = block.logical_block_size;
}

pub fn writeGrantedQueueTokens(base_va: u64, submit_token: u64, notify_token: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[queue_submit_token_index] = submit_token;
    words[queue_notify_token_index] = notify_token;
}
