const std = @import("std");
const smp_perf = @import("../smp_perf.zig");
const builtin = @import("builtin");
const x86_platform = @import("../arch/x86_64/platform.zig");
const types = @import("types.zig");
const nextObjectGeneration = @import("fd.zig").nextObjectGeneration;
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
    const leaf = @constCast(&self.vmo_directory_leaf);
    const flags = leaf.lock();
    const slot = self.native_vmos.resolve(.{ .index = vmo_ref.index, .generation = vmo_ref.generation });
    leaf.unlock(flags);
    // AS/shared-VM exclusion protects the body after the directory loan ends.
    const value = slot orelse return null;
    if (value.kind == .none or value.pending_data_return != null) return null;
    return value;
}

pub fn nativeVmoSlotConst(self: anytype, vmo_ref: NativeVmoRef) ?*const NativeVmoSlot {
    if (vmo_ref.isNull()) return null;
    const leaf = @constCast(&self.vmo_directory_leaf);
    const flags = leaf.lock();
    const slot = self.native_vmos.resolve(.{ .index = vmo_ref.index, .generation = vmo_ref.generation });
    leaf.unlock(flags);
    // AS/shared-VM exclusion protects the body after the directory loan ends.
    const value = slot orelse return null;
    if (value.kind == .none or value.pending_data_return != null) return null;
    return value;
}

pub fn clearNativeVmoSlot(self: anytype, slot: *NativeVmoSlot) void {
    types.references.Root.disconnect(&slot.parent_edge, true);
    std.debug.assert(slot.ram_cache_state == .wb);
    std.debug.assert(slot.pending_data_return == null);

    if (slot.has_page_store and slot.page_count != 0) {
        _ = freeVmoBackingPageStore(slot.page_store_start, slot.page_count, slot.page_store_owner);
    }
    if (slot.kind == .pager) _ = slot.pager_request_store.clear(slot.pager_request_start);
    self.native_vmos.remove(slot, &self.vmo_directory_leaf);
}

pub fn releaseNativeVmoOwnedPages(slot: *NativeVmoSlot, free_list: *FreePageList) void {
    // Do not make a non-WB PFN available for a fresh WB allocation. Final
    // release and partial unmap may run before outer cache retirement.
    if (slot.ram_cache_state != .wb) return;
    // A page view borrows every page from its retained parent. Returning those
    // pages here would put live parent memory into the PMM a second time.
    if (slot.kind == .page_view or slot.kind == .device_memory or slot.kind == .kernel_page) return;
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
    const page_count = try types.checkedVmoPageCount(size_bytes);
    const aligned_size = @as(u64, page_count) * native_page_size;
    const allocation = try self.allocatePool(&self.native_vmos, &self.vmo_directory_leaf, std.math.maxInt(u31));
    const index = allocation.ref.index;
    const slot = allocation.value;
    errdefer self.native_vmos.remove(slot, &self.vmo_directory_leaf);
    const page_store_owner = vmoBackingStoreOwner(.native_vmo, @intCast(index), slot.generation);
    const page_store_start = if (allocate_page_store)
        allocEmptyVmoBackingPageStore(&slot.page_store, page_count, page_store_owner) orelse return KernelError.OutOfFreePages
    else
        types.BackingRef{};
    slot.kind = kind;
    slot.size_bytes = aligned_size;
    slot.page_count = page_count;
    slot.page_store_start = page_store_start;
    slot.page_store_owner = page_store_owner;
    slot.has_page_store = allocate_page_store;
    slot.pager_request_start = if (kind == .pager) slot.pager_request_store.init(page_store_owner) else .{};
    slot.zero_on_demand = false;
    slot.ref_count = 0;
    return .{ .index = allocation.ref.index, .generation = allocation.ref.generation };
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
    if (page_count == 0 or page_count > types.max_vmo_logical_pages)
        return KernelError.InvalidState;
    const parent_slot = self.nativeVmoSlotConst(parent) orelse
        return KernelError.InvalidState;
    if (parent_slot.kind != .anonymous or !parent_slot.backing.parent.ref.isNull() or
        !parent_slot.has_page_store)
        return KernelError.InvalidState;

    const size_bytes = std.math.mul(u64, @intCast(page_count), native_page_size) catch
        return KernelError.InvalidState;
    const view = try self.createNativeVmo(.page_view, size_bytes);
    try self.retainNativeVmo(view);
    var keep_view = false;
    errdefer if (!keep_view) self.releaseNativeVmo(view);
    try self.retainNativeVmo(parent);
    errdefer self.releaseNativeVmo(parent);
    try self.publishNativeVmoParent(view, parent, 0);
    keep_view = true;
    return view;
}

/// PTE-only invalidation preserves each address reservation, VMA and its
/// protection ceiling. If any invalidation fails, previously removed PTEs
/// still fault against the OLD backing; no borrowed page may be released.
/// Caller holds K -> all-AS -> shared VM from staging through retirement.
pub fn refreshNativePageView(
    self: anytype,
    view: NativeVmoRef,
    staged: NativeVmoRef,
    invalidator: anytype,
) KernelError!void {
    if (nativeVmoRefsEqual(view, staged)) return KernelError.InvalidState;
    const target = self.nativeVmoSlot(view) orelse return KernelError.InvalidState;
    const source = self.nativeVmoSlot(staged) orelse return KernelError.InvalidState;
    if (target.kind != .page_view or source.kind != .page_view or
        !nativeVmoRefsEqual(target.backing.parent.ref, source.backing.parent.ref) or
        target.page_count != source.page_count or
        target.page_view_sparse != source.page_view_sparse or
        !std.meta.eql(target.page_view_max_prot, source.page_view_max_prot) or
        source.references.ownership().edges != 0 or source.ref_count != 1 or
        target.ram_cache_state != .wb or source.ram_cache_state != .wb or
        !target.has_page_store or !source.has_page_store)
        return KernelError.InvalidState;
    if (self.nativeVmoMappingsOverlapPinnedUserObjects(view)) return KernelError.InvalidState;
    try @import("vma.zig").validateRamAdmission(self, staged, 0, source.size_bytes, target.page_view_max_prot, .{ .shared = true });
    var cursor = types.references.Cursor.init(&target.references);
    while (cursor.next()) |edge| {
        if (edge.identity.kind != .vma) continue;
        const entry = resolveVmaEdge(self, view, edge.identity) orelse return KernelError.InvalidState;
        if (!invalidator.invalidate(@TypeOf(self.*).processPrincipal(edge.identity.owner), entry.start_va, entry.size_bytes)) return KernelError.InvalidState;
    }

    // All old aliases have acknowledged their TLB invalidation. Transfer only
    // radix contents, never lock/owner/Ref: nodes retain their issuing allocator
    // and the unpublished stage now owns retirement of the old metadata.
    // Outer lifetime exclusion prevents readers between these two leaf loans.
    target.page_store.lock.lock();
    const old_root = target.page_store.root;
    const old_used = target.page_store.used;
    target.page_store.root = source.page_store.root;
    target.page_store.used = source.page_store.used;
    target.page_store.lock.unlock();
    source.page_store.lock.lock();
    source.page_store.root = old_root;
    source.page_store.used = old_used;
    source.page_store.lock.unlock();
}

/// REVOKE alone is not authority to redirect a delegated view into arbitrary
/// parent RAM. Require the actual same-generation parent capability and its
/// full immutable MAP ceiling, not just the updater's attenuated view rights.
pub fn nativePageViewRefreshAuthority(self: anytype, owner: PrincipalId, view_fd: Fd, parent_fd: Fd) KernelError!NativeVmoRef {
    const view = try self.nativeVmoRefForRevokeFd(owner, view_fd);
    const slot = self.nativeVmoSlotConst(view) orelse return KernelError.InvalidState;
    if (slot.kind != .page_view) return KernelError.InvalidState;
    const parent = self.nativeVmoRefForFd(owner, parent_fd) orelse return KernelError.InvalidState;
    const entry = self.fdEntryConst(owner, parent_fd) orelse return KernelError.InvalidState;
    const max = slot.page_view_max_prot;
    if (!nativeVmoRefsEqual(parent, slot.backing.parent.ref) or !entry.rights.share or
        (max.read and !entry.rights.map_read) or
        (max.write and !entry.rights.map_write) or
        (max.exec and !entry.rights.map_exec)) return KernelError.InvalidState;
    return view;
}

