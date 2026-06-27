pub fn kernelStaticStorageStartAddr() usize {
    return 0;
}

pub fn kernelStaticStorageEndAddr() usize {
    return 0;
}

pub fn isActive() bool {
    return false;
}

pub fn isAddressableRange(paddr: u64, size: u64) bool {
    _ = paddr;
    _ = size;
    return true;
}

pub fn mapRange(iova: u64, paddr: u64, size: u64) bool {
    _ = iova;
    _ = paddr;
    _ = size;
    return true;
}

pub fn unmapRange(iova: u64, size: u64) void {
    _ = iova;
    _ = size;
}
