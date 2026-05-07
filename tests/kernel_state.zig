const std = @import("std");
const kernel = @import("kernel");
const device_capabilities = kernel.device_capabilities;
const cap_transfer_abi = @import("kernel_abi_root").cap_transfer_abi;

const KernelState = kernel.KernelState;
const DmaMappingState = kernel.DmaMappingState;
const KernelError = kernel.KernelError;
const OwnershipView = kernel.OwnershipView;
const FreePageList = kernel.FreePageList;
const EndpointCNode = kernel.EndpointCNode;
const PrincipalId = kernel.PrincipalId;
const processPrincipalFromIndex = kernel.processPrincipalFromIndex;
const initial_process_count = kernel.initial_process_count;
const default_process_principal: PrincipalId = kernel.processPrincipalFromIndex(0) orelse unreachable;

fn installProcessDmaCapForTest(s: *KernelState, paddr: u64) !void {
    try s.installCap(.Process0, paddr, .{
        .cpu_read = true,
        .cpu_write = true,
        .dma = true,
        .grant = true,
    });
}

test "dma mapping manager stage1 create state and release" {
    var s = KernelState.initPhase1();
    try installProcessDmaCapForTest(&s, 0x4000);
    const token = try s.dmaMapCreateStage1(
        .Process0,
        0x1001,
        0x4000,
        4096,
        .bidirectional,
    );
    const mapping = s.dmaMapFindStage1(token).?;
    try std.testing.expectEqual(@as(u64, 0x4000), mapping.paddr_start);
    try std.testing.expectEqual(@as(u64, 4096), mapping.length);
    try std.testing.expectEqual(DmaMappingState.mapped, mapping.state);

    try s.dmaMapSetStateStage1(token, .in_flight);
    try std.testing.expectEqual(DmaMappingState.in_flight, s.dmaMapFindStage1(token).?.state);
    try s.dmaMapSetStateStage1(token, .completed);
    try std.testing.expectEqual(DmaMappingState.completed, s.dmaMapFindStage1(token).?.state);

    try s.dmaMapReleaseStage1(token);
    try std.testing.expect(s.dmaMapFindStage1(token) == null);
}

test "dma mapping manager stage1 device domain bind" {
    var s = KernelState.initPhase1();
    try s.dmaBindDeviceDomainStage1(0x1001, 1);
    try std.testing.expectEqual(@as(?u32, 1), s.dmaDeviceDomainStage1(0x1001));
}

test "queue cap stage2 authorize submit and notify" {
    var s = KernelState.initPhase1();
    const submit_token = try device_capabilities.queueCapGrantStage2(&s, .Process1, 0x1001, 0, true, false);
    const notify_token = try device_capabilities.queueCapGrantStage2(&s, .Process1, 0x1001, 0, false, true);

    try device_capabilities.queueCapAuthorizeStage2(&s, .Process1, submit_token, 0, .submit);
    try device_capabilities.queueCapAuthorizeStage2(&s, .Process1, notify_token, 0, .notify);

    try std.testing.expectError(KernelError.InvalidState, device_capabilities.queueCapAuthorizeStage2(&s, .Process1, submit_token, 0, .notify));
    try std.testing.expectError(KernelError.InvalidState, device_capabilities.queueCapAuthorizeStage2(&s, .Process1, notify_token, 0, .submit));
}

test "queue cap stage2 rejects owner mismatch" {
    var s = KernelState.initPhase1();
    const token = try device_capabilities.queueCapGrantStage2(&s, .Process1, 0x1001, 0, true, true);
    try std.testing.expectError(KernelError.InvalidState, device_capabilities.queueCapAuthorizeStage2(&s, .Process0, token, 0, .submit));
}

