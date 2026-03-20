pub const mouse_config_magic: u64 = 0x4D4F5553; // "MOUS"
pub const keyboard_config_magic: u64 = 0x4B455942; // "KEYB"

pub const queue_submit_token_index: usize = 13;
pub const queue_notify_token_index: usize = 14;
pub const shared_target_va_index: usize = 18;

pub const ConfigPageDescriptor = struct {
    common_page_paddr: u64,
    notify_page_paddr: u64,
    isr_page_paddr: u64,
    device_page_paddr: u64,
    common_page_offset: u64,
    notify_page_offset: u64,
    isr_page_offset: u64,
    device_page_offset: u64,
    notify_off_multiplier: u64,
    shared_page_paddr: u64 = 0,
    queue_paddr0: u64 = 0,
    queue_paddr1: u64 = 0,
    queue_submit_token: u64 = 0,
    queue_notify_token: u64 = 0,
    screen_width: u64 = 0,
    screen_height: u64 = 0,
    screen_pitch: u64 = 0,
    shared_target_va: u64 = 0,
};

fn clearWords(base_va: u64) [*]volatile u64 {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }
    return words;
}

pub fn writeMouseConfigPage(base_va: u64, descriptor: ConfigPageDescriptor) void {
    const words = clearWords(base_va);
    words[0] = mouse_config_magic;
    words[1] = descriptor.common_page_paddr;
    words[2] = descriptor.notify_page_paddr;
    words[3] = descriptor.isr_page_paddr;
    words[4] = descriptor.device_page_paddr;
    words[5] = descriptor.common_page_offset;
    words[6] = descriptor.notify_page_offset;
    words[7] = descriptor.isr_page_offset;
    words[8] = descriptor.device_page_offset;
    words[9] = descriptor.notify_off_multiplier;
    words[10] = descriptor.shared_page_paddr;
    words[11] = descriptor.queue_paddr0;
    words[12] = descriptor.queue_paddr1;
    words[13] = descriptor.queue_submit_token;
    words[14] = descriptor.queue_notify_token;
    words[15] = descriptor.screen_width;
    words[16] = descriptor.screen_height;
    words[17] = descriptor.screen_pitch;
    words[shared_target_va_index] = descriptor.shared_target_va;
}

pub fn writeKeyboardConfigPage(base_va: u64, descriptor: ConfigPageDescriptor) void {
    const words = clearWords(base_va);
    words[0] = keyboard_config_magic;
    words[1] = descriptor.common_page_paddr;
    words[2] = descriptor.notify_page_paddr;
    words[3] = descriptor.isr_page_paddr;
    words[4] = descriptor.device_page_paddr;
    words[5] = descriptor.common_page_offset;
    words[6] = descriptor.notify_page_offset;
    words[7] = descriptor.isr_page_offset;
    words[8] = descriptor.device_page_offset;
    words[9] = descriptor.notify_off_multiplier;
    words[10] = descriptor.shared_page_paddr;
    words[11] = descriptor.queue_paddr0;
    words[12] = descriptor.queue_paddr1;
    words[13] = descriptor.queue_submit_token;
    words[14] = descriptor.queue_notify_token;
    words[shared_target_va_index] = descriptor.shared_target_va;
}

pub fn writeGrantedQueueTokens(base_va: u64, submit_token: u64, notify_token: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[queue_submit_token_index] = submit_token;
    words[queue_notify_token_index] = notify_token;
}
