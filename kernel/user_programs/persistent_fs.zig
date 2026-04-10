const std = @import("std");
const block_client = @import("block_client.zig");
const cap_transfer_abi = @import("cap_transfer_abi.zig");
const fs_abi = @import("fs_abi.zig");
const fs_protocol = @import("fs_protocol.zig");
const image_abi = @import("image_abi.zig");
const persistent_fs_bootstrap = @import("persistent_fs_bootstrap_abi.zig");
const process_abi = @import("process_abi.zig");

const syscall_alloc_page: u64 = 0x1;
const syscall_log: u64 = 0x9;
const syscall_map_page: u64 = 0x2;
const syscall_wait_event: u64 = 0x17;
const syscall_install_endpoint: u64 = 0x26;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_get_process_slot: u64 = 0x2E;

const config_page_va: u64 = process_abi.standard_config_target_va;
const block_request_va: u64 = 0x3C10_4000;
const block_response_va: u64 = 0x3C10_5000;
const session_base_va: u64 = 0x3C11_0000;
const session_va_stride: u64 = 0x2000;
const reply_endpoint_id_base: u64 = 0xD0;
const max_sessions: usize = 4;
const max_session_objects: usize = 16;
const max_block_bytes: usize = 4096;
const max_dir_entries: usize = 64;
const max_name_bytes: usize = 32;
const max_exec_file_bytes: usize = 256 * 1024;
const max_exec_slots: usize = 16;
const page_bytes: usize = 4096;
const exec_slot_base_va: u64 = 0x3C20_0000;
const exec_slot_va_stride: u64 = max_exec_file_bytes;
const fs_region_start_block: u64 = 2048;
const volume_magic: u64 = 0x3153_4650; // "PFS1"
const volume_version: u64 = 1;
const root_mount_object_id: u64 = 0x5053_4653; // "PSFS"
const root_dir_object_id: u64 = 0x5053_4654; // "PSFT"
const vnode_file_object_id_base: u64 = 0x5053_4700;
const open_file_object_id_base: u64 = 0x5053_4800;
const dir_entry_flag_used: u32 = 1;
const dir_mode_bits: u32 = 0x4000;
const file_mode_bits: u32 = 0x8000;

const SessionObjectKind = enum {
    mount,
    root_dir,
    vnode_file,
    open_file,
};

const SessionObject = struct {
    active: bool = false,
    client_token: u64 = 0,
    kind: SessionObjectKind = .mount,
    file_index: u16 = 0,
    _reserved: u16 = 0,
};

const Session = struct {
    active: bool = false,
    client_process_slot: u64 = 0,
    request_va: u64 = 0,
    response_va: u64 = 0,
    reply_endpoint_id: u64 = 0,
    last_completed_seq: u64 = 0,
    objects: [max_session_objects]SessionObject = [_]SessionObject{.{}} ** max_session_objects,
};

const VolumeSuperblock = extern struct {
    magic: u64 = volume_magic,
    version: u64 = volume_version,
    block_size: u64 = 0,
    fs_start_block: u64 = fs_region_start_block,
    dir_start_block: u64 = 0,
    dir_block_count: u64 = 0,
    data_start_block: u64 = 0,
    next_free_block: u64 = 0,
};

const VolumeDirEntry = extern struct {
    flags: u32 = 0,
    name_bytes: u16 = 0,
    reserved0: u16 = 0,
    file_size: u64 = 0,
    start_block: u64 = 0,
    block_count: u32 = 0,
    reserved1: u32 = 0,
    name: [max_name_bytes]u8 = [_]u8{0} ** max_name_bytes,
};

const ExecImageSlot = struct {
    active: bool = false,
    page_count: u16 = 0,
    file_index: u16 = 0,
    _reserved: u32 = 0,
};

comptime {
    std.debug.assert(@sizeOf(VolumeSuperblock) == 64);
    std.debug.assert(@sizeOf(VolumeDirEntry) == 64);
    std.debug.assert(max_exec_file_bytes % page_bytes == 0);
}

var endpoint_id: u64 = 0;
var admin_token: u64 = 0;
var process_slot: u64 = 0;
var block_size: u64 = 0;
var capacity_blocks: u64 = 0;
var root_mount_server_token: u64 = 0;
var root_dir_server_token: u64 = 0;
var block_state: block_client.Client = undefined;
var block_ready = false;
var volume_superblock = VolumeSuperblock{};
var volume_dir_entries: [max_dir_entries]VolumeDirEntry = [_]VolumeDirEntry{.{}} ** max_dir_entries;
var sessions: [max_sessions]Session = [_]Session{.{}} ** max_sessions;
var block_buffer: [max_block_bytes]u8 align(16) = [_]u8{0} ** max_block_bytes;
var io_buffer: [max_block_bytes]u8 align(16) = [_]u8{0} ** max_block_bytes;
var exec_slots: [max_exec_slots]ExecImageSlot = [_]ExecImageSlot{.{}} ** max_exec_slots;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn allocPage() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_page),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
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

fn getProcessSlot() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_get_process_slot),
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

