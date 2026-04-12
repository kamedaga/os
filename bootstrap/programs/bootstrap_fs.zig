const std = @import("std");
const bootfs_format = @import("support_root").bootfs_format;
const bootstrap_fs_bootstrap = @import("support_root").bootstrap_fs_bootstrap_abi;
const cap_transfer_abi = @import("support_root").cap_transfer_abi;
const fs_abi = @import("support_root").fs_abi;
const fs_protocol = @import("support_root").fs_protocol;
const image_abi = @import("support_root").image_abi;
const layout = @import("persistent_fs_layout");
const process_abi = @import("support_root").process_abi;

const syscall_alloc_map_pages: u64 = 0xC;
const syscall_log: u64 = 0x9;
const syscall_map_page: u64 = 0x2;
const syscall_map_mmio: u64 = 0xB;
const syscall_queue_submit: u64 = 0xE;
const syscall_queue_notify: u64 = 0xF;
const syscall_wait_event: u64 = 0x17;
const syscall_install_endpoint: u64 = 0x26;
const syscall_signal_endpoint: u64 = 0x2C;

const config_page_va: u64 = process_abi.standard_config_target_va;
const session_base_va: u64 = 0x3C11_0000;
const session_va_stride: u64 = 0x2000;
const reply_endpoint_id_base: u64 = 0xE0;
const mmio_common_page_va: usize = 0x2100_4000;
const mmio_notify_page_va: usize = 0x2100_5000;
const mmio_isr_page_va: usize = 0x2100_6000;
const mmio_device_page_va: usize = 0x2100_7000;
const queue_page0_va: usize = 0x2100_8000;
const queue_page1_va: usize = 0x2100_9000;
const dma_data_page_va: usize = 0x2100_A000;
const exec_slot_base_va: u64 = 0x3C20_0000;
const exec_slot_va_stride: u64 = 0x40000;
const max_sessions: usize = 4;
const max_session_objects: usize = 16;
const max_path_bytes: usize = 128;
const max_exec_slots: usize = 8;
const max_exec_file_bytes: usize = 256 * 1024;
const max_block_bytes: usize = 4096;
const dir_mode_bits: u32 = 0x4000;
const file_mode_bits: u32 = 0x8000;
const root_mount_object_id: u64 = 0x4254_4653; // "BTFS"
const root_dir_object_id: u64 = 0x4254_4654; // "BTFT"
const vnode_dir_hash_salt: u64 = 0x4254_4449_5231;
const vnode_file_hash_salt: u64 = 0x4254_4649_4C31;
const open_file_hash_salt: u64 = 0x4254_4F50_4E31;
const queue_cap_device_blk: u64 = 2;
const queue_index_request: u16 = 0;
const queue_size: u16 = 8;
const queue_used_offset: usize = 4096;
const queue_buffers_offset: usize = 4176;
const desc_flag_next: u16 = 1 << 0;
const desc_flag_write: u16 = 1 << 1;
const common_device_status: usize = 0x14;
const common_queue_select: usize = 0x16;
const common_queue_size: usize = 0x18;
const common_queue_enable: usize = 0x1C;
const common_queue_notify_off: usize = 0x1E;
const common_queue_desc: usize = 0x20;
const common_queue_avail: usize = 0x28;
const common_queue_used: usize = 0x30;
const status_acknowledge: u8 = 0x01;
const status_driver: u8 = 0x02;
const status_driver_ok: u8 = 0x04;
const request_type_in: u32 = 0;
const request_status_ok: u8 = 0;
const page_bytes: usize = 4096;

const ResolvedKind = enum {
    dir,
    file,
};

const NodeSource = enum {
    root,
    bootfs,
    rootfs,
};

const LookupPathResult = union(enum) {
    node: ResolvedNode,
    invalid,
    not_found,
    not_dir,
};

const BootFsChild = struct {
    name: []const u8,
    abs_path: []const u8,
    kind: ResolvedKind,
    size_bytes: u64,
};

const ResolvedNode = struct {
    source: NodeSource,
    kind: ResolvedKind,
    size_bytes: u64,
    path_len: u16 = 0,
    path_bytes: [max_path_bytes]u8 = [_]u8{0} ** max_path_bytes,

    fn init(source: NodeSource, kind: ResolvedKind, size_bytes: u64, abs_path: []const u8) ?ResolvedNode {
        if (abs_path.len == 0 or abs_path.len > max_path_bytes) return null;
        var node = ResolvedNode{
            .source = source,
            .kind = kind,
            .size_bytes = size_bytes,
            .path_len = @intCast(abs_path.len),
        };
        @memcpy(node.path_bytes[0..abs_path.len], abs_path);
        return node;
    }

    fn path(self: *const ResolvedNode) []const u8 {
        return self.path_bytes[0..self.path_len];
    }
};

const SessionObjectKind = enum {
    mount,
    vnode_dir,
    vnode_file,
    open_file,
};

