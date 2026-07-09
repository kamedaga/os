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

pub fn pipeSlot(self: anytype, pipe_ref: PipeRef) ?*PipeSlot {
    if (pipe_ref.isNull()) return null;
    const index: usize = @intCast(pipe_ref.index);
    if (index >= max_pipes) return null;
    const slot = &self.pipes[index];
    if (!slot.active or slot.generation != pipe_ref.generation) return null;
    return slot;
}

pub fn pipeSlotConst(self: anytype, pipe_ref: PipeRef) ?*const PipeSlot {
    if (pipe_ref.isNull()) return null;
    const index: usize = @intCast(pipe_ref.index);
    if (index >= max_pipes) return null;
    const slot = &self.pipes[index];
    if (!slot.active or slot.generation != pipe_ref.generation) return null;
    return slot;
}

pub fn createPipe(self: anytype) KernelError!PipeRef {
    var offset: usize = 0;
    while (offset < max_pipes) : (offset += 1) {
        const index = (self.next_pipe_scan + offset) % max_pipes;
        const slot = &self.pipes[index];
        if (slot.active) continue;
        if (slot.generation == 0) slot.generation = 1;
        const generation = slot.generation;
        slot.* = .{
            .active = true,
            .generation = generation,
            .read_refs = 1,
            .write_refs = 1,
        };
        self.next_pipe_scan = (index + 1) % max_pipes;
        return .{ .index = @intCast(index), .generation = generation };
    }
    return KernelError.TableFull;
}

pub fn clearPipeSlot(slot: *PipeSlot) void {
    slot.* = .{ .generation = nextObjectGeneration(slot.generation) };
}

pub fn releasePipeEndpoint(self: anytype, endpoint: PipeEndpointObject) void {
    const slot = self.pipeSlot(endpoint.pipe) orelse return;
    if (endpoint.write) {
        if (slot.write_refs != 0) slot.write_refs -= 1;
    } else {
        if (slot.read_refs != 0) slot.read_refs -= 1;
    }
    if (slot.read_refs == 0 and slot.write_refs == 0) @TypeOf(self.*).clearPipeSlot(slot);
}

pub fn pipeEndpointFromPayload(payload: *const KernelObjectPayload) ?PipeEndpointObject {
    return switch (payload.*) {
        .pipe => |endpoint| endpoint,
        else => null,
    };
}

pub fn pipeEndpointForFd(self: anytype, owner: PrincipalId, fd: Fd) ?PipeEndpointObject {
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    return @TypeOf(self.*).pipeEndpointFromPayload(&slot.payload);
}

pub fn pipeUsed(slot: *const PipeSlot) usize {
    return @intCast(slot.len);
}

pub fn pipeFree(slot: *const PipeSlot) usize {
    return pipe_buffer_bytes - pipeUsed(slot);
}

pub fn pipeReadyEventsForEndpoint(slot: *const PipeSlot, endpoint: PipeEndpointObject) u64 {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    var ready: u64 = 0;
    if (endpoint.write) {
        if (slot.read_refs == 0) {
            ready |= fd_abi.event_error | fd_abi.event_hangup;
        } else if (pipeFree(slot) != 0) {
            ready |= fd_abi.event_writable;
        }
    } else {
        if (slot.len != 0 or slot.write_refs == 0) ready |= fd_abi.event_readable;
        if (slot.write_refs == 0) ready |= fd_abi.event_hangup;
    }
    return ready;
}

pub fn pipeReadyEventsForFd(self: anytype, owner: PrincipalId, fd: Fd) ?u64 {
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    const endpoint = @TypeOf(self.*).pipeEndpointFromPayload(&slot.payload) orelse return null;
    const pipe = self.pipeSlotConst(endpoint.pipe) orelse return null;
    return @TypeOf(self.*).pipeReadyEventsForEndpoint(pipe, endpoint);
}

pub fn pipeReadyEventsForSide(self: anytype, pipe_ref: PipeRef, write_side: bool) ?u64 {
    const pipe = self.pipeSlotConst(pipe_ref) orelse return null;
    return @TypeOf(self.*).pipeReadyEventsForEndpoint(pipe, .{ .pipe = pipe_ref, .write = write_side });
}

pub fn pipeReadBytes(self: anytype, owner: PrincipalId, fd: Fd, out: []u8) PipeIoError!usize {
    const view = self.fdPayloadWithRightsConst(owner, fd, .{ .read = true }) orelse return PipeIoError.InvalidState;
    const endpoint = @TypeOf(self.*).pipeEndpointFromPayload(view.payload) orelse return PipeIoError.InvalidState;
    if (endpoint.write) return PipeIoError.InvalidState;
    if (out.len == 0) return 0;
    const pipe = self.pipeSlot(endpoint.pipe) orelse return PipeIoError.InvalidState;
    if (pipe.len == 0) {
        if (pipe.write_refs == 0) return 0;
        return PipeIoError.NotReady;
    }
    const count = @min(out.len, @as(usize, @intCast(pipe.len)));
    const first = @min(count, pipe_buffer_bytes - @as(usize, pipe.head));
    @memcpy(out[0..first], pipe.data[@as(usize, pipe.head) .. @as(usize, pipe.head) + first]);
    if (first < count) {
        @memcpy(out[first..count], pipe.data[0 .. count - first]);
    }
    pipe.head = @intCast((@as(usize, pipe.head) + count) % pipe_buffer_bytes);
    pipe.len = @intCast(@as(usize, pipe.len) - count);
    return count;
}

