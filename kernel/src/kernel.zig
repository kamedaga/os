const std = @import("std");
const builtin = @import("builtin");
pub const capability = @import("capability.zig");
pub const capsule = @import("capsule.zig");
const dma_mapping_manager = @import("dma_mapping_manager.zig");
pub const device_capabilities = @import("device_capabilities.zig");
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
pub const vm_object_token_tag: u64 = 1 << 62;

pub fn encodeVmObjectToken(cap_id: u64) u64 {
    std.debug.assert(cap_id != 0);
    std.debug.assert((cap_id & vm_object_token_tag) == 0);
    return vm_object_token_tag | cap_id;
}

pub fn decodeVmObjectToken(token: u64) ?u64 {
    if ((token & vm_object_token_tag) == 0) return null;
    const cap_id = token & ~vm_object_token_tag;
    if (cap_id == 0) return null;
    return cap_id;
}

pub const PrincipalRaw = u32;

fn PrincipalIdType() type {
    const fields = [_]std.builtin.Type.EnumField{
        .{
            .name = "Device0",
            .value = device_principal_raw,
        },
    };

    return @Type(.{ .@"enum" = .{
        .tag_type = PrincipalRaw,
        .fields = &fields,
        .decls = &.{},
        .is_exhaustive = false,
    } });
}

pub const PrincipalId = PrincipalIdType();
const default_process_principal: PrincipalId = processPrincipalFromIndex(0) orelse unreachable;
pub export var endpoint_generation_fast_mirror: u64 = 0;

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

pub const IpcBufferRights = packed struct(u32) {
    read: bool = false,
    write: bool = false,
    map: bool = false,
    grant: bool = false,
    _reserved: u28 = 0,
};

pub const IpcBufferRole = enum(u8) {
    generic = 0,
    request = 1,
    response = 2,
    bulk = 3,
};

pub const IpcBufferCapability = struct {
    paddr: u64,
    rights: IpcBufferRights,
    role: IpcBufferRole,
    cap_id: u64,
    root_cap_id: u64,
    parent_cap_id: u64,
};

pub const PendingIpcBufferTransfer = struct {
    transfer_id: u64,
    sender: PrincipalId,
    endpoint_id: u64,
    cap_id: u64,
    rights: IpcBufferRights,
    retain_sender: bool = true,
};

pub const Region = struct {
    id: u64,
};

pub const ProcessDescriptor = struct {
    active: bool = false,
    principal: PrincipalId = @enumFromInt(0),
    label: []const u8 = "",
    bootstrap_owner: bool = false,
    process_builder_owner: ?PrincipalId = null,
    process_builder_suspended: bool = false,
    abi_trap_delegate_endpoint_id: u64 = 0,
    abi_trap_delegate_flavor: u32 = 0,
    abi_trap_request_page_va: u64 = 0,
    faulted: bool = false,
    fault_vector: u8 = 0,
};

pub const AbiTrapDelegate = struct {
    endpoint_id: u64,
    flavor: u32,
    request_page_va: u64,
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
    VmObjectCapabilityNotFound,
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
    CapsuleRevoked,
};

pub const Fd = u32;
pub const fd_table_entries: usize = 256;
pub const max_fd_objects: usize = 4096;
pub const fd_known_flags_mask: u32 = (@as(u32, 1) << 4) - 1;
pub const fd_known_rights_mask: u64 = (@as(u64, 1) << 42) - 1;

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
    _reserved: u22 = 0,
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
    endpoint_compat = 2,
    vmo_compat = 3,
    capsule_compat = 4,
    event = 5,
    vmo = 6,
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
    process: PrincipalId,
    endpoint_compat: u64,
    vmo_compat: u64,
    capsule_compat: u64,
    event: u64,
    vmo: NativeVmoRef,
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

    pub fn isEmpty(self: *const FdEntry) bool {
        return self.object.isNull();
    }
};

pub const FdTable = struct {
    entries: [fd_table_entries]FdEntry = [_]FdEntry{.{}} ** fd_table_entries,
};

pub const FdTransferMode = enum(u8) {
    copy,
    move,
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
    _reserved: u5 = 0,
};

pub const MmapFlags = packed struct(u32) {
    fixed: bool = false,
    fixed_noreplace: bool = false,
    private: bool = false,
    shared: bool = false,
    anonymous: bool = false,
    noreserve: bool = false,
    _reserved: u26 = 0,
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
    while (i < vm_object_backing_page_store_free_range_len) : (i += 1) {
        pages += vm_object_backing_page_store_free_ranges[i].len;
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

pub const IpcBufferCNode = struct {
    pub const max_caps = 512;

    caps: [max_caps]IpcBufferCapability = undefined,
    len: usize = 0,

    pub fn add(self: *IpcBufferCNode, cap: IpcBufferCapability) KernelError!void {
        if (self.findByCapId(cap.cap_id) != null) return KernelError.InvalidState;
        if (self.len >= self.caps.len) return KernelError.TableFull;
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn findByCapId(self: *const IpcBufferCNode, cap_id: u64) ?*const IpcBufferCapability {
        if (self.findIndexByCapId(cap_id)) |index| return &self.caps[index];
        return null;
    }

    pub fn removeByCapId(self: *IpcBufferCNode, cap_id: u64) bool {
        if (self.findIndexByCapId(cap_id)) |index| {
            var i = index;
            while (i + 1 < self.len) : (i += 1) self.caps[i] = self.caps[i + 1];
            self.len -= 1;
            return true;
        }
        return false;
    }

    fn findIndexByCapId(self: *const IpcBufferCNode, cap_id: u64) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            if (self.caps[i].cap_id == cap_id) return i;
        }
        return null;
    }
};

pub const IpcBufferMailbox = struct {
    const inline_items = 8;
    const chunk_items = 16;
    const chunk_pool_count = 512;
    const invalid_chunk: u16 = std.math.maxInt(u16);

    items: [inline_items]PendingIpcBufferTransfer = undefined,
    overflow_head: u16 = invalid_chunk,
    len: usize = 0,

    pub fn push(self: *IpcBufferMailbox, transfer: PendingIpcBufferTransfer) KernelError!void {
        const slot = try self.slotAtOrAllocate(self.len);
        slot.* = transfer;
        self.len += 1;
    }

    pub fn pop(self: *IpcBufferMailbox) ?PendingIpcBufferTransfer {
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

    pub fn reset(self: *IpcBufferMailbox) void {
        freeIpcBufferMailboxChunks(self.overflow_head);
        self.* = .{};
    }

    fn slotAt(self: *IpcBufferMailbox, index: usize) ?*PendingIpcBufferTransfer {
        if (index < inline_items) return &self.items[index];
        var remaining = index - inline_items;
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            if (remaining < chunk_items) return &ipc_buffer_mailbox_chunk_pool[chunk_index].items[remaining];
            remaining -= chunk_items;
            chunk_index = ipc_buffer_mailbox_chunk_pool[chunk_index].next;
        }
        return null;
    }

    fn slotAtConst(self: *const IpcBufferMailbox, index: usize) ?*const PendingIpcBufferTransfer {
        if (index < inline_items) return &self.items[index];
        var remaining = index - inline_items;
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            if (remaining < chunk_items) return &ipc_buffer_mailbox_chunk_pool[chunk_index].items[remaining];
            remaining -= chunk_items;
            chunk_index = ipc_buffer_mailbox_chunk_pool[chunk_index].next;
        }
        return null;
    }

    fn slotAtOrAllocate(self: *IpcBufferMailbox, index: usize) KernelError!*PendingIpcBufferTransfer {
        if (index < inline_items) return &self.items[index];
        const needed_chunk_offset = (index - inline_items) / chunk_items;
        if (needed_chunk_offset >= chunk_pool_count) return KernelError.TableFull;
        if (self.overflow_head == invalid_chunk) self.overflow_head = try allocIpcBufferMailboxChunk();

        var chunk_index = self.overflow_head;
        var offset: usize = 0;
        while (offset < needed_chunk_offset) : (offset += 1) {
            if (ipc_buffer_mailbox_chunk_pool[chunk_index].next == invalid_chunk) {
                ipc_buffer_mailbox_chunk_pool[chunk_index].next = try allocIpcBufferMailboxChunk();
            }
            chunk_index = ipc_buffer_mailbox_chunk_pool[chunk_index].next;
        }
        return &ipc_buffer_mailbox_chunk_pool[chunk_index].items[(index - inline_items) % chunk_items];
    }

    fn trimUnusedChunks(self: *IpcBufferMailbox) void {
        if (self.overflow_head == invalid_chunk) return;
        if (self.len <= inline_items) {
            freeIpcBufferMailboxChunks(self.overflow_head);
            self.overflow_head = invalid_chunk;
            return;
        }
        const needed_chunks = (self.len - inline_items + chunk_items - 1) / chunk_items;
        var chunk_index = self.overflow_head;
        var kept: usize = 1;
        while (kept < needed_chunks and chunk_index != invalid_chunk) : (kept += 1) {
            chunk_index = ipc_buffer_mailbox_chunk_pool[chunk_index].next;
        }
        if (chunk_index == invalid_chunk) return;
        const free_head = ipc_buffer_mailbox_chunk_pool[chunk_index].next;
        ipc_buffer_mailbox_chunk_pool[chunk_index].next = invalid_chunk;
        freeIpcBufferMailboxChunks(free_head);
    }
};

const IpcBufferMailboxChunk = struct {
    used: bool = false,
    next: u16 = IpcBufferMailbox.invalid_chunk,
    items: [IpcBufferMailbox.chunk_items]PendingIpcBufferTransfer = undefined,
};

var ipc_buffer_mailbox_chunk_pool: [IpcBufferMailbox.chunk_pool_count]IpcBufferMailboxChunk = [_]IpcBufferMailboxChunk{.{}} ** IpcBufferMailbox.chunk_pool_count;

fn allocIpcBufferMailboxChunk() KernelError!u16 {
    var i: usize = 0;
    while (i < ipc_buffer_mailbox_chunk_pool.len) : (i += 1) {
        if (ipc_buffer_mailbox_chunk_pool[i].used) continue;
        ipc_buffer_mailbox_chunk_pool[i] = .{ .used = true };
        return @intCast(i);
    }
    return KernelError.TableFull;
}

fn freeIpcBufferMailboxChunks(head: u16) void {
    var chunk_index = head;
    while (chunk_index != IpcBufferMailbox.invalid_chunk) {
        const next = ipc_buffer_mailbox_chunk_pool[chunk_index].next;
        ipc_buffer_mailbox_chunk_pool[chunk_index] = .{};
        chunk_index = next;
    }
}

pub const max_vm_object_backing_pages: usize = 65535;
pub const max_vm_object_backing_store_pages: usize = 262144;
pub const max_vm_object_backing_store_free_ranges: usize = 1024;

var empty_vm_object_backing_page_store: [0]u64 = .{};
var vm_object_backing_page_store: []u64 = empty_vm_object_backing_page_store[0..];
var vm_object_backing_page_store_next: usize = 0;

const VmObjectBackingStoreFreeRange = struct {
    start: u32 = 0,
    len: u32 = 0,
};

var empty_vm_object_backing_page_store_free_ranges: [0]VmObjectBackingStoreFreeRange = .{};
var vm_object_backing_page_store_free_ranges: []VmObjectBackingStoreFreeRange = empty_vm_object_backing_page_store_free_ranges[0..];
var vm_object_backing_page_store_free_range_len: usize = 0;

fn removeVmObjectBackingFreeRange(index: usize) void {
    var i = index + 1;
    while (i < vm_object_backing_page_store_free_range_len) : (i += 1) {
        vm_object_backing_page_store_free_ranges[i - 1] = vm_object_backing_page_store_free_ranges[i];
    }
    vm_object_backing_page_store_free_range_len -= 1;
}

fn insertVmObjectBackingFreeRange(start: u32, len: u32) bool {
    if (len == 0) return true;
    var merged_start = start;
    var merged_len = len;
    var i: usize = 0;
    while (i < vm_object_backing_page_store_free_range_len) {
        const range = vm_object_backing_page_store_free_ranges[i];
        const range_end = range.start + range.len;
        const merged_end = merged_start + merged_len;
        if (range_end < merged_start or merged_end < range.start) {
            i += 1;
            continue;
        }
        if (range.start < merged_start) merged_start = range.start;
        const new_end = if (range_end > merged_end) range_end else merged_end;
        merged_len = new_end - merged_start;
        removeVmObjectBackingFreeRange(i);
    }
    if (vm_object_backing_page_store_free_range_len >= vm_object_backing_page_store_free_ranges.len) return false;
    vm_object_backing_page_store_free_ranges[vm_object_backing_page_store_free_range_len] = .{
        .start = merged_start,
        .len = merged_len,
    };
    vm_object_backing_page_store_free_range_len += 1;
    return true;
}

