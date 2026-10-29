const mmio = @import("memory/mmio.zig");
const builtin = @import("builtin");
const lock_diagnostics = @import("sync/lock_diagnostics.zig");
const std = @import("std");
const address_types = @import("memory/addresses.zig");
const copy_profile = @import("lock_profile.zig");
const perf = @import("smp_perf.zig");
const kernel = @import("kernel.zig");
const kernel_log = @import("kernel_log.zig");
const scheduler = @import("scheduler_connection.zig");
const smp = @import("smp.zig");
const user_vm = @import("memory/user_vm.zig");
const ram_cache = @import("memory/ram_cache_policy.zig");
const runtime_abi = @import("kernel_abi_root").runtime_abi;
const window_ways = @import("arch/x86_64/platform.zig").phys_copy_window_ways;

const CpuSet = @import("cpu_set.zig").CpuSet;
const residency = @import("tlb_residency.zig");
const bootstrap_cpu_rows = if (builtin.is_test) 4 else 1;

pub const Hooks = struct {
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    physical_map_limit: u64,
    phys_copy_window_va: u64,
    page_present: u64,
    page_rw: u64,
    kernel_cr3_value: *const u64,
    user_space_cr3_for_principal: *const fn (principal: kernel.PrincipalId) u64,
    phys_copy_window_pt: *[512]u64,
    read_cr3: *const fn () u64,
    write_cr3: *const fn (u64) void,
    invlpg: *const fn (u64) void,
};

var user_copy_hooks_storage: Hooks = undefined;
var user_copy_hooks_ready = false;
var ram_cache_snapshot: ?*const ram_cache.Snapshot = null;

/// Only the serialized physical-cache transition owner may publish. The
/// snapshot and its slices stay immutable and mapped until an acknowledged
/// all-CPU rendezvous after replacement; the caller owns their reclamation.
pub fn publishRamCacheSnapshot(snapshot: ?*const ram_cache.Snapshot) void {
    @atomicStore(?*const ram_cache.Snapshot, &ram_cache_snapshot, snapshot, .release);
}

/// Writer-side identity check without lending out the immutable snapshot.
pub fn ramCacheSnapshotIsCurrent(snapshot: ?*const ram_cache.Snapshot) bool {
    return @atomicLoad(?*const ram_cache.Snapshot, &ram_cache_snapshot, .acquire) == snapshot;
}

/// Caller holds either its IRQ-disabled copy guard or an address-space lock
/// included in every writer's all-VM transaction. Return values own no borrow
/// into a snapshot, so they remain valid after the short lookup completes.
pub fn ramCachePolicyForLockedPage(paddr: u64) ram_cache.Snapshot.Decision {
    const snapshot = @atomicLoad(?*const ram_cache.Snapshot, &ram_cache_snapshot, .acquire) orelse
        return .ordinary;
    return snapshot.query(paddr);
}

/// Same exclusion as the per-page lookup; pending-only snapshots are active.
pub fn hasRamCachePolicyLocked() bool {
    const snapshot = @atomicLoad(?*const ram_cache.Snapshot, &ram_cache_snapshot, .acquire) orelse return false;
    return snapshot.policy.len != 0 or snapshot.pending.len != 0;
}

// The guard is returned by value. Its intrusive record must stay at a
// stable per-CPU address, not move with the caller's guard value.
var copy_window_records_seed: [if (lock_diagnostics.enabled) bootstrap_cpu_rows else 0]lock_diagnostics.Record = @splat(.{});
var copy_window_records: []lock_diagnostics.Record = &copy_window_records_seed;
const PhysCopyWindowGuard = struct {
    cpu_slot: usize,
    interrupts_were_enabled: bool = false,

    fn interruptsEnabled() bool {
        var flags: u64 = 0;
        asm volatile (
            \\pushfq
            \\pop %[flags]
            : [flags] "=r" (flags),
        );
        return (flags & (1 << 9)) != 0;
    }

    fn lock() PhysCopyWindowGuard {
        const restore_interrupts = interruptsEnabled();
        if (!builtin.is_test) asm volatile ("cli" ::: .{ .memory = true });
        const cpu = boundedCurrentCpuSlot();
        if (comptime lock_diagnostics.enabled) {
            lock_diagnostics.beforeAcquire(&copy_window_records[cpu], .{ .class = .copy_window, .order = cpu });
            lock_diagnostics.acquired(&copy_window_records[cpu], .{ .class = .copy_window, .order = cpu });
        }
        return .{
            .cpu_slot = cpu,
            .interrupts_were_enabled = restore_interrupts,
        };
    }

    fn unlock(self: PhysCopyWindowGuard) void {
        if (comptime lock_diagnostics.enabled) {
            if (boundedCurrentCpuSlot() != self.cpu_slot) lock_diagnostics.failure(error.ContextMovedCpu);
            lock_diagnostics.assertOwned(&copy_window_records[self.cpu_slot]);
        }
        // Non-WB RAM copies may leave WC stores buffered even when no device
        // VMA was involved. Finish them before allowing a transition IPI.
        if (window_non_wb_used[self.cpu_slot] != 0) {
            asm volatile ("mfence" ::: .{ .memory = true });
            window_non_wb_used[self.cpu_slot] = 0;
        }
        if (comptime lock_diagnostics.enabled) lock_diagnostics.releasing(&copy_window_records[self.cpu_slot]);
        if (!builtin.is_test and self.interrupts_were_enabled) asm volatile ("sti" ::: .{ .memory = true });
    }
};

fn boundedCurrentCpuSlot() usize {
    const cpu_slot = scheduler.currentCpu();
    return if (cpu_slot < window_victim.len) cpu_slot else 0;
}

const TlbShootdownLock = struct {
    value: u8 = 0,
    diagnostic_record: lock_diagnostics.Record = .{},

    fn interruptsEnabled() bool {
        var flags: u64 = 0;
        asm volatile (
            \\pushfq
            \\pop %[flags]
            : [flags] "=r" (flags),
        );
        return (flags & (1 << 9)) != 0;
    }

    fn lock(self: *TlbShootdownLock) void {
        while (true) {
            const diagnostic_if = lock_diagnostics.maskInterrupts();
            lock_diagnostics.beforeAcquire(&self.diagnostic_record, .{ .class = .tlb_shootdown });
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) {
                lock_diagnostics.acquired(&self.diagnostic_record, .{ .class = .tlb_shootdown });
                lock_diagnostics.restoreInterrupts(diagnostic_if);
                return;
            }
            lock_diagnostics.restoreInterrupts(diagnostic_if);
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                // A competing initiator may be waiting for this CPU's ack.
                asm volatile ("sti; pause; cli" ::: .{ .memory = true });
            }
        }
    }

    fn unlock(self: *TlbShootdownLock) void {
        const diagnostic_if = lock_diagnostics.maskInterrupts();
        defer lock_diagnostics.restoreInterrupts(diagnostic_if);
        lock_diagnostics.releasing(&self.diagnostic_record);
        @atomicStore(u8, &self.value, 0, .release);
    }
};

// X serializes senders. A receiver reads ONLY its own mailbox; publication
// and ACK bracket descriptor lifetime, including delayed/coalesced IPIs.
const Mailbox = struct {
    cr3: u64 = 0,
    range: TlbRange = .{},
    retire_base: u64 = 0,
    retire_size: u64 = 0,
    flush_caches: bool = false,
    published: u64 = 0,
    ack: u64 = 0,

    fn pending(self: *Mailbox) u64 {
        const generation = @atomicLoad(u64, &self.published, .acquire);
        return if (generation == @atomicLoad(u64, &self.ack, .monotonic)) 0 else generation;
    }

    fn nextGeneration(self: *Mailbox) u64 {
        const previous = @atomicLoad(u64, &self.published, .monotonic);
        std.debug.assert(previous == @atomicLoad(u64, &self.ack, .acquire));
        // Per-mailbox sequence: a CPU omitted for a global counter's complete
        // wrap must not mistake its old ACK for a newly targeted request.
        return if (previous == std.math.maxInt(u64)) 1 else previous + 1;
    }
};
var mailbox_seed: [bootstrap_cpu_rows]Mailbox = @splat(.{});
var mailboxes: []Mailbox = &mailbox_seed;
var copy_absent_seed: [bootstrap_cpu_rows]?u64 = @splat(null);
var copy_absent_va: []?u64 = &copy_absent_seed;
const ProbeCoverage = struct {
    const Range = struct { first: u64 = 0, last: u64 = 0 };
    active: bool = false,
    owner: kernel.PrincipalId = @enumFromInt(0),
    count: usize = 0,
    ranges: [32]Range = @splat(.{}),
    fn contains(self: *const ProbeCoverage, va: u64) bool {
        for (self.ranges[0..self.count]) |range| if (va >= range.first and va <= range.last) return true;
        return false;
    }
};
var probe_coverage_seed: [bootstrap_cpu_rows]ProbeCoverage = @splat(.{});
var probe_coverage: []ProbeCoverage = &probe_coverage_seed;
var target_seed: [CpuSet.wordCount(bootstrap_cpu_rows)]usize = @splat(0);
var shootdown_targets = CpuSet{ .words = &target_seed, .capacity = bootstrap_cpu_rows };

pub fn initCpuStorage(count: usize) !void {
    const grow = @import("diagnostic_cpu_storage.zig").grow;
    mailboxes = try grow(Mailbox, mailboxes, count, .{});
    copy_absent_va = try grow(?u64, copy_absent_va, count, null);
    probe_coverage = try grow(ProbeCoverage, probe_coverage, count, .{});
    tlb_shootdown_active = try grow(u8, tlb_shootdown_active, count, 0);
    window_victim = try grow(u8, window_victim, count, 0);
    window_non_wb_used = try grow(u8, window_non_wb_used, count, 0);
    if (comptime lock_diagnostics.enabled)
        copy_window_records = try grow(lock_diagnostics.Record, copy_window_records, count, .{});
    const words = try @import("arch/x86_64/cpu_runtime.zig").alloc(usize, CpuSet.wordCount(count));
    shootdown_targets = CpuSet.init(words, count);
}

var tlb_shootdown_lock: TlbShootdownLock = .{};
var tlb_cost_measured: bool = false;
// GDB may change this only with all vCPUs stopped, outside measurement windows.
// Zero forces full; the read clamps to 256 so full-AS/parent requests stay full.
// Normal/profile builds have no dynamic setting and no storage for it.
var tlb_range_checked_limit: [if (lock_diagnostics.enabled) 1 else 0]usize = @splat(TlbRange.page_limit);
// Local interrupt completion may copy into a COW mapping and initiate another
// shootdown. Keep it out of this CPU's non-recursive lock interval, including
// acquisition waits. Maintenance IPIs and device event publication still run.
var tlb_shootdown_active_seed: [bootstrap_cpu_rows]u8 = @splat(0);
var tlb_shootdown_active: []u8 = &tlb_shootdown_active_seed;

const slot_table = @import("memory/slot_table.zig");
const WcWindowLease = struct {
    base: u64 = 0,
    size: u64 = 0,
};
var wc_window_leases: slot_table.SlotTable(WcWindowLease) = .{};
var wc_window_leaf: slot_table.Leaf = .{};
var wc_window_active_count: usize = 0;

var test_wc_allocator: ?@import("memory/owned_storage.zig").Allocator = null;

fn wcLeaseAllocator() @import("memory/owned_storage.zig").Allocator {
    if (builtin.is_test) return test_wc_allocator orelse slot_table.hostTestAllocator();
    return @import("memory/identity_allocator.zig").allocator(user_copy_hooks_storage.free_list);
}
// Next way to evict when every way of a CPU holds another page. Only touched
// by that CPU inside its IRQ-disabled copy guard.
var window_victim_seed: [bootstrap_cpu_rows]u8 = @splat(0);
var window_victim: []u8 = &window_victim_seed;
// Set when the current guard used a non-WB way. A retained non-WB way that the
// guard did not touch has no stores in flight, so the unlock fence keys on use.
var window_non_wb_used_seed: [bootstrap_cpu_rows]u8 = @splat(0);
var window_non_wb_used: []u8 = &window_non_wb_used_seed;

/// K serializes writers and owns tokens; the directory leaf excludes copy
/// readers through the entire range check. Allocation/returns run outside it.
/// Token zero means no lease; this is internal storage, not a syscall ABI.
pub fn acquireWcPhysicalWindowLease(base: u64, size: u64) ?u64 {
    if (size == 0 or ((base | size) & 4095) != 0 or
        base > std.math.maxInt(u64) - size) return null;
    const added = wc_window_leases.allocate(wcLeaseAllocator(), &wc_window_leaf, std.math.maxInt(u32)) catch return null;
    const flags = wc_window_leaf.lock();
    added.value.* = .{ .base = base, .size = size };
    _ = @atomicRmw(usize, &wc_window_active_count, .Add, 1, .release);
    wc_window_leaf.unlock(flags);
    return @as(u64, added.ref.index) + 1;
}

pub fn releaseWcPhysicalWindowLease(token: u64) void {
    if (token == 0 or token - 1 > std.math.maxInt(u32)) unreachable;
    // K keeps the resource alive while remove withdraws it under the leaf.
    const slot = wc_window_leases.get(@intCast(token - 1)) orelse unreachable;
    wc_window_leases.remove(slot, &wc_window_leaf);
    _ = @atomicRmw(usize, &wc_window_active_count, .Sub, 1, .release);
}

fn wcPhysicalWindowLeaseBlocks(page: u64) bool {
    // The first acquire is followed by all-CPU retirement before WC PTE
    // publication. No leases keeps ordinary RAM copy off the directory path.
    if (@atomicLoad(usize, &wc_window_active_count, .acquire) == 0) return false;
    // Writers mask IRQs and do no allocation/callback/IPI work under this
    // leaf. Copy -> directory is therefore acyclic, and concurrent readers
    // serialize without spuriously denying an unrelated physical page.
    const flags = wc_window_leaf.lock();
    defer wc_window_leaf.unlock(flags);
    var iterator = wc_window_leases.iterator();
    while (iterator.next()) |entry| {
        const slot = entry.value;
        if (page >= slot.base and page - slot.base < slot.size) return true;
    }
    return false;
}

