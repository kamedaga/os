/// Kernel boot entry point and primary boot globals.
/// kernelMain() is called from main.zig after switching to the ring-0 stack.
const std = @import("std");
const kernel = @import("../kernel.zig");
const capability = @import("../capability.zig");
const device_capabilities = @import("../device_capabilities.zig");
const untyped_memory = @import("../untyped_memory.zig");
const elf_loader = @import("../elf_loader.zig");
const scheduler = @import("../scheduler.zig");
const syscalls = @import("../syscalls.zig");
const traps = @import("../traps.zig");
const interrupts = @import("../interrupts.zig");
const lapic = @import("../lapic.zig");
const smp = @import("../smp.zig");
const serial = @import("../serial.zig");
const kernel_log = @import("../kernel_log.zig");
const page_fault_log = @import("../page_fault_log.zig");
const user_programs = @import("../user_programs.zig");
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
const process_builder = @import("../runtime/process_builder.zig");
const abi_trap_runtime = @import("../runtime/abi_trap.zig");
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
    op: device_capabilities.QueueOperation,
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
    return x86_platform.cr3WithUserPcid(user_spaces[idx].cr3, @intCast(idx + 1));
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
    const thread_index = scheduler.currentThreadIndex();
    const ctx = scheduler.getThreadContext(thread_index) orelse return;
    if (!scheduler.isThreadReady(thread_index)) return;
    asm volatile ("fxsave64 (%[ptr])"
        :
        : [ptr] "r" (&ctx.fx_state),
        : .{ .memory = true });
}

pub export fn restoreCurrentThreadFxState() callconv(.c) void {
    if (!kernel_state_ready) return;
    const thread_index = scheduler.currentThreadIndex();
    const ctx = scheduler.getThreadContext(thread_index) orelse return;
    if (!scheduler.isThreadReady(thread_index)) return;
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
        if (!user_copy.mapKernelRuntimeStorage(x86_platform.mapKernelRuntimeIdentityRange)) {
            halt.haltWithMessage("user copy runtime mapping failed");
        }
        _ = x86_platform.enablePcidIfSupported();
        x86_platform.hardenKernelMappingsSupervisorOnly();
    }
    installInterruptTrampolines();
    x86_platform.installSyscallEntry(traps.syscallLstarEntryForCpu(0));
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
        .four_gib = boot_static.four_gib,
        .physical_map_limit = boot_static.physical_map_limit_exclusive,
        .user_va = boot_static.user_va,
        .user_stack_page_va = boot_static.user_stack_page_va,
        .page_entries = boot_static.page_entries,
        .page_present = boot_static.page_present,
        .page_rw = boot_static.page_rw,
        .page_user = boot_static.page_user,
        .flush_user_tlb_for_principal_va = user_copy.flushUserTlbForPrincipalVa,
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
        if (!scheduler.isThreadReady(thread_index)) continue;
        return thread_index;
    }
    return null;
}

