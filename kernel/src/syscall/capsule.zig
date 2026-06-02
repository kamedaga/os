const std = @import("std");
const abi_root = @import("kernel_abi_root");
const capsule_abi = abi_root.capsule_abi;
const capability = @import("../capability.zig");
const device_events = @import("../device_events.zig");
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const pci = @import("../pci.zig");
const user_vm = @import("../memory/user_vm.zig");
const sc = @import("numbers.zig");

const TrapFrame = interrupts.TrapFrame;
const pci_config_size: usize = 256;
const page_size: u64 = 4096;

fn kernelCapsuleKindFromAbi(kind: capsule_abi.CapsuleKind) kernel.CapsuleKind {
    return switch (kind) {
        .session => .session,
        .device => .device,
        .mmio => .mmio,
        .dma_buffer => .dma_buffer,
        .dma_mapping => .dma_mapping,
        .irq => .irq,
        .event_queue => .event_queue,
    };
}

fn abiCapsuleKindFromKernel(kind: kernel.CapsuleKind) ?capsule_abi.CapsuleKind {
    return switch (kind) {
        .session => .session,
        .device => .device,
        .mmio => .mmio,
        .dma_buffer => .dma_buffer,
        .dma_mapping => .dma_mapping,
        .irq => .irq,
        .event_queue => .event_queue,
        .none => null,
    };
}

fn abiCapsuleStateFromKernel(state: kernel.CapsuleState) ?capsule_abi.CapsuleState {
    return switch (state) {
        .active => .active,
        .revoked => .revoked,
        .empty => null,
    };
}

fn decodeCapsuleToken(value: u64, expected_kind: ?capsule_abi.CapsuleKind) ?capsule_abi.DecodedCapsuleToken {
    const decoded = capsule_abi.decodeCapsuleToken(value) orelse return null;
    if (expected_kind) |kind| {
        if (decoded.kind != kind) return null;
    }
    return decoded;
}

fn u32Arg(value: u64) ?u32 {
    if (value > @as(u64, std.math.maxInt(u32))) return null;
    return @intCast(value);
}

fn flagsArg(value: u64, known_mask: u64) ?u32 {
    if ((value & ~known_mask) != 0) return null;
    return u32Arg(value);
}

fn pageAlignDown(value: u64) u64 {
    return value & ~(page_size - 1);
}

fn pageAlignUp(value: u64) ?u64 {
    const aligned, const overflow = @addWithOverflow(value, page_size - 1);
    if (overflow != 0) return null;
    return pageAlignDown(aligned);
}

fn parseDmaDirection(value: u64) ?kernel.CapsuleDmaDirection {
    if (value > @as(u64, std.math.maxInt(u2))) return null;
    return std.meta.intToEnum(kernel.CapsuleDmaDirection, @as(u2, @intCast(value))) catch null;
}

fn parseIrqKind(value: u64) ?kernel.CapsuleIrqKind {
    if (value > @as(u64, std.math.maxInt(u2))) return null;
    return std.meta.intToEnum(kernel.CapsuleIrqKind, @as(u2, @intCast(value))) catch null;
}

fn encodeCapsuleTokenForSnapshot(snapshot: kernel.CapsuleSnapshot) ?u64 {
    const kind = abiCapsuleKindFromKernel(snapshot.kind) orelse return null;
    return capsule_abi.encodeCapsuleToken(kind, snapshot.token);
}

fn encodeCapsuleLineageToken(state: *const kernel.KernelState, raw_token: u64) u64 {
    if (raw_token == 0) return 0;
    const snapshot = state.capsuleSnapshot(raw_token) catch return 0;
    return encodeCapsuleTokenForSnapshot(snapshot) orelse 0;
}

fn capsuleAccessStatus(err: kernel.KernelError) u64 {
    return switch (err) {
        kernel.KernelError.TableFull, kernel.KernelError.OutOfFreePages => sc.syscall_err_alloc,
        kernel.KernelError.CapsuleRevoked => sc.syscall_err_revoke,
        else => sc.syscall_err_invalid,
    };
}

fn capsuleGrantStatus(err: kernel.KernelError) u64 {
    return switch (err) {
        kernel.KernelError.TableFull, kernel.KernelError.OutOfFreePages => sc.syscall_err_alloc,
        kernel.KernelError.CapsuleRevoked => sc.syscall_err_revoke,
        else => sc.syscall_err_grant,
    };
}

