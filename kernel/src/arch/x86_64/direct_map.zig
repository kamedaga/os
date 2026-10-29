//! Detached boot page-table construction. The caller owns the input snapshot,
//! reservation records and all allocated pages until it publishes the root.
//! No PMM, current CR3, Limine pointer or runtime cache policy is implicit here.
const std = @import("std");
const paging = @import("../../memory/ram_cache_paging.zig");
const PhysicalAddress = @import("../../memory/addresses.zig").PhysicalAddress;
pub const LeafSize = paging.LeafSize;
pub const base: u64 = 0xffff_8000_0000_0000;
pub const first_slot: usize = 256;
pub const end_slot: usize = 510;
pub const aperture_bytes: u64 = (end_slot - first_slot) * (@as(u64, 1) << 39);
pub const io_base: u64 = base + aperture_bytes;
const page_bytes: u64 = 4096;
const nx: u64 = @as(u64, 1) << 63;
const leaf_flags: u64 = nx | 3;
const address_mask: u64 = 0x000f_ffff_ffff_f000;

pub const Region = struct { base: u64, bytes: u64, usable: bool, prepare_only: bool = false, firmware_reserved: bool = false };
pub const Extent = struct { start: u64, end: u64 };
pub const Error = error{ InvalidMap, PhysicalLimit, InvalidExclusion, InvalidImage, NoSpace, InvalidTable, OccupiedRoot };

pub fn physicalLimit(bits: u8) Error!u64 {
    // Four-level x86 page tables have 52 address bits. Reject a bad CPUID
    // result instead of shifting it or silently narrowing the memory map.
    if (bits < 32 or bits > 52) return error.PhysicalLimit;
    return @min(aperture_bytes, @as(u64, 1) << @as(u6, @intCast(bits)));
}

pub fn validateImage(image_start: u64, image_bytes: u64, stack_start: u64, stack_bytes: u64) Error!void {
    const image_end = std.math.add(u64, image_start, image_bytes) catch return error.InvalidImage;
    const stack_last = std.math.add(u64, stack_start, stack_bytes -| 1) catch return error.InvalidImage;
    if (image_bytes == 0 or stack_bytes == 0 or image_start < 0xffff_ff80_0000_0000 or
        stack_start < 0xffff_ff80_0000_0000 or
        (image_start <= stack_last and stack_start < image_end)) return error.InvalidImage;
}

fn roundedStart(value: u64) Error!u64 {
    return (std.math.add(u64, value, page_bytes - 1) catch return error.InvalidMap) & ~(page_bytes - 1);
}

