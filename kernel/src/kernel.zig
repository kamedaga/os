const std = @import("std");
const builtin = @import("builtin");
const capability = @import("capability.zig");
const abi_root = @import("kernel_abi_root");
const cap_transfer_abi = abi_root.cap_transfer_abi;
const dma_mapping_manager = @import("dma_mapping_manager.zig");
const device_capabilities = @import("device_capabilities.zig");
pub const initial_process_count: usize = 8;
pub const process_count: usize = 32;
pub const max_thread_slots: usize = 16;
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
    cpu_read: bool,
    cpu_write: bool,
    dma: bool,
    grant: bool = false,
};

pub const MapProt = struct {
    read: bool = false,
    write: bool = false,
    exec: bool = false,
};

pub const Capability = struct {
    paddr: u64,
    rights: Rights,
    cap_id: u64,
    root_cap_id: u64,
    parent_cap_id: u64,
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
    TooManyUntypedBlocks,
    UntypedNotFound,
    UntypedHasChildren,
};

pub const CNode = struct {
    pub const max_caps = 4096;

    caps: [max_caps]Capability = undefined,
    len: usize = 0,

    pub fn add(self: *CNode, cap: Capability) KernelError!void {
        if (self.findIndex(cap.paddr) != null) return KernelError.InvalidState;
        if (self.len >= self.caps.len) {
            return KernelError.TableFull;
        }
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn addAssumeFresh(self: *CNode, cap: Capability) KernelError!void {
        if (self.len >= self.caps.len) {
            return KernelError.TableFull;
        }
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn removeByPaddr(self: *CNode, paddr: u64) bool {
        if (self.findIndex(paddr)) |index| {
            var i = index;
            while (i + 1 < self.len) : (i += 1) {
                self.caps[i] = self.caps[i + 1];
            }
            self.len -= 1;
            return true;
        }
        return false;
    }

    pub fn find(self: *const CNode, paddr: u64) ?*const Capability {
        if (self.findIndex(paddr)) |index| {
            return &self.caps[index];
        }
        return null;
    }

    pub fn findByCapId(self: *const CNode, cap_id: u64) ?*const Capability {
        if (self.findIndexByCapId(cap_id)) |index| {
            return &self.caps[index];
        }
        return null;
    }

    pub fn removeByCapId(self: *CNode, cap_id: u64) bool {
        if (self.findIndexByCapId(cap_id)) |index| {
            var i = index;
            while (i + 1 < self.len) : (i += 1) {
                self.caps[i] = self.caps[i + 1];
            }
            self.len -= 1;
            return true;
        }
        return false;
    }

    fn findIndex(self: *const CNode, paddr: u64) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            if (self.caps[i].paddr == paddr) return i;
        }
        return null;
    }

    fn findIndexByCapId(self: *const CNode, cap_id: u64) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            if (self.caps[i].cap_id == cap_id) return i;
        }
        return null;
    }
};

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
        while (i < self.len) : (i += 1) {
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
        while (i < self.len) : (i += 1) {
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

pub const max_image_backing_pages: usize = 512;

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
    _reserved0: u32 = 0,
    size_bytes: u64 = 0,
    page_paddrs: [max_image_backing_pages]u64 = [_]u64{0} ** max_image_backing_pages,

    fn init(page_paddrs: []const u64, page_offset_bytes: u16, size_bytes: u64) ?ImageBacking {
        if (page_paddrs.len == 0 or page_paddrs.len > max_image_backing_pages) return null;
        if (size_bytes == 0) return null;
        const first_page_bytes = @as(u64, 4096 - page_offset_bytes);
        const total_capacity = first_page_bytes + (@as(u64, @intCast(page_paddrs.len - 1)) * 4096);
        if (total_capacity < size_bytes) return null;

        var backing = ImageBacking{
            .page_offset_bytes = page_offset_bytes,
            .page_count = @intCast(page_paddrs.len),
            .size_bytes = size_bytes,
        };
        for (page_paddrs, 0..) |paddr, i| {
            if ((paddr & 0xFFF) != 0) return null;
            backing.page_paddrs[i] = paddr;
        }
        return backing;
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

        var page_paddrs: [max_image_backing_pages]u64 = [_]u64{0} ** max_image_backing_pages;
        var i: usize = 0;
        while (i < page_count) : (i += 1) {
            page_paddrs[i] = self.page_paddrs[start_page_index + i];
        }
        return ImageBacking.init(page_paddrs[0..page_count], start_page_offset, size_bytes);
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
        // 直前 range と「同じ region かつ物理的に連続」の場合は range を延長する。
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

pub const max_retype_page_batch: usize = 64;

pub const UntypedFlags = packed struct(u8) {
    contiguous_only: bool = false,
    dma_ok: bool = false,
    _reserved: u6 = 0,
};

pub const UntypedBlock = struct {
    active: bool = false,
    base_paddr: u64 = 0,
    size_bytes: u64 = 0,
    used_bytes: u64 = 0,
    flags: UntypedFlags = .{},
};

pub const UntypedCapability = struct {
    block_id: u32,
    cap_id: u64,
    root_cap_id: u64,
    parent_cap_id: u64,
};

pub const UntypedCNode = struct {
    pub const max_caps = 256;

    caps: [max_caps]UntypedCapability = undefined,
    len: usize = 0,

    pub fn add(self: *UntypedCNode, cap: UntypedCapability) KernelError!void {
        if (self.findIndex(cap.block_id) != null) return KernelError.InvalidState;
        if (self.len >= self.caps.len) return KernelError.TableFull;
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn find(self: *const UntypedCNode, block_id: u32) ?*const UntypedCapability {
        if (self.findIndex(block_id)) |index| {
            return &self.caps[index];
        }
        return null;
    }

    pub fn findByCapId(self: *const UntypedCNode, cap_id: u64) ?*const UntypedCapability {
        if (self.findIndexByCapId(cap_id)) |index| {
            return &self.caps[index];
        }
        return null;
    }

    pub fn removeByBlockId(self: *UntypedCNode, block_id: u32) bool {
        if (self.findIndex(block_id)) |index| {
            var i = index;
            while (i + 1 < self.len) : (i += 1) {
                self.caps[i] = self.caps[i + 1];
            }
            self.len -= 1;
            return true;
        }
        return false;
    }

    pub fn removeByCapId(self: *UntypedCNode, cap_id: u64) bool {
        if (self.findIndexByCapId(cap_id)) |index| {
            var i = index;
            while (i + 1 < self.len) : (i += 1) {
                self.caps[i] = self.caps[i + 1];
            }
            self.len -= 1;
            return true;
        }
        return false;
    }

    fn findIndex(self: *const UntypedCNode, block_id: u32) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            if (self.caps[i].block_id == block_id) return i;
        }
        return null;
    }

    fn findIndexByCapId(self: *const UntypedCNode, cap_id: u64) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            if (self.caps[i].cap_id == cap_id) return i;
        }
        return null;
    }
};

