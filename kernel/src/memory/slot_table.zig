//! Stable resource bodies and replaceable, page-sized index directories.
//! Writers are serialized by the caller (K or futex). Directory readers use
//! Leaf when they do not share that writer exclusion. A returned body pointer
//! is a loan protected by the resource exclusion or an explicit read-side
//! domain, NOT by the directory leaf after it has been released.
const std = @import("std");
const builtin = @import("builtin");
const storage = @import("owned_storage.zig");
const diagnostics = @import("../sync/lock_diagnostics.zig");

pub const Leaf = struct {
    byte: diagnostics.Byte = .{},

    pub fn lock(self: *Leaf) bool {
        var flags: u64 = 0;
        if (!builtin.is_test) asm volatile ("pushfq; pop %[flags]; cli"
            : [flags] "=r" (flags),
            :
            : .{ .memory = true });
        diagnostics.beforeAcquire(&self.byte.record, .{ .class = .slot_directory });
        while (@cmpxchgWeak(u8, &self.byte.value, 0, 1, .acquire, .monotonic) != null)
            std.atomic.spinLoopHint();
        diagnostics.acquired(&self.byte.record, .{ .class = .slot_directory });
        return flags & (1 << 9) != 0;
    }

    pub fn unlock(self: *Leaf, enable_interrupts: bool) void {
        diagnostics.releasing(&self.byte.record);
        @atomicStore(u8, &self.byte.value, 0, .release);
        if (!builtin.is_test and enable_interrupts) asm volatile ("sti" ::: .{ .memory = true });
    }
};

pub const Ref = struct {
    index: u32,
    generation: u32,
};

pub const Error = storage.Error || error{ IndexExhausted, NeedStorage, StalePreparation };
pub var test_directory_copy_bytes: if (builtin.is_test) usize else void = if (builtin.is_test) 0 else {};
pub var test_mark_search_nodes: if (builtin.is_test) usize else void = if (builtin.is_test) 0 else {};

const none: u64 = std.math.maxInt(u64);