pub const Layout = struct {
    regions: []const Region,
    excluded: []const Extent,
    limit: u64,
    huge_pages: bool,
    returned: Extent = .{ .start = 0, .end = 0 },

    pub fn init(regions: []const Region, excluded: []const Extent, bits: u8, huge_pages: bool) Error!Layout {
        const limit = try physicalLimit(bits);
        var previous_end: u64 = 0;
        for (regions) |region| {
            const end = std.math.add(u64, region.base, region.bytes) catch return error.InvalidMap;
            if (region.bytes == 0 or region.base < previous_end) return error.InvalidMap;
            previous_end = end;
            // Report all out-of-aperture usable RAM, even an unaligned tail.
            if ((region.usable or region.prepare_only) and end > limit) return error.PhysicalLimit;
            if (region.prepare_only and (region.usable or (region.base | region.bytes) & 4095 != 0)) return error.InvalidMap;
        }
        previous_end = 0;
        for (excluded) |extent| {
            if (extent.start >= extent.end or extent.start < previous_end or
                (extent.start | extent.end) & (page_bytes - 1) != 0) return error.InvalidExclusion;
            previous_end = extent.end;
        }
        return .{ .regions = regions, .excluded = excluded, .limit = limit, .huge_pages = huge_pages };
    }

    pub fn iterator(self: Layout) Iterator {
        return .{ .layout = self };
    }

    pub fn contains(self: Layout, physical: u64, bytes: u64) bool {
        if (physical == 0 or bytes == 0) return false;
        const end = std.math.add(u64, physical, bytes) catch return false;
        if (end > self.limit) return false;
        if (physical >= self.returned.start and end <= self.returned.end) return true;
        for (self.excluded) |extent| {
            if (physical < extent.end and extent.start < end) return false;
        }
        var cursor = physical;
        for (self.regions) |region| {
            if (!region.usable) continue;
            const start = roundedStart(region.base) catch return false;
            const stop = (region.base + region.bytes) & ~(page_bytes - 1);
            if (stop <= cursor) continue;
            if (start > cursor) return false;
            cursor = @min(end, stop);
            if (cursor == end) return true;
        }
        return false;
    }

    pub fn tableCount(self: Layout) usize {
        var it = self.iterator();
        var last_pml4: ?u64 = null;
        var last_pdp: ?u64 = null;
        var last_pd: ?u64 = null;
        var count: usize = 0;
        while (it.next()) |leaf| {
            const pml4 = leaf.physical >> 39;
            const pdp = leaf.physical >> 30;
            const pd = leaf.physical >> 21;
            if (last_pml4 != pml4) {
                count += 1;
                last_pml4 = pml4;
            }
            if (leaf.size != .huge and last_pdp != pdp) {
                count += 1;
                last_pdp = pdp;
            }
            if (leaf.size == .page and last_pd != pd) {
                count += 1;
                last_pd = pd;
            }
        }
        return count;
    }
};

pub const Leaf = struct { physical: u64, size: LeafSize, present: bool = true };
pub const Iterator = struct {
    layout: Layout,
    region: usize = 0,
    cursor: u64 = 0,
    source: ?*const fn (usize) ?Region = null,

    pub fn next(self: *Iterator) ?Leaf {
        while (true) {
            const region = if (self.source) |read| read(self.region) orelse return null else if (self.region < self.layout.regions.len) self.layout.regions[self.region] else return null;
            if (!region.usable and !region.prepare_only) {
                self.region += 1;
                continue;
            }
            const start = roundedStart(region.base) catch unreachable;
            const end = (region.base + region.bytes) & ~(page_bytes - 1);
            self.cursor = @max(self.cursor, @max(start, page_bytes));
            var stop = end;
            for (self.layout.excluded) |extent| {
                if (extent.end <= self.cursor) continue;
                if (extent.start <= self.cursor) {
                    self.cursor = extent.end;
                } else {
                    stop = @min(stop, extent.start);
                    break;
                }
            }
            if (self.cursor >= end) {
                self.region += 1;
                continue;
            }
            var size: LeafSize = .page;
            for ([_]LeafSize{ .huge, .large }) |candidate| {
                if (region.prepare_only or (candidate == .huge and !self.layout.huge_pages)) continue;
                if (self.cursor & (candidate.bytes() - 1) == 0 and stop - self.cursor >= candidate.bytes()) {
                    size = candidate;
                    break;
                }
            }
            const result = Leaf{ .physical = self.cursor, .size = size, .present = !region.prepare_only };
            self.cursor += size.bytes();
            return result;
        }
    }
};

/// A monotonic boot reservation cursor. Each returned PA is reserved once;
/// the caller records it and subtracts it when handing usable RAM to the PMM.
/// A rejected HHDM span is skipped for boot allocation only, NOT for mapping.
pub const Reservations = struct {
    leaves: Iterator,
    cursor: u64 = 0,
    end: u64 = 0,

    pub fn init(layout: Layout) Reservations {
        return .{ .leaves = layout.iterator() };
    }

    // view is boot_hhdm.View (or a host fixture with the same inspect contract).
    // The serial boot owner keeps the active root stable until construction ends.
    pub fn take(self: *Reservations, view: anytype) Error!u64 {
        while (true) {
            if (self.cursor >= self.end) {
                const leaf = self.leaves.next() orelse return error.NoSpace;
                if (!leaf.present) continue;
                self.cursor = @max(self.cursor, leaf.physical);
                self.end = leaf.physical + leaf.size.bytes();
                if (self.cursor >= self.end) continue;
            }
            const pa = self.cursor;
            const access = view.inspect(pa);
            if (access.mapped and access.next >= pa + page_bytes) {
                self.cursor += page_bytes;
                return pa;
            }
            // Do not trust a malformed/non-progressing view to make progress.
            self.cursor = @max(pa + page_bytes, roundedStart(access.next) catch return error.NoSpace);
        }
    }
};

