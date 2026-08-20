const std = @import("std");
const builtin = @import("builtin");
const kernel_log = @import("../kernel_log.zig");
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
const NativeVmaDmaPinMode = types.NativeVmaDmaPinMode;
const NativeVmaFaultPlan = types.NativeVmaFaultPlan;
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

pub fn checkedEnd(start: u64, size: u64) KernelError!u64 {
    if (size == 0) return KernelError.InvalidState;
    if (start > std.math.maxInt(u64) - size) return KernelError.InvalidState;
    return start + size;
}

pub fn forkCowEligible(entry: *const VmaEntry) bool {
    return entry.flags.private and !entry.flags.shared and entry.max_prot.write;
}

fn rangesOverlap(start_a: u64, size_a: u64, start_b: u64, size_b: u64) bool {
    if (size_a == 0 or size_b == 0) return false;
    const end_a, const overflow_a = @addWithOverflow(start_a, size_a);
    const end_b, const overflow_b = @addWithOverflow(start_b, size_b);
    if (overflow_a != 0 or overflow_b != 0) return true;
    return start_a < end_b and start_b < end_a;
}

const UserObjectRange = struct {
    start_va: u64,
    size_bytes: u64,
};

fn pinnedUserObjectRange(slot: *const KernelObjectSlot, owner_raw: PrincipalRaw) ?UserObjectRange {
    if (slot.ref_count == 0) return null;
    return switch (slot.payload) {
        .mmio_region => |object| if (object.owner_principal_raw == owner_raw)
            .{ .start_va = object.user_va, .size_bytes = object.size }
        else
            null,
        .dma_buffer => |object| if (object.owner_principal_raw == owner_raw)
            .{ .start_va = object.user_va, .size_bytes = object.size }
        else
            null,
        .dma_mapping => |object| if (object.owner_principal_raw == owner_raw)
            .{ .start_va = object.user_va, .size_bytes = object.size }
        else
            null,
        else => null,
    };
}

/// MMIO and DMA objects retain address and/or physical-page identity outside
/// the VMA table.  Moving or replacing an overlapping VMA without updating
/// those objects would leave stale close-time unmaps or live DMA translations.
pub fn rangeOverlapsPinnedUserObject(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
) bool {
    return self.rangeOverlapsPinnedUserObjectExcept(owner, start_va, size_bytes, null);
}

pub fn rangeOverlapsPinnedUserObjectExcept(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    except_object: ?KernelObjectRef,
) bool {
    const owner_raw: PrincipalRaw = @intFromEnum(owner);
    for (self.fd_objects[0..], 0..) |*slot, object_index| {
        const object_range = pinnedUserObjectRange(slot, owner_raw) orelse continue;
        if (except_object) |except| {
            if (except.kind == slot.kind and
                @as(usize, @intCast(except.index)) == object_index and
                except.generation == slot.generation)
            {
                continue;
            }
        }
        if (rangesOverlap(start_va, size_bytes, object_range.start_va, object_range.size_bytes)) return true;
    }
    return false;
}

/// DMA derivation may legitimately pin two independent Linux streaming-DMA
/// mappings whose byte ranges share a resolved backing page. Keep that exception
/// separate from the general pinned-object overlap predicate: VMA mutation
/// and replacement must continue to reject a range while any pin survives.
///
/// MMIO and DMA buffers are never alias candidates. MMIO owns a close-time
/// user-VA unmap, while a DMA buffer is a separately published bidirectional
/// allocation. Streaming-mapping aliases are admitted only when the syscall's
/// lifetime policy makes closing either mapping harmless to the other. Scatter
/// mappings additionally require active VT-d so every alias owns an independent
/// IOVA allocation; pass-through retains only its existing linear-mapping alias.
pub fn rangeConflictsWithDmaDerivation(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    except_object: ?KernelObjectRef,
    allow_dma_mapping_aliases: bool,
    vtd_active: bool,
) bool {
    const owner_raw: PrincipalRaw = @intFromEnum(owner);
    for (self.fd_objects[0..], 0..) |*slot, object_index| {
        const object_range = pinnedUserObjectRange(slot, owner_raw) orelse continue;
        if (!rangesOverlap(start_va, size_bytes, object_range.start_va, object_range.size_bytes)) continue;

        const is_except = if (except_object) |except|
            except.kind == slot.kind and
                @as(usize, @intCast(except.index)) == object_index and
                except.generation == slot.generation
        else
            false;
        switch (slot.payload) {
            .mmio_region => {
                pachaTraceDmaDerivationConflict(
                    "mmio_region",
                    null,
                    start_va,
                    size_bytes,
                    allow_dma_mapping_aliases,
                    vtd_active,
                );
                return true;
            },
            .dma_buffer => if (!is_except) {
                pachaTraceDmaDerivationConflict(
                    "dma_buffer",
                    null,
                    start_va,
                    size_bytes,
                    allow_dma_mapping_aliases,
                    vtd_active,
                );
                return true;
            },
            .dma_mapping => |mapping| if (!is_except and
                (!allow_dma_mapping_aliases or (mapping.page_count != 0 and !vtd_active)))
            {
                pachaTraceDmaDerivationConflict(
                    "dma_mapping",
                    mapping.page_count,
                    start_va,
                    size_bytes,
                    allow_dma_mapping_aliases,
                    vtd_active,
                );
                return true;
            },
            else => unreachable,
        }
    }
    return false;
}

fn pachaTraceDmaDerivationConflict(
    object_kind: []const u8,
    page_count: ?u16,
    requested_start: u64,
    requested_size: u64,
    allow_dma_mapping_aliases: bool,
    vtd_active: bool,
) void {
    const requested_end, const overflow = @addWithOverflow(requested_start, requested_size);
    const range_end = if (overflow == 0) requested_end else std.math.maxInt(u64);
    var buf: [256]u8 = undefined;
    const line = if (page_count) |pages|
        std.fmt.bufPrint(
            &buf,
            "[trace] c=kernel e=dma_derivation_conflict cls=1 kind={s} page_count={} requested_start=0x{x} requested_end=0x{x} allow_dma_mapping_aliases={} vtd_active={}\n",
            .{ object_kind, pages, requested_start, range_end, @intFromBool(allow_dma_mapping_aliases), @intFromBool(vtd_active) },
        ) catch return
    else
        std.fmt.bufPrint(
            &buf,
            "[trace] c=kernel e=dma_derivation_conflict cls=1 kind={s} requested_start=0x{x} requested_end=0x{x} allow_dma_mapping_aliases={} vtd_active={}\n",
            .{ object_kind, requested_start, range_end, @intFromBool(allow_dma_mapping_aliases), @intFromBool(vtd_active) },
        ) catch return;
    if (builtin.is_test) {
        kernel_log.appendText(line);
    } else {
        kernel_log.write(line);
    }
}

pub fn ownerHasPinnedUserObject(self: anytype, owner: PrincipalId) bool {
    const owner_raw: PrincipalRaw = @intFromEnum(owner);
    for (self.fd_objects[0..]) |*slot| {
        if (pinnedUserObjectRange(slot, owner_raw) != null) return true;
    }
    return false;
}

/// Return the furthest end of any VMA or address-bound object intersecting a
/// candidate range.  The syscall allocator uses this to jump whole occupied
/// spans rather than retrying one page at a time through a large PROT_NONE
/// arena.
pub fn userMapCollisionEndVa(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
) ?u64 {
    var collision_end: u64 = 0;
    const table = self.getVmaTableConst(owner) orelse return std.math.maxInt(u64);
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = &table.entries[entry_index];
        if (!entry.active or !rangesOverlap(start_va, size_bytes, entry.start_va, entry.size_bytes)) continue;
        collision_end = @max(collision_end, entry.endVa());
    }
    const owner_raw: PrincipalRaw = @intFromEnum(owner);
    for (self.fd_objects[0..]) |*slot| {
        const object_range = pinnedUserObjectRange(slot, owner_raw) orelse continue;
        if (!rangesOverlap(start_va, size_bytes, object_range.start_va, object_range.size_bytes)) continue;
        const object_end, const overflow = @addWithOverflow(object_range.start_va, object_range.size_bytes);
        if (overflow != 0) return std.math.maxInt(u64);
        collision_end = @max(collision_end, object_end);
    }
    return if (collision_end == 0) null else collision_end;
}

pub fn markForkCowVmasCommitted(self: anytype, owner: PrincipalId) void {
    const table = self.getVmaTable(owner) orelse return;
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = &table.entries[entry_index];
        if (entry.active and @TypeOf(self.*).forkCowEligible(entry)) {
            entry.flags.fork_cow = true;
        }
    }
}

pub fn cloneVmaTableForFork(self: anytype, from: PrincipalId, to: PrincipalId) KernelError!void {
    if (from == to) return KernelError.InvalidState;
    const source_table = self.getVmaTable(from) orelse return KernelError.InvalidState;
    const dest_table = self.getVmaTable(to) orelse return KernelError.InvalidState;
    if (dest_table.active_count != 0) return KernelError.InvalidState;
    errdefer {
        while (dest_table.active_count != 0) {
            const vma_index: usize = @intCast(dest_table.active_indices[dest_table.active_count - 1]);
            const entry = &dest_table.entries[vma_index];
            const vmo_ref = entry.vmo;
            self.releaseVmaCowResources(entry, null);
            @TypeOf(self.*).clearVmaEntry(dest_table, vma_index);
            self.releaseNativeVmo(vmo_ref);
        }
    }

    var active_index: usize = 0;
    while (active_index < source_table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(source_table.active_indices[active_index]);
        var source_entry = source_table.entries[entry_index];
        if (!source_entry.active) continue;
        // Prepare only the child during the fallible retain phase.  The
        // syscall commits the parent COW flags immediately before its PTE
        // write-protect pass, so a failed retain never mutates parent state.
        if (@TypeOf(self.*).forkCowEligible(&source_entry)) {
            source_entry.flags.fork_cow = true;
        }
        try self.retainNativeVmo(source_entry.vmo);
        if (!source_entry.cow_table.isNull()) {
            self.retainNativeCowTable(source_entry.cow_table) catch |err| {
                self.releaseNativeVmo(source_entry.vmo);
                return err;
            };
        }
        @TypeOf(self.*).installVmaEntry(dest_table, entry_index, source_entry);
    }
    dest_table.next_user_map_va = source_table.next_user_map_va;
}

