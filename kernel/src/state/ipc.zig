const scheduler = @import("../scheduler.zig").connection;
const sc = @import("../syscall/numbers.zig");
const references = types.references;
const std = @import("std");
const builtin = @import("builtin");
const x86_platform = @import("../arch/x86_64/platform.zig");
const types = @import("types.zig");
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
const IpcReceiverHint = types.IpcReceiverHint;
const IpcNotifyWaiter = types.IpcNotifyWaiter;
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
const IpcRecvOutcome = types.IpcRecvOutcome;
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

const IpcChannelPair = struct {
    a: Fd,
    b: Fd,
};

// Failure-only diagnostic, serialized by the kernel state lock. Keep scans
// and serial output off the successful allocation path and bound the output.
var channel_full_reports: usize = 0;

pub fn channelDiagnosticStaticEndAddr() usize {
    return @intFromPtr(&channel_full_reports) + @sizeOf(@TypeOf(channel_full_reports));
}

fn reportChannelTableFull(self: anytype) void {
    if (builtin.is_test or channel_full_reports >= 4) return;
    channel_full_reports += 1;
    var active: usize = 0;
    var refs = [_]usize{0} ** 4;
    var queued: usize = 0;
    {
        var pool_iter_22 = self.ipc_channels.iterator();
        while (pool_iter_22.next()) |pool_entry_22| {
            const slot = pool_entry_22.value;
            if (!slot.active) continue;
            active += 1;
            refs[@min(@as(usize, slot.ref_count), refs.len - 1)] += 1;
            queued += @as(usize, slot.queues[0].len) + @as(usize, slot.queues[1].len);
        }
    }
    var endpoint_objects: usize = 0;
    var endpoint_refs: u64 = 0;
    {
        var pool_iter_20 = self.fd_objects.iterator();
        while (pool_iter_20.next()) |pool_entry_20| {
            const slot = pool_entry_20.value;
            if (slot.kind != .channel) continue;
            endpoint_objects += 1;
            endpoint_refs += slot.ref_count;
        }
    }
    @import("../kernel_log.zig").writeFmt(
        "ipc: channel-table-full report={} active={} capacity={} refs0={} refs1={} refs2={} refs_other={} queued={} endpoint_objects={} endpoint_refs={}\n",
        .{ channel_full_reports, active, self.ipc_channels.capacity, refs[0], refs[1], refs[2], refs[3], queued, endpoint_objects, endpoint_refs },
    );
    // FD counts include aliases and inherited descriptors. Unique channels
    // distinguish those from independently allocated channel pairs. Rights
    // counts overlap: one descriptor can contribute to several columns.
    for (0..self.process_capacity) |process_index| {
        const desc = self.processDescriptorSlotConst(process_index) orelse continue;
        if (!desc.active) continue;
        const principal = processPrincipalFromIndex(process_index) orelse continue;
        const table = self.fdTableForProcessIndexConst(process_index) orelse continue;
        var channels: usize = 0;
        var channel_slots = self.ipc_channels.iterator();
        while (channel_slots.next()) |item| self.ipc_channels.mark(item.ref.index, 1, false);
        var counts = struct {
            fds: usize = 0,
            send: usize = 0,
            recv: usize = 0,
            call: usize = 0,
            wait: usize = 0,
            poll: usize = 0,
            transfer: usize = 0,
            dup: usize = 0,
            cloexec: usize = 0,
            private: usize = 0,
        }{};
        for (0..table.capacity) |i| {
            const entry = table.at(i).*;
            if (entry.object.kind != .channel) continue;
            const object = self.kernelObjectSlotConst(entry.object) orelse continue;
            counts.fds += 1;
            counts.send += @intFromBool(entry.rights.send);
            counts.recv += @intFromBool(entry.rights.recv);
            counts.call += @intFromBool(entry.rights.call);
            counts.wait += @intFromBool(entry.rights.wait);
            counts.poll += @intFromBool(entry.rights.poll);
            counts.transfer += @intFromBool(entry.rights.transfer);
            counts.dup += @intFromBool(entry.rights.dup);
            counts.cloexec += @intFromBool(entry.flags.cloexec);
            counts.private += @intFromBool(entry.flags.private);
            const channel = object.payload.channel.channel;
            if (self.ipcChannelSlotConst(channel) != null and !self.ipc_channels.isMarked(channel.index, 1)) {
                self.ipc_channels.mark(channel.index, 1, true);
                channels += 1;
            }
        }
        @import("../kernel_log.zig").writeFmt(
            "ipc: channel-owner report={} principal={} label={s} fds={} channels={} send={} recv={} call={} wait={} poll={} transfer={} dup={} cloexec={} private={}\n",
            .{ channel_full_reports, @intFromEnum(principal), desc.label, counts.fds, channels, counts.send, counts.recv, counts.call, counts.wait, counts.poll, counts.transfer, counts.dup, counts.cloexec, counts.private },
        );
    }
}

pub fn releaseIpcMessage(self: anytype, msg: *IpcMessage) void {
    var i: usize = 0;
    while (i < msg.fd_count and i < max_ipc_message_fds) : (i += 1) {
        const object_ref = msg.fds[i].object();
        msg.removeReference(i);
        if (!object_ref.isNull()) self.releaseKernelObject(object_ref);
        msg.fds[i] = .{};
    }
    if (!msg.call.isNull()) self.releaseIpcCallAuthority(msg.call);
    msg.* = .{};
}

pub fn releaseIpcMessageWithFreeList(self: anytype, msg: *IpcMessage, free_list: *FreePageList) void {
    var i: usize = 0;
    while (i < msg.fd_count and i < max_ipc_message_fds) : (i += 1) {
        const object_ref = msg.fds[i].object();
        msg.removeReference(i);
        if (!object_ref.isNull()) self.releaseKernelObjectWithFreeList(object_ref, free_list);
        msg.fds[i] = .{};
    }
    if (!msg.call.isNull()) self.releaseIpcCallAuthority(msg.call);
    msg.* = .{};
}

pub fn clearIpcQueue(self: anytype, queue: *IpcQueue) void {
    while (true) {
        var endpoint_guard = queue.acquire();
        const popped = queue.pop();
        endpoint_guard.end();
        const msg_value = popped orelse break;
        var msg = msg_value;
        self.releaseIpcMessage(&msg);
    }
    // Clearing contents does not create a new queue identity. Preserve its
    // monotonic message epoch until the owning object generation is retired.
    // pop already resets head while holding the endpoint.
}

pub fn clearIpcQueueWithFreeList(self: anytype, queue: *IpcQueue, free_list: *FreePageList) void {
    while (true) {
        var endpoint_guard = queue.acquire();
        const popped = queue.pop();
        endpoint_guard.end();
        const msg_value = popped orelse break;
        var msg = msg_value;
        self.releaseIpcMessageWithFreeList(&msg, free_list);
    }
    // Clearing contents does not create a new queue identity. Preserve its
    // monotonic message epoch until the owning object generation is retired.
    // pop already resets head while holding the endpoint.
}

// Callers retain the object through K (slow) or the FD capability guard.
// Resolve before locking; never take the directory leaf while holding T.
pub fn exclusionForPayload(self: anytype, payload: *const KernelObjectPayload) ?*@import("../sync/ipc_endpoint.zig").Lock {
    return switch (payload.*) {
        .endpoint => |ref| &((self.ipcEndpointSlot(ref) orelse return null).exclusion),
        .channel => |handle| &((self.ipcChannelSlot(handle.channel) orelse return null).exclusion),
        .reply => |ref| &((self.ipcReplySlot(ref) orelse return null).exclusion),
        else => null,
    };
}

pub fn ipcEndpointSlot(self: anytype, endpoint_ref: IpcEndpointRef) ?*IpcEndpointSlot {
    if (endpoint_ref.isNull()) return null;
    return self.ipc_endpoints.resolve(.{ .index = endpoint_ref.index, .generation = endpoint_ref.generation });
}

