const capability = @import("../capability.zig");
const kernel = @import("../kernel.zig");
const scheduler = @import("../scheduler.zig");

const UserAddressSpace = capability.UserAddressSpace;

pub const Hooks = struct {
    user_spaces: []UserAddressSpace,
    four_gib: u64,
    physical_map_limit: u64,
    user_va: u64,
    user_stack_page_va: u64,
    page_entries: usize,
    page_addr_mask: u64,
    page_present: u64,
    page_rw: u64,
    page_user: u64,
    page_ps: u64,
    flush_user_tlb_for_principal_va: *const fn (principal: kernel.PrincipalId, va: u64) void,
    seed_user_pd_with_kernel_identity: *const fn ([]u64) void,
};

var hooks: ?Hooks = null;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

pub fn kernelStaticStorageEndAddr() usize {
    return staticStorageEnd(@TypeOf(hooks), &hooks);
}

pub fn init(new_hooks: Hooks) void {
    hooks = new_hooks;
}

pub fn getUserSpace(principal: kernel.PrincipalId) ?*UserAddressSpace {
    const h = hooks orelse return null;
    const idx = processIndex(principal) orelse return null;
    if (idx >= h.user_spaces.len) return null;
    return &h.user_spaces[idx];
}

pub fn currentUserSpace() *UserAddressSpace {
    return getUserSpace(scheduler.currentUserPrincipal()).?;
}

fn findUserPtSlotForPd(space: *const UserAddressSpace, pd_index: usize) ?usize {
    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        if (space.pt_page_pd_index[slot] == UserAddressSpace.no_pd_index) continue;
        if (space.pt_page_pd_index[slot] == pd_index) return slot;
    }
    return null;
}

fn seedPtSlotFromExistingPd(space: *UserAddressSpace, slot: usize, existing_pde: u64) void {
    const h = hooks orelse return;
    const pt_page: *[512]u64 = &space.pt_pages[slot];
    @memset(pt_page[0..], 0);
    if ((existing_pde & h.page_present) == 0) return;
    if ((existing_pde & h.page_ps) == 0) {
        if ((existing_pde & h.page_user) != 0) return;
        const src_pt_addr = existing_pde & h.page_addr_mask;
        if (src_pt_addr == 0 or src_pt_addr >= h.physical_map_limit) return;
        const src_pt: *const [512]u64 = @ptrFromInt(src_pt_addr);
        var copy_index: usize = 0;
        while (copy_index < h.page_entries) : (copy_index += 1) {
            pt_page[copy_index] = src_pt[copy_index] & ~h.page_user;
        }
        return;
    }

    const two_mib_mask = ~@as(u64, 0x1F_FFFF);
    const page_base = existing_pde & two_mib_mask;
    const pte_flags = (existing_pde & 0xFFF) & ~h.page_ps & ~h.page_user;
    var pt_index: usize = 0;
    while (pt_index < h.page_entries) : (pt_index += 1) {
        const paddr = page_base + @as(u64, @intCast(pt_index)) * 4096;
        pt_page[pt_index] = paddr | pte_flags;
    }
}

pub fn ensureUserPtSlotForPd(space: *UserAddressSpace, pd_index: usize) ?usize {
    const h = hooks orelse return null;
    if (pd_index >= h.page_entries) return null;
    if (findUserPtSlotForPd(space, pd_index)) |slot| return slot;

    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages and space.pt_page_pd_index[slot] != UserAddressSpace.no_pd_index) : (slot += 1) {}
    if (slot >= UserAddressSpace.max_dynamic_pt_pages) return null;
    space.pt_page_pd_index[slot] = @intCast(pd_index);
    const existing_pde = space.pd[pd_index];
    seedPtSlotFromExistingPd(space, slot, existing_pde);
    const pt_pa: u64 = @intFromPtr(&space.pt_pages[slot]);
    if (pt_pa >= h.four_gib) return null;
    space.pd[pd_index] = pt_pa | h.page_present | h.page_rw | h.page_user;
    const next_len = slot + 1;
    if (next_len > space.pt_page_used_len) {
        space.pt_page_used_len = @intCast(next_len);
    }
    return slot;
}