/// Stage immutable scatter backing from a capability-authorized BAR. This
/// owns metadata only, not the BAR pages. The caller must finish cache-lease
/// admission before publication; VMA entry points currently reject this kind.
pub fn createNativeDeviceMemoryBacking(
    self: anytype,
    backing: types.DeviceMemoryBacking,
    bar_base: u64,
    bar_size: u64,
    page_offsets: []const u64,
    free_list: ?*FreePageList,
) KernelError!NativeVmoRef {
    if (backing.state != .staged or backing.wc_lease_token != 0 or
        backing.device == invalid_dma_device_id or backing.bar_index >= 6 or
        bar_base == 0 or bar_base % native_page_size != 0 or
        bar_size == 0 or bar_size % native_page_size != 0 or
        page_offsets.len == 0 or page_offsets.len > types.max_vmo_logical_pages)
        return KernelError.InvalidState;
    _ = std.math.add(u64, bar_base, bar_size) catch return KernelError.InvalidState;
    for (page_offsets) |offset| {
        if (offset % native_page_size != 0 or offset >= bar_size)
            return KernelError.InvalidState;
    }
    const bytes = std.math.mul(u64, @intCast(page_offsets.len), native_page_size) catch
        return KernelError.InvalidState;
    const ref = try self.createNativeVmo(.device_memory, bytes);
    const slot = self.nativeVmoSlot(ref) orelse return KernelError.InvalidState;
    slot.backing = .{ .device = backing };
    try self.retainNativeVmo(ref);
    errdefer self.releaseNativeVmo(ref);
    for (page_offsets, 0..) |offset, index| {
        const paddr = bar_base + offset;
        try setVmoBackingPageStorePaddrs(slot.page_store_start, slot.page_count, index, slot.page_store_owner, &.{paddr}, free_list);
    }
    return ref;
}

/// Only anonymous VMOs and page views use the parent arm of the backing union.
pub fn vmoKindHasParentArm(kind: NativeVmoKind) bool {
    return kind == .anonymous or kind == .page_view;
}

/// Return the single kernel clock page VMO, creating it on first use. The
/// kernel keeps one reference forever, so the slot never reaches final release
/// and the borrowed kernel-image page is never handed to the page allocator.
/// Callers hold the KernelState and shared-VM locks.
pub fn kernelClockPageVmo(self: anytype, paddr: u64, free_list: ?*FreePageList) KernelError!NativeVmoRef {
    if (!self.kernel_clock_page_vmo.isNull()) {
        const existing = self.nativeVmoSlotConst(self.kernel_clock_page_vmo) orelse return KernelError.InvalidState;
        if (existing.kind != .kernel_page or existing.backing.kernel_page.paddr != paddr)
            return KernelError.InvalidState;
        return self.kernel_clock_page_vmo;
    }
    if (paddr == 0 or paddr % native_page_size != 0) return KernelError.InvalidState;
    const ref = try self.createNativeVmo(.kernel_page, native_page_size);
    const slot = self.nativeVmoSlot(ref) orelse return KernelError.InvalidState;
    slot.backing = .{ .kernel_page = .{ .paddr = paddr } };
    try self.retainNativeVmo(ref);
    errdefer self.releaseNativeVmo(ref);
    try setVmoBackingPageStorePaddrs(slot.page_store_start, slot.page_count, 0, slot.page_store_owner, &.{paddr}, free_list);
    slot.references.connect(&self.clock_reference, .{ .kind = .kernel }, .{}, true);
    self.kernel_clock_page_vmo = ref;
    return ref;
}

pub fn activateNativeDeviceMemory(self: anytype, ref: NativeVmoRef, pat_slot: u8, token: u64) KernelError!void {
    const slot = self.nativeVmoSlot(ref) orelse return KernelError.InvalidState;
    if (slot.kind != .device_memory or slot.ref_count != 1 or
        slot.backing.device.state != .staged or pat_slot > 7 or
        (slot.backing.device.cache == .wc) != (token != 0))
        return KernelError.InvalidState;
    slot.backing.device.pat_slot = pat_slot;
    slot.backing.device.wc_lease_token = token;
    slot.backing.device.state = .active;
}

fn retireDeviceMemory(self: anytype, slot: *NativeVmoSlot) bool {
    if (slot.kind != .device_memory or slot.backing.device.state != .active) return false;
    // Final drop may hold an address-space or shared-VM lock. Keep the
    // already allocated slot and lease; an outer lock-ordered drain retires
    // cache aliases after all PTEs and in-flight copy windows are gone.
    slot.backing.device.state = .retiring;
    self.pending_device_retirements += 1;
    return true;
}

pub fn nextDeviceMemoryRetirement(self: anytype) ?NativeVmoRef {
    if (self.pending_device_retirements == 0) return null;
    {
        var pool_iter_3 = self.native_vmos.iterator();
        while (pool_iter_3.next()) |pool_entry_3| {
            const slot = pool_entry_3.value;
            const index: usize = pool_entry_3.ref.index;
            if (slot.kind == .device_memory and slot.ref_count == 0 and
                slot.backing.device.state == .retiring)
                return .{ .index = @intCast(index), .generation = slot.generation };
        }
    }
    return null;
}

pub fn finishDeviceMemoryRetirement(self: anytype, ref: NativeVmoRef) KernelError!void {
    const slot = self.nativeVmoSlot(ref) orelse return KernelError.InvalidState;
    if (slot.kind != .device_memory or slot.ref_count != 0 or
        slot.backing.device.state != .retiring or self.pending_device_retirements == 0)
        return KernelError.InvalidState;
    // Caller has completed ordered PTE/cache retirement. Generation advances
    // only now, so a delayed retirement cannot consume a reused slot's lease.
    self.clearNativeVmoSlot(slot);
    self.pending_device_retirements -= 1;
}

const RamCacheSnapshot = @import("../memory/ram_cache_policy.zig").Snapshot;

fn cacheOwnerPresent(spans: []const @import("../memory/ram_cache_policy.zig").Span, owner: u64) bool {
    for (spans) |span| if (span.owner == owner) return true;
    return false;
}

/// Synchronize lifetime state only after a completed physical transition.
/// The supplied snapshot must be the writer's current published snapshot,
/// with all PTE/cache barriers complete. A pending owner is never releasable.
pub const RamCacheLifetime = struct {
    slot: *NativeVmoSlot,
    owner: u64,

    /// The ticket is valid only inside the same state/all-VM transaction.
    /// Prevalidation precedes physical publication, so no fallible lifetime
    /// update can strand non-WB pages in a VMO still marked ordinary WB.
    pub fn finish(self: RamCacheLifetime, snapshot: *const RamCacheSnapshot) void {
        std.debug.assert(self.slot.ref_count != 0 and self.slot.page_store_owner == self.owner);
        std.debug.assert(!cacheOwnerPresent(snapshot.pending, self.owner));
        self.slot.ram_cache_state = if (cacheOwnerPresent(snapshot.policy, self.owner)) .cached else .wb;
    }
};

pub fn prepareNativeRamCacheLifetime(self: anytype, ref: NativeVmoRef) KernelError!RamCacheLifetime {
    const slot = self.nativeVmoSlot(ref) orelse return KernelError.InvalidState;
    if (slot.kind != .anonymous or !slot.backing.parent.ref.isNull() or
        slot.ref_count == 0 or !slot.has_page_store or slot.ram_cache_state == .retiring)
        return KernelError.InvalidState;
    const owner = vmoBackingStoreOwner(.native_vmo, ref.index, ref.generation);
    if (slot.page_store_owner != owner or slot.page_store_start.owner != owner) return KernelError.InvalidState;
    return .{ .slot = slot, .owner = owner };
}

pub fn syncNativeRamCacheState(self: anytype, ref: NativeVmoRef, snapshot: *const RamCacheSnapshot) KernelError!void {
    const ticket = try self.prepareNativeRamCacheLifetime(ref);
    if (cacheOwnerPresent(snapshot.pending, ticket.owner)) return KernelError.InvalidState;
    ticket.finish(snapshot);
}

fn retireRamCache(self: anytype, slot: *NativeVmoSlot) bool {
    if (slot.ram_cache_state != .cached) return false;
    std.debug.assert(slot.kind == .anonymous and slot.backing.parent.ref.isNull() and slot.ref_count == 0);
    slot.ram_cache_state = .retiring;
    self.pending_ram_cache_retirements += 1;
    return true;
}

pub fn nextRamCacheRetirement(self: anytype) ?NativeVmoRef {
    if (self.pending_ram_cache_retirements == 0) return null;
    {
        var pool_iter_4 = self.native_vmos.iterator();
        while (pool_iter_4.next()) |pool_entry_4| {
            const slot = pool_entry_4.value;
            const index: usize = pool_entry_4.ref.index;
            if (slot.ram_cache_state == .retiring and slot.ref_count == 0)
                return .{ .index = @intCast(index), .generation = slot.generation };
        }
    }
    return null;
}

/// Outer lock-ordered caller must have revoked DMA access and completed WB
/// restoration, copy-window retirement and acknowledged CPU cache barriers.
/// Failed restoration leaves both the slot and its physical pages retained.
pub fn finishRamCacheRetirement(self: anytype, ref: NativeVmoRef, snapshot: *const RamCacheSnapshot, free_list: *FreePageList) KernelError!void {
    const slot = self.nativeVmoSlot(ref) orelse return KernelError.InvalidState;
    if (slot.kind != .anonymous or !slot.backing.parent.ref.isNull() or
        slot.ref_count != 0 or slot.ram_cache_state != .retiring or
        self.pending_ram_cache_retirements == 0) return KernelError.InvalidState;
    const owner = vmoBackingStoreOwner(.native_vmo, ref.index, ref.generation);
    if (slot.page_store_owner != owner or cacheOwnerPresent(snapshot.policy, owner) or
        cacheOwnerPresent(snapshot.pending, owner) or
        self.nativeVmoTreeMappingsOverlapPinnedUserObjects(ref)) return KernelError.InvalidState;
    slot.ram_cache_state = .wb;
    @TypeOf(self.*).releaseNativeVmoOwnedPages(slot, free_list);
    var remaining: [1]types.BackedPage = undefined;
    if (types.snapshotVmoBackingPages(slot.page_store_start, slot.page_count, owner, 0, slot.page_count, &remaining) != 0) {
        // PMM insertion can fail independently of successful WB restoration.
        // Keep the zero-ref owner for retry, never discard its remaining PFNs.
        slot.ram_cache_state = .retiring;
        return KernelError.PageQuarantined;
    }
    self.clearNativeVmoSlot(slot);
    self.pending_ram_cache_retirements -= 1;
}