test "WC physical window leases block only overlapping pages and survive slot reuse" {
    const first = acquireWcPhysicalWindowLease(0xd0000000, 8192).?;
    var first_live = true;
    defer if (first_live) releaseWcPhysicalWindowLease(first);
    try std.testing.expect(wcPhysicalWindowLeaseBlocks(0xd0000000));
    try std.testing.expect(wcPhysicalWindowLeaseBlocks(0xd0001000));
    try std.testing.expect(!wcPhysicalWindowLeaseBlocks(0xd0002000));
    releaseWcPhysicalWindowLease(first);
    first_live = false;
    const second = acquireWcPhysicalWindowLease(0x380000000000, 4096).?;
    defer releaseWcPhysicalWindowLease(second);
    try std.testing.expectEqual(first, second);
    try std.testing.expect(!wcPhysicalWindowLeaseBlocks(0xd0000000));
    try std.testing.expect(wcPhysicalWindowLeaseBlocks(0x380000000000));
}

pub fn tlbShootdownActiveOnCurrentCpu() bool {
    return @atomicLoad(u8, &tlb_shootdown_active[boundedCurrentCpuSlot()], .acquire) != 0;
}

pub fn testSetTlbShootdownActive(cpu: usize, active: bool) void {
    if (!@import("builtin").is_test) @compileError("test-only shootdown injection");
    std.debug.assert(cpu < tlb_shootdown_active.len);
    @atomicStore(u8, &tlb_shootdown_active[cpu], @intFromBool(active), .release);
}

test "shootdown interrupt guard is CPU local and clears independently" {
    const before = try std.testing.allocator.dupe(u8, tlb_shootdown_active);
    defer {
        @memcpy(tlb_shootdown_active, before);
        std.testing.allocator.free(before);
    }
    @memset(tlb_shootdown_active, 0);
    const cpu = boundedCurrentCpuSlot();
    const other = (cpu + 1) % window_victim.len;
    testSetTlbShootdownActive(other, true);
    try std.testing.expect(!tlbShootdownActiveOnCurrentCpu());
    testSetTlbShootdownActive(cpu, true);
    try std.testing.expect(tlbShootdownActiveOnCurrentCpu());
    testSetTlbShootdownActive(cpu, false);
    try std.testing.expect(!tlbShootdownActiveOnCurrentCpu());
    try std.testing.expectEqual(@as(u8, 1), tlb_shootdown_active[other]);
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    if (comptime lock_diagnostics.enabled) end = maxStaticEnd(end, staticStorageEnd(@TypeOf(copy_window_records), &copy_window_records));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_copy_hooks_storage), &user_copy_hooks_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_copy_hooks_ready), &user_copy_hooks_ready));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ram_cache_snapshot), &ram_cache_snapshot));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(tlb_shootdown_lock), &tlb_shootdown_lock));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(tlb_cost_measured), &tlb_cost_measured));
    if (comptime lock_diagnostics.enabled) end = maxStaticEnd(end, staticStorageEnd(@TypeOf(tlb_range_checked_limit), &tlb_range_checked_limit));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(tlb_shootdown_active), &tlb_shootdown_active));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(wc_window_leases), &wc_window_leases));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(wc_window_leaf), &wc_window_leaf));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(wc_window_active_count), &wc_window_active_count));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(window_victim), &window_victim));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(window_non_wb_used), &window_non_wb_used));
    return end;
}

pub fn mapKernelRuntimeStorage(map_identity_range: *const fn (u64, usize) bool) bool {
    if (comptime lock_diagnostics.enabled) {
        if (!map_identity_range(@intFromPtr(&copy_window_records), @sizeOf(@TypeOf(copy_window_records)))) return false;
    }
    if (!map_identity_range(@intFromPtr(&user_copy_hooks_storage), @sizeOf(@TypeOf(user_copy_hooks_storage)))) return false;
    if (!map_identity_range(@intFromPtr(&user_copy_hooks_ready), @sizeOf(@TypeOf(user_copy_hooks_ready)))) return false;
    if (!map_identity_range(@intFromPtr(&ram_cache_snapshot), @sizeOf(@TypeOf(ram_cache_snapshot)))) return false;
    if (!map_identity_range(@intFromPtr(&tlb_shootdown_lock), @sizeOf(@TypeOf(tlb_shootdown_lock)))) return false;
    if (!map_identity_range(@intFromPtr(&tlb_cost_measured), @sizeOf(@TypeOf(tlb_cost_measured)))) return false;
    if (comptime lock_diagnostics.enabled) {
        if (!map_identity_range(@intFromPtr(&tlb_range_checked_limit), @sizeOf(@TypeOf(tlb_range_checked_limit)))) return false;
    }
    if (!map_identity_range(@intFromPtr(&tlb_shootdown_active), @sizeOf(@TypeOf(tlb_shootdown_active)))) return false;
    if (!map_identity_range(@intFromPtr(&wc_window_leases), @sizeOf(@TypeOf(wc_window_leases)))) return false;
    if (!map_identity_range(@intFromPtr(&wc_window_leaf), @sizeOf(@TypeOf(wc_window_leaf)))) return false;
    if (!map_identity_range(@intFromPtr(&wc_window_active_count), @sizeOf(@TypeOf(wc_window_active_count)))) return false;
    if (!map_identity_range(@intFromPtr(&window_victim), @sizeOf(@TypeOf(window_victim)))) return false;
    if (!map_identity_range(@intFromPtr(&window_non_wb_used), @sizeOf(@TypeOf(window_non_wb_used)))) return false;
    return true;
}

pub fn init(new_hooks: Hooks) void {
    user_copy_hooks_storage = new_hooks;
    user_copy_hooks_ready = true;
}

fn getHooks() *const Hooks {
    if (!user_copy_hooks_ready) unreachable;
    return &user_copy_hooks_storage;
}

fn mapPhysPageForKernelAccess(page_paddr: u64, cpu_slot: usize) ?[*]u8 {
    return mapPhysPageWithCache(page_paddr, cpu_slot, .ram);
}

const PhysWindowCache = union(enum) { ram, uncached_mmio, device: u3 };

const MappedWindow = struct {
    ptr: [*]u8,
    /// PT index (cpu * window_ways + way); device copies retire exactly this.
    slot: usize,
    /// False when an identical retained PTE was reused without INVLPG.
    remapped: bool,
};

const window_paddr_mask: u64 = 0x000f_ffff_ffff_f000;

fn windowSlotVa(slot: usize) u64 {
    if (!builtin.is_test) return @import("arch/x86_64/platform.zig").perCpuWindowVa(slot / window_ways, slot % window_ways);
    return getHooks().phys_copy_window_va + @as(u64, @intCast(slot)) * 4096;
}

fn windowEntry(slot: usize) *u64 {
    if (!builtin.is_test) return @import("arch/x86_64/platform.zig").perCpuWindowEntry(slot / window_ways, slot % window_ways);
    return &getHooks().phys_copy_window_pt[slot];
}

fn mapPhysPageWithCache(page_paddr: u64, cpu_slot: usize, cache: PhysWindowCache) ?[*]u8 {
    return (mapPhysWindow(page_paddr, cpu_slot, cache) orelse return null).ptr;
}

fn mapPhysWindow(page_paddr: u64, cpu_slot: usize, cache: PhysWindowCache) ?MappedWindow {
    const h = getHooks();
    const page_base = page_paddr & ~@as(u64, 0xFFF);
    // Each CPU retains up to window_ways PTEs. Once a physical page is leased
    // as WC, neither generic RAM copy nor explicit UC access may recreate a
    // conflicting live alias. The transition IPI retires every matching way
    // first (retirePhysCopyWindowOnCurrentCpu), so these decisions must run
    // before a retained way can be reused.
    if (cache != .device and wcPhysicalWindowLeaseBlocks(page_base)) return null;
    if (page_base >= h.physical_map_limit) return null;
    if (cpu_slot >= window_victim.len) return null;
    const first_slot = cpu_slot * window_ways;
    if (builtin.is_test and first_slot + window_ways > h.phys_copy_window_pt.len) return null;
    // PCD|PWT selects PAT entry 3 (UC); MMIO callers prove that encoding and
    // alias compatibility before mapping. RAM continues to use entry 0.
    var pat: u3 = switch (cache) {
        .ram => 0,
        .uncached_mmio => 3,
        .device => |slot| slot,
    };
    // This load is inside the IRQ-disabled copy guard. Never wait for a
    // writer here: that writer may be waiting for this CPU's shootdown ack.
    switch (ramCachePolicyForLockedPage(page_base)) {
        .blocked => return null,
        .ordinary => {},
        .cached => |policy| {
            if (cache != .ram and pat != policy.pat_slot) return null;
            pat = policy.pat_slot;
        },
    }
    const cache_flags = (@as(u64, pat & 3) << 3) | (@as(u64, pat >> 2) << 7);
    const desired = page_base | h.page_present | h.page_rw | cache_flags;
    const accessed_dirty: u64 = (1 << 5) | (1 << 6);
    // The guard pins this CPU and excludes reentry. Reusing an identical way
    // requires neither a PTE store nor local TLB invalidation.
    var same_page: ?usize = null;
    var empty: ?usize = null;
    for (first_slot..first_slot + window_ways) |slot| {
        const entry = windowEntry(slot).*;
        if ((entry & ~accessed_dirty) == desired) return publishWindow(slot, desired, false);
        if ((entry & h.page_present) == 0) {
            if (empty == null) empty = slot;
        } else if ((entry & window_paddr_mask) == page_base) {
            same_page = slot;
        }
    }
    // A page whose memory type changed replaces its own way in place: one CPU
    // never holds two aliases of one physical page with different types.
    const slot = same_page orelse empty orelse blk: {
        const way = window_victim[cpu_slot] % window_ways;
        window_victim[cpu_slot] = @intCast((way + 1) % window_ways);
        break :blk first_slot + way;
    };
    windowEntry(slot).* = desired;
    const invalidate_start = copy_profile.copyTimestamp();
    h.invlpg(windowSlotVa(slot));
    copy_profile.copyEnd(.invlpg, invalidate_start);
    if (comptime @import("builtin").is_test) assertOneWayPerPage(first_slot);
    return publishWindow(slot, desired, true);
}

fn publishWindow(slot: usize, pte: u64, remapped: bool) MappedWindow {
    // Non-WB stores may still be buffered when the guard ends; record the use
    // before the pointer escapes so unlock fences them.
    if ((pte & 0x98) != 0) window_non_wb_used[slot / window_ways] = 1;
    return .{ .ptr = @ptrFromInt(windowSlotVa(slot)), .slot = slot, .remapped = remapped };
}

fn assertOneWayPerPage(first_slot: usize) void {
    const h = getHooks();
    for (first_slot..first_slot + window_ways) |a| {
        if ((windowEntry(a).* & h.page_present) == 0) continue;
        for (a + 1..first_slot + window_ways) |b| {
            if ((windowEntry(b).* & h.page_present) == 0) continue;
            std.debug.assert((windowEntry(a).* & window_paddr_mask) !=
                (windowEntry(b).* & window_paddr_mask));
        }
    }
}

fn physWindowAddr(addr: u64, access_len: usize, cpu_slot: usize) ?[*]u8 {
    const h = getHooks();
    if (access_len == 0 or access_len > 4096) return null;
    const offset: usize = @intCast(addr & 0xFFF);
    if (offset + access_len > 4096) return null;
    const last_addr, const overflow = @addWithOverflow(addr, @as(u64, @intCast(access_len - 1)));
    if (overflow != 0 or last_addr >= h.physical_map_limit) return null;
    const page = mapPhysPageForKernelAccess(addr, cpu_slot) orelse return null;
    return page + offset;
}

fn allocatePreparedFaultPage(
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    plan: kernel.NativeVmaFaultPlan,
) ?u64 {
    if (plan.kind != .allocate_zero and plan.kind != .allocate_copy) return null;
    const paddr = (state.allocPhysicalPage(free_list) catch return null).paddr;
    return paddr;
}

fn logCowResolutionFailure(
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    principal: kernel.PrincipalId,
    page_va: u64,
    stage: []const u8,
    plan: kernel.NativeVmaFaultPlan,
    shared_lock_held: bool,
) void {
    if (!shared_lock_held) user_vm.lockSharedVmObjects();
    defer if (!shared_lock_held) user_vm.unlockSharedVmObjects();
    const store = kernel.vmObjectBackingStoreStats();
    if (state.vmaEntryForVaConst(principal, page_va)) |entry| {
        const cow_refs: u32 = if (entry.cow_table.isNull())
            0
        else if (state.nativeCowTableSlotConst(entry.cow_table)) |table|
            table.ref_count
        else
            0;
        kernel_log.writeFmt(
            "cow: failure stage={s} principal={} va=0x{x} plan={s} free_pages={}\n",
            .{
                stage,
                @intFromEnum(principal),
                page_va,
                @tagName(plan.kind),
                free_list.pageCount(),
            },
        );
        kernel_log.writeFmt(
            "cow: vma start=0x{x} pages={} private={} anonymous={} fork_cow={} cow_null={} cow_refs={}\n",
            .{
                entry.start_va,
                entry.size_bytes / 4096,
                @intFromBool(entry.flags.private),
                @intFromBool(entry.flags.anonymous),
                @intFromBool(entry.flags.fork_cow),
                @intFromBool(entry.cow_table.isNull()),
                cow_refs,
            },
        );
        kernel_log.writeFmt(
            "cow: store used={} capacity={} free={}\n",
            .{
                store.used_entries,
                store.capacity,
                store.free_entries,
            },
        );
        return;
    }
    kernel_log.writeFmt(
        "cow: failure stage={s} principal={} va=0x{x} plan={s} free_pages={} vma=none\n",
        .{
            stage,
            @intFromEnum(principal),
            page_va,
            @tagName(plan.kind),
            free_list.pageCount(),
        },
    );
    kernel_log.writeFmt(
        "cow: store used={} capacity={} free={}\n",
        .{
            store.used_entries,
            store.capacity,
            store.free_entries,
        },
    );
}

