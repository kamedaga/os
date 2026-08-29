const std = @import("std");
const builtin = @import("builtin");
const vtd = @import("../vtd.zig");
const x86_platform = @import("../arch/x86_64/platform.zig");
const types = @import("types.zig");
const table = @import("table.zig");
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
const ProcessDescriptorTable = table.StaticPlusExtra(ProcessDescriptor, process_count);
const EndpointRuntimeTable = table.StaticPlusExtra(EndpointTable, process_count);
const FdRuntimeTable = table.StaticPlusExtra(FdTable, process_count);
const VmaRuntimeTable = table.StaticPlusExtra(VmaTable, process_count);
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
const kernelStaticStorageEndAddr = types.kernelStaticStorageEndAddr;
const runtimeStorageBytes = types.runtimeStorageBytes;
const initRuntimeStorage = types.initRuntimeStorage;

pub fn allocKernelSlice(comptime T: type, free_list: *FreePageList, count: usize) ?[]T {
    if (count == 0) return null;
    const bytes = @sizeOf(T) * count;
    const page_count = (bytes + 4095) / 4096;
    const paddr = free_list.popContiguousAtOrAbove(page_count, 0) catch return null;
    const raw: [*]u8 = @ptrFromInt(paddr);
    @memset(raw[0 .. page_count * 4096], 0);
    const ptr: [*]T = @ptrCast(@alignCast(raw));
    return ptr[0..count];
}

pub fn processPrincipal(index: usize) PrincipalId {
    return processPrincipalFromIndex(index) orelse unreachable;
}

pub fn isActiveProcess(self: anytype, principal: PrincipalId) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    return (self.processDescriptorSlotConst(index) orelse return false).active;
}

pub fn processDescriptor(self: anytype, principal: PrincipalId) ?*const ProcessDescriptor {
    const index = processIndexFromPrincipal(principal) orelse return null;
    const desc = self.processDescriptorSlotConst(index) orelse return null;
    if (!desc.active) return null;
    return desc;
}

pub fn processStatus(self: anytype, principal: PrincipalId) ProcessStatus {
    const index = processIndexFromPrincipal(principal) orelse return .{};
    const desc = (self.processDescriptorSlotConst(index) orelse return .{}).*;
    return .{
        .active = desc.active,
        .faulted = desc.faulted,
        .fault_vector = desc.fault_vector,
    };
}

pub fn isBootstrapOwner(self: anytype, principal: PrincipalId) bool {
    const desc = self.processDescriptor(principal) orelse return false;
    return desc.bootstrap_owner;
}

pub fn hasActivePrincipal(self: anytype, principal: PrincipalId) bool {
    if (processIndexFromPrincipal(principal)) |index| {
        return (self.processDescriptorSlotConst(index) orelse return false).active;
    }
    return principal == .Device0;
}

pub fn requireActiveProcess(self: anytype, principal: PrincipalId) KernelError!void {
    const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
    if (!(self.processDescriptorSlotConst(index) orelse return KernelError.InvalidState).active) return KernelError.InvalidState;
}

pub fn requireActivePrincipal(self: anytype, principal: PrincipalId) KernelError!void {
    if (!self.hasActivePrincipal(principal)) return KernelError.InvalidState;
}

pub fn principalStorageIndex(self: anytype, principal: PrincipalId) usize {
    if (processIndexFromPrincipal(principal)) |index| {
        std.debug.assert((self.processDescriptorSlotConst(index) orelse unreachable).active);
        return index;
    }
    std.debug.assert(principal == .Device0);
    return process_count;
}

pub fn extraIndex(index: usize) ?usize {
    return ProcessDescriptorTable.extraIndex(index);
}

pub fn processDescriptorSlot(self: anytype, index: usize) ?*ProcessDescriptor {
    if (index >= self.process_capacity) return null;
    if (@TypeOf(self.*).extraIndex(index)) |extra| return &self.process_descriptors_extra[extra];
    return &self.process_descriptors[index];
}

pub fn processDescriptorSlotConst(self: anytype, index: usize) ?*const ProcessDescriptor {
    if (index >= self.process_capacity) return null;
    if (@TypeOf(self.*).extraIndex(index)) |extra| return &self.process_descriptors_extra[extra];
    return &self.process_descriptors[index];
}

pub fn endpointTableForProcessIndex(self: anytype, index: usize) *EndpointTable {
    if (@TypeOf(self.*).extraIndex(index)) |extra| return &self.endpoint_tables_extra[extra];
    return &self.endpoint_tables[index];
}

pub fn endpointTableForProcessIndexConst(self: anytype, index: usize) *const EndpointTable {
    if (@TypeOf(self.*).extraIndex(index)) |extra| return &self.endpoint_tables_extra[extra];
    return &self.endpoint_tables[index];
}

