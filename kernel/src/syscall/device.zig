const std = @import("std");
const abi_root = @import("kernel_abi_root");
const device_capabilities = @import("../device_capabilities.zig");
const device_events = @import("../device_events.zig");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const queue_abi = abi_root.queue_abi;
const sc = @import("numbers.zig");

const TrapFrame = interrupts.TrapFrame;

pub fn dispatch(
    h: anytype,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    frame: *TrapFrame,
) ?u64 {
    return switch (frame.rax) {
        sc.syscall_queue_submit, sc.syscall_queue_notify => blk: {
            const queue_token = frame.rdi;
            const queue_index: u16 = @truncate(frame.rsi);
            const op: device_capabilities.QueueOperation = if (frame.rax == sc.syscall_queue_submit) .submit else .notify;
            device_capabilities.queueCapAuthorizeStage2(state, proc, queue_token, queue_index, op) catch {
                break :blk sc.syscall_err_invalid;
            };
            if (device_capabilities.queueCapDeviceForToken(state, proc, queue_token, queue_index)) |device| {
                _ = device_events.bindDeviceEvent(proc, device);
            }
            break :blk sc.syscall_ok;
        },
        sc.syscall_iommu_authorize => {
            const device: kernel.DmaDeviceId = frame.rsi;
            const op: device_capabilities.IommuOperation = std.meta.intToEnum(device_capabilities.IommuOperation, @as(u8, @truncate(frame.rdx))) catch return sc.syscall_err_invalid;
            device_capabilities.iommuCapAuthorizeStage2(state, proc, frame.rdi, device, op) catch return sc.syscall_err_invalid;
            return sc.syscall_ok;
        },
        sc.syscall_command_authorize => {
            const device: kernel.DmaDeviceId = frame.rsi;
            const opcode: device_capabilities.CommandOpcodeClass = std.meta.intToEnum(device_capabilities.CommandOpcodeClass, @as(u8, @truncate(frame.rdx))) catch return sc.syscall_err_invalid;
            device_capabilities.commandCapAuthorizeStage2(state, proc, frame.rdi, device, opcode) catch return sc.syscall_err_invalid;
            return sc.syscall_ok;
        },
        sc.syscall_dma_map_create => {
            const device: kernel.DmaDeviceId = frame.rdi;
            const direction: kernel.DmaDirection = std.meta.intToEnum(kernel.DmaDirection, @as(u8, @truncate(frame.r8))) catch return sc.syscall_err_invalid;
            const token = state.dmaMapCreateStage1(proc, device, frame.rsi, frame.rdx, direction) catch |err| switch (err) {
                kernel.KernelError.InvalidState => return sc.syscall_err_invalid,
                kernel.KernelError.TableFull => return sc.syscall_err_alloc,
                else => return sc.syscall_err_invalid,
            };
            return queue_abi.encodeDmaMappingToken(token);
        },
        sc.syscall_dma_map_set_state => {
            const mapping = state.dmaMapFindStage1(frame.rdi) orelse return sc.syscall_err_invalid;
            if (mapping.owner_principal_raw != @intFromEnum(proc)) return sc.syscall_err_invalid;
            const next_state: kernel.DmaMappingState = std.meta.intToEnum(kernel.DmaMappingState, @as(u8, @truncate(frame.rsi))) catch return sc.syscall_err_invalid;
            state.dmaMapSetStateStage1(frame.rdi, next_state) catch |err| switch (err) {
                kernel.KernelError.CapabilityNotFound => return sc.syscall_err_invalid,
                kernel.KernelError.InvalidState => return sc.syscall_err_invalid,
                else => return sc.syscall_err_invalid,
            };
            return sc.syscall_ok;
        },
        sc.syscall_dma_map_release => {
            const mapping = state.dmaMapFindStage1(frame.rdi) orelse return sc.syscall_err_invalid;
            if (mapping.owner_principal_raw != @intFromEnum(proc)) return sc.syscall_err_invalid;
            state.dmaMapReleaseStage1(frame.rdi) catch |err| switch (err) {
                kernel.KernelError.CapabilityNotFound => return sc.syscall_err_invalid,
                kernel.KernelError.InvalidState => return sc.syscall_err_invalid,
                else => return sc.syscall_err_invalid,
            };
            return sc.syscall_ok;
        },
        sc.syscall_revoke_device_cap => {
            const decoded = queue_abi.decodeCapToken(frame.rdi) orelse return sc.syscall_err_invalid;
            device_capabilities.revokeDeviceCapStage2(state, proc, decoded.kind, decoded.token) catch |err| switch (err) {
                kernel.KernelError.CapabilityNotFound => return sc.syscall_err_invalid,
                kernel.KernelError.InvalidState => return sc.syscall_err_invalid,
                else => return sc.syscall_err_revoke,
            };
            return sc.syscall_ok;
        },
        sc.syscall_derive_command_cap => {
            const token = queue_abi.decodeCommandCapToken(frame.rdi) orelse return sc.syscall_err_invalid;
            const child_token = device_capabilities.deriveCommandCapStage2(state, proc, token, frame.rsi) catch |err| switch (err) {
                kernel.KernelError.CapabilityNotFound => return sc.syscall_err_invalid,
                kernel.KernelError.InvalidState => return sc.syscall_err_invalid,
                kernel.KernelError.TableFull => return sc.syscall_err_alloc,
                else => return sc.syscall_err_grant,
            };
            return queue_abi.encodeCommandCapToken(child_token);
        },
        queue_abi.syscall_grant_cap => {
            const to = h.principal_from_process_slot(frame.rsi) orelse return sc.syscall_err_invalid;
            const decoded = queue_abi.decodeCapToken(frame.rdi) orelse return sc.syscall_err_invalid;
            switch (decoded.kind) {
                .iommu => {
                    const child_token = device_capabilities.grantIommuCapStage2(state, proc, to, decoded.token) catch return sc.syscall_err_grant;
                    return queue_abi.encodeIommuCapToken(child_token);
                },
                .virtqueue => {
                    const child_token = device_capabilities.grantQueueCapStage2(state, proc, to, decoded.token) catch return sc.syscall_err_grant;
                    return queue_abi.encodeVirtqueueCapToken(child_token);
                },
                .command => {
                    const child_token = device_capabilities.grantCommandCapStage2(state, proc, to, decoded.token) catch return sc.syscall_err_grant;
                    return queue_abi.encodeCommandCapToken(child_token);
                },
            }
        },
        else => null,
    };
}