fn loadRunnableThreadOrIdle(out_frame: *TrapFrame) void {
    while (true) {
        const current_thread = scheduler.currentThreadIndex();
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

    var endpoint_targets_removed = false;
    var storage_index: usize = 0;
    while (storage_index < kernel.principal_count) : (storage_index += 1) {
        var wake_owner = false;
        if (storage_index < kernel.process_count) {
            const removed = scrubEndpointTargets(&kernel_state_global.endpoint_tables[storage_index], principal);
            if (removed) endpoint_targets_removed = true;
            wake_owner = removed;
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
    if (endpoint_targets_removed) {
        kernel_state_global.bumpEndpointGeneration();
        scheduler.invalidateAllIpcFastpathState();
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

    var endpoint_targets_removed = false;
    var storage_index: usize = 0;
    while (storage_index < kernel.principal_count) : (storage_index += 1) {
        var wake_owner = false;
        if (storage_index < kernel.process_count) {
            const removed = scrubEndpointTargets(&kernel_state_global.endpoint_tables[storage_index], principal);
            if (removed) endpoint_targets_removed = true;
            wake_owner = removed;
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
    if (endpoint_targets_removed) {
        kernel_state_global.bumpEndpointGeneration();
        scheduler.invalidateAllIpcFastpathState();
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
    var smp_info = smp.prepareBootInfo(bs);

    const memory_stats = uefi_services.collectBootMemoryStatsOrHalt(bs, &global_free_list, user_spaces);
    uefi_services.exitBootServicesOrHalt();
    initKernelRuntimeOrHalt();
    scheduler.installCpuIdleObserver();
    smp.configureLstarEntries(traps.syscallLstarEntries());
    smp.configureApUserTimer(boot_static.lapic_timer_vector, boot_static.lapic_timer_initial_count);
    smp.startIdleAps(&smp_info, x86_platform.kernel_cr3_value);
    scheduler.refreshCpuTopology();
    logSchedulerCpuTopology();

    return .{
        .framebuffer_info = framebuffer_info,
        .disk_init_elf = disk_init_elf,
        .disk_bootfs_image = disk_bootfs_image,
        .memory_stats = memory_stats,
    };
}

fn logSchedulerCpuTopology() void {
    var cpu_slot: usize = 0;
    while (cpu_slot < scheduler.cpuCount()) : (cpu_slot += 1) {
        const info = scheduler.schedulerCpuInfo(cpu_slot) orelse continue;
        serial.writeRaw("SCHED CPU cpu=");
        serial.printNumber(cpu_slot);
        serial.writeRaw(" smp_state=");
        serial.printNumber(@intFromEnum(info.smp_state));
        serial.writeRaw(" current=");
        serial.printNumber(info.current_thread);
        serial.writeRaw(" idle_thread=");
        serial.printNumber(info.idle_thread);
        serial.writeRaw(" runnable=");
        serial.printNumber(info.runnable_count);
        serial.writeRaw(" enabled=");
        serial.printNumber(if (info.enabled) @as(u64, 1) else @as(u64, 0));
        serial.writeRaw(" accepts=");
        serial.printNumber(if (info.accepts_runnable) @as(u64, 1) else @as(u64, 0));
        serial.writeRaw(" accepts_req=");
        serial.printNumber(if (info.runnable_acceptance_requested) @as(u64, 1) else @as(u64, 0));
        serial.writeRaw(" idle=");
        serial.printNumber(if (info.is_idle) @as(u64, 1) else @as(u64, 0));
        serial.writeRaw(" obs=");
        serial.printNumber(info.observer_ticks);
        serial.writeRaw(" obs_run=");
        serial.printNumber(info.observed_runnable_ticks);
        serial.writeRaw(" handoff=");
        serial.printNumber(info.handoff_ticks);
        serial.writeRaw(" valid=");
        serial.printNumber(info.handoff_validation_ticks);
        serial.writeRaw(" consumed=");
        serial.printNumber(info.handoff_consume_ticks);
        serial.writeRaw(" snapshot=");
        serial.printNumber(info.handoff_snapshot_ticks);
        serial.writeRaw(" user_entry=");
        serial.printNumber(info.handoff_user_entry_ticks);
        serial.writeRaw(" ap_timer=");
        serial.printNumber(info.ap_timer_save_ticks);
        serial.writeRaw("\n");
    }
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
    if (scheduler.isThreadReady(init_thread)) {
        init_ctx.frame.rip = loaded_init.entry;
        init_ctx.frame.rsp = boot_static.user_entry_rsp;
    }
    if (!scheduler.spawnExecApPlacementExperimentEnabled()) {
        runSchedulerApObserverProbe(init_thread);
        runSchedulerApUserEntryProbe(state);
    }

    state.pte_sync_hook = capability.syncPageTableRightsForPrincipalPaddr;
    kernel_state_ready = true;
}

const SchedulerApUserThread = struct {
    principal: kernel.PrincipalId,
    thread_index: usize,
};

fn createSchedulerApUserEntryThread(state: *kernel.KernelState) ?SchedulerApUserThread {
    if (!scheduler.schedulerApQueueExperimentEnabled()) return null;
    const created = process_factory.tryCreateDynamicUserProcess(state, "apidle", &global_free_list, user_spaces) catch |err| {
        serial.writeRaw("SCHED APUSER create failed err=");
        serial.writeRaw(@errorName(err));
        serial.writeRaw("\n");
        return null;
    };
    user_programs.installIdleTaskCode(created.process.user_page.paddr);
    const ctx = scheduler.getThreadContext(created.process.thread_slot) orelse return null;
    ctx.frame.rip = boot_static.user_va;
    ctx.frame.rsp = boot_static.user_entry_rsp;
    return .{
        .principal = created.principal,
        .thread_index = created.process.thread_slot,
    };
}

const SchedulerApUserProbe = struct {
    active: bool = false,
    cpu_slot: usize = 0,
    principal: ?kernel.PrincipalId = null,
    next_principal: ?kernel.PrincipalId = null,
    thread_index: usize = scheduler.idle_thread_marker,
    next_thread_index: usize = scheduler.idle_thread_marker,
    before_entry_ticks: u64 = 0,
    before_timer_ticks: u64 = 0,
    before_requeue_ticks: u64 = 0,
    before_preempt_ticks: u64 = 0,
    enable_ok: bool = false,
    cycle_ok: bool = false,
    arm_ok: bool = false,
    move_ok: bool = false,
    move_next_ok: bool = false,
    disable_ok: bool = false,
    observed: bool = false,
    consumed: bool = false,
    snapshotted: bool = false,
    preempted: bool = false,
    entered: bool = false,
    timer_saved: bool = false,
    requeued: bool = false,
    returned_idle: bool = false,
    entered_thread: ?usize = null,
    timer_saved_thread: ?usize = null,
    timer_saved_rip: u64 = 0,
    timer_saved_rsp: u64 = 0,
    entry_count: u64 = 0,
    timer_count: u64 = 0,
    requeue_count: u64 = 0,
    preempt_count: u64 = 0,
    preempt_from_thread: ?usize = null,
    preempt_to_thread: ?usize = null,
    cpu_state: smp.CpuState = .absent,
    target_current_thread: usize = scheduler.idle_thread_marker,
    target_idle: bool = true,
};

fn runSchedulerApUserEntryProbe(state: *kernel.KernelState) void {
    if (!scheduler.schedulerApQueueExperimentEnabled()) return;
    if (scheduler.cpuCount() <= 1) {
        serial.writeRaw("SCHED APUSER probe skipped no_ap\n");
        return;
    }

    const policy_enable_ok = scheduler.setApRunnablePolicyForExperiment(true);
    var probes = [_]SchedulerApUserProbe{.{}} ** smp.max_cpus;
    var probe_count: usize = 0;
    var cpu_slot: usize = 1;
    while (cpu_slot < scheduler.cpuCount() and cpu_slot < probes.len) : (cpu_slot += 1) {
        const candidate_info = scheduler.schedulerCpuInfo(cpu_slot) orelse continue;
        if (!candidate_info.enabled or candidate_info.smp_state == .absent) continue;
        const created = createSchedulerApUserEntryThread(state) orelse {
            serial.writeRaw("SCHED APUSER probe skipped no_thread cpu=");
            serial.printNumber(cpu_slot);
            serial.writeRaw("\n");
            continue;
        };
        const next_created = createSchedulerApUserEntryThread(state) orelse {
            serial.writeRaw("SCHED APUSER probe skipped no_next_thread cpu=");
            serial.printNumber(cpu_slot);
            serial.writeRaw("\n");
            teardownExitedProcess(created.principal);
            continue;
        };

        const chosen_cpu = if (policy_enable_ok) scheduler.assignThreadPairToChosenCpu(created.thread_index, next_created.thread_index, false) else null;
        const selected_cpu = chosen_cpu orelse {
            serial.writeRaw("SCHED APUSER probe skipped no_chosen_cpu seed_cpu=");
            serial.printNumber(cpu_slot);
            serial.writeRaw("\n");
            teardownExitedProcess(next_created.principal);
            teardownExitedProcess(created.principal);
            continue;
        };
        const info = scheduler.schedulerCpuInfo(selected_cpu) orelse {
            teardownExitedProcess(next_created.principal);
            teardownExitedProcess(created.principal);
            continue;
        };

        var probe = SchedulerApUserProbe{
            .active = true,
            .cpu_slot = selected_cpu,
            .principal = created.principal,
            .next_principal = next_created.principal,
            .thread_index = created.thread_index,
            .next_thread_index = next_created.thread_index,
            .before_entry_ticks = info.handoff_user_entry_ticks,
            .before_timer_ticks = info.ap_timer_save_ticks,
            .before_requeue_ticks = info.ap_timer_requeue_ticks,
            .before_preempt_ticks = info.ap_preempt_switch_ticks,
            .cpu_state = info.smp_state,
            .target_current_thread = info.current_thread,
            .target_idle = info.is_idle,
        };
        probe.enable_ok = policy_enable_ok;
        probe.cycle_ok = probe.enable_ok and scheduler.setCpuProbeUserEntryTargetCyclesForExperiment(selected_cpu, probe.before_timer_ticks +% 2);
        probe.move_ok = scheduler.threadCpuSlot(created.thread_index) == selected_cpu;
        probe.move_next_ok = scheduler.threadCpuSlot(next_created.thread_index) == selected_cpu;
        probe.arm_ok = probe.cycle_ok and probe.move_ok and probe.move_next_ok and scheduler.setCpuUserEntryForExperiment(selected_cpu, true);
        probes[probe_count] = probe;
        probe_count += 1;
    }

    if (probe_count == 0) {
        _ = scheduler.setApRunnablePolicyForExperiment(false);
        serial.writeRaw("SCHED APUSER probe skipped no_targets\n");
        return;
    }

    var spins: usize = 0;
    while (spins < 12_000_000) : (spins += 1) {
        var all_done = true;
        var index: usize = 0;
        while (index < probe_count) : (index += 1) {
            updateSchedulerApUserProbe(&probes[index]);
            if (probes[index].arm_ok and !probes[index].returned_idle) all_done = false;
        }
        if (all_done) break;
        asm volatile ("pause");
    }

    var ready_count: usize = 0;
    const policy_disable_ok = scheduler.setApRunnablePolicyForExperiment(false);
    var index: usize = 0;
    while (index < probe_count) : (index += 1) {
        probes[index].disable_ok = policy_disable_ok;
        if (probes[index].returned_idle) ready_count += 1;
        logSchedulerApUserProbe(&probes[index]);
        if (probes[index].next_principal) |principal| teardownExitedProcess(principal);
        if (probes[index].principal) |principal| teardownExitedProcess(principal);
    }
    serial.writeRaw("SCHED APUSER all aps=");
    serial.printNumber(probe_count);
    serial.writeRaw(" ready=");
    serial.printNumber(ready_count);
    serial.writeRaw("\n");
}

fn updateSchedulerApUserProbe(probe: *SchedulerApUserProbe) void {
    if (!probe.active or !probe.move_ok) return;
    const info = scheduler.schedulerCpuInfo(probe.cpu_slot) orelse return;
    probe.cpu_state = info.smp_state;
    probe.target_current_thread = info.current_thread;
    probe.target_idle = info.is_idle;
    probe.entered_thread = info.entered_handoff_thread;
    probe.timer_saved_thread = info.ap_timer_saved_thread;
    probe.timer_saved_rip = info.ap_timer_saved_rip;
    probe.timer_saved_rsp = info.ap_timer_saved_rsp;
    probe.entry_count = info.handoff_user_entry_ticks -% probe.before_entry_ticks;
    probe.timer_count = info.ap_timer_save_ticks -% probe.before_timer_ticks;
    probe.requeue_count = info.ap_timer_requeue_ticks -% probe.before_requeue_ticks;
    probe.preempt_count = info.ap_preempt_switch_ticks -% probe.before_preempt_ticks;
    probe.preempt_from_thread = info.ap_preempt_from_thread;
    probe.preempt_to_thread = info.ap_preempt_to_thread;
    if (info.observed_next_thread == probe.thread_index or info.validated_handoff_thread == probe.thread_index or info.consumed_handoff_thread == probe.thread_index) {
        probe.observed = true;
    }
    if (info.consumed_handoff_thread == probe.thread_index and info.current_thread == probe.thread_index and !info.is_idle) {
        probe.consumed = true;
    }
    if (info.snapshot_handoff_thread == probe.thread_index and info.snapshot_cr3 != 0 and info.snapshot_rip != 0 and info.snapshot_rsp != 0) {
        probe.snapshotted = true;
    }
    if (probe.preempt_count != 0 and info.ap_preempt_from_thread == probe.thread_index and info.ap_preempt_to_thread == probe.next_thread_index) {
        probe.preempted = true;
    }
    if (probe.entry_count >= 2 and info.ap_timer_saved_thread == probe.next_thread_index) {
        probe.entered = true;
    }
    if (probe.timer_count >= 2 and info.ap_timer_saved_thread == probe.next_thread_index and info.ap_timer_saved_rip != 0 and info.ap_timer_saved_rsp != 0) {
        probe.timer_saved = true;
    }
    if (probe.requeue_count != 0 and probe.entry_count >= 2) {
        probe.requeued = true;
    }
    if (probe.timer_saved and info.smp_state == .idle and info.current_thread == scheduler.idle_thread_marker and info.is_idle) {
        probe.returned_idle = true;
    }
}

fn logSchedulerApUserProbe(probe: *const SchedulerApUserProbe) void {
    serial.writeRaw("SCHED APUSER probe cpu=");
    serial.printNumber(probe.cpu_slot);
    serial.writeRaw(" thread=");
    serial.printNumber(probe.thread_index);
    serial.writeRaw(" next_thread=");
    serial.printNumber(probe.next_thread_index);
    serial.writeRaw(" enable=");
    serial.printNumber(if (probe.enable_ok) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" cycle=");
    serial.printNumber(if (probe.cycle_ok) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" arm=");
    serial.printNumber(if (probe.arm_ok) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" move=");
    serial.printNumber(if (probe.move_ok) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" move_next=");
    serial.printNumber(if (probe.move_next_ok) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" observed=");
    serial.printNumber(if (probe.observed) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" consumed=");
    serial.printNumber(if (probe.consumed) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" snapshot=");
    serial.printNumber(if (probe.snapshotted) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" preempted=");
    serial.printNumber(if (probe.preempted) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" entered=");
    serial.printNumber(if (probe.entered) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" timer_saved=");
    serial.printNumber(if (probe.timer_saved) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" requeued=");
    serial.printNumber(if (probe.requeued) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" returned_idle=");
    serial.printNumber(if (probe.returned_idle) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" entries=");
    serial.printNumber(probe.entry_count);
    serial.writeRaw(" timers=");
    serial.printNumber(probe.timer_count);
    serial.writeRaw(" requeues=");
    serial.printNumber(probe.requeue_count);
    serial.writeRaw(" preempts=");
    serial.printNumber(probe.preempt_count);
    serial.writeRaw(" preempt_from=");
    if (probe.preempt_from_thread) |thread| {
        serial.printNumber(thread);
    } else {
        serial.writeRaw("none");
    }
    serial.writeRaw(" preempt_to=");
    if (probe.preempt_to_thread) |thread| {
        serial.printNumber(thread);
    } else {
        serial.writeRaw("none");
    }
    serial.writeRaw(" entered_thread=");
    if (probe.entered_thread) |thread| {
        serial.printNumber(thread);
    } else {
        serial.writeRaw("none");
    }
    serial.writeRaw(" smp_state=");
    serial.printNumber(@intFromEnum(probe.cpu_state));
    serial.writeRaw(" timer_thread=");
    if (probe.timer_saved_thread) |thread| {
        serial.printNumber(thread);
    } else {
        serial.writeRaw("none");
    }
    serial.writeRaw(" timer_rip=");
    serial.writeHexRaw(probe.timer_saved_rip);
    serial.writeRaw(" timer_rsp=");
    serial.writeHexRaw(probe.timer_saved_rsp);
    serial.writeRaw(" target_current=");
    serial.printNumber(probe.target_current_thread);
    serial.writeRaw(" target_idle=");
    serial.printNumber(if (probe.target_idle) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" disable=");
    serial.printNumber(if (probe.disable_ok) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw("\n");
}

fn runSchedulerApObserverProbe(thread_index: usize) void {
    if (!scheduler.schedulerApQueueExperimentEnabled()) return;
    if (scheduler.cpuCount() <= 1) {
        serial.writeRaw("SCHED APHAND probe skipped no_ap\n");
        return;
    }

    const target_cpu: usize = 1;
    const original_cpu = scheduler.threadCpuSlot(thread_index) orelse 0;
    const before_ticks = blk: {
        const info = scheduler.schedulerCpuInfo(target_cpu) orelse break :blk 0;
        break :blk info.observed_runnable_ticks;
    };
    const before_handoff_ticks = blk: {
        const info = scheduler.schedulerCpuInfo(target_cpu) orelse break :blk 0;
        break :blk info.handoff_ticks;
    };
    const before_validation_ticks = blk: {
        const info = scheduler.schedulerCpuInfo(target_cpu) orelse break :blk 0;
        break :blk info.handoff_validation_ticks;
    };
    const before_consume_ticks = blk: {
        const info = scheduler.schedulerCpuInfo(target_cpu) orelse break :blk 0;
        break :blk info.handoff_consume_ticks;
    };
    const before_snapshot_ticks = blk: {
        const info = scheduler.schedulerCpuInfo(target_cpu) orelse break :blk 0;
        break :blk info.handoff_snapshot_ticks;
    };

    const enable_ok = scheduler.setApRunnablePolicyForExperiment(true);
    const move_ok = enable_ok and scheduler.assignThreadToCpu(thread_index, target_cpu);

    var observed = false;
    var handed_off = false;
    var validated = false;
    var consumed = false;
    var snapshotted = false;
    var observed_next: ?usize = null;
    var observed_count: usize = 0;
    var handoff_thread: ?usize = null;
    var handoff_count: usize = 0;
    var validated_thread: ?usize = null;
    var validation_code: u8 = scheduler.handoff_validation_none;
    var consumed_thread: ?usize = null;
    var snapshot_thread: ?usize = null;
    var snapshot_cr3: u64 = 0;
    var snapshot_fs_base: u64 = 0;
    var snapshot_rip: u64 = 0;
    var snapshot_rsp: u64 = 0;
    var snapshot_rflags: u64 = 0;
    var snapshot_cs: u64 = 0;
    var snapshot_ss: u64 = 0;
    var target_current_thread: usize = scheduler.idle_thread_marker;
    var target_idle = true;
    if (move_ok) {
        var spins: usize = 0;
        while (spins < 5_000_000) : (spins += 1) {
            if (scheduler.schedulerCpuInfo(target_cpu)) |info| {
                observed_next = info.observed_next_thread;
                observed_count = info.observed_runnable_count;
                handoff_thread = info.pending_handoff_thread;
                handoff_count = info.handoff_runnable_count;
                validated_thread = info.validated_handoff_thread;
                validation_code = info.handoff_validation_code;
                consumed_thread = info.consumed_handoff_thread;
                snapshot_thread = info.snapshot_handoff_thread;
                snapshot_cr3 = info.snapshot_cr3;
                snapshot_fs_base = info.snapshot_fs_base;
                snapshot_rip = info.snapshot_rip;
                snapshot_rsp = info.snapshot_rsp;
                snapshot_rflags = info.snapshot_rflags;
                snapshot_cs = info.snapshot_cs;
                snapshot_ss = info.snapshot_ss;
                target_current_thread = info.current_thread;
                target_idle = info.is_idle;
                if (info.observed_runnable_ticks != before_ticks and
                    (info.observed_next_thread == thread_index or info.validated_handoff_thread == thread_index or info.consumed_handoff_thread == thread_index))
                {
                    observed = true;
                }
                if (info.handoff_ticks != before_handoff_ticks and
                    (info.pending_handoff_thread == thread_index or info.validated_handoff_thread == thread_index or info.consumed_handoff_thread == thread_index))
                {
                    handed_off = true;
                }
                if (info.handoff_validation_ticks != before_validation_ticks and info.validated_handoff_thread == thread_index and info.handoff_validation_code == scheduler.handoff_validation_ok) {
                    validated = true;
                }
                if (info.handoff_consume_ticks != before_consume_ticks and info.consumed_handoff_thread == thread_index and info.current_thread == thread_index and !info.is_idle) {
                    consumed = true;
                }
                if (info.handoff_snapshot_ticks != before_snapshot_ticks and
                    info.snapshot_handoff_thread == thread_index and
                    info.snapshot_cr3 != 0 and
                    info.snapshot_rip != 0 and
                    info.snapshot_rsp != 0)
                {
                    snapshotted = true;
                }
                if (observed and handed_off and validated and consumed and snapshotted) {
                    break;
                }
            }
            asm volatile ("pause");
        }
    }

    const restore_ok = scheduler.assignThreadToCpu(thread_index, original_cpu);
    const disable_ok = scheduler.setApRunnablePolicyForExperiment(false);

    serial.writeRaw("SCHED APHAND probe cpu=");
    serial.printNumber(target_cpu);
    serial.writeRaw(" thread=");
    serial.printNumber(thread_index);
    serial.writeRaw(" enable=");
    serial.printNumber(if (enable_ok) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" move=");
    serial.printNumber(if (move_ok) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" observed=");
    serial.printNumber(if (observed) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" handoff=");
    serial.printNumber(if (handed_off) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" valid=");
    serial.printNumber(if (validated) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" consumed=");
    serial.printNumber(if (consumed) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" snapshot=");
    serial.printNumber(if (snapshotted) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" next=");
    if (observed_next) |thread| {
        serial.printNumber(thread);
    } else {
        serial.writeRaw("none");
    }
    serial.writeRaw(" runnable=");
    serial.printNumber(observed_count);
    serial.writeRaw(" hand_thread=");
    if (handoff_thread) |thread| {
        serial.printNumber(thread);
    } else {
        serial.writeRaw("none");
    }
    serial.writeRaw(" hand_runnable=");
    serial.printNumber(handoff_count);
    serial.writeRaw(" valid_thread=");
    if (validated_thread) |thread| {
        serial.printNumber(thread);
    } else {
        serial.writeRaw("none");
    }
    serial.writeRaw(" valid_code=");
    serial.printNumber(validation_code);
    serial.writeRaw(" consumed_thread=");
    if (consumed_thread) |thread| {
        serial.printNumber(thread);
    } else {
        serial.writeRaw("none");
    }
    serial.writeRaw(" snap_thread=");
    if (snapshot_thread) |thread| {
        serial.printNumber(thread);
    } else {
        serial.writeRaw("none");
    }
    serial.writeRaw(" snap_cr3=");
    serial.writeHexRaw(snapshot_cr3);
    serial.writeRaw(" snap_fs=");
    serial.writeHexRaw(snapshot_fs_base);
    serial.writeRaw(" snap_rip=");
    serial.writeHexRaw(snapshot_rip);
    serial.writeRaw(" snap_rsp=");
    serial.writeHexRaw(snapshot_rsp);
    serial.writeRaw(" snap_rflags=");
    serial.writeHexRaw(snapshot_rflags);
    serial.writeRaw(" snap_cs=");
    serial.writeHexRaw(snapshot_cs);
    serial.writeRaw(" snap_ss=");
    serial.writeHexRaw(snapshot_ss);
    serial.writeRaw(" target_current=");
    serial.printNumber(target_current_thread);
    serial.writeRaw(" target_idle=");
    serial.printNumber(if (target_idle) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" restore=");
    serial.printNumber(if (restore_ok) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" disable=");
    serial.printNumber(if (disable_ok) @as(u64, 1) else @as(u64, 0));
    serial.writeRaw(" cpu=");
    serial.printNumber(scheduler.threadCpuSlot(thread_index) orelse scheduler.idle_thread_marker);
    serial.writeRaw("\n");
}

// ---------------------------------------------------------------------------
// Group 2b — kernel subsystem wiring (post-process, after kernel_state_ready)
// ---------------------------------------------------------------------------

fn wireRuntimeSubsystems(state: *kernel.KernelState, memory_stats: boot_static.MemoryStats) void {
    spawn.init(state, &global_free_list, user_spaces, boot_init_principal);
    process_builder.init(state, &global_free_list, user_spaces);
    abi_trap_runtime.init(.{
        .state = state,
        .free_list = &global_free_list,
        .user_spaces = user_spaces,
        .write = kernel_log.write,
        .principal_label = principalLabel,
        .write_user_u64 = user_copy.writeUserU64,
        .copy_user_bytes_from_va = user_copy.copyUserBytesFromVa,
        .copy_bytes_to_user_va = user_copy.copyBytesToUserVa,
        .consume_pending_signal_for_principal = scheduler.consumePendingSignalForPrincipal,
        .block_current_thread_for_event = scheduler.blockCurrentThreadForEvent,
        .exit_current_process = exitCurrentProcess,
    });

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
        .copy_bytes_to_user_va = user_copy.copyBytesToUserVa,
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
        .total_usable_memory_bytes = memory_stats.total_usable_bytes,
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
    wireRuntimeSubsystems(state, resources.memory_stats);

    const boot_ctx = scheduler.getThreadContextConst(scheduler.currentThreadIndex()).?;
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
        .init_iommu_token = 0,
        .init_queue_grant_count = 0,
        .init_queue_grants = [_]boot_abi.init_bootstrap_abi.DeviceQueueGrant{.{}} ** boot_abi.init_bootstrap_abi.max_device_queue_grants,
        .init_command_token = 0,
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
