//! Kernel-facing DMA translation boundary. Device drivers see the existing
//! capsule ABI; the kernel owns translation and the backing-page drain ledger.
const vtd = @import("vtd.zig");
const amd = @import("amd_iommu.zig");
const ivrs = @import("acpi_ivrs.zig");
const types = @import("state/types.zig");
const std = @import("std");

pub const DeviceIovaWindow = struct { start: u64 = 0, size: u64 = 0 };
pub const DmaEnableError = error{ Unsupported, NoDevice, Quarantined, DrainFailed };

pub fn kernelStaticStorageStartAddr() usize {
    return @min(vtd.kernelStaticStorageStartAddr(), amd.kernelStaticStorageStartAddr());
}

pub fn kernelStaticStorageEndAddr() usize {
    return @max(vtd.kernelStaticStorageEndAddr(), amd.kernelStaticStorageEndAddr());
}

/// Run only on the BSP after installing kernel page tables and before APs.
/// It does not enable DMA translation or touch an IOMMU register.
pub fn prepareBootMmio(rsdp_paddr: u64) bool {
    return amd.prepareBootMmio(rsdp_paddr);
}

pub fn init(rsdp_paddr: u64, free_list: *types.FreePageList, comptime checkpoint: fn (u8) void) void {
    if (ivrs.find(rsdp_paddr) != null) {
        checkpoint('I');
        if (!amd.init(rsdp_paddr, free_list, checkpoint))
            @import("halt.zig").haltWithMessage("AMD IOMMU initialization failed");
        return;
    }
    vtd.init(rsdp_paddr, free_list);
}

pub fn isActive() bool {
    return amd.isActive() or vtd.isActive();
}

pub fn deviceIovaWindow(device: types.DmaDeviceId) DeviceIovaWindow {
    if (amd.isActive()) {
        const window = amd.deviceIovaWindow(device);
        return .{ .start = window.start, .size = window.size };
    }
    const window = vtd.deviceIovaWindow(device);
    return .{ .start = window.start, .size = window.size };
}

pub fn allocIova(device: types.DmaDeviceId, page_count: usize) ?u64 {
    if (!isActive()) return null;
    if (amd.isActive()) return amd.allocIova(device, page_count);
    return vtd.allocIova(device, page_count);
}

pub fn reserveIova(device: types.DmaDeviceId, iova: u64, page_count: usize) bool {
    if (!isActive()) return false;
    if (amd.isActive()) return amd.reserveIova(device, iova, page_count);
    return vtd.reserveIova(device, iova, page_count);
}

pub fn freeIova(device: types.DmaDeviceId, iova: u64, page_count: usize) void {
    if (amd.isActive()) return amd.freeIova(device, iova, page_count);
    vtd.freeIova(device, iova, page_count);
}

pub fn mapPages(
    device: types.DmaDeviceId,
    iova: u64,
    paddrs: []const u64,
    readable: bool,
    writable: bool,
) bool {
    if (!isActive()) return false;
    if (amd.isActive()) return amd.mapPages(device, iova, paddrs, readable, writable);
    return vtd.mapPages(device, iova, paddrs, readable, writable);
}

pub fn unmapRangeForDevice(device: types.DmaDeviceId, iova: u64, size: u64) bool {
    if (!isActive()) return false;
    if (amd.isActive()) return amd.unmapRangeForDevice(device, iova, size);
    return vtd.unmapRangeForDevice(device, iova, size);
}

pub fn setDeviceDmaEnabled(device: types.DmaDeviceId, enabled: bool) DmaEnableError!void {
    if (amd.isActive()) return amd.setDeviceDmaEnabled(device, enabled);
    return vtd.setDeviceDmaEnabled(device, enabled);
}

pub fn deviceQuarantined(device: types.DmaDeviceId) bool {
    if (amd.isActive()) return amd.deviceQuarantined(device);
    return vtd.deviceQuarantined(device);
}

pub fn physicalRangeQuarantined(paddr: u64, page_count: usize) bool {
    // A failed drain retains backing pages even after the FD and process die.
    // This query must remain valid independently of an active DMA mapping.
    return amd.physicalRangeQuarantined(paddr, page_count) or vtd.physicalRangeQuarantined(paddr, page_count);
}

pub fn dmaSnapshotFlags(device: types.DmaDeviceId) u64 {
    if (amd.isActive()) return amd.dmaSnapshotFlags(device);
    return vtd.dmaSnapshotFlags(device);
}

test "IOMMU boundary retains DMA backing after a failed drain" {
    const allocator = std.testing.allocator;
    const backing = try allocator.alignedAlloc(u8, .fromByteUnits(4096), 32 * 4096);
    defer allocator.free(backing);
    const free_list = try allocator.create(types.FreePageList);
    defer allocator.destroy(free_list);
    free_list.* = .{};
    try free_list.appendContiguousRange(0, @intFromPtr(backing.ptr), 32);

    const device: types.DmaDeviceId = 0x1001;
    try vtd.TestSupport.begin(free_list, device);
    defer vtd.TestSupport.end();
    const page = try free_list.popFront();
    const iova: u64 = 0x8000_0000;
    try std.testing.expect(isActive());
    try std.testing.expect(reserveIova(device, iova, 1));
    try std.testing.expect(mapPages(device, iova, &.{page}, true, true));

    vtd.TestSupport.failInvalidation(true);
    try std.testing.expect(!unmapRangeForDevice(device, iova, 4096));
    try std.testing.expect(deviceQuarantined(device));
    try std.testing.expect(physicalRangeQuarantined(page, 1));
    freeIova(device, iova, 1);
    try std.testing.expectEqual(@as(usize, 1), vtd.TestSupport.usedPages(device));
    try std.testing.expectError(types.KernelError.InvalidState, free_list.appendPage(0, page));
}
