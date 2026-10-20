//! Serial boot owns construction and retained metadata. Runtime readers start
//! only after publication; cache writers use the existing all-VM exclusion.
const std = @import("std");
const map = @import("direct_map.zig");
const hhdm = @import("boot_hhdm.zig");
const scratch = @import("../../boot/boot_scratch.zig");
const addresses = @import("../../memory/addresses.zig");
pub var layout: ?map.Layout = null;
pub var draft: ?map.Draft = null;
pub var image_pds: []map.Table = &.{};
pub var image_pts: []map.Table = &.{};
var prepared: map.Extent = .{ .start = 0, .end = 0 };

pub fn metadata(comptime T: type, count: usize) ![]T {
    const allocation = try scratch.reservePersistent(try std.math.mul(usize, @sizeOf(T), count));
    var loan: scratch.Borrow = .{};
    try allocation.borrowInto(&loan);
    const result = std.mem.bytesAsSlice(T, try loan.bytes());
    @memset(std.mem.sliceAsBytes(result), 0);
    try loan.end();
    _ = try allocation.publishPersistent();
    return result;
}

pub fn build(regions: []const map.Region, bits: u8, huge: bool, offset: u64, scratch_extent: map.Extent) ![]const addresses.PhysicalAddress {
    if (draft != null) return error.InvalidState;
    const view = hhdm.current(offset) orelse return error.InvalidState;
    const input = try map.Layout.init(regions, &.{}, bits, huge);
    const records = try metadata(map.Table, input.tableCount());
    const image = @import("../../boot/image_range.zig");
    const sections = map.linkedImageSections();
    try sections.validate();
    if (image.size_bytes == 0 or image.virtual_base != sections.text or
        image.size_bytes < sections.end - sections.text) return error.InvalidImage;
    const pd_count = try sections.pdCount();
    const pt_count = try sections.ptCount();
    image_pds = try metadata(map.Table, pd_count);
    image_pts = try metadata(map.Table, pt_count);
    const reservations = try metadata(addresses.PhysicalAddress, records.len + pd_count + pt_count);
    const Provider = struct {
        cursor: map.Reservations,
        view: hhdm.View,
        pub fn take(self: *@This()) !map.Table {
            // Firmware's low-memory AP trampoline and other boot reservations
            // remain owned. They may be mapped, but cannot back a new table.
            if (boot_cursor != null) {
                const pa = try takeBootPage();
                return .{ .physical = pa, .words = bootWords(pa) };
            }
            var pa = try self.cursor.take(self.view);
            while (pa < 64 * 1024 * 1024) pa = try self.cursor.take(self.view);
            return .{ .physical = pa, .words = @ptrFromInt(self.view.base + pa) };
        }
        pub fn release(_: *@This(), _: map.Table) void {
            // On failure boot halts before PMM publication. No other allocator
            // has seen these pages; the detached cursor is abandoned with boot.
        }
    };
    var provider = Provider{ .cursor = map.Reservations.init(input), .view = view };
    draft = try map.Draft.build(input, records, &provider);
    // The same reservation cursor owns image tables, so no PMM handoff can
    // accidentally free a page used by either hierarchy.
    var next = records.len;
    for ([_][]map.Table{ image_pds, image_pts }) |tables| for (tables) |*table| {
        table.* = try provider.take();
        @memset(table.words, 0);
        reservations[next] = .fromInt(table.physical);
        next += 1;
    };
    _ = try draft.?.writeReservations(reservations[0..records.len]);
    std.sort.heap(addresses.PhysicalAddress, reservations, {}, struct {
        fn less(_: void, a: addresses.PhysicalAddress, b: addresses.PhysicalAddress) bool {
            return a.toInt() < b.toInt();
        }
    }.less);
    layout = input;
    prepared = scratch_extent;
    return reservations;
}

pub fn rebase() void {
    // Called immediately after loading our root. No Limine view escapes here.
    for (draft.?.tables) |*table| table.words = @ptrFromInt(map.base + table.physical);
    for ([_][]map.Table{ image_pds, image_pts }) |tables| for (tables) |*table| {
        table.words = @ptrFromInt(map.base + table.physical);
    };
    // Report the leaf plan actually used to construct this published draft;
    // the no-1GiB integration run must prove the fallback, not just allocation.
    var counts = [_]usize{ 0, 0, 0 };
    var leaves = layout.?.iterator();
    while (leaves.next()) |leaf| {
        const index: usize = switch (leaf.size) {
            .huge => 0,
            .large => 1,
            .page => 2,
        };
        counts[index] += 1;
    }
    @import("../../kernel_log.zig").writeFmt("direct-map: active slots=256..509 tables={} limit=0x{x} huge={} leaves-1G={} leaves-2M={} leaves-4K={}\n", .{ draft.?.tables.len, layout.?.limit, layout.?.huge_pages, counts[0], counts[1], counts[2] });
}

