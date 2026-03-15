const syscall_log: u64 = 0x9;
const syscall_wait_event: u64 = 0x17;
const mouse_input = @import("mouse_input.zig");
const window_client = @import("window_client.zig");

const window_pixels_va: usize = 0x3C00_4000;
const window_meta_shared_va: usize = 0x3C00_7000;
const window_cap_tmp_va: u64 = 0x3C00_8000;

const pixel_width: usize = 16;
const pixel_height: usize = 32;
const pixel_pitch: usize = 16;

const left_panel_x: usize = 0;
const left_panel_y: usize = 0;
const left_panel_w: usize = 16;
const left_panel_h: usize = 32;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn fillRect(x: usize, y: usize, w: usize, h: usize, color: u32) void {
    if (w == 0 or h == 0) return;
    if (x >= pixel_width or y >= pixel_height) return;
    const vfb: [*]volatile u32 = @ptrFromInt(window_pixels_va);
    const max_pixels = pixel_width * pixel_height;
    const x_end = @min(pixel_width, x + w);
    const y_end = @min(pixel_height, y + h);
    var yy: usize = y;
    while (yy < y_end) : (yy += 1) {
        const row = yy * pixel_pitch;
        var xx: usize = x;
        while (xx < x_end) : (xx += 1) {
            const index = row + xx;
            if (index >= max_pixels) break;
            vfb[index] = color;
        }
    }
}

fn drawMousePanel(vfb: [*]volatile u32, buttons: u64) void {
    _ = vfb;
    fillRect(left_panel_x, left_panel_y, left_panel_w, left_panel_h, 0x00FD_FDFD);
    fillRect(1, 4, 14, 24, 0x0060_6060);
    fillRect(2, 5, 12, 22, 0x00EE_EEEE);

    const left_pressed = (buttons & 0x1) != 0;
    const right_pressed = (buttons & 0x2) != 0;
    const middle_pressed = (buttons & 0x4) != 0;

    fillRect(3, 8, 3, 16, if (left_pressed) 0x0038_C172 else 0x00A8_A8A8);
    fillRect(7, 8, 2, 16, if (middle_pressed) 0x004F_8DFF else 0x00A8_A8A8);
    fillRect(10, 8, 3, 16, if (right_pressed) 0x00E0_5C5C else 0x00A8_A8A8);
}

pub export fn _start() noreturn {
    _ = userLog("MouseButtonDemo: started\n");

    if (mouse_input.sharedPage() == null) {
        _ = userLog("MouseButtonDemo: mouse shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }
    var mouse_reader = mouse_input.Reader.init(0, 0);

    const window_created = window_client.createAndPublishWindow(
        @intCast(pixel_width),
        @intCast(pixel_height),
        0,
        window_cap_tmp_va,
        window_pixels_va,
        window_meta_shared_va,
    );
    if (!window_created) {
        _ = userLog("MouseButtonDemo: create window failed\n");
        while (true) asm volatile ("pause");
    }
    const vfb: [*]volatile u32 = @ptrFromInt(window_pixels_va);
    var last_buttons: u64 = ~@as(u64, 0);
    window_client.setWindowTitle(window_meta_shared_va, "Mouse Demo");
    window_client.setWindowPosition(window_meta_shared_va, 520, 96);

    drawMousePanel(vfb, 0);
    window_client.markWindowDirty(window_meta_shared_va);

    while (true) {
        const mouse = mouse_reader.read() orelse {
            _ = userLog("MouseButtonDemo: mouse shared page unavailable\n");
            while (true) asm volatile ("pause");
        };
        if (mouse.buttons != last_buttons) {
            last_buttons = mouse.buttons;
            drawMousePanel(vfb, mouse.buttons);
            window_client.markWindowDirty(window_meta_shared_va);
        }
        _ = waitEvent(false, 1);
    }
}
