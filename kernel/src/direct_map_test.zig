const std = @import("std");
const dm = @import("arch/x86_64/direct_map.zig");
const hhdm = @import("arch/x86_64/boot_hhdm.zig");
const paging = @import("memory/ram_cache_paging.zig");
const nx: u64 = @as(u64, 1) << 63;
const mask: u64 = 0x000f_ffff_ffff_f000;

const Page = struct { words: [512]u64 align(4096) };

const Provider = struct {
    pages: std.ArrayList(dm.Table) = .empty,
    attempts: usize = 0,
    fail_at: usize = std.math.maxInt(usize),
    next_pa: u64 = 20 << 30,

    pub fn take(self: *Provider) !dm.Table {
        self.attempts += 1;
        if (self.attempts == self.fail_at) return error.Injected;
        const allocation = try std.testing.allocator.create(Page);
        const words = &allocation.words;
        errdefer std.testing.allocator.destroy(allocation);
        const result = dm.Table{ .physical = self.next_pa, .words = words };
        try self.pages.append(std.testing.allocator, result);
        self.next_pa += 4096;
        return result;
    }

    pub fn release(self: *Provider, released: dm.Table) void {
        for (self.pages.items, 0..) |item, index| {
            if (item.physical == released.physical) {
                std.testing.allocator.destroy(@as(*Page, @fieldParentPtr("words", item.words)));
                _ = self.pages.swapRemove(index);
                return;
            }
        }
        unreachable;
    }

    fn deinit(self: *Provider) void {
        for (self.pages.items) |page| std.testing.allocator.destroy(@as(*Page, @fieldParentPtr("words", page.words)));
        self.pages.deinit(std.testing.allocator);
    }

    fn table(self: *Provider, physical: u64) *[512]u64 {
        for (self.pages.items) |page| if (page.physical == physical) return page.words;
        unreachable;
    }

    fn walk(self: *Provider, root: *const [512]u64, va: u64) ?struct { physical: u64, shift: u6, flags: u64 } {
        var current = root;
        for ([_]u6{ 39, 30, 21, 12 }) |shift| {
            const entry = current[(va >> shift) & 511];
            if (entry & 1 == 0) return null;
            // Check supervisor and NX at every parent, not only the leaf.
            std.debug.assert(entry & 4 == 0 and entry & nx != 0);
            if (shift == 12 or (shift != 39 and entry & 128 != 0)) {
                const low_mask = (@as(u64, 1) << shift) - 1;
                return .{ .physical = (entry & mask & ~low_mask) | (va & low_mask), .shift = shift, .flags = entry };
            }
            current = self.table(entry & mask);
        }
        unreachable;
    }
};

