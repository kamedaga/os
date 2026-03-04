const syscall_log: u64 = 0x9;

const shared_page_va: usize = 0x2000_3000;
const framebuffer_va: usize = 0x2000_4000;

const shared_magic: u64 = 0x4D534852; // "MSHR"
const cursor_size: i32 = 9;
const cursor_half: i32 = cursor_size / 2;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .memory = true }
    );
}

fn clampI32(v: i32, lo: i32, hi: i32) i32 {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
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

    const shared: [*]volatile u64 = @ptrFromInt(shared_page_va);
    if (shared[0] != shared_magic) {
        _ = userLog("MouseDraw: shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }

    const width: i32 = @intCast(shared[1]);
    const height: i32 = @intCast(shared[2]);
    const pitch: i32 = @intCast(shared[3]);
    if (width <= 0 or height <= 0 or pitch <= 0) {
        _ = userLog("MouseDraw: invalid framebuffer info\n");
        while (true) asm volatile ("pause");
    }

    const fb: [*]volatile u32 = @ptrFromInt(framebuffer_va);

    // Clear screen once.
    var y: i32 = 0;
    while (y < height) : (y += 1) {
        const row_off: usize = @intCast(y * pitch);
        var x: i32 = 0;
        while (x < width) : (x += 1) {
            fb[row_off + @as(usize, @intCast(x))] = 0x00000000;
        }
    }

    var prev_x: i32 = -1;
    var prev_y: i32 = -1;
    var last_seq: u64 = 0;

    while (true) {
        const seq = shared[7];
        if (seq == last_seq) {
            asm volatile ("pause");
            continue;
        }
        last_seq = seq;

        var x: i32 = @intCast(shared[4]);
        var y2: i32 = @intCast(shared[5]);
        const buttons: u64 = shared[6];
        x = clampI32(x, 0, width - 1);
        y2 = clampI32(y2, 0, height - 1);

        if (prev_x >= 0 and prev_y >= 0) {
            drawRect(fb, width, height, pitch, prev_x - cursor_half, prev_y - cursor_half, cursor_size, cursor_size, 0x00000000);
        }

        const color: u32 = if ((buttons & 0x1) != 0) 0x000000FF else 0x00FFFFFF;
        drawRect(fb, width, height, pitch, x - cursor_half, y2 - cursor_half, cursor_size, cursor_size, color);
        prev_x = x;
        prev_y = y2;
    }
}
