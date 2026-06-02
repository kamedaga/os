const std = @import("std");
const abi_root = @import("kernel_abi_root");
const capsule_abi = abi_root.capsule_abi;

pub const invalid_capsule: u64 = 0;
pub const known_rights_mask: u64 = (1 << 13) - 1;

pub const CapsuleKind = enum(u8) {
    none = 0,
    session = 1,
    device = 2,
    mmio = 3,
    dma_buffer = 4,
    dma_mapping = 5,
    irq = 6,
    event_queue = 7,
};

pub const CapsuleState = enum(u8) {
    empty = 0,
    active = 1,
    revoked = 2,
};

comptime {
    std.debug.assert(known_rights_mask == capsule_abi.known_rights_mask);
    std.debug.assert(@intFromEnum(CapsuleKind.session) == @intFromEnum(capsule_abi.CapsuleKind.session));
    std.debug.assert(@intFromEnum(CapsuleKind.device) == @intFromEnum(capsule_abi.CapsuleKind.device));
    std.debug.assert(@intFromEnum(CapsuleKind.mmio) == @intFromEnum(capsule_abi.CapsuleKind.mmio));
    std.debug.assert(@intFromEnum(CapsuleKind.dma_buffer) == @intFromEnum(capsule_abi.CapsuleKind.dma_buffer));
    std.debug.assert(@intFromEnum(CapsuleKind.dma_mapping) == @intFromEnum(capsule_abi.CapsuleKind.dma_mapping));
    std.debug.assert(@intFromEnum(CapsuleKind.irq) == @intFromEnum(capsule_abi.CapsuleKind.irq));
    std.debug.assert(@intFromEnum(CapsuleKind.event_queue) == @intFromEnum(capsule_abi.CapsuleKind.event_queue));
    std.debug.assert(@intFromEnum(CapsuleState.active) == @intFromEnum(capsule_abi.CapsuleState.active));
    std.debug.assert(@intFromEnum(CapsuleState.revoked) == @intFromEnum(capsule_abi.CapsuleState.revoked));
}

pub const Rights = packed struct(u64) {
    query: bool = false,
    config_read: bool = false,
    config_write: bool = false,
    bar_info: bool = false,
    bar_map: bool = false,
    dma_alloc: bool = false,
    dma_map_user: bool = false,
    irq_bind: bool = false,
    bus_master: bool = false,
    reset: bool = false,
    power: bool = false,
    hotplug_observe: bool = false,
    grant: bool = false,
    _reserved: u51 = 0,
};

pub const Metadata = struct {
    device: u64 = 0,
    object_id: u64 = 0,
    user_va: u64 = 0,
    iova: u64 = 0,
    size: u64 = 0,
    index: u32 = 0,
    flags: u32 = 0,
};

pub const DmaDirection = enum(u2) {
    to_device = 1,
    from_device = 2,
    bidirectional = 3,
};

pub const IrqKind = enum(u2) {
    auto = 0,
    intx = 1,
    msi = 2,
    msix = 3,
};

pub const Capsule = struct {
    valid: bool = false,
    token: u64 = invalid_capsule,
    root_token: u64 = invalid_capsule,
    parent_token: u64 = invalid_capsule,
    owner_principal_raw: u8 = 0,
    generation: u64 = 0,
    revoke_generation: u64 = 0,
    kind: CapsuleKind = .none,
    state: CapsuleState = .empty,
    rights: Rights = .{},
    metadata: Metadata = .{},
};

pub const Snapshot = struct {
    token: u64 = invalid_capsule,
    root_token: u64 = invalid_capsule,
    parent_token: u64 = invalid_capsule,
    owner_principal_raw: u8 = 0,
    generation: u64 = 0,
    revoke_generation: u64 = 0,
    kind: CapsuleKind = .none,
    state: CapsuleState = .empty,
    rights: Rights = .{},
    metadata: Metadata = .{},
};

pub const CapsuleError = error{
    InvalidState,
    TableFull,
    NotFound,
    Denied,
    Revoked,
};

pub fn rightsFromBits(bits: u64) Rights {
    return @bitCast(bits & known_rights_mask);
}

pub fn rightsToBits(rights: Rights) u64 {
    return @as(u64, @bitCast(rights)) & known_rights_mask;
}

pub fn isRightsSubset(child: Rights, parent: Rights) bool {
    const child_bits = rightsToBits(child);
    const parent_bits = rightsToBits(parent);
    return (child_bits & ~parent_bits) == 0;
}