pub fn ipcEndpointSlotConst(self: anytype, endpoint_ref: IpcEndpointRef) ?*const IpcEndpointSlot {
    if (endpoint_ref.isNull()) return null;
    return self.ipc_endpoints.resolve(.{ .index = endpoint_ref.index, .generation = endpoint_ref.generation });
}

pub fn clearIpcEndpointSlot(self: anytype, endpoint_ref: IpcEndpointRef) void {
    const slot = self.ipcEndpointSlot(endpoint_ref) orelse return;
    {
        var endpoint_guard = slot.exclusion.acquire();
        defer endpoint_guard.end();
        slot.live.close();
    }
    self.clearIpcQueue(&slot.queue);
    self.cancelIpcDirectReceivers(&slot.waiters, null, null);
    {
        var endpoint_guard = slot.waiters.acquire();
        defer endpoint_guard.end();
        slot.waiters.waiters.deinitInto(&self.ipc_endpoints.body_owner);
    }
    self.ipc_endpoints.withdraw(slot, &self.object_directory_leaf);
    slot.retired_next = self.retired_ipc_endpoints;
    self.retired_ipc_endpoints = slot;
    self.reclaimFdCapabilities();
}

pub fn clearIpcEndpointSlotWithFreeList(
    self: anytype,
    endpoint_ref: IpcEndpointRef,
    free_list: *FreePageList,
) void {
    const slot = self.ipcEndpointSlot(endpoint_ref) orelse return;
    {
        var endpoint_guard = slot.exclusion.acquire();
        defer endpoint_guard.end();
        slot.live.close();
    }
    self.clearIpcQueueWithFreeList(&slot.queue, free_list);
    self.cancelIpcDirectReceivers(&slot.waiters, null, null);
    {
        var endpoint_guard = slot.waiters.acquire();
        defer endpoint_guard.end();
        slot.waiters.waiters.deinitInto(&self.ipc_endpoints.body_owner);
    }
    self.ipc_endpoints.withdraw(slot, &self.object_directory_leaf);
    slot.retired_next = self.retired_ipc_endpoints;
    self.retired_ipc_endpoints = slot;
    self.reclaimFdCapabilities();
}

// Reserve pages before publishing a slot. Rollback transfers rejected returns
// into the stable table owner, never into a stack-local owner or half-live body.
fn reserveMessagePage(transaction: *@import("../memory/owned_storage.zig").Transaction) KernelError!*types.IpcMessagePage {
    const page = transaction.reserve(types.IpcMessagePage, 1, .stable) catch return KernelError.OutOfFreePages;
    page[0] = @splat(.{});
    return &page[0];
}

pub fn createIpcEndpoint(self: anytype) KernelError!IpcEndpointRef {
    self.ipc_endpoints.return_queue = &self.fd_retired;
    var messages = @import("../memory/owned_storage.zig").Transaction.init(&self.ipc_endpoints.body_owner, try self.poolAllocator());
    defer if (!messages.finished) {
        messages.rollback() catch unreachable;
        // A failed page return must remain reachable from PMM, even when
        // message allocation failed before any slot was allocated.
        self.ipc_endpoints.reclaim();
    };
    const pages = .{ try reserveMessagePage(&messages), try reserveMessagePage(&messages) };
    const allocation = try self.allocatePool(&self.ipc_endpoints, &self.object_directory_leaf, std.math.maxInt(u32));
    messages.commitAdditionalStable() catch unreachable;
    allocation.value.queue.pages = pages;
    allocation.value.queue.exclusion = &allocation.value.exclusion;
    allocation.value.waiters.exclusion = &allocation.value.exclusion;
    allocation.value.queue.reference_identity = .{ .container = 1, .owner = allocation.ref.index, .owner_generation = allocation.ref.generation };
    allocation.value.live = @import("../sync/ipc_lifetime.zig").Admission.init();
    allocation.value.active = true;
    return .{ .index = allocation.ref.index, .generation = allocation.ref.generation };
}

pub fn ipcChannelSlot(self: anytype, channel_ref: IpcChannelRef) ?*IpcChannelSlot {
    if (channel_ref.isNull()) return null;
    return self.ipc_channels.resolve(.{ .index = channel_ref.index, .generation = channel_ref.generation });
}

pub fn ipcChannelSlotConst(self: anytype, channel_ref: IpcChannelRef) ?*const IpcChannelSlot {
    if (channel_ref.isNull()) return null;
    return self.ipc_channels.resolve(.{ .index = channel_ref.index, .generation = channel_ref.generation });
}

pub fn createIpcChannel(self: anytype) KernelError!IpcChannelRef {
    self.ipc_channels.return_queue = &self.fd_retired;
    var messages = @import("../memory/owned_storage.zig").Transaction.init(&self.ipc_channels.body_owner, try self.poolAllocator());
    defer if (!messages.finished) {
        messages.rollback() catch unreachable;
        // A failed page return must remain reachable from PMM, even when
        // message allocation failed before any slot was allocated.
        self.ipc_channels.reclaim();
    };
    const pages = .{ try reserveMessagePage(&messages), try reserveMessagePage(&messages), try reserveMessagePage(&messages) };
    const allocation = try self.allocatePool(&self.ipc_channels, &self.object_directory_leaf, std.math.maxInt(u32));
    messages.commitAdditionalStable() catch unreachable;
    allocation.value.queues[0].pages = .{ pages[0], pages[1] };
    allocation.value.queues[1].pages = .{ pages[1], pages[2] };
    allocation.value.queues[1].page_offset = 5;
    for (&allocation.value.queues) |*queue| queue.exclusion = &allocation.value.exclusion;
    for (&allocation.value.waiters) |*list| list.exclusion = &allocation.value.exclusion;
    for (&allocation.value.queues, 0..) |*queue, direction| queue.reference_identity = .{ .container = 2, .owner = allocation.ref.index, .owner_generation = allocation.ref.generation, .direction = @intCast(direction) };
    allocation.value.live = @import("../sync/ipc_lifetime.zig").Admission.init();
    allocation.value.active = true;
    allocation.value.ref_count = 2;
    return .{ .index = allocation.ref.index, .generation = allocation.ref.generation };
}

pub fn releaseIpcChannelHandle(self: anytype, handle: IpcChannelHandle) void {
    const slot = self.ipcChannelSlot(handle.channel) orelse return;
    const remaining = blk: {
        var endpoint_guard = slot.exclusion.acquire();
        defer endpoint_guard.end();
        if (slot.ref_count == 0) return;
        slot.ref_count -= 1;
        if (slot.ref_count == 0) slot.live.close();
        break :blk slot.ref_count;
    };
    if (remaining == 1) for (&slot.waiters) |*list| self.cancelIpcDirectReceivers(list, null, null);
    // The last capability for this side has gone. Its incoming requests can
    // never be received, even while the peer keeps the channel object alive.
    if (remaining == 1 and handle.side <= 1) self.clearIpcQueue(&slot.queues[handle.side]);
    if (remaining != 0) return;
    self.clearIpcQueue(&slot.queues[0]);
    self.clearIpcQueue(&slot.queues[1]);
    for (&slot.waiters) |*list| {
        self.cancelIpcDirectReceivers(list, null, null);
        var endpoint_guard = list.acquire();
        defer endpoint_guard.end();
        list.waiters.deinitInto(&self.ipc_channels.body_owner);
    }
    self.ipc_channels.withdraw(slot, &self.object_directory_leaf);
    slot.retired_next = self.retired_ipc_channels;
    self.retired_ipc_channels = slot;
    self.reclaimFdCapabilities();
}

