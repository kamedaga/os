const std = @import("std");
const storage = @import("owned_storage.zig");

pub const slots_per_bank = 64;
pub const no_index: u16 = 0xffff;
pub const Page = [4096]u8;
pub const PagePointer = *align(4096) Page;
pub const PtPage = [512]u64;
pub const PtPointer = *align(4096) PtPage;

/// Callbacks cannot reenter the store. Context must outlive every outstanding
/// bank and PT grant, including failed returns. Private stores require their
/// AS lock. The shared kernel RAM-split store instead requires the outer
/// KernelState/all-AS/shared-VM transaction through prepare/publish/release:
/// locking just one process AS cannot exclude another process's split plan.
pub const Allocator = storage.Allocator;
pub const PhysicalExtent = storage.PhysicalExtent;
pub const MappedExtent = storage.MappedExtent;

pub const Descriptor = struct {
    page: ?PtPointer = null,
    allocation: PhysicalExtent = undefined, // Valid exactly while page != null.
    // Owned detached page: reusable only after the enclosing AS shootdown ACK.
    retire_pending: bool = false,
    pml4: u16 = no_index,
    pdp: u16 = no_index,
    pd: u16 = no_index,
};

const Bank = struct {
    allocator: Allocator,
    allocation: PhysicalExtent,
    descriptors: [slots_per_bank]Descriptor = [_]Descriptor{.{}} ** slots_per_bank,
};

// A directory page covers a fixed packing unit, not a capacity limit. Missing
// ranges and banks are allowed; published descriptors never move on growth.
const banks_per_chunk = 256;
const DirectoryChunk = struct {
    next: ?*align(4096) DirectoryChunk = null,
    base_bank: usize,
    allocator: Allocator,
    allocation: PhysicalExtent,
    banks: [banks_per_chunk]?*align(4096) Bank = .{null} ** banks_per_chunk,
};

comptime {
    std.debug.assert(@sizeOf(Bank) <= 4096);
    std.debug.assert(@sizeOf(DirectoryChunk) <= 4096);
    std.debug.assert(@sizeOf(PtPage) == 4096);
}

