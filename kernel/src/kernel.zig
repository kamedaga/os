const std = @import("std");
const builtin = @import("builtin");
const capability = @import("capability.zig");
const dma_mapping_manager = @import("dma_mapping_manager.zig");
const device_abi = @import("abi/device_abi.zig");
const debug_window_force_free_list = true;

pub const initial_process_count: usize = 8;
pub const process_count: usize = 32;
pub const max_thread_slots: usize = 16;
pub const device_count: usize = 1;
pub const principal_count: usize = process_count + device_count;

comptime {
    if (initial_process_count < 8) @compileError("initial_process_count must be >= 8 for current boot role assignment");
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

pub const PrincipalId = PrincipalIdType(initial_process_count);
pub const vfs_process_index: usize = 8;
pub const vfs_principal: PrincipalId = @enumFromInt(vfs_process_index);

comptime {
    if (vfs_process_index >= process_count) @compileError("vfs_process_index must be < process_count");
    if (vfs_process_index < initial_process_count) @compileError("vfs_process_index must stay outside the initial boot role set");
}

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
    if (isVfsPrincipal(principal)) return "VFS";
    if (processIndexFromPrincipal(principal)) |index| {
        return process_labels[index];
    }
    if (principal == .Device0) return "Device0";
    return "Unknown";
}

pub const endpoint_to_process0: u64 = 0x10;
pub const endpoint_to_boot_display: u64 = 0x11;
pub const endpoint_to_process1: u64 = endpoint_to_boot_display;
pub const endpoint_to_process2: u64 = 0x12;
pub const endpoint_to_vfs: u64 = 0x13;
pub const endpoint_to_spawn_parent: u64 = 0x14;

fn isProcessPrincipal(principal: PrincipalId) bool {
    return processIndexFromPrincipal(principal) != null;
}