pub const CapsuleTable = struct {
    pub const max_capsules = 1024;

    entries: [max_capsules]Capsule = [_]Capsule{.{}} ** max_capsules,
    next_token: u64 = 1,
    next_generation: u64 = 1,
    next_revoke_generation: u64 = 1,

    fn allocToken(self: *CapsuleTable) u64 {
        var token = self.next_token;
        self.next_token +%= 1;
        if (token == invalid_capsule) {
            token = self.next_token;
            self.next_token +%= 1;
            if (token == invalid_capsule) token = 1;
        }
        return token;
    }

    fn allocGeneration(self: *CapsuleTable) u64 {
        var generation = self.next_generation;
        self.next_generation +%= 1;
        if (generation == 0) {
            generation = self.next_generation;
            self.next_generation +%= 1;
            if (generation == 0) generation = 1;
        }
        return generation;
    }

    fn allocRevokeGeneration(self: *CapsuleTable) u64 {
        var generation = self.next_revoke_generation;
        self.next_revoke_generation +%= 1;
        if (generation == 0) {
            generation = self.next_revoke_generation;
            self.next_revoke_generation +%= 1;
            if (generation == 0) generation = 1;
        }
        return generation;
    }

    fn firstFree(self: *CapsuleTable) ?usize {
        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (!self.entries[i].valid) return i;
        }
        return null;
    }

    fn findIndexByToken(self: *const CapsuleTable, token: u64) ?usize {
        if (token == invalid_capsule) return null;
        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (!self.entries[i].valid) continue;
            if (self.entries[i].token == token) return i;
        }
        return null;
    }

    pub fn findByToken(self: *const CapsuleTable, token: u64) ?*const Capsule {
        const index = self.findIndexByToken(token) orelse return null;
        return &self.entries[index];
    }

    fn findByTokenMut(self: *CapsuleTable, token: u64) ?*Capsule {
        const index = self.findIndexByToken(token) orelse return null;
        return &self.entries[index];
    }

    fn allocWithLineage(
        self: *CapsuleTable,
        owner_principal_raw: u8,
        kind: CapsuleKind,
        rights: Rights,
        metadata: Metadata,
        root_token_hint: u64,
        parent_token: u64,
    ) CapsuleError!u64 {
        if (kind == .none) return CapsuleError.InvalidState;
        if (parent_token != invalid_capsule) {
            const parent = self.findByToken(parent_token) orelse return CapsuleError.NotFound;
            if (parent.state == .revoked) return CapsuleError.Revoked;
            if (parent.state != .active) return CapsuleError.InvalidState;
            if (!isRightsSubset(rights, parent.rights)) return CapsuleError.Denied;
        }

        const index = self.firstFree() orelse return CapsuleError.TableFull;
        const token = self.allocToken();
        self.entries[index] = .{
            .valid = true,
            .token = token,
            .root_token = if (root_token_hint == invalid_capsule) token else root_token_hint,
            .parent_token = parent_token,
            .owner_principal_raw = owner_principal_raw,
            .generation = self.allocGeneration(),
            .kind = kind,
            .state = .active,
            .rights = rightsFromBits(rightsToBits(rights)),
            .metadata = metadata,
        };
        return token;
    }

    pub fn allocRoot(
        self: *CapsuleTable,
        owner_principal_raw: u8,
        kind: CapsuleKind,
        rights: Rights,
        metadata: Metadata,
    ) CapsuleError!u64 {
        return self.allocWithLineage(
            owner_principal_raw,
            kind,
            rights,
            metadata,
            invalid_capsule,
            invalid_capsule,
        );
    }

    pub fn derive(
        self: *CapsuleTable,
        owner_principal_raw: u8,
        parent_token: u64,
        kind: CapsuleKind,
        rights: Rights,
        metadata: Metadata,
    ) CapsuleError!u64 {
        const parent = self.findByToken(parent_token) orelse return CapsuleError.NotFound;
        if (parent.owner_principal_raw != owner_principal_raw) return CapsuleError.Denied;
        return self.allocWithLineage(
            owner_principal_raw,
            kind,
            rights,
            metadata,
            parent.root_token,
            parent.token,
        );
    }

    pub fn grant(
        self: *CapsuleTable,
        owner_principal_raw: u8,
        child_owner_principal_raw: u8,
        token: u64,
        rights: Rights,
    ) CapsuleError!u64 {
        const cap = self.findByToken(token) orelse return CapsuleError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return CapsuleError.Denied;
        if (cap.state == .revoked) return CapsuleError.Revoked;
        if (cap.state != .active) return CapsuleError.InvalidState;
        if (!cap.rights.grant) return CapsuleError.Denied;
        if (!isRightsSubset(rights, cap.rights)) return CapsuleError.Denied;
        return self.allocWithLineage(
            child_owner_principal_raw,
            cap.kind,
            rights,
            cap.metadata,
            cap.root_token,
            cap.token,
        );
    }

    pub fn authorize(
        self: *const CapsuleTable,
        owner_principal_raw: u8,
        token: u64,
        kind: CapsuleKind,
        required_rights: Rights,
    ) CapsuleError!void {
        const cap = self.findByToken(token) orelse return CapsuleError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return CapsuleError.Denied;
        if (cap.state == .revoked) return CapsuleError.Revoked;
        if (cap.state != .active) return CapsuleError.InvalidState;
        if (kind != .none and cap.kind != kind) return CapsuleError.InvalidState;
        if (!isRightsSubset(required_rights, cap.rights)) return CapsuleError.Denied;
    }

    pub fn snapshot(self: *const CapsuleTable, token: u64) CapsuleError!Snapshot {
        const cap = self.findByToken(token) orelse return CapsuleError.NotFound;
        return .{
            .token = cap.token,
            .root_token = cap.root_token,
            .parent_token = cap.parent_token,
            .owner_principal_raw = cap.owner_principal_raw,
            .generation = cap.generation,
            .revoke_generation = cap.revoke_generation,
            .kind = cap.kind,
            .state = cap.state,
            .rights = cap.rights,
            .metadata = cap.metadata,
        };
    }

    pub fn revokeSubtree(self: *CapsuleTable, owner_principal_raw: u8, token: u64) CapsuleError!usize {
        const start = self.findByToken(token) orelse return CapsuleError.NotFound;
        if (start.owner_principal_raw != owner_principal_raw) return CapsuleError.Denied;
        const root_token = start.root_token;
        const ancestor_parent = start.parent_token;
        const generation = self.allocRevokeGeneration();
        var count: usize = 0;

        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            const entry = &self.entries[i];
            if (!entry.valid) continue;
            if (entry.root_token != root_token) continue;
            if (entry.token != token and !isDescendantToken(self.entries[0..], entry.parent_token, token, ancestor_parent)) continue;
            if (entry.state == .revoked) continue;
            entry.state = .revoked;
            entry.revoke_generation = generation;
            count += 1;
        }
        return count;
    }

    pub fn closeSubtree(self: *CapsuleTable, owner_principal_raw: u8, token: u64) CapsuleError!usize {
        const start = self.findByToken(token) orelse return CapsuleError.NotFound;
        if (start.owner_principal_raw != owner_principal_raw) return CapsuleError.Denied;
        const root_token = start.root_token;
        const ancestor_parent = start.parent_token;
        var count: usize = 0;

        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            const entry = self.entries[i];
            if (!entry.valid) continue;
            if (entry.root_token != root_token) continue;
            if (entry.token != token and !isDescendantToken(self.entries[0..], entry.parent_token, token, ancestor_parent)) continue;
            self.entries[i] = .{};
            count += 1;
        }
        return count;
    }

    pub fn releaseOwner(self: *CapsuleTable, owner_principal_raw: u8) usize {
        var total: usize = 0;
        while (true) {
            var token: u64 = invalid_capsule;
            for (self.entries) |entry| {
                if (!entry.valid) continue;
                if (entry.owner_principal_raw != owner_principal_raw) continue;
                token = entry.token;
                break;
            }
            if (token == invalid_capsule) break;
            total += self.closeSubtree(owner_principal_raw, token) catch 0;
        }
        return total;
    }

    pub fn activeCount(self: *const CapsuleTable) usize {
        var count: usize = 0;
        for (self.entries) |entry| {
            if (entry.valid and entry.state == .active) count += 1;
        }
        return count;
    }

    pub fn validCount(self: *const CapsuleTable) usize {
        var count: usize = 0;
        for (self.entries) |entry| {
            if (entry.valid) count += 1;
        }
        return count;
    }
};