/// Storage only: callers exclude software borrowers and detach hardware
/// references BEFORE calling retirement. Private stores use A; shared stores
/// use K/all-A/S. There is no additional lock and callbacks cannot reenter.
pub const Store = struct {
    chunks: ?*align(4096) DirectoryChunk = null,
    slot_limit: usize = 0,

    pub fn capacity(self: *const Store) usize {
        return self.slot_limit;
    }

    fn bankAt(self: *const Store, index: usize) ?*align(4096) Bank {
        var current = self.chunks;
        while (current) |chunk| : (current = chunk.next) {
            if (index >= chunk.base_bank and index - chunk.base_bank < banks_per_chunk)
                return chunk.banks[index - chunk.base_bank];
        }
        return null;
    }

    pub fn descriptor(self: *Store, slot: usize) ?*Descriptor {
        const bank = self.bankAt(slot / slots_per_bank) orelse return null;
        return &bank.descriptors[slot % slots_per_bank];
    }

    pub fn descriptorConst(self: *const Store, slot: usize) ?*const Descriptor {
        const bank = self.bankAt(slot / slots_per_bank) orelse return null;
        return &bank.descriptors[slot % slots_per_bank];
    }

    // Iterates existing descriptors, skipping holes without scanning missing
    // address ranges. Valid while the enclosing AS transaction excludes frees.
    pub const Iterator = struct {
        chunk: ?*align(4096) DirectoryChunk,
        bank: usize = 0,
        entry: usize = 0,
        current: ?*const Descriptor = null,
        pub fn next(self: *Iterator) ?usize {
            while (self.chunk) |chunk| {
                while (self.bank < banks_per_chunk) {
                    if (chunk.banks[self.bank]) |bank| {
                        self.current = &bank.descriptors[self.entry];
                        const slot = (chunk.base_bank + self.bank) * slots_per_bank + self.entry;
                        self.entry += 1;
                        if (self.entry == slots_per_bank) {
                            self.entry = 0;
                            self.bank += 1;
                        }
                        return slot;
                    }
                    self.bank += 1;
                }
                self.chunk = chunk.next;
                self.bank = 0;
            }
            return null;
        }
    };
    pub fn iterator(self: *const Store) Iterator {
        return .{ .chunk = self.chunks };
    }

    /// Software coordinates use pml4 as the assignment marker at every level.
    /// Null pages and ACK-completed retained pages are equally reusable.
    pub fn firstFreeSlot(self: *const Store) ?usize {
        var candidate: usize = 0;
        var current = self.chunks;
        while (current) |chunk| : (current = chunk.next) {
            if (chunk.base_bank * slots_per_bank > candidate) return candidate;
            for (chunk.banks, 0..) |maybe_bank, bi| {
                const bank_index = chunk.base_bank + bi;
                const base = std.math.mul(usize, bank_index, slots_per_bank) catch return null;
                const bank = maybe_bank orelse return base;
                for (bank.descriptors, 0..) |entry, ei| {
                    if (entry.pml4 == no_index) return base + ei;
                }
                candidate = std.math.add(usize, base, slots_per_bank) catch return null;
            }
        }
        return candidate;
    }

    /// Caller has detached and acknowledged every hardware reference.
    pub fn resetDetachedContents(self: *Store) void {
        var it = self.iterator();
        while (it.next()) |slot| {
            const entry = self.descriptor(slot).?;
            entry.retire_pending = false;
            entry.pml4 = no_index;
            entry.pdp = no_index;
            entry.pd = no_index;
            if (entry.page) |page| @memset(page, 0);
        }
    }

    pub fn releasePageDetached(self: *Store, slot: usize) void {
        const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
        defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.retire, stage_sample);
        const entry = self.descriptor(slot) orelse return;
        const allocator = self.bankAt(slot / slots_per_bank).?.allocator;
        entry.retire_pending = false;
        entry.pml4 = no_index;
        entry.pdp = no_index;
        entry.pd = no_index;
        if (entry.page) |page| {
            @memset(page, 0);
            if (allocator.release(allocator.context, entry.allocation)) entry.page = null;
        }
    }

    /// Sparse growth never relocates a bank. Check all representational
    /// arithmetic before acquisition, including the rounded descriptor limit.
    /// Failed acquisitions leave only owned, unpublished metadata; reclaim it
    /// immediately where possible, retaining rejected original grants for retry.
    pub fn ensurePage(self: *Store, slot: usize, allocator: Allocator) ?PtPointer {
        return self.ensurePageChecked(slot, allocator) catch null;
    }

    // Transaction callers must preserve OOM separately from invalid coordinates;
    // the optional wrapper remains for callers with only success/failure results.
    pub fn ensurePageChecked(self: *Store, slot: usize, allocator: Allocator) error{ OutOfFreePages, InvalidState }!PtPointer {
        const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
        defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.pt_prepare, stage_sample);
        const bank_index = slot / slots_per_bank;
        const end_bank = std.math.add(usize, bank_index, 1) catch return error.InvalidState;
        const limit = std.math.mul(usize, end_bank, slots_per_bank) catch return error.InvalidState;
        const base_bank = bank_index - bank_index % banks_per_chunk;
        // Keep iterator arithmetic representable for the entire chunk.
        const end_chunk = std.math.add(usize, base_bank, banks_per_chunk) catch return error.InvalidState;
        _ = std.math.mul(usize, end_chunk, slots_per_bank) catch return error.InvalidState;
        var link = &self.chunks;
        while (link.*) |chunk| {
            if (chunk.base_bank >= base_bank) break;
            link = &chunk.next;
        }
        if (link.* == null or link.*.?.base_bank != base_bank) {
            const raw = allocator.acquire(allocator.context, 1) orelse return error.OutOfFreePages;
            std.debug.assert(raw.allocation.pages == 1 and raw.bytes.len == 4096);
            const chunk: *align(4096) DirectoryChunk = @ptrCast(raw.bytes.ptr);
            chunk.* = .{ .next = link.*, .base_bank = base_bank, .allocator = allocator, .allocation = raw.allocation };
            link.* = chunk;
        }
        const chunk = link.*.?;
        const bank_entry = &chunk.banks[bank_index - base_bank];
        if (bank_entry.* == null) {
            const raw = allocator.acquire(allocator.context, 1) orelse {
                self.reclaimEmptyBanks();
                return error.OutOfFreePages;
            };
            std.debug.assert(raw.allocation.pages == 1 and raw.bytes.len == 4096);
            const bank: *align(4096) Bank = @ptrCast(raw.bytes.ptr);
            bank.* = .{ .allocator = allocator, .allocation = raw.allocation };
            bank_entry.* = bank;
            self.slot_limit = @max(self.slot_limit, limit);
        }
        const bank = bank_entry.*.?;
        const entry = &bank.descriptors[slot % slots_per_bank];
        if (entry.page) |page| return page;
        const issuer = bank.allocator;
        const raw = issuer.acquire(issuer.context, 1) orelse {
            self.reclaimEmptyBanks();
            return error.OutOfFreePages;
        };
        std.debug.assert(raw.allocation.pages == 1 and raw.bytes.len == 4096);
        @memset(raw.bytes, 0);
        const page: PtPointer = @ptrCast(raw.bytes.ptr);
        entry.page = page;
        entry.allocation = raw.allocation;
        return page;
    }

    /// No descriptor borrow may cross this boundary. Only banks with no owned
    /// pages can leave; failed returns retain the bank/chunk and original issuer.
    pub fn reclaimEmptyBanks(self: *Store) void {
        var link = &self.chunks;
        var limit: usize = 0;
        while (link.*) |chunk| {
            var empty_chunk = true;
            for (&chunk.banks, 0..) |*bank_entry, bi| {
                if (bank_entry.*) |bank| {
                    var empty = true;
                    for (bank.descriptors) |entry| {
                        if (entry.page != null or entry.pml4 != no_index) {
                            empty = false;
                            break;
                        }
                    }
                    const allocator = bank.allocator;
                    const bank_grant = bank.allocation;
                    if (empty and allocator.release(allocator.context, bank_grant)) {
                        bank_entry.* = null;
                    } else {
                        empty_chunk = false;
                        limit = @max(limit, (chunk.base_bank + bi + 1) * slots_per_bank);
                    }
                }
            }
            const allocator = chunk.allocator;
            const grant = chunk.allocation;
            const next = chunk.next;
            if (empty_chunk and allocator.release(allocator.context, grant)) {
                link.* = next;
            } else link = &chunk.next;
        }
        self.slot_limit = limit;
    }

    /// PRECONDITION: every parent reference is detached and all required CPU
    /// shootdowns have completed. Return failure retains ownership for retry.
    pub fn freeDetached(self: *Store) void {
        var it = self.iterator();
        while (it.next()) |slot| self.releasePageDetached(slot);
        self.reclaimEmptyBanks();
    }
};