pub const UntypedPool = struct {
    pub const max_blocks = 256;

    blocks: [max_blocks]UntypedBlock = [_]UntypedBlock{.{}} ** max_blocks,
    len: usize = 0,

    pub fn allocBlock(
        self: *UntypedPool,
        base_paddr: u64,
        size_bytes: u64,
        flags: UntypedFlags,
    ) KernelError!u32 {
        if ((base_paddr & 0xFFF) != 0 or size_bytes == 0 or (size_bytes & 0xFFF) != 0) {
            return KernelError.InvalidState;
        }
        if (self.len >= self.blocks.len) return KernelError.TooManyUntypedBlocks;
        const block_id: u32 = @intCast(self.len);
        self.blocks[self.len] = .{
            .active = true,
            .base_paddr = base_paddr,
            .size_bytes = size_bytes,
            .used_bytes = 0,
            .flags = flags,
        };
        self.len += 1;
        return block_id;
    }

    pub fn getBlock(self: *UntypedPool, block_id: u32) ?*UntypedBlock {
        const index: usize = @intCast(block_id);
        if (index >= self.len) return null;
        if (!self.blocks[index].active) return null;
        return &self.blocks[index];
    }

    pub fn getBlockConst(self: *const UntypedPool, block_id: u32) ?*const UntypedBlock {
        const index: usize = @intCast(block_id);
        if (index >= self.len) return null;
        if (!self.blocks[index].active) return null;
        return &self.blocks[index];
    }
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
    device: DmaDeviceId = .virtio_gpu,
    paddr: u64 = 0,
};

const IommuNoCapDriverState = struct {
    const max_mappings = 512;

    mode: IommuNoCapDriverMode = .off,
    mappings: [max_mappings]IommuMapEntry = [_]IommuMapEntry{.{}} ** max_mappings,
};

