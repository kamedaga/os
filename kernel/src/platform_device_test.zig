const std = @import("std");
const kernel = @import("kernel.zig");
const platform = @import("platform_device.zig");

test "platform actual device IDT construction preserves maintenance IPI and rejects old 42 collision before mutation" {
    const arch = @import("arch/x86_64/platform.zig");
    const interrupts = @import("interrupts.zig");
    const boot = @import("boot/main_static.zig");
    const pci = @import("pci.zig");
    var table: [256]interrupts.IdtEntry = @splat(interrupts.zeroIdtEntry());
    interrupts.setIdtEntry(&table, boot.scheduler_wake_ipi_vector, arch.gdt_kernel_code_selector, 0x12345678, 0x8e);
    interrupts.setIdtEntry(&table, boot.lapic_timer_vector, arch.gdt_kernel_code_selector, 0x87654321, 0x8e);
    const maintenance = table[boot.scheduler_wake_ipi_vector];
    const timer = table[boot.lapic_timer_vector];
    var targets = std.mem.zeroes(arch.TrapTargets);
    targets.lapic_timer_vector = boot.lapic_timer_vector;
    targets.scheduler_wake_ipi_vector = boot.scheduler_wake_ipi_vector;
    targets.unrouted_device_interrupt_vector = 0x41;
    targets.device_interrupt_stub = 0xabcdef;
    targets.device_interrupt_vector = platform.interrupt_vector;
    targets.device_interrupt_vector_count = pci.interrupt_vector_end_exclusive - platform.interrupt_vector;
    try std.testing.expect(arch.installDeviceInterruptGates(&table, targets));
    try std.testing.expectEqualDeep(maintenance, table[boot.scheduler_wake_ipi_vector]);
    try std.testing.expectEqualDeep(timer, table[boot.lapic_timer_vector]);
    try std.testing.expectEqual(@as(u16, 0xcdef), table[platform.interrupt_vector].offset_low);
    try std.testing.expectEqualDeep(table[platform.interrupt_vector], table[pci.interrupt_vector_base]);
    try std.testing.expectEqualDeep(table[platform.interrupt_vector], table[pci.interrupt_vector_end_exclusive - 1]);
    const before = table;
    targets.device_interrupt_vector = 0x42;
    targets.device_interrupt_vector_count = pci.interrupt_vector_end_exclusive - 0x42;
    try std.testing.expect(!arch.installDeviceInterruptGates(&table, targets));
    try std.testing.expectEqualDeep(before, table);
}

const Fixture = struct {
    state: *kernel.KernelState,
    runtime: []align(4096) u8,
    fn init() !Fixture {
        const runtime = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
        errdefer std.testing.allocator.free(runtime);
        try std.testing.expect(kernel.initRuntimeStorage(runtime));
        const state = try std.testing.allocator.create(kernel.KernelState);
        errdefer std.testing.allocator.destroy(state);
        try state.initFromDetectedRegionsInPlace(1);
        platform.TestSupport.begin();
        return .{ .state = state, .runtime = runtime };
    }
    fn deinit(self: *Fixture) void {
        for (0..self.state.process_capacity) |i| self.state.releaseFdTableForProcessIndex(i);
        self.state.deinitPools();
        platform.TestSupport.end();
        std.testing.allocator.destroy(self.state);
        std.testing.allocator.free(self.runtime);
    }
};

