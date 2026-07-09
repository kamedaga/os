const std = @import("std");
pub const capsule = @import("../capsule.zig");
const table = @import("table.zig");
pub const initial_process_count: usize = 8;
pub const initial_process_capacity: usize = 32;
pub const process_count: usize = initial_process_capacity;
pub const max_process_slots: usize = 65536;
pub const initial_thread_capacity: usize = 32;
pub const max_thread_slots: usize = 65536;
pub const device_count: usize = 1;
pub const device_principal_raw: u32 = @intCast(max_process_slots);
pub const principal_count: usize = process_count + device_count;

pub const PrincipalRaw = u32;

fn PrincipalIdType() type {
    const field_names = [_][]const u8{"Device0"};
    const field_values = [_]PrincipalRaw{device_principal_raw};
    return @Enum(PrincipalRaw, .nonexhaustive, &field_names, &field_values);
}

pub const PrincipalId = PrincipalIdType();
const default_process_principal: PrincipalId = processPrincipalFromIndex(0) orelse unreachable;

const process_labels = blk: {
    var labels: [process_count][]const u8 = undefined;
    for (0..process_count) |i| {
        labels[i] = std.fmt.comptimePrint("Process{}", .{i});
    }
    break :blk labels;
};

pub fn processPrincipalFromIndex(index: usize) ?PrincipalId {
    if (index >= max_process_slots) return null;
    return @enumFromInt(@as(PrincipalRaw, @intCast(index)));
}

pub fn processIndexFromPrincipal(principal: PrincipalId) ?usize {
    const index: usize = @intFromEnum(principal);
    if (index >= max_process_slots) return null;
    return index;
}

pub fn principalLabel(principal: PrincipalId) []const u8 {
    if (processIndexFromPrincipal(principal)) |index| {
        if (index < process_labels.len) return process_labels[index];
        return "Process";
    }
    if (principal == .Device0) return "Device0";
    return "Unknown";
}

fn isProcessPrincipal(principal: PrincipalId) bool {
    return processIndexFromPrincipal(principal) != null;
}

pub const CapsuleKind = capsule.CapsuleKind;
pub const CapsuleRights = capsule.Rights;
pub const CapsuleMetadata = capsule.Metadata;
pub const CapsuleSnapshot = capsule.Snapshot;
pub const CapsuleDmaDirection = capsule.DmaDirection;
pub const CapsuleIrqKind = capsule.IrqKind;

pub const MapProt = struct {
    read: bool = false,
    write: bool = false,
    exec: bool = false,
    pkey: u4 = 0,
};

pub const EndpointRoute = struct {
    endpoint_id: u64,
    target: PrincipalId,
};

pub const Region = struct {
    id: u64,
};

pub const ProcessDescriptor = struct {
    active: bool = false,
    principal: PrincipalId = @enumFromInt(0),
    label: []const u8 = "",
    bootstrap_owner: bool = false,
    faulted: bool = false,
    fault_vector: u8 = 0,
};

pub const ProcessStatus = struct {
    active: bool = false,
    faulted: bool = false,
    fault_vector: u8 = 0,
};

pub const DebugProcessLifecycleReason = enum(u8) {
    create,
    ensure,
    fault,
    exit,
    remove,
};

pub const KernelError = error{
    RegionNotFound,
    EndpointNotFound,
    MailboxEmpty,
    RevokeOverflow,
    NoDmaRight,
    InvalidState,
    TableFull,
    EmptyRegionSet,
    TooManyRegions,
    TooManyFreePages,
    TooManyFreeRanges,
    OutOfFreePages,
};

pub const DmaDeviceId = u64;
pub const invalid_dma_device_id: DmaDeviceId = 0;

pub const Fd = u32;
pub const fd_table_entries: usize = 256;
pub const max_fd_objects: usize = 4096;
pub const max_pipes: usize = 256;
pub const pipe_buffer_bytes: usize = 4096;
pub const fd_known_flags_mask: u32 = (@as(u32, 1) << 4) - 1;
pub const fd_known_rights_mask: u64 = (@as(u64, 1) << 45) - 1;

pub const FdFlags = packed struct(u32) {
    cloexec: bool = false,
    nonblock: bool = false,
    inherit: bool = false,
    private: bool = false,
    _reserved: u28 = 0,
};

pub fn fdFlagsFromBits(bits: u32) FdFlags {
    return @bitCast(bits & fd_known_flags_mask);
}

pub fn fdFlagsToBits(flags: FdFlags) u32 {
    return @as(u32, @bitCast(flags)) & fd_known_flags_mask;
}

pub const FdRights = packed struct(u64) {
    inspect: bool = false,
    dup: bool = false,
    transfer: bool = false,
    wait: bool = false,
    poll: bool = false,
    set_flags: bool = false,
    close: bool = false,
    send: bool = false,
    recv: bool = false,
    call: bool = false,
    accept: bool = false,
    bind: bool = false,
    endpoint_signal: bool = false,
    map_read: bool = false,
    map_write: bool = false,
    map_exec: bool = false,
    resize: bool = false,
    share: bool = false,
    pager_attach: bool = false,
    pager_fault: bool = false,
    spawn: bool = false,
    start: bool = false,
    kill: bool = false,
    debug: bool = false,
    map_into: bool = false,
    set_context: bool = false,
    process_signal: bool = false,
    query: bool = false,
    config_read: bool = false,
    config_write: bool = false,
    derive_mmio: bool = false,
    derive_dma: bool = false,
    derive_irq: bool = false,
    mmio_map_read: bool = false,
    mmio_map_write: bool = false,
    cpu_read: bool = false,
    cpu_write: bool = false,
    dma_read: bool = false,
    dma_write: bool = false,
    irq_wait: bool = false,
    irq_ack: bool = false,
    bus_master: bool = false,
    read: bool = false,
    write: bool = false,
    revoke: bool = false,
    _reserved: u19 = 0,
};