pub const KernelState = struct {
    pub const max_regions = 256;
    const max_total_caps = principal_count * CNode.max_caps;

    regions: [max_regions]Region = undefined,
    region_len: usize = 0,
    process_descriptors: [process_count]ProcessDescriptor = [_]ProcessDescriptor{.{}} ** process_count,
    active_process_count: usize = 0,
    cap_tables: [principal_count]CNode = [_]CNode{.{}} ** principal_count,
    untyped_tables: [principal_count]UntypedCNode = [_]UntypedCNode{.{}} ** principal_count,
    endpoint_tables: [principal_count]EndpointCNode = [_]EndpointCNode{.{}} ** principal_count,
    published_service_endpoints: PublishedEndpointTable = .{},
    endpoint_generation: u64 = 0,
    cap_mailboxes: [principal_count]CapMailbox = [_]CapMailbox{.{}} ** principal_count,
    pending_page_transfers: [principal_count]?PendingCapTransfer = [_]?PendingCapTransfer{null} ** principal_count,
    vm_object_tables: [principal_count]VmObjectCNode = [_]VmObjectCNode{.{}} ** principal_count,
    exec_image_tables: [principal_count]ExecImageCNode = [_]ExecImageCNode{.{}} ** principal_count,
    pte_sync_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, paddr: u64) void = null,
    iommu_audit_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, paddr: u64, mapped: bool, reason: IommuSyncReason) void = null,
    debug_alloc_page_hook: ?*const fn (state: *const KernelState, requester: PrincipalId, stage: DebugAllocPageStage, paddr: u64) void = null,
    revoke_queue: [max_total_caps]u64 = undefined,
    revoke_subtree: [max_total_caps]u64 = undefined,
    untyped_revoke_queue: [max_total_caps]u64 = undefined,
    untyped_revoke_subtree: [max_total_caps]u64 = undefined,
    dma_restore: [max_total_caps]DmaRestoreEntry = [_]DmaRestoreEntry{.{}} ** max_total_caps,
    dma_mappings: dma_mapping_manager.DmaMappingTable = .{},
    dma_device_domains: dma_mapping_manager.DeviceDomainTable = .{},
    iommu_caps: device_capabilities.IommuCapabilityTable = .{},
    queue_caps: device_capabilities.QueueCapabilityTable = .{},
    command_caps: device_capabilities.CommandCapabilityTable = .{},
    iommu: IommuNoCapDriverState = .{},
    next_cap_id: u64 = 1,
    next_transfer_id: u64 = cap_transfer_abi.transfer_id_min,
    untyped_pool: UntypedPool = .{},

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

    pub fn createProcessDescriptor(self: *KernelState, label: []const u8) ?PrincipalId {
        var i: usize = 0;
        while (i < self.process_descriptors.len) : (i += 1) {
            if (self.process_descriptors[i].active) continue;
            const principal = processPrincipal(i);
            self.process_descriptors[i] = .{
                .active = true,
                .principal = principal,
                .label = label,
            };
            self.active_process_count += 1;
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
        self.process_descriptors[index] = .{
            .active = true,
            .principal = principal,
            .label = label,
        };
        self.active_process_count += 1;
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
        return true;
    }

    pub fn removeProcessDescriptor(self: *KernelState, principal: PrincipalId) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        if (!self.process_descriptors[index].active) return false;
        self.process_descriptors[index] = .{};
        if (self.active_process_count > 0) self.active_process_count -= 1;
        return true;
    }

    fn clearPrincipalState(self: *KernelState) void {
        var i: usize = 0;
        while (i < principal_count) : (i += 1) {
            self.cap_tables[i] = .{};
            self.untyped_tables[i] = .{};
            self.endpoint_tables[i] = .{};
            self.cap_mailboxes[i] = .{};
            self.pending_page_transfers[i] = null;
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

    pub fn getUntypedTable(self: *KernelState, principal: PrincipalId) *UntypedCNode {
        return &self.untyped_tables[self.principalStorageIndex(principal)];
    }

    pub fn getUntypedTableConst(self: *const KernelState, principal: PrincipalId) *const UntypedCNode {
        return &self.untyped_tables[self.principalStorageIndex(principal)];
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

    pub fn getExecImageTable(self: *KernelState, principal: PrincipalId) *ExecImageCNode {
        return &self.exec_image_tables[self.principalStorageIndex(principal)];
    }

    pub fn getExecImageTableConst(self: *const KernelState, principal: PrincipalId) *const ExecImageCNode {
        return &self.exec_image_tables[self.principalStorageIndex(principal)];
    }

    pub fn endpointTargetFor(self: *const KernelState, owner: PrincipalId, endpoint_id: u64) ?PrincipalId {
        if (!self.hasActivePrincipal(owner)) return null;
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

    fn paddrFallsInsideActiveUntypedBlock(self: *const KernelState, paddr: u64) bool {
        var i: usize = 0;
        while (i < self.untyped_pool.len) : (i += 1) {
            const block = self.untyped_pool.blocks[i];
            if (!block.active) continue;
            const block_end = block.base_paddr + block.size_bytes;
            if (paddr >= block.base_paddr and paddr < block_end) return true;
        }
        return false;
    }

    pub fn allocPage(
        self: *KernelState,
        requester: PrincipalId,
        free_list: *FreePageList,
    ) KernelError!PageCapability {
        _ = requester;
        const user_mappable_paddr_limit: u64 = 4 * 1024 * 1024 * 1024;
        while (true) {
            const paddr = try free_list.popFrontBelow(user_mappable_paddr_limit);
            if (self.anyPrincipalHasPageCap(paddr)) continue;
            if (self.paddrFallsInsideActiveUntypedBlock(paddr)) continue;
            return .{
                .paddr = paddr,
            };
        }
    }

    pub fn allocPageTo(
        self: *KernelState,
        requester: PrincipalId,
        free_list: *FreePageList,
    ) KernelError!PageCapability {
        try self.requireActiveProcess(requester);
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
        try self.getTable(requester).addAssumeFresh(.{
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

    pub fn createUntypedBlock(
        self: *KernelState,
        owner: PrincipalId,
        base_paddr: u64,
        size_bytes: u64,
        flags: UntypedFlags,
    ) KernelError!u32 {
        try self.requireActiveProcess(owner);
        const block_id = try self.untyped_pool.allocBlock(base_paddr, size_bytes, flags);
        const root_id = self.allocCapId();
        try self.getUntypedTable(owner).add(.{
            .block_id = block_id,
            .cap_id = root_id,
            .root_cap_id = root_id,
            .parent_cap_id = 0,
        });
        return block_id;
    }

    pub fn findOwnedUntypedForPages(
        self: *const KernelState,
        owner: PrincipalId,
        page_count: usize,
        contiguous: bool,
    ) ?u32 {
        self.requireActiveProcess(owner) catch return null;
        const bytes_needed, const overflow = @mulWithOverflow(@as(u64, @intCast(page_count)), @as(u64, 4096));
        if (overflow != 0) return null;
        const table = self.getUntypedTableConst(owner);
        var i: usize = 0;
        while (i < table.len) : (i += 1) {
            const cap = table.caps[i];
            const block = self.untyped_pool.getBlockConst(cap.block_id) orelse continue;
            if (contiguous and !block.flags.contiguous_only) continue;
            const used_aligned = pageAlignUp(block.used_bytes);
            if (used_aligned > block.size_bytes) continue;
            if ((block.size_bytes - used_aligned) >= bytes_needed) return cap.block_id;
        }
        return null;
    }

    pub fn retypeUntypedToPages(
        self: *KernelState,
        owner: PrincipalId,
        block_id: u32,
        page_count: usize,
        contiguous: bool,
        out_caps: []PageCapability,
    ) KernelError!void {
        try self.requireActiveProcess(owner);
        if (page_count == 0 or out_caps.len < page_count) return KernelError.InvalidState;
        const untyped_cap = self.getUntypedTableConst(owner).find(block_id) orelse return KernelError.UntypedNotFound;
        const block = self.untyped_pool.getBlock(block_id) orelse return KernelError.UntypedNotFound;
        if (contiguous and !block.flags.contiguous_only) return KernelError.InvalidState;

        const bytes_needed, const overflow = @mulWithOverflow(@as(u64, @intCast(page_count)), @as(u64, 4096));
        if (overflow != 0) return KernelError.InvalidState;
        const used_aligned = pageAlignUp(block.used_bytes);
        if (used_aligned > block.size_bytes or (block.size_bytes - used_aligned) < bytes_needed) {
            return KernelError.OutOfFreePages;
        }

        var i: usize = 0;
        while (i < page_count) : (i += 1) {
            const paddr = block.base_paddr + used_aligned + (@as(u64, @intCast(i)) * 4096);
            if (!builtin.is_test) {
                const page_bytes: [*]u8 = @ptrFromInt(paddr);
                @memset(page_bytes[0..4096], 0);
            }
        }
        i = 0;
        while (i < page_count) : (i += 1) {
            const paddr = block.base_paddr + used_aligned + (@as(u64, @intCast(i)) * 4096);
            const cap_id = self.allocCapId();
            try self.getTable(owner).add(.{
                .paddr = paddr,
                .rights = .{
                    .cpu_read = true,
                    .cpu_write = true,
                    .dma = block.flags.dma_ok,
                    .grant = true,
                },
                .cap_id = cap_id,
                .root_cap_id = untyped_cap.root_cap_id,
                .parent_cap_id = untyped_cap.cap_id,
            });
            out_caps[i] = .{ .paddr = paddr };
        }
        block.used_bytes = used_aligned + bytes_needed;
    }

    pub fn grantUntypedCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        block_id: u32,
    ) KernelError!void {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);
        const src_cap = self.getUntypedTableConst(from).find(block_id) orelse return KernelError.UntypedNotFound;
        if (self.getUntypedTableConst(to).find(block_id) != null) return KernelError.InvalidState;
        const child_id = self.allocCapId();
        try self.getUntypedTable(to).add(.{
            .block_id = block_id,
            .cap_id = child_id,
            .root_cap_id = src_cap.root_cap_id,
            .parent_cap_id = src_cap.cap_id,
        });
    }

    pub fn moveUntypedCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        block_id: u32,
    ) KernelError!void {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);
        const src = self.getUntypedTable(from);
        const src_cap = src.find(block_id) orelse return KernelError.UntypedNotFound;
        if (self.getUntypedTableConst(to).find(block_id) != null) return KernelError.InvalidState;
        const moved = src_cap.*;
        _ = src.removeByBlockId(block_id);
        try self.getUntypedTable(to).add(moved);
    }

    fn hasUntypedChildren(self: *const KernelState, cap_id: u64) bool {
        var pidx: usize = 0;
        while (pidx < principal_count) : (pidx += 1) {
            const table = &self.untyped_tables[pidx];
            var i: usize = 0;
            while (i < table.len) : (i += 1) {
                if (table.caps[i].parent_cap_id == cap_id) return true;
            }
        }
        return false;
    }

    fn hasPageChildren(self: *const KernelState, cap_id: u64) bool {
        var pidx: usize = 0;
        while (pidx < principal_count) : (pidx += 1) {
            const table = &self.cap_tables[pidx];
            var i: usize = 0;
            while (i < table.len) : (i += 1) {
                if (table.caps[i].parent_cap_id == cap_id) return true;
            }
        }
        return false;
    }

    pub fn resetUntyped(
        self: *KernelState,
        owner: PrincipalId,
        block_id: u32,
    ) KernelError!void {
        try self.requireActiveProcess(owner);
        const cap = self.getUntypedTableConst(owner).find(block_id) orelse return KernelError.UntypedNotFound;
        if (cap.parent_cap_id != 0) return KernelError.InvalidState;
        if (self.hasUntypedChildren(cap.cap_id) or self.hasPageChildren(cap.cap_id)) {
            return KernelError.UntypedHasChildren;
        }
        const block = self.untyped_pool.getBlock(block_id) orelse return KernelError.UntypedNotFound;
        block.used_bytes = 0;
    }

    pub fn revokeUntypedCapTree(
        self: *KernelState,
        owner: PrincipalId,
        block_id: u32,
    ) KernelError!void {
        try self.requireActiveProcess(owner);
        const start_cap = self.getUntypedTableConst(owner).find(block_id) orelse return KernelError.UntypedNotFound;
        const start_id = start_cap.cap_id;
        const is_root = start_cap.parent_cap_id == 0;

        const queue = &self.untyped_revoke_queue;
        var queue_len: usize = 0;
        var queue_head: usize = 0;
        queue[0] = start_id;
        queue_len = 1;

        const subtree = &self.untyped_revoke_subtree;
        var subtree_len: usize = 0;

        while (queue_head < queue_len) : (queue_head += 1) {
            const current_id = queue[queue_head];
            if (containsCapId(subtree[0..subtree_len], current_id)) continue;
            if (subtree_len >= subtree.len) return KernelError.RevokeOverflow;
            subtree[subtree_len] = current_id;
            subtree_len += 1;

            var pidx: usize = 0;
            while (pidx < principal_count) : (pidx += 1) {
                const page_table = &self.cap_tables[pidx];
                var i: usize = 0;
                while (i < page_table.len) : (i += 1) {
                    const cap = page_table.caps[i];
                    if (cap.parent_cap_id != current_id) continue;
                    if (containsCapId(queue[0..queue_len], cap.cap_id)) continue;
                    if (queue_len >= queue.len) return KernelError.RevokeOverflow;
                    queue[queue_len] = cap.cap_id;
                    queue_len += 1;
                }

                const untyped_table = &self.untyped_tables[pidx];
                i = 0;
                while (i < untyped_table.len) : (i += 1) {
                    const cap = untyped_table.caps[i];
                    if (cap.parent_cap_id != current_id) continue;
                    if (containsCapId(queue[0..queue_len], cap.cap_id)) continue;
                    if (queue_len >= queue.len) return KernelError.RevokeOverflow;
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
                const page_table = &self.cap_tables[pidx];
                if (page_table.findByCapId(cap_id)) |page_cap| {
                    const removed_paddr = page_cap.paddr;
                    _ = page_table.removeByCapId(cap_id);
                    self.removeDmaMappingsForPrincipalPaddr(@enumFromInt(pidx), removed_paddr);
                    try device_capabilities.syncIommuForPrincipalPaddr(self, @enumFromInt(pidx), removed_paddr, .revoke);
                    if (self.pte_sync_hook) |hook| {
                        callPteSyncHook(hook, self, @enumFromInt(pidx), removed_paddr);
                    }
                    break;
                }

                const untyped_table = &self.untyped_tables[pidx];
                if (untyped_table.findByCapId(cap_id) != null) {
                    _ = untyped_table.removeByCapId(cap_id);
                    break;
                }
            }
        }

        if (is_root) {
            const block = self.untyped_pool.getBlock(block_id) orelse return KernelError.UntypedNotFound;
            block.used_bytes = 0;
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
                    const cap = table.caps[i];
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

test "dma mapping manager stage1 create state and release" {
    var s = KernelState.initPhase1();
    const token = try s.dmaMapCreateStage1(
        .Process0,
        .virtio_gpu,
        0x4000,
        4096,
        .bidirectional,
    );
    const mapping = s.dmaMapFindStage1(token).?;
    try std.testing.expectEqual(@as(u64, 0x4000), mapping.paddr_start);
    try std.testing.expectEqual(@as(u64, 4096), mapping.length);
    try std.testing.expectEqual(DmaMappingState.mapped, mapping.state);

    try s.dmaMapSetStateStage1(token, .in_flight);
    try std.testing.expectEqual(DmaMappingState.in_flight, s.dmaMapFindStage1(token).?.state);
    try s.dmaMapSetStateStage1(token, .completed);
    try std.testing.expectEqual(DmaMappingState.completed, s.dmaMapFindStage1(token).?.state);

    try s.dmaMapReleaseStage1(token);
    try std.testing.expect(s.dmaMapFindStage1(token) == null);
}

test "dma mapping manager stage1 device domain bind" {
    var s = KernelState.initPhase1();
    try s.dmaBindDeviceDomainStage1(.virtio_gpu, 1);
    try std.testing.expectEqual(@as(?u32, 1), s.dmaDeviceDomainStage1(.virtio_gpu));
}

test "queue cap stage2 authorize submit and notify" {
    var s = KernelState.initPhase1();
    const submit_token = try device_capabilities.queueCapGrantStage2(&s, .Process1, .virtio_gpu, 0, true, false);
    const notify_token = try device_capabilities.queueCapGrantStage2(&s, .Process1, .virtio_gpu, 0, false, true);

    try device_capabilities.queueCapAuthorizeStage2(&s, .Process1, submit_token, 0, .submit);
    try device_capabilities.queueCapAuthorizeStage2(&s, .Process1, notify_token, 0, .notify);

    try std.testing.expectError(KernelError.InvalidState, device_capabilities.queueCapAuthorizeStage2(&s, .Process1, submit_token, 0, .notify));
    try std.testing.expectError(KernelError.InvalidState, device_capabilities.queueCapAuthorizeStage2(&s, .Process1, notify_token, 0, .submit));
}

test "queue cap stage2 rejects owner mismatch" {
    var s = KernelState.initPhase1();
    const token = try device_capabilities.queueCapGrantStage2(&s, .Process1, .virtio_gpu, 0, true, true);
    try std.testing.expectError(KernelError.InvalidState, device_capabilities.queueCapAuthorizeStage2(&s, .Process0, token, 0, .submit));
}

test "device queue cap revoke removes descendants" {
    var s = KernelState.initPhase1();
    const root = try device_capabilities.queueCapGrantStage2(&s, .Process1, .virtio_gpu, 0, true, true);
    const child = try device_capabilities.grantQueueCapStage2(&s, .Process1, .Process2, root);

    try device_capabilities.queueCapAuthorizeStage2(&s, .Process2, child, 0, .submit);
    try device_capabilities.revokeDeviceCapStage2(&s, .Process1, .virtqueue, root);

    try std.testing.expectError(KernelError.CapabilityNotFound, device_capabilities.queueCapAuthorizeStage2(&s, .Process1, root, 0, .submit));
    try std.testing.expectError(KernelError.CapabilityNotFound, device_capabilities.queueCapAuthorizeStage2(&s, .Process2, child, 0, .submit));
}

test "command cap derive subset preserves lineage and revoke" {
    var s = KernelState.initPhase1();
    const full_mask = commandOpcodeBitForTest(.blk_read) |
        commandOpcodeBitForTest(.blk_write) |
        commandOpcodeBitForTest(.blk_flush);
    const read_mask = commandOpcodeBitForTest(.blk_read);
    const root = try device_capabilities.commandCapGrantStage2(&s, .Process1, .virtio_blk, full_mask);
    const subset = try device_capabilities.deriveCommandCapStage2(&s, .Process1, root, read_mask);
    const child = try device_capabilities.grantCommandCapStage2(&s, .Process1, .Process2, subset);

    try device_capabilities.commandCapAuthorizeStage2(&s, .Process1, subset, .virtio_blk, .blk_read);
    try std.testing.expectError(KernelError.InvalidState, device_capabilities.commandCapAuthorizeStage2(&s, .Process1, subset, .virtio_blk, .blk_write));
    try device_capabilities.commandCapAuthorizeStage2(&s, .Process2, child, .virtio_blk, .blk_read);

    try device_capabilities.revokeDeviceCapStage2(&s, .Process1, .command, subset);

    try std.testing.expectError(KernelError.CapabilityNotFound, device_capabilities.commandCapAuthorizeStage2(&s, .Process1, subset, .virtio_blk, .blk_read));
    try std.testing.expectError(KernelError.CapabilityNotFound, device_capabilities.commandCapAuthorizeStage2(&s, .Process2, child, .virtio_blk, .blk_read));
    try device_capabilities.commandCapAuthorizeStage2(&s, .Process1, root, .virtio_blk, .blk_write);
}

test "gpu command cap isolates virgl submit from scanout" {
    var s = KernelState.initPhase1();
    const full_mask = commandOpcodeBitForTest(.gpu_admin) |
        commandOpcodeBitForTest(.gpu_virgl_context) |
        commandOpcodeBitForTest(.gpu_virgl_resource) |
        commandOpcodeBitForTest(.gpu_virgl_submit) |
        commandOpcodeBitForTest(.gpu_fence);
    const submit_mask = commandOpcodeBitForTest(.gpu_virgl_submit) |
        commandOpcodeBitForTest(.gpu_fence);
    const root = try device_capabilities.commandCapGrantStage2(&s, .Process1, .virtio_gpu, full_mask);
    const submit_only = try device_capabilities.deriveCommandCapStage2(&s, .Process1, root, submit_mask);

    try device_capabilities.commandCapAuthorizeStage2(&s, .Process1, submit_only, .virtio_gpu, .gpu_virgl_submit);
    try device_capabilities.commandCapAuthorizeStage2(&s, .Process1, submit_only, .virtio_gpu, .gpu_fence);
    try std.testing.expectError(KernelError.InvalidState, device_capabilities.commandCapAuthorizeStage2(&s, .Process1, submit_only, .virtio_gpu, .gpu_scanout));
    try std.testing.expectError(KernelError.InvalidState, device_capabilities.commandCapAuthorizeStage2(&s, .Process1, submit_only, .virtio_blk, .gpu_virgl_submit));
}

fn commandOpcodeBitForTest(opcode: device_capabilities.CommandOpcodeClass) u64 {
    return @as(u64, 1) << @as(u6, @intCast(@intFromEnum(opcode)));
}

test "dma mapping manager stage1 rejects invalid transition" {
    var s = KernelState.initPhase1();
    const token = try s.dmaMapCreateStage1(
        .Process0,
        .virtio_gpu,
        0x5000,
        4096,
        .bidirectional,
    );

    try std.testing.expectError(KernelError.InvalidState, s.dmaMapSetStateStage1(token, .completed));
    try std.testing.expectEqual(DmaMappingState.mapped, s.dmaMapFindStage1(token).?.state);
}

test "dma mapping manager stage1 release requires completed" {
    var s = KernelState.initPhase1();
    const token = try s.dmaMapCreateStage1(
        .Process0,
        .virtio_gpu,
        0x6000,
        4096,
        .bidirectional,
    );

    try std.testing.expectError(KernelError.InvalidState, s.dmaMapReleaseStage1(token));
    try s.dmaMapSetStateStage1(token, .in_flight);
    try s.dmaMapSetStateStage1(token, .completed);
    try s.dmaMapReleaseStage1(token);
    try std.testing.expect(s.dmaMapFindStage1(token) == null);
}
test "phase1 init state" {
    const s = KernelState.initPhase1();

    try std.testing.expectEqual(@as(usize, 1), s.region_len);
    try std.testing.expectEqualDeep(OwnershipView{ .owner = default_process_principal }, s.scanCapTables(0x1000));

    const p0 = s.getTableConst(.Process0);
    try std.testing.expectEqual(@as(usize, 1), p0.len);
    try std.testing.expect(p0.find(0x1000).?.rights.cpu_read);
    try std.testing.expect(p0.find(0x1000).?.rights.cpu_write);
    try std.testing.expect(p0.find(0x1000).?.rights.dma);
    try std.testing.expect(p0.find(0x1000).?.rights.grant);

    const dev = s.getTableConst(.Device0);
    try std.testing.expectEqual(@as(usize, 0), dev.len);
}

test "start dma moves owner and capabilities" {
    var s = KernelState.initPhase1();
    try s.startDma(default_process_principal, 0x1000);

    try std.testing.expectEqualDeep(OwnershipView{ .owner = .Device0 }, s.scanCapTables(0x1000));
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    const dev_cap = s.getTableConst(.Device0).find(0x1000).?;
    try std.testing.expect(dev_cap.rights.dma);
    try std.testing.expect(!dev_cap.rights.cpu_read);
    try std.testing.expect(!dev_cap.rights.cpu_write);
}

test "complete dma returns owner and capabilities" {
    var s = KernelState.initPhase1();
    try s.startDma(default_process_principal, 0x1000);
    try s.completeDma(0x1000);

    try std.testing.expectEqualDeep(OwnershipView{ .owner = default_process_principal }, s.scanCapTables(0x1000));
    try std.testing.expect(s.getTableConst(.Device0).find(0x1000) == null);
    const p0_cap = s.getTableConst(.Process0).find(0x1000).?;
    try std.testing.expect(p0_cap.rights.cpu_read);
    try std.testing.expect(p0_cap.rights.cpu_write);
    try std.testing.expect(p0_cap.rights.dma);
}

test "complete dma restores original rights" {
    var s = KernelState.initPhase1();
    try s.moveCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });
    try s.moveCap(.Process1, .Process0, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });

    try s.startDma(default_process_principal, 0x1000);
    try s.completeDma(0x1000);

    const p0_cap = s.getTableConst(.Process0).find(0x1000).?;
    try std.testing.expect(p0_cap.rights.cpu_read);
    try std.testing.expect(!p0_cap.rights.cpu_write);
    try std.testing.expect(p0_cap.rights.dma);
}

test "invalid transition rejected" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.InvalidState, s.completeDma(0x1000));
    try s.startDma(default_process_principal, 0x1000);
    try std.testing.expectError(KernelError.CapabilityNotFound, s.startDma(default_process_principal, 0x1000));
}

test "init from detected regions" {
    const s = try KernelState.initFromDetectedRegions(3);
    try std.testing.expectEqual(@as(usize, 3), s.region_len);
    try std.testing.expectEqualDeep(OwnershipView{ .none = {} }, s.scanCapTables(0x3000));
    try std.testing.expectEqual(@as(usize, 0), s.getTableConst(.Process0).len);
}

test "free page list append region" {
    var free_list = FreePageList{};
    try free_list.appendRegion(0, 0x1000, 3);

    try std.testing.expectEqual(@as(usize, 3), free_list.len);
    try std.testing.expectEqual(@as(usize, 1), free_list.range_len);
    try std.testing.expectEqual(@as(u64, 0), free_list.ranges[0].region_id);
    try std.testing.expectEqual(@as(u64, 0x1000), free_list.ranges[0].physical_start);
    try std.testing.expectEqual(@as(usize, 3), free_list.ranges[0].len);
}

test "free page list splits range when non-contiguous" {
    var free_list = FreePageList{};
    try free_list.appendPage(0, 0x1000);
    try free_list.appendPage(0, 0x2000);
    try free_list.appendPage(0, 0x5000);

    try std.testing.expectEqual(@as(usize, 3), free_list.len);
    try std.testing.expectEqual(@as(usize, 2), free_list.range_len);
    try std.testing.expectEqual(@as(usize, 2), free_list.ranges[0].len);
    try std.testing.expectEqual(@as(usize, 1), free_list.ranges[1].len);
}

test "free page list pop front updates ranges" {
    var free_list = FreePageList{};
    try free_list.appendRegion(0, 0x1000, 3);
    try std.testing.expectEqual(@as(u64, 0x1000), try free_list.popFront());
    try std.testing.expectEqual(@as(usize, 2), free_list.len);
    try std.testing.expectEqual(@as(usize, 1), free_list.range_len);
    try std.testing.expectEqual(@as(u64, 0x2000), free_list.ranges[0].physical_start);
    try std.testing.expectEqual(@as(usize, 2), free_list.ranges[0].len);
}

test "alloc page to principal installs capability by paddr" {
    var s = try KernelState.initFromDetectedRegions(1);
    var free_list = FreePageList{};
    try free_list.appendPage(0, 0x9000);
    const cap = try s.allocPageTo(.Process0, &free_list);
    try std.testing.expectEqual(@as(u64, 0x9000), cap.paddr);
    const installed = s.getTableConst(.Process0).find(0x9000).?;
    try std.testing.expect(installed.rights.grant);
}

test "allocPageTo skips paddr already covered by active untyped block" {
    var s = try KernelState.initFromDetectedRegions(1);
    var free_list = FreePageList{};
    try free_list.appendPage(0, 0x1000);
    try free_list.appendPage(0, 0x2000);
    _ = try s.createUntypedBlock(.Process0, 0x2000, 0x1000, .{
        .contiguous_only = true,
        .dma_ok = true,
    });

    const cap = try s.allocPageTo(.Process1, &free_list);
    try std.testing.expectEqual(@as(u64, 0x1000), cap.paddr);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) != null);
}

