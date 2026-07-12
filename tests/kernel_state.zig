const std = @import("std");
const kernel = @import("kernel");

const KernelState = kernel.KernelState;
const KernelError = kernel.KernelError;
const FreePageList = kernel.FreePageList;
const PrincipalId = kernel.PrincipalId;

const p0: PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
const p1: PrincipalId = kernel.processPrincipalFromIndex(1) orelse unreachable;
const p2: PrincipalId = kernel.processPrincipalFromIndex(2) orelse unreachable;

var fd_capacity_backing: [64 * 1024 * 1024]u8 align(4096) = undefined;
var runtime_storage: [kernel.runtimeStorageBytes()]u8 align(4096) = undefined;

fn initFdState() !KernelState {
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));
    return KernelState.initFromDetectedRegions(1);
}

fn createTestFdObject(s: *KernelState, id: u64) !kernel.KernelObjectRef {
    return s.createKernelObject(.event, .{ .event = id });
}

fn fdRights(comptime fields: anytype) kernel.FdRights {
    var rights = kernel.FdRights{};
    inline for (std.meta.fields(@TypeOf(fields))) |field| {
        @field(rights, field.name) = @field(fields, field.name);
    }
    return rights;
}

fn fdFlags(comptime fields: anytype) kernel.FdFlags {
    var flags = kernel.FdFlags{};
    inline for (std.meta.fields(@TypeOf(fields))) |field| {
        @field(flags, field.name) = @field(fields, field.name);
    }
    return flags;
}

fn vmaProt(comptime fields: anytype) kernel.VmaProt {
    var prot = kernel.VmaProt{};
    inline for (std.meta.fields(@TypeOf(fields))) |field| {
        @field(prot, field.name) = @field(fields, field.name);
    }
    return prot;
}

fn mmapFlags(comptime fields: anytype) kernel.MmapFlags {
    var flags = kernel.MmapFlags{};
    inline for (std.meta.fields(@TypeOf(fields))) |field| {
        @field(flags, field.name) = @field(fields, field.name);
    }
    return flags;
}

const NoopUnmapper = struct {
    pub fn unmap(_: NoopUnmapper, _: PrincipalId, _: u64, _: u64) bool {
        return true;
    }
};

test "fd install allocates lowest slots and close releases object" {
    var s = try initFdState();
    const obj0 = try createTestFdObject(&s, 1);
    const obj1 = try createTestFdObject(&s, 2);

    const rights = fdRights(.{ .dup = true, .transfer = true, .set_flags = true });
    try std.testing.expectEqual(@as(kernel.Fd, 0), try s.installFd(p0, obj0, rights, .{}, 0));
    try std.testing.expectEqual(@as(kernel.Fd, 1), try s.installFd(p0, obj1, rights, .{}, 0));
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(obj0));

    try s.closeFd(p0, 0);
    try std.testing.expectEqual(@as(?u32, null), s.kernelObjectRefCount(obj0));
    try std.testing.expect(s.fdEntryConst(p0, 0) == null);

    const obj2 = try createTestFdObject(&s, 3);
    try std.testing.expectEqual(@as(kernel.Fd, 0), try s.installFd(p0, obj2, rights, .{}, 0));
}

test "fd dup requires dup right and rejects rights escalation" {
    var s = try initFdState();
    const obj = try createTestFdObject(&s, 10);
    const no_dup = fdRights(.{ .transfer = true });
    const fd0 = try s.installFd(p0, obj, no_dup, .{}, 0);
    try std.testing.expectError(KernelError.InvalidState, s.dupFd(p0, fd0, 0, no_dup, .{}));

    try s.closeFd(p0, fd0);
    const obj2 = try createTestFdObject(&s, 11);
    const dup_only = fdRights(.{ .dup = true });
    const fd1 = try s.installFd(p0, obj2, dup_only, .{}, 0);
    const escalated = fdRights(.{ .dup = true, .transfer = true });
    try std.testing.expectError(KernelError.InvalidState, s.dupFd(p0, fd1, 0, escalated, .{}));

    const attenuated = fdRights(.{});
    const fd2 = try s.dupFd(p0, fd1, 0, attenuated, fdFlags(.{ .cloexec = true }));
    try std.testing.expectEqual(@as(kernel.Fd, 1), fd2);
    try std.testing.expectEqual(@as(?u32, 2), s.kernelObjectRefCount(obj2));
    try std.testing.expect((s.fdEntryConst(p0, fd2) orelse unreachable).flags.cloexec);
}