pub fn fdRightsFromBits(bits: u64) FdRights {
    return @bitCast(bits & fd_known_rights_mask);
}

pub fn fdRightsToBits(rights: FdRights) u64 {
    return @as(u64, @bitCast(rights)) & fd_known_rights_mask;
}

pub fn isFdRightsSubset(child: FdRights, parent: FdRights) bool {
    const child_bits = fdRightsToBits(child);
    const parent_bits = fdRightsToBits(parent);
    return (child_bits & ~parent_bits) == 0;
}

pub const KernelObjectKind = enum(u16) {
    none = 0,
    process = 1,
    thread = 2,
    event = 3,
    vmo = 4,
    endpoint = 5,
    channel = 6,
    reply = 7,
    device = 8,
    mmio_region = 9,
    dma_buffer = 10,
    dma_mapping = 11,
    irq = 12,
    timer = 13,
    serial = 14,
    schedctl = 15,
    sched_event = 16,
    pipe = 17,
};

pub const TaskObjectState = enum(u8) {
    active = 1,
    exited = 2,
    killed = 3,
    stopped = 4,
    continued = 5,
};

pub const ProcessObject = struct {
    principal_raw: PrincipalRaw = 0,
    state: TaskObjectState = .active,
    exit_code: u32 = 0,
};

pub const ThreadObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
    thread_index: u32 = 0,
    thread_generation: u32 = 0,
    state: TaskObjectState = .active,
    exit_code: u32 = 0,
};

pub const DeviceObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
    device: DmaDeviceId = 0,
};

pub const MmioRegionObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
    device: DmaDeviceId = 0,
    bar_index: u32 = 0,
    paddr: u64 = 0,
    user_va: u64 = 0,
    size: u64 = 0,
    flags: u32 = 0,
};

pub const DmaBufferObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
    device: DmaDeviceId = 0,
    user_va: u64 = 0,
    iova: u64 = 0,
    size: u64 = 0,
    flags: u32 = 0,
};

pub const DmaMappingObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
    device: DmaDeviceId = 0,
    user_va: u64 = 0,
    iova: u64 = 0,
    size: u64 = 0,
    direction: CapsuleDmaDirection = .bidirectional,
    flags: u32 = 0,
};

pub const IrqObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
    device: DmaDeviceId = 0,
    kind: CapsuleIrqKind = .auto,
    vector: u32 = 0,
    event_count: u64 = 0,
    flags: u32 = 0,
};

pub const IrqPublishSlot = struct {
    active: u8 = 0,
    generation: u32 = 0,
    owner_principal_raw: PrincipalRaw = 0,
    kind: u8 = 0,
    vector: u32 = 0,
    event_count: u64 = 0,
};

pub const TimerObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
    deadline_tick: u64 = 0,
    interval_ticks: u64 = 0,
    flags: u32 = 0,
};

pub const TimerFdState = struct {
    remaining_ticks: u64 = 0,
    interval_ticks: u64 = 0,
};

pub const SerialObject = struct {
    stream: u8 = 0,
};

pub const SchedulerControlObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
};

pub const SchedulerEventObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
};

pub const PipeRef = struct {
    index: u32 = 0,
    generation: u32 = 0,

    pub fn isNull(self: PipeRef) bool {
        return self.generation == 0;
    }
};

pub const PipeEndpointObject = struct {
    pipe: PipeRef = .{},
    write: bool = false,
};

pub const PipePair = struct {
    read: Fd = 0,
    write: Fd = 0,
};

pub const PipeIoError = error{
    InvalidState,
    NotReady,
    Closed,
};

pub const KernelObjectRef = struct {
    kind: KernelObjectKind = .none,
    index: u32 = 0,
    generation: u32 = 0,

    pub fn isNull(self: KernelObjectRef) bool {
        return self.kind == .none;
    }
};

pub const KernelObjectPayload = union(KernelObjectKind) {
    none: void,
    process: ProcessObject,
    thread: ThreadObject,
    event: u64,
    vmo: NativeVmoRef,
    endpoint: IpcEndpointRef,
    channel: IpcChannelHandle,
    reply: IpcReplyRef,
    device: DeviceObject,
    mmio_region: MmioRegionObject,
    dma_buffer: DmaBufferObject,
    dma_mapping: DmaMappingObject,
    irq: IrqObject,
    timer: TimerObject,
    serial: SerialObject,
    schedctl: SchedulerControlObject,
    sched_event: SchedulerEventObject,
    pipe: PipeEndpointObject,
};

pub const KernelObjectSlot = struct {
    kind: KernelObjectKind = .none,
    generation: u32 = 1,
    ref_count: u32 = 0,
    payload: KernelObjectPayload = .{ .none = {} },
};

pub const FdEntry = struct {
    object: KernelObjectRef = .{},
    rights: FdRights = .{},
    flags: FdFlags = .{},
    offset: u64 = 0,

    pub fn isEmpty(self: *const FdEntry) bool {
        return self.object.isNull();
    }
};

pub const FdInfo = struct {
    kind: KernelObjectKind = .none,
    rights_bits: u64 = 0,
    flags_bits: u32 = 0,
    size_bytes: u64 = 0,
    extra: u64 = 0,
};

pub const FdTable = struct {
    entries: [fd_table_entries]FdEntry = [_]FdEntry{.{}} ** fd_table_entries,
};

pub const FdTransferMode = enum(u8) {
    copy,
    move,
};

pub const max_ipc_endpoints: usize = 1024;
pub const max_ipc_channels: usize = 512;
pub const max_ipc_replies: usize = 1024;
pub const max_ipc_queue_messages: usize = 16;
pub const max_ipc_message_fds: usize = 19;
pub const max_ipc_waiters: usize = 256;

