const std = @import("std");

const syscall_log: u64 = 0x9;
const syscall_map_mmio: u64 = 0xB;
const syscall_alloc_map_pages: u64 = 0xC;

const syscall_ok: u64 = 0;

const config_page_va: usize = 0x3C00_2000;
const common_page_va: usize = 0x2000_4000;
const notify_page_va: usize = 0x2000_5000;
const isr_page_va: usize = 0x2000_6000;
const device_page_va: usize = 0x2000_7000;
const queue_page0_va: usize = 0x2000_8000;
const queue_page1_va: usize = 0x2000_9000;
const shared_page_va: usize = 0x3C00_6000;

const config_magic: u64 = 0x4B455942; // "KEYB"
const shared_magic: u64 = 0x4B534852; // "KSHR"

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
const syn_report: u16 = 0x00;

const key_left_shift: u16 = 0x2A;
const key_right_shift: u16 = 0x36;

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

fn keycodeToAscii(code: u16, shift: bool) ?u8 {
    return switch (code) {
        0x02 => if (shift) '!' else '1',
        0x03 => if (shift) '@' else '2',
        0x04 => if (shift) '#' else '3',
        0x05 => if (shift) '$' else '4',
        0x06 => if (shift) '%' else '5',
        0x07 => if (shift) '^' else '6',
        0x08 => if (shift) '&' else '7',
        0x09 => if (shift) '*' else '8',
        0x0A => if (shift) '(' else '9',
        0x0B => if (shift) ')' else '0',
        0x0C => if (shift) '_' else '-',
        0x0D => if (shift) '+' else '=',
        0x0F => '\t',
        0x10 => if (shift) 'Q' else 'q',
        0x11 => if (shift) 'W' else 'w',
        0x12 => if (shift) 'E' else 'e',
        0x13 => if (shift) 'R' else 'r',
        0x14 => if (shift) 'T' else 't',
        0x15 => if (shift) 'Y' else 'y',
        0x16 => if (shift) 'U' else 'u',
        0x17 => if (shift) 'I' else 'i',
        0x18 => if (shift) 'O' else 'o',
        0x19 => if (shift) 'P' else 'p',
        0x1A => if (shift) '{' else '[',
        0x1B => if (shift) '}' else ']',
        0x1C => '\n',
        0x1E => if (shift) 'A' else 'a',
        0x1F => if (shift) 'S' else 's',
        0x20 => if (shift) 'D' else 'd',
        0x21 => if (shift) 'F' else 'f',
        0x22 => if (shift) 'G' else 'g',
        0x23 => if (shift) 'H' else 'h',
        0x24 => if (shift) 'J' else 'j',
        0x25 => if (shift) 'K' else 'k',
        0x26 => if (shift) 'L' else 'l',
        0x27 => if (shift) ':' else ';',
        0x28 => if (shift) '"' else '\'',
        0x29 => if (shift) '~' else '`',
        0x2B => if (shift) '|' else '\\',
        0x2C => if (shift) 'Z' else 'z',
        0x2D => if (shift) 'X' else 'x',
        0x2E => if (shift) 'C' else 'c',
        0x2F => if (shift) 'V' else 'v',
        0x30 => if (shift) 'B' else 'b',
        0x31 => if (shift) 'N' else 'n',
        0x32 => if (shift) 'M' else 'm',
        0x33 => if (shift) '<' else ',',
        0x34 => if (shift) '>' else '.',
        0x35 => if (shift) '?' else '/',
        0x39 => ' ',
        else => null,
    };
}

pub export fn _start() noreturn {
    _ = userLog("KeyboardDriver: started\n");
    if (readCfgU64(0) != config_magic) {
        _ = userLog("KeyboardDriver: config magic mismatch\n");
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
    const shared_page_paddr = readCfgU64(10);
    var queue_paddr0 = readCfgU64(11);
    var queue_paddr1 = readCfgU64(12);

    if (shared_page_paddr < 0x1000) {
        _ = userLog("KeyboardDriver: invalid shared page paddr\n");
        while (true) asm volatile ("pause");
    }

    if (mapMmioPage(common_page_va, common_page_paddr, true) != syscall_ok) {
        _ = userLog("KeyboardDriver: map common mmio failed\n");
        while (true) asm volatile ("pause");
    }
    if (mapMmioPage(notify_page_va, notify_page_paddr, true) != syscall_ok) {
        _ = userLog("KeyboardDriver: map notify mmio failed\n");
        while (true) asm volatile ("pause");
    }
    if (isr_page_paddr != 0) {
        if (mapMmioPage(isr_page_va, isr_page_paddr, false) != syscall_ok) {
            _ = userLog("KeyboardDriver: map isr mmio failed\n");
            while (true) asm volatile ("pause");
        }
    }

    const common_base = common_page_va + common_off;
    const notify_base = notify_page_va + notify_off;
    const isr_base = if (isr_page_paddr != 0) isr_page_va + isr_off else 0;

    if (queue_paddr0 < 0x1000 or queue_paddr1 < 0x1000) {
        var queue_paddrs: [2]u64 = .{ 0, 0 };
        if (allocMapPages(queue_page0_va, 2, true, @intFromPtr(&queue_paddrs)) != syscall_ok) {
            _ = userLog("KeyboardDriver: alloc/map queue pages failed\n");
            while (true) asm volatile ("pause");
        }
        queue_paddr0 = queue_paddrs[0];
        queue_paddr1 = queue_paddrs[1];
    }
    if (queue_paddr0 < 0x1000 or queue_paddr1 < 0x1000) {
        _ = userLog("KeyboardDriver: alloc queue pages failed\n");
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
        _ = userLog("KeyboardDriver: queue_size unsupported\n");
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
    mmioWriteU16(notify_addr, queue_index_event);
    mmioWriteU8(common_base + common_device_status, mmioReadU8(common_base + common_device_status) | status_driver_ok);
    _ = userLog("KeyboardDriver: queue ready\n");

    const shared: [*]volatile u64 = @ptrFromInt(shared_page_va);
    if (shared[0] != shared_magic) {
        _ = userLog("KeyboardDriver: shared magic mismatch\n");
        while (true) asm volatile ("pause");
    }

    var last_used_idx: u16 = 0;
    var pending_code: u16 = 0;
    var pending_value: u32 = 0;
    var has_pending_key = false;
    var pending_ascii: u8 = '?';
    var has_pending_ascii = false;
    var shift_down = false;

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
                switch (ev.event_type) {
                    event_type_key => {
                        if (ev.code == key_left_shift or ev.code == key_right_shift) {
                            shift_down = ev.value != 0;
                        }
                        pending_code = ev.code;
                        pending_value = ev.value;
                        has_pending_key = true;
                        if (ev.value != 0) {
                            if (keycodeToAscii(ev.code, shift_down)) |ascii| {
                                pending_ascii = ascii;
                                has_pending_ascii = true;
                            }
                        }
                    },
                    event_type_syn => {
                        if (ev.code == syn_report and has_pending_key) {
                            var buf: [96]u8 = undefined;
                            const msg = std.fmt.bufPrint(
                                buf[0..],
                                "key code=0x{x} value={d}\n",
                                .{ pending_code, pending_value },
                            ) catch "";
                            _ = userLog(msg);
                            if (has_pending_ascii and pending_value != 0) {
                                shared[2] = pending_ascii;
                                shared[3] = pending_code;
                                shared[4] = pending_value;
                                shared[1] +%= 1;
                            }
                            has_pending_key = false;
                            has_pending_ascii = false;
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
