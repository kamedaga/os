const std = @import("std");
const support = @import("support_root");

const gpu_protocol = support.gpu_protocol;
const process_abi = support.process_abi;
const process_args_env_bootstrap_abi = support.process_args_env_bootstrap_abi;
const service_registry_abi = support.service_registry_abi;

const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_log: u64 = 0x9;
const syscall_wait_event: u64 = 0x17;
const syscall_install_endpoint: u64 = 0x26;
const syscall_share_cap: u64 = 0x2B;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_get_tick_count: u64 = 0x2D;
const syscall_get_process_slot: u64 = 0x2E;
const syscall_ipc_call_reply_recv: u64 = 0x40;

const iterations: u64 = 100;
const rounds: usize = 5;
const contention_burst: u64 = 4;
const bench_server_endpoint_id: u64 = 0x8A0;
const reply_endpoint_base: u64 = 0x880;
const ipc_call_flag_signal_only: u64 = 0x2;
const setup_page_va: u64 = 0x3C40_0000;
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
    var buf: [192]u8 = undefined;
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

fn readTscEnd() u64 {
    var lo: u32 = 0;
    var hi: u32 = 0;
    asm volatile ("rdtscp"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
        :
        : .{ .rcx = true });
    asm volatile ("lfence");
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

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return syscall3(syscall_map_page, va, paddr, @intFromBool(writable));
}

fn installEndpoint(endpoint_id: u64, target_process_slot: u64) u64 {
    return syscall3(syscall_install_endpoint, 0, endpoint_id, target_process_slot);
}

fn spinDelay(count: u64) void {
    var i: u64 = 0;
    while (i < count) : (i += 1) asm volatile ("pause");
}

fn waitForGpuService() service_registry_abi.ServiceEntry {
    var tries: u64 = 0;
    while (tries < 1_000_000) : (tries += 1) {
        if (service_registry_abi.findService(process_abi.service_registry_shadow_va, .gpu)) |entry| {
            if (entry.endpoint_id != 0 and entry.process_slot != 0) return entry;
        }
        spinDelay(128);
    }
    return .{ .kind = 0, .process_slot = 0, .endpoint_id = gpu_protocol.endpoint_id, .flags = 0 };
}

fn argSlice(index: usize) ?[]const u8 {
    const page: *const volatile process_args_env_bootstrap_abi.Page = @ptrFromInt(process_args_env_bootstrap_abi.target_va);
    if (page.header.magic != process_args_env_bootstrap_abi.magic or page.header.version != process_args_env_bootstrap_abi.version) return null;
    if (index >= page.header.arg_count or index >= process_args_env_bootstrap_abi.max_args) return null;
    const entry = page.args[index];
    if (entry.len == 0) return null;
    const off: usize = entry.offset;
    const len: usize = entry.len;
    if (off + len > process_args_env_bootstrap_abi.data_bytes) return null;
    const data: [*]const u8 = @ptrFromInt(@intFromPtr(page) + @offsetOf(process_args_env_bootstrap_abi.Page, "data") + off);
    return data[0..len];
}

fn parseU64(text: []const u8) ?u64 {
    if (text.len == 0) return null;
    var value: u64 = 0;
    for (text) |ch| {
        if (ch < '0' or ch > '9') return null;
        value = value * 10 + @as(u64, ch - '0');
    }
    return value;
}

fn resolveServerBinding() service_registry_abi.ServiceEntry {
    if (argSlice(1)) |slot_text| {
        if (parseU64(slot_text)) |slot| {
            if (slot != 0) {
                return .{
                    .kind = 0,
                    .process_slot = slot,
                    .endpoint_id = bench_server_endpoint_id,
                    .flags = service_registry_abi.service_flag_process_slot_compat,
                };
            }
        }
    }
    return waitForGpuService();
}

fn sendSetupPage(server_endpoint_id: u64, self_slot: u64) bool {
    const paddr = syscall0(syscall_alloc_page);
    if (paddr < 0x1000) {
        logFmt("IpcPairBenchClient: setup alloc failed status=0x{X}\n", .{paddr});
        return false;
    }
    const map_status = mapPage(setup_page_va, paddr, true);
    if (map_status != 0) {
        logFmt("IpcPairBenchClient: setup map failed status=0x{X} paddr=0x{X}\n", .{ map_status, paddr });
        return false;
    }
    const words: [*]volatile u64 = @ptrFromInt(setup_page_va);
    words[0] = self_slot;
    words[1] = reply_endpoint_base + self_slot;
    const share_status = syscall2(syscall_share_cap, paddr, server_endpoint_id);
    if (share_status != 0) {
        logFmt("IpcPairBenchClient: setup share failed status=0x{X}\n", .{share_status});
        return false;
    }
    return true;
}

fn measureLoopBaseline() u64 {
    var acc: u64 = 0;
    const start = readTscStart();
    var i: u64 = 0;
    while (i < iterations) : (i += 1) {
        asm volatile (""
            :
            : [value] "{rax}" (i),
            : .{ .memory = true });
        acc +%= i;
    }
    const end = readTscEnd();
    if (acc == 0xffff_ffff_ffff_ffff) _ = userLog("IpcPairBenchClient: impossible baseline checksum\n");
    return end - start;
}

