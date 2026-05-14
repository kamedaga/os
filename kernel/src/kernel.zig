const std = @import("std");
const builtin = @import("builtin");
const capability = @import("capability.zig");
const abi_root = @import("kernel_abi_root");
const cap_transfer_abi = abi_root.cap_transfer_abi;
const dma_mapping_manager = @import("dma_mapping_manager.zig");
pub const device_capabilities = @import("device_capabilities.zig");
pub const initial_process_count: usize = 8;
pub const process_count: usize = 32;
pub const max_thread_slots: usize = 32;
pub const device_count: usize = 1;
pub const principal_count: usize = process_count + device_count;

comptime {
    if (principal_count > std.math.maxInt(u8)) @compileError("principal_count must fit in u8");
}

fn PrincipalIdType(comptime named_process_slots: usize) type {
    var fields: [named_process_slots + device_count]std.builtin.Type.EnumField = undefined;
    inline for (0..named_process_slots) |i| {
        fields[i] = .{
            .name = std.fmt.comptimePrint("Process{}", .{i}),
            .value = i,
        };
    }
    fields[named_process_slots] = .{
        .name = "Device0",
        .value = process_count,
    };

    return @Type(.{ .@"enum" = .{
        .tag_type = u8,
        .fields = &fields,
        .decls = &.{},
        .is_exhaustive = false,
    } });
}

pub const PrincipalId = PrincipalIdType(process_count);
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
    if (index >= process_count) return null;
    return @enumFromInt(index);
}

pub fn processIndexFromPrincipal(principal: PrincipalId) ?usize {
    const index: usize = @intFromEnum(principal);
    if (index >= process_count) return null;
    return index;
}

