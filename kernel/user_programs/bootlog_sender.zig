const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_send_cap: u64 = 0x6;
const syscall_log: u64 = 0x9;

const syscall_ok: u64 = 0;

const shared_page_va: usize = 0x3C00_3000;
const ipc_page_va: usize = 0x2000_B000;

const shared_magic: u64 = 0x4D534852; // "MSHR"
const shared_header_bytes: usize = 128;
const shared_log_max_bytes: usize = 4096 - shared_header_bytes;

const endpoint_to_process1: u64 = 0x11;
const bootlog_ipc_magic: u64 = 0x424C4F47; // "BLOG"
const bootlog_ipc_header_bytes: usize = 16;

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

fn sendCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_send_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
        : .{ .memory = true });
}

pub export fn _start() noreturn {
    _ = userLog("BootLogSender: started\n");
    const shared_words: [*]const volatile u64 = @ptrFromInt(shared_page_va);
    if (shared_words[0] != shared_magic) {
        _ = userLog("BootLogSender: shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }

    var text_len: usize = @intCast(shared_words[9]);
    if (text_len > shared_log_max_bytes) text_len = shared_log_max_bytes;
    if (text_len == 0) {
        _ = userLog("BootLogSender: no boot log payload\n");
        while (true) asm volatile ("pause");
    }

    const page_paddr = allocPage();
    if (page_paddr < 0x1000) {
        _ = userLog("BootLogSender: alloc page failed\n");
        while (true) asm volatile ("pause");
    }
    if (mapPage(ipc_page_va, page_paddr, true) != syscall_ok) {
        _ = userLog("BootLogSender: map page failed\n");
        while (true) asm volatile ("pause");
    }

    const msg_words: [*]volatile u64 = @ptrFromInt(ipc_page_va);
    const msg_bytes: [*]volatile u8 = @ptrFromInt(ipc_page_va);
    const shared_bytes: [*]const volatile u8 = @ptrFromInt(shared_page_va);
    msg_words[0] = bootlog_ipc_magic;
    msg_words[1] = @intCast(text_len);
    var i: usize = 0;
    while (i < text_len) : (i += 1) {
        msg_bytes[bootlog_ipc_header_bytes + i] = shared_bytes[shared_header_bytes + i];
    }

    if (sendCap(page_paddr, endpoint_to_process1) != syscall_ok) {
        _ = userLog("BootLogSender: send_cap failed\n");
        while (true) asm volatile ("pause");
    }
    _ = userLog("BootLogSender: boot log page sent\n");
    while (true) asm volatile ("pause");
}
