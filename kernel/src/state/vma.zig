const references = types.references;
const user_vm = @import("../memory/user_vm.zig");
const std = @import("std");
const smp_perf = @import("../smp_perf.zig");
const builtin = @import("builtin");
const kernel_log = @import("../kernel_log.zig");
const x86_platform = @import("../arch/x86_64/platform.zig");
const types = @import("types.zig");
var backing_failure_diagnosed: u8 = 0;
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
const NativeVmaDmaPinMode = types.NativeVmaDmaPinMode;
const NativeVmaFaultPlan = types.NativeVmaFaultPlan;
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

pub fn rangeOverlapsPinnedUserObjectExcept(self: anytype, owner: PrincipalId, start_va: u64, size_bytes: u64, except_object: ?KernelObjectRef) bool {
    const result = indexedPinnedOverlap(self, owner, start_va, size_bytes, except_object);
    if (comptime @import("reference_check.zig").pin_oracle_enabled) {
        if (rangeOverlapsPinnedUserObjectExceptOld(self, owner, start_va, size_bytes, except_object) != result) @import("reference_check.zig").failure(error.PinnedMismatch);
    }
    return result;
}

pub fn pinnedObjectCursor(self: anytype, owner: PrincipalId) references.Cursor {
    const index = processIndexFromPrincipal(owner) orelse return .{};
    const table = self.vmaTableForProcessIndex(index) orelse return .{};
    return references.Cursor.init(&table.pinned_objects);
}
// Runtime callers hold K. AS recursion is deliberate: DERIVE/VM mutation
// already hold A, while PROCESS_CLONE/EXEC inspect pins before their AS pair.
// Serial logical host fixtures can have no architectural AS; runtime cannot.
pub fn lockPinIndex(owner: PrincipalId) bool {
    const locked = user_vm.lockAddressSpace(owner);
    if (!locked and !@import("builtin").is_test) @panic("pin index lost owner AS");
    return locked;
}
pub fn unlockPinIndex(owner: PrincipalId, locked: bool) void {
    if (locked) user_vm.unlockAddressSpace(owner);
}
pub fn pinExcept(object: ?KernelObjectRef) ?references.Identity {
    const o = object orelse return null;
    return .{ .container = @intFromEnum(o.kind), .index = o.index, .generation = o.generation };
}
fn indexedPinnedOverlap(self: anytype, owner: PrincipalId, start: u64, size: u64, except: ?KernelObjectRef) bool {
    const locked = lockPinIndex(owner);
    defer unlockPinIndex(owner, locked);
    const table = self.getVmaTableConst(owner) orelse return false;
    return table.pinned_index.anyOverlap(start, size, 15, pinExcept(except));
}

