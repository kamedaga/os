//! Sparse reservation bookkeeping. Banks describe address space, not physical
//! backing; absent banks are entirely free. Caller serializes all operations.
const std = @import("std");
pub const page_size: u64 = 4096;
// Start above legacy low-memory DMA/firmware space. UEFI can assign the first
// PCI MMIO BAR at 0x8000_0000; starting there leaves no aperture even though
// translated IOVAs below that BAR are free. PCI and ACPI still clip the end.
pub const window_start: u64 = 0x0100_0000;
pub const window_ceiling: u64 = 0xfee0_0000; // Below the x86 MSI window and 4 GiB.
pub const bank_pages: usize = 16 * 1024;
pub const Bank = struct {
    next: ?*Bank = null,
    start: usize = 0,
    used: usize = 0,
    bits: [bank_pages / 64]u64 = @splat(0),

    fn occupied(self: *const Bank, index: usize) bool {
        return self.bits[index / 64] & (@as(u64, 1) << @intCast(index % 64)) != 0;
    }

    fn set(self: *Bank, index: usize, value: bool) void {
        const mask = @as(u64, 1) << @intCast(index % 64);
        if (value) self.bits[index / 64] |= mask else self.bits[index / 64] &= ~mask;
    }
};
comptime {
    if (@sizeOf(Bank) > page_size) @compileError("IOVA bank must fit in one PMM page");
}

pub const Storage = struct {
    context: ?*anyopaque = null,
    allocate: *const fn (?*anyopaque) ?*Bank,
    release: *const fn (?*anyopaque, *Bank) void,
};

pub const Allocator = struct {
    banks: ?*Bank = null,
    storage: ?Storage = null,
    start: u64 = 0,
    page_count: usize = 0,
    cursor: usize = 0,
    used_pages: usize = 0,
    peak_pages: usize = 0,

    pub fn init(start: u64, end: u64, storage: Storage) Allocator {
        std.debug.assert(start <= end and (start | end) % page_size == 0);
        return .{ .start = start, .page_count = @intCast((end - start) / page_size), .storage = storage };
    }

    fn index(self: *const Allocator, address: u64, count: usize) ?usize {
        if (count == 0 or address < self.start or address % page_size != 0) return null;
        const first: usize = @intCast((address - self.start) / page_size);
        if (first >= self.page_count or count > self.page_count - first) return null;
        return first;
    }

    // Visit only populated banks. A hole of any size costs one list step;
    // runs may span a bank suffix, absent banks and the next bank's prefix.
    fn firstFit(self: *const Allocator, first: usize, limit: usize, count: usize) ?usize {
        var candidate = first;
        var current = self.banks;
        while (current) |bank| : (current = bank.next) {
            if (bank.start + bank_pages <= candidate) continue;
            if (candidate >= limit or count > self.page_count - candidate) return null;
            if (bank.start >= candidate + count) return candidate;
            var bit = @max(candidate, bank.start) - bank.start;
            while (bit < bank_pages and bank.start + bit < candidate + count) : (bit += 1) {
                if (bank.occupied(bit)) {
                    candidate = bank.start + bit + 1;
                    if (candidate >= limit or count > self.page_count - candidate) return null;
                }
            }
        }
        return if (candidate < limit and count <= self.page_count - candidate) candidate else null;
    }

    fn allMatch(self: *const Allocator, first: usize, count: usize, occupied: bool) bool {
        var position = first;
        const end = first + count;
        var current = self.banks;
        while (current) |bank| : (current = bank.next) {
            if (bank.start + bank_pages <= position) continue;
            if (position >= end) break;
            if (bank.start > position) {
                if (occupied) return false;
                position = @min(bank.start, end);
            }
            const stop = @min(end, bank.start + bank_pages);
            while (position < stop) : (position += 1) {
                if (bank.occupied(position - bank.start) != occupied) return false;
            }
        }
        return !occupied or position == end;
    }

    fn discardEmpty(self: *Allocator) void {
        const storage = self.storage orelse return;
        var link = &self.banks;
        while (link.*) |bank| {
            if (bank.used == 0) {
                link.* = bank.next;
                storage.release(storage.context, bank);
            } else link = &bank.next;
        }
    }

    fn changeBits(self: *Allocator, first: usize, count: usize, value: bool) void {
        const end = first + count;
        var current = self.banks;
        while (current) |bank| : (current = bank.next) {
            if (bank.start >= end) break;
            if (bank.start + bank_pages <= first) continue;
            const lo = @max(first, bank.start);
            const hi = @min(end, bank.start + bank_pages);
            for (lo..hi) |position| bank.set(position - bank.start, value);
            if (value) bank.used += hi - lo else bank.used -= hi - lo;
        }
    }

    /// Metadata allocation is transactional: no live bits or counters change
    /// until every missing bank exists. Empty banks exist only during reserve.
    pub fn reserve(self: *Allocator, address: u64, count: usize) bool {
        const first = self.index(address, count) orelse return false;
        const storage = self.storage orelse return false;
        if (!self.allMatch(first, count, false)) return false;
        var start = first / bank_pages * bank_pages;
        var link = &self.banks;
        while (start < first + count) : (start += bank_pages) {
            while (link.*) |bank| {
                if (bank.start >= start) break;
                link = &bank.next;
            }
            if (link.* == null or link.*.?.start != start) {
                const bank = storage.allocate(storage.context) orelse {
                    self.discardEmpty();
                    return false;
                };
                bank.* = .{ .start = start, .next = link.* };
                link.* = bank;
            }
            link = &link.*.?.next;
        }
        self.changeBits(first, count, true);
        self.used_pages += count;
        self.peak_pages = @max(self.peak_pages, self.used_pages);
        return true;
    }

    pub fn alloc(self: *Allocator, count: usize) ?u64 {
        if (count == 0 or count > self.page_count) return null;
        const first = self.firstFit(self.cursor, self.page_count, count) orelse
            self.firstFit(0, self.cursor, count) orelse return null;
        const address = self.start + @as(u64, @intCast(first)) * page_size;
        if (!self.reserve(address, count)) return null;
        self.cursor = (first + count) % self.page_count;
        return address;
    }

    /// Partial and cross-reservation frees are valid. Free never allocates and
    /// an invalid/double free leaves the entire range unchanged.
    pub fn free(self: *Allocator, address: u64, count: usize) bool {
        const first = self.index(address, count) orelse return false;
        if (!self.allMatch(first, count, true)) return false;
        self.changeBits(first, count, false);
        self.used_pages -= count;
        self.discardEmpty();
        return true;
    }
};

