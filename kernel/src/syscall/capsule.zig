const std = @import("std");
const abi_root = @import("kernel_abi_root");
const capsule_abi = abi_root.capsule_abi;
const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const pci = @import("../pci.zig");
const user_copy = @import("../user_copy.zig");
const user_vm = @import("../memory/user_vm.zig");
const sc = @import("numbers.zig");
const vtd = @import("../vtd.zig");

const TrapFrame = interrupts.TrapFrame;
const pci_config_size: usize = 256;
const page_size: u64 = 4096;
const first_dynamic_fd: kernel.Fd = abi_root.fd_abi.first_dynamic_fd;
// Capsule syscalls run under the global kernel-state lock, so this bounded
// page-resolution scratch array cannot be used concurrently by two derives.
var dma_resolved_paddrs: [kernel.max_vmo_backing_pages]u64 = undefined;

fn statusFromKernelError(err: kernel.KernelError) u64 {
    return switch (err) {
        kernel.KernelError.TableFull, kernel.KernelError.OutOfFreePages => sc.syscall_err_alloc,
        else => sc.syscall_err_invalid,
    };
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
    return std.enums.fromInt(kernel.CapsuleDmaDirection, @as(u2, @intCast(value)));
}

fn parseIrqKind(value: u64) ?kernel.CapsuleIrqKind {
    if (value > @as(u64, std.math.maxInt(u2))) return null;
    return std.enums.fromInt(kernel.CapsuleIrqKind, @as(u2, @intCast(value)));
}

fn deviceRequired(comptime fields: anytype) kernel.FdRights {
    var rights = kernel.FdRights{};
    rights.query = true;
    inline for (std.meta.fields(@TypeOf(fields))) |field| {
        @field(rights, field.name) = @field(fields, field.name);
    }
    return rights;
}