pub fn fdTableForProcessIndex(self: anytype, index: usize) ?*FdTable {
    if (index >= self.process_capacity) return null;
    if (@TypeOf(self.*).extraIndex(index)) |extra| return &self.fd_tables_extra[extra];
    return &self.fd_tables[index];
}

pub fn fdTableForProcessIndexConst(self: anytype, index: usize) ?*const FdTable {
    if (index >= self.process_capacity) return null;
    if (@TypeOf(self.*).extraIndex(index)) |extra| return &self.fd_tables_extra[extra];
    return &self.fd_tables[index];
}

pub fn vmaTableForProcessIndex(self: anytype, index: usize) ?*VmaTable {
    if (index >= self.process_capacity) return null;
    if (@TypeOf(self.*).extraIndex(index)) |extra| return &self.vma_tables_extra[extra];
    return &self.vma_tables[index];
}

pub fn vmaTableForProcessIndexConst(self: anytype, index: usize) ?*const VmaTable {
    if (index >= self.process_capacity) return null;
    if (@TypeOf(self.*).extraIndex(index)) |extra| return &self.vma_tables_extra[extra];
    return &self.vma_tables[index];
}

pub fn getFdTable(self: anytype, principal: PrincipalId) ?*FdTable {
    const index = processIndexFromPrincipal(principal) orelse return null;
    return self.fdTableForProcessIndex(index);
}

pub fn getFdTableConst(self: anytype, principal: PrincipalId) ?*const FdTable {
    const index = processIndexFromPrincipal(principal) orelse return null;
    return self.fdTableForProcessIndexConst(index);
}

pub fn inheritFdsForProcessCreate(self: anytype, from: PrincipalId, to: PrincipalId) KernelError!void {
    if (from == to) return KernelError.InvalidState;
    const source_table = try self.fdTableForActiveProcessConst(from);
    const dest_table = try self.fdTableForActiveProcess(to);
    var fd_index: usize = 0;
    while (fd_index < fd_table_entries) : (fd_index += 1) {
        const source = source_table.entries[fd_index];
        if (source.object.isNull() or !source.flags.inherit or source.flags.private) continue;
        if (self.kernelObjectIsPinnedUserObject(source.object)) continue;
        if (!dest_table.entries[fd_index].isEmpty()) return KernelError.InvalidState;
        try self.retainKernelObject(source.object);
        dest_table.entries[fd_index] = .{
            .object = source.object,
            .rights = fdRightsFromBits(fdRightsToBits(source.rights)),
            .flags = fdFlagsFromBits(fdFlagsToBits(source.flags)),
            .offset = source.offset,
        };
    }
}

