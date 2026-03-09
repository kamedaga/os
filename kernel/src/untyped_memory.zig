const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");

pub const BootstrapStats = struct {
    block_count: usize = 0,
    total_bytes: u64 = 0,
};

pub const AllocMapPagesError = error{
    InvalidArgument,
    AllocationFailed,
    MapFailed,
};

pub const UntypedAllocFlags = packed struct(u64) {
    contiguous_only: bool = false,
    dma_ok: bool = false,
    _reserved: u62 = 0,
};

pub const UntypedRetypeFlags = packed struct(u64) {
    writable: bool = false,
    drop_cap_after_map: bool = false,
    contiguous: bool = true,
    _reserved: u61 = 0,
};

pub const UntypedAllocMapFlags = packed struct(u64) {
    writable: bool = false,
    drop_cap_after_map: bool = false,
    contiguous: bool = true,
    dma_ok: bool = false,
    _reserved: u60 = 0,
};

pub const UntypedRetypePagesError = error{
    InvalidArgument,
    AllocationFailed,
    MapFailed,
};

pub const UntypedAllocMapPagesError = error{
    InvalidArgument,
    AllocationFailed,
    MapFailed,
};

pub const AllocMapPagesConfig = struct {
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    free_list: *kernel.FreePageList,
    base_va: u64,
    page_count: usize,
    writable: bool,
    drop_cap_after_map: bool,
    out_paddr_list_va: u64,
    write_user_u64: *const fn (principal: kernel.PrincipalId, dest_user_va: u64, value: u64) bool,
};

const bootstrap_min_untyped_pages: usize = 256;
const bootstrap_reserved_free_pages: usize = 2048;
const untyped_token_base: u64 = 0x1000;

pub fn bootstrapProcess0Untyped(
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
) kernel.KernelError!BootstrapStats {
    var stats: BootstrapStats = .{};
    var index = free_list.range_len;
    while (index > 0) {
        index -= 1;
        const range = free_list.ranges[index];
        if (range.len < bootstrap_min_untyped_pages) continue;
        if (free_list.len <= range.len) break;
        if ((free_list.len - range.len) < bootstrap_reserved_free_pages) continue;

        _ = try state.createUntypedBlock(
            .Process0,
            range.physical_start,
            @as(u64, range.len) * 4096,
            .{
                .contiguous_only = true,
                .dma_ok = true,
            },
        );
        stats.block_count += 1;
        stats.total_bytes += @as(u64, range.len) * 4096;
        removeRange(free_list, index);
    }
    return stats;
}

pub fn grantProcess0UntypedTo(
    state: *kernel.KernelState,
    target: kernel.PrincipalId,
) kernel.KernelError!usize {
    var granted: usize = 0;
    const process0_table = state.getUntypedTableConst(.Process0);
    var i: usize = 0;
    while (i < process0_table.len) : (i += 1) {
        const block_id = process0_table.caps[i].block_id;
        try state.grantUntypedCap(.Process0, target, block_id);
        granted += 1;
    }
    return granted;
}

pub fn allocUntyped(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    free_list: *kernel.FreePageList,
    bytes: u64,
    alignment: u64,
    flags: UntypedAllocFlags,
) kernel.KernelError!u64 {
    if (bytes == 0) return kernel.KernelError.InvalidState;
    const aligned_bytes = alignUp(bytes, 4096) orelse return kernel.KernelError.InvalidState;
    const eff_align = if (alignment <= 4096) @as(u64, 4096) else alignment;
    if ((eff_align & (eff_align - 1)) != 0) return kernel.KernelError.InvalidState;

    const block_start = try carveFreeRange(free_list, aligned_bytes, eff_align);
    const block_id = try state.createUntypedBlock(owner, block_start, aligned_bytes, .{
        .contiguous_only = flags.contiguous_only,
        .dma_ok = flags.dma_ok,
    });
    return encodeToken(block_id);
}

