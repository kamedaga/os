const lock_diagnostics = @import("../sync/lock_diagnostics.zig");
const std = @import("std");
const physical_layout = @import("../arch/x86_64/physical_layout.zig");
const builtin = @import("builtin");
const x86_platform = @import("../arch/x86_64/platform.zig");
const types = @import("types.zig");
const table_util = @import("table.zig");
const owned_storage = @import("../memory/owned_storage.zig");
const identity_allocator = @import("../memory/identity_allocator.zig");
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
const ProcessDescriptorTable = table_util.StaticPlusExtra(ProcessDescriptor, process_count);
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

pub fn allocKernelSlice(comptime T: type, free_list: *FreePageList, count: usize) ?[]T {
    if (count == 0) return null;
    const bytes = std.math.mul(usize, @sizeOf(T), count) catch return null;
    const rounded = std.math.add(usize, bytes, 4095) catch return null;
    const page_count = rounded / 4096;
    const paddr = free_list.popContiguousBelow(page_count, @import("../arch/x86_64/physical_layout.zig").allocation_limit) catch return null;
    // PMM grants these pages to this allocation; identitySlice validates the
    // conversion, while the table owner retains responsibility for lifetime.
    const raw = physical_layout.identitySlice(u8, .fromInt(paddr), page_count * 4096) orelse unreachable;
    @memset(raw, 0);
    const ptr: [*]T = @ptrCast(@alignCast(raw.ptr));
    return ptr[0..count];
}

pub fn processPrincipal(index: usize) PrincipalId {
    return processPrincipalFromIndex(index) orelse unreachable;
}

pub fn isActiveProcess(self: anytype, principal: PrincipalId) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    const mutable = @constCast(self);
    const interrupts = lockDescriptors(mutable);
    defer unlockDescriptors(mutable, interrupts);
    return (descriptorSlotLocked(mutable, index) orelse return false).active;
}

pub fn processDescriptor(self: anytype, principal: PrincipalId) ?ProcessDescriptor {
    const index = processIndexFromPrincipal(principal) orelse return null;
    const desc = self.processDescriptorSlotConst(index) orelse return null;
    if (!desc.active) return null;
    return desc;
}

pub fn processStatus(self: anytype, principal: PrincipalId) ProcessStatus {
    const index = processIndexFromPrincipal(principal) orelse return .{};
    const mutable = @constCast(self);
    const interrupts = lockDescriptors(mutable);
    defer unlockDescriptors(mutable, interrupts);
    const desc = descriptorSlotLocked(mutable, index) orelse return .{};
    return .{
        .active = desc.active,
        .faulted = desc.faulted,
        .fault_vector = desc.fault_vector,
    };
}

pub fn processSignalPendingHintVa(self: anytype, principal: PrincipalId) u64 {
    const index = processIndexFromPrincipal(principal) orelse return 0;
    const mutable = @constCast(self);
    const interrupts = lockDescriptors(mutable);
    defer unlockDescriptors(mutable, interrupts);
    const desc = descriptorSlotLocked(mutable, index) orelse return 0;
    return if (desc.active) desc.signal_pending_hint_va else 0;
}

pub fn setProcessSignalPendingHintVa(self: anytype, principal: PrincipalId, hint_va: u64) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    const interrupts = lockDescriptors(self);
    defer unlockDescriptors(self, interrupts);
    const desc = descriptorSlotLocked(self, index) orelse return false;
    if (!desc.active) return false;
    desc.signal_pending_hint_va = hint_va;
    return true;
}

pub fn clearProcessSignalPendingHintVa(self: anytype, principal: PrincipalId) bool {
    return self.setProcessSignalPendingHintVa(principal, 0);
}

pub fn isBootstrapOwner(self: anytype, principal: PrincipalId) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    const mutable = @constCast(self);
    const interrupts = lockDescriptors(mutable);
    defer unlockDescriptors(mutable, interrupts);
    const desc = descriptorSlotLocked(mutable, index) orelse return false;
    return desc.active and desc.bootstrap_owner;
}

pub fn hasActivePrincipal(self: anytype, principal: PrincipalId) bool {
    if (processIndexFromPrincipal(principal) != null) return self.isActiveProcess(principal);
    return principal == .Device0;
}

pub fn requireActiveProcess(self: anytype, principal: PrincipalId) KernelError!void {
    if (!self.isActiveProcess(principal)) return KernelError.InvalidState;
}

pub fn requireActivePrincipal(self: anytype, principal: PrincipalId) KernelError!void {
    if (!self.hasActivePrincipal(principal)) return KernelError.InvalidState;
}

pub fn extraIndex(index: usize) ?usize {
    return ProcessDescriptorTable.extraIndex(index);
}

fn descriptorSlotLocked(self: anytype, index: usize) ?*ProcessDescriptor {
    if (index < process_count) return &self.process_descriptors[index];
    const extra = index - process_count;
    if (extra >= self.process_descriptors_extra.len) return null;
    return &self.process_descriptors_extra[extra];
}

