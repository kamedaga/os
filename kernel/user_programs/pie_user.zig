pub var relocated_target: u64 = 0;
pub var relocated_ptr: *u64 = &relocated_target;

extern fn cap_write(fd: i32, buf: [*]const u8, len: usize) callconv(.c) i64;
extern fn cap_malloc(len: usize) callconv(.c) ?[*]u8;
extern fn cap_free(ptr: ?[*]u8) callconv(.c) void;
extern var cap_errno: i32;

pub export fn _start() noreturn {
    const msg = "hello from pie_user via libcapc cap_write\n";
    if (cap_write(1, msg.ptr, msg.len) < 0) {
        _ = cap_errno;
    }

    if (cap_malloc(64)) |buf| {
        buf[0] = 0xAA;
        buf[63] = 0x55;
        _ = cap_write(1, "cap_malloc ok\n".ptr, "cap_malloc ok\n".len);
        cap_free(buf);
        _ = cap_write(1, "cap_free ok\n".ptr, "cap_free ok\n".len);
    } else {
        _ = cap_write(2, "cap_malloc failed\n".ptr, "cap_malloc failed\n".len);
        _ = cap_errno;
    }
    relocated_ptr.* = 0x1122_3344_5566_7788;
    while (true) {
        asm volatile ("pause");
    }
}
