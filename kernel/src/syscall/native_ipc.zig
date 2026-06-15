const abi_root = @import("kernel_abi_root");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const sc = @import("numbers.zig");

const fd_abi = abi_root.fd_abi;
const ipc_abi = abi_root.ipc_abi;
const TrapFrame = interrupts.TrapFrame;
const first_dynamic_fd: kernel.Fd = fd_abi.first_dynamic_fd;

fn mapError(err: kernel.KernelError) u64 {
    return switch (err) {
        kernel.KernelError.MailboxEmpty => sc.syscall_err_empty,
        kernel.KernelError.TableFull => sc.syscall_err_alloc,
        else => sc.syscall_err_invalid,
    };
}

fn fdFlagsFromTransfer(flags_bits: u64, transfer_flags: u64) ?kernel.FdFlags {
    if ((transfer_flags & ~ipc_abi.transfer_known_mask) != 0) return null;
    var bits: u32 = @truncate(flags_bits);
    if ((transfer_flags & ipc_abi.transfer_cloexec) != 0) bits |= fd_abi.flag_cloexec;
    if ((transfer_flags & ipc_abi.transfer_nonblock) != 0) bits |= fd_abi.flag_nonblock;
    if ((transfer_flags & ipc_abi.transfer_inherit) != 0) bits |= fd_abi.flag_inherit;
    if ((transfer_flags & ipc_abi.transfer_private) != 0) bits |= fd_abi.flag_private;
    return kernel.fdFlagsFromBits(bits);
}

fn readIpcMessage(
    h: anytype,
    proc: kernel.PrincipalId,
    msg_va: u64,
    fd_storage: *[kernel.max_ipc_message_fds]kernel.IpcSendFd,
) ?kernel.IpcSendMessage {
    if (msg_va == 0) return null;
    const fd_count_u64 = h.read_user_u64(proc, msg_va + ipc_abi.msg_fd_count_offset) orelse return null;
    if (fd_count_u64 > kernel.max_ipc_message_fds) return null;
    const fd_count: usize = @intCast(fd_count_u64);
    const fd_array_va = h.read_user_u64(proc, msg_va + ipc_abi.msg_fd_array_offset) orelse return null;
    if (fd_count != 0 and fd_array_va == 0) return null;

    var words: [4]u64 = .{ 0, 0, 0, 0 };
    words[0] = h.read_user_u64(proc, msg_va + ipc_abi.msg_word0_offset) orelse return null;
    words[1] = h.read_user_u64(proc, msg_va + ipc_abi.msg_word1_offset) orelse return null;
    words[2] = h.read_user_u64(proc, msg_va + ipc_abi.msg_word2_offset) orelse return null;
    words[3] = h.read_user_u64(proc, msg_va + ipc_abi.msg_word3_offset) orelse return null;

    var i: usize = 0;
    while (i < fd_count) : (i += 1) {
        const item_va = fd_array_va + @as(u64, @intCast(i)) * ipc_abi.fd_item_size;
        const fd = h.read_user_u64(proc, item_va + ipc_abi.fd_item_fd_offset) orelse return null;
        const rights_bits = h.read_user_u64(proc, item_va + ipc_abi.fd_item_rights_offset) orelse return null;
        const flags_bits = h.read_user_u64(proc, item_va + ipc_abi.fd_item_flags_offset) orelse return null;
        const transfer_flags = h.read_user_u64(proc, item_va + ipc_abi.fd_item_transfer_flags_offset) orelse return null;
        fd_storage[i] = .{
            .fd = @intCast(fd),
            .rights = kernel.fdRightsFromBits(rights_bits),
            .flags = fdFlagsFromTransfer(flags_bits, transfer_flags) orelse return null,
            .move = (transfer_flags & ipc_abi.transfer_move) != 0,
        };
    }
    return .{
        .words = words,
        .fds = fd_storage[0..fd_count],
    };
}