test "fd transfer copy and move enforce transfer right and refcounts" {
    var s = try initFdState();
    const obj = try createTestFdObject(&s, 20);
    const no_transfer = fdRights(.{ .dup = true });
    const fd0 = try s.installFd(p0, obj, no_transfer, .{}, 0);
    try std.testing.expectError(KernelError.InvalidState, s.transferFd(p0, p1, fd0, 0, no_transfer, .{}, .copy));

    try s.closeFd(p0, fd0);
    const obj2 = try createTestFdObject(&s, 21);
    const transferable = fdRights(.{ .transfer = true, .dup = true });
    const fd1 = try s.installFd(p0, obj2, transferable, .{}, 0);
    const copied = try s.transferFd(p0, p1, fd1, 0, fdRights(.{ .dup = true }), fdFlags(.{ .inherit = true }), .copy);
    try std.testing.expectEqual(@as(kernel.Fd, 0), copied);
    try std.testing.expectEqual(@as(?u32, 2), s.kernelObjectRefCount(obj2));
    try std.testing.expect((s.fdEntryConst(p1, copied) orelse unreachable).flags.inherit);

    const moved = try s.transferFd(p0, p2, fd1, 0, fdRights(.{}), .{}, .move);
    try std.testing.expectEqual(@as(kernel.Fd, 0), moved);
    try std.testing.expect(s.fdEntryConst(p0, fd1) == null);
    try std.testing.expectEqual(@as(?u32, 2), s.kernelObjectRefCount(obj2));
}

test "capsule device authority is a native fd object" {
    var s = try initFdState();
    const rights = fdRights(.{
        .inspect = true,
        .dup = true,
        .transfer = true,
        .close = true,
        .query = true,
        .config_read = true,
        .derive_mmio = true,
        .derive_dma = true,
        .derive_irq = true,
    });
    const fd = try s.createDeviceFd(p0, 0x1001, rights, .{}, 16);
    try std.testing.expectEqual(@as(kernel.Fd, 16), fd);
    const info = s.fdInfo(p0, fd) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(kernel.KernelObjectKind.device, info.kind);
    try std.testing.expectEqual(kernel.fdRightsToBits(rights), info.rights_bits);

    const device = s.deviceObjectForFd(p0, fd, fdRights(.{ .query = true })) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(@as(kernel.DmaDeviceId, 0x1001), device.device);
    try std.testing.expect(s.deviceObjectForFd(p0, fd, fdRights(.{ .config_write = true })) == null);

    const child_fd = try s.transferFd(p0, p1, fd, 16, fdRights(.{ .query = true, .close = true }), .{}, .copy);
    try std.testing.expectEqual(@as(kernel.Fd, 16), child_fd);
    try std.testing.expect((s.deviceObjectForFd(p1, child_fd, fdRights(.{ .query = true })) orelse return error.TestExpectedEqual).device == 0x1001);
    try std.testing.expect(s.deviceObjectForFd(p1, child_fd, fdRights(.{ .derive_mmio = true })) == null);
}

test "process fd exposes process object kind and lifecycle state" {
    var s = try initFdState();
    const rights = fdRights(.{
        .inspect = true,
        .dup = true,
        .transfer = true,
        .wait = true,
        .close = true,
        .spawn = true,
        .kill = true,
        .map_into = true,
        .set_context = true,
    });
    const fd = try s.createProcessFd(p0, .{
        .principal_raw = @intFromEnum(p1),
        .state = .active,
        .exit_code = 0,
    }, rights, .{}, 16);

    const info = s.fdInfo(p0, fd) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(kernel.KernelObjectKind.process, info.kind);
    try std.testing.expectEqual(kernel.fdRightsToBits(rights), info.rights_bits);
    try std.testing.expectEqual(@as(u64, @intFromEnum(kernel.TaskObjectState.active)), info.extra);

    const process = try s.setProcessObjectStateForFd(p0, fd, fdRights(.{ .kill = true }), .killed, 7);
    try std.testing.expectEqual(@as(kernel.PrincipalRaw, @intFromEnum(p1)), process.principal_raw);
    try std.testing.expectEqual(kernel.TaskObjectState.killed, process.state);
    try std.testing.expectEqual(@as(u32, 7), process.exit_code);

    const updated = s.processObjectForFd(p0, fd, fdRights(.{ .wait = true })) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(kernel.TaskObjectState.killed, updated.state);
    try std.testing.expectEqual(@as(u32, 7), updated.exit_code);
}

test "one thread can wait on multiple process fds" {
    var s = try initFdState();
    const rights = fdRights(.{ .wait = true, .poll = true, .close = true });
    const fd1 = try s.createProcessFd(p0, .{
        .principal_raw = @intFromEnum(p1),
        .state = .active,
        .exit_code = 0,
    }, rights, .{}, 16);
    const fd2 = try s.createProcessFd(p0, .{
        .principal_raw = @intFromEnum(p2),
        .state = .active,
        .exit_code = 0,
    }, rights, .{}, 16);

    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, fd1, 1, 0x1000, 7, 11));
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, fd2, 1, 0x1018, 7, 11));

    var targets: [2]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 1), s.takeTaskReadableWaitersForPrincipal(p1, targets[0..]));
    try std.testing.expectEqual(@as(u64, 0x1000), targets[0].pollfd_va);
    try std.testing.expectEqual(@as(usize, 1), s.takeTaskReadableWaitersForPrincipal(p2, targets[0..]));
    try std.testing.expectEqual(@as(u64, 0x1018), targets[0].pollfd_va);
}

