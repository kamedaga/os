//! Migration-only oracle. The caller holds K and every VM transaction; it
//! compares a stable old-table traversal with edges maintained at publication.
//! This file never withdraws sources or returns backing pages.
const std = @import("std");
const types = @import("types.zig");
const refs = types.references;
pub const enabled = if (@import("builtin").is_test) true else if (@hasDecl(@import("root"), "kernel_reference_checks_enabled")) @import("root").kernel_reference_checks_enabled else false;
// Host performance fixtures disable only the pin scan oracle; production follows the existing reference-check option.
pub const pin_oracle_enabled = enabled and (if (@import("builtin").is_test and @hasDecl(@import("root"), "kernel_pin_oracle_enabled")) @import("root").kernel_pin_oracle_enabled else true);
pub const Error = error{ Missing, Target, Identity, Range, Allocation, Extra, BrokenLink, Duplicate, Count, Ownership, VmaMark };

/// Stable-boundary checks also cover anonymous/logical entries without a VMO.
/// A linked flag alone is insufficient: lookup proves reachability from root.
pub fn vmaTable(table: *const types.VmaTable) Error!void {
    if (table.entries.firstMarkedAtOrAfter(0) != null) return error.VmaMark;
    if (table.address_index.count != table.active_count) return error.Count;
    var entries = table.entries.iterator();
    var count: usize = 0;
    while (entries.next()) |item| {
        const entry = item.value;
        if (!entry.active) continue;
        count += 1;
        const pair = entry.references orelse return error.Missing;
        if (!pair.address_node.linked) return error.Missing;
        if (pair.address_owner != table or pair.entry != entry) return error.Ownership;
        if (pair.address_node.key.start != entry.start_va or pair.address_node.key.slot != item.ref.index) return error.Identity;
        if (entry.active_position >= table.active_count) return error.Count;
        if ((table.active_indices.get(entry.active_position) orelse return error.Missing).* != item.ref.index) return error.Identity;
        if (table.entryForVa(entry.start_va) != entry) return error.Missing;
    }
    if (count != table.active_count) return error.Count;
}

/// Compare the frozen tree overlap set against the former full slot scan,
/// before any range/reference mutation can make the two views incomparable.
pub fn vmaOverlapMarks(table: *const types.VmaTable, start: u64, end: u64) Error!void {
    var entries = table.entries.iterator();
    while (entries.next()) |item| {
        const entry = item.value;
        const old_overlap = entry.active and start < entry.endVa() and entry.start_va < end;
        if (old_overlap != table.entries.isMarked(item.ref.index, 1)) return error.VmaMark;
    }
}

// Scalar failure evidence only; the caller's K/all-AS exclusion serializes
// the oracle. UART output happens after the target leaf has been released.
var range_failure: ?struct { identity: refs.Identity, expected: refs.Range, actual: refs.Range } = null;

fn expect(root: *refs.Root, node: ?*refs.Node, identity: refs.Identity, range: refs.Range) Error!void {
    const edge = node orelse return error.Missing;
    const flags = root.leaf.lock();
    defer root.leaf.unlock(flags);
    if (edge.target != root) return error.Target;
    var cursor = root.head;
    var visited: u32 = 0;
    while (cursor != edge) {
        const candidate = cursor orelse return error.Missing;
        if (visited >= root.count) return error.Count;
        visited += 1;
        cursor = candidate.next;
    }
    if (!edge.identity.eql(identity)) return error.Identity;
    if (!std.meta.eql(edge.range, range)) {
        range_failure = .{ .identity = identity, .expected = range, .actual = edge.range };
        return error.Range;
    }
    root.checked_seen += 1;
}

fn reset(root: *refs.Root) void {
    root.checked_seen = 0;
}
fn finish(root: *refs.Root, count: ?u32) Error!void {
    try root.validate(count);
    if (root.checked_seen != root.count) return error.Extra;
}
fn tracked(kind: types.KernelObjectKind) bool {
    return switch (kind) {
        .vmo, .mmio_region, .dma_buffer, .dma_mapping => true,
        else => false,
    };
}
fn allocation(self: anytype, node: *refs.Node) Error!void {
    if (self.reference_nodes.nodes.resolve(node.allocation) != node) return error.Allocation;
}
fn queue(self: anytype, q: *types.IpcQueue, identity: refs.Identity) Error!void {
    var endpoint_guard = q.acquire();
    defer endpoint_guard.end();
    for (0..q.len) |offset| {
        const index = q.slotIndex(offset);
        const msg = q.messageAt(index);
        var cursor = msg.reference_head;
        var positions: u32 = 0;
        while (cursor) |node| : (cursor = node.message_next) {
            const position = node.identity.position;
            if (position >= msg.fd_count or position >= types.max_ipc_message_fds) return error.Extra;
            const bit = @as(u32, 1) << @as(u5, @intCast(position));
            if (positions & bit != 0) return error.Duplicate;
            positions |= bit;
            if (!tracked(msg.fds[position].object().kind)) return error.Extra;
        }
        for (msg.fds[0..msg.fd_count], 0..) |item, position| {
            if (!tracked(item.object().kind)) continue;
            const node = msg.referenceAt(position) orelse return error.Missing;
            try allocation(self, node);
            const object = self.kernelObjectSlot(item.object()) orelse return error.Target;
            var expected = identity;
            expected.index = @intCast(index);
            expected.sequence = q.messageSequence(offset);
            expected.position = @intCast(position);
            try expect(&object.reference_state.?.holders, node, expected, .{});
        }
    }
}

