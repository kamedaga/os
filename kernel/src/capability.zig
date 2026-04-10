const kernel = @import("kernel.zig");
const interrupts = @import("interrupts.zig");

pub const UserAddressSpace = struct {
    // Includes user VA mappings plus supervisor-only helper PTs for return stacks.
    pub const max_dynamic_pt_pages: usize = 320;
    pub const no_pd_index: u16 = 0xFFFF;

    pml4: [512]u64 align(4096) = [_]u64{0} ** 512,
    pdp: [512]u64 align(4096) = [_]u64{0} ** 512,
    pd: [512]u64 align(4096) = [_]u64{0} ** 512,
    pt_pages: [max_dynamic_pt_pages][512]u64 align(4096) = [_][512]u64{[_]u64{0} ** 512} ** max_dynamic_pt_pages,
    pt_page_pd_index: [max_dynamic_pt_pages]u16 = [_]u16{no_pd_index} ** max_dynamic_pt_pages,
    pt_page_used_len: u16 = 0,
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
var lookup_diag_count: u64 = 0;

fn shouldLogLookupDiag(principal: kernel.PrincipalId) bool {
    _ = principal;
    return false;
}

fn logLookupSpaceDiag(prefix: []const u8, principal: kernel.PrincipalId, space: *const UserAddressSpace) void {
    runtime.serial_write(prefix);
    runtime.serial_write(" proc=");
    runtime.serial_write(runtime.principal_label(principal));
    runtime.serial_write(" space=");
    runtime.print_hex(@intFromPtr(space));
    runtime.serial_write(" used=");
    runtime.print_hex(space.pt_page_used_len);
    runtime.serial_write(" cr3=");
    runtime.print_hex(space.cr3);
    runtime.serial_write(" pd256=");
    runtime.print_hex(space.pd[256]);
    runtime.serial_write(" pd257=");
    runtime.print_hex(space.pd[257]);
    runtime.serial_write(" pd479=");
    runtime.print_hex(space.pd[479]);
    runtime.serial_write(" pd480=");
    runtime.print_hex(space.pd[480]);
    runtime.serial_write(" meta0=");
    runtime.print_hex(space.pt_page_pd_index[0]);
    runtime.serial_write(" meta1=");
    runtime.print_hex(space.pt_page_pd_index[1]);
    runtime.serial_write(" meta2=");
    runtime.print_hex(space.pt_page_pd_index[2]);
    runtime.serial_write(" meta130=");
    runtime.print_hex(space.pt_page_pd_index[130]);
    runtime.serial_write("\n");
}

fn logByteWindow(prefix: []const u8, bytes: []const u8) void {
    runtime.serial_write(prefix);
    runtime.serial_write("=");
    for (bytes, 0..) |b, i| {
        if (i != 0) runtime.serial_write(" ");
        runtime.print_hex(b);
    }
    runtime.serial_write("\n");
}

pub fn init(config: RuntimeConfig) void {
    runtime = config;
    runtime_ready = true;
}

fn processIndex(principal: kernel.PrincipalId) ?usize {
    return kernel.processIndexFromPrincipal(principal);
}

fn principalFromProcessIndex(index: usize) ?kernel.PrincipalId {
    return kernel.processPrincipalFromIndex(index);
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
    if (shouldLogLookupDiag(principal)) {
        lookup_diag_count +%= 1;
        runtime.serial_write("lookup enter va=");
        runtime.print_hex(va);
        runtime.serial_write("\n");
        logLookupSpaceDiag("lookup pre", principal, space);
    }
    const pd_index = userPdIndexForVa(va) orelse return null;
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const slot = findPtSlotForPd(space, pd_index) orelse return null;
    const entry = space.pt_pages[slot][pt_index];
    const paddr = entry & runtime.page_addr_mask;
    if (shouldLogLookupDiag(principal)) {
        runtime.serial_write("lookup mid pd=");
        runtime.print_hex(@intCast(pd_index));
        runtime.serial_write(" slot=");
        runtime.print_hex(@intCast(slot));
        runtime.serial_write(" pt=");
        runtime.print_hex(@intCast(pt_index));
        runtime.serial_write(" entry=");
        runtime.print_hex(entry);
        runtime.serial_write(" paddr=");
        runtime.print_hex(paddr);
        runtime.serial_write("\n");
        logLookupSpaceDiag("lookup post", principal, space);
    }
    if (paddr == 0) return null;
    return paddr;
}

pub fn parseRights(bits: u64) kernel.Rights {
    return .{
        .cpu_read = (bits & 0x1) != 0,
        .cpu_write = (bits & 0x2) != 0,
        .dma = (bits & 0x4) != 0,
        .grant = (bits & 0x8) != 0,
    };
}

fn pageAlignDown(addr: u64) u64 {
    return addr & ~@as(u64, 4095);
}

fn userPdIndexForVa(va: u64) ?usize {
    const pml4_index: usize = @intCast((va >> 39) & 0x1FF);
    const pdp_index: usize = @intCast((va >> 30) & 0x1FF);
    const pd_index: usize = @intCast((va >> 21) & 0x1FF);
    const user_pdp_index: usize = @intCast((runtime.user_va >> 30) & 0x1FF);

    if (pml4_index != 0 or pdp_index != user_pdp_index) return null;
    return pd_index;
}

fn aliasVaForPdPt(pd_index: usize, pt_index: usize) u64 {
    const user_pdp_index: u64 = (runtime.user_va >> 30) & 0x1FF;
    return (user_pdp_index << 30) |
        (@as(u64, @intCast(pd_index)) << 21) |
        (@as(u64, @intCast(pt_index)) << 12);
}

fn slotPtPa(space: *const UserAddressSpace, slot: usize) u64 {
    return @intFromPtr(&space.pt_pages[slot]);
}

fn slotHasAnyMappedPaddr(space: *const UserAddressSpace, slot: usize) bool {
    if (slot >= UserAddressSpace.max_dynamic_pt_pages) return true;
    var i: usize = 0;
    while (i < runtime.page_entries) : (i += 1) {
        if ((space.pt_pages[slot][i] & runtime.page_addr_mask) != 0) return true;
    }
    return false;
}

fn pdIndexForPtSlot(space: *const UserAddressSpace, slot: usize) ?usize {
    if (slot >= UserAddressSpace.max_dynamic_pt_pages) return null;
    const pt_pa = slotPtPa(space, slot);
    var pd_index: usize = 0;
    while (pd_index < 512) : (pd_index += 1) {
        const pde = space.pd[pd_index];
        if ((pde & runtime.page_present) == 0) continue;
        if ((pde & (@as(u64, 1) << 7)) != 0) continue; // huge page
        if ((pde & runtime.page_addr_mask) == pt_pa) return pd_index;
    }
    return null;
}

fn findPtSlotForPd(space: *UserAddressSpace, pd_index: usize) ?usize {
    if (pd_index >= 512) return null;

    const principal = blk: {
        var i: usize = 0;
        while (i < runtime.user_spaces.len) : (i += 1) {
            if (&runtime.user_spaces[i] == space) {
                break :blk principalFromProcessIndex(i);
            }
        }
        break :blk null;
    };
    if (principal) |proc| {
        if (shouldLogLookupDiag(proc)) {
            runtime.serial_write("findPtSlot enter pd=");
            runtime.print_hex(@intCast(pd_index));
            runtime.serial_write("\n");
            logLookupSpaceDiag("findPtSlot pre", proc, space);
        }
    }

    const pde = space.pd[pd_index];
    if ((pde & runtime.page_present) != 0 and (pde & (@as(u64, 1) << 7)) == 0) {
        const pt_pa = pde & runtime.page_addr_mask;
        var slot_by_pd: usize = 0;
        while (slot_by_pd < UserAddressSpace.max_dynamic_pt_pages) : (slot_by_pd += 1) {
            if (slotPtPa(space, slot_by_pd) == pt_pa) {
                if (principal) |proc| {
                    if (shouldLogLookupDiag(proc)) {
                        runtime.serial_write("findPtSlot pde_match slot=");
                        runtime.print_hex(@intCast(slot_by_pd));
                        runtime.serial_write(" pt_pa=");
                        runtime.print_hex(pt_pa);
                        runtime.serial_write("\n");
                        logLookupSpaceDiag("findPtSlot pde_post", proc, space);
                    }
                }
                return slot_by_pd;
            }
        }
    }

    // Fallback to metadata if pd entry was not initialized yet.
    var slot_meta: usize = 0;
    while (slot_meta < UserAddressSpace.max_dynamic_pt_pages) : (slot_meta += 1) {
        if (space.pt_page_pd_index[slot_meta] == UserAddressSpace.no_pd_index) continue;
        if (space.pt_page_pd_index[slot_meta] == pd_index) {
            // Self-heal stale/missing PD entry from metadata.
            const pt_pa = slotPtPa(space, slot_meta);
            space.pd[pd_index] = pt_pa | runtime.page_present | runtime.page_rw | runtime.page_user;
            const next_len = slot_meta + 1;
            if (next_len > space.pt_page_used_len) {
                space.pt_page_used_len = @intCast(next_len);
            }
            if (principal) |proc| {
                if (shouldLogLookupDiag(proc)) {
                    runtime.serial_write("findPtSlot meta_match slot=");
                    runtime.print_hex(@intCast(slot_meta));
                    runtime.serial_write(" pt_pa=");
                    runtime.print_hex(pt_pa);
                    runtime.serial_write("\n");
                    logLookupSpaceDiag("findPtSlot meta_post", proc, space);
                }
            }
            return slot_meta;
        }
    }
    return null;
}

fn ensurePtSlotForPd(space: *UserAddressSpace, pd_index: usize) ?usize {
    if (pd_index >= 512) return null;
    if (findPtSlotForPd(space, pd_index)) |existing| return existing;

    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        if (space.pt_page_pd_index[slot] != UserAddressSpace.no_pd_index) continue;
        if (pdIndexForPtSlot(space, slot) != null) continue;
        if (slotHasAnyMappedPaddr(space, slot)) continue;
        break;
    }
    if (slot >= UserAddressSpace.max_dynamic_pt_pages) return null;
    space.pt_page_pd_index[slot] = @intCast(pd_index);
    @memset(space.pt_pages[slot][0..], 0);
    const pt_pa = @intFromPtr(&space.pt_pages[slot]);
    space.pd[pd_index] = pt_pa | runtime.page_present | runtime.page_rw | runtime.page_user;
    const next_len = slot + 1;
    if (next_len > space.pt_page_used_len) {
        space.pt_page_used_len = @intCast(next_len);
    }
    return slot;
}

