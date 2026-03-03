const std = @import("std");

pub const PrincipalId = enum(u8) {
    Process0,
    Process1,
    Device0,
};

pub const Rights = struct {
    cpu_read: bool,
    cpu_write: bool,
    dma: bool,
};

pub const Capability = struct {
    paddr: u64,
    rights: Rights,
};

pub const Region = struct {
    id: u64,
};

pub const KernelError = error{
    RegionNotFound,
    CapabilityNotFound,
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
    const max_caps = 8;

    caps: [max_caps]Capability = undefined,
    len: usize = 0,

    pub fn add(self: *CNode, cap: Capability) KernelError!void {
        // 同一 paddr の capability は上書き扱いにする。
        if (self.findIndex(cap.paddr)) |index| {
            self.caps[index] = cap;
            return;
        }
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

    fn findIndex(self: *const CNode, paddr: u64) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            if (self.caps[i].paddr == paddr) return i;
        }
        return null;
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
};

pub const PageCapability = struct {
    paddr: u64,
};

pub const OwnershipView = enum {
    Process0,
    Device0,
    Shared,
    None,
};

pub const KernelState = struct {
    pub const max_regions = 256;
    const principal_count = 3;

    regions: [max_regions]Region = undefined,
    region_len: usize = 0,
    cap_tables: [principal_count]CNode = .{ .{}, .{}, .{} },
    pte_sync_hook: ?*const fn (state: *const KernelState, paddr: u64) void = null,

    pub fn initPhase1() KernelState {
        var state = KernelState{};
        state.regions[0] = .{
            .id = 0,
        };
        state.region_len = 1;

        state.cap_tables[@intFromEnum(PrincipalId.Process0)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Process1)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Device0)] = .{};

        // Process0 initially owns region0 and has read + dma.
        state.cap_tables[@intFromEnum(PrincipalId.Process0)].add(.{
            .paddr = 0x1000,
            .rights = .{ .cpu_read = true, .cpu_write = true, .dma = true },
        }) catch unreachable;

        return state;
    }

    pub fn initFromDetectedRegions(region_count: usize) KernelError!KernelState {
        if (region_count == 0) return KernelError.EmptyRegionSet;
        if (region_count > max_regions) return KernelError.TooManyRegions;

        var state = KernelState{};
        state.cap_tables[@intFromEnum(PrincipalId.Process0)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Process1)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Device0)] = .{};

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
            .paddr = try free_list.popFront(),
        };
    }

    pub fn allocPageTo(
        self: *KernelState,
        requester: PrincipalId,
        free_list: *FreePageList,
    ) KernelError!PageCapability {
        const cap = try self.allocPage(requester, free_list);
        try self.getTable(requester).add(.{
            .paddr = cap.paddr,
            .rights = .{
                .cpu_read = true,
                .cpu_write = true,
                .dma = true,
            },
        });
        if (self.pte_sync_hook) |hook| {
            hook(self, cap.paddr);
        }
        return cap;
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
        _ = src.find(paddr) orelse return KernelError.CapabilityNotFound;
        if (self.getTable(to).find(paddr) != null) return KernelError.InvalidState;

        _ = src.removeByPaddr(paddr);
        try self.getTable(to).add(.{
            .paddr = paddr,
            .rights = rights,
        });
        if (self.pte_sync_hook) |hook| {
            hook(self, paddr);
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
