//! Boot-only storage before AP user dispatch, not a runtime heap. The boot
//! sequence serializes callers. Temporary allocations have exclusive loans
//! and LIFO reclaim; persistent publication transfers a kernel-lifetime view.
//! Runtime checks reject copied active loans/stale tickets, not escaped Zig
//! slices or language-level copies. Callers must end all temporary borrows.
const std = @import("std");
pub const Error = error{ InvalidState, AlreadyInstalled, InvalidSize, OutOfMemory, Borrowed, OutOfOrder, Sealed, Unpublished };
const Lifetime = enum { temporary, persistent };
const no_previous = std.math.maxInt(usize);
const Header = struct {
    previous: usize,
    end: usize,
    length: usize,
    id: u64,
    lifetime: Lifetime,
    published: bool,
    borrowed: bool,
};
var empty_scratch: [0]u8 align(4096) = .{};

pub const Growth = struct {
    context: *anyopaque,
    // Map a longer prefix at the SAME VA. On failure, any newly reserved
    // backing stays owned by the backend; no allocation ticket is published.
    ensure: *const fn (*anyopaque, usize) ?[]align(4096) u8,
};

pub const Arena = struct {
    growth: ?Growth = null,
    buffer: []align(4096) u8 = empty_scratch[0..],
    used: usize = 0,
    high_water: usize = 0,
    persistent_payload: usize = 0,
    tail_address: usize = 0,
    tail_bytes: usize = 0,
    reclaim_started: bool = false,
    reclaimed: bool = false,
    head: ?usize = null,
    next_id: u64 = 1,
    bound_address: ?*Arena = null,
    installed: bool = false,
    sealed: bool = false,

    fn check(self: *Arena) Error!void {
        // Identity precedes header access: a copy does not inherit ownership.
        if (!self.installed or self.bound_address != self) return error.InvalidState;
    }
    pub fn install(self: *Arena, buffer: []align(4096) u8) Error!void {
        if (self.bound_address != null and self.bound_address != self) return error.InvalidState;
        // Even used==0 is not a reset boundary: stale tickets may still exist.
        if (self.installed) return error.AlreadyInstalled;
        if (buffer.len % 4096 != 0) return error.InvalidSize;
        self.buffer = buffer;
        self.bound_address = self;
        self.installed = true;
    }
    pub fn installGrowing(self: *Arena, growth: Growth) Error!void {
        const initial = growth.ensure(growth.context, 0) orelse return error.OutOfMemory;
        try self.install(initial);
        self.growth = growth;
    }
    fn headerAt(self: *Arena, offset: usize) Error!*Header {
        if (self.used > self.buffer.len or offset % @alignOf(Header) != 0 or self.used < @sizeOf(Header) or
            offset > self.used - @sizeOf(Header)) return error.InvalidState;
        const header: *Header = @ptrCast(@alignCast(self.buffer.ptr + offset));
        if (header.end > self.used or header.end < offset + @sizeOf(Header) or
            header.length > header.end - offset - @sizeOf(Header) or
            (header.previous != no_previous and header.previous >= offset)) return error.InvalidState;
        return header;
    }
    fn record(self: *Arena, offset: usize, id: u64) Error!*Header {
        try self.check();
        // Validate membership in the live chain BEFORE accessing a ticket's
        // header: its old bytes may already belong to a different allocation.
        var cursor = self.head;
        while (cursor) |current| {
            const header = try self.headerAt(current);
            if (current == offset) {
                if (header.id != id) return error.InvalidState;
                return header;
            }
            cursor = if (header.previous == no_previous) null else header.previous;
        }
        return error.InvalidState;
    }
    fn reserve(self: *Arena, bytes: usize, lifetime: Lifetime) Error!Allocation {
        try self.check();
        if (self.sealed) return error.Sealed;
        if (bytes == 0) return error.InvalidSize;
        if (lifetime == .persistent) {
            // Publication must not pin an older cancellable draft below it.
            // Finish temporary staging and publish/cancel any existing draft
            // before reserving another kernel-lifetime catalog.
            var cursor = self.head;
            while (cursor) |offset| {
                const header = try self.headerAt(offset);
                if (header.lifetime == .temporary or !header.published) return error.Unpublished;
                cursor = if (header.previous == no_previous) null else header.previous;
            }
        }
        const rounded = std.math.add(usize, bytes, 7) catch return error.InvalidSize;
        const payload = rounded & ~@as(usize, 7);
        const start = std.math.add(usize, self.used, @sizeOf(Header)) catch return error.InvalidSize;
        const end = std.math.add(usize, start, payload) catch return error.InvalidSize;
        const next_id = std.math.add(u64, self.next_id, 1) catch return error.InvalidSize;
        if (end > self.buffer.len) {
            const growth = self.growth orelse return error.OutOfMemory;
            const expanded = growth.ensure(growth.context, end) orelse return error.OutOfMemory;
            // Published pointers and active loans must never move on growth.
            if (expanded.ptr != self.buffer.ptr or expanded.len < end or expanded.len % 4096 != 0)
                return error.InvalidState;
            self.buffer = expanded;
        }
        const offset = self.used;
        const header: *Header = @ptrCast(@alignCast(self.buffer.ptr + offset));
        header.* = .{ .previous = self.head orelse no_previous, .end = end, .length = bytes, .id = self.next_id, .lifetime = lifetime, .published = false, .borrowed = false };
        self.head = offset;
        self.used = end;
        self.high_water = @max(self.high_water, end);
        self.next_id = next_id;
        return .{ .arena = self, .offset = offset, .id = header.id };
    }
    pub fn reserveTemporary(self: *Arena, bytes: usize) Error!Allocation {
        return self.reserve(bytes, .temporary);
    }
    pub fn reservePersistent(self: *Arena, bytes: usize) Error!Allocation {
        return self.reserve(bytes, .persistent);
    }
    pub fn seal(self: *Arena) Error!void {
        try self.check();
        if (self.sealed) return error.Sealed;
        var payload: usize = 0;
        var cursor = self.head;
        while (cursor) |offset| {
            const header = try self.headerAt(offset);
            if (header.borrowed) return error.Borrowed;
            if (header.lifetime != .persistent or !header.published) return error.Unpublished;
            payload = std.math.add(usize, payload, header.length) catch return error.InvalidSize;
            cursor = if (header.previous == no_previous) null else header.previous;
        }
        const rounded = std.math.add(usize, self.used, 4095) catch return error.InvalidSize;
        const retained = rounded & ~@as(usize, 4095);
        if (retained > self.buffer.len) return error.InvalidState;
        const tail_address = std.math.add(usize, @intFromPtr(self.buffer.ptr), retained) catch return error.InvalidSize;
        // Poison is diagnostic evidence, not lifetime enforcement: the high
        // mapping must still be withdrawn and acknowledged before PMM reuse.
        const root = @import("root");
        const poison = if (@hasDecl(root, "boot_diagnostic_enabled")) root.boot_diagnostic_enabled else @import("builtin").is_test;
        if (poison) @memset(self.buffer[retained..], 0xa5);
        self.tail_address = tail_address;
        self.tail_bytes = self.buffer.len - retained;
        // Do not retain a slice exposing pages which are about to change owner.
        // Headers remain in the prefix; stale tickets traverse only that chain.
        self.buffer = self.buffer[0..retained];
        self.persistent_payload = payload;
        self.growth = null;
        self.sealed = true;
    }

    pub fn reclaim(self: *Arena, comptime Retirer: type, context: anytype) !void {
        try self.check();
        if (!self.sealed or self.reclaim_started) return error.InvalidState;
        self.reclaim_started = true;
        if (self.tail_bytes != 0) {
            const physical = try Retirer.physical(context, self.tail_address, self.tail_bytes);
            try Retirer.withdraw(context, self.tail_address, self.tail_bytes);
            try Retirer.acknowledge(context, self.tail_address, self.tail_bytes);
            // Every error retains ownership and the numeric extent. In boot a
            // failed transaction halts; never retry withdrawal or double-return.
            try Retirer.release(context, physical, self.tail_bytes / 4096);
        }
        self.reclaimed = true;
    }

    pub fn statistics(self: *const Arena) struct { used: usize, high_water: usize, persistent_payload: usize, retained: usize, tail_address: usize, tail_bytes: usize, reclaimed: bool } {
        return .{ .used = self.used, .high_water = self.high_water, .persistent_payload = self.persistent_payload, .retained = self.buffer.len, .tail_address = self.tail_address, .tail_bytes = self.tail_bytes, .reclaimed = self.reclaimed };
    }
};