pub fn cloneFdTableForFork(self: anytype, from: PrincipalId, to: PrincipalId) KernelError!void {
    if (from == to) return KernelError.InvalidState;
    const source_table = try self.fdTableForActiveProcessConst(from);
    const dest_table = try self.fdTableForActiveProcess(to);
    var fd_index: usize = 0;
    errdefer {
        var release_index: usize = 0;
        while (release_index < fd_table_entries) : (release_index += 1) {
            const object_ref = dest_table.entries[release_index].object;
            if (object_ref.isNull()) continue;
            dest_table.entries[release_index] = .{};
            self.releaseKernelObject(object_ref);
        }
    }
    while (fd_index < fd_table_entries) : (fd_index += 1) {
        const source = source_table.entries[fd_index];
        if (source.object.isNull()) continue;
        if (!dest_table.entries[fd_index].isEmpty()) return KernelError.InvalidState;
        try self.retainKernelObject(source.object);
        dest_table.entries[fd_index] = .{
            .object = source.object,
            .rights = fdRightsFromBits(fdRightsToBits(source.rights)),
            .flags = fdFlagsFromBits(fdFlagsToBits(source.flags)),
            .offset = source.offset,
        };
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
    while (i < self.fd_objects.len) : (i += 1) {
        const slot = &self.fd_objects[i];
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
    while (i < self.fd_objects.len) : (i += 1) {
        const slot = &self.fd_objects[i];
        if (slot.kind != .thread) continue;
        var thread = @TypeOf(self.*).threadFromPayload(&slot.payload) orelse continue;
        if (thread.owner_principal_raw != principal_raw) continue;
        if (!state.isTerminal() and thread.state.isTerminal()) continue;
        thread.state = state;
        thread.exit_code = exit_code;
        slot.payload = .{ .thread = thread };
    }
}

pub fn markThreadObjectsExitedBySlot(self: anytype, thread_index: usize, thread_generation: u32, state: TaskObjectState, exit_code: u32) void {
    var i: usize = 0;
    while (i < self.fd_objects.len) : (i += 1) {
        const slot = &self.fd_objects[i];
        if (slot.kind != .thread) continue;
        var thread = @TypeOf(self.*).threadFromPayload(&slot.payload) orelse continue;
        if (thread.thread_index != thread_index or thread.thread_generation != thread_generation) continue;
        thread.state = state;
        thread.exit_code = exit_code;
        slot.payload = .{ .thread = thread };
    }
}

pub fn nextProcessCapacity(self: anytype, required: usize) ?usize {
    if (required > max_process_slots) return null;
    var capacity = self.process_capacity;
    if (capacity == 0) capacity = process_count;
    while (capacity < required) {
        const doubled = capacity * 2;
        capacity = if (doubled > max_process_slots) max_process_slots else doubled;
        if (capacity < required and capacity == max_process_slots) return null;
    }
    return capacity;
}

pub fn ensureProcessCapacity(self: anytype, required: usize, free_list: *FreePageList) bool {
    if (required <= self.process_capacity) return true;
    const capacity = self.nextProcessCapacity(required) orelse return false;
    const extra_count = ProcessDescriptorTable.extraCount(capacity);
    const old_extra_count = ProcessDescriptorTable.extraCount(self.process_capacity);

    const new_desc = @TypeOf(self.*).allocKernelSlice(ProcessDescriptor, free_list, extra_count) orelse return false;
    const new_endpoint = @TypeOf(self.*).allocKernelSlice(EndpointTable, free_list, extra_count) orelse return false;
    const new_fd = @TypeOf(self.*).allocKernelSlice(FdTable, free_list, extra_count) orelse return false;
    const new_vma = @TypeOf(self.*).allocKernelSlice(VmaTable, free_list, extra_count) orelse return false;

    ProcessDescriptorTable.copyExistingExtra(new_desc, self.process_descriptors_extra, self.process_capacity);
    EndpointRuntimeTable.copyExistingExtra(new_endpoint, self.endpoint_tables_extra, self.process_capacity);
    FdRuntimeTable.copyExistingExtra(new_fd, self.fd_tables_extra, self.process_capacity);
    VmaRuntimeTable.copyExistingExtra(new_vma, self.vma_tables_extra, self.process_capacity);

    var i = old_extra_count;
    while (i < extra_count) : (i += 1) {
        new_desc[i] = .{};
        new_endpoint[i] = .{};
        new_fd[i] = .{};
        new_vma[i] = .{};
    }

    self.process_descriptors_extra = new_desc;
    self.endpoint_tables_extra = new_endpoint;
    self.fd_tables_extra = new_fd;
    self.vma_tables_extra = new_vma;
    self.process_capacity = capacity;
    return true;
}

pub fn clearPrincipalTablesForReuse(self: anytype, index: usize) void {
    self.resetProcessRuntimeTables(index);
}

fn nextProcessGeneration(generation: u32) u32 {
    const next = generation +% 1;
    return if (next == 0) 1 else next;
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
        const desc = self.processDescriptorSlot(i) orelse continue;
        if (desc.active) continue;
        const principal = @TypeOf(self.*).processPrincipal(i);
        if (reusable) |is_reusable| {
            if (!is_reusable(principal)) continue;
        }
        const generation = nextProcessGeneration(desc.generation);
        self.clearPrincipalTablesForReuse(i);
        desc.* = .{
            .active = true,
            .principal = principal,
            .generation = generation,
            .label = label,
        };
        self.active_process_count += 1;
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

pub fn ensureProcessDescriptor(self: anytype, principal: PrincipalId, label: []const u8) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    const desc = self.processDescriptorSlot(index) orelse return false;
    if (desc.active) {
        desc.label = label;
        return true;
    }
    const generation = nextProcessGeneration(desc.generation);
    self.clearPrincipalTablesForReuse(index);
    desc.* = .{
        .active = true,
        .principal = principal,
        .generation = generation,
        .label = label,
    };
    self.active_process_count += 1;
    if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .ensure);
    return true;
}

pub fn setBootstrapOwner(self: anytype, principal: PrincipalId, enabled: bool) KernelError!void {
    const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
    const desc = self.processDescriptorSlot(index) orelse return KernelError.InvalidState;
    if (!desc.active) return KernelError.InvalidState;
    desc.bootstrap_owner = enabled;
}

pub fn markProcessFaulted(self: anytype, principal: PrincipalId, fault_vector: u8) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    const desc = self.processDescriptorSlot(index) orelse return false;
    if (!desc.active) return false;
    self.releaseFdTableForProcessIndex(index);
    desc.active = false;
    desc.bootstrap_owner = false;
    desc.faulted = true;
    desc.fault_vector = fault_vector;
    if (self.active_process_count > 0) self.active_process_count -= 1;
    if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .fault);
    return true;
}

pub fn markProcessExited(self: anytype, principal: PrincipalId) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    const desc = self.processDescriptorSlot(index) orelse return false;
    if (!desc.active) return false;
    self.releaseFdTableForProcessIndex(index);
    desc.active = false;
    desc.bootstrap_owner = false;
    desc.faulted = false;
    desc.fault_vector = 0;
    if (self.active_process_count > 0) self.active_process_count -= 1;
    if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .exit);
    return true;
}