test "allocPageTo skips paddr that already exists in another cap table" {
    var s = try KernelState.initFromDetectedRegions(1);
    var free_list = FreePageList{};
    try free_list.appendPage(0, 0x1000);
    try free_list.appendPage(0, 0x3000);
    try s.installCap(.Process1, 0x3000, .{
        .cpu_read = true,
        .cpu_write = true,
        .dma = true,
    });

    const cap = try s.allocPageTo(.Process0, &free_list);
    try std.testing.expectEqual(@as(u64, 0x1000), cap.paddr);
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) != null);
}

test "untyped retype installs page caps under untyped root" {
    var s = try KernelState.initFromDetectedRegions(1);
    const block_id = try s.createUntypedBlock(.Process0, 0x20_0000, 0x4000, .{
        .contiguous_only = true,
        .dma_ok = true,
    });
    var caps: [2]PageCapability = undefined;
    try s.retypeUntypedToPages(.Process0, block_id, 2, true, caps[0..]);

    try std.testing.expectEqual(@as(u64, 0x20_0000), caps[0].paddr);
    try std.testing.expectEqual(@as(u64, 0x20_1000), caps[1].paddr);
    try std.testing.expect(s.getTableConst(.Process0).find(caps[0].paddr).?.rights.grant);
    try std.testing.expect(s.getTableConst(.Process0).find(caps[1].paddr).?.rights.grant);
}

