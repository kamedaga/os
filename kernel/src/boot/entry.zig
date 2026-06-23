/// Kernel boot entry point and primary boot globals.
/// kernelMain() is called from main.zig after switching to the ring-0 stack.
const std = @import("std");
const kernel = @import("../kernel.zig");
const elf_loader = @import("../elf_loader.zig");
const scheduler = @import("../scheduler.zig").connection;
const syscalls = @import("../syscalls.zig");
const traps = @import("../traps.zig");
const interrupts = @import("../interrupts.zig");
const lapic = @import("../lapic.zig");
const smp = @import("../smp.zig");
const serial = @import("../serial.zig");
const kernel_log = @import("../kernel_log.zig");
const page_fault_log = @import("../page_fault_log.zig");
const user_copy = @import("../user_copy.zig");
const pci = @import("../pci.zig");
const kernel_vm = @import("../memory/kernel_vm.zig");
const pmm = @import("../memory/pmm.zig");
const user_vm = @import("../memory/user_vm.zig");
const vtd = @import("../vtd.zig");
const x86_platform = @import("../arch/x86_64/platform.zig");
const boot_static = @import("main_static.zig");
const boot_images = @import("boot_images.zig");
const boot_abi = @import("abi.zig");
const init_bootstrap_layout = @import("init_bootstrap_layout.zig");
const uefi_services = @import("uefi_services.zig");
const process_factory = @import("process_factory.zig");
const elf_load = @import("elf_load.zig");
const init_setup = @import("init_setup.zig");
const halt = @import("../halt.zig");
const log_util = @import("../log_util.zig");

const TrapFrame = interrupts.TrapFrame;
const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;

// ---------------------------------------------------------------------------
// Boot globals
// ---------------------------------------------------------------------------

var empty_user_spaces_storage: [0]boot_static.UserAddressSpace align(4096) = .{};
var user_spaces: []boot_static.UserAddressSpace = empty_user_spaces_storage[0..];
var empty_kernel_runtime_storage: [0]u8 align(4096) = .{};
var kernel_runtime_storage: []align(4096) u8 = empty_kernel_runtime_storage[0..];

pub var global_free_list: kernel.FreePageList = .{};
pub var kernel_state_global: kernel.KernelState = undefined;
pub var kernel_state_ready: bool = false;

var boot_init_principal: ?kernel.PrincipalId = null;
const spawn_parent_endpoint_id: u64 = 0x14;
const generic_device_interrupt_vector: u8 = 0x41;
const device_interrupt_vector_count: u8 = 1;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn staticStorageStart(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

fn minStaticStart(a: usize, b: usize) usize {
    return if (a < b) a else b;
}

fn kernelStaticStorageStartAddr() usize {
    var start = uefi_services.kernelStaticStorageStartAddr();
    start = minStaticStart(start, staticStorageStart(@TypeOf(user_spaces), &user_spaces));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_runtime_storage), &kernel_runtime_storage));
    start = minStaticStart(start, staticStorageStart(@TypeOf(global_free_list), &global_free_list));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_state_global), &kernel_state_global));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_state_ready), &kernel_state_ready));
    start = minStaticStart(start, vtd.kernelStaticStorageStartAddr());
    start = minStaticStart(start, x86_platform.kernelStaticStorageStartAddr());
    return start;
}

fn kernelStaticStorageEndAddr() usize {
    var end = uefi_services.kernelStaticStorageEndAddr();
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_spaces), &user_spaces));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_runtime_storage), &kernel_runtime_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(global_free_list), &global_free_list));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_state_global), &kernel_state_global));
    end = maxStaticEnd(end, kernel.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_state_ready), &kernel_state_ready));
    end = maxStaticEnd(end, user_copy.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, user_vm.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, page_fault_log.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, scheduler.staticStorageEndAddr());
    end = maxStaticEnd(end, syscalls.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, traps.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, smp.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, vtd.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, x86_platform.kernelStaticStorageEndAddr());
    return end;
}

// ---------------------------------------------------------------------------
// Thread label helper (used in Hooks)
// ---------------------------------------------------------------------------