fn installEndpoint(endpoint: u64, target_process_slot: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_install_endpoint),
          [arg0] "{rdi}" (@as(u64, 0)),
          [arg1] "{rsi}" (endpoint),
          [arg2] "{rdx}" (target_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn signalEndpoint(endpoint: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_signal_endpoint),
          [arg0] "{rdi}" (endpoint),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installFsCap(object_id: u64, kind: fs_abi.ObjectKind, rights: fs_abi.Rights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (fs_abi.syscall_install_cap),
          [arg0] "{rdi}" (object_id),
          [arg1] "{rsi}" (@as(u64, @intFromEnum(kind))),
          [arg2] "{rdx}" (fs_abi.rightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantFsCap(token: u64, to_process_slot: u64, rights: fs_abi.Rights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (fs_abi.syscall_grant_cap),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (to_process_slot),
          [arg2] "{rdx}" (fs_abi.rightsToBits(rights)),
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

fn clearPage(va: u64) void {
    const bytes: [*]volatile u8 = @ptrFromInt(va);
    var i: usize = 0;
    while (i < fs_protocol.page_bytes) : (i += 1) {
        bytes[i] = 0;
    }
}

fn compilerBarrier() void {
    asm volatile ("" ::: .{ .memory = true });
}

fn requestHeader(session: *const Session) *volatile fs_protocol.FsRequestHeader {
    return @ptrFromInt(session.request_va);
}

fn responseHeader(session: *const Session) *volatile fs_protocol.FsResponseHeader {
    return @ptrFromInt(session.response_va);
}

fn requestPayload(session: *const Session) [*]volatile u8 {
    return @ptrFromInt(session.request_va + fs_protocol.request_header_bytes);
}

fn responsePayload(session: *const Session) [*]volatile u8 {
    return @ptrFromInt(session.response_va + fs_protocol.response_header_bytes);
}

fn sessionRequestVa(slot: usize) u64 {
    return session_base_va + @as(u64, @intCast(slot)) * session_va_stride;
}

fn sessionResponseVa(slot: usize) u64 {
    return sessionRequestVa(slot) + 0x1000;
}

fn serverMountRights() fs_abi.Rights {
    return .{
        .lookup = true,
        .read = true,
        .write = true,
        .readdir = true,
        .stat = true,
        .create = true,
        .unlink = true,
        .rename = true,
        .exec = true,
        .mount = true,
        .grant = true,
        .admin = true,
    };
}

fn clientMountRights() fs_abi.Rights {
    return .{
        .lookup = true,
        .read = true,
        .write = true,
        .readdir = true,
        .stat = true,
        .create = true,
        .unlink = true,
        .rename = true,
        .exec = true,
        .mount = true,
    };
}

fn serverDirRights() fs_abi.Rights {
    return .{
        .lookup = true,
        .read = true,
        .write = true,
        .readdir = true,
        .stat = true,
        .create = true,
        .unlink = true,
        .rename = true,
        .exec = true,
        .grant = true,
        .admin = true,
    };
}

fn clientDirRights() fs_abi.Rights {
    return .{
        .lookup = true,
        .read = true,
        .write = true,
        .readdir = true,
        .stat = true,
        .create = true,
        .unlink = true,
        .rename = true,
        .exec = true,
    };
}

fn serverFileRights() fs_abi.Rights {
    return .{
        .read = true,
        .write = true,
        .stat = true,
        .exec = true,
        .grant = true,
        .admin = true,
    };
}

fn clientFileRights() fs_abi.Rights {
    return .{
        .read = true,
        .write = true,
        .stat = true,
        .exec = true,
    };
}

fn serverOpenFileRights() fs_abi.Rights {
    return .{
        .read = true,
        .write = true,
        .stat = true,
        .grant = true,
        .admin = true,
    };
}

fn clientOpenFileRights() fs_abi.Rights {
    return .{
        .read = true,
        .write = true,
        .stat = true,
    };
}

fn parseConfig() bool {
    const words: [*]volatile u64 = @ptrFromInt(config_page_va);
    if (words[0] != persistent_fs_bootstrap.config_magic) return false;
    if (words[1] != persistent_fs_bootstrap.config_version) return false;
    endpoint_id = words[persistent_fs_bootstrap.endpoint_id_index];
    admin_token = words[persistent_fs_bootstrap.admin_token_index];
    return endpoint_id != 0 and fs_abi.isCapToken(admin_token);
}

fn blockSlice(buffer: []u8) []u8 {
    return buffer[0..@as(usize, @intCast(block_size))];
}

fn initBlockService() bool {
    process_slot = getProcessSlot();
    if (process_slot == 0) return false;
    block_state = block_client.Client.connectFromServiceRegistry(block_request_va, block_response_va, process_slot) catch return false;
    block_size = block_state.block_size;
    capacity_blocks = block_state.capacity_blocks;
    if (block_size == 0 or block_size > max_block_bytes or capacity_blocks == 0) return false;
    block_ready = true;
    return true;
}

fn readBlock(block_index: u64, out: []u8) bool {
    if (!block_ready or out.len < block_size) return false;
    _ = block_state.readBlocks(block_index, out[0..@as(usize, @intCast(block_size))]) catch return false;
    return true;
}

fn writeBlock(block_index: u64, bytes: []const u8) bool {
    if (!block_ready or bytes.len < block_size) return false;
    block_state.writeBlocks(block_index, bytes[0..@as(usize, @intCast(block_size))]) catch return false;
    return true;
}

fn flushBlocks() bool {
    if (!block_ready) return false;
    block_state.flush() catch return false;
    return true;
}

fn dirEntryBytes(entry: *const VolumeDirEntry) []const u8 {
    return std.mem.asBytes(entry);
}

fn dirEntryBytesMut(entry: *VolumeDirEntry) []u8 {
    return std.mem.asBytes(entry);
}

fn requiredDirBlockCount() u64 {
    const bytes: u64 = max_dir_entries * @sizeOf(VolumeDirEntry);
    return (bytes + block_size - 1) / block_size;
}

fn expectedDirStartBlock() u64 {
    return fs_region_start_block + 1;
}

fn expectedDataStartBlock() u64 {
    return expectedDirStartBlock() + requiredDirBlockCount();
}

fn validateSuperblock(sb: *const VolumeSuperblock) bool {
    if (sb.magic != volume_magic or sb.version != volume_version) return false;
    if (sb.block_size != block_size) return false;
    if (sb.fs_start_block != fs_region_start_block) return false;
    if (sb.dir_start_block != expectedDirStartBlock()) return false;
    if (sb.dir_block_count != requiredDirBlockCount()) return false;
    if (sb.data_start_block != expectedDataStartBlock()) return false;
    if (sb.next_free_block < sb.data_start_block or sb.next_free_block > capacity_blocks) return false;
    return true;
}

fn persistSuperblock() bool {
    const block = blockSlice(block_buffer[0..]);
    @memset(block, 0);
    @memcpy(block[0..@sizeOf(VolumeSuperblock)], std.mem.asBytes(&volume_superblock));
    return writeBlock(fs_region_start_block, block);
}

fn loadSuperblock() bool {
    const block = blockSlice(block_buffer[0..]);
    if (!readBlock(fs_region_start_block, block)) return false;
    @memcpy(std.mem.asBytes(&volume_superblock), block[0..@sizeOf(VolumeSuperblock)]);
    return validateSuperblock(&volume_superblock);
}

fn persistDirectory() bool {
    const entries_per_block: usize = @intCast(block_size / @sizeOf(VolumeDirEntry));
    if (entries_per_block == 0) return false;
    var entry_index: usize = 0;
    var block_offset: u64 = 0;
    while (block_offset < volume_superblock.dir_block_count) : (block_offset += 1) {
        const block = blockSlice(block_buffer[0..]);
        @memset(block, 0);
        var slot: usize = 0;
        while (slot < entries_per_block and entry_index < max_dir_entries) : ({
            slot += 1;
            entry_index += 1;
        }) {
            const start = slot * @sizeOf(VolumeDirEntry);
            const end = start + @sizeOf(VolumeDirEntry);
            @memcpy(block[start..end], dirEntryBytes(&volume_dir_entries[entry_index]));
        }
        if (!writeBlock(volume_superblock.dir_start_block + block_offset, block)) return false;
    }
    return true;
}

fn loadDirectory() bool {
    volume_dir_entries = [_]VolumeDirEntry{.{}} ** max_dir_entries;
    const entries_per_block: usize = @intCast(block_size / @sizeOf(VolumeDirEntry));
    if (entries_per_block == 0) return false;
    var entry_index: usize = 0;
    var block_offset: u64 = 0;
    while (block_offset < volume_superblock.dir_block_count) : (block_offset += 1) {
        const block = blockSlice(block_buffer[0..]);
        if (!readBlock(volume_superblock.dir_start_block + block_offset, block)) return false;
        var slot: usize = 0;
        while (slot < entries_per_block and entry_index < max_dir_entries) : ({
            slot += 1;
            entry_index += 1;
        }) {
            const start = slot * @sizeOf(VolumeDirEntry);
            const end = start + @sizeOf(VolumeDirEntry);
            @memcpy(dirEntryBytesMut(&volume_dir_entries[entry_index]), block[start..end]);
        }
    }
    return true;
}

fn formatVolume() bool {
    const dir_block_count = requiredDirBlockCount();
    const data_start_block = fs_region_start_block + 1 + dir_block_count;
    if (capacity_blocks <= data_start_block + 16) return false;
    volume_superblock = .{
        .magic = volume_magic,
        .version = volume_version,
        .block_size = block_size,
        .fs_start_block = fs_region_start_block,
        .dir_start_block = fs_region_start_block + 1,
        .dir_block_count = dir_block_count,
        .data_start_block = data_start_block,
        .next_free_block = data_start_block,
    };
    volume_dir_entries = [_]VolumeDirEntry{.{}} ** max_dir_entries;
    if (!persistSuperblock()) return false;
    if (!persistDirectory()) return false;
    if (!flushBlocks()) return false;
    _ = userLog("PersistentFs: formatted volume\n");
    return true;
}

fn loadOrInitializeVolume() bool {
    if (loadSuperblock()) {
        if (!loadDirectory()) return false;
        recomputeNextFreeBlock();
        return true;
    }
    if (!formatVolume()) return false;
    recomputeNextFreeBlock();
    return true;
}

fn initRootCaps() bool {
    root_mount_server_token = installFsCap(root_mount_object_id, .mount, serverMountRights());
    if (!fs_abi.isCapToken(root_mount_server_token)) return false;
    root_dir_server_token = installFsCap(root_dir_object_id, .vnode_dir, serverDirRights());
    return fs_abi.isCapToken(root_dir_server_token);
}

fn dirEntryUsed(entry: *const VolumeDirEntry) bool {
    return (entry.flags & dir_entry_flag_used) != 0;
}

fn dirEntryName(entry: *const VolumeDirEntry) []const u8 {
    return entry.name[0..entry.name_bytes];
}

fn namesEqualVolatile(bytes: [*]volatile u8, len: usize, expected: []const u8) bool {
    if (len != expected.len) return false;
    var i: usize = 0;
    while (i < expected.len) : (i += 1) {
        if (bytes[i] != expected[i]) return false;
    }
    return true;
}

fn requestPathIsRootAlias(session: *const Session) bool {
    const request = requestHeader(session);
    if (request.path_bytes == 0) return true;
    return namesEqualVolatile(requestPayload(session), request.path_bytes, ".") or namesEqualVolatile(requestPayload(session), request.path_bytes, "/");
}

fn extractRequestName(session: *const Session, out: *[max_name_bytes]u8) ?[]const u8 {
    const request = requestHeader(session);
    const len: usize = request.path_bytes;
    if (len == 0 or len > max_name_bytes or len > fs_protocol.request_payload_bytes) return null;
    var i: usize = 0;
    while (i < len) : (i += 1) {
        const byte = requestPayload(session)[i];
        if (byte == '/' or byte == 0) return null;
        out[i] = byte;
    }
    if (len == 1 and out[0] == '.') return null;
    return out[0..len];
}

fn findDirEntryByName(name: []const u8) ?usize {
    for (&volume_dir_entries, 0..) |*entry, index| {
        if (!dirEntryUsed(entry)) continue;
        if (std.mem.eql(u8, dirEntryName(entry), name)) return index;
    }
    return null;
}

fn allocDirEntry(name: []const u8) ?usize {
    for (&volume_dir_entries, 0..) |*entry, index| {
        if (dirEntryUsed(entry)) continue;
        entry.* = .{};
        entry.flags = dir_entry_flag_used;
        entry.name_bytes = @intCast(name.len);
        @memcpy(entry.name[0..name.len], name);
        return index;
    }
    return null;
}

fn requestInlineBytes(session: *const Session, out: []u8) ?[]const u8 {
    const request = requestHeader(session);
    const inline_len: usize = request.inline_bytes;
    const path_len: usize = request.path_bytes;
    if (path_len + inline_len > fs_protocol.request_payload_bytes) return null;
    if (inline_len > out.len) return null;
    copyVolatileBytes(requestPayload(session) + path_len, out[0..inline_len]);
    return out[0..inline_len];
}

fn extractInlineName(session: *const Session, out: *[max_name_bytes]u8) ?[]const u8 {
    const request = requestHeader(session);
    const len: usize = request.inline_bytes;
    const path_len: usize = request.path_bytes;
    if (len == 0 or len > max_name_bytes or path_len + len > fs_protocol.request_payload_bytes) return null;
    var i: usize = 0;
    while (i < len) : (i += 1) {
        const byte = requestPayload(session)[path_len + i];
        if (byte == '/' or byte == 0) return null;
        out[i] = byte;
    }
    if (len == 1 and out[0] == '.') return null;
    return out[0..len];
}

const Extent = struct {
    start: u64,
    end: u64,
};

fn buildUsedExtents(exclude_file_index: ?usize, out: *[max_dir_entries]Extent) usize {
    var count: usize = 0;
    for (&volume_dir_entries, 0..) |*entry, index| {
        if (!dirEntryUsed(entry)) continue;
        if (exclude_file_index != null and exclude_file_index.? == index) continue;
        if (entry.block_count == 0 or entry.start_block == 0) continue;
        out[count] = .{
            .start = entry.start_block,
            .end = entry.start_block + entry.block_count,
        };
        count += 1;
    }
    var i: usize = 0;
    while (i < count) : (i += 1) {
        var min_index = i;
        var j: usize = i + 1;
        while (j < count) : (j += 1) {
            if (out[j].start < out[min_index].start) min_index = j;
        }
        if (min_index != i) {
            const tmp = out[i];
            out[i] = out[min_index];
            out[min_index] = tmp;
        }
    }
    return count;
}

fn recomputeNextFreeBlock() void {
    var next = volume_superblock.data_start_block;
    for (&volume_dir_entries) |*entry| {
        if (!dirEntryUsed(entry)) continue;
        if (entry.block_count == 0 or entry.start_block == 0) continue;
        const end = entry.start_block + entry.block_count;
        if (end > next) next = end;
    }
    volume_superblock.next_free_block = next;
}

fn findFreeExtent(required_blocks: u64, exclude_file_index: ?usize) ?u64 {
    if (required_blocks == 0) return 0;
    var extents: [max_dir_entries]Extent = undefined;
    const count = buildUsedExtents(exclude_file_index, &extents);
    var cursor = volume_superblock.data_start_block;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const extent = extents[i];
        if (cursor + required_blocks <= extent.start) return cursor;
        if (extent.end > cursor) cursor = extent.end;
    }
    if (cursor + required_blocks <= capacity_blocks) return cursor;
    return null;
}

fn fileBusy(file_index: usize) bool {
    for (&sessions) |*session| {
        if (!session.active) continue;
        for (&session.objects) |*object| {
            if (!object.active) continue;
            switch (object.kind) {
                .vnode_file, .open_file => if (object.file_index == file_index) return true,
                else => {},
            }
        }
    }
    return false;
}

fn fileHasElfMagic(entry: *const VolumeDirEntry) bool {
    if (entry.file_size < 4) return false;
    var header: [4]u8 = [_]u8{0} ** 4;
    const bytes_read = readFileBytes(entry, 0, header[0..]) orelse return false;
    return bytes_read == 4 and header[0] == 0x7F and header[1] == 'E' and header[2] == 'L' and header[3] == 'F';
}

fn execSlotBaseVa(slot: usize) u64 {
    return exec_slot_base_va + @as(u64, @intCast(slot)) * exec_slot_va_stride;
}

fn loadFileIntoExecSlot(slot: usize, entry: *const VolumeDirEntry) bool {
    const file_bytes: usize = @intCast(entry.file_size);
    var offset: usize = 0;
    var page_index: usize = 0;
    while (offset < file_bytes) : (page_index += 1) {
        const page: [*]u8 = @ptrFromInt(execSlotBaseVa(slot) + @as(u64, @intCast(page_index * page_bytes)));
        @memset(page[0..page_bytes], 0);
        const chunk = @min(page_bytes, file_bytes - offset);
        const bytes_read = readFileBytes(entry, offset, page[0..chunk]) orelse return false;
        if (bytes_read != chunk) return false;
        offset += chunk;
    }
    return true;
}

fn grantExecToken(session: *Session, file_index: usize, entry: *const VolumeDirEntry) ?u64 {
    const file_bytes: usize = @intCast(entry.file_size);
    if (file_bytes == 0 or file_bytes > max_exec_file_bytes) return null;
    const needed_pages = (file_bytes + page_bytes - 1) / page_bytes;

    var slot_index: usize = 0;
    while (slot_index < max_exec_slots) : (slot_index += 1) {
        if (exec_slots[slot_index].active) continue;

        var page_index: usize = 0;
        while (page_index < needed_pages) : (page_index += 1) {
            const paddr = allocPage();
            if (paddr < 0x1000) return null;
            const page_va = execSlotBaseVa(slot_index) + @as(u64, @intCast(page_index * page_bytes));
            if (mapPage(page_va, paddr, true) != 0) return null;
        }
        if (!loadFileIntoExecSlot(slot_index, entry)) return null;

        const server_vm_token = installVmObject(execSlotBaseVa(slot_index), entry.file_size, .{ .read = true });
        if (image_abi.decodeVmObjectToken(server_vm_token) == null) return null;
        const server_exec_token = installExecImage(server_vm_token, .{ .exec = true, .grant = true });
        if (image_abi.decodeExecImageToken(server_exec_token) == null) return null;
        const client_token = grantExecImage(server_exec_token, session.client_process_slot, .{ .exec = true });
        if (image_abi.decodeExecImageToken(client_token) == null) return null;

        exec_slots[slot_index] = .{
            .active = true,
            .page_count = @intCast(needed_pages),
            .file_index = @intCast(file_index),
        };
        return client_token;
    }
    return null;
}

fn fileObjectId(file_index: usize) u64 {
    return vnode_file_object_id_base + @as(u64, @intCast(file_index));
}

fn openFileObjectId(file_index: usize) u64 {
    return open_file_object_id_base + @as(u64, @intCast(file_index));
}

fn findSessionObject(session: *Session, token: u64) ?*SessionObject {
    for (&session.objects) |*object| {
        if (!object.active) continue;
        if (object.client_token == token) return object;
    }
    return null;
}

fn findSessionObjectByKind(session: *Session, kind: SessionObjectKind, file_index: u16) ?u64 {
    for (&session.objects) |*object| {
        if (!object.active or object.kind != kind) continue;
        if (kind == .mount or kind == .root_dir or object.file_index == file_index) return object.client_token;
    }
    return null;
}

fn storeSessionObject(session: *Session, kind: SessionObjectKind, file_index: u16, token: u64) bool {
    for (&session.objects) |*object| {
        if (object.active) continue;
        object.* = .{
            .active = true,
            .client_token = token,
            .kind = kind,
            .file_index = file_index,
        };
        return true;
    }
    return false;
}

fn ensureRootDirToken(session: *Session) ?u64 {
    if (findSessionObjectByKind(session, .root_dir, 0)) |token| return token;
    const client_token = grantFsCap(root_dir_server_token, session.client_process_slot, clientDirRights());
    if (!fs_abi.isCapToken(client_token)) return null;
    if (!storeSessionObject(session, .root_dir, 0, client_token)) return null;
    return client_token;
}

fn ensureFileVnodeToken(session: *Session, file_index: usize) ?u64 {
    if (findSessionObjectByKind(session, .vnode_file, @intCast(file_index))) |token| return token;
    const server_token = installFsCap(fileObjectId(file_index), .vnode_file, serverFileRights());
    if (!fs_abi.isCapToken(server_token)) return null;
    const client_token = grantFsCap(server_token, session.client_process_slot, clientFileRights());
    if (!fs_abi.isCapToken(client_token)) return null;
    if (!storeSessionObject(session, .vnode_file, @intCast(file_index), client_token)) return null;
    return client_token;
}

fn ensureOpenFileToken(session: *Session, file_index: usize) ?u64 {
    if (findSessionObjectByKind(session, .open_file, @intCast(file_index))) |token| return token;
    const server_token = installFsCap(openFileObjectId(file_index), .open_file, serverOpenFileRights());
    if (!fs_abi.isCapToken(server_token)) return null;
    const client_token = grantFsCap(server_token, session.client_process_slot, clientOpenFileRights());
    if (!fs_abi.isCapToken(client_token)) return null;
    if (!storeSessionObject(session, .open_file, @intCast(file_index), client_token)) return null;
    return client_token;
}

fn writeResponseHeader(
    session: *Session,
    op: fs_protocol.Opcode,
    request_seq: u64,
    status: fs_protocol.Status,
    result_token: u64,
    file_bytes: u64,
    cursor_next: u64,
    object_kind: fs_abi.ObjectKind,
    inline_bytes: u16,
) void {
    const response = responseHeader(session);
    response.magic = fs_protocol.response_magic;
    response.version = fs_protocol.version;
    response.op = fs_protocol.opcodeRaw(op);
    response.status = fs_protocol.statusRaw(status);
    response.result_flags = 0;
    response.result_token = result_token;
    response.file_bytes = file_bytes;
    response.cursor_next = cursor_next;
    response.inline_bytes = inline_bytes;
    response.object_kind = fs_protocol.objectKindRaw(object_kind);
    response.reserved0 = 0;
    response.reserved1 = 0;
    response.arg0 = block_size;
    response.arg1 = capacity_blocks;
    compilerBarrier();
    response.response_seq = request_seq;
    if (session.reply_endpoint_id != 0) _ = signalEndpoint(session.reply_endpoint_id);
}

fn replyStatus(session: *Session, op: fs_protocol.Opcode, request_seq: u64, status: fs_protocol.Status) void {
    clearPage(session.response_va);
    writeResponseHeader(session, op, request_seq, status, 0, 0, 0, .none, 0);
}

fn responseObjectKind(kind: SessionObjectKind) fs_abi.ObjectKind {
    return switch (kind) {
        .mount => .mount,
        .root_dir => .vnode_dir,
        .vnode_file => .vnode_file,
        .open_file => .open_file,
    };
}

fn modeBitsForKind(kind: SessionObjectKind) u32 {
    return switch (kind) {
        .mount, .root_dir => dir_mode_bits,
        .vnode_file, .open_file => file_mode_bits,
    };
}

fn replyLookup(session: *Session, op: fs_protocol.Opcode, request_seq: u64, client_token: u64, object_kind: fs_abi.ObjectKind, file_bytes: u64) void {
    clearPage(session.response_va);
    writeResponseHeader(session, op, request_seq, .ok, client_token, file_bytes, 0, object_kind, 0);
}

fn replyStat(session: *Session, request_seq: u64, object_kind: fs_abi.ObjectKind, size_bytes: u64, mode_bits: u32) void {
    clearPage(session.response_va);
    const record: *volatile fs_protocol.FsStatRecord = @ptrFromInt(session.response_va + fs_protocol.response_header_bytes);
    record.object_kind = fs_protocol.objectKindRaw(object_kind);
    record.size_bytes = size_bytes;
    record.mode_bits = mode_bits;
    record.mtime_unix_sec = 0;
    writeResponseHeader(session, .stat, request_seq, .ok, 0, size_bytes, 0, object_kind, fs_protocol.stat_record_bytes);
}

fn replyReaddirEnd(session: *Session, request_seq: u64) void {
    clearPage(session.response_va);
    writeResponseHeader(session, .readdir, request_seq, .end_of_dir, 0, 0, 0, .vnode_dir, 0);
}

fn replyReaddirEntry(session: *Session, request_seq: u64, next_cursor: u64, name: []const u8) void {
    clearPage(session.response_va);
    const record: *volatile fs_protocol.FsDirentRecord = @ptrFromInt(session.response_va + fs_protocol.response_header_bytes);
    record.next_cursor = next_cursor;
    record.object_kind = fs_protocol.objectKindRaw(.vnode_file);
    record.name_bytes = @intCast(name.len);
    copyBytesToVolatile(responsePayload(session) + fs_protocol.dirent_record_bytes, name);
    writeResponseHeader(
        session,
        .readdir,
        request_seq,
        .ok,
        0,
        0,
        next_cursor,
        .vnode_file,
        @intCast(fs_protocol.dirent_record_bytes + name.len),
    );
}

fn replyRead(session: *Session, request_seq: u64, file_bytes: u64, next_offset: u64, bytes: []const u8) void {
    clearPage(session.response_va);
    copyBytesToVolatile(responsePayload(session), bytes);
    writeResponseHeader(session, .read, request_seq, .ok, 0, file_bytes, next_offset, .open_file, @intCast(bytes.len));
}

fn replyOpenExec(session: *Session, request_seq: u64, client_token: u64, file_bytes: u64) void {
    clearPage(session.response_va);
    writeResponseHeader(session, .open_exec, request_seq, .ok, client_token, file_bytes, 0, .exec, 0);
}

fn readFileBytes(entry: *const VolumeDirEntry, offset: u64, out: []u8) ?usize {
    if (offset >= entry.file_size or out.len == 0) return 0;
    if (entry.block_count == 0 or entry.start_block == 0) return 0;
    var remaining: usize = @min(out.len, @as(usize, @intCast(entry.file_size - offset)));
    var copied: usize = 0;
    var block_index = entry.start_block + offset / block_size;
    var block_offset: usize = @intCast(offset % block_size);
    while (remaining > 0) {
        const block = blockSlice(block_buffer[0..]);
        if (!readBlock(block_index, block)) return null;
        const chunk = @min(remaining, @as(usize, @intCast(block_size)) - block_offset);
        @memcpy(out[copied .. copied + chunk], block[block_offset .. block_offset + chunk]);
        copied += chunk;
        remaining -= chunk;
        block_index += 1;
        block_offset = 0;
    }
    return copied;
}

fn writeFileBytes(entry: *VolumeDirEntry, file_index: usize, data: []const u8) fs_protocol.Status {
    const needed_blocks: u64 = if (data.len == 0) 0 else (@as(u64, @intCast(data.len)) + block_size - 1) / block_size;
    const current_blocks: u64 = if (entry.start_block == 0) 0 else entry.block_count;
    const target_start: u64 = blk: {
        if (needed_blocks == 0) break :blk 0;
        if (current_blocks != 0 and needed_blocks <= current_blocks) break :blk entry.start_block;
        break :blk findFreeExtent(needed_blocks, file_index) orelse return .too_big;
    };

    if (needed_blocks > 0) {
        var remaining = data;
        var block_index = target_start;
        var written_blocks: u64 = 0;
        while (written_blocks < needed_blocks) : (written_blocks += 1) {
            const block = blockSlice(block_buffer[0..]);
            @memset(block, 0);
            const chunk = @min(remaining.len, @as(usize, @intCast(block_size)));
            @memcpy(block[0..chunk], remaining[0..chunk]);
            if (!writeBlock(block_index, block)) return .io_error;
            remaining = remaining[chunk..];
            block_index += 1;
        }
    }

    entry.start_block = target_start;
    entry.block_count = @intCast(needed_blocks);
    entry.file_size = @intCast(data.len);
    recomputeNextFreeBlock();
    if (!persistDirectory()) return .io_error;
    if (!persistSuperblock()) return .io_error;
    if (!flushBlocks()) return .io_error;
    return .ok;
}

fn handleLookup(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .lookup, request_seq, .not_found);
        return;
    };
    switch (object.kind) {
        .mount, .root_dir => {
            if (requestPathIsRootAlias(session)) {
                const client_token = ensureRootDirToken(session) orelse {
                    replyStatus(session, .lookup, request_seq, .busy);
                    return;
                };
                replyLookup(session, .lookup, request_seq, client_token, .vnode_dir, 0);
                return;
            }
            var name_buf: [max_name_bytes]u8 = undefined;
            const name = extractRequestName(session, &name_buf) orelse {
                replyStatus(session, .lookup, request_seq, .invalid);
                return;
            };
            const file_index = findDirEntryByName(name) orelse {
                replyStatus(session, .lookup, request_seq, .not_found);
                return;
            };
            const client_token = ensureFileVnodeToken(session, file_index) orelse {
                replyStatus(session, .lookup, request_seq, .busy);
                return;
            };
            replyLookup(session, .lookup, request_seq, client_token, .vnode_file, volume_dir_entries[file_index].file_size);
        },
        else => replyStatus(session, .lookup, request_seq, .not_dir),
    }
}

fn handleStat(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .stat, request_seq, .not_found);
        return;
    };
    switch (object.kind) {
        .mount, .root_dir => replyStat(session, request_seq, responseObjectKind(object.kind), 0, modeBitsForKind(object.kind)),
        .vnode_file, .open_file => {
            const entry = &volume_dir_entries[object.file_index];
            if (!dirEntryUsed(entry)) {
                replyStatus(session, .stat, request_seq, .not_found);
                return;
            }
            replyStat(session, request_seq, responseObjectKind(object.kind), entry.file_size, modeBitsForKind(object.kind));
        },
    }
}