test "thread fd stores owner slot generation and lifecycle state" {
    var s = try initFdState();
    const rights = fdRights(.{
        .inspect = true,
        .dup = true,
        .transfer = true,
        .wait = true,
        .close = true,
        .start = true,
        .kill = true,
        .set_context = true,
    });
    const fd0 = try s.createThreadFd(p0, .{
        .owner_principal_raw = @intFromEnum(p1),
        .thread_index = 3,
        .thread_generation = 10,
        .state = .active,
        .exit_code = 0,
    }, rights, .{}, 16);
    const fd1 = try s.createThreadFd(p0, .{
        .owner_principal_raw = @intFromEnum(p1),
        .thread_index = 4,
        .thread_generation = 11,
        .state = .active,
        .exit_code = 0,
    }, rights, .{}, 16);

    try std.testing.expect(fd0 != fd1);
    const info = s.fdInfo(p0, fd0) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(kernel.KernelObjectKind.thread, info.kind);
    try std.testing.expectEqual(@as(u64, @intFromEnum(kernel.TaskObjectState.active)), info.extra);

    const killed = try s.setThreadObjectStateForFd(p0, fd0, fdRights(.{ .kill = true }), .killed, 9);
    try std.testing.expectEqual(@as(u32, 3), killed.thread_index);
    try std.testing.expectEqual(@as(u32, 10), killed.thread_generation);
    try std.testing.expectEqual(kernel.TaskObjectState.killed, killed.state);
    try std.testing.expectEqual(@as(u32, 9), killed.exit_code);

    const still_active = s.threadObjectForFd(p0, fd1, fdRights(.{ .wait = true })) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(kernel.TaskObjectState.active, still_active.state);
    try std.testing.expectEqual(@as(u32, 11), still_active.thread_generation);
}

test "fd replace releases destination exactly once" {
    var s = try initFdState();
    const obj_a = try createTestFdObject(&s, 30);
    const obj_b = try createTestFdObject(&s, 31);
    const rights = fdRights(.{ .dup = true, .transfer = true });
    const fd_a = try s.installFd(p0, obj_a, rights, .{}, 0);
    const fd_b = try s.installFd(p0, obj_b, rights, .{}, 0);

    try s.replaceFd(p0, fd_a, fd_b, fdRights(.{ .transfer = true }), fdFlags(.{ .private = true }));
    try std.testing.expectEqual(@as(?u32, null), s.kernelObjectRefCount(obj_a));
    try std.testing.expectEqual(@as(?u32, 2), s.kernelObjectRefCount(obj_b));
    const replaced = s.fdEntryConst(p0, fd_a) orelse unreachable;
    try std.testing.expect(replaced.rights.transfer);
    try std.testing.expect(!replaced.rights.dup);
    try std.testing.expect(replaced.flags.private);
}

test "fd set flags requires set_flags right" {
    var s = try initFdState();
    const obj = try createTestFdObject(&s, 40);
    const fd0 = try s.installFd(p0, obj, fdRights(.{}), .{}, 0);
    try std.testing.expectError(KernelError.InvalidState, s.setFdFlags(p0, fd0, fdFlags(.{ .nonblock = true }), fdFlags(.{ .nonblock = true })));

    try s.closeFd(p0, fd0);
    const obj2 = try createTestFdObject(&s, 41);
    const fd1 = try s.installFd(p0, obj2, fdRights(.{ .set_flags = true }), .{}, 0);
    try s.setFdFlags(p0, fd1, fdFlags(.{ .nonblock = true }), fdFlags(.{ .nonblock = true }));
    try std.testing.expect((s.fdEntryConst(p0, fd1) orelse unreachable).flags.nonblock);
}

test "fd process runtime reset releases fd table" {
    var s = try initFdState();
    const obj = try createTestFdObject(&s, 50);
    _ = try s.installFd(p0, obj, fdRights(.{ .dup = true }), .{}, 0);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(obj));

    s.resetProcessRuntimeTables(0);
    try std.testing.expect(s.fdEntryConst(p0, 0) == null);
    try std.testing.expectEqual(@as(?u32, null), s.kernelObjectRefCount(obj));
}

test "fd process exit and remove release fd table" {
    var s = try initFdState();
    const exit_obj = try createTestFdObject(&s, 55);
    _ = try s.installFd(p1, exit_obj, fdRights(.{}), .{}, 0);
    try std.testing.expect(s.markProcessExited(p1));
    try std.testing.expectEqual(@as(?u32, null), s.kernelObjectRefCount(exit_obj));

    try std.testing.expect(s.ensureProcessDescriptor(p1, "reused"));
    const remove_obj = try createTestFdObject(&s, 56);
    _ = try s.installFd(p1, remove_obj, fdRights(.{}), .{}, 0);
    try std.testing.expect(s.removeProcessDescriptor(p1));
    try std.testing.expectEqual(@as(?u32, null), s.kernelObjectRefCount(remove_obj));
}

test "fd process capacity growth preserves extra fd tables" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(
        0,
        @intFromPtr(&fd_capacity_backing),
        fd_capacity_backing.len / 4096,
    );

    try std.testing.expect(s.ensureProcessCapacity(kernel.process_count + 1, &free_list));
    const extra_principal = kernel.processPrincipalFromIndex(kernel.process_count).?;
    try std.testing.expect(s.ensureProcessDescriptor(extra_principal, "extra"));
    const obj = try createTestFdObject(&s, 60);
    const fd = try s.installFd(extra_principal, obj, fdRights(.{ .dup = true }), fdFlags(.{ .cloexec = true }), 0);

    try std.testing.expect(s.ensureProcessCapacity(kernel.process_count * 2 + 1, &free_list));
    const entry = s.fdEntryConst(extra_principal, fd) orelse unreachable;
    try std.testing.expect(entry.flags.cloexec);
    try std.testing.expect(entry.rights.dup);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(obj));
}