const SessionObject = struct {
    active: bool = false,
    client_token: u64 = 0,
    kind: SessionObjectKind = .mount,
    source: NodeSource = .root,
    size_bytes: u64 = 0,
    path_len: u16 = 0,
    _reserved0: u16 = 0,
    _reserved1: u32 = 0,
    path_bytes: [max_path_bytes]u8 = [_]u8{0} ** max_path_bytes,
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

const BootBlockState = struct {
    rootfs_start_block: u64 = 0,
    capacity_sectors: u64 = 0,
    logical_block_size: u64 = 0,
    capacity_blocks: u64 = 0,
    sectors_per_block: u64 = 0,
    common_page_paddr: u64 = 0,
    notify_page_paddr: u64 = 0,
    isr_page_paddr: u64 = 0,
    device_page_paddr: u64 = 0,
    common_page_offset: u64 = 0,
    notify_page_offset: u64 = 0,
    isr_page_offset: u64 = 0,
    device_page_offset: u64 = 0,
    notify_off_multiplier: u64 = 0,
    queue_submit_token: u64 = 0,
    queue_notify_token: u64 = 0,
};

const VirtqDesc = extern struct {
    addr: u64,
    len: u32,
    flags: u16,
    next: u16,
};

const VirtqUsedElem = extern struct {
    id: u32,
    len: u32,
};

const VirtioBlkReqHeader = extern struct {
    request_type: u32,
    reserved: u32,
    sector: u64,
};

const ExecSlot = struct {
    active: bool = false,
    page_count: u16 = 0,
};

const BootFsArchiveView = struct {
    base_va: u64 = 0,
    size_bytes: usize = 0,
    page_count: usize = 0,

    fn getHeader(self: *const BootFsArchiveView) ?*const bootfs_format.BootFsHeader {
        if (self.size_bytes < @sizeOf(bootfs_format.BootFsHeader)) return null;
        const hdr: *const bootfs_format.BootFsHeader = @ptrFromInt(self.base_va);
        if (hdr.magic != bootfs_format.magic) return null;
        if (hdr.version != bootfs_format.version) return null;
        if (hdr.header_bytes != @sizeOf(bootfs_format.BootFsHeader)) return null;
        if (hdr.image_bytes == 0 or hdr.image_bytes > self.size_bytes) return null;
        if (hdr.entry_table_offset + hdr.entry_bytes > hdr.image_bytes) return null;
        if (hdr.string_table_offset + hdr.string_table_bytes > hdr.image_bytes) return null;
        if (hdr.data_offset + hdr.data_bytes > hdr.image_bytes) return null;
        return hdr;
    }

    fn entries(self: *const BootFsArchiveView, hdr: *const bootfs_format.BootFsHeader) []const bootfs_format.BootFsEntry {
        const ptr: [*]const bootfs_format.BootFsEntry = @ptrFromInt(self.base_va + @as(u64, @intCast(hdr.entry_table_offset)));
        return ptr[0..hdr.entry_count];
    }

    fn stringTable(self: *const BootFsArchiveView, hdr: *const bootfs_format.BootFsHeader) []const u8 {
        const ptr: [*]const u8 = @ptrFromInt(self.base_va + @as(u64, @intCast(hdr.string_table_offset)));
        return ptr[0..@intCast(hdr.string_table_bytes)];
    }

    fn pathForEntry(self: *const BootFsArchiveView, hdr: *const bootfs_format.BootFsHeader, entry: bootfs_format.BootFsEntry) ?[]const u8 {
        const table = self.stringTable(hdr);
        const begin: usize = entry.path_offset;
        const end = begin + entry.path_bytes;
        if (entry.kind != bootfs_format.kind_regular) return null;
        if (end > table.len) return null;
        return table[begin..end];
    }

    fn findRegularFile(self: *const BootFsArchiveView, abs_path: []const u8) ?[]const u8 {
        const hdr = self.getHeader() orelse return null;
        for (self.entries(hdr)) |entry| {
            const path = self.pathForEntry(hdr, entry) orelse continue;
            if (!std.mem.eql(u8, path, abs_path)) continue;
            if (entry.data_offset > hdr.image_bytes) return null;
            if (entry.data_bytes > hdr.image_bytes - entry.data_offset) return null;
            const ptr: [*]const u8 = @ptrFromInt(self.base_va + @as(u64, @intCast(entry.data_offset)));
            return ptr[0..@intCast(entry.data_bytes)];
        }
        return null;
    }
};

const fs_token_tag: u64 = 1 << 63;
var next_fs_token: u64 = 1;

fn allocFsToken() u64 {
    const t = fs_token_tag | next_fs_token;
    next_fs_token += 1;
    return t;
}

var endpoint_id: u64 = 0;
var archive = BootFsArchiveView{};
var block_state = BootBlockState{};
var common_base: usize = 0;
var notify_addr: usize = 0;
var isr_base: usize = 0;
var last_used_idx: u16 = 0;
var rootfs_ready = false;
var rootfs_superblock = layout.VolumeSuperblock{};
var rootfs_entries: [layout.max_dir_entries]layout.VolumeDirEntry = [_]layout.VolumeDirEntry{.{}} ** layout.max_dir_entries;
var sessions: [max_sessions]Session = [_]Session{.{}} ** max_sessions;
var block_buffer: [max_block_bytes]u8 align(16) = [_]u8{0} ** max_block_bytes;
var io_buffer: [max_block_bytes]u8 align(16) = [_]u8{0} ** max_block_bytes;
var exec_slots: [max_exec_slots]ExecSlot = [_]ExecSlot{.{}} ** max_exec_slots;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
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

fn mapMmioPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_mmio),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn queueSubmit(token: u64, device: u64, queue_index: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_queue_submit),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (device),
          [arg2] "{rdx}" (queue_index),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn queueNotify(token: u64, device: u64, queue_index: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_queue_notify),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (device),
          [arg2] "{rdx}" (queue_index),
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

