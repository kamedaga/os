const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_grant_cap: u64 = 0x8;
const syscall_log: u64 = 0x9;
const syscall_recv_cap: u64 = 0xA;

const syscall_ok: u64 = 0;
const syscall_err_empty: u64 = 13;

const shared_page_va: usize = 0x3C00_3000;
const framebuffer_va: usize = 0x3C00_5000;
const virtual_framebuffer_va: usize = 0x3C00_4000;
const ipc_rx_page_va: usize = 0x201F_F000;
const back_buffer_base_va: usize = 0x2040_0000;

const shared_magic: u64 = 0x4D534852; // "MSHR"
const process0_id: u64 = 0;
const rights_read_write: u64 = 0x3;

const fb_width: usize = 832;
const fb_height: usize = 624;
const fb_pitch: usize = 832;
const fb_pixels: usize = fb_pitch * fb_height;

const vfb_width: usize = 32;
const vfb_height: usize = 32;
const vfb_pitch: usize = 32;
const vfb_pixels: usize = vfb_pitch * vfb_height;

const page_bytes: usize = 4096;
const back_buffer_bytes: usize = fb_pixels * @sizeOf(u32);
const back_buffer_pages: usize = (back_buffer_bytes + page_bytes - 1) / page_bytes;
var back_buffer_storage: [fb_pixels]u32 align(4096) = [_]u32{0} ** fb_pixels;
var shadow_storage: [vfb_pixels]u32 align(64) = [_]u32{0} ** vfb_pixels;

const cursor_size_i32: i32 = 11;
const cursor_half: i32 = cursor_size_i32 / 2;
const center_square_size: usize = 160;

const Geometry = struct {
    scale: usize,
    off_x: usize,
    off_y: usize,
};

const Rect = struct {
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
};

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .memory = true });
}

fn allocPage() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_page),
        : .{ .memory = true });
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .memory = true });
}

fn recvCap() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_recv_cap),
        : .{ .memory = true });
}

fn grantCap(to_process: u64, paddr: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (to_process),
          [arg2] "{rdx}" (rights_bits),
        : .{ .memory = true });
}

fn clampI32(v: i32, lo: i32, hi: i32) i32 {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

fn absI32(v: i32) i32 {
    return if (v < 0) -v else v;
}

fn calcGeometry() Geometry {
    const scale_x = fb_width / vfb_width;
    const scale_y = fb_height / vfb_height;
    const scale = if (scale_x < scale_y) scale_x else scale_y;
    const out_w = vfb_width * scale;
    const out_h = vfb_height * scale;
    return .{
        .scale = scale,
        .off_x = (fb_width - out_w) / 2,
        .off_y = (fb_height - out_h) / 2,
    };
}

fn initBackBuffer() [*]u32 {
    _ = back_buffer_bytes;
    _ = back_buffer_pages;
    _ = page_bytes;
    _ = back_buffer_base_va;
    _ = userLog("Compositor: back buffer ready (bss)\n");
    return @ptrCast(&back_buffer_storage[0]);
}

fn clearBackBuffer(back_buffer: [*]u32, color: u32) void {
    var i: usize = 0;
    while (i < fb_pixels) : (i += 1) {
        back_buffer[i] = color;
    }
}

fn blitScaledPixelToBack(back_buffer: [*]u32, g: Geometry, vx: usize, vy: usize, color: u32) void {
    const px0 = g.off_x + vx * g.scale;
    const py0 = g.off_y + vy * g.scale;
    var sy: usize = 0;
    while (sy < g.scale) : (sy += 1) {
        const row = (py0 + sy) * fb_pitch;
        var sx: usize = 0;
        while (sx < g.scale) : (sx += 1) {
            back_buffer[row + px0 + sx] = color;
        }
    }
}

fn copyRectBackToFb(back_buffer: [*]u32, fb: [*]volatile u32, rect: Rect) void {
    var x0 = rect.x0;
    var y0 = rect.y0;
    var x1 = rect.x1;
    var y1 = rect.y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > @as(i32, @intCast(fb_width))) x1 = @intCast(fb_width);
    if (y1 > @as(i32, @intCast(fb_height))) y1 = @intCast(fb_height);
    if (x0 >= x1 or y0 >= y1) return;

    const ux0: usize = @intCast(x0);
    const ux1: usize = @intCast(x1);
    var y: usize = @intCast(y0);
    const uy1: usize = @intCast(y1);
    while (y < uy1) : (y += 1) {
        const row = y * fb_pitch;
        var x: usize = ux0;
        while (x < ux1) : (x += 1) {
            fb[row + x] = back_buffer[row + x];
        }
    }
}

