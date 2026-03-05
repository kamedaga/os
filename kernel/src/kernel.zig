const std = @import("std");

pub const PrincipalId = enum(u8) {
    Process0,
    Process1,
    Process2,
    Process3,
    Process4,
    Device0,
};

pub const endpoint_to_process0: u64 = 0x10;
pub const endpoint_to_process1: u64 = 0x11;
pub const endpoint_to_process2: u64 = 0x12;

fn isProcessPrincipal(principal: PrincipalId) bool {
    return switch (principal) {
        .Process0, .Process1, .Process2, .Process3, .Process4 => true,
        .Device0 => false,
    };
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
};

pub const CNode = struct {
    pub const max_caps = 1024;

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
    start_index: usize,
    len: usize,
    physical_start: u64,
};

pub const FreePageList = struct {
    pub const max_pages = 262_144;
    pub const max_ranges = 256;

    pages: [max_pages]u64 = undefined,
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
        if (self.len >= self.pages.len) return KernelError.TooManyFreePages;

        // 直前 range と「同じ region かつ物理的に連続」の場合は range を延長する。
        if (self.range_len > 0) {
            const last = &self.ranges[self.range_len - 1];
            const expected_next = last.physical_start + (@as(u64, last.len) * 4096);
            if (last.region_id == region_id and paddr == expected_next) {
                self.pages[self.len] = paddr;
                self.len += 1;
                last.len += 1;
                return;
            }
        }

        if (self.range_len >= self.ranges.len) return KernelError.TooManyFreeRanges;

        const start_index = self.len;
        self.pages[self.len] = paddr;
        self.len += 1;

        self.ranges[self.range_len] = .{
            .region_id = region_id,
            .start_index = start_index,
            .len = 1,
            .physical_start = paddr,
        };
        self.range_len += 1;
    }

    pub fn popFront(self: *FreePageList) KernelError!u64 {
        if (self.len == 0) return KernelError.OutOfFreePages;

        // 単純な FIFO。起動初期段階なので O(n) シフトで実装する。
        const paddr = self.pages[0];

        var i: usize = 1;
        while (i < self.len) : (i += 1) {
            self.pages[i - 1] = self.pages[i];
        }
        self.len -= 1;

        if (self.range_len > 0) {
            self.ranges[0].start_index = 0;
            if (self.ranges[0].len > 0) self.ranges[0].len -= 1;

            if (self.ranges[0].len == 0) {
                var r: usize = 1;
                while (r < self.range_len) : (r += 1) {
                    self.ranges[r - 1] = self.ranges[r];
                    self.ranges[r - 1].start_index -= 1;
                }
                self.range_len -= 1;
            } else {
                self.ranges[0].physical_start += 4096;
                var r: usize = 1;
                while (r < self.range_len) : (r += 1) {
                    self.ranges[r].start_index -= 1;
                }
            }
        }

        return paddr;
    }

    pub fn popBack(self: *FreePageList) KernelError!u64 {
        if (self.len == 0) return KernelError.OutOfFreePages;

        const paddr = self.pages[self.len - 1];
        self.len -= 1;

        if (self.range_len > 0) {
            const last = &self.ranges[self.range_len - 1];
            if (last.len > 0) last.len -= 1;
            if (last.len == 0) {
                self.range_len -= 1;
            }
        }

        return paddr;
    }
};

