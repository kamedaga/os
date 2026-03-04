const std = @import("std");

const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_log: u64 = 0x9;
const syscall_map_mmio: u64 = 0xB;

const syscall_ok: u64 = 0;

const config_page_va: usize = 0x2000_3000;
const common_page_va: usize = 0x2000_4000;
const notify_page_va: usize = 0x2000_5000;
const isr_page_va: usize = 0x2000_6000;
const device_page_va: usize = 0x2000_7000;
const queue_page0_va: usize = 0x2000_8000;
const queue_page1_va: usize = 0x2000_9000;
const shared_page_va: usize = 0x2000_A000;

const config_magic: u64 = 0x4D4F5553; // "MOUS"
const shared_magic: u64 = 0x4D534852; // "MSHR"

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

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .memory = true }
    );
}

fn allocPage() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_page),
        : .{ .memory = true }
    );
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .memory = true }
    );
}

fn mapMmioPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_mmio),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .memory = true }
    );
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
    const device_page_paddr = readCfgU64(4);
    const common_off: usize = @intCast(readCfgU64(5));
    const notify_off: usize = @intCast(readCfgU64(6));
    const isr_off: usize = @intCast(readCfgU64(7));
    const _device_off: usize = @intCast(readCfgU64(8));
    _ = _device_off;
    const notify_off_multiplier: usize = @intCast(readCfgU64(9));

    if (mapMmioPage(common_page_va, common_page_paddr, true) != syscall_ok) {
        _ = userLog("MouseDriver: map common mmio failed\n");
        while (true) asm volatile ("pause");
    }
    if (mapMmioPage(notify_page_va, notify_page_paddr, true) != syscall_ok) {
        _ = userLog("MouseDriver: map notify mmio failed\n");
        while (true) asm volatile ("pause");
    }
    if (isr_page_paddr != 0) {
        if (mapMmioPage(isr_page_va, isr_page_paddr, false) != syscall_ok) {
            _ = userLog("MouseDriver: map isr mmio failed\n");
            while (true) asm volatile ("pause");
        }
    }
    if (device_page_paddr != 0) {
        _ = mapMmioPage(device_page_va, device_page_paddr, false);
    }

    const common_base = common_page_va + common_off;
    const notify_base = notify_page_va + notify_off;
    const isr_base = if (isr_page_paddr != 0) isr_page_va + isr_off else 0;

    const queue_paddr0 = allocPage();
    const queue_paddr1 = allocPage();
    if (queue_paddr0 < 0x1000 or queue_paddr1 < 0x1000) {
        _ = userLog("MouseDriver: alloc queue pages failed\n");
        while (true) asm volatile ("pause");
    }
    if (queue_paddr1 != queue_paddr0 + 4096) {
        _ = userLog("MouseDriver: queue pages non-contiguous\n");
        while (true) asm volatile ("pause");
    }
    if (mapPage(queue_page0_va, queue_paddr0, true) != syscall_ok or mapPage(queue_page1_va, queue_paddr1, true) != syscall_ok) {
        _ = userLog("MouseDriver: map queue pages failed\n");
        while (true) asm volatile ("pause");
    }
    const queue_bytes: [*]volatile u8 = @ptrFromInt(queue_page0_va);
    var i: usize = 0;
    while (i < queue_region_bytes) : (i += 1) {
        queue_bytes[i] = 0;
    }

    mmioWriteU8(common_base + common_device_status, 0);
    mmioWriteU8(common_base + common_device_status, status_acknowledge | status_driver);
    mmioWriteU32(common_base + common_device_feature_select, 0);
    _ = mmioReadU32(common_base + common_device_feature);
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
    mmioWriteU64(common_base + common_queue_used, queue_paddr0 + queue_used_offset);

    const queue_notify_off = mmioReadU16(common_base + common_queue_notify_off);
    const notify_addr = notify_base + @as(usize, queue_notify_off) * notify_off_multiplier;
    mmioWriteU16(common_base + common_queue_enable, 1);

    var d: u16 = 0;
    while (d < queue_size) : (d += 1) {
        const event_addr = queue_paddr0 + queue_buffers_offset + @as(u64, d) * @sizeOf(VirtioInputEvent);
        const desc = queueDescPtr(d);
        desc.* = .{
            .addr = event_addr,
            .len = @sizeOf(VirtioInputEvent),
            .flags = desc_flag_write,
            .next = 0,
        };
        queuePushAvail(d);
    }
    mmioWriteU16(notify_addr, queue_index_event);
    mmioWriteU8(common_base + common_device_status, mmioReadU8(common_base + common_device_status) | status_driver_ok);
    _ = userLog("MouseDriver: queue ready\n");

    const shared: [*]volatile u64 = @ptrFromInt(shared_page_va);
    if (shared[0] != shared_magic) {
        _ = userLog("MouseDriver: shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }
    const screen_w: i32 = @intCast(shared[1]);
    const screen_h: i32 = @intCast(shared[2]);
    var cursor_x: i32 = @intCast(shared[4]);
    var cursor_y: i32 = @intCast(shared[5]);
    if (screen_w <= 0 or screen_h <= 0) {
        _ = userLog("MouseDriver: invalid shared framebuffer size\n");
        while (true) asm volatile ("pause");
    }

    var last_used_idx: u16 = 0;
    var buttons_mask: u8 = 0;
    var reported_buttons_mask: u8 = 0;
    var accum_dx: i32 = 0;
    var accum_dy: i32 = 0;
    var accum_wheel: i32 = 0;
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
                switch (ev.event_type) {
                    event_type_rel => switch (ev.code) {
                        rel_x => accum_dx +%= value_signed,
                        rel_y => accum_dy +%= value_signed,
                        rel_wheel => accum_wheel +%= value_signed,
                        else => {},
                    },
                    event_type_abs => switch (ev.code) {
                        abs_x => {
                            abs_pos_x = value_signed;
                            abs_dirty = true;
                        },
                        abs_y => {
                            abs_pos_y = value_signed;
                            abs_dirty = true;
                        },
                        else => {},
                    },
                    event_type_key => {
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
                    },
                    event_type_syn => {
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
                                shared[4] = @intCast(cursor_x);
                                shared[5] = @intCast(cursor_y);
                                shared[6] = buttons_mask;
                                shared[8] = @as(u32, @bitCast(accum_wheel));
                                shared[7] +%= 1;

                                var buf: [128]u8 = undefined;
                                const msg = std.fmt.bufPrint(
                                    buf[0..],
                                    "mouse x={d} y={d} dx={d} dy={d} wheel={d} btn={d} absdx={d} absdy={d}\n",
                                    .{ cursor_x, cursor_y, accum_dx, accum_dy, accum_wheel, buttons_mask, abs_dx, abs_dy },
                                ) catch "";
                                _ = userLog(msg);
                                accum_dx = 0;
                                accum_dy = 0;
                                accum_wheel = 0;
                                reported_buttons_mask = buttons_mask;
                                abs_dirty = false;
                            }
                        }
                    },
                    else => {},
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