fn handleReaddir(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .readdir, request_seq, .not_found);
        return;
    };
    if (object.kind != .root_dir) {
        replyStatus(session, .readdir, request_seq, .not_dir);
        return;
    }
    var cursor: usize = @intCast(request.offset);
    while (cursor < max_dir_entries) : (cursor += 1) {
        const entry = &volume_dir_entries[cursor];
        if (!dirEntryUsed(entry)) continue;
        replyReaddirEntry(session, request_seq, cursor + 1, dirEntryName(entry));
        return;
    }
    replyReaddirEnd(session, request_seq);
}

fn handleCreate(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .create, request_seq, .not_found);
        return;
    };
    if (object.kind != .root_dir and object.kind != .mount) {
        replyStatus(session, .create, request_seq, .not_dir);
        return;
    }
    var name_buf: [max_name_bytes]u8 = undefined;
    const name = extractRequestName(session, &name_buf) orelse {
        replyStatus(session, .create, request_seq, .invalid);
        return;
    };
    const file_index = findDirEntryByName(name) orelse allocDirEntry(name) orelse {
        replyStatus(session, .create, request_seq, .busy);
        return;
    };
    if (!persistDirectory() or !flushBlocks()) {
        replyStatus(session, .create, request_seq, .io_error);
        return;
    }
    const client_token = ensureFileVnodeToken(session, file_index) orelse {
        replyStatus(session, .create, request_seq, .busy);
        return;
    };
    replyLookup(session, .create, request_seq, client_token, .vnode_file, volume_dir_entries[file_index].file_size);
}

