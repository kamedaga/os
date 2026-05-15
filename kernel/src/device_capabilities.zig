const kernel = @import("kernel.zig");
const dma_mapping_manager = @import("dma_mapping_manager.zig");

pub const IommuOperation = dma_mapping_manager.IommuOperation;
pub const IommuCapability = dma_mapping_manager.IommuCapability;
pub const IommuCapabilityTable = dma_mapping_manager.IommuCapabilityTable;
pub const QueueOperation = dma_mapping_manager.QueueOperation;
pub const QueueCapability = dma_mapping_manager.QueueCapability;
pub const QueueCapabilityTable = dma_mapping_manager.QueueCapabilityTable;
pub const CommandOpcodeClass = dma_mapping_manager.CommandOpcodeClass;
pub const CommandCapability = dma_mapping_manager.CommandCapability;
pub const CommandCapabilityTable = dma_mapping_manager.CommandCapabilityTable;

pub fn principalHasIommuCapForDevice(state: *const kernel.KernelState, principal: kernel.PrincipalId, device: kernel.DmaDeviceId) bool {
    const owner_raw: u8 = @intCast(@intFromEnum(principal));
    for (state.iommu_caps.entries) |cap| {
        if (!cap.valid) continue;
        if (cap.owner_principal_raw != owner_raw) continue;
        if (cap.device == device) return true;
    }
    return false;
}

pub fn principalHasQueueCapForDevice(state: *const kernel.KernelState, principal: kernel.PrincipalId, device: kernel.DmaDeviceId) bool {
    const owner_raw: u8 = @intCast(@intFromEnum(principal));
    for (state.queue_caps.entries) |cap| {
        if (!cap.valid) continue;
        if (cap.owner_principal_raw != owner_raw) continue;
        if (cap.device == device) return true;
    }
    return false;
}

fn syncIommuForPrincipalDevicePaddr(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    device: kernel.DmaDeviceId,
    paddr: u64,
    reason: kernel.IommuSyncReason,
) kernel.KernelError!void {
    const had_mapping = state.iommuFindMappingIndex(principal, device, paddr) != null;
    const cap = state.getTableConst(principal).find(paddr);
    if (cap) |c| {
        if (c.rights.dma and
            principalHasIommuCapForDevice(state, principal, device) and
            principalHasQueueCapForDevice(state, principal, device) and
            state.hasActiveDmaMappingForPrincipalDevicePaddr(principal, device, paddr))
        {
            try state.iommuMap(principal, device, paddr);
            if (!had_mapping) {
                if (state.iommu_audit_hook) |hook| {
                    hook(state, principal, paddr, true, reason);
                }
            }
            return;
        }
    }
    state.iommuUnmap(principal, device, paddr);
    if (had_mapping) {
        if (state.iommu_audit_hook) |hook| {
            hook(state, principal, paddr, false, reason);
        }
    }
}

pub fn syncIommuForPrincipalPaddr(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    paddr: u64,
    reason: kernel.IommuSyncReason,
) kernel.KernelError!void {
    if (state.iommu.mode == .off) return;
    const owner_raw: u8 = @intCast(@intFromEnum(principal));
    for (state.iommu_caps.entries) |cap| {
        if (!cap.valid or cap.owner_principal_raw != owner_raw) continue;
        try syncIommuForPrincipalDevicePaddr(state, principal, cap.device, paddr, reason);
    }
    for (state.queue_caps.entries) |cap| {
        if (!cap.valid or cap.owner_principal_raw != owner_raw) continue;
        try syncIommuForPrincipalDevicePaddr(state, principal, cap.device, paddr, reason);
    }
}

pub fn iommuHasMappingForPrincipalForTest(state: *const kernel.KernelState, principal: kernel.PrincipalId, paddr: u64) bool {
    for (state.iommu.mappings) |entry| {
        if (!entry.valid) continue;
        if (entry.principal != principal or entry.paddr != paddr) continue;
        return true;
    }
    return false;
}

pub fn syncAllIommuForPrincipal(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    reason: kernel.IommuSyncReason,
) kernel.KernelError!void {
    const table = state.getTableConst(principal);
    var i: usize = 0;
    while (i < table.len) : (i += 1) {
        const cap = table.get(i) orelse break;
        try syncIommuForPrincipalPaddr(state, principal, cap.paddr, reason);
    }
}