pub const IpcEndpointRef = struct {
    index: u32 = 0,
    generation: u32 = 0,

    pub fn isNull(self: IpcEndpointRef) bool {
        return self.generation == 0;
    }
};

pub const IpcChannelRef = struct {
    index: u32 = 0,
    generation: u32 = 0,

    pub fn isNull(self: IpcChannelRef) bool {
        return self.generation == 0;
    }
};

pub const IpcChannelHandle = struct {
    channel: IpcChannelRef = .{},
    side: u8 = 0,
};

pub const IpcReplyRef = struct {
    index: u32 = 0,
    generation: u32 = 0,

    pub fn isNull(self: IpcReplyRef) bool {
        return self.generation == 0;
    }
};

pub const IpcWaitKey = struct {
    kind: KernelObjectKind = .none,
    index: u32 = 0,
    generation: u32 = 0,
    side: u8 = 0,

    fn matches(self: IpcWaitKey, other: IpcWaitKey) bool {
        return self.kind == other.kind and
            self.index == other.index and
            self.generation == other.generation and
            self.side == other.side;
    }
};

pub const IpcWaiter = struct {
    active: bool = false,
    owner: PrincipalId = default_process_principal,
    thread_index: u32 = 0,
    thread_generation: u32 = 0,
    pollfd_va: u64 = 0,
    recv_msg_va: u64 = 0,
    recv_fd: Fd = 0,
    recv_fd_capacity: u8 = 0,
    events: u64 = 0,
    min_write_bytes: u64 = 0,
    key: IpcWaitKey = .{},
};

pub const ThreadWakeTarget = struct {
    owner: PrincipalId = default_process_principal,
    thread_index: usize = 0,
    thread_generation: u32 = 0,
    hint_only: bool = false,
    pollfd_va: u64 = 0,
    recv_msg_va: u64 = 0,
    recv_fd: Fd = 0,
    recv_fd_capacity: u8 = 0,
    revents: u64 = 0,
};

pub const TaskFdWaiter = struct {
    active: bool = false,
    principal_raw: PrincipalRaw = 0,
    owner: PrincipalId = default_process_principal,
    pollfd_va: u64 = 0,
    events: u64 = 0,
    thread_index: u32 = 0,
    thread_generation: u32 = 0,
};

pub const max_task_fd_waiters: usize = 64;
pub const max_ipc_object_waiters: usize = 8;

pub const PipeSlot = struct {
    active: bool = false,
    generation: u32 = 1,
    read_refs: u32 = 0,
    write_refs: u32 = 0,
    head: u16 = 0,
    len: u16 = 0,
    data: [pipe_buffer_bytes]u8 = [_]u8{0} ** pipe_buffer_bytes,
    waiters: [2]IpcWaitList = .{ .{}, .{} },
};

pub const IpcWaitList = struct {
    waiters: [max_ipc_object_waiters]IpcWaiter = [_]IpcWaiter{.{}} ** max_ipc_object_waiters,
    handoff_hint_valid: bool = false,
    handoff_hint: ThreadWakeTarget = .{},

    pub fn rememberHandoffHint(
        self: *IpcWaitList,
        owner: PrincipalId,
        thread_index: usize,
        thread_generation: u32,
    ) void {
        self.handoff_hint = .{
            .owner = owner,
            .thread_index = thread_index,
            .thread_generation = thread_generation,
            .hint_only = true,
        };
        self.handoff_hint_valid = true;
    }

    pub fn register(
        self: *IpcWaitList,
        key: IpcWaitKey,
        owner: PrincipalId,
        pollfd_va: u64,
        recv_msg_va: u64,
        recv_fd: Fd,
        recv_fd_capacity: u8,
        events: u64,
        min_write_bytes: u64,
        thread_index: usize,
        thread_generation: u32,
    ) KernelError!void {
        if (thread_index > std.math.maxInt(u32)) return KernelError.InvalidState;
        const thread_index_u32: u32 = @intCast(thread_index);
        self.rememberHandoffHint(owner, thread_index, thread_generation);
        var free_index: ?usize = null;
        var i: usize = 0;
        while (i < self.waiters.len) : (i += 1) {
            const waiter = &self.waiters[i];
            if (!waiter.active) {
                if (free_index == null) free_index = i;
                continue;
            }
            if (waiter.thread_index == thread_index_u32 and waiter.thread_generation == thread_generation) {
                waiter.events |= events;
                waiter.key = key;
                waiter.owner = owner;
                waiter.pollfd_va = pollfd_va;
                waiter.recv_msg_va = recv_msg_va;
                waiter.recv_fd = recv_fd;
                waiter.recv_fd_capacity = recv_fd_capacity;
                if (min_write_bytes > waiter.min_write_bytes) waiter.min_write_bytes = min_write_bytes;
                return;
            }
        }
        const target = free_index orelse return KernelError.TableFull;
        self.waiters[target] = .{
            .active = true,
            .owner = owner,
            .thread_index = thread_index_u32,
            .thread_generation = thread_generation,
            .pollfd_va = pollfd_va,
            .recv_msg_va = recv_msg_va,
            .recv_fd = recv_fd,
            .recv_fd_capacity = recv_fd_capacity,
            .events = events,
            .min_write_bytes = min_write_bytes,
            .key = key,
        };
    }

    pub fn handoffHint(self: *const IpcWaitList) ?ThreadWakeTarget {
        if (!self.handoff_hint_valid) return null;
        return self.handoff_hint;
    }

    pub fn unregister(self: *IpcWaitList, thread_index: usize, thread_generation: u32) void {
        if (thread_index > std.math.maxInt(u32)) return;
        const thread_index_u32: u32 = @intCast(thread_index);
        for (self.waiters[0..]) |*waiter| {
            if (!waiter.active) continue;
            if (waiter.thread_index == thread_index_u32 and waiter.thread_generation == thread_generation) {
                waiter.* = .{};
            }
        }
    }

    pub fn takeReadable(self: *IpcWaitList, out: []ThreadWakeTarget) usize {
        const fd_abi = @import("kernel_abi_root").fd_abi;
        return self.takeEvents(fd_abi.event_readable, out);
    }

    pub fn takeEvents(self: *IpcWaitList, ready_events: u64, out: []ThreadWakeTarget) usize {
        return self.takeEventsWithWritableBytes(ready_events, std.math.maxInt(u64), out);
    }

    pub fn takeEventsWithWritableBytes(self: *IpcWaitList, ready_events: u64, writable_bytes: u64, out: []ThreadWakeTarget) usize {
        const fd_abi = @import("kernel_abi_root").fd_abi;
        var count: usize = 0;
        for (self.waiters[0..]) |*waiter| {
            if (!waiter.active) continue;
            var revents = ready_events & (waiter.events | fd_abi.event_error | fd_abi.event_hangup);
            if ((revents & fd_abi.event_writable) != 0 and waiter.min_write_bytes != 0 and writable_bytes < waiter.min_write_bytes) {
                revents &= ~fd_abi.event_writable;
            }
            if (revents == 0) continue;
            const target = ThreadWakeTarget{
                .owner = waiter.owner,
                .thread_index = waiter.thread_index,
                .thread_generation = waiter.thread_generation,
                .pollfd_va = waiter.pollfd_va,
                .recv_msg_va = waiter.recv_msg_va,
                .recv_fd = waiter.recv_fd,
                .recv_fd_capacity = waiter.recv_fd_capacity,
                .revents = revents,
            };
            waiter.* = .{};
            if (count < out.len) {
                out[count] = target;
                count += 1;
            }
        }
        return count;
    }
};

