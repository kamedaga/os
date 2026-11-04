//! Owner-local sparse PFN index. Callers keep the containing VMO/COW body alive
//! across prepare/commit and exclude final destruction with their resource lock.
const std = @import("std");
const builtin = @import("builtin");
const storage = @import("owned_storage.zig");
const lock_diagnostics = @import("../sync/lock_diagnostics.zig");

pub const Lock = struct {
    byte: lock_diagnostics.Byte = .{},
    takes: if (builtin.is_test) usize else void = if (builtin.is_test) 0 else {},
    pub fn lock(self: *Lock) void {
        while (true) {
            const flags = lock_diagnostics.maskInterrupts();
            lock_diagnostics.beforeAcquire(&self.byte.record, .{ .class = .vmo_backing });
            if (@cmpxchgWeak(u8, &self.byte.value, 0, 1, .acquire, .monotonic) == null) {
                lock_diagnostics.acquired(&self.byte.record, .{ .class = .vmo_backing });
                if (builtin.is_test) self.takes += 1;
                lock_diagnostics.restoreInterrupts(flags);
                return;
            }
            lock_diagnostics.restoreInterrupts(flags);
            while (@atomicLoad(u8, &self.byte.value, .monotonic) != 0) {
                if (builtin.is_test) std.atomic.spinLoopHint() else {
                    var saved: u64 = 0;
                    asm volatile ("pushfq; pop %[flags]; sti; pause; cli"
                        : [flags] "=r" (saved),
                        :
                        : .{ .memory = true });
                    if (saved & (1 << 9) != 0) asm volatile ("sti" ::: .{ .memory = true });
                }
            }
        }
    }
    pub fn unlock(self: *Lock) void {
        const flags = lock_diagnostics.maskInterrupts();
        defer lock_diagnostics.restoreInterrupts(flags);
        lock_diagnostics.releasing(&self.byte.record);
        @atomicStore(u8, &self.byte.value, 0, .release);
    }
};

pub const Leaf = struct {
    pages: [256]u64 = .{0} ** 256,
    view_refs: [256]u16 = .{0} ** 256,
    parent_index: [256]u32 = .{0} ** 256,
    used: u32 = 0,
    // Survives removal of the last CPU alias; metadata is reserved alongside
    // PFNs, so retiring mappings never needs an allocation to retain writes.
    dirty: [4]u64 = .{0} ** 4,
};
const Directory = @import("radix.zig").Directory(Leaf);
const Node = Directory.Node;
const levels = 4;
const Page = struct {
    allocator: storage.Allocator,
    grant: storage.PhysicalExtent,
    next: ?*Page = null,
    test_next: if (builtin.is_test) ?*Page else void = if (builtin.is_test) null else {},
    payload: union { node: Node, leaf: Leaf },
};
comptime {
    std.debug.assert(@sizeOf(Page) <= 4096);
}
pub const BackedPage = struct { index: u32, paddr: u64 };
pub const Error = error{ OutOfMemory, InvalidState };
pub const Ref = struct { store: ?*Store = null, owner: u64 = 0 };

// Only rejected returns use this global queue. Normal lookup/insertion never
// takes its lock. A detached page owns its issuer even after its VMO dies.
var pending_lock: Lock = .{};
var pending: ?*Page = null;
var pending_pages: usize = 0;
// Kept separate from pending: its drain can run under K/AS/shared VM. These
// detached grants may call PMM only after those outer exclusions have ended.
var deferred_lock: @import("slot_table.zig").Leaf = .{};
var deferred: ?*Page = null;
var deferred_pages: usize = 0;
var used_pages: u64 = 0;
var metadata_pages: u64 = 0;
var allocations: u64 = 0;
var test_pages: if (builtin.is_test) ?*Page else void = if (builtin.is_test) null else {};
pub var test_allocator: if (builtin.is_test) ?storage.Allocator else void = if (builtin.is_test) null else {};
pub fn resetForTest() void {
    if (!builtin.is_test) @compileError("test-only reset");
    // Serial fixture teardown: all former body loans ended before this call.
    reclaim();
    reclaimDeferred();
    std.debug.assert(!hasPending() and !hasDeferred());
    while (test_pages) |page| release(page);
    used_pages = 0;
    allocations = 0;
    test_allocator = null;
}