pub fn releaseIpcChannelHandleWithFreeList(
    self: anytype,
    handle: IpcChannelHandle,
    free_list: *FreePageList,
) void {
    const slot = self.ipcChannelSlot(handle.channel) orelse return;
    const remaining = blk: {
        var endpoint_guard = slot.exclusion.acquire();
        defer endpoint_guard.end();
        if (slot.ref_count == 0) return;
        slot.ref_count -= 1;
        if (slot.ref_count == 0) slot.live.close();
        break :blk slot.ref_count;
    };
    if (remaining == 1) for (&slot.waiters) |*list| self.cancelIpcDirectReceivers(list, null, null);
    if (remaining == 1 and handle.side <= 1) self.clearIpcQueueWithFreeList(&slot.queues[handle.side], free_list);
    if (remaining != 0) return;
    self.clearIpcQueueWithFreeList(&slot.queues[0], free_list);
    self.clearIpcQueueWithFreeList(&slot.queues[1], free_list);
    for (&slot.waiters) |*list| {
        self.cancelIpcDirectReceivers(list, null, null);
        var endpoint_guard = list.acquire();
        defer endpoint_guard.end();
        list.waiters.deinitInto(&self.ipc_channels.body_owner);
    }
    self.ipc_channels.withdraw(slot, &self.object_directory_leaf);
    slot.retired_next = self.retired_ipc_channels;
    self.retired_ipc_channels = slot;
    self.reclaimFdCapabilities();
}

pub fn ipcReplySlot(self: anytype, reply_ref: IpcReplyRef) ?*IpcReplySlot {
    if (reply_ref.isNull()) return null;
    return self.ipc_replies.resolve(.{ .index = reply_ref.index, .generation = reply_ref.generation });
}

pub fn ipcReplySlotConst(self: anytype, reply_ref: IpcReplyRef) ?*const IpcReplySlot {
    if (reply_ref.isNull()) return null;
    return self.ipc_replies.resolve(.{ .index = reply_ref.index, .generation = reply_ref.generation });
}

pub fn clearIpcReplySlot(self: anytype, reply_ref: IpcReplyRef) void {
    const slot = self.ipcReplySlot(reply_ref) orelse return;
    self.cancelIpcDirectReceivers(&slot.waiters, null, null);
    {
        var endpoint_guard = slot.waiters.acquire();
        defer endpoint_guard.end();
        slot.waiters.waiters.deinitInto(&self.ipc_replies.body_owner);
    }
    self.ipc_replies.remove(slot, null);
}

pub fn clearIpcReplySlotWithFreeList(
    self: anytype,
    reply_ref: IpcReplyRef,
    free_list: *FreePageList,
) void {
    const slot = self.ipcReplySlot(reply_ref) orelse return;
    _ = free_list;
    self.cancelIpcDirectReceivers(&slot.waiters, null, null);
    {
        var endpoint_guard = slot.waiters.acquire();
        defer endpoint_guard.end();
        slot.waiters.waiters.deinitInto(&self.ipc_replies.body_owner);
    }
    self.ipc_replies.remove(slot, null);
}

pub fn createIpcReply(self: anytype) KernelError!IpcReplyRef {
    const allocation = try self.allocateWaitPool(&self.ipc_replies);
    allocation.value.waiters.exclusion = &allocation.value.exclusion;
    allocation.value.active = true;
    allocation.value.ref = .{ .index = allocation.ref.index, .generation = allocation.ref.generation };
    allocation.value.block_owner.context = allocation.value;
    allocation.value.receive.context = allocation.value;
    allocation.value.outgoing.context = allocation.value;
    allocation.value.implicit.context = allocation.value;
    return allocation.value.ref;
}

pub fn ensureIpcBlock(self: anytype, owner: PrincipalId, index: usize, generation: u32) KernelError!*IpcReplySlot {
    if (scheduler.ipcBlock(index, generation, owner)) |block| return block;
    const ref = try self.createIpcReply();
    const block = self.ipcReplySlot(ref).?;
    block.lifetime_refs.store(1, .release);
    if (!scheduler.bindIpcBlock(index, generation, owner, &block.block_owner)) {
        self.releaseIpcCallReference(ref);
        return KernelError.InvalidState;
    }
    return block;
}

pub fn registerIpcDirectReceive(self: anytype, owner: PrincipalId, fd: Fd, memory: @import("kernel_abi_root").ipc_abi.ReceiveMemory, capacity: u8, index: usize, generation: u32, token: u64) KernelError!void {
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.recv or !entry.rights.wait) return KernelError.InvalidState;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return KernelError.InvalidState;
    const key = @TypeOf(self.*).ipcRecvWaitKeyFromPayload(&slot.payload) orelse return KernelError.InvalidState;
    const list = self.ipcWaitListForRecvPayload(&slot.payload) orelse return KernelError.InvalidState;
    const block = try self.ensureIpcBlock(owner, index, generation);
    var endpoint_guard = list.acquire();
    defer endpoint_guard.end();
    if (!scheduler.ipcReceiveDetached(block) or block.receive_list != null) return KernelError.InvalidState;
    const target: ThreadWakeTarget = .{ .owner = owner, .thread_index = index, .thread_generation = generation, .wait_token = token, .recv_memory = memory, .recv_fd = fd, .recv_fd_capacity = capacity, .receive_block = block.ref, .registration = .{ .kind = switch (key.kind) {
        .endpoint => .endpoint,
        .channel => .channel,
        .reply => .reply,
        else => return KernelError.InvalidState,
    }, .index = key.index, .generation = key.generation, .side = key.side } };
    block.retain(1);
    block.receive_target = target;
    if (!scheduler.bindIpcReceive(target, &block.receive)) {
        self.releaseIpcCallReference(block.ref);
        return KernelError.InvalidState;
    }
    list.appendReceive(block);
}

// Close/revoke retain K and acquire the endpoint before T. Unlink before the endpoint
// can disappear; a terminal T winner retains the node until its later drain.
pub fn cancelIpcDirectReceivers(self: anytype, list: *IpcWaitList, owner: ?PrincipalId, fd: ?Fd) void {
    var endpoint_guard = list.acquire();
    defer endpoint_guard.end();
    _ = self;
    var cursor = list.receive_head;
    while (cursor) |block| {
        cursor = block.receive_next;
        const t = block.receive_target;
        if (owner != null and (t.owner != owner.? or t.recv_fd != fd.?)) continue;
        list.unlinkReceive(block);
        if (scheduler.claimWaitTokenCompletion(t.thread_index, t.thread_generation, t.owner, t.wait_token) == .claimed)
            _ = scheduler.publishClaimedWaitCompletion(t.thread_index, t.thread_generation, t.owner, t.wait_token, sc.syscall_err_closed);
    }
}
pub fn closeIpcReceiveFd(self: anytype, owner: PrincipalId, fd: Fd, object: KernelObjectRef) void {
    const slot = self.kernelObjectSlotConst(object) orelse return;
    const list = self.ipcWaitListForRecvPayload(&slot.payload) orelse return;
    self.cancelIpcDirectReceivers(list, owner, fd);
}

