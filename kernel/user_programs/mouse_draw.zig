const protocol = @import("window_protocol.zig");
const window_client = @import("window_client.zig");

const syscall_log: u64 = 0x9;

const shared_page_va: usize = 0x3C00_3000;
const window_pixels_va: usize = 0x3C00_4000;
const window_meta_shared_va: usize = 0x3C00_7000;
const window_cap_tmp_va: u64 = 0x3C10_0000;
const pixel_width: i32 = 32;
const pixel_height: i32 = 32;
const pixel_pitch: i32 = 32;

const shared_magic = protocol.mouse_shared_magic;
const cursor_size: i32 = 9;
const cursor_half: i32 = cursor_size / 2;
const MouseSharedPage = protocol.MouseSharedPage;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn clampI32(v: i32, lo: i32, hi: i32) i32 {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

fn scaleCoord(raw: u64, src_extent: u64, dst_extent: i32) i32 {
    if (dst_extent <= 0) return 0;
    if (src_extent == 0) return 0;
    const max_src = src_extent - 1;
    const clamped = if (raw > max_src) max_src else raw;
    const dst_minus_one_u64: u64 = @intCast(dst_extent - 1);
    const scaled = (clamped * dst_minus_one_u64) / max_src;
    return @intCast(scaled);
}

fn drawRect(
    fb: [*]volatile u32,
    width: i32,
    height: i32,
    pitch: i32,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    color: u32,
) void {
    if (w <= 0 or h <= 0) return;
    var x0 = x;
    var y0 = y;
    var x1 = x + w;
    var y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    if (x0 >= x1 or y0 >= y1) return;

    var py = y0;
    while (py < y1) : (py += 1) {
        const row_off: usize = @intCast(py * pitch);
        var px = x0;
        while (px < x1) : (px += 1) {
            const idx: usize = row_off + @as(usize, @intCast(px));
            fb[idx] = color;
        }
    }
}

pub export fn _start() noreturn {
    _ = userLog("MouseDraw: started\n");

    const shared: *volatile MouseSharedPage = @ptrFromInt(shared_page_va);
    if (shared.magic != shared_magic) {
        _ = userLog("MouseDraw: shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }

    const window_created = window_client.createAndPublishWindowWithDma(
        @intCast(pixel_width),
        @intCast(pixel_height),
        window_cap_tmp_va,
        window_pixels_va,
        window_meta_shared_va,
    );
    if (!window_created) {
        _ = userLog("MouseDraw: create window failed\n");
        while (true) asm volatile ("pause");
    }
    window_client.setWindowTitle(window_meta_shared_va, "Mouse Draw");

    const fb: [*]volatile u32 = @ptrFromInt(window_pixels_va);

    // Clear screen once.
    var y: i32 = 0;
    while (y < pixel_height) : (y += 1) {
        const row_off: usize = @intCast(y * pixel_pitch);
        var x: i32 = 0;
        while (x < pixel_width) : (x += 1) {
            fb[row_off + @as(usize, @intCast(x))] = 0x00000000;
        }
    }
    window_client.markWindowDirty(window_meta_shared_va);

    var prev_x: i32 = -1;
    var prev_y: i32 = -1;
    var last_seq: u64 = 0;

    while (true) {
        const seq = shared.seq;
        if (seq == last_seq) {
            asm volatile ("pause");
            continue;
        }
        last_seq = seq;

        var x = scaleCoord(shared.cursor_x, shared.width, pixel_width);
        var y2 = scaleCoord(shared.cursor_y, shared.height, pixel_height);
        const buttons: u64 = shared.buttons;
        x = clampI32(x, 0, pixel_width - 1);
        y2 = clampI32(y2, 0, pixel_height - 1);

        if (prev_x >= 0 and prev_y >= 0) {
            drawRect(fb, pixel_width, pixel_height, pixel_pitch, prev_x - cursor_half, prev_y - cursor_half, cursor_size, cursor_size, 0x00000000);
        }

        const color: u32 = if ((buttons & 0x1) != 0) 0x000000FF else 0x00FFFFFF;
        drawRect(fb, pixel_width, pixel_height, pixel_pitch, x - cursor_half, y2 - cursor_half, cursor_size, cursor_size, color);
        prev_x = x;
        prev_y = y2;
        window_client.markWindowDirty(window_meta_shared_va);
    }
}
