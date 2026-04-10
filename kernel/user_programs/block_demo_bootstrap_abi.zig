pub const config_magic: u64 = 0x4244_4346; // "BDCF"
pub const config_version: u64 = 1;
pub const process_slot_index: usize = 2;

pub fn writeConfigPage(base_va: u64, process_slot: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }
    words[0] = config_magic;
    words[1] = config_version;
    words[process_slot_index] = process_slot;
}

pub fn writeProcessSlot(base_va: u64, process_slot: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    words[process_slot_index] = process_slot;
}