fn parseConfig() bool {
    const words: [*]volatile u64 = @ptrFromInt(config_page_va);
    if (words[0] != bootstrap_fs_bootstrap.config_magic) return false;
    if (words[1] != bootstrap_fs_bootstrap.config_version) return false;
    endpoint_id = words[bootstrap_fs_bootstrap.endpoint_id_index];
    block_state = .{
        .rootfs_start_block = words[bootstrap_fs_bootstrap.rootfs_start_block_index],
        .capacity_sectors = words[bootstrap_fs_bootstrap.capacity_sectors_index],
        .logical_block_size = words[bootstrap_fs_bootstrap.logical_block_size_index],
        .common_page_paddr = words[bootstrap_fs_bootstrap.common_page_paddr_index],
        .notify_page_paddr = words[bootstrap_fs_bootstrap.notify_page_paddr_index],
        .isr_page_paddr = words[bootstrap_fs_bootstrap.isr_page_paddr_index],
        .device_page_paddr = words[bootstrap_fs_bootstrap.device_page_paddr_index],
        .common_page_offset = words[bootstrap_fs_bootstrap.common_page_offset_index],
        .notify_page_offset = words[bootstrap_fs_bootstrap.notify_page_offset_index],
        .isr_page_offset = words[bootstrap_fs_bootstrap.isr_page_offset_index],
        .device_page_offset = words[bootstrap_fs_bootstrap.device_page_offset_index],
        .notify_off_multiplier = words[bootstrap_fs_bootstrap.notify_off_multiplier_index],
        .queue_submit_token = words[bootstrap_fs_bootstrap.queue_submit_token_index],
        .queue_notify_token = words[bootstrap_fs_bootstrap.queue_notify_token_index],
    };
    if (endpoint_id == 0) return false;
    if (block_state.logical_block_size == 0 or block_state.logical_block_size > max_block_bytes or (block_state.logical_block_size % 512) != 0) return false;
    block_state.sectors_per_block = block_state.logical_block_size / 512;
    if (block_state.sectors_per_block == 0) return false;
    block_state.capacity_blocks = block_state.capacity_sectors / block_state.sectors_per_block;
    if (block_state.rootfs_start_block == 0 or block_state.capacity_blocks == 0) return false;
    return true;
}

fn mmioReadU8(addr: usize) u8 {
    const p: *volatile u8 = @ptrFromInt(addr);
    return p.*;
}

fn mmioWriteU8(addr: usize, value: u8) void {
    const p: *volatile u8 = @ptrFromInt(addr);
    p.* = value;
}

fn mmioReadU16(addr: usize) u16 {
    const p: *volatile u16 = @ptrFromInt(addr);
    return p.*;
}

fn mmioWriteU16(addr: usize, value: u16) void {
    const p: *volatile u16 = @ptrFromInt(addr);
    p.* = value;
}

fn mmioWriteU32(addr: usize, value: u32) void {
    const p: *volatile u32 = @ptrFromInt(addr);
    p.* = value;
}

fn mmioWriteU64(addr: usize, value: u64) void {
    const p: *volatile u64 = @ptrFromInt(addr);
    p.* = value;
}

fn queueRegionPhys(queue_paddr0: u64, queue_paddr1: u64, offset: usize) u64 {
    if (offset < page_bytes) return queue_paddr0 + @as(u64, @intCast(offset));
    return queue_paddr1 + @as(u64, @intCast(offset - page_bytes));
}

fn queueDescPtr(index: u16) *volatile VirtqDesc {
    const offset = @as(usize, index) * @sizeOf(VirtqDesc);
    return @ptrFromInt(queue_page0_va + offset);
}

fn queueAvailIdxPtr() *volatile u16 {
    return @ptrFromInt(queue_page0_va + @as(usize, queue_size) * @sizeOf(VirtqDesc) + 2);
}

fn queueAvailRingPtr() [*]volatile u16 {
    return @ptrFromInt(queue_page0_va + @as(usize, queue_size) * @sizeOf(VirtqDesc) + 4);
}

fn queueUsedIdxPtr() *volatile u16 {
    return @ptrFromInt(queue_page0_va + queue_used_offset + 2);
}

fn queueUsedRingPtr() [*]volatile VirtqUsedElem {
    return @ptrFromInt(queue_page0_va + queue_used_offset + 4);
}

fn queuePushAvail(desc_id: u16) void {
    const avail_idx_ptr = queueAvailIdxPtr();
    const avail_idx = avail_idx_ptr.*;
    const slot: usize = @intCast(avail_idx % queue_size);
    queueAvailRingPtr()[slot] = desc_id;
    avail_idx_ptr.* = avail_idx +% 1;
}

fn reqHeaderPtr() *volatile VirtioBlkReqHeader {
    return @ptrFromInt(queue_page0_va + queue_buffers_offset);
}

fn reqStatusPtr() *volatile u8 {
    return @ptrFromInt(queue_page0_va + queue_buffers_offset + @sizeOf(VirtioBlkReqHeader));
}

fn requestDataPtr() [*]volatile u8 {
    return @ptrFromInt(dma_data_page_va);
}

fn initBlockQueueMemory() bool {
    var queue_paddrs: [3]u64 = .{ 0, 0, 0 };
    if (allocMapPages(queue_page0_va, 3, true, @intFromPtr(&queue_paddrs)) != 0) return false;
    block_buffer = [_]u8{0} ** max_block_bytes;
    io_buffer = [_]u8{0} ** max_block_bytes;
    // queue page paddrs are only needed during queue setup, so keep them local in initBlockDevice
    return queue_paddrs[0] >= 0x1000 and queue_paddrs[1] >= 0x1000 and queue_paddrs[2] >= 0x1000;
}

