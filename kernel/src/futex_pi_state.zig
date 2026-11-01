//! Allocation-free PI ownership for the supported equal-priority domain.
//! Scheduler T excludes every reader/mutator, including timeout and teardown.
//! Caller owns stable Thread/Mutex storage and user-word CAS; this module never
//! accesses user memory, completes a wait, frees a record, or changes priority.
const std = @import("std");

pub const Identity = struct {
    principal: u64,
    index: u32,
    generation: u32,
};
pub const Key = struct {
    principal: u64,
    address: u64,
    backing_index: u32 = 0,
    backing_generation: u32 = 0,
    backing_offset: u64 = 0,
};
pub const Thread = struct {
    identity: Identity,
    owned_head: ?*Mutex = null,
    waiting_mutex: ?*Mutex = null,
    queue_prev: ?*Thread = null,
    queue_next: ?*Thread = null,
    wait_token: u64 = 0,

    /// Retirement must drain owned queues before this identity can be reused.
    pub fn reset(self: *Thread, identity: Identity) void {
        std.debug.assert(self.owned_head == null and self.waiting_mutex == null);
        std.debug.assert(self.queue_prev == null and self.queue_next == null and self.wait_token == 0);
        std.debug.assert(identity.generation != 0);
        self.* = .{ .identity = identity };
    }
};
pub const Mutex = struct {
    key: Key,
    owner: ?*Thread = null,
    owner_generation: u32 = 0,
    owned_prev: ?*Mutex = null,
    owned_next: ?*Mutex = null,
    hash_next: ?*Mutex = null,
    head: ?*Thread = null,
    tail: ?*Thread = null,
    attached: bool = false,
};
pub const Waiter = struct {
    thread: *Thread,
    identity: Identity,
    wait_token: u64,
};
pub const Cancel = struct {
    removed: bool = false,
    retired: ?*Mutex = null,
};
pub const Handoff = struct {
    waiter: Waiter,
    retired: ?*Mutex,
};
pub const AdmissionError = error{ InvalidIdentity, InvalidToken, AlreadyWaiting, AlreadyAttached, DuplicateKey, InvalidOwner, Deadlock };