const TestAllocator = struct {
    region_id: u64 = 73,
    // Deliberately not an identity mapping: reconstructing a physical grant
    // from the host pointer must fail even if the issuer/region are correct.
    physical_bias: u64 = 0x8000000000000,
    active: [16]?PagePointer = .{null} ** 16,
    allocate_calls: usize = 0,
    release_calls: usize = 0,
    fail_allocate_at: usize = 0,
    fail_release: ?PagePointer = null,
    fail_all_releases: bool = false,

    fn callbacks(self: *TestAllocator) Allocator {
        return .{ .context = self, .acquire = allocate, .release = release };
    }

    fn allocate(context: *anyopaque, pages: usize) ?MappedExtent {
        const self: *TestAllocator = @ptrCast(@alignCast(context));
        std.debug.assert(pages == 1);
        self.allocate_calls += 1;
        if (self.allocate_calls == self.fail_allocate_at) return null;
        const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096) catch return null;
        const page: PagePointer = @ptrCast(bytes.ptr);
        @memset(page, 0xa5);
        for (&self.active) |*entry| {
            if (entry.* != null) continue;
            entry.* = page;
            return .{ .allocation = .{ .physical = .fromInt(@intFromPtr(page) ^ self.physical_bias), .pages = 1, .region_id = self.region_id }, .bytes = page };
        }
        unreachable;
    }

    fn release(context: *anyopaque, grant: PhysicalExtent) bool {
        const self: *TestAllocator = @ptrCast(@alignCast(context));
        std.debug.assert(grant.pages == 1 and grant.region_id == self.region_id);
        self.release_calls += 1;
        for (&self.active) |*entry| {
            const page = entry.* orelse continue;
            if ((@intFromPtr(page) ^ self.physical_bias) != grant.physical.toInt()) continue;
            if (self.fail_all_releases or self.fail_release == page) return false;
            entry.* = null;
            @memset(page, 0xdd);
            const bytes: []align(4096) u8 = page;
            std.testing.allocator.free(bytes);
            return true;
        }
        // A second release or a foreign pointer is an ownership violation.
        unreachable;
    }

    fn cleanup(self: *TestAllocator, store: *Store) void {
        self.fail_all_releases = false;
        self.fail_release = null;
        store.freeDetached();
        for (self.active) |entry| std.debug.assert(entry == null);
    }
};