pub fn prepareReturn(pa: u64, bytes: usize) !void {
    if (draft == null or !draft.?.published or bytes == 0 or pa < prepared.start or
        bytes > prepared.end -| pa or (pa | bytes) & 4095 != 0) return error.InvalidState;
    var offset: usize = 0;
    while (offset < bytes) : (offset += 4096) {
        const value = (try draft.?.preparedLeaf(pa + offset)).*;
        if (value != 0 and (value & ~@as(u64, 0x60)) != ((pa + offset) | (@as(u64, 1) << 63) | 3)) return error.InvalidState;
    }
}

pub fn publishReturn(pa: u64, bytes: usize) void {
    // Validation and image TLB ACK precede this non-fallible publication.
    // No CPU could previously have a present translation for these leaves.
    var offset: usize = 0;
    while (offset < bytes) : (offset += 4096) {
        const leaf = draft.?.preparedLeaf(pa + offset) catch unreachable;
        @atomicStore(u64, leaf, (pa + offset) | (@as(u64, 1) << 63) | 3, .release);
    }
    layout.?.returned = .{ .start = pa, .end = pa + bytes };
}

test "scratch direct publication rejects bad extent and preserves absent image neighbors" {
    const saved_draft = draft;
    const saved_layout = layout;
    const saved_prepared = prepared;
    defer {
        draft = saved_draft;
        layout = saved_layout;
        prepared = saved_prepared;
    }
    const storage = try std.testing.allocator.alignedAlloc(u64, .fromByteUnits(4096), 3 * 512);
    defer std.testing.allocator.free(storage);
    @memset(storage, 0);
    var tables: [3]map.Table = undefined;
    for (&tables, 0..) |*table, i| table.* = .{ .physical = (20 << 30) + i * 4096, .words = @ptrCast(@alignCast(storage.ptr + i * 512)) };
    const flags = (@as(u64, 1) << 63) | 3;
    draft = .{ .tables = &tables, .published = true };
    draft.?.roots[0] = tables[0].physical | flags;
    tables[0].words[0] = tables[1].physical | flags;
    tables[1].words[2] = tables[2].physical | flags;
    layout = try map.Layout.init(&.{}, &.{}, 48, true);
    prepared = .{ .start = 4 << 20, .end = (4 << 20) + 8192 };
    const tail = prepared.end - 4096;
    try std.testing.expectError(error.InvalidState, prepareReturn(tail, 8192));
    try std.testing.expectEqual(@as(u64, 0), tables[2].words[1]);
    try prepareReturn(tail, 4096);
    publishReturn(tail, 4096);
    try std.testing.expectEqual(@as(u64, tail | flags), tables[2].words[1]);
    try std.testing.expectEqual(@as(u64, 0), tables[2].words[0]);
    try std.testing.expect(layout.?.contains(tail, 4096));
    try std.testing.expect(!layout.?.contains(prepared.start, 4096));
    // A PMM return failure may retry the owned, already-published WB leaf.
    try prepareReturn(tail, 4096);
    tables[2].words[1] |= 8;
    try std.testing.expectError(error.InvalidState, prepareReturn(tail, 4096));
}

// Scratch shares [511] with the image and AP stacks, but occupies only PDP
// slots below the image. The VA aperture is an architectural bound, not backing.
pub const scratch_base: u64 = 0xffff_ff80_0000_0000;
var scratch_mapped: usize = 0;
var scratch_limit: usize = 0;
var scratch_roots: [512]u64 = @splat(0);
var scratch_live_pdp: u64 = 0;
var scratch_native = false;
var scratch_pmm: ?*@import("../../kernel.zig").FreePageList = null;
var boot_cursor: ?map.Reservations = null;
var boot_view: ?hhdm.View = null;
var boot_image: [1]map.Extent = undefined;
const ReservationPage = extern struct {
    next: u64,
    count: usize,
    pages: [510]u64,
};
var reservation_head: u64 = 0;
var reservation_tail: u64 = 0;
var reservation_count: usize = 0;