/// Give a freshly cloned address space an independent snapshot of every
/// already-dirty private page.  Clean VMO pages remain shared and acquire a
/// private COW table on first write.  This keeps a child's inherited TLS and
/// libc state independent of concurrent writes or teardown in its parent.
/// The caller holds the VM transaction locks for both address spaces.
pub fn detachForkChildDirtyCowTables(
    self: anytype,
    child: PrincipalId,
    free_list: *FreePageList,
) KernelError!void {
    const table = self.getVmaTable(child) orelse return KernelError.InvalidState;
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = &table.entries[entry_index];
        if (!entry.active or entry.cow_table.isNull()) continue;
        if (self.nativeCowTableIsUnique(entry.cow_table)) continue;
        try self.detachSharedEntryCowTable(entry, free_list);
    }
}

pub fn copyForkAnonymousPresentPageToChild(
    self: anytype,
    to: PrincipalId,
    entry_index: usize,
    va: u64,
    src_paddr: u64,
    free_list: *FreePageList,
) KernelError!void {
    const dest_table = self.getVmaTable(to) orelse return KernelError.InvalidState;
    if (entry_index >= dest_table.entries.len) return KernelError.InvalidState;
    const dest_entry = &dest_table.entries[entry_index];
    if (!dest_entry.active or !dest_entry.flags.fork_cow) return KernelError.InvalidState;
    const copied_paddr = (self.allocPhysicalPage(free_list) catch return KernelError.OutOfFreePages).paddr;
    var installed = false;
    defer if (!installed) free_list.appendPage(0, copied_paddr) catch {};
    @TypeOf(self.*).copyPhysicalPage(copied_paddr, src_paddr);
    self.ensureEntryCowTable(dest_entry, free_list) catch return KernelError.TableFull;
    const cow_page = @TypeOf(self.*).entryCowPageIndex(dest_entry, va) orelse return KernelError.InvalidState;
    self.setNativeCowPagePaddr(dest_entry.cow_table, cow_page, copied_paddr) catch return KernelError.TableFull;
    installed = true;
}

pub fn getVmaTable(self: anytype, principal: PrincipalId) ?*VmaTable {
    const index = processIndexFromPrincipal(principal) orelse return null;
    return self.vmaTableForProcessIndex(index);
}

pub fn getVmaTableConst(self: anytype, principal: PrincipalId) ?*const VmaTable {
    const index = processIndexFromPrincipal(principal) orelse return null;
    return self.vmaTableForProcessIndexConst(index);
}

pub fn vmaEntryConst(self: anytype, owner: PrincipalId, start_va: u64) ?*const VmaEntry {
    const table = self.getVmaTableConst(owner) orelse return null;
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = &table.entries[entry_index];
        if (entry.start_va == start_va) return entry;
    }
    return null;
}

pub fn vmaEntryForVaConst(self: anytype, owner: PrincipalId, va: u64) ?*const VmaEntry {
    const table = self.getVmaTableConst(owner) orelse return null;
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = &table.entries[entry_index];
        if (va >= entry.start_va and va < entry.endVa()) return entry;
    }
    return null;
}

pub fn vmaProtAllowsFault(prot: VmaProt, write_access: bool, instruction_fetch: bool) bool {
    if (instruction_fetch) return prot.exec;
    if (write_access) return prot.write;
    return prot.read;
}

pub fn nativeFaultMappingProt(entry: *const VmaEntry) MapProt {
    var write = entry.prot.write;
    if (entry.flags.private and !entry.flags.shared and (entry.flags.fork_cow or !entry.flags.anonymous)) {
        write = false;
    }
    return .{
        .read = entry.prot.read,
        .write = write,
        .exec = entry.prot.exec,
        .pkey = entry.prot.pkey,
    };
}

pub fn nativeVmaFaultMapping(
    self: anytype,
    owner: PrincipalId,
    fault_page_va: u64,
    write_access: bool,
    instruction_fetch: bool,
) ?NativeVmaFaultMapping {
    if (!@TypeOf(self.*).isPageAligned(fault_page_va)) return null;
    const entry = self.vmaEntryForVaConst(owner, fault_page_va) orelse return null;
    if (!@TypeOf(self.*).vmaProtAllowsFault(entry.prot, write_access, instruction_fetch)) return null;
    const page_delta = (fault_page_va - entry.start_va) / native_page_size;
    const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
    const dirty_paddr = self.entryDirtyPagePaddr(entry, fault_page_va);
    const paddr = dirty_paddr orelse
        (self.nativeVmoResolvedPagePaddr(entry.vmo, @intCast(vmo_page)) orelse return null);
    return .{
        .paddr = paddr,
        .prot = if (dirty_paddr != null) self.dirtyCowMappingProt(entry) else @TypeOf(self.*).nativeFaultMappingProt(entry),
    };
}

pub fn nativeVmaDmaPinMode(
    self: anytype,
    owner: PrincipalId,
    page_va: u64,
    device_writes: bool,
) ?NativeVmaDmaPinMode {
    if (!@TypeOf(self.*).isPageAligned(page_va)) return null;
    const entry = self.vmaEntryForVaConst(owner, page_va) orelse return null;
    if (!entry.prot.read or (device_writes and !entry.prot.write)) return null;
    if (!entry.prot.write) return .read_only;
    if (entry.flags.private and
        !entry.flags.shared and
        (entry.flags.fork_cow or !entry.flags.anonymous))
    {
        return .cow_writable;
    }
    return .direct_writable;
}

pub fn dmaAddressForResolvedPages(paddrs: []const u64, first_page_offset: u64) ?u64 {
    if (paddrs.len == 0 or first_page_offset >= native_page_size) return null;
    const first_paddr = paddrs[0];
    if (first_paddr == 0 or (first_paddr & (native_page_size - 1)) != 0) return null;
    for (paddrs[1..], 1..) |paddr, page_index| {
        const page_delta, const delta_overflow = @mulWithOverflow(
            @as(u64, @intCast(page_index)),
            native_page_size,
        );
        if (delta_overflow != 0) return null;
        const expected, const expected_overflow = @addWithOverflow(first_paddr, page_delta);
        if (expected_overflow != 0 or paddr != expected) return null;
    }
    const result, const result_overflow = @addWithOverflow(first_paddr, first_page_offset);
    return if (result_overflow == 0) result else null;
}

pub fn nativeVmaInitialMapping(
    self: anytype,
    owner: PrincipalId,
    page_va: u64,
) ?NativeVmaFaultMapping {
    if (!@TypeOf(self.*).isPageAligned(page_va)) return null;
    const entry = self.vmaEntryForVaConst(owner, page_va) orelse return null;
    const page_delta = (page_va - entry.start_va) / native_page_size;
    const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
    const dirty_paddr = self.entryDirtyPagePaddr(entry, page_va);
    const paddr = dirty_paddr orelse
        (self.nativeVmoResolvedPagePaddr(entry.vmo, @intCast(vmo_page)) orelse return null);
    return .{
        .paddr = paddr,
        .prot = if (dirty_paddr != null) self.dirtyCowMappingProt(entry) else @TypeOf(self.*).nativeFaultMappingProt(entry),
    };
}

pub fn prepareNativeVmaFaultMapping(
    self: anytype,
    owner: PrincipalId,
    fault_page_va: u64,
    write_access: bool,
    instruction_fetch: bool,
) NativeVmaFaultPlan {
    if (!@TypeOf(self.*).isPageAligned(fault_page_va)) return .{};
    const table = self.getVmaTable(owner) orelse return .{};
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = &table.entries[entry_index];
        if (fault_page_va < entry.start_va or fault_page_va >= entry.endVa()) continue;
        if (!@TypeOf(self.*).vmaProtAllowsFault(entry.prot, write_access, instruction_fetch)) return .{};

        const page_delta = (fault_page_va - entry.start_va) / native_page_size;
        const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
        const vmo_page_index: usize = @intCast(vmo_page);
        const paddr = self.entryDirtyPagePaddr(entry, fault_page_va) orelse
            (self.nativeVmoResolvedPagePaddr(entry.vmo, vmo_page_index) orelse 0);
        if (paddr != 0) {
            return .{
                .kind = .ready,
                .mapping = .{
                    .paddr = paddr,
                    .prot = @TypeOf(self.*).nativeFaultMappingProt(entry),
                },
            };
        }
        if (!entry.flags.anonymous or vmo_page > std.math.maxInt(u32)) return .{};
        return .{
            .kind = .allocate_zero,
            .mapping = .{ .paddr = 0, .prot = @TypeOf(self.*).nativeFaultMappingProt(entry) },
            .entry_index = @intCast(entry_index),
            .fault_page_va = fault_page_va,
            .vmo = entry.vmo,
            .vmo_page_index = @intCast(vmo_page),
        };
    }
    return .{};
}

pub fn commitNativeVmaFaultMapping(
    self: anytype,
    owner: PrincipalId,
    plan: NativeVmaFaultPlan,
    candidate_paddr: u64,
) ?NativeVmaFaultMapping {
    if (plan.kind != .allocate_zero or candidate_paddr == 0 or (candidate_paddr & 0xFFF) != 0) return null;
    const table = self.getVmaTable(owner) orelse return null;
    const entry_index: usize = @intCast(plan.entry_index);
    if (entry_index >= table.entries.len) return null;
    const entry = &table.entries[entry_index];
    if (!entry.active or plan.fault_page_va < entry.start_va or plan.fault_page_va >= entry.endVa()) return null;
    if (!@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, plan.vmo)) return null;
    const page_delta = (plan.fault_page_va - entry.start_va) / native_page_size;
    const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
    if (vmo_page != plan.vmo_page_index) return null;

    var paddr = self.entryDirtyPagePaddr(entry, plan.fault_page_va) orelse
        (self.nativeVmoResolvedPagePaddr(entry.vmo, @intCast(vmo_page)) orelse 0);
    if (paddr == 0) {
        if (!entry.flags.anonymous) return null;
        var page = [_]u64{candidate_paddr};
        self.installNativeVmoPages(entry.vmo, @intCast(vmo_page), page[0..]) catch return null;
        paddr = candidate_paddr;
    }
    return .{
        .paddr = paddr,
        .prot = @TypeOf(self.*).nativeFaultMappingProt(entry),
    };
}

pub fn copyPhysicalPage(dst_paddr: u64, src_paddr: u64) void {
    if (builtin.is_test) return;
    const dst: [*]u8 = @ptrFromInt(dst_paddr);
    const src: [*]const u8 = @ptrFromInt(src_paddr);
    @memcpy(dst[0..4096], src[0..4096]);
}