pub const Table = struct {
    physical: u64,
    words: *align(4096) [512]u64,
};

/// Caller-supplied records are sized by Layout.tableCount, not a fixed pool.
/// Allocator.take returns reserved, HHDM-checked writable pages and release
/// returns unpublished pages only. All live-root stores happen in publish.
pub const Draft = struct {
    roots: [end_slot - first_slot]u64 = @splat(0),
    tables: []Table,
    published: bool = false,

    pub fn validateRoot(self: *const Draft, root: *const [512]u64) Error!void {
        if (!self.published) return error.InvalidTable;
        for (root[first_slot..end_slot], self.roots) |entry, expected| {
            // Hardware may set A on a live parent; no other drift is benign.
            if ((entry & ~@as(u64, 1 << 5)) != expected) return error.InvalidTable;
        }
    }

    /// Copy only the physical reservation list for PMM seeding. PMM need not
    /// understand architectural table records or borrow their writable views.
    /// The boot owner keeps the returned list immutable until seeding ends.
    pub fn writeReservations(self: *const Draft, output: []PhysicalAddress) Error![]const PhysicalAddress {
        if (output.len < self.tables.len) return error.NoSpace;
        for (self.tables, output[0..self.tables.len]) |table, *address| address.* = .fromInt(table.physical);
        const used = output[0..self.tables.len];
        std.sort.heap(PhysicalAddress, used, {}, struct {
            fn less(_: void, a: PhysicalAddress, b: PhysicalAddress) bool {
                return a.toInt() < b.toInt();
            }
        }.less);
        return used;
    }

    /// The live-root owner must validate its PML4 links against roots first.
    /// Boot records own the original tables; split_view proves ownership of
    /// later dynamic PD/PTs before it returns their mutable borrow. No table
    /// pointer supplied by a corrupt PTE is dereferenced without that proof.
    pub fn visitAliases(self: *const Draft, layout: Layout, pages: []const u64, split_view: anytype, context: anytype, comptime visit: anytype) !void {
        try paging.validatePages(pages, layout.limit);
        var last_large: ?*u64 = null;
        for (pages) |pa| {
            if (!layout.contains(pa, page_bytes)) continue;
            const root = self.roots[@intCast(pa >> 39)];
            const pdp = try self.borrowTable(root, 0, .page, split_view);
            var directory = &pdp[(pa >> 30) & 511];
            var size: LeafSize = .huge;
            while (true) {
                const value = @atomicLoad(u64, directory, .acquire);
                if ((value & (1 | 4 | 256)) != 1) return error.InvalidTable;
                const physical_base = pa & ~(size.bytes() - 1);
                if (size == .page) {
                    if (value & address_mask != pa) return error.InvalidTable;
                    try visit(context, paging.AliasSource{ .leaf = directory });
                    break;
                }
                if (value & 128 != 0) {
                    try paging.IdentitySplit.validateSourceSized(value, physical_base, size);
                    if (last_large != directory) {
                        try visit(context, paging.AliasSource{ .identity = .{
                            .directory = directory,
                            .base = physical_base,
                            .size = size,
                            .alias_va = base + physical_base,
                        } });
                        last_large = directory;
                    }
                    break;
                }
                const table = try self.borrowTable(value, base + physical_base, size, split_view);
                size = try size.child();
                directory = &table[(pa >> @intFromEnum(size)) & 511];
            }
        }
    }

    fn borrowTable(self: *const Draft, entry: u64, va: u64, size: LeafSize, split_view: anytype) !*[512]u64 {
        // Parent cache bits refer to PT RAM, never to the target RAM's PAT.
        if ((entry & ~(address_mask | 0x60)) != leaf_flags) return error.InvalidTable;
        const pa = entry & address_mask;
        for (self.tables) |table| if (table.physical == pa) return table.words;
        if (size == .page) return error.InvalidTable; // PML4 child is boot-owned.
        return split_view.borrow(va, size, pa) orelse error.InvalidTable;
    }

    pub fn build(layout: Layout, records: []Table, allocator: anytype) !Draft {
        const count = layout.tableCount();
        if (records.len < count) return error.NoSpace;
        var used: usize = 0;
        errdefer for (records[0..used]) |table| allocator.release(table);
        // Allocate everything before wiring. Failure never changes a live root.
        while (used < count) {
            const table = try allocator.take();
            var valid = table.physical != 0 and table.physical & 4095 == 0 and
                table.physical < layout.limit and table.physical & ~address_mask == 0 and
                layout.contains(table.physical, page_bytes);
            for (records[0..used]) |old| {
                if (old.physical == table.physical or old.words == table.words) valid = false;
            }
            if (!valid) {
                // A broken provider can return an already owned page. Do not
                // free that duplicate twice; the prior record still owns it.
                var duplicate = false;
                for (records[0..used]) |old| {
                    if (old.physical == table.physical or old.words == table.words) duplicate = true;
                }
                if (!duplicate) allocator.release(table);
                return error.InvalidTable;
            }
            records[used] = table;
            used += 1;
            @memset(table.words, 0);
        }
        var draft = Draft{ .tables = records[0..count] };
        var next_table: usize = 0;
        var it = layout.iterator();
        var last_pml4: ?usize = null;
        var last_pdp: ?u64 = null;
        var last_pd: ?u64 = null;
        var pdp: *[512]u64 = undefined;
        var pd: *[512]u64 = undefined;
        var pt: *[512]u64 = undefined;
        while (it.next()) |leaf| {
            const pml4: usize = @intCast(leaf.physical >> 39);
            if (last_pml4 != pml4) {
                const table = records[next_table];
                next_table += 1;
                draft.roots[pml4] = table.physical | leaf_flags;
                pdp = table.words;
                last_pml4 = pml4;
            }
            const pdp_index = (leaf.physical >> 30) & 511;
            if (leaf.size == .huge) {
                pdp[pdp_index] = leaf.physical | leaf_flags | 128;
                continue;
            }
            if (last_pdp != leaf.physical >> 30) {
                const table = records[next_table];
                next_table += 1;
                pdp[pdp_index] = table.physical | leaf_flags;
                pd = table.words;
                last_pdp = leaf.physical >> 30;
            }
            const pd_index = (leaf.physical >> 21) & 511;
            if (leaf.size == .large) {
                pd[pd_index] = leaf.physical | leaf_flags | 128;
                continue;
            }
            if (last_pd != leaf.physical >> 21) {
                const table = records[next_table];
                next_table += 1;
                pd[pd_index] = table.physical | leaf_flags;
                pt = table.words;
                last_pd = leaf.physical >> 21;
            }
            pt[(leaf.physical >> 12) & 511] = if (leaf.present) leaf.physical | leaf_flags else 0;
        }
        std.debug.assert(next_table == count);
        return draft;
    }

    /// Serial boot only, BEFORE loading this root on any CPU. Runtime cache
    /// alias enumeration must be installed before this map becomes active.
    pub fn publish(self: *Draft, root: *[512]u64) Error!void {
        if (self.published) return error.OccupiedRoot;
        for (root[first_slot..end_slot]) |entry| if (entry != 0) return error.OccupiedRoot;
        for (self.roots, first_slot..) |entry, slot| root[slot] = entry;
        self.published = true;
    }

    /// Prepared image scratch has no leaf alias until its old mapping is
    /// withdrawn and acknowledged. The caller retains ownership on failure.
    pub fn preparedLeaf(self: *const Draft, pa: u64) Error!*u64 {
        var entry = self.roots[@intCast(pa >> 39)];
        for ([_]u6{ 30, 21, 12 }) |shift| {
            if ((entry & ~(address_mask | 0x60)) != leaf_flags) return error.InvalidTable;
            var words: ?*[512]u64 = null;
            for (self.tables) |table| if (table.physical == (entry & address_mask)) {
                words = table.words;
                break;
            };
            const table = words orelse return error.InvalidTable;
            const slot = &table[(pa >> shift) & 511];
            if (shift == 12) return slot;
            entry = slot.*;
        }
        unreachable;
    }

    pub fn discard(self: *Draft, allocator: anytype) void {
        std.debug.assert(!self.published);
        for (self.tables) |table| allocator.release(table);
        self.tables = &.{};
        @memset(&self.roots, 0);
    }
};

