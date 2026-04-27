const std = @import("std");
const cap_transfer_abi = @import("support_root").cap_transfer_abi;
const bootfs_format = @import("support_root").bootfs_format;
const fs_abi = @import("support_root").fs_abi;
const image_abi = @import("support_root").image_abi;
const process_abi = @import("support_root").process_abi;
const user_vm = @import("support_root").user_vm;
const vfs_protocol = @import("support_root").vfs_protocol;

const syscall_map_page: u64 = 0x2;
const syscall_log: u64 = 0x9;
const syscall_wait_event: u64 = 0x17;

const syscall_ok: u64 = 0;
const page_right_cpu_read: u64 = 0x1;
const page_right_cpu_write: u64 = 0x2;

const vfs_config_va: usize = @intCast(process_abi.standard_config_target_va);
const vfs_boot_config_magic: u64 = 0x5646_5343; // "VFSC"
const vfs_boot_config_version: u64 = 2;
const vfs_boot_config_flag_bootfs_present: u64 = 1 << 0;
const fs_cap_token_tag: u64 = 1 << 63;
const bootfs_root_object_id: u64 = 1;
const session_poll_timeout_ticks: u64 = 1;
const max_sessions: usize = 4;
const max_session_objects: usize = 8;

const ResolvedKind = enum(u8) {
    dir,
    file,
};

const bootfs_mount_token: u64 = 1; // VFS-internal mount identifier

const BootState = struct {
    endpoint_id: u64 = 0,
    bootfs_vm_token: u64 = 0,
    bootfs_size_bytes: u64 = 0,
    flags: u64 = 0,
};

const ResolvedNode = struct {
    source_mount_token: u64,
    object_id: u64,
    kind: ResolvedKind,
    size_bytes: u64 = 0,
    abs_path: []const u8 = "/",
};

const BootFsChild = struct {
    name: []const u8,
    abs_path: []const u8,
    kind: ResolvedKind,
    size_bytes: u64 = 0,
};

const BootFsFileData = struct {
    data_offset: u64,
    data_bytes: u64,
};

const LookupPathResult = union(enum) {
    node: ResolvedNode,
    invalid,
    not_found,
    not_dir,
};

const SessionObject = struct {
    active: bool = false,
    client_token: u64 = 0,
    kind: fs_abi.ObjectKind = .none,
    node: ResolvedNode = .{
        .source_mount_token = 0,
        .object_id = 0,
        .kind = .dir,
        .size_bytes = 0,
        .abs_path = "/",
    },
};

const Session = struct {
    active: bool = false,
    client_process_slot: u64 = 0,
    request_paddr: u64 = 0,
    response_paddr: u64 = 0,
    request_va: u64 = 0,
    response_va: u64 = 0,
    last_completed_seq: u64 = 0,
    objects: [max_session_objects]SessionObject = [_]SessionObject{.{}} ** max_session_objects,
};

