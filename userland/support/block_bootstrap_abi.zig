pub const config_magic: u64 = 0x424C_4B43; // "BLKC"
pub const config_version: u64 = 1;

pub const endpoint_id_index: usize = 2;
pub const common_page_paddr_index: usize = 3;
pub const notify_page_paddr_index: usize = 4;
pub const isr_page_paddr_index: usize = 5;
pub const device_page_paddr_index: usize = 6;
pub const common_page_offset_index: usize = 7;
pub const notify_page_offset_index: usize = 8;
pub const isr_page_offset_index: usize = 9;
pub const device_page_offset_index: usize = 10;
pub const notify_off_multiplier_index: usize = 11;
pub const queue_submit_token_index: usize = 12;
pub const queue_notify_token_index: usize = 13;
pub const root_token_index: usize = 14;
pub const capacity_sectors_index: usize = 15;
pub const logical_block_size_index: usize = 16;
pub const driver_status_index: usize = 17;
pub const driver_status_ready: u64 = 0x4452_4459; // "DRDY"

pub const ConfigPageDescriptor = struct {
    endpoint_id: u64,
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
    root_token: u64 = 0,
    capacity_sectors: u64 = 0,
    logical_block_size: u64 = 0,
};

pub fn writeConfigPage(base_va: u64, descriptor: ConfigPageDescriptor) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }

    words[0] = config_magic;
    words[1] = config_version;
    words[endpoint_id_index] = descriptor.endpoint_id;
    words[common_page_paddr_index] = descriptor.common_page_paddr;
    words[notify_page_paddr_index] = descriptor.notify_page_paddr;
    words[isr_page_paddr_index] = descriptor.isr_page_paddr;
    words[device_page_paddr_index] = descriptor.device_page_paddr;
    words[common_page_offset_index] = descriptor.common_page_offset;
    words[notify_page_offset_index] = descriptor.notify_page_offset;
    words[isr_page_offset_index] = descriptor.isr_page_offset;
    words[device_page_offset_index] = descriptor.device_page_offset;
    words[notify_off_multiplier_index] = descriptor.notify_off_multiplier;
    words[queue_submit_token_index] = descriptor.queue_submit_token;
    words[queue_notify_token_index] = descriptor.queue_notify_token;
    words[root_token_index] = descriptor.root_token;
    words[capacity_sectors_index] = descriptor.capacity_sectors;
    words[logical_block_size_index] = descriptor.logical_block_size;
}

pub fn writeGrantedQueueTokens(base_va: u64, submit_token: u64, notify_token: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[queue_submit_token_index] = submit_token;
    words[queue_notify_token_index] = notify_token;
}

pub fn writeGrantedRootToken(base_va: u64, root_token: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[root_token_index] = root_token;
}

pub fn writeDriverStatus(base_va: u64, status: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[driver_status_index] = status;
}
