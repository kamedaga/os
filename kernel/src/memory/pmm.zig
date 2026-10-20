const std = @import("std");
const kernel = @import("../kernel.zig");
pub const RegionBoundary = @import("free_page_list.zig").RegionBoundary;
const kernel_vm = @import("kernel_vm.zig");
const PhysicalAddress = @import("addresses.zig").PhysicalAddress;

pub const MemoryStats = struct {
    detected_regions: usize,
    total_usable_bytes: u64,
};

const ReservedRange = struct {
    start: u64,
    end: u64,
};

pub const MemoryRegionKind = enum {
    usable,
    reserved,
    bootloader_reclaimable,
    executable_and_modules,
    framebuffer,
    acpi_reclaimable,
    acpi_nvs,
    bad_memory,
    unknown,
};

pub const MemoryRegion = struct {
    base: u64,
    length: u64,
    kind: MemoryRegionKind,
};

// Usable IDs retain firmware usable-entry order, independently of sorting.
// Registration establishes return provenance, never initial free ownership:
// boot reservations (including scratch backing) are excluded during seeding.
pub fn regionBoundary(id: u64, region: MemoryRegion) !?RegionBoundary {
    if (region.kind != .usable) return null;
    const limit = std.math.add(u64, region.base, region.length) catch return error.InvalidState;
    const rounded = std.math.add(u64, region.base, 4095) catch return error.InvalidState;
    const start = rounded & ~@as(u64, 4095);
    const end = limit & ~@as(u64, 4095);
    if (start >= end) return null;
    return .{ .id = id, .start = start, .end = end };
}

// Validate coverage even for usable entries: a hole, unaligned intersection,
// or non-RAM firmware kind must not be admitted merely because it is inside
// the executable's enclosing physical extent. Overlap is rejected by installRegions.
pub fn scratchRegionIntersection(region: MemoryRegion, id: u64, start: u64, end: u64) !?RegionBoundary {
    if (start == 0 or start >= end or ((start | end) & 4095) != 0) return error.InvalidState;
    const limit = std.math.add(u64, region.base, region.length) catch return error.InvalidState;
    const first = @max(start, region.base);
    const last = @min(end, limit);
    if (first >= last) return null;
    if (region.kind != .usable and region.kind != .executable_and_modules) return error.InvalidState;
    if (((first | last) & 4095) != 0) return error.InvalidState;
    return .{ .id = id, .start = first, .end = last };
}

pub fn sortBoundaries(regions: []RegionBoundary) void {
    std.mem.sort(RegionBoundary, regions, {}, struct {
        fn less(_: void, a: RegionBoundary, b: RegionBoundary) bool {
            return a.start < b.start;
        }
    }.less);
}

pub const Hooks = struct {
    write: *const fn ([]const u8) void,
    main_addr: usize,
    kernel_cr3_addr: usize,
    kernel_image_base_paddr: *const u64,
    kernel_image_size_bytes: *const usize,
    kernel_static_start_addr: usize,
    kernel_static_end_addr: usize,
    reserved_low_mem_end: u64,
};

var hooks: ?Hooks = null;

pub fn init(new_hooks: Hooks) void {
    hooks = new_hooks;
}

fn isReserved(paddr: u64, reserved: []const ReservedRange) bool {
    for (reserved) |r| {
        if (paddr >= r.start and paddr < r.end) return true;
    }
    return false;
}

