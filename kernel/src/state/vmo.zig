const std = @import("std");
const builtin = @import("builtin");
const vtd = @import("../vtd.zig");
const x86_platform = @import("../arch/x86_64/platform.zig");
const types = @import("types.zig");
const nextObjectGeneration = @import("fd.zig").nextObjectGeneration;
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
const SchedulerControlObject = types.SchedulerControlObject;
const SchedulerEventObject = types.SchedulerEventObject;
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
const max_native_cow_tables = types.max_native_cow_tables;
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

pub fn nativeVmoSlot(self: anytype, vmo_ref: NativeVmoRef) ?*NativeVmoSlot {
    if (vmo_ref.isNull()) return null;
    const index: usize = @intCast(vmo_ref.index);
    if (index >= max_native_vmos) return null;
    const slot = &self.native_vmos[index];
    if (slot.kind == .none or slot.generation != vmo_ref.generation) return null;
    return slot;
}

pub fn nativeVmoSlotConst(self: anytype, vmo_ref: NativeVmoRef) ?*const NativeVmoSlot {
    if (vmo_ref.isNull()) return null;
    const index: usize = @intCast(vmo_ref.index);
    if (index >= max_native_vmos) return null;
    const slot = &self.native_vmos[index];
    if (slot.kind == .none or slot.generation != vmo_ref.generation) return null;
    return slot;
}

pub fn clearNativeVmoSlot(slot: *NativeVmoSlot) void {
    const next_generation = nextObjectGeneration(slot.generation);
    if (slot.has_page_store and slot.page_count != 0) {
        _ = freeVmoBackingPageStore(slot.page_store_start, slot.page_count);
    }
    slot.* = .{ .generation = next_generation };
}

pub fn releaseNativeVmoOwnedPages(slot: *NativeVmoSlot, free_list: *FreePageList) void {
    if (slot.has_page_store) {
        var page_index: usize = 0;
        while (page_index < slot.page_count) : (page_index += 1) {
            const paddr = vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_index) orelse continue;
            if (paddr == 0) continue;
            if (free_list.canAppendPage(0, paddr)) {
                free_list.appendPage(0, paddr) catch {};
                _ = setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_index, 0);
            }
        }
    }
}

pub fn createNativeVmoWithPageStore(
    self: anytype,
    kind: NativeVmoKind,
    size_bytes: u64,
    allocate_page_store: bool,
) KernelError!NativeVmoRef {
    if (kind == .none or size_bytes == 0) return KernelError.InvalidState;
    const aligned_size = @TypeOf(self.*).pageAlignUp(size_bytes);
    const page_count_u64 = aligned_size / native_page_size;
    if (page_count_u64 == 0 or page_count_u64 > max_vmo_backing_pages) return KernelError.InvalidState;
    const page_count: u32 = @intCast(page_count_u64);
    var offset: usize = 0;
    while (offset < max_native_vmos) : (offset += 1) {
        const index = (self.next_native_vmo_scan + offset) % max_native_vmos;
        const slot = &self.native_vmos[index];
        if (slot.kind != .none or slot.ref_count != 0) continue;
        const page_store_start = if (allocate_page_store)
            allocEmptyVmoBackingPageStore(page_count) orelse return KernelError.TableFull
        else
            0;
        if (slot.generation == 0) slot.generation = 1;
        slot.kind = kind;
        slot.size_bytes = aligned_size;
        slot.page_count = page_count;
        slot.page_store_start = page_store_start;
        slot.has_page_store = allocate_page_store;
        slot.ref_count = 0;
        self.next_native_vmo_scan = (index + 1) % max_native_vmos;
        return .{
            .index = @intCast(index),
            .generation = slot.generation,
        };
    }
    return KernelError.TableFull;
}

pub fn createNativeVmo(self: anytype, kind: NativeVmoKind, size_bytes: u64) KernelError!NativeVmoRef {
    return self.createNativeVmoWithPageStore(kind, size_bytes, true);
}

