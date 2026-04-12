const protocol = @import("window_protocol.zig");

pub const shared_page_va: usize = 0x3C00_3000;
pub const MouseSharedPage = protocol.MouseSharedPage;
const shared_magic = protocol.mouse_shared_magic;
pub const button_left: u64 = 0x1;
pub const button_right: u64 = 0x2;
pub const button_middle: u64 = 0x4;
const max_coord_limit: u64 = 0x7FFF_FFFF;
var current_shared_page_va: usize = shared_page_va;

pub const Snapshot = struct {
    seq: u64,
    changed: bool,
    screen_x: i32,
    screen_y: i32,
    local_x: i32,
    local_y: i32,
    buttons: u64,
    prev_buttons: u64,

    pub fn buttonDown(self: Snapshot, mask: u64) bool {
        return (self.buttons & mask) != 0;
    }

    pub fn buttonJustPressed(self: Snapshot, mask: u64) bool {
        return (self.buttons & mask) != 0 and (self.prev_buttons & mask) == 0;
    }

    pub fn buttonJustReleased(self: Snapshot, mask: u64) bool {
        return (self.buttons & mask) == 0 and (self.prev_buttons & mask) != 0;
    }

    pub fn leftDown(self: Snapshot) bool {
        return self.buttonDown(button_left);
    }

    pub fn rightDown(self: Snapshot) bool {
        return self.buttonDown(button_right);
    }

    pub fn middleDown(self: Snapshot) bool {
        return self.buttonDown(button_middle);
    }

    pub fn leftJustPressed(self: Snapshot) bool {
        return self.buttonJustPressed(button_left);
    }

    pub fn rightJustPressed(self: Snapshot) bool {
        return self.buttonJustPressed(button_right);
    }

    pub fn middleJustPressed(self: Snapshot) bool {
        return self.buttonJustPressed(button_middle);
    }

    pub fn leftJustReleased(self: Snapshot) bool {
        return self.buttonJustReleased(button_left);
    }

    pub fn rightJustReleased(self: Snapshot) bool {
        return self.buttonJustReleased(button_right);
    }

    pub fn middleJustReleased(self: Snapshot) bool {
        return self.buttonJustReleased(button_middle);
    }
};

pub const Reader = struct {
    local_origin_x: i32 = 0,
    local_origin_y: i32 = 0,
    initialized: bool = false,
    last_seq: u64 = 0,
    last_buttons: u64 = 0,

    pub fn init(local_origin_x: i32, local_origin_y: i32) Reader {
        return .{
            .local_origin_x = local_origin_x,
            .local_origin_y = local_origin_y,
        };
    }

    pub fn setLocalOrigin(self: *Reader, local_origin_x: i32, local_origin_y: i32) void {
        self.local_origin_x = local_origin_x;
        self.local_origin_y = local_origin_y;
    }

    pub fn read(self: *Reader) ?Snapshot {
        const page = sharedPage() orelse return null;
        const screen_width = decodeLimit(page.width);
        const screen_height = decodeLimit(page.height);
        const screen_x = decodeSharedCoord(page.cursor_x, screen_width, 0);
        const screen_y = decodeSharedCoord(page.cursor_y, screen_height, 0);
        const prev_buttons = if (self.initialized) self.last_buttons else page.buttons;
        const changed = self.initialized and page.seq != self.last_seq;
        self.initialized = true;
        self.last_seq = page.seq;
        self.last_buttons = page.buttons;
        return .{
            .seq = page.seq,
            .changed = changed,
            .screen_x = screen_x,
            .screen_y = screen_y,
            .local_x = screen_x - self.local_origin_x,
            .local_y = screen_y - self.local_origin_y,
            .buttons = page.buttons,
            .prev_buttons = prev_buttons,
        };
    }
};

fn decodeSharedCoord(raw: u64, limit: i32, fallback: i32) i32 {
    if (limit <= 0) return fallback;
    const max_ok: u64 = @intCast(limit - 1);
    if (raw > max_ok) return fallback;
    return @intCast(raw);
}

fn decodeLimit(raw: u64) i32 {
    if (raw == 0 or raw > max_coord_limit) return 0;
    return @intCast(raw);
}

pub fn setSharedPageVa(va: u64) void {
    if (va == 0) return;
    current_shared_page_va = @intCast(va);
}

pub fn sharedPage() ?*const volatile MouseSharedPage {
    const page: *const volatile MouseSharedPage = @ptrFromInt(current_shared_page_va);
    if (page.magic != shared_magic) return null;
    return page;
}

pub fn readSnapshot(local_origin_x: i32, local_origin_y: i32) ?Snapshot {
    var reader = Reader.init(local_origin_x, local_origin_y);
    return reader.read();
}

pub fn leftPressed(buttons: u64) bool {
    return (buttons & button_left) != 0;
}

pub fn rightPressed(buttons: u64) bool {
    return (buttons & button_right) != 0;
}

pub fn middlePressed(buttons: u64) bool {
    return (buttons & button_middle) != 0;
}