fn handleOpen(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .open, request_seq, .not_found);
        return;
    };
    if (object.kind != .vnode_file) {
        replyStatus(session, .open, request_seq, .is_dir);
        return;
    }
    const entry = &volume_dir_entries[object.file_index];
    if (!dirEntryUsed(entry)) {
        replyStatus(session, .open, request_seq, .not_found);
        return;
    }
    const client_token = ensureOpenFileToken(session, object.file_index) orelse {
        replyStatus(session, .open, request_seq, .busy);
        return;
    };
    replyLookup(session, .open, request_seq, client_token, .open_file, entry.file_size);
}

fn handleRead(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .read, request_seq, .not_found);
        return;
    };
    if (object.kind != .open_file) {
        replyStatus(session, .read, request_seq, .invalid);
        return;
    }
    const entry = &volume_dir_entries[object.file_index];
    if (!dirEntryUsed(entry)) {
        replyStatus(session, .read, request_seq, .not_found);
        return;
    }
    const requested_len: usize = @min(@as(usize, request.length), fs_protocol.response_payload_bytes);
    const bytes_read = readFileBytes(entry, request.offset, io_buffer[0..requested_len]) orelse {
        replyStatus(session, .read, request_seq, .io_error);
        return;
    };
    replyRead(session, request_seq, entry.file_size, request.offset + bytes_read, io_buffer[0..bytes_read]);
}

