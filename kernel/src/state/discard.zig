//! Native content withdrawal without changing VMA/VMO/futex identity.
//! All state operations require K -> owner AS -> shared VM for their duration.
const std = @import("std");
const types = @import("types.zig");
const retire = @import("../memory/discard_pages.zig");
const refs = types.references;
const Error = types.KernelError;
pub var test_metadata_allocator: if (@import("builtin").is_test) ?@import("../memory/owned_storage.zig").Allocator else void = if (@import("builtin").is_test) null else {};
fn metadataAllocator(self: anytype, fl: *types.FreePageList) Error!@import("../memory/owned_storage.zig").Allocator {
    if (comptime @import("builtin").is_test) if (test_metadata_allocator) |allocator| return allocator;
    // K/AS/shared-VM exclusion serializes issuer setup. Only fully returned
    // metadata grants enter this cache; rejected data returns keep the batch
    // alive in the retirement queue. Boot registers the cache for PMM pressure.
    return self.discard_storage_cache.allocator(try types.backingAllocator(fl));
}
fn privateAnonymous(entry: *const types.VmaEntry) bool {
    return entry.active and !entry.device_backing and entry.flags.private and !entry.flags.shared and entry.flags.anonymous;
}
fn entryEligible(entry: *const types.VmaEntry) bool {
    return privateAnonymous(entry) and !entry.flags.fork_cow and entry.cow_table.isNull();
}

/// Unlike a sole-retain test, this permits disjoint mprotect/munmap fragments.
/// Every reverse edge is resolved against its live stable source; an FD, child,
/// stale source or overlapping alias cannot silently retain the discarded PA.
pub fn eligible(self: anytype, owner: types.PrincipalId, entry: *const types.VmaEntry, va: u64, length: u64) bool {
    if (!entryEligible(entry) or length == 0 or va < entry.start_va) return false;
    const last = std.math.add(u64, va, length) catch return false;
    const entry_end = std.math.add(u64, entry.start_va, entry.size_bytes) catch return false;
    if (last > entry_end) return false;
    const slot = self.nativeVmoSlot(entry.vmo) orelse return false;
    if (slot.kind != .anonymous or !slot.backing.parent.ref.isNull() or
        slot.ram_cache_state != .wb or !slot.has_page_store) return false;
    // Native anonymous VMAs already select allocate_zero on a missing slot
    // (prepareNativeVmaFaultMapping). zero_on_demand is the FD-VMO policy,
    // not a requirement on anonymous mmap; requiring it rejects every mmap.
    const expected_owner = types.vmoBackingStoreOwner(.native_vmo, entry.vmo.index, entry.vmo.generation);
    if (!types.vmoBackingPageStoreOwnerMatches(slot.page_store_start, slot.page_count, slot.page_store_owner, expected_owner)) return false;
    const first = std.math.add(u64, entry.vmo_offset, va - entry.start_va) catch return false;
    const end = std.math.add(u64, first, length) catch return false;
    if (first & 4095 != 0 or end > slot.size_bytes or end / 4096 > slot.page_count) return false;
    if (self.rangeOverlapsPinnedUserObject(owner, va, length)) return false;
    const process_index = types.processIndexFromPrincipal(owner) orelse return false;
    const process = self.processDescriptor(owner) orelse return false;
    const table = self.getVmaTableConst(owner) orelse return false;
    const counts = slot.references.ownership();
    if (counts.temporary != 0 or counts.edges == 0 or counts.edges != slot.ref_count) return false;
    var cursor = refs.Cursor.init(&slot.references);
    var count: u32 = 0;
    var found = false;
    while (cursor.next()) |edge| {
        count += 1;
        if (edge.identity.kind != .vma or edge.identity.owner != process_index or edge.identity.owner_generation != process.generation) return false;
        if (table.entries.generation(edge.identity.index) != edge.identity.generation) return false;
        const carrier = table.entries.get(edge.identity.index) orelse return false;
        if (!entryEligible(carrier) or !std.meta.eql(carrier.vmo, entry.vmo)) return false;
        const pair = carrier.references orelse return false;
        if (!edge.identity.eql(pair.identity)) return false;
        const carrier_end = std.math.add(u64, carrier.vmo_offset, carrier.size_bytes) catch return false;
        if (carrier_end > slot.size_bytes or carrier.vmo_offset & 4095 != 0 or carrier.size_bytes & 4095 != 0) return false;
        const range: refs.Range = .{ .start = carrier.start_va, .size = carrier.size_bytes, .offset = carrier.vmo_offset };
        if (!std.meta.eql(range, edge.range)) return false;
        if (carrier == entry) {
            if (found) return false;
            found = true;
        } else if (refs.overlap(first, length, edge.range.offset, edge.range.size)) return false;
    }
    return found and count == counts.edges;
}

