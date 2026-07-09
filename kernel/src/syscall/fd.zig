const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const ipc_metric = @import("../ipc_metric.zig");
const kernel = @import("../kernel.zig");
const kernel_log = @import("../kernel_log.zig");
const scheduler = @import("../scheduler.zig").connection;
const user_vm = @import("../memory/user_vm.zig");
const sc = @import("numbers.zig");

const fd_abi = abi_root.fd_abi;
const scheduler_abi = abi_root.scheduler_abi;
const vm_abi = abi_root.vm_abi;
const TrapFrame = interrupts.TrapFrame;
const first_dynamic_fd: kernel.Fd = fd_abi.first_dynamic_fd;
const max_fd_wake_owners = kernel.fd_table_entries;
const max_pollfds: usize = @intCast(fd_abi.max_pollfds);

const PollItem = struct {
    fd: kernel.Fd = 0,
    events: u64 = 0,
    item_va: u64 = 0,
};

fn protFromBits(bits: u64) ?kernel.VmaProt {
    if ((bits & ~(vm_abi.prot_read | vm_abi.prot_write | vm_abi.prot_exec)) != 0) return null;
    return .{
        .read = (bits & vm_abi.prot_read) != 0,
        .write = (bits & vm_abi.prot_write) != 0,
        .exec = (bits & vm_abi.prot_exec) != 0,
    };
}

