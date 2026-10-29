/// Kernel boot entry point and primary boot globals.
/// Bootloader-specific entry code switches to the ring-0 stack before entering
/// this module.
const std = @import("std");
const abi_root = @import("kernel_abi_root");
const kernel = @import("../kernel.zig");
const kernel_runtime = @import("../kernel_runtime.zig");
const elf_loader = @import("../elf_loader.zig");
const scheduler = @import("../scheduler.zig").connection;
const syscalls = @import("../syscalls.zig");
const traps = @import("../traps.zig");
const interrupts = @import("../interrupts.zig");
const lapic = @import("../lapic.zig");
const smp = @import("../smp.zig");
const iommu = @import("../iommu.zig");
const acpi_power = @import("../acpi_power.zig");
const serial = @import("../serial.zig");
const kernel_log = @import("../kernel_log.zig");
const page_fault_log = @import("../page_fault_log.zig");
const user_copy = @import("../user_copy.zig");
const pci = @import("../pci.zig");
const kernel_vm = @import("../memory/kernel_vm.zig");
const pmm = @import("../memory/pmm.zig");
const user_vm = @import("../memory/user_vm.zig");
const UserVirtualAddress = @import("../memory/addresses.zig").UserVirtualAddress;
const x86_platform = @import("../arch/x86_64/platform.zig");
const xstate_runtime = @import("../xstate_runtime.zig");
const boot_static = @import("main_static.zig");
const boot_resources = @import("boot_resources.zig");
const boot_diag = @import("boot_diag.zig");
const boot_scratch = @import("boot_scratch.zig");
const resource_pages = @import("resource_pages.zig");
const image_range = @import("image_range.zig");
const process_factory = @import("process_factory.zig");
const elf_load = @import("elf_load.zig");
const init_setup = @import("init_setup.zig");
const halt = @import("../halt.zig");
const log_util = @import("../log_util.zig");

const TrapFrame = interrupts.TrapFrame;
const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;
const fd_abi = abi_root.fd_abi;
const enable_exit_teardown_metrics = false;

// ---------------------------------------------------------------------------
// Boot globals
// ---------------------------------------------------------------------------

var empty_user_spaces_storage: [0]boot_static.UserAddressSpace align(4096) = .{};
var user_spaces: []boot_static.UserAddressSpace = empty_user_spaces_storage[0..];
var user_space_table: boot_static.UserAddressSpaceTable = .{};
var empty_kernel_runtime_storage: [0]u8 align(4096) = .{};
var kernel_runtime_storage: []align(4096) u8 = empty_kernel_runtime_storage[0..];

const limine_runtime_storage_bytes: usize = kernel.runtimeStorageBytes();
var limine_free_list_storage: kernel.FreePageList align(4096) = undefined;
var limine_kernel_state_storage: kernel.KernelState align(4096) = undefined;
var limine_kernel_runtime_storage: [limine_runtime_storage_bytes]u8 align(4096) = undefined;
var limine_user_spaces_storage: [boot_static.user_process_count]boot_static.UserAddressSpace align(4096) = undefined;

var boot_init_principal: ?kernel.PrincipalId = null;
var boot_rsdp_paddr: u64 = 0;

/// E is before IOMMU ownership changes and userland. Linux inspected later
/// has already replaced this firmware state, so the isolated ELF reads it here.
pub fn diagnosticSnapshotVtd() noreturn {
    @import("../vtd.zig").diagnosticSnapshot(boot_rsdp_paddr);
    boot_diag.logFramebufferSnapshot();
    halt.haltWithMessage("VT-D READ-ONLY SNAPSHOT COMPLETE - BEFORE INITIALIZATION");
}
const unrouted_device_interrupt_vector: u8 = 0x41;
const platform_device = @import("../platform_device.zig");
const device_interrupt_vector: u8 = platform_device.interrupt_vector;
const device_interrupt_vector_count: u8 = pci.interrupt_vector_end_exclusive - device_interrupt_vector;
comptime {
    @import("std").debug.assert(x86_platform.deviceInterruptLayoutValid(device_interrupt_vector, device_interrupt_vector_count,
        boot_static.lapic_timer_vector, boot_static.scheduler_wake_ipi_vector, unrouted_device_interrupt_vector));
    @import("std").debug.assert(platform_device.interrupt_vector < pci.interrupt_vector_base);
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    // Zero-sized storage has only a compiler sentinel, no reservable bytes.
    if (@sizeOf(T) == 0) return 0;
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn staticStorageStart(comptime T: type, ptr: *T) usize {
    // A zero-sized array may have address=alignment. Including that sentinel
    // in the minimum would reserve nearly all physical RAM during boot.
    if (@sizeOf(T) == 0) return std.math.maxInt(usize);
    return @intFromPtr(ptr);
}

comptime {
    // The backing hash was the last runtime BSS allocation. Its empty array
    // must be the identity element for the reserved-range aggregation.
    std.debug.assert(staticStorageStart([0]u8, @ptrFromInt(4096)) == std.math.maxInt(usize));
    std.debug.assert(staticStorageEnd([0]u8, @ptrFromInt(4096)) == 0);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

fn minStaticStart(a: usize, b: usize) usize {
    return if (a < b) a else b;
}

fn kernelStaticStorageStartAddr() usize {
    var start = boot_scratch.kernelStaticStorageStartAddr();
    start = minStaticStart(start, boot_scratch.kernelStaticStorageStartAddr());
    start = minStaticStart(start, image_range.kernelStaticStorageStartAddr());
    start = minStaticStart(start, staticStorageStart(@TypeOf(user_spaces), &user_spaces));
    start = minStaticStart(start, staticStorageStart(@TypeOf(user_space_table), &user_space_table));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_runtime_storage), &kernel_runtime_storage));
    start = minStaticStart(start, kernel_runtime.kernelStaticStorageStartAddr());
    start = minStaticStart(start, staticStorageStart(@TypeOf(limine_free_list_storage), &limine_free_list_storage));
    start = minStaticStart(start, staticStorageStart(@TypeOf(limine_kernel_state_storage), &limine_kernel_state_storage));
    start = minStaticStart(start, staticStorageStart(@TypeOf(limine_kernel_runtime_storage), &limine_kernel_runtime_storage));
    start = minStaticStart(start, staticStorageStart(@TypeOf(limine_user_spaces_storage), &limine_user_spaces_storage));
    start = minStaticStart(start, staticStorageStart(@TypeOf(boot_rsdp_paddr), &boot_rsdp_paddr));
    start = minStaticStart(start, iommu.kernelStaticStorageStartAddr());
    start = minStaticStart(start, acpi_power.staticStart());
    start = minStaticStart(start, platform_device.staticStart());
    start = minStaticStart(start, x86_platform.kernelStaticStorageStartAddr());
    return start;
}

