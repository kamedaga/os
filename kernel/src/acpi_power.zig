const std = @import("std");
const tables = @import("acpi_tables.zig");
const kernel_log = @import("kernel_log.zig");

pub const Action = enum(u64) { poweroff = 1, reboot = 2 };
const Plan = struct {
    pm1a: u16 = 0,
    pm1b: u16 = 0,
    smi_cmd: u16 = 0,
    acpi_enable: u8 = 0,
    s5a: u16 = 0,
    s5b: u16 = 0,
    reset_port: u16 = 0,
    reset_value: u8 = 0,
};
var plan: Plan = .{};

pub fn staticStart() usize { return @intFromPtr(&plan); }
pub fn staticEnd() usize { return @intFromPtr(&plan) + @sizeOf(Plan); }

fn read16(b: []const u8, offset: usize) u16 {
    return std.mem.readInt(u16, b[offset..][0..2], .little);
}
fn read32(b: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, b[offset..][0..4], .little);
}
fn read64(b: []const u8, offset: usize) u64 {
    return std.mem.readInt(u64, b[offset..][0..8], .little);
}

fn gasIoPort(b: []const u8, offset: usize, width: u8) ?u16 {
    if (b.len < offset + 12 or b[offset] != 1 or b[offset + 2] != 0 or
        b[offset + 1] != width or (b[offset + 3] != 0 and b[offset + 3] != width / 8)) return null;
    const address = read64(b, offset + 4);
    if (address == 0 or address > 0xffff) return null;
    return @intCast(address);
}

fn pm1IoPort(b: []const u8, offset: usize) ?u16 {
    if (b.len < offset + 12 or b[offset] != 1) return null;
    // ACPI defines PM1 control as a group of byte/word accesses; its GAS
    // bit-width, offset and access-size fields are ignored for this register.
    const address = read64(b, offset + 4);
    if (address == 0 or address > 0xffff) return null;
    return @intCast(address);
}

fn amlInteger(bytes: []const u8, pos: *usize) ?u64 {
    if (pos.* >= bytes.len) return null;
    const op = bytes[pos.*];
    pos.* += 1;
    switch (op) {
        0x00 => return 0,
        0x01 => return 1,
        0xff => return std.math.maxInt(u64),
        0x0a => {
            if (pos.* >= bytes.len) return null;
            const value = bytes[pos.*]; pos.* += 1; return value;
        },
        0x0b => {
            if (bytes.len - pos.* < 2) return null;
            const value = read16(bytes, pos.*); pos.* += 2; return value;
        },
        0x0c => {
            if (bytes.len - pos.* < 4) return null;
            const value = read32(bytes, pos.*); pos.* += 4; return value;
        },
        0x0e => {
            if (bytes.len - pos.* < 8) return null;
            const value = read64(bytes, pos.*); pos.* += 8; return value;
        },
        else => return null,
    }
}

fn s5Types(aml: []const u8) ?[2]u16 {
    var result: ?[2]u16 = null;
    var i: usize = 0;
    while (i + 8 < aml.len) : (i += 1) {
        if (aml[i] != 0x08) continue; // NameOp
        var cursor = i + 1;
        if (aml[cursor] == '\\') cursor += 1;
        if (cursor + 5 >= aml.len or !std.mem.eql(u8, aml[cursor..][0..4], "_S5_") or aml[cursor + 4] != 0x12) continue;
        cursor += 5; // PackageOp
        const lead = aml[cursor]; cursor += 1;
        const extra: usize = lead >> 6;
        if (aml.len - cursor < extra) continue;
        var package_bytes: usize = if (extra == 0) lead & 0x3f else lead & 0x0f;
        for (0..extra) |n| package_bytes |= @as(usize, aml[cursor + n]) << @intCast(4 + n * 8);
        cursor += extra;
        if (package_bytes < 1 + extra or package_bytes > aml.len - (cursor - 1 - extra)) continue;
        const end = cursor - 1 - extra + package_bytes;
        if (cursor >= end or aml[cursor] < 2) continue;
        cursor += 1; // NumElements
        const a = amlInteger(aml[0..end], &cursor) orelse continue;
        const b = amlInteger(aml[0..end], &cursor) orelse continue;
        if (a > 7 or b > 7) continue;
        const candidate: [2]u16 = .{ @intCast(a), @intCast(b) };
        // A byte-pattern scan is deliberately fail-closed when more than one
        // plausible namespace definition exists; interpreting AML methods is
        // not a safe kernel shortcut.
        if (result != null) return null;
        result = candidate;
    }
    return result;
}

