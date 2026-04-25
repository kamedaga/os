const std = @import("std");
const cap_transfer_abi = @import("support_root").cap_transfer_abi;
const gpu_bootstrap = @import("support_root").gpu_bootstrap_abi;
const gpu_protocol = @import("support_root").gpu_protocol;
const queue_abi = @import("support_root").queue_abi;

const syscall_map_page: u64 = 0x2;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_map_mmio: u64 = 0xB;
const syscall_wait_event: u64 = 0x17;
const syscall_log: u64 = 0x9;
const syscall_queue_submit: u64 = 0xE;
const syscall_queue_notify: u64 = 0xF;
const syscall_iommu_authorize: u64 = queue_abi.syscall_iommu_authorize;
const syscall_command_authorize: u64 = queue_abi.syscall_command_authorize;
const syscall_dma_map_create: u64 = queue_abi.syscall_dma_map_create;
const syscall_dma_map_set_state: u64 = queue_abi.syscall_dma_map_set_state;
const syscall_dma_map_release: u64 = queue_abi.syscall_dma_map_release;

const syscall_ok: u64 = 0;

const config_page_va: usize = 0x3C00_2000;
const common_page_va: usize = 0x2100_4000;
const notify_page_va: usize = 0x2100_5000;
const isr_page_va: usize = 0x2100_6000;
const device_page_va: usize = 0x2100_7000;
const queue_page0_va: usize = 0x2100_8000;
const queue_page1_va: usize = 0x2100_9000;
const control_page_va: usize = 0x2100_A000;
const mem_entries_page0_va: usize = 0x2100_B000;
const mem_entries_page1_va: usize = 0x2100_C000;
const backing_base_va: usize = 0x2140_0000;
const gpu_request_page_va: usize = 0x3C01_0000;
const gpu_response_page_va: usize = 0x3C01_1000;

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

const feature_virgl: u32 = 1 << 0;
const feature_version_1: u32 = 1 << 0;

const queue_index_control: u16 = 0;
const queue_size_requested: u16 = 8;
const queue_used_offset: usize = 4096;
const desc_flag_next: u16 = 1 << 0;
const desc_flag_write: u16 = 1 << 1;
const max_backing_pages: usize = 512;
const max_alloc_chunk_pages: usize = 64;
const scanout_width_cap: u32 = 800;
const scanout_height_cap: u32 = 600;
const wait_spin_limit: usize = 400_000;
const wait_sleep_limit: usize = 2048;

const virtio_gpu_cmd_get_display_info: u32 = 0x0100;
const virtio_gpu_cmd_resource_create_2d: u32 = 0x0101;
const virtio_gpu_cmd_resource_unref: u32 = 0x0102;
const virtio_gpu_cmd_set_scanout: u32 = 0x0103;
const virtio_gpu_cmd_resource_flush: u32 = 0x0104;
const virtio_gpu_cmd_transfer_to_host_2d: u32 = 0x0105;
const virtio_gpu_cmd_resource_attach_backing: u32 = 0x0106;
const virtio_gpu_cmd_get_capset_info: u32 = 0x0108;
const virtio_gpu_cmd_ctx_create: u32 = 0x0200;
const virtio_gpu_cmd_ctx_attach_resource: u32 = 0x0202;
const virtio_gpu_cmd_ctx_detach_resource: u32 = 0x0203;
const virtio_gpu_cmd_resource_create_3d: u32 = 0x0204;
const virtio_gpu_cmd_submit_3d: u32 = 0x0207;
const virtio_gpu_resp_ok_nodata: u32 = 0x1100;
const virtio_gpu_resp_ok_display_info: u32 = 0x1101;
const virtio_gpu_resp_ok_capset_info: u32 = 0x1102;
const virtio_gpu_format_b8g8r8x8_unorm: u32 = 2;
const virgl_format_b8g8r8a8_unorm: u32 = 1;
const virgl_format_r8_unorm: u32 = 64;
const pipe_buffer: u32 = 0;
const pipe_texture_2d: u32 = 2;
const pipe_bind_render_target: u32 = 2;
const pipe_bind_sampler_view: u32 = 1 << 3;
const pipe_bind_vertex_buffer: u32 = 1 << 4;
const pipe_bind_display_target: u32 = 1 << 7;
const pipe_bind_scanout: u32 = 1 << 18;
const virtio_gpu_capset_virgl: u32 = 1;
const virtio_gpu_capset_virgl2: u32 = 2;
const default_context_id: u32 = 1;
const virgl_object_null: u32 = 0;
const virgl_ccmd_nop: u32 = 0;
const virgl_ccmd_resource_inline_write: u32 = 9;

const BootState = struct {
    endpoint_id: u64 = 0,
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
    control_queue_submit_token: u64 = 0,
    control_queue_notify_token: u64 = 0,
    cursor_queue_submit_token: u64 = 0,
    cursor_queue_notify_token: u64 = 0,
    command_token: u64 = 0,
};

