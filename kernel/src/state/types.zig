pub const references = @import("../memory/reference_index.zig");
const slot_storage = @import("../memory/slot_table.zig");
const lock_diagnostics = @import("../sync/lock_diagnostics.zig");
const std = @import("std");
const builtin = @import("builtin");
pub const capsule = @import("../capsule.zig");
const table = @import("table.zig");
const owned_storage = @import("../memory/owned_storage.zig");
const physical_layout = @import("../arch/x86_64/physical_layout.zig");
pub const initial_process_count: usize = 8;
pub const initial_process_capacity: usize = 32;
pub const process_count: usize = initial_process_capacity;
// Number of representable nonnegative Linux pid_t process principals;
// this is an ABI value range, not an allocation quota.
pub const process_principal_value_count: usize = @as(usize, 1) << 31;
pub const initial_thread_capacity: usize = 32;
// Number of representable GETTID values: index+1 in u32, excluding zero.
// Storage grows on demand within this ABI value range.
pub const thread_id_value_count: usize = std.math.maxInt(u32);
// Outside the Linux pid_t process range, independent of directory capacity.
pub const device_principal_raw: u32 = std.math.maxInt(u32);
pub const max_process_generation: u32 = std.math.maxInt(i32);

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
    if (index >= process_principal_value_count) return null;
    return @enumFromInt(@as(PrincipalRaw, @intCast(index)));
}

pub fn processIndexFromPrincipal(principal: PrincipalId) ?usize {
    const index: usize = @intFromEnum(principal);
    if (index >= process_principal_value_count) return null;
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

pub const principal_syscall_columns = if (builtin.is_test or (@hasDecl(@import("root"), "ipc_profile_enabled") and @import("root").ipc_profile_enabled)) 6 else 0;

pub const ProcessDescriptor = struct {
    signal_timer: ?slot_storage.Ref = null,
    // Descriptor-leaf protected totals survive directory relocation. They are
    // diagnostic slot totals, never capability identity.
    syscall_counts: [principal_syscall_columns]u64 = .{0} ** principal_syscall_columns,
    active: bool = false,
    principal: PrincipalId = @enumFromInt(0),
    generation: u32 = 0,
    label: []const u8 = "",
    bootstrap_owner: bool = false,
    faulted: bool = false,
    fault_vector: u8 = 0,
    signal_pending_hint_va: u64 = 0,
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

pub const KernelError = @import("errors.zig").KernelError;

pub const DmaDeviceId = u64;
pub const invalid_dma_device_id: DmaDeviceId = 0;

pub const Fd = u32;
pub const fd_table_entries: usize = 256;
// The top two u32 values are ABI sentinels (thread_self_fd and invalid FD).
pub const fd_number_capacity: usize = @import("kernel_abi_root").fd_abi.thread_self_fd;
pub const pipe_buffer_bytes: usize = 4096;
pub const fd_known_flags_mask: u32 = (@as(u32, 1) << 4) - 1;
pub const fd_known_rights_mask: u64 = (@as(u64, 1) << 47) - 1;

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
    cache_policy: bool = false,
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
    controller_write: bool = false,
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
    _reserved: u17 = 0,
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
    platform_device = 9,
    mmio_region = 10,
    dma_buffer = 11,
    dma_mapping = 12,
    irq = 13,
    timer = 14,
    serial = 15,
    pipe = 16,
    pager_port = 17,
};

comptime {
    const fd_abi = @import("kernel_abi_root").fd_abi;
    for (@typeInfo(KernelObjectKind).@"enum".fields) |field| {
        std.debug.assert(field.value == @field(fd_abi, "fd_kind_" ++ field.name));
    }
    for (@typeInfo(FdRights).@"struct".fields) |field| {
        const name = "right_" ++ field.name;
        if (@hasDecl(fd_abi, name)) std.debug.assert(@field(fd_abi, name) == @as(u64, 1) << @bitOffsetOf(FdRights, field.name));
    }
}

pub const TaskObjectState = enum(u8) {
    active = 1,
    exited = 2,
    killed = 3,
    stopped = 4,
    continued = 5,

    pub fn isTerminal(self: TaskObjectState) bool {
        return self == .exited or self == .killed;
    }
};

pub const ProcessObject = struct {
    principal_raw: PrincipalRaw = 0,
    generation: u32 = 0,
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

pub const PlatformDeviceObject = struct {
    grant: @import("../platform_device.zig").Grant,
};

pub const IrqSource = union(enum) {
    pci: DmaDeviceId,
    platform: @import("../platform_device.zig").IrqGrant,

    pub fn eql(a: IrqSource, b: IrqSource) bool {
        return switch (a) {
            .pci => |device| b == .pci and b.pci == device,
            .platform => |grant| b == .platform and std.meta.eql(grant, b.platform),
        };
    }
    pub fn resourceId(self: IrqSource) u64 {
        return switch (self) { .pci => |device| device, .platform => |grant| @intFromEnum(grant.device.resource) };
    }
};

pub const MmioRegionObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
    device: DmaDeviceId = 0,
    bar_index: u32 = 0,
    paddr: u64 = 0,
    user_va: u64 = 0,
    size: u64 = 0,
    flags: u32 = 0,
    // Internal ownership of the physical WC copy-window guard and, for
    // low addresses, temporary identity alias suppression.
    // This is not part of the capsule ABI.
    wc_window_lease_token: u64 = 0,
};

pub const DmaBufferObject = struct {
    retention: ?*@import("../memory/dma_retention.zig").Record = null,
    owner_principal_raw: PrincipalRaw = 0,
    device: DmaDeviceId = 0,
    user_va: u64 = 0,
    iova: u64 = 0,
    size: u64 = 0,
    flags: u32 = 0,
};

pub const DmaMappingObject = struct {
    retention: ?*@import("../memory/dma_retention.zig").Record = null,
    owner_principal_raw: PrincipalRaw = 0,
    device: DmaDeviceId = 0,
    user_va: u64 = 0,
    iova: u64 = 0,
    size: u64 = 0,
    /// Non-zero only for a scatter derive and exposed by the existing query
    /// ABI. Teardown derives the owned contiguous IOVA span from iova+size.
    page_count: u16 = 0,
    direction: CapsuleDmaDirection = .bidirectional,
    flags: u32 = 0,
};

pub const IrqObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
    source: IrqSource = .{ .pci = 0 },
    kind: CapsuleIrqKind = .auto,
    vector: u32 = 0,
    event_count: u64 = 0,
    flags: u32 = 0,
    /// Shared across FD aliases. Retirement has already drained hardware and
    /// released the lease; destruction must never affect its replacement.
    retired: bool = false,
};

pub const IrqPublishSlot = struct {
    active: u8 = 0,
    generation: u32 = 0,
    owner_principal_raw: PrincipalRaw = 0,
    source: IrqSource = .{ .pci = 0 },
    kind: u8 = 0,
    vector: u32 = 0,
    event_count: u64 = 0,
    observed_count: u64 = 0,
};

pub const TimerObject = struct {
    owner_principal_raw: PrincipalRaw = 0,
    deadline_ns: u64 = 0,
    interval_ns: u64 = 0,
    flags: u32 = 0,
};

pub const TimerFdState = struct {
    remaining_ns: u64 = 0,
    interval_ns: u64 = 0,
};

pub const SerialObject = struct {
    stream: u8 = 0,
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
    platform_device: PlatformDeviceObject,
    mmio_region: MmioRegionObject,
    dma_buffer: DmaBufferObject,
    dma_mapping: DmaMappingObject,
    irq: IrqObject,
    timer: TimerObject,
    serial: SerialObject,
    pipe: PipeEndpointObject,
    pager_port: PagerPortRef,
};

// Only revoke-traversable objects need holder edges. The payload edge is
// either VMO ownership or a weak process pin; these kinds are disjoint.
pub const ObjectReferences = struct {
    holders: references.Root = .{},
    payload_edge: references.Node = .{},
};
pub const KernelObjectSlot = struct {
    live: @import("../sync/ipc_lifetime.zig").Admission = .{},
    retired_next: ?*KernelObjectSlot = null,
    reference_state: ?*ObjectReferences = null,
    irq_publication: ?slot_storage.Ref = null,
    kind: KernelObjectKind = .none,
    generation: u32 = 1,
    ref_count: u32 = 0,
    // Weak observers do not own communication peers. This index avoids
    // scanning every retained set for an unrelated temporary VMO close.
    persistent_observers: u32 = 0,
    payload: KernelObjectPayload = .{ .none = {} },
};

pub var fd_test_write_bytes: if (builtin.is_test) usize else void = if (builtin.is_test) 0 else {};