pub fn SlotTable(comptime T: type) type {
    return struct {
        const Self = @This();
        // Small bodies share a page grant; large IPC queue bodies require
        // only their own minimum extent. Neither grows with table capacity.
        pub const block_slots = std.math.floorPowerOfTwo(usize, @max(1, 3072 / @max(@sizeOf(T), 32)));
        const block_shift = std.math.log2_int(usize, block_slots);
        const levels = (32 - block_shift + 5) / 6;
        const Body = struct { index: u32, value: T };
        const Entry = struct {
            generation: u32 = 1,
            used: bool = false,
            withdrawn: bool = false,
            marks: u8 = 0,
            exhausted: bool = false,
            next: u64 = none,
        };
        const Block = struct {
            entries: [block_slots]Entry = [_]Entry{.{}} ** block_slots,
            values: ?[*]Body = null,
            live: usize = 0,
        };
        const Directory = @import("radix.zig").Directory(Block);
        const Node = Directory.Node;
        pub const max_path_copy_bytes: usize = @as(usize, levels + 1) * @sizeOf(Node);
        pub const max_chunk_allocations = levels + 3;
        fn initialValue() T {
            return switch (@typeInfo(T)) {
                .@"struct" => .{},
                else => std.mem.zeroes(T),
            };
        }
        pub const Allocation = struct { ref: Ref, value: *T };

        return_queue: ?*storage.RetiredQueue = null,
        root: ?*Node = null,
        capacity: u64 = 0,
        live: usize = 0,
        free_head: u64 = none,
        directory_owner: storage.Owner = .{},
        metadata_owner: storage.Owner = .{},
        body_owner: storage.Owner = .{},

        pub const Prepared = struct {
            directory: storage.Transaction,
            metadata: storage.Transaction,
            body: storage.Transaction,
            nodes: [levels + 1]*Node = undefined,
            block: ?*Block = null,
            values: ?[*]Body = null,

            pub fn init(table: *Self, allocator: storage.Allocator) Prepared {
                return .{
                    .directory = storage.Transaction.init(&table.directory_owner, allocator),
                    .metadata = storage.Transaction.init(&table.metadata_owner, allocator),
                    .body = storage.Transaction.init(&table.body_owner, allocator),
                };
            }

            /// May run outside writer exclusion: draft memory is private;
            /// no published pointer, capacity or free list is read here.
            pub fn reserve(self: *Prepared) Error!void {
                for (&self.nodes) |*node| {
                    const memory = try self.directory.reserve(Node, 1, .stable);
                    memory[0] = .{};
                    node.* = &memory[0];
                }
                const metadata = try self.metadata.reserve(Block, 1, .stable);
                metadata[0] = .{};
                self.block = &metadata[0];
                const bodies = try self.body.reserve(Body, block_slots, .stable);
                for (bodies) |*body| body.* = .{ .index = 0, .value = initialValue() };
                self.values = bodies.ptr;
            }

            /// Writer exclusion required, outside any directory leaf. Even a
            /// failed reservation must come here so rejected returns stay owned.
            pub fn finish(self: *Prepared) void {
                if (!self.directory.finished) self.directory.rollback() catch unreachable;
                if (!self.metadata.finished) self.metadata.rollback() catch unreachable;
                if (!self.body.finished) self.body.rollback() catch unreachable;
            }
        };

        fn blockAt(self: *const Self, index: u32) ?*Block {
            if (@as(u64, index) >= self.capacity) return null;
            return Directory.get(self.root, index >> block_shift, levels);
        }

        /// Caller owns writer exclusion or the directory leaf. No directory
        /// pointer escapes. Body lifetime must separately be protected through
        /// the last dereference, including before acquiring a body-level lock.
        pub fn get(self: *const Self, index: u32) ?*T {
            const block = self.blockAt(index) orelse return null;
            const offset = index & (block_slots - 1);
            if (!block.entries[offset].used) return null;
            return &(block.values orelse unreachable)[offset].value;
        }

        pub fn resolve(self: *const Self, ref: Ref) ?*T {
            const block = self.blockAt(ref.index) orelse return null;
            const offset = ref.index & (block_slots - 1);
            if (!block.entries[offset].used or block.entries[offset].generation != ref.generation) return null;
            return &(block.values orelse unreachable)[offset].value;
        }

        pub fn lookup(self: *const Self, index: u32, leaf: *Leaf) ?*T {
            const flags = leaf.lock();
            defer leaf.unlock(flags);
            return self.get(index);
        }
        pub fn mark(self: *Self, index: u32, mask: u8, set: bool) void {
            const entry = &self.blockAt(index).?.entries[index & (block_slots - 1)];
            if (set) entry.marks |= mask else entry.marks &= ~mask;
            if (mask & 1 == 0) return;
            // Bit 1 has a radix summary. Fixed-depth updates/search avoid the
            // linear free-FD scan on a dense 4,200-descriptor process.
            var path: [levels + 1]*Node = undefined;
            var digits: [levels]u6 = undefined;
            var node = self.root.?;
            const ordinal = index >> block_shift;
            for (0..levels) |depth| {
                path[depth] = node;
                const shift: u5 = @intCast((levels - depth - 1) * 6);
                const digit: u6 = @intCast((ordinal >> shift) & 63);
                digits[depth] = digit;
                node = node.children[digit].?;
            }
            path[levels] = node;
            const bit = @as(u64, 1) << @as(u6, @intCast(index & (block_slots - 1)));
            if (set) node.marked |= bit else node.marked &= ~bit;
            var depth: usize = levels;
            while (depth != 0) {
                depth -= 1;
                const child_bit = @as(u64, 1) << digits[depth];
                if (path[depth + 1].marked != 0) path[depth].marked |= child_bit else path[depth].marked &= ~child_bit;
            }
        }

        pub fn firstMarkedAtOrAfter(self: *const Self, minimum: u32) ?u32 {
            return searchMarked(self.root orelse return null, levels, 0, minimum);
        }
        fn searchMarked(node: *const Node, depth: usize, prefix: u64, minimum: u32) ?u32 {
            if (builtin.is_test) test_mark_search_nodes += 1;
            if (depth == 0) {
                const base = prefix << block_shift;
                if (base + block_slots <= minimum) return null;
                const offset: u6 = @intCast(if (minimum > base) minimum - base else 0);
                const bits = node.marked & (@as(u64, std.math.maxInt(u64)) << offset);
                return if (bits == 0) null else @intCast(base + @ctz(bits));
            }
            var bits = node.marked;
            while (bits != 0) {
                const digit: u6 = @intCast(@ctz(bits));
                bits &= bits - 1;
                const child_prefix = (prefix << 6) | digit;
                const shift: u6 = @intCast((depth - 1) * 6 + block_shift);
                if ((child_prefix + 1) << shift <= minimum) continue;
                if (searchMarked(node.children[digit].?, depth - 1, child_prefix, minimum)) |index| return index;
            }
            return null;
        }
        pub fn isMarked(self: *const Self, index: u32, mask: u8) bool {
            const block = self.blockAt(index) orelse return false;
            return block.entries[index & (block_slots - 1)].marks & mask != 0;
        }
        pub const MarkIterator = struct {
            base: Iterator,
            mask: u8,
            pub fn next(self: *MarkIterator) ?u32 {
                while (self.base.next()) |item| if (self.base.table.isMarked(item.ref.index, self.mask)) return item.ref.index;
                return null;
            }
        };
        pub fn marked(self: *const Self, mask: u8) MarkIterator {
            return .{ .base = self.iterator(), .mask = mask };
        }

        pub fn isExhausted(self: *const Self, index: u32) bool {
            const block = self.blockAt(index) orelse return false;
            return block.entries[index & (block_slots - 1)].exhausted;
        }

        pub fn generation(self: *const Self, index: u32) ?u32 {
            const block = self.blockAt(index) orelse return null;
            return block.entries[index & (block_slots - 1)].generation;
        }

        /// Fast allocation never calls an allocator. NeedStorage means either
        /// a new index block or a previously returned body grant is required.
        pub fn hasPreparedFree(self: *const Self) bool {
            if (self.free_head == none) return false;
            return self.blockAt(@intCast(self.free_head)).?.values != null;
        }

        /// Count only the allocation-ready prefix: exhausted generations and
        /// reclaimed bodies cannot authorize an infallible multi-slot commit.
        pub fn preparedFreeCount(self: *const Self, limit: usize) usize {
            var index = self.free_head;
            var count: usize = 0;
            while (index != none and count < limit) {
                const block = self.blockAt(@intCast(index)).?;
                if (block.values == null) break;
                const entry = &block.entries[@as(u32, @intCast(index)) & (block_slots - 1)];
                std.debug.assert(!entry.used and !entry.exhausted);
                count += 1;
                index = entry.next;
            }
            return count;
        }

        pub fn allocatePrepared(self: *Self) Error!Allocation {
            if (self.free_head == none) return error.NeedStorage;
            const index: u32 = @intCast(self.free_head);
            const block = self.blockAt(index).?;
            const values = block.values orelse return error.NeedStorage;
            const offset = index & (block_slots - 1);
            const entry = &block.entries[offset];
            std.debug.assert(!entry.used and !entry.exhausted);
            self.free_head = entry.next;
            entry.marks = 0;
            entry.next = none;
            block.live += 1;
            self.live += 1;
            values[offset] = .{ .index = index, .value = initialValue() };
            if (comptime @typeInfo(T) == .@"struct" and @hasField(T, "generation")) values[offset].value.generation = entry.generation;
            @atomicStore(bool, &entry.used, true, .release);
            return .{ .ref = .{ .index = index, .generation = entry.generation }, .value = &values[offset].value };
        }

        pub const RetiredPath = struct { nodes: [levels + 1]?*Node = .{null} ** (levels + 1) };

        /// Writer exclusion + optional directory leaf. All fallible acquisition
        /// happened in Prepared.reserve. Copy only one bounded directory path;
        /// existing bodies and generation metadata are never copied or moved.
        pub fn commitPrepared(self: *Self, draft: *Prepared, max_index: u32) Error!RetiredPath {
            return self.commitPreparedMode(draft, max_index, false);
        }

        fn commitPreparedMode(self: *Self, draft: *Prepared, max_index: u32, append: bool) Error!RetiredPath {
            if (draft.values == null or draft.block == null) return error.InvalidState;
            if (!append and self.free_head != none) {
                const block = self.blockAt(@intCast(self.free_head)).?;
                if (block.values != null) return error.StalePreparation;
                try draft.body.commitAdditionalStable();
                @atomicStore(?[*]Body, &block.values, draft.values, .release);
                return .{};
            }
            if (self.capacity > max_index or self.capacity + block_slots - 1 > max_index)
                return error.IndexExhausted;
            var retired: RetiredPath = .{};
            const ordinal: u32 = @intCast(self.capacity >> block_shift);
            var old = self.root;
            for (0..levels + 1) |depth| {
                const node = draft.nodes[depth];
                retired.nodes[depth] = old;
                if (old) |previous| {
                    node.* = previous.*;
                    if (builtin.is_test) test_directory_copy_bytes += @sizeOf(Node);
                } else node.* = .{};
                if (depth == levels) {
                    node.block = draft.block;
                } else {
                    const shift: u5 = @intCast((levels - depth - 1) * 6);
                    const digit = (ordinal >> shift) & 63;
                    old = if (old) |previous| previous.children[digit] else null;
                    node.children[digit] = draft.nodes[depth + 1];
                }
            }
            const block = draft.block.?;
            @atomicStore(?[*]Body, &block.values, draft.values, .release);
            for (&block.entries, 0..) |*entry, offset| entry.next = if (offset + 1 == block_slots) self.free_head else self.capacity + offset + 1;
            // The owners and root are published in this same exclusion. No
            // allocator callback or recoverable failure follows the first commit.
            draft.directory.commitAdditionalStable() catch unreachable;
            draft.metadata.commitAdditionalStable() catch unreachable;
            draft.body.commitAdditionalStable() catch unreachable;
            @atomicStore(?*Node, &self.root, draft.nodes[0], .release);
            self.free_head = self.capacity;
            @atomicStore(u64, &self.capacity, self.capacity + block_slots, .release);
            return retired;
        }

        /// After releasing the publication leaf, still under writer exclusion.
        pub fn retirePath(self: *Self, path: RetiredPath) void {
            for (path.nodes) |node| if (node) |old| {
                self.directory_owner.retireAllocation(Node, @as([*]Node, @ptrCast(old))) catch unreachable;
            };
        }

        /// Logical removal requires the caller to have ended every body loan.
        /// Empty bodies are retired, not returned under a leaf. Their index and
        /// generation metadata survives to reject stale capabilities on reuse.
        pub fn release(self: *Self, ref: Ref) Error!void {
            const block = self.blockAt(ref.index) orelse return error.InvalidState;
            const entry = &block.entries[ref.index & (block_slots - 1)];
            if (!entry.used or entry.generation != ref.generation) return error.InvalidState;
            @atomicStore(bool, &entry.used, false, .release);
            if (entry.marks & 1 != 0) self.mark(ref.index, 1, false);
            entry.marks = 0;
            block.live -= 1;
            self.live -= 1;
            if (entry.generation == std.math.maxInt(u32)) {
                entry.exhausted = true;
            } else {
                @atomicStore(u32, &entry.generation, entry.generation + 1, .release);
                entry.next = self.free_head;
                self.free_head = ref.index;
            }
        }

        /// Call only after directory readers AND body borrowers are excluded.
        /// No leaf may be held while allocator callbacks run.
        pub fn reclaimEmptyBodies(self: *Self) void {
            var base: u64 = 0;
            while (base < self.capacity) : (base += block_slots) {
                const block = self.blockAt(@intCast(base)).?;
                if (block.live == 0) if (block.values) |values| {
                    self.body_owner.retireAllocation(Body, values) catch unreachable;
                    @atomicStore(?[*]Body, &block.values, null, .release);
                };
            }
            self.reclaim();
        }

        pub fn reclaim(self: *Self) void {
            for ([_]*storage.Owner{ &self.directory_owner, &self.metadata_owner, &self.body_owner }) |owner| {
                if (owner.bound_address == null and owner.published == null and owner.retired == null and owner.pending == null) continue;
                owner.retryUnpublished() catch unreachable;
                if (self.return_queue) |domain|
                    owner.transferRetiredAfterBorrowExclusion(domain) catch unreachable
                else
                    owner.reclaimRetiredAfterBorrowExclusion() catch unreachable;
            }
        }

        /// Logical withdrawal requires directory exclusion and closed resource
        /// references. Callers exclude all borrowers before deinit; the optional
        /// return_queue preserves rejected grants for PMM pressure retries.
        /// Generation history reset is for whole-owner teardown only.
        pub fn retire(self: *Self) void {
            @atomicStore(?*Node, &self.root, null, .release);
            @atomicStore(u64, &self.capacity, 0, .release);
            self.live = 0;
            self.free_head = none;
            for ([_]*storage.Owner{ &self.directory_owner, &self.metadata_owner, &self.body_owner }) |owner| {
                if (owner.bound_address == null and owner.published == null and owner.retired == null and owner.pending == null) continue;
                owner.retireAll() catch unreachable;
            }
        }

        pub fn deinit(self: *Self) void {
            self.retire();
            self.reclaim();
        }

        /// Dense/specified-index clients (VMA indices, group links) use this
        /// same directory and grant mechanism. Preparation does not publish a
        /// resource; the caller claims the index only after fallible setup.
        pub fn prepareIndex(self: *Self, index: u32, allocator: storage.Allocator) Error!void {
            while (self.capacity <= index) {
                var draft = Prepared.init(self, allocator);
                defer draft.finish();
                try draft.reserve();
                self.retirePath(try self.commitPreparedMode(&draft, std.math.maxInt(u32), true));
                self.reclaim();
            }
            const block = self.blockAt(index).?;
            if (block.entries[index & (block_slots - 1)].exhausted) return error.IndexExhausted;
            if (block.values == null) {
                // Existing generation metadata already names this block. A
                // fresh directory draft would reserve unused pages on every
                // cache-backed wait; reserve only the missing body.
                var body = storage.Transaction.init(&self.body_owner, allocator);
                defer if (!body.finished) body.rollback() catch unreachable;
                const values = try body.reserve(Body, block_slots, .stable);
                for (values) |*value| value.* = .{ .index = 0, .value = initialValue() };
                try body.commitAdditionalStable();
                @atomicStore(?[*]Body, &block.values, values.ptr, .release);
            }
        }

        pub fn claimIndex(self: *Self, index: u32) Error!*T {
            const block = self.blockAt(index) orelse return error.NeedStorage;
            const values = block.values orelse return error.NeedStorage;
            const offset = index & (block_slots - 1);
            const entry = &block.entries[offset];
            if (entry.withdrawn) return error.InvalidState;
            if (entry.used) return &values[offset].value;
            if (entry.exhausted) return error.IndexExhausted;
            var link = &self.free_head;
            while (link.* != none and link.* != index) {
                const at: u32 = @intCast(link.*);
                link = &self.blockAt(at).?.entries[at & (block_slots - 1)].next;
            }
            if (link.* == none) return error.InvalidState;
            link.* = entry.next;
            entry.next = none;
            entry.marks = 0;
            block.live += 1;
            self.live += 1;
            values[offset] = .{ .index = index, .value = initialValue() };
            if (comptime @typeInfo(T) == .@"struct" and @hasField(T, "generation")) values[offset].value.generation = entry.generation;
            @atomicStore(bool, &entry.used, true, .release);
            return &values[offset].value;
        }

        pub fn prepareFree(self: *Self, allocator: storage.Allocator) Error!u32 {
            if (self.free_head == none) {
                if (self.capacity > std.math.maxInt(u32)) return error.IndexExhausted;
                const index: u32 = @intCast(self.capacity);
                try self.prepareIndex(index, allocator);
                return index;
            }
            const index: u32 = @intCast(self.free_head);
            try self.prepareIndex(index, allocator);
            return index;
        }

        pub fn indexOf(value: *const T) u32 {
            const body: *const Body = @fieldParentPtr("value", value);
            return body.index;
        }

        pub fn allocate(self: *Self, allocator: storage.Allocator, leaf: ?*Leaf, max_index: u32) Error!Allocation {
            const flags = if (leaf) |l| l.lock() else false;
            const fast = self.allocatePrepared();
            if (leaf) |l| l.unlock(flags);
            if (fast) |value| return value else |err| if (err != error.NeedStorage) return err;
            // With no directory leaf the caller holds writer exclusion across
            // preparation, so an existing empty body can be restored directly.
            if (leaf == null and self.free_head != none) {
                try self.prepareIndex(@intCast(self.free_head), allocator);
                return self.allocatePrepared();
            }
            return self.growAndAllocate(allocator, leaf, max_index);
        }

        // Keep allocation/rollback/reclamation out of the ordinary free-slot
        // path; it also makes the no-growth IRQ contract visible in codegen.
        noinline fn growAndAllocate(self: *Self, allocator: storage.Allocator, leaf: ?*Leaf, max_index: u32) Error!Allocation {
            var draft = Prepared.init(self, allocator);
            defer draft.finish();
            try draft.reserve();
            const publish_flags = if (leaf) |l| l.lock() else false;
            const path = self.commitPrepared(&draft, max_index) catch |err| {
                if (leaf) |l| l.unlock(publish_flags);
                return err;
            };
            const value = self.allocatePrepared() catch unreachable;
            if (leaf) |l| l.unlock(publish_flags);
            self.retirePath(path);
            self.reclaim();
            return value;
        }

        /// End visibility and advance the generation without discarding warm
        /// storage. The caller owns the retained capacity and must reclaim it
        /// under pressure or at teardown after excluding all body borrowers.
        pub fn removeRetainingCapacity(self: *Self, value: *T) void {
            const index = indexOf(value);
            self.release(.{ .index = index, .generation = self.blockAt(index).?.entries[index & (block_slots - 1)].generation }) catch unreachable;
        }

        /// Withdraw visibility under the reader leaf, but quarantine the
        /// index and count the body as occupied until external guards/pins end.
        /// Ordinary pressure reclamation must never return quarantined bodies.
        pub fn withdraw(self: *Self, value: *T, leaf: ?*Leaf) void {
            const flags = if (leaf) |l| l.lock() else false;
            defer if (leaf) |l| l.unlock(flags);
            const index = indexOf(value);
            const entry = &self.blockAt(index).?.entries[index & (block_slots - 1)];
            std.debug.assert(entry.used and !entry.withdrawn);
            @atomicStore(bool, &entry.used, false, .release);
            entry.withdrawn = true;
            if (entry.marks & 1 != 0) self.mark(index, 1, false);
            entry.marks = 0;
            self.live -= 1;
        }

        /// Writer-only, AFTER withdrawal and external quiescence/live-pin
        /// exclusion. Only here can this generation's storage be reused.
        pub fn finishWithdrawal(self: *Self, value: *T, leaf: ?*Leaf) void {
            const index = indexOf(value);
            const flags = if (leaf) |l| l.lock() else false;
            const block = self.blockAt(index).?;
            const entry = &block.entries[index & (block_slots - 1)];
            std.debug.assert(!entry.used and entry.withdrawn);
            entry.withdrawn = false;
            @atomicStore(bool, &entry.used, true, .release);
            self.live += 1;
            self.release(.{ .index = index, .generation = entry.generation }) catch unreachable;
            const empty = if (block.live == 0) block.values else null;
            if (empty != null) @atomicStore(?[*]Body, &block.values, null, .release);
            if (leaf) |l| l.unlock(flags);
            if (empty) |values| self.body_owner.retireAllocation(Body, values) catch unreachable;
            self.reclaim();
        }

        /// The caller has ended all body loans under its resource exclusion.
        /// Remove visibility under the leaf, then retire/return outside it.
        pub fn remove(self: *Self, value: *T, leaf: ?*Leaf) void {
            const index = indexOf(value);
            const flags = if (leaf) |l| l.lock() else false;
            const block = self.blockAt(index).?;
            self.release(.{ .index = index, .generation = block.entries[index & (block_slots - 1)].generation }) catch unreachable;
            const empty = if (block.live == 0) block.values else null;
            if (empty != null) @atomicStore(?[*]Body, &block.values, null, .release);
            if (leaf) |l| l.unlock(flags);
            if (empty) |values| self.body_owner.retireAllocation(Body, values) catch unreachable;
            self.reclaim();
        }

        /// Exec transfers the whole table under both AS locks. No resource
        /// body or address-bound Owner is copied, and no allocation can fail
        /// after the old address space has been withdrawn.
        pub fn takeFrom(self: *Self, source: *Self) void {
            std.debug.assert(self != source and self.live == 0);
            self.deinit();
            self.directory_owner.takePublished(&source.directory_owner) catch unreachable;
            self.metadata_owner.takePublished(&source.metadata_owner) catch unreachable;
            self.body_owner.takePublished(&source.body_owner) catch unreachable;
            self.root = source.root;
            self.capacity = source.capacity;
            self.live = source.live;
            self.free_head = source.free_head;
            source.root = null;
            source.capacity = 0;
            source.live = 0;
            source.free_head = none;
        }

        pub fn deinitInto(self: *Self, owner: *storage.Owner) void {
            self.deinit();
            inline for (.{ &self.directory_owner, &self.metadata_owner, &self.body_owner }) |child|
                child.transferUnreturned(owner) catch unreachable;
        }

        pub const Iterator = struct {
            table: *const Self,
            next_index: u64 = 0,
            leaf: ?*Leaf = null,
            pub fn next(self: *Iterator) ?Allocation {
                while (true) {
                    const flags = if (self.leaf) |l| l.lock() else false;
                    if (self.next_index >= self.table.capacity) {
                        if (self.leaf) |l| l.unlock(flags);
                        return null;
                    }
                    const index: u32 = @intCast(self.next_index);
                    self.next_index += 1;
                    const value = self.table.get(index);
                    const gen = if (value != null) self.table.generation(index).? else 0;
                    if (self.leaf) |l| l.unlock(flags);
                    if (value) |ptr| return .{ .ref = .{ .index = index, .generation = gen }, .value = ptr };
                }
            }
        };

        pub fn iteratorLocked(self: *const Self, leaf: *Leaf) Iterator {
            return .{ .table = self, .leaf = leaf };
        }
        pub fn iterator(self: *const Self) Iterator {
            return .{ .table = self };
        }
    };
}

