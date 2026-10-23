const b3 = @import("../ipc_path_trace.zig");
const references = types.references;
const dma_retention = @import("../memory/dma_retention.zig");
const lock_diagnostics = @import("../sync/lock_diagnostics.zig");
const std = @import("std");
const builtin = @import("builtin");
const kernel_log = @import("../kernel_log.zig");
const iommu = @import("../iommu.zig");
const x86_platform = @import("../arch/x86_64/platform.zig");
const types = @import("types.zig");
const capsule = types.capsule;
const initial_process_count = types.initial_process_count;
const initial_process_capacity = types.initial_process_capacity;
const process_count = types.process_count;
const process_principal_value_count = types.process_principal_value_count;
const initial_thread_capacity = types.initial_thread_capacity;
const thread_id_value_count = types.thread_id_value_count;
const device_principal_raw = types.device_principal_raw;
const PrincipalRaw = types.PrincipalRaw;
const PrincipalId = types.PrincipalId;
const CapsuleKind = types.CapsuleKind;
const CapsuleRights = types.CapsuleRights;
const CapsuleMetadata = types.CapsuleMetadata;
const CapsuleSnapshot = types.CapsuleSnapshot;
const CapsuleDmaDirection = types.CapsuleDmaDirection;
const CapsuleIrqKind = types.CapsuleIrqKind;
const MapProt = types.MapProt;
const ProcessDescriptor = types.ProcessDescriptor;
const ProcessStatus = types.ProcessStatus;
const DebugProcessLifecycleReason = types.DebugProcessLifecycleReason;
const KernelError = types.KernelError;
const DmaDeviceId = types.DmaDeviceId;
const invalid_dma_device_id = types.invalid_dma_device_id;
const Fd = types.Fd;
const fd_table_entries = types.fd_table_entries;
const pipe_buffer_bytes = types.pipe_buffer_bytes;
const fd_known_flags_mask = types.fd_known_flags_mask;
const fd_known_rights_mask = types.fd_known_rights_mask;
const FdFlags = types.FdFlags;
const FdRights = types.FdRights;
const KernelObjectKind = types.KernelObjectKind;
const TaskObjectState = types.TaskObjectState;
const ProcessObject = types.ProcessObject;
const ThreadObject = types.ThreadObject;
const DeviceObject = types.DeviceObject;
const MmioRegionObject = types.MmioRegionObject;
const DmaBufferObject = types.DmaBufferObject;
const DmaMappingObject = types.DmaMappingObject;
const IrqObject = types.IrqObject;
const IrqPublishSlot = types.IrqPublishSlot;
const TimerObject = types.TimerObject;
const TimerFdState = types.TimerFdState;
const SerialObject = types.SerialObject;
const PipeRef = types.PipeRef;
const PipeEndpointObject = types.PipeEndpointObject;
const PipePair = types.PipePair;
const PipeIoError = types.PipeIoError;
const KernelObjectRef = types.KernelObjectRef;
const KernelObjectPayload = types.KernelObjectPayload;
const KernelObjectSlot = types.KernelObjectSlot;
const FdEntry = types.FdEntry;
const FdInfo = types.FdInfo;
const FdTable = types.FdTable;
const FdTransferMode = types.FdTransferMode;
const max_ipc_queue_messages = types.max_ipc_queue_messages;
const max_ipc_message_fds = types.max_ipc_message_fds;
const IpcEndpointRef = types.IpcEndpointRef;
const IpcChannelRef = types.IpcChannelRef;
const IpcChannelHandle = types.IpcChannelHandle;
const IpcReplyRef = types.IpcReplyRef;
const IpcWaitKey = types.IpcWaitKey;
const IpcWaiter = types.IpcWaiter;
const ThreadWakeTarget = types.ThreadWakeTarget;
const FdWaitRegistrationRef = types.FdWaitRegistrationRef;
const FdWaitGroupRef = types.FdWaitGroupRef;
const TaskFdWaiter = types.TaskFdWaiter;
const PipeSlot = types.PipeSlot;
const IpcWaitList = types.IpcWaitList;
const IpcTransferredFd = types.IpcTransferredFd;
const IpcMessage = types.IpcMessage;
const IpcQueue = types.IpcQueue;
const IpcEndpointSlot = types.IpcEndpointSlot;
const IpcChannelSlot = types.IpcChannelSlot;
const IpcReplySlot = types.IpcReplySlot;
const IpcSendFd = types.IpcSendFd;
const IpcSendMessage = types.IpcSendMessage;
const IpcRecvFd = types.IpcRecvFd;
const IpcRecvResult = types.IpcRecvResult;
const native_page_size = types.native_page_size;
const NativeVmoKind = types.NativeVmoKind;
const NativeVmoRef = types.NativeVmoRef;
const NativeCowTableRef = types.NativeCowTableRef;
const NativeVmoSlot = types.NativeVmoSlot;
const NativeCowTableSlot = types.NativeCowTableSlot;
const VmaProt = types.VmaProt;
const MmapFlags = types.MmapFlags;
const VmaEntry = types.VmaEntry;
const VmaTable = types.VmaTable;
const NativeVmaFaultMapping = types.NativeVmaFaultMapping;
const RegionFreeRange = types.RegionFreeRange;
const FreePageList = types.FreePageList;
const PageCapability = types.PageCapability;
const empty_process_descriptors_extra = types.empty_process_descriptors_extra;
const empty_vma_tables_extra = types.empty_vma_tables_extra;
const processPrincipalFromIndex = types.processPrincipalFromIndex;
const processIndexFromPrincipal = types.processIndexFromPrincipal;
const principalLabel = types.principalLabel;
const fdFlagsFromBits = types.fdFlagsFromBits;
const fdFlagsToBits = types.fdFlagsToBits;
const fdRightsFromBits = types.fdRightsFromBits;
const fdRightsToBits = types.fdRightsToBits;
const isFdRightsSubset = types.isFdRightsSubset;
const allocEmptyVmoBackingPageStore = types.allocEmptyVmoBackingPageStore;
const vmoBackingPageStorePaddr = types.vmoBackingPageStorePaddr;
const setVmoBackingPageStorePaddr = types.setVmoBackingPageStorePaddr;
const freeVmoBackingPageStore = types.freeVmoBackingPageStore;
const resetVmoBackingPageStore = types.resetVmoBackingPageStore;
pub fn kernelStaticStorageEndAddr() usize {
    return @max(@import("ipc.zig").channelDiagnosticStaticEndAddr(), @max(types.kernelStaticStorageEndAddr(), @max(@intFromPtr(&object_full_reports) + @sizeOf(usize), @intFromPtr(&fd_full_reports) + @sizeOf(usize))));
}
const runtimeStorageBytes = types.runtimeStorageBytes;
const initRuntimeStorage = types.initRuntimeStorage;

pub fn fdIndex(fd: Fd) ?usize {
    if (fd >= types.fd_number_capacity) return null;
    return @intCast(fd);
}

pub fn findFreeFd(table: *const FdTable, min_fd: Fd) ?usize {
    const minimum = fdIndex(min_fd) orelse return null;
    const index = table.entries.firstMarkedAtOrAfter(@intCast(minimum)) orelse return null;
    return if (index < table.capacity) index else null;
}

pub fn nextObjectGeneration(generation: u32) u32 {
    var next = generation +% 1;
    if (next == 0) next = 1;
    return next;
}

pub fn objectOwner(raw: PrincipalRaw) ?PrincipalId {
    const principal: PrincipalId = @enumFromInt(raw);
    if (processIndexFromPrincipal(principal) != null or principal == .Device0) return principal;
    return null;
}

pub fn irqPublishSlotForRef(self: anytype, object_ref: KernelObjectRef) ?*IrqPublishSlot {
    if (object_ref.kind != .irq) return null;
    const object = self.kernelObjectSlotConst(object_ref) orelse return null;
    return self.irq_publish_slots.resolve(object.irq_publication orelse return null);
}

pub fn irqPublishSlotForRefConst(self: anytype, object_ref: KernelObjectRef) ?*const IrqPublishSlot {
    if (object_ref.kind != .irq) return null;
    const object = self.kernelObjectSlotConst(object_ref) orelse return null;
    return self.irq_publish_slots.resolve(object.irq_publication orelse return null);
}

pub fn publishIrqObject(self: anytype, object_ref: KernelObjectRef, irq: IrqObject) void {
    const flags = self.object_directory_leaf.lock();
    defer self.object_directory_leaf.unlock(flags);
    const slot = self.irqPublishSlotForRef(object_ref) orelse return;
    @atomicStore(u8, &slot.active, 0, .release);
    @atomicStore(u64, &slot.event_count, 0, .release);
    @atomicStore(u64, &slot.observed_count, 0, .release);
    @atomicStore(u32, &slot.generation, object_ref.generation, .release);
    @atomicStore(PrincipalRaw, &slot.owner_principal_raw, irq.owner_principal_raw, .release);
    @atomicStore(DmaDeviceId, &slot.device, irq.device, .release);
    @atomicStore(u8, &slot.kind, @intFromEnum(irq.kind), .release);
    @atomicStore(u32, &slot.vector, irq.vector, .release);
    // Route admission still keeps hardware masked/drained until publication.
    @atomicStore(u8, &slot.active, 1, .release);
}

pub fn unpublishIrqObject(self: anytype, object_ref: KernelObjectRef) void {
    const flags = self.object_directory_leaf.lock();
    defer self.object_directory_leaf.unlock(flags);
    const slot = self.irqPublishSlotForRef(object_ref) orelse return;
    if (slot.generation != object_ref.generation) return;
    // The leaf excludes even readers that have not yet examined active.
    // The old 1->2 handshake protected reuse, but not the gap from directory
    // lookup to handshake acquisition when the body's grant can be returned.
    @atomicStore(u8, &slot.active, 0, .release);
}

pub fn irqPublishedEventCount(self: anytype, object_ref: KernelObjectRef) ?u64 {
    const slot = self.irqPublishSlotForRefConst(object_ref) orelse return null;
    if (@atomicLoad(u8, &slot.active, .acquire) == 0) return 0;
    if (@atomicLoad(u32, &slot.generation, .acquire) != object_ref.generation) return 0;
    return @atomicLoad(u64, &slot.event_count, .acquire);
}

pub fn irqPublishedEventPending(self: anytype, object_ref: KernelObjectRef) ?bool {
    const slot = self.irqPublishSlotForRefConst(object_ref) orelse return null;
    if (@atomicLoad(u8, &slot.active, .acquire) == 0) return false;
    if (@atomicLoad(u32, &slot.generation, .acquire) != object_ref.generation) return false;
    return @atomicLoad(u64, &slot.event_count, .acquire) !=
        @atomicLoad(u64, &slot.observed_count, .acquire);
}

pub fn acknowledgeIrqEventCountForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    observed_count: u64,
) bool {
    const entry = self.fdEntryConst(owner, fd) orelse return false;
    if (!entry.rights.irq_wait) return false;
    const slot = self.irqPublishSlotForRef(entry.object) orelse return false;
    if (@atomicLoad(u8, &slot.active, .acquire) == 0) return false;
    if (@atomicLoad(u32, &slot.generation, .acquire) != entry.object.generation) return false;
    var previous = @atomicLoad(u64, &slot.observed_count, .acquire);
    while (previous < observed_count) {
        previous = @cmpxchgWeak(
            u64,
            &slot.observed_count,
            previous,
            observed_count,
            .acq_rel,
            .acquire,
        ) orelse return true;
    }
    return true;
}