pub fn resetNativeIpcObjects(self: anytype) void {
    @import("fd_capability.zig").deinit(self);
    {
        var pool_iter_21 = self.ipc_endpoints.iterator();
        while (pool_iter_21.next()) |pool_entry_21| {
            const slot = pool_entry_21.value;
            if (slot.active) self.clearIpcQueue(&slot.queue);
            self.cancelIpcDirectReceivers(&slot.waiters, null, null);
            {
                var endpoint_guard = slot.waiters.acquire();
                defer endpoint_guard.end();
                slot.waiters.waiters.deinitInto(&self.ipc_endpoints.body_owner);
            }
        }
    }
    {
        var pool_iter_23 = self.ipc_channels.iterator();
        while (pool_iter_23.next()) |pool_entry_23| {
            const slot = pool_entry_23.value;
            if (slot.active) {
                self.clearIpcQueue(&slot.queues[0]);
                self.clearIpcQueue(&slot.queues[1]);
            }
            for (&slot.waiters) |*list| {
                self.cancelIpcDirectReceivers(list, null, null);
                var endpoint_guard = list.acquire();
                defer endpoint_guard.end();
                list.waiters.deinitInto(&self.ipc_channels.body_owner);
            }
        }
    }
    {
        var pool_iter_24 = self.ipc_replies.iterator();
        while (pool_iter_24.next()) |pool_entry_24| {
            const slot = pool_entry_24.value;
            self.cancelIpcDirectReceivers(&slot.waiters, null, null);
            {
                var endpoint_guard = slot.waiters.acquire();
                defer endpoint_guard.end();
                slot.waiters.waiters.deinitInto(&self.ipc_replies.body_owner);
            }
        }
    }
    self.ipc_endpoints.deinit();
    self.ipc_channels.deinit();
    self.ipc_replies.deinit();
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

pub fn replySendRights() FdRights {
    return .{
        .dup = true,
        .transfer = true,
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
        // Explicit reply authority may only be exercised by IPC_REPLY's
        // completion claim. IPC_SEND cannot bypass single-use/caller death.
        .reply => return KernelError.InvalidState,
        else => return KernelError.InvalidState,
    };
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

fn registerIpcPollWaiterInList(
    self: anytype,
    waiters: *IpcWaitList,
    key: IpcWaitKey,
    owner: PrincipalId,
    pollfd_va: u64,
    events: u64,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
    group: FdWaitGroupRef,
) KernelError!void {
    waiters.assertHeld();
    waiters.allocator = try self.waitPoolAllocator();
    const registration = waiters.register(
        key,
        owner,
        pollfd_va,
        .{},
        0,
        0,
        events,
        0,
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
}

pub fn registerIpcPollWaitersForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    requested_events: u64,
    pollfd_va: u64,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
    group: FdWaitGroupRef,
) KernelError!bool {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    if (thread_index > std.math.maxInt(u32)) return KernelError.InvalidState;
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.wait and !entry.rights.poll) return false;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return KernelError.InvalidState;
    const exclusion = exclusionForPayload(self, &slot.payload) orelse return false;
    var endpoint_guard = exclusion.acquire();
    defer endpoint_guard.end();
    switch (slot.payload) {
        .endpoint, .reply => {
            var wait_events = requested_events & (fd_abi.event_readable | fd_abi.event_writable);
            if (!entry.rights.recv) wait_events &= ~fd_abi.event_readable;
            if (!entry.rights.send and !entry.rights.call) wait_events &= ~fd_abi.event_writable;
            if (wait_events == 0) return false;
            const key = @TypeOf(self.*).ipcRecvWaitKeyFromPayload(&slot.payload) orelse return false;
            const waiters = self.ipcWaitListForRecvPayload(&slot.payload) orelse
                return KernelError.InvalidState;
            try registerIpcPollWaiterInList(
                self,
                waiters,
                key,
                owner,
                pollfd_va,
                wait_events,
                thread_index,
                thread_generation,
                wait_token,
                group,
            );
            return true;
        },
        .channel => {
            // Channel receive and send readiness refer to opposite queues.
            // Keep a receive-side registration even with events=0 because
            // POLLHUP is implicit and must wake a poll that requested only
            // writable (or no ordinary events) when the peer closes.
            var recv_events = requested_events & fd_abi.event_readable;
            if (!entry.rights.recv) recv_events = 0;
            const recv_key = @TypeOf(self.*).ipcRecvWaitKeyFromPayload(&slot.payload) orelse
                return KernelError.InvalidState;
            const recv_waiters = self.ipcWaitListForRecvPayload(&slot.payload) orelse
                return KernelError.InvalidState;
            try registerIpcPollWaiterInList(
                self,
                recv_waiters,
                recv_key,
                owner,
                pollfd_va,
                recv_events,
                thread_index,
                thread_generation,
                wait_token,
                group,
            );

            var send_events = requested_events & fd_abi.event_writable;
            if (!entry.rights.send and !entry.rights.call) send_events = 0;
            if (send_events != 0) {
                const send_key = @TypeOf(self.*).ipcRecvWaitKeyFromSendPayload(&slot.payload) orelse
                    return KernelError.InvalidState;
                const send_waiters = self.ipcWaitListForSendPayload(&slot.payload) orelse
                    return KernelError.InvalidState;
                try registerIpcPollWaiterInList(
                    self,
                    send_waiters,
                    send_key,
                    owner,
                    pollfd_va,
                    send_events,
                    thread_index,
                    thread_generation,
                    wait_token,
                    group,
                );
            }
            return true;
        },
        else => return false,
    }
}

pub fn registerIpcRecvCompletionWaiterForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    memory: @import("kernel_abi_root").ipc_abi.ReceiveMemory,
    fd_capacity: u8,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
    group: FdWaitGroupRef,
) KernelError!FdWaitRegistrationRef {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    if (!memory.active) return KernelError.InvalidState;
    if (thread_index > std.math.maxInt(u32)) return KernelError.InvalidState;
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.recv or !entry.rights.wait) return KernelError.InvalidState;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return KernelError.InvalidState;
    const key = @TypeOf(self.*).ipcRecvWaitKeyFromPayload(&slot.payload) orelse
        return KernelError.InvalidState;
    const waiters = self.ipcWaitListForRecvPayload(&slot.payload) orelse return KernelError.InvalidState;
    var endpoint_guard = waiters.acquire();
    defer endpoint_guard.end();
    waiters.assertHeld();
    waiters.allocator = try self.waitPoolAllocator();
    const registration = waiters.register(
        key,
        owner,
        0,
        memory,
        fd,
        fd_capacity,
        fd_abi.event_readable,
        0,
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
    return registration;
}

pub fn unregisterIpcPollWaitersForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    thread_index: usize,
    thread_generation: u32,
) void {
    const entry = self.fdEntryConst(owner, fd) orelse return;
    if (!entry.rights.wait and !entry.rights.poll) return;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return;
    const exclusion = exclusionForPayload(self, &slot.payload) orelse return;
    var endpoint_guard = exclusion.acquire();
    defer endpoint_guard.end();
    switch (slot.payload) {
        .endpoint, .reply => {
            const waiters = self.ipcWaitListForRecvPayload(&slot.payload) orelse return;
            waiters.unregister(thread_index, thread_generation);
        },
        .channel => {
            const recv_waiters = self.ipcWaitListForRecvPayload(&slot.payload) orelse return;
            const send_waiters = self.ipcWaitListForSendPayload(&slot.payload) orelse return;
            recv_waiters.unregister(thread_index, thread_generation);
            send_waiters.unregister(thread_index, thread_generation);
        },
        else => {},
    }
}

pub fn ipcRecvFdMatchesRegistration(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    registration: FdWaitRegistrationRef,
) bool {
    const entry = self.fdEntryConst(owner, fd) orelse return false;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return false;
    const key = @TypeOf(self.*).ipcRecvWaitKeyFromPayload(&slot.payload) orelse return false;
    const kind: types.FdWaitRegistrationKind = switch (key.kind) {
        .endpoint => .endpoint,
        .channel => .channel,
        .reply => .reply,
        else => return false,
    };
    return registration.kind == kind and
        registration.index == key.index and
        registration.generation == key.generation and
        registration.side == key.side;
}

pub fn wakeIpcWaitersForSendFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    out: []ThreadWakeTarget,
) KernelError!usize {
    const source = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    const send_slot = self.kernelObjectSlotConst(source.object) orelse return KernelError.InvalidState;
    _ = @TypeOf(self.*).ipcRecvWaitKeyFromSendPayload(&send_slot.payload) orelse
        return KernelError.InvalidState;
    const waiters = self.ipcWaitListForSendPayload(&send_slot.payload) orelse return KernelError.InvalidState;
    var endpoint_guard = waiters.acquire();
    defer endpoint_guard.end();
    return waiters.takeReadable(out);
}

