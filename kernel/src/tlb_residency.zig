//! PCID retention, user execution and borrowed roots are distinct facts. The owner
//! publishes active with IF clear; the requester may invalidate retained roots.
//! A zero row forbids NOFLUSH on the next return, even after an IPI ACK.
const std = @import("std");
const builtin = @import("builtin");
const pcids = 1 << 12;
const root_mask: u64 = 0x000f_ffff_ffff_f000;

/// Shared identity for scheduler CR3s and principal CR3s; never includes NOFLUSH.
pub const UserContext = struct {
    value: u64,
    pub fn fromCr3(cr3: u64) UserContext {
        return .{ .value = cr3 & (root_mask | 4095) };
    }
    fn pcid(self: UserContext) usize {
        return @intCast(self.value & 4095);
    }
    fn root(self: UserContext) u64 {
        return self.value & root_mask;
    }
};
const Active = struct { value: u64 align(64) = 0 };
comptime {
    std.debug.assert(@sizeOf(Active) == 64);
}
var bootstrap: [1][pcids]u64 = @splat(@splat(0));
var roots: [][pcids]u64 = &bootstrap;
var bootstrap_active: [1]Active = .{.{}};
var active: []Active = &bootstrap_active;
// Kernel-mode user-root loads are not user returns. Keep their exposure
// separate so a temporary root cannot erase a return-window registration.
var bootstrap_borrowed: [1]Active = .{.{}};
var borrowed: []Active = &bootstrap_borrowed;

// BSP boot allocation, before AP publication. Both arrays have kernel lifetime;
// failure aborts boot and cannot publish a partially initialized CPU capacity.
pub fn initCpuStorage(count: usize) !void {
    std.debug.assert(count >= roots.len and count >= active.len and count >= borrowed.len);
    const runtime = @import("arch/x86_64/cpu_runtime.zig");
    const storage = try runtime.alloc([pcids]u64, count);
    const execution = try runtime.alloc(Active, count);
    const kernel_execution = try runtime.alloc(Active, count);
    @memset(storage, @splat(0));
    @memset(execution, .{});
    @memset(kernel_execution, .{});
    @memcpy(storage[0..roots.len], roots);
    @memcpy(execution[0..active.len], active);
    @memcpy(kernel_execution[0..borrowed.len], borrowed);
    roots = storage;
    active = execution;
    borrowed = kernel_execution;
}

/// Scalar-only: used with live user XState. This non-publishing entry serves
/// kernel-mode CR3 loads too; it must never claim that user execution began.
pub fn enterRetained(cpu: usize, cr3: u64) bool {
    const context = UserContext.fromCr3(cr3);
    if (context.pcid() == 0 or cpu >= roots.len) return false;
    const slot = &roots[cpu][context.pcid()];
    if (@atomicLoad(u64, slot, .monotonic) == context.root()) return true;
    @atomicStore(u64, slot, context.root(), .monotonic);
    // A CAS may clear the row between our mismatch load and store. In that
    // case this store can restore it, but the mandatory flushing CR3 load is
    // AFTER this fence and the PTE change. IF stays clear through that load.
    fenceBeforeSnapshot();
    return false;
}

/// Every user-return load, including forced flush/first entry, publishes here.
/// The following assembly CR3 load and return must keep IF clear throughout.
pub fn enterUser(cpu: usize, context: UserContext) bool {
    std.debug.assert(cpu < active.len);
    @atomicStore(u64, &active[cpu].value, context.value, .monotonic);
    // Paired with requester PTE stores -> MFENCE -> CAS -> active snapshot:
    // either it sees us and waits for an IRQ flush, or we see its cleared row
    // and flush on entry. Store-buffering must not let both sides miss.
    fenceBeforeSnapshot();
    return enterRetained(cpu, context.value);
}

