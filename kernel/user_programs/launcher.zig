const process_abi = @import("process_abi.zig");
const vfs_client = @import("vfs_client.zig");

const syscall_log: u64 = 0x9;

const launcher_process_slot: u64 = 9;
const vfs_request_va: u64 = 0x3C10_4000;
const vfs_response_va: u64 = 0x3C10_5000;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn spawnExec(exec_token: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (@as(u64, 0)),
          [arg2] "{rdx}" (@as(u64, 0)),
          [arg3] "{rcx}" (@as(u64, 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

pub export fn _start() noreturn {
    _ = userLog("Launcher: started\n");

    var client = vfs_client.Client.connect(.{
        .request_va = vfs_request_va,
        .response_va = vfs_response_va,
        .client_process_slot = launcher_process_slot,
    }) catch {
        _ = userLog("Launcher: VFS connect failed\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("Launcher: VFS connect ok\n");

    const init_file = client.lookup(0, "/boot/init.elf") catch {
        _ = userLog("Launcher: lookup /boot/init.elf failed\n");
        while (true) asm volatile ("pause");
    };
    if (init_file.object_kind != .vnode_file) {
        _ = userLog("Launcher: lookup /boot/init.elf bad token\n");
        while (true) asm volatile ("pause");
    }
    _ = userLog("Launcher: lookup /boot/init.elf ok\n");

    const exec_file = client.openExec(init_file.token) catch {
        _ = userLog("Launcher: open_exec /boot/init.elf failed\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("Launcher: open_exec /boot/init.elf ok\n");

    const spawned = spawnExec(exec_file.token);
    if (process_abi.decodeSpawnedProcessSlot(spawned) == null) {
        _ = userLog("Launcher: spawn /boot/init.elf failed\n");
        while (true) asm volatile ("pause");
    }
    _ = userLog("Launcher: spawn /boot/init.elf ok\n");

    while (true) asm volatile ("pause");
}