fn measureSignalWaitPair(server_endpoint_id: u64, checksum: *u64) u64 {
    var acc: u64 = 0;
    const start = readTscStart();
    var i: u64 = 0;
    while (i < iterations) : (i += 1) {
        acc +%= syscall1(syscall_signal_endpoint, server_endpoint_id);
        acc +%= syscall2(syscall_wait_event, 0, 0);
    }
    const end = readTscEnd();
    checksum.* +%= acc;
    return end - start;
}

fn measureCallReplyRecv(server_endpoint_id: u64, checksum: *u64) u64 {
    var acc: u64 = 0;
    const start = readTscStart();
    var i: u64 = 0;
    while (i < iterations) : (i += 1) {
        acc +%= syscall3(syscall_ipc_call_reply_recv, 0, server_endpoint_id, ipc_call_flag_signal_only);
    }
    const end = readTscEnd();
    checksum.* +%= acc;
    return end - start;
}

fn measureContendedSignalQueue(server_endpoint_id: u64, checksum: *u64) u64 {
    var acc: u64 = 0;
    const start = readTscStart();
    var i: u64 = 0;
    while (i < iterations) : (i += 1) {
        var send_index: u64 = 0;
        while (send_index < contention_burst) : (send_index += 1) {
            acc +%= syscall1(syscall_signal_endpoint, server_endpoint_id);
        }
        var recv_index: u64 = 0;
        while (recv_index < contention_burst) : (recv_index += 1) {
            acc +%= syscall2(syscall_wait_event, 0, 0);
        }
    }
    const end = readTscEnd();
    checksum.* +%= acc;
    return (end - start) / contention_burst;
}

const SplitResult = struct {
    call_min: u64 = std.math.maxInt(u64),
    call_avg: u64 = 0,
    reply_min: u64 = std.math.maxInt(u64),
    reply_avg: u64 = 0,
    total_min: u64 = std.math.maxInt(u64),
    total_avg: u64 = 0,
};