/// Snapshot, not a reference or admission reservation. Writers remain under
/// KernelState exclusion; short readers need not acquire that outer lock.
pub fn processDescriptorSlotConst(self: anytype, index: usize) ?ProcessDescriptor {
    const mutable = @constCast(self); // Mutable KernelState through a const view.
    const interrupts = lockDescriptors(mutable);
    defer unlockDescriptors(mutable, interrupts);
    return (descriptorSlotLocked(mutable, index) orelse return null).*;
}

pub fn processCapacitySnapshot(self: anytype) usize {
    const mutable = @constCast(self);
    const interrupts = lockDescriptors(mutable);
    defer unlockDescriptors(mutable, interrupts);
    return self.process_capacity;
}

fn disableDescriptorInterrupts() bool {
    var flags: u64 = 0;
    if (!builtin.is_test) asm volatile ("pushfq; pop %[flags]; cli"
        : [flags] "=r" (flags),
        :
        : .{ .memory = true });
    return flags & (1 << 9) != 0;
}

fn restoreDescriptorInterrupts(interrupts: bool) void {
    if (!builtin.is_test and interrupts) asm volatile ("sti" ::: .{ .memory = true });
}

fn lockDescriptors(self: anytype) bool {
    const interrupts = disableDescriptorInterrupts();
    // Leaf only: never run a callback, allocate, release an FD, or acquire a
    // VMA/AS lock here. Diagnostics use tryProcessLabel to avoid exception
    // reentry deadlocking on the interrupted CPU's own descriptor leaf.
    lock_diagnostics.beforeAcquire(&self.descriptor_lock.record, .{ .class = .process_descriptor });
    while (@cmpxchgWeak(u8, &self.descriptor_lock.value, 0, 1, .acquire, .monotonic) != null) {
        if (builtin.is_test) if (self.descriptor_test_probe) |probe| {
            @atomicStore(u8, probe, 1, .release);
        };
        while (@atomicLoad(u8, &self.descriptor_lock.value, .monotonic) != 0) asm volatile ("pause");
    }
    lock_diagnostics.acquired(&self.descriptor_lock.record, .{ .class = .process_descriptor });
    return interrupts;
}

fn unlockDescriptors(self: anytype, interrupts: bool) void {
    lock_diagnostics.releasing(&self.descriptor_lock.record);
    @atomicStore(u8, &self.descriptor_lock.value, 0, .release);
    restoreDescriptorInterrupts(interrupts);
}

/// Diagnostic-only nonblocking read: exceptions/NMIs can interrupt a writer
/// even with IF=0. On contention the caller uses its static principal label.
/// Label bytes must have immutable kernel lifetime (current callers: literals);
/// copying the slice does not prolong the lifetime of its backing bytes.
pub fn tryProcessLabel(self: anytype, principal: PrincipalId) ?[]const u8 {
    const index = processIndexFromPrincipal(principal) orelse return null;
    const mutable = @constCast(self);
    const interrupts = disableDescriptorInterrupts();
    if (@cmpxchgStrong(u8, &mutable.descriptor_lock.value, 0, 1, .acquire, .monotonic) != null) {
        restoreDescriptorInterrupts(interrupts);
        return null;
    }
    lock_diagnostics.acquiredTry(&mutable.descriptor_lock.record, .{ .class = .process_descriptor });
    defer unlockDescriptors(mutable, interrupts);
    const desc = descriptorSlotLocked(mutable, index) orelse return null;
    return if (desc.active) desc.label else null;
}

pub fn fdTableForProcessIndex(self: anytype, index: usize) ?*FdTable {
    // KernelState (or pre-publication boot exclusivity) covers the entire
    // borrow. No table/entry borrow may survive dropping that exclusion.
    if (index >= self.process_capacity) return null;
    if (@TypeOf(self.*).extraIndex(index)) |extra| return self.fd_tables_extra.get(@intCast(extra));
    return &self.fd_tables[index];
}

pub fn fdTableForProcessIndexConst(self: anytype, index: usize) ?*const FdTable {
    if (index >= self.process_capacity) return null;
    if (@TypeOf(self.*).extraIndex(index)) |extra| return self.fd_tables_extra.get(@intCast(extra));
    return &self.fd_tables[index];
}

pub fn vmaTableForProcessIndex(self: anytype, index: usize) ?*VmaTable {
    if (index < process_count) return &self.vma_tables[index];
    const interrupts = lockVmaDirectory(self);
    defer unlockVmaDirectory(self, interrupts);
    const extra = index - process_count;
    if (extra >= self.vma_tables_extra.len) return null;
    return self.vma_tables_extra[extra];
}

pub fn vmaTableForProcessIndexConst(self: anytype, index: usize) ?*const VmaTable {
    // Production KernelState is mutable storage seen through a const view,
    // not an object in read-only memory. Only its synchronization word changes;
    // no mutable body borrow escapes. Body access still needs AS/shared locks.
    return vmaTableForProcessIndex(@constCast(self), index);
}

fn vmaStorageInvariantFailure() noreturn {
    // May run with the leaf held: logging would reenter unrelated subsystems.
    if (builtin.is_test) @panic("VMA storage ownership invariant");
    while (true) asm volatile ("cli; hlt");
}

