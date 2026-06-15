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

test "physical page allocation does not install page capability" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x1_0000_0000, 2);
    try std.testing.expectEqual(@as(usize, 0), s.getTableConst(p0).len);

    const page = try s.allocPhysicalPage(&free_list);
    try std.testing.expectEqual(@as(u64, 0x1_0000_0000), page.paddr);
    try std.testing.expectEqual(@as(usize, 0), s.getTableConst(p0).len);
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

    try s.munmapExact(p0, 0x4200_0000, 4096);
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
    try s.munmapExactWithFreeList(p0, 0x4400_0000, 8192, &free_list);
    try std.testing.expectEqual(original_free, free_list.len);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo));
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
    try std.testing.expectEqual(@as(usize, 0), s.getTableConst(p0).len);

    try std.testing.expect(s.nativeVmaFaultMapping(p0, 0x4500_0000, true, false) == null);
    try s.munmapExactWithFreeList(p0, 0x4500_0000, 4096, &free_list);
    try std.testing.expect(s.nativeVmaFaultMapping(p0, 0x4500_0000, false, false) == null);
}
