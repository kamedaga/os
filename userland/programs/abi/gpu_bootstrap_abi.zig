pub const config_magic: u64 = 0x4750_5547; // "GPUG"
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
pub const iommu_token_index: usize = 12;
pub const control_queue_submit_token_index: usize = 13;
pub const control_queue_notify_token_index: usize = 14;
pub const cursor_queue_submit_token_index: usize = 15;
pub const cursor_queue_notify_token_index: usize = 16;
pub const command_token_index: usize = 17;
pub const root_token_index: usize = 18;
pub const device_features_low_index: usize = 19;
pub const device_features_high_index: usize = 20;
pub const driver_status_index: usize = 21;

pub const control_queue_index: u16 = 0;
pub const cursor_queue_index: u16 = 1;

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
    iommu_token: u64 = 0,
    control_queue_submit_token: u64 = 0,
    control_queue_notify_token: u64 = 0,
    cursor_queue_submit_token: u64 = 0,
    cursor_queue_notify_token: u64 = 0,
    command_token: u64 = 0,
    root_token: u64 = 0,
    device_features_low: u64 = 0,
    device_features_high: u64 = 0,
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
    words[iommu_token_index] = descriptor.iommu_token;
    words[control_queue_submit_token_index] = descriptor.control_queue_submit_token;
    words[control_queue_notify_token_index] = descriptor.control_queue_notify_token;
    words[cursor_queue_submit_token_index] = descriptor.cursor_queue_submit_token;
    words[cursor_queue_notify_token_index] = descriptor.cursor_queue_notify_token;
    words[command_token_index] = descriptor.command_token;
    words[root_token_index] = descriptor.root_token;
    words[device_features_low_index] = descriptor.device_features_low;
    words[device_features_high_index] = descriptor.device_features_high;
}

pub fn writeGrantedControlQueueTokens(base_va: u64, submit_token: u64, notify_token: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[control_queue_submit_token_index] = submit_token;
    words[control_queue_notify_token_index] = notify_token;
}

pub fn writeGrantedCursorQueueTokens(base_va: u64, submit_token: u64, notify_token: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[cursor_queue_submit_token_index] = submit_token;
    words[cursor_queue_notify_token_index] = notify_token;
}

pub fn writeGrantedCapabilityTokens(
    base_va: u64,
    iommu_token: u64,
    control_submit_token: u64,
    control_notify_token: u64,
    cursor_submit_token: u64,
    cursor_notify_token: u64,
    command_token: u64,
) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[iommu_token_index] = iommu_token;
    words[control_queue_submit_token_index] = control_submit_token;
    words[control_queue_notify_token_index] = control_notify_token;
    words[cursor_queue_submit_token_index] = cursor_submit_token;
    words[cursor_queue_notify_token_index] = cursor_notify_token;
    words[command_token_index] = command_token;
}

pub fn writeGrantedRootToken(base_va: u64, root_token: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[root_token_index] = root_token;
}

pub fn writeDriverStatus(base_va: u64, status: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[driver_status_index] = status;
}