test "direct map builds sparse usable RAM including above 16 GiB with exact table budget" {
    const regions = [_]dm.Region{
        .{ .base = 1, .bytes = (2 << 20) - 1, .usable = true },
        .{ .base = 2 << 20, .bytes = 2 << 20, .usable = false },
        .{ .base = 4 << 20, .bytes = 2 << 20, .usable = true },
        .{ .base = 16 << 30, .bytes = 8 << 30, .usable = true },
    };
    const excluded = [_]dm.Extent{.{ .start = (17 << 30) + 4096, .end = (17 << 30) + 8192 }};
    for ([_]bool{ true, false }) |huge| {
        const layout = try dm.Layout.init(&regions, &excluded, 48, huge);
        const records = try std.testing.allocator.alloc(dm.Table, layout.tableCount());
        defer std.testing.allocator.free(records);
        var provider = Provider{};
        defer provider.deinit();
        var draft = try dm.Draft.build(layout, records, &provider);
        try std.testing.expectEqual(layout.tableCount(), provider.pages.items.len);
        var root: [512]u64 = @splat(0);
        root[0] = 0x123;
        root[1] = 0x456;
        root[510] = 0x789;
        root[511] = 0xabc;
        try draft.publish(&root);
        try draft.validateRoot(&root);
        root[256] |= 1 << 5;
        try draft.validateRoot(&root);
        root[256] ^= 4096;
        try std.testing.expectError(error.InvalidTable, draft.validateRoot(&root));
        root[256] ^= 4096;
        try std.testing.expectEqual(@as(u64, 0x123), root[0]);
        try std.testing.expectEqual(@as(u64, 0x456), root[1]);
        try std.testing.expectEqual(@as(u64, 0x789), root[510]);
        try std.testing.expectEqual(@as(u64, 0xabc), root[511]);
        for ([_]u64{ 0, 2 << 20, (4 << 20) - 4096, 6 << 20, (17 << 30) + 4096, 24 << 30 }) |pa|
            try std.testing.expect(provider.walk(&root, dm.base + pa) == null);
        for ([_]u64{ 4096, (2 << 20) - 1, 4 << 20, (6 << 20) - 1, 16 << 30, (17 << 30) - 1, (17 << 30) + 8192 }) |pa| {
            const leaf = provider.walk(&root, dm.base + pa).?;
            try std.testing.expectEqual(pa, leaf.physical);
            try std.testing.expectEqual(@as(u64, 0), leaf.flags & (4 | 8 | 16 | 256));
        }
        try std.testing.expectEqual(@as(u6, if (huge) 30 else 21), provider.walk(&root, dm.base + (16 << 30)).?.shift);
        try std.testing.expectEqual(@as(u6, 12), provider.walk(&root, dm.base + (17 << 30)).?.shift);
        try std.testing.expectEqual(@as(u6, 21), provider.walk(&root, dm.base + (4 << 20)).?.shift);
        try std.testing.expectError(error.OccupiedRoot, draft.publish(&root));
    }
}

test "direct map validates physical limits overlaps partial pages and image stack collision" {
    try std.testing.expectEqual(@as(u64, 0xffff_ff00_0000_0000), dm.io_base);
    try std.testing.expectEqual(@as(u64, 1) << 36, try dm.physicalLimit(36));
    try std.testing.expectEqual(dm.aperture_bytes, try dm.physicalLimit(52));
    for ([_]u8{ 0, 31, 53, 64 }) |bits| try std.testing.expectError(error.PhysicalLimit, dm.physicalLimit(bits));
    try std.testing.expectError(error.PhysicalLimit, dm.Layout.init(&.{.{ .base = 1 << 36, .bytes = 4096, .usable = true }}, &.{}, 36, true));
    try std.testing.expectError(error.PhysicalLimit, dm.Layout.init(&.{.{ .base = dm.aperture_bytes, .bytes = 4096, .usable = true }}, &.{}, 52, true));
    try std.testing.expectError(error.InvalidMap, dm.Layout.init(&.{.{ .base = std.math.maxInt(u64), .bytes = 1, .usable = false }}, &.{}, 48, true));
    try std.testing.expectError(error.InvalidMap, dm.Layout.init(&.{
        .{ .base = 4096, .bytes = 8192, .usable = true },
        .{ .base = 8192, .bytes = 4096, .usable = false },
    }, &.{}, 48, true));
    try std.testing.expectError(error.InvalidExclusion, dm.Layout.init(&.{}, &.{.{ .start = 4097, .end = 8192 }}, 48, true));
    const layout = try dm.Layout.init(&.{.{ .base = 4097, .bytes = 8192, .usable = true }}, &.{}, 48, true);
    var it = layout.iterator();
    try std.testing.expectEqual(@as(u64, 8192), it.next().?.physical);
    try std.testing.expect(it.next() == null);
    try dm.validateImage(0xffff_ffff_8000_0000, 0x4000_0000, 0xffff_ffff_c000_0000, 1 << 30);
    try std.testing.expectError(error.InvalidImage, dm.validateImage(0xffff_ffff_8000_0000, 0x4000_0001, 0xffff_ffff_c000_0000, 1 << 30));
}

