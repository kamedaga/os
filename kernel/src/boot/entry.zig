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
    device: kernel.DmaDeviceId,
    queue_index: u16,
    op: kernel.QueueOperation,
    err: anyerror,
) void {
    boot_debug.logQueueCapDeny(bootDebugHooks(), proc, token, device, queue_index, op, err);
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

// ---------------------------------------------------------------------------
// Process setup helpers
// ---------------------------------------------------------------------------

fn setupBootLogConsoleProcess(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    keyboard_cfg: ?init_setup.MouseDriverConfig,
) struct {
    process: process_factory.CreatedUserProcess,
    boot_log_page: kernel.PageCapability,
    boot_log_stack_page: kernel.PageCapability,
} {
    const process = process_factory.createUserProcess(state, principal, "boot_display", &global_free_list, user_spaces);
    const boot_log_page = process_factory.allocPageForProcessOrHalt(state, principal, "boot log", "page", &global_free_list);
    const boot_log_stack_page = process_factory.allocPageForProcessOrHalt(state, principal, "boot_display", "stack page", &global_free_list);
    const keyboard_config_page = process_factory.allocPageForProcessOrHalt(state, principal, "boot_display", "keyboard config page", &global_free_list);
    process_factory.mapUserLinearRegionOrHalt(principal, boot_abi.process_abi.standard_config_target_va, keyboard_config_page.paddr, 4096, true, "shell keyboard config page map failed");

    const mmio_rw_rights = kernel.Rights{ .cpu_read = true, .cpu_write = true, .dma = false, .grant = true };
    const mmio_ro_rights = kernel.Rights{ .cpu_read = true, .cpu_write = false, .dma = false, .grant = true };
    var keyboard_submit_token: u64 = 0;
    var keyboard_notify_token: u64 = 0;
    if (keyboard_cfg) |cfg| {
        state.installCap(principal, cfg.common.page_paddr, mmio_rw_rights) catch |err| halt.haltWithError("shell install keyboard common cap failed: ", err);
        state.installCap(principal, cfg.notify.page_paddr, mmio_rw_rights) catch |err| halt.haltWithError("shell install keyboard notify cap failed: ", err);
        if (cfg.isr.page_paddr != 0) state.installCap(principal, cfg.isr.page_paddr, mmio_ro_rights) catch |err| halt.haltWithError("shell install keyboard isr cap failed: ", err);
        if (cfg.device.page_paddr != 0) state.installCap(principal, cfg.device.page_paddr, mmio_ro_rights) catch |err| halt.haltWithError("shell install keyboard device cap failed: ", err);
        keyboard_submit_token = state.queueCapGrantStage2(principal, .virtio_input, 0, true, false) catch |err| halt.haltWithError("shell keyboard queue submit cap grant failed: ", err);
        keyboard_notify_token = state.queueCapGrantStage2(principal, .virtio_input, 0, false, true) catch |err| halt.haltWithError("shell keyboard queue notify cap grant failed: ", err);
    }
    init_setup.publishShellKeyboardConfigPage(keyboard_config_page.paddr, keyboard_cfg, keyboard_submit_token, keyboard_notify_token);
    return .{
        .process = process,
        .boot_log_page = boot_log_page,
        .boot_log_stack_page = boot_log_stack_page,
    };
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
    disk_boot_log_console_elf: []const u8,
    disk_init_elf: []const u8,
    disk_bootfs_image: []const u8,
    memory_stats: boot_static.MemoryStats,
};

const DetectedDevices = struct {
    input: [boot_abi.init_bootstrap_abi.max_input_device_descriptors]?init_setup.DetectedInputBootstrap,
    block: ?init_setup.DetectedBlockBootstrap,
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
    const disk_boot_log_console_elf = uefi_services.loadBootDiskFile(bs, boot_images.boot_log_console) orelse {
        halt.haltWithMessage("disk shell ELF load failed");
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
        .disk_boot_log_console_elf = disk_boot_log_console_elf,
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

    const untyped_bootstrap = untyped_memory.bootstrapUntypedForOwner(state, &global_free_list, .Process0) catch |err| {
        halt.haltWithError("untyped bootstrap failed: ", err);
    };
    _ = untyped_bootstrap;

    return state;
}

// ---------------------------------------------------------------------------
// Group 4 — device discovery (virtio probe)
// ---------------------------------------------------------------------------

fn discoverDevices() DetectedDevices {
    var result: DetectedDevices = .{
        .input = [_]?init_setup.DetectedInputBootstrap{null} ** boot_abi.init_bootstrap_abi.max_input_device_descriptors,
        .block = null,
    };

    if (virtio_probe.probeMouseModern(noopLog)) |info| {
        appendDetectedInput(&result.input, .pointer, .{
            .common = mmioPageWithOffset(info.common_cfg),
            .notify = mmioPageWithOffset(info.notify_cfg),
            .isr = mmioPageWithOffset(info.isr_cfg),
            .device = mmioPageWithOffset(info.device_cfg),
            .notify_off_multiplier = info.notify_off_multiplier,
        });
    }
    if (virtio_probe.probeKeyboardModern(noopLog)) |info| {
        appendDetectedInput(&result.input, .keyboard, .{
            .common = mmioPageWithOffset(info.common_cfg),
            .notify = mmioPageWithOffset(info.notify_cfg),
            .isr = mmioPageWithOffset(info.isr_cfg),
            .device = mmioPageWithOffset(info.device_cfg),
            .notify_off_multiplier = info.notify_off_multiplier,
        });
    }
    if (virtio_probe.probeBlkModern(noopLog)) |info| {
        result.block = .{
            .descriptor = blockDeviceDescriptorForKind(.virtio_blk),
            .config = .{
                .common = mmioPageWithOffset(info.common_cfg),
                .notify = mmioPageWithOffset(info.notify_cfg),
                .isr = mmioPageWithOffset(info.isr_cfg),
                .device = mmioPageWithOffset(info.device_cfg),
                .notify_off_multiplier = info.notify_off_multiplier,
                .capacity_sectors = info.capacity_sectors,
                .logical_block_size = info.logical_block_size,
            },
        };
    }

    return result;
}

// ---------------------------------------------------------------------------
// Group 3 — boot process construction
// ---------------------------------------------------------------------------

fn constructBootProcesses(state: *kernel.KernelState, res: BootResources, devs: *DetectedDevices) void {
    const display_principal = state.createProcessDescriptor("boot_display") orelse
        halt.haltWithMessage("boot_display process descriptor alloc failed");

    const keyboard_cfg = blk: {
        for (devs.input) |entry| {
            if (entry) |device| {
                if (device.descriptor.kind == @intFromEnum(boot_abi.init_bootstrap_abi.InputDeviceKind.keyboard)) {
                    break :blk device.config;
                }
            }
        }
        break :blk null;
    };

    const shell_setup = setupBootLogConsoleProcess(state, display_principal, keyboard_cfg);
    activateThreadOrHalt(shell_setup.process.thread_slot);

    const init_principal = state.createProcessDescriptor("init") orelse
        halt.haltWithMessage("init process descriptor alloc failed");
    boot_init_principal = init_principal;
    const init_process = process_factory.createUserProcess(state, init_principal, "init", &global_free_list, user_spaces);
    init_setup.setupInitBootstrapResources(
        state,
        init_principal,
        display_principal,
        devs.input[0..],
        &devs.block,
        res.disk_bootfs_image,
        res.framebuffer_info,
        &global_free_list,
    );
    activateThreadOrHalt(init_process.thread_slot);

    const boot_display_untyped_grants = untyped_memory.grantUntypedFromOwnerTo(state, .Process0, display_principal) catch |err| {
        halt.haltWithError("untyped grant to boot display failed: ", err);
    };
    _ = boot_display_untyped_grants;

    scheduler.sanitizeAllThreadContextsWithSpaces(user_spaces, process_factory.buildInitialUserTrapFrame());

    scheduler.scheduler_tick_accum = 0;
    scheduler.scheduler_switch_count = 0;
    scheduler.scheduler_int80_log_count = 0;
    scheduler.scheduler_race_log_count = 0;
    scheduler.scheduler_probe_log_count = 0;
    boot_debug.logReadyTitle(bootDebugHooks(), "USER_PAGE_READY");

    process_factory.mapUserLinearRegionOrHalt(display_principal, boot_abi.process_abi.auxPageVa(5), res.framebuffer_info.paddr, res.framebuffer_info.size_bytes, true, "framebuffer user mapping failed");
    boot_debug.logReadyTitle(bootDebugHooks(), "FRAMEBUFFER_SERVER_READY");

    process_factory.mapUserLinearRegionOrHalt(
        display_principal,
        boot_static.boot_log_console_stack_page_va,
        shell_setup.boot_log_stack_page.paddr,
        4096,
        true,
        "shell stack page map failed",
    );
    process_factory.mapUserLinearRegionOrHalt(
        display_principal,
        boot_static.boot_log_user_va,
        shell_setup.boot_log_page.paddr,
        4096,
        false,
        "boot log page map failed",
    );

    const loaded_shell = elf_load.loadUserElfIntoProcessPagesOrHalt(
        state,
        display_principal,
        shell_setup.process.user_page.paddr,
        shell_setup.process.user_stack_page.paddr,
        res.disk_boot_log_console_elf,
        "shell ELF load failed\n",
        &global_free_list,
    );
    const shell_thread = scheduler.threadSlotForPrincipal(display_principal).?;
    const shell_ctx = scheduler.getThreadContext(shell_thread).?;
    shell_ctx.frame.rip = loaded_shell.entry;
    shell_ctx.frame.rsp = boot_static.boot_log_console_entry_rsp;

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
    serial.writeRaw("RAW ENTER MAIN\n");
    uefi_services.earlyUefiWrite(&[_:0]u16{ 'E', 'N', 'T', 'E', 'R', ' ', 'M', 'A', 'I', 'N', '\r', '\n' });
    serial.init();
    kernel_log.reset();
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
// Helper: virtio device descriptor lookup
// ---------------------------------------------------------------------------

fn blockDeviceDescriptorForKind(kind: boot_abi.init_bootstrap_abi.BlockDeviceKind) boot_abi.init_bootstrap_abi.BlockDeviceDescriptor {
    inline for (init_bootstrap_layout.builtin_block_devices) |descriptor| {
        if (descriptor.kind == @intFromEnum(kind)) return descriptor;
    }
    unreachable;
}

fn appendDetectedInput(
    devices: *[boot_abi.init_bootstrap_abi.max_input_device_descriptors]?init_setup.DetectedInputBootstrap,
    kind: boot_abi.init_bootstrap_abi.InputDeviceKind,
    config: init_setup.MouseDriverConfig,
) void {
    for (devices) |*entry| {
        if (entry.* == null) {
            entry.* = .{
                .descriptor = inputDeviceDescriptorForKind(kind),
                .config = config,
            };
            return;
        }
    }
    halt.haltWithMessage("init bootstrap input device table full");
}

fn inputDeviceDescriptorForKind(kind: boot_abi.init_bootstrap_abi.InputDeviceKind) boot_abi.init_bootstrap_abi.InputDeviceDescriptor {
    inline for (init_bootstrap_layout.builtin_input_devices) |descriptor| {
        if (descriptor.kind == @intFromEnum(kind)) return descriptor;
    }
    unreachable;
}