fn header(ptr: anytype) *Page {
    return @ptrFromInt(@intFromPtr(ptr) - @offsetOf(Page, "payload"));
}
fn acquire(allocator: storage.Allocator, leaf: bool) Error!*Page {
    const grant = allocator.acquire(allocator.context, 1) orelse return error.OutOfMemory;
    const p: *Page = @ptrCast(grant.bytes.ptr);
    p.* = .{ .allocator = allocator, .grant = grant.allocation, .payload = if (leaf) .{ .leaf = .{} } else .{ .node = .{} } };
    _ = @atomicRmw(u64, &metadata_pages, .Add, 1, .monotonic);
    _ = @atomicRmw(u64, &allocations, .Add, 1, .monotonic);
    if (builtin.is_test) {
        pending_lock.lock();
        p.test_next = test_pages;
        test_pages = p;
        pending_lock.unlock();
    }
    return p;
}
// Queue-neutral return: callback success invalidates the grant immediately.
// The registry must be withdrawn before that callback, and accounting changes
// only on success. Both retry domains share these responsibilities.
fn releaseGrant(allocator: storage.Allocator, grant: storage.PhysicalExtent) bool {
    if (builtin.is_test) {
        pending_lock.lock();
        var link = &test_pages;
        while (link.*) |page| {
            // Host issuers can use equal synthetic PA tokens for different
            // mapped grants. Unregister the exact issuer/grant, never a page
            // belonging to another live owner before its callback runs.
            if (page.allocator.context == allocator.context and page.allocator.acquire == allocator.acquire and
                page.allocator.release == allocator.release and page.allocator.release_mapped == allocator.release_mapped and
                page.grant.physical.toInt() == grant.physical.toInt() and page.grant.pages == grant.pages and page.grant.region_id == grant.region_id) {
                link.* = page.test_next;
                break;
            }
            link = &page.test_next;
        }
        pending_lock.unlock();
    }
    if (!allocator.release(allocator.context, grant)) return false;
    _ = @atomicRmw(u64, &metadata_pages, .Sub, 1, .monotonic);
    return true;
}
fn release(p: *Page) void {
    const allocator = p.allocator;
    const grant = p.grant;
    if (releaseGrant(allocator, grant)) return;
    pending_lock.lock();
    p.next = pending;
    pending = p;
    _ = @atomicRmw(usize, &pending_pages, .Add, 1, .release);
    pending_lock.unlock();
}
fn enqueueDeferred(p: *Page) void {
    const irq = deferred_lock.lock();
    p.next = deferred;
    deferred = p;
    _ = @atomicRmw(usize, &deferred_pages, .Add, 1, .release);
    deferred_lock.unlock(irq);
    // Publication transfers ownership; another CPU may already have freed p.
}
pub fn hasDeferred() bool {
    return @atomicLoad(usize, &deferred_pages, .acquire) != 0;
}
pub fn reclaimDeferred() void {
    const irq = deferred_lock.lock();
    var list = deferred;
    deferred = null;
    @atomicStore(usize, &deferred_pages, 0, .release);
    deferred_lock.unlock(irq);
    // Each invocation owns a finite snapshot. A forbidden callback reentry
    // can see only the public queue, never this invocation's remaining pages.
    while (list) |p| {
        const next = p.next;
        const allocator = p.allocator;
        const grant = p.grant;
        if (!releaseGrant(allocator, grant)) enqueueDeferred(p);
        list = next;
    }
}
pub fn hasPending() bool {
    return @atomicLoad(usize, &pending_pages, .acquire) != 0;
}
// Detach one finite batch; rejected grants wait until the next drain.
pub fn reclaim() void {
    pending_lock.lock();
    var list = pending;
    pending = null;
    @atomicStore(usize, &pending_pages, 0, .release);
    pending_lock.unlock();
    while (list) |p| {
        list = p.next;
        release(p);
    }
}
pub fn stats() struct { used: u64, metadata: u64, allocations: u64, pending: usize } {
    pending_lock.lock();
    const n = @atomicLoad(usize, &pending_pages, .acquire);
    pending_lock.unlock();
    return .{ .used = @atomicLoad(u64, &used_pages, .monotonic), .metadata = @atomicLoad(u64, &metadata_pages, .monotonic), .allocations = @atomicLoad(u64, &allocations, .monotonic), .pending = n };
}