pub const Manager = struct {
    pub const bucket_count = 64;
    buckets: [bucket_count]?*Mutex = .{null} ** bucket_count,

    fn bucket(key: Key) usize {
        // Buckets are only an index: collisions retain full Key equality and
        // unbounded caller-provided chains, never an admission capacity limit.
        var h = (key.address >> 2) ^ (key.principal *% 0x9e3779b97f4a7c15);
        h ^= h >> 32;
        return @intCast(h & (bucket_count - 1));
    }
    pub fn find(self: *const Manager, key: Key) ?*Mutex {
        var cursor = self.buckets[bucket(key)];
        while (cursor) |mutex| : (cursor = mutex.hash_next)
            if (std.meta.eql(mutex.key, key)) return mutex;
        return null;
    }
    pub fn findAddress(self: *const Manager, principal: u64, address: u64) ?*Mutex {
        var cursor = self.buckets[bucket(.{ .principal = principal, .address = address })];
        while (cursor) |mutex| : (cursor = mutex.hash_next)
            if (mutex.key.principal == principal and mutex.key.address == address) return mutex;
        return null;
    }
    fn ownerOf(mutex: *const Mutex) AdmissionError!*Thread {
        const owner = mutex.owner orelse return error.InvalidOwner;
        if (!mutex.attached or owner.identity.generation == 0 or
            mutex.owner_generation != owner.identity.generation or
            mutex.key.principal != owner.identity.principal) return error.InvalidOwner;
        return owner;
    }
    fn step(thread: *Thread) AdmissionError!?*Thread {
        return if (thread.waiting_mutex) |mutex| try ownerOf(mutex) else null;
    }
    pub fn check(owner: *Thread, waiter: *Thread, token: u64) AdmissionError!void {
        if (owner.identity.generation == 0 or waiter.identity.generation == 0 or
            owner.identity.principal != waiter.identity.principal) return error.InvalidIdentity;
        if (token == 0) return error.InvalidToken;
        if (waiter.waiting_mutex != null or waiter.queue_prev != null or waiter.queue_next != null or waiter.wait_token != 0)
            return error.AlreadyWaiting;
        // A thread has one outgoing wait edge. Floyd's walk also rejects an
        // already-cyclic input graph without allocating visited-node storage.
        var slow: ?*Thread = owner;
        var fast: ?*Thread = owner;
        while (slow) |current| {
            if (current == waiter) return error.Deadlock;
            slow = try step(current);
            if (fast) |f| fast = try step(f);
            if (fast) |f| fast = try step(f);
            if (slow != null and slow == fast) return error.Deadlock;
        }
    }
    fn linkOwner(mutex: *Mutex, owner: *Thread) void {
        mutex.owner = owner;
        mutex.owner_generation = owner.identity.generation;
        mutex.owned_prev = null;
        mutex.owned_next = owner.owned_head;
        if (owner.owned_head) |first| first.owned_prev = mutex;
        owner.owned_head = mutex;
    }
    fn unlinkOwner(mutex: *Mutex) void {
        const owner = mutex.owner.?;
        std.debug.assert(owner.identity.generation == mutex.owner_generation);
        if (mutex.owned_prev) |previous| previous.owned_next = mutex.owned_next else owner.owned_head = mutex.owned_next;
        if (mutex.owned_next) |next| next.owned_prev = mutex.owned_prev;
        mutex.owner = null;
        mutex.owner_generation = 0;
        mutex.owned_prev = null;
        mutex.owned_next = null;
    }
    fn append(mutex: *Mutex, waiter: *Thread, token: u64) void {
        waiter.waiting_mutex = mutex;
        waiter.wait_token = token;
        waiter.queue_prev = mutex.tail;
        waiter.queue_next = null;
        if (mutex.tail) |tail| tail.queue_next = waiter else mutex.head = waiter;
        mutex.tail = waiter;
    }
    fn remove(mutex: *Mutex, thread: *Thread) Waiter {
        std.debug.assert(thread.waiting_mutex == mutex and thread.wait_token != 0);
        const result: Waiter = .{ .thread = thread, .identity = thread.identity, .wait_token = thread.wait_token };
        if (thread.queue_prev) |previous| previous.queue_next = thread.queue_next else mutex.head = thread.queue_next;
        if (thread.queue_next) |next| next.queue_prev = thread.queue_prev else mutex.tail = thread.queue_prev;
        thread.waiting_mutex = null;
        thread.wait_token = 0;
        thread.queue_prev = null;
        thread.queue_next = null;
        return result;
    }
    fn detach(self: *Manager, mutex: *Mutex) void {
        std.debug.assert(mutex.attached);
        var link = &self.buckets[bucket(mutex.key)];
        while (link.* != mutex) link = &link.*.?.hash_next;
        link.* = mutex.hash_next;
        mutex.hash_next = null;
        mutex.attached = false;
        unlinkOwner(mutex);
    }
    /// Attach only together with the first waiter: uncontended ownership lives
    /// in the user word, so a successful fastpath needs no persistent record.
    /// All fallible checks precede mutation (caller can reserve storage first).
    pub fn attach(self: *Manager, mutex: *Mutex, owner: *Thread, waiter: *Thread, token: u64) AdmissionError!void {
        if (mutex.attached or mutex.owner != null or mutex.head != null or mutex.tail != null or
            mutex.owned_prev != null or mutex.owned_next != null or mutex.hash_next != null) return error.AlreadyAttached;
        if (mutex.key.principal != owner.identity.principal) return error.InvalidOwner;
        if (self.find(mutex.key) != null) return error.DuplicateKey;
        try check(owner, waiter, token);
        linkOwner(mutex, owner);
        append(mutex, waiter, token);
        const index = bucket(mutex.key);
        mutex.hash_next = self.buckets[index];
        self.buckets[index] = mutex;
        mutex.attached = true;
    }
    pub fn enqueue(_: *Manager, mutex: *Mutex, waiter: *Thread, token: u64) AdmissionError!void {
        const owner = try ownerOf(mutex);
        try check(owner, waiter, token);
        append(mutex, waiter, token);
    }
    /// Generic completion calls this under T before claiming the token. A stale
    /// token cannot cancel a later wait. Sticky user WAITERS is caller policy.
    pub fn cancel(self: *Manager, thread: *Thread, token: u64) Cancel {
        const mutex = thread.waiting_mutex orelse return .{};
        if (token == 0 or token != thread.wait_token) return .{};
        std.debug.assert(mutex.attached);
        _ = remove(mutex, thread);
        if (mutex.head != null) return .{ .removed = true };
        self.detach(mutex);
        return .{ .removed = true, .retired = mutex };
    }
    /// Caller has committed the word CAS to head.identity while holding T.
    /// Detach the waiter BEFORE completion claim: generic cancellation must
    /// not undo an ownership handoff which has already won the word CAS.
    pub fn handoff(self: *Manager, mutex: *Mutex) Handoff {
        std.debug.assert(mutex.attached and mutex.head != null);
        const waiter = remove(mutex, mutex.head.?);
        if (mutex.head == null) {
            self.detach(mutex);
            return .{ .waiter = waiter, .retired = mutex };
        }
        unlinkOwner(mutex);
        linkOwner(mutex, waiter.thread);
        return .{ .waiter = waiter, .retired = null };
    }
    /// Teardown first cancels the dying thread's own wait, then repeatedly
    /// retires its owned records. Drain each returned record with popRetired,
    /// complete every waiter with NoOwner, and free it before identity reuse.
    /// No user-word access is required or permitted by this state operation.
    pub fn retireOwned(self: *Manager, owner: *Thread) ?*Mutex {
        std.debug.assert(owner.waiting_mutex == null);
        const mutex = owner.owned_head orelse return null;
        self.detach(mutex);
        return mutex;
    }
    pub fn popRetired(mutex: *Mutex) ?Waiter {
        std.debug.assert(!mutex.attached and mutex.owner == null);
        return remove(mutex, mutex.head orelse return null);
    }
};

