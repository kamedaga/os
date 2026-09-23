const std = @import("std");
const perf = @import("smp_perf.zig");
const kernel = @import("kernel.zig");
const kernel_log = @import("kernel_log.zig");
const scheduler = @import("scheduler_connection.zig");
const smp = @import("smp.zig");
const user_vm = @import("memory/user_vm.zig");

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
        asm volatile ("cli" ::: .{ .memory = true });
        return .{
            .cpu_slot = boundedCurrentCpuSlot(),
            .interrupts_were_enabled = restore_interrupts,
        };
    }

    fn unlock(self: PhysCopyWindowGuard) void {
        if (self.interrupts_were_enabled) asm volatile ("sti" ::: .{ .memory = true });
    }
};

fn boundedCurrentCpuSlot() usize {
    const cpu_slot = scheduler.currentCpu();
    return if (cpu_slot < smp.max_cpus) cpu_slot else 0;
}

const TlbShootdownLock = struct {
    value: u8 = 0,

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
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) return;
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                // A competing initiator may be waiting for this CPU's ack.
                asm volatile ("sti; pause; cli" ::: .{ .memory = true });
            }
        }
    }

    fn unlock(self: *TlbShootdownLock) void {
        @atomicStore(u8, &self.value, 0, .release);
    }
};

var tlb_shootdown_lock: TlbShootdownLock = .{};
var tlb_shootdown_target_cr3: u64 = 0;
var tlb_shootdown_generation: u64 = 0;
var tlb_shootdown_ack: [smp.max_cpus]u64 = [_]u64{0} ** smp.max_cpus;
// Local interrupt completion may copy into a COW mapping and initiate another
// shootdown. Keep it out of this CPU's non-recursive lock interval, including
// acquisition waits. Maintenance IPIs and device event publication still run.
var tlb_shootdown_active: [smp.max_cpus]u8 = [_]u8{0} ** smp.max_cpus;

pub fn tlbShootdownActiveOnCurrentCpu() bool {
    return @atomicLoad(u8, &tlb_shootdown_active[boundedCurrentCpuSlot()], .acquire) != 0;
}

pub fn testSetTlbShootdownActive(cpu: usize, active: bool) void {
    if (!@import("builtin").is_test) @compileError("test-only shootdown injection");
    std.debug.assert(cpu < tlb_shootdown_active.len);
    @atomicStore(u8, &tlb_shootdown_active[cpu], @intFromBool(active), .release);
}

test "shootdown interrupt guard is CPU local and clears independently" {
    const before = tlb_shootdown_active;
    defer tlb_shootdown_active = before;
    @memset(&tlb_shootdown_active, 0);
    const cpu = boundedCurrentCpuSlot();
    const other = (cpu + 1) % smp.max_cpus;
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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_copy_hooks_storage), &user_copy_hooks_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_copy_hooks_ready), &user_copy_hooks_ready));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(tlb_shootdown_lock), &tlb_shootdown_lock));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(tlb_shootdown_target_cr3), &tlb_shootdown_target_cr3));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(tlb_shootdown_generation), &tlb_shootdown_generation));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(tlb_shootdown_ack), &tlb_shootdown_ack));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(tlb_shootdown_active), &tlb_shootdown_active));
    return end;
}