test "device queue cap revoke removes descendants" {
    var s = KernelState.initPhase1();
    const root = try device_capabilities.queueCapGrantStage2(&s, .Process1, 0x1001, 0, true, true);
    const child = try device_capabilities.grantQueueCapStage2(&s, .Process1, .Process2, root);

    try device_capabilities.queueCapAuthorizeStage2(&s, .Process2, child, 0, .submit);
    try device_capabilities.revokeDeviceCapStage2(&s, .Process1, .virtqueue, root);

    try std.testing.expectError(KernelError.CapabilityNotFound, device_capabilities.queueCapAuthorizeStage2(&s, .Process1, root, 0, .submit));
    try std.testing.expectError(KernelError.CapabilityNotFound, device_capabilities.queueCapAuthorizeStage2(&s, .Process2, child, 0, .submit));
}

test "command cap derive subset preserves lineage and revoke" {
    var s = KernelState.initPhase1();
    const full_mask = commandOpcodeBitForTest(.blk_read) |
        commandOpcodeBitForTest(.blk_write) |
        commandOpcodeBitForTest(.blk_flush);
    const read_mask = commandOpcodeBitForTest(.blk_read);
    const root = try device_capabilities.commandCapGrantStage2(&s, .Process1, 0x1002, full_mask);
    const subset = try device_capabilities.deriveCommandCapStage2(&s, .Process1, root, read_mask);
    const child = try device_capabilities.grantCommandCapStage2(&s, .Process1, .Process2, subset);

    try device_capabilities.commandCapAuthorizeStage2(&s, .Process1, subset, 0x1002, .blk_read);
    try std.testing.expectError(KernelError.InvalidState, device_capabilities.commandCapAuthorizeStage2(&s, .Process1, subset, 0x1002, .blk_write));
    try device_capabilities.commandCapAuthorizeStage2(&s, .Process2, child, 0x1002, .blk_read);

    try device_capabilities.revokeDeviceCapStage2(&s, .Process1, .command, subset);

    try std.testing.expectError(KernelError.CapabilityNotFound, device_capabilities.commandCapAuthorizeStage2(&s, .Process1, subset, 0x1002, .blk_read));
    try std.testing.expectError(KernelError.CapabilityNotFound, device_capabilities.commandCapAuthorizeStage2(&s, .Process2, child, 0x1002, .blk_read));
    try device_capabilities.commandCapAuthorizeStage2(&s, .Process1, root, 0x1002, .blk_write);
}

test "gpu command cap isolates virgl submit from scanout" {
    var s = KernelState.initPhase1();
    const full_mask = commandOpcodeBitForTest(.gpu_admin) |
        commandOpcodeBitForTest(.gpu_virgl_context) |
        commandOpcodeBitForTest(.gpu_virgl_resource) |
        commandOpcodeBitForTest(.gpu_virgl_submit) |
        commandOpcodeBitForTest(.gpu_fence);
    const submit_mask = commandOpcodeBitForTest(.gpu_virgl_submit) |
        commandOpcodeBitForTest(.gpu_fence);
    const root = try device_capabilities.commandCapGrantStage2(&s, .Process1, 0x1001, full_mask);
    const submit_only = try device_capabilities.deriveCommandCapStage2(&s, .Process1, root, submit_mask);

    try device_capabilities.commandCapAuthorizeStage2(&s, .Process1, submit_only, 0x1001, .gpu_virgl_submit);
    try device_capabilities.commandCapAuthorizeStage2(&s, .Process1, submit_only, 0x1001, .gpu_fence);
    try std.testing.expectError(KernelError.InvalidState, device_capabilities.commandCapAuthorizeStage2(&s, .Process1, submit_only, 0x1001, .gpu_scanout));
    try std.testing.expectError(KernelError.InvalidState, device_capabilities.commandCapAuthorizeStage2(&s, .Process1, submit_only, 0x1002, .gpu_virgl_submit));
}

fn commandOpcodeBitForTest(opcode: device_capabilities.CommandOpcodeClass) u64 {
    return @as(u64, 1) << @as(u6, @intCast(@intFromEnum(opcode)));
}

