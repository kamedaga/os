const std = @import("std");
const kernel = @import("kernel");
const fd_abi = @import("kernel_abi_root").fd_abi;
const ProcessFdGrant = @import("kernel_abi_root").process_abi.ProcessFdGrant;

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
    // Avoid stacking two by-value initialization helpers for this large state.
    var state: KernelState = undefined;
    try state.initFromDetectedRegionsInPlace(1);
    return state;
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

fn writeLe16(bytes: []u8, offset: usize, value: u16) void {
    bytes[offset] = @truncate(value);
    bytes[offset + 1] = @truncate(value >> 8);
}

fn writeLe32(bytes: []u8, offset: usize, value: u32) void {
    bytes[offset] = @truncate(value);
    bytes[offset + 1] = @truncate(value >> 8);
    bytes[offset + 2] = @truncate(value >> 16);
    bytes[offset + 3] = @truncate(value >> 24);
}

fn writeLe64(bytes: []u8, offset: usize, value: u64) void {
    writeLe32(bytes, offset, @truncate(value));
    writeLe32(bytes, offset + 4, @truncate(value >> 32));
}

fn initDmarFixture(bytes: []u8, host_address_width: u8, flags: u8) void {
    @memset(bytes, 0);
    @memcpy(bytes[0..4], "DMAR");
    writeLe32(bytes, 4, @intCast(bytes.len));
    bytes[8] = 1;
    bytes[36] = host_address_width;
    bytes[37] = flags;
}

fn writeDmarStructureHeader(bytes: []u8, offset: usize, structure_type: u16, length: u16) void {
    writeLe16(bytes, offset, structure_type);
    writeLe16(bytes, offset + 2, length);
}

fn writeDrhdWithLength(bytes: []u8, offset: usize, length: u16, flags: u8, segment: u16, register_base: u64) void {
    writeDmarStructureHeader(bytes, offset, 0, length);
    bytes[offset + 4] = flags;
    writeLe16(bytes, offset + 6, segment);
    writeLe64(bytes, offset + 8, register_base);
}

fn writeDrhd(bytes: []u8, offset: usize, flags: u8, segment: u16, register_base: u64) void {
    writeDrhdWithLength(bytes, offset, 16, flags, segment, register_base);
}

fn writeDeviceScope(
    bytes: []u8,
    offset: usize,
    scope_type: u8,
    enumeration_id: u8,
    start_bus: u8,
    path: []const kernel.acpi_dmar.DevicePath,
) void {
    bytes[offset] = scope_type;
    bytes[offset + 1] = @intCast(6 + path.len * 2);
    bytes[offset + 4] = enumeration_id;
    bytes[offset + 5] = start_bus;
    for (path, 0..) |entry, index| {
        bytes[offset + 6 + index * 2] = entry.device;
        bytes[offset + 7 + index * 2] = entry.function;
    }
}

fn finishAcpiChecksum(bytes: []u8) void {
    bytes[9] = 0;
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    bytes[9] = 0 -% sum;
}

test "DMAR RMRR clips continuous DMA window and rejects malformed reservations" {
    var fixture: [48 + 16 + 32]u8 = undefined;
    initDmarFixture(&fixture, 47, 0);
    writeDrhd(&fixture, 48, 1, 0, 0xfed90000);
    writeDmarStructureHeader(&fixture, 64, 1, 32);
    writeLe64(&fixture, 72, 0xa0000000);
    writeLe64(&fixture, 80, 0xa000ffff);
    writeDeviceScope(&fixture, 88, 1, 0, 0, &.{.{ .device = 2 }});
    finishAcpiChecksum(&fixture);
    try std.testing.expectEqual(@as(u64, 0xa0000000), (try kernel.acpi_dmar.parseDmar(&fixture)).dma_window_end);
    writeLe64(&fixture, 72, 0x7ffff000);
    writeLe64(&fixture, 80, 0x80000fff);
    finishAcpiChecksum(&fixture);
    try std.testing.expectEqual(@as(u64, 0x80000000), (try kernel.acpi_dmar.parseDmar(&fixture)).dma_window_end);
    writeLe64(&fixture, 80, 0x80000000);
    finishAcpiChecksum(&fixture);
    try std.testing.expectError(error.InvalidReservedRange, kernel.acpi_dmar.parseDmar(&fixture));
    writeLe64(&fixture, 80, 0x80000fff);
    fixture[89] = 7;
    finishAcpiChecksum(&fixture);
    try std.testing.expectError(error.InvalidDeviceScopeLength, kernel.acpi_dmar.parseDmar(&fixture));
}

test "DMAR parser accepts one DRHD" {
    var fixture = [_]u8{0} ** 64;
    initDmarFixture(fixture[0..], 47, 0x1);
    writeDrhd(fixture[0..], 48, 0x1, 3, 0xfed9_0000);
    finishAcpiChecksum(fixture[0..]);

    const parsed = try kernel.acpi_dmar.parseDmar(fixture[0..]);
    try std.testing.expectEqual(@as(u8, 47), parsed.host_address_width);
    try std.testing.expectEqual(@as(u16, 48), parsed.hostAddressWidthBits());
    try std.testing.expectEqual(@as(u8, 0x1), parsed.flags);
    try std.testing.expectEqual(@as(usize, 1), parsed.drhd_count);
    try std.testing.expectEqual(@as(u64, 0xfed9_0000), parsed.drhds[0].register_base);
    try std.testing.expectEqual(@as(u16, 3), parsed.drhds[0].segment);
    try std.testing.expect(parsed.drhds[0].include_pci_all);
}

test "DMAR parser retains multiple DRHD structures" {
    var fixture = [_]u8{0} ** 80;
    initDmarFixture(fixture[0..], 38, 0);
    writeDrhd(fixture[0..], 48, 0, 0, 0xfed9_0000);
    writeDrhd(fixture[0..], 64, 1, 2, 0xfeda_0000);
    finishAcpiChecksum(fixture[0..]);

    const parsed = try kernel.acpi_dmar.parseDmar(fixture[0..]);
    try std.testing.expectEqual(@as(usize, 2), parsed.drhd_count);
    try std.testing.expect(!parsed.drhds[0].include_pci_all);
    try std.testing.expectEqual(@as(u16, 2), parsed.drhds[1].segment);
    try std.testing.expectEqual(@as(u64, 0xfeda_0000), parsed.drhds[1].register_base);
    try std.testing.expect(parsed.drhds[1].include_pci_all);
}

test "DMAR parser retains DRHD device scopes and paths" {
    var fixture = [_]u8{0} ** 74;
    initDmarFixture(fixture[0..], 47, 0);
    writeDrhdWithLength(fixture[0..], 48, 26, 0, 0, 0xfed9_0000);
    const path = [_]kernel.acpi_dmar.DevicePath{
        .{ .device = 2, .function = 0 },
        .{ .device = 3, .function = 1 },
    };
    writeDeviceScope(fixture[0..], 64, 1, 7, 0, path[0..]);
    finishAcpiChecksum(fixture[0..]);

    const parsed = try kernel.acpi_dmar.parseDmar(fixture[0..]);
    try std.testing.expectEqual(@as(u16, 26), parsed.drhds[0].length);
    try std.testing.expectEqual(@as(usize, 1), parsed.drhds[0].scope_count);
    const scope = parsed.drhds[0].scopes[0];
    try std.testing.expectEqual(@as(u8, 1), scope.scope_type);
    try std.testing.expectEqual(@as(u8, 7), scope.enumeration_id);
    try std.testing.expectEqual(@as(usize, 2), scope.path_count);
    try std.testing.expectEqual(@as(u8, 3), scope.path[1].device);
    try std.testing.expectEqual(@as(u8, 1), scope.path[1].function);
}

test "DMAR parser rejects malformed DRHD device scope" {
    var fixture = [_]u8{0} ** 72;
    initDmarFixture(fixture[0..], 47, 0);
    writeDrhdWithLength(fixture[0..], 48, 24, 0, 0, 0xfed9_0000);
    fixture[64] = 1;
    fixture[65] = 7;
    finishAcpiChecksum(fixture[0..]);
    try std.testing.expectError(
        error.InvalidDeviceScopeLength,
        kernel.acpi_dmar.parseDmar(fixture[0..]),
    );
}

test "DMAR parser skips unknown remapping structure types" {
    var fixture = [_]u8{0} ** 72;
    initDmarFixture(fixture[0..], 38, 0);
    writeDmarStructureHeader(fixture[0..], 48, 0x7fff, 8);
    fixture[52] = 0xaa;
    writeDrhd(fixture[0..], 56, 1, 0, 0xfed9_0000);
    finishAcpiChecksum(fixture[0..]);

    const parsed = try kernel.acpi_dmar.parseDmar(fixture[0..]);
    try std.testing.expectEqual(@as(usize, 1), parsed.drhd_count);
    try std.testing.expectEqual(@as(u64, 0xfed9_0000), parsed.drhds[0].register_base);
}

test "DMAR parser rejects invalid and out-of-bounds structure lengths" {
    var zero_length = [_]u8{0} ** 52;
    initDmarFixture(zero_length[0..], 38, 0);
    writeDmarStructureHeader(zero_length[0..], 48, 0, 0);
    finishAcpiChecksum(zero_length[0..]);
    try std.testing.expectError(
        error.InvalidStructureLength,
        kernel.acpi_dmar.parseDmar(zero_length[0..]),
    );

    var out_of_bounds = [_]u8{0} ** 56;
    initDmarFixture(out_of_bounds[0..], 38, 0);
    writeDmarStructureHeader(out_of_bounds[0..], 48, 0, 16);
    finishAcpiChecksum(out_of_bounds[0..]);
    try std.testing.expectError(
        error.StructureOutOfBounds,
        kernel.acpi_dmar.parseDmar(out_of_bounds[0..]),
    );
}

test "DMAR parser rejects checksum mismatch" {
    var fixture = [_]u8{0} ** 64;
    initDmarFixture(fixture[0..], 38, 0);
    writeDrhd(fixture[0..], 48, 1, 0, 0xfed9_0000);
    finishAcpiChecksum(fixture[0..]);
    fixture[38] +%= 1;

    try std.testing.expectError(
        error.ChecksumMismatch,
        kernel.acpi_dmar.parseDmar(fixture[0..]),
    );
}

test "VT-d table builders encode legacy root context and second-level entries" {
    var root = [_]u64{0xffff_ffff_ffff_ffff} ** 512;
    kernel.vtd_tables.clear(&root);
    try std.testing.expect(kernel.vtd_tables.setRootEntry(&root, 0x2a, 0x1234_5000));
    try std.testing.expectEqual(@as(u64, 0x1234_5001), root[0x2a * 2]);
    try std.testing.expectEqual(@as(u64, 0), root[0x2a * 2 + 1]);

    var context = [_]u64{0} ** 512;
    try std.testing.expect(kernel.vtd_tables.setContextEntry(&context, 0x9b, 0x2345_6000, 0x1357));
    try std.testing.expectEqual(@as(u64, 0x2345_6001), context[0x9b * 2]);
    try std.testing.expectEqual(@as(u64, 0x0013_5702), context[0x9b * 2 + 1]);

    var second_level = [_]u64{0} ** 512;
    try std.testing.expect(kernel.vtd_tables.setSecondLevelEntry(&second_level, 17, 0x3456_7000, true, true));
    try std.testing.expectEqual(@as(u64, 0x3456_7003), second_level[17]);
    try std.testing.expect(kernel.vtd_tables.setSecondLevelEntry(&second_level, 18, 0x4567_8000, true, false));
    try std.testing.expectEqual(@as(u64, 0x4567_8001), second_level[18]);
}

test "VT-d table builders reject unaligned and out-of-range inputs" {
    var page = [_]u64{0} ** 512;
    try std.testing.expect(!kernel.vtd_tables.setRootEntry(&page, 0, 0x1234_5001));
    try std.testing.expect(!kernel.vtd_tables.setContextEntry(&page, 0, 0x0010_0000_0000_0000, 1));
    try std.testing.expect(kernel.vtd_tables.setContextEntry(&page, 0, 0x1000, 0));
    try std.testing.expectEqual(@as(u64, 0x2), page[1]);
    try std.testing.expect(!kernel.vtd_tables.setSecondLevelEntry(&page, page.len, 0x1000, true, true));
}

test "VT-d IOVA allocator allocates frees exhausts and wraps its cursor" {
    const t = kernel.vtd_tables;
    var storage = t.IovaTestStorage{};
    const page_count = 65536;
    const end = t.iova_window_start + page_count * t.page_size;
    var allocator = t.IovaAllocator.init(t.iova_window_start, end, storage.storage());
    defer storage.cleanup(&allocator);

    try std.testing.expectEqual(t.iova_window_start, allocator.alloc(2).?);
    try std.testing.expectEqual(t.iova_window_start + 2 * t.page_size, allocator.alloc(3).?);
    try std.testing.expect(allocator.free(t.iova_window_start, 2));
    try std.testing.expectEqual(t.iova_window_start + 5 * t.page_size, allocator.alloc(2).?);

    storage.cleanup(&allocator);
    allocator = t.IovaAllocator.init(t.iova_window_start, end, storage.storage());
    try std.testing.expectEqual(t.iova_window_start, allocator.alloc(page_count - 1).?);
    try std.testing.expectEqual(end - t.page_size, allocator.alloc(1).?);
    try std.testing.expect(allocator.alloc(1) == null);
    try std.testing.expect(allocator.free(t.iova_window_start, 1));
    try std.testing.expectEqual(t.iova_window_start, allocator.alloc(1).?);
    try std.testing.expectEqual(page_count, allocator.peak_pages);

    try std.testing.expect(allocator.free(t.iova_window_start, page_count));
    try std.testing.expectEqual(@as(usize, 0), allocator.used_pages);
    try std.testing.expectEqual(t.iova_window_start, allocator.alloc(page_count).?);
    try std.testing.expect(allocator.alloc(1) == null);
}

test "VT-d device domains isolate mappings and reuse teardown IOVA" {
    const t = kernel.vtd_tables;
    var context = [_]u64{0} ** t.entries_per_page;
    var domain_roots: [2]t.TablePage align(4096) = undefined;
    t.clear(&domain_roots[0]);
    t.clear(&domain_roots[1]);
    var domain_a_leaf = [_]u64{0} ** t.entries_per_page;
    const domain_b_leaf = [_]u64{0} ** t.entries_per_page;
    try std.testing.expect(t.setContextEntry(&context, 0x18, @intFromPtr(&domain_roots[0]), 1));
    try std.testing.expect(t.setContextEntry(&context, 0x20, @intFromPtr(&domain_roots[1]), 2));
    try std.testing.expect(context[0x18 * 2] != context[0x20 * 2]);

    const leaf_index: usize = @intCast((t.iova_window_start >> 12) & 0x1ff);
    try std.testing.expect(t.setSecondLevelEntry(&domain_a_leaf, leaf_index, 0x1234_5000, true, true));
    try std.testing.expectEqual(@as(u64, 0x1234_5003), domain_a_leaf[leaf_index]);
    try std.testing.expectEqual(@as(u64, 0), domain_b_leaf[leaf_index]);

    var storage = t.IovaTestStorage{};
    const page_count = 65536;
    const end = t.iova_window_start + page_count * t.page_size;
    var allocator = t.IovaAllocator.init(t.iova_window_start, end, storage.storage());
    defer storage.cleanup(&allocator);
    const iova = allocator.alloc(1).?;
    domain_a_leaf[leaf_index] = 0;
    try std.testing.expect(allocator.free(iova, 1));
    try std.testing.expectEqual(iova + t.page_size, allocator.alloc(page_count - 1).?);
    try std.testing.expectEqual(iova, allocator.alloc(1).?);
}

test "process descriptor capacity limit does not exceed address-space storage" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x20_0000, 16);
    const limit = kernel.initial_process_count + 2;

    const first = s.createProcessDescriptorWithCapacityLimit("limited-0", &free_list, limit) orelse unreachable;
    const second = s.createProcessDescriptorWithCapacityLimit("limited-1", &free_list, limit) orelse unreachable;
    try std.testing.expect(@intFromEnum(first) < limit);
    try std.testing.expect(@intFromEnum(second) < limit);
    try std.testing.expect(s.createProcessDescriptorWithCapacityLimit("over-limit", &free_list, limit) == null);
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

test "pinned overlap exception is exact and other pins still reject" {
    var s = try initFdState();
    const base: u64 = 0x4000_0000;
    s.next_fd_object_scan = kernel.max_fd_objects - 1;
    const source_fd = try s.createDmaBufferFd(p0, .{
        .device = 1,
        .user_va = base + 0x80,
        .iova = 0x8000_0080,
        .size = 64,
    }, fdRights(.{ .close = true, .dup = true }), .{}, 16);
    const source_ref = (s.fdEntryConst(p0, source_fd) orelse unreachable).object;

    try std.testing.expect(s.rangeOverlapsPinnedUserObject(p0, base, 4096));
    try std.testing.expect(!s.rangeOverlapsPinnedUserObjectExcept(p0, base, 4096, source_ref));

    var stale_ref = source_ref;
    stale_ref.generation = KernelState.nextObjectGeneration(stale_ref.generation);
    try std.testing.expect(s.rangeOverlapsPinnedUserObjectExcept(p0, base, 4096, stale_ref));

    _ = try s.createDmaMappingFd(p0, .{
        .device = 1,
        .user_va = base + 0x200,
        .iova = 0x9000_0200,
        .size = 64,
        .direction = .to_device,
    }, fdRights(.{ .close = true }), .{}, 16);
    try std.testing.expect(s.rangeOverlapsPinnedUserObjectExcept(p0, base, 4096, source_ref));

    const alias = try s.dupFd(p0, source_fd, 16, fdRights(.{ .close = true }), .{});
    try s.closeFd(p0, source_fd);
    try std.testing.expect(s.pinned_object_slots.isSet(source_ref.index));
    var free_list = FreePageList{};
    try s.closeFdWithFreeList(p0, alias, &free_list);
    try std.testing.expect(!s.pinned_object_slots.isSet(source_ref.index));
    try std.testing.expect(s.rangeOverlapsPinnedUserObject(p0, base, 4096));
    s.next_fd_object_scan = source_ref.index;
    const reused = try createTestFdObject(&s, 22);
    try std.testing.expectEqual(source_ref.index, reused.index);
    try std.testing.expect(reused.generation != source_ref.generation);
    try std.testing.expect(!s.pinned_object_slots.isSet(reused.index));
    s.resetKernelObjectTable();
    try std.testing.expectEqual(@as(usize, 0), s.pinned_object_slots.count());
    try std.testing.expect(!s.rangeOverlapsPinnedUserObject(p0, base, 4096));
    // An unpublished object is indexed, but ref_count=0 is not a live pin.
    const pending = try s.createKernelObject(.dma_mapping, .{ .dma_mapping = .{
        .owner_principal_raw = @intFromEnum(p0),
        .user_va = base,
        .size = 64,
    } });
    try std.testing.expect(s.pinned_object_slots.isSet(pending.index));
    try std.testing.expect(!s.rangeOverlapsPinnedUserObject(p0, base, 4096));
    s.clearKernelObjectSlot(s.kernelObjectSlot(pending).?);
    try std.testing.expectEqual(@as(usize, 0), s.pinned_object_slots.count());
}

test "DMA derivation aliases DMA pins only when explicitly allowed" {
    var s = try initFdState();
    const base: u64 = 0x4080_0000;
    const source_fd = try s.createDmaBufferFd(p0, .{
        .device = 1,
        .user_va = base + 0x80,
        .iova = 0x8080_0080,
        .size = 64,
    }, fdRights(.{ .close = true }), .{}, 16);
    const source_ref = (s.fdEntryConst(p0, source_fd) orelse unreachable).object;

    try std.testing.expect(s.rangeConflictsWithDmaDerivation(p0, base, 4096, null, true, false));
    try std.testing.expect(s.rangeConflictsWithDmaDerivation(p0, base, 4096, null, false, false));
    try std.testing.expect(!s.rangeConflictsWithDmaDerivation(p0, base, 4096, source_ref, false, false));

    var stale_ref = source_ref;
    stale_ref.generation = KernelState.nextObjectGeneration(stale_ref.generation);
    try std.testing.expect(s.rangeConflictsWithDmaDerivation(p0, base, 4096, stale_ref, false, false));

    const mapping_base: u64 = base + 0x8_0000;
    const first_mapping_fd = try s.createDmaMappingFd(p0, .{
        .device = 1,
        .user_va = mapping_base + 0x80,
        .iova = 0x9088_0080,
        .size = 64,
        .direction = .from_device,
    }, fdRights(.{ .close = true }), .{}, 16);
    try std.testing.expect(!s.rangeConflictsWithDmaDerivation(p0, mapping_base, 4096, null, true, false));
    try std.testing.expect(s.rangeConflictsWithDmaDerivation(p0, mapping_base, 4096, null, false, false));

    const scatter_base: u64 = base + 0xc_0000;
    var scatter_free_list = FreePageList{};
    const scatter_fd = try s.createDmaMappingFd(p0, .{
        .device = 1,
        .user_va = scatter_base + 0x80,
        .iova = 0x908c_0080,
        .size = 64,
        .page_count = 1,
        .direction = .from_device,
    }, fdRights(.{ .close = true }), .{}, 16);
    try std.testing.expect(s.rangeConflictsWithDmaDerivation(p0, scatter_base, 4096, null, true, false));

    const second_mapping_fd = try s.createDmaMappingFd(p0, .{
        .device = 1,
        .user_va = mapping_base + 0x300,
        .iova = 0x9088_0300,
        .size = 64,
        .direction = .to_device,
    }, fdRights(.{ .close = true }), .{}, 16);
    try std.testing.expect(s.rangeOverlapsPinnedUserObject(p0, mapping_base, 4096));
    try s.closeFd(p0, first_mapping_fd);
    try std.testing.expect(s.rangeOverlapsPinnedUserObject(p0, mapping_base, 4096));
    try s.closeFd(p0, second_mapping_fd);
    try std.testing.expect(!s.rangeOverlapsPinnedUserObject(p0, mapping_base, 4096));
    try s.closeFdWithFreeList(p0, scatter_fd, &scatter_free_list);
    try std.testing.expectEqual(@as(usize, 0), scatter_free_list.len);

    const mmio_base: u64 = base + 0x10_0000;
    const mmio_fd = try s.createMmioRegionFd(p0, .{
        .device = 1,
        .bar_index = 0,
        .paddr = 0xa000_0000,
        .user_va = mmio_base,
        .size = 4096,
    }, fdRights(.{ .close = true }), .{}, 16);
    const mmio_ref = (s.fdEntryConst(p0, mmio_fd) orelse unreachable).object;
    try std.testing.expect(s.rangeConflictsWithDmaDerivation(p0, mmio_base, 4096, null, true, false));
    try std.testing.expect(s.rangeConflictsWithDmaDerivation(p0, mmio_base, 4096, mmio_ref, true, false));

    // The general mutation guard remains strict for the unrelated DMA buffer.
    try std.testing.expect(s.rangeOverlapsPinnedUserObject(p0, base, 4096));
}

test "VT-d scatter DMA aliases do not relax other pinned objects or pass-through" {
    var s = try initFdState();
    const scatter_base: u64 = 0x4098_0000;
    _ = try s.createDmaMappingFd(p0, .{
        .device = 1,
        .user_va = scatter_base + 0x180,
        .iova = 0x9098_0180,
        .size = 64,
        .page_count = 1,
        .direction = .from_device,
    }, fdRights(.{ .close = true }), .{}, 16);

    try std.testing.expect(!s.rangeConflictsWithDmaDerivation(
        p0,
        scatter_base,
        4096,
        null,
        true,
        true,
    ));
    try std.testing.expect(s.rangeConflictsWithDmaDerivation(
        p0,
        scatter_base,
        4096,
        null,
        true,
        false,
    ));

    const dma_buffer_base: u64 = scatter_base + 0x4_0000;
    _ = try s.createDmaBufferFd(p0, .{
        .device = 1,
        .user_va = dma_buffer_base,
        .iova = 0x909c_0000,
        .size = 4096,
    }, fdRights(.{ .close = true }), .{}, 16);
    try std.testing.expect(s.rangeConflictsWithDmaDerivation(
        p0,
        dma_buffer_base,
        4096,
        null,
        true,
        true,
    ));

    const mmio_base: u64 = scatter_base + 0x8_0000;
    _ = try s.createMmioRegionFd(p0, .{
        .device = 1,
        .bar_index = 0,
        .paddr = 0xa100_0000,
        .user_va = mmio_base,
        .size = 4096,
    }, fdRights(.{ .close = true }), .{}, 16);
    try std.testing.expect(s.rangeConflictsWithDmaDerivation(
        p0,
        mmio_base,
        4096,
        null,
        true,
        true,
    ));
}