pub fn releaseMmioRegionObject(self: anytype, mmio: MmioRegionObject) void {
    if (mmio.user_va == 0 or mmio.size == 0) return;
    const owner = @TypeOf(self.*).objectOwner(mmio.owner_principal_raw) orelse return;
    if (mmio.size > @as(u64, std.math.maxInt(usize))) return;
    const user_vm = @import("../memory/user_vm.zig");
    if (mmio.wc_window_lease_token != 0) user_vm.lockAllVmTransactions();
    defer if (mmio.wc_window_lease_token != 0) user_vm.unlockAllVmTransactions();
    if (mmio.flags & @import("kernel_abi_root").capsule_abi.mmio_map_flag_replace_existing != 0) {
        // The underlying reservation never left the VMA table. Remove only
        // this lease's PTEs, including after an earlier process-VM teardown.
        if (!user_vm.unmapUserMmioOverlay(
            owner,
            mmio.user_va,
            mmio.paddr,
            @intCast(mmio.size),
        )) @panic("MMIO overlay last-close lost its address identity");
    } else {
        const unmapped = user_vm.unmapUserLinearRegion(owner, mmio.user_va, @intCast(mmio.size));
        if (!unmapped and mmio.wc_window_lease_token != 0)
            @panic("WC MMIO last-close lost its user mapping");
    }
    if (mmio.wc_window_lease_token != 0) {
        const x86 = @import("../arch/x86_64/platform.zig");
        @import("../user_copy.zig").retirePhysCopyWindowsForRange(x86.kernel_cr3_value, mmio.paddr, @intCast(mmio.size));
        @import("../user_copy.zig").flushTlbAndCachesForCr3Range(x86.kernel_cr3_value, mmio.paddr, @intCast(mmio.size));
        @import("../user_copy.zig").releaseWcPhysicalWindowLease(mmio.wc_window_lease_token);
    }
}

pub fn releaseDmaBufferObject(self: anytype, dma: DmaBufferObject) void {
    _ = self;
    if (dma.retention) |record| dma_retention.close(record);
    if (dma.size == 0) return;
    _ = releaseDmaIova(dma.device, dma.iova, dma.size);
}

pub fn releaseDmaMappingObject(self: anytype, mapping: DmaMappingObject) void {
    _ = self;
    if (mapping.retention) |record| dma_retention.close(record);
    if (mapping.size == 0) return;
    _ = releaseDmaIova(mapping.device, mapping.iova, mapping.size);
}

fn releaseDmaIova(device: DmaDeviceId, iova: u64, size: u64) bool {
    if (!iommu.isActive() or size == 0) return true;
    const page_size: u64 = 4096;
    const iova_base = iova & ~(page_size - 1);
    const span, const overflow = @addWithOverflow(iova - iova_base, size);
    if (overflow != 0) return false;
    const aligned, const align_overflow = @addWithOverflow(span, page_size - 1);
    if (align_overflow != 0) return false;
    const page_count: usize = @intCast((aligned & ~(page_size - 1)) / page_size);
    if (!iommu.unmapRangeForDevice(device, .fromInt(iova), size)) return false;
    iommu.freeIova(device, .fromInt(iova_base), page_count);
    return true;
}

/// Explicit close must report a failed DMA drain and retain its FD. Forced
/// owner cleanup cannot retain that FD, but the IOMMU quarantine independently
/// owns the physical-page ledger and excludes those pages from PMM reuse.
fn prepareLastDmaClose(self: anytype, object_ref: KernelObjectRef) KernelError!void {
    const slot = self.kernelObjectSlot(object_ref) orelse return KernelError.InvalidState;
    if (slot.ref_count != 1) return;
    switch (slot.payload) {
        .dma_buffer => |*dma| {
            if (!releaseDmaIova(dma.device, dma.iova, dma.size)) return KernelError.InvalidState;
            dma.size = 0;
            if (slot.reference_state.?.payload_edge.target != null) references.Root.update(&slot.reference_state.?.payload_edge, slot.reference_state.?.payload_edge.identity, .{ .start = dma.user_va, .size = 0 });
        },
        .dma_mapping => |*dma| {
            if (!releaseDmaIova(dma.device, dma.iova, dma.size)) return KernelError.InvalidState;
            dma.size = 0;
            if (slot.reference_state.?.payload_edge.target != null) references.Root.update(&slot.reference_state.?.payload_edge, slot.reference_state.?.payload_edge.identity, .{ .start = dma.user_va, .size = 0 });
        },
        else => {},
    }
}

pub fn releaseIrqObject(self: anytype, irq: IrqObject) void {
    _ = self;
    if (irq.retired) return;
    // Last object reference, including process teardown: stop the real source
    // and drain CPU-pending delivery before any subsequent route acquisition.
    // Failure leaves publication removed and reacquisition fail-closed.
    _ = @import("../pci.zig").releaseInterruptRoute(irq.device, @intFromEnum(irq.kind), irq.vector);
}

pub fn objectPayloadMatches(kind: KernelObjectKind, payload: KernelObjectPayload) bool {
    return std.meta.activeTag(payload) == kind;
}

pub fn releaseKernelObjectPayload(self: anytype, slot: *const KernelObjectSlot) void {
    if (slot.kind == .vmo) references.Root.disconnect(&slot.reference_state.?.payload_edge, true);
    switch (slot.payload) {
        .vmo => |vmo_ref| self.releaseNativeVmo(vmo_ref),
        .pager_port => |port_ref| self.pagerPortClosed(port_ref),
        .endpoint => |endpoint_ref| self.clearIpcEndpointSlot(endpoint_ref),
        .channel => |channel_handle| self.releaseIpcChannelHandle(channel_handle),
        .reply => |reply_ref| self.releaseIpcCallAuthority(reply_ref),
        .mmio_region => |mmio| self.releaseMmioRegionObject(mmio),
        .dma_buffer => |dma| self.releaseDmaBufferObject(dma),
        .dma_mapping => |mapping| self.releaseDmaMappingObject(mapping),
        .irq => |irq| self.releaseIrqObject(irq),
        .pipe => |pipe| self.releasePipeEndpoint(pipe),
        else => {},
    }
}

pub fn releaseKernelObjectPayloadWithFreeList(
    self: anytype,
    slot: *const KernelObjectSlot,
    free_list: *FreePageList,
) void {
    if (slot.kind == .vmo) references.Root.disconnect(&slot.reference_state.?.payload_edge, true);
    switch (slot.payload) {
        .vmo => |vmo_ref| self.releaseNativeVmoWithFreeList(vmo_ref, free_list),
        .pager_port => |port_ref| self.pagerPortClosed(port_ref),
        .endpoint => |endpoint_ref| self.clearIpcEndpointSlotWithFreeList(endpoint_ref, free_list),
        .channel => |channel_handle| self.releaseIpcChannelHandleWithFreeList(channel_handle, free_list),
        .reply => |reply_ref| self.releaseIpcCallAuthority(reply_ref),
        .mmio_region => |mmio| self.releaseMmioRegionObject(mmio),
        .dma_buffer => |dma| self.releaseDmaBufferObject(dma),
        .dma_mapping => |mapping| self.releaseDmaMappingObject(mapping),
        .irq => |irq| self.releaseIrqObject(irq),
        .pipe => |pipe| self.releasePipeEndpoint(pipe),
        else => {},
    }
}

// Invalidate identity without destroying the payload. DMA publication failure
// leaves IOVA cleanup to its caller; normal destruction calls this afterwards.
fn resetKernelObjectSlot(self: anytype, slot: *KernelObjectSlot) void {
    const index = @TypeOf(self.fd_objects).indexOf(slot);
    // A waiter on a destroyed timer no longer resolves and leaves the scan.
    if (slot.kind == .timer) self.markTimerDeadlineDirty();
    _ = index;
    if (slot.irq_publication) |ref| {
        const publication = self.irq_publish_slots.resolve(ref).?;
        std.debug.assert(publication.active == 0);
        self.irq_publish_slots.remove(publication, &self.object_directory_leaf);
    }
    if (slot.reference_state) |record| {
        references.Root.disconnect(&record.payload_edge, false);
        self.object_reference_slots.remove(record, null);
        slot.reference_state = null;
    }
    // K and endpoint exclusion have ended resource borrows. Withdraw lookup
    // visibility before recycling the slot and advancing its generation.
    slot.live.close();
    self.fd_objects.withdraw(slot, &self.object_directory_leaf);
    slot.retired_next = self.retired_fd_objects;
    self.retired_fd_objects = slot;
    self.reclaimFdCapabilities();
}

pub fn clearKernelObjectSlot(self: anytype, slot: *KernelObjectSlot) void {
    slot.live.close();
    self.releaseKernelObjectPayload(slot);
    resetKernelObjectSlot(self, slot);
}

pub fn clearKernelObjectSlotWithFreeList(
    self: anytype,
    slot: *KernelObjectSlot,
    free_list: *FreePageList,
) void {
    slot.live.close();
    self.releaseKernelObjectPayloadWithFreeList(slot, free_list);
    resetKernelObjectSlot(self, slot);
}

pub fn resetKernelObjectTable(self: anytype) void {
    @import("fd_capability.zig").deinit(self);
    self.markTimerDeadlineDirty();
    // Construction/teardown only: all producers and body borrowers are gone.
    var objects = self.fd_objects.iterator();
    while (objects.next()) |item| {
        // Payload storage is about to disappear; weak process pin links must
        // be detached first even when teardown bypasses ordinary last-close.
        if (item.value.kind != .vmo) if (item.value.reference_state) |record|
            references.Root.disconnect(&record.payload_edge, false);
        if (item.value.kind != .none) self.releaseKernelObjectPayload(item.value);
    }
    self.irq_publish_slots.deinit();
    self.object_reference_slots.deinit();
    self.fd_objects.deinit();
    var pipes = self.pipes.iterator();
    while (pipes.next()) |item| {
        for (&item.value.waiters) |*list| list.waiters.deinitInto(&self.pipes.body_owner);
    }
    self.pipes.deinit();
    self.next_fd_object_scan = 0;
    self.next_pipe_scan = 0;
}

pub fn resetNativeVmoTable(self: anytype) void {
    self.native_vmos.deinit();
    self.next_native_vmo_scan = 0;
}

// Temporary exhaustion diagnostic. Only failure paths scan and log; the
// state lock serializes these reports with object and FD table mutation.
var object_full_reports: usize = 0;
var fd_full_reports: usize = 0;

fn reportObjectTableFull(self: anytype, requested: KernelObjectKind) void {
    if (builtin.is_test or object_full_reports >= 4) return;
    object_full_reports += 1;
    var counts = [_]usize{0} ** @typeInfo(KernelObjectKind).@"enum".fields.len;
    {
        var pool_iter_26 = self.fd_objects.iterator();
        while (pool_iter_26.next()) |pool_entry_26| {
            const slot = pool_entry_26.value;
            counts[@intFromEnum(slot.kind)] += 1;
        }
    }
    kernel_log.writeFmt("fd: object-table-full requested={s} capacity={}\n", .{ @tagName(requested), self.fd_objects.capacity });
    for (counts, 0..) |count, kind| {
        if (count == 0) continue;
        kernel_log.writeFmt("fd: occupied kind={s} count={}\n", .{ @tagName(@as(KernelObjectKind, @enumFromInt(kind))), count });
    }
}

pub fn createKernelObject(
    self: anytype,
    kind: KernelObjectKind,
    payload: KernelObjectPayload,
) KernelError!KernelObjectRef {
    if (kind == .none or !@TypeOf(self.*).objectPayloadMatches(kind, payload)) return KernelError.InvalidState;
    const allocation = try self.allocatePool(&self.fd_objects, &self.object_directory_leaf, std.math.maxInt(u32));
    const slot = allocation.value;
    errdefer self.fd_objects.remove(slot, &self.object_directory_leaf);
    // Reserve target metadata before publishing kind/payload. K already
    // serializes object creation; connection uses only this record's leaf.
    if (trackedReferenceKind(kind)) {
        const record = try self.allocatePool(&self.object_reference_slots, null, std.math.maxInt(u32));
        slot.reference_state = record.value;
    }
    errdefer if (slot.reference_state) |record| self.object_reference_slots.remove(record, null);
    if (kind == .irq) {
        const publication = try self.allocatePool(&self.irq_publish_slots, &self.object_directory_leaf, std.math.maxInt(u32));
        slot.irq_publication = publication.ref;
    }
    slot.live = @import("../sync/ipc_lifetime.zig").Admission.init();
    slot.kind = kind;
    slot.payload = payload;
    self.fd_objects.mark(allocation.ref.index, 1, kind == .mmio_region or kind == .dma_buffer or kind == .dma_mapping);
    slot.ref_count = 0;
    if (payload == .vmo) {
        const vmo = self.nativeVmoSlot(payload.vmo) orelse return KernelError.InvalidState;
        vmo.references.connect(&slot.reference_state.?.payload_edge, .{ .kind = .fd_object, .index = allocation.ref.index, .generation = allocation.ref.generation }, .{}, true);
    }
    return .{ .kind = kind, .index = allocation.ref.index, .generation = allocation.ref.generation };
}