fn bootWords(pa: u64) *align(4096) [512]u64 {
    // Only pages checked at reservation, or the explicitly checked live PDP,
    // reach this conversion. Rebase discards the Limine view before runtime.
    if (@import("builtin").is_test) return @ptrCast(@alignCast((@import("../../memory/physical_access_test.zig").slice(u64, pa, 512) orelse unreachable).ptr));
    if (scratch_native) return @alignCast(@import("physical_layout.zig").tablePointer(.fromInt(pa)).?);
    return @ptrFromInt(boot_view.?.base + pa);
}
fn reservationPage(pa: u64) *ReservationPage {
    return @ptrCast(bootWords(pa));
}
const ScratchTest = if (@import("builtin").is_test) struct {
    var take: ?*const fn () anyerror!u64 = null;
} else struct {};
fn rawBootPage() !u64 {
    if (@import("builtin").is_test) if (ScratchTest.take) |take| return take();
    return boot_cursor.?.take(boot_view.?);
}
fn takeBootPage() !u64 {
    if (reservation_tail == 0 or reservationPage(reservation_tail).count == 510) {
        // Bookkeeping reserves its own page without recursion through scratch.
        const pa = try rawBootPage();
        const record = reservationPage(pa);
        record.* = .{ .next = 0, .count = 1, .pages = @splat(0) };
        record.pages[0] = pa;
        if (reservation_tail != 0) reservationPage(reservation_tail).next = pa else reservation_head = pa;
        reservation_tail = pa;
        reservation_count += 1;
    }
    const pa = try rawBootPage();
    const record = reservationPage(reservation_tail);
    record.pages[record.count] = pa;
    record.count += 1;
    reservation_count += 1;
    return pa;
}
fn scratchPage() !u64 {
    if (scratch_pmm) |pmm| return pmm.popFront();
    return takeBootPage();
}
fn scratchTable() !u64 {
    const pa = try scratchPage();
    @memset(bootWords(pa), 0);
    return pa;
}

pub fn installScratch(source: *const fn (usize) ?map.Region, bits: u8, offset: u64) !void {
    if (boot_cursor != null) return error.InvalidState;
    const image = @import("../../boot/image_range.zig");
    try map.validateImage(image.virtual_base, image.size_bytes, 0xffff_ffff_c000_0000, 1 << 30);
    const first_image_slot = (image.virtual_base >> 30) & 511;
    if (first_image_slot == 0) return error.InvalidImage;
    scratch_limit = @intCast(first_image_slot << 30);
    const view = hhdm.current(offset) orelse return error.InvalidState;
    const limit = try map.physicalLimit(bits);
    boot_image[0] = .{ .start = image.base_paddr & ~@as(u64, 4095), .end = std.mem.alignForward(u64, image.base_paddr + image.size_bytes, 4096) };
    var cursor = map.Reservations.init(try map.Layout.init(&.{}, &boot_image, bits, false));
    cursor.leaves.source = source;
    cursor.leaves.cursor = 64 << 20; // preserve firmware/AP low-memory ownership
    // Validate the complete source before the first destructive reservation.
    var previous: u64 = 0;
    var index: usize = 0;
    while (source(index)) |region| : (index += 1) {
        const end = try std.math.add(u64, region.base, region.bytes);
        if (region.base < previous or (region.usable and end > limit)) return error.InvalidMap;
        previous = end;
    }
    if (!view.contains(view.root & 0x000f_ffff_ffff_f000, 4096)) return error.UnmappedLimineRoot;
    const root: *[512]u64 = @ptrFromInt(view.base + (view.root & 0x000f_ffff_ffff_f000));
    if (root[511] & 0x83 != 3) {
        @import("../../kernel_log.zig").writeFmt("scratch: Limine image root=0x{x}\n", .{root[511]});
        return error.InvalidLimineImageRoot;
    }
    const pdp_pa = root[511] & 0x000f_ffff_ffff_f000;
    if (!view.contains(pdp_pa, 4096)) return error.UnmappedLimineImagePdp;
    const pdp: *[512]u64 = @ptrFromInt(view.base + pdp_pa);
    for (pdp[0..first_image_slot]) |entry| if (entry != 0) return error.OccupiedRoot;
    // Limine leaves U/S set on intermediate image tables. No user or AP runs
    // yet: tighten the shared image parent before publishing scratch children,
    // then discard cached permissions. The HHDM proof above covers this write.
    root[511] &= ~@as(u64, 4);
    asm volatile ("mov %[root], %%cr3"
        :
        : [root] "r" (view.root),
        : .{ .memory = true });
    boot_cursor = cursor;
    boot_view = view;
    scratch_live_pdp = pdp_pa;
    try scratch.installGrowing(.{ .context = &scratch_mapped, .ensure = growScratch });
}