// Boot alone owns HHDM holes until our direct mapping becomes active. Do not
// publish an intrusive descriptor into a page Limine cannot actually access.
const DeferredBootRange = struct { next: ?*DeferredBootRange = null, first: u64, pages: usize };
var deferred_head: ?*DeferredBootRange = null;
var deferred_tail: ?*DeferredBootRange = null;
var deferred_records: []DeferredBootRange = &.{};
var deferred_used: usize = 0;
// Reserve every possible missing-HHDM span before PMM seed begins. No scratch
// growth (and therefore no late boot reservation) occurs during that transfer.
pub fn prepareBootAccess(regions: []const @import("../arch/x86_64/direct_map.zig").Region) !void {
    const view = @import("free_page_list.zig").boot_view orelse return error.InvalidState;
    var count: usize = 0;
    for (regions) |region| {
        if (!region.usable) continue;
        var cursor = std.mem.alignForward(u64, region.base, 4096);
        const end = (region.base + region.bytes) & ~@as(u64, 4095);
        while (cursor < end) {
            const proof = view.inspect(cursor);
            if (!proof.mapped) count += 1;
            if (proof.next <= cursor) return error.InvalidState;
            cursor = proof.next;
        }
    }
    // Each of the four fixed boot exclusions can split a missing span once.
    if (count != 0) deferred_records = try @import("../arch/x86_64/boot_direct_map.zig").metadata(DeferredBootRange, count + 4);
}
pub fn finishBootAccess() !void {
    const free_list = @import("../kernel_runtime.zig").global_free_list;
    @import("free_page_list.zig").boot_view = null;
    while (deferred_head) |range| {
        try free_list.appendContiguousRange(range.first, range.pages);
        deferred_head = range.next;
    }
    deferred_tail = null;
}
fn deferBootPage(pa: u64) !void {
    if (deferred_tail) |tail| {
        if (tail.first + @as(u64, tail.pages) * 4096 == pa) {
            tail.pages += 1;
            return;
        }
    }
    if (deferred_used == deferred_records.len) return error.InvalidState;
    const range = &deferred_records[deferred_used];
    deferred_used += 1;
    range.* = .{ .first = pa, .pages = 1 };
    if (deferred_tail) |tail| tail.next = range else deferred_head = range;
    deferred_tail = range;
}