fn requireDeviceFd(state: *const kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, rights: kernel.FdRights) ?kernel.DeviceObject {
    const device = state.deviceObjectForFd(proc, fd, rights) orelse return null;
    if (device.device == 0) return null;
    _ = pci.locationFromResourceId(device.device) orelse return null;
    return device;
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

fn mapPciBarIntoUser(
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    user_va: u64,
    map_size: u64,
    flags: u64,
    info: pci.BarInfo,
) bool {
    if ((user_va & (page_size - 1)) != 0 or (map_size & (page_size - 1)) != 0) return false;
    if (map_size == 0 or map_size > @as(u64, std.math.maxInt(usize))) return false;
    const paddr = pageAlignDown(info.start);
    if (paddr == 0) return false;
    const span, const span_overflow = @addWithOverflow(info.start - paddr, info.size);
    if (span_overflow != 0) return false;
    const required_size = pageAlignUp(span) orelse return false;
    // The capability names one complete BAR, not an arbitrary physical span.
    // Accepting an oversized length would expose adjacent MMIO resources.
    if (map_size != required_size) return false;
    const physical_end, const physical_end_overflow = @addWithOverflow(paddr, map_size - 1);
    if (physical_end_overflow != 0) return false;
    _ = physical_end;
    const size_usize: usize = @intCast(map_size);
    if (!user_vm.lockVmTransaction(proc)) return false;
    defer user_vm.unlockVmTransaction(proc);
    // BAR mappings are intentionally outside the VMA/VMO model and carry a
    // close-time address identity in their FD.  Overwriting any existing VMA
    // or address-bound object here would leave stale metadata.  Keep both the
    // ordinary and replace-existing forms fail-closed until replacement can
    // update those owners atomically.
    if (!(state.userMapRangeIsFree(proc, user_va, map_size) catch false)) return false;
    if (user_vm.mapUserUncachedLinearRegionWithProt(proc, user_va, paddr, size_usize, .{
        .read = true,
        .write = true,
        .exec = false,
    })) return true;
    _ = flags;
    return false;
}

fn userDmaAddressForRange(
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    proc: kernel.PrincipalId,
    user_va: u64,
    size: u64,
    device_writes: bool,
    except_pinned_object: ?kernel.KernelObjectRef,
    allow_dma_mapping_aliases: bool,
) ?u64 {
    if (user_va == 0 or size == 0) return null;
    const first_page_va = pageAlignDown(user_va);
    const offset = user_va - first_page_va;
    const span, const span_overflow = @addWithOverflow(offset, size);
    if (span_overflow != 0) return null;
    const page_span = pageAlignUp(span) orelse return null;
    const page_count = page_span / page_size;
    if (page_count == 0 or page_count > kernel.max_vmo_backing_pages) return null;
    if (page_span > std.math.maxInt(usize)) return null;

    if (!user_vm.lockAddressSpace(proc)) return null;
    defer user_vm.unlockAddressSpace(proc);
    if (state.rangeConflictsWithDmaDerivation(
        proc,
        first_page_va,
        page_span,
        except_pinned_object,
        allow_dma_mapping_aliases,
    )) {
        return null;
    }

    const page_count_usize: usize = @intCast(page_count);
    var page_index: u64 = 0;
    while (page_index < page_count) : (page_index += 1) {
        const page_delta, const delta_overflow = @mulWithOverflow(page_index, page_size);
        if (delta_overflow != 0) return null;
        const page_va, const va_overflow = @addWithOverflow(first_page_va, page_delta);
        if (va_overflow != 0) return null;
        const paddr = resolveDmaPageWithAddressSpaceLocked(
            state,
            free_list,
            proc,
            page_va,
            device_writes,
        ) orelse return null;
        dma_resolved_paddrs[@intCast(page_index)] = paddr;
    }
    if (kernel.dmaAddressForResolvedPages(
        dma_resolved_paddrs[0..page_count_usize],
        offset,
    )) |dma_address| return dma_address;

    @import("../kernel_log.zig").writeFmt("dma derive noncontig owner={} va=0x{x} pages={}\n", .{ @intFromEnum(proc), first_page_va, page_count });
    return null;
}

fn installResolvedDmaPage(
    proc: kernel.PrincipalId,
    page_va: u64,
    mapping: kernel.NativeVmaFaultMapping,
) ?u64 {
    if (mapping.paddr == 0 or (mapping.paddr & (page_size - 1)) != 0) return null;
    if (mapping.invalidate_size_bytes != 0) {
        if (mapping.invalidate_size_bytes > std.math.maxInt(usize)) return null;
        if (!user_vm.invalidatePresentUserLinearRegionPtes(
            proc,
            mapping.invalidate_start_va,
            @intCast(mapping.invalidate_size_bytes),
        )) return null;
    }
    var paddrs = [_]u64{mapping.paddr};
    if (user_vm.lookupUserMappedPaddrForVa(proc, page_va) != null) {
        if (!user_vm.remapTrustedUserPaddrsWithProt(proc, page_va, paddrs[0..], mapping.prot)) return null;
    } else if (!user_vm.mapLazyUserPaddrsWithProt(proc, page_va, paddrs[0..], mapping.prot)) {
        return null;
    }
    return mapping.paddr;
}

// The caller holds the process address-space lock across the whole scatter
// range. The resolvers take the shared VM-object lock only around their
// prepare/commit operations, matching the normal page-fault/COW path.
fn resolveDmaPageWithAddressSpaceLocked(
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    proc: kernel.PrincipalId,
    page_va: u64,
    device_writes: bool,
) ?u64 {
    const mode = state.nativeVmaDmaPinMode(proc, page_va, device_writes) orelse return null;
    switch (mode) {
        .cow_writable => {
            const mapping = user_copy.resolveNativeVmaCowMappingWithAddressSpaceLocked(
                state,
                free_list,
                proc,
                page_va,
                true,
                false,
            ) orelse return null;
            if (!mapping.prot.read or !mapping.prot.write) return null;
            return installResolvedDmaPage(proc, page_va, mapping);
        },
        .direct_writable, .read_only => {},
    }

    const write_access = mode == .direct_writable;
    // This is required even for an already-present PTE: unlike a bare PTE
    // lookup it validates that the VMA permits the requested DMA access.
    const mapping = user_copy.resolveNativeVmaFaultMappingWithAddressSpaceLocked(
        state,
        free_list,
        proc,
        page_va,
        write_access,
        false,
    ) orelse return null;
    if (!mapping.prot.read or (write_access and !mapping.prot.write)) return null;
    return installResolvedDmaPage(proc, page_va, mapping);
}

fn dmaIovaOrKernelChoice(iova_arg: u64, paddr: u64, size: u64) ?u64 {
    if (iova_arg == capsule_abi.dma_iova_kernel_choose) {
        return if (!vtd.isActive() or vtd.isAddressableRange(paddr, size)) paddr else null;
    }
    if (!vtd.isActive() or vtd.isAddressableRange(iova_arg, size)) return iova_arg;
    return if (vtd.isAddressableRange(paddr, size)) paddr else null;
}

fn rightsForMmio(parent: kernel.FdRights) kernel.FdRights {
    return .{
        .inspect = parent.inspect,
        .dup = parent.dup,
        // The payload contains owner-specific virtual-address identity.
        .transfer = false,
        .close = parent.close,
        .query = parent.query,
        .mmio_map_read = parent.mmio_map_read,
        .mmio_map_write = parent.mmio_map_write,
    };
}

fn rightsForDma(parent: kernel.FdRights) kernel.FdRights {
    return .{
        .inspect = parent.inspect,
        .dup = parent.dup,
        // The payload contains owner-specific VA/IOVA identity.
        .transfer = false,
        .close = parent.close,
        .query = parent.query,
        .derive_dma = parent.derive_dma,
        .cpu_read = parent.cpu_read,
        .cpu_write = parent.cpu_write,
        .dma_read = parent.dma_read,
        .dma_write = parent.dma_write,
    };
}

fn dmaDirectionRights(direction: kernel.CapsuleDmaDirection) kernel.FdRights {
    return switch (direction) {
        // dma_read means the device may read memory; dma_write means it may
        // write memory.
        .to_device => .{ .derive_dma = true, .dma_read = true },
        .from_device => .{ .derive_dma = true, .dma_write = true },
        .bidirectional => .{ .derive_dma = true, .dma_read = true, .dma_write = true },
    };
}

fn rightsForDmaMapping(parent: kernel.FdRights, direction: kernel.CapsuleDmaDirection) kernel.FdRights {
    return .{
        .inspect = parent.inspect,
        .dup = parent.dup,
        // The payload contains owner-specific VA/IOVA identity.
        .transfer = false,
        .close = parent.close,
        .query = parent.query,
        .cpu_read = parent.cpu_read,
        .cpu_write = parent.cpu_write,
        .dma_read = parent.dma_read and direction != .from_device,
        .dma_write = parent.dma_write and direction != .to_device,
    };
}

fn rightsForIrq(parent: kernel.FdRights) kernel.FdRights {
    return .{
        .inspect = parent.inspect,
        .dup = parent.dup,
        .transfer = parent.transfer,
        .close = parent.close,
        .wait = parent.irq_wait,
        .poll = parent.irq_wait,
        .read = parent.irq_wait,
        .query = parent.query,
        .irq_wait = parent.irq_wait,
        .irq_ack = parent.irq_ack,
    };
}

fn capsuleKindForPayload(payload: *const kernel.KernelObjectPayload) ?capsule_abi.CapsuleKind {
    return switch (payload.*) {
        .device => .device,
        .mmio_region => .mmio,
        .dma_buffer => .dma_buffer,
        .dma_mapping => .dma_mapping,
        .irq => .irq,
        else => null,
    };
}

fn writeFdSnapshot(h: anytype, state: *const kernel.KernelState, proc: kernel.PrincipalId, fd: kernel.Fd, out_va: u64, max_words: u64) u64 {
    if (max_words < @as(u64, @intCast(capsule_abi.snapshot_word_count))) return sc.syscall_err_invalid;
    const view = state.fdPayloadWithRightsConst(proc, fd, .{ .query = true }) orelse return sc.syscall_err_invalid;
    const kind = capsuleKindForPayload(view.payload) orelse return sc.syscall_err_invalid;
    var words: [capsule_abi.snapshot_word_count]u64 = [_]u64{0} ** capsule_abi.snapshot_word_count;
    words[capsule_abi.snapshot_fd_index] = fd;
    words[capsule_abi.snapshot_kind_index] = @intFromEnum(kind);
    words[capsule_abi.snapshot_rights_index] = kernel.fdRightsToBits(view.rights);
    words[capsule_abi.snapshot_owner_index] = @intFromEnum(proc);
    switch (view.payload.*) {
        .device => |device| {
            words[capsule_abi.snapshot_device_index] = device.device;
            words[capsule_abi.snapshot_index_index] =
                pci.interruptVectorBaseForResourceId(device.device) orelse 0;
        },
        .mmio_region => |mmio| {
            words[capsule_abi.snapshot_device_index] = mmio.device;
            words[capsule_abi.snapshot_object_id_index] = mmio.paddr;
            words[capsule_abi.snapshot_user_va_index] = mmio.user_va;
            words[capsule_abi.snapshot_size_index] = mmio.size;
            words[capsule_abi.snapshot_index_index] = mmio.bar_index;
            words[capsule_abi.snapshot_flags_index] = mmio.flags;
        },
        .dma_buffer => |dma| {
            words[capsule_abi.snapshot_device_index] = dma.device;
            words[capsule_abi.snapshot_user_va_index] = dma.user_va;
            words[capsule_abi.snapshot_iova_index] = dma.iova;
            words[capsule_abi.snapshot_size_index] = dma.size;
            words[capsule_abi.snapshot_flags_index] = dma.flags;
        },
        .dma_mapping => |mapping| {
            words[capsule_abi.snapshot_device_index] = mapping.device;
            words[capsule_abi.snapshot_user_va_index] = mapping.user_va;
            words[capsule_abi.snapshot_iova_index] = mapping.iova;
            words[capsule_abi.snapshot_size_index] = mapping.size;
            words[capsule_abi.snapshot_index_index] = mapping.page_count;
            words[capsule_abi.snapshot_flags_index] = mapping.flags | (@as(u32, @intFromEnum(mapping.direction)) & 0x3);
        },
        .irq => |irq| {
            words[capsule_abi.snapshot_device_index] = irq.device;
            words[capsule_abi.snapshot_index_index] = irq.vector;
            words[capsule_abi.snapshot_flags_index] = irq.flags | ((@as(u32, @intFromEnum(irq.kind)) & 0x3) << 16);
        },
        else => {},
    }
    for (words, 0..) |word, index| {
        const offset = @as(u64, @intCast(index)) * @sizeOf(u64);
        if (!h.write_user_u64(proc, out_va + offset, word)) return sc.syscall_err_invalid;
    }
    return @as(u64, @intCast(capsule_abi.snapshot_word_count));
}

pub fn dispatch(
    h: anytype,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    frame: *TrapFrame,
) ?u64 {
    return switch (frame.rax) {
        sc.syscall_capsule_query => writeFdSnapshot(h, state, proc, @intCast(frame.rdi), frame.rsi, frame.rdx),
        sc.syscall_capsule_derive_mmio => blk: {
            const device_fd: kernel.Fd = @intCast(frame.rdi);
            const view = state.fdPayloadWithRightsConst(proc, device_fd, deviceRequired(.{ .derive_mmio = true })) orelse break :blk sc.syscall_err_invalid;
            const device = switch (view.payload.*) {
                .device => |dev| dev,
                else => break :blk sc.syscall_err_invalid,
            };
            const bar_index = u32Arg(frame.rsi) orelse break :blk sc.syscall_err_invalid;
            const flags = flagsArg(frame.r8, capsule_abi.mmio_map_known_flags_mask) orelse break :blk sc.syscall_err_invalid;
            if (bar_index >= capsule_abi.pci_bar_count) break :blk sc.syscall_err_invalid;
            const loc = pci.locationFromResourceId(device.device) orelse break :blk sc.syscall_err_invalid;
            const bar = pci.probeBarInfo(loc, @intCast(bar_index)) orelse break :blk sc.syscall_err_invalid;
            if ((bar.flags & pci.bar_flag_mem) == 0) break :blk sc.syscall_err_invalid;
            if (!user_vm.lockVmTransaction(proc)) break :blk sc.syscall_err_map;
            defer user_vm.unlockVmTransaction(proc);
            if (!mapPciBarIntoUser(state, proc, frame.rdx, frame.r10, flags, bar)) break :blk sc.syscall_err_map;
            break :blk state.createMmioRegionFd(proc, .{
                .device = device.device,
                .bar_index = bar_index,
                .paddr = pageAlignDown(bar.start),
                .user_va = frame.rdx,
                .size = frame.r10,
                .flags = flags,
            }, rightsForMmio(view.rights), .{}, first_dynamic_fd) catch |err| {
                // installFd failure already runs the MMIO destructor; object
                // allocation/validation failure has no destructor yet.  The
                // encompassing VM transaction makes this idempotent cleanup
                // race-free in both cases.
                _ = user_vm.unmapUserLinearRegion(proc, frame.rdx, @intCast(frame.r10));
                break :blk statusFromKernelError(err);
            };
        },
        sc.syscall_capsule_derive_dma_buffer => blk: {
            const device_fd: kernel.Fd = @intCast(frame.rdi);
            // A buffer FD publishes an IOVA that callers may use in either
            // direction without another direction-bearing syscall.
            const view = state.fdPayloadWithRightsConst(proc, device_fd, deviceRequired(.{
                .derive_dma = true,
                .dma_read = true,
                .dma_write = true,
            })) orelse break :blk sc.syscall_err_invalid;
            const device = switch (view.payload.*) {
                .device => |dev| dev,
                else => break :blk sc.syscall_err_invalid,
            };
            const flags = flagsArg(frame.r8, capsule_abi.dma_buffer_known_flags_mask) orelse break :blk sc.syscall_err_invalid;
            const paddr = userDmaAddressForRange(
                state,
                h.free_list,
                proc,
                frame.rsi,
                frame.r10,
                true,
                null,
                false,
            ) orelse break :blk sc.syscall_err_invalid;
            const iova = dmaIovaOrKernelChoice(frame.rdx, paddr, frame.r10) orelse break :blk sc.syscall_err_invalid;
            if (!vtd.mapRange(iova, paddr, frame.r10)) break :blk sc.syscall_err_map;
            break :blk state.createDmaBufferFd(proc, .{
                .device = device.device,
                .user_va = frame.rsi,
                .iova = iova,
                .size = frame.r10,
                .flags = flags,
            }, rightsForDma(view.rights), .{}, first_dynamic_fd) catch |err| {
                vtd.unmapRange(iova, frame.r10);
                break :blk statusFromKernelError(err);
            };
        },
        sc.syscall_capsule_derive_dma_mapping => blk: {
            const device_fd: kernel.Fd = @intCast(frame.rdi);
            const view = state.fdPayloadWithRightsConst(proc, device_fd, deviceRequired(.{ .derive_dma = true })) orelse break :blk sc.syscall_err_invalid;
            const device = switch (view.payload.*) {
                .device => |dev| dev,
                else => break :blk sc.syscall_err_invalid,
            };
            const direction = parseDmaDirection(frame.r8) orelse break :blk sc.syscall_err_invalid;
            if (!kernel.isFdRightsSubset(dmaDirectionRights(direction), view.rights)) {
                break :blk sc.syscall_err_invalid;
            }
            const flags = flagsArg(frame.r9, capsule_abi.dma_mapping_known_flags_mask) orelse break :blk sc.syscall_err_invalid;
            const paddr = userDmaAddressForRange(
                state,
                h.free_list,
                proc,
                frame.rsi,
                frame.r10,
                direction != .to_device,
                null,
                !vtd.isActive(),
            ) orelse break :blk sc.syscall_err_invalid;
            const iova = dmaIovaOrKernelChoice(frame.rdx, paddr, frame.r10) orelse break :blk sc.syscall_err_invalid;
            if (!vtd.mapRange(iova, paddr, frame.r10)) break :blk sc.syscall_err_map;
            break :blk state.createDmaMappingFd(proc, .{
                .device = device.device,
                .user_va = frame.rsi,
                .iova = iova,
                .size = frame.r10,
                .direction = direction,
                .flags = flags,
            }, rightsForDmaMapping(view.rights, direction), .{}, first_dynamic_fd) catch |err| {
                vtd.unmapRange(iova, frame.r10);
                break :blk statusFromKernelError(err);
            };
        },
        sc.syscall_capsule_derive_dma_mapping_pages => blk: {
            // A scatter mapping exposes physical page addresses directly. It
            // cannot safely coexist with VT-d until the mapping object owns
            // per-page IOVA state and teardown metadata.
            if (vtd.isActive()) break :blk sc.syscall_err_invalid;

            const device_fd_u32 = u32Arg(frame.rdi) orelse break :blk sc.syscall_err_invalid;
            const view = state.fdPayloadWithRightsConst(
                proc,
                @intCast(device_fd_u32),
                deviceRequired(.{ .derive_dma = true }),
            ) orelse break :blk sc.syscall_err_invalid;
            const device = switch (view.payload.*) {
                .device => |dev| dev,
                else => break :blk sc.syscall_err_invalid,
            };
            const layout = capsule_abi.dmaMappingPagesLayout(frame.rsi, frame.rdx) orelse
                break :blk sc.syscall_err_invalid;
            const direction = parseDmaDirection(frame.r10) orelse break :blk sc.syscall_err_invalid;
            if (!kernel.isFdRightsSubset(dmaDirectionRights(direction), view.rights)) {
                break :blk sc.syscall_err_invalid;
            }
            if (frame.r8 == 0 or frame.r9 < @as(u64, @intCast(layout.page_count)))
                break :blk sc.syscall_err_invalid;
            const output_end, const output_overflow = @addWithOverflow(frame.r8, layout.output_size_bytes);
            if (output_overflow != 0 or
                !user_vm.isUserCanonicalVa(frame.rsi) or
                !user_vm.isUserCanonicalVa(layout.end_va_exclusive - 1) or
                !user_vm.isUserCanonicalVa(frame.r8) or
                !user_vm.isUserCanonicalVa(output_end - 1))
            {
                break :blk sc.syscall_err_invalid;
            }
            const input_page_span: u64 = @as(u64, @intCast(layout.page_count)) * page_size;
            const overlaps = capsule_abi.dmaMappingPagesOutputOverlapsInput(layout, frame.r8) orelse
                break :blk sc.syscall_err_invalid;
            if (overlaps) break :blk sc.syscall_err_invalid;

            const free_fd_count = state.fdFreeCountFrom(proc, first_dynamic_fd) catch |err|
                break :blk statusFromKernelError(err);
            if (free_fd_count == 0) break :blk sc.syscall_err_alloc;

            if (!user_vm.lockAddressSpace(proc)) break :blk sc.syscall_err_invalid;
            defer user_vm.unlockAddressSpace(proc);
            if (state.rangeOverlapsPinnedUserObject(
                proc,
                layout.first_page_va,
                input_page_span,
            )) break :blk sc.syscall_err_invalid;

            const device_writes = direction != .to_device;
            var page_addresses: [capsule_abi.dma_mapping_pages_max_pages]u64 = undefined;
            var page_index: usize = 0;
            while (page_index < layout.page_count) : (page_index += 1) {
                const page_delta: u64 = @as(u64, @intCast(page_index)) * page_size;
                const page_va, const page_overflow = @addWithOverflow(layout.first_page_va, page_delta);
                if (page_overflow != 0) break :blk sc.syscall_err_invalid;
                const page_paddr = resolveDmaPageWithAddressSpaceLocked(
                    state,
                    h.free_list,
                    proc,
                    page_va,
                    device_writes,
                ) orelse break :blk sc.syscall_err_invalid;
                if (page_index == 0) {
                    const first_dma, const dma_overflow = @addWithOverflow(page_paddr, layout.first_page_offset);
                    if (dma_overflow != 0) break :blk sc.syscall_err_invalid;
                    page_addresses[page_index] = first_dma;
                } else {
                    page_addresses[page_index] = page_paddr;
                }
            }

            // Like the existing DMA mapping ABI, the caller must keep the
            // userspace range mapped and unchanged until this FD is closed.
            // This capability deliberately records that lifetime contract;
            // extending it with VMO/COW page pins is a separate design.
            const mapping_fd = state.createDmaMappingFd(proc, .{
                .device = device.device,
                .user_va = frame.rsi,
                .iova = page_addresses[0],
                .size = frame.rdx,
                .page_count = @intCast(layout.page_count),
                .direction = direction,
                .flags = 0,
            }, rightsForDmaMapping(view.rights, direction), .{}, first_dynamic_fd) catch |err|
                break :blk statusFromKernelError(err);

            const output = std.mem.sliceAsBytes(page_addresses[0..layout.page_count]);
            if (!h.copy_bytes_to_user_va(proc, frame.r8, output)) {
                state.closeFdWithFreeList(proc, mapping_fd, h.free_list) catch {};
                break :blk sc.syscall_err_invalid;
            }
            break :blk mapping_fd;
        },
        sc.syscall_capsule_derive_dma_mapping_from_buffer => blk: {
            const buffer_fd: kernel.Fd = @intCast(frame.rdi);
            const buffer_entry = state.fdEntryConst(proc, buffer_fd) orelse break :blk sc.syscall_err_invalid;
            const buffer = state.dmaBufferObjectForFd(proc, buffer_fd, .{ .derive_dma = true }) orelse break :blk sc.syscall_err_invalid;
            const direction = parseDmaDirection(frame.r10) orelse break :blk sc.syscall_err_invalid;
            if (!kernel.isFdRightsSubset(dmaDirectionRights(direction), buffer_entry.rights)) {
                break :blk sc.syscall_err_invalid;
            }
            const flags = flagsArg(frame.r8, capsule_abi.dma_mapping_known_flags_mask) orelse break :blk sc.syscall_err_invalid;
            if (frame.rdx == 0 or frame.rdx > buffer.size) break :blk sc.syscall_err_invalid;
            const paddr = userDmaAddressForRange(
                state,
                h.free_list,
                proc,
                buffer.user_va,
                frame.rdx,
                direction != .to_device,
                buffer_entry.object,
                false,
            ) orelse break :blk sc.syscall_err_invalid;
            const iova = dmaIovaOrKernelChoice(frame.rsi, paddr, frame.rdx) orelse break :blk sc.syscall_err_invalid;
            if (!vtd.mapRange(iova, paddr, frame.rdx)) break :blk sc.syscall_err_map;
            break :blk state.createDmaMappingFd(proc, .{
                .device = buffer.device,
                .user_va = buffer.user_va,
                .iova = iova,
                .size = frame.rdx,
                .direction = direction,
                .flags = flags,
            }, rightsForDmaMapping(buffer_entry.rights, direction), .{}, first_dynamic_fd) catch |err| {
                vtd.unmapRange(iova, frame.rdx);
                break :blk statusFromKernelError(err);
            };
        },
        sc.syscall_capsule_derive_irq => blk: {
            const device_fd: kernel.Fd = @intCast(frame.rdi);
            const view = state.fdPayloadWithRightsConst(proc, device_fd, deviceRequired(.{ .derive_irq = true })) orelse break :blk sc.syscall_err_invalid;
            const device = switch (view.payload.*) {
                .device => |dev| dev,
                else => break :blk sc.syscall_err_invalid,
            };
            const kind = parseIrqKind(frame.rsi) orelse break :blk sc.syscall_err_invalid;
            const vector = u32Arg(frame.rdx) orelse break :blk sc.syscall_err_invalid;
            const flags = flagsArg(frame.r10, capsule_abi.irq_known_flags_mask) orelse break :blk sc.syscall_err_invalid;
            if (pci.interruptVectorBaseForResourceId(device.device) == null)
                break :blk sc.syscall_err_invalid;
            switch (kind) {
                .auto => if (vector != 0) break :blk sc.syscall_err_invalid,
                .msi, .msix => if (vector >= pci.interrupt_vectors_per_device)
                    break :blk sc.syscall_err_invalid,
                .intx => break :blk sc.syscall_err_invalid,
            }
            break :blk state.createIrqFd(proc, .{
                .device = device.device,
                .kind = kind,
                .vector = vector,
                .flags = flags,
            }, rightsForIrq(view.rights), .{}, first_dynamic_fd) catch |err| statusFromKernelError(err);
        },
        sc.syscall_capsule_irq_poll => blk: {
            const count = state.irqEventCountForFd(proc, @intCast(frame.rdi), .{ .irq_wait = true }) orelse break :blk sc.syscall_err_invalid;
            const max_words = frame.r10;
            const flags = u32Arg(frame.r8) orelse break :blk sc.syscall_err_invalid;
            if (flags != 0 or max_words < 1) break :blk sc.syscall_err_invalid;
            if (count != frame.rsi) {
                if (!h.write_user_u64(proc, frame.rdx, count)) break :blk sc.syscall_err_invalid;
                if (!state.acknowledgeIrqEventCountForFd(proc, @intCast(frame.rdi), count)) break :blk sc.syscall_err_invalid;
                break :blk 1;
            }
            break :blk sc.syscall_err_not_ready;
        },
        sc.syscall_capsule_pci_config_read => blk: {
            const device = requireDeviceFd(state, proc, @intCast(frame.rdi), deviceRequired(.{ .config_read = true })) orelse break :blk sc.syscall_err_invalid;
            const offset = u32Arg(frame.rsi) orelse break :blk sc.syscall_err_invalid;
            const len = u32Arg(frame.r10) orelse break :blk sc.syscall_err_invalid;
            if (!validatePciConfigRange(offset, len)) break :blk sc.syscall_err_invalid;
            const loc = pci.locationFromResourceId(device.device) orelse break :blk sc.syscall_err_invalid;
            var bytes: [pci_config_size]u8 = undefined;
            const len_usize: usize = @intCast(len);
            var i: usize = 0;
            while (i < len_usize) : (i += 1) {
                const config_offset: u8 = @intCast(offset + @as(u32, @intCast(i)));
                bytes[i] = pci.readConfigU8(loc, config_offset);
            }
            if (!h.copy_bytes_to_user_va(proc, frame.rdx, bytes[0..len_usize])) break :blk sc.syscall_err_invalid;
            break :blk sc.syscall_ok;
        },
        sc.syscall_capsule_pci_config_write => blk: {
            const device = requireDeviceFd(state, proc, @intCast(frame.rdi), deviceRequired(.{ .config_write = true })) orelse break :blk sc.syscall_err_invalid;
            const offset = u32Arg(frame.rsi) orelse break :blk sc.syscall_err_invalid;
            const len = u32Arg(frame.r10) orelse break :blk sc.syscall_err_invalid;
            if (!validatePciConfigRange(offset, len)) break :blk sc.syscall_err_invalid;
            const loc = pci.locationFromResourceId(device.device) orelse break :blk sc.syscall_err_invalid;
            var bytes: [pci_config_size]u8 = undefined;
            const len_usize: usize = @intCast(len);
            if (!h.copy_user_bytes_from_va(proc, frame.rdx, bytes[0..len_usize])) break :blk sc.syscall_err_invalid;
            var i: usize = 0;
            while (i < len_usize) : (i += 1) {
                const config_offset: u8 = @intCast(offset + @as(u32, @intCast(i)));
                pci.writeConfigU8(loc, config_offset, bytes[i]);
            }
            break :blk sc.syscall_ok;
        },
        sc.syscall_capsule_pci_bar_info => blk: {
            const device = requireDeviceFd(state, proc, @intCast(frame.rdi), deviceRequired(.{})) orelse break :blk sc.syscall_err_invalid;
            const bar_index = u32Arg(frame.rsi) orelse break :blk sc.syscall_err_invalid;
            if (bar_index >= capsule_abi.pci_bar_count) break :blk sc.syscall_err_invalid;
            const loc = pci.locationFromResourceId(device.device) orelse break :blk sc.syscall_err_invalid;
            const info = pci.probeBarInfo(loc, @intCast(bar_index)) orelse break :blk sc.syscall_err_invalid;
            if (!writePciBarInfo(h, proc, frame.rdx, frame.r10, info)) break :blk sc.syscall_err_invalid;
            break :blk @as(u64, @intCast(capsule_abi.bar_info_word_count));
        },
        sc.syscall_capsule_dma_pool_create => blk: {
            if (flagsArg(frame.r10, capsule_abi.dma_pool_known_flags_mask) == null)
                break :blk sc.syscall_err_invalid;
            var rights = kernel.fdRightsFromBits(frame.rsi);
            if (kernel.fdRightsToBits(rights) == 0) {
                rights = .{
                    .inspect = true,
                    .dup = true,
                    .transfer = true,
                    .close = true,
                    .map_read = true,
                    .map_write = true,
                    .revoke = true,
                };
            }
            break :blk state.createContiguousVmoFdWithPages(
                proc,
                frame.rdi,
                rights,
                kernel.fdFlagsFromBits(@truncate(frame.rdx)),
                first_dynamic_fd,
                h.free_list,
            ) catch |err| statusFromKernelError(err);
        },
        else => null,
    };
}