test "process-create grants reject pinned objects and never inherit ambient fds" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const pin_fd = try s.createDmaBufferFd(p0, .{
        .device = 1,
        .user_va = 0x4100_0000,
        .iova = 0x8100_0000,
        .size = 4096,
    }, fdRights(.{ .transfer = true, .close = true }), fdFlags(.{ .inherit = true }), 16);
    const pin_ref = (s.fdEntryConst(p0, pin_fd) orelse unreachable).object;
    const event_ref = try createTestFdObject(&s, 0x5049_4e);
    const event_fd = try s.installFd(
        p0,
        event_ref,
        fdRights(.{ .transfer = true, .close = true }),
        fdFlags(.{ .inherit = true }),
        0,
    );

    try std.testing.expectError(
        KernelError.InvalidState,
        s.transferFd(p0, p1, pin_fd, 16, fdRights(.{ .close = true }), .{}, .copy),
    );
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(pin_ref));
    try std.testing.expect(s.ownerHasPinnedUserObject(p0));

    try s.grantFdsForProcessCreate(p0, p1, &.{}, &free_list);
    try std.testing.expect(s.fdEntryConst(p1, event_fd) == null);
    const pin_grants = [_]ProcessFdGrant{.{ .source_fd = pin_fd, .target_fd = pin_fd, .rights = fd_abi.right_close, .flags = 0 }};
    try std.testing.expectError(KernelError.InvalidState, s.grantFdsForProcessCreate(p0, p1, &pin_grants, &free_list));
    const event_grants = [_]ProcessFdGrant{.{ .source_fd = event_fd, .target_fd = event_fd, .rights = fd_abi.right_close, .flags = 0 }};
    try s.grantFdsForProcessCreate(p0, p1, &event_grants, &free_list);
    try std.testing.expect(s.fdEntryConst(p1, pin_fd) == null);
    try std.testing.expectEqual(
        event_ref,
        (s.fdEntryConst(p1, event_fd) orelse unreachable).object,
    );
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(pin_ref));
    try std.testing.expectEqual(@as(?u32, 2), s.kernelObjectRefCount(event_ref));
}

test "process-create grants require transfer and attenuate private sources without parent mutation" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const ref = try createTestFdObject(&s, 42);
    const source = try s.installFd(p0, ref, fdRights(.{ .transfer = true, .close = true, .inspect = true }), fdFlags(.{ .private = true, .inherit = true }), 16);
    const before = (s.fdEntryConst(p0, source) orelse unreachable).*;
    const grants = [_]ProcessFdGrant{.{ .source_fd = source, .target_fd = 7, .rights = fd_abi.right_inspect, .flags = 0 }};
    try s.grantFdsForProcessCreate(p0, p1, &grants, &free_list);
    const child = s.fdEntryConst(p1, 7) orelse unreachable;
    try std.testing.expectEqual(ref, child.object);
    try std.testing.expectEqual(fd_abi.right_inspect, kernel.fdRightsToBits(child.rights));
    try std.testing.expectEqual(@as(u32, 0), kernel.fdFlagsToBits(child.flags));
    try std.testing.expectEqualDeep(before, (s.fdEntryConst(p0, source) orelse unreachable).*);
    const no_transfer = try s.installFd(p0, ref, fdRights(.{ .close = true }), .{}, 16);
    const invalid = [_]ProcessFdGrant{.{ .source_fd = no_transfer, .target_fd = 8, .rights = fd_abi.right_close, .flags = 0 }};
    try std.testing.expectError(KernelError.InvalidState, s.grantFdsForProcessCreate(p0, p1, &invalid, &free_list));
    try std.testing.expect(s.fdEntryConst(p1, 8) == null);
}

test "process-create rejects invalid grant batches before copying any reference" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const ref = try createTestFdObject(&s, 43);
    const source = try s.installFd(p0, ref, fdRights(.{ .transfer = true, .close = true }), .{}, 16);
    const first = ProcessFdGrant{ .source_fd = source, .target_fd = 4, .rights = fd_abi.right_close, .flags = 0 };
    const invalid = [_]ProcessFdGrant{
        first, // duplicate destination
        .{ .source_fd = source, .target_fd = 4096, .rights = 0, .flags = 0 },
        .{ .source_fd = source, .target_fd = 5, .rights = fd_abi.right_dup, .flags = 0 },
        .{ .source_fd = source, .target_fd = 5, .rights = 1 << 63, .flags = 0 },
        .{ .source_fd = source, .target_fd = 5, .rights = 0, .flags = 1 << 63 },
    };
    for (invalid) |bad| {
        const grants = [_]ProcessFdGrant{ first, bad };
        try std.testing.expectError(KernelError.InvalidState, s.grantFdsForProcessCreate(p0, p1, &grants, &free_list));
        try std.testing.expect(s.fdEntryConst(p1, 4) == null);
        try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(ref));
    }
    const high = [_]ProcessFdGrant{ first, .{ .source_fd = source, .target_fd = 4095, .rights = fd_abi.right_close, .flags = 0 } };
    try std.testing.expectError(KernelError.OutOfFreePages, s.grantFdsForProcessCreate(p0, p1, &high, &free_list));
    try std.testing.expect(s.fdEntryConst(p1, 4) == null);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(ref));
    try free_list.appendContiguousRange(0, @intFromPtr(&fd_capacity_backing), 64);
    const free_before = free_list.pageCount();
    try s.grantFdsForProcessCreate(p0, p1, &high, &free_list);
    try std.testing.expectEqual(ref, (s.fdEntryConst(p1, 4095) orelse unreachable).object);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expectEqual(free_before, free_list.pageCount());
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(ref));
}

test "process-create rolls back partial retains including non-closeable grants" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const first = try createTestFdObject(&s, 44);
    const second = try createTestFdObject(&s, 45);
    const a = try s.installFd(p0, first, fdRights(.{ .transfer = true }), .{}, 16);
    const b = try s.installFd(p0, second, fdRights(.{ .transfer = true }), .{}, 16);
    const saturated = s.kernelObjectSlot(second) orelse unreachable;
    saturated.ref_count = std.math.maxInt(u32);
    defer saturated.ref_count = 1;
    const grants = [_]ProcessFdGrant{
        .{ .source_fd = a, .target_fd = 4, .rights = 0, .flags = 0 },
        .{ .source_fd = b, .target_fd = 5, .rights = 0, .flags = 0 },
    };
    try std.testing.expectError(KernelError.TableFull, s.grantFdsForProcessCreate(p0, p1, &grants, &free_list));
    try std.testing.expect(s.fdEntryConst(p1, 4) == null);
    try std.testing.expect(s.fdEntryConst(p1, 5) == null);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(first));
}

test "process-create handle failure cleanup returns high grant pages and references" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, @intFromPtr(&fd_capacity_backing), 64);
    const pages = free_list.pageCount();
    const ref = try createTestFdObject(&s, 46);
    const source = try s.installFd(p0, ref, fdRights(.{ .transfer = true }), .{}, 16);
    while (true) {
        _ = s.installFd(p0, ref, fdRights(.{}), .{}, 16) catch |err| {
            try std.testing.expectEqual(KernelError.TableFull, err);
            break;
        };
    }
    const references = s.kernelObjectRefCount(ref);
    const grants = [_]ProcessFdGrant{.{ .source_fd = source, .target_fd = 4095, .rights = 0, .flags = 0 }};
    try s.grantFdsForProcessCreate(p0, p1, &grants, &free_list);
    try std.testing.expect(free_list.pageCount() < pages);
    try std.testing.expectError(KernelError.TableFull, s.createProcessFd(p0, .{ .principal_raw = @intFromEnum(p1), .state = .active, .exit_code = 0 }, fdRights(.{ .close = true }), .{}, 16));
    // These are the state cleanup calls in the unpublished syscall transaction.
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expect(s.removeProcessDescriptor(p1));
    try std.testing.expectEqual(references, s.kernelObjectRefCount(ref));
    try std.testing.expectEqual(pages, free_list.pageCount());
    try std.testing.expect(s.fdEntryConst(p1, 4095) == null);
}

test "owner teardown revokes exact pinned aliases only" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const owner_fd = try s.createDmaBufferFd(p0, .{
        .device = 1,
        .user_va = 0x4200_0000,
        .iova = 0x8200_0000,
        .size = 4096,
    }, fdRights(.{ .close = true }), .{}, 16);
    const owner_ref = (s.fdEntryConst(p0, owner_fd) orelse unreachable).object;
    const alias_fd = try s.installFd(p1, owner_ref, fdRights(.{ .close = true }), .{}, 16);
    const unrelated_fd = try s.createDmaMappingFd(p1, .{
        .device = 2,
        .user_va = 0x4300_0000,
        .iova = 0x8300_0000,
        .size = 4096,
        .direction = .bidirectional,
    }, fdRights(.{ .close = true }), .{}, 16);
    const unrelated_ref = (s.fdEntryConst(p1, unrelated_fd) orelse unreachable).object;

    try std.testing.expectEqual(@as(?u32, 2), s.kernelObjectRefCount(owner_ref));
    s.releasePrincipalNativeMemory(p0, &free_list);
    try std.testing.expect(s.fdEntryConst(p0, owner_fd) == null);
    try std.testing.expect(s.fdEntryConst(p1, alias_fd) == null);
    try std.testing.expectEqual(@as(?u32, null), s.kernelObjectRefCount(owner_ref));
    try std.testing.expect(!s.pinned_object_slots.isSet(owner_ref.index));
    try std.testing.expect(s.pinned_object_slots.isSet(unrelated_ref.index));
    try std.testing.expectEqual(unrelated_ref, (s.fdEntryConst(p1, unrelated_fd) orelse unreachable).object);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(unrelated_ref));
}

test "scatter DMA mapping keeps ABI page count without side storage" {
    var s = try initFdState();

    const fd = try s.createDmaMappingFd(p0, .{
        .device = 1,
        .user_va = 0x4400_0080,
        .iova = 0x8200_0080,
        .size = 3 * 4096 - 0x80,
        .page_count = 3,
        .direction = .bidirectional,
    }, fdRights(.{ .close = true }), .{}, 16);

    const object_ref = (s.fdEntryConst(p0, fd) orelse unreachable).object;
    const slot = s.kernelObjectSlotConst(object_ref) orelse unreachable;
    const mapping = switch (slot.payload) {
        .dma_mapping => |value| value,
        else => return error.TestExpectedEqual,
    };
    try std.testing.expectEqual(@as(u16, 3), mapping.page_count);
    try std.testing.expectEqual(@as(u64, 0x8200_0080), mapping.iova);

    try s.closeFd(p0, fd);
    try std.testing.expectEqual(@as(?u32, null), s.kernelObjectRefCount(object_ref));
}

test "scatter DMA mapping validates fd capacity without allocator side storage" {
    var s = try initFdState();
    try std.testing.expectError(KernelError.TableFull, s.createDmaMappingFd(p0, .{
        .device = 1,
        .user_va = 0x4500_0000,
        .iova = 0x8200_0000,
        .size = 4096,
        .page_count = 1,
    }, fdRights(.{ .close = true }), .{}, @intCast(kernel.fd_table_entries)));
    try std.testing.expectEqual(@as(usize, 0), s.pinned_object_slots.count());
    try std.testing.expectError(KernelError.TableFull, s.createDmaBufferFd(p0, .{
        .device = 1,
        .user_va = 0x4500_0000,
        .iova = 0x8200_0000,
        .size = 4096,
    }, fdRights(.{ .close = true }), .{}, @intCast(kernel.fd_table_entries)));
    try std.testing.expectEqual(@as(usize, 0), s.pinned_object_slots.count());
}

test "owner revoke tears down scatter DMA without side storage" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const owner_fd = try s.createDmaMappingFd(p0, .{
        .device = 1,
        .user_va = 0x4600_0080,
        .iova = 0x8200_0080,
        .size = 8192 - 0x80,
        .page_count = 2,
    }, fdRights(.{ .close = true }), .{}, 16);
    const object_ref = (s.fdEntryConst(p0, owner_fd) orelse unreachable).object;
    const alias_fd = try s.installFd(p1, object_ref, fdRights(.{ .close = true }), .{}, 16);
    s.releasePrincipalNativeMemory(p0, &free_list);
    try std.testing.expect(s.fdEntryConst(p0, owner_fd) == null);
    try std.testing.expect(s.fdEntryConst(p1, alias_fd) == null);
    try std.testing.expectEqual(@as(usize, 0), free_list.len);
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

test "process signal pending hint is process-scoped and cleared by lifecycle" {
    var s = try initFdState();
    try std.testing.expectEqual(@as(u64, 0), s.processSignalPendingHintVa(p1));
    try std.testing.expect(s.setProcessSignalPendingHintVa(p1, 0x1234));
    try std.testing.expectEqual(@as(u64, 0x1234), s.processSignalPendingHintVa(p1));

    const child = s.createProcessDescriptor("fork-child") orelse unreachable;
    try std.testing.expectEqual(@as(u64, 0), s.processSignalPendingHintVa(child));

    const p1_index = kernel.processIndexFromPrincipal(p1) orelse unreachable;
    try std.testing.expect(s.markProcessExited(p1));
    try std.testing.expectEqual(
        @as(u64, 0),
        s.processDescriptorSlotConst(p1_index).?.signal_pending_hint_va,
    );

    try std.testing.expect(s.ensureProcessDescriptor(p1, "reused"));
    try std.testing.expect(s.setProcessSignalPendingHintVa(p1, 0x5678));
    try std.testing.expect(s.markProcessFaulted(p1, 13));
    try std.testing.expectEqual(
        @as(u64, 0),
        s.processDescriptorSlotConst(p1_index).?.signal_pending_hint_va,
    );
}

test "stale active process fd cannot retarget a reused principal generation" {
    var s = try initFdState();
    const rights = fdRights(.{ .kill = true, .wait = true, .close = true });
    const stale_fd = try s.createProcessFd(p0, .{
        .principal_raw = @intFromEnum(p1),
        .state = .active,
    }, rights, .{}, 16);
    const old_generation = s.processDescriptor(p1).?.generation;

    try std.testing.expect(s.markProcessExited(p1));
    try std.testing.expect(s.ensureProcessDescriptor(p1, "reused"));
    try std.testing.expect(s.processDescriptor(p1).?.generation != old_generation);
    try std.testing.expect(s.processObjectForFd(p0, stale_fd, fdRights(.{ .kill = true })) == null);
}

test "terminal process fd remains waitable after principal reuse" {
    var s = try initFdState();
    const rights = fdRights(.{ .wait = true, .close = true });
    const fd = try s.createProcessFd(p0, .{
        .principal_raw = @intFromEnum(p1),
        .state = .active,
    }, rights, .{}, 16);

    s.markProcessObjectsExited(p1, .exited, 23);
    try std.testing.expect(s.markProcessExited(p1));
    try std.testing.expect(s.ensureProcessDescriptor(p1, "reused"));
    const terminal = s.processObjectForFd(p0, fd, fdRights(.{ .wait = true })) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(kernel.TaskObjectState.exited, terminal.state);
    try std.testing.expectEqual(@as(u32, 23), terminal.exit_code);
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

    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, fd1, 1, 0x1000, 7, 11, 1, group));
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, fd2, 1, 0x1018, 7, 11, 1, group));
    try s.armFdWaitGroup(group);

    var targets: [2]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), s.takeTaskReadableWaitersForPrincipal(p1, targets[0..]));
    s.markProcessObjectsExited(p1, .exited, 0);
    try std.testing.expectEqual(@as(usize, 1), s.takeTaskReadableWaitersForPrincipal(p1, targets[0..]));
    try std.testing.expectEqual(@as(u64, 0x1000), targets[0].pollfd_va);
    s.markProcessObjectsExited(p2, .exited, 0);
    try std.testing.expectEqual(@as(usize, 1), s.takeTaskReadableWaitersForPrincipal(p2, targets[0..]));
    try std.testing.expectEqual(@as(u64, 0x1018), targets[0].pollfd_va);
}

test "fork excludes private capabilities without changing parent references" {
    var s = try initFdState();
    const private_object = try createTestFdObject(&s, 501);
    const ordinary_object = try createTestFdObject(&s, 502);
    const private_fd = try s.installFd(p0, private_object, fdRights(.{ .inspect = true, .close = true }), .{ .private = true, .inherit = true }, 16);
    const ordinary_fd = try s.installFd(p0, ordinary_object, fdRights(.{ .inspect = true, .close = true, .dup = true }), .{ .cloexec = true, .nonblock = true }, 16);
    (s.getFdTable(p0) orelse unreachable).entries[ordinary_fd].offset = 12345;
    const original = (s.getFdTable(p0) orelse unreachable).entries[ordinary_fd];
    try s.cloneFdTableForFork(p0, p1);
    try std.testing.expect(s.fdEntryConst(p1, private_fd) == null);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(private_object));
    try std.testing.expectEqual(@as(?u32, 2), s.kernelObjectRefCount(ordinary_object));
    const copied = s.fdEntryConst(p1, ordinary_fd) orelse unreachable;
    try std.testing.expectEqual(original.object, copied.object);
    try std.testing.expectEqual(kernel.fdRightsToBits(original.rights), kernel.fdRightsToBits(copied.rights));
    try std.testing.expectEqual(kernel.fdFlagsToBits(original.flags), kernel.fdFlagsToBits(copied.flags));
    try std.testing.expectEqual(original.offset, copied.offset);
    try s.closeFd(p1, ordinary_fd);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(ordinary_object));
    try std.testing.expect(s.fdEntryConst(p0, private_fd) != null);
}

test "thread observation wakes only terminal generation across objects and owners" {
    var s = try initFdState();
    const rights = fdRights(.{ .inspect = true, .wait = true, .poll = true, .transfer = true, .close = true });
    var fds: [3]kernel.Fd = undefined;
    for ([_]u32{ 3, 3, 4 }, 0..) |slot, i| {
        const source = try s.createThreadFd(p1, .{
            .owner_principal_raw = @intFromEnum(p1),
            .thread_index = slot,
            .thread_generation = 10,
            .state = .active,
            .exit_code = 0,
        }, rights, .{}, 16);
        fds[i] = try s.transferFd(p1, p0, source, 16, rights, .{}, .move);
    }
    const process_fd = try s.createProcessFd(p0, .{
        .principal_raw = @intFromEnum(p1),
        .state = .active,
        .exit_code = 0,
    }, rights, .{}, 16);
    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    for (fds, 0..) |fd, i|
        try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, fd, fd_abi.event_readable, 0x1000 + i * 24, 7, 11, 1, group));
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, process_fd, fd_abi.event_readable, 0x2000, 7, 11, 1, group));
    try s.armFdWaitGroup(group);
    var targets: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), s.takeTaskReadableWaitersForPrincipal(p1, &targets));
    s.markThreadObjectsExitedBySlot(3, 10, .exited, 23);
    // A short output buffer must not consume an undelivered wake.
    try std.testing.expectEqual(@as(usize, 1), s.takeTaskReadableWaitersForPrincipal(p1, &targets));
    try std.testing.expectEqual(@as(u64, 0x1000), targets[0].pollfd_va);
    try std.testing.expectEqual(@as(usize, 1), s.takeTaskReadableWaitersForPrincipal(p1, &targets));
    try std.testing.expectEqual(@as(u64, 0x1018), targets[0].pollfd_va);
    try std.testing.expectEqual(@as(usize, 0), s.takeTaskReadableWaitersForPrincipal(p1, &targets));
    for (fds[0..2]) |fd| {
        const thread = s.threadObjectForFd(p0, fd, .{ .wait = true }) orelse unreachable;
        try std.testing.expectEqual(kernel.TaskObjectState.exited, thread.state);
        try std.testing.expectEqual(@as(u32, 23), thread.exit_code);
    }
    try std.testing.expectEqual(@as(?u64, 0), s.fdPollEvents(p0, fds[2], fd_abi.event_readable, 0));
    try std.testing.expectEqual(@as(?u64, 0), s.fdPollEvents(p0, process_fd, fd_abi.event_readable, 0));
    const reused = try s.createThreadFd(p0, .{
        .owner_principal_raw = @intFromEnum(p1),
        .thread_index = 3,
        .thread_generation = 11,
        .state = .active,
        .exit_code = 0,
    }, rights, .{}, 16);
    s.markThreadObjectsExitedBySlot(3, 11, .killed, 99);
    try std.testing.expectEqual(@as(u32, 99), (s.threadObjectForFd(p0, reused, .{ .wait = true }) orelse unreachable).exit_code);
    try std.testing.expectEqual(@as(u32, 23), (s.threadObjectForFd(p0, fds[0], .{ .wait = true }) orelse unreachable).exit_code);
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

test "dynamic fd table preserves capabilities through growth fork and teardown" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, @intFromPtr(&fd_capacity_backing), fd_capacity_backing.len / 4096);
    const initial_pages = free_list.pageCount();
    const object = try createTestFdObject(&s, 71);
    const low = try s.installFd(p0, object, fdRights(.{ .dup = true, .close = true }), .{}, 16);
    try s.ensureFdTableCapacity(p0, 512, &free_list);
    try std.testing.expectEqual(@as(usize, 512), s.getFdTableConst(p0).?.slots().len);
    const high = try s.dupFd(p0, low, 300, fdRights(.{ .dup = true, .close = true }), .{ .cloexec = true });
    try std.testing.expectEqual(@as(kernel.Fd, 300), high);
    try s.ensureFdTableCapacity(p0, 1024, &free_list);
    try std.testing.expectEqual(@as(?u32, 2), s.kernelObjectRefCount(object));
    try std.testing.expect(s.getFdTableConst(p0).?.entries[low].isEmpty());
    try std.testing.expect(s.fdEntryConst(p0, 4096) == null);
    try std.testing.expect(s.fdEntryConst(p1, high) == null);
    try s.ensureFdTableCapacity(p1, 1024, &free_list);
    try s.cloneFdTableForFork(p0, p1);
    try std.testing.expect(s.fdEntryConst(p1, high) != null);
    try s.closeCloexecFdsWithFreeList(p1, &free_list);
    try std.testing.expect(s.fdEntryConst(p1, high) == null);
    var empty = FreePageList{};
    try std.testing.expectError(KernelError.OutOfFreePages, s.ensureFdTableCapacity(p0, 2048, &empty));
    try std.testing.expect(s.fdEntryConst(p0, high) != null);
    s.releasePrincipalNativeMemory(p0, &free_list);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expectEqual(initial_pages, free_list.pageCount());
    try std.testing.expectEqual(@as(?u32, null), s.kernelObjectRefCount(object));
    try std.testing.expectEqual(@as(usize, 256), s.getFdTableConst(p0).?.slots().len);
}

test "dynamic fd metadata survives full free range list and descriptor reuse" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, @intFromPtr(&fd_capacity_backing), fd_capacity_backing.len / 4096);
    const initial_pages = free_list.pageCount();
    try s.ensureFdTableCapacity(p1, 512, &free_list);
    const allocated_pages = initial_pages - free_list.pageCount();
    var full = FreePageList{};
    // Synthetic occupied PMM range records, never dereferenced as memory.
    for (&full.ranges, 0..) |*range, i| {
        range.* = .{ .region_id = 1, .physical_start = @as(u64, @intCast(i)) * 8192, .len = 1 };
    }
    full.range_len = full.ranges.len;
    full.len = full.ranges.len;
    s.releasePrincipalNativeMemory(p1, &full);
    try std.testing.expect(s.getFdTable(p1).?.retired_storage != null);
    try std.testing.expect(s.removeProcessDescriptor(p1));
    try std.testing.expect(s.ensureProcessDescriptor(p1, "reused-retired-fds"));
    try std.testing.expect(s.getFdTable(p1).?.retired_storage != null);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expect(s.getFdTable(p1).?.retired_storage == null);
    try std.testing.expectEqual(initial_pages, free_list.pageCount());
    try std.testing.expect(allocated_pages > 0);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expectEqual(initial_pages, free_list.pageCount());
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

test "ipc writable waiter wakes when receive frees a full channel queue" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const rights = fdRights(.{
        .send = true,
        .recv = true,
        .wait = true,
        .poll = true,
        .close = true,
    });
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);

    for (0..kernel.max_ipc_queue_messages) |sequence| {
        try s.ipcSend(
            p0,
            pair.a,
            .{ .words = .{ @intCast(sequence), 0, 0, 0 } },
            &free_list,
        );
    }
    try std.testing.expectEqual(
        @as(?u64, 0),
        s.fdPollEvents(p0, pair.a, fd_abi.event_writable, 0),
    );
    try std.testing.expectError(
        KernelError.MailboxFull,
        s.ipcSend(p0, pair.a, .{}, &free_list),
    );

    const group = try s.beginFdWaitGroup(p0, 7, 11, 71);
    try std.testing.expect(try s.registerIpcPollWaitersForFd(
        p0,
        pair.a,
        fd_abi.event_writable,
        0x1000,
        7,
        11,
        71,
        group,
    ));
    try s.armFdWaitGroup(group);

    var targets: [2]kernel.ThreadWakeTarget = undefined;
    const outcome = try s.ipcRecvWithWritableWake(
        p0,
        pair.b,
        0,
        16,
        &free_list,
        targets[0..],
    );
    try std.testing.expectEqual(@as(u64, 0), outcome.message.words[0]);
    try std.testing.expectEqual(@as(usize, 1), outcome.writable_wake_count);
    try std.testing.expectEqual(fd_abi.event_writable, targets[0].revents);
    try std.testing.expectEqual(@as(u64, 0x1000), targets[0].pollfd_va);
    try std.testing.expect(targets[0].group.matches(group));
    try std.testing.expectEqual(
        @as(?u64, fd_abi.event_writable),
        s.fdPollEvents(p0, pair.a, fd_abi.event_writable, 0),
    );
    s.cancelFdWaitGroup(group, 71);
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
    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, first.a, 1, 0x1000, 7, 11, 1, group));
    try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, second.a, 1, 0x1018, 7, 11, 1, group));
    try s.armFdWaitGroup(group);

    s.unregisterFdWaitersForThread(p0, 7, 11);

    var targets: [2]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), try s.wakeIpcWaitersForSendFd(p0, first.b, targets[0..]));
    try std.testing.expectEqual(@as(usize, 0), try s.wakeIpcWaitersForSendFd(p0, second.b, targets[0..]));
}

