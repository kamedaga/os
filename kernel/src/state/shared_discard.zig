//! Shared content withdrawal. K owns alias publication; target AS loans and
//! shared VM exclusion remain held until every borrowed descriptor is done.
const std = @import("std");
const types = @import("types.zig");
const vm = @import("../memory/user_vm.zig");
const reclaim = @import("../memory/shared_discard_slots.zig");
const abi = @import("kernel_abi_root").fd_abi;
pub const Range = abi.VmoDiscardRange;
pub const Error = error{ Invalid, Alloc, NotReady };
pub var test_work: if (@import("builtin").is_test) struct { lock_attempts: usize = 0, locked: usize = 0, released: usize = 0, flushes: usize = 0 } else void = if (@import("builtin").is_test) .{} else {};

pub fn validateRanges(ranges: []const Range, pages: u64) Error!usize {
    if (ranges.len == 0 or ranges.len > 32) return error.Invalid;
    var end: u64 = 0;
    var total: u64 = 0;
    for (ranges) |range| {
        if (range.page_count == 0 or range.first_page < end) return error.Invalid;
        end = std.math.add(u64, range.first_page, range.page_count) catch return error.Invalid;
        total = std.math.add(u64, total, range.page_count) catch return error.Invalid;
        if (end > pages or total > 1024) return error.Invalid;
    }
    return @intCast(total);
}
const Span = struct { start: u64, end: u64 };
fn intersection(entry: *const types.VmaEntry, range: Range) ?Span {
    const first = @max(entry.vmo_offset, range.first_page * 4096);
    const last = @min(entry.vmo_offset + entry.size_bytes, (range.first_page + range.page_count) * 4096);
    if (first >= last) return null;
    return .{ .start = entry.start_va + first - entry.vmo_offset, .end = entry.start_va + last - entry.vmo_offset };
}
pub fn discard(state: anytype, ref: types.NativeVmoRef, ranges: []const Range, fl: *types.FreePageList) Error!bool {
    @import("../sync/lock_diagnostics.zig").assertKernelStateHeld();
    if (comptime @import("builtin").is_test) test_work = .{};
    const slot = state.nativeVmoSlot(ref) orelse return error.Invalid;
    if (slot.kind != .anonymous or !slot.backing.parent.ref.isNull() or !slot.has_page_store or !slot.zero_on_demand or
        slot.ram_cache_state == .retiring or slot.pending_data_return != null or slot.kernel_edges != 0 or slot.non_view_children != 0) return error.Invalid;
    var work = try validateRanges(ranges, slot.page_count);
    const owner = types.vmoBackingStoreOwner(.native_vmo, ref.index, ref.generation);
    if (!types.vmoBackingPageStoreOwnerMatches(slot.page_store_start, slot.page_count, slot.page_store_owner, owner)) return error.Invalid;
    var principals: [4]types.PrincipalId = undefined;
    var as_count: usize = 0;
    var aliases: usize = 0;
    var pair = slot.vma_alias_head;
    while (pair) |alias| : (pair = alias.vmo_alias_next) {
        aliases += 1;
        if (aliases > 1024 / ranges.len) return error.Invalid;
        const entry = alias.entry orelse return error.Invalid;
        const table = state.vmaTableForProcessIndex(alias.identity.owner) orelse return error.Invalid;
        const desc = state.processDescriptorSlotConst(alias.identity.owner) orelse return error.Invalid;
        if (desc.generation != alias.identity.owner_generation or table.entries.resolve(.{ .index = alias.identity.index, .generation = alias.identity.generation }) != entry or
            !entry.active or !std.meta.eql(entry.vmo, ref) or alias.vmo.target != &slot.references) return error.Invalid;
        const principal = @TypeOf(state.*).processPrincipal(alias.identity.owner);
        var pos: usize = 0;
        while (pos < as_count and @intFromEnum(principals[pos]) < @intFromEnum(principal)) : (pos += 1) {}
        if (pos == as_count or principals[pos] != principal) {
            if (as_count == principals.len) return error.Invalid;
            var i = as_count;
            while (i > pos) : (i -= 1) principals[i] = principals[i - 1];
            principals[pos] = principal;
            as_count += 1;
        }
        for (ranges) |range| if (intersection(entry, range)) |span| {
            work += @intCast((span.end - span.start) / 4096);
            if (work > 4096) return error.Invalid;
        };
    }
    // Borrow only participating ASes; K keeps this set stable. Unwind a failed
    // acquisition in reverse order, including every successful directory loan.
    var locked: usize = 0;
    defer while (locked != 0) {
        locked -= 1;
        vm.unlockAddressSpace(principals[locked]);
        if (comptime @import("builtin").is_test) test_work.released += 1;
    };
    for (principals[0..as_count]) |principal| {
        if (comptime @import("builtin").is_test) test_work.lock_attempts += 1;
        if (!vm.lockAddressSpace(principal)) return error.NotReady;
        locked += 1;
        if (comptime @import("builtin").is_test) test_work.locked += 1;
    }
    _ = fl; // PMM destination was fixed at boot, independently of this VMO.
    vm.lockSharedVmObjects();
    defer vm.unlockSharedVmObjects();
    var busy: [32]bool = @splat(false);
    // Reserve before any PTE/store mutation. K serializes producers; OOM and
    // exits may own draining slots, so exhaustion is a non-waiting NOT_READY.
    // No store/quarantine leaf survives into a possible K-held PMM drain.
    const retired = reclaim.global.claim() orelse return error.NotReady;
    defer retired.publish();
    var resident: [32]bool = @splat(false);
    for (ranges, 0..) |range, r| {
        slot.page_store.lock.lock();
        for (@as(u32, @intCast(range.first_page))..@as(u32, @intCast(range.first_page + range.page_count))) |index| {
            if (slot.page_store.viewRefsLocked(@intCast(index)) != 0) busy[r] = true;
            // K excludes cache writers even for an arena with no aliases.
            // Keep shared VM through detach so a validated WB page cannot gain
            // a lease between the busy decision and removal from its owner.
            const pa = slot.page_store.getLocked(@intCast(index));
            if (pa != 0) {
                resident[r] = true;
                // This check is sound together with K through detach and ALL
                // alias pin/view checks below: live DMA retains a pin; forced
                // teardown (fd.zig) publishes retention BEFORE dropping pins.
                // Thus neither a surviving mapping nor a new retention can
                // appear after admission. Detach removes every resolution
                // source, so an owned PA is safe to return outside K.
                if (@import("../iommu.zig").physicalRangeQuarantined(pa, 1)) busy[r] = true;
                if (slot.ram_cache_state == .cached and @import("../memory/ram_cache_snapshot.zig").policyForLockedPage(pa) != .ordinary) busy[r] = true;
            }
        }
        slot.page_store.lock.unlock();
        pair = slot.vma_alias_head;
        while (pair) |alias| : (pair = alias.vmo_alias_next) {
            const entry = alias.entry.?;
            if (intersection(entry, range)) |span| {
                const principal = @TypeOf(state.*).processPrincipal(alias.identity.owner);
                if (entry.flags.private or entry.flags.fork_cow or !entry.cow_table.isNull() or state.rangeOverlapsPinnedUserObject(principal, span.start, span.end - span.start)) busy[r] = true;
            }
        }
    }
    for (principals[0..as_count]) |principal| {
        var low: u64 = std.math.maxInt(u64);
        var high: u64 = 0;
        for (ranges, 0..) |range, r| {
            if (busy[r] or !resident[r]) continue;
            pair = slot.vma_alias_head;
            while (pair) |alias| : (pair = alias.vmo_alias_next) {
                if (@TypeOf(state.*).processPrincipal(alias.identity.owner) != principal) continue;
                if (intersection(alias.entry.?, range)) |span| {
                    // Failure may leave earlier aliases withdrawn, but backing
                    // remains owned and resolves to the original PA on fault.
                    low = @min(low, span.start);
                    high = @max(high, span.end);
                    if (!vm.withdrawSharedDiscardRange(principal, span.start, @intCast(span.end - span.start))) busy[r] = true;
                }
            }
        }
        if (low < high) {
            vm.flushSharedDiscard(principal, low, high);
            if (comptime @import("builtin").is_test) test_work.flushes += 1;
        }
    }
    // Every successful withdrawal has completed its synchronous ACK before
    // any data owner changes. A failed range keeps all of its original PAs.
    var all_done = true;
    for (ranges, 0..) |range, r| {
        if (busy[r]) {
            all_done = false;
            continue;
        }
        var first: usize = @intCast(range.first_page);
        const last: usize = @intCast(range.first_page + range.page_count);
        var pages: [64]types.BackedPage = undefined;
        while (first < last) {
            const count = types.snapshotVmoBackingPagesChecked(slot.page_store_start, slot.page_count, slot.page_store_owner, first, last, &pages) catch @panic("shared discard snapshot after ACK");
            if (count == 0) break;
            slot.page_store.lock.lock();
            if (!slot.page_store.matches(slot.page_store_start, slot.page_store_owner) or retired.count + count > reclaim.capacity) @panic("shared discard store ownership");
            for (pages[0..count], 0..) |page, i| {
                if (page.index < first or page.index >= last or page.paddr == 0 or page.paddr & 4095 != 0 or
                    (i != 0 and pages[i - 1].index >= page.index) or slot.page_store.getLocked(page.index) != page.paddr) @panic("shared discard changed after ACK");
            }
            for (pages[0..count]) |page| {
                // Record ownership in the same K/store exclusion as detach;
                // occupied is invisible to reclaim until every ACK/detach ends.
                retired.pages[retired.count] = page.paddr;
                retired.count += 1;
                slot.page_store.clearDirtyPageLocked(page.index);
                slot.page_store.putLocked(page.index, 0);
            }
            slot.page_store.lock.unlock();
            first = @as(usize, pages[count - 1].index) + 1;
        }
    }
    // All detach operations precede pruning. Pass a PAGE index inside each
    // touched leaf (not a leaf ID); busy ranges keep their live leaf.used.
    for (ranges, 0..) |range, r| {
        if (busy[r]) continue;
        var index = range.first_page;
        const end = range.first_page + range.page_count;
        while (index < end) {
            slot.page_store.pruneEmptyDeferred(slot.page_store_start, @intCast(index));
            index = ((index >> 8) + 1) << 8;
        }
    }
    return all_done;
}