var boot_state: BootState = .{};
var boot_state_ready = false;
var bootfs_mapped_prefix_bytes: u64 = 0;
var bootfs_image_va: usize = 0;
var bootfs_probe_va: usize = 0;
var bootfs_read_window_va: usize = 0;
var sessions: [max_sessions]Session = [_]Session{.{}} ** max_sessions;

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

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn acceptCapTransfer(transfer_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (cap_transfer_abi.syscall_accept_cap_transfer),
          [arg0] "{rdi}" (transfer_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapVmObject(token: u64, target_va: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_map_vm_object),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (target_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn sliceVmObject(token: u64, offset_bytes: u64, size_bytes: u64, rights: image_abi.VmObjectRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_slice_vm_object),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (offset_bytes),
          [arg2] "{rdx}" (size_bytes),
          [arg3] "{rcx}" (image_abi.vmObjectRightsToBits(rights)),
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

fn grantExecImage(token: u64, to_process_slot: u64, rights: image_abi.ExecImageRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_grant_exec_image),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (to_process_slot),
          [arg2] "{rdx}" (image_abi.execImageRightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn compilerBarrier() void {
    asm volatile ("" ::: .{ .memory = true });
}

fn haltForever() noreturn {
    while (true) asm volatile ("pause");
}

var next_fs_token: u64 = 1;

fn allocFsToken() u64 {
    const t = fs_cap_token_tag | next_fs_token;
    next_fs_token += 1;
    return t;
}

fn parseBootState() ?BootState {
    const cfg: [*]const volatile u64 = @ptrFromInt(vfs_config_va);
    if (cfg[0] != vfs_boot_config_magic) return null;
    if (cfg[1] != vfs_boot_config_version) return null;
    return .{
        .endpoint_id = cfg[2],
        .bootfs_vm_token = cfg[4],
        .bootfs_size_bytes = cfg[5],
        .flags = cfg[6],
    };
}

fn ensureBootFsImageVa() bool {
    if (bootfs_image_va != 0) return true;
    if (boot_state.bootfs_size_bytes == 0) return false;
    const page_count: usize = @intCast((boot_state.bootfs_size_bytes + vfs_protocol.page_bytes - 1) / vfs_protocol.page_bytes);
    bootfs_image_va = user_vm.reservePages(page_count) orelse return false;
    return true;
}

fn ensureBootFsWindowVas() bool {
    if (bootfs_probe_va != 0 and bootfs_read_window_va != 0) return true;
    bootfs_probe_va = user_vm.reservePages(2) orelse return false;
    bootfs_read_window_va = user_vm.reservePages(2) orelse return false;
    return true;
}

fn rootNode(_: *const BootState) ResolvedNode {
    return .{
        .source_mount_token = bootfs_mount_token,
        .object_id = bootfs_root_object_id,
        .kind = .dir,
        .size_bytes = 0,
        .abs_path = "/",
    };
}

fn bootfsHeader() ?*const bootfs_format.BootFsHeader {
    if (!boot_state_ready) return null;
    if (bootfs_image_va == 0) return null;
    if ((boot_state.flags & vfs_boot_config_flag_bootfs_present) == 0) return null;
    if (boot_state.bootfs_size_bytes < @sizeOf(bootfs_format.BootFsHeader)) return null;
    const header: *const bootfs_format.BootFsHeader = @ptrFromInt(bootfs_image_va);
    if (header.magic != bootfs_format.magic) return null;
    if (header.version != bootfs_format.version) return null;
    if (header.header_bytes != @sizeOf(bootfs_format.BootFsHeader)) return null;
    if (header.image_bytes == 0 or header.image_bytes > boot_state.bootfs_size_bytes) return null;
    if (header.entry_table_offset + header.entry_bytes > header.image_bytes) return null;
    if (header.string_table_offset + header.string_table_bytes > header.image_bytes) return null;
    if (header.data_offset + header.data_bytes > header.image_bytes) return null;
    return header;
}

fn alignUpToPageBytes(size_bytes: u64) u64 {
    return (size_bytes + vfs_protocol.page_bytes - 1) & ~@as(u64, vfs_protocol.page_bytes - 1);
}

fn ensureBootFsPrefixMapped(required_end_bytes: u64) bool {
    if ((boot_state.flags & vfs_boot_config_flag_bootfs_present) == 0) return true;
    if (!ensureBootFsImageVa()) {
        _ = userLog("VFS: bootfs image VA reserve failed\n");
        return false;
    }
    const vm_token = boot_state.bootfs_vm_token;
    _ = image_abi.decodeVmObjectToken(vm_token) orelse {
        _ = userLog("VFS: bootfs prefix decode failed\n");
        return false;
    };
    const capped_end = @min(required_end_bytes, boot_state.bootfs_size_bytes);
    if (capped_end == 0) {
        _ = userLog("VFS: bootfs prefix zero bytes\n");
        return false;
    }
    if (capped_end <= bootfs_mapped_prefix_bytes) return true;
    const prefix_token = sliceVmObject(vm_token, 0, capped_end, .{ .read = true, .map = true });
    if (image_abi.decodeVmObjectToken(prefix_token) == null) {
        _ = userLog("VFS: bootfs prefix slice failed\n");
        userLogHex("VFS: bootfs prefix slice ret=", prefix_token);
        return false;
    }
    const map_ret = mapVmObject(prefix_token, bootfs_image_va);
    if (map_ret != syscall_ok) {
        _ = userLog("VFS: bootfs prefix map failed\n");
        userLogHex("VFS: bootfs prefix map ret=", map_ret);
        return false;
    }
    bootfs_mapped_prefix_bytes = capped_end;
    return true;
}

fn ensureBootFsMetadataMapped() bool {
    if (!ensureBootFsPrefixMapped(vfs_protocol.page_bytes)) return false;
    const header = bootfsHeader() orelse {
        _ = userLog("VFS: bootfs header invalid after prefix map\n");
        return false;
    };
    var metadata_end = @as(u64, @sizeOf(bootfs_format.BootFsHeader));
    metadata_end = @max(metadata_end, header.entry_table_offset + header.entry_bytes);
    metadata_end = @max(metadata_end, header.string_table_offset + header.string_table_bytes);
    if (!ensureBootFsPrefixMapped(metadata_end)) {
        _ = userLog("VFS: bootfs metadata prefix map failed\n");
        return false;
    }
    return true;
}

fn mapBootFsWindow(file_offset: u64, length: usize, target_va: usize) ?[]const u8 {
    if (length == 0) return &[_]u8{};
    if (target_va == 0) return null;
    const vm_token = boot_state.bootfs_vm_token;
    _ = image_abi.decodeVmObjectToken(vm_token) orelse return null;
    const aligned_offset = file_offset & ~@as(u64, 0xFFF);
    const in_page_offset: usize = @intCast(file_offset - aligned_offset);
    const window_bytes = @as(u64, @intCast(in_page_offset + length));
    const window_token = sliceVmObject(vm_token, aligned_offset, window_bytes, .{ .read = true, .map = true });
    if (image_abi.decodeVmObjectToken(window_token) == null) return null;
    if (mapVmObject(window_token, target_va) != syscall_ok) return null;
    const src: [*]const u8 = @ptrFromInt(target_va + in_page_offset);
    return src[0..length];
}

fn bootfsEntries(header: *const bootfs_format.BootFsHeader) []const bootfs_format.BootFsEntry {
    const ptr: [*]const bootfs_format.BootFsEntry = @ptrFromInt(bootfs_image_va + @as(usize, @intCast(header.entry_table_offset)));
    return ptr[0..header.entry_count];
}

fn bootfsStringTable(header: *const bootfs_format.BootFsHeader) []const u8 {
    const ptr: [*]const u8 = @ptrFromInt(bootfs_image_va + @as(usize, @intCast(header.string_table_offset)));
    return ptr[0..@intCast(header.string_table_bytes)];
}

fn bootfsPathForEntry(header: *const bootfs_format.BootFsHeader, entry: bootfs_format.BootFsEntry) ?[]const u8 {
    const table = bootfsStringTable(header);
    const begin: usize = entry.path_offset;
    const end = begin + entry.path_bytes;
    if (entry.kind != bootfs_format.kind_regular) return null;
    if (end > table.len) return null;
    return table[begin..end];
}

fn bootfsObjectId(abs_path: []const u8, kind: ResolvedKind) u64 {
    var hash: u64 = 0xcbf2_9ce4_8422_2325;
    for (abs_path) |byte| {
        hash ^= byte;
        hash *%= 0x0000_0100_0000_01b3;
    }
    hash ^= switch (kind) {
        .dir => 0xD1D0_0000_0000_0001,
        .file => 0xF11E_0000_0000_0001,
    };
    hash &= 0x7FFF_FFFF_FFFF_FFFF;
    if (hash == 0 or hash == bootfs_root_object_id) hash +%= 0x1000;
    return hash;
}

fn bootfsDirectChildForPath(parent_abs_path: []const u8, entry_path: []const u8, file_size: u64) ?BootFsChild {
    if (entry_path.len < 2 or entry_path[0] != '/') return null;
    const remainder = if (std.mem.eql(u8, parent_abs_path, "/")) blk: {
        break :blk entry_path[1..];
    } else blk: {
        if (!std.mem.startsWith(u8, entry_path, parent_abs_path)) return null;
        if (entry_path.len <= parent_abs_path.len or entry_path[parent_abs_path.len] != '/') return null;
        break :blk entry_path[parent_abs_path.len + 1 ..];
    };
    if (remainder.len == 0) return null;
    const slash_index = std.mem.indexOfScalar(u8, remainder, '/');
    if (slash_index) |idx| {
        if (idx == 0) return null;
        const abs_end = if (std.mem.eql(u8, parent_abs_path, "/")) 1 + idx else parent_abs_path.len + 1 + idx;
        return .{
            .name = remainder[0..idx],
            .abs_path = entry_path[0..abs_end],
            .kind = .dir,
            .size_bytes = 0,
        };
    }
    return .{
        .name = remainder,
        .abs_path = entry_path,
        .kind = .file,
        .size_bytes = file_size,
    };
}

fn bootfsChildAt(parent: ResolvedNode, cursor: u64) ?BootFsChild {
    const header = bootfsHeader() orelse return null;
    const entries = bootfsEntries(header);
    var unique_index: u64 = 0;
    var prev_abs_path: []const u8 = "";
    var has_prev = false;
    for (entries) |entry| {
        const path = bootfsPathForEntry(header, entry) orelse continue;
        const child = bootfsDirectChildForPath(parent.abs_path, path, entry.data_bytes) orelse continue;
        if (has_prev and std.mem.eql(u8, child.abs_path, prev_abs_path)) continue;
        if (unique_index == cursor) return child;
        prev_abs_path = child.abs_path;
        has_prev = true;
        unique_index += 1;
    }
    return null;
}

fn bootfsChildByName(parent: ResolvedNode, name: []const u8) ?BootFsChild {
    const header = bootfsHeader() orelse return null;
    const entries = bootfsEntries(header);
    var prev_abs_path: []const u8 = "";
    var has_prev = false;
    for (entries) |entry| {
        const path = bootfsPathForEntry(header, entry) orelse continue;
        const child = bootfsDirectChildForPath(parent.abs_path, path, entry.data_bytes) orelse continue;
        if (has_prev and std.mem.eql(u8, child.abs_path, prev_abs_path)) continue;
        prev_abs_path = child.abs_path;
        has_prev = true;
        if (std.mem.eql(u8, child.name, name)) return child;
    }
    return null;
}

fn bootfsFileData(abs_path: []const u8) ?BootFsFileData {
    const header = bootfsHeader() orelse return null;
    const entries = bootfsEntries(header);
    for (entries) |entry| {
        const path = bootfsPathForEntry(header, entry) orelse continue;
        if (!std.mem.eql(u8, path, abs_path)) continue;
        if (entry.data_offset > header.image_bytes) return null;
        if (entry.data_bytes > header.image_bytes - entry.data_offset) return null;
        return .{
            .data_offset = entry.data_offset,
            .data_bytes = entry.data_bytes,
        };
    }
    return null;
}

fn nodeFromBootFsChild(child: BootFsChild) ResolvedNode {
    return .{
        .source_mount_token = bootfs_mount_token,
        .object_id = bootfsObjectId(child.abs_path, child.kind),
        .kind = child.kind,
        .size_bytes = child.size_bytes,
        .abs_path = child.abs_path,
    };
}

fn resolveLookupPath(base: ResolvedNode, path: []const u8) LookupPathResult {
    var current = base;
    var remaining = path;
    if (remaining.len == 0) return .{ .node = current };

    while (true) {
        const component_end = std.mem.indexOfScalar(u8, remaining, '/') orelse remaining.len;
        if (component_end == 0) return .invalid;

        const component = remaining[0..component_end];
        if (std.mem.eql(u8, component, ".") or std.mem.eql(u8, component, "..")) return .invalid;
        if (current.kind != .dir) return .not_dir;

        const child = bootfsChildByName(current, component) orelse return .not_found;
        current = nodeFromBootFsChild(child);

        if (component_end == remaining.len) return .{ .node = current };

        remaining = remaining[component_end + 1 ..];
        if (remaining.len == 0) {
            return if (current.kind == .dir)
                .{ .node = current }
            else
                .not_dir;
        }
    }
}

fn resolveRootPath(state: *const BootState, path: []const u8) ?ResolvedNode {
    if (path.len == 0 or std.mem.eql(u8, path, "/")) {
        return rootNode(state);
    }
    return null;
}

fn statResolved(node: ResolvedNode) ResolvedNode {
    return node;
}

fn runBootSelfTest(state: *const BootState) bool {
    const root = resolveRootPath(state, "/") orelse {
        _ = userLog("VFS: self-test lookup / failed\n");
        return false;
    };
    if (root.source_mount_token != bootfs_mount_token or root.kind != .dir) {
        _ = userLog("VFS: self-test lookup / mismatch\n");
        return false;
    }
    _ = userLog("VFS: self-test lookup / ok\n");

    const stat_root = statResolved(root);
    if (stat_root.kind != .dir or stat_root.size_bytes != 0) {
        _ = userLog("VFS: self-test stat / mismatch\n");
        return false;
    }
    _ = userLog("VFS: self-test stat / dir ok\n");

    if ((state.flags & vfs_boot_config_flag_bootfs_present) != 0 and bootfsHeader() != null) {
        _ = userLog("VFS: bootfs handoff present\n");
    } else {
        _ = userLog("VFS: bootfs handoff absent\n");
    }
    return true;
}

fn bootfsExpectedPageCount(state: *const BootState) usize {
    if ((state.flags & vfs_boot_config_flag_bootfs_present) == 0) return 0;
    if (state.bootfs_size_bytes == 0) return 0;
    return @intCast((state.bootfs_size_bytes + vfs_protocol.page_bytes - 1) / vfs_protocol.page_bytes);
}

fn waitForBootResources() bool {
    var bootfs_ready = (boot_state.flags & vfs_boot_config_flag_bootfs_present) == 0;
    var bootfs_token_ready = false;

    if ((boot_state.flags & vfs_boot_config_flag_bootfs_present) != 0 and image_abi.decodeVmObjectToken(boot_state.bootfs_vm_token) != null) {
        bootfs_token_ready = true;
        _ = userLog("VFS: bootfs vm token ready\n");
        _ = userLog("VFS: bootfs metadata map begin\n");
        if (!ensureBootFsMetadataMapped()) {
            _ = userLog("VFS: bootfs metadata map failed\n");
            return false;
        }
        _ = userLog("VFS: bootfs metadata map done\n");
        bootfs_ready = bootfsHeader() != null;
    }
    if ((boot_state.flags & vfs_boot_config_flag_bootfs_present) != 0 and bootfs_ready) {
        _ = userLog("VFS: bootfs handoff present\n");
    } else if ((boot_state.flags & vfs_boot_config_flag_bootfs_present) == 0) {
        _ = userLog("VFS: bootfs handoff absent\n");
    }

    var spin: usize = 0;
    while (spin < 200000 and !bootfs_ready) : (spin += 1) {
        asm volatile ("pause");
        boot_state = parseBootState() orelse continue;
        if (!bootfs_ready and (boot_state.flags & vfs_boot_config_flag_bootfs_present) != 0 and image_abi.decodeVmObjectToken(boot_state.bootfs_vm_token) != null) {
            if (!bootfs_token_ready) {
                bootfs_token_ready = true;
                _ = userLog("VFS: bootfs vm token ready\n");
            }
            _ = userLog("VFS: bootfs metadata map begin\n");
            if (!ensureBootFsMetadataMapped()) {
                _ = userLog("VFS: bootfs metadata map failed\n");
                return false;
            }
            _ = userLog("VFS: bootfs metadata map done\n");
            if (bootfsHeader() != null) {
                bootfs_ready = true;
                _ = userLog("VFS: bootfs handoff present\n");
            }
        }
    }

    while (!bootfs_ready) {
        _ = waitEvent(false, session_poll_timeout_ticks);
        boot_state = parseBootState() orelse {
            _ = userLog("VFS: config lost during bootstrap\n");
            return false;
        };
        if (!bootfs_ready and (boot_state.flags & vfs_boot_config_flag_bootfs_present) != 0 and image_abi.decodeVmObjectToken(boot_state.bootfs_vm_token) != null) {
            if (!bootfs_token_ready) {
                bootfs_token_ready = true;
                _ = userLog("VFS: bootfs vm token ready\n");
            }
            _ = userLog("VFS: bootfs metadata map begin\n");
            if (!ensureBootFsMetadataMapped()) {
                _ = userLog("VFS: bootfs metadata map failed\n");
                return false;
            }
            _ = userLog("VFS: bootfs metadata map done\n");
            if (bootfsHeader() != null) {
                bootfs_ready = true;
                _ = userLog("VFS: bootfs handoff present\n");
            }
        }
    }
    return true;
}

fn requestHeader(session: *const Session) *volatile vfs_protocol.VfsRequestHeader {
    return @ptrFromInt(session.request_va);
}

fn responseHeader(session: *const Session) *volatile vfs_protocol.VfsResponseHeader {
    return @ptrFromInt(session.response_va);
}

fn requestPayload(session: *const Session) [*]volatile u8 {
    return @ptrFromInt(session.request_va + vfs_protocol.request_header_bytes);
}

fn responsePayload(session: *const Session) [*]volatile u8 {
    return @ptrFromInt(session.response_va + vfs_protocol.response_header_bytes);
}

fn clearPage(va: u64) void {
    const bytes: [*]volatile u8 = @ptrFromInt(va);
    var i: usize = 0;
    while (i < vfs_protocol.page_bytes) : (i += 1) {
        bytes[i] = 0;
    }
}

fn objectKindForNode(kind: ResolvedKind) fs_abi.ObjectKind {
    return switch (kind) {
        .dir => .vnode_dir,
        .file => .vnode_file,
    };
}

fn modeBitsForNode(kind: ResolvedKind) u32 {
    return switch (kind) {
        .dir => 0x4000,
        .file => 0x8000,
    };
}

fn openFileObjectId(node: ResolvedNode) u64 {
    var object_id = node.object_id ^ 0x0F0E_0000_0000_0001;
    object_id &= 0x7FFF_FFFF_FFFF_FFFF;
    if (object_id == 0 or object_id == bootfs_root_object_id) object_id +%= 0x2000;
    return object_id;
}

fn rememberSessionObject(session: *Session, client_token: u64, kind: fs_abi.ObjectKind, node: ResolvedNode) bool {
    for (&session.objects) |*entry| {
        if (entry.active and entry.client_token == client_token) {
            entry.kind = kind;
            entry.node = node;
            return true;
        }
    }
    for (&session.objects) |*entry| {
        if (!entry.active) {
            entry.* = .{
                .active = true,
                .client_token = client_token,
                .kind = kind,
                .node = node,
            };
            return true;
        }
    }
    return false;
}

fn resolveSessionObject(session: *const Session, token: u64) ?SessionObject {
    if (token == 0) {
        return .{
            .active = true,
            .client_token = 0,
            .kind = .vnode_dir,
            .node = rootNode(&boot_state),
        };
    }
    for (session.objects) |entry| {
        if (entry.active and entry.client_token == token) return entry;
    }
    return null;
}

fn resolveSessionNode(session: *const Session, token: u64) ?ResolvedNode {
    const object = resolveSessionObject(session, token) orelse return null;
    return object.node;
}

fn writeResponseHeader(
    session: *Session,
    op: vfs_protocol.Opcode,
    request_seq: u64,
    status: vfs_protocol.Status,
    result_token: u64,
    file_bytes: u64,
    cursor_next: u64,
    object_kind: fs_abi.ObjectKind,
    inline_bytes: u16,
) void {
    const response = responseHeader(session);
    response.magic = vfs_protocol.response_magic;
    response.version = vfs_protocol.version;
    response.op = vfs_protocol.opcodeRaw(op);
    response.status = vfs_protocol.statusRaw(status);
    response.result_flags = 0;
    response.result_token = result_token;
    response.file_bytes = file_bytes;
    response.cursor_next = cursor_next;
    response.inline_bytes = inline_bytes;
    response.object_kind = vfs_protocol.objectKindRaw(object_kind);
    response.reserved0 = 0;
    response.reserved1 = 0;
    response.arg0 = 0;
    response.arg1 = 0;
    compilerBarrier();
    response.response_seq = request_seq;
}

fn replyStatus(session: *Session, op: vfs_protocol.Opcode, request_seq: u64, status: vfs_protocol.Status) void {
    clearPage(session.response_va);
    writeResponseHeader(session, op, request_seq, status, 0, 0, 0, .none, 0);
}

fn replyLookup(session: *Session, request_seq: u64, client_token: u64, node: ResolvedNode) void {
    clearPage(session.response_va);
    writeResponseHeader(session, .lookup, request_seq, .ok, client_token, node.size_bytes, 0, objectKindForNode(node.kind), 0);
}

fn replyOpen(session: *Session, request_seq: u64, client_token: u64, node: ResolvedNode) void {
    clearPage(session.response_va);
    writeResponseHeader(session, .open, request_seq, .ok, client_token, node.size_bytes, 0, .open_file, 0);
}

fn replyOpenExec(session: *Session, request_seq: u64, client_token: u64, node: ResolvedNode) void {
    clearPage(session.response_va);
    writeResponseHeader(session, .open_exec, request_seq, .ok, client_token, node.size_bytes, 0, .exec, 0);
}

fn replyStat(session: *Session, request_seq: u64, node: ResolvedNode) void {
    clearPage(session.response_va);
    const stat: *volatile vfs_protocol.VfsStatRecord = @ptrFromInt(session.response_va + vfs_protocol.response_header_bytes);
    stat.object_kind = vfs_protocol.objectKindRaw(objectKindForNode(node.kind));
    stat.reserved0 = [_]u8{0} ** 7;
    stat.size_bytes = node.size_bytes;
    stat.mode_bits = modeBitsForNode(node.kind);
    stat.reserved1 = 0;
    stat.mtime_unix_sec = 0;
    stat.reserved2 = [_]u64{0} ** 2;
    compilerBarrier();
    writeResponseHeader(
        session,
        .stat,
        request_seq,
        .ok,
        0,
        node.size_bytes,
        0,
        objectKindForNode(node.kind),
        @as(u16, vfs_protocol.stat_record_bytes),
    );
}

fn replyRead(session: *Session, request_seq: u64, request_offset: u64, file_size: u64, bytes: []const u8) void {
    clearPage(session.response_va);
    const payload = responsePayload(session);
    var i: usize = 0;
    while (i < bytes.len) : (i += 1) {
        payload[i] = bytes[i];
    }
    compilerBarrier();
    writeResponseHeader(
        session,
        .read,
        request_seq,
        .ok,
        0,
        file_size,
        request_offset + @as(u64, @intCast(bytes.len)),
        .open_file,
        @as(u16, @intCast(bytes.len)),
    );
}

fn replyReaddirEnd(session: *Session, request_seq: u64, cursor_next: u64, node: ResolvedNode) void {
    clearPage(session.response_va);
    writeResponseHeader(
        session,
        .readdir,
        request_seq,
        .end_of_dir,
        0,
        node.size_bytes,
        cursor_next,
        objectKindForNode(node.kind),
        0,
    );
}

fn replyReaddirEntry(
    session: *Session,
    request_seq: u64,
    cursor_next: u64,
    node: ResolvedNode,
    child: BootFsChild,
) void {
    clearPage(session.response_va);
    const record: *volatile vfs_protocol.VfsDirentRecord = @ptrFromInt(session.response_va + vfs_protocol.response_header_bytes);
    record.next_cursor = cursor_next;
    record.object_kind = vfs_protocol.objectKindRaw(objectKindForNode(child.kind));
    record.reserved0 = [_]u8{0} ** 7;
    record.name_bytes = @intCast(child.name.len);
    record.reserved1 = 0;
    record.reserved2 = 0;
    const payload = responsePayload(session);
    var i: usize = 0;
    while (i < child.name.len) : (i += 1) {
        payload[vfs_protocol.dirent_record_bytes + i] = child.name[i];
    }
    compilerBarrier();
    writeResponseHeader(
        session,
        .readdir,
        request_seq,
        .ok,
        0,
        node.size_bytes,
        cursor_next,
        objectKindForNode(node.kind),
        @as(u16, @intCast(vfs_protocol.dirent_record_bytes + child.name.len)),
    );
}

fn requestPath(session: *const Session) []const u8 {
    const request = requestHeader(session);
    const bytes: usize = @min(@as(usize, request.path_bytes), vfs_protocol.request_payload_bytes);
    const payload: [*]const u8 = @ptrCast(@volatileCast(requestPayload(session)));
    return payload[0..bytes];
}

fn dirClientRights() fs_abi.Rights {
    return .{
        .lookup = true,
        .read = true,
        .readdir = true,
        .stat = true,
    };
}

fn fileClientRights() fs_abi.Rights {
    return .{
        .read = true,
        .stat = true,
    };
}

fn openFileClientRights() fs_abi.Rights {
    return .{
        .read = true,
        .stat = true,
    };
}

fn nodeServerRights(node: ResolvedNode) fs_abi.Rights {
    return switch (node.kind) {
        .dir => .{
            .lookup = true,
            .read = true,
            .readdir = true,
            .stat = true,
            .grant = true,
        },
        .file => .{
            .read = true,
            .stat = true,
            .grant = true,
        },
    };
}

fn openFileServerRights() fs_abi.Rights {
    return .{
        .read = true,
        .stat = true,
        .grant = true,
    };
}

fn nodeClientRights(node: ResolvedNode) fs_abi.Rights {
    return switch (node.kind) {
        .dir => dirClientRights(),
        .file => fileClientRights(),
    };
}

fn grantNodeToken(session: *Session, node: ResolvedNode) ?u64 {
    const object_kind = objectKindForNode(node.kind);
    const client_token = allocFsToken();
    if (!rememberSessionObject(session, client_token, object_kind, node)) return null;
    return client_token;
}

fn grantOpenFileToken(session: *Session, node: ResolvedNode) ?u64 {
    const client_token = allocFsToken();
    if (!rememberSessionObject(session, client_token, .open_file, node)) return null;
    return client_token;
}

fn grantExecToken(session: *Session, node: ResolvedNode) ?u64 {
    const file_data = bootfsFileData(node.abs_path) orelse return null;
    const server_vm_token = sliceVmObject(
        boot_state.bootfs_vm_token,
        file_data.data_offset,
        file_data.data_bytes,
        .{ .read = true },
    );
    if (image_abi.decodeVmObjectToken(server_vm_token) == null) return null;
    const server_exec_token = installExecImage(server_vm_token, .{ .exec = true, .grant = true });
    if (image_abi.decodeExecImageToken(server_exec_token) == null) return null;
    const client_token = grantExecImage(server_exec_token, session.client_process_slot, .{ .exec = true });
    if (image_abi.decodeExecImageToken(client_token) == null) return null;
    return client_token;
}

fn bootfsFileHasElfMagic(node: ResolvedNode) bool {
    const file_data = bootfsFileData(node.abs_path) orelse return false;
    if (file_data.data_bytes < 4) return false;
    if (!ensureBootFsWindowVas()) return false;
    const src = mapBootFsWindow(file_data.data_offset, 4, bootfs_probe_va) orelse return false;
    return src[0] == 0x7F and src[1] == 'E' and src[2] == 'L' and src[3] == 'F';
}

fn handleLookup(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    var node = rootNode(&boot_state);
    var path = requestPath(session);

    if (request.object_token != 0) {
        const base_object = resolveSessionObject(session, request.object_token) orelse {
            replyStatus(session, .lookup, request_seq, .not_found);
            return;
        };
        node = base_object.node;
        if (path.len == 0) {
            replyStatus(session, .lookup, request_seq, .invalid);
            return;
        }
        if (path[0] != '/') {
            if (base_object.kind != .vnode_dir or base_object.node.kind != .dir) {
                replyStatus(session, .lookup, request_seq, .not_dir);
                return;
            }
        }
    }

    if (path.len == 0 or std.mem.eql(u8, path, "/")) {
        node = rootNode(&boot_state);
    } else {
        if (path[0] == '/') {
            path = path[1..];
            node = rootNode(&boot_state);
            if (path.len == 0) {
                const client_token = grantNodeToken(session, node) orelse {
                    replyStatus(session, .lookup, request_seq, .busy);
                    return;
                };
                replyLookup(session, request_seq, client_token, node);
                return;
            }
        }

        switch (resolveLookupPath(node, path)) {
            .node => |resolved| node = resolved,
            .invalid => {
                replyStatus(session, .lookup, request_seq, .invalid);
                return;
            },
            .not_found => {
                replyStatus(session, .lookup, request_seq, .not_found);
                return;
            },
            .not_dir => {
                replyStatus(session, .lookup, request_seq, .not_dir);
                return;
            },
        }
    }

    const client_token = grantNodeToken(session, node) orelse {
        replyStatus(session, .lookup, request_seq, .busy);
        return;
    };
    replyLookup(session, request_seq, client_token, node);
}

fn handleOpen(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = resolveSessionObject(session, request.object_token) orelse {
        replyStatus(session, .open, request_seq, .not_found);
        return;
    };
    if (object.kind == .vnode_dir or object.node.kind == .dir) {
        replyStatus(session, .open, request_seq, .is_dir);
        return;
    }
    if (object.kind != .vnode_file or object.node.kind != .file) {
        replyStatus(session, .open, request_seq, .invalid);
        return;
    }
    if (request.flags != 0) {
        replyStatus(session, .open, request_seq, .not_supported);
        return;
    }
    if (bootfsFileData(object.node.abs_path) == null) {
        replyStatus(session, .open, request_seq, .not_found);
        return;
    }
    const client_token = grantOpenFileToken(session, object.node) orelse {
        replyStatus(session, .open, request_seq, .busy);
        return;
    };
    replyOpen(session, request_seq, client_token, object.node);
}

fn handleOpenExec(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = resolveSessionObject(session, request.object_token) orelse {
        replyStatus(session, .open_exec, request_seq, .not_found);
        return;
    };
    if (object.kind == .vnode_dir or object.node.kind == .dir) {
        replyStatus(session, .open_exec, request_seq, .is_dir);
        return;
    }
    if (object.kind != .vnode_file or object.node.kind != .file) {
        replyStatus(session, .open_exec, request_seq, .invalid);
        return;
    }
    if (!bootfsFileHasElfMagic(object.node)) {
        replyStatus(session, .open_exec, request_seq, .invalid);
        return;
    }
    const client_token = grantExecToken(session, object.node) orelse {
        replyStatus(session, .open_exec, request_seq, .busy);
        return;
    };
    replyOpenExec(session, request_seq, client_token, object.node);
}

fn handleStat(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const node = resolveSessionNode(session, request.object_token) orelse {
        replyStatus(session, .stat, request_seq, .not_found);
        return;
    };
    replyStat(session, request_seq, node);
}

fn handleRead(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = resolveSessionObject(session, request.object_token) orelse {
        replyStatus(session, .read, request_seq, .not_found);
        return;
    };
    if (object.kind != .open_file or object.node.kind != .file) {
        replyStatus(session, .read, request_seq, .invalid);
        return;
    }
    const file_data = bootfsFileData(object.node.abs_path) orelse {
        replyStatus(session, .read, request_seq, .io_error);
        return;
    };
    const read_offset = @min(request.offset, file_data.data_bytes);
    const remaining = file_data.data_bytes - read_offset;
    const copy_len_u64 = @min(
        @as(u64, request.length),
        @min(remaining, @as(u64, @intCast(vfs_protocol.response_payload_bytes))),
    );
    const copy_len: usize = @intCast(copy_len_u64);
    if (!ensureBootFsWindowVas()) {
        replyStatus(session, .read, request_seq, .io_error);
        return;
    }
    const src = mapBootFsWindow(file_data.data_offset + read_offset, copy_len, bootfs_read_window_va) orelse {
        replyStatus(session, .read, request_seq, .io_error);
        return;
    };
    replyRead(session, request_seq, request.offset, file_data.data_bytes, src);
}

fn handleReaddir(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = resolveSessionObject(session, request.object_token) orelse {
        replyStatus(session, .readdir, request_seq, .not_found);
        return;
    };
    if (object.kind != .vnode_dir or object.node.kind != .dir) {
        replyStatus(session, .readdir, request_seq, .not_dir);
        return;
    }
    const node = object.node;
    if (request.length == 0) {
        replyStatus(session, .readdir, request_seq, .invalid);
        return;
    }
    if (bootfsChildAt(node, request.offset)) |child| {
        replyReaddirEntry(session, request_seq, request.offset + 1, node, child);
        return;
    }
    replyReaddirEnd(session, request_seq, request.offset, node);
}

fn handleClose(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    for (&session.objects) |*entry| {
        if (!entry.active or entry.client_token != request.object_token) continue;
        if (entry.kind != .open_file) {
            replyStatus(session, .close, request_seq, .invalid);
            return;
        }
        entry.* = .{};
        replyStatus(session, .close, request_seq, .ok);
        return;
    }
    replyStatus(session, .close, request_seq, .not_found);
}

fn processSessionRequest(session: *Session) void {
    const request = requestHeader(session);
    if (request.magic != vfs_protocol.request_magic or request.version != vfs_protocol.version) return;
    const request_seq = request.request_seq;
    if (request_seq == 0 or request_seq <= session.last_completed_seq) return;
    const op = std.meta.intToEnum(vfs_protocol.Opcode, request.op) catch {
        replyStatus(session, .connect, request_seq, .invalid);
        session.last_completed_seq = request_seq;
        return;
    };
    switch (op) {
        .connect => replyStatus(session, .connect, request_seq, .ok),
        .lookup => handleLookup(session, request_seq),
        .open => handleOpen(session, request_seq),
        .open_exec => handleOpenExec(session, request_seq),
        .read => handleRead(session, request_seq),
        .readdir => handleReaddir(session, request_seq),
        .stat => handleStat(session, request_seq),
        .close => handleClose(session, request_seq),
        else => replyStatus(session, op, request_seq, .not_supported),
    }
    session.last_completed_seq = request_seq;
}

fn handleConnectRequest(request_paddr: u64) void {
    for (&sessions) |*session| {
        if (session.active) continue;
        const request_page = user_vm.mapPageAtDynamicVa(request_paddr, false) orelse {
            _ = userLog("VFS: session request map failed\n");
            return;
        };
        const req_va: u64 = @intCast(request_page.va);
        const request: *volatile vfs_protocol.VfsRequestHeader = @ptrFromInt(req_va);
        if (request.magic != vfs_protocol.request_magic or
            request.version != vfs_protocol.version or
            request.op != vfs_protocol.opcodeRaw(.connect) or
            request.request_seq == 0 or
            request.arg0 < 0x1000)
        {
            _ = userLog("VFS: connect request invalid\n");
            return;
        }
        const response_page = user_vm.mapPageAtDynamicVa(request.arg0, true) orelse {
            _ = userLog("VFS: session response map failed\n");
            return;
        };
        const resp_va: u64 = @intCast(response_page.va);
        session.* = .{
            .active = true,
            .client_process_slot = request.arg1,
            .request_paddr = request_paddr,
            .response_paddr = request.arg0,
            .request_va = req_va,
            .response_va = resp_va,
            .last_completed_seq = 0,
        };
        clearPage(resp_va);
        writeResponseHeader(session, .connect, request.request_seq, .ok, 0, 0, 0, .none, 0);
        session.last_completed_seq = request.request_seq;
        _ = userLog("VFS: session connect ok\n");
        return;
    }
    _ = userLog("VFS: session table full\n");
}

fn pollSessions() void {
    for (&sessions) |*session| {
        if (!session.active) continue;
        processSessionRequest(session);
    }
}

pub export fn _start() noreturn {
    _ = userLog("VFS: started\n");

    boot_state = parseBootState() orelse {
        _ = userLog("VFS: config magic mismatch\n");
        haltForever();
    };
    boot_state_ready = true;

    _ = userLog("VFS: config ok\n");
    if (boot_state.endpoint_id != 0) {
        _ = userLog("VFS: endpoint ready\n");
    }
    if (!waitForBootResources()) {
        haltForever();
    }
    if ((boot_state.flags & vfs_boot_config_flag_bootfs_present) != 0) {
        if (bootfsHeader() != null) {
            _ = userLog("VFS: bootfs image ready\n");
        } else {
            _ = userLog("VFS: bootfs image invalid\n");
            haltForever();
        }
    }
    _ = userLog("VFS: boot ready\n");

    while (true) {
        const received = waitEvent(true, session_poll_timeout_ticks);
        if (received >= 0x1000) {
            const request_paddr = acceptCapTransfer(received);
            if (request_paddr >= 0x1000) {
                handleConnectRequest(request_paddr);
            } else {
                _ = userLog("VFS: accept cap transfer failed\n");
            }
        }
        pollSessions();
    }
}
