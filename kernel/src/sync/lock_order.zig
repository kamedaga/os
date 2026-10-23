const std = @import("std");

// Window-bearing locks precede scheduler locks: a timer may acquire the
// latter while interrupting a window. Leaf locks never wait for CPU IPIs.
pub const Class = enum {
    principal_lifecycle,
    kernel_state,
    fd_writer,
    futex,
    address_space,
    shared_vm,
    pager,
    vmo_backing,
    tlb_shootdown,
    vtd,
    amd_iommu,
    ipc_endpoint,
    thread_table,
    cpu_state,
    runqueue,
    thread_directory,
    pmm_cache,
    pmm,
    as_directory,
    process_descriptor,
    vma_directory,
    pci_config,
    copy_window,
    // Directory leaves perform no callbacks or further lock acquisition.
    // Copy readers may wait here: every writer masks IRQs while holding the
    // leaf, and releases it before PMM returns, copy work or IPI rendezvous.
    slot_directory,
    irq_publication,
    clock_publication,
    boot_trace,
    boot_drawing,
    log_ring,

    pub fn forbidsIpiWait(self: Class) bool {
        return switch (self) {
            .principal_lifecycle, .kernel_state, .address_space, .shared_vm, .vmo_backing, .tlb_shootdown, .boot_trace, .boot_drawing => false,
            else => true,
        };
    }
};
// A successful one-shot try can invert rank without waiting, but its hold
// must constrain every subsequent blocking acquire just like any other hold.
pub const Mode = enum { blocking, try_once };
pub const Key = struct {
    class: Class,
    order: usize = 0,

    fn follows(self: Key, held: Key) bool {
        if (self.class != held.class) return @intFromEnum(self.class) > @intFromEnum(held.class);
        return self.order > held.order;
    }
};

pub const Error = error{
    ReverseOrder,
    Reentry,
    NotHeld,
    WrongContext,
    CopiedRecord,
    RecordStillLinked,
    ContextOverflow,
    ContextUnderflow,
    ContextStillHoldsLocks,
    NotQuiescent,
    IpiWaitWithBlockingLock,
    InvalidRecursion,
    DepthOverflow,
    DepthUnderflow,
};

// Intrusive records have the lock's lifetime: no heap operation under a lock
// and no fixed-size held-lock stack (all-CPU/all-address-space transactions
// can hold many locks). Moving a previously used lock requires fresh storage.
pub const Record = struct {
    bound_address: ?*Record = null,
    next: ?*Record = null,
    key: Key = .{ .class = .thread_table },
    context: usize = 0,
    linked: bool = false,
    mode: Mode = .blocking,
};

