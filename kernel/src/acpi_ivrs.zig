const std = @import("std");

const header_bytes: usize = 48;
pub const max_units: usize = 8;
pub const max_rules_per_unit: usize = 512;
pub const max_memory_ranges: usize = 128;
pub const max_special_sources: usize = 128;

pub const SpecialSource = struct { source: u16 = 0, settings: u8 = 0 };

pub const Rule = struct {
    first: u16 = 0,
    last: u16 = 0,
    source: u16 = 0,
    settings: u8 = 0,
    alias: bool = false,

    pub fn sourceFor(self: Rule, device_id: u16) ?u16 {
        if (device_id < self.first or device_id > self.last) return null;
        return if (self.alias) self.source else device_id;
    }
};

pub const Unit = struct {
    block_type: u8 = 0,
    flags: u8 = 0,
    device_id: u16 = 0,
    capability_offset: u16 = 0,
    mmio_base: u64 = 0,
    segment: u16 = 0,
    default_settings: ?u8 = null,
    rules: [max_rules_per_unit]Rule = [_]Rule{.{}} ** max_rules_per_unit,
    rule_count: usize = 0,
    special_sources: [max_special_sources]SpecialSource = [_]SpecialSource{.{}} ** max_special_sources,
    special_count: usize = 0,

    fn ordinaryRuleFor(self: *const Unit, device_id: u16) ?Rule {
        var best: ?Rule = null;
        for (self.rules[0..self.rule_count]) |rule| {
            if (rule.alias or device_id < rule.first or device_id > rule.last) continue;
            // A narrower explicit range is more specific than broad firmware
            // coverage. Equal-width conflicting settings are rejected on input.
            if (best == null or rule.last - rule.first < best.?.last - best.?.first) best = rule;
        }
        return best;
    }

    pub fn ruleFor(self: *const Unit, device_id: u16) ?Rule {
        for (self.rules[0..self.rule_count]) |rule| {
            // An alias names the DMA requester. It wins over ordinary
            // coverage independently of the IVHD entry order; conflicting
            // overlapping aliases are rejected while parsing.
            if (rule.alias and device_id >= rule.first and device_id <= rule.last) return rule;
        }
        if (self.ordinaryRuleFor(device_id)) |rule| return rule;
        if (self.default_settings) |settings| return .{ .first = 0, .last = 0xffff, .settings = settings };
        return null;
    }

    pub fn settingsForSource(self: *const Unit, source: u16) u8 {
        const direct = self.ruleFor(source);
        // A BDF aliased *away* from this index does not configure this DTE.
        var settings: u8 = if (direct != null and direct.?.sourceFor(source).? == source) direct.?.settings else 0;
        // The alias target is the hardware DTE index, even when an ordinary
        // range also covers that ID. Parsing checks conflicting target flags.
        for (self.rules[0..self.rule_count]) |rule| {
            if (rule.alias and rule.source == source) settings = rule.settings;
        }
        for (self.special_sources[0..self.special_count]) |special| {
            if (special.source == source and special.settings != 0) settings = special.settings;
        }
        return settings;
    }
};

pub const MemoryRange = struct {
    flags: u8 = 0,
    first_device: u16 = 0,
    last_device: u16 = 0xffff,
    segment: u16 = 0,
    all_segments: bool = false,
    base: u64 = 0,
    length: u64 = 0,
};

pub const Info = struct {
    units: [max_units]Unit = [_]Unit{.{}} ** max_units,
    unit_count: usize = 0,
    memory_ranges: [max_memory_ranges]MemoryRange = [_]MemoryRange{.{}} ** max_memory_ranges,
    memory_range_count: usize = 0,
};

pub const ParseError = error{
    InvalidSignature,
    InvalidLength,
    InvalidChecksum,
    UnsupportedRevision,
    InvalidBlock,
    InvalidDeviceEntry,
    OverlappingRule,
    TooManyUnits,
    TooManyRules,
    TooManyMemoryRanges,
    TooManySpecialSources,
};

fn le16(bytes: []const u8, offset: usize) u16 {
    return std.mem.readInt(u16, bytes[offset..][0..2], .little);
}

fn le32(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}

fn le64(bytes: []const u8, offset: usize) u64 {
    return std.mem.readInt(u64, bytes[offset..][0..8], .little);
}