test "dma mapping manager stage1 rejects invalid transition" {
    var s = KernelState.initPhase1();
    try installProcessDmaCapForTest(&s, 0x5000);
    const token = try s.dmaMapCreateStage1(
        .Process0,
        0x1001,
        0x5000,
        4096,
        .bidirectional,
    );

    try std.testing.expectError(KernelError.InvalidState, s.dmaMapSetStateStage1(token, .completed));
    try std.testing.expectEqual(DmaMappingState.mapped, s.dmaMapFindStage1(token).?.state);
}

test "dma mapping manager stage1 release requires completed" {
    var s = KernelState.initPhase1();
    try installProcessDmaCapForTest(&s, 0x6000);
    const token = try s.dmaMapCreateStage1(
        .Process0,
        0x1001,
        0x6000,
        4096,
        .bidirectional,
    );

    try std.testing.expectError(KernelError.InvalidState, s.dmaMapReleaseStage1(token));
    try s.dmaMapSetStateStage1(token, .in_flight);
    try s.dmaMapSetStateStage1(token, .completed);
    try s.dmaMapReleaseStage1(token);
    try std.testing.expect(s.dmaMapFindStage1(token) == null);
}
test "phase1 init state" {
    const s = KernelState.initPhase1();

    try std.testing.expectEqual(@as(usize, 1), s.region_len);
    try std.testing.expectEqualDeep(OwnershipView{ .owner = default_process_principal }, s.scanCapTables(0x1000));

    const p0 = s.getTableConst(.Process0);
    try std.testing.expectEqual(@as(usize, 1), p0.len);
    try std.testing.expect(p0.find(0x1000).?.rights.cpu_read);
    try std.testing.expect(p0.find(0x1000).?.rights.cpu_write);
    try std.testing.expect(p0.find(0x1000).?.rights.dma);
    try std.testing.expect(p0.find(0x1000).?.rights.grant);

    const dev = s.getTableConst(.Device0);
    try std.testing.expectEqual(@as(usize, 0), dev.len);
}

test "start dma moves owner and capabilities" {
    var s = KernelState.initPhase1();
    try s.startDma(default_process_principal, 0x1000);

    try std.testing.expectEqualDeep(OwnershipView{ .owner = .Device0 }, s.scanCapTables(0x1000));
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    const dev_cap = s.getTableConst(.Device0).find(0x1000).?;
    try std.testing.expect(dev_cap.rights.dma);
    try std.testing.expect(!dev_cap.rights.cpu_read);
    try std.testing.expect(!dev_cap.rights.cpu_write);
}

test "complete dma returns owner and capabilities" {
    var s = KernelState.initPhase1();
    try s.startDma(default_process_principal, 0x1000);
    try s.completeDma(0x1000);

    try std.testing.expectEqualDeep(OwnershipView{ .owner = default_process_principal }, s.scanCapTables(0x1000));
    try std.testing.expect(s.getTableConst(.Device0).find(0x1000) == null);
    const p0_cap = s.getTableConst(.Process0).find(0x1000).?;
    try std.testing.expect(p0_cap.rights.cpu_read);
    try std.testing.expect(p0_cap.rights.cpu_write);
    try std.testing.expect(p0_cap.rights.dma);
}

test "complete dma restores original rights" {
    var s = KernelState.initPhase1();
    try s.moveCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });
    try s.moveCap(.Process1, .Process0, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });

    try s.startDma(default_process_principal, 0x1000);
    try s.completeDma(0x1000);

    const p0_cap = s.getTableConst(.Process0).find(0x1000).?;
    try std.testing.expect(p0_cap.rights.cpu_read);
    try std.testing.expect(!p0_cap.rights.cpu_write);
    try std.testing.expect(p0_cap.rights.dma);
}

