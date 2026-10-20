const std = @import("std");
const kernel = @import("kernel");

const KernelError = kernel.KernelError;
const KernelState = kernel.KernelState;
const PrincipalId = kernel.PrincipalId;
const FreePageList = kernel.FreePageList;

const p0: PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
const p1: PrincipalId = kernel.processPrincipalFromIndex(1) orelse unreachable;

var runtime_storage: [kernel.runtimeStorageBytes()]u8 align(4096) = undefined;

fn initState(state: *KernelState) !void {
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));
    try state.initFromDetectedRegionsInPlace(1);
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
    var objects = s.fd_objects.iterator();
    while (objects.next()) |item| if (item.value.kind != .none) {
        counts[0] += 1;
    };
    var replies = s.ipc_replies.iterator();
    while (replies.next()) |item| if (item.value.active) {
        counts[1] += 1;
    };
    return counts;
}

test "full ipc call preserves MOVE and resources until one successful retry" {
    var s: KernelState = undefined;
    try initState(&s);
    defer s.deinitPools();
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
        try std.testing.expectError(KernelError.MailboxFull, s.ipcCall(p0, pair.a, .{ .words = .{ 100, 0, 0, 0 }, .fds = &moved }, .{ .owner = p0 }, &free_list));
        try std.testing.expectEqual(resources, liveIpcResources(&s));
        try std.testing.expectEqual(free_fds, try s.fdFreeCountFrom(p0, 16));
        try std.testing.expectEqual(refs, s.kernelObjectRefCount(object));
        try std.testing.expectEqual(object, (s.fdEntryConst(p0, vmo) orelse unreachable).object);
    }
    const first = try s.ipcRecv(p0, pair.b, 0, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 0), first.words[0]);
    const reply = try s.ipcCall(p0, pair.a, .{ .words = .{ 100, 0, 0, 0 }, .fds = &moved }, .{ .owner = p0 }, &free_list);
    try std.testing.expect(s.fdEntryConst(p0, vmo) == null);
    for (1..kernel.max_ipc_queue_messages) |i| {
        const message = try s.ipcRecv(p0, pair.b, 0, 16, &free_list);
        try std.testing.expectEqual(@as(u64, i), message.words[0]);
    }
    const request = try s.ipcRecv(p0, pair.b, 2, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 100), request.words[0]);
    try std.testing.expectEqual(@as(usize, 1), request.fd_count);
    try std.testing.expectEqual(object, (s.fdEntryConst(p0, request.fds[0].fd) orelse unreachable).object);
    try std.testing.expectError(KernelError.MailboxEmpty, s.ipcRecv(p0, pair.b, 2, 16, &free_list));
    try std.testing.expectEqual(reply, request.call);
    s.releaseIpcCallAuthority(request.call);
    s.releaseIpcCallReference(reply);
    try s.closeFdWithFreeList(p0, request.fds[0].fd, &free_list);
    try s.closeFdWithFreeList(p0, pair.a, &free_list);
    try s.closeFdWithFreeList(p0, pair.b, &free_list);
    try std.testing.expectEqual([2]usize{ 0, 0 }, liveIpcResources(&s));
}

test "ipc call needs no descriptor and failed extraction preserves authority" {
    var s: KernelState = undefined;
    try initState(&s);
    defer s.deinitPools();
    var free_list = FreePageList{};
    const endpoint = try s.createIpcEndpointFd(p0, rights(.{ .call = true, .recv = true, .close = true }), .{}, 16);
    while (try s.fdFreeCountFrom(p0, 16) != 0)
        _ = try s.createEventFd(p0, 0, .{}, .{ .close = true }, 16);
    const resources = liveIpcResources(&s);
    for (0..32) |_| {
        const call = try s.ipcCall(p0, endpoint, .{}, .{ .owner = p0 }, &free_list);
        const request = try s.ipcRecv(p0, endpoint, 0, 16, &free_list);
        try std.testing.expectEqual(@as(usize, 0), request.fd_count);
        try std.testing.expectEqual(call, request.call);
        try std.testing.expectError(KernelError.TableFull, s.ipcExtractReply(p0, request.call, 16));
        try std.testing.expect(s.ipcReplySlot(call) != null);
        s.releaseIpcCallAuthority(request.call);
        s.releaseIpcCallReference(call);
        try std.testing.expectEqual(resources, liveIpcResources(&s));
    }
}

test "ipc receive capacity preserves queued FDs and validates empty transfers" {
    var s: KernelState = undefined;
    try initState(&s);
    defer s.deinitPools();
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

test "minimal fd ipc call moves vmo and extracts transferable reply authority" {
    var s: KernelState = undefined;
    try initState(&s);
    defer s.deinitPools();
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
        .{ .owner = p1 },
        &free_list,
    );
    try std.testing.expect(s.fdEntryConst(p1, client_vmo) == null);

    const request = try s.ipcRecv(p0, server_endpoint, 2, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 42), request.words[0]);
    try std.testing.expectEqual(@as(u64, 7), request.words[1]);
    try std.testing.expectEqual(@as(usize, 1), request.fd_count);

    const received_vmo = request.fds[0].fd;
    const server_reply = try s.ipcExtractReply(p0, request.call, 16);
    try std.testing.expectEqual(client_vmo_ref, s.nativeVmoRefForFd(p0, received_vmo) orelse unreachable);
    const received_entry = s.fdEntryConst(p0, received_vmo) orelse unreachable;
    try std.testing.expect(received_entry.rights.map_read);
    try std.testing.expect(!received_entry.rights.map_write);
    try std.testing.expect(received_entry.flags.cloexec);

    const reply_entry = s.fdEntryConst(p0, server_reply) orelse unreachable;
    try std.testing.expect(reply_entry.rights.send);
    try std.testing.expect(!reply_entry.rights.recv);

    try std.testing.expect(reply_entry.rights.transfer);
    const forwarded = try s.transferFd(p0, p1, server_reply, 16, reply_entry.rights, .{}, .move);
    try std.testing.expect(s.fdEntryConst(p0, server_reply) == null);
    try s.closeFdWithFreeList(p1, forwarded, &free_list);
    // The caller binding still pins the body after the last authority closes.
    try std.testing.expect(s.ipcReplySlot(client_reply) != null);
    s.releaseIpcCallReference(client_reply);
    try std.testing.expect(s.ipcReplySlot(client_reply) == null);
}