const Rect = extern struct {
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

const VirtioGpuDisplayOne = extern struct {
    rect: Rect,
    enabled: u32,
    flags: u32,
};

const VirtioGpuRespDisplayInfo = extern struct {
    hdr: VirtioGpuCtrlHdr,
    pmodes: [16]VirtioGpuDisplayOne,
};

const VirtioGpuResourceCreate2d = extern struct {
    hdr: VirtioGpuCtrlHdr,
    resource_id: u32,
    format: u32,
    width: u32,
    height: u32,
};

const VirtioGpuResourceCreate3d = extern struct {
    hdr: VirtioGpuCtrlHdr,
    resource_id: u32,
    target: u32,
    format: u32,
    bind: u32,
    width: u32,
    height: u32,
    depth: u32,
    array_size: u32,
    last_level: u32,
    nr_samples: u32,
    flags: u32,
    padding: u32,
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

const VirtioGpuGetCapsetInfo = extern struct {
    hdr: VirtioGpuCtrlHdr,
    capset_index: u32,
    padding: u32,
};

const VirtioGpuRespCapsetInfo = extern struct {
    hdr: VirtioGpuCtrlHdr,
    capset_id: u32,
    capset_max_version: u32,
    capset_max_size: u32,
    padding: u32,
};

const VirtioGpuCtxCreate = extern struct {
    hdr: VirtioGpuCtrlHdr,
    nlen: u32,
    context_init: u32,
    debug_name: [64]u8,
};

const VirtioGpuCtxResource = extern struct {
    hdr: VirtioGpuCtrlHdr,
    resource_id: u32,
    padding: u32,
};

const VirtioGpuCmdSubmit = extern struct {
    hdr: VirtioGpuCtrlHdr,
    size: u32,
    padding: u32,
};

const VirtioGpuMemEntry = extern struct {
    addr: u64,
    length: u32,
    padding: u32,
};

const GpuState = struct {
    common_base: usize = 0,
    notify_base: usize = 0,
    notify_addr: usize = 0,
    notify_off_multiplier: usize = 0,
    queue_size: u16 = 0,
    used_idx_seen: u16 = 0,
    queue_paddr0: u64 = 0,
    queue_paddr1: u64 = 0,
    control_page_paddr: u64 = 0,
    mem_entries_paddr0: u64 = 0,
    mem_entries_paddr1: u64 = 0,
    backing_page_count: usize = 0,
    backing_bytes_len: usize = 0,
    resource_id: u32 = 1,
    scanout_id: u32 = 0,
    width: u32 = 0,
    height: u32 = 0,
    scanout_ready: bool = false,
    virgl_supported: bool = false,
    capset_id: u32 = 0,
    capset_max_version: u32 = 0,
    context_id: u32 = default_context_id,
    virgl_resource_id: u32 = gpu_protocol.default_virgl_resource_id,
    virgl_vertex_buffer_id: u32 = gpu_protocol.default_virgl_vertex_buffer_id,
    next_virgl_texture_resource_id: u32 = gpu_protocol.first_virgl_texture_resource_id,
    virgl_render_target_ready: bool = false,
    virgl_vertex_buffer_ready: bool = false,
    next_virgl_surface_id: u32 = 1,
    submit_3d_logged: bool = false,
    present_3d_logged: bool = false,
    texture_upload_logged: bool = false,
    app_surface_logged: bool = false,
};

var boot_state: BootState = .{};
var gpu_state: GpuState = .{};
var backing_paddrs: [max_backing_pages]u64 = [_]u64{0} ** max_backing_pages;
var request_page_paddr_seen: u64 = 0;
var response_page_paddr_seen: u64 = 0;
var last_request_seq_seen: u64 = 0;

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

fn iommuAuthorize(token: u64, device: queue_abi.DeviceId, op: queue_abi.IommuOperation) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_iommu_authorize),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (@as(u64, @intFromEnum(device))),
          [arg2] "{rdx}" (@as(u64, @intFromEnum(op))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn commandAuthorize(token: u64, device: queue_abi.DeviceId, opcode: queue_abi.CommandOpcodeClass) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_command_authorize),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (@as(u64, @intFromEnum(device))),
          [arg2] "{rdx}" (@as(u64, @intFromEnum(opcode))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn dmaMapCreate(device: queue_abi.DeviceId, paddr_start: u64, length: u64, direction: queue_abi.DmaDirection) u64 {
    const raw = asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_dma_map_create),
          [arg0] "{rdi}" (@as(u64, @intFromEnum(device))),
          [arg1] "{rsi}" (paddr_start),
          [arg2] "{rdx}" (length),
          [arg3] "{r8}" (@as(u64, @intFromEnum(direction))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
    return queue_abi.decodeDmaMappingToken(raw) orelse 0;
}

fn dmaMapSetState(token: u64, state: queue_abi.DmaMappingState) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_dma_map_set_state),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (@as(u64, @intFromEnum(state))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn dmaMapRelease(token: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_dma_map_release),
          [arg0] "{rdi}" (token),
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

fn mmioReadU32(addr: usize) u32 {
    const ptr: *const volatile u32 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioWriteU32(addr: usize, value: u32) void {
    const ptr: *volatile u32 = @ptrFromInt(addr);
    ptr.* = value;
}

fn mmioWriteU64(addr: usize, value: u64) void {
    const ptr: *volatile u64 = @ptrFromInt(addr);
    ptr.* = value;
}

fn clearBytes(base_va: usize, len: usize) void {
    const bytes: [*]volatile u8 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < len) : (i += 1) {
        bytes[i] = 0;
    }
}

fn copyVolatileBytes(dest: [*]volatile u8, src: [*]const volatile u8, len: usize) void {
    var i: usize = 0;
    while (i < len) : (i += 1) {
        dest[i] = src[i];
    }
}

fn memoryBarrier() void {
    asm volatile ("" ::: .{ .memory = true });
}

fn minU32(a: u32, b: u32) u32 {
    return if (a < b) a else b;
}

fn waitMapMmioPage(va: u64, paddr: u64, writable: bool) bool {
    if (paddr < 0x1000) return false;
    var attempt: usize = 0;
    while (attempt < 4096) : (attempt += 1) {
        if (mapMmioPage(va, paddr, writable) == syscall_ok) return true;
        _ = waitEvent(false, 1);
    }
    return false;
}

fn parseBootState() ?BootState {
    if (readCfgU64(0) != gpu_bootstrap.config_magic or readCfgU64(1) != gpu_bootstrap.config_version) return null;
    return .{
        .endpoint_id = readCfgU64(gpu_bootstrap.endpoint_id_index),
        .common_page_paddr = readCfgU64(gpu_bootstrap.common_page_paddr_index),
        .notify_page_paddr = readCfgU64(gpu_bootstrap.notify_page_paddr_index),
        .isr_page_paddr = readCfgU64(gpu_bootstrap.isr_page_paddr_index),
        .device_page_paddr = readCfgU64(gpu_bootstrap.device_page_paddr_index),
        .common_page_offset = readCfgU64(gpu_bootstrap.common_page_offset_index),
        .notify_page_offset = readCfgU64(gpu_bootstrap.notify_page_offset_index),
        .isr_page_offset = readCfgU64(gpu_bootstrap.isr_page_offset_index),
        .device_page_offset = readCfgU64(gpu_bootstrap.device_page_offset_index),
        .notify_off_multiplier = readCfgU64(gpu_bootstrap.notify_off_multiplier_index),
        .iommu_token = readCfgU64(gpu_bootstrap.iommu_token_index),
        .control_queue_submit_token = readCfgU64(gpu_bootstrap.control_queue_submit_token_index),
        .control_queue_notify_token = readCfgU64(gpu_bootstrap.control_queue_notify_token_index),
        .cursor_queue_submit_token = readCfgU64(gpu_bootstrap.cursor_queue_submit_token_index),
        .cursor_queue_notify_token = readCfgU64(gpu_bootstrap.cursor_queue_notify_token_index),
        .command_token = readCfgU64(gpu_bootstrap.command_token_index),
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
            boot_state.command_token != 0 and
            boot_state.iommu_token != 0 and
            boot_state.control_queue_submit_token != 0 and
            boot_state.control_queue_notify_token != 0)
        {
            return;
        }
        _ = waitEvent(false, 1);
        asm volatile ("pause");
    }
}

fn mapDeviceView() bool {
    if (!waitMapMmioPage(common_page_va, boot_state.common_page_paddr, true)) {
        _ = userLog("VirtioGpuGl: map common failed\n");
        return false;
    }
    const notify_map_va = if (boot_state.notify_page_paddr == boot_state.common_page_paddr) common_page_va else notify_page_va;
    if (notify_map_va != common_page_va and !waitMapMmioPage(notify_map_va, boot_state.notify_page_paddr, true)) {
        _ = userLog("VirtioGpuGl: map notify failed\n");
        return false;
    }
    var isr_map_va: usize = 0;
    if (boot_state.isr_page_paddr != 0) {
        isr_map_va = if (boot_state.isr_page_paddr == boot_state.common_page_paddr) common_page_va else if (boot_state.isr_page_paddr == boot_state.notify_page_paddr) notify_map_va else isr_page_va;
        if (isr_map_va != common_page_va and isr_map_va != notify_map_va and !waitMapMmioPage(isr_map_va, boot_state.isr_page_paddr, false)) {
            _ = userLog("VirtioGpuGl: map isr failed\n");
            return false;
        }
    }
    if (boot_state.device_page_paddr != 0) {
        const device_map_va =
            if (boot_state.device_page_paddr == boot_state.common_page_paddr)
                common_page_va
            else if (boot_state.device_page_paddr == boot_state.notify_page_paddr)
                notify_map_va
            else if (boot_state.isr_page_paddr != 0 and boot_state.device_page_paddr == boot_state.isr_page_paddr)
                isr_map_va
            else
                device_page_va;
        if (device_map_va != common_page_va and device_map_va != notify_map_va and device_map_va != isr_map_va and !waitMapMmioPage(device_map_va, boot_state.device_page_paddr, false)) {
            _ = userLog("VirtioGpuGl: map device failed\n");
            return false;
        }
    }

    gpu_state.common_base = common_page_va + @as(usize, @intCast(boot_state.common_page_offset));
    gpu_state.notify_base = notify_map_va + @as(usize, @intCast(boot_state.notify_page_offset));
    gpu_state.notify_off_multiplier = @intCast(boot_state.notify_off_multiplier);
    if (gpu_state.notify_off_multiplier == 0) return false;
    return true;
}

fn queueRegionPhys(offset: usize) u64 {
    if (offset < 4096) return gpu_state.queue_paddr0 + @as(u64, @intCast(offset));
    return gpu_state.queue_paddr1 + @as(u64, @intCast(offset - 4096));
}

fn queueDescPtr(index: u16) *volatile VirtqDesc {
    const offset = @as(usize, index) * @sizeOf(VirtqDesc);
    return @ptrFromInt(queue_page0_va + offset);
}

fn queueAvailIdxPtr() *volatile u16 {
    return @ptrFromInt(queue_page0_va + @as(usize, gpu_state.queue_size) * @sizeOf(VirtqDesc) + 2);
}

fn queueAvailRingPtr() [*]volatile u16 {
    return @ptrFromInt(queue_page0_va + @as(usize, gpu_state.queue_size) * @sizeOf(VirtqDesc) + 4);
}

fn queueUsedIdxPtr() *volatile u16 {
    return @ptrFromInt(queue_page0_va + queue_used_offset + 2);
}

fn queueUsedRingPtr() [*]volatile VirtqUsedElem {
    return @ptrFromInt(queue_page0_va + queue_used_offset + 4);
}

fn queuePushAvail(head_desc: u16) void {
    const avail_idx_ptr = queueAvailIdxPtr();
    const avail_idx = avail_idx_ptr.*;
    const slot: usize = @intCast(avail_idx % gpu_state.queue_size);
    queueAvailRingPtr()[slot] = head_desc;
    memoryBarrier();
    avail_idx_ptr.* = avail_idx +% 1;
    memoryBarrier();
}

fn controlReqPaddr() u64 {
    return gpu_state.control_page_paddr + control_request_offset;
}

fn controlRespPaddr() u64 {
    return gpu_state.control_page_paddr + control_response_offset;
}

fn initQueueMemory() bool {
    var init_paddrs: [5]u64 = .{ 0, 0, 0, 0, 0 };
    if (allocMapPages(queue_page0_va, init_paddrs.len, true, @intFromPtr(&init_paddrs)) != syscall_ok) {
        _ = userLog("VirtioGpuGl: alloc queue pages failed\n");
        return false;
    }
    for (init_paddrs) |paddr| {
        if (paddr < 0x1000) return false;
    }
    gpu_state.queue_paddr0 = init_paddrs[0];
    gpu_state.queue_paddr1 = init_paddrs[1];
    gpu_state.control_page_paddr = init_paddrs[2];
    gpu_state.mem_entries_paddr0 = init_paddrs[3];
    gpu_state.mem_entries_paddr1 = init_paddrs[4];
    clearBytes(queue_page0_va, 8192);
    clearBytes(control_page_va, 4096);
    clearBytes(mem_entries_page0_va, 8192);
    return true;
}

fn initVirtio() bool {
    mmioWriteU8(gpu_state.common_base + common_device_status, 0);
    mmioWriteU8(gpu_state.common_base + common_device_status, status_acknowledge | status_driver);

    mmioWriteU32(gpu_state.common_base + common_device_feature_select, 0);
    const features_low = mmioReadU32(gpu_state.common_base + common_device_feature);
    mmioWriteU32(gpu_state.common_base + common_device_feature_select, 1);
    const features_high = mmioReadU32(gpu_state.common_base + common_device_feature);
    writeCfgU64(gpu_bootstrap.device_features_low_index, features_low);
    writeCfgU64(gpu_bootstrap.device_features_high_index, features_high);
    gpu_state.virgl_supported = (features_low & feature_virgl) != 0;

    if ((features_high & feature_version_1) == 0) {
        _ = userLog("VirtioGpuGl: VERSION_1 missing\n");
        return false;
    }
    if (gpu_state.virgl_supported) {
        _ = userLog("VirtioGpuGl: VIRGL feature present\n");
    } else {
        _ = userLog("VirtioGpuGl: VIRGL feature missing\n");
    }

    mmioWriteU32(gpu_state.common_base + common_driver_feature_select, 0);
    mmioWriteU32(gpu_state.common_base + common_driver_feature, if (gpu_state.virgl_supported) feature_virgl else 0);
    mmioWriteU32(gpu_state.common_base + common_driver_feature_select, 1);
    mmioWriteU32(gpu_state.common_base + common_driver_feature, feature_version_1);
    mmioWriteU8(gpu_state.common_base + common_device_status, status_acknowledge | status_driver | status_features_ok);
    if ((mmioReadU8(gpu_state.common_base + common_device_status) & status_features_ok) == 0) {
        _ = userLog("VirtioGpuGl: FEATURES_OK rejected\n");
        return false;
    }

    mmioWriteU16(gpu_state.common_base + common_queue_select, queue_index_control);
    const device_queue_size = mmioReadU16(gpu_state.common_base + common_queue_size);
    if (device_queue_size < 4) {
        _ = userLog("VirtioGpuGl: control queue too small\n");
        return false;
    }
    gpu_state.queue_size = if (device_queue_size < queue_size_requested) device_queue_size else queue_size_requested;
    const queue_notify_off = mmioReadU16(gpu_state.common_base + common_queue_notify_off);
    mmioWriteU16(gpu_state.common_base + common_queue_size, gpu_state.queue_size);
    mmioWriteU64(gpu_state.common_base + common_queue_desc, queueRegionPhys(0));
    mmioWriteU64(gpu_state.common_base + common_queue_avail, queueRegionPhys(@as(usize, gpu_state.queue_size) * @sizeOf(VirtqDesc)));
    mmioWriteU64(gpu_state.common_base + common_queue_used, queueRegionPhys(queue_used_offset));
    mmioWriteU16(gpu_state.common_base + common_queue_enable, 1);
    gpu_state.notify_addr = gpu_state.notify_base + @as(usize, queue_notify_off) * gpu_state.notify_off_multiplier;
    gpu_state.used_idx_seen = 0;
    mmioWriteU8(gpu_state.common_base + common_device_status, status_acknowledge | status_driver | status_features_ok | status_driver_ok);
    return true;
}

fn setMappingState(token: u64, state: queue_abi.DmaMappingState) bool {
    if (token == 0) return true;
    return dmaMapSetState(token, state) == syscall_ok;
}

fn releaseMapping(token: u64) bool {
    if (token == 0) return true;
    return dmaMapRelease(token) == syscall_ok;
}

fn createControlMapping(paddr: u64, len: usize, direction: queue_abi.DmaDirection, op: queue_abi.IommuOperation) u64 {
    if (iommuAuthorize(boot_state.iommu_token, .virtio_gpu, op) != syscall_ok) return 0;
    return dmaMapCreate(.virtio_gpu, paddr, @intCast(len), direction);
}

fn submitCommand(
    req_len: usize,
    extra_paddr: u64,
    extra_len: usize,
    opcode: queue_abi.CommandOpcodeClass,
    expected_resp_type: u32,
) bool {
    if (commandAuthorize(boot_state.command_token, .virtio_gpu, opcode) != syscall_ok) {
        _ = userLog("VirtioGpuGl: command denied\n");
        return false;
    }

    const extra0_len: usize = if (extra_len > 4096) 4096 else extra_len;
    const extra1_len: usize = extra_len - extra0_len;
    const extra1_paddr = if (extra_paddr == gpu_state.mem_entries_paddr0) gpu_state.mem_entries_paddr1 else extra_paddr + 4096;

    const req_mapping = createControlMapping(controlReqPaddr(), req_len, .read, .map_read);
    const resp_mapping = createControlMapping(controlRespPaddr(), @sizeOf(VirtioGpuRespDisplayInfo), .write, .map_write);
    const extra0_mapping = if (extra0_len != 0) createControlMapping(extra_paddr, extra0_len, .read, .map_read) else 0;
    const extra1_mapping = if (extra1_len != 0) createControlMapping(extra1_paddr, extra1_len, .read, .map_read) else 0;
    if (req_mapping == 0 or resp_mapping == 0 or (extra0_len != 0 and extra0_mapping == 0) or (extra1_len != 0 and extra1_mapping == 0)) {
        _ = userLog("VirtioGpuGl: dma map failed\n");
        return false;
    }

    clearBytes(control_page_va + control_response_offset, @sizeOf(VirtioGpuRespDisplayInfo));
    memoryBarrier();

    var desc_count: u16 = 0;
    queueDescPtr(desc_count).* = .{
        .addr = controlReqPaddr(),
        .len = @intCast(req_len),
        .flags = 0,
        .next = 0,
    };
    desc_count += 1;

    if (extra0_len != 0) {
        queueDescPtr(desc_count - 1).flags = desc_flag_next;
        queueDescPtr(desc_count - 1).next = desc_count;
        queueDescPtr(desc_count).* = .{
            .addr = extra_paddr,
            .len = @intCast(extra0_len),
            .flags = 0,
            .next = 0,
        };
        desc_count += 1;
    }

    if (extra1_len != 0) {
        queueDescPtr(desc_count - 1).flags = desc_flag_next;
        queueDescPtr(desc_count - 1).next = desc_count;
        queueDescPtr(desc_count).* = .{
            .addr = extra1_paddr,
            .len = @intCast(extra1_len),
            .flags = 0,
            .next = 0,
        };
        desc_count += 1;
    }

    queueDescPtr(desc_count - 1).flags = desc_flag_next;
    queueDescPtr(desc_count - 1).next = desc_count;
    queueDescPtr(desc_count).* = .{
        .addr = controlRespPaddr(),
        .len = @sizeOf(VirtioGpuRespDisplayInfo),
        .flags = desc_flag_write,
        .next = 0,
    };

    if (!setMappingState(req_mapping, .in_flight) or
        !setMappingState(resp_mapping, .in_flight) or
        !setMappingState(extra0_mapping, .in_flight) or
        !setMappingState(extra1_mapping, .in_flight))
    {
        return false;
    }
    if (queueSubmit(boot_state.control_queue_submit_token, queue_index_control) != syscall_ok) return false;
    queuePushAvail(0);
    if (queueNotify(boot_state.control_queue_notify_token, queue_index_control) != syscall_ok) return false;
    memoryBarrier();
    mmioWriteU16(gpu_state.notify_addr, queue_index_control);

    var spin: usize = 0;
    while (spin < wait_spin_limit) : (spin += 1) {
        if (queueUsedIdxPtr().* != gpu_state.used_idx_seen) break;
        asm volatile ("pause");
    }
    var sleeps: usize = 0;
    while (queueUsedIdxPtr().* == gpu_state.used_idx_seen and sleeps < wait_sleep_limit) : (sleeps += 1) {
        _ = waitEvent(false, 1);
    }
    if (queueUsedIdxPtr().* == gpu_state.used_idx_seen) {
        _ = userLog("VirtioGpuGl: command timeout\n");
        return false;
    }

    const used = queueUsedRingPtr()[@intCast(gpu_state.used_idx_seen % gpu_state.queue_size)];
    gpu_state.used_idx_seen +%= 1;
    if (used.id != 0) {
        _ = userLog("VirtioGpuGl: bad used id\n");
        return false;
    }
    memoryBarrier();
    const resp: *const volatile VirtioGpuCtrlHdr = @ptrFromInt(control_page_va + control_response_offset);
    if (resp.type != expected_resp_type) {
        _ = userLog("VirtioGpuGl: bad response\n");
        return false;
    }

    if (!setMappingState(req_mapping, .completed) or
        !setMappingState(resp_mapping, .completed) or
        !setMappingState(extra0_mapping, .completed) or
        !setMappingState(extra1_mapping, .completed))
    {
        return false;
    }
    return releaseMapping(req_mapping) and releaseMapping(resp_mapping) and releaseMapping(extra0_mapping) and releaseMapping(extra1_mapping);
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

fn virglCmd0(command: u32, object: u32, len: u32) u32 {
    return command | (object << 8) | (len << 16);
}

fn getDisplayInfo() bool {
    const req: *volatile VirtioGpuCtrlHdr = @ptrFromInt(control_page_va + control_request_offset);
    req.* = controlHdr(virtio_gpu_cmd_get_display_info);
    if (!submitCommand(@sizeOf(VirtioGpuCtrlHdr), 0, 0, .gpu_admin, virtio_gpu_resp_ok_display_info)) {
        _ = userLog("VirtioGpuGl: display info failed\n");
        return false;
    }
    const resp: *const volatile VirtioGpuRespDisplayInfo = @ptrFromInt(control_page_va + control_response_offset);
    var chosen = resp.pmodes[0];
    var scanout: u32 = 0;
    var i: usize = 0;
    while (i < resp.pmodes.len) : (i += 1) {
        const mode = resp.pmodes[i];
        if (mode.enabled != 0 and mode.rect.width != 0 and mode.rect.height != 0) {
            chosen = mode;
            scanout = @intCast(i);
            break;
        }
    }
    if (chosen.rect.width == 0 or chosen.rect.height == 0) {
        chosen.rect.width = scanout_width_cap;
        chosen.rect.height = scanout_height_cap;
    }
    gpu_state.scanout_id = scanout;
    gpu_state.width = minU32(chosen.rect.width, scanout_width_cap);
    gpu_state.height = minU32(chosen.rect.height, scanout_height_cap);
    if (gpu_state.width == 0 or gpu_state.height == 0) return false;
    _ = userLog("VirtioGpuGl: display info ready\n");
    return true;
}

fn queryCapsetInfo() bool {
    if (!gpu_state.virgl_supported) return false;

    var selected: VirtioGpuRespCapsetInfo = .{
        .hdr = controlHdr(0),
        .capset_id = 0,
        .capset_max_version = 0,
        .capset_max_size = 0,
        .padding = 0,
    };
    var index: u32 = 0;
    while (index < 4) : (index += 1) {
        const req: *volatile VirtioGpuGetCapsetInfo = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_get_capset_info),
            .capset_index = index,
            .padding = 0,
        };
        if (!submitCommand(@sizeOf(VirtioGpuGetCapsetInfo), 0, 0, .gpu_admin, virtio_gpu_resp_ok_capset_info)) {
            _ = userLog("VirtioGpuGl: capset info failed\n");
            return false;
        }
        const resp: *const volatile VirtioGpuRespCapsetInfo = @ptrFromInt(control_page_va + control_response_offset);
        if (resp.capset_max_version == 0 or resp.capset_max_size == 0) continue;
        if (resp.capset_id == virtio_gpu_capset_virgl2) {
            selected = resp.*;
            break;
        }
        if (resp.capset_id == virtio_gpu_capset_virgl and selected.capset_id == 0) {
            selected = resp.*;
        }
    }

    if (selected.capset_id == 0) {
        _ = userLog("VirtioGpuGl: no virgl capset\n");
        return false;
    }
    gpu_state.capset_id = selected.capset_id;
    gpu_state.capset_max_version = selected.capset_max_version;
    _ = userLog("VirtioGpuGl: capset ready\n");
    return true;
}