pub const PageCapability = struct {
    paddr: u64,
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

pub const VirtualFramebufferCapability = struct {
    paddr: u64,
    size_bytes: usize,
    width: u32,
    height: u32,
    pixels_per_scan_line: u32,
    pixel_format: u32,
    allow_read: bool = true,
    allow_write: bool = false,
};

pub const WindowRights = packed struct(u16) {
    read_meta: bool = false,
    write_meta: bool = false,
    write_pixels: bool = false,
    control: bool = false,
    _reserved: u12 = 0,
};

pub const WindowCap = packed struct {
    magic: u32, // 'WCAP'
    version: u16,
    rights_bits: u16,
    window_id: u32,
    owner_pid: u32,
    vfb_cap_paddr: u64,
    meta_cap_paddr: u64,
    vfb_size_bytes: u32,
    vfb_page_count: u16,
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

pub const WindowRecord = struct {
    active: bool = false,
    window_id: u32 = 0,
    owner: PrincipalId = .Process0,
    cap_paddr: u64 = 0,
    vfb_paddr: u64 = 0,
    meta_paddr: u64 = 0,
    vfb_size_bytes: u32 = 0,
    vfb_page_count: u16 = 0,
    pixels_per_scan_line: u16 = 0,
    width: u16 = 0,
    height: u16 = 0,
    flags: u32 = 0,
    z_hint: i32 = 0,
};

pub const CreateWindowResult = struct {
    window_cap_paddr: u64,
    meta_paddr: u64,
    vfb_first_paddr: u64,
    vfb_page_count: u16,
};

pub const OwnershipView = enum {
    Process0,
    Device0,
    Shared,
    None,
};

pub const KernelState = struct {
    pub const max_regions = 256;
    const principal_count = 6;
    const max_total_caps = principal_count * CNode.max_caps;
    pub const max_windows = 64;

    regions: [max_regions]Region = undefined,
    region_len: usize = 0,
    cap_tables: [principal_count]CNode = [_]CNode{.{}} ** principal_count,
    endpoint_tables: [principal_count]EndpointCNode = [_]EndpointCNode{.{}} ** principal_count,
    cap_mailboxes: [principal_count]CapMailbox = [_]CapMailbox{.{}} ** principal_count,
    framebuffer_caps: [principal_count]?FramebufferCapability = [_]?FramebufferCapability{null} ** principal_count,
    virtual_framebuffer_caps: [principal_count]?VirtualFramebufferCapability = [_]?VirtualFramebufferCapability{null} ** principal_count,
    pte_sync_hook: ?*const fn (state: *const KernelState, principal: PrincipalId, paddr: u64) void = null,
    revoke_queue: [max_total_caps]u64 = undefined,
    revoke_subtree: [max_total_caps]u64 = undefined,
    next_cap_id: u64 = 1,
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

        const pitch: u16 = width;
        const pixel_count = @as(u64, width) * @as(u64, height);
        const bytes_unaligned = pixel_count * 4;
        if (bytes_unaligned == 0 or bytes_unaligned > (16 * 1024 * 1024)) return KernelError.InvalidState;
        const bytes_aligned = pageAlignUp(bytes_unaligned);
        const page_count_u64 = bytes_aligned / 4096;
        if (page_count_u64 == 0 or page_count_u64 > 1024) return KernelError.InvalidState;
        const page_count: usize = @intCast(page_count_u64);

        const window_cap_page = try self.allocPageTo(owner, free_list);
        const meta_page = try self.allocPageTo(owner, free_list);

        var vfb_first_paddr: u64 = 0;
        var i: usize = 0;
        while (i < page_count) : (i += 1) {
            const p = try self.allocPageTo(owner, free_list);
            if (i == 0) vfb_first_paddr = p.paddr;
            try self.grantCap(owner, compositor, p.paddr, .{
                .cpu_read = true,
                .cpu_write = false,
                .dma = false,
            });
            const page_words: [*]volatile u64 = @ptrFromInt(p.paddr);
            var w: usize = 0;
            while (w < 512) : (w += 1) page_words[w] = 0;
        }

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
            .vfb_cap_paddr = vfb_first_paddr,
            .meta_cap_paddr = meta_page.paddr,
            .vfb_size_bytes = @intCast(bytes_aligned),
            .vfb_page_count = @intCast(page_count),
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
            .vfb_paddr = vfb_first_paddr,
            .meta_paddr = meta_page.paddr,
            .vfb_size_bytes = @intCast(bytes_aligned),
            .vfb_page_count = @intCast(page_count),
            .pixels_per_scan_line = pitch,
            .width = width,
            .height = height,
            .flags = flags,
            .z_hint = 0,
        };

        return .{
            .window_cap_paddr = window_cap_page.paddr,
            .meta_paddr = meta_page.paddr,
            .vfb_first_paddr = vfb_first_paddr,
            .vfb_page_count = @intCast(page_count),
        };
    }

    fn isRightsSubset(child: Rights, parent: Rights) bool {
        return (!child.cpu_read or parent.cpu_read) and
            (!child.cpu_write or parent.cpu_write) and
            (!child.dma or parent.dma);
    }

    pub fn initPhase1() KernelState {
        var state = KernelState{};
        state.next_cap_id = 1;
        state.regions[0] = .{
            .id = 0,
        };
        state.region_len = 1;

        state.cap_tables[@intFromEnum(PrincipalId.Process0)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Process1)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Process2)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Process3)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Process4)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Device0)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Process0)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Process1)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Process2)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Process3)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Process4)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Device0)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Process0)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Process1)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Process2)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Process3)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Process4)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Device0)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Process0)].add(.{
            .endpoint_id = endpoint_to_process1,
            .target = .Process1,
        }) catch unreachable;
        state.endpoint_tables[@intFromEnum(PrincipalId.Process1)].add(.{
            .endpoint_id = endpoint_to_process0,
            .target = .Process0,
        }) catch unreachable;
        state.endpoint_tables[@intFromEnum(PrincipalId.Process2)].add(.{
            .endpoint_id = endpoint_to_process1,
            .target = .Process1,
        }) catch unreachable;
        state.endpoint_tables[@intFromEnum(PrincipalId.Process3)].add(.{
            .endpoint_id = endpoint_to_process1,
            .target = .Process1,
        }) catch unreachable;
        state.endpoint_tables[@intFromEnum(PrincipalId.Process4)].add(.{
            .endpoint_id = endpoint_to_process1,
            .target = .Process1,
        }) catch unreachable;

        // Process0 initially owns region0 and has read + dma.
        const root_id = state.allocCapId();
        state.cap_tables[@intFromEnum(PrincipalId.Process0)].add(.{
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
        state.cap_tables[@intFromEnum(PrincipalId.Process0)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Process1)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Process2)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Process3)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Process4)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Device0)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Process0)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Process1)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Process2)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Process3)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Process4)] = .{};
        state.endpoint_tables[@intFromEnum(PrincipalId.Device0)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Process0)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Process1)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Process2)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Process3)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Process4)] = .{};
        state.cap_mailboxes[@intFromEnum(PrincipalId.Device0)] = .{};
        try state.endpoint_tables[@intFromEnum(PrincipalId.Process0)].add(.{
            .endpoint_id = endpoint_to_process1,
            .target = .Process1,
        });
        try state.endpoint_tables[@intFromEnum(PrincipalId.Process1)].add(.{
            .endpoint_id = endpoint_to_process0,
            .target = .Process0,
        });
        try state.endpoint_tables[@intFromEnum(PrincipalId.Process2)].add(.{
            .endpoint_id = endpoint_to_process1,
            .target = .Process1,
        });
        try state.endpoint_tables[@intFromEnum(PrincipalId.Process3)].add(.{
            .endpoint_id = endpoint_to_process1,
            .target = .Process1,
        });
        try state.endpoint_tables[@intFromEnum(PrincipalId.Process4)].add(.{
            .endpoint_id = endpoint_to_process1,
            .target = .Process1,
        });

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

        // DMA 完了時も moveCap 経由で Process0 に戻す。
        try self.moveCap(
            .Device0,
            .Process0,
            paddr,
            .{
                .cpu_read = true,
                .cpu_write = true,
                .dma = true,
            },
        );
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

    pub fn grantVirtualFramebufferCap(
        self: *KernelState,
        to: PrincipalId,
        virtual_framebuffer: VirtualFramebufferCapability,
    ) KernelError!void {
        if (!isProcessPrincipal(to)) return KernelError.InvalidState;
        if (virtual_framebuffer.size_bytes == 0) return KernelError.InvalidState;
        if (virtual_framebuffer.width == 0 or virtual_framebuffer.height == 0) return KernelError.InvalidState;
        if (virtual_framebuffer.pixels_per_scan_line < virtual_framebuffer.width) return KernelError.InvalidState;

        const size_minus_one = virtual_framebuffer.size_bytes - 1;
        const paddr_overflow = @addWithOverflow(virtual_framebuffer.paddr, @as(u64, @intCast(size_minus_one)))[1];
        if (paddr_overflow != 0) return KernelError.InvalidState;

        const required_bytes = @as(u64, virtual_framebuffer.pixels_per_scan_line) *
            @as(u64, virtual_framebuffer.height) *
            @as(u64, 4);
        if (required_bytes > virtual_framebuffer.size_bytes) return KernelError.InvalidState;

        self.virtual_framebuffer_caps[@intFromEnum(to)] = virtual_framebuffer;
    }

    pub fn getVirtualFramebufferCap(self: *const KernelState, principal: PrincipalId) ?VirtualFramebufferCapability {
        if (!isProcessPrincipal(principal)) return null;
        return self.virtual_framebuffer_caps[@intFromEnum(principal)];
    }

    pub fn canAccessVirtualFramebuffer(
        self: *const KernelState,
        principal: PrincipalId,
        paddr: u64,
        size_bytes: usize,
        writable: bool,
    ) bool {
        if (size_bytes == 0) return false;

        const virtual_framebuffer = self.getVirtualFramebufferCap(principal) orelse return false;
        if (!virtual_framebuffer.allow_read) return false;
        if (writable and !virtual_framebuffer.allow_write) return false;

        const map_end, const map_overflow = @addWithOverflow(paddr, @as(u64, @intCast(size_bytes - 1)));
        if (map_overflow != 0) return false;
        const vfb_end, const vfb_overflow = @addWithOverflow(virtual_framebuffer.paddr, @as(u64, @intCast(virtual_framebuffer.size_bytes - 1)));
        if (vfb_overflow != 0) return false;

        return paddr >= virtual_framebuffer.paddr and map_end <= vfb_end;
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

    pub fn allocPage(
        self: *KernelState,
        requester: PrincipalId,
        free_list: *FreePageList,
    ) KernelError!PageCapability {
        _ = self;
        _ = requester;
        return .{
            .paddr = try free_list.popBack(),
        };
    }

    pub fn allocPageTo(
        self: *KernelState,
        requester: PrincipalId,
        free_list: *FreePageList,
    ) KernelError!PageCapability {
        const cap = try self.allocPage(requester, free_list);
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

        const src = self.getTable(from);
        const src_cap = src.find(paddr) orelse return KernelError.CapabilityNotFound;
        if (self.getTable(to).find(paddr) != null) return KernelError.InvalidState;

        var moved = src_cap.*;
        moved.rights = rights;
        _ = src.removeByPaddr(paddr);
        try self.getTable(to).add(moved);
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
        if (self.pte_sync_hook) |hook| {
            hook(self, to, paddr);
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
                if (self.pte_sync_hook) |hook| {
                    hook(self, @enumFromInt(pidx), removed_paddr);
                }
                break;
            }
        }
    }
};

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
    try std.testing.expectEqual(@as(u64, 0x1000), free_list.pages[0]);
    try std.testing.expectEqual(@as(u64, 0x2000), free_list.pages[1]);
    try std.testing.expectEqual(@as(u64, 0x3000), free_list.pages[2]);
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

