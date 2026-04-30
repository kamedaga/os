const std = @import("std");
const support = @import("abi_root");
const exec_loader_bootstrap_abi = support.exec_loader_bootstrap_abi;
const fs_client = support.fs_client;
const fs_abi = support.fs_abi;
const image_abi = support.image_abi;
const process_abi = support.process_abi;
const service_registry_abi = support.service_registry_abi;
const trap_abi = support.trap_abi;
const user_vm = support.user_vm;

const syscall_log: u64 = 0x9;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_get_process_slot: u64 = 0x2E;
const syscall_ok: u64 = 0;

const exec_loader_path = "/srv/exec_loader.elf";
const linux_abi_server_path = "/srv/linux_abi_server.elf";
const smoke_app_path = "/cmd/musl_smoke.elf";
const ld_path = "/lib/ld.so";
const linux_abi_endpoint_id: u64 = 0x90;

const max_file_bytes: usize = 128 * 1024;
const stack_extension_pages: u64 = 4;

var smoke_app_image: [max_file_bytes]u8 align(4096) = undefined;
var ld_image: [max_file_bytes]u8 align(4096) = undefined;
var exec_loader_config_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var exec_loader_bootstrap_table = process_abi.BootstrapDescriptorTable{};

const FileImage = struct {
    bytes: usize,
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

fn userLogHex(label: []const u8, value: u64) void {
    var buf: [128]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, "{s}0x{X}\n", .{ label, value }) catch return;
    _ = userLog(msg);
}