pub const IpcTransferredFd = struct {
    object: KernelObjectRef = .{},
    rights: FdRights = .{},
    flags: FdFlags = .{},
};

pub const IpcMessage = struct {
    active: bool = false,
    sender: PrincipalId = default_process_principal,
    words: [4]u64 = .{ 0, 0, 0, 0 },
    fd_count: u8 = 0,
    fds: [max_ipc_message_fds]IpcTransferredFd = [_]IpcTransferredFd{.{}} ** max_ipc_message_fds,
};

pub const IpcQueue = struct {
    messages: [max_ipc_queue_messages]IpcMessage = [_]IpcMessage{.{}} ** max_ipc_queue_messages,
    head: u8 = 0,
    len: u8 = 0,

    pub fn isEmpty(self: *const IpcQueue) bool {
        return self.len == 0;
    }

    pub fn isFull(self: *const IpcQueue) bool {
        return self.len >= max_ipc_queue_messages;
    }

    pub fn slotIndex(self: *const IpcQueue, offset: usize) usize {
        return (@as(usize, self.head) + offset) % max_ipc_queue_messages;
    }

    pub fn peek(self: *const IpcQueue) ?*const IpcMessage {
        if (self.len == 0) return null;
        return &self.messages[self.head];
    }

    pub fn push(self: *IpcQueue, msg: IpcMessage) KernelError!void {
        if (self.isFull()) return KernelError.TableFull;
        const index = self.slotIndex(self.len);
        self.messages[index] = msg;
        self.messages[index].active = true;
        self.len += 1;
    }

    pub fn pop(self: *IpcQueue) ?IpcMessage {
        if (self.len == 0) return null;
        const index = self.head;
        const msg = self.messages[index];
        self.messages[index] = .{};
        self.head = @intCast((@as(usize, self.head) + 1) % max_ipc_queue_messages);
        self.len -= 1;
        if (self.len == 0) self.head = 0;
        return msg;
    }
};

pub const IpcEndpointSlot = struct {
    active: bool = false,
    generation: u32 = 1,
    queue: IpcQueue = .{},
    waiters: IpcWaitList = .{},
};

pub const IpcChannelSlot = struct {
    active: bool = false,
    generation: u32 = 1,
    ref_count: u8 = 0,
    queues: [2]IpcQueue = .{ .{}, .{} },
    waiters: [2]IpcWaitList = .{ .{}, .{} },
};

pub const IpcReplySlot = struct {
    active: bool = false,
    generation: u32 = 1,
    sent: bool = false,
    queue: IpcQueue = .{},
    waiters: IpcWaitList = .{},
};

pub const IpcSendFd = struct {
    fd: Fd,
    rights: FdRights,
    flags: FdFlags = .{},
    move: bool = false,
};

pub const IpcSendMessage = struct {
    words: [4]u64 = .{ 0, 0, 0, 0 },
    fds: []const IpcSendFd = &.{},
};

pub const IpcRecvFd = struct {
    fd: Fd = 0,
    rights: FdRights = .{},
    flags: FdFlags = .{},
};

pub const IpcRecvResult = struct {
    words: [4]u64 = .{ 0, 0, 0, 0 },
    fd_count: usize = 0,
    fds: [max_ipc_message_fds]IpcRecvFd = [_]IpcRecvFd{.{}} ** max_ipc_message_fds,
};

pub const native_page_size: u64 = 4096;
pub const max_native_vmos: usize = 32768;
pub const max_vmas_per_process: usize = 8192;
pub const max_native_cow_tables: usize = max_vmas_per_process;

pub const NativeVmoKind = enum(u8) {
    none = 0,
    anonymous = 1,
};

pub const NativeVmoRef = struct {
    index: u32 = 0,
    generation: u32 = 0,

    pub fn isNull(self: NativeVmoRef) bool {
        return self.generation == 0;
    }
};

