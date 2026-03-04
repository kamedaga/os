const kernel = @import("kernel.zig");
const interrupts = @import("interrupts.zig");

pub const UserAddressSpace = struct {
    pml4: [512]u64 align(4096) = [_]u64{0} ** 512,
    pdp: [512]u64 align(4096) = [_]u64{0} ** 512,
    pd: [512]u64 align(4096) = [_]u64{0} ** 512,
    pt: [512]u64 align(4096) = [_]u64{0} ** 512,
    cr3: u64 = 0,
};

pub const RuntimeConfig = struct {
    user_spaces: []UserAddressSpace,
    user_va: u64,
    physical_map_limit: u64,
    page_entries: usize,
    page_addr_mask: u64,
    page_present: u64,
    page_rw: u64,
    page_user: u64,
    canonical_user_limit_exclusive: u64,
    serial_write: *const fn (text: []const u8) void,
    print_hex: *const fn (value: u64) void,
    principal_label: *const fn (principal: kernel.PrincipalId) []const u8,
    flush_user_tlb_for_principal_va: *const fn (principal: kernel.PrincipalId, va: u64) void,
};

pub const PageFaultCapability = struct {
    principal: kernel.PrincipalId,
    fault_va: u64,
    fault_page_va: u64,
    fault_rip: u64,
    present_violation: bool,
    write_access: bool,
    instruction_fetch: bool,
    candidate_paddr: ?u64,
};

var runtime_ready = false;
var runtime: RuntimeConfig = undefined;

pub fn init(config: RuntimeConfig) void {
    runtime = config;
    runtime_ready = true;
}

fn processIndex(principal: kernel.PrincipalId) ?usize {
    return switch (principal) {
        .Process0 => 0,
        .Process1 => 1,
        else => null,
    };
}

fn principalFromProcessIndex(index: usize) ?kernel.PrincipalId {
    return switch (index) {
        0 => .Process0,
        1 => .Process1,
        else => null,
    };
}

fn getUserSpace(principal: kernel.PrincipalId) ?*UserAddressSpace {
    if (!runtime_ready) return null;
    const index = processIndex(principal) orelse return null;
    if (index >= runtime.user_spaces.len) return null;
    return &runtime.user_spaces[index];
}

pub fn isUserCanonicalVa(va: u64) bool {
    if (!runtime_ready) return false;
    return va < runtime.canonical_user_limit_exclusive;
}

pub fn lookupUserMappedPaddrForVa(principal: kernel.PrincipalId, va: u64) ?u64 {
    const space = getUserSpace(principal) orelse return null;
    const pml4_index: usize = @intCast((va >> 39) & 0x1FF);
    const pdp_index: usize = @intCast((va >> 30) & 0x1FF);
    const pd_index: usize = @intCast((va >> 21) & 0x1FF);
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const user_pdp_index: usize = @intCast((runtime.user_va >> 30) & 0x1FF);
    const user_pd_index: usize = @intCast((runtime.user_va >> 21) & 0x1FF);

    if (pml4_index != 0 or pdp_index != user_pdp_index or pd_index != user_pd_index) return null;

    const entry = space.pt[pt_index];
    const paddr = entry & runtime.page_addr_mask;
    if (paddr == 0) return null;
    return paddr;
}

pub fn parseRights(bits: u64) kernel.Rights {
    return .{
        .cpu_read = (bits & 0x1) != 0,
        .cpu_write = (bits & 0x2) != 0,
        .dma = (bits & 0x4) != 0,
    };
}

fn pageAlignDown(addr: u64) u64 {
    return addr & ~@as(u64, 4095);
}

pub fn issuePageFaultCapability(
    principal: kernel.PrincipalId,
    frame: *const interrupts.ExceptionTrapFrame,
    cr2: u64,
) ?PageFaultCapability {
    const ec = frame.error_code;
    const user_mode = (ec & (1 << 2)) != 0;
    if (!user_mode) return null;
    if (!isUserCanonicalVa(cr2)) return null;

    const fault_page_va = pageAlignDown(cr2);
    return .{
        .principal = principal,
        .fault_va = cr2,
        .fault_page_va = fault_page_va,
        .fault_rip = frame.rip,
        .present_violation = (ec & (1 << 0)) != 0,
        .write_access = (ec & (1 << 1)) != 0,
        .instruction_fetch = (ec & (1 << 4)) != 0,
        .candidate_paddr = lookupUserMappedPaddrForVa(principal, fault_page_va),
    };
}

pub fn resolvePageFaultCapability(state: *const kernel.KernelState, pf_cap: PageFaultCapability) bool {
    if (pf_cap.present_violation) return false;
    const candidate_paddr = pf_cap.candidate_paddr orelse return false;
    const cap = state.getTableConst(pf_cap.principal).find(candidate_paddr) orelse return false;
    if (!cap.rights.cpu_read) return false;
    if (pf_cap.write_access and !cap.rights.cpu_write) return false;
    if (pf_cap.instruction_fetch) return false;

    return mapUserPageFromCapability(
        state,
        pf_cap.principal,
        pf_cap.fault_page_va,
        candidate_paddr,
        cap.rights.cpu_write,
    );
}

