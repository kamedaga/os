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
const syscall_install_endpoint: u64 = 0x26;
const syscall_ipc_call_reply_recv: u64 = 0x40;
const syscall_ok: u64 = 0;
const ipc_call_flag_signal_only: u64 = 0x2;

const exec_loader_path = "/srv/exec_loader.elf";
const linux_abi_server_path = "/srv/linux_abi_server.elf";
const smoke_app_path = "/cmd/musl_smoke.elf";
const ld_path = "/lib/ld.so";
const linux_abi_endpoint_id: u64 = 0x90;
const linux_abi_config_endpoint_id: u64 = 0x91;

const max_file_bytes: usize = 128 * 1024;
const stack_extension_pages: u64 = 4;

var smoke_app_image: [max_file_bytes]u8 align(4096) = undefined;
var ld_image: [max_file_bytes]u8 align(4096) = undefined;
var exec_loader_image: [max_file_bytes]u8 align(4096) = undefined;
var linux_abi_server_image: [max_file_bytes]u8 align(4096) = undefined;
var linux_abi_registry_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var exec_loader_config_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var exec_loader_bootstrap_table = process_abi.BootstrapDescriptorTable{};
var linux_abi_bootstrap_table = process_abi.BootstrapDescriptorTable{};

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

fn installEndpoint(endpoint_id: u64, target_process_slot: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_install_endpoint),
          [arg0] "{rdi}" (@as(u64, 0)),
          [arg1] "{rsi}" (endpoint_id),
          [arg2] "{rdx}" (target_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn signalEndpointValue(endpoint_id: u64, value: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_ipc_call_reply_recv),
          [arg0] "{rdi}" (value),
          [arg1] "{rsi}" (endpoint_id),
          [arg2] "{rdx}" (@as(u64, ipc_call_flag_signal_only)),
          [arg3] "{r8}" (@as(u64, 0)),
          [arg4] "{r9}" (@as(u64, 0)),
          [arg5] "{r10}" (@as(u64, 0)),
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

fn installExecImage(vm_token: u64, rights: image_abi.ExecImageRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_install_exec_image),
          [arg0] "{rdi}" (vm_token),
          [arg1] "{rsi}" (image_abi.execImageRightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn spawnExecWithExtendedBootstrapTable(exec_token: u64, table: *const process_abi.BootstrapDescriptorTable, child_bootstrap_owner: bool) u64 {
    const flags = process_abi.spawn_flag_bootstrap_extended_descriptor_table |
        if (child_bootstrap_owner) process_abi.spawn_flag_child_bootstrap_owner else 0;
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (@as(u64, @intFromPtr(table))),
          [arg2] "{rdx}" (@as(u64, 0)),
          [arg3] "{rcx}" (flags),
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
    if (root.object_kind != fs_abi.ObjectKind.mount and root.object_kind != fs_abi.ObjectKind.vnode_dir) fail("ExecSmokeRunner: root lookup returned non-dir\n");
    return root;
}

fn runRootVfsSmoke(process_slot: u64) void {
    const ipc_va = user_vm.reservePages(2) orelse fail("ExecSmokeRunner: vfs IPC VA reserve failed\n");
    var client = fs_client.Client.connectFromRegistryPageKind(
        process_abi.service_registry_shadow_va,
        .vfs,
        @intCast(ipc_va),
        @intCast(ipc_va + user_vm.page_bytes),
        process_slot,
    ) catch |err| failErr("ExecSmokeRunner: vfs connect failed: ", err);
    _ = userLog("ExecSmokeRunner: vfs connect ok\n");

    const root_stat = client.stat(client.mount_token) catch |err| failErr("ExecSmokeRunner: vfs root stat failed: ", err);
    if (root_stat.object_kind != fs_abi.ObjectKind.mount and root_stat.object_kind != fs_abi.ObjectKind.vnode_dir) {
        fail("ExecSmokeRunner: vfs root stat non-dir\n");
    }
    _ = userLog("ExecSmokeRunner: vfs root stat ok\n");

    const root_lookup = client.lookup(client.mount_token, "/") catch |err| failErr("ExecSmokeRunner: vfs root lookup failed: ", err);
    if (root_lookup.object_kind != fs_abi.ObjectKind.mount) fail("ExecSmokeRunner: vfs root lookup non-mount\n");
    _ = userLog("ExecSmokeRunner: vfs root lookup ok\n");

    var entry_buf: [16]fs_client.DirEntry = undefined;
    var name_storage: [192]u8 = undefined;
    const entries = client.readdirMany(client.mount_token, 0, entry_buf[0..], name_storage[0..]) catch |err| failErr("ExecSmokeRunner: vfs readdir failed: ", err);
    var saw_dev = false;
    var saw_proc = false;
    var saw_tmp = false;
    var saw_run = false;
    var saw_cmd = false;
    var saw_srv = false;
    var saw_sys = false;
    var saw_sbin = false;
    for (entries.entries) |entry| {
        if (entry.object_kind != fs_abi.ObjectKind.vnode_dir) fail("ExecSmokeRunner: vfs readdir non-dir\n");
        if (std.mem.eql(u8, entry.name, "dev")) saw_dev = true;
        if (std.mem.eql(u8, entry.name, "proc")) saw_proc = true;
        if (std.mem.eql(u8, entry.name, "tmp")) saw_tmp = true;
        if (std.mem.eql(u8, entry.name, "run")) saw_run = true;
        if (std.mem.eql(u8, entry.name, "cmd")) saw_cmd = true;
        if (std.mem.eql(u8, entry.name, "srv")) saw_srv = true;
        if (std.mem.eql(u8, entry.name, "sys")) saw_sys = true;
        if (std.mem.eql(u8, entry.name, "sbin")) saw_sbin = true;
    }
    if (!saw_dev or !saw_proc or !saw_tmp or !saw_run) fail("ExecSmokeRunner: vfs builtin mount missing\n");
    if (!saw_cmd or !saw_srv or !saw_sys or !saw_sbin) fail("ExecSmokeRunner: vfs fat backend entry missing\n");
    _ = userLog("ExecSmokeRunner: vfs readdir ok\n");

    const cmd = client.lookup(client.mount_token, "/cmd") catch |err| failErr("ExecSmokeRunner: vfs /cmd lookup failed: ", err);
    if (cmd.object_kind != fs_abi.ObjectKind.vnode_dir) fail("ExecSmokeRunner: vfs /cmd non-dir\n");
    const shell = client.lookup(cmd.token, "shell.elf") catch |err| failErr("ExecSmokeRunner: vfs /cmd/shell.elf lookup failed: ", err);
    if (shell.object_kind != fs_abi.ObjectKind.vnode_file) fail("ExecSmokeRunner: vfs /cmd/shell.elf non-file\n");
    const startup = client.lookup(client.mount_token, "/sys/startup_manifest.txt") catch |err| failErr("ExecSmokeRunner: vfs startup lookup failed: ", err);
    if (startup.object_kind != fs_abi.ObjectKind.vnode_file) fail("ExecSmokeRunner: vfs startup non-file\n");
    const startup_open = client.open(startup.token) catch |err| failErr("ExecSmokeRunner: vfs startup open failed: ", err);
    var startup_buf: [64]u8 = undefined;
    const startup_read = client.read(startup_open.token, 0, startup_buf[0..]) catch |err| failErr("ExecSmokeRunner: vfs startup read failed: ", err);
    if (startup_read.bytes_read == 0) fail("ExecSmokeRunner: vfs startup read empty\n");
    client.close(startup_open.token) catch |err| failErr("ExecSmokeRunner: vfs startup close failed: ", err);
    _ = userLog("ExecSmokeRunner: vfs fat backend smoke ok\n");

    const dev = client.lookup(client.mount_token, "/dev") catch |err| failErr("ExecSmokeRunner: vfs /dev lookup failed: ", err);
    if (dev.object_kind != fs_abi.ObjectKind.vnode_dir) fail("ExecSmokeRunner: vfs /dev non-dir\n");
    const dev_stat = client.stat(dev.token) catch |err| failErr("ExecSmokeRunner: vfs /dev stat failed: ", err);
    if (dev_stat.object_kind != fs_abi.ObjectKind.vnode_dir) fail("ExecSmokeRunner: vfs /dev stat non-dir\n");

    var dev_entry_buf: [4]fs_client.DirEntry = undefined;
    var dev_name_storage: [32]u8 = undefined;
    const dev_entries = client.readdirMany(dev.token, 0, dev_entry_buf[0..], dev_name_storage[0..]) catch |err| failErr("ExecSmokeRunner: vfs /dev readdir failed: ", err);
    var saw_null = false;
    var saw_zero = false;
    for (dev_entries.entries) |entry| {
        if (entry.object_kind != fs_abi.ObjectKind.vnode_file) fail("ExecSmokeRunner: vfs /dev readdir non-file\n");
        if (std.mem.eql(u8, entry.name, "null")) saw_null = true;
        if (std.mem.eql(u8, entry.name, "zero")) saw_zero = true;
    }
    if (!saw_null or !saw_zero) fail("ExecSmokeRunner: vfs /dev builtin missing\n");

    const null_node = client.lookup(dev.token, "null") catch |err| failErr("ExecSmokeRunner: vfs /dev/null lookup failed: ", err);
    if (null_node.object_kind != fs_abi.ObjectKind.vnode_file) fail("ExecSmokeRunner: vfs /dev/null non-file\n");
    const null_open = client.open(null_node.token) catch |err| failErr("ExecSmokeRunner: vfs /dev/null open failed: ", err);
    var null_read_buf: [8]u8 = undefined;
    const null_read = client.read(null_open.token, 0, null_read_buf[0..]) catch |err| failErr("ExecSmokeRunner: vfs /dev/null read failed: ", err);
    if (null_read.bytes_read != 0) fail("ExecSmokeRunner: vfs /dev/null read non-empty\n");
    const null_written = client.write(null_open.token, 0, "discard") catch |err| failErr("ExecSmokeRunner: vfs /dev/null write failed: ", err);
    if (null_written != 7) fail("ExecSmokeRunner: vfs /dev/null write count invalid\n");
    client.close(null_open.token) catch |err| failErr("ExecSmokeRunner: vfs /dev/null close failed: ", err);

    const zero_node = client.lookup(client.mount_token, "/dev/zero") catch |err| failErr("ExecSmokeRunner: vfs /dev/zero lookup failed: ", err);
    if (zero_node.object_kind != fs_abi.ObjectKind.vnode_file) fail("ExecSmokeRunner: vfs /dev/zero non-file\n");
    const zero_open = client.open(zero_node.token) catch |err| failErr("ExecSmokeRunner: vfs /dev/zero open failed: ", err);
    var zero_buf: [16]u8 = undefined;
    const zero_read = client.read(zero_open.token, 0, zero_buf[0..]) catch |err| failErr("ExecSmokeRunner: vfs /dev/zero read failed: ", err);
    if (zero_read.bytes_read != zero_buf.len) fail("ExecSmokeRunner: vfs /dev/zero read size invalid\n");
    for (zero_buf) |byte| {
        if (byte != 0) fail("ExecSmokeRunner: vfs /dev/zero read nonzero\n");
    }
    client.close(zero_open.token) catch |err| failErr("ExecSmokeRunner: vfs /dev/zero close failed: ", err);
    _ = userLog("ExecSmokeRunner: vfs smoke ok\n");
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

fn loadRootFsExec(client: *fs_client.Client, root_token: u64, path: []const u8, out: []u8) fs_client.OpenResult {
    const image = readRootFsFile(client, root_token, path, out);
    const vm_token = installReadMapVmObject(out[0..image.bytes]);
    const exec_token = installExecImage(vm_token, .{ .exec = true, .grant = true });
    if (image_abi.decodeExecImageToken(exec_token) == null) fail("ExecSmokeRunner: install exec image failed\n");
    return .{ .token = exec_token, .file_bytes = image.bytes };
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
    mapStaticScratchBuffer(@intFromPtr(&exec_loader_image), file_pages);
    mapStaticScratchBuffer(@intFromPtr(&linux_abi_server_image), file_pages);
    mapStaticScratchBuffer(@intFromPtr(&linux_abi_registry_page), 1);
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
    abi_trap_request_page_va: u64,
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
        .abi_trap_request_page_va = abi_trap_request_page_va,
    };
    if (service_registry_abi.findService(process_abi.service_registry_shadow_va, .vfs)) |entry| {
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
    const spawned = spawnExecWithExtendedBootstrapTable(loader_exec.token, &exec_loader_bootstrap_table, true);
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

fn spawnLinuxAbiServer(exec_token: u64) u64 {
    const src: [*]volatile u8 = @ptrFromInt(process_abi.service_registry_shadow_va);
    const dst: [*]volatile u8 = @ptrFromInt(@intFromPtr(&linux_abi_registry_page));
    var i: usize = 0;
    while (i < linux_abi_registry_page.len) : (i += 1) {
        dst[i] = src[i];
    }

    linux_abi_bootstrap_table = .{};
    linux_abi_bootstrap_table.page_count = 1;
    linux_abi_bootstrap_table.page_descriptors[0] = .{
        .source_va = @intFromPtr(&linux_abi_registry_page),
        .target_va = process_abi.service_registry_shadow_va,
        .flags = 0,
    };
    return spawnExecWithExtendedBootstrapTable(exec_token, &linux_abi_bootstrap_table, false);
}

pub export fn _start() noreturn {
    _ = userLog("ExecSmokeRunner: started\n");
    extendStack();
    const process_slot = getProcessSlot();
    if (process_slot == 0) fail("ExecSmokeRunner: process slot unavailable\n");

    runRootVfsSmoke(process_slot);
    _ = userLog("ExecSmokeRunner: rootfs vfs-only smoke done\n");
    const run_legacy_dynamic_smoke = process_slot != 0;
    if (!run_legacy_dynamic_smoke) while (true) asm volatile ("pause");

    const ipc_va = user_vm.reservePages(2) orelse fail("ExecSmokeRunner: IPC VA reserve failed\n");
    var client = fs_client.Client.connectFromRegistryPageKind(
        process_abi.service_registry_shadow_va,
        .vfs,
        @intCast(ipc_va),
        @intCast(ipc_va + user_vm.page_bytes),
        process_slot,
    ) catch |err| failErr("ExecSmokeRunner: vfs connect for dynamic smoke failed: ", err);
    _ = userLog("ExecSmokeRunner: vfs connect for dynamic smoke ok\n");

    const root = lookupPersistentRoot(&client);
    const loader_exec = loadRootFsExec(&client, root.token, exec_loader_path, exec_loader_image[0..]);
    const linux_abi_server_exec = loadRootFsExec(&client, root.token, linux_abi_server_path, linux_abi_server_image[0..]);
    const smoke = readRootFsFile(&client, root.token, smoke_app_path, smoke_app_image[0..]);
    const ld = readRootFsFile(&client, root.token, ld_path, ld_image[0..]);

    const smoke_token = installReadMapVmObject(smoke_app_image[0..smoke.bytes]);
    const ld_token = installReadMapVmObject(ld_image[0..ld.bytes]);
    _ = userLog("ExecSmokeRunner: assets ready\n");
    const abi_trap_request_page_va = user_vm.reservePages(1) orelse fail("ExecSmokeRunner: abi trap VA reserve failed\n");
    const abi_server_spawned = spawnLinuxAbiServer(linux_abi_server_exec.token);
    const abi_server_process_slot = process_abi.decodeSpawnedProcessSlot(abi_server_spawned) orelse {
        userLogHex("ExecSmokeRunner: linux abi spawn ret=", abi_server_spawned);
        fail("ExecSmokeRunner: linux abi server spawn failed\n");
    };
    if (installEndpoint(linux_abi_config_endpoint_id, abi_server_process_slot) != syscall_ok) fail("ExecSmokeRunner: linux abi config endpoint failed\n");
    if (signalEndpointValue(linux_abi_config_endpoint_id, @intCast(abi_trap_request_page_va)) != syscall_ok) fail("ExecSmokeRunner: linux abi config send failed\n");
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
        @intCast(abi_trap_request_page_va),
    );

    while (true) asm volatile ("pause");
}