pub fn replaceVmaPageWithAnonymousPrivatePage(
    self: anytype,
    table: *VmaTable,
    entry_index: usize,
    fault_page_va: u64,
    new_paddr: u64,
    free_list: *FreePageList,
) KernelError!NativeVmoRef {
    if (entry_index >= table.entries.len) return KernelError.InvalidState;
    var entry = &table.entries[entry_index];
    if (!entry.active) return KernelError.InvalidState;
    if (fault_page_va < entry.start_va or fault_page_va >= entry.endVa()) return KernelError.InvalidState;

    const original = entry.*;
    const page_end_va = fault_page_va + native_page_size;
    const has_before = fault_page_va > original.start_va;
    const has_after = page_end_va < original.endVa();
    const target_offset = fault_page_va - original.start_va;
    const before_size = fault_page_va - original.start_va;
    const after_size = original.endVa() - page_end_va;

    const before_index = if (has_before) @TypeOf(self.*).findFreeVma(table) orelse return KernelError.TableFull else 0;
    const after_index = if (has_after) blk: {
        for (table.entries[0..], 0..) |candidate, candidate_index| {
            if (has_before and candidate_index == before_index) continue;
            if (!candidate.active) break :blk candidate_index;
        }
        return KernelError.TableFull;
    } else 0;

    const private_vmo = try self.createNativeVmo(.anonymous, native_page_size);
    try self.retainNativeVmo(private_vmo);
    errdefer self.releaseNativeVmoWithFreeList(private_vmo, free_list);
    var page = [_]u64{new_paddr};
    try self.installNativeVmoPages(private_vmo, 0, page[0..]);

    if (has_before) try self.retainNativeVmo(original.vmo);
    errdefer if (has_before) self.releaseNativeVmo(original.vmo);
    if (has_after) try self.retainNativeVmo(original.vmo);
    errdefer if (has_after) self.releaseNativeVmo(original.vmo);

    if (has_before) {
        @TypeOf(self.*).installVmaEntry(table, before_index, original);
        table.entries[before_index].size_bytes = before_size;
    }
    if (has_after) {
        @TypeOf(self.*).installVmaEntry(table, after_index, original);
        table.entries[after_index].start_va = page_end_va;
        table.entries[after_index].size_bytes = after_size;
        table.entries[after_index].vmo_offset = original.vmo_offset + target_offset + native_page_size;
    }

    entry = &table.entries[entry_index];
    @TypeOf(self.*).installVmaEntry(table, entry_index, .{
        .active = true,
        .start_va = fault_page_va,
        .size_bytes = native_page_size,
        .prot = original.prot,
        .max_prot = original.max_prot,
        .flags = .{
            .fixed = original.flags.fixed,
            .fixed_noreplace = original.flags.fixed_noreplace,
            .private = true,
            .shared = false,
            .anonymous = true,
            .noreserve = original.flags.noreserve,
            .pkey = original.flags.pkey,
        },
        .vmo = private_vmo,
        .vmo_offset = 0,
    });
    self.releaseNativeVmo(original.vmo);
    return private_vmo;
}

fn writableCowFaultMapping(entry: *const VmaEntry, paddr: u64) NativeVmaFaultMapping {
    return .{
        .paddr = paddr,
        .prot = .{
            .read = entry.prot.read,
            .write = entry.prot.write,
            .exec = entry.prot.exec,
            .pkey = entry.prot.pkey,
        },
    };
}

fn nativeCowRefsEqual(a: NativeCowTableRef, b: NativeCowTableRef) bool {
    return a.index == b.index and a.generation == b.generation;
}

pub fn prepareNativeVmaCowMapping(
    self: anytype,
    owner: PrincipalId,
    fault_page_va: u64,
    write_access: bool,
    instruction_fetch: bool,
) NativeVmaFaultPlan {
    if (!@TypeOf(self.*).isPageAligned(fault_page_va) or !write_access or instruction_fetch) return .{};
    const table = self.getVmaTable(owner) orelse return .{};
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = &table.entries[entry_index];
        if (fault_page_va < entry.start_va or fault_page_va >= entry.endVa()) continue;
        if (!entry.flags.private or entry.flags.shared or !entry.prot.write) return .{};
        if (entry.flags.anonymous and !entry.flags.fork_cow) return .{};

        const page_delta = (fault_page_va - entry.start_va) / native_page_size;
        const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
        if (vmo_page > std.math.maxInt(u32)) return .{};
        const cow_page = if (entry.cow_table.isNull()) 0 else (@TypeOf(self.*).entryCowPageIndex(entry, fault_page_va) orelse return .{});
        if (self.entryDirtyPagePaddr(entry, fault_page_va)) |owned_paddr| {
            if (!entry.cow_table.isNull() and !self.nativeCowTableIsUnique(entry.cow_table)) {
                return .{ .kind = .locked_slow_path };
            }
            return .{
                .kind = .ready,
                .mapping = writableCowFaultMapping(entry, owned_paddr),
            };
        }
        const src_paddr = self.nativeVmoResolvedPagePaddr(entry.vmo, @intCast(vmo_page)) orelse 0;
        if (src_paddr == 0 and !entry.flags.anonymous) return .{};
        if (!entry.cow_table.isNull() and !self.nativeCowTableIsUnique(entry.cow_table)) {
            return .{ .kind = .locked_slow_path };
        }
        return .{
            .kind = if (src_paddr == 0) .allocate_zero else .allocate_copy,
            .mapping = writableCowFaultMapping(entry, 0),
            .entry_index = @intCast(entry_index),
            .fault_page_va = fault_page_va,
            .vmo = entry.vmo,
            .vmo_page_index = @intCast(vmo_page),
            .cow_table = entry.cow_table,
            .cow_page_index = cow_page,
            .source_paddr = src_paddr,
        };
    }
    return .{};
}

pub fn commitNativeVmaCowMapping(
    self: anytype,
    owner: PrincipalId,
    plan: NativeVmaFaultPlan,
    candidate_paddr: u64,
    free_list: *FreePageList,
) ?NativeVmaFaultMapping {
    if ((plan.kind != .allocate_zero and plan.kind != .allocate_copy) or candidate_paddr == 0 or (candidate_paddr & 0xFFF) != 0) return null;
    const table = self.getVmaTable(owner) orelse return null;
    const entry_index: usize = @intCast(plan.entry_index);
    if (entry_index >= table.entries.len) return null;
    const entry = &table.entries[entry_index];
    if (!entry.active or plan.fault_page_va < entry.start_va or plan.fault_page_va >= entry.endVa()) return null;
    if (!entry.flags.private or entry.flags.shared or !entry.prot.write) return null;
    if (!@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, plan.vmo)) return null;
    if (!nativeCowRefsEqual(entry.cow_table, plan.cow_table)) return null;

    if (self.entryDirtyPagePaddr(entry, plan.fault_page_va)) |winner_paddr| {
        if (!entry.cow_table.isNull() and !self.nativeCowTableIsUnique(entry.cow_table)) return null;
        return writableCowFaultMapping(entry, winner_paddr);
    }
    const page_delta = (plan.fault_page_va - entry.start_va) / native_page_size;
    const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
    if (vmo_page != plan.vmo_page_index) return null;
    const current_source = self.nativeVmoResolvedPagePaddr(entry.vmo, @intCast(vmo_page)) orelse 0;
    if (current_source != plan.source_paddr) return null;

    self.ensureEntryCowTable(entry, free_list) catch return null;
    if (!self.nativeCowTableIsUnique(entry.cow_table)) return null;
    const cow_page = @TypeOf(self.*).entryCowPageIndex(entry, plan.fault_page_va) orelse return null;
    if (cow_page != plan.cow_page_index and !plan.cow_table.isNull()) return null;
    self.setNativeCowPagePaddr(entry.cow_table, cow_page, candidate_paddr) catch return null;
    return writableCowFaultMapping(entry, candidate_paddr);
}

pub fn nativeVmaCowPlanSourceIsCurrent(
    self: anytype,
    owner: PrincipalId,
    plan: NativeVmaFaultPlan,
) bool {
    if (plan.kind != .allocate_copy or plan.source_paddr == 0) return false;
    const table = self.getVmaTable(owner) orelse return false;
    const entry_index: usize = @intCast(plan.entry_index);
    if (entry_index >= table.entries.len) return false;
    const entry = &table.entries[entry_index];
    if (!entry.active or plan.fault_page_va < entry.start_va or plan.fault_page_va >= entry.endVa()) return false;
    if (!entry.flags.private or entry.flags.shared or !entry.prot.write) return false;
    if (!@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, plan.vmo)) return false;
    if (!nativeCowRefsEqual(entry.cow_table, plan.cow_table)) return false;
    if (self.entryDirtyPagePaddr(entry, plan.fault_page_va) != null) return false;
    const page_delta = (plan.fault_page_va - entry.start_va) / native_page_size;
    const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
    if (vmo_page != plan.vmo_page_index) return false;
    return (self.nativeVmoResolvedPagePaddr(entry.vmo, @intCast(vmo_page)) orelse 0) == plan.source_paddr;
}