test "ipc channel hangup waiter wakes only after the peer's final fd closes" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const rights = fdRights(.{
        .send = true,
        .recv = true,
        .dup = true,
        .wait = true,
        .poll = true,
        .close = true,
    });
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const peer_duplicate = try s.dupFd(p0, pair.b, 16, rights, .{});
    const peer_view = s.fdPayloadWithRightsConst(p0, pair.b, .{}) orelse unreachable;
    const peer_handle = switch (peer_view.payload.*) {
        .channel => |handle| handle,
        else => unreachable,
    };

    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, pair.a, fd_abi.event_hangup, 0x1000, 7, 11, 1, group));
    try s.armFdWaitGroup(group);

    var targets: [2]kernel.ThreadWakeTarget = undefined;
    try s.closeFdWithFreeList(p0, pair.b, &free_list);
    try std.testing.expectEqual(@as(usize, 0), s.takeIpcChannelPeerCloseWaiters(peer_handle, targets[0..]));

    try s.closeFdWithFreeList(p0, peer_duplicate, &free_list);
    try std.testing.expectEqual(@as(usize, 1), s.takeIpcChannelPeerCloseWaiters(peer_handle, targets[0..]));
    try std.testing.expectEqual(@as(u64, 0x1000), targets[0].pollfd_va);
    try std.testing.expectEqual(fd_abi.event_hangup, targets[0].revents);
    try std.testing.expectEqual(@as(usize, 0), s.takeIpcChannelPeerCloseWaiters(peer_handle, targets[0..]));
}

test "fd waiter cleanup removes a channel hangup-only waiter" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const rights = fdRights(.{
        .send = true,
        .recv = true,
        .wait = true,
        .poll = true,
        .close = true,
    });
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const peer_view = s.fdPayloadWithRightsConst(p0, pair.b, .{}) orelse unreachable;
    const peer_handle = switch (peer_view.payload.*) {
        .channel => |handle| handle,
        else => unreachable,
    };
    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, pair.a, fd_abi.event_hangup, 0x1000, 7, 11, 1, group));
    try s.armFdWaitGroup(group);

    s.unregisterFdWaitersForThread(p0, 7, 11);
    try s.closeFdWithFreeList(p0, pair.b, &free_list);

    var targets: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), s.takeIpcChannelPeerCloseWaiters(peer_handle, targets[0..]));
}

test "shared broker channel supports 24 hangup waiters and bounded reuse" {
    const s = try std.testing.allocator.create(KernelState);
    defer std.testing.allocator.destroy(s);
    s.* = try initFdState();
    var free_list = FreePageList{};
    const rights = fdRights(.{ .send = true, .recv = true, .wait = true, .poll = true, .close = true });
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const peer = s.fdPayloadWithRightsConst(p0, pair.b, .{}).?.payload.channel;
    var groups: [32]kernel.FdWaitGroupRef = undefined;
    for (&groups, 0..) |*group, i| {
        group.* = try s.beginFdWaitGroup(p0, i, 1, i + 1);
        try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, pair.a, fd_abi.event_hangup, 0x1000 + 24 * i, i, 1, i + 1, group.*));
        try s.armFdWaitGroup(group.*);
    }
    const extra = try s.beginFdWaitGroup(p0, 32, 1, 33);
    try std.testing.expectError(KernelError.TableFull, s.registerIpcPollWaitersForFd(p0, pair.a, fd_abi.event_hangup, 0x2000, 32, 1, 33, extra));
    s.cancelFdWaitGroup(groups[0], 1);
    try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, pair.a, fd_abi.event_hangup, 0x2000, 32, 1, 33, extra));
    try s.armFdWaitGroup(extra);
    // Leave 24 live registrations, including the reused slot.
    for (24..32) |i| s.cancelFdWaitGroup(groups[i], i + 1);
    try s.closeFdWithFreeList(p0, pair.b, &free_list);
    var targets: [32]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 24), s.takeIpcChannelPeerCloseWaiters(peer, &targets));
    var duplicates: [32]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), s.takeIpcChannelPeerCloseWaiters(peer, &duplicates));
    var seen = [_]bool{false} ** 33;
    for (targets[0..24]) |target| {
        try std.testing.expectEqual(fd_abi.event_hangup, target.revents);
        try std.testing.expect(target.thread_index < seen.len);
        try std.testing.expect(!seen[target.thread_index]);
        seen[target.thread_index] = true;
        s.cancelFdWaitGroup(target.group, target.wait_token);
    }
    try std.testing.expect(seen[32] and !seen[0]);
    try std.testing.expectEqual(@as(usize, 0), s.takeIpcChannelPeerCloseWaiters(peer, &targets));
    for (s.fd_wait_group_thread_counts) |count|
        try std.testing.expectEqual(@as(u16, 0), count);
}

test "signal wake cancellation clears ipc recv completion waiter" {
    var s = try initFdState();
    const rights = fdRights(.{
        .send = true,
        .recv = true,
        .wait = true,
        .close = true,
    });
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    _ = try s.registerIpcRecvCompletionWaiterForFd(p0, pair.a, 0x2000, 2, 7, 11, 1, group);
    try s.armFdWaitGroup(group);

    s.unregisterFdWaitersForThread(p0, 7, 11);

    var targets: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), try s.wakeIpcWaitersForSendFd(p0, pair.b, targets[0..]));
}

test "fd wait group cancellation survives a destroyed object in the registration map" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const rights = fdRights(.{
        .send = true,
        .recv = true,
        .wait = true,
        .poll = true,
        .close = true,
    });
    const destroyed = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const survivor = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const group = try s.beginFdWaitGroup(p0, 7, 11, 41);
    try std.testing.expect(try s.registerIpcPollWaitersForFd(
        p0,
        destroyed.a,
        fd_abi.event_readable,
        0x1000,
        7,
        11,
        41,
        group,
    ));
    try std.testing.expect(try s.registerIpcPollWaitersForFd(
        p0,
        survivor.a,
        fd_abi.event_readable,
        0x1018,
        7,
        11,
        41,
        group,
    ));
    try s.armFdWaitGroup(group);

    try s.closeFdWithFreeList(p0, destroyed.a, &free_list);
    try s.closeFdWithFreeList(p0, destroyed.b, &free_list);
    s.cancelFdWaitGroup(group, 41);

    var targets: [2]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(
        @as(usize, 0),
        try s.wakeIpcWaitersForSendFd(p0, survivor.b, targets[0..]),
    );
}

test "fd wait group counts preserve colliding threads and exact cancellation" {
    var s = try initFdState();
    const collision = 7 + kernel.max_fd_wait_groups;
    try std.testing.expectEqual(@as(u16, 0), s.fd_wait_group_thread_counts[7]);
    s.cancelFdWaitGroupsForThread(p0, 7, 11);
    const first = try s.beginFdWaitGroup(p0, 7, 11, 1);
    _ = try s.beginFdWaitGroup(p0, 7, 11, 2);
    const other_thread = try s.beginFdWaitGroup(p1, collision, 11, 3);
    const other_generation = try s.beginFdWaitGroup(p0, 7, 12, 4);
    try std.testing.expectEqual(@as(u16, 4), s.fd_wait_group_thread_counts[7]);
    s.cancelFdWaitGroup(first, 99);
    s.cancelFdWaitGroupsForThread(p1, 7, 11);
    try std.testing.expectEqual(@as(u16, 4), s.fd_wait_group_thread_counts[7]);
    s.cancelFdWaitGroupsForThread(p0, 7, 11);
    try std.testing.expectEqual(@as(u16, 2), s.fd_wait_group_thread_counts[7]);
    try std.testing.expect(s.fd_wait_groups[other_thread.index].state == .building);
    try std.testing.expect(s.fd_wait_groups[other_generation.index].state == .building);
    s.cancelFdWaitGroup(first, 1);
    try std.testing.expectEqual(@as(u16, 2), s.fd_wait_group_thread_counts[7]);
    s.cancelFdWaitGroupsForOwner(p1);
    try std.testing.expectEqual(@as(u16, 1), s.fd_wait_group_thread_counts[7]);
    s.cancelFdWaitGroupsForThread(p0, 7, 12);
    try std.testing.expectEqual(@as(u16, 0), s.fd_wait_group_thread_counts[7]);
}

test "fd wait group counts survive failed setup full capacity and reset" {
    var s = try initFdState();
    try std.testing.expectError(KernelError.InvalidState, s.beginFdWaitGroup(p0, 7, 11, 0));
    const failed_setup = try s.beginFdWaitGroup(p0, 7, 11, 1);
    try std.testing.expectError(KernelError.InvalidState, s.linkFdWaitRegistration(failed_setup, .{}));
    try s.armFdWaitGroup(failed_setup);
    try std.testing.expectError(KernelError.InvalidState, s.armFdWaitGroup(failed_setup));
    s.cancelFdWaitGroup(failed_setup, 1);
    try std.testing.expectEqual(@as(u16, 0), s.fd_wait_group_thread_counts[7]);
    for (0..kernel.max_fd_wait_groups) |i|
        _ = try s.beginFdWaitGroup(p0, 7, 11, i + 1);
    try std.testing.expectError(KernelError.TableFull, s.beginFdWaitGroup(p0, 7, 11, 999));
    try std.testing.expectEqual(@as(u16, kernel.max_fd_wait_groups), s.fd_wait_group_thread_counts[7]);
    s.cancelFdWaitGroupsForOwner(p0);
    try std.testing.expectEqual(@as(u16, 0), s.fd_wait_group_thread_counts[7]);
    _ = try s.beginFdWaitGroup(p0, 7, 11, 1000);
    try s.initFromDetectedRegionsInPlace(1);
    for (s.fd_wait_group_thread_counts) |count|
        try std.testing.expectEqual(@as(u16, 0), count);
}

test "delayed target cannot cancel a reused fd wait group slot" {
    var s = try initFdState();
    const rights = fdRights(.{
        .send = true,
        .recv = true,
        .wait = true,
        .poll = true,
        .close = true,
    });
    const old_pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const new_pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const old_group = try s.beginFdWaitGroup(p0, 7, 11, 51);
    try std.testing.expect(try s.registerIpcPollWaitersForFd(
        p0,
        old_pair.a,
        fd_abi.event_readable,
        0x1000,
        7,
        11,
        51,
        old_group,
    ));
    try s.armFdWaitGroup(old_group);
    var delayed: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(
        @as(usize, 1),
        try s.wakeIpcWaitersForSendFd(p0, old_pair.b, delayed[0..]),
    );
    s.cancelFdWaitGroup(old_group, 51);

    var i: usize = 0;
    while (i < kernel.max_fd_wait_groups - 1) : (i += 1) {
        const transient = try s.beginFdWaitGroup(p0, 8, @intCast(100 + i), @intCast(1000 + i));
        s.cancelFdWaitGroup(transient, @intCast(1000 + i));
    }
    const new_group = try s.beginFdWaitGroup(p0, 7, 11, 52);
    try std.testing.expectEqual(old_group.index, new_group.index);
    try std.testing.expect(old_group.generation != new_group.generation);
    try std.testing.expect(try s.registerIpcPollWaitersForFd(
        p0,
        new_pair.a,
        fd_abi.event_readable,
        0x2000,
        7,
        11,
        52,
        new_group,
    ));
    try s.armFdWaitGroup(new_group);

    s.cancelFdWaitGroup(delayed[0].group, delayed[0].wait_token);
    try std.testing.expectEqual(@as(u16, 1), s.fd_wait_group_thread_counts[7]);
    var current: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(
        @as(usize, 1),
        try s.wakeIpcWaitersForSendFd(p0, new_pair.b, current[0..]),
    );
    try std.testing.expect(current[0].group.matches(new_group));
}

test "duplicate poll entries stay grouped until completion" {
    var s = try initFdState();
    const rights = fdRights(.{
        .send = true,
        .recv = true,
        .wait = true,
        .poll = true,
        .close = true,
    });
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const group = try s.beginFdWaitGroup(p0, 7, 11, 61);
    try std.testing.expect(try s.registerIpcPollWaitersForFd(
        p0,
        pair.a,
        fd_abi.event_readable,
        0x1000,
        7,
        11,
        61,
        group,
    ));
    try std.testing.expect(try s.registerIpcPollWaitersForFd(
        p0,
        pair.a,
        fd_abi.event_readable,
        0x1018,
        7,
        11,
        61,
        group,
    ));
    try s.armFdWaitGroup(group);

    var targets: [2]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(
        @as(usize, 2),
        try s.wakeIpcWaitersForSendFd(p0, pair.b, targets[0..]),
    );
    try std.testing.expect(targets[0].group.matches(group));
    try std.testing.expect(targets[1].group.matches(group));
    try std.testing.expectEqual(@as(u64, 0x1000), targets[0].pollfd_va);
    try std.testing.expectEqual(@as(u64, 0x1018), targets[1].pollfd_va);
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

test "ipc receiver handoff hints follow endpoint receiver and reply creator" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const endpoint = try s.createIpcEndpointFd(
        p0,
        fdRights(.{ .recv = true, .call = true, .transfer = true, .close = true }),
        .{},
        16,
    );
    const client_endpoint = try s.transferFd(
        p0,
        p1,
        endpoint,
        16,
        fdRights(.{ .call = true, .close = true }),
        .{},
        .copy,
    );

    try std.testing.expect(s.ipcReceiverHandoffHintForSendFd(p1, client_endpoint) == null);
    try std.testing.expect(s.noteIpcReceiverForFd(p0, endpoint, 7, 11));
    const endpoint_hint = s.ipcReceiverHandoffHintForSendFd(p1, client_endpoint) orelse unreachable;
    try std.testing.expectEqual(@as(usize, 7), endpoint_hint.thread_index);
    try std.testing.expectEqual(@as(u32, 11), endpoint_hint.thread_generation);
    try std.testing.expectEqual(p0, endpoint_hint.owner);

    const client_reply = try s.ipcCall(
        p1,
        client_endpoint,
        .{ .words = .{ 99, 0, 0, 0 } },
        16,
        &free_list,
    );
    try std.testing.expect(s.noteIpcReceiverForFd(p1, client_reply, 9, 13));
    const request = try s.ipcRecv(p0, endpoint, 1, 16, &free_list);
    const server_reply = request.fds[0].fd;
    const reply_hint = s.ipcReceiverHandoffHintForSendFd(p0, server_reply) orelse unreachable;
    try std.testing.expectEqual(@as(usize, 9), reply_hint.thread_index);
    try std.testing.expectEqual(@as(u32, 13), reply_hint.thread_generation);
    try std.testing.expectEqual(p1, reply_hint.owner);
}

test "irq fd records only matching device and MSI-X entry" {
    var s = try initFdState();
    const irq_fd = try s.createIrqFd(p0, .{
        .device = 0x1001,
        .kind = .msix,
        .vector = 1,
    }, fdRights(.{ .irq_wait = true, .poll = true, .read = true, .close = true }), .{}, 16);
    const auto_fd = try s.createIrqFd(p0, .{
        .device = 0x1001,
        .kind = .auto,
        .vector = 0,
    }, fdRights(.{ .irq_wait = true, .poll = true, .read = true, .close = true }), .{}, 16);
    const other_device_fd = try s.createIrqFd(p0, .{
        .device = 0x1002,
        .kind = .auto,
        .vector = 0,
    }, fdRights(.{ .irq_wait = true, .poll = true, .read = true, .close = true }), .{}, 16);

    try std.testing.expectEqual(@as(?u64, 0), s.irqEventCountForFd(p0, irq_fd, fdRights(.{ .irq_wait = true })));
    try std.testing.expectEqual(@as(?u64, 0), s.fdPollEventsWithWriteMin(p0, irq_fd, 1, 0, 0));

    var wake_owners: [4]kernel.PrincipalId = undefined;
    const wake_count = s.recordDeviceInterruptEvent(0x1001, 1, wake_owners[0..]);
    try std.testing.expectEqual(@as(usize, 1), wake_count);
    try std.testing.expectEqual(p0, wake_owners[0]);
    try std.testing.expectEqual(@as(?u64, 1), s.irqEventCountForFd(p0, irq_fd, fdRights(.{ .irq_wait = true })));
    try std.testing.expectEqual(@as(?u64, 1), s.irqEventCountForFd(p0, auto_fd, fdRights(.{ .irq_wait = true })));
    try std.testing.expectEqual(@as(?u64, 0), s.irqEventCountForFd(p0, other_device_fd, fdRights(.{ .irq_wait = true })));
    try std.testing.expectEqual(@as(?u64, 1), s.fdPollEventsWithWriteMin(p0, irq_fd, 1, 0, 0));
    try std.testing.expect(s.acknowledgeIrqEventCountForFd(p0, irq_fd, 1));
    try std.testing.expectEqual(@as(?u64, 0), s.fdPollEventsWithWriteMin(p0, irq_fd, 1, 0, 0));

    try std.testing.expectEqual(@as(usize, 1), s.recordDeviceInterruptEvent(0x1001, 2, wake_owners[0..]));
    try std.testing.expectEqual(@as(?u64, 1), s.irqEventCountForFd(p0, irq_fd, fdRights(.{ .irq_wait = true })));
    try std.testing.expectEqual(@as(?u64, 2), s.irqEventCountForFd(p0, auto_fd, fdRights(.{ .irq_wait = true })));
    try std.testing.expectEqual(@as(usize, 1), s.recordDeviceInterruptEvent(0x1002, 1, wake_owners[0..]));
    try std.testing.expectEqual(@as(?u64, 1), s.irqEventCountForFd(p0, other_device_fd, fdRights(.{ .irq_wait = true })));
    try std.testing.expectEqual(@as(?u64, 0), s.fdPollEventsWithWriteMin(p0, irq_fd, 1, 0, 0));
}

test "retired irq aliases report hangup and cannot consume a replacement event" {
    var s = try initFdState();
    const rights = fdRights(.{ .irq_wait = true, .poll = true, .read = true, .close = true, .dup = true });
    const fd = try s.createIrqFd(p0, .{ .device = 0x1001, .kind = .msix, .vector = 1 }, rights, .{}, 16);
    const alias = try s.dupFd(p0, fd, 16, rights, .{});
    const original = s.fdEntryConst(p0, fd).?.object;
    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, fd, fd_abi.event_readable, 0x1000, 7, 11, 1, group));
    // A waiter interested only in terminal events must also be registered.
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, alias, 0, 0x1018, 7, 11, 1, group));
    try s.armFdWaitGroup(group);
    var targets: [4]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), s.takeReadyIrqWaiters(&targets));
    s.unpublishIrqObject(original);
    s.kernelObjectSlot(original).?.payload.irq.retired = true;
    try std.testing.expectEqual(@as(usize, 2), s.takeReadyIrqWaiters(&targets));
    for (targets[0..2]) |target| try std.testing.expectEqual(fd_abi.event_hangup, target.revents);
    try std.testing.expectEqual(@as(?u64, fd_abi.event_hangup), s.fdPollEvents(p0, alias, 0, 0));
    try std.testing.expect(s.irqEventCountForFd(p0, alias, .{ .read = true }) == null);
    s.cancelFdWaitGroup(group, 1);
    const replacement = try s.createIrqFd(p0, .{ .device = 0x1001, .kind = .msix, .vector = 1 }, rights, .{}, 16);
    var owners: [4]PrincipalId = undefined;
    try std.testing.expectEqual(@as(usize, 1), s.recordDeviceInterruptEvent(0x1001, 1, &owners));
    try std.testing.expectEqual(@as(?u64, 1), s.irqEventCountForFd(p0, replacement, .{ .read = true }));
    try std.testing.expectEqual(@as(?u64, fd_abi.event_hangup), s.fdPollEvents(p0, fd, fd_abi.event_readable, 0));
    try std.testing.expect(!s.acknowledgeIrqEventCountForFd(p0, alias, 1));
    try s.closeFd(p0, fd);
    try s.closeFd(p0, alias);
    try std.testing.expectEqual(@as(?u64, 1), s.irqEventCountForFd(p0, replacement, .{ .read = true }));
}

test "irq publication retirement drains racing writer before slot reuse" {
    var s = try initFdState();
    const fd = try s.createIrqFd(p0, .{
        .device = 0x1001,
        .kind = .msix,
        .vector = 1,
    }, fdRights(.{ .irq_wait = true, .close = true }), .{}, 16);
    const original = s.fdEntryConst(p0, fd).?.object;
    const Producer = struct {
        state: *KernelState,
        stop: bool = false,
        fn run(self: *@This()) void {
            var owners: [4]PrincipalId = undefined;
            while (!@atomicLoad(bool, &self.stop, .acquire))
                _ = self.state.recordDeviceInterruptEvent(0x1001, 1, &owners);
        }
    };
    var producer = Producer{ .state = &s };
    const thread = try std.Thread.spawn(.{}, Producer.run, .{&producer});
    defer {
        @atomicStore(bool, &producer.stop, true, .release);
        thread.join();
    }
    while (s.irqPublishedEventCount(original).? == 0) std.atomic.spinLoopHint();
    s.unpublishIrqObject(original);
    var generation = original.generation;
    for (0..256) |_| {
        const retiring = kernel.KernelObjectRef{ .kind = .irq, .index = original.index, .generation = generation };
        s.publishIrqObject(retiring, .{
            .owner_principal_raw = @intFromEnum(p0),
            .device = 0x1001,
            .kind = .msix,
            .vector = 1,
        });
        while (s.irqPublishedEventCount(retiring).? == 0) std.atomic.spinLoopHint();
        s.unpublishIrqObject(retiring);
        generation = KernelState.nextObjectGeneration(generation);
        const replacement = kernel.KernelObjectRef{ .kind = .irq, .index = original.index, .generation = generation };
        s.publishIrqObject(replacement, .{
            .owner_principal_raw = @intFromEnum(p1),
            .device = 0x1002,
            .kind = .msix,
            .vector = 1,
        });
        for (0..128) |_| std.atomic.spinLoopHint();
        try std.testing.expectEqual(@as(?u64, 0), s.irqPublishedEventCount(replacement));
        s.unpublishIrqObject(replacement);
        generation = KernelState.nextObjectGeneration(generation);
    }
}

test "irq candidate words preserve boundary slots retirement and reuse" {
    var s = try initFdState();
    var owners: [4]PrincipalId = undefined;
    try std.testing.expectEqual(@as(usize, 0), s.recordDeviceInterruptEvent(0x1001, 1, &owners));
    const indices = [_]u32{ 0, 63, 64, kernel.max_fd_objects - 1 };
    for (indices) |index| {
        const ref = kernel.KernelObjectRef{ .kind = .irq, .index = index, .generation = 7 };
        s.publishIrqObject(ref, .{
            .owner_principal_raw = @intFromEnum(p0),
            .device = 0x1001,
            .kind = .msix,
            .vector = 1,
        });
        try std.testing.expect((s.irq_publish_candidates[index / 64] &
            (@as(u64, 1) << @as(u6, @intCast(index % 64)))) != 0);
    }
    // Multiple matching slots still deduplicate the owner; mismatches are not
    // events even when their candidate bits are set.
    try std.testing.expectEqual(@as(usize, 0), s.recordDeviceInterruptEvent(0x1002, 1, &owners));
    try std.testing.expectEqual(@as(usize, 0), s.recordDeviceInterruptEvent(0x1001, 2, &owners));
    try std.testing.expectEqual(@as(usize, 1), s.recordDeviceInterruptEvent(0x1001, 1, &owners));
    try std.testing.expectEqual(p0, owners[0]);
    for (indices) |index| {
        const ref = kernel.KernelObjectRef{ .kind = .irq, .index = index, .generation = 7 };
        try std.testing.expectEqual(@as(?u64, 1), s.irqPublishedEventCount(ref));
        s.unpublishIrqObject(.{ .kind = .irq, .index = index, .generation = 6 });
        try std.testing.expectEqual(@as(?u64, 1), s.irqPublishedEventCount(ref));
        s.unpublishIrqObject(ref);
        try std.testing.expect((s.irq_publish_candidates[index / 64] &
            (@as(u64, 1) << @as(u6, @intCast(index % 64)))) != 0);
        s.next_fd_object_scan = index;
        const event = try s.createKernelObject(.event, .{ .event = 42 });
        try std.testing.expectEqual(index, event.index);
        try std.testing.expectEqual(@as(usize, 0), s.recordDeviceInterruptEvent(0x1002, 1, &owners));
        try std.testing.expectEqual(@as(u64, 42), s.kernelObjectSlot(event).?.payload.event);
    }
    try std.testing.expectEqual(@as(usize, 0), s.recordDeviceInterruptEvent(0x1001, 1, &owners));
    const replacement = kernel.KernelObjectRef{ .kind = .irq, .index = 64, .generation = 8 };
    s.publishIrqObject(replacement, .{
        .owner_principal_raw = @intFromEnum(p1),
        .device = 0x1002,
        .kind = .msix,
        .vector = 2,
    });
    // A fully saturated conservative bitmap must preserve exact filtering.
    @memset(&s.irq_publish_candidates, std.math.maxInt(u64));
    try std.testing.expectEqual(@as(usize, 0), s.recordDeviceInterruptEvent(0x1001, 1, &owners));
    try std.testing.expectEqual(@as(usize, 1), s.recordDeviceInterruptEvent(0x1002, 2, &owners));
    try std.testing.expectEqual(p1, owners[0]);
    try std.testing.expectEqual(@as(?u64, 1), s.irqPublishedEventCount(replacement));
    // The global reset is quiescent here, just like the existing table reset.
    s.resetKernelObjectTable();
    for (s.irq_publish_candidates) |word| try std.testing.expectEqual(@as(u64, 0), word);
    try std.testing.expectEqual(@as(usize, 0), s.recordDeviceInterruptEvent(0x1002, 2, &owners));
    s.publishIrqObject(replacement, .{
        .owner_principal_raw = @intFromEnum(p1),
        .device = 0x1002,
        .kind = .msix,
        .vector = 2,
    });
    try std.testing.expectEqual(@as(usize, 1), s.recordDeviceInterruptEvent(0x1002, 2, &owners));
}

test "physical page allocation returns a page handle" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x1_0000_0000, 2);

    const page = try s.allocPhysicalPage(&free_list);
    try std.testing.expectEqual(@as(u64, 0x1_0000_0000), page.paddr);
}