test "untyped grant allows child owner to retype" {
    var s = try KernelState.initFromDetectedRegions(1);
    const block_id = try s.createUntypedBlock(.Process0, 0x30_0000, 0x4000, .{
        .contiguous_only = true,
        .dma_ok = true,
    });
    try s.grantUntypedCap(.Process0, .Process1, block_id);

    var caps: [1]PageCapability = undefined;
    try s.retypeUntypedToPages(.Process1, block_id, 1, true, caps[0..]);
    try std.testing.expectEqual(@as(u64, 0x30_0000), caps[0].paddr);
    try std.testing.expect(s.getTableConst(.Process1).find(caps[0].paddr) != null);
}

test "findOwnedUntypedForPages sees granted block for Process1" {
    var s = try KernelState.initFromDetectedRegions(1);
    const block_id = try s.createUntypedBlock(.Process0, 0x35_0000, 0x8000, .{
        .contiguous_only = true,
        .dma_ok = true,
    });
    try s.grantUntypedCap(.Process0, .Process1, block_id);

    try std.testing.expectEqual(block_id, s.findOwnedUntypedForPages(.Process1, 2, true).?);
}

test "untyped revoke tree removes descendant pages and resets root block" {
    var s = try KernelState.initFromDetectedRegions(1);
    const block_id = try s.createUntypedBlock(.Process0, 0x40_0000, 0x4000, .{
        .contiguous_only = true,
        .dma_ok = true,
    });
    try s.grantUntypedCap(.Process0, .Process1, block_id);
    var caps: [1]PageCapability = undefined;
    try s.retypeUntypedToPages(.Process1, block_id, 1, true, caps[0..]);

    try s.revokeUntypedCapTree(.Process0, block_id);
    try std.testing.expectEqual(@as(usize, 0), s.getUntypedTableConst(.Process0).len);
    try std.testing.expectEqual(@as(usize, 0), s.getUntypedTableConst(.Process1).len);
    try std.testing.expect(s.getTableConst(.Process1).find(caps[0].paddr) == null);
    try std.testing.expectEqual(@as(u64, 0), s.untyped_pool.getBlockConst(block_id).?.used_bytes);
}