pub const Store = struct {
    lock: Lock = .{},
    root: ?*Node = null,
    owner: u64 = 0,
    used: u64 = 0,
    // Host-only work accounting; no runtime instrumentation in normal builds.
    visits: if (builtin.is_test) usize else void = if (builtin.is_test) 0 else {},

    pub fn init(self: *Store, owner: u64) Ref {
        std.debug.assert(self.root == null and self.used == 0 and owner != 0);
        self.owner = owner;
        return .{ .store = self, .owner = owner };
    }
    pub fn matches(self: *Store, ref: Ref, owner: u64) bool {
        return owner != 0 and ref.store == self and ref.owner == owner and self.owner == owner;
    }
    pub fn getLocked(self: *Store, index: u32) u64 {
        const leaf = Directory.get(self.root, index >> 8, levels) orelse return 0;
        return leaf.pages[index & 255];
    }

    fn missingDepth(self: *Store, index: u32) ?usize {
        var node = self.root orelse return 0;
        for (0..levels) |depth| {
            const shift: u5 = @intCast((levels - depth - 1) * 6);
            node = node.children[((index >> 8) >> shift) & 63] orelse return depth + 1;
        }
        return if (node.block == null) levels + 1 else null;
    }
    const Draft = [levels + 2]?*Page;
    fn reservePath(missing: usize, allocator: storage.Allocator) Error!Draft {
        var draft: Draft = .{null} ** (levels + 2);
        errdefer discardPath(draft);
        for (missing..levels + 2) |depth|
            draft[depth] = try acquire(allocator, depth == levels + 1);
        return draft;
    }
    fn discardPath(draft: Draft) void {
        for (draft) |p| if (p) |page| release(page);
    }
    fn publishPathLocked(self: *Store, index: u32, draft: *Draft) void {
        var link = &self.root;
        for (0..levels + 1) |depth| {
            if (link.* == null) {
                link.* = &draft[depth].?.payload.node;
                draft[depth] = null;
            }
            const node = link.*.?;
            if (depth == levels) {
                if (node.block == null) {
                    node.block = &draft[levels + 1].?.payload.leaf;
                    draft[levels + 1] = null;
                }
            } else {
                const shift: u5 = @intCast((levels - depth - 1) * 6);
                link = &node.children[((index >> 8) >> shift) & 63];
            }
        }
    }
    /// Enter/return holding the owner lock. A missing path needs a second
    /// acquisition for publication; fault commits fold their prior lookup in.
    pub fn ensureLocked(self: *Store, ref: Ref, index: u32, allocator: storage.Allocator) Error!void {
        while (true) {
            if (!self.matches(ref, ref.owner)) return error.InvalidState;
            const missing = self.missingDepth(index) orelse return;
            self.lock.unlock();
            var draft = reservePath(missing, allocator) catch |err| {
                self.lock.lock();
                return err;
            };
            self.lock.lock();
            const valid = self.matches(ref, ref.owner);
            const now = if (valid) self.missingDepth(index) else null;
            if (valid and now != null and now.? >= missing)
                self.publishPathLocked(index, &draft);
            var spare = false;
            for (draft) |p| if (p != null) {
                spare = true;
                break;
            };
            if (spare) {
                self.lock.unlock();
                discardPath(draft);
                self.lock.lock();
            }
            if (!valid) return error.InvalidState;
        }
    }
    /// The caller holds the existing VM lifetime exclusion throughout. Parent
    /// resolution and allocation run unlocked. One initial lookup and at most
    /// one publication acquisition in the uncontended cold fault path.
    pub fn resolveOrInsert(self: *Store, ref: Ref, index: u32, candidate: u64, allocator: ?storage.Allocator, has_parent: bool, context: anytype, comptime inherited: anytype) Error!u64 {
        while (true) {
            self.lock.lock();
            if (!self.matches(ref, ref.owner)) {
                self.lock.unlock();
                return error.InvalidState;
            }
            const existing = self.getLocked(index);
            if (existing != 0) {
                self.lock.unlock();
                return existing;
            }
            const missing = self.missingDepth(index);
            if (!has_parent and missing == null) {
                self.putLocked(index, candidate);
                self.lock.unlock();
                return candidate;
            }
            self.lock.unlock();
            if (has_parent) {
                const parent = try inherited(context);
                // The caller retains outer VM exclusion, keeping both
                // owners and their parent edge alive through this return.
                if (parent != 0) return parent;
            }
            var draft = if (missing) |depth| try reservePath(depth, allocator orelse return error.OutOfMemory) else @as(Draft, .{null} ** (levels + 2));
            self.lock.lock();
            const valid = self.matches(ref, ref.owner);
            var result: ?u64 = null;
            if (valid) {
                const winner = self.getLocked(index);
                if (winner != 0) {
                    result = winner;
                } else {
                    const now = self.missingDepth(index);
                    if (now != null and missing != null and now.? >= missing.?)
                        self.publishPathLocked(index, &draft);
                    if (self.hasPathLocked(index)) {
                        self.putLocked(index, candidate);
                        result = candidate;
                    }
                }
            }
            self.lock.unlock();
            discardPath(draft);
            if (!valid) return error.InvalidState;
            if (result) |pa| return pa;
        }
    }
    pub fn prepare(self: *Store, ref: Ref, index: u32, allocator: storage.Allocator) Error!void {
        self.lock.lock();
        defer self.lock.unlock();
        try self.ensureLocked(ref, index, allocator);
    }
    pub fn hasPathLocked(self: *Store, index: u32) bool {
        return Directory.get(self.root, index >> 8, levels) != null;
    }
    pub fn putLocked(self: *Store, index: u32, paddr: u64) void {
        const leaf = Directory.get(self.root, index >> 8, levels).?;
        const previous = leaf.pages[index & 255];
        leaf.pages[index & 255] = paddr;
        if (previous == 0 and paddr != 0) {
            leaf.used += 1;
            self.used += 1;
            _ = @atomicRmw(u64, &used_pages, .Add, 1, .monotonic);
        } else if (previous != 0 and paddr == 0) {
            leaf.used -= 1;
            self.used -= 1;
            _ = @atomicRmw(u64, &used_pages, .Sub, 1, .monotonic);
        }
    }
    pub fn viewRefsLocked(self: *Store, index: u32) u16 {
        const leaf = Directory.get(self.root, index >> 8, levels) orelse return 0;
        return leaf.view_refs[index & 255];
    }
    pub fn parentIndexLocked(self: *Store, index: u32) u32 {
        const leaf = Directory.get(self.root, index >> 8, levels) orelse return 0;
        return leaf.parent_index[index & 255];
    }
    /// Outer K/AS/shared exclusion keeps destinations unchanged between the
    /// three stages. Never nest parent and view Store locks: scatter indices
    /// can reach any parent leaf, and allocation must precede either publish.
    pub fn installView(self: *Store, ref: Ref, count: u32, offset: usize, parent: Ref, parent_count: u32, indices: []const u64, sparse: bool, allocator: storage.Allocator) Error!void {
        if (indices.len == 0 or indices.len > 64 or offset > count or indices.len > count - offset or self == parent.store) return error.InvalidState;
        const source = parent.store orelse return error.InvalidState;
        for (0..indices.len) |i| try self.prepare(ref, @intCast(offset + i), allocator);
        self.lock.lock();
        var valid = self.matches(ref, ref.owner);
        for (0..indices.len) |i| {
            const index: u32 = @intCast(offset + i);
            if (self.getLocked(index) != 0 or self.parentIndexLocked(index) != 0) valid = false;
        }
        self.lock.unlock();
        if (!valid) return error.InvalidState;
        var pages: [64]u64 = @splat(0);
        source.lock.lock();
        if (!source.matches(parent, parent.owner)) {
            source.lock.unlock();
            return error.InvalidState;
        }
        // Validate the entire chunk, including multiplicity, before the first
        // increment. A late hole/index/overflow cannot leak an unrecorded ref.
        for (indices, 0..) |index, i| {
            if (index == std.math.maxInt(u64) and sparse) continue;
            if (index >= parent_count) {
                source.lock.unlock();
                return error.InvalidState;
            }
            const ix: u32 = @intCast(index);
            const pa = source.getLocked(ix);
            if (pa == 0 or pa & 4095 != 0) {
                source.lock.unlock();
                return error.InvalidState;
            }
            var multiplicity: u32 = 0;
            for (indices) |other| if (other == index) {
                multiplicity += 1;
            };
            if (@as(u32, source.viewRefsLocked(ix)) + multiplicity > std.math.maxInt(u16)) {
                source.lock.unlock();
                return error.OutOfMemory;
            }
            pages[i] = pa;
        }
        for (indices, pages[0..indices.len]) |index, pa| {
            if (pa == 0) continue;
            const leaf = Directory.get(source.root, @as(u32, @intCast(index)) >> 8, levels).?;
            leaf.view_refs[index & 255] += 1;
        }
        source.lock.unlock();
        self.lock.lock();
        defer self.lock.unlock();
        std.debug.assert(self.matches(ref, ref.owner));
        for (indices, pages[0..indices.len], 0..) |index, pa, i| {
            if (pa == 0) continue;
            const dest: u32 = @intCast(offset + i);
            self.putLocked(dest, pa);
            Directory.get(self.root, dest >> 8, levels).?.parent_index[dest & 255] = @intCast(index + 1);
        }
    }
    /// The dying view still retains its parent, but has no readers. Snapshot
    /// one chunk under its own lock before taking the parent lock, so no two
    /// stores are held together and every committed reference is dropped once.
    pub fn releaseViewRefs(self: *Store, ref: Ref, parent: Ref, count: u32) void {
        const source = parent.store orelse @panic("view parent missing");
        var next: usize = 0;
        var pages: [64]BackedPage = undefined;
        var indices: [64]u32 = undefined;
        while (next < count) {
            self.lock.lock();
            std.debug.assert(self.matches(ref, ref.owner));
            const n = self.snapshotLocked(next, count, &pages);
            for (pages[0..n], 0..) |page, i| {
                indices[i] = self.parentIndexLocked(page.index);
                std.debug.assert(indices[i] != 0);
            }
            self.lock.unlock();
            if (n == 0) break;
            source.lock.lock();
            std.debug.assert(source.matches(parent, parent.owner));
            for (indices[0..n], pages[0..n]) |encoded, page| {
                const index = encoded - 1;
                const leaf = Directory.get(source.root, index >> 8, levels) orelse @panic("view parent leaf missing");
                if (leaf.pages[index & 255] != page.paddr or leaf.view_refs[index & 255] == 0) @panic("view reference mismatch");
                leaf.view_refs[index & 255] -= 1;
            }
            source.lock.unlock();
            next = @as(usize, pages[n - 1].index) + 1;
        }
    }
    pub fn markDirtyLocked(self: *Store, index: u32) bool {
        const leaf = Directory.get(self.root, index >> 8, levels) orelse return false;
        leaf.dirty[(index & 255) >> 6] |= @as(u64, 1) << @intCast(index & 63);
        return true;
    }

    pub fn dirtyWordLocked(self: *Store, word: u32) u64 {
        const leaf = Directory.get(self.root, word >> 2, levels) orelse return 0;
        return leaf.dirty[word & 3];
    }

    pub fn clearDirtyPageLocked(self: *Store, index: u32) void {
        const leaf = Directory.get(self.root, index >> 8, levels) orelse return;
        leaf.dirty[(index & 255) >> 6] &= ~(@as(u64, 1) << @as(u6, @intCast(index & 63)));
    }

    pub fn clearDirtyWordLocked(self: *Store, word: u32) void {
        const leaf = Directory.get(self.root, word >> 2, levels) orelse return;
        leaf.dirty[word & 3] = 0;
    }

    fn visit(self: *Store) void {
        if (builtin.is_test) self.visits += 1;
    }
    fn scan(self: *Store, node: *Node, depth: usize, prefix: u64, first: u64, end: u64, out: []BackedPage, count: *usize) void {
        self.visit();
        if (depth == levels) {
            const leaf = node.block orelse return;
            if (leaf.used == 0) return;
            const base = prefix << 8;
            var index = @max(base, first);
            const stop = @min(base + 256, end);
            while (index < stop and count.* < out.len) : (index += 1) {
                self.visit();
                const pa = leaf.pages[@intCast(index - base)];
                if (pa == 0) continue;
                out[count.*] = .{ .index = @intCast(index), .paddr = pa };
                count.* += 1;
            }
            return;
        }
        const shift: u6 = @intCast((levels - depth - 1) * 6 + 8);
        for (node.children, 0..) |child, digit| {
            const next = child orelse continue;
            const next_prefix = (prefix << 6) | digit;
            const base = next_prefix << shift;
            if (base >= end or base + (@as(u64, 1) << shift) <= first) continue;
            self.scan(next, depth + 1, next_prefix, first, end, out, count);
            if (count.* == out.len) return;
        }
    }
    pub fn snapshotLocked(self: *Store, first: usize, end: usize, out: []BackedPage) usize {
        var count: usize = 0;
        if (self.root) |root| self.scan(root, 0, 0, first, end, out, &count);
        return count;
    }
    fn retire(list: *?*Page, ptr: anytype) void {
        const p = header(ptr);
        p.next = list.*;
        list.* = p;
    }
    /// Detach an empty path under the lock; all readers return scalars and have
    /// ended before these grants are returned outside it.
    fn prune(node_link: *?*Node, index: u32, depth: usize, list: *?*Page) void {
        const node = node_link.* orelse return;
        if (depth == levels) {
            if (node.block) |leaf| {
                if (leaf.used != 0) return;
                retire(list, leaf);
                node.block = null;
            }
        } else {
            const shift: u5 = @intCast((levels - depth - 1) * 6);
            prune(&node.children[((index >> 8) >> shift) & 63], index, depth + 1, list);
            for (node.children) |child| if (child != null) return;
        }
        retire(list, node);
        node_link.* = null;
    }
    pub fn pruneEmpty(self: *Store, ref: Ref, index: u32) void {
        var list: ?*Page = null;
        self.lock.lock();
        if (self.matches(ref, ref.owner)) prune(&self.root, index, 0, &list);
        self.lock.unlock();
        while (list) |p| {
            list = p.next;
            release(p);
        }
    }
    pub fn pruneEmptyDeferred(self: *Store, ref: Ref, index: u32) void {
        var list: ?*Page = null;
        self.lock.lock();
        if (self.matches(ref, ref.owner)) prune(&self.root, index, 0, &list);
        self.lock.unlock();
        // No callback under the caller's outer VM transaction. Capture next
        // before publication, after which the Page belongs solely to queue.
        while (list) |p| {
            const next = p.next;
            enqueueDeferred(p);
            list = next;
        }
    }
    fn destroyNode(self: *Store, node: *Node, depth: usize) void {
        self.visit();
        if (depth == levels) {
            if (node.block) |leaf| release(header(leaf));
        } else for (node.children) |child| if (child) |next| self.destroyNode(next, depth + 1);
        release(header(node));
    }
    pub fn clear(self: *Store, ref: Ref) bool {
        self.lock.lock();
        if (!self.matches(ref, ref.owner)) {
            self.lock.unlock();
            return false;
        }
        const root = self.root;
        self.root = null;
        _ = @atomicRmw(u64, &used_pages, .Sub, self.used, .monotonic);
        self.used = 0;
        self.owner = 0;
        self.lock.unlock();
        if (root) |node| self.destroyNode(node, 0);
        return true;
    }
};

