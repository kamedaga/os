const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const kernel_log = @import("../kernel_log.zig");
const scheduler = @import("../scheduler.zig");
const user_vm = @import("../memory/user_vm.zig");
const sc = @import("numbers.zig");

const fd_abi = abi_root.fd_abi;
const TrapFrame = interrupts.TrapFrame;
const first_dynamic_fd: kernel.Fd = fd_abi.first_dynamic_fd;
const max_fd_wake_owners = kernel.fd_table_entries;

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

fn statusFromKernelError(err: kernel.KernelError) u64 {
    return switch (err) {
        kernel.KernelError.TableFull => sc.syscall_err_alloc,
        else => sc.syscall_err_invalid,
    };
}

fn writeUserU64Bytes(h: anytype, proc: kernel.PrincipalId, out_va: u64, len: u64, value: u64) u64 {
    if (out_va == 0 or len < 8) return sc.syscall_err_invalid;
    var bytes: [8]u8 = undefined;
    @import("std").mem.writeInt(u64, &bytes, value, .little);
    if (!h.copy_bytes_to_user_va(proc, out_va, bytes[0..])) return sc.syscall_err_invalid;
    return 8;
}
 
fn writeUserU16(h: anytype, proc: kernel.PrincipalId, out_va: u64, value: u16) bool {
    var bytes: [2]u8 = undefined;
    @import("std").mem.writeInt(u16, &bytes, value, .little);
    return h.copy_bytes_to_user_va(proc, out_va, bytes[0..]);
}

fn writeUserU32(h: anytype, proc: kernel.PrincipalId, out_va: u64, value: u32) bool {
    var bytes: [4]u8 = undefined;
    @import("std").mem.writeInt(u32, &bytes, value, .little);
    return h.copy_bytes_to_user_va(proc, out_va, bytes[0..]);
}

fn readUserU64Bytes(h: anytype, proc: kernel.PrincipalId, in_va: u64, len: u64) ?u64 {
    if (in_va == 0 or len < 8) return null;
    var bytes: [8]u8 = undefined;
    if (!h.copy_user_bytes_from_va(proc, in_va, bytes[0..])) return null;
    return @import("std").mem.readInt(u64, &bytes, .little);
}

fn wakeOwners(h: anytype, owners: []const kernel.PrincipalId) void {
    for (owners) |owner| {
        h.wake_waiting_thread_for_principal(owner);
    }
}

fn fdRead(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, out_va: u64, len: u64) u64 {
    if (state.eventReadCounter(proc, fd)) |count| {
        if (count == 0) return sc.syscall_err_not_ready;
        return writeUserU64Bytes(h, proc, out_va, len, count);
    }
    if (state.timerReadExpirations(proc, fd, scheduler.lapic_tick_count)) |count| {
        if (count == 0) return sc.syscall_err_not_ready;
        return writeUserU64Bytes(h, proc, out_va, len, count);
    }
    if (state.irqEventCountForFd(proc, fd, .{ .read = true })) |count| {
        if (count == 0) return sc.syscall_err_not_ready;
        return writeUserU64Bytes(h, proc, out_va, len, count);
    }
    return sc.syscall_err_invalid;
}

fn fdWrite(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, in_va: u64, len: u64) u64 {
    if (state.fdPayloadWithRightsConst(proc, fd, .{ .write = true })) |view| {
        switch (view.payload.*) {
            .serial => return fdWriteSerial(h, proc, in_va, len) orelse sc.syscall_err_invalid,
            else => {},
        }
    }
    const value = readUserU64Bytes(h, proc, in_va, len) orelse return sc.syscall_err_invalid;
    var wake_storage: [max_fd_wake_owners]kernel.PrincipalId = undefined;
    const wake_count = state.eventWakeOwnersForFd(proc, fd, wake_storage[0..]) catch return sc.syscall_err_invalid;
    state.eventWriteCounter(proc, fd, value) catch return sc.syscall_err_invalid;
    wakeOwners(h, wake_storage[0..wake_count]);
    return 8;
}

