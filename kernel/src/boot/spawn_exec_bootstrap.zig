const std = @import("std");
const kernel = @import("../kernel.zig");
const capability = @import("../capability.zig");
const abi_root = @import("kernel_abi_root");
const image_abi = abi_root.image_abi;
const process_abi = abi_root.process_abi;
const user_vm = @import("../memory/user_vm.zig");

pub const Hooks = struct {
    state: *kernel.KernelState,
    copy_user_bytes_from_va: *const fn (kernel.PrincipalId, u64, []u8) bool,
    write_user_u64: *const fn (kernel.PrincipalId, u64, u64) bool,
    write: *const fn ([]const u8) void,
    write_error_name: *const fn (anyerror) void,
    syscall_ok: u64,
    syscall_err_invalid: u64,
    syscall_err_map: u64,
    syscall_err_grant: u64,
};

pub const Request = struct {
    requested: bool = false,
    child_bootstrap_owner: bool = false,
    descriptor_mode: bool = false,
    extended_descriptor_mode: bool = false,
    page_descriptors: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined,
    page_descriptor_count: usize = 0,
    cap_descriptors: [process_abi.max_bootstrap_cap_descriptors]process_abi.BootstrapCapDescriptor = undefined,
    cap_descriptor_count: usize = 0,
};

pub fn parseRequest(
    hooks: Hooks,
    caller: kernel.PrincipalId,
    bootstrap_source_va: u64,
    bootstrap_target_va: u64,
    bootstrap_flags: u64,
    out: *Request,
) u64 {
    out.* = .{};
    const child_bootstrap_owner = (bootstrap_flags & process_abi.spawn_flag_child_bootstrap_owner) != 0;
    const request_flags = bootstrap_flags & ~process_abi.spawn_flag_child_bootstrap_owner;
    out.child_bootstrap_owner = child_bootstrap_owner;
    if (child_bootstrap_owner and !hooks.state.isBootstrapOwner(caller)) {
        hooks.write("spawn_exec child bootstrap owner denied\n");
        return hooks.syscall_err_invalid;
    }
    out.requested = bootstrap_source_va != 0 or bootstrap_target_va != 0 or request_flags != 0;
    if (!out.requested) return hooks.syscall_ok;

    out.descriptor_mode = (request_flags & process_abi.spawn_flag_bootstrap_descriptor_table) != 0;
    out.extended_descriptor_mode = (request_flags & process_abi.spawn_flag_bootstrap_extended_descriptor_table) != 0;

    if (out.extended_descriptor_mode) {
        if ((request_flags & ~process_abi.spawn_flag_bootstrap_extended_descriptor_table) != 0) {
            hooks.write("spawn_exec bootstrap bad flags\n");
            return hooks.syscall_err_invalid;
        }
        if (bootstrap_source_va == 0) {
            hooks.write("spawn_exec bootstrap descriptor table missing\n");
            return hooks.syscall_err_invalid;
        }
        var bootstrap_table: process_abi.BootstrapDescriptorTable = undefined;
        const table_bytes = std.mem.asBytes(&bootstrap_table);
        if (!hooks.copy_user_bytes_from_va(caller, bootstrap_source_va, table_bytes)) {
            hooks.write("spawn_exec bootstrap descriptor copy fail\n");
            return hooks.syscall_err_invalid;
        }
        out.page_descriptor_count = bootstrap_table.page_count;
        out.cap_descriptor_count = bootstrap_table.cap_count;
        if (out.page_descriptor_count > process_abi.max_bootstrap_page_descriptors or
            out.cap_descriptor_count > process_abi.max_bootstrap_cap_descriptors)
        {
            hooks.write("spawn_exec bootstrap descriptor count invalid\n");
            return hooks.syscall_err_invalid;
        }
        var i: usize = 0;
        while (i < out.page_descriptor_count) : (i += 1) {
            out.page_descriptors[i] = bootstrap_table.page_descriptors[i];
        }
        i = 0;
        while (i < out.cap_descriptor_count) : (i += 1) {
            out.cap_descriptors[i] = bootstrap_table.cap_descriptors[i];
        }
        return hooks.syscall_ok;
    }

    if (out.descriptor_mode) {
        if ((request_flags & ~process_abi.spawn_flag_bootstrap_descriptor_table) != 0) {
            hooks.write("spawn_exec bootstrap bad flags\n");
            return hooks.syscall_err_invalid;
        }
        if (bootstrap_source_va == 0 or bootstrap_target_va == 0) {
            hooks.write("spawn_exec bootstrap descriptor table missing\n");
            return hooks.syscall_err_invalid;
        }
        if (bootstrap_target_va > process_abi.max_bootstrap_page_descriptors) {
            hooks.write("spawn_exec bootstrap descriptor count invalid\n");
            return hooks.syscall_err_invalid;
        }
        out.page_descriptor_count = @intCast(bootstrap_target_va);
        if (out.page_descriptor_count == 0) {
            hooks.write("spawn_exec bootstrap descriptor count invalid\n");
            return hooks.syscall_err_invalid;
        }
        const desc_bytes = std.mem.sliceAsBytes(out.page_descriptors[0..out.page_descriptor_count]);
        if (!hooks.copy_user_bytes_from_va(caller, bootstrap_source_va, desc_bytes)) {
            hooks.write("spawn_exec bootstrap descriptor copy fail\n");
            return hooks.syscall_err_invalid;
        }
    }
    return hooks.syscall_ok;
}