fn addRule(unit: *Unit, rule: Rule) ParseError!void {
    if (rule.first > rule.last) return error.InvalidDeviceEntry;
    for (unit.rules[0..unit.rule_count]) |existing| {
        if (std.meta.eql(existing, rule)) return;
        // One requester ID has only one DTE. Disjoint alias ranges may share
        // a target, but cannot demand different settings for that target.
        if (rule.alias and existing.alias and rule.source == existing.source and rule.settings != existing.settings)
            return error.OverlappingRule;
        if (rule.first > existing.last or existing.first > rule.last) continue;
        if (rule.alias and existing.alias) {
            if (rule.source != existing.source or rule.settings != existing.settings) return error.OverlappingRule;
        } else if (rule.alias or existing.alias) {
            // AMD forbids overlapping IVRS entries. Accept the observed
            // firmware pattern only when alias and ordinary DTE flags agree;
            // the alias requester mapping is then unambiguous.
            if (rule.settings != existing.settings) return error.OverlappingRule;
        } else if (rule.settings != existing.settings and
            rule.last - rule.first == existing.last - existing.first)
        {
            return error.OverlappingRule;
        }
    }
    if (unit.rule_count == unit.rules.len) return error.TooManyRules;
    unit.rules[unit.rule_count] = rule;
    unit.rule_count += 1;
}

fn validateAliasTargets(unit: *const Unit) ParseError!void {
    for (unit.rules[0..unit.rule_count]) |rule| {
        if (!rule.alias) continue;
        // An alias target is a DTE index, not another alias hop. A directly
        // described ordinary source must agree on flags; DEV_ALL is merely
        // a fallback and does not override the alias's explicit settings.
        if (unit.ruleFor(rule.source)) |target| {
            if (target.alias and target.source != rule.source) return error.OverlappingRule;
        }
        if (unit.ordinaryRuleFor(rule.source)) |target| {
            if (target.settings != rule.settings) return error.OverlappingRule;
        }
    }
}

fn addSpecial(unit: *Unit, source: u16, settings: u8) ParseError!void {
    for (unit.special_sources[0..unit.special_count]) |*existing| {
        if (existing.source == source) {
            // HPET and IOAPIC may share one requester ID. A zero setting
            // contributes no DTE flags; preserve the nonzero definition,
            // while refusing conflicting nonzero flags for the same source.
            if (existing.settings == 0) existing.settings = settings;
            if (settings != 0 and existing.settings != settings) return error.InvalidDeviceEntry;
            return;
        }
    }
    if (unit.special_count == max_special_sources) return error.TooManySpecialSources;
    unit.special_sources[unit.special_count] = .{ .source = source, .settings = settings };
    unit.special_count += 1;
}

fn parseDeviceEntries(comptime allow_zero_pad: bool, unit: *Unit, bytes: []const u8) ParseError!void {
    var offset: usize = 0;
    var pending: ?Rule = null;
    while (offset < bytes.len) {
        const kind = bytes[offset];
        const length: usize = if (kind < 0x40) 4 else if (kind < 0x80) 8 else if (kind == 0xf0 and unit.block_type == 0x40) blk: {
            if (bytes.len - offset < 22) return error.InvalidDeviceEntry;
            if (bytes[offset + 20] > 2) return error.InvalidDeviceEntry;
            if (bytes[offset + 20] == 0 and bytes[offset + 21] != 0) return error.InvalidDeviceEntry;
            break :blk 22 + @as(usize, bytes[offset + 21]);
        } else return error.InvalidDeviceEntry;
        if (length > bytes.len - offset) return error.InvalidDeviceEntry;
        const entry = bytes[offset..][0..length];
        if ((kind == 0x42 or kind == 0x43) and (entry[4] != 0 or entry[7] != 0)) return error.InvalidDeviceEntry;
        if ((kind == 0x46 or kind == 0x47) and (le32(entry, 4) & 0x7fff_ffff) != 0) return error.InvalidDeviceEntry;
        const id = le16(entry, 1);
        const settings = entry[3];
        if (pending) |start| {
            if (kind != 0x04 or settings != 0) return error.InvalidDeviceEntry;
            var range = start;
            range.last = id;
            try addRule(unit, range);
            pending = null;
        } else switch (kind) {
            0x00, 0x40 => {
                // Older AMD IVRS revisions defined these as zero-filled
                // 4/8-byte alignment pads; newer revisions reserve the codes.
                // Narrow compatibility adds no device mapping, and an 8-byte
                // entry must start on an 8-byte boundary in the aligned IVHD.
                if (!allow_zero_pad or (offset & (length - 1)) != 0) return error.InvalidDeviceEntry;
                for (entry[1..]) |byte| {
                    if (byte != 0) return error.InvalidDeviceEntry;
                }
            },
            0x01 => {
                if (unit.default_settings != null and unit.default_settings.? != settings) return error.InvalidDeviceEntry;
                unit.default_settings = settings;
            },
            0x02, 0x46 => try addRule(unit, .{ .first = id, .last = id, .settings = settings }),
            0x03, 0x47 => pending = .{ .first = id, .settings = settings },
            0x42 => try addRule(unit, .{ .first = id, .last = id, .source = le16(entry, 5), .settings = settings, .alias = true }),
            0x43 => pending = .{ .first = id, .source = le16(entry, 5), .settings = settings, .alias = true },
            // These devices do not acquire a PCI DMA capability, but their
            // system-management message setting is still needed for interrupts.
            0x48 => try addSpecial(unit, le16(entry, 5), settings),
            0xf0 => try addSpecial(unit, id, settings),
            else => return error.InvalidDeviceEntry,
        }
        offset += length;
    }
    if (pending != null) return error.InvalidDeviceEntry;
    try validateAliasTargets(unit);
}