test "invalid transition rejected" {
    var s = KernelState.initPhase1();
    try s.installCap(.Device0, 0x1000, .{ .cpu_read = false, .cpu_write = false, .dma = true });
    try std.testing.expectError(KernelError.InvalidState, s.completeDma(0x1000));

    var s2 = KernelState.initPhase1();
    try s2.startDma(default_process_principal, 0x1000);
    try std.testing.expectError(KernelError.CapabilityNotFound, s2.startDma(default_process_principal, 0x1000));
}

test "init from detected regions" {
    const s = try KernelState.initFromDetectedRegions(3);
    try std.testing.expectEqual(@as(usize, 3), s.region_len);
    try std.testing.expectEqualDeep(OwnershipView{ .none = {} }, s.scanCapTables(0x3000));
    try std.testing.expectEqual(@as(usize, 0), s.getTableConst(.Process0).len);
}

test "free page list append region" {
    var free_list = FreePageList{};
    try free_list.appendRegion(0, 0x1000, 3);

    try std.testing.expectEqual(@as(usize, 3), free_list.len);
    try std.testing.expectEqual(@as(usize, 1), free_list.range_len);
    try std.testing.expectEqual(@as(u64, 0), free_list.ranges[0].region_id);
    try std.testing.expectEqual(@as(u64, 0x1000), free_list.ranges[0].physical_start);
    try std.testing.expectEqual(@as(usize, 3), free_list.ranges[0].len);
}

test "free page list splits range when non-contiguous" {
    var free_list = FreePageList{};
    try free_list.appendPage(0, 0x1000);
    try free_list.appendPage(0, 0x2000);
    try free_list.appendPage(0, 0x5000);

    try std.testing.expectEqual(@as(usize, 3), free_list.len);
    try std.testing.expectEqual(@as(usize, 2), free_list.range_len);
    try std.testing.expectEqual(@as(usize, 2), free_list.ranges[0].len);
    try std.testing.expectEqual(@as(usize, 1), free_list.ranges[1].len);
}

test "free page list pop front updates ranges" {
    var free_list = FreePageList{};
    try free_list.appendRegion(0, 0x1000, 3);
    try std.testing.expectEqual(@as(u64, 0x1000), try free_list.popFront());
    try std.testing.expectEqual(@as(usize, 2), free_list.len);
    try std.testing.expectEqual(@as(usize, 1), free_list.range_len);
    try std.testing.expectEqual(@as(u64, 0x2000), free_list.ranges[0].physical_start);
    try std.testing.expectEqual(@as(usize, 2), free_list.ranges[0].len);
}

test "alloc page to principal installs capability by paddr" {
    var s = try KernelState.initFromDetectedRegions(1);
    var free_list = FreePageList{};
    try free_list.appendPage(0, 0x9000);
    const cap = try s.allocPageTo(.Process0, &free_list);
    try std.testing.expectEqual(@as(u64, 0x9000), cap.paddr);
    const installed = s.getTableConst(.Process0).find(0x9000).?;
    try std.testing.expect(installed.rights.grant);
}

test "allocPageTo table full does not consume a free page" {
    var s = try KernelState.initFromDetectedRegions(1);
    var free_list = FreePageList{};
    try free_list.appendPage(0, 0x9000);
    const table = s.getTable(.Process0);
    while (table.len < table.caps.len) {
        const id: u64 = @intCast(table.len + 1);
        try table.addAssumeFresh(.{
            .paddr = 0x1000_0000 + id * 0x1000,
            .rights = .{
                .cpu_read = true,
                .cpu_write = true,
                .dma = true,
                .grant = true,
            },
            .cap_id = id,
            .root_cap_id = id,
            .parent_cap_id = 0,
        });
    }

    try std.testing.expectError(KernelError.TableFull, s.allocPageTo(.Process0, &free_list));
    try std.testing.expectEqual(@as(usize, 1), free_list.len);
    try std.testing.expectEqual(@as(u64, 0x9000), try free_list.popFront());
}