fn lockVmaDirectory(self: anytype) bool {
    var flags: u64 = 0;
    if (!builtin.is_test) asm volatile ("pushfq; pop %[flags]; cli"
        : [flags] "=r" (flags),
        :
        : .{ .memory = true });
    // KernelState writers and AS/shared fault readers converge on this leaf.
    // No allocation, external lock or callback is allowed while held. IF=0
    // prevents an IRQ from waiting for its own interrupted reader.
    lock_diagnostics.beforeAcquire(&self.vma_directory_lock.record, .{ .class = .vma_directory });
    while (@cmpxchgWeak(u8, &self.vma_directory_lock.value, 0, 1, .acquire, .monotonic) != null) {
        if (builtin.is_test) if (self.vma_directory_test_probe) |probe| {
            @atomicStore(u8, probe, 1, .release);
        };
        while (@atomicLoad(u8, &self.vma_directory_lock.value, .monotonic) != 0) asm volatile ("pause");
    }
    lock_diagnostics.acquired(&self.vma_directory_lock.record, .{ .class = .vma_directory });
    return flags & (1 << 9) != 0;
}

fn unlockVmaDirectory(self: anytype, interrupts: bool) void {
    lock_diagnostics.releasing(&self.vma_directory_lock.record);
    @atomicStore(u8, &self.vma_directory_lock.value, 0, .release);
    if (!builtin.is_test and interrupts) asm volatile ("sti" ::: .{ .memory = true });
}

fn retryVmaDirectoryReturns(self: anytype) void {
    // KernelState writer exclusion, outside the reader leaf. No directory
    // pointer escapes lookup; this says nothing about body/FD/descriptor loans.
    self.vma_directory_storage.retryUnpublished() catch vmaStorageInvariantFailure();
    self.vma_directory_storage.reclaimRetiredAfterBorrowExclusion() catch vmaStorageInvariantFailure();
    self.vma_body_storage.retryUnpublished() catch vmaStorageInvariantFailure();
}

pub fn getFdTable(self: anytype, principal: PrincipalId) ?*FdTable {
    const index = processIndexFromPrincipal(principal) orelse return null;
    return self.fdTableForProcessIndex(index);
}

pub fn getFdTableConst(self: anytype, principal: PrincipalId) ?*const FdTable {
    const index = processIndexFromPrincipal(principal) orelse return null;
    return self.fdTableForProcessIndexConst(index);
}

pub fn reclaimFdStorage(table: *FdTable, free_list: *FreePageList) void {
    _ = free_list;
    // K excludes directory readers. Stable entry bodies are retained until
    // teardown; only obsolete directory paths and failed drafts are returned.
    table.entries.reclaim();
}

pub fn releaseFdStorage(self: anytype, table: *FdTable, free_list: *FreePageList) void {
    self.retireFdStorage(table);
    reclaimFdStorage(table, free_list);
}

pub fn ensureFdTableCapacity(self: anytype, owner: PrincipalId, minimum: usize, free_list: *FreePageList) KernelError!void {
    try self.requireActiveProcess(owner);
    const table = self.getFdTable(owner) orelse return KernelError.InvalidState;
    defer self.finishFdGrowth(table);
    try table.ensureCapacity(@max(types.fd_table_entries, minimum), identity_allocator.allocator(free_list), &self.object_directory_leaf, &self.fd_retired);
}

pub fn grantFdsForProcessCreate(self: anytype, from: PrincipalId, to: PrincipalId, grants: []const @import("kernel_abi_root").process_abi.ProcessFdGrant, free_list: *FreePageList) KernelError!void {
    if (from == to) return KernelError.InvalidState;
    if (grants.len > @import("kernel_abi_root").process_abi.process_create_max_grants)
        return KernelError.InvalidState;
    const source_table = try self.fdTableForActiveProcessConst(from);
    const dest_table = try self.fdTableForActiveProcess(to);
    var capacity: usize = 0;
    for (grants, 0..) |grant, i| {
        if (grant.source_fd >= source_table.capacity or grant.target_fd >= types.fd_number_capacity or
            (grant.rights & ~types.fd_known_rights_mask) != 0 or
            (grant.flags & ~@as(u64, types.fd_known_flags_mask)) != 0) return KernelError.InvalidState;
        for (grants[0..i]) |previous| if (previous.target_fd == grant.target_fd) return KernelError.InvalidState;
        const source = source_table.at(@intCast(grant.source_fd)).*;
        if (source.object.isNull() or !source.rights.transfer or
            (grant.rights & ~fdRightsToBits(source.rights)) != 0 or
            self.kernelObjectSlotConst(source.object) == null or
            self.kernelObjectIsPinnedUserObject(source.object)) return KernelError.InvalidState;
        if (grant.target_fd < dest_table.capacity and
            !dest_table.at(@intCast(grant.target_fd)).*.isEmpty()) return KernelError.InvalidState;
        capacity = @max(capacity, @as(usize, @intCast(grant.target_fd)) + 1);
    }
    try self.ensureFdTableCapacity(to, capacity, free_list);
    var nodes: [@import("kernel_abi_root").process_abi.process_create_max_grants]?*types.references.Node = .{null} ** @import("kernel_abi_root").process_abi.process_create_max_grants;
    defer for (nodes) |node| if (node) |reserved| types.references.Pool.cancel(reserved);
    for (grants, 0..) |grant, position| nodes[position] = try self.prepareFdReference(source_table.at(@intCast(grant.source_fd)).object);
    var installed: usize = 0;
    errdefer for (grants[0..installed]) |grant|
        self.closeFdWithFreeList(to, @intCast(grant.target_fd), free_list) catch {};
    for (grants, 0..) |grant, position| {
        const source = source_table.at(@intCast(grant.source_fd)).*;
        try self.retainKernelObject(source.object);
        dest_table.write(@intCast(grant.target_fd), .{
            .object = source.object,
            .rights = fdRightsFromBits(grant.rights),
            .flags = fdFlagsFromBits(@intCast(grant.flags)),
            .cursor = source.cursor,
        });
        self.publishFdReference(to, @intCast(grant.target_fd), dest_table.at(@intCast(grant.target_fd)), nodes[position]);
        nodes[position] = null;
        installed += 1;
    }
}

