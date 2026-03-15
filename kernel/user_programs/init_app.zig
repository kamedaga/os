const process_abi = @import("process_abi.zig");
const vfs_client = @import("vfs_client.zig");

const syscall_log: u64 = 0x9;
const init_process_slot: u64 = 9;
const vfs_request_va: u64 = 0x3C10_4000;
const vfs_response_va: u64 = 0x3C10_5000;
const keyboard_shared_page_va: u64 = 0x3C00_6000;
const keyboard_shared_magic: u64 = 0x4B534852; // "KSHR"
const taskbar_config_source_va: u64 = 0x3C00_8000;
const taskbar_state_source_va: u64 = 0x3C00_9000;
const taskbar_command_source_va: u64 = 0x3C00_A000;
const taskbar_mouse_source_va: u64 = 0x3C00_B000;
const taskbar_config_target_va: u64 = 0x3C00_2000;
const taskbar_mouse_target_va: u64 = 0x3C00_3000;
const taskbar_state_target_va: u64 = 0x3C00_4000;
const taskbar_command_target_va: u64 = 0x3C00_6000;

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
    var len: usize = 0;
    while (len < label.len and len < buf.len) : (len += 1) {
        buf[len] = label[len];
    }
    if (len + 19 >= buf.len) return;
    buf[len] = '0';
    buf[len + 1] = 'x';
    len += 2;
    var shift: u6 = 60;
    while (true) {
        const nibble: u8 = @intCast((value >> shift) & 0xF);
        buf[len] = if (nibble < 10) '0' + nibble else 'A' + (nibble - 10);
        len += 1;
        if (shift == 0) break;
        shift -= 4;
    }
    buf[len] = '\n';
    len += 1;
    _ = userLog(buf[0..len]);
}

fn spawnExec(exec_token: u64, bootstrap_source_va: u64, bootstrap_target_va: u64, bootstrap_flags: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (bootstrap_source_va),
          [arg2] "{rdx}" (bootstrap_target_va),
          [arg3] "{rcx}" (bootstrap_flags),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn spawnExecWithBootstrapPages(exec_token: u64, descriptors: []const process_abi.BootstrapPageDescriptor) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (@as(u64, @intFromPtr(descriptors.ptr))),
          [arg2] "{rdx}" (@as(u64, @intCast(descriptors.len))),
          [arg3] "{rcx}" (process_abi.spawn_flag_bootstrap_descriptor_table),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn pauseLoop(iterations: usize) void {
    var i: usize = 0;
    while (i < iterations) : (i += 1) asm volatile ("pause");
}

fn lookupFileWithRetry(client: *vfs_client.Client, path: []const u8) ?vfs_client.LookupResult {
    var attempt: usize = 0;
    while (attempt < 4096) : (attempt += 1) {
        const result = client.lookup(0, path) catch {
            pauseLoop(1024);
            continue;
        };
        if (result.object_kind == .vnode_file) return result;
        pauseLoop(1024);
    }
    return null;
}

pub export fn _start() noreturn {
    _ = userLog("Init: started\n");

    const keyboard_shared: [*]const volatile u64 = @ptrFromInt(keyboard_shared_page_va);
    if (keyboard_shared[0] != keyboard_shared_magic) {
        _ = userLog("Init: keyboard shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }

    var client = vfs_client.Client.connect(.{
        .request_va = vfs_request_va,
        .response_va = vfs_response_va,
        .client_process_slot = init_process_slot,
    }) catch {
        _ = userLog("Init: VFS connect failed\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("Init: VFS connect ok\n");

    const terminal_file = lookupFileWithRetry(&client, "/bin/terminal_window.elf") orelse {
        _ = userLog("Init: lookup /bin/terminal_window.elf failed\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("Init: lookup /bin/terminal_window.elf ok\n");

    const exec_file = client.openExec(terminal_file.token) catch {
        _ = userLog("Init: open_exec /bin/terminal_window.elf failed\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("Init: open_exec /bin/terminal_window.elf ok\n");

    const spawned = spawnExec(exec_file.token, keyboard_shared_page_va, keyboard_shared_page_va, 0);
    if (process_abi.decodeSpawnedProcessSlot(spawned) == null) {
        _ = userLog("Init: spawn /bin/terminal_window.elf failed\n");
        userLogHex("Init: spawn ret=", spawned);
        while (true) asm volatile ("pause");
    }
    _ = userLog("Init: spawn /bin/terminal_window.elf ok\n");

    const taskbar_file = lookupFileWithRetry(&client, "/bin/taskbar.elf") orelse {
        _ = userLog("Init: lookup /bin/taskbar.elf failed\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("Init: lookup /bin/taskbar.elf ok\n");

    const taskbar_exec = client.openExec(taskbar_file.token) catch {
        _ = userLog("Init: open_exec /bin/taskbar.elf failed\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("Init: open_exec /bin/taskbar.elf ok\n");

    const taskbar_bootstrap = [_]process_abi.BootstrapPageDescriptor{
        .{ .source_va = taskbar_config_source_va, .target_va = taskbar_config_target_va, .flags = process_abi.spawn_flag_bootstrap_page_writable },
        .{ .source_va = taskbar_state_source_va, .target_va = taskbar_state_target_va, .flags = 0 },
        .{ .source_va = taskbar_command_source_va, .target_va = taskbar_command_target_va, .flags = process_abi.spawn_flag_bootstrap_page_writable },
        .{ .source_va = taskbar_mouse_source_va, .target_va = taskbar_mouse_target_va, .flags = 0 },
    };
    const spawned_taskbar = spawnExecWithBootstrapPages(taskbar_exec.token, taskbar_bootstrap[0..]);
    if (process_abi.decodeSpawnedProcessSlot(spawned_taskbar) == null) {
        _ = userLog("Init: spawn /bin/taskbar.elf failed\n");
        userLogHex("Init: taskbar spawn ret=", spawned_taskbar);
        while (true) asm volatile ("pause");
    }
    _ = userLog("Init: spawn /bin/taskbar.elf ok\n");

    while (true) asm volatile ("pause");
}
