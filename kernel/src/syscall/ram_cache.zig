const std = @import("std");
const kernel = @import("../kernel.zig");
const vm = @import("../memory/user_vm.zig");
const platform = @import("../arch/x86_64/platform.zig");
const storage = @import("../memory/ram_cache_storage.zig");

var store: storage.Store = .{};

const Failure = struct {
    count: usize = 0,
    signature: u64 = 0,
    first: kernel.NativeVmoRef = .{},
    reason: ?anyerror = null,
};
var previous_failure: Failure = .{};

pub fn kernelStaticStorageEndAddr() usize {
    return @max(@intFromPtr(&store) + @sizeOf(@TypeOf(store)), @intFromPtr(&previous_failure) + @sizeOf(@TypeOf(previous_failure)));
}

/// Internal entry, not a dispatched syscall. FD authority and state lock
/// belong to the outer caller; no untrusted PFN/owner token is accepted.
pub fn change(state: *kernel.KernelState, free_list: *kernel.FreePageList, ref: kernel.NativeVmoRef, first: usize, count: usize, mode: @import("../memory/ram_cache_transition.zig").Mode) !void {
    vm.lockAllVmTransactions();
    defer vm.unlockAllVmTransactions();
    try store.change(state, ref, first, count, mode, free_list, platform.kernel_cr3_value);
}

/// Called with KernelState held and no inner VM lock, after device DMA
/// teardown. Hardware quiescence must precede retirement; VMA pin absence
/// alone is not a substitute for that lifecycle ordering.
pub fn drain(state: *kernel.KernelState, free_list: *kernel.FreePageList) void {
    if (state.pending_ram_cache_retirements == 0) {
        previous_failure = .{};
        return;
    }
    vm.lockAllVmTransactions();
    defer vm.unlockAllVmTransactions();
    var engine = Engine{ .state = state, .free_list = free_list };
    const result = drainLocked(state, &engine);
    if (!std.meta.eql(result, previous_failure) and result.count != 0) {
        @import("../kernel_log.zig").writeFmt("ram-cache: retirement retained={} first_vmo={}:{} reason={s}\n", .{ result.count, result.first.index, result.first.generation, @errorName(result.reason.?) });
    }
    previous_failure = result;
}

const Engine = struct {
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    fn retire(self: *@This(), ref: kernel.NativeVmoRef) !void {
        try store.retire(self.state, ref, self.free_list, platform.kernel_cr3_value);
    }
};

fn drainLocked(state: anytype, engine: anytype) Failure {
    var failures = Failure{};
    // Scan once: a permanent failure must not starve later retired VMOs or
    // turn one syscall's exit path into an infinite retry loop.
    // KernelState excludes concurrent slot changes. Borrow the array rather
    // than copying it onto an AP/IST stack; retire may clear the current slot,
    // so diagnostics below use only the identity captured before that call.
    {
        var pool_iter_38 = state.native_vmos.iterator();
        while (pool_iter_38.next()) |pool_entry_38| {
            const slot = pool_entry_38.value;
            const index: usize = pool_entry_38.ref.index;
            if (slot.ref_count != 0 or slot.ram_cache_state != .retiring) continue;
            const ref = kernel.NativeVmoRef{ .index = @intCast(index), .generation = slot.generation };
            engine.retire(ref) catch |err| {
                if (failures.count == 0) {
                    failures.first = ref;
                    failures.reason = err;
                }
                failures.count += 1;
                failures.signature = (failures.signature *% 1099511628211) ^ @as(u64, @intCast(index));
                failures.signature = (failures.signature *% 1099511628211) ^ ref.generation;
                failures.signature = (failures.signature *% 1099511628211) ^ @intFromError(err);
            };
        }
    }
    return failures;
}