fn isCowFaultCandidate(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page_va: u64,
) bool {
    const entry = state.vmaEntryForVaConst(principal, page_va) orelse return false;
    return entry.flags.private and !entry.flags.shared and entry.prot.write and
        (entry.flags.fork_cow or !entry.flags.anonymous);
}

// The caller must hold the owner's address-space lock.  The shared-object
// lock is used only to snapshot and commit metadata. PMM allocation and zero
// happen between those sections. A COW source is revalidated and copied while
// the shared lock pins its backing identity, then committed without a gap.
pub fn resolveNativeVmaCowMappingWithAddressSpaceLocked(
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    principal: kernel.PrincipalId,
    page_va: u64,
    write_access: bool,
    instruction_fetch: bool,
) ?kernel.NativeVmaFaultMapping {
    var attempt: usize = 0;
    var last_plan: kernel.NativeVmaFaultPlan = .{};
    while (attempt < 2) : (attempt += 1) {
        const plan = blk: {
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            break :blk state.prepareNativeVmaCowMapping(principal, page_va, write_access, instruction_fetch);
        };
        last_plan = plan;
        switch (plan.kind) {
            .denied => {
                if (isCowFaultCandidate(state, principal, page_va)) {
                    logCowResolutionFailure(state, free_list, principal, page_va, "prepare", plan, false);
                }
                return null;
            },
            .ready => return plan.mapping,
            .locked_slow_path => {
                user_vm.lockSharedVmObjects();
                defer user_vm.unlockSharedVmObjects();
                // Detach can release the last shared-table reference held by
                // this process, making the old pages writable (or reclaimable)
                // elsewhere. Revoke every local PTE and wait for remote TLB
                // acknowledgements BEFORE that ownership change. Both the AS
                // and shared-object locks remain held across this handoff.
                var invalidated = false;
                if (state.vmaEntryForVaConst(principal, page_va)) |entry| {
                    if (!entry.cow_table.isNull() and
                        !state.nativeCowTableIsUnique(entry.cow_table))
                    {
                        if (entry.size_bytes > std.math.maxInt(usize) or
                            !user_vm.invalidatePresentUserLinearRegionPtes(
                                principal,
                                entry.start_va,
                                @intCast(entry.size_bytes),
                            )) return null;
                        invalidated = true;
                    }
                }
                var mapping = state.ensureNativeVmaCowMappingLockedSlow(
                    principal,
                    page_va,
                    write_access,
                    instruction_fetch,
                    free_list,
                );
                if (mapping == null) {
                    // A private pager page must be supplied before COW can copy
                    // it. Check the actual pager state while both locks still
                    // protect the VMA; other null results remain failures.
                    if (state.pendingPagerFaultPage(principal, page_va, write_access, instruction_fetch) == null)
                        logCowResolutionFailure(state, free_list, principal, page_va, "locked_slow", plan, true);
                } else if (invalidated) {
                    // Callers need not repeat the completed shootdown.
                    mapping.?.invalidate_start_va = 0;
                    mapping.?.invalidate_size_bytes = 0;
                }
                return mapping;
            },
            .allocate_zero, .allocate_copy => {},
        }
        const candidate = allocatePreparedFaultPage(state, free_list, plan) orelse {
            logCowResolutionFailure(state, free_list, principal, page_va, "page_alloc", plan, false);
            return null;
        };
        const mapping = blk: {
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            if (plan.kind == .allocate_copy) {
                if (!state.nativeVmaCowPlanSourceIsCurrent(principal, plan)) break :blk null;
                if (!kernel.KernelState.copyPhysicalPage(candidate, plan.source_paddr)) break :blk null;
            }
            break :blk state.commitNativeVmaCowMapping(principal, plan, candidate, free_list);
        };
        if (mapping) |resolved| {
            if (resolved.paddr != candidate) free_list.appendPage(candidate) catch |err| switch (err) {
                error.PageQuarantined => {},
                else => @panic("invalid PMM return"),
            };
            return resolved;
        }
        free_list.appendPage(candidate) catch |err| switch (err) {
            error.PageQuarantined => {},
            else => @panic("invalid PMM return"),
        };
    }
    logCowResolutionFailure(state, free_list, principal, page_va, "commit_retry", last_plan, false);
    return null;
}

pub fn resolveNativeVmaFaultMappingWithAddressSpaceLocked(
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    principal: kernel.PrincipalId,
    page_va: u64,
    write_access: bool,
    instruction_fetch: bool,
) ?kernel.NativeVmaFaultMapping {
    const profile_start = copy_profile.copyTimestamp();
    defer copy_profile.copyEnd(.page_fault, profile_start);
    var attempt: usize = 0;
    while (attempt < 2) : (attempt += 1) {
        const plan = blk: {
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            break :blk state.prepareNativeVmaFaultMapping(principal, page_va, write_access, instruction_fetch);
        };
        switch (plan.kind) {
            .denied => {
                // Native VMA lookup is only one resolver. A managed address
                // may be handled by the registered user fault handler next;
                // traps reports the full fault if that delegation also fails.
                return null;
            },
            .ready => return plan.mapping,
            .allocate_zero => {},
            .allocate_copy, .locked_slow_path => return null,
        }
        const candidate = allocatePreparedFaultPage(state, free_list, plan) orelse {
            const store = kernel.vmObjectBackingStoreStats();
            kernel_log.writeFmt(
                "vm: fault allocation failed principal={} va=0x{x} free_pages={} store_used={} store_free={}\n",
                .{ @intFromEnum(principal), page_va, free_list.pageCount(), store.used_entries, store.free_entries },
            );
            return null;
        };
        const mapping = blk: {
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            break :blk state.commitNativeVmaFaultMapping(principal, plan, candidate, free_list);
        };
        if (mapping) |resolved| {
            if (resolved.paddr != candidate) free_list.appendPage(candidate) catch |err| switch (err) {
                error.PageQuarantined => {},
                else => @panic("invalid PMM return"),
            };
            return resolved;
        }
        kernel_log.writeFmt("vm: fault commit retry principal={} va=0x{x} attempt={}\n", .{ @intFromEnum(principal), page_va, attempt });
        free_list.appendPage(candidate) catch |err| switch (err) {
            error.PageQuarantined => {},
            else => @panic("invalid PMM return"),
        };
    }
    return null;
}

// Both private callers hold the address-space lock through cache selection
// and the physical copy. Reacquiring it here adds no lifetime protection.
const CopyPage = struct { paddr: address_types.PhysicalAddress, cache: PhysWindowCache };

pub fn beginSyscallCopyTracking(owner: kernel.PrincipalId) void {
    const slot = boundedCurrentCpuSlot();
    copy_absent_va[slot] = null;
    probe_coverage[slot].active = true;
    probe_coverage[slot].owner = owner;
    probe_coverage[slot].count = 0;
}

pub const CopyAbsent = struct { va: u64, was_probed: bool };
pub fn takeSyscallCopyAbsent() ?CopyAbsent {
    const slot = boundedCurrentCpuSlot();
    const va = copy_absent_va[slot];
    copy_absent_va[slot] = null;
    const was_probed = if (va) |address| probe_coverage[slot].contains(address) else false;
    probe_coverage[slot].active = false;
    return if (va) |address| .{ .va = address, .was_probed = was_probed } else null;
}

test "pager user copy probe violation detector rejects unprobed page" {
    const slot = boundedCurrentCpuSlot();
    beginSyscallCopyTracking(@enumFromInt(1));
    copy_absent_va[slot] = 0x2000;
    const missed = takeSyscallCopyAbsent().?;
    try std.testing.expectEqual(@as(u64, 0x2000), missed.va);
    try std.testing.expect(!missed.was_probed);

    beginSyscallCopyTracking(@enumFromInt(1));
    probe_coverage[slot].ranges[0] = .{ .first = 0x1000, .last = 0x2fff };
    probe_coverage[slot].count = 1;
    copy_absent_va[slot] = 0x2000;
    try std.testing.expect(takeSyscallCopyAbsent().?.was_probed);

    beginSyscallCopyTracking(@enumFromInt(1));
    copy_absent_va[slot] = 0;
    try std.testing.expectEqual(@as(u64, 0), takeSyscallCopyAbsent().?.va);
}

fn requestPagerForCopyWithAddressSpaceLocked(principal: kernel.PrincipalId, page: address_types.UserVirtualAddress, write_access: bool) void {
    user_vm.lockSharedVmObjects();
    defer user_vm.unlockSharedVmObjects();
    const fault = getHooks().state.pendingPagerFaultPage(principal, page.toInt(), write_access, false) orelse return;
    const outcome = getHooks().state.requestPagerFault(fault.vmo, fault.index,
        if (write_access) @import("kernel_abi_root").pager_abi.access_write else @import("kernel_abi_root").pager_abi.access_read,
        principal, 0, 0, 0, 0) catch return;
    if (outcome == .pending) {
        const slot = boundedCurrentCpuSlot();
        // A sender may copy into another principal's previously registered
        // IPC/poll buffer. Its own syscall cannot return that principal's VA.
        if (probe_coverage[slot].active and probe_coverage[slot].owner == principal)
            copy_absent_va[slot] = page.toInt();
    }
}

pub const ProbeResult = union(enum) {
    resident,
    invalid_address,
    backing_absent: u64,
};

/// This is called before a syscall acquires its state lock or changes a
/// resource. A missing pager page is requested under AS -> shared VM -> pager
/// exclusion, then the locks are dropped. The caller returns its VA to user
/// mode, where a read fault supplies the page before the syscall is retried.
pub fn probeUserRange(principal: kernel.PrincipalId, start_va: u64, length: usize, write_access: bool) ProbeResult {
    if (length == 0) return .resident;
    const last, const overflow = @addWithOverflow(start_va, @as(u64, @intCast(length - 1)));
    if (overflow != 0) return .invalid_address;
    const coverage = &probe_coverage[boundedCurrentCpuSlot()];
    if (coverage.active) {
        if (coverage.count >= coverage.ranges.len) return .invalid_address;
        coverage.ranges[coverage.count] = .{ .first = start_va, .last = last };
        coverage.count += 1;
    }
    var page_va = start_va & ~@as(u64, 4095);
    const last_page = last & ~@as(u64, 4095);
    while (true) {
        if (!user_vm.lockAddressSpace(principal)) return .invalid_address;
        const page = address_types.UserVirtualAddress.fromInt(page_va);
        const mapped = profileResolve(principal, page, write_access);
        if (mapped == null) {
            user_vm.lockSharedVmObjects();
            const candidate = getHooks().state.pendingPagerFaultPage(principal, page_va, write_access, false);
            const outcome = if (candidate) |fault|
                getHooks().state.requestPagerFault(fault.vmo, fault.index,
                    if (write_access) @import("kernel_abi_root").pager_abi.access_write else @import("kernel_abi_root").pager_abi.access_read,
                    principal, 0, 0, 0, 0) catch .invalid
            else .invalid;
            user_vm.unlockSharedVmObjects();
            user_vm.unlockAddressSpace(principal);
            if (outcome == .pending) return .{ .backing_absent = page_va };
            if (outcome != .already_present) return .invalid_address;
        } else user_vm.unlockAddressSpace(principal);
        if (page_va == last_page) return .resident;
        page_va += 4096;
    }
}

fn ensureUserPageMappedForCopyWithAddressSpaceLocked(principal: kernel.PrincipalId, page: address_types.UserVirtualAddress, write_access: bool) ?CopyPage {
    const pa = resolveCopyPhysicalPageWithAddressSpaceLocked(principal, page, write_access) orelse return null;
    // COW can split/replace a VMA. Borrow the final entry only after resolving
    // the page, and keep A through classification and the actual copy.
    const start = copy_profile.copyTimestamp();
    defer copy_profile.copyEnd(.cache, start);
    const entry = getHooks().state.vmaEntryForVaConst(principal, page.toInt()) orelse
        return .{ .paddr = pa, .cache = .ram };
    if (!entry.device_backing) return .{ .paddr = pa, .cache = .ram };
    // RAM needs no VMO borrow. Device backing still uses shared-VM exclusion
    // for state/PAT and backing verification, before entering the copy guard.
    const lock_start = copy_profile.copyTimestamp();
    user_vm.lockSharedVmObjects();
    copy_profile.copyEnd(.device_lock, lock_start);
    defer user_vm.unlockSharedVmObjects();
    return .{ .paddr = pa, .cache = deviceCopyCache(getHooks().state, entry, page.toInt(), pa.toInt(), write_access) };
}

