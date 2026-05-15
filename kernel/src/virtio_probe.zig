const std = @import("std");
const pci = @import("pci.zig");
const pci_status_cap_list: u16 = 1 << 4;
const pci_cap_id_vendor: u8 = 0x09;
const pci_cap_id_msix: u8 = 0x11;

const virtio_vendor_id: u16 = 0x1AF4;
const virtio_net_legacy_device_id: u16 = 0x1000;
const virtio_net_modern_device_id: u16 = 0x1041;
const virtio_net_subsystem_id: u16 = 1;

const virtio_pci_cap_common_cfg: u8 = 1;
const virtio_pci_cap_notify_cfg: u8 = 2;
const virtio_pci_cap_isr_cfg: u8 = 3;
const virtio_pci_cap_device_cfg: u8 = 4;
const msix_control_table_size_mask: u16 = 0x07FF;
const msix_control_function_mask: u16 = 1 << 14;
const msix_control_enable: u16 = 1 << 15;
const pci_command_memory_space: u16 = 1 << 1;
const pci_command_bus_master: u16 = 1 << 2;

pub const ModernDeviceInfo = struct {
    location: pci.Location,
    vendor_id: u16,
    device_id: u16,
    subsystem_id: u16,
    common_cfg: u64,
    notify_cfg: u64,
    isr_cfg: u64,
    device_cfg: u64,
    notify_off_multiplier: u32,
    queue_count: u16,
    msix_enabled: bool,
};
pub const max_modern_devices: usize = 8;

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

fn mmioWrite32(paddr: u64, value: u32) void {
    const reg: *volatile u32 = @ptrFromInt(paddr);
    reg.* = value;
}

fn findMsixCapability(loc: pci.Location) ?u8 {
    const status = pci.readConfigU16(loc, 0x06);
    if ((status & pci_status_cap_list) == 0) return null;
    var cap_ptr: u8 = pci.readConfigU8(loc, 0x34) & 0xFC;
    var iter: usize = 0;
    while (cap_ptr != 0 and iter < 64) : (iter += 1) {
        const cap_id = pci.readConfigU8(loc, cap_ptr + 0);
        const next_ptr = pci.readConfigU8(loc, cap_ptr + 1) & 0xFC;
        if (cap_id == pci_cap_id_msix) return cap_ptr;
        if (next_ptr == cap_ptr) break;
        cap_ptr = next_ptr;
    }
    return null;
}

fn configureMsixSingleVector(
    write_log: *const fn ([]const u8) void,
    loc: pci.Location,
    device_id: u16,
    vector: u8,
) bool {
    if (device_id != virtio_net_modern_device_id and
        device_id != virtio_net_legacy_device_id and
        pci.readSubsystemId(loc) != virtio_net_subsystem_id) return false;
    const cap_ptr = findMsixCapability(loc) orelse return false;
    const control = pci.readConfigU16(loc, cap_ptr + 2);
    const table_size = (control & msix_control_table_size_mask) + 1;
    if (table_size == 0) return false;
    const table_info = pci.readConfigU32(loc, cap_ptr + 4);
    const bir: u8 = @intCast(table_info & 0x7);
    const table_offset = table_info & 0xFFFF_FFF8;
    const bar_base = readMemBarBase(loc, bir) orelse return false;
    const table_paddr = addU64(bar_base, table_offset) orelse return false;

    mmioWrite32(table_paddr + 0x0, 0xFEE0_0000);
    mmioWrite32(table_paddr + 0x4, 0);
    mmioWrite32(table_paddr + 0x8, vector);
    mmioWrite32(table_paddr + 0xC, 0);

    const command = pci.readConfigU16(loc, 0x04);
    pci.writeConfigU16(loc, 0x04, command | pci_command_memory_space | pci_command_bus_master);
    pci.writeConfigU16(loc, cap_ptr + 2, (control | msix_control_enable) & ~msix_control_function_mask);
    emitFmt(
        write_log,
        "virtio-probe: msix enabled vector=0x{x} table=0x{x}\n",
        .{ vector, table_paddr },
    );
    return true;
}

fn collectModernCaps(
    write_log: *const fn ([]const u8) void,
    loc: pci.Location,
    vendor_id: u16,
    device_id: u16,
    subsystem_id: u16,
) ?ModernDeviceInfo {
    emitFmt(
        write_log,
        "virtio-probe: candidate {x:0>2}:{x:0>2}.{x} did=0x{x} subsys=0x{x}\n",
        .{ loc.bus, loc.device, loc.function, device_id, subsystem_id },
    );

    const status = pci.readConfigU16(loc, 0x06);
    if ((status & pci_status_cap_list) == 0) {
        emit(write_log, "virtio-probe: no PCI capability list\n");
        return null;
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
        return null;
    }

    return .{
        .location = loc,
        .vendor_id = vendor_id,
        .device_id = device_id,
        .subsystem_id = subsystem_id,
        .common_cfg = common_cfg,
        .notify_cfg = notify_cfg,
        .isr_cfg = isr_cfg,
        .device_cfg = device_cfg,
        .notify_off_multiplier = notify_off_multiplier,
        .queue_count = 0,
        .msix_enabled = configureMsixSingleVector(write_log, loc, device_id, 0x41),
    };
}

pub fn probeModernDevices(write_log: *const fn ([]const u8) void) [max_modern_devices]?ModernDeviceInfo {
    var result = [_]?ModernDeviceInfo{null} ** max_modern_devices;
    var result_count: usize = 0;
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
                const info = collectModernCaps(write_log, loc, vendor_id, device_id, subsystem_id) orelse continue;
                if (result_count >= result.len) return result;
                result[result_count] = info;
                result_count += 1;
            }
        }
    }

    return result;
}