pub fn removeProcessDescriptor(self: anytype, principal: PrincipalId) bool {
    const index = processIndexFromPrincipal(principal) orelse return false;
    const desc = self.processDescriptorSlot(index) orelse return false;
    if (!desc.active) return false;
    self.releaseFdTableForProcessIndex(index);
    const generation = desc.generation;
    desc.* = .{ .generation = generation };
    if (self.active_process_count > 0) self.active_process_count -= 1;
    if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .remove);
    return true;
}

pub fn clearPrincipalState(self: anytype) void {
    var i: usize = 0;
    while (i < self.process_capacity) : (i += 1) {
        self.resetProcessRuntimeTables(i);
    }
    self.endpoint_tables[process_count] = .{};
    resetVmoBackingPageStore();
    self.published_service_endpoints = .{};
    self.resetKernelObjectTable();
    self.resetNativeVmoTable();
    self.resetNativeIpcObjects();
    i = 0;
    while (i < self.process_capacity) : (i += 1) {
        (self.processDescriptorSlot(i) orelse break).* = .{};
    }
    self.active_process_count = 0;
}

pub fn initPrincipalState(self: anytype) void {
    self.clearPrincipalState();
    var i: usize = 0;
    while (i < initial_process_count) : (i += 1) {
        const principal = @TypeOf(self.*).processPrincipal(i);
        (self.processDescriptorSlot(i) orelse unreachable).* = .{
            .active = true,
            .principal = principal,
            .generation = 1,
            .label = principalLabel(principal),
        };
        self.active_process_count += 1;
    }
}

pub fn initDynamicPrincipalState(self: anytype) void {
    self.clearPrincipalState();
}

pub fn getEndpointTable(self: anytype, principal: PrincipalId) *EndpointTable {
    if (processIndexFromPrincipal(principal)) |index| return self.endpointTableForProcessIndex(index);
    return &self.endpoint_tables[self.principalStorageIndex(principal)];
}

pub fn getEndpointTableConst(self: anytype, principal: PrincipalId) *const EndpointTable {
    if (processIndexFromPrincipal(principal)) |index| return self.endpointTableForProcessIndexConst(index);
    return &self.endpoint_tables[self.principalStorageIndex(principal)];
}

pub fn bumpEndpointGeneration(self: anytype) void {
    self.endpoint_generation +%= 1;
}

pub fn installEndpoint(
    self: anytype,
    owner: PrincipalId,
    endpoint_id: u64,
    target: PrincipalId,
) KernelError!void {
    try self.requireActiveProcess(owner);
    try self.requireActiveProcess(target);
    try self.getEndpointTable(owner).add(.{
        .endpoint_id = endpoint_id,
        .target = target,
    });
    self.bumpEndpointGeneration();
}

pub fn installServiceEndpointForActiveProcesses(
    self: anytype,
    endpoint_id: u64,
    target: PrincipalId,
) KernelError!void {
    try self.requireActiveProcess(target);
    var i: usize = 0;
    while (i < self.process_capacity) : (i += 1) {
        const desc = (self.processDescriptorSlotConst(i) orelse continue).*;
        if (!desc.active) continue;
        if (desc.principal == target) continue;
        try self.installEndpoint(desc.principal, endpoint_id, target);
    }
}

pub fn publishServiceEndpoint(
    self: anytype,
    endpoint_id: u64,
    target: PrincipalId,
) KernelError!void {
    try self.requireActiveProcess(target);
    try self.published_service_endpoints.publish(.{
        .endpoint_id = endpoint_id,
        .target = target,
    });
    self.bumpEndpointGeneration();
}

pub fn unpublishServiceEndpointsForTarget(self: anytype, target: PrincipalId) bool {
    const removed = self.published_service_endpoints.removeTarget(target);
    if (removed) {
        self.bumpEndpointGeneration();
    }
    return removed;
}

pub fn endpointTargetFor(self: anytype, owner: PrincipalId, endpoint_id: u64) ?PrincipalId {
    if (!self.hasActivePrincipal(owner)) return null;
    return self.endpointTargetForKnownActiveOwner(owner, endpoint_id);
}

pub fn endpointTargetForKnownActiveOwner(self: anytype, owner: PrincipalId, endpoint_id: u64) ?PrincipalId {
    if (self.getEndpointTableConst(owner).find(endpoint_id)) |ep| {
        if (!self.hasActivePrincipal(ep.target)) return null;
        return ep.target;
    }
    if (self.published_service_endpoints.find(endpoint_id)) |ep| {
        if (!self.hasActivePrincipal(ep.target)) return null;
        return ep.target;
    }
    return null;
}