/// The read-only diagnostic uses the production rule policy but rejects PAD4/
/// PAD8, allowing it to isolate padding compatibility from other IVRS rules.
pub fn parseLegacy(bytes: []const u8, info: *Info) ParseError!void {
    return parseImpl(false, bytes, info);
}

/// Parse IVRS without touching MMIO or physical memory. ACPI's byte checksum
/// is checked even when the caller did not use the checked ACPI table walker.
pub fn parse(bytes: []const u8, info: *Info) ParseError!void {
    return parseImpl(true, bytes, info);
}

fn parseImpl(comptime allow_zero_pad: bool, bytes: []const u8, info: *Info) ParseError!void {
    if (bytes.len < header_bytes) return error.InvalidLength;
    if (!std.mem.eql(u8, bytes[0..4], "IVRS")) return error.InvalidSignature;
    const table_length: usize = @intCast(le32(bytes, 4));
    if (table_length < header_bytes or table_length > bytes.len) return error.InvalidLength;
    const table = bytes[0..table_length];
    var checksum: u8 = 0;
    for (table) |byte| checksum +%= byte;
    if (checksum != 0) return error.InvalidChecksum;
    if (table[8] != 1 and table[8] != 2) return error.UnsupportedRevision;
    if (le64(table, 40) != 0) return error.InvalidBlock;
    info.* = .{};

    const SelectedIvhd = struct { offset: usize, kind: u8, base: u64, segment: u16, conflicting_duplicate: bool = false };
    var selected: [max_units]SelectedIvhd = undefined;
    var selected_count: usize = 0;
    // Choose the highest supported IVHD revision for each IOMMU before
    // interpreting entries. A superseded legacy block can have a different
    // entry vocabulary and must not reject a valid newer description.
    var offset: usize = header_bytes;
    while (offset < table.len) {
        if (table.len - offset < 4) return error.InvalidBlock;
        const kind = table[offset];
        const length: usize = le16(table, offset + 2);
        if (length < 4 or length > table.len - offset) return error.InvalidBlock;
        const block = table[offset..][0..length];
        switch (kind) {
            0x10, 0x11, 0x40 => {
                const header_length: usize = if (kind == 0x10) 24 else 40;
                if (length < header_length or (kind == 0x40 and table[8] != 2)) return error.InvalidBlock;
                const base = le64(block, 8);
                const segment = le16(block, 16);
                if (base == 0 or (base & 0xfff) != 0) return error.InvalidBlock;
                var index: usize = 0;
                while (index < selected_count) : (index += 1) {
                    if (selected[index].base == base and selected[index].segment == segment) break;
                }
                if (index == selected_count) {
                    if (selected_count == max_units) return error.TooManyUnits;
                    selected[index] = .{ .offset = offset, .kind = kind, .base = base, .segment = segment };
                    selected_count += 1;
                } else {
                    const previous = table[selected[index].offset..];
                    // Revision changes may supersede entry syntax, but cannot
                    // silently retarget one MMIO unit to another PCI function.
                    if (le16(previous, 4) != le16(block, 4) or le16(previous, 6) != le16(block, 6))
                        return error.InvalidBlock;
                    if (kind > selected[index].kind) {
                        selected[index] = .{ .offset = offset, .kind = kind, .base = base, .segment = segment };
                    } else if (kind == selected[index].kind) {
                        const previous_length: usize = le16(previous, 2);
                        if (previous_length != length or !std.mem.eql(u8, previous[0..previous_length], block))
                            selected[index].conflicting_duplicate = true;
                    }
                }
            },
            0x20, 0x21, 0x22 => {
                if (length != 32) return error.InvalidBlock;
            },
            else => return error.InvalidBlock,
        }
        offset += length;
    }
    if (selected_count == 0) return error.InvalidBlock;
    info.unit_count = selected_count;
    for (selected[0..selected_count], 0..) |choice, index| {
        // Conflicts in obsolete revisions are ignored, but two different
        // highest-revision descriptions of one MMIO unit are ambiguous.
        if (choice.conflicting_duplicate) return error.InvalidBlock;
        const block_length: usize = le16(table, choice.offset + 2);
        const block = table[choice.offset..][0..block_length];
        const header_length: usize = if (choice.kind == 0x10) 24 else 40;
        info.units[index] = .{
            .block_type = choice.kind,
            .flags = block[1],
            .device_id = le16(block, 4),
            .capability_offset = le16(block, 6),
            .mmio_base = choice.base,
            .segment = choice.segment,
        };
        try parseDeviceEntries(allow_zero_pad, &info.units[index], block[header_length..]);
        if (info.units[index].rule_count == 0 and info.units[index].default_settings == null) return error.InvalidBlock;
    }

    offset = header_bytes;
    while (offset < table.len) {
        const kind = table[offset];
        const length: usize = le16(table, offset + 2);
        if (kind == 0x20 or kind == 0x21 or kind == 0x22) {
            const block = table[offset..][0..length];
            if (info.memory_range_count == max_memory_ranges) return error.TooManyMemoryRanges;
            const base = le64(block, 16);
            const span = le64(block, 24);
            if (span == 0 or base > std.math.maxInt(u64) - span) return error.InvalidBlock;
            const first: u16 = if (kind == 0x20) 0 else le16(block, 4);
            const last: u16 = if (kind == 0x20) 0xffff else if (kind == 0x21) first else le16(block, 6);
            if (first > last) return error.InvalidBlock;
            info.memory_ranges[info.memory_range_count] = .{
                .flags = block[1],
                .first_device = first,
                .last_device = last,
                .segment = if (kind == 0x20) 0 else le16(block, 8),
                .all_segments = kind == 0x20,
                .base = base,
                .length = span,
            };
            info.memory_range_count += 1;
        }
        offset += length;
    }
}

