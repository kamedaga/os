const std = @import("std");

const present: u64 = 1;
const user: u64 = 1 << 2;
const large: u64 = 1 << 7;
const global: u64 = 1 << 8;
const accessed_dirty: u64 = (1 << 5) | (1 << 6);
const nx: u64 = 1 << 63;
const address_mask: u64 = 0x000f_ffff_ffff_f000;
pub const LeafSize = enum(u6) {
    page = 12,
    large = 21,
    huge = 30,

    pub fn bytes(self: LeafSize) u64 {
        return @as(u64, 1) << @intFromEnum(self);
    }

    pub fn child(self: LeafSize) SplitError!LeafSize {
        return switch (self) {
            .huge => .large,
            .large => .page,
            .page => error.Invalid,
        };
    }
};

pub const SplitError = error{ Invalid, Unsupported };

pub const AliasSource = union(enum) {
    leaf: *u64,
    identity: struct {
        directory: *u64,
        base: u64,
        size: LeafSize = .large,
        // Null retains the low identity alias. A direct-map alias has the
        // same physical base but a different page-table ownership tuple.
        alias_va: ?u64 = null,
    },
};

pub const Leaf = struct {
    entry: *u64,
    previous: u64,

    pub fn capture(entry: *u64) SplitError!Leaf {
        const value = @atomicLoad(u64, entry, .acquire);
        if ((value & present) == 0 or (value & global) != 0) return error.Unsupported;
        return .{ .entry = entry, .previous = value };
    }

    pub fn withdraw(self: *Leaf) void {
        const value = @atomicRmw(u64, self.entry, .And, ~present, .seq_cst);
        std.debug.assert((value & ~accessed_dirty) == (self.previous & ~accessed_dirty));
        self.previous = value;
    }

    /// The caller has completed the old-translation and cache barrier. Late
    /// A/D writes can still have reached the withdrawn slot during that wait.
    pub fn publish(self: *const Leaf, pat: u3) void {
        const value = @atomicLoad(u64, self.entry, .acquire);
        std.debug.assert((value & present) == 0 and
            (value & ~(present | accessed_dirty)) == (self.previous & ~(present | accessed_dirty)));
        const cache: u64 = (@as(u64, pat & 3) << 3) | (@as(u64, pat >> 2) << 7);
        @atomicStore(u64, self.entry, ((self.previous | (value & accessed_dirty)) & ~@as(u64, 0x98)) | cache, .release);
    }
};

pub fn validatePages(pages: []const u64, physical_limit: u64) SplitError!void {
    if (pages.len == 0) return error.Invalid;
    var previous: u64 = 0;
    for (pages) |pa| {
        if (pa == 0 or (pa & 4095) != 0 or pa <= previous or pa >= physical_limit)
            return error.Invalid;
        previous = pa;
    }
}

pub fn pagesIntersect(pages: []const u64, base: u64, bytes: u64) bool {
    var low: usize = 0;
    var high = pages.len;
    while (low < high) {
        const mid = low + (high - low) / 2;
        if (pages[mid] < base) low = mid + 1 else high = mid;
    }
    return low < pages.len and pages[low] - base < bytes;
}