pub fn check(self: anytype) Error!void {
    var vmos = self.native_vmos.iterator();
    while (vmos.next()) |item| reset(&item.value.references);
    // Independent source-table oracle: a missing alias cannot hide behind a
    // matching reverse-edge list maintained by the same publication helper.
    vmos = self.native_vmos.iterator();
    while (vmos.next()) |item| {
        const slot = item.value;
        var expected: usize = 0;
        for (0..self.process_capacity) |p| {
            const table = self.vmaTableForProcessIndex(p) orelse continue;
            var entries = table.entries.iterator();
            while (entries.next()) |source| {
                const entry = source.value;
                if (!entry.active or entry.vmo.index != item.ref.index or entry.vmo.generation != item.ref.generation) continue;
                expected += 1;
                const pair = entry.references orelse return error.Missing;
                var found = false;
                var cursor = slot.vma_alias_head;
                var visits: usize = 0;
                while (cursor) |alias| : (cursor = alias.vmo_alias_next) {
                    visits += 1;
                    if (visits > slot.references.count) return error.Count;
                    if (alias == pair) found = true;
                }
                if (!found) return error.Missing;
            }
        }
        var previous: ?*types.VmaReferences = null;
        var cursor = slot.vma_alias_head;
        var actual: usize = 0;
        while (cursor) |alias| : (cursor = alias.vmo_alias_next) {
            actual += 1;
            if (actual > expected or alias.vmo_alias_prev != previous or alias.vmo.target != &slot.references) return error.BrokenLink;
            const entry = alias.entry orelse return error.Missing;
            if (!entry.active or entry.references != alias or entry.vmo.index != item.ref.index or entry.vmo.generation != item.ref.generation) return error.Target;
            previous = alias;
        }
        if (actual != expected) return error.Count;
        var kernels: u32 = 0;
        var edge = slot.references.head;
        while (edge) |node| : (edge = node.next) if (node.identity.kind == .kernel) { kernels += 1; };
        if (kernels != slot.kernel_edges) return error.Count;
        var children: u32 = 0;
        var all = self.native_vmos.iterator();
        while (all.next()) |child| {
            if (child.value.kind == .anonymous and std.meta.eql(child.value.backing.parent.ref, types.NativeVmoRef{ .index = item.ref.index, .generation = item.ref.generation })) children += 1;
        }
        if (children != slot.non_view_children) return error.Count;
        if (slot.kind == .anonymous and slot.backing.parent.ref.isNull() and slot.has_page_store) try viewCounters(self, item.ref, slot);
        if (slot.kind == .page_view and slot.has_page_store) try viewParentPages(self, slot);

    }
    var cows = self.native_cow_tables.iterator();
    while (cows.next()) |item| reset(&item.value.references);
    var objects = self.fd_objects.iterator();
    while (objects.next()) |item| if (item.value.reference_state) |record| reset(&record.holders);
    for (0..self.process_capacity) |p| if (self.vmaTableForProcessIndex(p)) |table| reset(&table.pinned_objects);

    for (0..self.process_capacity) |p| {
        const desc = self.processDescriptorSlotConst(p) orelse continue;
        if (@import("../memory/user_vm.zig").getUserSpace(@enumFromInt(p))) |space| {
            if (!space.thread_identity_vmo.isNull()) {
                const vmo = self.nativeVmoSlot(space.thread_identity_vmo) orelse return error.Target;
                try expect(&vmo.references, &space.thread_identity_reference,
                    .{ .kind = .kernel, .owner = @intCast(p), .sequence = space.thread_identity_cookie }, .{});
            }
        }
        if (self.vmaTableForProcessIndex(p)) |table| {
            try vmaTable(table);
            for (0..table.active_count) |position| {
                const index = table.active_indices.get(@intCast(position)).?.*;
                const entry = table.entries.get(index).?;
                if (!entry.active or entry.vmo.isNull()) continue;
                const pair = entry.references orelse return error.Missing;
                const identity: refs.Identity = .{ .kind = .vma, .owner = @intCast(p), .owner_generation = desc.generation, .index = index, .generation = table.entries.generation(index).? };
                const vmo = self.nativeVmoSlot(entry.vmo) orelse return error.Target;
                try expect(&vmo.references, &pair.vmo, identity, .{ .start = entry.start_va, .size = entry.size_bytes, .offset = entry.vmo_offset });
                if (!entry.cow_table.isNull()) {
                    const cow = self.nativeCowTableSlot(entry.cow_table) orelse return error.Target;
                    try expect(&cow.references, &pair.cow, identity, .{ .start = entry.start_va, .size = entry.size_bytes, .offset = @as(u64, entry.cow_page_offset) * types.native_page_size });
                }
            }
        }
        if (self.fdTableForProcessIndex(p)) |table| for (0..table.capacity) |index| {
            const entry = table.at(index);
            if (!tracked(entry.object.kind)) continue;
            const node = entry.authority.reference orelse return error.Missing;
            try allocation(self, node);
            const object = self.kernelObjectSlot(entry.object) orelse return error.Target;
            try expect(&object.reference_state.?.holders, node, .{ .kind = .fd_entry, .owner = @intCast(p), .owner_generation = desc.generation, .index = @intCast(index), .generation = node.allocation.generation, .sequence = node.allocation.index }, .{});
        };
    }
    objects = self.fd_objects.iterator();
    while (objects.next()) |item| {
        const slot = item.value;
        if (slot.payload == .vmo) {
            const vmo = self.nativeVmoSlot(slot.payload.vmo) orelse return error.Target;
            try expect(&vmo.references, &slot.reference_state.?.payload_edge, .{ .kind = .fd_object, .index = item.ref.index, .generation = item.ref.generation }, .{});
        }
        if (slot.ref_count != 0) switch (slot.payload) {
            inline .mmio_region, .dma_buffer, .dma_mapping => |payload| {
                const p = types.processIndexFromPrincipal(@enumFromInt(payload.owner_principal_raw)) orelse return error.Identity;
                const desc = self.processDescriptorSlotConst(p) orelse return error.Identity;
                const table = self.vmaTableForProcessIndex(p) orelse return error.Identity;
                try expect(&table.pinned_objects, &slot.reference_state.?.payload_edge, .{ .kind = .pinned_object, .container = @intFromEnum(slot.kind), .owner = @intCast(p), .owner_generation = desc.generation, .index = item.ref.index, .generation = item.ref.generation }, .{ .start = payload.user_va, .size = payload.size });
                if (!slot.reference_state.?.payload_edge.pin.linked) return error.Missing;
            },
            else => {},
        };
    }
    vmos = self.native_vmos.iterator();
    while (vmos.next()) |item| {
        const slot = item.value;
        if (slot.kind != .anonymous and slot.kind != .page_view) continue;
        if (slot.backing.parent.ref.isNull()) continue;
        const parent = self.nativeVmoSlot(slot.backing.parent.ref) orelse return error.Target;
        try expect(&parent.references, &slot.parent_edge, .{ .kind = .child, .index = item.ref.index, .generation = item.ref.generation }, .{ .size = slot.size_bytes, .offset = slot.backing.parent.offset });
    }
    if (!self.kernel_clock_page_vmo.isNull()) {
        const vmo = self.nativeVmoSlot(self.kernel_clock_page_vmo) orelse return error.Target;
        try expect(&vmo.references, &self.clock_reference, .{ .kind = .kernel }, .{});
    }
    var endpoints = self.ipc_endpoints.iterator();
    while (endpoints.next()) |item| try queue(self, &item.value.queue, .{ .kind = .ipc, .container = 1, .owner = item.ref.index, .owner_generation = item.ref.generation });
    var channels = self.ipc_channels.iterator();
    while (channels.next()) |item| for (&item.value.queues, 0..) |*q, direction| try queue(self, q, .{ .kind = .ipc, .container = 2, .owner = item.ref.index, .owner_generation = item.ref.generation, .direction = @intCast(direction) });

    vmos = self.native_vmos.iterator();
    while (vmos.next()) |item| try finish(&item.value.references, item.value.ref_count);
    cows = self.native_cow_tables.iterator();
    while (cows.next()) |item| try finish(&item.value.references, item.value.ref_count);
    objects = self.fd_objects.iterator();
    while (objects.next()) |item| if (tracked(item.value.kind)) try finish(&item.value.reference_state.?.holders, item.value.ref_count);
    for (0..self.process_capacity) |p| if (self.vmaTableForProcessIndex(p)) |table| {
        try finish(&table.pinned_objects, null);
        var pins: usize = 0;
        for (&table.pinned_index.trees) |*tree| pins += tree.count;
        if (pins != table.pinned_objects.count) return error.Count;
        if (@import("builtin").is_test) table.pinned_index.validate() catch return error.Range;
    };
}