pub fn retiredRamCacheOwner(self: anytype, ref: NativeVmoRef) KernelError!u64 {
    const slot = self.nativeVmoSlotConst(ref) orelse return KernelError.InvalidState;
    const owner = vmoBackingStoreOwner(.native_vmo, ref.index, ref.generation);
    if (slot.kind != .anonymous or !slot.backing.parent.ref.isNull() or !slot.has_page_store or
        slot.ref_count != 0 or slot.ram_cache_state != .retiring or
        slot.page_store_owner != owner or slot.page_store_start.owner != owner or
        self.pending_ram_cache_retirements == 0 or self.nativeVmoTreeMappingsOverlapPinnedUserObjects(ref))
        return KernelError.InvalidState;
    return owner;
}

pub fn validateRetiredRamCachePages(self: anytype, ref: NativeVmoRef, pages: []const u64, seen: []bool) KernelError!u64 {
    const owner = try self.retiredRamCacheOwner(ref);
    @import("../memory/ram_cache_paging.zig").validatePages(pages, @as(u64, 1) << 52) catch return KernelError.InvalidState;
    if (seen.len != pages.len) return KernelError.InvalidState;
    @memset(seen, false);
    const slot = self.nativeVmoSlotConst(ref).?;
    // Scan backing once with binary lookup, rather than rescanning a large
    // Linux RAM VMO separately for every cached PFN.
    for (0..slot.page_count) |index| {
        const pa = self.nativeVmoOwnPagePaddr(ref, index) orelse continue;
        var low: usize = 0;
        var high = pages.len;
        while (low < high) {
            const mid = low + (high - low) / 2;
            if (pages[mid] < pa) low = mid + 1 else high = mid;
        }
        if (low < pages.len and pages[low] == pa) seen[low] = true;
    }
    for (seen) |found| if (!found) return KernelError.InvalidState;
    return owner;
}

pub fn deviceMemoryPhysicalBounds(self: anytype, ref: NativeVmoRef) ?struct { base: u64, size: u64 } {
    const slot = self.nativeVmoSlotConst(ref) orelse return null;
    if (slot.kind != .device_memory or slot.page_count == 0) return null;
    var base: u64 = std.math.maxInt(u64);
    var end: u64 = 0;
    for (0..slot.page_count) |index| {
        const paddr = self.nativeVmoPagePaddr(ref, index) orelse return null;
        base = @min(base, paddr);
        end = @max(end, std.math.add(u64, paddr, native_page_size) catch return null);
    }
    return .{ .base = base, .size = end - base };
}

pub fn nativeVmoDevicePatSlot(self: anytype, ref: NativeVmoRef) ?u3 {
    const slot = self.nativeVmoSlotConst(ref) orelse return null;
    if (slot.kind != .device_memory or slot.backing.device.state != .active) return null;
    return @intCast(slot.backing.device.pat_slot);
}

pub fn deviceMemoryCacheConflict(self: anytype, base: u64, size: u64, wc: bool) bool {
    if (size == 0) return true;
    const end = std.math.add(u64, base, size) catch return true;
    {
        var pool_iter_5 = self.native_vmos.iterator();
        while (pool_iter_5.next()) |pool_entry_5| {
            const slot = pool_entry_5.value;
            const index: usize = pool_entry_5.ref.index;
            if (slot.kind != .device_memory or slot.backing.device.state == .staged) continue;
            if ((slot.backing.device.cache == .wc) == wc) continue;
            const range = self.deviceMemoryPhysicalBounds(.{
                .index = @intCast(index),
                .generation = slot.generation,
            }) orelse return true;
            if (base < range.base + range.size and range.base < end) return true;
        }
    }
    return false;
}

pub fn ensureNativeVmoPageStore(slot: *NativeVmoSlot) KernelError!void {
    if (slot.ram_cache_state == .retiring) return KernelError.InvalidState;
    if (slot.has_page_store) return;
    if (slot.page_count == 0) return KernelError.InvalidState;
    slot.page_store_start = allocEmptyVmoBackingPageStore(&slot.page_store, slot.page_count, slot.page_store_owner) orelse return KernelError.TableFull;
    slot.has_page_store = true;
}

pub fn retainNativeVmo(self: anytype, vmo_ref: NativeVmoRef) KernelError!void {
    const slot = self.nativeVmoSlot(vmo_ref) orelse return KernelError.InvalidState;
    if (slot.pending_data_return != null) return KernelError.InvalidState;
    if (slot.ram_cache_state == .retiring) return KernelError.InvalidState;
    if (slot.kind == .device_memory and slot.backing.device.state == .retiring)
        return KernelError.InvalidState;
    if (slot.ref_count == std.math.maxInt(u32)) return KernelError.TableFull;
    slot.references.retainTemporary();
    slot.ref_count += 1;
}

pub fn releaseNativeVmo(self: anytype, vmo_ref: NativeVmoRef) void {
    const slot = self.nativeVmoSlot(vmo_ref) orelse return;
    if (slot.ref_count == 0) return;
    slot.references.releaseTemporary();
    slot.ref_count -= 1;
    if (slot.ref_count == 0) {
        if (retireDeviceMemory(self, slot) or retireRamCache(self, slot)) return;
        self.pagerVmoReleased(slot);
        const parent: NativeVmoRef = if (vmoKindHasParentArm(slot.kind)) slot.backing.parent.ref else .{};
        self.clearNativeVmoSlot(slot);
        if (!parent.isNull()) self.releaseNativeVmo(parent);
    }
}

pub fn releaseNativeVmoWithFreeList(self: anytype, vmo_ref: NativeVmoRef, free_list: *FreePageList) void {
    const slot = self.nativeVmoSlot(vmo_ref) orelse return;
    if (slot.ref_count == 0) return;
    slot.references.releaseTemporary();
    slot.ref_count -= 1;
    if (slot.ref_count == 0) {
        if (retireDeviceMemory(self, slot) or retireRamCache(self, slot)) return;
        self.pagerVmoReleased(slot);
        const parent: NativeVmoRef = if (vmoKindHasParentArm(slot.kind)) slot.backing.parent.ref else .{};
        smp_perf.vmoAdd(.final_drop_calls, 1);
        smp_perf.vmoAdd(.final_drop_logical_pages, slot.page_count);
        if (slot.has_page_store) smp_perf.vmoAdd(.final_drop_with_store, 1);
        var profile_stage = smp_perf.timestamp();
        const expected_owner = vmoBackingStoreOwner(.native_vmo, vmo_ref.index, vmo_ref.generation);
        // Validate the slot identity before scanning: a self-consistent but
        // foreign token must not return another live VMO's pages.
        if (vmoDataOwnerMatches(slot, expected_owner))
            @TypeOf(self.*).releaseNativeVmoOwnedPages(slot, free_list);
        smp_perf.vmoElapsed(.final_owned_cycles, profile_stage);
        if (vmoDataReturnUnfinished(slot, expected_owner)) {
            // Final drop cannot discard a failed PMM return. Keep generation,
            // kind and the parent's retain until a later successful drain.
            slot.pending_data_return = free_list;
            _ = @atomicRmw(usize, &self.pending_data_returns, .Add, 1, .release);
            return;
        }
        profile_stage = smp_perf.timestamp();
        self.clearNativeVmoSlot(slot);
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
    if (slot.kind != .anonymous or slot.ram_cache_state == .retiring or !slot.backing.parent.ref.isNull() or !slot.has_page_store or
        new_capacity == 0 or new_capacity % native_page_size != 0 or
        new_capacity < slot.size_bytes)
        return KernelError.InvalidState;
    const new_count = try types.checkedVmoPageCount(new_capacity);
    if (new_capacity == slot.size_bytes) return;
    const old_count = slot.page_count;
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
    var scratch = self.beginInvocationStorage(free_list);
    defer scratch.rollback() catch @panic("invalid invocation storage rollback");
    const pages = scratch.reserve(u64, @as(usize, new_count - old_count), .replaceable) catch return KernelError.OutOfFreePages;
    var allocated: usize = 0;
    errdefer {
        // Intrusive PMM needs no return metadata allocation. DMA-held PFNs
        // remain in the common retention ledger until device drain is proved.
        while (allocated != 0) {
            allocated -= 1;
            free_list.appendPage(pages[allocated]) catch |err| switch (err) {
                error.PageQuarantined => {},
                else => @panic("invalid PMM return"),
            };
        }
    }
    const added: usize = new_count - old_count;
    while (allocated < added) : (allocated += 1) {
        pages[allocated] = (try self.allocPhysicalPage(free_list)).paddr;
    }
    try setVmoBackingPageStorePaddrs(slot.page_store_start, new_count, old_count, slot.page_store_owner, pages[0..allocated], free_list);
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
    if (slot.kind != .anonymous or slot.ram_cache_state == .retiring or slot.ref_count != 1 or
        !slot.backing.parent.ref.isNull() or !slot.has_page_store or
        slot.size_bytes % native_page_size != 0)
    {
        return false;
    }
    if (slot.size_bytes > std.math.maxInt(u64) - additional_size_bytes) {
        return false;
    }
    const new_size = slot.size_bytes + additional_size_bytes;
    const new_page_count = types.checkedVmoPageCount(new_size) catch return false;
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

pub fn nativeVmoSparsePageViewHole(self: anytype, ref: NativeVmoRef, page: usize) bool {
    const slot = self.nativeVmoSlotConst(ref) orelse return false;
    if (slot.kind != .page_view or !slot.page_view_sparse or !slot.has_page_store or
        page >= slot.page_count) return false;
    const owner = vmoBackingStoreOwner(.native_vmo, ref.index, ref.generation);
    if (!types.vmoBackingPageStoreOwnerMatches(slot.page_store_start, slot.page_count, slot.page_store_owner, owner)) return false;
    return vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page) == @as(?u64, 0);
}

/// An unsupplied pager page has no physical alias to admit. Keep this proof
/// separate from resolution: fault callers must still request its file data
/// rather than treating the absence as an anonymous zero-fill page.
pub fn nativeVmoUnsuppliedPagerPage(self: anytype, ref: NativeVmoRef, page: usize) bool {
    const slot = self.nativeVmoSlotConst(ref) orelse return false;
    if (slot.kind != .pager or !slot.has_page_store or page >= slot.page_count) return false;
    const owner = vmoBackingStoreOwner(.native_vmo, ref.index, ref.generation);
    if (!types.vmoBackingPageStoreOwnerMatches(slot.page_store_start, slot.page_count, slot.page_store_owner, owner)) return false;
    return vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page) == @as(?u64, 0);
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
    const flags = @constCast(&self.vmo_directory_leaf).lock();
    const traversal_bound: usize = @intCast(self.native_vmos.capacity);
    @constCast(&self.vmo_directory_leaf).unlock(flags);
    for (0..traversal_bound) |_| {
        const slot = self.nativeVmoSlotConst(current) orelse return null;
        if (page >= slot.page_count) return null;
        if (slot.has_page_store) {
            const owner = vmoBackingStoreOwner(.native_vmo, current.index, current.generation);
            if (slot.page_store_owner != owner or slot.page_store_start.owner != owner) return null;
            const own = vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page) orelse return null;
            // The clock page store holds exactly the published kernel page;
            // anything else would expose unrelated kernel-image memory.
            if (slot.kind == .kernel_page and own != slot.backing.kernel_page.paddr) return null;
            if (own != 0) return own;
        }
        if (slot.kind != .anonymous) return null;
        if (slot.backing.parent.ref.isNull()) return 0;
        if (slot.backing.parent.offset % native_page_size != 0) return null;
        page = std.math.add(usize, @intCast(slot.backing.parent.offset / native_page_size), page) catch return null;
        current = slot.backing.parent.ref;
    }
    return null;
}