test "ipc endpoint fd sends and receives inline words" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const rights = fdRights(.{ .send = true, .recv = true, .close = true });
    const endpoint = try s.createIpcEndpointFd(p0, rights, .{}, 16);

    try s.ipcSend(p0, endpoint, .{ .words = .{ 11, 22, 33, 44 } }, &free_list);
    const received = try s.ipcRecv(p0, endpoint, 0, 16, &free_list);
    try std.testing.expectEqual(@as(usize, 0), received.fd_count);
    try std.testing.expectEqual(@as(u64, 11), received.words[0]);
    try std.testing.expectEqual(@as(u64, 22), received.words[1]);
    try std.testing.expectEqual(@as(u64, 33), received.words[2]);
    try std.testing.expectEqual(@as(u64, 44), received.words[3]);
    try std.testing.expectError(KernelError.MailboxEmpty, s.ipcRecv(p0, endpoint, 0, 16, &free_list));
}

test "ipc fd passing moves attenuated vmo fd through endpoint" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const endpoint_rights = fdRights(.{ .send = true, .recv = true, .transfer = true, .close = true });
    const endpoint = try s.createIpcEndpointFd(p0, endpoint_rights, .{}, 16);
    const remote_endpoint = try s.transferFd(p0, p1, endpoint, 16, fdRights(.{ .send = true, .close = true }), .{}, .copy);

    const source_rights = fdRights(.{ .transfer = true, .close = true, .map_read = true, .map_write = true });
    const source_vmo_fd = try s.createAnonymousVmoFd(p1, 4096, source_rights, .{}, 16);
    const source_vmo = s.nativeVmoRefForFd(p1, source_vmo_fd) orelse unreachable;

    const send_fds = [_]kernel.IpcSendFd{.{
        .fd = source_vmo_fd,
        .rights = fdRights(.{ .close = true, .map_read = true }),
        .flags = fdFlags(.{ .cloexec = true }),
        .move = true,
    }};
    try s.ipcSend(p1, remote_endpoint, .{ .words = .{ 1, 0, 0, 0 }, .fds = send_fds[0..] }, &free_list);
    try std.testing.expect(s.fdEntryConst(p1, source_vmo_fd) == null);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(source_vmo));

    const received = try s.ipcRecv(p0, endpoint, 1, 16, &free_list);
    try std.testing.expectEqual(@as(usize, 1), received.fd_count);
    const received_fd = received.fds[0].fd;
    const received_entry = s.fdEntryConst(p0, received_fd) orelse unreachable;
    try std.testing.expect(received_entry.rights.close);
    try std.testing.expect(received_entry.rights.map_read);
    try std.testing.expect(!received_entry.rights.map_write);
    try std.testing.expect(received_entry.flags.cloexec);
    try std.testing.expectEqual(source_vmo, s.nativeVmoRefForFd(p0, received_fd) orelse unreachable);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(source_vmo));
}

test "ipc channel pair sends to peer receive queue" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const pair = try s.createIpcChannelPairFds(p0, fdRights(.{ .send = true, .recv = true, .close = true }), .{}, 16);

    try s.ipcSend(p0, pair.a, .{ .words = .{ 5, 6, 7, 8 } }, &free_list);
    try std.testing.expectError(KernelError.MailboxEmpty, s.ipcRecv(p0, pair.a, 0, 16, &free_list));
    const received = try s.ipcRecv(p0, pair.b, 0, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 5), received.words[0]);
    try std.testing.expectEqual(@as(u64, 8), received.words[3]);
}

test "starting a new fd wait clears stale channel waiters for the thread" {
    var s = try initFdState();
    const rights = fdRights(.{
        .send = true,
        .recv = true,
        .wait = true,
        .poll = true,
        .close = true,
    });
    const first = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const second = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    try s.registerIpcReadableWaiterForFd(p0, first.a, 1, 0x1000, 7, 11);
    try s.registerIpcReadableWaiterForFd(p0, second.a, 1, 0x1018, 7, 11);

    s.unregisterFdWaitersForThread(p0, 7, 11);

    var targets: [2]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), try s.wakeIpcWaitersForSendFd(p0, first.b, targets[0..]));
    try std.testing.expectEqual(@as(usize, 0), try s.wakeIpcWaitersForSendFd(p0, second.b, targets[0..]));
}

