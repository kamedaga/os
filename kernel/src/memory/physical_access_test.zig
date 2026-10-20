const std = @import("std");
comptime {
    if (!@import("builtin").is_test) @compileError("physical fixture is host-test only");
}

pub const Mapping = struct { physical: u64, bytes: []align(4096) u8 };
pub var mapping: ?Mapping = null;
pub var host_mappings: []const Mapping = &.{};

pub fn slice(comptime T: type, physical: u64, count: usize) ?[]T {
    const bytes = std.math.mul(usize, @sizeOf(T), count) catch return null;
    if (physical == 0 or count == 0 or physical % @alignOf(T) != 0) return null;
    _ = std.math.add(u64, physical, bytes - 1) catch return null;
    for (host_mappings) |m| {
        if (physical >= m.physical and physical - m.physical <= m.bytes.len and bytes <= m.bytes.len - (physical - m.physical)) {
            const pointer: [*]T = @ptrFromInt(physical);
            return pointer[0..count];
        }
    }
    if (mapping) |m| {
        if (physical < m.physical) return null;
        const offset = physical - m.physical;
        if (offset > m.bytes.len or bytes > m.bytes.len - offset) return null;
        const pointer: [*]T = @ptrCast(@alignCast(m.bytes.ptr + @as(usize, @intCast(offset))));
        return pointer[0..count];
    }
    if (mappings.len != 0) {
        for (mappings) |m| {
            if (physical < m.physical) continue;
            const offset = physical - m.physical;
            if (offset > m.bytes.len or bytes > m.bytes.len - offset) continue;
            const pointer: [*]T = @ptrCast(@alignCast(m.bytes.ptr + @as(usize, @intCast(offset))));
            return pointer[0..count];
        }
        if (!allow_host_addresses) return null;
    }
    // Legacy tests explicitly seed PMM with host addresses, never production RAM.
    const pointer: [*]T = @ptrFromInt(physical);
    return pointer[0..count];
}

pub var mappings: []const Mapping = &.{};
var allow_host_addresses: bool = false;
// Sparse logical PAs have real, separately owned pages; holes stay unmapped.
// The fixture restores its predecessor and never substitutes a metadata-only
// descriptor registry for the actual contents of a returned page.
pub const BackingSet = struct {
    owned: []Mapping,
    previous: ?Mapping,
    previous_many: []const Mapping,
    previous_host_addresses: bool,
    pub const Range = struct { first: u64, pages: usize };
    pub fn init(ranges: []const Range) !@This() {
        return initWithHostAddresses(ranges, false);
    }
    // Mixed fixtures explicitly use both logical PAs and real host allocations.
    pub fn initMixed(ranges: []const Range) !@This() {
        return initWithHostAddresses(ranges, true);
    }
    fn initWithHostAddresses(ranges: []const Range, host_addresses: bool) !@This() {
        const owned = try std.testing.allocator.alloc(Mapping, ranges.len);
        errdefer std.testing.allocator.free(owned);
        var count: usize = 0;
        errdefer for (owned[0..count]) |entry| std.testing.allocator.free(entry.bytes);
        for (ranges, 0..) |range, i| {
            owned[i] = .{ .physical = range.first, .bytes = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), range.pages * 4096) };
            count += 1;
        }
        const result = @This(){ .owned = owned, .previous = mapping, .previous_many = mappings, .previous_host_addresses = allow_host_addresses };
        mapping = null;
        mappings = owned;
        allow_host_addresses = host_addresses;
        return result;
    }
    pub fn deinit(self: *@This()) void {
        mapping = self.previous;
        mappings = self.previous_many;
        allow_host_addresses = self.previous_host_addresses;
        for (self.owned) |entry| std.testing.allocator.free(entry.bytes);
        std.testing.allocator.free(self.owned);
    }
};
