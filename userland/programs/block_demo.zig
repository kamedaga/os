const std = @import("std");
const block_client = @import("support_root").block_client;
const user_vm = @import("support_root").user_vm;

const syscall_log: u64 = 0x9;
const syscall_get_tick_count: u64 = 0x2D;
const syscall_get_process_slot: u64 = 0x2E;
const demo_magic: u64 = 0x424C_4B44_454D_4F31; // "BLKDEMO1"

var block_storage: [4096]u8 align(16) = [_]u8{0} ** 4096;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn userLogHex(label: []const u8, value: u64) void {
    var buf: [96]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, "{s}0x{X}\n", .{ label, value }) catch return;
    _ = userLog(msg);
}

fn userLogNum(label: []const u8, value: u64) void {
    var buf: [96]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, "{s}{d}\n", .{ label, value }) catch return;
    _ = userLog(msg);
}

fn getTickCount() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_get_tick_count),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn getProcessSlot() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_get_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn userLogTick(label: []const u8, start_tick: u64, tick: u64) void {
    var buf: [128]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, "{s} tick={d} delta={d}\n", .{ label, tick, tick - start_tick }) catch return;
    _ = userLog(msg);
}

pub export fn _start() noreturn {
    _ = userLog("BlockDemo: started\n");
    const start_tick = getTickCount();
    userLogTick("BlockDemo: start", start_tick, start_tick);
    const process_slot = getProcessSlot();
    if (process_slot == 0) {
        _ = userLog("BlockDemo: process slot unavailable\n");
        while (true) asm volatile ("pause");
    }
    const process_slot_tick = getTickCount();
    userLogTick("BlockDemo: got process slot", start_tick, process_slot_tick);

    const ipc_va = user_vm.reservePages(2) orelse {
        _ = userLog("BlockDemo: IPC VA reserve failed\n");
        while (true) asm volatile ("pause");
    };
    var client = block_client.Client.connectFromServiceRegistry(@intCast(ipc_va), @intCast(ipc_va + user_vm.page_bytes), process_slot) catch |err| {
        _ = userLog("BlockDemo: block connect failed\n");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        while (true) asm volatile ("pause");
    };
    const connect_tick = getTickCount();
    _ = userLog("BlockDemo: block connect ok\n");
    userLogTick("BlockDemo: block connect", start_tick, connect_tick);

    const identify = client.identify() catch |err| {
        _ = userLog("BlockDemo: identify failed\n");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        while (true) asm volatile ("pause");
    };
    const identify_tick = getTickCount();
    userLogTick("BlockDemo: identify", start_tick, identify_tick);

    if (identify.block_size == 0 or identify.block_size > 4096 or identify.capacity_blocks <= 256) {
        _ = userLog("BlockDemo: unsupported block geometry\n");
        while (true) asm volatile ("pause");
    }

    const target_block = identify.capacity_blocks - 128;

    @memset(block_storage[0..], 0);
    const block_bytes = block_storage[0..@intCast(identify.block_size)];
    _ = client.readBlocks(target_block, block_bytes) catch |err| {
        _ = userLog("BlockDemo: initial read failed\n");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        while (true) asm volatile ("pause");
    };
    const initial_read_tick = getTickCount();
    userLogTick("BlockDemo: initial read", start_tick, initial_read_tick);

    const previous_magic = std.mem.readInt(u64, block_bytes[0..8], .little);
    const previous_generation = std.mem.readInt(u64, block_bytes[8..16], .little);
    if (previous_magic == demo_magic) {
        userLogNum("BlockDemo: previous generation=", previous_generation);
    } else {
        _ = userLog("BlockDemo: no persisted record yet\n");
    }

    @memset(block_bytes, 0);
    const next_generation: u64 = if (previous_magic == demo_magic) previous_generation + 1 else 1;
    std.mem.writeInt(u64, block_bytes[0..8], demo_magic, .little);
    std.mem.writeInt(u64, block_bytes[8..16], next_generation, .little);
    std.mem.writeInt(u64, block_bytes[16..24], target_block, .little);
    client.writeBlocks(target_block, block_bytes) catch |err| {
        _ = userLog("BlockDemo: write failed\n");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        while (true) asm volatile ("pause");
    };
    const write_tick = getTickCount();
    userLogTick("BlockDemo: write", start_tick, write_tick);
    client.flush() catch |err| {
        _ = userLog("BlockDemo: flush failed\n");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        while (true) asm volatile ("pause");
    };
    const flush_tick = getTickCount();
    userLogTick("BlockDemo: flush", start_tick, flush_tick);

    @memset(block_bytes, 0);
    _ = client.readBlocks(target_block, block_bytes) catch |err| {
        _ = userLog("BlockDemo: verify read failed\n");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        while (true) asm volatile ("pause");
    };
    const verify_read_tick = getTickCount();
    userLogTick("BlockDemo: verify read", start_tick, verify_read_tick);
    const verify_magic = std.mem.readInt(u64, block_bytes[0..8], .little);
    const verify_generation = std.mem.readInt(u64, block_bytes[8..16], .little);
    if (verify_magic != demo_magic or verify_generation != next_generation) {
        _ = userLog("BlockDemo: verify mismatch\n");
        userLogHex("BlockDemo: verify magic=", verify_magic);
        userLogNum("BlockDemo: verify generation=", verify_generation);
        while (true) asm volatile ("pause");
    }

    userLogNum("BlockDemo: persisted generation=", next_generation);
    _ = userLog("BlockDemo: persistence write+verify ok\n");
    userLogTick("BlockDemo: done", start_tick, getTickCount());

    while (true) asm volatile ("pause");
}