test "last receiver side close cancels queued call and releases MOVE attachments" {
    var s: KernelState = undefined;
    try initState(&s);
    defer s.deinitPools();
    var free_list = FreePageList{};
    const pair = try s.createIpcChannelPairFds(p0, rights(.{ .call = true, .send = true, .recv = true, .close = true }), .{}, 16);
    const attachment = try s.createAnonymousVmoFd(p0, 4096, rights(.{ .transfer = true, .close = true, .map_read = true }), .{}, 16);
    const object = s.fdEntryConst(p0, attachment).?.object;
    const moved = [_]kernel.IpcSendFd{.{ .fd = attachment, .rights = rights(.{ .close = true, .map_read = true }), .move = true }};
    const ref = try s.ipcCall(p0, pair.a, .{ .fds = &moved }, .{ .owner = p0 }, &free_list);
    try std.testing.expect(s.fdEntryConst(p0, attachment) == null);
    try std.testing.expectEqual(@as(usize, 2), s.ipcReplySlot(ref).?.lifetime_refs.load(.acquire));
    try s.closeFdWithFreeList(p0, pair.b, &free_list);
    try std.testing.expect(s.ipcReplySlot(ref).?.sent);
    try std.testing.expectEqual(@as(usize, 1), s.ipcReplySlot(ref).?.lifetime_refs.load(.acquire));
    try std.testing.expect(s.kernelObjectSlotConst(object) == null);
    s.releaseIpcCallReference(ref);
    try s.closeFdWithFreeList(p0, pair.a, &free_list);
    try std.testing.expectEqual([2]usize{ 0, 0 }, liveIpcResources(&s));
}

test "cancel from full channel publishes the waiting sender writable edge" {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    var s: KernelState = undefined;
    try initState(&s);
    defer s.deinitPools();
    var free_list = FreePageList{};
    const pair = try s.createIpcChannelPairFds(p0, rights(.{ .call = true, .send = true, .recv = true, .poll = true, .wait = true, .close = true }), .{}, 16);
    const ref = try s.ipcCall(p0, pair.a, .{}, .{ .owner = p0 }, &free_list);
    for (1..kernel.max_ipc_queue_messages) |_| try s.ipcSend(p0, pair.a, .{}, &free_list);
    const group = try s.beginFdWaitGroup(p0, 7, 11, 41);
    try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, pair.a, fd_abi.event_writable, 0x1000, 7, 11, 41, group));
    try s.armFdWaitGroup(group);
    var targets: [2]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), s.takeIpcCancellationWritableWaiters(&targets));
    s.ipcCancelCall(ref, &free_list);
    s.releaseIpcCallReference(ref);
    try std.testing.expectEqual(@as(usize, 1), s.takeIpcCancellationWritableWaiters(&targets));
    try std.testing.expectEqual(@as(u64, fd_abi.event_writable), targets[0].revents);
    try std.testing.expectEqual(@as(u64, 41), targets[0].wait_token);
    s.cancelFdWaitGroup(group, 41);
    try std.testing.expectEqual(@as(usize, 0), s.takeIpcCancellationWritableWaiters(&targets));
    try std.testing.expect(!s.ipc_cancel_writable_pending);
    try s.closeFdWithFreeList(p0, pair.a, &free_list);
    try s.closeFdWithFreeList(p0, pair.b, &free_list);
}

test "cancelled caller implicit authority can still be extracted and closed" {
    var s: KernelState = undefined;
    try initState(&s);
    defer s.deinitPools();
    var free_list = FreePageList{};
    const endpoint = try s.createIpcEndpointFd(p0, KernelState.endpointRights(), .{}, 16);
    const ref = try s.ipcCall(p0, endpoint, .{}, .{ .owner = p0 }, &free_list);
    const request = try s.ipcRecv(p0, endpoint, 0, 16, &free_list);
    try std.testing.expectEqual(ref, request.call);
    s.ipcCancelCall(ref, &free_list);
    const extracted = try s.ipcExtractReply(p0, request.call, 16);
    try std.testing.expect(s.ipcReplySlot(ref).?.sent);
    try std.testing.expect(!s.ipcClaimCallReply(ref));
    try s.closeFdWithFreeList(p0, extracted, &free_list);
    s.releaseIpcCallReference(ref);
    try std.testing.expect(s.ipcReplySlot(ref) == null);
    try s.closeFdWithFreeList(p0, endpoint, &free_list);
    try std.testing.expectEqual([2]usize{ 0, 0 }, liveIpcResources(&s));
}
