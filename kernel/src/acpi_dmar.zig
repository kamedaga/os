const std = @import("std");

const dmar_fixed_bytes: usize = 48;
const remapping_header_bytes: usize = 4;
const drhd_fixed_bytes: usize = 16;
const device_scope_fixed_bytes: usize = 6;

pub const max_drhd_count: usize = 32;
pub const max_device_scopes_per_drhd: usize = 32;
pub const max_device_scope_path_entries: usize = 16;

pub const DevicePath = struct {
    device: u8 = 0,
    function: u8 = 0,
};

pub const DeviceScope = struct {
    scope_type: u8 = 0,
    enumeration_id: u8 = 0,
    start_bus: u8 = 0,
    path: [max_device_scope_path_entries]DevicePath =
        [_]DevicePath{.{}} ** max_device_scope_path_entries,
    path_count: usize = 0,
};

pub const Drhd = struct {
    length: u16 = 0,
    register_base: u64 = 0,
    segment: u16 = 0,
    include_pci_all: bool = false,
    scopes: [max_device_scopes_per_drhd]DeviceScope =
        [_]DeviceScope{.{}} ** max_device_scopes_per_drhd,
    scope_count: usize = 0,
};

pub const DmarInfo = struct {
    /// ACPI encodes this field as one less than the supported address width.
    host_address_width: u8 = 0,
    flags: u8 = 0,
    drhds: [max_drhd_count]Drhd = [_]Drhd{.{}} ** max_drhd_count,
    drhd_count: usize = 0,

    pub fn hostAddressWidthBits(self: DmarInfo) u16 {
        return @as(u16, self.host_address_width) + 1;
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
    TooManyDrhds,
    TruncatedDeviceScope,
    InvalidDeviceScopeLength,
    TooManyDeviceScopes,
    DeviceScopePathTooLong,
};

pub const PhysicalTable = struct {
    paddr: u64,
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

/// Parse a complete DMAR byte sequence without accessing physical memory or
/// producing output. Only type-0 DRHD structures are retained; all other
/// well-formed remapping structures are skipped.
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
            if (result.drhd_count >= result.drhds.len) return error.TooManyDrhds;
            var drhd = Drhd{
                .length = @intCast(structure_len),
                .include_pci_all = (table[offset + 4] & 0x1) != 0,
                .segment = readLe16(table, offset + 6),
                .register_base = readLe64(table, offset + 8),
            };
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
                if (drhd.scope_count >= drhd.scopes.len) return error.TooManyDeviceScopes;
                const path_count = (scope_len - device_scope_fixed_bytes) / 2;
                if (path_count > max_device_scope_path_entries) return error.DeviceScopePathTooLong;
                var scope = DeviceScope{
                    .scope_type = table[scope_offset],
                    .enumeration_id = table[scope_offset + 4],
                    .start_bus = table[scope_offset + 5],
                    .path_count = path_count,
                };
                for (0..path_count) |path_index| {
                    const path_offset = scope_offset + device_scope_fixed_bytes + path_index * 2;
                    scope.path[path_index] = .{
                        .device = table[path_offset],
                        .function = table[path_offset + 1],
                    };
                }
                drhd.scopes[drhd.scope_count] = scope;
                drhd.scope_count += 1;
                scope_offset += scope_len;
            }
            result.drhds[result.drhd_count] = drhd;
            result.drhd_count += 1;
        }
        offset += structure_len;
    }
    return result;
}

/// Locate DMAR through the Limine-provided RSDP, following the same checked
/// RSDP -> XSDT/RSDT traversal used by SMP MADT discovery.
pub fn findDmar(rsdp_addr: u64) ?PhysicalTable {
    const bytes = @import("acpi_tables.zig").find(rsdp_addr, "DMAR", dmar_fixed_bytes) orelse return null;
    return .{ .paddr = @intFromPtr(bytes.ptr), .bytes = bytes };
}