test "MMIO overlay RAM drain retains failures but continues later generations" {
    const Slot = struct { ref_count: u32 = 0, ram_cache_state: enum { wb, retiring } = .retiring, generation: u32 = 1 };
    const State = struct { native_vmos: @import("../memory/slot_table.zig").SlotTable(Slot) = .{} };
    const Fake = struct {
        state: *State,
        calls: [4]usize = .{ 0, 0, 0, 0 },
        seen: [4]kernel.NativeVmoRef = .{ .{}, .{}, .{}, .{} },
        fail_restore: bool = true,
        fn retire(self: *@This(), ref: kernel.NativeVmoRef) !void {
            self.calls[ref.index] += 1;
            self.seen[ref.index] = ref;
            if (ref.index == 0 and self.fail_restore) return error.InjectedWbRestore;
            // Model successful retirement consuming only this slot. A later
            // pass must not retry it while an earlier failure remains.
            self.state.native_vmos.get(@intCast(ref.index)).?.*.ram_cache_state = .wb;
        }
    };
    var state = State{};
    defer state.native_vmos.deinit();
    for ([_]Slot{ .{ .generation = 7 }, .{ .generation = 13 }, .{ .ref_count = 1 }, .{ .ram_cache_state = .wb } }) |value| {
        const item = try state.native_vmos.allocate(@import("../memory/slot_table.zig").hostTestAllocator(), null, 0xffffffff);
        item.value.* = value;
    }
    var engine = Fake{ .state = &state };
    const first = drainLocked(&state, &engine);
    try std.testing.expectEqualSlices(usize, &.{ 1, 1, 0, 0 }, &engine.calls);
    try std.testing.expectEqual(@as(usize, 1), first.count);
    try std.testing.expectEqual(kernel.NativeVmoRef{ .index = 0, .generation = 7 }, first.first);
    try std.testing.expectEqual(kernel.NativeVmoRef{ .index = 1, .generation = 13 }, engine.seen[1]);
    try std.testing.expectEqual(error.InjectedWbRestore, first.reason.?);
    try std.testing.expect(state.native_vmos.get(@intCast(0)).?.*.ram_cache_state == .retiring);
    try std.testing.expect(std.meta.eql(first, drainLocked(&state, &engine)));
    try std.testing.expectEqualSlices(usize, &.{ 2, 1, 0, 0 }, &engine.calls);
    state.native_vmos.get(@intCast(0)).?.*.generation += 1;
    try std.testing.expect(!std.meta.eql(first, drainLocked(&state, &engine)));
    engine.fail_restore = false;
    try std.testing.expectEqual(@as(usize, 0), drainLocked(&state, &engine).count);
    try std.testing.expectEqualSlices(usize, &.{ 4, 1, 0, 0 }, &engine.calls);
    try std.testing.expectEqual(@as(usize, 0), drainLocked(&state, &engine).count);
    try std.testing.expectEqualSlices(usize, &.{ 4, 1, 0, 0 }, &engine.calls);
}

test "MMIO overlay RAM drain failure identity is captured before slot mutation" {
    const Slot = struct { ref_count: u32 = 0, ram_cache_state: enum { retiring } = .retiring, generation: u32 = 7 };
    const State = struct { native_vmos: @import("../memory/slot_table.zig").SlotTable(Slot) = .{} };
    const Fake = struct {
        state: *State,
        mutate: bool = false,
        fn retire(self: *@This(), _: kernel.NativeVmoRef) !void {
            // Adversarial engine: make a post-call slot read observably
            // different from the identity which was passed for retirement.
            if (self.mutate) self.state.native_vmos.get(@intCast(0)).?.*.generation = 99;
            return error.InjectedRetained;
        }
    };
    var state = State{};
    defer state.native_vmos.deinit();
    const item = try state.native_vmos.allocate(@import("../memory/slot_table.zig").hostTestAllocator(), null, 0xffffffff);
    item.value.generation = 7;
    var engine = Fake{ .state = &state };
    const original = drainLocked(&state, &engine);
    engine.mutate = true;
    const mutated = drainLocked(&state, &engine);
    try std.testing.expectEqual(@as(u32, 99), state.native_vmos.get(@intCast(0)).?.*.generation);
    try std.testing.expectEqualDeep(original, mutated);
    try std.testing.expectEqual(@as(u32, 7), mutated.first.generation);
}

test "MMIO overlay RAM drain with no pending retirement clears previous failure" {
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    const free_list = try std.testing.allocator.create(kernel.FreePageList);
    defer std.testing.allocator.destroy(free_list);
    // Leave every other field undefined: the empty path must not visit the
    // VMO array, PMM, or native VM locks, including in this host fixture.
    state.pending_ram_cache_retirements = 0;
    const saved_failure = previous_failure;
    defer previous_failure = saved_failure;
    previous_failure = .{ .count = 1, .signature = 42, .reason = error.InjectedRetained };
    drain(state, free_list);
    try std.testing.expectEqualDeep(Failure{}, previous_failure);
}
