const std = @import("std");

/// Physical RAM policy, not a mapping-local PAT preference. UC_MINUS is
/// represented separately because Linux CPA uses it between WB and WC.
pub const Mode = enum { wb, uc, uc_minus, wc };

/// Execute a preflighted physical-page transition while the caller holds the
/// outer state/shared-VM/all-address-space locks. Those locks prevent faults
/// and copy windows from publishing aliases, but do not stop existing user
/// instructions. Hence *all* old aliases must be withdrawn before any CPU
/// acknowledges the cache drain, and none may be republished until it ends.
///
/// prepare must reserve page tables/metadata and validate canonical ownership,
/// every alias, DMA exclusion and PAT/MTRR evidence without changing mappings.
/// Its error path owns reservation rollback. After it returns, the remaining
/// operations must not fail or allocate: partial publication cannot safely be
/// rolled back by restoring one alias at a time.
///
/// The architecture implementation must include supervisor identity mappings
/// and temporary copy windows in withdrawal. flush_translations waits for all
/// CPU acknowledgements; drain_caches performs the required all-CPU cache
/// operation, not just a compiler or memory-ordering barrier.
pub fn run(backend: anytype, target: Mode) !void {
    var prepared = try backend.prepare(target);
    defer backend.release(&prepared);
    if (!prepared.changed) return;

    // Copy-window readers do not take VM locks. Block new aliases before
    // withdrawal and retain every snapshot until readers have acknowledged
    // a rendezvous after its replacement, including the final replacement.
    backend.publish_pending(&prepared);
    backend.withdraw_all(&prepared);
    backend.flush_translations(&prepared);
    backend.drain_caches(&prepared);
    backend.commit_policy(&prepared, target);
    backend.publish_all(&prepared);
    backend.flush_translations(&prepared);
    backend.publish_final(&prepared);
    backend.reader_rendezvous(&prepared);
}

const Fixture = struct {
    const Prepared = struct { changed: bool };
    mode: Mode = .wb,
    // Parent, exported page view, supervisor identity, and copy window.
    aliases: [4]?Mode = .{ .wb, .wb, .wb, .wb },
    events: [11]u8 = undefined,
    event_count: usize = 0,
    failed_preflight: bool = false,
    reservation: bool = false,
    invalidated: bool = false,
    drained: bool = false,
    pending: bool = false,
    readers_retired: bool = true,

    fn event(self: *Fixture, value: u8) void {
        self.events[self.event_count] = value;
        self.event_count += 1;
    }

    fn prepare(self: *Fixture, target: Mode) !Prepared {
        self.event('p');
        if (self.failed_preflight) return error.OutOfMemory;
        self.reservation = true;
        return .{ .changed = self.mode != target };
    }

    fn release(self: *Fixture, _: *Prepared) void {
        std.debug.assert(self.reservation);
        std.debug.assert(!self.pending and self.readers_retired);
        self.reservation = false;
        self.event('r');
    }

    fn publish_pending(self: *Fixture, _: *Prepared) void {
        self.pending = true;
        self.readers_retired = false;
        self.event('b');
    }

    fn publish_final(self: *Fixture, _: *Prepared) void {
        std.debug.assert(self.pending and self.invalidated);
        self.pending = false;
        self.event('f');
    }

    fn reader_rendezvous(self: *Fixture, _: *Prepared) void {
        std.debug.assert(!self.pending);
        self.readers_retired = true;
        self.event('q');
    }

    fn withdraw_all(self: *Fixture, _: *Prepared) void {
        std.debug.assert(self.pending);
        self.aliases = .{ null, null, null, null };
        self.invalidated = false;
        self.drained = false;
        self.event('w');
    }

    fn flush_translations(self: *Fixture, _: *Prepared) void {
        self.invalidated = true;
        self.event('t');
    }

    fn drain_caches(self: *Fixture, _: *Prepared) void {
        std.debug.assert(self.invalidated);
        for (self.aliases) |alias| std.debug.assert(alias == null);
        self.drained = true;
        self.event('d');
    }

    fn commit_policy(self: *Fixture, _: *Prepared, target: Mode) void {
        std.debug.assert(self.drained);
        self.mode = target;
        self.event('c');
    }

    fn publish_all(self: *Fixture, _: *Prepared) void {
        std.debug.assert(self.drained and self.pending);
        self.invalidated = false;
        for (&self.aliases) |*alias| alias.* = self.mode;
        self.event('a');
    }
};

test "all aliases are withdrawn before drain and republished with one policy" {
    var fixture: Fixture = .{};
    for ([_]Mode{ .uc_minus, .wc, .wb, .uc, .wb }) |target| {
        fixture.event_count = 0;
        try run(&fixture, target);
        try std.testing.expectEqualStrings("pbwtdcatfqr", fixture.events[0..fixture.event_count]);
        try std.testing.expect(!fixture.reservation);
        for (fixture.aliases) |alias| try std.testing.expectEqual(target, alias.?);
    }
}

test "preflight failure leaves all old aliases and policy untouched" {
    var fixture: Fixture = .{ .failed_preflight = true };
    try std.testing.expectError(error.OutOfMemory, run(&fixture, .wc));
    try std.testing.expectEqualStrings("p", fixture.events[0..fixture.event_count]);
    try std.testing.expectEqual(Mode.wb, fixture.mode);
    try std.testing.expect(!fixture.reservation);
    for (fixture.aliases) |alias| try std.testing.expectEqual(Mode.wb, alias.?);
}

test "unchanged policy needs no alias withdrawal or hardware flush" {
    var fixture: Fixture = .{};
    try run(&fixture, .wb);
    try std.testing.expectEqualStrings("pr", fixture.events[0..fixture.event_count]);
    try std.testing.expect(!fixture.reservation);
}
