const std = @import("std");
const kernel = @import("kernel.zig");
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
    return end;
}

pub fn mapKernelRuntimeStorage(map_identity_range: *const fn (u64, usize) bool) bool {
    if (!map_identity_range(@intFromPtr(&user_copy_hooks_storage), @sizeOf(@TypeOf(user_copy_hooks_storage)))) return false;
    if (!map_identity_range(@intFromPtr(&user_copy_hooks_ready), @sizeOf(@TypeOf(user_copy_hooks_ready)))) return false;
    if (!map_identity_range(@intFromPtr(&tlb_shootdown_lock), @sizeOf(@TypeOf(tlb_shootdown_lock)))) return false;
    if (!map_identity_range(@intFromPtr(&tlb_shootdown_target_cr3), @sizeOf(@TypeOf(tlb_shootdown_target_cr3)))) return false;
    if (!map_identity_range(@intFromPtr(&tlb_shootdown_generation), @sizeOf(@TypeOf(tlb_shootdown_generation)))) return false;
    if (!map_identity_range(@intFromPtr(&tlb_shootdown_ack), @sizeOf(@TypeOf(tlb_shootdown_ack)))) return false;
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
    const h = getHooks();
    const page_base = page_paddr & ~@as(u64, 0xFFF);
    if (page_base >= h.physical_map_limit) return null;
    if (cpu_slot >= smp.max_cpus or cpu_slot >= h.phys_copy_window_pt.len) return null;
    const window_va = h.phys_copy_window_va + (@as(u64, @intCast(cpu_slot)) * 4096);
    h.phys_copy_window_pt[cpu_slot] = page_base | h.page_present | h.page_rw;
    h.invlpg(window_va);
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
    while (attempt < 2) : (attempt += 1) {
        const plan = blk: {
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            break :blk state.prepareNativeVmaCowMapping(principal, page_va, write_access, instruction_fetch);
        };
        switch (plan.kind) {
            .denied => return null,
            .ready => return plan.mapping,
            .locked_slow_path => {
                user_vm.lockSharedVmObjects();
                defer user_vm.unlockSharedVmObjects();
                return state.ensureNativeVmaCowMappingLockedSlow(
                    principal,
                    page_va,
                    write_access,
                    instruction_fetch,
                    free_list,
                );
            },
            .allocate_zero, .allocate_copy => {},
        }
        const candidate = allocatePreparedFaultPage(state, free_list, plan) orelse return null;
        const mapping = blk: {
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            if (plan.kind == .allocate_copy) {
                if (!state.nativeVmaCowPlanSourceIsCurrent(principal, plan)) break :blk null;
                kernel.KernelState.copyPhysicalPage(candidate, plan.source_paddr);
            }
            break :blk state.commitNativeVmaCowMapping(principal, plan, candidate, free_list);
        };
        if (mapping) |resolved| {
            if (resolved.paddr != candidate) free_list.appendPage(0, candidate) catch {};
            return resolved;
        }
        free_list.appendPage(0, candidate) catch {};
    }
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
            .denied => return null,
            .ready => return plan.mapping,
            .allocate_zero => {},
            .allocate_copy, .locked_slow_path => return null,
        }
        const candidate = allocatePreparedFaultPage(state, free_list, plan) orelse return null;
        const mapping = blk: {
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            break :blk state.commitNativeVmaFaultMapping(principal, plan, candidate);
        };
        if (mapping) |resolved| {
            if (resolved.paddr != candidate) free_list.appendPage(0, candidate) catch {};
            return resolved;
        }
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
    if (user_vm.lookupUserMappedPaddrForVa(principal, page_va)) |paddr| return paddr;
    const mapping = resolveNativeVmaFaultMappingWithAddressSpaceLocked(
        h.state,
        h.free_list,
        principal,
        page_va,
        write_access,
        false,
    ) orelse return null;
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
    const target_cr3 = @atomicLoad(u64, &tlb_shootdown_target_cr3, .monotonic);
    if (target_cr3 != 0) flushCr3ContextOnCurrentCpu(target_cr3);
    @atomicStore(u64, &tlb_shootdown_ack[cpu_slot], generation, .release);
}

fn shootdownCr3Context(target_cr3: u64) void {
    if (target_cr3 == 0) return;
    const restore_interrupts = TlbShootdownLock.interruptsEnabled();
    defer if (restore_interrupts) asm volatile ("sti" ::: .{ .memory = true });
    asm volatile ("cli" ::: .{ .memory = true });
    tlb_shootdown_lock.lock();
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

    var cpu_slot: usize = 0;
    while (cpu_slot < smp.max_cpus and cpu_slot < 64) : (cpu_slot += 1) {
        const targeted = (target_cpu_mask & (@as(u64, 1) << @intCast(cpu_slot))) != 0;
        if (!targeted or cpu_slot == current_cpu) continue;
        while (!smp.interruptCpu(cpu_slot)) {
            asm volatile ("sti; pause; cli" ::: .{ .memory = true });
        }
    }

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