pub fn cloneFdTableForFork(self: anytype, from: PrincipalId, to: PrincipalId) KernelError!void {
    if (from == to) return KernelError.InvalidState;
    const source_table = try self.fdTableForActiveProcessConst(from);
    const dest_table = try self.fdTableForActiveProcess(to);
    if (dest_table.capacity < source_table.capacity) return KernelError.TableFull;
    // Reserve the whole publication set before the first child FD appears.
    // Empty entries may hold draft nodes only while this K transaction runs.
    defer for (0..dest_table.capacity) |index| {
        const entry = dest_table.at(index);
        if (entry.object.isNull()) if (entry.authority.reference) |node| {
            types.references.Pool.cancel(node);
            entry.authority.reference = null;
        };
    };
    for (0..source_table.capacity) |index| {
        const source = source_table.at(index).*;
        if (source.object.isNull() or source.flags.private) continue;
        if (!dest_table.at(index).*.isEmpty()) return KernelError.InvalidState;
        dest_table.at(index).authority.reference = try self.prepareFdReference(source.object);
    }
    var fd_index: usize = 0;
    errdefer {
        var release_index: usize = 0;
        while (release_index < dest_table.capacity) : (release_index += 1) {
            const object_ref = dest_table.at(release_index).*.object;
            if (object_ref.isNull()) continue;
            self.unpublishFdReference(dest_table.at(release_index));
            dest_table.write(release_index, .{});
            self.releaseKernelObject(object_ref);
        }
    }
    while (fd_index < source_table.capacity) : (fd_index += 1) {
        const source = source_table.at(fd_index).*;
        if (source.object.isNull() or source.flags.private) continue;
        if (!dest_table.at(fd_index).*.isEmpty()) return KernelError.InvalidState;
        try self.retainKernelObject(source.object);
        const node = dest_table.at(fd_index).authority.reference;
        dest_table.write(fd_index, .{
            .object = source.object,
            .rights = fdRightsFromBits(fdRightsToBits(source.rights)),
            .flags = fdFlagsFromBits(fdFlagsToBits(source.flags)),
            .cursor = source.cursor,
        });
        self.publishFdReference(to, fd_index, dest_table.at(fd_index), node);
    }
}

pub fn processFromPayload(payload: *const KernelObjectPayload) ?ProcessObject {
    return switch (payload.*) {
        .process => |process| process,
        else => null,
    };
}

pub fn threadFromPayload(payload: *const KernelObjectPayload) ?ThreadObject {
    return switch (payload.*) {
        .thread => |thread| thread,
        else => null,
    };
}

pub fn processObjectForFd(self: anytype, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?ProcessObject {
    const view = self.fdPayloadWithRightsConst(owner, fd, required_rights) orelse return null;
    const process = @TypeOf(self.*).processFromPayload(view.payload) orelse return null;
    // A terminal object remains waitable after its descriptor is retired.
    // An active object, however, must never retarget a later process that
    // reused the same principal index.
    if (!process.state.isTerminal() and !self.processObjectTargetsCurrentGeneration(process)) return null;
    return process;
}

pub fn processObjectTargetsCurrentGeneration(self: anytype, process: ProcessObject) bool {
    const principal: PrincipalId = @enumFromInt(process.principal_raw);
    const index = processIndexFromPrincipal(principal) orelse return false;
    const desc = self.processDescriptorSlotConst(index) orelse return false;
    return desc.active and process.generation != 0 and process.generation == desc.generation;
}

pub fn threadObjectForFd(self: anytype, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?ThreadObject {
    const view = self.fdPayloadWithRightsConst(owner, fd, required_rights) orelse return null;
    return @TypeOf(self.*).threadFromPayload(view.payload);
}

pub fn createProcessFd(
    self: anytype,
    owner: PrincipalId,
    process_input: ProcessObject,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    var process = process_input;
    const principal: PrincipalId = @enumFromInt(process.principal_raw);
    const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
    const desc = self.processDescriptorSlotConst(index) orelse return KernelError.InvalidState;
    if (!desc.active or desc.generation == 0) return KernelError.InvalidState;
    process.generation = desc.generation;
    const object_ref = try self.createKernelObject(.process, .{ .process = process });
    return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
        if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
        return err;
    };
}

pub fn createThreadFd(
    self: anytype,
    owner: PrincipalId,
    thread: ThreadObject,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    if (thread.thread_generation == 0) return KernelError.InvalidState;
    if (processIndexFromPrincipal(@enumFromInt(thread.owner_principal_raw)) == null) return KernelError.InvalidState;
    const object_ref = try self.createKernelObject(.thread, .{ .thread = thread });
    return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
        if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
        return err;
    };
}