fn initBlockDevice() bool {
    while (block_state.queue_submit_token == 0 or block_state.queue_notify_token == 0) {
        _ = waitEvent(false, 1);
        if (!parseConfig()) return false;
    }

    while (mapMmioPage(mmio_common_page_va, block_state.common_page_paddr, true) != 0) _ = waitEvent(false, 1);
    while (mapMmioPage(mmio_notify_page_va, block_state.notify_page_paddr, true) != 0) _ = waitEvent(false, 1);
    if (block_state.isr_page_paddr != 0) {
        while (mapMmioPage(mmio_isr_page_va, block_state.isr_page_paddr, false) != 0) _ = waitEvent(false, 1);
    }
    if (block_state.device_page_paddr != 0) {
        while (mapMmioPage(mmio_device_page_va, block_state.device_page_paddr, false) != 0) _ = waitEvent(false, 1);
    }

    var queue_paddrs: [3]u64 = .{ 0, 0, 0 };
    if (allocMapPages(queue_page0_va, 3, true, @intFromPtr(&queue_paddrs)) != 0) return false;
    if (queue_paddrs[0] < 0x1000 or queue_paddrs[1] < 0x1000 or queue_paddrs[2] < 0x1000) return false;

    common_base = mmio_common_page_va + @as(usize, @intCast(block_state.common_page_offset));
    const notify_base = mmio_notify_page_va + @as(usize, @intCast(block_state.notify_page_offset));
    isr_base = if (block_state.isr_page_paddr != 0) mmio_isr_page_va + @as(usize, @intCast(block_state.isr_page_offset)) else 0;

    mmioWriteU8(common_base + common_device_status, 0);
    mmioWriteU8(common_base + common_device_status, status_acknowledge | status_driver);
    mmioWriteU16(common_base + common_queue_select, queue_index_request);
    const max_size = mmioReadU16(common_base + common_queue_size);
    if (max_size == 0 or max_size < queue_size) return false;
    mmioWriteU16(common_base + common_queue_size, queue_size);
    mmioWriteU64(common_base + common_queue_desc, queue_paddrs[0]);
    mmioWriteU64(common_base + common_queue_avail, queue_paddrs[0] + (@as(u64, queue_size) * @sizeOf(VirtqDesc)));
    mmioWriteU64(common_base + common_queue_used, queueRegionPhys(queue_paddrs[0], queue_paddrs[1], queue_used_offset));

    const header_paddr = queueRegionPhys(queue_paddrs[0], queue_paddrs[1], queue_buffers_offset);
    const status_paddr = header_paddr + @sizeOf(VirtioBlkReqHeader);
    queueDescPtr(0).*.addr = header_paddr;
    queueDescPtr(0).*.len = @sizeOf(VirtioBlkReqHeader);
    queueDescPtr(0).*.flags = desc_flag_next;
    queueDescPtr(0).*.next = 1;
    queueDescPtr(1).*.addr = queue_paddrs[2];
    queueDescPtr(1).*.len = 0;
    queueDescPtr(1).*.flags = desc_flag_next | desc_flag_write;
    queueDescPtr(1).*.next = 2;
    queueDescPtr(2).*.addr = status_paddr;
    queueDescPtr(2).*.len = 1;
    queueDescPtr(2).*.flags = desc_flag_write;
    queueDescPtr(2).*.next = 0;

    const queue_notify_off = mmioReadU16(common_base + common_queue_notify_off);
    notify_addr = notify_base + @as(usize, queue_notify_off) * @as(usize, @intCast(block_state.notify_off_multiplier));
    mmioWriteU16(common_base + common_queue_enable, 1);
    if (queueSubmit(block_state.queue_submit_token, queue_cap_device_blk, queue_index_request) != 0) return false;
    if (queueNotify(block_state.queue_notify_token, queue_cap_device_blk, queue_index_request) != 0) return false;
    mmioWriteU8(common_base + common_device_status, mmioReadU8(common_base + common_device_status) | status_driver_ok);
    return true;
}

fn executeBlockRead(block_index: u64, out: []u8) bool {
    if (out.len != block_state.logical_block_size) return false;
    const header = reqHeaderPtr();
    header.* = .{
        .request_type = request_type_in,
        .reserved = 0,
        .sector = block_index * block_state.sectors_per_block,
    };
    const status = reqStatusPtr();
    status.* = 0xFF;

    queueDescPtr(0).*.flags = desc_flag_next;
    queueDescPtr(0).*.next = 1;
    queueDescPtr(1).*.len = @intCast(out.len);
    queueDescPtr(1).*.flags = desc_flag_next | desc_flag_write;
    queueDescPtr(1).*.next = 2;
    queuePushAvail(0);
    mmioWriteU16(notify_addr, queue_index_request);

    while (true) {
        if (queueUsedIdxPtr().* != last_used_idx) {
            if (isr_base != 0) _ = mmioReadU8(isr_base);
            _ = queueUsedRingPtr()[@intCast(last_used_idx % queue_size)];
            last_used_idx +%= 1;
            if (status.* != request_status_ok) return false;
            const src = @as([*]const u8, @ptrCast(@volatileCast(requestDataPtr())));
            @memcpy(out, src[0..out.len]);
            return true;
        }
        if (isr_base != 0) _ = mmioReadU8(isr_base);
        asm volatile ("pause");
    }
}

fn loadRootFsMetadata() bool {
    if (!executeBlockRead(block_state.rootfs_start_block, block_buffer[0..@intCast(block_state.logical_block_size)])) return false;
    @memcpy(std.mem.asBytes(&rootfs_superblock), block_buffer[0..@sizeOf(layout.VolumeSuperblock)]);
    if (!layout.validateSuperblock(&rootfs_superblock, block_state.rootfs_start_block, block_state.logical_block_size, block_state.capacity_blocks)) return false;
    layout.clearDirectory(&rootfs_entries);
    const entries_per_block: usize = @intCast(block_state.logical_block_size / @sizeOf(layout.VolumeDirEntry));
    var entry_index: usize = 0;
    var block_offset: u64 = 0;
    while (block_offset < rootfs_superblock.dir_block_count) : (block_offset += 1) {
        if (!executeBlockRead(rootfs_superblock.dir_start_block + block_offset, block_buffer[0..@intCast(block_state.logical_block_size)])) return false;
        var slot: usize = 0;
        while (slot < entries_per_block and entry_index < layout.max_dir_entries) : ({
            slot += 1;
            entry_index += 1;
        }) {
            const start = slot * @sizeOf(layout.VolumeDirEntry);
            const end = start + @sizeOf(layout.VolumeDirEntry);
            @memcpy(std.mem.asBytes(&rootfs_entries[entry_index]), block_buffer[start..end]);
        }
    }
    rootfs_ready = true;
    return true;
}

fn rootfsFindEntryByName(name: []const u8) ?*const layout.VolumeDirEntry {
    if (!rootfs_ready) return null;
    for (&rootfs_entries) |*entry| {
        if (!layout.dirEntryUsed(entry)) continue;
        if (std.mem.eql(u8, layout.dirEntryName(entry), name)) return entry;
    }
    return null;
}