/// Remember a thread that has demonstrably consumed from an endpoint/channel, or the
/// creator that owns the receive side of a one-shot reply.  This is only a
/// scheduling hint: it never registers a wait or changes thread readiness.
pub fn noteIpcReceiverForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    thread_index: usize,
    thread_generation: u32,
) bool {
    if (thread_index > std.math.maxInt(u32) or thread_generation == 0) return false;
    const entry = self.fdEntryConst(owner, fd) orelse return false;
    if (!entry.rights.recv) return false;
    const object_slot = self.kernelObjectSlotConst(entry.object) orelse return false;
    const exclusion = exclusionForPayload(self, &object_slot.payload) orelse return false;
    var endpoint_guard = exclusion.acquire();
    defer endpoint_guard.end();
    const hint = IpcReceiverHint{
        .active = true,
        .owner = owner,
        .thread_index = @intCast(thread_index),
        .thread_generation = thread_generation,
    };
    switch (object_slot.payload) {
        .endpoint => |endpoint_ref| {
            const endpoint = self.ipcEndpointSlot(endpoint_ref) orelse return false;
            endpoint.receiver_hint = hint;
        },
        .reply => |reply_ref| {
            const reply = self.ipcReplySlot(reply_ref) orelse return false;
            reply.receiver_hint = hint;
        },
        .channel => |handle| {
            if (handle.side > 1) return false;
            const channel = self.ipcChannelSlot(handle.channel) orelse return false;
            channel.receiver_hints[handle.side] = hint;
        },
        else => return false,
    }
    return true;
}

/// Return the last confirmed receiver for the object targeted by a send.  The
/// scheduler still validates allocation, generation, readiness, and CPU
/// ownership before it can consume this hint for a directed handoff.
pub fn ipcReceiverHandoffHintForSendFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
) ?ThreadWakeTarget {
    const entry = self.fdEntryConst(owner, fd) orelse return null;
    if (!entry.rights.send and !entry.rights.call) return null;
    const object_slot = self.kernelObjectSlotConst(entry.object) orelse return null;
    const exclusion = exclusionForPayload(self, &object_slot.payload) orelse return null;
    var endpoint_guard = exclusion.acquire();
    defer endpoint_guard.end();
    const hint = switch (object_slot.payload) {
        .endpoint => |endpoint_ref| (self.ipcEndpointSlotConst(endpoint_ref) orelse return null).receiver_hint,
        .reply => |reply_ref| (self.ipcReplySlotConst(reply_ref) orelse return null).receiver_hint,
        .channel => |handle| blk: {
            if (handle.side > 1) return null;
            const channel = self.ipcChannelSlotConst(handle.channel) orelse return null;
            // A send targets the peer's queue, never this handle's receiver.
            break :blk channel.receiver_hints[handle.side ^ 1];
        },
        else => return null,
    };
    if (!hint.active) return null;
    return .{
        .owner = hint.owner,
        .thread_index = hint.thread_index,
        .thread_generation = hint.thread_generation,
    };
}

pub fn takeIpcWritableWaitersForRecvFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    out: []ThreadWakeTarget,
) KernelError!usize {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.recv) return KernelError.InvalidState;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return KernelError.InvalidState;
    // A successful receive creates send capacity for endpoints and for the
    // channel peer that targets this receive queue.  Reply handles are
    // single-shot: reply.sent remains true after dequeue, so they do not
    // become writable again.
    switch (slot.payload) {
        .endpoint, .channel => {},
        .reply => return 0,
        else => return KernelError.InvalidState,
    }
    const waiters = self.ipcWaitListForRecvPayload(&slot.payload) orelse
        return KernelError.InvalidState;
    var endpoint_guard = waiters.acquire();
    defer endpoint_guard.end();
    return waiters.takeEvents(fd_abi.event_writable, out);
}

pub fn takeIpcChannelPeerCloseWaiters(
    self: anytype,
    closed_handle: IpcChannelHandle,
    out: []ThreadWakeTarget,
) usize {
    if (closed_handle.side > 1) return 0;
    const channel = self.ipcChannelSlot(closed_handle.channel) orelse return 0;
    var endpoint_guard = @constCast(channel).exclusion.acquire();
    defer endpoint_guard.end();
    if (channel.ref_count != 1) return 0;
    const hangup = @import("kernel_abi_root").fd_abi.event_hangup;
    const remaining_side = 1 - @as(usize, closed_handle.side);
    var count = channel.waiters[remaining_side].takeEvents(hangup, out);
    if (count < out.len) {
        count += channel.waiters[@as(usize, closed_handle.side)].takeEvents(
            hangup,
            out[count..],
        );
    }
    return count;
}

pub const IpcNotifyPeerResult = struct {
    closed: bool = false,
    /// Detached peer waiter.  Its latch is still set: the caller clears it via
    /// settleIpcPeerNotifyForFd once that waiter's completion is
    /// published, so a waiter that died before the claim cannot swallow it.
    waiter: ?ThreadWakeTarget = null,
};

pub const IpcNotifyConsumeResult = enum { consumed, closed, empty };

fn ipcNotifyChannelForFd(self: anytype, owner: PrincipalId, fd: Fd, rights: FdRights) ?struct {
    channel: *IpcChannelSlot,
    side: usize,
} {
    const view = self.fdPayloadWithRightsConst(owner, fd, rights) orelse return null;
    const handle = switch (view.payload.*) {
        .channel => |handle| handle,
        else => return null,
    };
    if (handle.side > 1) return null;
    const channel = self.ipcChannelSlot(handle.channel) orelse return null;
    return .{ .channel = channel, .side = handle.side };
}

fn notifyWaiterTarget(waiter: IpcNotifyWaiter) ThreadWakeTarget {
    return .{
        .owner = waiter.owner,
        .thread_index = waiter.thread_index,
        .thread_generation = waiter.thread_generation,
        .wait_token = waiter.wait_token,
    };
}

/// IPC_NOTIFY: latch the peer side and detach its lean waiter.  A channel
/// whose peer is gone reports closed instead of latching a side nobody reads.
pub fn ipcNotifyPeerForFd(self: anytype, owner: PrincipalId, fd: Fd) KernelError!IpcNotifyPeerResult {
    try self.requireActiveProcess(owner);
    const found = ipcNotifyChannelForFd(self, owner, fd, .{ .send = true }) orelse
        return KernelError.InvalidState;
    var endpoint_guard = found.channel.exclusion.acquire();
    defer endpoint_guard.end();
    if (found.channel.ref_count == 1) return .{ .closed = true };
    const peer = 1 - found.side;
    @import("../notify_metric.zig").coalesced(found.channel.notify_pending[peer]);
    found.channel.notify_pending[peer] = true;
    const waiter = found.channel.notify_waiters[peer];
    found.channel.notify_waiters[peer] = .{};
    if (!waiter.active) return .{};
    return .{ .waiter = notifyWaiterTarget(waiter) };
}

/// What became of the waiter ipcNotifyPeerForFd detached.
pub const IpcNotifyCompletion = enum {
    /// Completed and now running on the sender's CPU.
    handed_off,
    /// Completed: runnable, or held until CONT if it is stopped.
    published,
    /// Completed; the scheduler retries its publication from the tick.
    pending,
    /// That wait ended first and never received the notification.
    stale,
};

