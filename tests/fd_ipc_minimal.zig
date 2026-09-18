const std = @import("std");
const kernel = @import("kernel");

const KernelError = kernel.KernelError;
const KernelState = kernel.KernelState;
const PrincipalId = kernel.PrincipalId;
const FreePageList = kernel.FreePageList;

const p0: PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
const p1: PrincipalId = kernel.processPrincipalFromIndex(1) orelse unreachable;

var runtime_storage: [kernel.runtimeStorageBytes()]u8 align(4096) = undefined;

fn initState() !KernelState {
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));
    return KernelState.initFromDetectedRegions(1);
}

fn rights(comptime fields: anytype) kernel.FdRights {
    var value = kernel.FdRights{};
    inline for (std.meta.fields(@TypeOf(fields))) |field| {
        @field(value, field.name) = @field(fields, field.name);
    }
    return value;
}

fn liveIpcResources(s: *const KernelState) [2]usize {
    var counts: [2]usize = .{ 0, 0 };
    for (&s.fd_objects) |slot| if (slot.kind != .none) {
        counts[0] += 1;
    };
    for (&s.ipc_replies) |slot| if (slot.active) {
        counts[1] += 1;
    };
    return counts;
}

test "full ipc call preserves MOVE and resources until one successful retry" {
    var s = try initState();
    var free_list = FreePageList{};
    const pair = try s.createIpcChannelPairFds(p0, rights(.{ .call = true, .send = true, .recv = true, .close = true }), .{}, 16);
    const vmo = try s.createAnonymousVmoFd(p0, 4096, rights(.{ .transfer = true, .close = true, .map_read = true }), .{}, 16);
    const object = (s.fdEntryConst(p0, vmo) orelse unreachable).object;
    const moved = [_]kernel.IpcSendFd{.{ .fd = vmo, .rights = rights(.{ .close = true, .map_read = true }), .move = true }};
    for (0..kernel.max_ipc_queue_messages) |i|
        try s.ipcSend(p0, pair.a, .{ .words = .{ i, 0, 0, 0 } }, &free_list);
    const resources = liveIpcResources(&s);
    const free_fds = try s.fdFreeCountFrom(p0, 16);
    const refs = s.kernelObjectRefCount(object);
    for (0..128) |_| {
        try std.testing.expectError(KernelError.MailboxFull, s.ipcCall(p0, pair.a, .{ .words = .{ 100, 0, 0, 0 }, .fds = &moved }, 16, &free_list));
        try std.testing.expectEqual(resources, liveIpcResources(&s));
        try std.testing.expectEqual(free_fds, try s.fdFreeCountFrom(p0, 16));
        try std.testing.expectEqual(refs, s.kernelObjectRefCount(object));
        try std.testing.expectEqual(object, (s.fdEntryConst(p0, vmo) orelse unreachable).object);
    }
    const first = try s.ipcRecv(p0, pair.b, 0, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 0), first.words[0]);
    const reply = try s.ipcCall(p0, pair.a, .{ .words = .{ 100, 0, 0, 0 }, .fds = &moved }, 16, &free_list);
    try std.testing.expect(s.fdEntryConst(p0, vmo) == null);
    for (1..kernel.max_ipc_queue_messages) |i| {
        const message = try s.ipcRecv(p0, pair.b, 0, 16, &free_list);
        try std.testing.expectEqual(@as(u64, i), message.words[0]);
    }
    const request = try s.ipcRecv(p0, pair.b, 2, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 100), request.words[0]);
    try std.testing.expectEqual(@as(usize, 2), request.fd_count);
    try std.testing.expectEqual(object, (s.fdEntryConst(p0, request.fds[0].fd) orelse unreachable).object);
    try std.testing.expectError(KernelError.MailboxEmpty, s.ipcRecv(p0, pair.b, 2, 16, &free_list));
    try s.ipcReply(p0, request.fds[1].fd, .{ .words = .{ 101, 0, 0, 0 } }, &free_list);
    const response = try s.ipcRecv(p0, reply, 0, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 101), response.words[0]);
    try s.closeFdWithFreeList(p0, reply, &free_list);
    try s.closeFdWithFreeList(p0, request.fds[1].fd, &free_list);
    try s.closeFdWithFreeList(p0, request.fds[0].fd, &free_list);
    try s.closeFdWithFreeList(p0, pair.a, &free_list);
    try s.closeFdWithFreeList(p0, pair.b, &free_list);
    try std.testing.expectEqual([2]usize{ 0, 0 }, liveIpcResources(&s));
}

test "ipc call descriptor exhaustion remains allocation failure" {
    var s = try initState();
    var free_list = FreePageList{};
    const endpoint = try s.createIpcEndpointFd(p0, rights(.{ .call = true, .recv = true, .close = true }), .{}, 16);
    while (try s.fdFreeCountFrom(p0, 16) != 0)
        _ = try s.createEventFd(p0, 0, .{}, .{ .close = true }, 16);
    const resources = liveIpcResources(&s);
    for (0..32) |_| {
        try std.testing.expectError(KernelError.TableFull, s.ipcCall(p0, endpoint, .{}, 16, &free_list));
        try std.testing.expectEqual(resources, liveIpcResources(&s));
        try std.testing.expectError(KernelError.MailboxEmpty, s.ipcRecv(p0, endpoint, 0, 16, &free_list));
    }
}