/// Fault commit only, under the existing AS/shared-VM lifetime exclusion.
/// Merge the previous root lookup and insert so metadata allocation does not
/// add an owner-lock acquisition. Parent locks are never nested inside it.
pub fn resolveOrInstallNativeVmoPage(self: anytype, ref: NativeVmoRef, index: u32, candidate: u64, free_list: ?*FreePageList) KernelError!u64 {
    const slot = self.nativeVmoSlot(ref) orelse return KernelError.InvalidState;
    if (slot.kind != .anonymous or slot.ram_cache_state != .wb or index >= slot.page_count or candidate == 0 or candidate & 4095 != 0)
        return KernelError.InvalidState;
    try @TypeOf(self.*).ensureNativeVmoPageStore(slot);
    const owner = vmoBackingStoreOwner(.native_vmo, ref.index, ref.generation);
    if (slot.page_store_owner != owner or slot.page_store_start.owner != owner) return KernelError.InvalidState;
    const Context = struct {
        state: @TypeOf(self),
        parent: NativeVmoRef,
        offset: u64,
        index: u32,
        fn resolve(c: @This()) types.backing_index.Error!u64 {
            if (c.offset % native_page_size != 0) return error.InvalidState;
            const page = std.math.add(usize, @intCast(c.offset / native_page_size), c.index) catch return error.InvalidState;
            return c.state.nativeVmoResolvedPagePaddrOrZero(c.parent, page) orelse error.InvalidState;
        }
    };
    return slot.page_store.resolveOrInsert(slot.page_store_start, index, candidate, types.backingAllocator(free_list) catch null, !slot.backing.parent.ref.isNull(), Context{ .state = self, .parent = slot.backing.parent.ref, .offset = slot.backing.parent.offset, .index = index }, Context.resolve) catch |err| return switch (err) {
        error.OutOfMemory => KernelError.OutOfFreePages,
        error.InvalidState => KernelError.InvalidState,
    };
}

pub fn nativeVmoIsZeroOnDemand(self: anytype, vmo_ref: NativeVmoRef) bool {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return false;
    return slot.kind == .anonymous and slot.backing.parent.ref.isNull() and slot.zero_on_demand;
}

pub fn nativeVmoIsSparsePageView(self: anytype, vmo_ref: NativeVmoRef) bool {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return false;
    return slot.kind == .page_view and slot.page_view_sparse;
}

/// Resolve canonical, already populated RAM without faulting pages in or
/// accepting authority through a borrowed scatter view. Callers additionally
/// validate FD rights, all live aliases, DMA pins and architectural policy
/// while holding the shared-VM/all-address-space transaction locks. Output is
/// scratch, not published state, and may be partially filled on failure.
pub fn nativeRamCachePages(self: anytype, vmo_ref: NativeVmoRef, first: usize, out: []u64) KernelError!u64 {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return KernelError.InvalidState;
    if (out.len == 0 or slot.kind != .anonymous or !slot.backing.parent.ref.isNull() or
        !slot.has_page_store or first >= slot.page_count or
        out.len > slot.page_count - first)
        return KernelError.InvalidState;
    const owner = vmoBackingStoreOwner(.native_vmo, vmo_ref.index, vmo_ref.generation);
    if (slot.page_store_owner != owner or slot.page_store_start.owner != owner)
        return KernelError.InvalidState;
    for (out, 0..) |*paddr, index| {
        paddr.* = vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, first + index) orelse
            return KernelError.InvalidState;
        if (paddr.* == 0) return KernelError.InvalidState;
    }
    return owner;
}

pub const RamCacheMappingError = error{ Invalid, PrivateAlias, ExecutableAlias, ShadowAlias, DmaPinned };

fn ramCacheOverlapsBlockingPin(self: anytype, principal: PrincipalId, start_va: u64, bytes: u64) bool {
    const PinRange = struct { start: u64, size: u64 };
    const end_va = std.math.add(u64, start_va, bytes) catch return true;
    const principal_raw = @intFromEnum(principal);
    var candidates = @import("vma.zig").pinnedObjectCursor(self, principal);
    while (candidates.next()) |edge| {
        const slot = self.kernelObjectSlot(.{ .kind = @enumFromInt(edge.identity.container), .index = edge.identity.index, .generation = edge.identity.generation }) orelse unreachable;
        if (slot.ref_count == 0) continue;
        const range: PinRange = switch (slot.payload) {
            .dma_mapping => continue,
            .mmio_region => |pin| if (pin.owner_principal_raw == principal_raw)
                .{ .start = pin.user_va, .size = pin.size }
            else
                continue,
            .dma_buffer => |pin| if (pin.owner_principal_raw == principal_raw)
                .{ .start = pin.user_va, .size = pin.size }
            else
                continue,
            // Unknown pinned-object kinds must not silently gain cache-policy
            // transition rights if the pinned-object index grows in future.
            else => return true,
        };
        const range_end = std.math.add(u64, range.start, range.size) catch return true;
        if (bytes != 0 and range.size != 0 and start_va < range_end and range.start < end_va) return true;
    }
    return false;
}

fn checkRamCacheMapping(self: anytype, principal: PrincipalId, entry: *const types.VmaEntry, first: u64, bytes: u64) RamCacheMappingError!void {
    if (!entry.flags.shared or entry.flags.private or !entry.cow_table.isNull()) return error.PrivateAlias;
    if (entry.prot.exec) return error.ExecutableAlias;
    // A streaming DMA mapping fixes the VA/PFN/IOVA lifetime, but does not
    // itself mean the device is accessing the pages. Linux TTM maps fresh
    // pages before changing their cache type and only binds them to the GPU
    // afterwards. Keep the general VMA mutation pin guard unchanged; cache
    // transitions still require the caller to quiesce actual device DMA.
    if (ramCacheOverlapsBlockingPin(self, principal, entry.start_va + first, bytes)) return error.DmaPinned;
}