/// Only after loading kernel CR3 on a proven ring-3 entry. Never clear on
/// kernel-mode #PF/#DB (including the return window): a missed clear costs an
/// extra IPI, whereas a false clear can let a noflush return evade a request.
/// Future NMI/#MC entry paths must obey the same rule.
pub fn leaveUser(cpu: usize) void {
    std.debug.assert(cpu < active.len);
    @atomicStore(u64, &active[cpu].value, 0, .release);
    leaveBorrowed(cpu);
}

/// A missing storage slot is conservatively targeted, never silently skipped.
pub fn isActive(cpu: usize, context: UserContext) bool {
    return cpu >= active.len or cpu >= borrowed.len or
        @atomicLoad(u64, &active[cpu].value, .acquire) == context.value or
        @atomicLoad(u64, &borrowed[cpu].value, .acquire) == context.value;
}

/// Caller holds IF=0 through the real load. Publishing before MFENCE pairs
/// with PTE stores -> requester MFENCE -> exposure snapshot: an entrant the
/// snapshot misses still owes the subsequent flushing CR3 load. This also
/// covers copy helpers restoring a user root while still executing ring 0.
pub fn loadKernelContext(comptime Load: type, cpu: usize, cr3: u64, kernel_cr3: u64, pcid_enabled: bool) void {
    const context = UserContext.fromCr3(cr3);
    const kernel_root = context.root() == UserContext.fromCr3(kernel_cr3).root();
    var operand = cr3;
    if (!kernel_root) {
        std.debug.assert(cpu < borrowed.len);
        @atomicStore(u64, &borrowed[cpu].value, context.value, .monotonic);
        fenceBeforeSnapshot();
        const retained = enterRetained(cpu, cr3);
        if (!pcid_enabled or context.pcid() == 0 or !retained) operand &= ~(@as(u64, 1) << 63);
    } else if (!pcid_enabled or context.pcid() == 0) operand &= ~(@as(u64, 1) << 63);
    Load.write(operand);
    // A clear must prove the hardware load has finished, not just that it
    // was planned. The old non-global translations are gone before reuse.
    if (kernel_root) leaveBorrowed(cpu);
}

fn leaveBorrowed(cpu: usize) void {
    std.debug.assert(cpu < borrowed.len);
    @atomicStore(u64, &borrowed[cpu].value, 0, .release);
}

/// Requester owns shootdown serialization. CAS cannot erase a replacement
/// root's registration; a concurrent mismatch-store is safe only because that
/// entrant owes a flushing load. Local CPU is flushed by the requester itself.
pub fn invalidateRetained(cpu: usize, context: UserContext) void {
    if (cpu >= roots.len or context.pcid() == 0) return;
    _ = @cmpxchgStrong(u64, &roots[cpu][context.pcid()], context.root(), 0, .seq_cst, .seq_cst);
}

pub fn contains(cpu: usize, cr3: u64) bool {
    if (cpu >= roots.len or (cr3 & 4095) == 0) return false;
    return @atomicLoad(u64, &roots[cpu][cr3 & 4095], .acquire) == (cr3 & root_mask);
}

pub fn fenceBeforeSnapshot() void {
    if (!builtin.is_test) asm volatile ("mfence" ::: .{ .memory = true });
}

test "resident roots survive migration and require flushing on PCID replacement" {
    var rows: [72][pcids]u64 = @splat(@splat(0));
    const before = roots;
    roots = &rows;
    defer roots = before;
    const a = 0x100001;
    const b = 0x200001;
    try std.testing.expect(!enterRetained(0, a));
    try std.testing.expect(enterRetained(0, a));
    try std.testing.expect(!enterRetained(71, a));
    try std.testing.expect(contains(0, a) and contains(71, a));
    try std.testing.expect(!contains(65, a));
    try std.testing.expect(!enterRetained(71, b));
    try std.testing.expect(!contains(71, a) and contains(71, b));
    try std.testing.expect(!enterRetained(71, a));
    try std.testing.expect(!enterRetained(0, 0x300000));
}

