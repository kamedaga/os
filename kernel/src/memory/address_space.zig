const lock_diagnostics = @import("../sync/lock_diagnostics.zig");
const pt_storage = @import("pt_storage.zig");
const std = @import("std");
const builtin = @import("builtin");
const owned_storage = @import("owned_storage.zig");
const identity_allocator = @import("identity_allocator.zig");
const table = @import("../state/table.zig");
const types = @import("../state/types.zig");

pub const AddressSpaceLockState = struct {
    value: u8 = 0,
    diagnostic_record: lock_diagnostics.Record = .{},
    owner_cpu: usize = ~@as(usize, 0),
    depth: u32 = 0,
};

pub const UserAddressSpace = struct {
    // Includes user VA mappings plus supervisor-only helper PTs for return stacks.
    pub const no_pd_index: u16 = 0xFFFF;

    pub const ReservationKind = enum(u8) {
        none = 0,
        bootstrap = 1,
        capability_page = 2,
        fresh_page = 3,
        linear_region = 4,
    };

    pub const Reservation = struct {
        base_va: u64 = 0,
        page_count: u64 = 0,
        generation: u32 = 0,
        kind: ReservationKind = .none,
        writable: bool = false,
        active: bool = false,
    };

    pub const ReservationTable = @import("slot_table.zig").SlotTable(Reservation);

    pml4: [512]u64 align(4096) = [_]u64{0} ** 512,
    pdp_store: pt_storage.Store = .{},
    pd_store: pt_storage.Store = .{},
    pt_store: pt_storage.Store = .{},
    // Temporary transaction pages also keep an owner across PMM rejection.
    // Never hardware-published; A serializes use by journal/topology preflight.
    scratch_store: pt_storage.Store = .{},
    pdp_page_used_len: usize = 0,
    pd_page_used_len: usize = 0,
    pt_page_used_len: usize = 0,
    reservations: ReservationTable = .{},
    reservation_draft: ReservationTable = .{},
    reservation_generation: u32 = 0,
    next_dynamic_map_page: u64 = 0,
    cr3: u64 = 0,
    // This object is address-stable: its page tables contain physical
    // pointers to its inline PML4.  Keep the lock with that lifetime
    // and never reset or copy it as address-space contents are recycled.
    lock_state: AddressSpaceLockState = .{},
    // AD protects publication and loans, including waiters not yet holding A.
    fast_live: @import("../sync/ipc_lifetime.zig").Live = .init(),
    borrowers: usize = 0,
    retiring: bool = false,
    retired_next: ?*UserAddressSpace = null,
    directory_index: usize = 0,

    // Assigned slots always have PT bodies. Unused slots may retain owned
    // pages if PMM return failed; every slot uses the same storage mechanism.
    pub fn ptCapacity(self: *const UserAddressSpace) usize {
        return self.pt_store.capacity();
    }

    fn DescriptorPointer(comptime Self: type) type {
        return if (@typeInfo(Self).pointer.is_const) *const pt_storage.Descriptor else *pt_storage.Descriptor;
    }

    fn ptDescriptor(self: anytype, slot: usize) DescriptorPointer(@TypeOf(self)) {
        if (comptime @typeInfo(@TypeOf(self)).pointer.is_const)
            return self.pt_store.descriptorConst(slot).?;
        return self.pt_store.descriptor(slot).?;
    }

    pub fn ptPage(self: anytype, slot: usize) if (@typeInfo(@TypeOf(self)).pointer.is_const) *align(4096) const pt_storage.PtPage else pt_storage.PtPointer {
        return self.ptDescriptor(slot).page.?;
    }

    pub fn ptPml4Index(self: anytype, slot: usize) if (@typeInfo(@TypeOf(self)).pointer.is_const) *const u16 else *u16 {
        return &self.ptDescriptor(slot).pml4;
    }

    pub fn ptPdpIndex(self: anytype, slot: usize) if (@typeInfo(@TypeOf(self)).pointer.is_const) *const u16 else *u16 {
        return &self.ptDescriptor(slot).pdp;
    }

    pub fn ptPdIndex(self: anytype, slot: usize) if (@typeInfo(@TypeOf(self)).pointer.is_const) *const u16 else *u16 {
        return &self.ptDescriptor(slot).pd;
    }

    fn pdpDescriptor(self: anytype, slot: usize) DescriptorPointer(@TypeOf(self)) {
        if (comptime @typeInfo(@TypeOf(self)).pointer.is_const)
            return self.pdp_store.descriptorConst(slot).?;
        return self.pdp_store.descriptor(slot).?;
    }
    pub fn pdpPage(self: anytype, slot: usize) if (@typeInfo(@TypeOf(self)).pointer.is_const) *align(4096) const pt_storage.PtPage else pt_storage.PtPointer {
        return self.pdpDescriptor(slot).page.?;
    }
    pub fn pdpPml4Index(self: anytype, slot: usize) if (@typeInfo(@TypeOf(self)).pointer.is_const) *const u16 else *u16 {
        return &self.pdpDescriptor(slot).pml4;
    }
    fn pdDescriptor(self: anytype, slot: usize) DescriptorPointer(@TypeOf(self)) {
        if (comptime @typeInfo(@TypeOf(self)).pointer.is_const)
            return self.pd_store.descriptorConst(slot).?;
        return self.pd_store.descriptor(slot).?;
    }
    pub fn pdPage(self: anytype, slot: usize) if (@typeInfo(@TypeOf(self)).pointer.is_const) *align(4096) const pt_storage.PtPage else pt_storage.PtPointer {
        return self.pdDescriptor(slot).page.?;
    }
    pub fn pdPml4Index(self: anytype, slot: usize) if (@typeInfo(@TypeOf(self)).pointer.is_const) *const u16 else *u16 {
        return &self.pdDescriptor(slot).pml4;
    }
    pub fn pdPdpIndex(self: anytype, slot: usize) if (@typeInfo(@TypeOf(self)).pointer.is_const) *const u16 else *u16 {
        return &self.pdDescriptor(slot).pdp;
    }
    pub fn firstFreePtSlot(self: *const UserAddressSpace) ?usize {
        const slot = self.pt_store.firstFreeSlot() orelse return null;
        return if (slot < self.ptCapacity()) slot else null;
    }
};

