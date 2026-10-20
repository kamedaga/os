const std = @import("std");
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
const FdWaitRegistrationRef = types.FdWaitRegistrationRef;
const FdWaitGroupRef = types.FdWaitGroupRef;
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
const vmoBackingPageStorePaddr = types.vmoBackingPageStorePaddr;
const setVmoBackingPageStorePaddr = types.setVmoBackingPageStorePaddr;
const freeVmoBackingPageStore = types.freeVmoBackingPageStore;
const resetVmoBackingPageStore = types.resetVmoBackingPageStore;
const kernelStaticStorageEndAddr = types.kernelStaticStorageEndAddr;
const runtimeStorageBytes = types.runtimeStorageBytes;
const initRuntimeStorage = types.initRuntimeStorage;

pub fn pipeSlot(self: anytype, pipe_ref: PipeRef) ?*PipeSlot {
    if (pipe_ref.isNull()) return null;
    return self.pipes.resolve(.{ .index = pipe_ref.index, .generation = pipe_ref.generation });
}

pub fn pipeSlotConst(self: anytype, pipe_ref: PipeRef) ?*const PipeSlot {
    if (pipe_ref.isNull()) return null;
    return self.pipes.resolve(.{ .index = pipe_ref.index, .generation = pipe_ref.generation });
}

pub fn createPipe(self: anytype) KernelError!PipeRef {
    const allocation = try self.allocatePool(&self.pipes, null, std.math.maxInt(u32));
    const slot = allocation.value;
    slot.active = true;
    slot.read_refs = 1;
    slot.write_refs = 1;
    return .{ .index = allocation.ref.index, .generation = allocation.ref.generation };
}

pub fn clearPipeSlot(self: anytype, slot: *PipeSlot) void {
    for (&slot.waiters) |*list| list.waiters.deinitInto(&self.pipes.body_owner);
    self.pipes.remove(slot, null);
}

pub fn releasePipeEndpoint(self: anytype, endpoint: PipeEndpointObject) void {
    const slot = self.pipeSlot(endpoint.pipe) orelse return;
    if (endpoint.write) {
        if (slot.write_refs != 0) slot.write_refs -= 1;
    } else {
        if (slot.read_refs != 0) slot.read_refs -= 1;
    }
    if (slot.read_refs == 0 and slot.write_refs == 0) self.clearPipeSlot(slot);
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

pub fn pipeWritableReady(slot: *const PipeSlot) bool {
    return pipeFree(slot) != 0;
}

pub fn pipeWritableReadyForBytes(slot: *const PipeSlot, min_write_bytes: u64) bool {
    const required = if (min_write_bytes == 0) 1 else min_write_bytes;
    return @as(u64, @intCast(pipeFree(slot))) >= required;
}

pub fn pipeReadyEventsForEndpoint(slot: *const PipeSlot, endpoint: PipeEndpointObject) u64 {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    var ready: u64 = 0;
    if (endpoint.write) {
        if (slot.read_refs == 0) {
            ready |= fd_abi.event_error | fd_abi.event_hangup;
        } else if (pipeWritableReady(slot)) {
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
        if (self.pipeSlot(pipe_ref)) |slot| self.clearPipeSlot(slot);
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
    min_write_bytes: u64,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
    group: FdWaitGroupRef,
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
    if (matched_events == 0) return false;
    const key = IpcWaitKey{
        .kind = .pipe,
        .index = endpoint.pipe.index,
        .generation = endpoint.pipe.generation,
        .side = if (endpoint.write) 1 else 0,
    };
    const waiters = self.pipeWaitListForEndpoint(endpoint) orelse return KernelError.InvalidState;
    waiters.allocator = try self.waitPoolAllocator();
    const registration = waiters.register(
        key,
        owner,
        pollfd_va,
        .{},
        0,
        0,
        requested_events,
        min_write_bytes,
        thread_index,
        thread_generation,
        wait_token,
        group,
    ) catch |err| {
        return err;
    };
    self.linkFdWaitRegistration(group, registration) catch |err| {
        if (registration.slot < waiters.waiters.capacity) {
            const waiter = waiters.waiters.resolve(.{ .index = registration.slot, .generation = registration.slot_generation }).?;
            if (waiter.active and waiter.registration.matches(registration) and
                waiter.binding.group.matches(group)) waiter.list.?.removeWaiter(waiter);
        }
        return err;
    };
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
    const writable_bytes: u64 = if (write_side) @intCast(@TypeOf(self.*).pipeFree(pipe)) else 0;
    return pipe.waiters[if (write_side) 1 else 0].takeEventsWithWritableBytes(ready_events, writable_bytes, out);
}