fn createVirglContext() bool {
    if (!gpu_state.virgl_supported or gpu_state.capset_id == 0) return false;
    const name = "capos-virgl";
    const req: *volatile VirtioGpuCtxCreate = @ptrFromInt(control_page_va + control_request_offset);
    req.* = .{
        .hdr = controlHdr(virtio_gpu_cmd_ctx_create),
        .nlen = name.len,
        .context_init = gpu_state.capset_id,
        .debug_name = [_]u8{0} ** 64,
    };
    req.hdr.ctx_id = gpu_state.context_id;
    var i: usize = 0;
    while (i < name.len and i < req.debug_name.len) : (i += 1) {
        req.debug_name[i] = name[i];
    }
    if (!submitCommand(@sizeOf(VirtioGpuCtxCreate), 0, 0, .gpu_virgl_context, virtio_gpu_resp_ok_nodata)) {
        _ = userLog("VirtioGpuGl: ctx_create failed\n");
        return false;
    }
    _ = userLog("VirtioGpuGl: ctx_create ready\n");
    return true;
}

fn createVirglResource3d(resource_id: u32, target: u32, format: u32, bind: u32, width: u32, height: u32) bool {
    if (!gpu_state.virgl_supported or gpu_state.capset_id == 0) return false;
    if (width == 0 or height == 0) return false;
    const req: *volatile VirtioGpuResourceCreate3d = @ptrFromInt(control_page_va + control_request_offset);
    req.* = .{
        .hdr = controlHdr(virtio_gpu_cmd_resource_create_3d),
        .resource_id = resource_id,
        .target = target,
        .format = format,
        .bind = bind,
        .width = width,
        .height = height,
        .depth = 1,
        .array_size = 1,
        .last_level = 0,
        .nr_samples = 0,
        .flags = 0,
        .padding = 0,
    };
    req.hdr.ctx_id = gpu_state.context_id;
    if (!submitCommand(@sizeOf(VirtioGpuResourceCreate3d), 0, 0, .gpu_virgl_resource, virtio_gpu_resp_ok_nodata)) {
        _ = userLog("VirtioGpuGl: resource_create_3d failed\n");
        return false;
    }
    _ = userLog("VirtioGpuGl: resource_create_3d ready\n");
    return true;
}

