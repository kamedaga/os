const std = @import("std");
const smp_perf = @import("../smp_perf.zig");
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
const native_cow_table_slots_per_chunk = types.native_cow_table_slots_per_chunk;
const NativeVmoKind = types.NativeVmoKind;
const NativeVmoRef = types.NativeVmoRef;
const NativeCowTableRef = types.NativeCowTableRef;
const NativeVmoSlot = types.NativeVmoSlot;
const NativeCowTableSlot = types.NativeCowTableSlot;
const NativeCowTableChunk = types.NativeCowTableChunk;
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
const growVmoBackingPageStore = types.growVmoBackingPageStore;
const vmoBackingStoreOwner = types.vmoBackingStoreOwner;
const vmoBackingPageStorePaddr = types.vmoBackingPageStorePaddr;
const setVmoBackingPageStorePaddr = types.setVmoBackingPageStorePaddr;
const setVmoBackingPageStorePaddrs = types.setVmoBackingPageStorePaddrs;
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
        _ = freeVmoBackingPageStore(slot.page_store_start, slot.page_count, slot.page_store_owner);
    }
    slot.* = .{ .generation = next_generation };
}

pub fn releaseNativeVmoOwnedPages(slot: *NativeVmoSlot, free_list: *FreePageList) void {
    // A page view borrows every page from its retained parent. Returning those
    // pages here would put live parent memory into the PMM a second time.
    if (slot.kind == .page_view) return;
    if (slot.has_page_store) {
        // Final release: no surviving VMA/FD can mutate this owner.
        releaseBackingRange(slot.page_store_start, slot.page_count, slot.page_store_owner, 0, slot.page_count, free_list);
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
    if (page_count_u64 == 0 or page_count_u64 > types.max_vmo_logical_pages or
        (kind != .anonymous and page_count_u64 > max_vmo_backing_pages)) return KernelError.InvalidState;
    const page_count: u32 = @intCast(page_count_u64);
    var offset: usize = 0;
    while (offset < max_native_vmos) : (offset += 1) {
        const index = (self.next_native_vmo_scan + offset) % max_native_vmos;
        const slot = &self.native_vmos[index];
        if (slot.kind != .none or slot.ref_count != 0) continue;
        if (slot.generation == 0) slot.generation = 1;
        const page_store_owner = vmoBackingStoreOwner(.native_vmo, @intCast(index), slot.generation);
        const page_store_start = if (allocate_page_store)
            allocEmptyVmoBackingPageStore(page_count, page_store_owner) orelse return KernelError.TableFull
        else
            0;
        slot.kind = kind;
        slot.size_bytes = aligned_size;
        slot.page_count = page_count;
        slot.page_store_start = page_store_start;
        slot.page_store_owner = page_store_owner;
        slot.has_page_store = allocate_page_store;
        slot.zero_on_demand = false;
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

/// Reserve a scatter view whose backing entries will be populated from one
/// anonymous parent before publication as an FD. The returned reference owns
/// one retain on itself; final release drops the parent retain but never frees
/// the borrowed physical pages.
pub fn createNativePageView(
    self: anytype,
    parent: NativeVmoRef,
    page_count: usize,
) KernelError!NativeVmoRef {
    if (page_count == 0 or page_count > max_vmo_backing_pages)
        return KernelError.InvalidState;
    const parent_slot = self.nativeVmoSlotConst(parent) orelse
        return KernelError.InvalidState;
    if (parent_slot.kind != .anonymous or !parent_slot.parent.isNull() or
        !parent_slot.has_page_store)
        return KernelError.InvalidState;

    const size_bytes = std.math.mul(u64, @intCast(page_count), native_page_size) catch
        return KernelError.InvalidState;
    const view = try self.createNativeVmo(.page_view, size_bytes);
    try self.retainNativeVmo(view);
    var keep_view = false;
    errdefer if (!keep_view) self.releaseNativeVmo(view);
    const view_slot = self.nativeVmoSlot(view) orelse
        return KernelError.InvalidState;
    try self.retainNativeVmo(parent);
    view_slot.parent = parent;
    keep_view = true;
    return view;
}

pub fn ensureNativeVmoPageStore(slot: *NativeVmoSlot) KernelError!void {
    if (slot.has_page_store) return;
    if (slot.page_count == 0) return KernelError.InvalidState;
    slot.page_store_start = allocEmptyVmoBackingPageStore(slot.page_count, slot.page_store_owner) orelse return KernelError.TableFull;
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
        smp_perf.vmoAdd(.final_drop_calls, 1);
        smp_perf.vmoAdd(.final_drop_logical_pages, slot.page_count);
        if (slot.has_page_store) smp_perf.vmoAdd(.final_drop_with_store, 1);
        var profile_stage = smp_perf.timestamp();
        @TypeOf(self.*).releaseNativeVmoOwnedPages(slot, free_list);
        smp_perf.vmoElapsed(.final_owned_cycles, profile_stage);
        profile_stage = smp_perf.timestamp();
        @TypeOf(self.*).clearNativeVmoSlot(slot);
        smp_perf.vmoElapsed(.final_clear_cycles, profile_stage);
        // Only local stages are timed; do not count recursive parent work twice.
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

/// Grow backing without replacing the object: existing shared mappings and
/// page views must keep referring to the same physical prefix. The syscall
/// caller holds the shared-VM-object lock as well as the KernelState lock.
pub fn growVmoFdWithPages(self: anytype, owner: PrincipalId, fd: Fd, new_capacity: u64, free_list: *FreePageList) KernelError!void {
    try self.requireActiveProcess(owner);
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.resize) return KernelError.InvalidState;
    const vmo_ref = self.nativeVmoRefForFd(owner, fd) orelse return KernelError.InvalidState;
    const slot = self.nativeVmoSlot(vmo_ref) orelse return KernelError.InvalidState;
    if (slot.kind != .anonymous or !slot.parent.isNull() or !slot.has_page_store or
        new_capacity == 0 or new_capacity % native_page_size != 0 or
        new_capacity < slot.size_bytes or new_capacity / native_page_size > max_vmo_backing_pages)
        return KernelError.InvalidState;
    if (new_capacity == slot.size_bytes) return;
    const old_count = slot.page_count;
    const new_count: u32 = @intCast(new_capacity / native_page_size);
    if (slot.size_bytes != @as(u64, old_count) * native_page_size)
        return KernelError.InvalidState;

    if (slot.zero_on_demand) {
        const start = growVmoBackingPageStore(slot.page_store_start, old_count, new_count, slot.page_store_owner) orelse
            return KernelError.InvalidState;
        slot.page_store_start = start;
        slot.page_count = new_count;
        slot.size_bytes = new_capacity;
        return;
    }

    // Like eager VMO_CREATE, keep unpublished allocations outside the backing
    // index until its all-or-nothing insertion succeeds. FD-backed faults do
    // not allocate holes, so merely increasing the logical size is not enough.
    var pages: [max_vmo_backing_pages]u64 = undefined;
    var allocated: usize = 0;
    errdefer {
        // Match eager VMO_CREATE's allocator contract. PMM may itself reject
        // a return (quarantine/range-table exhaustion); do not mistake this
        // best-effort resource return for rollback of the untouched old VMO.
        while (allocated != 0) {
            allocated -= 1;
            free_list.appendPage(0, pages[allocated]) catch {};
        }
    }
    const added: usize = new_count - old_count;
    while (allocated < added) : (allocated += 1) {
        pages[allocated] = (try self.allocPhysicalPage(free_list)).paddr;
    }
    try setVmoBackingPageStorePaddrs(slot.page_store_start, new_count, old_count, slot.page_store_owner, pages[0..allocated]);
    // No fallible work after publication, and no old PTE or VMA changes.
    slot.page_count = new_count;
    slot.size_bytes = new_capacity;
}

/// Extend an unaliased anonymous VMO so an adjacent mapping can share its VMA
/// entry.  A false result is a no-op; the caller must then install a separate
/// VMO/VMA as before.
pub fn growUniqueAnonymousVmo(
    self: anytype,
    vmo_ref: NativeVmoRef,
    additional_size_bytes: u64,
) bool {
    if (additional_size_bytes == 0 or
        additional_size_bytes % native_page_size != 0)
    {
        return false;
    }
    const slot = self.nativeVmoSlot(vmo_ref) orelse return false;
    if (slot.kind != .anonymous or slot.ref_count != 1 or
        !slot.parent.isNull() or !slot.has_page_store or
        slot.size_bytes % native_page_size != 0)
    {
        return false;
    }
    if (slot.size_bytes > std.math.maxInt(u64) - additional_size_bytes) {
        return false;
    }
    const new_size = slot.size_bytes + additional_size_bytes;
    const new_page_count_u64 = new_size / native_page_size;
    if (new_page_count_u64 == 0 or
        new_page_count_u64 > max_vmo_backing_pages or
        new_page_count_u64 > std.math.maxInt(u32))
    {
        return false;
    }
    const new_page_count: u32 = @intCast(new_page_count_u64);
    const new_store_start = growVmoBackingPageStore(
        slot.page_store_start,
        slot.page_count,
        new_page_count,
        slot.page_store_owner,
    );
    const committed_store_start = new_store_start orelse return false;
    slot.page_store_start = committed_store_start;
    slot.page_count = new_page_count;
    slot.size_bytes = new_size;
    return true;
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
    const paddr = self.nativeVmoResolvedPagePaddrOrZero(vmo_ref, page_index) orelse return null;
    return if (paddr == 0) null else paddr;
}

/// Zero denotes a valid anonymous hole, never an invalid reference or a hole
/// in a borrowed page view. Fault callers separately check allocation policy.
pub fn nativeVmoResolvedPagePaddrOrZero(self: anytype, vmo_ref: NativeVmoRef, page_index: usize) ?u64 {
    var current = vmo_ref;
    var page = page_index;
    for (0..max_native_vmos) |_| {
        const slot = self.nativeVmoSlotConst(current) orelse return null;
        if (page >= slot.page_count) return null;
        if (slot.has_page_store) {
            const owner = vmoBackingStoreOwner(.native_vmo, current.index, current.generation);
            if (slot.page_store_owner != owner or slot.page_store_start != owner) return null;
            const own = vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page) orelse return null;
            if (own != 0) return own;
        }
        if (slot.kind != .anonymous) return null;
        if (slot.parent.isNull()) return 0;
        if (slot.parent_offset % native_page_size != 0) return null;
        page = std.math.add(usize, @intCast(slot.parent_offset / native_page_size), page) catch return null;
        current = slot.parent;
    }
    return null;
}

pub fn nativeVmoIsZeroOnDemand(self: anytype, vmo_ref: NativeVmoRef) bool {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return false;
    return slot.kind == .anonymous and slot.parent.isNull() and slot.zero_on_demand;
}

pub fn nativeVmoHasParent(self: anytype, vmo_ref: NativeVmoRef) bool {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return false;
    return !slot.parent.isNull();
}

pub fn nativeVmoIsShadow(self: anytype, vmo_ref: NativeVmoRef) bool {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return false;
    return slot.kind == .anonymous and !slot.parent.isNull();
}

pub fn nativeVmoRefsEqual(a: NativeVmoRef, b: NativeVmoRef) bool {
    return a.index == b.index and a.generation == b.generation;
}

fn nativeCowTableStorageSlot(self: anytype, index: usize) ?*NativeCowTableSlot {
    const ordinal = index / native_cow_table_slots_per_chunk;
    const slot_index = index % native_cow_table_slots_per_chunk;
    if (ordinal >= self.native_cow_table_chunk_count) return null;
    if (ordinal == 0) return &self.native_cow_table_initial.slots[slot_index];

    var chunk = self.native_cow_table_overflow_head;
    while (chunk) |current| : (chunk = current.next) {
        if (current.ordinal == ordinal) return &current.slots[slot_index];
    }
    return null;
}

fn nativeCowTableStorageSlotConst(self: anytype, index: usize) ?*const NativeCowTableSlot {
    const ordinal = index / native_cow_table_slots_per_chunk;
    const slot_index = index % native_cow_table_slots_per_chunk;
    if (ordinal >= self.native_cow_table_chunk_count) return null;
    if (ordinal == 0) return &self.native_cow_table_initial.slots[slot_index];

    var chunk = self.native_cow_table_overflow_head;
    while (chunk) |current| : (chunk = current.next) {
        if (current.ordinal == ordinal) return &current.slots[slot_index];
    }
    return null;
}

pub fn nativeCowTableSlot(self: anytype, table_ref: NativeCowTableRef) ?*NativeCowTableSlot {
    if (table_ref.isNull()) return null;
    const slot = nativeCowTableStorageSlot(self, @intCast(table_ref.index)) orelse return null;
    if (!slot.active or slot.generation != table_ref.generation) return null;
    return slot;
}

pub fn nativeCowTableSlotConst(self: anytype, table_ref: NativeCowTableRef) ?*const NativeCowTableSlot {
    if (table_ref.isNull()) return null;
    const slot = nativeCowTableStorageSlotConst(self, @intCast(table_ref.index)) orelse return null;
    if (!slot.active or slot.generation != table_ref.generation) return null;
    return slot;
}

fn appendNativeCowTableChunk(self: anytype, free_list: *FreePageList) KernelError!void {
    // Backing-store owner tokens reserve bit 63 for the object kind, leaving
    // a full u32 generation and a u31 flat COW-table index.
    const max_chunk_count = (@as(usize, std.math.maxInt(u31)) + 1) /
        native_cow_table_slots_per_chunk;
    if (self.native_cow_table_chunk_count >= max_chunk_count) return KernelError.TableFull;
    const storage = @TypeOf(self.*).allocKernelSlice(NativeCowTableChunk, free_list, 1) orelse
        return KernelError.OutOfFreePages;
    const chunk = &storage[0];
    chunk.* = .{
        .next = self.native_cow_table_overflow_head,
        .ordinal = @intCast(self.native_cow_table_chunk_count),
    };
    self.native_cow_table_overflow_head = chunk;
    self.native_cow_table_chunk_count += 1;
}

pub fn createNativeCowTable(
    self: anytype,
    page_count: u32,
    free_list: *FreePageList,
) KernelError!NativeCowTableRef {
    if (page_count == 0) return KernelError.InvalidState;
    while (true) {
        const capacity = self.native_cow_table_chunk_count * native_cow_table_slots_per_chunk;
        var offset: usize = 0;
        while (offset < capacity) : (offset += 1) {
            const index = (self.next_native_cow_table_scan + offset) % capacity;
            if (index > std.math.maxInt(u31)) return KernelError.TableFull;
            const slot = nativeCowTableStorageSlot(self, index) orelse return KernelError.InvalidState;
            if (slot.active or slot.ref_count != 0) continue;
            if (slot.generation == 0) slot.generation = 1;
            const page_store_owner = vmoBackingStoreOwner(.native_cow, @intCast(index), slot.generation);
            // COW tables may be created directly from a page-fault path. The
            // sparse backing index has its own innermost lock, so reserving a
            // logical table does not require the global syscall-state lock.
            const page_store_start = allocEmptyVmoBackingPageStore(page_count, page_store_owner) orelse return KernelError.TableFull;
            slot.active = true;
            slot.ref_count = 0;
            slot.page_count = page_count;
            slot.page_store_start = page_store_start;
            self.next_native_cow_table_scan = (index + 1) % capacity;
            return .{
                .index = @intCast(index),
                .generation = slot.generation,
            };
        }
        try appendNativeCowTableChunk(self, free_list);
        self.next_native_cow_table_scan = capacity;
    }
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
    const owner = vmoBackingStoreOwner(.native_cow, table_ref.index, table_ref.generation);
    releaseBackingRange(table.page_store_start, table.page_count, owner, first_page, end_page, free_list);
}

// Callers hold the VM transaction and have established that no live alias
// owns this range (or that the entire VMO/COW table is on final release).
fn releaseBackingRange(start: u64, page_count: u32, owner: u64, first: usize, end: usize, free_list: ?*FreePageList) void {
    var pages: [64]types.BackedPage = undefined;
    var cursor = first;
    while (cursor < end) {
        const count = types.snapshotVmoBackingPages(start, page_count, owner, cursor, end, &pages);
        if (count == 0) break;
        cursor = @as(usize, pages[count - 1].index) + 1;
        var run_start: usize = 0;
        while (run_start < count) {
            var run_end = run_start + 1;
            if (free_list) |fl| {
                // Bound batching to this snapshot. PMM and backing-store locks
                // remain separate, and no page outside the snapshot is freed.
                while (run_end < count) : (run_end += 1) {
                    const next = std.math.add(u64, pages[run_end - 1].paddr, native_page_size) catch break;
                    if (pages[run_end].paddr != next) break;
                    // appendContiguousRange also computes the exclusive end.
                    _ = std.math.add(u64, next, native_page_size) catch break;
                }
                if (run_end - run_start > 1) {
                    if (fl.appendContiguousRange(0, pages[run_start].paddr, run_end - run_start)) |_| {
                        for (pages[run_start..run_end]) |page| {
                            setVmoBackingPageStorePaddr(start, page_count, page.index, owner, 0) catch {};
                        }
                        run_start = run_end;
                        continue;
                    } else |_| {
                        // Range insertion is atomic on failure. Preserve the
                        // old best-effort per-page fallback and backing entries
                        // for pages that could not be returned.
                    }
                }
            }
            for (pages[run_start..run_end]) |page| {
                if (free_list) |fl| fl.appendPage(0, page.paddr) catch continue;
                setVmoBackingPageStorePaddr(start, page_count, page.index, owner, 0) catch {};
            }
            run_start = run_end;
        }
    }
}

pub fn releaseNativeCowTable(self: anytype, table_ref: NativeCowTableRef, free_list: ?*FreePageList) void {
    const slot = self.nativeCowTableSlot(table_ref) orelse return;
    if (slot.ref_count == 0) return;
    slot.ref_count -= 1;
    if (slot.ref_count != 0) return;
    self.clearNativeCowPageSlots(table_ref, 0, slot.page_count, free_list);
    _ = freeVmoBackingPageStore(
        slot.page_store_start,
        slot.page_count,
        vmoBackingStoreOwner(.native_cow, table_ref.index, table_ref.generation),
    );
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
    const page_store_owner = vmoBackingStoreOwner(.native_cow, table_ref.index, table_ref.generation);
    if (page_index >= table.page_count) return KernelError.InvalidState;
    if (vmoBackingPageStorePaddr(table.page_store_start, table.page_count, page_index)) |existing| {
        if (existing != 0) return KernelError.InvalidState;
    } else {
        return KernelError.InvalidState;
    }
    try setVmoBackingPageStorePaddr(
        table.page_store_start,
        table.page_count,
        page_index,
        page_store_owner,
        paddr,
    );
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

pub fn ensureEntryCowTable(
    self: anytype,
    entry: *VmaEntry,
    free_list: *FreePageList,
) KernelError!void {
    if (!entry.cow_table.isNull()) return;
    const page_count_u64 = entry.size_bytes / native_page_size;
    if (page_count_u64 == 0 or page_count_u64 > std.math.maxInt(u32)) return KernelError.InvalidState;
    const table_ref = try self.createNativeCowTable(@intCast(page_count_u64), free_list);
    try self.retainNativeCowTable(table_ref);
    entry.cow_table = table_ref;
    entry.cow_page_offset = 0;
}

pub fn detachSharedEntryCowTable(self: anytype, entry: *VmaEntry, free_list: *FreePageList) KernelError!void {
    if (entry.cow_table.isNull()) return KernelError.InvalidState;
    if (self.nativeCowTableIsUnique(entry.cow_table)) return;
    const old_ref = entry.cow_table;
    const old_table = self.nativeCowTableSlotConst(old_ref) orelse return KernelError.InvalidState;
    const old_owner = vmoBackingStoreOwner(.native_cow, old_ref.index, old_ref.generation);
    if (old_table.page_store_start != old_owner) return KernelError.InvalidState;
    // mprotect/munmap splits retain the original table with an offset. Only
    // this VMA's slice belongs in its private snapshot; copying the whole
    // original table duplicates unrelated pages once per split at fork.
    const page_count_u64 = entry.size_bytes / native_page_size;
    if (entry.size_bytes % native_page_size != 0 or page_count_u64 == 0 or
        entry.cow_page_offset > old_table.page_count or
        page_count_u64 > old_table.page_count - entry.cow_page_offset)
        return KernelError.InvalidState;
    const page_count: u32 = @intCast(page_count_u64);
    const old_offset = entry.cow_page_offset;
    const new_ref = try self.createNativeCowTable(page_count, free_list);
    try self.retainNativeCowTable(new_ref);
    var installed = false;
    errdefer if (!installed) self.releaseNativeCowTable(new_ref, free_list);

    // Shared COW tables are immutable; every writer first detaches its slice.
    var pages: [64]types.BackedPage = undefined;
    var cursor: usize = old_offset;
    const end = @as(usize, old_offset) + page_count;
    while (cursor < end) {
        const count = types.snapshotVmoBackingPages(old_table.page_store_start, old_table.page_count, old_owner, cursor, end, &pages);
        if (count == 0) break;
        cursor = @as(usize, pages[count - 1].index) + 1;
        for (pages[0..count]) |page| {
            const copied_paddr = (self.allocPhysicalPage(free_list) catch return KernelError.OutOfFreePages).paddr;
            var copied_installed = false;
            defer if (!copied_installed) free_list.appendPage(0, copied_paddr) catch {};
            if (!@TypeOf(self.*).copyPhysicalPage(copied_paddr, page.paddr)) return KernelError.InvalidState;
            try self.setNativeCowPagePaddr(new_ref, page.index - old_offset, copied_paddr);
            copied_installed = true;
        }
    }

    entry.cow_table = new_ref;
    entry.cow_page_offset = 0;
    self.releaseNativeCowTable(old_ref, free_list);
    installed = true;
}

// A retain count alone is not proof: an FD object or page view can hold it.
// Identify the sole retain as a surviving VMA of this owner and verify its
// backing-page interval excludes the released range. The caller keeps the
// VM transaction held through page return; this does not replace TLB
// completion or change physical-page lifetime ordering.
fn soleVmaExcludesPageRange(self: anytype, owner: PrincipalId, vmo_ref: NativeVmoRef, slot: *const NativeVmoSlot, first_page: usize, release_end: usize) bool {
    if (slot.ref_count != 1) return false;
    const table = self.getVmaTableConst(owner) orelse return false;
    var found = false;
    for (table.active_indices[0..table.active_count]) |index| {
        const entry = &table.entries[index];
        if (!entry.active or !@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, vmo_ref)) continue;
        if (found) return false;
        found = true;
        if (entry.size_bytes == 0 or entry.size_bytes % native_page_size != 0 or
            entry.vmo_offset % native_page_size != 0) return false;
        const entry_first = entry.vmo_offset / native_page_size;
        const entry_count = entry.size_bytes / native_page_size;
        if (entry_first >= slot.page_count or entry_count > slot.page_count - entry_first) return false;
        if (first_page < entry_first + entry_count and release_end > entry_first) return false;
    }
    return found;
}

pub fn releaseUnmappedAnonymousVmoPageRange(
    self: anytype,
    owner: PrincipalId,
    vmo_ref: NativeVmoRef,
    first_page: usize,
    page_count: usize,
    free_list: *FreePageList,
) void {
    if (page_count == 0) return;
    const slot = self.nativeVmoSlot(vmo_ref) orelse return;
    if (slot.kind != .anonymous) return;
    if (first_page >= slot.page_count or page_count > @as(usize, slot.page_count) - first_page) return;
    const release_end = first_page + page_count;
    const sole_vma = soleVmaExcludesPageRange(self, owner, vmo_ref, slot, first_page, release_end);

    // A page view borrows parent pages without owning them. Until every view
    // is gone, keep the parent backing intact even if its FD and VMAs have
    // otherwise disappeared. This deliberately pins the whole parent: views
    // are bounded and do not form a deeper tree.
    if (!sole_vma) {
        for (self.native_vmos[0..]) |*candidate| {
            if (directPageViewOf(candidate, vmo_ref)) return;
        }
    }

    const profile_start = smp_perf.timestamp();
    defer smp_perf.vmoElapsed(.partial_total_cycles, profile_start);
    smp_perf.vmoAdd(.partial_calls, 1);
    smp_perf.vmoAdd(.partial_requested_pages, page_count);
    if (!slot.has_page_store) smp_perf.vmoAdd(.partial_no_page_store, 1);
    if (!sole_vma) {
        const profile_stage = smp_perf.timestamp();
        defer smp_perf.vmoElapsed(.partial_fd_scan_cycles, profile_stage);
        for (self.fd_objects[0..]) |object_slot| {
            if (object_slot.kind != .vmo or object_slot.ref_count == 0) continue;
            const object_vmo = switch (object_slot.payload) {
                .vmo => |object_ref| object_ref,
                else => continue,
            };
            if (@TypeOf(self.*).nativeVmoRefsEqual(object_vmo, vmo_ref)) {
                smp_perf.vmoAdd(.partial_fd_alias, 1);
                return;
            }
        }
    }

    if (!sole_vma) {
        const profile_stage = smp_perf.timestamp();
        defer smp_perf.vmoElapsed(.partial_vma_scan_cycles, profile_stage);
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
                if (first_page < entry_end_page and release_end > entry_first_page) {
                    smp_perf.vmoAdd(.partial_vma_alias, 1);
                    return;
                }
            }
        }
    }

    if (!slot.has_page_store) return;
    const profile_pages = smp_perf.timestamp();
    defer smp_perf.vmoElapsed(.partial_page_cycles, profile_pages);
    releaseBackingRange(slot.page_store_start, slot.page_count, slot.page_store_owner, first_page, release_end, free_list);
}