fn expectFreeRangeInvariants(list: *const FreePageList) !void {
    var pages: usize = 0;
    for (list.ranges[0..list.range_len], 0..) |range, i| {
        try std.testing.expect(range.len != 0);
        try std.testing.expectEqual(@as(u64, 0), range.physical_start % 4096);
        pages += range.len;
        const end = range.physical_start + range.len * 4096;
        for (list.ranges[i + 1 .. list.range_len]) |other| {
            try std.testing.expect(end <= other.physical_start or
                other.physical_start + other.len * 4096 <= range.physical_start);
        }
    }
    try std.testing.expectEqual(pages, list.pageCount());
}

test "free range removal preserves constrained allocation and surviving ranges" {
    const boundary: u64 = 4 << 30;
    for (0..6) |operation| {
        for (0..3) |removed| {
            if (operation == 0 and removed != 0) continue;
            if (operation == 1 and removed != 2) continue;
            var list = FreePageList{};
            const above = operation == 3 or operation == 5;
            const pages: usize = if (operation >= 4) 2 else 1;
            const target: u64 = if (above) boundary else boundary - pages * 4096;
            for (0..3) |i| {
                const start = if (i == removed) target else
                    (if (above) @as(u64, 0x100000) else boundary + 0x100000) + i * 0x10000;
                try list.appendContiguousRange(@intCast(i), start, if (i == removed) pages else 3);
            }
            const original = list.ranges[0..3].*;
            const allocated = switch (operation) {
                0 => try list.popFront(),
                1 => try list.popBack(),
                2 => try list.popFrontBelow(boundary),
                3 => try list.popFrontAtOrAbove(boundary),
                4 => try list.popContiguousBelow(pages, boundary),
                5 => try list.popContiguousAtOrAbove(pages, boundary),
                else => unreachable,
            };
            try std.testing.expectEqual(target, allocated);
            try std.testing.expectEqual(@as(usize, 2), list.rangeCount());
            try std.testing.expectEqual(@as(usize, 6), list.pageCount());
            for (original, 0..) |range, i| {
                if (i == removed) continue;
                var found: usize = 0;
                for (list.ranges[0..list.range_len]) |survivor| {
                    if (survivor.region_id == range.region_id) {
                        try std.testing.expectEqualDeep(range, survivor);
                        found += 1;
                    }
                }
                try std.testing.expectEqual(@as(usize, 1), found);
            }
            try expectFreeRangeInvariants(&list);
        }
    }
}

test "free range unordered bridge merge preserves relocated survivor" {
    const permutations = [_][3]usize{
        .{ 0, 1, 2 }, .{ 0, 2, 1 }, .{ 1, 0, 2 },
        .{ 1, 2, 0 }, .{ 2, 0, 1 }, .{ 2, 1, 0 },
    };
    for ([_]usize{ 1, 2 }) |gap_pages| {
        for (permutations) |order| {
            var list = FreePageList{};
            const base: u64 = 0x100000;
            const upper = base + (gap_pages + 1) * 4096;
            const starts = [_]u64{ base, upper, 0x300000 };
            for (order) |index| try list.appendPage(7, starts[index]);
            if (gap_pages == 1) {
                try list.appendPage(7, base + 4096);
            } else {
                try list.appendContiguousRange(7, base + 4096, gap_pages);
            }
            try std.testing.expectEqual(@as(usize, 2), list.rangeCount());
            try expectFreeRangeInvariants(&list);
            const merged = try list.popContiguousBelow(gap_pages + 2, 0x200000);
            try std.testing.expectEqual(base, merged);
            try std.testing.expectEqual(@as(u64, 0x300000), try list.popFront());
            try std.testing.expectEqual(@as(usize, 0), list.rangeCount());
            try expectFreeRangeInvariants(&list);
            try std.testing.expectError(KernelError.OutOfFreePages, list.popBack());
        }
    }
    // Physical adjacency alone must not merge distinct region owners.
    var separate = FreePageList{};
    try separate.appendPage(1, 0x100000);
    try separate.appendPage(2, 0x102000);
    try separate.appendPage(1, 0x101000);
    try std.testing.expectEqual(@as(usize, 2), separate.rangeCount());
    try std.testing.expectError(KernelError.OutOfFreePages, separate.popContiguousBelow(3, 0x200000));
    try expectFreeRangeInvariants(&separate);
}

test "free range fragmented drain returns every page once after repeated removals" {
    var list = FreePageList{};
    var seen = [_]bool{false} ** 257;
    const base: u64 = 0x100000;
    for (0..seen.len) |i| try list.appendPage(0, base + i * 8192);
    for (0..seen.len) |i| {
        const page = if (i % 2 == 0) try list.popFront() else try list.popBack();
        try std.testing.expect(page >= base and (page - base) % 8192 == 0);
        const index = (page - base) / 8192;
        try std.testing.expect(index < seen.len and !seen[index]);
        seen[index] = true;
        try std.testing.expectEqual(seen.len - i - 1, list.pageCount());
        try std.testing.expectEqual(seen.len - i - 1, list.rangeCount());
    }
    for (seen) |returned| try std.testing.expect(returned);
    try expectFreeRangeInvariants(&list);
    try std.testing.expectError(KernelError.OutOfFreePages, list.popFront());
}

test "free range allocation split failure preserves all physical pages" {
    for ([_]bool{ false, true }) |contiguous| {
        var free_list = FreePageList{};
        const boundary: u64 = 4 << 30;
        free_list.range_len = FreePageList.max_ranges;
        free_list.len = FreePageList.max_ranges + 3;
        free_list.ranges[0] = .{ .region_id = 0, .physical_start = boundary - 4096, .len = 4 };
        for (free_list.ranges[1..], 1..) |*range, index| {
            range.* = .{ .region_id = 0, .physical_start = (20 << 30) + index * 8192, .len = 1 };
        }
        if (contiguous) {
            try std.testing.expectError(KernelError.TooManyFreeRanges, free_list.popContiguousAtOrAbove(2, boundary));
        } else {
            try std.testing.expectError(KernelError.TooManyFreeRanges, free_list.popFrontAtOrAbove(boundary));
        }
        try std.testing.expectEqual(@as(usize, FreePageList.max_ranges), free_list.rangeCount());
        try std.testing.expectEqual(@as(usize, FreePageList.max_ranges + 3), free_list.pageCount());
        try std.testing.expectEqual(boundary - 4096, free_list.ranges[0].physical_start);
        try std.testing.expectEqual(@as(usize, 4), free_list.ranges[0].len);
        for (free_list.ranges[1..], 1..) |range, index| {
            try std.testing.expectEqual(@as(u64, (20 << 30) + index * 8192), range.physical_start);
            try std.testing.expectEqual(@as(usize, 1), range.len);
        }
        // A full descriptor table must still allow consuming the entire tail.
        const tail = try free_list.popContiguousAtOrAbove(3, boundary);
        try std.testing.expectEqual(boundary, tail);
        try std.testing.expectEqual(@as(usize, 1), free_list.ranges[0].len);
        try std.testing.expectEqual(@as(usize, FreePageList.max_ranges), free_list.pageCount());
    }
}

test "zero-on-demand VMO holes read as zero without physical allocation" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const rights = fdRights(.{ .read = true, .close = true, .resize = true, .map_read = true, .map_write = true });
    const fd = try s.createZeroOnDemandVmoFd(p0, 8192, rights, .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const before = kernel.vmObjectBackingStoreStats().used_entries;
    var bytes: [5000]u8 = @splat(0xa5);
    try std.testing.expectEqual(bytes.len, try s.readFdVmoBytes(p0, fd, &bytes));
    for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
    try std.testing.expectEqual(@as(usize, 3192), try s.readFdVmoBytes(p0, fd, &bytes));
    try std.testing.expectEqual(@as(usize, 0), try s.readFdVmoBytes(p0, fd, &bytes));
    try std.testing.expectEqual(before, kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoResolvedPagePaddrOrZero(ref, 1));
    try std.testing.expect(s.nativeVmoResolvedPagePaddr(ref, 1) == null);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expect(s.nativeVmoSlot(ref) == null);
    const eager = try s.createAnonymousVmoFd(p0, 4096, rights, .{}, 16);
    try std.testing.expect(!s.nativeVmoIsZeroOnDemand(s.nativeVmoRefForFd(p0, eager).?));
    try std.testing.expectError(KernelError.InvalidState, s.readFdVmoBytes(p0, eager, &bytes));
    try s.closeFdWithFreeList(p0, eager, &free_list);
}

test "zero-on-demand shared faults converge and grow retains aliases" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 20 << 30, 8);
    const rights = fdRights(.{ .close = true, .resize = true, .map_read = true, .map_write = true });
    const fd = try s.createZeroOnDemandVmoFd(p0, 8192, rights, .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const va: u64 = 0x4900_0000;
    _ = try s.mmapFd(p0, fd, va, 8192, .{ .read = true, .write = true }, .{ .shared = true }, 0);
    try s.cloneFdTableForFork(p0, p1);
    try s.cloneVmaTableForFork(p0, p1);
    const a = s.prepareNativeVmaFaultMapping(p0, va, true, false);
    const b = s.prepareNativeVmaFaultMapping(p1, va, false, false);
    try std.testing.expectEqual(.allocate_zero, a.kind);
    try std.testing.expectEqual(.allocate_zero, b.kind);
    const page = (try s.allocPhysicalPage(&free_list)).paddr;
    const spare = (try s.allocPhysicalPage(&free_list)).paddr;
    try std.testing.expectEqual(page, s.commitNativeVmaFaultMapping(p0, a, page).?.paddr);
    try std.testing.expectEqual(page, s.commitNativeVmaFaultMapping(p1, b, spare).?.paddr);
    try free_list.appendPage(0, spare);
    const old = s.vmaEntryForVaConst(p0, va).?.*;
    try s.growVmoFdWithPages(p0, fd, 16384, &free_list);
    try std.testing.expectEqual(@as(usize, 7), free_list.pageCount());
    try std.testing.expectEqualDeep(old, s.vmaEntryForVaConst(p0, va).?.*);
    try std.testing.expectEqual(@as(?u64, page), s.nativeVmoResolvedPagePaddr(ref, 0));
    try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoResolvedPagePaddrOrZero(ref, 3));
    var fixed = try s.prepareFixedFdMmap(p0, fd, va + 0x10000, 16384, .{ .read = true, .write = true }, .{ .shared = true }, 0, &free_list);
    s.discardFixedMmapPrepared(&fixed, &free_list);
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, fd, 4096, &free_list));
    try std.testing.expectEqual(@as(?u64, 16384), s.nativeVmoSize(ref));
    s.releasePrincipalNativeMemory(p0, &free_list);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expect(s.nativeVmoSlot(ref) == null);
    try std.testing.expectEqual(@as(usize, 8), free_list.pageCount());
}

test "zero-on-demand private COW retries changed source and separates fork writes" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 20 << 30, 16);
    const fd = try s.createZeroOnDemandVmoFd(p0, 8192, fdRights(.{ .close = true, .map_read = true, .map_write = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const va: u64 = 0x4900_0000;
    _ = try s.mmapFd(p0, fd, va, 8192, .{ .read = true, .write = true }, .{ .private = true }, 0);
    _ = try s.mmapFd(p0, fd, va + 0x10000, 8192, .{ .read = true, .write = true }, .{ .shared = true }, 0);
    const stale = s.prepareNativeVmaCowMapping(p0, va, true, false);
    try std.testing.expectEqual(.allocate_zero, stale.kind);
    const shared_plan = s.prepareNativeVmaFaultMapping(p0, va + 0x10000, true, false);
    const shared_page = (try s.allocPhysicalPage(&free_list)).paddr;
    _ = s.commitNativeVmaFaultMapping(p0, shared_plan, shared_page).?;
    const candidate = (try s.allocPhysicalPage(&free_list)).paddr;
    try std.testing.expect(s.commitNativeVmaCowMapping(p0, stale, candidate, &free_list) == null);
    try free_list.appendPage(0, candidate);
    try std.testing.expectEqual(.allocate_copy, s.prepareNativeVmaCowMapping(p0, va, true, false).kind);
    const untouched = s.prepareNativeVmaCowMapping(p0, va + 4096, true, false);
    try std.testing.expectEqual(.allocate_zero, untouched.kind);
    const own = (try s.allocPhysicalPage(&free_list)).paddr;
    _ = s.commitNativeVmaCowMapping(p0, untouched, own, &free_list).?;
    try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoResolvedPagePaddrOrZero(ref, 1));
    try s.cloneFdTableForFork(p0, p1);
    try s.cloneVmaTableForFork(p0, p1);
    try std.testing.expectEqual(.locked_slow_path, s.prepareNativeVmaCowMapping(p1, va + 4096, true, false).kind);
    const child = s.ensureNativeVmaCowMappingLockedSlow(p1, va + 4096, true, false, &free_list).?;
    try std.testing.expect(child.paddr != own);
    try std.testing.expectEqual(own, s.nativeVmaFaultMapping(p0, va + 4096, false, false).?.paddr);
    try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoResolvedPagePaddrOrZero(ref, 1));
    s.releasePrincipalNativeMemory(p0, &free_list);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expectEqual(@as(usize, 16), free_list.pageCount());
}

test "zero-on-demand does not accept stale out-of-bounds or malformed backing" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const fd = try s.createZeroOnDemandVmoFd(p0, 4096, fdRights(.{ .close = true, .map_read = true, .map_write = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const va: u64 = 0x4900_0000;
    _ = try s.mmapFd(p0, fd, va, 4096, .{ .read = true, .write = true }, .{ .shared = true }, 0);
    const plan = s.prepareNativeVmaFaultMapping(p0, va, true, false);
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 1) == null);
    const slot = s.nativeVmoSlot(ref).?;
    const saved = slot.*;
    slot.page_store_start +%= 1;
    try std.testing.expectEqual(.denied, s.prepareNativeVmaFaultMapping(p0, va, true, false).kind);
    try std.testing.expect(s.commitNativeVmaFaultMapping(p0, plan, 0x90000000) == null);
    slot.* = saved;
    slot.parent = .{ .index = ref.index, .generation = ref.generation + 1 };
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 0) == null);
    slot.* = saved;
    const view = try s.createNativePageView(ref, 1);
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(view, 0) == null);
    s.releaseNativeVmoWithFreeList(view, &free_list);
    s.releasePrincipalNativeMemory(p0, &free_list);
    s.next_native_vmo_scan = ref.index;
    const replacement = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{ .close = true }), .{}, 16);
    const fresh = s.nativeVmoRefForFd(p0, replacement).?;
    try std.testing.expectEqual(ref.index, fresh.index);
    try std.testing.expect(fresh.generation != ref.generation);
    try std.testing.expect(!s.nativeVmoIsZeroOnDemand(fresh));
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 0) == null);
    try s.closeFdWithFreeList(p0, replacement, &free_list);
}

test "zero-on-demand VMO holes read as zero and shared fault candidates converge across growth" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const base: u64 = 20 << 30;
    try free_list.appendContiguousRange(0, base, 8);
    const rights = fdRights(.{ .resize = true, .read = true, .inspect = true, .close = true, .dup = true, .map_read = true, .map_write = true });
    const fd = try s.createZeroOnDemandVmoFd(p0, 8192, rights, .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    try std.testing.expect(s.nativeVmoIsZeroOnDemand(ref));
    try std.testing.expectEqual(@as(usize, 8), free_list.pageCount());
    var bytes: [8192]u8 = @splat(0xa5);
    try std.testing.expectEqual(bytes.len, try s.readFdVmoBytes(p0, fd, &bytes));
    for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
    try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoResolvedPagePaddrOrZero(ref, 0));
    try std.testing.expect(s.nativeVmoResolvedPagePaddr(ref, 0) == null);
    _ = try s.mmapFd(p0, fd, 0x4900_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0);
    try s.cloneFdTableForFork(p0, p1);
    try s.cloneVmaTableForFork(p0, p1);
    const a = s.prepareNativeVmaFaultMapping(p0, 0x4900_0000, true, false);
    const b = s.prepareNativeVmaFaultMapping(p1, 0x4900_0000, false, false);
    try std.testing.expectEqual(.allocate_zero, a.kind);
    try std.testing.expectEqual(.allocate_zero, b.kind);
    const first = (try s.allocPhysicalPage(&free_list)).paddr;
    const spare = (try s.allocPhysicalPage(&free_list)).paddr;
    try std.testing.expectEqual(first, s.commitNativeVmaFaultMapping(p0, a, first).?.paddr);
    try std.testing.expectEqual(first, s.commitNativeVmaFaultMapping(p1, b, spare).?.paddr);
    try free_list.appendPage(0, spare);
    const original = s.vmaEntryForVaConst(p0, 0x4900_0000).?.*;
    try s.growVmoFdWithPages(p0, fd, 16384, &free_list);
    try std.testing.expectEqualDeep(original, s.vmaEntryForVaConst(p0, 0x4900_0000).?.*);
    try std.testing.expectEqual(@as(?u64, first), s.nativeVmoResolvedPagePaddr(ref, 0));
    try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoResolvedPagePaddrOrZero(ref, 3));
    try std.testing.expectEqual(@as(usize, 7), free_list.pageCount());
    var replacement = try s.prepareFixedFdMmap(p0, fd, 0x4a00_0000, 16384, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0, &free_list);
    s.discardFixedMmapPrepared(&replacement, &free_list);
    s.releasePrincipalNativeMemory(p0, &free_list);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expect(s.nativeVmoSlot(ref) == null);
    try std.testing.expectEqual(@as(usize, 8), free_list.pageCount());
}

test "zero-on-demand private COW stays separate and retries after shared publication" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 20 << 30, 16);
    const fd = try s.createZeroOnDemandVmoFd(p0, 8192, fdRights(.{ .close = true, .map_read = true, .map_write = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    _ = try s.mmapFd(p0, fd, 0x4900_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .private = true }), 0);
    _ = try s.mmapFd(p0, fd, 0x4a00_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0);
    const plan = s.prepareNativeVmaCowMapping(p0, 0x4900_0000, true, false);
    try std.testing.expectEqual(.allocate_zero, plan.kind);
    const private_page = (try s.allocPhysicalPage(&free_list)).paddr;
    const shared_page = (try s.allocPhysicalPage(&free_list)).paddr;
    const shared = s.prepareNativeVmaFaultMapping(p0, 0x4a00_0000, true, false);
    _ = s.commitNativeVmaFaultMapping(p0, shared, shared_page).?;
    try std.testing.expect(s.commitNativeVmaCowMapping(p0, plan, private_page, &free_list) == null);
    const retry = s.prepareNativeVmaCowMapping(p0, 0x4900_0000, true, false);
    try std.testing.expectEqual(.allocate_copy, retry.kind);
    try std.testing.expectEqual(shared_page, retry.source_paddr);
    // State-machine test: physical copying/zeroing is exercised by guest tests.
    try std.testing.expectEqual(private_page, s.commitNativeVmaCowMapping(p0, retry, private_page, &free_list).?.paddr);
    try std.testing.expectEqual(@as(?u64, shared_page), s.nativeVmoResolvedPagePaddr(ref, 0));
    try s.cloneVmaTableForFork(p0, p1);
    const child = s.ensureNativeVmaCowMappingLockedSlow(p1, 0x4900_1000, true, false, &free_list).?;
    try std.testing.expect(child.paddr != 0);
    try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoResolvedPagePaddrOrZero(ref, 1));
    try std.testing.expect(s.nativeVmaFaultMapping(p0, 0x4900_1000, false, false) == null);
    const parent = s.ensureNativeVmaCowMappingLockedSlow(p0, 0x4900_1000, true, false, &free_list).?;
    try std.testing.expect(parent.paddr != child.paddr);
    s.releasePrincipalNativeMemory(p1, &free_list);
    s.releasePrincipalNativeMemory(p0, &free_list);
    try std.testing.expectEqual(@as(usize, 16), free_list.pageCount());
}

test "zero-on-demand rejects invalid references and resets policy on slot reuse" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const fd = try s.createZeroOnDemandVmoFd(p0, 8192, fdRights(.{ .close = true, .resize = true, .map_read = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const slot = s.nativeVmoSlot(ref).?;
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 2) == null);
    var stale = ref;
    stale.generation += 1;
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(stale, 0) == null);
    slot.parent = stale;
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 0) == null);
    slot.parent = .{};
    slot.page_store_start ^= 1;
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 0) == null);
    const before = slot.*;
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, fd, 16384, &free_list));
    try std.testing.expectEqualDeep(before, slot.*);
    slot.page_store_start ^= 1;
    try s.closeFdWithFreeList(p0, fd, &free_list);
    s.next_native_vmo_scan = ref.index;
    const reused = try s.createNativeVmo(.anonymous, 4096);
    try std.testing.expectEqual(ref.index, reused.index);
    try std.testing.expect(!s.nativeVmoIsZeroOnDemand(reused));
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 0) == null);
    try s.retainNativeVmo(reused);
    s.releaseNativeVmoWithFreeList(reused, &free_list);
}

test "VMO grow preserves shared aliases page views and final ownership" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const base: u64 = 20 << 30;
    try free_list.appendContiguousRange(0, base, 8);
    const rights = fdRights(.{ .resize = true, .inspect = true, .close = true, .dup = true, .map_read = true, .map_write = true });
    const fd = try s.createAnonymousVmoFdWithPages(p0, 8192, rights, .{}, 16, &free_list);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    _ = try s.mmapFd(p0, fd, 0x4900_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0);
    try s.cloneFdTableForFork(p0, p1);
    try s.cloneVmaTableForFork(p0, p1);
    const view = try s.createNativePageView(ref, 1);
    try s.installNativeVmoPages(view, 0, &.{base + 4096});
    const refs_before = s.nativeVmoRefCount(ref);
    const original_entry = s.vmaEntryForVaConst(p0, 0x4900_0000).?.*;
    try s.growVmoFdWithPages(p0, fd, 16384, &free_list);
    try std.testing.expectEqual(refs_before, s.nativeVmoRefCount(ref));
    try std.testing.expectEqualDeep(original_entry, s.vmaEntryForVaConst(p0, 0x4900_0000).?.*);
    try std.testing.expectEqualDeep(original_entry, s.vmaEntryForVaConst(p1, 0x4900_0000).?.*);
    try std.testing.expectEqual(@as(u64, 16384), s.fdInfo(p1, fd).?.size_bytes);
    for (0..4) |page| try std.testing.expectEqual(@as(?u64, base + page * 4096), s.nativeVmoPagePaddr(ref, page));
    try std.testing.expectEqual(@as(?u64, base + 4096), s.nativeVmoResolvedPagePaddr(view, 0));
    try std.testing.expectEqual(@as(?u64, 4096), s.nativeVmoSize(view));
    _ = try s.mmapFd(p0, fd, 0x4a00_0000, 16384, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0);
    try std.testing.expectEqual(.ready, s.prepareNativeVmaFaultMapping(p0, 0x4a00_3000, true, false).kind);
    try std.testing.expectEqual(@as(usize, 4), free_list.pageCount());
    s.releasePrincipalNativeMemory(p0, &free_list);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(ref));
    s.releaseNativeVmoWithFreeList(view, &free_list);
    try std.testing.expect(s.nativeVmoSlot(ref) == null);
    try std.testing.expectEqual(@as(usize, 8), free_list.pageCount());
}

test "VMO grow rejects authority bad bounds and non-root objects without mutation" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x8200_0000, 4);
    const rights = fdRights(.{ .resize = true, .close = true, .dup = true, .map_read = true });
    const fd = try s.createAnonymousVmoFdWithPages(p0, 8192, rights, .{}, 16, &free_list);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const original = s.nativeVmoSlot(ref).?.*;
    for ([_]u64{ 0, 4096, 8191, 8193, std.math.maxInt(u64), (@as(u64, kernel.max_vmo_backing_pages) + 1) * 4096 }) |capacity| {
        try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, fd, capacity, &free_list));
        try std.testing.expectEqualDeep(original, s.nativeVmoSlot(ref).?.*);
    }
    try s.growVmoFdWithPages(p0, fd, 8192, &free_list);
    const no_resize = try s.dupFd(p0, fd, 0, fdRights(.{ .close = true, .map_read = true }), .{});
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, no_resize, 8192, &free_list));
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p1, fd, 16384, &free_list));
    const object = try createTestFdObject(&s, 123);
    const other_fd = try s.installFd(p0, object, rights, .{}, 16);
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, other_fd, 16384, &free_list));
    const view = try s.createNativePageView(ref, 1);
    const view_object = try s.createKernelObject(.vmo, .{ .vmo = view });
    const view_fd = try s.installFd(p0, view_object, rights, .{}, 16);
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, view_fd, 8192, &free_list));
    // A shadow has borrowed backing even though its kind is anonymous.
    s.nativeVmoSlot(ref).?.parent = view;
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, fd, 16384, &free_list));
    s.nativeVmoSlot(ref).?.parent = .{};
    try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, fd, 16384, &free_list));
}

