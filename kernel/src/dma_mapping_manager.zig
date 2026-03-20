pub const DmaDeviceId = enum(u8) {
    virtio_gpu,
    virtio_input,
};

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
    owner_principal_raw: u8 = 0,
    device: DmaDeviceId = .virtio_gpu,
    paddr_start: u64 = 0,
    length: u64 = 0,
    direction: DmaDirection = .bidirectional,
    state: DmaMappingState = .mapped,
};

pub const DeviceDomainBinding = struct {
    valid: bool = false,
    device: DmaDeviceId = .virtio_gpu,
    domain_id: u32 = 0,
};

pub const QueueOperation = enum(u8) {
    submit,
    notify,
};

pub const QueueCapability = struct {
    valid: bool = false,
    token: u64 = 0,
    owner_principal_raw: u8 = 0,
    device: DmaDeviceId = .virtio_gpu,
    queue_index: u16 = 0,
    allow_submit: bool = false,
    allow_notify: bool = false,
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
        owner_principal_raw: u8,
        device: DmaDeviceId,
        paddr_start: u64,
        length: u64,
        direction: DmaDirection,
    ) DmaMappingError!u64 {
        if (paddr_start == 0 or length == 0) return DmaMappingError.InvalidState;

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

    pub fn alloc(
        self: *QueueCapabilityTable,
        owner_principal_raw: u8,
        device: DmaDeviceId,
        queue_index: u16,
        allow_submit: bool,
        allow_notify: bool,
    ) DmaMappingError!u64 {
        if (!allow_submit and !allow_notify) return DmaMappingError.InvalidState;

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
        owner_principal_raw: u8,
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
        owner_principal_raw: u8,
        child_owner_principal_raw: u8,
        token: u64,
    ) DmaMappingError!u64 {
        const cap = self.findByToken(token) orelse return DmaMappingError.NotFound;
        if (cap.owner_principal_raw != owner_principal_raw) return DmaMappingError.Denied;
        return self.alloc(
            child_owner_principal_raw,
            cap.device,
            cap.queue_index,
            cap.allow_submit,
            cap.allow_notify,
        );
    }
};