pub const FdEntry = struct {
    seq: u64 = 0,
    // object.kind discriminates both words. IPC uses a borrowed queue and an
    // independent body generation; tracked objects (and empty fork drafts)
    // use a reference node. K owns these fields; canonical value snapshots exclude them.
    // Reusing the two existing words avoids increasing FD-table capacity cost.
    authority: extern union { reference: ?*references.Node, ipc_generation: u64 } = .{ .reference = null },
    object: KernelObjectRef = .{},
    rights: FdRights = .{},
    flags: FdFlags = .{},
    cursor: extern union { offset: u64, ipc_queue: ?*IpcQueue } = .{ .offset = 0 },

    comptime {
        // FD4200 is already capacity-constrained. Caching authority must not
        // increase the existing 48-byte entry or its chunk alignment.
        std.debug.assert(@sizeOf(FdEntry) == 48 and @alignOf(FdEntry) == 8);
    }

    pub fn isEmpty(self: *const FdEntry) bool {
        return self.object.isNull();
    }

    // K serializes writers. SC on BOTH seq and fields gives one total order:
    // an interleaved field store implies an odd or changed second seq. Plain
    // seqlock field accesses would still be a language-level data race.
    pub fn replace(self: *FdEntry, value: FdEntry) void {
        if (builtin.is_test) fd_test_write_bytes += @sizeOf(KernelObjectKind) + 4 + 4 + 8 + 4;
        const seq = @atomicLoad(u64, &self.seq, .seq_cst);
        const odd = if (seq == std.math.maxInt(u64)) seq else seq + 1;
        std.debug.assert(seq & 1 == 0 or seq == std.math.maxInt(u64));
        @atomicStore(u64, &self.seq, odd, .seq_cst);
        @atomicStore(KernelObjectKind, &self.object.kind, value.object.kind, .seq_cst);
        @atomicStore(u32, &self.object.index, value.object.index, .seq_cst);
        @atomicStore(u32, &self.object.generation, value.object.generation, .seq_cst);
        @atomicStore(u64, @as(*u64, @ptrCast(&self.rights)), @bitCast(value.rights), .seq_cst);
        @atomicStore(u32, @as(*u32, @ptrCast(&self.flags)), @bitCast(value.flags), .seq_cst);
        self.authority = value.authority;
        self.cursor = value.cursor;
        // Saturation closes this entry's fast admission, never wraps into ABA.
        @atomicStore(u64, &self.seq, if (odd == std.math.maxInt(u64)) odd else odd + 1, .seq_cst);
    }
    pub fn current(self: *const FdEntry, seq: u64) bool {
        return seq & 1 == 0 and @atomicLoad(u64, &self.seq, .seq_cst) == seq;
    }
    pub fn readFields(self: *const FdEntry, seq: u64) FdCapability {
        return .{ .generation = seq, .object = .{
            .kind = @atomicLoad(KernelObjectKind, &self.object.kind, .seq_cst),
            .index = @atomicLoad(u32, &self.object.index, .seq_cst),
            .generation = @atomicLoad(u32, &self.object.generation, .seq_cst),
        }, .rights = @bitCast(@atomicLoad(u64, @as(*const u64, @ptrCast(&self.rights)), .seq_cst)), .flags = @bitCast(@atomicLoad(u32, @as(*const u32, @ptrCast(&self.flags)), .seq_cst)) };
    }
    pub fn read(self: *const FdEntry) ?FdCapability {
        const seq = @atomicLoad(u64, &self.seq, .seq_cst);
        if (seq & 1 != 0) return null;
        const value = self.readFields(seq);
        return if (self.current(seq)) value else null;
    }
};

pub const FdInfo = struct {
    kind: KernelObjectKind = .none,
    rights_bits: u64 = 0,
    flags_bits: u32 = 0,
    size_bytes: u64 = 0,
    extra: u64 = 0,
};

// A bounded local capability value, never an allocated table publication.
pub const FdCapability = struct {
    generation: u64 = 0,
    object: KernelObjectRef = .{},
    rights: FdRights = .{},
    flags: FdFlags = .{},
};
pub const FdTable = struct {
    incarnation: u64 = 0,
    // Publication changes require retained observers to revalidate descriptor
    // incarnations, including replacement with an alias of the same object.
    mutation: u64 = 0,
    retired_next: ?*FdTable = null,
    retirement_queued: bool = false,
    entries: slot_storage.SlotTable(FdEntry) = .{},
    capacity: usize = 0,

    pub fn at(self: anytype, i: usize) if (@typeInfo(@TypeOf(self)).pointer.is_const) *const FdEntry else *FdEntry {
        std.debug.assert(i < self.capacity);
        return self.entries.get(@intCast(i)).?;
    }
    pub fn index(self: *const FdTable, fd: Fd) ?usize {
        return if (fd < self.capacity) @intCast(fd) else null;
    }
    pub fn write(self: *FdTable, i: usize, value: FdEntry) void {
        self.mutation +|= 1;
        self.at(i).replace(value);
        self.entries.mark(@intCast(i), 1, value.isEmpty());
    }
    pub fn ensureCapacity(self: *FdTable, minimum: usize, allocator: owned_storage.Allocator, leaf: *slot_storage.Leaf, domain: *owned_storage.RetiredQueue) KernelError!void {
        if (minimum > fd_number_capacity) return KernelError.InvalidState;
        self.entries.return_queue = domain;
        self.entries.reclaim();
        if (minimum <= self.capacity) return;
        const target = @min(fd_number_capacity, @max(minimum, @max(fd_table_entries, self.capacity * 2)));
        // Append bounded radix paths/chunks, never copy existing entries.
        // Partial growth remains prepared for retry; public capacity is last.
        while (self.entries.live < target) {
            const allocated = self.entries.allocate(allocator, leaf, std.math.maxInt(u32)) catch return KernelError.OutOfFreePages;
            std.debug.assert(allocated.ref.index + 1 == self.entries.live);
            self.entries.mark(allocated.ref.index, 1, true);
        }
        const flags = leaf.lock();
        @atomicStore(usize, &self.capacity, target, .release);
        leaf.unlock(flags);
    }
};

pub const FdTransferMode = enum(u8) {
    copy,
    move,
};

pub const max_ipc_queue_messages: usize = 16;
pub const max_ipc_message_fds: usize = 19;

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

pub const FdWaitRegistrationKind = enum(u8) {
    none = 0,
    task = 1,
    pipe = 2,
    endpoint = 3,
    channel = 4,
    reply = 5,
};

pub const FdWaitRegistrationRef = struct {
    kind: FdWaitRegistrationKind = .none,
    side: u8 = 0,
    slot: u32 = 0,
    slot_generation: u32 = 0,
    reserved0: u8 = 0,
    index: u32 = 0,
    generation: u32 = 0,

    pub fn isNull(self: FdWaitRegistrationRef) bool {
        return self.kind == .none;
    }

    pub fn matches(self: FdWaitRegistrationRef, other: FdWaitRegistrationRef) bool {
        return self.kind == other.kind and
            self.side == other.side and
            self.slot == other.slot and
            self.slot_generation == other.slot_generation and
            self.index == other.index and
            self.generation == other.generation;
    }
};

pub const FdWaitGroupRef = struct {
    index: u32 = 0,
    generation: u32 = 0,

    pub fn isNull(self: FdWaitGroupRef) bool {
        return self.generation == 0;
    }

    pub fn matches(self: FdWaitGroupRef, other: FdWaitGroupRef) bool {
        return self.index == other.index and self.generation == other.generation;
    }
};

pub const FdWaitGroupState = enum(u8) {
    free = 0,
    building = 1,
    armed = 2,
    completing = 3,
};

pub const FdWaitGroupSlot = struct {
    // Kernel-owned snapshot base: derive singleton indices without rereading user memory.
    pollfds_va: u64 = 0,
    state: FdWaitGroupState = .free,
    generation: u32 = 1,
    owner: PrincipalId = default_process_principal,
    thread_index: u32 = 0,
    thread_generation: u32 = 0,
    wait_token: u64 = 0,
    link_count: usize = 0,
    links: ?*slot_storage.SlotTable(FdWaitLink) = null,
};

pub const FdWaitGroupSnapshot = struct {
    group: FdWaitGroupRef = .{},
    owner: PrincipalId = default_process_principal,
    thread_index: u32 = 0,
    thread_generation: u32 = 0,
    wait_token: u64 = 0,
};

pub const FdWaitLink = struct {
    registration: FdWaitRegistrationRef = .{},
    ready: ?ThreadWakeTarget = null,
};

pub const FdWaitBinding = struct {
    persistent: ?*@import("persistent_wait_types.zig").Item = null,
    link: ?*FdWaitLink = null,
    group: FdWaitGroupRef = .{},
    link_index: u32 = 0,
    wait_token: u64 = 0,
    completion_pending: bool = false,
    seed_emitted: bool = false,

    // Readiness survives the bounded seed batch. The claimed group owns these
    // snapshots until the result is staged in registers or copied and the group is retired.
    pub fn appendReady(self: *FdWaitBinding, target: ThreadWakeTarget, out: []ThreadWakeTarget, count: *usize) void {
        if (self.persistent) |item| {
            item.publish();
            return;
        }
        if (self.link) |link| {
            if (link.ready) |*ready| ready.revents |= target.revents else link.ready = target;
        }
        self.completion_pending = true;
        if (!self.seed_emitted and count.* < out.len) {
            self.seed_emitted = true;
            out[count.*] = target;
            count.* += 1;
        }
    }
};

