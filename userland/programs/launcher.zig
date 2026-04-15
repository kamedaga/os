const process_abi = @import("support_root").process_abi;
const process_args_env_bootstrap_abi = @import("support_root").process_args_env_bootstrap_abi;
const process_exit_bootstrap_abi = @import("support_root").process_exit_bootstrap_abi;
const stdio_bootstrap_abi = @import("support_root").stdio_bootstrap_abi;
const vfs_client = @import("support_root").vfs_client;

const syscall_log: u64 = 0x9;
const syscall_alloc_map_pages: u64 = 0xC;

const launcher_process_slot: u64 = 9;
const vfs_request_va: u64 = 0x3C10_4000;
const vfs_response_va: u64 = 0x3C10_5000;
const launcher_stdio_bootstrap_source_va: u64 = 0x3F10_8000;
const launcher_exit_status_bootstrap_source_va: u64 = 0x3F10_9000;
const launcher_args_env_bootstrap_source_va: u64 = 0x3F10_A000;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn allocMapPages(base_va: u64, page_count: u64, writable: bool, out_paddr_list_va: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_map_pages),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
          [arg3] "{rcx}" (out_paddr_list_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn ensureStdioBootstrapPage() bool {
    if (allocMapPages(launcher_stdio_bootstrap_source_va, 1, true, 0) != 0) return false;
    stdio_bootstrap_abi.initZeroPage(launcher_stdio_bootstrap_source_va);
    return true;
}

fn ensureExitStatusBootstrapPage() bool {
    if (allocMapPages(launcher_exit_status_bootstrap_source_va, 1, true, 0) != 0) return false;
    process_exit_bootstrap_abi.initZeroPage(launcher_exit_status_bootstrap_source_va);
    return true;
}

fn ensureArgsEnvBootstrapPage() bool {
    if (allocMapPages(launcher_args_env_bootstrap_source_va, 1, true, 0) != 0) return false;
    process_args_env_bootstrap_abi.initZeroPage(launcher_args_env_bootstrap_source_va);
    return true;
}

fn spawnExec(exec_token: u64) u64 {
    var table = process_abi.BootstrapDescriptorTable{};
    table.page_count = 3;
    table.page_descriptors[0] = .{
        .source_va = launcher_stdio_bootstrap_source_va,
        .target_va = stdio_bootstrap_abi.target_va,
        .flags = 0,
    };
    table.page_descriptors[1] = .{
        .source_va = launcher_exit_status_bootstrap_source_va,
        .target_va = process_exit_bootstrap_abi.target_va,
        .flags = process_abi.spawn_flag_bootstrap_page_writable,
    };
    table.page_descriptors[2] = .{
        .source_va = launcher_args_env_bootstrap_source_va,
        .target_va = process_args_env_bootstrap_abi.target_va,
        .flags = 0,
    };
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (@as(u64, @intFromPtr(&table))),
          [arg2] "{rdx}" (@as(u64, 0)),
          [arg3] "{rcx}" (process_abi.spawn_flag_bootstrap_extended_descriptor_table),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

pub export fn _start() noreturn {
    _ = userLog("Launcher: started\n");
    if (!ensureStdioBootstrapPage()) {
        _ = userLog("Launcher: stdio bootstrap page failed\n");
        while (true) asm volatile ("pause");
    }
    if (!ensureExitStatusBootstrapPage()) {
        _ = userLog("Launcher: exit status bootstrap page failed\n");
        while (true) asm volatile ("pause");
    }
    if (!ensureArgsEnvBootstrapPage()) {
        _ = userLog("Launcher: args env bootstrap page failed\n");
        while (true) asm volatile ("pause");
    }

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