pub const Allocation = struct {
    arena: *Arena,
    offset: usize,
    id: u64,

    pub fn borrowInto(self: Allocation, loan: *Borrow) Error!void {
        if (loan.bound_address != null or loan.allocation != null or loan.ended) return error.InvalidState;
        const header = try self.arena.record(self.offset, self.id);
        if (header.published) return error.InvalidState;
        if (header.borrowed) return error.Borrowed;
        header.borrowed = true;
        // Bind directly in caller storage, rather than returning a bound copy.
        // Loans on OTHER allocations remain independent (nested ELF staging).
        loan.* = .{ .allocation = self, .bound_address = loan };
    }
    pub fn release(self: Allocation) Error!void {
        const header = try self.arena.record(self.offset, self.id);
        if (header.borrowed) return error.Borrowed;
        if (header.published) return error.InvalidState;
        // Do not rewind through another allocation/persistent publication.
        // Non-LIFO failure retains explicit ownership rather than silently
        // pretending that free succeeded.
        if (self.arena.head != self.offset) return error.OutOfOrder;
        self.arena.head = if (header.previous == no_previous) null else header.previous;
        self.arena.used = self.offset;
    }
    pub fn publishPersistent(self: Allocation) Error![]align(8) u8 {
        const header = try self.arena.record(self.offset, self.id);
        if (header.lifetime != .persistent or header.published) return error.InvalidState;
        if (header.borrowed) return error.Borrowed;
        header.published = true;
        // Recipient subsystem gets the sole kernel-lifetime publication.
        // The allocator can neither lend nor reclaim these bytes afterward.
        const pointer: [*]align(8) u8 = @ptrCast(@alignCast(self.arena.buffer.ptr + self.offset + @sizeOf(Header)));
        return pointer[0..header.length];
    }
};