const TestBackend = struct {
    grants: std.ArrayList(?storage.MappedExtent) = .empty,
    calls: usize = 0,
    fail_at: usize = 0,
    reject_returns: bool = false,
    returned: usize = 0,
    largest: usize = 0,

    fn interface(self: *TestBackend) storage.Allocator {
        return .{ .context = self, .acquire = acquire, .release = release };
    }
    fn acquire(context: *anyopaque, pages: usize) ?storage.MappedExtent {
        const self: *TestBackend = @ptrCast(@alignCast(context));
        self.calls += 1;
        self.largest = @max(self.largest, pages);
        if (self.calls == self.fail_at) return null;
        const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), pages * 4096) catch return null;
        const mapped: storage.MappedExtent = .{
            .allocation = .{ .physical = .fromInt((self.grants.items.len + 1) * 0x100000), .pages = pages, .region_id = 117 },
            .bytes = bytes,
        };
        self.grants.append(std.testing.allocator, mapped) catch {
            std.testing.allocator.free(bytes);
            return null;
        };
        return mapped;
    }
    fn release(context: *anyopaque, grant: storage.PhysicalExtent) bool {
        const self: *TestBackend = @ptrCast(@alignCast(context));
        if (self.reject_returns) return false;
        const index = grant.physical.toInt() / 0x100000 - 1;
        const mapped = self.grants.items[index].?;
        std.debug.assert(std.meta.eql(mapped.allocation, grant));
        @memset(mapped.bytes, 0xa5);
        std.testing.allocator.free(mapped.bytes);
        self.grants.items[index] = null;
        self.returned += 1;
        return true;
    }
    fn active(self: *TestBackend) usize {
        var n: usize = 0;
        for (self.grants.items) |grant| if (grant != null) {
            n += 1;
        };
        return n;
    }
    fn deinit(self: *TestBackend) void {
        for (self.grants.items) |grant| if (grant) |g| std.testing.allocator.free(g.bytes);
        self.grants.deinit(std.testing.allocator);
    }
};