pub fn isVfsPrincipal(principal: PrincipalId) bool {
    return @intFromEnum(principal) == vfs_process_index;
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

pub const ProcessDescriptor = struct {
    active: bool = false,
    principal: PrincipalId = @enumFromInt(0),
    label: []const u8 = "",
};

pub const KernelError = error{
    RegionNotFound,
    CapabilityNotFound,
    FsCapabilityNotFound,
    VmObjectCapabilityNotFound,
    ExecImageCapabilityNotFound,
    EndpointNotFound,
    MailboxEmpty,
    FsMailboxEmpty,
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

pub const FsObjectKind = enum(u8) {
    none = 0,
    mount = 1,
    vnode_dir = 2,
    vnode_file = 3,
    open_file = 4,
    exec = 5,
};

pub const FsRights = packed struct(u32) {
    lookup: bool = false,
    read: bool = false,
    write: bool = false,
    readdir: bool = false,
    stat: bool = false,
    create: bool = false,
    unlink: bool = false,
    rename: bool = false,
    exec: bool = false,
    mount: bool = false,
    grant: bool = false,
    admin: bool = false,
    _reserved: u20 = 0,
};

pub const FsCapability = struct {
    object_id: u64,
    kind: FsObjectKind,
    rights: FsRights,
    cap_id: u64,
    root_cap_id: u64,
    parent_cap_id: u64,
};

pub const FsCNode = struct {
    pub const max_caps = 256;

    caps: [max_caps]FsCapability = undefined,
    len: usize = 0,

    pub fn add(self: *FsCNode, cap: FsCapability) KernelError!void {
        if (self.findByCapId(cap.cap_id) != null) return KernelError.InvalidState;
        if (self.len >= self.caps.len) return KernelError.TableFull;
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn findByCapId(self: *const FsCNode, cap_id: u64) ?*const FsCapability {
        if (self.findIndexByCapId(cap_id)) |index| {
            return &self.caps[index];
        }
        return null;
    }

    pub fn removeByCapId(self: *FsCNode, cap_id: u64) bool {
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

    fn findIndexByCapId(self: *const FsCNode, cap_id: u64) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            if (self.caps[i].cap_id == cap_id) return i;
        }
        return null;
    }
};

pub const FsMailbox = struct {
    const max_items = 8;

    items: [max_items]u64 = undefined,
    len: usize = 0,

    pub fn push(self: *FsMailbox, cap_id: u64) KernelError!void {
        if (self.len >= self.items.len) return KernelError.TableFull;
        self.items[self.len] = cap_id;
        self.len += 1;
    }

    pub fn pop(self: *FsMailbox) ?u64 {
        if (self.len == 0) return null;
        const cap_id = self.items[0];
        var i: usize = 1;
        while (i < self.len) : (i += 1) {
            self.items[i - 1] = self.items[i];
        }
        self.len -= 1;
        return cap_id;
    }
};

pub const max_image_backing_pages: usize = 128;

pub const VmObjectRights = packed struct(u32) {
    read: bool = false,
    map: bool = false,
    grant: bool = false,
    _reserved: u29 = 0,
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
    dirty_x: u16,
    dirty_y: u16,
    dirty_w: u16,
    dirty_h: u16,
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

pub const DebugWindowStage = enum(u8) {
    after_alloc,
    after_grant_meta,
    after_cap_init,
    after_meta_init,
    after_record_write,
};

pub const DebugAllocPageStage = enum(u8) {
    after_pop,
    after_memset,
    after_cap_add,
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
    principal_devices: [principal_count]?IommuDevice = [_]?IommuDevice{null} ** principal_count,
};

pub const KernelState = struct {
    pub const max_regions = 256;
    const max_total_caps = principal_count * CNode.max_caps;
    pub const max_windows = 64;

    regions: [max_regions]Region = undefined,
    region_len: usize = 0,
    process_descriptors: [process_count]ProcessDescriptor = [_]ProcessDescriptor{.{}} ** process_count,
    active_process_count: usize = 0,
    cap_tables: [principal_count]CNode = [_]CNode{.{}} ** principal_count,
    untyped_tables: [principal_count]UntypedCNode = [_]UntypedCNode{.{}} ** principal_count,
    endpoint_tables: [principal_count]EndpointCNode = [_]EndpointCNode{.{}} ** principal_count,
    cap_mailboxes: [principal_count]CapMailbox = [_]CapMailbox{.{}} ** principal_count,
    framebuffer_caps: [principal_count]?FramebufferCapability = [_]?FramebufferCapability{null} ** principal_count,
    fs_tables: [principal_count]FsCNode = [_]FsCNode{.{}} ** principal_count,
    fs_mailboxes: [principal_count]FsMailbox = [_]FsMailbox{.{}} ** principal_count,
    vm_object_tables: [principal_count]VmObjectCNode = [_]VmObjectCNode{.{}} ** principal_count,
    exec_image_tables: [principal_count]ExecImageCNode = [_]ExecImageCNode{.{}} ** principal_count,
    pte_sync_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, paddr: u64) void = null,
    iommu_audit_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, paddr: u64, mapped: bool, reason: IommuSyncReason) void = null,
    debug_window_hook: ?*const fn (state: *const KernelState, owner: PrincipalId, stage: DebugWindowStage, slot: usize, cap_paddr: u64, meta_paddr: u64) void = null,
    debug_alloc_page_hook: ?*const fn (state: *const KernelState, requester: PrincipalId, stage: DebugAllocPageStage, paddr: u64) void = null,
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
            self.iommu.principal_devices = [_]?IommuDevice{null} ** principal_count;
        }
    }

    pub fn getIommuNoCapDriverMode(self: *const KernelState) IommuNoCapDriverMode {
        return self.iommu.mode;
    }

    fn iommuDeviceFromDmaDeviceId(device: DmaDeviceId) IommuDevice {
        const abi_device: device_abi.DeviceId = switch (device) {
            .virtio_gpu => .virtio_gpu,
            .virtio_input => .virtio_input,
        };
        return switch (abi_device) {
            .virtio_gpu => .virtio_gpu,
            .virtio_input => .virtio_input,
        };
    }

    fn iommuDeviceForPrincipal(self: *const KernelState, principal: PrincipalId) ?IommuDevice {
        const index = @intFromEnum(principal);
        if (index >= self.iommu.principal_devices.len) return null;
        return self.iommu.principal_devices[index];
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

    noinline fn callIommuAuditHook(
        hook: *const fn (state: *const KernelState, principal: PrincipalId, paddr: u64, mapped: bool, reason: IommuSyncReason) void,
        self: *const KernelState,
        principal: PrincipalId,
        paddr: u64,
        mapped: bool,
        reason: IommuSyncReason,
    ) void {
        hook(self, principal, paddr, mapped, reason);
    }

    noinline fn callPteSyncHook(
        hook: *const fn (state: *const KernelState, principal: PrincipalId, paddr: u64) void,
        self: *const KernelState,
        principal: PrincipalId,
        paddr: u64,
    ) void {
        hook(self, principal, paddr);
    }

    noinline fn syncIommuForPrincipalPaddr(self: *KernelState, principal: PrincipalId, paddr: u64, reason: IommuSyncReason) KernelError!void {
        if (self.iommu.mode == .off) return;
        const device = self.iommuDeviceForPrincipal(principal) orelse return;
        const had_mapping = self.iommuFindMappingIndex(device, paddr) != null;
        const cap = self.getTableConst(principal).find(paddr);
        if (cap) |c| {
            if (c.rights.dma) {
                try self.iommuMap(device, paddr);
                if (!had_mapping) {
                    if (self.iommu_audit_hook) |hook| {
                        callIommuAuditHook(hook, self, principal, paddr, true, reason);
                    }
                }
                return;
            }
        }
        self.iommuUnmap(device, paddr);
        if (had_mapping) {
            if (self.iommu_audit_hook) |hook| {
                callIommuAuditHook(hook, self, principal, paddr, false, reason);
            }
        }
    }

    pub fn iommuHasMappingForPrincipalForTest(self: *const KernelState, principal: PrincipalId, paddr: u64) bool {
        const device = self.iommuDeviceForPrincipal(principal) orelse return false;
        return self.iommuFindMappingIndex(device, paddr) != null;
    }

    fn unmapAllIommuForPrincipalDevice(self: *KernelState, principal: PrincipalId, device: IommuDevice) void {
        const table = self.getTableConst(principal);
        var i: usize = 0;
        while (i < table.len) : (i += 1) {
            const cap = table.caps[i];
            if (!cap.rights.dma) continue;
            self.iommuUnmap(device, cap.paddr);
        }
    }

    fn syncAllIommuForPrincipal(self: *KernelState, principal: PrincipalId, reason: IommuSyncReason) KernelError!void {
        const table = self.getTableConst(principal);
        var i: usize = 0;
        while (i < table.len) : (i += 1) {
            try self.syncIommuForPrincipalPaddr(principal, table.caps[i].paddr, reason);
        }
    }

    pub fn registerIommuNoCapDriver(self: *KernelState, principal: PrincipalId, device: DmaDeviceId) KernelError!void {
        try self.requireActiveProcess(principal);
        const index = @intFromEnum(principal);
        const new_device = iommuDeviceFromDmaDeviceId(device);
        const old_device = self.iommu.principal_devices[index];
        if (old_device != null and old_device.? != new_device) {
            self.unmapAllIommuForPrincipalDevice(principal, old_device.?);
        }
        self.iommu.principal_devices[index] = new_device;
        try self.syncAllIommuForPrincipal(principal, .grant_dma);
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

    noinline fn callDebugWindowHook(
        hook: *const fn (state: *const KernelState, owner: PrincipalId, stage: DebugWindowStage, slot: usize, cap_paddr: u64, meta_paddr: u64) void,
        self: *const KernelState,
        owner: PrincipalId,
        stage: DebugWindowStage,
        slot: usize,
        cap_paddr: u64,
        meta_paddr: u64,
    ) void {
        hook(self, owner, stage, slot, cap_paddr, meta_paddr);
    }

    noinline fn initWindowCapPage(
        window_cap_paddr: u64,
        owner: PrincipalId,
        meta_paddr: u64,
        pixels_first_paddr: u64,
        bytes_aligned: u64,
        page_count: usize,
        pitch: u16,
        width: u16,
        height: u16,
        flags: u32,
        window_id: u32,
        allow_pixel_dma: bool,
    ) void {
        const rights = WindowRights{
            .read_meta = true,
            .write_meta = true,
            .write_pixels = true,
            .control = true,
            .dma_pixels = allow_pixel_dma,
        };
        const cap_bytes: [*]volatile u8 = @ptrFromInt(window_cap_paddr);
        @memset(cap_bytes[0..4096], 0);
        const cap_view: *volatile WindowCap = @ptrFromInt(window_cap_paddr);
        cap_view.* = .{
            .magic = 0x57434150, // 'WCAP'
            .version = 1,
            .rights_bits = @bitCast(rights),
            .window_id = window_id,
            .owner_pid = @intFromEnum(owner),
            .pixels_cap_paddr = pixels_first_paddr,
            .meta_cap_paddr = meta_paddr,
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
    }

    noinline fn initWindowMetaPage(meta_paddr: u64, width: u16, height: u16) void {
        const meta_bytes: [*]volatile u8 = @ptrFromInt(meta_paddr);
        @memset(meta_bytes[0..4096], 0);
        const meta_view: *volatile WindowMeta = @ptrFromInt(meta_paddr);
        meta_view.* = .{
            .magic = 0x574D5441, // 'WMTA'
            .version = 2,
            .state = 1,
            .seq = 1,
            .pos_x = 64,
            .pos_y = 64,
            .width = width,
            .height = height,
            .dirty_x = 0,
            .dirty_y = 0,
            .dirty_w = width,
            .dirty_h = height,
            .title_len = 0,
            .title = [_]u8{0} ** 64,
        };
    }

    noinline fn writeWindowRecord(
        self: *KernelState,
        slot: usize,
        owner: PrincipalId,
        window_id: u32,
        cap_paddr: u64,
        meta_paddr: u64,
        pixels_first_paddr: u64,
        bytes_aligned: u64,
        page_count: usize,
        pitch: u16,
        width: u16,
        height: u16,
        flags: u32,
    ) void {
        self.windows[slot] = .{
            .active = true,
            .window_id = window_id,
            .owner = owner,
            .cap_paddr = cap_paddr,
            .pixels_paddr = pixels_first_paddr,
            .meta_paddr = meta_paddr,
            .pixels_size_bytes = @intCast(bytes_aligned),
            .pixels_page_count = @intCast(page_count),
            .pixels_per_scan_line = pitch,
            .width = width,
            .height = height,
            .flags = flags,
            .z_hint = 0,
        };
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
        if (!debug_window_force_free_list) {
            if (self.findOwnedUntypedForPages(owner, 2, true)) |block_id| {
                try self.retypeUntypedToPages(owner, block_id, 2, true, bookkeeping_pages[0..]);
            } else {
                bookkeeping_pages[0] = try self.allocPageTo(owner, free_list);
                bookkeeping_pages[1] = try self.allocPageTo(owner, free_list);
            }
        } else {
            bookkeeping_pages[0] = try self.allocPageTo(owner, free_list);
            bookkeeping_pages[1] = try self.allocPageTo(owner, free_list);
        }
        const window_cap_page = bookkeeping_pages[0];
        const meta_page = bookkeeping_pages[1];
        if (self.debug_window_hook) |hook| {
            callDebugWindowHook(hook, self, owner, .after_alloc, slot, window_cap_page.paddr, meta_page.paddr);
        }

        const pixels_first_paddr: u64 = 0;

        try self.grantCap(owner, compositor, meta_page.paddr, .{
            .cpu_read = true,
            .cpu_write = false,
            .dma = false,
        });
        if (self.debug_window_hook) |hook| {
            callDebugWindowHook(hook, self, owner, .after_grant_meta, slot, window_cap_page.paddr, meta_page.paddr);
        }

        const window_id = self.allocWindowId();
        initWindowCapPage(
            window_cap_page.paddr,
            owner,
            meta_page.paddr,
            pixels_first_paddr,
            bytes_aligned,
            page_count,
            pitch,
            width,
            height,
            flags,
            window_id,
            allow_pixel_dma,
        );
        if (self.debug_window_hook) |hook| {
            callDebugWindowHook(hook, self, owner, .after_cap_init, slot, window_cap_page.paddr, meta_page.paddr);
        }

        initWindowMetaPage(meta_page.paddr, width, height);
        if (self.debug_window_hook) |hook| {
            callDebugWindowHook(hook, self, owner, .after_meta_init, slot, window_cap_page.paddr, meta_page.paddr);
        }

        writeWindowRecord(
            self,
            slot,
            owner,
            window_id,
            window_cap_page.paddr,
            meta_page.paddr,
            pixels_first_paddr,
            bytes_aligned,
            page_count,
            pitch,
            width,
            height,
            flags,
        );
        if (self.debug_window_hook) |hook| {
            callDebugWindowHook(hook, self, owner, .after_record_write, slot, window_cap_page.paddr, meta_page.paddr);
        }

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

    fn isFsRightsSubset(child: FsRights, parent: FsRights) bool {
        const child_bits: u32 = @bitCast(child);
        const parent_bits: u32 = @bitCast(parent);
        return (child_bits & ~parent_bits) == 0;
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

    pub fn hasActivePrincipal(self: *const KernelState, principal: PrincipalId) bool {
        if (processIndexFromPrincipal(principal)) |index| {
            return self.process_descriptors[index].active;
        }
        return principal == .Device0;
    }

    fn requireActiveProcess(self: *const KernelState, principal: PrincipalId) KernelError!void {
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
        if (self.process_descriptors[index].active) return true;
        self.process_descriptors[index] = .{
            .active = true,
            .principal = principal,
            .label = label,
        };
        self.active_process_count += 1;
        return true;
    }

    pub fn removeProcessDescriptor(self: *KernelState, principal: PrincipalId) bool {
        const index = processIndexFromPrincipal(principal) orelse return false;
        if (!self.process_descriptors[index].active) return false;
        self.process_descriptors[index] = .{};
        if (self.active_process_count > 0) self.active_process_count -= 1;
        return true;
    }

    pub fn ensureVfsProcess(self: *KernelState) bool {
        return self.ensureProcessDescriptor(vfs_principal, principalLabel(vfs_principal));
    }

    fn initPrincipalState(self: *KernelState) void {
        var i: usize = 0;
        while (i < principal_count) : (i += 1) {
            self.cap_tables[i] = .{};
            self.untyped_tables[i] = .{};
            self.endpoint_tables[i] = .{};
            self.cap_mailboxes[i] = .{};
            self.framebuffer_caps[i] = null;
            self.fs_tables[i] = .{};
            self.fs_mailboxes[i] = .{};
            self.vm_object_tables[i] = .{};
            self.exec_image_tables[i] = .{};
        }
        i = 0;
        while (i < self.process_descriptors.len) : (i += 1) {
            self.process_descriptors[i] = .{};
        }
        self.active_process_count = 0;
        i = 0;
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
        while (i < self.active_process_count) : (i += 1) {
            const proc = processPrincipal(i);
            try self.endpoint_tables[@intFromEnum(proc)].add(.{
                .endpoint_id = endpoint_to_process1,
                .target = p1,
            });
        }
    }

    pub fn initPhase1InPlace(self: *KernelState) void {
        self.* = .{};
        self.next_cap_id = 1;
        self.next_window_id = 1;
        self.regions[0] = .{
            .id = 0,
        };
        self.region_len = 1;
        self.initPrincipalState();
        self.installDefaultEndpoints() catch unreachable;

        const p0 = processPrincipal(0);
        const root_id = self.allocCapId();
        self.cap_tables[@intFromEnum(p0)].add(.{
            .paddr = 0x1000,
            .rights = .{ .cpu_read = true, .cpu_write = true, .dma = true },
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
        self.next_window_id = 1;
        self.initPrincipalState();
        try self.installDefaultEndpoints();

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
            callPteSyncHook(hook, self, .Device0, paddr);
            callPteSyncHook(hook, self, .Process0, paddr);
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
        try self.requireActiveProcess(owner);
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
        try self.requireActiveProcess(owner);
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

    pub fn grantQueueCapStage2(
        self: *KernelState,
        owner: PrincipalId,
        child: PrincipalId,
        token: u64,
    ) KernelError!u64 {
        try self.requireActiveProcess(owner);
        try self.requireActiveProcess(child);
        return self.queue_caps.grant(
            @intFromEnum(owner),
            @intFromEnum(child),
            token,
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

    pub fn getFsTable(self: *KernelState, principal: PrincipalId) *FsCNode {
        return &self.fs_tables[self.principalStorageIndex(principal)];
    }

    pub fn getFsTableConst(self: *const KernelState, principal: PrincipalId) *const FsCNode {
        return &self.fs_tables[self.principalStorageIndex(principal)];
    }

    pub fn hasFsAdminCap(self: *const KernelState, principal: PrincipalId) bool {
        const table = self.getFsTableConst(principal);
        var i: usize = 0;
        while (i < table.len) : (i += 1) {
            if (table.caps[i].rights.admin) return true;
        }
        return false;
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
        const ep = self.getEndpointTableConst(owner).find(endpoint_id) orelse return null;
        return ep.target;
    }

    pub fn grantFramebufferCap(
        self: *KernelState,
        to: PrincipalId,
        framebuffer: FramebufferCapability,
    ) KernelError!void {
        try self.requireActiveProcess(to);
        if (framebuffer.size_bytes == 0) return KernelError.InvalidState;

        const size_minus_one = framebuffer.size_bytes - 1;
        const paddr_overflow = @addWithOverflow(framebuffer.paddr, @as(u64, @intCast(size_minus_one)))[1];
        if (paddr_overflow != 0) return KernelError.InvalidState;

        self.framebuffer_caps[@intFromEnum(to)] = framebuffer;
    }

    pub fn installFsCap(
        self: *KernelState,
        owner: PrincipalId,
        object_id: u64,
        kind: FsObjectKind,
        rights: FsRights,
    ) KernelError!u64 {
        try self.requireActiveProcess(owner);
        if (kind == .none) return KernelError.InvalidState;

        const root_id = self.allocCapId();
        try self.getFsTable(owner).add(.{
            .object_id = object_id,
            .kind = kind,
            .rights = rights,
            .cap_id = root_id,
            .root_cap_id = root_id,
            .parent_cap_id = 0,
        });
        return root_id;
    }

    pub fn grantFsCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        cap_id: u64,
        rights: FsRights,
    ) KernelError!u64 {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);

        const src_cap = self.getFsTableConst(from).findByCapId(cap_id) orelse return KernelError.FsCapabilityNotFound;
        if (!src_cap.rights.grant) return KernelError.InvalidState;
        if (!isFsRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;

        const child_id = self.allocCapId();
        try self.getFsTable(to).add(.{
            .object_id = src_cap.object_id,
            .kind = src_cap.kind,
            .rights = rights,
            .cap_id = child_id,
            .root_cap_id = src_cap.root_cap_id,
            .parent_cap_id = src_cap.cap_id,
        });
        return child_id;
    }

    pub fn moveFsCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        cap_id: u64,
        rights: FsRights,
    ) KernelError!u64 {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);

        const src = self.getFsTable(from);
        const src_cap = src.findByCapId(cap_id) orelse return KernelError.FsCapabilityNotFound;
        if (!isFsRightsSubset(rights, src_cap.rights)) return KernelError.InvalidState;

        var moved = src_cap.*;
        moved.rights = rights;
        _ = src.removeByCapId(cap_id);
        try self.getFsTable(to).add(moved);
        return moved.cap_id;
    }

    pub fn sendFsCap(
        self: *KernelState,
        from: PrincipalId,
        to: PrincipalId,
        cap_id: u64,
    ) KernelError!u64 {
        if (from == to) return KernelError.InvalidState;
        try self.requireActiveProcess(from);
        try self.requireActiveProcess(to);

        const src_cap = self.getFsTableConst(from).findByCapId(cap_id) orelse return KernelError.FsCapabilityNotFound;
        return self.moveFsCap(from, to, cap_id, src_cap.rights);
    }

    pub fn sendFsCapOnEndpoint(
        self: *KernelState,
        from: PrincipalId,
        endpoint_id: u64,
        cap_id: u64,
    ) KernelError!u64 {
        try self.requireActiveProcess(from);
        const target = self.endpointTargetFor(from, endpoint_id) orelse return KernelError.EndpointNotFound;
        const src_cap = self.getFsTableConst(from).findByCapId(cap_id) orelse return KernelError.FsCapabilityNotFound;
        const child_id = try self.grantFsCap(from, target, cap_id, src_cap.rights);
        try self.fs_mailboxes[@intFromEnum(target)].push(child_id);
        return child_id;
    }

    pub fn recvFsCap(self: *KernelState, receiver: PrincipalId) KernelError!u64 {
        try self.requireActiveProcess(receiver);
        return self.fs_mailboxes[@intFromEnum(receiver)].pop() orelse KernelError.FsMailboxEmpty;
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

    pub fn getFramebufferCap(self: *const KernelState, principal: PrincipalId) ?FramebufferCapability {
        if (!self.hasActivePrincipal(principal)) return null;
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
                    try self.syncIommuForPrincipalPaddr(@enumFromInt(pidx), removed_paddr, .revoke);
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
        try self.syncIommuForPrincipalPaddr(from, paddr, .move_from);
        try self.syncIommuForPrincipalPaddr(to, paddr, .move_to);
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
        if (self.pte_sync_hook) |hook| {
            if (!capability.principalHasMappedPaddr(to, paddr)) return;
            callPteSyncHook(hook, self, to, paddr);
        }
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
        try self.grantCap(from, target, paddr, src_cap.rights);
        try self.cap_mailboxes[@intFromEnum(target)].push(paddr);
    }

    pub fn recvCap(self: *KernelState, receiver: PrincipalId) KernelError!u64 {
        try self.requireActiveProcess(receiver);
        return self.cap_mailboxes[@intFromEnum(receiver)].pop() orelse KernelError.MailboxEmpty;
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
                try self.syncIommuForPrincipalPaddr(@enumFromInt(pidx), removed_paddr, .revoke);
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
    try s.registerIommuNoCapDriver(.Process1, .virtio_gpu);

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
    try s.registerIommuNoCapDriver(.Process1, .virtio_gpu);

    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    });
    try std.testing.expect(!s.iommuHasMappingForPrincipalForTest(.Process1, 0x1000));
}

test "iommu driver registration syncs existing dma grant" {
    var s = KernelState.initPhase1();
    s.setIommuNoCapDriverMode(.shadow);

    try s.grantCap(.Process0, .Process1, 0x1000, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = true,
    });
    try std.testing.expect(!s.iommuHasMappingForPrincipalForTest(.Process1, 0x1000));

    try s.registerIommuNoCapDriver(.Process1, .virtio_gpu);
    try std.testing.expect(s.iommuHasMappingForPrincipalForTest(.Process1, 0x1000));
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

test "ensureVfsProcess activates reserved VFS principal" {
    var s = KernelState.initPhase1();
    try std.testing.expect(!s.isActiveProcess(vfs_principal));
    try std.testing.expect(s.ensureVfsProcess());
    try std.testing.expect(s.isActiveProcess(vfs_principal));
    const desc = s.processDescriptor(vfs_principal).?;
    try std.testing.expectEqualStrings("VFS", desc.label);
}

test "filesystem capability grant preserves object and lineage" {
    var s = KernelState.initPhase1();
    try std.testing.expect(s.ensureVfsProcess());

    const root_token = try s.installFsCap(vfs_principal, 0x100, .mount, .{
        .lookup = true,
        .read = true,
        .readdir = true,
        .stat = true,
        .mount = true,
        .grant = true,
    });
    const child_token = try s.grantFsCap(vfs_principal, .Process0, root_token, .{
        .lookup = true,
        .read = true,
        .readdir = true,
        .stat = true,
    });

    const root_cap = s.getFsTableConst(vfs_principal).findByCapId(root_token).?;
    const child_cap = s.getFsTableConst(.Process0).findByCapId(child_token).?;
    try std.testing.expectEqual(@as(u64, 0x100), child_cap.object_id);
    try std.testing.expectEqual(FsObjectKind.mount, child_cap.kind);
    try std.testing.expectEqual(root_cap.root_cap_id, child_cap.root_cap_id);
    try std.testing.expectEqual(root_cap.cap_id, child_cap.parent_cap_id);
}

test "filesystem capability grant requires grant right" {
    var s = KernelState.initPhase1();
    try std.testing.expect(s.ensureVfsProcess());

    const root_token = try s.installFsCap(vfs_principal, 0x180, .vnode_dir, .{
        .lookup = true,
        .readdir = true,
        .stat = true,
    });
    try std.testing.expectError(KernelError.InvalidState, s.grantFsCap(vfs_principal, .Process0, root_token, .{
        .lookup = true,
    }));
}

test "filesystem capability send on endpoint enqueues granted token" {
    var s = KernelState.initPhase1();
    const root_token = try s.installFsCap(.Process0, 0x200, .vnode_dir, .{
        .lookup = true,
        .readdir = true,
        .stat = true,
    });

    const recv_token = try s.sendFsCapOnEndpoint(.Process0, endpoint_to_process1, root_token);
    try std.testing.expectEqual(recv_token, try s.recvFsCap(.Process1));
    const recv_cap = s.getFsTableConst(.Process1).findByCapId(recv_token).?;
    try std.testing.expectEqual(@as(u64, 0x200), recv_cap.object_id);
    try std.testing.expectEqual(FsObjectKind.vnode_dir, recv_cap.kind);
}
