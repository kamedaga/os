pub const DmaDeviceId = u64;
pub const invalid_dma_device_id: DmaDeviceId = 0;

pub const DmaDirection = enum(u8) {
    read,
    write,
    bidirectional,
};

pub const DmaMappingState = enum(u8) {
    mapped,
    in_flight,
    completed,
};

pub const DmaMapping = struct {
    valid: bool = false,
    token: u64 = 0,
    owner_principal_raw: u32 = 0,
    device: DmaDeviceId = invalid_dma_device_id,
    paddr_start: u64 = 0,
    length: u64 = 0,
    direction: DmaDirection = .bidirectional,
    state: DmaMappingState = .mapped,
};

pub const DeviceDomainBinding = struct {
    valid: bool = false,
    device: DmaDeviceId = invalid_dma_device_id,
    domain_id: u32 = 0,
};

pub const QueueOperation = enum(u8) {
    submit,
    notify,
};

pub const IommuOperation = enum(u8) {
    map_read,
    map_write,
    map_status,
};

pub const QueueCapability = struct {
    valid: bool = false,
    token: u64 = 0,
    root_token: u64 = 0,
    parent_token: u64 = 0,
    owner_principal_raw: u32 = 0,
    device: DmaDeviceId = invalid_dma_device_id,
    queue_index: u16 = 0,
    allow_submit: bool = false,
    allow_notify: bool = false,
};

pub const IommuCapability = struct {
    valid: bool = false,
    token: u64 = 0,
    root_token: u64 = 0,
    parent_token: u64 = 0,
    owner_principal_raw: u32 = 0,
    device: DmaDeviceId = invalid_dma_device_id,
    allow_map_read: bool = false,
    allow_map_write: bool = false,
    allow_map_status: bool = false,
};

pub const CommandOpcodeClass = enum(u8) {
    blk_read,
    blk_write,
    blk_flush,
    blk_identify,
    gpu_admin,
    gpu_resource_2d,
    gpu_scanout,
    gpu_cursor,
    gpu_virgl_context,
    gpu_virgl_resource,
    gpu_virgl_submit,
    gpu_fence,
};

pub const CommandCapability = struct {
    valid: bool = false,
    token: u64 = 0,
    root_token: u64 = 0,
    parent_token: u64 = 0,
    owner_principal_raw: u32 = 0,
    device: DmaDeviceId = invalid_dma_device_id,
    opcode_mask: u64 = 0,
};

pub const DmaMappingError = error{
    InvalidState,
    TableFull,
    NotFound,
    Denied,
};

pub const DmaMappingTable = struct {
    pub const max_mappings = 1024;

    entries: [max_mappings]DmaMapping = [_]DmaMapping{.{}} ** max_mappings,
    next_token: u64 = 1,

    pub fn alloc(
        self: *DmaMappingTable,
        owner_principal_raw: u32,
        device: DmaDeviceId,
        paddr_start: u64,
        length: u64,
        direction: DmaDirection,
    ) DmaMappingError!u64 {
        if (device == invalid_dma_device_id or paddr_start == 0 or length == 0) return DmaMappingError.InvalidState;

        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (self.entries[i].valid) continue;
            const token = self.next_token;
            self.next_token +%= 1;
            self.entries[i] = .{
                .valid = true,
                .token = token,
                .owner_principal_raw = owner_principal_raw,
                .device = device,
                .paddr_start = paddr_start,
                .length = length,
                .direction = direction,
                .state = .mapped,
            };
            return token;
        }
        return DmaMappingError.TableFull;
    }

    pub fn findByToken(self: *const DmaMappingTable, token: u64) ?*const DmaMapping {
        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (!self.entries[i].valid) continue;
            if (self.entries[i].token == token) return &self.entries[i];
        }
        return null;
    }

    pub fn findByTokenMut(self: *DmaMappingTable, token: u64) ?*DmaMapping {
        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (!self.entries[i].valid) continue;
            if (self.entries[i].token == token) return &self.entries[i];
        }
        return null;
    }

    fn canTransition(from: DmaMappingState, to: DmaMappingState) bool {
        if (from == to) return true;
        return switch (from) {
            .mapped => to == .in_flight,
            .in_flight => to == .completed,
            .completed => false,
        };
    }

    pub fn setState(self: *DmaMappingTable, token: u64, state: DmaMappingState) DmaMappingError!void {
        const entry = self.findByTokenMut(token) orelse return DmaMappingError.NotFound;
        if (!canTransition(entry.state, state)) return DmaMappingError.InvalidState;
        entry.state = state;
    }

    pub fn release(self: *DmaMappingTable, token: u64) DmaMappingError!void {
        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (!self.entries[i].valid) continue;
            if (self.entries[i].token != token) continue;
            if (self.entries[i].state != .completed) return DmaMappingError.InvalidState;
            self.entries[i] = .{};
            return;
        }
        return DmaMappingError.NotFound;
    }

    pub fn remove(self: *DmaMappingTable, token: u64) bool {
        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (!self.entries[i].valid) continue;
            if (self.entries[i].token != token) continue;
            self.entries[i] = .{};
            return true;
        }
        return false;
    }
};