fn kernelStaticStorageEndAddr() usize {
    var end = boot_scratch.kernelStaticStorageEndAddr();
    end = maxStaticEnd(end, boot_scratch.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, image_range.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_spaces), &user_spaces));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(user_space_table), &user_space_table));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_runtime_storage), &kernel_runtime_storage));
    end = maxStaticEnd(end, kernel_runtime.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, kernel.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(limine_free_list_storage), &limine_free_list_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(limine_kernel_state_storage), &limine_kernel_state_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(limine_kernel_runtime_storage), &limine_kernel_runtime_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(limine_user_spaces_storage), &limine_user_spaces_storage));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(boot_rsdp_paddr), &boot_rsdp_paddr));
    end = maxStaticEnd(end, iommu.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, acpi_power.staticEnd());
    end = maxStaticEnd(end, pci.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, platform_device.staticEnd());
    end = maxStaticEnd(end, user_copy.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, user_vm.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, page_fault_log.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, scheduler.staticStorageEndAddr());
    end = maxStaticEnd(end, syscalls.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, traps.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, xstate_runtime.staticEnd());
    end = maxStaticEnd(end, smp.kernelStaticStorageEndAddr());
    end = maxStaticEnd(end, @import("../smp_perf.zig").staticEnd());
    end = maxStaticEnd(end, @import("../wake_trace.zig").staticEnd());
    end = maxStaticEnd(end, @import("../sync/lock_diagnostics.zig").staticEnd());
    end = maxStaticEnd(end, x86_platform.kernelStaticStorageEndAddr());
    return end;
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

fn userCr3ForPrincipal(principal: kernel.PrincipalId) u64 {
    const idx = kernel.processIndexFromPrincipal(principal) orelse return 0;
    const space = user_space_table.borrow(idx) orelse return 0;
    defer user_space_table.endBorrow(space);
    return x86_platform.cr3WithUserPcid(space.cr3, x86_platform.userPcidForProcessIndex(idx));
}

// ---------------------------------------------------------------------------
// Platform initialization
// ---------------------------------------------------------------------------

fn installInterruptTrampolines() void {
    x86_platform.installInterruptTrampolines(.{
        .divide_error_stub = @intFromPtr(&traps.divideErrorHandlerStub),
        .breakpoint_stub = @intFromPtr(&traps.breakpointHandlerStub),
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
        .unrouted_device_interrupt_vector = unrouted_device_interrupt_vector,
        .device_interrupt_vector = device_interrupt_vector,
        .device_interrupt_vector_count = device_interrupt_vector_count,
    });
}

fn initKernelRuntimeOrHalt() void {
    if (!x86_platform.enableNxForCurrentCpu()) {
        halt.haltWithMessage("NX unavailable");
    }
    if (!x86_platform.enableWriteProtectForCurrentCpu()) halt.haltWithMessage("WP/NXE unavailable");
    xstate_runtime.initXStateSupport();
    if (!boot_static.debug_skip_cr3_switch) {
        if (!x86_platform.installKernelPageTables()) {
            halt.haltWithMessage("page table install failed");
        }
        // The diagnostic image alone has a mapped GOP after this CR3 switch.
        // Limine's framebuffer pointer must never be used from here onward.
        boot_diag.afterCr3();
        if (!user_copy.mapKernelRuntimeStorage(x86_platform.validateKernelRuntimeRange)) {
            halt.haltWithMessage("user copy runtime mapping failed");
        }
        if (!traps.mapKernelRuntimeStorage(x86_platform.validateKernelRuntimeRange)) {
            halt.haltWithMessage("trap runtime mapping failed");
        }
        if (!x86_platform.validateKernelRuntimeRange(@intFromPtr(kernel_runtime.kernel_state_global), @sizeOf(kernel.KernelState))) {
            halt.haltWithMessage("kernel state runtime mapping failed");
        }
        if (!x86_platform.validateKernelRuntimeRange(@intFromPtr(kernel_runtime.global_free_list), @sizeOf(kernel.FreePageList))) {
            halt.haltWithMessage("free list runtime mapping failed");
        }
        if (kernel_runtime_storage.len != 0 and
            !x86_platform.validateKernelRuntimeRange(@intFromPtr(kernel_runtime_storage.ptr), kernel_runtime_storage.len))
        {
            halt.haltWithMessage("kernel dynamic runtime mapping failed");
        }
        if (user_spaces.len != 0 and
            !x86_platform.validateKernelRuntimeRange(
                @intFromPtr(user_spaces.ptr),
                user_spaces.len * @sizeOf(boot_static.UserAddressSpace),
            ))
        {
            halt.haltWithMessage("user spaces runtime mapping failed");
        }
        _ = x86_platform.detectRdtscpSupport();
        _ = x86_platform.enablePcidIfSupported();
        kernel_memory_init();
        _ = x86_platform.enableFsgsbaseIfSupported();
        const pku_enabled = x86_platform.enablePkuIfSupported();
        kernel_log.write("pku: ");
        kernel_log.write(if (pku_enabled) "enabled\n" else "unavailable\n");
        x86_platform.hardenKernelMappingsSupervisorOnly();
        pci.initEcam(boot_rsdp_paddr);
        platform_device.prepareBootMmio(boot_rsdp_paddr);
        if (!iommu.prepareBootMmio(boot_rsdp_paddr)) {
            halt.haltWithMessage("IOMMU boot MMIO mapping failed");
        }
        const hpet = @import("../hpet.zig");
        const hpet_mapped = hpet.prepareBootMmio(boot_rsdp_paddr);
        const hpet_map = hpet.diagnostic();
        kernel_log.writeFmt("clock: HPET boot UC map={} reason={s} base=0x{x}\n", .{
            @intFromBool(hpet_mapped), @tagName(hpet_map.reason), hpet_map.address,
        });
    }
    x86_platform.loadGdtAndReloadSegments();
    installInterruptTrampolines();
    x86_platform.enableSyscallEntry(traps.syscallEntryForCpu(0));
    asm volatile ("cli");
    if (!lapic.initTimer(boot_static.lapic_timer_vector, boot_static.lapic_timer_initial_count)) {
        halt.haltWithMessage("LAPIC timer init failed");
    }
    kernel_log.write(if (lapic.usesX2Apic()) "lapic: mode=x2apic\n" else "lapic: mode=xapic\n");
    if (!elf_loader.probe()) {
        halt.haltWithMessage("ELF loader probe failed");
    }
}