pub fn kernelObjectSlot(self: anytype, object_ref: KernelObjectRef) ?*KernelObjectSlot {
    if (object_ref.kind == .none) return null;
    const slot = self.fd_objects.resolve(.{ .index = object_ref.index, .generation = object_ref.generation }) orelse return null;
    if (slot.kind != object_ref.kind) return null;
    return slot;
}

pub fn kernelObjectSlotConst(self: anytype, object_ref: KernelObjectRef) ?*const KernelObjectSlot {
    if (object_ref.kind == .none) return null;
    const slot = self.fd_objects.resolve(.{ .index = object_ref.index, .generation = object_ref.generation }) orelse return null;
    if (slot.kind != object_ref.kind) return null;
    return slot;
}

pub fn retainKernelObject(self: anytype, object_ref: KernelObjectRef) KernelError!void {
    const slot = self.kernelObjectSlot(object_ref) orelse return KernelError.InvalidState;
    if (slot.ref_count == std.math.maxInt(u32)) return KernelError.TableFull;
    if (trackedReferenceKind(slot.kind)) slot.reference_state.?.holders.retainTemporary();
    if (slot.ref_count == 0 and trackedReferenceKind(slot.kind)) publishPinnedReference(self, slot, object_ref);
    slot.ref_count += 1;
}

pub fn releaseKernelObject(self: anytype, object_ref: KernelObjectRef) void {
    const slot = self.kernelObjectSlot(object_ref) orelse return;
    if (slot.ref_count == 0) return;
    if (trackedReferenceKind(slot.kind)) slot.reference_state.?.holders.releaseTemporary();
    slot.ref_count -= 1;
    if (slot.ref_count == 0 and slot.kind != .vmo) if (slot.reference_state) |record| references.Root.disconnect(&record.payload_edge, false);
    if (slot.ref_count == 0) {
        self.unpublishIrqObject(object_ref);
        self.clearKernelObjectSlot(slot);
    }
}

pub fn releaseKernelObjectWithFreeList(
    self: anytype,
    object_ref: KernelObjectRef,
    free_list: *FreePageList,
) void {
    const slot = self.kernelObjectSlot(object_ref) orelse return;
    if (slot.ref_count == 0) return;
    if (trackedReferenceKind(slot.kind)) slot.reference_state.?.holders.releaseTemporary();
    slot.ref_count -= 1;
    if (slot.ref_count == 0 and slot.kind != .vmo) if (slot.reference_state) |record| references.Root.disconnect(&record.payload_edge, false);
    if (slot.ref_count == 0) {
        self.unpublishIrqObject(object_ref);
        self.clearKernelObjectSlotWithFreeList(slot, free_list);
    }
}

pub fn kernelObjectRefCount(self: anytype, object_ref: KernelObjectRef) ?u32 {
    const slot = self.kernelObjectSlotConst(object_ref) orelse return null;
    return slot.ref_count;
}

fn sameKernelObjectRef(a: KernelObjectRef, b: KernelObjectRef) bool {
    return a.kind == b.kind and a.index == b.index and a.generation == b.generation;
}

fn objectOwnedPinnedRange(slot: *const KernelObjectSlot, owner_raw: PrincipalRaw) bool {
    if (slot.ref_count == 0) return false;
    return switch (slot.payload) {
        .mmio_region => |object| object.owner_principal_raw == owner_raw,
        .dma_buffer => |object| object.owner_principal_raw == owner_raw,
        .dma_mapping => |object| object.owner_principal_raw == owner_raw,
        else => false,
    };
}

pub fn kernelObjectIsPinnedUserObject(self: anytype, object_ref: KernelObjectRef) bool {
    const slot = self.kernelObjectSlotConst(object_ref) orelse return false;
    return switch (slot.kind) {
        .mmio_region, .dma_buffer, .dma_mapping => true,
        else => false,
    };
}

// K (and all-AS for address-bound objects) is already held by revoke/exit.
// Copy identity under leaf, then resolve source generations under that outer
// transaction. Unlink transfers the existing strong retain to a temporary
// pin; release consumes it only after the source no longer publishes it.
// This needs neither allocation nor an extra refcount increment at close.
pub fn revokeObjectHolders(self: anytype, object_ref: KernelObjectRef, free_list: *FreePageList, kind: ?references.Kind) void {
    while (true) {
        const slot = self.kernelObjectSlot(object_ref) orelse return;
        const record = slot.reference_state orelse unreachable;
        var cursor = references.Cursor.init(&record.holders);
        var selected: ?references.Edge = null;
        while (cursor.next()) |edge| {
            if (kind == null or edge.identity.kind == kind.?) {
                selected = edge;
                break;
            }
        }
        const edge = selected orelse return;
        const id = edge.identity;
        switch (id.kind) {
            .fd_entry => {
                const process = self.processDescriptorSlotConst(id.owner) orelse unreachable;
                std.debug.assert(process.generation == id.owner_generation);
                const table = self.fdTableForProcessIndex(id.owner) orelse unreachable;
                std.debug.assert(id.index < table.capacity);
                const entry = table.at(id.index);
                std.debug.assert(sameKernelObjectRef(entry.object, object_ref) and entry.authority.reference.?.identity.eql(id));
                self.unpublishFdReference(entry);
                entry.replace(.{});
                table.entries.mark(id.index, 1, true);
            },
            .ipc => {
                const queue = switch (id.container) {
                    1 => &(self.ipcEndpointSlot(.{ .index = id.owner, .generation = id.owner_generation }) orelse unreachable).queue,
                    2 => &(self.ipcChannelSlot(.{ .index = id.owner, .generation = id.owner_generation }) orelse unreachable).queues[id.direction],
                    3 => unreachable,
                    else => unreachable,
                };
                var endpoint_guard = queue.acquire();
                defer endpoint_guard.end();
                var matched = false;
                for (0..queue.len) |offset| if (queue.slotIndex(offset) == id.index and queue.messageSequence(offset) == id.sequence) {
                    matched = true;
                    break;
                };
                std.debug.assert(matched);
                const msg = queue.messageAt(id.index);
                const position: usize = id.position;
                std.debug.assert(position < msg.fd_count and sameKernelObjectRef(msg.fds[position].object(), object_ref) and msg.referenceAt(position).?.identity.eql(id));
                msg.removeReference(position);
                var i = position + 1;
                while (i < msg.fd_count) : (i += 1) {
                    msg.fds[i - 1] = msg.fds[i];
                    msg.shiftReference(i, i - 1);
                }
                msg.fd_count -= 1;
                msg.fds[msg.fd_count] = .{};
            },
            else => unreachable,
        }
        self.releaseKernelObjectWithFreeList(object_ref, free_list);
    }
}
fn revokeKernelObjectEverywhereWithFreeList(self: anytype, object_ref: KernelObjectRef, free_list: *FreePageList) void {
    revokeObjectHolders(self, object_ref, free_list, null);
}

/// Address-bound objects cannot outlive the owner address space recorded in
/// their payload.  Revoke every transferred/duplicated/queued reference while
/// that address space and any DMA backing are still intact.
pub fn revokeOwnedPinnedUserObjectsWithFreeList(
    self: anytype,
    owner: PrincipalId,
    free_list: *FreePageList,
) void {
    const owner_raw: PrincipalRaw = @intFromEnum(owner);
    var candidates = @import("vma.zig").pinnedObjectCursor(self, owner);
    while (candidates.next()) |edge| {
        const object_ref: KernelObjectRef = .{ .kind = @enumFromInt(edge.identity.container), .index = edge.identity.index, .generation = edge.identity.generation };
        const slot = self.kernelObjectSlot(object_ref) orelse unreachable;
        std.debug.assert(objectOwnedPinnedRange(slot, owner_raw));
        switch (slot.payload) {
            .dma_buffer => |*dma| if (dma.retention) |record| {
                dma_retention.force(record);
                dma.retention = null;
            },
            .dma_mapping => |*dma| if (dma.retention) |record| {
                dma_retention.force(record);
                dma.retention = null;
            },
            else => {},
        }
        revokeKernelObjectEverywhereWithFreeList(self, object_ref, free_list);
    }
}

pub fn fdTableForActiveProcess(self: anytype, principal: PrincipalId) KernelError!*FdTable {
    try self.requireActiveProcess(principal);
    const result = self.getFdTable(principal) orelse return KernelError.InvalidState;
    if (result.capacity == 0) {
        defer self.finishFdGrowth(result);
        try result.ensureCapacity(types.fd_table_entries, try self.poolAllocator(), &self.object_directory_leaf, &self.fd_retired);
    }
    return result;
}

pub fn fdTableForActiveProcessConst(self: anytype, principal: PrincipalId) KernelError!*const FdTable {
    try self.requireActiveProcess(principal);
    return self.getFdTableConst(principal) orelse KernelError.InvalidState;
}

pub fn fdEntryConst(self: anytype, owner: PrincipalId, fd: Fd) ?*const FdEntry {
    const table = self.getFdTableConst(owner) orelse return null;
    const index = table.index(fd) orelse return null;
    if (table.at(index).*.isEmpty()) return null;
    return table.at(index);
}

// Caller holds KernelState and shared-VM-object locks together. Keep this
// separate from fdInfo: ordinary metadata callers do not hold the latter.
pub fn fdVmoLifetimeInfo(self: anytype, owner: PrincipalId, fd: Fd) ?u64 {
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    if (!entry.rights.inspect) return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    const vmo = switch (slot.payload) {
        .vmo => |vmo| vmo,
        else => return null,
    };
    const native_refs = self.nativeVmoRefCount(vmo) orelse return null;
    const fd_abi = @import("kernel_abi_root").fd_abi;
    return @as(u64, slot.ref_count) |
        (@as(u64, native_refs) << fd_abi.vmo_info_native_refs_shift);
}

pub fn fdInfo(self: anytype, owner: PrincipalId, fd: Fd) ?FdInfo {
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    var info = FdInfo{
        .kind = slot.kind,
        .rights_bits = fdRightsToBits(entry.rights),
        .flags_bits = fdFlagsToBits(entry.flags),
    };
    switch (slot.payload) {
        .process => |process| {
            info.size_bytes = process.exit_code;
            info.extra = @intFromEnum(process.state);
        },
        .thread => |thread| {
            info.size_bytes = thread.exit_code;
            info.extra = @intFromEnum(thread.state);
        },
        .event => |counter| {
            info.size_bytes = counter;
        },
        .vmo => |vmo_ref| {
            info.size_bytes = self.nativeVmoSize(vmo_ref) orelse 0;
        },
        .timer => |timer| {
            info.size_bytes = timer.deadline_ns;
            info.extra = timer.interval_ns;
        },
        .serial => |serial| {
            info.extra = serial.stream;
        },
        .pipe => |endpoint| {
            if (self.pipeSlotConst(endpoint.pipe)) |pipe| {
                info.size_bytes = pipe.len;
                info.extra =
                    (if (endpoint.write) @as(u64, 1) else @as(u64, 0)) |
                    (@as(u64, pipe.read_refs) << 8) |
                    (@as(u64, pipe.write_refs) << 40);
            }
        },
        else => {},
    }
    return info;
}

pub fn eventReadCounter(self: anytype, owner: PrincipalId, fd: Fd) ?u64 {
    const view = self.fdPayloadWithRights(owner, fd, .{ .read = true }) orelse return null;
    const counter = switch (view.payload.*) {
        .event => |counter| counter,
        else => return null,
    };
    if (counter == 0) return 0;
    view.payload.* = .{ .event = 0 };
    return counter;
}