pub fn find(rsdp_paddr: u64) ?[]const u8 {
    return @import("acpi_tables.zig").find(rsdp_paddr, "IVRS", header_bytes);
}

test "IVRS parses source aliases, inclusive ranges, and memory definitions" {
    var bytes: [124]u8 = @splat(0);
    @memcpy(bytes[0..4], "IVRS");
    std.mem.writeInt(u32, bytes[4..8], bytes.len, .little);
    bytes[8] = 1;
    bytes[48] = 0x10;
    std.mem.writeInt(u16, bytes[50..52], 44, .little);
    std.mem.writeInt(u64, bytes[56..64], 0xfed8_0000, .little);
    bytes[72..76].* = .{ 0x02, 0x00, 0x01, 0x00 };
    bytes[76..84].* = .{ 0x42, 0x01, 0x01, 0x00, 0x00, 0xe0, 0x00, 0x00 };
    bytes[84..88].* = .{ 0x03, 0x00, 0x02, 0x00 };
    bytes[88..92].* = .{ 0x04, 0xff, 0x02, 0x00 };
    bytes[92] = 0x20;
    std.mem.writeInt(u16, bytes[94..96], 32, .little);
    std.mem.writeInt(u64, bytes[108..116], 0x9000_0000, .little);
    std.mem.writeInt(u64, bytes[116..124], 0x1000, .little);
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    bytes[9] -%= sum;

    const info = try std.testing.allocator.create(Info);
    defer std.testing.allocator.destroy(info);
    try parse(&bytes, info);
    const legacy = try std.testing.allocator.create(Info);
    defer std.testing.allocator.destroy(legacy);
    try parseLegacy(&bytes, legacy);
    try std.testing.expect(std.meta.eql(info.*, legacy.*));
    try std.testing.expectEqual(@as(usize, 1), info.unit_count);
    try std.testing.expectEqual(@as(?u16, 0x0100), info.units[0].ruleFor(0x0100).?.sourceFor(0x0100));
    try std.testing.expectEqual(@as(?u16, 0x00e0), info.units[0].ruleFor(0x0101).?.sourceFor(0x0101));
    try std.testing.expectEqual(@as(?u16, 0x02ff), info.units[0].ruleFor(0x02ff).?.sourceFor(0x02ff));
    try std.testing.expect(info.units[0].ruleFor(0x0300) == null);
    try std.testing.expectEqual(@as(usize, 1), info.memory_range_count);
    try std.testing.expectEqual(@as(u64, 0x9000_0000), info.memory_ranges[0].base);

    bytes[9] +%= 1;
    try std.testing.expectError(error.InvalidChecksum, parse(&bytes, info));
}