/// End-exclusive continuous aperture clipping; never advertise reserved holes.
pub fn exclude(start: u64, end: u64, reserved_start: u64, reserved_last: u64) u64 {
    if (reserved_start > reserved_last or reserved_last < start or reserved_start >= end) return end;
    return @max(start, reserved_start & ~(page_size - 1));
}

pub const TestStorage = if (@import("builtin").is_test) struct {
    live: usize = 0,
    allowance: usize = std.math.maxInt(usize),

    fn allocate(context: ?*anyopaque) ?*Bank {
        const self: *@This() = @ptrCast(@alignCast(context.?));
        if (self.allowance == 0) return null;
        const bank = std.testing.allocator.create(Bank) catch return null;
        self.allowance -= 1;
        self.live += 1;
        return bank;
    }

    fn release(context: ?*anyopaque, bank: *Bank) void {
        const self: *@This() = @ptrCast(@alignCast(context.?));
        std.testing.allocator.destroy(bank);
        self.live -= 1;
    }

    pub fn storage(self: *@This()) Storage {
        return .{ .context = self, .allocate = allocate, .release = release };
    }

    pub fn cleanup(self: *@This(), allocator: *Allocator) void {
        while (allocator.banks) |bank| {
            allocator.banks = bank.next;
            release(self, bank);
        }
    }
} else void;

test "VT-d sparse IOVA bank boundary, cross free and allocation rollback" {
    var backing = TestStorage{};
    var a = Allocator.init(0x80000000, 0x180000000, backing.storage());
    defer backing.cleanup(&a);
    const boundary = a.start + bank_pages * page_size;
    try std.testing.expect(a.reserve(boundary - page_size, 1));
    const first_bank = a.banks.?;
    backing.allowance = 1;
    try std.testing.expect(!a.reserve(boundary, bank_pages + 1));
    try std.testing.expectEqual(@as(usize, 1), backing.live);
    try std.testing.expectEqual(@as(usize, 1), a.used_pages);
    try std.testing.expectEqual(@as(usize, 1), a.peak_pages);
    try std.testing.expectEqual(first_bank, a.banks.?);
    try std.testing.expectEqual(@as(usize, 0), a.cursor);
    backing.allowance = 2;
    try std.testing.expect(a.reserve(boundary, bank_pages + 1));
    backing.allowance = 0; // Free must never request backing.
    try std.testing.expect(a.free(boundary - page_size, bank_pages + 2));
    try std.testing.expectEqual(@as(usize, 0), backing.live);
    try std.testing.expectEqual(@as(usize, 0), a.used_pages);
    try std.testing.expect(!a.free(boundary, 1));
    backing.allowance = 2;
    try std.testing.expect(a.reserve(boundary - page_size, 2));
    try std.testing.expect(!a.free(boundary - 2 * page_size, 3));
    try std.testing.expectEqual(@as(usize, 2), a.used_pages);
    try std.testing.expect(a.free(boundary, 1));
    try std.testing.expectEqual(@as(usize, 1), backing.live);
}

test "VT-d sparse IOVA fits across bank suffix hole and next bank prefix" {
    var backing = TestStorage{};
    var a = Allocator.init(0x80000000, 1 << 48, backing.storage());
    defer backing.cleanup(&a);
    try std.testing.expect(a.reserve(a.start + 100 * page_size, 1));
    try std.testing.expect(a.reserve(a.start + (2 * bank_pages + 100) * page_size, 1));
    a.cursor = 101;
    try std.testing.expectEqual(a.start + 101 * page_size, a.alloc(2 * bank_pages - 1).?);
    const distant = (1 << 48) - page_size;
    try std.testing.expect(a.reserve(distant, 1));
    // Wide absent bank gaps are not materialized or page-scanned.
    a.cursor = 1 << 32;
    try std.testing.expectEqual(a.start + (@as(u64, 1) << 32) * page_size, a.alloc(1).?);
    try std.testing.expectEqual(@as(usize, 5), backing.live);
}

test "VT-d IOVA aperture clips reservations without advertising holes" {
    const start = 0x80000000;
    const end = 0xfee00000;
    try std.testing.expectEqual(@as(u64, 0xa0000000), exclude(start, end, 0xa0000100, 0xa0001fff));
    try std.testing.expectEqual(@as(u64, start), exclude(start, end, start - 1, start));
    try std.testing.expectEqual(@as(u64, end), exclude(start, end, 0x380000000000, 0x380000003fff));
    try std.testing.expectEqual(@as(u64, end), exclude(start, end, 0x1000, 0x1fff));
}