test "both registration snapshot orders cover the entrant" {
    var rows: [1][pcids]u64 = @splat(@splat(0));
    const before = roots;
    roots = &rows;
    defer roots = before;
    const cr3 = 0x12345001;
    // First kernel-mode registration owes a flushing load.
    try std.testing.expect(!contains(0, cr3));
    try std.testing.expect(!enterRetained(0, cr3));
    // Retention records the flushing load; it does not publish user execution.
    try std.testing.expect(contains(0, cr3));
    // Without invalidation, the same root may retain its translations.
    try std.testing.expect(enterRetained(0, cr3));
    try std.testing.expect(contains(0, cr3));
    // Exit/reuse first requires the existing all-online full flush and no
    // old-AS entrants. Sticky metadata does not replace that lifetime fence.
    try std.testing.expect(!enterRetained(0, 0x23456001));
}

test "zero retention never hides an active user from consecutive requests" {
    var rows: [1][pcids]u64 = @splat(@splat(0));
    var execution: [1]Active = .{.{}};
    const old_roots = roots;
    const old_active = active;
    const old_borrowed = borrowed;
    var test_borrowed: [72]Active = @splat(.{});
    borrowed = &test_borrowed;
    roots = &rows;
    active = &execution;
    defer {
        roots = old_roots;
        active = old_active;
        borrowed = old_borrowed;
    }
    const context = UserContext.fromCr3(0x8000_0000_1234_5001);
    try std.testing.expectEqual(@as(u64, 0x12345001), context.value);
    try std.testing.expect(!enterUser(0, context));
    for (0..2) |_| {
        invalidateRetained(0, context);
        try std.testing.expect(!contains(0, context.value));
        try std.testing.expect(isActive(0, context));
        // Receiver flushing does not restore the row. It may continue user
        // execution and refill translations before the next request.
    }
    leaveUser(0);
    try std.testing.expect(!isActive(0, context));
    try std.testing.expect(!enterUser(0, context));
    leaveUser(0);
    try std.testing.expect(enterRetained(0, context.value));
    try std.testing.expect(!isActive(0, context));
    try std.testing.expect(isActive(1, context));
    // A CAS for the old root must not erase another root using the same PCID.
    try std.testing.expect(!enterRetained(0, 0x23456001));
    invalidateRetained(0, context);
    try std.testing.expect(contains(0, 0x23456001));
}

test "CPU storage preserves bootstrap active and retention" {
    var seed_roots: [1][pcids]u64 = @splat(@splat(0));
    var seed_active: [1]Active = .{.{}};
    const old_roots = roots;
    const old_active = active;
    const old_borrowed = borrowed;
    var test_borrowed: [1]Active = @splat(.{});
    borrowed = &test_borrowed;
    roots = &seed_roots;
    active = &seed_active;
    defer {
        roots = old_roots;
        active = old_active;
        borrowed = old_borrowed;
    }
    const context = UserContext.fromCr3(0x12345001);
    try std.testing.expect(!enterUser(0, context));
    try initCpuStorage(72);
    defer std.heap.page_allocator.free(roots);
    defer std.heap.page_allocator.free(active);
    defer std.heap.page_allocator.free(borrowed);
    try std.testing.expect(contains(0, context.value));
    try std.testing.expect(isActive(0, context));
    try std.testing.expect(!contains(71, context.value));
    try std.testing.expect(!isActive(71, context));
    try std.testing.expect(!enterUser(71, context));
    invalidateRetained(71, context);
    try std.testing.expect(isActive(71, context));
    try std.testing.expect(!contains(71, context.value));
    leaveUser(71);
    try std.testing.expect(!isActive(71, context));
    try std.testing.expect(isActive(72, context));
}