fn isDescendantToken(entries: []const Capsule, start_parent_token: u64, ancestor_token: u64, ancestor_parent_token: u64) bool {
    if (start_parent_token == invalid_capsule) return false;
    var current = start_parent_token;
    var guard: usize = 0;
    while (guard < entries.len) : (guard += 1) {
        if (current == ancestor_token) return true;
        if (current == invalid_capsule or current == ancestor_parent_token) return false;
        var next_parent: u64 = invalid_capsule;
        for (entries) |entry| {
            if (!entry.valid) continue;
            if (entry.token != current) continue;
            next_parent = entry.parent_token;
            break;
        }
        if (next_parent == invalid_capsule) return false;
        current = next_parent;
    }
    return false;
}

test "capsule table derives grants revokes and closes lineage" {
    var table = CapsuleTable{};
    const root_rights = Rights{
        .query = true,
        .config_read = true,
        .config_write = true,
        .bar_map = true,
        .grant = true,
    };
    const root = try table.allocRoot(0, .device, root_rights, .{ .device = 0x1001 });
    const mmio = try table.derive(0, root, .mmio, .{ .query = true, .bar_map = true }, .{
        .device = 0x1001,
        .index = 0,
        .size = 0x4000,
    });
    const child = try table.grant(0, 1, root, .{ .query = true, .config_read = true });

    try table.authorize(0, mmio, .mmio, .{ .bar_map = true });
    try table.authorize(1, child, .device, .{ .config_read = true });
    try std.testing.expectError(CapsuleError.Denied, table.authorize(1, child, .device, .{ .config_write = true }));

    try std.testing.expectEqual(@as(usize, 3), try table.revokeSubtree(0, root));
    try std.testing.expectError(CapsuleError.Revoked, table.authorize(1, child, .device, .{ .query = true }));
    try std.testing.expectEqual(CapsuleState.revoked, (try table.snapshot(mmio)).state);

    try std.testing.expectEqual(@as(usize, 3), try table.closeSubtree(0, root));
    try std.testing.expectEqual(@as(usize, 0), table.validCount());
}
