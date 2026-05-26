const interrupts = @import("../interrupts.zig");
const kernel = @import("../kernel.zig");
const user_vm = @import("../memory/user_vm.zig");
const sc = @import("numbers.zig");

const TrapFrame = interrupts.TrapFrame;

fn vmObjectInstallError(err: kernel.KernelError) u64 {
    return switch (err) {
        kernel.KernelError.InvalidState => sc.syscall_err_invalid,
        kernel.KernelError.TableFull => sc.syscall_err_alloc,
        else => sc.syscall_err_grant,
    };
}

fn vmObjectGrantError(err: kernel.KernelError) u64 {
    return switch (err) {
        kernel.KernelError.VmObjectCapabilityNotFound => sc.syscall_err_send,
        kernel.KernelError.InvalidState => sc.syscall_err_invalid,
        kernel.KernelError.TableFull => sc.syscall_err_alloc,
        else => sc.syscall_err_grant,
    };
}

pub fn dispatch(
    h: anytype,
    state: *kernel.KernelState,
    proc: kernel.PrincipalId,
    frame: *TrapFrame,
    vm_object_page_scratch: *[kernel.max_vm_object_backing_pages]u64,
    free_list: *kernel.FreePageList,
) ?u64 {
    return switch (frame.rax) {
        sc.syscall_create_vm_object_from_current_pages => blk: {
            const size_bytes = frame.rsi;
            if (size_bytes == 0) return sc.syscall_err_invalid;
            const page_offset_bytes: u16 = @intCast(frame.rdi & 0xFFF);
            const page_base = frame.rdi & ~@as(u64, 0xFFF);
            const span_bytes = (@as(u64, page_offset_bytes) + size_bytes + 4095) & ~@as(u64, 4095);
            const page_count_u64 = span_bytes / 4096;
            if (page_count_u64 == 0 or page_count_u64 > kernel.max_vm_object_backing_pages) return sc.syscall_err_invalid;

            const page_paddrs = vm_object_page_scratch;
            const collected = user_vm.collectUserLinearRegionPaddrs(proc, page_base, @intCast(span_bytes), page_paddrs) orelse return sc.syscall_err_invalid;
            const page_count: usize = @intCast(page_count_u64);
            if (collected != page_count) return sc.syscall_err_invalid;

            for (page_paddrs[0..page_count]) |paddr| {
                const page_cap = state.getTableConst(proc).find(paddr) orelse return sc.syscall_err_invalid;
                if (!page_cap.rights.cpu_read or !page_cap.rights.cpu_write) return sc.syscall_err_invalid;
            }

            const cap_id = state.installVmObjectCap(
                proc,
                page_paddrs[0..page_count],
                page_offset_bytes,
                size_bytes,
                kernel.vmObjectRightsFromBits(frame.rdx),
            ) catch |err| return vmObjectInstallError(err);
            if (!user_vm.unmapUserLinearRegion(proc, page_base, @intCast(span_bytes))) return sc.syscall_err_map;
            for (page_paddrs[0..page_count]) |paddr| {
                _ = state.getTable(proc).removeByPaddr(paddr);
            }
            break :blk kernel.encodeVmObjectToken(cap_id);
        },
        sc.syscall_grant_vm_object => {
            const cap_id = kernel.decodeVmObjectToken(frame.rdi) orelse return sc.syscall_err_invalid;
            const to = h.principal_from_process_slot(frame.rsi) orelse return sc.syscall_err_invalid;
            const child_id = state.grantVmObjectCap(proc, to, cap_id, kernel.vmObjectRightsFromBits(frame.rdx)) catch |err| return vmObjectGrantError(err);
            return kernel.encodeVmObjectToken(child_id);
        },
        sc.syscall_release_vm_object => {
            const cap_id = kernel.decodeVmObjectToken(frame.rdi) orelse return sc.syscall_err_invalid;
            const mapped_va = frame.rsi;
            const size_bytes = frame.rdx;
            if (mapped_va != 0 or size_bytes != 0) {
                if (mapped_va == 0 or size_bytes == 0) return sc.syscall_err_invalid;
                if ((mapped_va & 0xFFF) != 0 or (size_bytes & 0xFFF) != 0) return sc.syscall_err_invalid;
                if (!user_vm.unmapUserLinearRegion(proc, mapped_va, @intCast(size_bytes))) return sc.syscall_err_map;
            }
            state.revokeVmObjectCapTree(proc, cap_id, free_list) catch return sc.syscall_err_revoke;
            return sc.syscall_ok;
        },
        sc.syscall_drop_vm_object => {
            const cap_id = kernel.decodeVmObjectToken(frame.rdi) orelse return sc.syscall_err_invalid;
            state.dropVmObjectCap(proc, cap_id, free_list) catch return sc.syscall_err_revoke;
            return sc.syscall_ok;
        },
        sc.syscall_map_vm_object => blk: {
            const cap_id = kernel.decodeVmObjectToken(frame.rdi) orelse return sc.syscall_err_invalid;
            const target_va = frame.rsi;
            if ((target_va & 0xFFF) != 0) return sc.syscall_err_invalid;
            const vm_cap = state.getVmObjectTableConst(proc).findByCapId(cap_id) orelse return sc.syscall_err_invalid;
            if (!vm_cap.rights.read or !vm_cap.rights.map) return sc.syscall_err_invalid;
            if (vm_cap.backing.page_offset_bytes != 0) return sc.syscall_err_invalid;
            var i: usize = 0;
            while (i < vm_cap.backing.page_count) {
                const run_start = i;
                const run_paddr = vm_cap.backing.pagePaddr(run_start) orelse return sc.syscall_err_invalid;
                var run_len: usize = 1;
                while (run_start + run_len < vm_cap.backing.page_count) : (run_len += 1) {
                    const expected = run_paddr + @as(u64, @intCast(run_len)) * 4096;
                    if ((vm_cap.backing.pagePaddr(run_start + run_len) orelse break) != expected) break;
                }
                if (!user_vm.mapUserLinearRegion(
                    proc,
                    target_va + @as(u64, @intCast(run_start)) * 4096,
                    run_paddr,
                    @as(u64, @intCast(run_len)) * 4096,
                    vm_cap.rights.write,
                )) {
                    return sc.syscall_err_map;
                }
                i = run_start + run_len;
            }
            break :blk sc.syscall_ok;
        },
        else => null,
    };
}