pub const NativeCowTableRef = struct {
    index: u32 = 0,
    generation: u32 = 0,

    pub fn isNull(self: NativeCowTableRef) bool {
        return self.generation == 0;
    }
};

pub const NativeVmoSlot = struct {
    kind: NativeVmoKind = .none,
    generation: u32 = 1,
    size_bytes: u64 = 0,
    page_count: u32 = 0,
    page_store_start: u32 = 0,
    has_page_store: bool = false,
    ref_count: u32 = 0,
    parent: NativeVmoRef = .{},
    parent_offset: u64 = 0,
};

pub const NativeCowTableSlot = struct {
    active: bool = false,
    generation: u32 = 1,
    ref_count: u32 = 0,
    page_count: u32 = 0,
    page_store_start: u32 = 0,
};

pub const VmaProt = packed struct(u8) {
    read: bool = false,
    write: bool = false,
    exec: bool = false,
    pkey: u4 = 0,
    _reserved: u1 = 0,
};

pub const MmapFlags = packed struct(u32) {
    fixed: bool = false,
    fixed_noreplace: bool = false,
    private: bool = false,
    shared: bool = false,
    anonymous: bool = false,
    noreserve: bool = false,
    fork_cow: bool = false,
    _reserved0: u1 = 0,
    pkey: u4 = 0,
    _reserved1: u20 = 0,
};

pub const VmaEntry = struct {
    active: bool = false,
    start_va: u64 = 0,
    size_bytes: u64 = 0,
    prot: VmaProt = .{},
    flags: MmapFlags = .{},
    vmo: NativeVmoRef = .{},
    vmo_offset: u64 = 0,
    cow_table: NativeCowTableRef = .{},
    cow_page_offset: u32 = 0,

    pub fn endVa(self: *const VmaEntry) u64 {
        return self.start_va + self.size_bytes;
    }
};

pub const VmaTable = struct {
    entries: [max_vmas_per_process]VmaEntry = [_]VmaEntry{.{}} ** max_vmas_per_process,
    active_indices: [max_vmas_per_process]u16 = [_]u16{0} ** max_vmas_per_process,
    next_user_map_va: u64 = 0,
    active_count: usize = 0,
};

pub const NativeVmaFaultMapping = struct {
    paddr: u64,
    prot: MapProt,
    invalidate_start_va: u64 = 0,
    invalidate_size_bytes: u64 = 0,
};

pub fn vmObjectBackingFreePageCount() u64 {
    var pages: u64 = 0;
    var i: usize = 0;
    while (i < vmo_backing_page_store_free_range_len) : (i += 1) {
        pages += vmo_backing_page_store_free_ranges[i].len;
    }
    return pages;
}