test "IVRS all-device setting is a fallback for explicit selections" {
    var bytes: [80]u8 = @splat(0);
    @memcpy(bytes[0..4], "IVRS");
    std.mem.writeInt(u32, bytes[4..8], bytes.len, .little);
    bytes[8] = 1;
    bytes[48] = 0x10;
    std.mem.writeInt(u16, bytes[50..52], 32, .little);
    std.mem.writeInt(u64, bytes[56..64], 0xfed8_0000, .little);
    bytes[72..76].* = .{ 0x01, 0, 0, 0x10 };
    bytes[76..80].* = .{ 0x02, 0x20, 0x01, 0x20 };
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    bytes[9] -%= sum;

    const info = try std.testing.allocator.create(Info);
    defer std.testing.allocator.destroy(info);
    try parse(&bytes, info);
    try std.testing.expectEqual(@as(u8, 0x10), info.units[0].ruleFor(0x0100).?.settings);
    try std.testing.expectEqual(@as(u8, 0x20), info.units[0].ruleFor(0x0120).?.settings);
}

fn setTestChecksum(bytes: []u8) void {
    bytes[9] = 0;
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    bytes[9] -%= sum;
}

fn paddedIvhd() [88]u8 {
    var bytes: [88]u8 = @splat(0);
    @memcpy(bytes[0..4], "IVRS");
    std.mem.writeInt(u32, bytes[4..8], bytes.len, .little);
    bytes[8] = 1;
    bytes[48] = 0x10;
    std.mem.writeInt(u16, bytes[50..52], 40, .little);
    std.mem.writeInt(u64, bytes[56..64], 0xfed8_0000, .little);
    bytes[72..76].* = .{ 0x02, 0x00, 0x01, 0x10 };
    bytes[76..80].* = .{ 0x00, 0x00, 0x00, 0x00 };
    bytes[80..88].* = .{ 0x40, 0, 0, 0, 0, 0, 0, 0 };
    setTestChecksum(&bytes);
    return bytes;
}

test "IVRS accepts only aligned zero-filled PAD4/PAD8 without adding rules" {
    var bytes = paddedIvhd();
    const info = try std.testing.allocator.create(Info);
    defer std.testing.allocator.destroy(info);
    try std.testing.expectError(error.InvalidDeviceEntry, parseLegacy(&bytes, info));
    try parse(&bytes, info);
    try std.testing.expectEqual(@as(usize, 1), info.unit_count);
    try std.testing.expectEqual(@as(usize, 1), info.units[0].rule_count);
    try std.testing.expectEqual(@as(?u16, 0x0100), info.units[0].ruleFor(0x0100).?.sourceFor(0x0100));
    try std.testing.expect(info.units[0].ruleFor(0x0101) == null);
    try std.testing.expectEqual(@as(usize, 0), info.units[0].special_count);

    bytes[78] = 1;
    setTestChecksum(&bytes);
    try std.testing.expectError(error.InvalidDeviceEntry, parse(&bytes, info));

    bytes = paddedIvhd();
    bytes[86] = 1;
    setTestChecksum(&bytes);
    try std.testing.expectError(error.InvalidDeviceEntry, parse(&bytes, info));

    bytes = paddedIvhd();
    bytes[80] = 0x41;
    setTestChecksum(&bytes);
    try std.testing.expectError(error.InvalidDeviceEntry, parse(&bytes, info));
}

test "IVRS rejects truncated or misaligned PAD8 and pads inside a pending range" {
    const info = try std.testing.allocator.create(Info);
    defer std.testing.allocator.destroy(info);

    var truncated = paddedIvhd();
    std.mem.writeInt(u32, truncated[4..8], 84, .little);
    std.mem.writeInt(u16, truncated[50..52], 36, .little);
    setTestChecksum(truncated[0..84]);
    try std.testing.expectError(error.InvalidDeviceEntry, parse(truncated[0..84], info));

    var misaligned: [84]u8 = @splat(0);
    @memcpy(misaligned[0..76], truncated[0..76]);
    misaligned[76] = 0x40;
    setTestChecksum(&misaligned);
    try std.testing.expectError(error.InvalidDeviceEntry, parse(&misaligned, info));

    var pending: [84]u8 = @splat(0);
    @memcpy(pending[0..84], truncated[0..84]);
    pending[72..76].* = .{ 0x03, 0x00, 0x01, 0x10 };
    pending[80..84].* = .{ 0x04, 0xff, 0x01, 0x00 };
    setTestChecksum(&pending);
    try std.testing.expectError(error.InvalidDeviceEntry, parse(&pending, info));
}