// A CPU owns its execution even when handoff changes currentThread. Interrupt
// depth distinguishes an IRQ from its interrupted owner without moving locks
// into the incoming thread's bookkeeping. Callers serialize this with IF=0.
pub const Cpu = struct {
    head: ?*Record = null,
    interrupt_depth: usize = 0,

    pub fn beforeAcquire(self: *const Cpu, record: *const Record, key: Key) Error!void {
        var held = self.head;
        var reversed = false;
        while (held) |node| : (held = node.next) {
            // Only traverse this CPU's list. The attempted record may still
            // belong to another CPU: do not inspect its mutable fields here.
            if (node == record) return error.Reentry;
            if (node.key.class == .slot_directory or !key.follows(node.key)) reversed = true;
        }
        if (reversed) return error.ReverseOrder;
    }

    /// Hardware lock acquired, interrupts disabled, record exclusively owned.
    pub fn acquired(self: *Cpu, record: *Record, key: Key) Error!void {
        return self.acquiredMode(record, key, .blocking);
    }

    pub fn acquiredMode(self: *Cpu, record: *Record, key: Key, mode: Mode) Error!void {
        if (mode == .blocking) try self.beforeAcquire(record, key);
        var held = self.head;
        while (held) |node| : (held = node.next) {
            if (node == record) return error.Reentry;
            if (node.key.class == .slot_directory) return error.ReverseOrder;
            // A successful try must not bypass IPC's commit protocol. In
            // particular cancellation records are detached from T before
            // entering an endpoint, even when that endpoint is uncontended.
            if (key.class == .ipc_endpoint and
                (node.key.class == .thread_table or node.key.class == .cpu_state or node.key.class == .runqueue))
                return error.ReverseOrder;
            if (node.key.class == .ipc_endpoint and (key.class == .kernel_state or key.class == .fd_writer))
                return error.ReverseOrder;
        }
        // IRQ publication must end before completion enters KernelState,
        // including a successful nonwaiting try: Q is a per-slot leaf.
        if (key.class == .kernel_state) {
            var publication = self.head;
            while (publication) |node| : (publication = node.next) {
                if (node.key.class == .irq_publication) return error.ReverseOrder;
            }
        }
        if (record.bound_address) |bound| {
            if (bound != record) return error.CopiedRecord;
        }
        if (record.linked) return error.RecordStillLinked;
        record.bound_address = record;
        record.key = key;
        record.mode = mode;
        record.context = self.interrupt_depth;
        record.next = self.head;
        record.linked = true;
        self.head = record;
    }

    pub fn assertOwned(self: *const Cpu, record: *const Record) Error!void {
        var held = self.head;
        while (held) |node| : (held = node.next) {
            if (node != record) continue;
            if (node.context != self.interrupt_depth) return error.WrongContext;
            return;
        }
        return error.NotHeld;
    }

    pub fn recursive(self: *const Cpu, record: *const Record) Error!bool {
        var held = self.head;
        while (held) |node| : (held = node.next) {
            if (node != record) continue;
            if (node.context != self.interrupt_depth) return error.WrongContext;
            if (node.key.class != .address_space and node.key.class != .shared_vm)
                return error.InvalidRecursion;
            return true;
        }
        return false;
    }

    pub fn assertCanWaitForIpi(self: *const Cpu) Error!void {
        var held = self.head;
        // Interrupted owners can block the remote responder too. Do not
        // filter by IRQ depth, nor by how this CPU acquired the held lock.
        while (held) |node| : (held = node.next) {
            if (node.key.class.forbidsIpiWait()) return error.IpiWaitWithBlockingLock;
        }
    }

    /// Unlink before the release store lets another CPU mutate the record.
    pub fn releasing(self: *Cpu, record: *Record) Error!void {
        var link = &self.head;
        while (link.*) |node| {
            if (node == record) {
                if (node.context != self.interrupt_depth) return error.WrongContext;
                link.* = node.next;
                node.next = null;
                node.linked = false;
                return;
            }
            link = &node.next;
        }
        // No membership means no permission to read record's mutable fields,
        // including when this is a wrong-CPU unlock rather than a double drop.
        return error.NotHeld;
    }

    pub fn enterInterrupt(self: *Cpu) Error!usize {
        if (self.interrupt_depth == std.math.maxInt(usize)) return error.ContextOverflow;
        self.interrupt_depth += 1;
        return self.interrupt_depth;
    }

    pub fn leaveInterrupt(self: *Cpu, depth: usize) Error!void {
        if (self.interrupt_depth == 0 or self.interrupt_depth != depth) return error.ContextUnderflow;
        var held = self.head;
        while (held) |node| : (held = node.next) {
            if (node.context == depth) return error.ContextStillHoldsLocks;
        }
        self.interrupt_depth -= 1;
    }

    pub fn assertQuiescent(self: *const Cpu) Error!void {
        if (self.head != null or self.interrupt_depth != 0) return error.NotQuiescent;
    }
};

test "thread directory is terminal and rejects reentry" {
    var cpu: Cpu = .{};
    var table: Record = .{};
    var state: Record = .{};
    var queue: Record = .{};
    var directory: Record = .{};
    try cpu.acquired(&table, .{ .class = .thread_table });
    try cpu.acquired(&state, .{ .class = .cpu_state });
    try cpu.acquired(&queue, .{ .class = .runqueue });
    try cpu.acquired(&directory, .{ .class = .thread_directory });
    try std.testing.expectError(error.Reentry, cpu.beforeAcquire(&directory, .{ .class = .thread_directory }));
    var other: Record = .{};
    inline for (.{ Class.thread_table, Class.cpu_state, Class.runqueue }) |class| {
        try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&other, .{ .class = class, .order = 99 }));
    }
    try cpu.releasing(&directory);
    try cpu.releasing(&queue);
    try cpu.releasing(&state);
    try cpu.releasing(&table);
    try cpu.assertQuiescent();
}

test "lock order rejects reversal and reentry before spinning" {
    var cpu: Cpu = .{};
    var table: Record = .{};
    var state: Record = .{};
    try cpu.acquired(&table, .{ .class = .thread_table });
    try cpu.acquired(&state, .{ .class = .cpu_state });
    try std.testing.expectError(error.Reentry, cpu.beforeAcquire(&table, .{ .class = .thread_table }));
    var another: Record = .{};
    try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&another, .{ .class = .thread_table }));
    try std.testing.expect(!another.linked);
    try cpu.releasing(&state);
    try cpu.releasing(&table);
    try std.testing.expectError(error.NotHeld, cpu.releasing(&table));
    try cpu.assertQuiescent();
}