test "moveCap enforces single holder" {
    var s = KernelState.initPhase1();
    try s.moveCap(.Process0, .Device0, 0x1000, .{ .cpu_read = false, .cpu_write = false, .dma = true });

    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    try std.testing.expect(s.getTableConst(.Device0).find(0x1000) != null);
    try std.testing.expectEqualDeep(OwnershipView{ .owner = .Device0 }, s.scanCapTables(0x1000));
}

test "moveCap rejects rights escalation" {
    var s = KernelState.initPhase1();
    try s.moveCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    });
    try std.testing.expectError(KernelError.InvalidState, s.moveCap(.Process1, .Device0, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    }));
}

test "moveCap rejects cpu rights when moving to Device0" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.InvalidState, s.moveCap(.Process0, .Device0, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    }));
}

test "sendCap moves capability process to process with rights preserved" {
    var s = KernelState.initPhase1();
    try s.sendCap(.Process0, .Process1, 0x1000);

    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    const p1_cap = s.getTableConst(.Process1).find(0x1000).?;
    try std.testing.expect(p1_cap.rights.cpu_read);
    try std.testing.expect(p1_cap.rights.cpu_write);
    try std.testing.expect(p1_cap.rights.dma);
    try std.testing.expect(p1_cap.rights.grant);
}

