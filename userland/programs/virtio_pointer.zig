const input_bootstrap = @import("abi_root").input_driver_bootstrap_abi;
const mouse_shared_abi = @import("abi_root").mouse_shared_abi;
const process_abi = @import("abi_root").process_abi;
const user_vm = @import("abi_root").user_vm;

const syscall_map_mmio: u64 = 0xB;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_queue_submit: u64 = 0xE;
const syscall_queue_notify: u64 = 0xF;
const syscall_wait_event: u64 = 0x17;
const syscall_log: u64 = 0x9;
const syscall_ok: u64 = 0;

const config_page_va: usize = @intCast(process_abi.standard_config_target_va);

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

const input_cfg_select: usize = 0;
const input_cfg_subsel: usize = 1;
const input_cfg_size: usize = 2;
const input_cfg_payload: usize = 8;
const input_cfg_select_abs_info: u8 = 0x12;

const queue_index_event: u16 = 0;
const queue_size: u16 = 8;
const queue_used_offset: usize = 4096;
const queue_buffers_offset: usize = 4176;
const desc_flag_write: u16 = 1 << 1;
var common_page_va: usize = 0;
var notify_page_va: usize = 0;
var isr_page_va: usize = 0;
var device_page_va: usize = 0;
var queue_page0_va: usize = 0;
var queue_page1_va: usize = 0;

const MouseSharedPage = mouse_shared_abi.MouseSharedPage;

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

