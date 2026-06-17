const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const user_vm = @import("../memory/user_vm.zig");
const sc = @import("numbers.zig");

const fd_abi = abi_root.fd_abi;
const TrapFrame = interrupts.TrapFrame;
const first_dynamic_fd: kernel.Fd = fd_abi.first_dynamic_fd;

fn protFromBits(bits: u64) ?kernel.VmaProt {
    if ((bits & ~(fd_abi.prot_read | fd_abi.prot_write | fd_abi.prot_exec)) != 0) return null;
    return .{
        .read = (bits & fd_abi.prot_read) != 0,
        .write = (bits & fd_abi.prot_write) != 0,
        .exec = (bits & fd_abi.prot_exec) != 0,
    };
}

fn mmapFlagsFromBits(bits: u64) ?kernel.MmapFlags {
    const known = fd_abi.mmap_fixed |
        fd_abi.mmap_fixed_noreplace |
        fd_abi.mmap_private |
        fd_abi.mmap_shared |
        fd_abi.mmap_anonymous |
        fd_abi.mmap_noreserve |
        fd_abi.mmap_pkey_mask;
    if ((bits & ~known) != 0) return null;
    if ((bits & fd_abi.mmap_private) != 0 and (bits & fd_abi.mmap_shared) != 0) return null;
    return .{
        .fixed = (bits & fd_abi.mmap_fixed) != 0,
        .fixed_noreplace = (bits & fd_abi.mmap_fixed_noreplace) != 0,
        .private = (bits & fd_abi.mmap_private) != 0,
        .shared = (bits & fd_abi.mmap_shared) != 0,
        .anonymous = (bits & fd_abi.mmap_anonymous) != 0,
        .noreserve = (bits & fd_abi.mmap_noreserve) != 0,
        .pkey = @intCast((bits & fd_abi.mmap_pkey_mask) >> fd_abi.mmap_pkey_shift),
    };
}

fn pageAlignUp(value: u64) ?u64 {
    if (value > @import("std").math.maxInt(u64) - 4095) return null;
    return (value + 4095) & ~@as(u64, 4095);
}

fn mapVmoFd(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    free_list: *kernel.FreePageList,
    fd: kernel.Fd,
    requested_va: u64,
    size_bytes: u64,
    prot_bits: u64,
    flags_bits: u64,
    vmo_offset: u64,
) u64 {
    if (size_bytes == 0) return sc.syscall_err_invalid;
    const aligned_size = pageAlignUp(size_bytes) orelse return sc.syscall_err_invalid;
    if (aligned_size / 4096 > kernel.max_vmo_backing_pages) return sc.syscall_err_invalid;
    var prot = protFromBits(prot_bits) orelse return sc.syscall_err_invalid;
    const flags = mmapFlagsFromBits(flags_bits) orelse return sc.syscall_err_invalid;
    if ((vmo_offset & 0xFFF) != 0) return sc.syscall_err_invalid;
    if (requested_va == 0 and (flags.fixed or flags.fixed_noreplace)) return sc.syscall_err_invalid;
    prot.pkey = flags.pkey;
    const base_va = if (requested_va != 0)
        requested_va
    else
        user_vm.findFreeUserMappingRange(proc, aligned_size / 4096, 1) orelse return sc.syscall_err_map;
    if ((base_va & 0xFFF) != 0) return sc.syscall_err_invalid;

    const vmo_ref = state.nativeVmoRefForFd(proc, fd) orelse return sc.syscall_err_invalid;
    _ = state.mmapFd(proc, fd, base_va, aligned_size, prot, flags, vmo_offset) catch |err| switch (err) {
        kernel.KernelError.TableFull => return sc.syscall_err_alloc,
        else => return sc.syscall_err_invalid,
    };

    var paddrs: [kernel.max_vmo_backing_pages]u64 = undefined;
    const page_count: usize = @intCast(aligned_size / 4096);
    const offset_pages: usize = @intCast(vmo_offset / 4096);
    var i: usize = 0;
    while (i < page_count) : (i += 1) {
        paddrs[i] = state.nativeVmoPagePaddr(vmo_ref, offset_pages + i) orelse {
            state.munmapExactWithFreeList(proc, base_va, aligned_size, free_list) catch {};
            return sc.syscall_err_map;
        };
    }
    if (!user_vm.mapTrustedUserPaddrsWithProt(proc, base_va, paddrs[0..page_count], .{
        .read = prot.read,
        .write = prot.write,
        .exec = prot.exec,
        .pkey = prot.pkey,
    })) {
        state.munmapExactWithFreeList(proc, base_va, aligned_size, free_list) catch {};
        return sc.syscall_err_map;
    }
    return base_va;
}

fn writeFdInfo(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, out_va: u64) u64 {
    if (out_va == 0) return sc.syscall_err_invalid;
    const info = state.fdInfo(proc, fd) orelse return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.fd_info_kind_offset, @intFromEnum(info.kind))) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.fd_info_rights_offset, info.rights_bits)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.fd_info_flags_offset, info.flags_bits)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.fd_info_size_offset, info.size_bytes)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.fd_info_extra_offset, info.extra)) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

pub fn dispatch(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    return switch (frame.rax) {
        sc.syscall_fd_close => blk: {
            state.closeFdWithFreeList(proc, @intCast(frame.rdi), h.free_list) catch break :blk sc.syscall_err_invalid;
            break :blk sc.syscall_ok;
        },
        sc.syscall_fd_dup => state.dupFd(
            proc,
            @intCast(frame.rdi),
            @intCast(frame.rsi),
            kernel.fdRightsFromBits(frame.rdx),
            kernel.fdFlagsFromBits(@truncate(frame.r10)),
        ) catch sc.syscall_err_invalid,
        sc.syscall_fd_get_info => writeFdInfo(h, state, proc, @intCast(frame.rdi), frame.rsi),
        sc.syscall_fd_set_flags => blk: {
            state.setFdFlags(proc, @intCast(frame.rdi), kernel.fdFlagsFromBits(@truncate(frame.rsi)), kernel.fdFlagsFromBits(@truncate(frame.rdx))) catch break :blk sc.syscall_err_invalid;
            break :blk sc.syscall_ok;
        },
        sc.syscall_vmo_create => state.createAnonymousVmoFdWithPages(
            proc,
            frame.rdi,
            kernel.fdRightsFromBits(frame.rsi),
            kernel.fdFlagsFromBits(@truncate(frame.rdx)),
            first_dynamic_fd,
            h.free_list,
        ) catch sc.syscall_err_alloc,
        sc.syscall_mmap => mapVmoFd(state, proc, h.free_list, @intCast(frame.rdi), frame.rsi, frame.rdx, frame.r10, frame.r8, frame.r9),
        sc.syscall_munmap => blk: {
            const size = pageAlignUp(frame.rsi) orelse break :blk sc.syscall_err_invalid;
            const vma = state.vmaEntryConst(proc, frame.rdi) orelse break :blk sc.syscall_err_invalid;
            if (vma.size_bytes != size) break :blk sc.syscall_err_invalid;
            if (!user_vm.unmapUserLinearRegion(proc, frame.rdi, @intCast(size))) break :blk sc.syscall_err_map;
            state.munmapExactWithFreeList(proc, frame.rdi, size, h.free_list) catch break :blk sc.syscall_err_map;
            break :blk sc.syscall_ok;
        },
        else => null,
    };
}
