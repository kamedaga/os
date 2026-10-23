const std = @import("std");

fn addVerifiedSchedulerHostObject(
    b: *std.Build,
    mod: *std.Build.Module,
    source: []const u8,
    basename: []const u8,
) void {
    const clang_path = freestandingClang(b);
    const clang = b.addSystemCommand(&.{
        clang_path,
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-O2",
        "-g0",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I../verified/scheduling/include",
        "-c",
        source,
        "-o",
    });
    clang.addFileInput(b.path(source));
    clang.addFileInput(b.path("../verified/scheduling/include/pacha_eevdf.h"));
    const object = clang.addOutputFileArg(basename);
    mod.addObjectFile(object);
}

fn addVerifiedSchedulerElfObject(
    b: *std.Build,
    mod: *std.Build.Module,
    source: []const u8,
    basename: []const u8,
    checked: bool,
) void {
    const clang_path = freestandingClang(b);
    const clang = b.addSystemCommand(&.{
        clang_path,
        "-target",
        "x86_64-unknown-none-elf",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-fno-exceptions",
        "-mno-red-zone",
        "-mgeneral-regs-only",
        "-O2",
        "-g0",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I../verified/scheduling/include",
        "-c",
        source,
        "-o",
    });
    clang.addFileInput(b.path(source));
    clang.addFileInput(b.path("../verified/scheduling/include/pacha_eevdf.h"));
    const object = clang.addOutputFileArg(basename);
    clang.addArg(if (checked) "-DPACHA_EEVDF_CHECKED=1" else "-DPACHA_EEVDF_CHECKED=0");
    mod.addObjectFile(object);
    const memory = b.addSystemCommand(&.{
        clang_path,     "-target",              "x86_64-unknown-none-elf", "-ffreestanding",
        "-fno-builtin", "-fno-stack-protector", "-mno-red-zone",           "-mgeneral-regs-only",
        "-O2",          "-Wall",                "-Wextra",                 "-Werror",
        "-c",
    });
    memory.addFileArg(b.path("src/arch/x86_64/memory.c"));
    memory.addArg("-o");
    mod.addObjectFile(memory.addOutputFileArg("kernel_memory.o"));
}

fn freestandingClang(b: *std.Build) []const u8 {
    return b.graph.environ_map.get("CAPOS_UNWRAPPED_CLANG") orelse
        (b.graph.environ_map.get("CAPOS_FREESTANDING_CC") orelse "clang");
}