pub fn iommuCapGrantStage2(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    device: kernel.DmaDeviceId,
    allow_map_read: bool,
    allow_map_write: bool,
    allow_map_status: bool,
) kernel.KernelError!u64 {
    try state.requireActiveProcess(owner);
    const token = state.iommu_caps.alloc(
        @intFromEnum(owner),
        device,
        allow_map_read,
        allow_map_write,
        allow_map_status,
    ) catch |err| switch (err) {
        error.InvalidState => kernel.KernelError.InvalidState,
        error.TableFull => kernel.KernelError.TableFull,
        else => kernel.KernelError.InvalidState,
    };
    try syncAllIommuForPrincipal(state, owner, .grant_dma);
    return token;
}

pub fn iommuCapAuthorizeStage2(
    state: *const kernel.KernelState,
    owner: kernel.PrincipalId,
    token: u64,
    device: kernel.DmaDeviceId,
    op: IommuOperation,
) kernel.KernelError!void {
    try state.requireActiveProcess(owner);
    state.iommu_caps.authorize(@intFromEnum(owner), token, device, op) catch |err| switch (err) {
        error.NotFound => return kernel.KernelError.CapabilityNotFound,
        error.Denied => return kernel.KernelError.InvalidState,
        error.InvalidState => return kernel.KernelError.InvalidState,
        error.TableFull => return kernel.KernelError.TableFull,
    };
}

pub fn grantIommuCapStage2(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    child: kernel.PrincipalId,
    token: u64,
) kernel.KernelError!u64 {
    try state.requireActiveProcess(owner);
    try state.requireActiveProcess(child);
    const child_token = state.iommu_caps.grant(
        @intFromEnum(owner),
        @intFromEnum(child),
        token,
    ) catch |err| switch (err) {
        error.NotFound => return kernel.KernelError.CapabilityNotFound,
        error.Denied => return kernel.KernelError.InvalidState,
        error.InvalidState => return kernel.KernelError.InvalidState,
        error.TableFull => return kernel.KernelError.TableFull,
    };
    try syncAllIommuForPrincipal(state, child, .grant_dma);
    return child_token;
}

pub fn queueCapGrantStage2(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    device: kernel.DmaDeviceId,
    queue_index: u16,
    allow_submit: bool,
    allow_notify: bool,
) kernel.KernelError!u64 {
    try state.requireActiveProcess(owner);
    const token = state.queue_caps.alloc(
        @intFromEnum(owner),
        device,
        queue_index,
        allow_submit,
        allow_notify,
    ) catch |err| switch (err) {
        error.InvalidState => kernel.KernelError.InvalidState,
        error.TableFull => kernel.KernelError.TableFull,
        else => kernel.KernelError.InvalidState,
    };
    try syncAllIommuForPrincipal(state, owner, .grant_dma);
    return token;
}

pub fn queueCapAuthorizeStage2(
    state: *const kernel.KernelState,
    owner: kernel.PrincipalId,
    token: u64,
    queue_index: u16,
    op: QueueOperation,
) kernel.KernelError!void {
    try state.requireActiveProcess(owner);
    const cap = state.queue_caps.findByToken(token) orelse return kernel.KernelError.CapabilityNotFound;
    if (cap.owner_principal_raw != @intFromEnum(owner)) return kernel.KernelError.InvalidState;
    if (cap.queue_index != queue_index) return kernel.KernelError.InvalidState;
    switch (op) {
        .submit => if (!cap.allow_submit) return kernel.KernelError.InvalidState,
        .notify => if (!cap.allow_notify) return kernel.KernelError.InvalidState,
    }
}

pub fn queueCapDeviceForToken(
    state: *const kernel.KernelState,
    owner: kernel.PrincipalId,
    token: u64,
    queue_index: u16,
) ?kernel.DmaDeviceId {
    const cap = state.queue_caps.findByToken(token) orelse return null;
    if (cap.owner_principal_raw != @intFromEnum(owner)) return null;
    if (cap.queue_index != queue_index) return null;
    return cap.device;
}

pub fn grantQueueCapStage2(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    child: kernel.PrincipalId,
    token: u64,
) kernel.KernelError!u64 {
    try state.requireActiveProcess(owner);
    try state.requireActiveProcess(child);
    const child_token = state.queue_caps.grant(
        @intFromEnum(owner),
        @intFromEnum(child),
        token,
    ) catch |err| switch (err) {
        error.NotFound => return kernel.KernelError.CapabilityNotFound,
        error.Denied => return kernel.KernelError.InvalidState,
        error.InvalidState => return kernel.KernelError.InvalidState,
        error.TableFull => return kernel.KernelError.TableFull,
    };
    try syncAllIommuForPrincipal(state, child, .grant_dma);
    return child_token;
}