fn initMemoryModules() void {
    user_space_table.initializeFreshStorage(user_spaces);
    for (user_spaces) |*space| {
        // First initialization owns the lock state; later address-space resets
        // preserve it. Initialize in place to avoid a multi-MiB .{} constant.
        space.lock_state = .{};
        user_vm.initializeUserAddressSpaceStorage(space);
    }

    user_vm.init(.{
        .retire_user_tlb_for_principal = user_copy.retireUserTlbForPrincipal,
        .before_pte_retirement = retainRetiredPageViewWrites,
        .pt_allocator = @import("../memory/identity_allocator.zig").allocatorFor(kernel_runtime.global_free_list, .pt_storage),
        .user_spaces = &user_space_table,
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
        .page_nx = boot_static.page_nx,
        .flush_user_tlb_for_principal_va = user_copy.flushUserTlbForPrincipalVa,
        .flush_user_tlb_for_principal_range = user_copy.flushUserTlbForPrincipalRange,
        .kernel_pointer_paddr = x86_platform.kernelAddressToPhysical,
        .seed_user_pml4_with_kernel = x86_platform.seedUserPml4WithKernel,
    });
    pmm.init(.{
        .write = kernel_log.write,
        .main_addr = @intFromPtr(&bootWithResources),
        .kernel_cr3_addr = @intFromPtr(&x86_platform.kernel_cr3_value),
        .kernel_image_base_paddr = &image_range.base_paddr,
        .kernel_image_size_bytes = &image_range.size_bytes,
        .kernel_static_start_addr = kernelStaticStorageStartAddr(),
        .kernel_static_end_addr = kernelStaticStorageEndAddr(),
        .reserved_low_mem_end = boot_static.reserved_low_mem_end,
    });
}

pub fn prepareLimineKernelStorageOrHalt() void {
    kernel_runtime.global_free_list = &limine_free_list_storage;
    // This boot-only reset must include every allocator index/cache field.
    kernel_runtime.global_free_list.* = .{};

    kernel_runtime.kernel_state_global = &limine_kernel_state_storage;

    kernel_runtime_storage = limine_kernel_runtime_storage[0..];
    @memset(kernel_runtime_storage, 0);
    if (!kernel.initRuntimeStorage(kernel_runtime_storage)) {
        halt.haltWithMessage("kernel runtime storage init failed");
    }

    user_spaces = limine_user_spaces_storage[0..];
    initMemoryModules();
    if (image_range.virtual_base != 0) {
        const static_end = kernel_vm.pageAlignUp(kernelStaticStorageEndAddr());
        if (static_end > image_range.virtual_base) {
            const required_size: usize = @intCast(static_end - image_range.virtual_base);
            if (required_size > image_range.size_bytes) image_range.size_bytes = required_size;
        }
    }
}

pub fn currentUserSpaces() []boot_static.UserAddressSpace {
    return user_spaces;
}

fn copyBootBytesToScratchOrHalt(bytes: []const u8, label: []const u8) boot_scratch.Allocation {
    const allocation = boot_scratch.reserveTemporary(bytes.len) catch {
        kernel_log.write("boot resource copy failed: ");
        kernel_log.write(label);
        kernel_log.write("\n");
        halt.haltWithMessage("boot resource copy failed");
    };
    var loan: boot_scratch.Borrow = .{};
    allocation.borrowInto(&loan) catch halt.haltWithMessage("boot resource borrow rejected");
    const out = loan.bytes() catch halt.haltWithMessage("boot resource view rejected");
    @memcpy(out[0..bytes.len], bytes);
    loan.end() catch halt.haltWithMessage("boot resource borrow end rejected");
    return allocation;
}

pub fn persistBootResourcesOrHalt(resources: BootResources, hhdm_offset: u64) PreparedBootResources {
    const bootfs = resources.bootfs_image;
    const hhdm_view = resource_pages.hhdm.current(hhdm_offset) orelse
        halt.haltWithMessage("bootfs HHDM paging mode unsupported");
    // Module bytes are also Limine HHDM loans. Check complete source ranges
    // before copying, as well as checking each owned destination below.
    for ([_][]const u8{ resources.init_elf, bootfs }) |source| {
        const virtual = @intFromPtr(source.ptr);
        if (virtual < hhdm_offset or !hhdm_view.containsReadable(virtual - hhdm_offset, source.len))
            halt.haltWithMessage("boot resource source HHDM mapping missing");
    }
    // Handles, not escaped scratch slices, cross the CR3/boot stages. Init
    // outlives the nested page-address list, then ends after ELF copying.
    const staged_init = copyBootBytesToScratchOrHalt(resources.init_elf, "init");
    const staging = resource_pages.stagingLayout(bootfs.len) catch
        halt.haltWithMessage("bootfs staging size invalid or overflow");
    const page_count = staging.pages;
    // Scratch holds address-stable ownership records, not image bytes. The
    // input determines the ledger size; exhaustion remains an arena/PMM error.
    const metadata_bytes = staging.metadata_bytes;
    const metadata = boot_scratch.reserveTemporary(metadata_bytes) catch
        halt.haltWithMessage("bootfs staging metadata unavailable");
    var loan: boot_scratch.Borrow = .{};
    metadata.borrowInto(&loan) catch halt.haltWithMessage("bootfs staging borrow rejected");
    const pages = std.mem.bytesAsSlice(resource_pages.StagedPage, loan.bytes() catch halt.haltWithMessage("bootfs staging view rejected"));
    // Only this fresh allocation is initialized. Reinitializing a live or
    // released record would discard grant history and defeat copy checks.
    for (pages) |*slot| slot.* = .{};
    const low_before = kernel_runtime.global_free_list.availableBelow(1 << 32);
    var low_pages: usize = 0;
    var search: resource_pages.Search = .{};
    for (pages, 0..) |*slot, index| {
        // Fatal boot failures retain the arena/loan and any staged pages.
        // haltLoop does not unwind parent defers; do not claim rollback here.
        slot.acquireMapped(kernel_runtime.global_free_list, &search, hhdm_view) catch
            halt.haltWithMessage("bootfs staging mapped page unavailable or return rejected");
        var page_loan: resource_pages.Loan = .{};
        slot.borrowInto(&page_loan) catch halt.haltWithMessage("bootfs page borrow rejected");
        if ((page_loan.physical() catch halt.haltWithMessage("bootfs physical view rejected")).toInt() < 1 << 32) low_pages += 1;
        const offset = index * 4096;
        const length = @min(bootfs.len - offset, 4096);
        const target = page_loan.hhdmBytes(.fromInt(hhdm_offset), hhdm_view) catch halt.haltWithMessage("bootfs staging HHDM rejected");
        @memcpy(target[0..length], bootfs[offset .. offset + length]);
        page_loan.end() catch halt.haltWithMessage("bootfs page borrow end rejected");
    }
    if (search.rejected != 0) kernel_log.writeFmt("bootfs: skipped unmapped HHDM candidates={}\n", .{search.rejected});
    const low_after = kernel_runtime.global_free_list.availableBelow(1 << 32);
    kernel_log.writeFmt("bootfs: staged pages={} metadata_bytes={} below4G={} free_low_before={} free_low_after={}\n", .{ page_count, metadata_bytes, low_pages, low_before.total, low_after.total });
    loan.end() catch halt.haltWithMessage("bootfs staging borrow end rejected");
    return .{
        .framebuffer_info = resources.framebuffer_info,
        .init_staging = staged_init,
        .bootfs_staging = metadata,
        .bootfs_size = bootfs.len,
        .memory_stats = resources.memory_stats,
    };
}

