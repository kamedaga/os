pub const window_cap_magic: u32 = 0x57434150; // 'WCAP'
pub const window_meta_magic: u32 = 0x574D5441; // 'WMTA'
pub const mouse_shared_magic: u64 = 0x4D534852; // "MSHR"
pub const window_protocol_version: u16 = 1;
pub const window_title_max_bytes: usize = 64;
pub const window_flag_allow_pixels_dma: u32 = 1 << 0;
pub const window_flag_low_scale: u32 = 1 << 1;

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
    title_len: u16,
    title: [window_title_max_bytes]u8,
};

pub const MouseSharedPage = extern struct {
    magic: u64,
    width: u64,
    height: u64,
    pitch: u64,
    cursor_x: u64,
    cursor_y: u64,
    buttons: u64,
    seq: u64,
    wheel: u64,
    log_len: u64,
};
