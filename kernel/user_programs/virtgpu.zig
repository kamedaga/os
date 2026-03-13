const syscall_log: u64 = 0x9;
const syscall_map_mmio: u64 = 0xB;
const syscall_queue_submit: u64 = 0xE;
const syscall_queue_notify: u64 = 0xF;
const syscall_untyped_reset: u64 = 0x12;
const syscall_untyped_alloc_map_pages: u64 = 0x13;

const syscall_ok: u64 = 0;
const queue_cap_device_gpu: u64 = 0;
const untyped_alloc_map_writable_flag: u64 = 1 << 0;
const untyped_alloc_map_drop_cap_after_map_flag: u64 = 1 << 1;
const untyped_alloc_map_contiguous_flag: u64 = 1 << 2;
const untyped_alloc_map_dma_ok_flag: u64 = 1 << 3;

const config_page_va: usize = 0x3C00_2000;
const common_page_va: usize = 0x2200_4000;
const notify_page_va: usize = 0x2200_5000;
const isr_page_va: usize = 0x2200_6000;
const device_page_va: usize = 0x2200_7000;
const queue_page0_va: usize = 0x2200_8000;
const queue_page1_va: usize = 0x2200_9000;
const control_page_va: usize = 0x2200_A000;
const mem_entries_page0_va: usize = 0x2200_B000;
const mem_entries_page1_va: usize = 0x2200_C000;
const cursor_queue_page0_va: usize = 0x2200_D000;
const cursor_queue_page1_va: usize = 0x2200_E000;
const backing_base_va: usize = 0x2240_0000;
const resource_backing_span: usize = 0x0040_0000;

const config_magic: u64 = 0x56475055; // "VGPU"
const control_request_offset: usize = 0x000;
const control_response_offset: usize = 0x800;

const common_device_feature_select: usize = 0x00;
const common_device_feature: usize = 0x04;
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
const feature_version_1: u32 = 1 << 0;

const queue_index_control: u16 = 0;
const queue_index_cursor: u16 = 1;
const queue_region_bytes: usize = queue_page1_va + 4096 - queue_page0_va;
const cursor_queue_region_bytes: usize = cursor_queue_page1_va + 4096 - cursor_queue_page0_va;
const queue_used_offset: usize = 4096;
const requested_queue_size: u16 = 8;
const max_backing_pages: usize = 512;
const max_alloc_chunk_pages: usize = 16;
const max_resources: usize = 8;
const init_alloc_page_count: u64 = 7;
const mem_entry_page_count: u64 = 2;
const mem_entries_region_bytes: usize = mem_entries_page1_va + 4096 - mem_entries_page0_va;
const wait_poll_limit: usize = 100_000;
const cursor_dim: usize = 64;
const warm_state_magic: u64 = 0x56475057; // "VGPW"
const state_canary_magic: u64 = 0x5653544154453031; // "VSTATE01"
const resource_canary_magic: u64 = 0x5652535243303031; // "VRSRC001"
const cfg_warm_state_index: usize = 11;
const cfg_queue_paddr0_index: usize = 12;
const cfg_queue_paddr1_index: usize = 13;
const cfg_control_page_paddr_index: usize = 14;
const cfg_mem_entries_paddr0_index: usize = 15;
const cfg_mem_entries_paddr1_index: usize = 16;
const cfg_cursor_queue_paddr0_index: usize = 17;
const cfg_cursor_queue_paddr1_index: usize = 18;
const cfg_queue_size_index: usize = 19;
const cfg_cursor_queue_size_index: usize = 20;
const cfg_control_submit_token_index: usize = 21;
const cfg_control_notify_token_index: usize = 22;
const cfg_cursor_submit_token_index: usize = 23;
const cfg_cursor_notify_token_index: usize = 24;

const desc_flag_next: u16 = 1 << 0;
const desc_flag_write: u16 = 1 << 1;

const virtio_gpu_cmd_resource_create_2d: u32 = 0x0101;
const virtio_gpu_cmd_set_scanout: u32 = 0x0103;
const virtio_gpu_cmd_resource_flush: u32 = 0x0104;
const virtio_gpu_cmd_transfer_to_host_2d: u32 = 0x0105;
const virtio_gpu_cmd_resource_attach_backing: u32 = 0x0106;
const virtio_gpu_cmd_update_cursor: u32 = 0x0300;
const virtio_gpu_cmd_move_cursor: u32 = 0x0301;
const virtio_gpu_resp_ok_nodata: u32 = 0x1100;
const virtio_gpu_format_b8g8r8a8_unorm: u32 = 1;
const virtio_gpu_format_b8g8r8x8_unorm: u32 = 2;