// Transcribed from the real B550 IVRS photograph. The ACPI checksum and FNV
// below guard against silently changing the captured firmware evidence.
const observed_b550_ivrs = [_]u8{
    0x49, 0x56, 0x52, 0x53, 0xd0, 0x00, 0x00, 0x00, 0x02, 0xb2, 0x41, 0x4d, 0x44, 0x20, 0x20, 0x00,
    0x41, 0x6d, 0x64, 0x54, 0x61, 0x62, 0x6c, 0x65, 0x01, 0x00, 0x00, 0x00, 0x41, 0x4d, 0x44, 0x20,
    0x01, 0x00, 0x00, 0x00, 0x41, 0x30, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0xb0, 0x48, 0x00, 0x02, 0x00, 0x40, 0x00, 0x00, 0x00, 0x50, 0xfd, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x2e, 0x8f, 0x04, 0x80, 0x03, 0x08, 0x00, 0x00, 0x04, 0xfe, 0xff, 0x00,
    0x43, 0x00, 0xff, 0x00, 0x00, 0xa5, 0x00, 0x00, 0x04, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x00, 0x02, 0x48, 0x00, 0x00, 0xd7, 0x19, 0xa0, 0x00, 0x01,
    0x48, 0x00, 0x00, 0x00, 0x1a, 0x01, 0x00, 0x01, 0x11, 0xb0, 0x58, 0x00, 0x02, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x50, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x04, 0x00,
    0x5a, 0x4a, 0x29, 0x22, 0xef, 0x77, 0x8f, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x08, 0x00, 0x00, 0x04, 0xfe, 0xff, 0x00, 0x43, 0x00, 0xff, 0x00, 0x00, 0xa5, 0x00, 0x00,
    0x04, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x00, 0x02,
    0x48, 0x00, 0x00, 0xd7, 0x19, 0xa0, 0x00, 0x01, 0x48, 0x00, 0x00, 0x00, 0x1a, 0x01, 0x00, 0x01,
};

test "real IVRS overlapping alias is resolved without firmware-order dependence" {
    try std.testing.expectEqual(@as(usize, 208), observed_b550_ivrs.len);
    var checksum: u8 = 0;
    var fingerprint: u32 = 0x811c9dc5;
    for (observed_b550_ivrs) |byte| {
        checksum +%= byte;
        fingerprint = (fingerprint ^ byte) *% 0x01000193;
    }
    try std.testing.expectEqual(@as(u8, 0), checksum);
    try std.testing.expectEqual(@as(u32, 0xb06c55b7), fingerprint);

    const info = try std.testing.allocator.create(Info);
    defer std.testing.allocator.destroy(info);
    try std.testing.expectError(error.InvalidDeviceEntry, parseLegacy(&observed_b550_ivrs, info));
    try parse(&observed_b550_ivrs, info);
    try std.testing.expectEqual(@as(usize, 1), info.unit_count);
    const unit = &info.units[0];
    try std.testing.expectEqual(@as(u8, 0x11), unit.block_type);
    try std.testing.expectEqual(@as(u64, 0xfd50_0000), unit.mmio_base);
    try std.testing.expectEqual(@as(usize, 2), unit.rule_count);
    try std.testing.expectEqual(@as(u16, 0x0008), unit.rules[0].first);
    try std.testing.expectEqual(@as(u16, 0xfffe), unit.rules[0].last);
    try std.testing.expectEqual(@as(u16, 0xff00), unit.rules[1].first);
    try std.testing.expectEqual(@as(u16, 0xffff), unit.rules[1].last);
    try std.testing.expectEqual(@as(?u16, 0x0008), unit.ruleFor(0x0008).?.sourceFor(0x0008));
    try std.testing.expectEqual(@as(?u16, 0xfeff), unit.ruleFor(0xfeff).?.sourceFor(0xfeff));
    try std.testing.expectEqual(@as(?u16, 0x00a5), unit.ruleFor(0xff00).?.sourceFor(0xff00));
    try std.testing.expectEqual(@as(?u16, 0x00a5), unit.ruleFor(0xfffe).?.sourceFor(0xfffe));
    try std.testing.expectEqual(@as(?u16, 0x00a5), unit.ruleFor(0xffff).?.sourceFor(0xffff));
    try std.testing.expect(unit.ruleFor(0x0007) == null);
    try std.testing.expectEqual(unit.rules[0].settings, unit.rules[1].settings);
    try std.testing.expectEqual(@as(usize, 2), unit.special_count);
    try std.testing.expectEqual(@as(u16, 0x00a0), unit.special_sources[0].source);
    try std.testing.expectEqual(@as(u8, 0xd7), unit.special_sources[0].settings);
    try std.testing.expectEqual(@as(u16, 0x0001), unit.special_sources[1].source);
}

