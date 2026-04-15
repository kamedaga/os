/// Kernel boot entry point and primary boot globals.
/// kernelMain() is called from main.zig after switching to the ring-0 stack.
const std = @import("std");
const kernel = @import("../kernel.zig");
const capability = @import("../capability.zig");
const untyped_memory = @import("../untyped_memory.zig");
const elf_loader = @import("../elf_loader.zig");
const scheduler = @import("../scheduler.zig");
const syscalls = @import("../syscalls.zig");
const traps = @import("../traps.zig");
const interrupts = @import("../interrupts.zig");
const lapic = @import("../lapic.zig");
const serial = @import("../serial.zig");
const kernel_log = @import("../kernel_log.zig");
const page_fault_log = @import("../page_fault_log.zig");
const user_copy = @import("../user_copy.zig");
const virtio_probe = @import("../virtio_probe.zig");
const kernel_vm = @import("../memory/kernel_vm.zig");
const pmm = @import("../memory/pmm.zig");
const user_vm = @import("../memory/user_vm.zig");
const x86_platform = @import("../arch/x86_64/platform.zig");
const boot_debug = @import("debug.zig");
const boot_static = @import("main_static.zig");
const boot_images = @import("boot_images.zig");
const boot_abi = @import("abi.zig");
const init_bootstrap_layout = @import("init_bootstrap_layout.zig");
const uefi_services = @import("uefi_services.zig");
const process_factory = @import("process_factory.zig");
const elf_load = @import("elf_load.zig");
const init_setup = @import("init_setup.zig");
const spawn = @import("../runtime/spawn.zig");
const halt = @import("../halt.zig");
const log_util = @import("../log_util.zig");

const TrapFrame = interrupts.TrapFrame;
const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;

// ---------------------------------------------------------------------------
// Boot globals
// ---------------------------------------------------------------------------

var user_spaces_storage: [boot_static.user_process_count]boot_static.UserAddressSpace align(4096) =
    [_]boot_static.UserAddressSpace{.{}} ** boot_static.user_process_count;
var user_spaces: []boot_static.UserAddressSpace = user_spaces_storage[0..];

pub var global_free_list: kernel.FreePageList = .{};
pub var kernel_state_global: kernel.KernelState = undefined;
pub var kernel_state_ready: bool = false;

var boot_init_principal: ?kernel.PrincipalId = null;
const spawn_parent_endpoint_id: u64 = 0x14;

// ---------------------------------------------------------------------------
// Thread label helper (used in Hooks)
// ---------------------------------------------------------------------------

fn threadLabel(thread_index: usize) []const u8 {
    const labels = comptime blk: {
        var items: [boot_static.user_thread_count][]const u8 = undefined;
        for (0..boot_static.user_thread_count) |i| {
            items[i] = std.fmt.comptimePrint("Thread{}", .{i});
        }
        break :blk items;
    };
    if (thread_index < labels.len) return labels[thread_index];
    return "Thread?";
}

fn principalLabel(principal: kernel.PrincipalId) []const u8 {
    if (kernel_state_ready) {
        if (kernel_state_global.processDescriptor(principal)) |desc| {
            return desc.label;
        }
    }
    return kernel.principalLabel(principal);
}

fn principalFromProcessSlot(raw: u64) ?kernel.PrincipalId {
    const idx = std.math.cast(usize, raw) orelse return null;
    return kernel.processPrincipalFromIndex(idx);
}

fn dumpAllProcessCaps(state: *const kernel.KernelState) void {
    boot_debug.dumpAllProcessCaps(state, boot_static.user_process_count, principalLabel);
}

fn bootDebugHooks() boot_debug.Hooks {
    return .{
        .write = kernel_log.write,
        .print_number = log_util.printNumberU64,
        .print_hex = log_util.printHex,
        .principal_label = principalLabel,
    };
}

fn logQueueCapDeny(
    proc: kernel.PrincipalId,
    token: u64,
    queue_index: u16,
    op: kernel.QueueOperation,
    err: anyerror,
) void {
    boot_debug.logQueueCapDeny(bootDebugHooks(), proc, token, queue_index, op, err);
}

fn logSchedulerRaceSendCap(
    from: kernel.PrincipalId,
    to: ?kernel.PrincipalId,
    endpoint_id: u64,
    paddr: u64,
    reason: []const u8,
) void {
    scheduler.logRaceSendCap(
        .{ .write = kernel_log.write, .print_hex = log_util.printHex, .principal_label = principalLabel },
        boot_static.scheduler_race_log_max_lines,
        from,
        to,
        endpoint_id,
        paddr,
        reason,
    );
}