pub const Plan = struct {
    head: ?*retire.Batch = null,
    start: u64,
    prefix_end: u64,
    end: u64,
    committed: bool = false,
    fail_after_shootdown: if (@import("builtin").is_test) bool else void = if (@import("builtin").is_test) false else {},
    pub fn commit(self: *Plan, context: anytype, comptime invalidate: anytype) Error!void {
        if (self.committed or self.prefix_end <= self.start) return Error.InvalidState;
        if (!invalidate(context, self.start, self.prefix_end - self.start)) return Error.InvalidState;
        if (comptime @import("builtin").is_test) if (self.fail_after_shootdown) return Error.InvalidState;
        self.detach();
    }
    /// Rollback discards only draft metadata: owned=0 keeps every data PA in
    /// its backing store. Returning metadata under a VM/store lock is forbidden.
    pub fn retireBatches(self: *Plan, queue: *retire.Queue) void {
        while (self.head) |batch| {
            self.head = batch.next;
            queue.push(batch) catch @panic("discard batch ownership");
        }
    }
    pub fn detach(self: *Plan) void {
        var cursor = self.head;
        while (cursor) |batch| : (cursor = batch.next) {
            const store_ref = batch.store;
            // All batches were checked before shootdown. Exclusion has not
            // ended; an identity mismatch now is corruption, not partial errno.
            retire.checkedDetach(batch) catch @panic("discard changed after prepare");
            // Empty paths may go only after the batch owns every withdrawn PA.
            for (batch.entries[0..batch.count]) |page| store_ref.store.?.pruneEmpty(store_ref, page.index);
        }
        self.committed = true;
    }
};

fn returnPage(context: *anyopaque, pa: u64) bool {
    const fl: *types.FreePageList = @ptrCast(@alignCast(context));
    fl.appendPage(pa) catch return false;
    return true;
}

/// No PTE/slot changes until this complete prefix (all split VMAs) is prepared.
/// On a hole Linux permits preceding effects with ENOMEM; stop at the first
/// hole. This does not claim identical side effects after multiple Linux holes.
pub fn prepare(self: anytype, owner: types.PrincipalId, start: u64, end: u64, fl: *types.FreePageList, queue: *retire.Queue) Error!Plan {
    var plan: Plan = .{ .start = start, .prefix_end = start, .end = end };
    errdefer plan.retireBatches(queue);
    var va = start;
    while (va < end) {
        const entry = self.vmaEntryForVaConst(owner, va) orelse break;
        const entry_end = std.math.add(u64, entry.start_va, entry.size_bytes) catch return Error.InvalidState;
        const next = @min(end, entry_end);
        if (next <= va or !eligible(self, owner, entry, va, next - va)) return Error.InvalidState;
        plan.prefix_end = next;
        va = next;
    }
    // Validate all VMAs before allocating; a later ineligible fragment must
    // not leave an earlier one discarded, nor reserve metadata unnecessarily.
    va = start;
    while (va < plan.prefix_end) {
        const entry = self.vmaEntryForVaConst(owner, va).?;
        const next = @min(plan.prefix_end, entry.endVa());
        const slot = self.nativeVmoSlot(entry.vmo).?;
        var first: usize = @intCast((entry.vmo_offset + va - entry.start_va) / 4096);
        const last: usize = first + @as(usize, @intCast((next - va) / 4096));
        var pages: [retire.capacity]types.BackedPage = undefined;
        while (first < last) {
            const count = try types.snapshotVmoBackingPagesChecked(slot.page_store_start, slot.page_count, slot.page_store_owner, first, last, &pages);
            if (count == 0) break;
            for (pages[0..count]) |page| try fl.validateReturnPage(page.paddr);
            const allocator = try metadataAllocator(self, fl);
            const batch = retire.prepare(allocator, fl, returnPage, slot.page_store_start, slot.page_count, pages[0..count]) catch return Error.OutOfFreePages;
            batch.next = plan.head;
            plan.head = batch;
            first = @as(usize, pages[count - 1].index) + 1;
        }
        va = next;
    }
    return plan;
}

pub fn validateFree(self: anytype, owner: types.PrincipalId, start: u64, end: u64) Error!bool {
    var va = start;
    while (va < end) {
        const entry = self.vmaEntryForVaConst(owner, va) orelse return false;
        const next = std.math.add(u64, entry.start_va, entry.size_bytes) catch return Error.InvalidState;
        const slot = self.nativeVmoSlot(entry.vmo) orelse return Error.InvalidState;
        if (next <= va or !privateAnonymous(entry) or slot.kind != .anonymous or !slot.backing.parent.ref.isNull()) return Error.InvalidState;
        va = next;
    }
    return true;
}