pub fn mapKernelRuntimeStorage(map_identity_range: *const fn (u64, usize) bool) bool {
    if (!map_identity_range(@intFromPtr(&user_copy_hooks_storage), @sizeOf(@TypeOf(user_copy_hooks_storage)))) return false;
    if (!map_identity_range(@intFromPtr(&user_copy_hooks_ready), @sizeOf(@TypeOf(user_copy_hooks_ready)))) return false;
    if (!map_identity_range(@intFromPtr(&tlb_shootdown_lock), @sizeOf(@TypeOf(tlb_shootdown_lock)))) return false;
    if (!map_identity_range(@intFromPtr(&tlb_shootdown_target_cr3), @sizeOf(@TypeOf(tlb_shootdown_target_cr3)))) return false;
    if (!map_identity_range(@intFromPtr(&tlb_shootdown_generation), @sizeOf(@TypeOf(tlb_shootdown_generation)))) return false;
    if (!map_identity_range(@intFromPtr(&tlb_shootdown_ack), @sizeOf(@TypeOf(tlb_shootdown_ack)))) return false;
    if (!map_identity_range(@intFromPtr(&tlb_shootdown_active), @sizeOf(@TypeOf(tlb_shootdown_active)))) return false;
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

const PhysWindowCache = enum { ram, uncached_mmio };

fn mapPhysPageWithCache(page_paddr: u64, cpu_slot: usize, cache: PhysWindowCache) ?[*]u8 {
    const h = getHooks();
    const page_base = page_paddr & ~@as(u64, 0xFFF);
    if (page_base >= h.physical_map_limit) return null;
    if (cpu_slot >= smp.max_cpus or cpu_slot >= h.phys_copy_window_pt.len) return null;
    const window_va = h.phys_copy_window_va + (@as(u64, @intCast(cpu_slot)) * 4096);
    // PCD|PWT selects PAT entry 3 (UC); MMIO callers prove that encoding and
    // alias compatibility before mapping. RAM continues to use entry 0.
    const cache_flags: u64 = if (cache == .uncached_mmio) (1 << 4) | (1 << 3) else 0;
    const desired = page_base | h.page_present | h.page_rw | cache_flags;
    const accessed_dirty: u64 = (1 << 5) | (1 << 6);
    // The guard pins this CPU and excludes reentry. Reusing its current
    // mapping requires neither a PTE store nor local TLB invalidation.
    if ((h.phys_copy_window_pt[cpu_slot] & ~accessed_dirty) != desired) {
        h.phys_copy_window_pt[cpu_slot] = desired;
        h.invlpg(window_va);
    }
    return @ptrFromInt(window_va);
}

fn physWindowAddr(addr: u64, access_len: usize, cpu_slot: usize) ?u64 {
    const h = getHooks();
    if (access_len == 0 or access_len > 4096) return null;
    const offset: usize = @intCast(addr & 0xFFF);
    if (offset + access_len > 4096) return null;
    const last_addr, const overflow = @addWithOverflow(addr, @as(u64, @intCast(access_len - 1)));
    if (overflow != 0 or last_addr >= h.physical_map_limit) return null;
    const page = mapPhysPageForKernelAccess(addr, cpu_slot) orelse return null;
    return @intFromPtr(page) + offset;
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
) void {
    user_vm.lockSharedVmObjects();
    defer user_vm.unlockSharedVmObjects();
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
                    logCowResolutionFailure(state, free_list, principal, page_va, "prepare", plan);
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
                                principal, entry.start_va, @intCast(entry.size_bytes),
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
                    logCowResolutionFailure(state, free_list, principal, page_va, "locked_slow", plan);
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
            logCowResolutionFailure(state, free_list, principal, page_va, "page_alloc", plan);
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
            if (resolved.paddr != candidate) free_list.appendPage(0, candidate) catch {};
            return resolved;
        }
        free_list.appendPage(0, candidate) catch {};
    }
    logCowResolutionFailure(state, free_list, principal, page_va, "commit_retry", last_plan);
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
    var attempt: usize = 0;
    while (attempt < 2) : (attempt += 1) {
        const plan = blk: {
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            break :blk state.prepareNativeVmaFaultMapping(principal, page_va, write_access, instruction_fetch);
        };
        switch (plan.kind) {
            .denied => {
                kernel_log.writeFmt("vm: fault prepare denied principal={} va=0x{x} write={} exec={}\n", .{ @intFromEnum(principal), page_va, write_access, instruction_fetch });
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
            break :blk state.commitNativeVmaFaultMapping(principal, plan, candidate);
        };
        if (mapping) |resolved| {
            if (resolved.paddr != candidate) free_list.appendPage(0, candidate) catch {};
            return resolved;
        }
        kernel_log.writeFmt("vm: fault commit retry principal={} va=0x{x} attempt={}\n", .{ @intFromEnum(principal), page_va, attempt });
        free_list.appendPage(0, candidate) catch {};
    }
    return null;
}

