const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const device_capabilities = @import("device_capabilities.zig");
const abi_root = @import("kernel_abi_root");
const cap_transfer_abi = abi_root.cap_transfer_abi;
const image_abi = abi_root.image_abi;
const trap_abi = abi_root.trap_abi;
const user_vm = @import("memory/user_vm.zig");
const process_abi = abi_root.process_abi;
const process_builder_abi = abi_root.process_builder_abi;
const queue_abi = abi_root.queue_abi;
const untyped_memory = @import("untyped_memory.zig");
const interrupts = @import("interrupts.zig");
const scheduler = @import("scheduler.zig");
const x86_platform = @import("arch/x86_64/platform.zig");
const process_builder = @import("runtime/process_builder.zig");

const TrapFrame = interrupts.TrapFrame;

const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_move_cap: u64 = 0x3;
const syscall_drop_present: u64 = 0x4;
const syscall_switch_thread: u64 = 0x5;
const syscall_send_cap: u64 = 0x6;
const syscall_revoke_tree: u64 = 0x7;
const syscall_grant_cap: u64 = 0x8;
const syscall_log: u64 = 0x9;
const syscall_recv_cap: u64 = 0xA;
const syscall_map_mmio: u64 = 0xB;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_queue_submit: u64 = 0xE;
const syscall_queue_notify: u64 = 0xF;
const syscall_untyped_alloc: u64 = 0x10;
const syscall_untyped_retype_pages: u64 = 0x11;
const syscall_untyped_reset: u64 = 0x12;
const syscall_untyped_alloc_map_pages: u64 = 0x13;
const syscall_grant_caps_batch: u64 = 0x14;
const syscall_map_pages_batch: u64 = 0x15;
const syscall_launch_pie_user: u64 = 0x16;
const syscall_wait_event: u64 = 0x17;
const syscall_grant_cap_on_endpoint: u64 = 0x24;
const syscall_grant_caps_batch_on_endpoint: u64 = 0x25;
const syscall_install_endpoint: u64 = 0x26;
const syscall_register_iommu_driver: u64 = 0x27;
const syscall_share_cap: u64 = 0x2B;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_get_tick_count: u64 = 0x2D;
const syscall_get_process_slot: u64 = 0x2E;
const syscall_set_fs_base_self: u64 = process_abi.syscall_set_fs_base_self;
const syscall_get_process_status: u64 = process_abi.syscall_get_process_status;
const syscall_process_exit: u64 = process_abi.syscall_process_exit;
const syscall_iommu_authorize: u64 = queue_abi.syscall_iommu_authorize;
const syscall_command_authorize: u64 = queue_abi.syscall_command_authorize;
const syscall_dma_map_create: u64 = queue_abi.syscall_dma_map_create;
const syscall_dma_map_set_state: u64 = queue_abi.syscall_dma_map_set_state;
const syscall_dma_map_release: u64 = queue_abi.syscall_dma_map_release;
const syscall_revoke_device_cap: u64 = queue_abi.syscall_revoke_cap;
const syscall_derive_command_cap: u64 = queue_abi.syscall_derive_command_cap;
const syscall_install_mmio_cap: u64 = 0x2F;
const syscall_install_caps_batch: u64 = 0x32;
const syscall_publish_service_endpoint: u64 = 0x33;
const syscall_accept_cap_transfer: u64 = cap_transfer_abi.syscall_accept_cap_transfer;
const syscall_get_memory_stats: u64 = 0x3C;
const syscall_ipc_call_reply_recv: u64 = 0x40;
const syscall_set_abi_trap_delegate: u64 = trap_abi.syscall_set_abi_trap_delegate;
const syscall_clear_abi_trap_delegate: u64 = trap_abi.syscall_clear_abi_trap_delegate;
const syscall_map_abi_trap_reply_target_pages: u64 = trap_abi.syscall_map_abi_trap_reply_target_pages;
const syscall_copy_from_abi_trap_reply_target: u64 = trap_abi.syscall_copy_from_abi_trap_reply_target;
const syscall_copy_to_abi_trap_reply_target: u64 = trap_abi.syscall_copy_to_abi_trap_reply_target;

const syscall_batch_max_pages: usize = 64;
const user_log_max_bytes: usize = 256;
const syscall_ok: u64 = 0;
const syscall_err_invalid: u64 = 1;
const syscall_err_not_ready: u64 = 2;
const syscall_err_alloc: u64 = 4;
const syscall_err_map: u64 = 5;
const syscall_err_move: u64 = 6;
const syscall_err_drop_present: u64 = 7;
const syscall_err_send: u64 = 8;
const syscall_err_endpoint: u64 = 9;
const syscall_err_revoke: u64 = 10;
const syscall_err_grant: u64 = 11;
const syscall_err_empty: u64 = 13;
const syscall_alloc_map_drop_cap_flag: u64 = 0x2;
const ipc_call_flag_retain_sender: u64 = 0x1;
const ipc_call_flag_signal_only: u64 = 0x2;

pub const Hooks = struct {
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    kernel_state_ready: *const bool,
    enable_cap_table_dump_logs: bool,
    enable_switch_thread_syscall_log: bool,
    scheduler_log_int80: bool,
    scheduler_int80_log_max_lines: u64,
    write: *const fn ([]const u8) void,
    print_hex: *const fn (u64) void,
    print_number: *const fn (u64) void,
    thread_label: *const fn (usize) []const u8,
    principal_label: *const fn (kernel.PrincipalId) []const u8,
    principal_from_process_slot: *const fn (u64) ?kernel.PrincipalId,
    dump_all_process_caps: *const fn (*const kernel.KernelState) void,
    read_user_u64: *const fn (kernel.PrincipalId, u64) ?u64,
    write_user_u64: *const fn (kernel.PrincipalId, u64, u64) bool,
    copy_user_bytes_from_va: *const fn (kernel.PrincipalId, u64, []u8) bool,
    copy_bytes_to_user_va: *const fn (kernel.PrincipalId, u64, []const u8) bool,
    launch_pie_user_thread: *const fn (*TrapFrame) u64,
    spawn_exec: *const fn (*TrapFrame) u64,
    wake_waiting_thread_for_principal: *const fn (kernel.PrincipalId) void,
    wake_blocked_thread_for_principal: *const fn (kernel.PrincipalId) void,
    consume_pending_signal_for_principal: *const fn (kernel.PrincipalId) bool,
    switch_to_thread: *const fn (usize, *TrapFrame, ?u64) bool,
    block_current_thread_for_event: *const fn (*TrapFrame, bool, u64, u64) bool,
    log_queue_cap_deny: *const fn (kernel.PrincipalId, u64, u16, device_capabilities.QueueOperation, anyerror) void,
    log_race_send_cap: *const fn (kernel.PrincipalId, ?kernel.PrincipalId, u64, u64, []const u8) void,
    log_race_switch: *const fn (usize, usize, []const u8) void,
    exit_current_process: *const fn (kernel.PrincipalId, u8, *TrapFrame) void,
    total_usable_memory_bytes: u64,
};

var hooks: ?Hooks = null;
pub export var syscall_return_writeback_enabled: u64 = 1;
const syscall_fast_handled_mask: u64 = 1 << 63;
extern var syscall_entry_is_lstar: u64;

const IpcSignalTarget = struct {
    principal: kernel.PrincipalId,
    thread_index: usize,
};

pub fn init(new_hooks: Hooks) void {
    hooks = new_hooks;
}

fn getHooks() *const Hooks {
    return &(hooks orelse unreachable);
}

fn hasExplicitUserLogLabel(message: []const u8) bool {
    if (message.len < 3 or message[0] != '[') return false;
    const close_index = std.mem.indexOfScalar(u8, message, ']') orelse return false;
    if (close_index <= 1) return false;
    return close_index + 1 == message.len or message[close_index + 1] == ' ';
}

fn writeThreadUserLogPrefix(h: *const Hooks, thread_index: usize) void {
    h.write("[Thread ");
    h.print_number(@intCast(thread_index));
    h.write("] ");
}

fn parseVmObjectRights(bits: u64) kernel.VmObjectRights {
    const abi_rights = image_abi.vmObjectRightsFromBits(bits);
    return @bitCast(abi_rights);
}

fn parseExecImageRights(bits: u64) kernel.ExecImageRights {
    const abi_rights = image_abi.execImageRightsFromBits(bits);
    return @bitCast(abi_rights);
}

fn collectMappedPagesForRange(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    base_va: u64,
    size_bytes: u64,
    out_pages: *[kernel.max_image_backing_pages]u64,
) ?struct {
    page_count: usize,
    page_offset_bytes: u16,
} {
    if (size_bytes == 0) return null;
    const page_base = base_va & ~@as(u64, 0xFFF);
    const page_offset_bytes: u16 = @intCast(base_va & 0xFFF);
    const span_bytes = (@as(u64, page_offset_bytes) + size_bytes + 4095) & ~@as(u64, 4095);
    const page_count_u64 = span_bytes / 4096;
    if (page_count_u64 == 0 or page_count_u64 > kernel.max_image_backing_pages) return null;
    const page_count: usize = @intCast(page_count_u64);

    var i: usize = 0;
    while (i < page_count) : (i += 1) {
        const page_va = page_base + (@as(u64, @intCast(i)) * 4096);
        const page_paddr = capability.lookupUserMappedPaddrForVa(proc, page_va) orelse return null;
        const page_cap = state.getTableConst(proc).find(page_paddr) orelse return null;
        if (!page_cap.rights.cpu_read) return null;
        out_pages[i] = page_paddr;
    }

    return .{
        .page_count = page_count,
        .page_offset_bytes = page_offset_bytes,
    };
}