test "ipc call attaches one-shot reply fd" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const endpoint = try s.createIpcEndpointFd(p0, fdRights(.{ .recv = true, .call = true, .transfer = true, .close = true }), .{}, 16);
    const client_endpoint = try s.transferFd(p0, p1, endpoint, 16, fdRights(.{ .call = true, .close = true }), .{}, .copy);

    const client_reply = try s.ipcCall(p1, client_endpoint, .{ .words = .{ 99, 0, 0, 0 } }, 16, &free_list);
    const request = try s.ipcRecv(p0, endpoint, 1, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 99), request.words[0]);
    try std.testing.expectEqual(@as(usize, 1), request.fd_count);
    const server_reply = request.fds[0].fd;
    const server_reply_entry = s.fdEntryConst(p0, server_reply) orelse unreachable;
    try std.testing.expect(server_reply_entry.rights.send);
    try std.testing.expect(!server_reply_entry.rights.recv);

    try s.ipcReply(p0, server_reply, .{ .words = .{ 1234, 0, 0, 0 } }, &free_list);
    try std.testing.expectError(KernelError.InvalidState, s.ipcReply(p0, server_reply, .{ .words = .{ 1, 0, 0, 0 } }, &free_list));

    const reply = try s.ipcRecv(p1, client_reply, 0, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 1234), reply.words[0]);
}

test "irq fd records interrupt events by vector" {
    var s = try initFdState();
    const irq_fd = try s.createIrqFd(p0, .{
        .device = 0x1001,
        .kind = .msix,
        .vector = 0x41,
    }, fdRights(.{ .irq_wait = true, .poll = true, .read = true, .close = true }), .{}, 16);

    try std.testing.expectEqual(@as(?u64, 0), s.irqEventCountForFd(p0, irq_fd, fdRights(.{ .irq_wait = true })));
    try std.testing.expectEqual(@as(?u64, 0), s.fdPollEventsWithWriteMin(p0, irq_fd, 1, 0, 0));

    var wake_owners: [4]kernel.PrincipalId = undefined;
    const wake_count = s.recordDeviceInterruptEvent(0x41, wake_owners[0..]);
    try std.testing.expectEqual(@as(usize, 1), wake_count);
    try std.testing.expectEqual(p0, wake_owners[0]);
    try std.testing.expectEqual(@as(?u64, 1), s.irqEventCountForFd(p0, irq_fd, fdRights(.{ .irq_wait = true })));
    try std.testing.expectEqual(@as(?u64, 1), s.fdPollEventsWithWriteMin(p0, irq_fd, 1, 0, 0));
    try std.testing.expect(s.acknowledgeIrqEventCountForFd(p0, irq_fd, 1));
    try std.testing.expectEqual(@as(?u64, 0), s.fdPollEventsWithWriteMin(p0, irq_fd, 1, 0, 0));

    try std.testing.expectEqual(@as(usize, 0), s.recordDeviceInterruptEvent(0x42, wake_owners[0..]));
    try std.testing.expectEqual(@as(?u64, 1), s.irqEventCountForFd(p0, irq_fd, fdRights(.{ .irq_wait = true })));
    try std.testing.expectEqual(@as(usize, 1), s.recordDeviceInterruptEvent(0x41, wake_owners[0..]));
    try std.testing.expectEqual(@as(?u64, 1), s.fdPollEventsWithWriteMin(p0, irq_fd, 1, 0, 0));
}

test "physical page allocation returns a page handle" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x1_0000_0000, 2);

    const page = try s.allocPhysicalPage(&free_list);
    try std.testing.expectEqual(@as(u64, 0x1_0000_0000), page.paddr);
}

test "page-backed vmo fd uses dynamic fd range and reports fd info" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x20_0000, 2);
    const original_free = free_list.len;

    const rights = fdRights(.{
        .inspect = true,
        .transfer = true,
        .close = true,
        .map_read = true,
        .map_write = true,
        .map_exec = true,
        .share = true,
    });
    const flags = fdFlags(.{ .cloexec = true });
    const fd = try s.createAnonymousVmoFdWithPages(p0, 6000, rights, flags, 16, &free_list);

    try std.testing.expectEqual(@as(kernel.Fd, 16), fd);
    try std.testing.expectEqual(original_free - 2, free_list.len);

    const info = s.fdInfo(p0, fd) orelse unreachable;
    try std.testing.expectEqual(kernel.KernelObjectKind.vmo, info.kind);
    try std.testing.expectEqual(@as(u64, 8192), info.size_bytes);
    try std.testing.expectEqual(kernel.fdRightsToBits(rights), info.rights_bits);
    try std.testing.expectEqual(kernel.fdFlagsToBits(flags), info.flags_bits);

    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    try std.testing.expect(s.nativeVmoPagePaddr(vmo, 0) != null);
    try std.testing.expect(s.nativeVmoPagePaddr(vmo, 1) != null);

    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(original_free, free_list.len);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo));
}

test "anonymous vmo fd maps through vma ledger without page capability install" {
    var s = try initFdState();
    const rights = fdRights(.{
        .dup = true,
        .map_read = true,
        .map_write = true,
        .transfer = true,
    });
    const fd = try s.createAnonymousVmoFd(p0, 8192, rights, .{}, 0);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    try std.testing.expectEqual(@as(?u64, 8192), s.nativeVmoSize(vmo));
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(vmo));

    const mapped = try s.mmapFd(
        p0,
        fd,
        0x4000_0000,
        8192,
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .shared = true }),
        0,
    );
    try std.testing.expectEqual(@as(u64, 0x4000_0000), mapped);
    try std.testing.expectEqual(@as(?u32, 2), s.nativeVmoRefCount(vmo));
    const vma = s.vmaEntryConst(p0, mapped) orelse unreachable;
    try std.testing.expect(vma.prot.read);
    try std.testing.expect(vma.prot.write);
    try std.testing.expect(vma.flags.shared);
    try std.testing.expectEqual(vmo, vma.vmo);
}

