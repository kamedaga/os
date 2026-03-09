const std = @import("std");
const builtin = @import("builtin");
const dma_mapping_manager = @import("dma_mapping_manager.zig");

pub const process_count: usize = 6;
pub const device_count: usize = 1;
pub const principal_count: usize = process_count + device_count;

comptime {
    if (process_count < 6) @compileError("process_count must be >= 6 for current boot role assignment");
    if (principal_count > std.math.maxInt(u8)) @compileError("principal_count must fit in u8");
}

fn PrincipalIdType(comptime process_slots: usize) type {
    var fields: [process_slots + device_count]std.builtin.Type.EnumField = undefined;
    inline for (0..process_slots) |i| {
        fields[i] = .{
            .name = std.fmt.comptimePrint("Process{}", .{i}),
            .value = i,
        };
    }
    fields[process_slots] = .{
        .name = "Device0",
        .value = process_slots,
    };

    return @Type(.{ .@"enum" = .{
        .tag_type = u8,
        .fields = &fields,
        .decls = &.{},
        .is_exhaustive = true,
    } });
}

pub const PrincipalId = PrincipalIdType(process_count);

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

pub const endpoint_to_process0: u64 = 0x10;
pub const endpoint_to_process1: u64 = 0x11;
pub const endpoint_to_process2: u64 = 0x12;

fn isProcessPrincipal(principal: PrincipalId) bool {
    return processIndexFromPrincipal(principal) != null;
}