pub fn ensureNativeVmoPageStore(slot: *NativeVmoSlot) KernelError!void {
    if (slot.has_page_store) return;
    if (slot.page_count == 0) return KernelError.InvalidState;
    slot.page_store_start = allocEmptyVmoBackingPageStore(slot.page_count) orelse return KernelError.TableFull;
    slot.has_page_store = true;
}

pub fn retainNativeVmo(self: anytype, vmo_ref: NativeVmoRef) KernelError!void {
    const slot = self.nativeVmoSlot(vmo_ref) orelse return KernelError.InvalidState;
    if (slot.ref_count == std.math.maxInt(u32)) return KernelError.TableFull;
    slot.ref_count += 1;
}

pub fn releaseNativeVmo(self: anytype, vmo_ref: NativeVmoRef) void {
    const slot = self.nativeVmoSlot(vmo_ref) orelse return;
    if (slot.ref_count == 0) return;
    slot.ref_count -= 1;
    if (slot.ref_count == 0) {
        const parent = slot.parent;
        @TypeOf(self.*).clearNativeVmoSlot(slot);
        if (!parent.isNull()) self.releaseNativeVmo(parent);
    }
}

pub fn releaseNativeVmoWithFreeList(self: anytype, vmo_ref: NativeVmoRef, free_list: *FreePageList) void {
    const slot = self.nativeVmoSlot(vmo_ref) orelse return;
    if (slot.ref_count == 0) return;
    slot.ref_count -= 1;
    if (slot.ref_count == 0) {
        const parent = slot.parent;
        @TypeOf(self.*).releaseNativeVmoOwnedPages(slot, free_list);
        @TypeOf(self.*).clearNativeVmoSlot(slot);
        if (!parent.isNull()) self.releaseNativeVmoWithFreeList(parent, free_list);
    }
}

pub fn nativeVmoRefCount(self: anytype, vmo_ref: NativeVmoRef) ?u32 {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return null;
    return slot.ref_count;
}

pub fn nativeVmoSize(self: anytype, vmo_ref: NativeVmoRef) ?u64 {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return null;
    return slot.size_bytes;
}

pub fn nativeVmoHasPageStore(self: anytype, vmo_ref: NativeVmoRef) bool {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return false;
    return slot.has_page_store;
}

pub fn nativeVmoPagePaddr(self: anytype, vmo_ref: NativeVmoRef, page_index: usize) ?u64 {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return null;
    if (!slot.has_page_store) return null;
    const paddr = vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_index) orelse return null;
    if (paddr == 0) return null;
    return paddr;
}

pub fn nativeVmoPagePaddrOrHole(self: anytype, vmo_ref: NativeVmoRef, page_index: usize) ?u64 {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return null;
    if (page_index >= slot.page_count) return null;
    if (!slot.has_page_store) return 0;
    return vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_index);
}

pub fn nativeVmoOwnPagePaddr(self: anytype, vmo_ref: NativeVmoRef, page_index: usize) ?u64 {
    const paddr = self.nativeVmoPagePaddrOrHole(vmo_ref, page_index) orelse return null;
    if (paddr == 0) return null;
    return paddr;
}

pub fn nativeVmoResolvedPagePaddr(self: anytype, vmo_ref: NativeVmoRef, page_index: usize) ?u64 {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return null;
    if (page_index >= slot.page_count) return null;
    if (slot.has_page_store) {
        const own_paddr = vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_index) orelse return null;
        if (own_paddr != 0) return own_paddr;
    }
    if (slot.parent.isNull()) return null;
    const parent_page = (slot.parent_offset / native_page_size) + @as(u64, @intCast(page_index));
    return self.nativeVmoResolvedPagePaddr(slot.parent, @intCast(parent_page));
}

pub fn nativeVmoHasParent(self: anytype, vmo_ref: NativeVmoRef) bool {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return false;
    return !slot.parent.isNull();
}

pub fn nativeVmoIsShadow(self: anytype, vmo_ref: NativeVmoRef) bool {
    return self.nativeVmoHasParent(vmo_ref);
}

