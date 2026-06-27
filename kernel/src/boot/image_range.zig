pub var base_paddr: u64 = 0;
pub var virtual_base: u64 = 0;
pub var size_bytes: usize = 0;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn staticStorageStart(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr);
}

pub fn kernelStaticStorageStartAddr() usize {
    var start = staticStorageStart(@TypeOf(base_paddr), &base_paddr);
    const virtual_start = staticStorageStart(@TypeOf(virtual_base), &virtual_base);
    if (virtual_start < start) start = virtual_start;
    const size_start = staticStorageStart(@TypeOf(size_bytes), &size_bytes);
    if (size_start < start) start = size_start;
    return start;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end = staticStorageEnd(@TypeOf(base_paddr), &base_paddr);
    const virtual_end = staticStorageEnd(@TypeOf(virtual_base), &virtual_base);
    if (virtual_end > end) end = virtual_end;
    const size_end = staticStorageEnd(@TypeOf(size_bytes), &size_bytes);
    if (size_end > end) end = size_end;
    return end;
}