fn resolveCopyPhysicalPageWithAddressSpaceLocked(principal: kernel.PrincipalId, page: address_types.UserVirtualAddress, write_access: bool) ?address_types.PhysicalAddress {
    const page_va = page.toInt();
    if (user_vm.lookupUserPhysicalPageWithAddressSpaceLocked(
        principal,
        page,
        write_access,
        false,
    )) |paddr| return paddr;
    const h = getHooks();
    if (write_access) {
        const cow_mapping = resolveNativeVmaCowMappingWithAddressSpaceLocked(
            h.state,
            h.free_list,
            principal,
            page_va,
            true,
            false,
        );
        if (cow_mapping) |mapping| {
            if (mapping.invalidate_size_bytes != 0) {
                if (mapping.invalidate_size_bytes > std.math.maxInt(usize)) return null;
                if (!user_vm.invalidatePresentUserLinearRegionPtes(
                    principal,
                    mapping.invalidate_start_va,
                    @intCast(mapping.invalidate_size_bytes),
                )) return null;
            }
            var paddrs = [_]u64{mapping.paddr};
            if (user_vm.lookupUserPhysicalPageWithAddressSpaceLocked(principal, page, false, false) != null) {
                if (!user_vm.remapTrustedUserPaddrsWithProt(
                    principal,
                    page_va,
                    paddrs[0..],
                    mapping.prot,
                )) return null;
            } else if (!user_vm.mapLazyUserPaddrsWithProt(
                principal,
                page_va,
                paddrs[0..],
                mapping.prot,
            )) return null;
            return .fromInt(mapping.paddr);
        }
    }
    // A present read-only PTE is not permission to copy out through its
    // physical address when write lookup/COW resolution above failed.
    if (!write_access) {
        if (user_vm.lookupUserPhysicalPageWithAddressSpaceLocked(principal, page, false, false)) |paddr| return paddr;
    }
    const mapping = resolveNativeVmaFaultMappingWithAddressSpaceLocked(
        h.state,
        h.free_list,
        principal,
        page_va,
        write_access,
        false,
    ) orelse return null;
    if (write_access and !mapping.prot.write) return null;
    if (!user_vm.mapNativeVmaFaultPage(principal, page_va, mapping)) return null;
    return .fromInt(mapping.paddr);
}

pub fn readPhysU8(addr: u64) ?u8 {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u8), window.cpu_slot) orelse return null;
    const ptr: *volatile u8 = @ptrCast(@alignCast(window_addr));
    return ptr.*;
}

pub fn readPhysU32(addr: u64) ?u32 {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u32), window.cpu_slot) orelse return null;
    const ptr: *volatile u32 = @ptrCast(@alignCast(window_addr));
    return ptr.*;
}

inline fn uncachedMmioWordAddress(addr: u64, cpu_slot: usize) ?mmio.Borrow {
    if ((addr & 3) != 0) return null;
    // Check physical address width, all online CPUs' PAT/MTRRs, and low
    // identity aliases. High BARs need explicit UC: firmware may leave their
    // MTRR type WB. Aligned DWORDs cannot cross a page boundary.
    if (!smp.uncachedKernelMmioPageAllowed(addr & ~@as(u64, 4095))) return null;
    const page = mapPhysPageWithCache(addr, cpu_slot, .uncached_mmio) orelse return null;
    // The UC window selection and effective-type proof above authorize only
    // this DWORD, for the lifetime of the current window guard.
    return mmio.mappedAfterValidation(page + (addr & 4095), 4, .uc);
}

/// The caller must first authorize the register's BAR and exact byte range.
/// Keep the CPU pinned until after the volatile access, not just the mapping.
pub fn readUncachedMmioU32(addr: u64) ?u32 {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = uncachedMmioWordAddress(addr, window.cpu_slot) orelse return null;
    return window_addr.read(u32, 0);
}

/// The same BAR authority and cache proof as readUncachedMmioU32 apply.
pub fn writeUncachedMmioU32(addr: u64, value: u32) bool {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = uncachedMmioWordAddress(addr, window.cpu_slot) orelse return false;
    window_addr.write(u32, 0, value);
    return true;
}

pub fn readPhysU64(addr: u64) ?u64 {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u64), window.cpu_slot) orelse return null;
    const ptr: *volatile u64 = @ptrCast(@alignCast(window_addr));
    return ptr.*;
}

pub fn writePhysU8(addr: u64, value: u8) bool {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u8), window.cpu_slot) orelse return false;
    const ptr: *volatile u8 = @ptrCast(@alignCast(window_addr));
    ptr.* = value;
    return true;
}

