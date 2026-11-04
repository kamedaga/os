const std = @import("std");
const kernel = @import("kernel.zig");
const scheduler = @import("scheduler.zig").connection;
const fixture = scheduler.PiTestSupport;
const abi = @import("kernel_abi_root").runtime_abi;
const sc = @import("syscall/numbers.zig");
const TrapFrame = @import("interrupts.zig").TrapFrame;
const owner: kernel.PrincipalId = @enumFromInt(fixture.key.principal);

fn Probe(comptime Front: type) type {
    return struct {
        var locked = false;
        var reads: usize = 0;
        var exchanges: usize = 0;
        var wakes: usize = 0;
        var resident = true;
        var writable = true;

        fn lock(proc: kernel.PrincipalId) bool {
            std.debug.assert(proc == owner and Front.locked() and !locked);
            locked = true;
            return true;
        }
        fn unlock(proc: kernel.PrincipalId) void {
            std.debug.assert(proc == owner and Front.locked() and locked);
            locked = false;
        }
        fn read(proc: kernel.PrincipalId, address: u64) ?u32 {
            std.debug.assert(proc == owner and address == fixture.key.address);
            std.debug.assert(locked and Front.locked());
            reads += 1;
            return if (resident) fixture.word else null;
        }
        fn cas(proc: kernel.PrincipalId, address: u64, expected: u32, desired: u32) ?u32 {
            std.debug.assert(locked and Front.locked());
            exchanges += 1;
            return if (writable) fixture.cas(proc, address, expected, desired) else null;
        }
        fn wake(_: usize, _: u32, _: kernel.PrincipalId, _: u64, _: u64) scheduler.KeyedWakeResult {
            std.debug.assert(Front.locked());
            wakes += 1;
            return .woke;
        }
        fn hooks() @TypeOf(.{
            .futex_lock_address_space = @as(?*const fn (kernel.PrincipalId) bool, lock),
            .futex_unlock_address_space = @as(?*const fn (kernel.PrincipalId) void, unlock),
            .resident_read_user_u32_with_address_space_locked = @as(?*const fn (kernel.PrincipalId, u64) ?u32, read),
            .resident_cmpxchg_user_u32_with_address_space_locked = @as(?scheduler.PiCas, cas),
        }) {
            return .{
                .futex_lock_address_space = lock,
                .futex_unlock_address_space = unlock,
                .resident_read_user_u32_with_address_space_locked = read,
                .resident_cmpxchg_user_u32_with_address_space_locked = cas,
            };
        }
        fn reset() void {
            locked = false;
            reads = 0;
            exchanges = 0;
            wakes = 0;
            resident = true;
            writable = true;
        }
    };
}

fn expectDone(actual: scheduler.FutexPiAttempt, expected: abi.FutexPiResult) !void {
    try std.testing.expect(actual == .done);
    try std.testing.expectEqual(expected, actual.done);
}