/// A caller-owned, initially unlinked PT. Preparation touches only that PT;
/// once published, its storage must be retained by the kernel identity map or
/// the owning process PT store, never freed as transaction scratch.
pub const IdentitySplit = struct {
    directory: *u64,
    previous: u64,
    table: *align(4096) [512]u64,
    table_paddr: u64,

    pub fn validateSource(old: u64, identity_base: u64) SplitError!void {
        return validateSourceSized(old, identity_base, .large);
    }

    pub fn validateSourceSized(old: u64, physical_base: u64, size: LeafSize) SplitError!void {
        _ = try size.child();
        const large_bytes = size.bytes();
        if ((physical_base & (large_bytes - 1)) != 0) return error.Invalid;
        if ((old & (present | large)) != (present | large) or
            (old & (user | global)) != 0 or
            (old & ~(address_mask | nx | 0xfff)) != 0) return error.Unsupported;
        if ((old & address_mask & ~(large_bytes - 1)) != physical_base or
            (old & address_mask & (large_bytes - 1) & ~@as(u64, 1 << 12)) != 0)
            return error.Invalid;
    }

    pub fn prepare(directory: *u64, identity_base: u64, table: *align(4096) [512]u64, table_paddr: u64) SplitError!IdentitySplit {
        return prepareSized(directory, identity_base, .large, table, table_paddr);
    }

    /// Physical base is independent of the alias VA. A caller may prepare a
    /// 1 GiB -> 2 MiB -> 4 KiB chain while all tables are still unpublished.
    /// Ownership and the all-CPU invalidation barrier remain the caller's job.
    pub fn prepareSized(directory: *u64, physical_base: u64, size: LeafSize, table: *align(4096) [512]u64, table_paddr: u64) SplitError!IdentitySplit {
        const child_size = try size.child();
        const old = @atomicLoad(u64, directory, .acquire);
        if (table_paddr == 0 or (table_paddr & 4095) != 0 or
            (table_paddr & ~address_mask) != 0) return error.Invalid;
        const table_address = @intFromPtr(table);
        if (@intFromPtr(directory) >= table_address and
            @intFromPtr(directory) - table_address < 4096) return error.Invalid;
        try validateSourceSized(old, physical_base, size);
        // PAT stays at bit 12 for 2 MiB children, but moves to bit 7 for
        // 4 KiB children. Never copy a leaf's cache bits into its new parent.
        const pat = old & (1 << 12);
        const flags = (old & (nx | 0xfff) & ~large) |
            (if (child_size == .page) (if (pat != 0) large else @as(u64, 0)) else large | pat);
        for (table, 0..) |*entry, i|
            entry.* = physical_base + @as(u64, @intCast(i)) * child_size.bytes() | flags;
        return .{ .directory = directory, .previous = old, .table = table, .table_paddr = table_paddr };
    }

    /// Publish an equivalent mapping, not a cache transition. Caller excludes
    /// software PT writers; hardware A/D may still change until the old large
    /// translation is shot down, so all new leaves conservatively retain A/D.
    /// The subsequent transition must invalidate old large translations before
    /// draining caches or exposing any newly tagged leaf.
    pub fn publishEquivalent(self: *const IdentitySplit) void {
        const current = @atomicLoad(u64, self.directory, .acquire);
        std.debug.assert((current & ~accessed_dirty) == (self.previous & ~accessed_dirty));
        for (self.table) |*entry| entry.* |= accessed_dirty;
        // The directory now references WB page-table storage. PWT/PCD/PAT
        // from the old data leaf must not become attributes of that storage.
        const directory_flags = self.previous & (nx | present | (1 << 1) | user | (1 << 5));
        @atomicStore(u64, self.directory, self.table_paddr | directory_flags, .release);
    }
};

test "identity split preserves every PAT slot RW NX and software bits" {
    var table: [512]u64 align(4096) = undefined;
    const base: u64 = 0x8000_0000;
    for (0..8) |pat| {
        for ([_]bool{ false, true }) |writable| {
            const cache = (@as(u64, pat & 3) << 3) | (@as(u64, pat >> 2) << 12);
            const original = base | present | large | nx | (1 << 10) | cache |
                (if (writable) @as(u64, 2) else 0);
            var directory = original;
            const plan = try IdentitySplit.prepare(&directory, base, &table, 0x2000);
            try std.testing.expectEqual(original, directory);
            const expected_cache = (@as(u64, pat & 3) << 3) | (@as(u64, pat >> 2) << 7);
            for (table, 0..) |entry, i| {
                try std.testing.expectEqual(base + i * 4096, entry & address_mask);
                try std.testing.expectEqual(expected_cache, entry & 0x98);
                try std.testing.expectEqual(original & (nx | 2 | (1 << 10)), entry & (nx | 2 | (1 << 10)));
            }
            directory |= accessed_dirty;
            plan.publishEquivalent();
            try std.testing.expectEqual(@as(u64, 0x2000), directory & address_mask);
            try std.testing.expectEqual(@as(u64, 0), directory & (large | (1 << 3) | (1 << 4)));
            for (table) |entry| try std.testing.expectEqual(accessed_dirty, entry & accessed_dirty);
        }
    }
}

test "identity split invalid inputs never mutate live directory or scratch" {
    var table: [512]u64 align(4096) = .{0xdead} ** 512;
    const base: u64 = 0x8000_0000;
    var directory: u64 = base | 0x83;
    for ([_]u64{ 0, 0x1001, @as(u64, 1) << 60 }) |invalid| {
        try std.testing.expectError(error.Invalid, IdentitySplit.prepare(&directory, base, &table, invalid));
    }
    for ([_]u64{ base | 3, base | 0x87, base | 0x183, base | 0x83 | (@as(u64, 1) << 62) }) |invalid| {
        directory = invalid;
        try std.testing.expectError(error.Unsupported, IdentitySplit.prepare(&directory, base, &table, 0x2000));
        try std.testing.expectEqual(invalid, directory);
    }
    directory = base | 0x83;
    try std.testing.expectError(error.Invalid, IdentitySplit.prepare(&directory, base + 4096, &table, 0x2000));
    try std.testing.expectError(error.Invalid, IdentitySplit.prepare(&directory, base + LeafSize.large.bytes(), &table, 0x2000));
    try std.testing.expectError(error.Invalid, IdentitySplit.prepare(&table[0], base, &table, 0x2000));
    for (table) |entry| try std.testing.expectEqual(@as(u64, 0xdead), entry);
    try std.testing.expectEqual(base | 0x83, directory);
}
