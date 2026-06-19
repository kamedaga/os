const std = @import("std");
const builtin = @import("builtin");
pub const capsule = @import("capsule.zig");
const vtd = @import("vtd.zig");
pub const initial_process_count: usize = 8;
pub const initial_process_capacity: usize = 32;
pub const process_count: usize = initial_process_capacity;
pub const max_process_slots: usize = 65536;
pub const initial_thread_capacity: usize = 32;
pub const max_thread_slots: usize = 65536;
pub const device_count: usize = 1;
pub const device_principal_raw: u32 = @intCast(max_process_slots);
pub const principal_count: usize = process_count + device_count;
pub const cap_transfer_id_min: u64 = 0x1000;

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

pub const Rights = struct {
    cpu_read: bool = false,
    cpu_write: bool = false,
    dma: bool = false,
    grant: bool = false,
};

pub const CapsuleKind = capsule.CapsuleKind;
pub const CapsuleState = capsule.CapsuleState;
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

pub const Capability = struct {
    paddr: u64 = 0,
    rights: Rights = .{},
    cap_id: u64 = 0,
    root_cap_id: u64 = 0,
    parent_cap_id: u64 = 0,
};

pub const EndpointCapability = struct {
    endpoint_id: u64,
    target: PrincipalId,
};

pub const PendingCapTransfer = struct {
    transfer_id: u64,
    sender: PrincipalId,
    endpoint_id: u64,
    paddr: u64,
    rights: Rights,
    retain_sender: bool = false,
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
    CapabilityNotFound,
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
pub const fd_known_flags_mask: u32 = (@as(u32, 1) << 4) - 1;
pub const fd_known_rights_mask: u64 = (@as(u64, 1) << 44) - 1;

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
    _reserved: u20 = 0,
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
};

pub const TaskObjectState = enum(u8) {
    active = 1,
    exited = 2,
    killed = 3,
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
pub const max_ipc_message_fds: usize = 8;

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

    fn slotIndex(self: *const IpcQueue, offset: usize) usize {
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
};

pub const IpcChannelSlot = struct {
    active: bool = false,
    generation: u32 = 1,
    ref_count: u8 = 0,
    queues: [2]IpcQueue = .{ .{}, .{} },
};

pub const IpcReplySlot = struct {
    active: bool = false,
    generation: u32 = 1,
    sent: bool = false,
    queue: IpcQueue = .{},
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
pub const max_native_vmos: usize = 4096;
pub const max_vmas_per_process: usize = 512;

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

pub const NativeVmoSlot = struct {
    kind: NativeVmoKind = .none,
    generation: u32 = 1,
    size_bytes: u64 = 0,
    page_count: u16 = 0,
    page_store_start: u32 = 0,
    ref_count: u32 = 0,
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
    _reserved0: u2 = 0,
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

    pub fn endVa(self: *const VmaEntry) u64 {
        return self.start_va + self.size_bytes;
    }
};

pub const VmaTable = struct {
    entries: [max_vmas_per_process]VmaEntry = [_]VmaEntry{.{}} ** max_vmas_per_process,
};

pub const NativeVmaFaultMapping = struct {
    paddr: u64,
    prot: MapProt,
};

pub const CNode = struct {
    pub const inline_caps = 512;
    pub const chunk_caps = 512;
    pub const chunk_pool_count = 8192;
    pub const max_caps = inline_caps + chunk_pool_count * chunk_caps;
    pub const paddr_index_slots = 65536;
    const invalid_chunk: u16 = std.math.maxInt(u16);
    const invalid_index: u32 = std.math.maxInt(u32);

    caps: [inline_caps]Capability = [_]Capability{.{}} ** inline_caps,
    paddr_index: [paddr_index_slots]u32 = [_]u32{invalid_index} ** paddr_index_slots,
    paddr_index_overflow: bool = false,
    overflow_head: u16 = invalid_chunk,
    len: usize = 0,

    pub fn add(self: *CNode, cap: Capability) KernelError!void {
        if (self.findIndex(cap.paddr) != null) return KernelError.InvalidState;
        try self.putFresh(cap);
    }

    pub fn addAssumeFresh(self: *CNode, cap: Capability) KernelError!void {
        try self.putFresh(cap);
    }

    fn putFresh(self: *CNode, cap: Capability) KernelError!void {
        const slot = try self.slotAtOrAllocate(self.len);
        slot.* = cap;
        self.insertPaddrIndex(cap.paddr, self.len);
        trackPageCapAdded(cap);
        self.len += 1;
    }

    pub fn reset(self: *CNode) void {
        self.releaseTrackedPageRefs();
        self.resetStorageOnly();
    }

    fn resetStorageOnly(self: *CNode) void {
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            const next = cap_chunk_pool[chunk_index].next;
            cap_chunk_pool[chunk_index] = .{};
            chunk_index = next;
        }
        self.* = .{};
    }

    pub fn removeByPaddr(self: *CNode, paddr: u64) bool {
        if (self.findIndex(paddr)) |index| {
            self.removeIndex(index);
            return true;
        }
        return false;
    }

    pub fn find(self: *const CNode, paddr: u64) ?*const Capability {
        if (self.findIndex(paddr)) |index| {
            return self.slotAtConst(index);
        }
        return null;
    }

    pub fn findByCapId(self: *const CNode, cap_id: u64) ?*const Capability {
        if (self.findIndexByCapId(cap_id)) |index| {
            return self.slotAtConst(index);
        }
        return null;
    }

    pub fn removeByCapId(self: *CNode, cap_id: u64) bool {
        if (self.findIndexByCapId(cap_id)) |index| {
            self.removeIndex(index);
            return true;
        }
        return false;
    }

    pub fn removeExclusiveRootByPaddr(self: *CNode, paddr: u64) KernelError!void {
        const index = self.findIndex(paddr) orelse return KernelError.CapabilityNotFound;
        const cap = (self.slotAtConst(index) orelse return KernelError.CapabilityNotFound).*;
        if (cap.cap_id != cap.root_cap_id or cap.parent_cap_id != 0) return KernelError.InvalidState;
        self.removeIndex(index);
    }

    pub fn get(self: *const CNode, index: usize) ?Capability {
        if (index >= self.len) return null;
        return (self.slotAtConst(index) orelse return null).*;
    }

    pub fn popLast(self: *CNode) ?Capability {
        if (self.len == 0) return null;
        const last_index = self.len - 1;
        const cap = (self.slotAtConst(last_index) orelse return null).*;
        self.removePaddrIndex(cap.paddr);
        trackPageCapRemoved(cap);
        self.len = last_index;
        if (self.slotAt(self.len)) |slot| slot.* = .{};
        self.trimUnusedChunks();
        return cap;
    }

    fn removeIndex(self: *CNode, index: usize) void {
        if (index >= self.len) return;
        const last_index = self.len - 1;
        const removed = (self.slotAtConst(index) orelse return).*;
        self.removePaddrIndex(removed.paddr);
        trackPageCapRemoved(removed);
        if (index != last_index) {
            const last = (self.slotAtConst(last_index) orelse return).*;
            self.removePaddrIndex(last.paddr);
            (self.slotAt(index) orelse return).* = last;
            self.insertPaddrIndex(last.paddr, index);
        }
        self.len -= 1;
        if (self.slotAt(self.len)) |slot| slot.* = .{};
        self.trimUnusedChunks();
    }

    fn findIndex(self: *const CNode, paddr: u64) ?usize {
        if (self.findIndexByPaddrIndex(paddr)) |index| return index;
        if (!self.paddr_index_overflow) return null;
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            const cap = self.slotAtConst(i) orelse return null;
            if (cap.paddr == paddr) return i;
        }
        return null;
    }

    fn paddrHash(paddr: u64) usize {
        const page = paddr >> 12;
        const mixed = page *% 11400714819323198485;
        return @intCast(mixed & @as(u64, paddr_index_slots - 1));
    }

    fn probeDistance(home: usize, slot: usize) usize {
        return (slot + paddr_index_slots - home) & (paddr_index_slots - 1);
    }

    fn findIndexByPaddrIndex(self: *const CNode, paddr: u64) ?usize {
        var slot = paddrHash(paddr);
        var probes: usize = 0;
        while (probes < paddr_index_slots) : (probes += 1) {
            const stored = self.paddr_index[slot];
            if (stored == invalid_index) return null;
            const index: usize = @intCast(stored);
            if (index < self.len) {
                const cap = self.slotAtConst(index) orelse return null;
                if (cap.paddr == paddr) return index;
            }
            slot = (slot + 1) & (paddr_index_slots - 1);
        }
        return null;
    }

    fn insertPaddrIndex(self: *CNode, paddr: u64, index: usize) void {
        if (index > std.math.maxInt(u32)) {
            self.paddr_index_overflow = true;
            return;
        }
        var slot = paddrHash(paddr);
        var probes: usize = 0;
        while (probes < paddr_index_slots) : (probes += 1) {
            if (self.paddr_index[slot] == invalid_index) {
                self.paddr_index[slot] = @intCast(index);
                return;
            }
            slot = (slot + 1) & (paddr_index_slots - 1);
        }
        self.paddr_index_overflow = true;
    }

    fn removePaddrIndex(self: *CNode, paddr: u64) void {
        var slot = paddrHash(paddr);
        var probes: usize = 0;
        while (probes < paddr_index_slots) : (probes += 1) {
            const stored = self.paddr_index[slot];
            if (stored == invalid_index) return;
            const index: usize = @intCast(stored);
            if (index < self.len) {
                if (self.slotAtConst(index)) |cap| {
                    if (cap.paddr == paddr) {
                        self.removePaddrIndexSlot(slot);
                        return;
                    }
                }
            }
            slot = (slot + 1) & (paddr_index_slots - 1);
        }
    }

    fn removePaddrIndexSlot(self: *CNode, remove_slot: usize) void {
        var hole = remove_slot;
        var slot = (hole + 1) & (paddr_index_slots - 1);
        while (self.paddr_index[slot] != invalid_index) {
            const index: usize = @intCast(self.paddr_index[slot]);
            const cap = self.slotAtConst(index) orelse break;
            const home = paddrHash(cap.paddr);
            if (probeDistance(home, slot) > probeDistance(home, hole)) {
                self.paddr_index[hole] = self.paddr_index[slot];
                hole = slot;
            }
            slot = (slot + 1) & (paddr_index_slots - 1);
        }
        self.paddr_index[hole] = invalid_index;
    }

    fn findIndexByCapId(self: *const CNode, cap_id: u64) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            const cap = self.slotAtConst(i) orelse return null;
            if (cap.cap_id == cap_id) return i;
        }
        return null;
    }

    fn slotAt(self: *CNode, index: usize) ?*Capability {
        if (index < inline_caps) return &self.caps[index];
        var remaining = index - inline_caps;
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            if (remaining < chunk_caps) return &cap_chunk_pool[chunk_index].caps[remaining];
            remaining -= chunk_caps;
            chunk_index = cap_chunk_pool[chunk_index].next;
        }
        return null;
    }

    fn slotAtConst(self: *const CNode, index: usize) ?*const Capability {
        if (index < inline_caps) return &self.caps[index];
        var remaining = index - inline_caps;
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            if (remaining < chunk_caps) return &cap_chunk_pool[chunk_index].caps[remaining];
            remaining -= chunk_caps;
            chunk_index = cap_chunk_pool[chunk_index].next;
        }
        return null;
    }

    fn slotAtOrAllocate(self: *CNode, index: usize) KernelError!*Capability {
        if (index < inline_caps) return &self.caps[index];
        const needed_chunk_offset = (index - inline_caps) / chunk_caps;
        if (needed_chunk_offset >= chunk_pool_count) return KernelError.TableFull;
        if (self.overflow_head == invalid_chunk) self.overflow_head = try allocCapChunk();

        var chunk_index = self.overflow_head;
        var offset: usize = 0;
        while (offset < needed_chunk_offset) : (offset += 1) {
            if (cap_chunk_pool[chunk_index].next == invalid_chunk) {
                cap_chunk_pool[chunk_index].next = try allocCapChunk();
            }
            chunk_index = cap_chunk_pool[chunk_index].next;
        }
        return &cap_chunk_pool[chunk_index].caps[(index - inline_caps) % chunk_caps];
    }

    fn trimUnusedChunks(self: *CNode) void {
        if (self.overflow_head == invalid_chunk) return;
        if (self.len <= inline_caps) {
            freeCapChunks(self.overflow_head);
            self.overflow_head = invalid_chunk;
            return;
        }
        const needed_chunks = (self.len - inline_caps + chunk_caps - 1) / chunk_caps;
        var chunk_index = self.overflow_head;
        var kept: usize = 1;
        while (kept < needed_chunks and chunk_index != invalid_chunk) : (kept += 1) {
            chunk_index = cap_chunk_pool[chunk_index].next;
        }
        if (chunk_index == invalid_chunk) return;
        const free_head = cap_chunk_pool[chunk_index].next;
        cap_chunk_pool[chunk_index].next = invalid_chunk;
        freeCapChunks(free_head);
    }

    fn releaseTrackedPageRefs(self: *CNode) void {
        const inline_limit = @min(self.len, inline_caps);
        var index: usize = 0;
        while (index < inline_limit) : (index += 1) {
            trackPageCapRemoved(self.caps[index]);
        }

        var remaining = self.len - inline_limit;
        var chunk_index = self.overflow_head;
        while (remaining > 0 and chunk_index != invalid_chunk) {
            const chunk = &cap_chunk_pool[chunk_index];
            const chunk_limit = @min(remaining, chunk_caps);
            index = 0;
            while (index < chunk_limit) : (index += 1) {
                trackPageCapRemoved(chunk.caps[index]);
            }
            remaining -= chunk_limit;
            chunk_index = chunk.next;
        }
    }
};

const CapChunk = struct {
    used: bool = false,
    next: u16 = CNode.invalid_chunk,
    caps: [CNode.chunk_caps]Capability = [_]Capability{.{}} ** CNode.chunk_caps,
};

var empty_cap_chunk_pool: [0]CapChunk = .{};
var cap_chunk_pool: []CapChunk = empty_cap_chunk_pool[0..];

fn freeCapChunks(head: u16) void {
    var chunk_index = head;
    while (chunk_index != CNode.invalid_chunk) {
        const next = cap_chunk_pool[chunk_index].next;
        cap_chunk_pool[chunk_index] = .{};
        chunk_index = next;
    }
}

const PageCapRefEntry = struct {
    paddr: u64 = 0,
    refs: u32 = 0,
};

const page_cap_ref_slots: usize = 1 << 19;
var empty_page_cap_refs: [0]PageCapRefEntry = .{};
var page_cap_refs: []PageCapRefEntry = empty_page_cap_refs[0..];
var page_cap_ref_overflow: bool = false;

fn pageCapRefHash(paddr: u64) usize {
    const page = paddr >> 12;
    const mixed = page *% 11400714819323198485;
    return @intCast(mixed & @as(u64, page_cap_ref_slots - 1));
}

fn pageCapRefProbeDistance(home: usize, slot: usize) usize {
    return (slot + page_cap_ref_slots - home) & (page_cap_ref_slots - 1);
}

fn pageCapRefRemoveSlot(remove_slot: usize) void {
    var hole = remove_slot;
    var slot = (hole + 1) & (page_cap_ref_slots - 1);
    while (page_cap_refs[slot].refs != 0) {
        const home = pageCapRefHash(page_cap_refs[slot].paddr);
        if (pageCapRefProbeDistance(home, slot) > pageCapRefProbeDistance(home, hole)) {
            page_cap_refs[hole] = page_cap_refs[slot];
            hole = slot;
        }
        slot = (slot + 1) & (page_cap_ref_slots - 1);
    }
    page_cap_refs[hole] = .{};
}

fn clearPageCapRefTable() void {
    @memset(page_cap_refs[0..], .{});
    page_cap_ref_overflow = false;
}