/// Check only the requested physical pages, not every mapping of the Linux
/// RAM VMO: its unrelated code and streaming DMA lifetime pins must not block
/// a newly allocated GTT range. Page views are scatter aliases; anonymous
/// shadows can inherit the root on a later fault and are rejected on overlap.
/// Caller holds all VM locks and separately checks actual PTE/supervisor aliases.
pub fn validateRamCacheMappings(self: anytype, ref: NativeVmoRef, first_page: usize, sorted_pages: []const u64) RamCacheMappingError!void {
    const result = indexedRamCacheMappings(self, ref, first_page, sorted_pages);
    if (comptime @import("reference_check.zig").enabled) {
        const old = oldValidateRamCacheMappings(self, ref, first_page, sorted_pages);
        // Alias iteration order may select a different rejection reason;
        // acceptance must be identical.
        const accepted = if (result) |_| true else |_| false;
        const old_accepted = if (old) |_| true else |_| false;
        if (accepted != old_accepted) @import("reference_check.zig").failure(error.RamCacheMismatch);
    }
    return result;
}
// Stackless DFS uses the already retained parent links. All VM locks exclude
// publication/removal for this read, so it needs no allocation or depth-sized
// stack and visits only this VMO's descendants and reverse edges.
fn nextVmoDescendant(self: anytype, root: NativeVmoRef, current: NativeVmoRef) ?NativeVmoRef {
    var slot = self.nativeVmoSlot(current) orelse unreachable;
    if (types.references.first(&slot.references, .child)) |edge|
        return .{ .index = edge.identity.index, .generation = edge.identity.generation };
    var child = current;
    while (!nativeVmoRefsEqual(child, root)) {
        const parent = slot.backing.parent.ref;
        const parent_slot = self.nativeVmoSlot(parent) orelse unreachable;
        var siblings = types.references.Cursor.after(&parent_slot.references, &slot.parent_edge);
        while (siblings.next()) |edge| {
            if (edge.identity.kind == .child)
                return .{ .index = edge.identity.index, .generation = edge.identity.generation };
        }
        child = parent;
        slot = parent_slot;
    }
    return null;
}
fn indexedRamCacheMappings(self: anytype, ref: NativeVmoRef, first_page: usize, sorted_pages: []const u64) RamCacheMappingError!void {
    const paging = @import("../memory/ram_cache_paging.zig");
    paging.validatePages(sorted_pages, @as(u64, 1) << 52) catch return error.Invalid;
    const root = self.nativeVmoSlotConst(ref) orelse return error.Invalid;
    if (root.kind != .anonymous or !root.backing.parent.ref.isNull() or !root.has_page_store or
        root.ram_cache_state == .retiring or first_page >= root.page_count or
        sorted_pages.len > root.page_count - first_page) return error.Invalid;
    for (0..sorted_pages.len) |index| {
        const pa = self.nativeVmoOwnPagePaddr(ref, first_page + index) orelse return error.Invalid;
        if (!paging.pagesIntersect(sorted_pages, pa, 4096)) return error.Invalid;
    }
    const first_byte: u64 = @as(u64, @intCast(first_page)) * native_page_size;
    const end_byte = first_byte + @as(u64, @intCast(sorted_pages.len)) * native_page_size;
    var current: ?NativeVmoRef = ref;
    while (current) |mapped| : (current = nextVmoDescendant(self, ref, mapped)) {
        const source = self.nativeVmoSlot(mapped) orelse return error.Invalid;
        var cursor = types.references.Cursor.init(&source.references);
        while (cursor.next()) |edge| {
            if (edge.identity.kind != .vma) continue;
            const entry = resolveVmaEdge(self, mapped, edge.identity) orelse return error.Invalid;
            const principal = @TypeOf(self.*).processPrincipal(edge.identity.owner);
            const backing = self.nativeVmoSlotConst(entry.vmo) orelse return error.Invalid;
            // Neither BAR pages nor the kernel clock page take RAM cache policy.
            if (backing.kind == .device_memory or backing.kind == .kernel_page) continue;
            if (directPageViewOf(backing, ref)) {
                const start: usize = @intCast(entry.vmo_offset / native_page_size);
                const count: usize = @intCast(entry.size_bytes / native_page_size);
                for (0..count) |index| {
                    const pa = self.nativeVmoOwnPagePaddr(entry.vmo, start + index) orelse continue;
                    if (paging.pagesIntersect(sorted_pages, pa, 4096))
                        try checkRamCacheMapping(self, principal, entry, @as(u64, @intCast(index)) * native_page_size, native_page_size);
                }
                continue;
            }
            var ancestor = entry.vmo;
            var offset = entry.vmo_offset;
            var depth: usize = 0;
            while (!@TypeOf(self.*).nativeVmoRefsEqual(ancestor, ref)) {
                const slot = self.nativeVmoSlotConst(ancestor) orelse return error.Invalid;
                if (slot.kind != .anonymous or slot.backing.parent.ref.isNull()) break;
                depth += 1;
                if (depth > self.native_vmos.capacity) return error.Invalid;
                offset = std.math.add(u64, offset, slot.backing.parent.offset) catch return error.Invalid;
                ancestor = slot.backing.parent.ref;
            }
            if (!@TypeOf(self.*).nativeVmoRefsEqual(ancestor, ref)) continue;
            const mapping_end = std.math.add(u64, offset, entry.size_bytes) catch return error.Invalid;
            const overlap_start = @max(first_byte, offset);
            const overlap_end = @min(end_byte, mapping_end);
            if (overlap_start >= overlap_end) continue;
            if (depth != 0) return error.ShadowAlias;
            try checkRamCacheMapping(self, principal, entry, overlap_start - offset, overlap_end - overlap_start);
        }
    }
}

pub fn nativeVmoHasParent(self: anytype, vmo_ref: NativeVmoRef) bool {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return false;
    return vmoKindHasParentArm(slot.kind) and !slot.backing.parent.ref.isNull();
}

pub fn nativeVmoIsShadow(self: anytype, vmo_ref: NativeVmoRef) bool {
    const slot = self.nativeVmoSlotConst(vmo_ref) orelse return false;
    return slot.kind == .anonymous and !slot.backing.parent.ref.isNull();
}

pub fn nativeVmoRefsEqual(a: NativeVmoRef, b: NativeVmoRef) bool {
    return a.index == b.index and a.generation == b.generation;
}

pub fn nativeCowTableSlot(self: anytype, table_ref: NativeCowTableRef) ?*NativeCowTableSlot {
    if (table_ref.isNull()) return null;
    // Shared VM exclusion spans lookup through the final use, including fault
    // callers. Empty body reclamation therefore cannot invalidate this loan.
    const slot = self.native_cow_tables.resolve(.{ .index = table_ref.index, .generation = table_ref.generation }) orelse return null;
    if (slot.pending_data_return != null) return null;
    return slot;
}

pub fn nativeCowTableSlotConst(self: anytype, table_ref: NativeCowTableRef) ?*const NativeCowTableSlot {
    return nativeCowTableSlot(self, table_ref);
}

fn refreshCowStorageReturnHint(self: anytype) void {
    var pending = false;
    inline for (.{ &self.native_cow_tables.directory_owner, &self.native_cow_tables.metadata_owner, &self.native_cow_tables.body_owner }) |owner|
        pending = pending or owner.pending != null or owner.retired != null;
    @atomicStore(u8, &self.pending_cow_storage_return, @intFromBool(pending), .release);
}

pub fn createNativeCowTable(
    self: anytype,
    page_count: u32,
    free_list: *FreePageList,
) KernelError!NativeCowTableRef {
    _ = free_list;
    defer refreshCowStorageReturnHint(self);
    if (page_count == 0) return KernelError.InvalidState;
    // Reuse the common grant owner and persistent generation directory. The
    // backing token reserves bit 63 for kind, hence the u31 index boundary.
    const allocation = try self.allocatePool(&self.native_cow_tables, null, std.math.maxInt(u31));
    errdefer self.native_cow_tables.remove(allocation.value, null);
    const slot = allocation.value;
    const owner = vmoBackingStoreOwner(.native_cow, allocation.ref.index, slot.generation);
    const start = allocEmptyVmoBackingPageStore(&slot.page_store, page_count, owner) orelse return KernelError.TableFull;
    slot.active = true;
    slot.page_count = page_count;
    slot.page_store_start = start;
    return .{ .index = allocation.ref.index, .generation = slot.generation };
}

pub fn retainNativeCowTable(self: anytype, table_ref: NativeCowTableRef) KernelError!void {
    const slot = self.nativeCowTableSlot(table_ref) orelse return KernelError.InvalidState;
    if (slot.pending_data_return != null) return KernelError.InvalidState;
    if (slot.ref_count == std.math.maxInt(u32)) return KernelError.TableFull;
    slot.references.retainTemporary();
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
fn releaseBackingRange(start: types.BackingRef, page_count: u32, owner: u64, first: usize, end: usize, free_list: ?*FreePageList) void {
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
                    if (fl.appendContiguousRange(pages[run_start].paddr, run_end - run_start)) |_| {
                        for (pages[run_start..run_end]) |page| {
                            setVmoBackingPageStorePaddr(start, page_count, page.index, owner, 0, null) catch {};
                        }
                        run_start = run_end;
                        continue;
                    } else |err| {
                        switch (err) {
                            error.PageQuarantined => {},
                            else => @panic("invalid PMM return"),
                        }
                        // Range insertion is atomic on failure. Preserve the
                        // old best-effort per-page fallback and backing entries
                        // for pages that could not be returned.
                    }
                }
            }
            for (pages[run_start..run_end]) |page| {
                if (free_list) |fl| fl.appendPage(page.paddr) catch |err| switch (err) {
                    error.PageQuarantined => continue,
                    else => @panic("invalid PMM return"),
                };
                setVmoBackingPageStorePaddr(start, page_count, page.index, owner, 0, null) catch {};
            }
            run_start = run_end;
        }
    }
}