/// Settle the peer latch set by the same ipcNotifyPeerForFd call. A completed
/// waiter, however it is scheduled, owns this notification, so the latch is
/// consumed; a stale one never saw it, so the latch stays for the next
/// NOTIFY_WAIT or WAIT_MANY on that side. The global state lock is held from
/// ipcNotifyPeerForFd through this call (a handoff in between does not drop
/// it), so no other notification can have merged into the latch.
pub fn settleIpcPeerNotifyForFd(self: anytype, owner: PrincipalId, fd: Fd, completion: IpcNotifyCompletion) void {
    if (completion == .stale) return;
    const found = ipcNotifyChannelForFd(self, owner, fd, .{ .send = true }) orelse return;
    var endpoint_guard = found.channel.exclusion.acquire();
    defer endpoint_guard.end();
    found.channel.notify_pending[1 - found.side] = false;
}

/// IPC_NOTIFY_WAIT's non-blocking half.  The latch drains before peer death is
/// reported, as RECV_WAIT drains queued messages first.
pub fn consumeIpcNotifyForFd(self: anytype, owner: PrincipalId, fd: Fd, will_wait: bool) KernelError!IpcNotifyConsumeResult {
    try self.requireActiveProcess(owner);
    const found = ipcNotifyChannelForFd(self, owner, fd, .{ .recv = true, .wait = will_wait }) orelse
        return KernelError.InvalidState;
    var endpoint_guard = found.channel.exclusion.acquire();
    defer endpoint_guard.end();
    if (found.channel.notify_pending[found.side]) {
        found.channel.notify_pending[found.side] = false;
        return .consumed;
    }
    if (found.channel.ref_count == 1) return .closed;
    return .empty;
}

pub fn ipcNotifyWaiterForFd(self: anytype, owner: PrincipalId, fd: Fd) ?ThreadWakeTarget {
    const found = ipcNotifyChannelForFd(self, owner, fd, .{ .recv = true }) orelse return null;
    var endpoint_guard = found.channel.exclusion.acquire();
    defer endpoint_guard.end();
    const waiter = found.channel.notify_waiters[found.side];
    if (!waiter.active) return null;
    return notifyWaiterTarget(waiter);
}

pub fn registerIpcNotifyWaiterForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
) KernelError!void {
    if (thread_index > std.math.maxInt(u32) or wait_token == 0) return KernelError.InvalidState;
    const found = ipcNotifyChannelForFd(self, owner, fd, .{ .recv = true, .wait = true }) orelse
        return KernelError.InvalidState;
    var endpoint_guard = found.channel.exclusion.acquire();
    defer endpoint_guard.end();
    found.channel.notify_waiters[found.side] = .{
        .active = true,
        .owner = owner,
        .thread_index = @intCast(thread_index),
        .thread_generation = thread_generation,
        .wait_token = wait_token,
    };
}

pub fn unregisterIpcNotifyWaiterForFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    thread_index: usize,
    thread_generation: u32,
    wait_token: u64,
) void {
    const found = ipcNotifyChannelForFd(self, owner, fd, .{ .recv = true }) orelse return;
    var endpoint_guard = found.channel.exclusion.acquire();
    defer endpoint_guard.end();
    const waiter = &found.channel.notify_waiters[found.side];
    if (waiter.active and waiter.owner == owner and waiter.thread_index == thread_index and
        waiter.thread_generation == thread_generation and waiter.wait_token == wait_token)
    {
        waiter.* = .{};
    }
}

/// Lean waiters of both sides, read before a close that may drop the last
/// reference: final release resets the slot, after which nothing could name
/// them and they would sleep forever.
pub fn ipcChannelNotifyWaiterSnapshot(self: anytype, handle: IpcChannelHandle) [2]?ThreadWakeTarget {
    var out: [2]?ThreadWakeTarget = .{ null, null };
    const channel = self.ipcChannelSlotConst(handle.channel) orelse return out;
    var endpoint_guard = @constCast(channel).exclusion.acquire();
    defer endpoint_guard.end();
    for (channel.notify_waiters, 0..) |waiter, side| {
        if (waiter.active) out[side] = notifyWaiterTarget(waiter);
    }
    return out;
}

/// After a close: once at most one handle remains (or none, and the slot was
/// reset) no peer can ever notify, so every lean waiter on either side must
/// complete with CLOSED, including one parked on the side a sibling thread
/// just closed.  Detaches still-registered entries and returns how many
/// targets were written.
pub fn takeIpcChannelNotifyWaitersAfterClose(
    self: anytype,
    handle: IpcChannelHandle,
    snapshot: [2]?ThreadWakeTarget,
    out: *[2]ThreadWakeTarget,
) usize {
    var count: usize = 0;
    if (self.ipcChannelSlot(handle.channel)) |channel| {
        var endpoint_guard = channel.exclusion.acquire();
        defer endpoint_guard.end();
        if (channel.ref_count > 1) return 0;
        for (&channel.notify_waiters) |*waiter| {
            if (!waiter.active) continue;
            out[count] = notifyWaiterTarget(waiter.*);
            count += 1;
            waiter.* = .{};
        }
        return count;
    }
    for (snapshot) |entry| {
        if (entry) |target| {
            out[count] = target;
            count += 1;
        }
    }
    return count;
}

pub fn wakeIpcPollWaitersForSendFd(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    out: []ThreadWakeTarget,
) KernelError!usize {
    const source = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    const send_slot = self.kernelObjectSlotConst(source.object) orelse return KernelError.InvalidState;
    const waiters = self.ipcWaitListForSendPayload(&send_slot.payload) orelse return KernelError.InvalidState;
    var endpoint_guard = waiters.acquire();
    defer endpoint_guard.end();
    return waiters.takeReadablePollWaiters(out);
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
        while (fd_index < table.capacity) : (fd_index += 1) {
            const candidate = table.at(fd_index).*;
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
        .reply => return KernelError.InvalidState,
        else => return KernelError.InvalidState,
    };
}

pub fn fdFreeCountFrom(self: anytype, owner: PrincipalId, min_fd: Fd) KernelError!usize {
    const table = try self.fdTableForActiveProcessConst(owner);
    var index = @TypeOf(self.*).fdIndex(min_fd) orelse return KernelError.InvalidState;
    var count: usize = 0;
    while (index < table.capacity) : (index += 1) {
        if (table.at(index).*.isEmpty()) count += 1;
    }
    return count;
}