fn pteFlagsForProt(h: Hooks, prot: kernel.MapProt) ?u64 {
    if (!prot.read) return null;
    // NX is not enabled yet, so exec is tracked by ABI but not enforced in PTEs.
    _ = prot.exec;
    return h.page_present | h.page_user | (if (prot.write) h.page_rw else 0);
}

pub fn mapUserLinearRegion(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddr_start: u64,
    size_bytes: usize,
    writable: bool,
) bool {
    return mapUserLinearRegionWithProt(principal, va_start, paddr_start, size_bytes, .{
        .read = true,
        .write = writable,
        .exec = true,
    });
}

pub fn mapUserLinearRegionWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddr_start: u64,
    size_bytes: usize,
    prot: kernel.MapProt,
) bool {
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    const pte_flags = pteFlagsForProt(h, prot) orelse return false;
    if (size_bytes == 0) return false;
    if ((va_start & 0xFFF) != 0 or (paddr_start & 0xFFF) != 0) return false;

    const map_end_va = va_start + size_bytes - 1;
    const map_end_pa = paddr_start + size_bytes - 1;
    if (map_end_pa >= h.physical_map_limit) return false;

    const user_pdp_index: usize = @intCast((h.user_va >> 30) & 0x1FF);
    const start_pml4: usize = @intCast((va_start >> 39) & 0x1FF);
    const start_pdp: usize = @intCast((va_start >> 30) & 0x1FF);
    const end_pml4: usize = @intCast((map_end_va >> 39) & 0x1FF);
    const end_pdp: usize = @intCast((map_end_va >> 30) & 0x1FF);
    if (start_pml4 != 0 or end_pml4 != 0) return false;
    if (start_pdp != user_pdp_index or end_pdp != user_pdp_index) return false;

    var offset: u64 = 0;
    while (offset < size_bytes) : (offset += 4096) {
        const va = va_start + offset;
        const paddr = paddr_start + offset;
        const pd_index: usize = @intCast((va >> 21) & 0x1FF);
        const pt_slot = ensureUserPtSlotForPd(space, pd_index) orelse return false;
        const pt_index: usize = @intCast((va >> 12) & 0x1FF);
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        const old_entry = pt_page[pt_index];
        if ((old_entry & h.page_present) != 0 and (old_entry & h.page_user) != 0) return false;
        pt_page[pt_index] = paddr | pte_flags;
    }

    return true;
}

pub fn protectUserLinearRegionWithProt(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
    prot: kernel.MapProt,
) bool {
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    const pte_flags = pteFlagsForProt(h, prot) orelse return false;
    if (size_bytes == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const size_u64: u64 = @intCast(size_bytes);
    const map_end_va, const overflow = @addWithOverflow(va_start, size_u64 - 1);
    if (overflow != 0) return false;

    const user_pdp_index: usize = @intCast((h.user_va >> 30) & 0x1FF);
    const start_pml4: usize = @intCast((va_start >> 39) & 0x1FF);
    const start_pdp: usize = @intCast((va_start >> 30) & 0x1FF);
    const end_pml4: usize = @intCast((map_end_va >> 39) & 0x1FF);
    const end_pdp: usize = @intCast((map_end_va >> 30) & 0x1FF);
    if (start_pml4 != 0 or end_pml4 != 0) return false;
    if (start_pdp != user_pdp_index or end_pdp != user_pdp_index) return false;

    var offset: u64 = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const pd_index: usize = @intCast((va >> 21) & 0x1FF);
        const pt_slot = findUserPtSlotForPd(space, pd_index) orelse return false;
        const pt_index: usize = @intCast((va >> 12) & 0x1FF);
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        const old_entry = pt_page[pt_index];
        if ((old_entry & h.page_present) == 0) return false;
        if ((old_entry & h.page_user) == 0) return false;
        const paddr = old_entry & ~@as(u64, 0xFFF);
        pt_page[pt_index] = paddr | pte_flags;
        h.flush_user_tlb_for_principal_va(principal, va);
    }

    return true;
}

