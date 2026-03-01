const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const test_mod = b.createModule(.{
        .root_source_file = b.path("src/kernel.zig"),
        .target = target,
        .optimize = optimize,
    });
    const unit_tests = b.addTest(.{
        .root_module = test_mod,
    });

    const run_unit_tests = b.addRunArtifact(unit_tests);
    const test_step = b.step("test", "Run kernel unit tests");
    test_step.dependOn(&run_unit_tests.step);

    const efi_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .uefi,
        .abi = .msvc,
    });

    const efi_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = efi_target,
        .optimize = optimize,
        .code_model = .small,
    });
    const efi_app = b.addExecutable(.{
        .name = "BOOTX64",
        .root_module = efi_mod,
    });

    const install_efi = b.addInstallArtifact(efi_app, .{
        .dest_sub_path = "EFI/BOOT/BOOTX64.EFI",
    });
    const efi_step = b.step("efi", "Build UEFI kernel application");
    efi_step.dependOn(&install_efi.step);
}