fn ensureUserPageMappedForCopy(principal: kernel.PrincipalId, page_va: u64, write_access: bool) ?u64 {
    if (!write_access) {
        if (user_vm.lookupUserMappedPaddrForVa(principal, page_va)) |paddr| return paddr;
    }
    const h = getHooks();
    if (!user_vm.lockAddressSpace(principal)) return null;
    defer user_vm.unlockAddressSpace(principal);
    if (write_access) {
        if (user_vm.lookupUserMappedPaddrForAccessWithAddressSpaceLocked(
            principal,
            page_va,
            true,
            false,
        )) |paddr| return paddr;
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
            if (user_vm.lookupUserMappedPaddrForVa(principal, page_va) != null) {
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
            return mapping.paddr;
        }
    }
    // A present read-only PTE is not permission to copy out through its
    // physical address when write lookup/COW resolution above failed.
    if (!write_access) {
        if (user_vm.lookupUserMappedPaddrForVa(principal, page_va)) |paddr| return paddr;
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
    var paddrs = [_]u64{mapping.paddr};
    if (!user_vm.mapLazyUserPaddrsWithProt(
        principal,
        page_va,
        paddrs[0..],
        mapping.prot,
    )) return null;
    return mapping.paddr;
}

pub fn readPhysU8(addr: u64) ?u8 {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u8), window.cpu_slot) orelse return null;
    const ptr: *volatile u8 = @ptrFromInt(window_addr);
    return ptr.*;
}

pub fn readPhysU32(addr: u64) ?u32 {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u32), window.cpu_slot) orelse return null;
    const ptr: *volatile u32 = @ptrFromInt(window_addr);
    return ptr.*;
}

fn uncachedMmioWordAddress(addr: u64, cpu_slot: usize) ?u64 {
    if ((addr & 3) != 0) return null;
    // Check physical address width, all online CPUs' PAT/MTRRs, and low
    // identity aliases. High BARs need explicit UC: firmware may leave their
    // MTRR type WB. Aligned DWORDs cannot cross a page boundary.
    if (!smp.uncachedKernelMmioPageAllowed(addr & ~@as(u64, 4095))) return null;
    const page = mapPhysPageWithCache(addr, cpu_slot, .uncached_mmio) orelse return null;
    return @intFromPtr(page) + (addr & 4095);
}

/// The caller must first authorize the register's BAR and exact byte range.
/// Keep the CPU pinned until after the volatile access, not just the mapping.
pub fn readUncachedMmioU32(addr: u64) ?u32 {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = uncachedMmioWordAddress(addr, window.cpu_slot) orelse return null;
    return @as(*volatile u32, @ptrFromInt(window_addr)).*;
}

/// The same BAR authority and cache proof as readUncachedMmioU32 apply.
pub fn writeUncachedMmioU32(addr: u64, value: u32) bool {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = uncachedMmioWordAddress(addr, window.cpu_slot) orelse return false;
    @as(*volatile u32, @ptrFromInt(window_addr)).* = value;
    return true;
}

pub fn readPhysU64(addr: u64) ?u64 {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u64), window.cpu_slot) orelse return null;
    const ptr: *volatile u64 = @ptrFromInt(window_addr);
    return ptr.*;
}

pub fn writePhysU8(addr: u64, value: u8) bool {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u8), window.cpu_slot) orelse return false;
    const ptr: *volatile u8 = @ptrFromInt(window_addr);
    ptr.* = value;
    return true;
}

pub fn zeroPhysicalPage(page_paddr: u64) bool {
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
    const source: [*]const u8 = @ptrFromInt(address);
    @memcpy(dest, source[0..dest.len]);
    return true;
}

