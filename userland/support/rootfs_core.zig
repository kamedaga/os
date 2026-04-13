const std = @import("std");
const image_abi = @import("image_abi.zig");
const layout = @import("persistent_fs_layout");
const queue_abi = @import("queue_abi.zig");

const syscall_alloc_map_pages: u64 = 0xC;
const syscall_map_mmio: u64 = 0xB;
const syscall_queue_submit: u64 = 0xE;
const syscall_queue_notify: u64 = 0xF;
const syscall_wait_event: u64 = 0x17;

const mmio_common_page_va: usize = 0x2100_4000;
const mmio_notify_page_va: usize = 0x2100_5000;
const mmio_isr_page_va: usize = 0x2100_6000;
const queue_page0_va: usize = 0x2100_8000;
const dma_data_page_va: usize = 0x2100_A000;
const exec_slot_base_va: u64 = 0x3C20_0000;
const exec_slot_va_stride: u64 = 0x40000;

const max_exec_slots: usize = 8;
const max_exec_file_bytes: usize = 256 * 1024;
const max_block_bytes: usize = 4096;
const page_bytes: usize = 4096;
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

pub const Config = struct {
    rootfs_start_block: u64,
    capacity_sectors: u64,
    logical_block_size: u64,
    common_page_paddr: u64,
    notify_page_paddr: u64,
    isr_page_paddr: u64,
    common_page_offset: u64,
    notify_page_offset: u64,
    isr_page_offset: u64,
    notify_off_multiplier: u64,
    queue_submit_token: u64,
    queue_notify_token: u64,
};

pub const OpenExecResult = struct {
    token: u64,
    file_bytes: u64,
};