pub fn nativeVmoRefsEqual(a: NativeVmoRef, b: NativeVmoRef) bool {
    return a.index == b.index and a.generation == b.generation;
}

pub fn nativeCowTableSlot(self: anytype, table_ref: NativeCowTableRef) ?*NativeCowTableSlot {
    if (table_ref.isNull()) return null;
    const index: usize = @intCast(table_ref.index);
    if (index >= max_native_cow_tables) return null;
    const slot = &self.native_cow_tables[index];
    if (!slot.active or slot.generation != table_ref.generation) return null;
    return slot;
}

pub fn nativeCowTableSlotConst(self: anytype, table_ref: NativeCowTableRef) ?*const NativeCowTableSlot {
    if (table_ref.isNull()) return null;
    const index: usize = @intCast(table_ref.index);
    if (index >= max_native_cow_tables) return null;
    const slot = &self.native_cow_tables[index];
    if (!slot.active or slot.generation != table_ref.generation) return null;
    return slot;
}

pub fn createNativeCowTable(self: anytype, page_count: u32) KernelError!NativeCowTableRef {
    if (page_count == 0) return KernelError.InvalidState;
    var offset: usize = 0;
    while (offset < max_native_cow_tables) : (offset += 1) {
        const index = (self.next_native_cow_table_scan + offset) % max_native_cow_tables;
        const slot = &self.native_cow_tables[index];
        if (slot.active or slot.ref_count != 0) continue;
        const page_store_start = allocEmptyVmoBackingPageStore(page_count) orelse return KernelError.TableFull;
        if (slot.generation == 0) slot.generation = 1;
        slot.active = true;
        slot.ref_count = 0;
        slot.page_count = page_count;
        slot.page_store_start = page_store_start;
        self.next_native_cow_table_scan = (index + 1) % max_native_cow_tables;
        return .{
            .index = @intCast(index),
            .generation = slot.generation,
        };
    }
    return KernelError.TableFull;
}

pub fn retainNativeCowTable(self: anytype, table_ref: NativeCowTableRef) KernelError!void {
    const slot = self.nativeCowTableSlot(table_ref) orelse return KernelError.InvalidState;
    if (slot.ref_count == std.math.maxInt(u32)) return KernelError.TableFull;
    slot.ref_count += 1;
}

pub fn clearNativeCowPageSlots(
    self: anytype,
    table_ref: NativeCowTableRef,
    first_page: u32,
    page_count: u32,
    free_list: ?*FreePageList,
) void {
    if (table_ref.isNull() or page_count == 0) return;
    if (first_page > std.math.maxInt(u32) - page_count) return;
    const end_page = first_page + page_count;
    const table = self.nativeCowTableSlot(table_ref) orelse return;
    if (end_page > table.page_count) return;
    var page_index: u32 = first_page;
    while (page_index < end_page) : (page_index += 1) {
        const paddr = vmoBackingPageStorePaddr(table.page_store_start, table.page_count, page_index) orelse continue;
        if (paddr == 0) continue;
        if (free_list) |fl| fl.appendPage(0, paddr) catch {};
        _ = setVmoBackingPageStorePaddr(table.page_store_start, table.page_count, page_index, 0);
    }
}

pub fn releaseNativeCowTable(self: anytype, table_ref: NativeCowTableRef, free_list: ?*FreePageList) void {
    const slot = self.nativeCowTableSlot(table_ref) orelse return;
    if (slot.ref_count == 0) return;
    slot.ref_count -= 1;
    if (slot.ref_count != 0) return;
    self.clearNativeCowPageSlots(table_ref, 0, slot.page_count, free_list);
    _ = freeVmoBackingPageStore(slot.page_store_start, slot.page_count);
    const next_generation = @TypeOf(self.*).nextObjectGeneration(slot.generation);
    slot.* = .{ .generation = next_generation };
}