pub fn eventWriteCounter(self: anytype, owner: PrincipalId, fd: Fd, value: u64) KernelError!void {
    if (value == 0) return;
    const view = self.fdPayloadWithRights(owner, fd, .{ .write = true }) orelse return KernelError.InvalidState;
    const counter = switch (view.payload.*) {
        .event => |counter| counter,
        else => return KernelError.InvalidState,
    };
    const next, const overflow = @addWithOverflow(counter, value);
    if (overflow != 0 or next == std.math.maxInt(u64)) return KernelError.InvalidState;
    view.payload.* = .{ .event = next };
}

pub fn eventWakeOwnersForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    out: []PrincipalId,
) KernelError!usize {
    const source = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    const slot = self.kernelObjectSlotConst(source.object) orelse return KernelError.InvalidState;
    if (slot.kind != .event) return KernelError.InvalidState;
    var count: usize = 0;
    var process_index: usize = 0;
    while (process_index < self.process_capacity) : (process_index += 1) {
        const desc = self.processDescriptorSlotConst(process_index) orelse continue;
        if (!desc.active) continue;
        const table = self.fdTableForProcessIndexConst(process_index) orelse continue;
        var fd_index: usize = 0;
        while (fd_index < table.capacity) : (fd_index += 1) {
            const candidate = table.at(fd_index).*;
            if (candidate.object.isNull()) continue;
            if (!candidate.rights.read or (!candidate.rights.wait and !candidate.rights.poll)) continue;
            if (candidate.object.kind != source.object.kind or
                candidate.object.index != source.object.index or
                candidate.object.generation != source.object.generation) continue;
            @TypeOf(self.*).appendUniquePrincipal(out, &count, desc.principal);
            break;
        }
    }
    return count;
}

pub fn timerDueCount(timer: TimerObject, now_ns: u64) u64 {
    if (timer.deadline_ns == 0 or now_ns < timer.deadline_ns) return 0;
    if (timer.interval_ns == 0) return 1;
    return 1 + (now_ns - timer.deadline_ns) / timer.interval_ns;
}

pub fn timerNextWakeNs(timer: TimerObject, now_ns: u64) ?u64 {
    if (timer.deadline_ns == 0) return null;
    if (timerDueCount(timer, now_ns) != 0) return now_ns;
    return timer.deadline_ns;
}

pub fn timerReadExpirations(self: anytype, owner: PrincipalId, fd: Fd, now_ns: u64) ?u64 {
    const view = self.fdPayloadWithRights(owner, fd, .{ .read = true }) orelse return null;
    var timer = switch (view.payload.*) {
        .timer => |timer| timer,
        else => return null,
    };
    const count = @TypeOf(self.*).timerDueCount(timer, now_ns);
    if (count == 0) return 0;
    self.markTimerDeadlineDirty();
    if (timer.interval_ns == 0) {
        timer.deadline_ns = 0;
    } else {
        const next = @as(u128, timer.deadline_ns) + @as(u128, count) * timer.interval_ns;
        timer.deadline_ns = if (next > std.math.maxInt(u64)) 0 else @intCast(next);
    }
    view.payload.* = .{ .timer = timer };
    return count;
}

pub fn timerFdState(self: anytype, owner: PrincipalId, fd: Fd, now_ns: u64) ?TimerFdState {
    const view = self.fdPayloadWithRightsConst(owner, fd, .{ .inspect = true }) orelse return null;
    const timer = switch (view.payload.*) {
        .timer => |timer| timer,
        else => return null,
    };
    const remaining = if (timer.deadline_ns == 0 or now_ns >= timer.deadline_ns) 0 else timer.deadline_ns - now_ns;
    return .{
        .remaining_ns = remaining,
        .interval_ns = timer.interval_ns,
    };
}

pub fn setTimerFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    deadline_ns: u64,
    interval_ns: u64,
    flags: u32,
) KernelError!void {
    const view = self.fdPayloadWithRights(owner, fd, .{ .write = true }) orelse return KernelError.InvalidState;
    var timer = switch (view.payload.*) {
        .timer => |timer| timer,
        else => return KernelError.InvalidState,
    };
    self.markTimerDeadlineDirty();
    timer.deadline_ns = deadline_ns;
    timer.interval_ns = interval_ns;
    timer.flags = flags;
    view.payload.* = .{ .timer = timer };
}

pub fn fdIpcReadable(self: anytype, payload: *const KernelObjectPayload) bool {
    const exclusion = @import("ipc.zig").exclusionForPayload(self, payload) orelse return false;
    var endpoint_guard = exclusion.acquire();
    defer endpoint_guard.end();
    return switch (payload.*) {
        .endpoint => |endpoint_ref| blk: {
            const endpoint = self.ipcEndpointSlotConst(endpoint_ref) orelse break :blk false;
            break :blk !endpoint.queue.isEmpty();
        },
        .channel => |handle| blk: {
            if (handle.side > 1) break :blk false;
            const channel = self.ipcChannelSlotConst(handle.channel) orelse break :blk false;
            // A latched notification must end WAIT_MANY like a message does:
            // multi-threaded clients and the owner wait on many doorbells there.
            break :blk !channel.queues[@as(usize, handle.side)].isEmpty() or
                channel.notify_pending[@as(usize, handle.side)];
        },
        .reply => false,
        else => false,
    };
}

pub fn fdIpcWritable(self: anytype, payload: *const KernelObjectPayload) bool {
    const exclusion = @import("ipc.zig").exclusionForPayload(self, payload) orelse return false;
    var endpoint_guard = exclusion.acquire();
    defer endpoint_guard.end();
    return switch (payload.*) {
        .endpoint => |endpoint_ref| blk: {
            const endpoint = self.ipcEndpointSlotConst(endpoint_ref) orelse break :blk false;
            break :blk !endpoint.queue.isFull();
        },
        .channel => |handle| blk: {
            if (handle.side > 1) break :blk false;
            const channel = self.ipcChannelSlotConst(handle.channel) orelse break :blk false;
            break :blk !channel.queues[1 - @as(usize, handle.side)].isFull();
        },
        .reply => |reply_ref| blk: {
            const reply = self.ipcReplySlotConst(reply_ref) orelse break :blk false;
            break :blk !reply.sent;
        },
        else => false,
    };
}

pub fn fdPollEvents(self: anytype, owner: PrincipalId, fd: Fd, requested: u64, now_ns: u64) ?u64 {
    return self.fdPollEventsWithWriteMin(owner, fd, requested, now_ns, 0);
}

pub fn fdPollEventsWithWriteMin(self: anytype, owner: PrincipalId, fd: Fd, requested: u64, now_ns: u64, min_write_bytes: u64) ?u64 {
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    if (!entry.rights.poll) return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    var ready: u64 = 0;
    if ((requested & @import("kernel_abi_root").fd_abi.event_readable) != 0) {
        const readable = switch (slot.payload) {
            .endpoint, .channel, .reply => entry.rights.recv and self.fdIpcReadable(&slot.payload),
            .process => |process| process.state.isTerminal(),
            .thread => |thread| thread.state.isTerminal(),
            .event => |counter| entry.rights.read and counter != 0,
            .irq => self.irqPublishedEventPending(entry.object) orelse false,
            .timer => |timer| @TypeOf(self.*).timerDueCount(timer, now_ns) != 0,
            .serial => false,
            .pipe => |endpoint| blk: {
                const pipe = self.pipeSlotConst(endpoint.pipe) orelse break :blk false;
                break :blk !endpoint.write and (pipe.len != 0 or pipe.write_refs == 0);
            },
            else => false,
        };
        if (readable) ready |= @import("kernel_abi_root").fd_abi.event_readable;
    }
    if ((requested & @import("kernel_abi_root").fd_abi.event_writable) != 0) {
        const writable = switch (slot.payload) {
            .endpoint, .channel, .reply => (entry.rights.send or entry.rights.call) and
                self.fdIpcWritable(&slot.payload),
            .event => entry.rights.write,
            .serial => entry.rights.write,
            .pipe => |endpoint| blk: {
                const pipe = self.pipeSlotConst(endpoint.pipe) orelse break :blk false;
                break :blk endpoint.write and pipe.read_refs != 0 and @TypeOf(self.*).pipeWritableReadyForBytes(pipe, min_write_bytes);
            },
            else => false,
        };
        if (writable) ready |= @import("kernel_abi_root").fd_abi.event_writable;
    }
    switch (slot.payload) {
        .irq => |irq| {
            if (irq.retired) ready |= @import("kernel_abi_root").fd_abi.event_hangup;
        },
        .channel => |handle| {
            if (handle.side > 1) return null;
            const channel = self.ipcChannelSlotConst(handle.channel) orelse return null;
            var peer_guard = @constCast(channel).exclusion.acquire();
            defer peer_guard.end();
            if (channel.ref_count == 1) ready |= @import("kernel_abi_root").fd_abi.event_hangup;
        },
        .pipe => |endpoint| {
            const pipe = self.pipeSlotConst(endpoint.pipe) orelse return null;
            const pipe_ready = @TypeOf(self.*).pipeReadyEventsForEndpoint(pipe, endpoint);
            ready |= pipe_ready & (@import("kernel_abi_root").fd_abi.event_error | @import("kernel_abi_root").fd_abi.event_hangup);
        },
        else => {},
    }
    return ready & (requested | @import("kernel_abi_root").fd_abi.event_error | @import("kernel_abi_root").fd_abi.event_hangup);
}

pub fn fdNextWakeNs(self: anytype, owner: PrincipalId, fd: Fd, now_ns: u64) ?u64 {
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    if (!entry.rights.wait and !entry.rights.poll) return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    return switch (slot.payload) {
        .timer => |timer| @TypeOf(self.*).timerNextWakeNs(timer, now_ns),
        else => null,
    };
}

pub fn fdPayloadWithRightsConst(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    required_rights: FdRights,
) ?struct { rights: FdRights, payload: *const KernelObjectPayload } {
    const b3_start = b3.now();
    defer b3.end(101, b3_start);
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    if (!isFdRightsSubset(required_rights, entry.rights)) return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    return .{ .rights = entry.rights, .payload = &slot.payload };
}

pub fn fdPayloadWithRights(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    required_rights: FdRights,
) ?struct { rights: FdRights, payload: *KernelObjectPayload } {
    const b3_start = b3.now();
    defer b3.end(101, b3_start);
    const table = self.getFdTable(owner) orelse return null;
    const index = table.index(fd) orelse return null;
    const entry = table.at(index);
    if (entry.isEmpty()) return null;
    if (!isFdRightsSubset(required_rights, entry.rights)) return null;
    const slot = self.kernelObjectSlot(entry.object) orelse return null;
    return .{ .rights = entry.rights, .payload = &slot.payload };
}

