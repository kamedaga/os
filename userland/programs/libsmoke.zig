const syscall_log: u64 = 0x9;

fn syscall3(nr: u64, arg0: u64, arg1: u64, arg2: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
          [arg2] "{rdx}" (arg2),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn userLog(message: []const u8) void {
    _ = syscall3(syscall_log, @intFromPtr(message.ptr), message.len, 0);
}

pub export fn smoke_hello() void {
    userLog("libsmoke: smoke_hello\n");
}

pub export fn _start() noreturn {
    userLog("libsmoke: _start (not for direct exec)\n");
    while (true) asm volatile ("pause");
}
