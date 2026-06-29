const std = @import("std");
const kernel = @import("kernel.zig");
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

const PhysCopyWindowLock = struct {
    value: u8 = 0,
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

    fn lock(self: *PhysCopyWindowLock) void {
        const restore_interrupts = interruptsEnabled();
        asm volatile ("cli" ::: .{ .memory = true });
        while (true) {
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) {
                self.interrupts_were_enabled = restore_interrupts;
                return;
            }
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                asm volatile ("pause");
            }
        }
    }

    fn unlock(self: *PhysCopyWindowLock) void {
        const restore_interrupts = self.interrupts_were_enabled;
        @atomicStore(u8, &self.value, 0, .release);
        if (restore_interrupts) asm volatile ("sti" ::: .{ .memory = true });
    }
};

var phys_copy_window_lock: PhysCopyWindowLock = .{};

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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(phys_copy_window_lock), &phys_copy_window_lock));
    return end;
}

pub fn mapKernelRuntimeStorage(map_identity_range: *const fn (u64, usize) bool) bool {
    if (!map_identity_range(@intFromPtr(&user_copy_hooks_storage), @sizeOf(@TypeOf(user_copy_hooks_storage)))) return false;
    if (!map_identity_range(@intFromPtr(&user_copy_hooks_ready), @sizeOf(@TypeOf(user_copy_hooks_ready)))) return false;
    if (!map_identity_range(@intFromPtr(&phys_copy_window_lock), @sizeOf(@TypeOf(phys_copy_window_lock)))) return false;
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

fn mapPhysPageForKernelAccess(page_paddr: u64) ?[*]u8 {
    const h = getHooks();
    const page_base = page_paddr & ~@as(u64, 0xFFF);
    if (page_base >= h.physical_map_limit) return null;
    h.phys_copy_window_pt[0] = page_base | h.page_present | h.page_rw;
    h.invlpg(h.phys_copy_window_va);
    return @ptrFromInt(h.phys_copy_window_va);
}

fn physWindowAddr(addr: u64, access_len: usize) ?u64 {
    const h = getHooks();
    if (access_len == 0 or access_len > 4096) return null;
    const offset: usize = @intCast(addr & 0xFFF);
    if (offset + access_len > 4096) return null;
    const last_addr, const overflow = @addWithOverflow(addr, @as(u64, @intCast(access_len - 1)));
    if (overflow != 0 or last_addr >= h.physical_map_limit) return null;
    const page = mapPhysPageForKernelAccess(addr) orelse return null;
    return @intFromPtr(page) + offset;
}

fn ensureUserPageMappedForCopy(principal: kernel.PrincipalId, page_va: u64, write_access: bool) ?u64 {
    if (user_vm.lookupUserMappedPaddrForVa(principal, page_va)) |paddr| return paddr;
    const h = getHooks();
    user_vm.lockAddressSpaces();
    defer user_vm.unlockAddressSpaces();
    if (user_vm.lookupUserMappedPaddrForVa(principal, page_va)) |paddr| return paddr;
    const mapping = h.state.ensureNativeVmaFaultMapping(
        principal,
        page_va,
        write_access,
        false,
        h.free_list,
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
    phys_copy_window_lock.lock();
    defer phys_copy_window_lock.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u8)) orelse return null;
    const ptr: *volatile u8 = @ptrFromInt(window_addr);
    return ptr.*;
}

pub fn readPhysU32(addr: u64) ?u32 {
    phys_copy_window_lock.lock();
    defer phys_copy_window_lock.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u32)) orelse return null;
    const ptr: *volatile u32 = @ptrFromInt(window_addr);
    return ptr.*;
}

pub fn readPhysU64(addr: u64) ?u64 {
    phys_copy_window_lock.lock();
    defer phys_copy_window_lock.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u64)) orelse return null;
    const ptr: *volatile u64 = @ptrFromInt(window_addr);
    return ptr.*;
}

pub fn writePhysU8(addr: u64, value: u8) bool {
    phys_copy_window_lock.lock();
    defer phys_copy_window_lock.unlock();
    const window_addr = physWindowAddr(addr, @sizeOf(u8)) orelse return false;
    const ptr: *volatile u8 = @ptrFromInt(window_addr);
    ptr.* = value;
    return true;
}

pub fn zeroPhysicalPage(page_paddr: u64) bool {
    if ((page_paddr & 0xFFF) != 0) return false;
    phys_copy_window_lock.lock();
    defer phys_copy_window_lock.unlock();
    const page = mapPhysPageForKernelAccess(page_paddr) orelse return false;
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
            phys_copy_window_lock.lock();
            defer phys_copy_window_lock.unlock();
            const src_page = mapPhysPageForKernelAccess(src_paddr) orelse return false;
            const src: [*]const u8 = @ptrCast(src_page + page_off);
            var i: usize = 0;
            while (i < chunk_len) : (i += 1) {
                dest[copied + i] = src[i];
            }
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
            phys_copy_window_lock.lock();
            defer phys_copy_window_lock.unlock();
            const dst_page = mapPhysPageForKernelAccess(dst_paddr) orelse return false;
            const dst: [*]u8 = @ptrCast(dst_page + page_off);
            var i: usize = 0;
            while (i < chunk_len) : (i += 1) {
                dst[i] = src[copied + i];
            }
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

pub fn flushTlbForCr3Va(target_cr3: u64, va: u64) void {
    const h = getHooks();
    if (target_cr3 == 0) return;
    const current_cr3 = h.read_cr3();
    if (current_cr3 == target_cr3) {
        h.invlpg(va);
        return;
    }

    h.write_cr3(target_cr3);
    h.invlpg(va);
    h.write_cr3(current_cr3);
}

pub fn flushTlbForCr3Range(target_cr3: u64, va: u64, size_bytes: usize) void {
    const h = getHooks();
    if (target_cr3 == 0 or size_bytes == 0) return;
    const page_count = (size_bytes + 4095) / 4096;
    if (page_count <= 4) {
        var offset: usize = 0;
        while (offset < size_bytes) : (offset += 4096) {
            flushTlbForCr3Va(target_cr3, va + @as(u64, @intCast(offset)));
        }
        return;
    }

    const current_cr3 = h.read_cr3();
    if (current_cr3 == target_cr3) {
        h.write_cr3(current_cr3);
        return;
    }

    h.write_cr3(target_cr3);
    h.write_cr3(current_cr3);
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