pub fn ensureNativeVmaCowMappingLockedSlow(
    self: anytype,
    owner: PrincipalId,
    fault_page_va: u64,
    write_access: bool,
    instruction_fetch: bool,
    free_list: *FreePageList,
) ?NativeVmaFaultMapping {
    if (!@TypeOf(self.*).isPageAligned(fault_page_va) or !write_access or instruction_fetch) return null;
    const table = self.getVmaTable(owner) orelse return null;
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = &table.entries[entry_index];
        if (fault_page_va < entry.start_va or fault_page_va >= entry.endVa()) continue;
        if (!entry.flags.private or entry.flags.shared or !entry.prot.write) return null;
        if (entry.flags.anonymous and !entry.flags.fork_cow) return null;

        const page_delta = (fault_page_va - entry.start_va) / native_page_size;
        const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
        if (self.entryDirtyPagePaddr(entry, fault_page_va)) |owned_paddr| {
            if (!entry.cow_table.isNull() and !self.nativeCowTableIsUnique(entry.cow_table)) {
                self.detachSharedEntryCowTable(entry, free_list) catch return null;
                const detached_paddr = self.entryDirtyPagePaddr(entry, fault_page_va) orelse return null;
                return .{
                    .paddr = detached_paddr,
                    .prot = .{
                        .read = entry.prot.read,
                        .write = entry.prot.write,
                        .exec = entry.prot.exec,
                        .pkey = entry.prot.pkey,
                    },
                    .invalidate_start_va = entry.start_va,
                    .invalidate_size_bytes = entry.size_bytes,
                };
            }
            return writableCowFaultMapping(entry, owned_paddr);
        }
        const src_paddr = self.nativeVmoResolvedPagePaddr(entry.vmo, @intCast(vmo_page)) orelse 0;
        if (src_paddr == 0 and !entry.flags.anonymous) return null;
        var invalidate_start_va: u64 = 0;
        var invalidate_size_bytes: u64 = 0;
        if (!entry.cow_table.isNull() and !self.nativeCowTableIsUnique(entry.cow_table)) {
            self.detachSharedEntryCowTable(entry, free_list) catch return null;
            invalidate_start_va = entry.start_va;
            invalidate_size_bytes = entry.size_bytes;
        }
        const new_paddr = (self.allocPhysicalPage(free_list) catch return null).paddr;
        var installed = false;
        defer if (!installed) free_list.appendPage(0, new_paddr) catch {};

        if (src_paddr != 0) {
            @TypeOf(self.*).copyPhysicalPage(new_paddr, src_paddr);
        }
        self.ensureEntryCowTable(entry, free_list) catch return null;
        const cow_page = @TypeOf(self.*).entryCowPageIndex(entry, fault_page_va) orelse return null;
        self.setNativeCowPagePaddr(entry.cow_table, cow_page, new_paddr) catch return null;
        installed = true;
        return .{
            .paddr = new_paddr,
            .prot = .{
                .read = entry.prot.read,
                .write = entry.prot.write,
                .exec = entry.prot.exec,
                .pkey = entry.prot.pkey,
            },
            .invalidate_start_va = invalidate_start_va,
            .invalidate_size_bytes = invalidate_size_bytes,
        };
    }
    return null;
}

pub fn setVmaProtRange(self: anytype, owner: PrincipalId, start_va: u64, size_bytes: u64, prot: VmaProt) KernelError!void {
    try self.requireActiveProcess(owner);
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes)) return KernelError.InvalidState;
    const end_va = try @TypeOf(self.*).checkedEnd(start_va, size_bytes);
    const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;

    const active_limit = table.active_count;
    var active_index: usize = 0;
    while (active_index < active_limit) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        var entry = &table.entries[entry_index];
        const entry_end = entry.endVa();
        if (start_va < entry.start_va or end_va > entry_end) continue;
        if (!@TypeOf(self.*).vmaProtAllowedByMax(prot, entry.max_prot)) return KernelError.InvalidState;

        if (entry.start_va == start_va and entry.size_bytes == size_bytes) {
            entry.prot = prot;
            return;
        }

        const original = entry.*;
        const has_before = start_va > original.start_va;
        const has_after = end_va < entry_end;
        const before_size = start_va - original.start_va;
        const target_offset = start_va - original.start_va;
        const after_size = entry_end - end_va;

        // Reserve every split slot before retaining or installing anything.
        // The old ordering installed the before fragment first, then could
        // fail to find an after slot and leave a live VMA holding released
        // VMO/COW references.
        var split_indices: [2]usize = .{ 0, 0 };
        const split_count: usize = @as(usize, @intFromBool(has_before)) +
            @as(usize, @intFromBool(has_after));
        var found_split_count: usize = 0;
        if (split_count != 0) {
            for (table.entries[0..], 0..) |candidate, candidate_index| {
                if (candidate.active) continue;
                split_indices[found_split_count] = candidate_index;
                found_split_count += 1;
                if (found_split_count == split_count) break;
            }
            if (found_split_count != split_count) return KernelError.TableFull;
        }
        const before_index = if (has_before) split_indices[0] else 0;
        const after_index = if (has_after)
            split_indices[@as(usize, @intFromBool(has_before))]
        else
            0;

        if (has_before) try self.retainNativeVmo(original.vmo);
        errdefer if (has_before) self.releaseNativeVmo(original.vmo);
        if (has_before and !original.cow_table.isNull()) try self.retainNativeCowTable(original.cow_table);
        errdefer if (has_before and !original.cow_table.isNull()) self.releaseNativeCowTable(original.cow_table, null);
        if (has_after) try self.retainNativeVmo(original.vmo);
        errdefer if (has_after) self.releaseNativeVmo(original.vmo);
        if (has_after and !original.cow_table.isNull()) try self.retainNativeCowTable(original.cow_table);
        errdefer if (has_after and !original.cow_table.isNull()) self.releaseNativeCowTable(original.cow_table, null);

        if (has_before) {
            @TypeOf(self.*).installVmaEntry(table, before_index, original);
            table.entries[before_index].size_bytes = before_size;
        }
        if (has_after) {
            @TypeOf(self.*).installVmaEntry(table, after_index, original);
            table.entries[after_index].start_va = end_va;
            table.entries[after_index].size_bytes = after_size;
            table.entries[after_index].vmo_offset = original.vmo_offset + target_offset + size_bytes;
            table.entries[after_index].cow_page_offset = original.cow_page_offset + @as(u32, @intCast((target_offset + size_bytes) / native_page_size));
        }

        entry = &table.entries[entry_index];
        entry.start_va = start_va;
        entry.size_bytes = size_bytes;
        entry.prot = prot;
        entry.vmo_offset = original.vmo_offset + target_offset;
        entry.cow_page_offset = original.cow_page_offset + @as(u32, @intCast(target_offset / native_page_size));
        return;
    }
    return KernelError.InvalidState;
}

pub fn releaseFdTableForProcessIndex(self: anytype, index: usize) void {
    const table = self.fdTableForProcessIndex(index) orelse return;
    var fd: usize = 0;
    while (fd < fd_table_entries) : (fd += 1) {
        const object_ref = table.entries[fd].object;
        if (object_ref.isNull()) continue;
        table.entries[fd] = .{};
        self.releaseKernelObject(object_ref);
    }
}

pub fn releaseFdTableForProcessIndexWithFreeList(
    self: anytype,
    index: usize,
    free_list: *FreePageList,
) void {
    const table = self.fdTableForProcessIndex(index) orelse return;
    var fd: usize = 0;
    while (fd < fd_table_entries) : (fd += 1) {
        const object_ref = table.entries[fd].object;
        if (object_ref.isNull()) continue;
        table.entries[fd] = .{};
        self.releaseKernelObjectWithFreeList(object_ref, free_list);
    }
}

pub fn releaseVmaCowPageRange(self: anytype, entry: *const VmaEntry, first_page_delta: usize, page_count: usize, free_list: ?*FreePageList) void {
    if (entry.cow_table.isNull() or page_count == 0) return;
    if (!self.nativeCowTableIsUnique(entry.cow_table)) return;
    if (first_page_delta > std.math.maxInt(u32) or page_count > std.math.maxInt(u32)) return;
    const delta: u32 = @intCast(first_page_delta);
    if (entry.cow_page_offset > std.math.maxInt(u32) - delta) return;
    const first = entry.cow_page_offset + delta;
    if (first > std.math.maxInt(u32) - @as(u32, @intCast(page_count))) return;
    self.clearNativeCowPageSlots(entry.cow_table, first, @intCast(page_count), free_list);
}

pub fn releaseVmaCowResources(self: anytype, entry: *const VmaEntry, free_list: ?*FreePageList) void {
    if (entry.cow_table.isNull()) return;
    const page_count: usize = @intCast(entry.size_bytes / native_page_size);
    self.releaseVmaCowPageRange(entry, 0, page_count, free_list);
    self.releaseNativeCowTable(entry.cow_table, free_list);
}

pub fn releaseVmaTableForProcessIndex(self: anytype, index: usize) void {
    const table = self.vmaTableForProcessIndex(index) orelse return;
    while (table.active_count != 0) {
        const vma_index: usize = @intCast(table.active_indices[table.active_count - 1]);
        const entry = &table.entries[vma_index];
        const vmo_ref = entry.vmo;
        self.releaseVmaCowResources(entry, null);
        @TypeOf(self.*).clearVmaEntry(table, vma_index);
        self.releaseNativeVmo(vmo_ref);
    }
}

pub fn releaseVmaTableForProcessIndexWithFreeList(
    self: anytype,
    index: usize,
    free_list: *FreePageList,
) void {
    const table = self.vmaTableForProcessIndex(index) orelse return;
    while (table.active_count != 0) {
        const vma_index: usize = @intCast(table.active_indices[table.active_count - 1]);
        const entry = &table.entries[vma_index];
        const vmo_ref = entry.vmo;
        self.releaseVmaCowResources(entry, free_list);
        @TypeOf(self.*).clearVmaEntry(table, vma_index);
        self.releaseNativeVmoWithFreeList(vmo_ref, free_list);
    }
}

pub fn releasePrincipalNativeMemory(
    self: anytype,
    owner: PrincipalId,
    free_list: *FreePageList,
) void {
    const index = processIndexFromPrincipal(owner) orelse return;
    // Address-bound FDs may have been duplicated or queued to another
    // process. Revoke every exact reference first so MMIO close-time unmaps
    // and DMA IOVA teardown complete before VMA pages return to PMM.
    self.revokeOwnedPinnedUserObjectsWithFreeList(owner, free_list);
    self.releaseVmaTableForProcessIndexWithFreeList(index, free_list);
    self.releaseFdTableForProcessIndexWithFreeList(index, free_list);
}

pub fn replaceVmaTableForExec(
    self: anytype,
    dest_owner: PrincipalId,
    source_owner: PrincipalId,
    free_list: *FreePageList,
) KernelError!void {
    if (dest_owner == source_owner) return KernelError.InvalidState;
    const dest_index = processIndexFromPrincipal(dest_owner) orelse return KernelError.InvalidState;
    const source_index = processIndexFromPrincipal(source_owner) orelse return KernelError.InvalidState;
    const dest_table = self.vmaTableForProcessIndex(dest_index) orelse return KernelError.InvalidState;
    const source_table = self.vmaTableForProcessIndex(source_index) orelse return KernelError.InvalidState;

    self.releaseVmaTableForProcessIndexWithFreeList(dest_index, free_list);
    var active_pos: usize = 0;
    while (active_pos < source_table.active_count) : (active_pos += 1) {
        const vma_index: usize = @intCast(source_table.active_indices[active_pos]);
        dest_table.entries[vma_index] = source_table.entries[vma_index];
        dest_table.active_indices[active_pos] = @intCast(vma_index);
        source_table.entries[vma_index] = .{};
        source_table.active_indices[active_pos] = 0;
    }
    dest_table.active_count = source_table.active_count;
    source_table.active_count = 0;
}