test "endpoint tables clamp corrupted length during lookup" {
    var endpoints = EndpointCNode{};
    endpoints.len = endpoints.caps.len + 64;
    for (&endpoints.caps, 0..) |*cap, index| {
        cap.* = .{
            .endpoint_id = @as(u64, @intCast(index + 1)),
            .target = .Process1,
        };
    }
    endpoints.caps[endpoints.caps.len - 1] = .{
        .endpoint_id = 0x90,
        .target = .Process2,
    };

    const found = endpoints.find(0x90) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(PrincipalId.Process2, found.target);
    try std.testing.expect(endpoints.find(0x91) == null);
}

test "allocPageTo skips paddr that already exists in another cap table" {
    var s = try KernelState.initFromDetectedRegions(1);
    var free_list = FreePageList{};
    try free_list.appendPage(0, 0x1000);
    try free_list.appendPage(0, 0x3000);
    try s.installCap(.Process1, 0x3000, .{
        .cpu_read = true,
        .cpu_write = true,
        .dma = true,
    });

    const cap = try s.allocPageTo(.Process0, &free_list);
    try std.testing.expectEqual(@as(u64, 0x1000), cap.paddr);
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) != null);
}

test "moveCap enforces single holder" {
    var s = KernelState.initPhase1();
    try s.moveCap(.Process0, .Device0, 0x1000, .{ .cpu_read = false, .cpu_write = false, .dma = true });

    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    try std.testing.expect(s.getTableConst(.Device0).find(0x1000) != null);
    try std.testing.expectEqualDeep(OwnershipView{ .owner = .Device0 }, s.scanCapTables(0x1000));
}

test "moveCap rejects rights escalation" {
    var s = KernelState.initPhase1();
    try s.moveCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    });
    try std.testing.expectError(KernelError.InvalidState, s.moveCap(.Process1, .Device0, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    }));
}

test "moveCap rejects cpu rights when moving to Device0" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.InvalidState, s.moveCap(.Process0, .Device0, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    }));
}

test "sendCap moves capability process to process with rights preserved" {
    var s = KernelState.initPhase1();
    try s.sendCap(.Process0, .Process1, 0x1000);

    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    const p1_cap = s.getTableConst(.Process1).find(0x1000).?;
    try std.testing.expect(p1_cap.rights.cpu_read);
    try std.testing.expect(p1_cap.rights.cpu_write);
    try std.testing.expect(p1_cap.rights.dma);
    try std.testing.expect(p1_cap.rights.grant);
}

test "sendCap rejects non-process endpoints" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.InvalidState, s.sendCap(.Process0, .Device0, 0x1000));
    try std.testing.expectError(KernelError.InvalidState, s.sendCap(.Device0, .Process1, 0x1000));
}

test "sendCapOnEndpoint requires endpoint capability" {
    var s = KernelState.initPhase1();
    try s.installEndpoint(.Process0, 0x11, .Process1);
    try s.sendCapOnEndpoint(.Process0, 0x11, 0x1000);
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) != null);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) == null);
    const transfer_id = try s.recvCap(.Process1);
    try std.testing.expect(transfer_id >= 0x1000);
    try std.testing.expectEqual(@as(u64, 0x1000), try s.acceptCapTransfer(.Process1, transfer_id));
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) != null);
}

test "iommu no-cap-driver shadow maps active dma mapping" {
    var s = KernelState.initPhase1();
    s.setIommuNoCapDriverMode(.shadow);
    _ = try device_capabilities.iommuCapGrantStage2(&s, .Process1, 0x1001, true, true, true);
    _ = try device_capabilities.queueCapGrantStage2(&s, .Process1, 0x1001, 0, true, false);

    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));

    const mapping = try s.dmaMapCreateStage1(.Process1, 0x1001, 0x1000, 128, .read);
    try std.testing.expect(device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));

    try s.dmaMapSetStateStage1(mapping, .in_flight);
    try s.dmaMapSetStateStage1(mapping, .completed);
    try s.dmaMapReleaseStage1(mapping);
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));

    const mapping2 = try s.dmaMapCreateStage1(.Process1, 0x1001, 0x1000, 128, .read);
    try std.testing.expect(device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));
    try s.revokeCapTree(.Process1, 0x1000);
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));
    try std.testing.expect(s.dmaMapFindStage1(mapping2) == null);
}