pub const Rights = struct {
    cpu_read: bool,
    cpu_write: bool,
    dma: bool,
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

pub const Region = struct {
    id: u64,
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

pub const CapMailbox = struct {
    const max_items = 8;

    items: [max_items]u64 = undefined,
    len: usize = 0,

    pub fn push(self: *CapMailbox, paddr: u64) KernelError!void {
        if (self.len >= self.items.len) return KernelError.TableFull;
        self.items[self.len] = paddr;
        self.len += 1;
    }

    pub fn pop(self: *CapMailbox) ?u64 {
        if (self.len == 0) return null;
        const paddr = self.items[0];
        var i: usize = 1;
        while (i < self.len) : (i += 1) {
            self.items[i - 1] = self.items[i];
        }
        self.len -= 1;
        return paddr;
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

pub const FramebufferCapability = struct {
    paddr: u64,
    size_bytes: usize,
    width: u32,
    height: u32,
    pixels_per_scan_line: u32,
    pixel_format: u32,
    allow_draw: bool = false,
};

pub const WindowRights = packed struct(u16) {
    read_meta: bool = false,
    write_meta: bool = false,
    write_pixels: bool = false,
    control: bool = false,
    dma_pixels: bool = false,
    _reserved: u11 = 0,
};

pub const window_flag_allow_pixels_dma: u32 = 1 << 0;
pub const window_flag_low_scale: u32 = 1 << 1;

pub const WindowCap = packed struct {
    magic: u32, // 'WCAP'
    version: u16,
    rights_bits: u16,
    window_id: u32,
    owner_pid: u32,
    pixels_cap_paddr: u64,
    meta_cap_paddr: u64,
    pixels_size_bytes: u32,
    pixels_page_count: u16,
    pixels_per_scan_line: u16,
    pixel_format: u32,
    evt_cap_paddr: u64,
    width: u16,
    height: u16,
    min_width: u16,
    min_height: u16,
    flags: u32,
    z_hint: i32,
    reserved0: u32,
};

pub const WindowMeta = extern struct {
    magic: u32, // 'WMTA'
    version: u16,
    state: u16,
    seq: u64,
    pos_x: i32,
    pos_y: i32,
    width: u16,
    height: u16,
    title_len: u16,
    title: [64]u8,
};

const DmaRestoreEntry = struct {
    valid: bool = false,
    paddr: u64 = 0,
    rights: Rights = .{ .cpu_read = false, .cpu_write = false, .dma = false },
};

pub const WindowRecord = struct {
    active: bool = false,
    window_id: u32 = 0,
    owner: PrincipalId = .Process0,
    cap_paddr: u64 = 0,
    pixels_paddr: u64 = 0,
    meta_paddr: u64 = 0,
    pixels_size_bytes: u32 = 0,
    pixels_page_count: u16 = 0,
    pixels_per_scan_line: u16 = 0,
    width: u16 = 0,
    height: u16 = 0,
    flags: u32 = 0,
    z_hint: i32 = 0,
};

pub const CreateWindowResult = struct {
    window_cap_paddr: u64,
    meta_paddr: u64,
    pixels_first_paddr: u64,
    pixels_page_count: u16,
};

pub const OwnershipView = enum {
    Process0,
    Device0,
    Shared,
    None,
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
pub const QueueOperation = dma_mapping_manager.QueueOperation;
pub const QueueCapability = dma_mapping_manager.QueueCapability;

const IommuDevice = enum(u8) {
    virtio_gpu,
    virtio_input,
};

const IommuMapEntry = struct {
    valid: bool = false,
    device: IommuDevice = .virtio_gpu,
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
    pub const max_windows = 64;

    regions: [max_regions]Region = undefined,
    region_len: usize = 0,
    cap_tables: [principal_count]CNode = [_]CNode{.{}} ** principal_count,
    untyped_tables: [principal_count]UntypedCNode = [_]UntypedCNode{.{}} ** principal_count,
    endpoint_tables: [principal_count]EndpointCNode = [_]EndpointCNode{.{}} ** principal_count,
    cap_mailboxes: [principal_count]CapMailbox = [_]CapMailbox{.{}} ** principal_count,
    framebuffer_caps: [principal_count]?FramebufferCapability = [_]?FramebufferCapability{null} ** principal_count,
    pte_sync_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, paddr: u64) void = null,
    iommu_audit_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, paddr: u64, mapped: bool, reason: IommuSyncReason) void = null,
    revoke_queue: [max_total_caps]u64 = undefined,
    revoke_subtree: [max_total_caps]u64 = undefined,
    untyped_revoke_queue: [max_total_caps]u64 = undefined,
    untyped_revoke_subtree: [max_total_caps]u64 = undefined,
    dma_restore: [max_total_caps]DmaRestoreEntry = [_]DmaRestoreEntry{.{}} ** max_total_caps,
    dma_mappings: dma_mapping_manager.DmaMappingTable = .{},
    dma_device_domains: dma_mapping_manager.DeviceDomainTable = .{},
    queue_caps: dma_mapping_manager.QueueCapabilityTable = .{},
    iommu: IommuNoCapDriverState = .{},
    next_cap_id: u64 = 1,
    untyped_pool: UntypedPool = .{},
    windows: [max_windows]WindowRecord = [_]WindowRecord{.{}} ** max_windows,
    next_window_id: u32 = 1,

    fn allocCapId(self: *KernelState) u64 {
        const id = self.next_cap_id;
        self.next_cap_id +%= 1;
        return id;
    }

    fn pageAlignUp(value: u64) u64 {
        return (value + 4095) & ~@as(u64, 4095);
    }

    fn findDmaRestoreIndex(self: *const KernelState, paddr: u64) ?usize {
        var i: usize = 0;
        while (i < self.dma_restore.len) : (i += 1) {
            if (self.dma_restore[i].valid and self.dma_restore[i].paddr == paddr) return i;
        }
        return null;
    }

    fn saveDmaRestoreRights(self: *KernelState, paddr: u64, rights: Rights) KernelError!void {
        if (self.findDmaRestoreIndex(paddr) != null) return KernelError.InvalidState;

        var i: usize = 0;
        while (i < self.dma_restore.len) : (i += 1) {
            if (self.dma_restore[i].valid) continue;
            self.dma_restore[i] = .{
                .valid = true,
                .paddr = paddr,
                .rights = rights,
            };
            return;
        }
        return KernelError.TableFull;
    }

    fn takeDmaRestoreRights(self: *KernelState, paddr: u64) ?Rights {
        const index = self.findDmaRestoreIndex(paddr) orelse return null;
        const rights = self.dma_restore[index].rights;
        self.dma_restore[index] = .{};
        return rights;
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

    fn iommuDeviceForPrincipal(principal: PrincipalId) ?IommuDevice {
        return switch (principal) {
            .Process1 => .virtio_gpu,
            .Process0, .Process3 => .virtio_input,
            else => null,
        };
    }

    fn iommuFindMappingIndex(self: *const KernelState, device: IommuDevice, paddr: u64) ?usize {
        var i: usize = 0;
        while (i < self.iommu.mappings.len) : (i += 1) {
            const entry = self.iommu.mappings[i];
            if (!entry.valid) continue;
            if (entry.device == device and entry.paddr == paddr) return i;
        }
        return null;
    }

    fn iommuMap(self: *KernelState, device: IommuDevice, paddr: u64) KernelError!void {
        if (self.iommuFindMappingIndex(device, paddr) != null) return;
        var i: usize = 0;
        while (i < self.iommu.mappings.len) : (i += 1) {
            if (self.iommu.mappings[i].valid) continue;
            self.iommu.mappings[i] = .{
                .valid = true,
                .device = device,
                .paddr = paddr,
            };
            return;
        }
        return KernelError.TableFull;
    }

    fn iommuUnmap(self: *KernelState, device: IommuDevice, paddr: u64) void {
        const index = self.iommuFindMappingIndex(device, paddr) orelse return;
        self.iommu.mappings[index] = .{};
    }

    fn syncIommuForPrincipalPaddr(self: *KernelState, principal: PrincipalId, paddr: u64, reason: IommuSyncReason) KernelError!void {
        if (self.iommu.mode == .off) return;
        const device = iommuDeviceForPrincipal(principal) orelse return;
        const had_mapping = self.iommuFindMappingIndex(device, paddr) != null;
        const cap = self.getTableConst(principal).find(paddr);
        if (cap) |c| {
            if (c.rights.dma) {
                try self.iommuMap(device, paddr);
                if (!had_mapping) {
                    if (self.iommu_audit_hook) |hook| {
                        hook(self, principal, paddr, true, reason);
                    }
                }
                return;
            }
        }
        self.iommuUnmap(device, paddr);
        if (had_mapping) {
            if (self.iommu_audit_hook) |hook| {
                hook(self, principal, paddr, false, reason);
            }
        }
    }

    pub fn iommuHasMappingForPrincipalForTest(self: *const KernelState, principal: PrincipalId, paddr: u64) bool {
        const device = iommuDeviceForPrincipal(principal) orelse return false;
        return self.iommuFindMappingIndex(device, paddr) != null;
    }

    pub fn allocWindowSlot(self: *KernelState) ?usize {
        var i: usize = 0;
        while (i < self.windows.len) : (i += 1) {
            if (!self.windows[i].active) return i;
        }
        return null;
    }

    pub fn allocWindowId(self: *KernelState) u32 {
        const id = self.next_window_id;
        self.next_window_id +%= 1;
        return id;
    }

    pub fn createWindow(
        self: *KernelState,
        owner: PrincipalId,
        compositor: PrincipalId,
        free_list: *FreePageList,
        width: u16,
        height: u16,
        flags: u32,
    ) KernelError!CreateWindowResult {
        if (!isProcessPrincipal(owner) or !isProcessPrincipal(compositor)) return KernelError.InvalidState;
        if (width == 0 or height == 0) return KernelError.InvalidState;
        if (width > 2048 or height > 2048) return KernelError.InvalidState;

        const slot = self.allocWindowSlot() orelse return KernelError.TableFull;
        const allow_pixel_dma = (flags & window_flag_allow_pixels_dma) != 0;

        const pitch: u16 = width;
        const pixel_count = @as(u64, width) * @as(u64, height);
        const bytes_unaligned = pixel_count * 4;
        if (bytes_unaligned == 0 or bytes_unaligned > (16 * 1024 * 1024)) return KernelError.InvalidState;
        const bytes_aligned = pageAlignUp(bytes_unaligned);
        const page_count_u64 = bytes_aligned / 4096;
        if (page_count_u64 == 0 or page_count_u64 > 1024) return KernelError.InvalidState;
        const page_count: usize = @intCast(page_count_u64);

        var bookkeeping_pages: [2]PageCapability = undefined;
        if (self.findOwnedUntypedForPages(owner, 2, true)) |block_id| {
            try self.retypeUntypedToPages(owner, block_id, 2, true, bookkeeping_pages[0..]);
        } else {
            bookkeeping_pages[0] = try self.allocPageTo(owner, free_list);
            bookkeeping_pages[1] = try self.allocPageTo(owner, free_list);
        }
        const window_cap_page = bookkeeping_pages[0];
        const meta_page = bookkeeping_pages[1];

        const pixels_first_paddr: u64 = 0;

        try self.grantCap(owner, compositor, meta_page.paddr, .{
            .cpu_read = true,
            .cpu_write = false,
            .dma = false,
        });

        const window_id = self.allocWindowId();
        const rights = WindowRights{
            .read_meta = true,
            .write_meta = true,
            .write_pixels = true,
            .control = true,
            .dma_pixels = allow_pixel_dma,
        };
        const cap_bytes: [*]volatile u8 = @ptrFromInt(window_cap_page.paddr);
        @memset(cap_bytes[0..4096], 0);
        const cap_view: *volatile WindowCap = @ptrFromInt(window_cap_page.paddr);
        cap_view.* = .{
            .magic = 0x57434150, // 'WCAP'
            .version = 1,
            .rights_bits = @bitCast(rights),
            .window_id = window_id,
            .owner_pid = @intFromEnum(owner),
            .pixels_cap_paddr = pixels_first_paddr,
            .meta_cap_paddr = meta_page.paddr,
            .pixels_size_bytes = @intCast(bytes_aligned),
            .pixels_page_count = @intCast(page_count),
            .pixels_per_scan_line = pitch,
            .pixel_format = 0,
            .evt_cap_paddr = 0,
            .width = width,
            .height = height,
            .min_width = 16,
            .min_height = 16,
            .flags = flags,
            .z_hint = 0,
            .reserved0 = 0,
        };

        const meta_bytes: [*]volatile u8 = @ptrFromInt(meta_page.paddr);
        @memset(meta_bytes[0..4096], 0);
        const meta_view: *volatile WindowMeta = @ptrFromInt(meta_page.paddr);
        meta_view.* = .{
            .magic = 0x574D5441, // 'WMTA'
            .version = 1,
            .state = 1,
            .seq = 1,
            .pos_x = 64,
            .pos_y = 64,
            .width = width,
            .height = height,
            .title_len = 0,
            .title = [_]u8{0} ** 64,
        };

        self.windows[slot] = .{
            .active = true,
            .window_id = window_id,
            .owner = owner,
            .cap_paddr = window_cap_page.paddr,
            .pixels_paddr = pixels_first_paddr,
            .meta_paddr = meta_page.paddr,
            .pixels_size_bytes = @intCast(bytes_aligned),
            .pixels_page_count = @intCast(page_count),
            .pixels_per_scan_line = pitch,
            .width = width,
            .height = height,
            .flags = flags,
            .z_hint = 0,
        };

        return .{
            .window_cap_paddr = window_cap_page.paddr,
            .meta_paddr = meta_page.paddr,
            .pixels_first_paddr = pixels_first_paddr,
            .pixels_page_count = @intCast(page_count),
        };
    }

    fn isRightsSubset(child: Rights, parent: Rights) bool {
        return (!child.cpu_read or parent.cpu_read) and
            (!child.cpu_write or parent.cpu_write) and
            (!child.dma or parent.dma);
    }

    fn processPrincipal(index: usize) PrincipalId {
        return processPrincipalFromIndex(index) orelse unreachable;
    }

    fn initPrincipalState(self: *KernelState) void {
        var i: usize = 0;
        while (i < principal_count) : (i += 1) {
            self.cap_tables[i] = .{};
            self.endpoint_tables[i] = .{};
            self.cap_mailboxes[i] = .{};
        }
    }

    fn installDefaultEndpoints(self: *KernelState) KernelError!void {
        const p0 = processPrincipal(0);
        const p1 = processPrincipal(1);
        try self.endpoint_tables[@intFromEnum(p0)].add(.{
            .endpoint_id = endpoint_to_process1,
            .target = p1,
        });
        try self.endpoint_tables[@intFromEnum(p1)].add(.{
            .endpoint_id = endpoint_to_process0,
            .target = p0,
        });

        var i: usize = 2;
        while (i < process_count) : (i += 1) {
            const proc = processPrincipal(i);
            try self.endpoint_tables[@intFromEnum(proc)].add(.{
                .endpoint_id = endpoint_to_process1,
                .target = p1,
            });
        }
    }

    pub fn initPhase1() KernelState {
        var state = KernelState{};
        state.next_cap_id = 1;
        state.regions[0] = .{
            .id = 0,
        };
        state.region_len = 1;
        state.initPrincipalState();
        state.installDefaultEndpoints() catch unreachable;

        // Process0 initially owns region0 and has read + dma.
        const p0 = processPrincipal(0);
        const root_id = state.allocCapId();
        state.cap_tables[@intFromEnum(p0)].add(.{
            .paddr = 0x1000,
            .rights = .{ .cpu_read = true, .cpu_write = true, .dma = true },
            .cap_id = root_id,
            .root_cap_id = root_id,
            .parent_cap_id = 0,
        }) catch unreachable;

        return state;
    }

    pub fn initFromDetectedRegions(region_count: usize) KernelError!KernelState {
        if (region_count == 0) return KernelError.EmptyRegionSet;
        if (region_count > max_regions) return KernelError.TooManyRegions;

        var state = KernelState{};
        state.next_cap_id = 1;
        state.initPrincipalState();
        try state.installDefaultEndpoints();

        var i: usize = 0;
        while (i < region_count) : (i += 1) {
            state.regions[i] = .{
                .id = i,
            };
        }
        state.region_len = region_count;

        return state;
    }

    pub fn startDma(self: *KernelState, paddr: u64) KernelError!void {
        const p0_table = self.getTable(.Process0);
        const p0_cap = p0_table.find(paddr) orelse return KernelError.CapabilityNotFound;
        if (!p0_cap.rights.dma) return KernelError.NoDmaRight;
        try self.saveDmaRestoreRights(paddr, p0_cap.rights);

        // DMA 開始時は moveCap 経由で Device0 へ委譲する。
        try self.moveCap(
            .Process0,
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
        if (self.getTable(.Process0).find(paddr) != null) return KernelError.InvalidState;

        const dev_table = self.getTable(.Device0);
        const dev_cap = dev_table.find(paddr) orelse return KernelError.CapabilityNotFound;
        if (!dev_cap.rights.dma) return KernelError.NoDmaRight;
        const restore_rights = self.takeDmaRestoreRights(paddr) orelse return KernelError.InvalidState;
        if (self.getTable(.Process0).find(paddr) != null) return KernelError.InvalidState;

        var restored = dev_cap.*;
        restored.rights = restore_rights;
        _ = dev_table.removeByPaddr(paddr);
        try self.getTable(.Process0).add(restored);
        if (self.pte_sync_hook) |hook| {
            hook(self, .Device0, paddr);
            hook(self, .Process0, paddr);
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
        if (!isProcessPrincipal(owner)) return KernelError.InvalidState;
        return self.dma_mappings.alloc(
            @intFromEnum(owner),
            device,
            paddr_start,
            length,
            direction,
        ) catch |err| switch (err) {
            error.InvalidState => KernelError.InvalidState,
            error.TableFull => KernelError.TableFull,
            else => KernelError.InvalidState,
        };
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
        self.dma_mappings.release(token) catch |err| switch (err) {
            error.NotFound => return KernelError.CapabilityNotFound,
            error.InvalidState => return KernelError.InvalidState,
            error.TableFull => return KernelError.TableFull,
            error.Denied => return KernelError.InvalidState,
        };
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

    pub fn queueCapGrantStage2(
        self: *KernelState,
        owner: PrincipalId,
        device: DmaDeviceId,
        queue_index: u16,
        allow_submit: bool,
        allow_notify: bool,
    ) KernelError!u64 {
        if (!isProcessPrincipal(owner)) return KernelError.InvalidState;
        return self.queue_caps.alloc(
            @intFromEnum(owner),
            device,
            queue_index,
            allow_submit,
            allow_notify,
        ) catch |err| switch (err) {
            error.InvalidState => KernelError.InvalidState,
            error.TableFull => KernelError.TableFull,
            else => KernelError.InvalidState,
        };
    }

    pub fn queueCapAuthorizeStage2(
        self: *const KernelState,
        owner: PrincipalId,
        token: u64,
        device: DmaDeviceId,
        queue_index: u16,
        op: QueueOperation,
    ) KernelError!void {
        if (!isProcessPrincipal(owner)) return KernelError.InvalidState;
        self.queue_caps.authorize(
            @intFromEnum(owner),
            token,
            device,
            queue_index,
            op,
        ) catch |err| switch (err) {
            error.NotFound => return KernelError.CapabilityNotFound,
            error.Denied => return KernelError.InvalidState,
            error.InvalidState => return KernelError.InvalidState,
            error.TableFull => return KernelError.TableFull,
        };
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
        return &self.cap_tables[@intFromEnum(principal)];
    }

    pub fn getTableConst(self: *const KernelState, principal: PrincipalId) *const CNode {
        return &self.cap_tables[@intFromEnum(principal)];
    }

    pub fn getUntypedTable(self: *KernelState, principal: PrincipalId) *UntypedCNode {
        return &self.untyped_tables[@intFromEnum(principal)];
    }

    pub fn getUntypedTableConst(self: *const KernelState, principal: PrincipalId) *const UntypedCNode {
        return &self.untyped_tables[@intFromEnum(principal)];
    }

    pub fn getEndpointTable(self: *KernelState, principal: PrincipalId) *EndpointCNode {
        return &self.endpoint_tables[@intFromEnum(principal)];
    }

    pub fn getEndpointTableConst(self: *const KernelState, principal: PrincipalId) *const EndpointCNode {
        return &self.endpoint_tables[@intFromEnum(principal)];
    }

    pub fn endpointTargetFor(self: *const KernelState, owner: PrincipalId, endpoint_id: u64) ?PrincipalId {
        const ep = self.getEndpointTableConst(owner).find(endpoint_id) orelse return null;
        return ep.target;
    }

    pub fn grantFramebufferCap(
        self: *KernelState,
        to: PrincipalId,
        framebuffer: FramebufferCapability,
    ) KernelError!void {
        if (!isProcessPrincipal(to)) return KernelError.InvalidState;
        if (framebuffer.size_bytes == 0) return KernelError.InvalidState;

        const size_minus_one = framebuffer.size_bytes - 1;
        const paddr_overflow = @addWithOverflow(framebuffer.paddr, @as(u64, @intCast(size_minus_one)))[1];
        if (paddr_overflow != 0) return KernelError.InvalidState;

        self.framebuffer_caps[@intFromEnum(to)] = framebuffer;
    }

    pub fn getFramebufferCap(self: *const KernelState, principal: PrincipalId) ?FramebufferCapability {
        if (!isProcessPrincipal(principal)) return null;
        return self.framebuffer_caps[@intFromEnum(principal)];
    }

    pub fn canDrawToFramebuffer(
        self: *const KernelState,
        principal: PrincipalId,
        paddr: u64,
        size_bytes: usize,
        writable: bool,
    ) bool {
        if (!writable) return false;
        if (size_bytes == 0) return false;

        const framebuffer = self.getFramebufferCap(principal) orelse return false;
        if (!framebuffer.allow_draw) return false;

        const map_end, const map_overflow = @addWithOverflow(paddr, @as(u64, @intCast(size_bytes - 1)));
        if (map_overflow != 0) return false;
        const fb_end, const fb_overflow = @addWithOverflow(framebuffer.paddr, @as(u64, @intCast(framebuffer.size_bytes - 1)));
        if (fb_overflow != 0) return false;

        return paddr >= framebuffer.paddr and map_end <= fb_end;
    }

    pub fn scanCapTables(self: *const KernelState, paddr: u64) OwnershipView {
        // デバッグ用: capability 走査から論理的な保持者ビューを作る。
        const p0_has = self.getTableConst(.Process0).find(paddr) != null;
        const dev_has = self.getTableConst(.Device0).find(paddr) != null;

        if (p0_has and dev_has) return .Shared;
        if (p0_has) return .Process0;
        if (dev_has) return .Device0;
        return .None;
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
        while (true) {
            const paddr = try free_list.popBack();
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
        const cap = try self.allocPage(requester, free_list);
        if (!builtin.is_test) {
            const page_bytes: [*]u8 = @ptrFromInt(cap.paddr);
            @memset(page_bytes[0..4096], 0);
        }
        const root_id = self.allocCapId();
        try self.getTable(requester).add(.{
            .paddr = cap.paddr,
            .rights = .{
                .cpu_read = true,
                .cpu_write = true,
                .dma = true,
            },
            .cap_id = root_id,
            .root_cap_id = root_id,
            .parent_cap_id = 0,
        });
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
        if (!isProcessPrincipal(owner)) return KernelError.InvalidState;
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
        if (!isProcessPrincipal(owner)) return null;
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
        if (!isProcessPrincipal(owner)) return KernelError.InvalidState;
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
        if (!isProcessPrincipal(from) or !isProcessPrincipal(to)) return KernelError.InvalidState;
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
        if (!isProcessPrincipal(from) or !isProcessPrincipal(to)) return KernelError.InvalidState;
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
        if (!isProcessPrincipal(owner)) return KernelError.InvalidState;
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
        if (!isProcessPrincipal(owner)) return KernelError.InvalidState;
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
                    try self.syncIommuForPrincipalPaddr(@enumFromInt(pidx), removed_paddr, .revoke);
                    if (self.pte_sync_hook) |hook| {
                        hook(self, @enumFromInt(pidx), removed_paddr);
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
            hook(self, owner, paddr);
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

        const src = self.getTable(from);
        const src_cap = src.find(paddr) orelse return KernelError.CapabilityNotFound;
        if (!isRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;
        if (self.getTable(to).find(paddr) != null) return KernelError.InvalidState;

        var moved = src_cap.*;
        moved.rights = rights;
        _ = src.removeByPaddr(paddr);
        try self.getTable(to).add(moved);
        try self.syncIommuForPrincipalPaddr(from, paddr, .move_from);
        try self.syncIommuForPrincipalPaddr(to, paddr, .move_to);
        if (self.pte_sync_hook) |hook| {
            hook(self, from, paddr);
            hook(self, to, paddr);
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
        if (!isProcessPrincipal(from) or !isProcessPrincipal(to)) return KernelError.InvalidState;

        const src_cap = self.getTableConst(from).find(paddr) orelse return KernelError.CapabilityNotFound;
        if (!isRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;
        if (self.getTable(to).find(paddr) != null) return KernelError.InvalidState;

        const child_id = self.allocCapId();
        try self.getTable(to).add(.{
            .paddr = paddr,
            .rights = rights,
            .cap_id = child_id,
            .root_cap_id = src_cap.root_cap_id,
            .parent_cap_id = src_cap.cap_id,
        });
        try self.syncIommuForPrincipalPaddr(to, paddr, if (rights.dma) .grant_dma else .grant_no_dma);
    }

    pub fn grantCapsBatch(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        paddrs: []const u64,
        rights: Rights,
    ) KernelError!void {
        if (from == to) return KernelError.InvalidState;
        if (!isProcessPrincipal(from) or !isProcessPrincipal(to)) return KernelError.InvalidState;
        if (paddrs.len == 0) return KernelError.InvalidState;
        var i: usize = 0;
        while (i < paddrs.len) : (i += 1) {
            const paddr = paddrs[i];
            const src_cap = self.getTableConst(from).find(paddr) orelse return KernelError.CapabilityNotFound;
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
            try self.syncIommuForPrincipalPaddr(to, paddr, if (rights.dma) .grant_dma else .grant_no_dma);
        }
    }

    pub fn sendCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        paddr: u64,
    ) KernelError!void {
        if (from == to) return KernelError.InvalidState;
        if (!isProcessPrincipal(from) or !isProcessPrincipal(to)) return KernelError.InvalidState;

        const src_cap = self.getTableConst(from).find(paddr) orelse return KernelError.CapabilityNotFound;
        try self.moveCap(from, to, paddr, src_cap.rights);
    }

    pub fn sendCapOnEndpoint(
        self: *KernelState,
        from: PrincipalId,
        endpoint_id: u64,
        paddr: u64,
    ) KernelError!void {
        if (!isProcessPrincipal(from)) return KernelError.InvalidState;
        const target = self.endpointTargetFor(from, endpoint_id) orelse return KernelError.EndpointNotFound;
        try self.sendCap(from, target, paddr);
        try self.cap_mailboxes[@intFromEnum(target)].push(paddr);
    }

    pub fn recvCap(self: *KernelState, receiver: PrincipalId) KernelError!u64 {
        if (!isProcessPrincipal(receiver)) return KernelError.InvalidState;
        return self.cap_mailboxes[@intFromEnum(receiver)].pop() orelse KernelError.MailboxEmpty;
    }

    pub fn revokeCapTree(
        self: *KernelState,
        owner: PrincipalId,
        paddr: u64,
    ) KernelError!void {
        if (!isProcessPrincipal(owner)) return KernelError.InvalidState;
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
                try self.syncIommuForPrincipalPaddr(@enumFromInt(pidx), removed_paddr, .revoke);
                if (self.pte_sync_hook) |hook| {
                    hook(self, @enumFromInt(pidx), removed_paddr);
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
    const submit_token = try s.queueCapGrantStage2(.Process1, .virtio_gpu, 0, true, false);
    const notify_token = try s.queueCapGrantStage2(.Process1, .virtio_gpu, 0, false, true);

    try s.queueCapAuthorizeStage2(.Process1, submit_token, .virtio_gpu, 0, .submit);
    try s.queueCapAuthorizeStage2(.Process1, notify_token, .virtio_gpu, 0, .notify);

    try std.testing.expectError(KernelError.InvalidState, s.queueCapAuthorizeStage2(.Process1, submit_token, .virtio_gpu, 0, .notify));
    try std.testing.expectError(KernelError.InvalidState, s.queueCapAuthorizeStage2(.Process1, notify_token, .virtio_gpu, 0, .submit));
}

test "queue cap stage2 rejects owner mismatch" {
    var s = KernelState.initPhase1();
    const token = try s.queueCapGrantStage2(.Process1, .virtio_gpu, 0, true, true);
    try std.testing.expectError(KernelError.InvalidState, s.queueCapAuthorizeStage2(.Process0, token, .virtio_gpu, 0, .submit));
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
    try std.testing.expectEqual(OwnershipView.Process0, s.scanCapTables(0x1000));

    const p0 = s.getTableConst(.Process0);
    try std.testing.expectEqual(@as(usize, 1), p0.len);
    try std.testing.expect(p0.find(0x1000).?.rights.cpu_read);
    try std.testing.expect(p0.find(0x1000).?.rights.cpu_write);
    try std.testing.expect(p0.find(0x1000).?.rights.dma);

    const dev = s.getTableConst(.Device0);
    try std.testing.expectEqual(@as(usize, 0), dev.len);
}

test "start dma moves owner and capabilities" {
    var s = KernelState.initPhase1();
    try s.startDma(0x1000);

    try std.testing.expectEqual(OwnershipView.Device0, s.scanCapTables(0x1000));
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    const dev_cap = s.getTableConst(.Device0).find(0x1000).?;
    try std.testing.expect(dev_cap.rights.dma);
    try std.testing.expect(!dev_cap.rights.cpu_read);
    try std.testing.expect(!dev_cap.rights.cpu_write);
}

test "complete dma returns owner and capabilities" {
    var s = KernelState.initPhase1();
    try s.startDma(0x1000);
    try s.completeDma(0x1000);

    try std.testing.expectEqual(OwnershipView.Process0, s.scanCapTables(0x1000));
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

    try s.startDma(0x1000);
    try s.completeDma(0x1000);

    const p0_cap = s.getTableConst(.Process0).find(0x1000).?;
    try std.testing.expect(p0_cap.rights.cpu_read);
    try std.testing.expect(!p0_cap.rights.cpu_write);
    try std.testing.expect(p0_cap.rights.dma);
}

test "invalid transition rejected" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.InvalidState, s.completeDma(0x1000));
    try s.startDma(0x1000);
    try std.testing.expectError(KernelError.CapabilityNotFound, s.startDma(0x1000));
}

test "init from detected regions" {
    const s = try KernelState.initFromDetectedRegions(3);
    try std.testing.expectEqual(@as(usize, 3), s.region_len);
    try std.testing.expectEqual(OwnershipView.None, s.scanCapTables(0x3000));
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
    try std.testing.expect(s.getTableConst(.Process0).find(0x9000) != null);
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
    try std.testing.expect(s.getTableConst(.Process0).find(caps[0].paddr) != null);
    try std.testing.expect(s.getTableConst(.Process0).find(caps[1].paddr) != null);
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
    try std.testing.expectEqual(OwnershipView.Device0, s.scanCapTables(0x1000));
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
}

test "sendCap rejects non-process endpoints" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.InvalidState, s.sendCap(.Process0, .Device0, 0x1000));
    try std.testing.expectError(KernelError.InvalidState, s.sendCap(.Device0, .Process1, 0x1000));
}

test "sendCapOnEndpoint requires endpoint capability" {
    var s = KernelState.initPhase1();
    try s.sendCapOnEndpoint(.Process0, endpoint_to_process1, 0x1000);
    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    try std.testing.expect(s.getTableConst(.Process1).find(0x1000) != null);
}

test "iommu no-cap-driver shadow maps dma grant to compositor" {
    var s = KernelState.initPhase1();
    s.setIommuNoCapDriverMode(.shadow);

    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });
    try std.testing.expect(s.iommuHasMappingForPrincipalForTest(.Process1, 0x1000));

    try s.revokeCapTree(.Process1, 0x1000);
    try std.testing.expect(!s.iommuHasMappingForPrincipalForTest(.Process1, 0x1000));
}

test "iommu no-cap-driver does not map non-dma grant" {
    var s = KernelState.initPhase1();
    s.setIommuNoCapDriverMode(.shadow);

    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    });
    try std.testing.expect(!s.iommuHasMappingForPrincipalForTest(.Process1, 0x1000));
}

test "sendCapOnEndpoint rejects missing endpoint" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.EndpointNotFound, s.sendCapOnEndpoint(.Process0, 0xDEAD, 0x1000));
}

test "sendCapOnEndpoint enqueues mailbox for target" {
    var s = KernelState.initPhase1();
    try s.sendCapOnEndpoint(.Process0, endpoint_to_process1, 0x1000);
    try std.testing.expectEqual(@as(u64, 0x1000), try s.recvCap(.Process1));
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

test "framebuffer capability grants draw right only to owner process" {
    var s = try KernelState.initFromDetectedRegions(1);
    const fb_cap = FramebufferCapability{
        .paddr = 0x8000_0000,
        .size_bytes = 0x20_0000,
        .width = 1024,
        .height = 768,
        .pixels_per_scan_line = 1024,
        .pixel_format = 0,
        .allow_draw = true,
    };

    try s.grantFramebufferCap(.Process0, fb_cap);
    try std.testing.expect(s.canDrawToFramebuffer(.Process0, 0x8000_1000, 0x1000, true));
    try std.testing.expect(!s.canDrawToFramebuffer(.Process1, 0x8000_1000, 0x1000, true));
    try std.testing.expect(!s.canDrawToFramebuffer(.Process0, 0x7FFF_F000, 0x2000, true));
    try std.testing.expect(!s.canDrawToFramebuffer(.Process0, 0x8000_1000, 0x1000, false));
}