const TestAllocator = struct {
    mutex: u8 = 0,
    calls: usize = 0,
    fail_at: usize = 0,
    live: usize = 0,
    peak: usize = 0,
    largest: usize = 0,
    reject: bool = false,
    checked_store: ?*Store = null,
    fn interface(self: *@This()) storage.Allocator {
        return .{ .context = self, .acquire = take, .release = give };
    }
    fn take(ctx: *anyopaque, pages: usize) ?storage.MappedExtent {
        const self: *@This() = @ptrCast(@alignCast(ctx));
        while (@cmpxchgWeak(u8, &self.mutex, 0, 1, .acquire, .monotonic) != null) std.atomic.spinLoopHint();
        defer @atomicStore(u8, &self.mutex, 0, .release);
        if (self.checked_store) |store| std.debug.assert(@atomicLoad(u8, &store.lock.byte.value, .monotonic) == 0);
        self.calls += 1;
        if (self.calls == self.fail_at) return null;
        const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), pages * 4096) catch return null;
        self.live += pages;
        self.peak = @max(self.peak, self.live);
        self.largest = @max(self.largest, pages);
        return .{ .allocation = .{ .physical = .fromInt(@intFromPtr(bytes.ptr)), .pages = pages, .region_id = 73 }, .bytes = bytes };
    }
    fn give(ctx: *anyopaque, grant: storage.PhysicalExtent) bool {
        const self: *@This() = @ptrCast(@alignCast(ctx));
        while (@cmpxchgWeak(u8, &self.mutex, 0, 1, .acquire, .monotonic) != null) std.atomic.spinLoopHint();
        defer @atomicStore(u8, &self.mutex, 0, .release);
        if (self.checked_store) |store| std.debug.assert(@atomicLoad(u8, &store.lock.byte.value, .monotonic) == 0);
        if (self.reject) return false;
        std.debug.assert(grant.region_id == 73);
        const ptr: [*]align(4096) u8 = @ptrFromInt(grant.physical.toInt());
        std.testing.allocator.free(ptr[0 .. grant.pages * 4096]);
        self.live -= grant.pages;
        return true;
    }
};