pub fn deviceObjectForFd(self: anytype, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?DeviceObject {
    const view = self.fdPayloadWithRightsConst(owner, fd, required_rights) orelse return null;
    return switch (view.payload.*) {
        .device => |device| device,
        else => null,
    };
}

pub fn dmaBufferObjectForFd(self: anytype, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?DmaBufferObject {
    const view = self.fdPayloadWithRightsConst(owner, fd, required_rights) orelse return null;
    return switch (view.payload.*) {
        .dma_buffer => |buffer| buffer,
        else => null,
    };
}

pub fn irqObjectForFd(self: anytype, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?IrqObject {
    const view = self.fdPayloadWithRightsConst(owner, fd, required_rights) orelse return null;
    return switch (view.payload.*) {
        .irq => |irq| irq,
        else => null,
    };
}

pub fn irqKindMatchesInterrupt(kind: u8, irq_entry: u32, entry: u32) bool {
    if (kind == @intFromEnum(CapsuleIrqKind.auto)) {
        return irq_entry == 0 or irq_entry == entry;
    }
    if (kind == @intFromEnum(CapsuleIrqKind.msi) or
        kind == @intFromEnum(CapsuleIrqKind.msix))
    {
        return irq_entry == entry;
    }
    return false;
}

pub fn appendUniquePrincipal(out: []PrincipalId, count: *usize, principal: PrincipalId) void {
    var i: usize = 0;
    while (i < count.*) : (i += 1) {
        if (out[i] == principal) return;
    }
    if (count.* >= out.len) return;
    out[count.*] = principal;
    count.* += 1;
}

pub fn recordDeviceInterruptEvent(
    self: anytype,
    device: DmaDeviceId,
    entry: u32,
    wake_owners: []PrincipalId,
) usize {
    var wake_count: usize = 0;
    var index: u64 = 0;
    while (true) : (index += 1) {
        // One candidate per hold: no allocator, callback, user copy, external
        // lock, or complete-table scan may run inside this IRQ-save leaf.
        const flags = self.object_directory_leaf.lock();
        if (index >= self.irq_publish_slots.capacity) {
            self.object_directory_leaf.unlock(flags);
            break;
        }
        const slot = self.irq_publish_slots.get(@intCast(index)) orelse {
            self.object_directory_leaf.unlock(flags);
            continue;
        };
        if (slot.active == 0 or slot.device != device or
            !@TypeOf(self.*).irqKindMatchesInterrupt(slot.kind, slot.vector, entry))
        {
            self.object_directory_leaf.unlock(flags);
            continue;
        }
        // Unpublish and withdrawal share this leaf. No slot pointer escapes,
        // so a returned body cannot be touched by a delayed IRQ reader. The
        // prior per-slot 1->2 handshake cannot provide that lifetime guarantee.
        _ = @atomicRmw(u64, &slot.event_count, .Add, 1, .acq_rel);
        const owner_raw = slot.owner_principal_raw;
        self.object_directory_leaf.unlock(flags);
        const owner = @TypeOf(self.*).objectOwner(owner_raw) orelse continue;
        @TypeOf(self.*).appendUniquePrincipal(wake_owners, &wake_count, owner);
    }
    return wake_count;
}

pub fn irqEventCountForFd(self: anytype, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?u64 {
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    if (!isFdRightsSubset(required_rights, entry.rights)) return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    if (slot.kind != .irq) return null;
    if (slot.payload.irq.retired) return null;
    return self.irqPublishedEventCount(entry.object);
}

pub fn createDeviceFd(
    self: anytype,
    owner: PrincipalId,
    device: DmaDeviceId,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    if (device == 0) return KernelError.InvalidState;
    const object_ref = try self.createKernelObject(.device, .{ .device = .{
        .owner_principal_raw = @intFromEnum(owner),
        .device = device,
    } });
    return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
        if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
        return err;
    };
}

pub fn createMmioRegionFd(
    self: anytype,
    owner: PrincipalId,
    mmio: MmioRegionObject,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    if (mmio.device == 0 or mmio.paddr == 0 or mmio.size == 0) return KernelError.InvalidState;
    var payload = mmio;
    payload.owner_principal_raw = @intFromEnum(owner);
    const object_ref = try self.createKernelObject(.mmio_region, .{ .mmio_region = payload });
    return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
        if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
        return err;
    };
}

/// Commit a staged overlay only after its PTE publication succeeds. K is
/// held from staging through here, so the FD and its pinned edge become
/// visible with the same address; failure cleanup still sees address zero.
pub fn publishMmioOverlayAddress(self: anytype, owner: PrincipalId, fd: Fd, user_va: u64, lease: u64) void {
    const entry = self.fdEntryConst(owner, fd) orelse unreachable;
    const slot = self.kernelObjectSlot(entry.object) orelse unreachable;
    std.debug.assert(slot.kind == .mmio_region and slot.payload.mmio_region.user_va == 0);
    const node = &slot.reference_state.?.payload_edge;
    slot.payload.mmio_region.user_va = user_va;
    slot.payload.mmio_region.wc_window_lease_token = lease;
    references.Root.update(node, node.identity, .{ .start = user_va, .size = slot.payload.mmio_region.size });
}

/// The global KernelState lock serializes this check with MMIO FD publication
/// and final close. Compare actual physical pages, not BAR ownership: a UC
/// register window and a disjoint WC VRAM window may coexist on one device.
pub fn mmioPhysicalCacheConflict(self: anytype, paddr: u64, size: u64, wc: bool) bool {
    if (self.deviceMemoryCacheConflict(paddr, size, wc)) return true;
    if (size == 0 or paddr > std.math.maxInt(u64) - size) return true;
    const end = paddr + size;
    const capsule_abi = @import("kernel_abi_root").capsule_abi;
    {
        var pool_iter_27 = self.fd_objects.iterator();
        while (pool_iter_27.next()) |pool_entry_27| {
            const slot = pool_entry_27.value;
            if (slot.kind != .mmio_region or slot.ref_count == 0) continue;
            const mmio = slot.payload.mmio_region;
            if (mmio.user_va == 0 or mmio.size == 0 or
                mmio.paddr > std.math.maxInt(u64) - mmio.size) continue;
            if (paddr >= mmio.paddr + mmio.size or mmio.paddr >= end) continue;
            const kind = (mmio.flags & capsule_abi.mmio_map_cache_mask) >>
                capsule_abi.mmio_map_cache_shift;
            if ((kind == @intFromEnum(capsule_abi.MmioCache.wc)) != wc) return true;
        }
    }
    return false;
}

pub fn createDmaBufferFd(
    self: anytype,
    owner: PrincipalId,
    dma: DmaBufferObject,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    if (dma.device == 0 or dma.user_va == 0 or dma.size == 0) return KernelError.InvalidState;
    var payload = dma;
    payload.owner_principal_raw = @intFromEnum(owner);
    payload.retention = try prepareDmaRetention(self, dma.device, dma.iova, dma.size, &.{});
    errdefer if (payload.retention) |record| dma_retention.close(record);
    const object_ref = try self.createKernelObject(.dma_buffer, .{ .dma_buffer = payload });
    return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
        // The syscall that owns the not-yet-published IOVA performs failure
        // cleanup. Do not run the DMA destructor here as well: freeing twice
        // would create a window where another thread can reuse the IOVA before
        // the caller's second unmap.
        if (self.kernelObjectSlot(object_ref)) |slot| {
            resetKernelObjectSlot(self, slot);
        }
        return err;
    };
}

pub fn createDmaMappingFd(
    self: anytype,
    owner: PrincipalId,
    mapping: DmaMappingObject,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    return createDmaMappingFdWithBacking(self, owner, mapping, rights, flags, min_fd, &.{});
}

pub fn createDmaMappingFdWithBacking(
    self: anytype,
    owner: PrincipalId,
    mapping: DmaMappingObject,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
    backing: []const u64,
) KernelError!Fd {
    if (mapping.device == 0 or mapping.user_va == 0 or mapping.size == 0) return KernelError.InvalidState;
    var payload = mapping;
    payload.owner_principal_raw = @intFromEnum(owner);
    if (!iommu.isActive() and mapping.page_count != 0 and backing.len != mapping.page_count) return KernelError.InvalidState;
    payload.retention = try prepareDmaRetention(self, mapping.device, mapping.iova, mapping.size, backing);
    errdefer if (payload.retention) |record| dma_retention.close(record);
    const object_ref = try self.createKernelObject(.dma_mapping, .{ .dma_mapping = payload });
    return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
        if (self.kernelObjectSlot(object_ref)) |slot| {
            resetKernelObjectSlot(self, slot);
        }
        return err;
    };
}

pub fn createIrqFd(
    self: anytype,
    owner: PrincipalId,
    irq: IrqObject,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    if (irq.device == 0) return KernelError.InvalidState;
    var payload = irq;
    payload.owner_principal_raw = @intFromEnum(owner);
    const object_ref = try self.createKernelObject(.irq, .{ .irq = payload });
    const fd = self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
        if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
        return err;
    };
    self.publishIrqObject(object_ref, payload);
    return fd;
}

pub fn createTimerFd(
    self: anytype,
    owner: PrincipalId,
    deadline_ns: u64,
    interval_ns: u64,
    flags: FdFlags,
    rights: FdRights,
    min_fd: Fd,
) KernelError!Fd {
    try self.requireActiveProcess(owner);
    const object_ref = try self.createKernelObject(.timer, .{ .timer = .{
        .owner_principal_raw = @intFromEnum(owner),
        .deadline_ns = deadline_ns,
        .interval_ns = interval_ns,
    } });
    self.markTimerDeadlineDirty();
    return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
        if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
        return err;
    };
}

pub fn createEventFd(
    self: anytype,
    owner: PrincipalId,
    initial_value: u64,
    flags: FdFlags,
    rights: FdRights,
    min_fd: Fd,
) KernelError!Fd {
    try self.requireActiveProcess(owner);
    if (initial_value == std.math.maxInt(u64)) return KernelError.InvalidState;
    const object_ref = try self.createKernelObject(.event, .{ .event = initial_value });
    return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
        if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
        return err;
    };
}

pub fn createSerialFdAt(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    stream: u8,
) KernelError!void {
    try self.requireActiveProcess(owner);
    const table = try self.fdTableForActiveProcess(owner);
    const index = table.index(fd) orelse return KernelError.InvalidState;
    if (!table.at(index).*.isEmpty()) return KernelError.InvalidState;
    const object_ref = try self.createKernelObject(.serial, .{ .serial = .{ .stream = stream } });
    errdefer if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
    try self.retainKernelObject(object_ref);
    table.write(index, .{
        .object = object_ref,
        .rights = .{
            .inspect = true,
            .dup = true,
            .transfer = true,
            .poll = true,
            .set_flags = true,
            .close = true,
            .read = true,
            .write = true,
        },
        .flags = .{ .inherit = true },
    });
}

pub fn installFd(self: anytype, owner: PrincipalId, object_ref: KernelObjectRef, rights: FdRights, flags: FdFlags, min_fd: Fd) KernelError!Fd {
    const node = try self.prepareFdReference(object_ref);
    errdefer if (node) |reserved| references.Pool.cancel(reserved);
    return self.installFdPrepared(owner, object_ref, rights, flags, min_fd, node);
}

pub fn installFdPrepared(
    self: anytype,
    owner: PrincipalId,
    object_ref: KernelObjectRef,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
    node: ?*references.Node,
) KernelError!Fd {
    const table = try self.fdTableForActiveProcess(owner);
    const index = @TypeOf(self.*).findFreeFd(table, min_fd) orelse {
        if (!builtin.is_test and fd_full_reports < 4) {
            fd_full_reports += 1;
            // A failed bounded index lookup already proves no usable slot;
            // diagnostics must not reintroduce a capacity-wide scan on OOM.
            kernel_log.writeFmt("fd: process-table-full owner={} kind={s} capacity={} min={}\n", .{ @intFromEnum(owner), @tagName(object_ref.kind), table.capacity, min_fd });
        }
        return KernelError.TableFull;
    };
    try self.retainKernelObject(object_ref);
    table.write(index, .{
        .object = object_ref,
        .rights = fdRightsFromBits(fdRightsToBits(rights)),
        .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
    });
    self.publishFdReference(owner, index, table.at(index), node);
    return @intCast(index);
}

pub fn closeFd(self: anytype, owner: PrincipalId, fd: Fd) KernelError!void {
    const table = try self.fdTableForActiveProcess(owner);
    const index = table.index(fd) orelse return KernelError.InvalidState;
    const object_ref = table.at(index).*.object;
    if (object_ref.isNull()) return KernelError.InvalidState;
    try prepareLastDmaClose(self, object_ref);
    self.closeIpcReceiveFd(owner, fd, object_ref);
    self.unpublishFdReference(table.at(index));
    table.write(index, .{});
    self.releaseKernelObject(object_ref);
}

pub fn closeFdWithFreeList(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    free_list: *FreePageList,
) KernelError!void {
    const table = try self.fdTableForActiveProcess(owner);
    const index = table.index(fd) orelse return KernelError.InvalidState;
    const object_ref = table.at(index).*.object;
    if (object_ref.isNull()) return KernelError.InvalidState;
    try prepareLastDmaClose(self, object_ref);
    self.closeIpcReceiveFd(owner, fd, object_ref);
    self.unpublishFdReference(table.at(index));
    table.write(index, .{});
    self.releaseKernelObjectWithFreeList(object_ref, free_list);
}

