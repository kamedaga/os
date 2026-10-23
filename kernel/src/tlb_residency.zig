//! PCID retention and user execution are separate CPU-local facts. The owner
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

// BSP boot allocation, before AP publication. Both arrays have kernel lifetime;
// failure aborts boot and cannot publish a partially initialized CPU capacity.
pub fn initCpuStorage(count: usize) !void {
    std.debug.assert(count >= roots.len and count >= active.len);
    const runtime = @import("arch/x86_64/cpu_runtime.zig");
    const storage = try runtime.alloc([pcids]u64, count);
    const execution = try runtime.alloc(Active, count);
    @memset(storage, @splat(0));
    @memcpy(storage[0..roots.len], roots);
    @memcpy(execution[0..active.len], active);
    roots = storage;
    active = execution;
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
}

/// A missing storage slot is conservatively targeted, never silently skipped.
pub fn isActive(cpu: usize, context: UserContext) bool {
    return cpu >= active.len or @atomicLoad(u64, &active[cpu].value, .acquire) == context.value;
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
    roots = &rows;
    active = &execution;
    defer {
        roots = old_roots;
        active = old_active;
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
    roots = &seed_roots;
    active = &seed_active;
    defer {
        roots = old_roots;
        active = old_active;
    }
    const context = UserContext.fromCr3(0x12345001);
    try std.testing.expect(!enterUser(0, context));
    try initCpuStorage(72);
    defer std.heap.page_allocator.free(roots);
    defer std.heap.page_allocator.free(active);
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