pub const IommuCapabilityTable = struct {
    pub const max_iommu_caps = 128;

    entries: [max_iommu_caps]IommuCapability = [_]IommuCapability{.{}} ** max_iommu_caps,
    next_token: u64 = 1,

    fn allocWithLineage(
        self: *IommuCapabilityTable,
        owner_principal_raw: u32,
        device: DmaDeviceId,
        allow_map_read: bool,
        allow_map_write: bool,
        allow_map_status: bool,
        root_token_hint: u64,
        parent_token: u64,
    ) DmaMappingError!u64 {
        if (device == invalid_dma_device_id or (!allow_map_read and !allow_map_write and !allow_map_status)) return DmaMappingError.InvalidState;

        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (self.entries[i].valid) continue;
            var token = self.next_token;
            self.next_token +%= 1;
            if (token == 0) {
                token = self.next_token;
                self.next_token +%= 1;
                if (token == 0) token = 1;
            }
            self.entries[i] = .{
                .valid = true,
                .token = token,
                .root_token = if (root_token_hint == 0) token else root_token_hint,
                .parent_token = parent_token,
                .owner_principal_raw = owner_principal_raw,
                .device = device,
                .allow_map_read = allow_map_read,
                .allow_map_write = allow_map_write,
                .allow_map_status = allow_map_status,
            };
            return token;
        }
        return DmaMappingError.TableFull;
    }

    pub fn alloc(
        self: *IommuCapabilityTable,
        owner_principal_raw: u32,
        device: DmaDeviceId,
        allow_map_read: bool,
        allow_map_write: bool,
        allow_map_status: bool,
    ) DmaMappingError!u64 {
        return self.allocWithLineage(
            owner_principal_raw,
            device,
            allow_map_read,
            allow_map_write,
            allow_map_status,
            0,
            0,
        );
    }

    pub fn findByToken(self: *const IommuCapabilityTable, token: u64) ?*const IommuCapability {
        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (!self.entries[i].valid) continue;
            if (self.entries[i].token == token) return &self.entries[i];
        }
        return null;
    }

    pub fn authorize(
        self: *const IommuCapabilityTable,
        owner_principal_raw: u32,
        token: u64,
        device: DmaDeviceId,
        op: IommuOperation,
    ) DmaMappingError!void {
        const cap = self.findByToken(token) orelse return DmaMappingError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return DmaMappingError.Denied;
        if (cap.device != device) return DmaMappingError.Denied;
        switch (op) {
            .map_read => if (!cap.allow_map_read) return DmaMappingError.Denied,
            .map_write => if (!cap.allow_map_write) return DmaMappingError.Denied,
            .map_status => if (!cap.allow_map_status) return DmaMappingError.Denied,
        }
    }

    pub fn grant(
        self: *IommuCapabilityTable,
        owner_principal_raw: u32,
        child_owner_principal_raw: u32,
        token: u64,
    ) DmaMappingError!u64 {
        const cap = self.findByToken(token) orelse return DmaMappingError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return DmaMappingError.Denied;
        return self.allocWithLineage(
            child_owner_principal_raw,
            cap.device,
            cap.allow_map_read,
            cap.allow_map_write,
            cap.allow_map_status,
            cap.root_token,
            cap.token,
        );
    }

    pub fn revokeSubtree(self: *IommuCapabilityTable, owner_principal_raw: u32, token: u64) DmaMappingError!usize {
        const cap = self.findByToken(token) orelse return DmaMappingError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return DmaMappingError.Denied;
        const root_token = cap.root_token;
        const subtree_parent = cap.parent_token;
        var removed: usize = 0;
        var changed = true;
        while (changed) {
            changed = false;
            var i: usize = 0;
            while (i < self.entries.len) : (i += 1) {
                const entry = self.entries[i];
                if (!entry.valid) continue;
                if (entry.root_token != root_token) continue;
                if (entry.token != token and !isDescendantToken(self.entries[0..], entry.parent_token, token, subtree_parent)) continue;
                self.entries[i] = .{};
                removed += 1;
                changed = true;
            }
        }
        return removed;
    }
};

