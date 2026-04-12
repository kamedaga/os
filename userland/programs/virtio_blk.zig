const std = @import("std");
const cap_transfer_abi = @import("support_root").cap_transfer_abi;
const block_bootstrap = @import("support_root").block_bootstrap_abi;
const block_protocol = @import("support_root").block_protocol;
const fs_abi = @import("support_root").fs_abi;

const syscall_alloc_map_pages: u64 = 0xC;
const syscall_map_mmio: u64 = 0xB;
const syscall_map_page: u64 = 0x2;
const syscall_queue_submit: u64 = 0xE;
const syscall_queue_notify: u64 = 0xF;
const syscall_wait_event: u64 = 0x17;
const syscall_log: u64 = 0x9;
const syscall_install_endpoint: u64 = 0x26;
const syscall_signal_endpoint: u64 = 0x2C;

const syscall_ok: u64 = 0;
const reply_endpoint_id_base: u64 = 0xC0;

const config_page_va: usize = 0x3C00_2000;
const common_page_va: usize = 0x2000_4000;
const notify_page_va: usize = 0x2000_5000;
const isr_page_va: usize = 0x2000_6000;
const device_page_va: usize = 0x2000_7000;
const queue_page0_va: usize = 0x2000_8000;
const queue_page1_va: usize = 0x2000_9000;
const dma_data_page_va: usize = 0x2000_A000;
const session_base_va: u64 = 0x3C01_0000;
const session_va_stride: u64 = 0x2000;
const session_poll_timeout_ticks: u64 = 1;
const request_completion_spin_limit: usize = 50_000_000;

const common_device_feature_select: usize = 0x00;
const common_driver_feature_select: usize = 0x08;
const common_driver_feature: usize = 0x0C;
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

const queue_index_request: u16 = 0;
const queue_size: u16 = 8;
const queue_used_offset: usize = 4096;
const queue_buffers_offset: usize = 4176;
const desc_flag_next: u16 = 1 << 0;
const desc_flag_write: u16 = 1 << 1;

const request_type_in: u32 = 0;
const request_type_out: u32 = 1;
const request_type_flush: u32 = 4;
const request_status_ok: u8 = 0;

const max_sessions: usize = 4;

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

const BootState = struct {
    endpoint_id: u64 = 0,
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
    queue_paddr0: u64 = 0,
    queue_paddr1: u64 = 0,
    dma_data_paddr: u64 = 0,
};

const Session = struct {
    active: bool = false,
    client_process_slot: u64 = 0,
    request_paddr: u64 = 0,
    response_paddr: u64 = 0,
    request_va: u64 = 0,
    response_va: u64 = 0,
    reply_endpoint_id: u64 = 0,
    last_completed_seq: u64 = 0,
    object_token: u64 = 0,
    rights: fs_abi.Rights = .{},
};

var boot_state: BootState = .{};
var sessions: [max_sessions]Session = [_]Session{.{}} ** max_sessions;
var common_base: usize = 0;
var notify_addr: usize = 0;
var isr_base: usize = 0;
var last_used_idx: u16 = 0;

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

fn signalEndpoint(endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_signal_endpoint),
          [arg0] "{rdi}" (endpoint_id),
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

const fs_token_tag: u64 = 1 << 63;
var next_fs_token: u64 = 1;

fn allocFsToken() u64 {
    const t = fs_token_tag | next_fs_token;
    next_fs_token += 1;
    return t;
}

fn readCfgU64(index: usize) u64 {
    const cfg: [*]const volatile u64 = @ptrFromInt(config_page_va);
    return cfg[index];
}