test "iommu no-cap-driver does not map non-dma grant" {
    var s = KernelState.initPhase1();
    s.setIommuNoCapDriverMode(.shadow);
    _ = try device_capabilities.iommuCapGrantStage2(&s, .Process1, 0x1001, true, true, true);
    _ = try device_capabilities.queueCapGrantStage2(&s, .Process1, 0x1001, 0, true, false);

    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    });
    try std.testing.expectError(KernelError.NoDmaRight, s.dmaMapCreateStage1(.Process1, 0x1001, 0x1000, 128, .read));
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));
}

test "queue cap grant syncs existing dma mapping" {
    var s = KernelState.initPhase1();
    s.setIommuNoCapDriverMode(.shadow);
    _ = try device_capabilities.iommuCapGrantStage2(&s, .Process1, 0x1001, true, true, true);

    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });
    const mapping = try s.dmaMapCreateStage1(.Process1, 0x1001, 0x1000, 128, .read);
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));

    const queue_token = try device_capabilities.queueCapGrantStage2(&s, .Process1, 0x1001, 0, true, false);
    try std.testing.expect(device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));

    try device_capabilities.revokeDeviceCapStage2(&s, .Process1, .virtqueue, queue_token);
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));
    try std.testing.expect(s.dmaMapFindStage1(mapping) == null);
}

test "sendCapOnEndpoint rejects missing endpoint" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.EndpointNotFound, s.sendCapOnEndpoint(.Process0, 0xDEAD, 0x1000));
}

test "sendCapOnEndpoint enqueues mailbox for target" {
    var s = KernelState.initPhase1();
    try s.installEndpoint(.Process0, 0x11, .Process1);
    try s.sendCapOnEndpoint(.Process0, 0x11, 0x1000);
    const transfer_id = try s.recvCap(.Process1);
    try std.testing.expect(transfer_id >= cap_transfer_abi.transfer_id_min);
    try std.testing.expectEqual(transfer_id, try s.recvCap(.Process1));
    try std.testing.expectEqual(@as(u64, 0x1000), try s.acceptCapTransfer(.Process1, transfer_id));
}

test "acceptCapTransfer rejects mismatched transfer token" {
    var s = KernelState.initPhase1();
    try s.installEndpoint(.Process0, 0x11, .Process1);
    try s.sendCapOnEndpoint(.Process0, 0x11, 0x1000);
    const transfer_id = try s.recvCap(.Process1);
    try std.testing.expect(transfer_id >= cap_transfer_abi.transfer_id_min);
    try std.testing.expectError(KernelError.InvalidState, s.acceptCapTransfer(.Process1, transfer_id + 1));
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) != null);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) == null);
    try std.testing.expectEqual(transfer_id, try s.recvCap(.Process1));
    try std.testing.expectEqual(@as(u64, 0x1000), try s.acceptCapTransfer(.Process1, transfer_id));
}

test "recvCap returns MailboxEmpty when queue is empty" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.MailboxEmpty, s.recvCap(.Process1));
}

test "grantCap creates child and revokeCapTree at root removes descendants" {
    var s = KernelState.initPhase1();
    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) != null);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) != null);

    try s.revokeCapTree(.Process0, 0x1000);
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) == null);
}

test "grantCap requires grant right on source page capability" {
    var s = KernelState.initPhase1();
    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    });

    try std.testing.expectError(KernelError.InvalidState, s.grantCap(.Process1, .Process2, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    }));
}