pub fn unmapUserLinearRegion(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
) bool {
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    if (size_bytes == 0) return false;
    if ((va_start & 0xFFF) != 0) return false;

    const size_u64: u64 = @intCast(size_bytes);
    const map_end_va, const overflow = @addWithOverflow(va_start, size_u64 - 1);
    if (overflow != 0) return false;

    const user_pdp_index: usize = @intCast((h.user_va >> 30) & 0x1FF);
    const start_pml4: usize = @intCast((va_start >> 39) & 0x1FF);
    const start_pdp: usize = @intCast((va_start >> 30) & 0x1FF);
    const end_pml4: usize = @intCast((map_end_va >> 39) & 0x1FF);
    const end_pdp: usize = @intCast((map_end_va >> 30) & 0x1FF);
    if (start_pml4 != 0 or end_pml4 != 0) return false;
    if (start_pdp != user_pdp_index or end_pdp != user_pdp_index) return false;

    var offset: u64 = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const pd_index: usize = @intCast((va >> 21) & 0x1FF);
        const pt_slot = findUserPtSlotForPd(space, pd_index) orelse return false;
        const pt_index: usize = @intCast((va >> 12) & 0x1FF);
        const pt_page: *const [512]u64 = &space.pt_pages[pt_slot];
        const old_entry = pt_page[pt_index];
        if ((old_entry & h.page_present) == 0) return false;
        if ((old_entry & h.page_user) == 0) return false;
    }

    offset = 0;
    while (offset < size_u64) : (offset += 4096) {
        const va = va_start + offset;
        const pd_index: usize = @intCast((va >> 21) & 0x1FF);
        const pt_slot = findUserPtSlotForPd(space, pd_index) orelse return false;
        const pt_index: usize = @intCast((va >> 12) & 0x1FF);
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot];
        pt_page[pt_index] = 0;
        h.flush_user_tlb_for_principal_va(principal, va);
    }

    return true;
}

pub fn collectUserLinearRegionPaddrs(
    principal: kernel.PrincipalId,
    va_start: u64,
    size_bytes: usize,
    out_paddrs: []u64,
) ?usize {
    const h = hooks orelse return null;
    const space = getUserSpace(principal) orelse return null;
    if (size_bytes == 0) return null;
    if ((va_start & 0xFFF) != 0) return null;

    const size_u64: u64 = @intCast(size_bytes);
    if ((size_u64 & 0xFFF) != 0) return null;
    const page_count: usize = @intCast(size_u64 / 4096);
    if (page_count == 0 or page_count > out_paddrs.len) return null;
    const map_end_va, const overflow = @addWithOverflow(va_start, size_u64 - 1);
    if (overflow != 0) return null;

    const user_pdp_index: usize = @intCast((h.user_va >> 30) & 0x1FF);
    const start_pml4: usize = @intCast((va_start >> 39) & 0x1FF);
    const start_pdp: usize = @intCast((va_start >> 30) & 0x1FF);
    const end_pml4: usize = @intCast((map_end_va >> 39) & 0x1FF);
    const end_pdp: usize = @intCast((map_end_va >> 30) & 0x1FF);
    if (start_pml4 != 0 or end_pml4 != 0) return null;
    if (start_pdp != user_pdp_index or end_pdp != user_pdp_index) return null;

    var page_index: usize = 0;
    while (page_index < page_count) : (page_index += 1) {
        const va = va_start + @as(u64, @intCast(page_index * 4096));
        const pd_index: usize = @intCast((va >> 21) & 0x1FF);
        const pt_slot = findUserPtSlotForPd(space, pd_index) orelse return null;
        const pt_index: usize = @intCast((va >> 12) & 0x1FF);
        const pt_page: *const [512]u64 = &space.pt_pages[pt_slot];
        const old_entry = pt_page[pt_index];
        if ((old_entry & h.page_present) == 0) return null;
        if ((old_entry & h.page_user) == 0) return null;
        out_paddrs[page_index] = old_entry & ~@as(u64, 0xFFF);
    }

    return page_count;
}

pub fn unmapUserMappedPaddr(principal: kernel.PrincipalId, paddr: u64) usize {
    const h = hooks orelse return 0;
    const space = getUserSpace(principal) orelse return 0;
    if ((paddr & 0xFFF) != 0) return 0;

    const user_pdp_index: u64 = (h.user_va >> 30) & 0x1FF;
    var removed: usize = 0;
    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        const pd_index_meta = space.pt_page_pd_index[slot];
        if (pd_index_meta == UserAddressSpace.no_pd_index) continue;
        const pd_index: usize = @intCast(pd_index_meta);
        var pt_index: usize = 0;
        while (pt_index < h.page_entries) : (pt_index += 1) {
            const pt_page: *[512]u64 = &space.pt_pages[slot];
            const old_entry = pt_page[pt_index];
            if ((old_entry & h.page_present) == 0) continue;
            if ((old_entry & h.page_user) == 0) continue;
            if ((old_entry & ~@as(u64, 0xFFF)) != paddr) continue;
            pt_page[pt_index] = 0;
            const va = (user_pdp_index << 30) |
                (@as(u64, @intCast(pd_index)) << 21) |
                (@as(u64, @intCast(pt_index)) << 12);
            h.flush_user_tlb_for_principal_va(principal, va);
            removed += 1;
        }
    }
    return removed;
}