pub const Rect = extern struct {
    x: u32,
    y: u32,
    width: u32,
    height: u32,
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

const VirtioGpuCtrlHdr = extern struct {
    type: u32,
    flags: u32,
    fence_id: u64,
    ctx_id: u32,
    padding: u32,
};

const VirtioGpuResourceCreate2d = extern struct {
    hdr: VirtioGpuCtrlHdr,
    resource_id: u32,
    format: u32,
    width: u32,
    height: u32,
};

const VirtioGpuSetScanout = extern struct {
    hdr: VirtioGpuCtrlHdr,
    rect: Rect,
    scanout_id: u32,
    resource_id: u32,
};

const VirtioGpuResourceFlush = extern struct {
    hdr: VirtioGpuCtrlHdr,
    rect: Rect,
    resource_id: u32,
    padding: u32,
};

const VirtioGpuTransferToHost2d = extern struct {
    hdr: VirtioGpuCtrlHdr,
    rect: Rect,
    offset: u64,
    resource_id: u32,
    padding: u32,
};

const VirtioGpuResourceAttachBacking = extern struct {
    hdr: VirtioGpuCtrlHdr,
    resource_id: u32,
    nr_entries: u32,
};

const VirtioGpuMemEntry = extern struct {
    addr: u64,
    length: u32,
    padding: u32,
};

const VirtioGpuCursorPos = extern struct {
    scanout_id: u32,
    x: u32,
    y: u32,
    padding: u32,
};

const VirtioGpuUpdateCursor = extern struct {
    hdr: VirtioGpuCtrlHdr,
    pos: VirtioGpuCursorPos,
    resource_id: u32,
    hot_x: u32,
    hot_y: u32,
    padding: u32,
};

pub const Resource = struct {
    canary: u64 = resource_canary_magic,
    in_use: bool = false,
    ready: bool = false,
    slot_index: usize = 0,
    base_va: usize = backing_base_va,
    resource_id: u32 = 0,
    width: u32 = 0,
    height: u32 = 0,
    stride_bytes: u32 = 0,
    bytes_len: usize = 0,
    page_count: usize = 0,
    pixels: [*]volatile u32 = @ptrFromInt(backing_base_va),
};

pub const ResourceHandle = usize;

const State = struct {
    canary: u64 = state_canary_magic,
    init_attempted: bool = false,
    ready: bool = false,
    common_base: usize = 0,
    notify_base: usize = 0,
    notify_addr: usize = 0,
    cursor_notify_addr: usize = 0,
    notify_off_multiplier: usize = 0,
    queue_size: u16 = 0,
    used_idx_seen: u16 = 0,
    cursor_queue_size: u16 = 0,
    cursor_used_idx_seen: u16 = 0,
    queue_paddr0: u64 = 0,
    queue_paddr1: u64 = 0,
    cursor_queue_paddr0: u64 = 0,
    cursor_queue_paddr1: u64 = 0,
    control_page_paddr: u64 = 0,
    mem_entries_paddr0: u64 = 0,
    mem_entries_paddr1: u64 = 0,
    next_resource_id: u32 = 1,
    default_scanout_id: u32 = 0,
    cursor_resource: ?ResourceHandle = null,
    cursor_hot_x: u32 = 0,
    cursor_hot_y: u32 = 0,
    control_submit_token: u64 = 0,
    control_notify_token: u64 = 0,
    cursor_submit_token: u64 = 0,
    cursor_notify_token: u64 = 0,
};

var state: State = .{};
var resources: [max_resources]Resource = [_]Resource{.{}} ** max_resources;
var resource_backing_paddrs_scratch: [max_backing_pages]u64 = [_]u64{0} ** max_backing_pages;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
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

fn allocUntypedMapPages(base_va: u64, page_count: u64, writable: bool, out_paddr_list_va: u64, drop_cap_after_map: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_untyped_alloc_map_pages),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (@as(u64, if (writable) untyped_alloc_map_writable_flag else 0) |
            @as(u64, if (drop_cap_after_map) untyped_alloc_map_drop_cap_after_map_flag else 0) |
            untyped_alloc_map_contiguous_flag |
            untyped_alloc_map_dma_ok_flag),
          [arg3] "{rcx}" (out_paddr_list_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn resetUntyped(token: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_untyped_reset),
          [arg0] "{rdi}" (token),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn queueSubmit(token: u64, queue_index: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_queue_submit),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (queue_cap_device_gpu),
          [arg2] "{rdx}" (queue_index),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn queueNotify(token: u64, queue_index: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_queue_notify),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (queue_cap_device_gpu),
          [arg2] "{rdx}" (queue_index),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
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

fn mmioReadU32(addr: usize) u32 {
    const p: *volatile u32 = @ptrFromInt(addr);
    return p.*;
}

fn mmioWriteU32(addr: usize, value: u32) void {
    const p: *volatile u32 = @ptrFromInt(addr);
    p.* = value;
}

fn mmioWriteU64(addr: usize, value: u64) void {
    const p: *volatile u64 = @ptrFromInt(addr);
    p.* = value;
}

fn queueRegionPhys(offset: usize) u64 {
    if (offset < 4096) return state.queue_paddr0 + @as(u64, @intCast(offset));
    return state.queue_paddr1 + @as(u64, @intCast(offset - 4096));
}

fn cursorQueueRegionPhys(offset: usize) u64 {
    if (offset < 4096) return state.cursor_queue_paddr0 + @as(u64, @intCast(offset));
    return state.cursor_queue_paddr1 + @as(u64, @intCast(offset - 4096));
}

fn queueDescPtr(index: u16) *volatile VirtqDesc {
    const offset = @as(usize, index) * @sizeOf(VirtqDesc);
    return @ptrFromInt(queue_page0_va + offset);
}

fn queueAvailIdxPtr() *volatile u16 {
    return @ptrFromInt(queue_page0_va + @as(usize, state.queue_size) * @sizeOf(VirtqDesc) + 2);
}

fn queueAvailRingPtr() [*]volatile u16 {
    return @ptrFromInt(queue_page0_va + @as(usize, state.queue_size) * @sizeOf(VirtqDesc) + 4);
}

fn queueUsedIdxPtr() *volatile u16 {
    return @ptrFromInt(queue_page0_va + queue_used_offset + 2);
}

fn queueUsedRingPtr() [*]volatile VirtqUsedElem {
    return @ptrFromInt(queue_page0_va + queue_used_offset + 4);
}

fn cursorQueueDescPtr(index: u16) *volatile VirtqDesc {
    const offset = @as(usize, index) * @sizeOf(VirtqDesc);
    return @ptrFromInt(cursor_queue_page0_va + offset);
}

fn cursorQueueAvailIdxPtr() *volatile u16 {
    return @ptrFromInt(cursor_queue_page0_va + @as(usize, state.cursor_queue_size) * @sizeOf(VirtqDesc) + 2);
}

fn cursorQueueAvailRingPtr() [*]volatile u16 {
    return @ptrFromInt(cursor_queue_page0_va + @as(usize, state.cursor_queue_size) * @sizeOf(VirtqDesc) + 4);
}

fn cursorQueueUsedIdxPtr() *volatile u16 {
    return @ptrFromInt(cursor_queue_page0_va + queue_used_offset + 2);
}

fn cursorQueueUsedRingPtr() [*]volatile VirtqUsedElem {
    return @ptrFromInt(cursor_queue_page0_va + queue_used_offset + 4);
}

fn controlReqPaddr() u64 {
    return state.control_page_paddr + control_request_offset;
}

fn controlRespPaddr() u64 {
    return state.control_page_paddr + control_response_offset;
}

fn clearBytes(base_va: usize, len: usize) void {
    const bytes: [*]u8 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < len) : (i += 1) {
        bytes[i] = 0;
    }
}

fn controlHdr(command_type: u32) VirtioGpuCtrlHdr {
    return .{
        .type = command_type,
        .flags = 0,
        .fence_id = 0,
        .ctx_id = 0,
        .padding = 0,
    };
}

fn memoryBarrier() void {
    asm volatile ("" ::: .{ .memory = true });
}

fn logText(text: []const u8) void {
    _ = userLog(text);
}

fn stateLooksValid() bool {
    if (state.canary != state_canary_magic) return false;
    if (state.ready) {
        if (state.common_base < common_page_va or state.common_base >= common_page_va + 4096) return false;
        if (state.notify_base < notify_page_va or state.notify_base >= notify_page_va + 4096) return false;
        if (state.notify_addr < notify_page_va or state.notify_addr >= notify_page_va + 4096) return false;
        if (state.cursor_notify_addr < notify_page_va or state.cursor_notify_addr >= notify_page_va + 4096) return false;
        if (state.queue_size < 4 or state.queue_size > requested_queue_size) return false;
        if (state.cursor_queue_size < 2 or state.cursor_queue_size > requested_queue_size) return false;
    }
    return true;
}

fn resourceLooksValid(resource: *const Resource) bool {
    if (resource.canary != resource_canary_magic) return false;
    if (resource.slot_index >= max_resources) return false;
    const expected_base = resourceBaseVa(resource.slot_index);
    const pixels_addr = @intFromPtr(resource.pixels);
    if (resource.base_va != expected_base) return false;
    if (pixels_addr != expected_base) return false;
    if (resource.page_count > max_backing_pages) return false;
    if (resource.bytes_len > resource_backing_span) return false;
    return true;
}

fn requireStateHealthy(context: []const u8) bool {
    if (stateLooksValid()) return true;
    logText(context);
    logText(": virtgpu state corrupt\n");
    return false;
}

fn isSyscallError(value: u64) bool {
    return value != 0 and value <= 13;
}

fn queuePushAvail(head_desc: u16) void {
    const avail_idx_ptr = queueAvailIdxPtr();
    const avail_idx = avail_idx_ptr.*;
    const slot: usize = @intCast(avail_idx % state.queue_size);
    queueAvailRingPtr()[slot] = head_desc;
    memoryBarrier();
    avail_idx_ptr.* = avail_idx +% 1;
    memoryBarrier();
}

fn cursorQueuePushAvail(head_desc: u16) void {
    const avail_idx_ptr = cursorQueueAvailIdxPtr();
    const avail_idx = avail_idx_ptr.*;
    const slot: usize = @intCast(avail_idx % state.cursor_queue_size);
    cursorQueueAvailRingPtr()[slot] = head_desc;
    memoryBarrier();
    avail_idx_ptr.* = avail_idx +% 1;
    memoryBarrier();
}

fn submitCommand(req_len: usize, extra_len: usize, expected_resp_type: u32) bool {
    if (!requireStateHealthy("submitCommand")) return false;
    if (!state.ready) return false;

    clearBytes(control_page_va + control_response_offset, @sizeOf(VirtioGpuCtrlHdr));

    var desc_count: u16 = 0;
    {
        const desc = queueDescPtr(desc_count);
        desc.* = .{
            .addr = controlReqPaddr(),
            .len = @intCast(req_len),
            .flags = 0,
            .next = 0,
        };
        desc_count += 1;
    }

    if (extra_len != 0) {
        var remaining = extra_len;
        const first_len: usize = if (remaining > 4096) 4096 else remaining;
        if (first_len != 0) {
            queueDescPtr(desc_count - 1).flags = desc_flag_next;
            queueDescPtr(desc_count - 1).next = desc_count;
            const desc = queueDescPtr(desc_count);
            desc.* = .{
                .addr = state.mem_entries_paddr0,
                .len = @intCast(first_len),
                .flags = 0,
                .next = 0,
            };
            desc_count += 1;
            remaining -= first_len;
        }
        if (remaining != 0) {
            queueDescPtr(desc_count - 1).flags = desc_flag_next;
            queueDescPtr(desc_count - 1).next = desc_count;
            const desc = queueDescPtr(desc_count);
            desc.* = .{
                .addr = state.mem_entries_paddr1,
                .len = @intCast(remaining),
                .flags = 0,
                .next = 0,
            };
            desc_count += 1;
        }
    }

    queueDescPtr(desc_count - 1).flags = desc_flag_next;
    queueDescPtr(desc_count - 1).next = desc_count;
    {
        const resp_desc = queueDescPtr(desc_count);
        resp_desc.* = .{
            .addr = controlRespPaddr(),
            .len = @sizeOf(VirtioGpuCtrlHdr),
            .flags = desc_flag_write,
            .next = 0,
        };
    }

    queuePushAvail(0);
    if (queueSubmit(state.control_submit_token, queue_index_control) != syscall_ok) {
        logText("Compositor: queue control submit cap denied\n");
        return false;
    }
    if (queueNotify(state.control_notify_token, queue_index_control) != syscall_ok) {
        logText("Compositor: queue control notify cap denied\n");
        return false;
    }
    mmioWriteU16(state.notify_addr, queue_index_control);

    var spin: usize = 0;
    while (spin < wait_poll_limit) : (spin += 1) {
        if (queueUsedIdxPtr().* != state.used_idx_seen) {
            const used = queueUsedRingPtr()[@as(usize, @intCast(state.used_idx_seen % state.queue_size))];
            state.used_idx_seen +%= 1;
            if (used.id != 0) return false;
            const resp: *const VirtioGpuCtrlHdr = @ptrFromInt(control_page_va + control_response_offset);
            return resp.type == expected_resp_type;
        }
        asm volatile ("pause");
    }

    logText("Compositor: virtgpu command timeout\n");
    return false;
}

fn submitCursorCommand(req_len: usize) bool {
    if (!requireStateHealthy("submitCursorCommand")) return false;
    if (!state.ready or state.cursor_queue_size < 2) return false;

    var desc = cursorQueueDescPtr(0);
    desc.* = .{
        .addr = controlReqPaddr(),
        .len = @intCast(req_len),
        .flags = desc_flag_next,
        .next = 1,
    };
    desc = cursorQueueDescPtr(1);
    desc.* = .{
        .addr = controlRespPaddr(),
        .len = @sizeOf(VirtioGpuCtrlHdr),
        .flags = desc_flag_write,
        .next = 0,
    };

    cursorQueuePushAvail(0);
    if (queueSubmit(state.cursor_submit_token, queue_index_cursor) != syscall_ok) {
        logText("Compositor: queue cursor submit cap denied\n");
        return false;
    }
    if (queueNotify(state.cursor_notify_token, queue_index_cursor) != syscall_ok) {
        logText("Compositor: queue cursor notify cap denied\n");
        return false;
    }
    mmioWriteU16(state.cursor_notify_addr, queue_index_cursor);

    var spin: usize = 0;
    while (spin < wait_poll_limit) : (spin += 1) {
        if (cursorQueueUsedIdxPtr().* != state.cursor_used_idx_seen) {
            const used = cursorQueueUsedRingPtr()[@as(usize, @intCast(state.cursor_used_idx_seen % state.cursor_queue_size))];
            state.cursor_used_idx_seen +%= 1;
            return used.id == 0;
        }
        asm volatile ("pause");
    }

    logText("Compositor: virtgpu cursor command timeout\n");
    return false;
}

fn initQueue() bool {
    mmioWriteU8(state.common_base + common_device_status, 0);
    mmioWriteU8(state.common_base + common_device_status, status_acknowledge | status_driver);
    mmioWriteU32(state.common_base + common_device_feature_select, 1);
    const device_features_hi = mmioReadU32(state.common_base + common_device_feature);
    if ((device_features_hi & feature_version_1) == 0) {
        logText("Compositor: virtgpu VERSION_1 missing\n");
        return false;
    }
    mmioWriteU32(state.common_base + common_driver_feature_select, 0);
    mmioWriteU32(state.common_base + common_driver_feature, 0);
    mmioWriteU32(state.common_base + common_driver_feature_select, 1);
    mmioWriteU32(state.common_base + common_driver_feature, feature_version_1);
    mmioWriteU8(state.common_base + common_device_status, status_acknowledge | status_driver | status_features_ok);
    if ((mmioReadU8(state.common_base + common_device_status) & status_features_ok) == 0) {
        logText("Compositor: virtgpu FEATURES_OK rejected\n");
        return false;
    }
    mmioWriteU16(state.common_base + common_queue_select, queue_index_control);
    const control_device_queue_size = mmioReadU16(state.common_base + common_queue_size);
    if (control_device_queue_size < 4) return false;
    state.queue_size = if (control_device_queue_size < requested_queue_size) control_device_queue_size else requested_queue_size;
    const control_queue_notify_off = mmioReadU16(state.common_base + common_queue_notify_off);
    mmioWriteU16(state.common_base + common_queue_size, state.queue_size);
    mmioWriteU64(state.common_base + common_queue_desc, queueRegionPhys(0));
    mmioWriteU64(state.common_base + common_queue_avail, queueRegionPhys(@as(usize, state.queue_size) * @sizeOf(VirtqDesc)));
    mmioWriteU64(state.common_base + common_queue_used, queueRegionPhys(queue_used_offset));
    mmioWriteU16(state.common_base + common_queue_enable, 1);
    state.notify_addr = state.notify_base + @as(usize, control_queue_notify_off) * state.notify_off_multiplier;
    state.used_idx_seen = 0;
    mmioWriteU16(state.common_base + common_queue_select, queue_index_cursor);
    const cursor_device_queue_size = mmioReadU16(state.common_base + common_queue_size);
    if (cursor_device_queue_size < 2) return false;
    state.cursor_queue_size = if (cursor_device_queue_size < requested_queue_size) cursor_device_queue_size else requested_queue_size;
    const cursor_queue_notify_off = mmioReadU16(state.common_base + common_queue_notify_off);
    mmioWriteU16(state.common_base + common_queue_size, state.cursor_queue_size);
    mmioWriteU64(state.common_base + common_queue_desc, cursorQueueRegionPhys(0));
    mmioWriteU64(state.common_base + common_queue_avail, cursorQueueRegionPhys(@as(usize, state.cursor_queue_size) * @sizeOf(VirtqDesc)));
    mmioWriteU64(state.common_base + common_queue_used, cursorQueueRegionPhys(queue_used_offset));
    mmioWriteU16(state.common_base + common_queue_enable, 1);
    state.cursor_notify_addr = state.notify_base + @as(usize, cursor_queue_notify_off) * state.notify_off_multiplier;
    state.cursor_used_idx_seen = 0;
    mmioWriteU8(state.common_base + common_device_status, status_acknowledge | status_driver | status_features_ok | status_driver_ok);
    return true;
}

fn loadPreallocatedQueuePaddrs() bool {
    state.queue_paddr0 = readCfgU64(cfg_queue_paddr0_index);
    state.queue_paddr1 = readCfgU64(cfg_queue_paddr1_index);
    state.control_page_paddr = readCfgU64(cfg_control_page_paddr_index);
    state.mem_entries_paddr0 = readCfgU64(cfg_mem_entries_paddr0_index);
    state.mem_entries_paddr1 = readCfgU64(cfg_mem_entries_paddr1_index);
    state.cursor_queue_paddr0 = readCfgU64(cfg_cursor_queue_paddr0_index);
    state.cursor_queue_paddr1 = readCfgU64(cfg_cursor_queue_paddr1_index);
    return state.queue_paddr0 >= 0x1000 and
        state.queue_paddr1 >= 0x1000 and
        state.control_page_paddr >= 0x1000 and
        state.mem_entries_paddr0 >= 0x1000 and
        state.mem_entries_paddr1 >= 0x1000 and
        state.cursor_queue_paddr0 >= 0x1000 and
        state.cursor_queue_paddr1 >= 0x1000;
}

fn recordWarmState() void {
    writeCfgU64(cfg_warm_state_index, warm_state_magic);
    writeCfgU64(cfg_queue_paddr0_index, state.queue_paddr0);
    writeCfgU64(cfg_queue_paddr1_index, state.queue_paddr1);
    writeCfgU64(cfg_control_page_paddr_index, state.control_page_paddr);
    writeCfgU64(cfg_mem_entries_paddr0_index, state.mem_entries_paddr0);
    writeCfgU64(cfg_mem_entries_paddr1_index, state.mem_entries_paddr1);
    writeCfgU64(cfg_cursor_queue_paddr0_index, state.cursor_queue_paddr0);
    writeCfgU64(cfg_cursor_queue_paddr1_index, state.cursor_queue_paddr1);
    writeCfgU64(cfg_queue_size_index, state.queue_size);
    writeCfgU64(cfg_cursor_queue_size_index, state.cursor_queue_size);
}

fn tryRestoreWarmState() bool {
    if (readCfgU64(cfg_warm_state_index) != warm_state_magic) return false;

    const common_page_paddr = readCfgU64(1);
    const notify_page_paddr = readCfgU64(2);
    if (common_page_paddr < 0x1000 or notify_page_paddr < 0x1000) return false;
    const common_off_raw = readCfgU64(5);
    const notify_off_raw = readCfgU64(6);
    if (common_off_raw >= 4096 or notify_off_raw >= 4096) {
        logText("Compositor: virtgpu warm restore rejected (bad cfg offset)\n");
        return false;
    }

    state.common_base = common_page_va + @as(usize, @intCast(common_off_raw));
    state.notify_base = notify_page_va + @as(usize, @intCast(notify_off_raw));
    state.notify_off_multiplier = @as(usize, @intCast(readCfgU64(9)));
    state.default_scanout_id = @intCast(readCfgU64(10));
    state.control_submit_token = readCfgU64(cfg_control_submit_token_index);
    state.control_notify_token = readCfgU64(cfg_control_notify_token_index);
    state.cursor_submit_token = readCfgU64(cfg_cursor_submit_token_index);
    state.cursor_notify_token = readCfgU64(cfg_cursor_notify_token_index);
    if (state.notify_off_multiplier == 0) return false;

    if (state.control_submit_token == 0 or state.control_notify_token == 0 or state.cursor_submit_token == 0 or state.cursor_notify_token == 0) return false;
    if (!loadPreallocatedQueuePaddrs()) return false;
    state.queue_size = @intCast(readCfgU64(cfg_queue_size_index));
    state.cursor_queue_size = @intCast(readCfgU64(cfg_cursor_queue_size_index));
    if (state.queue_size < 4 or state.cursor_queue_size < 2) return false;
    if (mapMmioPage(common_page_va, common_page_paddr, true) != syscall_ok) return false;
    if (mapMmioPage(notify_page_va, notify_page_paddr, true) != syscall_ok) return false;

    mmioWriteU16(state.common_base + common_queue_select, queue_index_control);
    state.notify_addr = state.notify_base +
        @as(usize, mmioReadU16(state.common_base + common_queue_notify_off)) * state.notify_off_multiplier;
    state.used_idx_seen = queueUsedIdxPtr().*;

    mmioWriteU16(state.common_base + common_queue_select, queue_index_cursor);
    state.cursor_notify_addr = state.notify_base +
        @as(usize, mmioReadU16(state.common_base + common_queue_notify_off)) * state.notify_off_multiplier;
    state.cursor_used_idx_seen = cursorQueueUsedIdxPtr().*;

    state.init_attempted = true;
    state.ready = true;
    return true;
}

fn initInternal(prewarm: bool) bool {
    if (!requireStateHealthy("initInternal-pre")) return false;
    if (state.ready) return true;
    if (state.init_attempted) return false;
    state.init_attempted = true;
    logText(if (prewarm) "BootLogConsole: virtgpu prewarm start\n" else "Compositor: virtgpu init start\n");
    if (readCfgU64(0) != config_magic) return false;

    const common_page_paddr = readCfgU64(1);
    const notify_page_paddr = readCfgU64(2);
    const _isr_page_paddr = readCfgU64(3);
    const _device_page_paddr = readCfgU64(4);
    const common_off_raw = readCfgU64(5);
    const notify_off_raw = readCfgU64(6);
    if (common_off_raw >= 4096 or notify_off_raw >= 4096) {
        logText("Compositor: virtgpu init rejected (bad cfg offset)\n");
        return false;
    }
    const common_off: usize = @intCast(common_off_raw);
    const notify_off: usize = @intCast(notify_off_raw);
    const _isr_off: usize = @intCast(readCfgU64(7));
    const _device_off: usize = @intCast(readCfgU64(8));
    const notify_off_multiplier: usize = @intCast(readCfgU64(9));
    state.default_scanout_id = @intCast(readCfgU64(10));
    state.control_submit_token = readCfgU64(cfg_control_submit_token_index);
    state.control_notify_token = readCfgU64(cfg_control_notify_token_index);
    state.cursor_submit_token = readCfgU64(cfg_cursor_submit_token_index);
    state.cursor_notify_token = readCfgU64(cfg_cursor_notify_token_index);
    if (common_page_paddr < 0x1000 or notify_page_paddr < 0x1000) return false;

    state.common_base = common_page_va + common_off;
    state.notify_base = notify_page_va + notify_off;
    state.notify_off_multiplier = notify_off_multiplier;
    _ = _isr_page_paddr;
    _ = _device_page_paddr;
    _ = _isr_off;
    _ = _device_off;
    if (notify_off_multiplier == 0) return false;
    if (state.control_submit_token == 0 or state.control_notify_token == 0 or state.cursor_submit_token == 0 or state.cursor_notify_token == 0) return false;
    // Queue/control backing pages are process-local mappings. Reusing physical
    // addresses recorded by a different process corrupts the virtqueue state.
    if (mapMmioPage(common_page_va, common_page_paddr, true) != syscall_ok) return false;
    if (mapMmioPage(notify_page_va, notify_page_paddr, true) != syscall_ok) return false;

    var init_paddrs: [init_alloc_page_count]u64 = [_]u64{0} ** init_alloc_page_count;
    if (allocUntypedMapPages(queue_page0_va, init_alloc_page_count, true, @intFromPtr(&init_paddrs), false) != syscall_ok) return false;
    inline for (0..init_alloc_page_count) |i| {
        if (init_paddrs[i] < 0x1000) return false;
    }
    state.queue_paddr0 = init_paddrs[0];
    state.queue_paddr1 = init_paddrs[1];
    state.control_page_paddr = init_paddrs[2];
    state.mem_entries_paddr0 = init_paddrs[3];
    state.mem_entries_paddr1 = init_paddrs[4];
    state.cursor_queue_paddr0 = init_paddrs[5];
    state.cursor_queue_paddr1 = init_paddrs[6];
    clearBytes(queue_page0_va, queue_region_bytes);
    clearBytes(cursor_queue_page0_va, cursor_queue_region_bytes);
    clearBytes(control_page_va, 4096);
    clearBytes(mem_entries_page0_va, mem_entries_region_bytes);
    if (!initQueue()) return false;

    state.ready = true;
    if (!requireStateHealthy("initInternal-post")) return false;
    recordWarmState();
    logText(if (prewarm) "BootLogConsole: virtgpu prewarm ready\n" else "Compositor: virtgpu queue ready\n");
    return true;
}

pub fn virtgpu_init() bool {
    return initInternal(false);
}

pub fn virtgpu_prewarm() bool {
    return initInternal(true);
}

fn resourceBaseVa(slot_index: usize) usize {
    return backing_base_va + slot_index * resource_backing_span;
}

fn resourcePixelsLooksValid(resource: *const Resource) bool {
    return resourceLooksValid(resource);
}

fn zeroResource(resource: *Resource, slot_index: usize) void {
    resource.* = .{};
    resource.canary = resource_canary_magic;
    resource.slot_index = slot_index;
    resource.base_va = resourceBaseVa(slot_index);
    resource.pixels = @ptrFromInt(resource.base_va);
}

fn getResource(handle: ResourceHandle) ?*Resource {
    if (!requireStateHealthy("getResource")) return null;
    if (handle >= resources.len) return null;
    const resource = &resources[handle];
    if (!resource.in_use or !resource.ready) return null;
    if (!resourcePixelsLooksValid(resource)) {
        logText("Compositor: virtgpu resource pointer rejected\n");
        return null;
    }
    return resource;
}

fn allocResourceSlot() ?struct { resource: *Resource, slot_index: usize } {
    var slot_index: usize = 0;
    while (slot_index < max_resources) : (slot_index += 1) {
        if (!resources[slot_index].in_use) {
            zeroResource(&resources[slot_index], slot_index);
            resources[slot_index].in_use = true;
            return .{
                .resource = &resources[slot_index],
                .slot_index = slot_index,
            };
        }
    }
    return null;
}

fn createResourceWithFormat(width: usize, height: usize, format: u32) ?ResourceHandle {
    if (!requireStateHealthy("createResourceWithFormat")) return null;
    if (!state.ready and !virtgpu_init()) return null;
    if (width == 0 or height == 0) return null;

    const allocation = allocResourceSlot() orelse return null;
    const resource = allocation.resource;
    const backing_paddrs = resource_backing_paddrs_scratch[0..];
    const stride_bytes = width * 4;
    const total_bytes = stride_bytes * height;
    const page_count = (total_bytes + 4095) / 4096;
    if (page_count == 0 or page_count > max_backing_pages) {
        zeroResource(resource, allocation.slot_index);
        return null;
    }
    if (page_count >= 128) {
        logText("Compositor: create_fb backing alloc begin\n");
    }

    var page_index: usize = 0;
    while (page_index < page_count) {
        const remaining = page_count - page_index;
        const chunk_pages: usize = if (remaining > max_alloc_chunk_pages) max_alloc_chunk_pages else remaining;
        const chunk_base_va = resource.base_va + page_index * 4096;
        if (allocUntypedMapPages(
            @intCast(chunk_base_va),
            @intCast(chunk_pages),
            true,
            @intFromPtr(&backing_paddrs[page_index]),
            true,
        ) != syscall_ok) {
            zeroResource(resource, allocation.slot_index);
            return null;
        }
        var i: usize = 0;
        while (i < chunk_pages) : (i += 1) {
            if (backing_paddrs[page_index + i] < 0x1000) {
                zeroResource(resource, allocation.slot_index);
                return null;
            }
        }
        page_index += chunk_pages;
    }
    if (page_count >= 128) {
        logText("Compositor: create_fb backing alloc done\n");
    }

    resource.resource_id = state.next_resource_id;
    state.next_resource_id += 1;
    resource.width = @intCast(width);
    resource.height = @intCast(height);
    resource.stride_bytes = @intCast(stride_bytes);
    resource.bytes_len = total_bytes;
    resource.page_count = page_count;
    resource.pixels = @ptrFromInt(resource.base_va);

    {
        const req: *VirtioGpuResourceCreate2d = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_resource_create_2d),
            .resource_id = resource.resource_id,
            .format = format,
            .width = @intCast(width),
            .height = @intCast(height),
        };
        if (!submitCommand(@sizeOf(VirtioGpuResourceCreate2d), 0, virtio_gpu_resp_ok_nodata)) {
            logText("Compositor: virtgpu resource_create failed\n");
            zeroResource(resource, allocation.slot_index);
            return null;
        }
    }
    if (page_count >= 128) {
        logText("Compositor: create_fb resource_create done\n");
    }

    {
        clearBytes(mem_entries_page0_va, mem_entries_region_bytes);
        var i: usize = 0;
        while (i < page_count) : (i += 1) {
            const entry: *VirtioGpuMemEntry = @ptrFromInt(mem_entries_page0_va + i * @sizeOf(VirtioGpuMemEntry));
            entry.* = .{
                .addr = backing_paddrs[i],
                .length = 4096,
                .padding = 0,
            };
        }

        const req: *VirtioGpuResourceAttachBacking = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_resource_attach_backing),
            .resource_id = resource.resource_id,
            .nr_entries = @intCast(page_count),
        };
        if (!submitCommand(
            @sizeOf(VirtioGpuResourceAttachBacking),
            page_count * @sizeOf(VirtioGpuMemEntry),
            virtio_gpu_resp_ok_nodata,
        )) {
            logText("Compositor: virtgpu attach_backing failed\n");
            zeroResource(resource, allocation.slot_index);
            return null;
        }
    }
    if (page_count >= 128) {
        logText("Compositor: create_fb attach_backing done\n");
    }

    resource.ready = true;
    return allocation.slot_index;
}