test "MMIO overlay overflow storage boundaries preserve addresses and content" {
    var fixture = TestAllocator{};
    var store = Store{};
    defer fixture.cleanup(&store);
    const callbacks = fixture.callbacks();
    const read_only: *const Store = &store;
    try std.testing.expect(store.ensurePage(std.math.maxInt(usize), callbacks) == null);
    try std.testing.expectEqual(@as(usize, 0), fixture.allocate_calls);
    try std.testing.expect(read_only.descriptorConst(0) == null);
    const first = store.ensurePage(0, callbacks).?;
    try std.testing.expect(std.mem.allEqual(u64, first, 0));
    try std.testing.expectEqual(no_index, read_only.descriptorConst(0).?.pd);
    try std.testing.expect(read_only.descriptorConst(1).?.page == null);
    first[511] = 0x1234;
    store.descriptor(0).?.pd = 7;
    const before = fixture.allocate_calls;
    try std.testing.expect(store.ensurePage(0, callbacks).? == first);
    try std.testing.expectEqual(before, fixture.allocate_calls);
    _ = store.ensurePage(63, callbacks).?;
    const next = store.ensurePage(64, callbacks).?;
    try std.testing.expect(first != next);
    try std.testing.expectEqual(@as(usize, 128), store.capacity());
    try std.testing.expectEqual(@as(u64, 0x1234), first[511]);
    try std.testing.expectEqual(@as(u16, 7), read_only.descriptorConst(0).?.pd);
    for (0..store.capacity()) |slot| {
        const entry = read_only.descriptorConst(slot).?;
        try std.testing.expectEqual(no_index, entry.pml4);
        try std.testing.expectEqual(no_index, entry.pdp);
        if (slot != 0) try std.testing.expectEqual(no_index, entry.pd);
    }
    const allocated = fixture.allocate_calls;
    try std.testing.expect(store.ensurePage(std.math.maxInt(usize), callbacks) == null);
    try std.testing.expectEqual(allocated, fixture.allocate_calls);
}

test "MMIO overlay overflow storage allocation failures retain ownership" {
    // Chunk, bank, body: fail every acquisition, also fail each rollback.
    for (1..4) |failure| {
        for ([_]bool{ false, true }) |reject| {
            var fixture = TestAllocator{};
            var store = Store{};
            defer fixture.cleanup(&store);
            fixture.fail_allocate_at = failure;
            fixture.fail_all_releases = reject;
            try std.testing.expect(store.ensurePage(0, fixture.callbacks()) == null);
            fixture.fail_all_releases = false;
            fixture.fail_allocate_at = 0;
            const first = store.ensurePage(0, fixture.callbacks()).?;
            first[0] = 23;
            fixture.fail_allocate_at = fixture.allocate_calls + 1;
            try std.testing.expect(store.ensurePage(1, fixture.callbacks()) == null);
            try std.testing.expectEqual(@as(u64, 23), first[0]);
            try std.testing.expect(store.descriptor(1).?.page == null);
        }
    }
}