test "VMO radix every missing-path allocation failure rolls back empty branches" {
    for (1..7) |failure| {
        var allocator = TestAllocator{ .fail_at = failure };
        var store = Store{};
        const ref = store.init(9);
        allocator.checked_store = &store;
        try std.testing.expectError(error.OutOfMemory, store.prepare(ref, 0xffff_fffe, allocator.interface()));
        try std.testing.expect(store.root == null);
        try std.testing.expectEqual(@as(usize, 0), allocator.live);
        allocator.fail_at = 0;
        try store.prepare(ref, 0xffff_fffe, allocator.interface());
        store.lock.lock();
        store.putLocked(0xffff_fffe, 4096);
        store.lock.unlock();
        try std.testing.expect(store.clear(ref));
        try std.testing.expectEqual(@as(usize, 0), allocator.live);
        try std.testing.expectEqual(@as(usize, 1), allocator.largest);
    }
}

test "VMO radix owner reuse rejects stale refs and isolated enumeration ignores other owners" {
    var allocator = TestAllocator{};
    var store = Store{};
    const ref = store.init(11);
    for ([_]u32{ 0, 131072, 0xffff_fffe }) |i| {
        try store.prepare(ref, i, allocator.interface());
        store.lock.lock();
        store.putLocked(i, (@as(u64, i) + 1) * 4096);
        store.lock.unlock();
    }
    var pages: [3]BackedPage = undefined;
    store.visits = 0;
    try std.testing.expectEqual(@as(usize, 3), store.snapshotLocked(0, 0xffff_ffff, &pages));
    const visits = store.visits;
    std.debug.print("VMO radix target-only visits={d} unrelated-owners=0/128\n", .{visits});
    var other: [128]Store = [_]Store{.{}} ** 128;
    for (&other, 0..) |*s, i| {
        const rr = s.init(i + 100);
        try s.prepare(rr, @intCast(i * 65536), allocator.interface());
        s.putLocked(@intCast(i * 65536), 4096);
    }
    store.visits = 0;
    try std.testing.expectEqual(@as(usize, 3), store.snapshotLocked(0, 0xffff_ffff, &pages));
    try std.testing.expectEqual(visits, store.visits);
    for (&other) |*s| try std.testing.expect(s.clear(.{ .store = s, .owner = s.owner }));
    for (pages) |p| {
        store.putLocked(p.index, 0);
        store.pruneEmpty(ref, p.index);
    }
    try std.testing.expect(store.root == null);
    try std.testing.expect(store.clear(ref));
    const next = store.init(12);
    try std.testing.expect(!store.clear(ref));
    try std.testing.expect(store.clear(next));
    try std.testing.expectEqual(@as(usize, 0), allocator.live);
}

test "VMO radix rejected metadata returns preserve both issuers after owner destruction" {
    var first = TestAllocator{};
    var second = TestAllocator{};
    var a = Store{};
    var b = Store{};
    const ar = a.init(3);
    const br = b.init(4);
    try a.prepare(ar, 1, first.interface());
    try b.prepare(br, 1, second.interface());
    first.reject = true;
    second.reject = true;
    try std.testing.expect(a.clear(ar));
    try std.testing.expect(b.clear(br));
    try std.testing.expectEqual(@as(usize, 12), stats().pending);
    reclaim();
    try std.testing.expectEqual(@as(usize, 12), stats().pending);
    first.reject = false;
    reclaim();
    try std.testing.expectEqual(@as(usize, 0), first.live);
    try std.testing.expectEqual(@as(usize, 6), second.live);
    second.reject = false;
    reclaim();
    try std.testing.expectEqual(@as(usize, 0), second.live);
    try std.testing.expectEqual(@as(usize, 0), stats().pending);
}

