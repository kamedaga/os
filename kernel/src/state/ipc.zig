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

const IpcChannelPair = struct {
    a: Fd,
    b: Fd,
};

pub fn releaseIpcMessage(self: anytype, msg: *IpcMessage) void {
    var i: usize = 0;
    while (i < msg.fd_count and i < max_ipc_message_fds) : (i += 1) {
        const object_ref = msg.fds[i].object;
        if (!object_ref.isNull()) self.releaseKernelObject(object_ref);
        msg.fds[i] = .{};
    }
    msg.* = .{};
}

pub fn releaseIpcMessageWithFreeList(self: anytype, msg: *IpcMessage, free_list: *FreePageList) void {
    var i: usize = 0;
    while (i < msg.fd_count and i < max_ipc_message_fds) : (i += 1) {
        const object_ref = msg.fds[i].object;
        if (!object_ref.isNull()) self.releaseKernelObjectWithFreeList(object_ref, free_list);
        msg.fds[i] = .{};
    }
    msg.* = .{};
}

pub fn clearIpcQueue(self: anytype, queue: *IpcQueue) void {
    while (queue.pop()) |msg_value| {
        var msg = msg_value;
        self.releaseIpcMessage(&msg);
    }
    queue.* = .{};
}

pub fn clearIpcQueueWithFreeList(self: anytype, queue: *IpcQueue, free_list: *FreePageList) void {
    while (queue.pop()) |msg_value| {
        var msg = msg_value;
        self.releaseIpcMessageWithFreeList(&msg, free_list);
    }
    queue.* = .{};
}

pub fn ipcEndpointSlot(self: anytype, endpoint_ref: IpcEndpointRef) ?*IpcEndpointSlot {
    if (endpoint_ref.isNull()) return null;
    const index: usize = @intCast(endpoint_ref.index);
    if (index >= max_ipc_endpoints) return null;
    const slot = &self.ipc_endpoints[index];
    if (!slot.active or slot.generation != endpoint_ref.generation) return null;
    return slot;
}

pub fn ipcEndpointSlotConst(self: anytype, endpoint_ref: IpcEndpointRef) ?*const IpcEndpointSlot {
    if (endpoint_ref.isNull()) return null;
    const index: usize = @intCast(endpoint_ref.index);
    if (index >= max_ipc_endpoints) return null;
    const slot = &self.ipc_endpoints[index];
    if (!slot.active or slot.generation != endpoint_ref.generation) return null;
    return slot;
}

pub fn clearIpcEndpointSlot(self: anytype, endpoint_ref: IpcEndpointRef) void {
    const slot = self.ipcEndpointSlot(endpoint_ref) orelse return;
    self.clearIpcQueue(&slot.queue);
    slot.* = .{ .generation = @TypeOf(self.*).nextObjectGeneration(slot.generation) };
}

pub fn clearIpcEndpointSlotWithFreeList(
    self: anytype,
    endpoint_ref: IpcEndpointRef,
    free_list: *FreePageList,
) void {
    const slot = self.ipcEndpointSlot(endpoint_ref) orelse return;
    self.clearIpcQueueWithFreeList(&slot.queue, free_list);
    slot.* = .{ .generation = @TypeOf(self.*).nextObjectGeneration(slot.generation) };
}

pub fn createIpcEndpoint(self: anytype) KernelError!IpcEndpointRef {
    var offset: usize = 0;
    while (offset < max_ipc_endpoints) : (offset += 1) {
        const index = (self.next_ipc_endpoint_scan + offset) % max_ipc_endpoints;
        const slot = &self.ipc_endpoints[index];
        if (slot.active) continue;
        if (slot.generation == 0) slot.generation = 1;
        slot.active = true;
        slot.queue = .{};
        self.next_ipc_endpoint_scan = (index + 1) % max_ipc_endpoints;
        return .{ .index = @intCast(index), .generation = slot.generation };
    }
    return KernelError.TableFull;
}

pub fn ipcChannelSlot(self: anytype, channel_ref: IpcChannelRef) ?*IpcChannelSlot {
    if (channel_ref.isNull()) return null;
    const index: usize = @intCast(channel_ref.index);
    if (index >= max_ipc_channels) return null;
    const slot = &self.ipc_channels[index];
    if (!slot.active or slot.generation != channel_ref.generation) return null;
    return slot;
}