pub const Borrow = struct {
    allocation: ?Allocation = null,
    bound_address: ?*Borrow = null,
    ended: bool = false,

    fn record(self: *Borrow) Error!*Header {
        if (self.bound_address != self or self.ended) return error.InvalidState;
        const allocation = self.allocation orelse return error.InvalidState;
        const header = try allocation.arena.record(allocation.offset, allocation.id);
        if (!header.borrowed or header.published) return error.InvalidState;
        return header;
    }
    pub fn bytes(self: *Borrow) Error![]align(8) u8 {
        const header = try self.record();
        const allocation = self.allocation.?;
        const pointer: [*]align(8) u8 = @ptrCast(@alignCast(allocation.arena.buffer.ptr + allocation.offset + @sizeOf(Header)));
        return pointer[0..header.length];
    }
    pub fn end(self: *Borrow) Error!void {
        const header = try self.record();
        header.borrowed = false;
        self.ended = true;
    }
};

var arena: Arena = .{};
pub fn kernelStaticStorageStartAddr() usize {
    return @intFromPtr(&arena);
}
pub fn kernelStaticStorageEndAddr() usize {
    return @intFromPtr(&arena) + @sizeOf(Arena);
}
pub fn install(buffer: []align(4096) u8) Error!void {
    try arena.install(buffer);
}
pub fn installGrowing(growth: Growth) Error!void {
    try arena.installGrowing(growth);
}
pub fn reserveTemporary(bytes: usize) Error!Allocation {
    return arena.reserveTemporary(bytes);
}
pub fn reservePersistent(bytes: usize) Error!Allocation {
    return arena.reservePersistent(bytes);
}
pub fn seal() Error!void {
    try arena.seal();
}
pub fn reclaim(comptime Retirer: type, context: anytype) !void {
    try arena.reclaim(Retirer, context);
}
pub fn statistics() @TypeOf(arena.statistics()) {
    return arena.statistics();
}

test "scratch nested temporary loans reclaim without invalidating parent" {
    var buffer: [4096]u8 align(4096) = undefined;
    var owner: Arena = .{};
    try owner.install(&buffer);
    const parent = try owner.reserveTemporary(17);
    var outer: Borrow = .{};
    try parent.borrowInto(&outer);
    @memset(try outer.bytes(), 0x71);
    const before = owner.used;
    const child = try owner.reserveTemporary(23);
    var inner: Borrow = .{};
    try child.borrowInto(&inner);
    try std.testing.expectError(error.Borrowed, parent.release());
    try inner.end();
    try child.release();
    try std.testing.expectEqual(before, owner.used);
    for (try outer.bytes()) |byte| try std.testing.expectEqual(@as(u8, 0x71), byte);
    try outer.end();
    try parent.release();
    try std.testing.expectEqual(@as(usize, 0), owner.used);
    try owner.seal();
}

