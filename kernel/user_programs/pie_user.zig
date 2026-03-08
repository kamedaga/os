pub var relocated_target: u64 = 0;
pub var relocated_ptr: *u64 = &relocated_target;

const syscall_log: u64 = 0x9;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

pub export fn _start() noreturn {
    _ = userLog("hello from pie_user via syscall_log\n");
    relocated_ptr.* = 0x1122_3344_5566_7788;
    while (true) {
        asm volatile ("pause");
    }
}