fn threadLabel(thread_index: usize) []const u8 {
    const labels = comptime blk: {
        var items: [kernel.initial_thread_capacity][]const u8 = undefined;
        for (0..kernel.initial_thread_capacity) |i| {
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
// Exported extended user-state save/restore (called from assembly stubs in traps)
// ---------------------------------------------------------------------------

pub export fn saveCurrentThreadFxState() callconv(.c) void {
    if (!kernel_state_ready) return;
    const thread_index = scheduler.currentThread();
    const ctx = scheduler.threadContextMutable(thread_index) orelse return;
    if (!scheduler.threadReady(thread_index)) return;
    ctx.pkru = x86_platform.readPkru();
    asm volatile ("fxsave64 (%[ptr])"
        :
        : [ptr] "r" (&ctx.fx_state),
        : .{ .memory = true });
}

pub export fn restoreCurrentThreadFxState() callconv(.c) void {
    if (!kernel_state_ready) return;
    const thread_index = scheduler.currentThread();
    const ctx = scheduler.threadContextMutable(thread_index) orelse return;
    if (!scheduler.threadReady(thread_index)) return;
    x86_platform.writePkru(ctx.pkru);
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
        .divide_error_stub = @intFromPtr(&traps.divideErrorHandlerStub),
        .page_fault_stub = @intFromPtr(&traps.pageFaultHandlerStub),
        .general_protection_stub = @intFromPtr(&traps.generalProtectionHandlerStub),
        .double_fault_stub = @intFromPtr(&traps.doubleFaultHandlerStub),
        .invalid_opcode_stub = @intFromPtr(&traps.invalidOpcodeHandlerStub),
        .invalid_tss_stub = @intFromPtr(&traps.invalidTssHandlerStub),
        .segment_not_present_stub = @intFromPtr(&traps.segmentNotPresentHandlerStub),
        .stack_segment_fault_stub = @intFromPtr(&traps.stackSegmentFaultHandlerStub),
        .timer_interrupt_stub = @intFromPtr(&traps.timerInterruptHandlerStub),
        .scheduler_wake_ipi_stub = @intFromPtr(&traps.schedulerWakeIpiHandlerStub),
        .device_interrupt_stub = @intFromPtr(&traps.deviceInterruptHandlerStub),
        .lapic_timer_vector = boot_static.lapic_timer_vector,
        .scheduler_wake_ipi_vector = boot_static.scheduler_wake_ipi_vector,
        .device_interrupt_vector = generic_device_interrupt_vector,
        .device_interrupt_vector_count = device_interrupt_vector_count,
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
        if (!traps.mapKernelRuntimeStorage(x86_platform.mapKernelRuntimeIdentityRange)) {
            halt.haltWithMessage("trap runtime mapping failed");
        }
        if (!x86_platform.mapKernelRuntimeIdentityRange(@intFromPtr(&kernel_state_global), @sizeOf(@TypeOf(kernel_state_global)))) {
            halt.haltWithMessage("kernel state runtime mapping failed");
        }
        if (kernel_runtime_storage.len != 0 and
            !x86_platform.mapKernelRuntimeIdentityRange(@intFromPtr(kernel_runtime_storage.ptr), kernel_runtime_storage.len))
        {
            halt.haltWithMessage("kernel dynamic runtime mapping failed");
        }
        if (user_spaces.len != 0 and
            !x86_platform.mapKernelRuntimeIdentityRange(
                @intFromPtr(user_spaces.ptr),
                user_spaces.len * @sizeOf(boot_static.UserAddressSpace),
            ))
        {
            halt.haltWithMessage("user spaces runtime mapping failed");
        }
        _ = x86_platform.enablePcidIfSupported();
        const pku_enabled = x86_platform.enablePkuIfSupported();
        kernel_log.write("pku: ");
        kernel_log.write(if (pku_enabled) "enabled\n" else "unavailable\n");
        x86_platform.hardenKernelMappingsSupervisorOnly();
    }
    installInterruptTrampolines();
    x86_platform.enableSyscallEntry(@intFromPtr(&traps.syscallEntryStub));
    asm volatile ("cli");
    if (!lapic.initTimer(boot_static.lapic_timer_vector, boot_static.lapic_timer_initial_count)) {
        halt.haltWithMessage("LAPIC timer init failed");
    }
    if (!elf_loader.probe()) {
        halt.haltWithMessage("ELF loader probe failed");
    }
}

fn initMemoryModules() void {
    for (user_spaces) |*space| space.* = .{};

    user_vm.init(.{
        .user_spaces = user_spaces,
        .four_gib = boot_static.four_gib,
        .physical_map_limit = boot_static.physical_map_limit_exclusive,
        .user_low_va = boot_static.user_low_va,
        .user_top_va = boot_static.user_top_va,
        .dynamic_map_base_va = boot_static.dynamic_map_base_va,
        .dynamic_map_end_va = boot_static.dynamic_map_end_va,
        .user_va = boot_static.user_va,
        .user_stack_page_va = boot_static.user_stack_page_va,
        .canonical_user_limit_exclusive = boot_static.canonical_user_limit_exclusive,
        .page_entries = boot_static.page_entries,
        .page_addr_mask = boot_static.page_addr_mask,
        .page_present = boot_static.page_present,
        .page_rw = boot_static.page_rw,
        .page_user = boot_static.page_user,
        .page_ps = boot_static.page_ps,
        .flush_user_tlb_for_principal_va = user_copy.flushUserTlbForPrincipalVa,
        .flush_user_tlb_for_principal_range = user_copy.flushUserTlbForPrincipalRange,
        .seed_user_pdp_with_kernel_identity = x86_platform.seedUserPdpWithKernelIdentity,
        .seed_user_pd_with_kernel_identity = x86_platform.seedUserPdWithKernelIdentity,
    });
    pmm.init(.{
        .write = kernel_log.write,
        .main_addr = @intFromPtr(&kernelMain),
        .kernel_cr3_addr = @intFromPtr(&x86_platform.kernel_cr3_value),
        .kernel_image_base_paddr = &uefi_services.kernel_image_base_paddr_ref,
        .kernel_image_size_bytes = &uefi_services.kernel_image_size_bytes_ref,
        .kernel_static_start_addr = kernelStaticStorageStartAddr(),
        .kernel_static_end_addr = kernelStaticStorageEndAddr(),
        .reserved_low_mem_end = boot_static.reserved_low_mem_end,
    });
}

fn allocateBootPagesBelow4GiBOrHalt(
    bs: *std.os.uefi.tables.BootServices,
    byte_count: usize,
    label: []const u8,
) []align(4096) u8 {
    const page_count = (byte_count + 4095) / 4096;
    if (page_count == 0) return empty_kernel_runtime_storage[0..];
    const max_addr: [*]align(4096) std.os.uefi.Page = @ptrFromInt(boot_static.four_gib - 4096);
    const pages = bs.allocatePages(.{ .max_address = max_addr }, .loader_data, page_count) catch {
        kernel_log.write("boot page allocation failed: ");
        kernel_log.write(label);
        kernel_log.write("\n");
        halt.haltWithMessage("boot page allocation failed");
    };
    const ptr: [*]align(4096) u8 = @ptrCast(pages.ptr);
    const bytes = ptr[0 .. page_count * 4096];
    @memset(bytes, 0);
    return bytes;
}

fn allocateDynamicKernelStorageOrHalt(bs: *std.os.uefi.tables.BootServices) void {
    kernel_runtime_storage = allocateBootPagesBelow4GiBOrHalt(bs, kernel.runtimeStorageBytes(), "kernel runtime storage");
    if (!kernel.initRuntimeStorage(kernel_runtime_storage)) {
        halt.haltWithMessage("kernel runtime storage init failed");
    }

    const user_space_bytes = @sizeOf(boot_static.UserAddressSpace) * boot_static.user_process_count;
    const raw_user_spaces = allocateBootPagesBelow4GiBOrHalt(bs, user_space_bytes, "user address spaces");
    const ptr: [*]boot_static.UserAddressSpace = @ptrCast(@alignCast(raw_user_spaces.ptr));
    user_spaces = ptr[0..boot_static.user_process_count];
}

fn activateOrHalt(thread_index: usize) void {
    if (scheduler.activate(thread_index)) return;
    halt.haltWithMessage("activate thread failed");
}

fn scrubEndpointTargets(table: *kernel.EndpointTable, target: kernel.PrincipalId) bool {
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

fn loadRunnableThreadOrIdle(out_frame: *TrapFrame) void {
    if (scheduler.loadPolicyThread(out_frame)) return;
    halt.haltWithMessage("external scheduler unavailable");
}

fn teardownFaultedProcess(principal: kernel.PrincipalId, fault_vector: u8) void {
    const process_index = kernel.processIndexFromPrincipal(principal) orelse return;
    const spawn_parent = kernel_state_global.endpointTargetFor(principal, spawn_parent_endpoint_id);

    _ = scheduler.releasePrincipalThreads(principal);

    user_vm.lockAddressSpaces();
    user_vm.clearUserAddressSpace(principal);
    kernel_state_global.releasePrincipalNativeMemory(principal, &global_free_list);
    kernel_state_global.resetProcessRuntimeTables(process_index);
    user_vm.unlockAddressSpaces();
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
        if (!wake_owner) continue;
        if (storage_index >= kernel.process_count) continue;
        const owner = kernel.processPrincipalFromIndex(storage_index) orelse continue;
        scheduler.wakeBlockedThread(owner);
    }
    if (endpoint_targets_removed) {
        kernel_state_global.bumpEndpointGeneration();
    }

    kernel_state_global.markThreadObjectsExitedForPrincipal(principal, .killed, fault_vector);
    kernel_state_global.markProcessObjectsExited(principal, .killed, fault_vector);
    _ = kernel_state_global.markProcessFaulted(principal, fault_vector);
    if (spawn_parent) |parent| scheduler.wakeBlockedThread(parent);
}

fn teardownExitedProcess(principal: kernel.PrincipalId) void {
    const process_index = kernel.processIndexFromPrincipal(principal) orelse return;
    const spawn_parent = kernel_state_global.endpointTargetFor(principal, spawn_parent_endpoint_id);

    _ = scheduler.releasePrincipalThreads(principal);

    user_vm.lockAddressSpaces();
    user_vm.clearUserAddressSpace(principal);
    kernel_state_global.releasePrincipalNativeMemory(principal, &global_free_list);
    kernel_state_global.resetProcessRuntimeTables(process_index);
    user_vm.unlockAddressSpaces();
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
        if (!wake_owner) continue;
        if (storage_index >= kernel.process_count) continue;
        const owner = kernel.processPrincipalFromIndex(storage_index) orelse continue;
        scheduler.wakeBlockedThread(owner);
    }
    if (endpoint_targets_removed) {
        kernel_state_global.bumpEndpointGeneration();
    }

    _ = kernel_state_global.markProcessExited(principal);
    if (spawn_parent) |parent| scheduler.wakeBlockedThread(parent);
}

fn resumeAfterFatalUserException(principal: kernel.PrincipalId, fault_vector: u8, out_frame: *TrapFrame) void {
    kernel_log.write("USER fault principal=");
    kernel_log.write(principalLabel(principal));
    kernel_log.write(" thread=");
    log_util.printNumber(@as(u64, @intCast(scheduler.currentThread())));
    kernel_log.write(" cpu=");
    log_util.printNumber(@as(u64, @intCast(scheduler.currentCpu())));
    kernel_log.write(" vector=");
    log_util.printNumber(@as(u64, fault_vector));
    kernel_log.write(" rip=");
    kernel_log.writeHexRaw(out_frame.rip);
    kernel_log.write(" rsp=");
    kernel_log.writeHexRaw(out_frame.rsp);
    kernel_log.write("\n");
    teardownFaultedProcess(principal, fault_vector);
    if (!scheduler.isBootstrapSchedulerCpu()) {
        smp.returnCurrentApToIdleFromInterrupt();
    }
    loadRunnableThreadOrIdle(out_frame);
}

fn exitCurrentProcess(
    principal: kernel.PrincipalId,
    exit_code: u8,
    out_frame: *TrapFrame,
    before_ap_idle: ?scheduler.BeforeCurrentThreadLeaveCallback,
) void {
    kernel_state_global.markThreadObjectsExitedForPrincipal(principal, .exited, exit_code);
    kernel_state_global.markProcessObjectsExited(principal, .exited, exit_code);
    teardownExitedProcess(principal);
    if (!scheduler.isBootstrapSchedulerCpu()) {
        if (before_ap_idle) |callback| callback.run(callback.context);
        smp.returnCurrentApToIdleFromInterrupt();
    }
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
          [ucr3] "r" (scheduler.currentCr3()),
        : .{ .memory = true });
    while (true) {
        asm volatile ("hlt");
    }
}