pub const EndpointTable = struct {
    const max_caps = 8;

    caps: [max_caps]EndpointRoute = undefined,
    len: usize = 0,

    pub fn add(self: *EndpointTable, cap: EndpointRoute) KernelError!void {
        if (self.findIndex(cap.endpoint_id)) |index| {
            self.caps[index] = cap;
            return;
        }
        if (self.len >= self.caps.len) return KernelError.TableFull;
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn find(self: *const EndpointTable, endpoint_id: u64) ?*const EndpointRoute {
        if (self.findIndex(endpoint_id)) |index| {
            return &self.caps[index];
        }
        return null;
    }

    fn findIndex(self: *const EndpointTable, endpoint_id: u64) ?usize {
        var i: usize = 0;
        const limit = @min(self.len, self.caps.len);
        while (i < limit) : (i += 1) {
            if (self.caps[i].endpoint_id == endpoint_id) return i;
        }
        return null;
    }
};

pub const PublishedEndpointTable = struct {
    const max_caps = 64;

    caps: [max_caps]EndpointRoute = undefined,
    len: usize = 0,

    pub fn publish(self: *PublishedEndpointTable, cap: EndpointRoute) KernelError!void {
        if (self.findIndex(cap.endpoint_id)) |index| {
            self.caps[index] = cap;
            return;
        }
        if (self.len >= self.caps.len) return KernelError.TableFull;
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn find(self: *const PublishedEndpointTable, endpoint_id: u64) ?*const EndpointRoute {
        if (self.findIndex(endpoint_id)) |index| {
            return &self.caps[index];
        }
        return null;
    }

    pub fn removeTarget(self: *PublishedEndpointTable, target: PrincipalId) bool {
        var removed = false;
        var read_index: usize = 0;
        var write_index: usize = 0;
        while (read_index < self.len) : (read_index += 1) {
            const cap = self.caps[read_index];
            if (cap.target == target) {
                removed = true;
                continue;
            }
            if (write_index != read_index) self.caps[write_index] = cap;
            write_index += 1;
        }
        self.len = write_index;
        return removed;
    }

    fn findIndex(self: *const PublishedEndpointTable, endpoint_id: u64) ?usize {
        var i: usize = 0;
        const limit = @min(self.len, self.caps.len);
        while (i < limit) : (i += 1) {
            if (self.caps[i].endpoint_id == endpoint_id) return i;
        }
        return null;
    }
};

pub const max_vmo_backing_pages: usize = 131072;
pub const max_vmo_backing_store_pages: usize = 1048576;
pub const max_vmo_backing_store_free_ranges: usize = 1024;

pub var empty_vmo_backing_page_store: [0]u64 = .{};
pub var vmo_backing_page_store: []u64 = empty_vmo_backing_page_store[0..];
pub var vmo_backing_page_store_next: usize = 0;

pub const VmoBackingStoreFreeRange = struct {
    start: u32 = 0,
    len: u32 = 0,
};

pub var empty_vmo_backing_page_store_free_ranges: [0]VmoBackingStoreFreeRange = .{};
pub var vmo_backing_page_store_free_ranges: []VmoBackingStoreFreeRange = empty_vmo_backing_page_store_free_ranges[0..];
pub var vmo_backing_page_store_free_range_len: usize = 0;

pub fn removeVmoBackingFreeRange(index: usize) void {
    var i = index + 1;
    while (i < vmo_backing_page_store_free_range_len) : (i += 1) {
        vmo_backing_page_store_free_ranges[i - 1] = vmo_backing_page_store_free_ranges[i];
    }
    vmo_backing_page_store_free_range_len -= 1;
}

pub fn insertVmoBackingFreeRange(start: u32, len: u32) bool {
    if (len == 0) return true;
    var merged_start = start;
    var merged_len = len;
    var i: usize = 0;
    while (i < vmo_backing_page_store_free_range_len) {
        const range = vmo_backing_page_store_free_ranges[i];
        const range_end = range.start + range.len;
        const merged_end = merged_start + merged_len;
        if (range_end < merged_start or merged_end < range.start) {
            i += 1;
            continue;
        }
        if (range.start < merged_start) merged_start = range.start;
        const new_end = if (range_end > merged_end) range_end else merged_end;
        merged_len = new_end - merged_start;
        removeVmoBackingFreeRange(i);
    }
    if (vmo_backing_page_store_free_range_len >= vmo_backing_page_store_free_ranges.len) return false;
    vmo_backing_page_store_free_ranges[vmo_backing_page_store_free_range_len] = .{
        .start = merged_start,
        .len = merged_len,
    };
    vmo_backing_page_store_free_range_len += 1;
    return true;
}

pub fn allocEmptyVmoBackingPageStore(page_count: usize) ?u32 {
    if (page_count == 0 or page_count > max_vmo_backing_pages) return null;
    var start: usize = 0;
    var free_index: ?usize = null;
    var i: usize = 0;
    while (i < vmo_backing_page_store_free_range_len) : (i += 1) {
        if (vmo_backing_page_store_free_ranges[i].len < page_count) continue;
        start = vmo_backing_page_store_free_ranges[i].start;
        free_index = i;
        break;
    }
    if (free_index) |index| {
        const consumed: u32 = @intCast(page_count);
        vmo_backing_page_store_free_ranges[index].start += consumed;
        vmo_backing_page_store_free_ranges[index].len -= consumed;
        if (vmo_backing_page_store_free_ranges[index].len == 0) removeVmoBackingFreeRange(index);
    } else {
        if (vmo_backing_page_store_next + page_count > vmo_backing_page_store.len) return null;
        start = vmo_backing_page_store_next;
        vmo_backing_page_store_next += page_count;
    }
    return @intCast(start);
}

pub fn vmoBackingPageStorePaddr(start: u32, page_count: u32, page_index: usize) ?u64 {
    if (page_index >= page_count) return null;
    const store_index = @as(usize, start) + page_index;
    if (store_index >= vmo_backing_page_store.len) return null;
    const paddr = vmo_backing_page_store[store_index];
    if ((paddr & 0xFFF) != 0) return null;
    return paddr;
}

pub fn setVmoBackingPageStorePaddr(start: u32, page_count: u32, page_index: usize, paddr: u64) bool {
    if ((paddr & 0xFFF) != 0) return false;
    if (page_index >= page_count) return false;
    const store_index = @as(usize, start) + page_index;
    if (store_index >= vmo_backing_page_store.len) return false;
    vmo_backing_page_store[store_index] = paddr;
    return true;
}

pub fn freeVmoBackingPageStore(start: u32, page_count: u32) bool {
    if (page_count == 0) return true;
    const start_usize: usize = @intCast(start);
    const count_usize: usize = @intCast(page_count);
    if (start_usize + count_usize > vmo_backing_page_store.len) return false;
    @memset(vmo_backing_page_store[start_usize .. start_usize + count_usize], 0);
    return insertVmoBackingFreeRange(start, @intCast(page_count));
}

pub fn resetVmoBackingPageStore() void {
    @memset(vmo_backing_page_store[0..], 0);
    @memset(vmo_backing_page_store_free_ranges[0..], .{});
    vmo_backing_page_store_next = 0;
    vmo_backing_page_store_free_range_len = 0;
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(vmo_backing_page_store), &vmo_backing_page_store));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(vmo_backing_page_store_next), &vmo_backing_page_store_next));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(vmo_backing_page_store_free_ranges), &vmo_backing_page_store_free_ranges));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(vmo_backing_page_store_free_range_len), &vmo_backing_page_store_free_range_len));
    return end;
}

fn runtimeStorageSlice(
    comptime T: type,
    storage: []align(4096) u8,
    cursor: *usize,
    count: usize,
) ?[]T {
    return table.runtimeStorageSlice(T, storage, cursor, count);
}

pub fn runtimeStorageBytes() usize {
    var cursor: usize = 0;
    cursor = std.mem.alignForward(usize, cursor, @alignOf(u64));
    cursor += @sizeOf(u64) * max_vmo_backing_store_pages;
    cursor = std.mem.alignForward(usize, cursor, @alignOf(VmoBackingStoreFreeRange));
    cursor += @sizeOf(VmoBackingStoreFreeRange) * max_vmo_backing_store_free_ranges;
    return std.mem.alignForward(usize, cursor, 4096);
}

pub fn initRuntimeStorage(storage: []align(4096) u8) bool {
    var cursor: usize = 0;
    vmo_backing_page_store = runtimeStorageSlice(u64, storage, &cursor, max_vmo_backing_store_pages) orelse return false;
    vmo_backing_page_store_free_ranges = runtimeStorageSlice(VmoBackingStoreFreeRange, storage, &cursor, max_vmo_backing_store_free_ranges) orelse return false;

    @memset(vmo_backing_page_store, 0);
    @memset(vmo_backing_page_store_free_ranges, .{});
    vmo_backing_page_store_next = 0;
    vmo_backing_page_store_free_range_len = 0;
    return true;
}