pub fn setProcessObjectStateForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    required_rights: FdRights,
    state: TaskObjectState,
    exit_code: u32,
) KernelError!ProcessObject {
    const view = self.fdPayloadWithRights(owner, fd, required_rights) orelse return KernelError.InvalidState;
    var process = @TypeOf(self.*).processFromPayload(view.payload) orelse return KernelError.InvalidState;
    if (!self.processObjectTargetsCurrentGeneration(process)) return KernelError.InvalidState;
    process.state = state;
    process.exit_code = exit_code;
    view.payload.* = .{ .process = process };
    return process;
}

pub fn setThreadObjectStateForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    required_rights: FdRights,
    state: TaskObjectState,
    exit_code: u32,
) KernelError!ThreadObject {
    const view = self.fdPayloadWithRights(owner, fd, required_rights) orelse return KernelError.InvalidState;
    var thread = @TypeOf(self.*).threadFromPayload(view.payload) orelse return KernelError.InvalidState;
    thread.state = state;
    thread.exit_code = exit_code;
    view.payload.* = .{ .thread = thread };
    return thread;
}

pub fn markProcessObjectsExited(self: anytype, principal: PrincipalId, state: TaskObjectState, exit_code: u32) void {
    var i: usize = 0;
    const principal_raw: PrincipalRaw = @intFromEnum(principal);
    const index = processIndexFromPrincipal(principal) orelse return;
    const generation = (self.processDescriptorSlotConst(index) orelse return).generation;
    if (generation == 0) return;
    while (i < self.fd_objects.capacity) : (i += 1) {
        const slot = (self.fd_objects.get(@intCast(i)) orelse continue);
        if (slot.kind != .process) continue;
        var process = @TypeOf(self.*).processFromPayload(&slot.payload) orelse continue;
        if (process.principal_raw != principal_raw or process.generation != generation) continue;
        if (!state.isTerminal() and process.state.isTerminal()) continue;
        process.state = state;
        process.exit_code = exit_code;
        slot.payload = .{ .process = process };
    }
}

pub fn markThreadObjectsExitedForPrincipal(self: anytype, principal: PrincipalId, state: TaskObjectState, exit_code: u32) void {
    var i: usize = 0;
    const principal_raw: PrincipalRaw = @intFromEnum(principal);
    while (i < self.fd_objects.capacity) : (i += 1) {
        const slot = (self.fd_objects.get(@intCast(i)) orelse continue);
        if (slot.kind != .thread) continue;
        var thread = @TypeOf(self.*).threadFromPayload(&slot.payload) orelse continue;
        if (thread.owner_principal_raw != principal_raw) continue;
        // An old terminal capability may outlive reuse of its owner principal.
        // Keep this scan linear: per-slot updates would rescan the complete
        // object table for every live object (including STOP/CONT aliases).
        if (thread.state.isTerminal()) continue;
        thread.state = state;
        thread.exit_code = exit_code;
        slot.payload = .{ .thread = thread };
    }
}

pub fn markThreadObjectsExitedBySlot(self: anytype, thread_index: usize, thread_generation: u32, state: TaskObjectState, exit_code: u32) void {
    var i: usize = 0;
    while (i < self.fd_objects.capacity) : (i += 1) {
        const slot = (self.fd_objects.get(@intCast(i)) orelse continue);
        if (slot.kind != .thread) continue;
        var thread = @TypeOf(self.*).threadFromPayload(&slot.payload) orelse continue;
        if (thread.thread_index != thread_index or thread.thread_generation != thread_generation) continue;
        if (thread.state.isTerminal()) continue;
        thread.state = state;
        thread.exit_code = exit_code;
        slot.payload = .{ .thread = thread };
    }
}

pub fn nextProcessCapacity(self: anytype, required: usize) ?usize {
    return table_util.nextGeometricCapacity(
        self.process_capacity,
        process_count,
        process_principal_value_count,
        required,
    );
}