pub fn ipcChannelSlotConst(self: anytype, channel_ref: IpcChannelRef) ?*const IpcChannelSlot {
    if (channel_ref.isNull()) return null;
    const index: usize = @intCast(channel_ref.index);
    if (index >= max_ipc_channels) return null;
    const slot = &self.ipc_channels[index];
    if (!slot.active or slot.generation != channel_ref.generation) return null;
    return slot;
}

pub fn createIpcChannel(self: anytype) KernelError!IpcChannelRef {
    var offset: usize = 0;
    while (offset < max_ipc_channels) : (offset += 1) {
        const index = (self.next_ipc_channel_scan + offset) % max_ipc_channels;
        const slot = &self.ipc_channels[index];
        if (slot.active) continue;
        if (slot.generation == 0) slot.generation = 1;
        slot.active = true;
        slot.ref_count = 2;
        slot.queues = .{ .{}, .{} };
        self.next_ipc_channel_scan = (index + 1) % max_ipc_channels;
        return .{ .index = @intCast(index), .generation = slot.generation };
    }
    return KernelError.TableFull;
}

pub fn releaseIpcChannelHandle(self: anytype, handle: IpcChannelHandle) void {
    const slot = self.ipcChannelSlot(handle.channel) orelse return;
    if (slot.ref_count == 0) return;
    slot.ref_count -= 1;
    if (slot.ref_count != 0) return;
    self.clearIpcQueue(&slot.queues[0]);
    self.clearIpcQueue(&slot.queues[1]);
    slot.* = .{ .generation = @TypeOf(self.*).nextObjectGeneration(slot.generation) };
}

pub fn releaseIpcChannelHandleWithFreeList(
    self: anytype,
    handle: IpcChannelHandle,
    free_list: *FreePageList,
) void {
    const slot = self.ipcChannelSlot(handle.channel) orelse return;
    if (slot.ref_count == 0) return;
    slot.ref_count -= 1;
    if (slot.ref_count != 0) return;
    self.clearIpcQueueWithFreeList(&slot.queues[0], free_list);
    self.clearIpcQueueWithFreeList(&slot.queues[1], free_list);
    slot.* = .{ .generation = @TypeOf(self.*).nextObjectGeneration(slot.generation) };
}

pub fn ipcReplySlot(self: anytype, reply_ref: IpcReplyRef) ?*IpcReplySlot {
    if (reply_ref.isNull()) return null;
    const index: usize = @intCast(reply_ref.index);
    if (index >= max_ipc_replies) return null;
    const slot = &self.ipc_replies[index];
    if (!slot.active or slot.generation != reply_ref.generation) return null;
    return slot;
}

pub fn ipcReplySlotConst(self: anytype, reply_ref: IpcReplyRef) ?*const IpcReplySlot {
    if (reply_ref.isNull()) return null;
    const index: usize = @intCast(reply_ref.index);
    if (index >= max_ipc_replies) return null;
    const slot = &self.ipc_replies[index];
    if (!slot.active or slot.generation != reply_ref.generation) return null;
    return slot;
}

pub fn clearIpcReplySlot(self: anytype, reply_ref: IpcReplyRef) void {
    const slot = self.ipcReplySlot(reply_ref) orelse return;
    self.clearIpcQueue(&slot.queue);
    slot.* = .{ .generation = @TypeOf(self.*).nextObjectGeneration(slot.generation) };
}

pub fn clearIpcReplySlotWithFreeList(
    self: anytype,
    reply_ref: IpcReplyRef,
    free_list: *FreePageList,
) void {
    const slot = self.ipcReplySlot(reply_ref) orelse return;
    self.clearIpcQueueWithFreeList(&slot.queue, free_list);
    slot.* = .{ .generation = @TypeOf(self.*).nextObjectGeneration(slot.generation) };
}

pub fn createIpcReply(self: anytype) KernelError!IpcReplyRef {
    var offset: usize = 0;
    while (offset < max_ipc_replies) : (offset += 1) {
        const index = (self.next_ipc_reply_scan + offset) % max_ipc_replies;
        const slot = &self.ipc_replies[index];
        if (slot.active) continue;
        if (slot.generation == 0) slot.generation = 1;
        slot.active = true;
        slot.sent = false;
        slot.queue = .{};
        self.next_ipc_reply_scan = (index + 1) % max_ipc_replies;
        return .{ .index = @intCast(index), .generation = slot.generation };
    }
    return KernelError.TableFull;
}

