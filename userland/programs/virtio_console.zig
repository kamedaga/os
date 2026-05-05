const console_bootstrap = @import("abi_root").console_bootstrap_abi;
const console_protocol = @import("abi_root").console_protocol;
const cap_transfer_abi = @import("abi_root").cap_transfer_abi;
const process_abi = @import("abi_root").process_abi;
const queue_abi = @import("abi_root").queue_abi;
const user_vm = @import("abi_root").user_vm;

const syscall_map_page: u64 = 0x2;
const syscall_log: u64 = 0x9;
const syscall_map_mmio: u64 = 0xB;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_queue_submit: u64 = 0xE;
const syscall_queue_notify: u64 = 0xF;
const syscall_wait_event: u64 = 0x17;
const syscall_install_endpoint: u64 = 0x26;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_iommu_authorize: u64 = queue_abi.syscall_iommu_authorize;
const syscall_dma_map_create: u64 = queue_abi.syscall_dma_map_create;
const syscall_dma_map_set_state: u64 = queue_abi.syscall_dma_map_set_state;
const syscall_dma_map_release: u64 = queue_abi.syscall_dma_map_release;

const syscall_ok: u64 = 0;
const config_page_va: usize = @intCast(process_abi.standard_config_target_va);
const cap_transfer_id_min: u64 = 0x1000;
const console_request_va: usize = 0x2720_0000;
const console_response_va: usize = 0x2720_1000;
const console_reply_endpoint_id: u64 = 0xE9;

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
const status_features_ok: u8 = 0x08;

const queue_size: u16 = 8;
const queue_used_offset: usize = 2048;
const rx_buffer_count: usize = 4;
const rx_buffer_bytes: usize = 256;
const tx_buffer_bytes: usize = 256;
const rx_ring_bytes: usize = 4096;
const edit_line_bytes: usize = 1024;
const desc_flag_next: u16 = 1 << 0;
const desc_flag_write: u16 = 1 << 1;

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

const QueueState = struct {
    index: u16,
    submit_token: u64,
    notify_token: u64,
    page_va: usize = 0,
    page_paddr: u64 = 0,
    notify_addr: usize = 0,
    last_used_idx: u16 = 0,
};

const BootState = struct {
    endpoint_id: u64 = 0,
    resource_id: queue_abi.DeviceId = queue_abi.invalid_device_id,
    common_page_paddr: u64 = 0,
    notify_page_paddr: u64 = 0,
    isr_page_paddr: u64 = 0,
    device_page_paddr: u64 = 0,
    common_page_offset: u64 = 0,
    notify_page_offset: u64 = 0,
    isr_page_offset: u64 = 0,
    device_page_offset: u64 = 0,
    notify_off_multiplier: u64 = 0,
    iommu_token: u64 = 0,
    command_token: u64 = 0,
};

const ConsoleSession = struct {
    active: bool = false,
    request_paddr: u64 = 0,
    response_paddr: u64 = 0,
    reply_endpoint_id: u64 = 0,
    session_nonce: u64 = 0,
    last_completed_seq: u64 = 0,
};