pub fn collectUserMappedPaddrs(principal: kernel.PrincipalId, out_paddrs: []u64) usize {
    const h = hooks orelse return 0;
    const space = getUserSpace(principal) orelse return 0;
    var count: usize = 0;
    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        if (space.pt_page_pd_index[slot] == UserAddressSpace.no_pd_index) continue;
        const pt_page: *const [512]u64 = &space.pt_pages[slot];
        var pt_index: usize = 0;
        while (pt_index < h.page_entries) : (pt_index += 1) {
            const entry = pt_page[pt_index];
            if ((entry & h.page_present) == 0) continue;
            if ((entry & h.page_user) == 0) continue;
            const paddr = entry & ~@as(u64, 0xFFF);
            if (paddr == 0) continue;
            var duplicate = false;
            for (out_paddrs[0..count]) |seen| {
                if (seen == paddr) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            if (count >= out_paddrs.len) return count;
            out_paddrs[count] = paddr;
            count += 1;
        }
    }
    return count;
}

pub const MappedUserPage = struct {
    va: u64,
    paddr: u64,
    writable: bool,
};

pub const MappedUserPageVisitor = *const fn (context: *anyopaque, page: MappedUserPage) bool;

pub fn forEachUserMappedPage(principal: kernel.PrincipalId, context: *anyopaque, visitor: MappedUserPageVisitor) bool {
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    const user_pdp_index: u64 = (h.user_va >> 30) & 0x1FF;
    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        const pd_index_meta = space.pt_page_pd_index[slot];
        if (pd_index_meta == UserAddressSpace.no_pd_index) continue;
        const pd_index: usize = @intCast(pd_index_meta);
        const pt_page: *const [512]u64 = &space.pt_pages[slot];
        var pt_index: usize = 0;
        while (pt_index < h.page_entries) : (pt_index += 1) {
            const entry = pt_page[pt_index];
            if ((entry & h.page_present) == 0) continue;
            if ((entry & h.page_user) == 0) continue;
            const paddr = entry & ~@as(u64, 0xFFF);
            if (paddr == 0) continue;
            const va = (user_pdp_index << 30) |
                (@as(u64, @intCast(pd_index)) << 21) |
                (@as(u64, @intCast(pt_index)) << 12);
            if (!visitor(context, .{
                .va = va,
                .paddr = paddr,
                .writable = (entry & h.page_rw) != 0,
            })) return false;
        }
    }
    return true;
}

pub fn collectUserMappedPages(principal: kernel.PrincipalId, out_pages: []MappedUserPage) usize {
    const h = hooks orelse return 0;
    const space = getUserSpace(principal) orelse return 0;
    const user_pdp_index: u64 = (h.user_va >> 30) & 0x1FF;
    var count: usize = 0;
    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        const pd_index_meta = space.pt_page_pd_index[slot];
        if (pd_index_meta == UserAddressSpace.no_pd_index) continue;
        const pd_index: usize = @intCast(pd_index_meta);
        const pt_page: *const [512]u64 = &space.pt_pages[slot];
        var pt_index: usize = 0;
        while (pt_index < h.page_entries) : (pt_index += 1) {
            const entry = pt_page[pt_index];
            if ((entry & h.page_present) == 0) continue;
            if ((entry & h.page_user) == 0) continue;
            const paddr = entry & ~@as(u64, 0xFFF);
            if (paddr == 0) continue;
            var duplicate = false;
            for (out_pages[0..count]) |seen| {
                if (seen.paddr == paddr) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            if (count >= out_pages.len) return count;
            const va = (user_pdp_index << 30) |
                (@as(u64, @intCast(pd_index)) << 21) |
                (@as(u64, @intCast(pt_index)) << 12);
            out_pages[count] = .{
                .va = va,
                .paddr = paddr,
                .writable = (entry & h.page_rw) != 0,
            };
            count += 1;
        }
    }
    return count;
}