pub fn resetNativeIpcObjects(self: anytype) void {
    for (self.ipc_endpoints[0..]) |*slot| {
        if (slot.active) self.clearIpcQueue(&slot.queue);
        slot.* = .{};
    }
    for (self.ipc_channels[0..]) |*slot| {
        if (slot.active) {
            self.clearIpcQueue(&slot.queues[0]);
            self.clearIpcQueue(&slot.queues[1]);
        }
        slot.* = .{};
    }
    for (self.ipc_replies[0..]) |*slot| {
        if (slot.active) self.clearIpcQueue(&slot.queue);
        slot.* = .{};
    }
    self.next_ipc_endpoint_scan = 0;
    self.next_ipc_channel_scan = 0;
    self.next_ipc_reply_scan = 0;
}

pub fn endpointRights() FdRights {
    return .{
        .inspect = true,
        .dup = true,
        .transfer = true,
        .wait = true,
        .poll = true,
        .set_flags = true,
        .close = true,
        .send = true,
        .recv = true,
        .call = true,
    };
}

pub fn replyReceiveRights() FdRights {
    return .{
        .inspect = true,
        .wait = true,
        .poll = true,
        .close = true,
        .recv = true,
    };
}

pub fn replySendRights() FdRights {
    return .{
        .inspect = true,
        .close = true,
        .send = true,
    };
}

pub fn createIpcEndpointFd(
    self: anytype,
    owner: PrincipalId,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!Fd {
    try self.requireActiveProcess(owner);
    const endpoint_ref = try self.createIpcEndpoint();
    const object_ref = self.createKernelObject(.endpoint, .{ .endpoint = endpoint_ref }) catch |err| {
        self.clearIpcEndpointSlot(endpoint_ref);
        return err;
    };
    return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
        if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
        return err;
    };
}

pub fn createIpcChannelPairFds(
    self: anytype,
    owner: PrincipalId,
    rights: FdRights,
    flags: FdFlags,
    min_fd: Fd,
) KernelError!IpcChannelPair {
    try self.requireActiveProcess(owner);
    const channel_ref = try self.createIpcChannel();
    const object_a = self.createKernelObject(.channel, .{ .channel = .{ .channel = channel_ref, .side = 0 } }) catch |err| {
        self.releaseIpcChannelHandle(.{ .channel = channel_ref, .side = 0 });
        self.releaseIpcChannelHandle(.{ .channel = channel_ref, .side = 1 });
        return err;
    };
    errdefer if (self.kernelObjectSlot(object_a)) |slot| self.clearKernelObjectSlot(slot);
    const object_b = self.createKernelObject(.channel, .{ .channel = .{ .channel = channel_ref, .side = 1 } }) catch |err| {
        return err;
    };
    errdefer if (self.kernelObjectSlot(object_b)) |slot| self.clearKernelObjectSlot(slot);
    const fd_a = try self.installFd(owner, object_a, rights, flags, min_fd);
    errdefer self.closeFd(owner, fd_a) catch {};
    const fd_b = try self.installFd(owner, object_b, rights, flags, min_fd);
    return .{ .a = fd_a, .b = fd_b };
}

pub fn ipcMessageQueueForSend(self: anytype, object_ref: KernelObjectRef) KernelError!*IpcQueue {
    const slot = self.kernelObjectSlot(object_ref) orelse return KernelError.InvalidState;
    return switch (slot.payload) {
        .endpoint => |endpoint_ref| &(self.ipcEndpointSlot(endpoint_ref) orelse return KernelError.InvalidState).queue,
        .channel => |handle| blk: {
            if (handle.side > 1) return KernelError.InvalidState;
            const channel = self.ipcChannelSlot(handle.channel) orelse return KernelError.InvalidState;
            break :blk &channel.queues[1 - @as(usize, handle.side)];
        },
        .reply => |reply_ref| blk: {
            const reply = self.ipcReplySlot(reply_ref) orelse return KernelError.InvalidState;
            if (reply.sent) return KernelError.InvalidState;
            break :blk &reply.queue;
        },
        else => return KernelError.InvalidState,
    };
}

pub fn markReplySentIfNeeded(self: anytype, object_ref: KernelObjectRef) void {
    const slot = self.kernelObjectSlot(object_ref) orelse return;
    switch (slot.payload) {
        .reply => |reply_ref| {
            const reply = self.ipcReplySlot(reply_ref) orelse return;
            reply.sent = true;
        },
        else => {},
    }
}