fn attachVirglResourceToContext(resource_id: u32) bool {
    if (!gpu_state.virgl_supported or gpu_state.capset_id == 0) return false;
    const req: *volatile VirtioGpuCtxResource = @ptrFromInt(control_page_va + control_request_offset);
    req.* = .{
        .hdr = controlHdr(virtio_gpu_cmd_ctx_attach_resource),
        .resource_id = resource_id,
        .padding = 0,
    };
    req.hdr.ctx_id = gpu_state.context_id;
    if (!submitCommand(@sizeOf(VirtioGpuCtxResource), 0, 0, .gpu_virgl_resource, virtio_gpu_resp_ok_nodata)) {
        _ = userLog("VirtioGpuGl: ctx_attach_resource failed\n");
        return false;
    }
    _ = userLog("VirtioGpuGl: ctx_attach_resource ready\n");
    return true;
}

fn detachVirglResourceFromContext(resource_id: u32) bool {
    if (!gpu_state.virgl_supported or gpu_state.capset_id == 0 or resource_id == 0) return false;
    const req: *volatile VirtioGpuCtxResource = @ptrFromInt(control_page_va + control_request_offset);
    req.* = .{
        .hdr = controlHdr(virtio_gpu_cmd_ctx_detach_resource),
        .resource_id = resource_id,
        .padding = 0,
    };
    req.hdr.ctx_id = gpu_state.context_id;
    if (!submitCommand(@sizeOf(VirtioGpuCtxResource), 0, 0, .gpu_virgl_resource, virtio_gpu_resp_ok_nodata)) {
        _ = userLog("VirtioGpuGl: ctx_detach_resource failed\n");
        return false;
    }
    _ = userLog("VirtioGpuGl: ctx_detach_resource ready\n");
    return true;
}