fn rangeOverlapsPinnedUserObjectExceptOld(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    except_object: ?KernelObjectRef,
) bool {
    const owner_raw: PrincipalRaw = @intFromEnum(owner);
    var candidates = self.fd_objects.marked(1);
    while (candidates.next()) |object_index| {
        const slot = (self.fd_objects.get(@intCast(object_index)) orelse continue);
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
/// mappings additionally require an active IOMMU so every alias owns an independent
/// IOVA allocation; pass-through retains only its existing linear-mapping alias.
pub fn rangeConflictsWithDmaDerivation(self: anytype, owner: PrincipalId, start_va: u64, size_bytes: u64, except_object: ?KernelObjectRef, allow_dma_mapping_aliases: bool, iommu_active: bool) bool {
    const locked = lockPinIndex(owner);
    defer unlockPinIndex(owner, locked);
    const table = self.getVmaTableConst(owner) orelse return false;
    const index = &table.pinned_index;
    const mask: u4 = 2 | (if (!allow_dma_mapping_aliases) @as(u4, 4) else 0) | (if (!allow_dma_mapping_aliases or !iommu_active) @as(u4, 8) else 0);
    // MMIO is never an alias exception. Allowed mapping classes are excluded
    // BEFORE traversal, even when thousands of them overlap the query VA.
    const result = index.anyOverlap(start_va, size_bytes, 1, null) or index.anyOverlap(start_va, size_bytes, mask, pinExcept(except_object));
    if (comptime @import("reference_check.zig").pin_oracle_enabled) {
        if (rangeConflictsWithDmaDerivationOracle(self, owner, start_va, size_bytes, except_object, allow_dma_mapping_aliases, iommu_active) != result) @import("reference_check.zig").failure(error.PinnedMismatch);
    }
    return result;
}
fn rangeConflictsWithDmaDerivationOracle(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    except_object: ?KernelObjectRef,
    allow_dma_mapping_aliases: bool,
    iommu_active: bool,
) bool {
    const owner_raw: PrincipalRaw = @intFromEnum(owner);
    var candidates = pinnedObjectCursor(self, owner);
    while (candidates.next()) |edge| {
        const object_index = edge.identity.index;
        const slot = self.kernelObjectSlot(.{ .kind = @enumFromInt(edge.identity.container), .index = object_index, .generation = edge.identity.generation }) orelse unreachable;
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
                    iommu_active,
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
                    iommu_active,
                );
                return true;
            },
            .dma_mapping => |mapping| if (!is_except and
                (!allow_dma_mapping_aliases or (mapping.page_count != 0 and !iommu_active)))
            {
                pachaTraceDmaDerivationConflict(
                    "dma_mapping",
                    mapping.page_count,
                    start_va,
                    size_bytes,
                    allow_dma_mapping_aliases,
                    iommu_active,
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
    iommu_active: bool,
) void {
    const requested_end, const overflow = @addWithOverflow(requested_start, requested_size);
    const range_end = if (overflow == 0) requested_end else std.math.maxInt(u64);
    var buf: [256]u8 = undefined;
    const line = if (page_count) |pages|
        std.fmt.bufPrint(
            &buf,
            "[trace] c=kernel e=dma_derivation_conflict cls=1 kind={s} page_count={} requested_start=0x{x} requested_end=0x{x} allow_dma_mapping_aliases={} iommu_active={}\n",
            .{ object_kind, pages, requested_start, range_end, @intFromBool(allow_dma_mapping_aliases), @intFromBool(iommu_active) },
        ) catch return
    else
        std.fmt.bufPrint(
            &buf,
            "[trace] c=kernel e=dma_derivation_conflict cls=1 kind={s} requested_start=0x{x} requested_end=0x{x} allow_dma_mapping_aliases={} iommu_active={}\n",
            .{ object_kind, requested_start, range_end, @intFromBool(allow_dma_mapping_aliases), @intFromBool(iommu_active) },
        ) catch return;
    if (builtin.is_test) {
        kernel_log.appendText(line);
    } else {
        kernel_log.write(line);
    }
}

pub fn ownerHasPinnedUserObject(self: anytype, owner: PrincipalId) bool {
    const locked = lockPinIndex(owner);
    defer unlockPinIndex(owner, locked);
    const table = self.getVmaTableConst(owner) orelse return false;
    var result = false;
    for (&table.pinned_index.trees) |*tree| result = result or tree.count != 0;
    if (comptime @import("reference_check.zig").pin_oracle_enabled) if (result != ownerHasPinnedUserObjectOracle(self, owner)) @import("reference_check.zig").failure(error.PinnedMismatch);
    return result;
}
fn ownerHasPinnedUserObjectOracle(self: anytype, owner: PrincipalId) bool {
    var cursor = pinnedObjectCursor(self, owner);
    while (cursor.next()) |edge| {
        const object: KernelObjectRef = .{ .kind = @enumFromInt(edge.identity.container), .index = edge.identity.index, .generation = edge.identity.generation };
        const slot = self.kernelObjectSlot(object) orelse unreachable;
        if (pinnedUserObjectRange(slot, @intFromEnum(owner)) != null) return true;
    }
    return false;
}

pub fn markForkCowVmasCommitted(self: anytype, owner: PrincipalId) void {
    const table = self.getVmaTable(owner) orelse return;
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices.get(@intCast(active_index)).?.*);
        const entry = (table.entries.get(@intCast(entry_index)).?);
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
    const fp = @import("../user_copy.zig").ForkProfile;
    var costs: [4]u64 = @splat(0);
    defer if (comptime fp.enabled) {
        inline for (.{ fp.Counter.vma_validate, fp.Counter.vma_prepare, fp.Counter.vma_retain, fp.Counter.vma_install }, 0..) |field, i| fp.add(field, costs[i]);
    };
    var scratch_count: usize = 0;
    // Both AS transaction locks keep source slots and the exclusion decision
    // stable. Scratch owns only slot numbers, never VMO/COW references. The
    // installed prefix owns those references; rollback must not release its
    // active slots a second time through the uninstalled scratch suffix.
    errdefer {
        var position = scratch_count;
        while (position > dest_table.active_count) {
            position -= 1;
            dest_table.active_indices.release(.{ .index = @intCast(position), .generation = dest_table.active_indices.generation(@intCast(position)).? }) catch unreachable;
        }
        while (dest_table.active_count != 0) {
            const vma_index: usize = @intCast(dest_table.active_indices.get(@intCast(dest_table.active_count - 1)).?.*);
            const entry = dest_table.entries.get(@intCast(vma_index)).?;
            const vmo_ref = entry.vmo;
            self.releaseVmaCowResources(entry, null);
            @TypeOf(self.*).clearVmaEntry(dest_table, vma_index);
            self.releaseNativeVmo(vmo_ref);
        }
    }
    for (0..source_table.active_count) |position| {
        const index = source_table.active_indices.get(@intCast(position)).?.*;
        const entry = source_table.entries.get(index).?;
        if (!entry.active) continue;
        if (self.nativeVmoSlotConst(entry.vmo)) |vmo| if (vmo.kind == .thread_page) continue;
        const start = if (comptime fp.enabled) fp.stamp() else 0;
        try validateRamAdmission(self, entry.vmo, entry.vmo_offset, entry.size_bytes, entry.prot, entry.flags);
        const after_validate = if (comptime fp.enabled) fp.stamp() else 0;
        if (comptime fp.enabled) if (!fp.accumulate(&costs[0], start, after_validate)) fp.add(.overflow, 1);
        // Claim each scratch slot before preparing the next block. Preparing
        // all blocks first makes claimIndex repeatedly scan newer free blocks.
        dest_table.active_indices.prepareIndex(@intCast(scratch_count), dest_table.allocator.?) catch return KernelError.OutOfFreePages;
        _ = dest_table.active_indices.claimIndex(@intCast(scratch_count)) catch unreachable;
        dest_table.active_indices.get(@intCast(scratch_count)).?.* = index;
        scratch_count += 1;
        if (comptime fp.enabled) if (!fp.accumulate(&costs[1], after_validate, fp.stamp())) fp.add(.overflow, 1);
    }
    const Sort = struct {
        table: @TypeOf(dest_table),
        pub fn lessThan(ctx: @This(), a: usize, b: usize) bool {
            return ctx.table.active_indices.get(@intCast(a)).?.* < ctx.table.active_indices.get(@intCast(b)).?.*;
        }
        pub fn swap(ctx: @This(), a: usize, b: usize) void {
            std.mem.swap(u32, ctx.table.active_indices.get(@intCast(a)).?, ctx.table.active_indices.get(@intCast(b)).?);
        }
    };
    // Stable slot identities, not enumeration order, key the address/reference
    // indexes. Sort just the private numbers, not retained objects or ranges.
    // Ascending incremental preparation bounds fresh-child free-list searches
    // to holes in the current block, without scanning sparse source capacity.
    std.sort.heapContext(0, scratch_count, Sort{ .table = dest_table });

    var active_index: usize = 0;
    while (active_index < scratch_count) : (active_index += 1) {
        // Install overwrites only this already-read scratch position. Filtering
        // above means no skip can shift the installed prefix over unread IDs.
        const entry_index = dest_table.active_indices.get(@intCast(active_index)).?.*;
        var source_entry = source_table.entries.get(entry_index).?.*;
        const prepare_start = if (comptime fp.enabled) fp.stamp() else 0;
        dest_table.reference_slots.prepareIndex(entry_index, dest_table.allocator.?) catch return KernelError.OutOfFreePages;
        dest_table.entries.prepareIndex(entry_index, dest_table.allocator.?) catch return KernelError.OutOfFreePages;
        if (comptime fp.enabled) if (!fp.accumulate(&costs[1], prepare_start, fp.stamp())) fp.add(.overflow, 1);
        // Prepare only the child during the fallible retain phase.  The
        // syscall commits the parent COW flags immediately before its PTE
        // write-protect pass, so a failed retain never mutates parent state.
        if (@TypeOf(self.*).forkCowEligible(&source_entry)) {
            source_entry.flags.fork_cow = true;
        }
        const start = if (comptime fp.enabled) fp.stamp() else 0;
        try self.retainNativeVmo(source_entry.vmo);
        if (!source_entry.cow_table.isNull()) {
            self.retainNativeCowTable(source_entry.cow_table) catch |err| {
                self.releaseNativeVmo(source_entry.vmo);
                return err;
            };
        }
        const after_retain = if (comptime fp.enabled) fp.stamp() else 0;
        if (comptime fp.enabled) if (!fp.accumulate(&costs[2], start, after_retain)) fp.add(.overflow, 1);
        self.installVmaEntry(dest_table, entry_index, source_entry);
        if (comptime fp.enabled) if (!fp.accumulate(&costs[3], after_retain, fp.stamp())) fp.add(.overflow, 1);
    }
    std.debug.assert(dest_table.active_count == scratch_count);
    // Fork snapshots a value, not shared placement state: either process may
    // advance independently after the source/destination VM transaction ends.
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
        const entry_index: usize = @intCast(table.active_indices.get(@intCast(active_index)).?.*);
        const entry = (table.entries.get(@intCast(entry_index)).?);
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
    if (entry_index >= dest_table.entries.capacity) return KernelError.InvalidState;
    const dest_entry = dest_table.entries.get(@intCast(entry_index)) orelse return KernelError.InvalidState;
    if (!dest_entry.active or !dest_entry.flags.fork_cow) return KernelError.InvalidState;
    const copied_paddr = (self.allocPhysicalPage(free_list) catch return KernelError.OutOfFreePages).paddr;
    var installed = false;
    defer if (!installed) free_list.appendPage(copied_paddr) catch |err| switch (err) {
        error.PageQuarantined => {},
        else => @panic("invalid PMM return"),
    };
    if (!@TypeOf(self.*).copyPhysicalPage(copied_paddr, src_paddr)) return KernelError.InvalidState;
    self.ensureEntryCowTable(dest_entry, free_list) catch return KernelError.TableFull;
    const cow_page = @TypeOf(self.*).entryCowPageIndex(dest_entry, va) orelse return KernelError.InvalidState;
    try self.setNativeCowPagePaddr(dest_entry.cow_table, cow_page, copied_paddr, free_list);
    installed = true;
}

pub fn getVmaTable(self: anytype, principal: PrincipalId) ?*VmaTable {
    const index = processIndexFromPrincipal(principal) orelse return null;
    const table = self.vmaTableForProcessIndex(index) orelse return null;
    if (table.reference_generation == 0) {
        table.reference_owner = @intCast(index);
        table.reference_generation = (self.processDescriptorSlotConst(index) orelse return null).generation;
    }
    if (table.allocator == null) table.allocator = self.poolAllocator() catch return null;
    return table;
}

pub fn getVmaTableConst(self: anytype, principal: PrincipalId) ?*const VmaTable {
    const index = processIndexFromPrincipal(principal) orelse return null;
    return self.vmaTableForProcessIndexConst(index);
}

pub fn vmaEntryConst(self: anytype, owner: PrincipalId, start_va: u64) ?*const VmaEntry {
    const entry = self.vmaEntryForVaConst(owner, start_va) orelse return null;
    return if (entry.start_va == start_va) entry else null;
}

pub fn vmaEntryForVaConst(self: anytype, owner: PrincipalId, va: u64) ?*const VmaEntry {
    const table = self.getVmaTableConst(owner) orelse return null;
    return table.entryForVa(va);
}

pub fn vmaProtAllowsFault(prot: VmaProt, write_access: bool, instruction_fetch: bool) bool {
    if (instruction_fetch) return prot.exec;
    if (write_access) return prot.write;
    return prot.read;
}

/// Report an unavailable borrowed page, not a protection or anonymous OOM
/// failure. Caller still owns the faulting AS and shared-VM locks, so a
/// concurrent unmap/refresh cannot change the cause between failure and tag.
pub fn nativeVmaFaultBackingUnavailable(self: anytype, owner: PrincipalId, page_va: u64, error_code: u64) bool {
    if (error_code & (1 << 2) == 0 or error_code & ~@as(u64, 0x16) != 0 or
        error_code & 0x12 == 0x12 or !@TypeOf(self.*).isPageAligned(page_va)) return false;
    const entry = self.vmaEntryForVaConst(owner, page_va) orelse return false;
    if (!vmaProtAllowsFault(entry.prot, error_code & 2 != 0, error_code & 16 != 0)) return false;
    const slot = self.nativeVmoSlotConst(entry.vmo) orelse return false;
    const page = (entry.vmo_offset + (page_va - entry.start_va)) / native_page_size;
    if (page >= slot.page_count) return false;
    if (slot.kind == .pager) {
        if (self.entryDirtyPagePaddr(entry, page_va) != null or
            self.nativeVmoResolvedPagePaddr(entry.vmo, @intCast(page)) != null) return false;
        return self.pagerPageFailed(entry.vmo, @intCast(page));
    }
    if (slot.kind != .page_view or !slot.page_view_sparse) return false;
    return self.nativeVmoSparsePageViewHole(entry.vmo, @intCast(page));
}

pub const PagerFaultPage = struct { vmo: NativeVmoRef, index: u32 };

/// Caller owns the faulting AS and shared-VM locks. This predicate runs only
/// after COW and present-page resolution, so a private dirty copy takes
/// precedence over its shared pager source.
pub fn pendingPagerFaultPage(self: anytype, owner: PrincipalId, page_va: u64, write_access: bool, instruction_fetch: bool) ?PagerFaultPage {
    if (!@TypeOf(self.*).isPageAligned(page_va)) return null;
    const entry = self.vmaEntryForVaConst(owner, page_va) orelse return null;
    if (!vmaProtAllowsFault(entry.prot, write_access, instruction_fetch) or
        self.entryDirtyPagePaddr(entry, page_va) != null) return null;
    const vmo = self.nativeVmoSlotConst(entry.vmo) orelse return null;
    if (vmo.kind != .pager or vmo.pager_port.isNull()) return null;
    const page = (entry.vmo_offset + page_va - entry.start_va) / native_page_size;
    if (page >= vmo.page_count or self.nativeVmoResolvedPagePaddr(entry.vmo, @intCast(page)) != null or
        self.pagerPageFailed(entry.vmo, @intCast(page))) return null;
    return .{ .vmo = entry.vmo, .index = @intCast(page) };
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
    return self.nativeVmaFaultMappingForEntry(entry, fault_page_va, write_access, instruction_fetch);
}

/// Caller keeps the VMA stable and holds shared-VM exclusion for its backing.
pub fn nativeVmaFaultMappingForEntry(self: anytype, entry: *const VmaEntry, fault_page_va: u64, write_access: bool, instruction_fetch: bool) ?NativeVmaFaultMapping {
    if (!@TypeOf(self.*).vmaProtAllowsFault(entry.prot, write_access, instruction_fetch)) return null;
    const page_delta = (fault_page_va - entry.start_va) / native_page_size;
    const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
    const dirty_paddr = self.entryDirtyPagePaddr(entry, fault_page_va);
    const paddr = dirty_paddr orelse
        (self.nativeVmoResolvedPagePaddr(entry.vmo, @intCast(vmo_page)) orelse return null);
    return .{
        .paddr = paddr,
        .prot = if (dirty_paddr != null) self.dirtyCowMappingProt(entry) else @TypeOf(self.*).nativeFaultMappingProt(entry),
        .device_pat_slot = self.nativeVmoDevicePatSlot(entry.vmo),
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
    const backing = self.nativeVmoSlotConst(entry.vmo) orelse return null;
    // Peer BAR memory is not ordinary RAM eligible for DMA page pinning, and
    // the kernel clock page must never become a device-visible IOMMU target.
    if (backing.kind == .device_memory or backing.kind == .kernel_page or backing.kind == .thread_page or backing.kind == .pager) return null;
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
        .device_pat_slot = self.nativeVmoDevicePatSlot(entry.vmo),
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
    {
        const entry = table.entryForVa(fault_page_va) orelse return .{};
        const entry_index: usize = entry.references.?.address_node.key.slot;
        if (!@TypeOf(self.*).vmaProtAllowsFault(entry.prot, write_access, instruction_fetch)) return .{};

        const page_delta = (fault_page_va - entry.start_va) / native_page_size;
        const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
        const vmo_page_index: usize = @intCast(vmo_page);
        const paddr = self.entryDirtyPagePaddr(entry, fault_page_va) orelse
            (self.nativeVmoResolvedPagePaddrOrZero(entry.vmo, vmo_page_index) orelse {
                // An absent pager page is handled by the #PF request path
                // after AS/shared-VM locks drop. It is not an access denial;
                // logging each miss as one would flood serial during boot.
                if (self.nativeVmoSlotConst(entry.vmo)) |vmo|
                    if (vmo.kind == .pager) return .{ .kind = .locked_slow_path };
                return .{};
            });
        if (paddr != 0) {
            return .{
                .kind = .ready,
                .mapping = .{
                    .paddr = paddr,
                    .prot = @TypeOf(self.*).nativeFaultMappingProt(entry),
                    .device_pat_slot = self.nativeVmoDevicePatSlot(entry.vmo),
                },
            };
        }
        if ((!entry.flags.anonymous and !self.nativeVmoIsZeroOnDemand(entry.vmo)) or vmo_page > std.math.maxInt(u32)) return .{};
        return .{
            .kind = .allocate_zero,
            .mapping = .{ .paddr = 0, .prot = @TypeOf(self.*).nativeFaultMappingProt(entry) },
            .entry_index = @intCast(entry_index),
            .fault_page_va = fault_page_va,
            .vmo = entry.vmo,
            .vmo_page_index = @intCast(vmo_page),
        };
    }
}

pub fn commitNativeVmaFaultMapping(
    self: anytype,
    owner: PrincipalId,
    plan: NativeVmaFaultPlan,
    candidate_paddr: u64,
    free_list: ?*FreePageList,
) ?NativeVmaFaultMapping {
    return commitNativeVmaFaultMappingResult(self, owner, plan, candidate_paddr, free_list) catch null;
}

// AS + shared-VM exclusion keeps the entry and VMO alive through validation.
// The caller owns candidate until this commit installs it; on error or when a
// competing PFN wins, the caller must return candidate to PMM.
pub fn commitNativeVmaFaultMappingResult(
    self: anytype,
    owner: PrincipalId,
    plan: NativeVmaFaultPlan,
    candidate_paddr: u64,
    free_list: ?*FreePageList,
) error{ Revalidate, OutOfMemory, Denied }!NativeVmaFaultMapping {
    return commitFault(self, owner, plan, candidate_paddr, free_list, null);
}

pub fn commitFreshNativeVmaFaultMapping(self: anytype, owner: PrincipalId, plan: NativeVmaFaultPlan, fl: *FreePageList) error{ Revalidate, OutOfMemory, Denied }!NativeVmaFaultMapping {
    const Context = struct {
        state: @TypeOf(self),
        owner: PrincipalId,
        plan: NativeVmaFaultPlan,
        fl: *FreePageList,
        fn commit(c: @This(), fresh: *@import("vmo.zig").FreshFaultPage) error{ Revalidate, OutOfMemory, Denied }!NativeVmaFaultMapping {
            return commitFault(c.state, c.owner, c.plan, 0, c.fl, fresh);
        }
    };
    return @import("vmo.zig").withFreshFaultPage(self, fl, Context{ .state = self, .owner = owner, .plan = plan, .fl = fl }, Context.commit);
}
fn commitFault(self: anytype, owner: PrincipalId, plan: NativeVmaFaultPlan, candidate_paddr: u64, free_list: ?*FreePageList, fresh: ?*@import("vmo.zig").FreshFaultPage) error{ Revalidate, OutOfMemory, Denied }!NativeVmaFaultMapping {
    if (plan.kind != .allocate_zero or (fresh == null and (candidate_paddr == 0 or (candidate_paddr & 0xFFF) != 0))) return error.Denied;
    const table = self.getVmaTable(owner) orelse return error.Revalidate;
    const entry_index: usize = @intCast(plan.entry_index);
    if (entry_index >= table.entries.capacity) return error.Revalidate;
    const entry = (table.entries.get(@intCast(entry_index)) orelse return error.Revalidate);
    if (!entry.active or plan.fault_page_va < entry.start_va or plan.fault_page_va >= entry.endVa()) return error.Revalidate;
    if (!@TypeOf(self.*).nativeVmoRefsEqual(entry.vmo, plan.vmo)) return error.Revalidate;
    const page_delta = (plan.fault_page_va - entry.start_va) / native_page_size;
    const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
    if (vmo_page != plan.vmo_page_index) return error.Revalidate;

    const paddr = self.entryDirtyPagePaddr(entry, plan.fault_page_va) orelse blk: {
        if (!entry.flags.anonymous and !self.nativeVmoIsZeroOnDemand(entry.vmo)) return error.Denied;
        break :blk (if (fresh) |page| @import("vmo.zig").resolveFreshNativeVmoPage(self, entry.vmo, @intCast(vmo_page), page, free_list.?) else self.resolveOrInstallNativeVmoPage(entry.vmo, @intCast(vmo_page), candidate_paddr, free_list)) catch |err| {
            // Host unit tests intentionally exhaust this table; port I/O is
            // available only in the guest. Keep the field diagnostic there.
            if (!builtin.is_test) {
                const slot = self.nativeVmoSlotConst(entry.vmo);
                const store = types.vmObjectBackingStoreStats();
                kernel_log.writeFmt(
                    "vm: fault install failed principal={} va=0x{x} page={} vmo_pages={} error={s} store_used={} store_capacity={} store_free={}\n",
                    .{ @intFromEnum(owner), plan.fault_page_va, vmo_page, if (slot) |vmo| vmo.page_count else 0, @errorName(err), store.used_entries, store.capacity, store.free_entries },
                );
                // A single failure census separates live backing from stale
                // owners without repeatedly scanning a full table on retries.
                if (@cmpxchgStrong(u8, &backing_failure_diagnosed, 0, 1, .monotonic, .monotonic) == null) {
                    const census = types.vmObjectBackingStoreBreakdown(&self.native_vmos, &self.vmo_directory_leaf);
                    kernel_log.writeFmt(
                        "vm: backing census native={} cow={} invalid_native={} demand_zero={} live_vmos={} live_demand_zero={} anonymous_logical_pages={} anonymous={} views={}\n",
                        .{ census.native_entries, census.cow_entries, census.invalid_native_entries, census.demand_zero_entries, census.live_vmos, census.live_demand_zero_vmos, census.anonymous_logical_pages, census.anonymous_entries, census.page_view_entries },
                    );
                    for (census.largest) |heavy| {
                        if (heavy.entries == 0) continue;
                        kernel_log.writeFmt(
                            "vm: backing owner token=0x{x} entries={} pages={} refs={} kind={s} parent={}:{}\n",
                            .{ heavy.owner, heavy.entries, heavy.pages, heavy.refs, @tagName(heavy.kind), heavy.parent.index, heavy.parent.generation },
                        );
                    }
                }
            }
            // Only reservePath/acquire metadata exhaustion becomes OutOfFreePages.
            // TableFull is invalid store initialization (no allocation), so
            // reporting it as OOM would make callers retry a permanent error.
            return if (err == error.OutOfFreePages) error.OutOfMemory else error.Denied;
        };
    };
    return .{
        .paddr = paddr,
        .prot = @TypeOf(self.*).nativeFaultMappingProt(entry),
    };
}


pub fn copyPhysicalPage(dst_paddr: u64, src_paddr: u64) bool {
    if (builtin.is_test) return true;
    return @import("../user_copy.zig").copyPhysicalPage(dst_paddr, src_paddr);
}

pub fn replaceVmaPageWithAnonymousPrivatePage(
    self: anytype,
    table: *VmaTable,
    entry_index: usize,
    fault_page_va: u64,
    new_paddr: u64,
    free_list: *FreePageList,
) KernelError!NativeVmoRef {
    if (entry_index >= table.entries.capacity) return KernelError.InvalidState;
    var entry = (table.entries.get(@intCast(entry_index)) orelse return KernelError.InvalidState);
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
    const after_index = if (has_after)
        selectPreparedVmaIndex(table, 0, 0, false, if (has_before) &.{before_index} else &.{}) orelse return KernelError.OutOfFreePages
    else
        0;

    const private_vmo = try self.createNativeVmo(.anonymous, native_page_size);
    try self.retainNativeVmo(private_vmo);
    errdefer self.releaseNativeVmoWithFreeList(private_vmo, free_list);
    var page = [_]u64{new_paddr};
    try self.installNativeVmoPages(private_vmo, 0, page[0..], free_list);

    if (has_before) try self.retainNativeVmo(original.vmo);
    errdefer if (has_before) self.releaseNativeVmo(original.vmo);
    if (has_before and !original.cow_table.isNull()) try self.retainNativeCowTable(original.cow_table);
    errdefer if (has_before and !original.cow_table.isNull()) self.releaseNativeCowTable(original.cow_table, free_list);
    if (has_after) try self.retainNativeVmo(original.vmo);
    errdefer if (has_after) self.releaseNativeVmo(original.vmo);
    if (has_after and !original.cow_table.isNull()) try self.retainNativeCowTable(original.cow_table);
    errdefer if (has_after and !original.cow_table.isNull()) self.releaseNativeCowTable(original.cow_table, free_list);

    if (has_before) {
        self.installVmaEntry(table, before_index, original);
        table.entries.get(@intCast(before_index)).?.*.size_bytes = before_size;
        self.refreshVmaReferences(table.entries.get(@intCast(before_index)).?);
    }
    if (has_after) {
        self.installVmaEntry(table, after_index, original);
        table.entries.get(@intCast(after_index)).?.*.start_va = page_end_va;
        table.entries.get(@intCast(after_index)).?.*.size_bytes = after_size;
        table.entries.get(@intCast(after_index)).?.*.vmo_offset = original.vmo_offset + target_offset + native_page_size;
        table.entries.get(@intCast(after_index)).?.cow_page_offset = original.cow_page_offset + @as(u32, @intCast((target_offset + native_page_size) / native_page_size));
        self.refreshVmaReferences(table.entries.get(@intCast(after_index)).?);
    }

    entry = (table.entries.get(@intCast(entry_index)).?);
    self.installVmaEntry(table, entry_index, .{
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
    self.releaseVmaCowResources(&original, free_list);
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
    {
        const entry = table.entryForVa(fault_page_va) orelse return .{};
        const entry_index: usize = entry.references.?.address_node.key.slot;
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
        const src_paddr = self.nativeVmoResolvedPagePaddrOrZero(entry.vmo, @intCast(vmo_page)) orelse {
            // The pager request path runs after VM locks drop. A write to a
            // private pager mapping must first wait for its source page.
            if (self.nativeVmoSlotConst(entry.vmo)) |vmo|
                if (vmo.kind == .pager) return .{ .kind = .locked_slow_path };
            return .{};
        };
        if (src_paddr == 0 and !entry.flags.anonymous and !self.nativeVmoIsZeroOnDemand(entry.vmo)) return .{};
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
    if (entry_index >= table.entries.capacity) return null;
    const entry = (table.entries.get(@intCast(entry_index)) orelse return null);
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
    const current_source = self.nativeVmoResolvedPagePaddrOrZero(entry.vmo, @intCast(vmo_page)) orelse return null;
    if (current_source != plan.source_paddr) return null;

    self.ensureEntryCowTable(entry, free_list) catch |err| {
        if (!builtin.is_test) {
            const store = types.vmObjectBackingStoreStats();
            kernel_log.writeFmt(
                "cow: commit failure stage=ensure_table err={s} vma_start=0x{x} " ++
                    "vma_pages={} store_used={} store_capacity={} store_free={}\n",
                .{
                    @errorName(err),
                    entry.start_va,
                    entry.size_bytes / native_page_size,
                    store.used_entries,
                    store.capacity,
                    store.free_entries,
                },
            );
        }
        return null;
    };
    if (!self.nativeCowTableIsUnique(entry.cow_table)) return null;
    const cow_page = @TypeOf(self.*).entryCowPageIndex(entry, plan.fault_page_va) orelse return null;
    if (cow_page != plan.cow_page_index and !plan.cow_table.isNull()) return null;
    self.setNativeCowPagePaddr(entry.cow_table, cow_page, candidate_paddr, free_list) catch |err| {
        if (!builtin.is_test) {
            kernel_log.writeFmt(
                "cow: commit failure stage=install_page err={s} vma_start=0x{x} cow_page={}\n",
                .{ @errorName(err), entry.start_va, cow_page },
            );
        }
        return null;
    };
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
    if (entry_index >= table.entries.capacity) return false;
    const entry = (table.entries.get(@intCast(entry_index)) orelse return false);
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
    {
        const entry = table.entryForVa(fault_page_va) orelse return null;
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
        const src_paddr = self.nativeVmoResolvedPagePaddrOrZero(entry.vmo, @intCast(vmo_page)) orelse return null;
        if (src_paddr == 0 and !entry.flags.anonymous and !self.nativeVmoIsZeroOnDemand(entry.vmo)) return null;
        var invalidate_start_va: u64 = 0;
        var invalidate_size_bytes: u64 = 0;
        if (!entry.cow_table.isNull() and !self.nativeCowTableIsUnique(entry.cow_table)) {
            self.detachSharedEntryCowTable(entry, free_list) catch return null;
            invalidate_start_va = entry.start_va;
            invalidate_size_bytes = entry.size_bytes;
        }
        const new_paddr = (self.allocPhysicalPage(free_list) catch return null).paddr;
        var installed = false;
        defer if (!installed) free_list.appendPage(new_paddr) catch |err| switch (err) {
            error.PageQuarantined => {},
            else => @panic("invalid PMM return"),
        };

        if (src_paddr != 0) {
            if (!@TypeOf(self.*).copyPhysicalPage(new_paddr, src_paddr)) return null;
        }
        self.ensureEntryCowTable(entry, free_list) catch return null;
        const cow_page = @TypeOf(self.*).entryCowPageIndex(entry, fault_page_va) orelse return null;
        self.setNativeCowPagePaddr(entry.cow_table, cow_page, new_paddr, free_list) catch return null;
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
}

pub fn setVmaProtRange(self: anytype, owner: PrincipalId, start_va: u64, size_bytes: u64, prot: VmaProt) KernelError!void {
    try self.requireActiveProcess(owner);
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes)) return KernelError.InvalidState;
    const end_va = try @TypeOf(self.*).checkedEnd(start_va, size_bytes);
    const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;

    {
        var entry = table.entryForVa(start_va) orelse return KernelError.InvalidState;
        const entry_index: usize = entry.references.?.address_node.key.slot;
        const entry_end = entry.endVa();
        if (end_va > entry_end) return KernelError.InvalidState;
        if (!@TypeOf(self.*).vmaProtAllowedByMax(prot, entry.max_prot)) return KernelError.InvalidState;
        try validateRamAdmission(self, entry.vmo, entry.vmo_offset + (start_va - entry.start_va), size_bytes, prot, entry.flags);

        // An unchanged subrange needs neither VMA slots nor extra backing refs.
        if (@as(u8, @bitCast(entry.prot)) == @as(u8, @bitCast(prot))) return;

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
        for (0..split_count) |i| {
            split_indices[i] = selectPreparedVmaIndex(table, 0, 0, false, split_indices[0..i]) orelse return KernelError.OutOfFreePages;
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
            self.installVmaEntry(table, before_index, original);
            table.entries.get(@intCast(before_index)).?.*.size_bytes = before_size;
            self.refreshVmaReferences(table.entries.get(@intCast(before_index)).?);
        }
        if (has_after) {
            self.installVmaEntry(table, after_index, original);
            table.entries.get(@intCast(after_index)).?.*.start_va = end_va;
            table.entries.get(@intCast(after_index)).?.*.size_bytes = after_size;
            table.entries.get(@intCast(after_index)).?.*.vmo_offset = original.vmo_offset + target_offset + size_bytes;
            table.entries.get(@intCast(after_index)).?.*.cow_page_offset = original.cow_page_offset + @as(u32, @intCast((target_offset + size_bytes) / native_page_size));
            self.refreshVmaReferences(table.entries.get(@intCast(after_index)).?);
        }

        entry = (table.entries.get(@intCast(entry_index)).?);
        entry.start_va = start_va;
        entry.size_bytes = size_bytes;
        entry.prot = prot;
        entry.vmo_offset = original.vmo_offset + target_offset;
        entry.cow_page_offset = original.cow_page_offset + @as(u32, @intCast(target_offset / native_page_size));
        self.refreshVmaReferences(entry);
        return;
    }
}

pub fn releaseFdTableForProcessIndex(self: anytype, index: usize) void {
    const table = self.fdTableForProcessIndex(index) orelse return;
    // Withdraw weak observers before any source FD or object can be recycled,
    // including table-release callers outside the ordinary exit sequence.
    @import("persistent_wait.zig").clearOwner(self, processPrincipalFromIndex(index).?);
    defer self.reclaimFdCapabilities();
    var fd: usize = 0;
    while (fd < table.capacity) : (fd += 1) {
        const object_ref = table.at(fd).*.object;
        if (object_ref.isNull()) continue;
        self.unpublishFdReference(table.at(fd));
        table.write(fd, .{});
        self.releaseKernelObject(object_ref);
    }
    // SlotTable records the original allocator in each grant, so process
    // retirement can return entries even when no current FreePageList is passed.
    self.retireFdStorage(table);
}

pub fn releaseFdTableForProcessIndexWithFreeList(
    self: anytype,
    index: usize,
    free_list: *FreePageList,
) void {
    const table = self.fdTableForProcessIndex(index) orelse return;
    @import("persistent_wait.zig").clearOwner(self, processPrincipalFromIndex(index).?);
    defer self.reclaimFdCapabilities();
    var fd: usize = 0;
    while (fd < table.capacity) : (fd += 1) {
        const object_ref = table.at(fd).*.object;
        if (object_ref.isNull()) continue;
        self.unpublishFdReference(table.at(fd));
        table.write(fd, .{});
        self.releaseKernelObjectWithFreeList(object_ref, free_list);
    }
    self.releaseFdStorage(table, free_list);
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
    const profile_start = smp_perf.timestamp();
    smp_perf.vmoAdd(.cow_calls, 1);
    defer smp_perf.vmoElapsed(.cow_cycles, profile_start);
    const page_count: usize = @intCast(entry.size_bytes / native_page_size);
    self.releaseVmaCowPageRange(entry, 0, page_count, free_list);
    if (entry.references) |pair| if (pair.entry == entry) references.Root.disconnect(&pair.cow, true);
    self.releaseNativeCowTable(entry.cow_table, free_list);
}

pub fn releaseVmaTableForProcessIndex(self: anytype, index: usize) void {
    const table = self.vmaTableForProcessIndex(index) orelse return;
    while (table.active_count != 0) {
        const vma_index: usize = @intCast(table.active_indices.get(@intCast(table.active_count - 1)).?.*);
        const entry = (table.entries.get(@intCast(vma_index)).?);
        const vmo_ref = entry.vmo;
        self.releaseVmaCowResources(entry, null);
        @TypeOf(self.*).clearVmaEntry(table, vma_index);
        self.releaseNativeVmo(vmo_ref);
    }
    // Process teardown has excluded every VMA/body loan. Generations are
    // internal to that AS lifetime; failed grant returns stay in stable Owners.
    table.reference_slots.deinit();
    table.reference_generation = 0;
    table.entries.deinit();
    table.active_indices.deinit();
}

pub fn releaseVmaTableForProcessIndexWithFreeList(
    self: anytype,
    index: usize,
    free_list: *FreePageList,
) void {
    const table = self.vmaTableForProcessIndex(index) orelse return;
    while (table.active_count != 0) {
        const vma_index: usize = @intCast(table.active_indices.get(@intCast(table.active_count - 1)).?.*);
        const entry = (table.entries.get(@intCast(vma_index)).?);
        const vmo_ref = entry.vmo;
        self.releaseVmaCowResources(entry, free_list);
        @TypeOf(self.*).clearVmaEntry(table, vma_index);
        self.releaseNativeVmoWithFreeList(vmo_ref, free_list);
    }
    // Process teardown has excluded every VMA/body loan. Generations are
    // internal to that AS lifetime; failed grant returns stay in stable Owners.
    table.reference_slots.deinit();
    table.reference_generation = 0;
    table.entries.deinit();
    table.active_indices.deinit();
}

pub fn releasePrincipalNativeMemory(
    self: anytype,
    owner: PrincipalId,
    free_list: *FreePageList,
) void {
    const index = processIndexFromPrincipal(owner) orelse return;
    self.pagerOwnerDied(owner);
    // Address-bound FDs may have been duplicated or queued to another
    // process. Revoke every exact reference first so MMIO close-time unmaps
    // and DMA IOVA teardown complete before VMA pages return to PMM.
    self.revokeOwnedPinnedUserObjectsWithFreeList(owner, free_list);
    if (self.getVmaTableConst(owner)) |table| {
        const locked = lockPinIndex(owner);
        defer unlockPinIndex(owner, locked);
        // This live-table cleanup precedes architectural AS retirement.
        // Empty post-retirement table teardown must not enter this borrower;
        // all source pointers must be gone before its first retirement.
        table.pinned_index.requireEmpty(table.pinned_objects.count) catch @panic("pin index survived owner retirement");
    }
    self.releaseVmaTableForProcessIndexWithFreeList(index, free_list);
    self.releaseFdTableForProcessIndexWithFreeList(index, free_list);
}

// Source PT retirement precedes this removal. Do not transfer an identity
// mapping (or its VMO reference) into a different AS incarnation on exec.
pub fn dropThreadIdentityVmasForExec(self: anytype, owner: PrincipalId, free_list: *FreePageList) void {
    const table = self.getVmaTable(owner) orelse return;
    var position: usize = 0;
    while (position < table.active_count) {
        const index = table.active_indices.get(@intCast(position)).?.*;
        const entry = table.entries.get(index).?;
        const vmo = self.nativeVmoSlotConst(entry.vmo);
        if (vmo != null and vmo.?.kind == .thread_page) {
            const ref = entry.vmo;
            self.releaseVmaCowResources(entry, free_list);
            @TypeOf(self.*).clearVmaEntry(table, index);
            self.releaseNativeVmoWithFreeList(ref, free_list);
        } else position += 1;
    }
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
    dest_table.reference_slots.takeFrom(&source_table.reference_slots);
    dest_table.entries.takeFrom(&source_table.entries);
    dest_table.active_indices.takeFrom(&source_table.active_indices);
    dest_table.active_count = source_table.active_count;
    dest_table.address_index = source_table.address_index;
    source_table.address_index = .{};
    for (0..dest_table.active_count) |position| {
        const i = dest_table.active_indices.get(@intCast(position)).?.*;
        const value = dest_table.entries.get(i).?;
        value.references.?.address_owner = dest_table;
        const pair = value.references.?;
        pair.identity.owner = @intCast(dest_index);
        pair.identity.owner_generation = (self.processDescriptorSlotConst(dest_index) orelse unreachable).generation;
        self.refreshVmaReferences(value);
    }
    source_table.active_count = 0;
}

pub fn resetProcessRuntimeTables(self: anytype, index: usize) void {
    self.releaseVmaTableForProcessIndex(index);
    self.releaseFdTableForProcessIndex(index);
}

pub fn findFreeVma(table: *VmaTable) ?usize {
    return selectPreparedVmaIndex(table, 0, 0, false, &.{});
}

/// Host tests only: the slot-selection contract used by MAP_FIXED/mremap.
pub fn selectPreparedVmaIndexForTest(table: *VmaTable, target_start: u64, target_size: u64, may_reuse_target_full: bool, excluded: []const usize) ?usize {
    if (!builtin.is_test) @compileError("test-only VMA slot selection entry");
    return selectPreparedVmaIndex(table, target_start, target_size, may_reuse_target_full, excluded);
}

pub fn installVmaEntry(self: anytype, table: *VmaTable, index: usize, entry: VmaEntry) void {
    const fp = @import("../user_copy.zig").ForkProfile;
    const entry_start = if (comptime fp.enabled) fp.stamp() else 0;
    const value = table.entries.claimIndex(@intCast(index)) catch unreachable;
    if (comptime fp.enabled) {
        const end = fp.stamp();
        if (end < entry_start) fp.add(.overflow, 1) else fp.add(.claim_entry, end - entry_start);
    }
    const was_active = value.active;
    const active_position = value.active_position;
    if (value.references) |old_pair| if (old_pair.address_node.linked) table.address_index.remove(value);
    const reference_start = if (comptime fp.enabled) fp.stamp() else 0;
    const pair = table.reference_slots.claimIndex(@intCast(index)) catch unreachable;
    if (comptime fp.enabled) {
        const end = fp.stamp();
        if (end < reference_start) fp.add(.overflow, 1) else fp.add(.claim_reference, end - reference_start);
    }
    disconnectVmoAlias(pair);
    references.Root.disconnect(&pair.cow, true);
    pair.entry = value;
    pair.identity = .{ .kind = .vma, .owner = table.reference_owner, .owner_generation = table.reference_generation, .index = @intCast(index), .generation = table.entries.generation(@intCast(index)).? };
    value.* = entry;
    value.active_position = active_position;
    // Snapshots (fork/split) carry source links; never publish those links in
    // the destination. SlotTable keeps the newly connected node stable.
    pair.address_node = .{};
    pair.address_owner = table;
    value.device_backing = if (self.nativeVmoSlotConst(entry.vmo)) |vmo| vmo.kind == .device_memory else false;
    value.references = pair;
    self.refreshVmaReferences(value);
    if (!was_active and entry.active) {
        const active_start = if (comptime fp.enabled) fp.stamp() else 0;
        const active = table.active_indices.claimIndex(@intCast(table.active_count)) catch unreachable;
        if (comptime fp.enabled) {
        const end = fp.stamp();
        if (end < active_start) fp.add(.overflow, 1) else fp.add(.claim_active, end - active_start);
    }
        active.* = @intCast(index);
        value.active_position = @intCast(table.active_count);
        table.active_count += 1;
    } else if (was_active and !entry.active and table.active_count != 0) {
        std.debug.assert(removeActiveVmaIndex(table, index));
        table.active_count -= 1;
    }
}

pub fn clearVmaEntry(table: *VmaTable, index: usize) void {
    const entry = table.entries.get(@intCast(index)) orelse return;
    if (entry.references) |pair| if (pair.address_node.linked) table.address_index.remove(entry);
    if (entry.active and table.active_count != 0) {
        std.debug.assert(removeActiveVmaIndex(table, index));
        table.active_count -= 1;
    }
    if (entry.references) |pair| {
        disconnectVmoAlias(pair);
        references.Root.disconnect(&pair.cow, true);
        pair.entry = null;
        table.reference_slots.release(.{ .index = @intCast(index), .generation = table.reference_slots.generation(@intCast(index)).? }) catch unreachable;
    }
    table.entries.release(.{ .index = @intCast(index), .generation = table.entries.generation(@intCast(index)).? }) catch unreachable;
}

pub fn removeActiveVmaIndex(table: *VmaTable, index: usize) bool {
    const entry = table.entries.get(@intCast(index)) orelse return false;
    if (entry.active_position >= table.active_count) return false;
    const current = table.active_indices.get(entry.active_position).?;
    if (current.* != index) return false;
    const last_index: u32 = @intCast(table.active_count - 1);
    const last = table.active_indices.get(last_index).?;
    // Preserve the old swap-with-last order: teardown/fork failure unwind
    // observes it, and page return order affects subsequent physical reuse.
    current.* = last.*;
    table.entries.get(last.*).?.active_position = entry.active_position;
    table.active_indices.release(.{ .index = last_index, .generation = table.active_indices.generation(last_index).? }) catch unreachable;
    return true;
}

pub fn vmaRangeOverlaps(table: *const VmaTable, start_va: u64, size_bytes: u64) KernelError!bool {
    const end_va = try checkedEnd(start_va, size_bytes);
    return table.overlaps(start_va, end_va);
}

// Native placement is kernel policy, not an ABI arena. Keep the historical
// starting point while allowing the cursor to continue across its former end.
// The former seed was discarded; retaining deterministic next-fit adds no new
// ASLR policy and does not reduce the old placement entropy.
pub const user_map_initial_va: u64 = 0x0000_0100_0000_0000;

const MappingInterval = user_vm.MappingInterval;
const UserMapBlockers = struct {
    table: *VmaTable,
    vma: ?*VmaEntry,
    pins: [4]?*references.Node,
    architectural: user_vm.ReservationBlockerIterator,
    architectural_next: ?MappingInterval,
    invalid_pin: bool = false,

    fn init(table: *VmaTable, owner: PrincipalId, start: u64, end: u64) UserMapBlockers {
        const architectural = user_vm.ReservationBlockerIterator.init(owner, start, end);
        var result: UserMapBlockers = .{
            .table = table,
            .vma = table.address_index.first,
            .pins = @splat(null),
            .architectural = architectural,
            .architectural_next = null,
        };
        for (&table.pinned_index.trees, 0..) |*tree, i| {
            result.pins[i] = tree.first;
            if (tree.root) |edge| result.invalid_pin = result.invalid_pin or edge.pin.invalid_count != 0;
        }
        result.architectural_next = result.architectural.next();
        return result;
    }

    fn pinInterval(edge: *references.Node) MappingInterval {
        return .{ .start = edge.range.start, .end = std.math.add(u64, edge.range.start, edge.range.size) catch std.math.maxInt(u64) };
    }

    fn next(self: *UserMapBlockers) ?MappingInterval {
        // PinIndex's overflow rule is fail-closed for every queried address,
        // even one before the malformed object's start. Preserve that rule
        // while sharing the same union predicate with hint admission.
        if (self.invalid_pin) {
            self.invalid_pin = false;
            return .{ .start = 0, .end = std.math.maxInt(u64) };
        }
        var selected: ?MappingInterval = self.architectural_next;
        var source: usize = 5;
        if (self.vma) |entry| {
            if (selected == null or entry.start_va < selected.?.start) {
                selected = .{ .start = entry.start_va, .end = entry.endVa() };
                source = 4;
            }
        }
        for (self.pins, 0..) |pin, i| if (pin) |edge| {
            const interval = pinInterval(edge);
            if (selected == null or interval.start < selected.?.start) {
                selected = interval;
                source = i;
            }
        };
        if (selected == null) return null;
        if (source == 5) {
            self.architectural_next = self.architectural.next();
        } else if (source == 4) {
            self.vma = self.table.address_index.successor(self.vma.?);
        } else {
            self.pins[source] = self.table.pinned_index.trees[source].successor(self.pins[source].?);
        }
        return selected;
    }
};

fn freePteGap(present: *user_vm.PresentUserMappingBlockerIterator, size: u64, start: u64, end: u64) ?u64 {
    if (start >= end or size > end - start) return null;
    present.seekPast(start);
    var candidate = start;
    while (present.nextBefore(candidate + size)) |blocker| {
        if (blocker.end <= blocker.start or blocker.end <= candidate) continue;
        if (blocker.start >= candidate + size) return candidate;
        candidate = blocker.end;
        if (candidate >= end or size > end - candidate) return null;
    }
    return candidate;
}

fn freeUserMapInSpan(table: *VmaTable, owner: PrincipalId, size: u64, start: u64, end: u64) ?u64 {
    if (start >= end or size > end - start) return null;
    var candidate = start;
    var blockers = UserMapBlockers.init(table, owner, start, end);
    var present = user_vm.PresentUserMappingBlockerIterator.init(owner, start, end);
    // The six sorted metadata streams advance once per blocker. Whole VMA,
    // reservation and pin intervals skip their PTEs: a TiB backed VMA costs
    // one metadata step rather than a page walk. Only otherwise-free gaps
    // inspect present PTE runs, including overlays without reservation records.
    while (blockers.next()) |blocker| {
        if (blocker.end <= blocker.start or blocker.end <= candidate) continue;
        if (blocker.start >= candidate + size) {
            if (freePteGap(&present, size, candidate, @min(end, blocker.start))) |free| return free;
        }
        // Address-bound objects may retain byte-sized BAR subranges. Round
        // their end upward so skipping a pin never returns an unaligned VA.
        const aligned_end = (std.math.add(u64, blocker.end, native_page_size - 1) catch return null) & ~(native_page_size - 1);
        candidate = @max(candidate, aligned_end);
        if (candidate >= end or size > end - candidate) return null;
    }
    return freePteGap(&present, size, candidate, end);
}

pub fn findFreeUserMapVa(self: anytype, owner: PrincipalId, size_bytes: u64) KernelError!u64 {
    if (!@TypeOf(self.*).isPageAligned(size_bytes) or size_bytes == 0) return KernelError.InvalidState;
    const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
    const bounds = user_vm.userMappingPlacementBounds() orelse return KernelError.InvalidState;
    if (size_bytes > bounds.end - bounds.start) return KernelError.TableFull;
    // Callers hold K then the VM transaction through publication. Recursing
    // into the owner's AS here follows the pin-index order (K -> AS), keeping
    // VMA, pin, reservation and PTE cursors stable throughout the merged walk.
    const locked = lockPinIndex(owner);
    defer unlockPinIndex(owner, locked);
    const initial = if (user_map_initial_va >= bounds.start and user_map_initial_va < bounds.end) user_map_initial_va else bounds.start;
    const cursor = if (table.next_user_map_va >= bounds.start and table.next_user_map_va < bounds.end) table.next_user_map_va else initial;
    const candidate = freeUserMapInSpan(table, owner, size_bytes, cursor, bounds.end) orelse
        freeUserMapInSpan(table, owner, size_bytes, bounds.start, bounds.end) orelse return KernelError.TableFull;
    // Search failure leaves the cursor unchanged. A successful selection
    // consumes only cursor state, not a reservation: a later install failure
    // may leave a hole, which the same wraparound walk can reuse.
    table.next_user_map_va = candidate + size_bytes;
    return candidate;
}

pub fn userMapRangeIsFree(self: anytype, owner: PrincipalId, start_va: u64, size_bytes: u64) KernelError!bool {
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes) or size_bytes == 0) return KernelError.InvalidState;
    const end = try checkedEnd(start_va, size_bytes);
    const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
    const locked = lockPinIndex(owner);
    defer unlockPinIndex(owner, locked);
    // Hints and NOREPLACE use the same union as unspecified placement. Their
    // legal address bounds remain the syscall's validation responsibility.
    var blockers = UserMapBlockers.init(table, owner, start_va, end);
    while (blockers.next()) |blocker| {
        if (blocker.start >= end) break;
        if (blocker.end > blocker.start and blocker.end > start_va) return false;
    }
    var present = user_vm.PresentUserMappingBlockerIterator.init(owner, start_va, end);
    return present.nextBefore(end) == null;
}

/// Read-only admission check for a BAR overlay. The caller must hold the VM
/// transaction through PTE publication and separately reject existing PTEs.
/// Keep these VMAs in place during the lease: they reserve the address both
/// before publication and after last-close, without an allocator-visible hole.
pub fn userRangeIsUnbackedMmioReservation(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
) bool {
    if (start_va == 0 or !@TypeOf(self.*).isPageAligned(start_va) or
        !@TypeOf(self.*).isPageAligned(size_bytes)) return false;
    const end_va = checkedEnd(start_va, size_bytes) catch return false;
    if (self.rangeOverlapsPinnedUserObject(owner, start_va, size_bytes)) return false;

    var cursor = start_va;
    while (cursor < end_va) {
        const entry = self.vmaEntryForVaConst(owner, cursor) orelse return false;
        if (!entry.flags.anonymous or !entry.flags.private or entry.flags.shared or
            !entry.flags.noreserve or entry.flags.fork_cow or
            !entry.cow_table.isNull() or
            entry.prot.read or entry.prot.write or entry.prot.exec or entry.prot.pkey != 0)
            return false;
        const vmo = self.nativeVmoSlotConst(entry.vmo) orelse return false;
        if (vmo.kind != .anonymous or !vmo.backing.parent.ref.isNull()) return false;
        const part_end = @min(end_va, entry.endVa());
        const offset = std.math.add(u64, entry.vmo_offset, cursor - entry.start_va) catch return false;
        const length = part_end - cursor;
        if (!@TypeOf(self.*).isPageAligned(offset) or
            offset > vmo.size_bytes or length > vmo.size_bytes - offset) return false;
        var page: usize = @intCast(offset / native_page_size);
        const end_page: usize = @intCast((offset + length) / native_page_size);
        while (page < end_page) : (page += 1) {
            const paddr = self.nativeVmoPagePaddrOrHole(entry.vmo, page) orelse return false;
            // mprotect(NONE) does not turn used anonymous RAM into a virgin
            // reservation; replacing it would silently discard its contents.
            if (paddr != 0) return false;
        }
        cursor = part_end;
    }
    return true;
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

fn maxProtForFdMapping(rights: FdRights, flags: MmapFlags) VmaProt {
    var max_prot = vmaMaxProtForRights(rights, flags.pkey);
    // Reading a backing permits modifying one's own copy, not the shared
    // object. Only this mapping mode goes through non-anonymous private COW.
    // Shared/default mappings still require map_write; exec is independent.
    if (rights.map_read and flags.private and !flags.shared and !flags.anonymous)
        max_prot.write = true;
    return max_prot;
}

pub fn vmaProtAllowedByMax(prot: VmaProt, max_prot: VmaProt) bool {
    // Read, write, and execute are independent VM ABI permissions.  Policy
    // must not silently narrow a requested combination once the mapping
    // authority represented by max_prot grants all of its bits.
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

/// A pager VMO owns a sparse page store but allocates no data pages until
/// supply. Keeping this as a separate creation path prevents VMO_CREATE's
/// eager anonymous branch from silently manufacturing zeroed file contents.
pub fn createPagerVmoFd(
    self: anytype,
    owner: PrincipalId,
    size_bytes: u64,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    try self.requireActiveProcess(owner);
    if (size_bytes == 0 or size_bytes & (native_page_size - 1) != 0) return KernelError.InvalidState;
    const vmo_ref = try self.createNativeVmo(.pager, size_bytes);
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

/// Install a new read-only FD for the kernel clock page. Each FD owns one
/// kernel object and one VMO reference; the VMO itself is shared and kept alive
/// by the kernel's permanent reference.
pub fn openKernelClockPageFd(
    self: anytype,
    owner: PrincipalId,
    paddr: u64,
    flags: FdFlags,
    min_fd: Fd,
    free_list: ?*FreePageList,
) KernelError!Fd {
    try self.requireActiveProcess(owner);
    const vmo_ref = try self.kernelClockPageVmo(paddr, free_list);
    try self.retainNativeVmo(vmo_ref);
    const object_ref = self.createKernelObject(.vmo, .{ .vmo = vmo_ref }) catch |err| {
        self.releaseNativeVmo(vmo_ref);
        return err;
    };
    const rights: FdRights = .{ .inspect = true, .close = true, .map_read = true, .dup = true, .transfer = true };
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
    const page_count_u64: u64 = try types.checkedVmoPageCount(size_bytes);
    const aligned_size = page_count_u64 * native_page_size;
    const fd = try self.createAnonymousVmoFd(owner, aligned_size, rights, flags, min_fd);
    errdefer self.closeFdWithFreeList(owner, fd, free_list) catch {};
    const vmo_ref = self.nativeVmoRefForFd(owner, fd) orelse return KernelError.InvalidState;
    var scratch = self.beginInvocationStorage(free_list);
    defer scratch.rollback() catch @panic("invalid invocation storage rollback");
    const pages = scratch.reserve(u64, @as(usize, @intCast(page_count_u64)), .replaceable) catch return KernelError.OutOfFreePages;
    var allocated: usize = 0;
    errdefer {
        // Intrusive PMM needs no return metadata allocation. DMA-held PFNs
        // remain in the common retention ledger until device drain is proved.
        while (allocated > 0) {
            allocated -= 1;
            free_list.appendPage(pages[allocated]) catch |err| switch (err) {
                error.PageQuarantined => {},
                else => @panic("invalid PMM return"),
            };
        }
    }
    while (allocated < page_count_u64) : (allocated += 1) {
        // Ordinary VMO pages are not DMA-constrained. Preserve low RAM for
        // the separate contiguous DMA pool and kernel bootstrap structures.
        pages[allocated] = (try self.allocPhysicalPage(free_list)).paddr;
    }
    try self.installNativeVmoPages(vmo_ref, 0, pages[0..allocated], free_list);
    return fd;
}

pub fn createZeroOnDemandVmoFd(
    self: anytype,
    owner: PrincipalId,
    size_bytes: u64,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    const count = try types.checkedVmoPageCount(size_bytes);
    const fd = try self.createAnonymousVmoFd(owner, @as(u64, count) * native_page_size, rights, flags, min_fd);
    const ref = self.nativeVmoRefForFd(owner, fd).?;
    self.nativeVmoSlot(ref).?.zero_on_demand = true;
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
    const aligned_size = @as(u64, try types.checkedVmoPageCount(size_bytes)) * native_page_size;
    const page_count: usize = @intCast(aligned_size / native_page_size);
    if (page_count == 0 or page_count > dma_pool_max_pages) return KernelError.InvalidState;

    const fd = try self.createAnonymousVmoFd(owner, aligned_size, rights, flags, min_fd);
    errdefer self.closeFdWithFreeList(owner, fd, free_list) catch {};
    const vmo_ref = self.nativeVmoRefForFd(owner, fd) orelse return KernelError.InvalidState;
    const base = try free_list.popContiguousBelow(page_count, @TypeOf(self.*).low_memory_limit);
    // Intrusive PMM needs no return metadata allocation. DMA-held PFNs
    // remain in the common retention ledger until device drain is proved.
    errdefer free_list.appendContiguousRange(base, page_count) catch |err| switch (err) {
        error.PageQuarantined => {},
        else => @panic("invalid PMM return"),
    };

    // One grant, all zeroing, then all-or-nothing backing insertion. Arithmetic
    // enumeration needs no second PFN array or per-batch publication.
    for (0..page_count) |page_index| {
        try self.zeroAllocatedPage(base + @as(u64, @intCast(page_index)) * native_page_size);
    }
    const slot = self.nativeVmoSlot(vmo_ref) orelse unreachable;
    try types.setVmoBackingPageStoreContiguous(slot.page_store_start, slot.page_count, slot.page_store_owner, base, page_count, free_list);
    return fd;
}

/// How full the two per-process/global tables behind an anonymous mapping are.
///
/// `TableFull` on a four-kilobyte mapping is reported to userspace as ENOMEM,
/// which sends the reader looking for exhausted memory instead of an exhausted
/// table.  Naming the table costs one error-path scan.
pub const AnonymousMapCapacity = struct {
    vmas_active: u64,
    vmas_capacity: u64,
    vmos_active: u64,
    vmos_capacity: u64,
    /// Composition of the exhausted table: many single-page anonymous entries
    /// mean an allocator whose separate mappings never rejoin, while a spread
    /// of larger file-backed entries means something else entirely.
    vmas_anonymous: u64,
    vmas_single_page: u64,
    vmas_bytes: u64,
    vmas_lowest_va: u64,
    vmas_highest_end_va: u64,
};

pub fn anonymousMapCapacity(self: anytype, owner: PrincipalId) AnonymousMapCapacity {
    var vmas_active: u64 = 0;
    var vmas_capacity: u64 = 0;
    var vmas_anonymous: u64 = 0;
    var vmas_single_page: u64 = 0;
    var vmas_bytes: u64 = 0;
    var vmas_lowest_va: u64 = std.math.maxInt(u64);
    var vmas_highest_end_va: u64 = 0;
    if (self.getVmaTableConst(owner)) |table| {
        vmas_active = @intCast(table.active_count);
        vmas_capacity = table.entries.capacity;
        var index: usize = 0;
        while (index < table.entries.capacity) : (index += 1) {
            const entry = table.entries.get(@intCast(index)) orelse continue;
            if (!entry.active) continue;
            if (entry.flags.anonymous) vmas_anonymous += 1;
            if (entry.size_bytes == native_page_size) vmas_single_page += 1;
            vmas_bytes += entry.size_bytes;
            if (entry.start_va < vmas_lowest_va) vmas_lowest_va = entry.start_va;
            if (entry.endVa() > vmas_highest_end_va) vmas_highest_end_va = entry.endVa();
        }
    }
    if (vmas_lowest_va == std.math.maxInt(u64)) vmas_lowest_va = 0;
    var vmos_active: u64 = 0;
    var index: usize = 0;
    while (index < self.native_vmos.capacity) : (index += 1) {
        const slot = self.native_vmos.get(@intCast(index)) orelse continue;
        if (slot.kind != .none or slot.ref_count != 0) vmos_active += 1;
    }
    return .{
        .vmas_active = vmas_active,
        .vmas_capacity = vmas_capacity,
        .vmos_active = vmos_active,
        .vmos_capacity = self.native_vmos.capacity,
        .vmas_anonymous = vmas_anonymous,
        .vmas_single_page = vmas_single_page,
        .vmas_bytes = vmas_bytes,
        .vmas_lowest_va = vmas_lowest_va,
        .vmas_highest_end_va = vmas_highest_end_va,
    };
}

fn anonymousVmaCanGrow(
    self: anytype,
    owner: PrincipalId,
    entry: *const VmaEntry,
    start_va: u64,
    prot: VmaProt,
    max_prot: VmaProt,
    flags: MmapFlags,
) bool {
    if (!entry.active or entry.endVa() != start_va or
        !flags.anonymous or !flags.private or flags.shared or
        flags.fixed or flags.fixed_noreplace or flags.fork_cow or
        !entry.flags.anonymous or !entry.flags.private or
        entry.flags.shared or entry.flags.fork_cow or
        !entry.cow_table.isNull() or entry.cow_page_offset != 0 or
        entry.vmo_offset != 0)
    {
        return false;
    }
    if (@as(u8, @bitCast(entry.prot)) != @as(u8, @bitCast(prot)) or
        @as(u8, @bitCast(entry.max_prot)) != @as(u8, @bitCast(max_prot)) or
        @as(u32, @bitCast(entry.flags)) != @as(u32, @bitCast(flags)))
    {
        return false;
    }
    const vmo_size = self.nativeVmoSize(entry.vmo) orelse return false;
    if (entry.size_bytes != vmo_size or self.nativeVmoHasParent(entry.vmo)) {
        return false;
    }
    // A DMA/MMIO pin retains the old VA-to-paddr identity independently of
    // the VMO refcount, so do not resize even a uniquely referenced VMO while
    // such an external object exists.
    return !self.rangeOverlapsPinnedUserObject(
        owner,
        entry.start_va,
        entry.size_bytes,
    );
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
    _ = try types.checkedVmoPageCount(size_bytes);
    if (!@TypeOf(self.*).vmaProtAllowedByMax(prot, max_prot)) return KernelError.InvalidState;

    const vma_table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
    if (try @TypeOf(self.*).vmaRangeOverlaps(vma_table, start_va, size_bytes)) return KernelError.InvalidState;

    // Linux workloads routinely issue thousands of adjacent one-page
    // MAP_PRIVATE|MAP_ANONYMOUS requests.  Preserve their syscall boundaries
    // while using one native VMA when the preceding VMO is provably unaliased.
    // Every rejected optimization falls through without mutation.
    grow: {
        if (start_va == 0) break :grow;
        const candidate = vma_table.entryForVa(start_va - 1) orelse break :grow;
        if (!anonymousVmaCanGrow(
            self,
            owner,
            candidate,
            start_va,
            prot,
            max_prot,
            flags,
        )) break :grow;
        if (!self.growUniqueAnonymousVmo(candidate.vmo, size_bytes)) break :grow;
        candidate.size_bytes += size_bytes;
        self.refreshVmaReferences(candidate);
        return candidate.vmo;
    }

    const vma_index = @TypeOf(self.*).findFreeVma(vma_table) orelse return KernelError.TableFull;

    const vmo_ref = try self.createNativeVmo(.anonymous, size_bytes);
    try self.retainNativeVmo(vmo_ref);
    errdefer self.releaseNativeVmoWithFreeList(vmo_ref, free_list);

    self.installVmaEntry(vma_table, vma_index, .{
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

// VMA metadata must not promise a future private/executable cached-RAM alias
// just because its PTE is absent. Caller holds the writer-excluding VM lock.
pub fn validateRamAdmission(self: anytype, ref: NativeVmoRef, offset: u64, bytes: u64, prot: VmaProt, flags: MmapFlags) KernelError!void {
    const copy = @import("../user_copy.zig");
    if (!copy.hasRamCachePolicyLocked()) return;
    const slot = self.nativeVmoSlotConst(ref) orelse return KernelError.InvalidState;
    // BAR authority and cache proof remain in device-memory admission; the
    // kernel clock page is never subject to RAM cache policy.
    if (slot.kind == .device_memory or slot.kind == .kernel_page or slot.kind == .thread_page) return;
    const shadow = slot.kind == .anonymous and !slot.backing.parent.ref.isNull();
    const end = try @TypeOf(self.*).checkedEnd(offset, bytes);
    var position = offset;
    while (position < end) : (position += native_page_size) {
        const page: usize = @intCast(position / native_page_size);
        const pa: u64 = self.nativeVmoResolvedPagePaddrOrZero(ref, page) orelse blk: {
            // An explicit sparse borrowed hole cannot be resolved through its
            // parent or allocated as zero-fill. It carries no cache alias.
            if (self.nativeVmoSparsePageViewHole(ref, page)) break :blk 0;
            // Pager absence is also alias-free, but remains a file-data fault
            // after admission. A resident pager PFN still needs cache checks.
            if (self.nativeVmoUnsuppliedPagerPage(ref, page)) break :blk 0;
            return KernelError.InvalidState;
        };
        if (pa == 0) continue;
        switch (copy.ramCachePolicyForLockedPage(pa)) {
            .ordinary => {},
            .blocked => return KernelError.InvalidState,
            .cached => if (prot.exec or !flags.shared or flags.private or flags.fork_cow or shadow)
                return KernelError.InvalidState,
        }
    }
}

pub fn pageViewProtAllowed(prot: VmaProt, ceiling: VmaProt) bool {
    // A backing capability grants R/W/X, not a protection-key assignment.
    // The mapping's own pkey remains subject to normal per-VMA checks.
    return (!prot.read or ceiling.read) and (!prot.write or ceiling.write) and
        (!prot.exec or ceiling.exec);
}

test "MMIO overlay RAM VMA admission checks only resolved target PFNs including pending-only policy" {
    const copy = @import("../user_copy.zig");
    const policy = @import("../memory/ram_cache_policy.zig");
    const Fixture = struct {
        shadow: bool = false,
        const Slot = struct {
            kind: enum { anonymous, device_memory, kernel_page, page_view, thread_page } = .anonymous,
            page_view_sparse: bool = false,
            backing: struct { parent: struct { ref: NativeVmoRef } },
        };
        pub fn nativeVmoSlotConst(self: @This(), _: NativeVmoRef) ?Slot {
            return .{ .backing = .{ .parent = .{ .ref = if (self.shadow) .{ .index = 1, .generation = 1 } else .{} } } };
        }
        pub fn nativeVmoResolvedPagePaddrOrZero(_: @This(), _: NativeVmoRef, page: usize) ?u64 {
            return switch (page) {
                0 => 0x1000,
                1 => 0x2000,
                2 => 0,
                else => null,
            };
        }
        pub fn nativeVmoPagePaddrOrHole(_: @This(), _: NativeVmoRef, _: usize) ?u64 {
            return null;
        }
        pub fn nativeVmoSparsePageViewHole(_: @This(), _: NativeVmoRef, _: usize) bool {
            return false;
        }
        pub fn nativeVmoUnsuppliedPagerPage(_: @This(), _: NativeVmoRef, _: usize) bool {
            return false;
        }
        pub const checkedEnd = @import("vma.zig").checkedEnd;
    };
    var f = Fixture{};
    const ref: NativeVmoRef = .{ .index = 0, .generation = 1 };
    const spans = [_]policy.Span{.{ .base = 0x2000, .bytes = 4096, .owner = 1, .mode = .wc, .pat_slot = 4 }};
    const active: policy.Snapshot = .{ .policy = &spans };
    const pending: policy.Snapshot = .{ .pending = &spans };
    defer copy.publishRamCacheSnapshot(null);
    copy.publishRamCacheSnapshot(&pending);
    try std.testing.expectError(KernelError.InvalidState, validateRamAdmission(&f, ref, 4096, 4096, .{ .read = true }, .{ .shared = true }));
    copy.publishRamCacheSnapshot(&active);
    try validateRamAdmission(&f, ref, 0, 4096, .{ .exec = true }, .{ .private = true });
    try validateRamAdmission(&f, ref, 0, 3 * 4096, .{ .read = true }, .{ .shared = true });
    try std.testing.expectError(KernelError.InvalidState, validateRamAdmission(&f, ref, 0, 8192, .{ .exec = true }, .{ .shared = true }));
    try std.testing.expectError(KernelError.InvalidState, validateRamAdmission(&f, ref, 4096, 4096, .{ .read = true }, .{ .private = true }));
    f.shadow = true;
    try std.testing.expectError(KernelError.InvalidState, validateRamAdmission(&f, ref, 4096, 4096, .{ .read = true }, .{ .shared = true }));
}

/// The kernel clock page is published by the kernel and KVM. A private
/// mapping would copy it on a write fault and a writable/executable one would
/// let user code alter or run kernel-image memory, so only a shared read-only
/// view is accepted, independent of the FD rights that produced it.
pub fn kernelPageMappingAllowed(prot: VmaProt, max_prot: VmaProt, flags: MmapFlags) bool {
    return flags.shared and !flags.private and !flags.anonymous and !flags.fork_cow and
        !prot.write and !prot.exec and !max_prot.write and !max_prot.exec;
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
    // The pager may own the source FD but cannot map its own destination:
    // otherwise a page-in worker can fault on the work it alone must finish.
    if (vmo.kind == .pager and (vmo.pager_port.isNull() or !flags.private or flags.shared or flags.anonymous or
        @intFromEnum(owner) == vmo.pager_owner or self.pagerProcessControlsPort(owner, vmo.pager_port))) return KernelError.InvalidState;
    if (vmo.kind == .device_memory and (vmo.backing.device.state != .active or
        !flags.shared or flags.private or flags.anonymous or prot.exec or max_prot.exec))
        return KernelError.InvalidState;
    if (!@TypeOf(self.*).vmaProtAllowedByMax(prot, max_prot)) return KernelError.InvalidState;
    if (vmo.kind == .page_view and
        (!flags.shared or flags.private or flags.anonymous or
            !pageViewProtAllowed(max_prot, vmo.page_view_max_prot)))
        return KernelError.InvalidState;
    if (vmo.kind == .thread_page and !@import("../thread_identity.zig").mappingAllowed(vmo, owner, prot, max_prot, flags))
        return KernelError.InvalidState;
    if (vmo.kind == .kernel_page and !kernelPageMappingAllowed(prot, max_prot, flags))
        return KernelError.InvalidState;

    try validateRamAdmission(self, vmo_ref, vmo_offset, size_bytes, prot, flags);
    const vma_table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
    if (try @TypeOf(self.*).vmaRangeOverlaps(vma_table, start_va, size_bytes)) return KernelError.InvalidState;
    const vma_index = @TypeOf(self.*).findFreeVma(vma_table) orelse return KernelError.TableFull;
    try self.retainNativeVmo(vmo_ref);
    self.installVmaEntry(vma_table, vma_index, .{
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
    const max_prot = maxProtForFdMapping(fd_entry.rights, flags);
    if (!@TypeOf(self.*).vmaProtAllowedByMax(prot, max_prot)) return KernelError.InvalidState;
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
    const max_prot = maxProtForFdMapping(fd_entry.rights, flags);
    if (!@TypeOf(self.*).vmaProtAllowedByMax(prot, max_prot)) return KernelError.InvalidState;
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
    // A middle cut strictly fits inside the entry containing start. Disjoint
    // published VMAs then imply exactly one overlap; two middles are impossible.
    if (table.entryForVa(start_va)) |entry| {
        if (start_va > entry.start_va and end_va < entry.endVa()) {
            result.middle_index = entry.references.?.address_node.key.slot;
            result.middle_entry = entry.*;
            if (builtin.mode == .Debug) {
                var overlaps = @constCast(table).overlapping(start_va, end_va);
                std.debug.assert(overlaps.next() == entry and overlaps.next() == null);
            }
        }
    }
    return result;
}

fn entryFullyCoveredByRange(entry: *const VmaEntry, start_va: u64, size_bytes: u64) bool {
    const end_va = start_va + size_bytes;
    return entry.active and start_va <= entry.start_va and end_va >= entry.endVa();
}

fn selectPreparedVmaIndex(
    table: *VmaTable,
    target_start: u64,
    target_size: u64,
    may_reuse_target_full: bool,
    excluded: []const usize,
) ?usize {
    const allocator = table.allocator orelse return null;
    // Reserve publication metadata before retaining VMO/COW references. A
    // failed operation may retain reusable capacity, never a partial VMA.
    const active_end = std.math.add(usize, table.active_count, excluded.len) catch return null;
    if (active_end > std.math.maxInt(u32)) return null;
    table.active_indices.prepareIndex(@intCast(active_end), allocator) catch return null;
    // Slot numbers carry no address/order meaning; callers need only a
    // publishable index distinct from `excluded`. The unused-index list
    // makes this O(excluded + 1) instead of a scan from slot 0, which cost
    // O(VMA count) on every mmap/split/MAP_FIXED (Firefox: thousands).
    // Prepared-but-unclaimed indices stay on that list; hence `excluded`.
    var free = table.entries.freeIndices();
    while (free.next()) |index| {
        if (std.mem.indexOfScalar(usize, excluded, index) != null) continue;
        return prepareVmaIndex(table, index, allocator);
    }
    // No unused index: a VMA the target range fully replaces is cleared by
    // the commit before its index is claimed again (MAP_FIXED/mremap only).
    if (may_reuse_target_full and target_size != 0) {
        const target_end = std.math.add(u64, target_start, target_size) catch return null;
        var overlaps = table.overlapping(target_start, target_end);
        while (overlaps.next()) |entry| {
            if (!entryFullyCoveredByRange(entry, target_start, target_size)) continue;
            const index: usize = @TypeOf(table.entries).indexOf(entry);
            if (std.mem.indexOfScalar(usize, excluded, index) != null) continue;
            return index;
        }
    }
    // Append: the first index past capacity, grown by prepareIndex.
    if (table.entries.capacity > std.math.maxInt(u32)) return null;
    return prepareVmaIndex(table, @intCast(table.entries.capacity), allocator);
}

fn prepareVmaIndex(table: *VmaTable, index: u32, allocator: @import("../memory/owned_storage.zig").Allocator) ?usize {
    // Both tables share the slot number; installVmaEntry claims both.
    table.reference_slots.prepareIndex(index, allocator) catch return null;
    table.entries.prepareIndex(index, allocator) catch return null;
    return index;
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
    const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.prepare, stage_sample);
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
    const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.commit, stage_sample);
    std.debug.assert(prepared.active);
    // Prepared indices borrow unused bodies until the complete commit ends.
    // Reclaim afterwards under AS/shared-VM exclusion, never midway through
    // MAP_FIXED withdrawal where the replacement still needs its reservation.
    defer if (self.getVmaTable(prepared.owner)) |finished_table| {
        finished_table.reference_slots.reclaimEmptyBodies();
        finished_table.entries.reclaimEmptyBodies();
        finished_table.active_indices.reclaimEmptyBodies();
    };
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
    const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.prepare, stage_sample);
    if (!flags.anonymous or flags.shared and flags.private or
        !@TypeOf(self.*).vmaProtAllowedByMax(prot, max_prot))
    {
        return KernelError.InvalidState;
    }
    _ = try types.checkedVmoPageCount(size_bytes);
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
    return prepareFixedFdMmapIntoProcess(self, owner, fd, owner, start_va, size_bytes, prot, flags, vmo_offset, free_list);
}

/// The caller holds both owners' VM transaction locks. The source capability
/// stays in source_owner; only its authorized mapping enters target_owner.
pub fn prepareFixedFdMmapIntoProcess(
    self: anytype,
    source_owner: PrincipalId,
    fd: Fd,
    target_owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    prot: VmaProt,
    flags: MmapFlags,
    vmo_offset: u64,
    free_list: *FreePageList,
) KernelError!FixedMmapPrepared {
    if (flags.anonymous or (flags.private and flags.shared)) return KernelError.InvalidState;
    if (!@TypeOf(self.*).isPageAligned(vmo_offset)) return KernelError.InvalidState;
    const fd_entry = self.fdEntryConst(source_owner, fd) orelse return KernelError.InvalidState;
    const max_prot = maxProtForFdMapping(fd_entry.rights, flags);
    if (!@TypeOf(self.*).vmaProtAllowedByMax(prot, max_prot)) return KernelError.InvalidState;
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
    if (vmo.kind == .device_memory and (vmo.backing.device.state != .active or
        !flags.shared or flags.private or flags.anonymous or prot.exec or max_prot.exec))
        return KernelError.InvalidState;
    if (vmo.kind == .page_view and
        (!flags.shared or flags.private or flags.anonymous or
            !pageViewProtAllowed(max_prot, vmo.page_view_max_prot)))
        return KernelError.InvalidState;
    if (vmo.kind == .thread_page and !@import("../thread_identity.zig").mappingAllowed(vmo, target_owner, prot, max_prot, flags))
        return KernelError.InvalidState;
    if (vmo.kind == .kernel_page and !kernelPageMappingAllowed(prot, max_prot, flags))
        return KernelError.InvalidState;
    // Ordinary shared VMOs still promise full backing. Sparse borrowed views
    // explicitly preserve unavailable holes, not anonymous allocation policy.
    try validateRamAdmission(self, vmo_ref, vmo_offset, size_bytes, prot, flags);
    if (!flags.private and !self.nativeVmoIsZeroOnDemand(vmo_ref) and
        !(vmo.kind == .page_view and vmo.page_view_sparse))
    {
        if (!@TypeOf(self.*).isPageAligned(size_bytes)) return KernelError.InvalidState;
        var offset = vmo_offset;
        while (offset < vmo_end) : (offset += native_page_size) {
            _ = self.nativeVmoResolvedPagePaddr(vmo_ref, @intCast(offset / native_page_size)) orelse
                return KernelError.InvalidState;
        }
    }
    var prepared = try prepareFixedMmapSlots(self, target_owner, start_va, size_bytes, free_list);
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
    const stage_sample = if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageBegin() else {};
    defer if (comptime @import("../lock_profile.zig").enabled) @import("../lock_profile.zig").stageEnd(.commit, stage_sample);
    std.debug.assert(prepared.active);
    // Prepared indices borrow unused bodies until the complete commit ends.
    // Reclaim afterwards under AS/shared-VM exclusion, never midway through
    // MAP_FIXED withdrawal where the replacement still needs its reservation.
    defer if (self.getVmaTable(prepared.owner)) |finished_table| {
        finished_table.reference_slots.reclaimEmptyBodies();
        finished_table.entries.reclaimEmptyBodies();
        finished_table.active_indices.reclaimEmptyBodies();
    };
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
    std.debug.assert(prepared.destination_index < table.entries.capacity);
    std.debug.assert(table.entries.get(@intCast(prepared.destination_index)) == null);
    self.installVmaEntry(table, prepared.destination_index, prepared.destination);
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

    std.debug.assert(table.entries.firstMarkedAtOrAfter(0) == null);
    // Freeze only overlaps before mutation invalidates tree cursors. COW page
    // reclaim depends on reference uniqueness, so visits must retain the old
    // slot order. claimIndex clears marks: a new suffix is never revisited.
    var overlaps = table.overlapping(start_va, end_va);
    while (overlaps.next()) |entry| table.entries.mark(entry.references.?.address_node.key.slot, 1, true);
    if (comptime @import("reference_check.zig").enabled) {
        @import("reference_check.zig").vmaOverlapMarks(table, start_va, end_va) catch |err| @import("reference_check.zig").failure(err);
    }
    // Non-prepared middle cuts can fail reserving a slot or retaining backing.
    // Their sole overlap has not changed; clear any remaining scratch on error.
    errdefer while (table.entries.firstMarkedAtOrAfter(0)) |index| table.entries.mark(index, 1, false);
    var minimum_slot: u64 = 0;
    while (minimum_slot <= std.math.maxInt(u32)) {
        const index = table.entries.firstMarkedAtOrAfter(@intCast(minimum_slot)) orelse break;
        table.entries.mark(index, 1, false);
        minimum_slot = @as(u64, index) + 1;
        var entry = (table.entries.get(@intCast(index)).?);
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
            // FILE_VMO snapshots are anonymous-backed but their private file
            // VMAs are not anonymous. Loader MAP_FIXED cuts otherwise pin a
            // whole image behind the remaining header. The page-return helper
            // still proves that no FD, borrower or other VMA needs this range.
            const can_release_unmapped_pages = entry.flags.anonymous or entry.flags.private or self.nativeVmoIsShadow(vmo_ref);
            const vmo_ref_count = if (can_release_unmapped_pages) self.nativeVmoRefCount(vmo_ref) orelse 0 else 0;
            self.releaseVmaCowResources(entry, free_list);
            @TypeOf(self.*).clearVmaEntry(table, index);
            // The removed edge's retain is now temporary. Consume it before
            // asking whether OTHER owners protect the cut. If it was last,
            // final release already returned backing after the caller's ACK.
            // Otherwise the outer VM transaction preserves surviving owners.
            self.releaseNativeVmoWithFreeList(vmo_ref, free_list);
            if (can_release_unmapped_pages and vmo_ref_count != 1) {
                self.releaseUnmappedAnonymousVmoPageRange(owner, vmo_ref, cut_vmo_first_page, cut_page_count, free_list);
            }
            continue;
        }

        if (cut_start <= entry_start) {
            const trim = cut_end - entry_start;
            self.releaseVmaCowPageRange(entry, 0, @intCast(trim / native_page_size), free_list);
            entry.start_va = cut_end;
            entry.size_bytes = entry_end - cut_end;
            entry.vmo_offset += trim;
            entry.cow_page_offset += @intCast(trim / native_page_size);
            self.refreshVmaReferences(entry);
            if (entry.flags.anonymous or entry.flags.private or self.nativeVmoIsShadow(entry.vmo)) {
                self.releaseUnmappedAnonymousVmoPageRange(owner, entry.vmo, cut_vmo_first_page, cut_page_count, free_list);
            }
            continue;
        }

        if (cut_end >= entry_end) {
            const vmo_ref = entry.vmo;
            const can_release_unmapped_pages = entry.flags.anonymous or entry.flags.private or self.nativeVmoIsShadow(vmo_ref);
            const cut_first_delta: usize = @intCast((cut_start - entry_start) / native_page_size);
            entry.size_bytes = cut_start - entry_start;
            self.refreshVmaReferences(entry);
            self.releaseVmaCowPageRange(entry, cut_first_delta, cut_page_count, free_list);
            if (can_release_unmapped_pages) {
                self.releaseUnmappedAnonymousVmoPageRange(owner, vmo_ref, cut_vmo_first_page, cut_page_count, free_list);
            }
            continue;
        }

        const original = entry.*;
        if (prepared_middle) |prepared| {
            if (!prepared.active or prepared.source_index != index) return KernelError.InvalidState;
            if (prepared.suffix_index >= table.entries.capacity or table.entries.get(@intCast(prepared.suffix_index)) != null) {
                return KernelError.InvalidState;
            }
            self.installVmaEntry(table, prepared.suffix_index, prepared.suffix);
            prepared.active = false;
        } else {
            const after_index = @TypeOf(self.*).findFreeVma(table) orelse return KernelError.TableFull;
            try self.retainNativeVmo(original.vmo);
            errdefer self.releaseNativeVmo(original.vmo);
            if (!original.cow_table.isNull()) try self.retainNativeCowTable(original.cow_table);
            errdefer if (!original.cow_table.isNull()) self.releaseNativeCowTable(original.cow_table, null);
            self.installVmaEntry(table, after_index, original);
            table.entries.get(@intCast(after_index)).?.*.start_va = cut_end;
            table.entries.get(@intCast(after_index)).?.*.size_bytes = entry_end - cut_end;
            table.entries.get(@intCast(after_index)).?.*.vmo_offset = original.vmo_offset + (cut_end - entry_start);
            table.entries.get(@intCast(after_index)).?.*.cow_page_offset = original.cow_page_offset + @as(u32, @intCast((cut_end - entry_start) / native_page_size));
            self.refreshVmaReferences(table.entries.get(@intCast(after_index)).?);
        }

        entry = (table.entries.get(@intCast(index)).?);
        entry.size_bytes = cut_start - entry_start;
        self.refreshVmaReferences(entry);
        self.releaseVmaCowPageRange(entry, @intCast((cut_start - entry_start) / native_page_size), cut_page_count, free_list);
        if (original.flags.anonymous or original.flags.private or self.nativeVmoIsShadow(original.vmo)) {
            self.releaseUnmappedAnonymousVmoPageRange(owner, original.vmo, cut_vmo_first_page, cut_page_count, free_list);
        }
    }
    std.debug.assert(table.entries.firstMarkedAtOrAfter(0) == null);
}

pub fn munmapRangeWithFreeList(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    free_list: *FreePageList,
) KernelError!void {
    defer if (self.getVmaTable(owner)) |table| {
        table.reference_slots.reclaimEmptyBodies();
        table.entries.reclaimEmptyBodies();
        table.active_indices.reclaimEmptyBodies();
    };
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

pub fn vmaRemapSourceAllowed(self: anytype, source: *const VmaEntry, old_size: u64, new_size: u64) bool {
    if (source.flags.anonymous) return true;
    // Relocation/shrink can retain a borrowed VMO's exact identity and offset.
    // Growth cannot materialize it as anonymous RAM without breaking sharing
    // and its backing owner's refresh/lifetime contract.
    if (new_size > old_size or !source.flags.shared or source.flags.private or
        source.flags.fork_cow or !source.cow_table.isNull()) return false;
    const slot = self.nativeVmoSlotConst(source.vmo) orelse return false;
    return slot.kind == .page_view;
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
    if (!self.vmaRemapSourceAllowed(source, old_size, new_size)) return KernelError.InvalidState;
    const source_snapshot = source.*;
    const source_delta = old_start - source_snapshot.start_va;
    const source_vmo_offset = source_snapshot.vmo_offset + source_delta;
    try validateRamAdmission(self, source_snapshot.vmo, source_vmo_offset, @min(old_size, new_size), source_snapshot.prot, source_snapshot.flags);
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
            if (!@TypeOf(self.*).copyPhysicalPage(dst_paddr, src_paddr)) {
                free_list.appendPage(dst_paddr) catch |return_err| switch (return_err) {
                    error.PageQuarantined => {},
                    else => @panic("invalid PMM return"),
                };
                return KernelError.InvalidState;
            }
            var page = [_]u64{dst_paddr};
            self.installNativeVmoPages(dst_vmo, page_index, page[0..], free_list) catch |err| {
                free_list.appendPage(dst_paddr) catch |return_err| switch (return_err) {
                    error.PageQuarantined => {},
                    else => @panic("invalid PMM return"),
                };
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
    // Prepared indices borrow unused bodies until the complete commit ends.
    // Reclaim afterwards under AS/shared-VM exclusion, never midway through
    // MAP_FIXED withdrawal where the replacement still needs its reservation.
    defer if (self.getVmaTable(prepared.owner)) |finished_table| {
        finished_table.reference_slots.reclaimEmptyBodies();
        finished_table.entries.reclaimEmptyBodies();
        finished_table.active_indices.reclaimEmptyBodies();
    };
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
    std.debug.assert(prepared.destination_index < table.entries.capacity);
    std.debug.assert(table.entries.get(@intCast(prepared.destination_index)) == null);
    self.installVmaEntry(table, prepared.destination_index, prepared.destination);
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
        const destination = (table.entries.get(@intCast(prepared.destination_index)).?);
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
        try self.findFreeUserMapVa(owner, new_size)
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

/// Entry storage and its node reservation are stable under the source VM
/// transaction. Snapshot copies cannot update the original entry's edges.
pub fn refreshVmaReferences(self: anytype, entry: *VmaEntry) void {
    const pair = entry.references orelse return;
    if (pair.entry != entry) return;
    // Every live range mutation already publishes its reference range here.
    // Rekey under that same VM transaction before any subsequent lookup.
    if (entry.active) pair.address_owner.?.reindex(entry, pair.identity.index);
    if (entry.vmo.isNull()) return;
    const range: references.Range = .{ .start = entry.start_va, .size = entry.size_bytes, .offset = entry.vmo_offset };
    if (pair.vmo.target != null) references.Root.update(&pair.vmo, pair.identity, range) else {
        const vmo = self.nativeVmoSlot(entry.vmo) orelse unreachable;
        connectVmoAlias(vmo, pair, range);
    }
    if (!entry.cow_table.isNull()) {
        const cow = self.nativeCowTableSlot(entry.cow_table) orelse unreachable;
        const cow_range: references.Range = .{ .start = entry.start_va, .size = entry.size_bytes, .offset = @as(u64, entry.cow_page_offset) * native_page_size };
        if (pair.cow.target != null and pair.cow.target != &cow.references) references.Root.disconnect(&pair.cow, true);
        if (pair.cow.target != null) references.Root.update(&pair.cow, pair.identity, cow_range) else cow.references.connect(&pair.cow, pair.identity, cow_range, true);
    }
}

// The alias list borrows the reference body: K excludes publication/removal,
// and the existing strong edge keeps its target alive for the whole borrow.
fn connectVmoAlias(vmo: *types.NativeVmoSlot, pair: *types.VmaReferences, range: references.Range) void {
    @import("../sync/lock_diagnostics.zig").assertKernelStateHeld();
    std.debug.assert(pair.vmo.target == null and pair.vmo_alias_prev == null and pair.vmo_alias_next == null);
    pair.vmo_alias_next = vmo.vma_alias_head;
    if (vmo.vma_alias_head) |head| head.vmo_alias_prev = pair;
    vmo.vma_alias_head = pair;
    vmo.references.connect(&pair.vmo, pair.identity, range, true);
}
fn disconnectVmoAlias(pair: *types.VmaReferences) void {
    @import("../sync/lock_diagnostics.zig").assertKernelStateHeld();
    const target = pair.vmo.target orelse return;
    const vmo: *types.NativeVmoSlot = @fieldParentPtr("references", target);
    if (pair.vmo_alias_prev) |prev| prev.vmo_alias_next = pair.vmo_alias_next else vmo.vma_alias_head = pair.vmo_alias_next;
    if (pair.vmo_alias_next) |next| next.vmo_alias_prev = pair.vmo_alias_prev;
    pair.vmo_alias_prev = null;
    pair.vmo_alias_next = null;
    references.Root.disconnect(&pair.vmo, true);
}