pub fn closeCloexecFdsWithFreeList(
    self: anytype,
    owner: PrincipalId,
    free_list: *FreePageList,
) KernelError!void {
    const table = try self.fdTableForActiveProcess(owner);
    var fd_index: usize = 0;
    while (fd_index < table.capacity) : (fd_index += 1) {
        const entry = table.at(fd_index).*;
        if (entry.object.isNull() or !entry.flags.cloexec) continue;
        self.closeIpcReceiveFd(owner, @intCast(fd_index), entry.object);
        self.unpublishFdReference(table.at(fd_index));
        table.write(fd_index, .{});
        self.releaseKernelObjectWithFreeList(entry.object, free_list);
    }
}

pub fn dupFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    min_fd: Fd,
    rights: FdRights,
    flags: FdFlags,
) KernelError!Fd {
    const table = try self.fdTableForActiveProcessConst(owner);
    const index = table.index(fd) orelse return KernelError.InvalidState;
    const source = table.at(index).*;
    if (source.object.isNull()) return KernelError.InvalidState;
    if (!source.rights.dup) return KernelError.InvalidState;
    if (!isFdRightsSubset(rights, source.rights)) return KernelError.InvalidState;
    return self.installFd(owner, source.object, rights, flags, min_fd);
}

pub fn replaceFd(
    self: anytype,
    owner: PrincipalId,
    dst_fd: Fd,
    src_fd: Fd,
    rights: FdRights,
    flags: FdFlags,
) KernelError!void {
    if (dst_fd == src_fd) return KernelError.InvalidState;
    const src_table = try self.fdTableForActiveProcessConst(owner);
    const src_index = src_table.index(src_fd) orelse return KernelError.InvalidState;
    const dst_index = src_table.index(dst_fd) orelse return KernelError.InvalidState;
    const source = src_table.at(src_index).*;
    if (source.object.isNull()) return KernelError.InvalidState;
    if (!source.rights.dup) return KernelError.InvalidState;
    if (!isFdRightsSubset(rights, source.rights)) return KernelError.InvalidState;

    const node = try self.prepareFdReference(source.object);
    errdefer if (node) |reserved| references.Pool.cancel(reserved);
    try self.retainKernelObject(source.object);
    errdefer self.releaseKernelObject(source.object);
    const dst_table = try self.fdTableForActiveProcess(owner);
    const old_object = dst_table.at(dst_index).*.object;
    self.closeIpcReceiveFd(owner, dst_fd, old_object);
    self.unpublishFdReference(dst_table.at(dst_index));
    dst_table.write(dst_index, .{
        .object = source.object,
        .rights = fdRightsFromBits(fdRightsToBits(rights)),
        .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
    });
    self.publishFdReference(owner, dst_index, dst_table.at(dst_index), node);
    if (!old_object.isNull()) self.releaseKernelObject(old_object);
}

pub fn setFdFlags(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    flags: FdFlags,
    mask: FdFlags,
) KernelError!void {
    const table = try self.fdTableForActiveProcess(owner);
    const index = table.index(fd) orelse return KernelError.InvalidState;
    const entry = table.at(index);
    if (entry.object.isNull()) return KernelError.InvalidState;
    if (!entry.rights.set_flags) return KernelError.InvalidState;
    const old_bits = fdFlagsToBits(entry.flags);
    const mask_bits = fdFlagsToBits(mask);
    const new_bits = (old_bits & ~mask_bits) | (fdFlagsToBits(flags) & mask_bits);
    var replacement = entry.*;
    replacement.flags = fdFlagsFromBits(new_bits);
    table.write(index, replacement);
}

pub fn transferFd(
    self: anytype,
    from: PrincipalId,
    to: PrincipalId,
    fd: Fd,
    min_fd: Fd,
    rights: FdRights,
    flags: FdFlags,
    mode: FdTransferMode,
) KernelError!Fd {
    if (from == to) return KernelError.InvalidState;
    const source_table = try self.fdTableForActiveProcessConst(from);
    const source_index = source_table.index(fd) orelse return KernelError.InvalidState;
    const source = source_table.at(source_index).*;
    if (source.object.isNull()) return KernelError.InvalidState;
    if (!source.rights.transfer) return KernelError.InvalidState;
    if (!isFdRightsSubset(rights, source.rights)) return KernelError.InvalidState;
    if (self.kernelObjectSlotConst(source.object) == null) return KernelError.InvalidState;
    if (self.kernelObjectIsPinnedUserObject(source.object)) return KernelError.InvalidState;

    const dest_table = try self.fdTableForActiveProcess(to);
    const dest_index = @TypeOf(self.*).findFreeFd(dest_table, min_fd) orelse return KernelError.TableFull;

    const node = try self.prepareFdReference(source.object);
    errdefer if (node) |reserved| references.Pool.cancel(reserved);
    const mutable_source_table = if (mode == .move) try self.fdTableForActiveProcess(from) else null;
    switch (mode) {
        .copy => {
            try self.retainKernelObject(source.object);
            dest_table.write(dest_index, .{
                .object = source.object,
                .rights = fdRightsFromBits(fdRightsToBits(rights)),
                .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
            });
        },
        .move => {
            dest_table.write(dest_index, .{
                .object = source.object,
                .rights = fdRightsFromBits(fdRightsToBits(rights)),
                .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
            });
            self.closeIpcReceiveFd(from, @intCast(source_index), source.object);
            self.unpublishFdReference(mutable_source_table.?.at(source_index));
            mutable_source_table.?.write(source_index, .{});
        },
    }
    self.publishFdReference(to, dest_index, dest_table.at(dest_index), node);
    return @intCast(dest_index);
}

pub fn registerTaskReadableWaiterForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    requested_events: u64,
    pollfd_va: u64,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
    group: FdWaitGroupRef,
) KernelError!bool {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    if (thread_index > std.math.maxInt(u32) or wait_token == 0 or group.isNull())
        return KernelError.InvalidState;
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.poll and !entry.rights.wait) return KernelError.InvalidState;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return KernelError.InvalidState;
    // IRQ retirement reports HANGUP independently of the requested event mask.
    if ((requested_events & fd_abi.event_readable) == 0 and slot.kind != .irq) return false;
    const principal_raw: PrincipalRaw = switch (slot.payload) {
        .process => |process| process.principal_raw,
        .thread => |thread| thread.owner_principal_raw,
        .event, .irq, .timer => 0,
        else => return false,
    };
    const thread_index_u32: u32 = @intCast(thread_index);
    {
        var pool_iter_32 = self.task_fd_waiters.iterator();
        while (pool_iter_32.next()) |pool_entry_32| {
            const waiter = pool_entry_32.value;

            if (waiter.thread_index == thread_index_u32 and
                waiter.thread_generation == thread_generation and
                waiter.binding.wait_token == wait_token and
                waiter.binding.group.matches(group) and
                waiter.object.kind == entry.object.kind and
                waiter.object.index == entry.object.index and
                waiter.object.generation == entry.object.generation and
                waiter.owner == owner and
                waiter.pollfd_va == pollfd_va)
            {
                waiter.events |= requested_events;
                return true;
            }
        }
    }
    const allocation = try self.allocateWaitPool(&self.task_fd_waiters);
    const target = allocation.ref.index;
    const registration = FdWaitRegistrationRef{
        .kind = .task,
        .slot = @intCast(target),
        .slot_generation = allocation.ref.generation,
    };
    self.task_fd_waiters.get(@intCast(target)).?.* = .{
        .active = true,
        .principal_raw = principal_raw,
        .object = entry.object,
        .owner = owner,
        .pollfd_va = pollfd_va,
        .events = requested_events,
        .thread_index = thread_index_u32,
        .thread_generation = thread_generation,
        .registration = registration,
        .binding = .{
            .wait_token = wait_token,
            .group = group,
        },
    };
    self.linkFdWaitRegistration(group, registration) catch |err| {
        self.task_fd_waiters.remove(allocation.value, null);
        return err;
    };
    if (entry.object.kind == .timer) {
        self.task_fd_waiters.mark(target, 1, true);
        self.markTimerDeadlineDirty();
    }
    return true;
}

fn retireTaskReadableWaiter(self: anytype, slot_index: usize) void {
    const waiter = (self.task_fd_waiters.get(@intCast(slot_index)).?);
    // The object may already be destroyed or reused; unindex using the
    // linked record's kind, not the current payload in the object table.
    if (waiter.active and waiter.object.kind == .timer) {
        std.debug.assert(self.task_fd_waiters.isMarked(@intCast(slot_index), 1));
        self.task_fd_waiters.mark(@intCast(slot_index), 1, false);
        self.markTimerDeadlineDirty();
    }
    self.task_fd_waiters.remove(waiter, null);
}

pub fn unregisterTaskReadableWaiterForThread(
    self: anytype,
    thread_index: usize,
    thread_generation: u32,
) void {
    if (thread_index > std.math.maxInt(u32)) return;
    const thread_index_u32: u32 = @intCast(thread_index);
    {
        var pool_iter_33 = self.task_fd_waiters.iterator();
        while (pool_iter_33.next()) |pool_entry_33| {
            const waiter = pool_entry_33.value;
            const slot_index: usize = pool_entry_33.ref.index;
            if (!waiter.active or waiter.binding.completion_pending) continue;
            if (waiter.thread_index == thread_index_u32 and waiter.thread_generation == thread_generation) {
                retireTaskReadableWaiter(self, slot_index);
            }
        }
    }
}

pub fn unregisterFdWaitersForThread(
    self: anytype,
    owner: PrincipalId,
    thread_index: usize,
    thread_generation: u32,
) void {
    self.cancelFdWaitGroupsForThread(owner, thread_index, thread_generation);
}

pub fn takeTaskReadableWaitersForPrincipal(
    self: anytype,
    principal: PrincipalId,
    out: []ThreadWakeTarget,
) usize {
    if (out.len == 0) return 0;
    const fd_abi = @import("kernel_abi_root").fd_abi;
    const principal_raw: PrincipalRaw = @intFromEnum(principal);
    var count: usize = 0;
    {
        var pool_iter_34 = self.task_fd_waiters.iterator();
        while (pool_iter_34.next()) |pool_entry_34| {
            const waiter = pool_entry_34.value;
            if (!waiter.active) continue;
            if (waiter.object.kind != .process and waiter.object.kind != .thread) continue;
            if (waiter.principal_raw != principal_raw) continue;
            if ((waiter.events & fd_abi.event_readable) == 0) continue;
            // A single thread may exit while its process and siblings remain
            // alive. Only publish readiness for the actual terminal objects.
            const slot = self.kernelObjectSlotConst(waiter.object) orelse continue;
            const terminal = switch (slot.payload) {
                .process => |process| process.state.isTerminal(),
                .thread => |thread| thread.state.isTerminal(),
                else => false,
            };
            if (!terminal) continue;
            const target = ThreadWakeTarget{
                .owner = waiter.owner,
                .thread_index = @intCast(waiter.thread_index),
                .thread_generation = waiter.thread_generation,
                .wait_token = waiter.binding.wait_token,
                .pollfd_va = waiter.pollfd_va,
                .revents = fd_abi.event_readable,
                .registration = waiter.registration,
                .group = waiter.binding.group,
                .group_link_index = waiter.binding.link_index,
            };
            waiter.binding.appendReady(target, out, &count);
        }
    }
    return count;
}

pub fn takeObjectReadableWaiters(
    self: anytype,
    object: KernelObjectRef,
    out: []ThreadWakeTarget,
) usize {
    if (out.len == 0) return 0;
    const fd_abi = @import("kernel_abi_root").fd_abi;
    var count: usize = 0;
    {
        var pool_iter_35 = self.task_fd_waiters.iterator();
        while (pool_iter_35.next()) |pool_entry_35| {
            const waiter = pool_entry_35.value;
            if (!waiter.active) continue;
            if (waiter.object.kind != object.kind or waiter.object.index != object.index or
                waiter.object.generation != object.generation or
                (waiter.events & fd_abi.event_readable) == 0)
            {
                continue;
            }
            const target = ThreadWakeTarget{
                .owner = waiter.owner,
                .thread_index = waiter.thread_index,
                .thread_generation = waiter.thread_generation,
                .wait_token = waiter.binding.wait_token,
                .pollfd_va = waiter.pollfd_va,
                .revents = fd_abi.event_readable,
                .registration = waiter.registration,
                .group = waiter.binding.group,
                .group_link_index = waiter.binding.link_index,
            };
            waiter.binding.appendReady(target, out, &count);
            if (waiter.object.kind == .timer) self.markTimerDeadlineDirty();
        }
    }
    return count;
}