fn readUserPaddrBatch(
    h: *const Hooks,
    proc: kernel.PrincipalId,
    list_va: u64,
    page_count_u64: u64,
    out_paddrs: *[syscall_batch_max_pages]u64,
) bool {
    var i: u64 = 0;
    while (i < page_count_u64) : (i += 1) {
        const entry_va = list_va + i * 8;
        out_paddrs[@intCast(i)] = h.read_user_u64(proc, entry_va) orelse return false;
    }
    return true;
}

fn transferPageCapOnEndpoint(
    state: *kernel.KernelState,
    h: *const Hooks,
    proc: kernel.PrincipalId,
    endpoint_id: u64,
    paddr: u64,
    retain_sender: bool,
) u64 {
    const to = state.endpointTargetFor(proc, endpoint_id) orelse {
        h.log_race_send_cap(proc, null, endpoint_id, paddr, "endpoint_not_found");
        return syscall_err_endpoint;
    };
    if (retain_sender) {
        state.shareCapOnEndpoint(proc, endpoint_id, paddr) catch |err| switch (err) {
            kernel.KernelError.EndpointNotFound => {
                h.log_race_send_cap(proc, null, endpoint_id, paddr, "endpoint_not_found");
                return syscall_err_endpoint;
            },
            kernel.KernelError.CapabilityNotFound => {
                h.log_race_send_cap(proc, to, endpoint_id, paddr, "cap_missing");
                return syscall_err_send;
            },
            else => {
                h.log_race_send_cap(proc, to, endpoint_id, paddr, @errorName(err));
                return syscall_err_send;
            },
        };
    } else {
        state.sendCapOnEndpoint(proc, endpoint_id, paddr) catch |err| switch (err) {
            kernel.KernelError.EndpointNotFound => {
                h.log_race_send_cap(proc, null, endpoint_id, paddr, "endpoint_not_found");
                return syscall_err_endpoint;
            },
            kernel.KernelError.CapabilityNotFound => {
                h.log_race_send_cap(proc, to, endpoint_id, paddr, "cap_missing");
                return syscall_err_send;
            },
            else => {
                h.log_race_send_cap(proc, to, endpoint_id, paddr, @errorName(err));
                return syscall_err_send;
            },
        };
    }
    h.wake_waiting_thread_for_principal(to);
    if (h.enable_cap_table_dump_logs) h.dump_all_process_caps(state);
    return syscall_ok;
}

fn dispatchIpcSyscall(
    h: *const Hooks,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    frame: *TrapFrame,
) ?u64 {
    return switch (frame.rax) {
        syscall_send_cap => transferPageCapOnEndpoint(state, h, proc, frame.rsi, frame.rdi, false),
        syscall_share_cap => transferPageCapOnEndpoint(state, h, proc, frame.rsi, frame.rdi, true),
        syscall_recv_cap => blk: {
            const received = state.recvCap(proc) catch |err| switch (err) {
                kernel.KernelError.MailboxEmpty => break :blk syscall_err_empty,
                else => break :blk syscall_err_send,
            };
            break :blk received;
        },
        syscall_accept_cap_transfer => blk: {
            const received = state.acceptCapTransfer(proc, frame.rdi) catch |err| switch (err) {
                kernel.KernelError.MailboxEmpty => break :blk syscall_err_empty,
                kernel.KernelError.InvalidState => break :blk syscall_err_invalid,
                kernel.KernelError.CapabilityNotFound => break :blk syscall_err_send,
                kernel.KernelError.TableFull => break :blk syscall_err_alloc,
                else => break :blk syscall_err_send,
            };
            break :blk received;
        },
        syscall_signal_endpoint => blk: {
            break :blk signalEndpointMessage(state, proc, frame.rdi, false, 0, 0, 0, 0);
        },
        syscall_wait_event => blk: {
            const wait_mailbox = (frame.rdi & 0x1) != 0;
            const timeout_ticks = frame.rsi;
            if (wait_mailbox) {
                const received = state.recvCap(proc) catch |err| switch (err) {
                    kernel.KernelError.MailboxEmpty => 0,
                    else => break :blk syscall_err_send,
                };
                if (received >= cap_transfer_abi.transfer_id_min) break :blk received;
            }
            if (consumeQueuedIpcMessageForPrincipal(proc, frame)) break :blk syscall_ok;
            if (h.consume_pending_signal_for_principal(proc)) break :blk syscall_ok;
            if (!h.block_current_thread_for_event(frame, wait_mailbox, timeout_ticks, syscall_ok)) {
                break :blk syscall_err_not_ready;
            }
            break :blk syscall_ok;
        },
        syscall_ipc_call_reply_recv => blk: {
            const endpoint_id = frame.rsi;
            const flags = frame.rdx;
            const signal_only = (flags & ipc_call_flag_signal_only) != 0;
            if (signal_only and endpoint_id == 0) {
                const status = replyToCurrentIpcToken(h, frame.rdi, frame.r8, frame.r9, frame.r10);
                if (status != syscall_ok) break :blk status;
            } else if (signal_only) {
                const status = signalEndpointMessage(state, proc, endpoint_id, true, frame.rdi, frame.r8, frame.r9, frame.r10);
                if (status != syscall_ok) break :blk status;
            } else {
                const status = transferPageCapOnEndpoint(
                    state,
                    h,
                    proc,
                    endpoint_id,
                    frame.rdi,
                    (flags & ipc_call_flag_retain_sender) != 0,
                );
                if (status != syscall_ok) break :blk status;
            }
            if (!signal_only) {
                const received = state.recvCap(proc) catch |err| switch (err) {
                    kernel.KernelError.MailboxEmpty => 0,
                    else => break :blk syscall_err_send,
                };
                if (received >= cap_transfer_abi.transfer_id_min) break :blk received;
            }
            if (consumeQueuedIpcMessageForPrincipal(proc, frame)) break :blk syscall_ok;
            if (h.consume_pending_signal_for_principal(proc)) break :blk syscall_ok;
            if (!h.block_current_thread_for_event(frame, !signal_only, 0, syscall_ok)) {
                break :blk syscall_err_not_ready;
            }
            break :blk syscall_ok;
        },
        else => null,
    };
}

pub export fn syscallIpcDispatch(frame: *TrapFrame) callconv(.c) u64 {
    const h = getHooks();
    const entry_thread = scheduler.current_thread_index;
    syscall_return_writeback_enabled = 1;
    defer {
        if (scheduler.current_thread_index != entry_thread) {
            syscall_return_writeback_enabled = 0;
        }
    }
    if (!h.kernel_state_ready.*) return syscall_err_not_ready;

    const state = h.state;
    const proc = scheduler.current_user_principal;
    return dispatchIpcSyscall(h, state, proc, frame) orelse syscall_err_invalid;
}

pub export fn syscallIpcCallReplyRecvSignalOnlyDispatch(frame: *TrapFrame) callconv(.c) u64 {
    const h = getHooks();
    const entry_thread = scheduler.current_thread_index;
    syscall_return_writeback_enabled = 1;
    defer {
        if (scheduler.current_thread_index != entry_thread) {
            syscall_return_writeback_enabled = 0;
        }
    }
    if (!h.kernel_state_ready.*) return syscall_err_not_ready;

    const proc = scheduler.current_user_principal;
    const status = signalEndpointMessage(h.state, proc, frame.rsi, true, frame.rdi, frame.r8, frame.r9, frame.r10);
    if (status != syscall_ok) return status;
    if (consumeQueuedIpcMessageForPrincipal(proc, frame)) return syscall_ok;
    if (h.consume_pending_signal_for_principal(proc)) return syscall_ok;
    if (!h.block_current_thread_for_event(frame, false, 0, syscall_ok)) return syscall_err_not_ready;
    return syscall_ok;
}

const IpcSignalSave = extern struct {
    r15: u64,
    r14: u64,
    r13: u64,
    r12: u64,
    rbp: u64,
    rbx: u64,
    rip: u64,
    rflags: u64,
    rsp: u64,
    mr0: u64,
    mr1: u64,
    mr2: u64,
    mr3: u64,
};

fn trapFrameFromIpcSignalSave(save: *const IpcSignalSave, rax: u64) TrapFrame {
    return .{
        .r15 = save.r15,
        .r14 = save.r14,
        .r13 = save.r13,
        .r12 = save.r12,
        .r11 = save.rflags,
        .r10 = 0,
        .r9 = 0,
        .r8 = 0,
        .rbp = save.rbp,
        .rdi = 0,
        .rsi = 0,
        .rdx = 0,
        .rcx = save.rip,
        .rbx = save.rbx,
        .rax = rax,
        .rip = save.rip,
        .cs = @as(u64, x86_platform.gdt_user_code_selector) | 0x3,
        .rflags = save.rflags,
        .rsp = save.rsp,
        .ss = @as(u64, x86_platform.gdt_user_data_selector) | 0x3,
    };
}