test "IVHD selection ignores superseded entry vocabularies but checks unit identity" {
    const info = try std.testing.allocator.create(Info);
    defer std.testing.allocator.destroy(info);
    var table = observed_b550_ivrs;
    table[72] = 0x41; // unsupported entry in superseded type 0x10 block
    setTestChecksum(&table);
    try parse(&table, info);
    try std.testing.expectEqual(@as(u8, 0x11), info.units[0].block_type);
    try std.testing.expectEqual(@as(?u16, 0x00a5), info.units[0].ruleFor(0xff00).?.sourceFor(0xff00));

    table = observed_b550_ivrs;
    table[52] = 0x03; // same MMIO and segment, different IOMMU BDF
    setTestChecksum(&table);
    try std.testing.expectError(error.InvalidBlock, parse(&table, info));

    var old_duplicates: [280]u8 = undefined;
    @memcpy(old_duplicates[0..120], observed_b550_ivrs[0..120]);
    @memcpy(old_duplicates[120..192], observed_b550_ivrs[48..120]);
    @memcpy(old_duplicates[192..280], observed_b550_ivrs[120..208]);
    old_duplicates[157] = 0xa6; // conflicting old 0x10 before valid 0x11
    std.mem.writeInt(u32, old_duplicates[4..8], old_duplicates.len, .little);
    setTestChecksum(&old_duplicates);
    try parse(&old_duplicates, info);
    try std.testing.expectEqual(@as(u8, 0x11), info.units[0].block_type);

    var duplicate: [296]u8 = undefined;
    @memcpy(duplicate[0..208], &observed_b550_ivrs);
    @memcpy(duplicate[208..296], observed_b550_ivrs[120..208]);
    std.mem.writeInt(u32, duplicate[4..8], duplicate.len, .little);
    setTestChecksum(&duplicate);
    try parse(&duplicate, info);
    duplicate[261] = 0xa6; // same-kind duplicate aliases another requester
    setTestChecksum(&duplicate);
    try std.testing.expectError(error.InvalidBlock, parse(&duplicate, info));
    duplicate = undefined;
    @memcpy(duplicate[0..208], &observed_b550_ivrs);
    @memcpy(duplicate[208..296], observed_b550_ivrs[120..208]);
    std.mem.writeInt(u32, duplicate[4..8], duplicate.len, .little);
    duplicate[212] = 0x03; // same revision and MMIO, conflicting IOMMU BDF
    setTestChecksum(&duplicate);
    try std.testing.expectError(error.InvalidBlock, parse(&duplicate, info));
}

