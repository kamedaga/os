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
    for ([_][]const u8{ "src/hpet.zig", "src/clockevent.zig", "src/acpi_tables.zig", "src/pci.zig" }) |source| {
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
        .filters = &.{ "migration waits", "preferred wake", "controlled thread context" },
    });
    test_step.dependOn(&b.addRunArtifact(scheduler_context_tests).step);

    const smp_test_mod = b.createModule(.{
        .root_source_file = b.path("src/smp.zig"),
        .target = target,
        .optimize = optimize,
    });
    smp_test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const smp_tests = b.addTest(.{ .root_module = smp_test_mod, .filters = &.{"broadcast"} });
    test_step.dependOn(&b.addRunArtifact(smp_tests).step);

    const kernel_debug = b.option(bool, "kernel-debug", "Keep kernel debug information and symbols in the boot ELF") orelse false;
    const smp_profile_options = b.addOptions();
    smp_profile_options.addOption(bool, "enabled", b.option(bool, "smp-profile", "Enable per-CPU SMP diagnostic counters") orelse false);
    const ipc_profile_options = b.addOptions();
    ipc_profile_options.addOption(bool, "ipc_enabled", b.option(bool, "ipc-profile", "Enable per-CPU native IPC diagnostic counters") orelse false);
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
    kernel_boot_api_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    limine_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    limine_mod.addOptions("smp_profile_options", smp_profile_options);
    limine_mod.addOptions("ipc_profile_options", ipc_profile_options);
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

    const default_step = b.step("kernel", "Build bootable kernel ELF");
    default_step.dependOn(&install_limine.step);
    b.default_step.dependOn(&install_limine.step);
}