fn logSchedulerRaceSwitch(current_thread: usize, target_thread: usize, reason: []const u8) void {
    scheduler.logRaceSwitch(
        .{ .write = kernel_log.write, .print_hex = log_util.printHex, .principal_label = principalLabel },
        boot_static.scheduler_race_log_max_lines,
        current_thread,
        target_thread,
        reason,
    );
}

fn iommuAuditHook(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    paddr: u64,
    mapped: bool,
    reason: kernel.IommuSyncReason,
) void {
    _ = state;
    _ = principal;
    _ = paddr;
    _ = mapped;
    _ = reason;
}

// ---------------------------------------------------------------------------
// CPU control register helpers (x86-specific, used only during boot)
// ---------------------------------------------------------------------------

fn readCr2() u64 {
    return x86_platform.readCr2();
}

fn readCr3() u64 {
    return kernel_vm.readCr3();
}

fn writeCr3(value: u64) void {
    kernel_vm.writeCr3(value);
}

fn invlpg(addr: u64) void {
    kernel_vm.invlpg(addr);
}

fn readCr0() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr0, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

fn writeCr0(value: u64) void {
    asm volatile ("mov %[value], %%cr0"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

fn readCr4() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr4, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

fn writeCr4(value: u64) void {
    asm volatile ("mov %[value], %%cr4"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

fn userCr3ForPrincipal(principal: kernel.PrincipalId) u64 {
    const idx = kernel.processIndexFromPrincipal(principal) orelse return 0;
    if (idx >= user_spaces.len) return 0;
    return user_spaces[idx].cr3;
}

// ---------------------------------------------------------------------------
// FX state support (called during initKernelRuntimeOrHalt)
// ---------------------------------------------------------------------------

fn initFxStateSupport() void {
    var cr0 = readCr0();
    cr0 &= ~@as(u64, 1 << 2); // EM=0
    cr0 |= @as(u64, 1 << 1); // MP=1
    writeCr0(cr0);
    var cr4 = readCr4();
    cr4 |= @as(u64, (1 << 9) | (1 << 10)); // OSFXSR | OSXMMEXCPT
    writeCr4(cr4);
    asm volatile ("clts");
    asm volatile ("fninit");
    asm volatile ("fxsave64 (%[ptr])"
        :
        : [ptr] "r" (&scheduler.initial_fx_state),
        : .{ .memory = true });
}

// ---------------------------------------------------------------------------
// Exported FX save/restore (called from assembly stubs in traps)
// ---------------------------------------------------------------------------

pub export fn saveCurrentThreadFxState() callconv(.c) void {
    if (!kernel_state_ready) return;
    const ctx = scheduler.getThreadContext(scheduler.current_thread_index) orelse return;
    if (!ctx.ready) return;
    asm volatile ("fxsave64 (%[ptr])"
        :
        : [ptr] "r" (&ctx.fx_state),
        : .{ .memory = true });
}

pub export fn restoreCurrentThreadFxState() callconv(.c) void {
    if (!kernel_state_ready) return;
    const ctx = scheduler.getThreadContext(scheduler.current_thread_index) orelse return;
    if (!ctx.ready) return;
    asm volatile ("fxrstor64 (%[ptr])"
        :
        : [ptr] "r" (&ctx.fx_state),
        : .{ .memory = true });
}

pub export fn saveKernelInterruptFxState() callconv(.c) void {
    asm volatile ("fxsave64 (%[ptr])"
        :
        : [ptr] "r" (&scheduler.kernel_interrupt_fx_state),
        : .{ .memory = true });
}

pub export fn restoreKernelInterruptFxState() callconv(.c) void {
    asm volatile ("fxrstor64 (%[ptr])"
        :
        : [ptr] "r" (&scheduler.kernel_interrupt_fx_state),
        : .{ .memory = true });
}

// ---------------------------------------------------------------------------
// Platform initialization
// ---------------------------------------------------------------------------

fn installInterruptTrampolines() void {
    x86_platform.installInterruptTrampolines(.{
        .syscall_stub = @intFromPtr(&traps.syscallHandlerStub),
        .page_fault_stub = @intFromPtr(&traps.pageFaultHandlerStub),
        .general_protection_stub = @intFromPtr(&traps.generalProtectionHandlerStub),
        .double_fault_stub = @intFromPtr(&traps.doubleFaultHandlerStub),
        .invalid_opcode_stub = @intFromPtr(&traps.invalidOpcodeHandlerStub),
        .invalid_tss_stub = @intFromPtr(&traps.invalidTssHandlerStub),
        .segment_not_present_stub = @intFromPtr(&traps.segmentNotPresentHandlerStub),
        .stack_segment_fault_stub = @intFromPtr(&traps.stackSegmentFaultHandlerStub),
        .timer_interrupt_stub = @intFromPtr(&traps.timerInterruptHandlerStub),
        .lapic_timer_vector = boot_static.lapic_timer_vector,
    });
}

fn initKernelRuntimeOrHalt() void {
    x86_platform.loadGdtAndReloadSegments();
    initFxStateSupport();
    if (!boot_static.debug_skip_cr3_switch) {
        if (!x86_platform.installIdentityPageTables0To1GiB()) {
            halt.haltWithMessage("page table install failed");
        }
        x86_platform.hardenKernelMappingsSupervisorOnly();
    }
    installInterruptTrampolines();
    asm volatile ("cli");
    if (!lapic.initTimer(boot_static.lapic_timer_vector, boot_static.lapic_timer_initial_count)) {
        halt.haltWithMessage("LAPIC timer init failed");
    }
    if (!elf_loader.probe()) {
        halt.haltWithMessage("ELF loader probe failed");
    }
}

fn initMemoryModules() void {
    user_spaces = user_spaces_storage[0..];
    for (user_spaces) |*space| space.* = .{};

    user_vm.init(.{
        .user_spaces = user_spaces,
        .current_user_principal = &scheduler.current_user_principal,
        .four_gib = boot_static.four_gib,
        .physical_map_limit = boot_static.physical_map_limit_exclusive,
        .user_va = boot_static.user_va,
        .user_stack_page_va = boot_static.user_stack_page_va,
        .page_entries = boot_static.page_entries,
        .page_present = boot_static.page_present,
        .page_rw = boot_static.page_rw,
        .page_user = boot_static.page_user,
        .seed_user_pd_with_kernel_identity = x86_platform.seedUserPdWithKernelIdentity,
    });
    pmm.init(.{
        .write = kernel_log.write,
        .main_addr = @intFromPtr(&kernelMain),
        .kernel_cr3_addr = @intFromPtr(&x86_platform.kernel_cr3_value),
        .kernel_image_base_paddr = &uefi_services.kernel_image_base_paddr_ref,
        .kernel_image_size_bytes = &uefi_services.kernel_image_size_bytes_ref,
        .post_exit_load_scratch_addr = uefi_services.postExitLoadScratchEndAddr(),
        .reserved_low_mem_end = boot_static.reserved_low_mem_end,
    });
}

fn activateThreadOrHalt(thread_index: usize) void {
    if (scheduler.threadContextLooksCorrupted(thread_index)) {
        _ = scheduler.repairThreadContextWithSpaces(thread_index, user_spaces, process_factory.buildInitialUserTrapFrame());
    }
    if (scheduler.activateThread(thread_index)) return;
    if (scheduler.repairThreadContextWithSpaces(thread_index, user_spaces, process_factory.buildInitialUserTrapFrame()) and
        scheduler.activateThread(thread_index)) return;
    halt.haltWithMessage("activate thread failed");
}

fn scrubMailboxSender(mailbox: *kernel.CapMailbox, sender: kernel.PrincipalId) bool {
    var out: usize = 0;
    var changed = false;
    var i: usize = 0;
    while (i < mailbox.len) : (i += 1) {
        const item = mailbox.items[i];
        if (item.sender == sender) {
            changed = true;
            continue;
        }
        mailbox.items[out] = item;
        out += 1;
    }
    mailbox.len = out;
    return changed;
}

fn scrubEndpointTargets(table: *kernel.EndpointCNode, target: kernel.PrincipalId) bool {
    var out: usize = 0;
    var changed = false;
    var i: usize = 0;
    while (i < table.len) : (i += 1) {
        const entry = table.caps[i];
        if (entry.target == target) {
            changed = true;
            continue;
        }
        table.caps[out] = entry;
        out += 1;
    }
    table.len = out;
    return changed;
}

fn nextReadyThreadAfter(current_thread: usize) ?usize {
    var step: usize = 1;
    while (step <= scheduler.max_thread_slots) : (step += 1) {
        const thread_index = (current_thread + step) % scheduler.max_thread_slots;
        const ctx = scheduler.getThreadContextConst(thread_index) orelse continue;
        if (!ctx.allocated or !ctx.ready) continue;
        return thread_index;
    }
    return null;
}

fn loadRunnableThreadOrIdle(out_frame: *TrapFrame) void {
    while (true) {
        const current_thread = scheduler.current_thread_index;
        if (nextReadyThreadAfter(current_thread)) |thread_index| {
            if (scheduler.threadContextLooksCorrupted(thread_index)) {
                _ = scheduler.repairThreadContextWithSpaces(thread_index, user_spaces, process_factory.buildInitialUserTrapFrame());
            }
            if (scheduler.activateThread(thread_index) and scheduler.loadThreadContextToFrame(thread_index, out_frame)) return;
            if (scheduler.repairThreadContextWithSpaces(thread_index, user_spaces, process_factory.buildInitialUserTrapFrame()) and
                scheduler.activateThread(thread_index) and
                scheduler.loadThreadContextToFrame(thread_index, out_frame)) return;
            halt.haltWithMessage("fault reschedule failed");
        }
        asm volatile ("sti\nhlt\ncli" ::: .{ .memory = true });
    }
}

fn teardownFaultedProcess(principal: kernel.PrincipalId, fault_vector: u8) void {
    const process_index = kernel.processIndexFromPrincipal(principal) orelse return;
    const spawn_parent = kernel_state_global.endpointTargetFor(principal, spawn_parent_endpoint_id);

    if (scheduler.runtime_priority_principal) |priority_principal| {
        if (priority_principal == principal) scheduler.setRuntimePriorityPrincipal(null);
    }

    if (scheduler.threadSlotForPrincipal(principal)) |thread_index| {
        _ = scheduler.releaseThreadSlot(thread_index);
    }

    user_spaces[process_index] = .{};
    kernel_state_global.cap_tables[process_index] = .{};
    kernel_state_global.untyped_tables[process_index] = .{};
    kernel_state_global.endpoint_tables[process_index] = .{};
    kernel_state_global.cap_mailboxes[process_index] = .{};
    kernel_state_global.pending_page_transfers[process_index] = null;
    kernel_state_global.vm_object_tables[process_index] = .{};
    kernel_state_global.exec_image_tables[process_index] = .{};
    _ = kernel_state_global.unpublishServiceEndpointsForTarget(principal);

    var storage_index: usize = 0;
    while (storage_index < kernel.principal_count) : (storage_index += 1) {
        var wake_owner = false;
        if (storage_index < kernel.process_count) {
            wake_owner = scrubEndpointTargets(&kernel_state_global.endpoint_tables[storage_index], principal);
        }
        if (scrubMailboxSender(&kernel_state_global.cap_mailboxes[storage_index], principal)) {
            wake_owner = true;
        }
        if (kernel_state_global.pending_page_transfers[storage_index]) |pending| {
            if (pending.sender == principal) {
                kernel_state_global.pending_page_transfers[storage_index] = null;
                wake_owner = true;
            }
        }
        if (!wake_owner) continue;
        if (storage_index >= kernel.process_count) continue;
        const owner = kernel.processPrincipalFromIndex(storage_index) orelse continue;
        scheduler.wakeBlockedThreadForPrincipal(owner);
    }

    _ = kernel_state_global.markProcessFaulted(principal, fault_vector);
    if (spawn_parent) |parent| scheduler.wakeBlockedThreadForPrincipal(parent);
}

fn teardownExitedProcess(principal: kernel.PrincipalId) void {
    const process_index = kernel.processIndexFromPrincipal(principal) orelse return;
    const spawn_parent = kernel_state_global.endpointTargetFor(principal, spawn_parent_endpoint_id);

    if (scheduler.runtime_priority_principal) |priority_principal| {
        if (priority_principal == principal) scheduler.setRuntimePriorityPrincipal(null);
    }

    if (scheduler.threadSlotForPrincipal(principal)) |thread_index| {
        _ = scheduler.releaseThreadSlot(thread_index);
    }

    user_spaces[process_index] = .{};
    kernel_state_global.cap_tables[process_index] = .{};
    kernel_state_global.untyped_tables[process_index] = .{};
    kernel_state_global.endpoint_tables[process_index] = .{};
    kernel_state_global.cap_mailboxes[process_index] = .{};
    kernel_state_global.pending_page_transfers[process_index] = null;
    kernel_state_global.vm_object_tables[process_index] = .{};
    kernel_state_global.exec_image_tables[process_index] = .{};
    _ = kernel_state_global.unpublishServiceEndpointsForTarget(principal);

    var storage_index: usize = 0;
    while (storage_index < kernel.principal_count) : (storage_index += 1) {
        var wake_owner = false;
        if (storage_index < kernel.process_count) {
            wake_owner = scrubEndpointTargets(&kernel_state_global.endpoint_tables[storage_index], principal);
        }
        if (scrubMailboxSender(&kernel_state_global.cap_mailboxes[storage_index], principal)) {
            wake_owner = true;
        }
        if (kernel_state_global.pending_page_transfers[storage_index]) |pending| {
            if (pending.sender == principal) {
                kernel_state_global.pending_page_transfers[storage_index] = null;
                wake_owner = true;
            }
        }
        if (!wake_owner) continue;
        if (storage_index >= kernel.process_count) continue;
        const owner = kernel.processPrincipalFromIndex(storage_index) orelse continue;
        scheduler.wakeBlockedThreadForPrincipal(owner);
    }

    _ = kernel_state_global.markProcessExited(principal);
    if (spawn_parent) |parent| scheduler.wakeBlockedThreadForPrincipal(parent);
}

fn resumeAfterFatalUserException(principal: kernel.PrincipalId, fault_vector: u8, out_frame: *TrapFrame) void {
    teardownFaultedProcess(principal, fault_vector);
    loadRunnableThreadOrIdle(out_frame);
}

fn exitCurrentProcess(principal: kernel.PrincipalId, exit_code: u8, out_frame: *TrapFrame) void {
    _ = exit_code;
    teardownExitedProcess(principal);
    loadRunnableThreadOrIdle(out_frame);
}

fn mmioPageWithOffset(addr: u64) init_setup.MmioPageWithOffset {
    if (addr == 0) return .{ .page_paddr = 0, .page_offset = 0 };
    return .{
        .page_paddr = kernel_vm.pageAlignDown(addr),
        .page_offset = addr & 0xFFF,
    };
}

// ---------------------------------------------------------------------------
// Enter user mode (point of no return)
// ---------------------------------------------------------------------------

fn enterUserModeIretq(user_entry_va: u64, user_rsp: u64) noreturn {
    const user_cs: u64 = boot_static.gdt_user_code_selector | 0x3;
    const user_ss: u64 = boot_static.gdt_user_data_selector | 0x3;
    const user_rflags: u64 = boot_static.user_entry_rflags;
    const kernel_transition_rsp = x86_platform.ring0StackTop();

    restoreCurrentThreadFxState();

    // user_return_iret_frame lives in main.zig (exported asm symbol)
    const iret: *volatile [5]u64 = @extern(*volatile [5]u64, .{ .name = "user_return_iret_frame" });
    iret[0] = user_entry_va;
    iret[1] = user_cs;
    iret[2] = user_rflags;
    iret[3] = user_rsp;
    iret[4] = user_ss;

    asm volatile (
        \\mov %[k_rsp], %%rsp
        \\lea user_return_iret_frame(%rip), %%rsp
        \\mov %[ucr3], %%rax
        \\mov %%rax, %%cr3
        \\iretq
        :
        : [k_rsp] "r" (kernel_transition_rsp),
          [ucr3] "r" (scheduler.user_cr3_value),
        : .{ .memory = true });
    unreachable;
}

// ---------------------------------------------------------------------------
// Boot resource types
// ---------------------------------------------------------------------------

const BootResources = struct {
    framebuffer_info: uefi_services.FramebufferInfo,
    disk_init_elf: []const u8,
    disk_bootfs_image: []const u8,
    memory_stats: boot_static.MemoryStats,
};

const DetectedDevices = struct {
    devices: [boot_abi.init_bootstrap_abi.max_device_descriptors]?init_setup.DetectedDeviceBootstrap,
};

// ---------------------------------------------------------------------------
// Group 1 — UEFI / platform boot services
// ---------------------------------------------------------------------------

fn runBootServicesPhase() BootResources {
    const bs = uefi_services.acquireBootServicesOrHalt();
    uefi_services.captureKernelImageRange(bs);
    initMemoryModules();

    const framebuffer_info = uefi_services.acquireFramebufferInfo(bs) orelse {
        halt.haltWithMessage("GraphicsOutput unavailable or mode unsupported");
    };
    const disk_init_elf = uefi_services.loadBootDiskFile(bs, boot_images.init_app) orelse {
        halt.haltWithMessage("disk init ELF load failed");
    };
    const disk_bootfs_image = uefi_services.loadBootDiskFile(bs, boot_images.bootfs_image) orelse {
        halt.haltWithMessage("disk bootfs image load failed");
    };

    const memory_stats = uefi_services.collectBootMemoryStatsOrHalt(bs, &global_free_list, user_spaces);
    uefi_services.exitBootServicesOrHalt();
    initKernelRuntimeOrHalt();

    return .{
        .framebuffer_info = framebuffer_info,
        .disk_init_elf = disk_init_elf,
        .disk_bootfs_image = disk_bootfs_image,
        .memory_stats = memory_stats,
    };
}

// ---------------------------------------------------------------------------
// Group 2a — kernel subsystem wiring (pre-process)
// ---------------------------------------------------------------------------

fn initKernelSubsystems(memory_stats: boot_static.MemoryStats) *kernel.KernelState {
    capability.init(.{
        .user_spaces = user_spaces,
        .user_va = boot_static.user_va,
        .physical_map_limit = boot_static.physical_map_limit_exclusive,
        .page_entries = boot_static.page_entries,
        .page_addr_mask = boot_static.page_addr_mask,
        .page_present = boot_static.page_present,
        .page_rw = boot_static.page_rw,
        .page_user = boot_static.page_user,
        .canonical_user_limit_exclusive = boot_static.canonical_user_limit_exclusive,
        .serial_write = kernel_log.write,
        .print_hex = log_util.printHex,
        .principal_label = principalLabel,
        .flush_user_tlb_for_principal_va = user_copy.flushUserTlbForPrincipalVa,
    });
    user_copy.init(.{
        .physical_map_limit = boot_static.physical_map_limit_exclusive,
        .phys_copy_window_va = boot_static.phys_copy_window_va,
        .page_present = boot_static.page_present,
        .page_rw = boot_static.page_rw,
        .kernel_cr3_value = &x86_platform.kernel_cr3_value,
        .user_space_cr3_for_principal = userCr3ForPrincipal,
        .phys_copy_window_pt = &x86_platform.phys_copy_window_pt,
        .read_cr3 = readCr3,
        .write_cr3 = writeCr3,
        .invlpg = invlpg,
    });
    page_fault_log.init(.{
        .page_entries = boot_static.page_entries,
        .page_addr_mask = boot_static.page_addr_mask,
        .page_present = boot_static.page_present,
        .page_ps = boot_static.page_ps,
        .kernel_state_ready = &kernel_state_ready,
        .state = &kernel_state_global,
        .current_user_principal = &scheduler.current_user_principal,
        .write = kernel_log.write,
        .write_hex_raw = kernel_log.writeHexRaw,
        .write_bool01 = kernel_log.writeBool01,
    });

    kernel_state_global.initFromDetectedRegionsInPlace(memory_stats.detected_regions) catch |err| {
        halt.haltWithError("region init failed: ", err);
    };
    const state = &kernel_state_global;
    if (boot_static.iommu_no_cap_driver_mode != .off) {
        state.setIommuNoCapDriverMode(boot_static.iommu_no_cap_driver_mode);
        state.iommu_audit_hook = iommuAuditHook;
    } else {
        state.setIommuNoCapDriverMode(.off);
        state.iommu_audit_hook = null;
    }
    state.debug_alloc_page_hook = null;
    state.pte_sync_hook = null;

    return state;
}

// ---------------------------------------------------------------------------
// Group 4 — device discovery (virtio probe)
// ---------------------------------------------------------------------------

fn discoverDevices() DetectedDevices {
    var result: DetectedDevices = .{
        .devices = [_]?init_setup.DetectedDeviceBootstrap{null} ** boot_abi.init_bootstrap_abi.max_device_descriptors,
    };

    const probed = virtio_probe.probeModernDevices(noopLog);
    for (probed, 0..) |entry, index| {
        const info = entry orelse continue;
        if (index >= boot_abi.init_bootstrap_abi.max_device_descriptors) break;
        const dma_device = dmaDeviceForModernDevice(info) orelse continue;
        appendDetectedDevice(&result.devices, .{
            .descriptor = descriptorFromModernDevice(info, init_bootstrap_layout.deviceConfigSourceVa(index)),
            .dma_device = dma_device,
        });
    }

    return result;
}

// ---------------------------------------------------------------------------
// Group 3 — boot process construction
// ---------------------------------------------------------------------------

fn constructBootProcesses(state: *kernel.KernelState, res: BootResources, devs: *DetectedDevices) void {
    const init_principal = state.createProcessDescriptor("bootseed") orelse
        halt.haltWithMessage("bootseed process descriptor alloc failed");
    state.setBootstrapOwner(init_principal, true) catch |err| {
        halt.haltWithError("init bootstrap owner mark failed: ", err);
    };
    boot_init_principal = init_principal;
    const untyped_bootstrap = untyped_memory.bootstrapUntypedForOwner(state, &global_free_list, init_principal) catch |err| {
        halt.haltWithError("untyped bootstrap failed: ", err);
    };
    _ = untyped_bootstrap;
    const init_process = process_factory.createUserProcess(state, init_principal, "init", &global_free_list, user_spaces);
    init_setup.setupInitBootstrapResources(
        state,
        init_principal,
        devs.devices[0..],
        res.disk_bootfs_image,
        res.framebuffer_info,
        &global_free_list,
    );
    activateThreadOrHalt(init_process.thread_slot);

    scheduler.sanitizeAllThreadContextsWithSpaces(user_spaces, process_factory.buildInitialUserTrapFrame());

    scheduler.scheduler_tick_accum = 0;
    scheduler.scheduler_switch_count = 0;
    scheduler.scheduler_int80_log_count = 0;
    scheduler.scheduler_race_log_count = 0;
    scheduler.scheduler_probe_log_count = 0;
    boot_debug.logReadyTitle(bootDebugHooks(), "USER_PAGE_READY");
    init_setup.refreshInitBootLogSnapshot(state, init_principal);

    const loaded_init = elf_load.loadUserElfIntoProcessPagesOrHalt(
        state,
        init_principal,
        init_process.user_page.paddr,
        init_process.user_stack_page.paddr,
        res.disk_init_elf,
        "init ELF load failed\n",
        &global_free_list,
    );
    const init_thread = scheduler.threadSlotForPrincipal(init_principal).?;
    const init_ctx = scheduler.getThreadContext(init_thread).?;
    if (init_ctx.ready) {
        init_ctx.frame.rip = loaded_init.entry;
        init_ctx.frame.rsp = boot_static.user_entry_rsp;
    }

    state.pte_sync_hook = capability.syncPageTableRightsForPrincipalPaddr;
    kernel_state_ready = true;
}

// ---------------------------------------------------------------------------
// Group 2b — kernel subsystem wiring (post-process, after kernel_state_ready)
// ---------------------------------------------------------------------------

fn wireRuntimeSubsystems(state: *kernel.KernelState) void {
    spawn.init(state, &global_free_list, user_spaces, boot_init_principal);

    syscalls.init(.{
        .state = state,
        .free_list = &global_free_list,
        .kernel_state_ready = &kernel_state_ready,
        .enable_cap_table_dump_logs = false,
        .enable_switch_thread_syscall_log = boot_static.enable_switch_thread_syscall_log,
        .scheduler_log_int80 = boot_static.scheduler_log_int80,
        .scheduler_int80_log_max_lines = boot_static.scheduler_int80_log_max_lines,
        .write = kernel_log.write,
        .print_hex = log_util.printHex,
        .print_number = log_util.printNumberU64,
        .thread_label = threadLabel,
        .principal_label = principalLabel,
        .principal_from_process_slot = principalFromProcessSlot,
        .dump_all_process_caps = dumpAllProcessCaps,
        .read_user_u64 = user_copy.readUserU64,
        .write_user_u64 = user_copy.writeUserU64,
        .copy_user_bytes_from_va = user_copy.copyUserBytesFromVa,
        .launch_pie_user_thread = spawn.launchPieUserThread,
        .spawn_exec = spawn.spawnExecFromSyscall,
        .wake_waiting_thread_for_principal = scheduler.wakeWaitingThreadForPrincipal,
        .wake_blocked_thread_for_principal = scheduler.wakeBlockedThreadForPrincipal,
        .consume_pending_signal_for_principal = scheduler.consumePendingSignalForPrincipal,
        .switch_to_thread = scheduler.switchToThread,
        .block_current_thread_for_event = scheduler.blockCurrentThreadForEvent,
        .log_queue_cap_deny = logQueueCapDeny,
        .log_race_send_cap = logSchedulerRaceSendCap,
        .log_race_switch = logSchedulerRaceSwitch,
        .exit_current_process = exitCurrentProcess,
    });

    traps.init(.{
        .kernel_state_ready = &kernel_state_ready,
        .state = state,
        .scheduler_quantum_ticks = boot_static.scheduler_quantum_ticks,
        .priority_hold_quanta = 0,
        .scheduler_log_switch = boot_static.scheduler_log_switch,
        .scheduler_switch_log_max_lines = boot_static.scheduler_switch_log_max_lines,
        .write = kernel_log.write,
        .write_hex_raw = kernel_log.writeHexRaw,
        .write_bool01 = kernel_log.writeBool01,
        .thread_label = threadLabel,
        .principal_label = principalLabel,
        .read_cr2 = readCr2,
        .read_cr3 = readCr3,
        .dump_page_walk_for_va = page_fault_log.dumpPageWalkForVa,
        .log_page_fault_step2 = page_fault_log.logStep2,
        .halt_loop = halt.haltLoop,
        .resume_after_fatal_user_exception = resumeAfterFatalUserException,
        .switch_to_thread = scheduler.switchToThread,
        .log_race_switch = logSchedulerRaceSwitch,
    });
}

// ---------------------------------------------------------------------------
// kernelMain — boot sequence
// ---------------------------------------------------------------------------

pub fn kernelMain() void {
    asm volatile ("cli");
    lapic.maskLegacyPic();
    kernel_log.reset();
    serial.writeRaw("RAW ENTER MAIN\n");
    kernel_log.appendText("RAW ENTER MAIN\n");
    uefi_services.earlyUefiWrite(&[_:0]u16{ 'E', 'N', 'T', 'E', 'R', ' ', 'M', 'A', 'I', 'N', '\r', '\n' });
    kernel_log.appendText("ENTER MAIN\n");
    serial.init();
    boot_init_principal = null;

    const resources = runBootServicesPhase();
    const state = initKernelSubsystems(resources.memory_stats);
    var devices = discoverDevices();
    constructBootProcesses(state, resources, &devices);
    wireRuntimeSubsystems(state);

    const boot_ctx = scheduler.getThreadContextConst(scheduler.current_thread_index).?;
    enterUserModeIretq(boot_ctx.frame.rip, boot_ctx.frame.rsp);
}

// ---------------------------------------------------------------------------
// Helper: no-op probe log
// ---------------------------------------------------------------------------

fn noopLog(_: []const u8) void {}

// ---------------------------------------------------------------------------
// Helper: generic virtio device export
// ---------------------------------------------------------------------------

fn dmaDeviceForModernDevice(info: virtio_probe.ModernDeviceInfo) ?kernel.DmaDeviceId {
    if (virtio_probe.isVirtioInputDeviceId(info.device_id, info.subsystem_id)) return .virtio_input;
    if (virtio_probe.isVirtioGpuDeviceId(info.device_id, info.subsystem_id)) return .virtio_gpu;
    if (virtio_probe.isVirtioBlkDeviceId(info.device_id, info.subsystem_id)) return .virtio_blk;
    return null;
}

fn descriptorFromModernDevice(
    info: virtio_probe.ModernDeviceInfo,
    bootstrap_source_va: u64,
) boot_abi.init_bootstrap_abi.DeviceDescriptor {
    const common = mmioPageWithOffset(info.common_cfg);
    const notify = mmioPageWithOffset(info.notify_cfg);
    const isr = mmioPageWithOffset(info.isr_cfg);
    const device = mmioPageWithOffset(info.device_cfg);
    return .{
        .transport = @intFromEnum(boot_abi.init_bootstrap_abi.DeviceTransport.virtio_pci_modern),
        .flags = 0,
        .bootstrap_source_va = bootstrap_source_va,
        .vendor_id = info.vendor_id,
        .device_id = info.device_id,
        .subsystem_id = info.subsystem_id,
        .pci_bus = info.location.bus,
        .pci_device = info.location.device,
        .pci_function = info.location.function,
        .common_page_paddr = common.page_paddr,
        .notify_page_paddr = notify.page_paddr,
        .isr_page_paddr = isr.page_paddr,
        .device_page_paddr = device.page_paddr,
        .common_page_offset = common.page_offset,
        .notify_page_offset = notify.page_offset,
        .isr_page_offset = isr.page_offset,
        .device_page_offset = device.page_offset,
        .notify_off_multiplier = info.notify_off_multiplier,
        .init_queue_submit_token = 0,
        .init_queue_notify_token = 0,
    };
}

fn appendDetectedDevice(
    devices: *[boot_abi.init_bootstrap_abi.max_device_descriptors]?init_setup.DetectedDeviceBootstrap,
    detected: init_setup.DetectedDeviceBootstrap,
) void {
    for (devices) |*entry| {
        if (entry.* == null) {
            entry.* = detected;
            return;
        }
    }
    halt.haltWithMessage("init bootstrap device table full");
}
