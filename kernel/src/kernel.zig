const std = @import("std");

pub const PrincipalId = enum(u8) {
    Process0,
    Device0,
};

pub const Rights = struct {
    read: bool,
    dma: bool,
};

pub const Capability = struct {
    region_id: u64,
    rights: Rights,
};

pub const Region = struct {
    id: u64,
    owner: PrincipalId,
};

pub const KernelError = error{
    RegionNotFound,
    CapabilityNotFound,
    NoDmaRight,
    InvalidOwner,
    TableFull,
};

pub const CNode = struct {
    const max_caps = 8;

    caps: [max_caps]Capability = undefined,
    len: usize = 0,

    pub fn add(self: *CNode, cap: Capability) KernelError!void {
        if (self.findIndex(cap.region_id)) |index| {
            self.caps[index] = cap;
            return;
        }
        if (self.len >= self.caps.len) {
            return KernelError.TableFull;
        }
        self.caps[self.len] = cap;
        self.len += 1;
    }

    pub fn removeByRegion(self: *CNode, region_id: u64) bool {
        if (self.findIndex(region_id)) |index| {
            var i = index;
            while (i + 1 < self.len) : (i += 1) {
                self.caps[i] = self.caps[i + 1];
            }
            self.len -= 1;
            return true;
        }
        return false;
    }

    pub fn find(self: *const CNode, region_id: u64) ?*const Capability {
        if (self.findIndex(region_id)) |index| {
            return &self.caps[index];
        }
        return null;
    }

    fn findIndex(self: *const CNode, region_id: u64) ?usize {
        var i: usize = 0;
        while (i < self.len) : (i += 1) {
            if (self.caps[i].region_id == region_id) return i;
        }
        return null;
    }
};

pub const KernelState = struct {
    const max_regions = 8;

    regions: [max_regions]Region = undefined,
    region_len: usize = 0,
    cap_tables: [2]CNode = .{ .{}, .{} },

    pub fn initPhase1() KernelState {
        var state = KernelState{};
        state.regions[0] = .{
            .id = 0,
            .owner = .Process0,
        };
        state.region_len = 1;

        state.cap_tables[@intFromEnum(PrincipalId.Process0)] = .{};
        state.cap_tables[@intFromEnum(PrincipalId.Device0)] = .{};

        // Process0 initially owns region0 and has read + dma.
        state.cap_tables[@intFromEnum(PrincipalId.Process0)].add(.{
            .region_id = 0,
            .rights = .{ .read = true, .dma = true },
        }) catch unreachable;

        return state;
    }

    pub fn startDma(self: *KernelState, region_id: u64) KernelError!void {
        const region = self.getRegion(region_id) orelse return KernelError.RegionNotFound;
        if (region.owner != .Process0) return KernelError.InvalidOwner;

        const p0_table = self.getTable(.Process0);
        const p0_cap = p0_table.find(region_id) orelse return KernelError.CapabilityNotFound;
        if (!p0_cap.rights.dma) return KernelError.NoDmaRight;

        region.owner = .Device0;

        // During DMA, Process0 loses access for phase1 safety conditions.
        _ = p0_table.removeByRegion(region_id);

        try self.getTable(.Device0).add(.{
            .region_id = region_id,
            .rights = .{
                .read = false,
                .dma = true,
            },
        });
    }

    pub fn completeDma(self: *KernelState, region_id: u64) KernelError!void {
        const region = self.getRegion(region_id) orelse return KernelError.RegionNotFound;
        if (region.owner != .Device0) return KernelError.InvalidOwner;

        const dev_table = self.getTable(.Device0);
        const dev_cap = dev_table.find(region_id) orelse return KernelError.CapabilityNotFound;
        if (!dev_cap.rights.dma) return KernelError.NoDmaRight;

        region.owner = .Process0;
        _ = dev_table.removeByRegion(region_id);

        try self.getTable(.Process0).add(.{
            .region_id = region_id,
            .rights = .{
                .read = true,
                .dma = true,
            },
        });
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
};

test "phase1 init state" {
    const s = KernelState.initPhase1();

    try std.testing.expectEqual(@as(usize, 1), s.region_len);
    try std.testing.expectEqual(PrincipalId.Process0, s.getRegionConst(0).?.owner);

    const p0 = s.getTableConst(.Process0);
    try std.testing.expectEqual(@as(usize, 1), p0.len);
    try std.testing.expect(p0.find(0).?.rights.read);
    try std.testing.expect(p0.find(0).?.rights.dma);

    const dev = s.getTableConst(.Device0);
    try std.testing.expectEqual(@as(usize, 0), dev.len);
}

test "start dma moves owner and capabilities" {
    var s = KernelState.initPhase1();
    try s.startDma(0);

    try std.testing.expectEqual(PrincipalId.Device0, s.getRegionConst(0).?.owner);
    try std.testing.expect(s.getTableConst(.Process0).find(0) == null);
    const dev_cap = s.getTableConst(.Device0).find(0).?;
    try std.testing.expect(dev_cap.rights.dma);
    try std.testing.expect(!dev_cap.rights.read);
}

test "complete dma returns owner and capabilities" {
    var s = KernelState.initPhase1();
    try s.startDma(0);
    try s.completeDma(0);

    try std.testing.expectEqual(PrincipalId.Process0, s.getRegionConst(0).?.owner);
    try std.testing.expect(s.getTableConst(.Device0).find(0) == null);
    const p0_cap = s.getTableConst(.Process0).find(0).?;
    try std.testing.expect(p0_cap.rights.read);
    try std.testing.expect(p0_cap.rights.dma);
}

test "invalid ownership rejected" {
    var s = KernelState.initPhase1();
    try std.testing.expectError(KernelError.InvalidOwner, s.completeDma(0));
    try s.startDma(0);
    try std.testing.expectError(KernelError.InvalidOwner, s.startDma(0));
}