fn handleWrite(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .write, request_seq, .not_found);
        return;
    };
    if (object.kind != .open_file) {
        replyStatus(session, .write, request_seq, .invalid);
        return;
    }
    if (request.offset != 0) {
        replyStatus(session, .write, request_seq, .not_supported);
        return;
    }
    const entry = &volume_dir_entries[object.file_index];
    if (!dirEntryUsed(entry)) {
        replyStatus(session, .write, request_seq, .not_found);
        return;
    }
    const payload = requestInlineBytes(session, io_buffer[0..]) orelse {
        replyStatus(session, .write, request_seq, .too_big);
        return;
    };
    const status = writeFileBytes(entry, object.file_index, payload);
    if (status != .ok) {
        replyStatus(session, .write, request_seq, status);
        return;
    }
    clearPage(session.response_va);
    writeResponseHeader(session, .write, request_seq, .ok, 0, entry.file_size, entry.file_size, .open_file, 0);
}

fn handleUnlink(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .unlink, request_seq, .not_found);
        return;
    };
    if (object.kind != .root_dir and object.kind != .mount) {
        replyStatus(session, .unlink, request_seq, .not_dir);
        return;
    }
    var name_buf: [max_name_bytes]u8 = undefined;
    const name = extractRequestName(session, &name_buf) orelse {
        replyStatus(session, .unlink, request_seq, .invalid);
        return;
    };
    const file_index = findDirEntryByName(name) orelse {
        replyStatus(session, .unlink, request_seq, .not_found);
        return;
    };
    if (fileBusy(file_index)) {
        replyStatus(session, .unlink, request_seq, .busy);
        return;
    }
    volume_dir_entries[file_index] = .{};
    recomputeNextFreeBlock();
    if (!persistDirectory() or !persistSuperblock() or !flushBlocks()) {
        replyStatus(session, .unlink, request_seq, .io_error);
        return;
    }
    replyStatus(session, .unlink, request_seq, .ok);
}

