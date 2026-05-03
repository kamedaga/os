const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const interrupts = @import("interrupts.zig");
const scheduler = @import("scheduler.zig");

const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;

pub const Hooks = struct {
    page_entries: usize,
    page_addr_mask: u64,
    page_present: u64,
    page_ps: u64,
    kernel_state_ready: *const bool,
    state: *kernel.KernelState,
    write: *const fn ([]const u8) void,
    write_hex_raw: *const fn (u64) void,
    write_bool01: *const fn (bool) void,
};

var hooks: ?Hooks = null;

pub fn init(new_hooks: Hooks) void {
    hooks = new_hooks;
}

fn getHooks() *const Hooks {
    return &(hooks orelse unreachable);
}

pub fn dumpPageWalkForVa(cr3: u64, va: u64) void {
    const h = getHooks();
    const pml4_index: usize = @intCast((va >> 39) & 0x1FF);
    const pdpt_index: usize = @intCast((va >> 30) & 0x1FF);
    const pd_index: usize = @intCast((va >> 21) & 0x1FF);
    const pt_index: usize = @intCast((va >> 12) & 0x1FF);

    const pml4_ptr: *const [512]u64 = @ptrFromInt(cr3 & h.page_addr_mask);
    const pml4e = pml4_ptr[pml4_index];
    h.write("  WALK.CR3=");
    h.write_hex_raw(cr3);
    h.write("\n");
    h.write("  WALK.PML4E=");
    h.write_hex_raw(pml4e);
    h.write("\n");
    if ((pml4e & h.page_present) == 0) return;

    const pdpt_ptr: *const [512]u64 = @ptrFromInt(pml4e & h.page_addr_mask);
    const pdpte = pdpt_ptr[pdpt_index];
    h.write("  WALK.PDPTE=");
    h.write_hex_raw(pdpte);
    h.write("\n");
    if ((pdpte & h.page_present) == 0) return;
    if ((pdpte & h.page_ps) != 0) return;

    const pd_ptr: *const [512]u64 = @ptrFromInt(pdpte & h.page_addr_mask);
    const pde = pd_ptr[pd_index];
    h.write("  WALK.PDE=");
    h.write_hex_raw(pde);
    h.write("\n");
    if ((pde & h.page_present) == 0) return;
    if ((pde & h.page_ps) != 0) return;

    const pt_ptr: *const [512]u64 = @ptrFromInt(pde & h.page_addr_mask);
    const pte = pt_ptr[pt_index];
    h.write("  WALK.PTE=");
    h.write_hex_raw(pte);
    h.write("\n");
}

pub fn logStep2(cr2: u64, frame: *const ExceptionTrapFrame) void {
    const h = getHooks();
    const ec_user = (frame.error_code & (1 << 2)) != 0;
    const va_user = capability.isUserCanonicalVa(cr2);

    h.write("  USER_MODE=");
    h.write_bool01(ec_user);
    h.write("\n");
    h.write("  USER_VA=");
    h.write_bool01(va_user);
    h.write("\n");

    const pf_cap = capability.issuePageFaultCapability(scheduler.currentUserPrincipal(), frame, cr2) orelse {
        h.write("  PF_CAP=none\n");
        h.write("  CAP_LOOKUP=skip\n");
        return;
    };
    h.write("  PF_CAP=issued\n");

    const candidate_paddr = pf_cap.candidate_paddr orelse {
        h.write("  CAND_PADDR=none\n");
        h.write("  CAP_LOOKUP=none\n");
        return;
    };
    h.write("  CAND_PADDR=");
    h.write_hex_raw(candidate_paddr);
    h.write("\n");

    if (!h.kernel_state_ready.*) {
        h.write("  CAP_LOOKUP=kernel_state_not_ready\n");
        return;
    }

    const has_cap = h.state.getTableConst(pf_cap.principal).find(candidate_paddr) != null;
    h.write("  CAP_LOOKUP=");
    h.write(if (has_cap) "found(current)\n" else "none(current)\n");
}