pub const DeviceDomainTable = struct {
    pub const max_bindings = 16;

    bindings: [max_bindings]DeviceDomainBinding = [_]DeviceDomainBinding{.{}} ** max_bindings,

    pub fn bind(self: *DeviceDomainTable, device: DmaDeviceId, domain_id: u32) DmaMappingError!void {
        var i: usize = 0;
        while (i < self.bindings.len) : (i += 1) {
            if (!self.bindings[i].valid) continue;
            if (self.bindings[i].device != device) continue;
            self.bindings[i].domain_id = domain_id;
            return;
        }

        i = 0;
        while (i < self.bindings.len) : (i += 1) {
            if (self.bindings[i].valid) continue;
            self.bindings[i] = .{
                .valid = true,
                .device = device,
                .domain_id = domain_id,
            };
            return;
        }
        return DmaMappingError.TableFull;
    }

    pub fn domainFor(self: *const DeviceDomainTable, device: DmaDeviceId) ?u32 {
        var i: usize = 0;
        while (i < self.bindings.len) : (i += 1) {
            if (!self.bindings[i].valid) continue;
            if (self.bindings[i].device == device) return self.bindings[i].domain_id;
        }
        return null;
    }
};

pub const QueueCapabilityTable = struct {
    pub const max_queue_caps = 128;

    entries: [max_queue_caps]QueueCapability = [_]QueueCapability{.{}} ** max_queue_caps,
    next_token: u64 = 1,

    fn allocWithLineage(
        self: *QueueCapabilityTable,
        owner_principal_raw: u32,
        device: DmaDeviceId,
        queue_index: u16,
        allow_submit: bool,
        allow_notify: bool,
        root_token_hint: u64,
        parent_token: u64,
    ) DmaMappingError!u64 {
        if (device == invalid_dma_device_id or (!allow_submit and !allow_notify)) return DmaMappingError.InvalidState;

        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (self.entries[i].valid) continue;
            var token = self.next_token;
            self.next_token +%= 1;
            if (token == 0) {
                token = self.next_token;
                self.next_token +%= 1;
                if (token == 0) token = 1;
            }
            self.entries[i] = .{
                .valid = true,
                .token = token,
                .root_token = if (root_token_hint == 0) token else root_token_hint,
                .parent_token = parent_token,
                .owner_principal_raw = owner_principal_raw,
                .device = device,
                .queue_index = queue_index,
                .allow_submit = allow_submit,
                .allow_notify = allow_notify,
            };
            return token;
        }
        return DmaMappingError.TableFull;
    }

    pub fn alloc(
        self: *QueueCapabilityTable,
        owner_principal_raw: u32,
        device: DmaDeviceId,
        queue_index: u16,
        allow_submit: bool,
        allow_notify: bool,
    ) DmaMappingError!u64 {
        return self.allocWithLineage(
            owner_principal_raw,
            device,
            queue_index,
            allow_submit,
            allow_notify,
            0,
            0,
        );
    }

    pub fn findByToken(self: *const QueueCapabilityTable, token: u64) ?*const QueueCapability {
        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (!self.entries[i].valid) continue;
            if (self.entries[i].token == token) return &self.entries[i];
        }
        return null;
    }

    pub fn authorize(
        self: *const QueueCapabilityTable,
        owner_principal_raw: u32,
        token: u64,
        device: DmaDeviceId,
        queue_index: u16,
        op: QueueOperation,
    ) DmaMappingError!void {
        const cap = self.findByToken(token) orelse return DmaMappingError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return DmaMappingError.Denied;
        if (cap.device != device) return DmaMappingError.Denied;
        if (cap.queue_index != queue_index) return DmaMappingError.Denied;
        switch (op) {
            .submit => if (!cap.allow_submit) return DmaMappingError.Denied,
            .notify => if (!cap.allow_notify) return DmaMappingError.Denied,
        }
    }

    pub fn grant(
        self: *QueueCapabilityTable,
        owner_principal_raw: u32,
        child_owner_principal_raw: u32,
        token: u64,
    ) DmaMappingError!u64 {
        const cap = self.findByToken(token) orelse return DmaMappingError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return DmaMappingError.Denied;
        return self.allocWithLineage(
            child_owner_principal_raw,
            cap.device,
            cap.queue_index,
            cap.allow_submit,
            cap.allow_notify,
            cap.root_token,
            cap.token,
        );
    }

    pub fn revokeSubtree(self: *QueueCapabilityTable, owner_principal_raw: u32, token: u64) DmaMappingError!usize {
        const cap = self.findByToken(token) orelse return DmaMappingError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return DmaMappingError.Denied;
        const root_token = cap.root_token;
        const subtree_parent = cap.parent_token;
        var removed: usize = 0;
        var changed = true;
        while (changed) {
            changed = false;
            var i: usize = 0;
            while (i < self.entries.len) : (i += 1) {
                const entry = self.entries[i];
                if (!entry.valid) continue;
                if (entry.root_token != root_token) continue;
                if (entry.token != token and !isDescendantToken(self.entries[0..], entry.parent_token, token, subtree_parent)) continue;
                self.entries[i] = .{};
                removed += 1;
                changed = true;
            }
        }
        return removed;
    }
};

