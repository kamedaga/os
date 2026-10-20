const std = @import("std");
const physical_layout = @import("arch/x86_64/physical_layout.zig");

pub const PhysicalAddress = physical_layout.PhysicalAddress;
pub const Table = struct { physical: PhysicalAddress, bytes: []const u8 };

const header_size = 36;

fn physicalBytes(address: u64, size: usize) ?[]const u8 {
    if (@import("builtin").is_test) {
        const fixture = @import("memory/physical_access_test.zig");
        if (fixture.mapping != null) return fixture.slice(u8, address, size);
    }
    if (@import("builtin").is_test) return null;
    return @import("arch/x86_64/platform.zig").mapKernelFirmwareBytes(address, size);
}

fn checksumOk(bytes: []const u8) bool {
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    return sum == 0;
}

fn read32(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}

pub fn tableResultAt(address: u64, signature: *const [4]u8, minimum: usize) ?Table {
    const header = physicalBytes(address, header_size) orelse return null;
    if (!std.mem.eql(u8, header[0..4], signature)) return null;
    const size = read32(header, 4);
    if (size < @max(minimum, header_size)) return null;
    const bytes = physicalBytes(address, size) orelse return null;
    if (!checksumOk(bytes)) return null;
    return .{ .physical = .fromInt(address), .bytes = bytes };
}

fn fromRoot(address: u64, xsdt: bool, signature: *const [4]u8, minimum: usize) ?Table {
    const root = (tableResultAt(address, if (xsdt) "XSDT" else "RSDT", header_size) orelse return null).bytes;
    const stride: usize = if (xsdt) 8 else 4;
    if ((root.len - header_size) % stride != 0) return null;
    var offset: usize = header_size;
    while (offset < root.len) : (offset += stride) {
        const child: u64 = if (xsdt)
            std.mem.readInt(u64, root[offset..][0..8], .little)
        else
            read32(root, offset);
        if (tableResultAt(child, signature, minimum)) |table| return table;
    }
    return null;
}

/// Firmware tables are only dereferenced in the checked high firmware window.
/// This discovers bytes; each consumer validates its own table payload.
pub fn findTable(rsdp_paddr: u64, signature: *const [4]u8, min_size: usize) ?Table {
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
    try std.testing.expect(physicalBytes((@as(u64, 1) << 52) - 16, 36) == null);
    try std.testing.expect(physicalBytes(std.math.maxInt(u64) - 16, 36) == null);
    try std.testing.expect(!checksumOk(&.{ 1, 2, 3 }));
    try std.testing.expect(checksumOk(&.{ 1, 2, 253 }));
}

/// Consumers that need only bytes do not reconstruct a physical address from
/// the mapping: findTable retains the firmware-provided physical token.
pub fn find(rsdp_paddr: u64, signature: *const [4]u8, min_size: usize) ?[]const u8 {
    return (findTable(rsdp_paddr, signature, min_size) orelse return null).bytes;
}

pub const MadtEntries = struct {
    bytes: []const u8,
    offset: usize = 44,

    pub fn next(self: *MadtEntries) error{InvalidEntry}!?[]const u8 {
        if (self.offset == self.bytes.len) return null;
        if (self.offset > self.bytes.len or self.bytes.len - self.offset < 2) return error.InvalidEntry;
        const entry = self.bytes[self.offset..];
        const length = entry[1];
        if (length < 2 or length > entry.len) return error.InvalidEntry;
        // Known records must contain every field used by topology consumers.
        if ((entry[0] == 0 and length < 8) or (entry[0] == 9 and length < 16)) return error.InvalidEntry;
        self.offset += length;
        return entry[0..length];
    }
};

pub fn madtEntries(rsdp: u64) ?MadtEntries {
    return .{ .bytes = find(rsdp, "APIC", 44) orelse return null };
}

pub fn tableAt(address: u64, signature: *const [4]u8, minimum: usize) ?[]const u8 {
    return (tableResultAt(address, signature, minimum) orelse return null).bytes;
}

fn fixtureChecksum(bytes: []u8, index: usize) void {
    bytes[index] = 0;
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    bytes[index] = 0 -% sum;
}