test "MMIO overlay overflow storage failed PT return is reusable without double release" {
    var fixture = TestAllocator{};
    var store = Store{};
    defer fixture.cleanup(&store);
    const callbacks = fixture.callbacks();
    const retained = store.ensurePage(0, callbacks).?;
    _ = store.ensurePage(63, callbacks).?;
    _ = store.ensurePage(64, callbacks).?;
    retained[0] = 42;
    store.descriptor(0).?.pml4 = 1;
    store.descriptor(0).?.pdp = 2;
    store.descriptor(0).?.pd = 3;
    fixture.fail_release = @ptrCast(retained);
    store.freeDetached();
    try std.testing.expectEqual(@as(usize, 64), store.capacity());
    try std.testing.expect(store.descriptor(0).?.page.? == retained);
    try std.testing.expect(std.mem.allEqual(u64, retained, 0));
    try std.testing.expectEqual(no_index, store.descriptor(0).?.pml4);
    try std.testing.expectEqual(no_index, store.descriptor(0).?.pdp);
    try std.testing.expectEqual(no_index, store.descriptor(0).?.pd);
    const allocated = fixture.allocate_calls;
    try std.testing.expect(store.ensurePage(0, callbacks).? == retained);
    try std.testing.expectEqual(allocated, fixture.allocate_calls);
    retained[0] = 99;
    fixture.fail_release = null;
    const released = fixture.release_calls;
    store.freeDetached();
    try std.testing.expectEqual(released + 3, fixture.release_calls); // PT, bank, directory chunk.
    try std.testing.expectEqual(@as(usize, 0), store.capacity());
    store.freeDetached();
    try std.testing.expectEqual(released + 3, fixture.release_calls);
}

test "MMIO overlay overflow storage bank failure retains only its sparse owner" {
    var fixture = TestAllocator{};
    var store = Store{};
    defer fixture.cleanup(&store);
    const callbacks = fixture.callbacks();
    _ = store.ensurePage(0, callbacks).?;
    _ = store.ensurePage(64, callbacks).?;
    fixture.fail_release = @ptrCast(store.bankAt(1).?);
    store.freeDetached();
    try std.testing.expectEqual(@as(usize, 128), store.capacity());
    try std.testing.expect(store.descriptor(0) == null);
    var it = store.iterator();
    while (it.next()) |slot| try std.testing.expect(store.descriptor(slot).?.page == null);
    const released = fixture.release_calls;
    fixture.fail_release = null;
    store.freeDetached();
    try std.testing.expectEqual(released + 2, fixture.release_calls); // Retained bank and directory; PTs already returned.
    try std.testing.expectEqual(@as(usize, 0), store.capacity());
}

test "MMIO overlay PT grants stay with each bank issuer across replacement and OOM" {
    var first = TestAllocator{ .region_id = 117 };
    var second = TestAllocator{ .region_id = 229 };
    var store = Store{};
    defer {
        first.fail_all_releases = false;
        second.fail_all_releases = false;
        store.freeDetached();
        for (first.active) |entry| std.debug.assert(entry == null);
        for (second.active) |entry| std.debug.assert(entry == null);
    }
    const old = store.ensurePage(0, first.callbacks()).?;
    old[511] = 0xabcdef;
    first.fail_allocate_at = first.allocate_calls + 1;
    // B has available memory, but an A bank cannot silently change issuer.
    try std.testing.expect(store.ensurePage(1, second.callbacks()) == null);
    try std.testing.expectEqual(@as(usize, 0), second.allocate_calls);
    try std.testing.expectEqual(@as(u64, 0xabcdef), old[511]);
    first.fail_allocate_at = 0;
    _ = store.ensurePage(1, second.callbacks()).?;
    try std.testing.expectEqual(@as(usize, 0), second.allocate_calls);
    _ = store.ensurePage(64, second.callbacks()).?;
    try std.testing.expectEqual(@as(usize, 2), second.allocate_calls);
    try std.testing.expectEqual(@as(u64, 117), store.descriptor(1).?.allocation.region_id);
    try std.testing.expectEqual(@as(u64, 229), store.descriptor(64).?.allocation.region_id);
    first.fail_all_releases = true;
    second.fail_all_releases = true;
    const a_before = first.release_calls;
    const b_before = second.release_calls;
    store.freeDetached();
    // Each PT is attempted once. Banks with live PT grants cannot be returned.
    try std.testing.expectEqual(a_before + 2, first.release_calls);
    try std.testing.expectEqual(b_before + 1, second.release_calls);
    try std.testing.expectEqual(@as(usize, 128), store.capacity());
    first.fail_all_releases = false;
    second.fail_all_releases = false;
    store.freeDetached();
    try std.testing.expectEqual(@as(usize, 0), store.capacity());
    const a_done = first.release_calls;
    const b_done = second.release_calls;
    store.freeDetached();
    try std.testing.expectEqual(a_done, first.release_calls);
    try std.testing.expectEqual(b_done, second.release_calls);
}

