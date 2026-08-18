const std = @import("std");
const vtd = @import("../vtd.zig");
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
const max_vmo_backing_store_free_ranges = types.max_vmo_backing_store_free_ranges;
const VmoBackingStoreFreeRange = types.VmoBackingStoreFreeRange;
const RegionFreeRange = types.RegionFreeRange;
const FreePageList = types.FreePageList;
const PageCapability = types.PageCapability;
const empty_vmo_backing_page_store = types.empty_vmo_backing_page_store;
const vmo_backing_page_store = types.vmo_backing_page_store;
const vmo_backing_page_store_next = types.vmo_backing_page_store_next;
const empty_vmo_backing_page_store_free_ranges = types.empty_vmo_backing_page_store_free_ranges;
const vmo_backing_page_store_free_ranges = types.vmo_backing_page_store_free_ranges;
const vmo_backing_page_store_free_range_len = types.vmo_backing_page_store_free_range_len;
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
const vmObjectBackingFreePageCount = types.vmObjectBackingFreePageCount;
const removeVmoBackingFreeRange = types.removeVmoBackingFreeRange;
const insertVmoBackingFreeRange = types.insertVmoBackingFreeRange;
const allocEmptyVmoBackingPageStore = types.allocEmptyVmoBackingPageStore;
const vmoBackingPageStorePaddr = types.vmoBackingPageStorePaddr;
const setVmoBackingPageStorePaddr = types.setVmoBackingPageStorePaddr;
const freeVmoBackingPageStore = types.freeVmoBackingPageStore;
const resetVmoBackingPageStore = types.resetVmoBackingPageStore;
const kernelStaticStorageEndAddr = types.kernelStaticStorageEndAddr;
const runtimeStorageBytes = types.runtimeStorageBytes;
const initRuntimeStorage = types.initRuntimeStorage;

pub fn fdIndex(fd: Fd) ?usize {
    if (fd >= fd_table_entries) return null;
    return @intCast(fd);
}

