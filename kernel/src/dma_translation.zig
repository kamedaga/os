const kernel = @import("kernel.zig");
const vtd = @import("vtd.zig");

fn staticStorageStart(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr);
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn minStaticStart(a: usize, b: usize) usize {
    return if (a < b) a else b;
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

var initialized: bool = false;

pub fn kernelStaticStorageStartAddr() usize {
    var start = staticStorageStart(@TypeOf(initialized), &initialized);
    start = minStaticStart(start, vtd.kernelStaticStorageStartAddr());
    return start;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end = staticStorageEnd(@TypeOf(initialized), &initialized);
    end = maxStaticEnd(end, vtd.kernelStaticStorageEndAddr());
    return end;
}

pub fn initHardware() void {
    initialized = vtd.init();
}

pub fn hardwareActive() bool {
    return initialized and vtd.isActive();
}

pub fn mapUserRange(
    device: kernel.DmaDeviceId,
    iova: u64,
    paddr: u64,
    size: u64,
) bool {
    _ = device;
    return vtd.mapRange(iova, paddr, size);
}

pub fn unmapUserRange(
    device: kernel.DmaDeviceId,
    iova: u64,
    size: u64,
) void {
    _ = device;
    vtd.unmapRange(iova, size);
}
