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
const max_process_slots = types.max_process_slots;
const initial_thread_capacity = types.initial_thread_capacity;
const max_thread_slots = types.max_thread_slots;
const device_count = types.device_count;
const device_principal_raw = types.device_principal_raw;
const principal_count = types.principal_count;
const PrincipalRaw = types.PrincipalRaw;
const PrincipalId = types.PrincipalId;
const CapsuleKind = types.CapsuleKind;
const CapsuleRights = types.CapsuleRights;
const CapsuleMetadata = types.CapsuleMetadata;
const CapsuleSnapshot = types.CapsuleSnapshot;
const CapsuleDmaDirection = types.CapsuleDmaDirection;
const CapsuleIrqKind = types.CapsuleIrqKind;
const MapProt = types.MapProt;
const EndpointRoute = types.EndpointRoute;
const Region = types.Region;
const ProcessDescriptor = types.ProcessDescriptor;
const ProcessStatus = types.ProcessStatus;
const DebugProcessLifecycleReason = types.DebugProcessLifecycleReason;
const KernelError = types.KernelError;
const DmaDeviceId = types.DmaDeviceId;
const invalid_dma_device_id = types.invalid_dma_device_id;
const Fd = types.Fd;
const fd_table_entries = types.fd_table_entries;
const max_fd_objects = types.max_fd_objects;
const max_pipes = types.max_pipes;
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
const max_ipc_endpoints = types.max_ipc_endpoints;
const max_ipc_channels = types.max_ipc_channels;
const max_ipc_replies = types.max_ipc_replies;
const max_ipc_queue_messages = types.max_ipc_queue_messages;
const max_ipc_message_fds = types.max_ipc_message_fds;
const max_ipc_waiters = types.max_ipc_waiters;
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
const max_task_fd_waiters = types.max_task_fd_waiters;
const max_ipc_object_waiters = types.max_ipc_object_waiters;
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
const max_native_vmos = types.max_native_vmos;
const max_vmas_per_process = types.max_vmas_per_process;
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
const EndpointTable = types.EndpointTable;
const PublishedEndpointTable = types.PublishedEndpointTable;
const max_vmo_backing_pages = types.max_vmo_backing_pages;
const max_vmo_backing_store_pages = types.max_vmo_backing_store_pages;
const RegionFreeRange = types.RegionFreeRange;
const FreePageList = types.FreePageList;
const PageCapability = types.PageCapability;
const empty_vmo_backing_page_store = types.empty_vmo_backing_page_store;
const vmo_backing_page_store = types.vmo_backing_page_store;
const empty_process_descriptors_extra = types.empty_process_descriptors_extra;
const empty_endpoint_tables_extra = types.empty_endpoint_tables_extra;
const empty_fd_tables_extra = types.empty_fd_tables_extra;
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
    if (fd >= types.fd_table_limit) return null;
    return @intCast(fd);
}

pub fn findFreeFd(table: *const FdTable, min_fd: Fd) ?usize {
    var index = fdIndex(min_fd) orelse return null;
    while (index < table.slots().len) : (index += 1) {
        if (table.slots()[index].isEmpty()) return index;
    }
    return null;
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
    const index: usize = @intCast(object_ref.index);
    if (index >= self.irq_publish_slots.len) return null;
    return &self.irq_publish_slots[index];
}

pub fn irqPublishSlotForRefConst(self: anytype, object_ref: KernelObjectRef) ?*const IrqPublishSlot {
    if (object_ref.kind != .irq) return null;
    const index: usize = @intCast(object_ref.index);
    if (index >= self.irq_publish_slots.len) return null;
    return &self.irq_publish_slots[index];
}