test "zero-on-demand faults preserve backing through physical and sparse-store exhaustion" {
    for ([_]bool{ false, true }) |store_full| {
        var s = try initFdState();
        var paddrs: [2]u64 = undefined;
        var owners: [2]u64 = undefined;
        var indices: [2]u32 = undefined;
        if (store_full) try std.testing.expect(kernel.initVmoBackingPageStoreForTest(&paddrs, &owners, &indices));
        defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
        var free_list = FreePageList{};
        const physical_count: usize = if (store_full) 3 else 1;
        try free_list.appendContiguousRange(0, 20 << 30, physical_count);
        const fd = try s.createZeroOnDemandVmoFd(p0, 8192,
            fdRights(.{ .close = true, .map_read = true, .map_write = true }), .{}, 16);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        _ = try s.mmapFd(p0, fd, 0x4900_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0);
        _ = try s.mmapFd(p0, fd, 0x4a00_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .private = true }), 0);
        const first = (try s.allocPhysicalPage(&free_list)).paddr;
        try s.installNativeVmoPages(ref, 0, &.{first});
        const before = s.nativeVmoSlot(ref).?.*;
        const free_before = free_list.pageCount();
        const used_before = kernel.vmObjectBackingStoreStats().used_entries;
        const shared = s.prepareNativeVmaFaultMapping(p0, 0x4900_1000, true, false);
        const private = s.prepareNativeVmaCowMapping(p0, 0x4a00_1000, true, false);
        try std.testing.expectEqual(.allocate_zero, shared.kind);
        try std.testing.expectEqual(.allocate_zero, private.kind);
        if (store_full) {
            const candidate = (try s.allocPhysicalPage(&free_list)).paddr;
            try std.testing.expect(s.commitNativeVmaFaultMapping(p0, shared, candidate) == null);
            try std.testing.expect(s.commitNativeVmaCowMapping(p0, private, candidate, &free_list) == null);
            // The fault caller retains ownership of an unpublished candidate.
            try free_list.appendPage(0, candidate);
        } else {
            try std.testing.expectError(KernelError.OutOfFreePages, s.allocPhysicalPage(&free_list));
            try std.testing.expect(s.ensureNativeVmaCowMappingLockedSlow(p0, 0x4a00_1000, true, false, &free_list) == null);
        }
        try std.testing.expectEqualDeep(before, s.nativeVmoSlot(ref).?.*);
        try std.testing.expectEqual(@as(?u64, first), s.nativeVmoResolvedPagePaddr(ref, 0));
        try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoResolvedPagePaddrOrZero(ref, 1));
        try std.testing.expect(s.nativeVmaFaultMapping(p0, 0x4a00_1000, false, false) == null);
        try std.testing.expectEqual(free_before, free_list.pageCount());
        try std.testing.expectEqual(used_before, kernel.vmObjectBackingStoreStats().used_entries);
        s.releasePrincipalNativeMemory(p0, &free_list);
        try std.testing.expectEqual(physical_count, free_list.pageCount());
        try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    }
}

test "VMO grow rolls back physical and sparse-store exhaustion" {
    for ([_]bool{ false, true }) |store_full| {
        var s = try initFdState();
        var paddr_slots: [3]u64 = undefined;
        var owner_slots: [3]u64 = undefined;
        var index_slots: [3]u32 = undefined;
        if (store_full) try std.testing.expect(kernel.initVmoBackingPageStoreForTest(&paddr_slots, &owner_slots, &index_slots));
        defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
        var free_list = FreePageList{};
        try free_list.appendContiguousRange(0, 0x9000_0000, if (store_full) 4 else 2);
        const fd = try s.createAnonymousVmoFdWithPages(p0, 4096, fdRights(.{ .resize = true, .close = true }), .{}, 16, &free_list);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        const old = s.nativeVmoSlot(ref).?.*;
        const free_before = free_list.pageCount();
        const used_before = kernel.vmObjectBackingStoreStats().used_entries;
        try std.testing.expectError(if (store_full) KernelError.TableFull else KernelError.OutOfFreePages, s.growVmoFdWithPages(p0, fd, 3 * 4096, &free_list));
        try std.testing.expectEqualDeep(old, s.nativeVmoSlot(ref).?.*);
        try std.testing.expectEqual(free_before, free_list.pageCount());
        try std.testing.expectEqual(used_before, kernel.vmObjectBackingStoreStats().used_entries);
        try std.testing.expectEqual(@as(?u64, 0), kernel.vmoBackingPageStorePaddr(old.page_store_start, 3, 1));
        try s.growVmoFdWithPages(p0, fd, 8192, &free_list);
        try std.testing.expectEqual(@as(?u64, 0x9000_0000), s.nativeVmoPagePaddr(ref, 0));
        try s.closeFdWithFreeList(p0, fd, &free_list);
        try std.testing.expectEqual(free_before + 1, free_list.pageCount());
    }
}

test "ordinary VMO pages use high RAM while contiguous DMA keeps low RAM" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const high: u64 = (16 << 30) + 4096;
    try free_list.appendContiguousRange(0, 0x200000, 2);
    try free_list.appendContiguousRange(0, high, 2);
    const rights = fdRights(.{ .map_read = true, .map_write = true, .close = true });
    const fd = try s.createAnonymousVmoFdWithPages(p0, 8192, rights, .{}, 16, &free_list);
    const vmo = s.nativeVmoRefForFd(p0, fd).?;
    try std.testing.expectEqual(@as(?u64, high), s.nativeVmoPagePaddr(vmo, 0));
    try std.testing.expectEqual(@as(?u64, high + 4096), s.nativeVmoPagePaddr(vmo, 1));
    const dma_fd = try s.createContiguousVmoFdWithPages(p0, 8192, rights, .{}, 16, &free_list);
    const dma = s.nativeVmoRefForFd(p0, dma_fd).?;
    try std.testing.expectEqual(@as(?u64, 0x200000), s.nativeVmoPagePaddr(dma, 0));
    try std.testing.expectEqual(@as(?u64, 0x201000), s.nativeVmoPagePaddr(dma, 1));
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    try s.closeFdWithFreeList(p0, dma_fd, &free_list);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(@as(usize, 4), free_list.pageCount());
}

test "ordinary VMO allocation succeeds without low RAM and rolls back shortage" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const high: u64 = 20 << 30;
    try free_list.appendContiguousRange(0, high, 1);
    const rights = fdRights(.{ .map_read = true, .map_write = true, .close = true });
    try std.testing.expectError(KernelError.OutOfFreePages, s.createAnonymousVmoFdWithPages(p0, 8192, rights, .{}, 16, &free_list));
    try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    const fd = try s.createAnonymousVmoFdWithPages(p0, 4096, rights, .{}, 16, &free_list);
    try std.testing.expectEqual(@as(?u64, high), s.nativeVmoPagePaddr(s.nativeVmoRefForFd(p0, fd).?, 0));
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
    try std.testing.expectError(KernelError.OutOfFreePages, s.createContiguousVmoFdWithPages(p0, 4096, rights, .{}, 16, &free_list));
    try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
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

test "128 GiB anonymous reservation keeps sparse backing across splits and release" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const base: u64 = 0x200_0000_0000;
    const size: u64 = 128 << 30;
    const flags = mmapFlags(.{ .anonymous = true, .private = true, .noreserve = true });
    const prot = vmaProt(.{ .read = true, .write = true, .exec = true });
    const vmo = try s.createAnonymousVmaWithPages(p0, base, size, prot, prot, flags, &free_list);
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    const slot = s.nativeVmoSlot(vmo).?;
    const indices = [_]u32{ 0, @intCast(size / 8192), @intCast(size / 4096 - 1) };
    for (indices, 0..) |page, i|
        try kernel.setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page, slot.page_store_owner, (i + 1) * 4096);
    try s.setVmaProtRange(p0, base + size / 2, 4096, .{});
    try s.setVmaProtRange(p0, base + size / 2, 4096, prot);
    try std.testing.expectEqual(@as(u64, size / 2), s.vmaEntryForVaConst(p0, base + size / 2).?.vmo_offset);
    try s.munmapRangeWithFreeList(p0, base + size / 2, 4096, &free_list);
    try std.testing.expectEqual(@as(u64, 2), kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
    try std.testing.expectEqual(@as(?u64, 4096), s.nativeVmoPagePaddr(vmo, 0));
    try std.testing.expectEqual(@as(?u64, 12288), s.nativeVmoPagePaddr(vmo, indices[2]));
    s.releasePrincipalNativeMemory(p0, &free_list);
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expectEqual(@as(usize, 3), free_list.pageCount());
    try std.testing.expectError(KernelError.InvalidState, s.createAnonymousVmaWithPages(p0, base, size, prot, prot, mmapFlags(.{ .anonymous = true, .private = true }), &free_list));
    try std.testing.expectError(KernelError.InvalidState, s.createAnonymousVmaWithPages(p0, base, size, prot, prot, mmapFlags(.{ .anonymous = true, .shared = true, .noreserve = true }), &free_list));
    try std.testing.expectError(KernelError.InvalidState, s.createAnonymousVmoFdWithPages(p0, size, fdRights(.{ .map_read = true }), .{}, 0, &free_list));
}

test "large backing snapshots survive wrapped rehash between batches" {
    var paddrs: [8]u64 = undefined;
    var owners: [8]u64 = undefined;
    var indices: [8]u32 = undefined;
    try std.testing.expect(kernel.initVmoBackingPageStoreForTest(&paddrs, &owners, &indices));
    defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
    const owner = kernel.vmoBackingStoreOwner(.native_vmo, 1, 9);
    const count: u32 = 128 << 18;
    var colliding: [3]u32 = undefined;
    var found: usize = 0;
    var page: u32 = 0;
    while (found < colliding.len) : (page += 1) {
        if (kernel.vmoBackingPageStoreBucketForTest(owner, page) != 7) continue;
        colliding[found] = page;
        found += 1;
    }
    for (colliding, 0..) |index, i|
        try kernel.setVmoBackingPageStorePaddr(owner, count, index, owner, (i + 1) * 4096);
    var batch: [1]kernel.BackedPage = undefined;
    try std.testing.expectEqual(@as(usize, 0), kernel.snapshotVmoBackingPages(owner, count, owner + 1, 0, count, &batch));
    try std.testing.expectEqual(@as(usize, 1), kernel.snapshotVmoBackingPages(owner, count, owner, 0, count, &batch));
    try std.testing.expectEqual(colliding[0], batch[0].index);
    try kernel.setVmoBackingPageStorePaddr(owner, count, colliding[0], owner, 0);
    try std.testing.expectEqual(@as(usize, 1), kernel.snapshotVmoBackingPages(owner, count, owner, colliding[0] + 1, count, &batch));
    try std.testing.expectEqual(colliding[1], batch[0].index);
    try std.testing.expectEqual(@as(u64, 8192), batch[0].paddr);
    try std.testing.expect(kernel.freeVmoBackingPageStore(owner, count, owner));
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expectEqual(@as(usize, 0), kernel.snapshotVmoBackingPages(owner, count, owner, 0, count, &batch));
}

test "large sparse release rejects a mismatched owner before returning RAM" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const vmo = try s.createNativeVmo(.anonymous, 128 << 30);
    try s.retainNativeVmo(vmo);
    const slot = s.nativeVmoSlot(vmo).?;
    const original = slot.page_store_owner;
    try kernel.setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, 0, original, 0x1000);
    slot.page_store_owner = kernel.vmoBackingStoreOwner(.native_vmo, vmo.index, vmo.generation + 1);
    KernelState.releaseNativeVmoOwnedPages(slot, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    try std.testing.expectEqual(@as(?u64, 0x1000), s.nativeVmoPagePaddr(vmo, 0));
    slot.page_store_owner = original;
    s.releaseNativeVmoWithFreeList(vmo, &free_list);
    try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
}

test "large sparse COW detach copies only backed pages of its logical slice" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const pages: u32 = 128 << 18;
    const original = try s.createNativeCowTable(pages, &free_list);
    try s.retainNativeCowTable(original);
    try s.retainNativeCowTable(original);
    try s.setNativeCowPagePaddr(original, 0, 0x1000);
    try s.setNativeCowPagePaddr(original, pages / 2, 0x2000);
    try s.setNativeCowPagePaddr(original, pages - 1, 0x3000);
    var slice = kernel.VmaEntry{
        .active = true,
        .start_va = 0x200_0000_0000,
        .size_bytes = @as(u64, pages / 2) * 4096,
        .cow_table = original,
        .cow_page_offset = pages / 2,
    };
    var copies = FreePageList{};
    try copies.appendContiguousRange(0, 0x9000000, 2);
    try s.detachSharedEntryCowTable(&slice, &copies);
    try std.testing.expectEqual(@as(usize, 0), copies.pageCount());
    try std.testing.expectEqual(@as(?u64, 0x9000000), s.nativeCowPagePaddr(slice.cow_table, 0));
    try std.testing.expectEqual(@as(?u64, 0x9001000), s.nativeCowPagePaddr(slice.cow_table, pages / 2 - 1));
    try std.testing.expectEqual(@as(?u64, 0x2000), s.nativeCowPagePaddr(original, pages / 2));
    s.releaseNativeCowTable(slice.cow_table, &copies);
    s.releaseNativeCowTable(original, &free_list);
    try std.testing.expectEqual(@as(usize, 2), copies.pageCount());
    try std.testing.expectEqual(@as(usize, 3), free_list.pageCount());
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
}

test "sparse vmo backing store charges only instantiated pages" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const max_bytes = @as(u64, kernel.max_vmo_backing_pages) * kernel.native_page_size;

    const large_vmo = try s.createNativeVmo(.anonymous, max_bytes);
    try s.retainNativeVmo(large_vmo);
    const large_cow = try s.createNativeCowTable(kernel.max_vmo_backing_pages, &free_list);
    try s.retainNativeCowTable(large_cow);
    var stats = kernel.vmObjectBackingStoreStats();
    try std.testing.expectEqual(@as(u64, 0), stats.used_entries);

    const vmo_slot = s.nativeVmoSlot(large_vmo) orelse unreachable;
    try kernel.setVmoBackingPageStorePaddr(
        vmo_slot.page_store_start,
        vmo_slot.page_count,
        1,
        vmo_slot.page_store_owner,
        0x1234_5000,
    );
    try kernel.setVmoBackingPageStorePaddr(
        vmo_slot.page_store_start,
        vmo_slot.page_count,
        kernel.max_vmo_backing_pages - 1,
        vmo_slot.page_store_owner,
        0x5678_9000,
    );
    try s.setNativeCowPagePaddr(large_cow, 77, 0x9abc_d000);
    try std.testing.expectEqual(@as(?u64, 0x1234_5000), s.nativeVmoPagePaddr(large_vmo, 1));
    try std.testing.expectEqual(
        @as(?u64, 0x5678_9000),
        s.nativeVmoPagePaddr(large_vmo, kernel.max_vmo_backing_pages - 1),
    );
    try std.testing.expectEqual(@as(?u64, 0x9abc_d000), s.nativeCowPagePaddr(large_cow, 77));
    stats = kernel.vmObjectBackingStoreStats();
    try std.testing.expectEqual(@as(u64, 3), stats.used_entries);
    try std.testing.expectEqual(stats.capacity - 3, stats.free_entries);

    const growing = try s.createNativeVmo(.anonymous, kernel.native_page_size);
    try s.retainNativeVmo(growing);
    try std.testing.expect(s.growUniqueAnonymousVmo(growing, 63 * kernel.native_page_size));
    try std.testing.expectEqual(@as(?u64, 64 * kernel.native_page_size), s.nativeVmoSize(growing));
    try std.testing.expectEqual(@as(u64, 3), kernel.vmObjectBackingStoreStats().used_entries);

    s.releaseNativeVmo(growing);
    s.releaseNativeVmo(large_vmo);
    s.releaseNativeCowTable(large_cow, null);
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
}

test "vmo backing store operations wait for the dedicated inner lock" {
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));

    const Context = struct {
        started: *u8,
        finished: *u8,
        result: *?u64,
        owner: u64,
    };
    const Worker = struct {
        fn run(context: *Context) void {
            @atomicStore(u8, context.started, 1, .release);
            context.result.* = kernel.allocEmptyVmoBackingPageStore(1, context.owner);
            @atomicStore(u8, context.finished, 1, .release);
        }
    };

    var started: u8 = 0;
    var finished: u8 = 0;
    var result: ?u64 = null;
    var lock_held = kernel.lockVmoBackingPageStoreForTest();
    try std.testing.expect(lock_held);
    defer if (lock_held) kernel.unlockVmoBackingPageStoreForTest();

    var context = Context{
        .started = &started,
        .finished = &finished,
        .result = &result,
        .owner = kernel.vmoBackingStoreOwner(.native_vmo, 2, 1),
    };
    const worker = try std.Thread.spawn(.{}, Worker.run, .{&context});
    while (@atomicLoad(u8, &started, .acquire) == 0) std.Thread.yield() catch {};
    for (0..1024) |_| std.Thread.yield() catch {};
    const finished_while_locked = @atomicLoad(u8, &finished, .acquire) != 0;

    kernel.unlockVmoBackingPageStoreForTest();
    lock_held = false;
    worker.join();

    try std.testing.expect(!finished_while_locked);
    try std.testing.expectEqual(@as(?u64, context.owner), result);
    try std.testing.expectEqual(@as(u8, 1), @atomicLoad(u8, &finished, .acquire));
}

test "vmo backing store owner ledger rejects cross-object writes and frees" {
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));
    const owner = kernel.vmoBackingStoreOwner(.native_vmo, 3, 1);
    const intruder = kernel.vmoBackingStoreOwner(.native_cow, 4, 1);
    const start = kernel.allocEmptyVmoBackingPageStore(1, owner) orelse unreachable;

    try std.testing.expectError(
        KernelError.InvalidState,
        kernel.setVmoBackingPageStorePaddr(start, 1, 0, intruder, 0x1000),
    );
    try std.testing.expectEqual(@as(?u64, 0), kernel.vmoBackingPageStorePaddr(start, 1, 0));
    try kernel.setVmoBackingPageStorePaddr(start, 1, 0, owner, 0x1000);
    try std.testing.expect(!kernel.freeVmoBackingPageStore(start, 1, intruder));
    try std.testing.expectEqual(@as(?u64, 0x1000), kernel.vmoBackingPageStorePaddr(start, 1, 0));
    try std.testing.expect(kernel.freeVmoBackingPageStore(start, 1, owner));

    const fault = kernel.vmoBackingStoreOwnershipFault();
    try std.testing.expectEqual(@as(u64, 2), fault.count);
    try std.testing.expectEqual(@as(u8, 3), fault.operation);
    try std.testing.expectEqual(intruder, fault.expected);
    try std.testing.expectEqual(owner, fault.actual);
}

test "sparse vmo backing store keeps full generation and owner identity" {
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));
    const current = kernel.vmoBackingStoreOwner(.native_vmo, 9, 0x8000_0001);
    const stale = kernel.vmoBackingStoreOwner(.native_vmo, 9, 1);
    const cow = kernel.vmoBackingStoreOwner(.native_cow, 9, 0x8000_0001);
    try std.testing.expect(current != stale);
    try std.testing.expect(current != cow);

    const start = kernel.allocEmptyVmoBackingPageStore(8, current) orelse unreachable;
    try kernel.setVmoBackingPageStorePaddr(start, 8, 3, current, 0x1111_1000);
    try std.testing.expectError(
        KernelError.InvalidState,
        kernel.setVmoBackingPageStorePaddr(start, 8, 3, stale, 0x2222_2000),
    );
    try std.testing.expectEqual(@as(?u64, 0x1111_1000), kernel.vmoBackingPageStorePaddr(start, 8, 3));

    const cow_start = kernel.allocEmptyVmoBackingPageStore(8, cow) orelse unreachable;
    try kernel.setVmoBackingPageStorePaddr(cow_start, 8, 3, cow, 0x3333_3000);
    try std.testing.expectEqual(@as(?u64, 0x3333_3000), kernel.vmoBackingPageStorePaddr(cow_start, 8, 3));
    try std.testing.expectEqual(@as(u64, 2), kernel.vmObjectBackingStoreStats().used_entries);
}

test "sparse vmo backing store repairs wrapped collision chains" {
    var paddrs: [8]u64 = undefined;
    var owners: [8]u64 = undefined;
    var page_indices: [8]u32 = undefined;
    try std.testing.expect(kernel.initVmoBackingPageStoreForTest(&paddrs, &owners, &page_indices));
    defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;

    var colliding: [3]u64 = undefined;
    var found: usize = 0;
    var candidate_index: u32 = 1;
    while (found < colliding.len and candidate_index < 10000) : (candidate_index += 1) {
        const candidate = kernel.vmoBackingStoreOwner(.native_vmo, candidate_index, 1);
        if (kernel.vmoBackingPageStoreBucketForTest(candidate, 0) == 7) {
            colliding[found] = candidate;
            found += 1;
        }
    }
    try std.testing.expectEqual(colliding.len, found);
    for (colliding, 0..) |owner, index| {
        const start = kernel.allocEmptyVmoBackingPageStore(1, owner) orelse unreachable;
        try kernel.setVmoBackingPageStorePaddr(start, 1, 0, owner, (@as(u64, index) + 1) * 0x1000);
    }

    try std.testing.expect(kernel.freeVmoBackingPageStore(colliding[1], 1, colliding[1]));
    try std.testing.expectEqual(@as(?u64, 0x1000), kernel.vmoBackingPageStorePaddr(colliding[0], 1, 0));
    try std.testing.expectEqual(@as(?u64, 0x3000), kernel.vmoBackingPageStorePaddr(colliding[2], 1, 0));
    try std.testing.expectEqual(@as(u64, 2), kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expect(kernel.freeVmoBackingPageStore(colliding[0], 1, colliding[0]));
    try std.testing.expectEqual(@as(?u64, 0x3000), kernel.vmoBackingPageStorePaddr(colliding[2], 1, 0));
    try std.testing.expect(kernel.freeVmoBackingPageStore(colliding[2], 1, colliding[2]));
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
}

test "sparse vmo backing batch is atomic at capacity" {
    var paddr_slots: [5]u64 = undefined;
    var owner_slots: [5]u64 = undefined;
    var index_slots: [5]u32 = undefined;
    try std.testing.expect(kernel.initVmoBackingPageStoreForTest(
        &paddr_slots,
        &owner_slots,
        &index_slots,
    ));
    defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
    const owner = kernel.vmoBackingStoreOwner(.native_vmo, 5, 1);
    const start = kernel.allocEmptyVmoBackingPageStore(16, owner) orelse unreachable;
    try kernel.setVmoBackingPageStorePaddrs(
        start,
        16,
        0,
        owner,
        &[_]u64{ 0x1000, 0x2000, 0x3000 },
    );
    try std.testing.expectError(
        KernelError.TableFull,
        kernel.setVmoBackingPageStorePaddrs(
            start,
            16,
            3,
            owner,
            &[_]u64{ 0x4000, 0x5000 },
        ),
    );
    try std.testing.expectEqual(@as(?u64, 0), kernel.vmoBackingPageStorePaddr(start, 16, 3));
    try std.testing.expectEqual(@as(?u64, 0), kernel.vmoBackingPageStorePaddr(start, 16, 4));
    try kernel.setVmoBackingPageStorePaddr(start, 16, 3, owner, 0x4000);
    try std.testing.expectError(
        KernelError.TableFull,
        kernel.setVmoBackingPageStorePaddr(start, 16, 4, owner, 0x5000),
    );
    try std.testing.expectEqual(@as(?u64, 0), kernel.vmoBackingPageStorePaddr(start, 16, 15));
}

test "native vmo page install rejects a bad batch without a partial write" {
    var s = try initFdState();
    const vmo = try s.createNativeVmo(.anonymous, 2 * kernel.native_page_size);
    try s.retainNativeVmo(vmo);
    try std.testing.expectError(
        KernelError.InvalidState,
        s.installNativeVmoPages(vmo, 0, &[_]u64{ 0x1000, 0x2001 }),
    );
    try std.testing.expectEqual(@as(?u64, null), s.nativeVmoPagePaddr(vmo, 0));
    try std.testing.expectEqual(@as(?u64, null), s.nativeVmoPagePaddr(vmo, 1));
}

test "native VMO slot layout stays within existing per-slot storage" {
    try std.testing.expectEqual(@as(usize, 56), @sizeOf(kernel.NativeVmoSlot));
}

test "page view lifetime survives failure multiple closes and generation reuse" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const parent = try s.createNativeVmo(.anonymous, 8192);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{ 0x8400_0000, 0x8400_1000 });
    const slot = s.nativeVmoSlot(parent).?;
    try std.testing.expectError(KernelError.InvalidState, s.createNativePageView(parent, 0));

    // Failure to retain the parent must unwind the unlinked child.
    slot.ref_count = std.math.maxInt(u32);
    const failed_index = s.next_native_vmo_scan;
    try std.testing.expectError(KernelError.TableFull, s.createNativePageView(parent, 1));
    try std.testing.expectEqual(kernel.NativeVmoKind.none, s.native_vmos[failed_index].kind);
    slot.ref_count = 1;

    const first = try s.createNativePageView(parent, 1);
    const second = try s.createNativePageView(parent, 1);
    try s.installNativeVmoPages(first, 0, &.{0x8400_0000});
    // A later setup failure does not remove the published parent retain or
    // release borrowed pages; normal rollback releases the child.
    try std.testing.expectError(KernelError.InvalidState, s.installNativeVmoPages(second, 0, &.{0x8400_1001}));
    s.releaseUnmappedAnonymousVmoPageRange(p0, parent, 0, 2, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    s.releaseNativeVmoWithFreeList(first, &free_list);
    s.releaseUnmappedAnonymousVmoPageRange(p0, parent, 0, 2, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    s.releaseNativeVmoWithFreeList(second, &free_list);
    s.releaseUnmappedAnonymousVmoPageRange(p0, parent, 0, 1, &free_list);
    try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
    s.releaseNativeVmoWithFreeList(parent, &free_list);
    try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
    s.next_native_vmo_scan = parent.index;
    const reused = try s.createNativeVmo(.anonymous, 4096);
    try std.testing.expectEqual(parent.index, reused.index);
    try std.testing.expect(parent.generation != reused.generation);
    try s.retainNativeVmo(reused);
    s.releaseNativeVmo(reused);
}

test "page view borrows scatter pages until its last reference closes" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const parent = try s.createNativeVmo(.anonymous, 3 * kernel.native_page_size);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{ 0x8100_0000, 0x8200_0000, 0x8300_0000 });

    const view = try s.createNativePageView(parent, 2);
    try s.installNativeVmoPages(view, 0, &.{ 0x8300_0000, 0x8100_0000 });
    try std.testing.expectEqual(@as(?u32, 2), s.nativeVmoRefCount(parent));
    try std.testing.expectEqual(@as(?u64, 0x8300_0000), s.nativeVmoResolvedPagePaddr(view, 0));
    try std.testing.expectEqual(@as(?u64, 0x8100_0000), s.nativeVmoResolvedPagePaddr(view, 1));

    s.releaseUnmappedAnonymousVmoPageRange(p0, parent, 0, 3, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    try std.testing.expectEqual(@as(?u64, 0x8200_0000), s.nativeVmoResolvedPagePaddr(parent, 1));

    s.releaseNativeVmoWithFreeList(view, &free_list);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(view));
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(parent));
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    s.releaseNativeVmoWithFreeList(parent, &free_list);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(parent));
    try std.testing.expectEqual(@as(usize, 3), free_list.pageCount());
}