pub const IpcWaiter = struct {
    prev: ?*IpcWaiter = null,
    next: ?*IpcWaiter = null,
    list: ?*IpcWaitList = null,
    active: bool = false,
    owner: PrincipalId = default_process_principal,
    thread_index: u32 = 0,
    thread_generation: u32 = 0,
    pollfd_va: u64 = 0,
    recv_memory: @import("kernel_abi_root").ipc_abi.ReceiveMemory = .{},
    recv_fd: Fd = 0,
    recv_fd_capacity: u8 = 0,
    events: u64 = 0,
    min_write_bytes: u64 = 0,
    key: IpcWaitKey = .{},
    registration: FdWaitRegistrationRef = .{},
    binding: FdWaitBinding = .{},
};

pub const ThreadWakeTarget = struct {
    owner: PrincipalId = default_process_principal,
    thread_index: usize = 0,
    thread_generation: u32 = 0,
    wait_token: u64 = 0,
    pollfd_va: u64 = 0,
    recv_memory: @import("kernel_abi_root").ipc_abi.ReceiveMemory = .{},
    recv_fd: Fd = 0,
    recv_fd_capacity: u8 = 0,
    revents: u64 = 0,
    registration: FdWaitRegistrationRef = .{},
    group: FdWaitGroupRef = .{},
    group_link_index: u32 = 0,
    receive_block: IpcReplyRef = .{},
};

/// Scheduling hint for an IPC object whose consumer is runnable rather than
/// currently registered in the object's wait list.  The generation prevents a
/// recycled thread-table slot from inheriting the hint.
pub const IpcReceiverHint = struct {
    active: bool = false,
    owner: PrincipalId = default_process_principal,
    thread_index: u32 = 0,
    thread_generation: u32 = 0,
};

/// The one thread parked in IPC_NOTIFY_WAIT on a channel side.  A doorbell has
/// a single elected receiver, and keeping it out of IpcWaitList avoids the
/// generic FD wait-group begin/register/arm/reclaim that dominated a blocking
/// receive.  The wait token alone validates it: timeout, signal or exit retire
/// the token, so a leftover entry is rejected at claim time.
pub const IpcNotifyWaiter = struct {
    active: bool = false,
    owner: PrincipalId = default_process_principal,
    thread_index: u32 = 0,
    thread_generation: u32 = 0,
    wait_token: u64 = 0,
};

pub const TaskFdWaiter = struct {
    active: bool = false,
    principal_raw: PrincipalRaw = 0,
    object: KernelObjectRef = .{},
    owner: PrincipalId = default_process_principal,
    pollfd_va: u64 = 0,
    events: u64 = 0,
    thread_index: u32 = 0,
    thread_generation: u32 = 0,
    registration: FdWaitRegistrationRef = .{},
    binding: FdWaitBinding = .{},
};

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
    exclusion: ?*@import("../sync/ipc_endpoint.zig").Lock = null,
    // The owning endpoint serializes this FIFO with publication and close. The
    // receive binding owns the block until unlink, including terminal races.
    receive_head: ?*IpcReplySlot = null,
    receive_tail: ?*IpcReplySlot = null,
    // FD_WAIT_MANY nodes are stable slot-table bodies. Endpoint exclusion owns this
    // queue; unlink must precede slot removal/reuse on every cancel/close path.
    poll_head: ?*IpcWaiter = null,
    poll_tail: ?*IpcWaiter = null,
    waiters: slot_storage.SlotTable(IpcWaiter) = .{},
    allocator: ?@import("../memory/owned_storage.zig").Allocator = null,

    pub fn assertHeld(self: *const IpcWaitList) void {
        if (self.exclusion) |lock| lock.assertHeld();
    }

    pub fn acquire(self: *const IpcWaitList) @import("../sync/ipc_endpoint.zig").Lock.Guard {
        return if (self.exclusion) |lock| lock.acquire() else .{ .lock = null, .restore = false };
    }

    /// Requires K and E: K protects the persistent FIFO and invocation, E the
    /// source list. Data operations cannot close a peer under K/E, so HUP-only
    /// observers need no publication. An idle observer already in the ready
    /// FIFO also owns the hint until REUSE rechecks the source under K. If that
    /// recheck finds nothing, it unqueues before arming: the next producer must
    /// publish again. Active invocations retain ordinary XSAVE completion.
    /// Close paths still walk the original list, including these observers.
    pub fn dataPollAfter(self: *const IpcWaitList, previous: ?*IpcWaiter) ?*IpcWaiter {
        self.assertHeld();
        const abi = @import("kernel_abi_root").fd_abi;
        var cursor = if (previous) |node| node.next else self.poll_head;
        while (cursor) |node| : (cursor = node.next) {
            if (!node.active or node.binding.persistent == null or node.recv_memory.active) return node;
            if (node.events & (abi.event_readable | abi.event_writable) != 0) {
                const item = node.binding.persistent.?;
                if (!item.queued or item.set.invocation != null) return node;
            }
        }
        return null;
    }

    pub fn appendReceive(self: *IpcWaitList, block: *IpcReplySlot) void {
        self.assertHeld();
        std.debug.assert(block.receive_list == null);
        block.receive_list = self;
        block.receive_prev = self.receive_tail;
        block.receive_next = null;
        if (self.receive_tail) |tail| tail.receive_next = block else self.receive_head = block;
        self.receive_tail = block;
    }
    pub fn unlinkReceive(self: *IpcWaitList, block: *IpcReplySlot) void {
        self.assertHeld();
        std.debug.assert(block.receive_list == self);
        if (block.receive_prev) |prev| prev.receive_next = block.receive_next else self.receive_head = block.receive_next;
        if (block.receive_next) |next| next.receive_prev = block.receive_prev else self.receive_tail = block.receive_prev;
        block.receive_list = null;
        block.receive_next = null;
        block.receive_prev = null;
    }
    pub fn takeDirectReceive(self: *IpcWaitList) ?ThreadWakeTarget {
        const block = self.receive_head orelse return null;
        const target = block.receive_target;
        std.debug.assert(target.receive_block.index == block.ref.index and target.receive_block.generation == block.ref.generation);
        self.unlinkReceive(block);
        return target;
    }

    pub fn register(
        self: *IpcWaitList,
        key: IpcWaitKey,
        owner: PrincipalId,
        pollfd_va: u64,
        recv_memory: @import("kernel_abi_root").ipc_abi.ReceiveMemory,
        recv_fd: Fd,
        recv_fd_capacity: u8,
        events: u64,
        min_write_bytes: u64,
        thread_index: usize,
        thread_generation: u32,
        wait_token: u64,
        group: FdWaitGroupRef,
    ) KernelError!FdWaitRegistrationRef {
        self.assertHeld();
        if (thread_index > std.math.maxInt(u32)) return KernelError.InvalidState;
        if (wait_token == 0 or group.isNull()) return KernelError.InvalidState;
        const thread_index_u32: u32 = @intCast(thread_index);
        var iterator = self.waiters.iterator();
        while (iterator.next()) |item| {
            const waiter = item.value;
            if (waiter.thread_index == thread_index_u32 and
                waiter.thread_generation == thread_generation and
                waiter.binding.wait_token == wait_token and
                waiter.binding.group.matches(group) and
                waiter.pollfd_va == pollfd_va and
                std.meta.eql(waiter.recv_memory, recv_memory))
            {
                waiter.events |= events;
                waiter.key = key;
                waiter.owner = owner;
                waiter.pollfd_va = pollfd_va;
                waiter.recv_memory = recv_memory;
                waiter.recv_fd = recv_fd;
                waiter.recv_fd_capacity = recv_fd_capacity;
                if (min_write_bytes > waiter.min_write_bytes) waiter.min_write_bytes = min_write_bytes;
                return waiter.registration;
            }
        }
        const allocation = self.waiters.allocate(self.allocator orelse return KernelError.OutOfFreePages, null, std.math.maxInt(u32)) catch return KernelError.OutOfFreePages;
        const target = allocation.ref.index;
        const registration = FdWaitRegistrationRef{
            .kind = switch (key.kind) {
                .pipe => .pipe,
                .endpoint => .endpoint,
                .channel => .channel,
                .reply => .reply,
                else => return KernelError.InvalidState,
            },
            .side = key.side,
            .slot = @intCast(target),
            .slot_generation = allocation.ref.generation,
            .index = key.index,
            .generation = key.generation,
        };
        allocation.value.* = .{
            .list = self,
            .active = true,
            .owner = owner,
            .thread_index = thread_index_u32,
            .thread_generation = thread_generation,
            .pollfd_va = pollfd_va,
            .recv_memory = recv_memory,
            .recv_fd = recv_fd,
            .recv_fd_capacity = recv_fd_capacity,
            .events = events,
            .min_write_bytes = min_write_bytes,
            .key = key,
            .registration = registration,
            .binding = .{
                .wait_token = wait_token,
                .group = group,
            },
        };
        const node = allocation.value;
        node.prev = self.poll_tail;
        if (self.poll_tail) |tail| tail.next = node else self.poll_head = node;
        self.poll_tail = node;
        return registration;
    }

    pub fn removeWaiter(self: *IpcWaitList, waiter: *IpcWaiter) void {
        self.assertHeld();
        std.debug.assert(waiter.list == self and waiter.active);
        if (waiter.prev) |prev| prev.next = waiter.next else self.poll_head = waiter.next;
        if (waiter.next) |next| next.prev = waiter.prev else self.poll_tail = waiter.prev;
        waiter.list = null;
        waiter.prev = null;
        waiter.next = null;
        self.waiters.remove(waiter, null);
    }

    pub fn unregister(self: *IpcWaitList, thread_index: usize, thread_generation: u32) void {
        self.assertHeld();
        if (thread_index > std.math.maxInt(u32)) return;
        const thread_index_u32: u32 = @intCast(thread_index);
        var iterator = self.waiters.iterator();
        while (iterator.next()) |item| {
            const waiter = item.value;
            if (waiter.binding.persistent != null) continue;
            if (!waiter.active) continue;
            if (waiter.thread_index == thread_index_u32 and waiter.thread_generation == thread_generation) {
                self.removeWaiter(waiter);
            }
        }
    }

    pub fn takeReceiver(self: *IpcWaitList) ?ThreadWakeTarget {
        self.assertHeld();
        var cursor = self.poll_head;
        while (cursor) |w| : (cursor = w.next) {
            if (!w.active or !w.recv_memory.active or w.binding.completion_pending) continue;
            var out: [1]ThreadWakeTarget = undefined;
            var count: usize = 0;
            w.binding.appendReady(.{ .owner = w.owner, .thread_index = w.thread_index, .thread_generation = w.thread_generation, .wait_token = w.binding.wait_token, .recv_memory = w.recv_memory, .recv_fd = w.recv_fd, .recv_fd_capacity = w.recv_fd_capacity, .registration = w.registration, .group = w.binding.group, .group_link_index = w.binding.link_index }, &out, &count);
            if (count != 0) return out[0];
        }
        return self.takeDirectReceive();
    }

    pub fn takeReadable(self: *IpcWaitList, out: []ThreadWakeTarget) usize {
        const fd_abi = @import("kernel_abi_root").fd_abi;
        return self.takeEvents(fd_abi.event_readable, out);
    }

    pub fn takeEvents(self: *IpcWaitList, ready_events: u64, out: []ThreadWakeTarget) usize {
        return self.takeEventsWithWritableBytes(ready_events, std.math.maxInt(u64), out);
    }

    pub fn takeEventsWithWritableBytes(self: *IpcWaitList, ready_events: u64, writable_bytes: u64, out: []ThreadWakeTarget) usize {
        return self.takeMatching(false, ready_events, writable_bytes, out);
    }

    /// A channel notification is readiness without a message.  Taking a
    /// RECV_WAIT registration for it would mark it completion-pending and wake
    /// it with NOT_READY, so those stay armed for a real message.
    pub fn takeReadablePollWaiters(self: *IpcWaitList, out: []ThreadWakeTarget) usize {
        const fd_abi = @import("kernel_abi_root").fd_abi;
        return self.takeMatching(true, fd_abi.event_readable, std.math.maxInt(u64), out);
    }

    fn takeMatching(self: *IpcWaitList, comptime poll_only: bool, ready_events: u64, writable_bytes: u64, out: []ThreadWakeTarget) usize {
        self.assertHeld();
        if (out.len == 0) return 0;
        const fd_abi = @import("kernel_abi_root").fd_abi;
        var count: usize = 0;
        var cursor = self.poll_head;
        // Visit all matching nodes even when the seed array fills: claiming
        // one seed detaches its entire FD group, whose remaining readiness
        // snapshots must already be recorded in the group's stable links.
        while (cursor) |waiter| : (cursor = waiter.next) {
            if (!waiter.active) continue;
            if (poll_only and waiter.recv_memory.active) continue;
            var revents = ready_events & (waiter.events | fd_abi.event_error | fd_abi.event_hangup);
            if ((revents & fd_abi.event_writable) != 0 and waiter.min_write_bytes != 0 and writable_bytes < waiter.min_write_bytes) {
                revents &= ~fd_abi.event_writable;
            }
            if (revents == 0) continue;
            const target = ThreadWakeTarget{
                .owner = waiter.owner,
                .thread_index = waiter.thread_index,
                .thread_generation = waiter.thread_generation,
                .wait_token = waiter.binding.wait_token,
                .pollfd_va = waiter.pollfd_va,
                .recv_memory = waiter.recv_memory,
                .recv_fd = waiter.recv_fd,
                .recv_fd_capacity = waiter.recv_fd_capacity,
                .revents = revents,
                .registration = waiter.registration,
                .group = waiter.binding.group,
                .group_link_index = waiter.binding.link_index,
            };
            waiter.binding.appendReady(target, out, &count);
        }
        if (!poll_only and (ready_events & (fd_abi.event_readable | fd_abi.event_hangup | fd_abi.event_error)) != 0) {
            while (count < out.len) {
                var target = self.takeDirectReceive() orelse break;
                target.revents = ready_events;
                out[count] = target;
                count += 1;
            }
        }
        return count;
    }
};

