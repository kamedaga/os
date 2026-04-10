const std = @import("std");
const device_abi = @import("device_abi.zig");
const input_bootstrap = @import("input_driver_bootstrap_abi.zig");
const protocol = @import("window_protocol.zig");

const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_switch_thread: u64 = 0x5;
const syscall_send_cap: u64 = 0x6;
const syscall_log: u64 = 0x9;
const syscall_map_mmio: u64 = 0xB;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_queue_submit: u64 = 0xE;
const syscall_queue_notify: u64 = 0xF;
const syscall_wait_event: u64 = 0x17;
const syscall_share_cap: u64 = 0x2B;

const syscall_ok: u64 = 0;
const queue_cap_device_input: u64 = 1;
const force_invalid_queue_cap_token = false;

const config_page_va: usize = 0x3C00_2000;
const common_page_va: usize = 0x2000_4000;
const notify_page_va: usize = 0x2000_5000;
const isr_page_va: usize = 0x2000_6000;
const device_page_va: usize = 0x2000_7000;
const queue_page0_va: usize = 0x2000_8000;
const queue_page1_va: usize = 0x2000_9000;
const default_shared_page_va: usize = 0x3C00_3000;

const config_magic = input_bootstrap.mouse_config_magic;
const shared_magic = protocol.mouse_shared_magic;
const endpoint_to_boot_display: u64 = 0x11;
const endpoint_to_spawn_parent: u64 = 0x14;
const MouseSharedPage = protocol.MouseSharedPage;

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

const event_type_syn: u16 = 0x00;
const event_type_key: u16 = 0x01;
const event_type_rel: u16 = 0x02;
const event_type_abs: u16 = 0x03;
const syn_report: u16 = 0x00;
const rel_x: u16 = 0x00;
const rel_y: u16 = 0x01;
const rel_wheel: u16 = 0x08;
const abs_x: u16 = 0x00;
const abs_y: u16 = 0x01;
const btn_left: u16 = 0x110;
const btn_right: u16 = 0x111;
const btn_middle: u16 = 0x112;

const queue_index_event: u16 = 0;
const queue_size: u16 = 8;
const queue_region_bytes: usize = 8192;
const queue_used_offset: usize = 4096;
const queue_buffers_offset: usize = 4176;
const desc_flag_write: u16 = 1 << 1;
const log_mouse_events = false;

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

const VirtioInputEvent = extern struct {
    event_type: u16,
    code: u16,
    value: u32,
};

fn queueRegionPhys(queue_paddr0: u64, queue_paddr1: u64, offset: usize) u64 {
    if (offset < 4096) return queue_paddr0 + @as(u64, @intCast(offset));
    return queue_paddr1 + @as(u64, @intCast(offset - 4096));
}

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
    var buf: [64]u8 = undefined;
    var len: usize = 0;
    while (len < label.len and len < buf.len) : (len += 1) {
        buf[len] = label[len];
    }
    if (len + 3 >= buf.len) return;
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

fn allocPage() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_page),
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

fn sendCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_send_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn shareCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_share_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
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

fn switchThread(thread_index: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_switch_thread),
          [arg0] "{rdi}" (thread_index),
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