fn hasReceiveFdCapacity(self: anytype, owner: PrincipalId, min_fd: Fd, needed: usize) KernelError!bool {
    const table = try self.fdTableForActiveProcessConst(owner);
    var index = @TypeOf(self.*).fdIndex(min_fd) orelse return KernelError.InvalidState;
    // Replies commonly carry no FDs. Preserve min_fd validation, but only
    // inspect as many free slots as this message needs under the state lock.
    var remaining = needed;
    if (remaining == 0) return true;
    while (index < table.capacity) : (index += 1) {
        if (!table.at(index).*.isEmpty()) continue;
        remaining -= 1;
        if (remaining == 0) return true;
    }
    return false;
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
    const index = table.index(spec.fd) orelse return KernelError.InvalidState;
    const source = table.at(index).*;
    if (source.object.isNull()) return KernelError.InvalidState;
    if (!source.rights.transfer) return KernelError.InvalidState;
    if (!isFdRightsSubset(spec.rights, source.rights)) return KernelError.InvalidState;
    if (self.kernelObjectSlotConst(source.object) == null) return KernelError.InvalidState;
    // MMIO/DMA objects are bound to the deriving principal's VA/IOVA.  IPC
    // transfer would outlive or retarget that address-space identity.
    if (self.kernelObjectIsPinnedUserObject(source.object)) return KernelError.InvalidState;
    const node = try self.prepareFdReference(source.object);
    errdefer if (node) |reserved| references.Pool.cancel(reserved);
    try self.retainKernelObject(source.object);
    if (node) |reserved| {
        reserved.prepared_target = &self.kernelObjectSlot(source.object).?.reference_state.?.holders;
        reserved.identity.position = msg.fd_count;
        reserved.message_next = msg.reference_head;
        msg.reference_head = reserved;
    }
    msg.fds[msg.fd_count] = IpcTransferredFd.init(source.object, spec.rights, spec.flags);
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
    const index = table.index(fd) orelse return KernelError.InvalidState;
    const entry = table.at(index).*;
    if (entry.object.isNull()) return KernelError.InvalidState;
    if (require_call) {
        if (!entry.rights.call) return KernelError.InvalidState;
    } else if (!entry.rights.send) {
        return KernelError.InvalidState;
    }
    var msg = try self.buildIpcMessage(owner, send, free_list);
    errdefer self.releaseIpcMessageWithFreeList(&msg, free_list);
    const queue = try self.ipcMessageQueueForSend(entry.object);
    var endpoint_guard = queue.acquire();
    defer endpoint_guard.end();
    try queue.push(msg);
    msg = .{};
    endpoint_guard.end();
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

// K serializes final reference release after endpoint/reply exclusion ends.
// Cancellation under T only records debt; its K-owned drain releases refs.
pub fn releaseIpcCallReference(self: anytype, ref: IpcReplyRef) void {
    const call = self.ipcReplySlot(ref) orelse return;
    if (call.release()) self.clearIpcReplySlot(ref);
}

pub fn ipcClaimCallReply(self: anytype, ref: IpcReplyRef) bool {
    const call = self.ipcReplySlot(ref) orelse return false;
    var reply_guard = call.exclusion.acquire();
    defer reply_guard.end();
    if (call.sent) return false;
    const t = call.caller;
    if (scheduler.claimWaitTokenCompletion(t.thread_index, t.thread_generation, t.owner, t.wait_token) == .stale) return false;
    call.sent = true;
    return true;
}

pub fn ipcFinishCallReply(self: anytype, ref: IpcReplyRef, status: u64) void {
    const call = self.ipcReplySlot(ref) orelse return;
    var reply_guard = call.exclusion.acquire();
    defer reply_guard.end();
    const t = call.caller;
    const detached = scheduler.detachIpcOutgoingLocked(t.thread_index, t.thread_generation, t.owner, &call.outgoing);
    _ = scheduler.publishClaimedWaitCompletionPreferred(t.thread_index, t.thread_generation, t.owner, t.wait_token, status, scheduler.currentCpu());
    reply_guard.end();
    if (detached) self.releaseIpcCallReference(ref);
}

pub fn releaseIpcCallAuthority(self: anytype, ref: IpcReplyRef) void {
    if (self.ipcClaimCallReply(ref)) self.ipcFinishCallReply(ref, sc.syscall_err_closed);
    if (self.ipcReplySlot(ref)) |body| {
        var reply_guard = body.exclusion.acquire();
        body.sent = true;
        reply_guard.end();
    }
    self.releaseIpcCallReference(ref);
}

pub fn ipcExtractReply(self: anytype, owner: PrincipalId, ref: IpcReplyRef, min_fd: Fd) KernelError!Fd {
    const body = self.ipcReplySlot(ref) orelse return KernelError.InvalidState;
    {
        var endpoint_guard = body.exclusion.acquire();
        defer endpoint_guard.end();
        body.retain(1);
    }
    // Local ownership spans installation and its failure cleanup, which can
    // allocate or close capabilities and must execute outside endpoint locks.
    defer self.releaseIpcCallReference(ref);
    // Extraction moves the held authority even after caller cancellation.
    // Otherwise a server could not discard it with extract+close; exercising
    // a stale authority still fails the reply completion claim.
    const object = try self.createKernelObject(.reply, .{ .reply = ref });
    // A newly created object has no reference. On failed install its payload
    // must not consume the still-bound implicit authority.
    errdefer if (self.kernelObjectSlot(object)) |slot| {
        slot.payload = .{ .none = {} };
        self.clearKernelObjectSlot(slot);
    };
    return self.installFd(owner, object, @TypeOf(self.*).replySendRights(), .{ .cloexec = true }, min_fd);
}

pub fn ipcTakeDirectReceiver(self: anytype, owner: PrincipalId, fd: Fd, require_call: bool) KernelError!?ThreadWakeTarget {
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (if (require_call) !entry.rights.call else !entry.rights.send) return KernelError.InvalidState;
    const queue = try self.ipcMessageQueueForSend(entry.object);
    var endpoint_guard = queue.acquire();
    defer endpoint_guard.end();
    // An older queued message always wins over the new direct send.
    if (!queue.isEmpty()) return null;
    const slot = self.kernelObjectSlotConst(entry.object) orelse return KernelError.InvalidState;
    const list = self.ipcWaitListForSendPayload(&slot.payload) orelse return KernelError.InvalidState;
    return list.takeReceiver();
}

pub fn ipcCreateCall(self: anytype, caller: ThreadWakeTarget) KernelError!IpcReplyRef {
    const reusable = scheduler.ipcBlock(caller.thread_index, caller.thread_generation, caller.owner);
    const body = if (reusable != null and scheduler.ipcBlockReusable(reusable.?))
        reusable.?
    else
        self.ipcReplySlot(try self.createIpcReply()).?;
    var reply_guard = body.exclusion.acquire();
    defer reply_guard.end();
    body.caller = caller;
    body.sent = false;
    body.queue_key = null;
    body.retain(2);
    return body.ref;
}

pub fn ipcCall(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    send: IpcSendMessage,
    caller: ThreadWakeTarget,
    free_list: *FreePageList,
) KernelError!IpcReplyRef {
    try self.requireActiveProcess(owner);
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.call) return KernelError.InvalidState;
    const queue = try self.ipcMessageQueueForSend(entry.object);
    var msg = try self.buildIpcMessage(owner, send, free_list);
    errdefer self.releaseIpcMessageWithFreeList(&msg, free_list);
    const ref = try self.ipcCreateCall(caller);
    errdefer {
        self.releaseIpcCallReference(ref);
        self.releaseIpcCallReference(ref);
    }
    const call = self.ipcReplySlot(ref).?;
    // A generational identity, not a retain: final endpoint close must drain
    // this queue and fail callers even while requests are outstanding.
    var endpoint_guard = @import("../sync/ipc_endpoint.zig").Pair.acquire(queue.exclusion.?, &call.exclusion);
    defer endpoint_guard.end();
    call.queue_key = @TypeOf(self.*).ipcRecvWaitKeyFromSendPayload(&self.kernelObjectSlotConst(entry.object).?.payload);
    msg.call = ref;
    queue.push(msg) catch |err| {
        msg.call = .{};
        return err;
    };
    msg = .{};
    endpoint_guard.end();
    self.closeMovedIpcSendFdsWithFreeList(owner, send.fds, free_list);
    return ref;
}

pub fn ipcCancelCall(self: anytype, ref: IpcReplyRef, free_list: *FreePageList) void {
    cancelCall(self, ref, free_list, null);
}

fn cancelCall(self: anytype, ref: IpcReplyRef, free_list: *FreePageList, cancellation: ?*@import("../ipc_call_binding.zig").Node) void {
    const call = self.ipcReplySlot(ref) orelse return;
    var reply_guard = call.exclusion.acquire();
    defer reply_guard.end();
    if (cancellation) |node| std.debug.assert(scheduler.validateIpcCancellation(node, call.caller.wait_token));
    call.sent = true;
    if (call.queue_key) |key| {
        call.queue_key = null;
        reply_guard.end();
        const found: ?*IpcQueue = switch (key.kind) {
            .endpoint => if (self.ipcEndpointSlot(.{ .index = key.index, .generation = key.generation })) |slot| &slot.queue else null,
            .channel => if (self.ipcChannelSlot(.{ .index = key.index, .generation = key.generation })) |slot| &slot.queues[key.side] else null,
            else => null,
        };
        if (found) |queue| {
            var removed: ?IpcMessage = null;
            var endpoint_guard = queue.acquire();
            if (cancellation) |node| std.debug.assert(scheduler.validateIpcCancellation(node, call.caller.wait_token));
            // Rotate exactly the original length. push reconnects each
            // attachment with its new sequence, retaining FIFO order.
            const count = queue.len;
            for (0..count) |_| {
                const msg = queue.pop().?;
                if (replyRefEqual(msg.call, ref)) {
                    self.ipc_cancel_writable_pending = true;
                    std.debug.assert(removed == null);
                    removed = msg;
                } else queue.push(msg) catch unreachable;
            }
            endpoint_guard.end();
            if (removed) |*msg| self.releaseIpcMessageWithFreeList(msg, free_list);
        }
    }
}

