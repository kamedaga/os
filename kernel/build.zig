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

    const limine_mod = b.createModule(.{
        .root_source_file = b.path("../bootloader/limine/kernel_entry.zig"),
        .target = limine_target,
        .optimize = .ReleaseSmall,
        .code_model = .kernel,
        .strip = false,
    });
    const kernel_boot_api_mod = b.createModule(.{
        .root_source_file = b.path("src/bootloader_api.zig"),
        .target = limine_target,
        .optimize = .ReleaseSmall,
        .code_model = .kernel,
    });
    kernel_boot_api_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    limine_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
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