fn unrefVirglResource(resource_id: u32) bool {
    if (resource_id == 0) return false;
    const req: *volatile VirtioGpuCtxResource = @ptrFromInt(control_page_va + control_request_offset);
    req.* = .{
        .hdr = controlHdr(virtio_gpu_cmd_resource_unref),
        .resource_id = resource_id,
        .padding = 0,
    };
    if (!submitCommand(@sizeOf(VirtioGpuCtxResource), 0, 0, .gpu_virgl_resource, virtio_gpu_resp_ok_nodata)) {
        _ = userLog("VirtioGpuGl: resource_unref failed\n");
        return false;
    }
    _ = userLog("VirtioGpuGl: resource_unref ready\n");
    return true;
}

fn deleteVirglTexture2d(resource_id: u32) bool {
    if (resource_id == 0) return false;
    if (!detachVirglResourceFromContext(resource_id)) return false;
    return unrefVirglResource(resource_id);
}

fn ensureVirglRenderTarget() bool {
    if (!gpu_state.virgl_supported or gpu_state.capset_id == 0) return false;
    if (gpu_state.width == 0 or gpu_state.height == 0) {
        if (!getDisplayInfo()) return false;
    }
    var initialized = false;
    if (!gpu_state.virgl_render_target_ready) {
        if (!createVirglResource3d(
            gpu_state.virgl_resource_id,
            pipe_texture_2d,
            virgl_format_b8g8r8a8_unorm,
            pipe_bind_render_target | pipe_bind_display_target | pipe_bind_scanout,
            gpu_state.width,
            gpu_state.height,
        )) return false;
        if (!attachVirglResourceToContext(gpu_state.virgl_resource_id)) return false;
        gpu_state.virgl_render_target_ready = true;
        initialized = true;
    }
    if (!gpu_state.virgl_vertex_buffer_ready) {
        if (!createVirglResource3d(
            gpu_state.virgl_vertex_buffer_id,
            pipe_buffer,
            virgl_format_r8_unorm,
            pipe_bind_vertex_buffer,
            4096,
            1,
        )) return false;
        if (!attachVirglResourceToContext(gpu_state.virgl_vertex_buffer_id)) return false;
        gpu_state.virgl_vertex_buffer_ready = true;
        initialized = true;
    }
    if (initialized) _ = userLog("VirtioGpuGl: render target ready\n");
    return true;
}

fn nextVirglSurfaceId() u32 {
    const surface_id = gpu_state.next_virgl_surface_id;
    gpu_state.next_virgl_surface_id +%= 1;
    if (gpu_state.next_virgl_surface_id == 0) gpu_state.next_virgl_surface_id = 1;
    return surface_id;
}

fn submitVirglNoop() bool {
    if (!gpu_state.virgl_supported or gpu_state.capset_id == 0) return false;
    clearBytes(mem_entries_page0_va, 4);
    const commands: [*]volatile u32 = @ptrFromInt(mem_entries_page0_va);
    commands[0] = virglCmd0(virgl_ccmd_nop, virgl_object_null, 0);
    return submitVirglCommandBuffer(4);
}

fn submitVirglInlineCommands(inline_bytes: usize) bool {
    if (!gpu_state.virgl_supported or gpu_state.capset_id == 0) return false;
    if (inline_bytes == 0 or inline_bytes > gpu_protocol.request_payload_bytes or (inline_bytes & 0x3) != 0) return false;
    clearBytes(mem_entries_page0_va, inline_bytes);
    const dest: [*]volatile u8 = @ptrFromInt(mem_entries_page0_va);
    const src: [*]const volatile u8 = @ptrFromInt(gpu_request_page_va + gpu_protocol.request_header_bytes);
    copyVolatileBytes(dest, src, inline_bytes);
    return submitVirglCommandBuffer(inline_bytes);
}

fn submitVirglCommandBuffer(command_bytes: usize) bool {
    if (!gpu_state.virgl_supported or gpu_state.capset_id == 0) return false;
    if (command_bytes == 0 or command_bytes > 4096 or (command_bytes & 0x3) != 0) return false;
    memoryBarrier();

    const req: *volatile VirtioGpuCmdSubmit = @ptrFromInt(control_page_va + control_request_offset);
    req.* = .{
        .hdr = controlHdr(virtio_gpu_cmd_submit_3d),
        .size = @intCast(command_bytes),
        .padding = 0,
    };
    req.hdr.ctx_id = gpu_state.context_id;
    if (!submitCommand(@sizeOf(VirtioGpuCmdSubmit), gpu_state.mem_entries_paddr0, command_bytes, .gpu_virgl_submit, virtio_gpu_resp_ok_nodata)) {
        _ = userLog("VirtioGpuGl: submit_3d failed\n");
        return false;
    }
    if (!gpu_state.submit_3d_logged) {
        _ = userLog("VirtioGpuGl: submit_3d ready\n");
        gpu_state.submit_3d_logged = true;
    }
    return true;
}