test "sendCap rejects non-process endpoints" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.InvalidState, s.sendCap(.Process0, .Device0, 0x1000));
    try std.testing.expectError(KernelError.InvalidState, s.sendCap(.Device0, .Process1, 0x1000));
}

test "sendCapOnEndpoint requires endpoint capability" {
    var s = KernelState.initPhase1();
    try s.installEndpoint(.Process0, 0x11, .Process1);
    try s.sendCapOnEndpoint(.Process0, 0x11, 0x1000);
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) != null);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) == null);
    const transfer_id = try s.recvCap(.Process1);
    try std.testing.expect(transfer_id >= 0x1000);
    try std.testing.expectEqual(@as(u64, 0x1000), try s.acceptCapTransfer(.Process1, transfer_id));
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) != null);
}

test "iommu no-cap-driver shadow maps active dma mapping" {
    var s = KernelState.initPhase1();
    s.setIommuNoCapDriverMode(.shadow);
    _ = try device_capabilities.iommuCapGrantStage2(&s, .Process1, .virtio_gpu, true, true, true);
    _ = try device_capabilities.queueCapGrantStage2(&s, .Process1, .virtio_gpu, 0, true, false);

    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));

    const mapping = try s.dmaMapCreateStage1(.Process1, .virtio_gpu, 0x1000, 128, .read);
    try std.testing.expect(device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));

    try s.dmaMapSetStateStage1(mapping, .in_flight);
    try s.dmaMapSetStateStage1(mapping, .completed);
    try s.dmaMapReleaseStage1(mapping);
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));

    const mapping2 = try s.dmaMapCreateStage1(.Process1, .virtio_gpu, 0x1000, 128, .read);
    try std.testing.expect(device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));
    try s.revokeCapTree(.Process1, 0x1000);
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));
    try std.testing.expect(s.dmaMapFindStage1(mapping2) == null);
}

test "iommu no-cap-driver does not map non-dma grant" {
    var s = KernelState.initPhase1();
    s.setIommuNoCapDriverMode(.shadow);
    _ = try device_capabilities.iommuCapGrantStage2(&s, .Process1, .virtio_gpu, true, true, true);
    _ = try device_capabilities.queueCapGrantStage2(&s, .Process1, .virtio_gpu, 0, true, false);

    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    });
    try std.testing.expectError(KernelError.NoDmaRight, s.dmaMapCreateStage1(.Process1, .virtio_gpu, 0x1000, 128, .read));
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));
}

test "queue cap grant syncs existing dma mapping" {
    var s = KernelState.initPhase1();
    s.setIommuNoCapDriverMode(.shadow);
    _ = try device_capabilities.iommuCapGrantStage2(&s, .Process1, .virtio_gpu, true, true, true);

    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });
    const mapping = try s.dmaMapCreateStage1(.Process1, .virtio_gpu, 0x1000, 128, .read);
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));

    const queue_token = try device_capabilities.queueCapGrantStage2(&s, .Process1, .virtio_gpu, 0, true, false);
    try std.testing.expect(device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));

    try device_capabilities.revokeDeviceCapStage2(&s, .Process1, .virtqueue, queue_token);
    try std.testing.expect(!device_capabilities.iommuHasMappingForPrincipalForTest(&s, .Process1, 0x1000));
    try std.testing.expect(s.dmaMapFindStage1(mapping) == null);
}