pub fn publishIrqObject(self: anytype, object_ref: KernelObjectRef, irq: IrqObject) void {
    const slot = self.irqPublishSlotForRef(object_ref) orelse return;
    @atomicStore(u8, &slot.active, 0, .release);
    @atomicStore(u64, &slot.event_count, 0, .release);
    @atomicStore(u64, &slot.observed_count, 0, .release);
    @atomicStore(u32, &slot.generation, object_ref.generation, .release);
    @atomicStore(PrincipalRaw, &slot.owner_principal_raw, irq.owner_principal_raw, .release);
    @atomicStore(DmaDeviceId, &slot.device, irq.device, .release);
    @atomicStore(u8, &slot.kind, @intFromEnum(irq.kind), .release);
    @atomicStore(u32, &slot.vector, irq.vector, .release);
    // Publish the conservative candidate before making the slot active. IRQ
    // registration keeps the hardware route masked/drained until this returns.
    // Never clear individual bits: retirement/reuse still uses the active
    // handshake below, without adding a bitmap-clear/republication race.
    const index: usize = @intCast(object_ref.index);
    _ = @atomicRmw(u64, &self.irq_publish_candidates[index / 64], .Or, @as(u64, 1) << @as(u6, @intCast(index % 64)), .release);
    @atomicStore(u8, &slot.active, 1, .release);
}

pub fn unpublishIrqObject(self: anytype, object_ref: KernelObjectRef) void {
    const slot = self.irqPublishSlotForRef(object_ref) orelse return;
    const generation = @atomicLoad(u32, &slot.generation, .acquire);
    if (generation != object_ref.generation) return;
    // 1 is published; 2 is a publisher holding this slot. Merely storing
    // inactive races an already validated handler incrementing a reused slot.
    while (true) {
        const active = @atomicLoad(u8, &slot.active, .acquire);
        if (active == 0) return;
        if (active == 1 and @cmpxchgWeak(u8, &slot.active, 1, 0, .acq_rel, .acquire) == null) return;
        std.atomic.spinLoopHint();
    }
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
    if (mmio.flags & @import("kernel_abi_root").capsule_abi.mmio_map_flag_replace_existing != 0) {
        // The underlying reservation never left the VMA table. Remove only
        // this lease's PTEs, including after an earlier process-VM teardown.
        if (!@import("../memory/user_vm.zig").unmapUserMmioOverlay(
            owner,
            mmio.user_va,
            mmio.paddr,
            @intCast(mmio.size),
        )) @panic("MMIO overlay last-close lost its address identity");
        return;
    }
    _ = @import("../memory/user_vm.zig").unmapUserLinearRegion(owner, mmio.user_va, @intCast(mmio.size));
}

pub fn releaseDmaBufferObject(self: anytype, dma: DmaBufferObject) void {
    _ = self;
    if (dma.size == 0) return;
    _ = releaseDmaIova(dma.device, dma.iova, dma.size);
}