fn writePhysicalBytesInWindow(paddr: u64, source: []const u8, cpu_slot: usize) bool {
    if (source.len == 0) return true;
    const address = physWindowAddr(paddr, source.len, cpu_slot) orelse return false;
    const dest: [*]u8 = @ptrFromInt(address);
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
    // One slot per CPU: remapping the destination invalidates the source
    // pointer. A kernel-stack page keeps their lifetimes strictly separate.
    var bytes: [4096]u8 = undefined;
    if (!readPhysicalBytesInWindow(src_paddr, &bytes, cpu_slot)) return false;
    return writePhysicalBytesInWindow(dst_paddr, &bytes, cpu_slot);
}

pub fn copyPhysicalPage(dst_paddr: u64, src_paddr: u64) bool {
    const window = PhysCopyWindowGuard.lock();
    defer window.unlock();
    return copyPhysicalPageInWindow(dst_paddr, src_paddr, window.cpu_slot);
}

pub fn copyUserBytesFromVa(principal: kernel.PrincipalId, src_user_va: u64, dest: []u8) bool {
    const h = getHooks();
    if (dest.len == 0) return true;

    const original_cr3 = h.read_cr3();
    if (original_cr3 != h.kernel_cr3_value.*) {
        h.write_cr3(h.kernel_cr3_value.*);
    }
    defer {
        if (original_cr3 != h.kernel_cr3_value.*) {
            h.write_cr3(original_cr3);
        }
    }

    var copied: usize = 0;
    while (copied < dest.len) {
        const copied_u64: u64 = @intCast(copied);
        const cur_va, const va_overflow = @addWithOverflow(src_user_va, copied_u64);
        if (va_overflow != 0) return false;

        const page_va = cur_va & ~@as(u64, 0xFFF);
        const page_off: usize = @intCast(cur_va & 0xFFF);
        const page_paddr = ensureUserPageMappedForCopy(principal, page_va, false) orelse return false;
        if (page_paddr >= h.physical_map_limit) return false;

        const page_remaining: usize = 4096 - page_off;
        const total_remaining: usize = dest.len - copied;
        const chunk_len: usize = if (total_remaining < page_remaining) total_remaining else page_remaining;

        const page_off_u64: u64 = @intCast(page_off);
        const src_paddr, const paddr_overflow = @addWithOverflow(page_paddr, page_off_u64);
        if (paddr_overflow != 0 or src_paddr >= h.physical_map_limit) return false;
        if (chunk_len == 0) return false;
        const last_paddr, const last_overflow = @addWithOverflow(src_paddr, @as(u64, @intCast(chunk_len - 1)));
        if (last_overflow != 0 or last_paddr >= h.physical_map_limit) return false;

        {
            const window = PhysCopyWindowGuard.lock();
            defer window.unlock();
            const src_page = mapPhysPageForKernelAccess(src_paddr, window.cpu_slot) orelse return false;
            const src: [*]const u8 = @ptrCast(src_page + page_off);
            @memcpy(dest[copied .. copied + chunk_len], src[0..chunk_len]);
        }
        copied += chunk_len;
    }
    return true;
}