fn pciDeviceSnapshot(
    state: *const kernel.KernelState,
    proc: kernel.PrincipalId,
    token: u64,
    required_rights: kernel.CapsuleRights,
) kernel.KernelError!kernel.CapsuleSnapshot {
    var rights = required_rights;
    rights.query = true;
    try state.capsuleAuthorize(proc, token, .device, rights);
    const snapshot = try state.capsuleSnapshot(token);
    _ = pci.locationFromResourceId(snapshot.metadata.device) orelse return kernel.KernelError.InvalidState;
    return snapshot;
}

fn validatePciConfigRange(offset: u32, len: u32) bool {
    const config_size: u32 = @intCast(pci_config_size);
    if (len == 0 or offset >= config_size) return false;
    const end, const overflow = @addWithOverflow(offset, len);
    return overflow == 0 and end <= config_size;
}

fn writePciBarInfo(h: anytype, proc: kernel.PrincipalId, out_va: u64, max_words: u64, info: pci.BarInfo) bool {
    if (max_words < @as(u64, @intCast(capsule_abi.bar_info_word_count))) return false;
    const end = info.end() orelse return false;
    var words: [capsule_abi.bar_info_word_count]u64 = undefined;
    words[capsule_abi.bar_info_start_index] = info.start;
    words[capsule_abi.bar_info_end_index] = end;
    words[capsule_abi.bar_info_size_index] = info.size;
    words[capsule_abi.bar_info_flags_index] = info.flags;
    for (words, 0..) |word, index| {
        const offset = @as(u64, @intCast(index)) * @sizeOf(u64);
        if (!h.write_user_u64(proc, out_va + offset, word)) return false;
    }
    return true;
}

fn mapPciBarIntoUser(proc: kernel.PrincipalId, user_va: u64, map_size: u64, flags: u64, info: pci.BarInfo) bool {
    if ((user_va & (page_size - 1)) != 0 or (map_size & (page_size - 1)) != 0) return false;
    if (map_size == 0 or map_size > @as(u64, std.math.maxInt(usize))) return false;
    const paddr = pageAlignDown(info.start);
    const required_size = pageAlignUp((info.start - paddr) + info.size) orelse return false;
    if (map_size < required_size) return false;
    const size_usize: usize = @intCast(map_size);
    if (user_vm.mapUserLinearRegion(proc, user_va, paddr, size_usize, true)) return true;
    if ((flags & capsule_abi.mmio_map_flag_replace_existing) == 0) return false;
    _ = user_vm.unmapUserLinearRegion(proc, user_va, size_usize);
    return user_vm.mapUserLinearRegion(proc, user_va, paddr, size_usize, true);
}

fn userDmaAddressForRange(proc: kernel.PrincipalId, user_va: u64, size: u64) ?u64 {
    if (user_va == 0 or size == 0) return null;
    const first_page_va = pageAlignDown(user_va);
    const offset = user_va - first_page_va;
    const span, const span_overflow = @addWithOverflow(offset, size);
    if (span_overflow != 0) return null;
    const page_span = pageAlignUp(span) orelse return null;
    const page_count = page_span / page_size;
    if (page_count == 0) return null;

    const first_paddr = capability.lookupUserMappedPaddrForVa(proc, first_page_va) orelse return null;
    if ((first_paddr & (page_size - 1)) != 0) return null;
    var page_index: u64 = 1;
    while (page_index < page_count) : (page_index += 1) {
        const va = first_page_va + page_index * page_size;
        const paddr = capability.lookupUserMappedPaddrForVa(proc, va) orelse return null;
        if (paddr != first_paddr + page_index * page_size) return null;
    }
    return first_paddr + offset;
}

fn cleanupMmioCapsuleMapping(state: *const kernel.KernelState, proc: kernel.PrincipalId, token: u64) void {
    const snapshot = state.capsuleSnapshot(token) catch return;
    if (snapshot.kind != .mmio or snapshot.metadata.user_va == 0 or snapshot.metadata.size == 0) return;
    if (snapshot.metadata.size > @as(u64, std.math.maxInt(usize))) return;
    _ = user_vm.unmapUserLinearRegion(proc, snapshot.metadata.user_va, @intCast(snapshot.metadata.size));
}