pub fn principalLabel(principal: PrincipalId) []const u8 {
    if (processIndexFromPrincipal(principal)) |index| {
        return process_labels[index];
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
    ExecImageCapabilityNotFound,
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

pub const CNode = struct {
    pub const inline_caps = 512;
    pub const chunk_caps = 512;
    pub const chunk_pool_count = 512;
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
        self.len += 1;
    }

    pub fn reset(self: *CNode) void {
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

    fn removeIndex(self: *CNode, index: usize) void {
        if (index >= self.len) return;
        const last_index = self.len - 1;
        const removed = (self.slotAtConst(index) orelse return).*;
        self.removePaddrIndex(removed.paddr);
        if (index != last_index) {
            const last = (self.slotAtConst(last_index) orelse return).*;
            self.removePaddrIndex(last.paddr);
            (self.slotAt(index) orelse return).* = last;
            self.insertPaddrIndex(last.paddr, index);
        }
        self.len -= 1;
        if (self.slotAt(self.len)) |slot| slot.* = .{};
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
};

const CapChunk = struct {
    used: bool = false,
    next: u16 = CNode.invalid_chunk,
    caps: [CNode.chunk_caps]Capability = [_]Capability{.{}} ** CNode.chunk_caps,
};

var cap_chunk_pool: [CNode.chunk_pool_count]CapChunk = [_]CapChunk{.{}} ** CNode.chunk_pool_count;

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
    const max_items = 8;

    items: [max_items]PendingCapTransfer = undefined,
    len: usize = 0,

    pub fn push(self: *CapMailbox, transfer: PendingCapTransfer) KernelError!void {
        if (self.len >= self.items.len) return KernelError.TableFull;
        self.items[self.len] = transfer;
        self.len += 1;
    }

    pub fn pop(self: *CapMailbox) ?PendingCapTransfer {
        if (self.len == 0) return null;
        const transfer = self.items[0];
        var i: usize = 1;
        while (i < self.len) : (i += 1) {
            self.items[i - 1] = self.items[i];
        }
        self.len -= 1;
        return transfer;
    }
};

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
    const max_items = 8;

    items: [max_items]PendingIpcBufferTransfer = undefined,
    len: usize = 0,

    pub fn push(self: *IpcBufferMailbox, transfer: PendingIpcBufferTransfer) KernelError!void {
        if (self.len >= self.items.len) return KernelError.TableFull;
        self.items[self.len] = transfer;
        self.len += 1;
    }

    pub fn pop(self: *IpcBufferMailbox) ?PendingIpcBufferTransfer {
        if (self.len == 0) return null;
        const transfer = self.items[0];
        var i: usize = 1;
        while (i < self.len) : (i += 1) self.items[i - 1] = self.items[i];
        self.len -= 1;
        return transfer;
    }
};

pub const max_image_backing_pages: usize = 4096;
pub const max_image_backing_store_pages: usize = 65536;

var image_backing_page_store: [max_image_backing_store_pages]u64 = [_]u64{0} ** max_image_backing_store_pages;
var image_backing_page_store_next: usize = 0;

fn allocImageBackingPageStore(page_paddrs: []const u64) ?u32 {
    if (page_paddrs.len == 0 or page_paddrs.len > max_image_backing_pages) return null;
    if (image_backing_page_store_next + page_paddrs.len > image_backing_page_store.len) return null;
    const start = image_backing_page_store_next;
    for (page_paddrs, 0..) |paddr, i| {
        if ((paddr & 0xFFF) != 0) return null;
        image_backing_page_store[start + i] = paddr;
    }
    image_backing_page_store_next += page_paddrs.len;
    return @intCast(start);
}

pub const VmObjectRights = packed struct(u32) {
    read: bool = false,
    write: bool = false,
    map: bool = false,
    grant: bool = false,
    _reserved: u28 = 0,
};

pub const ExecImageRights = packed struct(u32) {
    exec: bool = false,
    grant: bool = false,
    _reserved: u30 = 0,
};

pub const ImageBacking = struct {
    page_offset_bytes: u16 = 0,
    page_count: u16 = 0,
    page_store_start: u32 = 0,
    size_bytes: u64 = 0,

    fn init(page_paddrs: []const u64, page_offset_bytes: u16, size_bytes: u64) ?ImageBacking {
        if (page_paddrs.len == 0 or page_paddrs.len > max_image_backing_pages) return null;
        if (size_bytes == 0) return null;
        const first_page_bytes = @as(u64, 4096 - page_offset_bytes);
        const total_capacity = first_page_bytes + (@as(u64, @intCast(page_paddrs.len - 1)) * 4096);
        if (total_capacity < size_bytes) return null;

        const page_store_start = allocImageBackingPageStore(page_paddrs) orelse return null;
        return ImageBacking{
            .page_offset_bytes = page_offset_bytes,
            .page_count = @intCast(page_paddrs.len),
            .page_store_start = page_store_start,
            .size_bytes = size_bytes,
        };
    }

    pub fn pagePaddr(self: *const ImageBacking, page_index: usize) ?u64 {
        if (page_index >= self.page_count) return null;
        const store_index = @as(usize, self.page_store_start) + page_index;
        if (store_index >= image_backing_page_store.len) return null;
        const paddr = image_backing_page_store[store_index];
        if ((paddr & 0xFFF) != 0) return null;
        return paddr;
    }

    fn slice(self: *const ImageBacking, offset_bytes: u64, size_bytes: u64) ?ImageBacking {
        if (size_bytes == 0) return null;
        const end_bytes, const overflow = @addWithOverflow(offset_bytes, size_bytes);
        if (overflow != 0 or end_bytes > self.size_bytes) return null;

        const absolute_start = @as(u64, self.page_offset_bytes) + offset_bytes;
        const start_page_index: usize = @intCast(absolute_start / 4096);
        const start_page_offset: u16 = @intCast(absolute_start % 4096);
        if (start_page_index >= self.page_count) return null;

        const first_page_bytes = @as(u64, 4096 - start_page_offset);
        const remaining_after_first = if (size_bytes > first_page_bytes) size_bytes - first_page_bytes else 0;
        const extra_pages: usize = if (remaining_after_first == 0) 0 else @intCast((remaining_after_first + 4095) / 4096);
        const page_count = 1 + extra_pages;
        if (start_page_index + page_count > self.page_count) return null;

        return .{
            .page_offset_bytes = start_page_offset,
            .page_count = @intCast(page_count),
            .page_store_start = self.page_store_start + @as(u32, @intCast(start_page_index)),
            .size_bytes = size_bytes,
        };
    }
};

pub const VmObjectCapability = struct {
    backing: ImageBacking,
    rights: VmObjectRights,
    cap_id: u64,
    root_cap_id: u64,
    parent_cap_id: u64,
};

pub const ExecImageCapability = struct {
    backing: ImageBacking,
    rights: ExecImageRights,
    cap_id: u64,
    root_cap_id: u64,
    parent_cap_id: u64,
};

pub const VmObjectCNode = struct {
    pub const max_caps = 64;

    caps: [max_caps]VmObjectCapability = undefined,
    len: usize = 0,

    pub fn add(self: *VmObjectCNode, cap: VmObjectCapability) KernelError!void {
        if (self.findByCapId(cap.cap_id) != null) return KernelError.InvalidState;
        if (self.len >= self.caps.len) return KernelError.TableFull;
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn findByCapId(self: *const VmObjectCNode, cap_id: u64) ?*const VmObjectCapability {
        if (self.findIndexByCapId(cap_id)) |index| return &self.caps[index];
        return null;
    }

    fn findIndexByCapId(self: *const VmObjectCNode, cap_id: u64) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            if (self.caps[i].cap_id == cap_id) return i;
        }
        return null;
    }
};