fn writeRecvMessage(
    h: anytype,
    proc: kernel.PrincipalId,
    msg_va: u64,
    result: kernel.IpcRecvResult,
) u64 {
    if (msg_va == 0) return sc.syscall_err_invalid;
    const fd_capacity_u64 = h.read_user_u64(proc, msg_va + ipc_abi.msg_fd_capacity_offset) orelse return sc.syscall_err_invalid;
    if (fd_capacity_u64 < result.fd_count) return sc.syscall_err_alloc;
    const fd_array_va = h.read_user_u64(proc, msg_va + ipc_abi.msg_fd_array_offset) orelse return sc.syscall_err_invalid;
    if (result.fd_count != 0 and fd_array_va == 0) return sc.syscall_err_invalid;

    if (!h.write_user_u64(proc, msg_va + ipc_abi.msg_word0_offset, result.words[0])) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, msg_va + ipc_abi.msg_word1_offset, result.words[1])) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, msg_va + ipc_abi.msg_word2_offset, result.words[2])) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, msg_va + ipc_abi.msg_word3_offset, result.words[3])) return sc.syscall_err_invalid;
    if (!h.write_user_u64(proc, msg_va + ipc_abi.msg_fd_count_offset, result.fd_count)) return sc.syscall_err_invalid;

    var i: usize = 0;
    while (i < result.fd_count) : (i += 1) {
        const item = result.fds[i];
        const item_va = fd_array_va + @as(u64, @intCast(i)) * ipc_abi.fd_item_size;
        if (!h.write_user_u64(proc, item_va + ipc_abi.fd_item_fd_offset, item.fd)) return sc.syscall_err_invalid;
        if (!h.write_user_u64(proc, item_va + ipc_abi.fd_item_rights_offset, kernel.fdRightsToBits(item.rights))) return sc.syscall_err_invalid;
        if (!h.write_user_u64(proc, item_va + ipc_abi.fd_item_flags_offset, kernel.fdFlagsToBits(item.flags))) return sc.syscall_err_invalid;
        if (!h.write_user_u64(proc, item_va + ipc_abi.fd_item_transfer_flags_offset, 0)) return sc.syscall_err_invalid;
    }
    return sc.syscall_ok;
}

pub fn dispatch(h: anytype, state: *kernel.KernelState, proc: kernel.PrincipalId, frame: *TrapFrame) ?u64 {
    return switch (frame.rax) {
        sc.syscall_ipc_endpoint_create => state.createIpcEndpointFd(
            proc,
            kernel.fdRightsFromBits(frame.rdi),
            kernel.fdFlagsFromBits(@truncate(frame.rsi)),
            first_dynamic_fd,
        ) catch |err| mapError(err),
        sc.syscall_ipc_channel_create => blk: {
            if (frame.rdi == 0) break :blk sc.syscall_err_invalid;
            const pair = state.createIpcChannelPairFds(
                proc,
                kernel.fdRightsFromBits(frame.rsi),
                kernel.fdFlagsFromBits(@truncate(frame.rdx)),
                first_dynamic_fd,
            ) catch |err| break :blk mapError(err);
            if (!h.write_user_u64(proc, frame.rdi, pair.a)) break :blk sc.syscall_err_invalid;
            if (!h.write_user_u64(proc, frame.rdi + 8, pair.b)) break :blk sc.syscall_err_invalid;
            break :blk sc.syscall_ok;
        },
        sc.syscall_ipc_send => blk: {
            var fd_storage: [kernel.max_ipc_message_fds]kernel.IpcSendFd = undefined;
            const msg = readIpcMessage(h, proc, frame.rsi, &fd_storage) orelse break :blk sc.syscall_err_invalid;
            state.ipcSend(proc, @intCast(frame.rdi), msg) catch |err| break :blk mapError(err);
            break :blk sc.syscall_ok;
        },
        sc.syscall_ipc_recv => blk: {
            if (frame.rsi == 0) break :blk sc.syscall_err_invalid;
            const fd_capacity = h.read_user_u64(proc, frame.rsi + ipc_abi.msg_fd_capacity_offset) orelse break :blk sc.syscall_err_invalid;
            if (fd_capacity > kernel.max_ipc_message_fds) break :blk sc.syscall_err_invalid;
            const result = state.ipcRecv(proc, @intCast(frame.rdi), @intCast(fd_capacity), first_dynamic_fd) catch |err| break :blk mapError(err);
            const status = writeRecvMessage(h, proc, frame.rsi, result);
            if (status != sc.syscall_ok) {
                var i: usize = 0;
                while (i < result.fd_count) : (i += 1) {
                    state.closeFd(proc, result.fds[i].fd) catch {};
                }
            }
            break :blk status;
        },
        sc.syscall_ipc_call => blk: {
            var fd_storage: [kernel.max_ipc_message_fds]kernel.IpcSendFd = undefined;
            const msg = readIpcMessage(h, proc, frame.rsi, &fd_storage) orelse break :blk sc.syscall_err_invalid;
            break :blk state.ipcCall(proc, @intCast(frame.rdi), msg, first_dynamic_fd) catch |err| mapError(err);
        },
        sc.syscall_ipc_reply => blk: {
            var fd_storage: [kernel.max_ipc_message_fds]kernel.IpcSendFd = undefined;
            const msg = readIpcMessage(h, proc, frame.rsi, &fd_storage) orelse break :blk sc.syscall_err_invalid;
            state.ipcReply(proc, @intCast(frame.rdi), msg) catch |err| break :blk mapError(err);
            break :blk sc.syscall_ok;
        },
        else => null,
    };
}