pub fn endpointRefEqual(a: IpcEndpointRef, b: IpcEndpointRef) bool {
    return a.index == b.index and a.generation == b.generation;
}

pub fn channelRefEqual(a: IpcChannelRef, b: IpcChannelRef) bool {
    return a.index == b.index and a.generation == b.generation;
}

pub fn replyRefEqual(a: IpcReplyRef, b: IpcReplyRef) bool {
    return a.index == b.index and a.generation == b.generation;
}

pub fn ipcPayloadReceivesFromSendPayload(send_payload: *const KernelObjectPayload, recv_payload: *const KernelObjectPayload) bool {
    return switch (send_payload.*) {
        .endpoint => |send_ref| switch (recv_payload.*) {
            .endpoint => |recv_ref| endpointRefEqual(send_ref, recv_ref),
            else => false,
        },
        .channel => |send_handle| switch (recv_payload.*) {
            .channel => |recv_handle| send_handle.side <= 1 and
                recv_handle.side <= 1 and
                channelRefEqual(send_handle.channel, recv_handle.channel) and
                recv_handle.side == 1 - send_handle.side,
            else => false,
        },
        .reply => |send_ref| switch (recv_payload.*) {
            .reply => |recv_ref| replyRefEqual(send_ref, recv_ref),
            else => false,
        },
        else => false,
    };
}

pub fn ipcRecvWaitKeyFromPayload(payload: *const KernelObjectPayload) ?IpcWaitKey {
    return switch (payload.*) {
        .endpoint => |endpoint_ref| .{
            .kind = .endpoint,
            .index = endpoint_ref.index,
            .generation = endpoint_ref.generation,
            .side = 0,
        },
        .channel => |handle| blk: {
            if (handle.side > 1) break :blk null;
            break :blk .{
                .kind = .channel,
                .index = handle.channel.index,
                .generation = handle.channel.generation,
                .side = handle.side,
            };
        },
        .reply => |reply_ref| .{
            .kind = .reply,
            .index = reply_ref.index,
            .generation = reply_ref.generation,
            .side = 0,
        },
        else => null,
    };
}

pub fn ipcRecvWaitKeyFromSendPayload(payload: *const KernelObjectPayload) ?IpcWaitKey {
    return switch (payload.*) {
        .endpoint => |endpoint_ref| .{
            .kind = .endpoint,
            .index = endpoint_ref.index,
            .generation = endpoint_ref.generation,
            .side = 0,
        },
        .channel => |handle| blk: {
            if (handle.side > 1) break :blk null;
            break :blk .{
                .kind = .channel,
                .index = handle.channel.index,
                .generation = handle.channel.generation,
                .side = 1 - handle.side,
            };
        },
        .reply => |reply_ref| .{
            .kind = .reply,
            .index = reply_ref.index,
            .generation = reply_ref.generation,
            .side = 0,
        },
        else => null,
    };
}

pub fn ipcWaitListForRecvPayload(self: anytype, payload: *const KernelObjectPayload) ?*IpcWaitList {
    return switch (payload.*) {
        .endpoint => |endpoint_ref| &(self.ipcEndpointSlot(endpoint_ref) orelse return null).waiters,
        .channel => |handle| blk: {
            if (handle.side > 1) break :blk null;
            const channel = self.ipcChannelSlot(handle.channel) orelse break :blk null;
            break :blk &channel.waiters[@as(usize, handle.side)];
        },
        .reply => |reply_ref| &(self.ipcReplySlot(reply_ref) orelse return null).waiters,
        else => null,
    };
}

pub fn ipcWaitListForSendPayload(self: anytype, payload: *const KernelObjectPayload) ?*IpcWaitList {
    return switch (payload.*) {
        .endpoint => |endpoint_ref| &(self.ipcEndpointSlot(endpoint_ref) orelse return null).waiters,
        .channel => |handle| blk: {
            if (handle.side > 1) break :blk null;
            const channel = self.ipcChannelSlot(handle.channel) orelse break :blk null;
            break :blk &channel.waiters[1 - @as(usize, handle.side)];
        },
        .reply => |reply_ref| &(self.ipcReplySlot(reply_ref) orelse return null).waiters,
        else => null,
    };
}

pub fn registerIpcReadableWaiterForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    requested_events: u64,
    pollfd_va: u64,
    thread_index: usize,
    thread_generation: u32,
) KernelError!void {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    if ((requested_events & fd_abi.event_readable) == 0) return;
    if (thread_index > std.math.maxInt(u32)) return KernelError.InvalidState;
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.recv or (!entry.rights.wait and !entry.rights.poll)) return;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return KernelError.InvalidState;
    const key = @TypeOf(self.*).ipcRecvWaitKeyFromPayload(&slot.payload) orelse return;
    const waiters = self.ipcWaitListForRecvPayload(&slot.payload) orelse return KernelError.InvalidState;
    try waiters.register(key, owner, pollfd_va, 0, 0, 0, requested_events, 0, thread_index, thread_generation);
}

pub fn registerIpcRecvCompletionWaiterForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    msg_va: u64,
    fd_capacity: u8,
    thread_index: usize,
    thread_generation: u32,
) KernelError!void {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    if (msg_va == 0) return KernelError.InvalidState;
    if (thread_index > std.math.maxInt(u32)) return KernelError.InvalidState;
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.recv or !entry.rights.wait) return KernelError.InvalidState;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return KernelError.InvalidState;
    const key = @TypeOf(self.*).ipcRecvWaitKeyFromPayload(&slot.payload) orelse return;
    const waiters = self.ipcWaitListForRecvPayload(&slot.payload) orelse return KernelError.InvalidState;
    try waiters.register(key, owner, 0, msg_va, fd, fd_capacity, fd_abi.event_readable, 0, thread_index, thread_generation);
}

pub fn unregisterIpcReadableWaiterForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    requested_events: u64,
    thread_index: usize,
    thread_generation: u32,
) void {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    if ((requested_events & fd_abi.event_readable) == 0) return;
    const entry = self.fdEntryConst(owner, fd) orelse return;
    if (!entry.rights.recv or (!entry.rights.wait and !entry.rights.poll)) return;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return;
    const waiters = self.ipcWaitListForRecvPayload(&slot.payload) orelse return;
    waiters.unregister(thread_index, thread_generation);
}

pub fn wakeIpcWaitersForSendFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    out: []ThreadWakeTarget,
) KernelError!usize {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    const source = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    const send_slot = self.kernelObjectSlotConst(source.object) orelse return KernelError.InvalidState;
    const key = @TypeOf(self.*).ipcRecvWaitKeyFromSendPayload(&send_slot.payload) orelse return KernelError.InvalidState;
    _ = fd_abi;
    const waiters = self.ipcWaitListForSendPayload(&send_slot.payload) orelse return KernelError.InvalidState;
    _ = key;
    const count = waiters.takeReadable(out);
    if (count == 0 and out.len != 0) {
        if (waiters.handoffHint()) |hint| {
            out[0] = hint;
            return 1;
        }
    }
    return count;
}

pub fn takeIpcChannelPeerCloseWaiters(
    self: anytype,
    closed_handle: IpcChannelHandle,
    out: []ThreadWakeTarget,
) usize {
    if (closed_handle.side > 1) return 0;
    const channel = self.ipcChannelSlot(closed_handle.channel) orelse return 0;
    if (channel.ref_count != 1) return 0;
    return channel.waiters[1 - @as(usize, closed_handle.side)].takeEvents(
        @import("kernel_abi_root").fd_abi.event_hangup,
        out,
    );
}

pub fn ipcRecvWakeOwnersForSendFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    out: []PrincipalId,
) KernelError!usize {
    const source = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    const send_slot = self.kernelObjectSlotConst(source.object) orelse return KernelError.InvalidState;
    var count: usize = 0;
    var process_index: usize = 0;
    while (process_index < self.process_capacity) : (process_index += 1) {
        const desc = self.processDescriptorSlotConst(process_index) orelse continue;
        if (!desc.active) continue;
        const table = self.fdTableForProcessIndexConst(process_index) orelse continue;
        var fd_index: usize = 0;
        while (fd_index < fd_table_entries) : (fd_index += 1) {
            const candidate = table.entries[fd_index];
            if (candidate.object.isNull()) continue;
            if (!candidate.rights.recv or (!candidate.rights.wait and !candidate.rights.poll)) continue;
            const recv_slot = self.kernelObjectSlotConst(candidate.object) orelse continue;
            if (!@TypeOf(self.*).ipcPayloadReceivesFromSendPayload(&send_slot.payload, &recv_slot.payload)) continue;
            @TypeOf(self.*).appendUniquePrincipal(out, &count, desc.principal);
            break;
        }
    }
    return count;
}