/// First initialization only; the caller proves no PT storage is owned.
pub fn initializeUserAddressSpaceStorage(space: *UserAddressSpace) void {
    space.fast_live = .init();
    space.borrowers = 0;
    space.retiring = false;
    space.retired_next = null;
    space.directory_index = 0;
    space.pdp_store = .{};
    space.pd_store = .{};
    space.pt_store = .{};
    space.scratch_store = .{};
    space.reservations = .{};
    space.reservation_draft = .{};
    resetUserAddressSpaceStorage(space);
}

/// Caller has detached/synchronized live tables before reusing this storage.
pub fn resetUserAddressSpaceStorage(space: *UserAddressSpace) void {
    @memset(space.pml4[0..], 0);
    space.pdp_store.resetDetachedContents();
    space.pd_store.resetDetachedContents();
    space.pt_store.resetDetachedContents();
    space.pdp_page_used_len = 0;
    space.pd_page_used_len = 0;
    space.pt_page_used_len = 0;
    space.reservations.deinit();
    space.reservation_draft.deinit();
    space.reservation_generation = 0;
    space.next_dynamic_map_page = 0;
    space.cr3 = 0;
}

const DirectoryTestProbe = if (builtin.is_test) struct {
    var contended: ?*u8 = null;
} else struct {};

fn directoryInvariantFailure() noreturn {
    // The leaf may be held: logging/panic rendering can reenter VM or device
    // code. Stop without callbacks rather than turn corruption into deadlock.
    if (builtin.is_test) @panic("AS directory ownership invariant");
    while (true) asm volatile ("cli; hlt");
}