pub fn zeroPhysicalPage(page_paddr: u64) bool {
    const stage_sample = if (comptime @import("lock_profile.zig").enabled) @import("lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("lock_profile.zig").enabled) @import("lock_profile.zig").stageEnd(.zero, stage_sample);
    if ((page_paddr & 0xFFF) != 0) return false;
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const page = mapPhysPageForKernelAccess(page_paddr, window.cpu_slot) orelse return false;
    @memset(page[0..4096], 0);
    return true;
}

// The caller owns the physical RAM and holds any VMO/COW lifetime lock.
// Slices must be ordinary kernel memory, never aliases of this temporary
// window. Keep the CPU pinned through the access, not merely PTE publication.
fn readPhysicalBytesInWindow(paddr: u64, dest: []u8, cpu_slot: usize) bool {
    if (dest.len == 0) return true;
    const address = physWindowAddr(paddr, dest.len, cpu_slot) orelse return false;
    const source: [*]const u8 = address;
    @memcpy(dest, source[0..dest.len]);
    return true;
}

fn writePhysicalBytesInWindow(paddr: u64, source: []const u8, cpu_slot: usize) bool {
    if (source.len == 0) return true;
    const address = physWindowAddr(paddr, source.len, cpu_slot) orelse return false;
    const dest: [*]u8 = address;
    @memcpy(dest[0..source.len], source);
    return true;
}

/// Read RAM bytes contained within one physical page, including high RAM.
pub fn readPhysicalBytes(paddr: u64, dest: []u8) bool {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    return readPhysicalBytesInWindow(paddr, dest, window.cpu_slot);
}

/// Write RAM bytes contained within one physical page, including high RAM.
pub fn writePhysicalBytes(paddr: u64, source: []const u8) bool {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    return writePhysicalBytesInWindow(paddr, source, window.cpu_slot);
}

fn copyPhysicalPageInWindow(dst_paddr: u64, src_paddr: u64, cpu_slot: usize) bool {
    const h = getHooks();
    if (((dst_paddr | src_paddr) & 4095) != 0 or
        dst_paddr >= h.physical_map_limit or src_paddr >= h.physical_map_limit) return false;
    // Mapping the destination may evict the source's way (or reuse it when
    // both are the same page), so the source pointer is dead once the
    // destination is mapped. A kernel-stack page keeps their lifetimes apart.
    var bytes: [4096]u8 = undefined;
    if (!readPhysicalBytesInWindow(src_paddr, &bytes, cpu_slot)) return false;
    return writePhysicalBytesInWindow(dst_paddr, &bytes, cpu_slot);
}

pub fn copyPhysicalPage(dst_paddr: u64, src_paddr: u64) bool {
    const stage_sample = if (comptime @import("lock_profile.zig").enabled) @import("lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("lock_profile.zig").enabled) @import("lock_profile.zig").stageEnd(.copy, stage_sample);
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    return copyPhysicalPageInWindow(dst_paddr, src_paddr, window.cpu_slot);
}

// The retained device VMO cannot retire while A protects this live VMA.
// Shared-VM protects mutable backing state; only an exact backing/PA match
// grants its PAT. The device alias is still retired before A is released.
fn deviceCopyCache(state: anytype, entry: anytype, va: u64, paddr: u64, write: bool) PhysWindowCache {
    const mapping = state.nativeVmaFaultMappingForEntry(entry, va, write, false) orelse return .ram;
    if (mapping.paddr == paddr) {
        if (mapping.device_pat_slot) |slot| return .{ .device = slot };
    }
    return .ram;
}

test "device copy PAT requires current permitted backing and exact PA" {
    const Mock = struct {
        mapping: ?struct { paddr: u64, device_pat_slot: ?u3 },
        fn nativeVmaFaultMappingForEntry(self: *@This(), _: void, _: u64, _: bool, _: bool) @TypeOf(self.mapping) {
            return self.mapping;
        }
    };
    for ([_]u3{ 0, 1, 3, 7 }) |pat| {
        var state = Mock{ .mapping = .{ .paddr = 0x9000, .device_pat_slot = pat } };
        try std.testing.expectEqualDeep(PhysWindowCache{ .device = pat }, deviceCopyCache(&state, {}, 0x1000, 0x9000, true));
        try std.testing.expect(deviceCopyCache(&state, {}, 0x1000, 0xa000, true) == .ram);
        state.mapping.?.device_pat_slot = null;
        try std.testing.expect(deviceCopyCache(&state, {}, 0x1000, 0x9000, true) == .ram);
        state.mapping = null;
        try std.testing.expect(deviceCopyCache(&state, {}, 0x1000, 0x9000, true) == .ram);
    }
}

fn retireDeviceCopyWindow(cache: PhysWindowCache, slot: usize) void {
    if (cache != .device) return;
    // The transient alias must not outlive the VMA whose lease proved its
    // cache type; draining WC stores also precedes a possible BO unpin. Only
    // the way this copy used is cleared; other retained ways stay valid.
    asm volatile ("mfence" ::: .{ .memory = true });
    const h = getHooks();
    windowEntry(slot).* = 0;
    const invalidate_start = copy_profile.copyTimestamp();
    h.invlpg(windowSlotVa(slot));
    copy_profile.copyEnd(.invlpg, invalidate_start);
}

// Diagnostic-only mirror keeps the normal copy's code generation untouched.
// Shared permission/cache/fault/window helpers own all protection decisions.
fn copyUserWordBytesProfiled(principal: kernel.PrincipalId, source: address_types.UserVirtualAddress, dest: []u8) bool {
    const h = getHooks();
    if (dest.len == 0) return true;

    // Private diagnosis only: a word may cross a page, so entries and chunks
    // have separate denominators. No address/word data leaves the copy path.
    const profile: ?perf.RuntimePath = if (comptime perf.enabled)
        (if (dest.len == 4) .checked_word_copy else null)
    else
        null;
    const copy_start = perf.runtimeTimestamp(profile);
    perf.runtimeAdd(profile, .calls, 1);
    // Registered first so the timer includes AS/window/CR3 deferred cleanup.
    defer perf.runtimeElapsed(profile, .total_cycles, copy_start);

    const detail_start = copy_profile.copyTimestamp();
    defer copy_profile.copyEnd(.call, detail_start);
    const cr3_start = copy_profile.copyTimestamp();
    const original_cr3 = h.read_cr3();
    copy_profile.copyEnd(.cr3_read, cr3_start);
    if (original_cr3 != h.kernel_cr3_value.*) {
        const stamp = copy_profile.copyTimestamp();
        h.write_cr3(h.kernel_cr3_value.*);
        copy_profile.copyEnd(.cr3_write, stamp);
    }
    defer {
        if (original_cr3 != h.kernel_cr3_value.*) {
            const stamp = copy_profile.copyTimestamp();
            h.write_cr3(original_cr3);
            copy_profile.copyEnd(.cr3_write, stamp);
        }
    }

    var copied: usize = 0;
    while (copied < dest.len) {
        const copied_u64: u64 = @intCast(copied);
        const current = source.offset(copied_u64) orelse return false;
        const page_va = address_types.UserVirtualAddress.fromInt(current.toInt() & ~@as(u64, 0xFFF));
        const page_off: usize = @intCast(current.toInt() & 0xFFF);
        if (comptime perf.enabled) {
            const lock_start = perf.runtimeTimestamp(profile);
            const locked = profileLockAddressSpace(principal);
            perf.runtimeElapsed(profile, .lock_wait_cycles, lock_start);
            if (!locked) return false;
        } else if (!profileLockAddressSpace(principal)) return false;
        defer {
            if (comptime perf.enabled) {
                const unlock_start = perf.runtimeTimestamp(profile);
                user_vm.unlockAddressSpace(principal);
                perf.runtimeElapsed(profile, .unlock_cycles, unlock_start);
            } else user_vm.unlockAddressSpace(principal);
        }
        const hold_start = copy_profile.copyTimestamp();
        defer copy_profile.copyEnd(.as_hold, hold_start);
        const resolved = if (comptime perf.enabled) blk: {
            perf.runtimeAdd(profile, .body_calls, 1);
            const lookup_start = perf.runtimeTimestamp(profile);
            const mapped_page = profileResolve(principal, page_va, false);
            perf.runtimeElapsed(profile, .body_cycles, lookup_start);
            break :blk mapped_page orelse {
                requestPagerForCopyWithAddressSpaceLocked(principal, page_va, false);
                return false;
            };
        } else profileResolve(principal, page_va, false) orelse {
            requestPagerForCopyWithAddressSpaceLocked(principal, page_va, false);
            return false;
        };
        const page_paddr = resolved.paddr;
        const cache = resolved.cache;
        if (page_paddr.toInt() >= h.physical_map_limit) return false;

        const page_remaining: usize = 4096 - page_off;
        const total_remaining: usize = dest.len - copied;
        const chunk_len: usize = if (total_remaining < page_remaining) total_remaining else page_remaining;

        const page_off_u64: u64 = @intCast(page_off);
        const src_paddr = page_paddr.offset(page_off_u64) orelse return false;
        if (src_paddr.toInt() >= h.physical_map_limit) return false;
        if (chunk_len == 0) return false;
        if (!src_paddr.rangeFits(chunk_len, h.physical_map_limit)) return false;

        {
            perf.runtimeAdd(profile, .claim_calls, 1);
            const window_start = perf.runtimeTimestamp(profile);
            // Runs after retirement and IRQ restoration; elapsed time may
            // include interrupts after STI, not just window CPU instructions.
            defer perf.runtimeElapsed(profile, .claim_cycles, window_start);
            const guard_start = copy_profile.copyTimestamp();
            const window = PhysCopyWindowGuard.lock();
            copy_profile.copyEnd(.guard, guard_start);
            defer window.unlock();
            const mapped_window = profileWindow(src_paddr.toInt(), window.cpu_slot, cache);
            if (profile != null) {
                if (mapped_window) |mapped|
                    perf.runtimeAdd(profile, if (mapped.remapped) .claimed else .no_claim, 1)
                else
                    perf.runtimeAdd(profile, .invalid, 1);
            }
            const mapped = mapped_window orelse return false;
            defer {
                retireDeviceCopyWindow(cache, mapped.slot);
                if (comptime perf.enabled) {
                    if (cache == .device) perf.runtimeAdd(profile, .reserved, 1);
                }
            }
            const src: [*]const u8 = @ptrCast(mapped.ptr + page_off);
            const memcpy_start = copy_profile.copyTimestamp();
            @memcpy(dest[copied .. copied + chunk_len], src[0..chunk_len]);
            copy_profile.copyEnd(.memcpy, memcpy_start);
        }
        copied += chunk_len;
    }
    return true;
}

pub fn copyUserBytesFromVa(principal: kernel.PrincipalId, src_user_va: u64, dest: []u8) bool {
    return copyFromUser(principal, .fromInt(src_user_va), dest);
}

/// Futex comparison is made under the futex lock. The entry probe has already
/// faulted the page in; this path must never allocate, queue a pager request,
/// or wait while the futex lock is held.
pub fn readUserU32Resident(principal: kernel.PrincipalId, user_va: u64) ?u32 {
    if ((user_va & 3) != 0) return null;
    if (!user_vm.lockAddressSpace(principal)) return null;
    defer user_vm.unlockAddressSpace(principal);
    return readUserU32ResidentWithAddressSpaceLocked(principal, user_va);
}

/// F owns the compare/order with WAKE; the caller keeps A from this read
/// through waiter publication so a concurrent VM replacement cannot move
/// the checked word to a different page before registration.
pub fn readUserU32ResidentWithAddressSpaceLocked(principal: kernel.PrincipalId, user_va: u64) ?u32 {
    if ((user_va & 3) != 0) return null;
    const page = address_types.UserVirtualAddress.fromInt(user_va & ~@as(u64, 4095));
    const paddr = user_vm.lookupUserPhysicalPageWithAddressSpaceLocked(principal, page, false, false) orelse return null;
    const address, const overflow = @addWithOverflow(paddr.toInt(), user_va & 4095);
    if (overflow != 0) return null;
    return readPhysU32(address);
}

/// F orders the update with wait registration and both wake selections. AS
/// retains the VMA/PTE while the pinned CPU copy window supplies a stable RAM
/// alias. In particular, a writable device PTE is not authority for atomic
/// operations on a BAR, nor is the absence of a special RAM cache policy.
/// Preflight resolves absent/COW pages before F; this path never faults.
pub fn atomicUserU32ResidentWithAddressSpaceLocked(
    principal: kernel.PrincipalId,
    user_va: u64,
    operation: runtime_abi.FutexAtomicOperation,
    operand: u32,
) ?u32 {
    if (user_va == 0 or (user_va & 3) != 0) return null;
    const vma = getHooks().state.vmaEntryForVaConst(principal, user_va) orelse return null;
    if (vma.device_backing or !vma.prot.write) return null;
    const page = address_types.UserVirtualAddress.fromInt(user_va & ~@as(u64, 4095));
    const paddr = user_vm.lookupUserPhysicalPageWithAddressSpaceLocked(principal, page, true, false) orelse return null;
    if (ramCachePolicyForLockedPage(paddr.toInt()) != .ordinary) return null;
    const address, const overflow = @addWithOverflow(paddr.toInt(), user_va & 4095);
    if (overflow != 0) return null;
    const previous = blk: {
        const window = PhysCopyWindowGuard.lock();
        defer window.unlock();
        const alias = physWindowAddr(address, @sizeOf(u32), window.cpu_slot) orelse return null;
        const word: *u32 = @ptrCast(@alignCast(alias));
        break :blk switch (operation) {
            .set => @atomicRmw(u32, word, .Xchg, operand, .seq_cst),
            .add => @atomicRmw(u32, word, .Add, operand, .seq_cst),
            .bit_or => @atomicRmw(u32, word, .Or, operand, .seq_cst),
            .and_not => @atomicRmw(u32, word, .And, ~operand, .seq_cst),
            .bit_xor => @atomicRmw(u32, word, .Xor, operand, .seq_cst),
        };
    };
    kernel.page_view_dirty.markSoftwareWrite(getHooks().state, principal, user_va);
    return previous;
}

pub fn copyFromUser(principal: kernel.PrincipalId, source: address_types.UserVirtualAddress, dest: []u8) bool {
    const stage_sample = if (comptime @import("lock_profile.zig").enabled) @import("lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("lock_profile.zig").enabled) @import("lock_profile.zig").stageEnd(.copy, stage_sample);
    if (comptime perf.enabled) {
        if (dest.len == 4) return copyUserWordBytesProfiled(principal, source, dest);
    }
    const h = getHooks();
    if (dest.len == 0) return true;

    const detail_start = copy_profile.copyTimestamp();
    defer copy_profile.copyEnd(.call, detail_start);
    const cr3_start = copy_profile.copyTimestamp();
    const original_cr3 = h.read_cr3();
    copy_profile.copyEnd(.cr3_read, cr3_start);
    if (original_cr3 != h.kernel_cr3_value.*) {
        const stamp = copy_profile.copyTimestamp();
        h.write_cr3(h.kernel_cr3_value.*);
        copy_profile.copyEnd(.cr3_write, stamp);
    }
    defer {
        if (original_cr3 != h.kernel_cr3_value.*) {
            const stamp = copy_profile.copyTimestamp();
            h.write_cr3(original_cr3);
            copy_profile.copyEnd(.cr3_write, stamp);
        }
    }

    var copied: usize = 0;
    while (copied < dest.len) {
        const copied_u64: u64 = @intCast(copied);
        const current = source.offset(copied_u64) orelse return false;
        const page_va = address_types.UserVirtualAddress.fromInt(current.toInt() & ~@as(u64, 0xFFF));
        const page_off: usize = @intCast(current.toInt() & 0xFFF);
        if (!profileLockAddressSpace(principal)) return false;
        defer user_vm.unlockAddressSpace(principal);
        const hold_start = copy_profile.copyTimestamp();
        defer copy_profile.copyEnd(.as_hold, hold_start);
        const resolved = profileResolve(principal, page_va, false) orelse {
            requestPagerForCopyWithAddressSpaceLocked(principal, page_va, false);
            return false;
        };
        const page_paddr = resolved.paddr;
        const cache = resolved.cache;
        if (page_paddr.toInt() >= h.physical_map_limit) return false;

        const page_remaining: usize = 4096 - page_off;
        const total_remaining: usize = dest.len - copied;
        const chunk_len: usize = if (total_remaining < page_remaining) total_remaining else page_remaining;

        const page_off_u64: u64 = @intCast(page_off);
        const src_paddr = page_paddr.offset(page_off_u64) orelse return false;
        if (src_paddr.toInt() >= h.physical_map_limit) return false;
        if (chunk_len == 0) return false;
        if (!src_paddr.rangeFits(chunk_len, h.physical_map_limit)) return false;

        {
            const guard_start = copy_profile.copyTimestamp();
            const window = PhysCopyWindowGuard.lock();
            copy_profile.copyEnd(.guard, guard_start);
            defer window.unlock();
            const mapped = profileWindow(src_paddr.toInt(), window.cpu_slot, cache) orelse return false;
            defer retireDeviceCopyWindow(cache, mapped.slot);
            const src: [*]const u8 = @ptrCast(mapped.ptr + page_off);
            const memcpy_start = copy_profile.copyTimestamp();
            @memcpy(dest[copied .. copied + chunk_len], src[0..chunk_len]);
            copy_profile.copyEnd(.memcpy, memcpy_start);
        }
        copied += chunk_len;
    }
    return true;
}

pub fn copyBytesToUserVa(principal: kernel.PrincipalId, dest_user_va: u64, src: []const u8) bool {
    return copyToUser(principal, .fromInt(dest_user_va), src);
}

pub fn copyToUser(principal: kernel.PrincipalId, destination: address_types.UserVirtualAddress, src: []const u8) bool {
    const stage_sample = if (comptime @import("lock_profile.zig").enabled) @import("lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("lock_profile.zig").enabled) @import("lock_profile.zig").stageEnd(.copy, stage_sample);
    const h = getHooks();
    if (src.len == 0) return true;

    const detail_start = copy_profile.copyTimestamp();
    defer copy_profile.copyEnd(.call, detail_start);
    const cr3_start = copy_profile.copyTimestamp();
    const original_cr3 = h.read_cr3();
    copy_profile.copyEnd(.cr3_read, cr3_start);
    if (original_cr3 != h.kernel_cr3_value.*) {
        const stamp = copy_profile.copyTimestamp();
        h.write_cr3(h.kernel_cr3_value.*);
        copy_profile.copyEnd(.cr3_write, stamp);
    }
    defer {
        if (original_cr3 != h.kernel_cr3_value.*) {
            const stamp = copy_profile.copyTimestamp();
            h.write_cr3(original_cr3);
            copy_profile.copyEnd(.cr3_write, stamp);
        }
    }

    var copied: usize = 0;
    while (copied < src.len) {
        const copied_u64: u64 = @intCast(copied);
        const current = destination.offset(copied_u64) orelse return false;
        const page_va = address_types.UserVirtualAddress.fromInt(current.toInt() & ~@as(u64, 0xFFF));
        const page_off: usize = @intCast(current.toInt() & 0xFFF);
        if (!profileLockAddressSpace(principal)) return false;
        defer user_vm.unlockAddressSpace(principal);
        const hold_start = copy_profile.copyTimestamp();
        defer copy_profile.copyEnd(.as_hold, hold_start);
        const resolved = profileResolve(principal, page_va, true) orelse {
            requestPagerForCopyWithAddressSpaceLocked(principal, page_va, true);
            return false;
        };
        const page_paddr = resolved.paddr;
        const cache = resolved.cache;
        if (page_paddr.toInt() >= h.physical_map_limit) return false;

        const page_remaining: usize = 4096 - page_off;
        const total_remaining: usize = src.len - copied;
        const chunk_len: usize = if (total_remaining < page_remaining) total_remaining else page_remaining;
        if (chunk_len == 0) return false;

        const page_off_u64: u64 = @intCast(page_off);
        const dst_paddr = page_paddr.offset(page_off_u64) orelse return false;
        if (dst_paddr.toInt() >= h.physical_map_limit) return false;
        if (!dst_paddr.rangeFits(chunk_len, h.physical_map_limit)) return false;

        {
            const guard_start = copy_profile.copyTimestamp();
            const window = PhysCopyWindowGuard.lock();
            copy_profile.copyEnd(.guard, guard_start);
            defer window.unlock();
            const mapped = profileWindow(dst_paddr.toInt(), window.cpu_slot, cache) orelse return false;
            defer retireDeviceCopyWindow(cache, mapped.slot);
            const dst: [*]u8 = @ptrCast(mapped.ptr + page_off);
            const memcpy_start = copy_profile.copyTimestamp();
            @memcpy(dst[0..chunk_len], src[copied .. copied + chunk_len]);
            copy_profile.copyEnd(.memcpy, memcpy_start);
        }
        kernel.page_view_dirty.markSoftwareWrite(h.state, principal, current.toInt());
        copied += chunk_len;
    }
    return true;
}

pub fn writeUserU64(principal: kernel.PrincipalId, dest_user_va: u64, value: u64) bool {
    var buf: [8]u8 = undefined;
    std.mem.writeInt(u64, buf[0..], value, .little);
    return copyBytesToUserVa(principal, dest_user_va, buf[0..]);
}

pub fn readUserU64(principal: kernel.PrincipalId, src_user_va: u64) ?u64 {
    var buf: [8]u8 = undefined;
    if (!copyUserBytesFromVa(principal, src_user_va, buf[0..])) return null;
    return std.mem.readInt(u64, buf[0..], .little);
}

// Zero pages means whole context. Overflow, kernel/shared roots and large
// spans must never accidentally become a smaller invalidation.
const TlbRange = struct {
    start: u64 = 0,
    pages: u64 = 0,
    // Same-boot mprotect + untouched working-set refill (16/256 pages, 7:1):
    // one page beat full in all three paired groups; 2..16 were inconsistent,
    // and >=32 lost in every weighted pair. Keep only the supported prefix,
    // then check end-to-end two-boot profiles; warm INVPCID cost alone is not
    // a threshold argument. Parent/shared-root requests remain full.
    const page_limit = 1;
    fn init(va: u64, bytes: usize) TlbRange {
        if (bytes == 0) return .{};
        const last, const overflow = @addWithOverflow(va, bytes - 1);
        if (overflow != 0) return .{};
        const first = va & ~@as(u64, 4095);
        const pages = ((last - first) >> 12) + 1;
        // Snapshot once while creating the request; receivers use only the
        // immutable published page count, never this diagnostic setting.
        const limit = if (comptime lock_diagnostics.enabled) blk: {
            const pointer: *volatile usize = &tlb_range_checked_limit[0];
            break :blk @min(pointer.*, 256);
        } else page_limit;
        if (pages > limit) return .{};
        return .{ .start = first, .pages = pages };
    }
};

fn flushCr3RangeOnCurrentCpu(target_cr3: u64, range: TlbRange) void {
    const h = getHooks();
    if (range.pages == 0) return flushCr3ContextOnCurrentCpu(target_cr3);
    if (h.read_cr3() == target_cr3) {
        for (0..@intCast(range.pages)) |page| h.invlpg(range.start + page * 4096);
    } else if (!builtin.is_test and @import("arch/x86_64/platform.zig").hasInvpcid()) {
        for (0..@intCast(range.pages)) |page|
            @import("arch/x86_64/platform.zig").invalidatePcidAddress(@truncate(target_cr3 & 0xfff), range.start + page * 4096);
    } else flushCr3ContextOnCurrentCpu(target_cr3);
}

// The requester cannot return/reuse pages until every CPU that may execute P
// in user mode ACKs a post-PTE-change flush, and every other CPU owes a flushing
// load before next executing P. Zero rows stay zero even after an IPI flush.
fn shootdownTargetCpuSet(cr3: u64, force_all: bool) CpuSet {
    residency.fenceBeforeSnapshot();
    shootdown_targets.clear();
    const online = smp.onlineCpuSet();
    const context = residency.UserContext.fromCr3(cr3);
    const local = smp.currentCpuSlot();
    if (!force_all) {
        for (0..online.capacity) |cpu| {
            if (online.contains(cpu) and cpu != local)
                residency.invalidateRetained(cpu, context);
        }
    }
    // ALL online CPUs, independent of rows: a previous request may have
    // cleared an active CPU's row; it can refill TLBs without a CR3 reload.
    for (0..online.capacity) |cpu| {
        if (online.contains(cpu) and (force_all or residency.isActive(cpu, context)))
            shootdown_targets.insert(cpu);
    }
    return shootdown_targets;
}

fn maintenanceWriteCr3(cr3: u64) void {
    if (builtin.is_test) getHooks().write_cr3(cr3) else @import("arch/x86_64/platform.zig").writeCr3ForMaintenance(cr3);
}

fn flushCr3ContextOnCurrentCpu(target_cr3: u64) void {
    const h = getHooks();
    const current_cr3 = h.read_cr3();
    maintenanceWriteCr3(target_cr3);
    if (current_cr3 != target_cr3) maintenanceWriteCr3(current_cr3);
}

fn flushCurrentCpuCaches() void {
    // The native Zig test backend does not assemble this privileged opcode;
    // production uses LLVM and executes it only in ring 0 after all old
    // aliases have been made non-present.
    if (comptime @import("builtin").is_test) return;
    asm volatile ("wbinvd" ::: .{ .memory = true });
}

fn retirePhysCopyWindowOnCurrentCpu(base: u64, size_bytes: u64) void {
    if (size_bytes == 0) return;
    const h = getHooks();
    const cpu_slot = boundedCurrentCpuSlot();
    const first_slot = cpu_slot * window_ways;
    if (builtin.is_test and first_slot + window_ways > h.phys_copy_window_pt.len) return;
    // Every retained way may alias the range; leaving one would keep a stale
    // memory type live after the transition acknowledges this CPU.
    for (first_slot..first_slot + window_ways) |slot| {
        const entry = windowEntry(slot).*;
        if ((entry & h.page_present) == 0) continue;
        const paddr = entry & window_paddr_mask;
        if (paddr < base or paddr - base >= size_bytes) continue;
        windowEntry(slot).* = 0;
        const invalidate_start = copy_profile.copyTimestamp();
        h.invlpg(windowSlotVa(slot));
        copy_profile.copyEnd(.invlpg, invalidate_start);
    }
}

/// Called from the scheduler IPI with the kernel page table active.
pub fn acknowledgePendingTlbShootdown() void {
    const cpu = smp.currentCpuSlot();
    if (cpu >= mailboxes.len) return;
    const box = &mailboxes[cpu];
    const generation = box.pending();
    if (generation == 0) return;
    perf.add(.tlb_received, 1);
    retirePhysCopyWindowOnCurrentCpu(box.retire_base, box.retire_size);
    if (box.cr3 != 0) flushCr3RangeOnCurrentCpu(box.cr3, box.range);
    if (box.flush_caches) flushCurrentCpuCaches();
    // All reads and hardware effects finish before the sender may overwrite.
    @atomicStore(u64, &box.ack, generation, .release);
}

fn shootdownCr3Context(target_cr3: u64) void {
    shootdownCr3ContextWithOptions(target_cr3, false, 0, 0);
}

fn shootdownCr3ContextWithCache(target_cr3: u64, flush_caches: bool) void {
    shootdownCr3ContextWithOptions(target_cr3, flush_caches, 0, 0);
}

fn shootdownCr3ContextWithOptions(target_cr3: u64, flush_caches: bool, retire_base: u64, retire_size: u64) void {
    shootdownRequest(target_cr3, .{}, flush_caches, retire_base, retire_size, true);
}

fn shootdownRequest(target_cr3: u64, requested_range: TlbRange, flush_caches: bool, retire_base: u64, retire_size: u64, force_all: bool) void {
    if (target_cr3 == 0) return;
    // Check before taking X: a remote initiator can already need our ACK.
    lock_diagnostics.assertCanWaitForIpi();
    // Host callers test admission without executing CR3/APIC operations.
    if (builtin.is_test and @hasDecl(@import("root"), "lockDiagnosticsIpiWaitReached")) {
        @import("root").lockDiagnosticsIpiWaitReached();
        return;
    }
    // Diagnostic-only warm invalidation cost; never part of profile windows.
    // Log after X unlock so the log lock cannot extend the ACK critical path.
    var costs: ?[8]u64 = null;
    defer if (costs) |values| kernel_log.writeFmt("tlb-cost: full={d} pages1={d} pages2={d} pages4={d} pages8={d} pages16={d} pages32={d} pages64={d}\n", .{ values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7] });
    const restore_interrupts = TlbShootdownLock.interruptsEnabled();
    defer if (restore_interrupts) asm volatile ("sti" ::: .{ .memory = true });
    asm volatile ("cli" ::: .{ .memory = true });
    const guard_cpu = boundedCurrentCpuSlot();
    @atomicStore(u8, &tlb_shootdown_active[guard_cpu], 1, .release);
    // Registered before the lock's defer: unlock first, clear the guard next,
    // and only then restore IF. No nested timer/IRQ copyout sees a false gap.
    defer @atomicStore(u8, &tlb_shootdown_active[guard_cpu], 0, .release);
    const lock_start = perf.timestamp();
    tlb_shootdown_lock.lock();
    perf.elapsed(.tlb_lock_wait_cycles, lock_start);
    perf.add(.tlb_requests, 1);
    defer tlb_shootdown_lock.unlock();
    if (comptime lock_diagnostics.enabled) {
        const platform = @import("arch/x86_64/platform.zig");
        if (!tlb_cost_measured and (target_cr3 & 0xfff) != 0 and platform.hasInvpcid()) {
            tlb_cost_measured = true;
            var values: [8]u64 = @splat(std.math.maxInt(u64));
            for (0..8) |_| {
                for (0..8) |index| {
                    asm volatile ("lfence" ::: .{ .memory = true });
                    const begin = platform.readTimestampCounter();
                    // Exercise the actual receiver path, including current
                    // CR3 and per-CPU feature checks, not just the opcode.
                    flushCr3RangeOnCurrentCpu(target_cr3, .{
                        .start = 0x1000,
                        .pages = if (index == 0) 0 else @as(u64, 1) << @intCast(index - 1),
                    });
                    asm volatile ("lfence" ::: .{ .memory = true });
                    values[index] = @min(values[index], platform.readTimestampCounter() - begin);
                }
            }
            costs = values;
        }
    }

    const all = force_all or (target_cr3 & 4095) == 0 or flush_caches or retire_size != 0;
    const targets = shootdownTargetCpuSet(target_cr3, all);
    const range: TlbRange = if ((target_cr3 & 4095) == 0 or flush_caches or retire_size != 0) .{} else requested_range;
    const local = smp.currentCpuSlot();
    for (0..targets.capacity) |cpu| {
        if (!targets.contains(cpu) or cpu == local) continue;
        const box = &mailboxes[cpu];
        const generation = box.nextGeneration();
        box.cr3 = target_cr3;
        box.range = range;
        box.retire_base = retire_base;
        box.retire_size = retire_size;
        box.flush_caches = flush_caches;
        @atomicStore(u64, &box.published, generation, .release);
    }

    const current_cpu = smp.currentCpuSlot();
    retirePhysCopyWindowOnCurrentCpu(retire_base, retire_size);
    flushCr3RangeOnCurrentCpu(target_cr3, range);
    if (flush_caches) flushCurrentCpuCaches();

    const send_start = perf.timestamp();
    perf.add(.tlb_targets, targets.count() - @intFromBool(targets.contains(current_cpu)));
    var cpu_slot: usize = 0;
    const broadcast_sent = smp.interruptAllOtherOnlineCpus(targets);
    while (!broadcast_sent and cpu_slot < targets.capacity) : (cpu_slot += 1) {
        const targeted = targets.contains(cpu_slot);
        if (!targeted or cpu_slot == current_cpu) continue;
        while (!smp.interruptCpu(cpu_slot)) {
            asm volatile ("sti; pause; cli" ::: .{ .memory = true });
        }
    }

    perf.elapsed(.tlb_send_cycles, send_start);
    const ack_sample = if (comptime @import("lock_profile.zig").enabled) @import("lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("lock_profile.zig").enabled) @import("lock_profile.zig").stageEnd(.ipi_ack, ack_sample);
    const ack_start = perf.timestamp();
    defer perf.elapsed(.tlb_ack_wait_cycles, ack_start);
    cpu_slot = 0;
    while (cpu_slot < targets.capacity) : (cpu_slot += 1) {
        const targeted = targets.contains(cpu_slot);
        if (!targeted or cpu_slot == current_cpu) continue;
        var spins: u32 = 0;
        const generation = @atomicLoad(u64, &mailboxes[cpu_slot].published, .monotonic);
        while (@atomicLoad(u64, &mailboxes[cpu_slot].ack, .acquire) != generation) {
            spins +%= 1;
            if ((spins & 0xFFF) == 0) {
                // Lost or coalesced IPIs must not turn correctness into a
                // timeout policy. Keep retransmitting until this generation
                // is explicitly acknowledged.
                _ = smp.interruptCpu(cpu_slot);
            }
            asm volatile ("sti; pause; cli" ::: .{ .memory = true });
        }
    }
}

/// Complete a process-private memory-barrier rendezvous on every online CPU.
///
/// Reloading the process CR3 on each CPU is a serializing operation.  Using
/// the same all-CPU rendezvous as a TLB shootdown also covers CPUs which enter
/// this address space concurrently with the request.
pub fn synchronizeUserMemoryForPrincipal(principal: kernel.PrincipalId) bool {
    const target_cr3 = getHooks().user_space_cr3_for_principal(principal);
    if (target_cr3 == 0) return false;
    shootdownCr3Context(target_cr3);
    return true;
}

pub fn flushTlbForCr3Va(target_cr3: u64, va: u64) void {
    shootdownRequest(target_cr3, TlbRange.init(va, 1), false, 0, 0, false);
}

pub fn flushTlbForCr3Range(target_cr3: u64, va: u64, size_bytes: usize) void {
    if (size_bytes == 0) return;
    shootdownRequest(target_cr3, TlbRange.init(va, size_bytes), false, 0, 0, false);
}

/// Final phase of a physical memory-type transition. All old aliases must
/// already be non-present and every other CR3 context shot down. Acknowledged
/// WBINVD on every online CPU drains even speculative old WB cache lines.
pub fn flushTlbAndCachesForCr3Range(target_cr3: u64, va: u64, size_bytes: usize) void {
    _ = va;
    if (size_bytes == 0) return;
    shootdownCr3ContextWithCache(target_cr3, true);
}

/// First phase of a WC transition: each CPU drains any in-flight use of its
/// pinned physical-copy window (interrupts are disabled while it is in use),
/// removes an old alias to this physical range, and acknowledges the IPI.
/// The caller then invalidates every user CR3 and performs the cache barrier.
pub fn retirePhysCopyWindowsForRange(kernel_cr3: u64, paddr: u64, size_bytes: usize) void {
    if (size_bytes == 0) return;
    shootdownCr3ContextWithOptions(kernel_cr3, false, paddr, @intCast(size_bytes));
}

/// AS teardown/PCID reuse is the incarnation fence, including dormant roots.
pub fn retireUserTlbForPrincipal(principal: kernel.PrincipalId) void {
    shootdownCr3Context(getHooks().user_space_cr3_for_principal(principal));
}

pub fn flushUserTlbForPrincipalVa(principal: kernel.PrincipalId, va: u64) void {
    const h = getHooks();
    const target_cr3 = h.user_space_cr3_for_principal(principal);
    flushTlbForCr3Va(target_cr3, va);
}

pub fn flushUserTlbForPrincipalRange(principal: kernel.PrincipalId, va: u64, size_bytes: usize) void {
    const h = getHooks();
    const target_cr3 = h.user_space_cr3_for_principal(principal);
    flushTlbForCr3Range(target_cr3, va, size_bytes);
}

test "copy window reads writes and copies RAM above the identity limit" {
    // One backing buffer per way: a page's bytes live in its way while mapped
    // and are written back when that way is replaced or retired, so a copy
    // whose source and destination occupy different ways is really checked.
    const Mock = struct {
        const addresses = [_]u64{ 0x2000, (16 << 30) + 4096, 20 << 30 };
        var pages: [3][4096]u8 = undefined;
        var windows: [window_ways][4096]u8 align(4096) = undefined;
        var pt: [512]u64 = [_]u64{0} ** 512;
        var current: [window_ways]?usize = [_]?usize{null} ** window_ways;
        fn writeBack(slot: usize) void {
            if (current[slot]) |old| @memcpy(&pages[old], &windows[slot]);
            current[slot] = null;
        }
        fn invalidate(va: u64) void {
            const slot: usize = @intCast((va - @intFromPtr(&windows)) / 4096);
            std.debug.assert(slot < window_ways);
            writeBack(slot);
            if ((pt[slot] & 1) == 0) return;
            const physical = pt[slot] & ~@as(u64, 4095);
            for (addresses, 0..) |address, index| {
                if (physical != address) continue;
                @memcpy(&windows[slot], &pages[index]);
                current[slot] = index;
                return;
            }
            unreachable;
        }
        fn sync() void {
            for (current, 0..) |page, slot| if (page) |index| @memcpy(&pages[index], &windows[slot]);
        }
        fn slotOf(address: u64) ?usize {
            for (pt[0..window_ways], 0..) |entry, slot| {
                if ((entry & 1) != 0 and (entry & ~@as(u64, 4095)) == address) return slot;
            }
            return null;
        }
    };
    const saved_ready = user_copy_hooks_ready;
    const saved_hooks = user_copy_hooks_storage;
    defer {
        user_copy_hooks_storage = saved_hooks;
        user_copy_hooks_ready = saved_ready;
    }
    Mock.pt = [_]u64{0} ** 512;
    Mock.current = [_]?usize{null} ** window_ways;
    window_victim[0] = 0;
    for (&Mock.pages, 0..) |*page, index| {
        for (page, 0..) |*byte, offset| byte.* = @truncate(offset * 37 + index * 13);
    }
    init(.{
        .state = undefined,
        .free_list = undefined,
        .physical_map_limit = 1 << 46,
        .phys_copy_window_va = @intFromPtr(&Mock.windows),
        .page_present = 1,
        .page_rw = 2,
        .kernel_cr3_value = undefined,
        .user_space_cr3_for_principal = undefined,
        .phys_copy_window_pt = &Mock.pt,
        .read_cr3 = undefined,
        .write_cr3 = undefined,
        .invlpg = Mock.invalidate,
    });
    var bytes: [4096]u8 = undefined;
    const source = Mock.pages[1];
    try std.testing.expect(copyPhysicalPageInWindow(Mock.addresses[2], Mock.addresses[1], 0));
    // Source and destination stay mapped in distinct ways of CPU 0.
    try std.testing.expect(Mock.slotOf(Mock.addresses[1]).? != Mock.slotOf(Mock.addresses[2]).?);
    try std.testing.expect(readPhysicalBytesInWindow(Mock.addresses[2], &bytes, 0));
    try std.testing.expectEqualSlices(u8, &source, &bytes);
    try std.testing.expect(readPhysicalBytesInWindow(Mock.addresses[1], &bytes, 0));
    try std.testing.expectEqualSlices(u8, &source, &bytes);
    Mock.sync();
    try std.testing.expectEqualSlices(u8, &source, &Mock.pages[2]);
    try std.testing.expect(copyPhysicalPageInWindow(Mock.addresses[0], Mock.addresses[1], 0));
    try std.testing.expect(copyPhysicalPageInWindow(Mock.addresses[2], Mock.addresses[0], 0));
    try std.testing.expect(readPhysicalBytesInWindow(Mock.addresses[2], &bytes, 0));
    try std.testing.expectEqualSlices(u8, &source, &bytes);
    // Same page as both source and destination reuses one way.
    try std.testing.expect(copyPhysicalPageInWindow(Mock.addresses[2], Mock.addresses[2], 0));
    try std.testing.expect(readPhysicalBytesInWindow(Mock.addresses[2], &bytes, 0));
    try std.testing.expectEqualSlices(u8, &source, &bytes);
    try std.testing.expect(writePhysicalBytesInWindow(Mock.addresses[2] + 4093, &.{ 0x12, 0x34, 0x56 }, 0));
    try std.testing.expect(readPhysicalBytesInWindow(Mock.addresses[2] + 4093, bytes[0..3], 0));
    try std.testing.expectEqualSlices(u8, &.{ 0x12, 0x34, 0x56 }, bytes[0..3]);
    const before = Mock.windows;
    const before_pt = Mock.pt;
    try std.testing.expect(!writePhysicalBytesInWindow(Mock.addresses[2] + 4095, &.{ 1, 2 }, 0));
    try std.testing.expect(!readPhysicalBytesInWindow(Mock.addresses[2] + 4095, bytes[0..2], 0));
    try std.testing.expect(!copyPhysicalPageInWindow(Mock.addresses[2] + 1, Mock.addresses[1], 0));
    try std.testing.expect(!copyPhysicalPageInWindow(1 << 46, Mock.addresses[1], 0));
    try std.testing.expect(!readPhysicalBytesInWindow(std.math.maxInt(u64), bytes[0..2], 0));
    try std.testing.expectEqualSlices(u64, &before_pt, &Mock.pt);
    for (before, Mock.windows) |old, now| try std.testing.expectEqualSlices(u8, &old, &now);
}

test "copy window preserves identical mappings and invalidates every semantic change" {
    const Mock = struct {
        var calls: usize = 0;
        var last_va: u64 = 0;
        fn invalidate(va: u64) void {
            calls += 1;
            last_va = va;
        }
    };
    const saved: ?Hooks = if (user_copy_hooks_ready) user_copy_hooks_storage else null;
    defer {
        if (saved) |h| init(h) else user_copy_hooks_ready = false;
    }
    var pt = [_]u64{0} ** 512;
    init(.{
        .state = undefined,
        .free_list = undefined,
        .physical_map_limit = 1 << 32,
        .phys_copy_window_va = 0x400000,
        .page_present = 1,
        .page_rw = 2,
        .kernel_cr3_value = undefined,
        .user_space_cr3_for_principal = undefined,
        .phys_copy_window_pt = &pt,
        .read_cr3 = undefined,
        .write_cr3 = undefined,
        .invlpg = Mock.invalidate,
    });
    window_victim[0] = 0;
    window_victim[1] = 0;
    const cpu1_va: u64 = 0x400000 + window_ways * 4096;
    Mock.calls = 0;
    try std.testing.expectEqual(@as(usize, 0x400000), @intFromPtr(mapPhysPageForKernelAccess(0x12345, 0).?));
    try std.testing.expectEqual(@as(u64, 0x12003), pt[0]);
    try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    pt[0] |= (1 << 5) | (1 << 6);
    try std.testing.expect(!mapPhysWindow(0x12000, 0, .ram).?.remapped);
    try std.testing.expectEqual(@as(u64, 0x12063), pt[0]);
    try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    // A second CPU has its own ways and virtual windows.
    try std.testing.expectEqual(@as(usize, cpu1_va), @intFromPtr(mapPhysPageForKernelAccess(0x13000, 1).?));
    try std.testing.expectEqual(cpu1_va, Mock.last_va);
    try std.testing.expectEqual(@as(u64, 0x13003), pt[window_ways]);
    try std.testing.expectEqual(@as(u64, 0x12063), pt[0]);
    for ([_]u6{ 0, 1, 2, 3, 4, 7, 8, 12, 59, 63 }) |bit| {
        @memset(pt[0..window_ways], 0);
        pt[0] = 0x12063 ^ (@as(u64, 1) << bit);
        Mock.calls = 0;
        const mapped = mapPhysWindow(0x12000, 0, .ram).?;
        try std.testing.expect(mapped.remapped);
        try std.testing.expectEqual(@as(usize, 1), Mock.calls);
        try std.testing.expectEqual(@as(u64, 0x12003), pt[mapped.slot]);
        try std.testing.expectEqual(windowSlotVa(mapped.slot), Mock.last_va);
        if (bit == 12) {
            // A different present page keeps its way; the new page takes a free one.
            try std.testing.expectEqual(@as(usize, 1), mapped.slot);
            try std.testing.expectEqual(@as(u64, 0x13063), pt[0]);
        } else {
            // Same page with another type or a non-present entry is replaced in place.
            try std.testing.expectEqual(@as(usize, 0), mapped.slot);
            for (pt[1..window_ways]) |entry| try std.testing.expectEqual(@as(u64, 0), entry);
        }
    }
    @memset(pt[1..window_ways], 0);
    const before = pt;
    Mock.calls = 0;
    try std.testing.expect(mapPhysPageForKernelAccess(1 << 32, 0) == null);
    try std.testing.expect(mapPhysPageForKernelAccess(0x12000, window_victim.len) == null);
    try std.testing.expect(mapPhysPageForKernelAccess(0x12000, 512) == null);
    try std.testing.expectEqualSlices(u64, &before, &pt);
    try std.testing.expectEqual(@as(usize, 0), Mock.calls);

    // The same CPU-local ways cover firmware's high 64-bit PCI BARs.
    user_copy_hooks_storage.physical_map_limit = 1 << 46;
    const high_bar: u64 = 0x380000000000;
    try std.testing.expectEqual(@as(usize, 0x401ffc), @intFromPtr(physWindowAddr(high_bar + 0xffc, 4, 0).?));
    try std.testing.expectEqual(high_bar | 3, pt[1]);
    try std.testing.expectEqual(@as(u64, 0x12003), pt[0]);
    try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    try std.testing.expectEqual(before[window_ways], pt[window_ways]);
    try std.testing.expect(physWindowAddr(high_bar + 0xffd, 4, 0) == null);
    try std.testing.expect(physWindowAddr(1 << 46, 4, 0) == null);
    try std.testing.expect(physWindowAddr(std.math.maxInt(u64), 4, 0) == null);
    try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    window_non_wb_used[0] = 0;
    _ = mapPhysPageWithCache(high_bar, 0, .uncached_mmio).?;
    try std.testing.expectEqual(high_bar | 0x1b, pt[1]);
    try std.testing.expectEqual(@as(usize, 2), Mock.calls);
    try std.testing.expectEqual(@as(u8, 1), window_non_wb_used[0]);
    window_non_wb_used[0] = 0;
    _ = mapPhysPageWithCache(high_bar, 0, .uncached_mmio).?;
    try std.testing.expectEqual(@as(usize, 2), Mock.calls);
    // A hit on a retained non-WB way still requires the unlock fence.
    try std.testing.expectEqual(@as(u8, 1), window_non_wb_used[0]);
    window_non_wb_used[0] = 0;
    // The RAM page kept its own way across the BAR access.
    _ = mapPhysPageForKernelAccess(0x12000, 0).?;
    try std.testing.expectEqual(@as(u64, 0x12003), pt[0]);
    try std.testing.expectEqual(@as(usize, 2), Mock.calls);
    try std.testing.expectEqual(@as(u8, 0), window_non_wb_used[0]);
    retirePhysCopyWindowOnCurrentCpu(0x13000, 4096);
    try std.testing.expectEqual(@as(u64, 0x12003), pt[0]);
    try std.testing.expectEqual(@as(usize, 2), Mock.calls);
    retirePhysCopyWindowOnCurrentCpu(0x12000, 4096);
    try std.testing.expectEqual(@as(u64, 0), pt[0]);
    try std.testing.expectEqual(high_bar | 0x1b, pt[1]);
    try std.testing.expectEqual(@as(u64, 0x400000), Mock.last_va);
    try std.testing.expectEqual(@as(usize, 3), Mock.calls);
    const lease = acquireWcPhysicalWindowLease(high_bar, 4096).?;
    defer releaseWcPhysicalWindowLease(lease);
    try std.testing.expect(mapPhysPageForKernelAccess(high_bar, 0) == null);
    try std.testing.expect(mapPhysPageWithCache(high_bar, 0, .uncached_mmio) == null);
    const device = mapPhysWindow(high_bar, 0, .{ .device = 5 }).?;
    // The device type replaces the BAR's UC way in place: never two aliases.
    try std.testing.expectEqual(@as(usize, 1), device.slot);
    try std.testing.expectEqual(high_bar | 3 | (1 << 3) | (1 << 7), pt[1]);
    pt[2] = 0x15003;
    retireDeviceCopyWindow(.{ .device = 5 }, device.slot);
    try std.testing.expectEqual(@as(u64, 0), pt[1]);
    try std.testing.expectEqual(@as(u64, 0x15003), pt[2]);
    pt[2] = 0;
    window_non_wb_used[0] = 0;

    const saved_policy = @atomicLoad(?*const ram_cache.Snapshot, &ram_cache_snapshot, .acquire);
    defer publishRamCacheSnapshot(saved_policy);
    const cached = [_]ram_cache.Span{.{
        .base = 0x12000,
        .bytes = 8192,
        .owner = 9,
        .mode = .wc,
        .pat_slot = 4,
    }};
    const active: ram_cache.Snapshot = .{ .policy = &cached };
    publishRamCacheSnapshot(&active);
    _ = mapPhysPageForKernelAccess(0x12000, 0).?;
    try std.testing.expectEqual(@as(u64, 0x12083), pt[0]);
    // CPU 1's retained WB way for the same page is replaced, not duplicated.
    _ = mapPhysPageForKernelAccess(0x13000, 1).?;
    try std.testing.expectEqual(@as(u64, 0x13083), pt[window_ways]);
    for (pt[window_ways + 1 .. 2 * window_ways]) |entry| try std.testing.expectEqual(@as(u64, 0), entry);
    try std.testing.expect(mapPhysPageWithCache(0x12000, 0, .uncached_mmio) == null);

    const pending: ram_cache.Snapshot = .{ .policy = &cached, .pending = &cached };
    publishRamCacheSnapshot(&pending);
    const pending_before = pt;
    // A retained way must not bypass a pending transition.
    try std.testing.expect(mapPhysPageForKernelAccess(0x12000, 0) == null);
    try std.testing.expect(mapPhysWindow(0x12000, 0, .{ .device = 4 }) == null);
    try std.testing.expectEqualSlices(u64, &pending_before, &pt);
    // The production writer retires these windows and waits for every CPU
    // before exposing the replacement. Host tests do not execute real IPIs.
    retirePhysCopyWindowOnCurrentCpu(0x12000, 8192);
    const restored: ram_cache.Snapshot = .{};
    publishRamCacheSnapshot(&restored);
    _ = mapPhysPageForKernelAccess(0x12000, 0).?;
    try std.testing.expectEqual(@as(u64, 0x12003), pt[0]);
    window_non_wb_used[0] = 0;
    window_non_wb_used[1] = 0;
}

test "copy-window retirement may cover sparse holes without changing other windows" {
    const Mock = struct {
        var invalidations: usize = 0;
        fn invalidate(_: u64) void {
            invalidations += 1;
        }
    };
    const saved: ?Hooks = if (user_copy_hooks_ready) user_copy_hooks_storage else null;
    const saved_policy = @atomicLoad(?*const ram_cache.Snapshot, &ram_cache_snapshot, .acquire);
    defer {
        if (saved) |h| init(h) else user_copy_hooks_ready = false;
        publishRamCacheSnapshot(saved_policy);
    }
    publishRamCacheSnapshot(null);
    var pt = [_]u64{0} ** 512;
    init(.{
        .state = undefined,
        .free_list = undefined,
        .physical_map_limit = 1 << 32,
        .phys_copy_window_va = 0x400000,
        .page_present = 1,
        .page_rw = 2,
        .kernel_cr3_value = undefined,
        .user_space_cr3_for_principal = undefined,
        .phys_copy_window_pt = &pt,
        .read_cr3 = undefined,
        .write_cr3 = undefined,
        .invlpg = Mock.invalidate,
    });
    Mock.invalidations = 0;
    // Requested pages 0x12000 and 0x15000 bound an unrelated 0x13000
    // cached window. Evicting it is safe: its next use remaps it.
    pt[0] = 0x13003;
    retirePhysCopyWindowOnCurrentCpu(0x12000, 0x4000);
    try std.testing.expectEqual(@as(u64, 0), pt[0]);
    try std.testing.expectEqual(@as(usize, 1), Mock.invalidations);
    _ = mapPhysPageForKernelAccess(0x13000, 0).?;
    try std.testing.expectEqual(@as(u64, 0x13003), pt[0]);
    try std.testing.expectEqual(@as(usize, 2), Mock.invalidations);
    pt[0] = 0x16003;
    retirePhysCopyWindowOnCurrentCpu(0x12000, 0x4000);
    try std.testing.expectEqual(@as(u64, 0x16003), pt[0]);
    try std.testing.expectEqual(@as(usize, 2), Mock.invalidations);

    // Every way of this CPU is scanned; other CPUs' ways are left alone.
    pt[0] = 0x12003;
    pt[1] = 0x16003;
    pt[2] = 0x15003;
    pt[3] = 0;
    pt[window_ways] = 0x12003;
    Mock.invalidations = 0;
    retirePhysCopyWindowOnCurrentCpu(0x12000, 0x4000);
    try std.testing.expectEqual(@as(u64, 0), pt[0]);
    try std.testing.expectEqual(@as(u64, 0x16003), pt[1]);
    try std.testing.expectEqual(@as(u64, 0), pt[2]);
    try std.testing.expectEqual(@as(u64, 0x12003), pt[window_ways]);
    try std.testing.expectEqual(@as(usize, 2), Mock.invalidations);
    // A page retired from a way remaps on its next use.
    try std.testing.expect(mapPhysWindow(0x12000, 0, .ram).?.remapped);
    try std.testing.expectEqual(@as(usize, 3), Mock.invalidations);
}

test "copy window evicts round-robin only when every way holds another page" {
    const Mock = struct {
        var invalidations: usize = 0;
        fn invalidate(_: u64) void {
            invalidations += 1;
        }
    };
    const saved: ?Hooks = if (user_copy_hooks_ready) user_copy_hooks_storage else null;
    const saved_policy = @atomicLoad(?*const ram_cache.Snapshot, &ram_cache_snapshot, .acquire);
    defer {
        if (saved) |h| init(h) else user_copy_hooks_ready = false;
        publishRamCacheSnapshot(saved_policy);
    }
    publishRamCacheSnapshot(null);
    var pt = [_]u64{0} ** 512;
    init(.{
        .state = undefined,
        .free_list = undefined,
        .physical_map_limit = 1 << 32,
        .phys_copy_window_va = 0x400000,
        .page_present = 1,
        .page_rw = 2,
        .kernel_cr3_value = undefined,
        .user_space_cr3_for_principal = undefined,
        .phys_copy_window_pt = &pt,
        .read_cr3 = undefined,
        .write_cr3 = undefined,
        .invlpg = Mock.invalidate,
    });
    window_victim[0] = 0;
    Mock.invalidations = 0;
    for (0..window_ways) |way| {
        const mapped = mapPhysWindow(0x20000 + way * 4096, 0, .ram).?;
        try std.testing.expectEqual(way, mapped.slot);
    }
    try std.testing.expectEqual(window_ways, Mock.invalidations);
    // Hits on every way need no PTE store or invalidation.
    for (0..window_ways) |way| {
        const mapped = mapPhysWindow(0x20000 + way * 4096, 0, .ram).?;
        try std.testing.expect(!mapped.remapped);
        try std.testing.expectEqual(way, mapped.slot);
    }
    try std.testing.expectEqual(window_ways, Mock.invalidations);
    const first = mapPhysWindow(0x40000, 0, .ram).?;
    const second = mapPhysWindow(0x41000, 0, .ram).?;
    try std.testing.expectEqual(@as(usize, 0), first.slot);
    try std.testing.expectEqual(@as(usize, 1), second.slot);
    try std.testing.expectEqual(window_ways + 2, Mock.invalidations);
    try std.testing.expectEqual(@as(u64, 0x22003), pt[2]);
    // A free way left by a retire is used before evicting a live one.
    pt[3] = 0;
    try std.testing.expectEqual(@as(usize, 3), mapPhysWindow(0x42000, 0, .ram).?.slot);
    try std.testing.expectEqual(@as(u64, 0x22003), pt[2]);
}

test "lock ordering shootdown actual lock and per CPU copy window record" {
    if (!lock_diagnostics.enabled) return;
    var lock_state: TlbShootdownLock = .{};
    lock_state.lock();
    try std.testing.expectEqual(lock_diagnostics.order.Class.tlb_shootdown, lock_diagnostics.testCpu().head.?.key.class);
    try lock_diagnostics.testCpu().assertCanWaitForIpi();
    const guard = PhysCopyWindowGuard.lock();
    try std.testing.expectEqual(lock_diagnostics.order.Class.copy_window, lock_diagnostics.testCpu().head.?.key.class);
    try std.testing.expectError(error.IpiWaitWithBlockingLock, lock_diagnostics.testCpu().assertCanWaitForIpi());
    guard.unlock();
    lock_state.unlock();
    lock_diagnostics.assertRootEmpty();
}

test "lock ordering shootdown actual wait entrance admits window locks" {
    if (!lock_diagnostics.enabled) return;
    const root = @import("root");
    const before = root.ipi_admissions;
    var lock_state: TlbShootdownLock = .{};
    lock_state.lock();
    shootdownCr3ContextWithOptions(4096, false, 0, 0);
    lock_state.unlock();
    try std.testing.expectEqual(before + 1, root.ipi_admissions);
    lock_diagnostics.assertRootEmpty();
}

test "WC physical window leases exceed 128 and reclaim bodies after final release" {
    const Reader = struct {
        stop: bool = false,
        fn run(self: *@This()) void {
            while (!@atomicLoad(bool, &self.stop, .acquire)) {
                // The pinned lease must remain observable through every
                // directory publication and empty-body return below.
                std.debug.assert(wcPhysicalWindowLeaseBlocks(0xf0000000));
            }
        }
    };
    const pinned = acquireWcPhysicalWindowLease(0xf0000000, 4096).?;
    var reader = Reader{};
    const thread = try std.Thread.spawn(.{}, Reader.run, .{&reader});
    var joined = false;
    defer if (!joined) {
        @atomicStore(bool, &reader.stop, true, .release);
        thread.join();
    };
    var tokens: [129]u64 = undefined;
    for (&tokens, 0..) |*token, index| token.* = acquireWcPhysicalWindowLease(0x20000000 + index * 8192, 4096).?;
    for (0..tokens.len) |index| {
        try std.testing.expect(wcPhysicalWindowLeaseBlocks(0x20000000 + index * 8192));
        try std.testing.expect(!wcPhysicalWindowLeaseBlocks(0x20001000 + index * 8192));
    }
    for (tokens) |token| releaseWcPhysicalWindowLease(token);
    @atomicStore(bool, &reader.stop, true, .release);
    thread.join();
    joined = true;
    releaseWcPhysicalWindowLease(pinned);
    try std.testing.expect(wc_window_leases.body_owner.published == null);
    try std.testing.expectEqual(@as(usize, 0), wc_window_leases.live);
    try std.testing.expectEqual(@as(usize, 0), wc_window_active_count);
    try std.testing.expect(!wcPhysicalWindowLeaseBlocks(0x20000000));
    wc_window_leases.deinit();
}

test "WC physical window leases growth failure leaves no active token and can retry" {
    const Fail = struct {
        fn acquire(_: *anyopaque, _: usize) ?@import("memory/owned_storage.zig").MappedExtent {
            return null;
        }
        fn release(_: *anyopaque, _: @import("memory/owned_storage.zig").PhysicalExtent) bool {
            unreachable;
        }
    };
    wc_window_leases.deinit();
    var context: u8 = 0;
    test_wc_allocator = .{ .context = &context, .acquire = Fail.acquire, .release = Fail.release };
    defer test_wc_allocator = null;
    try std.testing.expect(acquireWcPhysicalWindowLease(4096, 4096) == null);
    try std.testing.expectEqual(@as(usize, 0), wc_window_active_count);
    test_wc_allocator = null;
    const token = acquireWcPhysicalWindowLease(4096, 4096).?;
    releaseWcPhysicalWindowLease(token);
    wc_window_leases.deinit();
}

test "copy window TLB range alignment overflow and full threshold" {
    try std.testing.expectEqual(@as(u64, 1), TlbRange.init(4095, 1).pages);
    // Crossing a page boundary exceeds the measured one-page limit.
    try std.testing.expectEqual(@as(u64, 0), TlbRange.init(4095, 2).pages);
    try std.testing.expectEqual(@as(u64, 0x2000), TlbRange.init(0x2345, 8).start);
    try std.testing.expectEqual(@as(u64, TlbRange.page_limit), TlbRange.init(0, TlbRange.page_limit * 4096).pages);
    try std.testing.expectEqual(@as(u64, 0), TlbRange.init(0, TlbRange.page_limit * 4096 + 1).pages);
    try std.testing.expectEqual(@as(u64, 0), TlbRange.init(~@as(u64, 0), 2).pages);
    try std.testing.expectEqual(@as(u64, 0), TlbRange.init(0, 1 << 47).pages);
}

test "copy window TLB receiver uses local pages and dormant CR3 fallback" {
    const Mock = struct {
        var current: u64 = 0;
        var writes: usize = 0;
        var pages: [4]u64 = undefined;
        var count: usize = 0;
        fn read() u64 {
            return current;
        }
        fn write(value: u64) void {
            current = value;
            writes += 1;
        }
        fn invalidate(value: u64) void {
            pages[count] = value;
            count += 1;
        }
    };
    const saved_hooks = user_copy_hooks_storage;
    const saved_ready = user_copy_hooks_ready;
    defer {
        user_copy_hooks_storage = saved_hooks;
        user_copy_hooks_ready = saved_ready;
    }
    user_copy_hooks_storage = undefined;
    user_copy_hooks_storage.read_cr3 = Mock.read;
    user_copy_hooks_storage.write_cr3 = Mock.write;
    user_copy_hooks_storage.invlpg = Mock.invalidate;
    user_copy_hooks_ready = true;
    Mock.current = 0x1001;
    Mock.writes = 0;
    Mock.count = 0;
    // The checked refill experiment also exercises multi-page descriptors.
    flushCr3RangeOnCurrentCpu(0x1001, .{ .start = 0x2000, .pages = 2 });
    try std.testing.expectEqualSlices(u64, &.{ 0x2000, 0x3000 }, Mock.pages[0..Mock.count]);
    try std.testing.expectEqual(@as(usize, 0), Mock.writes);
    // A dormant PCID must still be invalidated; host has no INVPCID, so the
    // old target-load/restore sequence is required even on another CPU/AS.
    flushCr3RangeOnCurrentCpu(0x2002, TlbRange.init(0x2000, 4096));
    try std.testing.expectEqual(@as(usize, 2), Mock.writes);
    try std.testing.expectEqual(@as(u64, 0x1001), Mock.current);
    flushCr3RangeOnCurrentCpu(0x1001, .{});
    try std.testing.expectEqual(@as(usize, 3), Mock.writes);
}

test "shootdown mailbox lifetime excludes non-target CPUs and survives wrap" {
    var boxes: [72]Mailbox = @splat(.{});
    const selected = &boxes[71];
    selected.cr3 = 0x1001;
    @atomicStore(u64, &selected.published, selected.nextGeneration(), .release);
    try std.testing.expectEqual(@as(u64, 1), selected.pending());
    try std.testing.expectEqual(@as(u64, 0), boxes[0].pending());
    // ACK is the last reader action; only then may the same mailbox advance.
    @atomicStore(u64, &selected.ack, selected.pending(), .release);
    try std.testing.expectEqual(@as(u64, 0), selected.pending());
    try std.testing.expectEqual(@as(u64, 2), selected.nextGeneration());
    selected.published = std.math.maxInt(u64);
    selected.ack = selected.published;
    const wrapped = selected.nextGeneration();
    try std.testing.expectEqual(@as(u64, 1), wrapped);
    @atomicStore(u64, &selected.published, wrapped, .release);
    try std.testing.expectEqual(wrapped, selected.pending());
    // A delayed IPI on an omitted CPU cannot read another CPU's new request.
    try std.testing.expectEqual(@as(u64, 0), boxes[0].pending());
}

fn profileLockAddressSpace(principal: kernel.PrincipalId) bool {
    const start = copy_profile.copyTimestamp();
    defer copy_profile.copyEnd(.as_lock, start);
    return user_vm.lockAddressSpace(principal);
}
fn profileResolve(principal: kernel.PrincipalId, page: address_types.UserVirtualAddress, write: bool) ?CopyPage {
    const start = copy_profile.copyTimestamp();
    defer copy_profile.copyEnd(.resolve, start);
    return ensureUserPageMappedForCopyWithAddressSpaceLocked(principal, page, write);
}
fn profileWindow(pa: u64, cpu: usize, cache: PhysWindowCache) ?MappedWindow {
    const start = copy_profile.copyTimestamp();
    defer copy_profile.copyEnd(.window, start);
    return mapPhysWindow(pa, cpu, cache);
}