pub const ExecImageCNode = struct {
    pub const max_caps = 64;

    caps: [max_caps]ExecImageCapability = undefined,
    len: usize = 0,

    pub fn add(self: *ExecImageCNode, cap: ExecImageCapability) KernelError!void {
        if (self.findByCapId(cap.cap_id) != null) return KernelError.InvalidState;
        if (self.len >= self.caps.len) return KernelError.TableFull;
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn findByCapId(self: *const ExecImageCNode, cap_id: u64) ?*const ExecImageCapability {
        if (self.findIndexByCapId(cap_id)) |index| return &self.caps[index];
        return null;
    }

    fn findIndexByCapId(self: *const ExecImageCNode, cap_id: u64) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            if (self.caps[i].cap_id == cap_id) return i;
        }
        return null;
    }
};

pub const RegionFreeRange = struct {
    region_id: u64,
    len: usize,
    physical_start: u64,
};

pub const FreePageList = struct {
    pub const max_ranges = 256;

    len: usize = 0,
    ranges: [max_ranges]RegionFreeRange = undefined,
    range_len: usize = 0,

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
        if (self.range_len > 0) {
            const last = &self.ranges[self.range_len - 1];
            const expected_next = last.physical_start + (@as(u64, last.len) * 4096);
            if (last.region_id == region_id and paddr == expected_next) {
                self.len += 1;
                last.len += 1;
                return;
            }
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

    pub fn canAppendPage(self: *const FreePageList, region_id: u64, paddr: u64) bool {
        if (self.range_len > 0) {
            const last = &self.ranges[self.range_len - 1];
            const expected_next = last.physical_start + (@as(u64, last.len) * 4096);
            if (last.region_id == region_id and paddr == expected_next) return true;
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

pub const DebugAllocPageStage = enum(u8) {
    after_pop,
    after_memset,
    after_cap_add,
};

pub const OwnershipView = union(enum) {
    owner: PrincipalId,
    shared: void,
    none: void,
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

pub const KernelState = struct {
    pub const max_regions = 256;
    const max_total_caps = 65536;

    regions: [max_regions]Region = undefined,
    region_len: usize = 0,
    process_descriptors: [process_count]ProcessDescriptor = [_]ProcessDescriptor{.{}} ** process_count,
    active_process_count: usize = 0,
    cap_tables: [principal_count]CNode = [_]CNode{.{}} ** principal_count,
    endpoint_tables: [principal_count]EndpointCNode = [_]EndpointCNode{.{}} ** principal_count,
    published_service_endpoints: PublishedEndpointTable = .{},
    endpoint_generation: u64 = 0,
    cap_mailboxes: [principal_count]CapMailbox = [_]CapMailbox{.{}} ** principal_count,
    pending_page_transfers: [principal_count]?PendingCapTransfer = [_]?PendingCapTransfer{null} ** principal_count,
    ipc_buffer_tables: [principal_count]IpcBufferCNode = [_]IpcBufferCNode{.{}} ** principal_count,
    ipc_buffer_mailboxes: [principal_count]IpcBufferMailbox = [_]IpcBufferMailbox{.{}} ** principal_count,
    pending_ipc_buffer_transfers: [principal_count]?PendingIpcBufferTransfer = [_]?PendingIpcBufferTransfer{null} ** principal_count,
    vm_object_tables: [principal_count]VmObjectCNode = [_]VmObjectCNode{.{}} ** principal_count,
    exec_image_tables: [principal_count]ExecImageCNode = [_]ExecImageCNode{.{}} ** principal_count,
    pte_sync_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, paddr: u64) void = null,
    iommu_audit_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, paddr: u64, mapped: bool, reason: IommuSyncReason) void = null,
    debug_alloc_page_hook: ?*const fn (state: *const KernelState, requester: PrincipalId, stage: DebugAllocPageStage, paddr: u64) void = null,
    debug_process_lifecycle_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, reason: DebugProcessLifecycleReason) void = null,
    revoke_queue: [max_total_caps]u64 = undefined,
    revoke_subtree: [max_total_caps]u64 = undefined,
    dma_restore: [max_total_caps]DmaRestoreEntry = [_]DmaRestoreEntry{.{}} ** max_total_caps,
    dma_mappings: dma_mapping_manager.DmaMappingTable = .{},
    dma_device_domains: dma_mapping_manager.DeviceDomainTable = .{},
    iommu_caps: device_capabilities.IommuCapabilityTable = .{},
    queue_caps: device_capabilities.QueueCapabilityTable = .{},
    command_caps: device_capabilities.CommandCapabilityTable = .{},
    iommu: IommuNoCapDriverState = .{},
    next_cap_id: u64 = 1,
    next_transfer_id: u64 = cap_transfer_abi.transfer_id_min,

    fn allocCapId(self: *KernelState) u64 {
        const id = self.next_cap_id;
        self.next_cap_id +%= 1;
        return id;
    }

    fn allocTransferId(self: *KernelState) u64 {
        var id = self.next_transfer_id;
        self.next_transfer_id +%= 1;
        if (id < cap_transfer_abi.transfer_id_min) {
            id = cap_transfer_abi.transfer_id_min;
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
        const owner_raw: u8 = @intCast(@intFromEnum(principal));
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

    pub fn removeDmaMappingsForPrincipalPaddr(self: *KernelState, principal: PrincipalId, paddr: u64) void {
        const owner_raw: u8 = @intCast(@intFromEnum(principal));
        var i: usize = 0;
        while (i < self.dma_mappings.entries.len) : (i += 1) {
            const mapping = self.dma_mappings.entries[i];
            if (!mapping.valid) continue;
            if (mapping.owner_principal_raw != owner_raw) continue;
            if (!dmaMappingContainsPage(mapping, paddr)) continue;
            self.dma_mappings.entries[i] = .{};
        }
    }

    pub fn removeDmaMappingsForPrincipalDevice(self: *KernelState, principal: PrincipalId, device: DmaDeviceId) void {
        const owner_raw: u8 = @intCast(@intFromEnum(principal));
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

    fn isExecImageRightsSubset(child: ExecImageRights, parent: ExecImageRights) bool {
        const child_bits: u32 = @bitCast(child);
        const parent_bits: u32 = @bitCast(parent);
        return (child_bits & ~parent_bits) == 0;
    }

    fn isIpcBufferRightsSubset(child: IpcBufferRights, parent: IpcBufferRights) bool {
        const child_bits: u32 = @bitCast(child);
        const parent_bits: u32 = @bitCast(parent);
        return (child_bits & ~parent_bits) == 0;
    }

    fn processPrincipal(index: usize) PrincipalId {
        return processPrincipalFromIndex(index) orelse unreachable;
    }

    pub fn isActiveProcess(self: *const KernelState, principal: PrincipalId) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        return self.process_descriptors[index].active;
    }

    pub fn processDescriptor(self: *const KernelState, principal: PrincipalId) ?*const ProcessDescriptor {
        const index = processIndexFromPrincipal(principal) orelse return null;
        if (!self.process_descriptors[index].active) return null;
        return &self.process_descriptors[index];
    }

    pub fn processStatus(self: *const KernelState, principal: PrincipalId) ProcessStatus {
        const index = processIndexFromPrincipal(principal) orelse return .{};
        const desc = self.process_descriptors[index];
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
            return self.process_descriptors[index].active;
        }
        return principal == .Device0;
    }

    pub fn requireActiveProcess(self: *const KernelState, principal: PrincipalId) KernelError!void {
        const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
        if (!self.process_descriptors[index].active) return KernelError.InvalidState;
    }

    fn principalStorageIndex(self: *const KernelState, principal: PrincipalId) usize {
        if (processIndexFromPrincipal(principal)) |index| {
            std.debug.assert(self.process_descriptors[index].active);
            return index;
        }
        std.debug.assert(principal == .Device0);
        return @intFromEnum(principal);
    }

    fn clearPrincipalTablesForReuse(self: *KernelState, index: usize) void {
        self.cap_tables[index].reset();
        self.endpoint_tables[index] = .{};
        self.cap_mailboxes[index] = .{};
        self.pending_page_transfers[index] = null;
        self.ipc_buffer_tables[index] = .{};
        self.ipc_buffer_mailboxes[index] = .{};
        self.pending_ipc_buffer_transfers[index] = null;
        self.vm_object_tables[index] = .{};
        self.exec_image_tables[index] = .{};
    }

    pub fn createProcessDescriptor(self: *KernelState, label: []const u8) ?PrincipalId {
        var i: usize = 0;
        while (i < self.process_descriptors.len) : (i += 1) {
            if (self.process_descriptors[i].active) continue;
            const principal = processPrincipal(i);
            self.clearPrincipalTablesForReuse(i);
            self.process_descriptors[i] = .{
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

    pub fn ensureProcessDescriptor(self: *KernelState, principal: PrincipalId, label: []const u8) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        if (self.process_descriptors[index].active) {
            self.process_descriptors[index].label = label;
            return true;
        }
        self.clearPrincipalTablesForReuse(index);
        self.process_descriptors[index] = .{
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
        if (!self.process_descriptors[index].active) return KernelError.InvalidState;
        self.process_descriptors[index].bootstrap_owner = enabled;
    }

    pub fn markProcessBuilderSuspended(self: *KernelState, principal: PrincipalId, owner: PrincipalId) KernelError!void {
        const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
        if (!self.process_descriptors[index].active) return KernelError.InvalidState;
        try self.requireActiveProcess(owner);
        self.process_descriptors[index].process_builder_owner = owner;
        self.process_descriptors[index].process_builder_suspended = true;
    }

    pub fn processBuilderOwnerMatches(self: *const KernelState, principal: PrincipalId, owner: PrincipalId) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        const desc = self.process_descriptors[index];
        if (!desc.active or !desc.process_builder_suspended) return false;
        return desc.process_builder_owner == owner;
    }

    pub fn clearProcessBuilderSuspended(self: *KernelState, principal: PrincipalId) KernelError!void {
        const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
        if (!self.process_descriptors[index].active) return KernelError.InvalidState;
        self.process_descriptors[index].process_builder_owner = null;
        self.process_descriptors[index].process_builder_suspended = false;
    }

    pub fn setAbiTrapDelegate(
        self: *KernelState,
        principal: PrincipalId,
        endpoint_id: u64,
        flavor: u32,
        request_page_va: u64,
    ) KernelError!void {
        const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
        if (!self.process_descriptors[index].active) return KernelError.InvalidState;
        if (self.endpointTargetFor(principal, endpoint_id) == null) return KernelError.EndpointNotFound;
        if (request_page_va == 0 or (request_page_va & 0xFFF) != 0 or !capability.isUserCanonicalVa(request_page_va)) return KernelError.InvalidState;
        self.process_descriptors[index].abi_trap_delegate_endpoint_id = endpoint_id;
        self.process_descriptors[index].abi_trap_delegate_flavor = flavor;
        self.process_descriptors[index].abi_trap_request_page_va = request_page_va;
    }

    pub fn clearAbiTrapDelegate(self: *KernelState, principal: PrincipalId) KernelError!void {
        const index = processIndexFromPrincipal(principal) orelse return KernelError.InvalidState;
        if (!self.process_descriptors[index].active) return KernelError.InvalidState;
        self.process_descriptors[index].abi_trap_delegate_endpoint_id = 0;
        self.process_descriptors[index].abi_trap_delegate_flavor = 0;
        self.process_descriptors[index].abi_trap_request_page_va = 0;
    }

    pub fn abiTrapDelegateFor(self: *const KernelState, principal: PrincipalId) ?AbiTrapDelegate {
        const index = processIndexFromPrincipal(principal) orelse return null;
        const desc = self.process_descriptors[index];
        if (!desc.active or desc.abi_trap_delegate_endpoint_id == 0) return null;
        return .{
            .endpoint_id = desc.abi_trap_delegate_endpoint_id,
            .flavor = desc.abi_trap_delegate_flavor,
            .request_page_va = desc.abi_trap_request_page_va,
        };
    }

    pub fn markProcessFaulted(self: *KernelState, principal: PrincipalId, fault_vector: u8) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        if (!self.process_descriptors[index].active) return false;
        self.process_descriptors[index].active = false;
        self.process_descriptors[index].bootstrap_owner = false;
        self.process_descriptors[index].process_builder_owner = null;
        self.process_descriptors[index].process_builder_suspended = false;
        self.process_descriptors[index].abi_trap_delegate_endpoint_id = 0;
        self.process_descriptors[index].abi_trap_delegate_flavor = 0;
        self.process_descriptors[index].abi_trap_request_page_va = 0;
        self.process_descriptors[index].faulted = true;
        self.process_descriptors[index].fault_vector = fault_vector;
        if (self.active_process_count > 0) self.active_process_count -= 1;
        if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .fault);
        return true;
    }

    pub fn markProcessExited(self: *KernelState, principal: PrincipalId) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        if (!self.process_descriptors[index].active) return false;
        self.process_descriptors[index].active = false;
        self.process_descriptors[index].bootstrap_owner = false;
        self.process_descriptors[index].process_builder_owner = null;
        self.process_descriptors[index].process_builder_suspended = false;
        self.process_descriptors[index].abi_trap_delegate_endpoint_id = 0;
        self.process_descriptors[index].abi_trap_delegate_flavor = 0;
        self.process_descriptors[index].abi_trap_request_page_va = 0;
        self.process_descriptors[index].faulted = false;
        self.process_descriptors[index].fault_vector = 0;
        if (self.active_process_count > 0) self.active_process_count -= 1;
        if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .exit);
        return true;
    }

    pub fn removeProcessDescriptor(self: *KernelState, principal: PrincipalId) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        if (!self.process_descriptors[index].active) return false;
        self.process_descriptors[index] = .{};
        if (self.active_process_count > 0) self.active_process_count -= 1;
        if (self.debug_process_lifecycle_hook) |hook| hook(self, principal, .remove);
        return true;
    }

    fn clearPrincipalState(self: *KernelState) void {
        var i: usize = 0;
        while (i < principal_count) : (i += 1) {
            self.cap_tables[i].reset();
            self.endpoint_tables[i] = .{};
            self.cap_mailboxes[i] = .{};
            self.pending_page_transfers[i] = null;
            self.ipc_buffer_tables[i] = .{};
            self.ipc_buffer_mailboxes[i] = .{};
            self.pending_ipc_buffer_transfers[i] = null;
            self.vm_object_tables[i] = .{};
            self.exec_image_tables[i] = .{};
        }
        self.published_service_endpoints = .{};
        i = 0;
        while (i < self.process_descriptors.len) : (i += 1) {
            self.process_descriptors[i] = .{};
        }
        self.active_process_count = 0;
    }

    fn initPrincipalState(self: *KernelState) void {
        self.clearPrincipalState();
        var i: usize = 0;
        while (i < initial_process_count) : (i += 1) {
            const principal = processPrincipal(i);
            self.process_descriptors[i] = .{
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
        self.next_cap_id = 1;
        self.regions[0] = .{
            .id = 0,
        };
        self.region_len = 1;
        self.initPrincipalState();
        const root_id = self.allocCapId();
        self.cap_tables[@intFromEnum(default_process_principal)].add(.{
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
        return &self.cap_tables[self.principalStorageIndex(principal)];
    }

    pub fn getTableConst(self: *const KernelState, principal: PrincipalId) *const CNode {
        return &self.cap_tables[self.principalStorageIndex(principal)];
    }

    pub fn getEndpointTable(self: *KernelState, principal: PrincipalId) *EndpointCNode {
        return &self.endpoint_tables[self.principalStorageIndex(principal)];
    }

    pub fn getEndpointTableConst(self: *const KernelState, principal: PrincipalId) *const EndpointCNode {
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
        while (i < self.process_descriptors.len) : (i += 1) {
            const desc = self.process_descriptors[i];
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
        return &self.vm_object_tables[self.principalStorageIndex(principal)];
    }

    pub fn getVmObjectTableConst(self: *const KernelState, principal: PrincipalId) *const VmObjectCNode {
        return &self.vm_object_tables[self.principalStorageIndex(principal)];
    }

    pub fn getIpcBufferTable(self: *KernelState, principal: PrincipalId) *IpcBufferCNode {
        return &self.ipc_buffer_tables[self.principalStorageIndex(principal)];
    }

    pub fn getIpcBufferTableConst(self: *const KernelState, principal: PrincipalId) *const IpcBufferCNode {
        return &self.ipc_buffer_tables[self.principalStorageIndex(principal)];
    }

    pub fn getExecImageTable(self: *KernelState, principal: PrincipalId) *ExecImageCNode {
        return &self.exec_image_tables[self.principalStorageIndex(principal)];
    }

    pub fn getExecImageTableConst(self: *const KernelState, principal: PrincipalId) *const ExecImageCNode {
        return &self.exec_image_tables[self.principalStorageIndex(principal)];
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
        try self.ipc_buffer_mailboxes[@intFromEnum(target)].push(.{
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
        const backing = ImageBacking.init(page_paddrs, page_offset_bytes, size_bytes) orelse return KernelError.InvalidState;

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

    pub fn deriveVmObjectCap(
        self: *KernelState,
        owner: PrincipalId,
        cap_id: u64,
        offset_bytes: u64,
        size_bytes: u64,
        rights: VmObjectRights,
    ) KernelError!u64 {
        try self.requireActiveProcess(owner);

        const src_cap = self.getVmObjectTableConst(owner).findByCapId(cap_id) orelse return KernelError.VmObjectCapabilityNotFound;
        if (!isVmObjectRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;
        const backing = src_cap.backing.slice(offset_bytes, size_bytes) orelse return KernelError.InvalidState;

        const child_id = self.allocCapId();
        try self.getVmObjectTable(owner).add(.{
            .backing = backing,
            .rights = rights,
            .cap_id = child_id,
            .root_cap_id = src_cap.root_cap_id,
            .parent_cap_id = src_cap.cap_id,
        });
        return child_id;
    }

    pub fn installExecImageCap(
        self: *KernelState,
        owner: PrincipalId,
        vm_cap_id: u64,
        rights: ExecImageRights,
    ) KernelError!u64 {
        try self.requireActiveProcess(owner);

        const vm_cap = self.getVmObjectTableConst(owner).findByCapId(vm_cap_id) orelse return KernelError.VmObjectCapabilityNotFound;
        if (!vm_cap.rights.read) return KernelError.InvalidState;

        const root_id = self.allocCapId();
        try self.getExecImageTable(owner).add(.{
            .backing = vm_cap.backing,
            .rights = rights,
            .cap_id = root_id,
            .root_cap_id = root_id,
            .parent_cap_id = 0,
        });
        return root_id;
    }

    pub fn grantExecImageCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        cap_id: u64,
        rights: ExecImageRights,
    ) KernelError!u64 {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);

        const src_cap = self.getExecImageTableConst(from).findByCapId(cap_id) orelse return KernelError.ExecImageCapabilityNotFound;
        if (!src_cap.rights.grant) return KernelError.InvalidState;
        if (!isExecImageRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;

        const child_id = self.allocCapId();
        try self.getExecImageTable(to).add(.{
            .backing = src_cap.backing,
            .rights = rights,
            .cap_id = child_id,
            .root_cap_id = src_cap.root_cap_id,
            .parent_cap_id = src_cap.cap_id,
        });
        return child_id;
    }

    pub fn scanCapTables(self: *const KernelState, paddr: u64) OwnershipView {
        // デバッグ用: capability 走査から論理的な保持者ビューを作る。
        var owner: ?PrincipalId = null;
        var pidx: usize = 0;
        while (pidx < principal_count) : (pidx += 1) {
            const principal: PrincipalId = @enumFromInt(pidx);
            if (self.cap_tables[pidx].find(paddr) == null) continue;
            if (owner != null) return .{ .shared = {} };
            owner = principal;
        }
        if (owner) |principal| return .{ .owner = principal };
        return .{ .none = {} };
    }

    fn anyPrincipalHasPageCap(self: *const KernelState, paddr: u64) bool {
        var pidx: usize = 0;
        while (pidx < principal_count) : (pidx += 1) {
            if (self.cap_tables[pidx].find(paddr) != null) return true;
        }
        return false;
    }

    pub fn anyOtherPrincipalHasPageCap(self: *const KernelState, owner: PrincipalId, paddr: u64) bool {
        const owner_index = self.principalStorageIndex(owner);
        var pidx: usize = 0;
        while (pidx < principal_count) : (pidx += 1) {
            if (pidx == owner_index) continue;
            if (self.cap_tables[pidx].find(paddr) != null) return true;
        }
        return false;
    }

    pub fn allocPage(
        self: *KernelState,
        requester: PrincipalId,
        free_list: *FreePageList,
    ) KernelError!PageCapability {
        _ = requester;
        _ = self;
        const user_mappable_paddr_limit: u64 = 4 * 1024 * 1024 * 1024;
        const paddr = try free_list.popFrontBelow(user_mappable_paddr_limit);
        return .{
            .paddr = paddr,
        };
    }

    pub fn allocPageTo(
        self: *KernelState,
        requester: PrincipalId,
        free_list: *FreePageList,
    ) KernelError!PageCapability {
        try self.requireActiveProcess(requester);
        const table = self.getTable(requester);
        if (table.len >= CNode.max_caps) return KernelError.TableFull;
        const cap = try self.allocPage(requester, free_list);
        if (self.debug_alloc_page_hook) |hook| {
            hook(self, requester, .after_pop, cap.paddr);
        }
        if (!builtin.is_test) {
            const page_bytes: [*]u8 = @ptrFromInt(cap.paddr);
            @memset(page_bytes[0..4096], 0);
        }
        if (self.debug_alloc_page_hook) |hook| {
            hook(self, requester, .after_memset, cap.paddr);
        }
        const root_id = self.allocCapId();
        try table.addAssumeFresh(.{
            .paddr = cap.paddr,
            .rights = .{
                .cpu_read = true,
                .cpu_write = true,
                .dma = true,
                .grant = true,
            },
            .cap_id = root_id,
            .root_cap_id = root_id,
            .parent_cap_id = 0,
        });
        if (self.debug_alloc_page_hook) |hook| {
            hook(self, requester, .after_cap_add, cap.paddr);
        }
        // Freshly allocated pages are not mapped yet, so no PTE rights sync is needed here.
        return cap;
    }

    pub fn reclaimExclusiveRootPage(
        self: *KernelState,
        owner: PrincipalId,
        paddr: u64,
        free_list: *FreePageList,
    ) KernelError!void {
        if (!free_list.canAppendPage(0, paddr)) return KernelError.TooManyFreeRanges;
        const table = self.getTable(owner);
        const cap = table.find(paddr) orelse return KernelError.CapabilityNotFound;
        if (cap.cap_id != cap.root_cap_id or cap.parent_cap_id != 0) return KernelError.InvalidState;
        switch (self.scanCapTables(paddr)) {
            .owner => |actual_owner| if (actual_owner != owner) return KernelError.InvalidState,
            .shared, .none => return KernelError.InvalidState,
        }
        _ = table.removeByPaddr(paddr);
        self.removeDmaMappingsForPrincipalPaddr(owner, paddr);
        try device_capabilities.syncIommuForPrincipalPaddr(self, owner, paddr, .revoke);
        try free_list.appendPage(0, paddr);
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
        self.removeDmaMappingsForPrincipalPaddr(owner, paddr);
        try device_capabilities.syncIommuForPrincipalPaddr(self, owner, paddr, .revoke);
        try free_list.appendPage(0, paddr);
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
        self.removeDmaMappingsForPrincipalPaddr(from, paddr);
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
        try self.cap_mailboxes[@intFromEnum(target)].push(.{
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
        try self.cap_mailboxes[@intFromEnum(target)].push(.{
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
        const storage_index = @intFromEnum(receiver);
        if (self.pending_page_transfers[storage_index]) |pending| {
            return pending.transfer_id;
        }
        const received = self.cap_mailboxes[storage_index].pop() orelse return KernelError.MailboxEmpty;
        self.pending_page_transfers[storage_index] = received;
        return received.transfer_id;
    }

    pub fn recvIpcBufferTransfer(self: *KernelState, receiver: PrincipalId) KernelError!u64 {
        try self.requireActiveProcess(receiver);
        const storage_index = @intFromEnum(receiver);
        if (self.pending_ipc_buffer_transfers[storage_index]) |pending| {
            return pending.transfer_id;
        }
        const received = self.ipc_buffer_mailboxes[storage_index].pop() orelse return KernelError.MailboxEmpty;
        self.pending_ipc_buffer_transfers[storage_index] = received;
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
        const storage_index = @intFromEnum(receiver);
        const pending = self.pending_page_transfers[storage_index] orelse return KernelError.MailboxEmpty;
        if (pending.transfer_id != transfer_id) return KernelError.InvalidState;
        if (pending.retain_sender) {
            try self.grantCap(pending.sender, receiver, pending.paddr, pending.rights);
        } else {
            try self.moveCap(pending.sender, receiver, pending.paddr, pending.rights);
        }
        self.pending_page_transfers[storage_index] = null;
        return pending.paddr;
    }

    pub fn acceptIpcBufferTransfer(self: *KernelState, receiver: PrincipalId, transfer_id: u64) KernelError!u64 {
        try self.requireActiveProcess(receiver);
        const storage_index = @intFromEnum(receiver);
        const pending = self.pending_ipc_buffer_transfers[storage_index] orelse return KernelError.MailboxEmpty;
        if (pending.transfer_id != transfer_id) return KernelError.InvalidState;
        const child_id = if (pending.retain_sender)
            try self.grantIpcBufferCap(pending.sender, receiver, pending.cap_id, pending.rights)
        else
            try self.moveIpcBufferCap(pending.sender, receiver, pending.cap_id, pending.rights);
        self.pending_ipc_buffer_transfers[storage_index] = null;
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
            while (pidx < principal_count) : (pidx += 1) {
                const table = &self.cap_tables[pidx];
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
            while (pidx < principal_count) : (pidx += 1) {
                const table = &self.cap_tables[pidx];
                const cap = table.findByCapId(cap_id) orelse continue;
                const removed_paddr = cap.paddr;
                _ = table.removeByCapId(cap_id);
                self.removeDmaMappingsForPrincipalPaddr(@enumFromInt(pidx), removed_paddr);
                try device_capabilities.syncIommuForPrincipalPaddr(self, @enumFromInt(pidx), removed_paddr, .revoke);
                if (self.pte_sync_hook) |hook| {
                    callPteSyncHook(hook, self, @enumFromInt(pidx), removed_paddr);
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
