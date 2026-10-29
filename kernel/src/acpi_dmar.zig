const std = @import("std");
const iova = @import("iova.zig");

const dmar_fixed_bytes: usize = 48;
const remapping_header_bytes: usize = 4;
const drhd_fixed_bytes: usize = 16;
const device_scope_fixed_bytes: usize = 6;

// Views borrow fully validated, immutable firmware bytes. The boot memory
// collector never reclaims ACPI memory; a future reclaim must first end these
// borrows, including Discovery. No iterator owns or copies firmware storage.
pub const DevicePath = extern struct { device: u8 = 0, function: u8 = 0 };
pub const DeviceScope = struct {
    scope_type: u8 = 0,
    enumeration_id: u8 = 0,
    start_bus: u8 = 0,
    path: []const DevicePath = &.{},
    path_count: usize = 0,
};
pub const Scopes = struct {
    bytes: []const u8,
    pub fn next(self: *Scopes) ?DeviceScope {
        if (self.bytes.len == 0) return null;
        const bytes = self.bytes[0..self.bytes[1]];
        self.bytes = self.bytes[bytes.len..];
        const path = std.mem.bytesAsSlice(DevicePath, bytes[6..]);
        return .{ .scope_type = bytes[0], .enumeration_id = bytes[4], .start_bus = bytes[5], .path = path, .path_count = path.len };
    }
};
pub const Drhd = struct {
    length: u16 = 0,
    register_base: u64 = 0,
    segment: u16 = 0,
    include_pci_all: bool = false,
    scope_bytes: []const u8 = &.{},
    scope_count: usize = 0,
    pub fn scopes(self: Drhd) Scopes {
        return .{ .bytes = self.scope_bytes };
    }
    pub fn scopeAt(self: Drhd, index: usize) DeviceScope {
        var it = self.scopes();
        for (0..index) |_| _ = it.next().?;
        return it.next().?;
    }
};
pub const Drhds = struct {
    bytes: []const u8,
    pub fn next(self: *Drhds) ?Drhd {
        while (self.bytes.len != 0) {
            const bytes = self.bytes[0..readLe16(self.bytes, 2)];
            self.bytes = self.bytes[bytes.len..];
            if (readLe16(bytes, 0) != 0) continue;
            var scopes = Scopes{ .bytes = bytes[16..] };
            var count: usize = 0;
            while (scopes.next() != null) count += 1;
            return .{ .length = @intCast(bytes.len), .register_base = readLe64(bytes, 8), .segment = readLe16(bytes, 6), .include_pci_all = bytes[4] & 1 != 0, .scope_bytes = bytes[16..], .scope_count = count };
        }
        return null;
    }
};
pub const DmarInfo = struct {
    dma_window_end: u64 = iova.window_ceiling,
    host_address_width: u8 = 0,
    flags: u8 = 0,
    structures: []const u8 = &.{},
    drhd_count: usize = 0,
    pub fn drhds(self: DmarInfo) Drhds {
        return .{ .bytes = self.structures };
    }
    pub fn rmrrs(self: DmarInfo) Rmrrs {
        return .{ .bytes = self.structures };
    }
    pub fn drhdAt(self: DmarInfo, index: usize) Drhd {
        var it = self.drhds();
        for (0..index) |_| _ = it.next().?;
        return it.next().?;
    }
    pub fn hostAddressWidthBits(self: DmarInfo) u16 {
        return @as(u16, self.host_address_width) + 1;
    }
};

pub const Rmrr = struct {
    segment: u16,
    base: u64,
    last: u64,
    scope_bytes: []const u8,
    pub fn scopes(self: Rmrr) Scopes { return .{ .bytes = self.scope_bytes }; }
};
pub const Rmrrs = struct {
    bytes: []const u8,
    pub fn next(self: *Rmrrs) ?Rmrr {
        while (self.bytes.len != 0) {
            const bytes = self.bytes[0..readLe16(self.bytes, 2)];
            self.bytes = self.bytes[bytes.len..];
            if (readLe16(bytes, 0) != 1) continue;
            return .{ .segment = readLe16(bytes, 6), .base = readLe64(bytes, 8),
                .last = readLe64(bytes, 16), .scope_bytes = bytes[24..] };
        }
        return null;
    }
};

