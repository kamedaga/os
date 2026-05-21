const abi_root = @import("kernel_abi_root");
const image_abi = abi_root.image_abi;
const kernel = @import("../kernel.zig");

pub const CopyUserBytesFn = *const fn (kernel.PrincipalId, u64, []u8) bool;

pub const CopyUserRangeConfig = struct {
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    proc: kernel.PrincipalId,
    base_va: u64,
    size_bytes: u64,
    out_pages: *[kernel.max_image_backing_pages]u64,
    copy_user_bytes_from_va: CopyUserBytesFn,
};

pub const CopiedVmObjectPages = struct {
    page_count: usize,
    page_offset_bytes: u16,
};

pub fn parseVmObjectRights(bits: u64) kernel.VmObjectRights {
    const abi_rights = image_abi.vmObjectRightsFromBits(bits);
    return @bitCast(abi_rights);
}

pub fn releaseCopiedVmObjectPages(free_list: *kernel.FreePageList, pages: []const u64) void {
    for (pages) |paddr| {
        free_list.appendPage(0, paddr) catch {};
    }
}

pub fn copyUserRangeIntoVmObjectPages(config: CopyUserRangeConfig) ?CopiedVmObjectPages {
    if (config.size_bytes == 0) return null;
    const page_offset_bytes: u16 = @intCast(config.base_va & 0xFFF);
    const span_bytes = (@as(u64, page_offset_bytes) + config.size_bytes + 4095) & ~@as(u64, 4095);
    const page_count_u64 = span_bytes / 4096;
    if (page_count_u64 == 0 or page_count_u64 > kernel.max_image_backing_pages) return null;
    const page_count: usize = @intCast(page_count_u64);

    var i: usize = 0;
    while (i < page_count) : (i += 1) {
        const cap = config.state.allocPage(config.proc, config.free_list) catch {
            releaseCopiedVmObjectPages(config.free_list, config.out_pages[0..i]);
            return null;
        };
        const page: [*]u8 = @ptrFromInt(cap.paddr);
        @memset(page[0..4096], 0);
        config.out_pages[i] = cap.paddr;
    }

    var copied: u64 = 0;
    while (copied < config.size_bytes) {
        const absolute = @as(u64, page_offset_bytes) + copied;
        const page_index: usize = @intCast(absolute / 4096);
        const page_off: usize = @intCast(absolute & 0xFFF);
        const page_remaining: u64 = 4096 - @as(u64, @intCast(page_off));
        const remaining = config.size_bytes - copied;
        const chunk_u64 = if (remaining < page_remaining) remaining else page_remaining;
        const chunk_len: usize = @intCast(chunk_u64);
        const dst: [*]u8 = @ptrFromInt(config.out_pages[page_index] + @as(u64, @intCast(page_off)));
        if (!config.copy_user_bytes_from_va(config.proc, config.base_va + copied, dst[0..chunk_len])) {
            releaseCopiedVmObjectPages(config.free_list, config.out_pages[0..page_count]);
            return null;
        }
        copied += chunk_u64;
    }

    return .{
        .page_count = page_count,
        .page_offset_bytes = page_offset_bytes,
    };
}