test "scratch copied loan arena stale ticket and double finish are rejected" {
    var buffer: [4096]u8 align(4096) = undefined;
    var owner: Arena = .{};
    try owner.install(&buffer);
    var copied_owner = owner;
    try std.testing.expectError(error.InvalidState, copied_owner.reserveTemporary(8));
    const first = try owner.reserveTemporary(8);
    var loan: Borrow = .{};
    try first.borrowInto(&loan);
    try std.testing.expectError(error.InvalidState, first.borrowInto(&loan));
    var another: Borrow = .{};
    try std.testing.expectError(error.Borrowed, first.borrowInto(&another));
    var copied_loan = loan;
    try std.testing.expectError(error.InvalidState, copied_loan.bytes());
    try std.testing.expectError(error.InvalidState, copied_loan.end());
    try loan.end();
    try std.testing.expectError(error.InvalidState, loan.end());
    try first.release();
    try std.testing.expectError(error.InvalidState, first.release());
    const replacement = try owner.reserveTemporary(8);
    try std.testing.expectEqual(first.offset, replacement.offset);
    try std.testing.expectError(error.InvalidState, first.borrowInto(&another));
    try replacement.release();
}

test "scratch non LIFO and published release retain ownership" {
    var buffer: [4096]u8 align(4096) = undefined;
    var owner: Arena = .{};
    try owner.install(&buffer);
    const first = try owner.reserveTemporary(7);
    const second = try owner.reserveTemporary(11);
    const used = owner.used;
    try std.testing.expectError(error.OutOfOrder, first.release());
    try std.testing.expectEqual(used, owner.used);
    try second.release();
    try first.release();
    const persistent = try owner.reservePersistent(19);
    var loan: Borrow = .{};
    try persistent.borrowInto(&loan);
    try std.testing.expectError(error.Borrowed, persistent.publishPersistent());
    @memset(try loan.bytes(), 0x42);
    try loan.end();
    const view = try persistent.publishPersistent();
    try std.testing.expectError(error.InvalidState, persistent.release());
    var invalid_loan: Borrow = .{};
    try std.testing.expectError(error.InvalidState, persistent.borrowInto(&invalid_loan));
    try owner.seal();
    for (view) |byte| try std.testing.expectEqual(@as(u8, 0x42), byte);
}

test "scratch install and sealing enforce every live lifetime" {
    var buffer: [4096]u8 align(4096) = undefined;
    var owner: Arena = .{};
    try owner.install(&buffer);
    try std.testing.expectError(error.AlreadyInstalled, owner.install(&buffer));
    const temp = try owner.reserveTemporary(8);
    var loan: Borrow = .{};
    try temp.borrowInto(&loan);
    try std.testing.expectError(error.Borrowed, owner.seal());
    try loan.end();
    try std.testing.expectError(error.Unpublished, owner.seal());
    try temp.release();
    const draft = try owner.reservePersistent(8);
    try std.testing.expectError(error.Unpublished, owner.seal());
    try draft.release();
    try owner.seal();
    try std.testing.expectError(error.Sealed, owner.reserveTemporary(8));
    try std.testing.expectError(error.Sealed, owner.reservePersistent(8));
    try std.testing.expectError(error.AlreadyInstalled, owner.install(&buffer));
}

test "scratch failed reservations preserve capacity and identity" {
    var buffer: [4096]u8 align(4096) = undefined;
    var owner: Arena = .{};
    try owner.install(&buffer);
    const first = try owner.reserveTemporary(16);
    const used = owner.used;
    const id = owner.next_id;
    try std.testing.expectError(error.InvalidSize, owner.reserveTemporary(0));
    try std.testing.expectError(error.InvalidSize, owner.reserveTemporary(std.math.maxInt(usize)));
    try std.testing.expectError(error.OutOfMemory, owner.reserveTemporary(buffer.len));
    try std.testing.expectEqual(used, owner.used);
    try std.testing.expectEqual(id, owner.next_id);
    owner.next_id = std.math.maxInt(u64);
    try std.testing.expectError(error.InvalidSize, owner.reserveTemporary(8));
    try std.testing.expectEqual(used, owner.used);
    try first.release();
}