fn testAllocate(table: anytype, backend: *TestBackend) !@TypeOf(table.*).Allocation {
    return table.allocatePrepared() catch |err| switch (err) {
        error.NeedStorage => blk: {
            var draft = @TypeOf(table.*).Prepared.init(table, backend.interface());
            defer draft.finish();
            try draft.reserve();
            const retired = try table.commitPrepared(&draft, std.math.maxInt(u32));
            table.retirePath(retired);
            table.reclaim();
            break :blk try table.allocatePrepared();
        },
        else => return err,
    };
}

test "slot table exceeds former VMO limit with stable bodies bounded grants and stale rejection" {
    var backend = TestBackend{};
    defer backend.deinit();
    var table = SlotTable(u64){};
    defer table.deinit();
    const first = try testAllocate(&table, &backend);
    first.value.* = 0x12345678;
    for (1..32769) |i| {
        const added = try testAllocate(&table, &backend);
        try std.testing.expectEqual(i, added.ref.index);
        added.value.* = i;
    }
    try std.testing.expect(table.resolve(first.ref).? == first.value);
    try std.testing.expectEqual(@as(u64, 0x12345678), first.value.*);
    try std.testing.expectEqual(@as(usize, 1), backend.largest);
    var iter = table.iterator();
    while (iter.next()) |entry| try table.release(entry.ref);
    table.reclaimEmptyBodies();
    try std.testing.expect(table.resolve(first.ref) == null);
    const reused = try testAllocate(&table, &backend);
    try std.testing.expectEqual(@as(u32, 32768), reused.ref.index);
    try std.testing.expectEqual(@as(u32, 2), reused.ref.generation);
    try table.release(reused.ref);
    table.deinit();
    try std.testing.expectEqual(@as(usize, 0), backend.active());
}