fn submitVirglTextureInlineWrite(resource_id: u32, x: u32, y: u32, width: u32, height: u32, payload_bytes: usize) bool {
    if (!gpu_state.virgl_supported or gpu_state.capset_id == 0) return false;
    if (width == 0 or height == 0 or payload_bytes == 0 or payload_bytes > gpu_protocol.request_payload_bytes) return false;
    const expected_bytes = @as(usize, width) * @as(usize, height) * 4;
    if (payload_bytes != expected_bytes) return false;
    const padded_bytes = (payload_bytes + 3) & ~@as(usize, 3);
    const command_bytes = 48 + padded_bytes;
    if (command_bytes > 4096) return false;

    clearBytes(mem_entries_page0_va, command_bytes);
    const commands: [*]volatile u32 = @ptrFromInt(mem_entries_page0_va);
    commands[0] = virglCmd0(virgl_ccmd_resource_inline_write, virgl_object_null, 11 + @as(u32, @intCast(padded_bytes / 4)));
    commands[1] = resource_id;
    commands[2] = 0;
    commands[3] = 0;
    commands[4] = width * 4;
    commands[5] = @intCast(payload_bytes);
    commands[6] = x;
    commands[7] = y;
    commands[8] = 0;
    commands[9] = width;
    commands[10] = height;
    commands[11] = 1;

    const dest: [*]volatile u8 = @ptrFromInt(mem_entries_page0_va + 48);
    const src: [*]const volatile u8 = @ptrFromInt(gpu_request_page_va + gpu_protocol.request_header_bytes);
    copyVolatileBytes(dest, src, payload_bytes);
    return submitVirglCommandBuffer(command_bytes);
}

fn uploadVirglTexture2d(resource_id_hint: u32, width: u32, height: u32, payload_bytes: usize) ?u32 {
    if (!ensureVirglRenderTarget()) return null;
    if (width == 0 or height == 0) return null;
    if (width > 1024 or height > 1024) return null;
    const expected_bytes = @as(usize, width) * @as(usize, height) * 4;
    if (payload_bytes != expected_bytes or payload_bytes > gpu_protocol.request_payload_bytes) return null;

    const resource_id = if (resource_id_hint >= gpu_protocol.first_virgl_texture_resource_id) resource_id_hint else blk: {
        const allocated = gpu_state.next_virgl_texture_resource_id;
        gpu_state.next_virgl_texture_resource_id +%= 1;
        if (gpu_state.next_virgl_texture_resource_id < gpu_protocol.first_virgl_texture_resource_id) {
            gpu_state.next_virgl_texture_resource_id = gpu_protocol.first_virgl_texture_resource_id;
        }
        if (!createVirglResource3d(
            allocated,
            pipe_texture_2d,
            virgl_format_b8g8r8a8_unorm,
            pipe_bind_sampler_view,
            width,
            height,
        )) return null;
        if (!attachVirglResourceToContext(allocated)) return null;
        break :blk allocated;
    };
    if (!submitVirglTextureInlineWrite(resource_id, 0, 0, width, height, payload_bytes)) return null;
    if (!gpu_state.texture_upload_logged) {
        _ = userLog("VirtioGpuGl: texture_upload ready\n");
        gpu_state.texture_upload_logged = true;
    }
    return resource_id;
}

fn createVirglAppSurface(width: u32, height: u32) ?u64 {
    if (!ensureVirglRenderTarget()) return null;
    if (width == 0 or height == 0) return null;
    if (width > 2048 or height > 2048) return null;

    const resource_id = gpu_state.next_virgl_texture_resource_id;
    gpu_state.next_virgl_texture_resource_id +%= 1;
    if (gpu_state.next_virgl_texture_resource_id < gpu_protocol.first_virgl_texture_resource_id) {
        gpu_state.next_virgl_texture_resource_id = gpu_protocol.first_virgl_texture_resource_id;
    }
    const surface_id = nextVirglSurfaceId();
    if (!createVirglResource3d(
        resource_id,
        pipe_texture_2d,
        virgl_format_b8g8r8a8_unorm,
        pipe_bind_sampler_view | pipe_bind_render_target,
        width,
        height,
    )) return null;
    if (!attachVirglResourceToContext(resource_id)) return null;
    if (!gpu_state.app_surface_logged) {
        _ = userLog("VirtioGpuGl: app_surface ready\n");
        gpu_state.app_surface_logged = true;
    }
    return @as(u64, resource_id) | (@as(u64, surface_id) << 32);
}

fn updateVirglTexture2d(resource_id: u32, x: u32, y: u32, width: u32, height: u32, payload_bytes: usize) bool {
    if (resource_id < gpu_protocol.first_virgl_texture_resource_id) return false;
    if (!ensureVirglRenderTarget()) return false;
    if (width == 0 or height == 0) return false;
    if (payload_bytes != @as(usize, width) * @as(usize, height) * 4) return false;
    if (!submitVirglTextureInlineWrite(resource_id, x, y, width, height, payload_bytes)) return false;
    if (!gpu_state.texture_upload_logged) {
        _ = userLog("VirtioGpuGl: texture_upload ready\n");
        gpu_state.texture_upload_logged = true;
    }
    return true;
}

fn allocBacking() bool {
    const total_bytes = @as(usize, gpu_state.width) * @as(usize, gpu_state.height) * 4;
    const page_count = (total_bytes + 4095) / 4096;
    if (page_count == 0 or page_count > max_backing_pages) return false;
    gpu_state.backing_bytes_len = total_bytes;
    gpu_state.backing_page_count = page_count;

    var page_index: usize = 0;
    while (page_index < page_count) {
        const remaining = page_count - page_index;
        const chunk_pages = if (remaining > max_alloc_chunk_pages) max_alloc_chunk_pages else remaining;
        const chunk_base_va = backing_base_va + page_index * 4096;
        if (allocMapPages(@intCast(chunk_base_va), @intCast(chunk_pages), true, @intFromPtr(&backing_paddrs[page_index])) != syscall_ok) return false;
        var j: usize = 0;
        while (j < chunk_pages) : (j += 1) {
            if (backing_paddrs[page_index + j] < 0x1000) return false;
        }
        page_index += chunk_pages;
    }
    return true;
}

fn paintBacking() void {
    const pixels: [*]volatile u32 = @ptrFromInt(backing_base_va);
    var y: u32 = 0;
    while (y < gpu_state.height) : (y += 1) {
        var x: u32 = 0;
        while (x < gpu_state.width) : (x += 1) {
            const band = ((x / 32) + (y / 32)) & 1;
            const accent = if (band == 0) @as(u32, 0x0030_8f89) else @as(u32, 0x0068_3cb5);
            const diag = if (((x + y) % 97) < 4) @as(u32, 0x00d9_d060) else @as(u32, 0);
            pixels[@as(usize, y) * @as(usize, gpu_state.width) + @as(usize, x)] = 0xff00_0000 | accent | diag;
        }
    }
}