pub fn commandCapGrantStage2(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    device: kernel.DmaDeviceId,
    opcode_mask: u64,
) kernel.KernelError!u64 {
    try state.requireActiveProcess(owner);
    return state.command_caps.alloc(
        @intFromEnum(owner),
        device,
        opcode_mask,
    ) catch |err| switch (err) {
        error.InvalidState => kernel.KernelError.InvalidState,
        error.TableFull => kernel.KernelError.TableFull,
        else => kernel.KernelError.InvalidState,
    };
}

pub fn commandCapAuthorizeStage2(
    state: *const kernel.KernelState,
    owner: kernel.PrincipalId,
    token: u64,
    device: kernel.DmaDeviceId,
    opcode: CommandOpcodeClass,
) kernel.KernelError!void {
    try state.requireActiveProcess(owner);
    state.command_caps.authorize(@intFromEnum(owner), token, device, opcode) catch |err| switch (err) {
        error.NotFound => return kernel.KernelError.CapabilityNotFound,
        error.Denied => return kernel.KernelError.InvalidState,
        error.InvalidState => return kernel.KernelError.InvalidState,
        error.TableFull => return kernel.KernelError.TableFull,
    };
}

pub fn grantCommandCapStage2(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    child: kernel.PrincipalId,
    token: u64,
) kernel.KernelError!u64 {
    try state.requireActiveProcess(owner);
    try state.requireActiveProcess(child);
    return state.command_caps.grant(
        @intFromEnum(owner),
        @intFromEnum(child),
        token,
    ) catch |err| switch (err) {
        error.NotFound => return kernel.KernelError.CapabilityNotFound,
        error.Denied => return kernel.KernelError.InvalidState,
        error.InvalidState => return kernel.KernelError.InvalidState,
        error.TableFull => return kernel.KernelError.TableFull,
    };
}

pub fn deriveCommandCapStage2(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    token: u64,
    opcode_mask: u64,
) kernel.KernelError!u64 {
    try state.requireActiveProcess(owner);
    return state.command_caps.deriveSubset(
        @intFromEnum(owner),
        token,
        opcode_mask,
    ) catch |err| switch (err) {
        error.NotFound => kernel.KernelError.CapabilityNotFound,
        error.Denied => kernel.KernelError.InvalidState,
        error.InvalidState => kernel.KernelError.InvalidState,
        error.TableFull => kernel.KernelError.TableFull,
    };
}

pub fn revokeDeviceCapStage2(
    state: *kernel.KernelState,
    owner: kernel.PrincipalId,
    kind: @import("kernel_abi_root").queue_abi.CapabilityKind,
    token: u64,
) kernel.KernelError!void {
    try state.requireActiveProcess(owner);
    switch (kind) {
        .iommu => {
            const cap = state.iommu_caps.findByToken(token) orelse return kernel.KernelError.CapabilityNotFound;
            const device = cap.device;
            _ = state.iommu_caps.revokeSubtree(@intFromEnum(owner), token) catch |err| switch (err) {
                error.NotFound => return kernel.KernelError.CapabilityNotFound,
                error.Denied => return kernel.KernelError.InvalidState,
                error.InvalidState => return kernel.KernelError.InvalidState,
                error.TableFull => return kernel.KernelError.TableFull,
            };
            state.removeDmaMappingsForPrincipalDevice(owner, device);
        },
        .virtqueue => {
            const cap = state.queue_caps.findByToken(token) orelse return kernel.KernelError.CapabilityNotFound;
            const device = cap.device;
            _ = state.queue_caps.revokeSubtree(@intFromEnum(owner), token) catch |err| switch (err) {
                error.NotFound => return kernel.KernelError.CapabilityNotFound,
                error.Denied => return kernel.KernelError.InvalidState,
                error.InvalidState => return kernel.KernelError.InvalidState,
                error.TableFull => return kernel.KernelError.TableFull,
            };
            state.removeDmaMappingsForPrincipalDevice(owner, device);
        },
        .command => {
            _ = state.command_caps.revokeSubtree(@intFromEnum(owner), token) catch |err| switch (err) {
                error.NotFound => return kernel.KernelError.CapabilityNotFound,
                error.Denied => return kernel.KernelError.InvalidState,
                error.InvalidState => return kernel.KernelError.InvalidState,
                error.TableFull => return kernel.KernelError.TableFull,
            };
        },
    }
    try syncAllIommuForPrincipal(state, owner, .revoke);
}