fn growScratch(_: *anyopaque, needed: usize) ?[]align(4096) u8 {
    if (needed > scratch_limit) return null;
    const rounded = std.mem.alignForward(usize, needed, 4096);
    const flags: u64 = (@as(u64, 1) << 63) | 3;
    while (scratch_mapped < rounded) {
        const va = scratch_base + scratch_mapped;
        const pdp_index = (va >> 30) & 511;
        if (scratch_roots[pdp_index] == 0) {
            const pd = scratchTable() catch return null;
            scratch_roots[pdp_index] = pd | flags;
            bootWords(scratch_live_pdp)[pdp_index] = pd | flags;
        }
        const pd = bootWords(scratch_roots[pdp_index] & 0x000f_ffff_ffff_f000);
        const pd_index = (va >> 21) & 511;
        if (pd[pd_index] == 0) {
            const pt = scratchTable() catch return null;
            pd[pd_index] = pt | flags;
        }
        const pt = bootWords(pd[pd_index] & 0x000f_ffff_ffff_f000);
        const leaf = &pt[(va >> 12) & 511];
        if (leaf.* != 0) return null;
        const page = scratchPage() catch return null;
        leaf.* = page | flags;
        scratch_mapped += 4096;
    }
    const bytes: [*]align(4096) u8 = @ptrFromInt(scratch_base);
    return bytes[0..scratch_mapped];
}

pub fn installScratchRoot(pdp: *[512]u64, physical: u64) !void {
    const slots = scratch_limit >> 30;
    for (pdp[0..slots]) |entry| if (entry != 0) return error.OccupiedRoot;
    @memcpy(pdp[0..slots], scratch_roots[0..slots]);
    scratch_live_pdp = physical;
}
/// The BSP runtime owner grows downward while temporary boot storage grows
/// upward. Reserve whole PDP ownership before installing runtime children so
/// scratch growth can never overwrite another owner's live parent link.
pub fn reserveRuntimeBoundary(low: u64) !void {
    if (!scratch_native or low < scratch_base) return error.InvalidState;
    const limit = (low - scratch_base) & ~@as(u64, (1 << 30) - 1);
    if (limit < scratch_mapped or limit > scratch_limit) return error.OutOfVirtualAddressSpace;
    scratch_limit = @intCast(limit);
}

pub fn scratchRootMatches(index: usize, entry: u64) bool {
    return index < scratch_limit >> 30 and
        entry & ~@as(u64, 1 << 5) == scratch_roots[index];
}

/// The final snapshot includes its own backing/table reservations. This runs
/// BEFORE seeding; no boot allocator may be used once the snapshot is handed off.
pub fn handoffScratch(pmm: *@import("../../kernel.zig").FreePageList) ![]const addresses.PhysicalAddress {
    if (scratch_pmm != null) return error.InvalidState;
    // Array bytes plus their own data/PT pages, recursively rounded to pages.
    var capacity = reservation_count;
    while (true) {
        const bytes = try std.math.mul(usize, capacity, @sizeOf(addresses.PhysicalAddress));
        const pages = (try std.math.add(usize, bytes, 4095 + 64)) / 4096;
        const extra = pages + (pages + 511) / 512 + (pages + 262143) / 262144 + (pages + 509) / 510 + 4;
        const required = try std.math.add(usize, reservation_count, extra);
        if (required <= capacity) break;
        capacity = required;
    }
    const output = try metadata(addresses.PhysicalAddress, capacity);
    if (output.len < reservation_count) return error.InvalidState;
    var n: usize = 0;
    var cursor = reservation_head;
    while (cursor != 0) {
        const record = reservationPage(cursor);
        for (record.pages[0..record.count]) |pa| {
            output[n] = .fromInt(pa);
            n += 1;
        }
        cursor = record.next;
    }
    std.sort.heap(addresses.PhysicalAddress, output[0..n], {}, struct {
        fn less(_: void, a: addresses.PhysicalAddress, b: addresses.PhysicalAddress) bool {
            return a.toInt() < b.toInt();
        }
    }.less);
    scratch_pmm = pmm;
    boot_cursor = null;
    return output[0..n];
}