pub fn failure(err: anyerror) noreturn {
    if (@import("builtin").is_test) @panic(@errorName(err));
    // No callback from a failed ownership proof: the protected state may be
    // corrupt. UART is independent of VM, K, allocator and log-ring locks.
    asm volatile ("cli" ::: .{ .memory = true });
    @import("../serial.zig").writeRaw("reference-check mismatch: ");
    @import("../serial.zig").writeRaw(@errorName(err));
    @import("../serial.zig").writeRaw("\n");
    if (err == error.Range) if (range_failure) |detail| {
        const serial = @import("../serial.zig");
        serial.writeRaw(@tagName(detail.identity.kind));
        inline for (.{ @as(u64, detail.identity.owner), @as(u64, detail.identity.index), detail.expected.start, detail.expected.size, detail.expected.offset, detail.actual.start, detail.actual.size, detail.actual.offset }) |value| {
            serial.writeRaw(" ");
            serial.writeHexRaw(value);
        }
        serial.writeRaw("\n");
    };
    while (true) asm volatile ("hlt");
}

fn overlap(a: u64, n: u64, b: u64, m: u64) bool {
    if (n == 0 or m == 0) return false;
    const ae = std.math.add(u64, a, n) catch return true;
    const be = std.math.add(u64, b, m) catch return true;
    return a < be and b < ae;
}