pub fn releaseNativeCowTable(self: anytype, table_ref: NativeCowTableRef, free_list: ?*FreePageList) void {
    defer refreshCowStorageReturnHint(self);
    const slot = self.nativeCowTableSlot(table_ref) orelse return;
    if (slot.ref_count == 0) return;
    slot.references.releaseTemporary();
    slot.ref_count -= 1;
    if (slot.ref_count != 0) return;
    self.clearNativeCowPageSlots(table_ref, 0, slot.page_count, free_list);
    const owner = vmoBackingStoreOwner(.native_cow, table_ref.index, table_ref.generation);
    if (free_list) |destination| {
        if (backingReturnUnfinished(slot.page_store_start, slot.page_count, owner)) {
            // active keeps this slot out of the allocator even at refcount 0.
            slot.pending_data_return = destination;
            _ = @atomicRmw(usize, &self.pending_data_returns, .Add, 1, .release);
            return;
        }
    }
    _ = freeVmoBackingPageStore(
        slot.page_store_start,
        slot.page_count,
        vmoBackingStoreOwner(.native_cow, table_ref.index, table_ref.generation),
    );
    self.native_cow_tables.remove(slot, null);
}

fn backingReturnUnfinished(start: types.BackingRef, count: u32, owner: u64) bool {
    var remaining: [1]types.BackedPage = undefined;
    // A rejected lookup cannot authorize discarding the last owner record.
    const found = types.snapshotVmoBackingPagesChecked(start, count, owner, 0, count, &remaining) catch return true;
    return found != 0;
}

fn vmoHasOwnedDataStore(slot: *const NativeVmoSlot) bool {
    // Views/device/kernel pages borrow their backing and must never return it.
    return slot.has_page_store and slot.kind != .page_view and
        slot.kind != .device_memory and slot.kind != .kernel_page;
}

fn vmoDataOwnerMatches(slot: *const NativeVmoSlot, expected_owner: u64) bool {
    return !vmoHasOwnedDataStore(slot) or
        types.vmoBackingPageStoreOwnerMatches(slot.page_store_start, slot.page_count, slot.page_store_owner, expected_owner);
}

fn vmoDataReturnUnfinished(slot: *const NativeVmoSlot, expected_owner: u64) bool {
    if (!vmoDataOwnerMatches(slot, expected_owner)) return true;
    return vmoHasOwnedDataStore(slot) and
        backingReturnUnfinished(slot.page_store_start, slot.page_count, expected_owner);
}

pub fn hasPendingDataReturns(self: anytype) bool {
    return types.backing_index.hasPending() or
        @import("../memory/dma_retention.zig").hasRetained() or
        @atomicLoad(usize, &self.pending_data_returns, .acquire) != 0 or
        @atomicLoad(u8, &self.pending_cow_storage_return, .acquire) != 0;
}

/// Caller holds KernelState -> all-AS -> shared-VM. Finalizers may arrive
/// from either domain, so neither lock alone excludes all pending writers.
/// One finite traversal only: rejected pages stay owned for a later call.
/// No allocator argument is accepted: retries use the failed destination,
/// whose lifetime must cover every retained slot (global PMM in production).
pub fn drainPendingDataReturns(self: anytype) void {
    @import("fd.zig").retryForcedDma(self);
    if (!self.hasPendingDataReturns()) return;
    types.reclaimVmoBackingStorage();
    self.native_cow_tables.reclaim();
    defer refreshCowStorageReturnHint(self);
    {
        var pool_iter_6 = self.native_vmos.iterator();
        while (pool_iter_6.next()) |pool_entry_6| {
            const slot = pool_entry_6.value;
            const index: usize = pool_entry_6.ref.index;
            const destination = slot.pending_data_return orelse continue;
            std.debug.assert(slot.ref_count == 0 and slot.ram_cache_state == .wb);
            const expected_owner = vmoBackingStoreOwner(.native_vmo, @intCast(index), slot.generation);
            if (!vmoDataOwnerMatches(slot, expected_owner)) continue;
            @TypeOf(self.*).releaseNativeVmoOwnedPages(slot, destination);
            if (vmoDataReturnUnfinished(slot, expected_owner)) continue;
            const parent: NativeVmoRef = if (vmoKindHasParentArm(slot.kind)) slot.backing.parent.ref else .{};
            slot.pending_data_return = null;
            self.clearNativeVmoSlot(slot);
            _ = @atomicRmw(usize, &self.pending_data_returns, .Sub, 1, .acq_rel);
            // This retained edge belongs to the retiring child, not each retry.
            if (!parent.isNull()) self.releaseNativeVmoWithFreeList(parent, destination);
        }
    }
    var cow_iterator = self.native_cow_tables.iterator();
    while (cow_iterator.next()) |entry| {
        const index = entry.ref.index;
        const slot = entry.value;
        const destination = slot.pending_data_return orelse continue;
        std.debug.assert(slot.active and slot.ref_count == 0);
        const owner = vmoBackingStoreOwner(.native_cow, @intCast(index), slot.generation);
        releaseBackingRange(slot.page_store_start, slot.page_count, owner, 0, slot.page_count, destination);
        if (backingReturnUnfinished(slot.page_store_start, slot.page_count, owner)) continue;
        _ = freeVmoBackingPageStore(slot.page_store_start, slot.page_count, owner);
        self.native_cow_tables.remove(slot, null);
        _ = @atomicRmw(usize, &self.pending_data_returns, .Sub, 1, .acq_rel);
    }
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
    free_list: ?*FreePageList,
) KernelError!void {
    if ((paddr & 0xFFF) != 0) return KernelError.InvalidState;
    const table = self.nativeCowTableSlotConst(table_ref) orelse return KernelError.InvalidState;
    const page_store_owner = vmoBackingStoreOwner(.native_cow, table_ref.index, table_ref.generation);
    if (page_index >= table.page_count) return KernelError.InvalidState;
    // Batch insertion validates absence under the publication lock, folding
    // the former separate lookup into the same transaction.
    try setVmoBackingPageStorePaddrs(table.page_store_start, table.page_count, page_index, page_store_owner, &.{paddr}, free_list);
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
    self.refreshVmaReferences(entry);
}

pub fn detachSharedEntryCowTable(self: anytype, entry: *VmaEntry, free_list: *FreePageList) KernelError!void {
    if (entry.cow_table.isNull()) return KernelError.InvalidState;
    if (self.nativeCowTableIsUnique(entry.cow_table)) return;
    const old_ref = entry.cow_table;
    const old_table = self.nativeCowTableSlotConst(old_ref) orelse return KernelError.InvalidState;
    const old_owner = vmoBackingStoreOwner(.native_cow, old_ref.index, old_ref.generation);
    if (old_table.page_store_start.owner != old_owner) return KernelError.InvalidState;
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
            defer if (!copied_installed) free_list.appendPage(copied_paddr) catch |err| switch (err) {
                error.PageQuarantined => {},
                else => @panic("invalid PMM return"),
            };
            if (!@TypeOf(self.*).copyPhysicalPage(copied_paddr, page.paddr)) return KernelError.InvalidState;
            try self.setNativeCowPagePaddr(new_ref, page.index - old_offset, copied_paddr, free_list);
            copied_installed = true;
        }
    }

    entry.cow_table = new_ref;
    entry.cow_page_offset = 0;
    self.refreshVmaReferences(entry);
    self.releaseNativeCowTable(old_ref, free_list);
    installed = true;
}

