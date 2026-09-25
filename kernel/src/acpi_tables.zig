const std = @import("std");
const physical_layout = @import("arch/x86_64/physical_layout.zig");

const header_size = 36;
const max_table_size = 1024 * 1024;

fn physicalBytes(address: u64, size: usize) ?[]const u8 {
    if (address == 0 or address >= physical_layout.identity_limit or
        size > physical_layout.identity_limit - address) return null;
    return @as([*]const u8, @ptrFromInt(address))[0..size];
}

fn checksumOk(bytes: []const u8) bool {
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    return sum == 0;
}

fn read32(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}

pub fn tableAt(address: u64, signature: *const [4]u8, minimum: usize) ?[]const u8 {
    const header = physicalBytes(address, header_size) orelse return null;
    if (!std.mem.eql(u8, header[0..4], signature)) return null;
    const size = read32(header, 4);
    if (size < @max(minimum, header_size) or size > max_table_size) return null;
    const bytes = physicalBytes(address, size) orelse return null;
    if (!checksumOk(bytes)) return null;
    return bytes;
}

fn fromRoot(address: u64, xsdt: bool, signature: *const [4]u8, minimum: usize) ?[]const u8 {
    const root = tableAt(address, if (xsdt) "XSDT" else "RSDT", header_size) orelse return null;
    const stride: usize = if (xsdt) 8 else 4;
    if ((root.len - header_size) % stride != 0) return null;
    var offset: usize = header_size;
    while (offset < root.len) : (offset += stride) {
        const child: u64 = if (xsdt)
            std.mem.readInt(u64, root[offset..][0..8], .little)
        else
            read32(root, offset);
        if (tableAt(child, signature, minimum)) |table| return table;
    }
    return null;
}

/// Firmware tables are only dereferenced in the checked identity aperture.
/// This discovers bytes; each consumer validates its own table payload.
pub fn find(rsdp_paddr: u64, signature: *const [4]u8, min_size: usize) ?[]const u8 {
    const legacy = physicalBytes(rsdp_paddr, 20) orelse return null;
    if (!std.mem.eql(u8, legacy[0..8], "RSD PTR ") or !checksumOk(legacy)) return null;
    if (legacy[15] >= 2) {
        if (physicalBytes(rsdp_paddr, 36)) |extended| {
            const size = read32(extended, 20);
            if (size >= 36 and size <= 4096) {
                if (physicalBytes(rsdp_paddr, size)) |rsdp| {
                    if (checksumOk(rsdp)) {
                        const xsdt = std.mem.readInt(u64, rsdp[24..32], .little);
                        if (fromRoot(xsdt, true, signature, min_size)) |table| return table;
                    }
                }
            }
        }
    }
    return fromRoot(read32(legacy, 16), false, signature, min_size);
}

test "ACPI checked physical ranges reject null overflow and inaccessible tables" {
    try std.testing.expect(physicalBytes(0, 36) == null);
    try std.testing.expect(physicalBytes(physical_layout.identity_limit - 16, 36) == null);
    try std.testing.expect(physicalBytes(std.math.maxInt(u64) - 16, 36) == null);
    try std.testing.expect(!checksumOk(&.{ 1, 2, 3 }));
    try std.testing.expect(checksumOk(&.{ 1, 2, 253 }));
}