pub fn build(b: *std.Build) void {
    const scheduler_test_filter = b.option([]const u8, "scheduler-test-filter", "Select scheduler host regressions");
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const limine_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .freestanding,
        .abi = .none,
        // Live-XState IPC shares locks/scheduler callbacks with the rest of
        // the kernel. Restrict all linked code, then audit the final ELF so
        // indirect calls and compiler-generated copies cannot evade the gate.
        .cpu_features_sub = std.Target.x86.featureSet(&.{ .mmx, .sse, .sse2, .avx, .avx2 }),
        .cpu_features_add = std.Target.x86.featureSet(&.{.soft_float}),
    });
    const kernel_abi_root_mod = b.createModule(.{
        .root_source_file = b.path("abi/kernel_abi_root.zig"),
    });
    const kernel_mod = b.createModule(.{
        .root_source_file = b.path("src/kernel.zig"),
        .target = target,
        .optimize = optimize,
    });
    kernel_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, kernel_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf.o");

    const test_mod = b.createModule(.{
        .root_source_file = b.path("../tests/kernel_state.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_mod.addImport("kernel", kernel_mod);
    test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const unit_tests = b.addTest(.{
        .filters = if (b.option([]const u8, "state-test-filter", "Select a kernel state lifecycle test")) |filter| &.{filter} else &.{},
        .root_module = test_mod,
    });
    unit_tests.stack_size = 512 * 1024 * 1024;

    const run_unit_tests = b.addRunArtifact(unit_tests);
    const test_step = b.step("test", "Run kernel unit tests");
    const memory_test = b.addSystemCommand(&.{"bash"});
    memory_test.addFileArg(b.path("../tests/run-kernel-memory-unit.sh"));
    test_step.dependOn(&memory_test.step);
    test_step.dependOn(&run_unit_tests.step);
    const tlb_protocol_test = b.addSystemCommand(&.{"python3"});
    tlb_protocol_test.addFileArg(b.path("../tests/test_tlb_lazy_protocol.py"));
    test_step.dependOn(&tlb_protocol_test.step);
    b.step("test-state", "Run kernel state lifecycle tests").dependOn(&run_unit_tests.step);

    const address_boundary_check = b.addSystemCommand(&.{"python3"});
    address_boundary_check.addFileArg(b.path("../tests/check_kernel_address_boundaries.py"));
    address_boundary_check.addArg(b.graph.zig_exe);
    test_step.dependOn(&address_boundary_check.step);
    const ipc_register_check = b.addSystemCommand(&.{"python3"});
    ipc_register_check.addFileArg(b.path("../tests/check_ipc_register_abi.py"));
    test_step.dependOn(&ipc_register_check.step);
    const wait_boundary_check = b.addSystemCommand(&.{"python3"});
    wait_boundary_check.addFileArg(b.path("../tests/check_uninterruptible_wait_contract.py"));
    test_step.dependOn(&wait_boundary_check.step);
    const storage_boundary_check = b.addSystemCommand(&.{"python3"});
    storage_boundary_check.addFileArg(b.path("../tests/check_kernel_storage_boundaries.py"));
    test_step.dependOn(&storage_boundary_check.step);
    const ipc_cost_boundary_check = b.addSystemCommand(&.{"python3"});
    ipc_cost_boundary_check.addFileArg(b.path("../tests/check_ipc_fast_cost_boundaries.py"));
    test_step.dependOn(&ipc_cost_boundary_check.step);
    const ipc_endpoint_boundary_check = b.addSystemCommand(&.{"python3"});
    ipc_endpoint_boundary_check.addFileArg(b.path("../tests/check_ipc_endpoint_boundaries.py"));
    test_step.dependOn(&ipc_endpoint_boundary_check.step);
    const fd_capability_boundary_check = b.addSystemCommand(&.{"python3"});
    fd_capability_boundary_check.addFileArg(b.path("../tests/check_fd_capability_boundaries.py"));
    test_step.dependOn(&fd_capability_boundary_check.step);
    const scratch_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/boot/boot_scratch.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    }) });
    test_step.dependOn(&b.addRunArtifact(scratch_tests).step);
    const staged_page_mod = b.createModule(.{
        .root_source_file = b.path("src/boot_resource_pages_test.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    });
    staged_page_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const staged_page_tests = b.addTest(.{ .root_module = staged_page_mod, .filters = &.{"staged page"} });
    test_step.dependOn(&b.addRunArtifact(staged_page_tests).step);
    const free_page_list_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/free_page_list_test.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    }), .filters = &.{"free page list"} });
    test_step.dependOn(&b.addRunArtifact(free_page_list_tests).step);
    const reference_mod = b.createModule(.{ .root_source_file = b.path("src/reference_index_test.zig"), .target = b.graph.host, .optimize = .Debug });
    reference_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const reference_run = b.addRunArtifact(b.addTest(.{ .root_module = reference_mod }));
    test_step.dependOn(&reference_run.step);
    b.step("test-reference-index", "Test reference reservations and ownership negatives").dependOn(&reference_run.step);
    const vmo_storage_mod = b.createModule(.{
        .root_source_file = b.path("src/vmo_backing_storage_test.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    });
    vmo_storage_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const vmo_storage_tests = b.addTest(.{ .root_module = vmo_storage_mod, .filters = &.{"VMO radix"} });
    const vmo_storage_run = b.addRunArtifact(vmo_storage_tests);
    test_step.dependOn(&vmo_storage_run.step);
    b.step("test-vmo-storage", "Test the private VMO metadata lifecycle").dependOn(&vmo_storage_run.step);
    const lock_profile_mod = b.createModule(.{ .root_source_file = b.path("src/lock_profile_test.zig"), .target = b.graph.host, .optimize = .Debug });
    lock_profile_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    // The checked CPU-capacity path reaches platform code; LLVM supports its
    // PKRU instructions, which the host self-hosted x86 encoder cannot emit.
    const lock_profile_run = b.addRunArtifact(b.addTest(.{ .root_module = lock_profile_mod, .use_llvm = true, .filters = &.{ "profile epoch", "histogram" } }));
    test_step.dependOn(&lock_profile_run.step);
    b.step("test-lock-profile", "Test profile epoch, attribution and distributions").dependOn(&lock_profile_run.step);
    const lock_order_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/sync/lock_order.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    }) });
    test_step.dependOn(&b.addRunArtifact(lock_order_tests).step);
    const notify_wake_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/sync/notify_wake.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    }) });
    test_step.dependOn(&b.addRunArtifact(notify_wake_tests).step);
    const ipc_lifetime_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/sync/ipc_lifetime.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    }) });
    const ipc_lifetime_run = b.addRunArtifact(ipc_lifetime_tests);
    test_step.dependOn(&ipc_lifetime_run.step);
    b.step("test-ipc-lifetime", "Test IPC snapshot grace periods and live pins").dependOn(&ipc_lifetime_run.step);
    const bsp_cpu_identity_mod = b.createModule(.{
        .root_source_file = b.path("src/cpu_stack_identity_test.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    });
    bsp_cpu_identity_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const bsp_cpu_identity_tests = b.addTest(.{ .root_module = bsp_cpu_identity_mod, .filters = &.{ "BSP stack CPU identity", "BSP stack painting" } });
    const bsp_cpu_identity_run = b.addRunArtifact(bsp_cpu_identity_tests);
    test_step.dependOn(&bsp_cpu_identity_run.step);
    const scratch_boundary_check = b.addSystemCommand(&.{"python3"});
    scratch_boundary_check.addFileArg(b.path("../tests/check_kernel_boot_scratch_boundaries.py"));
    test_step.dependOn(&scratch_boundary_check.step);
    const scratch_snapshot_tests = b.addSystemCommand(&.{"python3"});
    scratch_snapshot_tests.addFileArg(b.path("../tests/test_kernel_boot_scratch_snapshot.py"));
    test_step.dependOn(&scratch_snapshot_tests.step);
    const kernel_log_test_mod = b.createModule(.{
        .root_source_file = b.path("src/kernel_log.zig"),
        .target = target,
        .optimize = optimize,
    });
    kernel_log_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const kernel_log_tests = b.addTest(.{ .root_module = kernel_log_test_mod });
    test_step.dependOn(&b.addRunArtifact(kernel_log_tests).step);
    const clock_copy_mod = b.createModule(.{
        .root_source_file = b.path("src/runtime_clock_copy_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    clock_copy_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const clock_copy_tests = b.addTest(.{ .root_module = clock_copy_mod, .filters = &.{ "timespec copyout", "yield result", "clock read register" } });
    const run_clock_copy = b.addRunArtifact(clock_copy_tests);
    test_step.dependOn(&run_clock_copy.step);
    b.step("test-runtime-clock", "Test checked timespec copyout").dependOn(&run_clock_copy.step);
    const ipc_metric_mod = b.createModule(.{
        .root_source_file = b.path("src/ipc_metric_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    ipc_metric_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const ipc_metric_tests = b.addTest(.{ .root_module = ipc_metric_mod, .filters = &.{"principal syscall counts"} });
    const run_ipc_metric = b.addRunArtifact(ipc_metric_tests);
    test_step.dependOn(&run_ipc_metric.step);
    b.step("test-ipc-metric", "Test bounded diagnostic syscall counts").dependOn(&run_ipc_metric.step);
    const mmio_overlay_mod = b.createModule(.{
        .root_source_file = b.path("src/mmio_overlay_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    mmio_overlay_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, mmio_overlay_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf.o");
    // Real syscall dispatch links x86 trap assembly; the self-hosted backend
    // cannot encode it. Host fixtures fail if those privileged entries execute.
    const vm_test_filter = b.option([]const u8, "vm-test-filter", "Filter native VM host tests");
    const mmio_overlay_tests = b.addTest(.{ .root_module = mmio_overlay_mod, .filters = if (vm_test_filter) |filter| &.{filter} else &.{ "MMIO overlay", "VM Hooks reject", "RAM cache source", "RAM cache preparation", "CPU membership", "MADT", "resident roots", "both registration", "zero retention", "CPU storage preserves", "shootdown mailbox", "generated syscall", "direct syscall entry", "direct IPC kinds" }, .use_llvm = true });
    mmio_overlay_tests.stack_size = 64 * 1024 * 1024;
    const run_mmio_overlay = b.addRunArtifact(mmio_overlay_tests);
    test_step.dependOn(&run_mmio_overlay.step);
    b.step("test-ram-cache-plan", "Test native VM and RAM-cache page-table transactions").dependOn(&run_mmio_overlay.step);
    const ram_cache_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/memory/ram_cache_policy.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    }) });
    test_step.dependOn(&b.addRunArtifact(ram_cache_tests).step);
    const direct_map_mod = b.createModule(.{
        .root_source_file = b.path("src/direct_map_test.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    });
    direct_map_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, direct_map_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf.o");
    // Scratch retirement exercises the real PMM owner, whose linked privileged
    // stubs require LLVM even though host fixtures never execute those stubs.
    const direct_map_tests = b.addTest(.{ .root_module = direct_map_mod, .use_llvm = true, .filters = &.{ "direct map", "scratch prepares", "scratch direct", "image sections", "staged page HHDM", "identity split" } });
    const direct_map_run = b.addRunArtifact(direct_map_tests);
    test_step.dependOn(&direct_map_run.step);
    b.step("test-direct-map", "Test detached boot direct maps and large-page splitting").dependOn(&direct_map_run.step);
    const ram_cache_paging_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/memory/ram_cache_paging.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    }) });
    test_step.dependOn(&b.addRunArtifact(ram_cache_paging_tests).step);
    const interrupt_waiter_mod = b.createModule(.{
        .root_source_file = b.path("src/syscalls.zig"),
        .target = target,
        .optimize = optimize,
    });
    interrupt_waiter_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    interrupt_waiter_mod.addCSourceFile(.{ .file = b.path("../tests/interrupt_waiter_host.c"), .flags = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror" } });
    addVerifiedSchedulerHostObject(b, interrupt_waiter_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_interrupt_test.o");
    const interrupt_waiter_tests = b.addTest(.{ .root_module = interrupt_waiter_mod, .filters = &.{ "shootdown interrupt guard", "lifecycle admission", "process timer index", "PIT deadline", "copy window", "poll item snapshot", "MMIO cache admission", "WC identity lease", "WC physical window leases" }, .use_llvm = true });
    interrupt_waiter_tests.stack_size = 512 * 1024 * 1024;
    const interrupt_waiter_run = b.addRunArtifact(interrupt_waiter_tests);
    test_step.dependOn(&interrupt_waiter_run.step);
    b.step("test-process-lifetime", "Run process timer and lifecycle IRQ host regressions").dependOn(&interrupt_waiter_run.step);
    const realtime_clock_mod = b.createModule(.{
        .root_source_file = b.path("src/realtime_clock.zig"),
        .target = target,
        .optimize = optimize,
    });
    // The clock page layout is checked against the runtime ABI at comptime.
    realtime_clock_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const realtime_clock_tests = b.addTest(.{ .root_module = realtime_clock_mod });
    test_step.dependOn(&b.addRunArtifact(realtime_clock_tests).step);
    for ([_][]const u8{ "src/hpet.zig", "src/lapic.zig", "src/clockevent.zig", "src/acpi_tables.zig", "src/acpi_ivrs.zig", "src/acpi_dmar.zig", "src/elf_loader.zig", "src/pci.zig" }) |source| {
        const clock_test_mod = b.createModule(.{
            .root_source_file = b.path(source),
            .target = target,
            .optimize = optimize,
        });
        clock_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
        test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = clock_test_mod })).step);
    }
    const vtd_test_mod = b.createModule(.{
        .root_source_file = b.path("src/vtd.zig"),
        .target = target,
        .optimize = optimize,
    });
    vtd_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, vtd_test_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_vtd_test.o");
    const vtd_tests = b.addTest(.{ .root_module = vtd_test_mod, .filters = &.{"VT-d"} });
    vtd_tests.stack_size = 512 * 1024 * 1024;
    test_step.dependOn(&b.addRunArtifact(vtd_tests).step);
    const amd_iommu_test_mod = b.createModule(.{
        .root_source_file = b.path("src/amd_iommu.zig"),
        .target = target,
        .optimize = optimize,
    });
    amd_iommu_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, amd_iommu_test_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_amd_iommu_test.o");
    const amd_iommu_tests = b.addTest(.{ .root_module = amd_iommu_test_mod, .filters = &.{"AMD IOMMU"} });
    amd_iommu_tests.stack_size = 512 * 1024 * 1024;
    test_step.dependOn(&b.addRunArtifact(amd_iommu_tests).step);
    const iommu_test_mod = b.createModule(.{
        .root_source_file = b.path("src/iommu.zig"),
        .target = target,
        .optimize = optimize,
    });
    iommu_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const iommu_tests = b.addTest(.{ .root_module = iommu_test_mod, .filters = &.{"IOMMU boundary"} });
    iommu_tests.stack_size = 512 * 1024 * 1024;
    test_step.dependOn(&b.addRunArtifact(iommu_tests).step);
    const physical_layout_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/physical_layout_test.zig"),
        .target = target,
        .optimize = optimize,
    }) });
    test_step.dependOn(&b.addRunArtifact(physical_layout_tests).step);
    const owned_storage_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/owned_storage_test.zig"),
        .target = target,
        .optimize = optimize,
    }) });
    test_step.dependOn(&b.addRunArtifact(owned_storage_tests).step);
    const cpu_stack_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/arch/x86_64/cpu_stack_layout.zig"),
        .target = target,
        .optimize = optimize,
    }) });
    test_step.dependOn(&b.addRunArtifact(cpu_stack_tests).step);

    const fd_ipc_minimal_mod = b.createModule(.{
        .root_source_file = b.path("../tests/fd_ipc_minimal.zig"),
        .target = target,
        .optimize = optimize,
    });
    fd_ipc_minimal_mod.addImport("kernel", kernel_mod);
    fd_ipc_minimal_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const fd_ipc_minimal_tests = b.addTest(.{
        .root_module = fd_ipc_minimal_mod,
    });
    fd_ipc_minimal_tests.stack_size = 512 * 1024 * 1024;

    const run_fd_ipc_minimal_tests = b.addRunArtifact(fd_ipc_minimal_tests);
    test_step.dependOn(&run_fd_ipc_minimal_tests.step);

    const scheduler_runqueue_test_mod = b.createModule(.{
        .root_source_file = b.path("src/scheduler_runqueue.zig"),
        .target = target,
        .optimize = optimize,
    });
    addVerifiedSchedulerHostObject(b, scheduler_runqueue_test_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_runqueue_test.o");
    const scheduler_runqueue_tests = b.addTest(.{
        .root_module = scheduler_runqueue_test_mod,
    });
    const run_scheduler_runqueue_tests = b.addRunArtifact(scheduler_runqueue_tests);
    test_step.dependOn(&run_scheduler_runqueue_tests.step);

    const scheduler_context_test_mod = b.createModule(.{
        .root_source_file = b.path("src/scheduler_connection.zig"),
        .target = target,
        .optimize = optimize,
    });
    scheduler_context_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, scheduler_context_test_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_context_test.o");
    const scheduler_context_tests = b.addTest(.{
        .root_module = scheduler_context_test_mod,
        // Handoff negative tests reach the PKRU save/restore instructions;
        // Zig's self-hosted x86 backend cannot encode rdpkru/wrpkru yet.
        .use_llvm = true,
        .filters = if (scheduler_test_filter) |filter| &.{filter} else &.{ "migration waits", "preferred wake", "controlled thread context", "yield handoff", "futex", "context transfer", "IPC direct", "clock read admission", "AP block", "IPC notify", "thread storage", "uninterruptible wait", "deadline" },
    });
    test_step.dependOn(&b.addRunArtifact(scheduler_context_tests).step);
    const ipc_cost_mod = b.createModule(.{
        .root_source_file = b.path("src/scheduler_connection.zig"),
        .target = b.graph.host,
        .optimize = .ReleaseSafe,
    });
    ipc_cost_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, ipc_cost_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_cost_test.o");
    const ipc_cost_tests = b.addTest(.{ .root_module = ipc_cost_mod, .use_llvm = true, .filters = &.{"IPC host costs"} });
    b.step("test-ipc-costs", "Measure real warm queued IPC stages on the host").dependOn(&b.addRunArtifact(ipc_cost_tests).step);
    const fd_capability_mod = b.createModule(.{ .root_source_file = b.path("src/fd_capability_test.zig"), .target = b.graph.host, .optimize = .Debug });
    fd_capability_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, fd_capability_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_fd_capability_test.o");
    const fd_capability_tests = b.addTest(.{ .root_module = fd_capability_mod, .use_llvm = true, .filters = &.{"FD capability"} });
    const fd_capability_run = b.addRunArtifact(fd_capability_tests);
    test_step.dependOn(&fd_capability_run.step);
    b.step("test-fd-capability", "Test real FD capability writers and reader retirement races").dependOn(&fd_capability_run.step);
    const uninterruptible_wait_mod = b.createModule(.{
        .root_source_file = b.path("src/uninterruptible_wait_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    uninterruptible_wait_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, uninterruptible_wait_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_uninterruptible_wait_test.o");
    const uninterruptible_wait_tests = b.addTest(.{ .root_module = uninterruptible_wait_mod, .use_llvm = true, .filters = &.{"uninterruptible wait"} });
    test_step.dependOn(&b.addRunArtifact(uninterruptible_wait_tests).step);
    const ranked_scheduler_mod = b.createModule(.{
        .root_source_file = b.path("src/lock_diagnostics_scheduler_test.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    });
    ranked_scheduler_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, ranked_scheduler_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_ranked_context_test.o");
    const standard_test_runner = b.createModule(.{
        .root_source_file = .{ .cwd_relative = b.pathJoin(&.{ b.graph.zig_lib_directory.path.?, "compiler/test_runner.zig" }) },
        .target = b.graph.host,
        .optimize = .Debug,
    });
    const ipc_capacity_mod = b.createModule(.{ .root_source_file = b.path("src/ipc_capacity_test.zig"), .target = b.graph.host, .optimize = .Debug });
    ipc_capacity_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    ipc_capacity_mod.addImport("standard_test_runner", standard_test_runner);
    addVerifiedSchedulerHostObject(b, ipc_capacity_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_capacity_test.o");
    const ipc_capacity_run = b.addRunArtifact(b.addTest(.{ .root_module = ipc_capacity_mod, .use_llvm = true, .test_runner = .{ .path = b.path("src/lock_diagnostics_test_runner.zig"), .mode = .server }, .filters = &.{"channel FD4200"} }));
    test_step.dependOn(&ipc_capacity_run.step);
    b.step("test-ipc-capacity", "Test checked channel capacity under finite backing").dependOn(&ipc_capacity_run.step);
    ranked_scheduler_mod.addImport("standard_test_runner", standard_test_runner);
    const ranked_scheduler_tests = b.addTest(.{
        .root_module = ranked_scheduler_mod,
        .test_runner = .{ .path = b.path("src/lock_diagnostics_test_runner.zig"), .mode = .server },
        .use_llvm = true,
        .filters = if (scheduler_test_filter) |filter| &.{filter} else &.{ "migration waits", "preferred wake", "controlled thread context", "yield handoff", "futex", "context transfer", "IPC direct", "clock read admission", "AP block", "IPC notify", "thread storage", "uninterruptible wait", "deadline" },
    });
    const ranked_scheduler_run = b.addRunArtifact(ranked_scheduler_tests);
    test_step.dependOn(&ranked_scheduler_run.step);
    const lock_test_step = b.step("test-lock-order", "Run lock-order negatives and actual ranked scheduler paths");
    lock_test_step.dependOn(&ranked_scheduler_run.step);
    lock_test_step.dependOn(&b.addRunArtifact(lock_order_tests).step);
    lock_test_step.dependOn(&bsp_cpu_identity_run.step);
    const ranked_kernel_mod = b.createModule(.{
        .root_source_file = b.path("src/lock_diagnostics_kernel_test.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    });
    ranked_kernel_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, ranked_kernel_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_ranked_kernel_test.o");
    ranked_kernel_mod.addImport("standard_test_runner", standard_test_runner);
    const ranked_kernel_tests = b.addTest(.{
        .root_module = ranked_kernel_mod,
        .test_runner = .{ .path = b.path("src/lock_diagnostics_test_runner.zig"), .mode = .server },
        .use_llvm = true,
        .filters = &.{ "lock admission", "lock ordering", "MMIO overlay", "shootdown interrupt guard", "lifecycle admission", "process timer index", "PIT deadline", "futex fused collector", "VT-d withdrawal contract", "AMD IOMMU withdrawal contract", "FD capability", "thread storage IPC", "IPC endpoint", "IPC direct" },
    });
    const ipc_endpoint_tests = b.addTest(.{
        .root_module = ranked_kernel_mod,
        .test_runner = .{ .path = b.path("src/lock_diagnostics_test_runner.zig"), .mode = .server },
        .use_llvm = true,
        .filters = &.{ "lock ordering actual caller roots", "IPC endpoint", "thread storage IPC stable block" },
    });
    ipc_endpoint_tests.stack_size = 512 * 1024 * 1024;
    b.step("test-ipc-endpoint", "Run checked IPC endpoint races").dependOn(&b.addRunArtifact(ipc_endpoint_tests).step);
    ranked_kernel_tests.stack_size = 512 * 1024 * 1024;
    const ranked_kernel_run = b.addRunArtifact(ranked_kernel_tests);
    test_step.dependOn(&ranked_kernel_run.step);
    lock_test_step.dependOn(&ranked_kernel_run.step);
    b.step("test-lock-kernel", "Run lock ordering actual kernel lock callers").dependOn(&ranked_kernel_run.step);
    const lock_boundary_check = b.addSystemCommand(&.{"python3"});
    lock_boundary_check.addFileArg(b.path("../tests/check_kernel_lock_boundaries.py"));
    test_step.dependOn(&lock_boundary_check.step);
    lock_test_step.dependOn(&lock_boundary_check.step);
    const cpu_codegen_tests = b.addSystemCommand(&.{ "python3", "-m", "unittest", "discover", "-s" });
    cpu_codegen_tests.addDirectoryArg(b.path("../tests"));
    cpu_codegen_tests.addArgs(&.{ "-p", "test_kernel_cpu_identity_codegen.py" });
    test_step.dependOn(&cpu_codegen_tests.step);
    const frame_codegen_tests = b.addSystemCommand(&.{ "python3", "-m", "unittest", "discover", "-s" });
    frame_codegen_tests.addDirectoryArg(b.path("../tests"));
    frame_codegen_tests.addArgs(&.{ "-p", "test_kernel_stack_frames.py" });
    test_step.dependOn(&frame_codegen_tests.step);

    const futex_runtime_test_mod = b.createModule(.{
        .root_source_file = b.path("src/futex_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    futex_runtime_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    addVerifiedSchedulerHostObject(b, futex_runtime_test_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_futex_test.o");
    // Collector retirement and scheduler publication share a transaction;
    // testing only scheduler roots misses the runtime's lock/callback edge.
    const futex_runtime_tests = b.addTest(.{
        .root_module = futex_runtime_test_mod,
        .use_llvm = true,
        .filters = &.{"futex"},
    });
    const futex_runtime_run = b.addRunArtifact(futex_runtime_tests);
    test_step.dependOn(&futex_runtime_run.step);
    b.step("test-pager-futex", "Test fault-free futex comparison under lock").dependOn(&futex_runtime_run.step);

    const smp_test_mod = b.createModule(.{
        .root_source_file = b.path("src/smp.zig"),
        .target = target,
        .optimize = optimize,
    });
    smp_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const smp_tests = b.addTest(.{ .root_module = smp_test_mod, .filters = &.{ "broadcast", "MMIO alias", "MMIO WC snapshot", "WC PAT setup", "FS/GS bases", "SYSRET" } });
    test_step.dependOn(&b.addRunArtifact(smp_tests).step);

    const stack_paint_options = b.addOptions();
    stack_paint_options.addOption(bool, "stack_high_water_enabled", b.option(bool, "stack-high-water", "Paint unused CPU stacks at boot for host-side high-water diagnostics") orelse false);
    const kernel_debug = b.option(bool, "kernel-debug", "Keep kernel debug information and symbols in the boot ELF") orelse false;
    const kernel_lock_check_options = b.addOptions();
    kernel_lock_check_options.addOption(bool, "user_return_checks_enabled", kernel_debug);
    kernel_lock_check_options.addOption(bool, "reference_checks_enabled", b.option(bool, "kernel-reference-checks", "Compare maintained references with old table scans (migration only)") orelse false);
    const lock_checks = b.option(bool, "kernel-lock-checks", "Diagnose audited lock order and IRQ ownership (not production profiling)") orelse false;
    kernel_lock_check_options.addOption(bool, "lock_checks_enabled", lock_checks);
    const log_ring_options = b.addOptions();
    log_ring_options.addOption(bool, "log_ring_enabled", b.option(bool, "kernel-log-ring", "Expose a bounded kernel-only log to user space for debugging") orelse false);
    const smp_profile_options = b.addOptions();
    const smp_profile_enabled = b.option(bool, "smp-profile", "Enable per-CPU SMP diagnostic counters") orelse false;
    smp_profile_options.addOption(bool, "enabled", smp_profile_enabled);
    const ipc_profile_options = b.addOptions();
    ipc_profile_options.addOption(bool, "b3_enabled", b.option(bool, "b3-profile", "Single CPU bounded syscall path experiment; no production ABI") orelse false);
    ipc_profile_options.addOption(bool, "ipc_enabled", b.option(bool, "ipc-profile", "Enable per-CPU native IPC diagnostic counters") orelse false);
    const wake_trace_enabled = b.option(bool, "wake-trace", "Enable bounded per-CPU wake timelines for stopped-QEMU diagnosis") orelse false;
    ipc_profile_options.addOption(bool, "wake_trace_enabled", wake_trace_enabled);
    const normal_checkpoint_options = b.addOptions();
    normal_checkpoint_options.addOption([]const u8, "name", "off");
    const limine_mod = b.createModule(.{
        .root_source_file = b.path("../bootloader/limine/kernel_entry.zig"),
        .target = limine_target,
        .optimize = .ReleaseSmall,
        .code_model = .kernel,
        // Ring-0 IRQs push onto the interrupted stack, including its red zone.
        .red_zone = false,
        // Profile snapshots resolve exact ELF symbols. Keep this host artifact;
        // strip its boot copy and verify identical PT_LOAD bytes before boot.
        .strip = !kernel_debug and !smp_profile_enabled and !wake_trace_enabled,
    });
    const kernel_boot_api_mod = b.createModule(.{
        .root_source_file = b.path("src/bootloader_api.zig"),
        .target = limine_target,
        .optimize = .ReleaseSmall,
        .code_model = .kernel,
        .red_zone = false,
    });
    const diag_font_files = b.addWriteFiles();
    const diag_font_source = diag_font_files.add("font.zig", "pub const bytes: []const u8 = @embedFile(\"font.psf\");\n");
    _ = diag_font_files.addCopyFile(b.path("../.artifacts/third_party/libvterm-0.3.3/build/font.psf"), "font.psf");
    kernel_boot_api_mod.addImport("boot_diag_font", b.createModule(.{ .root_source_file = diag_font_source }));
    kernel_boot_api_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const kernel_lock_options_mod = kernel_lock_check_options.createModule();
    kernel_boot_api_mod.addImport("kernel_lock_check_options", kernel_lock_options_mod);
    limine_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    limine_mod.addOptions("smp_profile_options", smp_profile_options);
    limine_mod.addOptions("ipc_profile_options", ipc_profile_options);
    limine_mod.addOptions("kernel_log_ring_options", log_ring_options);
    limine_mod.addImport("kernel_lock_check_options", kernel_lock_options_mod);
    limine_mod.addOptions("stack_paint_options", stack_paint_options);
    limine_mod.addOptions("boot_checkpoint_options", normal_checkpoint_options);
    limine_mod.addImport("kernel_boot_api", kernel_boot_api_mod);
    addVerifiedSchedulerElfObject(b, limine_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_limine.o", lock_checks);
    const limine_kernel = b.addExecutable(.{
        .name = "pacha-kernel",
        .root_module = limine_mod,
    });
    limine_kernel.entry = .{ .symbol_name = "_start" };
    limine_kernel.setLinkerScript(b.path("../bootloader/limine/kernel.ld"));
    const install_limine = b.addInstallArtifact(limine_kernel, .{
        .dest_sub_path = "limine/pacha-kernel.elf",
    });
    const limine_step = b.step("limine", "Build Limine kernel ELF");
    limine_step.dependOn(&install_limine.step);
    const cpu_codegen_check = b.addSystemCommand(&.{"python3"});
    cpu_codegen_check.addFileArg(b.path("../tests/check_kernel_cpu_identity_codegen.py"));
    cpu_codegen_check.addFileArg(limine_kernel.getEmittedBin());
    b.step("check-cpu-identity-codegen", "Audit live-XState CPU lookup (requires -Dkernel-debug=true)").dependOn(&cpu_codegen_check.step);
    // Stripped ELFs have no internal lookup symbols. Every symbol-bearing
    // production build checks its actual machine code before installation;
    // use both lock-check options when validating this boundary.
    if (kernel_debug) install_limine.step.dependOn(&cpu_codegen_check.step);
    const scalar_codegen_check = b.addSystemCommand(&.{"python3"});
    scalar_codegen_check.addFileArg(b.path("../tests/check_kernel_scalar_codegen.py"));
    scalar_codegen_check.addFileInput(b.path("../tests/check_kernel_cpu_identity_codegen.py"));
    scalar_codegen_check.addFileArg(limine_kernel.getEmittedBin());
    if (kernel_debug) install_limine.step.dependOn(&scalar_codegen_check.step);
    const xstate_transition_test = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/ipc_xstate_transition.zig"),
        .target = target,
        .optimize = optimize,
    }) });
    test_step.dependOn(&b.addRunArtifact(xstate_transition_test).step);
    const scalar_tests = b.addSystemCommand(&.{ "python3", "-m", "unittest", "discover", "-s", "../tests", "-p", "test_kernel_scalar_codegen.py" });
    test_step.dependOn(&scalar_tests.step);
    const frame_codegen_check = b.addSystemCommand(&.{"python3"});
    frame_codegen_check.addFileArg(b.path("../tests/check_kernel_stack_frames.py"));
    frame_codegen_check.addFileInput(b.path("../tests/check_kernel_cpu_identity_codegen.py"));
    frame_codegen_check.addFileInput(b.path("../tests/kernel_stack_switch_contracts.json"));
    frame_codegen_check.addFileArg(limine_kernel.getEmittedBin());
    b.step("check-stack-frames", "Audit per-function stack bounds (requires -Dkernel-debug=true)").dependOn(&frame_codegen_check.step);
    // The real symbols ELF is the contract: inlining and late stack changes
    // are invisible to source array-size checks. Stripped builds lack names.
    if (kernel_debug) install_limine.step.dependOn(&frame_codegen_check.step);

    // A separate, opt-in copy of the production boot path records the first
    // post-CR3 checkpoint and the terminal kernel log on GOP. It never
    // replaces the ordinary live kernel or changes its userland ABI.
    const diag_options = b.addOptions();
    diag_options.addOption([]const u8, "name", "DIAG");
    const diag_mod = b.createModule(.{
        .root_source_file = b.path("../bootloader/limine/kernel_entry.zig"),
        .target = limine_target,
        .optimize = .ReleaseSmall,
        .code_model = .kernel,
        .red_zone = false,
        .strip = !kernel_debug,
    });
    diag_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    diag_mod.addOptions("smp_profile_options", smp_profile_options);
    diag_mod.addOptions("ipc_profile_options", ipc_profile_options);
    diag_mod.addOptions("kernel_log_ring_options", log_ring_options);
    diag_mod.addOptions("kernel_lock_check_options", kernel_lock_check_options);
    diag_mod.addOptions("stack_paint_options", stack_paint_options);
    diag_mod.addOptions("boot_checkpoint_options", diag_options);
    diag_mod.addImport("kernel_boot_api", kernel_boot_api_mod);
    addVerifiedSchedulerElfObject(b, diag_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_diag.o", lock_checks);
    const diag_exe = b.addExecutable(.{ .name = "pacha-boot-diag", .root_module = diag_mod });
    diag_exe.entry = .{ .symbol_name = "_start" };
    diag_exe.setLinkerScript(b.path("../bootloader/limine/kernel.ld"));
    const diag_step = b.step("boot-diag", "Build isolated post-CR3 GOP diagnostic ELF");
    diag_step.dependOn(&b.addInstallArtifact(diag_exe, .{
        .dest_sub_path = "limine/DIAGBOOT.ELF",
    }).step);

    // QEMU-only display regression: halt after E so the GOP text and the
    // centralized halt-log renderer remain visible for screenshot inspection.
    const diag_test_options = b.addOptions();
    diag_test_options.addOption([]const u8, "name", "DIAGTEST");
    const diag_test_mod = b.createModule(.{
        .root_source_file = b.path("../bootloader/limine/kernel_entry.zig"),
        .target = limine_target,
        .optimize = .ReleaseSmall,
        .code_model = .kernel,
        .red_zone = false,
        .strip = true,
    });
    diag_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    diag_test_mod.addOptions("smp_profile_options", smp_profile_options);
    diag_test_mod.addOptions("ipc_profile_options", ipc_profile_options);
    diag_test_mod.addOptions("kernel_log_ring_options", log_ring_options);
    diag_test_mod.addOptions("kernel_lock_check_options", kernel_lock_check_options);
    diag_test_mod.addOptions("stack_paint_options", stack_paint_options);
    diag_test_mod.addOptions("boot_checkpoint_options", diag_test_options);
    diag_test_mod.addImport("kernel_boot_api", kernel_boot_api_mod);
    addVerifiedSchedulerElfObject(b, diag_test_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_diag_test.o", lock_checks);
    const diag_test_exe = b.addExecutable(.{ .name = "pacha-boot-diag-test", .root_module = diag_test_mod });
    diag_test_exe.entry = .{ .symbol_name = "_start" };
    diag_test_exe.setLinkerScript(b.path("../bootloader/limine/kernel.ld"));
    const diag_test_step = b.step("boot-diag-test", "Build QEMU-only GOP diagnostic halt regression ELF");
    diag_test_step.dependOn(&b.addInstallArtifact(diag_test_exe, .{
        .dest_sub_path = "limine/DIAGTEST.ELF",
    }).step);

    // These images execute the real kernel boot path up to one selected
    // checkpoint, then use the reset signal proven on the B550. The ordinary
    // kernel above always compiles the checkpoint branch out.
    const checkpoint_step = b.step("entry-checkpoints", "Build production-path Limine reset checkpoints");
    for ([_][]const u8{ "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "N" }) |checkpoint| {
        const checkpoint_options = b.addOptions();
        checkpoint_options.addOption([]const u8, "name", checkpoint);
        const checkpoint_mod = b.createModule(.{
            .root_source_file = b.path("../bootloader/limine/kernel_entry.zig"),
            .target = limine_target,
            .optimize = .ReleaseSmall,
            .code_model = .kernel,
            .red_zone = false,
            .strip = true,
        });
        checkpoint_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
        checkpoint_mod.addOptions("smp_profile_options", smp_profile_options);
        checkpoint_mod.addOptions("ipc_profile_options", ipc_profile_options);
        checkpoint_mod.addOptions("kernel_log_ring_options", log_ring_options);
        checkpoint_mod.addOptions("kernel_lock_check_options", kernel_lock_check_options);
        checkpoint_mod.addOptions("stack_paint_options", stack_paint_options);
        checkpoint_mod.addOptions("boot_checkpoint_options", checkpoint_options);
        checkpoint_mod.addImport("kernel_boot_api", kernel_boot_api_mod);
        addVerifiedSchedulerElfObject(b, checkpoint_mod, "../verified/scheduling/src/pacha_eevdf.c", b.fmt("pacha_eevdf_checkpoint_{s}.o", .{checkpoint}), lock_checks);
        const checkpoint_exe = b.addExecutable(.{
            .name = b.fmt("pacha-checkpoint-{s}", .{checkpoint}),
            .root_module = checkpoint_mod,
        });
        checkpoint_exe.entry = .{ .symbol_name = "_start" };
        checkpoint_exe.setLinkerScript(b.path("../bootloader/limine/kernel.ld"));
        checkpoint_step.dependOn(&b.addInstallArtifact(checkpoint_exe, .{
            .dest_sub_path = b.fmt("limine/CHECK{s}.ELF", .{checkpoint}),
        }).step);
    }

    // Real-machine entry probes are separate Limine executables. They never
    // enter the production kernel or alter its boot path and are not built by
    // the default/kernel steps.
    const entry_probe_step = b.step("entry-probes", "Build standalone real-machine Limine entry probes");
    for ([_]struct { name: []const u8, source: []const u8, output: []const u8 }{
        .{ .name = "pacha-entry-gop", .source = "../bootloader/limine/entry_probe_gop.zig", .output = "limine/ENTRYGOP.ELF" },
        .{ .name = "pacha-entry-reset", .source = "../bootloader/limine/entry_probe_reset.zig", .output = "limine/ENTRYRST.ELF" },
    }) |probe| {
        const probe_mod = b.createModule(.{
            .root_source_file = b.path(probe.source),
            .target = limine_target,
            .optimize = .ReleaseSmall,
            .code_model = .kernel,
            .red_zone = false,
            .strip = true,
        });
        const probe_exe = b.addExecutable(.{ .name = probe.name, .root_module = probe_mod });
        probe_exe.entry = .{ .symbol_name = "_start" };
        probe_exe.setLinkerScript(b.path("../bootloader/limine/kernel.ld"));
        entry_probe_step.dependOn(&b.addInstallArtifact(probe_exe, .{
            .dest_sub_path = probe.output,
        }).step);
    }

    // Generate a tiny module alongside its PSF file so @embedFile is a
    // declared build input, not an implicit dependency on a hard-coded path.
    const ivrs_font_files = b.addWriteFiles();
    const ivrs_font_source = ivrs_font_files.add("font.zig", "pub const bytes: []const u8 = @embedFile(\"font.psf\");\n");
    _ = ivrs_font_files.addCopyFile(b.path("../.artifacts/third_party/libvterm-0.3.3/build/font.psf"), "font.psf");
    const ivrs_font_mod = b.createModule(.{ .root_source_file = ivrs_font_source });
    const ivrs_probe_mod = b.createModule(.{
        .root_source_file = b.path("../bootloader/limine/entry_probe_ivrs.zig"),
        .target = limine_target,
        .optimize = .ReleaseSmall,
        .code_model = .kernel,
        .red_zone = false,
        .strip = true,
    });
    ivrs_probe_mod.addImport("ivrs_probe_font", ivrs_font_mod);
    ivrs_probe_mod.addImport("acpi_ivrs", b.createModule(.{
        .root_source_file = b.path("src/acpi_ivrs.zig"),
        .target = limine_target,
        .optimize = .ReleaseSmall,
    }));
    const ivrs_probe_exe = b.addExecutable(.{ .name = "pacha-entry-ivrs", .root_module = ivrs_probe_mod });
    ivrs_probe_exe.entry = .{ .symbol_name = "_start" };
    ivrs_probe_exe.setLinkerScript(b.path("../bootloader/limine/kernel.ld"));
    entry_probe_step.dependOn(&b.addInstallArtifact(ivrs_probe_exe, .{
        .dest_sub_path = "limine/ENTRYIVRS.ELF",
    }).step);
    const ivrs_probe_test_mod = b.createModule(.{
        .root_source_file = b.path("../bootloader/limine/entry_probe_ivrs.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    });
    ivrs_probe_test_mod.addImport("ivrs_probe_font", ivrs_font_mod);
    ivrs_probe_test_mod.addImport("acpi_ivrs", b.createModule(.{
        .root_source_file = b.path("src/acpi_ivrs.zig"),
        .target = b.graph.host,
        .optimize = .Debug,
    }));
    b.step("entry-probes-test", "Test standalone IVRS probe's bounds and PCI policy")
        .dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = ivrs_probe_test_mod })).step);

    const default_step = b.step("kernel", "Build bootable kernel ELF");
    default_step.dependOn(&install_limine.step);
    b.default_step.dependOn(&install_limine.step);
}