test "moveCap enforces single holder" {
    var s = KernelState.initPhase1();
    try s.moveCap(.Process0, .Device0, 0x1000, .{ .cpu_read = false, .cpu_write = false, .dma = true });

    try std.testing.expect(s.getTableConst(.Process0).find(0x1000) == null);
    try std.testing.expect(s.getTableConst(.Device0).find(0x1000) != null);
    try std.testing.expectEqual(OwnershipView.Device0, s.scanCapTables(0x1000));
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

test "virtual framebuffer capability grants shared access to configured processes" {
    var s = try KernelState.initFromDetectedRegions(1);
    const vfb_cap_owner = VirtualFramebufferCapability{
        .paddr = 0x9000_0000,
        .size_bytes = 4096,
        .width = 32,
        .height = 32,
        .pixels_per_scan_line = 32,
        .pixel_format = 0,
        .allow_read = true,
        .allow_write = true,
    };
    const vfb_cap_compositor = VirtualFramebufferCapability{
        .paddr = 0x9000_0000,
        .size_bytes = 4096,
        .width = 32,
        .height = 32,
        .pixels_per_scan_line = 32,
        .pixel_format = 0,
        .allow_read = true,
        .allow_write = true,
    };
    try s.grantVirtualFramebufferCap(.Process0, vfb_cap_owner);
    try s.grantVirtualFramebufferCap(.Process1, vfb_cap_compositor);

    try std.testing.expect(s.canAccessVirtualFramebuffer(.Process0, 0x9000_0000, 4096, true));
    try std.testing.expect(s.canAccessVirtualFramebuffer(.Process1, 0x9000_0080, 128, true));
    try std.testing.expect(!s.canAccessVirtualFramebuffer(.Process2, 0x9000_0000, 4096, false));
    try std.testing.expect(!s.canAccessVirtualFramebuffer(.Process0, 0x8FFF_F000, 4096, true));
}

test "virtual framebuffer capability rejects write when allow_write is false" {
    var s = try KernelState.initFromDetectedRegions(1);
    try s.grantVirtualFramebufferCap(.Process1, .{
        .paddr = 0x9100_0000,
        .size_bytes = 4096,
        .width = 32,
        .height = 32,
        .pixels_per_scan_line = 32,
        .pixel_format = 0,
        .allow_read = true,
        .allow_write = false,
    });

    try std.testing.expect(s.canAccessVirtualFramebuffer(.Process1, 0x9100_0000, 64, false));
    try std.testing.expect(!s.canAccessVirtualFramebuffer(.Process1, 0x9100_0000, 64, true));
}