fn wakeThreadTargetsFromBoot(targets: []const kernel.ThreadWakeTarget) void {
    _ = @import("../syscall/fd.zig").wakeThreadTargets(.{ .write_user_u64 = user_copy.writeUserU64 }, kernel_runtime.kernel_state_global, targets);
}

// Fault/exit dispatch must not inherit this seed frame. Group links keep the
// ready set under K so each group is claimed and published only once.
noinline fn wakeTaskFdWaiters(principal: kernel.PrincipalId) void {
    var wake_storage: [32]kernel.ThreadWakeTarget = undefined;
    while (true) {
        const wake_count = kernel_runtime.kernel_state_global.takeTaskReadableWaitersForPrincipal(
            principal,
            wake_storage[0..],
        );
        if (wake_count == 0) break;
        wakeThreadTargetsFromBoot(wake_storage[0..wake_count]);
    }
}

fn teardownFaultedProcess(principal: kernel.PrincipalId, fault_vector: u8) void {
    const process_index = kernel.processIndexFromPrincipal(principal) orelse return;

    // Teardown owns K and has quiesced the principal. Move scheduler bindings
    // under T, then reclaim under K before destroying the caller's mappings.
    scheduler.detachPrincipalIpcBindings(principal);
    kernel_runtime.kernel_state_global.drainIpcCancellations(kernel_runtime.global_free_list);
    kernel_runtime.kernel_state_global.cancelFdWaitGroupsForOwner(principal);
    _ = scheduler.releasePrincipalThreads(principal);

    @import("../syscall/fd.zig").closeProcessFdsWithWakes(.{
        .free_list = kernel_runtime.global_free_list,
        .write_user_u64 = user_copy.writeUserU64,
    }, kernel_runtime.kernel_state_global, principal, false);
    if (!user_vm.lockVmTransaction(principal)) return;
    user_vm.clearUserAddressSpace(principal);
    kernel_runtime.kernel_state_global.releasePrincipalNativeMemory(principal, kernel_runtime.global_free_list);
    kernel_runtime.kernel_state_global.resetProcessRuntimeTables(process_index);
    user_vm.retireUserAddressSpace(principal);
    user_vm.unlockVmTransaction(principal);

    kernel_runtime.kernel_state_global.markThreadObjectsExitedForPrincipal(principal, .killed, fault_vector);
    kernel_runtime.kernel_state_global.markProcessObjectsExited(principal, .killed, fault_vector);
    wakeTaskFdWaiters(principal);
    _ = kernel_runtime.kernel_state_global.markProcessFaulted(principal, fault_vector);
}

fn teardownExitedProcess(principal: kernel.PrincipalId) void {
    const process_index = kernel.processIndexFromPrincipal(principal) orelse return;
    const metric_start = if (enable_exit_teardown_metrics) x86_platform.readTimestampCounter() else 0;

    // Teardown owns K and has quiesced the principal. Move scheduler bindings
    // under T, then reclaim under K before destroying the caller's mappings.
    scheduler.detachPrincipalIpcBindings(principal);
    kernel_runtime.kernel_state_global.drainIpcCancellations(kernel_runtime.global_free_list);
    kernel_runtime.kernel_state_global.cancelFdWaitGroupsForOwner(principal);
    _ = scheduler.releasePrincipalThreads(principal);
    const metric_after_release_threads = if (enable_exit_teardown_metrics) x86_platform.readTimestampCounter() else 0;

    @import("../syscall/fd.zig").closeProcessFdsWithWakes(.{
        .free_list = kernel_runtime.global_free_list,
        .write_user_u64 = user_copy.writeUserU64,
    }, kernel_runtime.kernel_state_global, principal, false);
    if (!user_vm.lockVmTransaction(principal)) return;
    user_vm.clearUserAddressSpace(principal);
    const metric_after_clear_as = if (enable_exit_teardown_metrics) x86_platform.readTimestampCounter() else 0;
    kernel_runtime.kernel_state_global.releasePrincipalNativeMemory(principal, kernel_runtime.global_free_list);
    const metric_after_release_mem = if (enable_exit_teardown_metrics) x86_platform.readTimestampCounter() else 0;
    kernel_runtime.kernel_state_global.resetProcessRuntimeTables(process_index);
    const metric_after_reset_tables = if (enable_exit_teardown_metrics) x86_platform.readTimestampCounter() else 0;
    user_vm.retireUserAddressSpace(principal);
    user_vm.unlockVmTransaction(principal);

    _ = kernel_runtime.kernel_state_global.markProcessExited(principal);
    const metric_after_mark = if (enable_exit_teardown_metrics) x86_platform.readTimestampCounter() else 0;
    if (enable_exit_teardown_metrics) {
        kernel_log.writeFmt(
            "[kernel] metric scope=process_exit principal={} total_cycles={} release_threads={} clear_as={} release_mem={} reset_tables={} mark_exit={}\n",
            .{
                @intFromEnum(principal),
                metric_after_mark -% metric_start,
                metric_after_release_threads -% metric_start,
                metric_after_clear_as -% metric_after_release_threads,
                metric_after_release_mem -% metric_after_clear_as,
                metric_after_reset_tables -% metric_after_release_mem,
                metric_after_mark -% metric_after_reset_tables,
            },
        );
    }
}

pub export fn resumeAfterFatalUserException(principal: kernel.PrincipalId, fault_vector: u8, out_frame: *TrapFrame) callconv(.winapi) void {
    kernel_log.write("USER fault principal=");
    kernel_log.write(kernel_runtime.principalLabel(principal));
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
    switch (syscalls.beginExternalPrincipalTeardown(principal)) {
        .acquired => {
            teardownFaultedProcess(principal, fault_vector);
            syscalls.endExternalPrincipalTeardown();
        },
        .handled_by_existing_lifecycle => {},
    }
    if (!scheduler.isBootstrapSchedulerCpu()) {
        smp.returnCurrentApToIdleFromInterrupt();
    }
    scheduler.loadNextReadyThreadOrIdle(out_frame);
}

