const std = @import("std");

const syscall_log: u64 = 0x9;
const syscall_map_page: u64 = 0x2;
const syscall_wait_event: u64 = 0x17;
const syscall_accept_cap_transfer: u64 = 0x2A;
const syscall_install_endpoint: u64 = 0x26;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_get_process_slot: u64 = 0x2E;
const syscall_ipc_call_reply_recv: u64 = 0x40;

const reply_endpoint_base: u64 = 0x880;
const transfer_id_min: u64 = 0x1000;
const setup_page_va: u64 = 0x3C40_0000;
const ipc_call_flag_signal_only: u64 = 0x2;
const timed_split_request_magic: u64 = 0x5453_504C_4954_0001;
const timed_split_reply_magic: u64 = 0x5453_504C_4954_8001;

const IpcMsgReply = extern struct {
    status: u64 = 0,
    mr0: u64 = 0,
    mr1: u64 = 0,
    mr2: u64 = 0,
    mr3: u64 = 0,
};

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn logFmt(comptime fmt: []const u8, args: anytype) void {
    var buf: [160]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, fmt, args) catch return;
    _ = userLog(msg);
}

fn readTscStart() u64 {
    var lo: u32 = 0;
    var hi: u32 = 0;
    asm volatile ("lfence");
    asm volatile ("rdtsc"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
    );
    return (@as(u64, hi) << 32) | @as(u64, lo);
}

fn syscall0(nr: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall1(nr: u64, arg0: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return syscall3(syscall_map_page, va, paddr, @intFromBool(writable));
}

fn syscall2(nr: u64, arg0: u64, arg1: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall3(nr: u64, arg0: u64, arg1: u64, arg2: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
          [arg2] "{rdx}" (arg2),
        : .{ .rcx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn ipcCallReplyRecvMsg4(endpoint_id: u64, mr0: u64, mr1: u64, mr2: u64, mr3: u64) IpcMsgReply {
    var status: u64 = undefined;
    var out0: u64 = undefined;
    var out1: u64 = undefined;
    var out2: u64 = undefined;
    var out3: u64 = undefined;
    asm volatile (
        \\syscall
        : [status] "={rax}" (status),
          [out0] "={rdi}" (out0),
          [out1] "={rsi}" (out1),
          [out2] "={rdx}" (out2),
          [out3] "={r8}" (out3),
        : [nr] "{rax}" (syscall_ipc_call_reply_recv),
          [arg0] "{rdi}" (mr0),
          [arg1] "{rsi}" (endpoint_id),
          [arg2] "{rdx}" (ipc_call_flag_signal_only),
          [arg3] "{r8}" (mr1),
          [arg4] "{r9}" (mr2),
          [arg5] "{r10}" (mr3),
        : .{ .rcx = true, .r11 = true, .memory = true });
    return .{ .status = status, .mr0 = out0, .mr1 = out1, .mr2 = out2, .mr3 = out3 };
}

fn installReplyEndpoint(client_slot: u64) u64 {
    if (client_slot == 0 or client_slot >= 32) return 1;
    return syscall3(syscall_install_endpoint, 0, reply_endpoint_base + client_slot, client_slot);
}

fn signalReplyEndpoint(client_slot: u64) void {
    if (client_slot == 0) return;
    _ = syscall1(syscall_signal_endpoint, reply_endpoint_base + client_slot);
}

fn replyAndRecvMsg4(client_slot: u64, mr0: u64, mr1: u64, mr2: u64, mr3: u64) IpcMsgReply {
    if (client_slot == 0) {
        return .{ .status = syscall2(syscall_wait_event, 1, 0) };
    }
    return ipcCallReplyRecvMsg4(reply_endpoint_base + client_slot, mr0, mr1, mr2, mr3);
}

fn replyTransform0(mr0: u64, mr1: u64, mr2: u64, mr3: u64) u64 {
    return mr0 ^ mr1 ^ (mr2 << 1) ^ mr3 ^ 0x1111_2222_3333_4444;
}

fn replyTransform1(mr0: u64, mr1: u64, mr2: u64, mr3: u64) u64 {
    return mr1 +% mr0 +% (mr2 >> 1) +% mr3 +% 0x0102_0304_0506_0708;
}

fn replyTransform2(mr0: u64, mr1: u64, mr2: u64, mr3: u64) u64 {
    return mr2 ^ mr1 ^ mr0 ^ (mr3 << 2) ^ 0xA5A5_A5A5_A5A5_A5A5;
}

fn replyTransform3(mr0: u64, mr1: u64, mr2: u64, mr3: u64) u64 {
    return mr3 +% mr2 +% (mr1 << 1) +% (mr0 >> 1) +% 1;
}

fn acceptSetupPage(transfer_id: u64) ?u64 {
    const paddr = syscall1(syscall_accept_cap_transfer, transfer_id);
    if (paddr < transfer_id_min) return null;
    if (mapPage(setup_page_va, paddr, false) != 0) return null;
    const words: [*]volatile u64 = @ptrFromInt(setup_page_va);
    return words[0];
}

pub export fn _start() noreturn {
    const self_slot = syscall0(syscall_get_process_slot);
    logFmt("IpcPairBenchServer: started slot={d} reply_base=0x{X}\n", .{
        self_slot,
        reply_endpoint_base,
    });

    var client_slot: u64 = 0;
    var wake_count: u64 = 0;
    var reply_mr0: u64 = 0xABCD_0000_0000_0000;
    var reply_mr1: u64 = 0xABCD_0000_0000_0001;
    var reply_mr2: u64 = 0xABCD_0000_0000_0002;
    var reply_mr3: u64 = 0xABCD_0000_0000_0003;
    var reply_is_timed_split = false;
    var event = syscall2(syscall_wait_event, 1, 0);
    while (true) {
        if (event >= transfer_id_min) {
            if (acceptSetupPage(event)) |slot| {
                const install_status = installReplyEndpoint(slot);
                if (install_status == 0) {
                    client_slot = slot;
                    logFmt("IpcPairBenchServer: client_slot={d} reply_endpoint=0x{X}\n", .{
                        client_slot,
                        reply_endpoint_base + client_slot,
                    });
                    signalReplyEndpoint(client_slot);
                } else {
                    logFmt("IpcPairBenchServer: reply install failed slot={d} status=0x{X}\n", .{ slot, install_status });
                }
            }
            event = syscall2(syscall_wait_event, 1, 0);
            continue;
        }
        wake_count +%= 1;
        if (reply_is_timed_split) {
            reply_mr0 = readTscStart();
        }
        const received = replyAndRecvMsg4(client_slot, reply_mr0, reply_mr1, reply_mr2, reply_mr3);
        const server_recv_tsc = readTscStart();
        event = received.status;
        if (received.status == 0) {
            if (received.mr3 == timed_split_request_magic) {
                reply_is_timed_split = true;
                reply_mr0 = 0;
                reply_mr1 = server_recv_tsc - received.mr0;
                reply_mr2 = received.mr2;
                reply_mr3 = timed_split_reply_magic;
            } else {
                reply_is_timed_split = false;
                reply_mr0 = replyTransform0(received.mr0, received.mr1, received.mr2, received.mr3);
                reply_mr1 = replyTransform1(received.mr0, received.mr1, received.mr2, received.mr3);
                reply_mr2 = replyTransform2(received.mr0, received.mr1, received.mr2, received.mr3);
                reply_mr3 = replyTransform3(received.mr0, received.mr1, received.mr2, received.mr3);
            }
        }
        if ((wake_count & 0x3fff) == 0) {
            logFmt("IpcPairBenchServer: wake_count={d}\n", .{wake_count});
        }
    }
}