test "lock order checks every remaining record after non-LIFO drop" {
    var cpu: Cpu = .{};
    var a: Record = .{};
    var b: Record = .{};
    var c: Record = .{};
    try cpu.acquired(&a, .{ .class = .thread_table });
    try cpu.acquired(&b, .{ .class = .cpu_state, .order = 2 });
    try cpu.acquired(&c, .{ .class = .runqueue, .order = 4 });
    try cpu.releasing(&b);
    try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&b, .{ .class = .cpu_state, .order = 3 }));
    try cpu.releasing(&a);
    try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&b, .{ .class = .runqueue, .order = 3 }));
    try cpu.releasing(&c);
    try cpu.assertQuiescent();
}

test "lock order nests IRQ without giving it interrupted locks" {
    var cpu: Cpu = .{};
    var a: Record = .{};
    var b: Record = .{};
    try cpu.acquired(&a, .{ .class = .thread_table });
    const first = try cpu.enterInterrupt();
    try std.testing.expectError(error.WrongContext, cpu.releasing(&a));
    try std.testing.expectError(error.Reentry, cpu.beforeAcquire(&a, .{ .class = .thread_table }));
    try cpu.acquired(&b, .{ .class = .cpu_state });
    const second = try cpu.enterInterrupt();
    try std.testing.expectError(error.WrongContext, cpu.releasing(&b));
    try std.testing.expectError(error.ContextUnderflow, cpu.leaveInterrupt(first));
    try cpu.leaveInterrupt(second);
    try std.testing.expectError(error.ContextStillHoldsLocks, cpu.leaveInterrupt(first));
    try cpu.releasing(&b);
    try cpu.leaveInterrupt(first);
    try cpu.releasing(&a);
    try cpu.assertQuiescent();
}

test "lock order CPU migration cannot unlock another CPU owner" {
    var source: Cpu = .{};
    var destination: Cpu = .{};
    var record: Record = .{};
    try source.acquired(&record, .{ .class = .thread_table });
    // A contender's precheck must neither read nor overwrite source metadata.
    try destination.beforeAcquire(&record, .{ .class = .thread_table });
    try std.testing.expectError(error.NotHeld, destination.releasing(&record));
    try std.testing.expect(record.linked);
    try source.releasing(&record);
    try destination.acquired(&record, .{ .class = .thread_table });
    try destination.releasing(&record);
}

test "lock order detects moved records and context counter overflow" {
    var cpu: Cpu = .{};
    var record: Record = .{};
    try cpu.acquired(&record, .{ .class = .thread_table });
    try cpu.releasing(&record);
    var copied = record;
    try std.testing.expectError(error.CopiedRecord, cpu.acquired(&copied, .{ .class = .thread_table }));
    try cpu.assertQuiescent();
    cpu.interrupt_depth = std.math.maxInt(usize);
    try std.testing.expectError(error.ContextOverflow, cpu.enterInterrupt());
    try std.testing.expectEqual(std.math.maxInt(usize), cpu.interrupt_depth);
}

test "lock order has no small fixed held-lock capacity" {
    var cpu: Cpu = .{};
    var records: [1024]Record = [_]Record{.{}} ** 1024;
    for (&records, 0..) |*record, index| try cpu.acquired(record, .{ .class = .cpu_state, .order = index });
    var another: Record = .{};
    try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&another, .{ .class = .cpu_state, .order = 0 }));
    // Release ascending rather than list order, then check that all are gone.
    for (&records) |*record| try cpu.releasing(record);
    try cpu.assertQuiescent();
}

test "lock ordering all classes follow the approved order and IPI attributes" {
    var cpu: Cpu = .{};
    var records: [@typeInfo(Class).@"enum".fields.len]Record = @splat(.{});
    inline for (@typeInfo(Class).@"enum".fields, 0..) |field, i| {
        const class: Class = @enumFromInt(field.value);
        if (class != .slot_directory) try cpu.acquired(&records[i], .{ .class = class });
    }
    try std.testing.expectError(error.IpiWaitWithBlockingLock, cpu.assertCanWaitForIpi());
    for (&records) |*record| if (record.linked) {
        try cpu.releasing(record);
    };
    inline for (@typeInfo(Class).@"enum".fields) |field| {
        const class: Class = @enumFromInt(field.value);
        var record: Record = .{};
        try cpu.acquired(&record, .{ .class = class });
        if (class.forbidsIpiWait()) {
            try std.testing.expectError(error.IpiWaitWithBlockingLock, cpu.assertCanWaitForIpi());
        } else try cpu.assertCanWaitForIpi();
        try cpu.releasing(&record);
    }
    try cpu.assertQuiescent();
}