pub fn ipcMessageQueueForRecv(self: anytype, object_ref: KernelObjectRef) KernelError!*IpcQueue {
    const slot = self.kernelObjectSlot(object_ref) orelse return KernelError.InvalidState;
    return switch (slot.payload) {
        .endpoint => |endpoint_ref| &(self.ipcEndpointSlot(endpoint_ref) orelse return KernelError.InvalidState).queue,
        .channel => |handle| blk: {
            if (handle.side > 1) return KernelError.InvalidState;
            const channel = self.ipcChannelSlot(handle.channel) orelse return KernelError.InvalidState;
            break :blk &channel.queues[@as(usize, handle.side)];
        },
        .reply => |reply_ref| &(self.ipcReplySlot(reply_ref) orelse return KernelError.InvalidState).queue,
        else => return KernelError.InvalidState,
    };
}

pub fn fdFreeCountFrom(self: anytype, owner: PrincipalId, min_fd: Fd) KernelError!usize {
    const table = try self.fdTableForActiveProcessConst(owner);
    var index = @TypeOf(self.*).fdIndex(min_fd) orelse return KernelError.InvalidState;
    var count: usize = 0;
    while (index < fd_table_entries) : (index += 1) {
        if (table.entries[index].isEmpty()) count += 1;
    }
    return count;
}

pub fn validateIpcSendFds(specs: []const IpcSendFd) KernelError!void {
    if (specs.len > max_ipc_message_fds) return KernelError.InvalidState;
    for (specs, 0..) |spec, i| {
        if (!spec.move) continue;
        for (specs[i + 1 ..]) |later| {
            if (later.move and later.fd == spec.fd) return KernelError.InvalidState;
        }
    }
}

pub fn appendIpcSendFd(
    self: anytype,
    owner: PrincipalId,
    msg: *IpcMessage,
    spec: IpcSendFd,
) KernelError!void {
    if (msg.fd_count >= max_ipc_message_fds) return KernelError.InvalidState;
    const table = try self.fdTableForActiveProcessConst(owner);
    const index = @TypeOf(self.*).fdIndex(spec.fd) orelse return KernelError.InvalidState;
    const source = table.entries[index];
    if (source.object.isNull()) return KernelError.InvalidState;
    if (!source.rights.transfer) return KernelError.InvalidState;
    if (!isFdRightsSubset(spec.rights, source.rights)) return KernelError.InvalidState;
    if (self.kernelObjectSlotConst(source.object) == null) return KernelError.InvalidState;
    try self.retainKernelObject(source.object);
    msg.fds[msg.fd_count] = .{
        .object = source.object,
        .rights = fdRightsFromBits(fdRightsToBits(spec.rights)),
        .flags = fdFlagsFromBits(fdFlagsToBits(spec.flags)),
    };
    msg.fd_count += 1;
}

pub fn closeMovedIpcSendFdsWithFreeList(
    self: anytype,
    owner: PrincipalId,
    specs: []const IpcSendFd,
    free_list: *FreePageList,
) void {
    for (specs) |spec| {
        if (!spec.move) continue;
        self.closeFdWithFreeList(owner, spec.fd, free_list) catch {};
    }
}

pub fn buildIpcMessage(
    self: anytype,
    owner: PrincipalId,
    send: IpcSendMessage,
    free_list: *FreePageList,
) KernelError!IpcMessage {
    try @TypeOf(self.*).validateIpcSendFds(send.fds);
    var msg = IpcMessage{
        .active = true,
        .sender = owner,
        .words = send.words,
    };
    errdefer self.releaseIpcMessageWithFreeList(&msg, free_list);
    for (send.fds) |spec| {
        try self.appendIpcSendFd(owner, &msg, spec);
    }
    return msg;
}

pub fn enqueueIpcMessage(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    send: IpcSendMessage,
    require_call: bool,
    free_list: *FreePageList,
) KernelError!void {
    const table = try self.fdTableForActiveProcessConst(owner);
    const index = @TypeOf(self.*).fdIndex(fd) orelse return KernelError.InvalidState;
    const entry = table.entries[index];
    if (entry.object.isNull()) return KernelError.InvalidState;
    if (require_call) {
        if (!entry.rights.call) return KernelError.InvalidState;
    } else if (!entry.rights.send) {
        return KernelError.InvalidState;
    }
    var msg = try self.buildIpcMessage(owner, send, free_list);
    errdefer self.releaseIpcMessageWithFreeList(&msg, free_list);
    const queue = try self.ipcMessageQueueForSend(entry.object);
    try queue.push(msg);
    msg = .{};
    self.markReplySentIfNeeded(entry.object);
    self.closeMovedIpcSendFdsWithFreeList(owner, send.fds, free_list);
}