fn rootfsFindEntryByPath(abs_path: []const u8) ?*const layout.VolumeDirEntry {
    if (abs_path.len < 2 or abs_path[0] != '/') return null;
    return rootfsFindEntryByName(abs_path[1..]);
}

fn rootfsEntryAt(cursor: u64) ?*const layout.VolumeDirEntry {
    if (!rootfs_ready) return null;
    var index: u64 = 0;
    for (&rootfs_entries) |*entry| {
        if (!layout.dirEntryUsed(entry)) continue;
        if (index == cursor) return entry;
        index += 1;
    }
    return null;
}

fn rootfsReadFile(entry: *const layout.VolumeDirEntry, offset: u64, out: []u8) ?usize {
    if (offset >= entry.file_size) return 0;
    var remaining: usize = @min(out.len, @as(usize, @intCast(entry.file_size - offset)));
    var copied: usize = 0;
    var block_index = entry.start_block + offset / block_state.logical_block_size;
    var block_offset: usize = @intCast(offset % block_state.logical_block_size);
    while (remaining > 0) {
        if (!executeBlockRead(block_index, block_buffer[0..@intCast(block_state.logical_block_size)])) return null;
        const chunk = @min(remaining, @as(usize, @intCast(block_state.logical_block_size)) - block_offset);
        @memcpy(out[copied .. copied + chunk], block_buffer[block_offset .. block_offset + chunk]);
        copied += chunk;
        remaining -= chunk;
        block_index += 1;
        block_offset = 0;
    }
    return copied;
}

fn allocExecSlot(file_size: u64) ?struct { slot_index: usize, base_va: u64 } {
    if (file_size == 0 or file_size > max_exec_file_bytes) return null;
    const page_count_u64 = (file_size + page_bytes - 1) / page_bytes;
    const page_count: u16 = @intCast(page_count_u64);
    for (&exec_slots, 0..) |*slot, index| {
        if (slot.active) continue;
        const base_va = exec_slot_base_va + @as(u64, @intCast(index)) * exec_slot_va_stride;
        var paddrs: [64]u64 = [_]u64{0} ** 64;
        if (page_count > paddrs.len) return null;
        if (allocMapPages(base_va, page_count, true, @intFromPtr(&paddrs)) != 0) return null;
        slot.* = .{
            .active = true,
            .page_count = page_count,
        };
        return .{ .slot_index = index, .base_va = base_va };
    }
    return null;
}

fn loadRootFsExecToken(entry: *const layout.VolumeDirEntry) ?u64 {
    const slot = allocExecSlot(entry.file_size) orelse return null;
    const dst: [*]u8 = @ptrFromInt(slot.base_va);
    if (rootfsReadFile(entry, 0, dst[0..@intCast(entry.file_size)])) |bytes_read| {
        if (bytes_read != entry.file_size) return null;
    } else return null;
    const vm_token = installVmObject(slot.base_va, entry.file_size, .{ .read = true });
    if (image_abi.decodeVmObjectToken(vm_token) == null) return null;
    const exec_token = installExecImage(vm_token, .{ .exec = true, .grant = true });
    if (image_abi.decodeExecImageToken(exec_token) == null) return null;
    return exec_token;
}

fn serverMountRights() fs_abi.Rights {
    return .{ .lookup = true, .read = true, .readdir = true, .stat = true, .exec = true, .mount = true, .grant = true, .admin = true };
}

fn clientMountRights() fs_abi.Rights {
    return .{ .lookup = true, .read = true, .readdir = true, .stat = true, .exec = true, .mount = true };
}

fn serverDirRights() fs_abi.Rights {
    return .{ .lookup = true, .read = true, .readdir = true, .stat = true, .exec = true, .grant = true, .admin = true };
}

fn clientDirRights() fs_abi.Rights {
    return .{ .lookup = true, .read = true, .readdir = true, .stat = true, .exec = true };
}

fn serverFileRights() fs_abi.Rights {
    return .{ .read = true, .stat = true, .exec = true, .grant = true, .admin = true };
}

fn clientFileRights() fs_abi.Rights {
    return .{ .read = true, .stat = true, .exec = true };
}

fn serverOpenFileRights() fs_abi.Rights {
    return .{ .read = true, .stat = true, .grant = true, .admin = true };
}

fn clientOpenFileRights() fs_abi.Rights {
    return .{ .read = true, .stat = true };
}

fn initRootCaps() bool {
    return true;
}

fn hashPath(abs_path: []const u8, salt: u64) u64 {
    var hash: u64 = 0xcbf2_9ce4_8422_2325 ^ salt;
    for (abs_path) |byte| {
        hash ^= byte;
        hash *%= 0x0000_0100_0000_01b3;
    }
    hash &= 0x7FFF_FFFF_FFFF_FFFF;
    if (hash == 0 or hash == root_mount_object_id or hash == root_dir_object_id) hash +%= 0x1000;
    return hash;
}

fn vnodeDirObjectId(abs_path: []const u8) u64 {
    if (std.mem.eql(u8, abs_path, "/")) return root_dir_object_id;
    return hashPath(abs_path, vnode_dir_hash_salt);
}

fn vnodeFileObjectId(abs_path: []const u8) u64 {
    return hashPath(abs_path, vnode_file_hash_salt);
}

fn openFileObjectId(abs_path: []const u8) u64 {
    return hashPath(abs_path, open_file_hash_salt);
}

fn rootNode() ResolvedNode {
    return ResolvedNode.init(.root, .dir, 0, "/").?;
}

fn nodeFromChild(child: BootFsChild) ?ResolvedNode {
    return ResolvedNode.init(.bootfs, child.kind, child.size_bytes, child.abs_path);
}