test "overlap resolution is generic, order independent, and rejects ambiguous sources" {
    var unit = Unit{};
    const broad = Rule{ .first = 0x0008, .last = 0xfffe, .settings = 0x10 };
    const alias = Rule{ .first = 0xff00, .last = 0xffff, .source = 0x00a5, .settings = 0x10, .alias = true };
    try addRule(&unit, broad);
    try addRule(&unit, alias);
    try std.testing.expectEqual(@as(usize, 2), unit.rule_count);
    try std.testing.expectEqual(@as(u16, 0xfffe), unit.rules[0].last);
    try std.testing.expectEqual(@as(?u16, 0x00a5), unit.ruleFor(0xff00).?.sourceFor(0xff00));
    try std.testing.expectEqual(@as(u8, 0x10), unit.settingsForSource(0x00a5));

    try std.testing.expectError(error.OverlappingRule, addRule(&unit, .{
        .first = 0xff10,
        .last = 0xffff,
        .source = 0x00a6,
        .settings = 0x10,
        .alias = true,
    }));
    try std.testing.expectEqual(@as(usize, 2), unit.rule_count);

    unit = .{};
    try addRule(&unit, broad);
    var different = alias;
    different.settings = 0x20;
    try std.testing.expectError(error.OverlappingRule, addRule(&unit, different));
    try std.testing.expectEqual(@as(u16, 0xfffe), unit.rules[0].last);

    unit = .{};
    try addRule(&unit, alias);
    try addRule(&unit, broad);
    try std.testing.expectEqual(@as(?u16, 0x00a5), unit.ruleFor(0xff00).?.sourceFor(0xff00));
    try std.testing.expectEqual(@as(?u16, 0xfeff), unit.ruleFor(0xfeff).?.sourceFor(0xfeff));

    unit = .{};
    try addRule(&unit, .{ .first = 0x0100, .last = 0x0300, .settings = 0x20 });
    try addRule(&unit, .{ .first = 0x0180, .last = 0x0280, .source = 0x0050, .settings = 0x20, .alias = true });
    try std.testing.expectEqual(@as(?u16, 0x017f), unit.ruleFor(0x017f).?.sourceFor(0x017f));
    try std.testing.expectEqual(@as(?u16, 0x0050), unit.ruleFor(0x0200).?.sourceFor(0x0200));
    try std.testing.expectEqual(@as(?u16, 0x0281), unit.ruleFor(0x0281).?.sourceFor(0x0281));
    try std.testing.expectError(error.OverlappingRule, addRule(&unit, .{
        .first = 0x0200,
        .last = 0x0400,
        .source = 0x0051,
        .settings = 0x20,
        .alias = true,
    }));

    unit = .{};
    try addRule(&unit, .{ .first = 0x0100, .last = 0x0300, .settings = 0x10 });
    try addRule(&unit, .{ .first = 0x0180, .last = 0x0280, .settings = 0x20 });
    try std.testing.expectEqual(@as(u8, 0x10), unit.ruleFor(0x017f).?.settings);
    try std.testing.expectEqual(@as(u8, 0x20), unit.ruleFor(0x0200).?.settings);
    try std.testing.expectError(error.OverlappingRule, addRule(&unit, .{
        .first = 0x0181,
        .last = 0x0281,
        .settings = 0x30,
    }));

    unit = .{};
    unit.default_settings = 0x30;
    try addRule(&unit, .{ .first = 0x0100, .last = 0x0110, .source = 0x0020, .settings = 0x10, .alias = true });
    try std.testing.expectEqual(@as(?u16, 0x0020), unit.ruleFor(0x0100).?.sourceFor(0x0100));
    try std.testing.expectEqual(@as(u8, 0x10), unit.settingsForSource(0x0020));
    try std.testing.expectEqual(@as(u8, 0x30), unit.ruleFor(0x0120).?.settings);
    try addRule(&unit, unit.rules[0]);
    try std.testing.expectEqual(@as(usize, 1), unit.rule_count);

    unit = .{};
    try addRule(&unit, .{ .first = 0x0020, .last = 0x0020, .source = 0x0030, .settings = 0x10, .alias = true });
    try std.testing.expectEqual(@as(u8, 0), unit.settingsForSource(0x0020));
    try std.testing.expectEqual(@as(u8, 0x10), unit.settingsForSource(0x0030));

    unit = .{};
    try addRule(&unit, .{ .first = 0x0100, .last = 0x0110, .source = 0x0200, .settings = 0x10, .alias = true });
    try addRule(&unit, .{ .first = 0x0200, .last = 0x0200, .settings = 0x20 });
    try std.testing.expectError(error.OverlappingRule, validateAliasTargets(&unit));

    unit = .{};
    for (0..max_rules_per_unit - 1) |index| {
        unit.rules[index] = .{ .first = @intCast(index), .last = @intCast(index) };
    }
    unit.rules[max_rules_per_unit - 1] = broad;
    unit.rule_count = max_rules_per_unit;
    try std.testing.expectError(error.TooManyRules, addRule(&unit, alias));
    try std.testing.expectEqual(@as(u16, 0xfffe), unit.rules[max_rules_per_unit - 1].last);
}

test "shared special requester keeps nonzero settings and rejects conflicts" {
    var unit = Unit{};
    try addSpecial(&unit, 0x00a0, 0);
    try addSpecial(&unit, 0x00a0, 0xd7);
    try addSpecial(&unit, 0x00a0, 0);
    try addSpecial(&unit, 0x00a0, 0xd7);
    try std.testing.expectEqual(@as(usize, 1), unit.special_count);
    try std.testing.expectEqual(@as(u8, 0xd7), unit.special_sources[0].settings);
    try std.testing.expectError(error.InvalidDeviceEntry, addSpecial(&unit, 0x00a0, 0x10));
    try std.testing.expectEqual(@as(u8, 0xd7), unit.special_sources[0].settings);

    unit = .{};
    try addSpecial(&unit, 0x00a0, 0xd7);
    try addSpecial(&unit, 0x00a0, 0);
    try std.testing.expectEqual(@as(usize, 1), unit.special_count);
    try std.testing.expectEqual(@as(u8, 0xd7), unit.special_sources[0].settings);
}
