const std = @import("std");

pub const slots_per_bank = 64;
pub const max_banks = 512;
pub const max_slots = slots_per_bank * max_banks;
pub const no_index: u16 = 0xffff;
pub const Page = [4096]u8;
pub const PagePointer = *align(4096) Page;
pub const PtPage = [512]u64;
pub const PtPointer = *align(4096) PtPage;

/// Both callbacks use one ownership domain. Allocation returns a unique,
/// accessible page. Release transfers ownership only on true; false leaves
/// the page accessible and owned by the caller. Neither callback may reenter
/// this store. The caller serializes all operations with its AS lock.
pub const Allocator = struct {
    context: *anyopaque,
    allocate: *const fn (*anyopaque) ?PagePointer,
    release: *const fn (*anyopaque, PagePointer) bool,
};

pub const Descriptor = struct {
    page: ?PtPointer = null,
    pml4: u16 = no_index,
    pdp: u16 = no_index,
    pd: u16 = no_index,
};

const Bank = struct {
    descriptors: [slots_per_bank]Descriptor = [_]Descriptor{.{}} ** slots_per_bank,
};

comptime {
    std.debug.assert(@sizeOf(Bank) <= @sizeOf(Page));
    std.debug.assert(@sizeOf(PtPage) == @sizeOf(Page));
    std.debug.assert(@typeInfo(PagePointer).pointer.alignment == 4096);
    std.debug.assert(@typeInfo(PtPointer).pointer.alignment == 4096);
    std.debug.assert(max_slots == 64 * 512);
    std.debug.assert(max_slots <= std.math.maxInt(u16));
}

/// Storage only: does not publish PDEs, allocate data pages or synchronize
/// CPUs. Every PT, including the first one, uses this same ownership path.
/// Banks form a contiguous prefix even when PMM return fails. Capacity counts
/// descriptors, not allocated PT bodies; unused descriptors have null pages.
pub const Store = struct {
    banks: [max_banks]?*align(4096) Bank = .{null} ** max_banks,
    bank_count: usize = 0,

    pub fn capacity(self: *const Store) usize {
        return self.bank_count * slots_per_bank;
    }

    pub fn descriptor(self: *Store, slot: usize) ?*Descriptor {
        if (slot >= self.capacity()) return null;
        return &self.banks[slot / slots_per_bank].?.descriptors[slot % slots_per_bank];
    }

    pub fn descriptorConst(self: *const Store, slot: usize) ?*const Descriptor {
        if (slot >= self.capacity()) return null;
        return &self.banks[slot / slots_per_bank].?.descriptors[slot % slots_per_bank];
    }

    /// After detachment and shootdown, clear contents without losing ownership
    /// of pages whose return failed or pages being retained for immediate reuse.
    pub fn resetDetachedContents(self: *Store) void {
        for (0..self.capacity()) |slot| {
            const entry = self.descriptor(slot).?;
            entry.pml4 = no_index;
            entry.pdp = no_index;
            entry.pd = no_index;
            if (entry.page) |page| @memset(page, 0);
        }
    }

    /// Retire one unpublished/detached PT. Metadata banks remain until AS
    /// teardown, so slot pointers and the contiguous bank prefix stay stable.
    pub fn releasePageDetached(self: *Store, slot: usize, allocator: Allocator) void {
        const entry = self.descriptor(slot) orelse return;
        entry.pml4 = no_index;
        entry.pdp = no_index;
        entry.pd = no_index;
        if (entry.page) |page| {
            @memset(page, 0);
            if (allocator.release(allocator.context, @ptrCast(page))) entry.page = null;
        }
    }

    /// A request may use an existing bank or append exactly the next bank.
    /// Skipping banks and out-of-range slots fail without allocation/mutation.
    /// Existing PT contents and addresses are preserved. On allocation failure
    /// unpublished storage is returned, or retained if its return also fails.
    pub fn ensurePage(self: *Store, slot: usize, allocator: Allocator) ?PtPointer {
        if (slot >= max_slots) return null;
        const bank_index = slot / slots_per_bank;
        if (bank_index > self.bank_count) return null;
        const fresh_bank = bank_index == self.bank_count;
        const bank: *align(4096) Bank = if (fresh_bank) blk: {
            const raw = allocator.allocate(allocator.context) orelse return null;
            const allocated: *align(4096) Bank = @ptrCast(raw);
            allocated.* = .{}; // Includes nonzero index sentinels.
            break :blk allocated;
        } else self.banks[bank_index].?;
        const entry = &bank.descriptors[slot % slots_per_bank];
        if (entry.page) |page| return page;
        const raw = allocator.allocate(allocator.context) orelse {
            if (fresh_bank and !allocator.release(allocator.context, @ptrCast(bank))) {
                self.banks[bank_index] = bank;
                self.bank_count += 1;
            }
            return null;
        };
        @memset(raw, 0);
        const page: PtPointer = @ptrCast(raw);
        entry.page = page;
        if (fresh_bank) {
            self.banks[bank_index] = bank;
            self.bank_count += 1;
        }
        return page;
    }

    /// PRECONDITION: every parent reference is detached and all required CPU
    /// shootdowns have completed. Never call this from a mere metadata reset.
    /// A failed return retains ownership for safe reuse or a later retry.
    pub fn freeDetached(self: *Store, allocator: Allocator) void {
        var remaining = self.bank_count;
        while (remaining != 0) {
            remaining -= 1;
            const bank = self.banks[remaining].?;
            var empty = true;
            for (&bank.descriptors) |*entry| {
                entry.pml4 = no_index;
                entry.pdp = no_index;
                entry.pd = no_index;
                if (entry.page) |page| {
                    @memset(page, 0);
                    if (allocator.release(allocator.context, @ptrCast(page))) {
                        entry.page = null;
                    } else {
                        empty = false;
                    }
                }
            }
            // Only shrink the tail: a retained upper bank forbids holes in
            // the prefix, but does not prevent returning lower PT bodies.
            if (empty and remaining + 1 == self.bank_count and
                allocator.release(allocator.context, @ptrCast(bank)))
            {
                self.banks[remaining] = null;
                self.bank_count -= 1;
            }
        }
    }
};