test "VMO radix exceeds former global capacity without contiguous growth" {
    var allocator = TestAllocator{};
    var store = Store{};
    const ref = store.init(27);
    const count = 1048577;
    for (0..count) |i| {
        if (i % 256 == 0) try store.prepare(ref, @intCast(i), allocator.interface());
        store.putLocked(@intCast(i), (@as(u64, i) + 1) * 4096);
    }
    try std.testing.expectEqual(@as(u64, count), store.used);
    try std.testing.expectEqual(@as(usize, 1), allocator.largest);
    var out: [64]BackedPage = undefined;
    var cursor: usize = 0;
    var seen: usize = 0;
    while (cursor < count) {
        const n = store.snapshotLocked(cursor, count, &out);
        try std.testing.expect(n != 0);
        for (out[0..n]) |page| {
            try std.testing.expectEqual(seen, page.index);
            seen += 1;
        }
        cursor = @as(usize, out[n - 1].index) + 1;
    }
    try std.testing.expectEqual(@as(usize, count), seen);
    std.debug.print("VMO radix pages={d} metadata-peak-bytes={d} largest-request-bytes={d}; old-first-growth-request-bytes={d} old-overlap-bytes={d} single-path-reserve-max-bytes=24576\n", .{ count, allocator.peak * 4096, allocator.largest * 4096, ((56 + 2097152 * 20 + 4095) / 4096) * 4096, 1048576 * 20 + ((56 + 2097152 * 20 + 4095) / 4096) * 4096 });
    try std.testing.expect(store.clear(ref));
    try std.testing.expectEqual(@as(usize, 0), allocator.live);
}

test "VMO radix differential operations match old open addressing hash" {
    const Hash = @import("vmo_backing_reference_test.zig").Hash;
    var old = Hash{};
    var allocator = TestAllocator{};
    var stores: [4]Store = [_]Store{.{}} ** 4;
    var refs: [4]Ref = undefined;
    for (&stores, 0..) |*s, i| refs[i] = s.init(i + 1);
    defer for (&stores, refs) |*s, ref| {
        std.debug.assert(s.clear(ref));
    };
    var random: u64 = 0x123456789;
    for (0..12000) |_| {
        random = random *% 6364136223846793005 +% 1;
        const owner: usize = @intCast((random >> 32) & 3);
        const index: u32 = @as(u32, @intCast((random >> 40) & 255)) * 65537;
        const pa: u64 = if (random & 8 == 0) 0 else ((random >> 20) & 0xfffff) * 4096 + 4096;
        const ref = refs[owner];
        const store = &stores[owner];
        if (pa == 0) {
            if (old.findVmoBackingPageStoreSlotLocked(owner + 1, index)) |slot| old.removeVmoBackingPageStoreSlotLocked(slot);
            if (store.hasPathLocked(index)) store.putLocked(index, 0);
            store.pruneEmpty(ref, index);
        } else {
            _ = try old.insertVmoBackingPageStoreSlotLocked(owner + 1, index, pa);
            try store.prepare(ref, index, allocator.interface());
            store.putLocked(index, pa);
        }
        const expected = if (old.findVmoBackingPageStoreSlotLocked(owner + 1, index)) |slot| old.paddrs[slot] else 0;
        try std.testing.expectEqual(expected, store.getLocked(index));
    }
    for (&stores, 0..) |*store, owner| {
        var out: [256]BackedPage = undefined;
        const n = store.snapshotLocked(0, 0xffffffff, &out);
        var expected: usize = 0;
        for (old.owners) |o| {
            if (o == owner + 1) expected += 1;
        }
        try std.testing.expectEqual(expected, n);
        for (out[0..n], 0..) |page, i| {
            const slot = old.findVmoBackingPageStoreSlotLocked(owner + 1, page.index).?;
            try std.testing.expectEqual(old.paddrs[slot], page.paddr);
            if (i != 0) try std.testing.expect(out[i - 1].index < page.index);
        }
    }
}

test "VMO radix independent owner lock permits unrelated access and serializes same owner" {
    var a = Store{};
    var b = Store{};
    const ar = a.init(1);
    const br = b.init(2);
    var allocator = TestAllocator{};
    try a.prepare(ar, 0, allocator.interface());
    try b.prepare(br, 0, allocator.interface());
    a.putLocked(0, 4096);
    b.putLocked(0, 8192);
    const Context = struct {
        a: *Store,
        b: *Store,
        started: u8 = 0,
        done: u8 = 0,
        value: u64 = 0,
        fn run(c: *@This()) void {
            c.b.lock.lock();
            c.value = c.b.getLocked(0);
            c.b.lock.unlock();
            @atomicStore(u8, &c.started, 1, .release);
            c.a.lock.lock();
            c.value += c.a.getLocked(0);
            c.a.lock.unlock();
            @atomicStore(u8, &c.done, 1, .release);
        }
    };
    var c = Context{ .a = &a, .b = &b };
    a.lock.lock();
    const thread = std.Thread.spawn(.{}, Context.run, .{&c}) catch |err| {
        a.lock.unlock();
        return err;
    };
    while (@atomicLoad(u8, &c.started, .acquire) == 0) std.Thread.yield() catch {};
    const done = @atomicLoad(u8, &c.done, .acquire);
    a.lock.unlock();
    thread.join();
    try std.testing.expectEqual(@as(u8, 0), done);
    try std.testing.expectEqual(@as(u64, 12288), c.value);
    try std.testing.expect(a.clear(ar));
    try std.testing.expect(b.clear(br));
    try std.testing.expectEqual(@as(usize, 0), allocator.live);
}