pub fn ipcSend(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    send: IpcSendMessage,
    free_list: *FreePageList,
) KernelError!void {
    try self.requireActiveProcess(owner);
    try self.enqueueIpcMessage(owner, fd, send, false, free_list);
}

pub fn ipcReply(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    send: IpcSendMessage,
    free_list: *FreePageList,
) KernelError!void {
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return KernelError.InvalidState;
    if (slot.kind != .reply) return KernelError.InvalidState;
    try self.ipcSend(owner, fd, send, free_list);
}

pub fn ipcCall(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    send: IpcSendMessage,
    min_reply_fd: Fd,
    free_list: *FreePageList,
) KernelError!Fd {
    try self.requireActiveProcess(owner);
    if (send.fds.len >= max_ipc_message_fds) return KernelError.InvalidState;
    const reply_ref = try self.createIpcReply();
    const reply_object = self.createKernelObject(.reply, .{ .reply = reply_ref }) catch |err| {
        self.clearIpcReplySlot(reply_ref);
        return err;
    };
    errdefer if (self.kernelObjectSlot(reply_object)) |slot| self.clearKernelObjectSlot(slot);
    const reply_fd = try self.installFd(owner, reply_object, @TypeOf(self.*).replyReceiveRights(), .{ .cloexec = true }, min_reply_fd);
    errdefer self.closeFdWithFreeList(owner, reply_fd, free_list) catch {};

    var msg = try self.buildIpcMessage(owner, send, free_list);
    errdefer self.releaseIpcMessageWithFreeList(&msg, free_list);
    try self.retainKernelObject(reply_object);
    msg.fds[msg.fd_count] = .{
        .object = reply_object,
        .rights = @TypeOf(self.*).replySendRights(),
        .flags = .{ .cloexec = true },
    };
    msg.fd_count += 1;

    const table = try self.fdTableForActiveProcessConst(owner);
    const index = @TypeOf(self.*).fdIndex(fd) orelse return KernelError.InvalidState;
    const entry = table.entries[index];
    if (entry.object.isNull() or !entry.rights.call) return KernelError.InvalidState;
    const queue = try self.ipcMessageQueueForSend(entry.object);
    try queue.push(msg);
    msg = .{};
    self.closeMovedIpcSendFdsWithFreeList(owner, send.fds, free_list);
    return reply_fd;
}

pub fn ipcRecv(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    fd_capacity: usize,
    min_fd: Fd,
    free_list: *FreePageList,
) KernelError!IpcRecvResult {
    try self.requireActiveProcess(owner);
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.recv) return KernelError.InvalidState;
    if (fd_capacity > max_ipc_message_fds) return KernelError.InvalidState;
    const queue = try self.ipcMessageQueueForRecv(entry.object);
    const pending = queue.peek() orelse return KernelError.MailboxEmpty;
    if (pending.fd_count > fd_capacity) return KernelError.TableFull;
    if (try self.fdFreeCountFrom(owner, min_fd) < pending.fd_count) return KernelError.TableFull;

    var installed: [max_ipc_message_fds]Fd = [_]Fd{0} ** max_ipc_message_fds;
    var installed_count: usize = 0;
    errdefer {
        var i: usize = 0;
        while (i < installed_count) : (i += 1) {
            self.closeFdWithFreeList(owner, installed[i], free_list) catch {};
        }
    }

    var result = IpcRecvResult{
        .words = pending.words,
        .fd_count = pending.fd_count,
    };
    var i: usize = 0;
    while (i < pending.fd_count) : (i += 1) {
        const item = pending.fds[i];
        const new_fd = try self.installFd(owner, item.object, item.rights, item.flags, min_fd);
        installed[installed_count] = new_fd;
        installed_count += 1;
        result.fds[i] = .{
            .fd = new_fd,
            .rights = item.rights,
            .flags = item.flags,
        };
    }

    var msg = queue.pop() orelse return KernelError.MailboxEmpty;
    self.releaseIpcMessageWithFreeList(&msg, free_list);
    return result;
}
