const std = @import("std");

fn pruneZeroSizedBootx64Artifacts() bool {
    const cwd = std.fs.cwd();
    var repaired = false;
    const installed_bootx64 = "zig-out/bin/EFI/BOOT/BOOTX64.EFI";
    if (cwd.statFile(installed_bootx64)) |stat| {
        if (stat.size == 0) {
            cwd.deleteFile(installed_bootx64) catch |err| {
                std.debug.print("build.zig: failed to remove zero-byte zig-out BOOTX64.EFI: {s}\n", .{@errorName(err)});
            };
            std.debug.print("build.zig: removed zero-byte zig-out BOOTX64.EFI\n", .{});
            repaired = true;
        }
    } else |_| {}

    var cache_dir = cwd.openDir(".zig-cache/o", .{ .iterate = true }) catch return repaired;
    defer cache_dir.close();

    var walker = cache_dir.walk(std.heap.page_allocator) catch return repaired;
    defer walker.deinit();

    var removed_count: usize = 0;
    while (walker.next() catch null) |entry| {
        if (entry.kind != .file) continue;
        if (!std.ascii.eqlIgnoreCase(std.fs.path.basename(entry.path), "BOOTX64.efi")) continue;
        const stat = cache_dir.statFile(entry.path) catch continue;
        if (stat.size != 0) continue;
        cache_dir.deleteFile(entry.path) catch continue;
        removed_count += 1;
        repaired = true;
    }
    if (removed_count != 0) {
        std.debug.print("build.zig: removed {d} zero-byte BOOTX64 cache artifact(s)\n", .{removed_count});
    }
    return repaired;
}

pub fn build(b: *std.Build) void {
    const bootx64_cache_repaired = pruneZeroSizedBootx64Artifacts();

    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const scheduler_ap_queue_experiment = b.option(
        bool,
        "scheduler-ap-queue-experiment",
        "Allow non-BSP scheduler run queues to accept runnable threads for scheduler experiments",
    ) orelse false;
    const build_workarounds = b.addOptions();
    build_workarounds.addOption(bool, "bootx64_cache_repaired", bootx64_cache_repaired);
    build_workarounds.addOption(bool, "scheduler_ap_queue_experiment", scheduler_ap_queue_experiment);

    const test_mod = b.createModule(.{
        .root_source_file = b.path("src/kernel.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_mod.addOptions("build_workarounds", build_workarounds);
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
    const kernel_abi_root_mod = b.createModule(.{
        .root_source_file = b.path("abi/kernel_abi_root.zig"),
    });
    const persistent_fs_layout_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/abi/persistent_fs_layout.zig"),
    });
    test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);

    const efi_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = efi_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
    });
    efi_mod.addOptions("build_workarounds", build_workarounds);
    efi_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const efi_app = b.addExecutable(.{
        .name = "BOOTX64",
        .root_module = efi_mod,
    });

    const init_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .freestanding,
        .abi = .none,
    });
    const abi_root_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/abi/abi_root.zig"),
        .target = init_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    abi_root_mod.addImport("persistent_fs_layout", persistent_fs_layout_mod);
    const init_app_mod = b.createModule(.{
        .root_source_file = b.path("../bootstrap/programs/init_app.zig"),
        .target = init_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    init_app_mod.addImport("abi_root", abi_root_mod);
    init_app_mod.strip = true;
    const init_app = b.addExecutable(.{
        .name = "INITAPP",
        .root_module = init_app_mod,
    });
    init_app.pie = true;
    init_app.entry = .{ .symbol_name = "_start" };
    init_app.link_z_common_page_size = 0x10;
    init_app.link_z_max_page_size = 0x10;
    const install_init = b.addInstallArtifact(init_app, .{
        .dest_sub_path = "EFI/BOOT/INITAPP.ELF",
    });
    const init_step = b.step("init-elf", "Build init PIE ELF");
    init_step.dependOn(&install_init.step);

    const install_efi = b.addInstallArtifact(efi_app, .{
        .dest_sub_path = "EFI/BOOT/BOOTX64.EFI",
    });
    // `pactl` owns userland, bootfs/rootfs packaging, and host tools.
    // Keep `zig build efi` focused on kernel-side boot artifacts only.
    install_efi.step.dependOn(&install_init.step);
    const efi_step = b.step("efi", "Build UEFI kernel application");
    efi_step.dependOn(&install_efi.step);
}