pub fn installPage(
    hooks: Hooks,
    caller: kernel.PrincipalId,
    child: kernel.PrincipalId,
    source_va: u64,
    target_va: u64,
    descriptor_flags: u64,
) u64 {
    if (source_va == 0 or target_va == 0) {
        hooks.write("spawn_exec bootstrap missing va\n");
        return hooks.syscall_err_invalid;
    }
    if ((source_va & 0xFFF) != 0 or (target_va & 0xFFF) != 0) {
        hooks.write("spawn_exec bootstrap unaligned va\n");
        return hooks.syscall_err_invalid;
    }
    if ((descriptor_flags & ~process_abi.spawn_flag_bootstrap_page_writable) != 0) {
        hooks.write("spawn_exec bootstrap bad flags\n");
        return hooks.syscall_err_invalid;
    }

    const bootstrap_paddr = capability.lookupUserMappedPaddrForVa(caller, source_va) orelse {
        hooks.write("spawn_exec bootstrap lookup fail\n");
        return hooks.syscall_err_invalid;
    };
    const bootstrap_cap = hooks.state.getTableConst(caller).find(bootstrap_paddr) orelse {
        hooks.write("spawn_exec bootstrap cap missing\n");
        return hooks.syscall_err_invalid;
    };
    const writable = (descriptor_flags & process_abi.spawn_flag_bootstrap_page_writable) != 0;
    if (!bootstrap_cap.rights.cpu_read) {
        hooks.write("spawn_exec bootstrap no read right\n");
        return hooks.syscall_err_invalid;
    }
    if (writable and !bootstrap_cap.rights.cpu_write) {
        hooks.write("spawn_exec bootstrap no write right\n");
        return hooks.syscall_err_invalid;
    }
    hooks.state.grantCapToFreshUnmappedProcess(caller, child, bootstrap_paddr, .{
        .cpu_read = true,
        .cpu_write = writable,
        .dma = false,
    }) catch |err| {
        hooks.write("spawn_exec bootstrap grant failed: ");
        hooks.write_error_name(err);
        hooks.write("\n");
        return hooks.syscall_err_grant;
    };
    if (!user_vm.mapUserLinearRegion(child, target_va, bootstrap_paddr, 4096, writable)) {
        hooks.write("spawn_exec bootstrap map fail\n");
        return hooks.syscall_err_map;
    }
    return hooks.syscall_ok;
}

pub fn installCap(
    hooks: Hooks,
    caller: kernel.PrincipalId,
    child: kernel.PrincipalId,
    descriptor: process_abi.BootstrapCapDescriptor,
) u64 {
    if (descriptor.target_token_va == 0 or (descriptor.target_token_va & 0x7) != 0) {
        hooks.write("spawn_exec bootstrap cap bad target\n");
        return hooks.syscall_err_invalid;
    }

    const encoded_child_token = switch (descriptor.kind) {
        .vm_object => blk: {
            const cap_id = image_abi.decodeVmObjectToken(descriptor.source_token) orelse {
                hooks.write("spawn_exec bootstrap vm decode fail\n");
                return hooks.syscall_err_invalid;
            };
            const child_id = hooks.state.grantVmObjectCap(
                caller,
                child,
                cap_id,
                @bitCast(image_abi.vmObjectRightsFromBits(descriptor.rights_bits)),
            ) catch {
                hooks.write("spawn_exec bootstrap vm grant fail\n");
                return hooks.syscall_err_grant;
            };
            break :blk image_abi.encodeVmObjectToken(child_id);
        },
    };

    if (!hooks.write_user_u64(child, descriptor.target_token_va, encoded_child_token)) {
        hooks.write("spawn_exec bootstrap cap write fail\n");
        return hooks.syscall_err_map;
    }
    return hooks.syscall_ok;
}