test "mmap fd rejects rights escalation and overlapping vma" {
    var s = try initFdState();
    const fd = try s.createAnonymousVmoFd(p0, 12288, fdRights(.{ .map_read = true }), .{}, 0);

    try std.testing.expectError(KernelError.InvalidState, s.mmapFd(
        p0,
        fd,
        0x4100_0000,
        4096,
        vmaProt(.{ .write = true }),
        .{},
        0,
    ));

    _ = try s.mmapFd(p0, fd, 0x4100_0000, 8192, vmaProt(.{ .read = true }), .{}, 0);
    try std.testing.expectError(KernelError.InvalidState, s.mmapFd(
        p0,
        fd,
        0x4100_1000,
        4096,
        vmaProt(.{ .read = true }),
        .{},
        0,
    ));
}

test "fd close and munmap release native vmo lifetimes independently" {
    var s = try initFdState();
    const fd = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{ .map_read = true }), .{}, 0);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    _ = try s.mmapFd(p0, fd, 0x4200_0000, 4096, vmaProt(.{ .read = true }), .{}, 0);
    try std.testing.expectEqual(@as(?u32, 2), s.nativeVmoRefCount(vmo));

    try s.closeFd(p0, fd);
    try std.testing.expect(s.fdEntryConst(p0, fd) == null);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(vmo));

    var free_list = FreePageList{};
    try s.munmapRangeWithFreeList(p0, 0x4200_0000, 4096, &free_list);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo));
    try std.testing.expect(s.vmaEntryConst(p0, 0x4200_0000) == null);
}

test "process reset releases vma table and native vmo after fd close" {
    var s = try initFdState();
    const fd = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{ .map_read = true }), .{}, 0);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    _ = try s.mmapFd(p0, fd, 0x4300_0000, 4096, vmaProt(.{ .read = true }), .{}, 0);

    try s.closeFd(p0, fd);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(vmo));
    s.resetProcessRuntimeTables(0);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo));
    try std.testing.expect(s.vmaEntryConst(p0, 0x4300_0000) == null);
}

test "native vmo pages return to free list after vma and fd release" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x1_0000_0000, 4);
    const original_free = free_list.len;

    var pages: [2]u64 = undefined;
    pages[0] = (try s.allocPhysicalPage(&free_list)).paddr;
    pages[1] = (try s.allocPhysicalPage(&free_list)).paddr;
    try std.testing.expectEqual(original_free - 2, free_list.len);

    const fd = try s.createAnonymousVmoFd(p0, 8192, fdRights(.{ .map_read = true, .map_write = true }), .{}, 0);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    try s.installNativeVmoPages(vmo, 0, pages[0..]);
    try std.testing.expectEqual(@as(?u64, pages[0]), s.nativeVmoPagePaddr(vmo, 0));
    try std.testing.expectEqual(@as(?u64, pages[1]), s.nativeVmoPagePaddr(vmo, 1));

    _ = try s.mmapFd(p0, fd, 0x4400_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .anonymous = true }), 0);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(original_free - 2, free_list.len);
    try s.munmapRangeWithFreeList(p0, 0x4400_0000, 8192, &free_list);
    try std.testing.expectEqual(original_free, free_list.len);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo));
}

test "mprotect split and mremap keep vmo refs until final munmap" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x30_0000, 8);
    const original_free = free_list.len;

    const fd = try s.createAnonymousVmoFdWithPages(
        p0,
        12288,
        fdRights(.{ .close = true, .map_read = true, .map_write = true }),
        .{},
        16,
        &free_list,
    );
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    _ = try s.mmapFd(
        p0,
        fd,
        0x4800_0000,
        12288,
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .anonymous = true }),
        0,
    );
    try s.setVmaProtRange(p0, 0x4800_1000, 4096, vmaProt(.{ .read = true }));
    try std.testing.expectEqual(@as(?u32, 4), s.nativeVmoRefCount(vmo));

    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(@as(?u32, 3), s.nativeVmoRefCount(vmo));
    try s.munmapRangeWithFreeList(p0, 0x4800_0000, 12288, &free_list);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo));
    try std.testing.expectEqual(original_free, free_list.len);

    const fd2 = try s.createAnonymousVmoFdWithPages(
        p0,
        4096,
        fdRights(.{ .close = true, .map_read = true, .map_write = true }),
        .{},
        16,
        &free_list,
    );
    const vmo2 = s.nativeVmoRefForFd(p0, fd2) orelse unreachable;
    _ = try s.mmapFd(
        p0,
        fd2,
        0x4810_0000,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .anonymous = true }),
        0,
    );
    const moved = try s.mremapRangeWithFreeList(p0, 0x4810_0000, 4096, 4096, 0x4820_0000, true, true, &free_list);
    try std.testing.expectEqual(@as(u64, 0x4820_0000), moved);
    try s.closeFdWithFreeList(p0, fd2, &free_list);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(vmo2));
    try s.munmapRangeWithFreeList(p0, 0x4820_0000, 4096, &free_list);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo2));
    try std.testing.expectEqual(original_free, free_list.len);
}