fn drawCenterBlackSquare(back_buffer: [*]u32) Rect {
    const size = if (center_square_size <= fb_width and center_square_size <= fb_height)
        center_square_size
    else
        (if (fb_width < fb_height) fb_width else fb_height);
    const x0: usize = (fb_width - size) / 2;
    const y0: usize = (fb_height - size) / 2;
    const x1: usize = x0 + size;
    const y1: usize = y0 + size;

    var y: usize = y0;
    while (y < y1) : (y += 1) {
        const row = y * fb_pitch;
        var x: usize = x0;
        while (x < x1) : (x += 1) {
            back_buffer[row + x] = 0x0000_0000;
        }
    }

    return .{
        .x0 = @intCast(x0),
        .y0 = @intCast(y0),
        .x1 = @intCast(x1),
        .y1 = @intCast(y1),
    };
}

fn cursorRect(cx: i32, cy: i32) Rect {
    return .{
        .x0 = cx - cursor_half,
        .y0 = cy - cursor_half,
        .x1 = cx + cursor_half + 1,
        .y1 = cy + cursor_half + 1,
    };
}

fn drawCursorOnFb(fb: [*]volatile u32, cx: i32, cy: i32, buttons: u64) void {
    const r = cursorRect(cx, cy);
    var x0 = r.x0;
    var y0 = r.y0;
    var x1 = r.x1;
    var y1 = r.y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > @as(i32, @intCast(fb_width))) x1 = @intCast(fb_width);
    if (y1 > @as(i32, @intCast(fb_height))) y1 = @intCast(fb_height);
    if (x0 >= x1 or y0 >= y1) return;

    const color: u32 = if ((buttons & 0x1) != 0) 0x00FF_4040 else 0x00FF_FFFF;
    var y: i32 = y0;
    while (y < y1) : (y += 1) {
        const row: usize = @as(usize, @intCast(y)) * fb_pitch;
        var x: i32 = x0;
        while (x < x1) : (x += 1) {
            const dx = x - cx;
            const dy = y - cy;
            const edge = absI32(dx) == cursor_half or absI32(dy) == cursor_half;
            const cross = dx == 0 or dy == 0;
            const diagonal = absI32(dx) == absI32(dy);
            if (edge or cross or diagonal) {
                fb[row + @as(usize, @intCast(x))] = color;
            }
        }
    }
}