pub fn copyBytesToUserVa(principal: kernel.PrincipalId, dest_user_va: u64, src: []const u8) bool {
    const h = getHooks();
    if (src.len == 0) return true;

    const original_cr3 = h.read_cr3();
    if (original_cr3 != h.kernel_cr3_value.*) {
        h.write_cr3(h.kernel_cr3_value.*);
    }
    defer {
        if (original_cr3 != h.kernel_cr3_value.*) {
            h.write_cr3(original_cr3);
        }
    }

    var copied: usize = 0;
    while (copied < src.len) {
        const copied_u64: u64 = @intCast(copied);
        const cur_va, const va_overflow = @addWithOverflow(dest_user_va, copied_u64);
        if (va_overflow != 0) return false;

        const page_va = cur_va & ~@as(u64, 0xFFF);
        const page_off: usize = @intCast(cur_va & 0xFFF);
        const page_paddr = ensureUserPageMappedForCopy(principal, page_va, true) orelse return false;
        if (page_paddr >= h.physical_map_limit) return false;

        const page_remaining: usize = 4096 - page_off;
        const total_remaining: usize = src.len - copied;
        const chunk_len: usize = if (total_remaining < page_remaining) total_remaining else page_remaining;
        if (chunk_len == 0) return false;

        const page_off_u64: u64 = @intCast(page_off);
        const dst_paddr, const paddr_overflow = @addWithOverflow(page_paddr, page_off_u64);
        if (paddr_overflow != 0 or dst_paddr >= h.physical_map_limit) return false;
        const last_paddr, const last_overflow = @addWithOverflow(dst_paddr, @as(u64, @intCast(chunk_len - 1)));
        if (last_overflow != 0 or last_paddr >= h.physical_map_limit) return false;

        {
            const window = PhysCopyWindowGuard.lock();
            defer window.unlock();
            const dst_page = mapPhysPageForKernelAccess(dst_paddr, window.cpu_slot) orelse return false;
            const dst: [*]u8 = @ptrCast(dst_page + page_off);
            @memcpy(dst[0..chunk_len], src[copied .. copied + chunk_len]);
        }
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

fn flushCr3ContextOnCurrentCpu(target_cr3: u64) void {
    const h = getHooks();
    const current_cr3 = h.read_cr3();
    h.write_cr3(target_cr3);
    if (current_cr3 != target_cr3) h.write_cr3(current_cr3);
}

/// Called from the scheduler IPI with the kernel page table active.
pub fn acknowledgePendingTlbShootdown() void {
    const cpu_slot = smp.currentCpuSlot();
    if (cpu_slot >= tlb_shootdown_ack.len) return;
    const generation = @atomicLoad(u64, &tlb_shootdown_generation, .acquire);
    if (generation == 0 or @atomicLoad(u64, &tlb_shootdown_ack[cpu_slot], .monotonic) == generation) return;
    perf.add(.tlb_received, 1);
    const target_cr3 = @atomicLoad(u64, &tlb_shootdown_target_cr3, .monotonic);
    if (target_cr3 != 0) flushCr3ContextOnCurrentCpu(target_cr3);
    @atomicStore(u64, &tlb_shootdown_ack[cpu_slot], generation, .release);
}

fn shootdownCr3Context(target_cr3: u64) void {
    if (target_cr3 == 0) return;
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

    // A CPU can enter this CR3 after a "currently running" snapshot but
    // before the changed PTE is flushed. Broadcast to every online CPU so
    // both current users and concurrent entrants observe the new mapping.
    const target_cpu_mask = smp.onlineCpuMask();

    var generation = @atomicLoad(u64, &tlb_shootdown_generation, .monotonic) +% 1;
    if (generation == 0) generation = 1;
    @atomicStore(u64, &tlb_shootdown_target_cr3, target_cr3, .monotonic);
    @atomicStore(u64, &tlb_shootdown_generation, generation, .release);

    const current_cpu = smp.currentCpuSlot();
    flushCr3ContextOnCurrentCpu(target_cr3);
    if (current_cpu < tlb_shootdown_ack.len) {
        @atomicStore(u64, &tlb_shootdown_ack[current_cpu], generation, .release);
    }

    const send_start = perf.timestamp();
    perf.add(.tlb_targets, @popCount(target_cpu_mask) -| 1);
    var cpu_slot: usize = 0;
    const broadcast_sent = smp.interruptAllOtherOnlineCpus(target_cpu_mask);
    while (!broadcast_sent and cpu_slot < smp.max_cpus and cpu_slot < 64) : (cpu_slot += 1) {
        const targeted = (target_cpu_mask & (@as(u64, 1) << @intCast(cpu_slot))) != 0;
        if (!targeted or cpu_slot == current_cpu) continue;
        while (!smp.interruptCpu(cpu_slot)) {
            asm volatile ("sti; pause; cli" ::: .{ .memory = true });
        }
    }

    perf.elapsed(.tlb_send_cycles, send_start);
    const ack_start = perf.timestamp();
    defer perf.elapsed(.tlb_ack_wait_cycles, ack_start);
    cpu_slot = 0;
    while (cpu_slot < smp.max_cpus and cpu_slot < 64) : (cpu_slot += 1) {
        const targeted = (target_cpu_mask & (@as(u64, 1) << @intCast(cpu_slot))) != 0;
        if (!targeted or cpu_slot == current_cpu) continue;
        var spins: u32 = 0;
        while (@atomicLoad(u64, &tlb_shootdown_ack[cpu_slot], .acquire) != generation) {
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
    _ = va;
    shootdownCr3Context(target_cr3);
}

pub fn flushTlbForCr3Range(target_cr3: u64, va: u64, size_bytes: usize) void {
    _ = va;
    if (size_bytes == 0) return;
    shootdownCr3Context(target_cr3);
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
    const Mock = struct {
        const addresses = [_]u64{ 0x2000, (16 << 30) + 4096, 20 << 30 };
        var pages: [3][4096]u8 = undefined;
        var window: [4096]u8 align(4096) = undefined;
        var pt: [512]u64 = [_]u64{0} ** 512;
        var current: ?usize = null;
        fn invalidate(va: u64) void {
            std.debug.assert(va == @intFromPtr(&window));
            if (current) |old| @memcpy(&pages[old], &window);
            const physical = pt[0] & ~@as(u64, 4095);
            for (addresses, 0..) |address, index| {
                if (physical != address) continue;
                @memcpy(&window, &pages[index]);
                current = index;
                return;
            }
            unreachable;
        }
    };
    const saved_ready = user_copy_hooks_ready;
    const saved_hooks = user_copy_hooks_storage;
    defer {
        user_copy_hooks_storage = saved_hooks;
        user_copy_hooks_ready = saved_ready;
    }
    Mock.pt = [_]u64{0} ** 512;
    Mock.current = null;
    for (&Mock.pages, 0..) |*page, index| {
        for (page, 0..) |*byte, offset| byte.* = @truncate(offset * 37 + index * 13);
    }
    init(.{
        .state = undefined,
        .free_list = undefined,
        .physical_map_limit = 1 << 46,
        .phys_copy_window_va = @intFromPtr(&Mock.window),
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
    try std.testing.expect(readPhysicalBytesInWindow(Mock.addresses[2], &bytes, 0));
    try std.testing.expectEqualSlices(u8, &source, &bytes);
    try std.testing.expect(readPhysicalBytesInWindow(Mock.addresses[1], &bytes, 0));
    try std.testing.expectEqualSlices(u8, &source, &bytes);
    try std.testing.expectEqualSlices(u8, &source, &Mock.pages[2]);
    try std.testing.expect(copyPhysicalPageInWindow(Mock.addresses[0], Mock.addresses[1], 0));
    try std.testing.expect(copyPhysicalPageInWindow(Mock.addresses[2], Mock.addresses[0], 0));
    try std.testing.expect(readPhysicalBytesInWindow(Mock.addresses[2], &bytes, 0));
    try std.testing.expectEqualSlices(u8, &source, &bytes);
    try std.testing.expect(writePhysicalBytesInWindow(Mock.addresses[2] + 4093, &.{ 0x12, 0x34, 0x56 }, 0));
    try std.testing.expect(readPhysicalBytesInWindow(Mock.addresses[2] + 4093, bytes[0..3], 0));
    try std.testing.expectEqualSlices(u8, &.{ 0x12, 0x34, 0x56 }, bytes[0..3]);
    const before = Mock.window;
    const before_pte = Mock.pt[0];
    try std.testing.expect(!writePhysicalBytesInWindow(Mock.addresses[2] + 4095, &.{ 1, 2 }, 0));
    try std.testing.expect(!readPhysicalBytesInWindow(Mock.addresses[2] + 4095, bytes[0..2], 0));
    try std.testing.expect(!copyPhysicalPageInWindow(Mock.addresses[2] + 1, Mock.addresses[1], 0));
    try std.testing.expect(!copyPhysicalPageInWindow(1 << 46, Mock.addresses[1], 0));
    try std.testing.expect(!readPhysicalBytesInWindow(std.math.maxInt(u64), bytes[0..2], 0));
    try std.testing.expectEqual(before_pte, Mock.pt[0]);
    try std.testing.expectEqualSlices(u8, &before, &Mock.window);
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
    Mock.calls = 0;
    try std.testing.expectEqual(@as(usize, 0x400000), @intFromPtr(mapPhysPageForKernelAccess(0x12345, 0).?));
    try std.testing.expectEqual(@as(u64, 0x12003), pt[0]);
    try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    pt[0] |= (1 << 5) | (1 << 6);
    _ = mapPhysPageForKernelAccess(0x12000, 0).?;
    try std.testing.expectEqual(@as(u64, 0x12063), pt[0]);
    try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    // A second CPU has a distinct slot and virtual window.
    try std.testing.expectEqual(@as(usize, 0x401000), @intFromPtr(mapPhysPageForKernelAccess(0x13000, 1).?));
    try std.testing.expectEqual(@as(u64, 0x401000), Mock.last_va);
    try std.testing.expectEqual(@as(u64, 0x12063), pt[0]);
    for ([_]u6{ 0, 1, 2, 3, 4, 7, 8, 12, 59, 63 }) |bit| {
        pt[0] = 0x12063 ^ (@as(u64, 1) << bit);
        Mock.calls = 0;
        _ = mapPhysPageForKernelAccess(0x12000, 0).?;
        try std.testing.expectEqual(@as(u64, 0x12003), pt[0]);
        try std.testing.expectEqual(@as(u64, 0x400000), Mock.last_va);
        try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    }
    const before = pt;
    Mock.calls = 0;
    try std.testing.expect(mapPhysPageForKernelAccess(1 << 32, 0) == null);
    try std.testing.expect(mapPhysPageForKernelAccess(0x12000, smp.max_cpus) == null);
    try std.testing.expect(mapPhysPageForKernelAccess(0x12000, 512) == null);
    try std.testing.expectEqualSlices(u64, &before, &pt);
    try std.testing.expectEqual(@as(usize, 0), Mock.calls);

    // The same CPU-local window covers firmware's high 64-bit PCI BARs.
    user_copy_hooks_storage.physical_map_limit = 1 << 46;
    const high_bar: u64 = 0x380000000000;
    try std.testing.expectEqual(@as(?u64, 0x400ffc), physWindowAddr(high_bar + 0xffc, 4, 0));
    try std.testing.expectEqual(high_bar | 3, pt[0]);
    try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    try std.testing.expectEqual(before[1], pt[1]);
    try std.testing.expect(physWindowAddr(high_bar + 0xffd, 4, 0) == null);
    try std.testing.expect(physWindowAddr(1 << 46, 4, 0) == null);
    try std.testing.expect(physWindowAddr(std.math.maxInt(u64), 4, 0) == null);
    try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    _ = mapPhysPageWithCache(high_bar, 0, .uncached_mmio).?;
    try std.testing.expectEqual(high_bar | 0x1b, pt[0]);
    try std.testing.expectEqual(@as(usize, 2), Mock.calls);
    _ = mapPhysPageWithCache(high_bar, 0, .uncached_mmio).?;
    try std.testing.expectEqual(@as(usize, 2), Mock.calls);
    // Returning to a RAM page must replace the high PFN and invalidate locally.
    _ = mapPhysPageForKernelAccess(0x12000, 0).?;
    try std.testing.expectEqual(@as(u64, 0x12003), pt[0]);
    try std.testing.expectEqual(@as(usize, 3), Mock.calls);
}
