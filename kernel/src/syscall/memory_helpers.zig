const capability = @import("../capability.zig");
const kernel = @import("../kernel.zig");
const user_vm = @import("../memory/user_vm.zig");
const sc = @import("numbers.zig");

pub const AllocMapPagesError = error{
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

pub fn allocMapPages(config: AllocMapPagesConfig) AllocMapPagesError!void {
    if ((config.base_va & 0xFFF) != 0) return error.InvalidArgument;
    if (config.page_count == 0) return error.InvalidArgument;

    var allocated: [sc.syscall_batch_max_pages]u64 = undefined;
    if (config.page_count > allocated.len) return error.InvalidArgument;
    var allocated_count: usize = 0;
    var mapped_count: usize = 0;
    errdefer {
        if (mapped_count != 0) {
            _ = user_vm.unmapUserLinearRegion(config.proc, config.base_va, mapped_count * 4096);
        }
        var rollback_index: usize = allocated_count;
        while (rollback_index > 0) {
            rollback_index -= 1;
            config.state.reclaimExclusiveRootPage(config.proc, allocated[rollback_index], config.free_list) catch {};
        }
    }

    var i: usize = 0;
    while (i < config.page_count) : (i += 1) {
        const cap = config.state.allocPageTo(config.proc, config.free_list) catch return error.AllocationFailed;
        allocated[allocated_count] = cap.paddr;
        allocated_count += 1;
        const i_u64: u64 = @intCast(i);
        const offset_4k, const mul_overflow = @mulWithOverflow(i_u64, @as(u64, 4096));
        if (mul_overflow != 0) return error.MapFailed;
        const map_va, const va_overflow = @addWithOverflow(config.base_va, offset_4k);
        if (va_overflow != 0) return error.MapFailed;

        if (!capability.mapFreshUserPage(config.proc, map_va, cap.paddr, config.writable)) {
            return error.MapFailed;
        }
        mapped_count += 1;

        if (config.out_paddr_list_va != 0) {
            const offset_8, const list_mul_overflow = @mulWithOverflow(i_u64, @as(u64, 8));
            if (list_mul_overflow != 0) return error.InvalidArgument;
            const list_va, const list_va_overflow = @addWithOverflow(config.out_paddr_list_va, offset_8);
            if (list_va_overflow != 0) return error.InvalidArgument;
            if (!config.write_user_u64(config.proc, list_va, cap.paddr)) {
                return error.MapFailed;
            }
        }

    }

    if (config.drop_cap_after_map) {
        var drop_index: usize = 0;
        while (drop_index < allocated_count) : (drop_index += 1) {
            _ = config.state.getTable(config.proc).removeByPaddr(allocated[drop_index]);
        }
    }
}

pub fn allocMapPagesAnywhere(config: AllocMapPagesConfig) AllocMapPagesError!u64 {
    if (config.page_count == 0) return error.InvalidArgument;
    const base_va = capability.findFreeUserMappingRange(config.proc, @intCast(config.page_count), 1) orelse return error.MapFailed;
    var fixed = config;
    fixed.base_va = base_va;
    try allocMapPages(fixed);
    return base_va;
}