// No page-table or PMM fixture is needed: these tests exercise actual VMA
// authority and scheduler admission while replacing only resident RAM access.
pub fn mappings(comptime Front: type) !void {
    const P = Probe(Front);
    try fixture.start();
    defer fixture.end();
    Front.reset();
    defer Front.reset();
    P.reset();
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    state.* = .{};
    var references: @import("state/types.zig").VmaReferences = .{};
    var entry: kernel.VmaEntry = .{
        .active = true,
        .start_va = fixture.key.address,
        .size_bytes = 4096,
        .prot = .{ .read = true, .write = true },
        .flags = .{ .private = true },
        .vmo = .{ .index = fixture.key.backing_index, .generation = fixture.key.backing_generation },
        .references = &references,
    };
    state.vma_tables[kernel.processIndexFromPrincipal(owner).?].address_index.root = &entry;
    var frame: TrapFrame = std.mem.zeroes(TrapFrame);
    frame.rdi = fixture.key.address;
    for ([_]bool{ false, true }) |shared| {
        entry.flags = .{ .shared = shared, .private = !shared };
        for ([_]u64{ 0, abi.futex_pi_private_key }) |flags| {
            frame.r10 = flags;
            fixture.word = 0;
            P.reset();
            const rejected = shared and flags == 0;
            try expectDone(Front.attempt(P.hooks(), state, owner, &frame, .try_lock), if (rejected) .unsupported else .acquired);
            try std.testing.expectEqual(@as(usize, if (rejected) 0 else 1), P.reads);
            try std.testing.expectEqual(@as(usize, if (rejected) 0 else 1), P.exchanges);
            try std.testing.expectEqual(@as(u32, if (rejected) 0 else 5), fixture.word);
            try std.testing.expect(!P.locked and !Front.locked());
        }
    }
    entry.flags = .{ .private = true };
    frame.r10 = abi.futex_pi_private_key;
    for (0..3) |scenario| {
        entry.device_backing = scenario == 0;
        entry.prot.write = scenario != 1;
        frame.rdi = if (scenario == 2) fixture.key.address + 4096 else fixture.key.address;
        fixture.word = 0;
        P.reset();
        try expectDone(Front.attempt(P.hooks(), state, owner, &frame, .lock), .bad_memory);
        try std.testing.expect(P.reads == 0 and P.exchanges == 0 and fixture.word == 0);
        try std.testing.expect(!P.locked and !Front.locked());
    }
    entry.device_backing = false;
    entry.prot.write = true;
    frame.rdi = fixture.key.address;
    for ([_]bool{ false, true }) |readable| {
        P.reset();
        P.resident = readable;
        P.writable = false;
        try expectDone(Front.attempt(P.hooks(), state, owner, &frame, .try_lock), .bad_memory);
        try std.testing.expectEqual(@as(usize, if (readable) 1 else 0), P.exchanges);
        try std.testing.expectEqual(@as(u32, 0), fixture.word);
        try std.testing.expect(!P.locked and !Front.locked());
    }

    // A stale ordinary registration cannot mask a valid PI probe. A current
    // token must reject it before reading or changing the ownership word.
    P.reset();
    Front.normal(0, .{ .active = true, .principal = owner, .thread_index = 4, .thread_generation = 17, .user_va = fixture.key.address, .wait_token = 999 });
    try expectDone(Front.attempt(P.hooks(), state, owner, &frame, .try_lock), .acquired);
    try std.testing.expectEqual(@as(usize, 1), P.exchanges);
    const token = scheduler.reserveCurrentWaitToken(17).?;
    defer scheduler.cancelCurrentWaitToken(17, token);
    Front.normal(0, .{ .active = true, .principal = owner, .thread_index = 4, .thread_generation = 17, .user_va = fixture.key.address, .wait_token = token });
    P.reset();
    const word_before = fixture.word;
    try expectDone(Front.attempt(P.hooks(), state, owner, &frame, .lock), .invalid);
    try std.testing.expect(P.reads == 0 and P.exchanges == 0 and fixture.word == word_before);
    try std.testing.expect(Front.normalValue(0).active);
    try std.testing.expect(scheduler.waitTokenIsCurrent(owner, 4, 17, token));
    try std.testing.expect(!P.locked and !Front.locked());
}

pub fn mixed(comptime Front: type) !void {
    const P = Probe(Front);
    try fixture.start();
    defer fixture.end();
    Front.reset();
    defer Front.reset();
    P.reset();
    const token = try fixture.queue(5);
    const word_before = fixture.word;
    const address = fixture.key.address;
    Front.normal(0, .{ .active = true, .principal = owner, .thread_index = 6, .thread_generation = 17, .user_va = address + 4096, .wait_token = 99 });
    const negative_invalid = 0 -% sc.syscall_err_invalid;
    try std.testing.expectEqual(negative_invalid, Front.wake(P.wake, owner, address, 1));
    try std.testing.expectEqual(negative_invalid, Front.requeue(P.wake, owner, address, 1, 1, address + 4096));
    try std.testing.expectEqual(negative_invalid, Front.requeue(P.wake, owner, address + 4096, 1, 1, address));
    try std.testing.expectEqual(@as(usize, 0), P.wakes);
    try std.testing.expectEqual(word_before, fixture.word);
    const normal = Front.normalValue(0);
    try std.testing.expect(normal.active and normal.user_va == address + 4096 and normal.wait_token == 99);
    try std.testing.expect(scheduler.waitTokenIsCurrent(owner, 5, 17, token));
    try std.testing.expect(scheduler.futexPiHasAddress(owner, address));
    // A legitimate positive count of one must remain distinguishable from
    // native INVALID=1, even while an unrelated PI queue exists.
    try std.testing.expectEqual(@as(u64, 1), Front.wake(P.wake, owner, address + 4096, 1));
    try std.testing.expectEqual(@as(usize, 1), P.wakes);
    try std.testing.expect(!Front.normalValue(0).active and !Front.locked());
    try std.testing.expect(scheduler.waitTokenIsCurrent(owner, 5, 17, token));
}