test "platform actual FD transfer attenuation typed IRQ event matching and last close" {
    var fixture = try Fixture.init();
    defer fixture.deinit();
    const state = fixture.state;
    const p0 = kernel.processPrincipalFromIndex(0).?;
    const p1 = kernel.processPrincipalFromIndex(1).?;
    const rights = kernel.FdRights{ .inspect = true, .query = true, .dup = true, .transfer = true, .read = true, .write = true, .controller_write = true, .close = true };
    const grant = platform.claim(0).?;
    const fd = try state.createPlatformDeviceFd(p0, grant, rights, .{}, 16);
    try std.testing.expectEqual(kernel.KernelObjectKind.platform_device, state.fdInfo(p0, fd).?.kind);
    try std.testing.expect(state.deviceObjectForFd(p0, fd, .{}) == null);
    const copy = try state.dupFd(p0, fd, 16, rights, .{});
    const moved = try state.transferFd(p0, p1, copy, 16, rights, .{}, .move);
    _ = moved;
    var attenuated = rights;
    attenuated.controller_write = false;
    attenuated.transfer = false;
    const readonly_commands = try state.dupFd(p0, fd, 16, attenuated, .{});
    try std.testing.expect(state.fdPayloadWithRightsConst(p0, readonly_commands, .{ .controller_write = true }) == null);
    try std.testing.expectError(kernel.KernelError.InvalidState, state.dupFd(p0, readonly_commands, 16, rights, .{}));
    const encoding = @import("state/types.zig").IpcTransferredFd.init(state.fdEntryConst(p0, fd).?.object, rights, .{});
    try std.testing.expect(encoding.rights().controller_write);
    const irq = platform.acquireIrq(grant).?;
    const irq_rights = kernel.FdRights{ .query = true, .dup = true, .irq_wait = true, .irq_ack = true, .poll = true, .read = true, .close = true };
    const irq_fd = try state.createIrqFd(p0, .{ .source = .{ .platform = irq }, .vector = 1 }, irq_rights, .{}, 16);
    const irq_alias = try state.dupFd(p0, irq_fd, 16, irq_rights, .{});
    try std.testing.expect(platform.TestSupport.masked());
    try std.testing.expect(platform.setMask(irq, false));
    var owners: [4]kernel.PrincipalId = undefined;
    try std.testing.expectEqual(@as(usize, 0), state.recordDeviceInterruptEvent(1, 1, &owners));
    var stale = irq;
    stale.epoch += 1;
    try std.testing.expectEqual(@as(usize, 0), state.recordInterruptEvent(.{ .platform = stale }, 1, &owners));
    try std.testing.expectEqual(@as(usize, 0), state.recordInterruptEvent(.{ .platform = irq }, 2, &owners));
    try std.testing.expectEqual(@as(usize, 1), state.recordInterruptEvent(.{ .platform = irq }, 1, &owners));
    try std.testing.expectEqual(@as(?u64, 1), state.irqEventCountForFd(p0, irq_fd, .{ .irq_wait = true }));
    try state.closeFd(p0, irq_fd);
    try std.testing.expect(platform.irqValid(irq));
    try state.closeFd(p0, fd);
    try state.closeFd(p0, readonly_commands);
    state.releaseFdTableForProcessIndex(1);
    try std.testing.expect(platform.valid(grant)); // IRQ alias owns the final retain.
    try std.testing.expect(platform.claim(0) == null);
    try state.closeFd(p0, irq_alias);
    try std.testing.expect(platform.TestSupport.masked());
    try std.testing.expect(!platform.valid(grant));
    const replacement = platform.claim(0).?;
    try std.testing.expect(replacement.generation != grant.generation);
    platform.closeIrq(irq); // Stale close must not poison the replacement.
    try std.testing.expect(platform.valid(replacement));
    platform.release(replacement);
}

test "platform failed final IRQ drain discharges ownership and prohibits new claims" {
    var fixture = try Fixture.init();
    defer fixture.deinit();
    const p0 = kernel.processPrincipalFromIndex(0).?;
    const grant = platform.claim(0).?;
    const irq = platform.acquireIrq(grant).?;
    const fd = try fixture.state.createIrqFd(p0, .{ .source = .{ .platform = irq }, .vector = 1 }, .{ .close = true }, .{}, 16);
    platform.release(grant);
    platform.TestSupport.failDrain(true);
    try fixture.state.closeFd(p0, fd);
    try std.testing.expectEqual(@as(u32, 0), platform.TestSupport.references());
    try std.testing.expect(platform.claim(0) == null);
    try std.testing.expect(platform.interruptSource() == null);
}

const FailingAllocator = struct {
    const storage = @import("memory/owned_storage.zig");
    calls: usize = 0,
    fail_at: usize,
    fn allocator(self: *@This()) storage.Allocator {
        return .{ .context = self, .acquire = acquire, .release = release };
    }
    fn acquire(context: *anyopaque, pages: usize) ?storage.MappedExtent {
        const self: *@This() = @ptrCast(@alignCast(context));
        self.calls += 1;
        if (self.calls == self.fail_at) return null;
        const host = @import("memory/slot_table.zig").hostTestAllocator();
        return host.acquire(host.context, pages);
    }
    fn release(_: *anyopaque, grant: storage.PhysicalExtent) bool {
        const host = @import("memory/slot_table.zig").hostTestAllocator();
        return host.release(host.context, grant);
    }
};

test "platform OOM at every Device and IRQ FD reservation rolls back only unpublished ownership" {
    const p0 = kernel.processPrincipalFromIndex(0).?;
    for ([_]bool{ false, true }) |irq_object| {
        var succeeded = false;
        var failure: usize = 1;
        while (failure <= 64) : (failure += 1) {
            var allocator = FailingAllocator{ .fail_at = failure };
            var fixture = try Fixture.init();
            defer fixture.deinit();
            const state = fixture.state;
            state.pool_allocator = allocator.allocator();
            const grant = platform.claim(0).?;
            const irq = if (irq_object) platform.acquireIrq(grant).? else null;
            const result = if (irq) |source|
                state.createIrqFd(p0, .{ .source = .{ .platform = source }, .vector = 1 }, .{ .close = true }, .{}, 16)
            else
                state.createPlatformDeviceFd(p0, grant, .{ .close = true }, .{}, 16);
            if (result) |fd| {
                try state.closeFd(p0, fd);
                succeeded = true;
                if (irq_object) platform.release(grant);
            } else |err| {
                try std.testing.expectEqual(kernel.KernelError.OutOfFreePages, err);
                // A failed constructor cannot consume the caller's retain or
                // close a hardware lease that the caller must still roll back.
                try std.testing.expect(platform.valid(grant));
                if (irq) |source| {
                    try std.testing.expect(platform.irqValid(source));
                    platform.closeIrq(source);
                }
                platform.release(grant);
                var objects = state.fd_objects.iterator();
                while (objects.next()) |entry| {
                    try std.testing.expect(entry.value.kind != .platform_device and entry.value.kind != .irq);
                }
            }
            try std.testing.expectEqual(@as(u32, 0), platform.TestSupport.references());
            if (succeeded) break;
        }
        try std.testing.expect(succeeded);
        try std.testing.expect(failure > 1);
    }
}

