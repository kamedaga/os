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
    var vma_installed = false;
    var fd_installed = false;
    var pages_installed_in_vmo = false;
    var anon_fd: kernel.Fd = 0;
    errdefer {
        if (vma_installed) {
            _ = user_vm.unmapUserLinearRegion(config.proc, config.base_va, config.page_count * 4096);
            config.state.munmapExactWithFreeList(config.proc, config.base_va, config.page_count * 4096, config.free_list) catch {};
        }
        if (fd_installed) {
            config.state.closeFdWithFreeList(config.proc, anon_fd, config.free_list) catch {};
        }
        if (!pages_installed_in_vmo) {
            var rollback_index: usize = allocated_count;
            while (rollback_index > 0) {
                rollback_index -= 1;
                config.free_list.appendPage(0, allocated[rollback_index]) catch {};
            }
        }
    }

    var i: usize = 0;
    while (i < config.page_count) : (i += 1) {
        const page = config.state.allocPhysicalPage(config.free_list) catch return error.AllocationFailed;
        allocated[allocated_count] = page.paddr;
        allocated_count += 1;
        const i_u64: u64 = @intCast(i);

        if (config.out_paddr_list_va != 0) {
            const offset_8, const list_mul_overflow = @mulWithOverflow(i_u64, @as(u64, 8));
            if (list_mul_overflow != 0) return error.InvalidArgument;
            const list_va, const list_va_overflow = @addWithOverflow(config.out_paddr_list_va, offset_8);
            if (list_va_overflow != 0) return error.InvalidArgument;
            if (!config.write_user_u64(config.proc, list_va, page.paddr)) {
                return error.MapFailed;
            }
        }

    }

    const byte_count = config.page_count * 4096;
    var rights = kernel.FdRights{
        .map_read = true,
        .map_write = config.writable,
    };
    if (config.writable) rights.set_flags = true;
    anon_fd = config.state.createAnonymousVmoFd(config.proc, byte_count, rights, .{ .private = true }, 0) catch return error.AllocationFailed;
    fd_installed = true;
    const vmo_ref = config.state.nativeVmoRefForFd(config.proc, anon_fd) orelse return error.AllocationFailed;
    config.state.installNativeVmoPages(vmo_ref, 0, allocated[0..allocated_count]) catch return error.AllocationFailed;
    pages_installed_in_vmo = true;

    _ = config.state.mmapFd(
        config.proc,
        anon_fd,
        config.base_va,
        byte_count,
        .{ .read = true, .write = config.writable },
        .{ .anonymous = true, .private = true },
        0,
    ) catch return error.MapFailed;
    vma_installed = true;

    if (!user_vm.mapTrustedUserPaddrsWithProt(config.proc, config.base_va, allocated[0..config.page_count], .{
        .read = true,
        .write = config.writable,
        .exec = true,
    })) {
        return error.MapFailed;
    }

    // Anonymous mmap is owned by the VMA after mapping; no user-visible fd is returned.
    _ = config.drop_cap_after_map;
    config.state.closeFdWithFreeList(config.proc, anon_fd, config.free_list) catch return error.MapFailed;
    fd_installed = false;
}

pub fn allocMapPagesAnywhere(config: AllocMapPagesConfig) AllocMapPagesError!u64 {
    if (config.page_count == 0) return error.InvalidArgument;
    const base_va = capability.findFreeUserMappingRange(config.proc, @intCast(config.page_count), 1) orelse return error.MapFailed;
    var fixed = config;
    fixed.base_va = base_va;
    try allocMapPages(fixed);
    return base_va;
}