pub fn ensureProcessCapacity(self: anytype, required: usize, free_list: *FreePageList) bool {
    retryVmaDirectoryReturns(self);
    retryDescriptorReturns(self);
    retryFdDirectoryReturns(self);
    if (required <= self.process_capacity) return true;
    const capacity = self.nextProcessCapacity(required) orelse return false;
    const extra_count = ProcessDescriptorTable.extraCount(capacity);
    const old_extra_count = ProcessDescriptorTable.extraCount(self.process_capacity);

    // KernelState serializes writers but not fault-side AS/shared readers.
    // Reserve everything before publication; copy VMA pointers, never bodies,
    // so a fault's later write cannot be stranded in a retired table copy.
    var descriptors = owned_storage.Transaction.init(&self.descriptor_storage, identity_allocator.allocator(free_list));
    defer if (!descriptors.finished) descriptors.rollback() catch vmaStorageInvariantFailure();
    const new_desc = descriptors.reserve(ProcessDescriptor, extra_count, .replaceable) catch return false;
    var directory = owned_storage.Transaction.init(&self.vma_directory_storage, identity_allocator.allocator(free_list));
    defer if (!directory.finished) directory.rollback() catch vmaStorageInvariantFailure();
    const new_vma = directory.reserve(*VmaTable, extra_count, .replaceable) catch return false;
    var bodies = owned_storage.Transaction.init(&self.vma_body_storage, identity_allocator.allocator(free_list));
    defer if (!bodies.finished) bodies.rollback() catch vmaStorageInvariantFailure();
    const added_vma = bodies.reserve(VmaTable, extra_count - old_extra_count, .stable) catch return false;

    // Only the FD directory changes here: append stable canonical tables.
    // Unpublished prepared slots on OOM remain owned and are reused on retry.
    self.fd_tables_extra.return_queue = &self.fd_retired;
    self.fd_process_retired = true;
    defer self.reclaimFdCapabilities();
    while (self.fd_tables_extra.live < extra_count) {
        _ = self.fd_tables_extra.allocate(identity_allocator.allocator(free_list), &self.object_directory_leaf, std.math.maxInt(u32)) catch return false;
    }

    var i = old_extra_count;
    while (i < extra_count) : (i += 1) {
        new_desc[i] = .{};
        added_vma[i - old_extra_count] = .{};
        new_vma[i] = &added_vma[i - old_extra_count];
    }

    // FD tables are already stable; VMA/descriptor publication keeps its own leaves.
    const interrupts = lockVmaDirectory(self);
    @memcpy(new_vma[0..old_extra_count], self.vma_tables_extra);
    // No recoverable failures after the first commit: all drafts and bodies
    // are initialized. Keep directory readers excluded until owners/root agree.
    bodies.commitAdditionalStable() catch vmaStorageInvariantFailure();
    directory.commit() catch vmaStorageInvariantFailure();
    self.vma_tables_extra = new_vma;
    unlockVmaDirectory(self, interrupts);
    // Publish descriptor bounds/capacity last, with the other roots already
    // installed. New slots remain inactive until create publishes them. Never
    // nest the two reader leaves; no fallible step remains after first commit.
    const descriptor_interrupts = lockDescriptors(self);
    @memcpy(new_desc[0..old_extra_count], self.process_descriptors_extra);
    descriptors.commit() catch vmaStorageInvariantFailure();
    self.process_descriptors_extra = new_desc;
    self.process_capacity = capacity;
    unlockDescriptors(self, descriptor_interrupts);
    retryVmaDirectoryReturns(self);
    retryDescriptorReturns(self);
    retryFdDirectoryReturns(self);
    @atomicStore(usize, &self.fd_process_capacity, capacity, .seq_cst);
    return true;
}

fn retryFdDirectoryReturns(self: anytype) void {
    // The leaf ends directory borrows; canonical table bodies never move.
    self.fd_tables_extra.reclaim();
}

fn retryDescriptorReturns(self: anytype) void {
    // Only snapshots escape this directory; a completed publication leaf
    // excludes every old-array reader. FD table loans have no such proof.
    self.descriptor_storage.retryUnpublished() catch vmaStorageInvariantFailure();
    self.descriptor_storage.reclaimRetiredAfterBorrowExclusion() catch vmaStorageInvariantFailure();
}

// Live admission owns K and chooses an inactive slot after its old A/S
// teardown and CPU quiescence. K-only VMA/FD loans cannot cross that interval.
// Guarded FD entry bytes may survive, but changed seq/incarnation grants no
// authority to the reused process slot.
// The only other caller is unpublished bootstrap descriptor construction.
pub fn clearPrincipalTablesForReuse(self: anytype, index: usize) void {
    self.resetProcessRuntimeTables(index);
}

fn nextProcessGeneration(generation: u32) ?u32 {
    // Inactive descriptors retain identity history until stage B. Reusing an
    // exhausted slot would make an old process capability authoritative again.
    if (generation >= types.max_process_generation) return null;
    return generation + 1;
}