test "ACPI address domains nonidentity traversal retains physical and rejects malformed tables" {
    const fixture = @import("memory/physical_access_test.zig");
    const storage = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096);
    defer std.testing.allocator.free(storage);
    @memset(storage, 0);
    const base: u64 = 0x100000;
    try std.testing.expect(base != @intFromPtr(storage.ptr));
    fixture.mapping = .{ .physical = base, .bytes = storage };
    defer fixture.mapping = null;
    const rsdp = storage[0..36];
    const root = storage[256..300];
    const child = storage[512..564];
    @memcpy(rsdp[0..8], "RSD PTR ");
    rsdp[15] = 2;
    std.mem.writeInt(u32, rsdp[16..20], @intCast(base + 256), .little);
    std.mem.writeInt(u32, rsdp[20..24], 36, .little);
    std.mem.writeInt(u64, rsdp[24..32], base + 256, .little);
    fixtureChecksum(rsdp[0..20], 8);
    fixtureChecksum(rsdp, 32);
    @memcpy(root[0..4], "XSDT");
    std.mem.writeInt(u32, root[4..8], 44, .little);
    std.mem.writeInt(u64, root[36..44], base + 512, .little);
    fixtureChecksum(root, 9);
    @memcpy(child[0..4], "APIC");
    std.mem.writeInt(u32, child[4..8], 52, .little);
    child[45] = 8;
    child[47] = 2;
    child[48] = 1;
    fixtureChecksum(child, 9);
    const result = findTable(base, "APIC", 44).?;
    try std.testing.expectEqual(base + 512, result.physical.toInt());
    try std.testing.expectEqual(@intFromPtr(child.ptr), @intFromPtr(result.bytes.ptr));
    var entries = madtEntries(base).?;
    try std.testing.expectEqual(@as(u8, 2), (try entries.next()).?[3]);
    try std.testing.expect((try entries.next()) == null);
    child[9] +%= 1;
    try std.testing.expect(findTable(base, "APIC", 44) == null);
    fixtureChecksum(child, 9);
    child[0] = 'X';
    fixtureChecksum(child, 9);
    try std.testing.expect(findTable(base, "APIC", 44) == null);
    child[0] = 'A';
    fixtureChecksum(child, 9);
    std.mem.writeInt(u32, child[4..8], 1024 * 1024 + 1, .little);
    try std.testing.expect(findTable(base, "APIC", 44) == null);
    std.mem.writeInt(u32, child[4..8], 52, .little);
    fixtureChecksum(child, 9);
    // A root tail that is not a complete pointer is invalid even with checksum.
    std.mem.writeInt(u32, root[4..8], 43, .little);
    fixtureChecksum(root[0..43], 9);
    try std.testing.expect(findTable(base, "APIC", 44) == null);
    // Legacy RSDT fallback remains supported and retains the physical token.
    @memcpy(root[0..4], "RSDT");
    std.mem.writeInt(u32, root[4..8], 40, .little);
    fixtureChecksum(root[0..40], 9);
    try std.testing.expectEqual(base + 512, findTable(base, "APIC", 44).?.physical.toInt());
    rsdp[8] +%= 1;
    try std.testing.expect(findTable(base, "APIC", 44) == null);
}

test "ACPI address domains MADT entries reject truncated and short known records" {
    var bytes: [64]u8 = @splat(0);
    for ([_]u8{ 0, 1, 7, 21 }) |length| {
        bytes[45] = length;
        var entries: MadtEntries = .{ .bytes = &bytes };
        try std.testing.expectError(error.InvalidEntry, entries.next());
    }
    bytes[44] = 9;
    bytes[45] = 15;
    var short_x2: MadtEntries = .{ .bytes = &bytes };
    try std.testing.expectError(error.InvalidEntry, short_x2.next());
    var tail: MadtEntries = .{ .bytes = bytes[0..45] };
    try std.testing.expectError(error.InvalidEntry, tail.next());
}

test "ACPI header length admits tables larger than one MiB and rejects truncated backing" {
    const fixture = @import("memory/physical_access_test.zig");
    const length = 1024 * 1024 + 4096;
    const bytes = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), length);
    defer std.testing.allocator.free(bytes);
    @memset(bytes, 0);
    @memcpy(bytes[0..4], "TEST");
    std.mem.writeInt(u32, bytes[4..8], length, .little);
    fixtureChecksum(bytes, 9);
    fixture.mapping = .{ .physical = 0x100000, .bytes = bytes };
    defer fixture.mapping = null;
    for ([_]usize{ 1024 * 1024 - 1, 1024 * 1024, length }) |size| {
        std.mem.writeInt(u32, bytes[4..8], @intCast(size), .little);
        fixtureChecksum(bytes[0..size], 9);
        try std.testing.expectEqual(size, tableAt(0x100000, "TEST", 36).?.len);
    }
    bytes[length - 1] = 1;
    try std.testing.expect(tableAt(0x100000, "TEST", 36) == null);
    fixtureChecksum(bytes, 9);
    fixture.mapping.?.bytes = bytes[0 .. length - 1];
    try std.testing.expect(tableAt(0x100000, "TEST", 36) == null);
    fixture.mapping.?.bytes = bytes;
    std.mem.writeInt(u32, bytes[4..8], 35, .little);
    try std.testing.expect(tableAt(0x100000, "TEST", 36) == null);
}
