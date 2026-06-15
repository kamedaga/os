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

fn initFdState() !KernelState {
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
