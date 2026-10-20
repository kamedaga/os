//! MADT decoding is independent of storage and IPI delivery. Disabled CPUs
//! prevent shorthand broadcast, but do not consume runtime CPU slots.
const std = @import("std");
pub const Error = error{ InvalidRecord, InvalidApicId, UnsupportedApicId, NoSpace };
pub const Entry = struct { id: u32, enabled: bool, x2: bool };

pub fn decode(record: []const u8) Error!?Entry {
    if (record.len < 2 or record[1] != record.len) return error.InvalidRecord;
    return switch (record[0]) {
        0 => if (record.len < 8) error.InvalidRecord else .{
            .id = record[3],
            .enabled = (std.mem.readInt(u32, record[4..8], .little) & 1) != 0,
            .x2 = false,
        },
        9 => if (record.len < 16) error.InvalidRecord else .{
            // ACPI local x2APIC: reserved at 2, ID at 4, flags at 8.
            // Swapping these fields turns enabled=1 into APIC ID 1.
            .id = std.mem.readInt(u32, record[4..8], .little),
            .enabled = (std.mem.readInt(u32, record[8..12], .little) & 1) != 0,
            .x2 = true,
        },
        else => null,
    };
}

pub fn append(ids: []u32, count: *usize, entry: Entry, x2_mode: bool) Error!void {
    if (!entry.enabled) return;
    if (entry.id == std.math.maxInt(u32)) return error.InvalidApicId;
    if (!x2_mode and entry.id > 254) return error.UnsupportedApicId;
    for (ids[0..count.*]) |id| if (id == entry.id) return;
    if (count.* == ids.len) return error.NoSpace;
    ids[count.*] = entry.id;
    count.* += 1;
}

test "MADT type9 retains 32 bit ID and flags at their ACPI offsets" {
    var record = [_]u8{0} ** 16;
    record[0] = 9;
    record[1] = 16;
    std.mem.writeInt(u32, record[4..8], 0x12345678, .little);
    std.mem.writeInt(u32, record[8..12], 1, .little);
    const entry = (try decode(&record)).?;
    try std.testing.expectEqual(@as(u32, 0x12345678), entry.id);
    try std.testing.expect(entry.enabled);
    var ids: [1]u32 = undefined;
    var count: usize = 0;
    try std.testing.expectError(error.UnsupportedApicId, append(&ids, &count, entry, false));
    try append(&ids, &count, entry, true);
    try std.testing.expectEqual(@as(usize, 1), count);
    try append(&ids, &count, entry, true);
    try std.testing.expectEqual(@as(usize, 1), count);
    record[8] = 0;
    try std.testing.expect(!(try decode(&record)).?.enabled);
    try std.testing.expectError(error.InvalidRecord, decode(record[0..8]));
}

test "MADT storage holds more than 64 CPUs and rejects overflow explicitly" {
    var ids: [300]u32 = undefined;
    var count: usize = 0;
    for (0..ids.len) |id| try append(&ids, &count, .{ .id = @intCast(id), .enabled = true, .x2 = true }, true);
    try std.testing.expectEqual(@as(usize, 300), count);
    try std.testing.expectEqual(@as(u32, 299), ids[299]);
    try std.testing.expectError(error.NoSpace, append(&ids, &count, .{ .id = 301, .enabled = true, .x2 = true }, true));
    try std.testing.expectError(error.InvalidApicId, append(&ids, &count, .{ .id = 0xffffffff, .enabled = true, .x2 = true }, true));
}

test "mixed MADT type0 and type9 enumeration retains all 72 enabled CPUs" {
    var ids: [73]u32 = undefined;
    var count: usize = 0;
    for (0..72) |cpu| {
        var record = [_]u8{0} ** 16;
        const bytes: usize = if (cpu < 64) 8 else 16;
        record[0] = if (cpu < 64) 0 else 9;
        record[1] = @intCast(bytes);
        const id: u32 = if (cpu < 64) @intCast(cpu) else @intCast(cpu + 0x100);
        if (cpu < 64) {
            record[3] = @intCast(id);
            std.mem.writeInt(u32, record[4..8], 1, .little);
        } else {
            std.mem.writeInt(u32, record[4..8], id, .little);
            std.mem.writeInt(u32, record[8..12], 1, .little);
        }
        try append(&ids, &count, (try decode(record[0..bytes])).?, true);
    }
    try std.testing.expectEqual(@as(usize, 72), count);
    try std.testing.expectEqual(@as(u32, 327), ids[71]);
    // Disabled and duplicate entries cannot invent or consume a CPU slot.
    try append(&ids, &count, .{ .id = 500, .enabled = false, .x2 = true }, true);
    try append(&ids, &count, .{ .id = 327, .enabled = true, .x2 = true }, true);
    try std.testing.expectEqual(@as(usize, 72), count);
    try std.testing.expectError(error.UnsupportedApicId, append(&ids, &count, .{ .id = 255, .enabled = true, .x2 = false }, false));
}