fn runCallReplySplitCase(server_endpoint_id: u64, checksum: *u64) SplitResult {
    var result = SplitResult{};
    var call_sum: u64 = 0;
    var reply_sum: u64 = 0;
    var total_sum: u64 = 0;
    var samples: u64 = 0;
    var acc: u64 = 0;

    _ = ipcCallReplyRecvMsg4(server_endpoint_id, readTscStart(), 0, 0, timed_split_request_magic);

    var i: u64 = 0;
    while (i < iterations) : (i += 1) {
        const start = readTscStart();
        const reply = ipcCallReplyRecvMsg4(server_endpoint_id, start, 0, i, timed_split_request_magic);
        const end = readTscEnd();
        if (reply.status != 0 or reply.mr3 != timed_split_reply_magic) {
            logFmt("IpcPairBenchClient: split validate failed status=0x{X} mr3=0x{X}\n", .{ reply.status, reply.mr3 });
            continue;
        }
        const call_cycles = reply.mr1;
        const reply_cycles = if (end > reply.mr0) end - reply.mr0 else 0;
        const total_cycles = end - start;
        call_sum +%= call_cycles;
        reply_sum +%= reply_cycles;
        total_sum +%= total_cycles;
        result.call_min = @min(result.call_min, call_cycles);
        result.reply_min = @min(result.reply_min, reply_cycles);
        result.total_min = @min(result.total_min, total_cycles);
        samples +%= 1;
        acc +%= call_cycles +% reply_cycles +% total_cycles +% reply.mr2;
    }

    if (samples != 0) {
        result.call_avg = call_sum / samples;
        result.reply_avg = reply_sum / samples;
        result.total_avg = total_sum / samples;
    }
    checksum.* +%= acc;
    logFmt("IpcPairBenchClient: cross_call_reply_split call_min={d} call_avg={d} reply_min={d} reply_avg={d} total_min={d} total_avg={d} cycles\n", .{
        result.call_min,
        result.call_avg,
        result.reply_min,
        result.reply_avg,
        result.total_min,
        result.total_avg,
    });
    return result;
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

fn validateMsgReply(prev0: u64, prev1: u64, prev2: u64, prev3: u64, reply: IpcMsgReply) bool {
    return reply.status == 0 and
        reply.mr0 == replyTransform0(prev0, prev1, prev2, prev3) and
        reply.mr1 == replyTransform1(prev0, prev1, prev2, prev3) and
        reply.mr2 == replyTransform2(prev0, prev1, prev2, prev3) and
        reply.mr3 == replyTransform3(prev0, prev1, prev2, prev3);
}

fn measureCallReplyRecvMsg4(server_endpoint_id: u64, checksum: *u64) u64 {
    var prev0: u64 = 0xC001_CAFE_0000_0001;
    var prev1: u64 = 0xC001_CAFE_0000_0002;
    var prev2: u64 = 0xC001_CAFE_0000_0003;
    var prev3: u64 = 0xC001_CAFE_0000_0004;
    _ = ipcCallReplyRecvMsg4(server_endpoint_id, prev0, prev1, prev2, prev3);

    const check = ipcCallReplyRecvMsg4(server_endpoint_id, 0x10, 0x20, 0x30, 0x40);
    if (!validateMsgReply(0x10, 0x20, 0x30, 0x40, check)) {
        logFmt("IpcPairBenchClient: msg4 validate failed status=0x{X} mr0=0x{X} mr1=0x{X} mr2=0x{X} mr3=0x{X}\n", .{
            check.status,
            check.mr0,
            check.mr1,
            check.mr2,
            check.mr3,
        });
    }
    prev0 = 0x10;
    prev1 = 0x20;
    prev2 = 0x30;
    prev3 = 0x40;

    var acc: u64 = check.mr0 ^ check.mr1 ^ check.mr2 ^ check.mr3;
    const start = readTscStart();
    var i: u64 = 0;
    while (i < iterations) : (i += 1) {
        const cur0 = 0x1000_0000_0000_0000 | i;
        const cur1 = 0x2000_0000_0000_0000 | (i << 1);
        const cur2 = 0x3000_0000_0000_0000 | (i << 2);
        const cur3 = 0x4000_0000_0000_0000 | (i << 3);
        const reply = ipcCallReplyRecvMsg4(server_endpoint_id, cur0, cur1, cur2, cur3);
        acc +%= reply.status +% reply.mr0 +% reply.mr1 +% reply.mr2 +% reply.mr3;
        prev0 = cur0;
        prev1 = cur1;
        prev2 = cur2;
        prev3 = cur3;
    }
    const end = readTscEnd();
    checksum.* +%= acc +% prev0 +% prev1 +% prev2 +% prev3;
    return end - start;
}

const Result = struct {
    raw_min: u64 = std.math.maxInt(u64),
    net_min: u64 = std.math.maxInt(u64),
    raw_avg: u64 = 0,
    net_avg: u64 = 0,
};

fn runBlockingCase(name: []const u8, server_endpoint_id: u64, checksum: *u64, comptime measure: fn (u64, *u64) u64) Result {
    var result = Result{};
    var raw_sum: u64 = 0;
    var net_sum: u64 = 0;

    var round: usize = 0;
    while (round < rounds) : (round += 1) {
        const baseline = measureLoopBaseline();
        const raw = measure(server_endpoint_id, checksum);
        const net = if (raw > baseline) raw - baseline else 0;
        raw_sum +%= raw / iterations;
        net_sum +%= net / iterations;
        result.raw_min = @min(result.raw_min, raw / iterations);
        result.net_min = @min(result.net_min, net / iterations);
    }

    result.raw_avg = raw_sum / rounds;
    result.net_avg = net_sum / rounds;
    logFmt("IpcPairBenchClient: {s} raw_min={d} raw_avg={d} net_min={d} net_avg={d} cycles\n", .{
        name,
        result.raw_min,
        result.raw_avg,
        result.net_min,
        result.net_avg,
    });
    return result;
}

pub export fn _start() noreturn {
    const self_slot = syscall0(syscall_get_process_slot);
    const reply_endpoint_id = reply_endpoint_base + self_slot;
    const service = resolveServerBinding();
    logFmt("IpcPairBenchClient: started slot={d} reply_endpoint=0x{X} server_slot={d} server_endpoint=0x{X}\n", .{
        self_slot,
        reply_endpoint_id,
        service.process_slot,
        service.endpoint_id,
    });
    logFmt("IpcPairBenchClient: iterations={d} rounds={d}\n", .{ iterations, rounds });

    var checksum: u64 = 0;
    const endpoint_status = installEndpoint(service.endpoint_id, service.process_slot);
    logFmt("IpcPairBenchClient: install_server_endpoint=0x{X}\n", .{endpoint_status});
    if (endpoint_status != 0) {
        _ = userLog("IpcPairBenchClient: server endpoint install failed\n");
        while (true) asm volatile ("pause");
    }
    if (!sendSetupPage(service.endpoint_id, self_slot)) {
        _ = userLog("IpcPairBenchClient: setup failed\n");
        while (true) asm volatile ("pause");
    }
    _ = syscall2(syscall_wait_event, 0, 0);

    _ = runBlockingCase("cross_signal_wait_pair", service.endpoint_id, &checksum, measureSignalWaitPair);
    _ = runBlockingCase("cross_contended_signal_queue_burst4", service.endpoint_id, &checksum, measureContendedSignalQueue);
    _ = runBlockingCase("cross_call_reply_recv_signal", service.endpoint_id, &checksum, measureCallReplyRecv);
    _ = runCallReplySplitCase(service.endpoint_id, &checksum);
    _ = runBlockingCase("cross_call_reply_recv_msg4", service.endpoint_id, &checksum, measureCallReplyRecvMsg4);
    logFmt("IpcPairBenchClient: tick={d} checksum=0x{X}\n", .{ syscall0(syscall_get_tick_count), checksum });
    _ = userLog("IpcPairBenchClient: done\n");

    while (true) asm volatile ("pause");
}