pub const IpcTransferredFd = packed struct(u128) {
    // Queue-private encoding. Full u32 slot/generation and all known rights
    // fit without padding; this keeps a channel within three PMM pages.
    // Public IPC/FD layouts are unrelated and must never use these bytes.
    object_index: u32 = 0,
    object_generation: u32 = 0,
    object_kind: u8 = 0,
    permission_bits: u47 = 0,
    flag_bits: u4 = 0,
    reserved: u5 = 0,

    comptime {
        std.debug.assert(fd_known_rights_mask >> 47 == 0 and fd_known_flags_mask <= 15);
        for (@typeInfo(KernelObjectKind).@"enum".fields) |field| std.debug.assert(field.value <= 255);
    }
    pub fn init(ref: KernelObjectRef, permissions: FdRights, fd_flags: FdFlags) IpcTransferredFd {
        return .{ .object_index = ref.index, .object_generation = ref.generation, .object_kind = @intCast(@intFromEnum(ref.kind)), .permission_bits = @intCast(fdRightsToBits(permissions)), .flag_bits = @intCast(fdFlagsToBits(fd_flags)) };
    }
    pub fn object(self: IpcTransferredFd) KernelObjectRef {
        return .{ .kind = @enumFromInt(self.object_kind), .index = self.object_index, .generation = self.object_generation };
    }
    pub fn rights(self: IpcTransferredFd) FdRights {
        return fdRightsFromBits(self.permission_bits);
    }
    pub fn flags(self: IpcTransferredFd) FdFlags {
        return fdFlagsFromBits(self.flag_bits);
    }
};

pub const IpcMessage = struct {
    call: IpcReplyRef = .{},
    // Only tracked attachments allocate nodes. One list head avoids charging
    // every empty channel for nineteen pointers in each queue message.
    reference_head: ?*references.Node = null,
    active: bool = false,
    sender: PrincipalId = default_process_principal,
    words: [4]u64 = .{ 0, 0, 0, 0 },
    fd_count: u8 = 0,
    // Queue-private packed attachments need only word alignment. Requiring
    // u128 alignment adds a padding word per message and makes a channel's
    // body spill from three allocator pages into four after adding call refs.
    fds: [max_ipc_message_fds]IpcTransferredFd align(8) = [_]IpcTransferredFd{.{}} ** max_ipc_message_fds,
    pub fn referenceAt(self: *const IpcMessage, position: usize) ?*references.Node {
        var cursor = self.reference_head;
        var visited: usize = 0;
        while (cursor) |node| : (cursor = node.message_next) {
            if (visited >= max_ipc_message_fds) return null;
            visited += 1;
            if (node.identity.position == position) return node;
        }
        return null;
    }

    pub fn removeReference(self: *IpcMessage, position: usize) void {
        var link = &self.reference_head;
        while (link.*) |node| {
            if (node.identity.position == position) {
                link.* = node.message_next;
                references.Root.disconnect(node, true);
                references.Pool.cancel(node);
                return;
            }
            link = &node.message_next;
        }
    }

    pub fn shiftReference(self: *IpcMessage, from: usize, to: usize) void {
        if (self.referenceAt(from)) |node| {
            var identity = node.identity;
            identity.position = @intCast(to);
            references.Root.update(node, identity, node.range);
        }
    }
};

