const kernel = @import("kernel.zig");
const interrupts = @import("interrupts.zig");
const scheduler = @import("scheduler.zig").connection;
const user_vm = @import("memory/user_vm.zig");

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

var page_fault_log_hooks_storage: Hooks = undefined;
var page_fault_log_hooks_ready = false;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

pub fn kernelStaticStorageEndAddr() usize {
    return @max(
        staticStorageEnd(@TypeOf(page_fault_log_hooks_storage), &page_fault_log_hooks_storage),
        staticStorageEnd(@TypeOf(page_fault_log_hooks_ready), &page_fault_log_hooks_ready),
    );
}

pub fn init(new_hooks: Hooks) void {
    page_fault_log_hooks_storage = new_hooks;
    page_fault_log_hooks_ready = true;
}

fn getHooks() *const Hooks {
    if (!page_fault_log_hooks_ready) unreachable;
    return &page_fault_log_hooks_storage;
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
    const va_user = user_vm.isUserCanonicalVa(cr2);

    h.write("  USER_MODE=");
    h.write_bool01(ec_user);
    h.write("\n");
    h.write("  USER_VA=");
    h.write_bool01(va_user);
    h.write("\n");

    if (!ec_user or !va_user) {
        h.write("  USER_FAULT=none\n");
        return;
    }
    h.write("  USER_FAULT=observed\n");

    const principal = scheduler.currentPrincipal();
    const fault_page_va = cr2 & ~@as(u64, 4095);
    const write_access = (frame.error_code & (1 << 1)) != 0;
    const instruction_fetch = (frame.error_code & (1 << 4)) != 0;

    if (!h.kernel_state_ready.*) {
        h.write("  VMA_LOOKUP=kernel_state_not_ready\n");
        h.write("  MAPPED_PADDR=kernel_state_not_ready\n");
        return;
    }

    user_vm.lockAddressSpaces();
    defer user_vm.unlockAddressSpaces();

    const vma_mapping = h.state.nativeVmaFaultMapping(
        principal,
        fault_page_va,
        write_access,
        instruction_fetch,
    );
    h.write("  VMA_LOOKUP=");
    h.write(if (vma_mapping != null) "found(native)\n" else "none(native)\n");

    const mapped_paddr = user_vm.lookupUserMappedPaddrForVa(principal, fault_page_va) orelse {
        h.write("  MAPPED_PADDR=none\n");
        return;
    };
    h.write("  MAPPED_PADDR=");
    h.write_hex_raw(mapped_paddr);
    h.write("\n");
}
