const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_send_cap: u64 = 0x6;
const syscall_log: u64 = 0x9;

const syscall_ok: u64 = 0;
const endpoint_to_process1: u64 = 0x11;

const request_page_va: usize = 0x2000_3000;
const request_header_qwords: usize = 8;
const request_payload_offset: usize = request_header_qwords * @sizeOf(u64);
const request_payload_pixels: usize = (4096 - request_payload_offset) / @sizeOf(u32);
const request_op_fill_rect: u64 = 1;
const request_op_blit_rect: u64 = 2;

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

fn allocPage() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_page),
        : .{ .memory = true }
    );
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .memory = true }
    );
}

fn sendCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_send_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
        : .{ .memory = true }
    );
}

fn sendFillRect(dst_x: u64, dst_y: u64, width: u64, height: u64, color: u64) bool {
    const paddr = allocPage();
    if (paddr < 0x1000) return false;
    if (mapPage(request_page_va, paddr, true) != syscall_ok) return false;

    const req: [*]volatile u64 = @ptrFromInt(request_page_va);
    req[0] = request_op_fill_rect;
    req[1] = dst_x;
    req[2] = dst_y;
    req[3] = width;
    req[4] = height;
    req[5] = color;
    req[6] = 0;
    req[7] = 0;

    return sendCap(paddr, endpoint_to_process1) == syscall_ok;
}

fn sendBlitRect(dst_x: u64, dst_y: u64, width: u64, height: u64, stride: u64) bool {
    if (width == 0 or height == 0) return false;
    if (stride < width) return false;

    const w: usize = @intCast(width);
    const h: usize = @intCast(height);
    const s: usize = @intCast(stride);
    const needed_pixels = (h - 1) * s + w;
    if (needed_pixels > request_payload_pixels) return false;

    const paddr = allocPage();
    if (paddr < 0x1000) return false;
    if (mapPage(request_page_va, paddr, true) != syscall_ok) return false;

    const req: [*]volatile u64 = @ptrFromInt(request_page_va);
    req[0] = request_op_blit_rect;
    req[1] = dst_x;
    req[2] = dst_y;
    req[3] = width;
    req[4] = height;
    req[5] = stride;
    req[6] = 0;
    req[7] = 0;

    const pixels: [*]volatile u32 = @ptrFromInt(request_page_va + request_payload_offset);
    var y: usize = 0;
    while (y < h) : (y += 1) {
        var x: usize = 0;
        while (x < w) : (x += 1) {
            const on = ((x / 8) + (y / 8)) % 2 == 0;
            pixels[y * s + x] = if (on) 0x0000_FFFF else 0x0000_0000;
        }
    }

    return sendCap(paddr, endpoint_to_process1) == syscall_ok;
}

pub export fn _start() noreturn {
    _ = userLog("DrawClient: started\n");

    if (!sendFillRect(0, 0, 832, 624, 0x0000_3050)) {
        _ = userLog("DrawClient: fill_rect failed\n");
        while (true) asm volatile ("pause");
    }
    _ = userLog("DrawClient: fill_rect sent\n");

    if (!sendBlitRect(120, 80, 42, 24, 42)) {
        _ = userLog("DrawClient: blit_rect failed\n");
        while (true) asm volatile ("pause");
    }
    _ = userLog("DrawClient: blit_rect sent\n");
    while (true) {
        asm volatile ("pause");
    }
}