fn nodeFromSessionObject(object: *const SessionObject) ?ResolvedNode {
    const kind: ResolvedKind = switch (object.kind) {
        .mount, .vnode_dir => .dir,
        .vnode_file, .open_file => .file,
    };
    return ResolvedNode.init(object.source, kind, object.size_bytes, object.path_bytes[0..object.path_len]);
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
        return .{ .name = remainder[0..idx], .abs_path = entry_path[0..abs_end], .kind = .dir, .size_bytes = 0 };
    }
    return .{ .name = remainder, .abs_path = entry_path, .kind = .file, .size_bytes = file_size };
}

fn bootfsChildByName(parent_abs_path: []const u8, name: []const u8) ?BootFsChild {
    const hdr = archive.getHeader() orelse return null;
    var prev_abs_path: []const u8 = "";
    var has_prev = false;
    for (archive.entries(hdr)) |entry| {
        const path = archive.pathForEntry(hdr, entry) orelse continue;
        const child = bootfsDirectChildForPath(parent_abs_path, path, entry.data_bytes) orelse continue;
        if (has_prev and std.mem.eql(u8, child.abs_path, prev_abs_path)) continue;
        prev_abs_path = child.abs_path;
        has_prev = true;
        if (std.mem.eql(u8, child.name, name)) return child;
    }
    return null;
}

fn bootfsChildAt(parent_abs_path: []const u8, cursor: u64) ?BootFsChild {
    const hdr = archive.getHeader() orelse return null;
    var unique_index: u64 = 0;
    var prev_abs_path: []const u8 = "";
    var has_prev = false;
    for (archive.entries(hdr)) |entry| {
        const path = archive.pathForEntry(hdr, entry) orelse continue;
        const child = bootfsDirectChildForPath(parent_abs_path, path, entry.data_bytes) orelse continue;
        if (has_prev and std.mem.eql(u8, child.abs_path, prev_abs_path)) continue;
        if (unique_index == cursor) return child;
        prev_abs_path = child.abs_path;
        has_prev = true;
        unique_index += 1;
    }
    return null;
}

fn resolveLookupPath(base: ResolvedNode, raw_path: []const u8) LookupPathResult {
    if (raw_path.len == 0) return .{ .node = base };
    var current = if (raw_path[0] == '/') rootNode() else base;
    var remaining = if (raw_path[0] == '/') raw_path[1..] else raw_path;
    if (remaining.len == 0) return .{ .node = current };
    while (true) {
        const component_end = std.mem.indexOfScalar(u8, remaining, '/') orelse remaining.len;
        if (component_end == 0) return .invalid;
        const component = remaining[0..component_end];
        if (std.mem.eql(u8, component, ".") or std.mem.eql(u8, component, "..")) return .invalid;
        if (current.kind != .dir) return .not_dir;
        const child = bootfsChildByName(current.path(), component) orelse return .not_found;
        current = nodeFromChild(child) orelse return .invalid;
        if (component_end == remaining.len) return .{ .node = current };
        remaining = remaining[component_end + 1 ..];
        if (remaining.len == 0) return if (current.kind == .dir) .{ .node = current } else .not_dir;
    }
}

fn rootfsPathName(raw_path: []const u8) ?[]const u8 {
    if (raw_path.len == 0 or std.mem.eql(u8, raw_path, "/")) return null;
    const path = if (raw_path[0] == '/') raw_path[1..] else raw_path;
    if (path.len == 0) return null;
    if (std.mem.indexOfScalar(u8, path, '/')) |_| return null;
    return path;
}

fn resolveRootFsLookup(base: ResolvedNode, raw_path: []const u8) ?ResolvedNode {
    if (base.kind != .dir) return null;
    if (!std.mem.eql(u8, base.path(), "/")) return null;
    const name = rootfsPathName(raw_path) orelse return null;
    const entry = rootfsFindEntryByName(name) orelse return null;
    var abs_path_buf: [max_path_bytes]u8 = [_]u8{0} ** max_path_bytes;
    abs_path_buf[0] = '/';
    @memcpy(abs_path_buf[1 .. 1 + name.len], name);
    return ResolvedNode.init(.rootfs, .file, entry.file_size, abs_path_buf[0 .. 1 + name.len]);
}

fn resolveBootstrapLookup(base: ResolvedNode, raw_path: []const u8) LookupPathResult {
    if (raw_path.len == 0) return .{ .node = base };
    if (std.mem.eql(u8, raw_path, "/")) return .{ .node = rootNode() };
    if (base.kind != .dir) return .not_dir;
    if (!std.mem.eql(u8, base.path(), "/")) return .not_dir;
    const node = resolveRootFsLookup(base, raw_path) orelse return .not_found;
    return .{ .node = node };
}

fn requestPath(session: *const Session) []const u8 {
    const request = requestHeader(session);
    const bytes: usize = @min(@as(usize, request.path_bytes), fs_protocol.request_payload_bytes);
    const payload: [*]const u8 = @ptrCast(@volatileCast(requestPayload(session)));
    return payload[0..bytes];
}

fn copyPathIntoObject(object: *SessionObject, abs_path: []const u8) bool {
    if (abs_path.len == 0 or abs_path.len > max_path_bytes) return false;
    object.path_len = @intCast(abs_path.len);
    @memset(object.path_bytes[0..], 0);
    @memcpy(object.path_bytes[0..abs_path.len], abs_path);
    return true;
}

fn findSessionObject(session: *Session, client_token: u64) ?*SessionObject {
    for (&session.objects) |*object| {
        if (object.active and object.client_token == client_token) return object;
    }
    return null;
}

fn findSessionObjectByKindAndPath(session: *Session, kind: SessionObjectKind, abs_path: []const u8) ?u64 {
    for (&session.objects) |*object| {
        if (!object.active or object.kind != kind) continue;
        if (object.path_len != abs_path.len) continue;
        if (std.mem.eql(u8, object.path_bytes[0..object.path_len], abs_path)) return object.client_token;
    }
    return null;
}