fn handleRename(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .rename, request_seq, .not_found);
        return;
    };
    if (object.kind != .root_dir and object.kind != .mount) {
        replyStatus(session, .rename, request_seq, .not_dir);
        return;
    }
    var old_name_buf: [max_name_bytes]u8 = undefined;
    const old_name = extractRequestName(session, &old_name_buf) orelse {
        replyStatus(session, .rename, request_seq, .invalid);
        return;
    };
    var new_name_buf: [max_name_bytes]u8 = undefined;
    const new_name = extractInlineName(session, &new_name_buf) orelse {
        replyStatus(session, .rename, request_seq, .invalid);
        return;
    };
    const file_index = findDirEntryByName(old_name) orelse {
        replyStatus(session, .rename, request_seq, .not_found);
        return;
    };
    if (!std.mem.eql(u8, old_name, new_name)) {
        if (findDirEntryByName(new_name)) |existing_index| {
            if (existing_index != file_index) {
                replyStatus(session, .rename, request_seq, .busy);
                return;
            }
        }
        const entry = &volume_dir_entries[file_index];
        @memset(entry.name[0..], 0);
        @memcpy(entry.name[0..new_name.len], new_name);
        entry.name_bytes = @intCast(new_name.len);
        if (!persistDirectory() or !flushBlocks()) {
            replyStatus(session, .rename, request_seq, .io_error);
            return;
        }
    }
    replyStatus(session, .rename, request_seq, .ok);
}