fn exitCurrentProcess(
    principal: kernel.PrincipalId,
    out_frame: *TrapFrame,
    before_ap_idle: ?scheduler.BeforeCurrentThreadLeaveCallback,
    after_teardown: ?scheduler.BeforeCurrentThreadLeaveCallback,
) void {
    const exit_cpu = if (enable_exit_teardown_metrics) scheduler.currentCpu() else 0;
    const bootstrap_cpu = scheduler.isBootstrapSchedulerCpu();
    // PROCESS_EXIT arrives with both the lifecycle gate and state lock held.
    // The inactive-principal fallback enters that same exclusion here.
    var external_teardown_acquired = false;
    var teardown_owned_elsewhere = false;
    if (before_ap_idle == null) {
        switch (syscalls.beginExternalPrincipalTeardown(principal)) {
            .acquired => external_teardown_acquired = true,
            .handled_by_existing_lifecycle => teardown_owned_elsewhere = true,
        }
    }

    if (!teardown_owned_elsewhere) {
        teardownExitedProcess(principal);
        if (after_teardown) |callback| callback.run(callback.context);
    }
    if (external_teardown_acquired) {
        syscalls.endExternalPrincipalTeardown();
        external_teardown_acquired = false;
    }
    if (!bootstrap_cpu) {
        if (enable_exit_teardown_metrics) {
            kernel_log.writeFmt(
                "[kernel] metric scope=process_exit_handoff principal={} cpu={} has_target={} prepared={} loaded={} bootstrap={}\n",
                .{
                    @intFromEnum(principal),
                    exit_cpu,
                    @as(u8, 0),
                    @as(u8, 0),
                    @as(u8, 0),
                    @as(u8, 0),
                },
            );
        }
        if (before_ap_idle) |callback| callback.run(callback.context);
        smp.returnCurrentApToIdleFromInterrupt();
    }
    if (enable_exit_teardown_metrics) {
        kernel_log.writeFmt(
            "[kernel] metric scope=process_exit_handoff principal={} cpu={} has_target={} prepared={} loaded={} bootstrap={}\n",
            .{
                @intFromEnum(principal),
                exit_cpu,
                @as(u8, 0),
                @as(u8, 0),
                @as(u8, 0),
                @as(u8, 1),
            },
        );
    }
    if (before_ap_idle) |callback| callback.run(callback.context);
    scheduler.loadNextReadyThreadOrIdle(out_frame);
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
    // First entry uses the same publication/fence API, with a flushing load.
    const first_user_cr3 = x86_platform.userReturnCr3(scheduler.currentCr3(), true);

    xstate_runtime.restoreCurrentThreadXState();

    // user_return_iret_frame lives in main.zig (exported asm symbol)
    const iret: *volatile [5]u64 = @extern(*volatile [5]u64, .{ .name = "user_return_iret_frame" });
    iret[0] = user_entry_va;
    iret[1] = user_cs;
    iret[2] = user_rflags;
    iret[3] = user_rsp;
    iret[4] = user_ss;

    asm volatile (
        \\mov %[k_rsp], %%rsp
        \\mov %[ucr3], %%rax
        \\mov %%rax, %%cr3
    ++ traps.asmAssertUserCr3() ++
        \\lea user_return_iret_frame(%rip), %%rsp
        \\iretq
        :
        : [k_rsp] "r" (kernel_transition_rsp),
          [ucr3] "r" (first_user_cr3),
        : .{ .memory = true });
    while (true) {
        asm volatile ("hlt");
    }
}

// ---------------------------------------------------------------------------
// Boot resource types
// ---------------------------------------------------------------------------

pub const BootResources = struct {
    framebuffer_info: boot_resources.FramebufferInfo,
    init_elf: []const u8,
    bootfs_image: []const u8,
    memory_stats: boot_static.MemoryStats,
};

// A prepared resource owns kernel staging allocations. It cannot accidentally
// retain the bootloader's module slice after the original mappings disappear.
pub const PreparedBootResources = struct {
    framebuffer_info: boot_resources.FramebufferInfo,
    init_staging: boot_scratch.Allocation,
    bootfs_staging: boot_scratch.Allocation,
    bootfs_size: usize,
    memory_stats: boot_static.MemoryStats,
};

pub const LimineSmpResources = struct {
    rsdp_paddr: u64,
    trampoline_base: u64,
};

