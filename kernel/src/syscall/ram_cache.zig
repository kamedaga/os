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
    for (state.native_vmos, 0..) |slot, index| {
        if (slot.ref_count != 0 or slot.ram_cache_state != .retiring) continue;
        const ref = kernel.NativeVmoRef{ .index = @intCast(index), .generation = slot.generation };
        engine.retire(ref) catch |err| {
            if (failures.count == 0) {
                failures.first = ref;
                failures.reason = err;
            }
            failures.count += 1;
            failures.signature = (failures.signature *% 1099511628211) ^ @as(u64, @intCast(index));
            failures.signature = (failures.signature *% 1099511628211) ^ slot.generation;
            failures.signature = (failures.signature *% 1099511628211) ^ @intFromError(err);
        };
    }
    return failures;
}

test "MMIO overlay RAM drain retains failures but continues later generations" {
    const Slot = struct { ref_count: u32 = 0, ram_cache_state: enum { wb, retiring } = .retiring, generation: u32 = 1 };
    const State = struct { native_vmos: [3]Slot = .{ .{}, .{}, .{ .ref_count = 1 } } };
    const Fake = struct {
        calls: usize = 0,
        fn retire(self: *@This(), ref: kernel.NativeVmoRef) !void {
            self.calls += 1;
            if (ref.index == 0) return error.InjectedRetained;
        }
    };
    var state = State{};
    var engine = Fake{};
    const first = drainLocked(&state, &engine);
    try std.testing.expectEqual(@as(usize, 2), engine.calls);
    try std.testing.expectEqual(@as(usize, 1), first.count);
    try std.testing.expect(std.meta.eql(first, drainLocked(&state, &engine)));
    state.native_vmos[0].generation += 1;
    try std.testing.expect(!std.meta.eql(first, drainLocked(&state, &engine)));
    state.native_vmos[0].ram_cache_state = .wb;
    try std.testing.expectEqual(@as(usize, 0), drainLocked(&state, &engine).count);
}