test "direct map boot reservations skip HHDM gaps without excluding RAM from mapping" {
    const layout = try dm.Layout.init(&.{.{ .base = 4 << 30, .bytes = (1 << 30) + 8192, .usable = true }}, &.{}, 48, true);
    var reservations = dm.Reservations.init(layout);
    const view = hhdm.Fake.reset();
    hhdm.Fake.pdp[4] = 0;
    try std.testing.expectEqual(@as(u64, 5) << 30, try reservations.take(view));
    try std.testing.expectEqual((@as(u64, 5) << 30) + 4096, try reservations.take(view));
    try std.testing.expectError(error.NoSpace, reservations.take(view));
    var it = layout.iterator();
    try std.testing.expectEqual(@as(u64, 4) << 30, it.next().?.physical);
    var unreadable = dm.Reservations.init(layout);
    hhdm.Fake.table_missing = true;
    try std.testing.expectError(error.NoSpace, unreadable.take(view));
}

test "direct map failed allocation and occupied root never publish partial mappings" {
    const layout = try dm.Layout.init(&.{.{ .base = 4096, .bytes = 12288, .usable = true }}, &.{}, 48, true);
    const records = try std.testing.allocator.alloc(dm.Table, layout.tableCount());
    defer std.testing.allocator.free(records);
    for (1..layout.tableCount() + 1) |failure| {
        var provider = Provider{ .fail_at = failure, .next_pa = 4096 };
        defer provider.deinit();
        try std.testing.expectError(error.Injected, dm.Draft.build(layout, records, &provider));
        try std.testing.expectEqual(@as(usize, 0), provider.pages.items.len);
    }
    var provider = Provider{ .next_pa = 4096 };
    defer provider.deinit();
    try std.testing.expectError(error.NoSpace, dm.Draft.build(layout, records[0 .. records.len - 1], &provider));
    try std.testing.expectEqual(@as(usize, 0), provider.attempts);
    var draft = try dm.Draft.build(layout, records, &provider);
    var root: [512]u64 = @splat(0);
    root[509] = 1;
    try std.testing.expectError(error.OccupiedRoot, draft.publish(&root));
    try std.testing.expectEqual(@as(u64, 0), root[256]);
    draft.discard(&provider);
    try std.testing.expectEqual(@as(usize, 0), provider.pages.items.len);
}

test "direct map huge split preserves PAT and permissions through both levels" {
    var pd: [512]u64 align(4096) = undefined;
    var pt: [512]u64 align(4096) = undefined;
    const pa: u64 = 16 << 30;
    for (0..8) |pat| {
        for ([_]u64{ 0, 2 }) |rw| {
            var parent = pa | nx | 1 | 128 | rw | (1 << 10) |
                (@as(u64, @intCast(pat & 3)) << 3) | (@as(u64, @intCast(pat >> 2)) << 12);
            const huge = try paging.IdentitySplit.prepareSized(&parent, pa, .huge, &pd, 0x2000);
            const large = try paging.IdentitySplit.prepareSized(&pd[3], pa + 3 * (1 << 21), .large, &pt, 0x3000);
            for (pd, 0..) |entry, i| {
                try std.testing.expectEqual(pa + i * (1 << 21), entry & mask & ~@as(u64, (1 << 21) - 1));
                try std.testing.expectEqual(parent & (nx | 0x1c9f), entry & (nx | 0x1c9f));
            }
            for (pt, 0..) |entry, i| {
                try std.testing.expectEqual(pa + 3 * (1 << 21) + i * 4096, entry & mask);
                try std.testing.expectEqual(@as(u64, @intCast((pat & 3) << 3 | (pat >> 2) << 7)), entry & 0x98);
                try std.testing.expectEqual(nx | rw | 1 | (1 << 10), entry & (nx | 7 | (1 << 10)));
            }
            // Publish children before linking the parent. Publication of the
            // outer split must not overwrite the prepared child-table link.
            large.publishEquivalent();
            huge.publishEquivalent();
            try std.testing.expectEqual(@as(u64, 0x3000), pd[3] & mask);
            try std.testing.expectEqual(@as(u64, 0), parent & (128 | 8 | 16));
        }
    }
}