pub fn nativeCowPagePaddr(self: anytype, table_ref: NativeCowTableRef, page_index: u32) ?u64 {
    const table = self.nativeCowTableSlotConst(table_ref) orelse return null;
    if (page_index >= table.page_count) return null;
    const paddr = vmoBackingPageStorePaddr(table.page_store_start, table.page_count, page_index) orelse return null;
    if (paddr == 0) return null;
    return paddr;
}

pub fn nativeCowTableIsUnique(self: anytype, table_ref: NativeCowTableRef) bool {
    const table = self.nativeCowTableSlotConst(table_ref) orelse return false;
    return table.ref_count == 1;
}

pub fn dirtyCowMappingProt(self: anytype, entry: *const VmaEntry) MapProt {
    var write = entry.prot.write;
    if (!entry.cow_table.isNull() and !self.nativeCowTableIsUnique(entry.cow_table)) {
        write = false;
    }
    return .{
        .read = entry.prot.read,
        .write = write,
        .exec = entry.prot.exec,
        .pkey = entry.prot.pkey,
    };
}

pub fn setNativeCowPagePaddr(
    self: anytype,
    table_ref: NativeCowTableRef,
    page_index: u32,
    paddr: u64,
) KernelError!void {
    if ((paddr & 0xFFF) != 0) return KernelError.InvalidState;
    const table = self.nativeCowTableSlotConst(table_ref) orelse return KernelError.InvalidState;
    if (page_index >= table.page_count) return KernelError.InvalidState;
    if (vmoBackingPageStorePaddr(table.page_store_start, table.page_count, page_index)) |existing| {
        if (existing != 0) return KernelError.InvalidState;
    } else {
        return KernelError.InvalidState;
    }
    if (!setVmoBackingPageStorePaddr(table.page_store_start, table.page_count, page_index, paddr)) {
        return KernelError.InvalidState;
    }
}

pub fn entryCowPageIndex(entry: *const VmaEntry, fault_page_va: u64) ?u32 {
    if (fault_page_va < entry.start_va or fault_page_va >= entry.endVa()) return null;
    const page_delta = (fault_page_va - entry.start_va) / native_page_size;
    const cow_page = @as(u64, entry.cow_page_offset) + page_delta;
    if (cow_page > std.math.maxInt(u32)) return null;
    return @intCast(cow_page);
}

pub fn entryDirtyPagePaddr(self: anytype, entry: *const VmaEntry, fault_page_va: u64) ?u64 {
    if (entry.cow_table.isNull()) return null;
    const cow_page = @TypeOf(self.*).entryCowPageIndex(entry, fault_page_va) orelse return null;
    return self.nativeCowPagePaddr(entry.cow_table, cow_page);
}

pub fn ensureEntryCowTable(self: anytype, entry: *VmaEntry) KernelError!void {
    if (!entry.cow_table.isNull()) return;
    const page_count_u64 = entry.size_bytes / native_page_size;
    if (page_count_u64 == 0 or page_count_u64 > std.math.maxInt(u32)) return KernelError.InvalidState;
    const table_ref = try self.createNativeCowTable(@intCast(page_count_u64));
    try self.retainNativeCowTable(table_ref);
    entry.cow_table = table_ref;
    entry.cow_page_offset = 0;
}

pub fn detachSharedEntryCowTable(self: anytype, entry: *VmaEntry, free_list: *FreePageList) KernelError!void {
    if (entry.cow_table.isNull()) return KernelError.InvalidState;
    if (self.nativeCowTableIsUnique(entry.cow_table)) return;
    const old_ref = entry.cow_table;
    const old_table = self.nativeCowTableSlotConst(old_ref) orelse return KernelError.InvalidState;
    const new_ref = try self.createNativeCowTable(old_table.page_count);
    try self.retainNativeCowTable(new_ref);
    var installed = false;
    errdefer if (!installed) self.releaseNativeCowTable(new_ref, free_list);

    var page_index: u32 = 0;
    while (page_index < old_table.page_count) : (page_index += 1) {
        const src_paddr = vmoBackingPageStorePaddr(old_table.page_store_start, old_table.page_count, page_index) orelse return KernelError.InvalidState;
        if (src_paddr == 0) continue;
        const copied_paddr = (self.allocPhysicalPage(free_list) catch return KernelError.OutOfFreePages).paddr;
        var copied_installed = false;
        defer if (!copied_installed) free_list.appendPage(0, copied_paddr) catch {};
        @TypeOf(self.*).copyPhysicalPage(copied_paddr, src_paddr);
        try self.setNativeCowPagePaddr(new_ref, page_index, copied_paddr);
        copied_installed = true;
    }

    entry.cow_table = new_ref;
    self.releaseNativeCowTable(old_ref, free_list);
    installed = true;
}

