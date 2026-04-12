const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const abi_root = @import("kernel_abi_root");
const cap_transfer_abi = abi_root.cap_transfer_abi;
const device_abi = abi_root.device_abi;
const image_abi = abi_root.image_abi;
const user_vm = @import("memory/user_vm.zig");
const process_abi = abi_root.process_abi;
const queue_abi = abi_root.queue_abi;
const untyped_memory = @import("untyped_memory.zig");
const interrupts = @import("interrupts.zig");
const scheduler = @import("scheduler.zig");

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
const syscall_accept_cap_transfer: u64 = cap_transfer_abi.syscall_accept_cap_transfer;

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
    launch_pie_user_thread: *const fn (*TrapFrame) u64,
    spawn_exec: *const fn (*TrapFrame) u64,
    wake_waiting_thread_for_principal: *const fn (kernel.PrincipalId) void,
    wake_blocked_thread_for_principal: *const fn (kernel.PrincipalId) void,
    consume_pending_signal_for_principal: *const fn (kernel.PrincipalId) bool,
    switch_to_thread: *const fn (usize, *TrapFrame, ?u64) bool,
    block_current_thread_for_event: *const fn (*TrapFrame, bool, u64, u64) bool,
    log_queue_cap_deny: *const fn (kernel.PrincipalId, u64, kernel.DmaDeviceId, u16, kernel.QueueOperation, anyerror) void,
    log_race_send_cap: *const fn (kernel.PrincipalId, ?kernel.PrincipalId, u64, u64, []const u8) void,
    log_race_switch: *const fn (usize, usize, []const u8) void,
};

var hooks: ?Hooks = null;
pub export var syscall_return_writeback_enabled: u64 = 1;

pub fn init(new_hooks: Hooks) void {
    hooks = new_hooks;
}

fn getHooks() *const Hooks {
    return &(hooks orelse unreachable);
}

fn parseVmObjectRights(bits: u64) kernel.VmObjectRights {
    const abi_rights = image_abi.vmObjectRightsFromBits(bits);
    return @bitCast(abi_rights);
}

fn parseDmaDeviceId(value: u64) ?kernel.DmaDeviceId {
    const abi_device = std.meta.intToEnum(device_abi.DeviceId, @as(u8, @truncate(value))) catch return null;
    return switch (abi_device) {
        .virtio_gpu => .virtio_gpu,
        .virtio_input => .virtio_input,
        .virtio_blk => .virtio_blk,
    };
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

fn shouldLogEndpointTransfer(endpoint_id: u64) bool {
    return endpoint_id >= 0x80;
}

fn logEndpointTransfer(
    h: *const Hooks,
    phase: []const u8,
    from: kernel.PrincipalId,
    to: kernel.PrincipalId,
    endpoint_id: u64,
    paddr: u64,
) void {
    if (!shouldLogEndpointTransfer(endpoint_id)) return;
    h.write(phase);
    h.write(" from=");
    h.write(h.principal_label(from));
    h.write(" to=");
    h.write(h.principal_label(to));
    h.write(" ep=");
    h.print_hex(endpoint_id);
    h.write(" paddr=");
    h.print_hex(paddr);
    h.write("\n");
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
    logEndpointTransfer(h, if (retain_sender) "share_cap begin" else "send_cap begin", proc, to, endpoint_id, paddr);
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
    logEndpointTransfer(h, if (retain_sender) "share_cap done" else "send_cap done", proc, to, endpoint_id, paddr);
    if (h.enable_cap_table_dump_logs) h.dump_all_process_caps(state);
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
            const device: kernel.DmaDeviceId = switch (frame.rsi) {
                0 => .virtio_gpu,
                1 => .virtio_input,
                2 => .virtio_blk,
                else => return syscall_err_invalid,
            };
            const queue_index: u16 = @truncate(frame.rdx);
            const op: kernel.QueueOperation = if (frame.rax == syscall_queue_submit) .submit else .notify;
            state.queueCapAuthorizeStage2(proc, queue_token, device, queue_index, op) catch |err| {
                h.log_queue_cap_deny(proc, queue_token, device, queue_index, op, err);
                return syscall_err_invalid;
            };
            return syscall_ok;
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
            const to = state.endpointTargetFor(proc, frame.rdi) orelse return syscall_err_endpoint;
            h.wake_blocked_thread_for_principal(to);
            return syscall_ok;
        },
        syscall_get_tick_count => {
            return scheduler.lapic_tick_count;
        },
        syscall_get_process_slot => {
            const slot = kernel.processIndexFromPrincipal(proc) orelse return syscall_err_invalid;
            return @intCast(slot);
        },
        syscall_register_iommu_driver => {
            const device = parseDmaDeviceId(frame.rdi) orelse return syscall_err_invalid;
            state.registerIommuNoCapDriver(proc, device) catch return syscall_err_invalid;
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
                    false,
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
            const token = queue_abi.decodeQueueCapToken(frame.rdi) orelse return syscall_err_invalid;
            const to = h.principal_from_process_slot(frame.rsi) orelse return syscall_err_invalid;
            const child_token = state.grantQueueCapStage2(proc, to, token) catch return syscall_err_grant;
            return queue_abi.encodeQueueCapToken(child_token);
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
            h.write("revoke_tree by=");
            h.write(h.principal_label(proc));
            h.write(" paddr=");
            h.print_hex(frame.rdi);
            h.write("\n");
            if (h.enable_cap_table_dump_logs) h.dump_all_process_caps(state);
            return syscall_ok;
        },
        syscall_drop_present => {
            if (capability.dropPresentForUserMappedPaddr(state, proc, frame.rdi)) return syscall_ok;
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
            const target_ctx = scheduler.getThreadContextConst(target_thread).?;
            if (!target_ctx.ready) {
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
        syscall_log => {
            const req_len_u64 = frame.rsi;
            if (req_len_u64 == 0) return syscall_ok;
            if (req_len_u64 > user_log_max_bytes) return syscall_err_invalid;
            const req_len: usize = @intCast(req_len_u64);
            var buf: [user_log_max_bytes]u8 = undefined;
            const msg = buf[0..req_len];
            if (!h.copy_user_bytes_from_va(proc, frame.rdi, msg)) return syscall_err_invalid;
            h.write("userlog ");
            h.write(h.thread_label(scheduler.current_thread_index));
            h.write(": ");
            h.write(msg);
            return syscall_ok;
        },
        else => return syscall_err_invalid,
    }
}