pub fn virtgpu_create_resource(width: usize, height: usize) ?ResourceHandle {
    return createResourceWithFormat(width, height, virtio_gpu_format_b8g8r8x8_unorm);
}

pub fn virtgpu_create_resource_from_single_page(
    width: usize,
    height: usize,
    backing_paddr: u64,
    pixels_va: usize,
) ?ResourceHandle {
    if (!state.ready and !virtgpu_init()) return null;
    if (width == 0 or height == 0) return null;
    if (backing_paddr < 0x1000) return null;

    const stride_bytes = width * 4;
    const total_bytes = stride_bytes * height;
    if (total_bytes == 0 or total_bytes > 4096) return null;

    const allocation = allocResourceSlot() orelse return null;
    const resource = allocation.resource;
    resource.resource_id = state.next_resource_id;
    state.next_resource_id += 1;
    resource.width = @intCast(width);
    resource.height = @intCast(height);
    resource.stride_bytes = @intCast(stride_bytes);
    resource.bytes_len = total_bytes;
    resource.page_count = 1;
    resource.pixels = @ptrFromInt(pixels_va);

    {
        const req: *VirtioGpuResourceCreate2d = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_resource_create_2d),
            .resource_id = resource.resource_id,
            .format = virtio_gpu_format_b8g8r8x8_unorm,
            .width = @intCast(width),
            .height = @intCast(height),
        };
        if (!submitCommand(@sizeOf(VirtioGpuResourceCreate2d), 0, virtio_gpu_resp_ok_nodata)) {
            logText("Compositor: virtgpu resource_create failed\n");
            zeroResource(resource, allocation.slot_index);
            return null;
        }
    }

    {
        clearBytes(mem_entries_page0_va, mem_entries_region_bytes);
        const entry: *VirtioGpuMemEntry = @ptrFromInt(mem_entries_page0_va);
        entry.* = .{
            .addr = backing_paddr,
            .length = 4096,
            .padding = 0,
        };

        const req: *VirtioGpuResourceAttachBacking = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_resource_attach_backing),
            .resource_id = resource.resource_id,
            .nr_entries = 1,
        };
        if (!submitCommand(
            @sizeOf(VirtioGpuResourceAttachBacking),
            @sizeOf(VirtioGpuMemEntry),
            virtio_gpu_resp_ok_nodata,
        )) {
            logText("Compositor: virtgpu attach_backing failed\n");
            zeroResource(resource, allocation.slot_index);
            return null;
        }
    }

    resource.ready = true;
    return allocation.slot_index;
}