fn saveIpcSignalFrameToContext(ctx: *scheduler.ThreadContext, save: *const IpcSignalSave, rax: u64) void {
    ctx.frame.r15 = save.r15;
    ctx.frame.r14 = save.r14;
    ctx.frame.r13 = save.r13;
    ctx.frame.r12 = save.r12;
    ctx.frame.rbp = save.rbp;
    ctx.frame.rcx = save.rip;
    ctx.frame.rbx = save.rbx;
    ctx.frame.rax = rax;
    ctx.frame.rip = save.rip;
    ctx.frame.rflags = save.rflags;
    ctx.frame.rsp = save.rsp;
}

fn deliverIpcSignalMessageToContext(ctx: *scheduler.ThreadContext, save: *const IpcSignalSave) void {
    ctx.frame.rax = syscall_ok;
    ctx.frame.rdi = save.mr0;
    ctx.frame.rsi = save.mr1;
    ctx.frame.rdx = save.mr2;
    ctx.frame.r8 = save.mr3;
}

fn deliverIpcMessageToContext(ctx: *scheduler.ThreadContext, mr0: u64, mr1: u64, mr2: u64, mr3: u64) void {
    if (ctx.abi_trap_reply_pending) {
        ctx.abi_trap_reply_pending = false;
        ctx.frame.rax = mr0;
        if (mr2 != 0) ctx.frame.rip = mr2;
        if (mr3 != 0) ctx.frame.rsp = mr3;
        return;
    }
    ctx.frame.rax = syscall_ok;
    ctx.frame.rdi = mr0;
    ctx.frame.rsi = mr1;
    ctx.frame.rdx = mr2;
    ctx.frame.r8 = mr3;
}

fn applyQueuedIpcMessageToFrame(frame: *TrapFrame, msg: scheduler.IpcQueuedMessage) void {
    frame.rax = syscall_ok;
    frame.rdi = msg.mr0;
    frame.rsi = msg.mr1;
    frame.rdx = msg.mr2;
    frame.r8 = msg.mr3;
}

fn grantQueuedReplyToken(receiver_thread: usize, msg: scheduler.IpcQueuedMessage) void {
    if (!msg.grants_reply) return;
    scheduler.setIpcReplyTokenForThread(receiver_thread, true, msg.sender_thread);
}

fn consumeQueuedIpcMessageForThread(thread_index: usize, frame: *TrapFrame) bool {
    if (scheduler.dequeueIpcMessageForThread(thread_index)) |msg| {
        grantQueuedReplyToken(thread_index, msg);
        applyQueuedIpcMessageToFrame(frame, msg);
        return true;
    }
    return false;
}

fn consumeQueuedIpcMessageForPrincipal(principal: kernel.PrincipalId, frame: *TrapFrame) bool {
    const thread_index = scheduler.threadSlotForPrincipal(principal) orelse return false;
    return consumeQueuedIpcMessageForThread(thread_index, frame);
}

fn consumeQueuedAbiTrapReplyForPrincipal(principal: kernel.PrincipalId, frame: *TrapFrame) bool {
    const thread_index = scheduler.threadSlotForPrincipal(principal) orelse return false;
    if (scheduler.dequeueIpcMessageForThread(thread_index)) |msg| {
        grantQueuedReplyToken(thread_index, msg);
        frame.rax = msg.mr0;
        if (msg.mr2 != 0) frame.rip = msg.mr2;
        if (msg.mr3 != 0) frame.rsp = msg.mr3;
        _ = msg.mr1;
        return true;
    }
    return false;
}

fn writeCurrentIpcSignalQueuedReturn(out_frame: *TrapFrame, save: *const IpcSignalSave, msg: scheduler.IpcQueuedMessage) usize {
    out_frame.* = trapFrameFromIpcSignalSave(save, syscall_ok);
    grantQueuedReplyToken(scheduler.current_thread_index, msg);
    applyQueuedIpcMessageToFrame(out_frame, msg);
    return @intFromPtr(out_frame);
}

fn writeCurrentIpcSignalReturn(out_frame: *TrapFrame, save: *const IpcSignalSave, rax: u64) usize {
    out_frame.* = trapFrameFromIpcSignalSave(save, rax);
    return @intFromPtr(out_frame);
}

fn deliverOrQueueIpcMessageToThread(
    target_thread: usize,
    endpoint_id: u64,
    sender_thread: usize,
    grants_reply: bool,
    mr0: u64,
    mr1: u64,
    mr2: u64,
    mr3: u64,
) u64 {
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return syscall_err_endpoint;
    const target_hot = scheduler.getIpcHotThreadConst(target_thread) orelse return syscall_err_endpoint;
    if (target_hot.allocated == 0) return syscall_err_endpoint;
    if (target_hot.ready == 0) {
        target_ctx.wait_mailbox = false;
        target_ctx.wake_tick = 0;
        target_ctx.ready = true;
        scheduler.setIpcHotWaitState(target_thread, false, 0, true);
        if (grants_reply) {
            scheduler.setIpcReplyTokenForThread(target_thread, true, sender_thread);
        }
        deliverIpcMessageToContext(target_ctx, mr0, mr1, mr2, mr3);
        scheduler.preferIpcSwitchToThread(target_thread);
        return syscall_ok;
    }
    if (!scheduler.enqueueIpcMessageForThread(target_thread, endpoint_id, sender_thread, grants_reply, mr0, mr1, mr2, mr3)) {
        return syscall_err_not_ready;
    }
    scheduler.preferIpcSwitchToThread(target_thread);
    return syscall_ok;
}

fn signalEndpointMessage(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    endpoint_id: u64,
    grants_reply: bool,
    mr0: u64,
    mr1: u64,
    mr2: u64,
    mr3: u64,
) u64 {
    const target_principal = state.endpointTargetFor(owner, endpoint_id) orelse return syscall_err_endpoint;
    const target_thread = scheduler.threadSlotForPrincipal(target_principal) orelse return syscall_err_endpoint;
    return deliverOrQueueIpcMessageToThread(
        target_thread,
        endpoint_id,
        scheduler.current_thread_index,
        grants_reply,
        mr0,
        mr1,
        mr2,
        mr3,
    );
}

fn replyToCurrentIpcToken(h: *const Hooks, mr0: u64, mr1: u64, mr2: u64, mr3: u64) u64 {
    const current_thread = scheduler.current_thread_index;
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return syscall_err_not_ready;
    if (current_hot.ipc_reply_token_valid == 0) return syscall_err_endpoint;
    const target_thread = current_hot.ipc_reply_token_target_thread;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return syscall_err_endpoint;
    if (!target_ctx.allocated) return syscall_err_endpoint;
    scheduler.setIpcReplyTokenForThread(current_thread, false, 0);
    if (target_ctx.abi_trap_reply_pending and (mr1 & trap_abi.response_flag_exit) != 0) {
        const target_proc = target_ctx.owner_process;
        target_ctx.abi_trap_reply_pending = false;
        _ = h.state.markProcessExited(target_proc);
        _ = scheduler.releaseThreadSlot(target_thread);
        return syscall_ok;
    }
    return deliverOrQueueIpcMessageToThread(target_thread, 0, current_thread, false, mr0, mr1, mr2, mr3);
}

fn writeAbiTrapRequestU64(h: *const Hooks, target: kernel.PrincipalId, request_page_va: u64, offset: u64, value: u64) bool {
    return h.write_user_u64(target, request_page_va + offset, value);
}

fn writeAbiTrapRequest(
    h: *const Hooks,
    target: kernel.PrincipalId,
    request_page_va: u64,
    caller: kernel.PrincipalId,
    thread_id: u64,
    flavor: u32,
    frame: *const TrapFrame,
) bool {
    const version_kind = @as(u64, trap_abi.version) |
        (@as(u64, @intFromEnum(trap_abi.TrapKind.abi_syscall)) << 32);
    const flavor_reserved = @as(u64, flavor);

    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x00, trap_abi.magic)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x08, version_kind)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x10, flavor_reserved)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x18, @intFromEnum(caller))) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x20, thread_id)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x28, frame.rip)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x30, frame.rsp)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x38, 0)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x40, 0)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x48, frame.rax)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x50, frame.rdi)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x58, frame.rsi)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x60, frame.rdx)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x68, frame.r10)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x70, frame.r8)) return false;
    if (!writeAbiTrapRequestU64(h, target, request_page_va, 0x78, frame.r9)) return false;
    return true;
}

fn currentIpcReplyTargetPrincipal() ?kernel.PrincipalId {
    const current_thread = scheduler.current_thread_index;
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return null;
    if (current_hot.ipc_reply_token_valid == 0) return null;
    const target_thread = current_hot.ipc_reply_token_target_thread;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return null;
    if (!target_ctx.allocated) return null;
    return target_ctx.owner_process;
}

fn copyFromCurrentIpcReplyTarget(h: *const Hooks, proc: kernel.PrincipalId, dst_current_va: u64, src_target_va: u64, len_u64: u64) u64 {
    if (len_u64 > @as(u64, trap_abi.abi_trap_copy_max_bytes)) return syscall_err_invalid;
    const target_proc = currentIpcReplyTargetPrincipal() orelse return syscall_err_endpoint;
    const len: usize = @intCast(len_u64);
    if (len == 0) return 0;

    var scratch: [trap_abi.abi_trap_copy_max_bytes]u8 = undefined;
    const bytes = scratch[0..len];
    if (!h.copy_user_bytes_from_va(target_proc, src_target_va, bytes)) return syscall_err_invalid;
    if (!h.copy_bytes_to_user_va(proc, dst_current_va, bytes)) return syscall_err_invalid;
    return len_u64;
}