var boot_state: BootState = .{};
var console_session: ConsoleSession = .{};
var rx_queue = QueueState{ .index = console_bootstrap.rx_queue_index, .submit_token = 0, .notify_token = 0 };
var tx_queue = QueueState{ .index = console_bootstrap.tx_queue_index, .submit_token = 0, .notify_token = 0 };
var common_base: usize = 0;
var notify_base: usize = 0;
var isr_base: usize = 0;
var common_page_va: usize = 0;
var notify_page_va: usize = 0;
var isr_page_va: usize = 0;
var device_page_va: usize = 0;
var rx_buffer_va: usize = 0;
var tx_buffer_va: usize = 0;
var rx_buffer_paddrs: [rx_buffer_count]u64 = [_]u64{0} ** rx_buffer_count;
var tx_buffer_paddr: u64 = 0;
var rx_dma_tokens: [rx_buffer_count]u64 = [_]u64{0} ** rx_buffer_count;
var tx_dma_token: u64 = 0;
var rx_ring: [rx_ring_bytes]u8 = [_]u8{0} ** rx_ring_bytes;
var rx_ring_head: usize = 0;
var rx_ring_len: usize = 0;
var edit_line: [edit_line_bytes]u8 = [_]u8{0} ** edit_line_bytes;
var edit_line_len: usize = 0;
var input_prev_was_cr: bool = false;
var pending_read_seq: u64 = 0;
var pending_read_max_len: usize = 0;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn allocMapPages(base_va: usize, page_count: usize, writable: bool, paddrs_out: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_map_pages),
          [arg0] "{rdi}" (@as(u64, @intCast(base_va))),
          [arg1] "{rsi}" (@as(u64, @intCast(page_count))),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
          [arg3] "{r8}" (paddrs_out),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapPage(va: usize, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (@as(u64, @intCast(va))),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapMmioPage(va: usize, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_mmio),
          [arg0] "{rdi}" (@as(u64, @intCast(va))),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn acceptCapTransfer(transfer_id: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (cap_transfer_abi.syscall_accept_cap_transfer),
          [arg0] "{rdi}" (transfer_id),
          [arg1] "{rsi}" (@as(u64, 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installEndpoint(endpoint_id: u64, target_process_slot: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_install_endpoint),
          [arg0] "{rdi}" (@as(u64, 0)),
          [arg1] "{rsi}" (endpoint_id),
          [arg2] "{rdx}" (target_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn signalEndpoint(endpoint_id: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_signal_endpoint),
          [arg0] "{rdi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn queueSubmit(token: u64, queue_index: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_queue_submit),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (queue_index),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn queueNotify(token: u64, queue_index: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_queue_notify),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (queue_index),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn iommuAuthorize(token: u64, device: queue_abi.DeviceId, op: queue_abi.IommuOperation) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_iommu_authorize),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (device),
          [arg2] "{rdx}" (@as(u64, @intFromEnum(op))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn dmaMapCreate(device: queue_abi.DeviceId, paddr_start: u64, length: u64, direction: queue_abi.DmaDirection) u64 {
    const raw = asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_dma_map_create),
          [arg0] "{rdi}" (device),
          [arg1] "{rsi}" (paddr_start),
          [arg2] "{rdx}" (length),
          [arg3] "{r8}" (@as(u64, @intFromEnum(direction))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
    return queue_abi.decodeDmaMappingToken(raw) orelse 0;
}

fn dmaMapSetState(token: u64, state: queue_abi.DmaMappingState) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_dma_map_set_state),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (@as(u64, @intFromEnum(state))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn dmaMapRelease(token: u64) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_dma_map_release),
          [arg0] "{rdi}" (token),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mmioReadU8(addr: usize) u8 {
    const ptr: *volatile u8 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioReadU16(addr: usize) u16 {
    const ptr: *volatile u16 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioReadU32(addr: usize) u32 {
    const ptr: *volatile u32 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioWriteU8(addr: usize, value: u8) void {
    const ptr: *volatile u8 = @ptrFromInt(addr);
    ptr.* = value;
}

fn mmioWriteU16(addr: usize, value: u16) void {
    const ptr: *volatile u16 = @ptrFromInt(addr);
    ptr.* = value;
}

fn mmioWriteU32(addr: usize, value: u32) void {
    const ptr: *volatile u32 = @ptrFromInt(addr);
    ptr.* = value;
}

fn mmioWriteU64(addr: usize, value: u64) void {
    const ptr: *volatile u64 = @ptrFromInt(addr);
    ptr.* = value;
}

fn readCfgU64(index: usize) u64 {
    const words: [*]volatile u64 = @ptrFromInt(config_page_va);
    return words[index];
}

fn writeCfgU64(index: usize, value: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(config_page_va);
    words[index] = value;
}

fn parseBootState() ?BootState {
    if (readCfgU64(0) != console_bootstrap.config_magic or readCfgU64(1) != console_bootstrap.config_version) return null;
    rx_queue.submit_token = readCfgU64(console_bootstrap.rx_queue_submit_token_index);
    rx_queue.notify_token = readCfgU64(console_bootstrap.rx_queue_notify_token_index);
    tx_queue.submit_token = readCfgU64(console_bootstrap.tx_queue_submit_token_index);
    tx_queue.notify_token = readCfgU64(console_bootstrap.tx_queue_notify_token_index);
    return .{
        .endpoint_id = readCfgU64(console_bootstrap.endpoint_id_index),
        .resource_id = readCfgU64(console_bootstrap.resource_id_index),
        .common_page_paddr = readCfgU64(console_bootstrap.common_page_paddr_index),
        .notify_page_paddr = readCfgU64(console_bootstrap.notify_page_paddr_index),
        .isr_page_paddr = readCfgU64(console_bootstrap.isr_page_paddr_index),
        .device_page_paddr = readCfgU64(console_bootstrap.device_page_paddr_index),
        .common_page_offset = readCfgU64(console_bootstrap.common_page_offset_index),
        .notify_page_offset = readCfgU64(console_bootstrap.notify_page_offset_index),
        .isr_page_offset = readCfgU64(console_bootstrap.isr_page_offset_index),
        .device_page_offset = readCfgU64(console_bootstrap.device_page_offset_index),
        .notify_off_multiplier = readCfgU64(console_bootstrap.notify_off_multiplier_index),
        .iommu_token = readCfgU64(console_bootstrap.iommu_token_index),
        .command_token = readCfgU64(console_bootstrap.command_token_index),
    };
}

fn waitForBootResources() void {
    while (true) {
        boot_state = parseBootState() orelse {
            _ = waitEvent(false, 1);
            continue;
        };
        if (boot_state.endpoint_id != 0 and
            boot_state.resource_id != queue_abi.invalid_device_id and
            rx_queue.submit_token != 0 and
            rx_queue.notify_token != 0 and
            tx_queue.submit_token != 0 and
            tx_queue.notify_token != 0)
        {
            return;
        }
        _ = waitEvent(false, 1);
    }
}

fn reserveVirtioTargetVas() bool {
    if (common_page_va != 0) return true;
    const mmio_base = user_vm.reservePages(4) orelse return false;
    common_page_va = mmio_base;
    notify_page_va = mmio_base + user_vm.page_bytes;
    isr_page_va = mmio_base + 2 * user_vm.page_bytes;
    device_page_va = mmio_base + 3 * user_vm.page_bytes;
    return true;
}

fn descPtr(queue: *const QueueState, index: usize) *volatile VirtqDesc {
    return @ptrFromInt(queue.page_va + index * @sizeOf(VirtqDesc));
}

fn availIdxPtr(queue: *const QueueState) *volatile u16 {
    return @ptrFromInt(queue.page_va + @as(usize, queue_size) * @sizeOf(VirtqDesc) + 2);
}

fn availRingPtr(queue: *const QueueState) [*]volatile u16 {
    return @ptrFromInt(queue.page_va + @as(usize, queue_size) * @sizeOf(VirtqDesc) + 4);
}

fn usedIdxPtr(queue: *const QueueState) *volatile u16 {
    return @ptrFromInt(queue.page_va + queue_used_offset + 2);
}

fn usedRingPtr(queue: *const QueueState) [*]volatile VirtqUsedElem {
    return @ptrFromInt(queue.page_va + queue_used_offset + 4);
}

fn queuePushAvail(queue: *QueueState, desc_index: u16) void {
    const idx = availIdxPtr(queue).*;
    availRingPtr(queue)[@intCast(idx % queue_size)] = desc_index;
    asm volatile ("" ::: .{ .memory = true });
    availIdxPtr(queue).* = idx +% 1;
}

fn setupQueue(queue: *QueueState) bool {
    mmioWriteU16(common_base + common_queue_select, queue.index);
    const max_size = mmioReadU16(common_base + common_queue_size);
    if (max_size == 0 or max_size < queue_size) return false;
    mmioWriteU16(common_base + common_queue_size, queue_size);
    mmioWriteU64(common_base + common_queue_desc, queue.page_paddr);
    mmioWriteU64(common_base + common_queue_avail, queue.page_paddr + @as(u64, queue_size) * @sizeOf(VirtqDesc));
    mmioWriteU64(common_base + common_queue_used, queue.page_paddr + queue_used_offset);
    const queue_notify_off = mmioReadU16(common_base + common_queue_notify_off);
    queue.notify_addr = notify_base + @as(usize, queue_notify_off) * @as(usize, @intCast(boot_state.notify_off_multiplier));
    mmioWriteU16(common_base + common_queue_enable, 1);
    return true;
}

fn initQueueMemory() bool {
    const rx_page = user_vm.allocMapPage(true) orelse return false;
    const tx_page = user_vm.allocMapPage(true) orelse return false;
    rx_queue.page_va = rx_page.va;
    rx_queue.page_paddr = rx_page.paddr;
    tx_queue.page_va = tx_page.va;
    tx_queue.page_paddr = tx_page.paddr;

    var i: usize = 0;
    while (i < rx_buffer_count) : (i += 1) {
        const page = user_vm.allocMapPage(true) orelse return false;
        if (i == 0) rx_buffer_va = page.va;
        rx_buffer_paddrs[i] = page.paddr;
        if (i != 0 and page.va != rx_buffer_va + i * user_vm.page_bytes) return false;
    }
    const tx_page_buf = user_vm.allocMapPage(true) orelse return false;
    tx_buffer_va = tx_page_buf.va;
    tx_buffer_paddr = tx_page_buf.paddr;
    return true;
}

fn authorizeRxDma() bool {
    if (boot_state.iommu_token == 0) return true;
    if (iommuAuthorize(boot_state.iommu_token, boot_state.resource_id, .map_read) != syscall_ok) return false;
    var i: usize = 0;
    while (i < rx_buffer_count) : (i += 1) {
        rx_dma_tokens[i] = dmaMapCreate(boot_state.resource_id, rx_buffer_paddrs[i], rx_buffer_bytes, .write);
        if (rx_dma_tokens[i] == 0) return false;
        if (dmaMapSetState(rx_dma_tokens[i], .in_flight) != syscall_ok) return false;
    }
    return true;
}

fn authorizeTxDma() bool {
    if (boot_state.iommu_token == 0) return true;
    if (iommuAuthorize(boot_state.iommu_token, boot_state.resource_id, .map_read) != syscall_ok) return false;
    tx_dma_token = dmaMapCreate(boot_state.resource_id, tx_buffer_paddr, tx_buffer_bytes, .read);
    if (tx_dma_token == 0) return false;
    return dmaMapSetState(tx_dma_token, .in_flight) == syscall_ok;
}

fn primeRxQueue() bool {
    var i: usize = 0;
    while (i < rx_buffer_count) : (i += 1) {
        descPtr(&rx_queue, i).* = .{
            .addr = rx_buffer_paddrs[i],
            .len = rx_buffer_bytes,
            .flags = desc_flag_write,
            .next = 0,
        };
        queuePushAvail(&rx_queue, @intCast(i));
    }
    if (queueSubmit(rx_queue.submit_token, rx_queue.index) != syscall_ok) return false;
    if (queueNotify(rx_queue.notify_token, rx_queue.index) != syscall_ok) return false;
    mmioWriteU16(rx_queue.notify_addr, rx_queue.index);
    return true;
}

fn initVirtio() bool {
    while (mapMmioPage(common_page_va, boot_state.common_page_paddr, true) != syscall_ok) _ = waitEvent(false, 1);
    while (mapMmioPage(notify_page_va, boot_state.notify_page_paddr, true) != syscall_ok) _ = waitEvent(false, 1);
    if (boot_state.isr_page_paddr != 0) {
        while (mapMmioPage(isr_page_va, boot_state.isr_page_paddr, false) != syscall_ok) _ = waitEvent(false, 1);
    }
    if (boot_state.device_page_paddr != 0) {
        while (mapMmioPage(device_page_va, boot_state.device_page_paddr, true) != syscall_ok) _ = waitEvent(false, 1);
    }
    if (!initQueueMemory()) return false;

    common_base = common_page_va + @as(usize, @intCast(boot_state.common_page_offset));
    notify_base = notify_page_va + @as(usize, @intCast(boot_state.notify_page_offset));
    isr_base = if (boot_state.isr_page_paddr != 0) isr_page_va + @as(usize, @intCast(boot_state.isr_page_offset)) else 0;

    mmioWriteU8(common_base + common_device_status, 0);
    mmioWriteU8(common_base + common_device_status, status_acknowledge | status_driver);
    mmioWriteU32(common_base + common_device_feature_select, 0);
    mmioWriteU32(common_base + common_driver_feature_select, 0);
    mmioWriteU32(common_base + common_driver_feature, 0);
    mmioWriteU8(common_base + common_device_status, mmioReadU8(common_base + common_device_status) | status_features_ok);
    if ((mmioReadU8(common_base + common_device_status) & status_features_ok) == 0) return false;

    if (!setupQueue(&rx_queue)) return false;
    if (!setupQueue(&tx_queue)) return false;
    if (!authorizeRxDma()) return false;
    if (!authorizeTxDma()) return false;
    if (!primeRxQueue()) return false;

    mmioWriteU8(common_base + common_device_status, mmioReadU8(common_base + common_device_status) | status_driver_ok);
    return true;
}

fn sendBytes(bytes: []const u8) bool {
    if (bytes.len == 0 or bytes.len > tx_buffer_bytes) return false;
    const dst: [*]volatile u8 = @ptrFromInt(tx_buffer_va);
    var i: usize = 0;
    while (i < bytes.len) : (i += 1) dst[i] = bytes[i];

    descPtr(&tx_queue, 0).* = .{
        .addr = tx_buffer_paddr,
        .len = @intCast(bytes.len),
        .flags = 0,
        .next = 0,
    };
    if (queueSubmit(tx_queue.submit_token, tx_queue.index) != syscall_ok) return false;
    queuePushAvail(&tx_queue, 0);
    if (queueNotify(tx_queue.notify_token, tx_queue.index) != syscall_ok) return false;
    mmioWriteU16(tx_queue.notify_addr, tx_queue.index);

    var spins: usize = 0;
    while (spins < 1_000_000) : (spins += 1) {
        if (usedIdxPtr(&tx_queue).* != tx_queue.last_used_idx) {
            if (isr_base != 0) _ = mmioReadU8(isr_base);
            _ = usedRingPtr(&tx_queue)[@intCast(tx_queue.last_used_idx % queue_size)];
            tx_queue.last_used_idx +%= 1;
            return true;
        }
        asm volatile ("pause");
    }
    return false;
}

fn clearPage(base_va: usize) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) words[i] = 0;
}

fn requestHeader() *volatile console_protocol.ConsoleRequestHeader {
    return @ptrFromInt(console_request_va);
}

fn responseHeader() *volatile console_protocol.ConsoleResponseHeader {
    return @ptrFromInt(console_response_va);
}

fn requestPayload() [*]volatile u8 {
    return @ptrFromInt(console_request_va + console_protocol.request_header_bytes);
}

fn responsePayload() [*]volatile u8 {
    return @ptrFromInt(console_response_va + console_protocol.response_header_bytes);
}

fn writeConsoleResponse(op: console_protocol.Opcode, seq: u64, status: console_protocol.Status, inline_bytes: u32, arg0: u64, arg1: u64) void {
    const response = responseHeader();
    response.magic = console_protocol.response_magic;
    response.version = console_protocol.version;
    response.op = console_protocol.opcodeRaw(op);
    response.status = console_protocol.statusRaw(status);
    response.result_flags = 0;
    response.inline_bytes = inline_bytes;
    response.reserved0 = 0;
    response.arg0 = arg0;
    response.arg1 = arg1;
    response.reserved1 = 0;
    response.reserved2 = 0;
    asm volatile ("" ::: .{ .memory = true });
    response.response_seq = seq;
    if (console_session.reply_endpoint_id != 0) _ = signalEndpoint(console_session.reply_endpoint_id);
}

fn rxRingPush(byte: u8) void {
    if (rx_ring_len >= rx_ring_bytes) {
        rx_ring_head = (rx_ring_head + 1) % rx_ring_bytes;
        rx_ring_len -= 1;
    }
    const tail = (rx_ring_head + rx_ring_len) % rx_ring_bytes;
    rx_ring[tail] = byte;
    rx_ring_len += 1;
}

fn rxRingPopInto(dst: [*]volatile u8, max_len: usize) usize {
    const n = @min(max_len, rx_ring_len);
    var i: usize = 0;
    while (i < n) : (i += 1) {
        dst[i] = rx_ring[(rx_ring_head + i) % rx_ring_bytes];
    }
    rx_ring_head = (rx_ring_head + n) % rx_ring_bytes;
    rx_ring_len -= n;
    return n;
}

fn completeEditedLine() void {
    var i: usize = 0;
    while (i < edit_line_len) : (i += 1) rxRingPush(edit_line[i]);
    rxRingPush('\n');
    edit_line_len = 0;
}

fn handleInputByte(byte: u8) void {
    if (byte == '\n' and input_prev_was_cr) {
        input_prev_was_cr = false;
        return;
    }

    if (byte == '\r' or byte == '\n') {
        input_prev_was_cr = byte == '\r';
        completeEditedLine();
        _ = sendBytes("\r\n");
        return;
    }
    input_prev_was_cr = false;

    if (byte == 0x08 or byte == 0x7f) {
        if (edit_line_len != 0) {
            edit_line_len -= 1;
            _ = sendBytes("\x08 \x08");
        }
        return;
    }

    if (edit_line_len >= edit_line.len) {
        _ = sendBytes("\x07");
        return;
    }

    edit_line[edit_line_len] = byte;
    edit_line_len += 1;
    const echo = [_]u8{byte};
    _ = sendBytes(echo[0..]);
}

fn fulfillPendingRead() void {
    if (pending_read_seq == 0 or rx_ring_len == 0) return;
    const n = rxRingPopInto(responsePayload(), pending_read_max_len);
    writeConsoleResponse(.read, pending_read_seq, .ok, @intCast(n), n, 0);
    console_session.last_completed_seq = pending_read_seq;
    pending_read_seq = 0;
    pending_read_max_len = 0;
}

fn resetInputState() void {
    rx_ring_head = 0;
    rx_ring_len = 0;
    edit_line_len = 0;
    input_prev_was_cr = false;
    pending_read_seq = 0;
    pending_read_max_len = 0;
}

fn copyVolatileToTx(src: [*]volatile u8, len: usize) bool {
    var out: [tx_buffer_bytes]u8 = undefined;
    var out_len: usize = 0;
    var prev_was_cr = false;
    var i: usize = 0;
    while (i < len) : (i += 1) {
        const b = src[i];
        if (b == '\n' and !prev_was_cr) {
            if (out_len == out.len) {
                if (!sendBytes(out[0..out_len])) return false;
                out_len = 0;
            }
            out[out_len] = '\r';
            out_len += 1;
        }
        if (out_len == out.len) {
            if (!sendBytes(out[0..out_len])) return false;
            out_len = 0;
        }
        out[out_len] = b;
        out_len += 1;
        prev_was_cr = b == '\r';
    }
    if (out_len != 0 and !sendBytes(out[0..out_len])) return false;
    return true;
}

fn handleConsoleConnectTransfer(transfer_id: u64) void {
    const request_paddr = acceptCapTransfer(transfer_id);
    if (request_paddr < 0x1000) {
        _ = userLog("VirtioConsole: accept cap transfer failed\n");
        return;
    }
    if (mapPage(console_request_va, request_paddr, false) != syscall_ok) return;
    const request = requestHeader();
    if (request.magic != console_protocol.request_magic or
        request.version != console_protocol.version or
        request.op != console_protocol.opcodeRaw(.connect) or
        request.request_seq == 0 or
        request.arg0 < 0x1000 or
        request.session_nonce == 0)
    {
        _ = userLog("VirtioConsole: invalid connect request\n");
        return;
    }
    if (mapPage(console_response_va, request.arg0, true) != syscall_ok) return;
    clearPage(console_response_va);
    console_session.active = true;
    console_session.request_paddr = request_paddr;
    console_session.response_paddr = request.arg0;
    console_session.reply_endpoint_id = if (installEndpoint(console_reply_endpoint_id, request.arg1) == syscall_ok) console_reply_endpoint_id else 0;
    console_session.session_nonce = request.session_nonce;
    console_session.last_completed_seq = 0;
    resetInputState();
    writeConsoleResponse(.connect, request.request_seq, .ok, 0, 0, 0);
    console_session.last_completed_seq = request.request_seq;
    _ = userLog("VirtioConsole: session connect ok\n");
}

fn handleConsoleRequest() void {
    if (!console_session.active) return;
    const request = requestHeader();
    if (request.magic != console_protocol.request_magic or request.version != console_protocol.version) return;
    const seq = request.request_seq;
    if (seq == 0 or seq <= console_session.last_completed_seq) return;
    if (request.session_nonce != console_session.session_nonce) return;

    if (request.op == console_protocol.opcodeRaw(.read)) {
        const max_len = @min(@as(usize, @intCast(request.length)), console_protocol.response_payload_bytes);
        if (max_len == 0) {
            writeConsoleResponse(.read, seq, .invalid, 0, 0, 0);
        } else if (rx_ring_len == 0) {
            if (pending_read_seq == 0 or pending_read_seq == seq) {
                pending_read_seq = seq;
                pending_read_max_len = max_len;
            } else {
                writeConsoleResponse(.read, seq, .again, 0, 0, 0);
                console_session.last_completed_seq = seq;
            }
            return;
        } else {
            const n = rxRingPopInto(responsePayload(), max_len);
            writeConsoleResponse(.read, seq, .ok, @intCast(n), n, 0);
        }
    } else if (request.op == console_protocol.opcodeRaw(.write)) {
        const len = @min(@as(usize, @intCast(request.length)), console_protocol.request_payload_bytes);
        if (copyVolatileToTx(requestPayload(), len)) {
            writeConsoleResponse(.write, seq, .ok, 0, len, 0);
        } else {
            writeConsoleResponse(.write, seq, .io_error, 0, 0, 0);
        }
    } else if (request.op == console_protocol.opcodeRaw(.get_attr)) {
        writeConsoleResponse(.get_attr, seq, .ok, 0, 120, 40);
    } else if (request.op == console_protocol.opcodeRaw(.set_attr)) {
        writeConsoleResponse(.set_attr, seq, .ok, 0, 0, 0);
    } else if (request.op == console_protocol.opcodeRaw(.connect)) {
        writeConsoleResponse(.connect, seq, .ok, 0, 0, 0);
    } else {
        writeConsoleResponse(.read, seq, .invalid, 0, 0, 0);
    }
    console_session.last_completed_seq = seq;
}

fn pollRxQueue() void {
    while (usedIdxPtr(&rx_queue).* != rx_queue.last_used_idx) {
        const used = usedRingPtr(&rx_queue)[@intCast(rx_queue.last_used_idx % queue_size)];
        rx_queue.last_used_idx +%= 1;
        const desc_index: usize = @intCast(used.id % queue_size);
        const len: usize = @intCast(if (used.len > rx_buffer_bytes) rx_buffer_bytes else used.len);
        const src: [*]const u8 = @ptrFromInt(rx_buffer_va + desc_index * user_vm.page_bytes);
        if (len != 0) {
            var i: usize = 0;
            while (i < len) : (i += 1) {
                handleInputByte(src[i]);
            }
        }
        fulfillPendingRead();
        queuePushAvail(&rx_queue, @intCast(desc_index));
    }
    if (queueNotify(rx_queue.notify_token, rx_queue.index) == syscall_ok) {
        mmioWriteU16(rx_queue.notify_addr, rx_queue.index);
    }
}

pub export fn _start() noreturn {
    _ = userLog("VirtioConsole: started\n");
    waitForBootResources();
    if (!reserveVirtioTargetVas()) {
        _ = userLog("VirtioConsole: reserve VA failed\n");
        while (true) _ = waitEvent(false, 1);
    }
    if (!initVirtio()) {
        _ = userLog("VirtioConsole: init failed\n");
        while (true) _ = waitEvent(false, 1);
    }
    _ = sendBytes("VirtioConsole: ready\r\n");
    writeCfgU64(console_bootstrap.driver_status_index, console_bootstrap.driver_status_ready);
    _ = userLog("VirtioConsole: ready\n");
    while (true) {
        pollRxQueue();
        fulfillPendingRead();
        const received = waitEvent(true, 1);
        if (received >= cap_transfer_id_min) handleConsoleConnectTransfer(received);
        handleConsoleRequest();
    }
}