pub fn mapUserPageFromCapability(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    va: u64,
    paddr: u64,
    writable: bool,
) bool {
    if (!runtime_ready) return false;
    const space = getUserSpace(principal) orelse return false;
    if ((va & 0xFFF) != 0) return false;
    if ((paddr & 0xFFF) != 0) return false;
    if (paddr >= runtime.physical_map_limit) return false;

    const pml4_index: usize = @intCast((va >> 39) & 0x1FF);
    const pdp_index: usize = @intCast((va >> 30) & 0x1FF);
    const pd_index: usize = @intCast((va >> 21) & 0x1FF);
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const user_pdp_index: usize = @intCast((runtime.user_va >> 30) & 0x1FF);
    const user_pd_index: usize = @intCast((runtime.user_va >> 21) & 0x1FF);
    if (pml4_index != 0 or pdp_index != user_pdp_index or pd_index != user_pd_index) return false;

    const cap = state.getTableConst(principal).find(paddr) orelse return false;
    if (!cap.rights.cpu_read) return false;
    if (writable and !cap.rights.cpu_write) return false;

    var i: usize = 0;
    while (i < runtime.page_entries) : (i += 1) {
        if (i == pt_index) continue;
        const entry = space.pt[i];
        if ((entry & runtime.page_addr_mask) != paddr) continue;
        if (entry == 0) continue;
        space.pt[i] = 0;
        const alias_va = (runtime.user_va & ~@as(u64, 0x1F_FFFF)) + (@as(u64, i) * 4096);
        runtime.flush_user_tlb_for_principal_va(principal, alias_va);
    }

    space.pt[pt_index] = paddr | runtime.page_present | runtime.page_user | (if (writable) runtime.page_rw else 0);
    runtime.flush_user_tlb_for_principal_va(principal, va);
    return true;
}

pub fn dropPresentForUserMappedPaddr(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    paddr: u64,
) bool {
    if (!runtime_ready) return false;
    const space = getUserSpace(principal) orelse return false;
    _ = state.getTableConst(principal).find(paddr) orelse return false;
    const user_pt_base_va = runtime.user_va & ~@as(u64, 0x1F_FFFF);

    var i: usize = 0;
    while (i < runtime.page_entries) : (i += 1) {
        const entry = space.pt[i];
        if ((entry & runtime.page_addr_mask) != paddr) continue;
        if ((entry & runtime.page_present) == 0) continue;
        space.pt[i] = entry & ~runtime.page_present;
        runtime.flush_user_tlb_for_principal_va(principal, user_pt_base_va + (@as(u64, i) * 4096));
        return true;
    }
    return false;
}

pub fn syncPageTableRightsForPaddr(state: *const kernel.KernelState, paddr: u64) void {
    if (!runtime_ready) return;
    const user_pt_base_va = runtime.user_va & ~@as(u64, 0x1F_FFFF);

    var process_index: usize = 0;
    while (process_index < runtime.user_spaces.len) : (process_index += 1) {
        const principal = principalFromProcessIndex(process_index) orelse continue;
        const space = &runtime.user_spaces[process_index];
        const cap = state.getTableConst(principal).find(paddr);
        var kept_one = false;

        var i: usize = 0;
        while (i < runtime.page_entries) : (i += 1) {
            const old_entry = space.pt[i];
            const mapped_paddr = old_entry & runtime.page_addr_mask;
            if (mapped_paddr != paddr) continue;

            var new_entry: u64 = 0;
            if (cap) |c| {
                if (c.rights.cpu_read and !kept_one) {
                    new_entry = paddr | runtime.page_user | runtime.page_present;
                    if (c.rights.cpu_write) new_entry |= runtime.page_rw;
                    kept_one = true;
                }
            }

            if (new_entry == old_entry) continue;
            space.pt[i] = new_entry;
            const va = user_pt_base_va + (@as(u64, i) * 4096);
            runtime.flush_user_tlb_for_principal_va(principal, va);
        }
    }
}

fn logWrite(text: []const u8) void {
    if (!runtime_ready) return;
    runtime.serial_write(text);
}

fn logHex(value: u64) void {
    if (!runtime_ready) return;
    runtime.print_hex(value);
}

pub fn dumpPrincipalCaps(state: *const kernel.KernelState, principal: kernel.PrincipalId, label: []const u8) void {
    logWrite(label);
    logWrite(" caps:\n");

    const table = state.getTableConst(principal);
    if (table.len == 0) {
        logWrite("  none\n");
        return;
    }

    var i: usize = 0;
    while (i < table.len) : (i += 1) {
        const cap = table.caps[i];
        logWrite("  ");
        logHex(cap.paddr);
        logWrite(" id=");
        logHex(cap.cap_id);
        logWrite(" root=");
        logHex(cap.root_cap_id);
        logWrite(" parent=");
        logHex(cap.parent_cap_id);
        if (!cap.rights.cpu_read and !cap.rights.cpu_write and cap.rights.dma) {
            logWrite(" (dma)");
        }
        logWrite("\n");
    }
}

pub fn dumpCapabilityView(state: *const kernel.KernelState) void {
    dumpPrincipalCaps(state, .Process0, "Process0");
    dumpPrincipalCaps(state, .Process1, "Process1");
    dumpPrincipalCaps(state, .Device0, "Device0");
}

pub fn dumpPrincipalEndpoints(state: *const kernel.KernelState, principal: kernel.PrincipalId, label: []const u8) void {
    logWrite(label);
    logWrite(" endpoints:\n");
    const table = state.getEndpointTableConst(principal);
    if (table.len == 0) {
        logWrite("  none\n");
        return;
    }

    var i: usize = 0;
    while (i < table.len) : (i += 1) {
        const ep = table.caps[i];
        logWrite("  ep=");
        logHex(ep.endpoint_id);
        logWrite(" -> ");
        logWrite(runtime.principal_label(ep.target));
        logWrite("\n");
    }
}
