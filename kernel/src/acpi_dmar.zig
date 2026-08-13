const std = @import("std");

const acpi_header_bytes: usize = 36;
const dmar_fixed_bytes: usize = 48;
const remapping_header_bytes: usize = 4;
const drhd_fixed_bytes: usize = 16;
const max_physical_table_bytes: usize = 1024 * 1024;
const max_rsdp_bytes: usize = 4096;

pub const max_drhd_count: usize = 32;

pub const Drhd = struct {
    register_base: u64 = 0,
    segment: u16 = 0,
    include_pci_all: bool = false,
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
            result.drhds[result.drhd_count] = .{
                .include_pci_all = (table[offset + 4] & 0x1) != 0,
                .segment = readLe16(table, offset + 6),
                .register_base = readLe64(table, offset + 8),
            };
            result.drhd_count += 1;
        }
        offset += structure_len;
    }
    return result;
}

fn physicalBytes(addr: u64, len: usize) []const u8 {
    const ptr: [*]const u8 = @ptrFromInt(addr);
    return ptr[0..len];
}

fn physicalSignatureEquals(addr: u64, expected: []const u8) bool {
    return std.mem.eql(u8, physicalBytes(addr, expected.len), expected);
}

fn physicalReadU32(addr: u64) u32 {
    return readLe32(physicalBytes(addr, 4), 0);
}

fn physicalReadU64(addr: u64) u64 {
    return readLe64(physicalBytes(addr, 8), 0);
}

fn physicalChecksumOk(addr: u64, len: usize) bool {
    return checksumOk(physicalBytes(addr, len));
}

fn findDmarFromRoot(root_addr: u64, xsdt: bool) ?PhysicalTable {
    if (!physicalSignatureEquals(root_addr, if (xsdt) "XSDT" else "RSDT")) return null;
    const root_len: usize = @intCast(physicalReadU32(root_addr + 4));
    if (root_len < acpi_header_bytes or root_len > max_physical_table_bytes) return null;
    if (!physicalChecksumOk(root_addr, root_len)) return null;

    const entry_size: usize = if (xsdt) 8 else 4;
    var offset: usize = acpi_header_bytes;
    while (offset + entry_size <= root_len) : (offset += entry_size) {
        const entry_addr = root_addr + offset;
        const table_addr = if (xsdt)
            physicalReadU64(entry_addr)
        else
            @as(u64, physicalReadU32(entry_addr));
        if (table_addr == 0 or !physicalSignatureEquals(table_addr, "DMAR")) continue;
        const table_len: usize = @intCast(physicalReadU32(table_addr + 4));
        if (table_len < 8 or table_len > max_physical_table_bytes) return null;
        return .{
            .paddr = table_addr,
            .bytes = physicalBytes(table_addr, table_len),
        };
    }
    return null;
}

/// Locate DMAR through the Limine-provided RSDP, following the same checked
/// RSDP -> XSDT/RSDT traversal used by SMP MADT discovery.
pub fn findDmar(rsdp_addr: u64) ?PhysicalTable {
    if (rsdp_addr == 0 or !physicalSignatureEquals(rsdp_addr, "RSD PTR ")) return null;
    if (!physicalChecksumOk(rsdp_addr, 20)) return null;

    const legacy_rsdp = physicalBytes(rsdp_addr, 20);
    if (legacy_rsdp[15] >= 2) {
        const extended_rsdp = physicalBytes(rsdp_addr, 36);
        const rsdp_len: usize = @intCast(readLe32(extended_rsdp, 20));
        const xsdt_addr = readLe64(extended_rsdp, 24);
        if (rsdp_len >= 36 and rsdp_len <= max_rsdp_bytes and xsdt_addr != 0 and
            physicalChecksumOk(rsdp_addr, rsdp_len))
        {
            if (findDmarFromRoot(xsdt_addr, true)) |dmar| return dmar;
        }
    }

    const rsdt_addr = readLe32(legacy_rsdp, 16);
    if (rsdt_addr == 0) return null;
    return findDmarFromRoot(rsdt_addr, false);
}