test "slot table rolls back every growth allocation and retries rejected original grants" {
    const Table = SlotTable(u64);
    for (1..Table.levels + 4) |failure| {
        var backend = TestBackend{ .fail_at = failure, .reject_returns = true };
        defer backend.deinit();
        var table = Table{};
        var draft = Table.Prepared.init(&table, backend.interface());
        try std.testing.expectError(error.OutOfMemory, draft.reserve());
        draft.finish();
        try std.testing.expectEqual(@as(u64, 0), table.capacity);
        backend.reject_returns = false;
        table.reclaim();
        try std.testing.expectEqual(@as(usize, 0), backend.active());
        table.deinit();
    }
}

test "slot table stale preparation cannot replace a concurrent publication" {
    var backend = TestBackend{};
    defer backend.deinit();
    var table = SlotTable(u64){};
    defer table.deinit();
    var a = @TypeOf(table).Prepared.init(&table, backend.interface());
    defer a.finish();
    var b = @TypeOf(table).Prepared.init(&table, backend.interface());
    defer b.finish();
    try a.reserve();
    try b.reserve();
    table.retirePath(try table.commitPrepared(&a, std.math.maxInt(u32)));
    try std.testing.expectError(error.StalePreparation, table.commitPrepared(&b, std.math.maxInt(u32)));
    const value = try table.allocatePrepared();
    try std.testing.expectEqual(@as(u32, 0), value.ref.index);
}