const TestAllocator = struct {
    active: [16]?PagePointer = .{null} ** 16,
    allocate_calls: usize = 0,
    release_calls: usize = 0,
    fail_allocate_at: usize = 0,
    fail_release: ?PagePointer = null,
    fail_all_releases: bool = false,

    fn callbacks(self: *TestAllocator) Allocator {
        return .{ .context = self, .allocate = allocate, .release = release };
    }

    fn allocate(context: *anyopaque) ?PagePointer {
        const self: *TestAllocator = @ptrCast(@alignCast(context));
        self.allocate_calls += 1;
        if (self.allocate_calls == self.fail_allocate_at) return null;
        const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 4096) catch return null;
        const page: PagePointer = @ptrCast(bytes.ptr);
        @memset(page, 0xa5);
        for (&self.active) |*entry| {
            if (entry.* != null) continue;
            entry.* = page;
            return page;
        }
        unreachable;
    }

    fn release(context: *anyopaque, page: PagePointer) bool {
        const self: *TestAllocator = @ptrCast(@alignCast(context));
        self.release_calls += 1;
        for (&self.active) |*entry| {
            if (entry.* != page) continue;
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
        store.freeDetached(self.callbacks());
        for (self.active) |entry| std.debug.assert(entry == null);
    }
};

test "MMIO overlay overflow storage boundaries preserve addresses and content" {
    var fixture = TestAllocator{};
    var store = Store{};
    defer fixture.cleanup(&store);
    const callbacks = fixture.callbacks();
    const read_only: *const Store = &store;
    try std.testing.expect(store.ensurePage(64, callbacks) == null);
    try std.testing.expect(store.ensurePage(max_slots, callbacks) == null);
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
    try std.testing.expect(store.ensurePage(192, callbacks) == null);
    try std.testing.expectEqual(allocated, fixture.allocate_calls);
}

test "MMIO overlay overflow storage allocation failures retain ownership" {
    var fixture = TestAllocator{};
    var store = Store{};
    defer fixture.cleanup(&store);
    const callbacks = fixture.callbacks();
    fixture.fail_allocate_at = 1; // New bank allocation fails.
    try std.testing.expect(store.ensurePage(0, callbacks) == null);
    try std.testing.expectEqual(@as(usize, 0), store.capacity());
    fixture.fail_allocate_at = 3; // Bank succeeds, first PT fails.
    try std.testing.expect(store.ensurePage(0, callbacks) == null);
    try std.testing.expectEqual(@as(usize, 0), store.capacity());
    try std.testing.expectEqual(@as(usize, 1), fixture.release_calls);
    fixture.fail_allocate_at = 5;
    fixture.fail_all_releases = true;
    try std.testing.expect(store.ensurePage(0, callbacks) == null);
    try std.testing.expectEqual(@as(usize, 64), store.capacity());
    try std.testing.expect(store.descriptor(0).?.page == null);
    try std.testing.expectEqual(no_index, store.descriptor(0).?.pd);
    fixture.fail_all_releases = false;
    const first = store.ensurePage(0, callbacks).?;
    first[0] = 23;
    fixture.fail_allocate_at = fixture.allocate_calls + 1;
    try std.testing.expect(store.ensurePage(1, callbacks) == null);
    try std.testing.expectEqual(@as(u64, 23), first[0]);
    try std.testing.expect(store.descriptor(1).?.page == null);
    try std.testing.expectEqual(no_index, store.descriptor(1).?.pd);
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
    store.freeDetached(callbacks);
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
    store.freeDetached(callbacks);
    try std.testing.expectEqual(released + 2, fixture.release_calls); // Only retained PT and bank.
    try std.testing.expectEqual(@as(usize, 0), store.capacity());
    store.freeDetached(callbacks);
    try std.testing.expectEqual(released + 2, fixture.release_calls);
}

test "MMIO overlay overflow storage tail bank failure preserves contiguous prefix" {
    var fixture = TestAllocator{};
    var store = Store{};
    defer fixture.cleanup(&store);
    const callbacks = fixture.callbacks();
    _ = store.ensurePage(0, callbacks).?;
    _ = store.ensurePage(64, callbacks).?;
    fixture.fail_release = @ptrCast(store.banks[1].?);
    store.freeDetached(callbacks);
    try std.testing.expectEqual(@as(usize, 128), store.capacity());
    for (0..store.capacity()) |slot| try std.testing.expect(store.descriptor(slot).?.page == null);
    const released = fixture.release_calls;
    fixture.fail_release = null;
    store.freeDetached(callbacks);
    try std.testing.expectEqual(released + 2, fixture.release_calls); // Banks only; PTs already returned.
    try std.testing.expectEqual(@as(usize, 0), store.capacity());
}