pub fn resetProcessRuntimeTables(self: anytype, index: usize) void {
    self.releaseVmaTableForProcessIndex(index);
    self.releaseFdTableForProcessIndex(index);
    self.endpointTableForProcessIndex(index).* = .{};
}

pub fn findFreeVma(table: *const VmaTable) ?usize {
    for (table.entries[0..], 0..) |*entry, index| {
        if (!entry.active) return index;
    }
    return null;
}

pub fn installVmaEntry(table: *VmaTable, index: usize, entry: VmaEntry) void {
    if (index >= table.entries.len) return;
    const was_active = table.entries[index].active;
    table.entries[index] = entry;
    if (!was_active and entry.active) {
        std.debug.assert(table.active_count < table.active_indices.len);
        table.active_indices[table.active_count] = @intCast(index);
        table.active_count += 1;
    } else if (was_active and !entry.active and table.active_count != 0) {
        std.debug.assert(removeActiveVmaIndex(table, index));
        table.active_count -= 1;
    }
}

pub fn clearVmaEntry(table: *VmaTable, index: usize) void {
    if (index >= table.entries.len) return;
    if (table.entries[index].active and table.active_count != 0) {
        std.debug.assert(removeActiveVmaIndex(table, index));
        table.active_count -= 1;
    }
    table.entries[index] = .{};
}

pub fn removeActiveVmaIndex(table: *VmaTable, index: usize) bool {
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        if (@as(usize, @intCast(table.active_indices[active_index])) != index) continue;
        const last = table.active_count - 1;
        table.active_indices[active_index] = table.active_indices[last];
        table.active_indices[last] = 0;
        return true;
    }
    return false;
}

pub fn vmaRangeOverlaps(table: *const VmaTable, start_va: u64, size_bytes: u64) KernelError!bool {
    const end_va = try checkedEnd(start_va, size_bytes);
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = &table.entries[entry_index];
        if (start_va < entry.endVa() and end_va > entry.start_va) return true;
    }
    return false;
}

pub fn findFreeUserMapVa(
    self: anytype,
    owner: PrincipalId,
    size_bytes: u64,
    seed: u64,
) KernelError!u64 {
    if (!@TypeOf(self.*).isPageAligned(size_bytes) or size_bytes == 0) return KernelError.InvalidState;
    const base = @import("kernel_abi_root").process_abi.user_aslr_base_va;
    const end = @import("kernel_abi_root").process_abi.user_aslr_end_va;
    const granule = @import("kernel_abi_root").process_abi.user_aslr_granule;
    if (end <= base or size_bytes > end - base) return KernelError.InvalidState;
    const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
    const slots = ((end - base - size_bytes) / granule) + 1;
    if (slots == 0) return KernelError.InvalidState;
    _ = seed;
    const cursor = if (table.next_user_map_va >= base and table.next_user_map_va + size_bytes <= end)
        table.next_user_map_va
    else
        base;
    var candidate = cursor;
    while (candidate + size_bytes <= end) : (candidate += granule) {
        if (!try @TypeOf(self.*).vmaRangeOverlaps(table, candidate, size_bytes) and
            !self.rangeOverlapsPinnedUserObject(owner, candidate, size_bytes))
        {
            table.next_user_map_va = candidate + @TypeOf(self.*).pageAlignUp(size_bytes);
            return candidate;
        }
    }
    candidate = base;
    while (candidate < cursor and candidate + size_bytes <= end) : (candidate += granule) {
        if (!try @TypeOf(self.*).vmaRangeOverlaps(table, candidate, size_bytes) and
            !self.rangeOverlapsPinnedUserObject(owner, candidate, size_bytes))
        {
            table.next_user_map_va = candidate + @TypeOf(self.*).pageAlignUp(size_bytes);
            return candidate;
        }
    }
    return KernelError.TableFull;
}

pub fn userMapRangeIsFree(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
) KernelError!bool {
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes) or size_bytes == 0) return KernelError.InvalidState;
    const table = self.getVmaTableConst(owner) orelse return KernelError.InvalidState;
    return !(try @TypeOf(self.*).vmaRangeOverlaps(table, start_va, size_bytes)) and
        !self.rangeOverlapsPinnedUserObject(owner, start_va, size_bytes);
}

pub fn vmaProtAllowedByRights(prot: VmaProt, rights: FdRights) bool {
    if (prot.read and !rights.map_read) return false;
    if (prot.write and !rights.map_write) return false;
    if (prot.exec and !rights.map_exec) return false;
    return true;
}

pub fn vmaMaxProtForRights(rights: FdRights, pkey: u4) VmaProt {
    return .{
        .read = rights.map_read,
        .write = rights.map_write,
        .exec = rights.map_exec,
        .pkey = pkey,
    };
}

pub fn vmaProtAllowedByMax(prot: VmaProt, max_prot: VmaProt) bool {
    // W^X is a VMA invariant, not merely a syscall-parser convention.  The
    // ceiling may contain both bits so an RW image can transition to RX, but
    // no installed current protection may enable both simultaneously.
    if (prot.write and prot.exec) return false;
    if (prot.pkey != max_prot.pkey) return false;
    if (prot.read and !max_prot.read) return false;
    if (prot.write and !max_prot.write) return false;
    if (prot.exec and !max_prot.exec) return false;
    return true;
}

pub fn createAnonymousVmoFd(
    self: anytype,
    owner: PrincipalId,
    size_bytes: u64,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    try self.requireActiveProcess(owner);
    const vmo_ref = try self.createNativeVmo(.anonymous, size_bytes);
    try self.retainNativeVmo(vmo_ref);
    const object_ref = self.createKernelObject(.vmo, .{ .vmo = vmo_ref }) catch |err| {
        self.releaseNativeVmo(vmo_ref);
        return err;
    };
    return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
        if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
        return err;
    };
}

pub fn createAnonymousVmoFdWithPages(
    self: anytype,
    owner: PrincipalId,
    size_bytes: u64,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
    free_list: *FreePageList,
) KernelError!Fd {
    const aligned_size = @TypeOf(self.*).pageAlignUp(size_bytes);
    const page_count_u64 = aligned_size / native_page_size;
    if (size_bytes == 0 or page_count_u64 == 0 or page_count_u64 > max_vmo_backing_pages) return KernelError.InvalidState;
    const fd = try self.createAnonymousVmoFd(owner, aligned_size, rights, flags, min_fd);
    errdefer self.closeFdWithFreeList(owner, fd, free_list) catch {};
    const vmo_ref = self.nativeVmoRefForFd(owner, fd) orelse return KernelError.InvalidState;
    var pages: [max_vmo_backing_pages]u64 = undefined;
    var allocated: usize = 0;
    errdefer {
        while (allocated > 0) {
            allocated -= 1;
            free_list.appendPage(0, pages[allocated]) catch {};
        }
    }
    while (allocated < page_count_u64) : (allocated += 1) {
        pages[allocated] = (try self.allocLowPhysicalPage(free_list)).paddr;
    }
    try self.installNativeVmoPages(vmo_ref, 0, pages[0..allocated]);
    return fd;
}

pub fn createContiguousVmoFdWithPages(
    self: anytype,
    owner: PrincipalId,
    size_bytes: u64,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
    free_list: *FreePageList,
) KernelError!Fd {
    const dma_pool_max_pages = @import("kernel_abi_root").capsule_abi.dma_pool_max_pages;
    const max_size_bytes = @as(u64, @intCast(dma_pool_max_pages)) * native_page_size;
    if (size_bytes == 0 or size_bytes > max_size_bytes) return KernelError.InvalidState;
    const aligned_size = @TypeOf(self.*).pageAlignUp(size_bytes);
    const page_count: usize = @intCast(aligned_size / native_page_size);
    if (page_count == 0 or page_count > dma_pool_max_pages) return KernelError.InvalidState;

    const fd = try self.createAnonymousVmoFd(owner, aligned_size, rights, flags, min_fd);
    errdefer self.closeFdWithFreeList(owner, fd, free_list) catch {};
    const vmo_ref = self.nativeVmoRefForFd(owner, fd) orelse return KernelError.InvalidState;
    const base = try free_list.popContiguousBelow(page_count, @TypeOf(self.*).low_memory_limit);
    errdefer free_list.appendContiguousRange(0, base, page_count) catch {};

    var pages: [dma_pool_max_pages]u64 = undefined;
    for (pages[0..page_count], 0..) |*paddr, page_index| {
        paddr.* = base + (@as(u64, @intCast(page_index)) * native_page_size);
        try self.zeroAllocatedPage(paddr.*);
    }
    try self.installNativeVmoPages(vmo_ref, 0, pages[0..page_count]);
    return fd;
}

pub fn createAnonymousVmaWithPages(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    prot: VmaProt,
    max_prot: VmaProt,
    flags: MmapFlags,
    free_list: *FreePageList,
) KernelError!NativeVmoRef {
    try self.requireActiveProcess(owner);
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes)) return KernelError.InvalidState;
    const page_count_u64 = size_bytes / native_page_size;
    if (page_count_u64 == 0 or page_count_u64 > max_vmo_backing_pages) return KernelError.InvalidState;
    if (!@TypeOf(self.*).vmaProtAllowedByMax(prot, max_prot)) return KernelError.InvalidState;

    const vma_table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
    if (try @TypeOf(self.*).vmaRangeOverlaps(vma_table, start_va, size_bytes)) return KernelError.InvalidState;
    const vma_index = @TypeOf(self.*).findFreeVma(vma_table) orelse return KernelError.TableFull;

    const vmo_ref = try self.createNativeVmo(.anonymous, size_bytes);
    try self.retainNativeVmo(vmo_ref);
    errdefer self.releaseNativeVmoWithFreeList(vmo_ref, free_list);

    @TypeOf(self.*).installVmaEntry(vma_table, vma_index, .{
        .active = true,
        .start_va = start_va,
        .size_bytes = size_bytes,
        .prot = prot,
        .max_prot = max_prot,
        .flags = flags,
        .vmo = vmo_ref,
        .vmo_offset = 0,
    });
    return vmo_ref;
}