test "VMO radix isolated teardown visits ignore unrelated owners" {
    var allocator = TestAllocator{};
    var stores: [130]Store = [_]Store{.{}} ** 130;
    for (stores[0..2], 0..) |*s, i| {
        const ref = s.init(i + 1);
        for ([_]u32{ 0, 131072, 0xffff_fffe }) |index| {
            try s.prepare(ref, index, allocator.interface());
            s.putLocked(index, 4096);
        }
    }
    try std.testing.expect(stores[0].clear(.{ .store = &stores[0], .owner = 1 }));
    const before = stores[0].visits;
    for (stores[2..], 0..) |*s, i| {
        const ref = s.init(i + 3);
        try s.prepare(ref, @intCast(i * 65536), allocator.interface());
        s.putLocked(@intCast(i * 65536), 4096);
    }
    try std.testing.expect(stores[1].clear(.{ .store = &stores[1], .owner = 2 }));
    try std.testing.expectEqual(before, stores[1].visits);
    std.debug.print("VMO radix teardown visits={d} unrelated-owners=0/128\n", .{before});
    for (stores[2..]) |*s| try std.testing.expect(s.clear(.{ .store = s, .owner = s.owner }));
    try std.testing.expectEqual(@as(usize, 0), allocator.live);
}

test "VMO radix fault publication revalidates winner and owner after unlocked parent resolution" {
    const Context = struct {
        store: *Store,
        ref: Ref,
        allocator: storage.Allocator,
        reuse: bool,
        fn parent(c: @This()) Error!u64 {
            std.debug.assert(c.store.lock.byte.value == 0);
            if (c.reuse) {
                std.debug.assert(c.store.clear(c.ref));
                _ = c.store.init(2);
            } else {
                try c.store.prepare(c.ref, 0, c.allocator);
                c.store.lock.lock();
                c.store.putLocked(0, 8192);
                c.store.lock.unlock();
            }
            return 0;
        }
    };
    for ([_]bool{ false, true }) |reuse| {
        var allocator = TestAllocator{};
        var store = Store{};
        allocator.checked_store = &store;
        const ref = store.init(1);
        const context = Context{ .store = &store, .ref = ref, .allocator = allocator.interface(), .reuse = reuse };
        const result = store.resolveOrInsert(ref, 0, 4096, allocator.interface(), true, context, Context.parent);
        if (reuse) {
            try std.testing.expectError(error.InvalidState, result);
            try std.testing.expect(store.root == null);
        } else {
            try std.testing.expectEqual(@as(u64, 8192), try result);
            try std.testing.expectEqual(@as(u64, 1), store.used);
            try std.testing.expectEqual(@as(usize, 6), allocator.live);
        }
        try std.testing.expect(store.clear(.{ .store = &store, .owner = store.owner }));
        try std.testing.expectEqual(@as(usize, 0), allocator.live);
    }
}

test "VMO radix concurrent path growth keeps every key and returns losing drafts" {
    var allocator = TestAllocator{};
    var store = Store{};
    const ref = store.init(91);
    const Context = struct {
        store: *Store,
        ref: Ref,
        allocator: storage.Allocator,
        first: u32,
        fn run(c: @This()) void {
            for (0..1024) |i| {
                const index: u32 = c.first + @as(u32, @intCast(i)) * 4;
                c.store.lock.lock();
                c.store.ensureLocked(c.ref, index, c.allocator) catch @panic("test allocator exhausted");
                c.store.putLocked(index, (@as(u64, index) + 1) * 4096);
                c.store.lock.unlock();
            }
        }
    };
    var threads: [4]std.Thread = undefined;
    for (&threads, 0..) |*thread, i| thread.* = try std.Thread.spawn(.{}, Context.run, .{Context{ .store = &store, .ref = ref, .allocator = allocator.interface(), .first = @intCast(i) }});
    for (threads) |thread| thread.join();
    try std.testing.expectEqual(@as(u64, 4096), store.used);
    for (0..4096) |i| try std.testing.expectEqual(@as(u64, (i + 1) * 4096), store.getLocked(@intCast(i)));
    try std.testing.expect(store.clear(ref));
    try std.testing.expectEqual(@as(usize, 0), allocator.live);
}

test "shared discard view install all or nothing duplicate sparse and late failure" {
    var allocator: storage.TestAllocator = .{};
    defer allocator.deinit();
    var parent: Store = .{};
    const root_ref = parent.init(11);
    var view: Store = .{};
    const view_ref = view.init(12);
    defer { _ = view.clear(view_ref); _ = parent.clear(root_ref); reclaim(); }
    for ([_]u32{ 0, 256, 512 }) |index| {
        try parent.prepare(root_ref, index, allocator.interface());
        parent.putLocked(index, (@as(u64, index) + 1) * 4096);
    }
    try view.installView(view_ref, 8, 0, root_ref, 513, &.{ 0, 256, 0, std.math.maxInt(u64) }, true, allocator.interface());
    // Independent expectation comes from recorded view indices AND PA, never
    // from the helper's increments. Index zero is encoded as one.
    for ([_]u32{ 0, 256, 512 }) |index| {
        var expected: u16 = 0;
        for (0..8) |i| {
            const encoded = view.parentIndexLocked(@intCast(i));
            if (encoded == 0) { try std.testing.expectEqual(@as(u64, 0), view.getLocked(@intCast(i))); continue; }
            try std.testing.expectEqual(parent.getLocked(encoded - 1), view.getLocked(@intCast(i)));
            if (encoded - 1 == index) expected += 1;
        }
        try std.testing.expectEqual(expected, parent.viewRefsLocked(index));
    }
    try std.testing.expectError(error.InvalidState, view.installView(view_ref, 8, 4, root_ref, 513, &.{ 512, 511 }, false, allocator.interface()));
    try std.testing.expectEqual(@as(u16, 0), parent.viewRefsLocked(512));
    try std.testing.expectError(error.InvalidState, view.installView(view_ref, 8, 0, root_ref, 513, &.{512}, false, allocator.interface()));
    try std.testing.expectEqual(@as(u16, 0), parent.viewRefsLocked(512));
    const leaf = Directory.get(parent.root, 2, levels).?;
    leaf.view_refs[0] = std.math.maxInt(u16) - 1;
    try std.testing.expectError(error.OutOfMemory, view.installView(view_ref, 8, 4, root_ref, 513, &.{ 512, 512 }, false, allocator.interface()));
    try std.testing.expectEqual(std.math.maxInt(u16) - 1, leaf.view_refs[0]);
    leaf.view_refs[0] = 0;
    view.releaseViewRefs(view_ref, root_ref, 8);
    try std.testing.expectEqual(@as(u16, 0), parent.viewRefsLocked(0));
    try std.testing.expectEqual(@as(u16, 0), parent.viewRefsLocked(256));
}