test "lock ordering try inversion is recorded and constrains subsequent blocking across IRQ" {
    var cpu: Cpu = .{};
    var high: Record = .{};
    var low: Record = .{};
    var middle: Record = .{};
    try cpu.acquired(&high, .{ .class = .pmm });
    const depth = try cpu.enterInterrupt();
    try cpu.acquiredMode(&low, .{ .class = .kernel_state }, .try_once);
    try std.testing.expectEqual(Mode.try_once, low.mode);
    try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&middle, .{ .class = .address_space }));
    try std.testing.expectError(error.IpiWaitWithBlockingLock, cpu.assertCanWaitForIpi());
    try cpu.releasing(&low);
    try cpu.leaveInterrupt(depth);
    try cpu.releasing(&high);
    try cpu.assertQuiescent();
}

test "lock ordering recursive AS and shared VM retain one record and reject IRQ ownership" {
    inline for (.{ Class.address_space, Class.shared_vm }) |class| {
        var cpu: Cpu = .{};
        var other_cpu: Cpu = .{};
        var record: Record = .{};
        try std.testing.expect(!try cpu.recursive(&record));
        try cpu.acquired(&record, .{ .class = class, .order = 2 });
        try std.testing.expect(try cpu.recursive(&record));
        try std.testing.expect(!try other_cpu.recursive(&record));
        try std.testing.expectError(error.NotHeld, other_cpu.assertOwned(&record));
        const depth = try cpu.enterInterrupt();
        try std.testing.expectError(error.WrongContext, cpu.recursive(&record));
        try std.testing.expectError(error.WrongContext, cpu.assertOwned(&record));
        try cpu.leaveInterrupt(depth);
        try std.testing.expect(cpu.head == &record and record.next == null);
        try cpu.releasing(&record);
        try cpu.assertQuiescent();
    }
}

test "lock ordering AS indices strictly increase and nonrecursive classes reject recursion" {
    var cpu: Cpu = .{};
    var a: Record = .{};
    var b: Record = .{};
    try cpu.acquired(&a, .{ .class = .address_space, .order = 4 });
    try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&b, .{ .class = .address_space, .order = 3 }));
    try cpu.acquired(&b, .{ .class = .address_space, .order = 5 });
    try cpu.releasing(&a);
    try cpu.releasing(&b);
    try cpu.acquired(&a, .{ .class = .kernel_state });
    try std.testing.expectError(error.InvalidRecursion, cpu.recursive(&a));
    try cpu.releasing(&a);
}

test "lock admission lifecycle try exception does not authorize later blocking inversion" {
    var cpu: Cpu = .{};
    var k: Record = .{};
    var l: Record = .{};
    var other_l: Record = .{};
    try cpu.acquired(&k, .{ .class = .kernel_state });
    try cpu.acquiredMode(&l, .{ .class = .principal_lifecycle }, .try_once);
    try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&other_l, .{ .class = .principal_lifecycle }));
    try cpu.assertCanWaitForIpi();
    try cpu.releasing(&k);
    try cpu.releasing(&l);
}

test "lock admission Q clock and log conservatively forbid IPI waiting even after try" {
    inline for (.{ Class.irq_publication, Class.clock_publication, Class.log_ring }) |class| {
        var cpu: Cpu = .{};
        var record: Record = .{};
        try cpu.acquiredMode(&record, .{ .class = class }, .try_once);
        const irq = try cpu.enterInterrupt();
        try std.testing.expectError(error.IpiWaitWithBlockingLock, cpu.assertCanWaitForIpi());
        try cpu.leaveInterrupt(irq);
        try cpu.releasing(&record);
    }
}

test "lock admission Q forbids even successful K try until slot release" {
    var cpu: Cpu = .{};
    var q: Record = .{};
    var k: Record = .{};
    try cpu.acquired(&q, .{ .class = .irq_publication });
    try std.testing.expectError(error.ReverseOrder, cpu.acquiredMode(&k, .{ .class = .kernel_state }, .try_once));
    try cpu.releasing(&q);
    try cpu.acquiredMode(&k, .{ .class = .kernel_state }, .try_once);
    try cpu.releasing(&k);
}