test "ipc receive capacity preserves queued FDs and validates empty transfers" {
    var s = try initState();
    var free_list = FreePageList{};
    const pair = try s.createIpcChannelPairFds(p0, rights(.{ .send = true, .recv = true, .close = true }), .{}, 16);
    const vmo = try s.createAnonymousVmoFd(p0, 4096, rights(.{ .transfer = true, .close = true, .map_read = true }), .{}, 16);
    const object = (s.fdEntryConst(p0, vmo) orelse unreachable).object;
    const transfer = [_]kernel.IpcSendFd{.{ .fd = vmo, .rights = rights(.{ .close = true, .map_read = true }), .move = false }};
    var last: kernel.Fd = undefined;
    while (try s.fdFreeCountFrom(p0, 16) != 0)
        last = try s.createEventFd(p0, 0, .{}, .{ .close = true }, 16);

    // A full receiver can still receive words, but an invalid minimum is an
    // error even without attached FDs and must leave the message queued.
    try s.ipcSend(p0, pair.a, .{ .words = .{ 42, 0, 0, 0 } }, &free_list);
    try std.testing.expectError(KernelError.InvalidState, s.ipcRecv(p0, pair.b, 0, std.math.maxInt(kernel.Fd), &free_list));
    const words = try s.ipcRecv(p0, pair.b, 0, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 42), words.words[0]);
    try std.testing.expectEqual(@as(usize, 0), words.fd_count);

    try s.ipcSend(p0, pair.a, .{ .fds = &transfer }, &free_list);
    const refs = s.kernelObjectRefCount(object);
    try std.testing.expectError(KernelError.TableFull, s.ipcRecv(p0, pair.b, 1, 16, &free_list));
    try std.testing.expectEqual(refs, s.kernelObjectRefCount(object));
    try s.closeFdWithFreeList(p0, last, &free_list);
    // Exactly one free slot, at the end of the table, must be sufficient.
    const received = try s.ipcRecv(p0, pair.b, 1, 16, &free_list);
    try std.testing.expectEqual(last, received.fds[0].fd);
    try std.testing.expectEqual(object, (s.fdEntryConst(p0, last) orelse unreachable).object);
    try std.testing.expectEqual(refs, s.kernelObjectRefCount(object));
    try std.testing.expectError(KernelError.MailboxEmpty, s.ipcRecv(p0, pair.b, 1, 16, &free_list));
}

test "minimal fd ipc call moves vmo fd and receives reply" {
    var s = try initState();
    var free_list = FreePageList{};

    const server_endpoint = try s.createIpcEndpointFd(
        p0,
        rights(.{ .recv = true, .call = true, .transfer = true, .close = true }),
        .{},
        16,
    );
    const client_endpoint = try s.transferFd(
        p0,
        p1,
        server_endpoint,
        16,
        rights(.{ .call = true, .close = true }),
        .{},
        .copy,
    );

    const client_vmo = try s.createAnonymousVmoFd(
        p1,
        4096,
        rights(.{ .transfer = true, .map_read = true, .close = true }),
        .{},
        16,
    );
    const client_vmo_ref = s.nativeVmoRefForFd(p1, client_vmo) orelse unreachable;
    const request_fds = [_]kernel.IpcSendFd{.{
        .fd = client_vmo,
        .rights = rights(.{ .map_read = true, .close = true }),
        .flags = .{ .cloexec = true },
        .move = true,
    }};

    const client_reply = try s.ipcCall(
        p1,
        client_endpoint,
        .{ .words = .{ 42, 7, 0, 0 }, .fds = request_fds[0..] },
        16,
        &free_list,
    );
    try std.testing.expect(s.fdEntryConst(p1, client_vmo) == null);

    const request = try s.ipcRecv(p0, server_endpoint, 2, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 42), request.words[0]);
    try std.testing.expectEqual(@as(u64, 7), request.words[1]);
    try std.testing.expectEqual(@as(usize, 2), request.fd_count);

    const received_vmo = request.fds[0].fd;
    const server_reply = request.fds[1].fd;
    try std.testing.expectEqual(client_vmo_ref, s.nativeVmoRefForFd(p0, received_vmo) orelse unreachable);
    const received_entry = s.fdEntryConst(p0, received_vmo) orelse unreachable;
    try std.testing.expect(received_entry.rights.map_read);
    try std.testing.expect(!received_entry.rights.map_write);
    try std.testing.expect(received_entry.flags.cloexec);

    const reply_entry = s.fdEntryConst(p0, server_reply) orelse unreachable;
    try std.testing.expect(reply_entry.rights.send);
    try std.testing.expect(!reply_entry.rights.recv);

    try s.ipcReply(p0, server_reply, .{ .words = .{ 99, 0, 0, 0 } }, &free_list);
    try std.testing.expectError(KernelError.InvalidState, s.ipcReply(p0, server_reply, .{}, &free_list));

    const reply = try s.ipcRecv(p1, client_reply, 0, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 99), reply.words[0]);
    try std.testing.expectError(KernelError.MailboxEmpty, s.ipcRecv(p1, client_reply, 0, 16, &free_list));
}