test "shared discard deferred rejects outside lock and survives forbidden one level reentry" {
    var issuer: storage.TestAllocator = .{};
    defer issuer.deinit();
    const Probe = struct {
        var reenter = true;
        fn release(context: *anyopaque, grant: storage.PhysicalExtent) bool {
            lock_diagnostics.assertRootEmpty();
            if (reenter) { reenter = false; reclaimDeferred(); }
            const allocator: *storage.TestAllocator = @ptrCast(@alignCast(context));
            return allocator.interface().release(context, grant);
        }
    };
    Probe.reenter = true;
    var allocator = issuer.interface();
    allocator.release = Probe.release;
    var store: Store = .{};
    const ref = store.init(99);
    try store.prepare(ref, 0, allocator);
    store.putLocked(0, 4096);
    store.putLocked(0, 0);
    store.pruneEmptyDeferred(ref, 0);
    try std.testing.expect(hasDeferred());
    issuer.fail_return = true;
    reclaim(); // Existing pending drain cannot reach the deferred domain.
    try std.testing.expectEqual(@as(usize, 0), issuer.returns);
    reclaimDeferred();
    try std.testing.expect(hasDeferred());
    _ = store.clear(ref);
    issuer.fail_return = false;
    reclaimDeferred();
    try std.testing.expect(!hasDeferred());
    try std.testing.expectEqual(@as(usize, 0), issuer.active());
}

test "shared discard concurrent deferred enqueue and finite snapshot reclaim" {
    const Context = struct {
        issuer: storage.TestAllocator = .{},
        gate: bool = false,
        fn acquire(context: *anyopaque, pages: usize) ?storage.MappedExtent {
            const self: *@This() = @ptrCast(@alignCast(context));
            const allocator = self.issuer.interface();
            return allocator.acquire(allocator.context, pages);
        }
        fn release(context: *anyopaque, grant: storage.PhysicalExtent) bool {
            lock_diagnostics.assertRootEmpty();
            const self: *@This() = @ptrCast(@alignCast(context));
            while (@cmpxchgWeak(bool, &self.gate, false, true, .acquire, .monotonic) != null) std.atomic.spinLoopHint();
            defer @atomicStore(bool, &self.gate, false, .release);
            const allocator = self.issuer.interface();
            return allocator.release(allocator.context, grant);
        }
    };
    const Worker = struct {
        store: *Store,
        ref: Ref,
        start: *bool,
        fn run(self: *@This()) void {
            const root = @import("root");
            if (@hasDecl(root, "setTestLockDiagnosticsCpu")) root.setTestLockDiagnosticsCpu(1);
            while (!@atomicLoad(bool, self.start, .acquire)) std.atomic.spinLoopHint();
            self.store.pruneEmptyDeferred(self.ref, 0);
            reclaimDeferred();
        }
    };
    var context: Context = .{};defer context.issuer.deinit();
    const allocator = storage.Allocator{ .context = &context, .acquire = Context.acquire, .release = Context.release };
    var stores = [_]Store{ .{}, .{} };
    var refs: [2]Ref = undefined;
    for (&stores, 0..) |*store, i| {
        refs[i] = store.init(@intCast(i + 1));
        try store.prepare(refs[i], 0, allocator);
        store.putLocked(0, 4096);store.putLocked(0, 0);
    }
    var start = false;
    var worker = Worker{ .store = &stores[1], .ref = refs[1], .start = &start };
    const thread = try std.Thread.spawn(.{}, Worker.run, .{&worker});
    @atomicStore(bool, &start, true, .release);
    stores[0].pruneEmptyDeferred(refs[0], 0);
    reclaimDeferred();
    thread.join();
    reclaimDeferred();
    try std.testing.expect(!hasDeferred());
    try std.testing.expectEqual(@as(usize, 0), context.issuer.active());
}


test "shared discard releaseGrant unregisters exact issuer with equal synthetic PA tokens" {
    var first: storage.TestAllocator = .{};defer first.deinit();
    var second: storage.TestAllocator = .{};defer second.deinit();
    var a: Store = .{};
    var b: Store = .{};
    const ar = a.init(1);
    const br = b.init(2);
    try a.prepare(ar, 0, first.interface());
    try b.prepare(br, 0, second.interface());
    // Each issuer deliberately starts its token sequence at the same PA.
    // Releasing a must leave b's registry nodes intact, and no freed a node.
    _ = a.clear(ar);
    var cursor = test_pages;
    var count: usize = 0;
    while (cursor) |page| : (cursor = page.test_next) {
        try std.testing.expect(page.allocator.context != @as(*anyopaque, &first));
        if (page.allocator.context == @as(*anyopaque, &second)) count += 1;
    }
    try std.testing.expectEqual(second.active(), count);
    _ = b.clear(br);
    try std.testing.expectEqual(@as(usize, 0), first.active());
    try std.testing.expectEqual(@as(usize, 0), second.active());
}