fn storeSessionObject(session: *Session, kind: SessionObjectKind, client_token: u64, node: ResolvedNode) bool {
    for (&session.objects) |*object| {
        if (object.active) continue;
        object.* = .{
            .active = true,
            .client_token = client_token,
            .kind = kind,
            .source = node.source,
            .size_bytes = node.size_bytes,
        };
        return copyPathIntoObject(object, node.path());
    }
    return false;
}

fn ensureRootDirToken(session: *Session) ?u64 {
    if (findSessionObjectByKindAndPath(session, .vnode_dir, "/")) |token| return token;
    const client_token = allocFsToken();
    if (!storeSessionObject(session, .vnode_dir, client_token, rootNode())) return null;
    return client_token;
}

fn ensureDirVnodeToken(session: *Session, node: ResolvedNode) ?u64 {
    if (findSessionObjectByKindAndPath(session, .vnode_dir, node.path())) |token| return token;
    const client_token = allocFsToken();
    if (!storeSessionObject(session, .vnode_dir, client_token, node)) return null;
    return client_token;
}

fn ensureFileVnodeToken(session: *Session, node: ResolvedNode) ?u64 {
    if (findSessionObjectByKindAndPath(session, .vnode_file, node.path())) |token| return token;
    const client_token = allocFsToken();
    if (!storeSessionObject(session, .vnode_file, client_token, node)) return null;
    return client_token;
}

fn ensureOpenFileToken(session: *Session, node: ResolvedNode) ?u64 {
    if (findSessionObjectByKindAndPath(session, .open_file, node.path())) |token| return token;
    const client_token = allocFsToken();
    if (!storeSessionObject(session, .open_file, client_token, node)) return null;
    return client_token;
}

fn responseObjectKind(kind: SessionObjectKind) fs_abi.ObjectKind {
    return switch (kind) {
        .mount => .mount,
        .vnode_dir => .vnode_dir,
        .vnode_file => .vnode_file,
        .open_file => .open_file,
    };
}

fn modeBitsForObject(object: *const SessionObject) u32 {
    return switch (object.kind) {
        .mount, .vnode_dir => dir_mode_bits,
        .vnode_file, .open_file => file_mode_bits,
    };
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
    response.arg0 = 0;
    response.arg1 = 0;
    compilerBarrier();
    response.response_seq = request_seq;
    if (session.reply_endpoint_id != 0) _ = signalEndpoint(session.reply_endpoint_id);
}

fn replyStatus(session: *Session, op: fs_protocol.Opcode, request_seq: u64, status: fs_protocol.Status) void {
    clearPage(session.response_va);
    writeResponseHeader(session, op, request_seq, status, 0, 0, 0, .none, 0);
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

fn replyReaddirEnd(session: *Session, request_seq: u64, object_kind: fs_abi.ObjectKind) void {
    clearPage(session.response_va);
    writeResponseHeader(session, .readdir, request_seq, .end_of_dir, 0, 0, 0, object_kind, 0);
}

fn replyReaddirEntry(session: *Session, request_seq: u64, next_cursor: u64, child_kind: fs_abi.ObjectKind, name: []const u8) void {
    clearPage(session.response_va);
    const record: *volatile fs_protocol.FsDirentRecord = @ptrFromInt(session.response_va + fs_protocol.response_header_bytes);
    record.next_cursor = next_cursor;
    record.object_kind = fs_protocol.objectKindRaw(child_kind);
    record.name_bytes = @intCast(name.len);
    copyBytesToVolatile(responsePayload(session) + fs_protocol.dirent_record_bytes, name);
    writeResponseHeader(session, .readdir, request_seq, .ok, 0, 0, next_cursor, child_kind, @intCast(fs_protocol.dirent_record_bytes + name.len));
}

fn replyRead(session: *Session, request_seq: u64, file_bytes: u64, next_offset: u64, bytes: []const u8) void {
    clearPage(session.response_va);
    copyBytesToVolatile(responsePayload(session), bytes);
    writeResponseHeader(session, .read, request_seq, .ok, 0, file_bytes, next_offset, .open_file, @intCast(bytes.len));
}

fn replyOpenExec(session: *Session, request_seq: u64, exec_token: u64, file_bytes: u64) void {
    clearPage(session.response_va);
    writeResponseHeader(session, .open_exec, request_seq, .ok, exec_token, file_bytes, 0, .exec, 0);
}

fn handleLookup(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .lookup, request_seq, .not_found);
        return;
    };
    const base = nodeFromSessionObject(object) orelse {
        replyStatus(session, .lookup, request_seq, .invalid);
        return;
    };
    const path = requestPath(session);
    const node = switch (resolveBootstrapLookup(base, path)) {
        .node => |resolved| resolved,
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
    };
    const client_token = switch (node.kind) {
        .dir => if (std.mem.eql(u8, node.path(), "/")) ensureRootDirToken(session) else ensureDirVnodeToken(session, node),
        .file => ensureFileVnodeToken(session, node),
    } orelse {
        replyStatus(session, .lookup, request_seq, .busy);
        return;
    };
    replyLookup(session, .lookup, request_seq, client_token, switch (node.kind) {
        .dir => .vnode_dir,
        .file => .vnode_file,
    }, node.size_bytes);
}

fn handleStat(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .stat, request_seq, .not_found);
        return;
    };
    replyStat(session, request_seq, responseObjectKind(object.kind), object.size_bytes, modeBitsForObject(object));
}