fn writeCapsuleSnapshot(
    h: anytype,
    state: *const kernel.KernelState,
    proc: kernel.PrincipalId,
    snapshot: kernel.CapsuleSnapshot,
    out_va: u64,
    max_words: u64,
) bool {
    if (max_words < @as(u64, @intCast(capsule_abi.snapshot_word_count))) return false;
    const kind = abiCapsuleKindFromKernel(snapshot.kind) orelse return false;
    const capsule_state = abiCapsuleStateFromKernel(snapshot.state) orelse return false;
    var words: [capsule_abi.snapshot_word_count]u64 = undefined;
    words[capsule_abi.snapshot_token_index] = capsule_abi.encodeCapsuleToken(kind, snapshot.token);
    words[capsule_abi.snapshot_root_token_index] = encodeCapsuleLineageToken(state, snapshot.root_token);
    words[capsule_abi.snapshot_parent_token_index] = encodeCapsuleLineageToken(state, snapshot.parent_token);
    words[capsule_abi.snapshot_kind_index] = @as(u64, @intFromEnum(kind));
    words[capsule_abi.snapshot_state_index] = @as(u64, @intFromEnum(capsule_state));
    words[capsule_abi.snapshot_rights_index] = kernel.capsule.rightsToBits(snapshot.rights);
    words[capsule_abi.snapshot_owner_index] = @as(u64, snapshot.owner_principal_raw);
    words[capsule_abi.snapshot_generation_index] = snapshot.generation;
    words[capsule_abi.snapshot_revoke_generation_index] = snapshot.revoke_generation;
    words[capsule_abi.snapshot_device_index] = snapshot.metadata.device;
    words[capsule_abi.snapshot_object_id_index] = snapshot.metadata.object_id;
    words[capsule_abi.snapshot_user_va_index] = snapshot.metadata.user_va;
    words[capsule_abi.snapshot_iova_index] = snapshot.metadata.iova;
    words[capsule_abi.snapshot_size_index] = snapshot.metadata.size;
    words[capsule_abi.snapshot_index_index] = @as(u64, snapshot.metadata.index);
    words[capsule_abi.snapshot_flags_index] = @as(u64, snapshot.metadata.flags);

    for (words, 0..) |word, index| {
        const offset = @as(u64, @intCast(index)) * @sizeOf(u64);
        if (!h.write_user_u64(proc, out_va + offset, word)) return false;
    }
    return true;
}