fn allocVmObjectBackingPageStore(page_paddrs: []const u64) ?u32 {
    if (page_paddrs.len == 0 or page_paddrs.len > max_vm_object_backing_pages) return null;
    var start: usize = 0;
    var free_index: ?usize = null;
    var i: usize = 0;
    while (i < vm_object_backing_page_store_free_range_len) : (i += 1) {
        if (vm_object_backing_page_store_free_ranges[i].len < page_paddrs.len) continue;
        start = vm_object_backing_page_store_free_ranges[i].start;
        free_index = i;
        break;
    }
    if (free_index) |index| {
        const consumed: u32 = @intCast(page_paddrs.len);
        vm_object_backing_page_store_free_ranges[index].start += consumed;
        vm_object_backing_page_store_free_ranges[index].len -= consumed;
        if (vm_object_backing_page_store_free_ranges[index].len == 0) removeVmObjectBackingFreeRange(index);
    } else {
        if (vm_object_backing_page_store_next + page_paddrs.len > vm_object_backing_page_store.len) return null;
        start = vm_object_backing_page_store_next;
        vm_object_backing_page_store_next += page_paddrs.len;
    }
    for (page_paddrs, 0..) |paddr, page_index| {
        if ((paddr & 0xFFF) != 0) return null;
        vm_object_backing_page_store[start + page_index] = paddr;
    }
    return @intCast(start);
}

fn allocEmptyVmObjectBackingPageStore(page_count: usize) ?u32 {
    if (page_count == 0 or page_count > max_vm_object_backing_pages) return null;
    var start: usize = 0;
    var free_index: ?usize = null;
    var i: usize = 0;
    while (i < vm_object_backing_page_store_free_range_len) : (i += 1) {
        if (vm_object_backing_page_store_free_ranges[i].len < page_count) continue;
        start = vm_object_backing_page_store_free_ranges[i].start;
        free_index = i;
        break;
    }
    if (free_index) |index| {
        const consumed: u32 = @intCast(page_count);
        vm_object_backing_page_store_free_ranges[index].start += consumed;
        vm_object_backing_page_store_free_ranges[index].len -= consumed;
        if (vm_object_backing_page_store_free_ranges[index].len == 0) removeVmObjectBackingFreeRange(index);
    } else {
        if (vm_object_backing_page_store_next + page_count > vm_object_backing_page_store.len) return null;
        start = vm_object_backing_page_store_next;
        vm_object_backing_page_store_next += page_count;
    }
    @memset(vm_object_backing_page_store[start .. start + page_count], 0);
    return @intCast(start);
}

fn vmObjectBackingPageStorePaddr(start: u32, page_count: u16, page_index: usize) ?u64 {
    if (page_index >= page_count) return null;
    const store_index = @as(usize, start) + page_index;
    if (store_index >= vm_object_backing_page_store.len) return null;
    const paddr = vm_object_backing_page_store[store_index];
    if ((paddr & 0xFFF) != 0) return null;
    return paddr;
}

fn setVmObjectBackingPageStorePaddr(start: u32, page_count: u16, page_index: usize, paddr: u64) bool {
    if ((paddr & 0xFFF) != 0) return false;
    if (page_index >= page_count) return false;
    const store_index = @as(usize, start) + page_index;
    if (store_index >= vm_object_backing_page_store.len) return false;
    vm_object_backing_page_store[store_index] = paddr;
    return true;
}

fn freeVmObjectBackingPageStore(start: u32, page_count: u16) bool {
    if (page_count == 0) return true;
    const start_usize: usize = @intCast(start);
    const count_usize: usize = @intCast(page_count);
    if (start_usize + count_usize > vm_object_backing_page_store.len) return false;
    @memset(vm_object_backing_page_store[start_usize .. start_usize + count_usize], 0);
    return insertVmObjectBackingFreeRange(start, page_count);
}

fn resetVmObjectBackingPageStore() void {
    @memset(vm_object_backing_page_store[0..], 0);
    @memset(vm_object_backing_page_store_free_ranges[0..], .{});
    vm_object_backing_page_store_next = 0;
    vm_object_backing_page_store_free_range_len = 0;
}

pub const VmObjectRights = packed struct(u32) {
    read: bool = false,
    write: bool = false,
    map: bool = false,
    grant: bool = false,
    _reserved: u28 = 0,
};

pub fn vmObjectRightsFromBits(bits: u64) VmObjectRights {
    return @bitCast(@as(u32, @truncate(bits)));
}

pub const VmObjectBacking = struct {
    page_offset_bytes: u16 = 0,
    page_count: u16 = 0,
    page_store_start: u32 = 0,
    size_bytes: u64 = 0,

    fn init(page_paddrs: []const u64, page_offset_bytes: u16, size_bytes: u64) ?VmObjectBacking {
        if (page_paddrs.len == 0 or page_paddrs.len > max_vm_object_backing_pages) return null;
        if (size_bytes == 0) return null;
        const first_page_bytes = @as(u64, 4096 - page_offset_bytes);
        const total_capacity = first_page_bytes + (@as(u64, @intCast(page_paddrs.len - 1)) * 4096);
        if (total_capacity < size_bytes) return null;

        const page_store_start = allocVmObjectBackingPageStore(page_paddrs) orelse return null;
        return VmObjectBacking{
            .page_offset_bytes = page_offset_bytes,
            .page_count = @intCast(page_paddrs.len),
            .page_store_start = page_store_start,
            .size_bytes = size_bytes,
        };
    }

    pub fn pagePaddr(self: *const VmObjectBacking, page_index: usize) ?u64 {
        if (page_index >= self.page_count) return null;
        const store_index = @as(usize, self.page_store_start) + page_index;
        if (store_index >= vm_object_backing_page_store.len) return null;
        const paddr = vm_object_backing_page_store[store_index];
        if ((paddr & 0xFFF) != 0) return null;
        return paddr;
    }
};

pub const VmObjectCapability = struct {
    backing: VmObjectBacking,
    rights: VmObjectRights,
    cap_id: u64,
    root_cap_id: u64,
    parent_cap_id: u64,
};

pub const VmObjectCNode = struct {
    pub const inline_caps = 64;
    pub const chunk_caps = 64;
    pub const chunk_pool_count = 256;
    pub const max_caps = inline_caps + chunk_pool_count * chunk_caps;
    const invalid_chunk: u16 = std.math.maxInt(u16);

    caps: [inline_caps]VmObjectCapability = undefined,
    overflow_head: u16 = invalid_chunk,
    len: usize = 0,

    pub fn add(self: *VmObjectCNode, cap: VmObjectCapability) KernelError!void {
        if (self.findByCapId(cap.cap_id) != null) return KernelError.InvalidState;
        const slot = try self.slotAtOrAllocate(self.len);
        slot.* = cap;
        self.len += 1;
    }

    pub fn reset(self: *VmObjectCNode) void {
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            const next = vm_object_cap_chunk_pool[chunk_index].next;
            vm_object_cap_chunk_pool[chunk_index] = .{};
            chunk_index = next;
        }
        self.* = .{};
    }

    pub fn findByCapId(self: *const VmObjectCNode, cap_id: u64) ?*const VmObjectCapability {
        if (self.findIndexByCapId(cap_id)) |index| return self.slotAtConst(index);
        return null;
    }

    pub fn removeByCapId(self: *VmObjectCNode, cap_id: u64) ?VmObjectCapability {
        const index = self.findIndexByCapId(cap_id) orelse return null;
        const removed = (self.slotAt(index) orelse return null).*;
        const last_index = self.len - 1;
        if (index != last_index) {
            (self.slotAt(index) orelse return null).* = (self.slotAt(last_index) orelse return null).*;
        }
        self.len -= 1;
        self.trimUnusedChunks();
        return removed;
    }

    fn findIndexByCapId(self: *const VmObjectCNode, cap_id: u64) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            const cap = self.slotAtConst(i) orelse return null;
            if (cap.cap_id == cap_id) return i;
        }
        return null;
    }

    fn slotAtConst(self: *const VmObjectCNode, index: usize) ?*const VmObjectCapability {
        if (index < inline_caps) return &self.caps[index];
        var remaining = index - inline_caps;
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            if (remaining < chunk_caps) return &vm_object_cap_chunk_pool[chunk_index].caps[remaining];
            remaining -= chunk_caps;
            chunk_index = vm_object_cap_chunk_pool[chunk_index].next;
        }
        return null;
    }

    fn slotAt(self: *VmObjectCNode, index: usize) ?*VmObjectCapability {
        if (index < inline_caps) return &self.caps[index];
        var remaining = index - inline_caps;
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            if (remaining < chunk_caps) return &vm_object_cap_chunk_pool[chunk_index].caps[remaining];
            remaining -= chunk_caps;
            chunk_index = vm_object_cap_chunk_pool[chunk_index].next;
        }
        return null;
    }

    fn slotAtOrAllocate(self: *VmObjectCNode, index: usize) KernelError!*VmObjectCapability {
        if (index < inline_caps) return &self.caps[index];
        const needed_chunk_offset = (index - inline_caps) / chunk_caps;
        if (needed_chunk_offset >= chunk_pool_count) return KernelError.TableFull;
        if (self.overflow_head == invalid_chunk) self.overflow_head = try allocVmObjectCapChunk();

        var chunk_index = self.overflow_head;
        var offset: usize = 0;
        while (offset < needed_chunk_offset) : (offset += 1) {
            if (vm_object_cap_chunk_pool[chunk_index].next == invalid_chunk) {
                vm_object_cap_chunk_pool[chunk_index].next = try allocVmObjectCapChunk();
            }
            chunk_index = vm_object_cap_chunk_pool[chunk_index].next;
        }
        return &vm_object_cap_chunk_pool[chunk_index].caps[(index - inline_caps) % chunk_caps];
    }

    fn trimUnusedChunks(self: *VmObjectCNode) void {
        if (self.overflow_head == invalid_chunk) return;
        const needed_chunks = if (self.len <= inline_caps) 0 else ((self.len - inline_caps + chunk_caps - 1) / chunk_caps);
        if (needed_chunks == 0) {
            var chunk_index = self.overflow_head;
            while (chunk_index != invalid_chunk) {
                const next = vm_object_cap_chunk_pool[chunk_index].next;
                vm_object_cap_chunk_pool[chunk_index] = .{};
                chunk_index = next;
            }
            self.overflow_head = invalid_chunk;
            return;
        }

        var chunk_index = self.overflow_head;
        var offset: usize = 1;
        while (offset < needed_chunks and chunk_index != invalid_chunk) : (offset += 1) {
            chunk_index = vm_object_cap_chunk_pool[chunk_index].next;
        }
        if (chunk_index == invalid_chunk) return;
        var free_index = vm_object_cap_chunk_pool[chunk_index].next;
        vm_object_cap_chunk_pool[chunk_index].next = invalid_chunk;
        while (free_index != invalid_chunk) {
            const next = vm_object_cap_chunk_pool[free_index].next;
            vm_object_cap_chunk_pool[free_index] = .{};
            free_index = next;
        }
    }
};

const VmObjectCapChunk = struct {
    used: bool = false,
    next: u16 = VmObjectCNode.invalid_chunk,
    caps: [VmObjectCNode.chunk_caps]VmObjectCapability = undefined,
};

var empty_vm_object_cap_chunk_pool: [0]VmObjectCapChunk = .{};
var vm_object_cap_chunk_pool: []VmObjectCapChunk = empty_vm_object_cap_chunk_pool[0..];

fn allocVmObjectCapChunk() KernelError!u16 {
    var i: usize = 0;
    while (i < vm_object_cap_chunk_pool.len) : (i += 1) {
        if (vm_object_cap_chunk_pool[i].used) continue;
        vm_object_cap_chunk_pool[i] = .{ .used = true };
        return @intCast(i);
    }
    return KernelError.TableFull;
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
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(vm_object_backing_page_store), &vm_object_backing_page_store));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(vm_object_backing_page_store_next), &vm_object_backing_page_store_next));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(vm_object_backing_page_store_free_ranges), &vm_object_backing_page_store_free_ranges));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(vm_object_backing_page_store_free_range_len), &vm_object_backing_page_store_free_range_len));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(vm_object_cap_chunk_pool), &vm_object_cap_chunk_pool));
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
    cursor += @sizeOf(u64) * max_vm_object_backing_store_pages;
    cursor = std.mem.alignForward(usize, cursor, @alignOf(VmObjectBackingStoreFreeRange));
    cursor += @sizeOf(VmObjectBackingStoreFreeRange) * max_vm_object_backing_store_free_ranges;
    cursor = std.mem.alignForward(usize, cursor, @alignOf(VmObjectCapChunk));
    cursor += @sizeOf(VmObjectCapChunk) * VmObjectCNode.chunk_pool_count;
    return std.mem.alignForward(usize, cursor, 4096);
}