fn rebuildPageCapRefsExcluding(state: *const KernelState, excluded_owner: PrincipalId) void {
    clearPageCapRefTable();
    const excluded_index = state.principalStorageIndex(excluded_owner);
    var pidx: usize = 0;
    while (pidx < state.process_capacity) : (pidx += 1) {
        if (pidx == excluded_index) continue;
        if (!(state.processDescriptorSlotConst(pidx) orelse continue).active) continue;
        const table = state.capTableForProcessIndexConst(pidx);
        var index: usize = 0;
        while (index < table.len) : (index += 1) {
            const cap = table.get(index) orelse break;
            trackPageCapAdded(cap);
        }
    }
    if (excluded_owner != .Device0) {
        const table = &state.cap_tables[process_count];
        var index: usize = 0;
        while (index < table.len) : (index += 1) {
            const cap = table.get(index) orelse break;
            trackPageCapAdded(cap);
        }
    }
}

fn trackPageCapAdded(cap: Capability) void {
    if (cap.cap_id == 0) return;
    if (page_cap_ref_overflow) return;
    var slot = pageCapRefHash(cap.paddr);
    var probes: usize = 0;
    while (probes < page_cap_ref_slots) : (probes += 1) {
        const entry = &page_cap_refs[slot];
        if (entry.refs == 0) {
            entry.* = .{ .paddr = cap.paddr, .refs = 1 };
            return;
        }
        if (entry.paddr == cap.paddr) {
            entry.refs +|= 1;
            return;
        }
        slot = (slot + 1) & (page_cap_ref_slots - 1);
    }
    page_cap_ref_overflow = true;
}

fn trackPageCapRemoved(cap: Capability) void {
    if (cap.cap_id == 0) return;
    if (page_cap_ref_overflow) return;
    var slot = pageCapRefHash(cap.paddr);
    var probes: usize = 0;
    while (probes < page_cap_ref_slots) : (probes += 1) {
        const entry = &page_cap_refs[slot];
        if (entry.refs == 0) return;
        if (entry.paddr == cap.paddr) {
            entry.refs -= 1;
            if (entry.refs == 0) pageCapRefRemoveSlot(slot);
            return;
        }
        slot = (slot + 1) & (page_cap_ref_slots - 1);
    }
}

fn pageCapRefCount(paddr: u64) u32 {
    if (page_cap_ref_overflow) return std.math.maxInt(u32);
    var slot = pageCapRefHash(paddr);
    var probes: usize = 0;
    while (probes < page_cap_ref_slots) : (probes += 1) {
        const entry = page_cap_refs[slot];
        if (entry.refs == 0) return 0;
        if (entry.paddr == paddr) return entry.refs;
        slot = (slot + 1) & (page_cap_ref_slots - 1);
    }
    return std.math.maxInt(u32);
}

fn pageCapRefUniqueCount() u64 {
    if (page_cap_ref_overflow) return std.math.maxInt(u64);
    var count: u64 = 0;
    for (page_cap_refs) |entry| {
        if (entry.refs != 0) count += 1;
    }
    return count;
}

fn vmObjectBackingFreePageCount() u64 {
    var pages: u64 = 0;
    var i: usize = 0;
    while (i < vmo_backing_page_store_free_range_len) : (i += 1) {
        pages += vmo_backing_page_store_free_ranges[i].len;
    }
    return pages;
}

fn allocCapChunk() KernelError!u16 {
    var i: usize = 0;
    while (i < cap_chunk_pool.len) : (i += 1) {
        if (cap_chunk_pool[i].used) continue;
        cap_chunk_pool[i] = .{ .used = true };
        return @intCast(i);
    }
    return KernelError.TableFull;
}