pub fn virtgpu_create_fb(width: usize, height: usize) ?ResourceHandle {
    return virtgpu_create_resource(width, height);
}

pub fn virtgpu_set_scanout(handle: ResourceHandle) bool {
    if (!requireStateHealthy("virtgpu_set_scanout")) return false;
    const resource = getResource(handle) orelse return false;
    if (!state.ready or !resource.ready) return false;
    const req: *VirtioGpuSetScanout = @ptrFromInt(control_page_va + control_request_offset);
    req.* = .{
        .hdr = controlHdr(virtio_gpu_cmd_set_scanout),
        .rect = .{
            .x = 0,
            .y = 0,
            .width = resource.width,
            .height = resource.height,
        },
        .scanout_id = state.default_scanout_id,
        .resource_id = resource.resource_id,
    };
    const ok = submitCommand(@sizeOf(VirtioGpuSetScanout), 0, virtio_gpu_resp_ok_nodata);
    if (!ok) logText("Compositor: virtgpu set_scanout command failed\n");
    return ok;
}

pub fn virtgpu_transfer(handle: ResourceHandle, rect: Rect) bool {
    if (!requireStateHealthy("virtgpu_transfer")) return false;
    const resource = getResource(handle) orelse return false;
    if (!state.ready or !resource.ready) return false;
    if (rect.x >= resource.width or rect.y >= resource.height) return true;

    var clipped = rect;
    if (clipped.x + clipped.width > resource.width) clipped.width = resource.width - clipped.x;
    if (clipped.y + clipped.height > resource.height) clipped.height = resource.height - clipped.y;
    if (clipped.width == 0 or clipped.height == 0) return true;

    const offset = @as(u64, clipped.y) * @as(u64, resource.stride_bytes) + @as(u64, clipped.x) * 4;
    const req: *VirtioGpuTransferToHost2d = @ptrFromInt(control_page_va + control_request_offset);
    req.* = .{
        .hdr = controlHdr(virtio_gpu_cmd_transfer_to_host_2d),
        .rect = clipped,
        .offset = offset,
        .resource_id = resource.resource_id,
        .padding = 0,
    };
    return submitCommand(@sizeOf(VirtioGpuTransferToHost2d), 0, virtio_gpu_resp_ok_nodata);
}

