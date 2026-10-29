// Historical pool sizes used only to exercise growth beyond the removed limits.
const former_max_fd_objects: usize = 8192;
const former_max_pipes: usize = 256;
const former_max_ipc_endpoints: usize = 1024;
const former_max_ipc_channels: usize = 1024;
const former_max_ipc_replies: usize = 1024;
const former_max_native_vmos: usize = 32768;
const former_max_vmas_per_process: usize = 8192;
const former_max_fd_wait_groups: usize = 256;
const former_max_fd_wait_group_links: usize = 512;
const former_max_task_fd_waiters: usize = 256;

const std = @import("std");
const kernel = @import("kernel");
const fd_abi = @import("kernel_abi_root").fd_abi;
const pager_abi = @import("kernel_abi_root").pager_abi;
const ProcessFdGrant = @import("kernel_abi_root").process_abi.ProcessFdGrant;

const KernelState = kernel.KernelState;
const KernelError = kernel.KernelError;
const FreePageList = kernel.FreePageList;
const PrincipalId = kernel.PrincipalId;
const DmaHold = kernel.DmaRetentionTestHold;

const p0: PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;
const p1: PrincipalId = kernel.processPrincipalFromIndex(1) orelse unreachable;
const p2: PrincipalId = kernel.processPrincipalFromIndex(2) orelse unreachable;

var invocation_allocator = kernel.InvocationStorageTestAllocator{};

var fd_capacity_backing: [64 * 1024 * 1024]u8 align(4096) = undefined;
var runtime_storage: [kernel.runtimeStorageBytes()]u8 align(4096) = undefined;
var backing_growth_memory: [128 * 4096]u8 align(4096) = undefined;

var physical_host_grants: [3]kernel.PhysicalAccessTest.Mapping = undefined;
fn physicalFixture() !kernel.PhysicalAccessTest.BackingSet {
    // Explicit host grants take priority even when a linker places them inside
    // a logical fixture PA window. Their data and PMM descriptor share backing.
    physical_host_grants = .{
        .{ .physical = @intFromPtr(&fd_capacity_backing), .bytes = &fd_capacity_backing },
        .{ .physical = @intFromPtr(&runtime_storage), .bytes = &runtime_storage },
        .{ .physical = @intFromPtr(&backing_growth_memory), .bytes = &backing_growth_memory },
    };
    kernel.PhysicalAccessTest.host_mappings = &physical_host_grants;
    return kernel.PhysicalAccessTest.BackingSet.initMixed(&.{
        .{ .first = 4096, .pages = 4095 },
        .{ .first = 64 << 20, .pages = 768 },
        .{ .first = 0x08000000, .pages = 16 },
        .{ .first = 0x09000000, .pages = 16 },
        .{ .first = 0x0a000000, .pages = 16 },
        .{ .first = 0x0b000000, .pages = 16 },
        .{ .first = 0x80000000, .pages = 0x11000 },
        .{ .first = (1 << 32) - (2 << 20), .pages = 1536 },
        .{ .first = 20 << 30, .pages = 131072 },
        .{ .first = 16 << 30, .pages = 16 },
        .{ .first = 254 * (@as(u64, 1) << 39), .pages = 1024 },
    });
}

fn initFdState(state: *KernelState) !void {
    kernel.resetDmaRetentionForTest();
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));
    // Stable timer owners bind during process admission; initialize at final address.
    try state.initFromDetectedRegionsInPlace(300);
    state.invocation_test_allocator = invocation_allocator.interface();
}

fn reconstructUnbackedFixtureInPlace(state: *KernelState) !void {
    // Only these serial logical waiter fixtures use this teardown. No PMM
    // backing or borrower survives; do not make production live reset legal.
    try std.testing.expect(state.fd_tables_extra.directory_owner.bound_address == null);
    try std.testing.expect(state.fd_tables_extra.directory_owner.published == null and state.fd_tables_extra.directory_owner.retired == null and state.fd_tables_extra.directory_owner.pending == null);
    try std.testing.expect(state.vma_directory_storage.bound_address == null);
    try std.testing.expect(state.vma_body_storage.bound_address == null);
    try std.testing.expect(state.descriptor_storage.bound_address == null);
    state.deinitPools();
    state.* = undefined;
    try state.initFromDetectedRegionsInPlace(1);
}

fn createTestFdObject(s: *KernelState, id: u64) !kernel.KernelObjectRef {
    return s.createKernelObject(.event, .{ .event = id });
}

fn releaseFixtureOwner(owner: anytype) void {
    // Test-only teardown after all borrowers and workers finish. Save original
    // issuer/grant/link before release, which invalidates the record itself.
    inline for (.{ owner.published, owner.retired, owner.pending }) |head| {
        var cursor = head;
        while (cursor) |record| {
            const next = record.next;
            const allocator = record.allocator;
            const allocation = record.allocation;
            std.debug.assert(allocator.release(allocator.context, allocation));
            cursor = next;
        }
    }
    owner.published = null;
    owner.retired = null;
    owner.pending = null;
}

// Compare resource semantics without recursively traversing reverse links.
// ReferenceCheck verifies reverse links; expectVmaAddressIndex verifies the
// address index and its owner. Neither graph is a recursively comparable value.
fn expectResourceEqualDeep(expected: anytype, actual: @TypeOf(expected)) !void {
    const T = @TypeOf(expected);
    if (T == kernel.VmaEntry or T == kernel.FdEntry) {
        inline for (std.meta.fields(T)) |field| {
            if (comptime T == kernel.FdEntry and std.mem.eql(u8, field.name, "authority")) {
                if (expected.object.kind == .endpoint or expected.object.kind == .channel)
                    try std.testing.expectEqual(expected.authority.ipc_generation, actual.authority.ipc_generation);
            } else if (comptime T == kernel.FdEntry and std.mem.eql(u8, field.name, "cursor")) {
                if (expected.object.kind == .endpoint or expected.object.kind == .channel)
                    try std.testing.expectEqual(expected.cursor.ipc_queue, actual.cursor.ipc_queue)
                else
                    try std.testing.expectEqual(expected.cursor.offset, actual.cursor.offset);
            } else if (comptime !std.mem.eql(u8, field.name, "references")) {
                try std.testing.expectEqualDeep(@field(expected, field.name), @field(actual, field.name));
            }
        }
    } else try std.testing.expectEqualDeep(expected, actual);
}

fn expectVmoSlotEqual(expected: kernel.NativeVmoSlot, observed: kernel.NativeVmoSlot) !void {
    var actual = observed;
    // Instrumentation is not published VMO state. Still compare roots, owners,
    // PFN counts and the actual lock byte below.
    actual.page_store.lock.takes = expected.page_store.lock.takes;
    actual.page_store.visits = expected.page_store.visits;
    inline for (std.meta.fields(kernel.NativeVmoSlot)) |field| {
        if (comptime !std.mem.eql(u8, field.name, "backing") and !std.mem.eql(u8, field.name, "references") and !std.mem.eql(u8, field.name, "parent_edge"))
            try expectResourceEqualDeep(@field(expected, field.name), @field(actual, field.name));
    }
    if (expected.kind == .device_memory)
        try expectResourceEqualDeep(expected.backing.device, actual.backing.device)
    else
        try expectResourceEqualDeep(expected.backing.parent, actual.backing.parent);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
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
    try std.testing.expectEqual(@as(u64, 0x7ffff000), (try kernel.acpi_dmar.parseDmar(&fixture)).dma_window_end);
    writeLe64(&fixture, 80, 0x80000000);
    finishAcpiChecksum(&fixture);
    try std.testing.expectError(error.InvalidReservedRange, kernel.acpi_dmar.parseDmar(&fixture));
    writeLe64(&fixture, 80, 0x80000fff);
    fixture[89] = 7;
    finishAcpiChecksum(&fixture);
    try std.testing.expectError(error.InvalidDeviceScopeLength, kernel.acpi_dmar.parseDmar(&fixture));
}

test "DMAR parser accepts one DRHD" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var fixture = [_]u8{0} ** 64;
    initDmarFixture(fixture[0..], 47, 0x1);
    writeDrhd(fixture[0..], 48, 0x1, 3, 0xfed9_0000);
    finishAcpiChecksum(fixture[0..]);

    const parsed = try kernel.acpi_dmar.parseDmar(fixture[0..]);
    try std.testing.expectEqual(@as(u8, 47), parsed.host_address_width);
    try std.testing.expectEqual(@as(u16, 48), parsed.hostAddressWidthBits());
    try std.testing.expectEqual(@as(u8, 0x1), parsed.flags);
    try std.testing.expectEqual(@as(usize, 1), parsed.drhd_count);
    try std.testing.expectEqual(@as(u64, 0xfed9_0000), parsed.drhdAt(0).register_base);
    try std.testing.expectEqual(@as(u16, 3), parsed.drhdAt(0).segment);
    try std.testing.expect(parsed.drhdAt(0).include_pci_all);
}

test "DMAR parser retains multiple DRHD structures" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var fixture = [_]u8{0} ** 80;
    initDmarFixture(fixture[0..], 38, 0);
    writeDrhd(fixture[0..], 48, 0, 0, 0xfed9_0000);
    writeDrhd(fixture[0..], 64, 1, 2, 0xfeda_0000);
    finishAcpiChecksum(fixture[0..]);

    const parsed = try kernel.acpi_dmar.parseDmar(fixture[0..]);
    try std.testing.expectEqual(@as(usize, 2), parsed.drhd_count);
    try std.testing.expect(!parsed.drhdAt(0).include_pci_all);
    try std.testing.expectEqual(@as(u16, 2), parsed.drhdAt(1).segment);
    try std.testing.expectEqual(@as(u64, 0xfeda_0000), parsed.drhdAt(1).register_base);
    try std.testing.expect(parsed.drhdAt(1).include_pci_all);
}

test "DMAR parser retains DRHD device scopes and paths" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
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
    try std.testing.expectEqual(@as(u16, 26), parsed.drhdAt(0).length);
    try std.testing.expectEqual(@as(usize, 1), parsed.drhdAt(0).scope_count);
    const scope = parsed.drhdAt(0).scopeAt(0);
    try std.testing.expectEqual(@as(u8, 1), scope.scope_type);
    try std.testing.expectEqual(@as(u8, 7), scope.enumeration_id);
    try std.testing.expectEqual(@as(usize, 2), scope.path_count);
    try std.testing.expectEqual(@as(u8, 3), scope.path[1].device);
    try std.testing.expectEqual(@as(u8, 1), scope.path[1].function);
}

test "DMAR parser rejects malformed DRHD device scope" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var fixture = [_]u8{0} ** 72;
    initDmarFixture(fixture[0..], 38, 0);
    writeDmarStructureHeader(fixture[0..], 48, 0x7fff, 8);
    fixture[52] = 0xaa;
    writeDrhd(fixture[0..], 56, 1, 0, 0xfed9_0000);
    finishAcpiChecksum(fixture[0..]);

    const parsed = try kernel.acpi_dmar.parseDmar(fixture[0..]);
    try std.testing.expectEqual(@as(usize, 1), parsed.drhd_count);
    try std.testing.expectEqual(@as(u64, 0xfed9_0000), parsed.drhdAt(0).register_base);
}

test "DMAR parser rejects invalid and out-of-bounds structure lengths" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var page = [_]u64{0} ** 512;
    try std.testing.expect(!kernel.vtd_tables.setRootEntry(&page, 0, 0x1234_5001));
    try std.testing.expect(!kernel.vtd_tables.setContextEntry(&page, 0, 0x0010_0000_0000_0000, 1));
    try std.testing.expect(kernel.vtd_tables.setContextEntry(&page, 0, 0x1000, 0));
    try std.testing.expectEqual(@as(u64, 0x2), page[1]);
    try std.testing.expect(!kernel.vtd_tables.setSecondLevelEntry(&page, page.len, 0x1000, true, true));
}

test "VT-d IOVA allocator allocates frees exhausts and wraps its cursor" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x20_0000, .end = (0x20_0000) + (16) * 4096 }});
    try free_list.seedContiguousRange(0x20_0000, 16);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const base: u64 = 0x4000_0000;
    s.next_fd_object_scan = former_max_fd_objects - 1;
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
    try std.testing.expect(s.fd_objects.isMarked(@intCast(source_ref.index), 1));
    var free_list = FreePageList{};
    try s.closeFdWithFreeList(p0, alias, &free_list);
    try std.testing.expect(!s.fd_objects.isMarked(@intCast(source_ref.index), 1));
    try std.testing.expect(s.rangeOverlapsPinnedUserObject(p0, base, 4096));
    s.next_fd_object_scan = source_ref.index;
    const reused = try createTestFdObject(&s, 22);
    try std.testing.expectEqual(source_ref.index, reused.index);
    try std.testing.expect(reused.generation != source_ref.generation);
    try std.testing.expect(!s.fd_objects.isMarked(@intCast(reused.index), 1));
    s.resetKernelObjectTable();
    try std.testing.expectEqual(@as(usize, 0), markedCount(&s.fd_objects, 1));
    try std.testing.expect(!s.rangeOverlapsPinnedUserObject(p0, base, 4096));
    // An unpublished object is indexed, but ref_count=0 is not a live pin.
    const pending = try s.createKernelObject(.dma_mapping, .{ .dma_mapping = .{
        .owner_principal_raw = @intFromEnum(p0),
        .user_va = base,
        .size = 64,
    } });
    try std.testing.expect(s.fd_objects.isMarked(@intCast(pending.index), 1));
    try std.testing.expect(!s.rangeOverlapsPinnedUserObject(p0, base, 4096));
    s.clearKernelObjectSlot(s.kernelObjectSlot(pending).?);
    try std.testing.expectEqual(@as(usize, 0), markedCount(&s.fd_objects, 1));
}

test "DMA derivation aliases DMA pins only when explicitly allowed" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    const scatter_fd = try s.createDmaMappingFdWithBacking(p0, .{
        .device = 1,
        .user_va = scatter_base + 0x80,
        .iova = 0x908c_0080,
        .size = 64,
        .page_count = 1,
        .direction = .from_device,
    }, fdRights(.{ .close = true }), .{}, 16, &.{0x908c0000});
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const scatter_base: u64 = 0x4098_0000;
    _ = try s.createDmaMappingFdWithBacking(p0, .{
        .device = 1,
        .user_va = scatter_base + 0x180,
        .iova = 0x9098_0180,
        .size = 64,
        .page_count = 1,
        .direction = .from_device,
    }, fdRights(.{ .close = true }), .{}, 16, &.{0x90980000});

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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    try expectResourceEqualDeep(before, (s.fdEntryConst(p0, source) orelse unreachable).*);
    const no_transfer = try s.installFd(p0, ref, fdRights(.{ .close = true }), .{}, 16);
    const invalid = [_]ProcessFdGrant{.{ .source_fd = no_transfer, .target_fd = 8, .rights = fd_abi.right_close, .flags = 0 }};
    try std.testing.expectError(KernelError.InvalidState, s.grantFdsForProcessCreate(p0, p1, &invalid, &free_list));
    try std.testing.expect(s.fdEntryConst(p1, 8) == null);
}

test "process-create rejects invalid grant batches before copying any reference" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const ref = try createTestFdObject(&s, 43);
    const source = try s.installFd(p0, ref, fdRights(.{ .transfer = true, .close = true }), .{}, 16);
    const first = ProcessFdGrant{ .source_fd = source, .target_fd = 4, .rights = fd_abi.right_close, .flags = 0 };
    const invalid = [_]ProcessFdGrant{
        first, // duplicate destination
        .{ .source_fd = source, .target_fd = fd_abi.thread_self_fd, .rights = 0, .flags = 0 },
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
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(&fd_capacity_backing), .end = (@intFromPtr(&fd_capacity_backing)) + (256) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), 256);
    const free_before = free_list.pageCount();
    try s.grantFdsForProcessCreate(p0, p1, &high, &free_list);
    try std.testing.expectEqual(ref, (s.fdEntryConst(p1, 4095) orelse unreachable).object);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expectEqual(free_before, free_list.pageCount());
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(ref));
}

test "process-create rolls back partial retains including non-closeable grants" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(&fd_capacity_backing), .end = (@intFromPtr(&fd_capacity_backing)) + (256) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), 256);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    try std.testing.expect(!s.fd_objects.isMarked(@intCast(owner_ref.index), 1));
    try std.testing.expect(s.fd_objects.isMarked(@intCast(unrelated_ref.index), 1));
    try std.testing.expectEqual(unrelated_ref, (s.fdEntryConst(p1, unrelated_fd) orelse unreachable).object);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(unrelated_ref));
}

test "scatter DMA mapping keeps ABI page count with retained PFN backing" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);

    const fd = try s.createDmaMappingFdWithBacking(p0, .{
        .device = 1,
        .user_va = 0x4400_0080,
        .iova = 0x8200_0080,
        .size = 3 * 4096 - 0x80,
        .page_count = 3,
        .direction = .bidirectional,
    }, fdRights(.{ .close = true }), .{}, 16, &.{ 0x82000000, 0x82001000, 0x82002000 });

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

test "scatter DMA mapping validates fd capacity and rolls back retained PFN backing" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    try std.testing.expectError(KernelError.TableFull, s.createDmaMappingFdWithBacking(p0, .{
        .device = 1,
        .user_va = 0x4500_0000,
        .iova = 0x8200_0000,
        .size = 4096,
        .page_count = 1,
    }, fdRights(.{ .close = true }), .{}, @intCast(kernel.fd_table_entries), &.{0x82000000}));
    try std.testing.expectEqual(@as(usize, 0), markedCount(&s.fd_objects, 1));
    try std.testing.expectError(KernelError.TableFull, s.createDmaBufferFd(p0, .{
        .device = 1,
        .user_va = 0x4500_0000,
        .iova = 0x8200_0000,
        .size = 4096,
    }, fdRights(.{ .close = true }), .{}, @intCast(kernel.fd_table_entries)));
    try std.testing.expectEqual(@as(usize, 0), markedCount(&s.fd_objects, 1));
}

test "owner revoke retains scatter DMA backing after reference teardown" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const owner_fd = try s.createDmaMappingFdWithBacking(p0, .{
        .device = 1,
        .user_va = 0x4600_0080,
        .iova = 0x8200_0080,
        .size = 8192 - 0x80,
        .page_count = 2,
    }, fdRights(.{ .close = true }), .{}, 16, &.{ 0x82000000, 0x82001000 });
    const object_ref = (s.fdEntryConst(p0, owner_fd) orelse unreachable).object;
    const alias_fd = try s.installFd(p1, object_ref, fdRights(.{ .close = true }), .{}, 16);
    s.releasePrincipalNativeMemory(p0, &free_list);
    try std.testing.expect(s.fdEntryConst(p0, owner_fd) == null);
    try std.testing.expect(s.fdEntryConst(p1, alias_fd) == null);
    try std.testing.expectEqual(@as(usize, 0), free_list.len);
}

test "process fd exposes process object kind and lifecycle state" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const private_object = try createTestFdObject(&s, 501);
    const ordinary_object = try createTestFdObject(&s, 502);
    const private_fd = try s.installFd(p0, private_object, fdRights(.{ .inspect = true, .close = true }), .{ .private = true, .inherit = true }, 16);
    const ordinary_fd = try s.installFd(p0, ordinary_object, fdRights(.{ .inspect = true, .close = true, .dup = true }), .{ .cloexec = true, .nonblock = true }, 16);
    (s.getFdTable(p0) orelse unreachable).at(ordinary_fd).*.cursor.offset = 12345;
    const original = (s.getFdTable(p0) orelse unreachable).at(ordinary_fd).*;
    try s.cloneFdTableForFork(p0, p1);
    try std.testing.expect(s.fdEntryConst(p1, private_fd) == null);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(private_object));
    try std.testing.expectEqual(@as(?u32, 2), s.kernelObjectRefCount(ordinary_object));
    const copied = s.fdEntryConst(p1, ordinary_fd) orelse unreachable;
    try std.testing.expectEqual(original.object, copied.object);
    try std.testing.expectEqual(kernel.fdRightsToBits(original.rights), kernel.fdRightsToBits(copied.rights));
    try std.testing.expectEqual(kernel.fdFlagsToBits(original.flags), kernel.fdFlagsToBits(copied.flags));
    try std.testing.expectEqual(original.cursor.offset, copied.cursor.offset);
    try s.closeFd(p1, ordinary_fd);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(ordinary_object));
    try std.testing.expect(s.fdEntryConst(p0, private_fd) != null);
}

test "thread observation wakes only terminal generation across objects and owners" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const obj = try createTestFdObject(&s, 50);
    _ = try s.installFd(p0, obj, fdRights(.{ .dup = true }), .{}, 0);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(obj));

    s.resetProcessRuntimeTables(0);
    try std.testing.expect(s.fdEntryConst(p0, 0) == null);
    try std.testing.expectEqual(@as(?u32, null), s.kernelObjectRefCount(obj));
}

test "fd process exit and remove release fd table" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(&fd_capacity_backing), .end = (@intFromPtr(&fd_capacity_backing)) + (fd_capacity_backing.len / 4096) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), fd_capacity_backing.len / 4096);
    const initial_pages = free_list.pageCount();
    const object = try createTestFdObject(&s, 71);
    const low = try s.installFd(p0, object, fdRights(.{ .dup = true, .close = true }), .{}, 16);
    try s.ensureFdTableCapacity(p0, 512, &free_list);
    try std.testing.expectEqual(@as(usize, 512), s.getFdTableConst(p0).?.capacity);
    const high = try s.dupFd(p0, low, 300, fdRights(.{ .dup = true, .close = true }), .{ .cloexec = true });
    try std.testing.expectEqual(@as(kernel.Fd, 300), high);
    try s.ensureFdTableCapacity(p0, 1024, &free_list);
    try std.testing.expectEqual(@as(?u32, 2), s.kernelObjectRefCount(object));
    try std.testing.expect(!s.getFdTableConst(p0).?.at(low).isEmpty());
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
    try std.testing.expectEqual(@as(usize, 0), s.getFdTableConst(p0).?.capacity);
}

test "dynamic fd metadata survives DMA return refusal and descriptor reuse" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{ .{ .id = 73, .start = @intFromPtr(&fd_capacity_backing), .end = (@intFromPtr(&fd_capacity_backing)) + (fd_capacity_backing.len / 4096) * 4096 }, .{ .id = 1, .start = 1 << 40, .end = (1 << 40) + (1 << 29) } });
    try free_list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), fd_capacity_backing.len / 4096);
    const initial_pages = free_list.pageCount();
    try s.ensureFdTableCapacity(p1, 512, &free_list);
    const allocated_pages = initial_pages - free_list.pageCount();
    // Keep the unrelated remainder allocated while testing issuer retention.
    var remainders: std.ArrayList(struct { physical: u64, pages: usize }) = .empty;
    defer remainders.deinit(std.testing.allocator);
    while (free_list.pageCount() != 0) {
        const remainder = try free_list.popContiguousOwnedInRange(free_list.rangeAt(0).len, 4096, std.math.maxInt(u64));
        try remainders.append(std.testing.allocator, .{ .physical = remainder.physical.toInt(), .pages = remainder.pages });
    }
    var other = FreePageList{};
    try other.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    var hold = try DmaHold.init(@intFromPtr(&fd_capacity_backing), allocated_pages);
    defer hold.end();
    s.releasePrincipalNativeMemory(p1, &other);
    try std.testing.expect(s.fd_retired.head != null);
    try std.testing.expect(s.removeProcessDescriptor(p1));
    try std.testing.expect(s.ensureProcessDescriptor(p1, "reused-retired-fds"));
    try std.testing.expect(s.fd_retired.head != null);
    hold.end();
    s.releasePrincipalNativeMemory(p1, &other);
    try std.testing.expect(s.getFdTable(p1).?.entries.body_owner.retired == null);
    try std.testing.expectEqual(allocated_pages, free_list.pageCount());
    try std.testing.expectEqual(@as(u64, 73), free_list.rangeAt(0).region_id);
    try std.testing.expectEqual(@as(usize, 0), other.pageCount());
    for (remainders.items) |remainder| try free_list.appendContiguousRange(remainder.physical, remainder.pages);
    try std.testing.expectEqual(initial_pages, free_list.pageCount());
    try std.testing.expect(allocated_pages > 0);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expectEqual(initial_pages, free_list.pageCount());
}

test "fd process capacity growth preserves extra fd tables" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = @intFromPtr(&fd_capacity_backing), .end = (@intFromPtr(&fd_capacity_backing)) + (fd_capacity_backing.len / 4096) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), fd_capacity_backing.len / 4096);

    try std.testing.expect(s.ensureProcessCapacity(kernel.process_count + 1, &free_list));
    const extra_principal = kernel.processPrincipalFromIndex(kernel.process_count).?;
    try std.testing.expect(s.ensureProcessDescriptor(extra_principal, "extra"));
    const table = s.getFdTable(extra_principal).?;
    try s.ensureFdTableCapacity(extra_principal, 512, &free_list);
    const owner_address = table.entries.body_owner.bound_address;
    const entry_root = table.at(300);
    const obj = try createTestFdObject(&s, 60);
    const fd = try s.installFd(extra_principal, obj, fdRights(.{ .dup = true }), fdFlags(.{ .cloexec = true }), 300);

    try std.testing.expect(s.ensureProcessCapacity(kernel.process_count * 2 + 1, &free_list));
    try std.testing.expectEqual(table, s.getFdTable(extra_principal).?);
    try std.testing.expectEqual(owner_address, table.entries.body_owner.bound_address);
    try std.testing.expectEqual(entry_root, table.at(300));
    try std.testing.expect(s.fd_tables_extra.directory_owner.retired == null);
    const entry = s.fdEntryConst(extra_principal, fd) orelse unreachable;
    try std.testing.expect(entry.flags.cloexec);
    try std.testing.expect(entry.rights.dup);
    try std.testing.expectEqual(@as(?u32, 1), s.kernelObjectRefCount(obj));
    // A bound entry Owner survives another growth and slot reuse, while all
    // capability references and dynamic entry grants are actually returned.
    try s.ensureFdTableCapacity(extra_principal, 1024, &free_list);
    s.releasePrincipalNativeMemory(extra_principal, &free_list);
    try std.testing.expect(s.removeProcessDescriptor(extra_principal));
    try std.testing.expect(s.ensureProcessDescriptor(extra_principal, "reused"));
    try std.testing.expectEqual(table, s.getFdTable(extra_principal).?);
    try std.testing.expectEqual(owner_address, table.entries.body_owner.bound_address);
    try std.testing.expect(table.entries.body_owner.published == null and table.entries.body_owner.retired == null);
    try std.testing.expectEqual(@as(?u32, null), s.kernelObjectRefCount(obj));
    releaseFixtureOwner(&s.fd_tables_extra.directory_owner);
    releaseFixtureOwner(&s.fd_tables_extra.body_owner);
    releaseFixtureOwner(&s.fd_tables_extra.metadata_owner);
    releaseFixtureOwner(&s.descriptor_storage);
    releaseFixtureOwner(&s.vma_directory_storage);
    releaseFixtureOwner(&s.vma_body_storage);
    try std.testing.expectEqual(fd_capacity_backing.len / 4096, free_list.pageCount());
}

test "process growth preserves writes through an outstanding stable VMA borrow" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list: FreePageList = .{};
    const pages = fd_capacity_backing.len / 4096;
    try free_list.installRegions(&.{.{ .id = 97, .start = @intFromPtr(&fd_capacity_backing), .end = (@intFromPtr(&fd_capacity_backing)) + (pages) * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), pages);
    const Fixture = struct {
        fn releaseAll(owner: anytype) void {
            // Test teardown only: all borrowed pointers are dead here. Save
            // links and the original issuer/grant before returning each header.
            inline for (.{ owner.published, owner.retired, owner.pending }) |head| {
                var cursor = head;
                while (cursor) |record| {
                    const next = record.next;
                    const allocator = record.allocator;
                    const allocation = record.allocation;
                    std.debug.assert(allocator.release(allocator.context, allocation));
                    cursor = next;
                }
            }
            owner.published = null;
            owner.retired = null;
            owner.pending = null;
        }
    };
    var released = false;
    defer if (!released) Fixture.releaseAll(&s.fd_tables_extra.directory_owner);
    defer if (!released) Fixture.releaseAll(&s.fd_tables_extra.body_owner);
    defer if (!released) Fixture.releaseAll(&s.fd_tables_extra.metadata_owner);
    defer if (!released) Fixture.releaseAll(&s.descriptor_storage);
    defer if (!released) Fixture.releaseAll(&s.vma_directory_storage);
    defer if (!released) Fixture.releaseAll(&s.vma_body_storage);
    try std.testing.expect(s.ensureProcessCapacity(kernel.process_count + 1, &free_list));
    const principal = kernel.processPrincipalFromIndex(kernel.process_count).?;
    try std.testing.expect(s.ensureProcessDescriptor(principal, "growth-borrow"));
    {
        const borrowed = s.getVmaTable(principal).?;
        try borrowed.reference_slots.prepareIndex(0, try s.poolAllocator());
        try borrowed.entries.prepareIndex(0, try s.poolAllocator());
        try borrowed.active_indices.prepareIndex(0, try s.poolAllocator());
        s.installVmaEntry(borrowed, 0, .{
            .active = true,
            .start_va = 0x400000,
            .size_bytes = 4096,
            .prot = .{ .read = true },
        });
        // Fault readers own AS/shared locks, while growth owns KernelState.
        // The same body must survive publication: retaining an old copied
        // body prevented UAF but stranded this later write in the old table.
        try std.testing.expect(s.ensureProcessCapacity(kernel.process_count * 2 + 1, &free_list));
        borrowed.entries.get(@intCast(0)).?.*.prot.write = true;
        const published = s.getVmaTable(principal).?;
        try std.testing.expectEqual(borrowed, published);
        const view: *const KernelState = &s;
        try std.testing.expectEqual(@as(*const kernel.VmaTable, borrowed), view.getVmaTableConst(principal).?);
        try std.testing.expect(borrowed.entries.get(@intCast(0)).?.*.prot.write);
        try std.testing.expect(published.entries.get(@intCast(0)).?.*.prot.write);
        try std.testing.expect(s.vma_directory_storage.retired == null);
        try std.testing.expect(s.fd_tables_extra.directory_owner.retired == null);
        try std.testing.expect(s.vma_body_storage.retired == null);
    }
    Fixture.releaseAll(&s.fd_tables_extra.directory_owner);
    Fixture.releaseAll(&s.fd_tables_extra.body_owner);
    Fixture.releaseAll(&s.fd_tables_extra.metadata_owner);
    Fixture.releaseAll(&s.descriptor_storage);
    Fixture.releaseAll(&s.vma_directory_storage);
    Fixture.releaseAll(&s.vma_body_storage);
    released = true;
    try std.testing.expectEqual(pages, free_list.pageCount());
    for (0..free_list.rangeCount()) |range_index| {
        const range = free_list.rangeAt(range_index);
        try std.testing.expectEqual(@as(u64, 97), range.region_id);
    }
}

test "descriptor snapshots survive growth and diagnostics never wait on interrupted writer" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var list: FreePageList = .{};
    const pages = fd_capacity_backing.len / 4096;
    try list.installRegions(&.{.{ .id = 100, .start = @intFromPtr(&fd_capacity_backing), .end = (@intFromPtr(&fd_capacity_backing)) + (pages) * 4096 }});
    try list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), pages);
    defer releaseFixtureOwner(&s.fd_tables_extra.directory_owner);
    defer releaseFixtureOwner(&s.fd_tables_extra.body_owner);
    defer releaseFixtureOwner(&s.fd_tables_extra.metadata_owner);
    defer releaseFixtureOwner(&s.descriptor_storage);
    defer releaseFixtureOwner(&s.vma_directory_storage);
    defer releaseFixtureOwner(&s.vma_body_storage);
    try std.testing.expect(s.ensureProcessCapacity(kernel.process_count + 1, &list));
    const principal = kernel.processPrincipalFromIndex(kernel.process_count).?;
    try std.testing.expect(s.ensureProcessDescriptor(principal, "stable-label"));
    try std.testing.expect(s.setProcessSignalPendingHintVa(principal, 0x12340000));
    const snapshot = s.processDescriptor(principal).?;
    const old_grant = s.descriptor_storage.published.?.allocation;
    try std.testing.expect(s.ensureProcessCapacity(kernel.process_count * 2 + 1, &list));
    try expectResourceEqualDeep(snapshot, s.processDescriptor(principal).?);
    try std.testing.expect(s.descriptor_storage.retired == null);
    var returned = false;
    for (0..list.rangeCount()) |range_index| {
        const range = list.rangeAt(range_index);
        if (range.region_id == old_grant.region_id and
            range.physical_start <= old_grant.physical.toInt() and
            range.physical_start + range.len * 4096 >= old_grant.physical.toInt() + old_grant.pages * 4096)
            returned = true;
    }
    try std.testing.expect(returned);
    @atomicStore(u8, &s.descriptor_lock.value, 1, .release);
    const busy = s.tryProcessLabel(principal);
    @atomicStore(u8, &s.descriptor_lock.value, 0, .release);
    try std.testing.expect(busy == null);
    try std.testing.expectEqualStrings("stable-label", s.tryProcessLabel(principal).?);
    try std.testing.expect(s.processDescriptorSlotConst(std.math.maxInt(usize)) == null);
    try std.testing.expect(s.removeProcessDescriptor(principal));
    try std.testing.expect(s.tryProcessLabel(principal) == null);
    try std.testing.expect(s.ensureProcessDescriptor(principal, "new-label"));
    try std.testing.expect(snapshot.generation != s.processDescriptor(principal).?.generation);
    try std.testing.expectEqualStrings("stable-label", snapshot.label);
    try std.testing.expectEqual(@as(u64, 0x12340000), snapshot.signal_pending_hint_va);
    const Hook = struct {
        fn run(state: *const KernelState, target: PrincipalId, _: kernel.DebugProcessLifecycleReason) void {
            std.debug.assert(@atomicLoad(u8, &state.descriptor_lock.value, .acquire) == 0);
            _ = state.processDescriptor(target);
        }
    };
    s.debug_process_lifecycle_hook = Hook.run;
    try std.testing.expect(s.markProcessExited(principal));
    try std.testing.expect(s.ensureProcessDescriptor(principal, "hook"));
    try std.testing.expect(s.markProcessFaulted(principal, 14));
}

test "descriptor held reader excludes old root retirement and final capacity publication" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var list: FreePageList = .{};
    try list.installRegions(&.{.{ .id = 101, .start = @intFromPtr(&fd_capacity_backing), .end = (@intFromPtr(&fd_capacity_backing)) + (fd_capacity_backing.len / 4096) * 4096 }});
    try list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), fd_capacity_backing.len / 4096);
    defer releaseFixtureOwner(&s.fd_tables_extra.directory_owner);
    defer releaseFixtureOwner(&s.fd_tables_extra.body_owner);
    defer releaseFixtureOwner(&s.fd_tables_extra.metadata_owner);
    defer releaseFixtureOwner(&s.descriptor_storage);
    defer releaseFixtureOwner(&s.vma_directory_storage);
    defer releaseFixtureOwner(&s.vma_body_storage);
    try std.testing.expect(s.ensureProcessCapacity(kernel.process_count + 1, &list));
    const principal = kernel.processPrincipalFromIndex(kernel.process_count).?;
    try std.testing.expect(s.ensureProcessDescriptor(principal, "held-reader"));
    const old_root = s.process_descriptors_extra;
    const old_record = s.descriptor_storage.published.?;
    const old_capacity = s.process_capacity;
    var contended: u8 = 0;
    s.descriptor_test_probe = &contended;
    defer s.descriptor_test_probe = null;
    @atomicStore(u8, &s.descriptor_lock.value, 1, .release);
    var held = true;
    defer if (held) @atomicStore(u8, &s.descriptor_lock.value, 0, .release);
    const Writer = struct {
        state: *KernelState,
        pmm: *FreePageList,
        done: u8 = 0,
        ok: bool = false,
        fn run(self: *@This()) void {
            self.ok = self.state.ensureProcessCapacity(kernel.process_count * 2 + 1, self.pmm);
            @atomicStore(u8, &self.done, 1, .release);
        }
    };
    var writer = Writer{ .state = &s, .pmm = &list };
    const worker = try std.Thread.spawn(.{}, Writer.run, .{&writer});
    defer worker.join();
    defer {
        if (held) @atomicStore(u8, &s.descriptor_lock.value, 0, .release);
        held = false;
    }
    while (@atomicLoad(u8, &contended, .acquire) == 0 and @atomicLoad(u8, &writer.done, .acquire) == 0)
        std.atomic.spinLoopHint();
    try std.testing.expectEqual(@as(u8, 1), @atomicLoad(u8, &contended, .acquire));
    try std.testing.expectEqual(@as(u8, 0), @atomicLoad(u8, &writer.done, .acquire));
    try std.testing.expectEqual(old_root.ptr, s.process_descriptors_extra.ptr);
    try std.testing.expectEqual(old_record, s.descriptor_storage.published.?);
    try std.testing.expectEqual(old_capacity, s.process_capacity);
    try std.testing.expectEqualStrings("held-reader", old_root[0].label);
    // Other roots are already installed, but final public capacity still
    // describes the old set until this last short borrow ends.
    try std.testing.expect(s.vma_tables_extra.len > old_root.len);
    @atomicStore(u8, &s.descriptor_lock.value, 0, .release);
    held = false;
    while (@atomicLoad(u8, &writer.done, .acquire) == 0) std.atomic.spinLoopHint();
    try std.testing.expect(writer.ok);
    try std.testing.expect(s.descriptor_storage.retired == null);
    try std.testing.expect(s.processCapacitySnapshot() > old_capacity);
    try std.testing.expectEqualStrings("held-reader", s.processDescriptor(principal).?.label);
}

test "VMA directory held reader excludes publication and old directory return" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var list: FreePageList = .{};
    try list.installRegions(&.{.{ .id = 98, .start = @intFromPtr(&fd_capacity_backing), .end = (@intFromPtr(&fd_capacity_backing)) + (fd_capacity_backing.len / 4096) * 4096 }});
    try list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), fd_capacity_backing.len / 4096);
    defer releaseFixtureOwner(&s.fd_tables_extra.directory_owner);
    defer releaseFixtureOwner(&s.fd_tables_extra.body_owner);
    defer releaseFixtureOwner(&s.fd_tables_extra.metadata_owner);
    defer releaseFixtureOwner(&s.descriptor_storage);
    defer releaseFixtureOwner(&s.vma_directory_storage);
    defer releaseFixtureOwner(&s.vma_body_storage);
    try std.testing.expect(s.ensureProcessCapacity(kernel.process_count + 1, &list));
    const old_root = s.vma_tables_extra;
    const old_record = s.vma_directory_storage.published.?;
    const body = old_root[0];
    var contended: u8 = 0;
    s.vma_directory_test_probe = &contended;
    defer s.vma_directory_test_probe = null;
    // Deliberately emulate a real directory reader inside the same leaf. The
    // atomic probe proves the writer reached publication, not merely reserve.
    @atomicStore(u8, &s.vma_directory_lock.value, 1, .release);
    var held = true;
    defer if (held) @atomicStore(u8, &s.vma_directory_lock.value, 0, .release);
    const Writer = struct {
        state: *KernelState,
        pmm: *FreePageList,
        done: u8 = 0,
        ok: bool = false,
        fn run(self: *@This()) void {
            self.ok = self.state.ensureProcessCapacity(kernel.process_count * 2 + 1, self.pmm);
            @atomicStore(u8, &self.done, 1, .release);
        }
    };
    var writer = Writer{ .state = &s, .pmm = &list };
    const worker = try std.Thread.spawn(.{}, Writer.run, .{&writer});
    defer worker.join();
    // On failed assertions unlock before join, including error unwinding.
    defer {
        if (held) @atomicStore(u8, &s.vma_directory_lock.value, 0, .release);
        held = false;
    }
    while (@atomicLoad(u8, &contended, .acquire) == 0 and @atomicLoad(u8, &writer.done, .acquire) == 0)
        std.atomic.spinLoopHint();
    try std.testing.expectEqual(@as(u8, 1), @atomicLoad(u8, &contended, .acquire));
    try std.testing.expectEqual(@as(u8, 0), @atomicLoad(u8, &writer.done, .acquire));
    try std.testing.expectEqual(old_root.ptr, s.vma_tables_extra.ptr);
    try std.testing.expectEqual(old_record, s.vma_directory_storage.published.?);
    try std.testing.expectEqual(body, old_root[0]);
    @atomicStore(u8, &s.vma_directory_lock.value, 0, .release);
    held = false;
    while (@atomicLoad(u8, &writer.done, .acquire) == 0) std.atomic.spinLoopHint();
    try std.testing.expect(writer.ok);
    try std.testing.expect(s.vma_tables_extra.ptr != old_root.ptr);
    try std.testing.expectEqual(body, s.vma_tables_extra[0]);
    try std.testing.expect(s.vma_directory_storage.retired == null);
    try std.testing.expect(s.fd_tables_extra.directory_owner.retired == null);
    try std.testing.expect(s.vma_body_storage.retired == null);
    try std.testing.expect(s.vmaTableForProcessIndex(s.process_capacity) == null);
    try std.testing.expect(s.vmaTableForProcessIndex(std.math.maxInt(usize)) == null);
}

test "VMA directory retries PMM return rejection using original issuer without growth" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var list: FreePageList = .{};
    try list.installRegions(&.{ .{ .id = 99, .start = @intFromPtr(&fd_capacity_backing), .end = (@intFromPtr(&fd_capacity_backing)) + (fd_capacity_backing.len / 4096) * 4096 }, .{ .id = 900, .start = 1 << 40, .end = (1 << 40) + (1 << 29) } });
    try list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), fd_capacity_backing.len / 4096);
    defer {
        releaseFixtureOwner(&s.fd_tables_extra.directory_owner);
        releaseFixtureOwner(&s.fd_tables_extra.body_owner);
        releaseFixtureOwner(&s.fd_tables_extra.metadata_owner);
        releaseFixtureOwner(&s.descriptor_storage);
        releaseFixtureOwner(&s.vma_directory_storage);
        releaseFixtureOwner(&s.vma_body_storage);
    }
    try std.testing.expect(s.ensureProcessCapacity(kernel.process_count + 1, &list));
    const old_grant = s.vma_directory_storage.published.?.allocation;
    const old_descriptor_grant = s.descriptor_storage.published.?.allocation;
    const body = s.vmaTableForProcessIndex(kernel.process_count).?;
    // Radix path returns can split the free list; every piece must retain
    // its original region even though the former single-array layout is gone.
    try std.testing.expect(list.rangeCount() != 0);
    for (0..list.rangeCount()) |i| try std.testing.expectEqual(@as(u64, 99), list.rangeAt(i).region_id);
    var directory_hold = try DmaHold.init(old_grant.physical.toInt(), old_grant.pages);
    defer directory_hold.end();
    var descriptor_hold = try DmaHold.init(old_descriptor_grant.physical.toInt(), old_descriptor_grant.pages);
    defer descriptor_hold.end();
    try std.testing.expect(s.ensureProcessCapacity(kernel.process_count * 2 + 1, &list));
    try std.testing.expect(s.vma_directory_storage.retired != null);
    const after_failure = list.pageCount();
    const root = s.vma_tables_extra.ptr;
    const capacity = s.process_capacity;
    var other: FreePageList = .{};
    try other.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    try std.testing.expect(s.ensureProcessCapacity(capacity, &other));
    try std.testing.expect(s.vma_directory_storage.retired != null);
    directory_hold.end();
    try std.testing.expect(s.ensureProcessCapacity(capacity, &other));
    try std.testing.expect(s.vma_directory_storage.retired == null);
    try std.testing.expectEqual(after_failure + old_grant.pages, list.pageCount());
    try std.testing.expect(s.descriptor_storage.retired != null);
    descriptor_hold.end();
    try std.testing.expect(s.ensureProcessCapacity(capacity, &other));
    try std.testing.expect(s.descriptor_storage.retired == null);
    // Descriptor and VMA returns preserve their original issuer independently
    // of the canonical FD radix paths.
    try std.testing.expect(s.fd_tables_extra.directory_owner.retired == null);
    try std.testing.expectEqual(after_failure + old_grant.pages + old_descriptor_grant.pages, list.pageCount());
    try std.testing.expect(s.ensureProcessCapacity(capacity, &other));
    try std.testing.expectEqual(after_failure + old_grant.pages + old_descriptor_grant.pages, list.pageCount());
    try std.testing.expectEqual(@as(usize, 0), other.pageCount());
    try std.testing.expectEqual(root, s.vma_tables_extra.ptr);
    try std.testing.expectEqual(capacity, s.process_capacity);
    try std.testing.expectEqual(body, s.vmaTableForProcessIndex(kernel.process_count).?);
    var returned = false;
    for (0..list.rangeCount()) |range_index| {
        const range = list.rangeAt(range_index);
        // The original returned grant may coalesce with a radix-path return.
        if (range.physical_start <= old_grant.physical.toInt() and
            range.physical_start + range.len * 4096 >= old_grant.physical.toInt() + old_grant.pages * 4096)
        {
            try std.testing.expectEqual(@as(u64, 99), range.region_id);
            returned = true;
        }
    }
    try std.testing.expect(returned);
    try std.testing.expect(s.ensureProcessCapacity(capacity, &other));
    try std.testing.expectEqual(after_failure + old_grant.pages + old_descriptor_grant.pages, list.pageCount());
}

test "process storage growth rolls back every partial allocation preserving PMM region" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    const Owner = @TypeOf(@as(KernelState, undefined).fd_tables_extra.directory_owner);
    const sizes = [_]usize{
        try Owner.allocationPages(kernel.ProcessDescriptor, kernel.process_count),
        try Owner.allocationPages(*kernel.FdTable, kernel.process_count),
        try Owner.allocationPages(kernel.FdTable, kernel.process_count),
        try Owner.allocationPages(*kernel.VmaTable, kernel.process_count),
        try Owner.allocationPages(kernel.VmaTable, kernel.process_count),
    };
    var available: usize = 0;
    for (sizes) |size| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var list: FreePageList = .{};
        try list.installRegions(&.{.{ .id = 53, .start = @intFromPtr(&fd_capacity_backing), .end = (@intFromPtr(&fd_capacity_backing)) + (fd_capacity_backing.len / 4096) * 4096 }});
        if (available != 0) try list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), available);
        const old_desc = s.process_descriptors_extra.ptr;
        const old_vma = s.vma_tables_extra.ptr;
        try std.testing.expect(!s.ensureProcessCapacity(kernel.process_count + 1, &list));
        try std.testing.expectEqual(kernel.process_count, s.process_capacity);
        try std.testing.expectEqual(old_desc, s.process_descriptors_extra.ptr);
        try std.testing.expectEqual(kernel.process_count, s.fd_process_capacity);
        // Return any private prepared FD chunks before comparing all PMM pages.
        s.fd_tables_extra.deinit();
        try std.testing.expectEqual(old_vma, s.vma_tables_extra.ptr);
        try std.testing.expectEqual(@as(usize, 0), s.vma_tables_extra.len);
        try std.testing.expectEqual(available, list.pageCount());
        for (0..list.rangeCount()) |range_index| {
            const range = list.rangeAt(range_index);
            try std.testing.expectEqual(@as(u64, 53), range.region_id);
        }
        try std.testing.expect(s.fd_tables_extra.directory_owner.pending == null and s.fd_tables_extra.directory_owner.published == null);
        try std.testing.expect(s.descriptor_storage.pending == null and s.descriptor_storage.published == null);
        try std.testing.expect(s.vma_directory_storage.pending == null and s.vma_directory_storage.published == null);
        try std.testing.expect(s.vma_body_storage.pending == null and s.vma_body_storage.published == null);
        available += size;
    }
}

test "PMM owned grant preserves nonzero region across removal swap split and return" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var list: FreePageList = .{};
    try list.installRegions(&.{ .{ .id = 17, .start = 0x100000, .end = 0x104000 }, .{ .id = 29, .start = 0x200000, .end = 0x208000 } });
    try list.seedContiguousRange(0x100000, 4);
    try list.seedContiguousRange(0x200000, 8);
    const first = try list.popContiguousOwnedInRange(4, 0x100000, 0x104000);
    try std.testing.expectEqual(@as(u64, 17), first.region_id);
    try std.testing.expectEqual(@as(u64, 0x100000), first.physical.toInt());
    try std.testing.expectEqual(@as(u64, 29), list.rangeAt(0).region_id);
    const middle = try list.popContiguousOwnedInRange(2, 0x202000, 0x204000);
    try std.testing.expectEqual(@as(u64, 29), middle.region_id);
    try std.testing.expectEqual(@as(usize, 6), list.pageCount());
    try list.appendContiguousRange(middle.physical.toInt(), middle.pages);
    try list.appendContiguousRange(first.physical.toInt(), first.pages);
    try std.testing.expectEqual(@as(usize, 12), list.pageCount());
    try expectFreeRangeInvariants(&list);
}

test "ipc endpoint fd sends and receives inline words" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const pair = try s.createIpcChannelPairFds(p0, fdRights(.{ .send = true, .recv = true, .close = true }), .{}, 16);

    try s.ipcSend(p0, pair.a, .{ .words = .{ 5, 6, 7, 8 } }, &free_list);
    try std.testing.expectError(KernelError.MailboxEmpty, s.ipcRecv(p0, pair.a, 0, 16, &free_list));
    const received = try s.ipcRecv(p0, pair.b, 0, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 5), received.words[0]);
    try std.testing.expectEqual(@as(u64, 8), received.words[3]);
}

test "ipc writable waiter wakes when receive frees a full channel queue" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    const s = try std.testing.allocator.create(KernelState);
    defer std.testing.allocator.destroy(s);
    try initFdState(s);
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
    try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, pair.a, fd_abi.event_hangup, 0x2000, 32, 1, 33, extra));
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
    try std.testing.expectEqual(@as(usize, 0), s.fd_wait_group_thread_counts.live);
}

test "signal wake cancellation clears ipc recv completion waiter" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const rights = fdRights(.{
        .send = true,
        .recv = true,
        .wait = true,
        .close = true,
    });
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    _ = try s.registerIpcRecvCompletionWaiterForFd(p0, pair.a, .{ .active = true, .array = 0x2000 }, 2, 7, 11, 1, group);
    try s.armFdWaitGroup(group);

    s.unregisterFdWaitersForThread(p0, 7, 11);

    var targets: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), try s.wakeIpcWaitersForSendFd(p0, pair.b, targets[0..]));
}

test "fd wait group cancellation survives a destroyed object in the registration map" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const collision = 7 + former_max_fd_wait_groups;
    try std.testing.expectEqual(@as(u64, 0), groupCount(&s, 7));
    s.cancelFdWaitGroupsForThread(p0, 7, 11);
    const first = try s.beginFdWaitGroup(p0, 7, 11, 1);
    _ = try s.beginFdWaitGroup(p0, 7, 11, 2);
    const other_thread = try s.beginFdWaitGroup(p1, collision, 11, 3);
    const other_generation = try s.beginFdWaitGroup(p0, 7, 12, 4);
    try std.testing.expectEqual(@as(u64, 3), groupCount(&s, 7));
    s.cancelFdWaitGroup(first, 99);
    s.cancelFdWaitGroupsForThread(p1, 7, 11);
    try std.testing.expectEqual(@as(u64, 3), groupCount(&s, 7));
    s.cancelFdWaitGroupsForThread(p0, 7, 11);
    try std.testing.expectEqual(@as(u64, 1), groupCount(&s, 7));
    try std.testing.expect(s.fd_wait_groups.get(@intCast(other_thread.index)).?.*.state == .building);
    try std.testing.expect(s.fd_wait_groups.get(@intCast(other_generation.index)).?.*.state == .building);
    s.cancelFdWaitGroup(first, 1);
    try std.testing.expectEqual(@as(u64, 1), groupCount(&s, 7));
    s.cancelFdWaitGroupsForOwner(p1);
    try std.testing.expectEqual(@as(u64, 1), groupCount(&s, 7));
    s.cancelFdWaitGroupsForThread(p0, 7, 12);
    try std.testing.expectEqual(@as(u64, 0), groupCount(&s, 7));
}

test "fd wait group counts survive failed setup full capacity and reset" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    try std.testing.expectError(KernelError.InvalidState, s.beginFdWaitGroup(p0, 7, 11, 0));
    const failed_setup = try s.beginFdWaitGroup(p0, 7, 11, 1);
    try std.testing.expectError(KernelError.InvalidState, s.linkFdWaitRegistration(failed_setup, .{}));
    try s.armFdWaitGroup(failed_setup);
    try std.testing.expectError(KernelError.InvalidState, s.armFdWaitGroup(failed_setup));
    s.cancelFdWaitGroup(failed_setup, 1);
    try std.testing.expectEqual(@as(u64, 0), groupCount(&s, 7));
    for (0..former_max_fd_wait_groups) |i|
        _ = try s.beginFdWaitGroup(p0, 7, 11, i + 1);
    _ = try s.beginFdWaitGroup(p0, 7, 11, 999);
    try std.testing.expectEqual(@as(u64, former_max_fd_wait_groups + 1), groupCount(&s, 7));
    s.cancelFdWaitGroupsForOwner(p0);
    try std.testing.expectEqual(@as(u64, 0), groupCount(&s, 7));
    _ = try s.beginFdWaitGroup(p0, 7, 11, 1000);
    // Serial fixture teardown: no allocated backing or surviving borrower.
    // Construct a fresh state, rather than claiming a live-reset contract.
    try reconstructUnbackedFixtureInPlace(&s);
    try std.testing.expectEqual(@as(usize, 0), s.fd_wait_group_thread_counts.live);
}

fn expectTimerWaiterCount(s: *const KernelState, expected: usize) !void {
    var actual: usize = 0;
    {
        var pool_iter_39 = s.task_fd_waiters.iterator();
        while (pool_iter_39.next()) |pool_entry_39| {
            const waiter = pool_entry_39.value.*;
            const slot_index: usize = pool_entry_39.ref.index;
            const is_timer = waiter.active and waiter.object.kind == .timer;
            if (is_timer) actual += 1;
            try std.testing.expectEqual(is_timer, s.task_fd_waiters.isMarked(@intCast(slot_index), 1));
        }
    }
    try std.testing.expectEqual(expected, actual);
    try std.testing.expectEqual(actual, markedCount(&s.task_fd_waiters, 1));
}

test "timer waiter count preserves duplicates mixed fds deadlines and exact cleanup" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const rights = fdRights(.{ .wait = true, .poll = true, .write = true, .close = true });
    const timer = try s.createTimerFd(p0, 100, 0, .{}, rights, 16);
    const event = try s.createEventFd(p0, 0, .{}, rights, 16);
    var targets: [2]kernel.ThreadWakeTarget = undefined;
    try expectTimerWaiterCount(&s, 0);
    try std.testing.expectEqual(@as(?u64, null), s.nextTimerWaiterDeadline());
    try std.testing.expectEqual(@as(usize, 0), s.takeReadyTimerWaiters(100, &targets));
    const first = try s.beginFdWaitGroup(p0, 7, 11, 1);
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, event, fd_abi.event_readable, 0x1000, 7, 11, 1, first));
    try expectTimerWaiterCount(&s, 0);
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, timer, fd_abi.event_readable, 0x1018, 7, 11, 1, first));
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, timer, fd_abi.event_readable | fd_abi.event_hangup, 0x1018, 7, 11, 1, first));
    try expectTimerWaiterCount(&s, 1);
    const second = try s.beginFdWaitGroup(p0, 8, 11, 2);
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, timer, fd_abi.event_readable, 0x2000, 8, 11, 2, second));
    try expectTimerWaiterCount(&s, 2);
    try std.testing.expectEqual(@as(?u64, 100), s.nextTimerWaiterDeadline());
    try s.setTimerFd(p0, timer, 50, 0, 0);
    try std.testing.expectEqual(@as(?u64, 50), s.nextTimerWaiterDeadline());
    try s.setTimerFd(p0, timer, 200, 0, 0);
    try std.testing.expectEqual(@as(?u64, 200), s.nextTimerWaiterDeadline());
    try s.setTimerFd(p0, timer, 0, 0, 0);
    try expectTimerWaiterCount(&s, 2);
    try std.testing.expectEqual(@as(?u64, null), s.nextTimerWaiterDeadline());
    try std.testing.expectEqual(@as(usize, 0), s.takeReadyTimerWaiters(999, &targets));
    s.cancelFdWaitGroup(first, 99);
    s.unregisterTaskReadableWaiterForThread(7, 12);
    try expectTimerWaiterCount(&s, 2);
    s.unregisterTaskReadableWaiterForThread(7, 11);
    try expectTimerWaiterCount(&s, 1);
    s.cancelFdWaitGroup(first, 1);
    try expectTimerWaiterCount(&s, 1);
    s.cancelFdWaitGroupsForThread(p0, 8, 11);
    try expectTimerWaiterCount(&s, 0);
    try std.testing.expectEqual(@as(?u64, null), s.nextTimerWaiterDeadline());
    try s.setTimerFd(p0, timer, 300, 0, 0);
    const next = try s.beginFdWaitGroup(p0, 7, 12, 3);
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, timer, fd_abi.event_readable, 0x3000, 7, 12, 3, next));
    try expectTimerWaiterCount(&s, 1);
    try std.testing.expectEqual(@as(?u64, 300), s.nextTimerWaiterDeadline());
    s.cancelFdWaitGroupsForOwner(p0);
    try expectTimerWaiterCount(&s, 0);
}

test "timer waiter count retains pending completion until exact group retirement" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const timer = try s.createTimerFd(p0, 100, 0, .{}, fdRights(.{ .wait = true, .poll = true }), 16);
    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    _ = try s.registerTaskReadableWaiterForFd(p0, timer, fd_abi.event_readable, 0x1000, 7, 11, 1, group);
    try s.armFdWaitGroup(group);
    var targets: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), s.takeReadyTimerWaiters(99, &targets));
    try std.testing.expectEqual(@as(usize, 1), s.takeReadyTimerWaiters(100, &targets));
    try expectTimerWaiterCount(&s, 1);
    try std.testing.expectEqual(@as(?u64, null), s.nextTimerWaiterDeadline());
    try std.testing.expectEqual(@as(usize, 0), s.takeReadyTimerWaiters(100, &targets));
    s.unregisterTaskReadableWaiterForThread(7, 11);
    s.cancelFdWaitGroup(group, 2);
    s.cancelFdWaitGroup(.{ .index = group.index, .generation = group.generation + 1 }, 1);
    try expectTimerWaiterCount(&s, 1);
    s.cancelFdWaitGroup(targets[0].group, targets[0].wait_token);
    try expectTimerWaiterCount(&s, 0);
    s.cancelFdWaitGroup(group, 1);
    try expectTimerWaiterCount(&s, 0);
}

test "timer waiter count survives close reuse and object table reset" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const rights = fdRights(.{ .wait = true, .poll = true, .close = true });
    const timer = try s.createTimerFd(p0, 100, 0, .{}, rights, 16);
    const old = s.fdEntryConst(p0, timer).?.object;
    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    _ = try s.registerTaskReadableWaiterForFd(p0, timer, fd_abi.event_readable, 0x1000, 7, 11, 1, group);
    try s.closeFdWithFreeList(p0, timer, &free_list);
    try expectTimerWaiterCount(&s, 1);
    try std.testing.expectEqual(@as(?u64, null), s.nextTimerWaiterDeadline());
    // Force reuse through the allocator, not by modifying a waiter record.
    s.next_fd_object_scan = old.index;
    const replacement = try s.createTimerFd(p0, 10, 0, .{}, rights, 16);
    const current = s.fdEntryConst(p0, replacement).?.object;
    try std.testing.expectEqual(old.index, current.index);
    try std.testing.expect(old.generation != current.generation);
    var targets: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(?u64, null), s.nextTimerWaiterDeadline());
    try std.testing.expectEqual(@as(usize, 0), s.takeReadyTimerWaiters(100, &targets));
    s.cancelFdWaitGroup(group, 1);
    try expectTimerWaiterCount(&s, 0);
    const next = try s.beginFdWaitGroup(p0, 7, 11, 2);
    _ = try s.registerTaskReadableWaiterForFd(p0, replacement, fd_abi.event_readable, 0x1000, 7, 11, 2, next);
    s.resetKernelObjectTable();
    try expectTimerWaiterCount(&s, 1);
    try std.testing.expectEqual(@as(?u64, null), s.nextTimerWaiterDeadline());
    s.cancelFdWaitGroupsForOwner(p0);
    try expectTimerWaiterCount(&s, 0);
}

test "timer waiter count rolls back failed links and full capacity and resets" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const rights = fdRights(.{ .wait = true, .poll = true });
    const timer = try s.createTimerFd(p0, 100, 0, .{}, rights, 16);
    const event = try s.createEventFd(p0, 0, .{}, rights, 16);
    const failed = try s.beginFdWaitGroup(p0, 7, 11, 1);
    try s.armFdWaitGroup(failed);
    try std.testing.expectError(KernelError.InvalidState, s.registerTaskReadableWaiterForFd(p0, timer, fd_abi.event_readable, 0x1000, 7, 11, 1, failed));
    try expectTimerWaiterCount(&s, 0);
    {
        var pool_iter_40 = s.task_fd_waiters.iterator();
        while (pool_iter_40.next()) |pool_entry_40| {
            const waiter = pool_entry_40.value.*;
            try std.testing.expect(!waiter.active);
        }
    }
    s.cancelFdWaitGroup(failed, 1);
    for (0..former_max_task_fd_waiters) |i| {
        const group = try s.beginFdWaitGroup(p0, i, 11, i + 2);
        _ = try s.registerTaskReadableWaiterForFd(p0, if (i % 2 == 0) timer else event, fd_abi.event_readable, 0x1000 + 24 * i, i, 11, i + 2, group);
        try s.armFdWaitGroup(group);
    }
    try expectTimerWaiterCount(&s, former_max_task_fd_waiters / 2);
    var targets: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), s.takeReadyTimerWaiters(100, targets[0..0]));
    for (0..former_max_task_fd_waiters / 2) |i| {
        try std.testing.expectEqual(@as(usize, 1), s.takeReadyTimerWaiters(100, &targets));
        try std.testing.expectEqual(@as(u16, @intCast(2 * i)), targets[0].registration.slot);
        try expectTimerWaiterCount(&s, former_max_task_fd_waiters / 2);
    }
    try std.testing.expectEqual(@as(usize, 0), s.takeReadyTimerWaiters(100, &targets));
    try std.testing.expectEqual(@as(?u64, null), s.nextTimerWaiterDeadline());
    const first = s.task_fd_waiters.get(@intCast(0)).?.*.binding.group;
    try std.testing.expectError(KernelError.InvalidState, s.registerTaskReadableWaiterForFd(p0, timer, fd_abi.event_readable, 0x5000, 0, 11, 2, first));
    try expectTimerWaiterCount(&s, former_max_task_fd_waiters / 2);
    s.cancelFdWaitGroupsForOwner(p0);
    try expectTimerWaiterCount(&s, 0);
    const next = try s.beginFdWaitGroup(p0, 7, 11, 1000);
    _ = try s.registerTaskReadableWaiterForFd(p0, timer, fd_abi.event_readable, 0x1000, 7, 11, 1000, next);
    try expectTimerWaiterCount(&s, 1);
    // This fixture owns no PMM extents; discard its logical timer objects and
    // construct fresh storage. Production live owners cannot use constructors.
    try reconstructUnbackedFixtureInPlace(&s);
    try expectTimerWaiterCount(&s, 0);
}

test "timer waiter index preserves sparse last slot and minimum deadline order" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const rights = fdRights(.{ .wait = true, .poll = true, .write = true });
    const later = try s.createTimerFd(p0, 300, 0, .{}, rights, 16);
    const earlier = try s.createTimerFd(p0, 100, 0, .{}, rights, 16);
    const event = try s.createEventFd(p0, 0, .{}, rights, 16);
    for (0..former_max_task_fd_waiters) |i| {
        const group = try s.beginFdWaitGroup(p0, i, 11, i + 1);
        const fd = if (i == 63) later else if (i == former_max_task_fd_waiters - 1) earlier else event;
        _ = try s.registerTaskReadableWaiterForFd(p0, fd, fd_abi.event_readable, 0x1000 + 24 * i, i, 11, i + 1, group);
        try s.armFdWaitGroup(group);
    }
    try expectTimerWaiterCount(&s, 2);
    try std.testing.expect(s.task_fd_waiters.isMarked(@intCast(former_max_task_fd_waiters - 1), 1));
    try std.testing.expectEqual(@as(?u64, 100), s.nextTimerWaiterDeadline());
    var targets: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), s.takeReadyTimerWaiters(100, targets[0..0]));
    try std.testing.expectEqual(@as(?u64, 100), s.nextTimerWaiterDeadline());
    try std.testing.expectEqual(@as(usize, 1), s.takeReadyTimerWaiters(100, &targets));
    try std.testing.expectEqual(@as(u16, former_max_task_fd_waiters - 1), targets[0].registration.slot);
    try expectTimerWaiterCount(&s, 2);
    try std.testing.expectEqual(@as(?u64, 300), s.nextTimerWaiterDeadline());
    try std.testing.expectEqual(@as(usize, 0), s.takeReadyTimerWaiters(100, &targets));
    s.cancelFdWaitGroup(targets[0].group, targets[0].wait_token);
    try expectTimerWaiterCount(&s, 1);
    try s.setTimerFd(p0, later, 50, 0, 0);
    try std.testing.expectEqual(@as(?u64, 50), s.nextTimerWaiterDeadline());
    try std.testing.expectEqual(@as(usize, 1), s.takeReadyTimerWaiters(100, &targets));
    try std.testing.expectEqual(@as(u16, 63), targets[0].registration.slot);
    try expectTimerWaiterCount(&s, 1);
    s.cancelFdWaitGroupsForOwner(p0);
    try expectTimerWaiterCount(&s, 0);
    try std.testing.expectEqual(@as(?u64, null), s.nextTimerWaiterDeadline());
}

test "delayed target cannot cancel a reused fd wait group slot" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    while (i < former_max_fd_wait_groups - 1) : (i += 1) {
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
    try std.testing.expectEqual(@as(u64, 1), groupCount(&s, 7));
    var current: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(
        @as(usize, 1),
        try s.wakeIpcWaitersForSendFd(p0, new_pair.b, current[0..]),
    );
    try std.testing.expect(current[0].group.matches(new_group));
}

test "duplicate poll entries stay grouped until completion" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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

test "ipc call has no automatic fd and extraction owns reply authority" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const endpoint = try s.createIpcEndpointFd(p0, fdRights(.{ .recv = true, .call = true, .transfer = true, .close = true }), .{}, 16);
    const client_endpoint = try s.transferFd(p0, p1, endpoint, 16, fdRights(.{ .call = true, .close = true }), .{}, .copy);

    const before = try s.fdFreeCountFrom(p1, 16);
    const call = try s.ipcCall(p1, client_endpoint, .{ .words = .{ 99, 0, 0, 0 } }, .{ .owner = p1 }, &free_list);
    try std.testing.expectEqual(before, try s.fdFreeCountFrom(p1, 16));
    const request = try s.ipcRecv(p0, endpoint, 0, 16, &free_list);
    try std.testing.expectEqual(@as(u64, 99), request.words[0]);
    try std.testing.expectEqual(@as(usize, 0), request.fd_count);
    try std.testing.expectEqual(call, request.call);
    const extracted = try s.ipcExtractReply(p0, request.call, 16);
    const entry = s.fdEntryConst(p0, extracted).?;
    try std.testing.expect(entry.rights.send and entry.rights.transfer and !entry.rights.recv);
    try s.closeFdWithFreeList(p0, extracted, &free_list);
    s.releaseIpcCallReference(call);
    try std.testing.expect(s.ipcReplySlot(call) == null);
}

test "ipc receiver handoff hints and call caller identity are independent" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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

    const call = try s.ipcCall(p1, client_endpoint, .{}, .{ .owner = p1, .thread_index = 9, .thread_generation = 13, .wait_token = 7 }, &free_list);
    const request = try s.ipcRecv(p0, endpoint, 0, 16, &free_list);
    const caller = s.ipcReplySlot(call).?.caller;
    try std.testing.expectEqual(@as(usize, 9), caller.thread_index);
    try std.testing.expectEqual(@as(u32, 13), caller.thread_generation);
    try std.testing.expectEqual(p1, caller.owner);
    s.releaseIpcCallAuthority(request.call);
    s.releaseIpcCallReference(call);
}

test "ipc channel receiver hints isolate sides permissions and recycled objects" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const rights = fdRights(.{ .send = true, .recv = true, .transfer = true, .close = true });
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    try std.testing.expect(s.noteIpcReceiverForFd(p0, pair.a, 7, 11));
    try std.testing.expect(s.ipcReceiverHandoffHintForSendFd(p0, pair.a) == null);
    try std.testing.expect(s.noteIpcReceiverForFd(p0, pair.b, 9, 13));
    try std.testing.expectEqual(@as(usize, 9), s.ipcReceiverHandoffHintForSendFd(p0, pair.a).?.thread_index);
    try std.testing.expectEqual(@as(usize, 7), s.ipcReceiverHandoffHintForSendFd(p0, pair.b).?.thread_index);
    const recv_only = try s.transferFd(p0, p1, pair.a, 16, fdRights(.{ .recv = true, .close = true }), .{}, .copy);
    const send_only = try s.transferFd(p0, p1, pair.a, 16, fdRights(.{ .send = true, .close = true }), .{}, .copy);
    try std.testing.expect(s.ipcReceiverHandoffHintForSendFd(p1, recv_only) == null);
    try std.testing.expect(!s.noteIpcReceiverForFd(p1, send_only, 10, 14));
    try std.testing.expect(!s.noteIpcReceiverForFd(p0, pair.a, 7, 0));
    const object = s.fdEntryConst(p0, pair.a).?.object;
    const slot = s.kernelObjectSlot(object).?;
    const handle = slot.payload.channel;
    slot.payload.channel.side = 2;
    try std.testing.expect(!s.noteIpcReceiverForFd(p0, pair.a, 10, 14));
    try std.testing.expect(s.ipcReceiverHandoffHintForSendFd(p0, pair.a) == null);
    slot.payload.channel = handle;
    try s.closeFd(p1, recv_only);
    try s.closeFd(p1, send_only);
    try s.closeFd(p0, pair.a);
    try s.closeFd(p0, pair.b);
    try std.testing.expect(s.ipcChannelSlotConst(handle.channel) == null);
    s.next_ipc_channel_scan = handle.channel.index;
    const reused = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const next_handle = s.kernelObjectSlot(s.fdEntryConst(p0, reused.a).?.object).?.payload.channel;
    try std.testing.expectEqual(handle.channel.index, next_handle.channel.index);
    try std.testing.expect(handle.channel.generation != next_handle.channel.generation);
    try std.testing.expect(s.ipcReceiverHandoffHintForSendFd(p0, reused.a) == null);
    try std.testing.expect(s.ipcReceiverHandoffHintForSendFd(p0, reused.b) == null);
}

test "irq fd records only matching device and MSI-X entry" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const irq_fd = try s.createIrqFd(p0, .{
        .source = .{ .pci = 0x1001 },
        .kind = .msix,
        .vector = 1,
    }, fdRights(.{ .irq_wait = true, .poll = true, .read = true, .close = true }), .{}, 16);
    const auto_fd = try s.createIrqFd(p0, .{
        .source = .{ .pci = 0x1001 },
        .kind = .auto,
        .vector = 0,
    }, fdRights(.{ .irq_wait = true, .poll = true, .read = true, .close = true }), .{}, 16);
    const other_device_fd = try s.createIrqFd(p0, .{
        .source = .{ .pci = 0x1002 },
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const rights = fdRights(.{ .irq_wait = true, .poll = true, .read = true, .close = true, .dup = true });
    const fd = try s.createIrqFd(p0, .{ .source = .{ .pci = 0x1001 }, .kind = .msix, .vector = 1 }, rights, .{}, 16);
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
    const replacement = try s.createIrqFd(p0, .{ .source = .{ .pci = 0x1001 }, .kind = .msix, .vector = 1 }, rights, .{}, 16);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const fd = try s.createIrqFd(p0, .{
        .source = .{ .pci = 0x1001 },
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
    var current_fd = fd;
    var current_ref = original;
    for (0..256) |_| {
        try s.closeFd(p0, current_fd);
        try std.testing.expect(s.kernelObjectSlot(current_ref) == null);
        const replacement = try s.createIrqFd(p0, .{
            .source = .{ .pci = 0x1002 },
            .kind = .msix,
            .vector = 1,
        }, fdRights(.{ .irq_wait = true, .close = true }), .{}, 16);
        const replacement_ref = s.fdEntryConst(p0, replacement).?.object;
        try std.testing.expectEqual(current_ref.index, replacement_ref.index);
        try std.testing.expect(current_ref.generation != replacement_ref.generation);
        for (0..128) |_| std.atomic.spinLoopHint();
        try std.testing.expectEqual(@as(?u64, 0), s.irqPublishedEventCount(replacement_ref));
        try s.closeFd(p0, replacement);
        current_fd = try s.createIrqFd(p0, .{
            .source = .{ .pci = 0x1001 },
            .kind = .msix,
            .vector = 1,
        }, fdRights(.{ .irq_wait = true, .close = true }), .{}, 16);
        current_ref = s.fdEntryConst(p0, current_fd).?.object;
        while (s.irqPublishedEventCount(current_ref).? == 0) std.atomic.spinLoopHint();
    }
    try s.closeFd(p0, current_fd);
}

test "concurrent IRQ producers add each event exactly once" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    const fd = try s.createIrqFd(p0, .{ .source = .{ .pci = 0x1001 }, .kind = .msix, .vector = 1 }, fdRights(.{ .irq_wait = true, .close = true }), .{}, 16);
    const ref = s.fdEntryConst(p0, fd).?.object;
    const Producer = struct {
        fn run(state: *KernelState) void {
            var owners: [1]PrincipalId = undefined;
            for (0..10000) |_| _ = state.recordDeviceInterruptEvent(0x1001, 1, &owners);
        }
    };
    var threads: [4]std.Thread = undefined;
    for (&threads) |*thread| thread.* = try std.Thread.spawn(.{}, Producer.run, .{&s});
    for (threads) |thread| thread.join();
    try std.testing.expectEqual(@as(?u64, 40000), s.irqPublishedEventCount(ref));
    try s.closeFd(p0, fd);
}

test "irq directory filters live candidates and preserves generation on reuse" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var refs: [65]kernel.KernelObjectRef = undefined;
    for (&refs) |*ref| {
        ref.* = try s.createKernelObject(.irq, .{ .irq = .{ .owner_principal_raw = @intFromEnum(p0), .source = .{ .pci = 0x1001 }, .kind = .msix, .vector = 1 } });
        s.publishIrqObject(ref.*, s.kernelObjectSlot(ref.*).?.payload.irq);
    }
    var owners: [4]PrincipalId = undefined;
    try std.testing.expectEqual(@as(usize, 0), s.recordDeviceInterruptEvent(0x1002, 1, &owners));
    try std.testing.expectEqual(@as(usize, 0), s.recordDeviceInterruptEvent(0x1001, 2, &owners));
    try std.testing.expectEqual(@as(usize, 1), s.recordDeviceInterruptEvent(0x1001, 1, &owners));
    for (refs) |ref| {
        try std.testing.expectEqual(@as(?u64, 1), s.irqPublishedEventCount(ref));
        s.unpublishIrqObject(.{ .kind = .irq, .index = ref.index, .generation = ref.generation + 1 });
        try std.testing.expectEqual(@as(?u64, 1), s.irqPublishedEventCount(ref));
        s.unpublishIrqObject(ref);
    }
    try std.testing.expectEqual(@as(usize, 0), s.recordDeviceInterruptEvent(0x1001, 1, &owners));
}

test "physical page allocation returns a page handle" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x1_0000_0000, .end = (0x1_0000_0000) + (2) * 4096 }});
    try free_list.seedContiguousRange(0x1_0000_0000, 2);

    const page = try s.allocPhysicalPage(&free_list);
    try std.testing.expectEqual(@as(u64, 0x1_0000_0000), page.paddr);
}

fn expectFreeRangeInvariants(list: *const FreePageList) !void {
    var pages: usize = 0;
    for (0..list.rangeCount()) |i| {
        const range = list.rangeAt(i);
        try std.testing.expect(range.len != 0);
        try std.testing.expectEqual(@as(u64, 0), range.physical_start % 4096);
        pages += range.len;
        const end = range.physical_start + range.len * 4096;
        for (i + 1..list.rangeCount()) |j| {
            const other = list.rangeAt(j);
            try std.testing.expect(end <= other.physical_start or
                other.physical_start + other.len * 4096 <= range.physical_start);
        }
    }
    try std.testing.expectEqual(pages, list.pageCount());
}

test "free range removal preserves constrained allocation and surviving ranges" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    const boundary: u64 = 4 << 30;
    for (0..6) |operation| {
        for (0..3) |removed| {
            if (operation == 0 and removed != 0) continue;
            if (operation == 1 and removed != 2) continue;
            var list = FreePageList{};
            const above = operation == 3 or operation == 5;
            const pages: usize = if (operation >= 4) 2 else 1;
            const target: u64 = if (operation == 1) boundary + 0x200000 else if (above) boundary else boundary - pages * 4096;
            var boundaries: [3]std.meta.Child(@typeInfo(@TypeOf(FreePageList.installRegions)).@"fn".params[1].type.?) = undefined;
            for (&boundaries, 0..) |*region, i| {
                const start = if (i == removed) target else (if (above) @as(u64, 0x100000) else boundary + 0x100000) + i * 0x10000;
                region.* = .{ .id = i, .start = start, .end = start + (if (i == removed) pages else @as(usize, 3)) * 4096 };
            }
            std.mem.sort(@TypeOf(boundaries[0]), &boundaries, {}, struct {
                fn less(_: void, a: @TypeOf(boundaries[0]), b: @TypeOf(boundaries[0])) bool {
                    return a.start < b.start;
                }
            }.less);
            try list.installRegions(&boundaries);
            for (0..3) |i| {
                const start = if (i == removed) target else (if (above) @as(u64, 0x100000) else boundary + 0x100000) + i * 0x10000;
                try list.seedContiguousRange(start, if (i == removed) pages else 3);
            }
            const original = [_]kernel.RegionFreeRange{ list.rangeAt(0), list.rangeAt(1), list.rangeAt(2) };
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
            for (original) |range| {
                if (range.region_id == removed) continue;
                var found: usize = 0;
                for (0..list.rangeCount()) |range_index| {
                    const survivor = list.rangeAt(range_index);
                    if (survivor.region_id == range.region_id) {
                        try expectResourceEqualDeep(range, survivor);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
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
            try list.installRegions(&.{.{ .id = 7, .start = base, .end = 0x301000 }});
            for (order) |index| try list.seedContiguousRange(starts[index], 1);
            if (gap_pages == 1) {
                try list.seedContiguousRange(base + 4096, 1);
            } else {
                try list.seedContiguousRange(base + 4096, gap_pages);
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
    try separate.installRegions(&.{ .{ .id = 1, .start = 0x100000, .end = 0x102000 }, .{ .id = 2, .start = 0x102000, .end = 0x103000 } });
    try separate.seedContiguousRange(0x100000, 1);
    try separate.seedContiguousRange(0x102000, 1);
    try separate.seedContiguousRange(0x101000, 1);
    try std.testing.expectEqual(@as(usize, 2), separate.rangeCount());
    try std.testing.expectError(KernelError.OutOfFreePages, separate.popContiguousBelow(3, 0x200000));
    try expectFreeRangeInvariants(&separate);
}

test "free range fragmented drain returns every page once after repeated removals" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var list = FreePageList{};
    var seen = [_]bool{false} ** 257;
    const base: u64 = 0x100000;
    try list.installRegions(&.{.{ .id = 0, .start = base, .end = base + seen.len * 8192 }});
    for (0..seen.len) |i| try list.seedContiguousRange(base + i * 8192, 1);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]bool{ false, true }) |contiguous| {
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const boundary: u64 = 4 << 30;
        var backing = try kernel.PhysicalAccessTest.BackingSet.init(&.{.{ .first = boundary - 4096, .pages = 1 }});
        defer backing.deinit();
        try free_list.seedContiguousRange(boundary - 4096, 4);
        if (contiguous) {
            try std.testing.expectError(KernelError.InvalidState, free_list.popContiguousAtOrAbove(2, boundary));
        } else {
            try std.testing.expectError(KernelError.InvalidState, free_list.popFrontAtOrAbove(boundary));
        }
        try std.testing.expectEqual(@as(usize, 1), free_list.rangeCount());
        try std.testing.expectEqual(@as(usize, 4), free_list.pageCount());
        try std.testing.expectEqual(boundary - 4096, free_list.rangeAt(0).physical_start);
        try std.testing.expectEqual(@as(usize, 4), free_list.rangeAt(0).len);
        // Consuming the whole tail needs no new descriptor at an inaccessible PA.
        const tail = try free_list.popContiguousAtOrAbove(3, boundary);
        try std.testing.expectEqual(boundary, tail);
        try std.testing.expectEqual(@as(usize, 1), free_list.rangeAt(0).len);
        try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
    }
}

test "bounded contiguous allocation clips both ends and preserves excluded pages" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var list = FreePageList{};
    const boundary: u64 = 1 << 32;
    try list.installRegions(&.{.{ .id = 0, .start = boundary - 8192, .end = (boundary - 8192) + (8) * 4096 }});
    try list.seedContiguousRange(boundary - 8192, 8);
    try std.testing.expectEqual(boundary + 4096, try list.popContiguousInRange(2, boundary + 1, boundary + 3 * 4096));
    try std.testing.expectEqual(@as(usize, 6), list.pageCount());
    try std.testing.expectError(KernelError.OutOfFreePages, list.popContiguousInRange(1, boundary + 1, boundary + 4095));
    try std.testing.expectError(KernelError.OutOfFreePages, list.popContiguousInRange(1, std.math.maxInt(u64) - 1, std.math.maxInt(u64)));
    try std.testing.expectError(KernelError.OutOfFreePages, list.popContiguousInRange(1, boundary, boundary));
    try std.testing.expectEqual(@as(usize, 6), list.pageCount());
    try std.testing.expectEqual(boundary - 8192, try list.popContiguousBelow(3, boundary + 4096));
    try std.testing.expectEqual(boundary + 3 * 4096, try list.popContiguousInRange(3, boundary, boundary + 6 * 4096));
    try std.testing.expectEqual(@as(usize, 0), list.pageCount());
}

test "large bootfs above 16 GiB preserves DMA32 RAM and respects direct aperture" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var list = FreePageList{};
    const low: u64 = 64 << 20;
    const high: u64 = 20 << 30;
    const identity_end: u64 = 254 * (@as(u64, 1) << 39);
    const staging_pages = 107 * 256;
    // Keep an ineligible range first: free ranges are intentionally unordered.
    try list.installRegions(&.{ .{ .id = 1, .start = low, .end = low + 768 * 4096 }, .{ .id = 2, .start = high, .end = high + staging_pages * 4096 }, .{ .id = 0, .start = identity_end, .end = identity_end + 100 * 4096 } });
    try list.seedContiguousRange(identity_end, 100);
    try list.seedContiguousRange(low, 768);
    try list.seedContiguousRange(high, staging_pages);
    const records = try std.testing.allocator.alloc(kernel.boot_resource_pages.StagedPage, staging_pages);
    defer std.testing.allocator.free(records);
    for (records) |*record| record.* = .{};
    for (records) |*record| {
        try record.acquire(&list);
        var loan: kernel.boot_resource_pages.Loan = .{};
        try record.borrowInto(&loan);
        const physical = (try loan.physical()).toInt();
        try std.testing.expect(physical >= high and physical < identity_end);
        try loan.end();
    }
    try std.testing.expectEqual(@as(usize, 768), list.availableBelow(high).total);
    try std.testing.expectEqual(low, try list.popContiguousBelow(512, high));
    // A small-memory machine retains the existing low-RAM fallback.
    var fallback: kernel.boot_resource_pages.StagedPage = .{};
    try fallback.acquire(&list);
    var fallback_loan: kernel.boot_resource_pages.Loan = .{};
    try fallback.borrowInto(&fallback_loan);
    try std.testing.expectEqual(low + 512 * 4096, (try fallback_loan.physical()).toInt());
    try fallback_loan.end();
    _ = try list.popContiguousBelow(255, high);
    var exhausted: kernel.boot_resource_pages.StagedPage = .{};
    try std.testing.expectError(KernelError.OutOfFreePages, exhausted.acquire(&list));
    try std.testing.expectEqual(@as(usize, 100), list.pageCount());
    // These real ownership records outlive their grants, unlike the old test
    // loop's raw paddr values. Return each original region before metadata.
    for (records) |*record| try record.release();
    try fallback.release();
}

test "zero-on-demand VMO holes read as zero without physical allocation" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 20 << 30, .end = (20 << 30) + (8) * 4096 }});
    try free_list.seedContiguousRange(20 << 30, 8);
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
    try std.testing.expectEqual(page, s.commitNativeVmaFaultMapping(p0, a, page, null).?.paddr);
    try std.testing.expectEqual(page, s.commitNativeVmaFaultMapping(p1, b, spare, null).?.paddr);
    try free_list.appendPage(spare);
    const old = s.vmaEntryForVaConst(p0, va).?.*;
    try s.growVmoFdWithPages(p0, fd, 16384, &free_list);
    try std.testing.expectEqual(@as(usize, 7), free_list.pageCount());
    try expectResourceEqualDeep(old, s.vmaEntryForVaConst(p0, va).?.*);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 20 << 30, .end = (20 << 30) + (16) * 4096 }});
    try free_list.seedContiguousRange(20 << 30, 16);
    const fd = try s.createZeroOnDemandVmoFd(p0, 8192, fdRights(.{ .close = true, .map_read = true, .map_write = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const va: u64 = 0x4900_0000;
    _ = try s.mmapFd(p0, fd, va, 8192, .{ .read = true, .write = true }, .{ .private = true }, 0);
    _ = try s.mmapFd(p0, fd, va + 0x10000, 8192, .{ .read = true, .write = true }, .{ .shared = true }, 0);
    const stale = s.prepareNativeVmaCowMapping(p0, va, true, false);
    try std.testing.expectEqual(.allocate_zero, stale.kind);
    const shared_plan = s.prepareNativeVmaFaultMapping(p0, va + 0x10000, true, false);
    const shared_page = (try s.allocPhysicalPage(&free_list)).paddr;
    _ = s.commitNativeVmaFaultMapping(p0, shared_plan, shared_page, null).?;
    const candidate = (try s.allocPhysicalPage(&free_list)).paddr;
    try std.testing.expect(s.commitNativeVmaCowMapping(p0, stale, candidate, &free_list) == null);
    try free_list.appendPage(candidate);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const fd = try s.createZeroOnDemandVmoFd(p0, 4096, fdRights(.{ .close = true, .map_read = true, .map_write = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const va: u64 = 0x4900_0000;
    _ = try s.mmapFd(p0, fd, va, 4096, .{ .read = true, .write = true }, .{ .shared = true }, 0);
    const plan = s.prepareNativeVmaFaultMapping(p0, va, true, false);
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 1) == null);
    const slot = s.nativeVmoSlot(ref).?;
    const saved = slot.*;
    slot.page_store_start.owner +%= 1;
    try std.testing.expectEqual(.denied, s.prepareNativeVmaFaultMapping(p0, va, true, false).kind);
    try std.testing.expect(s.commitNativeVmaFaultMapping(p0, plan, 0x90000000, null) == null);
    slot.* = saved;
    slot.backing.parent.ref = .{ .index = ref.index, .generation = ref.generation + 1 };
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const base: u64 = 20 << 30;
    try free_list.installRegions(&.{.{ .id = 0, .start = base, .end = (base) + (8) * 4096 }});
    try free_list.seedContiguousRange(base, 8);
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
    try std.testing.expectEqual(first, s.commitNativeVmaFaultMapping(p0, a, first, null).?.paddr);
    try std.testing.expectEqual(first, s.commitNativeVmaFaultMapping(p1, b, spare, null).?.paddr);
    try free_list.appendPage(spare);
    const original = s.vmaEntryForVaConst(p0, 0x4900_0000).?.*;
    try s.growVmoFdWithPages(p0, fd, 16384, &free_list);
    try expectResourceEqualDeep(original, s.vmaEntryForVaConst(p0, 0x4900_0000).?.*);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 20 << 30, .end = (20 << 30) + (16) * 4096 }});
    try free_list.seedContiguousRange(20 << 30, 16);
    const fd = try s.createZeroOnDemandVmoFd(p0, 8192, fdRights(.{ .close = true, .map_read = true, .map_write = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    _ = try s.mmapFd(p0, fd, 0x4900_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .private = true }), 0);
    _ = try s.mmapFd(p0, fd, 0x4a00_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0);
    const plan = s.prepareNativeVmaCowMapping(p0, 0x4900_0000, true, false);
    try std.testing.expectEqual(.allocate_zero, plan.kind);
    const private_page = (try s.allocPhysicalPage(&free_list)).paddr;
    const shared_page = (try s.allocPhysicalPage(&free_list)).paddr;
    const shared = s.prepareNativeVmaFaultMapping(p0, 0x4a00_0000, true, false);
    _ = s.commitNativeVmaFaultMapping(p0, shared, shared_page, null).?;
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const fd = try s.createZeroOnDemandVmoFd(p0, 8192, fdRights(.{ .close = true, .resize = true, .map_read = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const slot = s.nativeVmoSlot(ref).?;
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 2) == null);
    var stale = ref;
    stale.generation += 1;
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(stale, 0) == null);
    slot.backing.parent.ref = stale;
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 0) == null);
    slot.backing.parent.ref = .{};
    slot.page_store_start.owner ^= 1;
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 0) == null);
    const before = slot.*;
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, fd, 16384, &free_list));
    try expectVmoSlotEqual(before, slot.*);
    slot.page_store_start.owner ^= 1;
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const base: u64 = 20 << 30;
    try free_list.installRegions(&.{.{ .id = 0, .start = base, .end = (base) + (8) * 4096 }});
    try free_list.seedContiguousRange(base, 8);
    const rights = fdRights(.{ .resize = true, .inspect = true, .close = true, .dup = true, .map_read = true, .map_write = true });
    const fd = try s.createAnonymousVmoFdWithPages(p0, 8192, rights, .{}, 16, &free_list);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    _ = try s.mmapFd(p0, fd, 0x4900_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0);
    try s.cloneFdTableForFork(p0, p1);
    try s.cloneVmaTableForFork(p0, p1);
    const view = try s.createNativePageView(ref, 1);
    try s.installNativeVmoPages(view, 0, &.{base + 4096}, null);
    const refs_before = s.nativeVmoRefCount(ref);
    const original_entry = s.vmaEntryForVaConst(p0, 0x4900_0000).?.*;
    try s.growVmoFdWithPages(p0, fd, 16384, &free_list);
    try std.testing.expectEqual(refs_before, s.nativeVmoRefCount(ref));
    try expectResourceEqualDeep(original_entry, s.vmaEntryForVaConst(p0, 0x4900_0000).?.*);
    try expectResourceEqualDeep(original_entry, s.vmaEntryForVaConst(p1, 0x4900_0000).?.*);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x8200_0000, .end = (0x8200_0000) + (4) * 4096 }});
    try free_list.seedContiguousRange(0x8200_0000, 4);
    const rights = fdRights(.{ .resize = true, .close = true, .dup = true, .map_read = true });
    const fd = try s.createAnonymousVmoFdWithPages(p0, 8192, rights, .{}, 16, &free_list);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const original = s.nativeVmoSlot(ref).?.*;
    for ([_]u64{ 0, 4096, 8191, 8193, std.math.maxInt(u64), (@as(u64, kernel.max_vmo_logical_pages) + 1) * 4096 }) |capacity| {
        try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, fd, capacity, &free_list));
        try expectVmoSlotEqual(original, s.nativeVmoSlot(ref).?.*);
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
    s.nativeVmoSlot(ref).?.backing.parent.ref = view;
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, fd, 16384, &free_list));
    s.nativeVmoSlot(ref).?.backing.parent.ref = .{};
    try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, fd, 16384, &free_list));
}

test "zero-on-demand faults preserve backing through physical exhaustion" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    {
        var s: KernelState = undefined;
        try initFdState(&s);
        defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
        var free_list = FreePageList{};
        const physical_count: usize = 1;
        try free_list.installRegions(&.{.{ .id = 0, .start = 254 * (@as(u64, 1) << 39), .end = (254 * (@as(u64, 1) << 39)) + (physical_count) * 4096 }});
        try free_list.seedContiguousRange(254 * (@as(u64, 1) << 39), physical_count);
        const fd = try s.createZeroOnDemandVmoFd(p0, 8192, fdRights(.{ .close = true, .map_read = true, .map_write = true }), .{}, 16);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        _ = try s.mmapFd(p0, fd, 0x4900_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0);
        _ = try s.mmapFd(p0, fd, 0x4a00_0000, 8192, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .private = true }), 0);
        const first = (try s.allocPhysicalPage(&free_list)).paddr;
        try s.installNativeVmoPages(ref, 0, &.{first}, null);
        const before = s.nativeVmoSlot(ref).?.*;
        const free_before = free_list.pageCount();
        const used_before = kernel.vmObjectBackingStoreStats().used_entries;
        const shared = s.prepareNativeVmaFaultMapping(p0, 0x4900_1000, true, false);
        const private = s.prepareNativeVmaCowMapping(p0, 0x4a00_1000, true, false);
        try std.testing.expectEqual(.allocate_zero, shared.kind);
        try std.testing.expectEqual(.allocate_zero, private.kind);
        try std.testing.expectError(KernelError.OutOfFreePages, s.allocPhysicalPage(&free_list));
        try std.testing.expect(s.ensureNativeVmaCowMappingLockedSlow(p0, 0x4a00_1000, true, false, &free_list) == null);
        try expectVmoSlotEqual(before, s.nativeVmoSlot(ref).?.*);
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

test "VMO grow rolls back physical exhaustion" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    {
        var s: KernelState = undefined;
        try initFdState(&s);
        defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
        var free_list = FreePageList{};
        const physical_base: u64 = 0x9000_0000;
        try free_list.installRegions(&.{.{ .id = 0, .start = physical_base, .end = (physical_base) + @as(u64, 2) * 4096 }});
        try free_list.seedContiguousRange(physical_base, 2);
        const fd = try s.createAnonymousVmoFdWithPages(p0, 4096, fdRights(.{ .resize = true, .close = true }), .{}, 16, &free_list);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        const old = s.nativeVmoSlot(ref).?.*;
        const free_before = free_list.pageCount();
        const used_before = kernel.vmObjectBackingStoreStats().used_entries;
        try std.testing.expectError(KernelError.OutOfFreePages, s.growVmoFdWithPages(p0, fd, 3 * 4096, &free_list));
        try expectVmoSlotEqual(old, s.nativeVmoSlot(ref).?.*);
        try std.testing.expectEqual(free_before, free_list.pageCount());
        try std.testing.expectEqual(used_before, kernel.vmObjectBackingStoreStats().used_entries);
        try std.testing.expectEqual(@as(?u64, 0), kernel.vmoBackingPageStorePaddr(old.page_store_start, 3, 1));
        try s.growVmoFdWithPages(p0, fd, 8192, &free_list);
        try std.testing.expectEqual(@as(?u64, physical_base), s.nativeVmoPagePaddr(ref, 0));
        try s.closeFdWithFreeList(p0, fd, &free_list);
        try std.testing.expectEqual(free_before + 1, free_list.pageCount());
    }
}

test "ordinary VMO pages use high RAM while contiguous DMA keeps low RAM" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const high: u64 = (16 << 30) + 4096;
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x200000, .end = high + 8192 }});
    try free_list.seedContiguousRange(0x200000, 2);
    try free_list.seedContiguousRange(high, 2);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const high: u64 = 20 << 30;
    try free_list.installRegions(&.{.{ .id = 0, .start = high, .end = (high) + (1) * 4096 }});
    try free_list.seedContiguousRange(high, 1);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x20_0000, .end = (0x20_0000) + (2) * 4096 }});
    try free_list.seedContiguousRange(0x20_0000, 2);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
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
        try kernel.setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page, slot.page_store_owner, (i + 1) * 4096, null);
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
    for ([_]kernel.MmapFlags{ mmapFlags(.{ .anonymous = true, .private = true }), mmapFlags(.{ .anonymous = true, .shared = true, .noreserve = true }) }) |map_flags| {
        _ = try s.createAnonymousVmaWithPages(p0, base, size, prot, prot, map_flags, &free_list);
        s.releasePrincipalNativeMemory(p0, &free_list);
    }
    try std.testing.expectError(KernelError.OutOfFreePages, s.createAnonymousVmoFdWithPages(p0, 131073 * 4096, fdRights(.{ .map_read = true }), .{}, 0, &free_list));
}

test "large backing snapshots survive empty branch removal between batches" {
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));
    var store = kernel.BackingStore{};
    const owner = kernel.vmoBackingStoreOwner(.native_vmo, 1, 9);
    const count: u32 = 128 << 18;
    const ref = kernel.allocEmptyVmoBackingPageStore(&store, count, owner).?;
    const indices = [_]u32{ 0, 131072, count - 1 };
    for (indices, 0..) |index, i| try kernel.setVmoBackingPageStorePaddr(ref, count, index, owner, (i + 1) * 4096, null);
    var batch: [1]kernel.BackedPage = undefined;
    try std.testing.expectEqual(@as(usize, 1), kernel.snapshotVmoBackingPages(ref, count, owner, 0, count, &batch));
    try kernel.setVmoBackingPageStorePaddr(ref, count, 0, owner, 0, null);
    try std.testing.expectEqual(@as(usize, 1), kernel.snapshotVmoBackingPages(ref, count, owner, 1, count, &batch));
    try std.testing.expectEqual(indices[1], batch[0].index);
    try std.testing.expect(kernel.freeVmoBackingPageStore(ref, count, owner));
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
}

test "large sparse release rejects a mismatched owner before returning RAM" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const vmo = try s.createNativeVmo(.anonymous, 128 << 30);
    try s.retainNativeVmo(vmo);
    const slot = s.nativeVmoSlot(vmo).?;
    const original = slot.page_store_owner;
    try kernel.setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, 0, original, 0x1000, null);
    slot.page_store_owner = kernel.vmoBackingStoreOwner(.native_vmo, vmo.index, vmo.generation + 1);
    KernelState.releaseNativeVmoOwnedPages(slot, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    try std.testing.expectEqual(@as(?u64, 0x1000), s.nativeVmoPagePaddr(vmo, 0));
    slot.page_store_owner = original;
    s.releaseNativeVmoWithFreeList(vmo, &free_list);
    try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
}

test "VMO final drop preserves data ownership when PMM return is refused" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    // Synthetic PFNs are never dereferenced or merged into a real allocator.
    // Restore the global backing fixture even when the regression fails.
    defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
    var full = FreePageList{};
    try full.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const physical: u64 = 0x1000;
    var hold = try DmaHold.init(physical, 1);
    defer hold.end();
    try std.testing.expectError(KernelError.PageQuarantined, full.appendPage(physical));
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    const ref = try s.createNativeVmo(.anonymous, 4096);
    try s.retainNativeVmo(ref);
    const slot = s.nativeVmoSlot(ref).?;
    const owner = slot.page_store_owner;
    const start = slot.page_store_start;
    try kernel.setVmoBackingPageStorePaddr(start, 1, 0, owner, physical, null);
    try std.testing.expectEqual(@as(u32, 1), slot.ref_count);
    try std.testing.expectEqual(@as(u64, 1), kernel.vmObjectBackingStoreStats().used_entries);
    s.releaseNativeVmoWithFreeList(ref, &full);
    try std.testing.expectEqual(@as(usize, 0), full.pageCount());
    var remaining: [1]kernel.BackedPage = undefined;
    try std.testing.expectEqual(@as(usize, 1), kernel.snapshotVmoBackingPages(start, 1, owner, 0, 1, &remaining));
    try std.testing.expectEqual(@as(u32, 0), remaining[0].index);
    try std.testing.expectEqual(physical, remaining[0].paddr);
    try std.testing.expectEqual(@as(u64, 1), kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expect(s.hasPendingDataReturns());
    try std.testing.expect(s.nativeVmoSlot(ref) == null);
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreBreakdown(&s.native_vmos, &s.vmo_directory_leaf).invalid_native_entries);
    try std.testing.expectError(KernelError.InvalidState, s.retainNativeVmo(ref));
    try std.testing.expectError(KernelError.InvalidState, s.installNativeVmoPages(ref, 0, &.{0x5000}, null));
    var wrong_destination = FreePageList{};
    try wrong_destination.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    s.releaseNativeVmoWithFreeList(ref, &wrong_destination);
    s.releaseNativeVmo(ref);
    for (0..3) |_| s.drainPendingDataReturns();
    try std.testing.expectEqual(@as(usize, 1), s.pending_data_returns);
    try std.testing.expectEqual(@as(usize, 0), wrong_destination.pageCount());
    s.next_native_vmo_scan = ref.index;
    const other = try s.createNativeVmo(.anonymous, 4096);
    try std.testing.expect(other.index != ref.index);
    hold.end();
    s.drainPendingDataReturns();
    try std.testing.expectEqual(@as(usize, 1), full.pageCount());
    try std.testing.expect(!s.hasPendingDataReturns());
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    s.drainPendingDataReturns();
    try std.testing.expectEqual(@as(usize, 1), full.pageCount());
    s.next_native_vmo_scan = ref.index;
    const reused = try s.createNativeVmo(.anonymous, 4096);
    try std.testing.expectEqual(ref.index, reused.index);
    try std.testing.expect(reused.generation != ref.generation);
    try kernel.ReferenceCheck.check(&s);
}

test "COW final drop preserves data ownership when PMM return is refused" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
    var full = FreePageList{};
    try full.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const physical: u64 = 0x3000;
    var hold = try DmaHold.init(physical, 1);
    defer hold.end();
    try std.testing.expectError(KernelError.PageQuarantined, full.appendPage(physical));
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    const ref = try s.createNativeCowTable(1, &full);
    try s.retainNativeCowTable(ref);
    const slot = s.nativeCowTableSlot(ref).?;
    const owner = kernel.vmoBackingStoreOwner(.native_cow, ref.index, ref.generation);
    const start = slot.page_store_start;
    try s.setNativeCowPagePaddr(ref, 0, physical, null);
    try std.testing.expectEqual(@as(u32, 1), slot.ref_count);
    try std.testing.expectEqual(@as(u64, 1), kernel.vmObjectBackingStoreStats().used_entries);
    s.releaseNativeCowTable(ref, &full);
    try std.testing.expectEqual(@as(usize, 0), full.pageCount());
    var remaining: [1]kernel.BackedPage = undefined;
    try std.testing.expectEqual(@as(usize, 1), kernel.snapshotVmoBackingPages(start, 1, owner, 0, 1, &remaining));
    try std.testing.expectEqual(@as(u32, 0), remaining[0].index);
    try std.testing.expectEqual(physical, remaining[0].paddr);
    try std.testing.expectEqual(@as(u64, 1), kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expect(s.hasPendingDataReturns());
    try std.testing.expect(s.nativeCowTableSlot(ref) == null);
    try std.testing.expectError(KernelError.InvalidState, s.retainNativeCowTable(ref));
    try std.testing.expectError(KernelError.InvalidState, s.setNativeCowPagePaddr(ref, 0, 0x5000, null));
    var wrong_destination = FreePageList{};
    try wrong_destination.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    s.releaseNativeCowTable(ref, &wrong_destination);
    s.releaseNativeCowTable(ref, null);
    for (0..3) |_| s.drainPendingDataReturns();
    try std.testing.expectEqual(@as(usize, 1), s.pending_data_returns);
    try std.testing.expectEqual(@as(usize, 0), wrong_destination.pageCount());
    const other = try s.createNativeCowTable(1, &full);
    try std.testing.expect(other.index != ref.index);
    hold.end();
    s.drainPendingDataReturns();
    try std.testing.expectEqual(@as(usize, 1), full.pageCount());
    try std.testing.expect(!s.hasPendingDataReturns());
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    s.drainPendingDataReturns();
    try std.testing.expectEqual(@as(usize, 1), full.pageCount());
    const reused = try s.createNativeCowTable(1, &full);
    try std.testing.expectEqual(ref.index, reused.index);
    try std.testing.expect(reused.generation != ref.generation);
    try kernel.ReferenceCheck.check(&s);
}

test "pending child keeps its parent until recovery and releases that edge exactly once" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
    var full = FreePageList{};
    try full.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    var child_hold = try DmaHold.init(0x1000, 1);
    defer child_hold.end();
    var parent_hold = try DmaHold.init(0x7000, 1);
    defer parent_hold.end();
    const parent = try s.createNativeVmo(.anonymous, 4096);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{0x7000}, null);
    const child = try s.createNativeVmo(.anonymous, 4096);
    try s.retainNativeVmo(child);
    try s.installNativeVmoPages(child, 0, &.{0x1000}, null);
    // Build the same retained parent edge as an anonymous shadow. The fixture
    // is unpublished and serial; no production borrower is bypassed here.
    try s.retainNativeVmo(parent);
    try s.publishNativeVmoParent(child, parent, 0);
    s.releaseNativeVmoWithFreeList(parent, &full);
    s.releaseNativeVmoWithFreeList(child, &full);
    for (0..3) |_| s.drainPendingDataReturns();
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(parent));
    try std.testing.expectEqual(@as(usize, 1), s.pending_data_returns);
    try std.testing.expectEqual(@as(u64, 2), kernel.vmObjectBackingStoreStats().used_entries);

    // The child is drained first; the parent still has DMA reachability.
    child_hold.end();
    s.drainPendingDataReturns();
    try std.testing.expect(s.native_vmos.get(child.index) == null);
    try std.testing.expect(s.native_vmos.generation(child.index).? != child.generation);
    try std.testing.expect(s.nativeVmoSlot(parent) == null);
    try std.testing.expectEqual(@as(u32, 0), s.native_vmos.get(@intCast(parent.index)).?.*.ref_count);
    try std.testing.expectEqual(@as(usize, 1), s.pending_data_returns);
    try std.testing.expectEqual(@as(u64, 1), kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expectEqual(@as(usize, 1), full.pageCount());
    s.releaseNativeVmo(child);
    s.releaseNativeVmoWithFreeList(parent, &full);
    for (0..3) |_| s.drainPendingDataReturns();
    try std.testing.expectEqual(@as(usize, 1), full.pageCount());

    // Drain the parent while preserving the already returned child in PMM.
    parent_hold.end();
    const before = full.pageCount();
    s.drainPendingDataReturns();
    try std.testing.expectEqual(before + 1, full.pageCount());
    try std.testing.expectEqual(@as(usize, 0), s.pending_data_returns);
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    s.drainPendingDataReturns();
    s.releaseNativeVmoWithFreeList(child, &full);
    s.releaseNativeVmoWithFreeList(parent, &full);
    try std.testing.expectEqual(before + 1, full.pageCount());
    try kernel.ReferenceCheck.check(&s);
}

test "disjoint VMO and COW finalizers count both pending destinations and retry only remaining pages" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
    var vmo_destination = FreePageList{};
    try vmo_destination.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    var cow_destination = FreePageList{};
    try cow_destination.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    var vmo_hold = try DmaHold.init(0x7000, 1);
    defer vmo_hold.end();
    var cow_hold = try DmaHold.init(0xb000, 1);
    defer cow_hold.end();
    const vmo = try s.createNativeVmo(.anonymous, 8192);
    try s.retainNativeVmo(vmo);
    try s.installNativeVmoPages(vmo, 0, &.{ 0x1000, 0x7000 }, null);
    const vmo_slot = s.nativeVmoSlot(vmo).?;
    const owner = vmo_slot.page_store_owner;
    const start = vmo_slot.page_store_start;
    const cow = try s.createNativeCowTable(1, &cow_destination);
    try s.retainNativeCowTable(cow);
    try s.setNativeCowPagePaddr(cow, 0, 0xb000, null);
    var start_gate: u32 = 0;
    const Finalizer = struct {
        state: *KernelState,
        destination: *FreePageList,
        gate: *u32,
        vmo: kernel.NativeVmoRef = .{},
        cow: kernel.NativeCowTableRef = .{},

        fn run(ctx: *const @This()) void {
            while (@atomicLoad(u32, ctx.gate, .acquire) == 0) std.atomic.spinLoopHint();
            if (!ctx.vmo.isNull())
                ctx.state.releaseNativeVmoWithFreeList(ctx.vmo, ctx.destination)
            else
                ctx.state.releaseNativeCowTable(ctx.cow, ctx.destination);
        }
    };
    // Each worker owns a separate slot family and destination. This checks
    // the cross-domain atomic hint, not the production VM-lock protocol.
    const vmo_ctx = Finalizer{ .state = &s, .destination = &vmo_destination, .gate = &start_gate, .vmo = vmo };
    const cow_ctx = Finalizer{ .state = &s, .destination = &cow_destination, .gate = &start_gate, .cow = cow };
    const vmo_worker = try std.Thread.spawn(.{}, Finalizer.run, .{&vmo_ctx});
    const cow_worker = std.Thread.spawn(.{}, Finalizer.run, .{&cow_ctx}) catch |err| {
        @atomicStore(u32, &start_gate, 1, .release);
        vmo_worker.join();
        return err;
    };
    @atomicStore(u32, &start_gate, 1, .release);
    vmo_worker.join();
    cow_worker.join();
    try std.testing.expectEqual(@as(usize, 2), s.pending_data_returns);
    try std.testing.expectEqual(@as(usize, 1), vmo_destination.pageCount());
    try std.testing.expectEqual(@as(usize, 0), cow_destination.pageCount());
    var remaining: [2]kernel.BackedPage = undefined;
    try std.testing.expectEqual(@as(usize, 1), kernel.snapshotVmoBackingPages(start, 2, owner, 0, 2, &remaining));
    try std.testing.expectEqual(@as(u32, 1), remaining[0].index);
    try std.testing.expectEqual(@as(u64, 0x7000), remaining[0].paddr);

    vmo_hold.end();
    const before = vmo_destination.pageCount();
    s.drainPendingDataReturns();
    try std.testing.expectEqual(before + 1, vmo_destination.pageCount());
    try std.testing.expectEqual(@as(usize, 1), s.pending_data_returns);
    try std.testing.expectEqual(@as(usize, 0), cow_destination.pageCount());
    try std.testing.expectEqual(@as(u64, 1), kernel.vmObjectBackingStoreStats().used_entries);
    cow_hold.end();
    s.drainPendingDataReturns();
    try std.testing.expect(!s.hasPendingDataReturns());
    try std.testing.expectEqual(@as(usize, 1), cow_destination.pageCount());
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    s.drainPendingDataReturns();
    try std.testing.expectEqual(before + 1, vmo_destination.pageCount());
    try std.testing.expectEqual(@as(usize, 1), cow_destination.pageCount());
}

test "COW storage grows repeatedly reclaims bodies and preserves stale generations" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.native_cow_tables.deinit();
    defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
    var free_list = FreePageList{};
    var refs: [513]kernel.NativeCowTableRef = undefined;
    for (0..3) |_| {
        for (&refs, 0..) |*ref, index| {
            ref.* = try s.createNativeCowTable(1, &free_list);
            try s.retainNativeCowTable(ref.*);
            try s.setNativeCowPagePaddr(ref.*, 0, (index + 1) * 4096, null);
        }
        for (refs, 0..) |ref, index| {
            try std.testing.expectEqual(@as(?u64, (index + 1) * 4096), s.nativeCowPagePaddr(ref, 0));
            s.releaseNativeCowTable(ref, null);
            try std.testing.expect(s.nativeCowTableSlot(ref) == null);
        }
        try std.testing.expectEqual(@as(u64, 0), s.native_cow_tables.live);
        try std.testing.expect(s.native_cow_tables.body_owner.published == null);
        try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    }
}

test "final VMO drop rejects invalid and foreign owner tokens before returning any page" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const victim = try s.createNativeVmo(.anonymous, 4096);
    try s.retainNativeVmo(victim);
    try s.installNativeVmoPages(victim, 0, &.{0x9000}, null);
    const foreign = s.nativeVmoSlot(victim).?.page_store_owner;
    for (0..3) |mode| {
        const ref = try s.createNativeVmo(.anonymous, 4096);
        try s.retainNativeVmo(ref);
        try s.installNativeVmoPages(ref, 0, &.{(mode + 1) * 4096}, null);
        const slot = s.nativeVmoSlot(ref).?;
        const start = slot.page_store_start;
        const owner = slot.page_store_owner;
        if (mode != 0) slot.page_store_start.owner = foreign;
        if (mode != 1) slot.page_store_owner = foreign;
        const before = free_list.pageCount();
        const faults = kernel.vmoBackingStoreOwnershipFault().count;
        s.releaseNativeVmoWithFreeList(ref, &free_list);
        for (0..3) |_| s.drainPendingDataReturns();
        try std.testing.expectEqual(before, free_list.pageCount());
        try std.testing.expect(kernel.vmoBackingStoreOwnershipFault().count > faults);
        try std.testing.expectEqual(@as(usize, 1), s.pending_data_returns);
        try std.testing.expectEqual(ref.generation, slot.generation);
        try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(victim));
        try std.testing.expectEqual(@as(?u64, 0x9000), s.nativeVmoPagePaddr(victim, 0));
        try std.testing.expectEqual(@as(u64, 2), kernel.vmObjectBackingStoreStats().used_entries);
        // Fault injection repair is test-only. Production leaves the failed
        // owner visible for diagnosis rather than guessing another token.
        slot.page_store_start = start;
        slot.page_store_owner = owner;
        s.drainPendingDataReturns();
        try std.testing.expectEqual(before + 1, free_list.pageCount());
        try std.testing.expect(!s.hasPendingDataReturns());
        try std.testing.expectEqual(@as(u64, 1), kernel.vmObjectBackingStoreStats().used_entries);
    }
    s.releaseNativeVmoWithFreeList(victim, &free_list);
    try std.testing.expectEqual(@as(usize, 4), free_list.pageCount());
}

test "final COW drop retains an invalid owner token until metadata is repaired" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const ref = try s.createNativeCowTable(1, &free_list);
    try s.retainNativeCowTable(ref);
    try s.setNativeCowPagePaddr(ref, 0, 0x1000, null);
    const slot = s.nativeCowTableSlot(ref).?;
    const start = slot.page_store_start;
    slot.page_store_start = .{};
    const faults = kernel.vmoBackingStoreOwnershipFault().count;
    s.releaseNativeCowTable(ref, &free_list);
    for (0..3) |_| s.drainPendingDataReturns();
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    try std.testing.expect(kernel.vmoBackingStoreOwnershipFault().count > faults);
    try std.testing.expectEqual(@as(usize, 1), s.pending_data_returns);
    try std.testing.expectEqual(@as(u64, 1), kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expectEqual(ref.generation, slot.generation);
    slot.page_store_start = start;
    s.drainPendingDataReturns();
    try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
    try std.testing.expect(!s.hasPendingDataReturns());
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
}

test "large sparse COW detach copies only backed pages of its logical slice" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const pages: u32 = 128 << 18;
    const original = try s.createNativeCowTable(pages, &free_list);
    try s.retainNativeCowTable(original);
    try s.retainNativeCowTable(original);
    try s.setNativeCowPagePaddr(original, 0, 0x1000, null);
    try s.setNativeCowPagePaddr(original, pages / 2, 0x2000, null);
    try s.setNativeCowPagePaddr(original, pages - 1, 0x3000, null);
    var slice = kernel.VmaEntry{
        .active = true,
        .start_va = 0x200_0000_0000,
        .size_bytes = @as(u64, pages / 2) * 4096,
        .cow_table = original,
        .cow_page_offset = pages / 2,
    };
    var copies = FreePageList{};
    try copies.installRegions(&.{.{ .id = 0, .start = 0x9000000, .end = (0x9000000) + (2) * 4096 }});
    try copies.seedContiguousRange(0x9000000, 2);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const max_bytes = @as(u64, 131072) * kernel.native_page_size;

    const large_vmo = try s.createNativeVmo(.anonymous, max_bytes);
    try s.retainNativeVmo(large_vmo);
    const large_cow = try s.createNativeCowTable(131072, &free_list);
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
        null,
    );
    try kernel.setVmoBackingPageStorePaddr(
        vmo_slot.page_store_start,
        vmo_slot.page_count,
        131072 - 1,
        vmo_slot.page_store_owner,
        0x5678_9000,
        null,
    );
    try s.setNativeCowPagePaddr(large_cow, 77, 0x9abc_d000, null);
    try std.testing.expectEqual(@as(?u64, 0x1234_5000), s.nativeVmoPagePaddr(large_vmo, 1));
    try std.testing.expectEqual(
        @as(?u64, 0x5678_9000),
        s.nativeVmoPagePaddr(large_vmo, 131072 - 1),
    );
    try std.testing.expectEqual(@as(?u64, 0x9abc_d000), s.nativeCowPagePaddr(large_cow, 77));
    stats = kernel.vmObjectBackingStoreStats();
    try std.testing.expectEqual(@as(u64, 3), stats.used_entries);
    try std.testing.expectEqual(stats.capacity - 3, stats.free_entries);

    const census = kernel.vmObjectBackingStoreBreakdown(&s.native_vmos, &s.vmo_directory_leaf);
    try std.testing.expectEqual(@as(u64, 2), census.native_entries);
    try std.testing.expectEqual(@as(u64, 1), census.cow_entries);
    try std.testing.expectEqual(@as(u64, 0), census.invalid_native_entries);
    try std.testing.expectEqual(@as(u64, 2), census.anonymous_entries);
    try std.testing.expectEqual(@as(u32, 2), census.largest[0].entries);
    try std.testing.expectEqual(vmo_slot.page_store_owner, census.largest[0].owner);
    const saved_refs = vmo_slot.ref_count;
    vmo_slot.ref_count = 0;
    try std.testing.expectEqual(@as(u64, 2), kernel.vmObjectBackingStoreBreakdown(&s.native_vmos, &s.vmo_directory_leaf).invalid_native_entries);
    vmo_slot.ref_count = saved_refs;
    try std.testing.expectEqual(@as(u64, 3), kernel.vmObjectBackingStoreStats().used_entries);

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

test "vmo backing store owner ledger rejects cross-object writes and frees" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));
    const owner = kernel.vmoBackingStoreOwner(.native_vmo, 3, 1);
    const intruder = kernel.vmoBackingStoreOwner(.native_cow, 4, 1);
    var store = kernel.BackingStore{};
    const start = kernel.allocEmptyVmoBackingPageStore(&store, 1, owner) orelse unreachable;

    try std.testing.expectError(
        KernelError.InvalidState,
        kernel.setVmoBackingPageStorePaddr(start, 1, 0, intruder, 0x1000, null),
    );
    try std.testing.expectEqual(@as(?u64, 0), kernel.vmoBackingPageStorePaddr(start, 1, 0));
    try kernel.setVmoBackingPageStorePaddr(start, 1, 0, owner, 0x1000, null);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    try std.testing.expect(kernel.initRuntimeStorage(runtime_storage[0..]));
    const current = kernel.vmoBackingStoreOwner(.native_vmo, 9, 0x8000_0001);
    const stale = kernel.vmoBackingStoreOwner(.native_vmo, 9, 1);
    const cow = kernel.vmoBackingStoreOwner(.native_cow, 9, 0x8000_0001);
    try std.testing.expect(current != stale);
    try std.testing.expect(current != cow);

    var store = kernel.BackingStore{};
    const start = kernel.allocEmptyVmoBackingPageStore(&store, 8, current) orelse unreachable;
    try kernel.setVmoBackingPageStorePaddr(start, 8, 3, current, 0x1111_1000, null);
    try std.testing.expectError(
        KernelError.InvalidState,
        kernel.setVmoBackingPageStorePaddr(start, 8, 3, stale, 0x2222_2000, null),
    );
    try std.testing.expectEqual(@as(?u64, 0x1111_1000), kernel.vmoBackingPageStorePaddr(start, 8, 3));

    var cow_store = kernel.BackingStore{};
    const cow_start = kernel.allocEmptyVmoBackingPageStore(&cow_store, 8, cow) orelse unreachable;
    try kernel.setVmoBackingPageStorePaddr(cow_start, 8, 3, cow, 0x3333_3000, null);
    try std.testing.expectEqual(@as(?u64, 0x3333_3000), kernel.vmoBackingPageStorePaddr(cow_start, 8, 3));
    try std.testing.expectEqual(@as(u64, 2), kernel.vmObjectBackingStoreStats().used_entries);
}

test "native vmo page install rejects a bad batch without a partial write" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const vmo = try s.createNativeVmo(.anonymous, 2 * kernel.native_page_size);
    try s.retainNativeVmo(vmo);
    try std.testing.expectError(
        KernelError.InvalidState,
        s.installNativeVmoPages(vmo, 0, &[_]u64{ 0x1000, 0x2001 }, null),
    );
    try std.testing.expectEqual(@as(?u64, null), s.nativeVmoPagePaddr(vmo, 0));
    try std.testing.expectEqual(@as(?u64, null), s.nativeVmoPagePaddr(vmo, 1));
}

test "RAM cache preflight resolves only populated canonical RAM owners" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const parent = try s.createNativeVmo(.anonymous, 3 * kernel.native_page_size);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{ 0x8400_0000, 0x8400_7000 }, null);
    var pages: [2]u64 = undefined;
    const owner = try s.nativeRamCachePages(parent, 0, &pages);
    try std.testing.expectEqual(kernel.vmoBackingStoreOwner(.native_vmo, parent.index, parent.generation), owner);
    try std.testing.expectEqualSlices(u64, &.{ 0x8400_0000, 0x8400_7000 }, &pages);
    try std.testing.expectError(KernelError.InvalidState, s.nativeRamCachePages(parent, 1, &pages));
    try std.testing.expectEqual(@as(?u64, null), s.nativeVmoPagePaddr(parent, 2));
    try std.testing.expectError(KernelError.InvalidState, s.nativeRamCachePages(parent, std.math.maxInt(usize), &pages));
    try std.testing.expectError(KernelError.InvalidState, s.nativeRamCachePages(parent, 0, pages[0..0]));
    var stale = parent;
    stale.generation += 1;
    try std.testing.expectError(KernelError.InvalidState, s.nativeRamCachePages(stale, 0, &pages));
    const view = try s.createNativePageView(parent, 1);
    try s.installNativeVmoPages(view, 0, &.{0x8400_0000}, null);
    try std.testing.expectError(KernelError.InvalidState, s.nativeRamCachePages(view, 0, pages[0..1]));
}

test "native VMO slot explicitly budgets its failed-return destination" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    // Ownership survives PMM rejection; do not overload the parent/device
    // union merely to preserve the old single-cache-line storage target.
    // The internal WC token now encodes every u32 slot plus reserved zero.
    const pager_metadata = @sizeOf(kernel.types.PagerPortRef) + 2 * @sizeOf(u64) + @sizeOf(kernel.PrincipalRaw) +
        @sizeOf(kernel.types.PagerReleaseRef) +
        @sizeOf(@TypeOf(@as(kernel.NativeVmoSlot, .{}).pager_request_store)) +
        @sizeOf(@TypeOf(@as(kernel.NativeVmoSlot, .{}).pager_request_start)) +
        2 * @sizeOf(kernel.types.PagerRequestRef);
    try std.testing.expect(@sizeOf(kernel.NativeVmoSlot) <= 64 + @sizeOf(*FreePageList) + 8 + @sizeOf(u64) + @sizeOf(kernel.BackingStore) + @sizeOf(*kernel.BackingStore) + @sizeOf(@TypeOf(@as(kernel.NativeVmoSlot, .{}).references)) + @sizeOf(@TypeOf(@as(kernel.NativeVmoSlot, .{}).parent_edge)) + pager_metadata + 8);
}

test "RAM lifetime ticket prevalidates canonical owner before infallible publication update" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const root = try s.createNativeVmo(.anonymous, 4096);
    try s.retainNativeVmo(root);
    try s.installNativeVmoPages(root, 0, &.{0x8400_0000}, null);
    const ticket = try s.prepareNativeRamCacheLifetime(root);
    var pages: [1]u64 = undefined;
    try std.testing.expectEqual(try s.nativeRamCachePages(root, 0, &pages), ticket.owner);
    const cached: kernel.RamCacheSnapshot = .{ .policy = &.{.{
        .base = 0x8400_0000,
        .bytes = 4096,
        .owner = ticket.owner,
        .mode = .wc,
        .pat_slot = 4,
    }} };
    ticket.finish(&cached);
    try std.testing.expect(s.nativeVmoSlotConst(root).?.ram_cache_state == .cached);
    const wb: kernel.RamCacheSnapshot = .{};
    ticket.finish(&wb);
    try std.testing.expect(s.nativeVmoSlotConst(root).?.ram_cache_state == .wb);
    const view = try s.createNativePageView(root, 1);
    try s.installNativeVmoPages(view, 0, &.{0x8400_0000}, null);
    try std.testing.expectError(KernelError.InvalidState, s.prepareNativeRamCacheLifetime(view));
    var stale = root;
    stale.generation += 1;
    try std.testing.expectError(KernelError.InvalidState, s.prepareNativeRamCacheLifetime(stale));
}

test "RAM cache mapping validation distinguishes target pages from unrelated code and DMA" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const root = try s.createNativeVmo(.anonymous, 4 * 4096);
    try s.retainNativeVmo(root);
    try s.installNativeVmoPages(root, 0, &.{ 0x8400_0000, 0x8400_1000, 0x8400_2000, 0x8400_3000 }, null);
    const rw = vmaProt(.{ .read = true, .write = true });
    const rx = vmaProt(.{ .read = true, .exec = true });
    const shared = mmapFlags(.{ .shared = true });
    try s.createVmaWithRetainedVmo(p0, 0x4100_0000, 4 * 4096, rw, rw, shared, root, 0);
    try s.createVmaWithRetainedVmo(p1, 0x4200_0000, 4096, rx, rx, shared, root, 3 * 4096);
    const dma_fd = try s.createDmaMappingFd(p0, .{
        .device = 1,
        .user_va = 0x4100_1000,
        .iova = 0x9000_0000,
        .size = 4096,
    }, fdRights(.{ .close = true }), .{}, 16);
    try s.validateRamCacheMappings(root, 0, &.{0x8400_0000});
    try s.validateRamCacheMappings(root, 1, &.{0x8400_1000});
    // A cache transition may proceed while the streaming mapping holds the
    // pages in place, but the unchanged VMA guard still rejects their reuse.
    try std.testing.expect(s.rangeOverlapsPinnedUserObject(p0, 0x4100_1000, 4096));
    try std.testing.expect(s.nativeVmoTreeMappingsOverlapPinnedUserObjects(root));
    const dma_buffer_fd = try s.createDmaBufferFd(p0, .{
        .device = 1,
        .user_va = 0x4100_1000,
        .iova = 0x9002_0000,
        .size = 4096,
    }, fdRights(.{ .close = true }), .{}, 16);
    try std.testing.expectError(error.DmaPinned, s.validateRamCacheMappings(root, 1, &.{0x8400_1000}));
    try std.testing.expect(s.nativeVmoSlotConst(root).?.ram_cache_state == .wb);
    try s.closeFd(p0, dma_buffer_fd);
    try s.validateRamCacheMappings(root, 1, &.{0x8400_1000});
    const mmio_fd = try s.createMmioRegionFd(p0, .{
        .device = 1,
        .bar_index = 0,
        .paddr = 0xa000_0000,
        .user_va = 0x4100_0000,
        .size = 4096,
    }, fdRights(.{ .close = true }), .{}, 16);
    try std.testing.expectError(error.DmaPinned, s.validateRamCacheMappings(root, 0, &.{0x8400_0000}));
    try s.closeFd(p0, mmio_fd);
    try s.validateRamCacheMappings(root, 0, &.{0x8400_0000});
    try std.testing.expectError(error.ExecutableAlias, s.validateRamCacheMappings(root, 3, &.{0x8400_3000}));
    const view = try s.createNativePageView(root, 1);
    try s.installNativeVmoPages(view, 0, &.{0x8400_2000}, null);
    try s.createVmaWithRetainedVmo(p2, 0x4300_0000, 4096, rw, rw, shared, view, 0);
    try s.validateRamCacheMappings(root, 2, &.{0x8400_2000});
    const view_dma_fd = try s.createDmaMappingFd(p2, .{
        .device = 1,
        .user_va = 0x4300_0080,
        .iova = 0x9001_0080,
        .size = 64,
    }, fdRights(.{ .close = true }), .{}, 16);
    try s.validateRamCacheMappings(root, 2, &.{0x8400_2000});
    try std.testing.expect(s.rangeOverlapsPinnedUserObject(p2, 0x4300_0000, 4096));
    try s.closeFd(p2, view_dma_fd);
    try std.testing.expect(!s.rangeOverlapsPinnedUserObject(p2, 0x4300_0000, 4096));
    try std.testing.expect(s.rangeOverlapsPinnedUserObject(p0, 0x4100_1000, 4096));
    try s.closeFd(p0, dma_fd);
    try std.testing.expect(!s.rangeOverlapsPinnedUserObject(p0, 0x4100_1000, 4096));
    try s.validateRamCacheMappings(root, 0, &.{0x8400_0000});
    try s.createVmaWithRetainedVmo(p1, 0x4400_0000, 4096, rw, rw, mmapFlags(.{ .private = true }), root, 0);
    try std.testing.expectError(error.PrivateAlias, s.validateRamCacheMappings(root, 0, &.{0x8400_0000}));
    try std.testing.expectError(error.Invalid, s.validateRamCacheMappings(root, 0, &.{0x8500_0000}));
    try std.testing.expectError(error.Invalid, s.validateRamCacheMappings(root, 4, &.{0x8400_0000}));
    try std.testing.expectError(error.Invalid, s.validateRamCacheMappings(view, 0, &.{0x8400_2000}));
    try std.testing.expect(s.nativeVmoSlotConst(root).?.ram_cache_state == .wb);
}

test "RAM cache mapping validation rejects inherited shadow overlap only" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const root = try s.createNativeVmo(.anonymous, 8192);
    try s.retainNativeVmo(root);
    try s.installNativeVmoPages(root, 0, &.{ 0x8400_0000, 0x8400_1000 }, null);
    const shadow = try s.createNativeVmo(.anonymous, 4096);
    try s.retainNativeVmo(shadow);
    try s.retainNativeVmo(root);
    try s.publishNativeVmoParent(shadow, root, 4096);
    const rw = vmaProt(.{ .read = true, .write = true });
    try s.createVmaWithRetainedVmo(p1, 0x4400_0000, 4096, rw, rw, mmapFlags(.{ .shared = true }), shadow, 0);
    try s.validateRamCacheMappings(root, 0, &.{0x8400_0000});
    try std.testing.expectError(error.ShadowAlias, s.validateRamCacheMappings(root, 1, &.{0x8400_1000}));
}

test "2 MiB page view can borrow pages after parent RAM becomes WC" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const page_count = 512;
    var pages: [page_count]u64 = undefined;
    for (&pages, 0..) |*page, index| page.* = 0x8400_0000 + @as(u64, @intCast(index)) * 4096;
    const parent = try s.createNativeVmo(.anonymous, page_count * 4096);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &pages, null);
    const owner = kernel.vmoBackingStoreOwner(.native_vmo, parent.index, parent.generation);
    const cached: kernel.RamCacheSnapshot = .{ .policy = &.{.{
        .base = 0x8400_0000,
        .bytes = page_count * 4096,
        .owner = owner,
        .mode = .wc,
        .pat_slot = 4,
    }} };
    try s.syncNativeRamCacheState(parent, &cached);
    const view = try s.createNativePageView(parent, page_count);
    var reversed: [page_count]u64 = undefined;
    for (&reversed, 0..) |*page, index| page.* = pages[page_count - 1 - index];
    for (0..page_count / 64) |chunk| {
        const first = chunk * 64;
        try s.installNativeVmoPages(view, first, reversed[first .. first + 64], null);
    }
    try std.testing.expectEqual(@as(?u64, pages[page_count - 1]), s.nativeVmoResolvedPagePaddr(view, 0));
    try std.testing.expectEqual(@as(?u64, pages[0]), s.nativeVmoResolvedPagePaddr(view, page_count - 1));
}

test "cached RAM final release waits for WB restoration including view and pending policy" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const parent = try s.createNativeVmo(.anonymous, 8192);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{ 0x8400_0000, 0x8400_1000 }, null);
    const owner = kernel.vmoBackingStoreOwner(.native_vmo, parent.index, parent.generation);
    const cached: kernel.RamCacheSnapshot = .{ .policy = &.{.{
        .base = 0x8400_0000,
        .bytes = 8192,
        .owner = owner,
        .mode = .wc,
        .pat_slot = 4,
    }} };
    const wb: kernel.RamCacheSnapshot = .{};
    const pending: kernel.RamCacheSnapshot = .{ .pending = cached.policy };
    const view = try s.createNativePageView(parent, 1);
    try s.installNativeVmoPages(view, 0, &.{0x8400_0000}, null);
    try s.syncNativeRamCacheState(parent, &cached);
    try std.testing.expectError(KernelError.InvalidState, s.syncNativeRamCacheState(view, &cached));
    try std.testing.expectError(KernelError.InvalidState, s.syncNativeRamCacheState(parent, &pending));
    try std.testing.expectError(KernelError.InvalidState, s.installNativeVmoPages(parent, 0, &.{0x8500_0000}, null));
    s.releaseUnmappedAnonymousVmoPageRange(p0, parent, 0, 2, &free_list);
    KernelState.releaseNativeVmoOwnedPages(s.nativeVmoSlot(parent).?, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    s.releaseNativeVmoWithFreeList(parent, &free_list);
    try std.testing.expectEqual(@as(usize, 0), s.pending_ram_cache_retirements);
    s.releaseNativeVmoWithFreeList(view, &free_list);
    try std.testing.expectEqual(@as(usize, 1), s.pending_ram_cache_retirements);
    try std.testing.expectEqual(parent, s.nextRamCacheRetirement().?);
    try std.testing.expectEqual(@as(?u64, 0x8400_0000), s.nativeVmoPagePaddr(parent, 0));
    try std.testing.expectError(KernelError.InvalidState, s.retainNativeVmo(parent));
    try std.testing.expectError(KernelError.InvalidState, KernelState.ensureNativeVmoPageStore(s.nativeVmoSlot(parent).?));
    try std.testing.expectError(KernelError.InvalidState, s.finishRamCacheRetirement(parent, &cached, &free_list));
    try std.testing.expectError(KernelError.InvalidState, s.finishRamCacheRetirement(parent, &pending, &free_list));
    s.releaseNativeVmoWithFreeList(parent, &free_list);
    s.releaseNativeVmo(parent);
    try std.testing.expectEqual(@as(usize, 1), s.pending_ram_cache_retirements);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    // This state unit models an already completed external cache barrier.
    // It does not claim to execute the hardware WB restoration itself.
    try s.finishRamCacheRetirement(parent, &wb, &free_list);
    try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
    try std.testing.expectEqual(@as(usize, 0), s.pending_ram_cache_retirements);
    try std.testing.expect(s.nextRamCacheRetirement() == null);
    try std.testing.expect(s.nativeVmoSlotConst(parent) == null);
    try std.testing.expectError(KernelError.InvalidState, s.finishRamCacheRetirement(parent, &wb, &free_list));
    s.next_native_vmo_scan = parent.index;
    const reused = try s.createNativeVmo(.anonymous, 4096);
    try std.testing.expectEqual(parent.index, reused.index);
    try std.testing.expect(reused.generation != parent.generation);
}

test "cached RAM explicit WB restoration resumes ordinary release and no-freelist drop defers" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]bool{ false, true }) |restore_before_release| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const ref = try s.createNativeVmo(.anonymous, 4096);
        try s.retainNativeVmo(ref);
        try s.installNativeVmoPages(ref, 0, &.{0x8400_0000}, null);
        const cached: kernel.RamCacheSnapshot = .{ .policy = &.{.{
            .base = 0x8400_0000,
            .bytes = 4096,
            .owner = kernel.vmoBackingStoreOwner(.native_vmo, ref.index, ref.generation),
            .mode = .uc_minus,
            .pat_slot = 2,
        }} };
        const wb: kernel.RamCacheSnapshot = .{};
        try s.syncNativeRamCacheState(ref, &cached);
        if (restore_before_release) {
            try s.syncNativeRamCacheState(ref, &wb);
            s.releaseNativeVmoWithFreeList(ref, &free_list);
            try std.testing.expectEqual(@as(usize, 0), s.pending_ram_cache_retirements);
        } else {
            s.releaseNativeVmo(ref);
            try std.testing.expectEqual(@as(usize, 1), s.pending_ram_cache_retirements);
            try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
            try s.finishRamCacheRetirement(ref, &wb, &free_list);
        }
        try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
    }
}

test "RAM retirement preserves owned backing when WB pages cannot return to DMA-held PMM" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    var first_hold = try DmaHold.init(0x8400_0000, 1);
    defer first_hold.end();
    var second_hold = try DmaHold.init(0x8400_2000, 1);
    defer second_hold.end();
    const ref = try s.createNativeVmo(.anonymous, 8192);
    try s.retainNativeVmo(ref);
    try s.installNativeVmoPages(ref, 0, &.{ 0x8400_0000, 0x8400_2000 }, null);
    const owner = kernel.vmoBackingStoreOwner(.native_vmo, ref.index, ref.generation);
    const cached: kernel.RamCacheSnapshot = .{ .policy = &.{.{
        .base = 0x8400_0000,
        .bytes = 4096,
        .owner = owner,
        .mode = .wc,
        .pat_slot = 4,
    }} };
    try s.syncNativeRamCacheState(ref, &cached);
    try std.testing.expectError(KernelError.InvalidState, s.retiredRamCacheOwner(ref));
    s.releaseNativeVmoWithFreeList(ref, &free_list);
    var seen: [1]bool = undefined;
    try std.testing.expectEqual(owner, try s.validateRetiredRamCachePages(ref, &.{0x8400_0000}, &seen));
    try std.testing.expectError(KernelError.InvalidState, s.validateRetiredRamCachePages(ref, &.{0x8400_1000}, &seen));
    const wb: kernel.RamCacheSnapshot = .{};
    try std.testing.expectError(KernelError.PageQuarantined, s.finishRamCacheRetirement(ref, &wb, &free_list));
    try std.testing.expectEqual(@as(usize, 1), s.pending_ram_cache_retirements);
    try std.testing.expectEqual(ref, s.nextRamCacheRetirement().?);
    try std.testing.expectEqual(@as(?u64, 0x8400_0000), s.nativeVmoPagePaddr(ref, 0));
    try std.testing.expectEqual(@as(?u64, 0x8400_2000), s.nativeVmoPagePaddr(ref, 1));
    // Only the first PFN can join an existing PMM range; the second remains
    // owned. A later retry must not return the first page for a second time.
    first_hold.end();
    try std.testing.expectError(KernelError.PageQuarantined, s.finishRamCacheRetirement(ref, &wb, &free_list));
    try std.testing.expectEqual(@as(?u64, null), s.nativeVmoPagePaddr(ref, 0));
    try std.testing.expectEqual(@as(?u64, 0x8400_2000), s.nativeVmoPagePaddr(ref, 1));
    // Empty fixture PMM models newly available range descriptors.
    second_hold.end();
    try s.finishRamCacheRetirement(ref, &wb, &free_list);
    try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
    try std.testing.expectEqual(@as(usize, 0), s.pending_ram_cache_retirements);
    try std.testing.expect(s.nativeVmoSlotConst(ref) == null);
}

test "page view lifetime survives failure multiple closes and generation reuse" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const parent = try s.createNativeVmo(.anonymous, 8192);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{ 0x8400_0000, 0x8400_1000 }, null);
    const slot = s.nativeVmoSlot(parent).?;
    try std.testing.expectError(KernelError.InvalidState, s.createNativePageView(parent, 0));

    // Failure to retain the parent must unwind the unlinked child.
    slot.ref_count = std.math.maxInt(u32);
    const live_before_failure = s.native_vmos.live;
    try std.testing.expectError(KernelError.TableFull, s.createNativePageView(parent, 1));
    try std.testing.expectEqual(live_before_failure, s.native_vmos.live);
    slot.ref_count = 1;

    const first = try s.createNativePageView(parent, 1);
    const second = try s.createNativePageView(parent, 1);
    try s.installNativeVmoPages(first, 0, &.{0x8400_0000}, null);
    // A later setup failure does not remove the published parent retain or
    // release borrowed pages; normal rollback releases the child.
    try std.testing.expectError(KernelError.InvalidState, s.installNativeVmoPages(second, 0, &.{0x8400_1001}, null));
    s.releaseUnmappedAnonymousVmoPageRange(p0, parent, 0, 2, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    s.releaseNativeVmoWithFreeList(first, &free_list);
    s.releaseUnmappedAnonymousVmoPageRange(p0, parent, 0, 2, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    s.releaseNativeVmoWithFreeList(second, &free_list);
    s.releaseUnmappedAnonymousVmoPageRange(p0, parent, 0, 1, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount()); // explicit temporary owner still pins backing
    s.releaseNativeVmoWithFreeList(parent, &free_list);
    try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
    s.next_native_vmo_scan = parent.index;
    const reused = try s.createNativeVmo(.anonymous, 4096);
    try std.testing.expectEqual(parent.index, reused.index);
    try std.testing.expect(parent.generation != reused.generation);
    try s.retainNativeVmo(reused);
    s.releaseNativeVmo(reused);
}

test "device memory backing validates BAR scatter and never frees device pages" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const backing = try s.createNativeDeviceMemoryBacking(.{ .device = 1, .bar_index = 0, .cache = .wc }, 0xd000_0000, 0x10000, &.{ 0x9000, 0, 0x3000 }, null);
    try std.testing.expectEqual(@as(?u64, 0xd000_9000), s.nativeVmoResolvedPagePaddr(backing, 0));
    try std.testing.expectEqual(@as(?u64, 0xd000_0000), s.nativeVmoResolvedPagePaddr(backing, 1));
    try std.testing.expectEqual(@as(?u64, 0xd000_3000), s.nativeVmoResolvedPagePaddr(backing, 2));
    try std.testing.expectEqual(@as(?u64, null), s.nativeVmoResolvedPagePaddr(backing, 3));
    try std.testing.expectError(KernelError.InvalidState, s.installNativeVmoPages(backing, 0, &.{0x8100_0000}, null));
    try std.testing.expectError(KernelError.InvalidState, s.createNativePageView(backing, 1));
    // Until cache-aware fault publication is connected, even an internal
    // caller cannot accidentally expose this backing as ordinary WB RAM.
    try std.testing.expectError(KernelError.InvalidState, s.createVmaWithRetainedVmo(p0, 0x4300_0000, 4096, vmaProt(.{ .read = true }), vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), backing, 0));
    s.releaseNativeVmoWithFreeList(backing, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    try std.testing.expect(s.nativeVmoSlotConst(backing) == null);
    try std.testing.expectError(KernelError.InvalidState, s.createNativeDeviceMemoryBacking(.{ .device = 1, .bar_index = 0, .cache = .uc }, 0xd000_0000, 4096, &.{4096}, null));
    try std.testing.expectError(KernelError.InvalidState, s.createNativeDeviceMemoryBacking(.{ .device = 1, .bar_index = 0, .cache = .wc }, 0xffff_ffff_ffff_f000, 4096, &.{0}, null));
    try std.testing.expectError(KernelError.InvalidState, s.createNativeDeviceMemoryBacking(.{ .device = 1, .bar_index = 0, .cache = .wc }, 0xd000_0000, 4096, &.{1}, null));
    try std.testing.expectError(KernelError.InvalidState, s.createNativeDeviceMemoryBacking(.{ .device = 0, .bar_index = 0, .cache = .wc }, 0xd000_0000, 4096, &.{0}, null));
}

test "device memory cache lease survives references and pending retirement" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const ref = try s.createNativeDeviceMemoryBacking(.{ .device = 1, .bar_index = 0, .cache = .wc }, 0xd000_0000, 0x10000, &.{ 0x9000, 0, 0x3000 }, null);
    try std.testing.expectError(KernelError.InvalidState, s.activateNativeDeviceMemory(ref, 1, 0));
    try s.activateNativeDeviceMemory(ref, 1, 7);
    try std.testing.expect(s.deviceMemoryCacheConflict(0xd000_3000, 4096, false));
    try std.testing.expect(!s.deviceMemoryCacheConflict(0xd000_3000, 4096, true));
    try std.testing.expect(!s.deviceMemoryCacheConflict(0xe000_0000, 4096, false));
    try s.retainNativeVmo(ref);
    s.releaseNativeVmoWithFreeList(ref, &free_list);
    try std.testing.expectEqual(@as(usize, 0), s.pending_device_retirements);
    s.releaseNativeVmoWithFreeList(ref, &free_list);
    try std.testing.expectEqual(@as(usize, 1), s.pending_device_retirements);
    try std.testing.expectEqual(ref, s.nextDeviceMemoryRetirement().?);
    try std.testing.expect(s.deviceMemoryCacheConflict(0xd000_3000, 4096, false));
    try std.testing.expectError(KernelError.InvalidState, s.retainNativeVmo(ref));
    try std.testing.expectEqual(@as(?u3, null), s.nativeVmoDevicePatSlot(ref));
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    try s.finishDeviceMemoryRetirement(ref);
    try std.testing.expectEqual(@as(usize, 0), s.pending_device_retirements);
    try std.testing.expect(!s.deviceMemoryCacheConflict(0xd000_3000, 4096, false));
    try std.testing.expectError(KernelError.InvalidState, s.finishDeviceMemoryRetirement(ref));
}

test "device memory transferred mappings keep cache type through fork and revoke" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const ref = try s.createNativeDeviceMemoryBacking(.{ .device = 1, .bar_index = 0, .cache = .wc }, 0xd000_0000, 0x10000, &.{ 0x9000, 0, 0x3000 }, null);
    try s.activateNativeDeviceMemory(ref, 1, 7);
    const object = try s.createKernelObject(.vmo, .{ .vmo = ref });
    const rights = fdRights(.{ .inspect = true, .dup = true, .transfer = true, .close = true, .revoke = true, .map_read = true, .map_write = true });
    const fd = try s.installFd(p0, object, rights, .{}, 16);
    const remote = try s.transferFd(p0, p1, fd, 16, rights, .{}, .copy);
    try std.testing.expectError(KernelError.InvalidState, s.mmapFd(p1, remote, 0x4100_0000, 12288, vmaProt(.{ .read = true }), mmapFlags(.{ .private = true }), 0));
    _ = try s.mmapFd(p1, remote, 0x4100_0000, 12288, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0);
    try s.closeFdWithFreeList(p1, remote, &free_list);
    try s.setVmaProtRange(p1, 0x4100_1000, 4096, vmaProt(.{ .read = true }));
    try s.cloneVmaTableForFork(p1, p2);
    const mapping = s.nativeVmaFaultMapping(p2, 0x4100_1000, false, false).?;
    try std.testing.expectEqual(@as(u64, 0xd000_0000), mapping.paddr);
    try std.testing.expectEqual(@as(?u3, 1), mapping.device_pat_slot);
    try std.testing.expect(s.nativeVmaDmaPinMode(p2, 0x4100_1000, false) == null);
    try std.testing.expect(s.nativeVmaFaultMapping(p2, 0x4100_1000, true, false) == null);
    try std.testing.expect(s.nativeVmaFaultMapping(p2, 0x4100_1000, false, true) == null);
    try s.munmapRangeWithFreeList(p1, 0x4100_0000, 12288, &free_list);
    _ = try s.revokeVmoFdWithFreeList(p0, fd, &free_list, NoopUnmapper{});
    try std.testing.expect(s.nativeVmaFaultMapping(p2, 0x4100_1000, false, false) == null);
    try std.testing.expectEqual(@as(usize, 1), s.pending_device_retirements);
    try s.finishDeviceMemoryRetirement(ref);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
}

test "kernel clock page is shared read-only only and never freed" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const paddr: u64 = 0x8500_0000;
    const fd = try s.openKernelClockPageFd(p0, paddr, .{}, 16, null);
    const ref = s.kernel_clock_page_vmo;
    try std.testing.expect(!ref.isNull());
    try std.testing.expectEqual(@as(?u64, paddr), s.nativeVmoResolvedPagePaddr(ref, 0));
    // Every open shares the one VMO; a different page is never accepted.
    const fd2 = try s.openKernelClockPageFd(p0, paddr, .{}, 16, null);
    const ref2 = s.nativeVmoRefForFd(p0, fd2).?;
    try std.testing.expect(ref2.index == ref.index and ref2.generation == ref.generation);
    try std.testing.expectError(KernelError.InvalidState, s.openKernelClockPageFd(p0, paddr + 4096, .{}, 16, null));
    const entry = s.fdEntryConst(p0, fd).?;
    try std.testing.expect(entry.rights.map_read and !entry.rights.map_write and !entry.rights.map_exec and
        !entry.rights.resize and !entry.rights.cache_policy and !entry.rights.revoke and !entry.rights.pager_attach);

    // Private (copy-on-write), writable, executable and fixed-private maps fail.
    try std.testing.expectError(KernelError.InvalidState, s.mmapFd(p0, fd, 0x4300_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .private = true }), 0));
    try std.testing.expectError(KernelError.InvalidState, s.mmapFd(p0, fd, 0x4300_0000, 4096, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0));
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmapIntoProcess(p0, fd, p1, 0x4400_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .fixed = true, .private = true }), 0, &free_list));
    // Not even an internal caller with full protections can make it writable.
    try std.testing.expectError(KernelError.InvalidState, s.createVmaWithRetainedVmo(p0, 0x4500_0000, 4096, vmaProt(.{ .read = true, .write = true }), vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), ref, 0));
    try std.testing.expectError(KernelError.InvalidState, s.createVmaWithRetainedVmo(p0, 0x4500_0000, 4096, vmaProt(.{ .read = true }), vmaProt(.{ .read = true, .exec = true }), mmapFlags(.{ .shared = true }), ref, 0));
    // Other VMO operations refuse the borrowed page.
    try std.testing.expectError(KernelError.InvalidState, s.installNativeVmoPages(ref, 0, &.{0x8600_0000}, null));
    try std.testing.expectError(KernelError.InvalidState, s.createNativePageView(ref, 1));
    try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, fd, 8192, &free_list));

    // A shared read-only view faults to the page, never for write, never DMA.
    _ = try s.mmapFd(p0, fd, 0x4300_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 0);
    try std.testing.expectEqual(paddr, (s.nativeVmaFaultMapping(p0, 0x4300_0000, false, false) orelse unreachable).paddr);
    try std.testing.expect(s.nativeVmaFaultMapping(p0, 0x4300_0000, true, false) == null);
    try std.testing.expect(s.nativeVmaFaultMapping(p0, 0x4300_0000, false, true) == null);
    try std.testing.expect(s.nativeVmaDmaPinMode(p0, 0x4300_0000, false) == null);
    try std.testing.expectError(KernelError.InvalidState, s.setVmaProtRange(p0, 0x4300_0000, 4096, vmaProt(.{ .read = true, .write = true })));
    // process_map without replacement and mremap move/grow refuse it too.
    try std.testing.expectError(KernelError.InvalidState, s.mmapFdIntoProcess(p0, fd, p1, 0x4600_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .private = true }), 0));
    try std.testing.expectError(KernelError.InvalidState, s.mmapFdIntoProcess(p0, fd, p1, 0x4600_0000, 4096, vmaProt(.{ .read = true, .write = true }), mmapFlags(.{ .shared = true }), 0));
    try std.testing.expectError(KernelError.InvalidState, s.mmapFdIntoProcess(p0, fd, p1, 0x4600_0000, 4096, vmaProt(.{ .read = true, .exec = true }), mmapFlags(.{ .shared = true }), 0));
    _ = try s.mmapFdIntoProcess(p0, fd, p1, 0x4600_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 0);
    try std.testing.expect(s.nativeVmaFaultMapping(p1, 0x4600_0000, true, false) == null);
    try s.munmapRangeWithFreeList(p1, 0x4600_0000, 4096, &free_list);
    try std.testing.expectError(KernelError.InvalidState, s.prepareMremapWithFreeList(p0, 0x4300_0000, 4096, 8192, 0, true, false, &free_list));
    try std.testing.expectError(KernelError.InvalidState, s.prepareMremapWithFreeList(p0, 0x4300_0000, 4096, 4096, 0x4700_0000, true, true, &free_list));
    // Fork keeps the same shared read-only view.
    try s.cloneVmaTableForFork(p0, p2);
    try std.testing.expectEqual(paddr, (s.nativeVmaFaultMapping(p2, 0x4300_0000, false, false) orelse unreachable).paddr);
    try std.testing.expect(s.nativeVmaFaultMapping(p2, 0x4300_0000, true, false) == null);

    // Dropping every user reference, and even the kernel's own, frees nothing.
    try s.munmapRangeWithFreeList(p0, 0x4300_0000, 4096, &free_list);
    try s.munmapRangeWithFreeList(p2, 0x4300_0000, 4096, &free_list);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try s.closeFdWithFreeList(p0, fd2, &free_list);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(ref));
    @TypeOf(s.nativeVmoSlot(ref).?.references).disconnect(&s.clock_reference, true);
    s.kernel_clock_page_vmo = .{};
    s.releaseNativeVmoWithFreeList(ref, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
}

test "page view borrows scatter pages until its last reference closes" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const parent = try s.createNativeVmo(.anonymous, 3 * kernel.native_page_size);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{ 0x8100_0000, 0x8200_0000, 0x8300_0000 }, null);

    const view = try s.createNativePageView(parent, 2);
    try s.installNativeVmoPages(view, 0, &.{ 0x8300_0000, 0x8100_0000 }, null);
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

test "page view refresh requires same parent and immutable maximum authority" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    const parent_rights = fdRights(.{ .close = true, .dup = true, .share = true, .map_read = true, .map_write = true, .map_exec = true });
    const parent_fd = try s.createAnonymousVmoFd(p0, 8192, parent_rights, .{}, 16);
    const other_fd = try s.createAnonymousVmoFd(p0, 8192, parent_rights, .{}, 16);
    const parent = s.nativeVmoRefForFd(p0, parent_fd).?;
    const view = try s.createNativePageView(parent, 1);
    s.nativeVmoSlot(view).?.page_view_max_prot = .{ .read = true, .write = true, .exec = true };
    const object = try s.createKernelObject(.vmo, .{ .vmo = view });
    // An updater may attenuate its own FD, but cannot erase writable/executable
    // authority already delegated through other FDs or VMAs of the view.
    const view_fd = try s.installFd(p0, object, fdRights(.{ .close = true, .dup = true, .revoke = true, .map_read = true }), .{}, 16);
    const read_parent = try s.dupFd(p0, parent_fd, 16, fdRights(.{ .close = true, .share = true, .map_read = true }), .{});
    const no_share = try s.dupFd(p0, parent_fd, 16, fdRights(.{ .close = true, .map_read = true, .map_write = true, .map_exec = true }), .{});
    const no_exec = try s.dupFd(p0, parent_fd, 16, fdRights(.{ .close = true, .share = true, .map_read = true, .map_write = true }), .{});
    const no_revoke = try s.dupFd(p0, view_fd, 16, fdRights(.{ .close = true, .map_read = true }), .{});
    try std.testing.expectError(KernelError.InvalidState, s.nativePageViewRefreshAuthority(p0, view_fd, other_fd));
    try std.testing.expectError(KernelError.InvalidState, s.nativePageViewRefreshAuthority(p0, view_fd, read_parent));
    try std.testing.expectError(KernelError.InvalidState, s.nativePageViewRefreshAuthority(p0, view_fd, no_share));
    try std.testing.expectError(KernelError.InvalidState, s.nativePageViewRefreshAuthority(p0, view_fd, no_exec));
    try std.testing.expectError(KernelError.InvalidState, s.nativePageViewRefreshAuthority(p0, no_revoke, parent_fd));
    try std.testing.expectError(KernelError.InvalidState, s.nativePageViewRefreshAuthority(p0, parent_fd, parent_fd));
    const authorized = try s.nativePageViewRefreshAuthority(p0, view_fd, parent_fd);
    try std.testing.expectEqual(view, authorized);
    // A parent identity includes its generation, not just its reusable slot.
    s.nativeVmoSlot(view).?.backing.parent.ref.generation +%= 1;
    try std.testing.expectError(KernelError.InvalidState, s.nativePageViewRefreshAuthority(p0, view_fd, parent_fd));
    s.nativeVmoSlot(view).?.backing.parent.ref = parent;
    for ([_]kernel.Fd{ view_fd, no_revoke, read_parent, no_share, no_exec, other_fd, parent_fd }) |fd|
        try s.closeFd(p0, fd);
}

test "sparse page view refresh preserves aliases and unavailable holes" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const parent = try s.createNativeVmo(.anonymous, 3 * 4096);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{ 0x8100_0000, 0x8200_0000, 0x8300_0000 }, null);
    const view = try s.createNativePageView(parent, 2);
    s.nativeVmoSlot(view).?.page_view_sparse = true;
    try s.installNativeVmoPages(view, 0, &.{ 0x8100_0000, 0 }, null);
    try std.testing.expect(s.nativeVmoIsSparsePageView(view));
    try std.testing.expect(!s.nativeVmoIsSparsePageView(.{ .index = view.index, .generation = view.generation + 1 }));
    s.nativeVmoSlot(parent).?.page_view_sparse = true;
    try std.testing.expect(!s.nativeVmoIsSparsePageView(parent));
    s.nativeVmoSlot(parent).?.page_view_sparse = false;
    const object = try s.createKernelObject(.vmo, .{ .vmo = view });
    const fd = try s.installFd(p0, object, fdRights(.{ .close = true, .map_read = true, .map_write = true }), .{}, 16);
    _ = try s.mmapFd(p0, fd, 0x4300_0000, 8192, .{ .read = true, .write = true, .pkey = 3 }, .{ .shared = true, .pkey = 3 }, 0);
    var prepared = try s.prepareFixedFdMmapIntoProcess(p0, fd, p1, 0x4400_0000, 8192, .{ .read = true, .pkey = 5 }, .{ .shared = true, .fixed = true, .pkey = 5 }, 0, &free_list);
    // Test the same prepare/commit path used by MAP_FIXED without replacing
    // the hole with anonymous demand-zero backing.
    s.commitFixedMmapPrepared(&prepared, &free_list);
    try std.testing.expect(s.nativeVmaFaultMapping(p0, 0x4300_1000, false, false) == null);
    try std.testing.expect(!s.nativeVmoIsZeroOnDemand(view));
    try std.testing.expectEqual(@as(?u64, null), s.nativeVmoResolvedPagePaddrOrZero(view, 1));

    try std.testing.expect(s.nativeVmaFaultBackingUnavailable(p0, 0x4300_1000, 4));
    try std.testing.expect(s.nativeVmaFaultBackingUnavailable(p0, 0x4300_1000, 6));
    try std.testing.expect(!s.nativeVmaFaultBackingUnavailable(p0, 0x4300_1000, 20));
    try std.testing.expect(!s.nativeVmaFaultBackingUnavailable(p1, 0x4400_1000, 6));
    try std.testing.expect(!s.nativeVmaFaultBackingUnavailable(p0, 0x4300_0000, 4));
    try std.testing.expect(!s.nativeVmaFaultBackingUnavailable(p0, 0x4500_0000, 4));
    for ([_]u64{ 0, 5, 7, 12, 36, 68, 0x8004, 22 }) |ec|
        try std.testing.expect(!s.nativeVmaFaultBackingUnavailable(p0, 0x4300_1000, ec));
    s.nativeVmoSlot(view).?.page_view_sparse = false;
    try std.testing.expect(!s.nativeVmoIsSparsePageView(view));
    try std.testing.expect(!s.nativeVmaFaultBackingUnavailable(p0, 0x4300_1000, 4));
    s.nativeVmoSlot(view).?.page_view_sparse = true;
    s.nativeVmoSlot(view).?.page_store_owner +%= 1;
    try std.testing.expect(!s.nativeVmaFaultBackingUnavailable(p0, 0x4300_1000, 4));
    s.nativeVmoSlot(view).?.page_store_owner -%= 1;
    try s.setVmaProtRange(p1, 0x4400_0000, 8192, .{ .pkey = 5 });
    try std.testing.expect(!s.nativeVmaFaultBackingUnavailable(p1, 0x4400_1000, 4));
    try s.setVmaProtRange(p1, 0x4400_0000, 8192, .{ .read = true, .pkey = 5 });

    const staged = try s.createNativePageView(parent, 2);
    s.nativeVmoSlot(staged).?.page_view_sparse = true;
    try s.installNativeVmoPages(staged, 0, &.{ 0, 0x8300_0000 }, null);
    const Invalidator = struct {
        calls: usize = 0,
        fail_at: usize = 0,
        pub fn invalidate(self: *@This(), _: PrincipalId, _: u64, _: u64) bool {
            self.calls += 1;
            return self.calls != self.fail_at;
        }
    };
    var invalidator = Invalidator{ .fail_at = 2 };
    try std.testing.expectError(KernelError.InvalidState, s.refreshNativePageView(view, staged, &invalidator));
    try std.testing.expectEqual(@as(usize, 2), invalidator.calls);
    try std.testing.expectEqual(@as(?u64, 0x8100_0000), s.nativeVmoResolvedPagePaddr(view, 0));
    try std.testing.expectEqual(@as(?u64, null), s.nativeVmoResolvedPagePaddrOrZero(view, 1));
    invalidator = .{};
    try s.refreshNativePageView(view, staged, &invalidator);
    try std.testing.expect(!s.nativeVmaFaultBackingUnavailable(p0, 0x4300_1000, 4));
    try std.testing.expectEqual(@as(usize, 2), invalidator.calls);
    try std.testing.expect(s.nativeVmaFaultMapping(p0, 0x4300_0000, false, false) == null);
    try std.testing.expectEqual(@as(u64, 0x8300_0000), s.nativeVmaFaultMapping(p0, 0x4300_1000, true, false).?.paddr);
    try std.testing.expectEqual(@as(u64, 0x8300_0000), s.nativeVmaFaultMapping(p1, 0x4400_1000, false, false).?.paddr);
    try std.testing.expect(s.nativeVmaFaultMapping(p1, 0x4400_1000, true, false) == null);
    try std.testing.expectEqual(@as(u4, 3), s.vmaEntryConst(p0, 0x4300_0000).?.prot.pkey);
    try std.testing.expectEqual(@as(u4, 5), s.vmaEntryConst(p1, 0x4400_0000).?.prot.pkey);
    s.releaseNativeVmoWithFreeList(staged, &free_list);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    try s.munmapRangeWithFreeList(p0, 0x4300_0000, 8192, &free_list);
    try s.munmapRangeWithFreeList(p1, 0x4400_0000, 8192, &free_list);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(parent));
    s.releaseNativeVmoWithFreeList(parent, &free_list);
    try std.testing.expectEqual(@as(usize, 3), free_list.pageCount());
}

test "page view mremap preserves borrowed identity offsets and refresh after relocation and shrink" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = 1 << 40 }});
    const parent = try s.createNativeVmo(.anonymous, 4 * 4096);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{ 0x8100_0000, 0x8200_0000, 0x8300_0000, 0x8400_0000 }, null);
    const view = try s.createNativePageView(parent, 4);
    s.nativeVmoSlot(view).?.page_view_sparse = true;
    try s.installNativeVmoPages(view, 0, &.{ 0x8100_0000, 0, 0x8300_0000, 0x8400_0000 }, null);
    const object = try s.createKernelObject(.vmo, .{ .vmo = view });
    const fd = try s.installFd(p0, object, fdRights(.{ .close = true, .map_read = true, .map_write = true }), .{}, 16);
    const old: u64 = 0x5000_0000;
    const alias: u64 = 0x5100_0000;
    const moved: u64 = 0x5200_0000;
    _ = try s.mmapFd(p0, fd, old, 16384, .{ .read = true, .write = true, .pkey = 3 }, .{ .shared = true, .pkey = 3 }, 0);
    _ = try s.mmapFd(p0, fd, alias, 16384, .{ .read = true, .pkey = 5 }, .{ .shared = true, .pkey = 5 }, 0);
    const source = s.vmaEntryForVaConst(p0, old).?;
    try std.testing.expect(s.vmaRemapSourceAllowed(source, 8192, 4096));
    try std.testing.expectError(KernelError.InvalidState, s.prepareMremapWithFreeList(p0, old, 4096, 8192, moved, true, true, &free_list));
    try std.testing.expectEqual(view, s.vmaEntryForVaConst(p0, old).?.vmo);
    var rejected = source.*;
    rejected.flags.private = true;
    rejected.flags.shared = false;
    try std.testing.expect(!s.vmaRemapSourceAllowed(&rejected, 8192, 4096));
    rejected = source.*;
    rejected.vmo = parent; // A private file snapshot is not a borrowed view.
    rejected.flags.private = true;
    rejected.flags.shared = false;
    try std.testing.expect(!s.vmaRemapSourceAllowed(&rejected, 8192, 4096));
    rejected = source.*;
    for ([_]kernel.NativeVmoKind{ .device_memory, .kernel_page }) |kind| {
        const unsupported = try s.createNativeVmo(kind, 4096);
        rejected.vmo = unsupported;
        try std.testing.expect(!s.vmaRemapSourceAllowed(&rejected, 4096, 4096));
    }
    const before_abort = s.nativeVmoRefCount(view);
    var abandoned = try s.prepareMremapWithFreeList(p0, old + 4096, 8192, 4096, moved, true, true, &free_list);
    try std.testing.expect(s.vmaEntryForVaConst(p0, moved) == null);
    try std.testing.expectEqual(@as(u64, 16384), s.vmaEntryConst(p0, old).?.size_bytes);
    s.discardMremapPrepared(&abandoned, &free_list);
    try std.testing.expectEqual(before_abort, s.nativeVmoRefCount(view));
    try std.testing.expectEqual(@as(u64, 16384), s.vmaEntryConst(p0, old).?.size_bytes);
    // Move/shrink the middle of a VMA; both prefix and suffix remain at their
    // original offsets. Borrowed tail PFNs must not enter the PMM free list.
    try std.testing.expectEqual(moved, try s.mremapRangeWithFreeList(p0, old + 4096, 8192, 4096, moved, true, true, &free_list));
    try std.testing.expectEqual(@as(u64, 4096), s.vmaEntryConst(p0, old).?.size_bytes);
    try std.testing.expectEqual(@as(u64, 3 * 4096), s.vmaEntryConst(p0, old + 3 * 4096).?.vmo_offset);
    try std.testing.expect(s.vmaEntryForVaConst(p0, old + 4096) == null);
    const destination = s.vmaEntryConst(p0, moved).?;
    try std.testing.expectEqual(view, destination.vmo);
    try std.testing.expectEqual(@as(u64, 4096), destination.vmo_offset);
    try std.testing.expectEqual(@as(u4, 3), destination.prot.pkey);
    try std.testing.expectEqual(@as(u4, 3), destination.max_prot.pkey);
    try std.testing.expect(s.nativeVmaFaultBackingUnavailable(p0, moved, 4));
    const staged = try s.createNativePageView(parent, 4);
    s.nativeVmoSlot(staged).?.page_view_sparse = true;
    try s.installNativeVmoPages(staged, 0, &.{ 0x8100_0000, 0x8200_0000, 0, 0x8400_0000 }, null);
    const Invalidator = struct {
        calls: usize = 0,
        pub fn invalidate(self: *@This(), _: PrincipalId, _: u64, _: u64) bool {
            self.calls += 1;
            return true;
        }
    };
    var invalidator = Invalidator{};
    try s.refreshNativePageView(view, staged, &invalidator);
    try std.testing.expectEqual(@as(usize, 4), invalidator.calls);
    try std.testing.expectEqual(@as(u64, 0x8200_0000), s.nativeVmaFaultMapping(p0, moved, true, false).?.paddr);
    try std.testing.expectEqual(@as(u64, 0x8200_0000), s.nativeVmaFaultMapping(p0, alias + 4096, false, false).?.paddr);
    try std.testing.expectEqual(@as(u64, 0x8400_0000), s.nativeVmaFaultMapping(p0, old + 3 * 4096, true, false).?.paddr);
    try std.testing.expectEqual(alias, try s.mremapRangeWithFreeList(p0, alias, 16384, 8192, 0, false, false, &free_list));
    try std.testing.expectEqual(@as(u4, 5), s.vmaEntryConst(p0, alias).?.prot.pkey);
    try std.testing.expectEqual(@as(u64, 8192), s.vmaEntryConst(p0, alias).?.size_bytes);
    const again: u64 = 0x5300_0000;
    try std.testing.expectEqual(again, try s.mremapRangeWithFreeList(p0, moved, 4096, 4096, again, true, true, &free_list));
    try std.testing.expect(s.vmaEntryForVaConst(p0, moved) == null);
    try std.testing.expectEqual(@as(u64, 0x8200_0000), s.nativeVmaFaultMapping(p0, again, true, false).?.paddr);
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    try std.testing.expectEqual(@as(?u64, 0x8300_0000), s.nativeVmoResolvedPagePaddr(parent, 2));
    s.releaseNativeVmoWithFreeList(staged, &free_list);
    try s.munmapRangeWithFreeList(p0, old, 16384, &free_list);
    try s.munmapRangeWithFreeList(p0, alias, 8192, &free_list);
    try s.munmapRangeWithFreeList(p0, again, 4096, &free_list);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(parent));
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    s.releaseNativeVmoWithFreeList(parent, &free_list);
    try std.testing.expectEqual(@as(usize, 4), free_list.pageCount());
}

test "page view mappings retain their non-executable creation ceiling in every map path" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const parent = try s.createNativeVmo(.anonymous, kernel.native_page_size);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{0x8400_0000}, null);
    const view = try s.createNativePageView(parent, 1);
    try s.installNativeVmoPages(view, 0, &.{0x8400_0000}, null);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x8500_0000, .end = (0x8500_0000) + (2) * 4096 }});
    try free_list.seedContiguousRange(0x8500_0000, 2);
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
    try s.installNativeVmoPages(view, 0, &.{second_page}, null);
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
    try kernel.ReferenceCheck.check(s);
    const expected = @as(u64, object_refs) |
        (@as(u64, native_refs) << fd_abi.vmo_info_native_refs_shift);
    try std.testing.expectEqual(@as(?u64, expected), s.fdVmoLifetimeInfo(p0, fd));
}

test "VMO lifetime snapshot covers dup mapping fork split and derived views" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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

test "preferred mmap arena distinguishes oversized placement from invalid requests" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    const process_abi = @import("kernel_abi_root").process_abi;
    const base = process_abi.user_aslr_base_va;
    const span = process_abi.user_aslr_end_va - base;
    const table = s.getVmaTable(p0) orelse unreachable;
    table.next_user_map_va = base + process_abi.user_aslr_granule;
    const cursor = table.next_user_map_va;
    for ([_]u64{ span + 4096, @as(u64, 128) << 30 }) |bytes| {
        try std.testing.expectError(KernelError.TableFull, s.findFreeUserMapVa(p0, bytes, 0));
        try std.testing.expectEqual(cursor, table.next_user_map_va);
    }
    try std.testing.expectError(KernelError.InvalidState, s.findFreeUserMapVa(p0, 0, 0));
    try std.testing.expectError(KernelError.InvalidState, s.findFreeUserMapVa(p0, 4097, 0));
    try std.testing.expectError(KernelError.InvalidState, s.findFreeUserMapVa(.Device0, span + 4096, 0));
    try std.testing.expectEqual(cursor, table.next_user_map_va);
    try std.testing.expectEqual(@as(usize, 0), table.active_count);
    try std.testing.expectEqual(cursor, try s.findFreeUserMapVa(p0, 4096, 0));
    // Finding a placement advances the cursor but does not publish a VMA.
    // An exact-fit request can still wrap to the empty arena's base.
    try std.testing.expectEqual(base, try s.findFreeUserMapVa(p0, span, 0));
    try std.testing.expectEqual(process_abi.user_aslr_end_va, table.next_user_map_va);
    try std.testing.expectEqual(@as(usize, 0), table.active_count);
}

test "anonymous vmo fd maps through vma ledger without page capability install" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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

test "MMIO reservation admission accepts a split 256 MiB hosted vmalloc window" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const window_base: u64 = 0x7000_0000;
    const window_size: u64 = 256 << 20;
    const bar_va: u64 = 0x71c0_0000;
    const bar_size: u64 = 512 << 10;
    const flags = mmapFlags(.{ .private = true, .anonymous = true, .noreserve = true, .fixed_noreplace = true });
    const ceiling = vmaProt(.{ .read = true, .write = true, .exec = true });
    _ = try s.createAnonymousVmaWithPages(p0, window_base, window_size, .{}, ceiling, flags, &free_list);
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, bar_va, bar_size));
    try s.setVmaProtRange(p0, bar_va, bar_size, vmaProt(.{ .read = true }));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, bar_va, bar_size));
    try s.setVmaProtRange(p0, bar_va, bar_size, .{});
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, bar_va, bar_size));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, bar_va, bar_size + 1));
    s.releasePrincipalNativeMemory(p0, &free_list);
}

test "MMIO reservation admission rejects used NONE RAM and COW" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
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
    slot.backing.parent.ref = vmo;
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 4096));
    slot.backing.parent.ref = .{};
    try kernel.setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, 1, slot.page_store_owner, 0x1234_5000, null);
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 3 * 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base + 4096, 4096));
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base, 4096));
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base + 2 * 4096, 4096));
    try std.testing.expectEqual(@as(?u64, 0x1234_5000), s.nativeVmoPagePaddr(vmo, 1));
    try kernel.setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, 1, slot.page_store_owner, 0, null);
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base, 3 * 4096));
    try s.cloneVmaTableForFork(p0, p1);
    s.markForkCowVmasCommitted(p0);
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p1, base, 4096));
    s.releasePrincipalNativeMemory(p1, &free_list);
    s.releasePrincipalNativeMemory(p0, &free_list);
}

test "MMIO reservation admission rejects address-bound MMIO and DMA" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    _ = try s.createDmaMappingFdWithBacking(p0, .{
        .device = 1,
        .user_va = base + 8192,
        .iova = 0x8002_0000,
        .size = 4096,
        .page_count = 1,
        .direction = .to_device,
    }, fdRights(.{ .close = true }), .{}, 16, &.{0x80020000});
    for (0..3) |page|
        try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base + page * 4096, 4096));
    try std.testing.expect(!s.userRangeIsUnbackedMmioReservation(p0, base, 4 * 4096));
    try std.testing.expect(s.userRangeIsUnbackedMmioReservation(p0, base + 3 * 4096, 4096));
}

test "MMIO cache leases compare overlapping physical pages rather than whole BARs" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const cache_abi = @import("kernel_abi_root").capsule_abi;
    _ = try s.createMmioRegionFd(p0, .{
        .device = 1,
        .paddr = 0xd000_0000,
        .user_va = 0x5000_0000,
        .size = 0x100000,
        .flags = @as(u64, @intFromEnum(cache_abi.MmioCache.uc)) <<
            cache_abi.mmio_map_cache_shift,
    }, fdRights(.{ .close = true }), .{}, 16);
    try std.testing.expect(s.mmioPhysicalCacheConflict(0xd000_0000, 4096, true));
    try std.testing.expect(s.mmioPhysicalCacheConflict(0xd00f_f000, 8192, true));
    try std.testing.expect(!s.mmioPhysicalCacheConflict(0xd010_0000, 4096, true));
    try std.testing.expect(!s.mmioPhysicalCacheConflict(0xd000_0000, 4096, false));
    _ = try s.createMmioRegionFd(p1, .{
        .device = 2,
        .paddr = 0xd010_0000,
        .user_va = 0x6000_0000,
        .size = 4096,
        .flags = @as(u64, @intFromEnum(cache_abi.MmioCache.wc)) <<
            cache_abi.mmio_map_cache_shift,
    }, fdRights(.{ .close = true }), .{}, 16);
    try std.testing.expect(s.mmioPhysicalCacheConflict(0xd010_0000, 4096, false));
    try std.testing.expect(!s.mmioPhysicalCacheConflict(0xd010_0000, 4096, true));
    try std.testing.expect(s.mmioPhysicalCacheConflict(0xffff_ffff_ffff_f000, 8192, true));
}

test "mmap and mprotect preserve combined write execute permissions" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const base: u64 = 0x4210_0000;
    var refs: [3]kernel.NativeVmoRef = undefined;
    for (&refs, 0..) |*ref, i| {
        ref.* = try s.createAnonymousVmaWithPages(p0, base + i * 8192, 4096, vmaProt(.{ .read = true }), vmaProt(.{ .read = true }), mmapFlags(.{ .private = true, .anonymous = true }), &free_list);
    }
    try s.munmapRangeWithFreeList(p0, base, 4096, &free_list);
    const table = s.getVmaTable(p0) orelse unreachable;
    try std.testing.expect(table.active_indices.get(@intCast(0)).?.* > table.active_indices.get(@intCast(1)).?.*);
    var prepared = try s.prepareMunmapRangeWithFreeList(p0, base + 8192, 12288, &free_list);
    s.commitMunmapPrepared(&prepared, &free_list);
    try std.testing.expectEqual(@as(usize, 0), table.active_count);
    for (refs) |ref| try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(ref));
}

test "munmap snapshot preserves middle split before and after source slot" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]bool{ false, true }) |prepared_path| {
        for ([_]bool{ false, true }) |earlier_suffix| {
            var s: KernelState = undefined;
            try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const base: u64 = 0x4230_0000;
    const ref = try s.createAnonymousVmaWithPages(p0, base, 12288, vmaProt(.{ .read = true }), vmaProt(.{ .read = true }), mmapFlags(.{ .private = true, .anonymous = true }), &free_list);
    const table = s.getVmaTable(p0) orelse unreachable;
    fillVmaTableExcept(&s, table, &.{});
    var failure = kernel.InvocationStorageTestAllocator{ .fail_at = 1 };
    defer failure.deinit();
    const saved_allocator = table.allocator;
    table.allocator = failure.interface();
    defer table.allocator = saved_allocator;
    try std.testing.expectError(KernelError.TableFull, s.prepareMunmapRangeWithFreeList(p0, base + 4096, 4096, &free_list));
    failure.fail_at = failure.calls + 1;
    try std.testing.expectError(KernelError.TableFull, s.munmapRangeWithFreeList(p0, base + 4096, 4096, &free_list));
    try std.testing.expectEqual(former_max_vmas_per_process, table.active_count);
    try std.testing.expectEqual(@as(u64, 12288), (s.vmaEntryConst(p0, base) orelse unreachable).size_bytes);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(ref));
}

test "process reset releases vma table and native vmo after fd close" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x1_0000_0000, .end = (0x1_0000_0000) + (4) * 4096 }});
    try free_list.seedContiguousRange(0x1_0000_0000, 4);
    const original_free = free_list.len;

    var pages: [2]u64 = undefined;
    pages[0] = (try s.allocPhysicalPage(&free_list)).paddr;
    pages[1] = (try s.allocPhysicalPage(&free_list)).paddr;
    try std.testing.expectEqual(original_free - 2, free_list.len);

    const fd = try s.createAnonymousVmoFd(p0, 8192, fdRights(.{ .map_read = true, .map_write = true }), .{}, 0);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    try s.installNativeVmoPages(vmo, 0, pages[0..], null);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x30_0000, .end = (0x30_0000) + (8) * 4096 }});
    try free_list.seedContiguousRange(0x30_0000, 8);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const base_va: u64 = 0x4828_0000;
    const mapping_count = former_max_vmas_per_process + 32;
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x38_0000, .end = (0x38_0000) + (4) * 4096 }});
    try free_list.seedContiguousRange(0x38_0000, 4);
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
    try s.installNativeVmoPages(first_vmo, 0, &[_]u64{page.paddr}, null);

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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x35_0000, .end = (0x35_0000) + (16) * 4096 }});
    try free_list.seedContiguousRange(0x35_0000, 16);
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
    try expectResourceEqualDeep(child_before, child_after_fork.*);
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
    try expectResourceEqualDeep(child_dirty, (s.vmaEntryConst(p1, arena_va) orelse unreachable).*);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    const source_index: usize = @intCast(table.active_indices.get(@intCast(0)).?.*);
    const free_index: usize = if (source_index == 0) 1 else 0;

    var index: usize = 0;
    while (index < former_max_vmas_per_process) : (index += 1) {
        if (index == source_index or index == free_index) continue;
        try table.reference_slots.prepareIndex(@intCast(index), table.allocator.?);
        try table.entries.prepareIndex(@intCast(index), table.allocator.?);
        try table.active_indices.prepareIndex(@intCast(table.active_count), table.allocator.?);
        s.installVmaEntry(table, index, .{
            .active = true,
            .start_va = 0x7000_0000 + @as(u64, @intCast(index)) * 0x1000,
            .size_bytes = 4096,
        });
    }
    try std.testing.expectEqual(former_max_vmas_per_process - 1, table.active_count);
    const original = table.entries.get(@intCast(source_index)).?.*;
    const original_refs = s.nativeVmoRefCount(vmo);

    try s.setVmaProtRange(p0, mapping_va + 4096, 4096, original.prot);
    try std.testing.expectEqual(original, table.entries.get(@intCast(source_index)).?.*);
    try std.testing.expectEqual(former_max_vmas_per_process - 1, table.active_count);
    try std.testing.expectEqual(original_refs, s.nativeVmoRefCount(vmo));

    var failure = kernel.InvocationStorageTestAllocator{ .fail_at = 1 };
    defer failure.deinit();
    const saved_allocator = table.allocator;
    table.allocator = failure.interface();
    defer table.allocator = saved_allocator;
    try std.testing.expectError(
        KernelError.OutOfFreePages,
        s.setVmaProtRange(
            p0,
            mapping_va + 4096,
            4096,
            vmaProt(.{ .read = true }),
        ),
    );
    try std.testing.expectEqual(original, table.entries.get(@intCast(source_index)).?.*);
    try std.testing.expect(table.entries.get(@intCast(free_index)) == null);
    try std.testing.expectEqual(former_max_vmas_per_process - 1, table.active_count);
    try std.testing.expectEqual(original_refs, s.nativeVmoRefCount(vmo));
}

test "fork fd and vma clones release vmo pages only after child cleanup" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x40_0000, .end = (0x40_0000) + (4) * 4096 }});
    try free_list.seedContiguousRange(0x40_0000, 4);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x60_0000, .end = (0x60_0000) + (4) * 4096 }});
    try free_list.seedContiguousRange(0x60_0000, 4);
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
    try kernel.ReferenceCheck.check(&s);
}

test "vmo revoke removes all fd vma and pending ipc references" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x50_0000, .end = (0x50_0000) + (6) * 4096 }});
    try free_list.seedContiguousRange(0x50_0000, 6);
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

fn returnRangeForTest(s: *KernelState, vmo: kernel.NativeVmoRef, count: usize, free_list: *FreePageList) void {
    // Exercise range return with a real surviving VMA, not a temporary owner:
    // C deliberately treats every temporary retain as a reclamation barrier.
    const slot = s.nativeVmoSlot(vmo).?;
    const offset = slot.size_bytes - 4096;
    s.createVmaWithRetainedVmo(p0, 0x7000_0000, 4096, vmaProt(.{ .read = true }), vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), vmo, offset) catch unreachable;
    s.releaseNativeVmo(vmo);
    s.releaseUnmappedAnonymousVmoPageRange(p0, vmo, 0, count, free_list);
    s.retainNativeVmo(vmo) catch unreachable;
    s.munmapRangeWithFreeList(p0, 0x7000_0000, 4096, free_list) catch unreachable;
}
fn returnVmoPagesForTest(s: *KernelState, vmo: kernel.NativeVmoRef, free_list: *FreePageList, partial: bool) void {
    if (partial) {
        returnRangeForTest(s, vmo, 2, free_list);
    } else {
        KernelState.releaseNativeVmoOwnedPages(s.nativeVmoSlot(vmo).?, free_list);
    }
}

test "VMO page return clears only successfully returned pages" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]bool{ false, true }) |partial| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const vmo = try s.createNativeVmo(.anonymous, 12288);
        try s.retainNativeVmo(vmo);
        const pages = [_]u64{ 0x8000_0000, 0x8000_1000 };
        try s.installNativeVmoPages(vmo, 0, &pages, null);
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

test "VMO page return preserves failed page and continues with neighbor merge" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]bool{ false, true }) |partial| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        try free_list.seedContiguousRange(0x100000, 1);
        const pages = [_]u64{ 0x1_0000_0000, 0x101000 };
        var hold = try DmaHold.init(pages[0], 1);
        defer hold.end();
        const vmo = try s.createNativeVmo(.anonymous, 12288);
        try s.retainNativeVmo(vmo);
        try s.installNativeVmoPages(vmo, 0, &pages, null);
        try std.testing.expectError(KernelError.PageQuarantined, free_list.appendPage(pages[0]));
        try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());

        returnVmoPagesForTest(&s, vmo, &free_list, partial);
        try std.testing.expectEqual(@as(?u64, pages[0]), s.nativeVmoPagePaddrOrHole(vmo, 0));
        try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, 1));
        try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
        try std.testing.expectEqual(@as(usize, 1), free_list.rangeCount());
        try std.testing.expectEqual(@as(usize, 2), free_list.rangeAt(0).len);
        try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(vmo));

        // DMA drains later: retry must return the held page
        // exactly once and must not return the already-cleared second page.
        hold.end();
        returnVmoPagesForTest(&s, vmo, &free_list, partial);
        try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, 0));
        try std.testing.expectEqual(@as(usize, 3), free_list.pageCount());
        s.releaseNativeVmoWithFreeList(vmo, &free_list);
    }
}

test "PMM rejects a duplicate VMO page before any backing pointer is cleared" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]bool{ false, true }) |partial| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const pages = [_]u64{ 0x8100_0000, 0x8100_1000 };
        const vmo = try s.createNativeVmo(.anonymous, 12288);
        try s.retainNativeVmo(vmo);
        try s.installNativeVmoPages(vmo, 0, &pages, null);
        // Deliberately inconsistent ownership tests the defensive failure;
        // production callers must never return an already-free physical page.
        try free_list.appendPage(pages[0]);
        try std.testing.expectError(error.FreeRangeOverlap, free_list.appendContiguousRange(pages[0], pages.len));
        try std.testing.expectEqual(@as(?u64, pages[1]), s.nativeVmoPagePaddrOrHole(vmo, 1));
        try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
        // Repair the deliberately inconsistent free copy before exercising
        // the production return path, whose invariant failures are fatal.
        _ = try free_list.popFront();
        returnVmoPagesForTest(&s, vmo, &free_list, partial);
        try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, 0));
        try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, 1));
        try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
        try std.testing.expectEqual(@as(usize, 1), free_list.rangeCount());
        s.releaseNativeVmo(vmo);
    }
}

test "VMO return batches bounded physical runs across snapshot boundaries" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]bool{ false, true }) |partial| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        var pages: [130]u64 = undefined;
        for (&pages, 0..) |*page, i| {
            // Two ascending runs, then descending/noncontiguous singletons.
            page.* = if (i < 65) 0x8200_0000 + i * 4096 else if (i < 128) 0x8300_0000 + (i - 65) * 4096 else 0x8400_0000 - (i - 128) * 8192;
        }
        const vmo = try s.createNativeVmo(.anonymous, (pages.len + 1) * 4096);
        try s.retainNativeVmo(vmo);
        try s.installNativeVmoPages(vmo, 0, &pages, null);
        if (partial) {
            returnRangeForTest(&s, vmo, 129, &free_list);
            try std.testing.expectEqual(@as(usize, 129), free_list.pageCount());
            try std.testing.expectEqual(@as(?u64, pages[129]), s.nativeVmoPagePaddrOrHole(vmo, 129));
        }
        s.releaseNativeVmoWithFreeList(vmo, &free_list);
        try std.testing.expectEqual(@as(usize, pages.len), free_list.pageCount());
        try std.testing.expectEqual(@as(usize, 4), free_list.rangeCount());
        for (pages) |paddr| try std.testing.expectError(KernelError.FreeRangeOverlap, free_list.appendPage(paddr));
        try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(vmo));
    }
}

test "VMO run return joins upper neighbor without metadata allocation" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    try free_list.seedContiguousRange(0x8500_2000, 1);
    const pages = [_]u64{ 0x8500_0000, 0x8500_1000 };
    const vmo = try s.createNativeVmo(.anonymous, 12288);
    try s.retainNativeVmo(vmo);
    try s.installNativeVmoPages(vmo, 0, &pages, null);
    returnVmoPagesForTest(&s, vmo, &free_list, true);
    try std.testing.expectEqual(@as(usize, 3), free_list.pageCount());
    try std.testing.expectEqual(@as(usize, 1), free_list.rangeCount());
    try std.testing.expectEqual(@as(usize, 3), free_list.rangeAt(0).len);
    for (0..2) |i| try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, @intCast(i)));
    s.releaseNativeVmoWithFreeList(vmo, &free_list);
}

test "VMO run return preserves DMA refusal and supports retry" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const pages = [_]u64{ 0x8600_0000, 0x8600_1000 };
    var hold = try DmaHold.init(pages[0], pages.len);
    defer hold.end();
    const vmo = try s.createNativeVmo(.anonymous, 12288);
    try s.retainNativeVmo(vmo);
    try s.installNativeVmoPages(vmo, 0, &pages, null);
    returnVmoPagesForTest(&s, vmo, &free_list, true);
    for (pages, 0..) |page, i| try std.testing.expectEqual(@as(?u64, page), s.nativeVmoPagePaddrOrHole(vmo, @intCast(i)));
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    hold.end();
    returnVmoPagesForTest(&s, vmo, &free_list, true);
    for (0..2) |i| try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(vmo, @intCast(i)));
    try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
    s.releaseNativeVmoWithFreeList(vmo, &free_list);
}

test "COW bounded run return preserves outer pages and null-list behavior" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]bool{ false, true }) |use_free_list| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const cow = try s.createNativeCowTable(68, &free_list);
        try s.retainNativeCowTable(cow);
        for (0..68) |i| try s.setNativeCowPagePaddr(cow, @intCast(i), 0x8700_0000 + i * 4096, null);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]u64{ 0, 4096 }) |offset| {
        for ([_]bool{ false, true }) |head| {
            var s: KernelState = undefined;
            try initFdState(&s);
            var free_list = FreePageList{};
            try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
            const base: u64 = 0x4680_0000;
            const pages = [_]u64{ 0x8500_0000, 0x8500_1000, 0x8500_2000, 0x8500_3000 };
            const fd = try s.createAnonymousVmoFd(p0, 16384, fdRights(.{ .map_read = true, .close = true }), .{}, 0);
            const ref = s.nativeVmoRefForFd(p0, fd).?;
            try s.installNativeVmoPages(ref, 0, &pages, null);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    const Mode = enum { fd_only, fd_and_vma, foreign_vma, local_vma, page_view, page_view_only };
    for (std.meta.tags(Mode)) |mode| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const base: u64 = 0x4690_0000;
        const pages = [_]u64{ 0x8600_0000, 0x8600_1000, 0x8600_2000 };
        const fd = try s.createAnonymousVmoFd(p0, 12288, fdRights(.{ .map_read = true, .close = true, .transfer = true }), .{}, 0);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        try s.installNativeVmoPages(ref, 0, &pages, null);
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
            try s.installNativeVmoPages(view.?, 0, pages[0..1], null);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]u64{ 0, 4096 }) |offset| {
        for ([_]bool{ false, true }) |protect_split| {
            var s: KernelState = undefined;
            try initFdState(&s);
            var free_list = FreePageList{};
            try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
            const base: u64 = 0x46a0_0000;
            const pages = [_]u64{ 0x8700_0000, 0x8700_1000, 0x8700_2000, 0x8700_3000, 0x8700_4000, 0x8700_5000, 0x8700_6000 };
            const fd = try s.createAnonymousVmoFd(p0, 7 * 4096, fdRights(.{ .map_read = true, .map_write = true, .close = true }), .{}, 0);
            const ref = s.nativeVmoRefForFd(p0, fd).?;
            try s.installNativeVmoPages(ref, 0, &pages, null);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    const Mode = enum { fd, view, foreign };
    for (std.meta.tags(Mode)) |mode| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const base: u64 = 0x46b0_0000;
        const pages = [_]u64{ 0x8800_0000, 0x8800_1000, 0x8800_2000 };
        const rights = fdRights(.{ .map_read = true, .close = true, .transfer = true });
        const fd = try s.createAnonymousVmoFd(p0, 12288, rights, .{}, 0);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        try s.installNativeVmoPages(ref, 0, &pages, null);
        _ = try s.mmapFd(p0, fd, base, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);
        _ = try s.mmapFd(p0, fd, base + 8192, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 8192);
        var view: ?kernel.NativeVmoRef = null;
        if (mode == .view) {
            view = try s.createNativePageView(ref, 1);
            try s.installNativeVmoPages(view.?, 0, pages[1..2], null);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    const Mode = enum { owned, fd, view, foreign, overlap, temporary };
    for (std.meta.tags(Mode)) |mode| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const base: u64 = 0x46c0_0000;
        const pages = [_]u64{ 0x8900_0000, 0x8900_1000, 0x8900_2000 };
        const rights = fdRights(.{ .map_read = true, .close = true, .transfer = true });
        const fd = try s.createAnonymousVmoFd(p0, 12288, rights, .{}, 0);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        try s.installNativeVmoPages(ref, 0, &pages, null);
        for (0..3) |index| {
            _ = try s.mmapFd(p0, fd, base + index * 4096, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), index * 4096);
        }
        var view: ?kernel.NativeVmoRef = null;
        if (mode == .view) {
            view = try s.createNativePageView(ref, 1);
            try s.installNativeVmoPages(view.?, 0, pages[1..2], null);
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
        const releasable = mode == .owned;
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x20_0000, .end = (0x20_0000) + (2) * 4096 }});
    try free_list.seedContiguousRange(0x20_0000, 2);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x80_0000, .end = (0x80_0000) + (8) * 4096 }});
    try free_list.seedContiguousRange(0x80_0000, 8);

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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x90_0000, .end = 0xa0_2000 }});
    try free_list.seedContiguousRange(0x90_0000, 2);
    try free_list.seedContiguousRange(0xa0_0000, 2);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0xb0_0000, .end = (0xb0_0000) + (8) * 4096 }});
    try free_list.seedContiguousRange(0xb0_0000, 8);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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

test "free page diagnostics clip crossing ranges without consuming pages" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    const list = try std.testing.allocator.create(FreePageList);
    defer std.testing.allocator.destroy(list);
    list.* = .{};
    try list.installRegions(&.{ .{ .id = 2, .start = 0x0800_0000, .end = 0x0800_3000 }, .{ .id = 1, .start = 0xffff_e000, .end = 0x1_0000_2000 }, .{ .id = 0, .start = 0x1_0000_2000, .end = 0x1_0000_c000 } });
    try list.seedContiguousRange(0x1_0000_2000, 10);
    try list.seedContiguousRange(0xffff_e000, 4);
    try list.seedContiguousRange(0x0800_0000, 3);
    const available = list.availableBelow(0x1_0000_0000);
    try std.testing.expectEqual(@as(usize, 5), available.total);
    try std.testing.expectEqual(@as(usize, 3), available.largest_run);
    try std.testing.expectEqual(@as(usize, 17), list.pageCount());
    try std.testing.expectEqual(@as(usize, 3), list.rangeCount());
    try std.testing.expectEqual(@as(usize, 0), list.availableBelow(0).total);
}

test "native vma fault mapping resolves backing page without page capability" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x1_0000_0000, .end = (0x1_0000_0000) + (2) * 4096 }});
    try free_list.seedContiguousRange(0x1_0000_0000, 2);
    const page = try s.allocPhysicalPage(&free_list);
    const fd = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{ .map_read = true }), .{}, 0);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    try s.installNativeVmoPages(vmo, 0, &[_]u64{page.paddr}, null);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x800_0000, .end = (0x800_0000) + (8) * 4096 }});
    try free_list.seedContiguousRange(0x800_0000, 8);
    const original = try s.createNativeCowTable(3, &free_list);
    try s.retainNativeCowTable(original);
    try s.retainNativeCowTable(original);
    var pages: [3]u64 = undefined;
    for (&pages, 0..) |*page, i| {
        page.* = (try s.allocPhysicalPage(&free_list)).paddr;
        try s.setNativeCowPagePaddr(original, @intCast(i), page.*, null);
    }
    var slice = kernel.VmaEntry{
        .active = true,
        .start_va = 0x4600_1000,
        .size_bytes = 4096,
        .cow_table = original,
        .cow_page_offset = 1,
    };
    var copy_free = FreePageList{};
    try copy_free.installRegions(&.{.{ .id = 0, .start = 0x900_0000, .end = (0x900_0000) + (1) * 4096 }});
    try copy_free.seedContiguousRange(0x900_0000, 1);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0xa00_0000, .end = (0xa00_0000) + (8) * 4096 }});
    try free_list.seedContiguousRange(0xa00_0000, 8);
    const original = try s.createNativeCowTable(3, &free_list);
    try s.retainNativeCowTable(original);
    try s.retainNativeCowTable(original);
    for (0..3) |i| {
        const page = (try s.allocPhysicalPage(&free_list)).paddr;
        try s.setNativeCowPagePaddr(original, @intCast(i), page, null);
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
    try copy_free.installRegions(&.{.{ .id = 0, .start = 0xb00_0000, .end = (0xb00_0000) + (1) * 4096 }});
    try copy_free.seedContiguousRange(0xb00_0000, 1);
    try std.testing.expectError(KernelError.OutOfFreePages, s.detachSharedEntryCowTable(&slice, &copy_free));
    try expectResourceEqualDeep(before, slice);
    try std.testing.expectEqual(@as(usize, 1), copy_free.pageCount());
    try std.testing.expectEqual(store_before, kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expectEqual(@as(u32, 2), (s.nativeCowTableSlotConst(original) orelse unreachable).ref_count);
    s.releaseNativeCowTable(original, &free_list);
    s.releaseNativeCowTable(original, &free_list);
    try std.testing.expectEqual(@as(usize, 8), free_list.pageCount());
    try kernel.ReferenceCheck.check(&s);
}

test "read-only backing permits private COW but not shared or executable mappings" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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

test "private file VMO partial unmap returns replaced snapshot pages" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    const Cut = enum { head, tail, middle, split, fixed };
    for (std.meta.tags(Cut)) |cut| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const va: u64 = 0x46c0_0000;
        const pages = [_]u64{ 0x8900_0000, 0x8900_1000, 0x8900_2000, 0x8900_3000 };
        const rights = fdRights(.{ .map_read = true, .map_write = true, .close = true });
        const fd = try s.createAnonymousVmoFd(p0, 16384, rights, .{}, 0);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        try s.installNativeVmoPages(ref, 0, &pages, null);
        _ = try s.mmapFd(p0, fd, va, 16384, .{ .read = true, .write = true }, .{ .private = true }, 0);
        try s.closeFdWithFreeList(p0, fd, &free_list);
        if (cut == .split) try s.setVmaProtRange(p0, va + 4096, 4096, .{ .read = true });
        const page: usize = switch (cut) {
            .head => 0,
            .tail => 3,
            else => 1,
        };
        if (cut == .fixed) {
            const replacement = try s.createAnonymousVmoFd(p0, 4096, rights, .{}, 0);
            var prepared = try s.prepareFixedFdMmap(p0, replacement, va + page * 4096, 4096, .{ .read = true }, .{ .private = true, .fixed = true }, 0, &free_list);
            // Preparing a replacement must not retire live source backing.
            try std.testing.expectEqual(@as(?u64, pages[page]), s.nativeVmoPagePaddr(ref, page));
            s.commitFixedMmapPrepared(&prepared, &free_list);
            try s.closeFdWithFreeList(p0, replacement, &free_list);
        } else {
            var prepared = try s.prepareMunmapRangeWithFreeList(p0, va + page * 4096, 4096, &free_list);
            s.commitMunmapPrepared(&prepared, &free_list);
        }
        try std.testing.expectEqual(@as(usize, 1), free_list.pageCount());
        try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(ref, page));
        for (pages, 0..) |paddr, i| {
            if (i != page) try std.testing.expectEqual(@as(?u64, paddr), s.nativeVmoPagePaddr(ref, i));
        }
        try s.munmapRangeWithFreeList(p0, va, 16384, &free_list);
        try std.testing.expectEqual(@as(usize, 4), free_list.pageCount());
        try std.testing.expect(s.nativeVmoSlot(ref) == null);
    }
}

test "private file VMO partial unmap preserves FD view shadow and fork aliases" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    const Alias = enum { fd, view, shadow, shadow_tree, foreign, forked };
    for (std.meta.tags(Alias)) |alias| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const va: u64 = 0x46d0_0000;
        const pages = [_]u64{ 0x8a00_0000, 0x8a00_1000, 0x8a00_2000 };
        const rights = fdRights(.{ .map_read = true, .transfer = true, .close = true });
        const fd = try s.createAnonymousVmoFd(p0, 12288, rights, .{}, 0);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        try s.installNativeVmoPages(ref, 0, &pages, null);
        _ = try s.mmapFd(p0, fd, va, 12288, .{ .read = true }, .{ .private = true }, 0);
        var view: ?kernel.NativeVmoRef = null;
        if (alias == .view) {
            view = try s.createNativePageView(ref, 1);
            try s.installNativeVmoPages(view.?, 0, pages[1..2], null);
        }
        if (alias == .shadow or alias == .shadow_tree) {
            view = try s.createNativeVmo(.anonymous, 4096);
            try s.retainNativeVmo(view.?);
            try s.retainNativeVmo(ref);
            try s.publishNativeVmoParent(view.?, ref, 4096);
            if (alias == .shadow_tree) {
                const child = view.?;
                view = try s.createNativeVmo(.anonymous, 4096);
                try s.retainNativeVmo(view.?);
                try s.retainNativeVmo(child);
                try s.publishNativeVmoParent(view.?, child, 0);
                s.releaseNativeVmoWithFreeList(child, &free_list);
            }
        }
        if (alias == .foreign) {
            const other = try s.transferFd(p0, p1, fd, 16, rights, .{}, .copy);
            _ = try s.mmapFd(p1, other, va, 4096, .{ .read = true }, .{ .private = true }, 4096);
            try s.closeFdWithFreeList(p1, other, &free_list);
        }
        if (alias == .forked) try s.cloneVmaTableForFork(p0, p1);
        if (alias != .fd) try s.closeFdWithFreeList(p0, fd, &free_list);
        try s.munmapRangeWithFreeList(p0, va + 4096, 4096, &free_list);
        try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
        try std.testing.expectEqual(@as(?u64, pages[1]), s.nativeVmoPagePaddr(ref, 1));
        if (alias == .shadow or alias == .shadow_tree) try std.testing.expectEqual(@as(?u64, pages[1]), s.nativeVmoResolvedPagePaddr(view.?, 0));
        try s.munmapRangeWithFreeList(p0, va, 12288, &free_list);
        if (alias == .shadow or alias == .shadow_tree) try std.testing.expectEqual(@as(?u64, pages[1]), s.nativeVmoResolvedPagePaddr(view.?, 0));
        if (alias == .foreign or alias == .forked) try s.munmapRangeWithFreeList(p1, va, 12288, &free_list);
        if (view) |v| s.releaseNativeVmoWithFreeList(v, &free_list);
        if (alias == .fd) try s.closeFdWithFreeList(p0, fd, &free_list);
        try std.testing.expectEqual(@as(usize, 3), free_list.pageCount());
        try std.testing.expect(s.nativeVmoSlot(ref) == null);
    }
}

test "file VMO same FD replacement and shared partial unmap preserve backing" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]bool{ false, true }) |shared| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const va: u64 = 0x46e0_0000;
        const pages = [_]u64{ 0x8b00_0000, 0x8b00_1000, 0x8b00_2000, 0x8b00_3000 };
        const fd = try s.createAnonymousVmoFd(p0, 16384, fdRights(.{ .map_read = true, .close = true }), .{}, 0);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        try s.installNativeVmoPages(ref, 0, &pages, null);
        _ = try s.mmapFd(p0, fd, va, 16384, .{ .read = true }, .{ .private = !shared, .shared = shared }, 0);
        if (shared) {
            try s.closeFdWithFreeList(p0, fd, &free_list);
            try s.munmapRangeWithFreeList(p0, va + 4096, 4096, &free_list);
        } else {
            var prepared = try s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, .{ .read = true }, .{ .fixed = true, .private = true }, 4096, &free_list);
            s.commitFixedMmapPrepared(&prepared, &free_list);
            try s.closeFdWithFreeList(p0, fd, &free_list);
            try std.testing.expectEqual(pages[1], s.nativeVmaFaultMapping(p0, va + 4096, false, false).?.paddr);
        }
        try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
        for (pages, 0..) |paddr, i| try std.testing.expectEqual(@as(?u64, paddr), s.nativeVmoPagePaddr(ref, i));
        try s.munmapRangeWithFreeList(p0, va, 16384, &free_list);
        try std.testing.expectEqual(@as(usize, 4), free_list.pageCount());
    }
}

test "private file vma faults read-only then COWs on write" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x1_0000_0000, .end = (0x1_0000_0000) + (4) * 4096 }});
    try free_list.seedContiguousRange(0x1_0000_0000, 4);
    const source_page = (try s.allocPhysicalPage(&free_list)).paddr;

    const fd = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{
        .map_read = true,
    }), .{}, 0);
    const source_vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    try s.installNativeVmoPages(source_vmo, 0, &[_]u64{source_page}, null);

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

    const losing_plan = s.prepareNativeVmaCowMapping(p0, 0x4600_0000, true, false);
    const losing_page = (try s.allocPhysicalPage(&free_list)).paddr;
    const write_mapping = s.ensureNativeVmaCowMappingLockedSlow(
        p0,
        0x4600_0000,
        true,
        false,
        &free_list,
    ) orelse unreachable;
    // The winner changed the COW identity while both plans were private.
    // The caller returns its unused PFN; the loser must publish no second edge.
    try std.testing.expect(s.commitNativeVmaCowMapping(p0, losing_plan, losing_page, &free_list) == null);
    try free_list.appendPage(losing_page);
    try kernel.ReferenceCheck.check(&s);
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
    try kernel.ReferenceCheck.check(&s);
}

fn fillVmaTableExcept(s: *KernelState, table: *kernel.VmaTable, except: []const usize) void {
    var index: usize = 0;
    while (index < former_max_vmas_per_process) : (index += 1) {
        var skip = false;
        for (except) |except_index| {
            if (index == except_index) {
                skip = true;
                break;
            }
        }
        if (skip or table.entries.get(@intCast(index)) != null) continue;
        table.reference_slots.prepareIndex(@intCast(index), table.allocator.?) catch unreachable;
        table.entries.prepareIndex(@intCast(index), table.allocator.?) catch unreachable;
        table.active_indices.prepareIndex(@intCast(table.active_count), table.allocator.?) catch unreachable;
        s.installVmaEntry(table, index, .{
            .active = true,
            .start_va = 0x7000_0000 + @as(u64, @intCast(index)) * 0x2000,
            .size_bytes = 4096,
        });
    }
}

test "fixed mmap prepare reuses a fully covered slot in a full vma table" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    const target_index: usize = @intCast(table.active_indices.get(@intCast(0)).?.*);
    fillVmaTableExcept(&s, table, &.{});
    try std.testing.expectEqual(former_max_vmas_per_process, table.active_count);

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
    try std.testing.expectEqual(former_max_vmas_per_process, table.active_count);
    try std.testing.expectEqual(dest_vmo, (s.vmaEntryConst(p0, target_va) orelse unreachable).vmo);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(source_vmo));
}

test "fixed mmap middle cut fails atomically with only one spare vma slot" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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
    const source_index: usize = @intCast(table.active_indices.get(@intCast(0)).?.*);
    const spare_index: usize = if (source_index == 0) 1 else 0;
    fillVmaTableExcept(&s, table, &.{ source_index, spare_index });
    try std.testing.expectEqual(former_max_vmas_per_process - 1, table.active_count);
    const original = table.entries.get(@intCast(source_index)).?.*;
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
    var failure = kernel.InvocationStorageTestAllocator{ .fail_at = 1 };
    defer failure.deinit();
    const saved_allocator = table.allocator;
    table.allocator = failure.interface();
    defer table.allocator = saved_allocator;
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
    try std.testing.expectEqual(original, table.entries.get(@intCast(source_index)).?.*);
    try std.testing.expect(table.entries.get(@intCast(spare_index)) == null);
    try std.testing.expectEqual(source_refs, s.nativeVmoRefCount(source_vmo));
    try std.testing.expectEqual(dest_refs, s.nativeVmoRefCount(dest_vmo));
}

test "shared fixed mmap retains aliases offsets and neighbors through fd close" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]kernel.MmapFlags{ .{ .fixed = true, .shared = true }, .{ .fixed = true } }) |flags| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
        const rw = vmaProt(.{ .read = true, .write = true });
        const va: u64 = 0x5500_0000;
        const fd = try s.createAnonymousVmoFd(p0, 3 * 4096, fdRights(.{ .map_read = true, .map_write = true, .close = true }), .{}, 16);
        const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
        try s.installNativeVmoPages(vmo, 0, &.{ 0x1000, 0x2000, 0x3000 }, null);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const rw = vmaProt(.{ .read = true, .write = true });
    const va: u64 = 0x5700_0000;
    const fd = try s.createAnonymousVmoFd(p0, 8192, fdRights(.{ .map_read = true, .map_write = true, .close = true }), .{}, 16);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    try s.installNativeVmoPages(vmo, 0, &.{ 0x1000, 0x2000 }, null);
    _ = try s.mmapFdIntoProcess(p0, fd, p1, va, 8192, rw, .{ .shared = true }, 0);
    _ = try s.mmapFd(p0, fd, va, 8192, rw, .{ .shared = true }, 0);
    try std.testing.expect(s.fdEntryConst(p1, fd) == null);
    const before = (s.vmaEntryConst(p1, va) orelse unreachable).*;
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmapIntoProcess(p1, fd, p1, va, 4096, rw, .{ .shared = true }, 4096, &free_list));
    try expectResourceEqualDeep(before, (s.vmaEntryConst(p1, va) orelse unreachable).*);
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
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    const va: u64 = 0x5600_0000;
    const rw = vmaProt(.{ .read = true, .write = true });
    const old = try s.createAnonymousVmaWithPages(p0, va, 3 * 4096, rw, rw, .{ .anonymous = true, .private = true }, &free_list);
    const original = (s.vmaEntryConst(p0, va) orelse unreachable).*;
    const fd = try s.createAnonymousVmoFd(p0, 3 * 4096, fdRights(.{ .map_read = true, .map_write = true }), .{}, 16);
    const vmo = s.nativeVmoRefForFd(p0, fd) orelse unreachable;
    const flags = mmapFlags(.{ .fixed = true, .shared = true });
    // Missing backing, out-of-range offsets, denied execute and malformed flags.
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, rw, flags, 0, &free_list));
    try s.installNativeVmoPages(vmo, 0, &.{ 0x1000, 0x2000, 0x3000 }, null);
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, rw, flags, 3 * 4096, &free_list));
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, .{ .read = true, .exec = true }, flags, 0, &free_list));
    try std.testing.expectError(KernelError.InvalidState, s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, rw, .{ .private = true, .shared = true }, 0, &free_list));
    try std.testing.expectEqual(original, (s.vmaEntryConst(p0, va) orelse unreachable).*);
    const table = s.getVmaTable(p0) orelse unreachable;
    fillVmaTableExcept(&s, table, &.{1});
    var failure = kernel.InvocationStorageTestAllocator{ .fail_at = 1 };
    defer failure.deinit();
    const saved_allocator = table.allocator;
    table.allocator = failure.interface();
    defer table.allocator = saved_allocator;
    try std.testing.expectError(KernelError.TableFull, s.prepareFixedFdMmap(p0, fd, va + 4096, 4096, rw, flags, 4096, &free_list));
    try std.testing.expectEqual(original, (s.vmaEntryConst(p0, va) orelse unreachable).*);
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(old));
    try std.testing.expectEqual(@as(?u32, 1), s.nativeVmoRefCount(vmo));
}

test "fixed mmap validation and discard preserve the old mapping" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
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

fn notifyTestHandle(state: *KernelState, owner: PrincipalId, fd: kernel.Fd) kernel.IpcChannelHandle {
    return state.fdPayloadWithRightsConst(owner, fd, .{}).?.payload.channel;
}

fn notifyTestChannel(state: *KernelState) !struct { a_owner: PrincipalId, b_owner: PrincipalId, a: kernel.Fd, b: kernel.Fd } {
    const a_owner = kernel.processPrincipalFromIndex(0).?;
    const b_owner = kernel.processPrincipalFromIndex(1).?;
    const rights = kernel.FdRights{ .inspect = true, .transfer = true, .wait = true, .poll = true, .close = true, .send = true, .recv = true };
    const pair = try state.createIpcChannelPairFds(a_owner, rights, .{}, 16);
    const b = try state.transferFd(a_owner, b_owner, pair.b, 16, rights, .{}, .move);
    return .{ .a_owner = a_owner, .b_owner = b_owner, .a = pair.a, .b = b };
}

test "ipc notify latch is binary, readable, and drained before peer death" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var state: KernelState = undefined;
    try initFdState(&state);
    const ch = try notifyTestChannel(&state);
    try std.testing.expectEqual(kernel.IpcNotifyConsumeResult.empty, try state.consumeIpcNotifyForFd(ch.b_owner, ch.b, true));
    try std.testing.expectEqual(@as(u64, 0), state.fdPollEvents(ch.b_owner, ch.b, fd_abi.event_readable, 0).?);
    // Two notifications merge into one latch: the ring, not a count, says how much work exists.
    _ = try state.ipcNotifyPeerForFd(ch.a_owner, ch.a);
    _ = try state.ipcNotifyPeerForFd(ch.a_owner, ch.a);
    try std.testing.expectEqual(fd_abi.event_readable, state.fdPollEvents(ch.b_owner, ch.b, fd_abi.event_readable, 0).?);
    // The sender's own side stays quiet.
    try std.testing.expectEqual(@as(u64, 0), state.fdPollEvents(ch.a_owner, ch.a, fd_abi.event_readable, 0).?);
    try std.testing.expectEqual(kernel.IpcNotifyConsumeResult.consumed, try state.consumeIpcNotifyForFd(ch.b_owner, ch.b, true));
    try std.testing.expectEqual(kernel.IpcNotifyConsumeResult.empty, try state.consumeIpcNotifyForFd(ch.b_owner, ch.b, true));
    // Messages and the latch are independent.
    try std.testing.expect(state.ipcChannelSlotConst(notifyTestHandle(&state, ch.b_owner, ch.b).channel).?.queues[1].isEmpty());

    _ = try state.ipcNotifyPeerForFd(ch.a_owner, ch.a);
    try state.closeFd(ch.a_owner, ch.a);
    try std.testing.expectEqual(kernel.IpcNotifyConsumeResult.consumed, try state.consumeIpcNotifyForFd(ch.b_owner, ch.b, false));
    try std.testing.expectEqual(kernel.IpcNotifyConsumeResult.closed, try state.consumeIpcNotifyForFd(ch.b_owner, ch.b, false));
    // A notification into a side nobody can read is an error, not a latch.
    try std.testing.expect((try state.ipcNotifyPeerForFd(ch.b_owner, ch.b)).closed);
}

test "ipc notify rights follow send, recv and wait" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var state: KernelState = undefined;
    try initFdState(&state);
    const owner = kernel.processPrincipalFromIndex(0).?;
    const pair = try state.createIpcChannelPairFds(owner, .{ .inspect = true, .close = true, .recv = true }, .{}, 16);
    try std.testing.expectError(KernelError.InvalidState, state.ipcNotifyPeerForFd(owner, pair.a));
    try std.testing.expectEqual(kernel.IpcNotifyConsumeResult.empty, try state.consumeIpcNotifyForFd(owner, pair.a, false));
    try std.testing.expectError(KernelError.InvalidState, state.consumeIpcNotifyForFd(owner, pair.a, true));
    try std.testing.expectError(KernelError.InvalidState, state.registerIpcNotifyWaiterForFd(owner, pair.a, 1, 1, 9));
}

test "ipc notify detaches the peer waiter but leaves the latch until publication" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var state: KernelState = undefined;
    try initFdState(&state);
    const ch = try notifyTestChannel(&state);
    try state.registerIpcNotifyWaiterForFd(ch.b_owner, ch.b, 3, 5, 77);
    const result = try state.ipcNotifyPeerForFd(ch.a_owner, ch.a);
    const target = result.waiter.?;
    try std.testing.expectEqual(ch.b_owner, target.owner);
    try std.testing.expectEqual(@as(usize, 3), target.thread_index);
    try std.testing.expectEqual(@as(u64, 77), target.wait_token);
    // Detached at claim time: a second waiter is not refused by a published one.
    try std.testing.expect(state.ipcNotifyWaiterForFd(ch.b_owner, ch.b) == null);
    // Until the completion is published the latch is kept, so a waiter that
    // vanished before its claim cannot swallow the notification.
    try std.testing.expectEqual(kernel.IpcNotifyConsumeResult.consumed, try state.consumeIpcNotifyForFd(ch.b_owner, ch.b, false));
    try state.registerIpcNotifyWaiterForFd(ch.b_owner, ch.b, 3, 5, 78);
    // Every completed outcome owns the notification and consumes the latch.
    for ([_]kernel.IpcNotifyCompletion{ .handed_off, .published, .pending }) |completion| {
        try state.registerIpcNotifyWaiterForFd(ch.b_owner, ch.b, 3, 5, 78);
        _ = try state.ipcNotifyPeerForFd(ch.a_owner, ch.a);
        state.settleIpcPeerNotifyForFd(ch.a_owner, ch.a, completion);
        try std.testing.expectEqual(kernel.IpcNotifyConsumeResult.empty, try state.consumeIpcNotifyForFd(ch.b_owner, ch.b, false));
    }
    // A stale waiter never saw it: the next NOTIFY_WAIT still finds it.
    try state.registerIpcNotifyWaiterForFd(ch.b_owner, ch.b, 3, 5, 78);
    _ = try state.ipcNotifyPeerForFd(ch.a_owner, ch.a);
    state.settleIpcPeerNotifyForFd(ch.a_owner, ch.a, .stale);
    try std.testing.expectEqual(kernel.IpcNotifyConsumeResult.consumed, try state.consumeIpcNotifyForFd(ch.b_owner, ch.b, false));
    // Unregister removes only the exact registration.
    try state.registerIpcNotifyWaiterForFd(ch.b_owner, ch.b, 3, 5, 79);
    state.unregisterIpcNotifyWaiterForFd(ch.b_owner, ch.b, 3, 5, 78);
    try std.testing.expect(state.ipcNotifyWaiterForFd(ch.b_owner, ch.b) != null);
    state.unregisterIpcNotifyWaiterForFd(ch.b_owner, ch.b, 3, 5, 79);
    try std.testing.expect(state.ipcNotifyWaiterForFd(ch.b_owner, ch.b) == null);
}

test "ipc notify waiters on both sides complete when the peer closes or the slot is released" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var state: KernelState = undefined;
    try initFdState(&state);
    const owner = kernel.processPrincipalFromIndex(0).?;
    const rights = kernel.FdRights{ .inspect = true, .wait = true, .poll = true, .close = true, .send = true, .recv = true };
    const pair = try state.createIpcChannelPairFds(owner, rights, .{}, 16);
    const handle = notifyTestHandle(&state, owner, pair.a);
    try state.registerIpcNotifyWaiterForFd(owner, pair.a, 1, 1, 11);
    try state.registerIpcNotifyWaiterForFd(owner, pair.b, 2, 1, 12);
    var out: [2]kernel.ThreadWakeTarget = undefined;
    const snapshot = state.ipcChannelNotifyWaiterSnapshot(handle);
    // A sibling thread closes the side another thread is parked on.
    try state.closeFd(owner, pair.a);
    try std.testing.expectEqual(@as(usize, 2), state.takeIpcChannelNotifyWaitersAfterClose(handle, snapshot, &out));
    try std.testing.expectEqual(@as(u64, 11), out[0].wait_token);
    try std.testing.expectEqual(@as(u64, 12), out[1].wait_token);
    // Detached once: a second close-side pass finds nothing left to complete.
    try std.testing.expectEqual(@as(usize, 0), state.takeIpcChannelNotifyWaitersAfterClose(handle, snapshot, &out));

    // Final release resets the slot; only the pre-close snapshot still names the waiter.
    try state.registerIpcNotifyWaiterForFd(owner, pair.b, 2, 1, 13);
    const last = state.ipcChannelNotifyWaiterSnapshot(handle);
    try state.closeFd(owner, pair.b);
    try std.testing.expect(state.ipcChannelSlotConst(handle.channel) == null);
    try std.testing.expectEqual(@as(usize, 1), state.takeIpcChannelNotifyWaitersAfterClose(handle, last, &out));
    try std.testing.expectEqual(@as(u64, 13), out[0].wait_token);
}

test "ipc notify poll take leaves message receive waiters armed" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var state: KernelState = undefined;
    try initFdState(&state);
    var list = kernel.IpcWaitList{ .allocator = try state.poolAllocator() };
    defer list.waiters.deinit();
    const owner = kernel.processPrincipalFromIndex(0).?;
    const key = kernel.IpcWaitKey{ .kind = .channel, .index = 1, .generation = 1, .side = 0 };
    _ = try list.register(key, owner, 0, .{ .active = true, .array = 0x1000 }, 20, 1, fd_abi.event_readable, 0, 1, 1, 5, .{ .index = 1, .generation = 1 });
    _ = try list.register(key, owner, 0x2000, .{}, 20, 0, fd_abi.event_readable, 0, 2, 1, 6, .{ .index = 2, .generation = 1 });
    var out: [4]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 1), list.takeReadablePollWaiters(out[0..]));
    try std.testing.expectEqual(@as(u64, 6), out[0].wait_token);
    // The RECV_WAIT registration is still claimable by a real message.
    try std.testing.expectEqual(@as(usize, 1), list.takeReadable(out[0..]));
    try std.testing.expectEqual(@as(u64, 5), out[0].wait_token);
}

// Models the lock release: republish only when dirty, and otherwise require
// that the published value already equals a full rescan.
fn publishTimerDeadline(s: *KernelState, published: *u64) !void {
    if (s.takeTimerDeadlineDirty()) {
        published.* = s.nextTimerWaiterDeadline() orelse 0;
    } else {
        try std.testing.expectEqual(published.*, s.nextTimerWaiterDeadline() orelse 0);
    }
}

test "timer deadline is republished exactly when its inputs change" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    // Zero-filled storage must still force the first publication.
    try std.testing.expect(s.takeTimerDeadlineDirty());
    try std.testing.expect(!s.takeTimerDeadlineDirty());
    var published: u64 = 0;
    const rights = fdRights(.{ .wait = true, .poll = true, .read = true, .write = true, .close = true, .send = true, .recv = true });
    // IPC doorbells, the hot FS path, leave the timer scan untouched.
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    _ = try s.ipcNotifyPeerForFd(p0, pair.a);
    _ = try s.consumeIpcNotifyForFd(p0, pair.b, false);
    try std.testing.expect(!s.takeTimerDeadlineDirty());

    const timer = try s.createTimerFd(p0, 100, 0, .{}, rights, 16);
    try std.testing.expect(s.takeTimerDeadlineDirty());
    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, timer, fd_abi.event_readable, 0x1000, 7, 11, 1, group));
    try publishTimerDeadline(&s, &published);
    try std.testing.expectEqual(@as(u64, 100), published);
    try publishTimerDeadline(&s, &published);
    // An earlier deadline must be published at once.
    try s.setTimerFd(p0, timer, 40, 0, 0);
    try publishTimerDeadline(&s, &published);
    try std.testing.expectEqual(@as(u64, 40), published);
    // A taken (pending) waiter leaves the scan.
    var targets: [2]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 1), s.takeReadyTimerWaiters(50, &targets));
    try publishTimerDeadline(&s, &published);
    try std.testing.expectEqual(@as(u64, 0), published);
    s.cancelFdWaitGroup(group, 1);
    try publishTimerDeadline(&s, &published);
    // Re-registering, then destroying the timer while its waiter remains.
    try s.setTimerFd(p0, timer, 70, 0, 0);
    const again = try s.beginFdWaitGroup(p0, 7, 11, 2);
    try std.testing.expect(try s.registerTaskReadableWaiterForFd(p0, timer, fd_abi.event_readable, 0x2000, 7, 11, 2, again));
    try publishTimerDeadline(&s, &published);
    try std.testing.expectEqual(@as(u64, 70), published);
    try s.closeFd(p0, timer);
    try publishTimerDeadline(&s, &published);
    try std.testing.expectEqual(@as(u64, 0), published);
    s.cancelFdWaitGroupsForOwner(p0);
    try publishTimerDeadline(&s, &published);
}

test "thread terminal objects survive principal and thread slot reuse" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const rights = fdRights(.{ .wait = true, .close = true });
    const old = try s.createThreadFd(p0, .{
        .owner_principal_raw = @intFromEnum(p1),
        .thread_index = 3,
        .thread_generation = 10,
        .state = .active,
    }, rights, .{}, 16);
    s.markThreadObjectsExitedBySlot(3, 10, .exited, 23);
    try std.testing.expect(s.removeProcessDescriptor(p1));
    try std.testing.expect(s.ensureProcessDescriptor(p1, "reused-owner"));
    const fresh = try s.createThreadFd(p0, .{
        .owner_principal_raw = @intFromEnum(p1),
        .thread_index = 3,
        .thread_generation = 11,
        .state = .active,
    }, rights, .{}, 16);
    s.markThreadObjectsExitedForPrincipal(p1, .stopped, 4);
    try std.testing.expectEqual(kernel.TaskObjectState.stopped, s.threadObjectForFd(p0, fresh, rights).?.state);
    s.markThreadObjectsExitedForPrincipal(p1, .continued, 5);
    try std.testing.expectEqual(kernel.TaskObjectState.continued, s.threadObjectForFd(p0, fresh, rights).?.state);
    s.markThreadObjectsExitedForPrincipal(p1, .killed, 99);
    s.markThreadObjectsExitedBySlot(3, 10, .killed, 100);
    const previous = s.threadObjectForFd(p0, old, rights).?;
    const latest = s.threadObjectForFd(p0, fresh, rights).?;
    try std.testing.expectEqual(kernel.TaskObjectState.exited, previous.state);
    try std.testing.expectEqual(@as(u32, 23), previous.exit_code);
    try std.testing.expectEqual(kernel.TaskObjectState.killed, latest.state);
    try std.testing.expectEqual(@as(u32, 99), latest.exit_code);
}

test "process reuse resets stable VMA and FD bodies after prepared loan ends" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const generation = s.processDescriptor(p0).?.generation;
    const fd = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{ .map_read = true }), .{}, 0);
    _ = try s.mmapFd(p0, fd, 0x4300_0000, 4096, vmaProt(.{ .read = true }), .{}, 0);
    const body_address = @intFromPtr(s.getVmaTable(p0).?);
    const fd_address = @intFromPtr(s.getFdTable(p0).?);
    var free_list = FreePageList{};
    {
        // Runtime callers retain K+A/S for this entire prepared loan. The
        // source guard separately rejects moving reset/drop into this interval.
        var prepared = try s.prepareMunmapRangeWithFreeList(p0, 0x4300_0000, 4096, &free_list);
        defer s.discardMunmapPrepared(&prepared, &free_list);
        s.commitMunmapPrepared(&prepared, &free_list);
    }
    s.resetProcessRuntimeTables(0);
    try std.testing.expect(s.removeProcessDescriptor(p0));
    try std.testing.expectEqual(p0, s.createProcessDescriptor("reused VMA body").?);
    try std.testing.expect(s.processDescriptor(p0).?.generation != generation);
    try std.testing.expectEqual(body_address, @intFromPtr(s.getVmaTable(p0).?));
    try std.testing.expectEqual(fd_address, @intFromPtr(s.getFdTable(p0).?));
    try std.testing.expectEqual(@as(usize, 0), s.getVmaTable(p0).?.active_count);
    try std.testing.expect(s.fdEntryConst(p0, fd) == null);
    try std.testing.expect(s.vmaEntryConst(p0, 0x4300_0000) == null);
}

test "invocation VMO workspace exact create and grow sizes and maximum backing" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]usize{ 511, 512, 513, 131072 }) |count| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var backend = kernel.InvocationStorageTestAllocator{};
        defer backend.deinit();
        s.invocation_test_allocator = backend.interface();
        var free_list = FreePageList{};
        const high: u64 = 20 << 30;
        try free_list.installRegions(&.{.{ .id = 0, .start = high, .end = high + @as(u64, count) * 4096 }});
        try free_list.seedContiguousRange(high, count);
        const fd = try s.createAnonymousVmoFdWithPages(p0, (count - 1) * 4096, fdRights(.{ .resize = true, .close = true }), .{}, 16, &free_list);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        try std.testing.expectEqual(@as(usize, 1), backend.calls);
        try std.testing.expectEqual(try kernel.InvocationStorageTestAllocator.allocationPages(u64, count - 1), backend.last_pages);
        try std.testing.expectEqual(@as(usize, 0), backend.active());
        try s.growVmoFdWithPages(p0, fd, count * 4096, &free_list);
        try std.testing.expectEqual(@as(usize, 2), backend.calls);
        try std.testing.expectEqual(try kernel.InvocationStorageTestAllocator.allocationPages(u64, 1), backend.last_pages);
        for (0..count) |i| try std.testing.expectEqual(@as(?u64, high + i * 4096), s.nativeVmoPagePaddr(ref, i));
        try std.testing.expectEqual(@as(usize, 0), backend.active());
        try s.closeFdWithFreeList(p0, fd, &free_list);
        try std.testing.expectEqual(count, free_list.pageCount());
        const full = try s.createAnonymousVmoFdWithPages(p0, count * 4096, fdRights(.{ .close = true }), .{}, 16, &free_list);
        try std.testing.expectEqual(try kernel.InvocationStorageTestAllocator.allocationPages(u64, count), backend.last_pages);
        try s.closeFdWithFreeList(p0, full, &free_list);
        const growing = try s.createAnonymousVmoFdWithPages(p0, 4096, fdRights(.{ .resize = true, .close = true }), .{}, 16, &free_list);
        try s.growVmoFdWithPages(p0, growing, count * 4096, &free_list);
        try std.testing.expectEqual(try kernel.InvocationStorageTestAllocator.allocationPages(u64, count - 1), backend.last_pages);
        try s.closeFdWithFreeList(p0, growing, &free_list);
        try std.testing.expectEqual(count, free_list.pageCount());
        // Beyond the former size limit, exhaustion is physical, not admission.
        try std.testing.expectError(KernelError.OutOfFreePages, s.createAnonymousVmoFdWithPages(p0, 131073 * 4096, fdRights(.{ .close = true }), .{}, 16, &free_list));
    }
}

var invocation_zero_calls: usize = 0;
var invocation_zero_fail_at: usize = 0;
fn invocationZero(_: u64) bool {
    invocation_zero_calls += 1;
    return invocation_zero_calls != invocation_zero_fail_at;
}

test "invocation VMO workspace allocation zeroing and return failures preserve old backing" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var backend = kernel.InvocationStorageTestAllocator{};
    defer backend.deinit();
    s.invocation_test_allocator = backend.interface();
    var free_list = FreePageList{};
    const high: u64 = 20 << 30;
    try free_list.installRegions(&.{.{ .id = 0, .start = high, .end = high + 8 * 4096 }});
    try free_list.seedContiguousRange(high, 8);
    const rights = fdRights(.{ .resize = true, .close = true });
    backend.fail_at = 1;
    try std.testing.expectError(KernelError.OutOfFreePages, s.createAnonymousVmoFdWithPages(p0, 4096, rights, .{}, 16, &free_list));
    try std.testing.expectEqual(@as(usize, 8), free_list.pageCount());
    backend.fail_at = 0;
    const fd = try s.createAnonymousVmoFdWithPages(p0, 4096, rights, .{}, 16, &free_list);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const old = s.nativeVmoSlot(ref).?.*;
    backend.fail_at = backend.calls + 1;
    try std.testing.expectError(KernelError.OutOfFreePages, s.growVmoFdWithPages(p0, fd, 4 * 4096, &free_list));
    try expectVmoSlotEqual(old, s.nativeVmoSlot(ref).?.*);
    try std.testing.expectEqual(@as(usize, 7), free_list.pageCount());
    backend.fail_at = 0;
    for ([_]usize{ 1, 2, 3 }) |failure| {
        invocation_zero_calls = 0;
        invocation_zero_fail_at = failure;
        s.zero_physical_page_hook = invocationZero;
        backend.fail_return = true;
        try std.testing.expectError(KernelError.InvalidState, s.growVmoFdWithPages(p0, fd, 4 * 4096, &free_list));
        try expectVmoSlotEqual(old, s.nativeVmoSlot(ref).?.*);
        try std.testing.expectEqual(@as(usize, 7), free_list.pageCount());
        try std.testing.expect(s.descriptor_storage.pending != null);
        const attempts = backend.returns;
        s.retryInvocationStorage();
        try std.testing.expectEqual(attempts + 1, backend.returns);
        backend.fail_return = false;
        s.retryInvocationStorage();
        try std.testing.expect(s.descriptor_storage.pending == null);
    }
    s.zero_physical_page_hook = null;
    backend.fail_return = true;
    try s.growVmoFdWithPages(p0, fd, 4 * 4096, &free_list);
    try std.testing.expectEqual(@as(?u64, 4 * 4096), s.nativeVmoSize(ref));
    try std.testing.expect(s.descriptor_storage.pending != null);
    backend.fail_return = false;
    s.retryInvocationStorage();
    try std.testing.expectEqual(@as(usize, 0), backend.active());
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(@as(usize, 8), free_list.pageCount());
}

test "invocation workspace uses identity grant and returns its metadata independently" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    s.invocation_test_allocator = null;
    var free_list = FreePageList{};
    const low: u64 = @intFromPtr(&fd_capacity_backing);
    const high: u64 = 20 << 30;
    try free_list.installRegions(&.{ .{ .id = 0, .start = low, .end = low + 8 * 4096 }, .{ .id = 1, .start = high, .end = high + 513 * 4096 } });
    try free_list.seedContiguousRange(low, 8);
    try free_list.seedContiguousRange(high, 513);
    const fd = try s.createAnonymousVmoFdWithPages(p0, 513 * 4096 - 1, fdRights(.{ .close = true }), .{}, 16, &free_list);
    try std.testing.expectEqual(@as(usize, 8), free_list.pageCount());
    try std.testing.expect(s.descriptor_storage.pending == null);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try std.testing.expectEqual(@as(usize, 521), free_list.pageCount());
}

test "contiguous DMA arithmetic insertion zero failures and maximum" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for ([_]usize{ 511, 512, 513, 16384 }) |count| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        const low: u64 = 0x200000;
        try free_list.installRegions(&.{.{ .id = 0, .start = low, .end = low + count * 4096 }});
        try free_list.seedContiguousRange(low, count);
        const rights = fdRights(.{ .close = true });
        s.zero_physical_page_hook = invocationZero;
        invocation_zero_calls = 0;
        invocation_zero_fail_at = count;
        try std.testing.expectError(KernelError.InvalidState, s.createContiguousVmoFdWithPages(p0, count * 4096, rights, .{}, 16, &free_list));
        try std.testing.expectEqual(count, free_list.pageCount());
        try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
        invocation_zero_calls = 0;
        invocation_zero_fail_at = 0;
        const fd = try s.createContiguousVmoFdWithPages(p0, count * 4096 - 1, rights, .{}, 16, &free_list);
        try std.testing.expectEqual(count, invocation_zero_calls);
        const ref = s.nativeVmoRefForFd(p0, fd).?;
        for (0..count) |i| try std.testing.expectEqual(@as(?u64, low + i * 4096), s.nativeVmoPagePaddr(ref, i));
        try s.closeFdWithFreeList(p0, fd, &free_list);
        try std.testing.expectEqual(count, free_list.pageCount());
    }
}

test "contiguous DMA backing capacity failure returns whole staged grant" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer if (!kernel.initRuntimeStorage(runtime_storage[0..])) unreachable;
    var free_list = FreePageList{};
    try free_list.installRegions(&.{.{ .id = 0, .start = 0x200000, .end = 0x204000 }});
    try free_list.seedContiguousRange(0x200000, 4);
    kernel.useBackingPmmForTest(&free_list);
    try std.testing.expectError(KernelError.OutOfFreePages, s.createContiguousVmoFdWithPages(p0, 4 * 4096, fdRights(.{ .close = true }), .{}, 16, &free_list));
    try std.testing.expectEqual(@as(usize, 4), free_list.pageCount());
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
    try std.testing.expect(s.fdEntryConst(p0, 16) == null);
}

test "invocation identity workspace rejected PMM return survives invocation end" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    s.invocation_test_allocator = null;
    var free_list = FreePageList{};
    const low: u64 = @intFromPtr(&fd_capacity_backing);
    try free_list.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
    try free_list.seedContiguousRange(low, 2);
    var hold: ?DmaHold = null;
    defer if (hold) |*h| h.end();
    {
        var scratch = s.beginInvocationStorage(&free_list);
        _ = try scratch.reserve(u64, 513, .replaceable);
        try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
        hold = try DmaHold.init(low, 2);
        try scratch.rollback();
    }
    try std.testing.expect(s.descriptor_storage.pending != null);
    s.retryInvocationStorage();
    try std.testing.expect(s.descriptor_storage.pending != null);
    hold.?.end();
    s.retryInvocationStorage();
    try std.testing.expect(s.descriptor_storage.pending == null);
    try std.testing.expectEqual(@as(usize, 2), free_list.pageCount());
    try std.testing.expectEqual(low, try free_list.popContiguousBelow(2, KernelState.low_memory_limit));
}

fn markedCount(table: anytype, mask: u8) usize {
    var iterator = table.marked(mask);
    var count: usize = 0;
    while (iterator.next() != null) count += 1;
    return count;
}

fn groupCount(s: *const KernelState, thread_index: u32) u64 {
    return if (s.fd_wait_group_thread_counts.get(thread_index)) |count| count.* else 0;
}

test "every runtime resource pool crosses its former bound and reuses generations" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    inline for (.{
        .{ "fd_objects", former_max_fd_objects },
        .{ "pipes", former_max_pipes },
        .{ "native_vmos", former_max_native_vmos },
        .{ "ipc_endpoints", former_max_ipc_endpoints },
        .{ "ipc_channels", former_max_ipc_channels },
        .{ "ipc_replies", former_max_ipc_replies },
        .{ "task_fd_waiters", former_max_task_fd_waiters },
        .{ "fd_wait_groups", former_max_fd_wait_groups },
    }) |case| {
        const table = &@field(s, case[0]);
        const first = try s.allocatePool(table, null, std.math.maxInt(u32));
        for (0..case[1] + 1) |_| _ = try s.allocatePool(table, null, std.math.maxInt(u32));
        try std.testing.expectEqual(first.value, table.resolve(first.ref).?);
        try std.testing.expect(table.live > case[1]);
        table.remove(first.value, null);
        try std.testing.expect(table.resolve(first.ref) == null);
        const reused = try s.allocatePool(table, null, std.math.maxInt(u32));
        try std.testing.expectEqual(first.ref.index, reused.ref.index);
        try std.testing.expect(first.ref.generation != reused.ref.generation);
    }
}

test "runtime pool allocation failure publishes no resource" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var backend = kernel.InvocationStorageTestAllocator{ .fail_at = 1 };
    defer backend.deinit();
    s.pool_allocator = backend.interface();
    try std.testing.expectError(KernelError.OutOfFreePages, s.createKernelObject(.event, .{ .event = 1 }));
    try std.testing.expectEqual(@as(usize, 0), s.fd_objects.live);
    try std.testing.expectEqual(@as(usize, 0), backend.active());
}

test "VMA entries and active indices grow past 8192 without moving a live entry" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    const table = s.getVmaTable(p0).?;
    defer table.reference_slots.deinit();
    defer table.entries.deinit();
    defer table.active_indices.deinit();
    fillVmaTableExcept(&s, table, &.{});
    const before = table.entries.get(0).?;
    const index = KernelState.findFreeVma(table).?;
    try std.testing.expect(index >= former_max_vmas_per_process);
    s.installVmaEntry(table, index, .{ .active = true, .start_va = 0x9000_0000, .size_bytes = 4096 });
    try std.testing.expectEqual(former_max_vmas_per_process + 1, table.active_count);
    try std.testing.expectEqual(before, table.entries.get(0).?);
    try std.testing.expectEqual(@as(u32, @intCast(index)), table.active_indices.get(@intCast(table.active_count - 1)).?.*);
}

test "IPC waiter and group links cross old bounds and complete every item in batches" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    const rights = fdRights(.{ .send = true, .recv = true, .wait = true, .poll = true, .close = true });
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    const peer = s.fdPayloadWithRightsConst(p0, pair.b, .{}).?.payload.channel;
    const group = try s.beginFdWaitGroup(p0, 7, 11, 123);
    const items = former_max_fd_wait_group_links + 17;
    for (0..items) |index| {
        try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, pair.a, fd_abi.event_hangup, 0x1000 + index * 24, 7, 11, 123, group));
    }
    try s.armFdWaitGroup(group);
    try s.closeFd(p0, pair.b);
    var batch: [32]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(batch.len, s.takeIpcChannelPeerCloseWaiters(peer, &batch));
    // Model the scheduler claim. State detachment preserves links but removes
    // every registration before the caller starts copying a bounded batch.
    s.detachFdWaitGroup(group, 123);
    var cursor: u64 = 0;
    var total: usize = 0;
    while (true) {
        const count = s.takeFdWaitGroupReadyBatch(group, 123, &cursor, &batch);
        if (count == 0) break;
        for (batch[0..count]) |target| {
            try std.testing.expectEqual(@as(u64, 0x1000 + total * 24), target.pollfd_va);
            try std.testing.expectEqual(fd_abi.event_hangup, target.revents);
            total += 1;
        }
    }
    try std.testing.expectEqual(items, total);
    s.cancelFdWaitGroup(group, 123);
    try std.testing.expectEqual(@as(usize, 0), s.fd_wait_groups.live);
    try std.testing.expectEqual(@as(usize, 0), s.takeIpcChannelPeerCloseWaiters(peer, &batch));
    try s.closeFd(p0, pair.a);
}

test "COW empty body rejected grant retries without another allocation" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var backend = kernel.InvocationStorageTestAllocator{};
    defer backend.deinit();
    s.pool_allocator = backend.interface();
    defer s.native_cow_tables.deinit();
    var free_list: FreePageList = .{};
    const ref = try s.createNativeCowTable(1, &free_list);
    try s.retainNativeCowTable(ref);
    backend.fail_return = true;
    s.releaseNativeCowTable(ref, null);
    try std.testing.expect(s.nativeCowTableSlot(ref) == null);
    try std.testing.expect(s.native_cow_tables.body_owner.retired != null);
    try std.testing.expect(s.hasPendingDataReturns());
    s.drainPendingDataReturns();
    try std.testing.expect(s.hasPendingDataReturns());
    backend.fail_return = false;
    s.drainPendingDataReturns();
    try std.testing.expect(!s.hasPendingDataReturns());
    try std.testing.expect(s.native_cow_tables.body_owner.retired == null);
}

test "process storage creates 65537 principals and retires exhausted generations" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    // Host-only low mapping supplies real dereferenceable PMM fixture pages
    // below the kernel identity window, without a multi-GB static test ELF.
    const bytes = try std.posix.mmap(@ptrFromInt(4 * 1024 * 1024 * 1024), 1536 * 1024 * 1024, .{ .READ = true, .WRITE = true }, .{ .TYPE = .PRIVATE, .ANONYMOUS = true }, -1, 0);
    defer std.posix.munmap(bytes);
    try std.testing.expect(@intFromPtr(bytes.ptr) + bytes.len <= 16 * 1024 * 1024 * 1024);
    var state: KernelState = undefined;
    try initFdState(&state);
    var list: FreePageList = .{};
    try list.installRegions(&.{.{ .id = 121, .start = @intFromPtr(bytes.ptr), .end = @intFromPtr(bytes.ptr) + bytes.len }});
    try list.seedContiguousRange(@intFromPtr(bytes.ptr), bytes.len / 4096);
    defer {
        // Snapshot teardown still borrows stable FD table bodies.
        state.deinitPools();
        releaseFixtureOwner(&state.fd_tables_extra.directory_owner);
        releaseFixtureOwner(&state.fd_tables_extra.body_owner);
        releaseFixtureOwner(&state.fd_tables_extra.metadata_owner);
        releaseFixtureOwner(&state.descriptor_storage);
        releaseFixtureOwner(&state.vma_directory_storage);
        releaseFixtureOwner(&state.vma_body_storage);
    }
    try std.testing.expect(state.ensureProcessCapacity(65537, &list));
    for (0..65537) |index| {
        const principal = kernel.processPrincipalFromIndex(index).?;
        try std.testing.expect(state.ensureProcessDescriptor(principal, "large"));
        try std.testing.expect(state.isActiveProcess(principal));
    }
    const last = kernel.processPrincipalFromIndex(65536).?;
    try std.testing.expect(state.removeProcessDescriptor(last));
    state.process_descriptors_extra[65536 - kernel.process_count].generation = kernel.types.max_process_generation;
    try std.testing.expect(!state.ensureProcessDescriptor(last, "retired"));
    try std.testing.expect(state.createProcessDescriptorWithCapacityLimit("retired", &list, 65537) == null);
    const next = state.createProcessDescriptor("fresh").?;
    try std.testing.expectEqual(@as(u32, 65537), @intFromEnum(next));
    try std.testing.expect(kernel.processPrincipalFromIndex((@as(usize, 1) << 31) - 1) != null);
    try std.testing.expect(kernel.processPrincipalFromIndex(@as(usize, 1) << 31) == null);
    try std.testing.expect(kernel.processIndexFromPrincipal(.Device0) == null);
    try std.testing.expect(state.nextProcessCapacity(kernel.process_principal_value_count + 1) == null);
}

test "process storage descriptor relocation retains retired generation history" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var state: KernelState = undefined;
    try initFdState(&state);
    var list: FreePageList = .{};
    try list.installRegions(&.{.{ .id = 129, .start = @intFromPtr(&fd_capacity_backing), .end = @intFromPtr(&fd_capacity_backing) + fd_capacity_backing.len }});
    try list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), fd_capacity_backing.len / 4096);
    defer {
        // Snapshot teardown still borrows stable FD table bodies.
        state.deinitPools();
        releaseFixtureOwner(&state.fd_tables_extra.directory_owner);
        releaseFixtureOwner(&state.fd_tables_extra.body_owner);
        releaseFixtureOwner(&state.fd_tables_extra.metadata_owner);
        releaseFixtureOwner(&state.descriptor_storage);
        releaseFixtureOwner(&state.vma_directory_storage);
        releaseFixtureOwner(&state.vma_body_storage);
    }
    try std.testing.expect(state.ensureProcessCapacity(64, &list));
    const principal = kernel.processPrincipalFromIndex(32).?;
    try std.testing.expect(state.ensureProcessDescriptor(principal, "history"));
    try std.testing.expect(state.removeProcessDescriptor(principal));
    state.process_descriptors_extra[0].generation = kernel.types.max_process_generation;
    const old = state.process_descriptors_extra.ptr;
    try std.testing.expect(state.ensureProcessCapacity(128, &list));
    try std.testing.expect(state.process_descriptors_extra.ptr != old);
    try std.testing.expect(state.descriptor_storage.retired == null);
    try std.testing.expectEqual(kernel.types.max_process_generation, state.processDescriptorSlotConst(32).?.generation);
    try std.testing.expect(!state.ensureProcessDescriptor(principal, "must remain retired"));
}

test "fd entries exceed 4096 with stable loans clone grants and complete reclamation" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const pages = fd_capacity_backing.len / 4096;
    try free_list.installRegions(&.{.{ .id = 123, .start = @intFromPtr(&fd_capacity_backing), .end = @intFromPtr(&fd_capacity_backing) + pages * 4096 }});
    try free_list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), pages);
    try s.ensureFdTableCapacity(p0, 256, &free_list);
    const rights = fdRights(.{ .dup = true, .transfer = true, .close = true });
    const object = try createTestFdObject(&s, 99);
    const first = try s.installFd(p0, object, rights, .{}, 16);
    const loan = s.fdEntryConst(p0, first).?;
    try s.ensureFdTableCapacity(p0, 8192, &free_list);
    const storage = &s.getFdTable(p0).?.entries;
    inline for (.{ &storage.directory_owner, &storage.metadata_owner, &storage.body_owner }) |owner| {
        var cursor = owner.published;
        while (cursor) |record| : (cursor = record.next) {
            try std.testing.expectEqual(@as(usize, 1), record.allocation.pages);
        }
    }
    try std.testing.expectEqual(loan, s.fdEntryConst(p0, first).?);
    for (1..4200) |i| {
        const fd = try s.dupFd(p0, first, @intCast(16 + i), rights, .{});
        try std.testing.expectEqual(16 + i, fd);
    }
    try std.testing.expectEqual(@as(?u32, 4200), s.kernelObjectRefCount(object));
    try s.ensureFdTableCapacity(p1, 8192, &free_list);
    try s.cloneFdTableForFork(p0, p1);
    try std.testing.expectEqual(@as(?u32, 8400), s.kernelObjectRefCount(object));
    const child = kernel.processPrincipalFromIndex(2).?;
    try s.grantFdsForProcessCreate(p0, child, &.{.{
        .source_fd = 4215,
        .target_fd = 5000,
        .rights = kernel.fdRightsToBits(rights),
        .flags = 0,
    }}, &free_list);
    try std.testing.expect(s.fdEntryConst(child, 5000) != null);
    try s.closeFdWithFreeList(p0, 4215, &free_list);
    try std.testing.expect(s.fdEntryConst(p1, 4215) != null);
    try std.testing.expectEqual(loan, s.fdEntryConst(p0, first).?);
    try std.testing.expectError(KernelError.InvalidState, s.ensureFdTableCapacity(p0, kernel.fd_number_capacity + 1, &free_list));
    try std.testing.expect(KernelState.fdIndex(fd_abi.thread_self_fd) == null);
    try std.testing.expect(KernelState.fdIndex(std.math.maxInt(u32)) == null);
    s.releasePrincipalNativeMemory(p0, &free_list);
    s.releasePrincipalNativeMemory(p1, &free_list);
    s.releasePrincipalNativeMemory(child, &free_list);
    try std.testing.expectEqual(@as(?u32, null), s.kernelObjectRefCount(object));
    try std.testing.expectEqual(pages, free_list.pageCount());
}

test "fd entry growth failure preserves publication and borrowed entries" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    for (1..10) |fail_at| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var free_list = FreePageList{};
        const pages = fd_capacity_backing.len / 4096;
        try free_list.installRegions(&.{.{ .id = 124, .start = @intFromPtr(&fd_capacity_backing), .end = @intFromPtr(&fd_capacity_backing) + pages * 4096 }});
        try free_list.seedContiguousRange(@intFromPtr(&fd_capacity_backing), pages);
        try s.ensureFdTableCapacity(p0, 256, &free_list);
        const object = try createTestFdObject(&s, 100);
        const fd = try s.installFd(p0, object, fdRights(.{ .close = true }), .{}, 16);
        const loan = s.fdEntryConst(p0, fd).?;
        const table = s.getFdTable(p0).?;
        var failure = kernel.InvocationStorageTestAllocator{ .fail_at = fail_at };
        defer failure.deinit();
        defer s.releasePrincipalNativeMemory(p0, &free_list);
        try std.testing.expectError(KernelError.OutOfFreePages, table.ensureCapacity(512, failure.interface(), &s.object_directory_leaf, &s.fd_retired));
        try std.testing.expectEqual(@as(usize, 256), table.capacity);
        try std.testing.expectEqual(loan, s.fdEntryConst(p0, fd).?);
        // A completed private block may survive the failure. It remains owned
        // by this allocator and must be returned at teardown, even after retry.
        try s.ensureFdTableCapacity(p0, 512, &free_list);
        try std.testing.expectEqual(loan, s.fdEntryConst(p0, fd).?);
        s.releasePrincipalNativeMemory(p0, &free_list);
        try std.testing.expectEqual(@as(usize, 0), failure.active());
        try std.testing.expectEqual(pages, free_list.pageCount());
    }
}

test "large VMO last-page fault fork partial unmap and final data return" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const physical: u64 = 20 << 30;
    try free_list.installRegions(&.{.{ .id = 73, .start = physical, .end = physical + 16 * 4096 }});
    try free_list.seedContiguousRange(physical, 16);
    const count: u64 = 131073;
    const size = count * 4096;
    const va: u64 = 0x40000000;
    const last = va + size - 4096;
    const fd = try s.createZeroOnDemandVmoFd(p0, size, fdRights(.{ .close = true, .map_read = true, .map_write = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    _ = try s.mmapFd(p0, fd, va, size, .{ .read = true, .write = true }, .{ .private = true }, 0);
    const plan = s.prepareNativeVmaCowMapping(p0, last, true, false);
    try std.testing.expectEqual(.allocate_zero, plan.kind);
    const own = (try s.allocPhysicalPage(&free_list)).paddr;
    _ = s.commitNativeVmaCowMapping(p0, plan, own, &free_list).?;
    try s.cloneFdTableForFork(p0, p1);
    try s.cloneVmaTableForFork(p0, p1);
    const child = s.ensureNativeVmaCowMappingLockedSlow(p1, last, true, false, &free_list).?;
    try std.testing.expect(child.paddr != own);
    try s.munmapRangeWithFreeList(p0, last, 4096, &free_list);
    try std.testing.expectEqual(child.paddr, s.nativeVmaFaultMapping(p1, last, false, false).?.paddr);
    try std.testing.expect(s.nativeVmoSlot(ref) != null);
    s.releasePrincipalNativeMemory(p0, &free_list);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try std.testing.expect(s.nativeVmoSlot(ref) == null);
    try std.testing.expectEqual(@as(usize, 16), free_list.pageCount());
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
}

test "VMO backing standalone insertion counts cold preparation and warm publication" {
    var s: KernelState = undefined;
    try initFdState(&s);
    const ref = try s.createNativeVmo(.anonymous, 8192);
    const slot = s.nativeVmoSlot(ref).?;
    const before = slot.page_store.lock.takes;
    try s.installNativeVmoPages(ref, 0, &.{4096}, null);
    try std.testing.expectEqual(@as(usize, 2), slot.page_store.lock.takes - before);
    const warm = slot.page_store.lock.takes;
    try s.installNativeVmoPages(ref, 1, &.{8192}, null);
    try std.testing.expectEqual(@as(usize, 1), slot.page_store.lock.takes - warm);
    const read = slot.page_store.lock.takes;
    try std.testing.expectEqual(@as(?u64, 4096), s.nativeVmoPagePaddr(ref, 0));
    try std.testing.expectEqual(@as(usize, 1), slot.page_store.lock.takes - read);
    s.clearNativeVmoSlot(slot);
}

test "VMO backing batch metadata OOM leaves PFNs and original issuer unchanged" {
    for (1..5) |failure| {
        var s: KernelState = undefined;
        try initFdState(&s);
        var backend = kernel.InvocationStorageTestAllocator{};
        defer backend.deinit();
        kernel.backing_index.test_allocator = backend.interface();
        defer kernel.backing_index.test_allocator = null;
        const ref = try s.createNativeVmo(.anonymous, 1024 * 4096);
        const slot = s.nativeVmoSlot(ref).?;
        defer s.clearNativeVmoSlot(slot);
        try s.installNativeVmoPages(ref, 0, &.{4096}, null);
        const initial = backend.active();
        backend.fail_at = backend.calls + failure;
        var pages: [257]u64 = undefined;
        for (&pages, 0..) |*page, i| page.* = (i + 2) * 4096;
        try std.testing.expectError(KernelError.OutOfFreePages, s.installNativeVmoPages(ref, 256, &pages, null));
        try std.testing.expectEqual(@as(?u64, 4096), s.nativeVmoPagePaddr(ref, 0));
        for (256..513) |i| try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(ref, i));
        try std.testing.expectEqual(@as(u64, 1), slot.page_store.used);
        try std.testing.expectEqual(initial, backend.active());
        try std.testing.expectEqual(@as(usize, 1), backend.last_pages);
    }
}

test "VMO logical size checked rounding preserves u32 count boundary" {
    var s: KernelState = undefined;
    try initFdState(&s);
    const size = @as(u64, kernel.max_vmo_logical_pages) * 4096;
    const ref = try s.createNativeVmo(.anonymous, size);
    const slot = s.nativeVmoSlot(ref).?;
    try std.testing.expectEqual(@as(u32, 0xffffffff), slot.page_count);
    s.clearNativeVmoSlot(slot);
    for ([_]u64{ 0, size + 1, std.math.maxInt(u64) }) |bad|
        try std.testing.expectError(KernelError.InvalidState, s.createNativeVmo(.anonymous, bad));
}

test "VMO metadata rejected returns drain after owner destruction without new allocation" {
    var s: KernelState = undefined;
    try initFdState(&s);
    var backend = kernel.InvocationStorageTestAllocator{};
    defer backend.deinit();
    kernel.backing_index.test_allocator = backend.interface();
    defer kernel.backing_index.test_allocator = null;
    const ref = try s.createNativeVmo(.anonymous, 4096);
    const slot = s.nativeVmoSlot(ref).?;
    try s.installNativeVmoPages(ref, 0, &.{4096}, null);
    backend.fail_return = true;
    s.clearNativeVmoSlot(slot);
    try std.testing.expect(s.hasPendingDataReturns());
    const active = backend.active();
    const calls = backend.calls;
    const returns = backend.returns;
    s.drainPendingDataReturns();
    try std.testing.expectEqual(active, backend.active());
    try std.testing.expectEqual(returns + active, backend.returns);
    backend.fail_return = false;
    s.drainPendingDataReturns();
    try std.testing.expect(!s.hasPendingDataReturns());
    try std.testing.expectEqual(@as(usize, 0), backend.active());
    try std.testing.expectEqual(calls, backend.calls);
}

test "VMO fault commit folds lookup into publication for root and shadow" {
    var s: KernelState = undefined;
    try initFdState(&s);
    const rw = vmaProt(.{ .read = true, .write = true });
    const ref = try s.createNativeVmo(.anonymous, 3 * 4096);
    try s.createVmaWithRetainedVmo(p0, 0x40000000, 3 * 4096, rw, rw, mmapFlags(.{ .anonymous = true, .shared = true }), ref, 0);
    const slot = s.nativeVmoSlot(ref).?;
    for (0..2) |i| {
        const plan = s.prepareNativeVmaFaultMapping(p0, 0x40000000 + i * 4096, false, false);
        const before = slot.page_store.lock.takes;
        try std.testing.expectEqual(@as(u64, (i + 1) * 4096), s.commitNativeVmaFaultMapping(p0, plan, (i + 1) * 4096, null).?.paddr);
        try std.testing.expectEqual(@as(usize, if (i == 0) 2 else 1), slot.page_store.lock.takes - before);
        const winner = slot.page_store.lock.takes;
        try std.testing.expectEqual(@as(u64, (i + 1) * 4096), s.commitNativeVmaFaultMapping(p0, plan, 12288, null).?.paddr);
        try std.testing.expectEqual(@as(usize, 1), slot.page_store.lock.takes - winner);
    }
    const child = try s.createNativeVmo(.anonymous, 3 * 4096);
    try s.retainNativeVmo(ref);
    const child_slot = s.nativeVmoSlot(child).?;
    try s.publishNativeVmoParent(child, ref, 0);
    try s.createVmaWithRetainedVmo(p0, 0x50000000, 3 * 4096, rw, rw, mmapFlags(.{ .anonymous = true, .shared = true }), child, 0);
    const plan = s.prepareNativeVmaFaultMapping(p0, 0x50002000, false, false);
    const parent_before = slot.page_store.lock.takes;
    const child_before = child_slot.page_store.lock.takes;
    try std.testing.expectEqual(@as(u64, 16384), s.commitNativeVmaFaultMapping(p0, plan, 16384, null).?.paddr);
    try std.testing.expectEqual(@as(usize, 1), slot.page_store.lock.takes - parent_before);
    try std.testing.expectEqual(@as(usize, 2), child_slot.page_store.lock.takes - child_before);
    // An earlier prepared hole can lose to a parent publication.
    const parent_plan = kernel.NativeVmaFaultPlan{ .kind = .allocate_zero, .entry_index = plan.entry_index, .fault_page_va = 0x50000000, .vmo = child, .vmo_page_index = 0 };
    const parent_winner = slot.page_store.lock.takes;
    const child_loser = child_slot.page_store.lock.takes;
    try std.testing.expectEqual(@as(u64, 4096), s.commitNativeVmaFaultMapping(p0, parent_plan, 20480, null).?.paddr);
    try std.testing.expectEqual(@as(usize, 1), slot.page_store.lock.takes - parent_winner);
    try std.testing.expectEqual(@as(usize, 1), child_slot.page_store.lock.takes - child_loser);
    std.debug.print("VMO fault commit backing locks root cold=2 warm=1 winner=1; shadow hole=2+parent1 parent-winner=1+parent1 (old 2/2/1 and 2+parent1/1+parent1)\n", .{});
    s.clearNativeVmoSlot(child_slot);
    s.clearNativeVmoSlot(slot);
}

test "VMO COW insertion folds absence check into publication" {
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const ref = try s.createNativeCowTable(2, &free_list);
    const slot = s.nativeCowTableSlot(ref).?;
    const cold = slot.page_store.lock.takes;
    try s.setNativeCowPagePaddr(ref, 0, 4096, null);
    try std.testing.expectEqual(@as(usize, 2), slot.page_store.lock.takes - cold);
    const warm = slot.page_store.lock.takes;
    try s.setNativeCowPagePaddr(ref, 1, 8192, null);
    try std.testing.expectEqual(@as(usize, 1), slot.page_store.lock.takes - warm);
    try std.testing.expectError(KernelError.InvalidState, s.setNativeCowPagePaddr(ref, 0, 12288, null));
    try std.testing.expectEqual(@as(?u64, 4096), s.nativeCowPagePaddr(ref, 0));
    _ = kernel.freeVmoBackingPageStore(slot.page_store_start, slot.page_count, slot.page_store_start.owner);
    s.native_cow_tables.remove(slot, null);
    try kernel.ReferenceCheck.check(&s);
}

test "VMO metadata physical zero is never consumed as an allocator grant" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var issuer = FreePageList{};
    try issuer.installRegions(&.{.{ .id = 91, .start = 0, .end = 4096 }});
    try std.testing.expectError(KernelError.InvalidState, issuer.seedContiguousRange(0, 1));
    kernel.useBackingPmmForTest(&issuer);
    defer kernel.backing_index.test_allocator = null;
    const ref = try s.createNativeVmo(.anonymous, 4096);
    const slot = s.nativeVmoSlot(ref).?;
    defer s.clearNativeVmoSlot(slot);
    try std.testing.expectError(KernelError.OutOfFreePages, s.installNativeVmoPages(ref, 0, &.{4096}, &issuer));
    try std.testing.expectEqual(@as(usize, 0), issuer.pageCount());
    try std.testing.expect(slot.page_store.root == null);
    try std.testing.expectEqual(@as(u64, 0), kernel.vmObjectBackingStoreStats().used_entries);
}

test "reference graph detects missing duplicate generation and ownership negatives" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list: FreePageList = .{};
    const fd = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{ .close = true, .map_read = true }), .{}, 16);
    _ = try s.mmapFd(p0, fd, 0x4000_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 0);
    try kernel.ReferenceCheck.check(&s);
    const source_fd = @constCast(s.fdEntryConst(p0, fd).?);
    const real = source_fd.authority.reference.?;
    const substitute = try s.reference_nodes.reserve(try s.poolAllocator());
    substitute.target = real.target;
    substitute.identity = real.identity;
    substitute.range = real.range;
    source_fd.authority.reference = substitute;
    try std.testing.expectError(error.Missing, kernel.ReferenceCheck.check(&s));
    source_fd.authority.reference = real;
    substitute.target = null;
    kernel.ReferenceIndex.Pool.cancel(substitute);
    const entry = @constCast(s.vmaEntryConst(p0, 0x4000_0000).?);
    const pair = entry.references.?;
    entry.references = null;
    try std.testing.expectError(error.Missing, kernel.ReferenceCheck.check(&s));
    entry.references = pair;
    const generation = pair.vmo.identity.owner_generation;
    pair.vmo.identity.owner_generation +%= 1;
    try std.testing.expectError(error.Identity, kernel.ReferenceCheck.check(&s));
    pair.vmo.identity.owner_generation = generation;
    const root = pair.vmo.target.?;
    var duplicate: @TypeOf(pair.vmo) = .{};
    root.retainTemporary();
    root.connect(&duplicate, pair.vmo.identity, pair.vmo.range, true);
    try std.testing.expectError(error.Duplicate, kernel.ReferenceCheck.check(&s));
    @TypeOf(root.*).disconnect(&duplicate, true);
    root.releaseTemporary();
    root.retainTemporary();
    try std.testing.expectError(error.Ownership, kernel.ReferenceCheck.check(&s));
    root.releaseTemporary();
    try kernel.ReferenceCheck.check(&s);
    try s.munmapRangeWithFreeList(p0, 0x4000_0000, 4096, &free_list);
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try kernel.ReferenceCheck.check(&s);
    try std.testing.expectEqual(@as(usize, 0), s.reference_nodes.nodes.live);
    s.reference_nodes.reclaim();
    try std.testing.expect(s.reference_nodes.nodes.body_owner.published == null);
}

test "reference queue wrap attachment compaction and revoke partial failure" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list: FreePageList = .{};
    const rights = fdRights(.{ .close = true, .transfer = true, .revoke = true, .map_read = true });
    const a = try s.createAnonymousVmoFd(p0, 8192, rights, .{}, 16);
    const b = try s.createAnonymousVmoFd(p0, 4096, rights, .{}, 16);
    const endpoint = try s.createIpcEndpointFd(p0, fdRights(.{ .send = true, .recv = true, .close = true }), .{}, 16);
    const fds = [_]kernel.IpcSendFd{
        .{ .fd = a, .rights = fdRights(.{ .close = true }), .move = false },
        .{ .fd = b, .rights = fdRights(.{ .close = true }), .move = false },
        .{ .fd = a, .rights = fdRights(.{ .close = true }), .move = false },
    };
    for (0..2) |_| try s.ipcSend(p0, endpoint, .{ .fds = &fds }, &free_list);
    for (0..35) |_| {
        const received = try s.ipcRecv(p0, endpoint, 3, 16, &free_list);
        for (received.fds[0..received.fd_count]) |item| try s.closeFdWithFreeList(p0, item.fd, &free_list);
        try s.ipcSend(p0, endpoint, .{ .fds = &fds }, &free_list);
        try kernel.ReferenceCheck.check(&s);
    }
    _ = try s.mmapFd(p0, a, 0x4000_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 0);
    _ = try s.mmapFdIntoProcess(p0, a, p1, 0x4100_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 0);
    const Failing = struct {
        calls: *usize,
        pub fn unmap(self: @This(), _: kernel.PrincipalId, _: u64, _: u64) bool {
            self.calls.* += 1;
            return self.calls.* != 2;
        }
    };
    var calls: usize = 0;
    try std.testing.expectError(KernelError.InvalidState, s.revokeVmoFdWithFreeList(p0, a, &free_list, Failing{ .calls = &calls }));
    try kernel.ReferenceCheck.check(&s);
    _ = try s.revokeVmoFdWithFreeList(p0, a, &free_list, NoopUnmapper{});
    try kernel.ReferenceCheck.check(&s);
    const remaining = try s.ipcRecv(p0, endpoint, 3, 16, &free_list);
    try std.testing.expectEqual(@as(usize, 1), remaining.fd_count);
    try s.closeFdWithFreeList(p0, remaining.fds[0].fd, &free_list);
    try s.closeFdWithFreeList(p0, endpoint, &free_list);
    try s.closeFdWithFreeList(p0, b, &free_list);
    try kernel.ReferenceCheck.check(&s);
    try std.testing.expectEqual(@as(usize, 0), s.reference_nodes.nodes.live);
}

const ReferenceFailureAllocator = struct {
    upstream: kernel.ReferenceIndex.Allocator,
    calls: usize = 0,
    fail_at: usize = 0,
    fail_return: bool = false,
    fn interface(self: *@This()) kernel.ReferenceIndex.Allocator {
        return .{ .context = self, .acquire = acquire, .release = release };
    }
    fn acquire(context: *anyopaque, pages: usize) ?kernel.ReferenceIndex.MappedExtent {
        const self: *@This() = @ptrCast(@alignCast(context));
        self.calls += 1;
        if (self.calls == self.fail_at) return null;
        return self.upstream.acquire(self.upstream.context, pages);
    }
    fn release(context: *anyopaque, extent: kernel.ReferenceIndex.PhysicalExtent) bool {
        const self: *@This() = @ptrCast(@alignCast(context));
        if (self.fail_return) return false;
        return self.upstream.release(self.upstream.context, extent);
    }
};
const ReferenceOperation = enum { mmap, fork, dup, send, recv };
fn referenceReservationCase(operation: ReferenceOperation, fail_at: usize) !usize {
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list: FreePageList = .{};
    var failure: ReferenceFailureAllocator = .{ .upstream = try s.poolAllocator(), .fail_at = fail_at };
    defer s.deinitPools();
    defer {
        s.releasePrincipalNativeMemory(p0, &free_list);
        s.releasePrincipalNativeMemory(p1, &free_list);
        s.reference_nodes.reclaim();
    }
    const rights = fdRights(.{ .close = true, .dup = true, .transfer = true, .map_read = true });
    const fd = try s.createAnonymousVmoFd(p0, 8192, rights, .{}, 16);
    const endpoint = try s.createIpcEndpointFd(p0, fdRights(.{ .send = true, .recv = true, .close = true }), .{}, 16);
    const attachment: kernel.IpcSendFd = .{ .fd = fd, .rights = fdRights(.{ .close = true }), .move = false };
    const attachments = [_]kernel.IpcSendFd{attachment} ** 19;
    if (operation == .fork) {
        _ = try s.mmapFd(p0, fd, 0x4000_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 0);
        _ = try s.mmapFd(p0, fd, 0x4100_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 4096);
    }
    if (operation == .recv) try s.ipcSend(p0, endpoint, .{ .fds = &attachments }, &free_list);
    var drafts: [64]?*kernel.ReferenceIndex.Node = .{null} ** 64;
    defer for (drafts) |draft| if (draft) |node| kernel.ReferenceIndex.Pool.cancel(node);
    if (operation == .dup or operation == .send) {
        var i: usize = 0;
        while (s.reference_nodes.nodes.hasPreparedFree()) : (i += 1) drafts[i] = try s.reference_nodes.reserve(try s.poolAllocator());
    }
    s.pool_allocator = failure.interface();
    if (operation == .mmap) s.getVmaTable(p0).?.allocator = failure.interface();
    if (operation == .fork) s.getVmaTable(p1).?.allocator = failure.interface();
    const result: KernelError!void = switch (operation) {
        .mmap => blk: {
            _ = s.mmapFd(p0, fd, 0x4000_0000, 8192, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 0) catch |err| break :blk err;
            break :blk {};
        },
        .fork => s.cloneVmaTableForFork(p0, p1),
        .dup => blk: {
            _ = s.dupFd(p0, fd, 16, rights, .{}) catch |err| break :blk err;
            break :blk {};
        },
        .send => s.ipcSend(p0, endpoint, .{ .fds = &attachments }, &free_list),
        .recv => blk: {
            _ = s.ipcRecv(p0, endpoint, 19, 16, &free_list) catch |err| break :blk err;
            break :blk {};
        },
    };
    if (fail_at == 0) try result else {
        if (result) |_| return error.ExpectedReservationFailure else |err| try std.testing.expect(err == KernelError.OutOfFreePages or err == KernelError.TableFull);
    }
    try kernel.ReferenceCheck.check(&s);
    return failure.calls;
}

test "reference all reservation positions preserve source and rollback ownership" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    for (std.enums.values(ReferenceOperation)) |operation| {
        const positions = try referenceReservationCase(operation, 0);
        try std.testing.expect(positions != 0);
        for (1..positions + 1) |position| _ = try referenceReservationCase(operation, position);
        std.debug.print("reference {s} reservation OOM positions={} PASS\n", .{ @tagName(operation), positions });
    }
}

fn referenceObjectCreationCase(fail_at: usize) !usize {
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list: FreePageList = .{};
    var failure: ReferenceFailureAllocator = .{ .upstream = try s.poolAllocator(), .fail_at = fail_at };
    s.pool_allocator = failure.interface();
    defer s.deinitPools();
    const result = s.createAnonymousVmoFd(p0, 4096, fdRights(.{ .close = true }), .{}, 16);
    if (fail_at == 0) {
        try s.closeFdWithFreeList(p0, try result, &free_list);
    } else if (result) |_| return error.ExpectedReservationFailure else |err| try std.testing.expect(err == KernelError.OutOfFreePages);
    try kernel.ReferenceCheck.check(&s);
    try std.testing.expectEqual(@as(usize, 0), s.object_reference_slots.live);
    try std.testing.expectEqual(@as(usize, 0), s.reference_nodes.nodes.live);
    return failure.calls;
}

test "reference object publication reserves before consuming ownership at every OOM" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    const count = try referenceObjectCreationCase(0);
    for (1..count + 1) |i| _ = try referenceObjectCreationCase(i);
    std.debug.print("reference object creation OOM positions={} PASS\n", .{count});
}

test "reference fork failure after first published FD rolls back nodes and retains" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free_list: FreePageList = .{};
    const rights = fdRights(.{ .close = true });
    _ = try s.createAnonymousVmoFd(p0, 4096, rights, .{}, 16);
    const second = try s.createAnonymousVmoFd(p0, 4096, rights, .{}, 16);
    const object = s.kernelObjectSlot(s.fdEntryConst(p0, second).?.object).?;
    const count = object.ref_count;
    object.ref_count = std.math.maxInt(u32);
    try std.testing.expectError(KernelError.TableFull, s.cloneFdTableForFork(p0, p1));
    object.ref_count = count;
    try kernel.ReferenceCheck.check(&s);
    try std.testing.expectEqual(@as(usize, 2), s.reference_nodes.nodes.live);
    s.releasePrincipalNativeMemory(p0, &free_list);
    s.releasePrincipalNativeMemory(p1, &free_list);
    try kernel.ReferenceCheck.check(&s);
    try std.testing.expectEqual(@as(usize, 0), s.reference_nodes.nodes.live);
    try std.testing.expectEqual(@as(usize, 0), s.object_reference_slots.live);
}

test "reference object return retry needs no later object allocation" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    var backend: ReferenceFailureAllocator = .{ .upstream = try s.poolAllocator() };
    s.pool_allocator = backend.interface();
    defer s.deinitPools();
    var free_list: FreePageList = .{};
    const fd = try s.createAnonymousVmoFd(p0, 4096, fdRights(.{ .close = true }), .{}, 16);
    backend.fail_return = true;
    try s.closeFdWithFreeList(p0, fd, &free_list);
    try kernel.ReferenceCheck.check(&s);
    try std.testing.expect(kernel.ReferenceIndex.pendingStorage(&s.object_reference_slots));
    s.retryReferenceStorage();
    try std.testing.expect(kernel.ReferenceIndex.pendingStorage(&s.object_reference_slots));
    backend.fail_return = false;
    s.retryReferenceStorage();
    try std.testing.expect(!kernel.ReferenceIndex.pendingStorage(&s.object_reference_slots));
    try std.testing.expect(!s.reference_nodes.needs_reclaim);
    try std.testing.expectEqual(@as(usize, 0), s.object_reference_slots.live);
}

test "reference staged MMIO overlay publishes the final pinned address" {
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    const fd = try s.createMmioRegionFd(p0, .{ .device = 1, .bar_index = 0, .paddr = 0xa000_0000, .user_va = 0, .size = 4096 }, fdRights(.{ .close = true }), .{}, 16);
    try kernel.ReferenceCheck.check(&s);
    const payload = s.fdPayloadWithRights(p0, fd, .{}).?.payload;
    payload.mmio_region.user_va = 0x4100_0000;
    try std.testing.expectError(error.Range, kernel.ReferenceCheck.check(&s));
    payload.mmio_region.user_va = 0;
    s.publishMmioOverlayAddress(p0, fd, 0x4100_0000, 0);
    try kernel.ReferenceCheck.check(&s);
    try std.testing.expect(kernel.ReferenceCheck.pinnedOverlap(&s, p0, 0x4100_0000, 4096, null));
    try std.testing.expect(!kernel.ReferenceCheck.pinnedOverlap(&s, p0, 0x4200_0000, 4096, null));
    try s.closeFd(p0, fd);
    try kernel.ReferenceCheck.check(&s);
}

test "reference empty channels require only single page allocations" {
    var s: KernelState = undefined;
    try initFdState(&s);
    const Tracker = struct {
        upstream: kernel.ReferenceIndex.Allocator,
        largest: usize = 0,
        live: usize = 0,
        peak: usize = 0,
        fn acquire(context: *anyopaque, pages: usize) ?kernel.ReferenceIndex.MappedExtent {
            const self: *@This() = @ptrCast(@alignCast(context));
            const grant = self.upstream.acquire(self.upstream.context, pages) orelse return null;
            self.largest = @max(self.largest, pages);
            self.live += pages;
            self.peak = @max(self.peak, self.live);
            return grant;
        }
        fn release(context: *anyopaque, grant: kernel.ReferenceIndex.PhysicalExtent) bool {
            const self: *@This() = @ptrCast(@alignCast(context));
            if (!self.upstream.release(self.upstream.context, grant)) return false;
            self.live -= grant.pages;
            return true;
        }
    };
    var allocator: Tracker = .{ .upstream = try s.poolAllocator() };
    s.pool_allocator = .{ .context = &allocator, .acquire = Tracker.acquire, .release = Tracker.release };
    defer s.deinitPools();
    var channels: [4200]kernel.IpcChannelRef = undefined;
    for (&channels) |*channel| channel.* = try s.createIpcChannel();
    std.debug.print("reference 4200 empty channels sizeof(message/queue/channel)={}/{}/{} max-request-pages={} peak-pages={}\n", .{ @sizeOf(kernel.types.IpcMessage), @sizeOf(kernel.types.IpcQueue), @sizeOf(kernel.types.IpcChannelSlot), allocator.largest, allocator.peak });
    try std.testing.expectEqual(@as(usize, 1), allocator.largest);
    try std.testing.expectEqual(@as(usize, 0), s.reference_nodes.nodes.live);
    try std.testing.expectEqual(@as(usize, 0), s.object_reference_slots.live);
    for (channels) |channel| {
        s.releaseIpcChannelHandle(.{ .channel = channel, .side = 0 });
        s.releaseIpcChannelHandle(.{ .channel = channel, .side = 1 });
    }
    try kernel.ReferenceCheck.check(&s);
    s.deinitPools();
    try std.testing.expectEqual(@as(usize, 0), allocator.live);
}

test "reference queue attributes preserve every right and flag without cross contamination" {
    const object: kernel.KernelObjectRef = .{ .kind = .vmo, .index = 123, .generation = 456 };
    for (0..64) |bit| for (0..16) |flags| {
        const bits = @as(u64, 1) << @as(u6, @intCast(bit));
        const item = kernel.IpcTransferredFd.init(object, @bitCast(bits), @bitCast(@as(u32, @intCast(flags))));
        try std.testing.expectEqual(bits & kernel.types.fd_known_rights_mask, kernel.fdRightsToBits(item.rights()));
        try std.testing.expectEqual(@as(u32, @intCast(flags)), kernel.fdFlagsToBits(item.flags()));
        try std.testing.expectEqualDeep(object, item.object());
    };
}

// The same fixture is appended to the captured A tree. It observes lifetimes
// before pool teardown, so an equal retain/temporary leak cannot hide in an oracle.
const SteadyCounts = struct {
    free: usize,
    vmos: usize,
    cows: usize,
    objects: usize,
    channels: usize,
    endpoints: usize,
    replies: usize,
    nodes: usize = 0,
    records: usize = 0,
    pairs: usize = 0,
    temporary: u64 = 0,
    fn read(s: *KernelState, pages: *FreePageList) @This() {
        var result: @This() = .{ .free = pages.pageCount(), .vmos = s.native_vmos.live, .cows = s.native_cow_tables.live, .objects = s.fd_objects.live, .channels = s.ipc_channels.live, .endpoints = s.ipc_endpoints.live, .replies = s.ipc_replies.live };
        if (@hasDecl(kernel, "ReferenceCheck")) {
            result.nodes = s.reference_nodes.nodes.live;
            result.records = s.object_reference_slots.live;
            var v = s.native_vmos.iterator();
            while (v.next()) |i| result.temporary += i.value.references.temporary;
            var c = s.native_cow_tables.iterator();
            while (c.next()) |i| result.temporary += i.value.references.temporary;
            var o = s.fd_objects.iterator();
            while (o.next()) |i| if (i.value.reference_state) |r| {
                result.temporary += r.holders.temporary;
            };
            for (0..s.process_capacity) |p| if (s.vmaTableForProcessIndex(p)) |t| {
                result.pairs += t.reference_slots.live;
            };
        }
        return result;
    }
};

const SteadyAllocator = struct {
    const Interface = @TypeOf(invocation_allocator.interface());
    const Mapped = @typeInfo(@typeInfo(@TypeOf(@as(kernel.InvocationStorageTestAllocator, undefined).slots)).array.child).optional.child;
    const Physical = @TypeOf(@as(Mapped, undefined).allocation);
    upstream: Interface,
    live: usize = 0,
    peak: usize = 0,
    fn interface(self: *@This()) Interface {
        return .{ .context = self, .acquire = acquire, .release = release };
    }
    fn acquire(context: *anyopaque, pages: usize) ?Mapped {
        const self: *@This() = @ptrCast(@alignCast(context));
        const grant = self.upstream.acquire(self.upstream.context, pages) orelse return null;
        self.live += pages;
        self.peak = @max(self.live, self.peak);
        return grant;
    }
    fn release(context: *anyopaque, grant: Physical) bool {
        const self: *@This() = @ptrCast(@alignCast(context));
        if (!self.upstream.release(self.upstream.context, grant)) return false;
        self.live -= grant.pages;
        return true;
    }
};

test "steady lifetime returns all ownership to baseline without teardown" {
    const Fixture = struct {
        fn steadyCycle(s: *KernelState, free: *FreePageList, mode: usize) !void {
            const rw = vmaProt(.{ .read = true, .write = true });
            const rights = fdRights(.{ .inspect = true, .close = true, .dup = true, .transfer = true, .map_read = true, .map_write = true, .revoke = true });
            const va = 0x4000_0000;
            switch (mode) {
                0, 1, 2, 3 => {
                    const fd = try s.createAnonymousVmoFdWithPages(p0, 16384, rights, .{}, 16, free);
                    _ = try s.mmapFd(p0, fd, va, 16384, rw, mmapFlags(.{ .private = true }), 0);
                    try s.closeFdWithFreeList(p0, fd, free);
                    if (mode == 0) try s.munmapRangeWithFreeList(p0, va + 4096, 4096, free);
                    if (mode == 1) {
                        try s.setVmaProtRange(p0, va + 4096, 4096, vmaProt(.{ .read = true }));
                        try s.setVmaProtRange(p0, va + 4096, 4096, rw);
                    }
                    if (mode == 2 or mode == 3) {
                        try s.cloneVmaTableForFork(p0, p1);
                        if (mode == 3) {
                            const plan = s.prepareNativeVmaCowMapping(p1, va, true, false);
                            const loser = (try s.allocPhysicalPage(free)).paddr;
                            _ = s.ensureNativeVmaCowMappingLockedSlow(p1, va, true, false, free) orelse return error.NoCow;
                            try std.testing.expect(s.commitNativeVmaCowMapping(p1, plan, loser, free) == null);
                            try free.appendPage(loser);
                        }
                        s.releasePrincipalNativeMemory(p1, free);
                    }
                    try s.munmapRangeWithFreeList(p0, va, 16384, free);
                },
                4 => {
                    const fd = try s.createAnonymousVmoFdWithPages(p0, 4096, rights, .{}, 16, free);
                    _ = try s.dupFd(p0, fd, 16, rights, .{ .cloexec = true });
                    try s.closeCloexecFdsWithFreeList(p0, free);
                    try s.closeFdWithFreeList(p0, fd, free);
                },
                5, 6, 7 => {
                    const fd = try s.createAnonymousVmoFdWithPages(p0, 4096, rights, .{}, 16, free);
                    const pair = try s.createIpcChannelPairFds(p0, fdRights(.{ .send = true, .recv = true, .close = true }), .{}, 16);
                    const fds = [_]kernel.IpcSendFd{.{ .fd = fd, .rights = rights, .move = false }};
                    for (0..35) |_| {
                        try s.ipcSend(p0, pair.a, .{ .fds = &fds }, free);
                        const received = try s.ipcRecv(p0, pair.b, 1, 16, free);
                        try s.closeFdWithFreeList(p0, received.fds[0].fd, free);
                    }
                    try s.ipcSend(p0, pair.a, .{ .fds = &fds }, free);
                    if (mode == 7) _ = try s.revokeVmoFdWithFreeList(p0, fd, free, NoopUnmapper{});
                    try s.closeFdWithFreeList(p0, pair.b, free);
                    try s.closeFdWithFreeList(p0, pair.a, free);
                    if (mode != 7) try s.closeFdWithFreeList(p0, fd, free);
                },
                8 => {
                    const fd = try s.createAnonymousVmoFdWithPages(p0, 4096, rights, .{}, 16, free);
                    const endpoint = try s.createIpcEndpointFd(p0, fdRights(.{ .call = true, .recv = true, .close = true }), .{}, 16);
                    const fds = [_]kernel.IpcSendFd{.{ .fd = fd, .rights = rights, .move = false }};
                    const call = try s.ipcCall(p0, endpoint, .{ .fds = &fds }, .{ .owner = p0 }, free);
                    const request = try s.ipcRecv(p0, endpoint, 1, 16, free);
                    const extracted = try s.ipcExtractReply(p0, request.call, 16);
                    try s.closeFdWithFreeList(p0, request.fds[0].fd, free);
                    try s.closeFdWithFreeList(p0, extracted, free);
                    s.releaseIpcCallReference(call);
                    try s.closeFdWithFreeList(p0, endpoint, free);
                    try s.closeFdWithFreeList(p0, fd, free);
                },
                9 => {
                    const fd = try s.createAnonymousVmoFdWithPages(p0, 8192, rights, .{}, 16, free);
                    const parent = s.nativeVmoRefForFd(p0, fd).?;
                    const view = try s.createNativePageView(parent, 1);
                    const object = try s.createKernelObject(.vmo, .{ .vmo = view });
                    const child = try s.installFd(p0, object, rights, .{}, 16);
                    try s.closeFdWithFreeList(p0, fd, free);
                    try s.closeFdWithFreeList(p0, child, free);
                },
                10 => {
                    const dma = try s.createDmaBufferFd(p0, .{ .device = 1, .user_va = va, .iova = 0x9000_0000, .size = 4096 }, fdRights(.{ .close = true }), .{}, 16);
                    const mmio = try s.createMmioRegionFd(p0, .{ .device = 1, .bar_index = 0, .paddr = 0xa000_0000, .user_va = va + 4096, .size = 4096 }, fdRights(.{ .close = true }), .{}, 16);
                    try s.closeFdWithFreeList(p0, dma, free);
                    try s.closeFdWithFreeList(p0, mmio, free);
                },
                11 => {
                    for ([_]PrincipalId{ p0, p1 }) |p| {
                        const fd = try s.createAnonymousVmoFdWithPages(p, 4096, rights, .{}, 16, free);
                        _ = try s.mmapFd(p, fd, va, 4096, rw, mmapFlags(.{ .private = true }), 0);
                        try s.closeFdWithFreeList(p, fd, free);
                    }
                    try s.replaceVmaTableForExec(p0, p1, free);
                    s.releasePrincipalNativeMemory(p0, free);
                    s.releasePrincipalNativeMemory(p1, free);
                },
                12 => {
                    const fd = try s.createAnonymousVmoFdWithPages(p0, 8192, rights, .{}, 16, free);
                    const parent = s.nativeVmoRefForFd(p0, fd).?;
                    const child = try s.createNativeVmo(.anonymous, 4096);
                    try s.retainNativeVmo(child);
                    try s.retainNativeVmo(parent);
                    if (@hasDecl(KernelState, "publishNativeVmoParent")) try s.publishNativeVmoParent(child, parent, 4096) else s.nativeVmoSlot(child).?.backing.parent = .{ .ref = parent, .offset = 4096 };
                    try s.closeFdWithFreeList(p0, fd, free);
                    s.releaseNativeVmoWithFreeList(child, free);
                },
                13 => {
                    const fd = try s.createAnonymousVmoFdWithPages(p0, 4096, rights, .{}, 16, free);
                    const vmo = s.nativeVmoRefForFd(p0, fd).?;
                    const paddr = s.nativeVmoPagePaddr(vmo, 0).?;
                    var hold = try DmaHold.init(paddr, 1);
                    defer hold.end();
                    try s.closeFdWithFreeList(p0, fd, free);
                    try std.testing.expect(s.hasPendingDataReturns());
                    hold.end();
                    s.drainPendingDataReturns();
                    try std.testing.expect(!s.hasPendingDataReturns());
                },
                else => unreachable,
            }
        }
    };

    var fixture = try physicalFixture();
    defer fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    var tracker: SteadyAllocator = .{ .upstream = try s.poolAllocator() };
    s.pool_allocator = tracker.interface();
    defer s.deinitPools();
    var free: FreePageList = .{};
    try free.installRegions(&.{.{ .id = 0, .start = 0x60_0000, .end = 0x70_0000 }});
    try free.seedContiguousRange(0x60_0000, 256);
    const baseline = SteadyCounts.read(&s, &free);
    for (0..14) |mode| {
        var initial_storage: ?usize = null;
        for (0..32) |cycle| {
            try Fixture.steadyCycle(&s, &free, mode);
            if (@hasDecl(kernel, "ReferenceCheck")) try kernel.ReferenceCheck.check(&s);
            const actual = SteadyCounts.read(&s, &free);
            if (initial_storage) |expected| {
                if (tracker.live != expected) std.debug.print("steady metadata mode={} cycle={} expected={} actual={} pages\n", .{ mode, cycle, expected, tracker.live });
                try std.testing.expectEqual(expected, tracker.live);
            } else initial_storage = tracker.live;
            if (!std.meta.eql(baseline, actual)) std.debug.print("steady mode={} cycle={} initial={any} actual={any}\n", .{ mode, cycle, baseline, actual });
            try std.testing.expectEqualDeep(baseline, actual);
        }
        std.debug.print("steady mode={} cycles=32 PMM/VMO/COW/object/node/record/pair/temporary baseline PASS\n", .{mode});
    }
    // Reusable directory capacity is not a live reference. At the end of
    // the owner lifetime, even that capacity must return to its issuer.
    s.releasePrincipalNativeMemory(p0, &free);
    s.releasePrincipalNativeMemory(p1, &free);
    s.deinitPools();
    try std.testing.expectEqual(@as(usize, 0), tracker.live);
}

test "steady census detects balanced temporary retain leaks independently of edges" {
    var fixture = try physicalFixture();
    defer fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const before = kernel.ReferenceCheck.census(&s, &free);
    const vmo = try s.createNativeVmo(.anonymous, 4096);
    try s.retainNativeVmo(vmo);
    // This is internally consistent, yet not a return to the quiescent baseline.
    try kernel.ReferenceCheck.check(&s);
    const leaked = kernel.ReferenceCheck.census(&s, &free);
    try std.testing.expectEqual(before.vmos + 1, leaked.vmos);
    try std.testing.expectEqual(before.temporary + 1, leaked.temporary);
    s.releaseNativeVmoWithFreeList(vmo, &free);
    const after = kernel.ReferenceCheck.census(&s, &free);
    try std.testing.expectEqual(before.vmos, after.vmos);
    try std.testing.expectEqual(before.temporary, after.temporary);
}

test "steady 4200 live channel pairs return storage and ownership each cycle" {
    var fixture = try physicalFixture();
    defer fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free: FreePageList = .{};
    try free.installRegions(&.{.{ .id = 0, .start = @intFromPtr(&fd_capacity_backing), .end = @intFromPtr(&fd_capacity_backing) + fd_capacity_backing.len }});
    try free.seedContiguousRange(@intFromPtr(&fd_capacity_backing), fd_capacity_backing.len / 4096);
    try s.ensureFdTableCapacity(p0, 16384, &free);
    var tracker: SteadyAllocator = .{ .upstream = try s.poolAllocator() };
    s.pool_allocator = tracker.interface();
    defer s.deinitPools();
    const Pair = KernelState.IpcChannelPair;
    var pairs: [4200]Pair = undefined;
    var steady_pages: ?usize = null;
    for (0..3) |cycle| {
        for (&pairs) |*pair| {
            const made = try s.createIpcChannelPairFds(p0, fdRights(.{ .send = true, .recv = true, .close = true }), .{}, 16);
            pair.* = .{ .a = made.a, .b = made.b };
        }
        std.debug.print("steady burst cycle={} live=4200 pages={} peak={}\n", .{ cycle, tracker.live, tracker.peak });
        for (pairs) |pair| {
            try s.closeFd(p0, pair.a);
            try s.closeFd(p0, pair.b);
        }
        if (@hasDecl(kernel, "ReferenceCheck")) {
            s.retryReferenceStorage();
            try kernel.ReferenceCheck.check(&s);
            try std.testing.expectEqual(@as(usize, 0), s.reference_nodes.nodes.live);
            try std.testing.expectEqual(@as(usize, 0), s.object_reference_slots.live);
        }
        try std.testing.expectEqual(@as(usize, 0), s.fd_objects.live);
        try std.testing.expectEqual(@as(usize, 0), s.ipc_channels.live);
        if (steady_pages) |n| try std.testing.expectEqual(n, tracker.live) else steady_pages = tracker.live;
        std.debug.print("steady burst closed cycle={} cached-pages={}\n", .{ cycle, tracker.live });
    }
    s.releasePrincipalNativeMemory(p0, &free);
    s.deinitPools();
    try std.testing.expectEqual(@as(usize, 0), tracker.live);
    try std.testing.expectEqual(@as(usize, fd_capacity_backing.len / 4096), free.pageCount());
}

test "reference packed queue identity keeps full slot and generation for every kind" {
    inline for (@typeInfo(kernel.types.KernelObjectKind).@"enum".fields) |field| {
        for ([_]u32{ 0, 1, 0x80000000, 0xffffffff }) |index| for ([_]u32{ 0, 1, 0x80000000, 0xffffffff }) |generation| {
            const ref: kernel.KernelObjectRef = .{ .kind = @enumFromInt(field.value), .index = index, .generation = generation };
            const item = kernel.IpcTransferredFd.init(ref, @bitCast(kernel.types.fd_known_rights_mask), @bitCast(kernel.types.fd_known_flags_mask));
            try std.testing.expectEqualDeep(ref, item.object());
            try std.testing.expectEqual(kernel.types.fd_known_rights_mask, kernel.fdRightsToBits(item.rights()));
            try std.testing.expectEqual(kernel.types.fd_known_flags_mask, kernel.fdFlagsToBits(item.flags()));
        };
    }
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(kernel.IpcTransferredFd));
}

test "C reverse operation visits ignore unrelated process VMO FD and queues" {
    const Fixture = struct {
        fn indexedVisitCase(unrelated: usize) ![5]usize {
            var s: KernelState = undefined;
            try initFdState(&s);
            var free: FreePageList = .{};
            try free.installRegions(&.{.{ .id = 0, .start = 4096, .end = (1 << 40) + (1 << 29) }});
            const rights = fdRights(.{ .close = true, .dup = true, .transfer = true, .revoke = true, .map_read = true });
            var growth: FreePageList = .{};
            try growth.installRegions(&.{.{ .id = 0, .start = @intFromPtr(&fd_capacity_backing), .end = @intFromPtr(&fd_capacity_backing) + fd_capacity_backing.len }});
            try growth.seedContiguousRange(@intFromPtr(&fd_capacity_backing), fd_capacity_backing.len / 4096);
            // Grow unrelated process tables as well as VMA/VMO/FD and IPC pools.
            try std.testing.expect(s.ensureProcessCapacity(kernel.process_count + unrelated, &growth));
            for (0..unrelated) |i| {
                const principal = kernel.processPrincipalFromIndex(kernel.process_count + i).?;
                try std.testing.expect(s.ensureProcessDescriptor(principal, "unrelated"));
                const fd = try s.createAnonymousVmoFd(principal, 4096, rights, .{}, 0);
                _ = try s.mmapFd(principal, fd, 0x5000_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 0);
                _ = try s.createIpcEndpointFd(principal, fdRights(.{ .close = true }), .{}, 0);
            }
            const fd = try s.createAnonymousVmoFd(p0, 12288, rights, .{}, 0);
            const ref = s.nativeVmoRefForFd(p0, fd).?;
            try s.installNativeVmoPages(ref, 0, &.{0x8900_0000}, null);
            _ = try s.mmapFd(p0, fd, 0x4000_0000, 12288, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 0);
            _ = try s.mmapFdIntoProcess(p0, fd, p1, 0x4100_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .shared = true }), 0);
            var out: [5]usize = undefined;
            kernel.ReferenceIndex.visits = 0;
            try std.testing.expect(!s.nativeVmoTreeMappingsOverlapPinnedUserObjects(ref));
            out[0] = kernel.ReferenceIndex.visits;
            kernel.ReferenceIndex.visits = 0;
            try s.validateRamCacheMappings(ref, 0, &.{0x8900_0000});
            out[1] = kernel.ReferenceIndex.visits;
            kernel.ReferenceIndex.visits = 0;
            s.releaseUnmappedAnonymousVmoPageRange(p0, ref, 1, 1, &free);
            out[2] = kernel.ReferenceIndex.visits;
            kernel.ReferenceIndex.visits = 0;
            _ = try s.revokeVmoFdWithFreeList(p0, fd, &free, NoopUnmapper{});
            out[3] = kernel.ReferenceIndex.visits;
            try kernel.ReferenceCheck.check(&s);
            try std.testing.expect(s.nativeVmoSlot(ref) == null);
            // Also count successful general-path reclamation: two surviving
            // disjoint VMAs, no FD/temporary, and an owned page in their gap.
            const gap_fd = try s.createAnonymousVmoFd(p0, 12288, rights, .{}, 0);
            const gap = s.nativeVmoRefForFd(p0, gap_fd).?;
            try s.installNativeVmoPages(gap, 1, &.{0x8900_1000}, null);
            _ = try s.mmapFd(p0, gap_fd, 0x4000_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 0);
            _ = try s.mmapFdIntoProcess(p0, gap_fd, p1, 0x4100_0000, 4096, vmaProt(.{ .read = true }), mmapFlags(.{ .anonymous = true }), 8192);
            try s.closeFdWithFreeList(p0, gap_fd, &free);
            const before = free.pageCount();
            kernel.ReferenceIndex.visits = 0;
            s.releaseUnmappedAnonymousVmoPageRange(p0, gap, 1, 1, &free);
            out[4] = kernel.ReferenceIndex.visits;
            try std.testing.expectEqual(before + 1, free.pageCount());
            try std.testing.expectEqual(@as(?u64, 0), s.nativeVmoPagePaddrOrHole(gap, 1));
            try s.munmapRangeWithFreeList(p0, 0x4000_0000, 4096, &free);
            try s.munmapRangeWithFreeList(p1, 0x4100_0000, 4096, &free);
            try kernel.ReferenceCheck.check(&s);
            for (0..unrelated) |i| s.releasePrincipalNativeMemory(kernel.processPrincipalFromIndex(kernel.process_count + i).?, &free);
            return out;
        }
    };

    var fixture = try physicalFixture();
    defer fixture.deinit();
    const small = try Fixture.indexedVisitCase(0);
    const large = try Fixture.indexedVisitCase(128);
    try std.testing.expectEqualDeep(small, large);
    std.debug.print("C visits pinned/ram-cache/blocked-partial/revoke/reclaimed-partial unrelated=0/128: {any} == {any}\n", .{ small, large });
}

test "FD wait storage reuses warm capacity and pressure reclaim preserves armed generations" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const Allocator = @typeInfo(@TypeOf(s.pool_allocator)).optional.child;
    const AcquireResult = @typeInfo(@typeInfo(@TypeOf(@as(Allocator, undefined).acquire)).pointer.child).@"fn".return_type.?;
    const Extent = @typeInfo(@typeInfo(@TypeOf(@as(Allocator, undefined).release)).pointer.child).@"fn".params[1].type.?;
    const ReleaseResult = @typeInfo(@typeInfo(@TypeOf(@as(Allocator, undefined).release)).pointer.child).@"fn".return_type.?;
    const Counting = struct {
        backing: Allocator,
        calls: usize = 0,
        returns: usize = 0,
        fn acquire(context: *anyopaque, pages: usize) AcquireResult {
            const self: *@This() = @ptrCast(@alignCast(context));
            self.calls += 1;
            return self.backing.acquire(self.backing.context, pages);
        }
        fn release(context: *anyopaque, extent: Extent) ReleaseResult {
            const self: *@This() = @ptrCast(@alignCast(context));
            const result = self.backing.release(self.backing.context, extent);
            self.returns += 1;
            return result;
        }
        fn active(self: *const @This()) usize {
            return self.calls - self.returns;
        }
    };
    var backend = Counting{ .backing = try s.poolAllocator() };
    s.pool_allocator = .{ .context = &backend, .acquire = Counting.acquire, .release = Counting.release };
    defer s.deinitPools();
    const rights = fdRights(.{ .send = true, .recv = true, .wait = true, .poll = true, .close = true });
    const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
    var stale: kernel.FdWaitGroupRef = .{};
    var warm_calls: usize = 0;
    var warm_returns: usize = 0;
    for (0..8) |round| {
        const token: u64 = round + 1;
        const group = try s.beginFdWaitGroup(p0, 7, 11, token);
        // Alternating sizes exercises dense link numbering after free-list reuse.
        const count: usize = if (round % 2 == 0) 300 else 1;
        for (0..count) |index| {
            try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, pair.a, fd_abi.event_readable, 0x1000 + index * 24, 7, 11, token, group));
        }
        try s.armFdWaitGroup(group);
        if (round != 0) {
            s.cancelFdWaitGroup(stale, token - 1);
            try std.testing.expectEqual(@as(u64, 1), groupCount(&s, 7));
        }
        s.cancelFdWaitGroup(group, token);
        stale = group;
        if (round == 0) {
            warm_calls = backend.calls;
            warm_returns = backend.returns;
        } else {
            try std.testing.expectEqual(warm_calls, backend.calls);
            try std.testing.expectEqual(warm_returns, backend.returns);
        }
    }
    const active = try s.beginFdWaitGroup(p0, 7, 11, 100);
    try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, pair.a, fd_abi.event_readable, 0x1000, 7, 11, 100, active));
    try s.armFdWaitGroup(active);
    s.reclaimFdWaitStorage();
    try std.testing.expectEqual(@as(u64, 1), groupCount(&s, 7));
    s.cancelFdWaitGroup(active, 100);
    const before = backend.active();
    s.reclaimFdWaitStorage();
    try std.testing.expect(backend.active() < before);
    const reused = try s.beginFdWaitGroup(p0, 7, 11, 101);
    s.cancelFdWaitGroup(active, 100);
    try std.testing.expectEqual(@as(u64, 1), groupCount(&s, 7));
    s.cancelFdWaitGroup(reused, 101);
}

// LPR eventfd allocates a channel pair before publishing its Linux FD.
test "LPR FD4200 sequential channel and growing supervisor wait set" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    const Allocator = @typeInfo(@TypeOf(s.pool_allocator)).optional.child;
    const AcquireResult = @typeInfo(@typeInfo(@TypeOf(@as(Allocator, undefined).acquire)).pointer.child).@"fn".return_type.?;
    const Extent = @typeInfo(@typeInfo(@TypeOf(@as(Allocator, undefined).release)).pointer.child).@"fn".params[1].type.?;
    const ReleaseResult = @typeInfo(@typeInfo(@TypeOf(@as(Allocator, undefined).release)).pointer.child).@"fn".return_type.?;
    const Counting = struct {
        backing: Allocator,
        calls: usize = 0,
        pages: usize = 0,
        limit: usize = std.math.maxInt(usize),
        failed_pages: usize = 0,
        returns: usize = 0,
        fn acquire(context: *anyopaque, pages: usize) AcquireResult {
            const self: *@This() = @ptrCast(@alignCast(context));
            if (pages > self.limit -| self.pages) {
                self.failed_pages = pages;
                return null;
            }
            const grant = self.backing.acquire(self.backing.context, pages) orelse return null;
            self.calls += 1;
            self.pages += pages;
            return grant;
        }
        fn release(context: *anyopaque, extent: Extent) ReleaseResult {
            const self: *@This() = @ptrCast(@alignCast(context));
            const result = self.backing.release(self.backing.context, extent);
            if (result) {
                self.returns += 1;
                self.pages -= extent.pages;
            }
            return result;
        }
        fn active(self: *const @This()) usize {
            return self.calls - self.returns;
        }
    };
    var backend = Counting{ .backing = try s.poolAllocator() };
    s.pool_allocator = .{ .context = &backend, .acquire = Counting.acquire, .release = Counting.release };
    defer s.deinitPools();

    var free_list = FreePageList{};
    const base = @intFromPtr(&fd_capacity_backing);
    try free_list.installRegions(&.{.{ .id = 0, .start = base, .end = base + fd_capacity_backing.len }});
    try free_list.seedContiguousRange(base, fd_capacity_backing.len / 4096);
    defer s.releasePrincipalNativeMemory(p0, &free_list);
    const rights = fdRights(.{ .send = true, .recv = true, .wait = true, .poll = true, .close = true });
    var waits: [4200]kernel.Fd = undefined;
    for (0..4200) |i| {
        try s.ensureFdTableCapacity(p0, 16 + 2 * (i + 1), &free_list);
        const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
        waits[i] = pair.a;
        if (!std.math.isPowerOfTwo(i + 1) and i != 4199) continue;
        const group = try s.beginFdWaitGroup(p0, 7, 11, i + 1);
        for (waits[0 .. i + 1], 0..) |fd, j| {
            try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, fd, fd_abi.event_readable, 0x1000 + j * 24, 7, 11, i + 1, group));
        }
        try s.armFdWaitGroup(group);
        s.cancelFdWaitGroup(group, i + 1);
    }
    std.debug.print("LPR_HOST opened=4200 native_fds=8400 incremental_capacity=true growing_wait_set=true fd_pmm_free={d}\n", .{free_list.pageCount()});
}

// Memory pressure originates in an unrelated page request, not a wait registration.
// All allocator traffic shares the actual PMM, including the reclaim retry.
test "PMM OOM reaches detached wait cache for unrelated page allocation" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const base = @intFromPtr(&fd_capacity_backing);
    try free_list.installRegions(&.{.{ .id = 0, .start = base, .end = base + fd_capacity_backing.len }});
    try free_list.seedContiguousRange(base, fd_capacity_backing.len / 4096);
    const Allocator = @typeInfo(@TypeOf(s.pool_allocator)).optional.child;
    const Result = @typeInfo(@typeInfo(@TypeOf(@as(Allocator, undefined).acquire)).pointer.child).@"fn".return_type.?;
    const Extent = @typeInfo(@typeInfo(@TypeOf(@as(Allocator, undefined).release)).pointer.child).@"fn".params[1].type.?;
    const Backend = struct {
        fn acquire(context: *anyopaque, pages: usize) Result {
            const fl: *FreePageList = @ptrCast(@alignCast(context));
            const grant = fl.popContiguousOwnedInRange(pages, 4096, std.math.maxInt(u64)) catch return null;
            const bytes: [*]align(4096) u8 = @ptrFromInt(grant.physical.toInt());
            return .{ .allocation = grant, .bytes = bytes[0 .. pages * 4096] };
        }
        fn release(context: *anyopaque, grant: Extent) bool {
            const fl: *FreePageList = @ptrCast(@alignCast(context));
            fl.appendContiguousRange(grant.physical.toInt(), grant.pages) catch return false;
            return true;
        }
    };
    s.pool_allocator = .{ .context = &free_list, .acquire = Backend.acquire, .release = Backend.release };
    free_list.reclaimable.register(&s.wait_storage_cache);
    defer s.deinitPools();
    defer s.releasePrincipalNativeMemory(p0, &free_list);
    var held: std.ArrayList(u64) = .empty;
    defer held.deinit(std.testing.allocator);
    defer for (held.items) |page| free_list.appendContiguousRange(page, 1) catch unreachable;
    var waits: [300]kernel.Fd = undefined;
    const rights = fdRights(.{ .send = true, .recv = true, .wait = true, .poll = true, .close = true });
    for (&waits, 0..) |*fd, i| {
        try s.ensureFdTableCapacity(p0, 16 + 2 * (i + 1), &free_list);
        const pair = try s.createIpcChannelPairFds(p0, rights, .{}, 16);
        fd.* = pair.a;
    }
    const group = try s.beginFdWaitGroup(p0, 7, 11, 1);
    for (waits, 0..) |fd, i| try std.testing.expect(try s.registerIpcPollWaitersForFd(p0, fd, fd_abi.event_readable, 0x1000 + i * 24, 7, 11, 1, group));
    try s.armFdWaitGroup(group);
    s.cancelFdWaitGroup(group, 1);
    const retained = s.wait_storage_cache.pages;
    try std.testing.expect(retained != 0);
    const free = free_list.pageCount();
    try held.ensureTotalCapacity(std.testing.allocator, free);
    for (0..free) |_| held.appendAssumeCapacity(try free_list.popFront());
    try std.testing.expectEqual(@as(usize, 0), free_list.pageCount());
    // A single unrelated physical allocation must use the common retry.
    const recovered = try free_list.popFront();
    defer free_list.appendContiguousRange(recovered, 1) catch unreachable;
    try std.testing.expect(s.wait_storage_cache.pages < retained);
    std.debug.print("COMMON_OOM cache_before={d} cache_after={d} pmm_free_before=0 pmm_free_after={d} unrelated_page=PASS\n", .{ retained, s.wait_storage_cache.pages, free_list.pageCount() });
}

fn expectVmaAddressIndex(s: *KernelState, owner: PrincipalId) !void {
    const table = s.getVmaTable(owner).?;
    try table.address_index.validate();
    try std.testing.expectEqual(table.active_count, table.address_index.count);
    for (0..table.active_count) |position| {
        const slot = table.active_indices.get(@intCast(position)).?.*;
        const entry = table.entries.get(slot).?;
        try std.testing.expect(entry.references.?.address_owner == table);
        try std.testing.expectEqual(slot, entry.references.?.address_node.key.slot);
        try std.testing.expect(table.entryForVa(entry.start_va) == entry);
        try std.testing.expect(table.entryForVa(entry.endVa() - 1) == entry);
        var depth: usize = 0;
        var cursor = table.address_index.root;
        while (cursor) |node| {
            depth += 1;
            if (node == entry) break;
            cursor = node.references.?.address_node.child[@intFromBool(entry.start_va > node.start_va)];
        }
        try std.testing.expect(depth <= 2 * (std.math.log2_int(usize, table.active_count + 1) + 1));
    }
}

test "VMA address index survives split trim fork exec transfer and many mappings" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    var free_list = FreePageList{};
    const base: u64 = 0x6000_0000;
    const rw = vmaProt(.{ .read = true, .write = true });
    const flags = mmapFlags(.{ .private = true, .anonymous = true, .noreserve = true });
    // Permute insertion order and cross storage leaves; no fixed VMA cap.
    for (0..1025) |i| {
        const n = (i * 513) % 1025;
        _ = try s.createAnonymousVmaWithPages(p0, base + n * 32768, 16384, rw, rw, flags, &free_list);
    }
    try expectVmaAddressIndex(&s, p0);
    try s.setVmaProtRange(p0, base + 4096, 4096, .{});
    try expectVmaAddressIndex(&s, p0);
    try s.setVmaProtRange(p0, base + 4096, 4096, rw);
    try expectVmaAddressIndex(&s, p0);
    try s.munmapRangeWithFreeList(p0, base + 32768 + 4096, 4096, &free_list);
    try expectVmaAddressIndex(&s, p0);
    try std.testing.expect(s.vmaEntryForVaConst(p0, base + 32768 + 4096) == null);
    try s.munmapRangeWithFreeList(p0, base + 65536, 4096, &free_list);
    try expectVmaAddressIndex(&s, p0);
    try s.munmapRangeWithFreeList(p0, base + 98304 + 12288, 4096, &free_list);
    try expectVmaAddressIndex(&s, p0);
    try s.cloneVmaTableForFork(p0, p1);
    try expectVmaAddressIndex(&s, p0);
    try expectVmaAddressIndex(&s, p1);
    try s.replaceVmaTableForExec(p2, p1, &free_list);
    try expectVmaAddressIndex(&s, p1);
    try expectVmaAddressIndex(&s, p2);
    s.releasePrincipalNativeMemory(p0, &free_list);
    s.releasePrincipalNativeMemory(p2, &free_list);
    try expectVmaAddressIndex(&s, p0);
    try expectVmaAddressIndex(&s, p2);
    // Adjacent anonymous growth retains one node and extends its range.
    _ = try s.createAnonymousVmaWithPages(p0, base, 4096, .{}, rw, flags, &free_list);
    _ = try s.createAnonymousVmaWithPages(p0, base + 4096, 4096, .{}, rw, flags, &free_list);
    try expectVmaAddressIndex(&s, p0);
    try std.testing.expectEqual(@as(usize, 1), s.getVmaTable(p0).?.active_count);
    try std.testing.expect(s.vmaEntryForVaConst(p0, base + 8191) != null);
    s.releasePrincipalNativeMemory(p0, &free_list);
}

test "VMA address index storage footprint" {
    const Before = struct {
        references: @FieldType(kernel.VmaEntry, "references") = null,
        active: bool = false,
        start_va: u64 = 0,
        size_bytes: u64 = 0,
        prot: kernel.VmaProt = .{},
        max_prot: kernel.VmaProt = .{},
        flags: kernel.MmapFlags = .{},
        vmo: kernel.NativeVmoRef = .{},
        vmo_offset: u64 = 0,
        cow_table: kernel.NativeCowTableRef = .{},
        cow_page_offset: u32 = 0,
    };
    const Ref = @typeInfo(@typeInfo(@FieldType(kernel.VmaEntry, "references")).optional.child).pointer.child;
    try std.testing.expectEqual(@sizeOf(Before), @sizeOf(kernel.VmaEntry));
    try std.testing.expectEqual(@as(usize, 32), @FieldType(kernel.VmaTable, "entries").block_slots);
    try std.testing.expectEqual(@as(usize, 8), @FieldType(kernel.VmaTable, "reference_slots").block_slots);
    std.debug.print("VMA footprint before_entry={} entry={} reference_pair={} before_block={} block={} refs_block={} object={}\n", .{
        @sizeOf(Before),                                                  @sizeOf(kernel.VmaEntry),                           @sizeOf(Ref),
        std.math.floorPowerOfTwo(usize, @max(1, 3072 / @sizeOf(Before))), @FieldType(kernel.VmaTable, "entries").block_slots, @FieldType(kernel.VmaTable, "reference_slots").block_slots,
        @sizeOf(kernel.KernelObjectSlot),
    });
}

test "IPC poll direct queue unlinks before slot reuse and preserves FIFO" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var state: KernelState = undefined;
    try initFdState(&state);
    var list = kernel.IpcWaitList{ .allocator = try state.poolAllocator() };
    defer list.waiters.deinit();
    const owner = kernel.processPrincipalFromIndex(0).?;
    const key = kernel.IpcWaitKey{ .kind = .channel, .index = 1, .generation = 1, .side = 0 };
    for (1..4) |tid| {
        _ = try list.register(key, owner, 0x1000 + tid * 24, .{}, 20, 0, fd_abi.event_readable, 0, tid, 1, tid, .{ .index = @intCast(tid), .generation = 1 });
    }
    const old = list.poll_head.?.next.?;
    const old_generation = old.registration.slot_generation;
    list.removeWaiter(old);
    const fresh = try list.register(key, owner, 0x4000, .{}, 20, 0, fd_abi.event_readable, 0, 4, 1, 4, .{ .index = 4, .generation = 1 });
    try std.testing.expect(fresh.slot_generation != old_generation);
    var targets: [1]kernel.ThreadWakeTarget = undefined;
    for ([_]usize{ 1, 3, 4 }) |tid| {
        try std.testing.expectEqual(@as(usize, 1), list.takeReadablePollWaiters(&targets));
        try std.testing.expectEqual(tid, targets[0].thread_index);
        list.unregister(tid, 1);
    }
    try std.testing.expect(list.poll_head == null and list.poll_tail == null);
    try std.testing.expectEqual(@as(usize, 0), list.waiters.live);
}

// The VM failed in the channel Body grant, after PMM drained every CPU cache.
// Exercise the real table with a finite page budget, not a fixed FD limit.
test "channel FD4200 fits finite backing without four page bodies" {
    const Table = @FieldType(KernelState, "ipc_channels");
    const Allocator = @typeInfo(@FieldType(KernelState, "pool_allocator")).optional.child;
    const Mapped = @typeInfo(@typeInfo(@typeInfo(@FieldType(Allocator, "acquire")).pointer.child).@"fn".return_type.?).optional.child;
    const Extent = @typeInfo(@typeInfo(@FieldType(Allocator, "release")).pointer.child).@"fn".params[1].type.?;
    const Backend = struct {
        pages: usize = 0,
        peak: usize = 0,
        largest: usize = 0,
        failed: usize = 0,
        fn acquire(ctx: *anyopaque, pages: usize) ?Mapped {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            if (pages > 21204 -| self.pages) {
                self.failed = pages;
                return null;
            }
            const bytes = std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), pages * 4096) catch return null;
            self.pages += pages;
            self.peak = @max(self.peak, self.pages);
            self.largest = @max(self.largest, pages);
            return .{ .allocation = .{ .physical = .fromInt(@intFromPtr(bytes.ptr)), .pages = pages, .region_id = 1 }, .bytes = bytes };
        }
        fn release(ctx: *anyopaque, grant: Extent) bool {
            const self: *@This() = @ptrCast(@alignCast(ctx));
            const ptr: [*]align(4096) u8 = @ptrFromInt(grant.physical.toInt());
            std.testing.allocator.free(ptr[0 .. grant.pages * 4096]);
            self.pages -= grant.pages;
            return true;
        }
    };
    var backend = Backend{};
    var table = Table{};
    defer table.deinit();
    const allocator: Allocator = .{ .context = &backend, .acquire = Backend.acquire, .release = Backend.release };
    var opened: usize = 0;
    while (opened < 4200) : (opened += 1) {
        _ = table.allocate(allocator, null, std.math.maxInt(u32)) catch |err| {
            std.debug.print("CHANNEL_PRESSURE opened={d} pages={d} failed_grant={d}\n", .{ opened, backend.pages, backend.failed });
            return err;
        };
    }
    try std.testing.expectEqual(@as(usize, 1), backend.largest);
    std.debug.print("CHANNEL_PRESSURE opened={d} peak_pages={d} largest={d}\n", .{ opened, backend.peak, backend.largest });
}

// The golden stream is generated by the pre-index munmap/preflight/removal
// implementation. Serialize semantic values, not pointer-bearing reverse links
// or padding; keep every original VMA field, slot/list order and backing slot.
const VmaCutOracle = struct {
    words: std.ArrayList(u64) = .empty,
    states: usize = 0,
    fn put(self: *@This(), word: u64) !void {
        try self.words.append(std.testing.allocator, word);
    }
    fn value(self: *@This(), v: anytype) !void {
        const T = @TypeOf(v);
        switch (@typeInfo(T)) {
            .bool => try self.put(@intFromBool(v)),
            .int, .comptime_int => try self.put(@intCast(v)),
            .@"enum" => try self.put(@intFromEnum(v)),
            .optional => {
                try self.put(@intFromBool(v != null));
                if (v) |n| try self.value(n);
            },
            .@"struct" => inline for (std.meta.fields(T)) |field| {
                if (comptime !std.mem.eql(u8, field.name, "references") and !std.mem.eql(u8, field.name, "active_position")) try self.value(@field(v, field.name));
            },
            else => @compileError("nonsemantic oracle value: " ++ @typeName(T)),
        }
    }
    fn snapshot(self: *@This(), s: *KernelState, free: *FreePageList) !void {
        try kernel.ReferenceCheck.check(s);
        try self.put(self.states);
        self.states += 1;
        for ([_]PrincipalId{ p0, p1, p2 }) |owner| {
            const table = s.getVmaTable(owner).?;
            try expectVmaAddressIndex(s, owner);
            // The new metadata is checked separately; it cannot change the
            // canonical legacy stream or the allocation density.
            if (comptime @hasField(kernel.VmaEntry, "active_position")) try kernel.ReferenceCheck.vmaTable(table);
            try self.put(table.active_count);
            try self.put(table.next_user_map_va);
            for (0..table.active_count) |pos| try self.value(table.active_indices.get(@intCast(pos)).?.*);
            var entries = table.entries.iterator();
            while (entries.next()) |item| {
                try self.value(item.ref);
                try self.value(item.value.*);
            }
            try self.put(std.math.maxInt(u64));
        }
        var cows = s.native_cow_tables.iterator();
        while (cows.next()) |item| {
            const slot = item.value;
            try self.value(item.ref);
            try self.value(slot.active);
            try self.value(slot.generation);
            try self.value(slot.ref_count);
            try self.value(slot.page_count);
            const ref = kernel.NativeCowTableRef{ .index = item.ref.index, .generation = item.ref.generation };
            for (0..slot.page_count) |i| try self.value(s.nativeCowPagePaddr(ref, @intCast(i)));
        }
        try self.put(std.math.maxInt(u64));
        var vmos = s.native_vmos.iterator();
        while (vmos.next()) |item| {
            const slot = item.value;
            try self.value(item.ref);
            try self.value(slot.kind);
            try self.value(slot.generation);
            try self.value(slot.ref_count);
            try self.value(slot.size_bytes);
            try self.value(slot.page_count);
            try self.value(slot.zero_on_demand);
            try self.value(slot.page_view_sparse);
            try self.value(slot.page_view_max_prot);
            try self.value(slot.ram_cache_state);
            try self.value(slot.backing.parent);
            const ref = kernel.NativeVmoRef{ .index = item.ref.index, .generation = item.ref.generation };
            for (0..slot.page_count) |i| try self.value(s.nativeVmoPagePaddrOrHole(ref, i));
        }
        try self.put(std.math.maxInt(u64));
        try self.value(free.pageCount());
        try self.value(free.rangeCount());
        for (0..free.rangeCount()) |i| {
            const range = free.rangeAt(i);
            try self.value(range.region_id);
            try self.value(range.physical_start);
            try self.value(range.len);
        }
    }
};

test "VMA cut legacy oracle randomized operations and shared COW fixed remap fork exec failures" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    try free.installRegions(&.{.{ .id = 0, .start = 4096, .end = 16 * 1024 * 1024 }});
    try free.seedContiguousRange(0x100000, 2048);
    var oracle: VmaCutOracle = .{};
    defer oracle.words.deinit(std.testing.allocator);
    const base: u64 = 0x60000000;
    const rw = vmaProt(.{ .read = true, .write = true });
    const flags = mmapFlags(.{ .anonymous = true, .private = true, .fork_cow = true });
    // Three-way split shares one COW table. Reverse address/slot order by
    // giving the prefix a higher slot; deleting the middle before trimming the
    // suffix exercises the uniqueness-sensitive reclamation described in C1.
    _ = try s.createAnonymousVmaWithPages(p0, base, 6 * 4096, rw, rw, flags, &free);
    const cow = try s.createNativeCowTable(6, &free);
    try s.retainNativeCowTable(cow);
    const entry = @constCast(s.vmaEntryForVaConst(p0, base).?);
    entry.cow_table = cow;
    s.refreshVmaReferences(entry);
    for (0..6) |i| try s.setNativeCowPagePaddr(cow, @intCast(i), try free.popFront(), &free);
    try s.setVmaProtRange(p0, base + 2 * 4096, 2 * 4096, .{ .read = true });
    try oracle.snapshot(&s, &free);
    try s.munmapRangeWithFreeList(p0, base + 4 * 4096, 2 * 4096, &free);
    try oracle.snapshot(&s, &free);
    try s.munmapRangeWithFreeList(p0, base + 4096, 3 * 4096, &free);
    try oracle.snapshot(&s, &free);
    try s.munmapRangeWithFreeList(p0, base, 6 * 4096, &free);
    try oracle.snapshot(&s, &free);
    // MAP_FIXED across many address/slot permutations, including holes.
    for (0..67) |i| {
        const n = (i * 31) % 67;
        const ref = try s.createAnonymousVmaWithPages(p0, base + n * 8192, 4096, rw, rw, flags, &free);
        try s.installNativeVmoPages(ref, 0, &.{try free.popFront()}, null);
    }
    try oracle.snapshot(&s, &free);
    var fixed = try s.prepareFixedAnonymousMmap(p0, base + 4096, 65 * 8192, rw, rw, flags, &free);
    s.commitFixedMmapPrepared(&fixed, &free);
    try oracle.snapshot(&s, &free);
    try s.munmapRangeWithFreeList(p0, base, 67 * 8192, &free);
    try oracle.snapshot(&s, &free);
    // Shrink, a forced MAYMOVE relocation, then FIXED over existing VMAs.
    _ = try s.createAnonymousVmaWithPages(p0, base, 4 * 4096, rw, rw, flags, &free);
    _ = try s.mremapRangeWithFreeList(p0, base, 4 * 4096, 3 * 4096, 0, false, false, &free);
    try oracle.snapshot(&s, &free);
    _ = try s.createAnonymousVmaWithPages(p0, base + 3 * 4096, 4096, rw, rw, .{ .anonymous = true, .private = true }, &free);
    var move = try s.prepareMremapWithFreeList(p0, base, 3 * 4096, 4 * 4096, base + 0x200000, true, false, &free);
    _ = s.commitMremapPrepared(&move, &free);
    try oracle.snapshot(&s, &free);
    _ = try s.mremapRangeWithFreeList(p0, base + 0x200000, 4 * 4096, 4 * 4096, base, true, true, &free);
    try oracle.snapshot(&s, &free);
    try s.cloneVmaTableForFork(p0, p1);
    try oracle.snapshot(&s, &free);
    try s.munmapRangeWithFreeList(p1, base + 4096, 4096, &free);
    try oracle.snapshot(&s, &free);
    try s.replaceVmaTableForExec(p2, p1, &free);
    try oracle.snapshot(&s, &free);
    try s.munmapRangeWithFreeList(p2, base + 8192, 8192, &free);
    try oracle.snapshot(&s, &free);
    for ([_]PrincipalId{ p0, p1, p2 }) |owner| s.releasePrincipalNativeMemory(owner, &free);
    try oracle.snapshot(&s, &free);
    // Non-prepared middle TableFull and retain failures leave semantic state.
    const ref = try s.createAnonymousVmaWithPages(p0, base, 3 * 4096, rw, rw, flags, &free);
    const table = s.getVmaTable(p0).?;
    const allocator = table.allocator;
    while (table.entries.live < table.entries.capacity) {
        const n = table.entries.live;
        _ = try s.createAnonymousVmaWithPages(p0, base + 0x400000 + n * 8192, 4096, rw, rw, flags, &free);
    }
    var failure: ReferenceFailureAllocator = .{ .upstream = allocator.?, .fail_at = 1 };
    const failure_before = oracle.words.items.len;
    try oracle.snapshot(&s, &free);
    const failure_after = oracle.words.items.len;
    table.allocator = failure.interface();
    try std.testing.expectError(KernelError.TableFull, s.munmapRangeWithFreeList(p0, base + 4096, 4096, &free));
    table.allocator = allocator;
    try oracle.snapshot(&s, &free);
    try std.testing.expectEqualSlices(u64, oracle.words.items[failure_before + 1 .. failure_after], oracle.words.items[failure_after + 1 ..]);
    s.nativeVmoSlot(ref).?.ref_count = std.math.maxInt(u32);
    try std.testing.expectError(KernelError.TableFull, s.munmapRangeWithFreeList(p0, base + 4096, 4096, &free));
    s.nativeVmoSlot(ref).?.ref_count = 1;
    try oracle.snapshot(&s, &free);
    const fail_cow = try s.createNativeCowTable(3, &free);
    try s.retainNativeCowTable(fail_cow);
    const fail_entry = @constCast(s.vmaEntryForVaConst(p0, base).?);
    fail_entry.cow_table = fail_cow;
    s.refreshVmaReferences(fail_entry);
    const cow_before = oracle.words.items.len;
    try oracle.snapshot(&s, &free);
    const cow_after = oracle.words.items.len;
    s.nativeCowTableSlot(fail_cow).?.ref_count = std.math.maxInt(u32);
    try std.testing.expectError(KernelError.TableFull, s.munmapRangeWithFreeList(p0, base + 4096, 4096, &free));
    s.nativeCowTableSlot(fail_cow).?.ref_count = 1;
    try oracle.snapshot(&s, &free);
    try std.testing.expectEqualSlices(u64, oracle.words.items[cow_before + 1 .. cow_after], oracle.words.items[cow_after + 1 ..]);
    try s.munmapRangeWithFreeList(p0, base, 3 * 4096, &free);
    // Every seed mixes fixed replacement, multi-VMA cuts, middle splits,
    // protection splits/merges, fork/child cuts, exec moves, shrink/fixed move.
    for ([_]u64{ 1, 0xdeadbeef, 0x766d615f636f77 }) |seed| {
        var rng = std.Random.DefaultPrng.init(seed);
        for (0..768) |step| {
            const start = base + rng.random().uintLessThan(u64, 128) * 4096;
            const pages = 1 + rng.random().uintLessThan(u64, 9);
            switch (step % 8) {
                0, 1 => {
                    var plan = try s.prepareFixedAnonymousMmap(p0, start, pages * 4096, rw, rw, flags, &free);
                    s.commitFixedMmapPrepared(&plan, &free);
                },
                2 => try s.munmapRangeWithFreeList(p0, start, pages * 4096, &free),
                3 => {
                    var plan = try s.prepareMunmapRangeWithFreeList(p0, start, pages * 4096, &free);
                    s.commitMunmapPrepared(&plan, &free);
                },
                4 => if (s.vmaEntryForVaConst(p0, start)) |e| {
                    try s.setVmaProtRange(p0, start, @min(pages * 4096, e.endVa() - start), if (step % 16 == 4) .{ .read = true } else rw);
                },
                5 => {
                    try s.cloneVmaTableForFork(p0, p1);
                    try s.munmapRangeWithFreeList(p1, start, pages * 4096, &free);
                    try oracle.snapshot(&s, &free);
                    try s.replaceVmaTableForExec(p2, p1, &free);
                    try s.munmapRangeWithFreeList(p2, start + 4096, pages * 4096, &free);
                    try oracle.snapshot(&s, &free);
                    s.releasePrincipalNativeMemory(p2, &free);
                },
                6 => if (s.vmaEntryForVaConst(p0, start)) |e| {
                    const bytes = e.endVa() - start;
                    if (bytes > 4096) _ = try s.mremapRangeWithFreeList(p0, start, bytes, 4096, 0, false, false, &free);
                },
                7 => if (s.vmaEntryForVaConst(p0, start)) |e| {
                    const bytes = @min(pages * 4096, e.endVa() - start);
                    _ = try s.mremapRangeWithFreeList(p0, start, bytes, bytes, base + 0x100000, true, true, &free);
                },
                else => unreachable,
            }
            try oracle.snapshot(&s, &free);
        }
        s.releasePrincipalNativeMemory(p0, &free);
        try oracle.snapshot(&s, &free);
    }
    const data = std.mem.sliceAsBytes(oracle.words.items);
    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(data, &digest, .{});
    const hex = std.fmt.bytesToHex(digest, .lower);
    std.debug.print("VMA_ORACLE states={} bytes={} sha256={s}\n", .{ oracle.states, data.len, hex });
    try std.Io.Dir.cwd().createDirPath(std.testing.io, "../.artifacts/kernel-vma-cut-oracle");
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = "../.artifacts/kernel-vma-cut-oracle/snapshots.bin", .data = data });
    // Filled from the unchanged legacy implementation; full byte streams are
    // also compared by the implementation validation harness.
    try std.testing.expectEqualStrings("a5b9852097f7ab2b294524a257bd1352b9d83d4a55d23ac40a43c6192abadca9", &hex);
}

test "VMA cut checks reject leftover marks missing tree nodes and incorrect overlap sets" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    _ = try s.createAnonymousVmaWithPages(p0, 0x60000000, 3 * 4096, .{}, .{}, .{ .anonymous = true, .private = true }, &free);
    const table = s.getVmaTable(p0).?;
    const slot = table.active_indices.get(0).?.*;
    const entry = table.entries.get(slot).?;
    try kernel.ReferenceCheck.vmaTable(table);
    table.entries.mark(slot, 1, true);
    try std.testing.expectError(error.VmaMark, kernel.ReferenceCheck.vmaTable(table));
    try std.testing.expectError(error.VmaMark, kernel.ReferenceCheck.vmaOverlapMarks(table, 0x70000000, 0x70001000));
    table.entries.mark(slot, 1, false);
    try std.testing.expectError(error.VmaMark, kernel.ReferenceCheck.vmaOverlapMarks(table, 0x60000000, 0x60001000));
    table.address_index.remove(entry);
    try std.testing.expectError(error.Count, kernel.ReferenceCheck.check(&s));
    // Equal counts must not hide a detached node whose linked flag is corrupt.
    table.address_index.count = 1;
    entry.references.?.address_node.key = .{ .start = entry.start_va, .slot = slot };
    entry.references.?.address_node.linked = true;
    try std.testing.expectError(error.Missing, kernel.ReferenceCheck.vmaTable(table));
    table.address_index.count = 0;
    entry.references.?.address_node.linked = false;
    table.reindex(entry, slot);
    try kernel.ReferenceCheck.check(&s);
    try s.munmapRangeWithFreeList(p0, 0x60001000, 4096, &free);
    try std.testing.expect(table.entries.firstMarkedAtOrAfter(0) == null);
    try kernel.ReferenceCheck.check(&s);
    s.releasePrincipalNativeMemory(p0, &free);
}

test "pager VMO coalesces fault, publishes one canonical page and releases last reference" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const base: u64 = 20 << 30;
    try free.installRegions(&.{.{ .id = 88, .start = base, .end = base + 16 * 4096 }});
    try free.seedContiguousRange(base, 16);
    const port_fd = try s.createPagerPortFd(p0, .{}, 16);
    const port = s.pagerPortRefForFd(p0, port_fd, .{ .pager_fault = true }).?;
    const vmo_fd = try s.createPagerVmoFd(p0, 8192, fdRights(.{ .dup = true, .close = true, .map_read = true, .map_write = true, .pager_attach = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, vmo_fd).?;
    try std.testing.expectEqual(kernel.NativeVmoKind.pager, s.nativeVmoSlot(ref).?.kind);
    try std.testing.expect(s.nativeVmoResolvedPagePaddr(ref, 0) == null);
    try std.testing.expect(s.nativeVmoResolvedPagePaddrOrZero(ref, 0) == null);
    try std.testing.expectError(KernelError.InvalidState, s.mmapFdIntoProcess(p0, vmo_fd, p1, 0x50000000, 4096, .{ .read = true }, .{ .private = true }, 0));
    const blind_port = try s.dupFd(p0, port_fd, 16, fdRights(.{ .wait = true, .close = true }), .{});
    const blind_vmo = try s.dupFd(p0, vmo_fd, 16, fdRights(.{ .map_read = true, .close = true }), .{});
    try std.testing.expect(s.pagerPortRefForFd(p0, blind_port, .{ .pager_fault = true }) == null);
    try std.testing.expectError(KernelError.InvalidState, s.attachPagerVmo(p0, blind_port, vmo_fd, 0x12));
    try std.testing.expectError(KernelError.InvalidState, s.attachPagerVmo(p0, port_fd, blind_vmo, 0x12));
    try s.closeFdWithFreeList(p0, blind_port, &free);
    try s.closeFdWithFreeList(p0, blind_vmo, &free);
    const generation = try s.attachPagerVmo(p0, port_fd, vmo_fd, 0x12);
    try std.testing.expect(generation != 0);
    try std.testing.expectError(KernelError.InvalidState, s.mmapFd(p0, vmo_fd, 0x40000000, 4096, .{ .read = true }, .{ .private = true }, 0));
    _ = try s.mmapFdIntoProcess(p0, vmo_fd, p1, 0x50000000, 4096, .{ .read = true }, .{ .private = true }, 0);
    _ = try s.mmapFdIntoProcess(p0, vmo_fd, p2, 0x60000000, 4096, .{ .read = true }, .{ .private = true }, 0);
    try std.testing.expectEqual(.locked_slow_path, s.prepareNativeVmaFaultMapping(p1, 0x50000000, false, false).kind);
    try std.testing.expectEqual(.pending, try s.requestPagerFault(ref, 0, pager_abi.access_read, p1, 41, 1, 1, 0xabba));
    try std.testing.expectEqual(.pending, try s.requestPagerFault(ref, 0, pager_abi.access_read, p2, 42, 1, 2, 0xabba));
    const event = s.peekPagerEvent(port).?;
    try std.testing.expectEqual(pager_abi.event_fault, event.kind);
    try std.testing.expectEqual(@as(u64, 0x12), event.key);
    try std.testing.expect(s.consumePagerEvent(port, event));
    try std.testing.expect(s.peekPagerEvent(port) == null);
    const physical = (try s.allocPhysicalPage(&free)).paddr;
    try s.publishPagerPages(ref, 0, &.{physical}, &free);
    try std.testing.expectError(KernelError.InvalidState, s.publishPagerPages(ref, 0, &.{physical}, &free));
    s.completePagerPublishedRange(port, ref, 0x12, generation, 0, 1);
    try std.testing.expectEqual(physical, s.nativeVmaFaultMapping(p1, 0x50000000, false, false).?.paddr);
    try std.testing.expectEqual(physical, s.nativeVmaFaultMapping(p2, 0x60000000, false, false).?.paddr);
    try s.closeFdWithFreeList(p0, vmo_fd, &free);
    s.releasePrincipalNativeMemory(p1, &free);
    s.releasePrincipalNativeMemory(p2, &free);
    const released = s.peekPagerEvent(port).?;
    try std.testing.expectEqual(pager_abi.event_vmo_release, released.kind);
    try std.testing.expect(s.consumePagerEvent(port, released));
    try std.testing.expect(s.peekPagerEvent(port) == null);
    s.releasePrincipalNativeMemory(p0, &free);
    try std.testing.expectEqual(@as(usize, 16), free.pageCount());
}

test "pager copy probe publishes request without scheduler waiter" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const base: u64 = 20 << 30;
    try free.installRegions(&.{.{ .id = 88, .start = base, .end = base + 8 * 4096 }});
    try free.seedContiguousRange(base, 8);
    const port_fd = try s.createPagerPortFd(p0, .{}, 16);
    const port = s.pagerPortRefForFd(p0, port_fd, .{ .pager_fault = true }).?;
    const vmo_fd = try s.createPagerVmoFd(p0, 4096, fdRights(.{ .close = true, .map_read = true, .pager_attach = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, vmo_fd).?;
    const generation = try s.attachPagerVmo(p0, port_fd, vmo_fd, 0x91);
    _ = try s.mmapFdIntoProcess(p0, vmo_fd, p1, 0x50000000, 4096, .{ .read = true }, .{ .private = true }, 0);
    try std.testing.expectEqual(.pending, try s.requestPagerFault(ref, 0, pager_abi.access_read, p1, 0, 0, 0, 0));
    const event = s.peekPagerEvent(port).?;
    const request_ref = kernel.types.PagerRequestRef{ .index = @truncate(event.request_id), .generation = @truncate(event.request_id >> 32) };
    const request = s.pager_requests.resolve(.{ .index = request_ref.index, .generation = request_ref.generation }).?;
    try std.testing.expect(request.waiters.isNull());
    try std.testing.expect(s.consumePagerEvent(port, event));
    const physical = (try s.allocPhysicalPage(&free)).paddr;
    try s.publishPagerPages(ref, 0, &.{physical}, &free);
    s.completePagerPublishedRange(port, ref, 0x91, generation, 0, 1);
    try std.testing.expectEqual(physical, s.nativeVmaFaultMapping(p1, 0x50000000, false, false).?.paddr);
    try s.closeFdWithFreeList(p0, vmo_fd, &free);
    s.releasePrincipalNativeMemory(p1, &free);
    const released = s.peekPagerEvent(port).?;
    try std.testing.expect(s.consumePagerEvent(port, released));
    s.releasePrincipalNativeMemory(p0, &free);
    try std.testing.expectEqual(@as(usize, 8), free.pageCount());
}

test "pager port death fails every pending page and releases waiters" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const port_fd = try s.createPagerPortFd(p0, .{}, 16);
    const vmo_fd = try s.createPagerVmoFd(p0, 8192, fdRights(.{ .close = true, .map_read = true, .pager_attach = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, vmo_fd).?;
    _ = try s.attachPagerVmo(p0, port_fd, vmo_fd, 0x29);
    _ = try s.mmapFdIntoProcess(p0, vmo_fd, p1, 0x50000000, 8192, .{ .read = true }, .{ .private = true }, 0);
    try std.testing.expectEqual(.pending, try s.requestPagerFault(ref, 0, pager_abi.access_read, p1, 41, 1, 1, 0x44));
    try std.testing.expectEqual(.pending, try s.requestPagerFault(ref, 4096 / 4096, pager_abi.access_read, p1, 42, 1, 2, 0x55));
    try std.testing.expectEqual(@as(usize, 2), s.pager_requests.live);
    try s.closeFdWithFreeList(p0, port_fd, &free);
    try std.testing.expect(s.pagerPageFailed(ref, 0));
    try std.testing.expect(s.pagerPageFailed(ref, 1));
    try std.testing.expectEqual(@as(usize, 0), s.pager_waiters.live);
    try std.testing.expectEqual(.failed, try s.requestPagerFault(ref, 0, pager_abi.access_read, p1, 43, 1, 3, 0x66));
    try s.closeFdWithFreeList(p0, vmo_fd, &free);
    s.releasePrincipalNativeMemory(p1, &free);
    s.releasePrincipalNativeMemory(p0, &free);
    try std.testing.expectEqual(@as(usize, 0), s.pager_ports.live);
    try std.testing.expectEqual(@as(usize, 0), s.pager_requests.live);
    try std.testing.expectEqual(@as(usize, 0), s.pager_releases.live);
}

test "pager supply source retain keeps sampled PFN across close and munmap" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const base: u64 = 0x35_0000;
    try free.installRegions(&.{.{ .id = 0, .start = base, .end = base + 16 * 4096 }});
    try free.seedContiguousRange(base, 16);
    const fd = try s.createAnonymousVmoFdWithPages(p0, 4096,
        fdRights(.{ .close = true, .map_read = true }), .{}, 16, &free);
    const ref = s.nativeVmoRefForFd(p0, fd).?;
    const va: u64 = 0x5700_0000;
    _ = try s.mmapFd(p0, fd, va, 4096, .{ .read = true }, .{ .private = true }, 0);
    const sampled = s.nativeVmoResolvedPagePaddr(ref, 0).?;
    const during_copy = free.pageCount();
    // This is the interleaving between pagerSupply's PFN snapshot and copy:
    // another thread closes the FD and removes its last VMA. The temporary
    // reference must prevent both final return and partial-unmap release.
    try s.retainNativeVmo(ref);
    try s.closeFdWithFreeList(p0, fd, &free);
    try s.munmapRangeWithFreeList(p0, va, 4096, &free);
    try std.testing.expectEqual(@as(?u64, sampled), s.nativeVmoResolvedPagePaddr(ref, 0));
    try std.testing.expectEqual(during_copy, free.pageCount());
    s.releaseNativeVmoWithFreeList(ref, &free);
    try std.testing.expectEqual(@as(?u32, null), s.nativeVmoRefCount(ref));
    try std.testing.expectEqual(@as(usize, 16), free.pageCount());
    s.releasePrincipalNativeMemory(p0, &free);
}

test "pager attach identity resolves reused key without stale VMO alias" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const port_fd = try s.createPagerPortFd(p0, .{}, 16);
    const port = s.pagerPortRefForFd(p0, port_fd, .{ .pager_fault = true }).?;
    const rights = fdRights(.{ .close = true, .pager_attach = true });
    const first_fd = try s.createPagerVmoFd(p0, 4096, rights, .{}, 16);
    const second_fd = try s.createPagerVmoFd(p0, 4096, rights, .{}, 16);
    const first = s.nativeVmoRefForFd(p0, first_fd).?;
    const second = s.nativeVmoRefForFd(p0, second_fd).?;
    const a = try s.attachPagerVmo(p0, port_fd, first_fd, 42);
    const b = try s.attachPagerVmo(p0, port_fd, second_fd, 42);
    try std.testing.expect(a != b);
    try std.testing.expectEqual(first, s.pagerVmoForKey(port, 42, a).?);
    try std.testing.expectEqual(second, s.pagerVmoForKey(port, 42, b).?);
    try s.closeFdWithFreeList(p0, first_fd, &free);
    try std.testing.expect(s.pagerVmoForKey(port, 42, a) == null);
    try std.testing.expectEqual(second, s.pagerVmoForKey(port, 42, b).?);
    const release_event = s.peekPagerEvent(port).?;
    try std.testing.expectEqual(a, release_event.vmo_generation);
    try std.testing.expect(s.consumePagerEvent(port, release_event));
    try s.closeFdWithFreeList(p0, second_fd, &free);
    try s.closeFdWithFreeList(p0, port_fd, &free);
    s.releasePrincipalNativeMemory(p0, &free);
}

test "pager release queue survives a full bounded fault queue" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const port_fd = try s.createPagerPortFd(p0, .{}, 16);
    const port = s.pagerPortRefForFd(p0, port_fd, .{ .pager_fault = true }).?;
    const faults_fd = try s.createPagerVmoFd(p0, 257 * 4096, fdRights(.{ .close = true, .map_read = true, .pager_attach = true }), .{}, 16);
    const faults_ref = s.nativeVmoRefForFd(p0, faults_fd).?;
    _ = try s.attachPagerVmo(p0, port_fd, faults_fd, 1);
    const release_fd = try s.createPagerVmoFd(p0, 4096, fdRights(.{ .close = true, .map_read = true, .pager_attach = true }), .{}, 16);
    _ = try s.attachPagerVmo(p0, port_fd, release_fd, 2);
    for (0..257) |page| {
        try std.testing.expectEqual(.pending, try s.requestPagerFault(faults_ref, @intCast(page), pager_abi.access_read, p1, page + 1, 1, page + 1, 0x11));
    }
    const port_slot = s.pager_ports.resolve(.{ .index = port.index, .generation = port.generation }).?;
    try std.testing.expectEqual(@as(u32, 256), port_slot.queued_faults);
    try s.closeFdWithFreeList(p0, release_fd, &free);
    const release_event = s.peekPagerEvent(port).?;
    try std.testing.expectEqual(pager_abi.event_vmo_release, release_event.kind);
    try std.testing.expectEqual(@as(u64, 2), release_event.key);
    try std.testing.expect(s.consumePagerEvent(port, release_event));
    const first = s.peekPagerEvent(port).?;
    try std.testing.expectEqual(pager_abi.event_fault, first.kind);
    try std.testing.expect(s.consumePagerEvent(port, first));
    try std.testing.expectEqual(@as(u32, 256), port_slot.queued_faults);
    try s.closeFdWithFreeList(p0, port_fd, &free);
    try s.closeFdWithFreeList(p0, faults_fd, &free);
    s.releasePrincipalNativeMemory(p0, &free);
    try std.testing.expectEqual(@as(usize, 0), s.pager_ports.live);
}

test "pager fork shares missing request and private COW diverges after supply" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const base: u64 = 0x35_0000;
    try free.installRegions(&.{.{ .id = 0, .start = base, .end = base + 16 * 4096 }});
    try free.seedContiguousRange(base, 16);
    const port_fd = try s.createPagerPortFd(p0, .{}, 16);
    const port = s.pagerPortRefForFd(p0, port_fd, .{ .pager_fault = true }).?;
    const vmo_fd = try s.createPagerVmoFd(p0, 4096, fdRights(.{ .close = true, .map_read = true, .map_write = true, .pager_attach = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, vmo_fd).?;
    const generation = try s.attachPagerVmo(p0, port_fd, vmo_fd, 4);
    const va: u64 = 0x5100_0000;
    _ = try s.mmapFdIntoProcess(p0, vmo_fd, p1, va, 4096, .{ .read = true, .write = true }, .{ .private = true }, 0);
    try s.cloneVmaTableForFork(p1, p2);
    s.markForkCowVmasCommitted(p1);
    try std.testing.expectEqual(.pending, try s.requestPagerFault(ref, 0, pager_abi.access_read, p1, 41, 1, 1, 1));
    try std.testing.expectEqual(.pending, try s.requestPagerFault(ref, 0, pager_abi.access_read, p2, 42, 1, 2, 2));
    const event = s.peekPagerEvent(port).?;
    try std.testing.expect(s.consumePagerEvent(port, event));
    try std.testing.expect(s.peekPagerEvent(port) == null);
    const physical = (try s.allocPhysicalPage(&free)).paddr;
    try s.publishPagerPages(ref, 0, &.{physical}, &free);
    s.completePagerPublishedRange(port, ref, 4, generation, 0, 1);
    try std.testing.expectEqual(physical, s.nativeVmaFaultMapping(p1, va, false, false).?.paddr);
    try std.testing.expectEqual(physical, s.nativeVmaFaultMapping(p2, va, false, false).?.paddr);
    try std.testing.expect(!s.nativeVmaFaultMapping(p2, va, false, false).?.prot.write);
    const child_write = s.ensureNativeVmaCowMappingLockedSlow(p2, va, true, false, &free) orelse unreachable;
    try std.testing.expect(child_write.paddr != physical);
    try std.testing.expectEqual(physical, s.nativeVmaFaultMapping(p1, va, false, false).?.paddr);
    try s.closeFdWithFreeList(p0, vmo_fd, &free);
    s.releasePrincipalNativeMemory(p2, &free);
    s.releasePrincipalNativeMemory(p1, &free);
    try std.testing.expectEqual(pager_abi.event_vmo_release, s.peekPagerEvent(port).?.kind);
    try s.closeFdWithFreeList(p0, port_fd, &free);
    s.releasePrincipalNativeMemory(p0, &free);
    try std.testing.expectEqual(@as(usize, 16), free.pageCount());
}

test "pager supply after MAP_FIXED replacement cannot alter the new VMA" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const base: u64 = 0x35_0000;
    try free.installRegions(&.{.{ .id = 0, .start = base, .end = base + 16 * 4096 }});
    try free.seedContiguousRange(base, 16);
    const port_fd = try s.createPagerPortFd(p0, .{}, 16);
    const port = s.pagerPortRefForFd(p0, port_fd, .{ .pager_fault = true }).?;
    const pager_fd = try s.createPagerVmoFd(p0, 4096, fdRights(.{ .close = true, .map_read = true, .pager_attach = true }), .{}, 16);
    const pager_ref = s.nativeVmoRefForFd(p0, pager_fd).?;
    const generation = try s.attachPagerVmo(p0, port_fd, pager_fd, 9);
    const va: u64 = 0x5300_0000;
    _ = try s.mmapFdIntoProcess(p0, pager_fd, p1, va, 4096, .{ .read = true }, .{ .private = true }, 0);
    try std.testing.expectEqual(.pending, try s.requestPagerFault(pager_ref, 0, pager_abi.access_read, p1, 41, 1, 1, 1));
    const event = s.peekPagerEvent(port).?;
    try std.testing.expect(s.consumePagerEvent(port, event));
    const other_fd = try s.createAnonymousVmoFdWithPages(p0, 4096, fdRights(.{ .close = true, .map_read = true }), .{}, 16, &free);
    var prepared = try s.prepareFixedFdMmapIntoProcess(p0, other_fd, p1, va, 4096,
        .{ .read = true }, .{ .private = true, .fixed = true }, 0, &free);
    s.commitFixedMmapPrepared(&prepared, &free);
    const replacement = s.nativeVmaFaultMapping(p1, va, false, false).?.paddr;
    const supplied = (try s.allocPhysicalPage(&free)).paddr;
    try std.testing.expect(replacement != supplied);
    try s.publishPagerPages(pager_ref, 0, &.{supplied}, &free);
    s.completePagerPublishedRange(port, pager_ref, 9, generation, 0, 1);
    try std.testing.expectEqual(replacement, s.nativeVmaFaultMapping(p1, va, false, false).?.paddr);
    try s.munmapRangeWithFreeList(p1, va, 4096, &free);
    try std.testing.expect(s.nativeVmaFaultMapping(p1, va, false, false) == null);
    try s.closeFdWithFreeList(p0, other_fd, &free);
    try s.closeFdWithFreeList(p0, pager_fd, &free);
    try std.testing.expectEqual(pager_abi.event_vmo_release, s.peekPagerEvent(port).?.kind);
    try s.closeFdWithFreeList(p0, port_fd, &free);
    s.releasePrincipalNativeMemory(p1, &free);
    s.releasePrincipalNativeMemory(p0, &free);
    try std.testing.expectEqual(@as(usize, 16), free.pageCount());
}

test "pager control FD inherited by fork prevents child self map" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const port_fd = try s.createPagerPortFd(p0, .{}, 16);
    const pager_fd = try s.createPagerVmoFd(p0, 4096,
        fdRights(.{ .close = true, .map_read = true, .pager_attach = true }), .{}, 16);
    _ = try s.attachPagerVmo(p0, port_fd, pager_fd, 11);
    try s.cloneFdTableForFork(p0, p1);
    try std.testing.expectError(KernelError.InvalidState,
        s.mmapFd(p1, pager_fd, 0x5400_0000, 4096, .{ .read = true }, .{ .private = true }, 0));
    try std.testing.expectError(KernelError.InvalidState,
        s.mmapFdIntoProcess(p0, pager_fd, p1, 0x5400_0000, 4096, .{ .read = true }, .{ .private = true }, 0));
    try s.closeFdWithFreeList(p1, pager_fd, &free);
    try s.closeFdWithFreeList(p1, port_fd, &free);
    try s.closeFdWithFreeList(p0, pager_fd, &free);
    try s.closeFdWithFreeList(p0, port_fd, &free);
    s.releasePrincipalNativeMemory(p1, &free);
    s.releasePrincipalNativeMemory(p0, &free);
    try std.testing.expectEqual(@as(usize, 0), s.pager_ports.live);
}

test "pager creator death fails waiters even with inherited control FD" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const port_fd = try s.createPagerPortFd(p0, .{}, 16);
    const pager_fd = try s.createPagerVmoFd(p0, 4096,
        fdRights(.{ .close = true, .map_read = true, .pager_attach = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, pager_fd).?;
    _ = try s.attachPagerVmo(p0, port_fd, pager_fd, 15);
    _ = try s.mmapFdIntoProcess(p0, pager_fd, p2, 0x5500_0000, 4096, .{ .read = true }, .{ .private = true }, 0);
    try s.cloneFdTableForFork(p0, p1);
    try std.testing.expectEqual(.pending, try s.requestPagerFault(ref, 0, pager_abi.access_read, p2, 42, 1, 1, 1));
    s.releasePrincipalNativeMemory(p0, &free);
    try std.testing.expect(s.pagerPageFailed(ref, 0));
    try std.testing.expectEqual(@as(usize, 0), s.pager_waiters.live);
    const inherited = s.pagerPortRefForFd(p1, port_fd, .{ .pager_fault = true }).?;
    try std.testing.expect(s.peekPagerEvent(inherited) == null);
    try std.testing.expectEqual(.failed, try s.requestPagerFault(ref, 0, pager_abi.access_read, p2, 42, 1, 2, 1));
    s.releasePrincipalNativeMemory(p2, &free);
    s.releasePrincipalNativeMemory(p1, &free);
    try std.testing.expectEqual(@as(usize, 0), s.pager_ports.live);
}

test "pager munmap while pending cannot resurrect a removed VMA" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    defer kernel.resetDmaRetentionForTest();
    var s: KernelState = undefined;
    try initFdState(&s);
    defer s.deinitPools();
    var free: FreePageList = .{};
    const base: u64 = 0x35_0000;
    try free.installRegions(&.{.{ .id = 0, .start = base, .end = base + 4 * 4096 }});
    try free.seedContiguousRange(base, 4);
    const port_fd = try s.createPagerPortFd(p0, .{}, 16);
    const port = s.pagerPortRefForFd(p0, port_fd, .{ .pager_fault = true }).?;
    const pager_fd = try s.createPagerVmoFd(p0, 4096,
        fdRights(.{ .close = true, .map_read = true, .pager_attach = true }), .{}, 16);
    const ref = s.nativeVmoRefForFd(p0, pager_fd).?;
    const generation = try s.attachPagerVmo(p0, port_fd, pager_fd, 16);
    const va: u64 = 0x5600_0000;
    _ = try s.mmapFdIntoProcess(p0, pager_fd, p1, va, 4096, .{ .read = true }, .{ .private = true }, 0);
    try std.testing.expectEqual(.pending, try s.requestPagerFault(ref, 0, pager_abi.access_read, p1, 42, 1, 1, 1));
    const event = s.peekPagerEvent(port).?;
    try std.testing.expect(s.consumePagerEvent(port, event));
    try s.munmapRangeWithFreeList(p1, va, 4096, &free);
    const physical = (try s.allocPhysicalPage(&free)).paddr;
    try s.publishPagerPages(ref, 0, &.{physical}, &free);
    s.completePagerPublishedRange(port, ref, 16, generation, 0, 1);
    try std.testing.expect(s.nativeVmaFaultMapping(p1, va, false, false) == null);
    try s.closeFdWithFreeList(p0, pager_fd, &free);
    try std.testing.expectEqual(pager_abi.event_vmo_release, s.peekPagerEvent(port).?.kind);
    try s.closeFdWithFreeList(p0, port_fd, &free);
    s.releasePrincipalNativeMemory(p1, &free);
    s.releasePrincipalNativeMemory(p0, &free);
    try std.testing.expectEqual(@as(usize, 4), free.pageCount());
}

test "page view dirty inventory unions pages without accepting private PFNs and masks tail" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    const parent = try s.createNativeVmo(.anonymous, 70 * 4096);
    try s.retainNativeVmo(parent);
    var pages: [70]u64 = undefined;
    for (&pages, 0..) |*page, i| page.* = 0x8400_0000 + i * 4096;
    try s.installNativeVmoPages(parent, 0, &pages, null);
    const view = try s.createNativePageView(parent, pages.len);
    try s.installNativeVmoPages(view, 0, &pages, null);
    const slot = s.nativeVmoSlot(view).?;
    kernel.page_view_dirty.record(slot, 1, pages[1]);
    kernel.page_view_dirty.record(slot, 69, pages[69]);
    kernel.page_view_dirty.record(slot, 2, 0x8500_2000); // private COW
    kernel.page_view_dirty.record(slot, 70, pages[0]); // outside view
    try std.testing.expectEqual(@as(u64, 2), kernel.page_view_dirty.word(slot, 0));
    try std.testing.expectEqual(@as(u64, 32), kernel.page_view_dirty.word(slot, 1));
    // Inspecting (or failing to deliver) a snapshot never acknowledges it.
    try std.testing.expectEqual(@as(u64, 2), kernel.page_view_dirty.word(slot, 0));
    kernel.page_view_dirty.acknowledge(slot);
    try std.testing.expectEqual(@as(u64, 0), kernel.page_view_dirty.word(slot, 0));
    try std.testing.expectEqual(@as(u64, 0), kernel.page_view_dirty.word(slot, 1));
    @atomicStore(bool, &slot.page_view_dirty_all, true, .release);
    try std.testing.expectEqual(std.math.maxInt(u64), kernel.page_view_dirty.word(slot, 0));
    try std.testing.expectEqual(@as(u64, 63), kernel.page_view_dirty.word(slot, 1));
    try std.testing.expectEqual(@as(u64, 0), kernel.page_view_dirty.word(slot, 2));
}

test "page view retirement preserves formerly writable aliases and refresh dirty inventory" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    const parent = try s.createNativeVmo(.anonymous, 2 * 4096);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{ 0x8400_0000, 0x8400_1000 }, null);
    const view = try s.createNativePageView(parent, 1);
    try s.installNativeVmoPages(view, 0, &.{0x8400_0000}, null);
    const ro = vmaProt(.{ .read = true });
    const rw = vmaProt(.{ .read = true, .write = true });
    try s.createVmaWithRetainedVmo(p0, 0x4100_0000, 4096, ro, rw, mmapFlags(.{ .shared = true }), view, 0);
    try s.createVmaWithRetainedVmo(p1, 0x4200_0000, 4096, rw, rw, mmapFlags(.{ .shared = true }), view, 0);
    const slot = s.nativeVmoSlot(view).?;
    kernel.page_view_dirty.markRetiredAliases(&s, p0, 0x4000_0000, 4096);
    try std.testing.expectEqual(@as(u64, 0), kernel.page_view_dirty.word(slot, 0));
    kernel.page_view_dirty.markRetiredAliases(&s, p0, 0x4100_0000, 4096);
    try std.testing.expectEqual(@as(u64, 1), kernel.page_view_dirty.word(slot, 0));
    kernel.page_view_dirty.acknowledge(slot);
    kernel.page_view_dirty.markRetiredAliases(&s, p1, 0x4200_0000, 4096);
    const staged = try s.createNativePageView(parent, 1);
    try s.installNativeVmoPages(staged, 0, &.{0x8400_1000}, null);
    const Invalidator = struct {
        pub fn invalidate(_: @This(), _: PrincipalId, _: u64, _: u64) bool {
            return true;
        }
    };
    try s.refreshNativePageView(view, staged, Invalidator{});
    try std.testing.expectEqual(@as(u64, 1), kernel.page_view_dirty.word(slot, 0));
    try std.testing.expectEqual(@as(?u64, 0x8400_1000), s.nativeVmoResolvedPagePaddr(view, 0));
    kernel.page_view_dirty.acknowledge(slot);
    kernel.page_view_dirty.record(slot, 0, 0x8400_0000); // retired backing
    try std.testing.expectEqual(@as(u64, 0), kernel.page_view_dirty.word(slot, 0));
    kernel.page_view_dirty.record(slot, 0, 0x8400_1000);
    try std.testing.expectEqual(@as(u64, 1), kernel.page_view_dirty.word(slot, 0));
}

test "page view dirty partial copyout failure retains full inventory until successful retry" {
    var physical_fixture = try physicalFixture();
    defer physical_fixture.deinit();
    var s: KernelState = undefined;
    try initFdState(&s);
    const parent = try s.createNativeVmo(.anonymous, 4096);
    try s.retainNativeVmo(parent);
    try s.installNativeVmoPages(parent, 0, &.{0x8400_0000}, null);
    const view = try s.createNativePageView(parent, 4097);
    s.nativeVmoSlot(view).?.page_view_sparse = true;
    try s.installNativeVmoPages(view, 0, &.{0x8400_0000}, null);
    try s.installNativeVmoPages(view, 4096, &.{0x8400_0000}, null);
    const slot = s.nativeVmoSlot(view).?;
    kernel.page_view_dirty.record(slot, 0, 0x8400_0000);
    kernel.page_view_dirty.record(slot, 4096, 0x8400_0000);
    const Sink = struct {
        fail_after_prefix: bool,
        calls: usize = 0,
        bitmap: [65]u64 = @splat(0),
        pub fn write(sink: *@This(), first: usize, values: []const u64) bool {
            sink.calls += 1;
            if (sink.fail_after_prefix and first != 0) return false;
            @memcpy(sink.bitmap[first..][0..values.len], values);
            return true;
        }
    };
    var failing = Sink{ .fail_after_prefix = true };
    try std.testing.expect(!kernel.page_view_dirty.deliver(slot, &failing));
    try std.testing.expectEqual(@as(usize, 2), failing.calls);
    try std.testing.expectEqual(@as(u64, 1), failing.bitmap[0]);
    try std.testing.expectEqual(@as(u64, 1), kernel.page_view_dirty.word(slot, 0));
    try std.testing.expectEqual(@as(u64, 1), kernel.page_view_dirty.word(slot, 64));
    var retry = Sink{ .fail_after_prefix = false };
    try std.testing.expect(kernel.page_view_dirty.deliver(slot, &retry));
    try std.testing.expectEqual(@as(u64, 1), retry.bitmap[0]);
    try std.testing.expectEqual(@as(u64, 1), retry.bitmap[64]);
    try std.testing.expectEqual(@as(u64, 0), kernel.page_view_dirty.word(slot, 0));
    try std.testing.expectEqual(@as(u64, 0), kernel.page_view_dirty.word(slot, 64));
}
