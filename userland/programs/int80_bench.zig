const std = @import("std");

const syscall_log: u64 = 0x9;
const syscall_recv_cap: u64 = 0x0A;
const syscall_wait_event: u64 = 0x17;
const syscall_install_endpoint: u64 = 0x26;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_get_tick_count: u64 = 0x2D;
const syscall_get_process_slot: u64 = 0x2E;
const syscall_ipc_call_reply_recv: u64 = 0x40;

const iterations: u64 = 10_000;
const rounds: usize = 9;
const single_samples: usize = 128;
const fast_invalid_syscall: u64 = 0xFFFF;
const slow_unknown_syscall: u64 = 0xFFFE;
const bench_endpoint_id: u64 = 0x777;
const ipc_call_flag_signal_only: u64 = 0x2;

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
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall0Syscall(nr: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall1Syscall(nr: u64, arg0: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall2Syscall(nr: u64, arg0: u64, arg1: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall3Syscall(nr: u64, arg0: u64, arg1: u64, arg2: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
          [arg2] "{rdx}" (arg2),
        : .{ .rcx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

const Entry = enum {
    int80,
    syscall,
};

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
    if (acc == 0xFFFF_FFFF_FFFF_FFFF) _ = userLog("Int80Bench: impossible baseline checksum\n");
    return end - start;
}

fn measureSyscall(entry: Entry, nr: u64, checksum: *u64) u64 {
    var acc: u64 = 0;
    const start = readTscStart();
    var i: u64 = 0;
    while (i < iterations) : (i += 1) {
        acc +%= switch (entry) {
            .int80 => syscall0(nr),
            .syscall => syscall0Syscall(nr),
        };
    }
    const end = readTscEnd();
    checksum.* +%= acc;
    return end - start;
}

fn measureSyscall2(nr: u64, arg0: u64, arg1: u64, checksum: *u64) u64 {
    var acc: u64 = 0;
    const start = readTscStart();
    var i: u64 = 0;
    while (i < iterations) : (i += 1) {
        acc +%= syscall2Syscall(nr, arg0, arg1);
    }
    const end = readTscEnd();
    checksum.* +%= acc;
    return end - start;
}

fn measureSyscall3(nr: u64, arg0: u64, arg1: u64, arg2: u64, checksum: *u64) u64 {
    var acc: u64 = 0;
    const start = readTscStart();
    var i: u64 = 0;
    while (i < iterations) : (i += 1) {
        acc +%= syscall3Syscall(nr, arg0, arg1, arg2);
    }
    const end = readTscEnd();
    checksum.* +%= acc;
    return end - start;
}

fn measureSignalWaitPair(endpoint_id: u64, checksum: *u64) u64 {
    var acc: u64 = 0;
    const start = readTscStart();
    var i: u64 = 0;
    while (i < iterations) : (i += 1) {
        acc +%= syscall1Syscall(syscall_signal_endpoint, endpoint_id);
        acc +%= syscall2Syscall(syscall_wait_event, 0, 0);
    }
    const end = readTscEnd();
    checksum.* +%= acc;
    return end - start;
}

const Result = struct {
    raw_min: u64 = std.math.maxInt(u64),
    net_min: u64 = std.math.maxInt(u64),
    raw_avg: u64 = 0,
    net_avg: u64 = 0,
};

fn runCase(entry: Entry, name: []const u8, nr: u64, checksum: *u64) Result {
    var raw_sum: u64 = 0;
    var net_sum: u64 = 0;
    var result = Result{};

    var round: usize = 0;
    while (round < rounds) : (round += 1) {
        const baseline = measureLoopBaseline();
        const raw = measureSyscall(entry, nr, checksum);
        const net = if (raw > baseline) raw - baseline else 0;
        raw_sum +%= raw / iterations;
        net_sum +%= net / iterations;
        result.raw_min = @min(result.raw_min, raw / iterations);
        result.net_min = @min(result.net_min, net / iterations);
    }

    result.raw_avg = raw_sum / rounds;
    result.net_avg = net_sum / rounds;
    logFmt("Int80Bench: {s} raw_min={d} raw_avg={d} net_min={d} net_avg={d} cycles\n", .{
        name,
        result.raw_min,
        result.raw_avg,
        result.net_min,
        result.net_avg,
    });
    return result;
}

fn runCase2(name: []const u8, nr: u64, arg0: u64, arg1: u64, checksum: *u64) Result {
    var raw_sum: u64 = 0;
    var net_sum: u64 = 0;
    var result = Result{};

    var round: usize = 0;
    while (round < rounds) : (round += 1) {
        const baseline = measureLoopBaseline();
        const raw = measureSyscall2(nr, arg0, arg1, checksum);
        const net = if (raw > baseline) raw - baseline else 0;
        raw_sum +%= raw / iterations;
        net_sum +%= net / iterations;
        result.raw_min = @min(result.raw_min, raw / iterations);
        result.net_min = @min(result.net_min, net / iterations);
    }

    result.raw_avg = raw_sum / rounds;
    result.net_avg = net_sum / rounds;
    logFmt("Int80Bench: {s} raw_min={d} raw_avg={d} net_min={d} net_avg={d} cycles\n", .{
        name,
        result.raw_min,
        result.raw_avg,
        result.net_min,
        result.net_avg,
    });
    return result;
}

fn runCase3(name: []const u8, nr: u64, arg0: u64, arg1: u64, arg2: u64, checksum: *u64) Result {
    var raw_sum: u64 = 0;
    var net_sum: u64 = 0;
    var result = Result{};

    var round: usize = 0;
    while (round < rounds) : (round += 1) {
        const baseline = measureLoopBaseline();
        const raw = measureSyscall3(nr, arg0, arg1, arg2, checksum);
        const net = if (raw > baseline) raw - baseline else 0;
        raw_sum +%= raw / iterations;
        net_sum +%= net / iterations;
        result.raw_min = @min(result.raw_min, raw / iterations);
        result.net_min = @min(result.net_min, net / iterations);
    }

    result.raw_avg = raw_sum / rounds;
    result.net_avg = net_sum / rounds;
    logFmt("Int80Bench: {s} raw_min={d} raw_avg={d} net_min={d} net_avg={d} cycles\n", .{
        name,
        result.raw_min,
        result.raw_avg,
        result.net_min,
        result.net_avg,
    });
    return result;
}

fn runSignalWaitPair(name: []const u8, endpoint_id: u64, checksum: *u64) Result {
    var raw_sum: u64 = 0;
    var net_sum: u64 = 0;
    var result = Result{};

    var round: usize = 0;
    while (round < rounds) : (round += 1) {
        const baseline = measureLoopBaseline();
        const raw = measureSignalWaitPair(endpoint_id, checksum);
        const net = if (raw > baseline) raw - baseline else 0;
        raw_sum +%= raw / iterations;
        net_sum +%= net / iterations;
        result.raw_min = @min(result.raw_min, raw / iterations);
        result.net_min = @min(result.net_min, net / iterations);
    }

    result.raw_avg = raw_sum / rounds;
    result.net_avg = net_sum / rounds;
    logFmt("Int80Bench: {s} raw_min={d} raw_avg={d} net_min={d} net_avg={d} cycles\n", .{
        name,
        result.raw_min,
        result.raw_avg,
        result.net_min,
        result.net_avg,
    });
    return result;
}

fn runTscOverhead() void {
    var min: u64 = std.math.maxInt(u64);
    var sum: u64 = 0;
    var sample: usize = 0;
    while (sample < single_samples) : (sample += 1) {
        const start = readTscStart();
        const end = readTscEnd();
        const delta = end - start;
        min = @min(min, delta);
        sum +%= delta;
    }
    logFmt("Int80Bench: tsc_pair_overhead min={d} avg={d} cycles samples={d}\n", .{
        min,
        sum / single_samples,
        single_samples,
    });
}

fn runSingleCase(entry: Entry, name: []const u8, nr: u64, checksum: *u64) void {
    var min: u64 = std.math.maxInt(u64);
    var max: u64 = 0;
    var sum: u64 = 0;

    var sample: usize = 0;
    while (sample < single_samples) : (sample += 1) {
        const start = readTscStart();
        const ret = switch (entry) {
            .int80 => syscall0(nr),
            .syscall => syscall0Syscall(nr),
        };
        const end = readTscEnd();
        const delta = end - start;
        min = @min(min, delta);
        max = @max(max, delta);
        sum +%= delta;
        checksum.* +%= ret;
    }

    logFmt("Int80Bench: single {s} min={d} avg={d} max={d} cycles samples={d}\n", .{
        name,
        min,
        sum / single_samples,
        max,
        single_samples,
    });
}

pub export fn _start() noreturn {
    _ = userLog("Int80Bench: started\n");
    logFmt("Int80Bench: iterations={d} rounds={d}\n", .{ iterations, rounds });

    var checksum: u64 = 0;
    const self_slot = syscall0Syscall(syscall_get_process_slot);
    const install_status = syscall3Syscall(syscall_install_endpoint, 0, bench_endpoint_id, self_slot);
    logFmt("Int80Bench: self_slot={d} install_endpoint=0x{X}\n", .{ self_slot, install_status });

    runTscOverhead();
    _ = runCase(.int80, "int80 fast_invalid_0xffff", fast_invalid_syscall, &checksum);
    _ = runCase(.int80, "int80 slow_unknown_0xfffe", slow_unknown_syscall, &checksum);
    _ = runCase(.int80, "int80 get_tick_count", syscall_get_tick_count, &checksum);
    _ = runCase(.int80, "int80 get_process_slot", syscall_get_process_slot, &checksum);
    _ = runCase(.syscall, "syscall fast_invalid_0xffff", fast_invalid_syscall, &checksum);
    _ = runCase(.syscall, "syscall slow_unknown_0xfffe", slow_unknown_syscall, &checksum);
    _ = runCase(.syscall, "syscall ipc_recv_empty", syscall_recv_cap, &checksum);
    _ = runCase2("syscall ipc_signal_missing", syscall_signal_endpoint, 0, 0, &checksum);
    if (install_status == 0) {
        _ = runSignalWaitPair("syscall ipc_signal_wait_pair_self", bench_endpoint_id, &checksum);
        _ = runCase3("syscall ipc_call_reply_recv_self_signal", syscall_ipc_call_reply_recv, 0, bench_endpoint_id, ipc_call_flag_signal_only, &checksum);
    }
    _ = runCase(.syscall, "syscall get_tick_count", syscall_get_tick_count, &checksum);
    _ = runCase(.syscall, "syscall get_process_slot", syscall_get_process_slot, &checksum);
    runSingleCase(.syscall, "syscall fast_invalid_0xffff", fast_invalid_syscall, &checksum);
    runSingleCase(.syscall, "syscall slow_unknown_0xfffe", slow_unknown_syscall, &checksum);
    runSingleCase(.syscall, "syscall get_tick_count", syscall_get_tick_count, &checksum);
    runSingleCase(.syscall, "syscall get_process_slot", syscall_get_process_slot, &checksum);
    logFmt("Int80Bench: checksum=0x{X}\n", .{checksum});
    _ = userLog("Int80Bench: done\n");

    while (true) asm volatile ("pause");
}