pub fn takeReadyIrqWaiters(
    self: anytype,
    out: []ThreadWakeTarget,
) usize {
    if (out.len == 0) return 0;
    const fd_abi = @import("kernel_abi_root").fd_abi;
    var count: usize = 0;
    {
        var pool_iter_36 = self.task_fd_waiters.iterator();
        while (pool_iter_36.next()) |pool_entry_36| {
            const waiter = pool_entry_36.value;
            if (!waiter.active) continue;
            if (waiter.object.kind != .irq) continue;
            const slot = self.kernelObjectSlotConst(waiter.object) orelse continue;
            const retired = slot.payload.irq.retired;
            if (!retired and ((waiter.events & fd_abi.event_readable) == 0 or
                !(self.irqPublishedEventPending(waiter.object) orelse false))) continue;
            const target = ThreadWakeTarget{
                .owner = waiter.owner,
                .thread_index = waiter.thread_index,
                .thread_generation = waiter.thread_generation,
                .wait_token = waiter.binding.wait_token,
                .pollfd_va = waiter.pollfd_va,
                .revents = if (retired) fd_abi.event_hangup else fd_abi.event_readable,
                .registration = waiter.registration,
                .group = waiter.binding.group,
                .group_link_index = waiter.binding.link_index,
            };
            waiter.binding.appendReady(target, out, &count);
        }
    }
    return count;
}

pub fn takeReadyTimerWaiters(self: anytype, now_ns: u64, out: []ThreadWakeTarget) usize {
    if (out.len == 0) return 0;
    const fd_abi = @import("kernel_abi_root").fd_abi;
    var count: usize = 0;
    var slots = self.task_fd_waiters.marked(1);
    while (slots.next()) |slot_index| {
        const waiter = (self.task_fd_waiters.get(@intCast(slot_index)).?);
        if (!waiter.active or waiter.object.kind != .timer or
            (waiter.events & fd_abi.event_readable) == 0) continue;
        const slot = self.kernelObjectSlotConst(waiter.object) orelse continue;
        if (timerDueCount(slot.payload.timer, now_ns) == 0) continue;
        const target = ThreadWakeTarget{
            .owner = waiter.owner,
            .thread_index = waiter.thread_index,
            .thread_generation = waiter.thread_generation,
            .wait_token = waiter.binding.wait_token,
            .pollfd_va = waiter.pollfd_va,
            .revents = fd_abi.event_readable,
            .registration = waiter.registration,
            .group = waiter.binding.group,
            .group_link_index = waiter.binding.link_index,
        };
        waiter.binding.appendReady(target, out, &count);
        self.markTimerDeadlineDirty();
    }
    return count;
}

pub fn markTimerDeadlineDirty(self: anytype) void {
    self.timer_deadline_dirty = true;
}

/// Consume the dirty mark under the kernel lock. A clear result means the
/// published deadline already equals nextTimerWaiterDeadline().
pub fn takeTimerDeadlineDirty(self: anytype) bool {
    const dirty = self.timer_deadline_dirty;
    self.timer_deadline_dirty = false;
    return dirty;
}

pub fn nextTimerWaiterDeadline(self: anytype) ?u64 {
    var earliest: ?u64 = null;
    var slots = self.task_fd_waiters.marked(1);
    while (slots.next()) |slot_index| {
        const waiter = (self.task_fd_waiters.get(@intCast(slot_index)).?);
        if (!waiter.active or waiter.binding.completion_pending or waiter.object.kind != .timer) continue;
        const slot = self.kernelObjectSlotConst(waiter.object) orelse continue;
        const deadline = slot.payload.timer.deadline_ns;
        if (deadline != 0 and (earliest == null or deadline < earliest.?)) earliest = deadline;
    }
    return earliest;
}

fn fdWaitListForRegistration(
    self: anytype,
    registration: FdWaitRegistrationRef,
) ?*IpcWaitList {
    return switch (registration.kind) {
        .pipe => blk: {
            const pipe = self.pipeSlot(.{
                .index = registration.index,
                .generation = registration.generation,
            }) orelse break :blk null;
            if (registration.side > 1) break :blk null;
            break :blk &pipe.waiters[registration.side];
        },
        .endpoint => &(self.ipcEndpointSlot(.{
            .index = registration.index,
            .generation = registration.generation,
        }) orelse return null).waiters,
        .channel => blk: {
            const channel = self.ipcChannelSlot(.{
                .index = registration.index,
                .generation = registration.generation,
            }) orelse break :blk null;
            if (registration.side > 1) break :blk null;
            break :blk &channel.waiters[registration.side];
        },
        .reply => &(self.ipcReplySlot(.{
            .index = registration.index,
            .generation = registration.generation,
        }) orelse return null).waiters,
        else => null,
    };
}

fn fdWaitIpcWaiterForRegistration(
    self: anytype,
    registration: FdWaitRegistrationRef,
) ?*IpcWaiter {
    const list = fdWaitListForRegistration(self, registration) orelse return null;
    if (registration.slot >= list.waiters.capacity) return null;
    const waiter = list.waiters.resolve(.{ .index = registration.slot, .generation = registration.slot_generation }) orelse return null;
    if (!waiter.active or !waiter.registration.matches(registration)) return null;
    return waiter;
}

fn fdWaitTaskWaiterForRegistration(
    self: anytype,
    registration: FdWaitRegistrationRef,
) ?*TaskFdWaiter {
    if (registration.kind != .task or
        registration.slot >= self.task_fd_waiters.capacity)
        return null;
    const waiter = self.task_fd_waiters.resolve(.{ .index = registration.slot, .generation = registration.slot_generation }) orelse return null;
    if (!waiter.active or !waiter.registration.matches(registration)) return null;
    return waiter;
}

fn nextFdWaitGroupGeneration(current: u32) u32 {
    const next = current +% 1;
    return if (next == 0) 1 else next;
}

fn fdWaitGroupSlot(self: anytype, group: FdWaitGroupRef) ?*types.FdWaitGroupSlot {
    if (group.isNull() or group.index >= self.fd_wait_groups.capacity) return null;
    const slot = self.fd_wait_groups.resolve(.{ .index = group.index, .generation = group.generation }) orelse return null;
    if (slot.state == .free or slot.generation != group.generation) return null;
    return slot;
}

pub fn beginFdWaitGroup(
    self: anytype,
    owner: PrincipalId,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
) KernelError!FdWaitGroupRef {
    if (thread_index > std.math.maxInt(u32) or wait_token == 0)
        return KernelError.InvalidState;
    self.fd_wait_group_thread_counts.prepareIndex(@intCast(thread_index), try self.waitPoolAllocator()) catch return KernelError.OutOfFreePages;
    const count = self.fd_wait_group_thread_counts.claimIndex(@intCast(thread_index)) catch return KernelError.OutOfFreePages;
    errdefer if (count.* == 0) self.fd_wait_group_thread_counts.remove(count, null);
    const allocation = try self.allocateWaitPool(&self.fd_wait_groups);
    errdefer self.fd_wait_groups.remove(allocation.value, null);
    self.fd_wait_group_links.prepareIndex(allocation.ref.index, try self.waitPoolAllocator()) catch return KernelError.OutOfFreePages;
    const links = self.fd_wait_group_links.claimIndex(allocation.ref.index) catch return KernelError.OutOfFreePages;
    const slot = allocation.value;
    slot.links = links;
    slot.state = .building;
    slot.owner = owner;
    slot.thread_index = @intCast(thread_index);
    slot.thread_generation = thread_generation;
    slot.wait_token = wait_token;
    count.* += 1;
    return .{ .index = allocation.ref.index, .generation = allocation.ref.generation };
}

pub fn linkFdWaitRegistration(
    self: anytype,
    group: FdWaitGroupRef,
    registration: FdWaitRegistrationRef,
) KernelError!void {
    if (registration.isNull()) return KernelError.InvalidState;
    const slot = fdWaitGroupSlot(self, group) orelse return KernelError.InvalidState;
    if (slot.state != .building) return KernelError.InvalidState;
    const link_index: u32 = std.math.cast(u32, slot.link_count) orelse return KernelError.OutOfFreePages;
    slot.links.?.prepareIndex(link_index, try self.waitPoolAllocator()) catch return KernelError.OutOfFreePages;
    const link = slot.links.?.claimIndex(link_index) catch return KernelError.OutOfFreePages;
    link.* = .{ .registration = registration };
    slot.link_count += 1;

    if (registration.kind == .task) {
        const waiter = fdWaitTaskWaiterForRegistration(self, registration) orelse
            return KernelError.InvalidState;
        if (!waiter.binding.group.matches(group)) return KernelError.InvalidState;
        waiter.binding.link_index = link_index;
        waiter.binding.link = link;
        return;
    }
    const waiter = fdWaitIpcWaiterForRegistration(self, registration) orelse
        return KernelError.InvalidState;
    if (!waiter.binding.group.matches(group)) return KernelError.InvalidState;
    waiter.binding.link_index = link_index;
    waiter.binding.link = link;
}

pub fn armFdWaitGroup(self: anytype, group: FdWaitGroupRef) KernelError!void {
    const slot = fdWaitGroupSlot(self, group) orelse return KernelError.InvalidState;
    if (slot.state != .building) return KernelError.InvalidState;
    slot.state = .armed;
}

pub fn detachFdWaitGroup(self: anytype, group: FdWaitGroupRef, wait_token: u64) void {
    const slot = fdWaitGroupSlot(self, group) orelse return;
    if (wait_token == 0 or slot.wait_token != wait_token) return;
    if (slot.state == .completing) return;
    slot.state = .completing;
    const link_count: usize = slot.link_count;
    var link_index: usize = 0;
    while (link_index < link_count) : (link_index += 1) {
        const registration = slot.links.?.get(@intCast(link_index)).?.registration;
        if (registration.kind == .task) {
            if (fdWaitTaskWaiterForRegistration(self, registration)) |waiter| {
                if (waiter.binding.wait_token == wait_token and
                    waiter.binding.group.matches(group) and
                    waiter.binding.link_index == link_index)
                {
                    retireTaskReadableWaiter(self, registration.slot);
                }
            }
            continue;
        }
        if (fdWaitIpcWaiterForRegistration(self, registration)) |waiter| {
            var endpoint_guard = waiter.list.?.acquire();
            defer endpoint_guard.end();
            if (waiter.binding.wait_token == wait_token and
                waiter.binding.group.matches(group) and
                waiter.binding.link_index == link_index)
            {
                waiter.list.?.removeWaiter(waiter);
            }
        }
    }
}

pub fn cancelFdWaitGroup(self: anytype, group: FdWaitGroupRef, wait_token: u64) void {
    const slot = fdWaitGroupSlot(self, group) orelse return;
    if (wait_token == 0 or slot.wait_token != wait_token) return;
    detachFdWaitGroup(self, group, wait_token);
    const count = self.fd_wait_group_thread_counts.get(slot.thread_index).?;
    std.debug.assert(count.* != 0);
    count.* -= 1;
    if (count.* == 0) self.fd_wait_group_thread_counts.remove(count, null);
    // Cancellation ended every registration/snapshot loan. Link identities
    // are private to this group generation, so their directory can also move
    // to the detached cache; keeping it published would hide it from OOM.
    const links = slot.links.?;
    links.deinitInto(&self.fd_wait_group_links.body_owner);
    self.fd_wait_group_links.remove(links, null);
    self.fd_wait_groups.remove(slot, null);
}