pub const ParseError = error{
    TableTooShort,
    InvalidSignature,
    InvalidTableLength,
    ChecksumMismatch,
    TruncatedStructureHeader,
    InvalidStructureLength,
    StructureOutOfBounds,
    TruncatedDeviceScope,
    InvalidDeviceScopeLength,
    InvalidReservedRange,
};

pub const PhysicalTable = struct {
    paddr: @import("memory/addresses.zig").PhysicalAddress,
    bytes: []const u8,
};

fn readLe16(bytes: []const u8, offset: usize) u16 {
    return @as(u16, bytes[offset]) |
        (@as(u16, bytes[offset + 1]) << 8);
}

fn readLe32(bytes: []const u8, offset: usize) u32 {
    return @as(u32, bytes[offset]) |
        (@as(u32, bytes[offset + 1]) << 8) |
        (@as(u32, bytes[offset + 2]) << 16) |
        (@as(u32, bytes[offset + 3]) << 24);
}

fn readLe64(bytes: []const u8, offset: usize) u64 {
    return @as(u64, readLe32(bytes, offset)) |
        (@as(u64, readLe32(bytes, offset + 4)) << 32);
}

fn checksumOk(bytes: []const u8) bool {
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    return sum == 0;
}

/// Validate the complete table before publishing a borrowed view. RMRRs clip
/// the DMA window without a reservation array; other well-formed structures
/// are skipped. Counts and cursor advances are bounded by the checked u32
/// table length and positive record lengths, not an implementation capacity.
pub fn parseDmar(bytes: []const u8) ParseError!DmarInfo {
    if (bytes.len < dmar_fixed_bytes) return error.TableTooShort;
    if (!std.mem.eql(u8, bytes[0..4], "DMAR")) return error.InvalidSignature;

    const table_len: usize = @intCast(readLe32(bytes, 4));
    if (table_len < dmar_fixed_bytes or table_len > bytes.len) return error.InvalidTableLength;
    const table = bytes[0..table_len];
    if (!checksumOk(table)) return error.ChecksumMismatch;

    var result = DmarInfo{
        .host_address_width = table[36],
        .flags = table[37],
        .structures = table[dmar_fixed_bytes..],
    };
    var offset: usize = dmar_fixed_bytes;
    while (offset < table.len) {
        const remaining = table.len - offset;
        if (remaining < remapping_header_bytes) return error.TruncatedStructureHeader;
        const structure_type = readLe16(table, offset);
        const structure_len: usize = readLe16(table, offset + 2);
        if (structure_len < remapping_header_bytes) return error.InvalidStructureLength;
        if (structure_len > remaining) return error.StructureOutOfBounds;

        if (structure_type == 0) {
            if (structure_len < drhd_fixed_bytes) return error.InvalidStructureLength;
            var scope_offset = offset + drhd_fixed_bytes;
            const structure_end = offset + structure_len;
            while (scope_offset < structure_end) {
                const scope_remaining = structure_end - scope_offset;
                if (scope_remaining < device_scope_fixed_bytes) return error.TruncatedDeviceScope;
                const scope_len: usize = table[scope_offset + 1];
                if (scope_len < device_scope_fixed_bytes or
                    ((scope_len - device_scope_fixed_bytes) & 1) != 0)
                {
                    return error.InvalidDeviceScopeLength;
                }
                if (scope_len > scope_remaining) return error.TruncatedDeviceScope;
                scope_offset += scope_len;
            }
            result.drhd_count += 1;
        } else if (structure_type == 1) {
            // ACPI DMAR RMRR: header, reserved, segment, base, inclusive limit,
            // followed by device scopes. Bounds follow actbl1.h / DMAR format.
            if (structure_len < 24 + 8) return error.InvalidStructureLength;
            const base = readLe64(table, offset + 8);
            const last = readLe64(table, offset + 16);
            if (base > last or base % 4096 != 0 or last % 4096 != 4095)
                return error.InvalidReservedRange;
            var scope = offset + 24;
            const end = offset + structure_len;
            while (scope < end) {
                if (end - scope < 8) return error.TruncatedDeviceScope;
                const length: usize = table[scope + 1];
                if (length < 8 or length % 2 != 0 or length > end - scope or
                    (table[scope] != 1 and table[scope] != 2)) return error.InvalidDeviceScopeLength;
                var path = scope + 6;
                while (path < scope + length) : (path += 2) {
                    if (table[path] >= 32 or table[path + 1] >= 8) return error.InvalidDeviceScopeLength;
                }
                scope += length;
            }
            result.dma_window_end = iova.exclude(iova.window_start, result.dma_window_end, base, last);
        }
        offset += structure_len;
    }
    return result;
}