pub const RegionFreeRange = struct {
    region_id: u64,
    len: usize,
    physical_start: u64,
};

pub const FreePageList = struct {
    pub const max_ranges = 65536;

    len: usize = 0,
    ranges: [max_ranges]RegionFreeRange = undefined,
    range_len: usize = 0,

    fn rangeEnd(range: *const RegionFreeRange) u64 {
        return range.physical_start + (@as(u64, @intCast(range.len)) * 4096);
    }

    fn removeRangeAt(self: *FreePageList, index: usize) void {
        var r = index + 1;
        while (r < self.range_len) : (r += 1) {
            self.ranges[r - 1] = self.ranges[r];
        }
        self.range_len -= 1;
    }

    pub fn appendRegion(
        self: *FreePageList,
        region_id: u64,
        physical_start: u64,
        number_of_pages: u64,
    ) KernelError!void {
        var i: u64 = 0;
        while (i < number_of_pages) : (i += 1) {
            try self.appendPage(region_id, physical_start + (i * 4096));
        }
    }

    pub fn appendPage(self: *FreePageList, region_id: u64, paddr: u64) KernelError!void {
        const page_end = paddr + 4096;
        var extend_before: ?usize = null;
        var extend_after: ?usize = null;
        var i: usize = 0;
        while (i < self.range_len) : (i += 1) {
            const range = &self.ranges[i];
            if (range.region_id != region_id) continue;
            const start = range.physical_start;
            const end = rangeEnd(range);
            if (paddr >= start and paddr < end) return KernelError.InvalidState;
            if (paddr == end) extend_before = i;
            if (page_end == start) extend_after = i;
        }

        if (extend_before) |before_index| {
            self.ranges[before_index].len += 1;
            self.len += 1;
            if (extend_after) |after_index| {
                if (after_index != before_index) {
                    self.ranges[before_index].len += self.ranges[after_index].len;
                    self.removeRangeAt(after_index);
                }
            }
            return;
        }

        if (extend_after) |after_index| {
            self.ranges[after_index].physical_start = paddr;
            self.ranges[after_index].len += 1;
            self.len += 1;
            return;
        }

        if (self.range_len >= self.ranges.len) return KernelError.TooManyFreeRanges;

        self.ranges[self.range_len] = .{
            .region_id = region_id,
            .len = 1,
            .physical_start = paddr,
        };
        self.range_len += 1;
        self.len += 1;
    }

    pub fn appendContiguousRange(
        self: *FreePageList,
        region_id: u64,
        physical_start: u64,
        page_count: usize,
    ) KernelError!void {
        if (page_count == 0) return;
        const byte_len = @as(u64, @intCast(page_count)) * 4096;
        const physical_end = physical_start + byte_len;
        var extend_before: ?usize = null;
        var extend_after: ?usize = null;
        var i: usize = 0;
        while (i < self.range_len) : (i += 1) {
            const range = &self.ranges[i];
            if (range.region_id != region_id) continue;
            const start = range.physical_start;
            const end = rangeEnd(range);
            if (physical_start < end and physical_end > start) return KernelError.InvalidState;
            if (physical_start == end) extend_before = i;
            if (physical_end == start) extend_after = i;
        }

        if (extend_before) |before_index| {
            self.ranges[before_index].len += page_count;
            self.len += page_count;
            if (extend_after) |after_index| {
                if (after_index != before_index) {
                    self.ranges[before_index].len += self.ranges[after_index].len;
                    self.removeRangeAt(after_index);
                }
            }
            return;
        }

        if (extend_after) |after_index| {
            self.ranges[after_index].physical_start = physical_start;
            self.ranges[after_index].len += page_count;
            self.len += page_count;
            return;
        }

        if (self.range_len >= self.ranges.len) return KernelError.TooManyFreeRanges;
        self.ranges[self.range_len] = .{
            .region_id = region_id,
            .len = page_count,
            .physical_start = physical_start,
        };
        self.range_len += 1;
        self.len += page_count;
    }

    pub fn canAppendPage(self: *const FreePageList, region_id: u64, paddr: u64) bool {
        const page_end = paddr + 4096;
        var i: usize = 0;
        while (i < self.range_len) : (i += 1) {
            const range = &self.ranges[i];
            if (range.region_id != region_id) continue;
            const start = range.physical_start;
            const end = rangeEnd(range);
            if (paddr >= start and paddr < end) return false;
            if (paddr == end or page_end == start) return true;
        }
        return self.range_len < self.ranges.len;
    }

    pub fn popFront(self: *FreePageList) KernelError!u64 {
        if (self.len == 0 or self.range_len == 0) return KernelError.OutOfFreePages;

        const first = &self.ranges[0];
        const paddr = first.physical_start;
        first.physical_start += 4096;
        first.len -= 1;
        self.len -= 1;

        if (first.len == 0) {
            var r: usize = 1;
            while (r < self.range_len) : (r += 1) {
                self.ranges[r - 1] = self.ranges[r];
            }
            self.range_len -= 1;
        }

        return paddr;
    }

    pub fn popFrontBelow(self: *FreePageList, limit_exclusive: u64) KernelError!u64 {
        if (self.len == 0 or self.range_len == 0) return KernelError.OutOfFreePages;

        var range_index: usize = 0;
        while (range_index < self.range_len) : (range_index += 1) {
            const range = &self.ranges[range_index];
            if (range.len == 0) continue;
            if (range.physical_start >= limit_exclusive) continue;

            const paddr = range.physical_start;
            range.physical_start += 4096;
            range.len -= 1;
            self.len -= 1;

            if (range.len == 0) {
                var r: usize = range_index + 1;
                while (r < self.range_len) : (r += 1) {
                    self.ranges[r - 1] = self.ranges[r];
                }
                self.range_len -= 1;
            }

            return paddr;
        }

        return KernelError.OutOfFreePages;
    }

    pub fn popFrontAtOrAbove(self: *FreePageList, min_inclusive: u64) KernelError!u64 {
        if (self.len == 0 or self.range_len == 0) return KernelError.OutOfFreePages;

        var range_index: usize = 0;
        while (range_index < self.range_len) : (range_index += 1) {
            const range = &self.ranges[range_index];
            if (range.len == 0) continue;
            const range_end = range.physical_start + (@as(u64, range.len) * 4096);
            if (range_end <= min_inclusive) continue;

            if (range.physical_start < min_inclusive) {
                const skip_pages: usize = @intCast((min_inclusive - range.physical_start + 4095) / 4096);
                if (skip_pages >= range.len) continue;
                const paddr = range.physical_start + (@as(u64, skip_pages) * 4096);
                const tail_len = range.len - skip_pages - 1;
                const head_len = skip_pages;
                range.len = head_len;
                self.len -= 1;
                if (tail_len > 0) {
                    if (self.range_len >= self.ranges.len) return KernelError.TooManyFreeRanges;
                    var move_index = self.range_len;
                    while (move_index > range_index + 1) : (move_index -= 1) {
                        self.ranges[move_index] = self.ranges[move_index - 1];
                    }
                    self.ranges[range_index + 1] = .{
                        .region_id = range.region_id,
                        .len = tail_len,
                        .physical_start = paddr + 4096,
                    };
                    self.range_len += 1;
                }
                return paddr;
            }

            const paddr = range.physical_start;
            range.physical_start += 4096;
            range.len -= 1;
            self.len -= 1;

            if (range.len == 0) {
                var r: usize = range_index + 1;
                while (r < self.range_len) : (r += 1) {
                    self.ranges[r - 1] = self.ranges[r];
                }
                self.range_len -= 1;
            }

            return paddr;
        }

        return KernelError.OutOfFreePages;
    }

    pub fn popBack(self: *FreePageList) KernelError!u64 {
        if (self.len == 0 or self.range_len == 0) return KernelError.OutOfFreePages;

        const last = &self.ranges[self.range_len - 1];
        const paddr = last.physical_start + (@as(u64, last.len - 1) * 4096);
        last.len -= 1;
        self.len -= 1;

        if (last.len == 0) {
            self.range_len -= 1;
        }

        return paddr;
    }

    pub fn popContiguousAtOrAbove(
        self: *FreePageList,
        page_count: usize,
        min_inclusive: u64,
    ) KernelError!u64 {
        if (page_count == 0) return KernelError.InvalidState;
        if (self.len < page_count or self.range_len == 0) return KernelError.OutOfFreePages;

        var range_index: usize = 0;
        while (range_index < self.range_len) : (range_index += 1) {
            const range = &self.ranges[range_index];
            if (range.len == 0) continue;
            const aligned_start = if (range.physical_start < min_inclusive)
                ((min_inclusive + 4095) & ~@as(u64, 4095))
            else
                range.physical_start;
            if (aligned_start < range.physical_start) continue;
            const skip_pages: usize = @intCast((aligned_start - range.physical_start) / 4096);
            if (skip_pages > range.len) continue;
            const available = range.len - skip_pages;
            if (available < page_count) continue;

            const alloc_start = aligned_start;
            const tail_pages = available - page_count;
            if (skip_pages == 0) {
                range.physical_start += @as(u64, @intCast(page_count)) * 4096;
                range.len -= page_count;
                if (range.len == 0) self.removeRangeAt(range_index);
            } else {
                range.len = skip_pages;
                if (tail_pages > 0) {
                    if (self.range_len >= self.ranges.len) return KernelError.TooManyFreeRanges;
                    var move_index = self.range_len;
                    while (move_index > range_index + 1) : (move_index -= 1) {
                        self.ranges[move_index] = self.ranges[move_index - 1];
                    }
                    self.ranges[range_index + 1] = .{
                        .region_id = range.region_id,
                        .len = tail_pages,
                        .physical_start = alloc_start + @as(u64, @intCast(page_count)) * 4096,
                    };
                    self.range_len += 1;
                }
            }
            self.len -= page_count;
            return alloc_start;
        }
        return KernelError.OutOfFreePages;
    }

    pub fn popContiguousBelow(
        self: *FreePageList,
        page_count: usize,
        limit_exclusive: u64,
    ) KernelError!u64 {
        if (page_count == 0) return KernelError.InvalidState;
        if (self.len < page_count or self.range_len == 0) return KernelError.OutOfFreePages;
        const size_bytes = @as(u64, @intCast(page_count)) * 4096;

        var range_index: usize = 0;
        while (range_index < self.range_len) : (range_index += 1) {
            const range = &self.ranges[range_index];
            if (range.len == 0) continue;
            const range_start = range.physical_start;
            const range_bytes = @as(u64, @intCast(range.len)) * 4096;
            const range_end = range_start + range_bytes;
            const usable_end = @min(range_end, limit_exclusive);
            if (usable_end <= range_start or usable_end - range_start < size_bytes) continue;

            const alloc_start = range_start;
            range.physical_start += size_bytes;
            range.len -= page_count;
            if (range.len == 0) self.removeRangeAt(range_index);
            self.len -= page_count;
            return alloc_start;
        }
        return KernelError.OutOfFreePages;
    }
};

pub const PageCapability = struct {
    paddr: u64,
};

pub var empty_process_descriptors_extra: [0]ProcessDescriptor = .{};
pub var empty_endpoint_tables_extra: [0]EndpointTable = .{};
pub var empty_fd_tables_extra: [0]FdTable = .{};
pub var empty_vma_tables_extra: [0]VmaTable = .{};
