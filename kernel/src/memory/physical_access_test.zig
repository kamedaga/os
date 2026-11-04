const std = @import("std");
comptime {
    if (!@import("builtin").is_test) @compileError("physical fixture is host-test only");
}

pub const Mapping = struct {
    physical: u64,
    bytes: []align(4096) u8,
    // Only owned sparse fixtures defer poisoning. Explicit host grants already
    // contain caller-owned contents and must never be initialized here.
    page_states: ?[]u8 = null,
};

fn initializePages(entry: Mapping, offset: usize, bytes: usize) void {
    const states = entry.page_states orelse return;
    const first = offset / 4096;
    const last = (offset + bytes - 1) / 4096;
    for (first..last + 1) |page| {
        const state = &states[page];
        if (@atomicLoad(u8, state, .acquire) == 2) continue;
        if (@cmpxchgStrong(u8, state, 0, 1, .acq_rel, .acquire) == null) {
            // Publish readiness only after the entire page is poisoned. A
            // concurrent borrower must not overwrite data with a late memset.
            @memset(entry.bytes[page * 4096 ..][0..4096], 0xaa);
            @atomicStore(u8, state, 2, .release);
        } else {
            while (@atomicLoad(u8, state, .acquire) != 2) std.atomic.spinLoopHint();
        }
    }
}
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
        initializePages(m, @intCast(offset), bytes);
        const pointer: [*]T = @ptrCast(@alignCast(m.bytes.ptr + @as(usize, @intCast(offset))));
        return pointer[0..count];
    }
    if (mappings.len != 0) {
        for (mappings) |m| {
            if (physical < m.physical) continue;
            const offset = physical - m.physical;
            if (offset > m.bytes.len or bytes > m.bytes.len - offset) continue;
            initializePages(m, @intCast(offset), bytes);
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
        errdefer for (owned[0..count]) |entry| freeBacking(entry);
        for (ranges, 0..) |range, i| {
            owned[i] = try allocateBacking(range);
            count += 1;
        }
        const result = @This(){ .owned = owned, .previous = mapping, .previous_many = mappings, .previous_host_addresses = allow_host_addresses };
        mapping = null;
        mappings = owned;
        allow_host_addresses = host_addresses;
        return result;
    }
    fn allocateBacking(range: Range) !Mapping {
        const length = std.math.mul(usize, range.pages, 4096) catch return error.OutOfMemory;
        const states = try std.testing.allocator.alloc(u8, range.pages);
        errdefer std.testing.allocator.free(states);
        @memset(states, 0);
        if (length == 0) return .{ .physical = range.first, .bytes = &.{}, .page_states = states };
        // Reserve real contiguous host memory with the leak-checking allocator,
        // but leave untouched fixture pages demand-paged. Allocator.alloc/free
        // poison the entire ~800 MiB range per case; slice poisons each page
        // before its first exposure instead. No raw payload escapes this owner.
        const pointer = std.testing.allocator.rawAlloc(length, .fromByteUnits(4096), @returnAddress()) orelse return error.OutOfMemory;
        return .{ .physical = range.first, .bytes = @as([*]align(4096) u8, @alignCast(pointer))[0..length], .page_states = states };
    }
    fn freeBacking(entry: Mapping) void {
        // Every borrower/worker must have ended before fixture teardown. Raw
        // release preserves allocator tracking without touching unmapped pages.
        if (entry.bytes.len != 0) std.testing.allocator.rawFree(entry.bytes, .fromByteUnits(4096), @returnAddress());
        std.testing.allocator.free(entry.page_states.?);
    }
    pub fn deinit(self: *@This()) void {
        mapping = self.previous;
        mappings = self.previous_many;
        allow_host_addresses = self.previous_host_addresses;
        for (self.owned) |entry| freeBacking(entry);
        std.testing.allocator.free(self.owned);
    }
};
