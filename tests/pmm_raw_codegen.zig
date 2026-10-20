// Emit the same raw PMM entry against both source trees. The production ELF
// may inline this API, so a missing standalone symbol is not a cost comparison.
const kernel = @import("kernel");

export fn pmmRawAllocate(list: *kernel.FreePageList, count: usize, min: u64, max: u64) u64 {
    return list.popContiguousInRange(count, min, max) catch 0;
}