pub fn initRuntimeStorage(storage: []align(4096) u8) bool {
    var cursor: usize = 0;
    cap_chunk_pool = runtimeStorageSlice(CapChunk, storage, &cursor, CNode.chunk_pool_count) orelse return false;
    page_cap_refs = runtimeStorageSlice(PageCapRefEntry, storage, &cursor, page_cap_ref_slots) orelse return false;
    vm_object_backing_page_store = runtimeStorageSlice(u64, storage, &cursor, max_vm_object_backing_store_pages) orelse return false;
    vm_object_backing_page_store_free_ranges = runtimeStorageSlice(VmObjectBackingStoreFreeRange, storage, &cursor, max_vm_object_backing_store_free_ranges) orelse return false;
    vm_object_cap_chunk_pool = runtimeStorageSlice(VmObjectCapChunk, storage, &cursor, VmObjectCNode.chunk_pool_count) orelse return false;

    @memset(cap_chunk_pool, .{});
    @memset(page_cap_refs, .{});
    @memset(vm_object_backing_page_store, 0);
    @memset(vm_object_backing_page_store_free_ranges, .{});
    @memset(vm_object_cap_chunk_pool, .{});
    page_cap_ref_overflow = false;
    vm_object_backing_page_store_next = 0;
    vm_object_backing_page_store_free_range_len = 0;
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

const DmaRestoreEntry = struct {
    valid: bool = false,
    paddr: u64 = 0,
    owner: PrincipalId = default_process_principal,
    rights: Rights = .{ .cpu_read = false, .cpu_write = false, .dma = false },
};

pub const IommuNoCapDriverMode = enum(u8) {
    off,
    shadow,
    enforce,
};

pub const IommuSyncReason = enum(u8) {
    grant_dma,
    grant_no_dma,
    move_from,
    move_to,
    revoke,
};

pub const DmaDeviceId = dma_mapping_manager.DmaDeviceId;
pub const DmaDirection = dma_mapping_manager.DmaDirection;
pub const DmaMappingState = dma_mapping_manager.DmaMappingState;
pub const DmaMapping = dma_mapping_manager.DmaMapping;
const IommuMapEntry = struct {
    valid: bool = false,
    principal: PrincipalId = default_process_principal,
    device: DmaDeviceId = 0x1001,
    paddr: u64 = 0,
};

const IommuNoCapDriverState = struct {
    const max_mappings = 512;

    mode: IommuNoCapDriverMode = .off,
    mappings: [max_mappings]IommuMapEntry = [_]IommuMapEntry{.{}} ** max_mappings,
};

var empty_process_descriptors_extra: [0]ProcessDescriptor = .{};
var empty_cap_tables_extra: [0]CNode = .{};
var empty_endpoint_tables_extra: [0]EndpointCNode = .{};
var empty_cap_mailboxes_extra: [0]CapMailbox = .{};
var empty_pending_page_transfers_extra: [0]?PendingCapTransfer = .{};
var empty_ipc_buffer_tables_extra: [0]IpcBufferCNode = .{};
var empty_ipc_buffer_mailboxes_extra: [0]IpcBufferMailbox = .{};
var empty_pending_ipc_buffer_transfers_extra: [0]?PendingIpcBufferTransfer = .{};
var empty_vm_object_tables_extra: [0]VmObjectCNode = .{};
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
    ipc_buffer_tables: [principal_count]IpcBufferCNode = [_]IpcBufferCNode{.{}} ** principal_count,
    ipc_buffer_mailboxes: [principal_count]IpcBufferMailbox = [_]IpcBufferMailbox{.{}} ** principal_count,
    pending_ipc_buffer_transfers: [principal_count]?PendingIpcBufferTransfer = [_]?PendingIpcBufferTransfer{null} ** principal_count,
    vm_object_tables: [principal_count]VmObjectCNode = [_]VmObjectCNode{.{}} ** principal_count,
    fd_tables: [process_count]FdTable = [_]FdTable{.{}} ** process_count,
    vma_tables: [process_count]VmaTable = [_]VmaTable{.{}} ** process_count,
    cap_mailboxes_extra: []CapMailbox = empty_cap_mailboxes_extra[0..],
    pending_page_transfers_extra: []?PendingCapTransfer = empty_pending_page_transfers_extra[0..],
    ipc_buffer_tables_extra: []IpcBufferCNode = empty_ipc_buffer_tables_extra[0..],
    ipc_buffer_mailboxes_extra: []IpcBufferMailbox = empty_ipc_buffer_mailboxes_extra[0..],
    pending_ipc_buffer_transfers_extra: []?PendingIpcBufferTransfer = empty_pending_ipc_buffer_transfers_extra[0..],
    vm_object_tables_extra: []VmObjectCNode = empty_vm_object_tables_extra[0..],
    fd_tables_extra: []FdTable = empty_fd_tables_extra[0..],
    vma_tables_extra: []VmaTable = empty_vma_tables_extra[0..],
    fd_objects: [max_fd_objects]KernelObjectSlot = [_]KernelObjectSlot{.{}} ** max_fd_objects,
    next_fd_object_scan: usize = 0,
    native_vmos: [max_native_vmos]NativeVmoSlot = [_]NativeVmoSlot{.{}} ** max_native_vmos,
    next_native_vmo_scan: usize = 0,
    pte_sync_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, paddr: u64) void = null,
    iommu_audit_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, paddr: u64, mapped: bool, reason: IommuSyncReason) void = null,
    zero_physical_page_hook: ?*const fn (paddr: u64) bool = null,
    debug_process_lifecycle_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, reason: DebugProcessLifecycleReason) void = null,
    revoke_queue: [max_total_caps]u64 = undefined,
    revoke_subtree: [max_total_caps]u64 = undefined,
    dma_restore: [max_total_caps]DmaRestoreEntry = [_]DmaRestoreEntry{.{}} ** max_total_caps,
    dma_mappings: dma_mapping_manager.DmaMappingTable = .{},
    dma_device_domains: dma_mapping_manager.DeviceDomainTable = .{},
    iommu_caps: device_capabilities.IommuCapabilityTable = .{},
    queue_caps: device_capabilities.QueueCapabilityTable = .{},
    command_caps: device_capabilities.CommandCapabilityTable = .{},
    capsules: capsule.CapsuleTable = .{},
    iommu: IommuNoCapDriverState = .{},
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
            _ = freeVmObjectBackingPageStore(slot.page_store_start, slot.page_count);
        }
        slot.* = .{ .generation = next_generation };
    }

    fn releaseNativeVmoOwnedPages(slot: *NativeVmoSlot, free_list: *FreePageList) void {
        var page_index: usize = 0;
        while (page_index < slot.page_count) : (page_index += 1) {
            const paddr = vmObjectBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_index) orelse continue;
            if (paddr == 0) continue;
            if (free_list.canAppendPage(0, paddr)) {
                free_list.appendPage(0, paddr) catch {};
                _ = setVmObjectBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_index, 0);
            }
        }
    }

    fn createNativeVmo(self: *KernelState, kind: NativeVmoKind, size_bytes: u64) KernelError!NativeVmoRef {
        if (kind == .none or size_bytes == 0) return KernelError.InvalidState;
        const aligned_size = pageAlignUp(size_bytes);
        const page_count_u64 = aligned_size / native_page_size;
        if (page_count_u64 == 0 or page_count_u64 > max_vm_object_backing_pages) return KernelError.InvalidState;
        const page_count: u16 = @intCast(page_count_u64);
        var offset: usize = 0;
        while (offset < max_native_vmos) : (offset += 1) {
            const index = (self.next_native_vmo_scan + offset) % max_native_vmos;
            const slot = &self.native_vmos[index];
            if (slot.kind != .none or slot.ref_count != 0) continue;
            const page_store_start = allocEmptyVmObjectBackingPageStore(page_count) orelse return KernelError.TableFull;
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
        const paddr = vmObjectBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_index) orelse return null;
        if (paddr == 0) return null;
        return paddr;
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
            if (vmObjectBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_offset + i) != 0) return KernelError.InvalidState;
        }
        for (paddrs, 0..) |paddr, i| {
            if (!setVmObjectBackingPageStorePaddr(slot.page_store_start, slot.page_count, page_offset + i, paddr)) {
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

    fn objectPayloadMatches(kind: KernelObjectKind, payload: KernelObjectPayload) bool {
        return std.meta.activeTag(payload) == kind;
    }

    fn releaseKernelObjectPayload(self: *KernelState, slot: *const KernelObjectSlot) void {
        switch (slot.payload) {
            .vmo => |vmo_ref| self.releaseNativeVmo(vmo_ref),
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

    fn dmaMappingEndExclusive(paddr_start: u64, length: u64) KernelError!u64 {
        if (length == 0) return KernelError.InvalidState;
        if (paddr_start > std.math.maxInt(u64) - length) return KernelError.InvalidState;
        if (paddr_start + length > std.math.maxInt(u64) - 4095) return KernelError.InvalidState;
        return pageAlignUp(paddr_start + length);
    }

    fn validateDmaMappingPages(self: *const KernelState, owner: PrincipalId, paddr_start: u64, length: u64) KernelError!void {
        const end = try dmaMappingEndExclusive(paddr_start, length);
        var paddr = pageAlignDown(paddr_start);
        while (paddr < end) : (paddr += 4096) {
            const cap = self.getTableConst(owner).find(paddr) orelse return KernelError.CapabilityNotFound;
            if (!cap.rights.dma) return KernelError.NoDmaRight;
        }
    }

    fn syncIommuForDmaMapping(self: *KernelState, mapping: DmaMapping, reason: IommuSyncReason) KernelError!void {
        const principal: PrincipalId = @enumFromInt(mapping.owner_principal_raw);
        const end = try dmaMappingEndExclusive(mapping.paddr_start, mapping.length);
        var paddr = pageAlignDown(mapping.paddr_start);
        while (paddr < end) : (paddr += 4096) {
            try device_capabilities.syncIommuForPrincipalPaddr(self, principal, paddr, reason);
        }
    }

    pub fn hasActiveDmaMappingForPrincipalDevicePaddr(self: *const KernelState, principal: PrincipalId, device: DmaDeviceId, paddr: u64) bool {
        const owner_raw: PrincipalRaw = @intCast(@intFromEnum(principal));
        const page = pageAlignDown(paddr);
        for (self.dma_mappings.entries) |mapping| {
            if (!mapping.valid) continue;
            if (mapping.owner_principal_raw != owner_raw) continue;
            if (mapping.device != device) continue;
            const end = dmaMappingEndExclusive(mapping.paddr_start, mapping.length) catch continue;
            if (page >= pageAlignDown(mapping.paddr_start) and page < end) return true;
        }
        return false;
    }

    fn dmaMappingContainsPage(mapping: DmaMapping, paddr: u64) bool {
        const page = pageAlignDown(paddr);
        const end = dmaMappingEndExclusive(mapping.paddr_start, mapping.length) catch return false;
        return page >= pageAlignDown(mapping.paddr_start) and page < end;
    }

    pub fn removeDmaMappingsForPrincipalPaddr(self: *KernelState, principal: PrincipalId, paddr: u64) bool {
        const owner_raw: PrincipalRaw = @intCast(@intFromEnum(principal));
        var removed = false;
        var i: usize = 0;
        while (i < self.dma_mappings.entries.len) : (i += 1) {
            const mapping = self.dma_mappings.entries[i];
            if (!mapping.valid) continue;
            if (mapping.owner_principal_raw != owner_raw) continue;
            if (!dmaMappingContainsPage(mapping, paddr)) continue;
            self.dma_mappings.entries[i] = .{};
            removed = true;
        }
        return removed;
    }

    pub fn removeDmaMappingsForPrincipalDevice(self: *KernelState, principal: PrincipalId, device: DmaDeviceId) void {
        const owner_raw: PrincipalRaw = @intCast(@intFromEnum(principal));
        var i: usize = 0;
        while (i < self.dma_mappings.entries.len) : (i += 1) {
            const mapping = self.dma_mappings.entries[i];
            if (!mapping.valid) continue;
            if (mapping.owner_principal_raw != owner_raw) continue;
            if (mapping.device != device) continue;
            self.dma_mappings.entries[i] = .{};
        }
    }

    fn findDmaRestoreIndex(self: *const KernelState, paddr: u64) ?usize {
        var i: usize = 0;
        while (i < self.dma_restore.len) : (i += 1) {
            if (self.dma_restore[i].valid and self.dma_restore[i].paddr == paddr) return i;
        }
        return null;
    }

    fn saveDmaRestoreEntry(self: *KernelState, owner: PrincipalId, paddr: u64, rights: Rights) KernelError!void {
        if (self.findDmaRestoreIndex(paddr) != null) return KernelError.InvalidState;

        var i: usize = 0;
        while (i < self.dma_restore.len) : (i += 1) {
            if (self.dma_restore[i].valid) continue;
            self.dma_restore[i] = .{
                .valid = true,
                .paddr = paddr,
                .owner = owner,
                .rights = rights,
            };
            return;
        }
        return KernelError.TableFull;
    }

    fn takeDmaRestoreEntry(self: *KernelState, paddr: u64) ?DmaRestoreEntry {
        const index = self.findDmaRestoreIndex(paddr) orelse return null;
        const entry = self.dma_restore[index];
        self.dma_restore[index] = .{};
        return entry;
    }

    pub fn setIommuNoCapDriverMode(self: *KernelState, mode: IommuNoCapDriverMode) void {
        self.iommu.mode = mode;
        if (mode == .off) {
            self.iommu.mappings = [_]IommuMapEntry{.{}} ** IommuNoCapDriverState.max_mappings;
        }
    }

    pub fn getIommuNoCapDriverMode(self: *const KernelState) IommuNoCapDriverMode {
        return self.iommu.mode;
    }

    pub fn iommuFindMappingIndex(self: *const KernelState, principal: PrincipalId, device: DmaDeviceId, paddr: u64) ?usize {
        var i: usize = 0;
        while (i < self.iommu.mappings.len) : (i += 1) {
            const entry = self.iommu.mappings[i];
            if (!entry.valid) continue;
            if (entry.principal == principal and entry.device == device and entry.paddr == paddr) return i;
        }
        return null;
    }

    pub fn iommuMap(self: *KernelState, principal: PrincipalId, device: DmaDeviceId, paddr: u64) KernelError!void {
        if (self.iommuFindMappingIndex(principal, device, paddr) != null) return;
        var i: usize = 0;
        while (i < self.iommu.mappings.len) : (i += 1) {
            if (self.iommu.mappings[i].valid) continue;
            self.iommu.mappings[i] = .{
                .valid = true,
                .principal = principal,
                .device = device,
                .paddr = paddr,
            };
            return;
        }
        return KernelError.TableFull;
    }

    pub fn iommuUnmap(self: *KernelState, principal: PrincipalId, device: DmaDeviceId, paddr: u64) void {
        const index = self.iommuFindMappingIndex(principal, device, paddr) orelse return;
        self.iommu.mappings[index] = .{};
    }

    noinline fn callPteSyncHook(
        hook: *const fn (state: *const KernelState, principal: PrincipalId, paddr: u64) void,
        self: *const KernelState,
        principal: PrincipalId,
        paddr: u64,
    ) void {
        hook(self, principal, paddr);
    }

    fn isRightsSubset(child: Rights, parent: Rights) bool {
        return (!child.cpu_read or parent.cpu_read) and
            (!child.cpu_write or parent.cpu_write) and
            (!child.dma or parent.dma) and
            (!child.grant or parent.grant);
    }

    fn isVmObjectRightsSubset(child: VmObjectRights, parent: VmObjectRights) bool {
        const child_bits: u32 = @bitCast(child);
        const parent_bits: u32 = @bitCast(parent);
        return (child_bits & ~parent_bits) == 0;
    }

    fn isIpcBufferRightsSubset(child: IpcBufferRights, parent: IpcBufferRights) bool {
        const child_bits: u32 = @bitCast(child);
        const parent_bits: u32 = @bitCast(parent);
        return (child_bits & ~parent_bits) == 0;
    }

    fn mapCapsuleError(err: capsule.CapsuleError) KernelError {
        return switch (err) {
            error.InvalidState => KernelError.InvalidState,
            error.TableFull => KernelError.TableFull,
            error.NotFound => KernelError.CapabilityNotFound,
            error.Denied => KernelError.InvalidState,
            error.Revoked => KernelError.CapsuleRevoked,
        };
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

    pub fn capsuleCreateRoot(
        self: *KernelState,
        owner: PrincipalId,
        kind: CapsuleKind,
        rights: CapsuleRights,
        metadata: CapsuleMetadata,
    ) KernelError!u64 {
        try self.requireActivePrincipal(owner);
        return self.capsules.allocRoot(@intFromEnum(owner), kind, rights, metadata) catch |err| return mapCapsuleError(err);
    }

    pub fn capsuleDerive(
        self: *KernelState,
        owner: PrincipalId,
        parent_token: u64,
        kind: CapsuleKind,
        rights: CapsuleRights,
        metadata: CapsuleMetadata,
    ) KernelError!u64 {
        try self.requireActivePrincipal(owner);
        return self.capsules.derive(@intFromEnum(owner), parent_token, kind, rights, metadata) catch |err| return mapCapsuleError(err);
    }

    pub fn capsuleGrant(
        self: *KernelState,
        owner: PrincipalId,
        child: PrincipalId,
        token: u64,
        rights: CapsuleRights,
    ) KernelError!u64 {
        try self.requireActivePrincipal(owner);
        try self.requireActivePrincipal(child);
        return self.capsules.grant(@intFromEnum(owner), @intFromEnum(child), token, rights) catch |err| return mapCapsuleError(err);
    }

    pub fn capsuleAuthorize(
        self: *const KernelState,
        owner: PrincipalId,
        token: u64,
        kind: CapsuleKind,
        required_rights: CapsuleRights,
    ) KernelError!void {
        try self.requireActivePrincipal(owner);
        return self.capsules.authorize(@intFromEnum(owner), token, kind, required_rights) catch |err| return mapCapsuleError(err);
    }

    pub fn capsuleSnapshot(self: *const KernelState, token: u64) KernelError!CapsuleSnapshot {
        return self.capsules.snapshot(token) catch |err| return mapCapsuleError(err);
    }

    pub fn capsuleCollectSubtreeTokens(
        self: *const KernelState,
        owner: PrincipalId,
        token: u64,
        out: []u64,
    ) KernelError!usize {
        try self.requireActivePrincipal(owner);
        return self.capsules.collectSubtreeTokens(@intFromEnum(owner), token, out) catch |err| return mapCapsuleError(err);
    }

    pub fn capsuleRevokeSubtree(self: *KernelState, owner: PrincipalId, token: u64) KernelError!usize {
        try self.requireActivePrincipal(owner);
        return self.capsules.revokeSubtree(@intFromEnum(owner), token) catch |err| return mapCapsuleError(err);
    }

    pub fn capsuleCloseSubtree(self: *KernelState, owner: PrincipalId, token: u64) KernelError!usize {
        try self.requireActivePrincipal(owner);
        return self.capsules.closeSubtree(@intFromEnum(owner), token) catch |err| return mapCapsuleError(err);
    }

    pub fn releasePrincipalCapsules(self: *KernelState, owner: PrincipalId) usize {
        return self.capsules.releaseOwner(@intFromEnum(owner));
    }

    fn capsuleGrantBitFromParent(parent: CapsuleSnapshot) bool {
        return parent.rights.grant;
    }

    fn deviceCapsuleForChild(
        self: *const KernelState,
        owner: PrincipalId,
        device_token: u64,
        required_rights: CapsuleRights,
    ) KernelError!CapsuleSnapshot {
        var effective_rights = required_rights;
        effective_rights.query = true;
        try self.capsuleAuthorize(owner, device_token, .device, effective_rights);
        const device = try self.capsuleSnapshot(device_token);
        if (device.metadata.device == 0) return KernelError.InvalidState;
        return device;
    }

    pub fn deviceCapsuleCreate(
        self: *KernelState,
        owner: PrincipalId,
        device_id: u64,
        rights: CapsuleRights,
    ) KernelError!u64 {
        if (device_id == 0) return KernelError.InvalidState;
        return self.capsuleCreateRoot(owner, .device, rights, .{ .device = device_id });
    }

    pub fn deviceCapsuleDeriveMmio(
        self: *KernelState,
        owner: PrincipalId,
        device_token: u64,
        bar_index: u32,
        bar_paddr: u64,
        user_va: u64,
        size: u64,
        flags: u32,
    ) KernelError!u64 {
        if (bar_paddr == 0 or size == 0) return KernelError.InvalidState;
        const device = try self.deviceCapsuleForChild(owner, device_token, .{ .bar_map = true });
        return self.capsuleDerive(owner, device_token, .mmio, .{
            .query = true,
            .bar_map = true,
            .grant = capsuleGrantBitFromParent(device),
        }, .{
            .device = device.metadata.device,
            .object_id = bar_paddr,
            .user_va = user_va,
            .size = size,
            .index = bar_index,
            .flags = flags,
        });
    }

    pub fn deviceCapsuleDeriveDmaBuffer(
        self: *KernelState,
        owner: PrincipalId,
        device_token: u64,
        user_va: u64,
        iova: u64,
        size: u64,
        flags: u32,
    ) KernelError!u64 {
        if (user_va == 0 or iova == 0 or size == 0) return KernelError.InvalidState;
        const device = try self.deviceCapsuleForChild(owner, device_token, .{ .dma_alloc = true });
        return self.capsuleDerive(owner, device_token, .dma_buffer, .{
            .query = true,
            .dma_alloc = true,
            .dma_map_user = device.rights.dma_map_user,
            .grant = capsuleGrantBitFromParent(device),
        }, .{
            .device = device.metadata.device,
            .user_va = user_va,
            .iova = iova,
            .size = size,
            .flags = flags,
        });
    }

    pub fn deviceCapsuleDeriveDmaMapping(
        self: *KernelState,
        owner: PrincipalId,
        device_token: u64,
        user_va: u64,
        iova: u64,
        size: u64,
        direction: CapsuleDmaDirection,
        flags: u32,
    ) KernelError!u64 {
        if (user_va == 0 or iova == 0 or size == 0) return KernelError.InvalidState;
        const device = try self.deviceCapsuleForChild(owner, device_token, .{ .dma_map_user = true });
        return self.capsuleDerive(owner, device_token, .dma_mapping, .{
            .query = true,
            .dma_map_user = true,
            .grant = capsuleGrantBitFromParent(device),
        }, .{
            .device = device.metadata.device,
            .user_va = user_va,
            .iova = iova,
            .size = size,
            .flags = flags | (@as(u32, @intFromEnum(direction)) & 0x3),
        });
    }

    pub fn dmaBufferCapsuleDeriveMapping(
        self: *KernelState,
        owner: PrincipalId,
        dma_buffer_token: u64,
        iova: u64,
        size: u64,
        direction: CapsuleDmaDirection,
        flags: u32,
    ) KernelError!u64 {
        if (iova == 0 or size == 0) return KernelError.InvalidState;
        try self.capsuleAuthorize(owner, dma_buffer_token, .dma_buffer, .{ .dma_map_user = true });
        const buffer = try self.capsuleSnapshot(dma_buffer_token);
        if (size > buffer.metadata.size) return KernelError.InvalidState;
        return self.capsuleDerive(owner, dma_buffer_token, .dma_mapping, .{
            .query = true,
            .dma_map_user = true,
            .grant = capsuleGrantBitFromParent(buffer),
        }, .{
            .device = buffer.metadata.device,
            .object_id = dma_buffer_token,
            .user_va = buffer.metadata.user_va,
            .iova = iova,
            .size = size,
            .flags = flags | (@as(u32, @intFromEnum(direction)) & 0x3),
        });
    }

    pub fn deviceCapsuleDeriveIrq(
        self: *KernelState,
        owner: PrincipalId,
        device_token: u64,
        kind: CapsuleIrqKind,
        vector: u32,
        flags: u32,
    ) KernelError!u64 {
        const device = try self.deviceCapsuleForChild(owner, device_token, .{ .irq_bind = true });
        return self.capsuleDerive(owner, device_token, .irq, .{
            .query = true,
            .irq_bind = true,
            .grant = capsuleGrantBitFromParent(device),
        }, .{
            .device = device.metadata.device,
            .index = vector,
            .flags = flags | ((@as(u32, @intFromEnum(kind)) & 0x3) << 16),
        });
    }

    pub fn mmioCapsuleSnapshotForAccess(
        self: *const KernelState,
        owner: PrincipalId,
        token: u64,
    ) KernelError!CapsuleSnapshot {
        try self.capsuleAuthorize(owner, token, .mmio, .{ .bar_map = true });
        return self.capsuleSnapshot(token);
    }

    pub fn dmaBufferCapsuleSnapshotForAccess(
        self: *const KernelState,
        owner: PrincipalId,
        token: u64,
    ) KernelError!CapsuleSnapshot {
        try self.capsuleAuthorize(owner, token, .dma_buffer, .{ .dma_alloc = true });
        return self.capsuleSnapshot(token);
    }

    pub fn dmaMappingCapsuleSnapshotForAccess(
        self: *const KernelState,
        owner: PrincipalId,
        token: u64,
    ) KernelError!CapsuleSnapshot {
        try self.capsuleAuthorize(owner, token, .dma_mapping, .{ .dma_map_user = true });
        return self.capsuleSnapshot(token);
    }

    pub fn irqCapsuleSnapshotForAccess(
        self: *const KernelState,
        owner: PrincipalId,
        token: u64,
    ) KernelError!CapsuleSnapshot {
        try self.capsuleAuthorize(owner, token, .irq, .{ .irq_bind = true });
        return self.capsuleSnapshot(token);
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

    fn ipcBufferTableForProcessIndex(self: *KernelState, index: usize) *IpcBufferCNode {
        if (extraIndex(index)) |extra| return &self.ipc_buffer_tables_extra[extra];
        return &self.ipc_buffer_tables[index];
    }

    fn ipcBufferTableForProcessIndexConst(self: *const KernelState, index: usize) *const IpcBufferCNode {
        if (extraIndex(index)) |extra| return &self.ipc_buffer_tables_extra[extra];
        return &self.ipc_buffer_tables[index];
    }

    fn ipcBufferMailboxForProcessIndex(self: *KernelState, index: usize) *IpcBufferMailbox {
        if (extraIndex(index)) |extra| return &self.ipc_buffer_mailboxes_extra[extra];
        return &self.ipc_buffer_mailboxes[index];
    }

    fn pendingIpcBufferTransferForProcessIndex(self: *KernelState, index: usize) *?PendingIpcBufferTransfer {
        if (extraIndex(index)) |extra| return &self.pending_ipc_buffer_transfers_extra[extra];
        return &self.pending_ipc_buffer_transfers[index];
    }

    fn capMailboxForPrincipal(self: *KernelState, principal: PrincipalId) *CapMailbox {
        if (processIndexFromPrincipal(principal)) |index| return self.capMailboxForProcessIndex(index);
        return &self.cap_mailboxes[self.principalStorageIndex(principal)];
    }

    fn pendingPageTransferForPrincipal(self: *KernelState, principal: PrincipalId) *?PendingCapTransfer {
        if (processIndexFromPrincipal(principal)) |index| return self.pendingPageTransferForProcessIndex(index);
        return &self.pending_page_transfers[self.principalStorageIndex(principal)];
    }

    fn ipcBufferMailboxForPrincipal(self: *KernelState, principal: PrincipalId) *IpcBufferMailbox {
        if (processIndexFromPrincipal(principal)) |index| return self.ipcBufferMailboxForProcessIndex(index);
        return &self.ipc_buffer_mailboxes[self.principalStorageIndex(principal)];
    }

    fn pendingIpcBufferTransferForPrincipal(self: *KernelState, principal: PrincipalId) *?PendingIpcBufferTransfer {
        if (processIndexFromPrincipal(principal)) |index| return self.pendingIpcBufferTransferForProcessIndex(index);
        return &self.pending_ipc_buffer_transfers[self.principalStorageIndex(principal)];
    }

    fn vmObjectTableForProcessIndex(self: *KernelState, index: usize) *VmObjectCNode {
        if (extraIndex(index)) |extra| return &self.vm_object_tables_extra[extra];
        return &self.vm_object_tables[index];
    }

    fn vmObjectTableForProcessIndexConst(self: *const KernelState, index: usize) *const VmObjectCNode {
        if (extraIndex(index)) |extra| return &self.vm_object_tables_extra[extra];
        return &self.vm_object_tables[index];
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
        self.ipcBufferTableForProcessIndex(index).* = .{};
        self.ipcBufferMailboxForProcessIndex(index).reset();
        self.pendingIpcBufferTransferForProcessIndex(index).* = null;
        self.vmObjectTableForProcessIndex(index).reset();
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
        const new_ipc_buffer = allocKernelSlice(IpcBufferCNode, free_list, extra_count) orelse return false;
        const new_ipc_mailbox = allocKernelSlice(IpcBufferMailbox, free_list, extra_count) orelse return false;
        const new_ipc_pending = allocKernelSlice(?PendingIpcBufferTransfer, free_list, extra_count) orelse return false;
        const new_vm = allocKernelSlice(VmObjectCNode, free_list, extra_count) orelse return false;
        const new_fd = allocKernelSlice(FdTable, free_list, extra_count) orelse return false;
        const new_vma = allocKernelSlice(VmaTable, free_list, extra_count) orelse return false;

        @memcpy(new_desc[0..old_extra_count], self.process_descriptors_extra);
        @memcpy(new_cap[0..old_extra_count], self.cap_tables_extra);
        @memcpy(new_endpoint[0..old_extra_count], self.endpoint_tables_extra);
        @memcpy(new_cap_mailbox[0..old_extra_count], self.cap_mailboxes_extra);
        @memcpy(new_pending[0..old_extra_count], self.pending_page_transfers_extra);
        @memcpy(new_ipc_buffer[0..old_extra_count], self.ipc_buffer_tables_extra);
        @memcpy(new_ipc_mailbox[0..old_extra_count], self.ipc_buffer_mailboxes_extra);
        @memcpy(new_ipc_pending[0..old_extra_count], self.pending_ipc_buffer_transfers_extra);
        @memcpy(new_vm[0..old_extra_count], self.vm_object_tables_extra);
        @memcpy(new_fd[0..old_extra_count], self.fd_tables_extra);
        @memcpy(new_vma[0..old_extra_count], self.vma_tables_extra);

        var i = old_extra_count;
        while (i < extra_count) : (i += 1) {
            new_desc[i] = .{};
            new_cap[i] = .{};
            new_endpoint[i] = .{};
            new_cap_mailbox[i] = .{};
            new_pending[i] = null;
            new_ipc_buffer[i] = .{};
            new_ipc_mailbox[i] = .{};
            new_ipc_pending[i] = null;
            new_vm[i] = .{};
            new_fd[i] = .{};
            new_vma[i] = .{};
        }

        self.process_descriptors_extra = new_desc;
        self.cap_tables_extra = new_cap;
        self.endpoint_tables_extra = new_endpoint;
        self.cap_mailboxes_extra = new_cap_mailbox;
        self.pending_page_transfers_extra = new_pending;
        self.ipc_buffer_tables_extra = new_ipc_buffer;
        self.ipc_buffer_mailboxes_extra = new_ipc_mailbox;
        self.pending_ipc_buffer_transfers_extra = new_ipc_pending;
        self.vm_object_tables_extra = new_vm;
        self.fd_tables_extra = new_fd;
        self.vma_tables_extra = new_vma;
        self.process_capacity = capacity;
        return true;
    }

    fn clearPrincipalTablesForReuse(self: *KernelState, index: usize) void {
        const principal = processPrincipal(index);
        _ = self.releasePrincipalCapsules(principal);
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

    pub fn markProcessBuilderSuspended(self: *KernelState, principal: PrincipalId, owner: PrincipalId) KernelError!void {
        const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
        const desc = self.processDescriptorSlot(index) orelse return KernelError.InvalidState;
        if (!desc.active) return KernelError.InvalidState;
        try self.requireActiveProcess(owner);
        desc.process_builder_owner = owner;
        desc.process_builder_suspended = true;
    }

    pub fn processBuilderOwnerMatches(self: *const KernelState, principal: PrincipalId, owner: PrincipalId) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        const desc = (self.processDescriptorSlotConst(index) orelse return false).*;
        if (!desc.active or !desc.process_builder_suspended) return false;
        return desc.process_builder_owner == owner;
    }

    pub fn clearProcessBuilderSuspended(self: *KernelState, principal: PrincipalId) KernelError!void {
        const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
        const desc = self.processDescriptorSlot(index) orelse return KernelError.InvalidState;
        if (!desc.active) return KernelError.InvalidState;
        desc.process_builder_owner = null;
        desc.process_builder_suspended = false;
    }

    pub fn setAbiTrapDelegate(
        self: *KernelState,
        principal: PrincipalId,
        endpoint_id: u64,
        flavor: u32,
        request_page_va: u64,
    ) KernelError!void {
        const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
        const desc = self.processDescriptorSlot(index) orelse return KernelError.InvalidState;
        if (!desc.active) return KernelError.InvalidState;
        if (self.endpointTargetFor(principal, endpoint_id) == null) return KernelError.EndpointNotFound;
        if (request_page_va == 0 or (request_page_va & 0xFFF) != 0 or !capability.isUserCanonicalVa(request_page_va)) return KernelError.InvalidState;
        desc.abi_trap_delegate_endpoint_id = endpoint_id;
        desc.abi_trap_delegate_flavor = flavor;
        desc.abi_trap_request_page_va = request_page_va;
    }

    pub fn clearAbiTrapDelegate(self: *KernelState, principal: PrincipalId) KernelError!void {
        const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
        const desc = self.processDescriptorSlot(index) orelse return KernelError.InvalidState;
        if (!desc.active) return KernelError.InvalidState;
        desc.abi_trap_delegate_endpoint_id = 0;
        desc.abi_trap_delegate_flavor = 0;
        desc.abi_trap_request_page_va = 0;
    }

    pub fn abiTrapDelegateFor(self: *const KernelState, principal: PrincipalId) ?AbiTrapDelegate {
        const index = processIndexFromPrincipal(principal) orelse return null;
        const desc = (self.processDescriptorSlotConst(index) orelse return null).*;
        if (!desc.active or desc.abi_trap_delegate_endpoint_id == 0) return null;
        return .{
            .endpoint_id = desc.abi_trap_delegate_endpoint_id,
            .flavor = desc.abi_trap_delegate_flavor,
            .request_page_va = desc.abi_trap_request_page_va,
        };
    }

    pub fn markProcessFaulted(self: *KernelState, principal: PrincipalId, fault_vector: u8) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        const desc = self.processDescriptorSlot(index) orelse return false;
        if (!desc.active) return false;
        self.releaseFdTableForProcessIndex(index);
        _ = self.releasePrincipalCapsules(principal);
        desc.active = false;
        desc.bootstrap_owner = false;
        desc.process_builder_owner = null;
        desc.process_builder_suspended = false;
        desc.abi_trap_delegate_endpoint_id = 0;
        desc.abi_trap_delegate_flavor = 0;
        desc.abi_trap_request_page_va = 0;
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
        _ = self.releasePrincipalCapsules(principal);
        desc.active = false;
        desc.bootstrap_owner = false;
        desc.process_builder_owner = null;
        desc.process_builder_suspended = false;
        desc.abi_trap_delegate_endpoint_id = 0;
        desc.abi_trap_delegate_flavor = 0;
        desc.abi_trap_request_page_va = 0;
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
        _ = self.releasePrincipalCapsules(principal);
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
        self.ipc_buffer_tables[process_count] = .{};
        self.ipc_buffer_mailboxes[process_count].reset();
        self.pending_ipc_buffer_transfers[process_count] = null;
        self.vm_object_tables[process_count].reset();
        resetVmObjectBackingPageStore();
        self.capsules = .{};
        self.published_service_endpoints = .{};
        self.resetKernelObjectTable();
        self.resetNativeVmoTable();
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

    pub fn startDma(self: *KernelState, owner: PrincipalId, paddr: u64) KernelError!void {
        try self.requireActiveProcess(owner);
        const owner_table = self.getTable(owner);
        const owner_cap = owner_table.find(paddr) orelse return KernelError.CapabilityNotFound;
        if (!owner_cap.rights.dma) return KernelError.NoDmaRight;
        try self.saveDmaRestoreEntry(owner, paddr, owner_cap.rights);

        // DMA 開始時は moveCap 経由で Device0 へ委譲する。
        try self.moveCap(
            owner,
            .Device0,
            paddr,
            .{
                .cpu_read = false,
                .cpu_write = false,
                .dma = true,
            },
        );
    }

    pub fn completeDma(self: *KernelState, paddr: u64) KernelError!void {
        const dev_table = self.getTable(.Device0);
        const dev_cap = dev_table.find(paddr) orelse return KernelError.CapabilityNotFound;
        if (!dev_cap.rights.dma) return KernelError.NoDmaRight;
        const restore = self.takeDmaRestoreEntry(paddr) orelse return KernelError.InvalidState;
        if (self.getTable(restore.owner).find(paddr) != null) return KernelError.InvalidState;

        var restored = dev_cap.*;
        restored.rights = restore.rights;
        _ = dev_table.removeByPaddr(paddr);
        try self.getTable(restore.owner).add(restored);
        if (self.pte_sync_hook) |hook| {
            callPteSyncHook(hook, self, .Device0, paddr);
            callPteSyncHook(hook, self, restore.owner, paddr);
        }
    }

    // Stage1: hold DMA mapping metadata before wiring full syscall/IOMMU flow.
    pub fn dmaMapCreateStage1(
        self: *KernelState,
        owner: PrincipalId,
        device: DmaDeviceId,
        paddr_start: u64,
        length: u64,
        direction: DmaDirection,
    ) KernelError!u64 {
        try self.requireActiveProcess(owner);
        try self.validateDmaMappingPages(owner, paddr_start, length);
        const token = self.dma_mappings.alloc(
            @intFromEnum(owner),
            device,
            paddr_start,
            length,
            direction,
        ) catch |err| {
            return switch (err) {
                error.InvalidState => KernelError.InvalidState,
                error.TableFull => KernelError.TableFull,
                else => KernelError.InvalidState,
            };
        };
        const mapping = self.dma_mappings.findByToken(token) orelse return KernelError.InvalidState;
        self.syncIommuForDmaMapping(mapping.*, .grant_dma) catch |err| {
            _ = self.dma_mappings.remove(token);
            return err;
        };
        return token;
    }

    pub fn dmaMapFindStage1(self: *const KernelState, token: u64) ?*const DmaMapping {
        return self.dma_mappings.findByToken(token);
    }

    pub fn dmaMapSetStateStage1(
        self: *KernelState,
        token: u64,
        state: DmaMappingState,
    ) KernelError!void {
        self.dma_mappings.setState(token, state) catch |err| switch (err) {
            error.NotFound => return KernelError.CapabilityNotFound,
            error.InvalidState => return KernelError.InvalidState,
            error.TableFull => return KernelError.TableFull,
            error.Denied => return KernelError.InvalidState,
        };
    }

    pub fn dmaMapReleaseStage1(self: *KernelState, token: u64) KernelError!void {
        const mapping = (self.dma_mappings.findByToken(token) orelse return KernelError.CapabilityNotFound).*;
        self.dma_mappings.release(token) catch |err| switch (err) {
            error.NotFound => return KernelError.CapabilityNotFound,
            error.InvalidState => return KernelError.InvalidState,
            error.TableFull => return KernelError.TableFull,
            error.Denied => return KernelError.InvalidState,
        };
        try self.syncIommuForDmaMapping(mapping, .revoke);
    }

    pub fn dmaBindDeviceDomainStage1(
        self: *KernelState,
        device: DmaDeviceId,
        domain_id: u32,
    ) KernelError!void {
        self.dma_device_domains.bind(device, domain_id) catch |err| switch (err) {
            error.TableFull => return KernelError.TableFull,
            error.Denied => return KernelError.InvalidState,
            error.InvalidState => return KernelError.InvalidState,
            error.NotFound => return KernelError.CapabilityNotFound,
        };
    }

    pub fn dmaDeviceDomainStage1(self: *const KernelState, device: DmaDeviceId) ?u32 {
        return self.dma_device_domains.domainFor(device);
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
        endpoint_generation_fast_mirror = self.endpoint_generation;
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

    pub fn getVmObjectTable(self: *KernelState, principal: PrincipalId) *VmObjectCNode {
        if (processIndexFromPrincipal(principal)) |index| return self.vmObjectTableForProcessIndex(index);
        return &self.vm_object_tables[self.principalStorageIndex(principal)];
    }

    pub fn getVmObjectTableConst(self: *const KernelState, principal: PrincipalId) *const VmObjectCNode {
        if (processIndexFromPrincipal(principal)) |index| return self.vmObjectTableForProcessIndexConst(index);
        return &self.vm_object_tables[self.principalStorageIndex(principal)];
    }

    pub fn getIpcBufferTable(self: *KernelState, principal: PrincipalId) *IpcBufferCNode {
        if (processIndexFromPrincipal(principal)) |index| return self.ipcBufferTableForProcessIndex(index);
        return &self.ipc_buffer_tables[self.principalStorageIndex(principal)];
    }

    pub fn getIpcBufferTableConst(self: *const KernelState, principal: PrincipalId) *const IpcBufferCNode {
        if (processIndexFromPrincipal(principal)) |index| return self.ipcBufferTableForProcessIndexConst(index);
        return &self.ipc_buffer_tables[self.principalStorageIndex(principal)];
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

    pub fn createIpcBufferFromPage(
        self: *KernelState,
        owner: PrincipalId,
        paddr: u64,
        rights: IpcBufferRights,
        role: IpcBufferRole,
    ) KernelError!u64 {
        try self.requireActiveProcess(owner);
        const page_cap = self.getTableConst(owner).find(paddr) orelse return KernelError.CapabilityNotFound;
        if (rights.read and !page_cap.rights.cpu_read) return KernelError.InvalidState;
        if (rights.write and !page_cap.rights.cpu_write) return KernelError.InvalidState;
        if (rights.map and !(page_cap.rights.cpu_read or page_cap.rights.cpu_write)) return KernelError.InvalidState;
        if (rights.grant and !page_cap.rights.grant) return KernelError.InvalidState;

        const root_id = self.allocCapId();
        try self.getIpcBufferTable(owner).add(.{
            .paddr = paddr,
            .rights = rights,
            .role = role,
            .cap_id = root_id,
            .root_cap_id = root_id,
            .parent_cap_id = 0,
        });
        return root_id;
    }

    pub fn installTrustedIpcBufferFromPage(
        self: *KernelState,
        owner: PrincipalId,
        paddr: u64,
        rights: IpcBufferRights,
        role: IpcBufferRole,
    ) KernelError!u64 {
        try self.requireActiveProcess(owner);

        const root_id = self.allocCapId();
        try self.getIpcBufferTable(owner).add(.{
            .paddr = paddr,
            .rights = rights,
            .role = role,
            .cap_id = root_id,
            .root_cap_id = root_id,
            .parent_cap_id = 0,
        });
        return root_id;
    }

    pub fn grantIpcBufferCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        cap_id: u64,
        rights: IpcBufferRights,
    ) KernelError!u64 {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);
        const src_cap = self.getIpcBufferTableConst(from).findByCapId(cap_id) orelse return KernelError.CapabilityNotFound;
        if (!src_cap.rights.grant) return KernelError.InvalidState;
        if (!isIpcBufferRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;

        const child_id = self.allocCapId();
        try self.getIpcBufferTable(to).add(.{
            .paddr = src_cap.paddr,
            .rights = rights,
            .role = src_cap.role,
            .cap_id = child_id,
            .root_cap_id = src_cap.root_cap_id,
            .parent_cap_id = src_cap.cap_id,
        });
        return child_id;
    }

    pub fn moveIpcBufferCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        cap_id: u64,
        rights: IpcBufferRights,
    ) KernelError!u64 {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);
        const src_cap = self.getIpcBufferTableConst(from).findByCapId(cap_id) orelse return KernelError.CapabilityNotFound;
        if (!isIpcBufferRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;

        const moved = IpcBufferCapability{
            .paddr = src_cap.paddr,
            .rights = rights,
            .role = src_cap.role,
            .cap_id = src_cap.cap_id,
            .root_cap_id = src_cap.root_cap_id,
            .parent_cap_id = src_cap.parent_cap_id,
        };
        try self.getIpcBufferTable(to).add(moved);
        _ = self.getIpcBufferTable(from).removeByCapId(cap_id);
        return moved.cap_id;
    }

    pub fn grantIpcBufferCapOnEndpoint(
        self: *KernelState,
        from: PrincipalId,
        endpoint_id: u64,
        cap_id: u64,
        rights: IpcBufferRights,
    ) KernelError!u64 {
        try self.requireActiveProcess(from);
        const target = self.endpointTargetFor(from, endpoint_id) orelse return KernelError.EndpointNotFound;
        return self.grantIpcBufferCap(from, target, cap_id, rights);
    }

    pub fn shareIpcBufferCapOnEndpoint(
        self: *KernelState,
        from: PrincipalId,
        endpoint_id: u64,
        cap_id: u64,
        rights: IpcBufferRights,
    ) KernelError!void {
        try self.requireActiveProcess(from);
        const target = self.endpointTargetFor(from, endpoint_id) orelse return KernelError.EndpointNotFound;
        const src_cap = self.getIpcBufferTableConst(from).findByCapId(cap_id) orelse return KernelError.CapabilityNotFound;
        if (!src_cap.rights.grant) return KernelError.InvalidState;
        if (!isIpcBufferRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;
        try self.ipcBufferMailboxForPrincipal(target).push(.{
            .transfer_id = self.allocTransferId(),
            .sender = from,
            .endpoint_id = endpoint_id,
            .cap_id = cap_id,
            .rights = rights,
            .retain_sender = true,
        });
    }

    pub fn installVmObjectCap(
        self: *KernelState,
        owner: PrincipalId,
        page_paddrs: []const u64,
        page_offset_bytes: u16,
        size_bytes: u64,
        rights: VmObjectRights,
    ) KernelError!u64 {
        try self.requireActiveProcess(owner);
        const backing = VmObjectBacking.init(page_paddrs, page_offset_bytes, size_bytes) orelse return KernelError.InvalidState;

        const root_id = self.allocCapId();
        try self.getVmObjectTable(owner).add(.{
            .backing = backing,
            .rights = rights,
            .cap_id = root_id,
            .root_cap_id = root_id,
            .parent_cap_id = 0,
        });
        return root_id;
    }

    pub fn grantVmObjectCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        cap_id: u64,
        rights: VmObjectRights,
    ) KernelError!u64 {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);

        const src_cap = self.getVmObjectTableConst(from).findByCapId(cap_id) orelse return KernelError.VmObjectCapabilityNotFound;
        if (!src_cap.rights.grant) return KernelError.InvalidState;
        if (!isVmObjectRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;

        const child_id = self.allocCapId();
        try self.getVmObjectTable(to).add(.{
            .backing = src_cap.backing,
            .rights = rights,
            .cap_id = child_id,
            .root_cap_id = src_cap.root_cap_id,
            .parent_cap_id = src_cap.cap_id,
        });
        return child_id;
    }

    fn anyVmObjectCapWithRoot(self: *const KernelState, root_cap_id: u64) bool {
        var pidx: usize = 0;
        while (pidx < self.process_capacity) : (pidx += 1) {
            const table = self.vmObjectTableForProcessIndexConst(pidx);
            var i: usize = 0;
            while (i < table.len) : (i += 1) {
                const cap = table.slotAtConst(i) orelse break;
                if (cap.root_cap_id == root_cap_id) return true;
            }
        }
        const device_table = &self.vm_object_tables[process_count];
        var i: usize = 0;
        while (i < device_table.len) : (i += 1) {
            const cap = device_table.slotAtConst(i) orelse break;
            if (cap.root_cap_id == root_cap_id) return true;
        }
        return false;
    }

    fn anyPrincipalHasMappedPaddr(self: *const KernelState, paddr: u64) bool {
        var pidx: usize = 0;
        while (pidx < self.process_capacity) : (pidx += 1) {
            const principal = processPrincipal(pidx);
            if (capability.principalHasMappedPaddr(principal, paddr)) return true;
        }
        return false;
    }

    fn releaseVmObjectBackingIfUnreferenced(
        self: *KernelState,
        backing: VmObjectBacking,
        root_cap_id: u64,
        free_list: *FreePageList,
    ) void {
        if (self.anyVmObjectCapWithRoot(root_cap_id)) return;
        var i: usize = 0;
        while (i < backing.page_count) : (i += 1) {
            const paddr = backing.pagePaddr(i) orelse return;
            if (self.anyPrincipalHasMappedPaddr(paddr)) return;
            if (!free_list.canAppendPage(0, paddr)) return;
        }
        i = 0;
        while (i < backing.page_count) : (i += 1) {
            const paddr = backing.pagePaddr(i) orelse return;
            free_list.appendPage(0, paddr) catch return;
        }
        _ = freeVmObjectBackingPageStore(backing.page_store_start, backing.page_count);
    }

    pub fn revokeVmObjectCapTree(
        self: *KernelState,
        owner: PrincipalId,
        cap_id: u64,
        free_list: *FreePageList,
    ) KernelError!void {
        try self.requireActiveProcess(owner);
        const start_cap = self.getVmObjectTableConst(owner).findByCapId(cap_id) orelse return KernelError.VmObjectCapabilityNotFound;
        const start_id = start_cap.cap_id;

        const queue = &self.revoke_queue;
        var queue_len: usize = 0;
        var queue_head: usize = 0;
        queue[0] = start_id;
        queue_len = 1;

        const subtree = &self.revoke_subtree;
        var subtree_len: usize = 0;

        while (queue_head < queue_len) : (queue_head += 1) {
            const current_id = queue[queue_head];
            if (containsCapId(subtree[0..subtree_len], current_id)) continue;
            if (subtree_len >= self.revoke_subtree.len) return KernelError.RevokeOverflow;
            subtree[subtree_len] = current_id;
            subtree_len += 1;

            var pidx: usize = 0;
            while (pidx < self.process_capacity) : (pidx += 1) {
                const table = self.vmObjectTableForProcessIndexConst(pidx);
                var i: usize = 0;
                while (i < table.len) : (i += 1) {
                    const cap = table.slotAtConst(i) orelse break;
                    if (cap.parent_cap_id != current_id) continue;
                    if (containsCapId(queue[0..queue_len], cap.cap_id)) continue;
                    if (queue_len >= self.revoke_queue.len) return KernelError.RevokeOverflow;
                    queue[queue_len] = cap.cap_id;
                    queue_len += 1;
                }
            }
            const table = &self.vm_object_tables[process_count];
            var i: usize = 0;
            while (i < table.len) : (i += 1) {
                const cap = table.slotAtConst(i) orelse break;
                if (cap.parent_cap_id != current_id) continue;
                if (containsCapId(queue[0..queue_len], cap.cap_id)) continue;
                if (queue_len >= self.revoke_queue.len) return KernelError.RevokeOverflow;
                queue[queue_len] = cap.cap_id;
                queue_len += 1;
            }
        }

        var s: usize = 0;
        while (s < subtree_len) : (s += 1) {
            const revoke_id = subtree[s];
            var pidx: usize = 0;
            while (pidx < self.process_capacity) : (pidx += 1) {
                const table = self.vmObjectTableForProcessIndex(pidx);
                const removed = table.removeByCapId(revoke_id) orelse continue;
                self.releaseVmObjectBackingIfUnreferenced(removed.backing, removed.root_cap_id, free_list);
                break;
            }
            const table = &self.vm_object_tables[process_count];
            const removed = table.removeByCapId(revoke_id) orelse continue;
            self.releaseVmObjectBackingIfUnreferenced(removed.backing, removed.root_cap_id, free_list);
        }
    }

    pub fn dropVmObjectCap(
        self: *KernelState,
        owner: PrincipalId,
        cap_id: u64,
        free_list: *FreePageList,
    ) KernelError!void {
        try self.requireActiveProcess(owner);
        const removed = self.getVmObjectTable(owner).removeByCapId(cap_id) orelse return KernelError.VmObjectCapabilityNotFound;
        self.releaseVmObjectBackingIfUnreferenced(removed.backing, removed.root_cap_id, free_list);
    }

    pub fn releasePrincipalVmObjectCaps(
        self: *KernelState,
        owner: PrincipalId,
        free_list: *FreePageList,
    ) void {
        const table = self.getVmObjectTable(owner);
        while (table.len > 0) {
            const cap = table.slotAtConst(table.len - 1) orelse break;
            const cap_id = cap.cap_id;
            const removed = table.removeByCapId(cap_id) orelse break;
            self.releaseVmObjectBackingIfUnreferenced(removed.backing, removed.root_cap_id, free_list);
        }
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

    fn debugVmObjectRootSeenBeforeProcess(
        self: *const KernelState,
        root_cap_id: u64,
        owner_index: usize,
        cap_index: usize,
    ) bool {
        var pidx: usize = 0;
        while (pidx < owner_index) : (pidx += 1) {
            const table = self.vmObjectTableForProcessIndexConst(pidx);
            var i: usize = 0;
            while (i < table.len) : (i += 1) {
                const cap = table.slotAtConst(i) orelse break;
                if (cap.root_cap_id == root_cap_id) return true;
            }
        }
        const table = self.vmObjectTableForProcessIndexConst(owner_index);
        var i: usize = 0;
        while (i < cap_index) : (i += 1) {
            const cap = table.slotAtConst(i) orelse break;
            if (cap.root_cap_id == root_cap_id) return true;
        }
        return false;
    }

    fn debugVmObjectRootSeenBeforeDevice(
        self: *const KernelState,
        root_cap_id: u64,
        cap_index: usize,
    ) bool {
        var pidx: usize = 0;
        while (pidx < self.process_capacity) : (pidx += 1) {
            const table = self.vmObjectTableForProcessIndexConst(pidx);
            var i: usize = 0;
            while (i < table.len) : (i += 1) {
                const cap = table.slotAtConst(i) orelse break;
                if (cap.root_cap_id == root_cap_id) return true;
            }
        }
        const table = &self.vm_object_tables[process_count];
        var i: usize = 0;
        while (i < cap_index) : (i += 1) {
            const cap = table.slotAtConst(i) orelse break;
            if (cap.root_cap_id == root_cap_id) return true;
        }
        return false;
    }

    fn debugVmObjectUniquePagesForProcess(self: *const KernelState, process_index: usize) u64 {
        const table = self.vmObjectTableForProcessIndexConst(process_index);
        var pages: u64 = 0;
        var i: usize = 0;
        while (i < table.len) : (i += 1) {
            const cap = table.slotAtConst(i) orelse break;
            if (self.debugVmObjectRootSeenBeforeProcess(cap.root_cap_id, process_index, i)) continue;
            pages += cap.backing.page_count;
        }
        return pages;
    }

    fn debugVmObjectUniquePagesForDevice(self: *const KernelState) u64 {
        const table = &self.vm_object_tables[process_count];
        var pages: u64 = 0;
        var i: usize = 0;
        while (i < table.len) : (i += 1) {
            const cap = table.slotAtConst(i) orelse break;
            if (self.debugVmObjectRootSeenBeforeDevice(cap.root_cap_id, i)) continue;
            pages += cap.backing.page_count;
        }
        return pages;
    }

    fn debugVmObjectBackingPageSum(table: *const VmObjectCNode) u64 {
        var pages: u64 = 0;
        var i: usize = 0;
        while (i < table.len) : (i += 1) {
            const cap = table.slotAtConst(i) orelse break;
            pages += cap.backing.page_count;
        }
        return pages;
    }

    pub fn debugLogMemoryOwnership(
        self: *const KernelState,
        free_list: *const FreePageList,
        write: *const fn ([]const u8) void,
        print_number: *const fn (u64) void,
        where: []const u8,
    ) void {
        var page_caps_total: u64 = 0;
        var vm_caps_total: u64 = 0;
        var vm_cap_pages_total: u64 = 0;
        var vm_unique_pages_total: u64 = 0;
        var ipc_caps_total: u64 = 0;
        var active_total: u64 = 0;

        var pidx: usize = 0;
        while (pidx < self.process_capacity) : (pidx += 1) {
            const page_table = self.capTableForProcessIndexConst(pidx);
            const vm_table = self.vmObjectTableForProcessIndexConst(pidx);
            const ipc_table = self.ipcBufferTableForProcessIndexConst(pidx);
            page_caps_total += @intCast(page_table.len);
            vm_caps_total += @intCast(vm_table.len);
            vm_cap_pages_total += debugVmObjectBackingPageSum(vm_table);
            vm_unique_pages_total += self.debugVmObjectUniquePagesForProcess(pidx);
            ipc_caps_total += @intCast(ipc_table.len);
            if ((self.processDescriptorSlotConst(pidx) orelse continue).active) active_total += 1;
        }

        const device_page_table = &self.cap_tables[process_count];
        const device_vm_table = &self.vm_object_tables[process_count];
        const device_ipc_table = &self.ipc_buffer_tables[process_count];
        page_caps_total += @intCast(device_page_table.len);
        vm_caps_total += @intCast(device_vm_table.len);
        vm_cap_pages_total += debugVmObjectBackingPageSum(device_vm_table);
        vm_unique_pages_total += self.debugVmObjectUniquePagesForDevice();
        ipc_caps_total += @intCast(device_ipc_table.len);

        write("Kernel.mem_diag where=");
        write(where);
        debugWriteField(write, print_number, "free_pages", @intCast(free_list.len));
        debugWriteField(write, print_number, "free_ranges", @intCast(free_list.range_len));
        debugWriteField(write, print_number, "process_capacity", @intCast(self.process_capacity));
        debugWriteField(write, print_number, "active_processes", active_total);
        debugWriteField(write, print_number, "tracked_active", @intCast(self.active_process_count));
        debugWriteField(write, print_number, "page_caps", page_caps_total);
        debugWriteField(write, print_number, "page_cap_unique", pageCapRefUniqueCount());
        debugWriteField(write, print_number, "vm_caps", vm_caps_total);
        debugWriteField(write, print_number, "vm_cap_pages_sum", vm_cap_pages_total);
        debugWriteField(write, print_number, "vm_unique_pages", vm_unique_pages_total);
        debugWriteField(write, print_number, "vm_store_next", @intCast(vm_object_backing_page_store_next));
        debugWriteField(write, print_number, "vm_store_free_pages", vmObjectBackingFreePageCount());
        debugWriteField(write, print_number, "vm_store_free_ranges", @intCast(vm_object_backing_page_store_free_range_len));
        debugWriteField(write, print_number, "ipc_caps", ipc_caps_total);
        write("\n");

        pidx = 0;
        while (pidx < self.process_capacity) : (pidx += 1) {
            const page_table = self.capTableForProcessIndexConst(pidx);
            const vm_table = self.vmObjectTableForProcessIndexConst(pidx);
            const ipc_table = self.ipcBufferTableForProcessIndexConst(pidx);
            const vm_unique_pages = self.debugVmObjectUniquePagesForProcess(pidx);
            const desc = self.processDescriptorSlotConst(pidx) orelse continue;
            if (!desc.active and page_table.len == 0 and vm_table.len == 0 and ipc_table.len == 0) continue;

            write("Kernel.mem_diag.proc");
            debugWriteField(write, print_number, "idx", @intCast(pidx));
            debugWriteField(write, print_number, "active", if (desc.active) 1 else 0);
            write(" label=");
            write(desc.label);
            debugWriteField(write, print_number, "page_caps", @intCast(page_table.len));
            debugWriteField(write, print_number, "vm_caps", @intCast(vm_table.len));
            debugWriteField(write, print_number, "vm_cap_pages_sum", debugVmObjectBackingPageSum(vm_table));
            debugWriteField(write, print_number, "vm_unique_pages", vm_unique_pages);
            debugWriteField(write, print_number, "ipc_caps", @intCast(ipc_table.len));
            write("\n");
        }

        if (device_page_table.len != 0 or device_vm_table.len != 0 or device_ipc_table.len != 0) {
            write("Kernel.mem_diag.proc");
            debugWriteField(write, print_number, "idx", @intCast(process_count));
            debugWriteField(write, print_number, "active", 1);
            write(" label=Device0");
            debugWriteField(write, print_number, "page_caps", @intCast(device_page_table.len));
            debugWriteField(write, print_number, "vm_caps", @intCast(device_vm_table.len));
            debugWriteField(write, print_number, "vm_cap_pages_sum", debugVmObjectBackingPageSum(device_vm_table));
            debugWriteField(write, print_number, "vm_unique_pages", self.debugVmObjectUniquePagesForDevice());
            debugWriteField(write, print_number, "ipc_caps", @intCast(device_ipc_table.len));
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

    pub fn reclaimScannedExclusiveRootPage(
        self: *KernelState,
        owner: PrincipalId,
        paddr: u64,
        free_list: *FreePageList,
    ) KernelError!void {
        if (!free_list.canAppendPage(0, paddr)) return KernelError.TooManyFreeRanges;
        const table = self.getTable(owner);
        try table.removeExclusiveRootByPaddr(paddr);
        _ = self.removeDmaMappingsForPrincipalPaddr(owner, paddr);
        try device_capabilities.syncIommuForPrincipalPaddr(self, owner, paddr, .revoke);
        try free_list.appendPage(0, paddr);
    }

    pub fn releasePrincipalPageCaps(
        self: *KernelState,
        owner: PrincipalId,
        free_list: *FreePageList,
    ) void {
        const table = self.getTable(owner);
        const sync_ptes = self.hasActivePrincipal(owner);
        rebuildPageCapRefsExcluding(self, owner);
        var free_run_start: u64 = 0;
        var free_run_len: usize = 0;
        const flushFreeRun = struct {
            fn call(list: *FreePageList, start: *u64, len: *usize) void {
                if (len.* == 0) return;
                list.appendContiguousRange(0, start.*, len.*) catch {};
                start.* = 0;
                len.* = 0;
            }
        }.call;

        const inline_limit = @min(table.len, CNode.inline_caps);
        var index: usize = 0;
        while (index < inline_limit) : (index += 1) {
            self.releasePrincipalPageCap(owner, table.caps[index], sync_ptes, free_list, &free_run_start, &free_run_len, false);
        }

        var remaining = table.len - inline_limit;
        var chunk_index = table.overflow_head;
        while (remaining > 0 and chunk_index != CNode.invalid_chunk) {
            const chunk = &cap_chunk_pool[chunk_index];
            const chunk_limit = @min(remaining, CNode.chunk_caps);
            index = 0;
            while (index < chunk_limit) : (index += 1) {
                self.releasePrincipalPageCap(owner, chunk.caps[index], sync_ptes, free_list, &free_run_start, &free_run_len, false);
            }
            remaining -= chunk_limit;
            chunk_index = chunk.next;
        }
        flushFreeRun(free_list, &free_run_start, &free_run_len);
        table.resetStorageOnly();
    }

    fn releasePrincipalPageCap(
        self: *KernelState,
        owner: PrincipalId,
        cap: Capability,
        sync_ptes: bool,
        free_list: *FreePageList,
        free_run_start: *u64,
        free_run_len: *usize,
        update_ref_table: bool,
    ) void {
        const paddr = cap.paddr;
        if (cap.rights.dma) {
            if (self.removeDmaMappingsForPrincipalPaddr(owner, paddr)) {
                device_capabilities.syncIommuForPrincipalPaddr(self, owner, paddr, .revoke) catch {};
            }
        }
        if (sync_ptes) {
            if (self.pte_sync_hook) |hook| {
                callPteSyncHook(hook, self, owner, paddr);
            }
        }
        if (update_ref_table) trackPageCapRemoved(cap);
        if (pageCapRefCount(paddr) == 0) {
            const expected = free_run_start.* + @as(u64, @intCast(free_run_len.*)) * 4096;
            if (free_run_len.* != 0 and paddr == expected) {
                free_run_len.* += 1;
            } else {
                if (free_run_len.* != 0) {
                    free_list.appendContiguousRange(0, free_run_start.*, free_run_len.*) catch {};
                }
                free_run_start.* = paddr;
                free_run_len.* = 1;
            }
        }
    }

    pub fn installCap(
        self: *KernelState,
        owner: PrincipalId,
        paddr: u64,
        rights: Rights,
    ) KernelError!void {
        if (!self.hasActivePrincipal(owner)) return KernelError.InvalidState;
        if (self.getTableConst(owner).find(paddr) != null) return;
        const root_id = self.allocCapId();
        try self.getTable(owner).add(.{
            .paddr = paddr,
            .rights = rights,
            .cap_id = root_id,
            .root_cap_id = root_id,
            .parent_cap_id = 0,
        });
        if (self.pte_sync_hook) |hook| {
            callPteSyncHook(hook, self, owner, paddr);
        }
    }

    pub fn moveCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        paddr: u64,
        rights: Rights,
    ) KernelError!void {
        if (from == to) return KernelError.InvalidState;
        if (to == .Device0 and (rights.cpu_read or rights.cpu_write or !rights.dma)) return KernelError.InvalidState;
        if (!self.hasActivePrincipal(from) or !self.hasActivePrincipal(to)) return KernelError.InvalidState;

        const src = self.getTable(from);
        const src_cap = src.find(paddr) orelse return KernelError.CapabilityNotFound;
        if (!isRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;
        if (self.getTable(to).find(paddr) != null) return KernelError.InvalidState;

        var moved = src_cap.*;
        moved.rights = rights;
        _ = src.removeByPaddr(paddr);
        try self.getTable(to).add(moved);
        _ = self.removeDmaMappingsForPrincipalPaddr(from, paddr);
        try device_capabilities.syncIommuForPrincipalPaddr(self, from, paddr, .move_from);
        try device_capabilities.syncIommuForPrincipalPaddr(self, to, paddr, .move_to);
        if (self.pte_sync_hook) |hook| {
            callPteSyncHook(hook, self, from, paddr);
            callPteSyncHook(hook, self, to, paddr);
        }
    }

    pub fn grantCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        paddr: u64,
        rights: Rights,
    ) KernelError!void {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);

        const src_cap = self.getTableConst(from).find(paddr) orelse return KernelError.CapabilityNotFound;
        if (!src_cap.rights.grant) return KernelError.InvalidState;
        if (!isRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;
        if (self.getTable(to).find(paddr) != null) return KernelError.InvalidState;

        const child_id = self.allocCapId();
        try self.getTable(to).addAssumeFresh(.{
            .paddr = paddr,
            .rights = rights,
            .cap_id = child_id,
            .root_cap_id = src_cap.root_cap_id,
            .parent_cap_id = src_cap.cap_id,
        });
        try device_capabilities.syncIommuForPrincipalPaddr(self, to, paddr, if (rights.dma) .grant_dma else .grant_no_dma);
        if (self.pte_sync_hook) |hook| {
            if (!capability.principalHasMappedPaddr(to, paddr)) return;
            callPteSyncHook(hook, self, to, paddr);
        }
    }

    pub fn grantCapToFreshUnmappedProcess(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        paddr: u64,
        rights: Rights,
    ) KernelError!void {
        // For spawn-time bootstrap pages the child has just been created and the
        // target paddr is not mapped yet, so non-DMA grants do not need IOMMU or
        // PTE synchronization scans.
        if (rights.dma) return KernelError.InvalidState;
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);

        const src_cap = self.getTableConst(from).find(paddr) orelse return KernelError.CapabilityNotFound;
        if (!src_cap.rights.grant) return KernelError.InvalidState;
        if (!isRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;
        if (self.getTable(to).find(paddr) != null) return KernelError.InvalidState;

        const child_id = self.allocCapId();
        try self.getTable(to).addAssumeFresh(.{
            .paddr = paddr,
            .rights = rights,
            .cap_id = child_id,
            .root_cap_id = src_cap.root_cap_id,
            .parent_cap_id = src_cap.cap_id,
        });
    }

    pub fn deriveCapForSharedAddressSpace(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        paddr: u64,
        rights: Rights,
    ) KernelError!void {
        if (rights.dma) return KernelError.InvalidState;
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);

        const src_cap = self.getTableConst(from).find(paddr) orelse return KernelError.CapabilityNotFound;
        if (!isRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;
        if (self.getTableConst(to).find(paddr)) |dst_cap| {
            if (isRightsSubset(rights, dst_cap.rights)) return;
            var upgraded = dst_cap.*;
            upgraded.rights.cpu_read = upgraded.rights.cpu_read or rights.cpu_read;
            upgraded.rights.cpu_write = upgraded.rights.cpu_write or rights.cpu_write;
            upgraded.rights.dma = upgraded.rights.dma or rights.dma;
            upgraded.rights.grant = upgraded.rights.grant or rights.grant;
            if (!isRightsSubset(upgraded.rights, src_cap.rights)) return KernelError.InvalidState;
            _ = self.getTable(to).removeByPaddr(paddr);
            try self.getTable(to).addAssumeFresh(upgraded);
            return;
        }

        const child_id = self.allocCapId();
        try self.getTable(to).addAssumeFresh(.{
            .paddr = paddr,
            .rights = rights,
            .cap_id = child_id,
            .root_cap_id = src_cap.root_cap_id,
            .parent_cap_id = src_cap.cap_id,
        });
    }

    pub fn grantCapsBatch(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        paddrs: []const u64,
        rights: Rights,
    ) KernelError!void {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);
        if (paddrs.len == 0) return KernelError.InvalidState;
        var i: usize = 0;
        while (i < paddrs.len) : (i += 1) {
            const paddr = paddrs[i];
            const src_cap = self.getTableConst(from).find(paddr) orelse return KernelError.CapabilityNotFound;
            if (!src_cap.rights.grant) return KernelError.InvalidState;
            if (!isRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;
            if (self.getTable(to).find(paddr) != null) return KernelError.InvalidState;
        }
        const saved_audit_hook = self.iommu_audit_hook;
        self.iommu_audit_hook = null;
        defer {
            self.iommu_audit_hook = saved_audit_hook;
        }

        i = 0;
        while (i < paddrs.len) : (i += 1) {
            const paddr = paddrs[i];
            const src_cap = self.getTableConst(from).find(paddr).?;
            const child_id = self.allocCapId();
            try self.getTable(to).add(.{
                .paddr = paddr,
                .rights = rights,
                .cap_id = child_id,
                .root_cap_id = src_cap.root_cap_id,
                .parent_cap_id = src_cap.cap_id,
            });
            try device_capabilities.syncIommuForPrincipalPaddr(self, to, paddr, if (rights.dma) .grant_dma else .grant_no_dma);
        }
    }

    pub fn grantCapOnEndpoint(
        self: *KernelState,
        from: PrincipalId,
        endpoint_id: u64,
        paddr: u64,
        rights: Rights,
    ) KernelError!void {
        try self.requireActiveProcess(from);
        const target = self.endpointTargetFor(from, endpoint_id) orelse return KernelError.EndpointNotFound;
        try self.grantCap(from, target, paddr, rights);
    }

    pub fn grantCapsBatchOnEndpoint(
        self: *KernelState,
        from: PrincipalId,
        endpoint_id: u64,
        paddrs: []const u64,
        rights: Rights,
    ) KernelError!void {
        try self.requireActiveProcess(from);
        const target = self.endpointTargetFor(from, endpoint_id) orelse return KernelError.EndpointNotFound;
        try self.grantCapsBatch(from, target, paddrs, rights);
    }

    pub fn sendCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        paddr: u64,
    ) KernelError!void {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);

        const src_cap = self.getTableConst(from).find(paddr) orelse return KernelError.CapabilityNotFound;
        try self.moveCap(from, to, paddr, src_cap.rights);
    }

    pub fn sendCapOnEndpoint(
        self: *KernelState,
        from: PrincipalId,
        endpoint_id: u64,
        paddr: u64,
    ) KernelError!void {
        try self.requireActiveProcess(from);
        const target = self.endpointTargetFor(from, endpoint_id) orelse return KernelError.EndpointNotFound;
        const src_cap = self.getTableConst(from).find(paddr) orelse return KernelError.CapabilityNotFound;
        try self.capMailboxForPrincipal(target).push(.{
            .transfer_id = self.allocTransferId(),
            .sender = from,
            .endpoint_id = endpoint_id,
            .paddr = paddr,
            .rights = src_cap.rights,
            .retain_sender = false,
        });
    }

    pub fn shareCapOnEndpoint(
        self: *KernelState,
        from: PrincipalId,
        endpoint_id: u64,
        paddr: u64,
    ) KernelError!void {
        try self.requireActiveProcess(from);
        const target = self.endpointTargetFor(from, endpoint_id) orelse return KernelError.EndpointNotFound;
        const src_cap = self.getTableConst(from).find(paddr) orelse return KernelError.CapabilityNotFound;
        if (!src_cap.rights.grant) return KernelError.InvalidState;
        try self.capMailboxForPrincipal(target).push(.{
            .transfer_id = self.allocTransferId(),
            .sender = from,
            .endpoint_id = endpoint_id,
            .paddr = paddr,
            .rights = src_cap.rights,
            .retain_sender = true,
        });
    }

    pub fn recvCap(self: *KernelState, receiver: PrincipalId) KernelError!u64 {
        try self.requireActiveProcess(receiver);
        const pending_slot = self.pendingPageTransferForPrincipal(receiver);
        if (pending_slot.*) |pending| {
            return pending.transfer_id;
        }
        const received = self.capMailboxForPrincipal(receiver).pop() orelse return KernelError.MailboxEmpty;
        pending_slot.* = received;
        return received.transfer_id;
    }

    pub fn recvIpcBufferTransfer(self: *KernelState, receiver: PrincipalId) KernelError!u64 {
        try self.requireActiveProcess(receiver);
        const pending_slot = self.pendingIpcBufferTransferForPrincipal(receiver);
        if (pending_slot.*) |pending| {
            return pending.transfer_id;
        }
        const received = self.ipcBufferMailboxForPrincipal(receiver).pop() orelse return KernelError.MailboxEmpty;
        pending_slot.* = received;
        return received.transfer_id;
    }

    pub fn recvAnyCapTransfer(self: *KernelState, receiver: PrincipalId) KernelError!u64 {
        return self.recvCap(receiver) catch |page_err| switch (page_err) {
            KernelError.MailboxEmpty => self.recvIpcBufferTransfer(receiver),
            else => page_err,
        };
    }

    pub fn acceptCapTransfer(self: *KernelState, receiver: PrincipalId, transfer_id: u64) KernelError!u64 {
        try self.requireActiveProcess(receiver);
        const pending_slot = self.pendingPageTransferForPrincipal(receiver);
        const pending = pending_slot.* orelse return KernelError.MailboxEmpty;
        if (pending.transfer_id != transfer_id) return KernelError.InvalidState;
        if (pending.retain_sender) {
            try self.grantCap(pending.sender, receiver, pending.paddr, pending.rights);
        } else {
            try self.moveCap(pending.sender, receiver, pending.paddr, pending.rights);
        }
        pending_slot.* = null;
        return pending.paddr;
    }

    pub fn acceptIpcBufferTransfer(self: *KernelState, receiver: PrincipalId, transfer_id: u64) KernelError!u64 {
        try self.requireActiveProcess(receiver);
        const pending_slot = self.pendingIpcBufferTransferForPrincipal(receiver);
        const pending = pending_slot.* orelse return KernelError.MailboxEmpty;
        if (pending.transfer_id != transfer_id) return KernelError.InvalidState;
        const child_id = if (pending.retain_sender)
            try self.grantIpcBufferCap(pending.sender, receiver, pending.cap_id, pending.rights)
        else
            try self.moveIpcBufferCap(pending.sender, receiver, pending.cap_id, pending.rights);
        pending_slot.* = null;
        return child_id;
    }

    pub fn revokeCapTree(
        self: *KernelState,
        owner: PrincipalId,
        paddr: u64,
    ) KernelError!void {
        try self.requireActiveProcess(owner);
        const start_cap = self.getTableConst(owner).find(paddr) orelse return KernelError.CapabilityNotFound;
        const start_id = start_cap.cap_id;

        const queue = &self.revoke_queue;
        var queue_len: usize = 0;
        var queue_head: usize = 0;
        queue[0] = start_id;
        queue_len = 1;

        const subtree = &self.revoke_subtree;
        var subtree_len: usize = 0;

        while (queue_head < queue_len) : (queue_head += 1) {
            const current_id = queue[queue_head];
            var already_in_subtree = false;
            var chk: usize = 0;
            while (chk < subtree_len) : (chk += 1) {
                if (subtree[chk] == current_id) {
                    already_in_subtree = true;
                    break;
                }
            }
            if (already_in_subtree) continue;
            if (subtree_len >= self.revoke_subtree.len) return KernelError.RevokeOverflow;
            subtree[subtree_len] = current_id;
            subtree_len += 1;

            var pidx: usize = 0;
            while (pidx < self.process_capacity) : (pidx += 1) {
                const table = self.capTableForProcessIndexConst(pidx);
                var i: usize = 0;
                while (i < table.len) : (i += 1) {
                    const cap = table.get(i) orelse break;
                    if (cap.parent_cap_id != current_id) continue;
                    var known = false;
                    var q: usize = 0;
                    while (q < queue_len) : (q += 1) {
                        if (queue[q] == cap.cap_id) {
                            known = true;
                            break;
                        }
                    }
                    if (known) continue;
                    if (queue_len >= self.revoke_queue.len) return KernelError.RevokeOverflow;
                    queue[queue_len] = cap.cap_id;
                    queue_len += 1;
                }
            }
        }

        var s: usize = 0;
        while (s < subtree_len) : (s += 1) {
            const cap_id = subtree[s];
            var pidx: usize = 0;
            while (pidx < self.process_capacity) : (pidx += 1) {
                const table = self.capTableForProcessIndex(pidx);
                const cap = table.findByCapId(cap_id) orelse continue;
                const removed_paddr = cap.paddr;
                _ = table.removeByCapId(cap_id);
                const principal = processPrincipal(pidx);
                _ = self.removeDmaMappingsForPrincipalPaddr(principal, removed_paddr);
                try device_capabilities.syncIommuForPrincipalPaddr(self, principal, removed_paddr, .revoke);
                if (self.pte_sync_hook) |hook| {
                    callPteSyncHook(hook, self, principal, removed_paddr);
                }
                break;
            }
        }
    }
};

fn containsCapId(ids: []const u64, target: u64) bool {
    for (ids) |id| {
        if (id == target) return true;
    }
    return false;
}