pub fn releaseDmaMappingObject(self: anytype, mapping: DmaMappingObject) void {
    _ = self;
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
    if (!iommu.unmapRangeForDevice(device, iova, size)) return false;
    iommu.freeIova(device, iova_base, page_count);
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
        },
        .dma_mapping => |*dma| {
            if (!releaseDmaIova(dma.device, dma.iova, dma.size)) return KernelError.InvalidState;
            dma.size = 0;
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
    switch (slot.payload) {
        .vmo => |vmo_ref| self.releaseNativeVmo(vmo_ref),
        .endpoint => |endpoint_ref| self.clearIpcEndpointSlot(endpoint_ref),
        .channel => |channel_handle| self.releaseIpcChannelHandle(channel_handle),
        .reply => |reply_ref| self.clearIpcReplySlot(reply_ref),
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
    switch (slot.payload) {
        .vmo => |vmo_ref| self.releaseNativeVmoWithFreeList(vmo_ref, free_list),
        .endpoint => |endpoint_ref| self.clearIpcEndpointSlotWithFreeList(endpoint_ref, free_list),
        .channel => |channel_handle| self.releaseIpcChannelHandleWithFreeList(channel_handle, free_list),
        .reply => |reply_ref| self.clearIpcReplySlotWithFreeList(reply_ref, free_list),
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
    const index = (@intFromPtr(slot) - @intFromPtr(&self.fd_objects[0])) / @sizeOf(KernelObjectSlot);
    self.pinned_object_slots.unset(index);
    slot.kind = .none;
    slot.ref_count = 0;
    slot.payload = .{ .none = {} };
    slot.generation = @TypeOf(self.*).nextObjectGeneration(slot.generation);
}

pub fn clearKernelObjectSlot(self: anytype, slot: *KernelObjectSlot) void {
    self.releaseKernelObjectPayload(slot);
    resetKernelObjectSlot(self, slot);
}

pub fn clearKernelObjectSlotWithFreeList(
    self: anytype,
    slot: *KernelObjectSlot,
    free_list: *FreePageList,
) void {
    self.releaseKernelObjectPayloadWithFreeList(slot, free_list);
    resetKernelObjectSlot(self, slot);
}

pub fn resetKernelObjectTable(self: anytype) void {
    for (self.irq_publish_slots[0..]) |*slot| {
        @atomicStore(u8, &slot.active, 0, .release);
        slot.* = .{};
    }
    // Like the existing slot reset, this requires all IRQ producers quiescent.
    @memset(&self.irq_publish_candidates, 0);
    for (self.fd_objects[0..]) |*slot| {
        if (slot.kind != .none) self.releaseKernelObjectPayload(slot);
        slot.* = .{};
    }
    self.pinned_object_slots = .initEmpty();
    for (self.pipes[0..]) |*slot| {
        slot.* = .{ .generation = @TypeOf(self.*).nextObjectGeneration(slot.generation) };
    }
    self.next_fd_object_scan = 0;
    self.next_pipe_scan = 0;
}

pub fn resetNativeVmoTable(self: anytype) void {
    @memset(self.native_vmos[0..], .{});
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
    for (self.fd_objects[0..]) |*slot| counts[@intFromEnum(slot.kind)] += 1;
    kernel_log.writeFmt("fd: object-table-full requested={s} capacity={}\n", .{ @tagName(requested), max_fd_objects });
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
    var offset: usize = 0;
    while (offset < max_fd_objects) : (offset += 1) {
        const index = (self.next_fd_object_scan + offset) % max_fd_objects;
        const slot = &self.fd_objects[index];
        if (slot.kind != .none or slot.ref_count != 0) continue;
        if (slot.generation == 0) slot.generation = 1;
        slot.kind = kind;
        slot.payload = payload;
        slot.ref_count = 0;
        self.pinned_object_slots.setValue(index, switch (kind) {
            .mmio_region, .dma_buffer, .dma_mapping => true,
            else => false,
        });
        self.next_fd_object_scan = (index + 1) % max_fd_objects;
        return .{
            .kind = kind,
            .index = @intCast(index),
            .generation = slot.generation,
        };
    }
    reportObjectTableFull(self, kind);
    return KernelError.TableFull;
}

pub fn kernelObjectSlot(self: anytype, object_ref: KernelObjectRef) ?*KernelObjectSlot {
    if (object_ref.kind == .none) return null;
    const index: usize = @intCast(object_ref.index);
    if (index >= max_fd_objects) return null;
    const slot = &self.fd_objects[index];
    if (slot.kind != object_ref.kind or slot.generation != object_ref.generation) return null;
    return slot;
}

pub fn kernelObjectSlotConst(self: anytype, object_ref: KernelObjectRef) ?*const KernelObjectSlot {
    if (object_ref.kind == .none) return null;
    const index: usize = @intCast(object_ref.index);
    if (index >= max_fd_objects) return null;
    const slot = &self.fd_objects[index];
    if (slot.kind != object_ref.kind or slot.generation != object_ref.generation) return null;
    return slot;
}

pub fn retainKernelObject(self: anytype, object_ref: KernelObjectRef) KernelError!void {
    const slot = self.kernelObjectSlot(object_ref) orelse return KernelError.InvalidState;
    if (slot.ref_count == std.math.maxInt(u32)) return KernelError.TableFull;
    slot.ref_count += 1;
}

pub fn releaseKernelObject(self: anytype, object_ref: KernelObjectRef) void {
    const slot = self.kernelObjectSlot(object_ref) orelse return;
    if (slot.ref_count == 0) return;
    slot.ref_count -= 1;
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
    slot.ref_count -= 1;
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

fn revokeKernelObjectFromIpcQueueWithFreeList(
    self: anytype,
    queue: *IpcQueue,
    object_ref: KernelObjectRef,
    free_list: *FreePageList,
) void {
    var msg_offset: usize = 0;
    while (msg_offset < queue.len) : (msg_offset += 1) {
        const msg = &queue.messages[queue.slotIndex(msg_offset)];
        var fd_index: usize = 0;
        while (fd_index < msg.fd_count and fd_index < max_ipc_message_fds) {
            if (!sameKernelObjectRef(msg.fds[fd_index].object, object_ref)) {
                fd_index += 1;
                continue;
            }
            self.releaseKernelObjectWithFreeList(object_ref, free_list);
            var shift_index = fd_index + 1;
            while (shift_index < msg.fd_count and shift_index < max_ipc_message_fds) : (shift_index += 1) {
                msg.fds[shift_index - 1] = msg.fds[shift_index];
            }
            if (msg.fd_count != 0) msg.fd_count -= 1;
            msg.fds[msg.fd_count] = .{};
        }
    }
}

fn revokeKernelObjectEverywhereWithFreeList(
    self: anytype,
    object_ref: KernelObjectRef,
    free_list: *FreePageList,
) void {
    var process_index: usize = 0;
    while (process_index < self.process_capacity) : (process_index += 1) {
        const table = self.fdTableForProcessIndex(process_index) orelse continue;
        for (table.slots()[0..]) |*entry| {
            if (!sameKernelObjectRef(entry.object, object_ref)) continue;
            entry.* = .{};
            self.releaseKernelObjectWithFreeList(object_ref, free_list);
        }
    }
    for (self.ipc_endpoints[0..]) |*slot| {
        if (slot.active) revokeKernelObjectFromIpcQueueWithFreeList(self, &slot.queue, object_ref, free_list);
    }
    for (self.ipc_channels[0..]) |*slot| {
        if (!slot.active) continue;
        revokeKernelObjectFromIpcQueueWithFreeList(self, &slot.queues[0], object_ref, free_list);
        revokeKernelObjectFromIpcQueueWithFreeList(self, &slot.queues[1], object_ref, free_list);
    }
    for (self.ipc_replies[0..]) |*slot| {
        if (slot.active) revokeKernelObjectFromIpcQueueWithFreeList(self, &slot.queue, object_ref, free_list);
    }
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
    var object_index: usize = 0;
    while (object_index < self.fd_objects.len) : (object_index += 1) {
        const slot = &self.fd_objects[object_index];
        if (!objectOwnedPinnedRange(slot, owner_raw)) continue;
        const object_ref = KernelObjectRef{
            .kind = slot.kind,
            .index = @intCast(object_index),
            .generation = slot.generation,
        };
        revokeKernelObjectEverywhereWithFreeList(self, object_ref, free_list);
    }
}

pub fn fdTableForActiveProcess(self: anytype, principal: PrincipalId) KernelError!*FdTable {
    try self.requireActiveProcess(principal);
    return self.getFdTable(principal) orelse KernelError.InvalidState;
}

pub fn fdTableForActiveProcessConst(self: anytype, principal: PrincipalId) KernelError!*const FdTable {
    try self.requireActiveProcess(principal);
    return self.getFdTableConst(principal) orelse KernelError.InvalidState;
}

pub fn fdEntryConst(self: anytype, owner: PrincipalId, fd: Fd) ?*const FdEntry {
    const table = self.getFdTableConst(owner) orelse return null;
    const index = table.index(fd) orelse return null;
    if (table.slots()[index].isEmpty()) return null;
    return &table.slots()[index];
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
        while (fd_index < table.slots().len) : (fd_index += 1) {
            const candidate = table.slots()[fd_index];
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
    timer.deadline_ns = deadline_ns;
    timer.interval_ns = interval_ns;
    timer.flags = flags;
    view.payload.* = .{ .timer = timer };
}

pub fn fdIpcReadable(self: anytype, payload: *const KernelObjectPayload) bool {
    return switch (payload.*) {
        .endpoint => |endpoint_ref| blk: {
            const endpoint = self.ipcEndpointSlotConst(endpoint_ref) orelse break :blk false;
            break :blk !endpoint.queue.isEmpty();
        },
        .channel => |handle| blk: {
            if (handle.side > 1) break :blk false;
            const channel = self.ipcChannelSlotConst(handle.channel) orelse break :blk false;
            break :blk !channel.queues[@as(usize, handle.side)].isEmpty();
        },
        .reply => |reply_ref| blk: {
            const reply = self.ipcReplySlotConst(reply_ref) orelse break :blk false;
            break :blk !reply.queue.isEmpty();
        },
        else => false,
    };
}

pub fn fdIpcWritable(self: anytype, payload: *const KernelObjectPayload) bool {
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
    const table = self.getFdTable(owner) orelse return null;
    const index = table.index(fd) orelse return null;
    const entry = &table.slots()[index];
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
    for (&self.irq_publish_candidates, 0..) |*word, word_index| {
        var candidates = @atomicLoad(u64, word, .acquire);
        slots: while (candidates != 0) : (candidates &= candidates - 1) {
            const index = word_index * 64 + @ctz(candidates);
            if (index >= self.irq_publish_slots.len) continue;
            const slot = &self.irq_publish_slots[index];
            while (true) {
                const active = @atomicLoad(u8, &slot.active, .acquire);
                if (active == 0) continue :slots;
                if (active == 1 and @cmpxchgWeak(u8, &slot.active, 1, 2, .acquire, .monotonic) == null) break;
                std.atomic.spinLoopHint();
            }
            defer @atomicStore(u8, &slot.active, 1, .release);
            const generation = @atomicLoad(u32, &slot.generation, .acquire);
            const irq_device = @atomicLoad(DmaDeviceId, &slot.device, .acquire);
            const kind = @atomicLoad(u8, &slot.kind, .acquire);
            const irq_entry = @atomicLoad(u32, &slot.vector, .acquire);
            if (irq_device != device or
                !@TypeOf(self.*).irqKindMatchesInterrupt(kind, irq_entry, entry))
            {
                continue;
            }
            if (@atomicLoad(u32, &slot.generation, .acquire) != generation) continue;
            _ = @atomicRmw(u64, &slot.event_count, .Add, 1, .acq_rel);
            const owner_raw = @atomicLoad(PrincipalRaw, &slot.owner_principal_raw, .acquire);
            const owner = @TypeOf(self.*).objectOwner(owner_raw) orelse continue;
            @TypeOf(self.*).appendUniquePrincipal(wake_owners, &wake_count, owner);
        }
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
    if (mapping.device == 0 or mapping.user_va == 0 or mapping.size == 0) return KernelError.InvalidState;
    var payload = mapping;
    payload.owner_principal_raw = @intFromEnum(owner);
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
    if (!table.slots()[index].isEmpty()) return KernelError.InvalidState;
    const object_ref = try self.createKernelObject(.serial, .{ .serial = .{ .stream = stream } });
    errdefer if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
    try self.retainKernelObject(object_ref);
    table.slots()[index] = .{
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
    };
}

pub fn installFd(
    self: anytype,
    owner: PrincipalId,
    object_ref: KernelObjectRef,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    const table = try self.fdTableForActiveProcess(owner);
    const index = @TypeOf(self.*).findFreeFd(table, min_fd) orelse {
        if (!builtin.is_test and fd_full_reports < 4) {
            fd_full_reports += 1;
            var free_slots: usize = 0;
            for (table.slots()) |entry| {
                if (entry.object.isNull()) free_slots += 1;
            }
            kernel_log.writeFmt("fd: process-table-full owner={} kind={s} capacity={} free={} min={}\n", .{ @intFromEnum(owner), @tagName(object_ref.kind), table.slots().len, free_slots, min_fd });
        }
        return KernelError.TableFull;
    };
    try self.retainKernelObject(object_ref);
    table.slots()[index] = .{
        .object = object_ref,
        .rights = fdRightsFromBits(fdRightsToBits(rights)),
        .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
    };
    return @intCast(index);
}

pub fn closeFd(self: anytype, owner: PrincipalId, fd: Fd) KernelError!void {
    const table = try self.fdTableForActiveProcess(owner);
    const index = table.index(fd) orelse return KernelError.InvalidState;
    const object_ref = table.slots()[index].object;
    if (object_ref.isNull()) return KernelError.InvalidState;
    try prepareLastDmaClose(self, object_ref);
    table.slots()[index] = .{};
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
    const object_ref = table.slots()[index].object;
    if (object_ref.isNull()) return KernelError.InvalidState;
    try prepareLastDmaClose(self, object_ref);
    table.slots()[index] = .{};
    self.releaseKernelObjectWithFreeList(object_ref, free_list);
}

pub fn closeCloexecFdsWithFreeList(
    self: anytype,
    owner: PrincipalId,
    free_list: *FreePageList,
) KernelError!void {
    const table = try self.fdTableForActiveProcess(owner);
    var fd_index: usize = 0;
    while (fd_index < table.slots().len) : (fd_index += 1) {
        const entry = table.slots()[fd_index];
        if (entry.object.isNull() or !entry.flags.cloexec) continue;
        table.slots()[fd_index] = .{};
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
    const source = table.slots()[index];
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
    const source = src_table.slots()[src_index];
    if (source.object.isNull()) return KernelError.InvalidState;
    if (!source.rights.dup) return KernelError.InvalidState;
    if (!isFdRightsSubset(rights, source.rights)) return KernelError.InvalidState;

    try self.retainKernelObject(source.object);
    const dst_table = try self.fdTableForActiveProcess(owner);
    const old_object = dst_table.slots()[dst_index].object;
    dst_table.slots()[dst_index] = .{
        .object = source.object,
        .rights = fdRightsFromBits(fdRightsToBits(rights)),
        .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
    };
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
    const entry = &table.slots()[index];
    if (entry.object.isNull()) return KernelError.InvalidState;
    if (!entry.rights.set_flags) return KernelError.InvalidState;
    const old_bits = fdFlagsToBits(entry.flags);
    const mask_bits = fdFlagsToBits(mask);
    const new_bits = (old_bits & ~mask_bits) | (fdFlagsToBits(flags) & mask_bits);
    entry.flags = fdFlagsFromBits(new_bits);
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
    const source = source_table.slots()[source_index];
    if (source.object.isNull()) return KernelError.InvalidState;
    if (!source.rights.transfer) return KernelError.InvalidState;
    if (!isFdRightsSubset(rights, source.rights)) return KernelError.InvalidState;
    if (self.kernelObjectSlotConst(source.object) == null) return KernelError.InvalidState;
    if (self.kernelObjectIsPinnedUserObject(source.object)) return KernelError.InvalidState;

    const dest_table = try self.fdTableForActiveProcess(to);
    const dest_index = @TypeOf(self.*).findFreeFd(dest_table, min_fd) orelse return KernelError.TableFull;

    switch (mode) {
        .copy => {
            try self.retainKernelObject(source.object);
            dest_table.slots()[dest_index] = .{
                .object = source.object,
                .rights = fdRightsFromBits(fdRightsToBits(rights)),
                .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
            };
        },
        .move => {
            const mutable_source_table = try self.fdTableForActiveProcess(from);
            dest_table.slots()[dest_index] = .{
                .object = source.object,
                .rights = fdRightsFromBits(fdRightsToBits(rights)),
                .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
            };
            mutable_source_table.slots()[source_index] = .{};
        },
    }
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
    var free_index: ?usize = null;
    for (&self.task_fd_waiters, 0..) |*waiter, i| {
        if (!waiter.active) {
            if (free_index == null) free_index = i;
            continue;
        }
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
    const target = free_index orelse return KernelError.TableFull;
    const registration = FdWaitRegistrationRef{
        .kind = .task,
        .slot = @intCast(target),
    };
    self.task_fd_waiters[target] = .{
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
        self.task_fd_waiters[target] = .{};
        return err;
    };
    return true;
}

pub fn unregisterTaskReadableWaiterForThread(
    self: anytype,
    thread_index: usize,
    thread_generation: u32,
) void {
    if (thread_index > std.math.maxInt(u32)) return;
    const thread_index_u32: u32 = @intCast(thread_index);
    for (&self.task_fd_waiters) |*waiter| {
        if (!waiter.active or waiter.binding.completion_pending) continue;
        if (waiter.thread_index == thread_index_u32 and waiter.thread_generation == thread_generation) {
            waiter.* = .{};
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
    const fd_abi = @import("kernel_abi_root").fd_abi;
    const principal_raw: PrincipalRaw = @intFromEnum(principal);
    var count: usize = 0;
    for (&self.task_fd_waiters) |*waiter| {
        if (!waiter.active or waiter.binding.completion_pending) continue;
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
        if (count == out.len) break;
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
        waiter.binding.completion_pending = true;
        if (count < out.len) {
            out[count] = target;
            count += 1;
        }
    }
    return count;
}

pub fn takeObjectReadableWaiters(
    self: anytype,
    object: KernelObjectRef,
    out: []ThreadWakeTarget,
) usize {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    var count: usize = 0;
    for (&self.task_fd_waiters) |*waiter| {
        if (!waiter.active or waiter.binding.completion_pending) continue;
        if (waiter.object.kind != object.kind or waiter.object.index != object.index or
            waiter.object.generation != object.generation or
            (waiter.events & fd_abi.event_readable) == 0)
        {
            continue;
        }
        if (count >= out.len) break;
        out[count] = .{
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
        waiter.binding.completion_pending = true;
        count += 1;
    }
    return count;
}

pub fn takeReadyIrqWaiters(
    self: anytype,
    out: []ThreadWakeTarget,
) usize {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    var count: usize = 0;
    for (&self.task_fd_waiters) |*waiter| {
        if (!waiter.active or waiter.binding.completion_pending) continue;
        if (waiter.object.kind != .irq) continue;
        const slot = self.kernelObjectSlotConst(waiter.object) orelse continue;
        const retired = slot.payload.irq.retired;
        if (!retired and ((waiter.events & fd_abi.event_readable) == 0 or
            !(self.irqPublishedEventPending(waiter.object) orelse false))) continue;
        if (count >= out.len) break;
        out[count] = .{
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
        waiter.binding.completion_pending = true;
        count += 1;
    }
    return count;
}

pub fn takeReadyTimerWaiters(self: anytype, now_ns: u64, out: []ThreadWakeTarget) usize {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    var count: usize = 0;
    for (&self.task_fd_waiters) |*waiter| {
        if (!waiter.active or waiter.binding.completion_pending or waiter.object.kind != .timer or
            (waiter.events & fd_abi.event_readable) == 0) continue;
        const slot = self.kernelObjectSlotConst(waiter.object) orelse continue;
        if (timerDueCount(slot.payload.timer, now_ns) == 0) continue;
        if (count == out.len) break;
        out[count] = .{
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
        waiter.binding.completion_pending = true;
        count += 1;
    }
    return count;
}

pub fn nextTimerWaiterDeadline(self: anytype) ?u64 {
    var earliest: ?u64 = null;
    for (&self.task_fd_waiters) |*waiter| {
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
    if (registration.slot >= list.waiters.len) return null;
    const waiter = &list.waiters[registration.slot];
    if (!waiter.active or !waiter.registration.matches(registration)) return null;
    return waiter;
}

fn fdWaitTaskWaiterForRegistration(
    self: anytype,
    registration: FdWaitRegistrationRef,
) ?*TaskFdWaiter {
    if (registration.kind != .task or
        registration.slot >= self.task_fd_waiters.len)
        return null;
    const waiter = &self.task_fd_waiters[registration.slot];
    if (!waiter.active or !waiter.registration.matches(registration)) return null;
    return waiter;
}

fn nextFdWaitGroupGeneration(current: u32) u32 {
    const next = current +% 1;
    return if (next == 0) 1 else next;
}

fn fdWaitGroupSlot(self: anytype, group: FdWaitGroupRef) ?*types.FdWaitGroupSlot {
    if (group.isNull() or group.index >= self.fd_wait_groups.len) return null;
    const slot = &self.fd_wait_groups[group.index];
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
    var offset: usize = 0;
    while (offset < self.fd_wait_groups.len) : (offset += 1) {
        const index = (self.next_fd_wait_group_scan + offset) % self.fd_wait_groups.len;
        const slot = &self.fd_wait_groups[index];
        if (slot.state != .free) continue;
        const generation = if (slot.generation == 0) 1 else slot.generation;
        slot.* = .{
            .state = .building,
            .generation = generation,
            .owner = owner,
            .thread_index = @intCast(thread_index),
            .thread_generation = thread_generation,
            .wait_token = wait_token,
        };
        self.fd_wait_group_thread_counts[thread_index % self.fd_wait_group_thread_counts.len] += 1;
        self.next_fd_wait_group_scan = (index + 1) % self.fd_wait_groups.len;
        return .{ .index = @intCast(index), .generation = generation };
    }
    return KernelError.TableFull;
}

pub fn linkFdWaitRegistration(
    self: anytype,
    group: FdWaitGroupRef,
    registration: FdWaitRegistrationRef,
) KernelError!void {
    if (registration.isNull()) return KernelError.InvalidState;
    const slot = fdWaitGroupSlot(self, group) orelse return KernelError.InvalidState;
    if (slot.state != .building or slot.link_count >= slot.links.len)
        return KernelError.TableFull;
    const link_index = slot.link_count;
    slot.links[link_index] = registration;
    slot.link_count += 1;

    if (registration.kind == .task) {
        const waiter = fdWaitTaskWaiterForRegistration(self, registration) orelse
            return KernelError.InvalidState;
        if (!waiter.binding.group.matches(group)) return KernelError.InvalidState;
        waiter.binding.link_index = link_index;
        return;
    }
    const waiter = fdWaitIpcWaiterForRegistration(self, registration) orelse
        return KernelError.InvalidState;
    if (!waiter.binding.group.matches(group)) return KernelError.InvalidState;
    waiter.binding.link_index = link_index;
}

pub fn armFdWaitGroup(self: anytype, group: FdWaitGroupRef) KernelError!void {
    const slot = fdWaitGroupSlot(self, group) orelse return KernelError.InvalidState;
    if (slot.state != .building) return KernelError.InvalidState;
    slot.state = .armed;
}

pub fn cancelFdWaitGroup(self: anytype, group: FdWaitGroupRef, wait_token: u64) void {
    const slot = fdWaitGroupSlot(self, group) orelse return;
    if (wait_token == 0 or slot.wait_token != wait_token) return;
    const link_count: usize = slot.link_count;
    var link_index: usize = 0;
    while (link_index < link_count) : (link_index += 1) {
        const registration = slot.links[link_index];
        if (registration.kind == .task) {
            if (fdWaitTaskWaiterForRegistration(self, registration)) |waiter| {
                if (waiter.binding.wait_token == wait_token and
                    waiter.binding.group.matches(group) and
                    waiter.binding.link_index == link_index)
                {
                    waiter.* = .{};
                }
            }
            continue;
        }
        if (fdWaitIpcWaiterForRegistration(self, registration)) |waiter| {
            if (waiter.binding.wait_token == wait_token and
                waiter.binding.group.matches(group) and
                waiter.binding.link_index == link_index)
            {
                waiter.* = .{};
            }
        }
    }
    const next_generation = nextFdWaitGroupGeneration(slot.generation);
    const bucket = slot.thread_index % self.fd_wait_group_thread_counts.len;
    std.debug.assert(self.fd_wait_group_thread_counts[bucket] != 0);
    self.fd_wait_group_thread_counts[bucket] -= 1;
    slot.* = .{ .generation = next_generation };
}

pub fn cancelFdWaitGroupsForThread(
    self: anytype,
    owner: PrincipalId,
    thread_index: usize,
    thread_generation: u32,
) void {
    if (thread_index > std.math.maxInt(u32)) return;
    if (self.fd_wait_group_thread_counts[thread_index % self.fd_wait_group_thread_counts.len] == 0)
        return;
    const thread_index_u32: u32 = @intCast(thread_index);
    var index: usize = 0;
    while (index < self.fd_wait_groups.len) : (index += 1) {
        const slot = &self.fd_wait_groups[index];
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
    while (index < self.fd_wait_groups.len) : (index += 1) {
        const slot = &self.fd_wait_groups[index];
        if (slot.state == .free or slot.owner != owner) continue;
        const group = FdWaitGroupRef{
            .index = @intCast(index),
            .generation = slot.generation,
        };
        const wait_token = slot.wait_token;
        self.cancelFdWaitGroup(group, wait_token);
    }
}

pub fn snapshotFdWaitGroups(
    self: anytype,
    out: []types.FdWaitGroupSnapshot,
) usize {
    var count: usize = 0;
    for (&self.fd_wait_groups, 0..) |*slot, index| {
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
    return count;
}