pub const MemoryCollector = struct {
    free_list: *kernel.FreePageList,
    reserved: [4]ReservedRange,
    detected_regions: usize,
    total_usable_bytes: u64,
    boot_reserved_pages: []const PhysicalAddress = &.{},
    boot_reservations_installed: bool = false,
    boot_checked_until: u64 = 0,
    boot_checked_mapped: bool = false,

    /// The boot owner retains this immutable sorted list through finish().
    /// Install before seeding, so reserved PT pages never transiently become
    /// free. This transfers no page ownership and performs no PMM allocation.
    pub fn reserveBootPages(self: *MemoryCollector, pages: []const PhysicalAddress) !void {
        if (self.boot_reservations_installed or self.detected_regions != 0 or
            self.free_list.len != 0 or self.free_list.range_len != 0) return error.InvalidState;
        var previous: u64 = 0;
        for (pages) |page| {
            const pa = page.toInt();
            if (pa == 0 or pa & 4095 != 0 or pa <= previous) return error.InvalidState;
            previous = pa;
        }
        self.boot_reserved_pages = pages;
        self.boot_reservations_installed = true;
    }

    fn bootPageReserved(self: *const MemoryCollector, pa: u64) bool {
        var low: usize = 0;
        var high = self.boot_reserved_pages.len;
        while (low < high) {
            const mid = low + (high - low) / 2;
            const reserved = self.boot_reserved_pages[mid].toInt();
            if (reserved == pa) return true;
            if (reserved < pa) low = mid + 1 else high = mid;
        }
        return false;
    }

    pub fn init(
        free_list: *kernel.FreePageList,
        user_spaces_start: u64,
        user_spaces_end: u64,
    ) ?MemoryCollector {
        const h = hooks orelse return null;
        const image_base = h.kernel_image_base_paddr.*;
        const image_bytes = h.kernel_image_size_bytes.*;
        // Without Limine's physical image extent there is no safe physical
        // fallback: a linked code address is virtual (or a compiler sentinel).
        if (image_base == 0 or image_bytes == 0) {
            h.write("pmm: missing physical kernel image extent\n");
            return null;
        }
        const image_start = kernel_vm.pageAlignDown(image_base);
        const image_limit = std.math.add(u64, image_base, @as(u64, @intCast(image_bytes))) catch {
            h.write("pmm: physical kernel image extent overflows\n");
            return null;
        };
        const image_rounded_limit = std.math.add(u64, image_limit, 4095) catch {
            h.write("pmm: physical kernel image extent overflows\n");
            return null;
        };
        const image_end = image_rounded_limit & ~@as(u64, 4095);
        if (image_end <= image_start) return null;
        if (free_list.len != 0 or free_list.range_len != 0) return null;
        const kernel_static_start = kernel_vm.pageAlignDown(h.kernel_static_start_addr);
        const kernel_static_end = kernel_vm.pageAlignUp(h.kernel_static_end_addr);
        // Limine's executable physical base is authoritative. A Zig function
        // pointer is not a physical-address fallback (and may be represented
        // as 1), so taking its minimum with the image base can accidentally
        // reserve all usable RAM below the kernel image.
        const kernel_reserved_start = image_start;
        const kernel_reserved_end = image_end;

        var trace_buf: [128]u8 = undefined;
        const trace = std.fmt.bufPrint(&trace_buf, "pmm: kernel physical reservation=0x{x}..0x{x}\n", .{ kernel_reserved_start, kernel_reserved_end }) catch "";
        h.write(trace);

        return .{
            .free_list = free_list,
            .reserved = .{
                .{ .start = 0, .end = h.reserved_low_mem_end },
                .{ .start = kernel_reserved_start, .end = kernel_reserved_end },
                .{ .start = kernel_static_start, .end = kernel_static_end },
                .{ .start = user_spaces_start, .end = user_spaces_end },
            },
            .detected_regions = 0,
            .total_usable_bytes = 0,
        };
    }

    pub fn addRegion(self: *MemoryCollector, region: MemoryRegion) !void {
        if (region.kind != .usable) return;
        if (try regionBoundary(self.detected_regions, region)) |boundary| {
            var paddr = boundary.start;
            while (paddr < boundary.end) : (paddr += 4096) {
                if (isReserved(paddr, self.reserved[0..]) or self.bootPageReserved(paddr)) continue;
                if (!@import("builtin").is_test) if (@import("free_page_list.zig").boot_view) |view| {
                    if (paddr >= self.boot_checked_until) {
                        const proof = view.inspect(paddr);
                        self.boot_checked_until = proof.next;
                        self.boot_checked_mapped = proof.mapped;
                    }
                    if (!self.boot_checked_mapped) {
                        try deferBootPage(paddr);
                        continue;
                    }
                };
                try self.free_list.seedContiguousRange(paddr, 1);
            }
        }
        self.detected_regions += 1;
        self.total_usable_bytes += region.length;
    }

    pub fn finish(self: MemoryCollector) MemoryStats {
        if (hooks) |h| {
            const low = self.free_list.availableBelow(1 << 32);
            var trace_buf: [112]u8 = undefined;
            const trace = std.fmt.bufPrint(&trace_buf, "pmm: usable={} MiB free_below4g={} pages\n", .{ self.total_usable_bytes / (1024 * 1024), low.total }) catch "";
            h.write(trace);
        }
        return .{
            .detected_regions = self.detected_regions,
            .total_usable_bytes = self.total_usable_bytes,
        };
    }
};

pub fn collectMemoryStatsAndFreePagesFromRegions(
    free_list: *kernel.FreePageList,
    regions: []const MemoryRegion,
    boundary_storage: []RegionBoundary,
    user_spaces_start: u64,
    user_spaces_end: u64,
) ?MemoryStats {
    var count: usize = 0;
    var id: u64 = 0;
    for (regions) |region| {
        if (region.kind != .usable) continue;
        if (regionBoundary(id, region) catch return null) |boundary| {
            if (count == boundary_storage.len) return null;
            boundary_storage[count] = boundary;
            count += 1;
        }
        id += 1;
    }
    sortBoundaries(boundary_storage[0..count]);
    free_list.installRegions(boundary_storage[0..count]) catch return null;
    var collector = MemoryCollector.init(free_list, user_spaces_start, user_spaces_end) orelse return null;
    for (regions) |region| {
        collector.addRegion(region) catch return null;
    }
    return collector.finish();
}

