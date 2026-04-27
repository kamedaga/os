const capability = @import("../capability.zig");
const kernel = @import("../kernel.zig");

const UserAddressSpace = capability.UserAddressSpace;

pub const Hooks = struct {
    user_spaces: []UserAddressSpace,
    current_user_principal: *kernel.PrincipalId,
    four_gib: u64,
    physical_map_limit: u64,
    user_va: u64,
    user_stack_page_va: u64,
    page_entries: usize,
    page_present: u64,
    page_rw: u64,
    page_user: u64,
    seed_user_pd_with_kernel_identity: *const fn ([]u64) void,
};

var hooks: ?Hooks = null;

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
    const h = hooks.?;
    return getUserSpace(h.current_user_principal.*).?;
}

fn findUserPtSlotForPd(space: *const UserAddressSpace, pd_index: usize) ?usize {
    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages) : (slot += 1) {
        if (space.pt_page_pd_index[slot] == UserAddressSpace.no_pd_index) continue;
        if (space.pt_page_pd_index[slot] == pd_index) return slot;
    }
    return null;
}

pub fn ensureUserPtSlotForPd(space: *UserAddressSpace, pd_index: usize) ?usize {
    const h = hooks orelse return null;
    if (pd_index >= h.page_entries) return null;
    if (findUserPtSlotForPd(space, pd_index)) |slot| return slot;

    var slot: usize = 0;
    while (slot < UserAddressSpace.max_dynamic_pt_pages and space.pt_page_pd_index[slot] != UserAddressSpace.no_pd_index) : (slot += 1) {}
    if (slot >= UserAddressSpace.max_dynamic_pt_pages) return null;
    space.pt_page_pd_index[slot] = @intCast(pd_index);
    @memset(space.pt_pages[slot][0..], 0);
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
        if ((space.pt_pages[pt_slot][pt_index] & h.page_present) != 0) return false;
        space.pt_pages[pt_slot][pt_index] = paddr | pte_flags;
    }

    return true;
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
        @memset(space.pt_pages[pt_slot_init][0..], 0);
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
    space.pt_pages[user_slot][user_pt_index] = user_page_paddr | h.page_present | h.page_rw | h.page_user;
    space.pt_pages[stack_slot][stack_pt_index] = user_stack_paddr | h.page_present | h.page_rw | h.page_user;
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
        @memset(space.pt_pages[pt_slot_init][0..], 0);
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