pub fn retypeMapPages(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    token: u64,
    base_va: u64,
    page_count: usize,
    flags_bits: u64,
    out_paddr_list_va: u64,
    write_user_u64: *const fn (principal: kernel.PrincipalId, dest_user_va: u64, value: u64) bool,
) UntypedRetypePagesError!void {
    if ((base_va & 0xFFF) != 0) return error.InvalidArgument;
    if (page_count == 0 or page_count > kernel.max_retype_page_batch) return error.InvalidArgument;
    const block_id = decodeToken(token) orelse return error.InvalidArgument;
    const flags: UntypedRetypeFlags = @bitCast(flags_bits);

    var caps: [kernel.max_retype_page_batch]kernel.PageCapability = undefined;
    state.retypeUntypedToPages(proc, block_id, page_count, flags.contiguous, caps[0..page_count]) catch |err| switch (err) {
        kernel.KernelError.InvalidState, kernel.KernelError.UntypedNotFound => return error.InvalidArgument,
        kernel.KernelError.OutOfFreePages, kernel.KernelError.TableFull => return error.AllocationFailed,
        else => return error.AllocationFailed,
    };

    var i: usize = 0;
    while (i < page_count) : (i += 1) {
        const cap = caps[i];
        const i_u64: u64 = @intCast(i);
        const offset_4k, const mul_overflow = @mulWithOverflow(i_u64, @as(u64, 4096));
        if (mul_overflow != 0) return error.MapFailed;
        const map_va, const va_overflow = @addWithOverflow(base_va, offset_4k);
        if (va_overflow != 0) return error.MapFailed;
        if (!capability.mapFreshUserPage(proc, map_va, cap.paddr, flags.writable)) {
            return error.MapFailed;
        }
        if (out_paddr_list_va != 0) {
            const offset_8, const list_mul_overflow = @mulWithOverflow(i_u64, @as(u64, 8));
            if (list_mul_overflow != 0) return error.InvalidArgument;
            const list_va, const list_va_overflow = @addWithOverflow(out_paddr_list_va, offset_8);
            if (list_va_overflow != 0) return error.InvalidArgument;
            if (!write_user_u64(proc, list_va, cap.paddr)) {
                return error.MapFailed;
            }
        }
        if (flags.drop_cap_after_map) {
            _ = state.getTable(proc).removeByPaddr(cap.paddr);
        }
    }
}

pub fn allocOwnedUntypedMapPages(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    base_va: u64,
    page_count: usize,
    flags_bits: u64,
    out_paddr_list_va: u64,
    write_user_u64: *const fn (principal: kernel.PrincipalId, dest_user_va: u64, value: u64) bool,
) UntypedAllocMapPagesError!void {
    if ((base_va & 0xFFF) != 0) return error.InvalidArgument;
    if (page_count == 0 or page_count > kernel.max_retype_page_batch) return error.InvalidArgument;
    const flags: UntypedAllocMapFlags = @bitCast(flags_bits);
    if (flags.dma_ok and state.findOwnedUntypedForPages(proc, page_count, flags.contiguous) == null) {
        return error.AllocationFailed;
    }
    const block_id = state.findOwnedUntypedForPages(proc, page_count, flags.contiguous) orelse return error.AllocationFailed;
    const retype_flags = UntypedRetypeFlags{
        .writable = flags.writable,
        .drop_cap_after_map = flags.drop_cap_after_map,
        .contiguous = flags.contiguous,
    };
    retypeMapPages(
        state,
        proc,
        encodeToken(block_id),
        base_va,
        page_count,
        @bitCast(retype_flags),
        out_paddr_list_va,
        write_user_u64,
    ) catch |err| return err;
}

pub fn resetUntyped(state: *kernel.KernelState, proc: kernel.PrincipalId, token: u64) kernel.KernelError!void {
    const block_id = decodeToken(token) orelse return kernel.KernelError.InvalidState;
    try state.resetUntyped(proc, block_id);
}

pub fn allocMapPages(config: AllocMapPagesConfig) AllocMapPagesError!void {
    if ((config.base_va & 0xFFF) != 0) return error.InvalidArgument;
    if (config.page_count == 0) return error.InvalidArgument;

    var used_untyped = false;
    var block_id: u32 = 0;
    if (config.state.findOwnedUntypedForPages(config.proc, config.page_count, true)) |owned_block_id| {
        used_untyped = true;
        block_id = owned_block_id;
    }

    var caps: [kernel.max_retype_page_batch]kernel.PageCapability = undefined;
    if (used_untyped) {
        config.state.retypeUntypedToPages(
            config.proc,
            block_id,
            config.page_count,
            true,
            caps[0..config.page_count],
        ) catch return error.AllocationFailed;
    }

    var i: usize = 0;
    while (i < config.page_count) : (i += 1) {
        const cap = if (used_untyped)
            caps[i]
        else
            config.state.allocPageTo(config.proc, config.free_list) catch return error.AllocationFailed;

        const i_u64: u64 = @intCast(i);
        const offset_4k, const mul_overflow = @mulWithOverflow(i_u64, @as(u64, 4096));
        if (mul_overflow != 0) return error.MapFailed;
        const map_va, const va_overflow = @addWithOverflow(config.base_va, offset_4k);
        if (va_overflow != 0) return error.MapFailed;

        if (!capability.mapFreshUserPage(config.proc, map_va, cap.paddr, config.writable)) {
            return error.MapFailed;
        }

        if (config.out_paddr_list_va != 0) {
            const offset_8, const list_mul_overflow = @mulWithOverflow(i_u64, @as(u64, 8));
            if (list_mul_overflow != 0) return error.InvalidArgument;
            const list_va, const list_va_overflow = @addWithOverflow(config.out_paddr_list_va, offset_8);
            if (list_va_overflow != 0) return error.InvalidArgument;
            if (!config.write_user_u64(config.proc, list_va, cap.paddr)) {
                return error.MapFailed;
            }
        }

        if (config.drop_cap_after_map) {
            _ = config.state.getTable(config.proc).removeByPaddr(cap.paddr);
        }
    }
}