/// Locate DMAR through the Limine-provided RSDP, following the same checked
/// RSDP -> XSDT/RSDT traversal used by SMP MADT discovery.
pub fn findDmar(rsdp_addr: u64) ?PhysicalTable {
    const table = @import("acpi_tables.zig").findTable(rsdp_addr, "DMAR", dmar_fixed_bytes) orelse return null;
    return .{ .paddr = table.physical, .bytes = table.bytes };
}

fn testChecksum(bytes: []u8) void {
    bytes[9] = 0;
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    bytes[9] = 0 -% sum;
}
fn testTable(allocator: std.mem.Allocator, drhds: usize, scopes: usize, paths: usize) ![]u8 {
    const scope_len = 6 + 2 * paths;
    const drhd_len = 16 + scopes * scope_len;
    const bytes = try allocator.alloc(u8, 48 + drhds * drhd_len);
    @memset(bytes, 0);
    @memcpy(bytes[0..4], "DMAR");
    std.mem.writeInt(u32, bytes[4..8], @intCast(bytes.len), .little);
    for (0..drhds) |index| {
        const block = bytes[48 + index * drhd_len ..][0..drhd_len];
        std.mem.writeInt(u16, block[2..4], @intCast(drhd_len), .little);
        std.mem.writeInt(u64, block[8..16], 0xfed0_0000 + index * 4096, .little);
        for (0..scopes) |scope_index| {
            const scope = block[16 + scope_index * scope_len ..][0..scope_len];
            scope[0] = 1;
            scope[1] = @intCast(scope_len);
            scope[4] = @intCast(scope_index % 256);
            for (0..paths) |path| {
                scope[6 + path * 2] = @intCast(path % 32);
                scope[7 + path * 2] = @intCast(path % 8);
            }
        }
    }
    testChecksum(bytes);
    return bytes;
}

test "DMAR validates and iterates beyond every former output bound" {
    const allocator = std.testing.allocator;
    for ([_]usize{ 31, 32, 33, 257 }) |count| {
        const bytes = try testTable(allocator, count, 0, 0);
        defer allocator.free(bytes);
        const info = try parseDmar(bytes);
        try std.testing.expectEqual(count, info.drhd_count);
        var it = info.drhds();
        for (0..count) |index| try std.testing.expectEqual(@as(u64, 0xfed0_0000) + index * 4096, it.next().?.register_base);
        try std.testing.expect(it.next() == null);
    }
    for ([_]usize{ 31, 32, 33 }) |scopes| {
        for ([_]usize{ 15, 16, 17, 124 }) |paths| {
            const bytes = try testTable(allocator, 1, scopes, paths);
            defer allocator.free(bytes);
            const unit = (try parseDmar(bytes)).drhdAt(0);
            try std.testing.expectEqual(scopes, unit.scope_count);
            var it = unit.scopes();
            for (0..scopes) |index| {
                const scope = it.next().?;
                try std.testing.expectEqual(index, scope.enumeration_id);
                try std.testing.expectEqual(paths, scope.path.len);
                try std.testing.expectEqual((paths - 1) % 32, scope.path[paths - 1].device);
            }
            try std.testing.expect(it.next() == null);
            // Late corruption must reject the whole view, not expose a prefix.
            bytes[bytes.len - (6 + paths * 2) + 1] = 7;
            testChecksum(bytes);
            try std.testing.expectError(error.InvalidDeviceScopeLength, parseDmar(bytes));
        }
    }
}
