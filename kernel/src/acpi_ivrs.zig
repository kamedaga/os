const std = @import("std");

const header_bytes: usize = 48;

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
    entry_bytes: []const u8 = &.{},
    rule_count: usize = 0,

    special_count: usize = 0,

    pub fn rawRules(self: Unit) Rules(false) {
        return .{ .entries = .{ .bytes = self.entry_bytes } };
    }
    pub fn rules(self: Unit) Rules(true) {
        return .{ .entries = .{ .bytes = self.entry_bytes } };
    }
    pub fn specials(self: Unit) Specials {
        return .{ .entries = .{ .bytes = self.entry_bytes } };
    }
    pub fn ruleAt(self: Unit, index: usize) Rule {
        var it = self.rules();
        for (0..index) |_| _ = it.next().?;
        return it.next().?;
    }
    pub fn specialAt(self: Unit, index: usize) SpecialSource {
        var it = self.specials();
        for (0..index) |_| _ = it.next().?;
        return it.next().?;
    }

    fn ordinaryRuleFor(self: *const Unit, device_id: u16) ?Rule {
        var best: ?Rule = null;
        var iterator = self.rawRules();
        while (iterator.next()) |rule| {
            if (rule.alias or device_id < rule.first or device_id > rule.last) continue;
            // A narrower explicit range is more specific than broad firmware
            // coverage. Equal-width conflicting settings are rejected on input.
            if (best == null or rule.last - rule.first < best.?.last - best.?.first) best = rule;
        }
        return best;
    }

    pub fn ruleFor(self: *const Unit, device_id: u16) ?Rule {
        var iterator = self.rawRules();
        while (iterator.next()) |rule| {
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
        var iterator = self.rawRules();
        while (iterator.next()) |rule| {
            if (rule.alias and rule.source == source) settings = rule.settings;
        }
        var entries = Entries{ .bytes = self.entry_bytes };
        while (entries.next()) |entry| {
            if (entry == .special and entry.special.source == source and entry.special.settings != 0) settings = entry.special.settings;
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

/// Borrowed firmware view: bytes must remain immutable and non-reclaimable
/// until every Info/Unit/iterator user has finished. Production ACPI memory is
/// excluded from PMM usable input. No selected-unit or decoded-output arrays.
pub const Info = struct {
    bytes: []const u8 = &.{},
    unit_count: usize = 0,
    memory_range_count: usize = 0,
    pub fn units(self: Info) Units {
        return .{ .blocks = .{ .bytes = self.bytes } };
    }
    pub fn memories(self: Info) Memories {
        return .{ .blocks = .{ .bytes = self.bytes } };
    }
    pub fn unitAt(self: Info, index: usize) Unit {
        var it = self.units();
        for (0..index) |_| _ = it.next().?;
        return it.next().?;
    }
    pub fn memoryAt(self: Info, index: usize) MemoryRange {
        var it = self.memories();
        for (0..index) |_| _ = it.next().?;
        return it.next().?;
    }
};

const Blocks = struct {
    bytes: []const u8,
    offset: usize = 0,
    fn next(self: *Blocks) ?[]const u8 {
        if (self.offset == self.bytes.len) return null;
        const block = self.bytes[self.offset..][0..le16(self.bytes, self.offset + 2)];
        self.offset += block.len;
        return block;
    }
};
fn isIvhd(block: []const u8) bool {
    return block[0] == 0x10 or block[0] == 0x11 or block[0] == 0x40;
}
fn sameUnit(a: []const u8, b: []const u8) bool {
    return isIvhd(b) and le64(a, 8) == le64(b, 8) and le16(a, 16) == le16(b, 16);
}
// At the first occurrence of a unit, find its highest revision. This scan is
// boot/discovery work; runtime DTE lookup uses the selected Unit view directly.
fn selectBlock(bytes: []const u8, first: []const u8) ParseError![]const u8 {
    var chosen = first;
    var conflict = false;
    var blocks = Blocks{ .bytes = bytes };
    while (blocks.next()) |block| {
        if (!sameUnit(first, block)) continue;
        if (le16(first, 4) != le16(block, 4) or le16(first, 6) != le16(block, 6)) return error.InvalidBlock;
        if (block[0] > chosen[0]) {
            chosen = block;
            conflict = false;
        } else if (block[0] == chosen[0] and !std.mem.eql(u8, chosen, block)) conflict = true;
    }
    if (conflict) return error.InvalidBlock;
    return chosen;
}
fn definition(block: []const u8) Unit {
    return .{ .block_type = block[0], .flags = block[1], .device_id = le16(block, 4), .capability_offset = le16(block, 6), .mmio_base = le64(block, 8), .segment = le16(block, 16), .entry_bytes = block[if (block[0] == 0x10) @as(usize, 24) else 40..] };
}
pub const Units = struct {
    blocks: Blocks,
    fn nextBlock(self: *Units) ParseError!?[]const u8 {
        while (true) {
            const start = self.blocks.offset;
            const block = self.blocks.next() orelse return null;
            if (!isIvhd(block)) continue;
            var prior = Blocks{ .bytes = self.blocks.bytes[0..start] };
            var seen = false;
            while (prior.next()) |previous| {
                if (sameUnit(block, previous)) {
                    seen = true;
                    break;
                }
            }
            if (!seen) return try selectBlock(self.blocks.bytes, block);
        }
    }
    pub fn next(self: *Units) ?Unit {
        const block = (self.nextBlock() catch unreachable) orelse return null;
        var unit = definition(block);
        var entries = Entries{ .bytes = unit.entry_bytes };
        while (entries.next()) |entry| switch (entry) {
            .all => |settings| unit.default_settings = settings,
            else => {},
        };
        var rules = unit.rules();
        while (rules.next() != null) unit.rule_count += 1;
        var specials = unit.specials();
        while (specials.next() != null) unit.special_count += 1;
        return unit;
    }
};
pub const Memories = struct {
    blocks: Blocks,
    pub fn next(self: *Memories) ?MemoryRange {
        while (self.blocks.next()) |block| {
            const kind = block[0];
            if (isIvhd(block)) continue;
            const first: u16 = if (kind == 0x20) 0 else le16(block, 4);
            return .{ .flags = block[1], .first_device = first, .last_device = if (kind == 0x20) 0xffff else if (kind == 0x21) first else le16(block, 6), .segment = if (kind == 0x20) 0 else le16(block, 8), .all_segments = kind == 0x20, .base = le64(block, 16), .length = le64(block, 24) };
        }
        return null;
    }
};
const Entry = union(enum) { rule: Rule, special: SpecialSource, all: u8 };
// Only validated complete entries (or their complete prefixes during checking)
// reach this decoder. Pending range starts in a prefix emit no partial rule.
const Entries = struct {
    bytes: []const u8,
    offset: usize = 0,
    fn next(self: *Entries) ?Entry {
        var pending: ?Rule = null;
        while (self.offset < self.bytes.len) {
            const e = self.bytes[self.offset..];
            const kind = e[0];
            const length: usize = if (kind < 0x40) 4 else if (kind < 0x80) 8 else 22 + @as(usize, e[21]);
            self.offset += length;
            const id = le16(e, 1);
            if (pending) |start| {
                var rule = start;
                rule.last = id;
                return .{ .rule = rule };
            }
            switch (kind) {
                0x01 => return .{ .all = e[3] },
                0x02, 0x46 => return .{ .rule = .{ .first = id, .last = id, .settings = e[3] } },
                0x03, 0x47 => pending = .{ .first = id, .settings = e[3] },
                0x42 => return .{ .rule = .{ .first = id, .last = id, .source = le16(e, 5), .settings = e[3], .alias = true } },
                0x43 => pending = .{ .first = id, .source = le16(e, 5), .settings = e[3], .alias = true },
                0x48 => return .{ .special = .{ .source = le16(e, 5), .settings = e[3] } },
                0xf0 => return .{ .special = .{ .source = id, .settings = e[3] } },
                else => {},
            }
        }
        return null;
    }
};
// Discovery needs canonical counts; runtime lookup must never carry the
// quadratic deduplication branch. Specialize the same decoder at compile time.
pub fn Rules(comptime unique: bool) type {
    return struct {
        entries: Entries,
        pub fn next(self: *@This()) ?Rule {
            while (true) {
                const start = self.entries.offset;
                const entry = self.entries.next() orelse return null;
                if (entry != .rule) continue;
                if (unique) {
                    var prior = Rules(false){ .entries = .{ .bytes = self.entries.bytes[0..start] } };
                    var seen = false;
                    while (prior.next()) |rule| {
                        if (std.meta.eql(rule, entry.rule)) {
                            seen = true;
                            break;
                        }
                    }
                    if (seen) continue;
                }
                return entry.rule;
            }
        }
    };
}
pub const Specials = struct {
    entries: Entries,
    pub fn next(self: *Specials) ?SpecialSource {
        while (true) {
            const start = self.entries.offset;
            const entry = self.entries.next() orelse return null;
            if (entry != .special) continue;
            var prior = Entries{ .bytes = self.entries.bytes[0..start] };
            var seen = false;
            while (prior.next()) |p| {
                if (p == .special and p.special.source == entry.special.source) {
                    seen = true;
                    break;
                }
            }
            if (seen) continue;
            var result = entry.special;
            var tail = self.entries;
            while (tail.next()) |p| {
                if (p == .special and p.special.source == result.source and p.special.settings != 0) result.settings = p.special.settings;
            }
            return result;
        }
    }
};

pub const ParseError = error{
    InvalidSignature,
    InvalidLength,
    InvalidChecksum,
    UnsupportedRevision,
    InvalidBlock,
    InvalidDeviceEntry,
    OverlappingRule,
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
    var rules = unit.rawRules();
    while (rules.next()) |existing| {
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
    unit.rule_count += 1;
}

fn validateAliasTargets(unit: *const Unit) ParseError!void {
    var rules = unit.rawRules();
    while (rules.next()) |rule| {
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
    // The prefix has already been validated. Scan its raw entries once,
    // rather than recomputing the merged special-source iterator for every
    // insertion (which would make validation cubic in the input size).
    var entries = Entries{ .bytes = unit.entry_bytes };
    var seen = false;
    while (entries.next()) |entry| {
        if (entry != .special or entry.special.source != source) continue;
        seen = true;
        const prior = entry.special.settings;
        // Shared HPET/IOAPIC zero settings add no flags; conflicting nonzero
        // settings for one DTE requester must still reject the entire table.
        if (prior != 0 and settings != 0 and prior != settings) return error.InvalidDeviceEntry;
    }
    if (!seen) unit.special_count += 1;
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
        unit.entry_bytes = bytes[0..offset];
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
    unit.entry_bytes = bytes;
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
    // Validate every block envelope before any nested/repeated scan. Publish
    // only after all chosen vocabularies and all IVMD records are validated.
    var offset: usize = header_bytes;
    while (offset < table.len) {
        if (table.len - offset < 4) return error.InvalidBlock;
        const length: usize = le16(table, offset + 2);
        if (length < 4 or length > table.len - offset) return error.InvalidBlock;
        const block = table[offset..][0..length];
        if (isIvhd(block)) {
            const minimum: usize = if (block[0] == 0x10) 24 else 40;
            if (length < minimum or (block[0] == 0x40 and table[8] != 2)) return error.InvalidBlock;
            const base = le64(block, 8);
            if (base == 0 or base & 0xfff != 0) return error.InvalidBlock;
        } else if (block[0] >= 0x20 and block[0] <= 0x22) {
            if (length != 32) return error.InvalidBlock;
            const base = le64(block, 16);
            const span = le64(block, 24);
            if (span == 0 or base > std.math.maxInt(u64) - span) return error.InvalidBlock;
            if (block[0] == 0x22 and le16(block, 4) > le16(block, 6)) return error.InvalidBlock;
        } else return error.InvalidBlock;
        offset += length;
    }
    var result = Info{ .bytes = table[header_bytes..] };
    var units = result.units();
    while (try units.nextBlock()) |block| {
        var unit = definition(block);
        const entries = unit.entry_bytes;
        unit.entry_bytes = &.{};
        try parseDeviceEntries(allow_zero_pad, &unit, entries);
        if (unit.rule_count == 0 and unit.default_settings == null) return error.InvalidBlock;
        result.unit_count += 1;
    }
    if (result.unit_count == 0) return error.InvalidBlock;
    var memories = result.memories();
    while (memories.next() != null) result.memory_range_count += 1;
    info.* = result;
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
    try std.testing.expectEqual(@as(?u16, 0x0100), info.unitAt(0).ruleFor(0x0100).?.sourceFor(0x0100));
    try std.testing.expectEqual(@as(?u16, 0x00e0), info.unitAt(0).ruleFor(0x0101).?.sourceFor(0x0101));
    try std.testing.expectEqual(@as(?u16, 0x02ff), info.unitAt(0).ruleFor(0x02ff).?.sourceFor(0x02ff));
    try std.testing.expect(info.unitAt(0).ruleFor(0x0300) == null);
    try std.testing.expectEqual(@as(usize, 1), info.memory_range_count);
    try std.testing.expectEqual(@as(u64, 0x9000_0000), info.memoryAt(0).base);

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
    try std.testing.expectEqual(@as(u8, 0x10), info.unitAt(0).ruleFor(0x0100).?.settings);
    try std.testing.expectEqual(@as(u8, 0x20), info.unitAt(0).ruleFor(0x0120).?.settings);
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
    try std.testing.expectEqual(@as(usize, 1), info.unitAt(0).rule_count);
    try std.testing.expectEqual(@as(?u16, 0x0100), info.unitAt(0).ruleFor(0x0100).?.sourceFor(0x0100));
    try std.testing.expect(info.unitAt(0).ruleFor(0x0101) == null);
    try std.testing.expectEqual(@as(usize, 0), info.unitAt(0).special_count);

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
    const unit = info.unitAt(0);
    try std.testing.expectEqual(@as(u8, 0x11), unit.block_type);
    try std.testing.expectEqual(@as(u64, 0xfd50_0000), unit.mmio_base);
    try std.testing.expectEqual(@as(usize, 2), unit.rule_count);
    try std.testing.expectEqual(@as(u16, 0x0008), unit.ruleAt(0).first);
    try std.testing.expectEqual(@as(u16, 0xfffe), unit.ruleAt(0).last);
    try std.testing.expectEqual(@as(u16, 0xff00), unit.ruleAt(1).first);
    try std.testing.expectEqual(@as(u16, 0xffff), unit.ruleAt(1).last);
    try std.testing.expectEqual(@as(?u16, 0x0008), unit.ruleFor(0x0008).?.sourceFor(0x0008));
    try std.testing.expectEqual(@as(?u16, 0xfeff), unit.ruleFor(0xfeff).?.sourceFor(0xfeff));
    try std.testing.expectEqual(@as(?u16, 0x00a5), unit.ruleFor(0xff00).?.sourceFor(0xff00));
    try std.testing.expectEqual(@as(?u16, 0x00a5), unit.ruleFor(0xfffe).?.sourceFor(0xfffe));
    try std.testing.expectEqual(@as(?u16, 0x00a5), unit.ruleFor(0xffff).?.sourceFor(0xffff));
    try std.testing.expect(unit.ruleFor(0x0007) == null);
    try std.testing.expectEqual(unit.ruleAt(0).settings, unit.ruleAt(1).settings);
    try std.testing.expectEqual(@as(usize, 2), unit.special_count);
    try std.testing.expectEqual(@as(u16, 0x00a0), unit.specialAt(0).source);
    try std.testing.expectEqual(@as(u8, 0xd7), unit.specialAt(0).settings);
    try std.testing.expectEqual(@as(u16, 0x0001), unit.specialAt(1).source);
}

test "IVHD selection ignores superseded entry vocabularies but checks unit identity" {
    const info = try std.testing.allocator.create(Info);
    defer std.testing.allocator.destroy(info);
    var table = observed_b550_ivrs;
    table[72] = 0x41; // unsupported entry in superseded type 0x10 block
    setTestChecksum(&table);
    try parse(&table, info);
    try std.testing.expectEqual(@as(u8, 0x11), info.unitAt(0).block_type);
    try std.testing.expectEqual(@as(?u16, 0x00a5), info.unitAt(0).ruleFor(0xff00).?.sourceFor(0xff00));

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
    try std.testing.expectEqual(@as(u8, 0x11), info.unitAt(0).block_type);

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

/// Test-only byte builder. Tests exercise the production decoder; no synthetic
/// rule storage is present in production Unit or Info.
pub const TestEntries = if (@import("builtin").is_test) struct {
    bytes: std.ArrayList(u8) = .empty,
    unit: Unit = .{},
    pub fn deinit(self: *@This()) void {
        self.bytes.deinit(std.testing.allocator);
    }
    pub fn clear(self: *@This()) void {
        self.bytes.clearRetainingCapacity();
        self.unit = .{};
    }
    pub fn appendRule(self: *@This(), rule: Rule) !void {
        try addRule(&self.unit, rule);
        var bytes: [12]u8 = @splat(0);
        bytes[0] = if (rule.alias) 0x43 else 0x03;
        std.mem.writeInt(u16, bytes[1..3], rule.first, .little);
        bytes[3] = rule.settings;
        const end: usize = if (rule.alias) 8 else 4;
        if (rule.alias) std.mem.writeInt(u16, bytes[5..7], rule.source, .little);
        bytes[end] = 0x04;
        std.mem.writeInt(u16, bytes[end + 1 ..][0..2], rule.last, .little);
        try self.bytes.appendSlice(std.testing.allocator, bytes[0 .. end + 4]);
        self.unit.entry_bytes = self.bytes.items;
    }
    pub fn appendSpecial(self: *@This(), source: u16, settings: u8) !void {
        try addSpecial(&self.unit, source, settings);
        var bytes = [_]u8{ 0x48, 0, 0, settings, 0, 0, 0, 0 };
        std.mem.writeInt(u16, bytes[5..7], source, .little);
        try self.bytes.appendSlice(std.testing.allocator, &bytes);
        self.unit.entry_bytes = self.bytes.items;
    }
} else struct {};

pub fn testTable(allocator: std.mem.Allocator, definitions: []const Unit) ![]u8 {
    if (!@import("builtin").is_test) @compileError("test fixture only");
    var length: usize = header_bytes;
    for (definitions) |unit| length += 24 + unit.entry_bytes.len + (if (unit.default_settings != null) @as(usize, 4) else 0);
    const bytes = try allocator.alloc(u8, length);
    @memset(bytes, 0);
    @memcpy(bytes[0..4], "IVRS");
    std.mem.writeInt(u32, bytes[4..8], @intCast(length), .little);
    bytes[8] = 1;
    var offset: usize = header_bytes;
    for (definitions, 0..) |unit, index| {
        const size = 24 + unit.entry_bytes.len + (if (unit.default_settings != null) @as(usize, 4) else 0);
        const block = bytes[offset..][0..size];
        block[0] = 0x10;
        block[1] = unit.flags;
        std.mem.writeInt(u16, block[2..4], @intCast(size), .little);
        std.mem.writeInt(u16, block[4..6], unit.device_id, .little);
        std.mem.writeInt(u16, block[6..8], unit.capability_offset, .little);
        std.mem.writeInt(u64, block[8..16], if (unit.mmio_base != 0) unit.mmio_base else 0xfed0_0000 + @as(u64, @intCast(index)) * 0x4000, .little);
        std.mem.writeInt(u16, block[16..18], unit.segment, .little);
        @memcpy(block[24..][0..unit.entry_bytes.len], unit.entry_bytes);
        if (unit.default_settings) |settings| block[size - 4 ..][0..4].* = .{ 1, 0, 0, settings };
        offset += size;
    }
    setTestChecksum(bytes);
    return bytes;
}

test "overlap resolution is generic, order independent, and rejects ambiguous sources" {
    var fixture = TestEntries{};
    defer fixture.deinit();
    const unit = &fixture.unit;
    const broad = Rule{ .first = 0x0008, .last = 0xfffe, .settings = 0x10 };
    const alias = Rule{ .first = 0xff00, .last = 0xffff, .source = 0x00a5, .settings = 0x10, .alias = true };
    try fixture.appendRule(broad);
    try fixture.appendRule(alias);
    try std.testing.expectEqual(@as(usize, 2), unit.rule_count);
    try std.testing.expectEqual(@as(u16, 0xfffe), unit.ruleAt(0).last);
    try std.testing.expectEqual(@as(?u16, 0x00a5), unit.ruleFor(0xff00).?.sourceFor(0xff00));
    try std.testing.expectEqual(@as(u8, 0x10), unit.settingsForSource(0x00a5));

    try std.testing.expectError(error.OverlappingRule, fixture.appendRule(.{
        .first = 0xff10,
        .last = 0xffff,
        .source = 0x00a6,
        .settings = 0x10,
        .alias = true,
    }));
    try std.testing.expectEqual(@as(usize, 2), unit.rule_count);

    fixture.clear();
    try fixture.appendRule(broad);
    var different = alias;
    different.settings = 0x20;
    try std.testing.expectError(error.OverlappingRule, fixture.appendRule(different));
    try std.testing.expectEqual(@as(u16, 0xfffe), unit.ruleAt(0).last);

    fixture.clear();
    try fixture.appendRule(alias);
    try fixture.appendRule(broad);
    try std.testing.expectEqual(@as(?u16, 0x00a5), unit.ruleFor(0xff00).?.sourceFor(0xff00));
    try std.testing.expectEqual(@as(?u16, 0xfeff), unit.ruleFor(0xfeff).?.sourceFor(0xfeff));

    fixture.clear();
    try fixture.appendRule(.{ .first = 0x0100, .last = 0x0300, .settings = 0x20 });
    try fixture.appendRule(.{ .first = 0x0180, .last = 0x0280, .source = 0x0050, .settings = 0x20, .alias = true });
    try std.testing.expectEqual(@as(?u16, 0x017f), unit.ruleFor(0x017f).?.sourceFor(0x017f));
    try std.testing.expectEqual(@as(?u16, 0x0050), unit.ruleFor(0x0200).?.sourceFor(0x0200));
    try std.testing.expectEqual(@as(?u16, 0x0281), unit.ruleFor(0x0281).?.sourceFor(0x0281));
    try std.testing.expectError(error.OverlappingRule, fixture.appendRule(.{
        .first = 0x0200,
        .last = 0x0400,
        .source = 0x0051,
        .settings = 0x20,
        .alias = true,
    }));

    fixture.clear();
    try fixture.appendRule(.{ .first = 0x0100, .last = 0x0300, .settings = 0x10 });
    try fixture.appendRule(.{ .first = 0x0180, .last = 0x0280, .settings = 0x20 });
    try std.testing.expectEqual(@as(u8, 0x10), unit.ruleFor(0x017f).?.settings);
    try std.testing.expectEqual(@as(u8, 0x20), unit.ruleFor(0x0200).?.settings);
    try std.testing.expectError(error.OverlappingRule, fixture.appendRule(.{
        .first = 0x0181,
        .last = 0x0281,
        .settings = 0x30,
    }));

    fixture.clear();
    unit.default_settings = 0x30;
    try fixture.appendRule(.{ .first = 0x0100, .last = 0x0110, .source = 0x0020, .settings = 0x10, .alias = true });
    try std.testing.expectEqual(@as(?u16, 0x0020), unit.ruleFor(0x0100).?.sourceFor(0x0100));
    try std.testing.expectEqual(@as(u8, 0x10), unit.settingsForSource(0x0020));
    try std.testing.expectEqual(@as(u8, 0x30), unit.ruleFor(0x0120).?.settings);
    try fixture.appendRule(unit.ruleAt(0));
    try std.testing.expectEqual(@as(usize, 1), unit.rule_count);

    fixture.clear();
    try fixture.appendRule(.{ .first = 0x0020, .last = 0x0020, .source = 0x0030, .settings = 0x10, .alias = true });
    try std.testing.expectEqual(@as(u8, 0), unit.settingsForSource(0x0020));
    try std.testing.expectEqual(@as(u8, 0x10), unit.settingsForSource(0x0030));

    fixture.clear();
    try fixture.appendRule(.{ .first = 0x0100, .last = 0x0110, .source = 0x0200, .settings = 0x10, .alias = true });
    try fixture.appendRule(.{ .first = 0x0200, .last = 0x0200, .settings = 0x20 });
    try std.testing.expectError(error.OverlappingRule, validateAliasTargets(unit));
}

test "shared special requester keeps nonzero settings and rejects conflicts" {
    var fixture = TestEntries{};
    defer fixture.deinit();
    const unit = &fixture.unit;
    try fixture.appendSpecial(0x00a0, 0);
    try fixture.appendSpecial(0x00a0, 0xd7);
    try fixture.appendSpecial(0x00a0, 0);
    try fixture.appendSpecial(0x00a0, 0xd7);
    try std.testing.expectEqual(@as(usize, 1), unit.special_count);
    try std.testing.expectEqual(@as(u8, 0xd7), unit.specialAt(0).settings);
    try std.testing.expectError(error.InvalidDeviceEntry, fixture.appendSpecial(0x00a0, 0x10));
    try std.testing.expectEqual(@as(u8, 0xd7), unit.specialAt(0).settings);

    fixture.clear();
    try fixture.appendSpecial(0x00a0, 0xd7);
    try fixture.appendSpecial(0x00a0, 0);
    try std.testing.expectEqual(@as(usize, 1), unit.special_count);
    try std.testing.expectEqual(@as(u8, 0xd7), unit.specialAt(0).settings);
}

test "IVRS selected unit rules specials and IVMD exceed former capacities" {
    const allocator = std.testing.allocator;
    for ([_]usize{ 7, 8, 9, 257 }) |count| {
        const definitions = try allocator.alloc(Unit, count);
        defer allocator.free(definitions);
        @memset(definitions, .{ .default_settings = 0 });
        const bytes = try testTable(allocator, definitions);
        defer allocator.free(bytes);
        var info: Info = .{};
        try parse(bytes, &info);
        try std.testing.expectEqual(count, info.unit_count);
        var units = info.units();
        for (0..count) |index| try std.testing.expectEqual(@as(u64, 0xfed0_0000) + index * 0x4000, units.next().?.mmio_base);
        try std.testing.expect(units.next() == null);
    }
    for ([_]usize{ 511, 512, 513, 1025 }) |count| {
        var fixture = TestEntries{};
        defer fixture.deinit();
        for (0..count) |index| try fixture.appendRule(.{ .first = @intCast(index), .last = @intCast(index), .settings = 0x10 });
        const bytes = try testTable(allocator, &.{fixture.unit});
        defer allocator.free(bytes);
        var info: Info = .{};
        try parse(bytes, &info);
        const unit = info.unitAt(0);
        try std.testing.expectEqual(count, unit.rule_count);
        try std.testing.expectEqual(@as(u16, @intCast(count - 1)), unit.ruleAt(count - 1).first);
        try std.testing.expectEqual(@as(u8, 0x10), unit.settingsForSource(@intCast(count - 1)));
    }
    for ([_]usize{ 127, 128, 129, 257 }) |count| {
        var fixture = TestEntries{};
        defer fixture.deinit();
        fixture.unit.default_settings = 0;
        for (0..count) |index| try fixture.appendSpecial(@intCast(index), 0x10);
        const unit_bytes = try testTable(allocator, &.{fixture.unit});
        defer allocator.free(unit_bytes);
        const bytes = try allocator.alloc(u8, unit_bytes.len + count * 32);
        defer allocator.free(bytes);
        @memcpy(bytes[0..unit_bytes.len], unit_bytes);
        @memset(bytes[unit_bytes.len..], 0);
        std.mem.writeInt(u32, bytes[4..8], @intCast(bytes.len), .little);
        for (0..count) |index| {
            const block = bytes[unit_bytes.len + index * 32 ..][0..32];
            block[0] = 0x20;
            std.mem.writeInt(u16, block[2..4], 32, .little);
            std.mem.writeInt(u64, block[16..24], 0x9000_0000 + index * 4096, .little);
            std.mem.writeInt(u64, block[24..32], 4096, .little);
        }
        setTestChecksum(bytes);
        var info: Info = .{};
        try parse(bytes, &info);
        const unit = info.unitAt(0);
        try std.testing.expectEqual(count, unit.special_count);
        try std.testing.expectEqual(@as(u16, @intCast(count - 1)), unit.specialAt(count - 1).source);
        try std.testing.expectEqual(count, info.memory_range_count);
        try std.testing.expectEqual(@as(u64, 0x9000_0000) + (count - 1) * 4096, info.memoryAt(count - 1).base);
        const saved = info;
        std.mem.writeInt(u64, bytes[bytes.len - 8 ..][0..8], std.math.maxInt(u64), .little);
        setTestChecksum(bytes);
        try std.testing.expectError(error.InvalidBlock, parse(bytes, &info));
        // No new partial view is published. End the old borrow before mutation
        // in production; here only its scalar extent is inspected after error.
        try std.testing.expectEqual(saved.unit_count, info.unit_count);
        try std.testing.expectEqual(saved.bytes.ptr, info.bytes.ptr);
    }
}

test "IVRS malformed envelopes pending ranges alias targets and highest conflicts reject" {
    const allocator = std.testing.allocator;
    var fixture = TestEntries{};
    defer fixture.deinit();
    try fixture.appendRule(.{ .first = 0x100, .last = 0x110, .source = 0x200, .settings = 0x10, .alias = true });
    try fixture.appendRule(.{ .first = 0x200, .last = 0x200, .settings = 0x20 });
    const bytes = try testTable(allocator, &.{fixture.unit});
    defer allocator.free(bytes);
    var info: Info = .{};
    try std.testing.expectError(error.OverlappingRule, parse(bytes, &info));
    for ([_]u16{ 0, 3, 23, 65535 }) |length| {
        std.mem.writeInt(u16, bytes[50..52], length, .little);
        setTestChecksum(bytes);
        try std.testing.expectError(error.InvalidBlock, parse(bytes, &info));
    }
    std.mem.writeInt(u16, bytes[50..52], @intCast(bytes.len - 48), .little);
    bytes[80] = 0x02; // range end replaced by a selector
    setTestChecksum(bytes);
    try std.testing.expectError(error.InvalidDeviceEntry, parse(bytes, &info));
}
