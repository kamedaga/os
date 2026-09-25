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

fn freestandingClang(b: *std.Build) []const u8 {
    return b.graph.environ_map.get("CAPOS_UNWRAPPED_CLANG") orelse
        (b.graph.environ_map.get("CAPOS_FREESTANDING_CC") orelse "clang");
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const limine_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .freestanding,
        .abi = .none,
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
        .root_module = test_mod,
    });
    unit_tests.stack_size = 512 * 1024 * 1024;

    const run_unit_tests = b.addRunArtifact(unit_tests);
    const test_step = b.step("test", "Run kernel unit tests");
    test_step.dependOn(&run_unit_tests.step);
    const clock_copy_mod = b.createModule(.{
        .root_source_file = b.path("src/runtime_clock_copy_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    clock_copy_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const clock_copy_tests = b.addTest(.{ .root_module = clock_copy_mod, .filters = &.{ "timespec copyout", "yield result" } });
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
    const mmio_overlay_tests = b.addTest(.{ .root_module = mmio_overlay_mod, .filters = &.{"MMIO overlay"} });
    mmio_overlay_tests.stack_size = 64 * 1024 * 1024;
    test_step.dependOn(&b.addRunArtifact(mmio_overlay_tests).step);
    const interrupt_waiter_mod = b.createModule(.{
        .root_source_file = b.path("src/syscalls.zig"),
        .target = target,
        .optimize = optimize,
    });
    interrupt_waiter_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    interrupt_waiter_mod.addCSourceFile(.{ .file = b.path("../tests/interrupt_waiter_host.c"), .flags = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror" } });
    addVerifiedSchedulerHostObject(b, interrupt_waiter_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_interrupt_test.o");
    const interrupt_waiter_tests = b.addTest(.{ .root_module = interrupt_waiter_mod, .filters = &.{ "shootdown interrupt guard", "copy window", "poll item snapshot" }, .use_llvm = true });
    interrupt_waiter_tests.stack_size = 512 * 1024 * 1024;
    test_step.dependOn(&b.addRunArtifact(interrupt_waiter_tests).step);
    const realtime_clock_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/realtime_clock.zig"),
        .target = target,
        .optimize = optimize,
    }) });
    test_step.dependOn(&b.addRunArtifact(realtime_clock_tests).step);
    for ([_][]const u8{ "src/hpet.zig", "src/lapic.zig", "src/clockevent.zig", "src/acpi_tables.zig", "src/acpi_ivrs.zig", "src/pci.zig" }) |source| {
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
    const vtd_tests = b.addTest(.{ .root_module = vtd_test_mod, .filters = &.{"VT-d"} });
    vtd_tests.stack_size = 512 * 1024 * 1024;
    test_step.dependOn(&b.addRunArtifact(vtd_tests).step);
    const amd_iommu_test_mod = b.createModule(.{
        .root_source_file = b.path("src/amd_iommu.zig"),
        .target = target,
        .optimize = optimize,
    });
    amd_iommu_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
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
        .root_source_file = b.path("src/arch/x86_64/physical_layout.zig"),
        .target = target,
        .optimize = optimize,
    }) });
    test_step.dependOn(&b.addRunArtifact(physical_layout_tests).step);
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
        .filters = &.{ "migration waits", "preferred wake", "controlled thread context", "yield handoff" },
    });
    test_step.dependOn(&b.addRunArtifact(scheduler_context_tests).step);

    const smp_test_mod = b.createModule(.{
        .root_source_file = b.path("src/smp.zig"),
        .target = target,
        .optimize = optimize,
    });
    smp_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const smp_tests = b.addTest(.{ .root_module = smp_test_mod, .filters = &.{ "broadcast", "MMIO alias" } });
    test_step.dependOn(&b.addRunArtifact(smp_tests).step);

    const kernel_debug = b.option(bool, "kernel-debug", "Keep kernel debug information and symbols in the boot ELF") orelse false;
    const smp_profile_options = b.addOptions();
    smp_profile_options.addOption(bool, "enabled", b.option(bool, "smp-profile", "Enable per-CPU SMP diagnostic counters") orelse false);
    const ipc_profile_options = b.addOptions();
    ipc_profile_options.addOption(bool, "ipc_enabled", b.option(bool, "ipc-profile", "Enable per-CPU native IPC diagnostic counters") orelse false);
    const normal_checkpoint_options = b.addOptions();
    normal_checkpoint_options.addOption([]const u8, "name", "off");
    const limine_mod = b.createModule(.{
        .root_source_file = b.path("../bootloader/limine/kernel_entry.zig"),
        .target = limine_target,
        .optimize = .ReleaseSmall,
        .code_model = .kernel,
        // Ring-0 IRQs push onto the interrupted stack, including its red zone.
        .red_zone = false,
        .strip = !kernel_debug,
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
    limine_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    limine_mod.addOptions("smp_profile_options", smp_profile_options);
    limine_mod.addOptions("ipc_profile_options", ipc_profile_options);
    limine_mod.addOptions("boot_checkpoint_options", normal_checkpoint_options);
    limine_mod.addImport("kernel_boot_api", kernel_boot_api_mod);
    addVerifiedSchedulerElfObject(b, limine_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_limine.o");
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
        .strip = true,
    });
    diag_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    diag_mod.addOptions("smp_profile_options", smp_profile_options);
    diag_mod.addOptions("ipc_profile_options", ipc_profile_options);
    diag_mod.addOptions("boot_checkpoint_options", diag_options);
    diag_mod.addImport("kernel_boot_api", kernel_boot_api_mod);
    addVerifiedSchedulerElfObject(b, diag_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_diag.o");
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
    diag_test_mod.addOptions("boot_checkpoint_options", diag_test_options);
    diag_test_mod.addImport("kernel_boot_api", kernel_boot_api_mod);
    addVerifiedSchedulerElfObject(b, diag_test_mod, "../verified/scheduling/src/pacha_eevdf.c", "pacha_eevdf_diag_test.o");
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
        checkpoint_mod.addOptions("boot_checkpoint_options", checkpoint_options);
        checkpoint_mod.addImport("kernel_boot_api", kernel_boot_api_mod);
        addVerifiedSchedulerElfObject(b, checkpoint_mod,
            "../verified/scheduling/src/pacha_eevdf.c",
            b.fmt("pacha_eevdf_checkpoint_{s}.o", .{checkpoint}));
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