fn shouldLogMapMmioDiag(principal: kernel.PrincipalId, va: u64) bool {
    _ = principal;
    _ = va;
    return false;
}

noinline fn logMapMmioDiagEnter(va: u64, paddr: u64) void {
    runtime.serial_write("map_mmio_diag enter va=");
    runtime.print_hex(va);
    runtime.serial_write(" paddr=");
    runtime.print_hex(paddr);
    runtime.serial_write("\n");
}

noinline fn logMapMmioDiagPostEnsure(pd_index: usize, pt_index: usize, map_slot: usize) void {
    runtime.serial_write("map_mmio_diag slot=");
    runtime.print_hex(@intCast(map_slot));
    runtime.serial_write(" pd=");
    runtime.print_hex(@intCast(pd_index));
    runtime.serial_write(" pt=");
    runtime.print_hex(@intCast(pt_index));
    runtime.serial_write("\n");
}

noinline fn logMapMmioDiagPostAliasLoop(space: *UserAddressSpace, map_slot: usize, pt_index: usize) void {
    runtime.serial_write("map_mmio_diag dst_pte=");
    runtime.print_hex(@intFromPtr(&space.pt_pages[map_slot][pt_index]));
    runtime.serial_write("\n");
}

noinline fn logMapMmioDiagPostPteWrite() void {
    runtime.serial_write("map_mmio_diag pte write done\n");
}