const BlockState = struct {
    rootfs_start_block: u64 = 0,
    capacity_sectors: u64 = 0,
    logical_block_size: u64 = 0,
    capacity_blocks: u64 = 0,
    sectors_per_block: u64 = 0,
    common_page_paddr: u64 = 0,
    notify_page_paddr: u64 = 0,
    isr_page_paddr: u64 = 0,
    common_page_offset: u64 = 0,
    notify_page_offset: u64 = 0,
    isr_page_offset: u64 = 0,
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

var block_state = BlockState{};
var common_base: usize = 0;
var notify_addr: usize = 0;
var isr_base: usize = 0;
var last_used_idx: u16 = 0;
var rootfs_ready = false;
var rootfs_superblock = layout.VolumeSuperblock{};
var rootfs_entries: [layout.max_dir_entries]layout.VolumeDirEntry = [_]layout.VolumeDirEntry{.{}} ** layout.max_dir_entries;
var block_buffer: [max_block_bytes]u8 align(16) = [_]u8{0} ** max_block_bytes;
var exec_slots: [max_exec_slots]ExecSlot = [_]ExecSlot{.{}} ** max_exec_slots;

pub fn init(config: Config) bool {
    const sectors_per_block = config.logical_block_size / 512;
    if (config.logical_block_size == 0 or config.logical_block_size > max_block_bytes or (config.logical_block_size % 512) != 0) return false;
    if (sectors_per_block == 0) return false;
    const capacity_blocks = config.capacity_sectors / sectors_per_block;
    if (config.rootfs_start_block == 0 or capacity_blocks == 0) return false;

    block_state = .{
        .rootfs_start_block = config.rootfs_start_block,
        .capacity_sectors = config.capacity_sectors,
        .logical_block_size = config.logical_block_size,
        .capacity_blocks = capacity_blocks,
        .sectors_per_block = sectors_per_block,
        .common_page_paddr = config.common_page_paddr,
        .notify_page_paddr = config.notify_page_paddr,
        .isr_page_paddr = config.isr_page_paddr,
        .common_page_offset = config.common_page_offset,
        .notify_page_offset = config.notify_page_offset,
        .isr_page_offset = config.isr_page_offset,
        .notify_off_multiplier = config.notify_off_multiplier,
        .queue_submit_token = config.queue_submit_token,
        .queue_notify_token = config.queue_notify_token,
    };
    common_base = 0;
    notify_addr = 0;
    isr_base = 0;
    last_used_idx = 0;
    rootfs_ready = false;
    layout.clearDirectory(&rootfs_entries);
    exec_slots = [_]ExecSlot{.{}} ** max_exec_slots;
    block_buffer = [_]u8{0} ** max_block_bytes;

    if (!initBlockDevice()) return false;
    return loadRootFsMetadata();
}

pub fn fileSize(abs_path: []const u8) ?u64 {
    const entry = findEntryByPath(abs_path) orelse return null;
    if (layout.dirEntryIsDirectory(entry)) return null;
    return entry.file_size;
}

pub fn loadFile(abs_path: []const u8, out: []u8) ?usize {
    const entry = findEntryByPath(abs_path) orelse return null;
    if (layout.dirEntryIsDirectory(entry)) return null;
    if (entry.file_size > out.len) return null;
    const bytes_read = readFileEntry(entry, 0, out[0..@intCast(entry.file_size)]) orelse return null;
    if (bytes_read != entry.file_size) return null;
    return bytes_read;
}

pub fn openExec(abs_path: []const u8) ?OpenExecResult {
    const entry = findEntryByPath(abs_path) orelse return null;
    if (layout.dirEntryIsDirectory(entry)) return null;
    return loadExec(entry);
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

fn queueSubmit(token: u64, queue_index: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_queue_submit),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (queue_index),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn queueNotify(token: u64, queue_index: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_queue_notify),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (queue_index),
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

fn waitMapMmioPage(va: u64, paddr: u64, writable: bool) bool {
    if (paddr == 0) return false;
    var attempt: usize = 0;
    while (attempt < 4096) : (attempt += 1) {
        if (mapMmioPage(va, paddr, writable) == 0) return true;
        _ = waitEvent(false, 1);
    }
    return false;
}

fn mmioReadU8(addr: usize) u8 {
    const ptr: *const volatile u8 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioWriteU8(addr: usize, value: u8) void {
    const ptr: *volatile u8 = @ptrFromInt(addr);
    ptr.* = value;
}

fn mmioReadU16(addr: usize) u16 {
    const ptr: *const volatile u16 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioWriteU16(addr: usize, value: u16) void {
    const ptr: *volatile u16 = @ptrFromInt(addr);
    ptr.* = value;
}

fn mmioWriteU64(addr: usize, value: u64) void {
    const ptr: *volatile u64 = @ptrFromInt(addr);
    ptr.* = value;
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

fn initBlockDevice() bool {
    if (!waitMapMmioPage(mmio_common_page_va, block_state.common_page_paddr, true)) return false;
    if (!waitMapMmioPage(mmio_notify_page_va, block_state.notify_page_paddr, true)) return false;
    if (block_state.isr_page_paddr != 0 and !waitMapMmioPage(mmio_isr_page_va, block_state.isr_page_paddr, false)) return false;

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
    if (queueSubmit(block_state.queue_submit_token, queue_index_request) != 0) return false;
    if (queueNotify(block_state.queue_notify_token, queue_index_request) != 0) return false;
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

fn entryParentMatches(entry: *const layout.VolumeDirEntry, parent_index: ?usize) bool {
    return layout.dirEntryParentIndex(entry) == parent_index;
}

fn findChildEntryIndexByName(parent_index: ?usize, name: []const u8) ?usize {
    if (!rootfs_ready) return null;
    for (&rootfs_entries, 0..) |*entry, index| {
        if (!layout.dirEntryUsed(entry)) continue;
        if (!entryParentMatches(entry, parent_index)) continue;
        if (std.mem.eql(u8, layout.dirEntryName(entry), name)) return index;
    }
    return null;
}

fn skipSlashes(path: []const u8, pos: *usize) void {
    while (pos.* < path.len and path[pos.*] == '/') : (pos.* += 1) {}
}

fn findEntryByPath(abs_path: []const u8) ?*const layout.VolumeDirEntry {
    if (abs_path.len == 0 or abs_path[0] != '/') return null;

    var current_index: ?usize = null;
    var pos: usize = 0;
    skipSlashes(abs_path, &pos);
    if (pos >= abs_path.len) return null;

    while (pos < abs_path.len) {
        const start = pos;
        while (pos < abs_path.len and abs_path[pos] != '/') : (pos += 1) {}
        const component = abs_path[start..pos];
        if (component.len == 0 or std.mem.eql(u8, component, ".")) {
            skipSlashes(abs_path, &pos);
            continue;
        }
        if (std.mem.eql(u8, component, "..")) {
            current_index = if (current_index) |entry_index|
                if (layout.dirEntryParentIndex(&rootfs_entries[entry_index])) |parent_index|
                    parent_index
                else
                    null
            else
                null;
            skipSlashes(abs_path, &pos);
            continue;
        }

        current_index = findChildEntryIndexByName(current_index, component) orelse return null;
        skipSlashes(abs_path, &pos);
    }

    return if (current_index) |entry_index| &rootfs_entries[entry_index] else null;
}

fn readFileEntry(entry: *const layout.VolumeDirEntry, offset: u64, out: []u8) ?usize {
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

fn allocExecSlot(file_size: u64) ?struct { base_va: u64 } {
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
        return .{ .base_va = base_va };
    }
    return null;
}

fn loadExec(entry: *const layout.VolumeDirEntry) ?OpenExecResult {
    const slot = allocExecSlot(entry.file_size) orelse return null;
    const dst: [*]u8 = @ptrFromInt(slot.base_va);
    if (readFileEntry(entry, 0, dst[0..@intCast(entry.file_size)])) |bytes_read| {
        if (bytes_read != entry.file_size) return null;
    } else return null;
    const vm_token = installVmObject(slot.base_va, entry.file_size, .{ .read = true });
    if (image_abi.decodeVmObjectToken(vm_token) == null) return null;
    const exec_token = installExecImage(vm_token, .{ .exec = true });
    if (image_abi.decodeExecImageToken(exec_token) == null) return null;
    return .{
        .token = exec_token,
        .file_bytes = entry.file_size,
    };
}