fn removeRange(free_list: *kernel.FreePageList, index: usize) void {
    const removed_len = free_list.ranges[index].len;
    var i = index;
    while (i + 1 < free_list.range_len) : (i += 1) {
        free_list.ranges[i] = free_list.ranges[i + 1];
    }
    free_list.range_len -= 1;
    free_list.len -= removed_len;
}

fn removeRangeNoLenChange(free_list: *kernel.FreePageList, index: usize) void {
    var i = index;
    while (i + 1 < free_list.range_len) : (i += 1) {
        free_list.ranges[i] = free_list.ranges[i + 1];
    }
    free_list.range_len -= 1;
}

fn encodeToken(block_id: u32) u64 {
    return untyped_token_base + @as(u64, block_id);
}

fn decodeToken(token: u64) ?u32 {
    if (token < untyped_token_base) return null;
    const raw = token - untyped_token_base;
    if (raw > std.math.maxInt(u32)) return null;
    return @intCast(raw);
}

fn alignUp(value: u64, alignment: u64) ?u64 {
    const plus, const overflow = @addWithOverflow(value, alignment - 1);
    if (overflow != 0) return null;
    return plus & ~(alignment - 1);
}

fn alignDown(value: u64, alignment: u64) u64 {
    return value & ~(alignment - 1);
}

fn carveFreeRange(free_list: *kernel.FreePageList, size_bytes: u64, alignment: u64) kernel.KernelError!u64 {
    var index = free_list.range_len;
    while (index > 0) {
        index -= 1;
        const range = free_list.ranges[index];
        const range_start = range.physical_start;
        const range_end = range.physical_start + (@as(u64, range.len) * 4096);
        if (range_end <= range_start) continue;
        if ((range_end - range_start) < size_bytes) continue;

        const candidate_start = alignDown(range_end - size_bytes, alignment);
        if (candidate_start < range_start) continue;
        const candidate_end = candidate_start + size_bytes;
        if (candidate_end > range_end) continue;

        const prefix_pages_u64 = (candidate_start - range_start) / 4096;
        const suffix_pages_u64 = (range_end - candidate_end) / 4096;
        const prefix_pages: usize = @intCast(prefix_pages_u64);
        const suffix_pages: usize = @intCast(suffix_pages_u64);
        const extra_ranges_needed: usize =
            @as(usize, if (prefix_pages > 0) 1 else 0) +
            @as(usize, if (suffix_pages > 0) 1 else 0);
        if (extra_ranges_needed > 1 and free_list.range_len >= free_list.ranges.len) {
            return kernel.KernelError.TooManyFreeRanges;
        }

        applyCarve(free_list, index, range.region_id, range_start, prefix_pages, candidate_end, suffix_pages);
        free_list.len -= @intCast(size_bytes / 4096);
        return candidate_start;
    }
    return kernel.KernelError.OutOfFreePages;
}

fn applyCarve(
    free_list: *kernel.FreePageList,
    index: usize,
    region_id: u64,
    prefix_start: u64,
    prefix_pages: usize,
    suffix_start: u64,
    suffix_pages: usize,
) void {
    if (prefix_pages > 0 and suffix_pages > 0) {
        shiftRight(free_list, index + 1);
        free_list.ranges[index] = .{
            .region_id = region_id,
            .len = prefix_pages,
            .physical_start = prefix_start,
        };
        free_list.ranges[index + 1] = .{
            .region_id = region_id,
            .len = suffix_pages,
            .physical_start = suffix_start,
        };
        return;
    }
    if (prefix_pages > 0) {
        free_list.ranges[index] = .{
            .region_id = region_id,
            .len = prefix_pages,
            .physical_start = prefix_start,
        };
        return;
    }
    if (suffix_pages > 0) {
        free_list.ranges[index] = .{
            .region_id = region_id,
            .len = suffix_pages,
            .physical_start = suffix_start,
        };
        return;
    }
    removeRangeNoLenChange(free_list, index);
}

fn shiftRight(free_list: *kernel.FreePageList, index: usize) void {
    var i = free_list.range_len;
    while (i > index) {
        free_list.ranges[i] = free_list.ranges[i - 1];
        i -= 1;
    }
    free_list.range_len += 1;
}