// Eleven messages plus the mapped-storage ownership header fit one page.
// A channel shares its middle page between the two queue views; the channel
// table owns each page once; K retains the body and E protects queue access.
pub const IpcMessagePage = [11]IpcMessage;
comptime {
    if ((@import("../memory/owned_storage.zig").requiredPages(IpcMessagePage, 1) catch unreachable) != 1)
        @compileError("IPC message page must never require contiguous physical pages");
}
pub const IpcQueue = struct {
    exclusion: ?*@import("../sync/ipc_endpoint.zig").Lock = null,
    // Queue identity is stable across messages and physical page boundaries.
    // Reconstruct attachment identities at publication so slot wrap/revoke
    // validates the same owner generation, direction and sequence as before.
    reference_identity: struct {
        owner: u32 = 0,
        owner_generation: u32 = 0,
        container: u16 = 0,
        direction: u8 = 0,
    } = .{},
    reference_sequence: u64 = 0,
    pages: [2]?*IpcMessagePage = .{ null, null },
    page_offset: u8 = 0,

    head: u8 = 0,
    len: u8 = 0,

    pub fn messageAt(self: *const IpcQueue, index: usize) *IpcMessage {
        std.debug.assert(index < max_ipc_queue_messages);
        const position = index + self.page_offset;
        return &self.pages[position / 11].?[position % 11];
    }

    pub fn assertHeld(self: *const IpcQueue) void {
        if (self.exclusion) |lock| lock.assertHeld();
    }

    pub fn acquire(self: *const IpcQueue) @import("../sync/ipc_endpoint.zig").Lock.Guard {
        return if (self.exclusion) |lock| lock.acquire() else .{ .lock = null, .restore = false };
    }

    pub fn isEmpty(self: *const IpcQueue) bool {
        return self.len == 0;
    }

    pub fn isFull(self: *const IpcQueue) bool {
        return self.len >= max_ipc_queue_messages;
    }

    pub fn slotIndex(self: *const IpcQueue, offset: usize) usize {
        return (@as(usize, self.head) + offset) % max_ipc_queue_messages;
    }

    // FIFO messages are never removed from the middle (revoke removes only
    // attachments). Thus sequence is derivable without sixteen extra u64s.
    pub fn messageSequence(self: *const IpcQueue, offset: usize) u64 {
        std.debug.assert(offset < self.len);
        return self.reference_sequence - self.len + 1 + offset;
    }

    pub fn peek(self: *const IpcQueue) ?*const IpcMessage {
        if (self.len == 0) return null;
        return self.messageAt(self.head);
    }

    pub fn push(self: *IpcQueue, msg: IpcMessage) KernelError!void {
        self.assertHeld();
        // Queue backpressure is not descriptor/object allocation failure.
        if (self.isFull()) return KernelError.MailboxFull;
        if (self.reference_sequence == std.math.maxInt(u64)) return KernelError.InvalidState;
        const index = self.slotIndex(self.len);
        self.reference_sequence += 1;
        var cursor = msg.reference_head;
        while (cursor) |node| : (cursor = node.message_next) {
            const identity: references.Identity = .{
                .kind = .ipc,
                .owner = self.reference_identity.owner,
                .owner_generation = self.reference_identity.owner_generation,
                .container = self.reference_identity.container,
                .direction = self.reference_identity.direction,
                .index = @intCast(index),
                .sequence = self.reference_sequence,
                .position = node.identity.position,
            };
            node.prepared_target.?.connect(node, identity, .{}, true);
        }
        self.messageAt(index).* = msg;
        self.messageAt(index).active = true;
        self.len += 1;
    }

    pub fn pop(self: *IpcQueue) ?IpcMessage {
        self.assertHeld();
        if (self.len == 0) return null;
        const index = self.head;
        const msg = self.messageAt(index).*;
        var cursor = msg.reference_head;
        while (cursor) |node| : (cursor = node.message_next) references.Root.disconnect(node, true);
        self.messageAt(index).* = .{};
        self.head = @intCast((@as(usize, self.head) + 1) % max_ipc_queue_messages);
        self.len -= 1;
        if (self.len == 0) self.head = 0;
        return msg;
    }
};

pub const IpcEndpointSlot = struct {
    exclusion: @import("../sync/ipc_endpoint.zig").Lock = .{},
    live: @import("../sync/ipc_lifetime.zig").Admission = .{},
    retired_next: ?*IpcEndpointSlot = null,
    active: bool = false,
    generation: u32 = 1,
    queue: IpcQueue = .{},
    waiters: IpcWaitList = .{},
    receiver_hint: IpcReceiverHint = .{},
};

pub const IpcChannelSlot = struct {
    exclusion: @import("../sync/ipc_endpoint.zig").Lock = .{},
    live: @import("../sync/ipc_lifetime.zig").Admission = .{},
    retired_next: ?*IpcChannelSlot = null,
    active: bool = false,
    generation: u32 = 1,
    ref_count: u8 = 0,
    queues: [2]IpcQueue = .{ .{}, .{} },
    waiters: [2]IpcWaitList = .{ .{}, .{} },
    receiver_hints: [2]IpcReceiverHint = .{ .{}, .{} },
    // Per-side notification latch: binary, no payload.  The channel capability
    // already authenticates the sender and the shared ring carries the work,
    // so a doorbell needs no message copy or queue slot.
    notify_pending: [2]bool = .{ false, false },
    notify_waiters: [2]IpcNotifyWaiter = .{ .{}, .{} },
};

comptime {
    // Checked lock records must not push the now-small body back across a
    // physical page boundary. Test the real owner header and table grouping.
    for (.{ IpcEndpointSlot, IpcChannelSlot }) |T| {
        const Body = struct { index: u32, value: T };
        const slots = slot_storage.SlotTable(T).block_slots;
        if (slots < 4 or (@import("../memory/owned_storage.zig").requiredPages(Body, slots) catch unreachable) != 1)
            @compileError("IPC body grouping must fit in individual physical pages");
    }
}

pub const IpcReplySlot = struct {
    exclusion: @import("../sync/ipc_endpoint.zig").Lock = .{},
    // A thread owns one reusable block. Independent reference counts cover
    // owner, receive, outgoing and implicit/extracted authority. No T borrower
    // frees storage; K returns the last reference to the common wait cache.
    block_owner: @import("../ipc_call_binding.zig").Node = .{ .kind = .block_owner },
    receive: @import("../ipc_call_binding.zig").Node = .{ .kind = .receive },
    receive_list: ?*IpcWaitList = null,
    receive_prev: ?*IpcReplySlot = null,
    receive_next: ?*IpcReplySlot = null,
    receive_target: ThreadWakeTarget = .{},

    // SlotTable bodies never move; scheduler bindings own references until
    // their pending cancellation nodes have been drained under K.
    ref: IpcReplyRef = .{},
    lifetime_refs: std.atomic.Value(usize) = .init(0),

    caller: ThreadWakeTarget = .{},
    queue_key: ?IpcWaitKey = null,
    outgoing: @import("../ipc_call_binding.zig").Node = .{ .kind = .outgoing },
    implicit: @import("../ipc_call_binding.zig").Node = .{ .kind = .implicit },
    active: bool = false,
    generation: u32 = 1,
    sent: bool = false,
    waiters: IpcWaitList = .{},
    receiver_hint: IpcReceiverHint = .{},
    // Each increment requires an already owned reference (or private creation).
    // The final release is unique; only its K-held caller reclaims the body.
    pub fn retain(self: *IpcReplySlot, count: usize) void {
        var previous = self.lifetime_refs.load(.monotonic);
        while (true) {
            const next = std.math.add(usize, previous, count) catch @panic("IPC reference overflow");
            if (self.lifetime_refs.cmpxchgWeak(previous, next, .monotonic, .monotonic)) |actual| previous = actual else return;
        }
    }
    pub fn release(self: *IpcReplySlot) bool {
        const previous = self.lifetime_refs.fetchSub(1, .acq_rel);
        std.debug.assert(previous != 0);
        return previous == 1;
    }
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
    call: IpcReplyRef = .{},
    words: [4]u64 = .{ 0, 0, 0, 0 },
    fd_count: usize = 0,
    fds: [max_ipc_message_fds]IpcRecvFd = [_]IpcRecvFd{.{}} ** max_ipc_message_fds,
};

pub const IpcRecvOutcome = struct {
    message: IpcRecvResult = .{},
    writable_wake_count: usize = 0,
};

pub const native_page_size: u64 = 4096;
pub const NativeVmoKind = enum(u8) {
    none = 0,
    anonymous = 1,
    /// A capability-bounded scatter view of an anonymous parent's pages.
    /// Explicit sparse holes are unavailable backing. The view owns metadata
    /// only; parent ownership and external borrowers' page pins are distinct.
    page_view = 2,
    /// Borrowed BAR pages, never allocator-owned RAM. Admission and cache
    /// leases must be established before this backing can acquire VMAs.
    device_memory = 3,
    /// The kernel-image clock page, never allocator-owned RAM. One permanent
    /// kernel reference keeps it alive; it is mapped shared and read-only only,
    /// and is excluded from DMA pinning, cache policy, growth and page views.
    kernel_page = 4,
    // Pages enter this VMO only through an authorized pager supply. In
    // particular an absent entry cannot take the anonymous zero-fill path.
    pager = 5,
};

pub const PagerPortRef = struct {
    index: u32 = 0,
    generation: u32 = 0,
    pub fn isNull(self: PagerPortRef) bool { return self.generation == 0; }
};

pub const PagerRequestRef = struct {
    index: u32 = 0,
    generation: u32 = 0,
    pub fn isNull(self: PagerRequestRef) bool { return self.generation == 0; }
};
pub const PagerWaiterRef = struct {
    index: u32 = 0,
    generation: u32 = 0,
    pub fn isNull(self: PagerWaiterRef) bool { return self.generation == 0; }
};
pub const PagerReleaseRef = struct {
    index: u32 = 0,
    generation: u32 = 0,
    pub fn isNull(self: PagerReleaseRef) bool { return self.generation == 0; }
};