pub fn createProcessDescriptorBelowChecked(
    self: anytype,
    label: []const u8,
    limit: usize,
    reusable: ?*const fn (PrincipalId) bool,
) ?PrincipalId {
    var i: usize = 0;
    const capped_limit = @min(limit, self.process_capacity);
    while (i < capped_limit) : (i += 1) {
        const previous = self.processDescriptorSlotConst(i) orelse continue;
        if (previous.active) continue;
        const principal = @TypeOf(self.*).processPrincipal(i);
        if (reusable) |is_reusable| {
            if (!is_reusable(principal)) continue;
        }
        const generation = nextProcessGeneration(previous.generation) orelse continue;
        const timer = self.process_timers.reserve(self.poolAllocator() catch return null, principal, generation) catch return null;
        self.clearPrincipalTablesForReuse(i);
        const interrupts = lockDescriptors(self);
        const desc = descriptorSlotLocked(self, i) orelse vmaStorageInvariantFailure();
        desc.* = .{
            .syscall_counts = desc.syscall_counts,
            .active = true,
            .principal = principal,
            .generation = generation,
            .signal_timer = timer,
            .label = label,
        };
        self.active_process_count += 1;
        unlockDescriptors(self, interrupts);
        if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .create);
        return principal;
    }
    return null;
}

pub fn createProcessDescriptorBelow(self: anytype, label: []const u8, limit: usize) ?PrincipalId {
    return createProcessDescriptorBelowChecked(self, label, limit, null);
}

pub fn createProcessDescriptor(self: anytype, label: []const u8) ?PrincipalId {
    return createProcessDescriptorBelow(self, label, self.process_capacity);
}

pub fn createProcessDescriptorWithCapacity(self: anytype, label: []const u8, free_list: *FreePageList) ?PrincipalId {
    if (self.createProcessDescriptor(label)) |principal| return principal;
    if (!self.ensureProcessCapacity(self.process_capacity + 1, free_list)) return null;
    return self.createProcessDescriptor(label);
}

pub fn createProcessDescriptorWithCapacityLimit(
    self: anytype,
    label: []const u8,
    free_list: *FreePageList,
    limit: usize,
) ?PrincipalId {
    if (limit == 0) return null;
    if (createProcessDescriptorBelow(self, label, limit)) |principal| return principal;
    if (self.process_capacity >= limit) return null;
    const required = @min(self.process_capacity + 1, limit);
    if (!self.ensureProcessCapacity(required, free_list)) return null;
    return createProcessDescriptorBelow(self, label, limit);
}

pub fn createProcessDescriptorWithCapacityLimitChecked(
    self: anytype,
    label: []const u8,
    free_list: *FreePageList,
    limit: usize,
    reusable: *const fn (PrincipalId) bool,
) ?PrincipalId {
    if (limit == 0) return null;
    if (createProcessDescriptorBelowChecked(self, label, limit, reusable)) |principal| return principal;
    if (self.process_capacity >= limit) return null;
    const required = @min(self.process_capacity + 1, limit);
    if (!self.ensureProcessCapacity(required, free_list)) return null;
    return createProcessDescriptorBelowChecked(self, label, limit, reusable);
}

pub fn createProcessDescriptorWithUserAddressSpaceChecked(
    self: anytype,
    label: []const u8,
    free_list: *FreePageList,
    user_spaces: anytype,
    reusable: *const fn (PrincipalId) bool,
) ?PrincipalId {
    var principal = createProcessDescriptorBelowChecked(
        self,
        label,
        self.process_capacity,
        reusable,
    );
    if (principal == null) {
        const capacity = self.nextProcessCapacity(self.process_capacity + 1) orelse return null;
        // Grow the address-space directory first. If process-table growth then
        // fails, the extra empty directory entries are harmless and reusable.
        if (!user_spaces.ensureCapacity(capacity, free_list)) return null;
        if (!self.ensureProcessCapacity(capacity, free_list)) return null;
        principal = createProcessDescriptorBelowChecked(
            self,
            label,
            capacity,
            reusable,
        );
    }

    const created = principal orelse return null;
    const index = processIndexFromPrincipal(created) orelse unreachable;
    if (user_spaces.ensureSlot(index, free_list) == null) {
        _ = self.removeProcessDescriptor(created);
        return null;
    }
    return created;
}

// Fresh boot construction only (plus host fixtures); never live admission.
// Runtime callers use the checked allocator under K and its reusable predicate.
pub fn ensureProcessDescriptor(self: anytype, principal: PrincipalId, label: []const u8) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    const previous = self.processDescriptorSlotConst(index) orelse return false;
    if (previous.active) {
        const interrupts = lockDescriptors(self);
        defer unlockDescriptors(self, interrupts);
        const desc = descriptorSlotLocked(self, index) orelse vmaStorageInvariantFailure();
        desc.label = label;
        return true;
    }
    const generation = nextProcessGeneration(previous.generation) orelse return false;
    const timer = self.process_timers.reserve(self.poolAllocator() catch return false, principal, generation) catch return false;
    self.clearPrincipalTablesForReuse(index);
    const interrupts = lockDescriptors(self);
    const desc = descriptorSlotLocked(self, index) orelse vmaStorageInvariantFailure();
    desc.* = .{
        .syscall_counts = desc.syscall_counts,
        .active = true,
        .principal = principal,
        .generation = generation,
        .signal_timer = timer,
        .label = label,
    };
    self.active_process_count += 1;
    unlockDescriptors(self, interrupts);
    if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .ensure);
    return true;
}

