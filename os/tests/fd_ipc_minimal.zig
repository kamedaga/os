const std = @import("std");
const kernel = @import("kernel");

const KernelError = kernel.KernelError;
const KernelState = kernel.KernelState;
const PrincipalId = kernel.PrincipalId;

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

test "minimal fd ipc call moves vmo fd and receives reply" {
    var s = try initState();

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
    );
    try std.testing.expect(s.fdEntryConst(p1, client_vmo) == null);

    const request = try s.ipcRecv(p0, server_endpoint, 2, 16);
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

    try s.ipcReply(p0, server_reply, .{ .words = .{ 99, 0, 0, 0 } });
    try std.testing.expectError(KernelError.InvalidState, s.ipcReply(p0, server_reply, .{}));

    const reply = try s.ipcRecv(p1, client_reply, 0, 16);
    try std.testing.expectEqual(@as(u64, 99), reply.words[0]);
    try std.testing.expectError(KernelError.MailboxEmpty, s.ipcRecv(p1, client_reply, 0, 16));
}