pub fn dispatch(
    h: anytype,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    frame: *TrapFrame,
) ?u64 {
    return switch (frame.rax) {
        sc.syscall_capsule_query => {
            const decoded = decodeCapsuleToken(frame.rdi, null) orelse return sc.syscall_err_invalid;
            const kind = kernelCapsuleKindFromAbi(decoded.kind);
            state.capsuleAuthorize(proc, decoded.token, kind, .{ .query = true }) catch |err| return capsuleAccessStatus(err);
            const snapshot = state.capsuleSnapshot(decoded.token) catch |err| return capsuleAccessStatus(err);
            if (!writeCapsuleSnapshot(h, state, proc, snapshot, frame.rsi, frame.rdx)) return sc.syscall_err_invalid;
            return @as(u64, @intCast(capsule_abi.snapshot_word_count));
        },
        sc.syscall_capsule_derive_mmio => {
            const decoded = decodeCapsuleToken(frame.rdi, .device) orelse return sc.syscall_err_invalid;
            const bar_index = u32Arg(frame.rsi) orelse return sc.syscall_err_invalid;
            const flags = flagsArg(frame.r8, capsule_abi.mmio_map_known_flags_mask) orelse return sc.syscall_err_invalid;
            if (bar_index >= capsule_abi.pci_bar_count) return sc.syscall_err_invalid;
            const device = pciDeviceSnapshot(state, proc, decoded.token, .{ .bar_map = true }) catch |err| return capsuleAccessStatus(err);
            const loc = pci.locationFromResourceId(device.metadata.device) orelse return sc.syscall_err_invalid;
            const bar = pci.probeBarInfo(loc, @intCast(bar_index)) orelse return sc.syscall_err_invalid;
            if ((bar.flags & pci.bar_flag_mem) == 0) return sc.syscall_err_invalid;
            if (!mapPciBarIntoUser(proc, frame.rdx, frame.rcx, flags, bar)) return sc.syscall_err_map;
            const child = state.deviceCapsuleDeriveMmio(proc, decoded.token, bar_index, pageAlignDown(bar.start), frame.rdx, frame.rcx, flags) catch |err| return capsuleAccessStatus(err);
            return capsule_abi.encodeCapsuleToken(.mmio, child);
        },
        sc.syscall_capsule_derive_dma_buffer => {
            const decoded = decodeCapsuleToken(frame.rdi, .device) orelse return sc.syscall_err_invalid;
            const flags = flagsArg(frame.r8, capsule_abi.dma_buffer_known_flags_mask) orelse return sc.syscall_err_invalid;
            const dma_addr = userDmaAddressForRange(proc, frame.rsi, frame.rcx) orelse return sc.syscall_err_invalid;
            const child = state.deviceCapsuleDeriveDmaBuffer(proc, decoded.token, frame.rsi, dma_addr, frame.rcx, flags) catch |err| return capsuleAccessStatus(err);
            return capsule_abi.encodeCapsuleToken(.dma_buffer, child);
        },
        sc.syscall_capsule_derive_dma_mapping => {
            const decoded = decodeCapsuleToken(frame.rdi, .device) orelse return sc.syscall_err_invalid;
            const direction = parseDmaDirection(frame.r8) orelse return sc.syscall_err_invalid;
            const flags = flagsArg(frame.r9, capsule_abi.dma_mapping_known_flags_mask) orelse return sc.syscall_err_invalid;
            const dma_addr = userDmaAddressForRange(proc, frame.rsi, frame.rcx) orelse return sc.syscall_err_invalid;
            const child = state.deviceCapsuleDeriveDmaMapping(proc, decoded.token, frame.rsi, dma_addr, frame.rcx, direction, flags) catch |err| return capsuleAccessStatus(err);
            return capsule_abi.encodeCapsuleToken(.dma_mapping, child);
        },
        sc.syscall_capsule_derive_dma_mapping_from_buffer => {
            const decoded = decodeCapsuleToken(frame.rdi, .dma_buffer) orelse return sc.syscall_err_invalid;
            const direction = parseDmaDirection(frame.rcx) orelse return sc.syscall_err_invalid;
            const flags = flagsArg(frame.r8, capsule_abi.dma_mapping_known_flags_mask) orelse return sc.syscall_err_invalid;
            const child = state.dmaBufferCapsuleDeriveMapping(proc, decoded.token, frame.rsi, frame.rdx, direction, flags) catch |err| return capsuleAccessStatus(err);
            return capsule_abi.encodeCapsuleToken(.dma_mapping, child);
        },
        sc.syscall_capsule_derive_irq => {
            const decoded = decodeCapsuleToken(frame.rdi, .device) orelse return sc.syscall_err_invalid;
            const kind = parseIrqKind(frame.rsi) orelse return sc.syscall_err_invalid;
            const vector = u32Arg(frame.rdx) orelse return sc.syscall_err_invalid;
            const flags = flagsArg(frame.rcx, capsule_abi.irq_known_flags_mask) orelse return sc.syscall_err_invalid;
            const device = pciDeviceSnapshot(state, proc, decoded.token, .{ .irq_bind = true }) catch |err| return capsuleAccessStatus(err);
            if (!device_events.bindDeviceEvent(proc, device.metadata.device)) return sc.syscall_err_alloc;
            const child = state.deviceCapsuleDeriveIrq(proc, decoded.token, kind, vector, flags) catch |err| return capsuleAccessStatus(err);
            return capsule_abi.encodeCapsuleToken(.irq, child);
        },
        sc.syscall_capsule_irq_poll => {
            const decoded = decodeCapsuleToken(frame.rdi, .irq) orelse return sc.syscall_err_invalid;
            const max_words = frame.rcx;
            const flags = u32Arg(frame.r8) orelse return sc.syscall_err_invalid;
            if (flags != 0 or max_words < 1) return sc.syscall_err_invalid;
            state.capsuleAuthorize(proc, decoded.token, .irq, .{ .irq_bind = true }) catch |err| return capsuleAccessStatus(err);
            const irq = state.capsuleSnapshot(decoded.token) catch |err| return capsuleAccessStatus(err);
            if (!device_events.bindDeviceEvent(proc, irq.metadata.device)) return sc.syscall_err_alloc;
            const count = device_events.interruptCountFor(proc, irq.metadata.device) orelse return sc.syscall_err_not_ready;
            if (count == frame.rsi) return sc.syscall_err_not_ready;
            if (!h.write_user_u64(proc, frame.rdx, count)) return sc.syscall_err_invalid;
            return 1;
        },
        sc.syscall_capsule_grant => {
            const decoded = decodeCapsuleToken(frame.rdi, null) orelse return sc.syscall_err_invalid;
            const to = h.principal_from_process_slot(frame.rsi) orelse return sc.syscall_err_invalid;
            const rights = kernel.capsule.rightsFromBits(frame.rdx);
            if (kernel.capsule.rightsToBits(rights) == 0) return sc.syscall_err_invalid;
            state.capsuleAuthorize(proc, decoded.token, kernelCapsuleKindFromAbi(decoded.kind), .{}) catch |err| return capsuleGrantStatus(err);
            const child = state.capsuleGrant(proc, to, decoded.token, rights) catch |err| return capsuleGrantStatus(err);
            return capsule_abi.encodeCapsuleToken(decoded.kind, child);
        },
        sc.syscall_capsule_revoke => {
            const decoded = decodeCapsuleToken(frame.rdi, null) orelse return sc.syscall_err_invalid;
            state.capsuleAuthorize(proc, decoded.token, kernelCapsuleKindFromAbi(decoded.kind), .{}) catch |err| return capsuleAccessStatus(err);
            if (decoded.kind == .mmio) cleanupMmioCapsuleMapping(state, proc, decoded.token);
            _ = state.capsuleRevokeSubtree(proc, decoded.token) catch |err| return capsuleAccessStatus(err);
            return sc.syscall_ok;
        },
        sc.syscall_capsule_close => {
            const decoded = decodeCapsuleToken(frame.rdi, null) orelse return sc.syscall_err_invalid;
            state.capsuleAuthorize(proc, decoded.token, kernelCapsuleKindFromAbi(decoded.kind), .{}) catch |err| return capsuleAccessStatus(err);
            if (decoded.kind == .mmio) cleanupMmioCapsuleMapping(state, proc, decoded.token);
            _ = state.capsuleCloseSubtree(proc, decoded.token) catch |err| return capsuleAccessStatus(err);
            return sc.syscall_ok;
        },
        sc.syscall_capsule_pci_config_read => {
            const decoded = decodeCapsuleToken(frame.rdi, .device) orelse return sc.syscall_err_invalid;
            const offset = u32Arg(frame.rsi) orelse return sc.syscall_err_invalid;
            const len = u32Arg(frame.rcx) orelse return sc.syscall_err_invalid;
            if (!validatePciConfigRange(offset, len)) return sc.syscall_err_invalid;
            const device = pciDeviceSnapshot(state, proc, decoded.token, .{ .config_read = true }) catch |err| return capsuleAccessStatus(err);
            const loc = pci.locationFromResourceId(device.metadata.device) orelse return sc.syscall_err_invalid;
            var bytes: [pci_config_size]u8 = undefined;
            const len_usize: usize = @intCast(len);
            var i: usize = 0;
            while (i < len_usize) : (i += 1) {
                const config_offset: u8 = @intCast(offset + @as(u32, @intCast(i)));
                bytes[i] = pci.readConfigU8(loc, config_offset);
            }
            if (!h.copy_bytes_to_user_va(proc, frame.rdx, bytes[0..len_usize])) return sc.syscall_err_invalid;
            return sc.syscall_ok;
        },
        sc.syscall_capsule_pci_config_write => {
            const decoded = decodeCapsuleToken(frame.rdi, .device) orelse return sc.syscall_err_invalid;
            const offset = u32Arg(frame.rsi) orelse return sc.syscall_err_invalid;
            const len = u32Arg(frame.rcx) orelse return sc.syscall_err_invalid;
            if (!validatePciConfigRange(offset, len)) return sc.syscall_err_invalid;
            const device = pciDeviceSnapshot(state, proc, decoded.token, .{ .config_write = true }) catch |err| return capsuleAccessStatus(err);
            const loc = pci.locationFromResourceId(device.metadata.device) orelse return sc.syscall_err_invalid;
            var bytes: [pci_config_size]u8 = undefined;
            const len_usize: usize = @intCast(len);
            if (!h.copy_user_bytes_from_va(proc, frame.rdx, bytes[0..len_usize])) return sc.syscall_err_invalid;
            var i: usize = 0;
            while (i < len_usize) : (i += 1) {
                const config_offset: u8 = @intCast(offset + @as(u32, @intCast(i)));
                pci.writeConfigU8(loc, config_offset, bytes[i]);
            }
            return sc.syscall_ok;
        },
        sc.syscall_capsule_pci_bar_info => {
            const decoded = decodeCapsuleToken(frame.rdi, .device) orelse return sc.syscall_err_invalid;
            const bar_index = u32Arg(frame.rsi) orelse return sc.syscall_err_invalid;
            if (bar_index >= capsule_abi.pci_bar_count) return sc.syscall_err_invalid;
            const device = pciDeviceSnapshot(state, proc, decoded.token, .{ .bar_info = true }) catch |err| return capsuleAccessStatus(err);
            const loc = pci.locationFromResourceId(device.metadata.device) orelse return sc.syscall_err_invalid;
            const info = pci.probeBarInfo(loc, @intCast(bar_index)) orelse return sc.syscall_err_invalid;
            if (!writePciBarInfo(h, proc, frame.rdx, frame.rcx, info)) return sc.syscall_err_invalid;
            return @as(u64, @intCast(capsule_abi.bar_info_word_count));
        },
        else => null,
    };
}