test "sendCapOnEndpoint moves derived page capability after receiver accepts" {
    var s = KernelState.initPhase1();
    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    });
    try s.installEndpoint(.Process1, 0x11, .Process2);

    try s.sendCapOnEndpoint(.Process1, 0x11, 0x1000);
    const transfer_id = try s.recvCap(.Process2);
    try std.testing.expectEqual(@as(u64, 0x1000), try s.acceptCapTransfer(.Process2, transfer_id));
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) == null);
    const received = s.getTableConst(.Process2).find(0x1000).?;
    try std.testing.expect(received.rights.cpu_read);
    try std.testing.expect(!received.rights.cpu_write);
    try std.testing.expect(!received.rights.grant);
}

test "shareCapOnEndpoint keeps sender capability after receiver accepts" {
    var s = KernelState.initPhase1();
    try s.installEndpoint(.Process0, 0x11, .Process1);
    try s.shareCapOnEndpoint(.Process0, 0x11, 0x1000);
    const transfer_id = try s.recvCap(.Process1);
    try std.testing.expectEqual(@as(u64, 0x1000), try s.acceptCapTransfer(.Process1, transfer_id));
    const sender = s.getTableConst(.Process0).find(0x1000).?;
    const receiver = s.getTableConst(.Process1).find(0x1000).?;
    try std.testing.expect(sender.rights.cpu_read);
    try std.testing.expect(sender.rights.cpu_write);
    try std.testing.expect(sender.rights.grant);
    try std.testing.expect(receiver.rights.cpu_read);
    try std.testing.expect(receiver.rights.cpu_write);
    try std.testing.expect(receiver.rights.grant);
}

test "revokeCapTree from child only removes child subtree" {
    var s = KernelState.initPhase1();
    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });

    try s.revokeCapTree(.Process1, 0x1000);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) == null);
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) != null);
}

test "ensureProcessDescriptor updates label for reserved slot" {
    var s = KernelState.initPhase1();
    const fs_owner = processPrincipalFromIndex(initial_process_count) orelse unreachable;
    try std.testing.expect(!s.isActiveProcess(fs_owner));
    try std.testing.expect(s.ensureProcessDescriptor(fs_owner, "bootstrap-fs"));
    try std.testing.expect(s.isActiveProcess(fs_owner));
    try std.testing.expectEqualStrings("bootstrap-fs", s.processDescriptor(fs_owner).?.label);
    try std.testing.expect(s.ensureProcessDescriptor(.Process0, "bootstrap-owner"));
    try std.testing.expectEqualStrings("bootstrap-owner", s.processDescriptor(.Process0).?.label);
}

test "markProcessFaulted records fault status" {
    var s = KernelState.initPhase1();

    try std.testing.expect(s.markProcessFaulted(.Process1, 14));
    const status = s.processStatus(.Process1);
    try std.testing.expect(!status.active);
    try std.testing.expect(status.faulted);
    try std.testing.expectEqual(@as(u8, 14), status.fault_vector);
}

test "endpointTargetFor ignores inactive faulted target" {
    var s = KernelState.initPhase1();

    try s.installEndpoint(.Process0, 0x11, .Process1);
    try std.testing.expectEqual(@as(?PrincipalId, .Process1), s.endpointTargetFor(.Process0, 0x11));
    try std.testing.expect(s.markProcessFaulted(.Process1, 13));
    try std.testing.expectEqual(@as(?PrincipalId, null), s.endpointTargetFor(.Process0, 0x11));
}

test "endpointTargetFor falls back to published service endpoint" {
    var s = KernelState.initPhase1();

    try s.publishServiceEndpoint(0x80, .Process1);
    try std.testing.expectEqual(@as(?PrincipalId, .Process1), s.endpointTargetFor(.Process0, 0x80));
}

test "unpublishServiceEndpointsForTarget removes published endpoint" {
    var s = KernelState.initPhase1();

    try s.publishServiceEndpoint(0x80, .Process1);
    try std.testing.expect(s.unpublishServiceEndpointsForTarget(.Process1));
    try std.testing.expectEqual(@as(?PrincipalId, null), s.endpointTargetFor(.Process0, 0x80));
}