pub fn rebaseScratch() void {
    scratch_native = true;
    boot_view = null;
    // PMM now owns all unreserved usable pages. Reservation records no longer
    // carry publication authority; save next before intrusive return overwrites.
    while (reservation_head != 0) {
        const page = reservation_head;
        const next = reservationPage(page).next;
        scratch_pmm.?.appendPage(page) catch @panic("boot reservation ledger return");
        reservation_head = next;
    }
    reservation_tail = 0;
}

fn scratchLeaf(va: u64) ?*u64 {
    if (va < scratch_base or va - scratch_base >= scratch_mapped) return null;
    const root = scratch_roots[(va >> 30) & 511];
    if (root & 1 == 0) return null;
    const pd = bootWords(root & 0x000f_ffff_ffff_f000);
    const parent = pd[(va >> 21) & 511];
    if (parent & 1 == 0 or parent & 128 != 0) return null;
    return &bootWords(parent & 0x000f_ffff_ffff_f000)[(va >> 12) & 511];
}
pub fn scratchPhysical(va: u64) ?addresses.PhysicalAddress {
    const leaf = scratchLeaf(va) orelse return null;
    if (leaf.* & 1 == 0) return null;
    return .fromInt((leaf.* & 0x000f_ffff_ffff_f000) + (va & 4095));
}
pub const ScratchRetirement = struct { virtual: addresses.KernelVirtualAddress, bytes: usize };
var scratch_retirement: ?ScratchRetirement = null;
var scratch_withdrawn = false;
var scratch_acknowledged = false;
pub fn prepareScratchRetirement(va: usize, bytes: usize) !ScratchRetirement {
    if (!scratch_native or scratch_retirement != null or va < scratch_base or bytes == 0 or
        (va | bytes) & 4095 != 0 or va - scratch_base > scratch_mapped or bytes != scratch_mapped - (va - scratch_base)) return error.InvalidState;
    var offset: usize = 0;
    while (offset < bytes) : (offset += 4096) {
        const value = (scratchLeaf(va + offset) orelse return error.InvalidTable).*;
        if (value & ~@as(u64, 0x000f_ffff_ffff_f060) != ((@as(u64, 1) << 63) | 3)) return error.InvalidTable;
    }
    const result = ScratchRetirement{ .virtual = .fromInt(va), .bytes = bytes };
    scratch_retirement = result;
    return result;
}
fn matchesRetirement(va: usize, bytes: usize) bool {
    const owned = scratch_retirement orelse return false;
    return owned.virtual.toInt() == va and owned.bytes == bytes;
}
pub fn withdrawScratch(va: usize, bytes: usize) !void {
    if (!matchesRetirement(va, bytes) or scratch_withdrawn) return error.InvalidState;
    var offset: usize = 0;
    while (offset < bytes) : (offset += 4096) {
        const leaf = scratchLeaf(va + offset) orelse return error.InvalidTable;
        // Preserve the owned PFN in a non-present leaf until all old views ACK.
        @atomicStore(u64, leaf, leaf.* & ~@as(u64, 1), .release);
    }
    scratch_withdrawn = true;
}
pub fn acknowledgeScratch(va: usize, bytes: usize) !void {
    if (!matchesRetirement(va, bytes) or !scratch_withdrawn or scratch_acknowledged) return error.InvalidState;
    scratch_acknowledged = true;
}
pub fn releaseScratch(pmm: *@import("../../kernel.zig").FreePageList, owned: ScratchRetirement, pages: usize) !void {
    if (!matchesRetirement(owned.virtual.toInt(), owned.bytes) or !scratch_acknowledged or pages != owned.bytes / 4096) return error.InvalidState;
    for (0..pages) |index| {
        const leaf = scratchLeaf(owned.virtual.toInt() + index * 4096) orelse return error.InvalidTable;
        const pa = leaf.* & 0x000f_ffff_ffff_f000;
        if (pa == 0 or leaf.* & 1 != 0) return error.InvalidTable;
        try pmm.appendPage(pa);
        leaf.* = 0;
    }
    // Table backing is still owned by the kernel root; no dangling PFN view
    // survives reuse. Subsequent conversion rejects every retired scratch VA.
    scratch_mapped -= owned.bytes;
}