pub fn releaseUnmappedAnonymousVmoPageRange(
    self: anytype,
    owner: PrincipalId,
    vmo_ref: NativeVmoRef,
    first_page: usize,
    page_count: usize,
    free_list: *FreePageList,
) void {
    _ = owner;
    if (page_count == 0) return;
    const slot = self.nativeVmoSlot(vmo_ref) orelse return;
    if (slot.kind != .anonymous) return;
    if (first_page >= slot.page_count or page_count > @as(usize, slot.page_count) - first_page) return;
    const release_end = first_page + page_count;

    for (self.fd_objects[0..]) |object_slot| {
        if (object_slot.kind != .vmo or object_slot.ref_count == 0) continue;
        const object_vmo = switch (object_slot.payload) {
            .vmo => |object_ref| object_ref,
            else => continue,
        };
        if (@TypeOf(self.*).nativeVmoRefsEqual(object_vmo, vmo_ref)) return;
    }

    var process_index: usize = 0;
    while (process_index < self.process_capacity) : (process_index += 1) {
        const table = self.vmaTableForProcessIndexConst(process_index) orelse continue;
        var active_index: usize = 0;
        while (active_index < table.active_count) : (active_index += 1) {
            const entry_index: usize = @intCast(table.active_indices[active_index]);
            const entry = &table.entries[entry_index];
            if (!@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, vmo_ref)) continue;
            const entry_first_page: usize = @intCast(entry.vmo_offset / native_page_size);
            const entry_page_count: usize = @intCast(entry.size_bytes / native_page_size);
            const entry_end_page = entry_first_page + entry_page_count;
            if (first_page < entry_end_page and release_end > entry_first_page) return;
        }
    }

    if (!slot.has_page_store) return;
    var page_index: usize = 0;
    while (page_index < page_count) : (page_index += 1) {
        const vmo_page = first_page + page_index;
        const paddr = vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, vmo_page) orelse continue;
        if (paddr == 0) continue;
        if (free_list.canAppendPage(0, paddr)) {
            free_list.appendPage(0, paddr) catch {};
            _ = setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, vmo_page, 0);
        }
    }
}

pub fn readFdVmoBytes(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    out: []u8,
) KernelError!usize {
    if (out.len == 0) return 0;
    const table = try self.fdTableForActiveProcess(owner);
    const fd_index = @TypeOf(self.*).fdIndex(fd) orelse return KernelError.InvalidState;
    const fd_entry = table.entries[fd_index];
    if (fd_entry.object.isNull() or !fd_entry.rights.read) return KernelError.InvalidState;
    const object_slot = self.kernelObjectSlotConst(fd_entry.object) orelse return KernelError.InvalidState;
    const vmo_ref = switch (object_slot.payload) {
        .vmo => |ref| ref,
        else => return KernelError.InvalidState,
    };
    const vmo = self.nativeVmoSlotConst(vmo_ref) orelse return KernelError.InvalidState;
    if (fd_entry.offset >= vmo.size_bytes) return 0;
    const max_len: usize = @intCast(@min(@as(u64, @intCast(out.len)), vmo.size_bytes - fd_entry.offset));
    var copied: usize = 0;
    while (copied < max_len) {
        const absolute = fd_entry.offset + @as(u64, @intCast(copied));
        const page_index: usize = @intCast(absolute / native_page_size);
        const page_offset: usize = @intCast(absolute % native_page_size);
        const paddr = self.nativeVmoPagePaddr(vmo_ref, page_index) orelse return KernelError.InvalidState;
        const page: [*]const u8 = @ptrFromInt(paddr);
        const chunk = @min(max_len - copied, native_page_size - page_offset);
        @memcpy(out[copied .. copied + chunk], page[page_offset .. page_offset + chunk]);
        copied += chunk;
    }
    const update_table = try self.fdTableForActiveProcess(owner);
    update_table.entries[fd_index].offset = fd_entry.offset + @as(u64, @intCast(copied));
    return copied;
}