fn createAndScanout2d() bool {
    if (gpu_state.scanout_ready) return true;
    if (!allocBacking()) {
        _ = userLog("VirtioGpuGl: backing alloc failed\n");
        return false;
    }
    paintBacking();

    {
        const req: *volatile VirtioGpuResourceCreate2d = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_resource_create_2d),
            .resource_id = gpu_state.resource_id,
            .format = virtio_gpu_format_b8g8r8x8_unorm,
            .width = gpu_state.width,
            .height = gpu_state.height,
        };
        if (!submitCommand(@sizeOf(VirtioGpuResourceCreate2d), 0, 0, .gpu_resource_2d, virtio_gpu_resp_ok_nodata)) {
            _ = userLog("VirtioGpuGl: resource_create failed\n");
            return false;
        }
    }

    {
        clearBytes(mem_entries_page0_va, 8192);
        var i: usize = 0;
        while (i < gpu_state.backing_page_count) : (i += 1) {
            const entry: *volatile VirtioGpuMemEntry = @ptrFromInt(mem_entries_page0_va + i * @sizeOf(VirtioGpuMemEntry));
            entry.* = .{
                .addr = backing_paddrs[i],
                .length = 4096,
                .padding = 0,
            };
        }
        const req: *volatile VirtioGpuResourceAttachBacking = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_resource_attach_backing),
            .resource_id = gpu_state.resource_id,
            .nr_entries = @intCast(gpu_state.backing_page_count),
        };
        if (!submitCommand(
            @sizeOf(VirtioGpuResourceAttachBacking),
            gpu_state.mem_entries_paddr0,
            gpu_state.backing_page_count * @sizeOf(VirtioGpuMemEntry),
            .gpu_resource_2d,
            virtio_gpu_resp_ok_nodata,
        )) {
            _ = userLog("VirtioGpuGl: attach_backing failed\n");
            return false;
        }
    }

    const rect = Rect{ .x = 0, .y = 0, .width = gpu_state.width, .height = gpu_state.height };
    {
        const req: *volatile VirtioGpuSetScanout = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_set_scanout),
            .rect = rect,
            .scanout_id = gpu_state.scanout_id,
            .resource_id = gpu_state.resource_id,
        };
        if (!submitCommand(@sizeOf(VirtioGpuSetScanout), 0, 0, .gpu_scanout, virtio_gpu_resp_ok_nodata)) {
            _ = userLog("VirtioGpuGl: set_scanout failed\n");
            return false;
        }
    }
    {
        const req: *volatile VirtioGpuTransferToHost2d = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_transfer_to_host_2d),
            .rect = rect,
            .offset = 0,
            .resource_id = gpu_state.resource_id,
            .padding = 0,
        };
        if (!submitCommand(@sizeOf(VirtioGpuTransferToHost2d), 0, 0, .gpu_resource_2d, virtio_gpu_resp_ok_nodata)) {
            _ = userLog("VirtioGpuGl: transfer failed\n");
            return false;
        }
    }
    {
        const req: *volatile VirtioGpuResourceFlush = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_resource_flush),
            .rect = rect,
            .resource_id = gpu_state.resource_id,
            .padding = 0,
        };
        if (!submitCommand(@sizeOf(VirtioGpuResourceFlush), 0, 0, .gpu_scanout, virtio_gpu_resp_ok_nodata)) {
            _ = userLog("VirtioGpuGl: flush failed\n");
            return false;
        }
    }

    _ = userLog("VirtioGpuGl: scanout ready\n");
    gpu_state.scanout_ready = true;
    return true;
}

fn gpuFeatureFlags() u64 {
    var flags: u64 = 0;
    if (gpu_state.virgl_supported and gpu_state.capset_id != 0) {
        flags |= gpu_protocol.feature_virgl;
        flags |= gpu_protocol.feature_submit_3d;
        flags |= gpu_protocol.feature_present_3d;
        flags |= gpu_protocol.feature_texture_2d;
        flags |= gpu_protocol.feature_app_surface;
    }
    flags |= gpu_protocol.feature_present_2d;
    return flags;
}

fn presentTestPattern() bool {
    if (gpu_state.width == 0 or gpu_state.height == 0) {
        if (!getDisplayInfo()) return false;
    }
    return createAndScanout2d();
}

fn presentVirglRenderTarget() bool {
    if (!ensureVirglRenderTarget()) return false;
    const rect = Rect{ .x = 0, .y = 0, .width = gpu_state.width, .height = gpu_state.height };
    {
        const req: *volatile VirtioGpuSetScanout = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_set_scanout),
            .rect = rect,
            .scanout_id = gpu_state.scanout_id,
            .resource_id = gpu_state.virgl_resource_id,
        };
        if (!submitCommand(@sizeOf(VirtioGpuSetScanout), 0, 0, .gpu_scanout, virtio_gpu_resp_ok_nodata)) {
            _ = userLog("VirtioGpuGl: set_scanout 3d failed\n");
            return false;
        }
    }
    {
        const req: *volatile VirtioGpuResourceFlush = @ptrFromInt(control_page_va + control_request_offset);
        req.* = .{
            .hdr = controlHdr(virtio_gpu_cmd_resource_flush),
            .rect = rect,
            .resource_id = gpu_state.virgl_resource_id,
            .padding = 0,
        };
        if (!submitCommand(@sizeOf(VirtioGpuResourceFlush), 0, 0, .gpu_scanout, virtio_gpu_resp_ok_nodata)) {
            _ = userLog("VirtioGpuGl: flush 3d failed\n");
            return false;
        }
    }
    gpu_state.scanout_ready = true;
    if (!gpu_state.present_3d_logged) {
        _ = userLog("VirtioGpuGl: present_3d ready\n");
        gpu_state.present_3d_logged = true;
    }
    return true;
}

fn mapGpuRequestPage(request_paddr: u64) bool {
    if (request_paddr < 0x1000) return false;
    if (request_page_paddr_seen == request_paddr) return true;
    if (request_page_paddr_seen != 0) {
        _ = userLog("VirtioGpuGl: request page changed\n");
        return false;
    }
    if (mapPage(gpu_request_page_va, request_paddr, false) != syscall_ok) {
        _ = userLog("VirtioGpuGl: map request page failed\n");
        return false;
    }
    request_page_paddr_seen = request_paddr;
    return true;
}

fn mapGpuResponsePage(response_paddr: u64) bool {
    if (response_paddr < 0x1000) return false;
    if (response_page_paddr_seen == response_paddr) return true;
    if (response_page_paddr_seen != 0) {
        _ = userLog("VirtioGpuGl: response page changed\n");
        return false;
    }
    if (mapPage(gpu_response_page_va, response_paddr, true) != syscall_ok) {
        _ = userLog("VirtioGpuGl: map response page failed\n");
        return false;
    }
    response_page_paddr_seen = response_paddr;
    return true;
}

fn writeGpuResponse(op: gpu_protocol.Opcode, seq: u64, status: gpu_protocol.Status, arg0: u64, arg1: u64, arg2: u64) void {
    const response: *volatile gpu_protocol.ResponseHeader = @ptrFromInt(gpu_response_page_va);
    response.magic = gpu_protocol.response_magic;
    response.version = gpu_protocol.version;
    response.op = gpu_protocol.opcodeRaw(op);
    response.status = gpu_protocol.statusRaw(status);
    response.result_flags = 0;
    response.arg0 = arg0;
    response.arg1 = arg1;
    response.arg2 = arg2;
    response.inline_bytes = 0;
    response.reserved0 = 0;
    memoryBarrier();
    response.response_seq = seq;
}