pub fn createVmaWithRetainedVmo(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    prot: VmaProt,
    max_prot: VmaProt,
    flags: MmapFlags,
    vmo_ref: NativeVmoRef,
    vmo_offset: u64,
) KernelError!void {
    try self.requireActiveProcess(owner);
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes) or !@TypeOf(self.*).isPageAligned(vmo_offset)) return KernelError.InvalidState;
    const vmo = self.nativeVmoSlotConst(vmo_ref) orelse return KernelError.InvalidState;
    const vmo_end = try @TypeOf(self.*).checkedEnd(vmo_offset, size_bytes);
    if (vmo_end > vmo.size_bytes) return KernelError.InvalidState;
    if (!@TypeOf(self.*).vmaProtAllowedByMax(prot, max_prot)) return KernelError.InvalidState;

    const vma_table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
    if (try @TypeOf(self.*).vmaRangeOverlaps(vma_table, start_va, size_bytes)) return KernelError.InvalidState;
    const vma_index = @TypeOf(self.*).findFreeVma(vma_table) orelse return KernelError.TableFull;
    try self.retainNativeVmo(vmo_ref);
    @TypeOf(self.*).installVmaEntry(vma_table, vma_index, .{
        .active = true,
        .start_va = start_va,
        .size_bytes = size_bytes,
        .prot = prot,
        .max_prot = max_prot,
        .flags = flags,
        .vmo = vmo_ref,
        .vmo_offset = vmo_offset,
    });
}

pub fn mmapFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    start_va: u64,
    size_bytes: u64,
    prot: VmaProt,
    flags: MmapFlags,
    vmo_offset: u64,
) KernelError!u64 {
    const fd_entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!@TypeOf(self.*).vmaProtAllowedByRights(prot, fd_entry.rights)) return KernelError.InvalidState;
    const max_prot = @TypeOf(self.*).vmaMaxProtForRights(fd_entry.rights, flags.pkey);
    const object_slot = self.kernelObjectSlotConst(fd_entry.object) orelse return KernelError.InvalidState;
    const vmo_ref = switch (object_slot.payload) {
        .vmo => |ref| ref,
        else => return KernelError.InvalidState,
    };
    try self.createVmaWithRetainedVmo(owner, start_va, size_bytes, prot, max_prot, flags, vmo_ref, vmo_offset);
    return start_va;
}

pub fn mmapFdIntoProcess(
    self: anytype,
    source_owner: PrincipalId,
    fd: Fd,
    target_owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    prot: VmaProt,
    flags: MmapFlags,
    vmo_offset: u64,
) KernelError!u64 {
    _ = try self.fdTableForActiveProcessConst(source_owner);
    try self.requireActiveProcess(target_owner);
    const fd_entry = self.fdEntryConst(source_owner, fd) orelse return KernelError.InvalidState;
    if (!@TypeOf(self.*).vmaProtAllowedByRights(prot, fd_entry.rights)) return KernelError.InvalidState;
    const max_prot = @TypeOf(self.*).vmaMaxProtForRights(fd_entry.rights, flags.pkey);
    const object_slot = self.kernelObjectSlotConst(fd_entry.object) orelse return KernelError.InvalidState;
    const vmo_ref = switch (object_slot.payload) {
        .vmo => |ref| ref,
        else => return KernelError.InvalidState,
    };
    try self.createVmaWithRetainedVmo(target_owner, start_va, size_bytes, prot, max_prot, flags, vmo_ref, vmo_offset);
    return start_va;
}

const VmaCutPreflight = struct {
    middle_index: ?usize = null,
    middle_entry: ?VmaEntry = null,
};

pub const MunmapPrepared = struct {
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    middle: PreparedMiddleCut = .{},
    active: bool = true,
};

pub const FixedMmapPrepared = struct {
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    target_middle: PreparedMiddleCut = .{},
    destination_index: usize = 0,
    destination: VmaEntry = .{},
    active: bool = true,
};

const PreparedMiddleCut = struct {
    source_index: usize = 0,
    suffix_index: usize = 0,
    suffix: VmaEntry = .{},
    active: bool = false,
};

pub const MremapPrepared = struct {
    owner: PrincipalId,
    old_start: u64,
    old_size: u64,
    target_start: u64,
    new_size: u64,
    fixed: bool,
    in_place: bool,
    destination_index: usize = 0,
    destination: VmaEntry = .{},
    target_middle: PreparedMiddleCut = .{},
    source_middle: PreparedMiddleCut = .{},
    active: bool = true,
};

fn preflightVmaCut(
    self: anytype,
    table: *const VmaTable,
    start_va: u64,
    size_bytes: u64,
) KernelError!VmaCutPreflight {
    const end_va = try @TypeOf(self.*).checkedEnd(start_va, size_bytes);
    var result: VmaCutPreflight = .{};
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = table.entries[entry_index];
        if (!entry.active or start_va >= entry.endVa() or end_va <= entry.start_va) continue;
        const cut_start = @max(start_va, entry.start_va);
        const cut_end = @min(end_va, entry.endVa());
        if (cut_start <= entry.start_va and cut_end >= entry.endVa()) {
            continue;
        } else if (cut_start > entry.start_va and cut_end < entry.endVa()) {
            if (result.middle_entry != null) return KernelError.InvalidState;
            result.middle_index = entry_index;
            result.middle_entry = entry;
        }
    }
    return result;
}

fn entryFullyCoveredByRange(entry: *const VmaEntry, start_va: u64, size_bytes: u64) bool {
    const end_va = start_va + size_bytes;
    return entry.active and start_va <= entry.start_va and end_va >= entry.endVa();
}

fn selectPreparedVmaIndex(
    table: *const VmaTable,
    target_start: u64,
    target_size: u64,
    may_reuse_target_full: bool,
    excluded: []const usize,
) ?usize {
    for (table.entries[0..], 0..) |*entry, index| {
        var is_excluded = false;
        for (excluded) |excluded_index| {
            if (index == excluded_index) {
                is_excluded = true;
                break;
            }
        }
        if (is_excluded) continue;
        if (!entry.active or
            (may_reuse_target_full and entryFullyCoveredByRange(entry, target_start, target_size)))
        {
            return index;
        }
    }
    return null;
}

fn prepareMiddleCut(
    self: anytype,
    preflight: VmaCutPreflight,
    cut_start: u64,
    cut_size: u64,
    suffix_index: usize,
) KernelError!PreparedMiddleCut {
    const original = preflight.middle_entry orelse return .{};
    const source_index = preflight.middle_index orelse return KernelError.InvalidState;
    const cut_end = try @TypeOf(self.*).checkedEnd(cut_start, cut_size);
    try self.retainNativeVmo(original.vmo);
    if (!original.cow_table.isNull()) {
        self.retainNativeCowTable(original.cow_table) catch |err| {
            self.releaseNativeVmo(original.vmo);
            return err;
        };
    }
    var suffix = original;
    suffix.start_va = cut_end;
    suffix.size_bytes = original.endVa() - cut_end;
    suffix.vmo_offset = original.vmo_offset + (cut_end - original.start_va);
    suffix.cow_page_offset = original.cow_page_offset +
        @as(u32, @intCast((cut_end - original.start_va) / native_page_size));
    return .{
        .source_index = source_index,
        .suffix_index = suffix_index,
        .suffix = suffix,
        .active = true,
    };
}

fn discardPreparedMiddleCut(
    self: anytype,
    prepared: *PreparedMiddleCut,
    free_list: *FreePageList,
) void {
    if (!prepared.active) return;
    if (!prepared.suffix.cow_table.isNull()) {
        self.releaseNativeCowTable(prepared.suffix.cow_table, free_list);
    }
    self.releaseNativeVmoWithFreeList(prepared.suffix.vmo, free_list);
    prepared.active = false;
}

pub fn prepareMunmapRangeWithFreeList(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    free_list: *FreePageList,
) KernelError!MunmapPrepared {
    try self.requireActiveProcess(owner);
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes)) {
        return KernelError.InvalidState;
    }
    if (size_bytes == 0) return KernelError.InvalidState;
    const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
    const preflight = try preflightVmaCut(self, table, start_va, size_bytes);
    var prepared: MunmapPrepared = .{
        .owner = owner,
        .start_va = start_va,
        .size_bytes = size_bytes,
    };
    errdefer discardPreparedMiddleCut(self, &prepared.middle, free_list);
    if (preflight.middle_entry != null) {
        const suffix_index = selectPreparedVmaIndex(table, 0, 0, false, &.{}) orelse
            return KernelError.TableFull;
        prepared.middle = try prepareMiddleCut(
            self,
            preflight,
            start_va,
            size_bytes,
            suffix_index,
        );
    }
    return prepared;
}

pub fn discardMunmapPrepared(
    self: anytype,
    prepared: *MunmapPrepared,
    free_list: *FreePageList,
) void {
    if (!prepared.active) return;
    discardPreparedMiddleCut(self, &prepared.middle, free_list);
    prepared.active = false;
}

pub fn commitMunmapPrepared(
    self: anytype,
    prepared: *MunmapPrepared,
    free_list: *FreePageList,
) void {
    std.debug.assert(prepared.active);
    munmapRangeWithFreeListInternal(
        self,
        prepared.owner,
        prepared.start_va,
        prepared.size_bytes,
        free_list,
        &prepared.middle,
    ) catch unreachable;
    prepared.active = false;
}

fn prepareFixedMmapSlots(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    free_list: *FreePageList,
) KernelError!FixedMmapPrepared {
    try self.requireActiveProcess(owner);
    if (size_bytes == 0) return KernelError.InvalidState;
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes)) {
        return KernelError.InvalidState;
    }
    const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
    const target_cut = try preflightVmaCut(self, table, start_va, size_bytes);
    var excluded: [1]usize = undefined;
    var excluded_count: usize = 0;
    const target_suffix_index = if (target_cut.middle_entry != null) blk: {
        const index = selectPreparedVmaIndex(table, start_va, size_bytes, false, &.{}) orelse
            return KernelError.TableFull;
        excluded[0] = index;
        excluded_count = 1;
        break :blk index;
    } else 0;
    const destination_index = selectPreparedVmaIndex(
        table,
        start_va,
        size_bytes,
        true,
        excluded[0..excluded_count],
    ) orelse return KernelError.TableFull;
    var prepared: FixedMmapPrepared = .{
        .owner = owner,
        .start_va = start_va,
        .size_bytes = size_bytes,
        .destination_index = destination_index,
    };
    errdefer discardFixedMmapPrepared(self, &prepared, free_list);
    if (target_cut.middle_entry != null) {
        prepared.target_middle = try prepareMiddleCut(
            self,
            target_cut,
            start_va,
            size_bytes,
            target_suffix_index,
        );
    }
    return prepared;
}