pub fn readFdVmoBytes(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    out: []u8,
) KernelError!usize {
    if (out.len == 0) return 0;
    const table = try self.fdTableForActiveProcess(owner);
    const fd_index = table.index(fd) orelse return KernelError.InvalidState;
    const fd_entry = table.slots()[fd_index];
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
        const paddr = self.nativeVmoResolvedPagePaddrOrZero(vmo_ref, page_index) orelse return KernelError.InvalidState;
        const chunk = @min(max_len - copied, native_page_size - page_offset);
        if (paddr == 0) {
            if (!self.nativeVmoIsZeroOnDemand(vmo_ref)) return KernelError.InvalidState;
            @memset(out[copied .. copied + chunk], 0);
        } else if (!@import("../user_copy.zig").readPhysicalBytes(paddr + page_offset, out[copied .. copied + chunk]))
            return KernelError.InvalidState;
        copied += chunk;
    }
    const update_table = try self.fdTableForActiveProcess(owner);
    update_table.slots()[fd_index].offset = fd_entry.offset + @as(u64, @intCast(copied));
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
    if (page_offset > slot.page_count or paddrs.len > @as(usize, slot.page_count) - page_offset) {
        return KernelError.InvalidState;
    }
    for (paddrs) |paddr| if ((paddr & 0xFFF) != 0) return KernelError.InvalidState;
    try @TypeOf(self.*).ensureNativeVmoPageStore(slot);
    try setVmoBackingPageStorePaddrs(
        slot.page_store_start,
        slot.page_count,
        page_offset,
        slot.page_store_owner,
        paddrs,
    );
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