// ---------------------------------------------------------------------------
// Boot resource types
// ---------------------------------------------------------------------------

const BootResources = struct {
    framebuffer_info: uefi_services.FramebufferInfo,
    disk_scheduler_policy_elf: []const u8,
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
    allocateDynamicKernelStorageOrHalt(bs);
    initMemoryModules();

    const framebuffer_info = uefi_services.acquireFramebufferInfo(bs) orelse {
        halt.haltWithMessage("GraphicsOutput unavailable or mode unsupported");
    };
    const disk_scheduler_policy_elf = uefi_services.loadBootDiskFile(bs, boot_images.scheduler_policy) orelse {
        halt.haltWithMessage("disk scheduler policy ELF load failed");
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
    scheduler.initializeStaticStorage();
    scheduler.installIdleHooks();
    smp.configureApSyscallEntry(@intFromPtr(&traps.syscallEntryStub));
    smp.configureApUserTimer(boot_static.lapic_timer_vector, boot_static.lapic_timer_initial_count);
    smp.configureWakeIpiVector(boot_static.scheduler_wake_ipi_vector);
    smp.startIdleAps(&smp_info, x86_platform.kernel_cr3_value);
    scheduler.refreshTopology();

    return .{
        .framebuffer_info = framebuffer_info,
        .disk_scheduler_policy_elf = disk_scheduler_policy_elf,
        .disk_init_elf = disk_init_elf,
        .disk_bootfs_image = disk_bootfs_image,
        .memory_stats = memory_stats,
    };
}

// ---------------------------------------------------------------------------
// Group 2a — kernel subsystem wiring (pre-process)
// ---------------------------------------------------------------------------

fn initKernelSubsystems(memory_stats: boot_static.MemoryStats) *kernel.KernelState {
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
    state.debug_process_lifecycle_hook = null;
    state.zero_physical_page_hook = user_copy.zeroPhysicalPage;

    return state;
}

// ---------------------------------------------------------------------------
// Group 4 — device discovery
// ---------------------------------------------------------------------------

fn discoverDevices() DetectedDevices {
    var result: DetectedDevices = .{
        .devices = [_]?init_setup.DetectedDeviceBootstrap{null} ** boot_abi.init_bootstrap_abi.max_device_descriptors,
    };

    var descriptor_index: usize = 0;
    appendGenericPciFunctionDevices(&result, &descriptor_index);

    return result;
}

// ---------------------------------------------------------------------------
// Group 3 — boot process construction
// ---------------------------------------------------------------------------

fn constructBootProcesses(state: *kernel.KernelState, res: BootResources, devs: *DetectedDevices) void {
    const scheduler_principal = state.createProcessDescriptor("scheduler_policy") orelse
        halt.haltWithMessage("scheduler policy process descriptor alloc failed");
    const scheduler_process = process_factory.createUserProcess(
        state,
        scheduler_principal,
        "scheduler_policy",
        &global_free_list,
        user_spaces,
    );
    state.createSerialFdAt(scheduler_principal, 1, 1) catch |err| {
        halt.haltWithError("scheduler stdout fd install failed: ", err);
    };
    state.createSerialFdAt(scheduler_principal, 2, 2) catch |err| {
        halt.haltWithError("scheduler stderr fd install failed: ", err);
    };
    state.createSchedulerControlFdAt(scheduler_principal, 16) catch |err| {
        halt.haltWithError("scheduler control fd install failed: ", err);
    };
    state.createSchedulerEventFdAt(scheduler_principal, 17) catch |err| {
        halt.haltWithError("scheduler event fd install failed: ", err);
    };

    const loaded_scheduler = elf_load.loadUserElfIntoProcessPagesOrHalt(
        state,
        scheduler_principal,
        scheduler_process.user_page.paddr,
        scheduler_process.user_stack_page.paddr,
        res.disk_scheduler_policy_elf,
        "scheduler policy ELF load failed\n",
        &global_free_list,
    );
    const scheduler_thread = scheduler.threadForPrincipal(scheduler_principal).?;
    const scheduler_ctx = scheduler.threadContextMutable(scheduler_thread).?;
    if (scheduler.threadReady(scheduler_thread)) {
        scheduler_ctx.frame.rip = loaded_scheduler.entry;
        scheduler_ctx.frame.rsp = process_factory.installInitialUserStackOrHalt(
            state,
            scheduler_principal,
            scheduler_process.user_stack_page.paddr,
            loaded_scheduler,
            "schedulerd",
        );
    }
    if (!scheduler.attachPolicyThread(scheduler_thread)) {
        halt.haltWithMessage("external scheduler policy install failed");
    }
    activateOrHalt(scheduler_thread);

    const init_principal = state.createProcessDescriptor("seed2_boot") orelse
        halt.haltWithMessage("seed2_boot process descriptor alloc failed");
    state.setBootstrapOwner(init_principal, true) catch |err| {
        halt.haltWithError("init bootstrap owner mark failed: ", err);
    };
    boot_init_principal = init_principal;
    const init_process = process_factory.createUserProcess(state, init_principal, "init", &global_free_list, user_spaces);
    state.createSerialFdAt(init_principal, 1, 1) catch |err| {
        halt.haltWithError("init stdout fd install failed: ", err);
    };
    state.createSerialFdAt(init_principal, 2, 2) catch |err| {
        halt.haltWithError("init stderr fd install failed: ", err);
    };
    init_setup.setupInitBootstrapResources(
        state,
        init_principal,
        devs.devices[0..],
        res.disk_bootfs_image,
        res.framebuffer_info,
        &global_free_list,
    );
    scheduler.scheduler_tick_accum = 0;
    scheduler.scheduler_switch_count = 0;
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
    const init_thread = scheduler.threadForPrincipal(init_principal).?;
    const init_ctx = scheduler.threadContextMutable(init_thread).?;
    init_ctx.frame.rip = loaded_init.entry;
    init_ctx.frame.rsp = process_factory.installInitialUserStackOrHalt(
        state,
        init_principal,
        init_process.user_stack_page.paddr,
        loaded_init,
        "init",
    );
    scheduler.publishThreadReady(init_thread);

    kernel_state_ready = true;
}

// ---------------------------------------------------------------------------
// Group 2b — kernel subsystem wiring (post-process, after kernel_state_ready)
// ---------------------------------------------------------------------------

fn wireRuntimeSubsystems(state: *kernel.KernelState, memory_stats: boot_static.MemoryStats) void {
    syscalls.init(.{
        .state = state,
        .free_list = &global_free_list,
        .user_spaces = user_spaces,
        .kernel_state_ready = &kernel_state_ready,
        .write = kernel_log.write,
        .print_hex = log_util.printHex,
        .print_number = log_util.printNumberU64,
        .principal_label = principalLabel,
        .read_user_u64 = user_copy.readUserU64,
        .write_user_u64 = user_copy.writeUserU64,
        .copy_user_bytes_from_va = user_copy.copyUserBytesFromVa,
        .copy_bytes_to_user_va = user_copy.copyBytesToUserVa,
        .wake_waiting_thread_for_principal = scheduler.wakeMailboxWaiter,
        .wake_blocked_thread_for_principal = scheduler.wakeBlockedThread,
        .consume_pending_signal_for_principal = scheduler.consumeSignal,
        .switch_to_thread = scheduler.switchTo,
        .block_current_thread_for_event = scheduler.blockCurrentThread,
        .exit_current_process = exitCurrentProcess,
        .total_usable_memory_bytes = memory_stats.total_usable_bytes,
    });

    traps.init(.{
        .kernel_state_ready = &kernel_state_ready,
        .state = state,
        .scheduler_quantum_ticks = boot_static.scheduler_quantum_ticks,
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
        .switch_to_thread = scheduler.switchTo,
    });
}

// ---------------------------------------------------------------------------
// kernelMain — boot sequence
// ---------------------------------------------------------------------------

pub fn kernelMain() void {
    asm volatile ("cli");
    lapic.maskLegacyPic();
    kernel_log.reset();
    serial.init();
    boot_init_principal = null;

    const resources = runBootServicesPhase();
    const state = initKernelSubsystems(resources.memory_stats);
    _ = vtd.init();
    var devices = discoverDevices();
    constructBootProcesses(state, resources, &devices);
    wireRuntimeSubsystems(state, resources.memory_stats);

    const boot_ctx = scheduler.threadContext(scheduler.currentThread()).?;
    enterUserModeIretq(boot_ctx.frame.rip, boot_ctx.frame.rsp);
}

// ---------------------------------------------------------------------------
// Helper: generic PCI device export
// ---------------------------------------------------------------------------

const virtio_vendor_id: u16 = 0x1AF4;

fn appendGenericPciFunctionDevices(result: *DetectedDevices, descriptor_index: *usize) void {
    var bus: u16 = 0;
    while (bus < 256) : (bus += 1) {
        var device: u8 = 0;
        while (device < 32) : (device += 1) {
            const func0 = pci.Location{
                .bus = @intCast(bus),
                .device = device,
                .function = 0,
            };
            if (pci.readVendorId(func0) == 0xFFFF) continue;
            const header0 = pci.readHeaderType(func0);
            const function_count: u8 = if ((header0 & 0x80) != 0) 8 else 1;
            var function: u8 = 0;
            while (function < function_count) : (function += 1) {
                const loc = pci.Location{
                    .bus = @intCast(bus),
                    .device = device,
                    .function = function,
                };
                const vendor_id = pci.readVendorId(loc);
                if (vendor_id == 0xFFFF) continue;
                if (!shouldExposeGenericPciFunction(loc, vendor_id)) continue;
                if (descriptor_index.* >= boot_abi.init_bootstrap_abi.max_device_descriptors) return;
                const resource_id = pci.resourceIdFromLocation(loc);
                appendDetectedDevice(&result.devices, .{
                    .descriptor = descriptorFromPciFunction(loc, init_bootstrap_layout.deviceConfigSourceVa(descriptor_index.*), resource_id),
                    .dma_device = resource_id,
                });
                descriptor_index.* += 1;
            }
        }
    }
}

fn shouldExposeGenericPciFunction(loc: pci.Location, vendor_id: u16) bool {
    _ = vendor_id;
    if (pci.readClassCode(loc) == 0x06) return false;
    return true;
}

fn descriptorFromPciFunction(
    loc: pci.Location,
    bootstrap_source_va: u64,
    resource_id: kernel.DmaDeviceId,
) boot_abi.init_bootstrap_abi.DeviceDescriptor {
    return .{
        .transport = @intFromEnum(boot_abi.init_bootstrap_abi.DeviceTransport.pci_function),
        .flags = 0,
        .bootstrap_source_va = bootstrap_source_va,
        .vendor_id = pci.readVendorId(loc),
        .device_id = pci.readDeviceId(loc),
        .subsystem_id = pci.readSubsystemId(loc),
        .pci_bus = loc.bus,
        .pci_device = loc.device,
        .pci_function = loc.function,
        .resource_id = resource_id,
        .queue_count = 0,
        .common_page_paddr = 0,
        .notify_page_paddr = 0,
        .isr_page_paddr = 0,
        .device_page_paddr = 0,
        .common_page_offset = 0,
        .notify_page_offset = 0,
        .isr_page_offset = 0,
        .device_page_offset = 0,
        .notify_off_multiplier = 0,
        .init_iommu_token = 0,
        .init_queue_grant_count = 0,
        .init_queue_grants = [_]boot_abi.init_bootstrap_abi.DeviceQueueGrant{.{}} ** boot_abi.init_bootstrap_abi.max_device_queue_grants,
        .init_command_token = 0,
        .init_device_fd = 0,
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