test "scratch serial allocations have no record count pool" {
    var buffer: [4096]u8 align(4096) = undefined;
    var owner: Arena = .{};
    try owner.install(&buffer);
    for (0..2000) |_| {
        const allocation = try owner.reserveTemporary(8);
        try allocation.release();
    }
    try std.testing.expectEqual(@as(usize, 0), owner.used);
    try std.testing.expectEqual(@as(u64, 2001), owner.next_id);
}

test "scratch persistent reservation cannot pin an older temporary" {
    var buffer: [4096]u8 align(4096) = undefined;
    var owner: Arena = .{};
    try owner.install(&buffer);
    const temporary = try owner.reserveTemporary(8);
    const before = owner.used;
    try std.testing.expectError(error.Unpublished, owner.reservePersistent(8));
    try std.testing.expectEqual(before, owner.used);
    try temporary.release();
    const catalog = try owner.reservePersistent(8);
    _ = try catalog.publishPersistent();
    try owner.seal();
}

test "scratch persistent draft remains cancellable until publication" {
    var buffer: [4096]u8 align(4096) = undefined;
    var owner: Arena = .{};
    try owner.install(&buffer);
    const cancelled = try owner.reservePersistent(8);
    const used = owner.used;
    const id = owner.next_id;
    try std.testing.expectError(error.Unpublished, owner.reservePersistent(8));
    try std.testing.expectEqual(used, owner.used);
    try std.testing.expectEqual(id, owner.next_id);
    try cancelled.release();
    const published = try owner.reservePersistent(8);
    _ = try published.publishPersistent();
    const next = try owner.reservePersistent(8);
    try next.release();
    try owner.seal();
}

const TestRetirer = struct {
    fail_at: usize = 0,
    step: usize = 0,
    returned: usize = 0,
    start: usize = 0,
    bytes: usize = 0,
    fn advance(self: *@This(), expected: usize) !void {
        try std.testing.expectEqual(expected - 1, self.step);
        self.step = expected;
        if (self.fail_at == expected) return error.Injected;
    }
    pub fn physical(self: *@This(), va: usize, bytes: usize) !u64 {
        try self.advance(1);
        self.start = va;
        self.bytes = bytes;
        return 4096;
    }
    pub fn withdraw(self: *@This(), va: usize, bytes: usize) !void {
        try self.advance(2);
        try std.testing.expectEqual(self.start, va);
        try std.testing.expectEqual(self.bytes, bytes);
    }
    pub fn acknowledge(self: *@This(), va: usize, bytes: usize) !void {
        try self.advance(3);
        try std.testing.expectEqual(self.start, va);
        try std.testing.expectEqual(self.bytes, bytes);
    }
    pub fn release(self: *@This(), physical_start: u64, pages: usize) !void {
        try self.advance(4);
        try std.testing.expectEqual(@as(u64, 4096), physical_start);
        try std.testing.expectEqual(self.bytes / 4096, pages);
        self.returned = pages;
    }
};

test "scratch seal rounds only the retained prefix and poisons complete tail pages" {
    for ([_]usize{ 0, 1, 4055, 4056, 4057, 8152, 12248 }) |payload| {
        var buffer: [12288]u8 align(4096) = undefined;
        @memset(&buffer, 0x37);
        var owner: Arena = .{};
        try owner.install(&buffer);
        var view: []const u8 = &.{};
        if (payload != 0) {
            const allocation = try owner.reservePersistent(payload);
            var loan: Borrow = .{};
            try allocation.borrowInto(&loan);
            @memset(try loan.bytes(), 0x71);
            try loan.end();
            view = try allocation.publishPersistent();
        }
        const used = owner.used;
        const expected = (used + 4095) & ~@as(usize, 4095);
        const pointer = view.ptr;
        try owner.seal();
        try std.testing.expectEqual(expected, owner.buffer.len);
        try std.testing.expectEqual(@intFromPtr(&buffer) + expected, owner.tail_address);
        try std.testing.expectEqual(buffer.len - expected, owner.tail_bytes);
        try std.testing.expectEqual(used, owner.high_water);
        try std.testing.expect(view.ptr == pointer);
        for (view) |byte| try std.testing.expectEqual(@as(u8, 0x71), byte);
        for (buffer[expected..]) |byte| try std.testing.expectEqual(@as(u8, 0xa5), byte);
        var retirer = TestRetirer{};
        try owner.reclaim(TestRetirer, &retirer);
        try std.testing.expect(owner.reclaimed);
        try std.testing.expectEqual(owner.tail_bytes / 4096, retirer.returned);
        try std.testing.expectEqual(if (owner.tail_bytes == 0) @as(usize, 0) else 4, retirer.step);
        @memset(buffer[expected..], 0xdd); // new physical owner overwrites returned RAM
        for (view) |byte| try std.testing.expectEqual(@as(u8, 0x71), byte);
        try std.testing.expectError(error.InvalidState, owner.reclaim(TestRetirer, &retirer));
        try std.testing.expectError(error.Sealed, owner.reserveTemporary(8));
    }
}