/// Cancellation is a dequeue and must publish the writable edge too. The
/// rare cancellation drain scans only while this debt is set; normal poll
/// storage/admission is unchanged. K keeps readiness stable across batches.
pub fn takeIpcCancellationWritableWaiters(self: anytype, out: []ThreadWakeTarget) usize {
    if (!self.ipc_cancel_writable_pending or out.len == 0) return 0;
    const writable = @import("kernel_abi_root").fd_abi.event_writable;
    var count: usize = 0;
    var endpoints = self.ipc_endpoints.iterator();
    while (endpoints.next()) |item| {
        var endpoint_guard = item.value.exclusion.acquire();
        defer endpoint_guard.end();
        if (!item.value.queue.isFull()) count += item.value.waiters.takeEvents(writable, out[count..]);
        if (count == out.len) return count;
    }
    var channels = self.ipc_channels.iterator();
    while (channels.next()) |item| {
        var endpoint_guard = item.value.exclusion.acquire();
        defer endpoint_guard.end();
        for (&item.value.queues, 0..) |*queue, side| {
            if (!queue.isFull()) count += item.value.waiters[side].takeEvents(writable, out[count..]);
            if (count == out.len) return count;
        }
    }
    if (count == 0) self.ipc_cancel_writable_pending = false;
    return count;
}

pub fn drainIpcCancellations(self: anytype, free_list: *FreePageList) void {
    while (scheduler.takePendingIpcCancellation()) |node| {
        const call: *IpcReplySlot = @ptrCast(@alignCast(node.context.?));
        const ref = call.ref;
        switch (node.kind) {
            .outgoing => cancelCall(self, ref, free_list, node),
            .implicit => {
                {
                    var endpoint_guard = call.exclusion.acquire();
                    defer endpoint_guard.end();
                    std.debug.assert(scheduler.validateIpcCancellation(node, call.caller.wait_token));
                }
                if (self.ipcClaimCallReply(ref)) self.ipcFinishCallReply(ref, sc.syscall_err_closed);
            },
            .receive => {
                const registration = call.receive_target.registration;
                const payload: KernelObjectPayload = switch (registration.kind) {
                    .endpoint => .{ .endpoint = .{ .index = registration.index, .generation = registration.generation } },
                    .channel => .{ .channel = .{ .channel = .{ .index = registration.index, .generation = registration.generation }, .side = registration.side } },
                    .reply => .{ .reply = .{ .index = registration.index, .generation = registration.generation } },
                    else => .{ .none = {} },
                };
                if (exclusionForPayload(self, &payload)) |exclusion| {
                    var endpoint_guard = exclusion.acquire();
                    defer endpoint_guard.end();
                    std.debug.assert(scheduler.validateIpcCancellation(node, call.receive_target.wait_token));
                    if (call.receive_list) |list| list.unlinkReceive(call);
                } else std.debug.assert(call.receive_list == null);
            },
            .block_owner => {},
        }
        // This is the final access to the intrusive node. A second pending
        // node in the same body independently keeps its own reference alive.
        scheduler.finishIpcCancellation(node);
        self.releaseIpcCallReference(ref);
    }
}

pub fn ipcInstallMessage(self: anytype, owner: PrincipalId, pending: *const IpcMessage, fd_capacity: usize, min_fd: Fd, free_list: *FreePageList) KernelError!IpcRecvResult {
    if (pending.fd_count > fd_capacity) return KernelError.TableFull;
    if (!try hasReceiveFdCapacity(self, owner, min_fd, pending.fd_count)) return KernelError.TableFull;

    var installed: [max_ipc_message_fds]Fd = [_]Fd{0} ** max_ipc_message_fds;
    var installed_count: usize = 0;
    errdefer {
        var i: usize = 0;
        while (i < installed_count) : (i += 1) {
            self.closeFdWithFreeList(owner, installed[i], free_list) catch {};
        }
    }

    var nodes: [max_ipc_message_fds]?*references.Node = .{null} ** max_ipc_message_fds;
    defer for (nodes) |node| if (node) |reserved| references.Pool.cancel(reserved);
    for (pending.fds[0..pending.fd_count], 0..) |item, position| nodes[position] = try self.prepareFdReference(item.object());

    var result = IpcRecvResult{
        .words = pending.words,
        .fd_count = pending.fd_count,
    };
    var i: usize = 0;
    while (i < pending.fd_count) : (i += 1) {
        const item = pending.fds[i];
        const new_fd = try self.installFdPrepared(owner, item.object(), item.rights(), item.flags(), min_fd, nodes[i]);
        nodes[i] = null;
        installed[installed_count] = new_fd;
        installed_count += 1;
        result.fds[i] = .{
            .fd = new_fd,
            .rights = item.rights(),
            .flags = item.flags(),
        };
    }

    return result;
}

// K keeps this observation valid until the frontend dequeues. An existing
// implicit authority excludes another CALL, but ordinary messages carry no
// replacement authority and must remain usable for nested service protocols.
pub fn ipcRecvPendingCall(self: anytype, owner: PrincipalId, fd: Fd) KernelError!bool {
    const entry = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
    if (!entry.rights.recv) return KernelError.InvalidState;
    const queue = try self.ipcMessageQueueForRecv(entry.object);
    var endpoint_guard = queue.acquire();
    defer endpoint_guard.end();
    const pending = queue.peek() orelse return false;
    return !pending.call.isNull();
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
    var endpoint_guard = queue.acquire();
    const pending = (queue.peek() orelse {
        endpoint_guard.end();
        return KernelError.MailboxEmpty;
    }).*;
    endpoint_guard.end();
    // K excludes other dequeuers/revoke. Fast writers may append, but cannot
    // change this head while attachment installation allocates or rolls back.
    var result = try self.ipcInstallMessage(owner, &pending, fd_capacity, min_fd, free_list);

    endpoint_guard = queue.acquire();
    var msg = queue.pop().?;
    endpoint_guard.end();
    result.call = msg.call;
    msg.call = .{};
    self.releaseIpcMessageWithFreeList(&msg, free_list);
    if (self.ipcReplySlot(result.call)) |call| {
        var reply_guard = call.exclusion.acquire();
        defer reply_guard.end();
        call.queue_key = null;
    }
    return result;
}

pub fn ipcRecvWithWritableWake(
    self: anytype,
    owner: PrincipalId,
    fd: Fd,
    fd_capacity: usize,
    min_fd: Fd,
    free_list: *FreePageList,
    writable_wake_out: []ThreadWakeTarget,
) KernelError!IpcRecvOutcome {
    const message = try self.ipcRecv(owner, fd, fd_capacity, min_fd, free_list);
    // Syscall dispatch holds the global KernelState lock across dequeue and
    // waiter extraction, so no sender can refill the queue between these two
    // operations.  A successful pop therefore publishes the exact writable
    // edge created by this receive.
    const writable_wake_count = try self.takeIpcWritableWaitersForRecvFd(
        owner,
        fd,
        writable_wake_out,
    );
    return .{
        .message = message,
        .writable_wake_count = writable_wake_count,
    };
}