test "slot table generation exhaustion retires the index rather than wrapping" {
    var backend = TestBackend{};
    defer backend.deinit();
    var table = SlotTable(u64){};
    defer table.deinit();
    const old = try testAllocate(&table, &backend);
    table.blockAt(old.ref.index).?.entries[0].generation = std.math.maxInt(u32);
    try table.release(.{ .index = old.ref.index, .generation = std.math.maxInt(u32) });
    try std.testing.expectEqual(@as(usize, 63), table.preparedFreeCount(64));
    const next = try testAllocate(&table, &backend);
    try std.testing.expect(next.ref.index != old.ref.index);
    try std.testing.expect(table.resolve(old.ref) == null);
    try std.testing.expectError(error.IndexExhausted, table.prepareIndex(old.ref.index, backend.interface()));
}

var host_context: u8 = 0;
/// Logical kernel fixtures have no identity-mapped PMM. These grants use the
/// host page allocator; storage ownership/rejected returns are tested above
/// with an independently addressed, leak-checking allocator instead.
pub fn hostTestAllocator() storage.Allocator {
    if (!builtin.is_test) @compileError("host allocator is test-only");
    return .{ .context = &host_context, .acquire = hostAcquire, .release = hostRelease };
}
fn hostAcquire(_: *anyopaque, pages: usize) ?storage.MappedExtent {
    const bytes = std.heap.page_allocator.alignedAlloc(u8, .fromByteUnits(4096), pages * 4096) catch return null;
    return .{ .allocation = .{ .physical = .fromInt(@intFromPtr(bytes.ptr)), .pages = pages, .region_id = 0 }, .bytes = bytes };
}
fn hostRelease(_: *anyopaque, grant: storage.PhysicalExtent) bool {
    const bytes: [*]align(4096) u8 = @ptrFromInt(grant.physical.toInt());
    std.heap.page_allocator.free(bytes[0 .. grant.pages * 4096]);
    return true;
}