pub fn setBootstrapOwner(self: anytype, principal: PrincipalId, enabled: bool) KernelError!void {
    const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
    const interrupts = lockDescriptors(self);
    defer unlockDescriptors(self, interrupts);
    const desc = descriptorSlotLocked(self, index) orelse return KernelError.InvalidState;
    if (!desc.active) return KernelError.InvalidState;
    desc.bootstrap_owner = enabled;
}

pub fn markProcessFaulted(self: anytype, principal: PrincipalId, fault_vector: u8) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    if (!self.isActiveProcess(principal)) return false;
    releaseProcessTimer(self, index);
    self.releaseFdTableForProcessIndex(index);
    const interrupts = lockDescriptors(self);
    const desc = descriptorSlotLocked(self, index) orelse vmaStorageInvariantFailure();
    desc.active = false;
    desc.bootstrap_owner = false;
    desc.signal_pending_hint_va = 0;
    desc.faulted = true;
    desc.fault_vector = fault_vector;
    if (self.active_process_count > 0) self.active_process_count -= 1;
    unlockDescriptors(self, interrupts);
    if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .fault);
    return true;
}

pub fn markProcessExited(self: anytype, principal: PrincipalId) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    if (!self.isActiveProcess(principal)) return false;
    releaseProcessTimer(self, index);
    self.releaseFdTableForProcessIndex(index);
    const interrupts = lockDescriptors(self);
    const desc = descriptorSlotLocked(self, index) orelse vmaStorageInvariantFailure();
    desc.active = false;
    desc.bootstrap_owner = false;
    desc.signal_pending_hint_va = 0;
    desc.faulted = false;
    desc.fault_vector = 0;
    if (self.active_process_count > 0) self.active_process_count -= 1;
    unlockDescriptors(self, interrupts);
    if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .exit);
    return true;
}

pub fn removeProcessDescriptor(self: anytype, principal: PrincipalId) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    if (!self.isActiveProcess(principal)) return false;
    releaseProcessTimer(self, index);
    self.releaseFdTableForProcessIndex(index);
    const interrupts = lockDescriptors(self);
    const desc = descriptorSlotLocked(self, index) orelse vmaStorageInvariantFailure();
    const generation = desc.generation;
    desc.* = .{ .generation = generation, .syscall_counts = desc.syscall_counts };
    if (self.active_process_count > 0) self.active_process_count -= 1;
    unlockDescriptors(self, interrupts);
    if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .remove);
    return true;
}

// Fresh, unpublished KernelState initialization only; this is not live reset.
// Runtime teardown resets one slot inside its A/S transaction while holding K.
pub fn clearPrincipalState(self: anytype) void {
    var i: usize = 0;
    while (i < self.process_capacity) : (i += 1) {
        releaseProcessTimer(self, i);
        self.resetProcessRuntimeTables(i);
    }
    resetVmoBackingPageStore();
    self.resetKernelObjectTable();
    self.resetNativeVmoTable();
    self.resetNativeIpcObjects();
    i = 0;
    const interrupts = lockDescriptors(self);
    defer unlockDescriptors(self, interrupts);
    while (i < self.process_capacity) : (i += 1) {
        (descriptorSlotLocked(self, i) orelse break).* = .{};
    }
    self.active_process_count = 0;
}

pub fn initPrincipalState(self: anytype) void {
    self.clearPrincipalState();
    for (0..initial_process_count) |i| {
        const principal = @TypeOf(self.*).processPrincipal(i);
        if (!self.ensureProcessDescriptor(principal, principalLabel(principal)))
            vmaStorageInvariantFailure();
    }
}

fn releaseProcessTimer(self: anytype, index: usize) void {
    const descriptor = self.processDescriptorSlotConst(index) orelse return;
    const ref = descriptor.signal_timer orelse return;
    // K excludes expiry, reset and reuse; detach the node before the stable
    // record can be returned. Allocator callbacks never run under the leaf.
    self.process_timers.release(ref);
    const interrupts = lockDescriptors(self);
    defer unlockDescriptors(self, interrupts);
    (descriptorSlotLocked(self, index) orelse unreachable).signal_timer = null;
}

pub fn initDynamicPrincipalState(self: anytype) void {
    self.clearPrincipalState();
}

// Entry counting precedes K acquisition; use the descriptor leaf so growth
// cannot copy a row and lose increments to its retired array. No pointer
// escapes the leaf, and no allocation or callback runs while it is held.
pub fn recordPrincipalSyscall(self: anytype, principal: PrincipalId, column: usize) void {
    const index = processIndexFromPrincipal(principal) orelse return;
    const flags = lockDescriptors(self);
    defer unlockDescriptors(self, flags);
    const desc = descriptorSlotLocked(self, index) orelse return;
    if (column >= desc.syscall_counts.len) return;
    desc.syscall_counts[column] +%= 1;
}