pub fn initializeLimineRuntimeOrHalt(smp_resources: LimineSmpResources) void {
    boot_rsdp_paddr = smp_resources.rsdp_paddr;
    initKernelRuntimeOrHalt();
    // AP trampoline W->X transitions already need the common TLB rendezvous.
    // Publish its hooks before any AP can start or receive a shootdown IPI.
    user_copy.init(.{
        .state = kernel_runtime.kernel_state_global,
        .free_list = kernel_runtime.global_free_list,
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
    acpi_power.prepare(boot_rsdp_paddr);
    kernel_log.write("boot: scheduler static\n");
    scheduler.initializeStaticStorage();
    kernel_log.write("boot: idle hooks\n");
    scheduler.installIdleHooks();
    const cpu_runtime = @import("../arch/x86_64/cpu_runtime.zig");
    cpu_runtime.init(kernel_runtime.global_free_list) catch |err| halt.haltWithError("CPU runtime initialization failed: ", err);
    var smp_info = smp.prepareBootInfo(
        smp_resources.rsdp_paddr,
        smp_resources.trampoline_base,
    ) orelse {
        halt.haltWithMessage("SMP boot information invalid");
    };
    cpu_runtime.setCapacity(smp_info.lapic_count) catch |err| halt.haltWithError("CPU capacity rejected: ", err);
    // No AP or user root exists yet. Complete shared mappings and every row
    // before publishing topology; the old BSP seed remains mapped.
    inline for (.{ x86_platform, traps, smp, scheduler, user_copy, @import("../tlb_residency.zig"), @import("../smp_perf.zig"), @import("../lock_profile.zig"), @import("../ipc_metric.zig"), @import("../sync/lock_diagnostics.zig"), @import("../realtime_clock.zig") }) |module| {
        module.initCpuStorage(smp_info.lapic_count) catch |err| halt.haltWithError("CPU storage allocation failed: ", err);
    }
    if (!x86_platform.checkBootPermissions(kernel_runtime.global_free_list)) halt.haltWithMessage("mapping permission self-check failed");
    for (1..smp_info.lapic_count) |cpu_slot| {
        if (!x86_platform.allocateApRuntimeStacks(cpu_slot, kernel_runtime.global_free_list)) {
            halt.haltWithMessage("SMP application processor stack allocation failed");
        }
    }
    kernel_runtime.global_free_list.initCaches(smp_info.lapic_count) catch |err| halt.haltWithError("PMM CPU cache initialization failed: ", err);
    cpu_runtime.seal();
    smp.configureApSyscallEntry(&traps.syscallEntryForCpu);
    smp.configureApUserTimer(
        boot_static.lapic_timer_vector,
        lapic.timerInitialCount(boot_static.lapic_timer_initial_count) * @as(u32, @intCast(boot_static.scheduler_slice_ticks)),
    );
    smp.configureWakeIpiVector(boot_static.scheduler_wake_ipi_vector);
    if (!smp.startIdleAps(&smp_info, x86_platform.kernel_cr3_value)) {
        halt.haltWithMessage("SMP application processor startup failed");
    }
    if (!x86_platform.checkRetiredLowMappings(kernel_runtime.global_free_list, smp_info.trampoline_base))
        halt.haltWithMessage("CR3 isolation self-check failed");
    kernel_log.writeFmt("boot: cpus ready count={}\n", .{smp.cpuCount()});
    kernel_log.write("boot: topology\n");
    scheduler.refreshTopology();
    kernel_log.write("boot: limine runtime done\n");
}

// ---------------------------------------------------------------------------
// Group 2a — kernel subsystem wiring (pre-process)
// ---------------------------------------------------------------------------

fn initKernelSubsystems(memory_stats: boot_static.MemoryStats) *kernel.KernelState {
    page_fault_log.init(.{
        .page_entries = boot_static.page_entries,
        .page_addr_mask = boot_static.page_addr_mask,
        .page_present = boot_static.page_present,
        .page_ps = boot_static.page_ps,
        .kernel_state_ready = &kernel_runtime.kernel_state_ready,
        .state = kernel_runtime.kernel_state_global,
        .write = kernel_log.write,
        .write_hex_raw = kernel_log.writeHexRaw,
        .write_bool01 = kernel_log.writeBool01,
    });

    kernel_runtime.kernel_state_global.initFromDetectedRegionsInPlace(memory_stats.detected_regions) catch |err| {
        halt.haltWithError("region init failed: ", err);
    };
    const state = kernel_runtime.kernel_state_global;
    state.pool_allocator = @import("../memory/identity_allocator.zig").allocator(kernel_runtime.global_free_list);
    kernel_runtime.global_free_list.reclaimable.register(&state.wait_storage_cache);
    kernel_runtime.global_free_list.reclaimable.fd_retired = &state.fd_retired;
    state.debug_process_lifecycle_hook = null;
    state.zero_physical_page_hook = user_copy.zeroPhysicalPage;

    return state;
}

// ---------------------------------------------------------------------------
// Group 3 — boot process construction
// ---------------------------------------------------------------------------

// Kernel image ownership and CR3/TLB protection cannot be delegated to
// userland: the first user dispatch must observe the completed transfer.
const ScratchRetirer = struct {
    const backing = @import("../arch/x86_64/boot_direct_map.zig");
    // A typed view retains individually owned PFNs; it is never flattened to
    // one physical extent merely because the scratch VA is contiguous.
    pub fn physical(_: *kernel.FreePageList, va: usize, bytes: usize) !backing.ScratchRetirement {
        return backing.prepareScratchRetirement(va, bytes);
    }
    pub fn withdraw(_: *kernel.FreePageList, va: usize, bytes: usize) !void {
        try backing.withdrawScratch(va, bytes);
    }
    pub fn acknowledge(_: *kernel.FreePageList, va: usize, bytes: usize) !void {
        // APs share [511], but no user PCID has dispatched yet. All CPU ACKs
        // precede any return, including CPUs booted via the temporary AP root.
        user_copy.flushTlbForCr3Range(x86_platform.kernel_cr3_value, va, bytes);
        try backing.acknowledgeScratch(va, bytes);
    }
    pub fn release(list: *kernel.FreePageList, owned: backing.ScratchRetirement, pages: usize) !void {
        try backing.releaseScratch(list, owned, pages);
    }
};

fn reclaimBootScratchOrHalt() void {
    const list = kernel_runtime.global_free_list;
    const before = list.pageCount();
    boot_scratch.reclaim(ScratchRetirer, list) catch |err|
        halt.haltWithError("boot scratch reclaim failed: ", err);
    const stats = boot_scratch.statistics();
    // Keep the unmapped numeric VA for comparison with a later fault CR2;
    // never inspect the poisoned bytes after PMM has taken ownership.
    kernel_log.writeFmt("boot scratch: tail_va=0x{x} used={} high_water={} payload={} retained={} reclaimed={} free_before={} free_after={}\n", .{
        stats.tail_address, stats.used, stats.high_water, stats.persistent_payload, stats.retained, stats.tail_bytes, before, list.pageCount(),
    });
}

// Boot is the only process writer and AP user dispatch is still disabled.
// Selecting the PFNs here makes the high-RAM test independent of allocation
// pressure or userland policy; it exercises the same VMO/COW commit machinery.
fn checkHighRam(state: *kernel.KernelState, principal: kernel.PrincipalId) !void {
    const list = kernel_runtime.global_free_list;
    const high = list.popContiguousAtOrAbove(2, @as(u64, 16) << 30) catch |err| switch (err) {
        error.OutOfFreePages => return,
        else => return err,
    };
    const copy = high + 4096;
    const layout = @import("../arch/x86_64/physical_layout.zig");
    const source = layout.identitySlice(u64, .fromInt(high), 512) orelse return error.InvalidState;
    const target = layout.identitySlice(u64, .fromInt(copy), 512) orelse return error.InvalidState;
    if (@intFromPtr(source.ptr) != @import("../arch/x86_64/direct_map.zig").base + high) return error.InvalidState;
    const observed: [*]volatile u64 = @ptrCast(source.ptr);
    const copied: [*]volatile u64 = @ptrCast(target.ptr);
    for (0..512) |i| observed[i] = high ^ (@as(u64, i) * 0x10001);
    for (0..512) |i| if (observed[i] != high ^ (@as(u64, i) * 0x10001)) return error.InvalidState;
    @memcpy(target, source);
    for (0..512) |i| if (copied[i] != observed[i]) return error.InvalidState;
    @memset(target, 0);
    for (0..512) |i| if (copied[i] != 0) return error.InvalidState;
    if (!user_copy.copyPhysicalPage(copy, high)) return error.InvalidState;
    for (0..512) |i| if (copied[i] != observed[i]) return error.InvalidState;
    if (!user_copy.zeroPhysicalPage(copy)) return error.InvalidState;
    for (0..512) |i| if (copied[i] != 0) return error.InvalidState;
    const fd = try state.createAnonymousVmoFd(principal, 4096, .{ .map_read = true, .map_write = true, .close = true }, .{}, 16);
    const ref = state.nativeVmoRefForFd(principal, fd) orelse return error.InvalidState;
    try state.installNativeVmoPages(ref, 0, &.{high}, list);
    const va: u64 = 0x4000_0000;
    _ = try state.mmapFd(principal, fd, va, 4096, .{ .read = true, .write = true }, .{ .private = true }, 0);
    if (!user_vm.mapLazyUserPaddrsWithProt(principal, va, &.{high}, .{ .read = true })) return error.InvalidState;
    for (0..512) |i| {
        if ((user_copy.readUserU64(principal, va + i * 8) orelse return error.InvalidState) != observed[i]) return error.InvalidState;
    }
    const plan = state.prepareNativeVmaCowMapping(principal, va, true, false);
    if (plan.kind != .allocate_copy or plan.source_paddr != high) return error.InvalidState;
    if (!kernel.KernelState.copyPhysicalPage(copy, plan.source_paddr)) return error.InvalidState;
    const mapping = state.commitNativeVmaCowMapping(principal, plan, copy, list) orelse return error.InvalidState;
    if (mapping.paddr != copy or !mapping.prot.write) return error.InvalidState;
    // Withdraw and acknowledge the shared source before publishing the private
    // destination. Data backing remains owned by the FD/COW table throughout.
    if (!user_vm.unmapPresentUserLinearRegion(principal, va, 4096)) return error.InvalidState;
    if (!user_vm.mapNativeVmaFaultPage(principal, va, mapping)) return error.InvalidState;
    const changed: u64 = 0xdecaf_bad0_1234_567;
    if (!user_copy.writeUserU64(principal, va, changed)) return error.InvalidState;
    if (copied[0] != changed or observed[0] != high) return error.InvalidState;
    if (!user_vm.unmapPresentUserLinearRegion(principal, va, 4096)) return error.InvalidState;
    try state.munmapRangeWithFreeList(principal, va, 4096, list);
    try state.closeFdWithFreeList(principal, fd, list);
    const reused = try list.popContiguousInRange(2, high, high + 8192);
    if (reused != high) return error.InvalidState;
    observed[0] = changed;
    copied[511] = changed;
    if (observed[0] != changed or copied[511] != changed) return error.InvalidState;
    if (!user_copy.zeroPhysicalPage(high) or !user_copy.zeroPhysicalPage(copy)) return error.InvalidState;
    try list.appendContiguousRange(high, 2);
    kernel_log.writeFmt("high-ram: pa=0x{x} copy=0x{x} direct-write-read=PASS zero=PASS copy=PASS user=PASS COW=PASS return-reuse=PASS\n", .{ high, copy });
}

fn constructBootProcesses(state: *kernel.KernelState, res: PreparedBootResources) void {
    const init_principal = state.createProcessDescriptor("seed2_boot") orelse
        halt.haltWithMessage("seed2_boot process descriptor alloc failed");
    state.setBootstrapOwner(init_principal, true) catch |err| {
        halt.haltWithError("init bootstrap owner mark failed: ", err);
    };
    boot_init_principal = init_principal;
    const init_process = process_factory.createUserProcess(state, init_principal, "init", kernel_runtime.global_free_list, &user_space_table);
    checkHighRam(state, init_principal) catch |err| halt.haltWithError("high RAM self-check failed: ", err);
    state.createSerialFdAt(init_principal, 1, 1) catch |err| {
        halt.haltWithError("init stdout fd install failed: ", err);
    };
    state.createSerialFdAt(init_principal, 2, 2) catch |err| {
        halt.haltWithError("init stderr fd install failed: ", err);
    };
    var bootfs_loan: boot_scratch.Borrow = .{};
    res.bootfs_staging.borrowInto(&bootfs_loan) catch halt.haltWithMessage("bootfs setup borrow rejected");
    const staged_pages = std.mem.bytesAsSlice(resource_pages.StagedPage, bootfs_loan.bytes() catch halt.haltWithMessage("bootfs setup view rejected"));
    // setup's fatal failures retain boot ownership; they do not unwind a
    // caller's defer. Successful VMO copying ends this loan explicitly below.
    init_setup.setupInitBootstrapResources(
        state,
        init_principal,
        staged_pages,
        res.bootfs_size,
        res.framebuffer_info,
        kernel_runtime.global_free_list,
    );
    // The init VMO now owns independent copies. Release staged RAM before
    // the init ELF load window reuses this scratch metadata.
    // Keep ALL records alive until every return succeeds. A fatal partial
    // failure retains metadata with released and still-owned slots distinct.
    for (staged_pages) |*slot| {
        slot.release() catch
            halt.haltWithMessage("bootfs staging page release failed");
    }
    bootfs_loan.end() catch halt.haltWithMessage("bootfs setup borrow end rejected");
    res.bootfs_staging.release() catch halt.haltWithMessage("bootfs metadata release rejected");
    scheduler.scheduler_tick_accum = 0;
    scheduler.scheduler_switch_count = 0;
    init_setup.refreshInitBootLogSnapshot(state, init_principal);

    var init_loan: boot_scratch.Borrow = .{};
    res.init_staging.borrowInto(&init_loan) catch halt.haltWithMessage("init ELF borrow rejected");
    // Use the fallible loader: its nested window defer must finish, then our
    // input loan/storage must end BEFORE a fatal null result stops the CPU.
    const load_result = elf_load.loadUserElfIntoProcessPages(
        state,
        init_principal,
        init_process.user_page.paddr,
        init_process.user_stack_page.paddr,
        init_loan.bytes() catch halt.haltWithMessage("init ELF view rejected"),
        kernel_runtime.global_free_list,
    );
    init_loan.end() catch halt.haltWithMessage("init ELF borrow end rejected");
    res.init_staging.release() catch halt.haltWithMessage("init ELF storage release rejected");
    // Image contains values, not references to the now-reclaimable ELF bytes.
    const loaded_init = load_result orelse {
        kernel_log.write("init ELF load failed\n");
        halt.haltLoop();
    };
    // ELF and bootfs staging are released before this long-lived catalog.
    if (!pci.captureBootFunctions())
        halt.haltWithMessage("PCI function catalog capture failed");
    boot_scratch.seal() catch halt.haltWithMessage("boot scratch has unfinished staging");
    reclaimBootScratchOrHalt();
    const init_thread = scheduler.threadForPrincipal(init_principal).?;
    const init_rsp = process_factory.installInitialUserStackOrHalt(
        state,
        init_principal,
        init_process.user_stack_page.paddr,
        loaded_init,
        "init",
    );
    if (!scheduler.setBootstrapThreadEntry(init_thread, init_principal, .fromInt(loaded_init.entry), .fromInt(init_rsp)))
        halt.haltWithMessage("bootstrap thread entry publication rejected");
    if (!scheduler.activateNextReadyOnCurrentCpu()) {
        halt.haltWithMessage("kernel scheduler failed to activate init");
    }

    @atomicStore(bool, &kernel_runtime.kernel_state_ready, true, .release);
    // Boot threads are already visible to the verified runqueues at this
    // point.  APs must not claim one until its ELF, stack, kernel state, and
    // initial BSP activation are all complete.
    scheduler.enableApUserDispatch();
}

// ---------------------------------------------------------------------------
// Group 2b — kernel subsystem wiring (post-process, after kernel_state_ready)
// ---------------------------------------------------------------------------

fn wireRuntimeSubsystems(state: *kernel.KernelState, memory_stats: boot_static.MemoryStats) void {
    syscalls.init(.{
        .state = state,
        .free_list = kernel_runtime.global_free_list,
        .user_spaces = &user_space_table,
        .kernel_state_ready = &kernel_runtime.kernel_state_ready,
        .write = kernel_log.write,
        .write_user = kernel_log.writeUser,
        .print_hex = log_util.printHex,
        .print_number = log_util.printNumberU64,
        .principal_label = kernel_runtime.principalLabel,
        .read_user_u64 = user_copy.readUserU64,
        .write_user_u64 = user_copy.writeUserU64,
        .copy_user_bytes_from_va = user_copy.copyUserBytesFromVa,
        .copy_bytes_to_user_va = user_copy.copyBytesToUserVa,
        .probe_user_range = user_copy.probeUserRange,
        .resident_read_user_u32 = user_copy.readUserU32Resident,
        .futex_lock_address_space = user_vm.lockAddressSpace,
        .futex_unlock_address_space = user_vm.unlockAddressSpace,
        .resident_read_user_u32_with_address_space_locked = user_copy.readUserU32ResidentWithAddressSpaceLocked,
        .resident_atomic_user_u32_with_address_space_locked = user_copy.atomicUserU32ResidentWithAddressSpaceLocked,
        .wake_waiting_thread_for_principal = scheduler.wakeMailboxWaiter,
        .thread_wake_target_is_live = scheduler.threadWakeTargetIsLive,
        .wake_waiting_thread_generation = scheduler.wakeIfWaitingGeneration,
        .wake_waiting_thread_generation_with_rax = scheduler.wakeIfWaitingGenerationWithRax,
        .wake_waiting_thread_generation_with_rax_prefer_current = scheduler.wakeIfWaitingGenerationWithRaxPreferCurrent,
        .wake_blocked_thread_for_principal = scheduler.wakeBlockedThread,
        .block_current_thread_for_event = scheduler.blockCurrentThread,
        .exit_current_process = exitCurrentProcess,
        .total_usable_memory_bytes = memory_stats.total_usable_bytes,
    });
}

pub fn prepareBootPrelude() void {
    asm volatile ("cli");
    xstate_runtime.initXStateSupport();
    lapic.maskLegacyPic();
    kernel_log.reset();
    serial.init();
    pci.clearInterruptRoutes();
    boot_init_principal = null;
}

pub fn bootWithResources(resources: PreparedBootResources, comptime checkpoint: fn (u8) void) noreturn {
    kernel_log.writeOnly("boot: bootWithResources entry\n");
    kernel_log.write("boot: init subsystems\n");
    const state = initKernelSubsystems(resources.memory_stats);
    const highres_ready = @import("../realtime_clock.zig").initializeHighResolution(
        boot_rsdp_paddr,
        struct {
            fn allowed(base: u64, bytes: u64) bool {
                const status = smp.explicitUcMmioStatus(base, bytes);
                if (status != .ready) {
                    const masks = smp.mmioCacheEvidenceCounts();
                    kernel_log.writeFmt("clock: HPET UC proof failed reason={s} base=0x{x} bytes=0x{x} verified={} matching={} online={}\n", .{
                        @tagName(status), base, bytes, masks.verified, masks.matching, masks.online,
                    });
                }
                return status == .ready;
            }
        }.allowed,
    );
    // APs are idle and user dispatch has not started. HPET UC proof is now
    // complete, so calibration can use an independent bounded deadline.
    const timer_calibration = lapic.calibrateTimer(
        boot_static.lapic_timer_initial_count,
        boot_static.lapic_timer_rearm_overhead_ns,
    );
    if (timer_calibration.calibrated) {
        kernel_log.writeFmt(
            "lapic-timer: calibration=pit frequency_hz={} initial_count={} rearm_overhead_ns={} pit_ticks={}\n",
            .{
                timer_calibration.frequency_hz,
                timer_calibration.initial_count,
                timer_calibration.rearm_overhead_ns,
                timer_calibration.pit_ticks,
            },
        );
    } else {
        kernel_log.writeFmt(
            "lapic-timer: calibration=fallback reason={s} frequency_hz=unknown initial_count={} rearm_overhead_ns={}\n",
            .{ @tagName(timer_calibration.reason), timer_calibration.initial_count, timer_calibration.rearm_overhead_ns },
        );
    }
    smp.configureApUserTimer(
        boot_static.lapic_timer_vector,
        lapic.timerInitialCount(boot_static.lapic_timer_initial_count) * @as(u32, @intCast(boot_static.scheduler_slice_ticks)),
    );
    const clock = @import("../realtime_clock.zig");
    const hpet = @import("../hpet.zig").diagnostic();
    kernel_log.writeFmt("clock: HPET init={s} base=0x{x} caps=0x{x} period_fs={} lapic-calibrated={}\n", .{
        @tagName(hpet.reason),                     hpet.address, hpet.capabilities, hpet.period_fs,
        @intFromBool(lapic.highResolutionReady()),
    });
    kernel_log.writeFmt("clock: pvclock stable-cpus={} online-cpus={}\n", .{ clock.stableCpuCount(), smp.onlineCpuSet().count() });
    if (highres_ready) {
        kernel_log.writeFmt("clock: monotonic={s} deadline={s} high-resolution=1\n", .{
            if (clock.usesStableKvmClock()) "kvm" else "hpet",
            if (lapic.highResolutionReady()) "lapic" else "periodic-fallback",
        });
    } else {
        kernel_log.writeFmt("clock: monotonic=tick high-resolution=0 bsp-tsc-hz={} rearm=expiry-only\n", .{
            clock.bootstrapCounterFrequencyHz(),
        });
    }
    checkpoint('E');
    iommu.init(boot_rsdp_paddr, kernel_runtime.global_free_list, checkpoint);
    platform_device.initialize();
    x86_platform.sealKernelMmioWindow();
    checkpoint('F');
    kernel_log.write("boot: construct processes\n");
    constructBootProcesses(state, resources);
    checkpoint('G');
    kernel_log.write("boot: wire runtime\n");
    wireRuntimeSubsystems(state, resources.memory_stats);

    var user_entry: UserVirtualAddress = undefined;
    var user_stack: UserVirtualAddress = undefined;
    if (!scheduler.currentUserEntrySnapshot(&user_entry, &user_stack))
        halt.haltWithMessage("bootstrap current thread snapshot unavailable");
    kernel_log.write("boot: enter user\n");
    checkpoint('H');
    // The isolated diagnostic ELF has no serial cable on the target machine.
    // Mirror subsequent existing log lines until userland owns the GOP, so
    // an early user fault and a late console failure are distinguishable.
    boot_diag.startPostUserLogMirror();
    enterUserModeIretq(user_entry.toInt(), user_stack.toInt());
}

pub fn installPmmBootAccess(offset: u64) void {
    @import("../memory/free_page_list.zig").boot_view =
        @import("../arch/x86_64/boot_hhdm.zig").current(offset) orelse
        @import("../halt.zig").haltWithMessage("PMM boot HHDM view unavailable");
}

// BSP-only initialization before APs can read the immutable copy policy.
extern fn kernel_memory_init() callconv(.c) void;

// The caller retains AS, hence every matching VMA's VMO lifetime. Retaining
// evidence is atomic and never enters K/shared-VM beneath the AS lock.
fn retainRetiredPageViewWrites(principal: kernel.PrincipalId, start: u64, bytes: u64) void {
    if (!@atomicLoad(bool, &kernel_runtime.kernel_state_ready, .acquire)) return;
    @import("../state/page_view_dirty.zig").markRetiredAliases(kernel_runtime.kernel_state_global, principal, start, bytes);
}