test "scratch retirement rejects unfinished owners stale views and repeated failed transactions" {
    for (1..5) |failure| {
        var buffer: [8192]u8 align(4096) = undefined;
        var owner: Arena = .{};
        try owner.install(&buffer);
        const persistent = try owner.reservePersistent(24);
        _ = try persistent.publishPersistent();
        const temporary = try owner.reserveTemporary(128);
        var loan: Borrow = .{};
        try temporary.borrowInto(&loan);
        var retirer = TestRetirer{ .fail_at = failure };
        try std.testing.expectError(error.InvalidState, owner.reclaim(TestRetirer, &retirer));
        try std.testing.expectError(error.Borrowed, owner.seal());
        try loan.end();
        try std.testing.expectError(error.Unpublished, owner.seal());
        try temporary.release();
        try owner.seal();
        try std.testing.expectError(error.InvalidState, loan.bytes());
        var stale: Borrow = .{};
        try std.testing.expectError(error.InvalidState, temporary.borrowInto(&stale));
        try std.testing.expectError(error.Injected, owner.reclaim(TestRetirer, &retirer));
        try std.testing.expect(!owner.reclaimed and owner.reclaim_started);
        try std.testing.expectEqual(@as(usize, 4096), owner.tail_bytes);
        try std.testing.expectEqual(@as(usize, 0), retirer.returned);
        try std.testing.expectError(error.InvalidState, owner.reclaim(TestRetirer, &retirer));
    }
    var odd: [4097]u8 align(4096) = undefined;
    var owner: Arena = .{};
    try std.testing.expectError(error.InvalidSize, owner.install(&odd));
}

test "scratch page growth preserves published pointers loans and failure identity" {
    const Backend = struct {
        bytes: []align(4096) u8,
        fail: bool = false,
        mapped: usize = 0,
        fn ensure(context: *anyopaque, needed: usize) ?[]align(4096) u8 {
            const self: *@This() = @ptrCast(@alignCast(context));
            if (self.fail or needed > self.bytes.len) return null;
            self.mapped = std.mem.alignForward(usize, needed, 4096);
            return self.bytes[0..self.mapped];
        }
    };
    // Exceed the former 32 MiB static image backing without moving any view.
    const backing = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), 33 * 1024 * 1024);
    defer std.testing.allocator.free(backing);
    var backend = Backend{ .bytes = backing };
    var owner: Arena = .{};
    try owner.installGrowing(.{ .context = &backend, .ensure = Backend.ensure });
    const first = try owner.reservePersistent(17);
    const published = try first.publishPersistent();
    @memset(published, 0x71);
    const original = published.ptr;
    const parent = try owner.reserveTemporary(4096);
    var loan: Borrow = .{};
    try parent.borrowInto(&loan);
    @memset(try loan.bytes(), 0x42);
    const used = owner.used;
    const next_id = owner.next_id;
    backend.fail = true;
    try std.testing.expectError(error.OutOfMemory, owner.reserveTemporary(32 * 1024 * 1024));
    try std.testing.expectEqual(used, owner.used);
    try std.testing.expectEqual(next_id, owner.next_id);
    backend.fail = false;
    const child = try owner.reserveTemporary(32 * 1024 * 1024);
    try std.testing.expect(owner.buffer.len > 32 * 1024 * 1024);
    try std.testing.expect(published.ptr == original);
    for (published) |byte| try std.testing.expectEqual(@as(u8, 0x71), byte);
    for (try loan.bytes()) |byte| try std.testing.expectEqual(@as(u8, 0x42), byte);
    try child.release();
    try loan.end();
    try parent.release();
    try owner.seal();
    try std.testing.expect(owner.growth == null);
    try std.testing.expectEqual(@as(usize, 4096), owner.buffer.len);
    try std.testing.expect(owner.tail_bytes > 32 * 1024 * 1024);
}
