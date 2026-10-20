// Actual kernel callers use the diagnostic build with a deterministic host CPU.
test "lock ordering actual caller roots" {
    try @import("std").testing.expect(@import("sync/lock_diagnostics.zig").enabled);
    _ = @import("syscalls.zig");
    _ = @import("mmio_overlay_test.zig");
    _ = @import("syscall/runtime.zig");
    _ = @import("vtd.zig");
    _ = @import("amd_iommu.zig");
    _ = @import("pci.zig");
    _ = @import("realtime_clock.zig");
    _ = @import("kernel_log.zig");
    _ = @import("boot/boot_diag.zig");
}

test "lock ordering actual B PMM AD PD VD public callers" {
    const std = @import("std");
    const d = @import("sync/lock_diagnostics.zig");
    const kernel = @import("kernel.zig");
    const types = @import("state/types.zig");
    const address_space = @import("memory/address_space.zig");
    var backing: types.backing_index.Store = .{};
    backing.lock.lock();
    try std.testing.expectEqual(d.order.Class.vmo_backing, d.testCpu().head.?.key.class);
    // B -> P on a real backing allocator, then B is still the sole hold.
    var pages: types.FreePageList = .{};
    _ = pages.pageCount();
    try std.testing.expectEqual(d.order.Class.pmm, pages.diagnostic_record.key.class);
    try std.testing.expect(!pages.diagnostic_record.linked);
    try std.testing.expectEqual(d.order.Class.vmo_backing, d.testCpu().head.?.key.class);
    backing.lock.unlock();
    var table: address_space.UserAddressSpaceTable = .{};
    table.initializeFreshStorage(&.{});
    try std.testing.expect(table.get(100) == null);
    try std.testing.expectEqual(d.order.Class.as_directory, table.directory_record.key.class);
    try std.testing.expect(!table.directory_record.linked);
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    state.* = .{};
    _ = state.tryProcessLabel(kernel.processPrincipalFromIndex(0).?);
    try std.testing.expectEqual(d.order.Mode.try_once, state.descriptor_lock.record.mode);
    _ = state.vmaTableForProcessIndex(kernel.process_count + 1);
    try std.testing.expectEqual(d.order.Class.vma_directory, state.vma_directory_lock.record.key.class);
    d.assertRootEmpty();
}

test "lock ordering IRQ drain and principal quiescence actual wait entrances" {
    const scheduler = @import("scheduler_connection.zig");
    const smp = @import("smp.zig");
    const kernel = @import("kernel.zig");
    scheduler.initializeStaticStorage();
    _ = smp.drainInterruptRange(0, 0);
    scheduler.waitForPrincipalQuiescence(kernel.processPrincipalFromIndex(0).?);
    @import("sync/lock_diagnostics.zig").assertRootEmpty();
}

test "lock admission Q actual device IRQ slot hold ends before completion" {
    const std = @import("std");
    const d = @import("sync/lock_diagnostics.zig");
    const kernel = @import("kernel.zig");
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    state.* = .{};
    defer state.deinitPools();
    const allocation = try state.allocatePool(&state.irq_publish_slots, &state.object_directory_leaf, 0xffffffff);
    const slot = allocation.value;
    slot.active = 1;
    slot.device = 9;
    var owners: [2]kernel.PrincipalId = undefined;
    _ = state.recordDeviceInterruptEvent(9, 0, &owners);
    try std.testing.expectEqual(d.order.Class.slot_directory, state.object_directory_leaf.byte.record.key.class);
    try std.testing.expectEqual(d.order.Mode.blocking, state.object_directory_leaf.byte.record.mode);
    try std.testing.expectEqual(@as(u8, 1), slot.active);
    try std.testing.expect(!state.object_directory_leaf.byte.record.linked);
    d.assertRootEmpty();
}

test "lock ordering PMM caches bounded single return and all CPU drain" {
    const std = @import("std");
    const module = @import("memory/free_page_list.zig");
    const List = module.WithQuarantine(struct {
        fn allowed(_: u64, _: usize) bool {
            return false;
        }
    }.allowed);
    var backing = try module.TestBacking.init(4096, 128);
    defer backing.deinit();
    var list = List{};
    try list.installRegions(&.{.{ .id = 1, .start = 4096, .end = 129 * 4096 }});
    try list.seedContiguousRange(4096, 128);
    try list.initCaches(2);
    defer list.deinitCaches();
    const limit = @import("arch/x86_64/physical_layout.zig").low_memory_limit;
    const first = try list.popContiguousBelow(1, limit);
    try list.appendPage(first);
    const second = try list.popContiguousBelow(1, limit);
    try std.testing.expect(second >= 4096 and second < limit);
    try list.appendPage(second);
    const range = try list.popContiguousBelow(64, 129 * 4096);
    try list.appendContiguousRange(range, 64);
    try std.testing.expectEqual(@as(usize, 128), list.availableBelow(129 * 4096).total);
    try std.testing.expect(@import("sync/lock_diagnostics.zig").testCpu().head == null);
    try list.validate();
}