pub fn virtgpu_flush_rect(handle: ResourceHandle, rect: Rect) bool {
    if (!requireStateHealthy("virtgpu_flush_rect")) return false;
    const resource = getResource(handle) orelse return false;
    if (!state.ready or !resource.ready) return false;
    if (rect.x >= resource.width or rect.y >= resource.height) return true;
    var clipped = rect;
    if (clipped.x + clipped.width > resource.width) clipped.width = resource.width - clipped.x;
    if (clipped.y + clipped.height > resource.height) clipped.height = resource.height - clipped.y;
    if (clipped.width == 0 or clipped.height == 0) return true;
    const req: *VirtioGpuResourceFlush = @ptrFromInt(control_page_va + control_request_offset);
    req.* = .{
        .hdr = controlHdr(virtio_gpu_cmd_resource_flush),
        .rect = clipped,
        .resource_id = resource.resource_id,
        .padding = 0,
    };
    return submitCommand(@sizeOf(VirtioGpuResourceFlush), 0, virtio_gpu_resp_ok_nodata);
}

pub fn virtgpu_flush(handle: ResourceHandle) bool {
    const resource = getResource(handle) orelse return false;
    return virtgpu_flush_rect(handle, .{
        .x = 0,
        .y = 0,
        .width = resource.width,
        .height = resource.height,
    });
}