fn fdWriteSerial(h: anytype, proc: kernel.PrincipalId, in_va: u64, len: u64) ?u64 {
    if (len == 0) return 0;
    if (in_va == 0) return null;
    var total: u64 = 0;
    var buf: [256]u8 = undefined;
    while (total < len) {
        const remaining = len - total;
        const chunk_len: usize = @intCast(@min(remaining, buf.len));
        if (!h.copy_user_bytes_from_va(proc, in_va + total, buf[0..chunk_len])) return null;
        kernel_log.write(buf[0..chunk_len]);
        total += chunk_len;
    }
    return total;
}

fn fdReadv(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, iov_va: u64, iov_count: u64) u64 {
    if (iov_va == 0 or iov_count == 0 or iov_count > fd_abi.max_iovecs) return sc.syscall_err_invalid;
    var i: u64 = 0;
    while (i < iov_count) : (i += 1) {
        const item_va = iov_va + i * fd_abi.iovec_size;
        const base = h.read_user_u64(proc, item_va + fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
        const len = h.read_user_u64(proc, item_va + fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
        if (len < fd_abi.timerfd_read_size) continue;
        return fdRead(h, state, proc, fd, base, len);
    }
    return sc.syscall_err_invalid;
}

fn fdWritev(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, iov_va: u64, iov_count: u64) u64 {
    if (iov_va == 0 or iov_count == 0 or iov_count > fd_abi.max_iovecs) return sc.syscall_err_invalid;
    if (state.fdPayloadWithRightsConst(proc, fd, .{ .write = true })) |view| {
        switch (view.payload.*) {
            .serial => {
                var total: u64 = 0;
                var i: u64 = 0;
                while (i < iov_count) : (i += 1) {
                    const item_va = iov_va + i * fd_abi.iovec_size;
                    const base = h.read_user_u64(proc, item_va + fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
                    const len = h.read_user_u64(proc, item_va + fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
                    const written = fdWriteSerial(h, proc, base, len) orelse return sc.syscall_err_invalid;
                    total += written;
                }
                return total;
            },
            else => {},
        }
    }
    const base = h.read_user_u64(proc, iov_va + fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
    const len = h.read_user_u64(proc, iov_va + fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
    return fdWrite(h, state, proc, fd, base, len);
}

fn fdFcntl(state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, cmd: u64, arg0: u64, arg1: u64) u64 {
    return switch (cmd) {
        fd_abi.fcntl_get_flags => blk: {
            const info = state.fdInfo(proc, fd) orelse break :blk sc.syscall_err_invalid;
            break :blk info.flags_bits;
        },
        fd_abi.fcntl_set_flags => blk: {
            state.setFdFlags(proc, fd, kernel.fdFlagsFromBits(@truncate(arg0)), kernel.fdFlagsFromBits(@truncate(arg1))) catch break :blk sc.syscall_err_invalid;
            break :blk sc.syscall_ok;
        },
        fd_abi.fcntl_dup => state.dupFd(proc, fd, @intCast(arg0), kernel.fdRightsFromBits(arg1), .{}) catch sc.syscall_err_invalid,
        else => sc.syscall_err_invalid,
    };
}

fn pollOnce(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, pollfds_va: u64, count: u64, now_tick: u64) ?u64 {
    if (pollfds_va == 0 or count > fd_abi.max_pollfds) return null;
    var ready_count: u64 = 0;
    var i: u64 = 0;
    while (i < count) : (i += 1) {
        const item_va = pollfds_va + i * fd_abi.pollfd_size;
        const fd_u64 = h.read_user_u64(proc, item_va + fd_abi.pollfd_fd_offset) orelse return null;
        const events = h.read_user_u64(proc, item_va + fd_abi.pollfd_events_offset) orelse return null;
        if ((events & ~fd_abi.event_known_mask) != 0) return null;
        const revents = state.fdPollEvents(proc, @intCast(fd_u64), events, now_tick) orelse return null;
        if (!h.write_user_u64(proc, item_va + fd_abi.pollfd_revents_offset, revents)) return null;
        if (revents != 0) ready_count += 1;
    }
    return ready_count;
}

fn nextPollWakeDelta(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, pollfds_va: u64, count: u64, now_tick: u64) ?u64 {
    var min_delta: ?u64 = null;
    var i: u64 = 0;
    while (i < count) : (i += 1) {
        const item_va = pollfds_va + i * fd_abi.pollfd_size;
        const fd_u64 = h.read_user_u64(proc, item_va + fd_abi.pollfd_fd_offset) orelse return null;
        const wake_tick = state.fdNextWakeTick(proc, @intCast(fd_u64), now_tick) orelse continue;
        const delta = if (wake_tick <= now_tick) 1 else wake_tick - now_tick;
        if (min_delta == null or delta < min_delta.?) min_delta = delta;
    }
    return min_delta;
}

fn fdPoll(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, pollfds_va: u64, count: u64) u64 {
    return pollOnce(h, state, proc, pollfds_va, count, scheduler.lapic_tick_count) orelse sc.syscall_err_invalid;
}

fn fdWaitMany(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const pollfds_va = frame.rdi;
    const count = frame.rsi;
    const timeout_ticks = frame.rdx;
    const flags = frame.r10;
    if (flags != 0) return sc.syscall_err_invalid;
    const now = scheduler.lapic_tick_count;
    const ready = pollOnce(h, state, proc, pollfds_va, count, now) orelse return sc.syscall_err_invalid;
    if (ready != 0) return ready;
    if (timeout_ticks == 0) return sc.syscall_err_not_ready;

    var block_ticks: u64 = if (timeout_ticks == fd_abi.wait_forever) 0 else timeout_ticks;
    if (nextPollWakeDelta(h, state, proc, pollfds_va, count, now)) |delta| {
        if (block_ticks == 0 or delta < block_ticks) block_ticks = delta;
    }
    if (h.block_current_thread_for_event(frame, true, block_ticks, sc.syscall_err_not_ready)) return sc.syscall_err_not_ready;
    return sc.syscall_err_not_ready;
}

fn nanosToTicks(nsec: u64) ?u64 {
    const tick_nsec: u64 = 1_000_000;
    if (nsec == 0) return 0;
    if (nsec > @import("std").math.maxInt(u64) - tick_nsec + 1) return null;
    return (nsec + tick_nsec - 1) / tick_nsec;
}

fn timerfdCreate(state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    if (frame.rdi != fd_abi.timerfd_clock_monotonic) return sc.syscall_err_invalid;
    if ((frame.rsi & ~fd_abi.timerfd_known_flags_mask) != 0) return sc.syscall_err_invalid;
    const initial_ticks = nanosToTicks(frame.rdx) orelse return sc.syscall_err_invalid;
    const interval_ticks = nanosToTicks(frame.r10) orelse return sc.syscall_err_invalid;
    const deadline_tick = if (initial_ticks == 0)
        0
    else if ((frame.rsi & fd_abi.timerfd_flag_abstime) != 0)
        initial_ticks
    else
        scheduler.lapic_tick_count + initial_ticks;
    return state.createTimerFd(
        proc,
        deadline_tick,
        interval_ticks,
        kernel.fdFlagsFromBits(@truncate(frame.r9)),
        kernel.fdRightsFromBits(frame.r8),
        first_dynamic_fd,
    ) catch |err| statusFromKernelError(err);
}

fn eventfdCreate(state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const initial_value = frame.rdi;
    const rights = kernel.fdRightsFromBits(frame.rsi);
    const flags = kernel.fdFlagsFromBits(@truncate(frame.rdx));
    return state.createEventFd(
        proc,
        initial_value,
        flags,
        rights,
        first_dynamic_fd,
    ) catch |err| statusFromKernelError(err);
}

fn defaultVmoRights(rights: kernel.FdRights) kernel.FdRights {
    var out = rights;
    if (kernel.fdRightsToBits(out) == 0) {
        out = .{
            .inspect = true,
            .dup = true,
            .transfer = true,
            .close = true,
            .map_read = true,
            .map_write = true,
        };
    }
    return out;
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

    const vmo_ref = if (flags.anonymous) blk: {
        if (vmo_offset != 0) return sc.syscall_err_invalid;
        break :blk state.createAnonymousVmaWithPages(proc, base_va, aligned_size, prot, flags, free_list) catch |err| switch (err) {
            kernel.KernelError.TableFull => return sc.syscall_err_alloc,
            else => return sc.syscall_err_invalid,
        };
    } else blk: {
        const ref = state.nativeVmoRefForFd(proc, fd) orelse return sc.syscall_err_invalid;
        _ = state.mmapFd(proc, fd, base_va, aligned_size, prot, flags, vmo_offset) catch |err| switch (err) {
            kernel.KernelError.TableFull => return sc.syscall_err_alloc,
            else => return sc.syscall_err_invalid,
        };
        break :blk ref;
    };

    var paddrs: [kernel.max_vmo_backing_pages]u64 = undefined;
    const page_count: usize = @intCast(aligned_size / 4096);
    const offset_pages: usize = if (flags.anonymous) 0 else @intCast(vmo_offset / 4096);
    var i: usize = 0;
    while (i < page_count) : (i += 1) {
        paddrs[i] = state.nativeVmoPagePaddr(vmo_ref, offset_pages + i) orelse {
            state.munmapExactWithFreeList(proc, base_va, aligned_size, free_list) catch {};
            return sc.syscall_err_map;
        };
    }
    if (!prot.read and !prot.write and !prot.exec) return base_va;
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

fn mprotectVmaRange(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    base_va: u64,
    size_bytes: u64,
    prot_bits: u64,
) u64 {
    if (size_bytes == 0 or (base_va & 0xFFF) != 0) return sc.syscall_err_invalid;
    const aligned_size = pageAlignUp(size_bytes) orelse return sc.syscall_err_invalid;
    if (aligned_size / 4096 > kernel.max_vmo_backing_pages) return sc.syscall_err_invalid;
    const prot = protFromBits(prot_bits) orelse return sc.syscall_err_invalid;

    if (!prot.read and !prot.write and !prot.exec) {
        if (!user_vm.unmapUserLinearRegion(proc, base_va, @intCast(aligned_size))) return sc.syscall_err_map;
        return sc.syscall_ok;
    }

    const page_count: usize = @intCast(aligned_size / 4096);
    var paddrs: [kernel.max_vmo_backing_pages]u64 = undefined;
    var page_index: usize = 0;
    while (page_index < page_count) : (page_index += 1) {
        const va = base_va + @as(u64, @intCast(page_index)) * 4096;
        const vma = state.vmaEntryForVaConst(proc, va) orelse return sc.syscall_err_invalid;
        if (va + 4096 > vma.endVa()) return sc.syscall_err_invalid;
        const vmo_page_u64 = (vma.vmo_offset + (va - vma.start_va)) / 4096;
        if (vmo_page_u64 > @as(u64, @intCast(kernel.max_vmo_backing_pages))) return sc.syscall_err_invalid;
        paddrs[page_index] = state.nativeVmoPagePaddr(vma.vmo, @intCast(vmo_page_u64)) orelse return sc.syscall_err_map;
    }

    if (!user_vm.remapTrustedUserPaddrsWithProt(proc, base_va, paddrs[0..page_count], .{
        .read = prot.read,
        .write = prot.write,
        .exec = prot.exec,
        .pkey = prot.pkey,
    })) return sc.syscall_err_map;
    return sc.syscall_ok;
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

fn statModeForFd(info: kernel.FdInfo) u64 {
    const rights = kernel.fdRightsFromBits(info.rights_bits);
    const kind_bits: u64 = switch (info.kind) {
        .serial => fd_abi.stat_mode_ifchr,
        .vmo => fd_abi.stat_mode_ifreg,
        .event, .timer, .endpoint, .channel, .reply => fd_abi.stat_mode_ififo,
        else => fd_abi.stat_mode_ifreg,
    };
    var mode = kind_bits;
    if (rights.read or rights.recv or rights.map_read or rights.cpu_read or rights.irq_wait) mode |= fd_abi.stat_mode_irusr;
    if (rights.write or rights.send or rights.map_write or rights.cpu_write or rights.irq_ack) mode |= fd_abi.stat_mode_iwusr;
    if (rights.map_exec) mode |= fd_abi.stat_mode_ixusr;
    return mode;
}

fn writeFdStat(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, out_va: u64) u64 {
    if (out_va == 0) return sc.syscall_err_invalid;
    const info = state.fdInfo(proc, fd) orelse return sc.syscall_err_invalid;
    const mode = statModeForFd(info);
    const blocks = if (info.size_bytes == 0) 0 else (info.size_bytes + 511) / 512;
    const dev: u64 = 0x706163686f73;
    const rdev: u64 = switch (info.kind) {
        .serial => 0x100 + info.extra,
        else => 0,
    };
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_dev_offset, dev)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_ino_offset, (@as(u64, @intFromEnum(info.kind)) << 32) | fd)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_nlink_offset, 1)) return sc.syscall_err_invalid;
    if (!writeUserU32(h, proc, out_va + fd_abi.stat_mode_offset, @intCast(mode))) return sc.syscall_err_invalid;
    if (!writeUserU32(h, proc, out_va + fd_abi.stat_uid_offset, 0)) return sc.syscall_err_invalid;
    if (!writeUserU32(h, proc, out_va + fd_abi.stat_gid_offset, 0)) return sc.syscall_err_invalid;
    if (!writeUserU32(h, proc, out_va + fd_abi.stat_pad0_offset, 0)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_rdev_offset, rdev)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_size_offset, info.size_bytes)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_blksize_offset, 4096)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_blocks_offset, blocks)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_atime_sec_offset, 0)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_atime_nsec_offset, 0)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_mtime_sec_offset, 0)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_mtime_nsec_offset, 0)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_ctime_sec_offset, 0)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_ctime_nsec_offset, 0)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_unused0_offset, 0)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_unused1_offset, 0)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, out_va + fd_abi.stat_unused2_offset, 0)) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn fdIoctl(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, request: u64, arg_va: u64) u64 {
    const view = state.fdPayloadWithRightsConst(proc, fd, .{ .inspect = true }) orelse return sc.syscall_err_invalid;
    switch (view.payload.*) {
        .serial => {},
        else => return sc.syscall_err_invalid,
    }
    if (arg_va == 0) return sc.syscall_err_invalid;
    return switch (request) {
        fd_abi.ioctl_tiocgwinsz => blk: {
            if (!writeUserU16(h, proc, arg_va + fd_abi.winsize_rows_offset, 25)) break :blk sc.syscall_err_invalid;
            if (!writeUserU16(h, proc, arg_va + fd_abi.winsize_cols_offset, 80)) break :blk sc.syscall_err_invalid;
            if (!writeUserU16(h, proc, arg_va + fd_abi.winsize_xpixel_offset, 0)) break :blk sc.syscall_err_invalid;
            if (!writeUserU16(h, proc, arg_va + fd_abi.winsize_ypixel_offset, 0)) break :blk sc.syscall_err_invalid;
            break :blk sc.syscall_ok;
        },
        fd_abi.ioctl_tiocswinsz => sc.syscall_ok,
        else => sc.syscall_err_invalid,
    };
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
        sc.syscall_fd_read => fdRead(h, state, proc, @intCast(frame.rdi), frame.rsi, frame.rdx),
        sc.syscall_fd_write => fdWrite(h, state, proc, @intCast(frame.rdi), frame.rsi, frame.rdx),
        sc.syscall_fd_readv => fdReadv(h, state, proc, @intCast(frame.rdi), frame.rsi, frame.rdx),
        sc.syscall_fd_writev => fdWritev(h, state, proc, @intCast(frame.rdi), frame.rsi, frame.rdx),
        sc.syscall_fd_fcntl => fdFcntl(state, proc, @intCast(frame.rdi), frame.rsi, frame.rdx, frame.r10),
        sc.syscall_fd_poll => fdPoll(h, state, proc, frame.rdi, frame.rsi),
        sc.syscall_fd_wait_many => fdWaitMany(h, state, proc, frame),
        sc.syscall_fd_ioctl => fdIoctl(h, state, proc, @intCast(frame.rdi), frame.rsi, frame.rdx),
        sc.syscall_fd_stat => writeFdStat(h, state, proc, @intCast(frame.rdi), frame.rsi),
        sc.syscall_eventfd_create => eventfdCreate(state, proc, frame),
        sc.syscall_timerfd_create => timerfdCreate(state, proc, frame),
        sc.syscall_vmo_create => state.createAnonymousVmoFdWithPages(
            proc,
            frame.rdi,
            defaultVmoRights(kernel.fdRightsFromBits(frame.rsi)),
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
        sc.syscall_mprotect => mprotectVmaRange(state, proc, frame.rdi, frame.rsi, frame.rdx),
        else => null,
    };
}