/// Linker-owned section boundaries, never inferred from a fixed image budget.
pub const ImageSections = struct {
    text: u64,
    rodata: u64,
    data: u64,
    end: u64,
    pub fn validate(self: ImageSections) !void {
        if ((self.text | self.rodata | self.data | self.end) & 4095 != 0 or
            self.text >= self.rodata or self.rodata >= self.data or self.data >= self.end)
            return error.InvalidImage;
        try validateImage(self.text, self.end - self.text, 0xffff_ffff_c000_0000, 1 << 30);
    }
    pub fn pdCount(self: ImageSections) !usize {
        try self.validate();
        return @intCast(((0xffff_ffff_c000_0000 >> 30) & 511) - ((self.text >> 30) & 511) + 1);
    }
    pub fn ptCount(self: ImageSections) !usize {
        try self.validate();
        return @intCast(((self.end - 1) >> 21) - (self.text >> 21) + 1);
    }
    /// Populate detached image tables using the same boot-owned Table records
    /// as the RAM direct map. Only the caller publishes the root afterwards.
    pub fn install(self: ImageSections, pdp: *[512]u64, pds: []Table, pts: []Table, physical: u64) !void {
        try self.validate();
        if (pds.len != try self.pdCount() or pts.len != try self.ptCount() or
            physical & 4095 != 0 or self.text & ((1 << 21) - 1) != 0 or
            physical > address_mask - (self.end - self.text)) return error.InvalidImage;
        for ([_][]Table{ pds, pts }) |tables| for (tables) |table| {
            if (table.physical == 0 or table.physical & ~address_mask != 0) return error.InvalidImage;
        };
        @memset(pdp, 0);
        for ([_][]Table{ pds, pts }) |tables| for (tables) |table| {
            @memset(table.words, 0);
        };
        var va = self.text;
        while (va < self.end) : (va += 4096) {
            const pdp_index = (va >> 30) & 511;
            const pd = pds[pdp_index - ((self.text >> 30) & 511)];
            const pt = pts[(va - self.text) >> 21];
            pdp[pdp_index] = pd.physical | 3;
            pd.words[(va >> 21) & 511] = pt.physical | 3;
            pt.words[(va >> 12) & 511] = (physical + va - self.text) | try self.flags(va);
        }
    }
    pub fn flags(self: ImageSections, va: u64) !u64 {
        if (va < self.text or va >= self.end) return error.InvalidImage;
        return if (va < self.rodata) 1 else if (va < self.data) nx | 1 else nx | 3;
    }
};
extern var __kernel_text_start: u8;
extern var __kernel_rodata_start: u8;
extern var __kernel_data_start: u8;
extern var __kernel_image_end: u8;
pub fn linkedImageSections() ImageSections {
    return .{ .text = @intFromPtr(&__kernel_text_start), .rodata = @intFromPtr(&__kernel_rodata_start), .data = @intFromPtr(&__kernel_data_start), .end = @intFromPtr(&__kernel_image_end) };
}
