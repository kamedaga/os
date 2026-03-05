const std = @import("std");
const pci = @import("pci.zig");

const pci_status_cap_list: u16 = 1 << 4;
const pci_cap_id_vendor: u8 = 0x09;

const virtio_vendor_id: u16 = 0x1AF4;
const virtio_input_device_modern: u16 = 0x1052;
const virtio_input_subsystem_id: u16 = 0x0012;

const virtio_pci_cap_common_cfg: u8 = 1;
const virtio_pci_cap_notify_cfg: u8 = 2;
const virtio_pci_cap_isr_cfg: u8 = 3;
const virtio_pci_cap_device_cfg: u8 = 4;

const input_cfg_select: usize = 0;
const input_cfg_subsel: usize = 1;
const input_cfg_size: usize = 2;
const input_cfg_payload: usize = 8;

const virtio_input_cfg_select_ev_bits: u8 = 0x11;
const virtio_input_ev_key: u8 = 0x01;
const virtio_input_ev_rel: u8 = 0x02;
const virtio_input_ev_abs: u8 = 0x03;

const input_code_rel_x: u16 = 0x00;
const input_code_rel_y: u16 = 0x01;
const input_code_abs_x: u16 = 0x00;
const input_code_abs_y: u16 = 0x01;
const input_code_key_a: u16 = 0x1E;
const input_code_btn_left: u16 = 0x110;

const InputKind = enum {
    mouse,
    keyboard,
};

pub const InputModernInfo = struct {
    location: pci.Location,
    device_id: u16,
    subsystem_id: u16,
    common_cfg: u64,
    notify_cfg: u64,
    isr_cfg: u64,
    device_cfg: u64,
    notify_off_multiplier: u32,
};

pub const MouseModernInfo = InputModernInfo;
pub const KeyboardModernInfo = InputModernInfo;

fn emit(write_log: *const fn ([]const u8) void, text: []const u8) void {
    write_log(text);
}

fn emitFmt(write_log: *const fn ([]const u8) void, comptime fmt: []const u8, args: anytype) void {
    var buf: [192]u8 = undefined;
    const s = std.fmt.bufPrint(buf[0..], fmt, args) catch return;
    emit(write_log, s);
}

fn addU64(a: u64, b: u64) ?u64 {
    const sum, const overflow = @addWithOverflow(a, b);
    if (overflow != 0) return null;
    return sum;
}

fn readMemBarBase(loc: pci.Location, bar_index: u8) ?u64 {
    if (bar_index >= 6) return null;
    const bar_off: u8 = @intCast(0x10 + bar_index * 4);
    const bar = pci.readConfigU32(loc, bar_off);
    if (bar == 0 or bar == 0xFFFF_FFFF) return null;
    if ((bar & 0x1) != 0) return null;

    const mem_type = (bar >> 1) & 0x3;
    const low_base = @as(u64, bar & 0xFFFF_FFF0);
    if (mem_type == 0x0) return low_base;
    if (mem_type == 0x2) {
        if (bar_index >= 5) return null;
        const high = pci.readConfigU32(loc, bar_off + 4);
        return low_base | (@as(u64, high) << 32);
    }
    return null;
}

fn looksLikeVirtioInput(device_id: u16, subsystem_id: u16) bool {
    return device_id == virtio_input_device_modern or subsystem_id == virtio_input_subsystem_id;
}

fn mmioReadU8(addr: u64) u8 {
    const p: *volatile u8 = @ptrFromInt(addr);
    return p.*;
}

fn mmioWriteU8(addr: u64, value: u8) void {
    const p: *volatile u8 = @ptrFromInt(addr);
    p.* = value;
}