noinline fn logMapMmioDiagPostMap() void {
    runtime.serial_write("map_mmio_diag map done\n");
}

noinline fn clearAliasMappings(
    space: *UserAddressSpace,
    principal: kernel.PrincipalId,
    paddr: u64,
    map_slot: usize,
    pt_index: usize,
) void {
    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        const slot_pd_index = pdIndexForPtSlot(space, slot) orelse continue;
        const pt_page = &space.pt_pages[slot];
        var i: usize = 0;
        while (i < runtime.page_entries) : (i += 1) {
            if (slot == map_slot and i == pt_index) continue;
            const entry = pt_page[i];
            if ((entry & runtime.page_addr_mask) != paddr) continue;
            if (entry == 0) continue;
            pt_page[i] = 0;
            const alias_va = aliasVaForPdPt(slot_pd_index, i);
            runtime.flush_user_tlb_for_principal_va(principal, alias_va);
        }
    }
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
    const diag = shouldLogMapMmioDiag(principal, va);
    if (diag) logMapMmioDiagEnter(va, paddr);
    if ((va & 0xFFF) != 0) return false;
    if ((paddr & 0xFFF) != 0) return false;
    if (paddr >= runtime.physical_map_limit) return false;

    const pd_index = userPdIndexForVa(va) orelse return false;
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const map_slot = ensurePtSlotForPd(space, pd_index) orelse return false;
    if (diag) logMapMmioDiagPostEnsure(pd_index, pt_index, map_slot);

    const cap = state.getTableConst(principal).find(paddr) orelse return false;
    if (!cap.rights.cpu_read) return false;
    if (writable and !cap.rights.cpu_write) return false;

    clearAliasMappings(space, principal, paddr, map_slot, pt_index);
    if (diag) logMapMmioDiagPostAliasLoop(space, map_slot, pt_index);

    space.pt_pages[map_slot][pt_index] = paddr | runtime.page_present | runtime.page_user | (if (writable) runtime.page_rw else 0);
    if (diag) logMapMmioDiagPostPteWrite();
    runtime.flush_user_tlb_for_principal_va(principal, va);
    if (diag) logMapMmioDiagPostMap();
    return true;
}