pub const EndpointCNode = struct {
    const max_caps = 8;

    caps: [max_caps]EndpointCapability = undefined,
    len: usize = 0,

    pub fn add(self: *EndpointCNode, cap: EndpointCapability) KernelError!void {
        if (self.findIndex(cap.endpoint_id)) |index| {
            self.caps[index] = cap;
            return;
        }
        if (self.len >= self.caps.len) return KernelError.TableFull;
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn find(self: *const EndpointCNode, endpoint_id: u64) ?*const EndpointCapability {
        if (self.findIndex(endpoint_id)) |index| {
            return &self.caps[index];
        }
        return null;
    }

    fn findIndex(self: *const EndpointCNode, endpoint_id: u64) ?usize {
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

    caps: [max_caps]EndpointCapability = undefined,
    len: usize = 0,

    pub fn publish(self: *PublishedEndpointTable, cap: EndpointCapability) KernelError!void {
        if (self.findIndex(cap.endpoint_id)) |index| {
            self.caps[index] = cap;
            return;
        }
        if (self.len >= self.caps.len) return KernelError.TableFull;
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn find(self: *const PublishedEndpointTable, endpoint_id: u64) ?*const EndpointCapability {
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

pub const CapMailbox = struct {
    const inline_items = 8;
    const chunk_items = 16;
    const chunk_pool_count = 512;
    const invalid_chunk: u16 = std.math.maxInt(u16);

    items: [inline_items]PendingCapTransfer = undefined,
    overflow_head: u16 = invalid_chunk,
    len: usize = 0,

    pub fn push(self: *CapMailbox, transfer: PendingCapTransfer) KernelError!void {
        const slot = try self.slotAtOrAllocate(self.len);
        slot.* = transfer;
        self.len += 1;
    }

    pub fn pop(self: *CapMailbox) ?PendingCapTransfer {
        if (self.len == 0) return null;
        const transfer = (self.slotAtConst(0) orelse return null).*;
        var i: usize = 1;
        while (i < self.len) : (i += 1) {
            const item = (self.slotAtConst(i) orelse return null).*;
            (self.slotAt(i - 1) orelse return null).* = item;
        }
        self.len -= 1;
        if (self.slotAt(self.len)) |slot| slot.* = undefined;
        self.trimUnusedChunks();
        return transfer;
    }

    pub fn reset(self: *CapMailbox) void {
        freeCapMailboxChunks(self.overflow_head);
        self.* = .{};
    }

    fn slotAt(self: *CapMailbox, index: usize) ?*PendingCapTransfer {
        if (index < inline_items) return &self.items[index];
        var remaining = index - inline_items;
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            if (remaining < chunk_items) return &cap_mailbox_chunk_pool[chunk_index].items[remaining];
            remaining -= chunk_items;
            chunk_index = cap_mailbox_chunk_pool[chunk_index].next;
        }
        return null;
    }

    fn slotAtConst(self: *const CapMailbox, index: usize) ?*const PendingCapTransfer {
        if (index < inline_items) return &self.items[index];
        var remaining = index - inline_items;
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            if (remaining < chunk_items) return &cap_mailbox_chunk_pool[chunk_index].items[remaining];
            remaining -= chunk_items;
            chunk_index = cap_mailbox_chunk_pool[chunk_index].next;
        }
        return null;
    }

    fn slotAtOrAllocate(self: *CapMailbox, index: usize) KernelError!*PendingCapTransfer {
        if (index < inline_items) return &self.items[index];
        const needed_chunk_offset = (index - inline_items) / chunk_items;
        if (needed_chunk_offset >= chunk_pool_count) return KernelError.TableFull;
        if (self.overflow_head == invalid_chunk) self.overflow_head = try allocCapMailboxChunk();

        var chunk_index = self.overflow_head;
        var offset: usize = 0;
        while (offset < needed_chunk_offset) : (offset += 1) {
            if (cap_mailbox_chunk_pool[chunk_index].next == invalid_chunk) {
                cap_mailbox_chunk_pool[chunk_index].next = try allocCapMailboxChunk();
            }
            chunk_index = cap_mailbox_chunk_pool[chunk_index].next;
        }
        return &cap_mailbox_chunk_pool[chunk_index].items[(index - inline_items) % chunk_items];
    }

    fn trimUnusedChunks(self: *CapMailbox) void {
        if (self.overflow_head == invalid_chunk) return;
        if (self.len <= inline_items) {
            freeCapMailboxChunks(self.overflow_head);
            self.overflow_head = invalid_chunk;
            return;
        }
        const needed_chunks = (self.len - inline_items + chunk_items - 1) / chunk_items;
        var chunk_index = self.overflow_head;
        var kept: usize = 1;
        while (kept < needed_chunks and chunk_index != invalid_chunk) : (kept += 1) {
            chunk_index = cap_mailbox_chunk_pool[chunk_index].next;
        }
        if (chunk_index == invalid_chunk) return;
        const free_head = cap_mailbox_chunk_pool[chunk_index].next;
        cap_mailbox_chunk_pool[chunk_index].next = invalid_chunk;
        freeCapMailboxChunks(free_head);
    }
};

const CapMailboxChunk = struct {
    used: bool = false,
    next: u16 = CapMailbox.invalid_chunk,
    items: [CapMailbox.chunk_items]PendingCapTransfer = undefined,
};

var cap_mailbox_chunk_pool: [CapMailbox.chunk_pool_count]CapMailboxChunk = [_]CapMailboxChunk{.{}} ** CapMailbox.chunk_pool_count;

fn allocCapMailboxChunk() KernelError!u16 {
    var i: usize = 0;
    while (i < cap_mailbox_chunk_pool.len) : (i += 1) {
        if (cap_mailbox_chunk_pool[i].used) continue;
        cap_mailbox_chunk_pool[i] = .{ .used = true };
        return @intCast(i);
    }
    return KernelError.TableFull;
}

fn freeCapMailboxChunks(head: u16) void {
    var chunk_index = head;
    while (chunk_index != CapMailbox.invalid_chunk) {
        const next = cap_mailbox_chunk_pool[chunk_index].next;
        cap_mailbox_chunk_pool[chunk_index] = .{};
        chunk_index = next;
    }
}

pub const max_vmo_backing_pages: usize = 65535;
pub const max_vmo_backing_store_pages: usize = 262144;
pub const max_vmo_backing_store_free_ranges: usize = 1024;

var empty_vmo_backing_page_store: [0]u64 = .{};
var vmo_backing_page_store: []u64 = empty_vmo_backing_page_store[0..];
var vmo_backing_page_store_next: usize = 0;

const VmoBackingStoreFreeRange = struct {
    start: u32 = 0,
    len: u32 = 0,
};

var empty_vmo_backing_page_store_free_ranges: [0]VmoBackingStoreFreeRange = .{};
var vmo_backing_page_store_free_ranges: []VmoBackingStoreFreeRange = empty_vmo_backing_page_store_free_ranges[0..];
var vmo_backing_page_store_free_range_len: usize = 0;

fn removeVmoBackingFreeRange(index: usize) void {
    var i = index + 1;
    while (i < vmo_backing_page_store_free_range_len) : (i += 1) {
        vmo_backing_page_store_free_ranges[i - 1] = vmo_backing_page_store_free_ranges[i];
    }
    vmo_backing_page_store_free_range_len -= 1;
}

fn insertVmoBackingFreeRange(start: u32, len: u32) bool {
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

fn allocEmptyVmoBackingPageStore(page_count: usize) ?u32 {
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
    @memset(vmo_backing_page_store[start .. start + page_count], 0);
    return @intCast(start);
}

fn vmoBackingPageStorePaddr(start: u32, page_count: u16, page_index: usize) ?u64 {
    if (page_index >= page_count) return null;
    const store_index = @as(usize, start) + page_index;
    if (store_index >= vmo_backing_page_store.len) return null;
    const paddr = vmo_backing_page_store[store_index];
    if ((paddr & 0xFFF) != 0) return null;
    return paddr;
}

fn setVmoBackingPageStorePaddr(start: u32, page_count: u16, page_index: usize, paddr: u64) bool {
    if ((paddr & 0xFFF) != 0) return false;
    if (page_index >= page_count) return false;
    const store_index = @as(usize, start) + page_index;
    if (store_index >= vmo_backing_page_store.len) return false;
    vmo_backing_page_store[store_index] = paddr;
    return true;
}

fn freeVmoBackingPageStore(start: u32, page_count: u16) bool {
    if (page_count == 0) return true;
    const start_usize: usize = @intCast(start);
    const count_usize: usize = @intCast(page_count);
    if (start_usize + count_usize > vmo_backing_page_store.len) return false;
    @memset(vmo_backing_page_store[start_usize .. start_usize + count_usize], 0);
    return insertVmoBackingFreeRange(start, page_count);
}

fn resetVmoBackingPageStore() void {
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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(cap_chunk_pool), &cap_chunk_pool));
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
    const start = std.mem.alignForward(usize, cursor.*, @alignOf(T));
    const bytes = @sizeOf(T) * count;
    if (start > storage.len or bytes > storage.len - start) return null;
    cursor.* = start + bytes;
    const ptr: [*]T = @ptrCast(@alignCast(storage.ptr + start));
    return ptr[0..count];
}

pub fn runtimeStorageBytes() usize {
    var cursor: usize = 0;
    cursor = std.mem.alignForward(usize, cursor, @alignOf(CapChunk));
    cursor += @sizeOf(CapChunk) * CNode.chunk_pool_count;
    cursor = std.mem.alignForward(usize, cursor, @alignOf(PageCapRefEntry));
    cursor += @sizeOf(PageCapRefEntry) * page_cap_ref_slots;
    cursor = std.mem.alignForward(usize, cursor, @alignOf(u64));
    cursor += @sizeOf(u64) * max_vmo_backing_store_pages;
    cursor = std.mem.alignForward(usize, cursor, @alignOf(VmoBackingStoreFreeRange));
    cursor += @sizeOf(VmoBackingStoreFreeRange) * max_vmo_backing_store_free_ranges;
    return std.mem.alignForward(usize, cursor, 4096);
}

pub fn initRuntimeStorage(storage: []align(4096) u8) bool {
    var cursor: usize = 0;
    cap_chunk_pool = runtimeStorageSlice(CapChunk, storage, &cursor, CNode.chunk_pool_count) orelse return false;
    page_cap_refs = runtimeStorageSlice(PageCapRefEntry, storage, &cursor, page_cap_ref_slots) orelse return false;
    vmo_backing_page_store = runtimeStorageSlice(u64, storage, &cursor, max_vmo_backing_store_pages) orelse return false;
    vmo_backing_page_store_free_ranges = runtimeStorageSlice(VmoBackingStoreFreeRange, storage, &cursor, max_vmo_backing_store_free_ranges) orelse return false;

    @memset(cap_chunk_pool, .{});
    @memset(page_cap_refs, .{});
    @memset(vmo_backing_page_store, 0);
    @memset(vmo_backing_page_store_free_ranges, .{});
    page_cap_ref_overflow = false;
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
};

pub const PageCapability = struct {
    paddr: u64,
};

var empty_process_descriptors_extra: [0]ProcessDescriptor = .{};
var empty_cap_tables_extra: [0]CNode = .{};
var empty_endpoint_tables_extra: [0]EndpointCNode = .{};
var empty_cap_mailboxes_extra: [0]CapMailbox = .{};
var empty_pending_page_transfers_extra: [0]?PendingCapTransfer = .{};
var empty_fd_tables_extra: [0]FdTable = .{};
var empty_vma_tables_extra: [0]VmaTable = .{};

pub const KernelState = struct {
    pub const max_regions = 256;
    pub const low_memory_limit: u64 = 4 * 1024 * 1024 * 1024;
    const max_total_caps = 65536;

    regions: [max_regions]Region = undefined,
    region_len: usize = 0,
    process_descriptors: [process_count]ProcessDescriptor = [_]ProcessDescriptor{.{}} ** process_count,
    process_descriptors_extra: []ProcessDescriptor = empty_process_descriptors_extra[0..],
    process_capacity: usize = process_count,
    active_process_count: usize = 0,
    cap_tables: [principal_count]CNode = [_]CNode{.{}} ** principal_count,
    endpoint_tables: [principal_count]EndpointCNode = [_]EndpointCNode{.{}} ** principal_count,
    cap_tables_extra: []CNode = empty_cap_tables_extra[0..],
    endpoint_tables_extra: []EndpointCNode = empty_endpoint_tables_extra[0..],
    published_service_endpoints: PublishedEndpointTable = .{},
    endpoint_generation: u64 = 0,
    cap_mailboxes: [principal_count]CapMailbox = [_]CapMailbox{.{}} ** principal_count,
    pending_page_transfers: [principal_count]?PendingCapTransfer = [_]?PendingCapTransfer{null} ** principal_count,
    fd_tables: [process_count]FdTable = [_]FdTable{.{}} ** process_count,
    vma_tables: [process_count]VmaTable = [_]VmaTable{.{}} ** process_count,
    cap_mailboxes_extra: []CapMailbox = empty_cap_mailboxes_extra[0..],
    pending_page_transfers_extra: []?PendingCapTransfer = empty_pending_page_transfers_extra[0..],
    fd_tables_extra: []FdTable = empty_fd_tables_extra[0..],
    vma_tables_extra: []VmaTable = empty_vma_tables_extra[0..],
    fd_objects: [max_fd_objects]KernelObjectSlot = [_]KernelObjectSlot{.{}} ** max_fd_objects,
    next_fd_object_scan: usize = 0,
    native_vmos: [max_native_vmos]NativeVmoSlot = [_]NativeVmoSlot{.{}} ** max_native_vmos,
    next_native_vmo_scan: usize = 0,
    aslr_sequence: u64 = 1,
    ipc_endpoints: [max_ipc_endpoints]IpcEndpointSlot = [_]IpcEndpointSlot{.{}} ** max_ipc_endpoints,
    next_ipc_endpoint_scan: usize = 0,
    ipc_channels: [max_ipc_channels]IpcChannelSlot = [_]IpcChannelSlot{.{}} ** max_ipc_channels,
    next_ipc_channel_scan: usize = 0,
    ipc_replies: [max_ipc_replies]IpcReplySlot = [_]IpcReplySlot{.{}} ** max_ipc_replies,
    next_ipc_reply_scan: usize = 0,
    zero_physical_page_hook: ?*const fn (paddr: u64) bool = null,
    debug_process_lifecycle_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, reason: DebugProcessLifecycleReason) void = null,
    revoke_queue: [max_total_caps]u64 = undefined,
    revoke_subtree: [max_total_caps]u64 = undefined,
    next_cap_id: u64 = 1,
    next_transfer_id: u64 = cap_transfer_id_min,

    fn allocCapId(self: *KernelState) u64 {
        const id = self.next_cap_id;
        self.next_cap_id +%= 1;
        return id;
    }

    fn allocTransferId(self: *KernelState) u64 {
        var id = self.next_transfer_id;
        self.next_transfer_id +%= 1;
        if (id < cap_transfer_id_min) {
            id = cap_transfer_id_min;
            self.next_transfer_id = id + 1;
        }
        return id;
    }

    fn pageAlignUp(value: u64) u64 {
        return (value + 4095) & ~@as(u64, 4095);
    }

    fn pageAlignDown(value: u64) u64 {
        return value & ~@as(u64, 4095);
    }

    fn isPageAligned(value: u64) bool {
        return (value & (native_page_size - 1)) == 0;
    }

    fn checkedEnd(start: u64, size: u64) KernelError!u64 {
        if (size == 0) return KernelError.InvalidState;
        if (start > std.math.maxInt(u64) - size) return KernelError.InvalidState;
        return start + size;
    }

    fn nativeVmoSlot(self: *KernelState, vmo_ref: NativeVmoRef) ?*NativeVmoSlot {
        if (vmo_ref.isNull()) return null;
        const index: usize = @intCast(vmo_ref.index);
        if (index >= max_native_vmos) return null;
        const slot = &self.native_vmos[index];
        if (slot.kind == .none or slot.generation != vmo_ref.generation) return null;
        return slot;
    }

    fn nativeVmoSlotConst(self: *const KernelState, vmo_ref: NativeVmoRef) ?*const NativeVmoSlot {
        if (vmo_ref.isNull()) return null;
        const index: usize = @intCast(vmo_ref.index);
        if (index >= max_native_vmos) return null;
        const slot = &self.native_vmos[index];
        if (slot.kind == .none or slot.generation != vmo_ref.generation) return null;
        return slot;
    }

    fn clearNativeVmoSlot(slot: *NativeVmoSlot) void {
        const next_generation = nextObjectGeneration(slot.generation);
        if (slot.page_count != 0) {
            _ = freeVmoBackingPageStore(slot.page_store_start, slot.page_count);
        }
        slot.* = .{ .generation = next_generation };
    }

    fn releaseNativeVmoOwnedPages(slot: *NativeVmoSlot, free_list: *FreePageList) void {
        var page_index: usize = 0;
        while (page_index < slot.page_count) : (page_index += 1) {
            const paddr = vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_index) orelse continue;
            if (paddr == 0) continue;
            if (free_list.canAppendPage(0, paddr)) {
                free_list.appendPage(0, paddr) catch {};
                _ = setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_index, 0);
            }
        }
    }

    fn createNativeVmo(self: *KernelState, kind: NativeVmoKind, size_bytes: u64) KernelError!NativeVmoRef {
        if (kind == .none or size_bytes == 0) return KernelError.InvalidState;
        const aligned_size = pageAlignUp(size_bytes);
        const page_count_u64 = aligned_size / native_page_size;
        if (page_count_u64 == 0 or page_count_u64 > max_vmo_backing_pages) return KernelError.InvalidState;
        const page_count: u16 = @intCast(page_count_u64);
        var offset: usize = 0;
        while (offset < max_native_vmos) : (offset += 1) {
            const index = (self.next_native_vmo_scan + offset) % max_native_vmos;
            const slot = &self.native_vmos[index];
            if (slot.kind != .none or slot.ref_count != 0) continue;
            const page_store_start = allocEmptyVmoBackingPageStore(page_count) orelse return KernelError.TableFull;
            if (slot.generation == 0) slot.generation = 1;
            slot.kind = kind;
            slot.size_bytes = aligned_size;
            slot.page_count = page_count;
            slot.page_store_start = page_store_start;
            slot.ref_count = 0;
            self.next_native_vmo_scan = (index + 1) % max_native_vmos;
            return .{
                .index = @intCast(index),
                .generation = slot.generation,
            };
        }
        return KernelError.TableFull;
    }

    fn retainNativeVmo(self: *KernelState, vmo_ref: NativeVmoRef) KernelError!void {
        const slot = self.nativeVmoSlot(vmo_ref) orelse return KernelError.InvalidState;
        if (slot.ref_count == std.math.maxInt(u32)) return KernelError.TableFull;
        slot.ref_count += 1;
    }

    fn releaseNativeVmo(self: *KernelState, vmo_ref: NativeVmoRef) void {
        const slot = self.nativeVmoSlot(vmo_ref) orelse return;
        if (slot.ref_count == 0) return;
        slot.ref_count -= 1;
        if (slot.ref_count == 0) clearNativeVmoSlot(slot);
    }

    fn releaseNativeVmoWithFreeList(self: *KernelState, vmo_ref: NativeVmoRef, free_list: *FreePageList) void {
        const slot = self.nativeVmoSlot(vmo_ref) orelse return;
        if (slot.ref_count == 0) return;
        slot.ref_count -= 1;
        if (slot.ref_count == 0) {
            releaseNativeVmoOwnedPages(slot, free_list);
            clearNativeVmoSlot(slot);
        }
    }

    fn releaseIpcMessage(self: *KernelState, msg: *IpcMessage) void {
        var i: usize = 0;
        while (i < msg.fd_count and i < max_ipc_message_fds) : (i += 1) {
            const object_ref = msg.fds[i].object;
            if (!object_ref.isNull()) self.releaseKernelObject(object_ref);
            msg.fds[i] = .{};
        }
        msg.* = .{};
    }

    fn releaseIpcMessageWithFreeList(self: *KernelState, msg: *IpcMessage, free_list: *FreePageList) void {
        var i: usize = 0;
        while (i < msg.fd_count and i < max_ipc_message_fds) : (i += 1) {
            const object_ref = msg.fds[i].object;
            if (!object_ref.isNull()) self.releaseKernelObjectWithFreeList(object_ref, free_list);
            msg.fds[i] = .{};
        }
        msg.* = .{};
    }

    fn clearIpcQueue(self: *KernelState, queue: *IpcQueue) void {
        while (queue.pop()) |msg_value| {
            var msg = msg_value;
            self.releaseIpcMessage(&msg);
        }
        queue.* = .{};
    }

    fn clearIpcQueueWithFreeList(self: *KernelState, queue: *IpcQueue, free_list: *FreePageList) void {
        while (queue.pop()) |msg_value| {
            var msg = msg_value;
            self.releaseIpcMessageWithFreeList(&msg, free_list);
        }
        queue.* = .{};
    }

    fn ipcEndpointSlot(self: *KernelState, endpoint_ref: IpcEndpointRef) ?*IpcEndpointSlot {
        if (endpoint_ref.isNull()) return null;
        const index: usize = @intCast(endpoint_ref.index);
        if (index >= max_ipc_endpoints) return null;
        const slot = &self.ipc_endpoints[index];
        if (!slot.active or slot.generation != endpoint_ref.generation) return null;
        return slot;
    }

    fn ipcEndpointSlotConst(self: *const KernelState, endpoint_ref: IpcEndpointRef) ?*const IpcEndpointSlot {
        if (endpoint_ref.isNull()) return null;
        const index: usize = @intCast(endpoint_ref.index);
        if (index >= max_ipc_endpoints) return null;
        const slot = &self.ipc_endpoints[index];
        if (!slot.active or slot.generation != endpoint_ref.generation) return null;
        return slot;
    }

    fn clearIpcEndpointSlot(self: *KernelState, endpoint_ref: IpcEndpointRef) void {
        const slot = self.ipcEndpointSlot(endpoint_ref) orelse return;
        self.clearIpcQueue(&slot.queue);
        slot.* = .{ .generation = nextObjectGeneration(slot.generation) };
    }

    fn clearIpcEndpointSlotWithFreeList(
        self: *KernelState,
        endpoint_ref: IpcEndpointRef,
        free_list: *FreePageList,
    ) void {
        const slot = self.ipcEndpointSlot(endpoint_ref) orelse return;
        self.clearIpcQueueWithFreeList(&slot.queue, free_list);
        slot.* = .{ .generation = nextObjectGeneration(slot.generation) };
    }

    fn createIpcEndpoint(self: *KernelState) KernelError!IpcEndpointRef {
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

    fn ipcChannelSlot(self: *KernelState, channel_ref: IpcChannelRef) ?*IpcChannelSlot {
        if (channel_ref.isNull()) return null;
        const index: usize = @intCast(channel_ref.index);
        if (index >= max_ipc_channels) return null;
        const slot = &self.ipc_channels[index];
        if (!slot.active or slot.generation != channel_ref.generation) return null;
        return slot;
    }

    fn ipcChannelSlotConst(self: *const KernelState, channel_ref: IpcChannelRef) ?*const IpcChannelSlot {
        if (channel_ref.isNull()) return null;
        const index: usize = @intCast(channel_ref.index);
        if (index >= max_ipc_channels) return null;
        const slot = &self.ipc_channels[index];
        if (!slot.active or slot.generation != channel_ref.generation) return null;
        return slot;
    }

    fn createIpcChannel(self: *KernelState) KernelError!IpcChannelRef {
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

    fn releaseIpcChannelHandle(self: *KernelState, handle: IpcChannelHandle) void {
        const slot = self.ipcChannelSlot(handle.channel) orelse return;
        if (slot.ref_count == 0) return;
        slot.ref_count -= 1;
        if (slot.ref_count != 0) return;
        self.clearIpcQueue(&slot.queues[0]);
        self.clearIpcQueue(&slot.queues[1]);
        slot.* = .{ .generation = nextObjectGeneration(slot.generation) };
    }

    fn releaseIpcChannelHandleWithFreeList(
        self: *KernelState,
        handle: IpcChannelHandle,
        free_list: *FreePageList,
    ) void {
        const slot = self.ipcChannelSlot(handle.channel) orelse return;
        if (slot.ref_count == 0) return;
        slot.ref_count -= 1;
        if (slot.ref_count != 0) return;
        self.clearIpcQueueWithFreeList(&slot.queues[0], free_list);
        self.clearIpcQueueWithFreeList(&slot.queues[1], free_list);
        slot.* = .{ .generation = nextObjectGeneration(slot.generation) };
    }

    fn ipcReplySlot(self: *KernelState, reply_ref: IpcReplyRef) ?*IpcReplySlot {
        if (reply_ref.isNull()) return null;
        const index: usize = @intCast(reply_ref.index);
        if (index >= max_ipc_replies) return null;
        const slot = &self.ipc_replies[index];
        if (!slot.active or slot.generation != reply_ref.generation) return null;
        return slot;
    }

    fn ipcReplySlotConst(self: *const KernelState, reply_ref: IpcReplyRef) ?*const IpcReplySlot {
        if (reply_ref.isNull()) return null;
        const index: usize = @intCast(reply_ref.index);
        if (index >= max_ipc_replies) return null;
        const slot = &self.ipc_replies[index];
        if (!slot.active or slot.generation != reply_ref.generation) return null;
        return slot;
    }

    fn clearIpcReplySlot(self: *KernelState, reply_ref: IpcReplyRef) void {
        const slot = self.ipcReplySlot(reply_ref) orelse return;
        self.clearIpcQueue(&slot.queue);
        slot.* = .{ .generation = nextObjectGeneration(slot.generation) };
    }

    fn clearIpcReplySlotWithFreeList(
        self: *KernelState,
        reply_ref: IpcReplyRef,
        free_list: *FreePageList,
    ) void {
        const slot = self.ipcReplySlot(reply_ref) orelse return;
        self.clearIpcQueueWithFreeList(&slot.queue, free_list);
        slot.* = .{ .generation = nextObjectGeneration(slot.generation) };
    }

    fn createIpcReply(self: *KernelState) KernelError!IpcReplyRef {
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

    fn resetNativeIpcObjects(self: *KernelState) void {
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

    pub fn nativeVmoRefCount(self: *const KernelState, vmo_ref: NativeVmoRef) ?u32 {
        const slot = self.nativeVmoSlotConst(vmo_ref) orelse return null;
        return slot.ref_count;
    }

    pub fn nativeVmoSize(self: *const KernelState, vmo_ref: NativeVmoRef) ?u64 {
        const slot = self.nativeVmoSlotConst(vmo_ref) orelse return null;
        return slot.size_bytes;
    }

    pub fn nativeVmoPagePaddr(self: *const KernelState, vmo_ref: NativeVmoRef, page_index: usize) ?u64 {
        const slot = self.nativeVmoSlotConst(vmo_ref) orelse return null;
        const paddr = vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_index) orelse return null;
        if (paddr == 0) return null;
        return paddr;
    }

    pub fn readFdVmoBytes(
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        out: []u8,
    ) KernelError!usize {
        if (out.len == 0) return 0;
        const table = try self.fdTableForActiveProcess(owner);
        const fd_index = fdIndex(fd) orelse return KernelError.InvalidState;
        const fd_entry = table.entries[fd_index];
        if (fd_entry.object.isNull() or !fd_entry.rights.read) return KernelError.InvalidState;
        const object_slot = self.kernelObjectSlotConst(fd_entry.object) orelse return KernelError.InvalidState;
        const vmo_ref = switch (object_slot.payload) {
            .vmo => |ref| ref,
            else => return KernelError.InvalidState,
        };
        const vmo = self.nativeVmoSlotConst(vmo_ref) orelse return KernelError.InvalidState;
        if (fd_entry.offset >= vmo.size_bytes) return 0;
        const max_len: usize = @intCast(@min(@as(u64, @intCast(out.len)), vmo.size_bytes - fd_entry.offset));
        var copied: usize = 0;
        while (copied < max_len) {
            const absolute = fd_entry.offset + @as(u64, @intCast(copied));
            const page_index: usize = @intCast(absolute / native_page_size);
            const page_offset: usize = @intCast(absolute % native_page_size);
            const paddr = self.nativeVmoPagePaddr(vmo_ref, page_index) orelse return KernelError.InvalidState;
            const page: [*]const u8 = @ptrFromInt(paddr);
            const chunk = @min(max_len - copied, native_page_size - page_offset);
            @memcpy(out[copied .. copied + chunk], page[page_offset .. page_offset + chunk]);
            copied += chunk;
        }
        const update_table = try self.fdTableForActiveProcess(owner);
        update_table.entries[fd_index].offset = fd_entry.offset + @as(u64, @intCast(copied));
        return copied;
    }

    pub fn installNativeVmoPages(
        self: *KernelState,
        vmo_ref: NativeVmoRef,
        page_offset: usize,
        paddrs: []const u64,
    ) KernelError!void {
        if (paddrs.len == 0) return KernelError.InvalidState;
        const slot = self.nativeVmoSlot(vmo_ref) orelse return KernelError.InvalidState;
        if (page_offset > slot.page_count or paddrs.len > @as(usize, slot.page_count) - page_offset) return KernelError.InvalidState;
        for (paddrs, 0..) |paddr, i| {
            if ((paddr & 0xFFF) != 0) return KernelError.InvalidState;
            if (vmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_offset + i) != 0) return KernelError.InvalidState;
        }
        for (paddrs, 0..) |paddr, i| {
            if (!setVmoBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_offset + i, paddr)) {
                return KernelError.InvalidState;
            }
        }
    }

    fn fdIndex(fd: Fd) ?usize {
        if (fd >= fd_table_entries) return null;
        return @intCast(fd);
    }

    fn findFreeFd(table: *const FdTable, min_fd: Fd) ?usize {
        var index = fdIndex(min_fd) orelse return null;
        while (index < fd_table_entries) : (index += 1) {
            if (table.entries[index].isEmpty()) return index;
        }
        return null;
    }

    fn nextObjectGeneration(generation: u32) u32 {
        var next = generation +% 1;
        if (next == 0) next = 1;
        return next;
    }

    fn objectOwner(raw: PrincipalRaw) ?PrincipalId {
        const principal: PrincipalId = @enumFromInt(raw);
        if (processIndexFromPrincipal(principal) != null or principal == .Device0) return principal;
        return null;
    }

    fn releaseMmioRegionObject(self: *KernelState, mmio: MmioRegionObject) void {
        _ = self;
        if (mmio.user_va == 0 or mmio.size == 0) return;
        const owner = objectOwner(mmio.owner_principal_raw) orelse return;
        if (mmio.size > @as(u64, std.math.maxInt(usize))) return;
        _ = @import("memory/user_vm.zig").unmapUserLinearRegion(owner, mmio.user_va, @intCast(mmio.size));
    }

    fn releaseDmaBufferObject(self: *KernelState, dma: DmaBufferObject) void {
        _ = self;
        if (dma.iova == 0 or dma.size == 0) return;
        _ = dma.device;
        vtd.unmapRange(dma.iova, dma.size);
    }

    fn releaseDmaMappingObject(self: *KernelState, mapping: DmaMappingObject) void {
        _ = self;
        if (mapping.iova == 0 or mapping.size == 0) return;
        _ = mapping.device;
        vtd.unmapRange(mapping.iova, mapping.size);
    }

    fn releaseIrqObject(self: *KernelState, irq: IrqObject) void {
        _ = self;
        _ = irq;
    }

    fn objectPayloadMatches(kind: KernelObjectKind, payload: KernelObjectPayload) bool {
        return std.meta.activeTag(payload) == kind;
    }

    fn releaseKernelObjectPayload(self: *KernelState, slot: *const KernelObjectSlot) void {
        switch (slot.payload) {
            .vmo => |vmo_ref| self.releaseNativeVmo(vmo_ref),
            .endpoint => |endpoint_ref| self.clearIpcEndpointSlot(endpoint_ref),
            .channel => |channel_handle| self.releaseIpcChannelHandle(channel_handle),
            .reply => |reply_ref| self.clearIpcReplySlot(reply_ref),
            .mmio_region => |mmio| self.releaseMmioRegionObject(mmio),
            .dma_buffer => |dma| self.releaseDmaBufferObject(dma),
            .dma_mapping => |mapping| self.releaseDmaMappingObject(mapping),
            .irq => |irq| self.releaseIrqObject(irq),
            else => {},
        }
    }

    fn releaseKernelObjectPayloadWithFreeList(
        self: *KernelState,
        slot: *const KernelObjectSlot,
        free_list: *FreePageList,
    ) void {
        switch (slot.payload) {
            .vmo => |vmo_ref| self.releaseNativeVmoWithFreeList(vmo_ref, free_list),
            .endpoint => |endpoint_ref| self.clearIpcEndpointSlotWithFreeList(endpoint_ref, free_list),
            .channel => |channel_handle| self.releaseIpcChannelHandleWithFreeList(channel_handle, free_list),
            .reply => |reply_ref| self.clearIpcReplySlotWithFreeList(reply_ref, free_list),
            .mmio_region => |mmio| self.releaseMmioRegionObject(mmio),
            .dma_buffer => |dma| self.releaseDmaBufferObject(dma),
            .dma_mapping => |mapping| self.releaseDmaMappingObject(mapping),
            .irq => |irq| self.releaseIrqObject(irq),
            else => {},
        }
    }

    fn clearKernelObjectSlot(self: *KernelState, slot: *KernelObjectSlot) void {
        self.releaseKernelObjectPayload(slot);
        slot.kind = .none;
        slot.ref_count = 0;
        slot.payload = .{ .none = {} };
        slot.generation = nextObjectGeneration(slot.generation);
    }

    fn clearKernelObjectSlotWithFreeList(
        self: *KernelState,
        slot: *KernelObjectSlot,
        free_list: *FreePageList,
    ) void {
        self.releaseKernelObjectPayloadWithFreeList(slot, free_list);
        slot.kind = .none;
        slot.ref_count = 0;
        slot.payload = .{ .none = {} };
        slot.generation = nextObjectGeneration(slot.generation);
    }

    fn resetKernelObjectTable(self: *KernelState) void {
        for (self.fd_objects[0..]) |*slot| {
            if (slot.kind != .none) self.releaseKernelObjectPayload(slot);
            slot.* = .{};
        }
        self.next_fd_object_scan = 0;
    }

    fn resetNativeVmoTable(self: *KernelState) void {
        @memset(self.native_vmos[0..], .{});
        self.next_native_vmo_scan = 0;
    }

    pub fn createKernelObject(
        self: *KernelState,
        kind: KernelObjectKind,
        payload: KernelObjectPayload,
    ) KernelError!KernelObjectRef {
        if (kind == .none or !objectPayloadMatches(kind, payload)) return KernelError.InvalidState;
        var offset: usize = 0;
        while (offset < max_fd_objects) : (offset += 1) {
            const index = (self.next_fd_object_scan + offset) % max_fd_objects;
            const slot = &self.fd_objects[index];
            if (slot.kind != .none or slot.ref_count != 0) continue;
            if (slot.generation == 0) slot.generation = 1;
            slot.kind = kind;
            slot.payload = payload;
            slot.ref_count = 0;
            self.next_fd_object_scan = (index + 1) % max_fd_objects;
            return .{
                .kind = kind,
                .index = @intCast(index),
                .generation = slot.generation,
            };
        }
        return KernelError.TableFull;
    }

    fn kernelObjectSlot(self: *KernelState, object_ref: KernelObjectRef) ?*KernelObjectSlot {
        if (object_ref.kind == .none) return null;
        const index: usize = @intCast(object_ref.index);
        if (index >= max_fd_objects) return null;
        const slot = &self.fd_objects[index];
        if (slot.kind != object_ref.kind or slot.generation != object_ref.generation) return null;
        return slot;
    }

    fn kernelObjectSlotConst(self: *const KernelState, object_ref: KernelObjectRef) ?*const KernelObjectSlot {
        if (object_ref.kind == .none) return null;
        const index: usize = @intCast(object_ref.index);
        if (index >= max_fd_objects) return null;
        const slot = &self.fd_objects[index];
        if (slot.kind != object_ref.kind or slot.generation != object_ref.generation) return null;
        return slot;
    }

    fn retainKernelObject(self: *KernelState, object_ref: KernelObjectRef) KernelError!void {
        const slot = self.kernelObjectSlot(object_ref) orelse return KernelError.InvalidState;
        if (slot.ref_count == std.math.maxInt(u32)) return KernelError.TableFull;
        slot.ref_count += 1;
    }

    fn releaseKernelObject(self: *KernelState, object_ref: KernelObjectRef) void {
        const slot = self.kernelObjectSlot(object_ref) orelse return;
        if (slot.ref_count == 0) return;
        slot.ref_count -= 1;
        if (slot.ref_count == 0) self.clearKernelObjectSlot(slot);
    }

    fn releaseKernelObjectWithFreeList(
        self: *KernelState,
        object_ref: KernelObjectRef,
        free_list: *FreePageList,
    ) void {
        const slot = self.kernelObjectSlot(object_ref) orelse return;
        if (slot.ref_count == 0) return;
        slot.ref_count -= 1;
        if (slot.ref_count == 0) self.clearKernelObjectSlotWithFreeList(slot, free_list);
    }

    pub fn kernelObjectRefCount(self: *const KernelState, object_ref: KernelObjectRef) ?u32 {
        const slot = self.kernelObjectSlotConst(object_ref) orelse return null;
        return slot.ref_count;
    }

    fn allocKernelSlice(comptime T: type, free_list: *FreePageList, count: usize) ?[]T {
        if (count == 0) return null;
        const bytes = @sizeOf(T) * count;
        const page_count = (bytes + 4095) / 4096;
        const paddr = free_list.popContiguousAtOrAbove(page_count, 0) catch return null;
        const raw: [*]u8 = @ptrFromInt(paddr);
        @memset(raw[0 .. page_count * 4096], 0);
        const ptr: [*]T = @ptrCast(@alignCast(raw));
        return ptr[0..count];
    }

    fn isRightsSubset(child: Rights, parent: Rights) bool {
        return (!child.cpu_read or parent.cpu_read) and
            (!child.cpu_write or parent.cpu_write) and
            (!child.dma or parent.dma) and
            (!child.grant or parent.grant);
    }

    fn processPrincipal(index: usize) PrincipalId {
        return processPrincipalFromIndex(index) orelse unreachable;
    }

    pub fn isActiveProcess(self: *const KernelState, principal: PrincipalId) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        return (self.processDescriptorSlotConst(index) orelse return false).active;
    }

    pub fn processDescriptor(self: *const KernelState, principal: PrincipalId) ?*const ProcessDescriptor {
        const index = processIndexFromPrincipal(principal) orelse return null;
        const desc = self.processDescriptorSlotConst(index) orelse return null;
        if (!desc.active) return null;
        return desc;
    }

    pub fn processStatus(self: *const KernelState, principal: PrincipalId) ProcessStatus {
        const index = processIndexFromPrincipal(principal) orelse return .{};
        const desc = (self.processDescriptorSlotConst(index) orelse return .{}).*;
        return .{
            .active = desc.active,
            .faulted = desc.faulted,
            .fault_vector = desc.fault_vector,
        };
    }

    pub fn isBootstrapOwner(self: *const KernelState, principal: PrincipalId) bool {
        const desc = self.processDescriptor(principal) orelse return false;
        return desc.bootstrap_owner;
    }

    pub fn hasActivePrincipal(self: *const KernelState, principal: PrincipalId) bool {
        if (processIndexFromPrincipal(principal)) |index| {
            return (self.processDescriptorSlotConst(index) orelse return false).active;
        }
        return principal == .Device0;
    }

    pub fn requireActiveProcess(self: *const KernelState, principal: PrincipalId) KernelError!void {
        const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
        if (!(self.processDescriptorSlotConst(index) orelse return KernelError.InvalidState).active) return KernelError.InvalidState;
    }

    fn requireActivePrincipal(self: *const KernelState, principal: PrincipalId) KernelError!void {
        if (!self.hasActivePrincipal(principal)) return KernelError.InvalidState;
    }

    fn principalStorageIndex(self: *const KernelState, principal: PrincipalId) usize {
        if (processIndexFromPrincipal(principal)) |index| {
            std.debug.assert((self.processDescriptorSlotConst(index) orelse unreachable).active);
            return index;
        }
        std.debug.assert(principal == .Device0);
        return process_count;
    }

    fn extraIndex(index: usize) ?usize {
        if (index < process_count) return null;
        return index - process_count;
    }

    fn processDescriptorSlot(self: *KernelState, index: usize) ?*ProcessDescriptor {
        if (index >= self.process_capacity) return null;
        if (extraIndex(index)) |extra| return &self.process_descriptors_extra[extra];
        return &self.process_descriptors[index];
    }

    fn processDescriptorSlotConst(self: *const KernelState, index: usize) ?*const ProcessDescriptor {
        if (index >= self.process_capacity) return null;
        if (extraIndex(index)) |extra| return &self.process_descriptors_extra[extra];
        return &self.process_descriptors[index];
    }

    fn capTableForProcessIndex(self: *KernelState, index: usize) *CNode {
        if (extraIndex(index)) |extra| return &self.cap_tables_extra[extra];
        return &self.cap_tables[index];
    }

    fn capTableForProcessIndexConst(self: *const KernelState, index: usize) *const CNode {
        if (extraIndex(index)) |extra| return &self.cap_tables_extra[extra];
        return &self.cap_tables[index];
    }

    fn endpointTableForProcessIndex(self: *KernelState, index: usize) *EndpointCNode {
        if (extraIndex(index)) |extra| return &self.endpoint_tables_extra[extra];
        return &self.endpoint_tables[index];
    }

    fn endpointTableForProcessIndexConst(self: *const KernelState, index: usize) *const EndpointCNode {
        if (extraIndex(index)) |extra| return &self.endpoint_tables_extra[extra];
        return &self.endpoint_tables[index];
    }

    fn capMailboxForProcessIndex(self: *KernelState, index: usize) *CapMailbox {
        if (extraIndex(index)) |extra| return &self.cap_mailboxes_extra[extra];
        return &self.cap_mailboxes[index];
    }

    fn pendingPageTransferForProcessIndex(self: *KernelState, index: usize) *?PendingCapTransfer {
        if (extraIndex(index)) |extra| return &self.pending_page_transfers_extra[extra];
        return &self.pending_page_transfers[index];
    }

    fn capMailboxForPrincipal(self: *KernelState, principal: PrincipalId) *CapMailbox {
        if (processIndexFromPrincipal(principal)) |index| return self.capMailboxForProcessIndex(index);
        return &self.cap_mailboxes[self.principalStorageIndex(principal)];
    }

    fn pendingPageTransferForPrincipal(self: *KernelState, principal: PrincipalId) *?PendingCapTransfer {
        if (processIndexFromPrincipal(principal)) |index| return self.pendingPageTransferForProcessIndex(index);
        return &self.pending_page_transfers[self.principalStorageIndex(principal)];
    }

    fn fdTableForProcessIndex(self: *KernelState, index: usize) ?*FdTable {
        if (index >= self.process_capacity) return null;
        if (extraIndex(index)) |extra| return &self.fd_tables_extra[extra];
        return &self.fd_tables[index];
    }

    fn fdTableForProcessIndexConst(self: *const KernelState, index: usize) ?*const FdTable {
        if (index >= self.process_capacity) return null;
        if (extraIndex(index)) |extra| return &self.fd_tables_extra[extra];
        return &self.fd_tables[index];
    }

    fn vmaTableForProcessIndex(self: *KernelState, index: usize) ?*VmaTable {
        if (index >= self.process_capacity) return null;
        if (extraIndex(index)) |extra| return &self.vma_tables_extra[extra];
        return &self.vma_tables[index];
    }

    fn vmaTableForProcessIndexConst(self: *const KernelState, index: usize) ?*const VmaTable {
        if (index >= self.process_capacity) return null;
        if (extraIndex(index)) |extra| return &self.vma_tables_extra[extra];
        return &self.vma_tables[index];
    }

    pub fn getFdTable(self: *KernelState, principal: PrincipalId) ?*FdTable {
        const index = processIndexFromPrincipal(principal) orelse return null;
        return self.fdTableForProcessIndex(index);
    }

    pub fn getFdTableConst(self: *const KernelState, principal: PrincipalId) ?*const FdTable {
        const index = processIndexFromPrincipal(principal) orelse return null;
        return self.fdTableForProcessIndexConst(index);
    }

    pub fn inheritFdsForProcessCreate(self: *KernelState, from: PrincipalId, to: PrincipalId) KernelError!void {
        if (from == to) return KernelError.InvalidState;
        const source_table = try self.fdTableForActiveProcessConst(from);
        const dest_table = try self.fdTableForActiveProcess(to);
        var fd_index: usize = 0;
        while (fd_index < fd_table_entries) : (fd_index += 1) {
            const source = source_table.entries[fd_index];
            if (source.object.isNull() or !source.flags.inherit or source.flags.private) continue;
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

    pub fn getVmaTable(self: *KernelState, principal: PrincipalId) ?*VmaTable {
        const index = processIndexFromPrincipal(principal) orelse return null;
        return self.vmaTableForProcessIndex(index);
    }

    pub fn getVmaTableConst(self: *const KernelState, principal: PrincipalId) ?*const VmaTable {
        const index = processIndexFromPrincipal(principal) orelse return null;
        return self.vmaTableForProcessIndexConst(index);
    }

    fn fdTableForActiveProcess(self: *KernelState, principal: PrincipalId) KernelError!*FdTable {
        try self.requireActiveProcess(principal);
        return self.getFdTable(principal) orelse KernelError.InvalidState;
    }

    fn fdTableForActiveProcessConst(self: *const KernelState, principal: PrincipalId) KernelError!*const FdTable {
        try self.requireActiveProcess(principal);
        return self.getFdTableConst(principal) orelse KernelError.InvalidState;
    }

    pub fn fdEntryConst(self: *const KernelState, owner: PrincipalId, fd: Fd) ?*const FdEntry {
        const table = self.getFdTableConst(owner) orelse return null;
        const index = fdIndex(fd) orelse return null;
        if (table.entries[index].isEmpty()) return null;
        return &table.entries[index];
    }

    pub fn vmaEntryConst(self: *const KernelState, owner: PrincipalId, start_va: u64) ?*const VmaEntry {
        const table = self.getVmaTableConst(owner) orelse return null;
        for (table.entries[0..]) |*entry| {
            if (!entry.active) continue;
            if (entry.start_va == start_va) return entry;
        }
        return null;
    }

    pub fn vmaEntryForVaConst(self: *const KernelState, owner: PrincipalId, va: u64) ?*const VmaEntry {
        const table = self.getVmaTableConst(owner) orelse return null;
        for (table.entries[0..]) |*entry| {
            if (!entry.active) continue;
            if (va >= entry.start_va and va < entry.endVa()) return entry;
        }
        return null;
    }

    fn vmaProtAllowsFault(prot: VmaProt, write_access: bool, instruction_fetch: bool) bool {
        if (instruction_fetch) return prot.exec;
        if (write_access) return prot.write;
        return prot.read;
    }

    pub fn nativeVmaFaultMapping(
        self: *const KernelState,
        owner: PrincipalId,
        fault_page_va: u64,
        write_access: bool,
        instruction_fetch: bool,
    ) ?NativeVmaFaultMapping {
        if (!isPageAligned(fault_page_va)) return null;
        const entry = self.vmaEntryForVaConst(owner, fault_page_va) orelse return null;
        if (!vmaProtAllowsFault(entry.prot, write_access, instruction_fetch)) return null;
        const page_delta = (fault_page_va - entry.start_va) / native_page_size;
        const vmo_page = (entry.vmo_offset / native_page_size) + page_delta;
        const paddr = self.nativeVmoPagePaddr(entry.vmo, @intCast(vmo_page)) orelse return null;
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

    pub fn nativeVmoRefForFd(self: *const KernelState, owner: PrincipalId, fd: Fd) ?NativeVmoRef {
        const entry = self.fdEntryConst(owner, fd) orelse return null;
        const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
        return switch (slot.payload) {
            .vmo => |vmo_ref| vmo_ref,
            else => null,
        };
    }

    fn releaseFdTableForProcessIndex(self: *KernelState, index: usize) void {
        const table = self.fdTableForProcessIndex(index) orelse return;
        var fd: usize = 0;
        while (fd < fd_table_entries) : (fd += 1) {
            const object_ref = table.entries[fd].object;
            if (object_ref.isNull()) continue;
            table.entries[fd] = .{};
            self.releaseKernelObject(object_ref);
        }
    }

    fn releaseFdTableForProcessIndexWithFreeList(
        self: *KernelState,
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

    fn releaseVmaTableForProcessIndex(self: *KernelState, index: usize) void {
        const table = self.vmaTableForProcessIndex(index) orelse return;
        for (table.entries[0..]) |*entry| {
            if (!entry.active) continue;
            const vmo_ref = entry.vmo;
            entry.* = .{};
            self.releaseNativeVmo(vmo_ref);
        }
    }

    fn releaseVmaTableForProcessIndexWithFreeList(
        self: *KernelState,
        index: usize,
        free_list: *FreePageList,
    ) void {
        const table = self.vmaTableForProcessIndex(index) orelse return;
        for (table.entries[0..]) |*entry| {
            if (!entry.active) continue;
            const vmo_ref = entry.vmo;
            entry.* = .{};
            self.releaseNativeVmoWithFreeList(vmo_ref, free_list);
        }
    }

    pub fn releasePrincipalNativeMemory(
        self: *KernelState,
        owner: PrincipalId,
        free_list: *FreePageList,
    ) void {
        const index = processIndexFromPrincipal(owner) orelse return;
        self.releaseVmaTableForProcessIndexWithFreeList(index, free_list);
        self.releaseFdTableForProcessIndexWithFreeList(index, free_list);
    }

    pub fn resetProcessRuntimeTables(self: *KernelState, index: usize) void {
        self.releaseVmaTableForProcessIndex(index);
        self.releaseFdTableForProcessIndex(index);
        self.capTableForProcessIndex(index).reset();
        self.endpointTableForProcessIndex(index).* = .{};
        self.capMailboxForProcessIndex(index).reset();
        self.pendingPageTransferForProcessIndex(index).* = null;
    }

    fn findFreeVma(table: *const VmaTable) ?usize {
        for (table.entries[0..], 0..) |*entry, index| {
            if (!entry.active) return index;
        }
        return null;
    }

    fn vmaRangeOverlaps(table: *const VmaTable, start_va: u64, size_bytes: u64) KernelError!bool {
        const end_va = try checkedEnd(start_va, size_bytes);
        for (table.entries[0..]) |*entry| {
            if (!entry.active) continue;
            if (start_va < entry.endVa() and end_va > entry.start_va) return true;
        }
        return false;
    }

    pub fn findFreeUserMapVa(
        self: *const KernelState,
        owner: PrincipalId,
        size_bytes: u64,
        seed: u64,
    ) KernelError!u64 {
        if (!isPageAligned(size_bytes) or size_bytes == 0) return KernelError.InvalidState;
        const base = @import("kernel_abi_root").process_abi.user_aslr_base_va;
        const end = @import("kernel_abi_root").process_abi.user_aslr_end_va;
        const granule = @import("kernel_abi_root").process_abi.user_aslr_granule;
        if (end <= base or size_bytes > end - base) return KernelError.InvalidState;
        const table = self.getVmaTableConst(owner) orelse return KernelError.InvalidState;
        const slots = ((end - base - size_bytes) / granule) + 1;
        if (slots == 0) return KernelError.InvalidState;
        var mixed = seed ^ (@as(u64, @intFromEnum(owner)) *% 0x9e37_79b9_7f4a_7c15);
        var attempt: u64 = 0;
        while (attempt < slots) : (attempt += 1) {
            mixed = mixed *% 0xbf58_476d_1ce4_e5b9 +% 0x94d0_49bb_1331_11eb;
            const slot = (mixed + attempt) % slots;
            const candidate = base + slot * granule;
            if (!try vmaRangeOverlaps(table, candidate, size_bytes)) return candidate;
        }
        var candidate = base;
        while (candidate + size_bytes <= end) : (candidate += granule) {
            if (!try vmaRangeOverlaps(table, candidate, size_bytes)) return candidate;
        }
        return KernelError.TableFull;
    }

    fn vmaProtAllowedByRights(prot: VmaProt, rights: FdRights) bool {
        if (prot.read and !rights.map_read) return false;
        if (prot.write and !rights.map_write) return false;
        if (prot.exec and !rights.map_exec) return false;
        return true;
    }

    pub fn createAnonymousVmoFd(
        self: *KernelState,
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
        self: *KernelState,
        owner: PrincipalId,
        size_bytes: u64,
        rights: FdRights,
        flags: FdFlags,
        min_fd: Fd,
        free_list: *FreePageList,
    ) KernelError!Fd {
        const aligned_size = pageAlignUp(size_bytes);
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
        self: *KernelState,
        owner: PrincipalId,
        start_va: u64,
        size_bytes: u64,
        prot: VmaProt,
        flags: MmapFlags,
        free_list: *FreePageList,
    ) KernelError!NativeVmoRef {
        try self.requireActiveProcess(owner);
        if (!isPageAligned(start_va) or !isPageAligned(size_bytes)) return KernelError.InvalidState;
        const page_count_u64 = size_bytes / native_page_size;
        if (page_count_u64 == 0 or page_count_u64 > max_vmo_backing_pages) return KernelError.InvalidState;

        const vma_table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
        if (try vmaRangeOverlaps(vma_table, start_va, size_bytes)) return KernelError.InvalidState;
        const vma_index = findFreeVma(vma_table) orelse return KernelError.TableFull;

        const vmo_ref = try self.createNativeVmo(.anonymous, size_bytes);
        try self.retainNativeVmo(vmo_ref);
        errdefer self.releaseNativeVmoWithFreeList(vmo_ref, free_list);

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
        allocated = 0;

        vma_table.entries[vma_index] = .{
            .active = true,
            .start_va = start_va,
            .size_bytes = size_bytes,
            .prot = prot,
            .flags = flags,
            .vmo = vmo_ref,
            .vmo_offset = 0,
        };
        return vmo_ref;
    }

    pub fn fdInfo(self: *const KernelState, owner: PrincipalId, fd: Fd) ?FdInfo {
        const entry = self.fdEntryConst(owner, fd) orelse return null;
        const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
        var info = FdInfo{
            .kind = slot.kind,
            .rights_bits = fdRightsToBits(entry.rights),
            .flags_bits = fdFlagsToBits(entry.flags),
        };
        switch (slot.payload) {
            .process => |process| {
                info.size_bytes = process.exit_code;
                info.extra = @intFromEnum(process.state);
            },
            .thread => |thread| {
                info.size_bytes = thread.exit_code;
                info.extra = @intFromEnum(thread.state);
            },
            .event => |counter| {
                info.size_bytes = counter;
            },
            .vmo => |vmo_ref| {
                info.size_bytes = self.nativeVmoSize(vmo_ref) orelse 0;
            },
            .timer => |timer| {
                info.size_bytes = timer.deadline_tick;
                info.extra = timer.interval_ticks;
            },
            .serial => |serial| {
                info.extra = serial.stream;
            },
            else => {},
        }
        return info;
    }

    pub fn eventReadCounter(self: *KernelState, owner: PrincipalId, fd: Fd) ?u64 {
        const view = self.fdPayloadWithRights(owner, fd, .{ .read = true }) orelse return null;
        const counter = switch (view.payload.*) {
            .event => |counter| counter,
            else => return null,
        };
        if (counter == 0) return 0;
        view.payload.* = .{ .event = 0 };
        return counter;
    }

    pub fn eventWriteCounter(self: *KernelState, owner: PrincipalId, fd: Fd, value: u64) KernelError!void {
        if (value == 0) return;
        const view = self.fdPayloadWithRights(owner, fd, .{ .write = true }) orelse return KernelError.InvalidState;
        const counter = switch (view.payload.*) {
            .event => |counter| counter,
            else => return KernelError.InvalidState,
        };
        const next, const overflow = @addWithOverflow(counter, value);
        if (overflow != 0 or next == std.math.maxInt(u64)) return KernelError.InvalidState;
        view.payload.* = .{ .event = next };
    }

    pub fn eventWakeOwnersForFd(
        self: *const KernelState,
        owner: PrincipalId,
        fd: Fd,
        out: []PrincipalId,
    ) KernelError!usize {
        const source = self.fdEntryConst(owner, fd) orelse return KernelError.InvalidState;
        const slot = self.kernelObjectSlotConst(source.object) orelse return KernelError.InvalidState;
        if (slot.kind != .event) return KernelError.InvalidState;
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
                if (!candidate.rights.read or (!candidate.rights.wait and !candidate.rights.poll)) continue;
                if (candidate.object.kind != source.object.kind or
                    candidate.object.index != source.object.index or
                    candidate.object.generation != source.object.generation) continue;
                appendUniquePrincipal(out, &count, desc.principal);
                break;
            }
        }
        return count;
    }

    fn timerDueCount(timer: TimerObject, now_tick: u64) u64 {
        if (timer.deadline_tick == 0 or now_tick < timer.deadline_tick) return 0;
        if (timer.interval_ticks == 0) return 1;
        return 1 + (now_tick - timer.deadline_tick) / timer.interval_ticks;
    }

    fn timerNextWakeTick(timer: TimerObject, now_tick: u64) ?u64 {
        if (timer.deadline_tick == 0) return null;
        if (timerDueCount(timer, now_tick) != 0) return now_tick;
        return timer.deadline_tick;
    }

    pub fn timerReadExpirations(self: *KernelState, owner: PrincipalId, fd: Fd, now_tick: u64) ?u64 {
        const view = self.fdPayloadWithRights(owner, fd, .{ .read = true }) orelse return null;
        var timer = switch (view.payload.*) {
            .timer => |timer| timer,
            else => return null,
        };
        const count = timerDueCount(timer, now_tick);
        if (count == 0) return 0;
        if (timer.interval_ticks == 0) {
            timer.deadline_tick = 0;
        } else {
            timer.deadline_tick +%= count * timer.interval_ticks;
        }
        view.payload.* = .{ .timer = timer };
        return count;
    }

    pub fn timerFdState(self: *const KernelState, owner: PrincipalId, fd: Fd, now_tick: u64) ?TimerFdState {
        const view = self.fdPayloadWithRightsConst(owner, fd, .{ .inspect = true }) orelse return null;
        const timer = switch (view.payload.*) {
            .timer => |timer| timer,
            else => return null,
        };
        const remaining = if (timer.deadline_tick == 0 or now_tick >= timer.deadline_tick) 0 else timer.deadline_tick - now_tick;
        return .{
            .remaining_ticks = remaining,
            .interval_ticks = timer.interval_ticks,
        };
    }

    pub fn setTimerFd(
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        deadline_tick: u64,
        interval_ticks: u64,
        flags: u32,
    ) KernelError!void {
        const view = self.fdPayloadWithRights(owner, fd, .{ .write = true }) orelse return KernelError.InvalidState;
        var timer = switch (view.payload.*) {
            .timer => |timer| timer,
            else => return KernelError.InvalidState,
        };
        timer.deadline_tick = deadline_tick;
        timer.interval_ticks = interval_ticks;
        timer.flags = flags;
        view.payload.* = .{ .timer = timer };
    }

    fn fdIpcReadable(self: *const KernelState, payload: *const KernelObjectPayload) bool {
        return switch (payload.*) {
            .endpoint => |endpoint_ref| blk: {
                const endpoint = self.ipcEndpointSlotConst(endpoint_ref) orelse break :blk false;
                break :blk !endpoint.queue.isEmpty();
            },
            .channel => |handle| blk: {
                if (handle.side > 1) break :blk false;
                const channel = self.ipcChannelSlotConst(handle.channel) orelse break :blk false;
                break :blk !channel.queues[@as(usize, handle.side)].isEmpty();
            },
            .reply => |reply_ref| blk: {
                const reply = self.ipcReplySlotConst(reply_ref) orelse break :blk false;
                break :blk !reply.queue.isEmpty();
            },
            else => false,
        };
    }

    fn fdIpcWritable(self: *const KernelState, payload: *const KernelObjectPayload) bool {
        return switch (payload.*) {
            .endpoint => |endpoint_ref| blk: {
                const endpoint = self.ipcEndpointSlotConst(endpoint_ref) orelse break :blk false;
                break :blk !endpoint.queue.isFull();
            },
            .channel => |handle| blk: {
                if (handle.side > 1) break :blk false;
                const channel = self.ipcChannelSlotConst(handle.channel) orelse break :blk false;
                break :blk !channel.queues[1 - @as(usize, handle.side)].isFull();
            },
            .reply => |reply_ref| blk: {
                const reply = self.ipcReplySlotConst(reply_ref) orelse break :blk false;
                break :blk !reply.sent;
            },
            else => false,
        };
    }

    pub fn fdPollEvents(self: *const KernelState, owner: PrincipalId, fd: Fd, requested: u64, now_tick: u64) ?u64 {
        const entry = self.fdEntryConst(owner, fd) orelse return null;
        if (!entry.rights.poll) return null;
        const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
        var ready: u64 = 0;
        if ((requested & @import("kernel_abi_root").fd_abi.event_readable) != 0) {
            const readable = switch (slot.payload) {
                .endpoint, .channel, .reply => self.fdIpcReadable(&slot.payload),
                .process => |process| process.state != .active,
                .thread => |thread| thread.state != .active,
                .event => |counter| entry.rights.read and counter != 0,
                .irq => |irq| irq.event_count != 0,
                .timer => |timer| timerDueCount(timer, now_tick) != 0,
                .serial => false,
                else => false,
            };
            if (readable) ready |= @import("kernel_abi_root").fd_abi.event_readable;
        }
        if ((requested & @import("kernel_abi_root").fd_abi.event_writable) != 0) {
            const writable = switch (slot.payload) {
                .endpoint, .channel, .reply => self.fdIpcWritable(&slot.payload),
                .event => entry.rights.write,
                .serial => entry.rights.write,
                else => false,
            };
            if (writable) ready |= @import("kernel_abi_root").fd_abi.event_writable;
        }
        return ready & requested;
    }

    pub fn fdNextWakeTick(self: *const KernelState, owner: PrincipalId, fd: Fd, now_tick: u64) ?u64 {
        const entry = self.fdEntryConst(owner, fd) orelse return null;
        if (!entry.rights.wait and !entry.rights.poll) return null;
        const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
        return switch (slot.payload) {
            .timer => |timer| timerNextWakeTick(timer, now_tick),
            else => null,
        };
    }

    pub fn fdPayloadWithRightsConst(
        self: *const KernelState,
        owner: PrincipalId,
        fd: Fd,
        required_rights: FdRights,
    ) ?struct { rights: FdRights, payload: *const KernelObjectPayload } {
        const entry = self.fdEntryConst(owner, fd) orelse return null;
        if (!isFdRightsSubset(required_rights, entry.rights)) return null;
        const slot = self.kernelObjectSlotConst(entry.object) orelse return null;
        return .{ .rights = entry.rights, .payload = &slot.payload };
    }

    pub fn fdPayloadWithRights(
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        required_rights: FdRights,
    ) ?struct { rights: FdRights, payload: *KernelObjectPayload } {
        const table = self.getFdTable(owner) orelse return null;
        const index = fdIndex(fd) orelse return null;
        const entry = &table.entries[index];
        if (entry.isEmpty()) return null;
        if (!isFdRightsSubset(required_rights, entry.rights)) return null;
        const slot = self.kernelObjectSlot(entry.object) orelse return null;
        return .{ .rights = entry.rights, .payload = &slot.payload };
    }

    fn processFromPayload(payload: *const KernelObjectPayload) ?ProcessObject {
        return switch (payload.*) {
            .process => |process| process,
            else => null,
        };
    }

    fn threadFromPayload(payload: *const KernelObjectPayload) ?ThreadObject {
        return switch (payload.*) {
            .thread => |thread| thread,
            else => null,
        };
    }

    pub fn processObjectForFd(self: *const KernelState, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?ProcessObject {
        const view = self.fdPayloadWithRightsConst(owner, fd, required_rights) orelse return null;
        return processFromPayload(view.payload);
    }

    pub fn threadObjectForFd(self: *const KernelState, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?ThreadObject {
        const view = self.fdPayloadWithRightsConst(owner, fd, required_rights) orelse return null;
        return threadFromPayload(view.payload);
    }

    pub fn createProcessFd(
        self: *KernelState,
        owner: PrincipalId,
        process: ProcessObject,
        rights: FdRights,
        flags: FdFlags,
        min_fd: Fd,
    ) KernelError!Fd {
        if (processIndexFromPrincipal(@enumFromInt(process.principal_raw)) == null) return KernelError.InvalidState;
        const object_ref = try self.createKernelObject(.process, .{ .process = process });
        return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
            if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
            return err;
        };
    }

    pub fn createThreadFd(
        self: *KernelState,
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
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        required_rights: FdRights,
        state: TaskObjectState,
        exit_code: u32,
    ) KernelError!ProcessObject {
        const view = self.fdPayloadWithRights(owner, fd, required_rights) orelse return KernelError.InvalidState;
        var process = processFromPayload(view.payload) orelse return KernelError.InvalidState;
        process.state = state;
        process.exit_code = exit_code;
        view.payload.* = .{ .process = process };
        return process;
    }

    pub fn setThreadObjectStateForFd(
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        required_rights: FdRights,
        state: TaskObjectState,
        exit_code: u32,
    ) KernelError!ThreadObject {
        const view = self.fdPayloadWithRights(owner, fd, required_rights) orelse return KernelError.InvalidState;
        var thread = threadFromPayload(view.payload) orelse return KernelError.InvalidState;
        thread.state = state;
        thread.exit_code = exit_code;
        view.payload.* = .{ .thread = thread };
        return thread;
    }

    pub fn markProcessObjectsExited(self: *KernelState, principal: PrincipalId, state: TaskObjectState, exit_code: u32) void {
        var i: usize = 0;
        const principal_raw: PrincipalRaw = @intFromEnum(principal);
        while (i < self.fd_objects.len) : (i += 1) {
            const slot = &self.fd_objects[i];
            if (slot.kind != .process) continue;
            var process = processFromPayload(&slot.payload) orelse continue;
            if (process.principal_raw != principal_raw) continue;
            process.state = state;
            process.exit_code = exit_code;
            slot.payload = .{ .process = process };
        }
    }

    pub fn markThreadObjectsExitedForPrincipal(self: *KernelState, principal: PrincipalId, state: TaskObjectState, exit_code: u32) void {
        var i: usize = 0;
        const principal_raw: PrincipalRaw = @intFromEnum(principal);
        while (i < self.fd_objects.len) : (i += 1) {
            const slot = &self.fd_objects[i];
            if (slot.kind != .thread) continue;
            var thread = threadFromPayload(&slot.payload) orelse continue;
            if (thread.owner_principal_raw != principal_raw) continue;
            thread.state = state;
            thread.exit_code = exit_code;
            slot.payload = .{ .thread = thread };
        }
    }

    pub fn markThreadObjectsExitedBySlot(self: *KernelState, thread_index: usize, thread_generation: u32, state: TaskObjectState, exit_code: u32) void {
        var i: usize = 0;
        while (i < self.fd_objects.len) : (i += 1) {
            const slot = &self.fd_objects[i];
            if (slot.kind != .thread) continue;
            var thread = threadFromPayload(&slot.payload) orelse continue;
            if (thread.thread_index != thread_index or thread.thread_generation != thread_generation) continue;
            thread.state = state;
            thread.exit_code = exit_code;
            slot.payload = .{ .thread = thread };
        }
    }

    pub fn deviceObjectForFd(self: *const KernelState, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?DeviceObject {
        const view = self.fdPayloadWithRightsConst(owner, fd, required_rights) orelse return null;
        return switch (view.payload.*) {
            .device => |device| device,
            else => null,
        };
    }

    pub fn dmaBufferObjectForFd(self: *const KernelState, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?DmaBufferObject {
        const view = self.fdPayloadWithRightsConst(owner, fd, required_rights) orelse return null;
        return switch (view.payload.*) {
            .dma_buffer => |buffer| buffer,
            else => null,
        };
    }

    pub fn irqObjectForFd(self: *const KernelState, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?IrqObject {
        const view = self.fdPayloadWithRightsConst(owner, fd, required_rights) orelse return null;
        return switch (view.payload.*) {
            .irq => |irq| irq,
            else => null,
        };
    }

    fn irqMatchesInterrupt(irq: IrqObject, vector: u32) bool {
        if (irq.kind == .auto) return irq.vector == 0 or irq.vector == vector;
        return irq.vector == vector;
    }

    fn appendUniquePrincipal(out: []PrincipalId, count: *usize, principal: PrincipalId) void {
        var i: usize = 0;
        while (i < count.*) : (i += 1) {
            if (out[i] == principal) return;
        }
        if (count.* >= out.len) return;
        out[count.*] = principal;
        count.* += 1;
    }

    pub fn recordDeviceInterruptEvent(self: *KernelState, vector: u32, wake_owners: []PrincipalId) usize {
        var wake_count: usize = 0;
        for (self.fd_objects[0..]) |*slot| {
            if (slot.kind != .irq) continue;
            switch (slot.payload) {
                .irq => |*irq| {
                    if (!irqMatchesInterrupt(irq.*, vector)) continue;
                    irq.event_count +%= 1;
                    const owner = objectOwner(irq.owner_principal_raw) orelse continue;
                    appendUniquePrincipal(wake_owners, &wake_count, owner);
                },
                else => {},
            }
        }
        return wake_count;
    }

    pub fn irqEventCountForFd(self: *const KernelState, owner: PrincipalId, fd: Fd, required_rights: FdRights) ?u64 {
        const irq = self.irqObjectForFd(owner, fd, required_rights) orelse return null;
        return irq.event_count;
    }

    pub fn createDeviceFd(
        self: *KernelState,
        owner: PrincipalId,
        device: DmaDeviceId,
        rights: FdRights,
        flags: FdFlags,
        min_fd: Fd,
    ) KernelError!Fd {
        if (device == 0) return KernelError.InvalidState;
        const object_ref = try self.createKernelObject(.device, .{ .device = .{
            .owner_principal_raw = @intFromEnum(owner),
            .device = device,
        } });
        return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
            if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
            return err;
        };
    }

    pub fn createMmioRegionFd(
        self: *KernelState,
        owner: PrincipalId,
        mmio: MmioRegionObject,
        rights: FdRights,
        flags: FdFlags,
        min_fd: Fd,
    ) KernelError!Fd {
        if (mmio.device == 0 or mmio.paddr == 0 or mmio.size == 0) return KernelError.InvalidState;
        var payload = mmio;
        payload.owner_principal_raw = @intFromEnum(owner);
        const object_ref = try self.createKernelObject(.mmio_region, .{ .mmio_region = payload });
        return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
            if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
            return err;
        };
    }

    pub fn createDmaBufferFd(
        self: *KernelState,
        owner: PrincipalId,
        dma: DmaBufferObject,
        rights: FdRights,
        flags: FdFlags,
        min_fd: Fd,
    ) KernelError!Fd {
        if (dma.device == 0 or dma.user_va == 0 or dma.iova == 0 or dma.size == 0) return KernelError.InvalidState;
        var payload = dma;
        payload.owner_principal_raw = @intFromEnum(owner);
        const object_ref = try self.createKernelObject(.dma_buffer, .{ .dma_buffer = payload });
        return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
            if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
            return err;
        };
    }

    pub fn createDmaMappingFd(
        self: *KernelState,
        owner: PrincipalId,
        mapping: DmaMappingObject,
        rights: FdRights,
        flags: FdFlags,
        min_fd: Fd,
    ) KernelError!Fd {
        if (mapping.device == 0 or mapping.user_va == 0 or mapping.iova == 0 or mapping.size == 0) return KernelError.InvalidState;
        var payload = mapping;
        payload.owner_principal_raw = @intFromEnum(owner);
        const object_ref = try self.createKernelObject(.dma_mapping, .{ .dma_mapping = payload });
        return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
            if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
            return err;
        };
    }

    pub fn createIrqFd(
        self: *KernelState,
        owner: PrincipalId,
        irq: IrqObject,
        rights: FdRights,
        flags: FdFlags,
        min_fd: Fd,
    ) KernelError!Fd {
        if (irq.device == 0) return KernelError.InvalidState;
        var payload = irq;
        payload.owner_principal_raw = @intFromEnum(owner);
        const object_ref = try self.createKernelObject(.irq, .{ .irq = payload });
        return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
            if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
            return err;
        };
    }

    pub fn createTimerFd(
        self: *KernelState,
        owner: PrincipalId,
        deadline_tick: u64,
        interval_ticks: u64,
        flags: FdFlags,
        rights: FdRights,
        min_fd: Fd,
    ) KernelError!Fd {
        try self.requireActiveProcess(owner);
        const object_ref = try self.createKernelObject(.timer, .{ .timer = .{
            .owner_principal_raw = @intFromEnum(owner),
            .deadline_tick = deadline_tick,
            .interval_ticks = interval_ticks,
        } });
        return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
            if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
            return err;
        };
    }

    pub fn createEventFd(
        self: *KernelState,
        owner: PrincipalId,
        initial_value: u64,
        flags: FdFlags,
        rights: FdRights,
        min_fd: Fd,
    ) KernelError!Fd {
        try self.requireActiveProcess(owner);
        if (initial_value == std.math.maxInt(u64)) return KernelError.InvalidState;
        const object_ref = try self.createKernelObject(.event, .{ .event = initial_value });
        return self.installFd(owner, object_ref, rights, flags, min_fd) catch |err| {
            if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
            return err;
        };
    }

    pub fn createSerialFdAt(
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        stream: u8,
    ) KernelError!void {
        try self.requireActiveProcess(owner);
        const index = fdIndex(fd) orelse return KernelError.InvalidState;
        const table = try self.fdTableForActiveProcess(owner);
        if (!table.entries[index].isEmpty()) return KernelError.InvalidState;
        const object_ref = try self.createKernelObject(.serial, .{ .serial = .{ .stream = stream } });
        errdefer if (self.kernelObjectSlot(object_ref)) |slot| self.clearKernelObjectSlot(slot);
        try self.retainKernelObject(object_ref);
        table.entries[index] = .{
            .object = object_ref,
            .rights = .{
                .inspect = true,
                .dup = true,
                .transfer = true,
                .poll = true,
                .set_flags = true,
                .close = true,
                .read = true,
                .write = true,
            },
            .flags = .{ .inherit = true },
        };
    }

    pub fn mmapFd(
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        start_va: u64,
        size_bytes: u64,
        prot: VmaProt,
        flags: MmapFlags,
        vmo_offset: u64,
    ) KernelError!u64 {
        const fd_table = try self.fdTableForActiveProcessConst(owner);
        const fd_index = fdIndex(fd) orelse return KernelError.InvalidState;
        const fd_entry = fd_table.entries[fd_index];
        if (fd_entry.object.isNull()) return KernelError.InvalidState;
        if (!vmaProtAllowedByRights(prot, fd_entry.rights)) return KernelError.InvalidState;
        if (!isPageAligned(start_va) or !isPageAligned(size_bytes) or !isPageAligned(vmo_offset)) return KernelError.InvalidState;
        const map_end = try checkedEnd(start_va, size_bytes);
        _ = map_end;

        const object_slot = self.kernelObjectSlotConst(fd_entry.object) orelse return KernelError.InvalidState;
        const vmo_ref = switch (object_slot.payload) {
            .vmo => |ref| ref,
            else => return KernelError.InvalidState,
        };
        const vmo = self.nativeVmoSlotConst(vmo_ref) orelse return KernelError.InvalidState;
        const vmo_end = try checkedEnd(vmo_offset, size_bytes);
        if (vmo_end > vmo.size_bytes) return KernelError.InvalidState;

        const vma_table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
        if (try vmaRangeOverlaps(vma_table, start_va, size_bytes)) return KernelError.InvalidState;
        const vma_index = findFreeVma(vma_table) orelse return KernelError.TableFull;
        try self.retainNativeVmo(vmo_ref);
        vma_table.entries[vma_index] = .{
            .active = true,
            .start_va = start_va,
            .size_bytes = size_bytes,
            .prot = prot,
            .flags = flags,
            .vmo = vmo_ref,
            .vmo_offset = vmo_offset,
        };
        return start_va;
    }

    pub fn mmapFdIntoProcess(
        self: *KernelState,
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
        if (!vmaProtAllowedByRights(prot, fd_entry.rights)) return KernelError.InvalidState;
        if (!isPageAligned(start_va) or !isPageAligned(size_bytes) or !isPageAligned(vmo_offset)) return KernelError.InvalidState;
        const map_end = try checkedEnd(start_va, size_bytes);
        _ = map_end;

        const object_slot = self.kernelObjectSlotConst(fd_entry.object) orelse return KernelError.InvalidState;
        const vmo_ref = switch (object_slot.payload) {
            .vmo => |ref| ref,
            else => return KernelError.InvalidState,
        };
        const vmo = self.nativeVmoSlotConst(vmo_ref) orelse return KernelError.InvalidState;
        const vmo_end = try checkedEnd(vmo_offset, size_bytes);
        if (vmo_end > vmo.size_bytes) return KernelError.InvalidState;

        const vma_table = self.getVmaTable(target_owner) orelse return KernelError.InvalidState;
        if (try vmaRangeOverlaps(vma_table, start_va, size_bytes)) return KernelError.InvalidState;
        const vma_index = findFreeVma(vma_table) orelse return KernelError.TableFull;
        try self.retainNativeVmo(vmo_ref);
        vma_table.entries[vma_index] = .{
            .active = true,
            .start_va = start_va,
            .size_bytes = size_bytes,
            .prot = prot,
            .flags = flags,
            .vmo = vmo_ref,
            .vmo_offset = vmo_offset,
        };
        return start_va;
    }

    pub fn munmapExact(self: *KernelState, owner: PrincipalId, start_va: u64, size_bytes: u64) KernelError!void {
        try self.requireActiveProcess(owner);
        if (!isPageAligned(start_va) or !isPageAligned(size_bytes)) return KernelError.InvalidState;
        const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
        for (table.entries[0..]) |*entry| {
            if (!entry.active) continue;
            if (entry.start_va != start_va or entry.size_bytes != size_bytes) continue;
            const vmo_ref = entry.vmo;
            entry.* = .{};
            self.releaseNativeVmo(vmo_ref);
            return;
        }
        return KernelError.InvalidState;
    }

    pub fn munmapExactWithFreeList(
        self: *KernelState,
        owner: PrincipalId,
        start_va: u64,
        size_bytes: u64,
        free_list: *FreePageList,
    ) KernelError!void {
        try self.requireActiveProcess(owner);
        if (!isPageAligned(start_va) or !isPageAligned(size_bytes)) return KernelError.InvalidState;
        const table = self.getVmaTable(owner) orelse return KernelError.InvalidState;
        for (table.entries[0..]) |*entry| {
            if (!entry.active) continue;
            if (entry.start_va != start_va or entry.size_bytes != size_bytes) continue;
            const vmo_ref = entry.vmo;
            entry.* = .{};
            self.releaseNativeVmoWithFreeList(vmo_ref, free_list);
            return;
        }
        return KernelError.InvalidState;
    }

    pub fn installFd(
        self: *KernelState,
        owner: PrincipalId,
        object_ref: KernelObjectRef,
        rights: FdRights,
        flags: FdFlags,
        min_fd: Fd,
    ) KernelError!Fd {
        const table = try self.fdTableForActiveProcess(owner);
        const index = findFreeFd(table, min_fd) orelse return KernelError.TableFull;
        try self.retainKernelObject(object_ref);
        table.entries[index] = .{
            .object = object_ref,
            .rights = fdRightsFromBits(fdRightsToBits(rights)),
            .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
        };
        return @intCast(index);
    }

    pub fn closeFd(self: *KernelState, owner: PrincipalId, fd: Fd) KernelError!void {
        const table = try self.fdTableForActiveProcess(owner);
        const index = fdIndex(fd) orelse return KernelError.InvalidState;
        const object_ref = table.entries[index].object;
        if (object_ref.isNull()) return KernelError.InvalidState;
        table.entries[index] = .{};
        self.releaseKernelObject(object_ref);
    }

    pub fn closeFdWithFreeList(
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        free_list: *FreePageList,
    ) KernelError!void {
        const table = try self.fdTableForActiveProcess(owner);
        const index = fdIndex(fd) orelse return KernelError.InvalidState;
        const object_ref = table.entries[index].object;
        if (object_ref.isNull()) return KernelError.InvalidState;
        table.entries[index] = .{};
        self.releaseKernelObjectWithFreeList(object_ref, free_list);
    }

    pub fn dupFd(
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        min_fd: Fd,
        rights: FdRights,
        flags: FdFlags,
    ) KernelError!Fd {
        const table = try self.fdTableForActiveProcessConst(owner);
        const index = fdIndex(fd) orelse return KernelError.InvalidState;
        const source = table.entries[index];
        if (source.object.isNull()) return KernelError.InvalidState;
        if (!source.rights.dup) return KernelError.InvalidState;
        if (!isFdRightsSubset(rights, source.rights)) return KernelError.InvalidState;
        return self.installFd(owner, source.object, rights, flags, min_fd);
    }

    pub fn replaceFd(
        self: *KernelState,
        owner: PrincipalId,
        dst_fd: Fd,
        src_fd: Fd,
        rights: FdRights,
        flags: FdFlags,
    ) KernelError!void {
        if (dst_fd == src_fd) return KernelError.InvalidState;
        const src_table = try self.fdTableForActiveProcessConst(owner);
        const src_index = fdIndex(src_fd) orelse return KernelError.InvalidState;
        const dst_index = fdIndex(dst_fd) orelse return KernelError.InvalidState;
        const source = src_table.entries[src_index];
        if (source.object.isNull()) return KernelError.InvalidState;
        if (!source.rights.dup) return KernelError.InvalidState;
        if (!isFdRightsSubset(rights, source.rights)) return KernelError.InvalidState;

        try self.retainKernelObject(source.object);
        const dst_table = try self.fdTableForActiveProcess(owner);
        const old_object = dst_table.entries[dst_index].object;
        dst_table.entries[dst_index] = .{
            .object = source.object,
            .rights = fdRightsFromBits(fdRightsToBits(rights)),
            .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
        };
        if (!old_object.isNull()) self.releaseKernelObject(old_object);
    }

    pub fn setFdFlags(
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        flags: FdFlags,
        mask: FdFlags,
    ) KernelError!void {
        const table = try self.fdTableForActiveProcess(owner);
        const index = fdIndex(fd) orelse return KernelError.InvalidState;
        const entry = &table.entries[index];
        if (entry.object.isNull()) return KernelError.InvalidState;
        if (!entry.rights.set_flags) return KernelError.InvalidState;
        const old_bits = fdFlagsToBits(entry.flags);
        const mask_bits = fdFlagsToBits(mask);
        const new_bits = (old_bits & ~mask_bits) | (fdFlagsToBits(flags) & mask_bits);
        entry.flags = fdFlagsFromBits(new_bits);
    }

    pub fn transferFd(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        fd: Fd,
        min_fd: Fd,
        rights: FdRights,
        flags: FdFlags,
        mode: FdTransferMode,
    ) KernelError!Fd {
        if (from == to) return KernelError.InvalidState;
        const source_table = try self.fdTableForActiveProcessConst(from);
        const source_index = fdIndex(fd) orelse return KernelError.InvalidState;
        const source = source_table.entries[source_index];
        if (source.object.isNull()) return KernelError.InvalidState;
        if (!source.rights.transfer) return KernelError.InvalidState;
        if (!isFdRightsSubset(rights, source.rights)) return KernelError.InvalidState;
        if (self.kernelObjectSlotConst(source.object) == null) return KernelError.InvalidState;

        const dest_table = try self.fdTableForActiveProcess(to);
        const dest_index = findFreeFd(dest_table, min_fd) orelse return KernelError.TableFull;

        switch (mode) {
            .copy => {
                try self.retainKernelObject(source.object);
                dest_table.entries[dest_index] = .{
                    .object = source.object,
                    .rights = fdRightsFromBits(fdRightsToBits(rights)),
                    .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
                };
            },
            .move => {
                const mutable_source_table = try self.fdTableForActiveProcess(from);
                dest_table.entries[dest_index] = .{
                    .object = source.object,
                    .rights = fdRightsFromBits(fdRightsToBits(rights)),
                    .flags = fdFlagsFromBits(fdFlagsToBits(flags)),
                };
                mutable_source_table.entries[source_index] = .{};
            },
        }
        return @intCast(dest_index);
    }

    fn endpointRights() FdRights {
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

    fn replyReceiveRights() FdRights {
        return .{
            .inspect = true,
            .wait = true,
            .poll = true,
            .close = true,
            .recv = true,
        };
    }

    fn replySendRights() FdRights {
        return .{
            .inspect = true,
            .close = true,
            .send = true,
        };
    }

    pub fn createIpcEndpointFd(
        self: *KernelState,
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

    pub const IpcChannelPair = struct {
        a: Fd,
        b: Fd,
    };

    pub fn createIpcChannelPairFds(
        self: *KernelState,
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

    fn ipcMessageQueueForSend(self: *KernelState, object_ref: KernelObjectRef) KernelError!*IpcQueue {
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

    fn markReplySentIfNeeded(self: *KernelState, object_ref: KernelObjectRef) void {
        const slot = self.kernelObjectSlot(object_ref) orelse return;
        switch (slot.payload) {
            .reply => |reply_ref| {
                const reply = self.ipcReplySlot(reply_ref) orelse return;
                reply.sent = true;
            },
            else => {},
        }
    }

    fn endpointRefEqual(a: IpcEndpointRef, b: IpcEndpointRef) bool {
        return a.index == b.index and a.generation == b.generation;
    }

    fn channelRefEqual(a: IpcChannelRef, b: IpcChannelRef) bool {
        return a.index == b.index and a.generation == b.generation;
    }

    fn replyRefEqual(a: IpcReplyRef, b: IpcReplyRef) bool {
        return a.index == b.index and a.generation == b.generation;
    }

    fn ipcPayloadReceivesFromSendPayload(send_payload: *const KernelObjectPayload, recv_payload: *const KernelObjectPayload) bool {
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

    pub fn ipcRecvWakeOwnersForSendFd(
        self: *const KernelState,
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
                if (!ipcPayloadReceivesFromSendPayload(&send_slot.payload, &recv_slot.payload)) continue;
                appendUniquePrincipal(out, &count, desc.principal);
                break;
            }
        }
        return count;
    }

    fn ipcMessageQueueForRecv(self: *KernelState, object_ref: KernelObjectRef) KernelError!*IpcQueue {
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

    fn fdFreeCountFrom(self: *const KernelState, owner: PrincipalId, min_fd: Fd) KernelError!usize {
        const table = try self.fdTableForActiveProcessConst(owner);
        var index = fdIndex(min_fd) orelse return KernelError.InvalidState;
        var count: usize = 0;
        while (index < fd_table_entries) : (index += 1) {
            if (table.entries[index].isEmpty()) count += 1;
        }
        return count;
    }

    fn validateIpcSendFds(specs: []const IpcSendFd) KernelError!void {
        if (specs.len > max_ipc_message_fds) return KernelError.InvalidState;
        for (specs, 0..) |spec, i| {
            if (!spec.move) continue;
            for (specs[i + 1 ..]) |later| {
                if (later.move and later.fd == spec.fd) return KernelError.InvalidState;
            }
        }
    }

    fn appendIpcSendFd(
        self: *KernelState,
        owner: PrincipalId,
        msg: *IpcMessage,
        spec: IpcSendFd,
    ) KernelError!void {
        if (msg.fd_count >= max_ipc_message_fds) return KernelError.InvalidState;
        const table = try self.fdTableForActiveProcessConst(owner);
        const index = fdIndex(spec.fd) orelse return KernelError.InvalidState;
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

    fn closeMovedIpcSendFdsWithFreeList(
        self: *KernelState,
        owner: PrincipalId,
        specs: []const IpcSendFd,
        free_list: *FreePageList,
    ) void {
        for (specs) |spec| {
            if (!spec.move) continue;
            self.closeFdWithFreeList(owner, spec.fd, free_list) catch {};
        }
    }

    fn buildIpcMessage(
        self: *KernelState,
        owner: PrincipalId,
        send: IpcSendMessage,
        free_list: *FreePageList,
    ) KernelError!IpcMessage {
        try validateIpcSendFds(send.fds);
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

    fn enqueueIpcMessage(
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        send: IpcSendMessage,
        require_call: bool,
        free_list: *FreePageList,
    ) KernelError!void {
        const table = try self.fdTableForActiveProcessConst(owner);
        const index = fdIndex(fd) orelse return KernelError.InvalidState;
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
        self: *KernelState,
        owner: PrincipalId,
        fd: Fd,
        send: IpcSendMessage,
        free_list: *FreePageList,
    ) KernelError!void {
        try self.requireActiveProcess(owner);
        try self.enqueueIpcMessage(owner, fd, send, false, free_list);
    }

    pub fn ipcReply(
        self: *KernelState,
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
        self: *KernelState,
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
        const reply_fd = try self.installFd(owner, reply_object, replyReceiveRights(), .{ .cloexec = true }, min_reply_fd);
        errdefer self.closeFdWithFreeList(owner, reply_fd, free_list) catch {};

        var msg = try self.buildIpcMessage(owner, send, free_list);
        errdefer self.releaseIpcMessageWithFreeList(&msg, free_list);
        try self.retainKernelObject(reply_object);
        msg.fds[msg.fd_count] = .{
            .object = reply_object,
            .rights = replySendRights(),
            .flags = .{ .cloexec = true },
        };
        msg.fd_count += 1;

        const table = try self.fdTableForActiveProcessConst(owner);
        const index = fdIndex(fd) orelse return KernelError.InvalidState;
        const entry = table.entries[index];
        if (entry.object.isNull() or !entry.rights.call) return KernelError.InvalidState;
        const queue = try self.ipcMessageQueueForSend(entry.object);
        try queue.push(msg);
        msg = .{};
        self.closeMovedIpcSendFdsWithFreeList(owner, send.fds, free_list);
        return reply_fd;
    }

    pub fn ipcRecv(
        self: *KernelState,
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

    fn nextProcessCapacity(self: *const KernelState, required: usize) ?usize {
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

    pub fn ensureProcessCapacity(self: *KernelState, required: usize, free_list: *FreePageList) bool {
        if (required <= self.process_capacity) return true;
        const capacity = self.nextProcessCapacity(required) orelse return false;
        const extra_count = capacity - process_count;
        const old_extra_count = self.process_capacity - process_count;

        const new_desc = allocKernelSlice(ProcessDescriptor, free_list, extra_count) orelse return false;
        const new_cap = allocKernelSlice(CNode, free_list, extra_count) orelse return false;
        const new_endpoint = allocKernelSlice(EndpointCNode, free_list, extra_count) orelse return false;
        const new_cap_mailbox = allocKernelSlice(CapMailbox, free_list, extra_count) orelse return false;
        const new_pending = allocKernelSlice(?PendingCapTransfer, free_list, extra_count) orelse return false;
        const new_fd = allocKernelSlice(FdTable, free_list, extra_count) orelse return false;
        const new_vma = allocKernelSlice(VmaTable, free_list, extra_count) orelse return false;

        @memcpy(new_desc[0..old_extra_count], self.process_descriptors_extra);
        @memcpy(new_cap[0..old_extra_count], self.cap_tables_extra);
        @memcpy(new_endpoint[0..old_extra_count], self.endpoint_tables_extra);
        @memcpy(new_cap_mailbox[0..old_extra_count], self.cap_mailboxes_extra);
        @memcpy(new_pending[0..old_extra_count], self.pending_page_transfers_extra);
        @memcpy(new_fd[0..old_extra_count], self.fd_tables_extra);
        @memcpy(new_vma[0..old_extra_count], self.vma_tables_extra);

        var i = old_extra_count;
        while (i < extra_count) : (i += 1) {
            new_desc[i] = .{};
            new_cap[i] = .{};
            new_endpoint[i] = .{};
            new_cap_mailbox[i] = .{};
            new_pending[i] = null;
            new_fd[i] = .{};
            new_vma[i] = .{};
        }

        self.process_descriptors_extra = new_desc;
        self.cap_tables_extra = new_cap;
        self.endpoint_tables_extra = new_endpoint;
        self.cap_mailboxes_extra = new_cap_mailbox;
        self.pending_page_transfers_extra = new_pending;
        self.fd_tables_extra = new_fd;
        self.vma_tables_extra = new_vma;
        self.process_capacity = capacity;
        return true;
    }

    fn clearPrincipalTablesForReuse(self: *KernelState, index: usize) void {
        self.resetProcessRuntimeTables(index);
    }

    pub fn createProcessDescriptor(self: *KernelState, label: []const u8) ?PrincipalId {
        var i: usize = 0;
        while (i < self.process_capacity) : (i += 1) {
            const desc = self.processDescriptorSlot(i) orelse continue;
            if (desc.active) continue;
            const principal = processPrincipal(i);
            self.clearPrincipalTablesForReuse(i);
            desc.* = .{
                .active = true,
                .principal = principal,
                .label = label,
            };
            self.active_process_count += 1;
            if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .create);
            return principal;
        }
        return null;
    }

    pub fn createProcessDescriptorWithCapacity(self: *KernelState, label: []const u8, free_list: *FreePageList) ?PrincipalId {
        if (self.createProcessDescriptor(label)) |principal| return principal;
        if (!self.ensureProcessCapacity(self.process_capacity + 1, free_list)) return null;
        return self.createProcessDescriptor(label);
    }

    pub fn ensureProcessDescriptor(self: *KernelState, principal: PrincipalId, label: []const u8) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        const desc = self.processDescriptorSlot(index) orelse return false;
        if (desc.active) {
            desc.label = label;
            return true;
        }
        self.clearPrincipalTablesForReuse(index);
        desc.* = .{
            .active = true,
            .principal = principal,
            .label = label,
        };
        self.active_process_count += 1;
        if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .ensure);
        return true;
    }

    pub fn setBootstrapOwner(self: *KernelState, principal: PrincipalId, enabled: bool) KernelError!void {
        const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
        const desc = self.processDescriptorSlot(index) orelse return KernelError.InvalidState;
        if (!desc.active) return KernelError.InvalidState;
        desc.bootstrap_owner = enabled;
    }

    pub fn markProcessFaulted(self: *KernelState, principal: PrincipalId, fault_vector: u8) bool {
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

    pub fn markProcessExited(self: *KernelState, principal: PrincipalId) bool {
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

    pub fn removeProcessDescriptor(self: *KernelState, principal: PrincipalId) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        const desc = self.processDescriptorSlot(index) orelse return false;
        if (!desc.active) return false;
        self.releaseFdTableForProcessIndex(index);
        desc.* = .{};
        if (self.active_process_count > 0) self.active_process_count -= 1;
        if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .remove);
        return true;
    }

    fn clearPrincipalState(self: *KernelState) void {
        var i: usize = 0;
        while (i < self.process_capacity) : (i += 1) {
            self.resetProcessRuntimeTables(i);
        }
        self.cap_tables[process_count].reset();
        self.endpoint_tables[process_count] = .{};
        self.cap_mailboxes[process_count].reset();
        self.pending_page_transfers[process_count] = null;
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

    fn initPrincipalState(self: *KernelState) void {
        self.clearPrincipalState();
        var i: usize = 0;
        while (i < initial_process_count) : (i += 1) {
            const principal = processPrincipal(i);
            (self.processDescriptorSlot(i) orelse unreachable).* = .{
                .active = true,
                .principal = principal,
                .label = principalLabel(principal),
            };
            self.active_process_count += 1;
        }
    }

    fn initDynamicPrincipalState(self: *KernelState) void {
        self.clearPrincipalState();
    }

    pub fn initPhase1InPlace(self: *KernelState) void {
        self.* = .{};
        clearPageCapRefTable();
        self.next_cap_id = 1;
        self.regions[0] = .{
            .id = 0,
        };
        self.region_len = 1;
        self.initPrincipalState();
        const root_id = self.allocCapId();
        self.getTable(default_process_principal).add(.{
            .paddr = 0x1000,
            .rights = .{ .cpu_read = true, .cpu_write = true, .dma = true, .grant = true },
            .cap_id = root_id,
            .root_cap_id = root_id,
            .parent_cap_id = 0,
        }) catch unreachable;
    }

    pub fn initPhase1() KernelState {
        var state: KernelState = undefined;
        state.initPhase1InPlace();
        return state;
    }

    pub fn initFromDetectedRegionsInPlace(self: *KernelState, region_count: usize) KernelError!void {
        if (region_count == 0) return KernelError.EmptyRegionSet;
        if (region_count > max_regions) return KernelError.TooManyRegions;

        self.* = .{};
        clearPageCapRefTable();
        self.next_cap_id = 1;
        if (builtin.is_test) {
            self.initPrincipalState();
        } else {
            self.initDynamicPrincipalState();
        }

        var i: usize = 0;
        while (i < region_count) : (i += 1) {
            self.regions[i] = .{
                .id = i,
            };
        }
        self.region_len = region_count;
    }

    pub fn initFromDetectedRegions(region_count: usize) KernelError!KernelState {
        var state: KernelState = undefined;
        try state.initFromDetectedRegionsInPlace(region_count);
        return state;
    }

    pub fn getRegion(self: *KernelState, region_id: u64) ?*Region {
        var i: usize = 0;
        while (i < self.region_len) : (i += 1) {
            if (self.regions[i].id == region_id) return &self.regions[i];
        }
        return null;
    }

    pub fn getRegionConst(self: *const KernelState, region_id: u64) ?*const Region {
        var i: usize = 0;
        while (i < self.region_len) : (i += 1) {
            if (self.regions[i].id == region_id) return &self.regions[i];
        }
        return null;
    }

    pub fn getTable(self: *KernelState, principal: PrincipalId) *CNode {
        if (processIndexFromPrincipal(principal)) |index| return self.capTableForProcessIndex(index);
        return &self.cap_tables[self.principalStorageIndex(principal)];
    }

    pub fn getTableConst(self: *const KernelState, principal: PrincipalId) *const CNode {
        if (processIndexFromPrincipal(principal)) |index| return self.capTableForProcessIndexConst(index);
        return &self.cap_tables[self.principalStorageIndex(principal)];
    }

    pub fn getEndpointTable(self: *KernelState, principal: PrincipalId) *EndpointCNode {
        if (processIndexFromPrincipal(principal)) |index| return self.endpointTableForProcessIndex(index);
        return &self.endpoint_tables[self.principalStorageIndex(principal)];
    }

    pub fn getEndpointTableConst(self: *const KernelState, principal: PrincipalId) *const EndpointCNode {
        if (processIndexFromPrincipal(principal)) |index| return self.endpointTableForProcessIndexConst(index);
        return &self.endpoint_tables[self.principalStorageIndex(principal)];
    }

    pub fn bumpEndpointGeneration(self: *KernelState) void {
        self.endpoint_generation +%= 1;
    }

    pub fn installEndpoint(
        self: *KernelState,
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
        self: *KernelState,
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
        self: *KernelState,
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

    pub fn unpublishServiceEndpointsForTarget(self: *KernelState, target: PrincipalId) bool {
        const removed = self.published_service_endpoints.removeTarget(target);
        if (removed) {
            self.bumpEndpointGeneration();
        }
        return removed;
    }

    pub fn endpointTargetFor(self: *const KernelState, owner: PrincipalId, endpoint_id: u64) ?PrincipalId {
        if (!self.hasActivePrincipal(owner)) return null;
        return self.endpointTargetForKnownActiveOwner(owner, endpoint_id);
    }

    pub fn endpointTargetForKnownActiveOwner(self: *const KernelState, owner: PrincipalId, endpoint_id: u64) ?PrincipalId {
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

    fn debugWriteField(
        write: *const fn ([]const u8) void,
        print_number: *const fn (u64) void,
        name: []const u8,
        value: u64,
    ) void {
        write(" ");
        write(name);
        write("=");
        print_number(value);
    }

    pub fn debugLogMemoryOwnership(
        self: *const KernelState,
        free_list: *const FreePageList,
        write: *const fn ([]const u8) void,
        print_number: *const fn (u64) void,
        where: []const u8,
    ) void {
        var page_caps_total: u64 = 0;
        var active_total: u64 = 0;

        var pidx: usize = 0;
        while (pidx < self.process_capacity) : (pidx += 1) {
            const page_table = self.capTableForProcessIndexConst(pidx);
            page_caps_total += @intCast(page_table.len);
            if ((self.processDescriptorSlotConst(pidx) orelse continue).active) active_total += 1;
        }

        const device_page_table = &self.cap_tables[process_count];
        page_caps_total += @intCast(device_page_table.len);

        write("Kernel.mem_diag where=");
        write(where);
        debugWriteField(write, print_number, "free_pages", @intCast(free_list.len));
        debugWriteField(write, print_number, "free_ranges", @intCast(free_list.range_len));
        debugWriteField(write, print_number, "process_capacity", @intCast(self.process_capacity));
        debugWriteField(write, print_number, "active_processes", active_total);
        debugWriteField(write, print_number, "tracked_active", @intCast(self.active_process_count));
        debugWriteField(write, print_number, "page_caps", page_caps_total);
        debugWriteField(write, print_number, "page_cap_unique", pageCapRefUniqueCount());
        debugWriteField(write, print_number, "vm_store_next", @intCast(vmo_backing_page_store_next));
        debugWriteField(write, print_number, "vm_store_free_pages", vmObjectBackingFreePageCount());
        debugWriteField(write, print_number, "vm_store_free_ranges", @intCast(vmo_backing_page_store_free_range_len));
        write("\n");

        pidx = 0;
        while (pidx < self.process_capacity) : (pidx += 1) {
            const page_table = self.capTableForProcessIndexConst(pidx);
            const desc = self.processDescriptorSlotConst(pidx) orelse continue;
            if (!desc.active and page_table.len == 0) continue;

            write("Kernel.mem_diag.proc");
            debugWriteField(write, print_number, "idx", @intCast(pidx));
            debugWriteField(write, print_number, "active", if (desc.active) 1 else 0);
            write(" label=");
            write(desc.label);
            debugWriteField(write, print_number, "page_caps", @intCast(page_table.len));
            write("\n");
        }

        if (device_page_table.len != 0) {
            write("Kernel.mem_diag.proc");
            debugWriteField(write, print_number, "idx", @intCast(process_count));
            debugWriteField(write, print_number, "active", 1);
            write(" label=Device0");
            debugWriteField(write, print_number, "page_caps", @intCast(device_page_table.len));
            write("\n");
        }
    }

    pub fn allocPhysicalPage(
        self: *KernelState,
        free_list: *FreePageList,
    ) KernelError!PageCapability {
        const paddr = free_list.popFrontAtOrAbove(low_memory_limit) catch |err| switch (err) {
            KernelError.OutOfFreePages => try free_list.popFrontBelow(low_memory_limit),
            else => return err,
        };
        errdefer free_list.appendPage(0, paddr) catch {};
        try self.zeroAllocatedPage(paddr);
        return .{ .paddr = paddr };
    }

    pub fn allocLowPhysicalPage(
        self: *KernelState,
        free_list: *FreePageList,
    ) KernelError!PageCapability {
        const paddr = try free_list.popFrontBelow(low_memory_limit);
        errdefer free_list.appendPage(0, paddr) catch {};
        try self.zeroAllocatedPage(paddr);
        return .{ .paddr = paddr };
    }

    fn zeroAllocatedPage(self: *KernelState, paddr: u64) KernelError!void {
        if (builtin.is_test) return;
        if (self.zero_physical_page_hook) |hook| {
            if (!hook(paddr)) return KernelError.InvalidState;
            return;
        }
        const page_bytes: [*]u8 = @ptrFromInt(paddr);
        @memset(page_bytes[0..4096], 0);
    }
};

fn containsCapId(ids: []const u64, target: u64) bool {
    for (ids) |id| {
        if (id == target) return true;
    }
    return false;
}

test "ipc send wake owner scan follows channel receive side" {
    var state = KernelState.initPhase1();
    const sender = processPrincipalFromIndex(0).?;
    const receiver = processPrincipalFromIndex(1).?;
    const rights = FdRights{
        .inspect = true,
        .transfer = true,
        .wait = true,
        .poll = true,
        .close = true,
        .send = true,
        .recv = true,
    };
    const pair = try state.createIpcChannelPairFds(sender, rights, .{}, 16);
    const receiver_fd = try state.transferFd(sender, receiver, pair.b, 16, rights, .{}, .move);
    try std.testing.expect(receiver_fd >= 16);

    var owners: [8]PrincipalId = undefined;
    const count = try state.ipcRecvWakeOwnersForSendFd(sender, pair.a, owners[0..]);
    try std.testing.expectEqual(@as(usize, 1), count);
    try std.testing.expectEqual(receiver, owners[0]);
}

test "ipc send wake owner scan includes endpoint receiver" {
    var state = KernelState.initPhase1();
    const owner = processPrincipalFromIndex(0).?;
    const rights = FdRights{
        .inspect = true,
        .wait = true,
        .poll = true,
        .close = true,
        .send = true,
        .recv = true,
    };
    const endpoint = try state.createIpcEndpointFd(owner, rights, .{}, 16);

    var owners: [8]PrincipalId = undefined;
    const count = try state.ipcRecvWakeOwnersForSendFd(owner, endpoint, owners[0..]);
    try std.testing.expectEqual(@as(usize, 1), count);
    try std.testing.expectEqual(owner, owners[0]);
}

test "mmapFdIntoProcess installs vmo fd into target vma table" {
    var state = KernelState.initPhase1();
    const source = processPrincipalFromIndex(0).?;
    const target = processPrincipalFromIndex(1).?;
    const vmo_fd = try state.createAnonymousVmoFd(source, native_page_size, .{
        .inspect = true,
        .close = true,
        .map_read = true,
        .map_exec = true,
    }, .{}, 16);

    const mapped = try state.mmapFdIntoProcess(
        source,
        vmo_fd,
        target,
        0x400000,
        native_page_size,
        .{ .read = true, .exec = true },
        .{ .fixed = true, .shared = true },
        0,
    );
    try std.testing.expectEqual(@as(u64, 0x400000), mapped);

    const vma = state.vmaEntryConst(target, 0x400000) orelse return error.TestExpectedEqual;
    try std.testing.expectEqual(native_page_size, vma.size_bytes);
    try std.testing.expect(vma.prot.read);
    try std.testing.expect(vma.prot.exec);
    try std.testing.expect(!vma.prot.write);
    try std.testing.expect(vma.flags.fixed);
    try std.testing.expect(vma.flags.shared);
}