test "PCID zero borrowed loads publish before hardware and clear only after kernel load" {
    var rows: [2][pcids]u64 = @splat(@splat(0));
    var users: [2]Active = @splat(.{});
    var loans: [2]Active = @splat(.{});
    const old_roots = roots;
    const old_active = active;
    const old_borrowed = borrowed;
    roots = &rows;
    active = &users;
    borrowed = &loans;
    defer { roots = old_roots; active = old_active; borrowed = old_borrowed; }
    const Load = struct {
        var expected: u64 = 0;
        var prior: u64 = 0;
        var kernel: bool = false;
        var calls: usize = 0;
        pub fn write(value: u64) void {
            calls += 1;
            std.debug.assert(value == expected);
            // During kernel load the old exposure must STILL be present.
            std.debug.assert(isActive(1, UserContext.fromCr3(if (kernel) prior else expected)));
            std.debug.assert(isActive(1, UserContext.fromCr3(0x9000))); // independent user return
        }
    };
    _ = enterUser(1, UserContext.fromCr3(0x9000));
    for ([_]u64{ 0x1000, 0x2000, 0x1000 }) |root| {
        Load.expected = root; Load.kernel = false;
        loadKernelContext(Load, 1, root | (@as(u64, 1) << 63), 0x8000, false);
        for (0..2) |_| {
            invalidateRetained(1, UserContext.fromCr3(root));
            try std.testing.expect(isActive(1, UserContext.fromCr3(root)));
        }
        try std.testing.expect(!contains(1, root));
    }
    Load.expected = 0x8000; Load.prior = 0x1000; Load.kernel = true;
    loadKernelContext(Load, 1, 0x8000, 0x8000, false);
    try std.testing.expect(!isActive(1, UserContext.fromCr3(0x1000)));
    try std.testing.expect(isActive(1, UserContext.fromCr3(0x9000)));
    leaveUser(1);
    try std.testing.expect(!isActive(1, UserContext.fromCr3(0x9000)));
    // A PCID-0 forced user return also publishes even without retention.
    try std.testing.expect(!enterUser(1, UserContext.fromCr3(0x3000)));
    try std.testing.expect(isActive(1, UserContext.fromCr3(0x3000)));
    leaveUser(1);
    try std.testing.expect(isActive(2, UserContext.fromCr3(0x3000)));
    try std.testing.expectEqual(@as(usize, 4), Load.calls);
}

test "PCID zero and tagged borrowed retention remain separate" {
    var rows: [1][pcids]u64 = @splat(@splat(0));
    var users: [1]Active = @splat(.{});
    var loans: [1]Active = @splat(.{});
    const old_roots = roots;
    const old_active = active;
    const old_borrowed = borrowed;
    roots = &rows; active = &users; borrowed = &loans;
    defer { roots = old_roots; active = old_active; borrowed = old_borrowed; }
    const Load = struct {
        var operand: u64 = 0;
        pub fn write(v: u64) void { operand = v; }
    };
    const root: u64 = 0x1001;
    const noflush = @as(u64, 1) << 63;
    loadKernelContext(Load, 0, root | noflush, 0x8000, true);
    try std.testing.expectEqual(root, Load.operand);
    try std.testing.expect(isActive(0, UserContext.fromCr3(root)));
    loadKernelContext(Load, 0, root | noflush, 0x8000, true);
    try std.testing.expectEqual(root | noflush, Load.operand);
    invalidateRetained(0, UserContext.fromCr3(root));
    try std.testing.expect(isActive(0, UserContext.fromCr3(root)));
    loadKernelContext(Load, 0, root | noflush, 0x8000, true);
    try std.testing.expectEqual(root, Load.operand);
    // Even with CR4.PCIDE, tag zero never inherits a noflush privilege.
    loadKernelContext(Load, 0, 0x2000 | noflush, 0x8000, true);
    try std.testing.expectEqual(@as(u64, 0x2000), Load.operand);
    // A tagged NOFLUSH load may leave dormant tag-0 entries behind. Safety
    // depends on the future tag-0 load flushing, not on erasing them on exit.
    loadKernelContext(Load, 0, root | noflush, 0x8000, true);
    try std.testing.expectEqual(root | noflush, Load.operand);
    loadKernelContext(Load, 0, 0x2000 | noflush, 0x8000, true);
    try std.testing.expectEqual(@as(u64, 0x2000), Load.operand);
    leaveUser(0);
    try std.testing.expect(!isActive(0, UserContext.fromCr3(0x2000)));
}