pub fn buildUserAddressSpace(principal: kernel.PrincipalId, user_page_paddr: u64, user_stack_paddr: u64) bool {
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    @memset(space.pml4[0..], 0);
    @memset(space.pdp[0..], 0);
    @memset(space.pd[0..], 0);
    var pt_slot_init: usize = 0;
    while (pt_slot_init < UserAddressSpace.max_dynamic_pt_pages) : (pt_slot_init += 1) {
        space.pt_page_pd_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot_init];
        @memset(pt_page[0..], 0);
    }
    space.pt_page_used_len = 0;

    const user_pml4_pa: u64 = @intFromPtr(&space.pml4);
    const user_pdp_pa: u64 = @intFromPtr(&space.pdp);
    const user_pd_pa: u64 = @intFromPtr(&space.pd);
    if (user_pml4_pa >= h.four_gib or user_pdp_pa >= h.four_gib or user_pd_pa >= h.four_gib) return false;
    if (user_page_paddr >= h.four_gib or user_stack_paddr >= h.four_gib) return false;

    const pdp_index: usize = @intCast((h.user_va >> 30) & 0x1FF);
    const pd_index_base: usize = @intCast((h.user_va >> 21) & 0x1FF);
    const user_pt_index: usize = @intCast((h.user_va >> 12) & 0x1FF);
    const stack_pt_index: usize = @intCast((h.user_stack_page_va >> 12) & 0x1FF);
    const stack_pd_index: usize = @intCast((h.user_stack_page_va >> 21) & 0x1FF);

    space.pml4[0] = user_pdp_pa | h.page_present | h.page_rw | h.page_user;
    space.pdp[pdp_index] = user_pd_pa | h.page_present | h.page_rw | h.page_user;
    h.seed_user_pd_with_kernel_identity(space.pd[0..]);
    const user_slot = ensureUserPtSlotForPd(space, pd_index_base) orelse return false;
    const stack_slot = ensureUserPtSlotForPd(space, stack_pd_index) orelse return false;
    const user_pt_page: *[512]u64 = &space.pt_pages[user_slot];
    const stack_pt_page: *[512]u64 = &space.pt_pages[stack_slot];
    user_pt_page[user_pt_index] = user_page_paddr | h.page_present | h.page_rw | h.page_user;
    stack_pt_page[stack_pt_index] = user_stack_paddr | h.page_present | h.page_rw | h.page_user;
    space.cr3 = user_pml4_pa;
    return true;
}

pub fn buildEmptyUserAddressSpace(principal: kernel.PrincipalId) bool {
    const h = hooks orelse return false;
    const space = getUserSpace(principal) orelse return false;
    @memset(space.pml4[0..], 0);
    @memset(space.pdp[0..], 0);
    @memset(space.pd[0..], 0);
    var pt_slot_init: usize = 0;
    while (pt_slot_init < UserAddressSpace.max_dynamic_pt_pages) : (pt_slot_init += 1) {
        space.pt_page_pd_index[pt_slot_init] = UserAddressSpace.no_pd_index;
        const pt_page: *[512]u64 = &space.pt_pages[pt_slot_init];
        @memset(pt_page[0..], 0);
    }
    space.pt_page_used_len = 0;

    const user_pml4_pa: u64 = @intFromPtr(&space.pml4);
    const user_pdp_pa: u64 = @intFromPtr(&space.pdp);
    const user_pd_pa: u64 = @intFromPtr(&space.pd);
    if (user_pml4_pa >= h.four_gib or user_pdp_pa >= h.four_gib or user_pd_pa >= h.four_gib) return false;

    const pdp_index: usize = @intCast((h.user_va >> 30) & 0x1FF);
    space.pml4[0] = user_pdp_pa | h.page_present | h.page_rw | h.page_user;
    space.pdp[pdp_index] = user_pd_pa | h.page_present | h.page_rw | h.page_user;
    h.seed_user_pd_with_kernel_identity(space.pd[0..]);
    space.cr3 = user_pml4_pa;
    return true;
}

fn processIndex(principal: kernel.PrincipalId) ?usize {
    return kernel.processIndexFromPrincipal(principal);
}
