const types = @import("../state/types.zig");
const layout = @import("../arch/x86_64/physical_layout.zig");

pub fn allocate(list: *types.FreePageList) types.KernelError!u64 {
    // Boot modules have no DMA32 constraint. Preserve scarce low RAM for
    // IOMMU metadata, while keeping staging directly readable after CR3.
    return list.popContiguousInRange(1, 1 << 32, layout.identity_limit) catch |err| switch (err) {
        error.OutOfFreePages => list.popContiguousBelow(1, layout.identity_limit),
        else => err,
    };
}
