const syscall_log: u64 = 0x9;
const protocol = @import("window_protocol.zig");
const window_client = @import("window_client.zig");

const mouse_shared_page_va: usize = 0x3C00_3000;
const window_pixels_va: usize = 0x3C00_4000;
const window_meta_shared_va: usize = 0x3C00_7000;
const window_cap_tmp_va: u64 = 0x201F_F000;

const mouse_shared_magic = protocol.mouse_shared_magic;
const pixel_width: usize = 16;
const pixel_height: usize = 32;
const pixel_pitch: usize = 16;
const MouseSharedPage = protocol.MouseSharedPage;

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

fn fillRect(vfb: [*]volatile u32, x: usize, y: usize, w: usize, h: usize, color: u32) void {
    if (w == 0 or h == 0) return;
    var yy: usize = y;
    while (yy < y + h and yy < pixel_height) : (yy += 1) {
        const row = yy * pixel_pitch;
        var xx: usize = x;
        while (xx < x + w and xx < pixel_width) : (xx += 1) {
            vfb[row + xx] = color;
        }
    }
}

fn drawMousePanel(vfb: [*]volatile u32, buttons: u64) void {
    fillRect(vfb, left_panel_x, left_panel_y, left_panel_w, left_panel_h, 0x00FD_FDFD);
    fillRect(vfb, 1, 4, 14, 24, 0x0060_6060);
    fillRect(vfb, 2, 5, 12, 22, 0x00EE_EEEE);

    const left_pressed = (buttons & 0x1) != 0;
    const right_pressed = (buttons & 0x2) != 0;
    const middle_pressed = (buttons & 0x4) != 0;

    fillRect(vfb, 3, 8, 3, 16, if (left_pressed) 0x0038_C172 else 0x00A8_A8A8);
    fillRect(vfb, 7, 8, 2, 16, if (middle_pressed) 0x004F_8DFF else 0x00A8_A8A8);
    fillRect(vfb, 10, 8, 3, 16, if (right_pressed) 0x00E0_5C5C else 0x00A8_A8A8);
}

pub export fn _start() noreturn {
    _ = userLog("MouseButtonDemo: started\n");

    const mouse_shared: *const volatile MouseSharedPage = @ptrFromInt(mouse_shared_page_va);
    if (mouse_shared.magic != mouse_shared_magic) {
        _ = userLog("MouseButtonDemo: mouse shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }

    _ = userLog("MouseButtonDemo: createAndPublishWindow before\n");
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
    _ = userLog("MouseButtonDemo: createAndPublishWindow after\n");
    const vfb: [*]volatile u32 = @ptrFromInt(window_pixels_va);
    var last_mouse_seq: u64 = 0;
    var last_buttons: u64 = ~@as(u64, 0);
    window_client.setWindowTitle(window_meta_shared_va, "Mouse Demo");

    drawMousePanel(vfb, 0);

    while (true) {
        const mouse_seq = mouse_shared.seq;
        const buttons = mouse_shared.buttons;
        if (mouse_seq != last_mouse_seq or buttons != last_buttons) {
            last_mouse_seq = mouse_seq;
            last_buttons = buttons;
            drawMousePanel(vfb, buttons);
        }

        asm volatile ("pause");
    }
}