test "direct map and identity leaves withdraw together before UC WC publication" {
    // Exercise the same Leaf primitives the native transaction uses. This is
    // deliberately not evidence that the runtime enumerates the new map yet.
    for ([_]u3{ 3, 4 }) |pat| {
        const pa: u64 = 20 << 30;
        var identity: u64 = pa | nx | 3;
        var direct: u64 = pa | nx | 3;
        var user: u64 = pa | nx | 7;
        var aliases = [_]paging.Leaf{
            try paging.Leaf.capture(&identity),
            try paging.Leaf.capture(&direct),
            try paging.Leaf.capture(&user),
        };
        for (&aliases) |*alias| alias.withdraw();
        for ([_]u64{ identity, direct, user }) |entry| try std.testing.expectEqual(@as(u64, 0), entry & 1);
        // A failure before the external ACK must leave all leaves withdrawn.
        // No leaf publication is attempted in that state. Once ACK is known,
        // all aliases receive the same PAT encoding, including the WB restore.
        for (&aliases) |*alias| alias.publish(pat);
        const expected = (@as(u64, pat & 3) << 3) | (@as(u64, pat >> 2) << 7);
        for ([_]u64{ identity, direct, user }) |entry| {
            try std.testing.expectEqual(expected, entry & 0x98);
            try std.testing.expectEqual(@as(u64, 1), entry & 1);
        }
        aliases = .{
            try paging.Leaf.capture(&identity),
            try paging.Leaf.capture(&direct),
            try paging.Leaf.capture(&user),
        };
        for (&aliases) |*alias| alias.withdraw();
        for (&aliases) |*alias| alias.publish(0);
        for ([_]u64{ identity, direct, user }) |entry| try std.testing.expectEqual(@as(u64, 0), entry & 0x98);
    }
}

test "direct map invalid huge split never changes parent or unlinked table" {
    var table: [512]u64 align(4096) = @splat(0xbeef);
    const pa: u64 = 16 << 30;
    var parent: u64 = pa | nx | 0x83;
    try std.testing.expectError(error.Invalid, paging.IdentitySplit.prepareSized(&parent, pa + (1 << 21), .huge, &table, 4096));
    try std.testing.expectError(error.Invalid, paging.IdentitySplit.prepareSized(&parent, pa, .page, &table, 4096));
    try std.testing.expectEqual(pa | nx | 0x83, parent);
    parent |= 1 << 21; // Reserved address bit in a 1 GiB leaf.
    try std.testing.expectError(error.Invalid, paging.IdentitySplit.prepareSized(&parent, pa, .huge, &table, 4096));
    for (table) |word| try std.testing.expectEqual(@as(u64, 0xbeef), word);
}

test "direct map table storage grows beyond former split pool across PML4 slots" {
    const regions = try std.testing.allocator.alloc(dm.Region, 513);
    defer std.testing.allocator.free(regions);
    for (regions, 1..) |*region, index| region.* = .{
        .base = @as(u64, @intCast(index)) << 30,
        .bytes = 4 << 20,
        .usable = true,
    };
    const layout = try dm.Layout.init(regions, &.{}, 48, true);
    try std.testing.expectEqual(@as(usize, 515), layout.tableCount());
    const records = try std.testing.allocator.alloc(dm.Table, layout.tableCount());
    defer std.testing.allocator.free(records);
    var provider = Provider{};
    defer provider.deinit();
    var draft = try dm.Draft.build(layout, records, &provider);
    var root: [512]u64 = @splat(0);
    try draft.publish(&root);
    for ([_]u64{ 1 << 30, 511 << 30, 512 << 30, 513 << 30 }) |pa| {
        try std.testing.expectEqual(pa, provider.walk(&root, dm.base + pa).?.physical);
        try std.testing.expect(provider.walk(&root, dm.base + pa + (4 << 20)) == null);
    }
    try std.testing.expect(root[256] != 0 and root[257] != 0 and root[258] == 0);
}