pub fn prepareFixedAnonymousMmap(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    prot: VmaProt,
    max_prot: VmaProt,
    flags: MmapFlags,
    free_list: *FreePageList,
) KernelError!FixedMmapPrepared {
    if (!flags.anonymous or flags.shared and flags.private or
        !@TypeOf(self.*).vmaProtAllowedByMax(prot, max_prot))
    {
        return KernelError.InvalidState;
    }
    const page_count = size_bytes / native_page_size;
    if (page_count == 0 or page_count > max_vmo_backing_pages) return KernelError.InvalidState;
    var prepared = try prepareFixedMmapSlots(self, owner, start_va, size_bytes, free_list);
    errdefer discardFixedMmapPrepared(self, &prepared, free_list);
    const vmo_ref = try self.createNativeVmo(.anonymous, size_bytes);
    self.retainNativeVmo(vmo_ref) catch unreachable;
    prepared.destination = .{
        .active = true,
        .start_va = start_va,
        .size_bytes = size_bytes,
        .prot = prot,
        .max_prot = max_prot,
        .flags = flags,
        .vmo = vmo_ref,
    };
    return prepared;
}

pub fn prepareFixedFdMmap(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    start_va: u64,
    size_bytes: u64,
    prot: VmaProt,
    flags: MmapFlags,
    vmo_offset: u64,
    free_list: *FreePageList,
) KernelError!FixedMmapPrepared {
    if (flags.anonymous or !flags.private or flags.shared) return KernelError.InvalidState;
    if (!@TypeOf(self.*).isPageAligned(vmo_offset)) return KernelError.InvalidState;
    const fd_entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!@TypeOf(self.*).vmaProtAllowedByRights(prot, fd_entry.rights)) return KernelError.InvalidState;
    const max_prot = @TypeOf(self.*).vmaMaxProtForRights(fd_entry.rights, flags.pkey);
    const object_slot = self.kernelObjectSlotConst(fd_entry.object) orelse return KernelError.InvalidState;
    const vmo_ref = switch (object_slot.payload) {
        .vmo => |ref| ref,
        else => return KernelError.InvalidState,
    };
    const vmo = self.nativeVmoSlotConst(vmo_ref) orelse return KernelError.InvalidState;
    const vmo_end = try @TypeOf(self.*).checkedEnd(vmo_offset, size_bytes);
    if (vmo_end > vmo.size_bytes or !@TypeOf(self.*).vmaProtAllowedByMax(prot, max_prot)) {
        return KernelError.InvalidState;
    }
    var prepared = try prepareFixedMmapSlots(self, owner, start_va, size_bytes, free_list);
    errdefer discardFixedMmapPrepared(self, &prepared, free_list);
    try self.retainNativeVmo(vmo_ref);
    prepared.destination = .{
        .active = true,
        .start_va = start_va,
        .size_bytes = size_bytes,
        .prot = prot,
        .max_prot = max_prot,
        .flags = flags,
        .vmo = vmo_ref,
        .vmo_offset = vmo_offset,
    };
    return prepared;
}

pub fn discardFixedMmapPrepared(
    self: anytype,
    prepared: *FixedMmapPrepared,
    free_list: *FreePageList,
) void {
    if (!prepared.active) return;
    discardPreparedMiddleCut(self, &prepared.target_middle, free_list);
    if (prepared.destination.active) {
        self.releaseNativeVmoWithFreeList(prepared.destination.vmo, free_list);
        prepared.destination.active = false;
    }
    prepared.active = false;
}

pub fn commitFixedMmapPrepared(
    self: anytype,
    prepared: *FixedMmapPrepared,
    free_list: *FreePageList,
) void {
    std.debug.assert(prepared.active);
    std.debug.assert(prepared.destination.active);
    munmapRangeWithFreeListInternal(
        self,
        prepared.owner,
        prepared.start_va,
        prepared.size_bytes,
        free_list,
        &prepared.target_middle,
    ) catch unreachable;
    const table = self.getVmaTable(prepared.owner) orelse unreachable;
    std.debug.assert(prepared.destination_index < table.entries.len);
    std.debug.assert(!table.entries[prepared.destination_index].active);
    @TypeOf(self.*).installVmaEntry(table, prepared.destination_index, prepared.destination);
    prepared.destination.active = false;
    prepared.active = false;
}

fn munmapRangeWithFreeListInternal(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    free_list: *FreePageList,
    prepared_middle: ?*PreparedMiddleCut,
) KernelError!void {
    try self.requireActiveProcess(owner);
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes)) return KernelError.InvalidState;
    const end_va = try @TypeOf(self.*).checkedEnd(start_va, size_bytes);
    const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;

    var index: usize = 0;
    while (index < table.entries.len) : (index += 1) {
        var entry = &table.entries[index];
        if (!entry.active) continue;
        const entry_start = entry.start_va;
        const entry_end = entry.endVa();
        if (start_va >= entry_end or end_va <= entry_start) {
            continue;
        }

        const cut_start = @max(start_va, entry_start);
        const cut_end = @min(end_va, entry_end);
        const cut_vmo_first_page: usize = @intCast((entry.vmo_offset + (cut_start - entry_start)) / native_page_size);
        const cut_page_count: usize = @intCast((cut_end - cut_start) / native_page_size);
        if (cut_start <= entry_start and cut_end >= entry_end) {
            const vmo_ref = entry.vmo;
            const can_release_unmapped_pages = entry.flags.anonymous or self.nativeVmoIsShadow(vmo_ref);
            const vmo_ref_count = if (can_release_unmapped_pages) self.nativeVmoRefCount(vmo_ref) orelse 0 else 0;
            self.releaseVmaCowResources(entry, free_list);
            @TypeOf(self.*).clearVmaEntry(table, index);
            if (can_release_unmapped_pages and vmo_ref_count != 1) {
                self.releaseUnmappedAnonymousVmoPageRange(owner, vmo_ref, cut_vmo_first_page, cut_page_count, free_list);
            }
            self.releaseNativeVmoWithFreeList(vmo_ref, free_list);
            continue;
        }

        if (cut_start <= entry_start) {
            const trim = cut_end - entry_start;
            self.releaseVmaCowPageRange(entry, 0, @intCast(trim / native_page_size), free_list);
            entry.start_va = cut_end;
            entry.size_bytes = entry_end - cut_end;
            entry.vmo_offset += trim;
            entry.cow_page_offset += @intCast(trim / native_page_size);
            if (entry.flags.anonymous or self.nativeVmoIsShadow(entry.vmo)) {
                self.releaseUnmappedAnonymousVmoPageRange(owner, entry.vmo, cut_vmo_first_page, cut_page_count, free_list);
            }
            continue;
        }

        if (cut_end >= entry_end) {
            const vmo_ref = entry.vmo;
            const can_release_unmapped_pages = entry.flags.anonymous or self.nativeVmoIsShadow(vmo_ref);
            const cut_first_delta: usize = @intCast((cut_start - entry_start) / native_page_size);
            entry.size_bytes = cut_start - entry_start;
            self.releaseVmaCowPageRange(entry, cut_first_delta, cut_page_count, free_list);
            if (can_release_unmapped_pages) {
                self.releaseUnmappedAnonymousVmoPageRange(owner, vmo_ref, cut_vmo_first_page, cut_page_count, free_list);
            }
            continue;
        }

        const original = entry.*;
        if (prepared_middle) |prepared| {
            if (!prepared.active or prepared.source_index != index) return KernelError.InvalidState;
            if (prepared.suffix_index >= table.entries.len or table.entries[prepared.suffix_index].active) {
                return KernelError.InvalidState;
            }
            @TypeOf(self.*).installVmaEntry(table, prepared.suffix_index, prepared.suffix);
            prepared.active = false;
        } else {
            const after_index = @TypeOf(self.*).findFreeVma(table) orelse return KernelError.TableFull;
            try self.retainNativeVmo(original.vmo);
            errdefer self.releaseNativeVmo(original.vmo);
            if (!original.cow_table.isNull()) try self.retainNativeCowTable(original.cow_table);
            errdefer if (!original.cow_table.isNull()) self.releaseNativeCowTable(original.cow_table, null);
            @TypeOf(self.*).installVmaEntry(table, after_index, original);
            table.entries[after_index].start_va = cut_end;
            table.entries[after_index].size_bytes = entry_end - cut_end;
            table.entries[after_index].vmo_offset = original.vmo_offset + (cut_end - entry_start);
            table.entries[after_index].cow_page_offset = original.cow_page_offset + @as(u32, @intCast((cut_end - entry_start) / native_page_size));
        }

        entry = &table.entries[index];
        entry.size_bytes = cut_start - entry_start;
        self.releaseVmaCowPageRange(entry, @intCast((cut_start - entry_start) / native_page_size), cut_page_count, free_list);
        if (original.flags.anonymous or self.nativeVmoIsShadow(original.vmo)) {
            self.releaseUnmappedAnonymousVmoPageRange(owner, original.vmo, cut_vmo_first_page, cut_page_count, free_list);
        }
    }
}

pub fn munmapRangeWithFreeList(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    free_list: *FreePageList,
) KernelError!void {
    return munmapRangeWithFreeListInternal(self, owner, start_va, size_bytes, free_list, null);
}

pub fn discardMremapPrepared(
    self: anytype,
    prepared: *MremapPrepared,
    free_list: *FreePageList,
) void {
    if (!prepared.active) return;
    discardPreparedMiddleCut(self, &prepared.target_middle, free_list);
    discardPreparedMiddleCut(self, &prepared.source_middle, free_list);
    if (!prepared.in_place and prepared.destination.active) {
        self.releaseVmaCowResources(&prepared.destination, free_list);
        self.releaseNativeVmoWithFreeList(prepared.destination.vmo, free_list);
    }
    prepared.active = false;
}