pub fn pinnedOverlap(self: anytype, owner: types.PrincipalId, start: u64, size: u64, except: ?types.KernelObjectRef) bool {
    const p = types.processIndexFromPrincipal(owner) orelse return false;
    const table = self.vmaTableForProcessIndex(p) orelse return false;
    const root = &table.pinned_objects;
    const flags = root.leaf.lock();
    defer root.leaf.unlock(flags);
    var cursor = root.head;
    while (cursor) |node| : (cursor = node.next) {
        if (except) |object| if (node.identity.index == object.index and node.identity.generation == object.generation and node.identity.container == @intFromEnum(object.kind)) continue;
        if (overlap(start, size, node.range.start, node.range.size)) return true;
    }
    return false;
}

/// This compares the old carrier proof, not the future C reclamation policy.
/// An unpublished retain is counted separately in ownership validation. B must
/// not silently change the old release decision when a caller holds that loan.
pub fn hasBackingCarrier(root: *refs.Root, first: usize, count: usize) bool {
    const flags = root.leaf.lock();
    defer root.leaf.unlock(flags);
    var cursor = root.head;
    while (cursor) |node| : (cursor = node.next) switch (node.identity.kind) {
        .child, .fd_object => return true,
        .vma => if (overlap(first * types.native_page_size, count * types.native_page_size, node.range.offset, node.range.size)) return true,
        else => {},
    };
    return false;
}