fn processMappedGpuRequest() void {
    const request: *const volatile gpu_protocol.RequestHeader = @ptrFromInt(gpu_request_page_va);
    const seq = request.request_seq;
    if (seq == 0 or seq == last_request_seq_seen) return;
    if (request.magic != gpu_protocol.request_magic or
        request.version != gpu_protocol.version or
        request.response_paddr < 0x1000)
    {
        _ = userLog("VirtioGpuGl: invalid request\n");
        return;
    }
    if (!mapGpuResponsePage(request.response_paddr)) return;
    const op = std.meta.intToEnum(gpu_protocol.Opcode, request.op) catch {
        writeGpuResponse(.query_caps, seq, .invalid, 0, 0, 0);
        return;
    };
    last_request_seq_seen = seq;
    switch (op) {
        .query_caps => {
            writeGpuResponse(
                .query_caps,
                seq,
                .ok,
                gpuFeatureFlags(),
                gpu_state.capset_id,
                gpu_state.capset_max_version,
            );
        },
        .submit_nop => {
            if ((gpuFeatureFlags() & gpu_protocol.feature_submit_3d) == 0) {
                writeGpuResponse(.submit_nop, seq, .unavailable, 0, 0, 0);
                return;
            }
            if (!submitVirglNoop()) {
                writeGpuResponse(.submit_nop, seq, .io_error, 0, 0, 0);
                return;
            }
            writeGpuResponse(.submit_nop, seq, .ok, 0, 0, 0);
        },
        .submit_3d => {
            if ((gpuFeatureFlags() & gpu_protocol.feature_submit_3d) == 0) {
                writeGpuResponse(.submit_3d, seq, .unavailable, 0, 0, 0);
                return;
            }
            const inline_bytes: usize = request.inline_bytes;
            if (inline_bytes == 0 or inline_bytes > gpu_protocol.request_payload_bytes or inline_bytes != request.arg0) {
                writeGpuResponse(.submit_3d, seq, .invalid, 0, 0, 0);
                return;
            }
            if (!submitVirglInlineCommands(inline_bytes)) {
                writeGpuResponse(.submit_3d, seq, .io_error, 0, 0, 0);
                return;
            }
            writeGpuResponse(.submit_3d, seq, .ok, inline_bytes, 0, 0);
        },
        .prepare_3d => {
            if ((gpuFeatureFlags() & gpu_protocol.feature_present_3d) == 0) {
                writeGpuResponse(.prepare_3d, seq, .unavailable, 0, 0, 0);
                return;
            }
            if (!ensureVirglRenderTarget()) {
                writeGpuResponse(.prepare_3d, seq, .io_error, 0, 0, 0);
                return;
            }
            const surface_id = nextVirglSurfaceId();
            const packed_handles = @as(u64, gpu_state.virgl_resource_id) | (@as(u64, surface_id) << 32);
            writeGpuResponse(.prepare_3d, seq, .ok, gpu_state.width, gpu_state.height, packed_handles);
        },
        .present_3d => {
            if ((gpuFeatureFlags() & gpu_protocol.feature_present_3d) == 0) {
                writeGpuResponse(.present_3d, seq, .unavailable, 0, 0, 0);
                return;
            }
            if (!presentVirglRenderTarget()) {
                writeGpuResponse(.present_3d, seq, .io_error, 0, 0, 0);
                return;
            }
            writeGpuResponse(.present_3d, seq, .ok, gpu_state.width, gpu_state.height, gpu_state.virgl_resource_id);
        },
        .upload_texture_2d => {
            if ((gpuFeatureFlags() & gpu_protocol.feature_texture_2d) == 0) {
                writeGpuResponse(.upload_texture_2d, seq, .unavailable, 0, 0, 0);
                return;
            }
            const resource_id_hint: u32 = @intCast(request.arg0);
            const width: u32 = @intCast(request.arg1 & 0xffff_ffff);
            const height: u32 = @intCast(request.arg1 >> 32);
            const inline_bytes: usize = request.inline_bytes;
            if (inline_bytes == 0 or inline_bytes > gpu_protocol.request_payload_bytes) {
                writeGpuResponse(.upload_texture_2d, seq, .invalid, 0, 0, 0);
                return;
            }
            const resource_id = uploadVirglTexture2d(resource_id_hint, width, height, inline_bytes) orelse {
                writeGpuResponse(.upload_texture_2d, seq, .io_error, 0, 0, 0);
                return;
            };
            writeGpuResponse(.upload_texture_2d, seq, .ok, resource_id, width, height);
        },
        .update_texture_2d => {
            if ((gpuFeatureFlags() & gpu_protocol.feature_texture_2d) == 0) {
                writeGpuResponse(.update_texture_2d, seq, .unavailable, 0, 0, 0);
                return;
            }
            const resource_id: u32 = @intCast(request.arg0 & 0xffff_ffff);
            const x: u32 = @intCast(request.arg0 >> 32);
            const y: u32 = @intCast(request.arg1 & 0xffff);
            const width: u32 = @intCast((request.arg1 >> 16) & 0xffff);
            const height: u32 = @intCast((request.arg1 >> 32) & 0xffff);
            const inline_bytes: usize = request.inline_bytes;
            if (inline_bytes == 0 or inline_bytes > gpu_protocol.request_payload_bytes) {
                writeGpuResponse(.update_texture_2d, seq, .invalid, 0, 0, 0);
                return;
            }
            if (!updateVirglTexture2d(resource_id, x, y, width, height, inline_bytes)) {
                writeGpuResponse(.update_texture_2d, seq, .io_error, 0, 0, 0);
                return;
            }
            writeGpuResponse(.update_texture_2d, seq, .ok, resource_id, width, height);
        },
        .delete_texture_2d => {
            if ((gpuFeatureFlags() & gpu_protocol.feature_texture_2d) == 0) {
                writeGpuResponse(.delete_texture_2d, seq, .unavailable, 0, 0, 0);
                return;
            }
            const resource_id: u32 = @intCast(request.arg0);
            if (resource_id == 0 or request.inline_bytes != 0) {
                writeGpuResponse(.delete_texture_2d, seq, .invalid, 0, 0, 0);
                return;
            }
            if (!deleteVirglTexture2d(resource_id)) {
                writeGpuResponse(.delete_texture_2d, seq, .io_error, 0, 0, 0);
                return;
            }
            writeGpuResponse(.delete_texture_2d, seq, .ok, resource_id, 0, 0);
        },
        .create_app_surface => {
            if ((gpuFeatureFlags() & gpu_protocol.feature_app_surface) == 0) {
                writeGpuResponse(.create_app_surface, seq, .unavailable, 0, 0, 0);
                return;
            }
            const width: u32 = @intCast(request.arg0 & 0xffff_ffff);
            const height: u32 = @intCast(request.arg0 >> 32);
            if (width == 0 or height == 0 or request.inline_bytes != 0) {
                writeGpuResponse(.create_app_surface, seq, .invalid, 0, 0, 0);
                return;
            }
            const packed_handles = createVirglAppSurface(width, height) orelse {
                writeGpuResponse(.create_app_surface, seq, .io_error, 0, 0, 0);
                return;
            };
            writeGpuResponse(.create_app_surface, seq, .ok, width, height, packed_handles);
        },
        .present_test_pattern => {
            if (!presentTestPattern()) {
                writeGpuResponse(.present_test_pattern, seq, .io_error, 0, 0, 0);
                return;
            }
            writeGpuResponse(.present_test_pattern, seq, .ok, gpu_state.width, gpu_state.height, 0);
        },
    }
}

fn handleGpuRequest(request_paddr: u64) void {
    if (!mapGpuRequestPage(request_paddr)) return;
    processMappedGpuRequest();
}

fn initGpuService() bool {
    if (!mapDeviceView()) return false;
    if (commandAuthorize(boot_state.command_token, .virtio_gpu, .gpu_admin) != syscall_ok) {
        _ = userLog("VirtioGpuGl: gpu_admin denied\n");
        return false;
    }
    if (!initQueueMemory()) return false;
    if (!initVirtio()) return false;
    if (gpu_state.virgl_supported) {
        if (!queryCapsetInfo()) return false;
        if (!createVirglContext()) return false;
    }
    return true;
}

pub export fn _start() noreturn {
    _ = userLog("VirtioGpuGl: started\n");
    waitForBootResources();
    if (!initGpuService()) {
        _ = userLog("VirtioGpuGl: init failed\n");
        while (true) asm volatile ("pause");
    }
    writeCfgU64(gpu_bootstrap.driver_status_index, gpu_bootstrap.driver_status_ready);
    _ = userLog("VirtioGpuGl: service ready\n");
    _ = userLog("VirtioGpuGl: ready\n");

    while (true) {
        const received = waitEvent(true, 100);
        if (received >= cap_transfer_abi.transfer_id_min) {
            const request_paddr = acceptCapTransfer(received);
            if (request_paddr >= 0x1000) {
                handleGpuRequest(request_paddr);
            } else {
                _ = userLog("VirtioGpuGl: accept cap transfer failed\n");
            }
        } else if (request_page_paddr_seen != 0) {
            processMappedGpuRequest();
        }
    }
}