test "page view mappings are shared non-executable in every map path" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const parent = try s.createNativeVmo(.anonymous, kernel.native_page_size);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{0x8400_0000});
    const view = try s.createNativePageView(parent, 1);
    try s.installNativeVmoPages(view, 0, &.{0x8400_0000});
    const object = try s.createKernelObject(.vmo, .{ .vmo = view });
    const fd = try s.installFd(
        p0,
        object,
        fdRights(.{ .close = true, .map_read = true, .map_write = true }),
        .{},
        16,
    );

    try std.testing.expectError(KernelError.InvalidState, s.mmapFd(
        p0,
        fd,
        0x4300_0000,
        4096,
        vmaProt(.{ .read = true }),
        mmapFlags(.{ .private = true }),
        0,
    ));
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmapIntoProcess(
        p0,
        fd,
        p1,
        0x4400_0000,
        4096,
        vmaProt(.{ .read = true }),
        mmapFlags(.{ .fixed = true, .private = true }),
        0,
        &free_list,
    ));
    _ = try s.mmapFd(
        p0,
        fd,
        0x4300_0000,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .shared = true }),
        0,
    );
    try std.testing.expectEqual(@as(u64, 0x8400_0000), (s.nativeVmaFaultMapping(p0, 0x4300_0000, true, false) orelse unreachable).paddr);

    try s.munmapRangeWithFreeList(p0, 0x4300_0000, 4096, &free_list);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(parent));
    s.releaseNativeVmoWithFreeList(parent, &free_list);
    try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
}

test "parent revoke removes page view fds and mappings before freeing pages" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x8500_0000, 2);
    const original_free = free_list.pageCount();
    const parent_fd = try s.createAnonymousVmoFdWithPages(
        p0,
        8192,
        fdRights(.{
            .close = true,
            .share = true,
            .revoke = true,
            .map_read = true,
            .map_write = true,
        }),
        .{},
        16,
        &free_list,
    );
    const parent = s.nativeVmoRefForFd(p0, parent_fd) orelse unreachable;
    const second_page = s.nativeVmoResolvedPagePaddr(parent, 1) orelse unreachable;
    const view = try s.createNativePageView(parent, 1);
    try s.installNativeVmoPages(view, 0, &.{second_page});
    const object = try s.createKernelObject(.vmo, .{ .vmo = view });
    const view_fd = try s.installFd(
        p1,
        object,
        fdRights(.{ .close = true, .map_read = true, .map_write = true }),
        .{},
        16,
    );
    _ = try s.mmapFd(
        p1,
        view_fd,
        0x4500_0000,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .shared = true }),
        0,
    );

    _ = try s.revokeVmoFdWithFreeList(p0, parent_fd, &free_list, NoopUnmapper{});
    try std.testing.expect(s.fdEntryConst(p0, parent_fd) == null);
    try std.testing.expect(s.fdEntryConst(p1, view_fd) == null);
    try std.testing.expect(s.vmaEntryConst(p1, 0x4500_0000) == null);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(view));
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(parent));
    try std.testing.expectEqual(original_free, free_list.pageCount());
}

fn expectVmoLifetime(s: *KernelState, fd: kernel.Fd, object_refs: u32, native_refs: u32) !void {
    const expected = @as(u64, object_refs) |
        (@as(u64, native_refs) << fd_abi.vmo_info_native_refs_shift);
    try std.testing.expectEqual(@as(?u64, expected), s.fdVmoLifetimeInfo(p0, fd));
}

test "VMO lifetime snapshot covers dup mapping fork split and derived views" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const rights = fdRights(.{ .inspect = true, .dup = true, .transfer = true, .close = true, .map_read = true, .map_write = true });
    const fd = try s.createAnonymousVmoFd(p0, 12288, rights, .{}, 16);
    const parent = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    _ = try s.mmapFd(p0, fd, 0x4000_0000, 12288, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0);
    try expectVmoLifetime(&s, fd, 1, 2);
    const dup = try s.dupFd(p0, fd, 16, rights, .{});
    try expectVmoLifetime(&s, fd, 2, 2);
    try s.closeFdWithFreeList(p0, dup, &free_list);
    const child = try s.transferFd(p0, p1, fd, 16, rights, .{}, .copy);
    _ = try s.mmapFd(p1, child, 0x4100_0000, 12288, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0);
    try s.closeFdWithFreeList(p1, child, &free_list);
    try expectVmoLifetime(&s, fd, 1, 3); // Mapping survives last remote FD.
    try s.setVmaProtRange(p1, 0x4100_1000, 4096, vmaProt(.{ .read = true }));
    try expectVmoLifetime(&s, fd, 1, 5); // Three VMAs, not one.
    try s.cloneVmaTableForFork(p1, p2);
    try expectVmoLifetime(&s, fd, 1, 8);
    try s.munmapRangeWithFreeList(p1, 0x4100_0000, 12288, &free_list);
    try s.munmapRangeWithFreeList(p2, 0x4100_0000, 12288, &free_list);
    try expectVmoLifetime(&s, fd, 1, 2);
    const view = try s.createNativePageView(parent, 1);
    const object = try s.createKernelObject(.vmo, .{ .vmo = view });
    const view_fd = try s.installFd(p1, object, rights, .{}, 16);
    _ = try s.mmapFd(p1, view_fd, 0x4200_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 0);
    try expectVmoLifetime(&s, fd, 1, 3);
    try s.closeFdWithFreeList(p1, view_fd, &free_list);
    try expectVmoLifetime(&s, fd, 1, 3); // View mapping still retains parent.
    try s.munmapRangeWithFreeList(p1, 0x4200_0000, 4096, &free_list);
    try expectVmoLifetime(&s, fd, 1, 2);
    try s.munmapRangeWithFreeList(p0, 0x4000_0000, 12288, &free_list);
    try expectVmoLifetime(&s, fd, 1, 1);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expect(s.fdVmoLifetimeInfo(p0, fd) == null);
}

test "VMO lifetime snapshot includes queued IPC and requires INSPECT" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const rights = fdRights(.{ .inspect = true, .transfer = true, .dup = true, .close = true, .map_read = true });
    const fd = try s.createAnonymousVmoFd(p0, 4096, rights, .{}, 16);
    const blind = try s.dupFd(p0, fd, 16, fdRights(.{ .close = true }), .{});
    try std.testing.expect(s.fdVmoLifetimeInfo(p0, blind) == null);
    // Existing internal metadata callers do not expose an unlocked count.
    try std.testing.expectEqual(@as(u64, 0), (s.fdInfo(p0, fd) orelse unreachable).extra);
    try s.closeFdWithFreeList(p0, blind, &free_list);
    const endpoint = try s.createIpcEndpointFd(p0, fdRights(.{ .send = true, .recv = true, .close = true }), .{}, 16);
    const send_fds = [_]kernel.IpcSendFd{.{ .fd = fd, .rights = fdRights(.{ .close = true, .map_read = true }), .move = false }};
    try s.ipcSend(p0, endpoint, .{ .words = .{ 0, 0, 0, 0 }, .fds = &send_fds }, &free_list);
    try expectVmoLifetime(&s, fd, 2, 1);
    const received = try s.ipcRecv(p0, endpoint, 1, 16, &free_list);
    try expectVmoLifetime(&s, fd, 2, 1);
    try std.testing.expect(s.fdVmoLifetimeInfo(p0, received.fds[0].fd) == null);
    try s.closeFdWithFreeList(p0, received.fds[0].fd, &free_list);
    try expectVmoLifetime(&s, fd, 1, 1);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try s.closeFdWithFreeList(p0, endpoint, &free_list);
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

test "MMIO reservation admission covers whole ranges without mutating VMAs" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const base: u64 = 0x4100_0000;
    const flags = mmapFlags(.{ .private = true, .anonymous = true, .noreserve = true, .fixed = true });
    const ceiling = vmaProt(.{ .read = true, .write = true });
    _ = try s.createAnonymousVmaWithPages(p0, base, 3 * 4096, .{}, ceiling, flags, &free_list);
    _ = try s.createAnonymousVmaWithPages(p0, base + 3 * 4096, 4096, .{}, ceiling, flags, &free_list);
    const first = (s.vmaEntryForVaConst(p0, base) orelse unreachable).*;
    const second = (s.vmaEntryForVaConst(p0, base + 3 * 4096) orelse unreachable).*;

    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base, 4 * 4096));
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base + 4096, 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 5 * 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base - 4096, 2 * 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p1, base, 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base + 1, 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 1));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 0));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, 0, 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, 0xffff_ffff_ffff_f000, 4096));
    try std.testing.expectEqual(first, (s.vmaEntryForVaConst(p0, base) orelse unreachable).*);
    try std.testing.expectEqual(second, (s.vmaEntryForVaConst(p0, base + 3 * 4096) orelse unreachable).*);
    try std.testing.expectEqual(@as(usize, 0), free_list.len);
    s.releasePrincipalNativeMemory(p0, &free_list);
}

test "MMIO reservation admission rejects used NONE RAM and COW" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const base: u64 = 0x4100_0000;
    const flags = mmapFlags(.{ .private = true, .anonymous = true, .noreserve = true });
    const ceiling = vmaProt(.{ .read = true, .write = true });
    const vmo = try s.createAnonymousVmaWithPages(p0, base, 3 * 4096, .{}, ceiling, flags, &free_list);
    // Splitting a non-COW VMA also advances cow_page_offset. That inactive
    // offset is not COW authority; backing checks must use the VMO offset.
    try s.setVmaProtRange(p0, base + 4096, 4096, vmaProt(.{ .read = true }));
    try s.setVmaProtRange(p0, base + 4096, 4096, .{});
    const middle = s.vmaEntryForVaConst(p0, base + 4096) orelse unreachable;
    try std.testing.expect(middle.cow_table.isNull() and middle.cow_page_offset != 0);
    try std.testing.expectEqual(@as(u64, 4096), middle.vmo_offset);
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base, 3 * 4096));
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base + 4096, 4096));
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base + 8192, 4096));
    const entry = @constCast(s.vmaEntryForVaConst(p0, base) orelse unreachable);
    const original = entry.*;

    inline for (.{ "read", "write", "exec" }) |field| {
        @field(entry.prot, field) = true;
        try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 4096));
        entry.* = original;
    }
    inline for (.{ "anonymous", "private", "noreserve" }) |field| {
        @field(entry.flags, field) = false;
        try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 4096));
        entry.* = original;
    }
    inline for (.{ "shared", "fork_cow" }) |field| {
        @field(entry.flags, field) = true;
        try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 4096));
        entry.* = original;
    }
    entry.prot.pkey = 1;
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 4096));
    entry.* = original;
    entry.cow_table = .{ .index = 0, .generation = 1 };
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 4096));
    entry.* = original;
    const slot = s.nativeVmoSlot(vmo) orelse unreachable;
    slot.parent = vmo;
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 4096));
    slot.parent = .{};
    try kernel.setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, 1, slot.page_store_owner, 0x1234_5000);
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 3 * 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base + 4096, 4096));
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base, 4096));
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base + 2 * 4096, 4096));
    try std.testing.expectEqual(@as(?u64, 0x1234_5000), s.nativeVmoPagePaddr(vmo, 1));
    try kernel.setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, 1, slot.page_store_owner, 0);
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base, 3 * 4096));
    try s.cloneVmaTableForFork(p0, p1);
    s.markForkCowVmasCommitted(p0);
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p1, base, 4096));
    s.releasePrincipalNativeMemory(p1, &free_list);
    s.releasePrincipalNativeMemory(p0, &free_list);
}

test "MMIO reservation admission rejects address-bound MMIO and DMA" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const base: u64 = 0x4100_0000;
    _ = try s.createAnonymousVmaWithPages(p0, base, 4 * 4096, .{}, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .private = true, .anonymous = true, .noreserve = true }), &free_list);
    _ = try s.createMmioRegionFd(p0, .{
        .device = 1,
        .paddr = 0x8000_0000,
        .user_va = base,
        .size = 4096,
    }, fdRights(.{ .close = true }), .{}, 16);
    _ = try s.createDmaBufferFd(p0, .{
        .device = 1,
        .user_va = base + 4096,
        .iova = 0x8001_0000,
        .size = 4096,
    }, fdRights(.{ .close = true }), .{}, 16);
    _ = try s.createDmaMappingFd(p0, .{
        .device = 1,
        .user_va = base + 8192,
        .iova = 0x8002_0000,
        .size = 4096,
        .page_count = 1,
        .direction = .to_device,
    }, fdRights(.{ .close = true }), .{}, 16);
    for (0..3) |page|
        try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base + page * 4096, 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 4 * 4096));
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base + 3 * 4096, 4096));
}

test "mmap and mprotect preserve combined write execute permissions" {
    var s = try initFdState();
    const fd = try s.createAnonymousVmoFd(
        p0,
        4096,
        fdRights(.{
            .map_read = true,
            .map_write = true,
            .map_exec = true,
        }),
        .{},
        0,
    );
    const rw = vmaProt(.{ .read = true, .write = true });
    const rwx = vmaProt(.{ .read = true, .write = true, .exec = true });
    _ = try s.mmapFd(
        p0,
        fd,
        0x4108_0000,
        4096,
        rwx,
        mmapFlags(.{ .private = true, .anonymous = true }),
        0,
    );
    try s.setVmaProtRange(p0, 0x4108_0000, 4096, rw);
    try s.setVmaProtRange(p0, 0x4108_0000, 4096, rwx);
    const mapped = s.vmaEntryConst(p0, 0x4108_0000) orelse unreachable;
    try std.testing.expect(mapped.prot.read);
    try std.testing.expect(mapped.prot.write);
    try std.testing.expect(mapped.prot.exec);
    try std.testing.expect(KernelState.vmaProtAllowedByMax(
        mapped.prot,
        mapped.max_prot,
    ));
}

test "mprotect cannot exceed the mapping authority or change its pkey" {
    var s = try initFdState();
    const read_fd = try s.createAnonymousVmoFd(
        p0,
        4096,
        fdRights(.{ .map_read = true }),
        .{},
        0,
    );
    _ = try s.mmapFd(
        p0,
        read_fd,
        0x4110_0000,
        4096,
        vmaProt(.{ .read = true }),
        mmapFlags(.{ .shared = true }),
        0,
    );
    const read_vma = s.vmaEntryConst(p0, 0x4110_0000) orelse unreachable;
    try std.testing.expect(read_vma.max_prot.read);
    try std.testing.expect(!read_vma.max_prot.write);
    try std.testing.expect(!read_vma.max_prot.exec);
    try std.testing.expectError(
        KernelError.InvalidState,
        s.setVmaProtRange(
            p0,
            0x4110_0000,
            4096,
            vmaProt(.{ .read = true, .write = true }),
        ),
    );
    try std.testing.expectError(
        KernelError.InvalidState,
        s.setVmaProtRange(
            p0,
            0x4110_0000,
            4096,
            vmaProt(.{ .read = true, .exec = true }),
        ),
    );
    try std.testing.expectEqual(
        vmaProt(.{ .read = true }),
        (s.vmaEntryConst(p0, 0x4110_0000) orelse unreachable).prot,
    );

    const keyed_fd = try s.createAnonymousVmoFd(
        p0,
        4096,
        fdRights(.{ .map_read = true, .map_write = true }),
        .{},
        0,
    );
    _ = try s.mmapFd(
        p0,
        keyed_fd,
        0x4120_0000,
        4096,
        vmaProt(.{ .read = true, .write = true, .pkey = 7 }),
        mmapFlags(.{ .private = true, .pkey = 7 }),
        0,
    );
    try s.setVmaProtRange(
        p0,
        0x4120_0000,
        4096,
        vmaProt(.{ .read = true, .pkey = 7 }),
    );
    try std.testing.expectError(
        KernelError.InvalidState,
        s.setVmaProtRange(
            p0,
            0x4120_0000,
            4096,
            vmaProt(.{ .read = true, .write = true, .pkey = 0 }),
        ),
    );
    const keyed_vma = s.vmaEntryConst(p0, 0x4120_0000) orelse unreachable;
    try std.testing.expectEqual(@as(u4, 7), keyed_vma.prot.pkey);
    try std.testing.expect(!keyed_vma.prot.write);

    const transition_fd = try s.createAnonymousVmoFd(
        p0,
        4096,
        fdRights(.{ .map_read = true, .map_write = true, .map_exec = true }),
        .{},
        0,
    );
    _ = try s.mmapFd(
        p0,
        transition_fd,
        0x4130_0000,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true }),
        0,
    );
    try s.setVmaProtRange(
        p0,
        0x4130_0000,
        4096,
        vmaProt(.{ .read = true, .exec = true }),
    );
    try s.setVmaProtRange(
        p0,
        0x4130_0000,
        4096,
        vmaProt(.{ .read = true, .write = true, .exec = true }),
    );
    const transition_vma = s.vmaEntryConst(p0, 0x4130_0000) orelse unreachable;
    try std.testing.expect(transition_vma.prot.read);
    try std.testing.expect(transition_vma.prot.write);
    try std.testing.expect(transition_vma.prot.exec);
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

test "munmap visits unsorted active slots despite removal swaps" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const base: u64 = 0x4210_0000;
    var refs: [3]kernel.NativeVmoRef = undefined;
    for (&refs, 0..) |*ref, i| {
        ref.* = try s.createAnonymousVmaWithPages(p0, base + i * 8192, 4096, vmaProt(.{ .read = true }), vmaProt(.{ .read = true }), mmapFlags(.{ .private = true, .anonymous = true }), &free_list);
    }
    try s.munmapRangeWithFreeList(p0, base, 4096, &free_list);
    const table = s.getVmaTable(p0) orelse unreachable;
    try std.testing.expect(table.active_indices[0] > table.active_indices[1]);
    var prepared = try s.prepareMunmapRangeWithFreeList(p0, base + 8192, 12288, &free_list);
    s.commitMunmapPrepared(&prepared, &free_list);
    try std.testing.expectEqual(@as(usize, 0), table.active_count);
    for (refs) |ref| try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(ref));
}

test "munmap snapshot preserves middle split before and after source slot" {
    for ([_]bool{ false, true }) |prepared_path| {
        for ([_]bool{ false, true }) |earlier_suffix| {
            var s = try initFdState();
            var free_list = FreePageList{};
            const base: u64 = 0x4220_0000;
            if (earlier_suffix) {
                _ = try s.createAnonymousVmaWithPages(p0, base - 8192, 4096, vmaProt(.{ .read = true }), vmaProt(.{ .read = true }), mmapFlags(.{ .private = true, .anonymous = true }), &free_list);
            }
            const ref = try s.createAnonymousVmaWithPages(p0, base, 12288, vmaProt(.{ .read = true }), vmaProt(.{ .read = true }), mmapFlags(.{ .private = true, .anonymous = true }), &free_list);
            if (earlier_suffix) try s.munmapRangeWithFreeList(p0, base - 8192, 4096, &free_list);
            if (prepared_path) {
                var prepared = try s.prepareMunmapRangeWithFreeList(p0, base + 4096, 4096, &free_list);
                s.commitMunmapPrepared(&prepared, &free_list);
            } else {
                try s.munmapRangeWithFreeList(p0, base + 4096, 4096, &free_list);
            }
            const table = s.getVmaTable(p0) orelse unreachable;
            try std.testing.expectEqual(@as(usize, 2), table.active_count);
            try std.testing.expectEqual(@as(u64, 4096), (s.vmaEntryConst(p0, base) orelse unreachable).size_bytes);
            try std.testing.expect(s.vmaEntryConst(p0, base + 4096) == null);
            try std.testing.expectEqual(@as(u64, 8192), (s.vmaEntryConst(p0, base + 8192) orelse unreachable).vmo_offset);
            try std.testing.expectEqual(@as(?u32, 2), s.nativeVmoRefCount(ref));
            try s.munmapRangeWithFreeList(p0, base, 12288, &free_list);
            try std.testing.expectEqual(@as(usize, 0), table.active_count);
            try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(ref));
        }
    }
}

test "munmap full table middle split failures preserve original range" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const base: u64 = 0x4230_0000;
    const ref = try s.createAnonymousVmaWithPages(p0, base, 12288, vmaProt(.{ .read = true }), vmaProt(.{ .read = true }), mmapFlags(.{ .private = true, .anonymous = true }), &free_list);
    const table = s.getVmaTable(p0) orelse unreachable;
    fillVmaTableExcept(table, &.{});
    try std.testing.expectError(KernelError.TableFull, s.prepareMunmapRangeWithFreeList(p0, base + 4096, 4096, &free_list));
    try std.testing.expectError(KernelError.TableFull, s.munmapRangeWithFreeList(p0, base + 4096, 4096, &free_list));
    try std.testing.expectEqual(kernel.max_vmas_per_process, table.active_count);
    try std.testing.expectEqual(@as(u64, 12288), (s.vmaEntryConst(p0, base) orelse unreachable).size_bytes);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(ref));
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

test "adjacent private anonymous mappings grow one unique vma past the fixed table capacity" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const base_va: u64 = 0x4828_0000;
    const mapping_count = kernel.max_vmas_per_process + 32;
    var first_vmo: ?kernel.NativeVmoRef = null;

    var index: usize = 0;
    while (index < mapping_count) : (index += 1) {
        const vmo = try s.createAnonymousVmaWithPages(
            p0,
            base_va + @as(u64, @intCast(index)) * 4096,
            4096,
            vmaProt(.{ .read = true, .write = true }),
            vmaProt(.{ .read = true, .write = true }),
            mmapFlags(.{ .private = true, .anonymous = true }),
            &free_list,
        );
        if (first_vmo) |expected| {
            try std.testing.expect(kernel.KernelState.nativeVmoRefsEqual(expected, vmo));
        } else {
            first_vmo = vmo;
        }
    }

    const table = s.getVmaTable(p0) orelse unreachable;
    try std.testing.expectEqual(@as(usize, 1), table.active_count);
    const entry = s.vmaEntryConst(p0, base_va) orelse unreachable;
    try std.testing.expectEqual(@as(u64, mapping_count * 4096), entry.size_bytes);
    try std.testing.expectEqual(
        @as(?u64, mapping_count * 4096),
        s.nativeVmoSize(first_vmo orelse unreachable),
    );

    try s.munmapRangeWithFreeList(
        p0,
        base_va,
        @as(u64, mapping_count * 4096),
        &free_list,
    );
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(first_vmo orelse unreachable));
}

