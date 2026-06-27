pub const FramebufferInfo = struct {
    paddr: u64,
    size_bytes: usize,
    width: u32,
    height: u32,
    pixels_per_scan_line: u32,
    pixel_format: u32,
    mode: u32,
};

pub const BootFile = struct {
    name: []const u8,
    bytes: []const u8,
};