test "platform failed FD install leaves claim and IRQ lease to caller rollback" {
    var fixture = try Fixture.init();
    defer fixture.deinit();
    const state = fixture.state;
    const p0 = kernel.processPrincipalFromIndex(0).?;
    const grant = platform.claim(0).?;
    try std.testing.expectError(kernel.KernelError.TableFull, state.createPlatformDeviceFd(p0, grant, .{ .close = true }, .{}, @intCast(kernel.fd_number_capacity)));
    try std.testing.expect(platform.valid(grant));
    const irq = platform.acquireIrq(grant).?;
    try std.testing.expectError(kernel.KernelError.TableFull, state.createIrqFd(p0, .{ .source = .{ .platform = irq }, .vector = 1 }, .{ .close = true }, .{}, @intCast(kernel.fd_number_capacity)));
    try std.testing.expect(platform.irqValid(irq));
    platform.closeIrq(irq);
    platform.release(grant);
}

test "platform syscall PIO rejects foreign PCI attenuated stale and truncated FDs before hardware" {
    var fixture = try Fixture.init();
    defer fixture.deinit();
    const state = fixture.state;
    const p0 = kernel.processPrincipalFromIndex(0).?;
    const p1 = kernel.processPrincipalFromIndex(1).?;
    const portIo = @import("syscall/capsule.zig").platformPortIo;
    const grant = platform.claim(0).?;
    const rights = kernel.FdRights{ .dup = true, .read = true, .write = true, .controller_write = true, .close = true };
    const fd = try state.createPlatformDeviceFd(p0, grant, rights, .{}, 16);
    const pci_fd = try state.createDeviceFd(p0, 1, rights, .{}, 16);
    const attenuated = try state.dupFd(p0, fd, 16, .{ .read = true, .write = true, .close = true }, .{});
    const read_only = try state.dupFd(p0, fd, 16, .{ .read = true, .close = true }, .{});
    const bad_read: u64 = @bitCast(-@as(i64, @intCast(@import("syscall/numbers.zig").syscall_err_invalid)));
    try std.testing.expectEqual(bad_read, portIo(state, p1, fd, 0x60, 1, false, 0));
    try std.testing.expectEqual(bad_read, portIo(state, p0, pci_fd, 0x60, 1, false, 0));
    try std.testing.expectEqual(bad_read, portIo(state, p0, @as(u64, fd) + (1 << 32), 0x60, 1, false, 0));
    try std.testing.expectEqual(bad_read, portIo(state, p0, fd, 0x60, 2, false, 0));
    try std.testing.expectEqual(bad_read, portIo(state, p0, fd, 0x10060, 1, false, 0));
    const invalid = @import("syscall/numbers.zig").syscall_err_invalid;
    try std.testing.expectEqual(invalid, portIo(state, p0, attenuated, 0x64, 1, true, 0xfe));
    try std.testing.expectEqual(invalid, portIo(state, p0, attenuated, 0x60, 1, true, 1));
    try std.testing.expectEqual(invalid, portIo(state, p0, read_only, 0x60, 1, true, 0));
    try std.testing.expectEqual(invalid, portIo(state, p0, fd, 0x60, 1, true, 256));
    try std.testing.expectEqual(@as(usize, 0), platform.TestSupport.ioOperations());
    // FD attenuation cannot scrub a pending controller command. Even after
    // the fully privileged alias issues D1, WRITE alone cannot supply its data.
    try std.testing.expectEqual(@as(u64, 0), portIo(state, p0, fd, 0x64, 1, true, 0xd1));
    try std.testing.expectEqual(invalid, portIo(state, p0, attenuated, 0x60, 1, true, 1));
    try std.testing.expectEqual(@as(usize, 1), platform.TestSupport.ioOperations());
    try std.testing.expectEqual(@as(u64, 0x5a), portIo(state, p0, read_only, 0x60, 1, false, 0));
    try std.testing.expectEqual(@as(u64, 0), portIo(state, p0, fd, 0x64, 1, true, 0xfe));
    try std.testing.expectEqual(@as(usize, 3), platform.TestSupport.ioOperations());
    try state.closeFd(p0, attenuated);
    try state.closeFd(p0, read_only);
    try state.closeFd(p0, fd);
    try std.testing.expectEqual(bad_read, portIo(state, p0, fd, 0x60, 1, false, 0));
}
