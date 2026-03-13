const protocol = @import("window_protocol.zig");

pub const window_title_max_bytes = protocol.window_title_max_bytes;

pub const Rect = struct {
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
};

pub const SourceRegion = struct {
    x: usize = 0,
    y: usize = 0,
    w: usize = 0,
    h: usize = 0,
};

pub const MouseState = struct {
    ready: bool = false,
    x: i32,
    y: i32,
    buttons: u64 = 0,
    seq: u64 = 0,
};

pub const WindowSource = struct {
    active: bool = false,
    window_id: u32 = 0,
    pixel_va: usize = 0,
    pixels_paddr: u64 = 0,
    pixels_page_count: usize = 0,
    dma_pixels: bool = false,
    meta_va: usize = 0,
    observed_meta_seq: u64 = 0,
    width: usize = 0,
    height: usize = 0,
    pitch: usize = 0,
    flags: u32 = 0,
};

pub const WindowFrame = struct {
    active: bool = false,
    visible: bool = false,
    src: SourceRegion = .{},
    x: i32 = 0,
    y: i32 = 0,
    drag_off_x: i32 = 0,
    drag_off_y: i32 = 0,
    prev_close_hover: bool = false,
    prev_close_down: bool = false,
    content_scale: u8 = 1,
    title: [window_title_max_bytes]u8 = [_]u8{0} ** window_title_max_bytes,
    title_len: usize = 0,
};

pub const WindowSlot = struct {
    source: WindowSource = .{},
    frame: WindowFrame = .{},
    z_order: u32 = 0,
    close_hover: bool = false,
    close_down: bool = false,
    title_draw_x_off: i32 = 0,
    title_clip_right_off: i32 = 0,
    title_cache_len: usize = 0,
    title_cache: [window_title_max_bytes + 3]u8 = [_]u8{0} ** (window_title_max_bytes + 3),

    pub fn reset(self: *WindowSlot) void {
        self.* = .{};
    }

    pub fn isActive(self: *const WindowSlot) bool {
        return self.source.active and self.frame.active;
    }

    pub fn isVisible(self: *const WindowSlot) bool {
        return self.isActive() and self.frame.visible;
    }
};

pub fn WindowStore(comptime max_windows: usize) type {
    return struct {
        const Self = @This();

        slots: [max_windows]WindowSlot = [_]WindowSlot{.{}} ** max_windows,

        pub fn reset(self: *Self) void {
            var i: usize = 0;
            while (i < max_windows) : (i += 1) {
                self.slots[i].reset();
            }
        }

        pub fn findById(self: *const Self, window_id: u32) ?usize {
            var i: usize = 0;
            while (i < max_windows) : (i += 1) {
                if (self.slots[i].source.active and self.slots[i].source.window_id == window_id) return i;
            }
            return null;
        }

        pub fn findFree(self: *const Self) ?usize {
            var i: usize = 0;
            while (i < max_windows) : (i += 1) {
                if (!self.slots[i].source.active) return i;
            }
            return null;
        }

        pub fn visibleCount(self: *const Self) usize {
            var count: usize = 0;
            var i: usize = 0;
            while (i < max_windows) : (i += 1) {
                if (self.slots[i].isVisible()) count += 1;
            }
            return count;
        }
    };
}