fn mapUserPageFromCapabilityNoAlias(
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

    const pd_index = userPdIndexForVa(va) orelse return false;
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const map_slot = ensurePtSlotForPd(space, pd_index) orelse return false;

    const cap = state.getTableConst(principal).find(paddr) orelse return false;
    if (!cap.rights.cpu_read) return false;
    if (writable and !cap.rights.cpu_write) return false;

    space.pt_pages[map_slot][pt_index] = paddr | runtime.page_present | runtime.page_user | (if (writable) runtime.page_rw else 0);
    runtime.flush_user_tlb_for_principal_va(principal, va);
    return true;
}

pub fn mapUserPagesFromCapabilityBatch(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    base_va: u64,
    paddrs: []const u64,
    writable: bool,
) bool {
    if (!runtime_ready) return false;
    if ((base_va & 0xFFF) != 0) return false;

    var i: usize = 0;
    while (i < paddrs.len) : (i += 1) {
        const paddr = paddrs[i];
        if ((paddr & 0xFFF) != 0) return false;
        if (paddr >= runtime.physical_map_limit) return false;

        var j: usize = 0;
        while (j < i) : (j += 1) {
            if (paddrs[j] == paddr) return false;
        }

        const cap = state.getTableConst(principal).find(paddr) orelse return false;
        if (!cap.rights.cpu_read) return false;
        if (writable and !cap.rights.cpu_write) return false;
    }

    i = 0;
    while (i < paddrs.len) : (i += 1) {
        const offset: u64 = @intCast(i * 4096);
        const va, const va_overflow = @addWithOverflow(base_va, offset);
        if (va_overflow != 0) return false;
        if (!mapUserPageFromCapabilityNoAlias(state, principal, va, paddrs[i], writable)) return false;
    }
    return true;
}

