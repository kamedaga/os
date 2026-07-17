const std = @import("std");
const builtin = @import("builtin");
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

pub fn checkedEnd(start: u64, size: u64) KernelError!u64 {
    if (size == 0) return KernelError.InvalidState;
    if (start > std.math.maxInt(u64) - size) return KernelError.InvalidState;
    return start + size;
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
        if (source_entry.flags.private and !source_entry.flags.shared and source_entry.prot.write) {
            source_entry.flags.fork_cow = true;
            source_table.entries[entry_index].flags.fork_cow = true;
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

pub fn ensureNativeVmaFaultMapping(
    self: anytype,
    owner: PrincipalId,
    fault_page_va: u64,
    write_access: bool,
    instruction_fetch: bool,
    free_list: *FreePageList,
) ?NativeVmaFaultMapping {
    if (!@TypeOf(self.*).isPageAligned(fault_page_va)) return null;
    const table = self.getVmaTable(owner) orelse return null;
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = &table.entries[entry_index];
        if (fault_page_va < entry.start_va or fault_page_va >= entry.endVa()) continue;
        if (!@TypeOf(self.*).vmaProtAllowsFault(entry.prot, write_access, instruction_fetch)) return null;

        const page_delta = (fault_page_va - entry.start_va) / native_page_size;
        const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
        const vmo_page_index: usize = @intCast(vmo_page);
        var paddr = self.entryDirtyPagePaddr(entry, fault_page_va) orelse
            (self.nativeVmoResolvedPagePaddr(entry.vmo, vmo_page_index) orelse 0);
        if (paddr == 0) {
            if (!entry.flags.anonymous) return null;
            paddr = (self.allocPhysicalPage(free_list) catch return null).paddr;
            var page = [_]u64{paddr};
            self.installNativeVmoPages(entry.vmo, vmo_page_index, page[0..]) catch {
                free_list.appendPage(0, paddr) catch {};
                return null;
            };
        }
        return .{
            .paddr = paddr,
            .prot = @TypeOf(self.*).nativeFaultMappingProt(entry),
        };
    }
    return null;
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

pub fn ensureNativeVmaCowMapping(
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
            return .{
                .paddr = owned_paddr,
                .prot = .{
                    .read = entry.prot.read,
                    .write = entry.prot.write,
                    .exec = entry.prot.exec,
                    .pkey = entry.prot.pkey,
                },
            };
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

pub fn packNativeVmaContiguousMapping(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    write_access: bool,
    free_list: *FreePageList,
    old_paddrs: []u64,
    new_paddrs: []u64,
) ?u64 {
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes) or size_bytes == 0) return null;
    const page_count_u64 = size_bytes / native_page_size;
    if (page_count_u64 == 0 or page_count_u64 > max_vmo_backing_pages) return null;
    const page_count: usize = @intCast(page_count_u64);
    if (old_paddrs.len < page_count or new_paddrs.len < page_count) return null;
    const end_va = @TypeOf(self.*).checkedEnd(start_va, size_bytes) catch return null;
    const table = self.getVmaTable(owner) orelse return null;
    var active_index: usize = 0;
    while (active_index < table.active_count) : (active_index += 1) {
        const entry_index: usize = @intCast(table.active_indices[active_index]);
        const entry = &table.entries[entry_index];
        if (start_va < entry.start_va or end_va > entry.endVa()) continue;
        if (!entry.flags.anonymous) return null;
        if (!@TypeOf(self.*).vmaProtAllowsFault(entry.prot, write_access, false)) return null;

        const first_vmo_page_u64 = (entry.vmo_offset + (start_va - entry.start_va)) / native_page_size;
        if (first_vmo_page_u64 > std.math.maxInt(usize)) return null;
        const first_vmo_page: usize = @intCast(first_vmo_page_u64);

        var first_paddr: u64 = 0;
        var already_contiguous = true;
        var i: usize = 0;
        while (i < page_count) : (i += 1) {
            const paddr = self.nativeVmoPagePaddrOrHole(entry.vmo, first_vmo_page + i) orelse return null;
            old_paddrs[i] = paddr;
            if (paddr == 0) {
                already_contiguous = false;
                continue;
            }
            if ((paddr & 0xFFF) != 0) return null;
            if (i == 0) {
                first_paddr = paddr;
            } else if (paddr != first_paddr + @as(u64, @intCast(i)) * native_page_size) {
                already_contiguous = false;
            }
        }
        if (already_contiguous and first_paddr != 0) {
            i = 0;
            while (i < page_count) : (i += 1) {
                new_paddrs[i] = first_paddr + @as(u64, @intCast(i)) * native_page_size;
            }
            return first_paddr;
        }

        const base_paddr = free_list.popContiguousBelow(page_count, @TypeOf(self.*).low_memory_limit) catch return null;
        i = 0;
        while (i < page_count) : (i += 1) {
            const new_paddr = base_paddr + @as(u64, @intCast(i)) * native_page_size;
            new_paddrs[i] = new_paddr;
            const dst: [*]u8 = @ptrFromInt(new_paddr);
            if (old_paddrs[i] == 0) {
                @memset(dst[0..native_page_size], 0);
            } else {
                const src: [*]const u8 = @ptrFromInt(old_paddrs[i]);
                @memcpy(dst[0..native_page_size], src[0..native_page_size]);
            }
        }

        self.replaceNativeVmoContiguousPages(entry.vmo, first_vmo_page, new_paddrs[0..page_count]) catch {
            var release: usize = 0;
            while (release < page_count) : (release += 1) {
                free_list.appendPage(0, new_paddrs[release]) catch {};
            }
            return null;
        };
        return base_paddr;
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

        const before_index = if (has_before) @TypeOf(self.*).findFreeVma(table) orelse return KernelError.TableFull else 0;
        if (has_before) try self.retainNativeVmo(original.vmo);
        errdefer if (has_before) self.releaseNativeVmo(original.vmo);
        if (has_before and !original.cow_table.isNull()) try self.retainNativeCowTable(original.cow_table);
        errdefer if (has_before and !original.cow_table.isNull()) self.releaseNativeCowTable(original.cow_table, null);
        if (has_before) {
            @TypeOf(self.*).installVmaEntry(table, before_index, original);
            table.entries[before_index].size_bytes = before_size;
        }

        const after_index = if (has_after) @TypeOf(self.*).findFreeVma(table) orelse return KernelError.TableFull else 0;
        if (has_after) try self.retainNativeVmo(original.vmo);
        errdefer if (has_after) self.releaseNativeVmo(original.vmo);
        if (has_after and !original.cow_table.isNull()) try self.retainNativeCowTable(original.cow_table);
        errdefer if (has_after and !original.cow_table.isNull()) self.releaseNativeCowTable(original.cow_table, null);
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
        if (!try @TypeOf(self.*).vmaRangeOverlaps(table, candidate, size_bytes)) {
            table.next_user_map_va = candidate + @TypeOf(self.*).pageAlignUp(size_bytes);
            return candidate;
        }
    }
    candidate = base;
    while (candidate < cursor and candidate + size_bytes <= end) : (candidate += granule) {
        if (!try @TypeOf(self.*).vmaRangeOverlaps(table, candidate, size_bytes)) {
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
    return !(try @TypeOf(self.*).vmaRangeOverlaps(table, start_va, size_bytes));
}

pub fn vmaProtAllowedByRights(prot: VmaProt, rights: FdRights) bool {
    if (prot.read and !rights.map_read) return false;
    if (prot.write and !rights.map_write) return false;
    if (prot.exec and !rights.map_exec) return false;
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

pub fn createAnonymousVmaWithPages(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    prot: VmaProt,
    flags: MmapFlags,
    free_list: *FreePageList,
) KernelError!NativeVmoRef {
    try self.requireActiveProcess(owner);
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes)) return KernelError.InvalidState;
    const page_count_u64 = size_bytes / native_page_size;
    if (page_count_u64 == 0 or page_count_u64 > max_vmo_backing_pages) return KernelError.InvalidState;

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
    flags: MmapFlags,
    vmo_ref: NativeVmoRef,
    vmo_offset: u64,
) KernelError!void {
    try self.requireActiveProcess(owner);
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes) or !@TypeOf(self.*).isPageAligned(vmo_offset)) return KernelError.InvalidState;
    const vmo = self.nativeVmoSlotConst(vmo_ref) orelse return KernelError.InvalidState;
    const vmo_end = try @TypeOf(self.*).checkedEnd(vmo_offset, size_bytes);
    if (vmo_end > vmo.size_bytes) return KernelError.InvalidState;

    const vma_table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
    if (try @TypeOf(self.*).vmaRangeOverlaps(vma_table, start_va, size_bytes)) return KernelError.InvalidState;
    const vma_index = @TypeOf(self.*).findFreeVma(vma_table) orelse return KernelError.TableFull;
    try self.retainNativeVmo(vmo_ref);
    @TypeOf(self.*).installVmaEntry(vma_table, vma_index, .{
        .active = true,
        .start_va = start_va,
        .size_bytes = size_bytes,
        .prot = prot,
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
    const object_slot = self.kernelObjectSlotConst(fd_entry.object) orelse return KernelError.InvalidState;
    const vmo_ref = switch (object_slot.payload) {
        .vmo => |ref| ref,
        else => return KernelError.InvalidState,
    };
    try self.createVmaWithRetainedVmo(owner, start_va, size_bytes, prot, flags, vmo_ref, vmo_offset);
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
    const object_slot = self.kernelObjectSlotConst(fd_entry.object) orelse return KernelError.InvalidState;
    const vmo_ref = switch (object_slot.payload) {
        .vmo => |ref| ref,
        else => return KernelError.InvalidState,
    };
    try self.createVmaWithRetainedVmo(target_owner, start_va, size_bytes, prot, flags, vmo_ref, vmo_offset);
    return start_va;
}

pub fn munmapRangeWithFreeList(
    self: anytype,
    owner: PrincipalId,
    start_va: u64,
    size_bytes: u64,
    free_list: *FreePageList,
) KernelError!void {
    try self.requireActiveProcess(owner);
    if (!@TypeOf(self.*).isPageAligned(start_va) or !@TypeOf(self.*).isPageAligned(size_bytes)) return KernelError.InvalidState;
    const end_va = try @TypeOf(self.*).checkedEnd(start_va, size_bytes);
    const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;

    var index: usize = 0;
    var seen_active: usize = 0;
    const active_limit = table.active_count;
    while (index < table.entries.len) : (index += 1) {
        var entry = &table.entries[index];
        if (!entry.active) continue;
        seen_active += 1;
        const entry_start = entry.start_va;
        const entry_end = entry.endVa();
        if (start_va >= entry_end or end_va <= entry_start) {
            if (seen_active >= active_limit) break;
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
            if (seen_active >= active_limit) break;
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
            if (seen_active >= active_limit) break;
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
            if (seen_active >= active_limit) break;
            continue;
        }

        const after_index = @TypeOf(self.*).findFreeVma(table) orelse return KernelError.TableFull;
        const original = entry.*;
        try self.retainNativeVmo(original.vmo);
        errdefer self.releaseNativeVmo(original.vmo);
        if (!original.cow_table.isNull()) try self.retainNativeCowTable(original.cow_table);
        errdefer if (!original.cow_table.isNull()) self.releaseNativeCowTable(original.cow_table, null);
        @TypeOf(self.*).installVmaEntry(table, after_index, original);
        table.entries[after_index].start_va = cut_end;
        table.entries[after_index].size_bytes = entry_end - cut_end;
        table.entries[after_index].vmo_offset = original.vmo_offset + (cut_end - entry_start);
        table.entries[after_index].cow_page_offset = original.cow_page_offset + @as(u32, @intCast((cut_end - entry_start) / native_page_size));

        entry = &table.entries[index];
        entry.size_bytes = cut_start - entry_start;
        self.releaseVmaCowPageRange(entry, @intCast((cut_start - entry_start) / native_page_size), cut_page_count, free_list);
        if (original.flags.anonymous or self.nativeVmoIsShadow(original.vmo)) {
            self.releaseUnmappedAnonymousVmoPageRange(owner, original.vmo, cut_vmo_first_page, cut_page_count, free_list);
        }
        if (seen_active >= active_limit) break;
    }
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
    try self.requireActiveProcess(owner);
    if (!@TypeOf(self.*).isPageAligned(old_start) or !@TypeOf(self.*).isPageAligned(old_size) or !@TypeOf(self.*).isPageAligned(new_size)) return KernelError.InvalidState;
    if (fixed and !@TypeOf(self.*).isPageAligned(new_start)) return KernelError.InvalidState;
    if (old_size == 0 or new_size == 0) return KernelError.InvalidState;
    const old_end = try @TypeOf(self.*).checkedEnd(old_start, old_size);

    const source = self.vmaEntryForVaConst(owner, old_start) orelse return KernelError.InvalidState;
    if (old_end > source.endVa()) return KernelError.InvalidState;
    if (!source.flags.anonymous) return KernelError.InvalidState;
    const source_prot = source.prot;
    const source_flags = source.flags;
    const source_vmo = source.vmo;
    const source_vmo_offset = source.vmo_offset + (old_start - source.start_va);

    if (!fixed and new_size <= old_size) {
        if (new_size < old_size) {
            try self.munmapRangeWithFreeList(owner, old_start + new_size, old_size - new_size, free_list);
        }
        return old_start;
    }
    if (!may_move) return KernelError.OutOfFreePages;
    if (fixed and new_start == 0) return KernelError.InvalidState;

    const target_start = if (fixed)
        new_start
    else
        try self.findRandomizedFreeUserMapVa(owner, new_size, 0x4d52_454d_4150_0000);
    const target_end = try @TypeOf(self.*).checkedEnd(target_start, new_size);
    if (target_start < old_end and target_end > old_start) return KernelError.InvalidState;

    if (fixed) {
        try self.munmapRangeWithFreeList(owner, target_start, new_size, free_list);
    }

    if (new_size <= old_size) {
        try self.createVmaWithRetainedVmo(owner, target_start, new_size, source_prot, source_flags, source_vmo, source_vmo_offset);
        errdefer self.munmapRangeWithFreeList(owner, target_start, new_size, free_list) catch {};
    } else {
        const dst_vmo = try self.createAnonymousVmaWithPages(owner, target_start, new_size, source_prot, source_flags, free_list);
        errdefer self.munmapRangeWithFreeList(owner, target_start, new_size, free_list) catch {};

        const copy_bytes = @min(old_size, new_size);
        const copy_pages: usize = @intCast(copy_bytes / native_page_size);
        const source_first_page: usize = @intCast(source_vmo_offset / native_page_size);
        var page_index: usize = 0;
        while (page_index < copy_pages) : (page_index += 1) {
            const src_paddr = self.nativeVmoPagePaddrOrHole(source_vmo, source_first_page + page_index) orelse return KernelError.InvalidState;
            if (src_paddr == 0) continue;
            const dst_paddr = (try self.allocPhysicalPage(free_list)).paddr;
            if (!builtin.is_test) {
                const src_page: [*]const u8 = @ptrFromInt(src_paddr);
                const dst_page: [*]u8 = @ptrFromInt(dst_paddr);
                @memcpy(dst_page[0..native_page_size], src_page[0..native_page_size]);
            }
            var page = [_]u64{dst_paddr};
            self.installNativeVmoPages(dst_vmo, page_index, page[0..]) catch |err| {
                free_list.appendPage(0, dst_paddr) catch {};
                return err;
            };
        }
    }

    try self.munmapRangeWithFreeList(owner, old_start, old_size, free_list);
    return target_start;
}