fn mmapFlagsFromBits(bits: u64) ?kernel.MmapFlags {
    const known = vm_abi.mmap_fixed |
        vm_abi.mmap_fixed_noreplace |
        vm_abi.mmap_private |
        vm_abi.mmap_shared |
        vm_abi.mmap_anonymous |
        vm_abi.mmap_noreserve |
        vm_abi.mmap_pkey_mask;
    if ((bits & ~known) != 0) return null;
    if ((bits & vm_abi.mmap_private) != 0 and (bits & vm_abi.mmap_shared) != 0) return null;
    return .{
        .fixed = (bits & vm_abi.mmap_fixed) != 0,
        .fixed_noreplace = (bits & vm_abi.mmap_fixed_noreplace) != 0,
        .private = (bits & vm_abi.mmap_private) != 0,
        .shared = (bits & vm_abi.mmap_shared) != 0,
        .anonymous = (bits & vm_abi.mmap_anonymous) != 0,
        .noreserve = (bits & vm_abi.mmap_noreserve) != 0,
        .pkey = @intCast((bits & vm_abi.mmap_pkey_mask) >> vm_abi.mmap_pkey_shift),
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

fn statusFromPipeError(err: kernel.PipeIoError) u64 {
    const status = switch (err) {
        kernel.PipeIoError.InvalidState => sc.syscall_err_invalid,
        kernel.PipeIoError.NotReady => sc.syscall_err_not_ready,
        kernel.PipeIoError.Closed => sc.syscall_err_closed,
    };
    return 0 -% status;
}

fn wakeThreadTargets(h: anytype, targets: []const kernel.ThreadWakeTarget) bool {
    for (targets) |target| {
        if (target.pollfd_va != 0) {
            if (!h.write_user_u64(target.owner, target.pollfd_va + fd_abi.pollfd_revents_offset, target.revents)) return false;
        }
        _ = h.wake_waiting_thread_generation_with_rax(target.thread_index, target.thread_generation, 1);
    }
    return true;
}

fn wakePipeWaiters(h: anytype, state: *kernel.KernelState, pipe_ref: kernel.PipeRef, write_side: bool, ready_events: u64) bool {
    var wake_storage: [max_pollfds]kernel.ThreadWakeTarget = undefined;
    const wake_count = state.takePipeWaiters(pipe_ref, write_side, ready_events, wake_storage[0..]);
    return wakeThreadTargets(h, wake_storage[0..wake_count]);
}

fn fdRead(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, out_va: u64, len: u64) u64 {
    if (out_va == 0) return sc.syscall_err_invalid;
    if (state.pipeEndpointForFd(proc, fd)) |endpoint| {
        if (endpoint.write) return sc.syscall_err_invalid;
        if (len == 0) return 0;
        var total: u64 = 0;
        var buf: [256]u8 = undefined;
        while (total < len) {
            const chunk_len: usize = @intCast(@min(len - total, buf.len));
            const got = state.pipeReadBytes(proc, fd, buf[0..chunk_len]) catch |err| {
                return if (total != 0) total else statusFromPipeError(err);
            };
            if (got == 0) return total;
            if (!h.copy_bytes_to_user_va(proc, out_va + total, buf[0..got])) return sc.syscall_err_invalid;
            total += got;
            if (!wakePipeWaiters(h, state, endpoint.pipe, true, fd_abi.event_writable)) return sc.syscall_err_invalid;
            if (got < chunk_len) return total;
        }
        return total;
    }
    if (state.fdPayloadWithRightsConst(proc, fd, .{ .read = true })) |view| {
        switch (view.payload.*) {
            .sched_event => {
                var event_buf: [@as(usize, @intCast(scheduler_abi.sched_event_size))]u8 = undefined;
                const got = scheduler.readPolicyEventBytes(event_buf[0..]) orelse return sc.syscall_err_invalid;
                if (got == 0) return sc.syscall_err_not_ready;
                if (len < got) return sc.syscall_err_invalid;
                if (!h.copy_bytes_to_user_va(proc, out_va, event_buf[0..got])) return sc.syscall_err_invalid;
                return got;
            },
            else => {},
        }
    }
    var vmo_buf: [256]u8 = undefined;
    if (len > 0) {
        var total: u64 = 0;
        while (total < len) {
            const chunk_len: usize = @intCast(@min(len - total, vmo_buf.len));
            const got = state.readFdVmoBytes(proc, fd, vmo_buf[0..chunk_len]) catch break;
            if (got == 0) return total;
            if (!h.copy_bytes_to_user_va(proc, out_va + total, vmo_buf[0..got])) return sc.syscall_err_invalid;
            total += got;
            if (got < chunk_len) return total;
        }
        if (total != 0) return total;
    }
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
    if (state.pipeEndpointForFd(proc, fd)) |endpoint| {
        if (!endpoint.write) return sc.syscall_err_invalid;
        if (in_va == 0 and len != 0) return sc.syscall_err_invalid;
        if (len == 0) return 0;
        var total: u64 = 0;
        var buf: [kernel.pipe_buffer_bytes]u8 = undefined;
        if (len <= kernel.pipe_buffer_bytes) {
            const chunk_len: usize = @intCast(len);
            if (!h.copy_user_bytes_from_va(proc, in_va, buf[0..chunk_len])) return sc.syscall_err_invalid;
            const written = state.pipeWriteBytes(proc, fd, buf[0..chunk_len], true) catch |err| return statusFromPipeError(err);
            if (!wakePipeWaiters(h, state, endpoint.pipe, false, fd_abi.event_readable)) return sc.syscall_err_invalid;
            return written;
        }
        while (total < len) {
            const chunk_len: usize = @intCast(@min(len - total, buf.len));
            if (!h.copy_user_bytes_from_va(proc, in_va + total, buf[0..chunk_len])) return if (total != 0) total else sc.syscall_err_invalid;
            const written = state.pipeWriteBytes(proc, fd, buf[0..chunk_len], false) catch |err| {
                return if (total != 0) total else statusFromPipeError(err);
            };
            if (written == 0) return total;
            total += written;
            if (!wakePipeWaiters(h, state, endpoint.pipe, false, fd_abi.event_readable)) return sc.syscall_err_invalid;
            if (written < chunk_len) return total;
        }
        return total;
    }
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
    if (state.pipeEndpointForFd(proc, fd)) |endpoint| {
        if (endpoint.write) return sc.syscall_err_invalid;
        var total: u64 = 0;
        var buf: [256]u8 = undefined;
        var i: u64 = 0;
        while (i < iov_count) : (i += 1) {
            const item_va = iov_va + i * fd_abi.iovec_size;
            const base = h.read_user_u64(proc, item_va + fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
            const len = h.read_user_u64(proc, item_va + fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
            if (len == 0) continue;
            if (base == 0) return if (total != 0) total else sc.syscall_err_invalid;
            var offset: u64 = 0;
            while (offset < len) {
                const chunk_len: usize = @intCast(@min(len - offset, buf.len));
                const got = state.pipeReadBytes(proc, fd, buf[0..chunk_len]) catch |err| {
                    return if (total != 0) total else statusFromPipeError(err);
                };
                if (got == 0) return total;
                if (!h.copy_bytes_to_user_va(proc, base + offset, buf[0..got])) return sc.syscall_err_invalid;
                offset += got;
                total += got;
                if (!wakePipeWaiters(h, state, endpoint.pipe, true, fd_abi.event_writable)) return sc.syscall_err_invalid;
                if (got < chunk_len) return total;
            }
        }
        return if (total != 0) total else sc.syscall_err_not_ready;
    }
    var i: u64 = 0;
    while (i < iov_count) : (i += 1) {
        const item_va = iov_va + i * fd_abi.iovec_size;
        const base = h.read_user_u64(proc, item_va + fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
        const len = h.read_user_u64(proc, item_va + fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
        if (len == 0) continue;
        return fdRead(h, state, proc, fd, base, len);
    }
    return sc.syscall_err_invalid;
}

fn fdWritev(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, iov_va: u64, iov_count: u64) u64 {
    if (iov_va == 0 or iov_count == 0 or iov_count > fd_abi.max_iovecs) return sc.syscall_err_invalid;
    if (state.pipeEndpointForFd(proc, fd)) |endpoint| {
        if (!endpoint.write) return sc.syscall_err_invalid;
        var requested: u64 = 0;
        var i: u64 = 0;
        while (i < iov_count) : (i += 1) {
            const item_va = iov_va + i * fd_abi.iovec_size;
            _ = h.read_user_u64(proc, item_va + fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
            const len = h.read_user_u64(proc, item_va + fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
            const next, const overflow = @addWithOverflow(requested, len);
            if (overflow != 0) return sc.syscall_err_invalid;
            requested = next;
        }
        if (requested == 0) return 0;
        var buf: [kernel.pipe_buffer_bytes]u8 = undefined;
        if (requested <= kernel.pipe_buffer_bytes) {
            var copied: usize = 0;
            i = 0;
            while (i < iov_count) : (i += 1) {
                const item_va = iov_va + i * fd_abi.iovec_size;
                const base = h.read_user_u64(proc, item_va + fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
                const len = h.read_user_u64(proc, item_va + fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
                if (len == 0) continue;
                const len_usize: usize = @intCast(len);
                if (base == 0 or !h.copy_user_bytes_from_va(proc, base, buf[copied .. copied + len_usize])) return sc.syscall_err_invalid;
                copied += len_usize;
            }
            const written = state.pipeWriteBytes(proc, fd, buf[0..copied], true) catch |err| return statusFromPipeError(err);
            if (!wakePipeWaiters(h, state, endpoint.pipe, false, fd_abi.event_readable)) return sc.syscall_err_invalid;
            return written;
        }
        var total: u64 = 0;
        i = 0;
        while (i < iov_count) : (i += 1) {
            const item_va = iov_va + i * fd_abi.iovec_size;
            const base = h.read_user_u64(proc, item_va + fd_abi.iovec_base_offset) orelse return if (total != 0) total else sc.syscall_err_invalid;
            const len = h.read_user_u64(proc, item_va + fd_abi.iovec_len_offset) orelse return if (total != 0) total else sc.syscall_err_invalid;
            if (len == 0) continue;
            if (base == 0) return if (total != 0) total else sc.syscall_err_invalid;
            var offset: u64 = 0;
            while (offset < len) {
                const chunk_len: usize = @intCast(@min(len - offset, buf.len));
                if (!h.copy_user_bytes_from_va(proc, base + offset, buf[0..chunk_len])) return if (total != 0) total else sc.syscall_err_invalid;
                const written = state.pipeWriteBytes(proc, fd, buf[0..chunk_len], false) catch |err| {
                    return if (total != 0) total else statusFromPipeError(err);
                };
                if (written == 0) return total;
                offset += written;
                total += written;
                if (!wakePipeWaiters(h, state, endpoint.pipe, false, fd_abi.event_readable)) return sc.syscall_err_invalid;
                if (written < chunk_len) return total;
            }
        }
        return total;
    }
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

fn readPollItems(h: anytype, proc: kernel.PrincipalId, pollfds_va: u64, count: u64, out: []PollItem) ?[]PollItem {
    if (pollfds_va == 0 or count > fd_abi.max_pollfds or count > out.len) return null;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const item_va = pollfds_va + @as(u64, @intCast(i)) * fd_abi.pollfd_size;
        const fd_u64 = h.read_user_u64(proc, item_va + fd_abi.pollfd_fd_offset) orelse return null;
        const events = h.read_user_u64(proc, item_va + fd_abi.pollfd_events_offset) orelse return null;
        if ((events & ~fd_abi.event_known_mask) != 0) return null;
        out[i] = .{
            .fd = @intCast(fd_u64),
            .events = events,
            .item_va = item_va,
        };
    }
    return out[0..@intCast(count)];
}

fn pollCached(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, items: []const PollItem, now_tick: u64) ?u64 {
    var ready_count: u64 = 0;
    for (items) |item| {
        const revents = state.fdPollEvents(proc, item.fd, item.events, now_tick) orelse return null;
        if (!h.write_user_u64(proc, item.item_va + fd_abi.pollfd_revents_offset, revents)) return null;
        if (revents != 0) ready_count += 1;
    }
    return ready_count;
}

fn registerCachedWaitersForPoll(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    items: []const PollItem,
    thread_index: usize,
    thread_generation: u32,
) kernel.KernelError!void {
    for (items) |item| {
        if (try state.registerTaskReadableWaiterForFd(proc, item.fd, item.events, item.item_va, thread_index, thread_generation)) {
            continue;
        }
        if (try state.registerPipeWaiterForFd(proc, item.fd, item.events, item.item_va, thread_index, thread_generation)) {
            continue;
        }
        try state.registerIpcReadableWaiterForFd(proc, item.fd, item.events, item.item_va, thread_index, thread_generation);
    }
}

fn unregisterCachedWaitersForPoll(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    items: []const PollItem,
    thread_index: usize,
    thread_generation: u32,
) void {
    for (items) |item| {
        state.unregisterPipeWaiterForFd(proc, item.fd, item.events, thread_index, thread_generation);
        state.unregisterIpcReadableWaiterForFd(proc, item.fd, item.events, thread_index, thread_generation);
    }
    state.unregisterTaskReadableWaiterForThread(thread_index, thread_generation);
}

fn nextCachedPollWakeDelta(state: *kernel.KernelState, proc: kernel.PrincipalId, items: []const PollItem, now_tick: u64) ?u64 {
    var min_delta: ?u64 = null;
    for (items) |item| {
        const wake_tick = state.fdNextWakeTick(proc, item.fd, now_tick) orelse continue;
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
    var poll_items_storage: [max_pollfds]PollItem = undefined;
    const poll_items = readPollItems(h, proc, pollfds_va, count, poll_items_storage[0..]) orelse return sc.syscall_err_invalid;
    const ready = pollCached(h, state, proc, poll_items, now) orelse return sc.syscall_err_invalid;
    if (ready != 0) return ready;
    if (timeout_ticks == 0) return sc.syscall_err_not_ready;

    const current_thread = scheduler.currentThread();
    const current_generation = scheduler.generationOfThread(current_thread) orelse return sc.syscall_err_invalid;
    const register_start = ipc_metric.timestamp();
    registerCachedWaitersForPoll(state, proc, poll_items, current_thread, current_generation) catch |err| {
        unregisterCachedWaitersForPoll(state, proc, poll_items, current_thread, current_generation);
        return statusFromKernelError(err);
    };
    ipc_metric.record(.wait_register, register_start);
    const repoll_start = ipc_metric.timestamp();
    const ready_after_register = pollCached(h, state, proc, poll_items, scheduler.lapic_tick_count) orelse {
        unregisterCachedWaitersForPoll(state, proc, poll_items, current_thread, current_generation);
        return sc.syscall_err_invalid;
    };
    ipc_metric.record(.wait_repoll, repoll_start);
    if (ready_after_register != 0) {
        unregisterCachedWaitersForPoll(state, proc, poll_items, current_thread, current_generation);
        return ready_after_register;
    }

    var block_ticks: u64 = if (timeout_ticks == fd_abi.wait_forever) 0 else timeout_ticks;
    if (nextCachedPollWakeDelta(state, proc, poll_items, now)) |delta| {
        if (block_ticks == 0 or delta < block_ticks) block_ticks = delta;
    }
    if (h.block_current_thread_for_event(frame, true, block_ticks, sc.syscall_err_not_ready, h.before_current_thread_leave)) return frame.rax;
    unregisterCachedWaitersForPoll(state, proc, poll_items, current_thread, current_generation);
    return sc.syscall_err_not_ready;
}

fn nanosToTicks(nsec: u64) ?u64 {
    const tick_nsec: u64 = 1_000_000;
    if (nsec == 0) return 0;
    if (nsec > @import("std").math.maxInt(u64) - tick_nsec + 1) return null;
    return (nsec + tick_nsec - 1) / tick_nsec;
}

fn ticksToTimespec(ticks: u64) struct { sec: u64, nsec: u64 } {
    return .{
        .sec = ticks / 1000,
        .nsec = (ticks % 1000) * 1_000_000,
    };
}

fn timespecToTicks(sec: u64, nsec: u64) ?u64 {
    if (nsec >= 1_000_000_000) return null;
    const sec_ns, const sec_overflow = @mulWithOverflow(sec, 1_000_000_000);
    if (sec_overflow != 0) return null;
    const total_ns, const add_overflow = @addWithOverflow(sec_ns, nsec);
    if (add_overflow != 0) return null;
    return nanosToTicks(total_ns);
}

fn readTimerSpec(h: anytype, proc: kernel.PrincipalId, spec_va: u64) ?struct { value_ticks: u64, interval_ticks: u64 } {
    if (spec_va == 0) return null;
    const interval_sec = h.read_user_u64(proc, spec_va + fd_abi.timerfd_spec_interval_sec_offset) orelse return null;
    const interval_nsec = h.read_user_u64(proc, spec_va + fd_abi.timerfd_spec_interval_nsec_offset) orelse return null;
    const value_sec = h.read_user_u64(proc, spec_va + fd_abi.timerfd_spec_value_sec_offset) orelse return null;
    const value_nsec = h.read_user_u64(proc, spec_va + fd_abi.timerfd_spec_value_nsec_offset) orelse return null;
    return .{
        .value_ticks = timespecToTicks(value_sec, value_nsec) orelse return null,
        .interval_ticks = timespecToTicks(interval_sec, interval_nsec) orelse return null,
    };
}

fn writeTimerSpec(h: anytype, proc: kernel.PrincipalId, spec_va: u64, state: kernel.TimerFdState) u64 {
    if (spec_va == 0) return sc.syscall_ok;
    const interval = ticksToTimespec(state.interval_ticks);
    const value = ticksToTimespec(state.remaining_ticks);
    if (!h.write_user_u64(proc, spec_va + fd_abi.timerfd_spec_interval_sec_offset, interval.sec)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, spec_va + fd_abi.timerfd_spec_interval_nsec_offset, interval.nsec)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, spec_va + fd_abi.timerfd_spec_value_sec_offset, value.sec)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, spec_va + fd_abi.timerfd_spec_value_nsec_offset, value.nsec)) return sc.syscall_err_invalid;
    return sc.syscall_ok;
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

fn timerfdSettime(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const fd: kernel.Fd = @intCast(frame.rdi);
    const flags = frame.rsi;
    if ((flags & ~fd_abi.timerfd_known_flags_mask) != 0) return sc.syscall_err_invalid;
    const spec = readTimerSpec(h, proc, frame.rdx) orelse return sc.syscall_err_invalid;
    const old_state = state.timerFdState(proc, fd, scheduler.lapic_tick_count) orelse return sc.syscall_err_invalid;
    const old_status = writeTimerSpec(h, proc, frame.r10, old_state);
    if (old_status != sc.syscall_ok) return old_status;
    const deadline_tick = if (spec.value_ticks == 0)
        0
    else if ((flags & fd_abi.timerfd_flag_abstime) != 0)
        spec.value_ticks
    else
        scheduler.lapic_tick_count + spec.value_ticks;
    state.setTimerFd(proc, fd, deadline_tick, spec.interval_ticks, @truncate(flags)) catch return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn timerfdGettime(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, out_va: u64) u64 {
    if (out_va == 0) return sc.syscall_err_invalid;
    const timer_state = state.timerFdState(proc, fd, scheduler.lapic_tick_count) orelse return sc.syscall_err_invalid;
    return writeTimerSpec(h, proc, out_va, timer_state);
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

fn pipeCreate(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const out_pair_va = frame.rdi;
    const flags_bits = frame.rsi;
    if (out_pair_va == 0 or (flags_bits & ~@as(u64, fd_abi.known_flags_mask)) != 0) return sc.syscall_err_invalid;
    const flags = kernel.fdFlagsFromBits(@truncate(flags_bits));
    const pair = state.createPipePairFds(proc, flags, first_dynamic_fd) catch |err| return statusFromKernelError(err);
    if (!h.write_user_u64(proc, out_pair_va + fd_abi.pipe_pair_read_fd_offset, pair.read) or
        !h.write_user_u64(proc, out_pair_va + fd_abi.pipe_pair_write_fd_offset, pair.write))
    {
        state.closeFdWithFreeList(proc, pair.read, h.free_list) catch {};
        state.closeFdWithFreeList(proc, pair.write, h.free_list) catch {};
        return sc.syscall_err_invalid;
    }
    return sc.syscall_ok;
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
            .revoke = true,
        };
    }
    return out;
}

const VmoRevokeUnmapper = struct {
    pub fn unmap(_: VmoRevokeUnmapper, owner: kernel.PrincipalId, start_va: u64, size_bytes: u64) bool {
        if (size_bytes > @import("std").math.maxInt(usize)) return false;
        return user_vm.unmapPresentUserLinearRegion(owner, start_va, @intCast(size_bytes));
    }
};

fn revokeVmoFd(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    fd: kernel.Fd,
    free_list: *kernel.FreePageList,
) u64 {
    user_vm.lockAddressSpaces();
    defer user_vm.unlockAddressSpaces();
    _ = state.revokeVmoFdWithFreeList(proc, fd, free_list, VmoRevokeUnmapper{}) catch |err| switch (err) {
        kernel.KernelError.TableFull => return sc.syscall_err_alloc,
        else => return sc.syscall_err_invalid,
    };
    return sc.syscall_ok;
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
    user_vm.lockAddressSpaces();
    defer user_vm.unlockAddressSpaces();

    const use_requested_hint =
        requested_va != 0 and
        !flags.fixed and
        !flags.fixed_noreplace and
        (requested_va & 0xFFF) == 0 and
        (state.userMapRangeIsFree(proc, requested_va, aligned_size) catch false);
    const base_va = if (flags.fixed or flags.fixed_noreplace or use_requested_hint)
        requested_va
    else
        state.findRandomizedFreeUserMapVa(proc, aligned_size, 0x4644_4d4d_4150_0000 ^ scheduler.lapic_tick_count ^ @as(u64, fd)) catch return sc.syscall_err_map;
    if ((base_va & 0xFFF) != 0) return sc.syscall_err_invalid;
    if (flags.fixed) {
        if (!user_vm.unmapPresentUserLinearRegion(proc, base_va, @intCast(aligned_size))) return sc.syscall_err_map;
        state.munmapRangeWithFreeList(proc, base_va, aligned_size, free_list) catch return sc.syscall_err_map;
    } else if (flags.fixed_noreplace) {
        if (!(state.userMapRangeIsFree(proc, base_va, aligned_size) catch false)) return sc.syscall_err_invalid;
    }

    if (flags.anonymous) {
        if (vmo_offset != 0) return sc.syscall_err_invalid;
        _ = state.createAnonymousVmaWithPages(proc, base_va, aligned_size, prot, flags, free_list) catch |err| {
            switch (err) {
                kernel.KernelError.TableFull => return sc.syscall_err_alloc,
                else => return sc.syscall_err_invalid,
            }
        };
        return base_va;
    }

    _ = state.mmapFd(proc, fd, base_va, aligned_size, prot, flags, vmo_offset) catch |err| switch (err) {
        kernel.KernelError.TableFull => return sc.syscall_err_alloc,
        else => return sc.syscall_err_invalid,
    };

    if (flags.private and !flags.shared) {
        return base_va;
    }
    var paddrs: [kernel.max_vmo_backing_pages]u64 = undefined;
    const page_count: usize = @intCast(aligned_size / 4096);
    var map_prot: ?kernel.MapProt = null;
    var i: usize = 0;
    while (i < page_count) : (i += 1) {
        const mapping = state.nativeVmaInitialMapping(proc, base_va + @as(u64, @intCast(i)) * 4096) orelse {
            state.munmapRangeWithFreeList(proc, base_va, aligned_size, free_list) catch {};
            return sc.syscall_err_map;
        };
        paddrs[i] = mapping.paddr;
        if (map_prot == null) map_prot = mapping.prot;
    }
    if (!prot.read and !prot.write and !prot.exec) return base_va;
    if (!user_vm.mapTrustedUserPaddrsWithProt(proc, base_va, paddrs[0..page_count], map_prot orelse .{})) {
        state.munmapRangeWithFreeList(proc, base_va, aligned_size, free_list) catch {};
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

    user_vm.lockAddressSpaces();
    defer user_vm.unlockAddressSpaces();

    const start_vma = state.vmaEntryForVaConst(proc, base_va) orelse return sc.syscall_err_invalid;
    if (base_va + aligned_size > start_vma.endVa()) return sc.syscall_err_invalid;

    if (!prot.read and !prot.write and !prot.exec) {
        if (!user_vm.unmapPresentUserLinearRegion(proc, base_va, @intCast(aligned_size))) return sc.syscall_err_map;
        state.setVmaProtRange(proc, base_va, aligned_size, prot) catch return sc.syscall_err_invalid;
        return sc.syscall_ok;
    }

    const page_count: usize = @intCast(aligned_size / 4096);
    var page_index: usize = 0;
    while (page_index < page_count) : (page_index += 1) {
        const va = base_va + @as(u64, @intCast(page_index)) * 4096;
        const vma = state.vmaEntryForVaConst(proc, va) orelse return sc.syscall_err_invalid;
        if (va + 4096 > vma.endVa()) return sc.syscall_err_invalid;
    }

    if (!user_vm.protectPresentUserLinearRegionWithProt(proc, base_va, @intCast(aligned_size), .{
        .read = prot.read,
        .write = prot.write,
        .exec = prot.exec,
        .pkey = prot.pkey,
    })) return sc.syscall_err_map;
    state.setVmaProtRange(proc, base_va, aligned_size, prot) catch return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn mremapVmaRange(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    free_list: *kernel.FreePageList,
    old_va: u64,
    old_len: u64,
    new_len: u64,
    flags_bits: u64,
    new_va: u64,
) u64 {
    if ((old_va & 0xFFF) != 0 or old_len == 0 or new_len == 0) return sc.syscall_err_invalid;
    if (new_va != 0 and (new_va & 0xFFF) != 0) return sc.syscall_err_invalid;
    const old_size = pageAlignUp(old_len) orelse return sc.syscall_err_invalid;
    const new_size = pageAlignUp(new_len) orelse return sc.syscall_err_invalid;
    if (old_size / 4096 > kernel.max_vmo_backing_pages or new_size / 4096 > kernel.max_vmo_backing_pages) return sc.syscall_err_invalid;
    if ((flags_bits & ~vm_abi.mremap_known_flags) != 0) return sc.syscall_err_invalid;
    const may_move = (flags_bits & vm_abi.mremap_maymove) != 0;
    const fixed = (flags_bits & vm_abi.mremap_fixed) != 0;
    if (fixed and !may_move) return sc.syscall_err_invalid;
    if (!fixed and new_va != 0) return sc.syscall_err_invalid;

    user_vm.lockAddressSpaces();
    defer user_vm.unlockAddressSpaces();

    const result = state.mremapRangeWithFreeList(proc, old_va, old_size, new_size, new_va, may_move, fixed, free_list) catch |err| switch (err) {
        kernel.KernelError.OutOfFreePages => return sc.syscall_err_alloc,
        kernel.KernelError.TableFull => return sc.syscall_err_alloc,
        else => return sc.syscall_err_invalid,
    };
    if (fixed and !user_vm.unmapPresentUserLinearRegion(proc, new_va, @intCast(new_size))) return sc.syscall_err_map;
    if (result != old_va or new_size < old_size) {
        const unmap_va = if (result == old_va) old_va + new_size else old_va;
        const unmap_size = if (result == old_va) old_size - new_size else old_size;
        if (unmap_size != 0 and !user_vm.unmapPresentUserLinearRegion(proc, unmap_va, @intCast(unmap_size))) return sc.syscall_err_map;
    }
    return result;
}

fn madviseVmaRange(
    base_va: u64,
    size_bytes: u64,
    advice: u64,
) u64 {
    if (size_bytes == 0) return sc.syscall_ok;
    if ((base_va & 0xFFF) != 0) return sc.syscall_err_invalid;
    if (!(advice <= 4 or advice == 8 or (advice >= 14 and advice <= 17) or advice == 20 or advice == 21)) return sc.syscall_err_invalid;
    _ = pageAlignUp(size_bytes) orelse return sc.syscall_err_invalid;
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
        .event, .timer, .endpoint, .channel, .reply, .pipe => fd_abi.stat_mode_ififo,
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

fn writeSchedulerCaps(h: anytype, proc: kernel.PrincipalId, arg_va: u64) u64 {
    if (arg_va == 0) return sc.syscall_err_invalid;
    if (!writeUserU32(h, proc, arg_va + scheduler_abi.sched_caps_size_offset, @intCast(scheduler_abi.sched_caps_size))) return sc.syscall_err_invalid;
    if (!writeUserU16(h, proc, arg_va + scheduler_abi.sched_caps_version_offset, scheduler_abi.abi_version)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, arg_va + scheduler_abi.sched_caps_event_size_offset, scheduler_abi.sched_event_size)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, arg_va + scheduler_abi.sched_caps_commit_size_offset, scheduler_abi.sched_commit_size)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, arg_va + scheduler_abi.sched_caps_weight_size_offset, scheduler_abi.sched_weight_size)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, arg_va + scheduler_abi.sched_caps_flags_offset, scheduler_abi.flag_bootstrap)) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn readSchedulerCommit(h: anytype, proc: kernel.PrincipalId, arg_va: u64) ?struct {
    cpu_id: u32,
    thread_id: u64,
    generation: u64,
} {
    if (arg_va == 0) return null;
    var bytes: [@as(usize, @intCast(scheduler_abi.sched_commit_size))]u8 = undefined;
    if (!h.copy_user_bytes_from_va(proc, arg_va, bytes[0..])) return null;
    const size_off: usize = @intCast(scheduler_abi.sched_commit_size_offset);
    const version_off: usize = @intCast(scheduler_abi.sched_commit_version_offset);
    const cpu_off: usize = @intCast(scheduler_abi.sched_commit_cpu_id_offset);
    const thread_off: usize = @intCast(scheduler_abi.sched_commit_thread_id_offset);
    const generation_off: usize = @intCast(scheduler_abi.sched_commit_generation_offset);
    const size = @import("std").mem.readInt(u32, bytes[size_off..][0..4], .little);
    const version = @import("std").mem.readInt(u16, bytes[version_off..][0..2], .little);
    if (size < scheduler_abi.sched_commit_size or version != scheduler_abi.abi_version) return null;
    return .{
        .cpu_id = @import("std").mem.readInt(u32, bytes[cpu_off..][0..4], .little),
        .thread_id = @import("std").mem.readInt(u64, bytes[thread_off..][0..8], .little),
        .generation = @import("std").mem.readInt(u64, bytes[generation_off..][0..8], .little),
    };
}

fn fdIoctl(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, request: u64, arg_va: u64, frame: *TrapFrame) u64 {
    const view = state.fdPayloadWithRightsConst(proc, fd, .{ .inspect = true }) orelse return sc.syscall_err_invalid;
    switch (view.payload.*) {
        .schedctl => {
            return switch (request) {
                scheduler_abi.ioctl_query_caps => writeSchedulerCaps(h, proc, arg_va),
                scheduler_abi.ioctl_commit => blk: {
                    const metric_start = scheduler.externalSchedulerMetricTimestamp();
                    const commit = readSchedulerCommit(h, proc, arg_va) orelse {
                        scheduler.noteExternalSchedulerCommitIoctlCycles(metric_start);
                        break :blk sc.syscall_err_invalid;
                    };
                    const caller_thread = scheduler.currentThread();
                    if (!scheduler.commitPolicyDecision(commit.cpu_id, commit.thread_id, commit.generation, frame, sc.syscall_ok)) {
                        scheduler.noteExternalSchedulerCommitIoctlCycles(metric_start);
                        break :blk sc.syscall_err_invalid;
                    }
                    scheduler.noteExternalSchedulerCommitIoctlCycles(metric_start);
                    const resumed_thread = scheduler.currentThread();
                    break :blk if (resumed_thread != caller_thread) frame.rax else sc.syscall_ok;
                },
                scheduler_abi.ioctl_set_weight => sc.syscall_ok,
                else => sc.syscall_err_invalid,
            };
        },
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
            const pipe_endpoint = state.pipeEndpointForFd(proc, @intCast(frame.rdi));
            state.closeFdWithFreeList(proc, @intCast(frame.rdi), h.free_list) catch break :blk sc.syscall_err_invalid;
            if (pipe_endpoint) |endpoint| {
                const wake_side_write = !endpoint.write;
                if (state.pipeReadyEventsForSide(endpoint.pipe, wake_side_write)) |ready_events| {
                    if (ready_events != 0 and !wakePipeWaiters(h, state, endpoint.pipe, wake_side_write, ready_events)) break :blk sc.syscall_err_invalid;
                }
            }
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
        sc.syscall_fd_ioctl => fdIoctl(h, state, proc, @intCast(frame.rdi), frame.rsi, frame.rdx, frame),
        sc.syscall_fd_stat => writeFdStat(h, state, proc, @intCast(frame.rdi), frame.rsi),
        sc.syscall_eventfd_create => eventfdCreate(state, proc, frame),
        sc.syscall_pipe_create => pipeCreate(h, state, proc, frame),
        sc.syscall_timerfd_create => timerfdCreate(state, proc, frame),
        sc.syscall_timerfd_settime => timerfdSettime(h, state, proc, frame),
        sc.syscall_timerfd_gettime => timerfdGettime(h, state, proc, @intCast(frame.rdi), frame.rsi),
        sc.syscall_vmo_create => state.createAnonymousVmoFdWithPages(
            proc,
            frame.rdi,
            defaultVmoRights(kernel.fdRightsFromBits(frame.rsi)),
            kernel.fdFlagsFromBits(@truncate(frame.rdx)),
            first_dynamic_fd,
            h.free_list,
        ) catch sc.syscall_err_alloc,
        sc.syscall_vmo_revoke => revokeVmoFd(state, proc, @intCast(frame.rdi), h.free_list),
        sc.syscall_mmap => mapVmoFd(state, proc, h.free_list, @intCast(frame.rdi), frame.rsi, frame.rdx, frame.r10, frame.r8, frame.r9),
        sc.syscall_munmap => blk: {
            if (frame.rsi == 0 or (frame.rdi & 0xFFF) != 0) break :blk sc.syscall_err_invalid;
            const size = pageAlignUp(frame.rsi) orelse break :blk sc.syscall_err_invalid;
            user_vm.lockAddressSpaces();
            defer user_vm.unlockAddressSpaces();
            if (!user_vm.unmapPresentUserLinearRegion(proc, frame.rdi, @intCast(size))) break :blk sc.syscall_err_map;
            state.munmapRangeWithFreeList(proc, frame.rdi, size, h.free_list) catch break :blk sc.syscall_err_map;
            break :blk sc.syscall_ok;
        },
        sc.syscall_mprotect => mprotectVmaRange(state, proc, frame.rdi, frame.rsi, frame.rdx),
        sc.syscall_mremap => mremapVmaRange(state, proc, h.free_list, frame.rdi, frame.rsi, frame.rdx, frame.r10, frame.r8),
        sc.syscall_madvise => madviseVmaRange(frame.rdi, frame.rsi, frame.rdx),
        else => null,
    };
}