test "MMIO overlay PMM boot table handoff excludes high pages before any allocation" {
    var backing = try @import("free_page_list.zig").TestBacking.init(21474836480, 5);
    defer backing.deinit();
    const direct = @import("../arch/x86_64/direct_map.zig");
    const allocator = std.testing.allocator;
    const free_list = try allocator.create(kernel.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    const base: u64 = 20 << 30;
    try free_list.installRegions(&.{.{ .id = 0, .start = base, .end = base + 5 * 4096 }});
    var collector = MemoryCollector{
        .free_list = free_list,
        .reserved = @splat(.{ .start = 0, .end = 0 }),
        .detected_regions = 0,
        .total_usable_bytes = 0,
    };
    // Architectural records are not handed to PMM. Unordered input becomes
    // a sorted physical-only reservation snapshot with no address narrowing.
    var words: [512]u64 align(4096) = @splat(0);
    var tables = [_]direct.Table{
        .{ .physical = base + 3 * 4096, .words = &words },
        .{ .physical = base + 4096, .words = &words },
    };
    const draft = direct.Draft{ .tables = &tables };
    var storage: [2]PhysicalAddress = undefined;
    try std.testing.expectError(error.NoSpace, draft.writeReservations(storage[0..1]));
    const reserved = try draft.writeReservations(&storage);
    try collector.reserveBootPages(reserved);
    try std.testing.expectError(error.InvalidState, collector.reserveBootPages(reserved));
    try collector.addRegion(.{ .base = base, .length = 5 * 4096, .kind = .usable });
    try std.testing.expectEqual(@as(usize, 3), free_list.pageCount());
    for (0..3) |_| {
        const grant = try free_list.popContiguousOwnedInRange(1, base, base + 5 * 4096);
        try std.testing.expect(grant.physical.toInt() != reserved[0].toInt());
        try std.testing.expect(grant.physical.toInt() != reserved[1].toInt());
        try std.testing.expect(grant.physical.toInt() >= base);
    }
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    try std.testing.expectError(error.InvalidState, collector.reserveBootPages(&.{}));
}

test "MMIO overlay PMM rejects malformed boot reservations before retaining the borrow" {
    const free_list = try std.testing.allocator.create(kernel.FreePageList);
    defer std.testing.allocator.destroy(free_list);
    free_list.* = .{};
    var collector = MemoryCollector{
        .free_list = free_list,
        .reserved = @splat(.{ .start = 0, .end = 0 }),
        .detected_regions = 0,
        .total_usable_bytes = 0,
    };
    for ([_][2]PhysicalAddress{
        .{ .fromInt(0), .fromInt(4096) },
        .{ .fromInt(4097), .fromInt(8192) },
        .{ .fromInt(4096), .fromInt(4096) },
        .{ .fromInt(8192), .fromInt(4096) },
    }) |invalid| {
        try std.testing.expectError(error.InvalidState, collector.reserveBootPages(&invalid));
        try std.testing.expect(!collector.boot_reservations_installed);
        try std.testing.expectEqual(@as(usize, 0), collector.boot_reserved_pages.len);
    }
}

test "MMIO overlay PMM reserves the known physical kernel image without losing lower RAM" {
    var backing = try @import("free_page_list.zig").TestBacking.init(67108864, 2);
    defer backing.deinit();
    const allocator = std.testing.allocator;
    const free_list = try allocator.create(kernel.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    var image_base: u64 = 0x6efbf000;
    var image_bytes: usize = 0xaf32000;
    const previous_hooks = hooks;
    defer hooks = previous_hooks;
    init(.{
        .write = struct {
            fn write(_: []const u8) void {}
        }.write,
        .main_addr = 1,
        .kernel_cr3_addr = 0xffff_ffff_8014_d248,
        .kernel_image_base_paddr = &image_base,
        .kernel_image_size_bytes = &image_bytes,
        .kernel_static_start_addr = 0xffff_ffff_800e_5000,
        .kernel_static_end_addr = 0xffff_ffff_8af3_2000,
        .reserved_low_mem_end = 64 * 1024 * 1024,
    });
    image_base = 0;
    try std.testing.expect(MemoryCollector.init(free_list, 0, 0) == null);
    image_base = 0x6efbf000;
    image_bytes = 0;
    try std.testing.expect(MemoryCollector.init(free_list, 0, 0) == null);
    image_base = std.math.maxInt(u64) - 4095;
    image_bytes = 8192;
    try std.testing.expect(MemoryCollector.init(free_list, 0, 0) == null);
    image_base = 0x6efbf000;
    image_bytes = 0xaf32000;
    try free_list.installRegions(&.{
        .{ .id = 0, .start = 0x4000000, .end = 0x4002000 },
        .{ .id = 1, .start = 0x6efbf000, .end = 0x6efc1000 },
    });
    var collector = MemoryCollector.init(free_list, 0, 0) orelse return error.InvalidState;
    try std.testing.expectEqual(@as(u64, 0x6efbf000), collector.reserved[1].start);
    try std.testing.expectEqual(@as(u64, 0x79ef1000), collector.reserved[1].end);
    try collector.addRegion(.{ .base = 0x4000000, .length = 8192, .kind = .usable });
    try collector.addRegion(.{ .base = 0x6efbf000, .length = 8192, .kind = .usable });
    try std.testing.expectEqual(@as(usize, 2), free_list.availableBelow(1 << 32).total);
}

test "MMIO overlay PMM persistent catalog survives scratch staging seal and exhausted free ranges" {
    var backing = try @import("free_page_list.zig").TestBacking.init(4096, 1);
    defer backing.deinit();
    const scratch = @import("../boot/boot_scratch.zig");
    var storage: [4096]u8 align(4096) = undefined;
    var arena = scratch.Arena{};
    try arena.install(&storage);
    const allocation = try arena.reservePersistent(@sizeOf(RegionBoundary));
    var loan: scratch.Borrow = .{};
    try allocation.borrowInto(&loan);
    const regions = std.mem.bytesAsSlice(RegionBoundary, try loan.bytes());
    regions[0] = .{ .id = 301, .start = 4096, .end = 8192 };
    try loan.end();
    const published = try allocation.publishPersistent();
    const list = try std.testing.allocator.create(kernel.FreePageList);
    defer std.testing.allocator.destroy(list);
    list.* = .{};
    try list.installRegions(std.mem.bytesAsSlice(RegionBoundary, published));
    const temporary = try arena.reserveTemporary(128);
    try temporary.release();
    try arena.seal();
    try list.seedContiguousRange(4096, 1);
    const page = try list.popFront();
    try list.appendPage(page);
    try std.testing.expectEqual(@as(u64, 301), list.rangeAt(0).region_id);
    try std.testing.expectError(error.InvalidState, allocation.release());
}

test "MMIO overlay PMM page seeding preserves reserved middle ends and adjacent firmware regions" {
    var backing = try @import("free_page_list.zig").TestBacking.init(65536, 12);
    defer backing.deinit();
    const list = try std.testing.allocator.create(kernel.FreePageList);
    defer std.testing.allocator.destroy(list);
    list.* = .{};
    try list.installRegions(&.{ .{ .id = 0, .start = 0x10000, .end = 0x16000 }, .{ .id = 1, .start = 0x16000, .end = 0x1c000 } });
    var collector = MemoryCollector{
        .free_list = list,
        .reserved = .{
            .{ .start = 0x10000, .end = 0x11000 },
            .{ .start = 0x13000, .end = 0x15000 },
            .{ .start = 0x1a000, .end = 0x1c000 },
            .{ .start = 0, .end = 0 },
        },
        .detected_regions = 0,
        .total_usable_bytes = 0,
    };
    try collector.addRegion(.{ .base = 0x10000, .length = 0x6000, .kind = .usable });
    try collector.addRegion(.{ .base = 0x16000, .length = 0x6000, .kind = .usable });
    const expected = [_]kernel.RegionFreeRange{
        .{ .region_id = 0, .physical_start = 0x11000, .len = 2 },
        .{ .region_id = 0, .physical_start = 0x15000, .len = 1 },
        .{ .region_id = 1, .physical_start = 0x16000, .len = 4 },
    };
    try std.testing.expectEqual(expected.len, list.rangeCount());
    for (expected, 0..) |range, i| {
        const actual = list.rangeAt(i);
        try std.testing.expectEqual(range.physical_start, actual.physical_start);
        try std.testing.expectEqual(range.len, actual.len);
        try std.testing.expectEqual(range.region_id, actual.region_id);
    }
    try std.testing.expectEqual(@as(usize, 7), list.pageCount());
    try std.testing.expectEqual(@as(usize, 2), collector.detected_regions);
}

test "MMIO overlay boot scratch image RAM boundaries preserve usable IDs and do not seed pages" {
    var backing = try @import("free_page_list.zig").TestBacking.init(4096, 6);
    defer backing.deinit();
    const list = try std.testing.allocator.create(kernel.FreePageList);
    defer std.testing.allocator.destroy(list);
    list.* = .{};
    const usable: MemoryRegion = .{ .base = 4096, .length = 8192, .kind = .usable };
    const image: MemoryRegion = .{ .base = 12288, .length = 16384, .kind = .executable_and_modules };
    const part = (try scratchRegionIntersection(image, 8, 8192, 24576)).?;
    try std.testing.expectEqual(@as(u64, 12288), part.start);
    try std.testing.expectEqual(@as(u64, 24576), part.end);
    var boundaries = [_]RegionBoundary{ part, (try regionBoundary(7, usable)).? };
    sortBoundaries(&boundaries);
    try list.installRegions(&boundaries);
    try std.testing.expectEqual(@as(usize, 0), list.pageCount());
    // The retained prefix is not returned; a tail crossing the boundary is
    // split atomically and subsequent pop/return still derives the same IDs.
    try list.appendContiguousRange(8192, 4);
    try std.testing.expectEqual(@as(usize, 4), list.pageCount());
    try std.testing.expectEqual(@as(u64, 7), list.rangeAt(0).region_id);
    try std.testing.expectEqual(@as(u64, 8), list.rangeAt(1).region_id);
    try std.testing.expectError(error.RegionNotFound, list.appendPage(24576));
    try std.testing.expectError(error.InvalidState, list.installRegions(&boundaries));
    try std.testing.expectEqual(@as(u64, 4096), (try scratchRegionIntersection(usable, 7, 4096, 8192)).?.start);
    try std.testing.expectEqual(@as(?RegionBoundary, null), try scratchRegionIntersection(image, 8, 4096, 8192));
    for ([_]MemoryRegionKind{ .reserved, .framebuffer, .bad_memory, .unknown, .bootloader_reclaimable }) |kind| {
        try std.testing.expectError(error.InvalidState, scratchRegionIntersection(.{ .base = 4096, .length = 4096, .kind = kind }, 0, 4096, 8192));
    }
    try std.testing.expectError(error.InvalidState, scratchRegionIntersection(.{ .base = 4097, .length = 4096, .kind = .executable_and_modules }, 0, 4096, 8192));
    try std.testing.expectError(error.InvalidState, scratchRegionIntersection(.{ .base = std.math.maxInt(u64), .length = 4096, .kind = .usable }, 0, 4096, 8192));
    list.* = .{};
    try std.testing.expectError(error.InvalidState, list.installRegions(&.{ boundaries[0], boundaries[0] }));
    try std.testing.expectEqual(@as(usize, 0), list.pageCount());
}

test "MMIO overlay boot scratch retirement uses real PMM return and retains ownership on quarantine" {
    const scratch = @import("../boot/boot_scratch.zig");
    const Quarantine = struct {
        var active = false;
        fn query(_: u64, _: usize) bool {
            return active;
        }
    };
    const List = @import("free_page_list.zig").WithQuarantine(Quarantine.query);
    const Context = struct { list: *List, base: usize, withdrew: bool = false, acknowledged: bool = false };
    const Retirer = struct {
        pub fn physical(c: *Context, va: usize, _: usize) !u64 {
            _ = c;
            return va;
        }
        pub fn withdraw(c: *Context, _: usize, _: usize) !void {
            c.withdrew = true;
        }
        pub fn acknowledge(c: *Context, _: usize, _: usize) !void {
            try std.testing.expect(c.withdrew);
            c.acknowledged = true;
        }
        pub fn release(c: *Context, pa: u64, pages: usize) !void {
            try std.testing.expect(c.acknowledged);
            try c.list.appendContiguousRange(pa, pages);
        }
    };
    const list = try std.testing.allocator.create(List);
    defer std.testing.allocator.destroy(list);
    defer Quarantine.active = false;
    for (0..2) |mode| {
        Quarantine.active = false;
        list.* = .{};
        var buffer: [12288]u8 align(4096) = undefined;
        var arena: scratch.Arena = .{};
        try arena.install(&buffer);
        const allocation = try arena.reservePersistent(@sizeOf(RegionBoundary));
        var loan: scratch.Borrow = .{};
        try allocation.borrowInto(&loan);
        const regions = std.mem.bytesAsSlice(RegionBoundary, try loan.bytes());
        regions[0] = .{ .id = 8, .start = @intFromPtr(&buffer), .end = @intFromPtr(&buffer) + buffer.len };
        try loan.end();
        const published = try allocation.publishPersistent();
        try list.installRegions(std.mem.bytesAsSlice(RegionBoundary, published));
        const stable = list.regions.?.ptr;
        const temporary = try arena.reserveTemporary(4096);
        try temporary.release();
        try arena.seal();
        var context = Context{ .list = list, .base = @intFromPtr(&buffer) };
        Quarantine.active = mode == 1;
        const before = list.pageCount();
        if (mode == 0) {
            try arena.reclaim(Retirer, &context);
            try std.testing.expectEqual(before + 2, list.pageCount());
            const page = try list.popContiguousBelow(2, std.math.maxInt(u64));
            @memset(buffer[4096..], 0xdd);
            try list.appendContiguousRange(page, 2);
            try std.testing.expectEqual(@as(u64, 8), list.rangeAt(0).region_id);
            try std.testing.expect(arena.reclaimed);
        } else {
            const failure = error.PageQuarantined;
            try std.testing.expectError(failure, arena.reclaim(Retirer, &context));
            try std.testing.expectEqual(before, list.pageCount());
            try std.testing.expect(!arena.reclaimed);
            try std.testing.expectEqual(@as(usize, 8192), arena.tail_bytes);
        }
        try std.testing.expect(list.regions.?.ptr == stable);
        try std.testing.expectEqual(@as(u64, @intFromPtr(&buffer)), list.regions.?[0].start);
        try std.testing.expectError(error.InvalidState, arena.reclaim(Retirer, &context));
        var stale: scratch.Borrow = .{};
        try std.testing.expectError(error.InvalidState, temporary.borrowInto(&stale));
    }
}