// A retain count alone is not proof: an FD object or page view can hold it.
// Identify the sole retain as a surviving VMA of this owner and verify its
// backing-page interval excludes the released range. The caller keeps the
// VM transaction held through page return; this does not replace TLB
// completion or change physical-page lifetime ordering.
fn soleVmaExcludesPageRange(self: anytype, owner: PrincipalId, vmo_ref: NativeVmoRef, slot: *const NativeVmoSlot, first_page: usize, release_end: usize) bool {
    _ = self;
    _ = vmo_ref;
    if (slot.ref_count != 1) return false;
    const root = @constCast(&slot.references);
    const counts = root.ownership();
    if (counts.edges != 1 or counts.temporary != 0) return false;
    const edge = types.references.first(root, .vma) orelse return false;
    if (edge.identity.owner != (types.processIndexFromPrincipal(owner) orelse return false)) return false;
    return !types.references.overlap(first_page * native_page_size, (release_end - first_page) * native_page_size, edge.range.offset, edge.range.size);
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
    if (slot.kind != .anonymous or slot.ram_cache_state != .wb) return;
    if (first_page >= slot.page_count or page_count > @as(usize, slot.page_count) - first_page) return;
    const release_end = first_page + page_count;
    const sole_vma = soleVmaExcludesPageRange(self, owner, vmo_ref, slot, first_page, release_end);
    const carrier = if (sole_vma) false else types.references.backingCarrier(&slot.references, first_page, page_count);
    if (comptime @import("reference_check.zig").enabled) {
        // The old search cannot see unpublished retains. Apply the explicit
        // temporary-owner safety rule to both proofs before comparing them.
        const old = oldBackingCarrier(self, owner, vmo_ref, slot, first_page, page_count) or slot.references.ownership().temporary != 0;
        if (old != carrier) @import("reference_check.zig").failure(error.PartialUnmapMismatch);
    }
    if (carrier) return;
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
    const fd_entry = table.at(fd_index).*;
    if (fd_entry.object.isNull() or !fd_entry.rights.read) return KernelError.InvalidState;
    const object_slot = self.kernelObjectSlotConst(fd_entry.object) orelse return KernelError.InvalidState;
    const vmo_ref = switch (object_slot.payload) {
        .vmo => |ref| ref,
        else => return KernelError.InvalidState,
    };
    const vmo = self.nativeVmoSlotConst(vmo_ref) orelse return KernelError.InvalidState;
    if (fd_entry.cursor.offset >= vmo.size_bytes) return 0;
    const max_len: usize = @intCast(@min(@as(u64, @intCast(out.len)), vmo.size_bytes - fd_entry.cursor.offset));
    var copied: usize = 0;
    while (copied < max_len) {
        const absolute = fd_entry.cursor.offset + @as(u64, @intCast(copied));
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
    update_table.at(fd_index).*.cursor.offset = fd_entry.cursor.offset + @as(u64, @intCast(copied));
    return copied;
}

pub fn installNativeVmoPages(
    self: anytype,
    vmo_ref: NativeVmoRef,
    page_offset: usize,
    paddrs: []const u64,
    free_list: ?*FreePageList,
) KernelError!void {
    if (paddrs.len == 0) return KernelError.InvalidState;
    const slot = self.nativeVmoSlot(vmo_ref) orelse return KernelError.InvalidState;
    if (slot.kind == .device_memory or slot.kind == .kernel_page or slot.kind == .pager or slot.ram_cache_state != .wb) return KernelError.InvalidState;
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
        free_list,
    );
}

/// The pager syscall has already checked port authority and copied every
/// source page into fresh canonical pages. The backing store verifies every
/// slot and allocates metadata before one locked, all-or-nothing publish.
pub fn publishPagerPages(self: anytype, ref: NativeVmoRef, first: usize, pages: []const u64, free_list: *FreePageList) KernelError!void {
    const slot = self.nativeVmoSlot(ref) orelse return KernelError.InvalidState;
    if (slot.kind != .pager or slot.pager_port.isNull() or pages.len == 0 or
        first > slot.page_count or pages.len > slot.page_count - first or slot.ram_cache_state != .wb)
        return KernelError.InvalidState;
    for (pages) |paddr| if (paddr == 0 or paddr & 4095 != 0) return KernelError.InvalidState;
    try setVmoBackingPageStorePaddrs(slot.page_store_start, slot.page_count, first, slot.page_store_owner, pages, free_list);
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

pub fn nativeVmoMappingsOverlapPinnedUserObjects(self: anytype, vmo_ref: NativeVmoRef) bool {
    const result = indexedMappingsPinned(self, vmo_ref);
    if (comptime @import("reference_check.zig").enabled) {
        if (result != oldNativeVmoMappingsOverlapPinnedUserObjects(self, vmo_ref))
            @import("reference_check.zig").failure(error.PinnedMismatch);
    }
    return result;
}
fn indexedMappingsPinned(self: anytype, vmo_ref: NativeVmoRef) bool {
    const slot = self.nativeVmoSlot(vmo_ref) orelse return false;
    var cursor = types.references.Cursor.init(&slot.references);
    while (cursor.next()) |edge| {
        if (edge.identity.kind != .vma) continue;
        const entry = resolveVmaEdge(self, vmo_ref, edge.identity) orelse return true;
        if (self.rangeOverlapsPinnedUserObject(@TypeOf(self.*).processPrincipal(edge.identity.owner), entry.start_va, entry.size_bytes)) return true;
    }
    return false;
}
// The caller's outer VM transaction preserves the source between the leaf
// snapshot and this generation check. No source lookup occurs under leaf.
fn resolveVmaEdge(self: anytype, ref: NativeVmoRef, id: types.references.Identity) ?*types.VmaEntry {
    const desc = self.processDescriptorSlotConst(id.owner) orelse return null;
    if (desc.generation != id.owner_generation) return null;
    const table = self.vmaTableForProcessIndex(id.owner) orelse return null;
    const entry = table.entries.resolve(.{ .index = id.index, .generation = id.generation }) orelse return null;
    if (!entry.active or !nativeVmoRefsEqual(entry.vmo, ref)) return null;
    return entry;
}

fn directPageViewOf(slot: *const NativeVmoSlot, parent: NativeVmoRef) bool {
    return slot.kind == .page_view and
        slot.backing.parent.ref.index == parent.index and
        slot.backing.parent.ref.generation == parent.generation;
}

pub fn nativeVmoTreeMappingsOverlapPinnedUserObjects(
    self: anytype,
    vmo_ref: NativeVmoRef,
) bool {
    if (self.nativeVmoMappingsOverlapPinnedUserObjects(vmo_ref)) return true;
    const root = self.nativeVmoSlot(vmo_ref) orelse return false;
    var cursor = types.references.Cursor.init(&root.references);
    while (cursor.next()) |edge| {
        if (edge.identity.kind != .child) continue;
        const child: NativeVmoRef = .{ .index = edge.identity.index, .generation = edge.identity.generation };
        const slot = self.nativeVmoSlotConst(child) orelse return true;
        if (directPageViewOf(slot, vmo_ref) and self.nativeVmoMappingsOverlapPinnedUserObjects(child)) return true;
    }
    return false;
}

fn revokeVmoObjectHolders(self: anytype, vmo_ref: NativeVmoRef, free_list: *FreePageList, kind: types.references.Kind) void {
    const slot = self.nativeVmoSlot(vmo_ref) orelse return;
    // The caller pins this VMO. Removing one FD object cannot invalidate the
    // successor object edge while K/all-AS excludes other source publishers.
    var cursor = types.references.Cursor.init(&slot.references);
    while (cursor.next()) |edge| {
        if (edge.identity.kind != .fd_object) continue;
        const object: KernelObjectRef = .{ .kind = .vmo, .index = edge.identity.index, .generation = edge.identity.generation };
        std.debug.assert(self.kernelObjectMatchesNativeVmo(object, vmo_ref));
        @import("fd.zig").revokeObjectHolders(self, object, free_list, kind);
    }
}
pub fn revokeNativeVmoFromFdTablesWithFreeList(self: anytype, vmo_ref: NativeVmoRef, free_list: *FreePageList) void {
    revokeVmoObjectHolders(self, vmo_ref, free_list, .fd_entry);
}
pub fn revokeNativeVmoFromIpcMessagesWithFreeList(self: anytype, vmo_ref: NativeVmoRef, free_list: *FreePageList) void {
    revokeVmoObjectHolders(self, vmo_ref, free_list, .ipc);
}

pub fn revokeNativeVmoFromVmaTablesWithFreeList(
    self: anytype,
    vmo_ref: NativeVmoRef,
    free_list: *FreePageList,
    unmapper: anytype,
) KernelError!void {
    const slot = self.nativeVmoSlot(vmo_ref) orelse return KernelError.InvalidState;
    // Pin across callback/withdrawal. Caller has already acquired K -> all-AS;
    // no outer lock or unmapper is entered while a target leaf is held.
    try self.retainNativeVmo(vmo_ref);
    defer self.releaseNativeVmoWithFreeList(vmo_ref, free_list);
    while (types.references.first(&slot.references, .vma)) |edge| {
        const id = edge.identity;
        const desc = self.processDescriptorSlotConst(id.owner) orelse return KernelError.InvalidState;
        if (desc.generation != id.owner_generation) return KernelError.InvalidState;
        const table = self.vmaTableForProcessIndex(id.owner) orelse return KernelError.InvalidState;
        const entry = table.entries.resolve(.{ .index = id.index, .generation = id.generation }) orelse return KernelError.InvalidState;
        if (!entry.active or !@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, vmo_ref)) return KernelError.InvalidState;
        const owner = @TypeOf(self.*).processPrincipal(id.owner);
        // unmap includes the TLB acknowledgement. Failure keeps this edge and
        // its original retain; only the temporary pin is returned by defer.
        if (!unmapper.unmap(owner, entry.start_va, entry.size_bytes)) return KernelError.InvalidState;
        self.releaseVmaCowResources(entry, free_list);
        @TypeOf(self.*).clearVmaEntry(table, id.index);
        self.releaseNativeVmoWithFreeList(vmo_ref, free_list);
        table.reference_slots.reclaimEmptyBodies();
        table.entries.reclaimEmptyBodies();
        table.active_indices.reclaimEmptyBodies();
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
    // The migration oracle matches exact source identities (including queue
    // sequence/position), hence also the filtered revoke target set. It only
    // observes; all withdrawals below are driven by the new index.
    if (comptime @import("reference_check.zig").enabled)
        @import("reference_check.zig").check(self) catch |err| @import("reference_check.zig").failure(err);
    try self.retainNativeVmo(vmo_ref);
    defer self.releaseNativeVmoWithFreeList(vmo_ref, free_list);
    const root = self.nativeVmoSlot(vmo_ref) orelse unreachable;
    var children = types.references.Cursor.init(&root.references);
    while (children.next()) |edge| {
        if (edge.identity.kind != .child) continue;
        const child: NativeVmoRef = .{ .index = edge.identity.index, .generation = edge.identity.generation };
        const slot = self.nativeVmoSlot(child) orelse return KernelError.InvalidState;
        if (!directPageViewOf(slot, vmo_ref)) continue;
        try self.retainNativeVmo(child);
        {
            defer self.releaseNativeVmoWithFreeList(child, free_list);
            try self.revokeNativeVmoFromVmaTablesWithFreeList(child, free_list, unmapper);
            self.revokeNativeVmoFromFdTablesWithFreeList(child, free_list);
            self.revokeNativeVmoFromIpcMessagesWithFreeList(child, free_list);
        }
        if (self.nativeVmoSlotConst(child) != null) return KernelError.InvalidState;
    }
    try self.revokeNativeVmoFromVmaTablesWithFreeList(vmo_ref, free_list, unmapper);
    self.revokeNativeVmoFromFdTablesWithFreeList(vmo_ref, free_list);
    self.revokeNativeVmoFromIpcMessagesWithFreeList(vmo_ref, free_list);
    return vmo_ref;
}

/// Consume an already prepared parent retain. The child is still unpublished,
/// so no fault can observe its backing arm before the matching edge exists.
pub fn publishNativeVmoParent(self: anytype, child: NativeVmoRef, parent: NativeVmoRef, offset: u64) KernelError!void {
    if (@TypeOf(self.*).nativeVmoRefsEqual(child, parent)) return KernelError.InvalidState;
    const slot = self.nativeVmoSlot(child) orelse return KernelError.InvalidState;
    const source = self.nativeVmoSlot(parent) orelse return KernelError.InvalidState;
    if (!vmoKindHasParentArm(slot.kind) or source.kind != .anonymous or !slot.backing.parent.ref.isNull() or slot.references.ownership().edges != 0) return KernelError.InvalidState;
    if (offset % native_page_size != 0 or offset > source.size_bytes or (slot.kind == .anonymous and slot.size_bytes > source.size_bytes - offset)) return KernelError.InvalidState;
    slot.backing.parent = .{ .ref = parent, .offset = offset };
    source.references.connect(&slot.parent_edge, .{ .kind = .child, .index = child.index, .generation = child.generation }, .{ .size = slot.size_bytes, .offset = offset }, true);
}

fn oldBackingCarrier(self: anytype, owner: PrincipalId, vmo_ref: NativeVmoRef, slot: *const NativeVmoSlot, first_page: usize, page_count: usize) bool {
    const release_end = first_page + page_count;
    _ = owner;
    _ = slot;
    // An independent oracle must not trust the edge used by the fast path.
    const sole_vma = false;

    // Page views and anonymous shadows borrow parent pages without owning
    // them. A shadow's inherited pages can still be faulted after the parent's
    // FD and direct VMAs disappear, so a direct borrower pins the whole parent.
    // Descendants retain each intervening parent, preserving deeper trees too.
    if (!sole_vma) {
        {
            var pool_iter_7 = self.native_vmos.iterator();
            while (pool_iter_7.next()) |pool_entry_7| {
                const candidate = pool_entry_7.value;
                if (vmoKindHasParentArm(candidate.kind) and
                    @TypeOf(self.*).nativeVmoRefsEqual(candidate.backing.parent.ref, vmo_ref))
                {
                    return true;
                }
            }
        }
    }

    if (!sole_vma) {
        {
            var pool_iter_2 = self.fd_objects.iterator();
            while (pool_iter_2.next()) |pool_entry_2| {
                const object_slot = pool_entry_2.value.*;
                if (object_slot.kind != .vmo or object_slot.ref_count == 0) continue;
                const object_vmo = switch (object_slot.payload) {
                    .vmo => |object_ref| object_ref,
                    else => continue,
                };
                if (@TypeOf(self.*).nativeVmoRefsEqual(object_vmo, vmo_ref)) {
                    return true;
                }
            }
        }
    }

    if (!sole_vma) {
        var process_index: usize = 0;
        while (process_index < self.process_capacity) : (process_index += 1) {
            const table = self.vmaTableForProcessIndexConst(process_index) orelse continue;
            var active_index: usize = 0;
            while (active_index < table.active_count) : (active_index += 1) {
                const entry_index: usize = @intCast(table.active_indices.get(@intCast(active_index)).?.*);
                const entry = (table.entries.get(@intCast(entry_index)).?);
                if (!@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, vmo_ref)) continue;
                const entry_first_page: usize = @intCast(entry.vmo_offset / native_page_size);
                const entry_page_count: usize = @intCast(entry.size_bytes / native_page_size);
                const entry_end_page = entry_first_page + entry_page_count;
                if (first_page < entry_end_page and release_end > entry_first_page) {
                    return true;
                }
            }
        }
    }

    return false;
}