test "fork fd and vma clones release vmo pages only after child cleanup" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x40_0000, 4);
    const original_free = free_list.len;

    const rights = fdRights(.{ .close = true, .map_read = true, .map_write = true });
    const fd = try s.createAnonymousVmoFdWithPages(p0, 8192, rights, .{}, 16, &free_list);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    _ = try s.mmapFd(p0, fd, 0x4900_0000, 8192, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);
    try s.cloneFdTableForFork(p0, p1);
    try s.cloneVmaTableForFork(p0, p1);
    try std.testing.expectEqual(@as(?u32, 3), s.nativeVmoRefCount(vmo));

    try s.closeFdWithFreeList(p0, fd, &free_list);
    try s.munmapRangeWithFreeList(p0, 0x4900_0000, 8192, &free_list);
    try std.testing.expectEqual(@as(?u32, 2), s.nativeVmoRefCount(vmo));
    try std.testing.expectEqual(original_free - 2, free_list.len);

    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo));
    try std.testing.expectEqual(original_free, free_list.len);
}

test "exec vma replacement releases old mappings and moves staged mappings" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x60_0000, 4);
    const original_free = free_list.len;

    const old_fd = try s.createAnonymousVmoFdWithPages(
        p0,
        4096,
        fdRights(.{ .close = true, .map_read = true }),
        .{},
        16,
        &free_list,
    );
    const old_vmo = s.nativeVmoRefForFd(p0, old_fd) orelse unreachable;
    _ = try s.mmapFd(p0, old_fd, 0x4c00_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);

    const staged_fd = try s.createAnonymousVmoFdWithPages(
        p1,
        4096,
        fdRights(.{ .close = true, .map_read = true }),
        .{},
        16,
        &free_list,
    );
    const staged_vmo = s.nativeVmoRefForFd(p1, staged_fd) orelse unreachable;
    _ = try s.mmapFd(p1, staged_fd, 0x4d00_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);

    try s.replaceVmaTableForExec(p0, p1, &free_list);
    try std.testing.expect(s.vmaEntryConst(p0, 0x4c00_0000) == null);
    try std.testing.expect((s.vmaEntryConst(p0, 0x4d00_0000) orelse unreachable).vmo.index == staged_vmo.index);
    try std.testing.expect(s.vmaEntryConst(p1, 0x4d00_0000) == null);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(old_vmo));

    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(staged_vmo));
    try s.closeFdWithFreeList(p0, old_fd, &free_list);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(old_vmo));
    try s.munmapRangeWithFreeList(p0, 0x4d00_0000, 4096, &free_list);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(staged_vmo));
    try std.testing.expectEqual(original_free, free_list.len);
}

test "vmo revoke removes all fd vma and pending ipc references" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x50_0000, 6);
    const original_free = free_list.len;

    const owner_rights = fdRights(.{
        .inspect = true,
        .dup = true,
        .transfer = true,
        .close = true,
        .map_read = true,
        .map_write = true,
        .revoke = true,
    });
    const fd = try s.createAnonymousVmoFdWithPages(p0, 8192, owner_rights, .{}, 16, &free_list);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    const dup_fd = try s.dupFd(p0, fd, 16, fdRights(.{ .close = true, .map_read = true }), .{});
    const child_fd = try s.transferFd(
        p0,
        p1,
        fd,
        16,
        fdRights(.{ .transfer = true, .close = true, .map_read = true }),
        .{},
        .copy,
    );

    _ = try s.mmapFd(p0, fd, 0x4a00_0000, 8192, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);
    _ = try s.mmapFd(p1, child_fd, 0x4b00_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);

    const endpoint = try s.createIpcEndpointFd(p2, fdRights(.{ .send = true, .recv = true, .transfer = true, .close = true }), .{}, 16);
    const remote_endpoint = try s.transferFd(p2, p1, endpoint, 16, fdRights(.{ .send = true, .close = true }), .{}, .copy);
    const send_fds = [_]kernel.IpcSendFd{.{
        .fd = child_fd,
        .rights = fdRights(.{ .map_read = true, .close = true }),
        .move = true,
    }};
    try s.ipcSend(p1, remote_endpoint, .{ .words = .{ 7, 0, 0, 0 }, .fds = send_fds[0..] }, &free_list);
    try std.testing.expect(s.fdEntryConst(p1, child_fd) == null);
    try std.testing.expectError(KernelError.InvalidState, s.revokeVmoFdWithFreeList(p0, dup_fd, &free_list, NoopUnmapper{}));

    _ = try s.revokeVmoFdWithFreeList(p0, fd, &free_list, NoopUnmapper{});
    try std.testing.expect(s.fdEntryConst(p0, fd) == null);
    try std.testing.expect(s.fdEntryConst(p0, dup_fd) == null);
    try std.testing.expect(s.vmaEntryConst(p0, 0x4a00_0000) == null);
    try std.testing.expect(s.vmaEntryConst(p1, 0x4b00_0000) == null);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo));
    try std.testing.expectEqual(original_free, free_list.len);
    try std.testing.expectError(KernelError.InvalidState, s.closeFdWithFreeList(p0, fd, &free_list));

    const received = try s.ipcRecv(p2, endpoint, 1, 16, &free_list);
    try std.testing.expectEqual(@as(usize, 0), received.fd_count);
    try std.testing.expectEqual(@as(u64, 7), received.words[0]);
}

