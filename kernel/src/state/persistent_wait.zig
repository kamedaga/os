//! Explicit native wait-set lifetime. K protects the directory, descriptor
//! identity, ready FIFO, and invocation; source registration additionally takes
//! E in the established K -> E order. Observers own neither FD nor peer refs.
const std = @import("std");
const types = @import("types.zig");
const storage = @import("../memory/slot_table.zig");
const records = @import("persistent_wait_types.zig");
pub const Set = records.Set;
pub const Item = records.Item;

fn notify(raw: *anyopaque, index: u32) void {
    const state: *@import("../kernel.zig").KernelState = @ptrCast(@alignCast(raw));
    state.persistent_wait_sets.mark(index, 1, true);
}

pub fn current(state: anytype, owner: types.PrincipalId, thread: usize, generation: u32) ?*Set {
    if (thread > std.math.maxInt(u32)) return null;
    const ref = state.persistent_wait_threads.get(@intCast(thread)) orelse return null;
    const set = state.persistent_wait_sets.resolve(ref.set) orelse return null;
    if (set.owner != owner or set.thread_generation != generation) return null;
    return set;
}

pub fn draft(state: anytype, owner: types.PrincipalId, thread: usize, generation: u32, count: usize) types.KernelError!*Set {
    if (thread > std.math.maxInt(u32)) return types.KernelError.InvalidState;
    const allocator = try state.waitPoolAllocator();
    state.persistent_wait_threads.prepareIndex(@intCast(thread), allocator) catch return types.KernelError.OutOfFreePages;
    const allocation = try state.allocateWaitPool(&state.persistent_wait_sets);
    const set = allocation.value;
    set.* = .{ .owner = owner, .thread = @intCast(thread), .thread_generation = generation,
        .index = allocation.ref.index, .generation = allocation.ref.generation,
        .count = @intCast(count), .context = state, .notify = notify };
    errdefer destroy(state, set);
    for (0..count) |i| {
        set.items.prepareIndex(@intCast(i), allocator) catch return types.KernelError.OutOfFreePages;
        _ = set.items.claimIndex(@intCast(i)) catch return types.KernelError.OutOfFreePages;
    }
    return set;
}

pub fn destroy(state: anytype, set: *Set) void {
    var items = set.items.iterator();
    while (items.next()) |entry| {
        const item = entry.value;
        for (item.registrations[0..item.registration_count]) |registration|
            state.retirePersistentRegistration(registration);
        endObservation(state, item);
    }
    set.items.deinitInto(&state.persistent_wait_sets.body_owner);
    state.persistent_wait_sets.remove(set, null);
}

fn endObservation(state: anytype, item: *Item) void {
    if (!item.observing) return;
    const source = state.kernelObjectSlot(item.object).?;
    std.debug.assert(source.persistent_observers != 0);
    source.persistent_observers -= 1;
    item.observing = false;
}

pub fn commit(state: anytype, set: *Set) types.KernelError!void {
    // claimIndex was prepared before any old subscription is withdrawn. All
    // remaining publication/destruction is infallible under K.
    const map = state.persistent_wait_threads.claimIndex(set.thread) catch return types.KernelError.OutOfFreePages;
    if (map.set.generation != 0) {
        if (state.persistent_wait_sets.resolve(map.set)) |old| destroy(state, old);
    }
    map.set = .{ .index = set.index, .generation = set.generation };
}

pub fn clearThread(state: anytype, owner: types.PrincipalId, thread: usize, generation: u32) void {
    const set = current(state, owner, thread, generation) orelse return;
    destroy(state, set);
    state.persistent_wait_threads.remove(state.persistent_wait_threads.get(@intCast(thread)).?, null);
}

pub fn clearOwner(state: anytype, owner: types.PrincipalId) void {
    var sets = state.persistent_wait_sets.iterator();
    while (sets.next()) |entry| {
        const set = entry.value;
        if (set.owner == owner) clearThread(state, owner, set.thread, set.thread_generation);
    }
}

pub fn clearAll(state: anytype) void {
    var sets = state.persistent_wait_sets.iterator();
    while (sets.next()) |entry| {
        const set = entry.value;
        clearThread(state, set.owner, set.thread, set.thread_generation);
    }
}

pub fn invalidate(state: anytype, set: *Set) void {
    if (set.invalidated) return;
    // Detach before the source descriptor/object can be reclaimed. No strong
    // endpoint reference is used: observing a pipe must never postpone EOF.
    var items = set.items.iterator();
    while (items.next()) |entry| {
        const item = entry.value;
        for (item.registrations[0..item.registration_count]) |registration|
            state.retirePersistentRegistration(registration);
        item.registration_count = 0;
        endObservation(state, item);
    }
    while (set.pop() != null) {}
    set.invalidated = true;
    set.kick();
}

pub fn invalidateFd(state: anytype, owner: types.PrincipalId, fd: types.Fd) void {
    const source = state.fdEntryConst(owner, fd) orelse return;
    const object = state.kernelObjectSlotConst(source.object) orelse return;
    if (object.persistent_observers == 0) return;
    var sets = state.persistent_wait_sets.iterator();
    while (sets.next()) |entry| {
        const set = entry.value;
        if (set.owner != owner or set.invalidated) continue;
        var items = set.items.iterator();
        while (items.next()) |member| {
            if (member.value.fd == fd) { invalidate(state, set); break; }
        }
    }
}

pub fn invalidateObject(state: anytype, object: types.KernelObjectRef) void {
    const source = state.kernelObjectSlotConst(object) orelse return;
    if (source.persistent_observers == 0) return;
    var sets = state.persistent_wait_sets.iterator();
    while (sets.next()) |entry| {
        const set = entry.value;
        if (set.invalidated) continue;
        var items = set.items.iterator();
        while (items.next()) |member| {
            if (std.meta.eql(member.value.object, object)) { invalidate(state, set); break; }
        }
    }
}

pub fn validate(state: anytype, set: *Set) bool {
    if (set.invalidated) return false;
    const table = state.fdTableForActiveProcessConst(set.owner) catch { invalidate(state, set); return false; };
    if (table.mutation == set.table_mutation and table.mutation != std.math.maxInt(u64)) return true;
    var items = set.items.iterator();
    while (items.next()) |entry| {
        const item = entry.value;
        const fd = state.fdEntryConst(set.owner, item.fd) orelse { invalidate(state, set); return false; };
        if (fd.seq != item.descriptor_seq or !std.meta.eql(fd.object, item.object)) {
            invalidate(state, set);
            return false;
        }
    }
    set.table_mutation = table.mutation;
    return true;
}