const AbsAxisRange = struct {
    min: i32,
    max: i32,
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

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
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

fn mmioReadU32(addr: usize) u32 {
    const p: *volatile u32 = @ptrFromInt(addr);
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

fn readCfgU64(index: usize) u64 {
    const cfg: [*]const volatile u64 = @ptrFromInt(config_page_va);
    return cfg[index];
}

fn queueRegionPhys(queue_paddr0: u64, queue_paddr1: u64, offset: usize) u64 {
    if (offset < 4096) return queue_paddr0 + @as(u64, @intCast(offset));
    return queue_paddr1 + @as(u64, @intCast(offset - 4096));
}

fn reserveVirtioTargetVas() bool {
    if (common_page_va != 0) return true;
    const mmio_base = user_vm.reservePages(4) orelse return false;
    common_page_va = mmio_base;
    notify_page_va = mmio_base + user_vm.page_bytes;
    isr_page_va = mmio_base + 2 * user_vm.page_bytes;
    device_page_va = mmio_base + 3 * user_vm.page_bytes;
    const queue_base = user_vm.reservePages(2) orelse return false;
    queue_page0_va = queue_base;
    queue_page1_va = queue_base + user_vm.page_bytes;
    return true;
}

fn queueDescPtr(index: u16) *volatile VirtqDesc {
    return @ptrFromInt(queue_page0_va + @as(usize, index) * @sizeOf(VirtqDesc));
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
    queueAvailRingPtr()[@intCast(avail_idx % queue_size)] = desc_id;
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
    return @intCast(@divTrunc(@as(i64, v) * @as(i64, screen_extent - 1), 32767));
}

fn mapAbsRangeToScreen(abs_value: i32, range: AbsAxisRange, screen_extent: i32) i32 {
    if (screen_extent <= 1) return 0;
    if (range.max <= range.min) return clampI32(abs_value, 0, screen_extent - 1);
    const v = clampI32(abs_value, range.min, range.max);
    const numerator = @as(i64, v - range.min) * @as(i64, screen_extent - 1);
    const denominator = @as(i64, range.max - range.min);
    return @intCast(@divTrunc(numerator, denominator));
}

fn growFallbackAbsMax(current: i32, observed: i32) i32 {
    if (observed <= current) return current;
    if (observed <= 65535) return 65535;
    return observed;
}

fn readAbsAxisRange(device_cfg_base: usize, code: u16) ?AbsAxisRange {
    if (device_cfg_base == 0) return null;
    mmioWriteU8(device_cfg_base + input_cfg_select, input_cfg_select_abs_info);
    mmioWriteU8(device_cfg_base + input_cfg_subsel, @intCast(code & 0xFF));
    if (mmioReadU8(device_cfg_base + input_cfg_size) < 8) return null;
    const min: i32 = @bitCast(mmioReadU32(device_cfg_base + input_cfg_payload));
    const max: i32 = @bitCast(mmioReadU32(device_cfg_base + input_cfg_payload + 4));
    if (max <= min) return null;
    return .{ .min = min, .max = max };
}

fn setButton(mask: *u8, bit: u8, down: bool) void {
    if (down) {
        mask.* |= bit;
    } else {
        mask.* &= ~bit;
    }
}

fn initSharedPage(shared: *volatile MouseSharedPage, width: u64, height: u64, pitch: u64) void {
    const bytes: [*]volatile u8 = @ptrFromInt(@intFromPtr(shared));
    mouse_shared_abi.clearPage(bytes[0..mouse_shared_abi.page_bytes]);
    shared.* = .{
        .magic = mouse_shared_abi.magic,
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

pub export fn _start() noreturn {
    _ = userLog("VirtioPointer: started\n");

    if (readCfgU64(0) != input_bootstrap.mouse_config_magic) {
        _ = userLog("VirtioPointer: config magic mismatch\n");
        while (true) asm volatile ("pause");
    }
    if (!reserveVirtioTargetVas()) {
        _ = userLog("VirtioPointer: reserve target VAs failed\n");
        while (true) asm volatile ("pause");
    }

    const common_page_paddr = readCfgU64(1);
    const notify_page_paddr = readCfgU64(2);
    const isr_page_paddr = readCfgU64(3);
    const device_page_paddr = readCfgU64(4);
    const common_off: usize = @intCast(readCfgU64(5));
    const notify_off: usize = @intCast(readCfgU64(6));
    const isr_off: usize = @intCast(readCfgU64(7));
    const device_off: usize = @intCast(readCfgU64(8));
    const notify_off_multiplier: usize = @intCast(readCfgU64(9));
    var iommu_token = readCfgU64(input_bootstrap.iommu_token_index);
    var queue_submit_token = readCfgU64(input_bootstrap.queue_submit_token_index);
    var queue_notify_token = readCfgU64(input_bootstrap.queue_notify_token_index);
    var command_token = readCfgU64(input_bootstrap.command_token_index);

    while (mapMmioPage(common_page_va, common_page_paddr, true) != syscall_ok) _ = waitEvent(false, 1);
    while (mapMmioPage(notify_page_va, notify_page_paddr, true) != syscall_ok) _ = waitEvent(false, 1);
    if (isr_page_paddr != 0) {
        while (mapMmioPage(isr_page_va, isr_page_paddr, false) != syscall_ok) _ = waitEvent(false, 1);
    }
    const device_cfg_base: usize = if (device_page_paddr != 0) blk: {
        while (mapMmioPage(device_page_va, device_page_paddr, true) != syscall_ok) _ = waitEvent(false, 1);
        break :blk device_page_va + device_off;
    } else 0;

    var queue_paddrs: [2]u64 = .{ 0, 0 };
    if (allocMapPages(queue_page0_va, 2, true, @intFromPtr(&queue_paddrs)) != syscall_ok) {
        _ = userLog("VirtioPointer: queue page alloc failed\n");
        while (true) asm volatile ("pause");
    }

    while (iommu_token == 0 or queue_submit_token == 0 or queue_notify_token == 0 or command_token == 0) {
        _ = waitEvent(false, 1);
        iommu_token = readCfgU64(input_bootstrap.iommu_token_index);
        queue_submit_token = readCfgU64(input_bootstrap.queue_submit_token_index);
        queue_notify_token = readCfgU64(input_bootstrap.queue_notify_token_index);
        command_token = readCfgU64(input_bootstrap.command_token_index);
    }

    const common_base = common_page_va + common_off;
    const notify_base = notify_page_va + notify_off;
    const isr_base = if (isr_page_paddr != 0) isr_page_va + isr_off else 0;

    mmioWriteU8(common_base + common_device_status, 0);
    mmioWriteU8(common_base + common_device_status, status_acknowledge | status_driver);
    mmioWriteU32(common_base + common_device_feature_select, 0);
    mmioWriteU32(common_base + common_driver_feature_select, 0);
    mmioWriteU32(common_base + common_driver_feature, 0);
    mmioWriteU32(common_base + common_driver_feature_select, 1);
    mmioWriteU32(common_base + common_driver_feature, 0);

    mmioWriteU16(common_base + common_queue_select, queue_index_event);
    const max_size = mmioReadU16(common_base + common_queue_size);
    if (max_size < queue_size) {
        _ = userLog("VirtioPointer: queue size unsupported\n");
        while (true) asm volatile ("pause");
    }
    mmioWriteU16(common_base + common_queue_size, queue_size);
    mmioWriteU64(common_base + common_queue_desc, queue_paddrs[0]);
    mmioWriteU64(common_base + common_queue_avail, queue_paddrs[0] + (@as(u64, queue_size) * @sizeOf(VirtqDesc)));
    mmioWriteU64(common_base + common_queue_used, queueRegionPhys(queue_paddrs[0], queue_paddrs[1], queue_used_offset));

    const queue_notify_off = mmioReadU16(common_base + common_queue_notify_off);
    const notify_addr = notify_base + @as(usize, queue_notify_off) * notify_off_multiplier;
    mmioWriteU16(common_base + common_queue_enable, 1);

    var d: u16 = 0;
    const queue_buffers_base_paddr = queueRegionPhys(queue_paddrs[0], queue_paddrs[1], queue_buffers_offset);
    while (d < queue_size) : (d += 1) {
        queueDescPtr(d).* = .{
            .addr = queue_buffers_base_paddr + @as(u64, d) * @sizeOf(VirtioInputEvent),
            .len = @sizeOf(VirtioInputEvent),
            .flags = desc_flag_write,
            .next = 0,
        };
        queuePushAvail(d);
    }
    if (queueSubmit(queue_submit_token, queue_index_event) != syscall_ok) {
        _ = userLog("VirtioPointer: queue submit denied\n");
        while (true) asm volatile ("pause");
    }
    if (queueNotify(queue_notify_token, queue_index_event) != syscall_ok) {
        _ = userLog("VirtioPointer: queue notify denied\n");
        while (true) asm volatile ("pause");
    }
    mmioWriteU16(notify_addr, queue_index_event);
    mmioWriteU8(common_base + common_device_status, mmioReadU8(common_base + common_device_status) | status_driver_ok);

    const screen_w_u64 = readCfgU64(17);
    const screen_h_u64 = readCfgU64(18);
    const screen_pitch_u64 = readCfgU64(19);
    const shared: *volatile MouseSharedPage = @ptrFromInt(input_bootstrap.pointer_shared_target_va);
    initSharedPage(shared, screen_w_u64, screen_h_u64, screen_pitch_u64);
    _ = userLog("VirtioPointer: ready\n");

    const screen_w: i32 = @intCast(screen_w_u64);
    const screen_h: i32 = @intCast(screen_h_u64);
    const abs_x_range = readAbsAxisRange(device_cfg_base, abs_x);
    const abs_y_range = readAbsAxisRange(device_cfg_base, abs_y);
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
    var abs_dirty_x = false;
    var abs_dirty_y = false;
    var fallback_abs_x_max: i32 = 32767;
    var fallback_abs_y_max: i32 = 32767;

    while (true) {
        if (isr_base != 0) _ = mmioReadU8(isr_base);
        var needs_notify = false;
        while (last_used_idx != queueUsedIdxPtr().*) {
            const used_elem = queueUsedRingPtr()[@intCast(last_used_idx % queue_size)];
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
                            abs_dirty_x = true;
                        },
                        abs_y => {
                            abs_pos_y = value_signed;
                            abs_dirty_y = true;
                        },
                        else => {},
                    }
                } else if (ev.event_type == event_type_key) {
                    const down = ev.value != 0;
                    switch (ev.code) {
                        btn_left => setButton(&buttons_mask, 1, down),
                        btn_right => setButton(&buttons_mask, 2, down),
                        btn_middle => setButton(&buttons_mask, 4, down),
                        else => {},
                    }
                } else if (ev.event_type == event_type_syn and ev.code == syn_report) {
                    const abs_dirty = abs_dirty_x or abs_dirty_y;
                    if (abs_dirty) {
                        if (abs_pos_x >= screen_w) fallback_abs_x_max = growFallbackAbsMax(fallback_abs_x_max, abs_pos_x);
                        if (abs_pos_y >= screen_h) fallback_abs_y_max = growFallbackAbsMax(fallback_abs_y_max, abs_pos_y);
                        const abs_target_x = if (abs_dirty_x)
                            if (abs_x_range) |range|
                                mapAbsRangeToScreen(abs_pos_x, range, screen_w)
                            else if (abs_pos_x >= 0 and abs_pos_x < screen_w)
                                abs_pos_x
                            else
                                mapAbsRangeToScreen(abs_pos_x, .{ .min = 0, .max = fallback_abs_x_max }, screen_w)
                        else
                            cursor_x;
                        const abs_target_y = if (abs_dirty_y)
                            if (abs_y_range) |range|
                                mapAbsRangeToScreen(abs_pos_y, range, screen_h)
                            else if (abs_pos_y >= 0 and abs_pos_y < screen_h)
                                abs_pos_y
                            else
                                mapAbsRangeToScreen(abs_pos_y, .{ .min = 0, .max = fallback_abs_y_max }, screen_h)
                        else
                            cursor_y;
                        accum_dx = abs_target_x - cursor_x;
                        accum_dy = abs_target_y - cursor_y;
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
                        accum_dx = 0;
                        accum_dy = 0;
                        accum_wheel = 0;
                        reported_buttons_mask = buttons_mask;
                        abs_dirty_x = false;
                        abs_dirty_y = false;
                    }
                }
            }
            queuePushAvail(desc_id);
            last_used_idx +%= 1;
            needs_notify = true;
        }
        if (needs_notify) mmioWriteU16(notify_addr, queue_index_event);
        _ = waitEvent(false, 1);
    }
}