pub fn nativeVmoMappingsOverlapPinnedUserObjects(
    self: anytype,
    vmo_ref: NativeVmoRef,
) bool {
    var process_index: usize = 0;
    while (process_index < self.process_capacity) : (process_index += 1) {
        const table = self.vmaTableForProcessIndexConst(process_index) orelse continue;
        const owner = @TypeOf(self.*).processPrincipal(process_index);
        var active_index: usize = 0;
        while (active_index < table.active_count) : (active_index += 1) {
            const entry_index: usize = @intCast(table.active_indices[active_index]);
            const entry = &table.entries[entry_index];
            if (!entry.active or !@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, vmo_ref)) continue;
            if (self.rangeOverlapsPinnedUserObject(owner, entry.start_va, entry.size_bytes)) return true;
        }
    }
    return false;
}

fn directPageViewOf(slot: *const NativeVmoSlot, parent: NativeVmoRef) bool {
    return slot.kind == .page_view and
        slot.parent.index == parent.index and
        slot.parent.generation == parent.generation;
}

pub fn nativeVmoTreeMappingsOverlapPinnedUserObjects(
    self: anytype,
    vmo_ref: NativeVmoRef,
) bool {
    if (self.nativeVmoMappingsOverlapPinnedUserObjects(vmo_ref)) return true;
    for (self.native_vmos[0..], 0..) |*slot, index| {
        if (!directPageViewOf(slot, vmo_ref)) continue;
        const child = NativeVmoRef{
            .index = @intCast(index),
            .generation = slot.generation,
        };
        if (self.nativeVmoMappingsOverlapPinnedUserObjects(child)) return true;
    }
    return false;
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
        while (fd_index < table.slots().len) : (fd_index += 1) {
            const object_ref = table.slots()[fd_index].object;
            if (object_ref.isNull()) continue;
            if (!self.kernelObjectMatchesNativeVmo(object_ref, vmo_ref)) continue;
            table.slots()[fd_index] = .{};
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
    // Page views cannot derive further views, so one bounded direct-child
    // scan is the complete descendant set. Revoke children before the parent:
    // each child final drop releases its parent retain.
    var child_index: usize = 0;
    while (child_index < self.native_vmos.len) : (child_index += 1) {
        const slot = &self.native_vmos[child_index];
        if (!directPageViewOf(slot, vmo_ref)) continue;
        const child = NativeVmoRef{
            .index = @intCast(child_index),
            .generation = slot.generation,
        };
        try self.revokeNativeVmoFromVmaTablesWithFreeList(child, free_list, unmapper);
        self.revokeNativeVmoFromFdTablesWithFreeList(child, free_list);
        self.revokeNativeVmoFromIpcMessagesWithFreeList(child, free_list);
        if (self.nativeVmoSlotConst(child) != null)
            return KernelError.InvalidState;
    }
    try self.revokeNativeVmoFromVmaTablesWithFreeList(vmo_ref, free_list, unmapper);
    self.revokeNativeVmoFromFdTablesWithFreeList(vmo_ref, free_list);
    self.revokeNativeVmoFromIpcMessagesWithFreeList(vmo_ref, free_list);
    return vmo_ref;
}
