//! A native thread owns subscriptions independently of any blocking invocation.
//! K serializes queues and source publication. IPC sources additionally hold E;
//! direct IPC paths reject data observers that need new publication. Idle
//! queued hints and HUP-only observers remain linked for future REUSE/close.
const types = @import("types.zig");
const storage = @import("../memory/slot_table.zig");

pub const Invocation = struct {
    token: u64,
};

pub const ThreadEntry = struct {
    set: storage.Ref = .{ .index = 0, .generation = 0 },
};

pub const Set = struct {
    owner: types.PrincipalId = @enumFromInt(1),
    thread: u32 = 0,
    thread_generation: u32 = 0,
    index: u32 = 0,
    generation: u32 = 0,
    count: u32 = 0,
    table_mutation: u64 = 0,
    invalidated: bool = false,
    invocation: ?Invocation = null,
    items: storage.SlotTable(Item) = .{},
    head: ?*Item = null,
    tail: ?*Item = null,
    queued: usize = 0,
    context: ?*anyopaque = null,
    notify: ?*const fn (*anyopaque, u32) void = null,

    pub fn kick(self: *Set) void {
        if (self.invocation != null and (self.queued != 0 or self.invalidated))
            if (self.notify) |notify| notify(self.context.?, self.index);
    }

    pub fn pop(self: *Set) ?*Item {
        const item = self.head orelse return null;
        self.head = item.next;
        if (self.head == null) self.tail = null;
        item.next = null;
        item.queued = false;
        self.queued -= 1;
        return item;
    }
};

pub const Item = struct {
    set: *Set = undefined,
    index: u32 = 0,
    fd: types.Fd = 0,
    descriptor_seq: u64 = 0,
    object: types.KernelObjectRef = .{},
    events: u64 = 0,
    min_write_bytes: u64 = 0,
    registrations: [2]types.FdWaitRegistrationRef = @splat(.{}),
    registration_count: usize = 0,
    observing: bool = false,
    queued: bool = false,
    next: ?*Item = null,

    pub fn publish(self: *Item) void {
        if (self.set.invalidated) return;
        if (!self.queued) {
            self.queued = true;
            self.next = null;
            if (self.set.tail) |tail| tail.next = self else self.set.head = self;
            self.set.tail = self;
            self.set.queued += 1;
        }
        self.set.kick();
    }
};