pub fn findFreeFd(table: *const FdTable, min_fd: Fd) ?usize {
    var index = fdIndex(min_fd) orelse return null;
    while (index < fd_table_entries) : (index += 1) {
        if (table.entries[index].isEmpty()) return index;
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
    @atomicStore(u8, &slot.active, 1, .release);
}

pub fn unpublishIrqObject(self: anytype, object_ref: KernelObjectRef) void {
    const slot = self.irqPublishSlotForRef(object_ref) orelse return;
    const generation = @atomicLoad(u32, &slot.generation, .acquire);
    if (generation != object_ref.generation) return;
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
    _ = @import("../memory/user_vm.zig").unmapUserLinearRegion(owner, mmio.user_va, @intCast(mmio.size));
}

pub fn releaseDmaBufferObject(self: anytype, dma: DmaBufferObject) void {
    _ = self;
    if (dma.size == 0) return;
    releaseDmaIova(dma.device, dma.iova, dma.size);
}

pub fn releaseDmaMappingObject(self: anytype, mapping: DmaMappingObject) void {
    _ = self;
    if (mapping.size == 0) return;
    releaseDmaIova(mapping.device, mapping.iova, mapping.size);
}

fn releaseDmaIova(device: DmaDeviceId, iova: u64, size: u64) void {
    if (!vtd.isActive() or size == 0) return;
    const page_size: u64 = 4096;
    const iova_base = iova & ~(page_size - 1);
    const span, const overflow = @addWithOverflow(iova - iova_base, size);
    if (overflow != 0) return;
    const aligned, const align_overflow = @addWithOverflow(span, page_size - 1);
    if (align_overflow != 0) return;
    const page_count: usize = @intCast((aligned & ~(page_size - 1)) / page_size);
    vtd.unmapRangeForDevice(device, iova, size);
    vtd.freeIova(device, iova_base, page_count);
}

pub fn releaseIrqObject(self: anytype, irq: IrqObject) void {
    _ = self;
    _ = irq;
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

pub fn clearKernelObjectSlot(self: anytype, slot: *KernelObjectSlot) void {
    self.releaseKernelObjectPayload(slot);
    slot.kind = .none;
    slot.ref_count = 0;
    slot.payload = .{ .none = {} };
    slot.generation = @TypeOf(self.*).nextObjectGeneration(slot.generation);
}

pub fn clearKernelObjectSlotWithFreeList(
    self: anytype,
    slot: *KernelObjectSlot,
    free_list: *FreePageList,
) void {
    self.releaseKernelObjectPayloadWithFreeList(slot, free_list);
    slot.kind = .none;
    slot.ref_count = 0;
    slot.payload = .{ .none = {} };
    slot.generation = @TypeOf(self.*).nextObjectGeneration(slot.generation);
}

pub fn resetKernelObjectTable(self: anytype) void {
    for (self.irq_publish_slots[0..]) |*slot| {
        @atomicStore(u8, &slot.active, 0, .release);
        slot.* = .{};
    }
    for (self.fd_objects[0..]) |*slot| {
        if (slot.kind != .none) self.releaseKernelObjectPayload(slot);
        slot.* = .{};
    }
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
        self.next_fd_object_scan = (index + 1) % max_fd_objects;
        return .{
            .kind = kind,
            .index = @intCast(index),
            .generation = slot.generation,
        };
    }
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
        for (table.entries[0..]) |*entry| {
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
    vtd.dumpRuntimeCheckpoint();
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
    const index = @TypeOf(self.*).fdIndex(fd) orelse return null;
    if (table.entries[index].isEmpty()) return null;
    return &table.entries[index];
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
            info.size_bytes = timer.deadline_tick;
            info.extra = timer.interval_ticks;
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
        while (fd_index < fd_table_entries) : (fd_index += 1) {
            const candidate = table.entries[fd_index];
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

pub fn timerDueCount(timer: TimerObject, now_tick: u64) u64 {
    if (timer.deadline_tick == 0 or now_tick < timer.deadline_tick) return 0;
    if (timer.interval_ticks == 0) return 1;
    return 1 + (now_tick - timer.deadline_tick) / timer.interval_ticks;
}

pub fn timerNextWakeTick(timer: TimerObject, now_tick: u64) ?u64 {
    if (timer.deadline_tick == 0) return null;
    if (timerDueCount(timer, now_tick) != 0) return now_tick;
    return timer.deadline_tick;
}

pub fn timerReadExpirations(self: anytype, owner: PrincipalId, fd: Fd, now_tick: u64) ?u64 {
    const view = self.fdPayloadWithRights(owner, fd, .{ .read = true }) orelse return null;
    var timer = switch (view.payload.*) {
        .timer => |timer| timer,
        else => return null,
    };
    const count = @TypeOf(self.*).timerDueCount(timer, now_tick);
    if (count == 0) return 0;
    if (timer.interval_ticks == 0) {
        timer.deadline_tick = 0;
    } else {
        timer.deadline_tick +%= count * timer.interval_ticks;
    }
    view.payload.* = .{ .timer = timer };
    return count;
}

pub fn timerFdState(self: anytype, owner: PrincipalId, fd: Fd, now_tick: u64) ?TimerFdState {
    const view = self.fdPayloadWithRightsConst(owner, fd, .{ .inspect = true }) orelse return null;
    const timer = switch (view.payload.*) {
        .timer => |timer| timer,
        else => return null,
    };
    const remaining = if (timer.deadline_tick == 0 or now_tick >= timer.deadline_tick) 0 else timer.deadline_tick - now_tick;
    return .{
        .remaining_ticks = remaining,
        .interval_ticks = timer.interval_ticks,
    };
}

pub fn setTimerFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    deadline_tick: u64,
    interval_ticks: u64,
    flags: u32,
) KernelError!void {
    const view = self.fdPayloadWithRights(owner, fd, .{ .write = true }) orelse return KernelError.InvalidState;
    var timer = switch (view.payload.*) {
        .timer => |timer| timer,
        else => return KernelError.InvalidState,
    };
    timer.deadline_tick = deadline_tick;
    timer.interval_ticks = interval_ticks;
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

pub fn fdPollEvents(self: anytype, owner: PrincipalId, fd: Fd, requested: u64, now_tick: u64) ?u64 {
    return self.fdPollEventsWithWriteMin(owner, fd, requested, now_tick, 0);
}

pub fn fdPollEventsWithWriteMin(self: anytype, owner: PrincipalId, fd: Fd, requested: u64, now_tick: u64, min_write_bytes: u64) ?u64 {
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    if (!entry.rights.poll) return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    var ready: u64 = 0;
    if ((requested & @import("kernel_abi_root").fd_abi.event_readable) != 0) {
        const readable = switch (slot.payload) {
            .endpoint, .channel, .reply => self.fdIpcReadable(&slot.payload),
            .process => |process| process.state.isTerminal(),
            .thread => |thread| thread.state.isTerminal(),
            .event => |counter| entry.rights.read and counter != 0,
            .irq => self.irqPublishedEventPending(entry.object) orelse false,
            .timer => |timer| @TypeOf(self.*).timerDueCount(timer, now_tick) != 0,
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
            .endpoint, .channel, .reply => self.fdIpcWritable(&slot.payload),
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

pub fn fdNextWakeTick(self: anytype, owner: PrincipalId, fd: Fd, now_tick: u64) ?u64 {
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    if (!entry.rights.wait and !entry.rights.poll) return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    return switch (slot.payload) {
        .timer => |timer| @TypeOf(self.*).timerNextWakeTick(timer, now_tick),
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
    const index = @TypeOf(self.*).fdIndex(fd) orelse return null;
    const entry = &table.entries[index];
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
    for (self.irq_publish_slots[0..]) |*slot| {
        if (@atomicLoad(u8, &slot.active, .acquire) == 0) continue;
        const generation = @atomicLoad(u32, &slot.generation, .acquire);
        const irq_device = @atomicLoad(DmaDeviceId, &slot.device, .acquire);
        const kind = @atomicLoad(u8, &slot.kind, .acquire);
        const irq_entry = @atomicLoad(u32, &slot.vector, .acquire);
        if (irq_device != device or
            !@TypeOf(self.*).irqKindMatchesInterrupt(kind, irq_entry, entry))
        {
            continue;
        }
        if (@atomicLoad(u8, &slot.active, .acquire) == 0) continue;
        if (@atomicLoad(u32, &slot.generation, .acquire) != generation) continue;
        _ = @atomicRmw(u64, &slot.event_count, .Add, 1, .acq_rel);
        const owner_raw = @atomicLoad(PrincipalRaw, &slot.owner_principal_raw, .acquire);
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
            slot.kind = .none;
            slot.ref_count = 0;
            slot.payload = .{ .none = {} };
            slot.generation = @TypeOf(self.*).nextObjectGeneration(slot.generation);
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
            slot.kind = .none;
            slot.ref_count = 0;
            slot.payload = .{ .none = {} };
            slot.generation = @TypeOf(self.*).nextObjectGeneration(slot.generation);
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
    deadline_tick: u64,
    interval_ticks: u64,
    flags: FdFlags,
    rights: FdRights,
    min_fd: Fd,
) KernelError!Fd {
    try self.requireActiveProcess(owner);
    const object_ref = try self.createKernelObject(.timer, .{ .timer = .{
        .owner_principal_raw = @intFromEnum(owner),
        .deadline_tick = deadline_tick,
        .interval_ticks = interval_ticks,
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
    const index = @TypeOf(self.*).fdIndex(fd) orelse return KernelError.InvalidState;
    const table = try self.fdTableForActiveProcess(owner);
    if (!table.entries[index].isEmpty()) return KernelError.InvalidState;
    const object_ref = try self.createKernelObject(.serial, .{ .serial = .{ .stream = stream } });
    errdefer if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
    try self.retainKernelObject(object_ref);
    table.entries[index] = .{
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
    const index = @TypeOf(self.*).findFreeFd(table, min_fd) orelse return KernelError.TableFull;
    try self.retainKernelObject(object_ref);
    table.entries[index] = .{
        .object = object_ref,
        .rights = fdRightsFromBits(fdRightsToBits(rights)),
        .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
    };
    return @intCast(index);
}

pub fn closeFd(self: anytype, owner: PrincipalId, fd: Fd) KernelError!void {
    const table = try self.fdTableForActiveProcess(owner);
    const index = @TypeOf(self.*).fdIndex(fd) orelse return KernelError.InvalidState;
    const object_ref = table.entries[index].object;
    if (object_ref.isNull()) return KernelError.InvalidState;
    table.entries[index] = .{};
    self.releaseKernelObject(object_ref);
}

pub fn closeFdWithFreeList(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    free_list: *FreePageList,
) KernelError!void {
    const table = try self.fdTableForActiveProcess(owner);
    const index = @TypeOf(self.*).fdIndex(fd) orelse return KernelError.InvalidState;
    const object_ref = table.entries[index].object;
    if (object_ref.isNull()) return KernelError.InvalidState;
    table.entries[index] = .{};
    self.releaseKernelObjectWithFreeList(object_ref, free_list);
}

pub fn closeCloexecFdsWithFreeList(
    self: anytype,
    owner: PrincipalId,
    free_list: *FreePageList,
) KernelError!void {
    const table = try self.fdTableForActiveProcess(owner);
    var fd_index: usize = 0;
    while (fd_index < fd_table_entries) : (fd_index += 1) {
        const entry = table.entries[fd_index];
        if (entry.object.isNull() or !entry.flags.cloexec) continue;
        table.entries[fd_index] = .{};
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
    const index = @TypeOf(self.*).fdIndex(fd) orelse return KernelError.InvalidState;
    const source = table.entries[index];
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
    const src_index = @TypeOf(self.*).fdIndex(src_fd) orelse return KernelError.InvalidState;
    const dst_index = @TypeOf(self.*).fdIndex(dst_fd) orelse return KernelError.InvalidState;
    const source = src_table.entries[src_index];
    if (source.object.isNull()) return KernelError.InvalidState;
    if (!source.rights.dup) return KernelError.InvalidState;
    if (!isFdRightsSubset(rights, source.rights)) return KernelError.InvalidState;

    try self.retainKernelObject(source.object);
    const dst_table = try self.fdTableForActiveProcess(owner);
    const old_object = dst_table.entries[dst_index].object;
    dst_table.entries[dst_index] = .{
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
    const index = @TypeOf(self.*).fdIndex(fd) orelse return KernelError.InvalidState;
    const entry = &table.entries[index];
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
    const source_index = @TypeOf(self.*).fdIndex(fd) orelse return KernelError.InvalidState;
    const source = source_table.entries[source_index];
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
            dest_table.entries[dest_index] = .{
                .object = source.object,
                .rights = fdRightsFromBits(fdRightsToBits(rights)),
                .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
            };
        },
        .move => {
            const mutable_source_table = try self.fdTableForActiveProcess(from);
            dest_table.entries[dest_index] = .{
                .object = source.object,
                .rights = fdRightsFromBits(fdRightsToBits(rights)),
                .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
            };
            mutable_source_table.entries[source_index] = .{};
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
) KernelError!bool {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    if ((requested_events & fd_abi.event_readable) == 0) return false;
    if (thread_index > std.math.maxInt(u32)) return KernelError.InvalidState;
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.poll and !entry.rights.wait) return KernelError.InvalidState;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return KernelError.InvalidState;
    const principal_raw: PrincipalRaw = switch (slot.payload) {
        .process => |process| process.principal_raw,
        .thread => |thread| thread.owner_principal_raw,
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
            waiter.principal_raw == principal_raw and
            waiter.owner == owner and
            waiter.pollfd_va == pollfd_va)
        {
            waiter.events |= requested_events;
            return true;
        }
    }
    const target = free_index orelse return KernelError.TableFull;
    self.task_fd_waiters[target] = .{
        .active = true,
        .principal_raw = principal_raw,
        .owner = owner,
        .pollfd_va = pollfd_va,
        .events = requested_events,
        .thread_index = thread_index_u32,
        .thread_generation = thread_generation,
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
        if (!waiter.active) continue;
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
    self.unregisterTaskReadableWaiterForThread(thread_index, thread_generation);
    var fd_index: usize = 0;
    while (fd_index < fd_table_entries) : (fd_index += 1) {
        const fd: Fd = @intCast(fd_index);
        if (self.fdEntryConst(owner, fd) == null) continue;
        self.unregisterPipeWaiterForFd(
            owner,
            fd,
            @import("kernel_abi_root").fd_abi.event_known_mask,
            thread_index,
            thread_generation,
        );
        self.unregisterIpcReadableWaiterForFd(
            owner,
            fd,
            @import("kernel_abi_root").fd_abi.event_readable,
            thread_index,
            thread_generation,
        );
    }
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
        if (!waiter.active) continue;
        if (waiter.principal_raw != principal_raw) continue;
        if ((waiter.events & fd_abi.event_readable) == 0) continue;
        const target = ThreadWakeTarget{
            .owner = waiter.owner,
            .thread_index = @intCast(waiter.thread_index),
            .thread_generation = waiter.thread_generation,
            .pollfd_va = waiter.pollfd_va,
            .revents = fd_abi.event_readable,
        };
        waiter.* = .{};
        if (count < out.len) {
            out[count] = target;
            count += 1;
        }
    }
    return count;
}
