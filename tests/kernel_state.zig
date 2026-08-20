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
    var bitmap: [t.iova_bitmap_bytes]u8 = undefined;
    var allocator = t.IovaAllocator.init(&bitmap);

    try std.testing.expectEqual(t.iova_window_start, allocator.alloc(2).?);
    try std.testing.expectEqual(t.iova_window_start + 2 * t.page_size, allocator.alloc(3).?);
    try std.testing.expect(allocator.free(t.iova_window_start, 2));
    try std.testing.expectEqual(t.iova_window_start + 5 * t.page_size, allocator.alloc(2).?);

    allocator = t.IovaAllocator.init(&bitmap);
    try std.testing.expectEqual(t.iova_window_start, allocator.alloc(t.iova_page_count - 1).?);
    try std.testing.expectEqual(t.iova_window_end - t.page_size, allocator.alloc(1).?);
    try std.testing.expect(allocator.alloc(1) == null);
    try std.testing.expect(allocator.free(t.iova_window_start, 1));
    try std.testing.expectEqual(t.iova_window_start, allocator.alloc(1).?);
    try std.testing.expectEqual(t.iova_page_count, allocator.peak_pages);

    try std.testing.expect(allocator.free(t.iova_window_start, t.iova_page_count));
    try std.testing.expectEqual(@as(usize, 0), allocator.used_pages);
    try std.testing.expectEqual(t.iova_window_start, allocator.alloc(t.iova_page_count).?);
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

    var bitmap: [t.iova_bitmap_bytes]u8 = undefined;
    var allocator = t.IovaAllocator.init(&bitmap);
    const iova = allocator.alloc(1).?;
    domain_a_leaf[leaf_index] = 0;
    try std.testing.expect(allocator.free(iova, 1));
    try std.testing.expectEqual(iova + t.page_size, allocator.alloc(t.iova_page_count - 1).?);
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
    const source_fd = try s.createDmaBufferFd(p0, .{
        .device = 1,
        .user_va = base + 0x80,
        .iova = 0x8000_0080,
        .size = 64,
    }, fdRights(.{ .close = true }), .{}, 16);
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

test "pinned fds reject transfer and skip process-create inheritance" {
    var s = try initFdState();
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

    try s.inheritFdsForProcessCreate(p0, p1);
    try std.testing.expect(s.fdEntryConst(p1, pin_fd) == null);
    try std.testing.expectEqual(
        event_ref,
        (s.fdEntryConst(p1, event_fd) orelse unreachable).object,
    );
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(pin_ref));
    try std.testing.expectEqual(@as(?u32, 2), s.kernelObjectRefCount(event_ref));
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

test "signal wake cancellation clears ipc recv completion waiter" {
    var s = try initFdState();
    const rights = fdRights(.{
        .send = true,
        .recv = true,
        .wait = true,
        .close = true,
    });
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    try s.registerIpcRecvCompletionWaiterForFd(p0, pair.a, 0x2000, 2, 7, 11);

    s.unregisterFdWaitersForThread(p0, 7, 11);

    var targets: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), try s.wakeIpcWaitersForSendFd(p0, pair.b, targets[0..]));
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

test "vmo backing store reclaims and coalesces its high-water tail" {
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));

    try std.testing.expectEqual(@as(?u32, 0), kernel.allocEmptyVmoBackingPageStore(2));
    try std.testing.expectEqual(@as(?u32, 2), kernel.allocEmptyVmoBackingPageStore(3));
    try std.testing.expectEqual(@as(?u32, 5), kernel.allocEmptyVmoBackingPageStore(4));
    try std.testing.expect(kernel.freeVmoBackingPageStore(2, 3));
    try std.testing.expect(kernel.freeVmoBackingPageStore(5, 4));
    try std.testing.expectEqual(@as(?u32, 2), kernel.allocEmptyVmoBackingPageStore(8));

    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));
    const range_count = kernel.max_vmo_backing_store_free_ranges;
    const reserved_pages = range_count * 2 + 1;
    try std.testing.expectEqual(@as(?u32, 0), kernel.allocEmptyVmoBackingPageStore(reserved_pages));
    for (0..range_count) |i| {
        try std.testing.expect(kernel.freeVmoBackingPageStore(@intCast(i * 2), 1));
    }
    try std.testing.expect(kernel.freeVmoBackingPageStore(@intCast(reserved_pages - 1), 1));
    try std.testing.expectEqual(
        @as(?u32, @intCast(reserved_pages - 1)),
        kernel.allocEmptyVmoBackingPageStore(2),
    );
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
        mmapFlags(.{ .private = true }),
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
    try std.testing.expectError(
        KernelError.InvalidState,
        s.setVmaProtRange(
            p0,
            0x4130_0000,
            4096,
            vmaProt(.{ .read = true, .write = true, .exec = true }),
        ),
    );
    const transition_vma = s.vmaEntryConst(p0, 0x4130_0000) orelse unreachable;
    try std.testing.expect(transition_vma.prot.read);
    try std.testing.expect(!transition_vma.prot.write);
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
        fdRights(.{ .map_read = true, .map_write = true }),
        .{},
        16,
    );
    const dest_vmo = s.nativeVmoRefForFd(p0, dest_fd) orelse unreachable;
    var prepared = try s.prepareFixedFdMmap(
        p0,
        dest_fd,
        target_va,
        4096,
        vmaProt(.{ .read = true }),
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