fn copyToCurrentIpcReplyTarget(h: *const Hooks, proc: kernel.PrincipalId, dst_target_va: u64, src_current_va: u64, len_u64: u64) u64 {
    if (len_u64 > @as(u64, trap_abi.abi_trap_copy_max_bytes)) return syscall_err_invalid;
    const target_proc = currentIpcReplyTargetPrincipal() orelse return syscall_err_endpoint;
    const len: usize = @intCast(len_u64);
    if (len == 0) return 0;

    var scratch: [trap_abi.abi_trap_copy_max_bytes]u8 = undefined;
    const bytes = scratch[0..len];
    if (!h.copy_user_bytes_from_va(proc, src_current_va, bytes)) return syscall_err_invalid;
    if (!h.copy_bytes_to_user_va(target_proc, dst_target_va, bytes)) return syscall_err_invalid;
    return len_u64;
}

fn mapPagesToCurrentIpcReplyTarget(h: *const Hooks, target_va: u64, page_count: u64, prot_bits: u64) u64 {
    if ((target_va & 0xFFF) != 0) return syscall_err_invalid;
    if (page_count == 0 or page_count > syscall_batch_max_pages) return syscall_err_invalid;
    const current_thread = scheduler.current_thread_index;
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return syscall_err_not_ready;
    if (current_hot.ipc_reply_token_valid == 0) return syscall_err_endpoint;
    const target_thread = current_hot.ipc_reply_token_target_thread;
    const target_ctx = scheduler.getThreadContext(target_thread) orelse return syscall_err_endpoint;
    if (!target_ctx.allocated) return syscall_err_endpoint;
    const target_proc = target_ctx.owner_process;
    const abi_prot = process_builder_abi.mapProtFromBits(prot_bits);
    const prot = kernel.MapProt{
        .read = abi_prot.read,
        .write = abi_prot.write,
        .exec = abi_prot.exec,
    };
    if (!prot.read or (prot.write and prot.exec)) return syscall_err_invalid;

    var page_index: u64 = 0;
    while (page_index < page_count) : (page_index += 1) {
        const page = h.state.allocPageTo(target_proc, h.free_list) catch return syscall_err_alloc;
        const va = target_va + page_index * 4096;
        if (!user_vm.mapUserLinearRegionWithProt(target_proc, va, page.paddr, 4096, prot)) return syscall_err_map;
    }
    return syscall_ok;
}

fn resolveIpcSignalTargetThread(
    state: *const kernel.KernelState,
    current_ctx: *scheduler.ThreadContext,
    owner: kernel.PrincipalId,
    endpoint_id: u64,
) ?IpcSignalTarget {
    _ = current_ctx;
    const generation = state.endpoint_generation;
    const current_thread = scheduler.current_thread_index;
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return null;
    if (current_hot.ipc_cached_endpoint_generation == generation and
        current_hot.ipc_cached_endpoint_id == endpoint_id)
    {
        return .{
            .principal = current_hot.ipc_cached_target,
            .thread_index = current_hot.ipc_cached_target_thread,
        };
    }

    const target = state.endpointTargetFor(owner, endpoint_id) orelse return null;
    const thread_index = scheduler.threadSlotForPrincipal(target) orelse return null;
    scheduler.setIpcEndpointCacheForThread(current_thread, generation, endpoint_id, target, thread_index);
    return .{ .principal = target, .thread_index = thread_index };
}

pub export fn syscallIpcCallReplyRecvSignalOnlySparse(endpoint_id: u64, save: *const IpcSignalSave, out_frame: *TrapFrame) callconv(.c) usize {
    const h = getHooks();
    if (!h.kernel_state_ready.*) return writeCurrentIpcSignalReturn(out_frame, save, syscall_err_not_ready);

    const proc = scheduler.current_user_principal;
    const current_thread = scheduler.current_thread_index;
    const current_ctx = scheduler.getThreadContext(current_thread) orelse return writeCurrentIpcSignalReturn(out_frame, save, syscall_err_not_ready);
    const target = resolveIpcSignalTargetThread(h.state, current_ctx, proc, endpoint_id) orelse return writeCurrentIpcSignalReturn(out_frame, save, syscall_err_endpoint);
    const target_ctx = scheduler.getThreadContext(target.thread_index) orelse return writeCurrentIpcSignalReturn(out_frame, save, syscall_err_endpoint);
    const current_hot = scheduler.getIpcHotThreadConst(current_thread) orelse return writeCurrentIpcSignalReturn(out_frame, save, syscall_err_not_ready);
    const target_hot = scheduler.getIpcHotThreadConst(target.thread_index) orelse return writeCurrentIpcSignalReturn(out_frame, save, syscall_err_endpoint);
    if (target_hot.allocated == 0 or target_hot.owner_process != target.principal) return writeCurrentIpcSignalReturn(out_frame, save, syscall_err_endpoint);
    if (target.thread_index == current_thread) return writeCurrentIpcSignalReturn(out_frame, save, syscall_err_not_ready);

    const is_reply = current_hot.ipc_reply_token_valid != 0;
    if (is_reply) {
        if (current_hot.ipc_reply_token_target_thread != target.thread_index) {
            return writeCurrentIpcSignalReturn(out_frame, save, syscall_err_endpoint);
        }
        scheduler.setIpcReplyTokenForThread(current_thread, false, 0);
    }

    const target_was_ready = target_hot.ready != 0;
    const send_status = deliverOrQueueIpcMessageToThread(
        target.thread_index,
        endpoint_id,
        current_thread,
        true,
        save.mr0,
        save.mr1,
        save.mr2,
        save.mr3,
    );
    if (send_status != syscall_ok) return writeCurrentIpcSignalReturn(out_frame, save, send_status);

    if (scheduler.dequeueIpcMessageForThread(current_thread)) |msg| {
        return writeCurrentIpcSignalQueuedReturn(out_frame, save, msg);
    }

    if (current_hot.signal_pending != 0) {
        current_ctx.signal_pending = false;
        scheduler.setIpcHotSignalPending(current_thread, false);
        return writeCurrentIpcSignalReturn(out_frame, save, syscall_ok);
    }

    if (current_hot.allocated == 0) {
        return writeCurrentIpcSignalReturn(out_frame, save, syscall_err_not_ready);
    }
    if (target_was_ready) {
        out_frame.* = trapFrameFromIpcSignalSave(save, syscall_ok);
        if (!scheduler.blockCurrentThreadForEvent(out_frame, false, 0, syscall_ok)) {
            return writeCurrentIpcSignalReturn(out_frame, save, syscall_err_not_ready);
        }
        return @intFromPtr(out_frame);
    }

    saveIpcSignalFrameToContext(current_ctx, save, syscall_ok);
    current_ctx.cr3 = scheduler.user_cr3_value;
    current_ctx.wait_mailbox = false;
    current_ctx.wake_tick = 0;
    current_ctx.ready = false;
    scheduler.setIpcHotCr3(current_thread, scheduler.user_cr3_value);
    scheduler.setIpcHotWaitState(current_thread, false, 0, false);

    scheduler.current_thread_index = target.thread_index;
    scheduler.current_user_principal = target.principal;
    scheduler.user_cr3_value = target_hot.cr3;
    _ = scheduler.applyThreadFsBase(target.thread_index);

    return @intFromPtr(&target_ctx.frame);
}

pub export fn syscallIpcFastDispatch(nr: u64, arg0: u64, arg1: u64, arg2: u64) callconv(.c) u64 {
    const h = getHooks();
    if (!h.kernel_state_ready.*) return syscall_fast_handled_mask | syscall_err_not_ready;

    const state = h.state;
    const proc = scheduler.current_user_principal;
    const result: u64 = switch (nr) {
        syscall_send_cap => transferPageCapOnEndpoint(state, h, proc, arg1, arg0, false),
        syscall_share_cap => transferPageCapOnEndpoint(state, h, proc, arg1, arg0, true),
        syscall_recv_cap => blk: {
            const received = state.recvCap(proc) catch |err| switch (err) {
                kernel.KernelError.MailboxEmpty => break :blk syscall_err_empty,
                else => break :blk syscall_err_send,
            };
            break :blk received;
        },
        syscall_accept_cap_transfer => blk: {
            const received = state.acceptCapTransfer(proc, arg0) catch |err| switch (err) {
                kernel.KernelError.MailboxEmpty => break :blk syscall_err_empty,
                kernel.KernelError.InvalidState => break :blk syscall_err_invalid,
                kernel.KernelError.CapabilityNotFound => break :blk syscall_err_send,
                kernel.KernelError.TableFull => break :blk syscall_err_alloc,
                else => break :blk syscall_err_send,
            };
            break :blk received;
        },
        syscall_signal_endpoint => blk: {
            break :blk signalEndpointMessage(state, proc, arg0, false, 0, 0, 0, 0);
        },
        syscall_ipc_call_reply_recv => blk: {
            const endpoint_id = arg1;
            const flags = arg2;
            const to = state.endpointTargetFor(proc, endpoint_id) orelse break :blk syscall_err_endpoint;
            if (to != proc) return 0;
            if ((flags & ipc_call_flag_signal_only) != 0) {
                h.wake_blocked_thread_for_principal(to);
                if (h.consume_pending_signal_for_principal(proc)) break :blk syscall_ok;
                break :blk syscall_err_not_ready;
            }
            const status = transferPageCapOnEndpoint(
                state,
                h,
                proc,
                endpoint_id,
                arg0,
                (flags & ipc_call_flag_retain_sender) != 0,
            );
            if (status != syscall_ok) break :blk status;
            const received = state.recvCap(proc) catch |err| switch (err) {
                kernel.KernelError.MailboxEmpty => break :blk syscall_err_empty,
                else => break :blk syscall_err_send,
            };
            break :blk received;
        },
        else => syscall_err_invalid,
    };
    return syscall_fast_handled_mask | result;
}