// K and the shared-VM lock can each enter pager state, but every list below
// has one writer domain: KernelState.pager_lock. Never keep a pointer from a
// slot after releasing that lock unless a retained VMO/FD pins its lifetime.
pub const PagerPortSlot = struct {
    generation: u32 = 1,
    live: bool = false,
    owner: PrincipalRaw = 0,
    attach_count: u32 = 0,
    queued_faults: u32 = 0,
    first_fault: PagerRequestRef = .{},
    last_fault: PagerRequestRef = .{},
    first_unpublished: PagerRequestRef = .{},
    last_unpublished: PagerRequestRef = .{},
    first_release: PagerReleaseRef = .{},
    last_release: PagerReleaseRef = .{},
    waiting: PagerWaiterRef = .{},
    first_request: PagerRequestRef = .{},
};
pub const PagerRequestSlot = struct {
    generation: u32 = 1,
    active: bool = false,
    vmo: NativeVmoRef = .{},
    port: PagerPortRef = .{},
    page_index: u32 = 0,
    request_id: u64 = 0,
    wait_started_ns: u64 = 0,
    access: u32 = 0,
    status: enum(u8) { pending, failed } = .pending,
    reason: u32 = 0,
    retained: bool = false,
    published: bool = false,
    queue: enum(u2) { none, fault, unpublished } = .none,
    prev: PagerRequestRef = .{},
    next: PagerRequestRef = .{},
    next_failed: PagerRequestRef = .{},
    next_port: PagerRequestRef = .{},
    prev_port: PagerRequestRef = .{},
    waiters: PagerWaiterRef = .{},
};
pub const PagerWaiterSlot = struct {
    generation: u32 = 1,
    owner: PrincipalId = default_process_principal,
    thread_index: usize = 0,
    thread_generation: u32 = 0,
    token: u64 = 0,
    resume_rax: u64 = @import("kernel_abi_root").pager_abi.wait_wake_retry_status,
    next: PagerWaiterRef = .{},
};
pub const PagerReleaseSlot = struct {
    generation: u32 = 1,
    key: u64 = 0,
    attach_generation: u64 = 0,
    port: PagerPortRef = .{},
    vmo: NativeVmoRef = .{}, // weak: final drop moves this tombstone to queue
    queued: bool = false,
    next: PagerReleaseRef = .{},
};

pub const DeviceMemoryCache = enum(u8) { uc, wc };

pub const DeviceMemoryBacking = struct {
    device: DmaDeviceId,
    bar_index: u8,
    cache: DeviceMemoryCache,
    state: enum(u8) { staged, active, retiring } = .staged,
    pat_slot: u8 = 0,
    wc_lease_token: u64 = 0,
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
    parent_edge: references.Node = .{},
    references: references.Root = .{},
    // Failure-path scratch is prepared with the body, under the backing-store lock.
    census_count: u32 = 0,
    kind: NativeVmoKind = .none,
    generation: u32 = 1,
    size_bytes: u64 = 0,
    page_count: u32 = 0,
    // Opaque kernel-internal owner token for sparse backing-page lookup.
    page_store_start: BackingRef = .{},
    page_store: backing_index.Store = .{},
    page_store_owner: u64 = 0,
    has_page_store: bool = false,
    // A separate sparse index keeps pending/failed pager request identity by
    // page. Its values are encoded request refs, never physical addresses.
    pager_request_store: backing_index.Store = .{},
    pager_request_start: BackingRef = .{},
    zero_on_demand: bool = false,
    pager_port: PagerPortRef = .{},
    pager_key: u64 = 0,
    pager_attach_generation: u64 = 0,
    pager_owner: PrincipalRaw = 0,
    pager_release: PagerReleaseRef = .{},
    pager_failed_head: PagerRequestRef = .{},
    // Borrowed backing may be replaced, but neither a hole nor an attenuated
    // updater FD may enlarge the authority established at publication.
    page_view_sparse: bool = false,
    // AS-local PTE retirement can run without the shared-VM lock. An atomic
    // conservative mark bridges disappearing aliases until all-AS collection.
    // This flag belongs to the view, not its replaceable backing radix root.
    page_view_dirty_all: bool = false,
    page_view_max_prot: VmaProt = .{ .read = true, .write = true },
    // Cached RAM cannot be recycled at final drop under an inner VM lock.
    // The outer retirement path restores WB before returning owned pages.
    ram_cache_state: enum(u8) { wb, cached, retiring } = .wb,
    ref_count: u32 = 0,
    // Non-null only after final release could not return all owned data.
    // The failed destination must outlive the retry. This is NOT proof of
    // the original data allocation's issuer/region (a separate migration).
    pending_data_return: ?*FreePageList = null,
    // Device backing has no anonymous parent; keep their storage disjoint
    // without reinterpreting either arm to hide retirement ownership.
    backing: union {
        parent: struct { ref: NativeVmoRef = .{}, offset: u64 = 0 },
        device: DeviceMemoryBacking,
        kernel_page: struct { paddr: u64 },
    } = .{ .parent = .{} },
};