fn readInputBitmapBit(device_cfg: u64, ev_type: u8, code: u16) bool {
    if (device_cfg == 0) return false;
    mmioWriteU8(device_cfg + input_cfg_select, virtio_input_cfg_select_ev_bits);
    mmioWriteU8(device_cfg + input_cfg_subsel, ev_type);
    const size = mmioReadU8(device_cfg + input_cfg_size);
    if (size == 0) return false;

    const byte_index: usize = @intCast(code / 8);
    if (byte_index >= size or byte_index >= 128) return false;
    const bit_index: u3 = @intCast(code & 7);
    const bits = mmioReadU8(device_cfg + input_cfg_payload + byte_index);
    return ((bits >> bit_index) & 1) != 0;
}

fn matchInputKind(device_cfg: u64, want: InputKind, write_log: *const fn ([]const u8) void) bool {
    if (device_cfg == 0) {
        emit(write_log, "virtio-probe: skip candidate (device cfg missing for input classify)\n");
        return false;
    }

    const has_rel_x = readInputBitmapBit(device_cfg, virtio_input_ev_rel, input_code_rel_x);
    const has_rel_y = readInputBitmapBit(device_cfg, virtio_input_ev_rel, input_code_rel_y);
    const has_abs_x = readInputBitmapBit(device_cfg, virtio_input_ev_abs, input_code_abs_x);
    const has_abs_y = readInputBitmapBit(device_cfg, virtio_input_ev_abs, input_code_abs_y);
    const has_key_a = readInputBitmapBit(device_cfg, virtio_input_ev_key, input_code_key_a);
    const has_btn_left = readInputBitmapBit(device_cfg, virtio_input_ev_key, input_code_btn_left);

    const pointer_like = (has_rel_x and has_rel_y) or (has_abs_x and has_abs_y and has_btn_left);
    const keyboard_like = has_key_a and !has_rel_x and !has_rel_y and !has_abs_x and !has_abs_y;

    emitFmt(
        write_log,
        "virtio-probe: classify rel_xy={d} abs_xy={d} key_a={d} btn_left={d}\n",
        .{
            if (has_rel_x and has_rel_y) @as(u8, 1) else @as(u8, 0),
            if (has_abs_x and has_abs_y) @as(u8, 1) else @as(u8, 0),
            if (has_key_a) @as(u8, 1) else @as(u8, 0),
            if (has_btn_left) @as(u8, 1) else @as(u8, 0),
        },
    );
    return switch (want) {
        .mouse => pointer_like,
        .keyboard => keyboard_like,
    };
}