fn handleOpenExec(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .open_exec, request_seq, .not_found);
        return;
    };
    if (object.kind == .mount or object.kind == .root_dir) {
        replyStatus(session, .open_exec, request_seq, .is_dir);
        return;
    }
    if (object.kind != .vnode_file) {
        replyStatus(session, .open_exec, request_seq, .invalid);
        return;
    }
    const entry = &volume_dir_entries[object.file_index];
    if (!dirEntryUsed(entry)) {
        replyStatus(session, .open_exec, request_seq, .not_found);
        return;
    }
    if (!fileHasElfMagic(entry)) {
        replyStatus(session, .open_exec, request_seq, .invalid);
        return;
    }
    const client_token = grantExecToken(session, object.file_index, entry) orelse {
        replyStatus(session, .open_exec, request_seq, .busy);
        return;
    };
    replyOpenExec(session, request_seq, client_token, entry.file_size);
}

fn handleClose(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .close, request_seq, .not_found);
        return;
    };
    switch (object.kind) {
        .mount, .root_dir => {},
        .vnode_file, .open_file => object.active = false,
    }
    replyStatus(session, .close, request_seq, .ok);
}

fn processSessionRequest(session: *Session) void {
    const request = requestHeader(session);
    if (request.magic != fs_protocol.request_magic or request.version != fs_protocol.version) return;
    const request_seq = request.request_seq;
    if (request_seq == 0 or request_seq <= session.last_completed_seq) return;
    const op = std.meta.intToEnum(fs_protocol.Opcode, request.op) catch {
        replyStatus(session, .connect, request_seq, .invalid);
        session.last_completed_seq = request_seq;
        return;
    };
    switch (op) {
        .connect => replyStatus(session, .connect, request_seq, .busy),
        .lookup => handleLookup(session, request_seq),
        .open => handleOpen(session, request_seq),
        .read => handleRead(session, request_seq),
        .readdir => handleReaddir(session, request_seq),
        .stat => handleStat(session, request_seq),
        .close => handleClose(session, request_seq),
        .create => handleCreate(session, request_seq),
        .write => handleWrite(session, request_seq),
        .unlink => handleUnlink(session, request_seq),
        .rename => handleRename(session, request_seq),
        .open_exec => handleOpenExec(session, request_seq),
    }
    session.last_completed_seq = request_seq;
}