test "MMIO overlay scratch scattered backing owns reservations and rejects return before TLB ACK" {
    const physical_test = @import("../../memory/physical_access_test.zig");
    var backing = try physical_test.BackingSet.init(&.{.{ .first = 20 << 30, .pages = 2048 }});
    defer backing.deinit();
    const Provider = struct {
        var next: u64 = 20 << 30;
        var fail: bool = false;
        fn take() !u64 {
            if (fail) return error.NoSpace;
            const pa = next;
            next += 8192; // deliberately non-contiguous scratch backing
            return pa;
        }
    };
    Provider.next = 20 << 30;
    Provider.fail = false;
    ScratchTest.take = Provider.take;
    defer ScratchTest.take = null;
    scratch_mapped = 0;
    scratch_limit = 1 << 30;
    scratch_roots = @splat(0);
    scratch_native = false;
    scratch_pmm = null;
    reservation_head = 0;
    reservation_tail = 0;
    reservation_count = 0;
    scratch_retirement = null;
    scratch_withdrawn = false;
    scratch_acknowledged = false;
    defer {
        scratch_limit = 0;
        scratch_mapped = 0;
        scratch_roots = @splat(0);
        reservation_head = 0;
        reservation_tail = 0;
        reservation_count = 0;
        scratch_retirement = null;
        scratch_native = false;
        scratch_pmm = null;
    }
    scratch_live_pdp = try scratchTable();
    _ = growScratch(&scratch_mapped, 513 * 4096) orelse return error.TestUnexpectedResult;
    try std.testing.expect(reservation_count > 510);
    try std.testing.expectEqual((Provider.next - (20 << 30)) / 8192, reservation_count);
    const first = scratchPhysical(scratch_base).?.toInt();
    const second = scratchPhysical(scratch_base + 4096).?.toInt();
    try std.testing.expectEqual(first + 8192, second);
    try std.testing.expect(scratchRootMatches(0, bootWords(scratch_live_pdp)[0]));
    try std.testing.expect(!scratchRootMatches(0, bootWords(scratch_live_pdp)[0] | 4));
    const before = scratch_mapped;
    Provider.fail = true;
    try std.testing.expect(growScratch(&scratch_mapped, before + 4096) == null);
    try std.testing.expectEqual(before, scratch_mapped);
    try std.testing.expectEqual(first, scratchPhysical(scratch_base).?.toInt());
    Provider.fail = false;
    scratch_native = true;
    var list = @import("../../kernel.zig").FreePageList{};
    try list.installRegions(&.{.{ .id = 1, .start = 20 << 30, .end = (20 << 30) + 2048 * 4096 }});
    const retained = 4096;
    const tail = scratch_base + retained;
    const bytes = scratch_mapped - retained;
    try std.testing.expectError(error.InvalidState, prepareScratchRetirement(tail, bytes - 4096));
    const owned = try prepareScratchRetirement(tail, bytes);
    try std.testing.expectError(error.InvalidState, releaseScratch(&list, owned, bytes / 4096));
    try withdrawScratch(tail, bytes);
    try std.testing.expect(scratchPhysical(tail) == null);
    try std.testing.expectEqual(first, scratchPhysical(scratch_base).?.toInt());
    try std.testing.expectError(error.InvalidState, releaseScratch(&list, owned, bytes / 4096));
    try std.testing.expectEqual(@as(usize, 0), list.pageCount());
    try acknowledgeScratch(tail, bytes);
    try releaseScratch(&list, owned, bytes / 4096);
    try std.testing.expectEqual(@as(usize, 512), list.pageCount());
    try std.testing.expectEqual(second, try list.popFront());
    try std.testing.expect(scratchPhysical(tail) == null);
    try std.testing.expectEqual(first, scratchPhysical(scratch_base).?.toInt());
}

test "scratch direct runtime boundary cannot overwrite active scratch and limits future growth" {
    const old_native = scratch_native;
    const old_mapped = scratch_mapped;
    const old_limit = scratch_limit;
    defer {
        scratch_native = old_native;
        scratch_mapped = old_mapped;
        scratch_limit = old_limit;
    }
    scratch_native = true;
    scratch_mapped = (1 << 30) + 4096;
    scratch_limit = 4 << 30;
    try reserveRuntimeBoundary(scratch_base + (2 << 30) + 4096);
    try std.testing.expectEqual(@as(usize, 2 << 30), scratch_limit);
    try std.testing.expectError(error.OutOfVirtualAddressSpace, reserveRuntimeBoundary(scratch_base + (1 << 30) + 4096));
    try std.testing.expectEqual(@as(usize, 2 << 30), scratch_limit);
    try std.testing.expectError(error.OutOfVirtualAddressSpace, reserveRuntimeBoundary(scratch_base + (3 << 30)));
    try std.testing.expect(growScratch(&scratch_mapped, (2 << 30) + 1) == null);
    scratch_native = false;
    try std.testing.expectError(error.InvalidState, reserveRuntimeBoundary(scratch_base + (2 << 30)));
}