pub fn prepareMremapWithFreeList(
    self: anytype,
    owner: PrincipalId,
    old_start: u64,
    old_size: u64,
    new_size: u64,
    new_start: u64,
    may_move: bool,
    fixed: bool,
    free_list: *FreePageList,
) KernelError!MremapPrepared {
    try self.requireActiveProcess(owner);
    if (!@TypeOf(self.*).isPageAligned(old_start) or !@TypeOf(self.*).isPageAligned(old_size) or !@TypeOf(self.*).isPageAligned(new_size)) return KernelError.InvalidState;
    if (fixed and !@TypeOf(self.*).isPageAligned(new_start)) return KernelError.InvalidState;
    if (old_size == 0 or new_size == 0) return KernelError.InvalidState;
    const old_end = try @TypeOf(self.*).checkedEnd(old_start, old_size);

    const source = self.vmaEntryForVaConst(owner, old_start) orelse return KernelError.InvalidState;
    if (old_end > source.endVa()) return KernelError.InvalidState;
    if (!source.flags.anonymous) return KernelError.InvalidState;
    const source_snapshot = source.*;
    const source_delta = old_start - source_snapshot.start_va;
    const source_vmo_offset = source_snapshot.vmo_offset + source_delta;
    const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
    if (new_size > old_size and source_snapshot.flags.shared) return KernelError.InvalidState;

    if (!fixed and new_size <= old_size) {
        const shrink_cut = if (new_size < old_size)
            try preflightVmaCut(self, table, old_start + new_size, old_size - new_size)
        else
            VmaCutPreflight{};
        var prepared: MremapPrepared = .{
            .owner = owner,
            .old_start = old_start,
            .old_size = old_size,
            .target_start = old_start,
            .new_size = new_size,
            .fixed = false,
            .in_place = true,
        };
        errdefer self.discardMremapPrepared(&prepared, free_list);
        if (shrink_cut.middle_entry != null) {
            const suffix_index = selectPreparedVmaIndex(table, 0, 0, false, &.{}) orelse
                return KernelError.TableFull;
            prepared.source_middle = try prepareMiddleCut(
                self,
                shrink_cut,
                old_start + new_size,
                old_size - new_size,
                suffix_index,
            );
        }
        return prepared;
    }
    if (!may_move) return KernelError.OutOfFreePages;
    if (new_start == 0) return KernelError.InvalidState;

    const target_start = new_start;
    const target_end = try @TypeOf(self.*).checkedEnd(target_start, new_size);
    if (target_start < old_end and target_end > old_start) return KernelError.InvalidState;
    // Moving into another part of the containing source VMA would make the
    // target cut mutate the source snapshot before it is committed.  Reject
    // that ambiguous case instead of relying on mutation order.
    if (target_start < source_snapshot.endVa() and target_end > source_snapshot.start_va) {
        return KernelError.InvalidState;
    }

    const target_cut = if (fixed)
        try preflightVmaCut(self, table, target_start, new_size)
    else blk: {
        if (try @TypeOf(self.*).vmaRangeOverlaps(table, target_start, new_size)) return KernelError.InvalidState;
        break :blk VmaCutPreflight{};
    };
    const source_cut = try preflightVmaCut(self, table, old_start, old_size);

    var excluded_indices: [2]usize = undefined;
    var excluded_count: usize = 0;
    const target_suffix_index = if (target_cut.middle_entry != null) blk: {
        const index = selectPreparedVmaIndex(table, target_start, new_size, false, excluded_indices[0..0]) orelse
            return KernelError.TableFull;
        excluded_indices[excluded_count] = index;
        excluded_count += 1;
        break :blk index;
    } else 0;
    const destination_index = selectPreparedVmaIndex(
        table,
        target_start,
        new_size,
        true,
        excluded_indices[0..excluded_count],
    ) orelse return KernelError.TableFull;
    excluded_indices[excluded_count] = destination_index;
    excluded_count += 1;
    const source_suffix_index = if (source_cut.middle_entry != null)
        selectPreparedVmaIndex(
            table,
            target_start,
            new_size,
            true,
            excluded_indices[0..excluded_count],
        ) orelse return KernelError.TableFull
    else
        0;

    var prepared: MremapPrepared = .{
        .owner = owner,
        .old_start = old_start,
        .old_size = old_size,
        .target_start = target_start,
        .new_size = new_size,
        .fixed = fixed,
        .in_place = false,
        .destination_index = destination_index,
    };
    errdefer self.discardMremapPrepared(&prepared, free_list);
    if (target_cut.middle_entry != null) {
        prepared.target_middle = try prepareMiddleCut(
            self,
            target_cut,
            target_start,
            new_size,
            target_suffix_index,
        );
    }
    if (source_cut.middle_entry != null) {
        prepared.source_middle = try prepareMiddleCut(
            self,
            source_cut,
            old_start,
            old_size,
            source_suffix_index,
        );
    }

    if (new_size <= old_size) {
        try self.retainNativeVmo(source_snapshot.vmo);
        if (!source_snapshot.cow_table.isNull()) {
            self.retainNativeCowTable(source_snapshot.cow_table) catch |err| {
                self.releaseNativeVmo(source_snapshot.vmo);
                return err;
            };
        }
        prepared.destination = source_snapshot;
        prepared.destination.start_va = target_start;
        prepared.destination.size_bytes = new_size;
        prepared.destination.vmo_offset = source_vmo_offset;
        prepared.destination.cow_page_offset = source_snapshot.cow_page_offset +
            @as(u32, @intCast(source_delta / native_page_size));
    } else {
        // A grown mapping is independent of older fork siblings.  Materialize
        // this process's current view into a fresh anonymous VMO, preferring
        // dirty COW pages over the shared base image and preserving holes.
        var materialized_flags = source_snapshot.flags;
        materialized_flags.fork_cow = false;
        const dst_vmo = try self.createNativeVmo(.anonymous, new_size);
        try self.retainNativeVmo(dst_vmo);
        prepared.destination = .{
            .active = true,
            .start_va = target_start,
            .size_bytes = new_size,
            .prot = source_snapshot.prot,
            .max_prot = source_snapshot.max_prot,
            .flags = materialized_flags,
            .vmo = dst_vmo,
        };

        const copy_pages: usize = @intCast(old_size / native_page_size);
        const source_first_page: usize = @intCast(source_vmo_offset / native_page_size);
        var page_index: usize = 0;
        while (page_index < copy_pages) : (page_index += 1) {
            const source_va = old_start + @as(u64, @intCast(page_index)) * native_page_size;
            var src_paddr = self.entryDirtyPagePaddr(&source_snapshot, source_va) orelse
                (self.nativeVmoPagePaddrOrHole(source_snapshot.vmo, source_first_page + page_index) orelse return KernelError.InvalidState);
            if (src_paddr == 0 and self.nativeVmoIsShadow(source_snapshot.vmo)) {
                src_paddr = self.nativeVmoResolvedPagePaddr(
                    source_snapshot.vmo,
                    source_first_page + page_index,
                ) orelse 0;
            }
            if (src_paddr == 0) continue;
            const dst_paddr = (try self.allocPhysicalPage(free_list)).paddr;
            @TypeOf(self.*).copyPhysicalPage(dst_paddr, src_paddr);
            var page = [_]u64{dst_paddr};
            self.installNativeVmoPages(dst_vmo, page_index, page[0..]) catch |err| {
                free_list.appendPage(0, dst_paddr) catch {};
                return err;
            };
        }
    }

    return prepared;
}

/// Commit only after the syscall layer has invalidated source translations,
/// prepared every fallible destination resource, and made fixed-target PTE
/// teardown non-fallible.  `prepareMremapWithFreeList` proves every retain and
/// VMA-slot operation performed by the existing cut helper below.
pub fn commitMremapPrepared(
    self: anytype,
    prepared: *MremapPrepared,
    free_list: *FreePageList,
) u64 {
    std.debug.assert(prepared.active);
    if (prepared.in_place) {
        if (prepared.new_size < prepared.old_size) {
            munmapRangeWithFreeListInternal(
                self,
                prepared.owner,
                prepared.old_start + prepared.new_size,
                prepared.old_size - prepared.new_size,
                free_list,
                &prepared.source_middle,
            ) catch unreachable;
        }
        prepared.active = false;
        return prepared.old_start;
    }

    if (prepared.fixed) {
        munmapRangeWithFreeListInternal(
            self,
            prepared.owner,
            prepared.target_start,
            prepared.new_size,
            free_list,
            &prepared.target_middle,
        ) catch unreachable;
    }
    const table = self.getVmaTable(prepared.owner) orelse unreachable;
    std.debug.assert(prepared.destination_index < table.entries.len);
    std.debug.assert(!table.entries[prepared.destination_index].active);
    @TypeOf(self.*).installVmaEntry(table, prepared.destination_index, prepared.destination);
    prepared.destination.active = false;
    munmapRangeWithFreeListInternal(
        self,
        prepared.owner,
        prepared.old_start,
        prepared.old_size,
        free_list,
        &prepared.source_middle,
    ) catch unreachable;

    if (prepared.new_size < prepared.old_size) {
        const destination = &table.entries[prepared.destination_index];
        const kept_pages: usize = @intCast(prepared.new_size / native_page_size);
        const tail_pages: usize = @intCast((prepared.old_size - prepared.new_size) / native_page_size);
        // The source removal may have deferred cleanup while the destination
        // still retained the same VMO/COW table.  Re-run the precise tail cut
        // after ownership transfer; sharing checks keep pages needed by any
        // remaining alias or fork sibling.
        self.releaseVmaCowPageRange(destination, kept_pages, tail_pages, free_list);
        const first_vmo_page = @as(usize, @intCast(destination.vmo_offset / native_page_size)) + kept_pages;
        self.releaseUnmappedAnonymousVmoPageRange(
            prepared.owner,
            destination.vmo,
            first_vmo_page,
            tail_pages,
            free_list,
        );
    }
    prepared.active = false;
    return prepared.target_start;
}

pub fn mremapRangeWithFreeList(
    self: anytype,
    owner: PrincipalId,
    old_start: u64,
    old_size: u64,
    new_size: u64,
    new_start: u64,
    may_move: bool,
    fixed: bool,
    free_list: *FreePageList,
) KernelError!u64 {
    const target_start = if (fixed)
        new_start
    else if (new_size > old_size)
        try self.findRandomizedFreeUserMapVa(owner, new_size, 0x4d52_454d_4150_0000)
    else
        old_start;
    var prepared = try self.prepareMremapWithFreeList(
        owner,
        old_start,
        old_size,
        new_size,
        target_start,
        may_move,
        fixed,
        free_list,
    );
    defer self.discardMremapPrepared(&prepared, free_list);
    return self.commitMremapPrepared(&prepared, free_list);
}