test "slot table sparse claims preserve holes and exec transfers stable bodies" {
    var backend = TestBackend{};
    defer backend.deinit();
    var source = SlotTable(u64){};
    defer source.deinit();
    var destination = SlotTable(u64){};
    defer destination.deinit();
    try source.prepareIndex(8192, backend.interface());
    const body = try source.claimIndex(8192);
    body.* = 77;
    const generation_value = source.generation(8192).?;
    destination.takeFrom(&source);
    try std.testing.expect(source.root == null and source.live == 0);
    try std.testing.expectEqual(body, destination.resolve(.{ .index = 8192, .generation = generation_value }).?);
    try std.testing.expectEqual(@as(u64, 77), body.*);
    const hole = try destination.allocate(backend.interface(), null, std.math.maxInt(u32));
    try std.testing.expect(hole.ref.index != 8192);
}

test "nested table rejected returns remain owned after parent body retirement" {
    var backend = TestBackend{};
    defer backend.deinit();
    const Child = SlotTable(u64);
    var parent = SlotTable(struct { child: Child = .{} }){};
    const entry = try parent.allocate(backend.interface(), null, std.math.maxInt(u32));
    _ = try entry.value.child.allocate(backend.interface(), null, std.math.maxInt(u32));
    backend.reject_returns = true;
    entry.value.child.deinitInto(&parent.body_owner);
    parent.remove(entry.value, null);
    try std.testing.expect(backend.active() != 0);
    backend.reject_returns = false;
    parent.deinit();
    try std.testing.expectEqual(@as(usize, 0), backend.active());
}

