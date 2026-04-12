pub const config_magic: u64 = 0x5445524D; // "TERM"
pub const config_version: u64 = 1;

pub fn writeConfigPage(base_va: u64, service_registry_va: u64, keyboard_shared_va: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }
    words[0] = config_magic;
    words[1] = config_version;
    words[2] = service_registry_va;
    words[3] = keyboard_shared_va;
}