fn testThread(index: u32) Thread {
    return .{ .identity = .{ .principal = 7, .index = index, .generation = 1 } };
}
fn record(address: u64) Mutex {
    return .{ .key = .{ .principal = 7, .address = address } };
}

test "PI state FIFO handoff keeps only nonempty queues and detached completion tokens" {
    var manager: Manager = .{};
    var owner = testThread(1);
    var a = testThread(2);
    var b = testThread(3);
    var mutex = record(4096);
    try manager.attach(&mutex, &owner, &a, 11);
    try manager.enqueue(&mutex, &b, 12);
    try std.testing.expect(manager.find(mutex.key) == &mutex);
    const first = manager.handoff(&mutex);
    try std.testing.expect(first.waiter.thread == &a and first.waiter.wait_token == 11 and first.retired == null);
    try std.testing.expect(owner.owned_head == null and a.owned_head == &mutex and mutex.owner == &a);
    try std.testing.expect(!manager.cancel(&a, 11).removed);
    const last = manager.handoff(&mutex);
    try std.testing.expect(last.waiter.thread == &b and last.waiter.wait_token == 12 and last.retired == &mutex);
    try std.testing.expect(manager.find(mutex.key) == null and a.owned_head == null and b.owned_head == null);
    try std.testing.expect(mutex.head == null and mutex.tail == null);
}

test "PI state deadlock chains self lock and admission failures never mutate state" {
    var manager: Manager = .{};
    var a = testThread(1);
    var b = testThread(2);
    var c = testThread(3);
    var ab = record(4096);
    var bc = record(8192);
    var ca = record(12288);
    try manager.attach(&ab, &a, &b, 1);
    try manager.attach(&bc, &b, &c, 2);
    const before = manager;
    const before_a = a;
    const before_b = b;
    const before_c = c;
    const before_ab = ab;
    const before_bc = bc;
    try std.testing.expectError(error.Deadlock, manager.attach(&ca, &c, &a, 3));
    try std.testing.expectError(error.Deadlock, manager.attach(&ca, &a, &a, 3));
    try std.testing.expectError(error.InvalidToken, manager.attach(&ca, &c, &a, 0));
    try std.testing.expectError(error.AlreadyWaiting, manager.enqueue(&ab, &c, 4));
    var duplicate = record(4096);
    try std.testing.expectError(error.DuplicateKey, manager.attach(&duplicate, &a, &a, 5));
    for (before.buckets, manager.buckets) |expected, actual| try std.testing.expect(expected == actual);
    try std.testing.expect(std.meta.eql(before_a, a) and std.meta.eql(before_b, b) and std.meta.eql(before_c, c));
    try std.testing.expect(std.meta.eql(before_ab, ab) and std.meta.eql(before_bc, bc));
    try std.testing.expect(ca.owner == null and ca.head == null and !ca.attached);
    try std.testing.expect(a.waiting_mutex == null and a.wait_token == 0);
    try std.testing.expect(manager.cancel(&c, 2).retired == &bc);
    try std.testing.expect(manager.cancel(&b, 1).retired == &ab);
}