pub fn installNativeVmoPages(
    self: anytype,
    vmo_ref: NativeVmoRef,
    page_offset: usize,
    paddrs: []const u64,
) KernelError!void {
    if (paddrs.len == 0) return KernelError.InvalidState;
    const slot = self.nativeVmoSlot(vmo_ref) orelse return KernelError.InvalidState;
    if (page_offset > slot.page_count or paddrs.len > @as(usize, slot.page_count) - page_offset) return KernelError.InvalidState;
    for (paddrs, 0..) |paddr, i| {
        if ((paddr & 0xFFF) != 0) return KernelError.InvalidState;
        if (slot.has_page_store and vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_offset + i) != 0) return KernelError.InvalidState;
    }
    try @TypeOf(self.*).ensureNativeVmoPageStore(slot);
    for (paddrs, 0..) |paddr, i| {
        if (!setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_offset + i, paddr)) {
            return KernelError.InvalidState;
        }
    }
}

pub fn replaceNativeVmoContiguousPages(
    self: anytype,
    vmo_ref: NativeVmoRef,
    page_offset: usize,
    new_paddrs: []const u64,
) KernelError!void {
    if (new_paddrs.len == 0) return KernelError.InvalidState;
    const slot = self.nativeVmoSlot(vmo_ref) orelse return KernelError.InvalidState;
    if (page_offset > slot.page_count or new_paddrs.len > @as(usize, slot.page_count) - page_offset) return KernelError.InvalidState;
    for (new_paddrs) |paddr| {
        if ((paddr & 0xFFF) != 0) return KernelError.InvalidState;
    }
    try @TypeOf(self.*).ensureNativeVmoPageStore(slot);
    for (new_paddrs, 0..) |paddr, i| {
        if (!setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_offset + i, paddr)) {
            return KernelError.InvalidState;
        }
    }
}

pub fn nativeVmoRefForFd(self: anytype, owner: PrincipalId, fd: Fd) ?NativeVmoRef {
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    return switch (slot.payload) {
        .vmo => |vmo_ref| vmo_ref,
        else => null,
    };
}

pub fn nativeVmoRefForKernelObject(self: anytype, object_ref: KernelObjectRef) ?NativeVmoRef {
    const slot = self.kernelObjectSlotConst(object_ref) orelse return null;
    return switch (slot.payload) {
        .vmo => |vmo_ref| vmo_ref,
        else => null,
    };
}

pub fn kernelObjectMatchesNativeVmo(self: anytype, object_ref: KernelObjectRef, vmo_ref: NativeVmoRef) bool {
    const object_vmo = self.nativeVmoRefForKernelObject(object_ref) orelse return false;
    return @TypeOf(self.*).nativeVmoRefsEqual(object_vmo, vmo_ref);
}

pub fn nativeVmoRefForRevokeFd(self: anytype, owner: PrincipalId, fd: Fd) KernelError!NativeVmoRef {
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.revoke) return KernelError.InvalidState;
    return self.nativeVmoRefForKernelObject(entry.object) orelse KernelError.InvalidState;
}