fn dispatchAbiTrapDelegate(
    h: *const Hooks,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    frame: *TrapFrame,
) ?u64 {
    const delegate = state.abiTrapDelegateFor(proc) orelse return null;
    const current_thread = scheduler.current_thread_index;
    const current_ctx = scheduler.getThreadContext(current_thread) orelse return syscall_err_not_ready;
    const target_principal = state.endpointTargetFor(proc, delegate.endpoint_id) orelse return syscall_err_endpoint;
    const target_thread = scheduler.threadSlotForPrincipal(target_principal) orelse return syscall_err_endpoint;
    if (!writeAbiTrapRequest(h, target_principal, delegate.request_page_va, proc, @intCast(current_thread), delegate.flavor, frame)) {
        h.write("abi_trap request write failed target=");
        h.write(h.principal_label(target_principal));
        h.write("\n");
        return syscall_err_invalid;
    }

    const status = deliverOrQueueIpcMessageToThread(
        target_thread,
        delegate.endpoint_id,
        current_thread,
        true,
        delegate.request_page_va,
        0,
        0,
        0,
    );
    if (status != syscall_ok) return status;

    if (consumeQueuedAbiTrapReplyForPrincipal(proc, frame)) return frame.rax;
    if (h.consume_pending_signal_for_principal(proc)) return syscall_ok;

    current_ctx.abi_trap_reply_pending = true;
    if (!h.block_current_thread_for_event(frame, false, 0, syscall_err_not_ready)) {
        current_ctx.abi_trap_reply_pending = false;
        return syscall_err_not_ready;
    }
    return syscall_ok;
}

