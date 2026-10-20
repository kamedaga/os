//! Per-CPU PCID residency. Only the owning CPU writes its row, with IF clear.
//! Entries are sticky until that PCID loads another root: a conservative set
//! of CPUs which can retain translations, including dormant address spaces.
const std = @import("std");
const builtin = @import("builtin");
const pcids = 1 << 12; // CR3 PCID field, not a CPU-count bound.
const root_mask: u64 = 0x000f_ffff_ffff_f000;
var bootstrap: [1][pcids]u64 = @splat(@splat(0));
var roots: [][pcids]u64 = &bootstrap;

pub fn initCpuStorage(count: usize) !void {
    const storage = try @import("arch/x86_64/cpu_runtime.zig").alloc([pcids]u64, count);
    @memset(storage, @splat(0));
    @memcpy(storage[0..roots.len], roots);
    roots = storage;
}

/// Scalar-only: called before XSAVE/after XRSTOR as well as ordinary loads.
/// False requires a flushing CR3 operand even if the caller requested noflush.
pub fn enter(cpu: usize, cr3: u64) bool {
    const pcid = cr3 & 4095;
    if (pcid == 0 or cpu >= roots.len) return false;
    const value = cr3 & root_mask;
    const slot = &roots[cpu][pcid];
    if (@atomicLoad(u64, slot, .monotonic) == value) return true;
    @atomicStore(u64, slot, value, .monotonic);
    // The request's PTE stores -> MFENCE -> residency load pairs with this
    // publication -> MFENCE -> flushing load. If the request misses us, the
    // flushing load starts after its snapshot and observes changed PTEs.
    if (!builtin.is_test) asm volatile ("mfence" ::: .{ .memory = true });
    return false;
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
    try std.testing.expect(!enter(0, a));
    try std.testing.expect(enter(0, a));
    try std.testing.expect(!enter(71, a));
    try std.testing.expect(contains(0, a) and contains(71, a));
    try std.testing.expect(!contains(65, a));
    try std.testing.expect(!enter(71, b));
    try std.testing.expect(!contains(71, a) and contains(71, b));
    try std.testing.expect(!enter(71, a));
    try std.testing.expect(!enter(0, 0x300000));
}

test "both registration snapshot orders cover the entrant" {
    var rows: [1][pcids]u64 = @splat(@splat(0));
    const before = roots;
    roots = &rows;
    defer roots = before;
    const cr3 = 0x12345001;
    // Request before first registration: entrant owes its own full flush.
    try std.testing.expect(!contains(0, cr3));
    try std.testing.expect(!enter(0, cr3));
    // Registration before request: ACK is required, even before first load.
    try std.testing.expect(contains(0, cr3));
    // Subsequent noflush entry cannot disappear from the request's snapshot.
    try std.testing.expect(enter(0, cr3));
    try std.testing.expect(contains(0, cr3));
    // Exit/reuse first requires the existing all-online full flush and no
    // old-AS entrants. Sticky metadata does not replace that lifetime fence.
    try std.testing.expect(!enter(0, 0x23456001));
}