pub fn revokeNativeVmoFromFdTablesWithFreeList(
    self: anytype,
    vmo_ref: NativeVmoRef,
    free_list: *FreePageList,
) void {
    var process_index: usize = 0;
    while (process_index < self.process_capacity) : (process_index += 1) {
        const table = self.fdTableForProcessIndex(process_index) orelse continue;
        var fd_index: usize = 0;
        while (fd_index < fd_table_entries) : (fd_index += 1) {
            const object_ref = table.entries[fd_index].object;
            if (object_ref.isNull()) continue;
            if (!self.kernelObjectMatchesNativeVmo(object_ref, vmo_ref)) continue;
            table.entries[fd_index] = .{};
            self.releaseKernelObjectWithFreeList(object_ref, free_list);
        }
    }
}

pub fn revokeNativeVmoFromIpcQueueWithFreeList(
    self: anytype,
    queue: *IpcQueue,
    vmo_ref: NativeVmoRef,
    free_list: *FreePageList,
) void {
    var msg_offset: usize = 0;
    while (msg_offset < queue.len) : (msg_offset += 1) {
        const msg_index = queue.slotIndex(msg_offset);
        const msg = &queue.messages[msg_index];
        var fd_index: usize = 0;
        while (fd_index < msg.fd_count and fd_index < max_ipc_message_fds) {
            const object_ref = msg.fds[fd_index].object;
            if (object_ref.isNull() or !self.kernelObjectMatchesNativeVmo(object_ref, vmo_ref)) {
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

pub fn revokeNativeVmoFromIpcMessagesWithFreeList(
    self: anytype,
    vmo_ref: NativeVmoRef,
    free_list: *FreePageList,
) void {
    for (self.ipc_endpoints[0..]) |*slot| {
        if (!slot.active) continue;
        self.revokeNativeVmoFromIpcQueueWithFreeList(&slot.queue, vmo_ref, free_list);
    }
    for (self.ipc_channels[0..]) |*slot| {
        if (!slot.active) continue;
        self.revokeNativeVmoFromIpcQueueWithFreeList(&slot.queues[0], vmo_ref, free_list);
        self.revokeNativeVmoFromIpcQueueWithFreeList(&slot.queues[1], vmo_ref, free_list);
    }
    for (self.ipc_replies[0..]) |*slot| {
        if (!slot.active) continue;
        self.revokeNativeVmoFromIpcQueueWithFreeList(&slot.queue, vmo_ref, free_list);
    }
}

pub fn revokeNativeVmoFromVmaTablesWithFreeList(
    self: anytype,
    vmo_ref: NativeVmoRef,
    free_list: *FreePageList,
    unmapper: anytype,
) KernelError!void {
    var process_index: usize = 0;
    while (process_index < self.process_capacity) : (process_index += 1) {
        const table = self.vmaTableForProcessIndex(process_index) orelse continue;
        const owner = @TypeOf(self.*).processPrincipal(process_index);
        var active_pos: usize = 0;
        while (active_pos < table.active_count) {
            const vma_index: usize = @intCast(table.active_indices[active_pos]);
            const entry = &table.entries[vma_index];
            if (!entry.active or !@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, vmo_ref)) {
                active_pos += 1;
                continue;
            }
            if (!unmapper.unmap(owner, entry.start_va, entry.size_bytes)) return KernelError.InvalidState;
            const entry_vmo = entry.vmo;
            self.releaseVmaCowResources(entry, free_list);
            @TypeOf(self.*).clearVmaEntry(table, vma_index);
            self.releaseNativeVmoWithFreeList(entry_vmo, free_list);
        }
    }
}

pub fn revokeVmoFdWithFreeList(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    free_list: *FreePageList,
    unmapper: anytype,
) KernelError!NativeVmoRef {
    const vmo_ref = try self.nativeVmoRefForRevokeFd(owner, fd);
    try self.revokeNativeVmoFromVmaTablesWithFreeList(vmo_ref, free_list, unmapper);
    self.revokeNativeVmoFromFdTablesWithFreeList(vmo_ref, free_list);
    self.revokeNativeVmoFromIpcMessagesWithFreeList(vmo_ref, free_list);
    return vmo_ref;
}