pub fn virtgpu_cursor_resource() ?ResourceHandle {
    if (!requireStateHealthy("virtgpu_cursor_resource")) return null;
    if (!state.ready and !virtgpu_init()) return null;
    if (state.cursor_resource) |resource_handle| {
        const resource = getResource(resource_handle) orelse {
            state.cursor_resource = null;
            return null;
        };
        if (resource.ready) return resource_handle;
    }
    const resource = createResourceWithFormat(cursor_dim, cursor_dim, virtio_gpu_format_b8g8r8a8_unorm) orelse return null;
    state.cursor_resource = resource;
    return resource;
}

pub fn virtgpu_update_cursor(handle: ResourceHandle, hot_x: u32, hot_y: u32, x: i32, y: i32) bool {
    if (!requireStateHealthy("virtgpu_update_cursor")) return false;
    const resource = getResource(handle) orelse return false;
    if (!state.ready or !resource.ready) return false;
    state.cursor_hot_x = hot_x;
    state.cursor_hot_y = hot_y;
    const req: *VirtioGpuUpdateCursor = @ptrFromInt(control_page_va + control_request_offset);
    req.* = .{
        .hdr = controlHdr(virtio_gpu_cmd_update_cursor),
        .pos = .{
            .scanout_id = state.default_scanout_id,
            .x = @intCast(if (x < 0) 0 else x),
            .y = @intCast(if (y < 0) 0 else y),
            .padding = 0,
        },
        .resource_id = resource.resource_id,
        .hot_x = hot_x,
        .hot_y = hot_y,
        .padding = 0,
    };
    return submitCursorCommand(@sizeOf(VirtioGpuUpdateCursor));
}