// Lifetime census is independent of edge/refcount equality: a leaked retain
// raises both sides of that equality, but remains visible here after a cycle.
// K + all VM transactions exclude publishers; output only after releasing VM.
pub const Census = struct {
    free: usize = 0,
    vmos: usize = 0,
    cows: usize = 0,
    objects: usize = 0,
    channels: usize = 0,
    nodes: usize = 0,
    records: usize = 0,
    pairs: usize = 0,
    temporary: u64 = 0,
    storage: usize = 0,
    pending: usize = 0,
};
fn ownerPages(owner: anytype, out: *Census) void {
    inline for (.{ "published", "retired", "pending" }) |field| {
        var cursor = @field(owner, field);
        while (cursor) |record| : (cursor = record.next) {
            out.storage += record.allocation.pages;
            if (comptime !std.mem.eql(u8, field, "published")) out.pending += record.allocation.pages;
        }
    }
}
fn tablePages(table: anytype, out: *Census) void {
    inline for (.{ "directory_owner", "metadata_owner", "body_owner" }) |field| ownerPages(&@field(table, field), out);
}
pub fn census(self: anytype, free: *types.FreePageList) Census {
    var out: Census = .{ .free = free.pageCount(), .vmos = self.native_vmos.live, .cows = self.native_cow_tables.live, .objects = self.fd_objects.live, .channels = self.ipc_channels.live, .nodes = self.reference_nodes.nodes.live, .records = self.object_reference_slots.live };
    var vmos = self.native_vmos.iterator();
    while (vmos.next()) |item| out.temporary += item.value.references.temporary;
    var cows = self.native_cow_tables.iterator();
    while (cows.next()) |item| out.temporary += item.value.references.temporary;
    var objects = self.fd_objects.iterator();
    while (objects.next()) |item| if (item.value.reference_state) |r| {
        out.temporary += r.holders.temporary;
    };
    inline for (.{ "fd_objects", "native_vmos", "native_cow_tables", "ipc_channels", "ipc_endpoints", "ipc_replies", "object_reference_slots" }) |field| tablePages(&@field(self, field), &out);
    tablePages(&self.reference_nodes.nodes, &out);
    for (0..self.process_capacity) |p| if (self.vmaTableForProcessIndex(p)) |table| {
        out.pairs += table.reference_slots.live;
        tablePages(&table.reference_slots, &out);
        tablePages(&table.entries, &out);
        tablePages(&table.active_indices, &out);
    };
    return out;
}
pub fn report(value: Census) void {
    @import("../kernel_log.zig").writeFmt("reference-live free={} vmo={} cow={} object={} channel={} node={} record={} pair={} temporary={} storage-pages={} pending-pages={}\n", .{ value.free, value.vmos, value.cows, value.objects, value.channels, value.nodes, value.records, value.pairs, value.temporary, value.storage, value.pending });
}

// Recount from independently enumerated view records. No pair of Store locks
// is held together; the caller's outer transaction excludes refresh/teardown.
fn viewCounters(self: anytype, ref: anytype, parent: *types.NativeVmoSlot) Error!void {
    var first: usize = 0;
    var pages: [64]types.BackedPage = undefined;
    while (first < parent.page_count) {
        const count = types.snapshotVmoBackingPagesChecked(parent.page_store_start, parent.page_count, parent.page_store_owner, first, parent.page_count, &pages) catch return error.Ownership;
        if (count == 0) break;
        for (pages[0..count]) |page| {
            var expected: u32 = 0;
            var views = self.native_vmos.iterator();
            while (views.next()) |item| {
                const view = item.value;
                if (view.kind != .page_view or view.backing.parent.ref.index != ref.index or view.backing.parent.ref.generation != ref.generation) continue;
                var next: usize = 0;
                var borrowed: [64]types.BackedPage = undefined;
                while (next < view.page_count) {
                    const n = types.snapshotVmoBackingPagesChecked(view.page_store_start, view.page_count, view.page_store_owner, next, view.page_count, &borrowed) catch return error.Ownership;
                    if (n == 0) break;
                    view.page_store.lock.lock();
                    for (borrowed[0..n]) |entry| {
                        const encoded = view.page_store.parentIndexLocked(entry.index);
                        if (encoded == 0) { view.page_store.lock.unlock(); return error.Ownership; }
                        if (encoded - 1 == page.index) {
                            if (entry.paddr != page.paddr) { view.page_store.lock.unlock(); return error.Target; }
                            expected += 1;
                        }
                    }
                    view.page_store.lock.unlock();
                    next = @as(usize, borrowed[n - 1].index) + 1;
                }
            }
            parent.page_store.lock.lock();
            const actual = parent.page_store.viewRefsLocked(page.index);
            parent.page_store.lock.unlock();
            if (actual != expected) return error.Count;
        }
        first = @as(usize, pages[count - 1].index) + 1;
    }
}

fn viewParentPages(self: anytype, view: *types.NativeVmoSlot) Error!void {
    const parent = self.nativeVmoSlot(view.backing.parent.ref) orelse return error.Target;
    var first: usize = 0;
    var pages: [64]types.BackedPage = undefined;
    var indices: [64]u32 = undefined;
    while (first < view.page_count) {
        const n = types.snapshotVmoBackingPagesChecked(view.page_store_start, view.page_count, view.page_store_owner, first, view.page_count, &pages) catch return error.Ownership;
        if (n == 0) break;
        view.page_store.lock.lock();
        for (pages[0..n], 0..) |page, i| indices[i] = view.page_store.parentIndexLocked(page.index);
        view.page_store.lock.unlock();
        parent.page_store.lock.lock();
        for (pages[0..n], indices[0..n]) |page, encoded| {
            if (encoded == 0 or encoded - 1 >= parent.page_count or parent.page_store.getLocked(encoded - 1) != page.paddr) {
                parent.page_store.lock.unlock();
                return error.Target;
            }
        }
        parent.page_store.lock.unlock();
        first = @as(usize, pages[n - 1].index) + 1;
    }
}