pub const UserAddressSpaceTable = struct {
    inline_spaces: []UserAddressSpace = undefined,
    // Only get borrows this directory; returned bodies have independent,
    // stable storage because live CR3/PTEs and AS locks retain their addresses.
    extra_slots: [*]usize = undefined,
    capacity_value: usize = 0,
    directory_lock: u8 = 0,
    directory_record: lock_diagnostics.Record = .{},
    directory_storage: owned_storage.Owner = .{},
    body_storage: owned_storage.Owner = .{},
    retired_head: ?*UserAddressSpace = null,

    fn lockDirectory(self: *UserAddressSpaceTable) bool {
        var flags: u64 = 0;
        if (!builtin.is_test) asm volatile ("pushfq; pop %[flags]; cli"
            : [flags] "=r" (flags),
            :
            : .{ .memory = true });
        // Leaf lock: AS/shared VM callers already hold their own locks.
        // Never wait for another subsystem, allocate, return, or callback here.
        // IF=0 prevents a local IRQ from spinning on its interrupted reader.
        lock_diagnostics.beforeAcquire(&self.directory_record, .{ .class = .as_directory });
        while (@cmpxchgWeak(u8, &self.directory_lock, 0, 1, .acquire, .monotonic) != null) {
            if (builtin.is_test) if (DirectoryTestProbe.contended) |probe| {
                @atomicStore(u8, probe, 1, .release);
            };
            while (@atomicLoad(u8, &self.directory_lock, .monotonic) != 0) asm volatile ("pause");
        }
        lock_diagnostics.acquired(&self.directory_record, .{ .class = .as_directory });
        return flags & (1 << 9) != 0;
    }

    fn unlockDirectory(self: *UserAddressSpaceTable, restore_interrupts: bool) void {
        lock_diagnostics.releasing(&self.directory_record);
        @atomicStore(u8, &self.directory_lock, 0, .release);
        if (!builtin.is_test and restore_interrupts) asm volatile ("sti" ::: .{ .memory = true });
    }

    /// Fresh, unpublished storage only, including fixture reconstruction.
    /// This is not a reset: published bodies may still be CPU/PTE/lock owners.
    /// Production construction is confined to boot by the caller census.
    pub fn initializeFreshStorage(self: *UserAddressSpaceTable, inline_spaces: []UserAddressSpace) void {
        self.inline_spaces = inline_spaces;
        self.extra_slots = undefined;
        self.directory_lock = 0;
        self.directory_record = .{};
        self.directory_storage = .{};
        self.body_storage = .{};
        self.retired_head = null;
        @atomicStore(usize, &self.capacity_value, inline_spaces.len, .release);
    }

    pub fn capacity(self: *UserAddressSpaceTable) usize {
        return @atomicLoad(usize, &self.capacity_value, .acquire);
    }

    pub fn get(self: *UserAddressSpaceTable, index: usize) ?*UserAddressSpace {
        if (index < self.inline_spaces.len) return &self.inline_spaces[index];
        const restore_interrupts = self.lockDirectory();
        defer self.unlockDirectory(restore_interrupts);
        if (index >= @atomicLoad(usize, &self.capacity_value, .acquire)) return null;
        const raw = self.extra_slots[index - self.inline_spaces.len];
        if (raw == 0) return null;
        return @ptrFromInt(raw);
    }

    pub const FastPin = struct {
        space: *UserAddressSpace,
        pub fn end(self: FastPin) void {
            self.space.fast_live.unpin();
        }
    };

    // AD bridges lookup to inc-not-zero: no raw AS pointer crosses this
    // leaf without ownership. The final unpin never returns PMM storage.
    pub fn pinFast(self: *UserAddressSpaceTable, index: usize) ?FastPin {
        const flags = self.lockDirectory();
        defer self.unlockDirectory(flags);
        const space = self.getLocked(index) orelse return null;
        if (space.retiring or !space.fast_live.tryPin()) return null;
        return .{ .space = space };
    }
    pub fn fastPinsEnded(self: *UserAddressSpaceTable, index: usize) bool {
        const flags = self.lockDirectory();
        defer self.unlockDirectory(flags);
        const space = self.getLocked(index) orelse return true;
        return space.fast_live.pinsEnded();
    }

    /// Publication and pin acquisition share AD; never acquire A under AD.
    pub fn borrow(self: *UserAddressSpaceTable, index: usize) ?*UserAddressSpace {
        const flags = self.lockDirectory();
        defer self.unlockDirectory(flags);
        const space = self.getLocked(index) orelse return null;
        if (space.retiring or space.borrowers == std.math.maxInt(usize)) return null;
        space.borrowers += 1;
        return space;
    }

    fn getLocked(self: *UserAddressSpaceTable, index: usize) ?*UserAddressSpace {
        if (index < self.inline_spaces.len) return &self.inline_spaces[index];
        if (index >= self.capacity_value) return null;
        const raw = self.extra_slots[index - self.inline_spaces.len];
        return if (raw == 0) null else @ptrFromInt(raw);
    }

    pub fn endBorrow(self: *UserAddressSpaceTable, space: *UserAddressSpace) void {
        const flags = self.lockDirectory();
        defer self.unlockDirectory(flags);
        std.debug.assert(space.borrowers != 0);
        space.borrowers -= 1;
    }

    /// K and A exclude lifecycle writers. Keep the directory entry until all
    /// old loans, A waiters and recursive holders have executed endBorrow.
    pub fn retire(self: *UserAddressSpaceTable, index: usize) void {
        const flags = self.lockDirectory();
        defer self.unlockDirectory(flags);
        const space = self.getLocked(index) orelse return;
        if (space.retiring) return;
        space.fast_live.close();
        space.retiring = true;
        space.directory_index = index;
        space.retired_next = self.retired_head;
        self.retired_head = space;
    }

    pub fn loansEnded(self: *UserAddressSpaceTable, space: *UserAddressSpace) bool {
        const flags = self.lockDirectory();
        defer self.unlockDirectory(flags);
        return space.borrowers == 0 and space.fast_live.pinsEnded();
    }

    /// Caller proves all CPU/context use ended, children returned and the old
    /// CR3/PCID shootdown completed. K serializes the retirement list writer.
    pub fn reclaimRetiredBody(self: *UserAddressSpaceTable, space: *UserAddressSpace) void {
        const flags = self.lockDirectory();
        std.debug.assert(space.retiring and space.borrowers == 0 and space.fast_live.pinsEnded() and space.lock_state.value == 0);
        var link = &self.retired_head;
        while (link.* != space) link = &link.*.?.retired_next;
        link.* = space.retired_next;
        space.retired_next = null;
        const index = space.directory_index;
        if (index >= self.inline_spaces.len) self.extra_slots[index - self.inline_spaces.len] = 0;
        self.unlockDirectory(flags);
        // Inline boot bodies own no grant. They remain withdrawn until the
        // next ensureSlot publishes that process slot for a new occupant.
        if (index >= self.inline_spaces.len) {
            self.body_storage.retireAllocation(UserAddressSpace, @as([*]UserAddressSpace, @ptrCast(space))) catch directoryInvariantFailure();
        }
        self.body_storage.reclaimRetiredAfterBorrowExclusion() catch directoryInvariantFailure();
    }

    pub fn ensureCapacity(
        self: *UserAddressSpaceTable,
        required: usize,
        free_list: *types.FreePageList,
    ) bool {
        self.retryDetachedDirectoryReturns();
        const old_capacity = self.capacity();
        if (required <= old_capacity) return true;
        const new_capacity = table.nextGeometricCapacity(
            old_capacity,
            self.inline_spaces.len,
            types.process_principal_value_count,
            required,
        ) orelse return false;
        const new_extra_count = new_capacity - self.inline_spaces.len;
        const old_extra_count = old_capacity - self.inline_spaces.len;
        // KernelState serializes writers, including allocator/owner mutation.
        // Directory readers need only the short copy/publication exclusion.
        self.directory_storage.retryUnpublished() catch @panic("moved AS directory owner");
        var transaction = owned_storage.Transaction.init(&self.directory_storage, identity_allocator.allocator(free_list));
        defer if (!transaction.finished) transaction.rollback() catch @panic("invalid AS directory rollback");
        const new_extra = transaction.reserve(usize, new_extra_count, .replaceable) catch return false;
        @memset(new_extra, 0);
        const restore_interrupts = self.lockDirectory();
        if (old_extra_count != 0) {
            @memcpy(new_extra[0..old_extra_count], self.extra_slots[0..old_extra_count]);
        }

        transaction.commit() catch directoryInvariantFailure();
        self.extra_slots = new_extra.ptr;
        @atomicStore(usize, &self.capacity_value, new_capacity, .release);
        self.unlockDirectory(restore_interrupts);
        // Prior readers finished before the swap; new readers cannot reach
        // retired directories. Bodies were copied as pointers, never moved.
        self.retryDetachedDirectoryReturns();
        return true;
    }

    fn retryDetachedDirectoryReturns(self: *UserAddressSpaceTable) void {
        // Only this owner's previously swapped-out directories are retired.
        // No reader can reacquire them; writer exclusion still serializes the
        // list. Retry even without growth so a transient PMM rejection does not
        // retain pages until a future capacity increase (which may never occur).
        self.directory_storage.reclaimRetiredAfterBorrowExclusion() catch directoryInvariantFailure();
    }

    pub fn ensureSlot(
        self: *UserAddressSpaceTable,
        index: usize,
        free_list: *types.FreePageList,
    ) ?*UserAddressSpace {
        if (types.processPrincipalFromIndex(index) == null) return null;
        if (!self.ensureCapacity(index + 1, free_list)) return null;
        if (self.get(index)) |space| {
            if (space.retiring) {
                // Only a reaped inline body can be republished. Pending
                // retirees keep their slot unavailable even with no CPU pin.
                var retired = self.retired_head;
                while (retired) |body| : (retired = body.retired_next) if (body == space) return null;
                std.debug.assert(index < self.inline_spaces.len and space.borrowers == 0);
                const flags = self.lockDirectory();
                space.fast_live = .init();
                space.retiring = false;
                self.unlockDirectory(flags);
            }
            return space;
        }
        if (index < self.inline_spaces.len) unreachable;

        self.body_storage.retryUnpublished() catch @panic("moved AS body owner");
        var transaction = owned_storage.Transaction.init(&self.body_storage, identity_allocator.allocator(free_list));
        defer if (!transaction.finished) transaction.rollback() catch @panic("invalid AS body rollback");
        const storage = transaction.reserve(UserAddressSpace, 1, .stable) catch return null;
        const space = &storage[0];
        space.lock_state = .{};
        space.borrowers = 0;
        space.retiring = false;
        space.retired_next = null;
        space.directory_index = index;
        initializeUserAddressSpaceStorage(space);
        const restore_interrupts = self.lockDirectory();
        const entry = &self.extra_slots[index - self.inline_spaces.len];
        if (entry.* != 0) {
            const existing: *UserAddressSpace = @ptrFromInt(entry.*);
            self.unlockDirectory(restore_interrupts);
            return existing;
        }
        transaction.commitAdditionalStable() catch directoryInvariantFailure();
        entry.* = @intFromPtr(space);
        self.unlockDirectory(restore_interrupts);
        return space;
    }
};