fn registerIommuDriver(device: device_abi.DeviceId) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (device_abi.syscall_register_iommu_driver),
          [arg0] "{rdi}" (@intFromEnum(device)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
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

fn readCfgU64(index: usize) u64 {
    const cfg: [*]const volatile u64 = @ptrFromInt(config_page_va);
    return cfg[index];
}

fn writeCfgU64(index: usize, value: u64) void {
    const cfg: [*]volatile u64 = @ptrFromInt(config_page_va);
    cfg[index] = value;
}

fn clearSharedPage(bytes: []volatile u8) void {
    var i: usize = 0;
    while (i < bytes.len) : (i += 1) {
        bytes[i] = 0;
    }
}

fn initializeSharedPage(shared: *volatile MouseSharedPage, width: u64, height: u64, pitch: u64) void {
    const shared_target_va_u64 = readCfgU64(input_bootstrap.shared_target_va_index);
    const shared_page_va: usize = @intCast(if (shared_target_va_u64 != 0) shared_target_va_u64 else default_shared_page_va);
    const shared_bytes: [*]volatile u8 = @ptrFromInt(shared_page_va);
    clearSharedPage(shared_bytes[0..protocol.mouse_shared_page_bytes]);
    shared.* = .{
        .magic = shared_magic,
        .width = width,
        .height = height,
        .pitch = pitch,
        .cursor_x = width / 2,
        .cursor_y = height / 2,
        .buttons = 0,
        .seq = 1,
        .wheel = 0,
        .log_len = 0,
    };
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

fn queueEventPtr(desc_id: u16) *volatile VirtioInputEvent {
    const offset = queue_buffers_offset + @as(usize, desc_id) * @sizeOf(VirtioInputEvent);
    return @ptrFromInt(queue_page0_va + offset);
}

fn queuePushAvail(desc_id: u16) void {
    const avail_idx_ptr = queueAvailIdxPtr();
    const avail_idx = avail_idx_ptr.*;
    const slot: usize = @intCast(avail_idx % queue_size);
    queueAvailRingPtr()[slot] = desc_id;
    avail_idx_ptr.* = avail_idx +% 1;
}

fn clampI32(v: i32, lo: i32, hi: i32) i32 {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

fn mapAbsToScreen(abs_value: i32, screen_extent: i32) i32 {
    if (screen_extent <= 1) return 0;
    const v = clampI32(abs_value, 0, 32767);
    const scaled: i64 = @divTrunc(@as(i64, v) * @as(i64, screen_extent - 1), 32767);
    return @intCast(scaled);
}

pub export fn _start() noreturn {
    _ = userLog("MouseDriver: started\n");

    if (readCfgU64(0) != config_magic) {
        _ = userLog("MouseDriver: config magic mismatch\n");
        while (true) asm volatile ("pause");
    }

    const common_page_paddr = readCfgU64(1);
    const notify_page_paddr = readCfgU64(2);
    const isr_page_paddr = readCfgU64(3);
    const _device_page_paddr = readCfgU64(4);
    const common_off: usize = @intCast(readCfgU64(5));
    const notify_off: usize = @intCast(readCfgU64(6));
    const isr_off: usize = @intCast(readCfgU64(7));
    const _device_off: usize = @intCast(readCfgU64(8));
    _ = _device_page_paddr;
    _ = _device_off;
    const notify_off_multiplier: usize = @intCast(readCfgU64(9));
    var shared_page_paddr = readCfgU64(10);
    const shared_target_va_u64 = readCfgU64(input_bootstrap.shared_target_va_index);
    const shared_page_va: usize = @intCast(if (shared_target_va_u64 != 0) shared_target_va_u64 else default_shared_page_va);
    var queue_paddr0 = readCfgU64(11);
    var queue_paddr1 = readCfgU64(12);
    var queue_submit_token_raw = readCfgU64(13);
    var queue_notify_token_raw = readCfgU64(14);
    const screen_w_u64 = readCfgU64(15);
    const screen_h_u64 = readCfgU64(16);
    const screen_pitch_u64 = readCfgU64(17);
    while (mapMmioPage(common_page_va, common_page_paddr, true) != syscall_ok) {
        _ = waitEvent(false, 1);
        asm volatile ("pause");
    }
    while (mapMmioPage(notify_page_va, notify_page_paddr, true) != syscall_ok) {
        _ = waitEvent(false, 1);
        asm volatile ("pause");
    }
    if (isr_page_paddr != 0) {
        while (mapMmioPage(isr_page_va, isr_page_paddr, false) != syscall_ok) {
            _ = waitEvent(false, 1);
            asm volatile ("pause");
        }
    }

    const common_base = common_page_va + common_off;
    const notify_base = notify_page_va + notify_off;
    const isr_base = if (isr_page_paddr != 0) isr_page_va + isr_off else 0;

    if (registerIommuDriver(.virtio_input) != syscall_ok) {
        _ = userLog("MouseDriver: register iommu driver failed\n");
        while (true) asm volatile ("pause");
    }

    if (queue_paddr0 < 0x1000 or queue_paddr1 < 0x1000) {
        var queue_paddrs: [2]u64 = .{ 0, 0 };
        if (allocMapPages(queue_page0_va, 2, true, @intFromPtr(&queue_paddrs)) != syscall_ok) {
            _ = userLog("MouseDriver: alloc/map queue pages failed\n");
            while (true) asm volatile ("pause");
        }
        queue_paddr0 = queue_paddrs[0];
        queue_paddr1 = queue_paddrs[1];
        writeCfgU64(11, queue_paddr0);
        writeCfgU64(12, queue_paddr1);
    }
    if (queue_paddr0 < 0x1000 or queue_paddr1 < 0x1000) {
        _ = userLog("MouseDriver: alloc queue pages failed\n");
        while (true) asm volatile ("pause");
    }
    while (queue_submit_token_raw == 0 or queue_notify_token_raw == 0) {
        _ = waitEvent(false, 1);
        queue_submit_token_raw = readCfgU64(13);
        queue_notify_token_raw = readCfgU64(14);
        asm volatile ("pause");
    }
    const queue_submit_token = if (force_invalid_queue_cap_token) @as(u64, 0) else queue_submit_token_raw;
    const queue_notify_token = if (force_invalid_queue_cap_token) @as(u64, 0) else queue_notify_token_raw;
    if (queue_submit_token == 0 or queue_notify_token == 0) {
        _ = userLog("MouseDriver: queue cap token missing\n");
        while (true) asm volatile ("pause");
    }
    mmioWriteU8(common_base + common_device_status, 0);
    mmioWriteU8(common_base + common_device_status, status_acknowledge | status_driver);
    mmioWriteU32(common_base + common_device_feature_select, 0);
    mmioWriteU32(common_base + common_driver_feature_select, 0);
    mmioWriteU32(common_base + common_driver_feature, 0);
    mmioWriteU32(common_base + common_driver_feature_select, 1);
    mmioWriteU32(common_base + common_driver_feature, 0);

    mmioWriteU16(common_base + common_queue_select, queue_index_event);
    const max_size = mmioReadU16(common_base + common_queue_size);
    if (max_size == 0 or max_size < queue_size) {
        _ = userLog("MouseDriver: queue_size unsupported\n");
        while (true) asm volatile ("pause");
    }

    mmioWriteU16(common_base + common_queue_size, queue_size);
    mmioWriteU64(common_base + common_queue_desc, queue_paddr0);
    mmioWriteU64(common_base + common_queue_avail, queue_paddr0 + (@as(u64, queue_size) * @sizeOf(VirtqDesc)));
    mmioWriteU64(common_base + common_queue_used, queueRegionPhys(queue_paddr0, queue_paddr1, queue_used_offset));

    const queue_notify_off = mmioReadU16(common_base + common_queue_notify_off);
    const notify_addr = notify_base + @as(usize, queue_notify_off) * notify_off_multiplier;
    mmioWriteU16(common_base + common_queue_enable, 1);

    var d: u16 = 0;
    const queue_buffers_base_paddr = queueRegionPhys(queue_paddr0, queue_paddr1, queue_buffers_offset);
    while (d < queue_size) : (d += 1) {
        const event_addr = queue_buffers_base_paddr + @as(u64, d) * @sizeOf(VirtioInputEvent);
        const desc = queueDescPtr(d);
        desc.* = .{
            .addr = event_addr,
            .len = @sizeOf(VirtioInputEvent),
            .flags = desc_flag_write,
            .next = 0,
        };
        queuePushAvail(d);
    }
    if (queueSubmit(queue_submit_token, queue_cap_device_input, queue_index_event) != syscall_ok) {
        _ = userLog("MouseDriver: queue submit cap denied\n");
        while (true) asm volatile ("pause");
    }
    if (queueNotify(queue_notify_token, queue_cap_device_input, queue_index_event) != syscall_ok) {
        _ = userLog("MouseDriver: queue notify cap denied\n");
        while (true) asm volatile ("pause");
    }
    mmioWriteU16(notify_addr, queue_index_event);
    mmioWriteU8(common_base + common_device_status, mmioReadU8(common_base + common_device_status) | status_driver_ok);
    _ = userLog("MouseDriver: queue ready\n");
    if (shared_page_paddr < 0x1000) {
        shared_page_paddr = allocPage();
        if (shared_page_paddr < 0x1000) {
            _ = userLog("MouseDriver: alloc shared page failed\n");
            while (true) asm volatile ("pause");
        }
        writeCfgU64(10, shared_page_paddr);
    }
    var wait_grant_spin: usize = 0;
    while (mapPage(shared_page_va, shared_page_paddr, true) != syscall_ok) : (wait_grant_spin +%= 1) {
        _ = waitEvent(false, 1);
        asm volatile ("pause");
    }

    const shared: *volatile MouseSharedPage = @ptrFromInt(shared_page_va);
    if (screen_w_u64 == 0 or screen_h_u64 == 0) {
        _ = userLog("MouseDriver: invalid config framebuffer size\n");
        while (true) asm volatile ("pause");
    }
    initializeSharedPage(shared, screen_w_u64, screen_h_u64, screen_pitch_u64);
    if (shareCap(shared_page_paddr, endpoint_to_spawn_parent) != syscall_ok and
        shareCap(shared_page_paddr, endpoint_to_boot_display) != syscall_ok)
    {
        _ = userLog("MouseDriver: send shared cap failed\n");
        while (true) asm volatile ("pause");
    }
    const screen_w: i32 = @intCast(screen_w_u64);
    const screen_h: i32 = @intCast(screen_h_u64);
    var cursor_x: i32 = @intCast(shared.cursor_x);
    var cursor_y: i32 = @intCast(shared.cursor_y);

    var last_used_idx: u16 = 0;
    var buttons_mask: u8 = 0;
    var reported_buttons_mask: u8 = 0;
    var accum_dx: i32 = 0;
    var accum_dy: i32 = 0;
    var accum_wheel: i32 = 0;
    var wheel_total: i32 = 0;
    var abs_pos_x: i32 = 0;
    var abs_pos_y: i32 = 0;
    var abs_dirty = false;

    while (true) {
        if (isr_base != 0) {
            _ = mmioReadU8(isr_base);
        }

        var needs_notify = false;
        while (last_used_idx != queueUsedIdxPtr().*) {
            const slot: usize = @intCast(last_used_idx % queue_size);
            const used_elem = queueUsedRingPtr()[slot];
            const desc_id: u16 = @intCast(used_elem.id & 0xFFFF);
            if (desc_id < queue_size) {
                const ev = queueEventPtr(desc_id).*;
                const value_signed: i32 = @bitCast(ev.value);
                if (ev.event_type == event_type_rel) {
                    switch (ev.code) {
                        rel_x => accum_dx +%= value_signed,
                        rel_y => accum_dy +%= value_signed,
                        rel_wheel => accum_wheel +%= value_signed,
                        else => {},
                    }
                } else if (ev.event_type == event_type_abs) {
                    switch (ev.code) {
                        abs_x => {
                            abs_pos_x = value_signed;
                            abs_dirty = true;
                        },
                        abs_y => {
                            abs_pos_y = value_signed;
                            abs_dirty = true;
                        },
                        else => {},
                    }
                } else if (ev.event_type == event_type_key) {
                    {
                        const down = ev.value != 0;
                        switch (ev.code) {
                            btn_left => {
                                if (down) buttons_mask |= 1 else buttons_mask &= ~@as(u8, 1);
                            },
                            btn_right => {
                                if (down) buttons_mask |= 2 else buttons_mask &= ~@as(u8, 2);
                            },
                            btn_middle => {
                                if (down) buttons_mask |= 4 else buttons_mask &= ~@as(u8, 4);
                            },
                            else => {},
                        }
                    }
                } else if (ev.event_type == event_type_syn) {
                    if (ev.code == syn_report) {
                        var abs_dx: i32 = 0;
                        var abs_dy: i32 = 0;
                        if (abs_dirty) {
                            const abs_target_x = if (abs_pos_x >= 0 and abs_pos_x < screen_w)
                                abs_pos_x
                            else
                                mapAbsToScreen(abs_pos_x, screen_w);
                            const abs_target_y = if (abs_pos_y >= 0 and abs_pos_y < screen_h)
                                abs_pos_y
                            else
                                mapAbsToScreen(abs_pos_y, screen_h);
                            // For absolute devices (tablet), absolute position is authoritative.
                            abs_dx = abs_target_x - cursor_x;
                            abs_dy = abs_target_y - cursor_y;
                            accum_dx = abs_dx;
                            accum_dy = abs_dy;
                        }

                        const moved = accum_dx != 0 or accum_dy != 0 or accum_wheel != 0;
                        const button_changed = buttons_mask != reported_buttons_mask;
                        if (moved or button_changed or abs_dirty) {
                            cursor_x = clampI32(cursor_x +% accum_dx, 0, screen_w - 1);
                            cursor_y = clampI32(cursor_y +% accum_dy, 0, screen_h - 1);
                            wheel_total +%= accum_wheel;
                            shared.cursor_x = @intCast(cursor_x);
                            shared.cursor_y = @intCast(cursor_y);
                            shared.buttons = buttons_mask;
                            shared.wheel = @as(u64, @as(u32, @bitCast(wheel_total)));
                            shared.seq = shared.seq +% 1;

                            if (log_mouse_events) {
                                var buf: [128]u8 = undefined;
                                const msg = std.fmt.bufPrint(
                                    buf[0..],
                                    "mouse x={d} y={d} dx={d} dy={d} wheel_d={d} wheel_t={d} btn={d} absdx={d} absdy={d}\n",
                                    .{ cursor_x, cursor_y, accum_dx, accum_dy, accum_wheel, wheel_total, buttons_mask, abs_dx, abs_dy },
                                ) catch "";
                                _ = userLog(msg);
                            }
                            accum_dx = 0;
                            accum_dy = 0;
                            accum_wheel = 0;
                            reported_buttons_mask = buttons_mask;
                            abs_dirty = false;
                        }
                    }
                }
            }

            queuePushAvail(desc_id);
            last_used_idx +%= 1;
            needs_notify = true;
        }
        if (needs_notify) {
            mmioWriteU16(notify_addr, queue_index_event);
        }
        asm volatile ("pause");
    }
}