fn probeInputModern(write_log: *const fn ([]const u8) void, want: InputKind) ?InputModernInfo {
    var bus: u16 = 0;
    while (bus < 256) : (bus += 1) {
        var device: u16 = 0;
        while (device < 32) : (device += 1) {
            const func0 = pci.Location{
                .bus = @intCast(bus),
                .device = @intCast(device),
                .function = 0,
            };
            if (pci.readVendorId(func0) == 0xFFFF) continue;
            const header0 = pci.readHeaderType(func0);
            const function_count: u8 = if ((header0 & 0x80) != 0) 8 else 1;

            var function: u8 = 0;
            while (function < function_count) : (function += 1) {
                const loc = pci.Location{
                    .bus = @intCast(bus),
                    .device = @intCast(device),
                    .function = function,
                };
                const vendor_id = pci.readVendorId(loc);
                if (vendor_id == 0xFFFF or vendor_id != virtio_vendor_id) continue;

                const device_id = pci.readDeviceId(loc);
                const subsystem_id = pci.readSubsystemId(loc);
                if (!looksLikeVirtioInput(device_id, subsystem_id)) continue;

                emitFmt(
                    write_log,
                    "virtio-probe: candidate {x:0>2}:{x:0>2}.{x} did=0x{x} subsys=0x{x}\n",
                    .{ loc.bus, loc.device, loc.function, device_id, subsystem_id },
                );

                const status = pci.readConfigU16(loc, 0x06);
                if ((status & pci_status_cap_list) == 0) {
                    emit(write_log, "virtio-probe: no PCI capability list\n");
                    continue;
                }

                var common_cfg: u64 = 0;
                var notify_cfg: u64 = 0;
                var isr_cfg: u64 = 0;
                var device_cfg: u64 = 0;
                var notify_off_multiplier: u32 = 0;

                var cap_ptr: u8 = pci.readConfigU8(loc, 0x34) & 0xFC;
                var iter: usize = 0;
                while (cap_ptr != 0 and iter < 64) : (iter += 1) {
                    const cap_id = pci.readConfigU8(loc, cap_ptr + 0);
                    const next_ptr = pci.readConfigU8(loc, cap_ptr + 1) & 0xFC;

                    if (cap_id == pci_cap_id_vendor) {
                        const cfg_type = pci.readConfigU8(loc, cap_ptr + 3);
                        const bar_index = pci.readConfigU8(loc, cap_ptr + 4);
                        const cap_offset = pci.readConfigU32(loc, cap_ptr + 8);
                        const cap_length = pci.readConfigU32(loc, cap_ptr + 12);
                        emitFmt(
                            write_log,
                            "virtio-probe: cap cfg_type={d} bar={d} off=0x{x} len=0x{x}\n",
                            .{ cfg_type, bar_index, cap_offset, cap_length },
                        );
                        if (cap_length != 0 and bar_index < 6) {
                            const bar_base = readMemBarBase(loc, bar_index) orelse {
                                emit(write_log, "virtio-probe:   skip cap (BAR base unavailable)\n");
                                if (next_ptr == cap_ptr) break;
                                cap_ptr = next_ptr;
                                continue;
                            };
                            const start = addU64(bar_base, cap_offset) orelse {
                                emit(write_log, "virtio-probe:   skip cap (address overflow)\n");
                                if (next_ptr == cap_ptr) break;
                                cap_ptr = next_ptr;
                                continue;
                            };
                            _ = addU64(start, @as(u64, cap_length - 1)) orelse {
                                emit(write_log, "virtio-probe:   skip cap (length overflow)\n");
                                if (next_ptr == cap_ptr) break;
                                cap_ptr = next_ptr;
                                continue;
                            };
                            switch (cfg_type) {
                                virtio_pci_cap_common_cfg => common_cfg = start,
                                virtio_pci_cap_notify_cfg => {
                                    notify_cfg = start;
                                    notify_off_multiplier = pci.readConfigU32(loc, cap_ptr + 16);
                                },
                                virtio_pci_cap_isr_cfg => isr_cfg = start,
                                virtio_pci_cap_device_cfg => device_cfg = start,
                                else => {},
                            }
                        }
                    }

                    if (next_ptr == cap_ptr) break;
                    cap_ptr = next_ptr;
                }

                if (common_cfg == 0 or notify_cfg == 0) {
                    emitFmt(
                        write_log,
                        "virtio-probe: modern caps missing common={d} notify={d}\n",
                        .{ if (common_cfg != 0) @as(u8, 1) else @as(u8, 0), if (notify_cfg != 0) @as(u8, 1) else @as(u8, 0) },
                    );
                    continue;
                }
                if (!matchInputKind(device_cfg, want, write_log)) continue;

                return .{
                    .location = loc,
                    .device_id = device_id,
                    .subsystem_id = subsystem_id,
                    .common_cfg = common_cfg,
                    .notify_cfg = notify_cfg,
                    .isr_cfg = isr_cfg,
                    .device_cfg = device_cfg,
                    .notify_off_multiplier = notify_off_multiplier,
                };
            }
        }
    }

    return null;
}

pub fn probeMouseModern(write_log: *const fn ([]const u8) void) ?MouseModernInfo {
    return probeInputModern(write_log, .mouse);
}

pub fn probeKeyboardModern(write_log: *const fn ([]const u8) void) ?KeyboardModernInfo {
    return probeInputModern(write_log, .keyboard);
}
