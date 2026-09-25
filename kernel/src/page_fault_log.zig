const kernel = @import("kernel.zig");
const interrupts = @import("interrupts.zig");
const scheduler = @import("scheduler.zig").connection;
const user_vm = @import("memory/user_vm.zig");
const user_copy = @import("user_copy.zig");

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

fn checkedStackWordVa(rsp: u64, index: usize) ?u64 {
    if (index >= 8) return null;
    const va, const overflow = @addWithOverflow(rsp, @as(u64, @intCast(index)) * 8);
    if (overflow != 0) return null;
    const range_end = @addWithOverflow(va, @as(u64, 7));
    if (range_end[1] != 0 or (va & 4095) > 4088) return null;
    return va;
}

fn stackWordInReadableVma(va: u64, entry: *const kernel.VmaEntry) bool {
    return entry.active and entry.prot.read and va >= entry.start_va and
        entry.size_bytes >= 8 and va - entry.start_va <= entry.size_bytes - 8;
}

// The caller holds the address-space lock. Never fault in stack pages, follow
// arbitrary frame pointers, or read MMIO/pinned mappings without native VMAs.
// These are candidate words, not an unwound backtrace. For leaf memcpy the
// first word is the return address because memcpy does not change RSP.
fn logPresentStackWords(h: *const Hooks, principal: kernel.PrincipalId, rsp: u64) void {
    for (0..8) |index| {
        const va = checkedStackWordVa(rsp, index) orelse {
            h.write("  STACK_STOP=overflow_or_page_boundary\n");
            return;
        };
        if (!user_vm.isUserCanonicalVa(va) or !user_vm.isUserCanonicalVa(va + 7)) {
            h.write("  STACK_STOP=non_user_address\n");
            return;
        }
        const entry = h.state.vmaEntryForVaConst(principal, va) orelse {
            h.write("  STACK_STOP=no_native_vma\n");
            return;
        };
        if (!stackWordInReadableVma(va, entry)) {
            h.write("  STACK_STOP=not_readable_vma\n");
            return;
        }
        const page_va = va & ~@as(u64, 4095);
        const paddr = user_vm.lookupUserMappedPaddrForAccessWithAddressSpaceLocked(
            principal,
            page_va,
            false,
            false,
        ) orelse {
            h.write("  STACK_STOP=not_present\n");
            return;
        };
        const word_paddr, const overflow = @addWithOverflow(paddr, va & 4095);
        var word: u64 = undefined;
        if (overflow != 0 or !user_copy.readPhysicalBytes(word_paddr, @import("std").mem.asBytes(&word))) {
            h.write("  STACK_STOP=physical_read_failed\n");
            return;
        }
        h.write("  STACK_WORD va=");
        h.write_hex_raw(va);
        h.write(" value=");
        h.write_hex_raw(word);
        if (user_vm.isUserCanonicalVa(word)) {
            if (h.state.vmaEntryForVaConst(principal, word)) |candidate| {
                if (candidate.prot.exec) {
                    h.write(" exec_vma_start=");
                    h.write_hex_raw(candidate.start_va);
                    h.write(" exec_vma_size=");
                    h.write_hex_raw(candidate.size_bytes);
                    h.write(" exec_vmo_offset=");
                    h.write_hex_raw(candidate.vmo_offset);
                }
            }
        }
        h.write("\n");
    }
}

/// Fatal user traps without an error code do not run logStep2. Reuse exactly
/// the same bounded, present-page-only stack reader before process teardown.
pub fn logUserTrapStack(frame: *const interrupts.TrapFrame) void {
    if ((frame.cs & 3) != 3 or !page_fault_log_hooks_ready) return;
    const h = getHooks();
    if (!h.kernel_state_ready.*) return;
    const principal = scheduler.currentPrincipal();
    if (!user_vm.lockAddressSpace(principal)) return;
    defer user_vm.unlockAddressSpace(principal);
    logPresentStackWords(h, principal, frame.rsp);
}