pub export fn _start() noreturn {
    _ = userLog("Compositor: started\n");

    const fb: [*]volatile u32 = @ptrFromInt(framebuffer_va);
    const vfb: [*]const volatile u32 = @ptrFromInt(virtual_framebuffer_va);
    var shared_ready = false;
    var shared_words: [*]volatile u64 = undefined;
    var cursor_x: i32 = @intCast(fb_width / 2);
    var cursor_y: i32 = @intCast(fb_height / 2);
    var cursor_buttons: u64 = 0;
    var last_seq: u64 = 0;
    var cursor_drawn = false;
    var last_cursor_rect = cursorRect(cursor_x, cursor_y);

    // Receive shared page as early as possible so MouseDriver can finish handshake.
    while (true) {
        const page_paddr = recvCap();
        if (page_paddr == syscall_err_empty) break;
        if (page_paddr < 0x1000) continue;
        if (mapPage(ipc_rx_page_va, page_paddr, false) != syscall_ok) continue;

        const msg_words: [*]const volatile u64 = @ptrFromInt(ipc_rx_page_va);
        if (msg_words[0] != shared_magic or shared_ready) continue;
        if (mapPage(shared_page_va, page_paddr, false) != syscall_ok) continue;

        shared_words = @ptrFromInt(shared_page_va);
        if (shared_words[0] != shared_magic) continue;

        cursor_x = clampI32(@intCast(shared_words[4]), 0, @as(i32, @intCast(fb_width)) - 1);
        cursor_y = clampI32(@intCast(shared_words[5]), 0, @as(i32, @intCast(fb_height)) - 1);
        cursor_buttons = shared_words[6];
        last_seq = shared_words[7];
        shared_ready = true;

        if (grantCap(process0_id, page_paddr, rights_read_write) != syscall_ok) {
            _ = userLog("Compositor: grant shared cap back failed\n");
            while (true) asm volatile ("pause");
        }
        _ = userLog("Compositor: mouse shared page received via IPC\n");
    }

    const back_buffer: [*]u32 = initBackBuffer();
    const g = calcGeometry();
    if (g.scale == 0) {
        _ = userLog("Compositor: invalid geometry\n");
        while (true) asm volatile ("pause");
    }

    const shadow: [*]u32 = @ptrCast(&shadow_storage[0]);
    var force_full = true;

    // Avoid black flash: keep BootLog screen until first real compose.
    clearBackBuffer(back_buffer, 0x0000_0000);
    _ = userLog("Compositor: vfb compose ready\n");

    while (true) {
        var mouse_changed = false;

        while (true) {
            const page_paddr = recvCap();
            if (page_paddr == syscall_err_empty) break;
            if (page_paddr < 0x1000) {
                _ = userLog("Compositor: recv cap err\n");
                break;
            }
            if (mapPage(ipc_rx_page_va, page_paddr, false) != syscall_ok) continue;

            const msg_words: [*]const volatile u64 = @ptrFromInt(ipc_rx_page_va);
            if (msg_words[0] != shared_magic or shared_ready) continue;
            if (mapPage(shared_page_va, page_paddr, false) != syscall_ok) continue;

            shared_words = @ptrFromInt(shared_page_va);
            if (shared_words[0] != shared_magic) continue;

            cursor_x = clampI32(@intCast(shared_words[4]), 0, @as(i32, @intCast(fb_width)) - 1);
            cursor_y = clampI32(@intCast(shared_words[5]), 0, @as(i32, @intCast(fb_height)) - 1);
            cursor_buttons = shared_words[6];
            last_seq = shared_words[7];
            shared_ready = true;
            mouse_changed = true;

            if (grantCap(process0_id, page_paddr, rights_read_write) != syscall_ok) {
                _ = userLog("Compositor: grant shared cap back failed\n");
                while (true) asm volatile ("pause");
            }
            _ = userLog("Compositor: mouse shared page received via IPC\n");
        }

        var dirty_any = false;
        var dirty_x0: usize = 0;
        var dirty_y0: usize = 0;
        var dirty_x1: usize = 0;
        var dirty_y1: usize = 0;

        var i: usize = 0;
        while (i < vfb_pixels) : (i += 1) {
            const color = vfb[i];
            if (!force_full and color == shadow[i]) continue;
            shadow[i] = color;
            const vy = i / vfb_pitch;
            const vx = i - vy * vfb_pitch;
            const px0 = g.off_x + vx * g.scale;
            const py0 = g.off_y + vy * g.scale;
            const px1 = px0 + g.scale;
            const py1 = py0 + g.scale;
            blitScaledPixelToBack(back_buffer, g, vx, vy, color);

            if (!dirty_any) {
                dirty_x0 = px0;
                dirty_y0 = py0;
                dirty_x1 = px1;
                dirty_y1 = py1;
                dirty_any = true;
            } else {
                if (px0 < dirty_x0) dirty_x0 = px0;
                if (py0 < dirty_y0) dirty_y0 = py0;
                if (px1 > dirty_x1) dirty_x1 = px1;
                if (py1 > dirty_y1) dirty_y1 = py1;
            }
        }

        const center_rect = drawCenterBlackSquare(back_buffer);
        if (!dirty_any) {
            dirty_x0 = @intCast(center_rect.x0);
            dirty_y0 = @intCast(center_rect.y0);
            dirty_x1 = @intCast(center_rect.x1);
            dirty_y1 = @intCast(center_rect.y1);
            dirty_any = true;
        } else {
            const cx0: usize = @intCast(center_rect.x0);
            const cy0: usize = @intCast(center_rect.y0);
            const cx1: usize = @intCast(center_rect.x1);
            const cy1: usize = @intCast(center_rect.y1);
            if (cx0 < dirty_x0) dirty_x0 = cx0;
            if (cy0 < dirty_y0) dirty_y0 = cy0;
            if (cx1 > dirty_x1) dirty_x1 = cx1;
            if (cy1 > dirty_y1) dirty_y1 = cy1;
        }

        if (shared_ready) {
            const seq = shared_words[7];
            if (seq != last_seq) {
                last_seq = seq;
                cursor_x = clampI32(@intCast(shared_words[4]), 0, @as(i32, @intCast(fb_width)) - 1);
                cursor_y = clampI32(@intCast(shared_words[5]), 0, @as(i32, @intCast(fb_height)) - 1);
                cursor_buttons = shared_words[6];
                mouse_changed = true;
            }
        }

        if (force_full) {
            copyRectBackToFb(back_buffer, fb, .{ .x0 = 0, .y0 = 0, .x1 = @intCast(fb_width), .y1 = @intCast(fb_height) });
        } else if (dirty_any) {
            copyRectBackToFb(back_buffer, fb, .{
                .x0 = @intCast(dirty_x0),
                .y0 = @intCast(dirty_y0),
                .x1 = @intCast(dirty_x1),
                .y1 = @intCast(dirty_y1),
            });
        } else if (mouse_changed and cursor_drawn) {
            copyRectBackToFb(back_buffer, fb, last_cursor_rect);
        }

        if (shared_ready and (force_full or dirty_any or mouse_changed or !cursor_drawn)) {
            drawCursorOnFb(fb, cursor_x, cursor_y, cursor_buttons);
            last_cursor_rect = cursorRect(cursor_x, cursor_y);
            cursor_drawn = true;
        }

        force_full = false;
        asm volatile ("pause");
    }
}