pub fn mapFreshUserPage(
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

    const pd_index = userPdIndexForVa(va) orelse {
        runtime.serial_write("mapFreshUserPage fail: bad user VA proc=");
        runtime.serial_write(runtime.principal_label(principal));
        runtime.serial_write(" va=");
        runtime.print_hex(va);
        runtime.serial_write("\n");
        return false;
    };
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);
    const map_slot = ensurePtSlotForPd(space, pd_index) orelse {
        runtime.serial_write("mapFreshUserPage fail: ensurePtSlot proc=");
        runtime.serial_write(runtime.principal_label(principal));
        runtime.serial_write(" va=");
        runtime.print_hex(va);
        runtime.serial_write(" pd=");
        runtime.print_hex(@intCast(pd_index));
        runtime.serial_write(" used=");
        runtime.print_hex(space.pt_page_used_len);
        runtime.serial_write("\n");
        return false;
    };
    const old_entry = space.pt_pages[map_slot][pt_index];
    if ((old_entry & runtime.page_present) != 0) {
        runtime.serial_write("mapFreshUserPage fail: already_present proc=");
        runtime.serial_write(runtime.principal_label(principal));
        runtime.serial_write(" va=");
        runtime.print_hex(va);
        runtime.serial_write(" paddr=");
        runtime.print_hex(paddr);
        runtime.serial_write(" slot=");
        runtime.print_hex(@intCast(map_slot));
        runtime.serial_write(" old=");
        runtime.print_hex(old_entry);
        runtime.serial_write("\n");
        return false;
    }

    space.pt_pages[map_slot][pt_index] = paddr | runtime.page_present | runtime.page_user | (if (writable) runtime.page_rw else 0);
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
    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        const slot_pd_index = pdIndexForPtSlot(space, slot) orelse continue;
        var i: usize = 0;
        while (i < runtime.page_entries) : (i += 1) {
            const entry = space.pt_pages[slot][i];
            if ((entry & runtime.page_addr_mask) != paddr) continue;
            if ((entry & runtime.page_present) == 0) continue;
            space.pt_pages[slot][i] = entry & ~runtime.page_present;
            runtime.flush_user_tlb_for_principal_va(principal, aliasVaForPdPt(slot_pd_index, i));
            return true;
        }
    }
    return false;
}

noinline fn syncPageTableRightsScan(
    space: *UserAddressSpace,
    principal: kernel.PrincipalId,
    paddr: u64,
    cap: ?*const kernel.Capability,
) void {
    var kept_one = false;
    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        const slot_pd_index = pdIndexForPtSlot(space, slot) orelse continue;
        var i: usize = 0;
        while (i < runtime.page_entries) : (i += 1) {
            const old_entry = space.pt_pages[slot][i];
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
            space.pt_pages[slot][i] = new_entry;
            const va = aliasVaForPdPt(slot_pd_index, i);
            runtime.flush_user_tlb_for_principal_va(principal, va);
        }
    }
}

pub fn principalHasMappedPaddr(principal: kernel.PrincipalId, paddr: u64) bool {
    if (!runtime_ready) return false;
    const space = getUserSpace(principal) orelse return false;
    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        const slot_pd_index = pdIndexForPtSlot(space, slot) orelse {
            continue;
        };
        _ = slot_pd_index;
        var i: usize = 0;
        while (i < runtime.page_entries) : (i += 1) {
            const entry = space.pt_pages[slot][i];
            if ((entry & runtime.page_present) == 0) continue;
            if ((entry & runtime.page_addr_mask) == paddr) return true;
        }
    }
    return false;
}

pub fn syncPageTableRightsForPrincipalPaddr(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    paddr: u64,
) void {
    if (!runtime_ready) return;
    const space = getUserSpace(principal) orelse return;
    const cap = state.getTableConst(principal).find(paddr);
    syncPageTableRightsScan(space, principal, paddr, cap);
}

pub fn syncPageTableRightsForPaddr(state: *const kernel.KernelState, paddr: u64) void {
    if (!runtime_ready) return;

    var process_index: usize = 0;
    while (process_index < runtime.user_spaces.len) : (process_index += 1) {
        const principal = principalFromProcessIndex(process_index) orelse continue;
        syncPageTableRightsForPrincipalPaddr(state, principal, paddr);
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
        logWrite(" rights=");
        if (cap.rights.cpu_read) logWrite("r");
        if (cap.rights.cpu_write) logWrite("w");
        if (cap.rights.dma) logWrite("d");
        if (cap.rights.grant) logWrite("g");
        logWrite("\n");
    }
}

pub fn dumpCapabilityView(state: *const kernel.KernelState) void {
    var i: usize = 0;
    while (i < kernel.process_count) : (i += 1) {
        const principal = kernel.processPrincipalFromIndex(i) orelse unreachable;
        dumpPrincipalCaps(state, principal, kernel.principalLabel(principal));
    }
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