test "fault stack word bounds and readable VMA checks" {
    const testing = @import("std").testing;
    try testing.expectEqual(@as(?u64, 0x7fb8), checkedStackWordVa(0x7fb8, 0));
    try testing.expectEqual(@as(?u64, 0x7ff0), checkedStackWordVa(0x7fb8, 7));
    try testing.expectEqual(@as(?u64, 0x8000), checkedStackWordVa(0x7ff8, 1));
    try testing.expect(checkedStackWordVa(0x7fb8, 8) == null);
    try testing.expect(checkedStackWordVa(0x7ffc, 0) == null);
    try testing.expect(checkedStackWordVa(@import("std").math.maxInt(u64), 1) == null);
    try testing.expect(checkedStackWordVa(@import("std").math.maxInt(u64) - 3, 0) == null);
    var entry = kernel.VmaEntry{ .active = true, .start_va = 0x7000, .size_bytes = 4096, .prot = .{ .read = true } };
    try testing.expect(stackWordInReadableVma(0x7ff8, &entry));
    try testing.expect(!stackWordInReadableVma(0x6ff8, &entry));
    try testing.expect(!stackWordInReadableVma(0x7ff9, &entry));
    try testing.expect(!stackWordInReadableVma(0x8000, &entry));
    entry.prot.read = false;
    try testing.expect(!stackWordInReadableVma(0x7000, &entry));
    entry.prot.read = true;
    entry.active = false;
    try testing.expect(!stackWordInReadableVma(0x7000, &entry));
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

    if (!user_vm.lockAddressSpace(principal)) return;
    defer user_vm.unlockAddressSpace(principal);

    const vma_mapping = blk: {
        user_vm.lockSharedVmObjects();
        defer user_vm.unlockSharedVmObjects();
        break :blk h.state.nativeVmaFaultMapping(
            principal,
            fault_page_va,
            write_access,
            instruction_fetch,
        );
    };
    h.write("  VMA_LOOKUP=");
    h.write(if (vma_mapping != null) "found(native)\n" else "none(native)\n");

    if (h.state.vmaEntryForVaConst(principal, fault_page_va)) |fault_entry| {
        h.write("  FAULT_VMA_START=");
        h.write_hex_raw(fault_entry.start_va);
        h.write("\n  FAULT_VMA_SIZE=");
        h.write_hex_raw(fault_entry.size_bytes);
        h.write("\n  FAULT_VMA_PRIVATE=");
        h.write_bool01(fault_entry.flags.private);
        h.write("\n  FAULT_VMA_ANONYMOUS=");
        h.write_bool01(fault_entry.flags.anonymous);
        h.write("\n  FAULT_VMA_FORK_COW=");
        h.write_bool01(fault_entry.flags.fork_cow);
        h.write("\n  FAULT_VMA_COW_TABLE=");
        h.write(if (fault_entry.cow_table.isNull()) "none\n" else "present\n");
    }

    // The faulting address says what was touched; the mapping that holds the
    // instruction says which image was running.  Without it a report gives an
    // instruction pointer nobody can resolve to a file.
    if (h.state.vmaEntryForVaConst(principal, frame.rip)) |rip_entry| {
        h.write("  RIP_VMA_START=");
        h.write_hex_raw(rip_entry.start_va);
        h.write("\n  RIP_VMA_SIZE=");
        h.write_hex_raw(rip_entry.size_bytes);
        h.write("\n");
    } else {
        h.write("  RIP_VMA=none\n");
    }

    logPresentStackWords(h, principal, frame.rsp);

    // TEMPORARY (apk network-install fault): the instruction itself, so the
    // faulting store can be decoded without first guessing which file the
    // address belongs to.
    {
        const rip_page = frame.rip & ~@as(u64, 4095);
        const rip_offset = frame.rip & 4095;
        if (rip_offset + 16 <= 4096) {
            if (user_vm.lookupUserMappedPaddrForVa(principal, rip_page)) |rip_paddr| {
                var words: [2]u64 = undefined;
                // Instruction addresses need not be DWORD/QWORD aligned.
                if (!user_copy.readPhysicalBytes(rip_paddr + rip_offset, @import("std").mem.asBytes(&words))) return;
                h.write("  RIP_BYTES0=");
                h.write_hex_raw(words[0]);
                h.write("\n  RIP_BYTES1=");
                h.write_hex_raw(words[1]);
                h.write("\n");
            } else {
                h.write("  RIP_BYTES=unmapped\n");
            }
        }
    }

    const mapped_paddr = user_vm.lookupUserMappedPaddrForVa(principal, fault_page_va) orelse {
        h.write("  MAPPED_PADDR=none\n");
        return;
    };
    h.write("  MAPPED_PADDR=");
    h.write_hex_raw(mapped_paddr);
    h.write("\n");
}