pub export fn syscallDispatch(frame: *TrapFrame) callconv(.c) u64 {
    const h = getHooks();
    const entry_thread = scheduler.current_thread_index;
    syscall_return_writeback_enabled = 1;
    defer {
        if (scheduler.current_thread_index != entry_thread) {
            // When a syscall returns to a different thread, the loaded frame
            // already belongs to that target thread and must not inherit the
            // caller's syscall result in RAX.
            syscall_return_writeback_enabled = 0;
        }
    }
    if (!h.kernel_state_ready.*) return syscall_err_not_ready;

    const state = h.state;
    const proc = scheduler.current_user_principal;
    if (dispatchIpcSyscall(h, state, proc, frame)) |result| return result;
    if (syscall_entry_is_lstar != 0) {
        if (dispatchAbiTrapDelegate(h, state, proc, frame)) |result| return result;
    }
    if (h.scheduler_log_int80 and scheduler.scheduler_int80_log_count < h.scheduler_int80_log_max_lines) {
        h.write("INT80 dispatch ");
        h.write(h.thread_label(scheduler.current_thread_index));
        h.write("/");
        h.write(h.principal_label(proc));
        h.write(" SYS=");
        h.print_hex(frame.rax);
        h.write("\n");
        scheduler.scheduler_int80_log_count +%= 1;
    }

    switch (frame.rax) {
        syscall_alloc_page => {
            const cap = state.allocPageTo(proc, h.free_list) catch |err| {
                h.write("sys_alloc_page failed proc=");
                h.write(h.principal_label(proc));
                h.write(" err=");
                h.write(@errorName(err));
                h.write(" caps=");
                h.print_number(@intCast(state.getTableConst(proc).len));
                h.write("/");
                h.print_number(kernel.CNode.max_caps);
                h.write(" free_pages=");
                h.print_number(@intCast(h.free_list.len));
                h.write("\n");
                return syscall_err_alloc;
            };
            return cap.paddr;
        },
        syscall_map_page, syscall_map_mmio => {
            const writable = (frame.rdx & 0x1) != 0;
            if (capability.mapUserPageFromCapability(state, proc, frame.rdi, frame.rsi, writable)) {
                return syscall_ok;
            }
            return syscall_err_map;
        },
        syscall_map_pages_batch => {
            const page_count_u64 = frame.rdx;
            if (page_count_u64 == 0 or page_count_u64 > syscall_batch_max_pages) return syscall_err_invalid;
            var paddrs: [syscall_batch_max_pages]u64 = undefined;
            const page_count: usize = @intCast(page_count_u64);
            const buf = std.mem.sliceAsBytes(paddrs[0..page_count]);
            if (!h.copy_user_bytes_from_va(proc, frame.rsi, buf)) return syscall_err_invalid;
            if (capability.mapUserPagesFromCapabilityBatch(state, proc, frame.rdi, paddrs[0..page_count], (frame.rcx & 0x1) != 0)) {
                return syscall_ok;
            }
            return syscall_err_map;
        },
        syscall_alloc_map_pages => {
            const page_count_u64 = frame.rsi;
            if (page_count_u64 == 0 or page_count_u64 > syscall_batch_max_pages) return syscall_err_invalid;
            untyped_memory.allocMapPages(.{
                .state = state,
                .proc = proc,
                .free_list = h.free_list,
                .base_va = frame.rdi,
                .page_count = @intCast(page_count_u64),
                .writable = (frame.rdx & 0x1) != 0,
                .drop_cap_after_map = (frame.rdx & syscall_alloc_map_drop_cap_flag) != 0,
                .out_paddr_list_va = frame.rcx,
                .write_user_u64 = h.write_user_u64,
            }) catch |err| switch (err) {
                error.InvalidArgument => {
                    h.write("sys_alloc_map_pages invalid proc=");
                    h.write(h.principal_label(proc));
                    h.write(" base_va=");
                    h.print_hex(frame.rdi);
                    h.write(" pages=");
                    h.print_number(page_count_u64);
                    h.write(" out_va=");
                    h.print_hex(frame.rcx);
                    h.write("\n");
                    return syscall_err_invalid;
                },
                error.AllocationFailed => {
                    h.write("sys_alloc_map_pages alloc failed proc=");
                    h.write(h.principal_label(proc));
                    h.write(" base_va=");
                    h.print_hex(frame.rdi);
                    h.write(" pages=");
                    h.print_number(page_count_u64);
                    h.write(" caps=");
                    h.print_number(@intCast(state.getTableConst(proc).len));
                    h.write("/");
                    h.print_number(kernel.CNode.max_caps);
                    h.write(" free_pages=");
                    h.print_number(@intCast(h.free_list.len));
                    h.write("\n");
                    return syscall_err_alloc;
                },
                error.MapFailed => {
                    var existing_idx: ?usize = null;
                    var existing_paddr: u64 = 0;
                    var scan_i: usize = 0;
                    const scan_pages: usize = @intCast(page_count_u64);
                    while (scan_i < scan_pages) : (scan_i += 1) {
                        const offset = @as(u64, @intCast(scan_i)) * 4096;
                        const va = frame.rdi + offset;
                        if (capability.lookupUserMappedPaddrForVa(proc, va)) |paddr| {
                            existing_idx = scan_i;
                            existing_paddr = paddr;
                            break;
                        }
                    }
                    h.write("sys_alloc_map_pages map failed proc=");
                    h.write(h.principal_label(proc));
                    h.write(" base_va=");
                    h.print_hex(frame.rdi);
                    h.write(" pages=");
                    h.print_number(page_count_u64);
                    h.write(" out_va=");
                    h.print_hex(frame.rcx);
                    if (existing_idx) |idx| {
                        h.write(" first_existing_idx=");
                        h.print_number(@intCast(idx));
                        h.write(" first_existing_va=");
                        h.print_hex(frame.rdi + (@as(u64, @intCast(idx)) * 4096));
                        h.write(" first_existing_paddr=");
                        h.print_hex(existing_paddr);
                    }
                    h.write("\n");
                    return syscall_err_map;
                },
            };
            return syscall_ok;
        },
        syscall_untyped_alloc => {
            const token = untyped_memory.allocUntyped(state, proc, h.free_list, frame.rdi, frame.rsi, @bitCast(frame.rdx)) catch |err| switch (err) {
                kernel.KernelError.InvalidState => return syscall_err_invalid,
                kernel.KernelError.OutOfFreePages, kernel.KernelError.TooManyFreeRanges, kernel.KernelError.TooManyUntypedBlocks, kernel.KernelError.TableFull => return syscall_err_alloc,
                else => return syscall_err_alloc,
            };
            return token;
        },
        syscall_untyped_retype_pages => {
            const page_count_u64 = frame.rdx;
            if (page_count_u64 == 0 or page_count_u64 > kernel.max_retype_page_batch) return syscall_err_invalid;
            untyped_memory.retypeMapPages(state, proc, frame.rdi, frame.rsi, @intCast(page_count_u64), frame.r10, frame.r8, h.write_user_u64) catch |err| switch (err) {
                error.InvalidArgument => return syscall_err_invalid,
                error.AllocationFailed => return syscall_err_alloc,
                error.MapFailed => return syscall_err_map,
            };
            return syscall_ok;
        },
        syscall_untyped_reset => {
            untyped_memory.resetUntyped(state, proc, frame.rdi) catch |err| switch (err) {
                kernel.KernelError.InvalidState, kernel.KernelError.UntypedNotFound => return syscall_err_invalid,
                kernel.KernelError.UntypedHasChildren => return syscall_err_not_ready,
                else => return syscall_err_revoke,
            };
            return syscall_ok;
        },
        syscall_untyped_alloc_map_pages => {
            const page_count_u64 = frame.rsi;
            if (page_count_u64 == 0 or page_count_u64 > kernel.max_retype_page_batch) return syscall_err_invalid;
            untyped_memory.allocOwnedUntypedMapPages(state, proc, frame.rdi, @intCast(page_count_u64), frame.rdx, frame.rcx, h.write_user_u64) catch |err| switch (err) {
                error.InvalidArgument => return syscall_err_invalid,
                error.AllocationFailed => return syscall_err_alloc,
                error.MapFailed => return syscall_err_map,
            };
            return syscall_ok;
        },
        syscall_queue_submit, syscall_queue_notify => {
            const queue_token = frame.rdi;
            const queue_index: u16 = @truncate(frame.rsi);
            const op: device_capabilities.QueueOperation = if (frame.rax == syscall_queue_submit) .submit else .notify;
            device_capabilities.queueCapAuthorizeStage2(state, proc, queue_token, queue_index, op) catch |err| {
                h.log_queue_cap_deny(proc, queue_token, queue_index, op, err);
                return syscall_err_invalid;
            };
            return syscall_ok;
        },
        syscall_iommu_authorize => {
            const device: kernel.DmaDeviceId = std.meta.intToEnum(kernel.DmaDeviceId, @as(u8, @truncate(frame.rsi))) catch return syscall_err_invalid;
            const op: device_capabilities.IommuOperation = std.meta.intToEnum(device_capabilities.IommuOperation, @as(u8, @truncate(frame.rdx))) catch return syscall_err_invalid;
            device_capabilities.iommuCapAuthorizeStage2(state, proc, frame.rdi, device, op) catch return syscall_err_invalid;
            return syscall_ok;
        },
        syscall_command_authorize => {
            const device: kernel.DmaDeviceId = std.meta.intToEnum(kernel.DmaDeviceId, @as(u8, @truncate(frame.rsi))) catch return syscall_err_invalid;
            const opcode: device_capabilities.CommandOpcodeClass = std.meta.intToEnum(device_capabilities.CommandOpcodeClass, @as(u8, @truncate(frame.rdx))) catch return syscall_err_invalid;
            device_capabilities.commandCapAuthorizeStage2(state, proc, frame.rdi, device, opcode) catch return syscall_err_invalid;
            return syscall_ok;
        },
        syscall_dma_map_create => {
            const device: kernel.DmaDeviceId = std.meta.intToEnum(kernel.DmaDeviceId, @as(u8, @truncate(frame.rdi))) catch return syscall_err_invalid;
            const direction: kernel.DmaDirection = std.meta.intToEnum(kernel.DmaDirection, @as(u8, @truncate(frame.r8))) catch return syscall_err_invalid;
            const token = state.dmaMapCreateStage1(proc, device, frame.rsi, frame.rdx, direction) catch |err| switch (err) {
                kernel.KernelError.InvalidState => return syscall_err_invalid,
                kernel.KernelError.TableFull => return syscall_err_alloc,
                else => return syscall_err_invalid,
            };
            return queue_abi.encodeDmaMappingToken(token);
        },
        syscall_dma_map_set_state => {
            const mapping = state.dmaMapFindStage1(frame.rdi) orelse return syscall_err_invalid;
            if (mapping.owner_principal_raw != @intFromEnum(proc)) return syscall_err_invalid;
            const next_state: kernel.DmaMappingState = std.meta.intToEnum(kernel.DmaMappingState, @as(u8, @truncate(frame.rsi))) catch return syscall_err_invalid;
            state.dmaMapSetStateStage1(frame.rdi, next_state) catch |err| switch (err) {
                kernel.KernelError.CapabilityNotFound => return syscall_err_invalid,
                kernel.KernelError.InvalidState => return syscall_err_invalid,
                else => return syscall_err_invalid,
            };
            return syscall_ok;
        },
        syscall_dma_map_release => {
            const mapping = state.dmaMapFindStage1(frame.rdi) orelse return syscall_err_invalid;
            if (mapping.owner_principal_raw != @intFromEnum(proc)) return syscall_err_invalid;
            state.dmaMapReleaseStage1(frame.rdi) catch |err| switch (err) {
                kernel.KernelError.CapabilityNotFound => return syscall_err_invalid,
                kernel.KernelError.InvalidState => return syscall_err_invalid,
                else => return syscall_err_invalid,
            };
            return syscall_ok;
        },
        syscall_revoke_device_cap => {
            const decoded = queue_abi.decodeCapToken(frame.rdi) orelse return syscall_err_invalid;
            device_capabilities.revokeDeviceCapStage2(state, proc, decoded.kind, decoded.token) catch |err| switch (err) {
                kernel.KernelError.CapabilityNotFound => return syscall_err_invalid,
                kernel.KernelError.InvalidState => return syscall_err_invalid,
                else => return syscall_err_revoke,
            };
            return syscall_ok;
        },
        syscall_derive_command_cap => {
            const token = queue_abi.decodeCommandCapToken(frame.rdi) orelse return syscall_err_invalid;
            const child_token = device_capabilities.deriveCommandCapStage2(state, proc, token, frame.rsi) catch |err| switch (err) {
                kernel.KernelError.CapabilityNotFound => return syscall_err_invalid,
                kernel.KernelError.InvalidState => return syscall_err_invalid,
                kernel.KernelError.TableFull => return syscall_err_alloc,
                else => return syscall_err_grant,
            };
            return queue_abi.encodeCommandCapToken(child_token);
        },
        syscall_move_cap => {
            const to = switch (frame.rsi) {
                0 => proc,
                1 => kernel.PrincipalId.Device0,
                else => return syscall_err_invalid,
            };
            const from = if (to == proc) kernel.PrincipalId.Device0 else proc;
            const rights = capability.parseRights(frame.rdx);
            state.moveCap(from, to, frame.rdi, rights) catch return syscall_err_move;
            return syscall_ok;
        },
        syscall_grant_cap => {
            const to = h.principal_from_process_slot(frame.rsi) orelse return syscall_err_invalid;
            const rights = capability.parseRights(frame.rdx);
            state.grantCap(proc, to, frame.rdi, rights) catch return syscall_err_grant;
            if (h.enable_cap_table_dump_logs) h.dump_all_process_caps(state);
            return syscall_ok;
        },
        syscall_grant_caps_batch => {
            const page_count_u64 = frame.rsi;
            if (page_count_u64 == 0 or page_count_u64 > syscall_batch_max_pages) return syscall_err_invalid;
            const to = h.principal_from_process_slot(frame.rdx) orelse return syscall_err_invalid;
            const rights = capability.parseRights(frame.rcx);
            var paddrs: [syscall_batch_max_pages]u64 = undefined;
            if (!readUserPaddrBatch(h, proc, frame.rdi, page_count_u64, &paddrs)) return syscall_err_invalid;
            state.grantCapsBatch(proc, to, paddrs[0..@intCast(page_count_u64)], rights) catch return syscall_err_grant;
            if (rights.dma and page_count_u64 > 1) {
                h.write("grant_caps_batch dma to=");
                h.write(h.principal_label(to));
                h.write(" pages=");
                h.print_number(page_count_u64);
                h.write(" first=");
                h.print_hex(paddrs[0]);
                h.write(" last=");
                h.print_hex(paddrs[@intCast(page_count_u64 - 1)]);
                h.write("\n");
            }
            if (h.enable_cap_table_dump_logs) h.dump_all_process_caps(state);
            return syscall_ok;
        },
        syscall_install_caps_batch => {
            if (!state.isBootstrapOwner(proc)) return syscall_err_invalid;
            const page_count_u64 = frame.rsi;
            if (page_count_u64 == 0 or page_count_u64 > syscall_batch_max_pages) return syscall_err_invalid;
            const rights = capability.parseRights(frame.rdx);
            var paddrs: [syscall_batch_max_pages]u64 = undefined;
            if (!readUserPaddrBatch(h, proc, frame.rdi, page_count_u64, &paddrs)) return syscall_err_invalid;
            var i: usize = 0;
            while (i < page_count_u64) : (i += 1) {
                state.installCap(proc, paddrs[i], rights) catch return syscall_err_grant;
            }
            if (h.enable_cap_table_dump_logs) h.dump_all_process_caps(state);
            return syscall_ok;
        },
        syscall_grant_cap_on_endpoint => {
            const endpoint_id = frame.rsi;
            const rights = capability.parseRights(frame.rdx);
            state.grantCapOnEndpoint(proc, endpoint_id, frame.rdi, rights) catch |err| switch (err) {
                kernel.KernelError.EndpointNotFound => return syscall_err_endpoint,
                else => return syscall_err_grant,
            };
            if (h.enable_cap_table_dump_logs) h.dump_all_process_caps(state);
            return syscall_ok;
        },
        syscall_grant_caps_batch_on_endpoint => {
            const page_count_u64 = frame.rsi;
            if (page_count_u64 == 0 or page_count_u64 > syscall_batch_max_pages) return syscall_err_invalid;
            const endpoint_id = frame.rdx;
            const rights = capability.parseRights(frame.rcx);
            var paddrs: [syscall_batch_max_pages]u64 = undefined;
            if (!readUserPaddrBatch(h, proc, frame.rdi, page_count_u64, &paddrs)) return syscall_err_invalid;
            state.grantCapsBatchOnEndpoint(proc, endpoint_id, paddrs[0..@intCast(page_count_u64)], rights) catch |err| switch (err) {
                kernel.KernelError.EndpointNotFound => return syscall_err_endpoint,
                else => return syscall_err_grant,
            };
            if (h.enable_cap_table_dump_logs) h.dump_all_process_caps(state);
            return syscall_ok;
        },
        syscall_install_endpoint => {
            const target = h.principal_from_process_slot(frame.rdx) orelse return syscall_err_invalid;
            state.installEndpoint(proc, frame.rsi, target) catch |err| switch (err) {
                kernel.KernelError.InvalidState => return syscall_err_invalid,
                kernel.KernelError.TableFull => return syscall_err_alloc,
                else => return syscall_err_endpoint,
            };
            return syscall_ok;
        },
        syscall_signal_endpoint => {
            return signalEndpointMessage(state, proc, frame.rdi, false, 0, 0, 0, 0);
        },
        syscall_get_tick_count => {
            return scheduler.lapic_tick_count;
        },
        syscall_get_process_slot => {
            const slot = kernel.processIndexFromPrincipal(proc) orelse return syscall_err_invalid;
            return @intCast(slot);
        },
        syscall_set_fs_base_self => {
            const fs_base = frame.rdi;
            if (fs_base != 0 and !capability.isUserCanonicalVa(fs_base)) return syscall_err_invalid;
            if (!scheduler.setCurrentThreadFsBase(fs_base)) return syscall_err_not_ready;
            return syscall_ok;
        },
        syscall_get_process_status => {
            const target = h.principal_from_process_slot(frame.rdi) orelse return syscall_err_invalid;
            const status = state.processStatus(target);
            const kind: process_abi.ProcessStatusKind = if (status.active)
                .active
            else if (status.faulted)
                .faulted
            else
                .inactive;
            return process_abi.encodeProcessStatus(kind, status.fault_vector);
        },
        syscall_get_memory_stats => {
            const out_va = frame.rdi;
            if ((out_va & 0x7) != 0) return syscall_err_invalid;
            const free_bytes = @as(u64, @intCast(h.free_list.len)) * 4096;
            const total_bytes = h.total_usable_memory_bytes;
            const used_bytes = if (total_bytes >= free_bytes) total_bytes - free_bytes else 0;
            if (!h.write_user_u64(proc, out_va + 0, total_bytes)) return syscall_err_invalid;
            if (!h.write_user_u64(proc, out_va + 8, used_bytes)) return syscall_err_invalid;
            if (!h.write_user_u64(proc, out_va + 16, free_bytes)) return syscall_err_invalid;
            if (!h.write_user_u64(proc, out_va + 24, 4096)) return syscall_err_invalid;
            return syscall_ok;
        },
        syscall_process_exit => {
            h.exit_current_process(proc, @truncate(frame.rdi), frame);
            return syscall_ok;
        },
        syscall_register_iommu_driver => {
            return syscall_ok;
        },
        syscall_send_cap => {
            return transferPageCapOnEndpoint(state, h, proc, frame.rsi, frame.rdi, false);
        },
        syscall_share_cap => {
            return transferPageCapOnEndpoint(state, h, proc, frame.rsi, frame.rdi, true);
        },
        syscall_recv_cap => {
            const received = state.recvCap(proc) catch |err| switch (err) {
                kernel.KernelError.MailboxEmpty => syscall_err_empty,
                else => syscall_err_send,
            };
            return received;
        },
        syscall_install_mmio_cap => {
            if (!state.isBootstrapOwner(proc)) return syscall_err_invalid;
            const paddr = frame.rdi;
            const rights = capability.parseRights(frame.rsi);
            state.installCap(proc, paddr, rights) catch return syscall_err_grant;
            return syscall_ok;
        },
        syscall_accept_cap_transfer => {
            const received = state.acceptCapTransfer(proc, frame.rdi) catch |err| switch (err) {
                kernel.KernelError.MailboxEmpty => syscall_err_empty,
                kernel.KernelError.InvalidState => syscall_err_invalid,
                kernel.KernelError.CapabilityNotFound => syscall_err_send,
                kernel.KernelError.TableFull => syscall_err_alloc,
                else => syscall_err_send,
            };
            return received;
        },
        syscall_publish_service_endpoint => {
            if (!state.isBootstrapOwner(proc)) return syscall_err_invalid;
            const target = h.principal_from_process_slot(frame.rsi) orelse return syscall_err_invalid;
            state.publishServiceEndpoint(frame.rdi, target) catch |err| switch (err) {
                kernel.KernelError.InvalidState => return syscall_err_invalid,
                kernel.KernelError.TableFull => return syscall_err_alloc,
                else => return syscall_err_endpoint,
            };
            return syscall_ok;
        },
        syscall_set_abi_trap_delegate => {
            state.setAbiTrapDelegate(proc, frame.rdi, @truncate(frame.rsi), frame.rdx) catch |err| switch (err) {
                kernel.KernelError.EndpointNotFound => return syscall_err_endpoint,
                else => return syscall_err_invalid,
            };
            return syscall_ok;
        },
        syscall_clear_abi_trap_delegate => {
            state.clearAbiTrapDelegate(proc) catch return syscall_err_invalid;
            return syscall_ok;
        },
        syscall_map_abi_trap_reply_target_pages => {
            return mapPagesToCurrentIpcReplyTarget(h, frame.rdi, frame.rsi, frame.rdx);
        },
        syscall_copy_from_abi_trap_reply_target => {
            return copyFromCurrentIpcReplyTarget(h, proc, frame.rdi, frame.rsi, frame.rdx);
        },
        syscall_copy_to_abi_trap_reply_target => {
            return copyToCurrentIpcReplyTarget(h, proc, frame.rdi, frame.rsi, frame.rdx);
        },
        image_abi.syscall_install_vm_object => {
            var page_paddrs: [kernel.max_image_backing_pages]u64 = undefined;
            const collected = collectMappedPagesForRange(state, proc, frame.rdi, frame.rsi, &page_paddrs) orelse return syscall_err_invalid;
            const cap_id = state.installVmObjectCap(
                proc,
                page_paddrs[0..collected.page_count],
                collected.page_offset_bytes,
                frame.rsi,
                parseVmObjectRights(frame.rdx),
            ) catch return syscall_err_grant;
            return image_abi.encodeVmObjectToken(cap_id);
        },
        image_abi.syscall_install_vm_object_mmio_range => {
            if (!state.isBootstrapOwner(proc)) return syscall_err_invalid;
            const base_paddr = frame.rdi;
            const size_bytes = frame.rsi;
            if (size_bytes == 0 or (base_paddr & 0xFFF) != 0) return syscall_err_invalid;
            const span_bytes = (size_bytes + 4095) & ~@as(u64, 4095);
            const page_count_u64 = span_bytes / 4096;
            if (page_count_u64 == 0 or page_count_u64 > kernel.max_image_backing_pages) return syscall_err_invalid;
            var page_paddrs: [kernel.max_image_backing_pages]u64 = undefined;
            var i: usize = 0;
            while (i < page_count_u64) : (i += 1) {
                page_paddrs[i] = base_paddr + (@as(u64, @intCast(i)) * 4096);
            }
            const cap_id = state.installVmObjectCap(
                proc,
                page_paddrs[0..@intCast(page_count_u64)],
                0,
                size_bytes,
                parseVmObjectRights(frame.rdx),
            ) catch |err| switch (err) {
                kernel.KernelError.InvalidState => return syscall_err_invalid,
                else => return syscall_err_grant,
            };
            return image_abi.encodeVmObjectToken(cap_id);
        },
        image_abi.syscall_grant_vm_object => {
            const cap_id = image_abi.decodeVmObjectToken(frame.rdi) orelse return syscall_err_invalid;
            const to = h.principal_from_process_slot(frame.rsi) orelse return syscall_err_invalid;
            const child_id = state.grantVmObjectCap(proc, to, cap_id, parseVmObjectRights(frame.rdx)) catch return syscall_err_grant;
            return image_abi.encodeVmObjectToken(child_id);
        },
        image_abi.syscall_slice_vm_object => {
            const cap_id = image_abi.decodeVmObjectToken(frame.rdi) orelse return syscall_err_invalid;
            const child_id = state.deriveVmObjectCap(proc, cap_id, frame.rsi, frame.rdx, parseVmObjectRights(frame.rcx)) catch return syscall_err_grant;
            return image_abi.encodeVmObjectToken(child_id);
        },
        image_abi.syscall_map_vm_object => {
            const cap_id = image_abi.decodeVmObjectToken(frame.rdi) orelse {
                h.write("map_vm_object invalid token token=");
                h.print_hex(frame.rdi);
                h.write("\n");
                return syscall_err_invalid;
            };
            const target_va = frame.rsi;
            if ((target_va & 0xFFF) != 0) {
                h.write("map_vm_object target va unaligned va=");
                h.print_hex(target_va);
                h.write("\n");
                return syscall_err_invalid;
            }
            const vm_cap = state.getVmObjectTableConst(proc).findByCapId(cap_id) orelse {
                h.write("map_vm_object cap missing proc=");
                h.write(h.principal_label(proc));
                h.write(" cap=");
                h.print_hex(cap_id);
                h.write("\n");
                return syscall_err_invalid;
            };
            if (!vm_cap.rights.read or !vm_cap.rights.map) {
                h.write("map_vm_object rights invalid cap=");
                h.print_hex(cap_id);
                h.write(" rights=");
                h.print_hex(@as(u64, @as(u32, @bitCast(vm_cap.rights))));
                h.write("\n");
                return syscall_err_invalid;
            }
            if (vm_cap.backing.page_offset_bytes != 0) {
                h.write("map_vm_object page_offset invalid cap=");
                h.print_hex(cap_id);
                h.write(" page_offset=");
                h.print_hex(vm_cap.backing.page_offset_bytes);
                h.write(" size=");
                h.print_hex(vm_cap.backing.size_bytes);
                h.write("\n");
                return syscall_err_invalid;
            }
            var i: usize = 0;
            while (i < vm_cap.backing.page_count) {
                const run_start = i;
                const run_paddr = vm_cap.backing.page_paddrs[run_start];
                var run_len: usize = 1;
                while (run_start + run_len < vm_cap.backing.page_count) : (run_len += 1) {
                    const expected = run_paddr + @as(u64, @intCast(run_len)) * 4096;
                    if (vm_cap.backing.page_paddrs[run_start + run_len] != expected) break;
                }
                if (!user_vm.mapUserLinearRegion(
                    proc,
                    target_va + @as(u64, @intCast(run_start)) * 4096,
                    run_paddr,
                    @as(u64, @intCast(run_len)) * 4096,
                    vm_cap.rights.write,
                )) {
                    h.write("map_vm_object user map failed proc=");
                    h.write(h.principal_label(proc));
                    h.write(" va=");
                    h.print_hex(target_va + @as(u64, @intCast(run_start)) * 4096);
                    h.write(" paddr=");
                    h.print_hex(run_paddr);
                    h.write(" bytes=");
                    h.print_hex(@as(u64, @intCast(run_len)) * 4096);
                    h.write("\n");
                    return syscall_err_map;
                }
                i = run_start + run_len;
            }
            return syscall_ok;
        },
        image_abi.syscall_install_exec_image => {
            const vm_cap_id = image_abi.decodeVmObjectToken(frame.rdi) orelse return syscall_err_invalid;
            const cap_id = state.installExecImageCap(proc, vm_cap_id, parseExecImageRights(frame.rsi)) catch return syscall_err_grant;
            return image_abi.encodeExecImageToken(cap_id);
        },
        image_abi.syscall_grant_exec_image => {
            const cap_id = image_abi.decodeExecImageToken(frame.rdi) orelse return syscall_err_invalid;
            const to = h.principal_from_process_slot(frame.rsi) orelse return syscall_err_invalid;
            const child_id = state.grantExecImageCap(proc, to, cap_id, parseExecImageRights(frame.rdx)) catch return syscall_err_grant;
            return image_abi.encodeExecImageToken(child_id);
        },
        queue_abi.syscall_grant_cap => {
            const to = h.principal_from_process_slot(frame.rsi) orelse return syscall_err_invalid;
            const decoded = queue_abi.decodeCapToken(frame.rdi) orelse return syscall_err_invalid;
            switch (decoded.kind) {
                .iommu => {
                    const child_token = device_capabilities.grantIommuCapStage2(state, proc, to, decoded.token) catch return syscall_err_grant;
                    return queue_abi.encodeIommuCapToken(child_token);
                },
                .virtqueue => {
                    const child_token = device_capabilities.grantQueueCapStage2(state, proc, to, decoded.token) catch return syscall_err_grant;
                    return queue_abi.encodeVirtqueueCapToken(child_token);
                },
                .command => {
                    const child_token = device_capabilities.grantCommandCapStage2(state, proc, to, decoded.token) catch return syscall_err_grant;
                    return queue_abi.encodeCommandCapToken(child_token);
                },
            }
        },
        syscall_wait_event => {
            const wait_mailbox = (frame.rdi & 0x1) != 0;
            const timeout_ticks = frame.rsi;
            if (wait_mailbox) {
                const received = state.recvCap(proc) catch |err| switch (err) {
                    kernel.KernelError.MailboxEmpty => 0,
                    else => return syscall_err_send,
                };
                if (received >= cap_transfer_abi.transfer_id_min) {
                    return received;
                }
            }
            if (consumeQueuedIpcMessageForPrincipal(proc, frame)) {
                return syscall_ok;
            }
            if (h.consume_pending_signal_for_principal(proc)) {
                return syscall_ok;
            }
            if (!h.block_current_thread_for_event(frame, wait_mailbox, timeout_ticks, syscall_ok)) {
                return syscall_err_not_ready;
            }
            return syscall_ok;
        },
        syscall_revoke_tree => {
            state.revokeCapTree(proc, frame.rdi) catch return syscall_err_revoke;
            state.bumpEndpointGeneration();
            scheduler.invalidateAllIpcFastpathState();
            h.write("revoke_tree by=");
            h.write(h.principal_label(proc));
            h.write(" paddr=");
            h.print_hex(frame.rdi);
            h.write("\n");
            if (h.enable_cap_table_dump_logs) h.dump_all_process_caps(state);
            return syscall_ok;
        },
        syscall_drop_present => {
            if (capability.dropPresentForUserMappedPaddr(state, proc, frame.rdi)) {
                state.bumpEndpointGeneration();
                scheduler.invalidateAllIpcFastpathState();
                return syscall_ok;
            }
            return syscall_err_drop_present;
        },
        syscall_switch_thread => {
            const target_thread: usize = @intCast(frame.rdi);
            if (target_thread >= scheduler.max_thread_slots) {
                h.write("switch_thread invalid target=");
                h.print_hex(frame.rdi);
                h.write("\n");
                return syscall_err_invalid;
            }
            const current_thread = scheduler.current_thread_index;
            if (!scheduler.isThreadReady(target_thread)) {
                h.log_race_switch(current_thread, target_thread, "target_not_ready");
                return syscall_err_not_ready;
            }
            if (!h.switch_to_thread(target_thread, frame, syscall_ok)) {
                h.log_race_switch(current_thread, target_thread, "context_switch_failed");
                return syscall_err_not_ready;
            }
            if (h.enable_switch_thread_syscall_log) {
                h.write("switch_thread ok from=");
                h.write(h.thread_label(current_thread));
                h.write(" to=");
                h.write(h.thread_label(target_thread));
                h.write("\n");
            }
            return syscall_ok;
        },
        syscall_launch_pie_user => return h.launch_pie_user_thread(frame),
        process_abi.syscall_spawn_exec => return h.spawn_exec(frame),
        process_builder_abi.syscall_create_suspended_process => {
            return process_builder.createSuspendedProcess(proc);
        },
        process_builder_abi.syscall_map_vm_object_to_process => {
            return process_builder.mapVmObjectToProcess(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx);
        },
        process_builder_abi.syscall_alloc_map_pages_to_process => {
            return process_builder.allocMapPagesToProcess(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx, frame.r8);
        },
        process_builder_abi.syscall_set_process_initial_context => {
            return process_builder.setInitialContext(proc, frame.rdi, frame.rsi, frame.rdx);
        },
        process_builder_abi.syscall_start_process => {
            return process_builder.startProcess(proc, frame.rdi);
        },
        process_builder_abi.syscall_abort_process => {
            return process_builder.abortProcess(proc, frame.rdi);
        },
        process_builder_abi.syscall_copy_to_process => {
            return process_builder.copyToProcess(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx);
        },
        process_builder_abi.syscall_mprotect_self => {
            return process_builder.mprotectSelf(proc, frame.rdi, frame.rsi, frame.rdx);
        },
        process_builder_abi.syscall_set_process_abi_trap_delegate => {
            return process_builder.setAbiTrapDelegate(proc, frame.rdi, frame.rsi, frame.rdx, frame.rcx, frame.r8);
        },
        syscall_log => {
            const req_len_u64 = frame.rsi;
            if (req_len_u64 == 0) return syscall_ok;
            if (req_len_u64 > user_log_max_bytes) return syscall_err_invalid;
            const req_len: usize = @intCast(req_len_u64);
            var buf: [user_log_max_bytes]u8 = undefined;
            const msg = buf[0..req_len];
            if (!h.copy_user_bytes_from_va(proc, frame.rdi, msg)) return syscall_err_invalid;
            if (!hasExplicitUserLogLabel(msg)) {
                writeThreadUserLogPrefix(h, scheduler.current_thread_index);
            }
            h.write(msg);
            return syscall_ok;
        },
        else => return syscall_err_invalid,
    }
}

test "explicit userlog label detection" {
    try std.testing.expect(hasExplicitUserLogLabel("[seed] ready\n"));
    try std.testing.expect(hasExplicitUserLogLabel("[seed]"));
    try std.testing.expect(!hasExplicitUserLogLabel("seed ready\n"));
    try std.testing.expect(!hasExplicitUserLogLabel("[seed"));
    try std.testing.expect(!hasExplicitUserLogLabel("[] bad\n"));
}
