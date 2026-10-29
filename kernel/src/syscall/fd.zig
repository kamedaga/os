const abi_root = @import("kernel_abi_root");
const std_rc = @import("std");
const interrupts = @import("../interrupts.zig");
const ipc_metric = @import("../ipc_metric.zig");
const lock_profile = @import("../lock_profile.zig");
const kernel = @import("../kernel.zig");
const kernel_log = @import("../kernel_log.zig");
const scheduler = @import("../scheduler.zig").connection;
const user_vm = @import("../memory/user_vm.zig");
const sc = @import("numbers.zig");
const smp_perf = @import("../smp_perf.zig");

const fd_abi = abi_root.fd_abi;
const pager_abi = abi_root.pager_abi;
const vm_abi = abi_root.vm_abi;
const TrapFrame = interrupts.TrapFrame;
const first_dynamic_fd: kernel.Fd = fd_abi.first_dynamic_fd;
const max_fd_wake_owners = kernel.fd_table_entries;
const max_pollfds: usize = @intCast(fd_abi.max_pollfds);

const PollItem = struct {
    fd: kernel.Fd = 0,
    events: u64 = 0,
    min_write_bytes: u64 = 0,
    item_va: u64 = 0,
};

fn protFromBits(bits: u64) ?kernel.VmaProt {
    if ((bits & ~(vm_abi.prot_read | vm_abi.prot_write | vm_abi.prot_exec)) != 0) return null;
    const prot: kernel.VmaProt = .{
        .read = (bits & vm_abi.prot_read) != 0,
        .write = (bits & vm_abi.prot_write) != 0,
        .exec = (bits & vm_abi.prot_exec) != 0,
    };
    return prot;
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

/// TEMPORARY (apk network-install fault): a refused allocation that says
/// nothing turns every downstream failure into guesswork.  Rate-limited.
var refusal_count: u64 = 0;

fn reportRefusal(site: []const u8, err: kernel.KernelError) void {
    refusal_count += 1;
    if (refusal_count > 24 and refusal_count % 256 != 0) return;
    kernel_log.writeFmt(
        "mem: refused site={s} err={s} n={}\n",
        .{ site, @errorName(err), refusal_count },
    );
    if (err == kernel.KernelError.TableFull) {
        const store = kernel.vmObjectBackingStoreStats();
        kernel_log.writeFmt(
            "mem: backing store_used={} store_capacity={} store_free={}\n",
            .{ store.used_entries, store.capacity, store.free_entries },
        );
    }
}

var ram_cache_refusal_count: u64 = 0;

fn reportRamCacheRefusal(err: anyerror, offset: u64, bytes: u64, mode: @import("../memory/ram_cache_transition.zig").Mode) void {
    const count = @atomicRmw(u64, &ram_cache_refusal_count, .Add, 1, .monotonic) + 1;
    if (count > 24 and count % 256 != 0) return;
    // The syscall deliberately returns a small stable status set. Its MAP
    // status combines distinct ownership, alias, and cache-proof failures;
    // retain the internal reason in the existing kernel log for diagnosis.
    kernel_log.writeFmt(
        "ram-cache: syscall refused reason={s} offset=0x{x} pages={} mode={s} n={}\n",
        .{ @errorName(err), offset, bytes / 4096, @tagName(mode), count },
    );
}

fn statusFromKernelError(err: kernel.KernelError) u64 {
    const status: u64 = switch (err) {
        kernel.KernelError.TableFull, kernel.KernelError.OutOfFreePages => sc.syscall_err_alloc,
        else => sc.syscall_err_invalid,
    };
    reportRefusal("kernel_error", err);
    return status;
}

var page_view_refusal_count: u64 = 0;

fn reportPageViewRefusal(comptime stage: []const u8, page_count: u64, list_index: ?usize, parent_page: ?u64, reason: []const u8) void {
    if (comptime !kernel_log.ring_enabled) return;
    const count = @atomicRmw(u64, &page_view_refusal_count, .Add, 1, .monotonic) + 1;
    if (count > 24 and count % 256 != 0) return;
    // The public status deliberately does not expose VMO internals. A single
    // failure-only ring line distinguishes the kernel's refusal without
    // logging user pointers, physical addresses, or every input page.
    kernel_log.writeFmt(
        "page-view: refused stage={s} index={} page_known={} parent_page={} pages={} reason={s} n={}\n",
        .{ stage, if (list_index) |index| @as(i64, @intCast(index)) else -1, @intFromBool(parent_page != null), parent_page orelse 0, page_count, reason, count },
    );
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

fn sameFdWaitGroup(a: kernel.ThreadWakeTarget, b: kernel.ThreadWakeTarget) bool {
    return a.wait_token == b.wait_token and a.group.matches(b.group);
}

pub fn wakeThreadTargets(
    h: anytype,
    state: *kernel.KernelState,
    targets: []const kernel.ThreadWakeTarget,
) bool {
    var all_copies_ok = true;
    for (targets, 0..) |target, target_index| {
        if (target.wait_token == 0 or (target.group.isNull() and target.receive_block.isNull())) continue;
        var already_handled = false;
        for (targets[0..target_index]) |earlier| {
            if (!target.group.isNull() and sameFdWaitGroup(earlier, target)) {
                already_handled = true;
                break;
            }
        }
        if (already_handled) continue;

        const claim = scheduler.claimWaitTokenCompletion(
            target.thread_index,
            target.thread_generation,
            target.owner,
            target.wait_token,
        );
        switch (claim) {
            .stale => {
                state.cancelFdWaitGroup(target.group, target.wait_token);
                continue;
            },
            .claimed => {},
        }

        // Detach before staging/copyout; the claim keeps the thread
        // non-runnable while stable group links retain every ready poll item.
        state.detachFdWaitGroup(target.group, target.wait_token);
        if (target.recv_memory.active) {
            state.cancelFdWaitGroup(target.group, target.wait_token);
            // No message was dequeued by this generic wake path. In
            // particular, peer death must not masquerade as RECV success.
            _ = scheduler.publishClaimedWaitCompletion(
                target.thread_index,
                target.thread_generation,
                target.owner,
                target.wait_token,
                if ((target.revents & fd_abi.event_hangup) != 0)
                    sc.syscall_err_closed
                else
                    sc.syscall_err_not_ready,
            );
            continue;
        }
        const result = copyClaimedFdWaitGroup(h, state, target);
        const copy_ok = result.copy_ok;
        const resume_rax = result.status;
        _ = scheduler.publishClaimedWaitCompletion(
            target.thread_index,
            target.thread_generation,
            target.owner,
            target.wait_token,
            resume_rax,
        );
        if (!copy_ok) all_copies_ok = false;
    }
    return all_copies_ok;
}

pub fn copyClaimedFdWaitGroup(h: anytype, state: *kernel.KernelState, target: kernel.ThreadWakeTarget) struct { copy_ok: bool, status: u64 } {
    var batch: [32]kernel.ThreadWakeTarget = undefined;
    var cursor: u64 = 0;
    var ready_count: u64 = 0;
    var first: kernel.ThreadWakeTarget = undefined;
    var copy_ok = true;
    while (true) {
        const count = state.takeFdWaitGroupReadyBatch(target.group, target.wait_token, &cursor, &batch);
        if (count == 0) break;
        for (batch[0..count]) |member| {
            if (member.revents == 0) continue;
            ready_count += 1;
            if (ready_count == 1) {
                first = member;
                continue;
            }
            if (ready_count == 2 and !h.write_user_u64(first.owner, first.pollfd_va + fd_abi.pollfd_revents_offset, first.revents)) copy_ok = false;
            if (!h.write_user_u64(member.owner, member.pollfd_va + fd_abi.pollfd_revents_offset, member.revents)) copy_ok = false;
        }
    }
    if (ready_count == 1) {
        const base = state.fd_wait_groups.get(target.group.index).?.pollfds_va;
        // Claim excludes timeout/signal; K keeps group storage alive through staging.
        copy_ok = scheduler.stagePollResult(target, (first.pollfd_va - base) / fd_abi.pollfd_size, first.revents);
    }
    state.cancelFdWaitGroup(target.group, target.wait_token);
    return .{ .copy_ok = copy_ok, .status = if (copy_ok) ready_count else sc.syscall_err_invalid };
}

// Keep the seed batch separate from the admission frame; stable group links
// retain readiness until claim, copy and publication finish under K.
noinline fn wakePipeWaiters(h: anytype, state: *kernel.KernelState, pipe_ref: kernel.PipeRef, write_side: bool, ready_events: u64) bool {
    var wake_storage: [32]kernel.ThreadWakeTarget = undefined;
    var ok = true;
    while (true) {
        const wake_count = state.takePipeWaiters(pipe_ref, write_side, ready_events, wake_storage[0..]);
        if (wake_count == 0) return ok;
        if (!wakeThreadTargets(h, state, wake_storage[0..wake_count])) ok = false;
    }
}

/// Retirement is a terminal event even for aliases already sleeping in
/// WAIT_MANY. Reuse the same wait-group claim/copy/wake protocol as normal I/O.
/// Keep its seed-batch frame out of retirement/fault callers as well.
pub noinline fn wakeRetiredIrqWaiters(h: anytype, state: *kernel.KernelState) void {
    var targets: [32]kernel.ThreadWakeTarget = undefined;
    while (true) {
        const count = state.takeReadyIrqWaiters(&targets);
        if (count == 0) return;
        // A bad destination is reported to that waiter, not as a failed retire
        // after the route's no-fail commit has already completed.
        _ = wakeThreadTargets(h, state, targets[0..count]);
    }
}

fn ipcChannelHandleForFd(state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd) ?kernel.IpcChannelHandle {
    const view = state.fdPayloadWithRightsConst(proc, fd, .{}) orelse return null;
    return switch (view.payload.*) {
        .channel => |handle| handle,
        else => null,
    };
}

// Like pipe wake, keep this seed batch out of the teardown caller frame.
noinline fn wakeIpcChannelCloseWaiters(h: anytype, state: *kernel.KernelState, handle: kernel.IpcChannelHandle) bool {
    var wake_storage: [32]kernel.ThreadWakeTarget = undefined;
    var ok = true;
    while (true) {
        const wake_count = state.takeIpcChannelPeerCloseWaiters(handle, wake_storage[0..]);
        if (wake_count == 0) return ok;
        if (!wakeThreadTargets(h, state, wake_storage[0..wake_count])) ok = false;
    }
}

/// Completes a lean IPC_NOTIFY_WAIT waiter with `status`.  The waiter was
/// already detached from its channel slot, so a stale token only means the
/// thread left the wait (timeout, signal, exit) and owes nothing.
pub fn completeIpcNotifyWaiter(target: kernel.ThreadWakeTarget, status: u64) scheduler.ClaimedWaitPublishResult {
    return scheduler.claimAndPublishWaitCompletionPreferred(
        target.thread_index,
        target.thread_generation,
        target.owner,
        target.wait_token,
        status,
    );
}

fn wakeIpcChannelNotifyWaitersAfterClose(
    state: *kernel.KernelState,
    handle: kernel.IpcChannelHandle,
    snapshot: [2]?kernel.ThreadWakeTarget,
) void {
    var targets: [2]kernel.ThreadWakeTarget = undefined;
    const count = state.takeIpcChannelNotifyWaitersAfterClose(handle, snapshot, &targets);
    for (targets[0..count]) |target| _ = completeIpcNotifyWaiter(target, sc.syscall_err_closed);
}

fn wakeReadyPipeWaiters(h: anytype, state: *kernel.KernelState, pipe_ref: kernel.PipeRef, write_side: bool) bool {
    const ready_events = state.pipeReadyEventsForSide(pipe_ref, write_side) orelse return false;
    if (ready_events == 0) return true;
    return wakePipeWaiters(h, state, pipe_ref, write_side, ready_events);
}

fn fdRead(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, out_va: u64, len: u64) u64 {
    if (out_va == 0) return sc.syscall_err_invalid;
    if (state.pipeEndpointForFd(proc, fd)) |endpoint| {
        if (endpoint.write) return sc.syscall_err_invalid;
        if (len == 0) return 0;
        var total: u64 = 0;
        var buf: [kernel.pipe_buffer_bytes]u8 = undefined;
        while (total < len) {
            const chunk_len: usize = @intCast(@min(len - total, buf.len));
            const got = state.pipeReadBytes(proc, fd, buf[0..chunk_len]) catch |err| {
                return if (total != 0) total else statusFromPipeError(err);
            };
            if (got == 0) return total;
            if (!h.copy_bytes_to_user_va(proc, out_va + total, buf[0..got])) return sc.syscall_err_invalid;
            total += got;
            if (!wakeReadyPipeWaiters(h, state, endpoint.pipe, true)) return sc.syscall_err_invalid;
            if (got < chunk_len) return total;
        }
        return total;
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
    if (state.fdPayloadWithRightsConst(proc, fd, .{ .read = true })) |view| {
        if (view.payload.* == .timer) {
            const count = state.timerReadExpirations(proc, fd, timerNowNs()) orelse return sc.syscall_err_invalid;
            if (count == 0) return sc.syscall_err_not_ready;
            return writeUserU64Bytes(h, proc, out_va, len, count);
        }
    }
    if (state.irqObjectForFd(proc, fd, .{ .read = true })) |irq| {
        if (irq.retired) return sc.syscall_err_closed;
    }
    if (state.irqEventCountForFd(proc, fd, .{ .read = true })) |count| {
        if (count == 0) return sc.syscall_err_not_ready;
        const written = writeUserU64Bytes(h, proc, out_va, len, count);
        if (written != sc.syscall_err_invalid) {
            _ = state.acknowledgeIrqEventCountForFd(proc, fd, count);
        }
        return written;
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
    const event_object = (state.fdEntryConst(proc, fd) orelse return sc.syscall_err_invalid).object;
    var wake_storage: [max_fd_wake_owners]kernel.PrincipalId = undefined;
    const wake_count = state.eventWakeOwnersForFd(proc, fd, wake_storage[0..]) catch return sc.syscall_err_invalid;
    state.eventWriteCounter(proc, fd, value) catch return sc.syscall_err_invalid;
    var target_storage: [32]kernel.ThreadWakeTarget = undefined;
    var copies_ok = true;
    while (true) {
        const target_count = state.takeObjectReadableWaiters(event_object, target_storage[0..]);
        if (target_count == 0) break;
        if (!wakeThreadTargets(h, state, target_storage[0..target_count])) copies_ok = false;
    }
    if (!copies_ok) return sc.syscall_err_invalid;
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
        // Serial is also a user-visible FD. Keep user output on the serial
        // stream, but do not present it as kernel-originated ring content.
        kernel_log.writeUser(buf[0..chunk_len]);
        total += chunk_len;
    }
    return total;
}

fn readIovecWord(h: anytype, proc: kernel.PrincipalId, item_va: u64, field_offset: u64) ?u64 {
    // The entry probe captured descriptors before taking K. Using the same
    // snapshot for transfer prevents another thread from swapping a resident
    // target for an absent one after the probe and before a side effect.
    const Hook = switch (@typeInfo(@TypeOf(h))) {
        .pointer => |pointer| pointer.child,
        else => @TypeOf(h),
    };
    if (comptime @hasField(Hook, "iovec_snapshot")) {
        if (h.iovec_snapshot) |snapshot| {
            if (item_va >= snapshot.base_va and (item_va - snapshot.base_va) % fd_abi.iovec_size == 0) {
                const index = (item_va - snapshot.base_va) / fd_abi.iovec_size;
                if (index < snapshot.count and field_offset <= 8 and field_offset % 8 == 0)
                    return snapshot.words[@as(usize, @intCast(index * 2 + field_offset / 8))];
            }
            return null;
        }
    }
    return h.read_user_u64(proc, item_va + field_offset);
}

test "iovec snapshot prevents a post-probe pointer swap" {
    const Mock = struct {
        fn stale(_: kernel.PrincipalId, _: u64) ?u64 {
            @panic("iovec descriptor was re-read after preflight");
        }
    };
    const snapshot: struct { base_va: u64, count: u64, words: [fd_abi.max_iovecs * 2]u64 } = .{
        .base_va = 0x3000,
        .count = 1,
        .words = [_]u64{ 0x5000, 8192 } ++ [_]u64{0} ** ((fd_abi.max_iovecs - 1) * 2),
    };
    const h = .{ .iovec_snapshot = @as(?*const @TypeOf(snapshot), &snapshot), .read_user_u64 = Mock.stale };
    const proc: kernel.PrincipalId = @enumFromInt(1);
    try std_rc.testing.expectEqual(@as(u64, 0x5000), readIovecWord(&h, proc, 0x3000, fd_abi.iovec_base_offset).?);
    try std_rc.testing.expectEqual(@as(u64, 8192), readIovecWord(&h, proc, 0x3000, fd_abi.iovec_len_offset).?);
}

fn fdReadv(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, iov_va: u64, iov_count: u64) u64 {
    if (iov_va == 0 or iov_count == 0 or iov_count > fd_abi.max_iovecs) return sc.syscall_err_invalid;
    if (state.pipeEndpointForFd(proc, fd)) |endpoint| {
        if (endpoint.write) return sc.syscall_err_invalid;
        var total: u64 = 0;
        var buf: [kernel.pipe_buffer_bytes]u8 = undefined;
        var i: u64 = 0;
        while (i < iov_count) : (i += 1) {
            const item_va = iov_va + i * fd_abi.iovec_size;
            const base = readIovecWord(h, proc, item_va, fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
            const len = readIovecWord(h, proc, item_va, fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
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
                if (!wakeReadyPipeWaiters(h, state, endpoint.pipe, true)) return sc.syscall_err_invalid;
                if (got < chunk_len) return total;
            }
        }
        return if (total != 0) total else sc.syscall_err_not_ready;
    }
    var i: u64 = 0;
    while (i < iov_count) : (i += 1) {
        const item_va = iov_va + i * fd_abi.iovec_size;
        const base = readIovecWord(h, proc, item_va, fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
        const len = readIovecWord(h, proc, item_va, fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
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
            _ = readIovecWord(h, proc, item_va, fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
            const len = readIovecWord(h, proc, item_va, fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
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
                const base = readIovecWord(h, proc, item_va, fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
                const len = readIovecWord(h, proc, item_va, fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
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
            const base = readIovecWord(h, proc, item_va, fd_abi.iovec_base_offset) orelse return if (total != 0) total else sc.syscall_err_invalid;
            const len = readIovecWord(h, proc, item_va, fd_abi.iovec_len_offset) orelse return if (total != 0) total else sc.syscall_err_invalid;
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
                    const base = readIovecWord(h, proc, item_va, fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
                    const len = readIovecWord(h, proc, item_va, fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
                    const written = fdWriteSerial(h, proc, base, len) orelse return sc.syscall_err_invalid;
                    total += written;
                }
                return total;
            },
            else => {},
        }
    }
    const base = readIovecWord(h, proc, iov_va, fd_abi.iovec_base_offset) orelse return sc.syscall_err_invalid;
    const len = readIovecWord(h, proc, iov_va, fd_abi.iovec_len_offset) orelse return sc.syscall_err_invalid;
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
        fd_abi.fcntl_dup => if (arg0 >= kernel.fd_number_capacity) sc.syscall_err_invalid else state.dupFd(proc, fd, @intCast(arg0), kernel.fdRightsFromBits(arg1), .{}) catch sc.syscall_err_invalid,
        else => sc.syscall_err_invalid,
    };
}

// Only timer readability consumes time. Keep one fresh sample per pass,
// including the separate pass after waiter registration, without MMIO for IPC.
fn pollNowNs(state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, events: u64, now: *?u64) u64 {
    if (now.*) |sample| return sample;
    if ((events & fd_abi.event_readable) == 0) return 0;
    const view = state.fdPayloadWithRightsConst(proc, fd, .{ .poll = true }) orelse return 0;
    if (view.payload.* != .timer) return 0;
    const sample = timerNowNs();
    now.* = sample;
    return sample;
}

fn readPollItems(h: anytype, proc: kernel.PrincipalId, pollfds_va: u64, count: u64, out: []PollItem) ?[]PollItem {
    if (pollfds_va == 0 or count > fd_abi.max_pollfds or count > out.len) return null;
    _ = std_rc.math.add(u64, pollfds_va, count * fd_abi.pollfd_size) catch return null;
    var wire_storage: [max_pollfds * fd_abi.pollfd_size]u8 = undefined;
    const bytes = wire_storage[0 .. @as(usize, @intCast(count)) * fd_abi.pollfd_size];
    // One bounded snapshot avoids per-entry user walks and TOCTOU re-reads.
    if (count != 0 and !h.copy_user_bytes_from_va(proc, pollfds_va, bytes)) return null;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const item_va = pollfds_va + @as(u64, @intCast(i)) * fd_abi.pollfd_size;
        const wire = bytes[i * fd_abi.pollfd_size ..][0..fd_abi.pollfd_size];
        const fd_u64 = @import("std").mem.readInt(u64, wire[fd_abi.pollfd_fd_offset..][0..8], .little);
        const events = @import("std").mem.readInt(u64, wire[fd_abi.pollfd_events_offset..][0..8], .little);
        const min_write_bytes = @import("std").mem.readInt(u64, wire[fd_abi.pollfd_min_write_bytes_offset..][0..8], .little);
        if (fd_u64 >= kernel.fd_number_capacity or (events & ~fd_abi.event_known_mask) != 0) return null;
        out[i] = .{
            .fd = @intCast(fd_u64),
            .events = events,
            .min_write_bytes = if ((events & fd_abi.event_writable) != 0) min_write_bytes else 0,
            .item_va = item_va,
        };
    }
    return out[0..@intCast(count)];
}

test "poll item snapshot copies one range and preserves validation" {
    const std = @import("std");
    const Mock = struct {
        const base: u64 = 0x1ff0; // Forward a page-crossing item unchanged.
        var bytes: [max_pollfds * fd_abi.pollfd_size]u8 = undefined;
        var calls: usize = 0;
        var fail_at: usize = 0;
        fn copy(_: kernel.PrincipalId, va: u64, dest: []u8) bool {
            calls += 1;
            if (calls == fail_at) return false;
            std.debug.assert(dest.len <= bytes.len and dest.len % fd_abi.pollfd_size == 0);
            std.debug.assert(va == base);
            const offset: usize = @intCast(va - base);
            @memcpy(dest, bytes[offset..][0..dest.len]);
            return true;
        }
    };
    const h = .{ .copy_user_bytes_from_va = Mock.copy };
    const owner = kernel.processPrincipalFromIndex(0).?;
    var storage: [2]PollItem = undefined;
    for ([_]u64{ 16, fd_abi.event_readable, 999, 0xffff, 17, fd_abi.event_writable, 123, 0xffff }, 0..) |value, i|
        std.mem.writeInt(u64, Mock.bytes[i * 8 ..][0..8], value, .little);
    Mock.calls = 0;
    Mock.fail_at = 0;
    const items = readPollItems(&h, owner, Mock.base, 2, &storage).?;
    try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    try std.testing.expectEqualDeep(PollItem{ .fd = 16, .events = fd_abi.event_readable, .min_write_bytes = 0, .item_va = Mock.base }, items[0]);
    try std.testing.expectEqualDeep(PollItem{ .fd = 17, .events = fd_abi.event_writable, .min_write_bytes = 123, .item_va = Mock.base + 32 }, items[1]);
    Mock.calls = 0;
    try std.testing.expect(readPollItems(&h, owner, 0, 1, &storage) == null);
    try std.testing.expect(readPollItems(&h, owner, Mock.base, fd_abi.max_pollfds + 1, &storage) == null);
    try std.testing.expect(readPollItems(&h, owner, Mock.base, 3, &storage) == null);
    try std.testing.expectEqual(@as(usize, 0), readPollItems(&h, owner, Mock.base, 0, &storage).?.len);
    try std.testing.expectEqual(@as(usize, 0), Mock.calls);
    Mock.fail_at = 1;
    try std.testing.expect(readPollItems(&h, owner, Mock.base, 2, &storage) == null);
    try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    Mock.fail_at = 0;
    Mock.calls = 0;
    std.mem.writeInt(u64, Mock.bytes[8..16], 1 << 63, .little);
    try std.testing.expect(readPollItems(&h, owner, Mock.base, 1, &storage) == null);
    std.mem.writeInt(u64, Mock.bytes[8..16], fd_abi.event_readable, .little);
    for ([_]u64{ 4097, 0x80000000, kernel.fd_number_capacity - 1 }) |fd| {
        Mock.calls = 0;
        std.mem.writeInt(u64, Mock.bytes[0..8], fd, .little);
        try std.testing.expectEqual(fd, readPollItems(&h, owner, Mock.base, 1, &storage).?[0].fd);
    }
    for ([_]u64{ fd_abi.thread_self_fd, std.math.maxInt(u32), (@as(u64, 1) << 32) | 16 }) |fd| {
        Mock.calls = 0;
        std.mem.writeInt(u64, Mock.bytes[0..8], fd, .little);
        try std.testing.expect(readPollItems(&h, owner, Mock.base, 1, &storage) == null);
    }
    var many: [max_pollfds]PollItem = undefined;
    for (0..max_pollfds) |i| {
        const wire = Mock.bytes[i * fd_abi.pollfd_size ..][0..fd_abi.pollfd_size];
        std.mem.writeInt(u64, wire[0..8], 16, .little);
        std.mem.writeInt(u64, wire[8..16], fd_abi.event_writable, .little);
        std.mem.writeInt(u64, wire[16..24], 58, .little);
        std.mem.writeInt(u64, wire[24..32], std.math.maxInt(u64), .little);
    }
    Mock.calls = 0;
    try std.testing.expectEqual(max_pollfds, readPollItems(&h, owner, Mock.base, max_pollfds, &many).?.len);
    try std.testing.expectEqual(@as(usize, 1), Mock.calls);
    try std.testing.expectEqual(@as(u64, 58), many[max_pollfds - 1].min_write_bytes);
    try std.testing.expect(readPollItems(&h, owner, std.math.maxInt(u64) - 8, 1, &many) == null);
}

fn pollCached(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, items: []const PollItem, frame: *TrapFrame) ?u64 {
    const poll_scan_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.poll_scan, poll_scan_sample);
    var ready_count: u64 = 0;
    var first_index: usize = 0;
    var first_events: u64 = 0;
    var now: ?u64 = null;
    for (items, 0..) |item, index| {
        const now_ns = pollNowNs(state, proc, item.fd, item.events, &now);
        const revents = state.fdPollEventsWithWriteMin(proc, item.fd, item.events, now_ns, item.min_write_bytes) orelse return null;
        if (revents == 0) continue;
        ready_count += 1;
        if (ready_count == 1) {
            first_index = index;
            first_events = revents;
        } else {
            if (ready_count == 2 and !h.write_user_u64(proc, items[first_index].item_va + fd_abi.pollfd_revents_offset, first_events)) return null;
            if (!h.write_user_u64(proc, item.item_va + fd_abi.pollfd_revents_offset, revents)) return null;
        }
    }
    if (ready_count == 1) {
        frame.r10 = first_index;
        frame.r8 = first_events;
    }
    return ready_count;
}

fn registerCachedWaitersForPoll(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    items: []const PollItem,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
    group: kernel.FdWaitGroupRef,
) kernel.KernelError!void {
    for (items) |item| {
        const reg_task_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        const task_registered = try state.registerTaskReadableWaiterForFd(
            proc,
            item.fd,
            item.events,
            item.item_va,
            thread_index,
            thread_generation,
            wait_token,
            group,
        );
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.reg_task, reg_task_sample);
        if (task_registered) continue;
        const reg_pipe_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        const pipe_registered = try state.registerPipeWaiterForFd(
            proc,
            item.fd,
            item.events,
            item.item_va,
            item.min_write_bytes,
            thread_index,
            thread_generation,
            wait_token,
            group,
        );
        if (comptime lock_profile.enabled) lock_profile.stageEnd(.reg_pipe, reg_pipe_sample);
        if (pipe_registered) continue;
        const reg_ipc_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
        defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.reg_ipc, reg_ipc_sample);
        _ = try state.registerIpcPollWaitersForFd(
            proc,
            item.fd,
            item.events,
            item.item_va,
            thread_index,
            thread_generation,
            wait_token,
            group,
        );
    }
}

pub fn reclaimStaleFdWaitGroups(state: *kernel.KernelState) void {
    var groups = state.fd_wait_groups.iterator();
    while (groups.next()) |entry| {
        const slot = entry.value;
        if (scheduler.waitTokenIsCurrent(slot.owner, slot.thread_index, slot.thread_generation, slot.wait_token)) continue;
        state.cancelFdWaitGroup(.{ .index = entry.ref.index, .generation = entry.ref.generation }, slot.wait_token);
    }
}

pub fn beginFdWaitGroupWithReclaim(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
) kernel.KernelError!kernel.FdWaitGroupRef {
    return state.beginFdWaitGroup(
        owner,
        thread_index,
        thread_generation,
        wait_token,
    ) catch |err| switch (err) {
        kernel.KernelError.TableFull, kernel.KernelError.OutOfFreePages => {
            reclaimStaleFdWaitGroups(state);
            return state.beginFdWaitGroup(
                owner,
                thread_index,
                thread_generation,
                wait_token,
            );
        },
        else => return err,
    };
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
        state.unregisterIpcPollWaitersForFd(proc, item.fd, thread_index, thread_generation);
    }
    state.unregisterTaskReadableWaiterForThread(thread_index, thread_generation);
}

fn timerNowNs() u64 {
    return @import("../realtime_clock.zig").monotonicNs() orelse scheduler.lapic_tick_count * 1_000_000;
}

fn replacePersistentSet(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId,
    address: u64, count: u64, thread: usize, generation: u32) kernel.KernelError!*kernel.persistent_wait.Set {
    const wait_register_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.wait_register, wait_register_sample);
    var storage: [max_pollfds]PollItem = undefined;
    const items = readPollItems(h, proc, address, count, &storage) orelse return kernel.KernelError.InvalidState;
    const set = try kernel.persistent_wait.draft(state, proc, thread, generation, items.len);
    errdefer kernel.persistent_wait.destroy(state, set);
    // The transient group is only a failure-atomic construction transaction.
    // K excludes producers until every binding is converted; none of its token
    // or user-VA state becomes part of the published persistent subscriptions.
    const group = try state.beginFdWaitGroup(proc, thread, generation, 1);
    defer state.cancelFdWaitGroup(group, 1);
    const now = timerNowNs();
    for (items, 0..) |source, i| {
        const fd = state.fdEntryConst(proc, source.fd) orelse return kernel.KernelError.InvalidState;
        const ready = state.fdPollEventsWithWriteMin(proc, source.fd, source.events, now,
            source.min_write_bytes) orelse return kernel.KernelError.InvalidState;
        const item = set.items.get(@intCast(i)).?;
        item.* = .{ .set = set, .index = @intCast(i), .fd = source.fd,
            .descriptor_seq = fd.seq, .object = fd.object, .events = source.events,
            .min_write_bytes = source.min_write_bytes };
        const object = state.kernelObjectSlot(item.object).?;
        object.persistent_observers = std_rc.math.add(u32, object.persistent_observers, 1) catch
            return kernel.KernelError.TableFull;
        item.observing = true;
        const begin = state.fd_wait_groups.get(group.index).?.link_count;
        try registerCachedWaitersForPoll(state, proc, &.{source}, thread, generation, 1, group);
        const draft = state.fd_wait_groups.get(group.index).?;
        if (draft.link_count - begin > item.registrations.len) return kernel.KernelError.InvalidState;
        for (begin..draft.link_count) |index| {
            item.registrations[item.registration_count] = draft.links.?.get(@intCast(index)).?.registration;
            item.registration_count += 1;
        }
        if (ready != 0) item.publish();
    }
    // Prepare the directory publication before changing any source binding.
    _ = state.persistent_wait_threads.claimIndex(@intCast(thread)) catch return kernel.KernelError.OutOfFreePages;
    var iterator = set.items.iterator();
    while (iterator.next()) |entry| {
        const item = entry.value;
        for (item.registrations[0..item.registration_count]) |registration|
            state.makePersistentRegistration(registration, item);
    }
    set.table_mutation = (try state.fdTableForActiveProcessConst(proc)).mutation;
    try kernel.persistent_wait.commit(state, set);
    return set;
}

fn persistentReady(h: anytype, state: *kernel.KernelState, set: *kernel.persistent_wait.Set,
    output: u64, frame: *TrapFrame) u64 {
    const wait_set_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    defer if (comptime lock_profile.enabled) lock_profile.stageEnd(.wait_set, wait_set_sample);
    if (!kernel.persistent_wait.validate(state, set)) return fd_abi.wait_set_invalidated;
    // Only entries present at entry are examined: requeued level-ready entries
    // cannot monopolize one invocation. Idle readiness is a hint, never authority.
    const limit = set.queued;
    var count: u64 = 0;
    var first_index: u32 = 0;
    var first_events: u64 = 0;
    const now = timerNowNs();
    for (0..limit) |_| {
        const item = set.pop().?;
        const events = state.fdPollEventsWithWriteMin(set.owner, item.fd, item.events, now,
            item.min_write_bytes) orelse return fd_abi.wait_set_invalidated;
        if (item.object.kind == .timer) state.markTimerDeadlineDirty();
        if (events == 0) continue;
        item.publish();
        count += 1;
        if (count == 1) { first_index = item.index; first_events = events; }
        if (count == 2 and !h.write_user_u64(set.owner, output + first_index * fd_abi.pollfd_size +
            fd_abi.pollfd_revents_offset, first_events)) return 0 -% sc.syscall_err_invalid;
        if (count > 1 and !h.write_user_u64(set.owner, output + item.index * fd_abi.pollfd_size +
            fd_abi.pollfd_revents_offset, events)) return 0 -% sc.syscall_err_invalid;
    }
    if (count == 1) {
        frame.r10 = first_index;
        frame.r8 = first_events;
    }
    return count;
}

/// Called with K after inner endpoint locks have ended. A marked set has at
/// most one invocation; source publication never copies user memory or calls T
/// under E. Timeout/signal invalidate the claim and retain only subscriptions.
pub fn completePersistentWaiters(h: anytype, state: *kernel.KernelState) void {
    var pending = state.persistent_wait_sets.marked(1);
    while (pending.next()) |index| {
        state.persistent_wait_sets.mark(@intCast(index), 1, false);
        const set = state.persistent_wait_sets.get(@intCast(index)).?;
        const invocation = set.invocation orelse continue;
        set.invocation = null;
        const claim = scheduler.claimWaitTokenCompletion(set.thread, set.thread_generation, set.owner, invocation.token);
        if (claim == .stale) continue;
        const target: kernel.ThreadWakeTarget = .{ .owner = set.owner, .thread_index = set.thread,
            .thread_generation = set.thread_generation, .wait_token = invocation.token };
        const output = scheduler.claimedWaitOutput(target);
        var registers: TrapFrame = undefined;
        registers.r10 = 0;
        registers.r8 = 0;
        var result = if (output) |address| persistentReady(h, state, set, address, &registers)
            else 0 -% sc.syscall_err_invalid;
        // Preserve the native singleton ABI on asynchronous completion too.
        // A foreign user-VA copy here would add VM/AS work to every wake even
        // though the claimed scheduler token can carry these two scalars.
        if (result == 1 and !scheduler.stagePollResult(target, registers.r10, registers.r8))
            result = 0 -% sc.syscall_err_invalid;
        _ = scheduler.publishClaimedWaitCompletion(set.thread, set.thread_generation, set.owner, invocation.token,
            if (result == 0) sc.syscall_err_not_ready else result);
    }
}

fn fdWaitPersistent(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId,
    frame: *TrapFrame, flags: u64) u64 {
    const operation = flags & (fd_abi.wait_set_replace | fd_abi.wait_set_reuse | fd_abi.wait_set_clear);
    if (operation == 0 or operation & (operation - 1) != 0 or
        flags & ~(fd_abi.wait_uninterruptible | fd_abi.wait_set_replace | fd_abi.wait_set_reuse | fd_abi.wait_set_clear) != 0)
        return 0 -% sc.syscall_err_invalid;
    const thread = scheduler.currentThread();
    const generation = scheduler.currentThreadGeneration() orelse return 0 -% sc.syscall_err_invalid;
    if (operation == fd_abi.wait_set_clear) {
        if (frame.rdi != 0 or frame.rsi != 0 or frame.rdx != 0 or flags != operation) return 0 -% sc.syscall_err_invalid;
        kernel.persistent_wait.clearThread(state, proc, thread, generation);
        return sc.syscall_ok;
    }
    if (!persistentOutputValid(frame.rdi, frame.rsi)) return 0 -% sc.syscall_err_invalid;
    const set = if (operation == fd_abi.wait_set_replace)
        replacePersistentSet(h, state, proc, frame.rdi, frame.rsi, thread, generation) catch |err| return 0 -% statusFromKernelError(err)
    else kernel.persistent_wait.current(state, proc, thread, generation) orelse return fd_abi.wait_set_invalidated;
    if (set.count != frame.rsi) return 0 -% sc.syscall_err_invalid;
    set.invocation = null;
    const ready = persistentReady(h, state, set, frame.rdi, frame);
    if (ready != 0) return ready;
    if (frame.rdx == 0) return sc.syscall_err_not_ready;
    const token = scheduler.reserveCurrentWaitToken(generation) orelse return 0 -% sc.syscall_err_invalid;
    set.invocation = .{ .token = token };
    // Registration, readiness observation and parking are under K. A producer
    // either populated the FIFO above or will mark this exact invocation.
    const ticks = if (frame.rdx == fd_abi.wait_forever) 0 else frame.rdx;
    lock_profile.parkBegin();
    defer lock_profile.parkEnd();
    if (h.block_current_thread_for_event(frame, true, flags & fd_abi.wait_uninterruptible != 0,
        token, ticks, sc.syscall_err_not_ready, h.before_current_thread_leave)) return frame.rax;
    scheduler.cancelCurrentWaitToken(generation, token);
    set.invocation = null;
    return sc.syscall_err_not_ready;
}

fn persistentOutputValid(address: u64, count: u64) bool {
    if (count > fd_abi.max_pollfds) return false;
    if (count == 0) return true;
    const bytes = count * fd_abi.pollfd_size;
    return address >= 4096 and address < 0x0000_8000_0000_0000 and
        bytes <= 0x0000_8000_0000_0000 - address;
}

fn fdPoll(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    frame.r10 = 0;
    frame.r8 = 0;
    var storage: [max_pollfds]PollItem = undefined;
    const items = readPollItems(h, proc, frame.rdi, frame.rsi, &storage) orelse return sc.syscall_err_invalid;
    return pollCached(h, state, proc, items, frame) orelse sc.syscall_err_invalid;
}

fn fdWaitMany(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const pollfds_va = frame.rdi;
    const count = frame.rsi;
    const timeout_ticks = frame.rdx;
    const flags = frame.r10;
    if (comptime lock_profile.enabled) {
        const operation = flags & (fd_abi.wait_set_replace | fd_abi.wait_set_reuse | fd_abi.wait_set_clear);
        if (operation == 0 or operation == fd_abi.wait_set_replace) {
            // Only the kernel can pair this principal with the K lock hold
            // that ends at a scheduler leave or syscall return.
            lock_profile.waitCaller(@intFromEnum(proc), state.tryProcessLabel(proc) orelse kernel.principalLabel(proc), count,
                operation == fd_abi.wait_set_replace);
        }
    }
    const trace = @import("../wake_trace.zig");
    trace.record(.fd_wait_enter, count, pollfds_va, scheduler.currentThread());
    // INVALID and singleton count both have numeric value 1. A nonzero
    // event register distinguishes success, including error returns before block.
    frame.r10 = 0;
    frame.r8 = 0;
    if (flags & (fd_abi.wait_set_replace | fd_abi.wait_set_reuse | fd_abi.wait_set_clear) != 0)
        return fdWaitPersistent(h, state, proc, frame, flags);
    if ((flags & ~fd_abi.wait_uninterruptible) != 0) return sc.syscall_err_invalid;
    var poll_items_storage: [max_pollfds]PollItem = undefined;
    const poll_items = readPollItems(h, proc, pollfds_va, count, poll_items_storage[0..]) orelse return sc.syscall_err_invalid;
    const ready = pollCached(h, state, proc, poll_items, frame) orelse return sc.syscall_err_invalid;
    if (ready != 0) return ready;
    if (timeout_ticks == 0) return sc.syscall_err_not_ready;

    const current_thread = scheduler.currentThread();
    const current_generation = scheduler.currentThreadGeneration() orelse return sc.syscall_err_invalid;
    const register_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    state.unregisterFdWaitersForThread(proc, current_thread, current_generation);
    const wait_token = scheduler.reserveCurrentWaitToken(current_generation) orelse
        return sc.syscall_err_invalid;
    const wait_group_sample = if (comptime lock_profile.enabled) lock_profile.stageBegin() else {};
    const group = beginFdWaitGroupWithReclaim(
        state,
        proc,
        current_thread,
        current_generation,
        wait_token,
    ) catch |err| {
        scheduler.cancelCurrentWaitToken(current_generation, wait_token);
        return statusFromKernelError(err);
    };
    state.fd_wait_groups.get(group.index).?.pollfds_va = pollfds_va;
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.wait_group, wait_group_sample);
    const register_start = ipc_metric.timestamp();
    registerCachedWaitersForPoll(
        state,
        proc,
        poll_items,
        current_thread,
        current_generation,
        wait_token,
        group,
    ) catch |err| {
        scheduler.cancelCurrentWaitToken(current_generation, wait_token);
        state.cancelFdWaitGroup(group, wait_token);
        return statusFromKernelError(err);
    };
    state.armFdWaitGroup(group) catch |err| {
        scheduler.cancelCurrentWaitToken(current_generation, wait_token);
        state.cancelFdWaitGroup(group, wait_token);
        return statusFromKernelError(err);
    };
    ipc_metric.record(.wait_register, register_start);
    if (comptime lock_profile.enabled) lock_profile.stageEnd(.wait_register, register_sample);
    trace.record(.fd_wait_register, count, wait_token, current_thread);
    const repoll_start = ipc_metric.timestamp();
    const ready_after_register = pollCached(h, state, proc, poll_items, frame) orelse {
        scheduler.cancelCurrentWaitToken(current_generation, wait_token);
        state.cancelFdWaitGroup(group, wait_token);
        return sc.syscall_err_invalid;
    };
    ipc_metric.record(.wait_repoll, repoll_start);
    trace.record(.fd_wait_repoll, count, wait_token, current_thread);
    if (ready_after_register != 0) {
        scheduler.cancelCurrentWaitToken(current_generation, wait_token);
        state.cancelFdWaitGroup(group, wait_token);
        return ready_after_register;
    }

    const block_ticks: u64 = if (timeout_ticks == fd_abi.wait_forever) 0 else timeout_ticks;
    trace.record(.fd_wait_block, count, wait_token, current_thread);
    lock_profile.parkBegin();
    defer lock_profile.parkEnd();
    if (h.block_current_thread_for_event(
        frame,
        true,
        (flags & fd_abi.wait_uninterruptible) != 0,
        wait_token,
        block_ticks,
        sc.syscall_err_not_ready,
        h.before_current_thread_leave,
    )) return frame.rax;
    scheduler.cancelCurrentWaitToken(current_generation, wait_token);
    state.cancelFdWaitGroup(group, wait_token);
    return sc.syscall_err_not_ready;
}

fn nanosToTimespec(ns: u64) struct { sec: u64, nsec: u64 } {
    return .{
        .sec = ns / 1_000_000_000,
        .nsec = ns % 1_000_000_000,
    };
}

fn timespecToNanos(sec: u64, nsec: u64) ?u64 {
    if (nsec >= 1_000_000_000) return null;
    const sec_ns, const sec_overflow = @mulWithOverflow(sec, 1_000_000_000);
    if (sec_overflow != 0) return null;
    const total_ns, const add_overflow = @addWithOverflow(sec_ns, nsec);
    if (add_overflow != 0) return null;
    return total_ns;
}

fn readTimerSpec(h: anytype, proc: kernel.PrincipalId, spec_va: u64) ?struct { value_ns: u64, interval_ns: u64 } {
    if (spec_va == 0) return null;
    const interval_sec = h.read_user_u64(proc, spec_va + fd_abi.timerfd_spec_interval_sec_offset) orelse return null;
    const interval_nsec = h.read_user_u64(proc, spec_va + fd_abi.timerfd_spec_interval_nsec_offset) orelse return null;
    const value_sec = h.read_user_u64(proc, spec_va + fd_abi.timerfd_spec_value_sec_offset) orelse return null;
    const value_nsec = h.read_user_u64(proc, spec_va + fd_abi.timerfd_spec_value_nsec_offset) orelse return null;
    return .{
        .value_ns = timespecToNanos(value_sec, value_nsec) orelse return null,
        .interval_ns = timespecToNanos(interval_sec, interval_nsec) orelse return null,
    };
}

fn writeTimerSpec(h: anytype, proc: kernel.PrincipalId, spec_va: u64, state: kernel.TimerFdState) u64 {
    if (spec_va == 0) return sc.syscall_ok;
    const interval = nanosToTimespec(state.interval_ns);
    const value = nanosToTimespec(state.remaining_ns);
    if (!h.write_user_u64(proc, spec_va + fd_abi.timerfd_spec_interval_sec_offset, interval.sec)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, spec_va + fd_abi.timerfd_spec_interval_nsec_offset, interval.nsec)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, spec_va + fd_abi.timerfd_spec_value_sec_offset, value.sec)) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, spec_va + fd_abi.timerfd_spec_value_nsec_offset, value.nsec)) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn timerfdCreate(state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    if (frame.rdi != fd_abi.timerfd_clock_monotonic) return sc.syscall_err_invalid;
    if ((frame.rsi & ~fd_abi.timerfd_known_flags_mask) != 0) return sc.syscall_err_invalid;
    const deadline_ns = timerDeadline(frame.rdx, frame.rsi, timerNowNs()) orelse return sc.syscall_err_invalid;
    return state.createTimerFd(
        proc,
        deadline_ns,
        frame.r10,
        kernel.fdFlagsFromBits(@truncate(frame.r9)),
        kernel.fdRightsFromBits(frame.r8),
        first_dynamic_fd,
    ) catch |err| statusFromKernelError(err);
}