test "PI state cancel head middle last and stale token" {
    var manager: Manager = .{};
    var owner = testThread(1);
    var a = testThread(2);
    var b = testThread(3);
    var c = testThread(4);
    var d = testThread(5);
    var mutex = record(4096);
    try manager.attach(&mutex, &owner, &a, 1);
    try manager.enqueue(&mutex, &b, 2);
    try manager.enqueue(&mutex, &c, 3);
    try manager.enqueue(&mutex, &d, 4);
    try std.testing.expect(!manager.cancel(&b, 99).removed);
    try std.testing.expect(manager.cancel(&b, 2).retired == null);
    try std.testing.expect(a.queue_next == &c and c.queue_prev == &a);
    try std.testing.expect(manager.cancel(&a, 1).retired == null and mutex.head == &c);
    try std.testing.expect(manager.cancel(&d, 4).retired == null and mutex.tail == &c);
    const last = manager.cancel(&c, 3);
    try std.testing.expect(last.removed and last.retired == &mutex);
    try std.testing.expect(owner.owned_head == null and manager.find(mutex.key) == null);
    try std.testing.expect(!manager.cancel(&c, 3).removed);
}

test "PI state owner death drains all queues before generation reset" {
    var manager: Manager = .{};
    var owner = testThread(1);
    var other = testThread(2);
    var a = testThread(3);
    var b = testThread(4);
    var c = testThread(5);
    var waiting = record(4096);
    var one = record(8192);
    var two = record(12288);
    try manager.attach(&waiting, &other, &owner, 9);
    try manager.attach(&one, &owner, &a, 10);
    try manager.enqueue(&one, &b, 11);
    try manager.attach(&two, &owner, &c, 12);
    try std.testing.expect(manager.cancel(&owner, 9).retired == &waiting);
    var completions: usize = 0;
    var freed: usize = 1;
    while (manager.retireOwned(&owner)) |mutex| {
        try std.testing.expect(manager.find(mutex.key) == null);
        while (Manager.popRetired(mutex)) |waiter| {
            try std.testing.expect(waiter.thread.waiting_mutex == null and waiter.thread.wait_token == 0);
            try std.testing.expect(!manager.cancel(waiter.thread, waiter.wait_token).removed);
            try std.testing.expect(waiter.wait_token >= 10 and waiter.wait_token <= 12);
            completions += 1; // caller completes this token with NoOwner
        }
        freed += 1; // caller returns this stable record to its SlotTable
    }
    try std.testing.expectEqual(@as(usize, 3), completions);
    try std.testing.expectEqual(@as(usize, 3), freed);
    owner.reset(.{ .principal = 7, .index = 1, .generation = 2 });
    try std.testing.expectEqual(@as(u32, 2), owner.identity.generation);
    try manager.attach(&one, &owner, &a, 20);
    try std.testing.expectEqual(@as(u32, 2), one.owner_generation);
    try std.testing.expect(!manager.cancel(&a, 10).removed);
    try std.testing.expect(manager.cancel(&a, 20).retired == &one);
}

test "PI state hash collisions and backing generations keep distinct keys" {
    var manager: Manager = .{};
    var owner = testThread(1);
    var a = testThread(2);
    var b = testThread(3);
    var one = record(4096);
    var two = record(4096);
    two.key.backing_generation = 2;
    try manager.attach(&one, &owner, &a, 1);
    try manager.attach(&two, &owner, &b, 2);
    try std.testing.expect(manager.find(one.key) == &one and manager.find(two.key) == &two);
    try std.testing.expect(manager.cancel(&a, 1).retired == &one);
    try std.testing.expect(manager.find(two.key) == &two);
    try std.testing.expect(manager.cancel(&b, 2).retired == &two);
    try std.testing.expect(owner.owned_head == null);
}

test "PI state rejects stale owner generation and foreign identity without mutation" {
    var manager: Manager = .{};
    var owner = testThread(1);
    var a = testThread(2);
    var b = testThread(3);
    var mutex = record(4096);
    try manager.attach(&mutex, &owner, &a, 1);
    mutex.owner_generation = 2; // simulate an invalid retained identity
    try std.testing.expectError(error.InvalidOwner, manager.enqueue(&mutex, &b, 2));
    try std.testing.expect(b.waiting_mutex == null and b.wait_token == 0 and mutex.tail == &a);
    mutex.owner_generation = 1;
    b.identity.principal = 8;
    try std.testing.expectError(error.InvalidIdentity, manager.enqueue(&mutex, &b, 2));
    b.identity.principal = 7;
    b.identity.generation = 0;
    try std.testing.expectError(error.InvalidIdentity, manager.enqueue(&mutex, &b, 2));
    try std.testing.expect(b.waiting_mutex == null and b.wait_token == 0 and mutex.tail == &a);
    try std.testing.expect(manager.cancel(&a, 1).retired == &mutex);
}