test "slot directory is terminal even for successful try acquisitions" {
    var cpu: Cpu = .{};
    var leaf: Record = .{};
    var other: Record = .{};
    try cpu.acquired(&leaf, .{ .class = .slot_directory });
    inline for (@typeInfo(Class).@"enum".fields) |field| {
        const key: Key = .{ .class = @enumFromInt(field.value), .order = 1 };
        try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&other, key));
        try std.testing.expectError(error.ReverseOrder, cpu.acquiredMode(&other, key, .try_once));
    }
    try cpu.releasing(&leaf);
}

test "lock ordering copy window may read a slot directory but directory cannot enter copy work" {
    var cpu = Cpu{};
    var copy: Record = .{};
    var leaf: Record = .{};
    try cpu.acquired(&copy, .{ .class = .copy_window });
    try cpu.acquired(&leaf, .{ .class = .slot_directory });
    try cpu.releasing(&leaf);
    try cpu.releasing(&copy);
    try cpu.acquired(&leaf, .{ .class = .slot_directory });
    try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&copy, .{ .class = .copy_window }));
    try cpu.releasing(&leaf);
}

// IPC uses endpoint address ordering only when more than one endpoint is
// needed. T and S cannot be used to discover and then lock another endpoint.
test "IPC lock order admits writer and reader paths and rejects terminal inversions" {
    var cpu: Cpu = .{};
    var k: Record = .{};
    var fd: Record = .{};
    var endpoint: Record = .{};
    var table: Record = .{};
    var state: Record = .{};
    var queue: Record = .{};
    try cpu.acquired(&k, .{ .class = .kernel_state });
    try cpu.acquired(&fd, .{ .class = .fd_writer });
    try cpu.acquired(&endpoint, .{ .class = .ipc_endpoint, .order = 10 });
    try cpu.acquired(&table, .{ .class = .thread_table });
    try cpu.acquired(&state, .{ .class = .cpu_state });
    try cpu.acquired(&queue, .{ .class = .runqueue });
    var injected: Record = .{};
    inline for (.{ Class.kernel_state, Class.fd_writer, Class.ipc_endpoint }) |class| {
        try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&injected, .{ .class = class, .order = 20 }));
    }
    try cpu.releasing(&queue);
    try cpu.releasing(&state);
    try cpu.releasing(&table);
    try cpu.releasing(&endpoint);
    try cpu.releasing(&fd);
    try cpu.releasing(&k);
    // The same endpoint/T/S order is legal with no K or FD writer hold.
    try cpu.acquired(&endpoint, .{ .class = .ipc_endpoint, .order = 10 });
    try std.testing.expectError(error.ReverseOrder, cpu.beforeAcquire(&injected, .{ .class = .ipc_endpoint, .order = 9 }));
    try cpu.acquired(&table, .{ .class = .thread_table });
    try cpu.acquired(&state, .{ .class = .cpu_state });
    try cpu.acquired(&queue, .{ .class = .runqueue });
    try std.testing.expectError(error.IpiWaitWithBlockingLock, cpu.assertCanWaitForIpi());
    try cpu.releasing(&queue);
    try cpu.releasing(&state);
    try cpu.releasing(&table);
    try cpu.releasing(&endpoint);
    try cpu.assertQuiescent();
}

test "IPC terminal inversion is rejected even by successful nonwaiting try" {
    inline for (.{ Class.thread_table, Class.cpu_state, Class.runqueue }) |class| {
        var cpu: Cpu = .{};
        var held: Record = .{};
        var endpoint: Record = .{};
        try cpu.acquired(&held, .{ .class = class });
        try std.testing.expectError(error.ReverseOrder, cpu.acquiredMode(&endpoint, .{ .class = .ipc_endpoint }, .try_once));
        try cpu.releasing(&held);
    }
    inline for (.{ Class.kernel_state, Class.fd_writer }) |class| {
        var cpu: Cpu = .{};
        var endpoint: Record = .{};
        var lower: Record = .{};
        try cpu.acquired(&endpoint, .{ .class = .ipc_endpoint });
        try std.testing.expectError(error.ReverseOrder, cpu.acquiredMode(&lower, .{ .class = class }, .try_once));
        try cpu.releasing(&endpoint);
    }
}