fn handleReaddir(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .readdir, request_seq, .not_found);
        return;
    };
    if (object.kind != .mount and object.kind != .vnode_dir) {
        replyStatus(session, .readdir, request_seq, .not_dir);
        return;
    }
    const node = nodeFromSessionObject(object) orelse {
        replyStatus(session, .readdir, request_seq, .invalid);
        return;
    };
    if (!std.mem.eql(u8, node.path(), "/")) {
        replyReaddirEnd(session, request_seq, responseObjectKind(object.kind));
        return;
    }
    if (rootfsEntryAt(request.offset)) |entry| {
        const name = layout.dirEntryName(entry);
        replyReaddirEntry(session, request_seq, request.offset + 1, .vnode_file, name);
        return;
    }
    replyReaddirEnd(session, request_seq, responseObjectKind(object.kind));
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
    const node = nodeFromSessionObject(object) orelse {
        replyStatus(session, .open, request_seq, .invalid);
        return;
    };
    switch (node.source) {
        .rootfs => if (rootfsFindEntryByPath(node.path()) == null) {
            replyStatus(session, .open, request_seq, .not_found);
            return;
        },
        else => {
            replyStatus(session, .open, request_seq, .invalid);
            return;
        },
    }
    const client_token = ensureOpenFileToken(session, node) orelse {
        replyStatus(session, .open, request_seq, .busy);
        return;
    };
    replyLookup(session, .open, request_seq, client_token, .open_file, node.size_bytes);
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
    switch (object.source) {
        .rootfs => {
            const entry = rootfsFindEntryByPath(object.path_bytes[0..object.path_len]) orelse {
                replyStatus(session, .read, request_seq, .not_found);
                return;
            };
            const requested_len: usize = @min(@as(usize, request.length), fs_protocol.response_payload_bytes);
            const bytes_read = rootfsReadFile(entry, request.offset, io_buffer[0..requested_len]) orelse {
                replyStatus(session, .read, request_seq, .io_error);
                return;
            };
            replyRead(session, request_seq, entry.file_size, request.offset + @as(u64, @intCast(bytes_read)), io_buffer[0..bytes_read]);
        },
        else => {
            replyStatus(session, .read, request_seq, .invalid);
            return;
        },
    }
}

fn handleOpenExec(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .open_exec, request_seq, .not_found);
        return;
    };
    if (object.kind != .vnode_file) {
        replyStatus(session, .open_exec, request_seq, .is_dir);
        return;
    }
    const node = nodeFromSessionObject(object) orelse {
        replyStatus(session, .open_exec, request_seq, .invalid);
        return;
    };
    const client_token = switch (node.source) {
        .rootfs => blk: {
            const entry = rootfsFindEntryByPath(node.path()) orelse {
                replyStatus(session, .open_exec, request_seq, .not_found);
                return;
            };
            const magic = rootfsReadFile(entry, 0, io_buffer[0..4]) orelse {
                replyStatus(session, .open_exec, request_seq, .io_error);
                return;
            };
            if (magic < 4 or io_buffer[0] != 0x7F or io_buffer[1] != 'E' or io_buffer[2] != 'L' or io_buffer[3] != 'F') {
                replyStatus(session, .open_exec, request_seq, .invalid);
                return;
            }
            const server_exec_token = loadRootFsExecToken(entry) orelse {
                replyStatus(session, .open_exec, request_seq, .busy);
                return;
            };
            const child = grantExecImage(server_exec_token, session.client_process_slot, .{ .exec = true });
            if (image_abi.decodeExecImageToken(child) == null) {
                replyStatus(session, .open_exec, request_seq, .busy);
                return;
            }
            break :blk child;
        },
        else => {
            replyStatus(session, .open_exec, request_seq, .invalid);
            return;
        },
    };
    replyOpenExec(session, request_seq, client_token, node.size_bytes);
}

fn handleClose(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const object = findSessionObject(session, request.object_token) orelse {
        replyStatus(session, .close, request_seq, .not_found);
        return;
    };
    switch (object.kind) {
        .mount, .vnode_dir => {},
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
        .open_exec => handleOpenExec(session, request_seq),
        else => replyStatus(session, op, request_seq, .not_supported),
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
            _ = userLog("BootstrapFs: invalid connect request\n");
            return;
        }
        if (mapPage(resp_va, request.arg0, true) != 0) return;
        const reply_endpoint_id = reply_endpoint_id_base + @as(u64, @intCast(slot));
        if (installEndpoint(reply_endpoint_id, request.arg1) != 0) return;
        const mount_client_token = allocFsToken();
        session.* = .{
            .active = true,
            .client_process_slot = request.arg1,
            .request_va = req_va,
            .response_va = resp_va,
            .reply_endpoint_id = reply_endpoint_id,
            .last_completed_seq = 0,
        };
        session.objects[0] = .{
            .active = true,
            .client_token = mount_client_token,
            .kind = .mount,
            .source = .root,
            .size_bytes = 0,
            .path_len = 1,
            .path_bytes = [_]u8{0} ** max_path_bytes,
        };
        session.objects[0].path_bytes[0] = '/';
        clearPage(resp_va);
        writeResponseHeader(session, .connect, request.request_seq, .ok, mount_client_token, 0, 0, .mount, 0);
        session.last_completed_seq = request.request_seq;
        _ = userLog("BootstrapFs: session connect ok\n");
        return;
    }
    _ = userLog("BootstrapFs: session table full\n");
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

pub export fn _start() noreturn {
    _ = userLog("BootstrapFs: started\n");
    if (!parseConfig()) {
        _ = userLog("BootstrapFs: config invalid\n");
        while (true) asm volatile ("pause");
    }
    if (!initRootCaps()) {
        _ = userLog("BootstrapFs: root cap init failed\n");
        while (true) asm volatile ("pause");
    }
    if (!initBlockDevice()) {
        _ = userLog("BootstrapFs: rootfs block init failed\n");
    } else if (!loadRootFsMetadata()) {
        _ = userLog("BootstrapFs: rootfs metadata unavailable\n");
    } else {
        _ = userLog("BootstrapFs: rootfs ready\n");
    }
    _ = userLog("BootstrapFs: endpoint ready\n");

    while (true) {
        const received = waitEvent(true, 1);
        if (received >= cap_transfer_abi.transfer_id_min) {
            const request_paddr = acceptCapTransfer(received);
            if (request_paddr >= 0x1000) {
                handleConnectRequest(request_paddr);
            }
        }
        pollSessions();
    }
}