test "anonymous vma metadata relocation preserves populated pages" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x38_0000, 4);
    const original_free = free_list.len;
    const base_va: u64 = 0x4829_0000;
    const blocker_va: u64 = 0x4929_0000;

    const first_vmo = try s.createAnonymousVmaWithPages(
        p0,
        base_va,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    const page = try s.allocPhysicalPage(&free_list);
    try s.installNativeVmoPages(first_vmo, 0, &[_]u64{page.paddr});

    _ = try s.createAnonymousVmaWithPages(
        p0,
        blocker_va,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    const grown_vmo = try s.createAnonymousVmaWithPages(
        p0,
        base_va + 4096,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    try std.testing.expect(kernel.KernelState.nativeVmoRefsEqual(first_vmo, grown_vmo));
    try std.testing.expectEqual(@as(?u64, page.paddr), s.nativeVmoPagePaddr(first_vmo, 0));
    try std.testing.expectEqual(@as(?u64, null), s.nativeVmoPagePaddr(first_vmo, 1));

    s.releasePrincipalNativeMemory(p0, &free_list);
    try std.testing.expectEqual(original_free, free_list.len);
}

test "anonymous vma growth rejects fork aliases and incompatible protection" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const base_va: u64 = 0x482a_0000;
    const first_vmo = try s.createAnonymousVmaWithPages(
        p0,
        base_va,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    try s.cloneVmaTableForFork(p0, p1);
    const aliased_vmo = try s.createAnonymousVmaWithPages(
        p0,
        base_va + 4096,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    try std.testing.expect(!kernel.KernelState.nativeVmoRefsEqual(first_vmo, aliased_vmo));

    const protected_vmo = try s.createAnonymousVmaWithPages(
        p0,
        base_va + 8192,
        4096,
        vmaProt(.{ .read = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    try std.testing.expect(!kernel.KernelState.nativeVmoRefsEqual(aliased_vmo, protected_vmo));
    try std.testing.expectEqual(@as(usize, 3), (s.getVmaTable(p0) orelse unreachable).active_count);

    s.releasePrincipalNativeMemory(p1, &free_list);
    s.releasePrincipalNativeMemory(p0, &free_list);
}

test "fork keeps PROT_NONE private mappings COW when mprotect enables writes" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x35_0000, 16);
    const original_free = free_list.len;
    const arena_va: u64 = 0x4830_0000;

    _ = try s.createAnonymousVmaWithPages(
        p0,
        arena_va,
        8192,
        vmaProt(.{}),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    try s.cloneVmaTableForFork(p0, p1);

    const parent_after_fork = s.vmaEntryConst(p0, arena_va) orelse unreachable;
    const child_after_fork = s.vmaEntryConst(p1, arena_va) orelse unreachable;
    try std.testing.expect(!parent_after_fork.flags.fork_cow);
    try std.testing.expect(child_after_fork.flags.fork_cow);
    try std.testing.expect(!parent_after_fork.prot.write);
    try std.testing.expect(!child_after_fork.prot.write);

    s.markForkCowVmasCommitted(p0);
    try std.testing.expect((s.vmaEntryConst(p0, arena_va) orelse unreachable).flags.fork_cow);

    const child_before = child_after_fork.*;
    const child_count = s.getVmaTable(p1).?.active_count;
    const vmo_refs = s.nativeVmoRefCount(child_before.vmo);
    const cow_refs: ?u32 = if (s.nativeCowTableSlotConst(child_before.cow_table)) |cow| cow.ref_count else null;
    try s.setVmaProtRange(p1, arena_va + 4096, 4096, vmaProt(.{}));
    try std.testing.expectEqualDeep(child_before, child_after_fork.*);
    try std.testing.expectEqual(child_count, s.getVmaTable(p1).?.active_count);
    try std.testing.expectEqual(vmo_refs, s.nativeVmoRefCount(child_before.vmo));
    try std.testing.expectEqual(cow_refs, if (s.nativeCowTableSlotConst(child_before.cow_table)) |cow| @as(?u32, cow.ref_count) else null);

    try s.setVmaProtRange(p1, arena_va, 4096, vmaProt(.{ .read = true, .write = true }));
    const child_enabled = s.vmaEntryConst(p1, arena_va) orelse unreachable;
    try std.testing.expect(child_enabled.flags.fork_cow);
    try std.testing.expect(child_enabled.prot.write);
    try std.testing.expect(!KernelState.nativeFaultMappingProt(child_enabled).write);

    const child_write = s.ensureNativeVmaCowMappingLockedSlow(
        p1,
        arena_va,
        true,
        false,
        &free_list,
    ) orelse unreachable;
    try std.testing.expect(child_write.prot.write);
    const child_dirty = (s.vmaEntryConst(p1, arena_va) orelse unreachable).*;
    const dirty_refs = s.nativeCowTableSlotConst(child_dirty.cow_table).?.ref_count;
    try s.setVmaProtRange(p1, arena_va, 4096, child_dirty.prot);
    try std.testing.expectEqualDeep(child_dirty, (s.vmaEntryConst(p1, arena_va) orelse unreachable).*);
    try std.testing.expectEqual(dirty_refs, s.nativeCowTableSlotConst(child_dirty.cow_table).?.ref_count);
    try std.testing.expectEqual(
        @as(?u64, child_write.paddr),
        s.entryDirtyPagePaddr(s.vmaEntryConst(p1, arena_va) orelse unreachable, arena_va),
    );
    try std.testing.expectEqual(
        @as(?u64, null),
        s.entryDirtyPagePaddr(s.vmaEntryConst(p0, arena_va) orelse unreachable, arena_va),
    );

    try s.setVmaProtRange(p0, arena_va, 4096, vmaProt(.{ .read = true, .write = true }));
    const parent_write = s.ensureNativeVmaCowMappingLockedSlow(
        p0,
        arena_va,
        true,
        false,
        &free_list,
    ) orelse unreachable;
    try std.testing.expect(parent_write.paddr != child_write.paddr);

    s.releasePrincipalNativeMemory(p1, &free_list);
    s.releasePrincipalNativeMemory(p0, &free_list);
    try std.testing.expectEqual(original_free, free_list.len);
}

test "mprotect middle split fails atomically when only one VMA slot is free" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const mapping_va: u64 = 0x4840_0000;
    const vmo = try s.createAnonymousVmaWithPages(
        p0,
        mapping_va,
        12288,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    const table = s.getVmaTable(p0) orelse unreachable;
    const source_index: usize = @intCast(table.active_indices[0]);
    const free_index: usize = if (source_index == 0) 1 else 0;

    var index: usize = 0;
    while (index < table.entries.len) : (index += 1) {
        if (index == source_index or index == free_index) continue;
        table.entries[index] = .{
            .active = true,
            .start_va = 0x7000_0000 + @as(u64, @intCast(index)) * 0x1000,
            .size_bytes = 4096,
        };
        table.active_indices[table.active_count] = @intCast(index);
        table.active_count += 1;
    }
    try std.testing.expectEqual(kernel.max_vmas_per_process - 1, table.active_count);
    const original = table.entries[source_index];
    const original_refs = s.nativeVmoRefCount(vmo);

    try s.setVmaProtRange(p0, mapping_va + 4096, 4096, original.prot);
    try std.testing.expectEqual(original, table.entries[source_index]);
    try std.testing.expectEqual(kernel.max_vmas_per_process - 1, table.active_count);
    try std.testing.expectEqual(original_refs, s.nativeVmoRefCount(vmo));

    try std.testing.expectError(
        KernelError.TableFull,
        s.setVmaProtRange(
            p0,
            mapping_va + 4096,
            4096,
            vmaProt(.{ .read = true }),
        ),
    );
    try std.testing.expectEqual(original, table.entries[source_index]);
    try std.testing.expect(!table.entries[free_index].active);
    try std.testing.expectEqual(kernel.max_vmas_per_process - 1, table.active_count);
    try std.testing.expectEqual(original_refs, s.nativeVmoRefCount(vmo));
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

fn returnVmoPagesForTest(s: *KernelState, vmo: kernel.NativeVmoRef, free_list: *FreePageList, partial: bool) void {
    if (partial) {
        s.releaseUnmappedAnonymousVmoPageRange(p0, vmo, 0, 2, free_list);
    } else {
        // Inspect the owned-page helper before final release clears the slot.
        KernelState.releaseNativeVmoOwnedPages(s.nativeVmoSlot(vmo).?, free_list);
    }
}

test "VMO page return clears only successfully returned pages" {
    for ([_]bool{ false, true }) |partial| {
        var s = try initFdState();
        var free_list = FreePageList{};
        const vmo = try s.createNativeVmo(.anonymous, 8192);
        try s.retainNativeVmo(vmo);
        const pages = [_]u64{ 0x8000_0000, 0x8000_1000 };
        try s.installNativeVmoPages(vmo, 0, &pages);
        returnVmoPagesForTest(&s, vmo, &free_list, partial);
        try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
        try std.testing.expectEqual(@as(usize, 1), free_list.rangeCount());
        try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, 0));
        try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, 1));
        try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(vmo));
        s.releaseNativeVmoWithFreeList(vmo, &free_list);
        try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo));
        try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
    }
}

test "VMO page return preserves failed page and continues with full-list merge" {
    for ([_]bool{ false, true }) |partial| {
        var s = try initFdState();
        var free_list = FreePageList{};
        // Construct a valid maximally fragmented fixture directly, avoiding
        // quadratic append work merely to fill the test's range table.
        for (&free_list.ranges, 0..) |*range, index| {
            range.* = .{ .region_id = 0, .len = 1, .physical_start = 0x100_0000 + @as(u64, @intCast(index)) * 8192 };
        }
        free_list.range_len = FreePageList.max_ranges;
        free_list.len = FreePageList.max_ranges;
        const last = FreePageList.max_ranges - 1;
        const pages = [_]u64{ 0x1_0000_0000, free_list.ranges[last].physical_start + 4096 };
        const vmo = try s.createNativeVmo(.anonymous, 8192);
        try s.retainNativeVmo(vmo);
        try s.installNativeVmoPages(vmo, 0, &pages);
        try std.testing.expectError(KernelError.TooManyFreeRanges, free_list.appendPage(0, pages[0]));
        try std.testing.expectEqual(@as(usize, FreePageList.max_ranges), free_list.pageCount());

        returnVmoPagesForTest(&s, vmo, &free_list, partial);
        try std.testing.expectEqual(@as(?u64, pages[0]), s.nativeVmoPagePaddrOrHole(vmo, 0));
        try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, 1));
        try std.testing.expectEqual(@as(usize, FreePageList.max_ranges + 1), free_list.pageCount());
        try std.testing.expectEqual(@as(usize, FreePageList.max_ranges), free_list.rangeCount());
        try std.testing.expectEqual(@as(usize, 2), free_list.ranges[last].len);
        try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(vmo));

        // Capacity becomes available later: retry must return the held page
        // exactly once and must not return the already-cleared second page.
        _ = try free_list.popFront();
        returnVmoPagesForTest(&s, vmo, &free_list, partial);
        try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, 0));
        try std.testing.expectEqual(@as(usize, FreePageList.max_ranges + 1), free_list.pageCount());
        s.releaseNativeVmoWithFreeList(vmo, &free_list);
    }
}

test "VMO page return rejects duplicate without clearing its backing pointer" {
    for ([_]bool{ false, true }) |partial| {
        var s = try initFdState();
        var free_list = FreePageList{};
        const pages = [_]u64{ 0x8100_0000, 0x8100_1000 };
        const vmo = try s.createNativeVmo(.anonymous, 8192);
        try s.retainNativeVmo(vmo);
        try s.installNativeVmoPages(vmo, 0, &pages);
        // Deliberately inconsistent ownership tests the defensive failure;
        // production callers must never return an already-free physical page.
        try free_list.appendPage(0, pages[0]);
        returnVmoPagesForTest(&s, vmo, &free_list, partial);
        try std.testing.expectEqual(@as(?u64, pages[0]), s.nativeVmoPagePaddrOrHole(vmo, 0));
        try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, 1));
        try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
        try std.testing.expectEqual(@as(usize, 1), free_list.rangeCount());
        s.releaseNativeVmo(vmo);
    }
}

test "VMO return batches bounded physical runs across snapshot boundaries" {
    for ([_]bool{ false, true }) |partial| {
        var s = try initFdState();
        var free_list = FreePageList{};
        var pages: [130]u64 = undefined;
        for (&pages, 0..) |*page, i| {
            // Two ascending runs, then descending/noncontiguous singletons.
            page.* = if (i < 65) 0x8200_0000 + i * 4096 else if (i < 128) 0x8300_0000 + (i - 65) * 4096 else 0x8400_0000 - (i - 128) * 8192;
        }
        const vmo = try s.createNativeVmo(.anonymous, pages.len * 4096);
        try s.retainNativeVmo(vmo);
        try s.installNativeVmoPages(vmo, 0, &pages);
        if (partial) {
            s.releaseUnmappedAnonymousVmoPageRange(p0, vmo, 0, 129, &free_list);
            try std.testing.expectEqual(@as(usize, 129), free_list.pageCount());
            try std.testing.expectEqual(@as(?u64, pages[129]), s.nativeVmoPagePaddrOrHole(vmo, 129));
        }
        s.releaseNativeVmoWithFreeList(vmo, &free_list);
        try std.testing.expectEqual(@as(usize, pages.len), free_list.pageCount());
        try std.testing.expectEqual(@as(usize, 4), free_list.rangeCount());
        for (pages) |paddr| try std.testing.expectError(KernelError.InvalidState, free_list.appendPage(0, paddr));
        try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo));
    }
}

test "VMO run return joins upper neighbor even with full range table" {
    var s = try initFdState();
    var free_list = FreePageList{};
    for (&free_list.ranges, 0..) |*range, i| {
        range.* = .{ .region_id = 0, .len = 1, .physical_start = 0x100_0000 + @as(u64, @intCast(i)) * 8192 };
    }
    free_list.range_len = FreePageList.max_ranges;
    free_list.len = FreePageList.max_ranges;
    const last = FreePageList.max_ranges - 1;
    free_list.ranges[last].physical_start = 0x8500_2000;
    const pages = [_]u64{ 0x8500_0000, 0x8500_1000 };
    const vmo = try s.createNativeVmo(.anonymous, 8192);
    try s.retainNativeVmo(vmo);
    try s.installNativeVmoPages(vmo, 0, &pages);
    // Unlike the old sequential path, the complete run can join the upper
    // neighbor without first needing another free-range slot.
    try std.testing.expectError(KernelError.TooManyFreeRanges, free_list.appendPage(0, pages[0]));
    returnVmoPagesForTest(&s, vmo, &free_list, true);
    try std.testing.expectEqual(@as(usize, FreePageList.max_ranges + 2), free_list.pageCount());
    try std.testing.expectEqual(@as(usize, FreePageList.max_ranges), free_list.rangeCount());
    try std.testing.expectEqual(@as(usize, 3), free_list.ranges[last].len);
    for (0..2) |i| try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, @intCast(i)));
    s.releaseNativeVmoWithFreeList(vmo, &free_list);
}

test "VMO run return preserves full-table failure and supports retry" {
    var s = try initFdState();
    var free_list = FreePageList{};
    for (&free_list.ranges, 0..) |*range, i| {
        range.* = .{ .region_id = 0, .len = 1, .physical_start = 0x100_0000 + @as(u64, @intCast(i)) * 8192 };
    }
    free_list.range_len = FreePageList.max_ranges;
    free_list.len = FreePageList.max_ranges;
    const pages = [_]u64{ 0x8600_0000, 0x8600_1000 };
    const vmo = try s.createNativeVmo(.anonymous, 8192);
    try s.retainNativeVmo(vmo);
    try s.installNativeVmoPages(vmo, 0, &pages);
    returnVmoPagesForTest(&s, vmo, &free_list, true);
    for (pages, 0..) |page, i| try std.testing.expectEqual(@as(?u64, page), s.nativeVmoPagePaddrOrHole(vmo, @intCast(i)));
    try std.testing.expectEqual(@as(usize, FreePageList.max_ranges), free_list.pageCount());
    _ = try free_list.popFront();
    returnVmoPagesForTest(&s, vmo, &free_list, true);
    for (0..2) |i| try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, @intCast(i)));
    try std.testing.expectEqual(@as(usize, FreePageList.max_ranges + 1), free_list.pageCount());
    s.releaseNativeVmoWithFreeList(vmo, &free_list);
}

test "COW bounded run return preserves outer pages and null-list behavior" {
    for ([_]bool{ false, true }) |use_free_list| {
        var s = try initFdState();
        var free_list = FreePageList{};
        const cow = try s.createNativeCowTable(68, &free_list);
        try s.retainNativeCowTable(cow);
        for (0..68) |i| try s.setNativeCowPagePaddr(cow, @intCast(i), 0x8700_0000 + i * 4096);
        s.clearNativeCowPageSlots(cow, 1, 66, if (use_free_list) &free_list else null);
        for (1..67) |i| try std.testing.expectEqual(@as(?u64, null), s.nativeCowPagePaddr(cow, @intCast(i)));
        try std.testing.expectEqual(@as(?u64, 0x8700_0000), s.nativeCowPagePaddr(cow, 0));
        try std.testing.expectEqual(@as(?u64, 0x8704_3000), s.nativeCowPagePaddr(cow, 67));
        try std.testing.expectEqual(@as(usize, if (use_free_list) 66 else 0), free_list.pageCount());
        s.releaseNativeCowTable(cow, if (use_free_list) &free_list else null);
        try std.testing.expectEqual(@as(usize, if (use_free_list) 68 else 0), free_list.pageCount());
        try std.testing.expectEqual(@as(usize, if (use_free_list) 1 else 0), free_list.rangeCount());
        try std.testing.expect(s.nativeCowTableSlot(cow) == null);
    }
}

test "sole VMA page release validates owner range and head tail trims" {
    for ([_]u64{ 0, 4096 }) |offset| {
        for ([_]bool{ false, true }) |head| {
            var s = try initFdState();
            var free_list = FreePageList{};
            const base: u64 = 0x4680_0000;
            const pages = [_]u64{ 0x8500_0000, 0x8500_1000, 0x8500_2000, 0x8500_3000 };
            const fd = try s.createAnonymousVmoFd(p0, 16384, fdRights(.{ .map_read = true, .close = true }), .{}, 0);
            const ref = s.nativeVmoRefForFd(p0, fd).?;
            try s.installNativeVmoPages(ref, 0, &pages);
            _ = try s.mmapFd(p0, fd, base, 12288, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), offset);
            try s.closeFdWithFreeList(p0, fd, &free_list);
            try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(ref));
            const first: usize = @intCast(offset / 4096);
            // Refcount one must not allow an overlapping or wrong-owner
            // request to return pages. Invalid ranges are no-ops too.
            s.releaseUnmappedAnonymousVmoPageRange(p0, ref, first, 1, &free_list);
            s.releaseUnmappedAnonymousVmoPageRange(p1, ref, first, 1, &free_list);
            s.releaseUnmappedAnonymousVmoPageRange(p0, ref, 4, 1, &free_list);
            s.releaseUnmappedAnonymousVmoPageRange(p0, ref, 1, std.math.maxInt(usize), &free_list);
            s.releaseUnmappedAnonymousVmoPageRange(p0, ref, 0, 0, &free_list);
            var stale = ref;
            stale.generation += 1;
            s.releaseUnmappedAnonymousVmoPageRange(p0, stale, first, 1, &free_list);
            try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
            const cut_va = base + @as(u64, if (head) 0 else 8192);
            const cut_page = first + @as(usize, if (head) 0 else 2);
            var prepared = try s.prepareMunmapRangeWithFreeList(p0, cut_va, 4096, &free_list);
            s.commitMunmapPrepared(&prepared, &free_list);
            try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
            try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(ref, cut_page));
            for (pages, 0..) |page, i| {
                if (i != cut_page) try std.testing.expectEqual(@as(?u64, page), s.nativeVmoPagePaddr(ref, i));
            }
            s.releaseUnmappedAnonymousVmoPageRange(p0, ref, cut_page, 1, &free_list);
            try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
            try s.munmapRangeWithFreeList(p0, base, 12288, &free_list);
            try std.testing.expectEqual(@as(usize, 4), free_list.pageCount());
            try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(ref));
        }
    }
}

test "sole VMA optimization preserves FD foreign VMA and page view aliases" {
    const Mode = enum { fd_only, fd_and_vma, foreign_vma, local_vma, page_view, page_view_only };
    for (std.meta.tags(Mode)) |mode| {
        var s = try initFdState();
        var free_list = FreePageList{};
        const base: u64 = 0x4690_0000;
        const pages = [_]u64{ 0x8600_0000, 0x8600_1000, 0x8600_2000 };
        const fd = try s.createAnonymousVmoFd(p0, 12288, fdRights(.{ .map_read = true, .close = true, .transfer = true }), .{}, 0);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        try s.installNativeVmoPages(ref, 0, &pages);
        const has_vma = mode != .fd_only and mode != .page_view_only;
        if (has_vma) _ = try s.mmapFd(p0, fd, base, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 4096);
        if (mode == .foreign_vma) {
            // A second owner obtains its VMA through an actual transferred FD.
            const other_fd = try s.transferFd(p0, p1, fd, 16, fdRights(.{ .map_read = true, .close = true }), .{}, .copy);
            _ = try s.mmapFd(p1, other_fd, base + 8192, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);
            try s.closeFdWithFreeList(p1, other_fd, &free_list);
        }
        if (mode == .local_vma) _ = try s.mmapFd(p0, fd, base + 8192, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);
        var view: ?kernel.NativeVmoRef = null;
        if (mode == .page_view or mode == .page_view_only) {
            view = try s.createNativePageView(ref, 1);
            try s.installNativeVmoPages(view.?, 0, pages[0..1]);
        }
        const keep_fd = mode == .fd_only or mode == .fd_and_vma;
        if (!keep_fd) try s.closeFdWithFreeList(p0, fd, &free_list);
        if (mode == .fd_only or mode == .page_view_only)
            try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(ref));
        // The owner's own VMA, when present, excludes page 0. Each mode
        // supplies a different external retain that must keep that page live.
        s.releaseUnmappedAnonymousVmoPageRange(p0, ref, 0, 1, &free_list);
        try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
        try std.testing.expectEqual(@as(?u64, pages[0]), s.nativeVmoPagePaddr(ref, 0));
        if (has_vma) try s.munmapRangeWithFreeList(p0, base, 12288, &free_list);
        if (mode == .foreign_vma) try s.munmapRangeWithFreeList(p1, base + 8192, 4096, &free_list);
        if (view) |v| s.releaseNativeVmoWithFreeList(v, &free_list);
        if (keep_fd) try s.closeFdWithFreeList(p0, fd, &free_list);
        try std.testing.expectEqual(@as(usize, 3), free_list.pageCount());
    }
}

test "owned VMA page release handles middle cuts and protection splits" {
    for ([_]u64{ 0, 4096 }) |offset| {
        for ([_]bool{ false, true }) |protect_split| {
            var s = try initFdState();
            var free_list = FreePageList{};
            const base: u64 = 0x46a0_0000;
            const pages = [_]u64{ 0x8700_0000, 0x8700_1000, 0x8700_2000, 0x8700_3000, 0x8700_4000, 0x8700_5000, 0x8700_6000 };
            const fd = try s.createAnonymousVmoFd(p0, 7 * 4096, fdRights(.{ .map_read = true, .map_write = true, .close = true }), .{}, 0);
            const ref = s.nativeVmoRefForFd(p0, fd).?;
            try s.installNativeVmoPages(ref, 0, &pages);
            _ = try s.mmapFd(p0, fd, base, 5 * 4096, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .anonymous = true }), offset);
            try s.closeFdWithFreeList(p0, fd, &free_list);
            if (protect_split) {
                try s.setVmaProtRange(p0, base + 2 * 4096, 4096, vmaProt(.{ .read = true }));
                try std.testing.expectEqual(@as(?u32, 3), s.nativeVmoRefCount(ref));
            }
            const cut: usize = if (protect_split) 1 else 2;
            const page = offset / 4096 + cut;
            var prepared = try s.prepareMunmapRangeWithFreeList(p0, base + cut * 4096, 4096, &free_list);
            // A prepared suffix holds a retain but is not yet installed. The
            // original mapping still covers the page and must protect it.
            s.releaseUnmappedAnonymousVmoPageRange(p0, ref, page, 1, &free_list);
            try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
            s.commitMunmapPrepared(&prepared, &free_list);
            try std.testing.expectEqual(@as(?u32, if (protect_split) 3 else 2), s.nativeVmoRefCount(ref));
            try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
            try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(ref, page));
            // Releasing the hole again cannot double-return it. A later VMA
            // that overlaps the request must prevent release of its page.
            s.releaseUnmappedAnonymousVmoPageRange(p0, ref, page, 1, &free_list);
            s.releaseUnmappedAnonymousVmoPageRange(p0, ref, offset / 4096 + 4, 1, &free_list);
            try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
            for (pages, 0..) |paddr, index| {
                if (index != page) try std.testing.expectEqual(@as(?u64, paddr), s.nativeVmoPagePaddr(ref, index));
            }
            try s.munmapRangeWithFreeList(p0, base, 5 * 4096, &free_list);
            try std.testing.expectEqual(@as(usize, 7), free_list.pageCount());
            try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(ref));
        }
    }
}

test "multiple owned VMAs do not hide FD view or foreign mapping retains" {
    const Mode = enum { fd, view, foreign };
    for (std.meta.tags(Mode)) |mode| {
        var s = try initFdState();
        var free_list = FreePageList{};
        const base: u64 = 0x46b0_0000;
        const pages = [_]u64{ 0x8800_0000, 0x8800_1000, 0x8800_2000 };
        const rights = fdRights(.{ .map_read = true, .close = true, .transfer = true });
        const fd = try s.createAnonymousVmoFd(p0, 12288, rights, .{}, 0);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        try s.installNativeVmoPages(ref, 0, &pages);
        _ = try s.mmapFd(p0, fd, base, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);
        _ = try s.mmapFd(p0, fd, base + 8192, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 8192);
        var view: ?kernel.NativeVmoRef = null;
        if (mode == .view) {
            view = try s.createNativePageView(ref, 1);
            try s.installNativeVmoPages(view.?, 0, pages[1..2]);
        }
        if (mode == .foreign) {
            const other_fd = try s.transferFd(p0, p1, fd, 16, rights, .{}, .copy);
            _ = try s.mmapFd(p1, other_fd, base, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 4096);
            try s.closeFdWithFreeList(p1, other_fd, &free_list);
        }
        if (mode != .fd) try s.closeFdWithFreeList(p0, fd, &free_list);
        // Make the owner's total VMA count equal the target's retain count.
        // Only matching VMAs count: this unrelated mapping must not hide
        // the additional FD/view/foreign retain at the final proof check.
        const unrelated_fd = try s.createAnonymousVmoFd(p0, 4096, rights, .{}, 0);
        _ = try s.mmapFd(p0, unrelated_fd, base + 16384, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);
        try s.closeFdWithFreeList(p0, unrelated_fd, &free_list);
        try std.testing.expectEqual(@as(?u32, 3), s.nativeVmoRefCount(ref));
        s.releaseUnmappedAnonymousVmoPageRange(p0, ref, 1, 1, &free_list);
        try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
        try std.testing.expectEqual(@as(?u64, pages[1]), s.nativeVmoPagePaddr(ref, 1));
        try s.munmapRangeWithFreeList(p0, base, 12288, &free_list);
        if (mode == .foreign) try s.munmapRangeWithFreeList(p1, base, 4096, &free_list);
        if (view) |v| s.releaseNativeVmoWithFreeList(v, &free_list);
        if (mode == .fd) try s.closeFdWithFreeList(p0, fd, &free_list);
        try s.munmapRangeWithFreeList(p0, base + 16384, 4096, &free_list);
        try std.testing.expectEqual(@as(usize, 3), free_list.pageCount());
    }
}

