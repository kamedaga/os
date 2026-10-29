//! CPU-write evidence for borrowed page views. Filesystem writeback policy is
//! outside the kernel. K/all-AS/shared-VM retain view identity during collect;
//! an individual AS plus its VMA reference suffices for conservative retirement.
const types = @import("types.zig");
const std = @import("std");

pub fn markRetiredAliases(self: anytype, owner: types.PrincipalId, start: u64, bytes: u64) void {
    const table = self.getVmaTableConst(owner) orelse return;
    const end = std.math.add(u64, start, bytes) catch std.math.maxInt(u64);
    // Retirement is also used for frequent anonymous/COW updates. Reuse
    // the AS address index so unrelated VMAs do not become scan overhead.
    var overlap = @constCast(table).overlapping(start, end);
    while (overlap.next()) |entry| {
        const slot = self.nativeVmoSlot(entry.vmo) orelse unreachable;
        // mprotect can leave a formerly writable alias read-only with D set.
        // The immutable creation ceiling, not today's VMA protection, matters.
        if (slot.kind == .page_view and slot.page_view_max_prot.write)
            @atomicStore(bool, &slot.page_view_dirty_all, true, .release);
    }
}

pub fn wordCount(slot: *const types.NativeVmoSlot) usize {
    return (@as(usize, slot.page_count) + 63) / 64;
}

pub fn record(slot: *types.NativeVmoSlot, page: u64, paddr: u64) void {
    if (page >= slot.page_count) return;
    slot.page_store.lock.lock();
    defer slot.page_store.lock.unlock();
    // A private COW leaf belongs to its private owner, not the borrowed file.
    if (slot.page_store.getLocked(@intCast(page)) != paddr) return;
    if (!slot.page_store.markDirtyLocked(@intCast(page))) unreachable;
}

pub fn word(slot: *types.NativeVmoSlot, index: usize) u64 {
    if (index >= wordCount(slot)) return 0;
    slot.page_store.lock.lock();
    defer slot.page_store.lock.unlock();
    const bits = if (@atomicLoad(bool, &slot.page_view_dirty_all, .acquire)) std.math.maxInt(u64) else slot.page_store.dirtyWordLocked(@intCast(index));
    const remaining = @as(usize, slot.page_count) - index * 64;
    return if (remaining < 64) bits & ((@as(u64, 1) << @intCast(remaining)) - 1) else bits;
}

/// Only after every output byte has been delivered, with all-AS still held.
/// Copyout failure leaves BOTH the bitmap and conservative retirement intact.
pub fn acknowledge(slot: *types.NativeVmoSlot) void {
    slot.page_store.lock.lock();
    defer slot.page_store.lock.unlock();
    for (0..wordCount(slot)) |i| slot.page_store.clearDirtyWordLocked(@intCast(i));
    @atomicStore(bool, &slot.page_view_dirty_all, false, .release);
}

pub fn harvest(self: anytype, view: types.NativeVmoRef, walker: anytype) types.KernelError!void {
    const slot = self.nativeVmoSlot(view) orelse return error.InvalidState;
    if (slot.kind != .page_view) return error.InvalidState;
    const Visitor = struct {
        slot: *types.NativeVmoSlot,
        pub fn record(v: @This(), page: u64, paddr: u64) void {
            @import("page_view_dirty.zig").record(v.slot, page, paddr);
        }
    };
    var cursor = types.references.Cursor.init(&slot.references);
    while (cursor.next()) |edge| {
        const id = edge.identity;
        if (id.kind != .vma) continue;
        const desc = self.processDescriptorSlotConst(id.owner) orelse return error.InvalidState;
        if (desc.generation != id.owner_generation) return error.InvalidState;
        const table = self.vmaTableForProcessIndex(id.owner) orelse return error.InvalidState;
        const entry = table.entries.resolve(.{ .index = id.index, .generation = id.generation }) orelse return error.InvalidState;
        if (!entry.active or !@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, view)) return error.InvalidState;
        if (!walker.collect(@TypeOf(self.*).processPrincipal(id.owner), entry.start_va, entry.size_bytes, entry.vmo_offset / 4096, Visitor{ .slot = slot })) return error.InvalidState;
    }
}

/// Physical copy windows bypass the destination user PTE. AS retains the VMA
/// until this atomic conservative mark is visible; this also works below F,
/// where entering a backing-store leaf would invert the futex lock order.
pub fn markSoftwareWrite(self: anytype, owner: types.PrincipalId, va: u64) void {
    const entry = self.vmaEntryForVaConst(owner, va) orelse return;
    const slot = self.nativeVmoSlot(entry.vmo) orelse unreachable;
    if (slot.kind == .page_view and slot.page_view_max_prot.write)
        @atomicStore(bool, &slot.page_view_dirty_all, true, .release);
}

/// Physical aliases include an owner's direct parent-RAM mapping, not only
/// VMAs naming this view. Sparse views are traversed by populated PFNs.
pub fn containsPhysicalPage(slot: *types.NativeVmoSlot, paddr: u64) bool {
    slot.page_store.lock.lock();
    defer slot.page_store.lock.unlock();
    var pages: [64]@import("../memory/vmo_backing.zig").BackedPage = undefined;
    var first: usize = 0;
    while (first < slot.page_count) {
        const n = slot.page_store.snapshotLocked(first, slot.page_count, &pages);
        if (n == 0) break;
        for (pages[0..n]) |page| if (page.paddr == paddr) return true;
        first = @as(usize, pages[n - 1].index) + 1;
    }
    return false;
}

/// Delivery is transactional with respect to the dirty inventory, even when
/// a destination accepts a prefix and then fails. Caller retains all-AS.
pub fn deliver(slot: *types.NativeVmoSlot, sink: anytype) bool {
    var first: usize = 0;
    while (first < wordCount(slot)) {
        var buffer: [64]u64 = undefined;
        const n = @min(buffer.len, wordCount(slot) - first);
        for (buffer[0..n], 0..) |*value, i| value.* = word(slot, first + i);
        if (!sink.write(first, buffer[0..n])) return false;
        first += n;
    }
    acknowledge(slot);
    return true;
}