fn timerDeadline(value_ns: u64, flags: u64, now_ns: u64) ?u64 {
    return if (value_ns == 0)
        0
    else if ((flags & fd_abi.timerfd_flag_abstime) != 0)
        value_ns
    else
        @import("std").math.add(u64, now_ns, value_ns) catch null;
}

fn timerfdSettime(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    const fd: kernel.Fd = @intCast(frame.rdi);
    const flags = frame.rsi;
    if ((flags & ~fd_abi.timerfd_known_flags_mask) != 0) return sc.syscall_err_invalid;
    const spec = readTimerSpec(h, proc, frame.rdx) orelse return sc.syscall_err_invalid;
    const now_ns = timerNowNs();
    const deadline_ns = timerDeadline(spec.value_ns, flags, now_ns) orelse return sc.syscall_err_invalid;
    const old_state = state.timerFdState(proc, fd, now_ns) orelse return sc.syscall_err_invalid;
    const old_status = writeTimerSpec(h, proc, frame.r10, old_state);
    if (old_status != sc.syscall_ok) return old_status;
    state.setTimerFd(proc, fd, deadline_ns, spec.interval_ns, @truncate(flags)) catch return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn timerfdGettime(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, out_va: u64) u64 {
    if (out_va == 0) return sc.syscall_err_invalid;
    const timer_state = state.timerFdState(proc, fd, timerNowNs()) orelse return sc.syscall_err_invalid;
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

fn residentAnonymousUserOutput(state: *kernel.KernelState, proc: kernel.PrincipalId, va: u64, len: u64) bool {
    const end = std_rc.math.add(u64, va, len) catch return false;
    if (!user_vm.lockVmTransaction(proc)) return false;
    defer user_vm.unlockVmTransaction(proc);
    var page = va & ~@as(u64, 4095);
    while (page < end) : (page += 4096) {
        const entry = state.vmaEntryForVaConst(proc, page) orelse return false;
        const vmo = state.nativeVmoSlotConst(entry.vmo) orelse return false;
        // WAIT writes before dequeuing. Keep its destination independent of
        // this pager's progress, including aliases and sparse anonymous VMOs.
        if (vmo.kind != .anonymous or !entry.prot.write or
            (state.entryDirtyPagePaddr(entry, page) == null and
            state.nativeVmoResolvedPagePaddr(entry.vmo, @intCast((entry.vmo_offset + page - entry.start_va) / 4096)) == null)) return false;
        if (page > std_rc.math.maxInt(u64) - 4096) return false;
    }
    return true;
}

fn pagerWait(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    if (frame.rdi >= kernel.fd_number_capacity or frame.rdx != 0 or
        !residentAnonymousUserOutput(state, proc, frame.rsi, @sizeOf(pager_abi.Event))) return sc.syscall_err_invalid;
    const port_ref = state.pagerPortRefForFd(proc, @intCast(frame.rdi), .{ .wait = true, .pager_fault = true }) orelse return sc.syscall_err_invalid;
    // Copyout is tested before dequeue. K serializes PAGER_WAIT consumers;
    // fault producers may append but cannot remove the chosen head.
    const zero = std_rc.mem.zeroes(pager_abi.Event);
    if (!h.copy_bytes_to_user_va(proc, frame.rsi, std_rc.mem.asBytes(&zero))) return sc.syscall_err_invalid;
    while (true) {
        if (state.peekPagerEvent(port_ref)) |event| {
            if (!h.copy_bytes_to_user_va(proc, frame.rsi, std_rc.mem.asBytes(&event))) return sc.syscall_err_invalid;
            if (!state.consumePagerEvent(port_ref, event)) return sc.syscall_err_closed;
            return sc.syscall_ok;
        }
        const generation = scheduler.currentThreadGeneration() orelse return sc.syscall_err_invalid;
        const token = scheduler.reserveCurrentWaitToken(generation) orelse return sc.syscall_err_not_ready;
        const registered = state.registerPagerPortWait(port_ref, proc, scheduler.currentThread(), generation, token) catch {
            scheduler.cancelCurrentWaitToken(generation, token);
            return sc.syscall_err_closed;
        };
        if (!registered) {
            scheduler.cancelCurrentWaitToken(generation, token);
            continue;
        }
        // The wait token latches a wake that arrives between registration and
        // park. No port, AS, shared-VM or copy-window lock crosses this call.
        if (h.block_current_thread_for_event(frame, true, false, token, 0, sc.syscall_err_not_ready, h.before_current_thread_leave))
            return frame.rax;
        scheduler.cancelCurrentWaitToken(generation, token);
        state.cancelPagerPortWait(port_ref, token);
        return sc.syscall_err_not_ready;
    }
}

fn pagerSupply(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) u64 {
    if (frame.rdi >= kernel.fd_number_capacity) return sc.syscall_err_invalid;
    const port_ref = state.pagerPortRefForFd(proc, @intCast(frame.rdi), .{ .pager_fault = true }) orelse return sc.syscall_err_invalid;
    var desc: pager_abi.Supply = undefined;
    if (!h.copy_user_bytes_from_va(proc, frame.rsi, std_rc.mem.asBytes(&desc))) return sc.syscall_err_invalid;
    if (desc.length == 0 or desc.length > pager_abi.max_supply_bytes or
        (desc.length | desc.source_offset | desc.target_offset) & 4095 != 0 or
        desc.source_fd >= kernel.fd_number_capacity) return sc.syscall_err_invalid;
    const count: usize = @intCast(desc.length / 4096);
    const source_view = state.fdPayloadWithRightsConst(proc, @intCast(desc.source_fd), .{ .map_read = true }) orelse return sc.syscall_err_invalid;
    const source_ref = switch (source_view.payload.*) {
        .vmo => |ref| ref,
        else => return sc.syscall_err_invalid,
    };
    var source_pages: [128]u64 = undefined;
    var target_ref: kernel.NativeVmoRef = .{};
    user_vm.lockSharedVmObjects();
    const source = state.nativeVmoSlotConst(source_ref) orelse {
        user_vm.unlockSharedVmObjects();
        return sc.syscall_err_invalid;
    };
    target_ref = state.pagerVmoForKey(port_ref, desc.key, desc.vmo_generation) orelse {
        user_vm.unlockSharedVmObjects();
        return sc.syscall_err_invalid;
    };
    const target = state.nativeVmoSlotConst(target_ref) orelse {
        user_vm.unlockSharedVmObjects();
        return sc.syscall_err_invalid;
    };
    const source_end = std_rc.math.add(u64, desc.source_offset, desc.length) catch 0;
    const target_end = std_rc.math.add(u64, desc.target_offset, desc.length) catch 0;
    if (source.kind != .anonymous or source_end == 0 or source_end > source.size_bytes or
        target.kind != .pager or target_end == 0 or target_end > target.size_bytes or
        !state.pagerRequestCovers(port_ref, target_ref, desc.request_id, @intCast(desc.target_offset / 4096), @intCast(count))) {
        user_vm.unlockSharedVmObjects();
        return sc.syscall_err_invalid;
    }
    for (0..count) |i| {
        const source_page: usize = @intCast(desc.source_offset / 4096 + i);
        source_pages[i] = state.nativeVmoResolvedPagePaddr(source_ref, source_page) orelse {
            user_vm.unlockSharedVmObjects();
            return sc.syscall_err_invalid;
        };
        const target_page: usize = @intCast(desc.target_offset / 4096 + i);
        if (state.nativeVmoResolvedPagePaddr(target_ref, target_page) != null or
            state.pagerPageFailed(target_ref, @intCast(target_page))) {
            user_vm.unlockSharedVmObjects();
            return sc.syscall_err_invalid;
        }
    }
    // The source FD may be closed as soon as shared-VM exclusion ends. Its
    // temporary ownership also prevents partial-unmap reclamation of the
    // sampled PFNs while the physical copy window reads them.
    state.retainNativeVmo(source_ref) catch {
        user_vm.unlockSharedVmObjects();
        return sc.syscall_err_alloc;
    };
    state.retainNativeVmo(target_ref) catch {
        state.releaseNativeVmoWithFreeList(source_ref, h.free_list);
        user_vm.unlockSharedVmObjects();
        return sc.syscall_err_alloc;
    };
    user_vm.unlockSharedVmObjects();
    defer state.releaseNativeVmoWithFreeList(source_ref, h.free_list);
    defer state.releaseNativeVmoWithFreeList(target_ref, h.free_list);
    var pages: [128]u64 = undefined;
    var staged: usize = 0;
    defer if (staged != 0) for (pages[0..staged]) |paddr| h.free_list.appendPage(paddr) catch |err| switch (err) {
        error.PageQuarantined => {},
        else => @panic("pager staging return failed"),
    };
    while (staged < count) : (staged += 1) {
        pages[staged] = (state.allocPhysicalPage(h.free_list) catch return sc.syscall_err_alloc).paddr;
        if (!kernel.KernelState.copyPhysicalPage(pages[staged], source_pages[staged])) {
            h.free_list.appendPage(pages[staged]) catch {};
            return sc.syscall_err_map;
        }
    }
    user_vm.lockSharedVmObjects();
    defer user_vm.unlockSharedVmObjects();
    // The page store checks every destination and grows metadata first, then
    // publishes the whole batch under one lock. Failed copy/validation leaves
    // every destination absent and returns all staging pages above.
    state.publishPagerPages(target_ref, @intCast(desc.target_offset / 4096), pages[0..count], h.free_list) catch return sc.syscall_err_map;
    staged = 0; // canonical VMO owns the pages from this point onward.
    state.completePagerPublishedRange(port_ref, target_ref, desc.key, desc.vmo_generation, @intCast(desc.target_offset / 4096), @intCast(count));
    return sc.syscall_ok;
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
    user_vm.lockAllVmTransactions();
    defer user_vm.unlockAllVmTransactions();
    if (comptime @import("../state/reference_check.zig").enabled) @import("../state/reference_check.zig").check(state) catch |err| @import("../state/reference_check.zig").failure(err);
    const vmo_ref = state.nativeVmoRefForRevokeFd(proc, fd) catch return sc.syscall_err_invalid;
    if (state.nativeVmoTreeMappingsOverlapPinnedUserObjects(vmo_ref)) return sc.syscall_err_invalid;
    _ = state.revokeVmoFdWithFreeList(proc, fd, free_list, VmoRevokeUnmapper{}) catch |err| switch (err) {
        kernel.KernelError.TableFull => return sc.syscall_err_alloc,
        else => return sc.syscall_err_invalid,
    };
    return sc.syscall_ok;
}

fn createPageViewVmoFd(
    h: anytype,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    parent_fd: kernel.Fd,
    indices_va: u64,
    page_count_u64: u64,
    rights_bits: u64,
    flags_bits: u64,
) u64 {
    if (indices_va == 0 or page_count_u64 == 0 or
        page_count_u64 > kernel.max_vmo_logical_pages or
        page_count_u64 > (@import("std").math.maxInt(u64) / fd_abi.vmo_page_view_index_size) or
        indices_va > @import("std").math.maxInt(u64) -
            page_count_u64 * fd_abi.vmo_page_view_index_size or
        (rights_bits & ~fd_abi.vmo_page_view_rights_mask) != 0 or
        (flags_bits & ~fd_abi.vmo_page_view_known_flags_mask) != 0)
    {
        reportPageViewRefusal("input", page_count_u64, null, null, "invalid-range-or-flags");
        return sc.syscall_err_invalid;
    }

    const rights = kernel.fdRightsFromBits(rights_bits);
    const flags = kernel.fdFlagsFromBits(@truncate(flags_bits & fd_abi.known_flags_mask));
    if (!rights.close or (!rights.map_read and !rights.map_write and !rights.map_exec) or
        parent_fd >= kernel.fd_number_capacity)
    {
        reportPageViewRefusal("rights", page_count_u64, null, null, "missing-map-or-close");
        return sc.syscall_err_invalid;
    }
    // Parent pages, cache policy and every derived alias must remain stable
    // from input staging until capability publication.
    user_vm.lockAllVmTransactions();
    defer user_vm.unlockAllVmTransactions();
    const parent_entry = state.fdEntryConst(proc, parent_fd) orelse {
        reportPageViewRefusal("parent-fd", page_count_u64, null, null, "not-found");
        return sc.syscall_err_invalid;
    };
    if (!parent_entry.rights.share or
        !kernel.isFdRightsSubset(rights, parent_entry.rights))
    {
        reportPageViewRefusal("parent-rights", page_count_u64, null, null, "not-subset");
        return sc.syscall_err_invalid;
    }
    const parent = state.nativeVmoRefForFd(proc, parent_fd) orelse {
        reportPageViewRefusal("parent-vmo", page_count_u64, null, null, "not-vmo");
        return sc.syscall_err_invalid;
    };
    const parent_slot = state.nativeVmoSlotConst(parent) orelse {
        reportPageViewRefusal("parent-vmo", page_count_u64, null, null, "stale");
        return sc.syscall_err_invalid;
    };
    if (parent_slot.kind != .anonymous or !parent_slot.backing.parent.ref.isNull() or
        !parent_slot.has_page_store)
    {
        reportPageViewRefusal("parent-kind", page_count_u64, null, null, "not-owned-ram");
        return sc.syscall_err_invalid;
    }

    const page_count: usize = @intCast(page_count_u64);
    const view = state.createNativePageView(parent, page_count) catch |err| {
        reportPageViewRefusal("create", page_count_u64, null, null, @errorName(err));
        return statusFromKernelError(err);
    };
    const view_slot = state.nativeVmoSlot(view) orelse unreachable;
    view_slot.page_view_sparse = flags_bits & fd_abi.vmo_page_view_sparse != 0;
    view_slot.page_view_max_prot = .{ .read = rights.map_read, .write = rights.map_write, .exec = rights.map_exec };
    const populated = populatePageView(h, state, proc, parent, view, indices_va, page_count);
    if (populated != sc.syscall_ok) {
        state.releaseNativeVmoWithFreeList(view, h.free_list);
        return populated;
    }

    const object = state.createKernelObject(.vmo, .{ .vmo = view }) catch |err| {
        reportPageViewRefusal("object", page_count_u64, null, null, @errorName(err));
        state.releaseNativeVmoWithFreeList(view, h.free_list);
        return statusFromKernelError(err);
    };
    return state.installFd(proc, object, rights, flags, first_dynamic_fd) catch |err| {
        reportPageViewRefusal("fd-install", page_count_u64, null, null, @errorName(err));
        if (state.kernelObjectSlot(object)) |slot|
            state.clearKernelObjectSlotWithFreeList(slot, h.free_list);
        return statusFromKernelError(err);
    };
}

// Input is read once into an unpublished metadata owner. A malformed late
// index therefore cannot partially modify any already published alias.
fn populatePageView(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, parent: kernel.NativeVmoRef, view: kernel.NativeVmoRef, indices_va: u64, page_count: usize) u64 {
    const sparse = (state.nativeVmoSlotConst(view) orelse return sc.syscall_err_invalid).page_view_sparse;
    var page_offset: usize = 0;
    while (page_offset < page_count) {
        var pages: [64]u64 = undefined;
        const chunk_count = @min(pages.len, page_count - page_offset);
        for (pages[0..chunk_count], 0..) |*paddr, chunk_index| {
            const list_index = page_offset + chunk_index;
            const list_va = indices_va +
                @as(u64, @intCast(list_index)) * fd_abi.vmo_page_view_index_size;
            const parent_page = h.read_user_u64(proc, list_va) orelse {
                reportPageViewRefusal("index-read", page_count, list_index, null, "unreadable");
                return sc.syscall_err_invalid;
            };
            if (sparse and parent_page == fd_abi.vmo_page_view_hole) {
                paddr.* = 0;
                continue;
            }
            if (parent_page > @import("std").math.maxInt(usize)) {
                reportPageViewRefusal("index-range", page_count, list_index, parent_page, "overflow");
                return sc.syscall_err_invalid;
            }
            paddr.* = state.nativeVmoResolvedPagePaddr(parent, @intCast(parent_page)) orelse {
                reportPageViewRefusal("page-resolve", page_count, list_index, parent_page, "absent-or-invalid");
                return sc.syscall_err_invalid;
            };
        }
        state.installNativeVmoPages(view, page_offset, pages[0..chunk_count], h.free_list) catch |err| {
            reportPageViewRefusal("install", page_count, page_offset, null, @errorName(err));
            return statusFromKernelError(err);
        };
        page_offset += chunk_count;
    }

    return sc.syscall_ok;
}

const PageViewInvalidator = struct {
    pub fn invalidate(_: @This(), owner: kernel.PrincipalId, start: u64, bytes: u64) bool {
        if (bytes > @import("std").math.maxInt(usize)) return false;
        return user_vm.invalidatePresentUserLinearRegionPtes(owner, start, @intCast(bytes));
    }
};

const PageViewDirtyWalker = struct {
    pub fn collect(_: @This(), owner: kernel.PrincipalId, start: u64, bytes: u64, first: u64, visitor: anytype) bool {
        return user_vm.collectUserPteDirty(owner, start, bytes, first, visitor);
    }
};

fn collectPageViewDirtyFd(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: u64, out_va: u64, words: u64, flags: u64) u64 {
    const dirty = kernel.page_view_dirty;
    if (fd >= kernel.fd_number_capacity or out_va == 0 or out_va & 7 != 0 or flags != 0 or !user_vm.pageViewDirtyTrackingAvailable()) return sc.syscall_err_invalid;
    user_vm.lockAllVmTransactions();
    defer user_vm.unlockAllVmTransactions();
    const view = state.nativeVmoRefForRevokeFd(proc, @intCast(fd)) catch return sc.syscall_err_invalid;
    const slot = state.nativeVmoSlot(view) orelse return sc.syscall_err_invalid;
    if (slot.kind != .page_view or words != dirty.wordCount(slot) or out_va > std_rc.math.maxInt(u64) - words * 8) return sc.syscall_err_invalid;
    // Entry preflight resolves writable output (including COW). All-AS retains
    // its physical identity through collection/copyout. Reject actual PFNs,
    // including parent-RAM aliases, before any D bit is cleared.
    var page_va = out_va & ~@as(u64, 4095);
    const last = (out_va + words * 8 - 1) & ~@as(u64, 4095);
    while (true) {
        const paddr = user_vm.lookupUserPhysicalPageWithAddressSpaceLocked(proc, .fromInt(page_va), true, false) orelse return sc.syscall_err_invalid;
        if (dirty.containsPhysicalPage(slot, paddr.toInt())) return sc.syscall_err_invalid;
        if (page_va == last) break;
        page_va += 4096;
    }
    dirty.harvest(state, view, PageViewDirtyWalker{}) catch return sc.syscall_err_invalid;
    const Sink = struct {
        hooks: @TypeOf(h),
        owner: kernel.PrincipalId,
        base: u64,
        pub fn write(sink: @This(), first: usize, values: []const u64) bool {
            return sink.hooks.copy_bytes_to_user_va(sink.owner, sink.base + first * 8, std_rc.mem.sliceAsBytes(values));
        }
    };
    if (!dirty.deliver(slot, Sink{ .hooks = h, .owner = proc, .base = out_va })) return sc.syscall_err_invalid;
    return sc.syscall_ok;
}

fn refreshPageViewVmoFd(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, view_fd: u64, parent_fd: u64, indices_va: u64, count: u64) u64 {
    if (view_fd >= kernel.fd_number_capacity or parent_fd >= kernel.fd_number_capacity or
        count == 0 or count > kernel.max_vmo_logical_pages or indices_va == 0 or
        indices_va > @import("std").math.maxInt(u64) - count * fd_abi.vmo_page_view_index_size)
        return sc.syscall_err_invalid;
    user_vm.lockAllVmTransactions();
    defer user_vm.unlockAllVmTransactions();
    const view = state.nativePageViewRefreshAuthority(proc, @intCast(view_fd), @intCast(parent_fd)) catch
        return sc.syscall_err_invalid;
    const slot = state.nativeVmoSlotConst(view) orelse return sc.syscall_err_invalid;
    if (count != slot.page_count) return sc.syscall_err_invalid;
    const staged = state.createNativePageView(slot.backing.parent.ref, @intCast(count)) catch |err|
        return statusFromKernelError(err);
    defer state.releaseNativeVmoWithFreeList(staged, h.free_list);
    const stage_slot = state.nativeVmoSlot(staged) orelse unreachable;
    stage_slot.page_view_sparse = slot.page_view_sparse;
    stage_slot.page_view_max_prot = slot.page_view_max_prot;
    const populated = populatePageView(h, state, proc, slot.backing.parent.ref, staged, indices_va, @intCast(count));
    if (populated != sc.syscall_ok) return populated;
    state.refreshNativePageView(view, staged, PageViewInvalidator{}) catch |err|
        return statusFromKernelError(err);
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
    const aligned_size = @as(u64, kernel.checkedVmoPageCount(size_bytes) catch return sc.syscall_err_invalid) * 4096;
    var prot = protFromBits(prot_bits) orelse return sc.syscall_err_invalid;
    const flags = mmapFlagsFromBits(flags_bits) orelse return sc.syscall_err_invalid;
    if (flags.anonymous and vmo_offset != 0) return sc.syscall_err_invalid;
    if ((vmo_offset & 0xFFF) != 0) return sc.syscall_err_invalid;
    if (requested_va == 0 and (flags.fixed or flags.fixed_noreplace)) return sc.syscall_err_invalid;
    prot.pkey = flags.pkey;
    if (!flags.anonymous) {
        const vmo_ref = state.nativeVmoRefForFd(proc, fd) orelse
            return sc.syscall_err_invalid;
        const vmo = state.nativeVmoSlotConst(vmo_ref) orelse
            return sc.syscall_err_invalid;
        if (vmo.kind == .page_view and
            (!flags.shared or flags.private or
                !kernel.KernelState.pageViewProtAllowed(prot, vmo.page_view_max_prot)))
            return sc.syscall_err_invalid;
        if (vmo.kind == .kernel_page and
            (!flags.shared or flags.private or prot.write or prot.exec))
            return sc.syscall_err_invalid;
    }
    if (!user_vm.lockVmTransaction(proc)) return sc.syscall_err_invalid;
    defer user_vm.unlockVmTransaction(proc);

    const use_requested_hint =
        requested_va != 0 and
        !flags.fixed and
        !flags.fixed_noreplace and
        (requested_va & 0xFFF) == 0 and
        (state.userMapRangeIsFree(proc, requested_va, aligned_size) catch false);
    const base_va = if (flags.fixed or flags.fixed_noreplace or use_requested_hint)
        requested_va
    else
        state.findRandomizedFreeUserMapVa(proc, aligned_size, 0x4644_4d4d_4150_0000 ^ scheduler.lapic_tick_count ^ @as(u64, fd)) catch |err| blk: {
            // Exhausting the preferred arena is a placement issue for every
            // mapping kind, not a NORESERVE/physical backing policy decision.
            // Fixed mappings and accepted hints never enter this fallback.
            if (err != kernel.KernelError.TableFull) return sc.syscall_err_map;
            break :blk findMremapMoveTarget(state, proc, aligned_size) orelse return sc.syscall_err_map;
        };
    if (!user_vm.validateUserLinearRegion(proc, base_va, @intCast(aligned_size))) return sc.syscall_err_invalid;
    if ((base_va & 0xFFF) != 0) return sc.syscall_err_invalid;
    if (flags.fixed) {
        // Prepare validates shared backing before replacing anything. All
        // fixed mappings install PTEs lazily through the native VMA fault
        // path, so no fallible eager PTE work follows the atomic commit.
        if (state.rangeOverlapsPinnedUserObject(proc, base_va, aligned_size)) {
            return sc.syscall_err_invalid;
        }
        const native_vma_source = nativeVmaCoversRange(state, proc, base_va, aligned_size);
        const reservation_slots_result = if (native_vma_source) user_vm.unmapPresentVmaSourceSplitSlotsRequired(
            proc,
            base_va,
            @intCast(aligned_size),
        ) else user_vm.unmapPresentUserLinearRegionSplitSlotsRequired(
            proc,
            base_va,
            @intCast(aligned_size),
        );
        const reservation_slots = reservation_slots_result orelse return sc.syscall_err_map;
        if (!user_vm.prepareUserReservationSlots(proc, reservation_slots)) return sc.syscall_err_alloc;
        const anonymous_max_prot = kernel.VmaProt{
            .read = true,
            .write = true,
            .exec = true,
            .pkey = flags.pkey,
        };
        var prepared = (if (flags.anonymous)
            state.prepareFixedAnonymousMmap(
                proc,
                base_va,
                aligned_size,
                prot,
                anonymous_max_prot,
                flags,
                free_list,
            )
        else
            state.prepareFixedFdMmap(
                proc,
                fd,
                base_va,
                aligned_size,
                prot,
                flags,
                vmo_offset,
                free_list,
            )) catch |err| return statusFromKernelError(err);
        defer state.discardFixedMmapPrepared(&prepared, free_list);
        const unmapped = if (native_vma_source)
            user_vm.unmapPresentVmaSource(proc, base_va, @intCast(aligned_size))
        else
            user_vm.unmapPresentUserLinearRegion(proc, base_va, @intCast(aligned_size));
        if (!unmapped) unreachable;
        state.commitFixedMmapPrepared(&prepared, free_list);
        return base_va;
    } else if (flags.fixed_noreplace) {
        if (!(state.userMapRangeIsFree(proc, base_va, aligned_size) catch false)) return sc.syscall_err_invalid;
    }

    if (flags.anonymous) {
        const max_prot = kernel.VmaProt{
            .read = true,
            .write = true,
            // The VM ABI represents read, write, and execute independently.
            // Preserve that full ceiling so mmap/mprotect can implement the
            // caller's requested permissions exactly.
            .exec = true,
            .pkey = flags.pkey,
        };
        _ = state.createAnonymousVmaWithPages(proc, base_va, aligned_size, prot, max_prot, flags, free_list) catch |err| {
            switch (err) {
                kernel.KernelError.TableFull => {
                    // Reported to userspace as ENOMEM, so say which table ran
                    // out rather than leaving the reader hunting for memory.
                    reportRefusal("anonymous_vma", err);
                    const capacity = state.anonymousMapCapacity(proc);
                    const store = kernel.vmObjectBackingStoreStats();
                    kernel_log.writeFmt(
                        "  anon_full vmas={}/{} vmos={}/{} store_used={} store_capacity={} store_free={} reservations_free={}\n",
                        .{
                            capacity.vmas_active,
                            capacity.vmas_capacity,
                            capacity.vmos_active,
                            capacity.vmos_capacity,
                            store.used_entries,
                            store.capacity,
                            store.free_entries,
                            user_vm.freeUserReservationSlotCount(proc),
                        },
                    );
                    kernel_log.writeFmt(
                        "  anon_full.shape anonymous={} single_page={} mapped_bytes={} span_lo={x} span_hi={x}\n",
                        .{
                            capacity.vmas_anonymous,
                            capacity.vmas_single_page,
                            capacity.vmas_bytes,
                            capacity.vmas_lowest_va,
                            capacity.vmas_highest_end_va,
                        },
                    );
                    return sc.syscall_err_alloc;
                },
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
    const mapped_vmo = state.nativeVmoRefForFd(proc, fd) orelse return sc.syscall_err_invalid;
    // These VMOs explicitly accept fault-time allocation failure. Leave even
    // populated pages to the normal fault path rather than requiring all holes.
    if (state.nativeVmoIsZeroOnDemand(mapped_vmo)) return base_va;
    // Explicit unavailable backing is a valid VMA, not an eager-map failure.
    // Fault-time lookup must distinguish its holes from demand-zero pages;
    // requiring a complete page sequence here would undo sparse admission.
    // Dense views (including ordinary GPU exports) retain eager validation.
    if (state.nativeVmoIsSparsePageView(mapped_vmo)) return base_va;
    // Device PTEs must carry the admitted PAT slot; the ordinary eager
    // shared-RAM path below deliberately has no device-cache parameter.
    if (state.nativeVmoDevicePatSlot(mapped_vmo) != null) return base_va;
    const page_count: usize = @intCast(aligned_size / 4096);
    const first = state.nativeVmaInitialMapping(proc, base_va) orelse {
        state.munmapRangeWithFreeList(proc, base_va, aligned_size, free_list) catch {};
        return sc.syscall_err_map;
    };
    const pages = user_vm.InitialVmaPages{ .state = state, .principal = proc, .base = base_va, .prot = first.prot };
    // Preserve the old PROT_NONE path's whole-source validation without PTEs.
    if (!prot.read and !prot.write and !prot.exec) {
        for (0..page_count) |index| {
            _ = pages.at(index) orelse {
                state.munmapRangeWithFreeList(proc, base_va, aligned_size, free_list) catch {};
                return sc.syscall_err_map;
            };
        }
        return base_va;
    }
    if (!user_vm.mapTrustedUserPageSequence(proc, base_va, page_count, pages, user_vm.InitialVmaPages.at, first.prot, false)) {
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
    const aligned_size = @as(u64, kernel.checkedVmoPageCount(size_bytes) catch return sc.syscall_err_invalid) * 4096;
    var prot = protFromBits(prot_bits) orelse return sc.syscall_err_invalid;

    if (!user_vm.lockVmTransaction(proc)) return sc.syscall_err_invalid;
    defer user_vm.unlockVmTransaction(proc);

    // DMA/MMIO objects retain the current address-to-physical identity.
    // A protection transition could make a read-only pin writable and then
    // move a private mapping through COW while the device still uses the old
    // page, so keep all overlapping transitions fail-closed until close.
    if (state.rangeOverlapsPinnedUserObject(proc, base_va, aligned_size)) {
        return sc.syscall_err_invalid;
    }

    const start_vma = state.vmaEntryForVaConst(proc, base_va) orelse return sc.syscall_err_invalid;
    if (!user_vm.validateUserLinearRegion(proc, base_va, @intCast(aligned_size))) return sc.syscall_err_invalid;
    if (base_va + aligned_size > start_vma.endVa()) return sc.syscall_err_invalid;
    // mprotect does not select a protection key.  Preserve the mapping's
    // original key instead of silently resetting it to key zero.
    prot.pkey = start_vma.max_prot.pkey;
    const unchanged_prot = @as(u8, @bitCast(start_vma.prot)) == @as(u8, @bitCast(prot));

    // start_vma already covers the complete range; no per-page walk is needed.
    state.setVmaProtRange(proc, base_va, aligned_size, prot) catch return sc.syscall_err_invalid;
    // Keep COW's narrower PTEs when the VMA protection did not change, but
    // retain the address-space validation normally performed by invalidation.
    if (unchanged_prot) {
        return if (user_vm.validateUserLinearRegion(proc, base_va, @intCast(aligned_size)))
            sc.syscall_ok
        else
            sc.syscall_err_map;
    }
    // Re-evaluate every present page from VMA/COW metadata after a protection
    // transition.  Directly restoring a writable PTE here bypassed the write
    // fault that separates fork-COW pages, including allocator arenas that
    // were PROT_NONE when fork occurred.  Invalidating preserves the address
    // reservation and lets the normal lazy-fault path install the exact new
    // protection (or allocate a private dirty page on the first write).
    if (!user_vm.invalidatePresentUserLinearRegionPtes(proc, base_va, @intCast(aligned_size))) {
        return sc.syscall_err_map;
    }
    return sc.syscall_ok;
}

fn findMremapMoveTarget(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    size_bytes: u64,
) ?u64 {
    const page_count = size_bytes / 4096;
    const align_pages = @import("kernel_abi_root").process_abi.user_aslr_granule / 4096;
    // Each retry jumps to the end of an entire colliding VMA/object.  The
    // K stabilizes the live blocker counts for this search. A fixed former
    // pool capacity would prematurely reject a gap after table growth.
    const vmas: u64 = if (state.getVmaTableConst(proc)) |table| table.active_count else 0;
    const max_attempts = vmas + @as(u64, state.fd_objects.live) + 1;
    var attempt: u64 = 0;
    while (attempt < max_attempts) : (attempt += 1) {
        const candidate = user_vm.findFreeUserMappingRange(proc, page_count, align_pages) orelse return null;
        const collision_end = state.userMapCollisionEndVa(proc, candidate, size_bytes) orelse
            return candidate;
        if (!user_vm.advanceFreeUserMappingSearch(proc, collision_end)) return null;
    }
    return null;
}

fn fixedMremapTargetIsVmaManaged(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    target_va: u64,
    size_bytes: u64,
) bool {
    var offset: u64 = 0;
    while (offset < size_bytes) : (offset += 4096) {
        const va = target_va + offset;
        if (!user_vm.userPageHasMappingOrReservation(proc, va)) continue;
        if (state.vmaEntryForVaConst(proc, va) == null) return false;
    }
    return true;
}

// Only fully VMA-owned ranges may treat supervisor seed PTEs as lazy holes.
// The caller holds the process VM transaction throughout validation and unmap.
fn nativeVmaCoversRange(state: *kernel.KernelState, proc: kernel.PrincipalId, start: u64, size: u64) bool {
    if (size == 0) return false;
    const end, const overflow = @addWithOverflow(start, size);
    if (overflow != 0) return false;
    var cursor = start;
    while (cursor < end) {
        const entry = state.vmaEntryForVaConst(proc, cursor) orelse return false;
        const next = entry.endVa();
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
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
    const old_size = @as(u64, kernel.checkedVmoPageCount(old_len) catch return sc.syscall_err_invalid) * 4096;
    const new_size = @as(u64, kernel.checkedVmoPageCount(new_len) catch return sc.syscall_err_invalid) * 4096;
    if ((flags_bits & ~vm_abi.mremap_known_flags) != 0) return sc.syscall_err_invalid;
    const may_move = (flags_bits & vm_abi.mremap_maymove) != 0;
    const fixed = (flags_bits & vm_abi.mremap_fixed) != 0;
    if (fixed and !may_move) return sc.syscall_err_invalid;
    if (!fixed and new_va != 0) return sc.syscall_err_invalid;
    const old_end, const old_overflow = @addWithOverflow(old_va, old_size);
    const requested_new_end, const new_overflow = @addWithOverflow(new_va, new_size);
    if (old_overflow != 0 or new_overflow != 0) return sc.syscall_err_invalid;
    if (fixed) {
        if (new_va < old_end and requested_new_end > old_va) {
            return sc.syscall_err_invalid;
        }
    }

    if (!user_vm.lockVmTransaction(proc)) return sc.syscall_err_invalid;
    defer user_vm.unlockVmTransaction(proc);

    const moves = fixed or new_size > old_size;
    const source = state.vmaEntryForVaConst(proc, old_va) orelse return sc.syscall_err_invalid;
    if (old_end > source.endVa()) return sc.syscall_err_invalid;
    if (state.rangeOverlapsPinnedUserObject(proc, old_va, old_size)) return sc.syscall_err_invalid;

    // musl probes the initial (file-backed) stack with a no-MAYMOVE growth.
    // A mapped but occupied extension is ENOMEM, not an unsupported-source
    // error. Check before source admission and before any
    // PTE or VMA mutation; the source's own remaining pages also collide.
    if (!may_move and new_size > old_size) {
        if (@addWithOverflow(old_va, new_size)[1] != 0) return sc.syscall_err_invalid;
        const tail_free = state.userMapRangeIsFree(proc, old_end, new_size - old_size) catch
            return sc.syscall_err_invalid;
        if (!tail_free) return sc.syscall_err_alloc;
    }
    if (!state.vmaRemapSourceAllowed(source, old_size, new_size)) return sc.syscall_err_invalid;

    const target_va = if (!moves)
        old_va
    else if (fixed)
        new_va
    else
        findMremapMoveTarget(state, proc, new_size) orelse {
            @import("../kernel_log.zig").writeFmt("vm: mremap target search failed principal={} old=0x{x}/0x{x} size=0x{x}\n", .{ @intFromEnum(proc), old_va, old_size, new_size });
            return sc.syscall_err_map;
        };
    if (moves and state.rangeOverlapsPinnedUserObject(proc, target_va, new_size)) {
        return sc.syscall_err_invalid;
    }
    if (fixed and !fixedMremapTargetIsVmaManaged(state, proc, target_va, new_size)) {
        return sc.syscall_err_invalid;
    }

    // Stop direct writes before the state layer retains or materializes a COW
    // view.  Invalidation preserves the source reservation and metadata, so a
    // later allocation failure merely causes lazy refaults in the old range.
    const invalidate_va = if (moves) old_va else old_va + new_size;
    const invalidate_size = if (moves) old_size else old_size - new_size;

    var reservation_split_slots: usize = 0;
    if (invalidate_size != 0) {
        reservation_split_slots += user_vm.unmapPresentVmaSourceSplitSlotsRequired(
            proc,
            invalidate_va,
            @intCast(invalidate_size),
        ) orelse {
            @import("../kernel_log.zig").writeFmt("vm: mremap source preflight failed principal={} va=0x{x} size=0x{x}\n", .{ @intFromEnum(proc), invalidate_va, invalidate_size });
            return sc.syscall_err_map;
        };
    }
    if (fixed) {
        reservation_split_slots += user_vm.unmapPresentUserLinearRegionSplitSlotsRequired(
            proc,
            target_va,
            @intCast(new_size),
        ) orelse return sc.syscall_err_map;
    }
    if (!user_vm.prepareUserReservationSlots(proc, reservation_split_slots)) {
        @import("../kernel_log.zig").writeFmt("vm: mremap reservation capacity failed principal={} slots={}\n", .{ @intFromEnum(proc), reservation_split_slots });
        return sc.syscall_err_alloc;
    }

    if (invalidate_size != 0 and
        !user_vm.invalidatePresentUserLinearRegionPtes(proc, invalidate_va, @intCast(invalidate_size)))
    {
        @import("../kernel_log.zig").writeFmt("vm: mremap invalidate failed principal={} va=0x{x} size=0x{x}\n", .{ @intFromEnum(proc), invalidate_va, invalidate_size });
        return sc.syscall_err_map;
    }

    var prepared = state.prepareMremapWithFreeList(proc, old_va, old_size, new_size, target_va, may_move, fixed, free_list) catch |err| {
        @import("../kernel_log.zig").writeFmt(
            "vm: mremap prepare failed principal={} old=0x{x}/0x{x} new=0x{x}/0x{x} free_pages={} error={s}\n",
            .{ @intFromEnum(proc), old_va, old_size, target_va, new_size, free_list.pageCount(), @errorName(err) },
        );
        return switch (err) {
            kernel.KernelError.OutOfFreePages, kernel.KernelError.TableFull => sc.syscall_err_alloc,
            else => sc.syscall_err_invalid,
        };
    };
    defer state.discardMremapPrepared(&prepared, free_list);

    // Destination backing and every metadata split are now prepared.  The
    // now may a fixed target cross the destructive commit boundary.
    if (fixed and !user_vm.unmapPresentUserLinearRegion(proc, target_va, @intCast(new_size))) {
        unreachable;
    }
    const result = state.commitMremapPrepared(&prepared, free_list);
    // The source PTEs are already absent, so releasing their reservation now
    // cannot leave a translation pointing at metadata/backing just freed by
    // the successful state transaction.
    if (invalidate_size != 0 and
        !user_vm.unmapPresentVmaSource(proc, invalidate_va, @intCast(invalidate_size)))
    {
        unreachable;
    }
    return result;
}

fn madviseVmaRange(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    base_va: u64,
    size_bytes: u64,
    advice: u64,
) u64 {
    // NORMAL/RANDOM/SEQUENTIAL/WILLNEED are paging hints. We accept them
    // without extra prefetch; advice requiring discard or inheritance changes
    // must not falsely succeed until those operations are implemented.
    // POPULATE has already performed checked, pager-aware prefault in syscall
    // preflight before K. Revalidate the VMA coverage here against concurrent
    // mapping changes; do not turn unsupported discard into a successful hint.
    if (advice > 3 and advice != vm_abi.madvise_populate_read and
        advice != vm_abi.madvise_populate_write) return sc.syscall_err_invalid;
    if ((base_va & 0xFFF) != 0) return sc.syscall_err_invalid;
    if (size_bytes == 0) return sc.syscall_ok;
    const size = pageAlignUp(size_bytes) orelse return sc.syscall_err_invalid;
    const end, const overflow = @addWithOverflow(base_va, size);
    _ = end;
    if (overflow != 0) return sc.syscall_err_invalid;
    if (!user_vm.lockVmTransaction(proc)) return sc.syscall_err_invalid;
    defer user_vm.unlockVmTransaction(proc);
    if (!nativeVmaCoversRange(state, proc, base_va, size)) return sc.syscall_err_map;
    return sc.syscall_ok;
}

fn writeFdInfo(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, out_va: u64) u64 {
    if (out_va == 0) return sc.syscall_err_invalid;
    // fd_get_info already holds KernelState. Release the shared VM lock
    // before copyout, which can acquire an address-space lock and fault.
    const info = blk: {
        user_vm.lockSharedVmObjects();
        defer user_vm.unlockSharedVmObjects();
        var snapshot = state.fdInfo(proc, fd) orelse return sc.syscall_err_invalid;
        if (snapshot.kind == .vmo)
            snapshot.extra = state.fdVmoLifetimeInfo(proc, fd) orelse 0;
        break :blk snapshot;
    };
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

fn fdIoctl(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, request: u64, arg_va: u64, _: *TrapFrame) u64 {
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

// Close and wake incrementally: lifecycle teardown must cover all descriptors
// without an FD-capacity-sized stack array or a truncated wake snapshot.
pub fn closeProcessFdsWithWakes(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, cloexec_only: bool) void {
    const table = state.getFdTableConst(proc) orelse return;
    for (0..table.capacity) |index| {
        const entry = table.at(index).*;
        if (entry.object.isNull() or (cloexec_only and !entry.flags.cloexec)) continue;
        const fd: kernel.Fd = @intCast(index);
        const endpoint = state.pipeEndpointForFd(proc, fd);
        const channel = ipcChannelHandleForFd(state, proc, fd);
        const notify_snapshot = if (channel) |handle| state.ipcChannelNotifyWaiterSnapshot(handle) else [2]?kernel.ThreadWakeTarget{ null, null };
        // The MMIO last-close destructor acquires all VM locks itself when
        // it owns a WC identity lease. Holding any VM lock here would nest
        // that acquisition and deadlock process teardown. Other descriptors
        // retain the ordinary per-process transaction around their close.
        const mmio = entry.object.kind == .mmio_region;
        if (!mmio and !user_vm.lockVmTransaction(proc)) return;
        const closed = state.closeFdWithFreeList(proc, fd, h.free_list);
        if (!mmio) user_vm.unlockVmTransaction(proc);
        closed catch continue;
        if (endpoint) |pipe| {
            if (state.pipeReadyEventsForSide(pipe.pipe, !pipe.write)) |ready| {
                if (ready != 0) _ = wakePipeWaiters(h, state, pipe.pipe, !pipe.write, ready);
            }
        }
        if (channel) |handle| {
            _ = wakeIpcChannelCloseWaiters(h, state, handle);
            wakeIpcChannelNotifyWaitersAfterClose(state, handle, notify_snapshot);
        }
    }
}

pub fn dispatch(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    // Validate the wire width before narrowing, so high bits cannot alias a
    // low descriptor or turn malformed input into a kernel overflow trap.
    switch (frame.rax) {
        sc.syscall_fd_close, sc.syscall_fd_get_info, sc.syscall_fd_set_flags, sc.syscall_fd_read, sc.syscall_fd_write, sc.syscall_fd_readv, sc.syscall_fd_writev, sc.syscall_fd_fcntl, sc.syscall_fd_ioctl, sc.syscall_fd_stat, sc.syscall_timerfd_settime, sc.syscall_timerfd_gettime, sc.syscall_vmo_revoke => if (frame.rdi >= kernel.fd_number_capacity) return sc.syscall_err_invalid,
        else => {},
    }
    return switch (frame.rax) {
        sc.syscall_fd_table => blk: {
            if (frame.rdi > kernel.fd_number_capacity or frame.rsi == 0 or
                frame.rsi > @import("std").math.maxInt(u64) - fd_abi.fd_table_info_size)
                break :blk sc.syscall_err_invalid;
            // Check the output before allocating. The returned free count is
            // an observation, never an admission reservation for another call.
            var bytes: [24]u8 = [_]u8{0} ** 24;
            if (!h.copy_bytes_to_user_va(proc, frame.rsi, &bytes)) break :blk sc.syscall_err_invalid;
            state.ensureFdTableCapacity(proc, @intCast(frame.rdi), h.free_list) catch |err|
                break :blk statusFromKernelError(err);
            const table = state.getFdTableConst(proc) orelse break :blk sc.syscall_err_invalid;
            const free = state.fdFreeCountFrom(proc, first_dynamic_fd) catch break :blk sc.syscall_err_invalid;
            @import("std").mem.writeInt(u64, bytes[0..8], table.capacity, .little);
            @import("std").mem.writeInt(u64, bytes[8..16], kernel.fd_number_capacity, .little);
            @import("std").mem.writeInt(u64, bytes[16..24], free, .little);
            if (!h.copy_bytes_to_user_va(proc, frame.rsi, &bytes)) break :blk sc.syscall_err_invalid;
            break :blk sc.syscall_ok;
        },
        sc.syscall_fd_close => blk: {
            const fd: kernel.Fd = @intCast(frame.rdi);
            const pipe_endpoint = state.pipeEndpointForFd(proc, fd);
            const channel_handle = ipcChannelHandleForFd(state, proc, fd);
            const notify_snapshot = if (channel_handle) |handle| state.ipcChannelNotifyWaiterSnapshot(handle) else [2]?kernel.ThreadWakeTarget{ null, null };
            state.closeFdWithFreeList(proc, fd, h.free_list) catch break :blk sc.syscall_err_invalid;
            if (pipe_endpoint) |endpoint| {
                const wake_side_write = !endpoint.write;
                if (state.pipeReadyEventsForSide(endpoint.pipe, wake_side_write)) |ready_events| {
                    if (ready_events != 0 and !wakePipeWaiters(h, state, endpoint.pipe, wake_side_write, ready_events)) break :blk sc.syscall_err_invalid;
                }
            }
            if (channel_handle) |handle| {
                wakeIpcChannelNotifyWaitersAfterClose(state, handle, notify_snapshot);
                if (!wakeIpcChannelCloseWaiters(h, state, handle)) break :blk sc.syscall_err_invalid;
            }
            break :blk sc.syscall_ok;
        },
        sc.syscall_fd_dup => blk: {
            if (frame.rsi >= kernel.fd_number_capacity or frame.rdi > fd_abi.thread_self_fd) break :blk sc.syscall_err_invalid;
            if (frame.rdi == fd_abi.thread_self_fd) {
                if ((frame.rdx & ~fd_abi.thread_self_rights_mask) != 0 or
                    (frame.r10 & ~@as(u64, fd_abi.known_flags_mask)) != 0 or
                    frame.rsi >= kernel.fd_number_capacity) break :blk sc.syscall_err_invalid;
                const current = scheduler.currentThread();
                const generation = scheduler.currentThreadGeneration() orelse
                    break :blk sc.syscall_err_not_ready;
                break :blk state.createThreadFd(proc, .{
                    .owner_principal_raw = @intFromEnum(proc),
                    .thread_index = @intCast(current),
                    .thread_generation = generation,
                    .state = .active,
                    .exit_code = 0,
                }, kernel.fdRightsFromBits(frame.rdx), kernel.fdFlagsFromBits(@truncate(frame.r10)), @intCast(frame.rsi)) catch |err| statusFromKernelError(err);
            }
            break :blk state.dupFd(
                proc,
                @intCast(frame.rdi),
                @intCast(frame.rsi),
                kernel.fdRightsFromBits(frame.rdx),
                kernel.fdFlagsFromBits(@truncate(frame.r10)),
            ) catch sc.syscall_err_invalid;
        },
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
        sc.syscall_fd_poll => fdPoll(h, state, proc, frame),
        sc.syscall_fd_wait_many => fdWaitMany(h, state, proc, frame),
        sc.syscall_fd_ioctl => fdIoctl(h, state, proc, @intCast(frame.rdi), frame.rsi, frame.rdx, frame),
        sc.syscall_fd_stat => writeFdStat(h, state, proc, @intCast(frame.rdi), frame.rsi),
        sc.syscall_eventfd_create => eventfdCreate(state, proc, frame),
        sc.syscall_pipe_create => pipeCreate(h, state, proc, frame),
        sc.syscall_timerfd_create => timerfdCreate(state, proc, frame),
        sc.syscall_timerfd_settime => timerfdSettime(h, state, proc, frame),
        sc.syscall_timerfd_gettime => timerfdGettime(h, state, proc, @intCast(frame.rdi), frame.rsi),
        sc.syscall_vmo_create => blk: {
            if ((frame.rdx & ~fd_abi.vmo_create_known_flags_mask) != 0) break :blk sc.syscall_err_invalid;
            if ((frame.rdx & (fd_abi.vmo_create_zero_on_demand | fd_abi.vmo_create_pager)) ==
                (fd_abi.vmo_create_zero_on_demand | fd_abi.vmo_create_pager)) break :blk sc.syscall_err_invalid;
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            const create_flags = kernel.fdFlagsFromBits(@intCast(frame.rdx & fd_abi.known_flags_mask));
            if ((frame.rdx & fd_abi.vmo_create_pager) != 0) {
                var rights = kernel.fdRightsFromBits(frame.rsi);
                if (frame.rsi == 0) rights = .{
                    .inspect = true, .dup = true, .transfer = true, .close = true,
                    .map_read = true, .map_write = true, .pager_attach = true,
                };
                // Pager authority belongs to the control port, and a client
                // must never gain it by receiving the VMO descriptor.
                if (rights.pager_fault or rights.read or rights.write or rights.resize or
                    rights.cache_policy or rights.share or !rights.pager_attach)
                    break :blk sc.syscall_err_invalid;
                break :blk state.createPagerVmoFd(proc, frame.rdi, rights, create_flags, first_dynamic_fd) catch |err| statusFromKernelError(err);
            }
            if ((frame.rdx & fd_abi.vmo_create_zero_on_demand) != 0) {
                break :blk state.createZeroOnDemandVmoFd(
                    proc,
                    frame.rdi,
                    defaultVmoRights(kernel.fdRightsFromBits(frame.rsi)),
                    create_flags,
                    first_dynamic_fd,
                ) catch |err| statusFromKernelError(err);
            }
            break :blk state.createAnonymousVmoFdWithPages(
                proc,
                frame.rdi,
                defaultVmoRights(kernel.fdRightsFromBits(frame.rsi)),
                create_flags,
                first_dynamic_fd,
                h.free_list,
            ) catch |err| statusFromKernelError(err);
        },
        sc.syscall_pager_port_create => blk: {
            if (frame.rdi & ~@as(u64, fd_abi.known_flags_mask) != 0) break :blk sc.syscall_err_invalid;
            const flags = kernel.fdFlagsFromBits(@intCast(frame.rdi));
            break :blk state.createPagerPortFd(proc, flags, first_dynamic_fd) catch |err| statusFromKernelError(err);
        },
        sc.syscall_pager_attach => blk: {
            if (frame.rdi >= kernel.fd_number_capacity or frame.rsi >= kernel.fd_number_capacity or frame.r10 != 0)
                break :blk sc.syscall_err_invalid;
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            const generation = state.attachPagerVmo(proc, @intCast(frame.rdi), @intCast(frame.rsi), frame.rdx) catch |err|
                break :blk statusFromKernelError(err);
            frame.rdx = generation;
            break :blk sc.syscall_ok;
        },
        sc.syscall_pager_wait => pagerWait(h, state, proc, frame),
        sc.syscall_pager_supply => pagerSupply(h, state, proc, frame),
        sc.syscall_pager_fail => blk: {
            if (frame.rdi >= kernel.fd_number_capacity or
                frame.r8 != pager_abi.fail_io and frame.r8 != pager_abi.fail_eof_or_truncate and frame.r8 != pager_abi.fail_pager_dead)
                break :blk sc.syscall_err_invalid;
            const port_ref = state.pagerPortRefForFd(proc, @intCast(frame.rdi), .{ .pager_fault = true }) orelse break :blk sc.syscall_err_invalid;
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            state.completePagerRequest(port_ref, frame.rsi, frame.rdx, frame.r10, true, @intCast(frame.r8)) catch
                break :blk sc.syscall_err_invalid;
            break :blk sc.syscall_ok;
        },
        sc.syscall_vmo_grow => blk: {
            if (frame.rdi > @import("std").math.maxInt(kernel.Fd)) break :blk sc.syscall_err_invalid;
            user_vm.lockSharedVmObjects();
            defer user_vm.unlockSharedVmObjects();
            state.growVmoFdWithPages(proc, @intCast(frame.rdi), frame.rsi, h.free_list) catch |err|
                break :blk statusFromKernelError(err);
            break :blk sc.syscall_ok;
        },
        sc.syscall_vmo_cache_policy => blk: {
            const fd = std_rc.math.cast(kernel.Fd, frame.rdi) orelse break :blk sc.syscall_err_invalid;
            if ((frame.rsi | frame.rdx) & 4095 != 0 or frame.rdx == 0 or
                frame.rsi / 4096 > std_rc.math.maxInt(usize) or
                frame.rdx / 4096 > std_rc.math.maxInt(usize)) break :blk sc.syscall_err_invalid;
            const mode: @import("../memory/ram_cache_transition.zig").Mode = switch (frame.r10) {
                fd_abi.vmo_cache_wb => .wb,
                fd_abi.vmo_cache_uc_minus => .uc_minus,
                fd_abi.vmo_cache_uc => .uc,
                fd_abi.vmo_cache_wc => .wc,
                else => break :blk sc.syscall_err_invalid,
            };
            const view = state.fdPayloadWithRightsConst(proc, fd, .{
                .cache_policy = true,
                .map_write = true,
            }) orelse break :blk sc.syscall_err_invalid;
            const ref = switch (view.payload.*) {
                .vmo => |vmo_ref| vmo_ref,
                else => break :blk sc.syscall_err_invalid,
            };
            @import("ram_cache.zig").change(
                state,
                h.free_list,
                ref,
                @intCast(frame.rsi / 4096),
                @intCast(frame.rdx / 4096),
                mode,
            ) catch |err| {
                reportRamCacheRefusal(err, frame.rsi, frame.rdx, mode);
                break :blk switch (err) {
                    error.UntranslatedDmaUnsupported => sc.syscall_err_not_ready,
                    error.OutOfMemory, error.OutOfFreePages, error.NoSpace => sc.syscall_err_alloc,
                    else => sc.syscall_err_map,
                };
            };
            break :blk sc.syscall_ok;
        },
        sc.syscall_vmo_revoke => revokeVmoFd(state, proc, @intCast(frame.rdi), h.free_list),
        sc.syscall_vmo_collect_dirty => collectPageViewDirtyFd(h, state, proc, frame.rdi, frame.rsi, frame.rdx, frame.r10),
        sc.syscall_vmo_refresh_page_view => refreshPageViewVmoFd(h, state, proc, frame.rdi, frame.rsi, frame.rdx, frame.r10),
        sc.syscall_vmo_create_page_view => if (frame.rdi >= kernel.fd_number_capacity) sc.syscall_err_invalid else createPageViewVmoFd(
            h,
            state,
            proc,
            @intCast(frame.rdi),
            frame.rsi,
            frame.rdx,
            frame.r10,
            frame.r8,
        ),
        sc.syscall_mmap => mapVmoFd(state, proc, h.free_list, @intCast(frame.rdi), frame.rsi, frame.rdx, frame.r10, frame.r8, frame.r9),
        sc.syscall_munmap => blk: {
            const profile_start = smp_perf.timestamp();
            smp_perf.munmapAdd(.calls, 1);
            defer smp_perf.munmapElapsed(.total_cycles, profile_start);
            var profile_success = false;
            defer smp_perf.munmapAdd(if (profile_success) .successes else .errors, 1);
            if (frame.rsi == 0 or (frame.rdi & 0xFFF) != 0) break :blk sc.syscall_err_invalid;
            const size = pageAlignUp(frame.rsi) orelse break :blk sc.syscall_err_invalid;
            // Size histogram covers requests that pass length/alignment
            // validation, including any later failure.
            const pages = size / 4096;
            smp_perf.munmapAdd(.requested_pages, pages);
            smp_perf.munmapAdd(if (pages == 1) .size_1_page else if (pages <= 4)
                .size_2_to_4_pages
            else if (pages <= 64) .size_5_to_64_pages else .size_over_64_pages, 1);
            var profile_stage = smp_perf.timestamp();
            const transaction_locked = user_vm.lockVmTransaction(proc);
            smp_perf.munmapElapsed(.transaction_lock_cycles, profile_stage);
            if (!transaction_locked) break :blk sc.syscall_err_invalid;
            defer user_vm.unlockVmTransaction(proc);
            profile_stage = smp_perf.timestamp();
            const overlaps_pinned = state.rangeOverlapsPinnedUserObject(proc, frame.rdi, size);
            smp_perf.munmapElapsed(.pinned_check_cycles, profile_stage);
            if (overlaps_pinned) break :blk sc.syscall_err_invalid;
            profile_stage = smp_perf.timestamp();
            const native_vma_source = nativeVmaCoversRange(state, proc, frame.rdi, size);
            const reservation_slots_result = if (native_vma_source) user_vm.unmapPresentVmaSourceSplitSlotsRequired(
                proc,
                frame.rdi,
                @intCast(size),
            ) else user_vm.unmapPresentUserLinearRegionSplitSlotsRequired(
                proc,
                frame.rdi,
                @intCast(size),
            );
            smp_perf.munmapElapsed(.reservation_validate_cycles, profile_stage);
            const reservation_slots = reservation_slots_result orelse break :blk sc.syscall_err_map;
            profile_stage = smp_perf.timestamp();
            const slots_ready = user_vm.prepareUserReservationSlots(proc, reservation_slots);
            smp_perf.munmapElapsed(.reservation_count_cycles, profile_stage);
            if (!slots_ready) break :blk sc.syscall_err_alloc;
            profile_stage = smp_perf.timestamp();
            const prepared_result = state.prepareMunmapRangeWithFreeList(
                proc,
                frame.rdi,
                size,
                h.free_list,
            );
            smp_perf.munmapElapsed(.prepare_cycles, profile_stage);
            var prepared = prepared_result catch |err| break :blk switch (err) {
                kernel.KernelError.TableFull, kernel.KernelError.OutOfFreePages => sc.syscall_err_alloc,
                else => sc.syscall_err_map,
            };
            defer state.discardMunmapPrepared(&prepared, h.free_list);
            profile_stage = smp_perf.timestamp();
            const unmapped = if (native_vma_source)
                user_vm.unmapPresentVmaSource(proc, frame.rdi, @intCast(size))
            else
                user_vm.unmapPresentUserLinearRegion(proc, frame.rdi, @intCast(size));
            if (!unmapped) unreachable;
            smp_perf.munmapElapsed(.unmap_cycles, profile_stage);
            profile_stage = smp_perf.timestamp();
            state.commitMunmapPrepared(&prepared, h.free_list);
            smp_perf.munmapElapsed(.commit_cycles, profile_stage);
            profile_success = true;
            break :blk sc.syscall_ok;
        },
        sc.syscall_mprotect => mprotectVmaRange(state, proc, frame.rdi, frame.rsi, frame.rdx),
        sc.syscall_mremap => mremapVmaRange(state, proc, h.free_list, frame.rdi, frame.rsi, frame.rdx, frame.r10, frame.r8),
        sc.syscall_madvise => madviseVmaRange(state, proc, frame.rdi, frame.rsi, frame.rdx),
        else => null,
    };
}

// Host tests enter the actual frontend without compiling unrelated dispatch arms.
pub const WaitTestSupport = if (@import("builtin").is_test) struct {
    pub const wait = fdWaitMany;
    pub const channelHandle = ipcChannelHandleForFd;
    pub const closeWake = wakeIpcChannelCloseWaiters;
    pub const closeNotify = wakeIpcChannelNotifyWaitersAfterClose;
} else struct {};

test "poll item snapshot ready zero single multiple maximum and fault" {
    const std = @import("std");
    const arena = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(arena);
    try std.testing.expect(kernel.initRuntimeStorage(arena));
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    try state.initFromDetectedRegionsInPlace(1);
    defer state.deinitPools();
    const owner = kernel.processPrincipalFromIndex(0).?;
    const pair = try state.createPipePairFds(owner, .{}, 16);
    const Copy = struct {
        var calls: usize = 0;
        var fail: bool = false;
        fn write(_: kernel.PrincipalId, va: u64, value: u64) bool {
            std.debug.assert((va - 0x1000) % 32 == 24);
            std.debug.assert(value == fd_abi.event_writable);
            calls += 1;
            return !fail;
        }
    };
    const h = .{ .write_user_u64 = Copy.write };
    var frame = std.mem.zeroes(TrapFrame);
    var items: [max_pollfds]PollItem = undefined;
    for (&items, 0..) |*item, i| item.* = .{ .fd = pair.read, .events = fd_abi.event_readable, .item_va = 0x1000 + i * 32 };
    Copy.calls = 0;
    Copy.fail = false;
    try std.testing.expectEqual(@as(?u64, 0), pollCached(h, state, owner, &items, &frame));
    items[199].fd = pair.write;
    items[199].events = fd_abi.event_writable;
    items[199].min_write_bytes = 58;
    try std.testing.expectEqual(@as(?u64, 1), pollCached(h, state, owner, &items, &frame));
    try std.testing.expectEqual(@as(u64, 199), frame.r10);
    try std.testing.expectEqual(fd_abi.event_writable, frame.r8);
    try std.testing.expectEqual(@as(usize, 0), Copy.calls);
    items[0].fd = pair.write;
    items[0].events = fd_abi.event_writable;
    try std.testing.expectEqual(@as(?u64, 2), pollCached(h, state, owner, &items, &frame));
    try std.testing.expectEqual(@as(usize, 2), Copy.calls);
    Copy.fail = true;
    try std.testing.expect(pollCached(h, state, owner, &items, &frame) == null);
    Copy.fail = false;
    Copy.calls = 0;
    for (&items) |*item| {
        item.fd = pair.write;
        item.events = fd_abi.event_writable;
    }
    try std.testing.expectEqual(@as(?u64, max_pollfds), pollCached(h, state, owner, &items, &frame));
    try std.testing.expectEqual(max_pollfds, Copy.calls);
    // Minimum is input-only and zero means a one-byte request.
    items[0].min_write_bytes = std.math.maxInt(u64);
    try std.testing.expectEqual(@as(?u64, 0), pollCached(h, state, owner, items[0..1], &frame));
}

test "poll item snapshot persistent subscriptions reuse rollback close and bounded FIFO" {
    const std = @import("std");
    const arena = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(arena);
    try std.testing.expect(kernel.initRuntimeStorage(arena));
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    try state.initFromDetectedRegionsInPlace(1);
    defer state.deinitPools();
    const owner = kernel.processPrincipalFromIndex(0).?;
    try std.testing.expect(!persistentOutputValid(std.math.maxInt(u64) - 8, 2));
    try std.testing.expect(!persistentOutputValid(0x0000_7fff_ffff_fff0, 2));
    try std.testing.expect(!persistentOutputValid(0, 1));
    try std.testing.expect(persistentOutputValid(0x1000, max_pollfds));
    const pair = try state.createPipePairFds(owner, .{}, 16);
    const Mock = struct {
        var bytes: [max_pollfds * fd_abi.pollfd_size]u8 = @splat(0);
        var reads: usize = 0;
        var writes: usize = 0;
        fn copy(_: kernel.PrincipalId, address: u64, out: []u8) bool {
            std.debug.assert(address == 0x1000 and out.len <= bytes.len);
            reads += 1;
            @memcpy(out, bytes[0..out.len]);
            return true;
        }
        fn write(_: kernel.PrincipalId, address: u64, value: u64) bool {
            const offset = address - 0x1000;
            std.debug.assert(offset % fd_abi.pollfd_size == fd_abi.pollfd_revents_offset);
            writes += 1;
            std.mem.writeInt(u64, bytes[offset..][0..8], value, .little);
            return true;
        }
        fn item(index: usize, fd: kernel.Fd) void {
            const wire = bytes[index * fd_abi.pollfd_size..][0..fd_abi.pollfd_size];
            @memset(wire, 0);
            std.mem.writeInt(u64, wire[0..8], fd, .little);
            std.mem.writeInt(u64, wire[8..16], fd_abi.event_writable, .little);
        }
    };
    const h = .{ .copy_user_bytes_from_va = Mock.copy, .write_user_u64 = Mock.write };
    Mock.reads = 0;
    Mock.writes = 0;
    Mock.item(0, pair.write);
    const set = try replacePersistentSet(h, state, owner, 0x1000, 1, 4, 9);
    try std.testing.expectEqual(@as(usize, 0), state.fd_wait_groups.live);
    for (0..32) |_| {
        var frame = std.mem.zeroes(TrapFrame);
        try std.testing.expectEqual(@as(u64, 1), persistentReady(h, state, set, 0x1000, &frame));
        try std.testing.expectEqual(fd_abi.event_writable, frame.r8);
        try std.testing.expectEqual(@as(usize, 1), set.queued);
    }
    try std.testing.expectEqual(@as(usize, 1), Mock.reads);
    // A late invalid FD must leave the prior native set and its subscriptions.
    Mock.item(1, @intCast(kernel.fd_table_entries - 1));
    try std.testing.expectError(kernel.KernelError.InvalidState,
        replacePersistentSet(h, state, owner, 0x1000, 2, 4, 9));
    try std.testing.expect(kernel.persistent_wait.current(state, owner, 4, 9).? == set);
    try std.testing.expectEqual(@as(usize, 1), state.persistent_wait_sets.live);
    try std.testing.expectEqual(@as(usize, 0), state.fd_wait_groups.live);
    // Same-object descriptor reuse is a new incarnation, not the old watch.
    const entry = state.fdEntryConst(owner, pair.write).?.*;
    const alias = try state.dupFd(owner, pair.write, 16, entry.rights, entry.flags);
    try state.closeFd(owner, pair.write);
    const replacement = try state.dupFd(owner, alias, pair.write, entry.rights, entry.flags);
    try std.testing.expectEqual(pair.write, replacement);
    var frame = std.mem.zeroes(TrapFrame);
    try std.testing.expectEqual(fd_abi.wait_set_invalidated, persistentReady(h, state, set, 0x1000, &frame));
    for (0..max_pollfds) |i| Mock.item(i, replacement);
    const many = try replacePersistentSet(h, state, owner, 0x1000, max_pollfds, 4, 9);
    try std.testing.expectEqual(max_pollfds, many.queued);
    try std.testing.expectEqual(@as(u64, max_pollfds), persistentReady(h, state, many, 0x1000, &frame));
    try std.testing.expectEqual(max_pollfds, Mock.writes);
    try std.testing.expectEqual(max_pollfds, many.queued);
    kernel.persistent_wait.clearThread(state, owner, 4, 9);
    try std.testing.expectEqual(@as(usize, 0), state.persistent_wait_sets.live);
    try std.testing.expectEqual(@as(usize, 0), state.persistent_wait_threads.live);
}

test "poll item snapshot persistent HUP observers preserve data admission and close wake" {
    const std = @import("std");
    const arena = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(arena);
    try std.testing.expect(kernel.initRuntimeStorage(arena));
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    try state.initFromDetectedRegionsInPlace(1);
    defer state.deinitPools();
    const owner = kernel.processPrincipalFromIndex(0).?;
    const pair = try state.createIpcChannelPairFds(owner,
        .{ .send = true, .recv = true, .wait = true, .poll = true, .close = true }, .{}, 16);
    const Mock = struct {
        var fd: kernel.Fd = 0;
        var events: u64 = 0;
        fn copy(_: kernel.PrincipalId, _: u64, out: []u8) bool {
            @memset(out, 0);
            std.mem.writeInt(u64, out[0..8], fd, .little);
            std.mem.writeInt(u64, out[8..16], events, .little);
            return true;
        }
        fn write(_: kernel.PrincipalId, _: u64, _: u64) bool { return true; }
    };
    Mock.fd = pair.a;
    Mock.events = 0;
    const h = .{ .copy_user_bytes_from_va = Mock.copy, .write_user_u64 = Mock.write };
    const set = try replacePersistentSet(h, state, owner, 0x1000, 1, 4, 9);
    const member = set.items.get(0).?;
    var transient: kernel.IpcWaiter = .{ .active = true, .events = fd_abi.event_readable };
    var before: kernel.IpcWaiter = .{ .active = true, .binding = .{ .persistent = member }, .next = &transient };
    var after: kernel.IpcWaiter = .{ .active = true, .binding = .{ .persistent = member } };
    transient.next = &after;
    const list: kernel.IpcWaitList = .{ .poll_head = &before };
    try std.testing.expect(list.dataPollAfter(null).? == &transient);
    try std.testing.expect(list.dataPollAfter(&transient) == null);
    after.events = fd_abi.event_writable;
    try std.testing.expect(list.dataPollAfter(&transient).? == &after);
    member.publish();
    try std.testing.expect(list.dataPollAfter(&transient) == null);
    set.invocation = .{ .token = 1 };
    try std.testing.expect(list.dataPollAfter(&transient).? == &after);
    set.invocation = null;
    _ = set.pop();
    try std.testing.expect(list.dataPollAfter(&transient).? == &after);
    before.recv_memory.active = true;
    try std.testing.expect(list.dataPollAfter(null).? == &before);

    const closed_handle = state.kernelObjectSlotConst(state.fdEntryConst(owner, pair.b).?.object).?.payload.channel;
    var free: kernel.FreePageList = .{};
    try state.ipcSend(owner, pair.b, .{ .words = .{ 1, 2, 3, 4 } }, &free);
    var targets: [1]kernel.ThreadWakeTarget = undefined;
    try std.testing.expectEqual(@as(usize, 0), try state.wakeIpcPollWaitersForSendFd(owner, pair.b, &targets));
    var frame = std.mem.zeroes(TrapFrame);
    try std.testing.expectEqual(@as(u64, 0), persistentReady(h, state, set, 0x1000, &frame));
    try state.closeFd(owner, pair.b);
    try std.testing.expectEqual(@as(usize, 0), state.takeIpcChannelPeerCloseWaiters(closed_handle, &targets));
    try std.testing.expectEqual(@as(u64, 1), persistentReady(h, state, set, 0x1000, &frame));
    try std.testing.expectEqual(fd_abi.event_hangup, frame.r8);

    const data_pair = try state.createIpcChannelPairFds(owner,
        .{ .send = true, .recv = true, .wait = true, .poll = true, .close = true }, .{}, 16);
    Mock.fd = data_pair.a;
    Mock.events = fd_abi.event_readable;
    const data_set = try replacePersistentSet(h, state, owner, 0x1000, 1, 4, 9);
    const object = state.kernelObjectSlotConst(state.fdEntryConst(owner, data_pair.a).?.object).?;
    const actual_list = state.ipcWaitListForRecvPayload(&object.payload).?;
    try state.ipcSend(owner, data_pair.b, .{}, &free);
    _ = try state.wakeIpcPollWaitersForSendFd(owner, data_pair.b, &targets);
    try std.testing.expectEqual(@as(u64, 1), persistentReady(h, state, data_set, 0x1000, &frame));
    _ = try state.ipcRecv(owner, data_pair.a, 0, 16, &free);
    {
        var guard = actual_list.acquire();
        defer guard.end();
        try std.testing.expect(actual_list.dataPollAfter(null) == null);
    }
    // An idle stale hint survives a fast producer that need not republish it.
    // REUSE must observe the new message, then unqueue after it is consumed.
    try state.ipcSend(owner, data_pair.b, .{}, &free);
    try std.testing.expectEqual(@as(u64, 1), persistentReady(h, state, data_set, 0x1000, &frame));
    _ = try state.ipcRecv(owner, data_pair.a, 0, 16, &free);
    try std.testing.expectEqual(@as(u64, 0), persistentReady(h, state, data_set, 0x1000, &frame));
    {
        var guard = actual_list.acquire();
        defer guard.end();
        try std.testing.expect(actual_list.dataPollAfter(null) != null);
    }
    try state.ipcSend(owner, data_pair.b, .{}, &free);
    _ = try state.wakeIpcPollWaitersForSendFd(owner, data_pair.b, &targets);
    try std.testing.expectEqual(@as(u64, 1), persistentReady(h, state, data_set, 0x1000, &frame));
}

test "poll item snapshot persistent parked wake timeout signal and thread reuse" {
    const std = @import("std");
    const kernel_runtime = @import("../kernel_runtime.zig");
    scheduler.initializeStaticStorage();
    defer scheduler.initializeStaticStorage();
    const tid = 7;
    const generation = 11;
    const token = 41;
    const ctx = scheduler.threadContextMutable(tid).?;
    const saved = ctx.*;
    defer ctx.* = saved;
    const arena = try std.testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), kernel.runtimeStorageBytes());
    defer std.testing.allocator.free(arena);
    try std.testing.expect(kernel.initRuntimeStorage(arena));
    const state = try std.testing.allocator.create(kernel.KernelState);
    defer std.testing.allocator.destroy(state);
    try state.initFromDetectedRegionsInPlace(1);
    defer state.deinitPools();
    const saved_state = kernel_runtime.kernel_state_global;
    kernel_runtime.kernel_state_global = state;
    defer kernel_runtime.kernel_state_global = saved_state;
    const owner = kernel.processPrincipalFromIndex(0).?;
    const pair = try state.createPipePairFds(owner, .{}, 16);
    const pipe = state.kernelObjectSlotConst(state.fdEntryConst(owner, pair.read).?.object).?.payload.pipe.pipe;
    const Mock = struct {
        var fd: kernel.Fd = 0;
        var copies: usize = 0;
        fn copy(_: kernel.PrincipalId, _: u64, bytes: []u8) bool {
            @memset(bytes, 0);
            var offset: usize = 0;
            while (offset < bytes.len) : (offset += fd_abi.pollfd_size) {
                std.mem.writeInt(u64, bytes[offset..][0..8], fd, .little);
                std.mem.writeInt(u64, bytes[offset + 8..][0..8], fd_abi.event_readable, .little);
            }
            return true;
        }
        fn write(_: kernel.PrincipalId, address: u64, events: u64) bool {
            std.debug.assert((address == 0x1018 or address == 0x1038) and events == fd_abi.event_readable);
            copies += 1;
            return true;
        }
    };
    Mock.fd = pair.read;
    const h = .{ .copy_user_bytes_from_va = Mock.copy, .write_user_u64 = Mock.write };
    for (0..5) |race| {
        ctx.* = .{ .id = tid, .allocated = true, .generation = generation, .owner_process = owner,
            .stopped = true, .wait_mailbox = true, .active_wait_token = token };
        ctx.frame.rdi = 0x1000;
        Mock.copies = 0;
        const count: u64 = if (race == 4) 2 else 1;
        const set = try replacePersistentSet(h, state, owner, 0x1000, count, tid, generation);
        try std.testing.expectEqual(@as(usize, 0), set.queued);
        set.invocation = .{ .token = token };
        if (race == 1) {
            _ = scheduler.wakeIfWaitingTokenGenerationWithRax(tid, generation, owner, token, 77);
        } else if (race == 2) {
            ctx.stopped = false;
            const delivered = scheduler.deliverThreadSignal(owner, tid, generation, 1, 77).?;
            try std.testing.expect(delivered.was_blocked);
        } else if (race == 3) {
            ctx.generation += 1;
        }
        try std.testing.expectEqual(@as(usize, 1), try state.pipeWriteBytes(owner, pair.write, "x", true));
        var targets: [1]kernel.ThreadWakeTarget = undefined;
        // Source publication only queues the observer; no old thread/token/VA
        // is embedded in it, and it survives a cancelled wait invocation.
        try std.testing.expectEqual(@as(usize, 0), state.takePipeWaiters(pipe, false, fd_abi.event_readable, &targets));
        completePersistentWaiters(h, state);
        try std.testing.expectEqual(@as(usize, if (race == 4) 2 else 0), Mock.copies);
        if (race == 0 or race == 4) try std.testing.expectEqual(count, ctx.frame.rax);
        if (race == 0) {
            try std.testing.expectEqual(@as(u64, 0), ctx.frame.r10);
            try std.testing.expectEqual(fd_abi.event_readable, ctx.frame.r8);
        }
        if (race == 1 or race == 2) try std.testing.expectEqual(@as(u64, 77), ctx.frame.rax);
        try std.testing.expect(set.invocation == null);
        var frame = std.mem.zeroes(TrapFrame);
        try std.testing.expectEqual(count, persistentReady(h, state, set, 0x1000, &frame));
        var byte: [1]u8 = undefined;
        try std.testing.expectEqual(@as(usize, 1), try state.pipeReadBytes(owner, pair.read, &byte));
        try std.testing.expectEqual(@as(u64, 0), persistentReady(h, state, set, 0x1000, &frame));
        kernel.persistent_wait.clearThread(state, owner, tid, generation);
        try std.testing.expectEqual(@as(usize, 0), state.persistent_wait_sets.live);
        try std.testing.expectEqual(@as(usize, 0), state.persistent_wait_threads.live);
        try std.testing.expectEqual(@as(u32, 0), state.kernelObjectSlot(state.fdEntryConst(owner, pair.read).?.object).?.persistent_observers);
    }
}