pub fn identity(comptime Front: type) !void {
    const P = Probe(Front);
    try fixture.start();
    defer fixture.end();
    Front.reset();
    defer Front.reset();
    const token = try fixture.queue(5);
    // Select the real owner without consuming the queue or performing a CAS.
    try expectDone(fixture.operation(4, .try_lock), .deadlock);
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    state.* = .{};
    var references: @import("state/types.zig").VmaReferences = .{};
    var entry: kernel.VmaEntry = .{
        .active = true,
        .start_va = fixture.key.address,
        .size_bytes = 4096,
        .prot = .{ .read = true, .write = true },
        .flags = .{ .private = true },
        .vmo = .{ .index = fixture.key.backing_index, .generation = fixture.key.backing_generation },
        .vmo_offset = fixture.key.backing_offset,
        .references = &references,
    };
    state.vma_tables[kernel.processIndexFromPrincipal(owner).?].address_index.root = &entry;
    var frame: TrapFrame = std.mem.zeroes(TrapFrame);
    frame.rdi = fixture.key.address;
    frame.r10 = abi.futex_pi_private_key;
    const word_before = fixture.word;
    for (0..3) |change| {
        entry.vmo.index = fixture.key.backing_index + @as(u32, if (change == 0) 1 else 0);
        entry.vmo.generation = fixture.key.backing_generation + @as(u32, if (change == 1) 1 else 0);
        entry.vmo_offset = fixture.key.backing_offset + @as(u64, if (change == 2) 4096 else 0);
        P.reset();
        try expectDone(Front.attempt(P.hooks(), state, owner, &frame, .unlock), .invalid);
        try std.testing.expect(P.reads == 1 and P.exchanges == 0 and fixture.word == word_before);
        try std.testing.expect(scheduler.waitTokenIsCurrent(owner, 5, 17, token));
        try std.testing.expect(scheduler.futexPiHasAddress(owner, fixture.key.address));
        try std.testing.expect(!P.locked and !Front.locked());
    }
    // Returning to the original identity permits the real scheduler handoff.
    entry.vmo_offset = fixture.key.backing_offset;
    P.reset();
    try expectDone(Front.attempt(P.hooks(), state, owner, &frame, .unlock), .acquired);
    try std.testing.expectEqual(@as(usize, 1), P.exchanges);
    try std.testing.expectEqual(@as(u32, 6), fixture.word);
    try std.testing.expect(!scheduler.futexPiHasAddress(owner, fixture.key.address));
    // Completion keeps the scalar token until the waiter consumes its result;
    // it must nevertheless be impossible to claim that completion twice.
    try std.testing.expectEqual(scheduler.WaitTokenClaimResult.stale, scheduler.claimWaitTokenCompletion(5, 17, owner, token));
    try std.testing.expect(!P.locked and !Front.locked());
}

pub fn discardIdentity(comptime Front: type) !void {
    const retire = @import("memory/discard_pages.zig");
    const discard = @import("state/discard.zig");
    const storage = @import("memory/owned_storage.zig");
    const backing = @import("memory/vmo_backing.zig");
    try fixture.start();
    defer fixture.end();
    Front.reset();
    defer Front.reset();
    const token = try fixture.queue(5);
    try expectDone(fixture.operation(4, .try_lock), .deadlock);
    const word = fixture.word;
    Front.normal(0, .{ .active = true, .principal = owner, .thread_index = 6, .thread_generation = 17, .user_va = fixture.key.address + 4096, .wait_token = 99 });
    const normal = Front.normalValue(0);
    var issuer: storage.TestAllocator = .{};
    defer issuer.deinit();
    var store: backing.Store = .{};
    const ref = store.init(17);
    defer std.debug.assert(store.clear(ref));
    try store.prepare(ref, 0, issuer.interface());
    store.putLocked(0, 4096);
    const Return = struct {
        fn give(_: *anyopaque, _: u64) bool { return true; }
        fn invalidate(_: *anyopaque, _: u64, _: u64) bool { return true; }
    };
    var context: u8 = 0;
    const batch = try retire.prepare(issuer.interface(), &context, Return.give, ref, 1, &.{.{ .index = 0, .paddr = 4096 }});
    var plan: discard.Plan = .{ .head = batch, .start = fixture.key.address, .prefix_end = fixture.key.address + 4096, .end = fixture.key.address + 4096 };
    try plan.commit(@as(*anyopaque, &context), Return.invalidate);
    var queue: retire.Queue = .{};
    plan.retireBatches(&queue);
    _ = queue.reclaim();
    try std.testing.expectEqual(@as(u64, 0), store.getLocked(0));
    // The actual detach/retire helpers never wake, requeue or rewrite keys.
    // Both the ordinary registration and PI ownership queue remain intact.
    try std.testing.expectEqualDeep(normal, Front.normalValue(0));
    try std.testing.expect(scheduler.waitTokenIsCurrent(owner, 5, 17, token));
    try std.testing.expect(scheduler.futexPiHasAddress(owner, fixture.key.address));
    try std.testing.expectEqual(word, fixture.word);
    try expectDone(fixture.operation(4, .try_lock), .deadlock);
}
