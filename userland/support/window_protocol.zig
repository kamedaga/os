const mouse_shared_abi = @import("mouse_shared_abi.zig");

pub const window_cap_magic: u32 = 0x57434150; // 'WCAP'
pub const window_meta_magic: u32 = 0x574D5441; // 'WMTA'
pub const mouse_shared_magic: u64 = mouse_shared_abi.magic;
pub const taskbar_state_magic: u32 = 0x54425354; // "TBST"
pub const taskbar_command_magic: u32 = 0x5442434D; // "TBCM"
pub const window_protocol_version: u16 = 2;
pub const taskbar_protocol_version: u16 = 1;
pub const window_title_max_bytes: usize = 64;
pub const taskbar_entry_max: usize = 5;
pub const window_flag_allow_pixels_dma: u32 = 1 << 0;
pub const window_flag_low_scale: u32 = 1 << 1;
pub const window_flag_frameless: u32 = 1 << 2;
pub const taskbar_entry_flag_visible: u32 = 1 << 0;
pub const taskbar_command_none: u16 = 0;
pub const taskbar_command_activate: u16 = 1;

pub const WindowRights = packed struct(u16) {
    read_meta: bool = false,
    write_meta: bool = false,
    write_pixels: bool = false,
    control: bool = false,
    dma_pixels: bool = false,
    _reserved: u11 = 0,
};

pub fn decodeWindowRights(bits: u16) WindowRights {
    return @bitCast(bits);
}

pub const WindowCap = packed struct {
    magic: u32,
    version: u16,
    rights_bits: u16,
    window_id: u32,
    owner_pid: u32,
    pixels_cap_paddr: u64,
    meta_cap_paddr: u64,
    pixels_size_bytes: u32,
    pixels_page_count: u16,
    pixels_per_scan_line: u16,
    pixel_format: u32,
    evt_cap_paddr: u64,
    width: u16,
    height: u16,
    min_width: u16,
    min_height: u16,
    flags: u32,
    z_hint: i32,
    reserved0: u32,
};

pub const WindowMeta = extern struct {
    magic: u32,
    version: u16,
    state: u16,
    seq: u64,
    pos_x: i32,
    pos_y: i32,
    width: u16,
    height: u16,
    dirty_x: u16,
    dirty_y: u16,
    dirty_w: u16,
    dirty_h: u16,
    title_len: u16,
    title: [window_title_max_bytes]u8,
};

pub const mouse_shared_page_bytes: usize = mouse_shared_abi.page_bytes;
pub const mouse_shared_header_bytes: usize = mouse_shared_abi.header_bytes;
pub const mouse_shared_log_offset_bytes: usize = mouse_shared_abi.log_offset_bytes;
pub const mouse_shared_log_capacity_bytes: usize = mouse_shared_abi.log_capacity_bytes;
pub const MouseSharedPage = mouse_shared_abi.MouseSharedPage;

pub const TaskbarEntry = extern struct {
    window_id: u32,
    flags: u32,
    title_len: u16,
    reserved0: u16 = 0,
    title: [window_title_max_bytes]u8,
};

pub const TaskbarStatePage = extern struct {
    magic: u32,
    version: u16,
    entry_count: u16,
    seq: u64,
    entries: [taskbar_entry_max]TaskbarEntry,
};

pub const TaskbarCommandPage = extern struct {
    magic: u32,
    version: u16,
    command: u16,
    seq: u64,
    window_id: u32,
    reserved0: u32 = 0,
};