pub const NativeCowTableSlot = struct {
    references: references.Root = .{},
    active: bool = false,
    generation: u32 = 1,
    ref_count: u32 = 0,
    page_count: u32 = 0,
    // Opaque kernel-internal owner token for sparse backing-page lookup.
    page_store_start: BackingRef = .{},
    page_store: backing_index.Store = .{},
    pending_data_return: ?*FreePageList = null,
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

pub const VmaReferences = struct {
    // Keep the address index in the already reserved reference body. Putting
    // it in VmaEntry halves SlotTable's entries per block (32 -> 16), adding
    // page grants under memory pressure. This placement keeps both densities.
    address_node: VmaAddressNode = .{},
    address_owner: ?*VmaTable = null,
    entry: ?*VmaEntry = null,
    identity: references.Identity = .{},
    vmo: references.Node = .{},
    cow: references.Node = .{},
};

pub const VmaAddressNode = struct {
    // Split transactions can briefly install equal-start fragments before
    // trimming the source. The stable slot breaks ties during that excluded
    // interval; published ranges remain disjoint.
    key: struct { start: u64 = 0, slot: u32 = 0 } = .{},
    parent: ?*VmaEntry = null,
    child: [2]?*VmaEntry = .{ null, null },
    red: bool = false,
    linked: bool = false,
};
pub const VmaAddressAdapter = struct {
    pub const Handle = *VmaEntry;
    pub const Node = VmaAddressNode;
    pub const Key = @FieldType(Node, "key");
    pub const augmented = false;
    pub fn resolve(entry: Handle) *Node {
        return &entry.references.?.address_node;
    }
    pub fn less(a: Key, b: Key) bool {
        return a.start < b.start or (a.start == b.start and a.slot < b.slot);
    }
    pub fn update(_: Handle) void {}
    pub fn validate(entry: Handle) !void {
        try std.testing.expect(entry.active);
        try std.testing.expectEqual(entry.start_va, entry.references.?.address_node.key.start);
    }
};
pub const VmaEntry = struct {
    // Backing kind is immutable for the retained VMO's generation. This is
    // only a discriminator; device PAT and exact backing are checked live.
    device_backing: bool = false,
    references: ?*VmaReferences = null,
    active: bool = false,
    // Position in the owning table's swap-with-last list, never a VMA slot.
    // Fork/split publication replaces the source snapshot's position.
    active_position: u32 = 0,
    start_va: u64 = 0,
    size_bytes: u64 = 0,
    prot: VmaProt = .{},
    // Immutable protection ceiling captured from the mapping authority.
    // mprotect may narrow and restore permissions within this ceiling, but
    // cannot manufacture map_write/map_exec rights after the source FD is
    // gone or transferred.
    max_prot: VmaProt = .{},
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
    pinned_objects: references.Root = .{},
    reference_slots: slot_storage.SlotTable(VmaReferences) = .{},
    reference_owner: u32 = 0,
    reference_generation: u32 = 0,
    // Mark mask 1 is reserved for one munmap's pre-mutation overlap set.
    // The owning VM transaction + K exclude other users; no marks survive it.
    entries: slot_storage.SlotTable(VmaEntry) = .{},
    allocator: ?@import("../memory/owned_storage.zig").Allocator = null,
    active_indices: slot_storage.SlotTable(u32) = .{},
    next_user_map_va: u64 = 0,
    active_count: usize = 0,
    address_index: @import("../memory/ordered_index.zig").Index(VmaAddressAdapter) = .{},

    // The owning VM transaction excludes lookups until every fragment and
    // reference range has been published. Entries never move in SlotTable.
    pub fn reindex(self: *VmaTable, entry: *VmaEntry, slot: u32) void {
        if (entry.references.?.address_node.linked) {
            if (entry.references.?.address_node.key.start == entry.start_va) return;
            self.address_index.remove(entry);
        }
        self.address_index.insert(entry, .{ .start = entry.start_va, .slot = slot });
    }

    pub fn entryForVa(self: *const VmaTable, va: u64) ?*VmaEntry {
        const profile = @import("../lock_profile.zig");
        const start = profile.copyTimestamp();
        defer profile.copyEnd(.vma_lookup, start);
        var cursor = self.address_index.root;
        var predecessor: ?*VmaEntry = null;
        while (cursor) |entry| {
            if (entry.start_va <= va) {
                predecessor = entry;
                cursor = entry.references.?.address_node.child[1];
            } else cursor = entry.references.?.address_node.child[0];
        }
        const entry = predecessor orelse return null;
        return if (va < entry.endVa()) entry else null;
    }

    pub fn overlaps(self: *const VmaTable, start: u64, end: u64) bool {
        var cursor = self.address_index.root;
        while (cursor) |entry| {
            if (entry.start_va >= end) cursor = entry.references.?.address_node.child[0] else if (entry.endVa() <= start) cursor = entry.references.?.address_node.child[1] else return true;
        }
        return false;
    }

    pub const OverlapIterator = struct {
        table: *VmaTable,
        cursor: ?*VmaEntry,
        start: u64,
        end: u64,
        previous_end: ?u64 = null,

        pub fn next(self: *OverlapIterator) ?*VmaEntry {
            const entry = self.cursor orelse return null;
            if (entry.start_va >= self.end) return null;
            std.debug.assert(entry.active and entry.endVa() > self.start);
            if (self.previous_end) |end| std.debug.assert(end <= entry.start_va);
            self.previous_end = entry.endVa();
            self.cursor = self.table.address_index.successor(entry);
            if (self.cursor) |next_entry| std.debug.assert(entry.endVa() <= next_entry.start_va);
            return entry;
        }
    };

    /// O(log N + overlaps), valid only while the VM transaction keeps the
    /// published disjoint ranges stable. Finish the walk before any mutation:
    /// split publication can temporarily contain equal-start fragments.
    pub fn overlapping(self: *VmaTable, start: u64, end: u64) OverlapIterator {
        var first: ?*VmaEntry = null;
        if (start < end) {
            first = self.address_index.lastAtOrBefore(.{ .start = start, .slot = std.math.maxInt(u32) });
            if (first) |entry| {
                if (entry.endVa() <= start) first = self.address_index.successor(entry);
            } else first = self.address_index.first;
        }
        return .{ .table = self, .cursor = first, .start = start, .end = end };
    }
};

pub const NativeVmaFaultMapping = struct {
    paddr: u64,
    prot: MapProt,
    device_pat_slot: ?u3 = null,
    invalidate_start_va: u64 = 0,
    invalidate_size_bytes: u64 = 0,
};

/// How a native VMA page must be resolved before its physical address can be
/// pinned for DMA. Writable private mappings need an owned COW page even for
/// device-read-only DMA: a later CPU write must not move the virtual address
/// away from the page recorded by the DMA object.
pub const NativeVmaDmaPinMode = enum(u8) {
    read_only,
    direct_writable,
    cow_writable,
};

pub const NativeVmaPrepareKind = enum(u8) {
    denied,
    ready,
    allocate_zero,
    allocate_copy,
    locked_slow_path,
};

// A fault plan is consumed while the caller keeps the owning address-space
// lock held.  Shared VMO/COW metadata may change between prepare and commit,
// so commit must still revalidate every reference and backing-page identity.
pub const NativeVmaFaultPlan = struct {
    kind: NativeVmaPrepareKind = .denied,
    mapping: NativeVmaFaultMapping = .{ .paddr = 0, .prot = .{} },
    entry_index: u32 = 0,
    fault_page_va: u64 = 0,
    vmo: NativeVmoRef = .{},
    vmo_page_index: u32 = 0,
    cow_table: NativeCowTableRef = .{},
    cow_page_index: u32 = 0,
    source_paddr: u64 = 0,
};

pub const backing_index = @import("../memory/vmo_backing.zig");
pub const BackingRef = backing_index.Ref;
pub const max_vmo_logical_pages: usize = std.math.maxInt(u32);
pub const BackedPage = backing_index.BackedPage;
// Error evidence alone is global; successful owner operations use only the
// owning store's lock. Never take this lock while holding another store lock.
var vmo_backing_page_store_lock: backing_index.Lock = .{};
pub const VmoBackingStoreOwnershipFault = struct {
    count: u64 = 0,
    operation: u8 = 0, // 1 allocate, 2 free, 3 write, 4 final-owner validation
    start: u64 = 0,
    len: u32 = 0,
    index: u32 = 0,
    expected: u64 = 0,
    actual: u64 = 0,
};

var vmo_backing_store_ownership_fault: VmoBackingStoreOwnershipFault = .{};

pub const VmoBackingStoreOwnerKind = enum(u1) {
    native_vmo = 0,
    native_cow = 1,
};

pub fn vmoBackingStoreOwner(kind: VmoBackingStoreOwnerKind, index: u32, generation: u32) u64 {
    if (index > std.math.maxInt(u31) or generation == 0) return 0;
    const kind_bit = @as(u64, @intFromEnum(kind)) << 63;
    const generation_bits = @as(u64, generation) << 31;
    return kind_bit | generation_bits | index;
}

fn recordVmoBackingStoreOwnershipFaultLocked(
    operation: u8,
    start: u64,
    len: u32,
    index: u32,
    expected: u64,
    actual: u64,
) void {
    vmo_backing_store_ownership_fault.count +%= 1;
    if (vmo_backing_store_ownership_fault.count != 1) return;
    vmo_backing_store_ownership_fault.operation = operation;
    vmo_backing_store_ownership_fault.start = start;
    vmo_backing_store_ownership_fault.len = len;
    vmo_backing_store_ownership_fault.index = index;
    vmo_backing_store_ownership_fault.expected = expected;
    vmo_backing_store_ownership_fault.actual = actual;
}

pub fn vmoBackingStoreOwnershipFault() VmoBackingStoreOwnershipFault {
    vmo_backing_page_store_lock.lock();
    defer vmo_backing_page_store_lock.unlock();
    return vmo_backing_store_ownership_fault;
}

fn ownershipFault(operation: u8, start: BackingRef, count: u32, index: usize, expected: u64) void {
    vmo_backing_page_store_lock.lock();
    defer vmo_backing_page_store_lock.unlock();
    recordVmoBackingStoreOwnershipFaultLocked(operation, start.owner, count, @intCast(@min(index, std.math.maxInt(u32))), expected, start.owner);
}
pub fn vmoBackingPageStoreOwnerMatches(start: BackingRef, page_count: u32, recorded: u64, expected: u64) bool {
    if (start.store) |store| {
        store.lock.lock();
        const valid = recorded == expected and store.matches(start, expected);
        store.lock.unlock();
        if (valid) return true;
    }
    ownershipFault(4, start, page_count, 0, expected);
    return false;
}
pub fn checkedVmoPageCount(size: u64) KernelError!u32 {
    if (size == 0) return KernelError.InvalidState;
    const rounded = std.math.add(u64, size, 4095) catch return KernelError.InvalidState;
    const count = rounded / 4096;
    if (count > max_vmo_logical_pages) return KernelError.InvalidState;
    return @intCast(count);
}
pub fn allocEmptyVmoBackingPageStore(store: *backing_index.Store, page_count: usize, owner: u64) ?BackingRef {
    if (page_count == 0 or page_count > max_vmo_logical_pages or owner == 0) return null;
    return store.init(owner);
}
pub fn growVmoBackingPageStore(start: BackingRef, old_page_count: u32, new_page_count: u32, owner: u64) ?BackingRef {
    if (old_page_count == 0 or new_page_count <= old_page_count or !vmoBackingPageStoreOwnerMatches(start, old_page_count, owner, owner)) return null;
    return start;
}
pub fn vmoBackingPageStorePaddr(start: BackingRef, page_count: u32, page_index: usize) ?u64 {
    const store = start.store orelse return null;
    store.lock.lock();
    defer store.lock.unlock();
    if (!store.matches(start, start.owner) or page_index >= page_count) return null;
    return store.getLocked(@intCast(page_index));
}
pub fn backingAllocator(free_list: ?*FreePageList) KernelError!owned_storage.Allocator {
    if (builtin.is_test) return backing_index.test_allocator orelse slot_storage.hostTestAllocator();
    if (free_list) |fl| return @import("../memory/identity_allocator.zig").allocatorFor(fl, .vmo_backing);
    return KernelError.OutOfFreePages;
}
pub fn setVmoBackingPageStorePaddr(start: BackingRef, page_count: u32, page_index: usize, owner: u64, paddr: u64, free_list: ?*FreePageList) KernelError!void {
    const store = start.store orelse {
        ownershipFault(3, start, page_count, page_index, owner);
        return KernelError.InvalidState;
    };
    if (page_index >= page_count or paddr & 4095 != 0) {
        ownershipFault(3, start, page_count, page_index, owner);
        return KernelError.InvalidState;
    }
    while (true) {
        store.lock.lock();
        if (!store.matches(start, owner)) {
            store.lock.unlock();
            ownershipFault(3, start, page_count, page_index, owner);
            return KernelError.InvalidState;
        }
        if (store.hasPathLocked(@intCast(page_index))) {
            store.putLocked(@intCast(page_index), paddr);
            store.lock.unlock();
            if (paddr == 0) store.pruneEmpty(start, @intCast(page_index));
            return;
        }
        if (paddr == 0) {
            store.lock.unlock();
            return;
        }
        const allocator = backingAllocator(free_list) catch |err| {
            store.lock.unlock();
            return err;
        };
        store.ensureLocked(start, @intCast(page_index), allocator) catch |err| {
            store.lock.unlock();
            return switch (err) {
                error.OutOfMemory => KernelError.OutOfFreePages,
                error.InvalidState => KernelError.InvalidState,
            };
        };
        if (!store.matches(start, owner)) {
            store.lock.unlock();
            return KernelError.InvalidState;
        }
        store.putLocked(@intCast(page_index), paddr);
        store.lock.unlock();
        return;
    }
}
pub fn setVmoBackingPageStorePaddrs(start: BackingRef, page_count: u32, page_offset: usize, owner: u64, paddrs: []const u64, free_list: ?*FreePageList) KernelError!void {
    return setVmoBackingPageStoreSequence(start, page_count, page_offset, owner, paddrs.len, paddrs, struct {
        fn at(pages: []const u64, index: usize) u64 {
            return pages[index];
        }
    }.at, free_list);
}
pub fn setVmoBackingPageStoreContiguous(start: BackingRef, page_count: u32, owner: u64, base: u64, count: usize, free_list: *FreePageList) KernelError!void {
    const bytes = std.math.mul(u64, count, 4096) catch return KernelError.InvalidState;
    _ = std.math.add(u64, base, bytes) catch return KernelError.InvalidState;
    if (base == 0 or base & 4095 != 0) return KernelError.InvalidState;
    return setVmoBackingPageStoreSequence(start, page_count, 0, owner, count, base, struct {
        fn at(first: u64, index: usize) u64 {
            return first + @as(u64, @intCast(index)) * 4096;
        }
    }.at, free_list);
}
fn setVmoBackingPageStoreSequence(start: BackingRef, page_count: u32, page_offset: usize, owner: u64, count: usize, context: anytype, comptime pageAt: anytype, free_list: ?*FreePageList) KernelError!void {
    const store = start.store orelse return KernelError.InvalidState;
    if (count == 0 or page_offset > page_count or count > page_count - page_offset) return KernelError.InvalidState;
    for (0..count) |i| if (pageAt(context, i) & 4095 != 0) return KernelError.InvalidState;
    store.lock.lock();
    var held = true;
    defer if (held) store.lock.unlock();
    // Keep the single-lock warm/fault path. ensureLocked only drops it when
    // physical metadata is actually needed; every key is revalidated afterward.
    errdefer {
        store.lock.unlock();
        held = false;
        var i: usize = 0;
        while (i < count) {
            const index: u32 = @intCast(page_offset + i);
            store.pruneEmpty(start, index);
            i += @min(count - i, 256 - (index & 255));
        }
    }
    while (true) {
        if (!store.matches(start, owner)) return KernelError.InvalidState;
        for (0..count) |i| {
            const index: u32 = @intCast(page_offset + i);
            if (store.getLocked(index) != 0) return KernelError.InvalidState;
            if (pageAt(context, i) != 0 and !store.hasPathLocked(index)) {
                store.ensureLocked(start, index, try backingAllocator(free_list)) catch |err| return switch (err) {
                    error.OutOfMemory => KernelError.OutOfFreePages,
                    error.InvalidState => KernelError.InvalidState,
                };
            }
        }
        if (!store.matches(start, owner)) return KernelError.InvalidState;
        var missing = false;
        for (0..count) |i| {
            const index: u32 = @intCast(page_offset + i);
            if (store.getLocked(index) != 0) return KernelError.InvalidState;
            if (pageAt(context, i) != 0 and !store.hasPathLocked(index)) {
                missing = true;
                break;
            }
        }
        if (missing) continue;
        for (0..count) |i| {
            const pa = pageAt(context, i);
            if (pa != 0) store.putLocked(@intCast(page_offset + i), pa);
        }
        return;
    }
}

pub fn freeVmoBackingPageStore(start: BackingRef, page_count: u32, owner: u64) bool {
    if (!vmoBackingPageStoreOwnerMatches(start, page_count, owner, owner)) return false;
    return start.store.?.clear(start);
}
pub fn snapshotVmoBackingPages(start: BackingRef, page_count: u32, owner: u64, first: usize, end: usize, out: []BackedPage) usize {
    return snapshotVmoBackingPagesChecked(start, page_count, owner, first, end, out) catch 0;
}
pub fn snapshotVmoBackingPagesChecked(start: BackingRef, page_count: u32, owner: u64, first: usize, end: usize, out: []BackedPage) KernelError!usize {
    const store = start.store orelse {
        ownershipFault(4, start, page_count, first, owner);
        return KernelError.InvalidState;
    };
    store.lock.lock();
    if (!store.matches(start, owner) or first >= end or end > page_count or out.len == 0) {
        store.lock.unlock();
        ownershipFault(4, start, page_count, first, owner);
        return KernelError.InvalidState;
    }
    defer store.lock.unlock();
    return store.snapshotLocked(first, end, out);
}
pub fn reclaimVmoBackingStorage() void {
    backing_index.reclaim();
}
pub fn resetVmoBackingPageStore() void {
    // Fresh boot/test initialization only; per-owner roots are destroyed by
    // their owners, never silently erased through a global reset.
    if (builtin.is_test) backing_index.resetForTest();
    vmo_backing_store_ownership_fault = .{};
}
pub fn runtimeStorageBytes() usize {
    return 0;
}
pub fn initRuntimeStorage(storage: []align(4096) u8) bool {
    _ = storage;
    resetVmoBackingPageStore();
    return true;
}
pub fn kernelStaticStorageEndAddr() usize {
    return @max(@intFromPtr(&vmo_backing_store_ownership_fault) + @sizeOf(VmoBackingStoreOwnershipFault), @intFromPtr(&vmo_backing_page_store_lock) + @sizeOf(backing_index.Lock));
}
pub const VmObjectBackingStoreStats = struct { capacity: u64, used_entries: u64, free_entries: u64, growth_count: u64, retired_pages: usize };
pub fn vmObjectBackingStoreStats() VmObjectBackingStoreStats {
    const s = backing_index.stats();
    return .{ .capacity = s.used, .used_entries = s.used, .free_entries = 0, .growth_count = s.allocations, .retired_pages = s.pending };
}
pub const RegionFreeRange = @import("../memory/free_page_list.zig").RegionFreeRange;
pub const FreePageList = @import("../memory/free_page_list.zig").WithQuarantine(@import("../iommu.zig").physicalRangeQuarantined);

pub const PageCapability = struct {
    paddr: u64,
};

pub var empty_process_descriptors_extra: [0]ProcessDescriptor = .{};
pub var empty_vma_tables_extra: [0]VmaTable = .{};

pub const VmObjectBackingStoreBreakdown = struct {
    native_entries: u64 = 0,
    cow_entries: u64 = 0,
    invalid_native_entries: u64 = 0,
    demand_zero_entries: u64 = 0,
    live_vmos: u64 = 0,
    live_demand_zero_vmos: u64 = 0,
    anonymous_logical_pages: u64 = 0,
    anonymous_entries: u64 = 0,
    page_view_entries: u64 = 0,
    largest: [8]VmObjectBackingOwner = [_]VmObjectBackingOwner{.{}} ** 8,
};

pub const VmObjectBackingOwner = struct {
    owner: u64 = 0,
    entries: u32 = 0,
    pages: u32 = 0,
    refs: u32 = 0,
    kind: NativeVmoKind = .none,
    parent: NativeVmoRef = .{},
};

pub fn vmObjectBackingStoreBreakdown(vmos: *const slot_storage.SlotTable(NativeVmoSlot), leaf: *slot_storage.Leaf) VmObjectBackingStoreBreakdown {
    var result: VmObjectBackingStoreBreakdown = .{};
    var iterator = vmos.iteratorLocked(leaf);
    while (iterator.next()) |item| {
        const vmo = item.value;
        if (vmo.kind == .none) continue;
        vmo.page_store.lock.lock();
        const count: u32 = @intCast(vmo.page_store.used);
        const owner = vmo.page_store.owner;
        vmo.page_store.lock.unlock();
        result.native_entries += count;
        if (vmo.ref_count != 0) result.live_vmos += 1;
        if (vmo.kind == .anonymous) {
            result.anonymous_logical_pages += vmo.page_count;
            result.anonymous_entries += count;
        }
        if (vmo.kind == .page_view) result.page_view_entries += count;
        if (count != 0 and ((vmo.ref_count == 0 and vmo.pending_data_return == null) or owner != vmo.page_store_owner or owner != vmoBackingStoreOwner(.native_vmo, item.ref.index, item.ref.generation))) result.invalid_native_entries += count;
        if (vmo.zero_on_demand) {
            result.demand_zero_entries += count;
            if (vmo.ref_count != 0) result.live_demand_zero_vmos += 1;
        }
        for (&result.largest, 0..) |*place, rank| {
            if (count <= place.entries) continue;
            var move = result.largest.len - 1;
            while (move > rank) : (move -= 1) result.largest[move] = result.largest[move - 1];
            place.* = .{ .owner = owner, .entries = count, .pages = vmo.page_count, .refs = vmo.ref_count, .kind = vmo.kind, .parent = if (vmo.kind == .page_view) vmo.backing.parent.ref else .{} };
            break;
        }
    }
    const total = backing_index.stats().used;
    result.cow_entries = total -| result.native_entries;
    return result;
}

test "IPC endpoint atomic retained authority survives concurrent drain releases" {
    var block: IpcReplySlot = .{};
    block.lifetime_refs.store(1, .release); // owner excludes the final reclaim
    const Worker = struct {
        fn run(body: *IpcReplySlot) void {
            for (0..10000) |_| {
                body.retain(1);
                std.debug.assert(!body.release());
            }
        }
    };
    const thread = try std.Thread.spawn(.{}, Worker.run, .{&block});
    Worker.run(&block);
    thread.join();
    try std.testing.expectEqual(@as(usize, 1), block.lifetime_refs.load(.acquire));
    try std.testing.expect(block.release());
}
