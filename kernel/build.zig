const std = @import("std");

fn pruneZeroSizedBootx64Artifacts(io: std.Io) bool {
    const cwd = std.Io.Dir.cwd();
    var repaired = false;
    const installed_bootx64 = "zig-out/bin/EFI/BOOT/BOOTX64.EFI";
    if (cwd.statFile(io, installed_bootx64, .{})) |stat| {
        if (stat.size == 0) {
            cwd.deleteFile(io, installed_bootx64) catch |err| {
                std.debug.print("build.zig: failed to remove zero-byte zig-out BOOTX64.EFI: {s}\n", .{@errorName(err)});
            };
            std.debug.print("build.zig: removed zero-byte zig-out BOOTX64.EFI\n", .{});
            repaired = true;
        }
    } else |_| {}

    var cache_dir = cwd.openDir(io, ".zig-cache/o", .{ .iterate = true }) catch return repaired;
    defer cache_dir.close(io);

    var walker = cache_dir.walk(std.heap.page_allocator) catch return repaired;
    defer walker.deinit();

    var removed_count: usize = 0;
    while (walker.next(io) catch null) |entry| {
        if (entry.kind != .file) continue;
        if (!std.ascii.eqlIgnoreCase(std.fs.path.basename(entry.path), "BOOTX64.efi")) continue;
        const stat = cache_dir.statFile(io, entry.path, .{}) catch continue;
        if (stat.size != 0) continue;
        cache_dir.deleteFile(io, entry.path) catch continue;
        removed_count += 1;
        repaired = true;
    }
    if (removed_count != 0) {
        std.debug.print("build.zig: removed {d} zero-byte BOOTX64 cache artifact(s)\n", .{removed_count});
    }
    return repaired;
}

pub fn build(b: *std.Build) void {
    _ = pruneZeroSizedBootx64Artifacts(b.graph.io);

    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const efi_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .uefi,
        .abi = .msvc,
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

    const efi_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = efi_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .strip = true,
    });
    efi_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const efi_app = b.addExecutable(.{
        .name = "BOOTX64",
        .root_module = efi_mod,
    });

    const install_efi = b.addInstallArtifact(efi_app, .{
        .dest_sub_path = "EFI/BOOT/BOOTX64.EFI",
    });
    // `pactl` owns userland, bootfs/rootfs packaging, and host tools.
    // Keep `zig build efi` focused on kernel-side boot artifacts only.
    const efi_step = b.step("efi", "Build UEFI kernel application");
    efi_step.dependOn(&install_efi.step);
}