test "MMIO overlay AS directory growth reclaims metadata but preserves body grants" {
    const Backing = struct {
        var bytes: [4 * 1024 * 1024]u8 align(4096) = undefined;
    };
    // Static host fixture stays below the actual identity allocator limit;
    // arbitrary testing-allocator virtual addresses are not physical grants.
    const memory = Backing.bytes[0..];
    const inline_spaces = try std.testing.allocator.alloc(UserAddressSpace, 1);
    defer std.testing.allocator.free(inline_spaces);
    var free_list: types.FreePageList = .{};
    try free_list.installRegions(&.{.{ .id = 93, .start = @intFromPtr(memory.ptr), .end = (@intFromPtr(memory.ptr)) + (memory.len / 4096) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(memory.ptr), memory.len / 4096);
    var spaces: UserAddressSpaceTable = .{};
    spaces.initializeFreshStorage(inline_spaces);
    try std.testing.expect(spaces.ensureSlot(types.process_principal_value_count, &free_list) == null);
    try std.testing.expect(spaces.ensureSlot(std.math.maxInt(usize), &free_list) == null);
    const body = spaces.ensureSlot(1, &free_list) orelse return error.TestUnexpectedResult;
    body.cr3 = 0x12345000;
    const body_pages = try owned_storage.requiredPages(UserAddressSpace, 1);
    for ([_]usize{ 3, 65, 513, 1025, 65537 }) |required| {
        try std.testing.expect(spaces.ensureCapacity(required, &free_list));
        try std.testing.expectEqual(body, spaces.get(1).?);
        try std.testing.expectEqual(@as(u64, 0x12345000), spaces.get(1).?.cr3);
        try std.testing.expect(spaces.get(required - 1) == null);
        const directory_pages = try owned_storage.requiredPages(usize, spaces.capacity() - 1);
        try std.testing.expectEqual(memory.len / 4096 - body_pages - directory_pages, free_list.pageCount());
        try std.testing.expect(spaces.directory_storage.retired == null);
        for (0..free_list.rangeCount()) |range_index| {
            const range = free_list.rangeAt(range_index);
            try std.testing.expectEqual(@as(u64, 93), range.region_id);
        }
    }
    const old_directory = spaces.extra_slots;
    const old_capacity = spaces.capacity();
    var empty: types.FreePageList = .{};
    try std.testing.expect(!spaces.ensureCapacity(old_capacity + 1, &empty));
    try std.testing.expectEqual(old_directory, spaces.extra_slots);
    try std.testing.expectEqual(old_capacity, spaces.capacity());
    try std.testing.expectEqual(body, spaces.get(1).?);
    try std.testing.expect(spaces.ensureSlot(2, &empty) == null);
    try std.testing.expect(spaces.get(2) == null);
    try std.testing.expectEqual(body, spaces.ensureSlot(1, &empty).?);
}

test "MMIO overlay AS directory concurrent readers survive actual metadata reuse" {
    const Backing = struct {
        var bytes: [4 * 1024 * 1024]u8 align(4096) = undefined;
    };
    const memory = Backing.bytes[0..];
    const inline_spaces = try std.testing.allocator.alloc(UserAddressSpace, 1);
    defer std.testing.allocator.free(inline_spaces);
    var free_list: types.FreePageList = .{};
    try free_list.installRegions(&.{.{ .id = 94, .start = @intFromPtr(memory.ptr), .end = (@intFromPtr(memory.ptr)) + (memory.len / 4096) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(memory.ptr), memory.len / 4096);
    var spaces: UserAddressSpaceTable = .{};
    spaces.initializeFreshStorage(inline_spaces);
    const body = spaces.ensureSlot(1, &free_list) orelse return error.TestUnexpectedResult;
    const Reader = struct {
        spaces: *UserAddressSpaceTable,
        body: *UserAddressSpace,
        started: u8 = 0,
        stop: u8 = 0,
        bad: bool = false,
        fn run(self: *@This()) void {
            if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(1);
            if (self.spaces.get(1) != self.body) self.bad = true;
            @atomicStore(u8, &self.started, 1, .release);
            while (@atomicLoad(u8, &self.stop, .acquire) == 0) {
                if (self.spaces.get(1) != self.body) self.bad = true;
            }
        }
    };
    var reader: Reader = .{ .spaces = &spaces, .body = body };
    const thread = try std.Thread.spawn(.{}, Reader.run, .{&reader});
    var joined = false;
    defer if (!joined) {
        @atomicStore(u8, &reader.stop, 1, .release);
        thread.join();
    };
    while (@atomicLoad(u8, &reader.started, .acquire) == 0) std.atomic.spinLoopHint();
    var required: usize = 3;
    while (required < 32768) : (required = (required - 1) * 2 + 1) {
        try std.testing.expect(spaces.ensureCapacity(required, &free_list));
    }
    @atomicStore(u8, &reader.stop, 1, .release);
    thread.join();
    joined = true;
    try std.testing.expect(!reader.bad);
}

test "MMIO overlay AS directory held reader excludes publication and return" {
    const Backing = struct {
        var bytes: [4 * 1024 * 1024]u8 align(4096) = undefined;
    };
    const inline_spaces = try std.testing.allocator.alloc(UserAddressSpace, 1);
    defer std.testing.allocator.free(inline_spaces);
    var free_list: types.FreePageList = .{};
    try free_list.installRegions(&.{.{ .id = 95, .start = @intFromPtr(&Backing.bytes), .end = (@intFromPtr(&Backing.bytes)) + (Backing.bytes.len / 4096) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(&Backing.bytes), Backing.bytes.len / 4096);
    var spaces: UserAddressSpaceTable = .{};
    spaces.initializeFreshStorage(inline_spaces);
    const body = spaces.ensureSlot(1, &free_list) orelse return error.TestUnexpectedResult;
    const before = free_list.pageCount();
    const restore = spaces.lockDirectory();
    var held = true;
    defer if (held) spaces.unlockDirectory(restore);
    const borrowed = spaces.extra_slots;
    var contended: u8 = 0;
    DirectoryTestProbe.contended = &contended;
    defer DirectoryTestProbe.contended = null;
    const Writer = struct {
        spaces: *UserAddressSpaceTable,
        free_list: *types.FreePageList,
        done: u8 = 0,
        ok: bool = false,
        fn run(self: *@This()) void {
            if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(1);
            self.ok = self.spaces.ensureCapacity(3, self.free_list);
            @atomicStore(u8, &self.done, 1, .release);
        }
    };
    var writer: Writer = .{ .spaces = &spaces, .free_list = &free_list };
    const thread = try std.Thread.spawn(.{}, Writer.run, .{&writer});
    var joined = false;
    defer if (!joined) {
        if (held) {
            spaces.unlockDirectory(restore);
            held = false;
        }
        thread.join();
    };
    while (@atomicLoad(u8, &contended, .acquire) == 0 and @atomicLoad(u8, &writer.done, .acquire) == 0)
        std.atomic.spinLoopHint();
    try std.testing.expectEqual(@as(u8, 1), @atomicLoad(u8, &contended, .acquire));
    try std.testing.expectEqual(@as(u8, 0), @atomicLoad(u8, &writer.done, .acquire));
    try std.testing.expectEqual(borrowed, spaces.extra_slots);
    try std.testing.expectEqual(@intFromPtr(body), borrowed[0]);
    // Fixture-only observation under the leaf; production never takes PMM
    // locks here. Writer already allocated its draft before reporting contention.
    // The writer published contended only after finishing PMM allocation and
    // is blocked on our directory leaf. Inspect the fixture count directly:
    // taking P here would invent an AD -> P edge absent from production.
    try std.testing.expectEqual(before - 1, free_list.len);
    spaces.unlockDirectory(restore);
    held = false;
    thread.join();
    joined = true;
    try std.testing.expect(writer.ok);
    try std.testing.expectEqual(body, spaces.get(1).?);
    try std.testing.expectEqual(before, free_list.pageCount());
    try std.testing.expect(spaces.directory_storage.retired == null);
}

test "MMIO overlay AS directory retries actual PMM rejection without further growth" {
    const Backing = struct {
        var bytes: [4 * 1024 * 1024]u8 align(4096) = undefined;
    };
    const inline_spaces = try std.testing.allocator.alloc(UserAddressSpace, 1);
    defer std.testing.allocator.free(inline_spaces);
    var free_list: types.FreePageList = .{};
    const base = @intFromPtr(&Backing.bytes);
    try free_list.installRegions(&.{.{ .id = 96, .start = base, .end = (base) + (Backing.bytes.len / 4096) * 4096 }});
    try free_list.seedContiguousRange(base, Backing.bytes.len / 4096);
    var spaces: UserAddressSpaceTable = .{};
    spaces.initializeFreshStorage(inline_spaces);
    const body = spaces.ensureSlot(1, &free_list) orelse return error.TestUnexpectedResult;
    try std.testing.expectEqual(@as(usize, 1), free_list.range_len);
    var hold = try @import("dma_retention.zig").TestHold.init(base, 1);
    defer hold.end();
    try std.testing.expect(spaces.ensureCapacity(3, &free_list));
    try std.testing.expect(spaces.directory_storage.retired != null);
    const after_failure = free_list.pageCount();
    const capacity = spaces.capacity();
    const directory = spaces.extra_slots;
    // A retry while still full must finish once, retain ownership, and return.
    var other_issuer: types.FreePageList = .{};
    try std.testing.expect(spaces.ensureCapacity(3, &other_issuer));
    try std.testing.expect(spaces.directory_storage.retired != null);
    hold.end();
    try std.testing.expect(spaces.ensureCapacity(3, &other_issuer));
    try std.testing.expect(spaces.directory_storage.retired == null);
    try std.testing.expectEqual(after_failure + 1, free_list.pageCount());
    try std.testing.expectEqual(@as(usize, 0), other_issuer.pageCount());
    try std.testing.expectEqual(capacity, spaces.capacity());
    try std.testing.expectEqual(directory, spaces.extra_slots);
    try std.testing.expectEqual(body, spaces.get(1).?);
    var returned = false;
    for (0..free_list.rangeCount()) |range_index| {
        const range = free_list.rangeAt(range_index);
        if (range.physical_start == base) {
            try std.testing.expectEqual(@as(u64, 96), range.region_id);
            try std.testing.expectEqual(@as(usize, 1), range.len);
            returned = true;
        }
    }
    try std.testing.expect(returned);
    try std.testing.expect(spaces.ensureCapacity(3, &other_issuer));
    try std.testing.expectEqual(after_failure + 1, free_list.pageCount());
}

test "MMIO overlay AS body loan spans directory publication withdrawal and original grant retry" {
    const Backing = struct {
        var bytes: [128 * 4096]u8 align(4096) = undefined;
    };
    var free_list: types.FreePageList = .{};
    const base = @intFromPtr(&Backing.bytes);
    try free_list.installRegions(&.{.{ .id = 97, .start = base, .end = base + Backing.bytes.len }});
    try free_list.seedContiguousRange(base, Backing.bytes.len / 4096);
    var spaces: UserAddressSpaceTable = .{};
    var inline_spaces: [1]UserAddressSpace = .{.{}};
    spaces.initializeFreshStorage(&inline_spaces);
    // The real allocator retains the original region-qualified extent.
    const body = spaces.ensureSlot(1, &free_list) orelse return error.TestUnexpectedResult;
    const loan = spaces.borrow(1).?;
    spaces.retire(1);
    try std.testing.expect(spaces.borrow(1) == null);
    try std.testing.expect(!spaces.loansEnded(body));
    try std.testing.expect(spaces.ensureSlot(1, &free_list) == null);
    spaces.endBorrow(loan);
    try std.testing.expect(spaces.loansEnded(body));
    const gap = try free_list.popContiguousOwnedInRange(1, base, base + Backing.bytes.len);
    const before = free_list.pageCount();
    const grant = spaces.body_storage.published.?.allocation;
    var hold = try @import("dma_retention.zig").TestHold.init(grant.physical.toInt(), grant.pages);
    defer hold.end();
    spaces.reclaimRetiredBody(body);
    try std.testing.expect(spaces.get(1) == null);
    try std.testing.expect(spaces.body_storage.retired != null);
    spaces.body_storage.reclaimRetiredAfterBorrowExclusion() catch unreachable;
    try std.testing.expect(spaces.body_storage.retired != null);
    hold.end();
    spaces.body_storage.reclaimRetiredAfterBorrowExclusion() catch unreachable;
    try std.testing.expect(spaces.body_storage.retired == null);
    try std.testing.expectEqual(before + try owned_storage.requiredPages(UserAddressSpace, 1), free_list.pageCount());
    try free_list.appendPage(gap.physical.toInt());
    spaces.directory_storage.retireAll() catch unreachable;
    spaces.directory_storage.reclaimRetiredAfterBorrowExclusion() catch unreachable;
    try std.testing.expectEqual(@as(usize, Backing.bytes.len / 4096), free_list.pageCount());
}

test "MMIO overlay AS withdrawal waits for a concurrent CR3 reader loan" {
    var inline_spaces: [1]UserAddressSpace = .{.{ .cr3 = 0x1234000 }};
    var spaces: UserAddressSpaceTable = .{};
    spaces.initializeFreshStorage(&inline_spaces);
    const Reader = struct {
        table: *UserAddressSpaceTable,
        ready: u8 = 0,
        proceed: u8 = 0,
        observed: u64 = 0,
        fn run(self: *@This()) void {
            if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(1);
            const body = self.table.borrow(0).?;
            @atomicStore(u8, &self.ready, 1, .release);
            while (@atomicLoad(u8, &self.proceed, .acquire) == 0) std.atomic.spinLoopHint();
            self.observed = body.cr3;
            self.table.endBorrow(body);
        }
    };
    var reader = Reader{ .table = &spaces };
    const thread = try std.Thread.spawn(.{}, Reader.run, .{&reader});
    var joined = false;
    defer if (!joined) {
        @atomicStore(u8, &reader.proceed, 1, .release);
        thread.join();
    };
    while (@atomicLoad(u8, &reader.ready, .acquire) == 0) std.atomic.spinLoopHint();
    spaces.retire(0);
    try std.testing.expect(spaces.borrow(0) == null);
    try std.testing.expect(!spaces.loansEnded(&inline_spaces[0]));
    @atomicStore(u8, &reader.proceed, 1, .release);
    thread.join();
    joined = true;
    try std.testing.expectEqual(@as(u64, 0x1234000), reader.observed);
    try std.testing.expect(spaces.loansEnded(&inline_spaces[0]));
}

test "MMIO overlay fresh AS initialization does not depend on zero BSS" {
    var space: UserAddressSpace = undefined;
    @memset(std.mem.asBytes(&space), 0xa5);
    space.lock_state = .{};
    initializeUserAddressSpaceStorage(&space);
    try std.testing.expectEqual(@as(usize, 0), space.borrowers);
    try std.testing.expect(!space.retiring);
    try std.testing.expect(space.retired_next == null);
    try std.testing.expectEqual(@as(u64, 0), space.reservations.live);
}

test "IPC endpoint AS live pin delays retirement across two host threads" {
    var spaces: [1]UserAddressSpace = .{.{}};
    var directory: UserAddressSpaceTable = .{};
    directory.initializeFreshStorage(&spaces);
    if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(0);
    const pin = directory.pinFast(0).?;
    const Worker = struct {
        table: *UserAddressSpaceTable,
        closed: std.atomic.Value(bool) = .init(false),
        proceed: std.atomic.Value(bool) = .init(false),
        fn run(w: *@This()) void {
            if (@hasDecl(@import("root"), "setTestLockDiagnosticsCpu")) @import("root").setTestLockDiagnosticsCpu(1);
            w.table.retire(0);
            w.closed.store(true, .release);
            while (!w.proceed.load(.acquire)) std.atomic.spinLoopHint();
            std.debug.assert(w.table.loansEnded(&w.table.inline_spaces[0]));
            w.table.reclaimRetiredBody(&w.table.inline_spaces[0]);
        }
    };
    var worker: Worker = .{ .table = &directory };
    const thread = try std.Thread.spawn(.{}, Worker.run, .{&worker});
    while (!worker.closed.load(.acquire)) std.atomic.spinLoopHint();
    try std.testing.expect(directory.pinFast(0) == null);
    try std.testing.expect(!directory.loansEnded(pin.space));
    try std.testing.expectError(error.PinsOutstanding, pin.space.fast_live.reclaimable());
    pin.end();
    worker.proceed.store(true, .release);
    thread.join();
    try std.testing.expect(directory.retired_head == null);
    try std.testing.expect(directory.pinFast(0) == null);
    var pages: types.FreePageList = .{};
    try std.testing.expectEqual(&spaces[0], directory.ensureSlot(0, &pages).?);
    const replacement = directory.pinFast(0).?;
    replacement.end();
}