pub fn virtgpu_move_cursor(x: i32, y: i32) bool {
    if (!requireStateHealthy("virtgpu_move_cursor")) return false;
    const handle = state.cursor_resource orelse return false;
    const resource = getResource(handle) orelse return false;
    if (!resource.ready) return false;
    const req: *VirtioGpuUpdateCursor = @ptrFromInt(control_page_va + control_request_offset);
    req.* = .{
        .hdr = controlHdr(virtio_gpu_cmd_move_cursor),
        .pos = .{
            .scanout_id = state.default_scanout_id,
            .x = @intCast(if (x < 0) 0 else x),
            .y = @intCast(if (y < 0) 0 else y),
            .padding = 0,
        },
        .resource_id = resource.resource_id,
        .hot_x = state.cursor_hot_x,
        .hot_y = state.cursor_hot_y,
        .padding = 0,
    };
    return submitCursorCommand(@sizeOf(VirtioGpuUpdateCursor));
}

pub fn virtgpu_pixels(handle: ResourceHandle) ?[*]volatile u32 {
    if (!requireStateHealthy("virtgpu_pixels")) return null;
    const resource = getResource(handle) orelse return null;
    return resource.pixels;
}

pub fn virtgpu_dimensions(handle: ResourceHandle) ?struct { width: u32, height: u32 } {
    if (!requireStateHealthy("virtgpu_dimensions")) return null;
    const resource = getResource(handle) orelse return null;
    return .{ .width = resource.width, .height = resource.height };
}

pub fn logInitFailureOnce() void {
    _ = userLog("Compositor: virtgpu unavailable, fallback to framebuffer\n");
}
