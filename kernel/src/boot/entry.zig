/// Kernel boot entry point and primary boot globals.
/// kernelMain() is called from main.zig after switching to the ring-0 stack.
const std = @import("std");
const kernel = @import("../kernel.zig");
const capability = @import("../capability.zig");
const device_events = @import("../device_events.zig");
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
const user_copy = @import("../user_copy.zig");
const virtio_probe = @import("../virtio_probe.zig");
const kernel_vm = @import("../memory/kernel_vm.zig");
const pmm = @import("../memory/pmm.zig");
const user_vm = @import("../memory/user_vm.zig");
const x86_platform = @import("../arch/x86_64/platform.zig");
const boot_static = @import("main_static.zig");
const boot_images = @import("boot_images.zig");
const boot_abi = @import("abi.zig");
const init_bootstrap_layout = @import("init_bootstrap_layout.zig");
const uefi_services = @import("uefi_services.zig");
const process_factory = @import("process_factory.zig");
const elf_load = @import("elf_load.zig");
const init_setup = @import("init_setup.zig");
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
    start = minStaticStart(start, staticStorageStart(@TypeOf(user_spaces_storage), &user_spaces_storage));
    start = minStaticStart(start, staticStorageStart(@TypeOf(global_free_list), &global_free_list));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_state_global), &kernel_state_global));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_state_ready), &kernel_state_ready));
    start = minStaticStart(start, device_events.kernelStaticStorageStartAddr());
    start = minStaticStart(start, x86_platform.kernelStaticStorageStartAddr());
    return start;
}