pub fn prepare(rsdp_paddr: u64) void {
    plan = .{};
    const fadt = tables.find(rsdp_paddr, "FACP", 116) orelse {
        kernel_log.write("acpi-power: FADT unavailable\n"); return;
    };
    const flags = read32(fadt, 112);
    if (fadt.len >= 129 and flags & (1 << 10) != 0) {
        plan.reset_port = gasIoPort(fadt, 116, 8) orelse 0;
        plan.reset_value = fadt[128];
    }
    if (flags & (1 << 20) == 0 and fadt.len >= 90 and fadt[89] >= 2) {
        const legacy_a = read32(fadt, 64);
        const legacy_b = read32(fadt, 68);
        plan.pm1a = if (legacy_a > 0 and legacy_a <= 0xffff) @intCast(legacy_a) else 0;
        plan.pm1b = if (legacy_b > 0 and legacy_b <= 0xffff) @intCast(legacy_b) else 0;
        if (fadt.len >= 196) {
            const extended_a = read64(fadt, 176);
            const extended_b = read64(fadt, 188);
            // A populated X_ field supersedes the legacy address. If its
            // address space is unsupported, never write a stale legacy port.
            if (extended_a != 0) plan.pm1a = pm1IoPort(fadt, 172) orelse 0;
            if (extended_b != 0) {
                plan.pm1b = pm1IoPort(fadt, 184) orelse 0;
                if (plan.pm1b == 0) plan.pm1a = 0;
            }
        }
        const smi_cmd = read32(fadt, 48);
        if (smi_cmd <= 0xffff) plan.smi_cmd = @intCast(smi_cmd);
        plan.acpi_enable = fadt[52];
        const legacy_dsdt = read32(fadt, 40);
        const x_dsdt = if (fadt.len >= 148) read64(fadt, 140) else 0;
        const dsdt = tables.tableAt(if (x_dsdt != 0) x_dsdt else legacy_dsdt, "DSDT", 36);
        if (dsdt) |bytes| {
            if (s5Types(bytes[36..])) |types| {
                plan.s5a = types[0] | 0x100; // preserve zero as a valid S5 type
                plan.s5b = types[1] | 0x100;
            }
        }
    }
    kernel_log.writeFmt("acpi-power: S5={d} reset={d} pm1a=0x{x} reset-port=0x{x}\n", .{
        @intFromBool(plan.pm1a != 0 and plan.s5a != 0), @intFromBool(plan.reset_port != 0), plan.pm1a, plan.reset_port,
    });
}

fn inw(port: u16) u16 {
    return asm volatile ("inw %[port], %[value]"
        : [value] "={ax}" (-> u16),
        : [port] "{dx}" (port),
    );
}
fn outw(port: u16, value: u16) void {
    asm volatile ("outw %[value], %[port]" :: [value] "{ax}" (value), [port] "{dx}" (port));
}
fn outb(port: u16, value: u8) void {
    asm volatile ("outb %[value], %[port]" :: [value] "{al}" (value), [port] "{dx}" (port));
}

pub fn execute(action: Action) bool {
    switch (action) {
        .reboot => {
            if (plan.reset_port == 0) return false;
            kernel_log.write("acpi-power: reboot via FADT RESET_REG\n");
            outb(plan.reset_port, plan.reset_value);
        },
        .poweroff => {
            if (plan.pm1a == 0 or plan.s5a == 0) return false;
            var value = inw(plan.pm1a);
            if (value & 1 == 0) {
                if (plan.smi_cmd == 0 or plan.acpi_enable == 0) return false;
                outb(plan.smi_cmd, plan.acpi_enable);
                var attempts: usize = 0;
                while (attempts < 1_000_000 and inw(plan.pm1a) & 1 == 0) : (attempts += 1) asm volatile ("pause");
                value = inw(plan.pm1a);
                if (value & 1 == 0) return false;
            }
            kernel_log.write("acpi-power: poweroff via FADT PM1 / _S5\n");
            outw(plan.pm1a, (value & ~@as(u16, 0x3c00)) | ((plan.s5a & 7) << 10) | 0x2000);
            if (plan.pm1b != 0) {
                const b = inw(plan.pm1b);
                outw(plan.pm1b, (b & ~@as(u16, 0x3c00)) | ((plan.s5b & 7) << 10) | 0x2000);
            }
        },
    }
    // Firmware should terminate the machine. Returning means it did not.
    return false;
}

test "S5 AML package accepts byte constants and rejects ambiguity" {
    const aml = [_]u8{ 0x08, '\\', '_', 'S', '5', '_', 0x12, 0x07, 0x04, 0x0a, 5, 0x0a, 5, 0, 0 };
    try std.testing.expectEqual([2]u16{ 5, 5 }, s5Types(&aml).?);
    const ambiguous = aml ++ aml;
    try std.testing.expect(s5Types(&ambiguous) == null);
}

test "PM1 GAS accepts I/O and rejects unsupported address space" {
    var gas = [_]u8{ 1, 32, 0, 3, 0x04, 0x06, 0, 0, 0, 0, 0, 0 };
    try std.testing.expectEqual(@as(?u16, 0x604), pm1IoPort(&gas, 0));
    gas[0] = 0;
    try std.testing.expect(pm1IoPort(&gas, 0) == null);
}