fn oldNativeVmoMappingsOverlapPinnedUserObjects(
    self: anytype,
    vmo_ref: NativeVmoRef,
) bool {
    var process_index: usize = 0;
    while (process_index < self.process_capacity) : (process_index += 1) {
        const table = self.vmaTableForProcessIndexConst(process_index) orelse continue;
        const owner = @TypeOf(self.*).processPrincipal(process_index);
        var active_index: usize = 0;
        while (active_index < table.active_count) : (active_index += 1) {
            const entry_index: usize = @intCast(table.active_indices.get(@intCast(active_index)).?.*);
            const entry = (table.entries.get(@intCast(entry_index)).?);
            if (!entry.active or !@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, vmo_ref)) continue;
            if (self.rangeOverlapsPinnedUserObject(owner, entry.start_va, entry.size_bytes)) return true;
        }
    }
    return false;
}

fn oldValidateRamCacheMappings(self: anytype, ref: NativeVmoRef, first_page: usize, sorted_pages: []const u64) RamCacheMappingError!void {
    const paging = @import("../memory/ram_cache_paging.zig");
    paging.validatePages(sorted_pages, @as(u64, 1) << 52) catch return error.Invalid;
    const root = self.nativeVmoSlotConst(ref) orelse return error.Invalid;
    if (root.kind != .anonymous or !root.backing.parent.ref.isNull() or !root.has_page_store or
        root.ram_cache_state == .retiring or first_page >= root.page_count or
        sorted_pages.len > root.page_count - first_page) return error.Invalid;
    for (0..sorted_pages.len) |index| {
        const pa = self.nativeVmoOwnPagePaddr(ref, first_page + index) orelse return error.Invalid;
        if (!paging.pagesIntersect(sorted_pages, pa, 4096)) return error.Invalid;
    }
    const first_byte: u64 = @as(u64, @intCast(first_page)) * native_page_size;
    const end_byte = first_byte + @as(u64, @intCast(sorted_pages.len)) * native_page_size;
    for (0..self.process_capacity) |process_index| {
        const table = self.vmaTableForProcessIndexConst(process_index) orelse continue;
        const principal = @TypeOf(self.*).processPrincipal(process_index);
        for (0..table.active_count) |active_position| {
            const entry_index = table.active_indices.get(@intCast(active_position)).?.*;
            const entry = (table.entries.get(@intCast(entry_index)).?);
            if (!entry.active) continue;
            const backing = self.nativeVmoSlotConst(entry.vmo) orelse return error.Invalid;
            // Neither BAR pages nor the kernel clock page take RAM cache policy.
            if (backing.kind == .device_memory or backing.kind == .kernel_page) continue;
            if (directPageViewOf(backing, ref)) {
                const start: usize = @intCast(entry.vmo_offset / native_page_size);
                const count: usize = @intCast(entry.size_bytes / native_page_size);
                for (0..count) |index| {
                    const pa = self.nativeVmoOwnPagePaddr(entry.vmo, start + index) orelse continue;
                    if (paging.pagesIntersect(sorted_pages, pa, 4096))
                        try checkRamCacheMapping(self, principal, entry, @as(u64, @intCast(index)) * native_page_size, native_page_size);
                }
                continue;
            }
            var ancestor = entry.vmo;
            var offset = entry.vmo_offset;
            var depth: usize = 0;
            while (!@TypeOf(self.*).nativeVmoRefsEqual(ancestor, ref)) {
                const slot = self.nativeVmoSlotConst(ancestor) orelse return error.Invalid;
                if (slot.kind != .anonymous or slot.backing.parent.ref.isNull()) break;
                depth += 1;
                if (depth > self.native_vmos.capacity) return error.Invalid;
                offset = std.math.add(u64, offset, slot.backing.parent.offset) catch return error.Invalid;
                ancestor = slot.backing.parent.ref;
            }
            if (!@TypeOf(self.*).nativeVmoRefsEqual(ancestor, ref)) continue;
            const mapping_end = std.math.add(u64, offset, entry.size_bytes) catch return error.Invalid;
            const overlap_start = @max(first_byte, offset);
            const overlap_end = @min(end_byte, mapping_end);
            if (overlap_start >= overlap_end) continue;
            if (depth != 0) return error.ShadowAlias;
            try checkRamCacheMapping(self, principal, entry, overlap_start - offset, overlap_end - overlap_start);
        }
    }
}