test "shared vmo fd pages survive munmap while fd remains open" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x20_0000, 2);
    const original_free = free_list.len;

    const fd = try s.createAnonymousVmoFdWithPages(
        p0,
        4096,
        fdRights(.{ .map_read = true, .map_write = true, .read = true, .close = true }),
        .{},
        0,
        &free_list,
    );
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    const page = s.nativeVmoPagePaddr(vmo, 0) orelse unreachable;
    try std.testing.expectEqual(original_free - 1, free_list.len);

    _ = try s.mmapFd(
        p0,
        fd,
        0x4700_0000,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .shared = true }),
        0,
    );
    try s.munmapRangeWithFreeList(p0, 0x4700_0000, 4096, &free_list);
    try std.testing.expectEqual(page, s.nativeVmoPagePaddr(vmo, 0) orelse unreachable);
    try std.testing.expectEqual(original_free - 1, free_list.len);

    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(original_free, free_list.len);
}

test "native vma fault mapping resolves backing page without page capability" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x1_0000_0000, 2);
    const page = try s.allocPhysicalPage(&free_list);
    const fd = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{ .map_read = true }), .{}, 0);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    try s.installNativeVmoPages(vmo, 0, &[_]u64{page.paddr});
    _ = try s.mmapFd(p0, fd, 0x4500_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);
    try s.closeFdWithFreeList(p0, fd, &free_list);

    const mapping = s.nativeVmaFaultMapping(p0, 0x4500_0000, false, false) orelse unreachable;
    try std.testing.expectEqual(page.paddr, mapping.paddr);
    try std.testing.expect(mapping.prot.read);
    try std.testing.expect(!mapping.prot.write);

    try std.testing.expect(s.nativeVmaFaultMapping(p0, 0x4500_0000, true, false) == null);
    try s.munmapRangeWithFreeList(p0, 0x4500_0000, 4096, &free_list);
    try std.testing.expect(s.nativeVmaFaultMapping(p0, 0x4500_0000, false, false) == null);
}

test "private file vma faults read-only then COWs on write" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x1_0000_0000, 4);
    const source_page = (try s.allocPhysicalPage(&free_list)).paddr;

    const fd = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{
        .map_read = true,
        .map_write = true,
    }), .{}, 0);
    const source_vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    try s.installNativeVmoPages(source_vmo, 0, &[_]u64{source_page});

    _ = try s.mmapFd(
        p0,
        fd,
        0x4600_0000,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true }),
        0,
    );

    const read_mapping = s.nativeVmaFaultMapping(p0, 0x4600_0000, false, false) orelse unreachable;
    try std.testing.expectEqual(source_page, read_mapping.paddr);
    try std.testing.expect(read_mapping.prot.read);
    try std.testing.expect(!read_mapping.prot.write);
    const private_vma_before = s.vmaEntryConst(p0, 0x4600_0000) orelse unreachable;
    try std.testing.expect(private_vma_before.flags.private);
    try std.testing.expect(!private_vma_before.flags.anonymous);
    try std.testing.expectEqual(source_vmo.index, private_vma_before.vmo.index);
    try std.testing.expectEqual(@as(?u64, null), s.nativeVmoPagePaddr(private_vma_before.vmo, 1));
    try std.testing.expectEqual(@as(?u32, 2), s.nativeVmoRefCount(source_vmo));

    const write_mapping = s.ensureNativeVmaCowMapping(
        p0,
        0x4600_0000,
        true,
        false,
        &free_list,
    ) orelse unreachable;
    try std.testing.expect(write_mapping.paddr != source_page);
    try std.testing.expect(write_mapping.prot.write);
    try std.testing.expectEqual(@as(?u32, 2), s.nativeVmoRefCount(source_vmo));

    const private_vma = s.vmaEntryConst(p0, 0x4600_0000) orelse unreachable;
    try std.testing.expect(private_vma.flags.private);
    try std.testing.expect(!private_vma.flags.anonymous);
    try std.testing.expectEqual(source_vmo.index, private_vma.vmo.index);
    try std.testing.expectEqual(source_page, s.nativeVmoPagePaddr(private_vma.vmo, 0) orelse unreachable);
    const reread_mapping = s.nativeVmaFaultMapping(p0, 0x4600_0000, false, false) orelse unreachable;
    try std.testing.expectEqual(write_mapping.paddr, reread_mapping.paddr);
    try std.testing.expect(reread_mapping.prot.write);

    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(source_vmo));
    try s.munmapRangeWithFreeList(p0, 0x4600_0000, 4096, &free_list);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(source_vmo));
}