test "sendCapOnEndpoint rejects missing endpoint" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.EndpointNotFound, s.sendCapOnEndpoint(.Process0, 0xDEAD, 0x1000));
}

test "sendCapOnEndpoint enqueues mailbox for target" {
    var s = KernelState.initPhase1();
    try s.installEndpoint(.Process0, 0x11, .Process1);
    try s.sendCapOnEndpoint(.Process0, 0x11, 0x1000);
    const transfer_id = try s.recvCap(.Process1);
    try std.testing.expect(transfer_id >= cap_transfer_abi.transfer_id_min);
    try std.testing.expectEqual(transfer_id, try s.recvCap(.Process1));
    try std.testing.expectEqual(@as(u64, 0x1000), try s.acceptCapTransfer(.Process1, transfer_id));
}

test "acceptCapTransfer rejects mismatched transfer token" {
    var s = KernelState.initPhase1();
    try s.installEndpoint(.Process0, 0x11, .Process1);
    try s.sendCapOnEndpoint(.Process0, 0x11, 0x1000);
    const transfer_id = try s.recvCap(.Process1);
    try std.testing.expect(transfer_id >= cap_transfer_abi.transfer_id_min);
    try std.testing.expectError(KernelError.InvalidState, s.acceptCapTransfer(.Process1, transfer_id + 1));
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) != null);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) == null);
    try std.testing.expectEqual(transfer_id, try s.recvCap(.Process1));
    try std.testing.expectEqual(@as(u64, 0x1000), try s.acceptCapTransfer(.Process1, transfer_id));
}

test "recvCap returns MailboxEmpty when queue is empty" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.MailboxEmpty, s.recvCap(.Process1));
}

test "grantCap creates child and revokeCapTree at root removes descendants" {
    var s = KernelState.initPhase1();
    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) != null);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) != null);

    try s.revokeCapTree(.Process0, 0x1000);
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) == null);
}

test "grantCap requires grant right on source page capability" {
    var s = KernelState.initPhase1();
    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    });

    try std.testing.expectError(KernelError.InvalidState, s.grantCap(.Process1, .Process2, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    }));
}

test "sendCapOnEndpoint moves derived page capability after receiver accepts" {
    var s = KernelState.initPhase1();
    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    });
    try s.installEndpoint(.Process1, 0x11, .Process2);

    try s.sendCapOnEndpoint(.Process1, 0x11, 0x1000);
    const transfer_id = try s.recvCap(.Process2);
    try std.testing.expectEqual(@as(u64, 0x1000), try s.acceptCapTransfer(.Process2, transfer_id));
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) == null);
    const received = s.getTableConst(.Process2).find(0x1000).?;
    try std.testing.expect(received.rights.cpu_read);
    try std.testing.expect(!received.rights.cpu_write);
    try std.testing.expect(!received.rights.grant);
}

test "shareCapOnEndpoint keeps sender capability after receiver accepts" {
    var s = KernelState.initPhase1();
    try s.installEndpoint(.Process0, 0x11, .Process1);
    try s.shareCapOnEndpoint(.Process0, 0x11, 0x1000);
    const transfer_id = try s.recvCap(.Process1);
    try std.testing.expectEqual(@as(u64, 0x1000), try s.acceptCapTransfer(.Process1, transfer_id));
    const sender = s.getTableConst(.Process0).find(0x1000).?;
    const receiver = s.getTableConst(.Process1).find(0x1000).?;
    try std.testing.expect(sender.rights.cpu_read);
    try std.testing.expect(sender.rights.cpu_write);
    try std.testing.expect(sender.rights.grant);
    try std.testing.expect(receiver.rights.cpu_read);
    try std.testing.expect(receiver.rights.cpu_write);
    try std.testing.expect(receiver.rights.grant);
}

test "revokeCapTree from child only removes child subtree" {
    var s = KernelState.initPhase1();
    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });

    try s.revokeCapTree(.Process1, 0x1000);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) == null);
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) != null);
}

test "ensureProcessDescriptor updates label for reserved slot" {
    var s = KernelState.initPhase1();
    const fs_owner = processPrincipalFromIndex(initial_process_count) orelse unreachable;
    try std.testing.expect(!s.isActiveProcess(fs_owner));
    try std.testing.expect(s.ensureProcessDescriptor(fs_owner, "bootstrap-fs"));
    try std.testing.expect(s.isActiveProcess(fs_owner));
    try std.testing.expectEqualStrings("bootstrap-fs", s.processDescriptor(fs_owner).?.label);
    try std.testing.expect(s.ensureProcessDescriptor(.Process0, "bootstrap-owner"));
    try std.testing.expectEqualStrings("bootstrap-owner", s.processDescriptor(.Process0).?.label);
}

test "markProcessFaulted records fault status" {
    var s = KernelState.initPhase1();

    try std.testing.expect(s.markProcessFaulted(.Process1, 14));
    const status = s.processStatus(.Process1);
    try std.testing.expect(!status.active);
    try std.testing.expect(status.faulted);
    try std.testing.expectEqual(@as(u8, 14), status.fault_vector);
}

test "endpointTargetFor ignores inactive faulted target" {
    var s = KernelState.initPhase1();

    try s.installEndpoint(.Process0, 0x11, .Process1);
    try std.testing.expectEqual(@as(?PrincipalId, .Process1), s.endpointTargetFor(.Process0, 0x11));
    try std.testing.expect(s.markProcessFaulted(.Process1, 13));
    try std.testing.expectEqual(@as(?PrincipalId, null), s.endpointTargetFor(.Process0, 0x11));
}

test "endpointTargetFor falls back to published service endpoint" {
    var s = KernelState.initPhase1();

    try s.publishServiceEndpoint(0x80, .Process1);
    try std.testing.expectEqual(@as(?PrincipalId, .Process1), s.endpointTargetFor(.Process0, 0x80));
}

test "unpublishServiceEndpointsForTarget removes published endpoint" {
    var s = KernelState.initPhase1();

    try s.publishServiceEndpoint(0x80, .Process1);
    try std.testing.expect(s.unpublishServiceEndpointsForTarget(.Process1));
    try std.testing.expectEqual(@as(?PrincipalId, null), s.endpointTargetFor(.Process0, 0x80));
}