fn writeCfgU64(index: usize, value: u64) void {
    const cfg: [*]volatile u64 = @ptrFromInt(config_page_va);
    cfg[index] = value;
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
    if (offset < 4096) return queue_paddr0 + @as(u64, @intCast(offset));
    return queue_paddr1 + @as(u64, @intCast(offset - 4096));
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

fn sessionRequestVa(slot: usize) u64 {
    return session_base_va + @as(u64, @intCast(slot)) * session_va_stride;
}

fn sessionResponseVa(slot: usize) u64 {
    return sessionRequestVa(slot) + 0x1000;
}

fn requestHeader(session: *const Session) *volatile block_protocol.BlockRequestHeader {
    return @ptrFromInt(session.request_va);
}

fn responseHeader(session: *const Session) *volatile block_protocol.BlockResponseHeader {
    return @ptrFromInt(session.response_va);
}

fn requestPayload(session: *const Session) [*]volatile u8 {
    return @ptrFromInt(session.request_va + block_protocol.request_header_bytes);
}

fn responsePayload(session: *const Session) [*]volatile u8 {
    return @ptrFromInt(session.response_va + block_protocol.response_header_bytes);
}

fn clearPage(va: u64) void {
    const bytes: [*]volatile u8 = @ptrFromInt(va);
    var i: usize = 0;
    while (i < block_protocol.page_bytes) : (i += 1) {
        bytes[i] = 0;
    }
}

fn compilerBarrier() void {
    asm volatile ("" ::: .{ .memory = true });
}

fn parseBootState() ?BootState {
    if (readCfgU64(0) != block_bootstrap.config_magic or readCfgU64(1) != block_bootstrap.config_version) return null;
    const block_size = readCfgU64(block_bootstrap.logical_block_size_index);
    if (block_size == 0 or (block_size % 512) != 0) return null;
    const sectors_per_block = block_size / 512;
    if (sectors_per_block == 0) return null;
    const capacity_sectors = readCfgU64(block_bootstrap.capacity_sectors_index);
    return .{
        .endpoint_id = readCfgU64(block_bootstrap.endpoint_id_index),
        .capacity_sectors = capacity_sectors,
        .logical_block_size = block_size,
        .capacity_blocks = capacity_sectors / sectors_per_block,
        .sectors_per_block = sectors_per_block,
        .common_page_paddr = readCfgU64(block_bootstrap.common_page_paddr_index),
        .notify_page_paddr = readCfgU64(block_bootstrap.notify_page_paddr_index),
        .isr_page_paddr = readCfgU64(block_bootstrap.isr_page_paddr_index),
        .device_page_paddr = readCfgU64(block_bootstrap.device_page_paddr_index),
        .common_page_offset = readCfgU64(block_bootstrap.common_page_offset_index),
        .notify_page_offset = readCfgU64(block_bootstrap.notify_page_offset_index),
        .isr_page_offset = readCfgU64(block_bootstrap.isr_page_offset_index),
        .device_page_offset = readCfgU64(block_bootstrap.device_page_offset_index),
        .notify_off_multiplier = readCfgU64(block_bootstrap.notify_off_multiplier_index),
        .queue_submit_token = readCfgU64(block_bootstrap.queue_submit_token_index),
        .queue_notify_token = readCfgU64(block_bootstrap.queue_notify_token_index),
    };
}

fn waitForBootResources() void {
    while (true) {
        boot_state = parseBootState() orelse {
            _ = waitEvent(false, 1);
            asm volatile ("pause");
            continue;
        };
        if (boot_state.endpoint_id != 0 and
            boot_state.queue_submit_token != 0 and
            boot_state.queue_notify_token != 0 and
            boot_state.capacity_blocks != 0)
        {
            return;
        }
        _ = waitEvent(false, 1);
        asm volatile ("pause");
    }
}

fn initQueueMemory() bool {
    var queue_paddrs: [3]u64 = .{ 0, 0, 0 };
    if (allocMapPages(queue_page0_va, 3, true, @intFromPtr(&queue_paddrs)) != syscall_ok) return false;
    boot_state.queue_paddr0 = queue_paddrs[0];
    boot_state.queue_paddr1 = queue_paddrs[1];
    boot_state.dma_data_paddr = queue_paddrs[2];
    return true;
}

fn initVirtio() bool {
    while (mapMmioPage(common_page_va, boot_state.common_page_paddr, true) != syscall_ok) {
        _ = waitEvent(false, 1);
    }
    while (mapMmioPage(notify_page_va, boot_state.notify_page_paddr, true) != syscall_ok) {
        _ = waitEvent(false, 1);
    }
    if (boot_state.isr_page_paddr != 0) {
        while (mapMmioPage(isr_page_va, boot_state.isr_page_paddr, false) != syscall_ok) {
            _ = waitEvent(false, 1);
        }
    }
    if (boot_state.device_page_paddr != 0) {
        while (mapMmioPage(device_page_va, boot_state.device_page_paddr, false) != syscall_ok) {
            _ = waitEvent(false, 1);
        }
    }

    if (!initQueueMemory()) return false;

    common_base = common_page_va + @as(usize, @intCast(boot_state.common_page_offset));
    const notify_base = notify_page_va + @as(usize, @intCast(boot_state.notify_page_offset));
    isr_base = if (boot_state.isr_page_paddr != 0) isr_page_va + @as(usize, @intCast(boot_state.isr_page_offset)) else 0;

    mmioWriteU8(common_base + common_device_status, 0);
    mmioWriteU8(common_base + common_device_status, status_acknowledge | status_driver);
    mmioWriteU32(common_base + common_device_feature_select, 0);
    mmioWriteU32(common_base + common_driver_feature_select, 0);
    mmioWriteU32(common_base + common_driver_feature, 0);
    mmioWriteU32(common_base + common_driver_feature_select, 1);
    mmioWriteU32(common_base + common_driver_feature, 0);

    mmioWriteU16(common_base + common_queue_select, queue_index_request);
    const max_size = mmioReadU16(common_base + common_queue_size);
    if (max_size == 0 or max_size < queue_size) return false;
    mmioWriteU16(common_base + common_queue_size, queue_size);
    mmioWriteU64(common_base + common_queue_desc, boot_state.queue_paddr0);
    mmioWriteU64(common_base + common_queue_avail, boot_state.queue_paddr0 + (@as(u64, queue_size) * @sizeOf(VirtqDesc)));
    mmioWriteU64(common_base + common_queue_used, queueRegionPhys(boot_state.queue_paddr0, boot_state.queue_paddr1, queue_used_offset));

    const header_paddr = queueRegionPhys(boot_state.queue_paddr0, boot_state.queue_paddr1, queue_buffers_offset);
    const status_paddr = header_paddr + @sizeOf(VirtioBlkReqHeader);
    const desc0 = queueDescPtr(0);
    const desc1 = queueDescPtr(1);
    const desc2 = queueDescPtr(2);
    desc0.* = .{
        .addr = header_paddr,
        .len = @sizeOf(VirtioBlkReqHeader),
        .flags = desc_flag_next,
        .next = 1,
    };
    desc1.* = .{
        .addr = boot_state.dma_data_paddr,
        .len = 0,
        .flags = desc_flag_next,
        .next = 2,
    };
    desc2.* = .{
        .addr = status_paddr,
        .len = 1,
        .flags = desc_flag_write,
        .next = 0,
    };

    const queue_notify_off = mmioReadU16(common_base + common_queue_notify_off);
    notify_addr = notify_base + @as(usize, queue_notify_off) * @as(usize, @intCast(boot_state.notify_off_multiplier));
    mmioWriteU16(common_base + common_queue_enable, 1);
    if (queueSubmit(boot_state.queue_submit_token, queue_index_request) != syscall_ok) return false;
    if (queueNotify(boot_state.queue_notify_token, queue_index_request) != syscall_ok) return false;
    mmioWriteU8(common_base + common_device_status, mmioReadU8(common_base + common_device_status) | status_driver_ok);
    return true;
}

fn requestDataPtr() [*]volatile u8 {
    return @ptrFromInt(dma_data_page_va);
}

fn clientRights() fs_abi.Rights {
    return .{
        .read = true,
        .write = true,
    };
}

fn copyVolatileToPlain(src: [*]volatile u8, dest: []u8) void {
    var i: usize = 0;
    while (i < dest.len) : (i += 1) {
        dest[i] = src[i];
    }
}

fn copyPlainToVolatile(dest: [*]volatile u8, src: []const u8) void {
    var i: usize = 0;
    while (i < src.len) : (i += 1) {
        dest[i] = src[i];
    }
}

fn writeResponseHeader(
    session: *Session,
    op: block_protocol.Opcode,
    request_seq: u64,
    status: block_protocol.Status,
    result_token: u64,
    object_kind: fs_abi.ObjectKind,
    inline_bytes: u16,
    arg0: u64,
    arg1: u64,
) void {
    const response = responseHeader(session);
    response.magic = block_protocol.response_magic;
    response.version = block_protocol.version;
    response.op = block_protocol.opcodeRaw(op);
    response.status = block_protocol.statusRaw(status);
    response.result_flags = 0;
    response.result_token = result_token;
    response.inline_bytes = inline_bytes;
    response.object_kind = block_protocol.objectKindRaw(object_kind);
    response.reserved0 = 0;
    response.reserved1 = 0;
    response.arg0 = arg0;
    response.arg1 = arg1;
    compilerBarrier();
    response.response_seq = request_seq;
    if (session.reply_endpoint_id != 0) _ = signalEndpoint(session.reply_endpoint_id);
}

fn replyStatus(session: *Session, op: block_protocol.Opcode, request_seq: u64, status: block_protocol.Status) void {
    clearPage(session.response_va);
    writeResponseHeader(session, op, request_seq, status, 0, .none, 0, boot_state.logical_block_size, boot_state.capacity_blocks);
}

fn executeBlockRequest(request_type: u32, block_index: u64, block_count: u32, write: bool) bool {
    const byte_count = @as(usize, block_count) * @as(usize, @intCast(boot_state.logical_block_size));
    if (byte_count > 4096) return false;
    const sectors = block_index * boot_state.sectors_per_block;
    const header = reqHeaderPtr();
    header.* = .{
        .request_type = request_type,
        .reserved = 0,
        .sector = sectors,
    };
    const status = reqStatusPtr();
    status.* = 0xFF;

    const desc0 = queueDescPtr(0);
    const desc1 = queueDescPtr(1);
    if (request_type == request_type_flush) {
        desc0.flags = desc_flag_next;
        desc0.next = 2;
    } else {
        desc0.flags = desc_flag_next;
        desc0.next = 1;
        desc1.len = @intCast(byte_count);
        desc1.flags = desc_flag_next | if (!write) desc_flag_write else @as(u16, 0);
        desc1.next = 2;
    }
    queuePushAvail(0);
    mmioWriteU16(notify_addr, queue_index_request);

    var spins: usize = 0;
    while (spins < request_completion_spin_limit) : (spins += 1) {
        if (queueUsedIdxPtr().* != last_used_idx) {
            if (isr_base != 0) _ = mmioReadU8(isr_base);
            _ = queueUsedRingPtr()[@intCast(last_used_idx % queue_size)];
            last_used_idx +%= 1;
            return status.* == request_status_ok;
        }
        if (isr_base != 0 and (spins & 0xFF) == 0) _ = mmioReadU8(isr_base);
        asm volatile ("pause");
    }
    return false;
}

fn resolveSession(session: *const Session, request: *const volatile block_protocol.BlockRequestHeader) ?fs_abi.Rights {
    if (!session.active) return null;
    if (request.object_token != session.object_token) return null;
    return session.rights;
}

fn handleIdentify(session: *Session, request_seq: u64) void {
    clearPage(session.response_va);
    writeResponseHeader(
        session,
        .identify,
        request_seq,
        .ok,
        0,
        .block_device,
        0,
        boot_state.logical_block_size,
        boot_state.capacity_blocks,
    );
}

fn handleReadBlocks(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const rights = resolveSession(session, request) orelse {
        replyStatus(session, .read_blocks, request_seq, .not_found);
        return;
    };
    if (!rights.read) {
        replyStatus(session, .read_blocks, request_seq, .no_right);
        return;
    }
    const byte_count = @as(u64, request.block_count) * boot_state.logical_block_size;
    const end_block = request.block_index + request.block_count;
    if (request.block_count == 0 or byte_count > block_protocol.response_payload_bytes or end_block > boot_state.capacity_blocks) {
        replyStatus(session, .read_blocks, request_seq, .too_big);
        return;
    }
    if (!executeBlockRequest(request_type_in, request.block_index, request.block_count, false)) {
        replyStatus(session, .read_blocks, request_seq, .io_error);
        return;
    }
    clearPage(session.response_va);
    const data = @as([*]const u8, @ptrCast(@volatileCast(requestDataPtr())));
    copyPlainToVolatile(responsePayload(session), data[0..@intCast(byte_count)]);
    writeResponseHeader(
        session,
        .read_blocks,
        request_seq,
        .ok,
        0,
        .block_device,
        @intCast(byte_count),
        boot_state.logical_block_size,
        boot_state.capacity_blocks,
    );
}

fn handleWriteBlocks(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const rights = resolveSession(session, request) orelse {
        replyStatus(session, .write_blocks, request_seq, .not_found);
        return;
    };
    if (!rights.write) {
        replyStatus(session, .write_blocks, request_seq, .no_right);
        return;
    }
    const byte_count = @as(u64, request.block_count) * boot_state.logical_block_size;
    const end_block = request.block_index + request.block_count;
    if (request.block_count == 0 or request.inline_bytes != byte_count or byte_count > 4096 or end_block > boot_state.capacity_blocks) {
        replyStatus(session, .write_blocks, request_seq, .too_big);
        return;
    }
    const data = @as([*]u8, @ptrCast(@volatileCast(requestDataPtr())));
    copyVolatileToPlain(requestPayload(session), data[0..@intCast(byte_count)]);
    if (!executeBlockRequest(request_type_out, request.block_index, request.block_count, true)) {
        replyStatus(session, .write_blocks, request_seq, .io_error);
        return;
    }
    clearPage(session.response_va);
    writeResponseHeader(
        session,
        .write_blocks,
        request_seq,
        .ok,
        0,
        .block_device,
        0,
        boot_state.logical_block_size,
        boot_state.capacity_blocks,
    );
}

fn handleFlush(session: *Session, request_seq: u64) void {
    const request = requestHeader(session);
    const rights = resolveSession(session, request) orelse {
        replyStatus(session, .flush, request_seq, .not_found);
        return;
    };
    if (!rights.write) {
        replyStatus(session, .flush, request_seq, .no_right);
        return;
    }
    if (!executeBlockRequest(request_type_flush, 0, 0, false)) {
        replyStatus(session, .flush, request_seq, .io_error);
        return;
    }
    clearPage(session.response_va);
    writeResponseHeader(
        session,
        .flush,
        request_seq,
        .ok,
        0,
        .block_device,
        0,
        boot_state.logical_block_size,
        boot_state.capacity_blocks,
    );
}

fn processSessionRequest(session: *Session) void {
    const request = requestHeader(session);
    if (request.magic != block_protocol.request_magic or request.version != block_protocol.version) return;
    const request_seq = request.request_seq;
    if (request_seq == 0 or request_seq <= session.last_completed_seq) return;
    const op = std.meta.intToEnum(block_protocol.Opcode, request.op) catch {
        replyStatus(session, .connect, request_seq, .invalid);
        session.last_completed_seq = request_seq;
        return;
    };
    switch (op) {
        .connect => replyStatus(session, .connect, request_seq, .busy),
        .identify => handleIdentify(session, request_seq),
        .read_blocks => handleReadBlocks(session, request_seq),
        .write_blocks => handleWriteBlocks(session, request_seq),
        .flush => handleFlush(session, request_seq),
    }
    session.last_completed_seq = request_seq;
}

fn handleConnectRequest(request_paddr: u64) void {
    _ = userLog("VirtioBlk: connect request\n");
    for (&sessions, 0..) |*session, slot| {
        if (session.active) continue;
        const req_va = sessionRequestVa(slot);
        const resp_va = sessionResponseVa(slot);
        if (mapPage(req_va, request_paddr, false) != syscall_ok) {
            _ = userLog("VirtioBlk: map request page failed\n");
            return;
        }
        const request: *volatile block_protocol.BlockRequestHeader = @ptrFromInt(req_va);
        if (request.magic != block_protocol.request_magic or
            request.version != block_protocol.version or
            request.op != block_protocol.opcodeRaw(.connect) or
            request.request_seq == 0 or
            request.arg0 < 0x1000)
        {
            _ = userLog("VirtioBlk: invalid connect request\n");
            return;
        }
        if (mapPage(resp_va, request.arg0, true) != syscall_ok) {
            _ = userLog("VirtioBlk: map response page failed\n");
            return;
        }
        const reply_endpoint_id = reply_endpoint_id_base + @as(u64, @intCast(slot));
        if (installEndpoint(reply_endpoint_id, request.arg1) != syscall_ok) {
            _ = userLog("VirtioBlk: install reply endpoint failed\n");
            return;
        }
        const rights = clientRights();
        const child_token = allocFsToken();
        session.* = .{
            .active = true,
            .client_process_slot = request.arg1,
            .request_paddr = request_paddr,
            .response_paddr = request.arg0,
            .request_va = req_va,
            .response_va = resp_va,
            .reply_endpoint_id = reply_endpoint_id,
            .last_completed_seq = 0,
            .object_token = child_token,
            .rights = rights,
        };
        clearPage(resp_va);
        writeResponseHeader(
            session,
            .connect,
            request.request_seq,
            .ok,
            child_token,
            .block_device,
            0,
            boot_state.logical_block_size,
            boot_state.capacity_blocks,
        );
        session.last_completed_seq = request.request_seq;
        return;
    }
    _ = userLog("VirtioBlk: session table full\n");
}

fn pollSessions() void {
    for (&sessions) |*session| {
        if (!session.active) continue;
        processSessionRequest(session);
    }
}

pub export fn _start() noreturn {
    _ = userLog("VirtioBlk: started\n");
    waitForBootResources();
    if (!initVirtio()) {
        _ = userLog("VirtioBlk: init failed\n");
        while (true) asm volatile ("pause");
    }
    writeCfgU64(block_bootstrap.driver_status_index, block_bootstrap.driver_status_ready);
    _ = userLog("VirtioBlk: queue ready\n");

    while (true) {
        const received = waitEvent(true, session_poll_timeout_ticks);
        if (received >= 0x1000) {
            const request_paddr = acceptCapTransfer(received);
            if (request_paddr >= 0x1000) {
                handleConnectRequest(request_paddr);
            } else {
                _ = userLog("VirtioBlk: accept cap transfer failed\n");
            }
        }
        pollSessions();
    }
}
