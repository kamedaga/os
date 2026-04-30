const std = @import("std");

pub const magic: u64 = 0x4D534852; // "MSHR"
pub const page_bytes: usize = 4096;
pub const header_bytes: usize = 128;
pub const log_offset_bytes: usize = header_bytes;
pub const log_capacity_bytes: usize = page_bytes - header_bytes;

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

comptime {
    if (@sizeOf(MouseSharedPage) > header_bytes) {
        @compileError("MouseSharedPage must fit in header_bytes");
    }
}

pub fn clearPage(bytes: []volatile u8) void {
    std.debug.assert(bytes.len >= page_bytes);
    var i: usize = 0;
    while (i < page_bytes) : (i += 1) {
        bytes[i] = 0;
    }
}