fn getProcessSlot() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_get_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn allocMapPages(base_va: u64, page_count: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_map_pages),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
          [arg3] "{rcx}" (@as(u64, 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installVmObject(base_va: u64, size_bytes: u64, rights: image_abi.VmObjectRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_install_vm_object),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (size_bytes),
          [arg2] "{rdx}" (image_abi.vmObjectRightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn spawnExecWithExtendedBootstrapTable(exec_token: u64, table: *const process_abi.BootstrapDescriptorTable) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (@as(u64, @intFromPtr(table))),
          [arg2] "{rdx}" (@as(u64, 0)),
          [arg3] "{rcx}" (process_abi.spawn_flag_bootstrap_extended_descriptor_table | process_abi.spawn_flag_child_bootstrap_owner),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn fail(message: []const u8) noreturn {
    _ = userLog(message);
    while (true) asm volatile ("pause");
}

fn failErr(prefix: []const u8, err: anyerror) noreturn {
    _ = userLog(prefix);
    _ = userLog(@errorName(err));
    _ = userLog("\n");
    while (true) asm volatile ("pause");
}

fn extendStack() void {
    const base_va = process_abi.user_stack_page_va - stack_extension_pages * user_vm.page_bytes;
    if (allocMapPages(base_va, stack_extension_pages, true) != syscall_ok) fail("ExecSmokeRunner: stack extension failed\n");
}

fn lookupPersistentRoot(client: *fs_client.Client) fs_client.LookupResult {
    const root = client.lookup(client.mount_token, ".") catch |err| failErr("ExecSmokeRunner: root lookup failed: ", err);
    if (root.object_kind != fs_abi.ObjectKind.vnode_dir) fail("ExecSmokeRunner: root lookup returned non-dir\n");
    return root;
}

fn readRootFsFile(client: *fs_client.Client, root_token: u64, path: []const u8, out: []u8) FileImage {
    const lookup = client.lookup(root_token, path) catch |err| failErr("ExecSmokeRunner: lookup failed: ", err);
    if (lookup.object_kind != fs_abi.ObjectKind.vnode_file) fail("ExecSmokeRunner: lookup returned non-file\n");

    const open_file = client.open(lookup.token) catch |err| failErr("ExecSmokeRunner: open failed: ", err);
    defer client.close(open_file.token) catch {};
    if (open_file.file_bytes == 0 or open_file.file_bytes > out.len) fail("ExecSmokeRunner: file size unsupported\n");

    const file_bytes: usize = @intCast(open_file.file_bytes);
    var offset: usize = 0;
    while (offset < file_bytes) {
        const read_result = client.read(open_file.token, offset, out[offset..file_bytes]) catch |err| failErr("ExecSmokeRunner: read failed: ", err);
        if (read_result.bytes_read == 0) fail("ExecSmokeRunner: short read\n");
        offset += read_result.bytes_read;
    }
    return .{ .bytes = file_bytes };
}

fn openRootFsExec(client: *fs_client.Client, root_token: u64, path: []const u8) fs_client.OpenResult {
    const lookup = client.lookup(root_token, path) catch |err| failErr("ExecSmokeRunner: lookup exec failed: ", err);
    if (lookup.object_kind != fs_abi.ObjectKind.vnode_file) fail("ExecSmokeRunner: exec lookup returned non-file\n");
    return client.openExec(lookup.token) catch |err| failErr("ExecSmokeRunner: open_exec failed: ", err);
}

fn installReadMapVmObject(bytes: []u8) u64 {
    const token = installVmObject(@intFromPtr(bytes.ptr), bytes.len, .{
        .read = true,
        .map = true,
        .grant = true,
    });
    if (image_abi.decodeVmObjectToken(token) == null) fail("ExecSmokeRunner: vm install failed\n");
    return token;
}

fn mapStaticScratchBuffers() void {
    const file_pages = max_file_bytes / user_vm.page_bytes;
    mapStaticScratchBuffer(@intFromPtr(&smoke_app_image), file_pages);
    mapStaticScratchBuffer(@intFromPtr(&ld_image), file_pages);
}

fn mapStaticScratchBuffer(base_va: u64, page_count: usize) void {
    var page_index: usize = 0;
    while (page_index < page_count) : (page_index += 1) {
        const page_va = base_va + @as(u64, @intCast(page_index * user_vm.page_bytes));
        // The ELF spawn path may already map the file-backed prefix of large BSS-backed
        // globals. There is no userland query syscall yet, so map one page at a time and
        // tolerate already-present failures while filling the missing BSS tail.
        _ = allocMapPages(page_va, 1, true);
    }
}

fn spawnExecLoader(
    loader_exec: fs_client.OpenResult,
    executable_token: u64,
    executable_bytes: usize,
    interpreter_token: u64,
    interpreter_bytes: usize,
    bootfs_token: u64,
    bootfs_bytes: usize,
    abi_server_process_slot: u64,
) void {
    const cfg: *volatile exec_loader_bootstrap_abi.Config = @ptrCast(@alignCast(&exec_loader_config_page));
    cfg.* = .{
        .magic = exec_loader_bootstrap_abi.magic,
        .version = exec_loader_bootstrap_abi.version,
        .executable_vm_token = 0,
        .executable_file_bytes = executable_bytes,
        .flags = 0,
        .interpreter_vm_token = 0,
        .interpreter_file_bytes = interpreter_bytes,
        .bootfs_vm_token = 0,
        .bootfs_file_bytes = bootfs_bytes,
        .fs_endpoint_id = 0,
        .fs_compat_process_slot = 0,
        .abi_trap_endpoint_id = linux_abi_endpoint_id,
        .abi_trap_endpoint_process_slot = abi_server_process_slot,
        .abi_trap_flavor = @intFromEnum(trap_abi.AbiFlavor.linux_x86_64),
    };
    if (service_registry_abi.findService(process_abi.service_registry_shadow_va, .persistent_fs)) |entry| {
        cfg.fs_endpoint_id = entry.endpoint_id;
        cfg.fs_compat_process_slot = entry.process_slot;
    }

    exec_loader_bootstrap_table = .{};
    exec_loader_bootstrap_table.page_count = 1;
    exec_loader_bootstrap_table.page_descriptors[0] = .{
        .source_va = @intFromPtr(&exec_loader_config_page),
        .target_va = exec_loader_bootstrap_abi.target_va,
        .flags = 0,
    };
    exec_loader_bootstrap_table.cap_count = if (image_abi.decodeVmObjectToken(bootfs_token) != null and bootfs_bytes != 0) 3 else 2;
    exec_loader_bootstrap_table.cap_descriptors[0] = .{
        .source_token = executable_token,
        .target_token_va = exec_loader_bootstrap_abi.target_va + @offsetOf(exec_loader_bootstrap_abi.Config, "executable_vm_token"),
        .rights_bits = image_abi.vmObjectRightsToBits(.{ .read = true, .map = true }),
        .kind = .vm_object,
    };
    exec_loader_bootstrap_table.cap_descriptors[1] = .{
        .source_token = interpreter_token,
        .target_token_va = exec_loader_bootstrap_abi.target_va + @offsetOf(exec_loader_bootstrap_abi.Config, "interpreter_vm_token"),
        .rights_bits = image_abi.vmObjectRightsToBits(.{ .read = true, .map = true }),
        .kind = .vm_object,
    };
    if (exec_loader_bootstrap_table.cap_count == 3) {
        exec_loader_bootstrap_table.cap_descriptors[2] = .{
            .source_token = bootfs_token,
            .target_token_va = exec_loader_bootstrap_abi.target_va + @offsetOf(exec_loader_bootstrap_abi.Config, "bootfs_vm_token"),
            .rights_bits = image_abi.vmObjectRightsToBits(.{ .read = true, .map = true }),
            .kind = .vm_object,
        };
    }

    _ = userLog("ExecSmokeRunner: exec_loader spawn begin\n");
    const spawned = spawnExecWithExtendedBootstrapTable(loader_exec.token, &exec_loader_bootstrap_table);
    if (process_abi.decodeSpawnedProcessSlot(spawned) == null) {
        userLogHex("ExecSmokeRunner: spawn ret=", spawned);
        fail("ExecSmokeRunner: exec_loader spawn failed\n");
    }
    _ = userLog("ExecSmokeRunner: exec_loader spawn ok\n");
}

fn spawnPlainExec(exec_token: u64) u64 {
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
    _ = userLog("ExecSmokeRunner: started\n");
    extendStack();
    const process_slot = getProcessSlot();
    if (process_slot == 0) fail("ExecSmokeRunner: process slot unavailable\n");

    const ipc_va = user_vm.reservePages(2) orelse fail("ExecSmokeRunner: IPC VA reserve failed\n");
    var client = fs_client.Client.connectFromRegistryPage(
        process_abi.service_registry_shadow_va,
        @intCast(ipc_va),
        @intCast(ipc_va + user_vm.page_bytes),
        process_slot,
    ) catch |err| failErr("ExecSmokeRunner: persistent fs connect failed: ", err);
    _ = userLog("ExecSmokeRunner: persistent fs connect ok\n");

    const root = lookupPersistentRoot(&client);
    const loader_exec = openRootFsExec(&client, root.token, exec_loader_path);
    const linux_abi_server_exec = openRootFsExec(&client, root.token, linux_abi_server_path);
    const smoke = readRootFsFile(&client, root.token, smoke_app_path, smoke_app_image[0..]);
    const ld = readRootFsFile(&client, root.token, ld_path, ld_image[0..]);

    const smoke_token = installReadMapVmObject(smoke_app_image[0..smoke.bytes]);
    const ld_token = installReadMapVmObject(ld_image[0..ld.bytes]);
    _ = userLog("ExecSmokeRunner: assets ready\n");
    const abi_server_spawned = spawnPlainExec(linux_abi_server_exec.token);
    const abi_server_process_slot = process_abi.decodeSpawnedProcessSlot(abi_server_spawned) orelse {
        userLogHex("ExecSmokeRunner: linux abi spawn ret=", abi_server_spawned);
        fail("ExecSmokeRunner: linux abi server spawn failed\n");
    };
    _ = userLog("ExecSmokeRunner: linux abi server spawn ok\n");

    spawnExecLoader(
        loader_exec,
        smoke_token,
        smoke.bytes,
        ld_token,
        ld.bytes,
        0,
        0,
        abi_server_process_slot,
    );

    while (true) asm volatile ("pause");
}

