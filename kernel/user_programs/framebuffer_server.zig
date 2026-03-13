const syscall_map_page: u64 = 0x2;
const syscall_log: u64 = 0x9;
const syscall_recv_cap: u64 = 0xA;
const window_client = @import("window_client.zig");

const syscall_ok: u64 = 0;
const syscall_err_empty: u64 = 13;

const window_pixels_va: usize = 0x3C00_4000;
const window_meta_shared_va: usize = 0x3C00_7000;
const window_cap_tmp_va: u64 = 0x2000_4000;
const request_page_va: usize = 0x2000_3000;
const fb_width: usize = 32;
const fb_height: usize = 32;
const fb_pitch: usize = 32;
const request_header_qwords: usize = 8;
const request_payload_offset: usize = request_header_qwords * @sizeOf(u64);
const request_payload_bytes: usize = 4096 - request_payload_offset;
const request_payload_pixels: usize = request_payload_bytes / @sizeOf(u32);
const request_op_fill_rect: u64 = 1;
const request_op_blit_rect: u64 = 2;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn recvCap() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_recv_cap),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn clampRectToFramebuffer(
    dst_x: usize,
    dst_y: usize,
    width: usize,
    height: usize,
) ?struct { x: usize, y: usize, width: usize, height: usize } {
    if (width == 0 or height == 0) return null;
    if (dst_x >= fb_width or dst_y >= fb_height) return null;

    const max_w = fb_width - dst_x;
    const max_h = fb_height - dst_y;
    const clamped_w = if (width > max_w) max_w else width;
    const clamped_h = if (height > max_h) max_h else height;
    if (clamped_w == 0 or clamped_h == 0) return null;

    return .{
        .x = dst_x,
        .y = dst_y,
        .width = clamped_w,
        .height = clamped_h,
    };
}

pub export fn _start() noreturn {
    _ = userLog("FramebufferServer: started\n");
    const window_created = window_client.createAndPublishWindowWithDma(
        @intCast(fb_width),
        @intCast(fb_height),
        window_cap_tmp_va,
        window_pixels_va,
        window_meta_shared_va,
    );
    if (!window_created) {
        _ = userLog("FramebufferServer: create window failed\n");
        while (true) asm volatile ("pause");
    }
    window_client.setWindowTitle(window_meta_shared_va, "FramebufferSrv");
    const fb: [*]volatile u32 = @ptrFromInt(window_pixels_va);
    while (true) {
        const paddr = recvCap();
        if (paddr == syscall_err_empty) {
            asm volatile ("pause");
            continue;
        }
        if (paddr < 0x1000) {
            asm volatile ("pause");
            continue;
        }
        if (mapPage(request_page_va, paddr, false) != syscall_ok) {
            _ = userLog("FramebufferServer: map request failed\n");
            asm volatile ("pause");
            continue;
        }

        const req: [*]volatile u64 = @ptrFromInt(request_page_va);
        const op = req[0];
        const dst_x: usize = @intCast(req[1]);
        const dst_y: usize = @intCast(req[2]);
        const width: usize = @intCast(req[3]);
        const height: usize = @intCast(req[4]);
        const arg0 = req[5];

        const rect = clampRectToFramebuffer(dst_x, dst_y, width, height) orelse {
            _ = userLog("FramebufferServer: invalid rect\n");
            asm volatile ("pause");
            continue;
        };

        if (op == request_op_fill_rect) {
            const color: u32 = @intCast(arg0 & 0xFFFF_FFFF);
            var y: usize = 0;
            while (y < rect.height) : (y += 1) {
                const fb_row = (rect.y + y) * fb_pitch + rect.x;
                var x: usize = 0;
                while (x < rect.width) : (x += 1) {
                    fb[fb_row + x] = color;
                }
            }
            window_client.markWindowDirtyRect(window_meta_shared_va, rect.x, rect.y, rect.width, rect.height);
            _ = userLog("FramebufferServer: rect fill done\n");
            asm volatile ("pause");
            continue;
        }

        if (op == request_op_blit_rect) {
            const src_stride: usize = @intCast(arg0);
            if (src_stride < rect.width) {
                _ = userLog("FramebufferServer: invalid blit stride\n");
                asm volatile ("pause");
                continue;
            }

            const needed_pixels = (rect.height - 1) * src_stride + rect.width;
            if (needed_pixels > request_payload_pixels) {
                _ = userLog("FramebufferServer: blit payload too large\n");
                asm volatile ("pause");
                continue;
            }

            const src: [*]volatile u32 = @ptrFromInt(request_page_va + request_payload_offset);
            var y: usize = 0;
            while (y < rect.height) : (y += 1) {
                const fb_row = (rect.y + y) * fb_pitch + rect.x;
                const src_row = y * src_stride;
                var x: usize = 0;
                while (x < rect.width) : (x += 1) {
                    fb[fb_row + x] = src[src_row + x];
                }
            }
            window_client.markWindowDirtyRect(window_meta_shared_va, rect.x, rect.y, rect.width, rect.height);
            _ = userLog("FramebufferServer: rect blit done\n");
            asm volatile ("pause");
            continue;
        }

        _ = userLog("FramebufferServer: unknown request\n");
        asm volatile ("pause");
    }
}