pub const CommandCapabilityTable = struct {
    pub const max_command_caps = 128;

    entries: [max_command_caps]CommandCapability = [_]CommandCapability{.{}} ** max_command_caps,
    next_token: u64 = 1,

    fn allocWithLineage(
        self: *CommandCapabilityTable,
        owner_principal_raw: u32,
        device: DmaDeviceId,
        opcode_mask: u64,
        root_token_hint: u64,
        parent_token: u64,
    ) DmaMappingError!u64 {
        if (device == invalid_dma_device_id or opcode_mask == 0) return DmaMappingError.InvalidState;

        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (self.entries[i].valid) continue;
            var token = self.next_token;
            self.next_token +%= 1;
            if (token == 0) {
                token = self.next_token;
                self.next_token +%= 1;
                if (token == 0) token = 1;
            }
            self.entries[i] = .{
                .valid = true,
                .token = token,
                .root_token = if (root_token_hint == 0) token else root_token_hint,
                .parent_token = parent_token,
                .owner_principal_raw = owner_principal_raw,
                .device = device,
                .opcode_mask = opcode_mask,
            };
            return token;
        }
        return DmaMappingError.TableFull;
    }

    pub fn alloc(
        self: *CommandCapabilityTable,
        owner_principal_raw: u32,
        device: DmaDeviceId,
        opcode_mask: u64,
    ) DmaMappingError!u64 {
        return self.allocWithLineage(owner_principal_raw, device, opcode_mask, 0, 0);
    }

    pub fn findByToken(self: *const CommandCapabilityTable, token: u64) ?*const CommandCapability {
        var i: usize = 0;
        while (i < self.entries.len) : (i += 1) {
            if (!self.entries[i].valid) continue;
            if (self.entries[i].token == token) return &self.entries[i];
        }
        return null;
    }

    pub fn authorize(
        self: *const CommandCapabilityTable,
        owner_principal_raw: u32,
        token: u64,
        device: DmaDeviceId,
        opcode: CommandOpcodeClass,
    ) DmaMappingError!void {
        const cap = self.findByToken(token) orelse return DmaMappingError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return DmaMappingError.Denied;
        if (cap.device != device) return DmaMappingError.Denied;
        const bit: u64 = @as(u64, 1) << @as(u6, @intCast(@intFromEnum(opcode)));
        if ((cap.opcode_mask & bit) == 0) return DmaMappingError.Denied;
    }

    pub fn grant(
        self: *CommandCapabilityTable,
        owner_principal_raw: u32,
        child_owner_principal_raw: u32,
        token: u64,
    ) DmaMappingError!u64 {
        const cap = self.findByToken(token) orelse return DmaMappingError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return DmaMappingError.Denied;
        return self.allocWithLineage(child_owner_principal_raw, cap.device, cap.opcode_mask, cap.root_token, cap.token);
    }

    pub fn deriveSubset(
        self: *CommandCapabilityTable,
        owner_principal_raw: u32,
        token: u64,
        opcode_mask: u64,
    ) DmaMappingError!u64 {
        const cap = self.findByToken(token) orelse return DmaMappingError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return DmaMappingError.Denied;
        if (opcode_mask == 0 or (opcode_mask & ~cap.opcode_mask) != 0) return DmaMappingError.Denied;
        return self.allocWithLineage(owner_principal_raw, cap.device, opcode_mask, cap.root_token, cap.token);
    }

    pub fn revokeSubtree(self: *CommandCapabilityTable, owner_principal_raw: u32, token: u64) DmaMappingError!usize {
        const cap = self.findByToken(token) orelse return DmaMappingError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return DmaMappingError.Denied;
        const root_token = cap.root_token;
        const subtree_parent = cap.parent_token;
        var removed: usize = 0;
        var changed = true;
        while (changed) {
            changed = false;
            var i: usize = 0;
            while (i < self.entries.len) : (i += 1) {
                const entry = self.entries[i];
                if (!entry.valid) continue;
                if (entry.root_token != root_token) continue;
                if (entry.token != token and !isDescendantToken(self.entries[0..], entry.parent_token, token, subtree_parent)) continue;
                self.entries[i] = .{};
                removed += 1;
                changed = true;
            }
        }
        return removed;
    }
};

fn isDescendantToken(entries: anytype, start_parent_token: u64, ancestor_token: u64, ancestor_parent_token: u64) bool {
    if (start_parent_token == 0) return false;
    var current = start_parent_token;
    while (current != 0) {
        if (current == ancestor_token) return true;
        if (current == ancestor_parent_token) return false;
        var next_parent: u64 = 0;
        var found = false;
        for (entries) |entry| {
            if (!entry.valid or entry.token != current) continue;
            next_parent = entry.parent_token;
            found = true;
            break;
        }
        if (!found) return false;
        current = next_parent;
    }
    return false;
}