test "scratch prepares absent leaves without mapping image or allocating reserved RAM" {
    const regions = [_]dm.Region{
        .{ .base = 4 << 20, .bytes = 32 << 20, .usable = false, .prepare_only = true },
        .{ .base = 20 << 30, .bytes = 1 << 30, .usable = true },
    };
    const layout = try dm.Layout.init(&regions, &.{}, 48, true);
    const records = try std.testing.allocator.alloc(dm.Table, layout.tableCount());
    defer std.testing.allocator.free(records);
    var provider = Provider{};
    defer provider.deinit();
    var draft = try dm.Draft.build(layout, records, &provider);
    var root: [512]u64 = @splat(0);
    try draft.publish(&root);
    var cursor: u64 = 4 << 20;
    while (cursor < 36 << 20) : (cursor += 4096) {
        try std.testing.expect(!layout.contains(cursor, 4096));
        try std.testing.expect(provider.walk(&root, dm.base + cursor) == null);
        try std.testing.expectEqual(@as(u64, 0), (try draft.preparedLeaf(cursor)).*);
    }
    var reserved = dm.Reservations.init(layout);
    const View = struct {
        pub fn inspect(_: @This(), pa: u64) hhdm.Result {
            return .{ .mapped = true, .next = pa + 4096 };
        }
    };
    try std.testing.expectEqual(@as(u64, 20) << 30, try reserved.take(View{}));
    const tail = (36 << 20) - 4096;
    (try draft.preparedLeaf(tail)).* = tail | nx | 3;
    try std.testing.expectEqual(@as(u64, tail), provider.walk(&root, dm.base + tail).?.physical);
    try std.testing.expect(provider.walk(&root, dm.base + tail - 4096) == null);
}

test {
    _ = @import("arch/x86_64/boot_direct_map.zig");
}

test "image sections build exact supervisor RX RO-NX RW-NX leaves across PT boundaries" {
    const start: u64 = 0xffff_ffff_8000_0000;
    const sections = dm.ImageSections{ .text = start, .rodata = start + (2 << 20), .data = start + (2 << 20) + 4096, .end = start + (2 << 20) + 12288 };
    var provider: Provider = .{};
    defer provider.deinit();
    var pds: [2]dm.Table = undefined;
    var pts: [2]dm.Table = undefined;
    for (&pds) |*table| table.* = try provider.take();
    for (&pts) |*table| table.* = try provider.take();
    var pdp: [512]u64 = undefined;
    try sections.install(&pdp, &pds, &pts, 64 << 20);
    try std.testing.expectEqual(@as(u64, 0), pdp[511]); // reserved AP stack window
    for ([_]u64{ start, sections.rodata - 4096, sections.rodata, sections.data, sections.end - 4096 }) |va| {
        var effective_user = true; // even a user-enabled ancestor cannot promote kernel children
        var effective_rw = true;
        var effective_nx = false;
        var words: *[512]u64 = &pdp;
        for ([_]u6{ 30, 21, 12 }) |shift| {
            const entry = words[(va >> shift) & 511];
            try std.testing.expect(entry & 1 != 0);
            effective_user = effective_user and entry & 4 != 0;
            effective_rw = effective_rw and entry & 2 != 0;
            effective_nx = effective_nx or entry & nx != 0;
            if (shift == 12) {
                try std.testing.expectEqual((64 << 20) + va - start, entry & mask);
            } else {
                try std.testing.expectEqual(@as(u64, 3), entry & ~mask);
                words = provider.table(entry & mask);
            }
        }
        try std.testing.expect(!effective_user);
        try std.testing.expectEqual(va >= sections.data, effective_rw);
        try std.testing.expectEqual(va >= sections.rodata, effective_nx);
    }
    try std.testing.expectEqual(@as(u64, 0), pts[1].words[3]);
    try std.testing.expectError(error.InvalidImage, sections.install(&pdp, &pds, pts[0..1], 64 << 20));
    var invalid = sections;
    invalid.end = invalid.text;
    try std.testing.expectError(error.InvalidImage, invalid.validate());
    invalid = sections;
    invalid.rodata += 1;
    try std.testing.expectError(error.InvalidImage, invalid.validate());
    invalid = sections;
    invalid.end = 0xffff_ffff_c000_1000;
    try std.testing.expectError(error.InvalidImage, invalid.validate());
}

test {
    _ = @import("arch/x86_64/cpu_runtime.zig");
}
