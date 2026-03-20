const protocol = @import("window_protocol.zig");

pub const config_magic: u64 = 0x54424152; // "TBAR"
pub const config_version: u64 = 1;
pub const self_process_slot_index: usize = 4;
pub const service_registry_va_index: usize = 5;
pub const pointer_shared_va_index: usize = 6;
pub const state_page_va_index: usize = 7;
pub const command_page_va_index: usize = 8;

pub fn writeConfigPage(
    base_va: u64,
    screen_width: u64,
    screen_height: u64,
    self_process_slot: u64,
    service_registry_va: u64,
    pointer_shared_va: u64,
    state_page_va: u64,
    command_page_va: u64,
) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }
    words[0] = config_magic;
    words[1] = config_version;
    words[2] = screen_width;
    words[3] = screen_height;
    words[self_process_slot_index] = self_process_slot;
    words[service_registry_va_index] = service_registry_va;
    words[pointer_shared_va_index] = pointer_shared_va;
    words[state_page_va_index] = state_page_va;
    words[command_page_va_index] = command_page_va;
}

pub fn initStatePage(base_va: u64) void {
    const page: *volatile protocol.TaskbarStatePage = @ptrFromInt(base_va);
    page.* = .{
        .magic = protocol.taskbar_state_magic,
        .version = protocol.taskbar_protocol_version,
        .entry_count = 0,
        .seq = 1,
        .entries = [_]protocol.TaskbarEntry{.{
            .window_id = 0,
            .flags = 0,
            .title_len = 0,
            .reserved0 = 0,
            .title = [_]u8{0} ** protocol.window_title_max_bytes,
        }} ** protocol.taskbar_entry_max,
    };
}

pub fn initCommandPage(base_va: u64) void {
    const page: *volatile protocol.TaskbarCommandPage = @ptrFromInt(base_va);
    page.* = .{
        .magic = protocol.taskbar_command_magic,
        .version = protocol.taskbar_protocol_version,
        .command = protocol.taskbar_command_none,
        .seq = 1,
        .window_id = 0,
        .reserved0 = 0,
    };
}