/// K and a scheduler completion claim protect the detached group's snapshots.
/// Coalesce duplicate poll items across the entire group, not just this batch.
pub fn takeFdWaitGroupReadyBatch(self: anytype, group: FdWaitGroupRef, wait_token: u64, cursor: *u64, out: []ThreadWakeTarget) usize {
    const slot = fdWaitGroupSlot(self, group) orelse return 0;
    if (slot.state != .completing or slot.wait_token != wait_token) return 0;
    var count: usize = 0;
    while (cursor.* < slot.link_count and count < out.len) {
        const index = cursor.*;
        cursor.* += 1;
        var target = slot.links.?.get(@intCast(index)).?.ready orelse continue;
        if (target.pollfd_va == 0) continue;
        var duplicate = false;
        var links = slot.links.?.iterator();
        while (links.next()) |item| {
            const other = item.value.ready orelse continue;
            if (other.pollfd_va != target.pollfd_va) continue;
            if (item.ref.index < index) {
                duplicate = true;
                break;
            }
            target.revents |= other.revents;
        }
        if (duplicate) continue;
        out[count] = target;
        count += 1;
    }
    return count;
}

pub fn cancelFdWaitGroupsForThread(
    self: anytype,
    owner: PrincipalId,
    thread_index: usize,
    thread_generation: u32,
) void {
    if (thread_index > std.math.maxInt(u32)) return;
    if (self.fd_wait_group_thread_counts.get(@intCast(thread_index)) == null) return;
    const thread_index_u32: u32 = @intCast(thread_index);
    var index: usize = 0;
    while (index < self.fd_wait_groups.capacity) : (index += 1) {
        const slot = self.fd_wait_groups.get(@intCast(index)) orelse continue;
        if (slot.state == .free or slot.owner != owner or
            slot.thread_index != thread_index_u32 or
            slot.thread_generation != thread_generation)
        {
            continue;
        }
        const group = FdWaitGroupRef{
            .index = @intCast(index),
            .generation = slot.generation,
        };
        const wait_token = slot.wait_token;
        self.cancelFdWaitGroup(group, wait_token);
    }
}

pub fn cancelFdWaitGroupsForOwner(self: anytype, owner: PrincipalId) void {
    var index: usize = 0;
    while (index < self.fd_wait_groups.capacity) : (index += 1) {
        const slot = self.fd_wait_groups.get(@intCast(index)) orelse continue;
        if (slot.state == .free or slot.owner != owner) continue;
        const group = FdWaitGroupRef{
            .index = @intCast(index),
            .generation = slot.generation,
        };
        const wait_token = slot.wait_token;
        self.cancelFdWaitGroup(group, wait_token);
    }
}

/// K excludes registration and completion borrowers. Only inactive group
/// caches can be destroyed; active links may still carry completion snapshots.
/// Retain generation metadata in all externally referenced slot tables.
pub fn reclaimFdWaitStorage(self: anytype) void {
    var caches = self.fd_wait_group_links.iterator();
    while (caches.next()) |item| {
        if (self.fd_wait_groups.get(item.ref.index) != null) continue;
        item.value.deinitInto(&self.fd_wait_group_links.body_owner);
        self.fd_wait_group_links.remove(item.value, null);
    }
    self.fd_wait_groups.reclaimEmptyBodies();
    self.fd_wait_group_thread_counts.reclaimEmptyBodies();
    self.task_fd_waiters.reclaimEmptyBodies();
    inline for (.{ "ipc_endpoints", "ipc_replies" }) |name| {
        var objects = @field(self, name).iterator();
        while (objects.next()) |item| item.value.waiters.waiters.reclaimEmptyBodies();
    }
    inline for (.{ "pipes", "ipc_channels" }) |name| {
        var objects = @field(self, name).iterator();
        while (objects.next()) |item| for (&item.value.waiters) |*list| list.waiters.reclaimEmptyBodies();
    }
    _ = self.wait_storage_cache.reclaim();
}

pub fn snapshotFdWaitGroups(
    self: anytype,
    out: []types.FdWaitGroupSnapshot,
) usize {
    var count: usize = 0;
    {
        var pool_iter_37 = self.fd_wait_groups.iterator();
        while (pool_iter_37.next()) |pool_entry_37| {
            const slot = pool_entry_37.value;
            const index: usize = pool_entry_37.ref.index;
            if (slot.state == .free) continue;
            if (count >= out.len) break;
            out[count] = .{
                .group = .{ .index = @intCast(index), .generation = slot.generation },
                .owner = slot.owner,
                .thread_index = slot.thread_index,
                .thread_generation = slot.thread_generation,
                .wait_token = slot.wait_token,
            };
            count += 1;
        }
    }
    return count;
}

fn prepareDmaRetention(self: anytype, device: u64, iova: u64, size: u64, backing: []const u64) KernelError!?*dma_retention.Record {
    if (iommu.isActive()) return null;
    const first = iova & ~@as(u64, 4095);
    const span = std.math.add(u64, iova - first, size) catch return KernelError.InvalidState;
    const rounded = std.math.add(u64, span, 4095) catch return KernelError.InvalidState;
    return dma_retention.prepare(try self.poolAllocator(), device, first, @intCast(rounded / 4096), backing) catch return KernelError.OutOfFreePages;
}

fn dmaEnablingReference(self: anytype, device: u64, object: KernelObjectRef, rights: FdRights) bool {
    // config_write can set command.BME through the existing ABI. Counting
    // only bus_master would let a surviving holder restart a stopped function.
    if (!rights.bus_master and !rights.config_write) return false;
    const slot = self.kernelObjectSlotConst(object) orelse return false;
    return slot.kind == .device and slot.payload.device.device == device;
}
fn dmaReference(self: anytype, device: u64, object: KernelObjectRef) bool {
    const slot = self.kernelObjectSlotConst(object) orelse return false;
    return slot.kind == .device and slot.payload.device.device == device;
}
fn dmaEnablingQueue(self: anytype, device: u64, queue: *const IpcQueue, accounted: *usize) bool {
    var endpoint_guard = queue.acquire();
    defer endpoint_guard.end();
    for (0..queue.len) |offset| {
        const message = queue.messageAt(queue.slotIndex(offset));
        for (message.fds[0..message.fd_count]) |fd| {
            if (dmaEnablingReference(self, device, fd.object(), fd.rights())) return true;
            if (dmaReference(self, device, fd.object())) accounted.* += 1;
        }
    }
    return false;
}
fn hasDmaEnablingReference(self: anytype, device: u64) bool {
    var accounted: usize = 0;
    for (0..self.process_capacity) |process| {
        const table = self.fdTableForProcessIndex(process) orelse continue;
        for (0..table.capacity) |index| {
            const fd = table.at(index).*;
            if (dmaEnablingReference(self, device, fd.object, fd.rights)) return true;
            if (dmaReference(self, device, fd.object)) accounted += 1;
        }
    }
    inline for (.{"ipc_endpoints"}) |name| {
        var iterator = @field(self, name).iterator();
        while (iterator.next()) |item| {
            if (item.value.active and dmaEnablingQueue(self, device, &item.value.queue, &accounted)) return true;
        }
    }
    var channels = self.ipc_channels.iterator();
    while (channels.next()) |item| {
        if (!item.value.active) continue;
        for (&item.value.queues) |*queue| if (dmaEnablingQueue(self, device, queue, &accounted)) return true;
    }
    // A retained reference outside these carriers (for example a staged IPC
    // message) has no visible rights proof. Keep DMA backing until that loan
    // ends; counting only installed FDs could stop a still-authorized device.
    var reference_count: usize = 0;
    var objects = self.fd_objects.iterator();
    while (objects.next()) |item| {
        const slot = item.value;
        if (slot.kind == .device and slot.payload.device.device == device)
            reference_count += slot.ref_count;
    }
    return reference_count != accounted;
}

/// Runs in the existing deferred return pass with KernelState/all-VM held.
/// Capability publication, IPC transfer and PCI config writes share KernelState,
/// so no BME-enabling reference or write can race the proof and ledger removal.
pub fn retryForcedDma(self: anytype) void {
    var cursor = dma_retention.firstRetained();
    while (cursor) |record| {
        if (hasDmaEnablingReference(self, record.device)) {
            record.attempted = false;
        } else if (!record.attempted) {
            const device = record.device;
            dma_retention.attempted(device);
            if (@import("../pci.zig").stopBypassDma(device)) {
                dma_retention.complete(device);
                // Completion may remove several consecutive nodes. Never use
                // a saved successor from their now-reclaimable metadata pages.
                cursor = dma_retention.firstRetained();
                continue;
            }
            if (!@import("builtin").is_test)
                @import("../kernel_log.zig").writeFmt("dma: bypass drain unproven device=0x{x} backing-retained=1\n", .{device});
        }
        cursor = dma_retention.nextRetained(record);
    }
}

fn trackedReferenceKind(kind: KernelObjectKind) bool {
    return switch (kind) {
        .vmo, .mmio_region, .dma_buffer, .dma_mapping => true,
        else => false,
    };
}

pub fn prepareFdReference(self: anytype, object: KernelObjectRef) KernelError!?*references.Node {
    if (!trackedReferenceKind(object.kind)) return null;
    return self.reference_nodes.reserve(try self.poolAllocator()) catch return KernelError.OutOfFreePages;
}

pub fn publishFdReference(self: anytype, owner: PrincipalId, index: usize, entry: *FdEntry, node: ?*references.Node) void {
    // Every install/dup/transfer/fork publishes here under K, after acquiring
    // its object reference. That reference retains the body until withdrawal;
    // the cache adds no ownership and must never be used outside K. Rebuild
    // it even for copies: tracked reference nodes are not shared by entries.
    switch (entry.object.kind) {
        .endpoint => {
            const object = self.kernelObjectSlot(entry.object) orelse unreachable;
            const body = self.ipcEndpointSlot(object.payload.endpoint) orelse unreachable;
            entry.cursor = .{ .ipc_queue = &body.queue };
            entry.authority = .{ .ipc_generation = body.generation };
        },
        .channel => {
            const object = self.kernelObjectSlot(entry.object) orelse unreachable;
            const handle = object.payload.channel;
            const body = self.ipcChannelSlot(handle.channel) orelse unreachable;
            // Store the local receive side. Only CALL reverses direction.
            entry.cursor = .{ .ipc_queue = &body.queues[handle.side] };
            entry.authority = .{ .ipc_generation = body.generation };
        },
        else => {},
    }
    const reserved = node orelse return;
    const owner_index = processIndexFromPrincipal(owner) orelse unreachable;
    const generation = (self.processDescriptorSlotConst(owner_index) orelse unreachable).generation;
    const object = self.kernelObjectSlot(entry.object) orelse unreachable;
    entry.authority.reference = reserved;
    object.reference_state.?.holders.connect(reserved, .{ .kind = .fd_entry, .owner = @intCast(owner_index), .owner_generation = generation, .index = @intCast(index), .generation = reserved.allocation.generation, .sequence = reserved.allocation.index }, .{}, true);
}

pub fn unpublishFdReference(self: anytype, entry: *FdEntry) void {
    _ = self;
    if (!trackedReferenceKind(entry.object.kind)) return;
    const node = entry.authority.reference orelse return;
    references.Root.disconnect(node, true);
    references.Pool.cancel(node);
    entry.authority.reference = null;
}

fn publishPinnedReference(self: anytype, slot: *KernelObjectSlot, object: KernelObjectRef) void {
    const Pinned = struct { owner: PrincipalRaw, range: references.Range };
    const pinned: Pinned = switch (slot.payload) {
        inline .mmio_region, .dma_buffer, .dma_mapping => |payload| .{ .owner = payload.owner_principal_raw, .range = .{ .start = payload.user_va, .size = payload.size } },
        else => return,
    };
    const index = processIndexFromPrincipal(@enumFromInt(pinned.owner)) orelse unreachable;
    const table = self.vmaTableForProcessIndex(index) orelse unreachable;
    const generation = (self.processDescriptorSlotConst(index) orelse unreachable).generation;
    table.pinned_objects.connect(&slot.reference_state.?.payload_edge, .{ .kind = .pinned_object, .container = @intFromEnum(slot.kind), .owner = @intCast(index), .owner_generation = generation, .index = object.index, .generation = object.generation }, pinned.range, false);
}