pub fn pipeWriteBytes(self: anytype, owner: PrincipalId, fd: Fd, in: []const u8, atomic: bool) PipeIoError!usize {
    const view = self.fdPayloadWithRightsConst(owner, fd, .{ .write = true }) orelse return PipeIoError.InvalidState;
    const endpoint = @TypeOf(self.*).pipeEndpointFromPayload(view.payload) orelse return PipeIoError.InvalidState;
    if (!endpoint.write) return PipeIoError.InvalidState;
    if (in.len == 0) return 0;
    const pipe = self.pipeSlot(endpoint.pipe) orelse return PipeIoError.InvalidState;
    if (pipe.read_refs == 0) return PipeIoError.Closed;
    const free = @TypeOf(self.*).pipeFree(pipe);
    if (free == 0) return PipeIoError.NotReady;
    if (atomic and free < in.len) return PipeIoError.NotReady;
    const count = @min(in.len, free);
    const tail = (@as(usize, pipe.head) + @as(usize, @intCast(pipe.len))) % pipe_buffer_bytes;
    const first = @min(count, pipe_buffer_bytes - tail);
    @memcpy(pipe.data[tail .. tail + first], in[0..first]);
    if (first < count) {
        @memcpy(pipe.data[0 .. count - first], in[first..count]);
    }
    pipe.len = @intCast(@as(usize, pipe.len) + count);
    return count;
}

pub fn pipeRights(readable: bool) FdRights {
    return .{
        .inspect = true,
        .dup = true,
        .transfer = true,
        .wait = true,
        .poll = true,
        .set_flags = true,
        .close = true,
        .read = readable,
        .write = !readable,
    };
}

pub fn createPipePairFds(
    self: anytype,
    owner: PrincipalId,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!PipePair {
    try self.requireActiveProcess(owner);
    const pipe_ref = try self.createPipe();
    const read_object = self.createKernelObject(.pipe, .{ .pipe = .{ .pipe = pipe_ref, .write = false } }) catch |err| {
        if (self.pipeSlot(pipe_ref)) |slot| @TypeOf(self.*).clearPipeSlot(slot);
        return err;
    };
    errdefer if (self.kernelObjectSlot(read_object)) |slot| self.clearKernelObjectSlot(slot);
    const write_object = self.createKernelObject(.pipe, .{ .pipe = .{ .pipe = pipe_ref, .write = true } }) catch |err| {
        if (self.pipeSlot(pipe_ref)) |slot| {
            if (slot.write_refs != 0) slot.write_refs -= 1;
        }
        return err;
    };
    errdefer if (self.kernelObjectSlot(write_object)) |slot| self.clearKernelObjectSlot(slot);
    const read_fd = try self.installFd(owner, read_object, @TypeOf(self.*).pipeRights(true), flags, min_fd);
    errdefer self.closeFd(owner, read_fd) catch {};
    const write_fd = try self.installFd(owner, write_object, @TypeOf(self.*).pipeRights(false), flags, min_fd);
    return .{ .read = read_fd, .write = write_fd };
}

pub fn pipeWaitListForEndpoint(self: anytype, endpoint: PipeEndpointObject) ?*IpcWaitList {
    const pipe = self.pipeSlot(endpoint.pipe) orelse return null;
    return &pipe.waiters[if (endpoint.write) 1 else 0];
}

pub fn registerPipeWaiterForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    requested_events: u64,
    pollfd_va: u64,
    thread_index: usize,
    thread_generation: u32,
) KernelError!bool {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    if (thread_index > std.math.maxInt(u32)) return KernelError.InvalidState;
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.poll and !entry.rights.wait) return KernelError.InvalidState;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return KernelError.InvalidState;
    const endpoint = @TypeOf(self.*).pipeEndpointFromPayload(&slot.payload) orelse return false;
    const matched_events = if (endpoint.write)
        requested_events & (fd_abi.event_writable | fd_abi.event_error | fd_abi.event_hangup)
    else
        requested_events & (fd_abi.event_readable | fd_abi.event_error | fd_abi.event_hangup);
    if (matched_events == 0) return true;
    const key = IpcWaitKey{
        .kind = .pipe,
        .index = endpoint.pipe.index,
        .generation = endpoint.pipe.generation,
        .side = if (endpoint.write) 1 else 0,
    };
    const waiters = self.pipeWaitListForEndpoint(endpoint) orelse return KernelError.InvalidState;
    try waiters.register(key, owner, pollfd_va, 0, 0, 0, requested_events, thread_index, thread_generation);
    return true;
}

pub fn unregisterPipeWaiterForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    requested_events: u64,
    thread_index: usize,
    thread_generation: u32,
) void {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    const entry = self.fdEntryConst(owner, fd) orelse return;
    if (!entry.rights.poll and !entry.rights.wait) return;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return;
    const endpoint = @TypeOf(self.*).pipeEndpointFromPayload(&slot.payload) orelse return;
    const matched_events = if (endpoint.write)
        requested_events & (fd_abi.event_writable | fd_abi.event_error | fd_abi.event_hangup)
    else
        requested_events & (fd_abi.event_readable | fd_abi.event_error | fd_abi.event_hangup);
    if (matched_events == 0) return;
    const waiters = self.pipeWaitListForEndpoint(endpoint) orelse return;
    waiters.unregister(thread_index, thread_generation);
}

pub fn takePipeWaiters(
    self: anytype,
    pipe_ref: PipeRef,
    write_side: bool,
    ready_events: u64,
    out: []ThreadWakeTarget,
) usize {
    const pipe = self.pipeSlot(pipe_ref) orelse return 0;
    return pipe.waiters[if (write_side) 1 else 0].takeEvents(ready_events, out);
}