fn handleConnectRequest(request_paddr: u64) void {
    for (&sessions, 0..) |*session, slot| {
        if (session.active) continue;
        const req_va = sessionRequestVa(slot);
        const resp_va = sessionResponseVa(slot);
        if (mapPage(req_va, request_paddr, false) != 0) return;
        const request: *volatile fs_protocol.FsRequestHeader = @ptrFromInt(req_va);
        if (request.magic != fs_protocol.request_magic or
            request.version != fs_protocol.version or
            request.op != fs_protocol.opcodeRaw(.connect) or
            request.request_seq == 0 or
            request.arg0 < 0x1000 or
            request.arg1 == 0)
        {
            _ = userLog("PersistentFs: invalid connect request\n");
            return;
        }
        if (mapPage(resp_va, request.arg0, true) != 0) return;
        const reply_endpoint_id = reply_endpoint_id_base + @as(u64, @intCast(slot));
        if (installEndpoint(reply_endpoint_id, request.arg1) != 0) return;
        const mount_client_token = grantFsCap(root_mount_server_token, request.arg1, clientMountRights());
        if (!fs_abi.isCapToken(mount_client_token)) return;
        session.* = .{
            .active = true,
            .client_process_slot = request.arg1,
            .request_va = req_va,
            .response_va = resp_va,
            .reply_endpoint_id = reply_endpoint_id,
            .last_completed_seq = 0,
        };
        if (!storeSessionObject(session, .mount, 0, mount_client_token)) return;
        clearPage(resp_va);
        writeResponseHeader(session, .connect, request.request_seq, .ok, mount_client_token, 0, 0, .mount, 0);
        session.last_completed_seq = request.request_seq;
        _ = userLog("PersistentFs: session connect ok\n");
        return;
    }
    _ = userLog("PersistentFs: session table full\n");
}

fn pollSessions() void {
    for (&sessions) |*session| {
        if (!session.active) continue;
        processSessionRequest(session);
    }
}

fn copyBytesToVolatile(dst: [*]volatile u8, src: []const u8) void {
    for (src, 0..) |byte, i| {
        dst[i] = byte;
    }
}

fn copyVolatileBytes(src: [*]volatile u8, dst: []u8) void {
    for (dst, 0..) |*byte, i| {
        byte.* = src[i];
    }
}

pub export fn _start() noreturn {
    _ = userLog("PersistentFs: started\n");
    if (!parseConfig()) {
        _ = userLog("PersistentFs: config invalid\n");
        while (true) asm volatile ("pause");
    }
    if (!initBlockService()) {
        _ = userLog("PersistentFs: block service init failed\n");
        while (true) asm volatile ("pause");
    }
    if (!loadOrInitializeVolume()) {
        _ = userLog("PersistentFs: volume init failed\n");
        while (true) asm volatile ("pause");
    }
    if (!initRootCaps()) {
        _ = userLog("PersistentFs: root cap init failed\n");
        while (true) asm volatile ("pause");
    }
    _ = userLog("PersistentFs: block ready\n");
    _ = userLog("PersistentFs: endpoint ready\n");

    while (true) {
        const received = waitEvent(true, 1);
        if (received >= cap_transfer_abi.transfer_id_min and !fs_abi.isCapToken(received)) {
            const request_paddr = acceptCapTransfer(received);
            if (request_paddr >= 0x1000) {
                handleConnectRequest(request_paddr);
            }
        }
        pollSessions();
    }
}