test "MMIO overlay PT detached page retry preserves issuer and full PTE payload" {
    var first = TestAllocator{ .region_id = 13 };
    var second = TestAllocator{ .region_id = 29 };
    var store = Store{};
    defer first.cleanup(&store);
    const page = store.ensurePage(0, first.callbacks()).?;
    @memset(page, 0xffffffffffffffff);
    first.fail_release = @ptrCast(page);
    store.releasePageDetached(0);
    try std.testing.expect(store.descriptor(0).?.page.? == page);
    try std.testing.expect(std.mem.allEqual(u64, page, 0));
    try std.testing.expect(store.ensurePage(0, second.callbacks()).? == page);
    try std.testing.expectEqual(@as(usize, 0), second.allocate_calls);
    first.fail_release = null;
    store.releasePageDetached(0);
    const released = first.release_calls;
    store.releasePageDetached(0);
    try std.testing.expectEqual(released, first.release_calls);
    try std.testing.expect(store.descriptor(0).?.page == null);
}

test "MMIO overlay G3 sparse software slots cross u16 and reject overflow" {
    var fixture = TestAllocator{};
    var store = Store{};
    defer fixture.cleanup(&store);
    const low = store.ensurePage(0, fixture.callbacks()).?;
    store.descriptor(0).?.pml4 = 1;
    low[0] = 123;
    for ([_]usize{ 65535, 65536 }) |slot| {
        const page = store.ensurePage(slot, fixture.callbacks()).?;
        page[511] = slot;
        store.descriptor(slot).?.pml4 = 2;
    }
    try std.testing.expectEqual(@as(u64, 65535), store.descriptorConst(65535).?.page.?[511]);
    try std.testing.expectEqual(@as(u64, 65536), store.descriptorConst(65536).?.page.?[511]);
    const acquired = fixture.allocate_calls;
    for ([_]usize{ std.math.maxInt(usize), std.math.maxInt(usize) - 63 }) |slot|
        try std.testing.expect(store.ensurePage(slot, fixture.callbacks()) == null);
    try std.testing.expectEqual(acquired, fixture.allocate_calls);
    store.releasePageDetached(65535);
    store.reclaimEmptyBanks();
    try std.testing.expect(store.descriptorConst(65535) == null);
    try std.testing.expect(store.descriptorConst(65536) != null);
    try std.testing.expectEqual(low, store.descriptorConst(0).?.page.?);
    try std.testing.expectEqual(@as(u64, 123), low[0]);
}

test "MMIO overlay G3 sparse directory growth OOM preserves live lower bank" {
    for (1..4) |failure| {
        for ([_]bool{ false, true }) |reject| {
            var fixture = TestAllocator{};
            var store = Store{};
            defer fixture.cleanup(&store);
            const first = store.ensurePage(0, fixture.callbacks()).?;
            first[123] = 0xabcdef;
            store.descriptor(0).?.pml4 = 1;
            fixture.fail_allocate_at = fixture.allocate_calls + failure;
            fixture.fail_all_releases = reject;
            try std.testing.expect(store.ensurePage(banks_per_chunk * slots_per_bank, fixture.callbacks()) == null);
            try std.testing.expectEqual(first, store.descriptorConst(0).?.page.?);
            try std.testing.expectEqual(@as(u64, 0xabcdef), first[123]);
            fixture.fail_all_releases = false;
            store.reclaimEmptyBanks();
            try std.testing.expectEqual(@as(usize, slots_per_bank), store.capacity());
        }
    }
}

test "MMIO overlay G3 rejected directory return keeps owner with zero capacity" {
    var fixture = TestAllocator{};
    var store = Store{};
    defer fixture.cleanup(&store);
    _ = store.ensurePage(0, fixture.callbacks()).?;
    fixture.fail_release = @ptrCast(store.chunks.?);
    store.freeDetached();
    try std.testing.expectEqual(@as(usize, 0), store.capacity());
    try std.testing.expect(store.chunks != null);
    fixture.fail_release = null;
    store.freeDetached();
    try std.testing.expect(store.chunks == null);
    const returns = fixture.release_calls;
    store.freeDetached();
    try std.testing.expectEqual(returns, fixture.release_calls);
}