test "whole VMA cuts account only for the removed mapping retain" {
    const Mode = enum { owned, fd, view, foreign, overlap, temporary };
    for (std.meta.tags(Mode)) |mode| {
        var s = try initFdState();
        var free_list = FreePageList{};
        const base: u64 = 0x46c0_0000;
        const pages = [_]u64{ 0x8900_0000, 0x8900_1000, 0x8900_2000 };
        const rights = fdRights(.{ .map_read = true, .close = true, .transfer = true });
        const fd = try s.createAnonymousVmoFd(p0, 12288, rights, .{}, 0);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        try s.installNativeVmoPages(ref, 0, &pages);
        for (0..3) |index| {
            _ = try s.mmapFd(p0, fd, base + index * 4096, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), index * 4096);
        }
        var view: ?kernel.NativeVmoRef = null;
        if (mode == .view) {
            view = try s.createNativePageView(ref, 1);
            try s.installNativeVmoPages(view.?, 0, pages[1..2]);
        }
        if (mode == .foreign) {
            const other_fd = try s.transferFd(p0, p1, fd, 16, rights, .{}, .copy);
            _ = try s.mmapFd(p1, other_fd, base, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 4096);
            try s.closeFdWithFreeList(p1, other_fd, &free_list);
        }
        if (mode == .overlap) {
            _ = try s.mmapFd(p0, fd, base + 16384, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 4096);
        }
        if (mode == .temporary) try s.retainNativeVmo(ref);
        if (mode != .fd) try s.closeFdWithFreeList(p0, fd, &free_list);
        // Unrelated VMAs ensure a missing external retain must fail the
        // exact matching-count check, not just the active-count shortcut.
        const unrelated_fd = try s.createAnonymousVmoFd(p0, 8192, rights, .{}, 0);
        for (0..2) |index| {
            _ = try s.mmapFd(p0, unrelated_fd, base + 32768 + index * 4096, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), index * 4096);
        }
        try s.closeFdWithFreeList(p0, unrelated_fd, &free_list);
        try s.munmapRangeWithFreeList(p0, base + 4096, 4096, &free_list);
        const releasable = mode == .owned or mode == .temporary;
        try std.testing.expectEqual(@as(usize, if (releasable) 1 else 0), free_list.pageCount());
        try std.testing.expectEqual(@as(?u64, if (releasable) 0 else pages[1]), s.nativeVmoPagePaddrOrHole(ref, 1));
        try std.testing.expectEqual(@as(?u32, if (mode == .owned) 2 else 3), s.nativeVmoRefCount(ref));
        try std.testing.expectEqual(@as(?u64, pages[0]), s.nativeVmoPagePaddr(ref, 0));
        try std.testing.expectEqual(@as(?u64, pages[2]), s.nativeVmoPagePaddr(ref, 2));
        // One syscall removes both remaining pieces around the hole. The
        // known removed retain is per iteration, never accumulated.
        try s.munmapRangeWithFreeList(p0, base, 12288, &free_list);
        if (mode == .foreign) try s.munmapRangeWithFreeList(p1, base, 4096, &free_list);
        if (mode == .overlap) try s.munmapRangeWithFreeList(p0, base + 16384, 4096, &free_list);
        if (mode == .temporary) s.releaseNativeVmoWithFreeList(ref, &free_list);
        if (view) |v| s.releaseNativeVmoWithFreeList(v, &free_list);
        if (mode == .fd) try s.closeFdWithFreeList(p0, fd, &free_list);
        try s.munmapRangeWithFreeList(p0, base + 32768, 8192, &free_list);
        try std.testing.expectEqual(@as(usize, 3), free_list.pageCount());
        try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(ref));
    }
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

test "contiguous DMA pool VMO installs consecutive low pages" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x80_0000, 8);

    const fd = try s.createContiguousVmoFdWithPages(
        p0,
        3 * 4096 - 1,
        fdRights(.{ .close = true, .map_read = true, .map_write = true }),
        .{},
        16,
        &free_list,
    );
    try std.testing.expect(fd >= 16);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    const base = s.nativeVmoPagePaddr(vmo, 0) orelse unreachable;
    var page_index: usize = 0;
    while (page_index < 3) : (page_index += 1) {
        const paddr = s.nativeVmoPagePaddr(vmo, page_index) orelse unreachable;
        try std.testing.expectEqual(base + (@as(u64, @intCast(page_index)) * 4096), paddr);
        try std.testing.expect(paddr < KernelState.low_memory_limit);
    }
    try s.closeFdWithFreeList(p0, fd, &free_list);
}

test "contiguous DMA pool VMO rejects fragmented free pages without consuming them" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x90_0000, 2);
    try free_list.appendContiguousRange(0, 0xa0_0000, 2);
    const original_free = free_list.pageCount();
    const original_ranges = free_list.rangeCount();

    try std.testing.expectError(
        KernelError.OutOfFreePages,
        s.createContiguousVmoFdWithPages(
            p0,
            3 * 4096,
            fdRights(.{ .close = true, .map_read = true, .map_write = true }),
            .{},
            16,
            &free_list,
        ),
    );
    try std.testing.expectEqual(original_free, free_list.pageCount());
    try std.testing.expectEqual(original_ranges, free_list.rangeCount());
}

test "contiguous DMA pool VMO close restores and merges its extent" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0xb0_0000, 8);
    const original_free = free_list.pageCount();

    const fd = try s.createContiguousVmoFdWithPages(
        p0,
        4 * 4096,
        fdRights(.{ .close = true }),
        .{},
        16,
        &free_list,
    );
    try std.testing.expectEqual(original_free - 4, free_list.pageCount());
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(original_free, free_list.pageCount());
    try std.testing.expectEqual(@as(usize, 1), free_list.rangeCount());
}

test "contiguous DMA pool VMO enforces the ABI page limit" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const oversized = (@as(u64, kernel.dma_pool_max_pages) + 1) * kernel.native_page_size;

    try std.testing.expectError(
        KernelError.InvalidState,
        s.createContiguousVmoFdWithPages(
            p0,
            oversized,
            fdRights(.{ .close = true }),
            .{},
            16,
            &free_list,
        ),
    );
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
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

test "DMA pin mode owns writable private COW pages and preserves read-only mappings" {
    var s = try initFdState();
    var free_list = FreePageList{};

    const read_only_va: u64 = 0x4510_0000;
    _ = try s.createAnonymousVmaWithPages(
        p0,
        read_only_va,
        4096,
        vmaProt(.{ .read = true }),
        vmaProt(.{ .read = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    try std.testing.expectEqual(
        kernel.NativeVmaDmaPinMode.read_only,
        s.nativeVmaDmaPinMode(p0, read_only_va, false) orelse unreachable,
    );
    try std.testing.expect(s.nativeVmaDmaPinMode(p0, read_only_va, true) == null);

    const direct_va: u64 = 0x4520_0000;
    _ = try s.createAnonymousVmaWithPages(
        p0,
        direct_va,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    try std.testing.expectEqual(
        kernel.NativeVmaDmaPinMode.direct_writable,
        s.nativeVmaDmaPinMode(p0, direct_va, false) orelse unreachable,
    );
    try std.testing.expectEqual(
        kernel.NativeVmaDmaPinMode.direct_writable,
        s.nativeVmaDmaPinMode(p0, direct_va, true) orelse unreachable,
    );

    const cow_va: u64 = 0x4530_0000;
    _ = try s.createAnonymousVmaWithPages(
        p0,
        cow_va,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true, .fork_cow = true }),
        &free_list,
    );
    try std.testing.expectEqual(
        kernel.NativeVmaDmaPinMode.cow_writable,
        s.nativeVmaDmaPinMode(p0, cow_va, false) orelse unreachable,
    );
    try std.testing.expectEqual(
        kernel.NativeVmaDmaPinMode.cow_writable,
        s.nativeVmaDmaPinMode(p0, cow_va, true) orelse unreachable,
    );
    try std.testing.expect(s.nativeVmaDmaPinMode(p0, cow_va + 1, false) == null);
}

test "DMA address derivation accepts contiguous pages and rejects fragmented pages" {
    const single_page = [_]u64{0x1200_0000};
    try std.testing.expectEqual(
        @as(?u64, 0x1200_0080),
        kernel.dmaAddressForResolvedPages(single_page[0..], 0x80),
    );

    const contiguous_pages = [_]u64{ 0x1300_0000, 0x1300_1000, 0x1300_2000 };
    try std.testing.expectEqual(
        @as(?u64, 0x1300_0080),
        kernel.dmaAddressForResolvedPages(contiguous_pages[0..], 0x80),
    );

    const fragmented_pages = [_]u64{ 0x1400_0000, 0x1400_3000, 0x1400_7000 };
    try std.testing.expectEqual(
        @as(?u64, null),
        kernel.dmaAddressForResolvedPages(fragmented_pages[0..], 0x80),
    );
}

test "COW detach copies only a split VMA range with one free page" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x800_0000, 8);
    const original = try s.createNativeCowTable(3, &free_list);
    try s.retainNativeCowTable(original);
    try s.retainNativeCowTable(original);
    var pages: [3]u64 = undefined;
    for (&pages, 0..) |*page, i| {
        page.* = (try s.allocPhysicalPage(&free_list)).paddr;
        try s.setNativeCowPagePaddr(original, @intCast(i), page.*);
    }
    var slice = kernel.VmaEntry{
        .active = true,
        .start_va = 0x4600_1000,
        .size_bytes = 4096,
        .cow_table = original,
        .cow_page_offset = 1,
    };
    var copy_free = FreePageList{};
    try copy_free.appendContiguousRange(0, 0x900_0000, 1);
    try s.detachSharedEntryCowTable(&slice, &copy_free);
    try std.testing.expectEqual(@as(usize, 0), copy_free.pageCount());
    try std.testing.expectEqual(@as(u32, 0), slice.cow_page_offset);
    try std.testing.expectEqual(@as(u32, 1), (s.nativeCowTableSlotConst(slice.cow_table) orelse unreachable).page_count);
    try std.testing.expectEqual(@as(?u64, 0x900_0000), s.entryDirtyPagePaddr(&slice, slice.start_va));
    for (pages, 0..) |page, i|
        try std.testing.expectEqual(@as(?u64, page), s.nativeCowPagePaddr(original, @intCast(i)));
    s.releaseNativeCowTable(slice.cow_table, &copy_free);
    s.releaseNativeCowTable(original, &free_list);
    try std.testing.expectEqual(@as(usize, 1), copy_free.pageCount());
    try std.testing.expectEqual(@as(usize, 8), free_list.pageCount());
}

test "failed split COW detach preserves shared metadata and returns copied pages" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0xa00_0000, 8);
    const original = try s.createNativeCowTable(3, &free_list);
    try s.retainNativeCowTable(original);
    try s.retainNativeCowTable(original);
    for (0..3) |i| {
        const page = (try s.allocPhysicalPage(&free_list)).paddr;
        try s.setNativeCowPagePaddr(original, @intCast(i), page);
    }
    var slice = kernel.VmaEntry{
        .active = true,
        .start_va = 0x4600_1000,
        .size_bytes = 8192,
        .cow_table = original,
        .cow_page_offset = 1,
    };
    const before = slice;
    const store_before = kernel.vmObjectBackingStoreStats().used_entries;
    var copy_free = FreePageList{};
    try copy_free.appendContiguousRange(0, 0xb00_0000, 1);
    try std.testing.expectError(KernelError.OutOfFreePages, s.detachSharedEntryCowTable(&slice, &copy_free));
    try std.testing.expectEqualDeep(before, slice);
    try std.testing.expectEqual(@as(usize, 1), copy_free.pageCount());
    try std.testing.expectEqual(store_before, kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expectEqual(@as(u32, 2), (s.nativeCowTableSlotConst(original) orelse unreachable).ref_count);
    s.releaseNativeCowTable(original, &free_list);
    s.releaseNativeCowTable(original, &free_list);
    try std.testing.expectEqual(@as(usize, 8), free_list.pageCount());
}

test "read-only backing permits private COW but not shared or executable mappings" {
    var s = try initFdState();
    const fd = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{ .map_read = true }), .{}, 0);
    const rw = vmaProt(.{ .read = true, .write = true });
    const rx = vmaProt(.{ .read = true, .exec = true });
    const va: u64 = 0x4500_0000;
    const denied_flags = [_]kernel.MmapFlags{
        .{},
        mmapFlags(.{ .shared = true }),
        mmapFlags(.{ .private = true, .shared = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
    };
    for (denied_flags) |flags| {
        try std.testing.expectError(KernelError.InvalidState, s.mmapFd(p0, fd, va, 4096, rw, flags, 0));
        try std.testing.expectError(KernelError.InvalidState, s.mmapFdIntoProcess(p0, fd, p1, va, 4096, rw, flags, 0));
        var free_list = FreePageList{};
        try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmap(p0, fd, va, 4096, rw, flags, 0, &free_list));
    }
    const private = mmapFlags(.{ .private = true });
    try std.testing.expectError(KernelError.InvalidState, s.mmapFd(p0, fd, va, 4096, rx, private, 0));
    try std.testing.expectError(KernelError.InvalidState, s.mmapFdIntoProcess(p0, fd, p1, va, 4096, rx, private, 0));
    _ = try s.mmapFd(p0, fd, va, 4096, .{}, private, 0);
    try s.setVmaProtRange(p0, va, 4096, rw);
    try std.testing.expectError(KernelError.InvalidState, s.setVmaProtRange(p0, va, 4096, rx));
    try std.testing.expectError(KernelError.InvalidState, s.setVmaProtRange(p0, va, 4096, vmaProt(.{ .read = true, .pkey = 1 })));
    _ = try s.mmapFdIntoProcess(p0, fd, p1, va, 4096, rw, private, 0);
    const mapped = s.vmaEntryConst(p1, va) orelse unreachable;
    try std.testing.expect(mapped.max_prot.write and !mapped.max_prot.exec);
    try std.testing.expectEqual(rw, (s.vmaEntryConst(p0, va) orelse unreachable).prot);
}

test "private file vma faults read-only then COWs on write" {
    var s = try initFdState();
    var free_list = FreePageList{};
    try free_list.appendContiguousRange(0, 0x1_0000_0000, 4);
    const source_page = (try s.allocPhysicalPage(&free_list)).paddr;

    const fd = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{
        .map_read = true,
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

    const write_mapping = s.ensureNativeVmaCowMappingLockedSlow(
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

fn fillVmaTableExcept(table: *kernel.VmaTable, except: []const usize) void {
    var index: usize = 0;
    while (index < table.entries.len) : (index += 1) {
        var skip = false;
        for (except) |except_index| {
            if (index == except_index) {
                skip = true;
                break;
            }
        }
        if (skip or table.entries[index].active) continue;
        KernelState.installVmaEntry(table, index, .{
            .active = true,
            .start_va = 0x7000_0000 + @as(u64, @intCast(index)) * 0x2000,
            .size_bytes = 4096,
        });
    }
}

test "fixed mmap prepare reuses a fully covered slot in a full vma table" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const target_va: u64 = 0x5100_0000;
    const source_vmo = try s.createAnonymousVmaWithPages(
        p0,
        target_va,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    const table = s.getVmaTable(p0) orelse unreachable;
    const target_index: usize = @intCast(table.active_indices[0]);
    fillVmaTableExcept(table, &.{});
    try std.testing.expectEqual(kernel.max_vmas_per_process, table.active_count);

    const dest_fd = try s.createAnonymousVmoFd(
        p0,
        4096,
        fdRights(.{ .map_read = true }),
        .{},
        16,
    );
    const dest_vmo = s.nativeVmoRefForFd(p0, dest_fd) orelse unreachable;
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmap(
        p0,
        dest_fd,
        target_va,
        4096,
        vmaProt(.{ .read = true, .exec = true }),
        mmapFlags(.{ .fixed = true, .private = true }),
        0,
        &free_list,
    ));
    try std.testing.expectEqual(source_vmo, (s.vmaEntryConst(p0, target_va) orelse unreachable).vmo);
    var prepared = try s.prepareFixedFdMmap(
        p0,
        dest_fd,
        target_va,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .fixed = true, .private = true }),
        0,
        &free_list,
    );
    try std.testing.expectEqual(target_index, prepared.destination_index);
    s.commitFixedMmapPrepared(&prepared, &free_list);
    try std.testing.expectEqual(kernel.max_vmas_per_process, table.active_count);
    try std.testing.expectEqual(dest_vmo, (s.vmaEntryConst(p0, target_va) orelse unreachable).vmo);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(source_vmo));
}

test "fixed mmap middle cut fails atomically with only one spare vma slot" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const target_va: u64 = 0x5200_0000;
    const source_vmo = try s.createAnonymousVmaWithPages(
        p0,
        target_va,
        3 * 4096,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    const table = s.getVmaTable(p0) orelse unreachable;
    const source_index: usize = @intCast(table.active_indices[0]);
    const spare_index: usize = if (source_index == 0) 1 else 0;
    fillVmaTableExcept(table, &.{ source_index, spare_index });
    try std.testing.expectEqual(kernel.max_vmas_per_process - 1, table.active_count);
    const original = table.entries[source_index];
    const source_refs = s.nativeVmoRefCount(source_vmo);

    const dest_fd = try s.createAnonymousVmoFd(
        p0,
        4096,
        fdRights(.{ .map_read = true }),
        .{},
        16,
    );
    const dest_vmo = s.nativeVmoRefForFd(p0, dest_fd) orelse unreachable;
    const dest_refs = s.nativeVmoRefCount(dest_vmo);
    try std.testing.expectError(KernelError.TableFull, s.prepareFixedFdMmap(
        p0,
        dest_fd,
        target_va + 4096,
        4096,
        vmaProt(.{ .read = true }),
        mmapFlags(.{ .fixed = true, .private = true }),
        0,
        &free_list,
    ));
    try std.testing.expectEqual(original, table.entries[source_index]);
    try std.testing.expect(!table.entries[spare_index].active);
    try std.testing.expectEqual(source_refs, s.nativeVmoRefCount(source_vmo));
    try std.testing.expectEqual(dest_refs, s.nativeVmoRefCount(dest_vmo));
}

test "shared fixed mmap retains aliases offsets and neighbors through fd close" {
    for ([_]kernel.MmapFlags{ .{ .fixed = true, .shared = true }, .{ .fixed = true } }) |flags| {
        var s = try initFdState();
        var free_list = FreePageList{};
        const rw = vmaProt(.{ .read = true, .write = true });
        const va: u64 = 0x5500_0000;
        const fd = try s.createAnonymousVmoFd(p0, 3 * 4096, fdRights(.{ .map_read = true, .map_write = true, .close = true }), .{}, 16);
        const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
        try s.installNativeVmoPages(vmo, 0, &.{ 0x1000, 0x2000, 0x3000 });
        _ = try s.mmapFd(p0, fd, va, 3 * 4096, rw, .{ .shared = true }, 0);
        _ = try s.mmapFd(p0, fd, va + 0x10000, 3 * 4096, rw, .{ .shared = true }, 0);
        var prepared = try s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, rw, flags, 8192, &free_list);
        s.commitFixedMmapPrepared(&prepared, &free_list);
        try s.closeFdWithFreeList(p0, fd, &free_list);
        const replacement = s.nativeVmaFaultMapping(p0, va + 4096, true, false) orelse unreachable;
        const alias = s.nativeVmaFaultMapping(p0, va + 0x10000 + 8192, true, false) orelse unreachable;
        try std.testing.expectEqual(@as(u64, 0x3000), replacement.paddr);
        try std.testing.expectEqual(alias.paddr, replacement.paddr);
        try std.testing.expect(replacement.prot.write and alias.prot.write);
        try std.testing.expectEqual(@as(u64, 0x1000), (s.nativeVmaFaultMapping(p0, va, false, false) orelse unreachable).paddr);
        try std.testing.expectEqual(@as(u64, 0x3000), (s.nativeVmaFaultMapping(p0, va + 8192, false, false) orelse unreachable).paddr);
        try std.testing.expectEqual(@as(?u32, 4), s.nativeVmoRefCount(vmo));
    }
}

test "remote fixed mapping resolves only caller capabilities and preserves peer aliases" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const rw = vmaProt(.{ .read = true, .write = true });
    const va: u64 = 0x5700_0000;
    const fd = try s.createAnonymousVmoFd(p0, 8192, fdRights(.{ .map_read = true, .map_write = true, .close = true }), .{}, 16);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    try s.installNativeVmoPages(vmo, 0, &.{ 0x1000, 0x2000 });
    _ = try s.mmapFdIntoProcess(p0, fd, p1, va, 8192, rw, .{ .shared = true }, 0);
    _ = try s.mmapFd(p0, fd, va, 8192, rw, .{ .shared = true }, 0);
    try std.testing.expect(s.fdEntryConst(p1, fd) == null);
    const before = (s.vmaEntryConst(p1, va) orelse unreachable).*;
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmapIntoProcess(p1, fd, p1, va, 4096, rw, .{ .shared = true }, 4096, &free_list));
    try std.testing.expectEqualDeep(before, (s.vmaEntryConst(p1, va) orelse unreachable).*);
    var prepared = try s.prepareFixedFdMmapIntoProcess(p0, fd, p1, va, 4096, rw, .{ .shared = true }, 4096, &free_list);
    s.commitFixedMmapPrepared(&prepared, &free_list);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(@as(u64, 0x2000), s.nativeVmaFaultMapping(p1, va, true, false).?.paddr);
    try std.testing.expectEqual(@as(u64, 0x1000), s.nativeVmaFaultMapping(p0, va, true, false).?.paddr);
    var removal = try s.prepareMunmapRangeWithFreeList(p1, va, 4096, &free_list);
    s.commitMunmapPrepared(&removal, &free_list);
    try std.testing.expect(s.nativeVmaFaultMapping(p1, va, false, false) == null);
    try std.testing.expectEqual(@as(u64, 0x2000), s.nativeVmaFaultMapping(p1, va + 4096, true, false).?.paddr);
    try std.testing.expectEqual(@as(u64, 0x1000), s.nativeVmaFaultMapping(p0, va, true, false).?.paddr);
}

test "shared fixed mmap preflight failures leave old backing intact" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const va: u64 = 0x5600_0000;
    const rw = vmaProt(.{ .read = true, .write = true });
    const old = try s.createAnonymousVmaWithPages(p0, va, 3 * 4096, rw, rw, .{ .anonymous = true, .private = true }, &free_list);
    const original = (s.vmaEntryConst(p0, va) orelse unreachable).*;
    const fd = try s.createAnonymousVmoFd(p0, 3 * 4096, fdRights(.{ .map_read = true, .map_write = true }), .{}, 16);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    const flags = mmapFlags(.{ .fixed = true, .shared = true });
    // Missing backing, out-of-range offsets, denied execute and malformed flags.
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, rw, flags, 0, &free_list));
    try s.installNativeVmoPages(vmo, 0, &.{ 0x1000, 0x2000, 0x3000 });
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, rw, flags, 3 * 4096, &free_list));
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, .{ .read = true, .exec = true }, flags, 0, &free_list));
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, rw, .{ .private = true, .shared = true }, 0, &free_list));
    try std.testing.expectEqual(original, (s.vmaEntryConst(p0, va) orelse unreachable).*);
    const table = s.getVmaTable(p0) orelse unreachable;
    fillVmaTableExcept(table, &.{1});
    try std.testing.expectError(KernelError.TableFull, s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, rw, flags, 4096, &free_list));
    try std.testing.expectEqual(original, (s.vmaEntryConst(p0, va) orelse unreachable).*);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(old));
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(vmo));
}

test "fixed mmap validation and discard preserve the old mapping" {
    var s = try initFdState();
    var free_list = FreePageList{};
    const target_va: u64 = 0x5300_0000;
    const source_vmo = try s.createAnonymousVmaWithPages(
        p0,
        target_va,
        3 * 4096,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true }),
        mmapFlags(.{ .private = true, .anonymous = true }),
        &free_list,
    );
    const original = (s.vmaEntryConst(p0, target_va) orelse unreachable).*;
    const table = s.getVmaTable(p0) orelse unreachable;
    const active_before = table.active_count;
    const source_refs = s.nativeVmoRefCount(source_vmo);
    const fixed_private = mmapFlags(.{ .fixed = true, .private = true });
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmap(
        p0,
        std.math.maxInt(kernel.Fd),
        target_va + 4096,
        4096,
        vmaProt(.{ .read = true }),
        fixed_private,
        0,
        &free_list,
    ));
    try std.testing.expectEqual(original, (s.vmaEntryConst(p0, target_va) orelse unreachable).*);
    try std.testing.expectEqual(active_before, table.active_count);
    try std.testing.expectEqual(source_refs, s.nativeVmoRefCount(source_vmo));

    const dest_fd = try s.createAnonymousVmoFd(
        p0,
        4096,
        fdRights(.{ .map_read = true, .map_write = true }),
        .{},
        16,
    );
    const dest_vmo = s.nativeVmoRefForFd(p0, dest_fd) orelse unreachable;
    var prepared = try s.prepareFixedFdMmap(
        p0,
        dest_fd,
        target_va + 4096,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        fixed_private,
        0,
        &free_list,
    );
    try std.testing.expectEqual(@as(?u32, 2), s.nativeVmoRefCount(source_vmo));
    try std.testing.expectEqual(@as(?u32, 2), s.nativeVmoRefCount(dest_vmo));
    s.discardFixedMmapPrepared(&prepared, &free_list);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(source_vmo));
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(dest_vmo));
    try std.testing.expectEqual(original, (s.vmaEntryConst(p0, target_va) orelse unreachable).*);

    var anonymous = try s.prepareFixedAnonymousMmap(
        p0,
        target_va + 4096,
        4096,
        vmaProt(.{ .read = true, .write = true }),
        vmaProt(.{ .read = true, .write = true, .exec = true }),
        mmapFlags(.{ .fixed = true, .private = true, .anonymous = true }),
        &free_list,
    );
    const fresh_vmo = anonymous.destination.vmo;
    try std.testing.expectEqual(@as(?u32, 2), s.nativeVmoRefCount(source_vmo));
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(fresh_vmo));
    s.discardFixedMmapPrepared(&anonymous, &free_list);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(source_vmo));
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(fresh_vmo));
    try std.testing.expectEqual(original, (s.vmaEntryConst(p0, target_va) orelse unreachable).*);
}