fn kernelStaticStorageEndAddr() usize {
    var end = uefi_services.kernelStaticStorageEndAddr();
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_spaces_storage), &user_spaces_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(global_free_list), &global_free_list));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_state_global), &kernel_state_global));
    end = maxStaticEnd(end, kernel.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_state_ready), &kernel_state_ready));
    end = maxStaticEnd(end, capability.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, user_copy.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, user_vm.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, page_fault_log.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, process_builder.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, abi_trap_runtime.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, scheduler.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, syscalls.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, traps.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, smp.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, device_events.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, x86_platform.kernelStaticStorageEndAddr());
    return end;
}

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
        .divide_error_stub = @intFromPtr(&traps.divideErrorHandlerStub),
        .page_fault_stub = @intFromPtr(&traps.pageFaultHandlerStub),
        .general_protection_stub = @intFromPtr(&traps.generalProtectionHandlerStub),
        .double_fault_stub = @intFromPtr(&traps.doubleFaultHandlerStub),
        .invalid_opcode_stub = @intFromPtr(&traps.invalidOpcodeHandlerStub),
        .invalid_tss_stub = @intFromPtr(&traps.invalidTssHandlerStub),
        .segment_not_present_stub = @intFromPtr(&traps.segmentNotPresentHandlerStub),
        .stack_segment_fault_stub = @intFromPtr(&traps.stackSegmentFaultHandlerStub),
        .timer_interrupt_stub = @intFromPtr(&traps.timerInterruptHandlerStub),
        .device_interrupt_stub = @intFromPtr(&traps.deviceInterruptHandlerStub),
        .lapic_timer_vector = boot_static.lapic_timer_vector,
        .device_interrupt_vector = device_events.generic_device_interrupt_vector,
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
        if (!x86_platform.mapKernelRuntimeIdentityRange(@intFromPtr(&user_spaces_storage), @sizeOf(@TypeOf(user_spaces_storage)))) {
            halt.haltWithMessage("user spaces runtime mapping failed");
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
        .user_low_va = boot_static.user_low_va,
        .user_top_va = boot_static.user_top_va,
        .user_va = boot_static.user_va,
        .user_stack_page_va = boot_static.user_stack_page_va,
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
    const cpu_slot = scheduler.currentCpuSlot();
    if (cpu_slot != 0 and scheduler.userApSchedulingReady()) {
        while (true) {
            const current_thread = scheduler.currentThreadIndex();
            if (scheduler.pickNextReadyThreadIndexForCpu(cpu_slot, current_thread)) |thread_index| {
                if (scheduler.threadContextLooksCorrupted(thread_index)) {
                    _ = scheduler.repairThreadContextWithSpaces(thread_index, user_spaces, process_factory.buildInitialUserTrapFrame());
                }
                if (scheduler.activateThread(thread_index) and scheduler.loadThreadContextToFrame(thread_index, out_frame)) return;
                if (scheduler.repairThreadContextWithSpaces(thread_index, user_spaces, process_factory.buildInitialUserTrapFrame()) and
                    scheduler.activateThread(thread_index) and
                    scheduler.loadThreadContextToFrame(thread_index, out_frame)) return;
            }
            scheduler.parkCurrentApAfterCurrentThreadStopped();
        }
    }

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

    if (scheduler.threadSlotForPrincipal(principal)) |thread_index| {
        _ = scheduler.releaseThreadSlot(thread_index);
    }

    kernel_state_global.releasePrincipalPageCaps(principal, &global_free_list);
    user_spaces[process_index] = .{};
    kernel_state_global.releasePrincipalVmObjectCaps(principal, &global_free_list);
    kernel_state_global.cap_tables[process_index].reset();
    kernel_state_global.endpoint_tables[process_index] = .{};
    kernel_state_global.cap_mailboxes[process_index] = .{};
    kernel_state_global.pending_page_transfers[process_index] = null;
    kernel_state_global.vm_object_tables[process_index].reset();
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

    if (scheduler.threadSlotForPrincipal(principal)) |thread_index| {
        _ = scheduler.releaseThreadSlot(thread_index);
    }

    kernel_state_global.releasePrincipalPageCaps(principal, &global_free_list);
    user_spaces[process_index] = .{};
    kernel_state_global.releasePrincipalVmObjectCaps(principal, &global_free_list);
    kernel_state_global.cap_tables[process_index].reset();
    kernel_state_global.endpoint_tables[process_index] = .{};
    kernel_state_global.cap_mailboxes[process_index] = .{};
    kernel_state_global.pending_page_transfers[process_index] = null;
    kernel_state_global.vm_object_tables[process_index].reset();
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
    kernel_log.write("USER fault principal=");
    kernel_log.write(principalLabel(principal));
    kernel_log.write(" thread=");
    log_util.printNumber(@as(u64, @intCast(scheduler.currentThreadIndex())));
    kernel_log.write(" cpu=");
    log_util.printNumber(@as(u64, @intCast(scheduler.currentCpuSlot())));
    kernel_log.write(" vector=");
    log_util.printNumber(@as(u64, fault_vector));
    kernel_log.write(" rip=");
    kernel_log.writeHexRaw(out_frame.rip);
    kernel_log.write(" rsp=");
    kernel_log.writeHexRaw(out_frame.rsp);
    kernel_log.write("\n");
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
        .user_low_va = boot_static.user_low_va,
        .user_top_va = boot_static.user_top_va,
        .dynamic_map_base_va = boot_static.dynamic_map_base_va,
        .dynamic_map_end_va = boot_static.dynamic_map_end_va,
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
    state.debug_process_lifecycle_hook = null;
    state.pte_sync_hook = null;
    state.zero_physical_page_hook = user_copy.zeroPhysicalPage;

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
    var descriptor_index: usize = 0;
    for (probed) |entry| {
        const info = entry orelse continue;
        if (descriptor_index >= boot_abi.init_bootstrap_abi.max_device_descriptors) break;
        const resource_id = resourceIdForModernDevice(info);
        appendDetectedDevice(&result.devices, .{
            .descriptor = descriptorFromModernDevice(info, init_bootstrap_layout.deviceConfigSourceVa(descriptor_index), resource_id),
            .dma_device = resource_id,
        });
        descriptor_index += 1;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Group 3 — boot process construction
// ---------------------------------------------------------------------------

fn constructBootProcesses(state: *kernel.KernelState, res: BootResources, devs: *DetectedDevices) void {
    const init_principal = state.createProcessDescriptor("seed2_boot") orelse
        halt.haltWithMessage("seed2_boot process descriptor alloc failed");
    state.setBootstrapOwner(init_principal, true) catch |err| {
        halt.haltWithError("init bootstrap owner mark failed: ", err);
    };
    boot_init_principal = init_principal;
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
    activateThreadOrHalt(init_thread);

    state.pte_sync_hook = capability.syncPageTableRightsForPrincipalPaddr;
    kernel_state_ready = true;
}

// ---------------------------------------------------------------------------
// Group 2b — kernel subsystem wiring (post-process, after kernel_state_ready)
// ---------------------------------------------------------------------------

fn wireRuntimeSubsystems(state: *kernel.KernelState, memory_stats: boot_static.MemoryStats) void {
    process_builder.init(state, &global_free_list, user_spaces);
    abi_trap_runtime.init(.{
        .state = state,
        .free_list = &global_free_list,
        .user_spaces = user_spaces,
        .write = kernel_log.write,
        .print_hex = log_util.printHex,
        .print_number = log_util.printNumberU64,
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
        .write = kernel_log.write,
        .print_hex = log_util.printHex,
        .print_number = log_util.printNumberU64,
        .principal_label = principalLabel,
        .principal_from_process_slot = principalFromProcessSlot,
        .read_user_u64 = user_copy.readUserU64,
        .write_user_u64 = user_copy.writeUserU64,
        .copy_user_bytes_from_va = user_copy.copyUserBytesFromVa,
        .copy_bytes_to_user_va = user_copy.copyBytesToUserVa,
        .wake_waiting_thread_for_principal = scheduler.wakeWaitingThreadForPrincipal,
        .wake_blocked_thread_for_principal = scheduler.wakeBlockedThreadForPrincipal,
        .consume_pending_signal_for_principal = scheduler.consumePendingSignalForPrincipal,
        .switch_to_thread = scheduler.switchToThread,
        .block_current_thread_for_event = scheduler.blockCurrentThreadForEvent,
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
        .switch_to_thread = scheduler.switchToThread,
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

fn resourceIdForModernDevice(info: virtio_probe.ModernDeviceInfo) kernel.DmaDeviceId {
    const loc = info.location;
    return 0x50434900_00000000 |
        (@as(u64, loc.bus) << 16) |
        (@as(u64, loc.device) << 8) |
        @as(u64, loc.function);
}

fn descriptorFromModernDevice(
    info: virtio_probe.ModernDeviceInfo,
    bootstrap_source_va: u64,
    resource_id: kernel.DmaDeviceId,
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
        .resource_id = resource_id,
        .queue_count = info.queue_count,
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
