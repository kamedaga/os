/// Dynamic process spawning via the spawn_exec syscall.
/// Must be initialized via init() before any syscall is dispatched.
const std = @import("std");
const kernel = @import("../kernel.zig");
const scheduler = @import("../scheduler.zig");
const interrupts = @import("../interrupts.zig");
const user_copy = @import("../user_copy.zig");
const boot_static = @import("../boot/main_static.zig");
const boot_abi = @import("../boot/abi.zig");
const spawn_exec_bootstrap = @import("../boot/spawn_exec_bootstrap.zig");
const process_factory = @import("../boot/process_factory.zig");
const elf_load = @import("../boot/elf_load.zig");
const kernel_log = @import("../kernel_log.zig");
const log_util = @import("../log_util.zig");

const TrapFrame = interrupts.TrapFrame;

// ---------------------------------------------------------------------------
// Module state (initialized once during boot)
// ---------------------------------------------------------------------------

var state_ptr: *kernel.KernelState = undefined;
var free_list_ptr: *kernel.FreePageList = undefined;
var user_spaces_ptr: []@import("../boot/main_static.zig").UserAddressSpace = undefined;
var boot_init_principal: ?kernel.PrincipalId = null;

pub fn init(
    state: *kernel.KernelState,
    free_list: *kernel.FreePageList,
    user_spaces: []boot_static.UserAddressSpace,
    init_principal: ?kernel.PrincipalId,
) void {
    state_ptr = state;
    free_list_ptr = free_list;
    user_spaces_ptr = user_spaces;
    boot_init_principal = init_principal;
}

// ---------------------------------------------------------------------------
// spawn_exec syscall handler
// ---------------------------------------------------------------------------

pub fn spawnExecFromSyscall(frame: *TrapFrame) u64 {
    const caller = scheduler.current_user_principal;
    const cap_id = boot_abi.image_abi.decodeExecImageToken(frame.rdi) orelse return boot_static.syscall_err_invalid;
    const bootstrap_source_va = frame.rsi;
    const bootstrap_target_va = frame.rdx;
    const bootstrap_flags = frame.rcx;
    const exec_cap = state_ptr.getExecImageTableConst(caller).findByCapId(cap_id) orelse return boot_static.syscall_err_invalid;
    if (!exec_cap.rights.exec) return boot_static.syscall_err_invalid;
    if (exec_cap.backing.size_bytes > boot_static.user_program_max_load_bytes) return boot_static.syscall_err_invalid;

    const bootstrap_hooks: spawn_exec_bootstrap.Hooks = .{
        .state = state_ptr,
        .copy_user_bytes_from_va = user_copy.copyUserBytesFromVa,
        .write_user_u64 = user_copy.writeUserU64,
        .write = kernel_log.write,
        .write_error_name = log_util.writeErrorName,
        .syscall_ok = boot_static.syscall_ok,
        .syscall_err_invalid = boot_static.syscall_err_invalid,
        .syscall_err_map = boot_static.syscall_err_map,
        .syscall_err_grant = boot_static.syscall_err_grant,
    };
    var bootstrap_request = spawn_exec_bootstrap.Request{};
    const bootstrap_rc = spawn_exec_bootstrap.parseRequest(
        bootstrap_hooks,
        caller,
        bootstrap_source_va,
        bootstrap_target_va,
        bootstrap_flags,
        &bootstrap_request,
    );
    if (bootstrap_rc != boot_static.syscall_ok) return bootstrap_rc;

    const created = process_factory.tryCreateDynamicUserProcess(state_ptr, "spawned exec", free_list_ptr, user_spaces_ptr) catch return boot_static.syscall_err_alloc;
    _ = scheduler.setThreadReady(created.process.thread_slot, false);

    process_factory.installSpawnParentEndpoint(state_ptr, created.principal, caller) catch return boot_static.syscall_err_endpoint;

    const loaded = elf_load.loadUserElfIntoProcessPagesFromBacking(
        state_ptr,
        created.principal,
        created.process.user_page.paddr,
        created.process.user_stack_page.paddr,
        exec_cap.backing,
        free_list_ptr,
    ) orelse {
        kernel_log.write("spawn_exec ELF load failed\n");
        return boot_static.syscall_err_invalid;
    };

    if (bootstrap_request.requested) {
        if (bootstrap_request.extended_descriptor_mode or bootstrap_request.descriptor_mode) {
            var i: usize = 0;
            while (i < bootstrap_request.page_descriptor_count) : (i += 1) {
                const desc = bootstrap_request.page_descriptors[i];
                const rc = spawn_exec_bootstrap.installPage(bootstrap_hooks, caller, created.principal, desc.source_va, desc.target_va, desc.flags);
                if (rc != boot_static.syscall_ok) return rc;
            }
            if (bootstrap_request.extended_descriptor_mode) {
                i = 0;
                while (i < bootstrap_request.cap_descriptor_count) : (i += 1) {
                    const rc = spawn_exec_bootstrap.installCap(bootstrap_hooks, caller, created.principal, bootstrap_request.cap_descriptors[i]);
                    if (rc != boot_static.syscall_ok) return rc;
                }
            }
        } else {
            const rc = spawn_exec_bootstrap.installPage(bootstrap_hooks, caller, created.principal, bootstrap_source_va, bootstrap_target_va, bootstrap_flags);
            if (rc != boot_static.syscall_ok) return rc;
        }
    }
    if (bootstrap_request.child_bootstrap_owner) {
        state_ptr.setBootstrapOwner(created.principal, true) catch return boot_static.syscall_err_invalid;
    }

    const ctx = scheduler.getThreadContext(created.process.thread_slot).?;
    ctx.frame.rip = loaded.entry;
    ctx.frame.rsp = boot_static.user_entry_rsp;
    if (!scheduler.setThreadReady(created.process.thread_slot, true)) return boot_static.syscall_err_not_ready;

    if (boot_init_principal) |init_principal| {
        if (caller == init_principal) {
            kernel_log.write("spawn_exec ready child=");
            log_util.printNumber(@as(u64, @intCast(kernel.processIndexFromPrincipal(created.principal).?)));
            kernel_log.write(" thread=");
            log_util.printNumber(@as(u64, @intCast(created.process.thread_slot)));
            kernel_log.write("\n");
        }
    }

    return boot_abi.process_abi.encodeSpawnedProcess(
        @intCast(kernel.processIndexFromPrincipal(created.principal).?),
        @intCast(created.process.thread_slot),
    );
}

// ---------------------------------------------------------------------------
// launchPieUserThread stub (not yet implemented)
// ---------------------------------------------------------------------------

pub fn launchPieUserThread(_: *TrapFrame) u64 {
    return boot_static.syscall_err_invalid;
}