test "outside writer readers keep stable bodies while directories grow and retire" {
    var backend = TestBackend{};
    defer backend.deinit();
    const Table = SlotTable(u64);
    var table = Table{};
    defer table.deinit();
    var leaf = Leaf{};
    const first = try table.allocate(backend.interface(), &leaf, std.math.maxInt(u32));
    first.value.* = 123;
    const Reader = struct {
        table: *Table,
        leaf: *Leaf,
        reference: Ref,
        value: *u64,
        stop: bool = false,
        reads: usize = 0,
        fn run(self: *@This()) void {
            while (!@atomicLoad(bool, &self.stop, .acquire)) {
                const flags = self.leaf.lock();
                const body = self.table.resolve(self.reference).?;
                self.leaf.unlock(flags);
                // The writer retains this resource until the reader joins,
                // just as AS/shared VM protects a VMO loan after lookup.
                std.debug.assert(body == self.value and body.* == 123);
                _ = @atomicRmw(usize, &self.reads, .Add, 1, .monotonic);
            }
        }
    };
    var reader = Reader{ .table = &table, .leaf = &leaf, .reference = first.ref, .value = first.value };
    const thread = try std.Thread.spawn(.{}, Reader.run, .{&reader});
    defer {
        @atomicStore(bool, &reader.stop, true, .release);
        thread.join();
    }
    while (@atomicLoad(usize, &reader.reads, .monotonic) == 0) std.atomic.spinLoopHint();
    for (0..4097) |_| _ = try table.allocate(backend.interface(), &leaf, std.math.maxInt(u32));
    try std.testing.expectEqual(first.value, table.resolve(first.ref).?);
}
