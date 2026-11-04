//! Read-only current-thread snapshots. The AS owns a resident VMO reference;
//! local CPU pins retain its stable owner. No allocation/lock on user return.
const std = @import("std");
const types = @import("state/types.zig");
const fd_abi = @import("kernel_abi_root").fd_abi;
const physical = @import("arch/x86_64/physical_layout.zig");
const addresses = @import("memory/addresses.zig");

pub const Header = extern struct {
    magic: u64 = fd_abi.thread_page_magic,
    total_bytes: u64,
    capacity: u64,
    stride: u64 = fd_abi.thread_page_slot_stride,
    enabled: u64 = 0,
    owner_cookie: u64,
    reserved: [2]u64 = .{ 0, 0 },
};
pub const Slot = extern struct {
    seq: u64 = 0,
    current: u64 = 0,
    reserved: [6]u64 = @splat(0),
};
comptime {
    std.debug.assert(@sizeOf(Header) == 64 and @sizeOf(Slot) == 64);
    std.debug.assert(@offsetOf(Header, "enabled") == 32 and @offsetOf(Header, "owner_cookie") == 40);
    std.debug.assert(@offsetOf(Slot, "current") == 8);
}
// K serializes creation; exhaustion refuses new pages rather than reusing an
// incarnation cookie while a revoked old VMO may still be retained by a FD.
var next_cookie: u64 = 1;
// The cookie issuer survives every AS. Reserve its image storage explicitly,
// rather than relying on its current placement between unrelated globals.
pub fn staticStart() usize { return @intFromPtr(&next_cookie); }
pub fn staticEnd() usize { return @intFromPtr(&next_cookie) + @sizeOf(@TypeOf(next_cookie)); }
fn takeCookie() ?u64 {
    if (next_cookie == std.math.maxInt(u64)) return null;
    const cookie = next_cookie;
    next_cookie += 1;
    return cookie;
}
pub fn pageBytes(capacity: u64) ?u64 {
    if (capacity == 0 or capacity > std.math.maxInt(u32)) return null;
    const bytes = std.math.add(u64, 64, std.math.mul(u64, capacity, 64) catch return null) catch return null;
    return (std.math.add(u64, bytes, 4095) catch return null) & ~@as(u64, 4095);
}
fn header(space: anytype) ?*Header {
    const pointer = @atomicLoad(usize, &space.thread_identity_header, .acquire);
    return if (pointer == 0) null else @ptrFromInt(pointer);
}
pub fn publishSlot(slot: *Slot, tid: u64) void {
    const old = @atomicLoad(u64, &slot.seq, .monotonic);
    if (old == std.math.maxInt(u64)) return;
    if (old == std.math.maxInt(u64) - 1) {
        @atomicStore(u64, &slot.seq, std.math.maxInt(u64), .release);
        return;
    }
    // One IRQ-masked writer per CPU line. TSO orders the actual stores;
    // memory clobbers additionally forbid the compiler moving current before
    // the odd mark, or the final even mark before current. No locked RMW.
    @atomicStore(u64, &slot.seq, old + 1, .monotonic);
    asm volatile ("" ::: .{ .memory = true });
    @atomicStore(u64, &slot.current, tid, .monotonic);
    asm volatile ("" ::: .{ .memory = true });
    @atomicStore(u64, &slot.seq, old + 2, .release);
}
pub fn publish(space: anytype, cpu: usize, tid: u64) void {
    const h = header(space) orelse return;
    if (@atomicLoad(u64, &h.enabled, .acquire) != 1) return;
    if (cpu >= h.capacity or tid == 0) { close(space); return; }
    // This directory is immutable after the release publication above; K/AS
    // teardown withdraws it only after this process cannot return on a CPU.
    const value = space.thread_identity_slots.get(@intCast(cpu)) orelse { close(space); return; };
    publishSlot(@ptrFromInt(value.*), tid);
}
pub fn close(space: anytype) void {
    if (header(space)) |h| @atomicStore(u64, &h.enabled, 0, .release);
    @atomicStore(usize, &space.thread_identity_header, 0, .release);
}
pub fn retire(state: anytype, space: anytype, free_list: *types.FreePageList) void {
    close(space);
    if (!space.thread_identity_vmo.isNull()) {
        const ref = space.thread_identity_vmo;
        const vmo = state.nativeVmoSlot(ref) orelse unreachable;
        std.debug.assert(vmo.kernel_edges != 0);
        @import("sync/lock_diagnostics.zig").assertKernelStateHeld();
        vmo.kernel_edges -= 1;
        types.references.Root.disconnect(&space.thread_identity_reference, true);
        space.thread_identity_vmo = .{};
        state.releaseNativeVmoWithFreeList(ref, free_list);
    }
    space.thread_identity_slots.deinit();
    space.thread_identity_cookie = 0;
}
pub fn mappingAllowed(vmo: *const types.NativeVmoSlot, owner: types.PrincipalId, prot: types.VmaProt, max_prot: types.VmaProt, flags: types.MmapFlags) bool {
    const space = @import("memory/user_vm.zig").getUserSpace(owner) orelse return false;
    return vmo.thread_page_cookie != 0 and vmo.thread_page_cookie == space.thread_identity_cookie and
        @import("state/vma.zig").kernelPageMappingAllowed(prot, max_prot, flags);
}
fn installFd(state: anytype, owner: types.PrincipalId, ref: types.NativeVmoRef, free_list: *types.FreePageList) types.KernelError!types.Fd {
    try state.retainNativeVmo(ref);
    const object = state.createKernelObject(.vmo, .{ .vmo = ref }) catch |err| {
        state.releaseNativeVmoWithFreeList(ref, free_list); return err;
    };
    return state.installFd(owner, object, .{ .inspect = true, .close = true, .map_read = true },
        .{ .private = true, .cloexec = true }, fd_abi.first_dynamic_fd) catch |err| {
        if (state.kernelObjectSlot(object)) |slot| state.clearKernelObjectSlot(slot);
        return err;
    };
}
pub fn open(state: anytype, owner: types.PrincipalId, free_list: *types.FreePageList) types.KernelError!types.Fd {
    const space = @import("memory/user_vm.zig").getUserSpace(owner) orelse return types.KernelError.InvalidState;
    if (!space.thread_identity_vmo.isNull()) return installFd(state, owner, space.thread_identity_vmo, free_list);
    if (!@import("arch/x86_64/platform.zig").threadIdentityCpuQualification()) return types.KernelError.InvalidState;
    return createForSpace(state, owner, free_list, @import("arch/x86_64/cpu_runtime.zig").capacity());
}
pub const Test = if (@import("builtin").is_test) struct {
    pub const openQualified = createForSpace;
} else struct {};
fn createForSpace(state: anytype, owner: types.PrincipalId, free_list: *types.FreePageList, capacity: usize) types.KernelError!types.Fd {
    const space = @import("memory/user_vm.zig").getUserSpace(owner) orelse return types.KernelError.InvalidState;
    const bytes = pageBytes(capacity) orelse return types.KernelError.InvalidState;
    const cookie = takeCookie() orelse return types.KernelError.InvalidState;
    // Ordinary owned RAM supplies the same failure-safe PMM return path. The
    // object is marked special before K/VM exclusion can publish its FD.
    const fd = try state.createAnonymousVmoFdWithPages(owner, bytes,
        .{ .inspect = true, .close = true, .map_read = true },
        .{ .private = true, .cloexec = true }, fd_abi.first_dynamic_fd, free_list);
    errdefer state.closeFdWithFreeList(owner, fd, free_list) catch {};
    const ref = state.nativeVmoRefForFd(owner, fd).?;
    const vmo = state.nativeVmoSlot(ref).?;
    const allocator = @import("memory/identity_allocator.zig").allocator(free_list);
    errdefer space.thread_identity_slots.deinit();
    for (0..capacity) |cpu| {
        space.thread_identity_slots.prepareIndex(@intCast(cpu), allocator) catch return types.KernelError.OutOfFreePages;
        const offset = 64 + cpu * 64;
        const pa = state.nativeVmoPagePaddr(ref, offset / 4096) orelse return types.KernelError.InvalidState;
        const mem = physical.identitySlice(Slot, addresses.PhysicalAddress.fromInt(pa + offset % 4096), 1) orelse return types.KernelError.InvalidState;
        _ = space.thread_identity_slots.claimIndex(@intCast(cpu)) catch return types.KernelError.OutOfFreePages;
        space.thread_identity_slots.get(@intCast(cpu)).?.* = @intFromPtr(mem.ptr);
    }
    const pa = state.nativeVmoPagePaddr(ref, 0) orelse return types.KernelError.InvalidState;
    const h = &(physical.identitySlice(Header, .fromInt(pa), 1) orelse return types.KernelError.InvalidState)[0];
    h.* = .{ .total_bytes = bytes, .capacity = capacity, .owner_cookie = cookie };
    vmo.kind = .thread_page;
    vmo.thread_page_cookie = cookie;
    try state.retainNativeVmo(ref);
    @import("sync/lock_diagnostics.zig").assertKernelStateHeld();
    vmo.kernel_edges += 1;
    vmo.references.connect(&space.thread_identity_reference, .{ .kind = .kernel, .owner = @intCast(space.directory_index), .sequence = cookie }, .{}, true);
    space.thread_identity_vmo = ref;
    space.thread_identity_cookie = cookie;
    @atomicStore(u64, &h.enabled, 1, .release);
    @atomicStore(usize, &space.thread_identity_header, @intFromPtr(h), .release);
    return fd;
}
test "thread identity sequence closes forever at overflow" {
    var s: Slot = .{};
    publishSlot(&s, 9);
    try std.testing.expectEqual(@as(u64, 2), s.seq);
    try std.testing.expectEqual(@as(u64, 9), s.current);
    s.seq = std.math.maxInt(u64) - 1;
    publishSlot(&s, 10);
    try std.testing.expectEqual(std.math.maxInt(u64), s.seq);
    publishSlot(&s, 11);
    try std.testing.expectEqual(std.math.maxInt(u64), s.seq);
    try std.testing.expectEqual(@as(u64, 9), s.current);
}
test "thread identity size covers multiple pages without fixed CPU limit" {
    try std.testing.expectEqual(@as(?u64, 4096), pageBytes(63));
    try std.testing.expectEqual(@as(?u64, 8192), pageBytes(64));
    try std.testing.expectEqual(@as(?u64, null), pageBytes(0));
    try std.testing.expectEqual(@as(?u64, null), pageBytes(std.math.maxInt(u64)));
}

test "thread identity incarnation exhaustion never wraps" {
    const saved = next_cookie; defer next_cookie = saved;
    next_cookie = std.math.maxInt(u64) - 1;
    try std.testing.expectEqual(@as(?u64, std.math.maxInt(u64) - 1), takeCookie());
    try std.testing.expectEqual(@as(?u64, null), takeCookie());
    try std.testing.expectEqual(@as(?u64, null), takeCookie());
}
